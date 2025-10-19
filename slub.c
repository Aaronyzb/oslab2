// kern/mm/slub.c
#include <slub.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

//把x向上对齐到a的整数倍
static inline size_t round_up(size_t x, size_t a) { return (x + a - 1) & ~(a - 1); }

static inline void cache_list_init(struct kmem_cache *c) {
    list_init(&c->partial); list_init(&c->full); list_init(&c->empty);
}


//算出kmem_cache中的每个slab能放多少个对象
static void compute_layout(struct kmem_cache *c) {
    if (c->align < sizeof(void*)) c->align = sizeof(void*);
    if (c->obj_size < sizeof(void*)) c->obj_size = sizeof(void*);
    size_t slab_bytes = ((size_t)1 << c->order) * PGSIZE;
    size_t off = round_up(sizeof(struct slab), c->align);
    size_t payload = slab_bytes - off;//这里算出真正的对象空间大小
    c->objs_per_slab = (uint16_t)(payload / c->obj_size);
    assert(c->objs_per_slab > 0);
}


//根据 kmem_cache 和起始页，构造 slab 结构并初始化 freelist，注意我们是把下一个对象的地址存放在当前对象的首字节里
static struct slab *slab_build(struct kmem_cache *c, struct Page *pages) {
    uintptr_t pa = page2pa(pages);
    void *kva = (void *)(pa + va_pa_offset);

    struct slab *s = (struct slab *)kva;
    s->cache = c; s->order = c->order; s->pages = pages;
    s->inuse = 0; s->total = c->objs_per_slab;

    uintptr_t obj_start = (uintptr_t)kva + round_up(sizeof(struct slab), c->align);
    void **prev = NULL;
    for (uint16_t i = 0; i < s->total; i++) {
        void *obj = (void *)(obj_start + (size_t)i * c->obj_size);
        if (c->ctor) c->ctor(obj);
        if (prev) *prev = obj;
        prev = (void **)obj;
    }
    if (prev) *prev = NULL;
    s->freelist = (void *)obj_start;
    return s;
}


//创建一个kmem_cache结构体并初始化
struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align, uint16_t order, ctor_t ctor) {
    // 用一页放 kmem_cache 结构体
    struct Page *mgmt = alloc_pages(1); assert(mgmt);
    void *mem = (void *)(page2pa(mgmt) + va_pa_offset);
    memset(mem, 0, PGSIZE);

    struct kmem_cache *c = (struct kmem_cache *)mem;
    c->name=name; c->obj_size=size; c->align=align?align:sizeof(void*); c->order=order; c->ctor=ctor;
    cache_list_init(c); compute_layout(c);
    return c;
}


//如果没有可用的slab，就分配新页并创建slab
static struct slab *cache_grow(struct kmem_cache *c) {
    struct Page *p = alloc_pages(1u << c->order);
    if (!p) return NULL;
    struct slab *s = slab_build(c, p);
    list_add(&c->partial, &s->link);
    return s;
}


//分配一个对象
void *kmem_cache_alloc(struct kmem_cache *c) {
    list_entry_t *le = list_next(&c->partial);
    struct slab *s;
    if (le == &c->partial) {
        if (list_empty(&c->empty)) {
            s = cache_grow(c);
            if (!s) return NULL;
        } else {
            le = list_next(&c->empty);
            s = to_struct(le, struct slab, link);//还原到slab结构体指针
            list_del(le); list_add(&c->partial, le);
        }
    } else {
        s = to_struct(le, struct slab, link);
    }
    assert(s->freelist);
    void *obj = s->freelist;//就从链表头的第一个对象给他摘出来使用
    s->freelist = *(void **)obj;
    s->inuse++; c->inuse_objs++;
    if (s->inuse == s->total) { list_del(&s->link); list_add(&c->full, &s->link); }
    return obj;
}


//释放一个对象
void kmem_cache_free(struct kmem_cache *c, void *obj) {
    size_t slab_bytes = ((size_t)1 << c->order) * PGSIZE;
    uintptr_t kva = (uintptr_t)obj;
    uintptr_t base = kva & ~(slab_bytes - 1);
    struct slab *s = (struct slab *)base;
    assert(s->cache == c);

    *(void **)obj = s->freelist; s->freelist = obj;
    assert(s->inuse > 0); s->inuse--; c->inuse_objs--;

    // 若在 full，搬回 partial（线性查找一次，列表不大）
    if (s->inuse == s->total - 1) {
        if (!list_empty(&c->full)) {
            list_entry_t *le = list_next(&c->full);
            while (le != &c->full) {
                struct slab *t = to_struct(le, struct slab, link);
                if (t == s) { list_del(le); break; }
                le = list_next(le);
            }
        }
        list_add(&c->partial, &s->link);
    }
    // slab 空了：直接收缩，把页还给 buddy
    if (s->inuse == 0) {
        list_del(&s->link);
        free_pages(s->pages, 1u << s->order);
        return;
    }

}

