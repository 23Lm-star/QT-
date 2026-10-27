// ThreadCache（线程缓存）：每个线程独享，用无锁路径响应不大于 256KB 的分配与释放。
// 每个大小类对应一条自由链表；不足时向 CentralCache 批量补货，过多时批量归还。
#pragma once

#include"Common.h"

class ThreadCache
{
public:
	// 主分配入口：定位桶后头删；桶为空时向 CentralCache 批量补货。
	void* Allocate(size_t size);
	// 释放入口：头插回对应桶；达到批量阈值时摘出一批归还。
	void Deallocate(void* ptr, size_t size);
	// 向 CentralCache 申请一批块：自己留 1 块返回，其余挂入本地桶。
	void* FetchFromCentralCache(size_t index, size_t size);
	// 本地链表过长时，一次摘出 MaxSize 个块成批归还，减少 CentralCache 往返。
	void ListTooLong(FreeList& list, size_t size);

private:
	// 线程本地自由链表数组，下标与 SizeClass::Index 一致。
	FreeList _FreeLists[MaxFreeLists];
};

// 返回线程本地指针的引用；首次使用前为 nullptr，由 ConCurrentAlloc 惰性创建。
// Windows 使用 __declspec(thread)，其他平台使用 thread_local，各线程互不影响。
inline ThreadCache*& GetTLSThreadCache()
{
#ifdef _WIN32
	__declspec(thread) static ThreadCache* pTLSThreadCache = nullptr;
#else
	thread_local static ThreadCache* pTLSThreadCache = nullptr;
#endif
	return pTLSThreadCache;
}