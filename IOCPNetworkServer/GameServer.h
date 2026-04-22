#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "IOCPServer.h"
#include "GameMap.h"
#include "GlobalQueue.h"
#include "JobQueue.h"
#include "PacketTypes.h"

class GameServer : public IOCPServer
{
public:
    GameServer() = default;
    ~GameServer() override;

    bool Start(std::optional<std::string_view> openIp, uint16_t port,
               int workerThreadCount, int runningThreadCount,
               bool nagleOption, int maxSessionCount);

    void Stop();

    bool OnConnectionRequest(std::string_view ip, uint16_t port) override;
    void OnClientJoin(SessionID sessionID) override;
    void OnClientLeave(SessionID sessionID) override;
    void OnRecv(SessionID sessionID, Packet* packet) override;

private:
    struct SessionAuthState
    {
        uint64_t    accountId   = 0;
        std::string displayName;
    };

    enum class FrameTaskType { clientJoin, clientLeave, playerMove, playerAttack, playerSkill, playerAuth };

    struct FrameTask
    {
        FrameTaskType type;
        SessionID     sessionID;
        SessionID     targetID    = 0;
        uint64_t      accountId   = 0;
        std::string   displayName;
        float         curX  = 0.f, curY  = 0.f;
        float         destX = 0.f, destY = 0.f;
        uint16_t      skillID = 0;
        MapID         mapID  = DEFAULT_MAP_ID;
    };

    void BroadcastAll(MapID mapID, Packet* packet);
    void BroadcastExcept(MapID mapID, SessionID excludeID, Packet* packet);
    GameMap* FindPlayerMap(SessionID sessionID);

    void CreateImmediateThread(std::stop_token stopToken);
    void CreateFrameThread(std::stop_token stopToken);
    void EnqueueFrameTask(FrameTask task);
    void ProcessFrameTask(const FrameTask& task);
    void Update(int64_t deltaMs);

    struct AuthRequest
    {
        SessionID sessionID;
        char      ticket[32];
    };

    void HandlePacket(SessionID sessionID, Packet* packet);
    void OnEcho(SessionID sessionID, Packet* packet);
    void OnCS_LoginAuth(SessionID sessionID, Packet* packet);
    void OnCS_Move(SessionID sessionID, Packet* packet);
    void OnCS_Stop(SessionID sessionID, Packet* packet);
    void OnCS_Attack(SessionID sessionID, Packet* packet);
    void OnCS_Skill(SessionID sessionID, Packet* packet);
    void OnCS_ItemPickup(SessionID sessionID, Packet* packet);
    void OnCS_ItemDrop(SessionID sessionID, Packet* packet);

    void CreateAuthThread(std::stop_token stopToken);
    void ProcessAuthResult(SessionID sessionID, const struct AuthResult& auth);

    std::mutex                  _authQueueMutex;
    std::condition_variable     _authQueueCv;
    std::queue<AuthRequest>     _authPendingQueue;
    std::jthread                _authThread;

    std::mutex            _frameTaskMutex;
    std::queue<FrameTask> _frameTaskQueue;
    std::jthread          _frameThread;
    std::vector<std::jthread> _immediateThreads;
    GlobalQueue               _globalQueue;
    std::shared_mutex                                      _jobQueuesMutex;
    std::unordered_map<SessionID, std::shared_ptr<JobQueue>> _sessionJobQueues;
    std::shared_mutex                                      _authStatesMutex;
    std::unordered_map<SessionID, SessionAuthState>        _sessionAuthStates;
    std::unordered_map<MapID, std::unique_ptr<GameMap>>    _maps;
    std::unordered_map<SessionID, MapID>                   _sessionToMapID;

    std::atomic<bool> _isGameRunning = false;
};

