// 页号到 span* 的映射（TCMalloc 风格 PageMap）：用多级数组代替哈希表，
// 查询只做位移和掩码，无哈希冲突；未建立映射的页返回 NULL。
// PageMap1 单层（32 位页号）、PageMap2 两层（root + leaf）、PageMap3 三层（64 位页号）。
#pragma once

#include"Common.h"


// 单层基数树
// BITS 为页号总位数；数组下标就是页号，范围 [0, 2^BITS)。
template <int BITS>
class TCMalloc_PageMap1 
{
private:
	//指针数组长度:页号最大值
	static const int LENGTH = 1 << BITS; //开 2^32 / 2^13 个位置
	// 一维页表：下标为页号，值为对应 span*（类型擦除为 void*；NULL 表示该页未映射）。
	void** array_; //指针数组(记录的是span*) 下标(页号)->span*

public:
	typedef uintptr_t Number;
	// 按字节数申请数组空间，再经 SizeClass::RoundUp 向上取整到 8KB 页后交给 SystemAlloc。
	TCMalloc_PageMap1()
	{
		//直接把空间开好
		size_t size = sizeof(void*) << BITS;
		size_t bytes = SizeClass::RoundUp(size);
		array_ = (void**)SystemAlloc(bytes>>PageShift);
		memset(array_, 0, sizeof(void*) << BITS);
	}

	//按照页号获取
	// 越界页号（k >> BITS != 0）视为未映射，避免数组越界。
	void* get(Number k) const
	{
		if ((k >> BITS) > 0)//k / 2^BITS 如果页号超过了页号总数，证明当前页号没有对应的内存
		{
			return NULL;
		}
		//返回页号对应的span*
		return array_[k];
	}

	//设置页号:span*的映射
	// 调用方须保证 k 在 [0, 2^BITS) 内，本函数不做越界检查。
	void set(Number k, void* v) 
	{
		array_[k] = v;
	}
};

// 双层基数树
// 两层页表：高 ROOT_BITS 位选 root 槽，低 BITS-ROOT_BITS 位选 leaf 内槽位。
template <int BITS>
class TCMalloc_PageMap2 {
private:
	//确定好页号的分级表示 root-leaf
	//完整的页号为 root页号+leaf页号
	// 页号按 bit 切分：前 5 位是 root 下标，其余位是 leaf 下标。
	static const int ROOT_BITS = 5;				   //通过页号的前5个bit表示位置
	static const int ROOT_LENGTH = 1 << ROOT_BITS; //第一层开多少个元素
	static const int LEAF_BITS = BITS - ROOT_BITS; //通过页号的前6~14个bit表示位置
	static const int LEAF_LENGTH = 1 << LEAF_BITS; //第二层开多少个元素 
	
	//叶子页表的类型定义
	struct Leaf {
		void* values[LEAF_LENGTH];
	};
	//root页表——每一个位置存储的是root页号对应的leaf页表
	// root 槽为空表示整段 leaf 尚未建立，get 可直接返回 NULL。
	Leaf* root_[ROOT_LENGTH]; 

public:
	typedef uintptr_t Number;

	explicit TCMalloc_PageMap2() 
	{
		memset(root_, 0, sizeof(root_));
		//直接开空间
		Ensure(0, 1 << BITS);
	}

	//为start页到后面n页 为其开辟存储其span*的页表
	// 按 leaf 粒度补齐 [start, start+n) 覆盖的 root 槽；key 每次跳到下一个 leaf 边界。
	bool Ensure(Number start, size_t n)
	{
		for (Number key = start; key <= start + n - 1;) 
		{
			const Number i1 = key >> LEAF_BITS;//根页表下标
			if (root_[i1] == NULL) {
				//若是对应的叶子页表不存在那么就从对象池里面new一个叶子页表出来
				static ObjectPool<Leaf> LeafPool;
				Leaf* leaf = LeafPool.New();
				memset(leaf, 0, sizeof(*leaf));
				root_[i1] = leaf;
			}
			//跳到下一个叶子页表的第一个叶子页的页号
			key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
		}
		return true;
	}

	//根据页号->span*
	// i1 为 root 下标，i2 为 leaf 内偏移；越界或 leaf 未建立均返回 NULL。
	void* get(Number k) const {
		const Number i1 = k >> LEAF_BITS;//取出根页表索引
		const Number i2 = k & (LEAF_LENGTH - 1);//取出叶子页表索引
		//如果当前页号根本不存在||当前根页表的叶子页表没构建
		//证明当前页号->span*的映射根本存储
		if ((k >> BITS) > 0 || root_[i1] == NULL) {
			return NULL;
		}
		//返回对应 根页表索引i1->子页表索引i2->span*
		return root_[i1]->values[i2];
	}

