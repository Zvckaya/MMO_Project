#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

class Job;
class GlobalQueue;

struct PendingMove
{
    float             curX  = 0.f, curY  = 0.f;
    float             destX = 0.f, destY = 0.f;
    std::atomic<bool> dirty{ false };
};

struct PendingStop
{
    float             curX = 0.f, curY = 0.f;
    std::atomic<bool> dirty{ false };
};

class JobQueue : public std::enable_shared_from_this<JobQueue>
{
public:
    explicit JobQueue(GlobalQueue& globalQueue) : _globalQueue(globalQueue) {}
    void Post(std::function<void()> callback);
    void Execute(uint64_t endTick);

    PendingMove       pendingMove;
    PendingStop       pendingStop;

private:
    void Push(std::shared_ptr<Job> job);

    GlobalQueue& _globalQueue;

    std::mutex                          _jobMutex;
    std::queue<std::shared_ptr<Job>>    _jobs; //잡큐 
    std::atomic<int>                    _jobCount = 0; //jobCount를 이용해 글로벌 큐에 등록함 
};

