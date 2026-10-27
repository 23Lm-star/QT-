// 内存池统一分配/释放入口：小对象走线程缓存，大对象直接走页缓存。
#pragma once 

#include "Common.h"
#include "ThreadCache.h"
#include "PageCache.h"
#include "ObjectPool.h"

// size <= 256KB 走当前线程的 ThreadCache；更大直接向 PageCache 申请整页。
void* ConCurrentAlloc(size_t size);
// 通过地址反查 span，并依据分配时记录的块大小选择归还路径。
void ConCurrentFree(void* ptr);