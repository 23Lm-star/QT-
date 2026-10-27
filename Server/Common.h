// 内存池公共设施：大小类常量、系统页分配、自由链表、span 与 span 链表。
// 三级结构：ThreadCache（线程本地）-> CentralCache（全局分桶）-> PageCache（页级），
// 不大于 256KB 的对象走三级缓存，更大的请求直接按 8KB 页向系统申请。
#pragma once

#include<iostream>
#include<vector>
#include<unordered_map>

#include<time.h>
#include<assert.h>

#include<thread>
#include<mutex>

#include<algorithm>
#include<atomic>

#include"ObjectPool.h"

using std::cout;
using std::endl;

// 桶总数：128B 以内 16 档 + 三个区间各 56 档 + 256KB 区间 24 档。
static const size_t MaxFreeLists = 208;//���������������(������ֵ)
// 小对象上限：超过该值不再切块，直接走 PageCache 页分配。
static const size_t MaxBytes = 256 * 1024;//(��������ֽ���)
// 页桶下标范围 1..128；0 号桶保留不用。
static const size_t MaxPage = 129;//span�������ҳ��-1
// 一页 2^13 = 8KB；页号与地址之间通过移位互换。
static const size_t PageShift = 13;//�趨һҳ��СΪ2^13(8K)

//����ҳ����������Page_t
//_WIN64ֻ��64λ�����_WIN32���ж���
#ifdef _WIN64 //���л���64λ
	// 页号类型：地址右移 PageShift 得到；64 位下页号可达 2^51。
	typedef unsigned long long Page_t;
#elif _WIN32 //���л���32λ
	typedef size_t Page_t;
#else 
	//Linux
#endif

#ifdef _WIN32
#include<Windows.h>
#else 
	//Linux
#endif

//ʹ��ϵͳ��������ռ�(����ҳ����)
	inline static void* SystemAlloc(size_t page)
	{
#ifdef _WIN32
		void* ptr = VirtualAlloc(
			0,       // ��ϵͳ�Զ�ѡ���ַ
			page<<PageShift,          // �����С(�ֽ���)
			MEM_COMMIT | MEM_RESERVE, // �ύ�������ڴ�
			PAGE_READWRITE // �ɶ���д
		);
#else
		//ʹ��Linux��mmap...
		void* ptr = mmap(
			nullptr,					  // ��ϵͳ�Զ�ѡ���ַ
			page * (1 << 13),              // �����С
				PROT_READ | PROT_WRITE,       // �ɶ���д
				MAP_PRIVATE | MAP_ANONYMOUS,  // ˽�������ڴ棨�����ļ�������
				-1,                           // �ļ�������������ӳ����Ϊ -1��
				0                             // ƫ����
				);
#endif
		if (ptr == nullptr)
		{
			throw std::bad_alloc();
			exit(-1);
		}
		return ptr;
	}
	//ʹ��ϵͳ�����ͷſռ�
	inline static void SystemFree(void* ptr){
#ifdef _WIN32
		VirtualFree(ptr, 0, MEM_RELEASE);
#else
		// sbrk unmmap��
#endif
	}

//��ȡ����ǰ��ַ�� ǰ8���ֽ� ��ָ��
// 自由链表后继：复用对象的前 sizeof(void*) 字节存放下一节点地址。
static void*& Next(void* obj){
	return *(void**)obj;
}


