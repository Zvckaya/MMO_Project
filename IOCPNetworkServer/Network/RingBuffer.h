#pragma once
#include <algorithm>
#include <atomic>
#include <array>
#include "ServerConfig.h"

class RingBuffer
{
public:
	RingBuffer();
	~RingBuffer() = default;

	int GetBufferSize(void);
	int GetUseSize(void);
	int GetFreeSize(void);
	int Enqueue(const char* chpData, int size);
	int Dequeue(char* chpDest, int iSize);
	int Peek(char* chpDest, int iSize);
	void ClearBuffer(void);


	int DirectEnqueueSize(void);
	int DirectDequeueSize(void);
	int MoveRear(int iSize);
	int MoveFront(int iSize);
	char* GetFrontBufferPtr(void);
	char* GetRearBufferPtr(void);
	char* GetBufferStartPtr(void);


private:
	std::array<char, BUFFERSIZE> m_buffer{};
	std::atomic<int> m_iFront;
	std::atomic<int> m_iRear;

};
