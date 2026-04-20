#include "Session.h"
#include <utility>

void Session::RecvPost()
{
	_recvOverLapped = {}; //오버랩 io 초기화

	int freeSize = _recvBuffer.GetFreeSize();
	int directSize = _recvBuffer.DirectEnqueueSize();

	WSABUF bufs[2];
	DWORD  bufCount = 1;

	bufs[0].buf = _recvBuffer.GetRearBufferPtr();

	if (freeSize <= directSize)
	{
		bufs[0].len = static_cast<ULONG>(freeSize);
	}
	else //랩 어라운드 시 
	{
		bufs[0].len = static_cast<ULONG>(directSize);
		bufs[1].buf = _recvBuffer.GetBufferStartPtr();
		bufs[1].len = static_cast<ULONG>(freeSize - directSize);
		bufCount = 2;
	}

	_ioCount.fetch_add(1); //반드시 io 올리고 요청해야함 . 요청하고 ->io 올리면 이미 clear후 다른 세션일 수 있음 

	DWORD flags = 0;
	DWORD cbTransferred = 0;
	int rc = WSARecv(_sock, bufs, bufCount, &cbTransferred, &flags, &_recvOverLapped, nullptr);
	if (rc == SOCKET_ERROR)
	{
		int err = WSAGetLastError();
		if (err != WSA_IO_PENDING)
		{
			Disconnect();
		}
	}
}

void Session::SendPost()
{
	bool needDisconnect = false;
	{
		std::lock_guard<std::mutex> lock(_sendLock);

		bool expected = false;
		if (!_isSending.compare_exchange_strong(expected, true)) //send1회 제한, enqueue는 다른 함수로 있고, send완료시 재요청 되기 떄문에 
			return;

		int totalSize = _sendBuffer.GetUseSize();
		if (totalSize == 0)
		{
			_isSending = false;
			return;
		}

		int directSize = _sendBuffer.DirectDequeueSize();

		_sendOverLapped = {};

		WSABUF bufs[2];
		DWORD  bufCount = 1;

		bufs[0].buf = _sendBuffer.GetFrontBufferPtr(); 

		if (totalSize <= directSize)
		{
			bufs[0].len = static_cast<ULONG>(totalSize);
		}
		else //랩 어라운드시 
		{
			bufs[0].len = static_cast<ULONG>(directSize);
			bufs[1].buf = _sendBuffer.GetBufferStartPtr();
			bufs[1].len = static_cast<ULONG>(totalSize - directSize);
			bufCount = 2;
		}

		_ioCount.fetch_add(1); //반드시 io카운트를 올리고 io 요처을 해야한다 댕글링 문제는 없지만 재사용된 세션에 대해 접근할 수 있음 

		DWORD flags = 0;
		DWORD cbTransferred = 0;
		int rc = WSASend(_sock, bufs, bufCount, &cbTransferred, flags, &_sendOverLapped, nullptr);
		if (rc == SOCKET_ERROR)
		{
			int err = WSAGetLastError();
			if (err != WSA_IO_PENDING)
				needDisconnect = true;
		}
	}

	if (needDisconnect)
		Disconnect();
}

bool Session::TryEnqueueSend(SessionID ownerSessionID, const char* data, int size) //세션에 대해 enqueue 할 수 있는지 판별 
{
	std::lock_guard<std::mutex> lock(_sendLock);

	if (GetSessionID() != ownerSessionID || _isDisconnected.load() || _sock == INVALID_SOCKET)
	{
		return false;
	}
	if (_sendBuffer.GetFreeSize() < size)
	{
		return false;
	}

	if (_sendBuffer.Enqueue(data, size) != size)
	{
		return false;
	}

	return true;
}

bool Session::CompleteSend(int transferredBytes)
{
	std::lock_guard<std::mutex> lock(_sendLock);

	_sendBuffer.MoveFront(transferredBytes); //원자적으로 front 전진 
	_isSending = false;//전송 플래그 해제 
	return _sendBuffer.GetUseSize() > 0; //getusesize와 movefront 분리시 datarace가 생긴다  
}


void Session::Clear()
{

	SessionID releasedSessionID = _playerId.load(std::memory_order_acquire); //현재 그 세션의 진짜 플레이어 id 가져오기 

	_playerId.store(0, std::memory_order_release);

	_recvBuffer.ClearBuffer();
	{
		std::lock_guard<std::mutex> lock(_sendLock);
		_sendBuffer.ClearBuffer();
		_isSending = false;
	}
	_isDisconnected = false;

	//iocp쪽 세션 핸들러 실행
	if (releasedSessionID != 0) 
		_eventHandler.onSessionReleased(releasedSessionID);
	_eventHandler.onSessionCleared();
	_eventHandler.onReturnIndex(); //슬롯반환은 맨 마지막에 이루어져야한다. 
}

void Session::ReleaseRef()
{
	if (_ioCount.fetch_sub(1) == 1)
	{
		Clear();
	}
}

void Session::Disconnect()
{
	bool expected = false;
	//첫 disconnect 호출자만 진입 
	if (_isDisconnected.compare_exchange_strong(expected, true))
	{
		SOCKET sockToClose = INVALID_SOCKET;
		{
			std::lock_guard<std::mutex> lock(_sendLock); //tryenqueuesend에서 소켓 상태를 보고 send하는데 그부분이 atomic하지 않음. 따라서 socket을 atomic하게 바꿔줘야함 
			closesocket(_sock);
			_sock = INVALID_SOCKET; 
		}

		ReleaseRef(); //소유권 io 감소 
	}
	ReleaseRef(); //이 io에 대한 감소
}


