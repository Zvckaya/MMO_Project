#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "IOCPServer.h"
#include "Player.h"
#include "GlobalQueue.h"
#include "JobQueue.h"
#include "PacketProc.h"

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
    enum class FrameTaskType { clientJoin, clientLeave, playerMove };

    struct FrameTask
    {
        FrameTaskType type;
        SessionID     sessionID;
        int32_t       x = 0;
        int32_t       y = 0;
    };

    void BroadcastAll(Packet* packet);
    void BroadcastExcept(SessionID excludeID, Packet* packet);

    void CreateImmediateThread(std::stop_token stopToken);
    void CreateFrameThread(std::stop_token stopToken);
    void EnqueueFrameTask(FrameTask task);
    void ProcessFrameTask(const FrameTask& task);
    void Update(int64_t deltaMs);
    std::mutex            _frameTaskMutex;
    std::queue<FrameTask> _frameTaskQueue;
    std::jthread          _frameThread;
    std::vector<std::jthread> _immediateThreads;
    GlobalQueue               _globalQueue;
    std::shared_mutex                                          _jobQueuesMutex;
    std::unordered_map<SessionID, std::shared_ptr<JobQueue>>  _sessionJobQueues;
    PacketProc _packetProc;
    std::unordered_map<SessionID, std::unique_ptr<Player>> _players;

    std::atomic<bool> _isGameRunning = false;
};

