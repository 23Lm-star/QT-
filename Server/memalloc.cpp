// 内存池封装实现：带 8 字节头部的分配，池异常时自动降级到 malloc。
#include "memalloc.h"
#include "ConCurrentAlloc.h"

namespace MemAlloc {

std::atomic<bool> s_poolEnabled(true);
std::atomic<bool> s_poolAvailable(true);

// 8 字节头部（bool + 7 字节填充），使 header+1 返回的指针保持最大对齐。
struct MemHeader {
    bool fromPool;
    char padding[7];
};

void* allocate(size_t size) {
    if (size == 0) {
        return nullptr;
    }

    // 头部与用户区一起申请，头部隐式位于返回指针之前。
    size_t totalSize = size + sizeof(MemHeader);

    if (!isPoolEnabled()) {
        MemHeader* header = (MemHeader*)malloc(totalSize);
        if (header != nullptr) {
            header->fromPool = false;
            return (void*)(header + 1);
        }
        return nullptr;
    }

    MemHeader* header = nullptr;
    // 池内可能抛 bad_alloc；捕获后永久关闭池路径并改用 malloc 回退。
    try {
        header = (MemHeader*)ConCurrentAlloc(totalSize);
    } catch (...) {
        setPoolAvailable(false);
    }

    if (header == nullptr) {
        setPoolAvailable(false);
        header = (MemHeader*)malloc(totalSize);
        if (header != nullptr) {
            header->fromPool = false;
            return (void*)(header + 1);
        }
        return nullptr;
    }

    header->fromPool = true;
    return (void*)(header + 1);
}

void deallocate(void* ptr) {
    if (ptr == nullptr) {
        return;
    }

    // 指针运算回退一个 MemHeader，得到分配时写入的元数据。
    MemHeader* header = (MemHeader*)ptr - 1;

    // 池内对象必须还回池；若归还过程异常，仍以 free 兜底避免泄漏。
    if (header->fromPool) {
        try {
            ConCurrentFree(header);
            return;
        } catch (...) {
        }
    }

    free(header);
}

}