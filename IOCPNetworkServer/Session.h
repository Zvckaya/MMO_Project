#pragma once

#include "ServerConfig.h"
#include "RingBuffer.h"
#include <functional>
#include <mutex>
#include <winsock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#pragma comment(lib, "Ws2_32.lib")

using SessionID = uint64_t;

struct SessionEventHandler //세션이 iocpserver * 를 들고 있으면 의존이 생김. 핸들러 방식을 사용하여 레이어 분리
{
	std::function<void()>          onReturnIndex;
	std::function<void(SessionID)> onSessionReleased;
	std::function<void()>          onSessionCleared;
};

class Session
{
public:
	Session() = default;
	~Session() = default;

	void      SetSocket(SOCKET sock)     { _sock = sock; }
	void      SetSessionID(SessionID id) { _playerId.store(id, std::memory_order_release); }
	SOCKET    GetSocket()    const       { return _sock; }
	SessionID GetSessionID() const       { return _playerId.load(std::memory_order_acquire); }

	void SetEventHandler(SessionEventHandler handler) { _eventHandler = std::move(handler); }

	void AddContentRef()    { _ioCount.fetch_add(1); }
	void AcquireOwnerRef()  { _ioCount.fetch_add(1); }
	void ReleaseOwnerRef()  { _ioCount.fetch_sub(1); }
	bool HasActiveIO() const { return _ioCount.load() != 0; }
	bool IsConnected() const { return !_isDisconnected.load(); }

	bool IsRecvOverlapped(OVERLAPPED* ol) const { return &_recvOverLapped == ol; }
	bool IsSendOverlapped(OVERLAPPED* ol) const { return &_sendOverLapped == ol; }

	RingBuffer& GetRecvBuffer() { return _recvBuffer; }

	void ReleaseRef();
	void Disconnect();

	void RecvPost();
	void SendPost();
	bool TryEnqueueSend(SessionID ownerSessionID, const char* data, int size);
	bool CompleteSend(int transferredBytes);

private:
	void Clear();

	SessionEventHandler _eventHandler;

	OVERLAPPED _recvOverLapped = {};
	OVERLAPPED _sendOverLapped = {};

	RingBuffer _recvBuffer;
	RingBuffer _sendBuffer;

	std::atomic<bool> _isSending      = false;
	std::atomic<int>  _ioCount        = 0;
	std::atomic<bool> _isDisconnected = false;
	SOCKET            _sock           = INVALID_SOCKET;

	std::atomic<SessionID> _playerId = 0;
	std::mutex _sendLock;
};
