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
#include <unordered_set>
#include <vector>

#include "IOCPServer.h"
#include "DBClient.h"
#include "GameMap.h"
#include "GridMap.h"
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

    enum class FrameTaskType { clientJoin, clientLeave, playerMove, playerAttack, playerSkill, playerAuth, playerMapChange,
                               itemSlotSwap, itemDrop, itemPickup };

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
        MapID         mapID       = DEFAULT_MAP_ID;
        MapID         targetMapID = DEFAULT_MAP_ID;
        PlayerDBData  dbData;
        uint16_t      fromSlot = 0;
        uint16_t      toSlot   = 0;
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
    void OnCS_ItemMove(SessionID sessionID, Packet* packet);
    void OnCS_MapChangeReq(SessionID sessionID, Packet* packet);

    void CreateAuthThread(std::stop_token stopToken);
    void ProcessAuthResult(SessionID sessionID, const struct AuthResult& auth);

    struct DBRequest {
        enum class Type { loadPlayer, savePlayer, saveInventory };
        Type         type;
        uint64_t     accountId = 0;
        PlayerDBData saveData;
        std::vector<InventorySlotData> inventoryData;
        std::function<void(const PlayerDBData&)> onLoad;
    };

    void EnqueueDBRequest(DBRequest req);
    void CreateDBThread(std::stop_token stopToken);

    std::mutex                  _authQueueMutex;
    std::condition_variable     _authQueueCv;
    std::queue<AuthRequest>     _authPendingQueue;
    std::jthread                _authThread;

    std::mutex                  _dbQueueMutex;
    std::condition_variable     _dbQueueCv;
    std::queue<DBRequest>       _dbPendingQueue;
    std::jthread                _dbThread;

    std::mutex            _frameTaskMutex;
    std::queue<FrameTask> _frameTaskQueue;
    std::jthread          _frameThread;
    std::vector<std::jthread> _immediateThreads;
    GlobalQueue               _globalQueue;
    std::shared_mutex                                      _jobQueuesMutex;
    std::unordered_map<SessionID, std::shared_ptr<JobQueue>> _sessionJobQueues;
    std::mutex                                             _unauthMutex;
    std::unordered_set<SessionID>                          _unauthSessions;
    std::shared_mutex                                      _authStatesMutex;
    std::unordered_map<SessionID, SessionAuthState>        _sessionAuthStates;
    std::unordered_map<MapID, std::unique_ptr<GameMap>>         _maps;
    std::unordered_map<SessionID, MapID>                        _sessionToMapID;
    std::unordered_map<uint16_t, ItemData, std::hash<uint16_t>> _itemDataMap;

    std::atomic<bool> _isGameRunning = false;

    GridMap _gridMap;
};