void kmem_cache_destroy(struct kmem_cache *c) {
    //未实现
    (void)c;
}

// -------- kmalloc/kfree：常用尺寸类 --------
#define KMALLOC_CLASSES 10
static const size_t kmalloc_sizes[KMALLOC_CLASSES] = {8,16,32,64,128,256,512,1024,2048,4096};
static struct kmem_cache *kmalloc_caches[KMALLOC_CLASSES];


//给一个最小的对象大小
static inline int kmalloc_index(size_t sz) {
    for (int i=0;i<KMALLOC_CLASSES;i++) if (sz<=kmalloc_sizes[i]) return i;
    return -1;
}

//为每个常用尺寸类创建 kmem_cache
void kmalloc_init(void) {
    for (int i = 0; i < KMALLOC_CLASSES; i++) {
        size_t sz = kmalloc_sizes[i];
        uint16_t order = 0;

        // 选择最小的 order，使 slab（2^order 页）至少容纳 1 个对象
        while (1) {
            size_t slab_bytes = ((size_t)1 << order) * PGSIZE;
            size_t align = sizeof(void*);
            size_t off = (sizeof(struct slab) + align - 1) & ~(align - 1);
            size_t payload = slab_bytes - off;
            if (payload >= sz) break;
            order++;
        }

        kmalloc_caches[i] =
            kmem_cache_create("kmalloc", sz, sizeof(void*), order, NULL);
        assert(kmalloc_caches[i] != NULL);
    }
}


void *kmalloc(size_t size) {
    int idx = kmalloc_index(size);
    if (idx < 0) { // 大块直接页分配
        size_t pages = (size + PGSIZE - 1) / PGSIZE;
        struct Page *p = alloc_pages(pages);
        return p ? (void *)(page2pa(p) + va_pa_offset) : NULL;
    }
    return kmem_cache_alloc(kmalloc_caches[idx]);
}

void kfree(void *p, size_t size) {
    if (!p) return;
    int idx = kmalloc_index(size);
    if (idx < 0) {
        size_t pages = (size + PGSIZE - 1) / PGSIZE;
        struct Page *base = pa2page((uintptr_t)p - va_pa_offset);
        free_pages(base, pages);
    } else {
        kmem_cache_free(kmalloc_caches[idx], p);
    }
}

// 自测
static uint64_t slub_rng = 1;
static inline uint32_t rnd32(void){ slub_rng=slub_rng*6364136223846793005ULL+1; return (uint32_t)(slub_rng>>32); }

void slub_check(void) {
    cprintf("[slub] check begin\n");
    size_t base = nr_free_pages();

    // 基础类批量 alloc/free
    for (int i=0;i<KMALLOC_CLASSES;i++) {
        size_t sz = kmalloc_sizes[i];
        enum { N=256 };
        void *ptrs[N];
        for (int j=0;j<N;j++){ ptrs[j]=kmalloc(sz); assert(ptrs[j]); memset(ptrs[j],0xA5,sz); }
        for (int j=0;j<N;j++) kfree(ptrs[j], sz);
    }

    // 大块直走页分配
    void *p1 = kmalloc(PGSIZE-32); assert(p1); kfree(p1, PGSIZE-32);
    void *p2 = kmalloc(PGSIZE+128); assert(p2); kfree(p2, PGSIZE+128);

    // 随机压力
    enum { M=1024, LIVE=256 };
    struct { void* p; size_t sz; } live[LIVE]; int n=0;
    for (int i=0;i<M;i++){
        if ((rnd32()%3) && n>0){ int k=rnd32()%n; kfree(live[k].p, live[k].sz); live[k]=live[n-1]; n--; }
        else { size_t sz=(rnd32()%5000)+1; void* p=kmalloc(sz); if(p){ if(n<LIVE) live[n++] = (typeof(live[0])){p,sz}; else kfree(p,sz);} }
    }
    for (int i=0;i<n;i++) kfree(live[i].p, live[i].sz);

    size_t end = nr_free_pages();
    assert(base == end);
    cprintf("[slub] check OK, free=%ld pages\n", end);
}

void slub_init(void){ kmalloc_init(); }
