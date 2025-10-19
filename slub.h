// kern/mm/slub.h
#ifndef __KERN_MM_SLUB_H__
#define __KERN_MM_SLUB_H__

#include <defs.h>
#include <list.h>
#include <pmm.h>

typedef void (*ctor_t)(void *obj);

struct slab;

struct kmem_cache {
    const char   *name;
    size_t        obj_size;
    size_t        align;
    uint16_t      order;          // slab占用 2^order 页
    uint16_t      objs_per_slab;  // 一个slab能容纳的对象个数
    ctor_t        ctor;           // 可选构造函数
    list_entry_t  partial;        // 仍有空闲对象
    list_entry_t  full;           // 已用满
    list_entry_t  empty;          // 空slab（可选回收）
    size_t        inuse_objs;     // 已分配对象数(统计/测试)
};

struct slab {
    list_entry_t  link;           // 链到 cache 对应列表
    struct kmem_cache *cache;     // 反向指针
    void         *freelist;       // 空闲对象单链表(复用对象首字)
    uint16_t      inuse;          // 已用对象数
    uint16_t      total;          // 总对象数
    uint16_t      order;          // 2^order 页
    struct Page  *pages;          // slab起始页
};

// 初始化与 cache 接口
void slub_init(void);
struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align, uint16_t order, ctor_t ctor);
void *kmem_cache_alloc(struct kmem_cache *cache);
void kmem_cache_free(struct kmem_cache *cache, void *obj);
void kmem_cache_destroy(struct kmem_cache *cache);

// 通用 kmalloc/kfree
void  kmalloc_init(void);
void *kmalloc(size_t size);
void  kfree(void *p, size_t size);

// 自测
void slub_check(void);

#endif /* __KERN_MM_SLUB_H__ */
