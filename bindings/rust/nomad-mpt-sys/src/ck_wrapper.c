// C wrapper for Concurrency Kit FIFO
// 编译: cc -std=c11 -c ck_wrapper.c -I../depend/ck/include

#include "ck_wrapper.h"

#include <ck_fifo.h>
#include <ck_pr.h>
#include <stdlib.h>

// aligned_alloc 要求 size 是 alignment 的倍数，否则是 UB。
// ck_fifo_mpmc_entry_t 在某些平台上可能是 24 字节，因此这里将 size 向上取整。
static inline void* ck_aligned_malloc(size_t alignment, size_t size) {
    if (alignment == 0) return NULL;
    size_t rounded = ((size + alignment - 1) / alignment) * alignment;
    return aligned_alloc(alignment, rounded);
}

// ============================================================
// SPSC Wrapper 结构体
// ============================================================

struct ck_fifo_spsc_wrapper {
    ck_fifo_spsc_t fifo;
};

ck_fifo_spsc_wrapper_t* ck_fifo_spsc_wrapper_create(void) {
    ck_fifo_spsc_wrapper_t* wrapper = malloc(sizeof(ck_fifo_spsc_wrapper_t));
    if (!wrapper) return NULL;
    
    // 分配 stub entry
    ck_fifo_spsc_entry_t* stub = malloc(sizeof(ck_fifo_spsc_entry_t));
    if (!stub) {
        free(wrapper);
        return NULL;
    }
    
    ck_fifo_spsc_init(&wrapper->fifo, stub);
    return wrapper;
}

void ck_fifo_spsc_wrapper_destroy(ck_fifo_spsc_wrapper_t* fifo) {
    if (!fifo) return;
    
    // ck_fifo_spsc 的设计：
    // - init 时需要一个 stub entry，它会作为初始的 head
    // - enqueue 时，新 entry 成为新的 head，旧 head 通过下一次 dequeue 返回
    // - dequeue 返回的是之前的 head（可以被释放），同时取出值
    //
    // 因此，在销毁时：
    // 1. 如果队列空，head == tail，只需释放 head（原始 stub）
    // 2. 如果队列非空，需要释放所有节点
    
    // 简化处理：直接释放 head
    // 注意：如果队列中有残留节点，它们会泄漏
    // 在实际使用中，应该先确保队列为空
    if (fifo->fifo.head) {
        free(fifo->fifo.head);
    }
    
    free(fifo);
}

void ck_fifo_spsc_wrapper_enqueue(ck_fifo_spsc_wrapper_t* fifo, void* entry, void* value) {
    if (!fifo) return;
    ck_fifo_spsc_enqueue(&fifo->fifo, (ck_fifo_spsc_entry_t*)entry, value);
}

void* ck_fifo_spsc_wrapper_dequeue(ck_fifo_spsc_wrapper_t* fifo, void** garbage_out) {
    if (!fifo) return NULL;
    
    // ck_fifo_spsc_dequeue 的第二个参数是输出 value 的位置
    // API: bool ck_fifo_spsc_dequeue(fifo, void *value)
    // 它会把 entry->value 写入到 value 指向的位置
    void* value = NULL;
    if (ck_fifo_spsc_dequeue(&fifo->fifo, &value)) {
        // 获取可回收的 garbage entry（用于内存复用）
        if (garbage_out) {
            *garbage_out = ck_fifo_spsc_recycle(&fifo->fifo);
        }
        return value;  // 返回用户存储的值
    }
    return NULL;
}

bool ck_fifo_spsc_wrapper_isempty(ck_fifo_spsc_wrapper_t* fifo) {
    if (!fifo) return true;
    return ck_fifo_spsc_isempty(&fifo->fifo);
}

void* ck_fifo_spsc_wrapper_alloc_entry(void) {
    // 分配 entry + 足够的空间存储用户数据
    // ck_fifo_spsc_entry_t 是 16 字节
    return aligned_alloc(16, sizeof(ck_fifo_spsc_entry_t));
}

void ck_fifo_spsc_wrapper_free_entry(void* entry) {
    free(entry);
}

