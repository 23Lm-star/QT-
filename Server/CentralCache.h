// CentralCache（中心缓存）：位于 ThreadCache 与 PageCache 之间，
// 按大小类维护 span 链表，把 span 切块后成批借给 ThreadCache，并回收其归还的块；
// 进程内单例，每个桶由 SpanList 自带的互斥锁保护。
#pragma once
#include"Common.h"
//CentralCache（中心缓存）
//设计成单例模式(饿汉模式)
class CentralCache{
public:
	// 返回进程内唯一实例；函数内静态对象的初始化由 C++11 保证线程安全。
	static CentralCache* GetInstance(){
		return &_CenIns;
	}

	// 在CentralCage指定的桶里找span给ThreadCache
	// 在桶中找一个仍有空闲块的 span；没有则向 PageCache 申请并切块。调用方必须已持有 list 的锁。
	span* GetOneSpan(SpanList& list, size_t size);

	// 从CentralCache获取一定数量的内存给ThreadCache
	// 从 span 摘取至多 batchNum 个块，start/end 返回链表首尾，返回实际摘取数量。
	size_t FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size);

	// 将一定数量的对象释放到span
	// 把 ThreadCache 归还的一批块按各自所属 span 挂回；UseCount 归零的 span 还给 PageCache。
	void ReleaseListToSpans(void* start, size_t size);

private:
	// 每个大小类对应一个 span 桶，下标与 SizeClass::Index 完全一致。
	SpanList _SpanList[MaxFreeLists];
// 私有构造并禁用拷贝，保证单例；实例与 span 对象池定义在 .cpp 中。
private:
	CentralCache() 
	{}
	CentralCache(const CentralCache&) = delete;

	static CentralCache _CenIns;
};