// kern/mm/buddy_pmm.c
#include <defs.h>
#include <list.h>
#include <pmm.h>
#include <memlayout.h>
#include <string.h>
#include <stdio.h>

// ------------------------------
// Buddy System 说明
// - 每个阶 (order) 的块大小 = (1 << order) 页
// - 空闲链表只挂“块头 Page”，并用 PageProperty(head)=1 标识
// - 在 buddy 实现里，Page.property 字段存放“阶 (order)”
// - 伙伴页号: buddy_idx = idx ^ (1 << order)  (idx 是物理页号, 非字节地址!)——————核心理解
// ------------------------------


//找到一个最小的k，使得2的k次方大于等于n
static inline int ceil_log2_pages(size_t n) {
    size_t p = 1; int k = 0;
    while (p < n) { p <<= 1; k++; }
    return k;
}

static inline void check_block_alignment(struct Page *p, int order) {
    // 阶=order 的 buddy 块：起始页号必须 2^order 对齐（低 order 位为 0）
    size_t idx  = (size_t)(p - pages) + nbase;   // page_idx()
    size_t mask = ((size_t)1 << order) - 1;
    assert((idx & mask) == 0);
}


typedef struct {
    list_entry_t free_list;   
    unsigned int nr_free;  
} free_area_buddy_t;

static free_area_buddy_t *buddy_area = NULL;  // [0..max_order] 的数组
static int max_order = 0;                     // 运行时计算的最大阶上限
static size_t buddy_total_free_pages = 0;     // 当前系统空闲“页总数”

// 返回物理页号
static inline size_t page_idx(struct Page *p) {
    return (size_t)((p - pages) + nbase);//nabase是物理页起始页号
}
// 根据物理页号返回 Page 指针
static inline struct Page *idx_page(size_t idx) {
    return pages + (idx - nbase);
}




// 根据可用页总数计算最大阶
static int calc_max_order_by_total(size_t total_pages) {
    int k = 0;
    while ((1u << (k + 1)) <= total_pages) k++;
    return k;
}

static void buddy_lists_clear(void) {
    // 分配 buddy_area（用内核的小堆/静态内存不方便，这里用固定上限 32）
    static free_area_buddy_t static_area[32];   // 支持到 order=31（2^31 页）
    buddy_area = static_area;
    for (int i = 0; i <= 31; i++) {
        list_init(&buddy_area[i].free_list);
        buddy_area[i].nr_free = 0;
    }
    buddy_total_free_pages = 0;
}

// 挂入该阶链表（作为空闲“块头”） 头插法
static inline void buddy_push(int order, struct Page *head) {
    SetPageProperty(head);    
    head->property = order;   
    list_add(&buddy_area[order].free_list, &head->page_link);
    buddy_area[order].nr_free++;
}

// 从该阶链表移除节点（不修改标志）
static inline void buddy_erase_node(struct Page *head) {
    list_del(&head->page_link);
}

// 从该阶链表“弹出”此块（并清空块头标志）
static inline void buddy_pop(int order, struct Page *head) {
    buddy_erase_node(head);
    ClearPageProperty(head);
    buddy_area[order].nr_free--;
}

// 在 [cur, cur+remain) 中找“最大且对齐”的阶
static int max_order_fit(struct Page *cur, size_t remain) {
    size_t idx = page_idx(cur);
    for (int k = max_order; k >= 0; k--) {
        size_t blk = (1u << k);
        if (blk <= remain && (idx % blk == 0)) return k;
    }
    return -1; 
}

// pmm_manager 接口实现 

//.init: 初始化内部桶结构（不接收可用页区间）
static void buddy_init(void) {
    buddy_lists_clear();
    max_order = 0;
}

//.init_memmap: 把 [base, base+n) 的可用物理页构造成按阶对齐的空闲块 
static void buddy_init_memmap(struct Page *base, size_t n) {
    if (n == 0) return;

    // 计算本次可用页数上限，可与历史 max_order 取更大值
    int mo = calc_max_order_by_total(npage - nbase); 
    if (mo > 31) mo = 31;
    if (mo > max_order) max_order = mo;

    for (struct Page *p = base; p != base + n; p++) {
        p->flags = 0;
        set_page_ref(p, 0);
    }

    size_t remain = n;
    struct Page *cur = base;
    while (remain) {
        int k = max_order_fit(cur, remain);
        if (k < 0) {
            k = 0;
        }
        buddy_push(k, cur);
        cur += (1u << k);
        remain -= (1u << k);
    }

    buddy_total_free_pages += n;
}

static struct Page *buddy_alloc_pages(size_t n) {
    if (n == 0) return NULL;
    if (n > buddy_total_free_pages) return NULL;

    int need_k = ceil_log2_pages(n);

    // 从 need_k 阶开始向上找首个非空阶 j
    int j = need_k;
    while (j <= max_order && list_empty(&buddy_area[j].free_list)) j++;
    if (j > max_order) return NULL; 

    list_entry_t *le = list_next(&buddy_area[j].free_list);
    struct Page *blk = le2page(le, page_link);
    buddy_pop(j, blk);

    // 逐级二分拆到 need_k
    while (j > need_k) {
        j--;
        struct Page *right = blk + (1u << j);  
        buddy_push(j, right);
        // 左半块（blk）继续往下拆
    }


