// ThreadCache 实现：线程本地快速路径，以及与 CentralCache 的批量换入换出。
#include"ThreadCache.h"
#include"CentralCache.h"
#include<algorithm>

//��������ڴ�
//����:�����ڴ���ֽڴ�С
// �������ڴ��ֽ������벢��ת���ɶ�Ӧ��Ͱ�������ҿ����ڴ棺
// 1�����ҵ��ڴ棬����Ͱ�ڵ�һ�������ڴ��
// 2��δ���ҵ���ȥ CentralCache��������
// return : �����ڴ��׵�ַ
void* ThreadCache::Allocate(size_t size)
{
	//assert(size <= MaxBytes);
	//������ ��Ͱ����
	// RoundUp 得到档位大小，Index 得到桶下标；二者必须基于同一个 size 成对使用。
	size_t align = SizeClass::RoundUp(size);
	size_t index = SizeClass::Index(size);

	if (_FreeLists[index].Empty()){
		//����Ϊ�����벻���ڴ棬ȥCentralCache�����ڴ沢����
		return FetchFromCentralCache(index, align);//Ҫ��������
	}
	else{
		//������Ϊ�գ����������׽ڵ��ַ�����Ƴ��׽ڵ�
		return _FreeLists[index].Pop();
	}
}

//��CentralCache��ȡ�ڴ�
// ������Ͱ����,������ڴ���С
//����:ThreadCache û�ڴ���->ȥ CentralCache ��������һ���ڴ�
// �Լ��� 1 ��  ʣ�µĹҵ�������������
//return : ��ȡ���ڴ��ַ
void* ThreadCache::FetchFromCentralCache(size_t index, size_t size)
{
	//������CentralCache����(��������޵ͣ�С�������޸� [2,512])
	//ͬʱ���ܳ��������������ڴ���������
	// 批量上限取“按 256KB 折算的块数”与“本地自适应阈值”的较小值。
	size_t batchNum = std::min(SizeClass::NumMoveSize(size), _FreeLists[index].MaxSize());
	//�����������ڴ�����==�������������ֵ����������Ҫ������������
	//����ʼ�㷨(��������ӵ��)
	//ֻҪ��������������ޣ�����+1.��ֹ�̻߳�������ռ�ÿ����ڴ����
	// 未触及 NumMoveSize 上限时阈值 +1，逐步摊薄 CentralCache 往返成本。
	if (_FreeLists[index].MaxSize() == batchNum){
		_FreeLists[index].MaxSize()++;
	}

	//��ʽ��CentralCache����(ʵ������)
	void* start = nullptr;//��������ͷ
	void* end = nullptr;//����������β
	//��CentralCache���Ļ�������batchNum��size��С�ڴ�飬����ʵ���������
	size_t ActualNum = CentralCache::GetInstance()->FetchRangeObj(start, end, batchNum, size);
	//�������뵽һ���ڴ�
	assert(ActualNum >= 1);
	//������ڴ������û�г����쳣
	assert(start != nullptr && end != nullptr);

	if (ActualNum == 1){
		assert(start == end);
		return start;
	}
	else{
		// start 留给调用者，Next(start) 到 end 的 ActualNum-1 块挂到本地桶。
		//���뵽��һ�����ڴ�ȡ��һ��������,
		//ʣ�µĹҵ�ThreadCache��Ӧ��_freeList��
		_FreeLists[index].PushRange(Next(start), end, ActualNum - 1);//�ǵü������Լ�Ҫ�õ�һ��
		return start;
	}
}

//�ͷ��ڴ�����̻߳����
//����:�ͷ��ڴ��׵�ַ���ͷ��ڴ��С
//return: 
void ThreadCache::Deallocate(void* ptr, size_t size)
{
	//���յ�_FreeList��Ӧindex��(ͷ��)
	//size_t align = SizeClass::RoundUp(size);
	// 释放不重新对齐：调用方传入的 size 与分配时一致，直接映射到同一桶。
	size_t index = SizeClass::Index(size);
	//��Ϊ֮ǰ�����ڴ��ʱ����Ƿ������֮����ڴ棬��˻��յ�ʱ��Ͳ���Ҫ����
	_FreeLists[index].Push(ptr);

	//_FreeLIsts[index]̫��Ҫ��CentralCache����
	if (_FreeLists[index].Size() >= _FreeLists[index].MaxSize())
	{
		//�����������׽ڵ���ո�CentralCache
		ListTooLong(_FreeLists[index], size);
	}
}
/// <summary>
/// ȡ������ �ڴ��size��С���ڴ��������Ȼ���ڴ���������յ�CentralCache���Ļ���
/// </summary>
/// <param name="list">��������</param>
/// <param name="size">�����ڴ���С</param>
/// return :
void ThreadCache::ListTooLong(FreeList& list, size_t size)
{
	//Ϊʲô��Ҫһ�����黹MaxSize()���ڴ��;
	//�������գ���������������ҪƵ�����ع黹��
	//	���ÿ��ֻ�� 1 �飺	�ͷ� �� �� 1 ��,�ͷ� �� �� 1 ��
	//	ÿ�ζ�Ҫ�� CentralCache,�������ܻ��ر��
	//	�������˼·�ǣ�
	//	��һ���ٻ�,һ�λ�һ���,���ٹ黹����,��������ͻ
	//	���������������ȫ�ԳƵ���ƣ�
	//	���룺һ����һ�����ٷ��� CentralCache
	//	�ͷţ���һ���ٻ����ٷ��� CentralCache

	void* start = nullptr;//����������ʼλ��
	void* end = nullptr;//������������λ��
	//list.PopRange(start, end, list.Size());
	//ɾ��maxsize��С�Ŀ�
	// 只摘 MaxSize 个而非全部：保留一部分本地块，避免刚归还又立刻补货。
	list.PopRange(start, end, list.MaxSize());
	//��Maxsize()��С���ڴ���������뵽���Ļ�������                 
	CentralCache::GetInstance()->ReleaseListToSpans(start, size);
}