//������������
// 单链表自由链表：头插/头删 O(1)；_size 用于判断是否需要批量归还。
class FreeList{
public:
	//ͷ�������ڴ��
	void Push(void* obj){
		assert(obj);
		//ͷ��
		Next(obj) = _FreeList;
		_FreeList = obj;
		++_size;
	}
	//ͷ��ɾ���ڴ��&&����ɾ�����ڴ��
	void* Pop(){
		assert(_FreeList != nullptr);
		//ͷɾ
		void* obj = _FreeList;
		_FreeList = Next(obj);
		--_size;
		return obj;
	}
	//ͷ������һ���ڴ������
	void PushRange(void* start, void* end, size_t n){
		//ͷ��
		Next(end) = _FreeList;
		_FreeList = start;
		_size += n;
	}
	//ͷ��ɾ��һ���ڴ������
	void PopRange(void*& start, void*& end, size_t n){
		assert(n <= _size);
		//ͷɾ(ɾ������������ǰn���ڵ�)
		start = _FreeList;
		end = _FreeList;
		//end�ƶ��������ڶ����ڵ�(׼ȷ��˵�ǵ�n���ڵ�)
		for (size_t i = 0; i < n - 1; i++)
		{
			end = Next(end);
		}
		//�����һ���ڵ���뵽��Ӧλ�õ�Ͱ����Ϊ�µĿ�������
		_FreeList = Next(end);
		//ʣ��������ڵ�start-end��Ϊɾ���Ľڵ�
		Next(end) = nullptr;
		
		//�����ڵ��ܸ���-n
		_size -= n;
	}
	//�жϿ��������Ƿ�Ϊ��
	bool Empty(){
		return _FreeList == nullptr;
	}
	//��ȡ���������ڴ������������ֵ&+ÿһ�λ��յ��ڴ�����
	size_t& MaxSize(){
		return _MaxSize;
	}
	//��ȡ���������ڴ������
	size_t Size(){
		return _size;
	}

private:
	void* _FreeList=nullptr;
	// 每次向 CentralCache 补货的批量上限，随补货成功自适应增长。
	size_t _MaxSize = 1;//������CentralCacle����ĸ���/���������������ɵ�����ڴ�����
	size_t _size = 0;//��ǰ_FreeList���ӵ��ڴ�����
};

//�ڴ湤���ࣺ
//���������ڴ����,����Ͱ��������
// 大小类映射：把任意请求字节数规整到固定档位，并映射到唯一的桶下标。
class SizeClass{
public:
	// ��������
	// [1,128]               8byte����			freelist[0,16)
	// [128+1,1024]			 16byte����			freelist[16,72)
	// [1024+1,8*1024]		 128byte����		freelist[72,128)
	// [8*1024+1,64*1024]	 1024byte����		freelist[128,184)
	// [64*1024+1,256*1024]  8*1024byte����		freelist[184,208)
	
	/// <summary>
	///���ݶ����ֽ���align����bytes�ֽڽ����ڴ���� 
	/// </summary>
	/// <param name="bytes">�ֽ���</param>
	/// <param name="align">�����ֽ���</param>
	/// <returns>������bytes�ֽ���</returns>
	static inline size_t _RoundUp(size_t bytes, size_t align){
		// -1��Ϊ����8�ܶԵ�8    ���λ��1���� �����1���0
		return (bytes + align - 1) & (~(align - 1));
	}

	//���������
	static inline size_t RoundUp(size_t bytes){
		if (bytes <= 128){
			return _RoundUp(bytes, 8);
		}
		else if (bytes <= 1024){
			return _RoundUp(bytes, 16);
		}
		else if (bytes <= 8 * 1024){
			return _RoundUp(bytes, 128);
		}
		else if (bytes <= 64 * 1024){
			return _RoundUp(bytes, 1024);
		}
		else if (bytes <= 256 * 1024){
			return _RoundUp(bytes, 8 * 1024);
		}
		else{
			//����256k��ҳ���ж���
			return _RoundUp(bytes, 1 << PageShift);
		}
	}

	//������ + Ͱ�Ķ�����(+8��+16��+128...)->��ϣͰ����
	// 档位公式：ceil(bytes / 2^AlignIndex) - 1；AlignIndex 即对齐字节数的 log2。
	static inline size_t _Index(size_t bytes, size_t AlignIndex)//��2��ָ������
{
		//        -1Ҳ��Ϊ����8�����λ���ֲ���    ֻʣ���λ     
		return ((bytes + ((size_t)1 << AlignIndex) - 1) >> AlignIndex) - 1;
	}

	//ӳ���ϣͰ���±�
	// 按区间选择对齐位移，返回值贯穿三级缓存，作为桶/自由链表下标。
	static inline size_t Index(size_t bytes){
		//�Ż���
		if (bytes <= 128){
			return _Index(bytes, 3);
		}
		else if (bytes <= 1024){
			return _Index(bytes, 4);
		}
		else if (bytes <= 8 * 1024){
			return _Index(bytes, 7);
		}
		else if (bytes <= 64 * 1024){
			return _Index(bytes, 10);
		}
		else if (bytes <= 256 * 1024){
			return _Index(bytes, 13);
		}
		else{
			assert(false);
			return -1;
		}
	}

