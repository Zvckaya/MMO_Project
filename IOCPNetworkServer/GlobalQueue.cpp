#include "GlobalQueue.h"
#include "JobQueue.h"

void GlobalQueue::Push(std::shared_ptr<JobQueue> jobQueue)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _queue.push(std::move(jobQueue));
    }
    _cv.notify_one(); //자는 스레드 하나 깨우기 (즉답 스레드가 될 것이다)
}

std::shared_ptr<JobQueue> GlobalQueue::Pop(std::stop_token stopToken)
{
    std::unique_lock<std::mutex> lock(_mutex); //wait 내부에서 unlock/lock 반복해야해서 unique_lock;

    std::stop_callback cb(stopToken, [this]() { _cv.notify_all(); }); //stop시 모든 스레드 꺠우기 

    _cv.wait(lock, [this, &stopToken]() //조건이 false면 mutex 풀고 잠-> notify시 꺠어나서 조건 재확인 
    { //즉 큐가 비었으면 false-> 대기 
        return !_queue.empty() || stopToken.stop_requested();
    });

    if (stopToken.stop_requested() && _queue.empty())
        return nullptr;

    auto front = std::move(_queue.front()); //큐에서 첫번째 잡큐 move 
    _queue.pop();
    return front;
}
