#pragma once
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <stop_token>

class JobQueue;

class GlobalQueue
{
public:
    void Push(std::shared_ptr<JobQueue> jobQueue);
    std::shared_ptr<JobQueue> Pop(std::stop_token stopToken);

private:
    std::mutex                            _mutex;
    std::condition_variable               _cv;
    std::queue<std::shared_ptr<JobQueue>> _queue;
};

