#include "JobQueue.h"
#include "Job.h"
#include "GlobalQueue.h"

#include <Windows.h>

void JobQueue::Post(std::function<void()> callback)
{
    Push(std::make_shared<Job>(std::move(callback))); //콜백을 job으로 만들어서 등록 
}

void JobQueue::Push(std::shared_ptr<Job> job)
{
    const int prevCount = _jobCount.fetch_add(1);

    {
        std::lock_guard<std::mutex> lock(_jobMutex);
        _jobs.push(std::move(job));  //job을 큐에 push 
    }

    if (prevCount == 0)
    {
        _globalQueue.Push(shared_from_this()); //글로벌 큐에 등록 
    }
}

void JobQueue::Execute(uint64_t endTick)
{
    while (true)
    {
        std::vector<std::shared_ptr<Job>> jobs;
        {
            std::lock_guard<std::mutex> lock(_jobMutex);
            while (!_jobs.empty())
            {
                jobs.push_back(std::move(_jobs.front()));
                _jobs.pop();
            }
        }

        const int jobCount = static_cast<int>(jobs.size());
        for (auto& job : jobs)
            job->Execute();
        if (_jobCount.fetch_sub(jobCount) == jobCount) //job을 전부 처리했으면 return
            return;
        if (::GetTickCount64() >= endTick) //시간 초과했는데 jbo이있으면 
        {
            _globalQueue.Push(shared_from_this()); //다시 재등록
            return;
        }
    }
}

