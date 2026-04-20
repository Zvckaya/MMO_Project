#include "GameServer.h"
#include <chrono>
#include <Windows.h>

void GameServer::BroadcastAll(Packet* packet) //모두에게 broadcast한다.
{
	for (auto& [id, player] : _players)
		SendPacket(id, packet);
}

void GameServer::BroadcastExcept(SessionID excludeID, Packet* packet) //특정 세션을 제외하고 broadcast한다 
{
	for (auto& [id, player] : _players)
		if (id != excludeID)
			SendPacket(id, packet);
}

GameServer::~GameServer()
{
	Stop();
}

bool GameServer::Start(std::optional<std::string_view> openIp, uint16_t port,
	int workerThreadCount, int runningThreadCount,
	bool nagleOption, int maxSessionCount)
{
	constexpr int immediateThreadCount = 4; //즉답 스레드 개수

	if (_isGameRunning.load())
		return false;

	if (!IOCPServer::Start(openIp, port, workerThreadCount, runningThreadCount, nagleOption, maxSessionCount))
		return false;

	//패킷 등록(여기서 핸들러는 세션id와 패킷을 인자로 받아 해당 패킷에 대응하는 함수를 call한다.
	_packetProc.Register(PKT_ECHO, [this](SessionID sessionID, Packet* packet) 
	{
		SendPacket(sessionID, packet);
	});

	_isGameRunning.store(true);

	for (int i = 0; i < immediateThreadCount; ++i) //즉답 스레드 생성
	{
		_immediateThreads.emplace_back([this](std::stop_token stopToken)
		{
			CreateImmediateThread(stopToken);
		});
	}

	_frameThread = std::jthread([this](std::stop_token stopToken) //프레임 스레드(싱글) 생성
	{
		CreateFrameThread(stopToken);
	});

	return true;
}

void GameServer::Stop()
{
	if (!_isGameRunning.exchange(false))
	{
		IOCPServer::Stop();
		return;
	}

	_immediateThreads.clear();
	_frameThread.request_stop();
	if (_frameThread.joinable())
		_frameThread = std::jthread();

	{
		std::unique_lock lock(_jobQueuesMutex);
		_sessionJobQueues.clear();
	}

	{
		std::lock_guard lock(_frameTaskMutex);
		while (!_frameTaskQueue.empty())
			_frameTaskQueue.pop();
	}

	_players.clear();

	IOCPServer::Stop();
}

bool GameServer::OnConnectionRequest(std::string_view ip, uint16_t port)
{
	
	return true;
}

void GameServer::OnClientJoin(SessionID sessionID)
{
	//클라이언트 join시 
	if (!_isGameRunning.load())
		return;
	auto jobQueue = std::make_shared<JobQueue>(_globalQueue); //세션 전용 jobqueue생성 
	{
		std::unique_lock lock(_jobQueuesMutex);
		_sessionJobQueues[sessionID] = std::move(jobQueue); //등록 
	}
	EnqueueFrameTask({ FrameTaskType::clientJoin, sessionID }); 
}

void GameServer::OnClientLeave(SessionID sessionID) //이는 세션이 삭제되었을시에만 호출됨 
{
	if (!_isGameRunning.load())
		return;
	{
		std::unique_lock lock(_jobQueuesMutex);
		_sessionJobQueues.erase(sessionID); //세션 job큐 삭제만
	}
	EnqueueFrameTask({ FrameTaskType::clientLeave, sessionID }); //플레이어 삭제 요청 
}

void GameServer::OnRecv(SessionID sessionID, Packet* packet)
{
	if (!_isGameRunning.load())
	{
		_packetPool.Free(packet);
		ReleaseContentRef(sessionID); //콘텐츠로 넘어왔으니 io 감소 
		return;
	}

	std::shared_ptr<JobQueue> jobQueue; 
	{
		std::shared_lock lock(_jobQueuesMutex);
		auto it = _sessionJobQueues.find(sessionID); 
		if (it == _sessionJobQueues.end())
		{
			_packetPool.Free(packet);
			ReleaseContentRef(sessionID); //무조건 패킷을 풀고 ioCount감소시켜야한다 .
			return;
		}
		jobQueue = it->second; // 플레이어 잡큐 
	}

	jobQueue->Post([this, sessionID, packet]() //플레이어 잡큐에 push
	{
		_packetProc.Dispatch(sessionID, packet);
		_packetPool.Free(packet);
		ReleaseContentRef(sessionID);
	});
}

void GameServer::CreateImmediateThread(std::stop_token stopToken) //즉답스레드 
{
	while (!stopToken.stop_requested())
	{
		auto jobQueue = _globalQueue.Pop(stopToken); //Pop안에 event로 blocking 
		if (!jobQueue)
			break;
		const uint64_t endTick = ::GetTickCount64() + 64; //무한 점유를 막기위해 해당 시간 wpgks 
		jobQueue->Execute(endTick);
	}
}

void GameServer::CreateFrameThread(std::stop_token stopToken)
{
	using Clock = std::chrono::steady_clock;
	using Ms    = std::chrono::milliseconds;

	const Ms frameDuration(1000 / FRAME_RATE);
	std::queue<FrameTask> localQueue;
	auto prevTime = Clock::now();

	while (true)
	{
		auto frameStart = Clock::now();
		int64_t deltaMs = std::chrono::duration_cast<Ms>(frameStart - prevTime).count();
		prevTime = frameStart;

		{
			std::lock_guard lock(_frameTaskMutex);
			std::swap(localQueue, _frameTaskQueue); //프레임 스레드는 1개니까 sleep된동안 쌓인 task를 swap을 통해 교체함 
		}

		while (!localQueue.empty())
		{
			ProcessFrameTask(localQueue.front());
			localQueue.pop();
		}

		Update(deltaMs);

		auto elapsed = std::chrono::duration_cast<Ms>(Clock::now() - frameStart);
		if (elapsed < frameDuration)
			std::this_thread::sleep_for(frameDuration - elapsed);

		if (stopToken.stop_requested())
			break;
	}
}

void GameServer::EnqueueFrameTask(FrameTask task)
{
	if (!_isGameRunning.load())
		return;

	std::lock_guard lock(_frameTaskMutex);
	_frameTaskQueue.push(std::move(task));
}

void GameServer::ProcessFrameTask(const FrameTask& task)
{
	switch (task.type)
	{
	case FrameTaskType::clientJoin:
		_players[task.sessionID] = std::make_unique<Player>(task.sessionID);
		break;
	case FrameTaskType::clientLeave:
		_players.erase(task.sessionID);
		break;
	}
}

void GameServer::Update(int64_t deltaMs)
{
	(void)deltaMs; 
}