	// ������CentralCache��ȡ���ٸ�
	// 每次向 CentralCache 领取的块数：256KB/size，并夹在 [2,512] 之间。
	static size_t NumMoveSize(size_t size){
		assert(size <= MaxBytes);
		size_t num = MaxBytes / size;
		//�����
		if (num < 2){
			num = 2;
		}
		//С����
		if (num > 512){
			num = 512;
		}
		return num;
	}

	// ����һ����ϵͳ��ȡ����ҳ(1ҳ8KB)
	// 批量块总字节数右移得到页数（向下取整），至少分配 1 页。
	static size_t NumMovePage(size_t size){
		//����size����Ҫ������ٸ�size��С���ڴ��
		size_t num = NumMoveSize(size);
		//���ݸ��������������ֽ�����Ȼ���ټ���������ҳ�� 
		size_t npage = num * size;
		npage >>= PageShift;

		if (npage == 0)
			npage = 1;
		return npage;
	}
};

//�ڴ��Ļ�����λ������һ��ҳ�������ڴ�
// span：一组连续页的元数据，同时用作 CentralCache/PageCache 链表节点。
struct span{
	// 起始页号（页号 = 地址 >> PageShift），不是地址本身。
	Page_t Page_Adder = 0;//�������ڴ����ʼҳ��(��ַ)
	size_t _n = 0;//ҳ������
	//ǰ��span��ָ��
	span* _prev=nullptr;
	span* _next=nullptr;

	// 已借给 ThreadCache 且尚未归还的块数；归零表示该 span 可整体回收。
	size_t _UseCount = 0;//ThreadCache���˶��ٸ�С���ڴ�
	void* _FreeList = nullptr;//span���ŵ�һ��һ���ڴ����׵�ַ(ҳ�Ŀ�������)

	// true 表示 span 正被 CentralCache 使用；PageCache 合并邻居时会跳过这种 span。
	bool _IsUse = false;//��PageCache�����Ƿ��CentralCache��(span�Ƿ����ڱ�ʹ��)
	//�����ڴ�����
	size_t _FreeKnowSize = 0;
};

//��span��֯����(һ��ѭ������,����_head����ڵ�)
// span 环形双向链表：_head 为哨兵节点，空表时指向自身；每个桶自带一把互斥锁。
class SpanList{
public:
	SpanList(){//����span��������������span�ڵ�
		//_head = new span;
		_head = _SpanPool.New();
		_head->_next = _head;
		_head->_prev = _head;
	}

	//ͷ����span
	void InsertFront(span* NewPos){
		Insert(begin(), NewPos);//��ͷ��һ��spanǰ����newspan
	}
	//posǰ������span
	void Insert(span* pos, span* NewPos){
		assert(NewPos != nullptr);
		// prev NewPos pos
		span* prev = pos->_prev;

		prev->_next = NewPos;
		NewPos->_prev = prev;
		NewPos->_next = pos;
		pos->_prev = NewPos;
	}

	//ͷɾ��span
	span* EraseFront(){
		//��¼ͷspan
		span* EraseSpan = _head->_next;
		//ɾ��ͷspan��span������
		Erase(EraseSpan);
		//����ͷspan
		return EraseSpan;
	}
	//��span������ɾ��span
	void Erase(span* pos){
		assert(pos != nullptr);
		assert(pos != _head);
		//��¼prev,next,Ȼ���span������ɾ����span
		span* prev = pos->_prev;
		span* next = pos->_next;

		prev->_next = next;
		next->_prev = prev;
	}
	//���ػ�������Ͱ����
	std::mutex& Mutex(){
		return _mtx;
	}
	//���������е�ͷspan
	span* begin(){
		return _head->_next;
	}
	//���������е�βspan(��Ч�ڵ�,Խ��λ��)
	span* end(){
		return _head;
	}
	//�ж�span�����Ƿ�Ϊnullptr(��Чspan����Ϊ0)
	bool Empty(){
		return _head->_next == _head;
	}

private:
	span* _head;
	std::mutex _mtx;//Ͱ�� ÿ��thread����ͬһͰ(ͬһ��Ͱ��¼����һ��spanlist)��Ҫ����

	//span����(�ظ��ˣ�û���õ�)
	// 所有 SpanList 共用的哨兵对象池；静态成员只有这一份，定义在 CentralCache.cpp。
	static ObjectPool<span> _SpanPool;//staticʹ��ʱ�ȶ���
};