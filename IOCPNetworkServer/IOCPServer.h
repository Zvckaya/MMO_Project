#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stack>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#pragma comment(lib, "Ws2_32.lib")

#include "MemoryPool.h"
#include "Packet.h"
#include "ServerConfig.h"
#include "Session.h"

using SessionID = uint64_t;

class IOCPServer
{
public:
    IOCPServer()          = default;
    virtual ~IOCPServer();

    IOCPServer(const IOCPServer&)            = delete;
    IOCPServer& operator=(const IOCPServer&) = delete;

    bool Start(std::optional<std::string_view> openIp, uint16_t port,
               int workerThreadCount, int runningThreadCount,
               bool nagleOption, int maxSessionCount); //서버 ip,포트, 러닝스레드,컨쿼런트스레드 및 네이글, 세션개수를 지정가능 

    void Stop(); //현재 완료패킷 큐를 drain 하고 종료

    bool SendPacket(SessionID sessionID, Packet* packet); //특정 세션에 해당 packet send요청 
    bool Disconnect(SessionID sessionID); //특정 세션을 끊는다.

protected: //하위 컨텐츠 레이어에서 상속받아 사용할 가상함수
    virtual bool OnConnectionRequest(std::string_view ip, uint16_t port) = 0;
    virtual void OnClientJoin(SessionID sessionID)                        = 0;
    virtual void OnClientLeave(SessionID sessionID)                       = 0;
    virtual void OnRecv(SessionID sessionID, Packet* packet)              = 0;
    bool ReleaseContentRef(SessionID sessionID); //컨텐츠에서 해당 

    MemoryPool<Packet> _packetPool{ PACKET_POOL_SIZE };

private:
    void CreateWorkerThread();
    void CreateAcceptThread(std::stop_token stopToken);
    void WaitForSessionDrain();
    Session* FindSession(SessionID sessionID);

    HANDLE _hIOCP        = nullptr;
    SOCKET _listenSock = INVALID_SOCKET;

    bool offNagle = false;

    std::array<Session, MAXSESSIONSIZE> _sessions;
    std::atomic<uint64_t> _generationCnt = 1;

    std::mutex _emptyLock;
    std::stack<int> _emptySlot;
    std::mutex _drainMutex;
    std::condition_variable _drainCv;

    std::vector<std::jthread> _workerThreads;
    std::jthread              _acceptThread;
    std::atomic<bool>         _isRunning = false;

    std::atomic<int> disconnectError = 0;
};

