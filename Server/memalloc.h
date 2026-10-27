// 对外内存分配封装：可选启用自研内存池，关闭或失败时回退到 malloc/free。
// 每次分配额外携带一个 MemHeader，释放时据此判断来源（池内/系统）。
#pragma once

#include <stdlib.h>
#include <atomic>

namespace MemAlloc {

// 总开关（可由调用方动态关闭）与可用性（池分配失败后置为 false）。
extern std::atomic<bool> s_poolEnabled;
extern std::atomic<bool> s_poolAvailable;

inline void enablePool(bool enable) {
    s_poolEnabled.store(enable);
}

// 两个标志都成立才允许走内存池。
inline bool isPoolEnabled() {
    return s_poolEnabled.load() && s_poolAvailable.load();
}

inline void setPoolAvailable(bool available) {
    s_poolAvailable.store(available);
}

// 分配用户内存：含头部开销，池不可用时自动降级。
void* allocate(size_t size);
// 释放 allocate 返回的指针，内部区分池内/系统来源。
void deallocate(void* ptr);

}

// 统一宏入口，业务代码通过它使用分配器，便于整体切换实现。
#define memalloc(size) MemAlloc::allocate(size)
#define memfree(ptr) MemAlloc::deallocate(ptr)