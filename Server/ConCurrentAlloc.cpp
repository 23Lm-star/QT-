// 三级缓存对外接口：负责线程缓存的惰性创建，以及按请求大小分流。
#include "ConCurrentAlloc.h"
#include "ThreadCache.h"

void* ConCurrentAlloc(size_t size){
	if (size > MaxBytes){
		// 大对象：按页取整后整 span 分配，不切块，也不占用 ThreadCache。
		size_t align = SizeClass::RoundUp(size);
		// RoundUp 已把大小对齐到 8KB 倍数，右移立刻得到页数。
		size_t page = align >> PageShift;

		// PageCache 的所有操作都在其全局锁下进行。
		PageCache::GetInstance()->GetMutex().lock();
		span* Span = PageCache::GetInstance()->NewSpan(page);
		Span->_FreeKnowSize = align;
		PageCache::GetInstance()->GetMutex().unlock();

		// Page_Adder 是页号，左移还原为内存地址。
		return (void*)(Span->Page_Adder << PageShift);

	}
	else{
		if (GetTLSThreadCache() == nullptr)
		{
			// 双重检查：先无锁读 TLS，为空才加全局锁创建，避免每次分配都加锁。
			static std::mutex tcMtx;
			static ObjectPool<ThreadCache> TLPool;
			tcMtx.lock();
			if (GetTLSThreadCache() == nullptr) {
				GetTLSThreadCache() = TLPool.New();
			}
			tcMtx.unlock();
		}

		return GetTLSThreadCache()->Allocate(size);
	}
}

void ConCurrentFree(void* ptr){
	// 地址右移得到页号，查映射得到分配时登记的 span。
	span* Span = PageCache::GetInstance()->MapObjectToSpan(ptr);
	size_t size = Span->_FreeKnowSize;

	if (size > MaxBytes){
		// 大对象把整个 span 还给 PageCache，可能触发与相邻空闲 span 的合并。
		PageCache::GetInstance()->GetMutex().lock();
		PageCache::GetInstance()->ReleaseSpanToPageCache(Span);
		PageCache::GetInstance()->GetMutex().unlock();
	}
	else{
		assert(GetTLSThreadCache());
		GetTLSThreadCache()->Deallocate(ptr, size);
	}
}