    ClearPageProperty(blk);              
    buddy_total_free_pages -= (1u << need_k);
    return blk;
}

// .free_pages: 释放从 base 开始的 >= n 页连续块，并尽可能向上合并
static void buddy_free_pages(struct Page *base, size_t n) {
    if (n == 0) return;

    // 分配的时候就是分配的order页，释放的时候当然从order开始合并
    int order = ceil_log2_pages(n);
    size_t idx = page_idx(base);
    struct Page *cur = base;

    // 逐阶向上尝试与伙伴合并
    while (order < max_order) {
        size_t buddy_idx = idx ^ (1u << order);   
        struct Page *bud = idx_page(buddy_idx);

        // 判断伙伴并摘除
        int can_merge = 0;
        if (PageProperty(bud) && bud->property == (unsigned)order) {
            list_entry_t *pos = list_next(&buddy_area[order].free_list);
            while (pos != &buddy_area[order].free_list) {
                struct Page *tmp = le2page(pos, page_link);
                if (tmp == bud) {
                    buddy_erase_node(bud);
                    ClearPageProperty(bud);
                    buddy_area[order].nr_free--;
                    can_merge = 1;
                    break;
                }
                pos = list_next(pos);
            }
        }
        if (!can_merge) break;

        if (buddy_idx < idx) { cur = bud; idx = buddy_idx; }
        order++;
    }

    // 把最终合并后的块挂回对应阶
    SetPageProperty(cur);
    cur->property = order;
    list_add(&buddy_area[order].free_list, &cur->page_link);
    buddy_area[order].nr_free++;

    buddy_total_free_pages += (1u << ceil_log2_pages(n));
}


static size_t buddy_nr_free_pages(void) {
    return buddy_total_free_pages;
}



//生成一个无符号整型32位的随机数
static uint64_t prng_state = 1;
static inline uint32_t rnd32(void) {          
    prng_state = prng_state * 6364136223846793005ULL + 1;
    return (uint32_t)(prng_state >> 32);
}




static void buddy_check(void) {
    cprintf("[buddy] check begin\n");
    size_t baseline = nr_free_pages();

    // 1) 基本 sanity + 非2幂向上取整
    {
        struct Page *p1 = alloc_pages(1);  assert(p1);
        struct Page *p3 = alloc_pages(3);  assert(p3);
        check_block_alignment(p3, ceil_log2_pages(3));
        free_pages(p1, 1);
        free_pages(p3, 3);
        assert(baseline == nr_free_pages());
    }

    // 2) 合并路径：8 -> (4,4) -> 合回 8 
    {
        struct Page *b8 = alloc_pages(8);  assert(b8);
        check_block_alignment(b8, 3);     
        struct Page *left4  = b8;
        struct Page *right4 = b8 + 4;
        free_pages(right4, 4);
        free_pages(left4,  4);
        assert(baseline == nr_free_pages());
    }

    // 3) 跨阶混合：交错分配、乱序释放
    {
        struct Page *a4 = alloc_pages(4);  assert(a4);
        struct Page *b2 = alloc_pages(2);  assert(b2);
        struct Page *c1 = alloc_pages(1);  assert(c1);
        check_block_alignment(a4, 2);
        check_block_alignment(b2, 1);
        check_block_alignment(c1, 0);

        free_pages(c1, 1);
        free_pages(b2, 2);
        free_pages(a4, 4);
        assert(baseline == nr_free_pages());
    }

    // 4) 边界：拒绝不合法/过大请求
    {
        size_t too_big = baseline + 1;
        assert(alloc_pages(too_big) == NULL);
        assert(baseline == nr_free_pages());
    }

    // 5) 非 2 幂一致性：多组对齐检查
    {
        for (size_t req = 1; req <= 17; req++) {
            struct Page *p = alloc_pages(req);  assert(p);
            check_block_alignment(p, ceil_log2_pages(req));
            free_pages(p, req);
        }
        assert(baseline == nr_free_pages());
    }

    // 6) 随机压力（有限步、确定性）
    {
        enum { N = 512, LIVE_MAX = 128 };
        struct { struct Page *p; size_t req; } live[LIVE_MAX];
        int live_cnt = 0;

        for (int i = 0; i < N; i++) {
            int op = rnd32() % 3;                  // ~2/3 alloc, 1/3 free
            if (op != 0 && live_cnt > 0) {
                int k = rnd32() % live_cnt;
                free_pages(live[k].p, live[k].req);
                live[k] = live[live_cnt - 1];
                live_cnt--;
            } else {
                size_t req = (rnd32() % 32) + 1;   // 1..32 页
                struct Page *p = alloc_pages(req);
                if (p) {
                    check_block_alignment(p, ceil_log2_pages(req));
                    if (live_cnt < LIVE_MAX) {
                        live[live_cnt++] = (typeof(live[0])){ .p = p, .req = req };
                    } else {
                        free_pages(p, req);
                    }
                }
            }
        }
        for (int i = 0; i < live_cnt; i++) free_pages(live[i].p, live[i].req);
        assert(baseline == nr_free_pages());
    }

    cprintf("[buddy] check OK, free=%ld pages\n", nr_free_pages());
}

// 对外导出管理器实例
const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages  = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};
