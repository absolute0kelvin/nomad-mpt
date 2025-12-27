// C wrapper for Concurrency Kit FIFO
// 避免 C++ 编译时的类型转换问题

#ifndef CK_WRAPPER_H
#define CK_WRAPPER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// SPSC FIFO - 单生产者单消费者（用于 Request 队列）
// ============================================================

// Opaque FIFO handle
typedef struct ck_fifo_spsc_wrapper ck_fifo_spsc_wrapper_t;

// 创建/销毁 FIFO
ck_fifo_spsc_wrapper_t* ck_fifo_spsc_wrapper_create(void);
void ck_fifo_spsc_wrapper_destroy(ck_fifo_spsc_wrapper_t* fifo);

// 入队（producer）
// 将 value 入队，entry 是临时存储（调用者分配）
void ck_fifo_spsc_wrapper_enqueue(ck_fifo_spsc_wrapper_t* fifo, void* entry, void* value);

// 出队（consumer）
// 返回 value 指针，如果队列空返回 NULL
// garbage_out 返回可回收的 entry（可选，用于内存复用，通过 ck_fifo_spsc_recycle）
void* ck_fifo_spsc_wrapper_dequeue(ck_fifo_spsc_wrapper_t* fifo, void** garbage_out);

// 检查队列是否为空
bool ck_fifo_spsc_wrapper_isempty(ck_fifo_spsc_wrapper_t* fifo);

// 分配 entry（16 字节对齐）
void* ck_fifo_spsc_wrapper_alloc_entry(void);
void ck_fifo_spsc_wrapper_free_entry(void* entry);

// ============================================================
// MPMC FIFO - 多生产者多消费者（用于 Completion 队列）
// 多 worker 线程 enqueue，单 Rust 线程 dequeue
// ============================================================

typedef struct ck_fifo_mpmc_wrapper ck_fifo_mpmc_wrapper_t;

// 创建/销毁 MPMC FIFO
ck_fifo_mpmc_wrapper_t* ck_fifo_mpmc_wrapper_create(void);
void ck_fifo_mpmc_wrapper_destroy(ck_fifo_mpmc_wrapper_t* fifo);

// 入队（多线程安全）
void ck_fifo_mpmc_wrapper_enqueue(ck_fifo_mpmc_wrapper_t* fifo, void* entry, void* value);

// 出队（多线程安全）
// 返回 value 指针，如果队列空返回 NULL
// garbage_out 返回可释放的 garbage entry（调用者应调用 free_entry 释放）
void* ck_fifo_mpmc_wrapper_dequeue(ck_fifo_mpmc_wrapper_t* fifo, void** garbage_out);

// 检查队列是否为空
bool ck_fifo_mpmc_wrapper_isempty(ck_fifo_mpmc_wrapper_t* fifo);

// 分配 MPMC entry
void* ck_fifo_mpmc_wrapper_alloc_entry(void);
void ck_fifo_mpmc_wrapper_free_entry(void* entry);

// ============================================================
// 通用工具
// ============================================================

// CPU stall（等待时的低功耗暂停）
void ck_wrapper_stall(void);

// 内存屏障
void ck_wrapper_fence_store(void);

#ifdef __cplusplus
}
#endif

#endif // CK_WRAPPER_H

