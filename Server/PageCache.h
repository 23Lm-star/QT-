// PageCache（页缓存）：以 8KB 页为单位的最终分配层，
// 按页数分桶管理空闲 span，不足时向系统申请，回收时与相邻空闲 span 合并；
// 同时维护“页号 -> span*”映射，供任意对象地址反查所属 span；单例 + 全局互斥锁。
#pragma once

#include "Common.h"
#include "ObjectPool.h"
#include "RadixTreeHash.h"
//PageCache（页缓存）
//单例模式(饿汉模式)
class PageCache{
public:
	// 返回进程内唯一实例。
	static PageCache* GetInstance(){
		return &_PCIns;
	}
	//返回互斥量
	// 全局锁：NewSpan、ReleaseSpanToPageCache 以及页映射读写的公共保护。
	std::mutex& GetMutex(){
		return _mtx;
	}

	// 获取一个K页的span
	// 取一个恰好或至少 k 页的 span，必要时向系统申请并切分。调用方必须持有全局锁。
	span* NewSpan(size_t k);

	// 获取从内存地址到span的映射
	// 由对象地址反查所属 span：地址右移 PageShift 得到页号再查映射。
	span* MapObjectToSpan(void* obj);

	// 释放空闲span回到Pagecache，并合并相邻的span
	// 回收 span：与前后相邻空闲 span 合并后按页数重新入桶，超大 span 直接还给系统。
	void ReleaseSpanToPageCache(span* span);

private:
	// 下标为 span 页数 1..128；PageList[0] 不使用。
	SpanList _PageList[MaxPage];
	std::mutex _mtx;//申请内存时加锁

	// 64 位进程页号 51 位，采用三层映射；32 位进程页号 19 位，采用单层映射。
	//记录页号到span的映射
	//通过任意内存地址，能快速找到它属于哪个 Span
	//std::unordered_map<Page_t, span*> _PageToSpan;//建立页号与span*的映射
#ifdef _WIN64
	TCMalloc_PageMap3<64 - PageShift> _PageToSpan;
#elif _WIN32
	TCMalloc_PageMap1<32 - PageShift> _PageToSpan;
#endif//..
	// span 结构体同样从对象池分配，避免高频 new/delete。
	ObjectPool<span> _pool;
private:
	PageCache() {}
	PageCache(const PageCache&) = delete;
	static PageCache _PCIns;//全局只有一个
};