// ============================================================
// MPMC Wrapper 结构体
// 用于 completion 和 large_value 队列（多 worker enqueue，单 Rust dequeue）
// ============================================================

struct ck_fifo_mpmc_wrapper {
    ck_fifo_mpmc_t fifo;
};

ck_fifo_mpmc_wrapper_t* ck_fifo_mpmc_wrapper_create(void) {
    ck_fifo_mpmc_wrapper_t* wrapper = malloc(sizeof(ck_fifo_mpmc_wrapper_t));
    if (!wrapper) return NULL;
    
    // MPMC FIFO 也需要 stub entry
    ck_fifo_mpmc_entry_t* stub = ck_aligned_malloc(16, sizeof(ck_fifo_mpmc_entry_t));
    if (!stub) {
        free(wrapper);
        return NULL;
    }
    
    ck_fifo_mpmc_init(&wrapper->fifo, stub);
    return wrapper;
}

void ck_fifo_mpmc_wrapper_destroy(ck_fifo_mpmc_wrapper_t* fifo) {
    if (!fifo) return;
    
    // 使用 deinit 获取需要释放的节点
    ck_fifo_mpmc_entry_t* garbage = NULL;
    ck_fifo_mpmc_deinit(&fifo->fifo, &garbage);
    if (garbage) {
        free(garbage);
    }
    
    free(fifo);
}

void ck_fifo_mpmc_wrapper_enqueue(ck_fifo_mpmc_wrapper_t* fifo, void* entry, void* value) {
    if (!fifo) return;
    ck_fifo_mpmc_enqueue(&fifo->fifo, (ck_fifo_mpmc_entry_t*)entry, value);
}

void* ck_fifo_mpmc_wrapper_dequeue(ck_fifo_mpmc_wrapper_t* fifo, void** garbage_out) {
    if (!fifo) return NULL;
    
    // ck_fifo_mpmc_trydequeue 的签名：
    // bool ck_fifo_mpmc_trydequeue(fifo, void *value, ck_fifo_mpmc_entry_t **garbage)
    // - value: 输出用户存储的值
    // - garbage: 输出可以释放的旧 entry
    ck_fifo_mpmc_entry_t* garbage = NULL;
    void* value = NULL;
    
    if (ck_fifo_mpmc_trydequeue(&fifo->fifo, &value, &garbage)) {
        if (garbage_out) *garbage_out = garbage;
        return value;  // 返回用户存储的值，而不是 garbage entry
    }
    return NULL;
}

bool ck_fifo_mpmc_wrapper_isempty(ck_fifo_mpmc_wrapper_t* fifo) {
    if (!fifo) return true;
    // ⚠️ 注意：这是一个非原子的启发式检查，在 MPMC 环境下可能不准确！
    // 
    // 问题：
    // 1. 在检查 head 和 head->next 之间，其他线程可能已经修改了队列
    // 2. 可能出现 ABA 问题：队列从空变非空再变空
    // 3. 此函数仅用于性能优化提示，不应用于同步逻辑
    //
    // 正确用法：
    // - 用于避免不必要的 dequeue 调用（优化）
    // - 不要依赖此结果做出关键决策
    // - 如果需要精确判断，应使用 trydequeue 并检查返回值
    //
    // MPMC 没有 isempty 函数，通过检查 head->next 是否为 NULL 判断
    // 如果 head 的 next 指针为 NULL，队列为空
    ck_fifo_mpmc_entry_t* head = ck_pr_load_ptr(&fifo->fifo.head.pointer);
    if (!head) return true;
    return ck_pr_load_ptr(&head->next.pointer) == NULL;
}

void* ck_fifo_mpmc_wrapper_alloc_entry(void) {
    // ck_fifo_mpmc_entry_t 通常更大（包含 CAS 需要的字段）
    return ck_aligned_malloc(16, sizeof(ck_fifo_mpmc_entry_t));
}

void ck_fifo_mpmc_wrapper_free_entry(void* entry) {
    free(entry);
}

// ============================================================
// 通用工具
// ============================================================

void ck_wrapper_stall(void) {
    ck_pr_stall();
}

void ck_wrapper_fence_store(void) {
    ck_pr_fence_store();
}

