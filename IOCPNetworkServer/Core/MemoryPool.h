#pragma once
#include "ServerConfig.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

template<class DATA>
class MemoryPool
{
public:
	static constexpr uint32_t CODE_ALLOC = 0x99999999u;
	static constexpr uint32_t CODE_FREE  = 0xDEADDEADu;

	struct Node
	{
		MemoryPool*                owner;		//노드를 할당한 메모리 풀 주소
		uint32_t                   checkCode;	//현재 노드 상태
		Node*                      next;		//다음 노드

		bool isConstructed;

		//템플릿으로 넘겨받은 객체를 직접 선언하지 않고, 그 크기만큼의 메모리 공간만 잡아놈
		// 처음에는 DATA를 놨으나.그러면 생성자 호출을 무조건 해야함.
		//alignas이용
		alignas(DATA) unsigned char data[sizeof(DATA)];
	};

	MemoryPool(int blockNum, bool placementNew = false) : _placementNew(placementNew)
	{
		for (int i = 0; i < blockNum; ++i)
		{
			// 여기서 new를 해도 Node 안의 data는 단순 배열이므로
			// DATA의 생성자 호출을 막을 수 있다.
			Node* node          = new Node;

			//할당자
			node->owner         = this;
			node->checkCode     = CODE_FREE;	//반환됨 형태
			node->isConstructed = false;

			//LIFO 방식으로 연결(스택 형태)
			pushNode(node);

			_capacity.fetch_add(1, std::memory_order_relaxed);
		}
	}

	~MemoryPool()
	{
		Node* node = unpackPtr(_head.load(std::memory_order_relaxed));
		while (node != nullptr)
		{
			Node* next = node->next;

			if (node->isConstructed) //객체가 생성된적이 있었으면 소멸자 호출
				reinterpret_cast<DATA*>(node->data)->~DATA();

			delete node;
			node = next;
		}
	}

	MemoryPool(const MemoryPool&)            = delete;
	MemoryPool& operator=(const MemoryPool&) = delete;

	//가변 인자 템플릿 적용
	//몇개가 들어올지는  모르지만 일단 다 받겠다~...
	//아래의 의미는 타입이 여러개 오는 데,그걸 Args로 부르겠다 라는 의미.
	template<typename... Args>
	DATA* Alloc(Args&&... args) //타입에 맞는 변수도 여러개 오는데, 퉁처서 args라고 부르겠다
	{
		TlsCache& tls = _tls;

		if (tls.owner != this)
		{
			tls.owner = this;
			tls.head  = nullptr;
			tls.count = 0;
		}

		if (tls.head == nullptr)
		{
			for (int i = 0; i < TLS_CACHE_BATCH; ++i)
			{
				Node* node = popNode();
				if (node == nullptr)
				{
					node                = new Node;
					node->owner         = this;
					node->checkCode     = CODE_FREE;
					node->isConstructed = false;
					_capacity.fetch_add(1, std::memory_order_relaxed);
				}
				node->next = tls.head;
				tls.head   = node;
				++tls.count;
			}
		}

		Node* node = tls.head;
		tls.head   = node->next;
		--tls.count;

		node->checkCode = CODE_ALLOC;
		_useCount.fetch_add(1, std::memory_order_relaxed);

		//배열의 시작 주소를 DATA*로 캐스팅
		DATA* ret = reinterpret_cast<DATA*>(node->data);

		if (node->isConstructed == false)
		{
			new (ret) DATA(std::forward<Args>(args)...);

			// [플래그 ON] 이제 얘는 건물 지어진 땅임
			node->isConstructed = true;

			return ret;
		}

		if (_placementNew)
		{
			//new (Ret) <- placementNew 문법, 새롭게 메모리 할당 ㄴ
			//DATA(생성자...)
			//forword를 사용해서 넘어온 형태 그대로 넘긴다.
			new (ret) DATA(std::forward<Args>(args)...);
		}

		return ret;
	}

	bool Free(DATA* data)
	{
		if (data == nullptr)
			return false;

		//offsetof로 data의 위치를 구한후, pData의 주소에서 빼주어 node의 위치를 구함
		Node* node = reinterpret_cast<Node*>(
			reinterpret_cast<uintptr_t>(data) - offsetof(Node, data));
		//그 후 그 주소위치를 node로 캐스팅

		//내풀에서 만든 노드인가?
		if (node->owner != this)
			std::abort();

		//이미 해제된 노드인가?
		if (node->checkCode == CODE_FREE)
			std::abort();

		//정상 해제 절차

		if (_placementNew)
			data->~DATA();

		node->checkCode = CODE_FREE;
		_useCount.fetch_sub(1, std::memory_order_relaxed);

		TlsCache& tls = _tls;

		if (tls.owner == nullptr)
			tls.owner = this;

		if (tls.owner != this)
		{
			pushNode(node);
			return true;
		}

		node->next = tls.head;
		tls.head   = node;
		++tls.count;

		if (tls.count >= TLS_CACHE_BATCH * 2)
		{
			for (int i = 0; i < TLS_CACHE_BATCH; ++i)
			{
				Node* n  = tls.head;
				tls.head = n->next;
				--tls.count;
				pushNode(n);
			}
		}

		return true;
	}

	int GetCapacityCount() const { return _capacity.load(std::memory_order_relaxed); }
	int GetUseCount()      const { return _useCount.load(std::memory_order_relaxed); }

private:
	// x64 가상 주소는 하위 48비트만 사용 → 상위 16비트를 ABA 방지 태그로 활용
	static constexpr uintptr_t PTR_MASK = (1ULL << 48) - 1;

	static uint64_t pack(Node* ptr, uint16_t tag) noexcept
	{
		return (static_cast<uint64_t>(tag) << 48)
			 | (reinterpret_cast<uintptr_t>(ptr) & PTR_MASK);
	}

	static Node* unpackPtr(uint64_t val) noexcept
	{
		return reinterpret_cast<Node*>(val & PTR_MASK);
	}

	static uint16_t unpackTag(uint64_t val) noexcept
	{
		return static_cast<uint16_t>(val >> 48);
	}

	void pushNode(Node* node) noexcept
	{
		uint64_t oldHead = _head.load(std::memory_order_relaxed);
		uint64_t newHead;
		do {
			node->next = unpackPtr(oldHead);
			newHead    = pack(node, unpackTag(oldHead) + 1);
		} while (!_head.compare_exchange_weak(
			oldHead, newHead,
			std::memory_order_release,
			std::memory_order_relaxed));
	}

	Node* popNode() noexcept
	{
		uint64_t oldHead = _head.load(std::memory_order_acquire);
		uint64_t newHead;
		Node*    node;
		do {
			node = unpackPtr(oldHead);
			if (node == nullptr)
				return nullptr;
			newHead = pack(node->next, unpackTag(oldHead) + 1);
		} while (!_head.compare_exchange_weak(
			oldHead, newHead,
			std::memory_order_acquire,
			std::memory_order_acquire));
		return node;
	}

	struct TlsCache
	{
		MemoryPool* owner = nullptr;
		Node*       head  = nullptr;
		int         count = 0;

		~TlsCache()
		{
			Node* node = head;
			while (node != nullptr)
			{
				Node* next = node->next;
				owner->pushNode(node);
				node = next;
			}
		}
	};
	static thread_local TlsCache _tls;

	alignas(64) std::atomic<uint64_t> _head = 0;
	std::atomic<int> _capacity  = 0;
	std::atomic<int> _useCount  = 0;
	bool             _placementNew;
};

template<class DATA>
thread_local typename MemoryPool<DATA>::TlsCache MemoryPool<DATA>::_tls;
