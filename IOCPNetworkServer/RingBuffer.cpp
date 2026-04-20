#include "Ringbuffer.h"
#include <cstring>

RingBuffer::RingBuffer(void)
	: m_iFront(0), m_iRear(0)
{
}

int RingBuffer::GetBufferSize(void) {
	return BUFFERSIZE;
}

int RingBuffer::GetUseSize(void) {
	if (m_iRear >= m_iFront) {
		return m_iRear - m_iFront;
	}
	else
	{
		return (BUFFERSIZE - m_iFront) + m_iRear;
	}
}

int RingBuffer::GetFreeSize(void) {
	int useSize = GetUseSize();
	return (BUFFERSIZE - 1) - useSize;
}

int RingBuffer::Enqueue(const char* chpData, int iSize) {
	int freeSize = GetFreeSize(); 

	if (iSize > freeSize) { 
		iSize = freeSize; 
	}
	if (iSize <= 0) return 0;

	int linearFreeSize = BUFFERSIZE - m_iRear; 

	if (iSize <= linearFreeSize) { 
		memcpy(m_buffer.data() + m_iRear, chpData, iSize); 
	}
	else { 
		memcpy(m_buffer.data() + m_iRear, chpData, linearFreeSize); 
		memcpy(m_buffer.data(), chpData + linearFreeSize, iSize - linearFreeSize); 
	}

	m_iRear.store((m_iRear + iSize) % BUFFERSIZE,std::memory_order_release);
	return iSize; 
}

int RingBuffer::Dequeue(char* chpDest, int iSize) {

	int useSize = GetUseSize();

	if (iSize > useSize) {
		iSize = useSize;
	}

	if (iSize <= 0)return 0;

	int linearuseSize = BUFFERSIZE - m_iFront;  

	if (iSize <= linearuseSize) {
		memcpy(chpDest, m_buffer.data() + m_iFront, iSize);
	}
	else {
		memcpy(chpDest, m_buffer.data() + m_iFront, linearuseSize);
		memcpy(chpDest + linearuseSize, m_buffer.data(), iSize - linearuseSize);
	}

	m_iFront.store((m_iFront + iSize) % BUFFERSIZE, std::memory_order_release);

	return iSize;

}

int RingBuffer::Peek(char* chpDest, int iSize)
{
	int useSize = GetUseSize();

	if (iSize > useSize)
	{
		iSize = useSize;
	}

	if (iSize <= 0) return 0;

	int linearUseSize = BUFFERSIZE - m_iFront;

	if (iSize <= linearUseSize)
	{
		memcpy(chpDest, m_buffer.data() + m_iFront, iSize);
	}
	else
	{
		memcpy(chpDest, m_buffer.data() + m_iFront, linearUseSize);
		memcpy(chpDest + linearUseSize, m_buffer.data(), iSize - linearUseSize);
	}

	return iSize;
}

void RingBuffer::ClearBuffer(void)
{
	m_iFront = 0;
	m_iRear = 0;
}

int RingBuffer::DirectEnqueueSize(void)
{

	if (m_iRear >= m_iFront)
	{
		int size = BUFFERSIZE - m_iRear;
		if (m_iFront == 0)
		{
			return size - 1;
		}
		return size;
	}
	else
	{
		return (m_iFront - m_iRear) - 1;
	}
}

int RingBuffer::DirectDequeueSize(void)
{

	if (m_iRear >= m_iFront)
	{
		return m_iRear - m_iFront;
	}

	else
	{
		return BUFFERSIZE - m_iFront;
	}
}

int RingBuffer::MoveRear(int iSize)
{
	int newRear = ((m_iRear.load(std::memory_order_relaxed) + iSize) % BUFFERSIZE);
	m_iRear.store(newRear, std::memory_order_release);
	return iSize;
}

int RingBuffer::MoveFront(int iSize)
{
	int newFront = ((m_iFront.load(std::memory_order_relaxed) + iSize) % BUFFERSIZE);
	m_iFront.store(newFront, std::memory_order_release);
	return iSize;
}

char* RingBuffer::GetFrontBufferPtr(void)
{
	return m_buffer.data() + m_iFront;
}

char* RingBuffer::GetRearBufferPtr(void)
{
	return m_buffer.data() + m_iRear;
}

char* RingBuffer::GetBufferStartPtr(void)
{
	return m_buffer.data();
}