	//设置页号->span*
	// 构造时 Ensure 已覆盖全部页号，因此 set 可直接写；assert 仅防越界。
	void set(Number k, void* v) {
		//获取根页表索引和叶子页表索引
		const Number i1 = k >> LEAF_BITS;
		const Number i2 = k & (LEAF_LENGTH - 1);
		assert(i1 < ROOT_LENGTH);//检查是否超过了根页表下标最大值
		//记录下span*到对应的i1\i2位置
		root_[i1]->values[i2] = v;
	}
};

// 处理64位情况
// 三层页表（64 位页号）：两段内部节点 + 一段叶节点，位数近似三等分。
template <int BITS>
class TCMalloc_PageMap3 {
private:
	//计算前两层和第三层的页表的所占用的bit位数和页号长度
	// 中间两层各占约 BITS/3 位，叶层占剩余位；每层数组长度都是 2 的幂。
	static const int INTERIOR_BITS = (BITS + 2) / 3;         // 第一层和第二层的页号的比特位个数
	static const int INTERIOR_LENGTH = 1 << INTERIOR_BITS;   // 第一层和第二层开多少元素
	static const int LEAF_BITS = BITS - 2 * INTERIOR_BITS;	 // 第三层的页号的比特位个数
	static const int LEAF_LENGTH = 1 << LEAF_BITS;			 // 第三层开多少元素
	
	//第一,二层结构（一个统一的页表结构：每一个位置指向下一个页表）
	// 内部节点槽位统一存 Node*；叶节点指针也放在这里，读取时再 reinterpret_cast 成 Leaf*。
	struct Node {
		Node* ptrs[INTERIOR_LENGTH];
	};
	//第三层结构(叶子页表结构:每一个位置存储span*)
	struct Leaf {
		void* values[LEAF_LENGTH];
	};
	//指针实现
	//三级页表（就是比双层基数树多了一层,唯一的问题就是到叶子页表时候需要转换成leaf类型）
	// 根节点常驻：root_->ptrs 指向第二层，第二层 ptrs 再指向 Leaf。
	Node* root_;

	// 节点来自静态 ObjectPool，创建后清零；返回 NULL 表示池内存不足。
	Node* NewNode() {
		static ObjectPool<Node> ObjectNode;
		Node* result = ObjectNode.New();

		if (result != NULL) {
			memset(result, 0, sizeof(*result));
		}
		return result;
	}
public:
	typedef uintptr_t Number;
	//初始化先开辟第一层空间
	// 只先建根节点，后续两层在 Ensure/set 时按需建立，避免为未映射区间预留大数组。
	explicit TCMalloc_PageMap3() {
		root_ = NewNode();
	}

	// i1/i2/i3 分别为三段页号；任一中间节点为空即表示未映射。
	void* get(Number k) const {
		const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS); //第一层下标
		const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1); //第二层小标
		const Number i3 = k & (LEAF_LENGTH - 1); //第三层下标
		//页号超出范围，或映射该页号的空间未开辟
		if ((k >> BITS) > 0 || root_->ptrs[i1] == NULL || root_->ptrs[i1]->ptrs[i2] == NULL){
			return NULL;
		}
		return reinterpret_cast<Leaf*>(root_->ptrs[i1]->ptrs[i2])->values[i3];
	}

	// set 先 Ensure 打通 i1/i2 路径，再写叶槽；调用方保证页号不越界。
	void set(Number k, void* v) {
		assert(k >> BITS == 0);
		const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
		const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
		const Number i3 = k & (LEAF_LENGTH - 1);
		//没空间去开辟一组映射出来
		Ensure(k, 1);
		reinterpret_cast<Leaf*>(root_->ptrs[i1]->ptrs[i2])->values[i3] = v;
	}

	// 建立 [start, start+n) 涉及的 i1/i2 节点；key 每次跳到下一个叶块边界。
	bool Ensure(Number start, size_t n) {
		for (Number key = start; key <= start + n - 1;) {
			const Number i1 = key >> (LEAF_BITS + INTERIOR_BITS);
			const Number i2 = (key >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
			if (i1 >= INTERIOR_LENGTH || i2 >= INTERIOR_LENGTH){
				return false;
			}

			if (root_->ptrs[i1] == NULL) {
				Node* n = NewNode();
				if (n == NULL) return false;
				root_->ptrs[i1] = n;
			}

			if (root_->ptrs[i1]->ptrs[i2] == NULL) {
				static ObjectPool<Leaf> ObjectLeaf;
				Leaf* leaf = ObjectLeaf.New();

				memset(leaf, 0, sizeof(*leaf));
				root_->ptrs[i1]->ptrs[i2] = reinterpret_cast<Node*>(leaf);
			}
			key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
		}
		return true;
	}
};