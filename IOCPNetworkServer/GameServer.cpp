#include "GameServer.h"
#include "AuthClient.h"
#include <chrono>
#include <cmath>
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

	_isGameRunning.store(true);

	_authThread = std::jthread([this](std::stop_token stopToken)
	{
		CreateAuthThread(stopToken);
	});

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

	if (_authThread.joinable())
		_authThread = std::jthread();

	_frameThread.request_stop();
	if (_frameThread.joinable())
		_frameThread = std::jthread();

	{
		std::unique_lock lock(_jobQueuesMutex);
		_sessionJobQueues.clear();
	}

	{
		std::unique_lock lock(_authStatesMutex);
		_sessionAuthStates.clear();
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
	if (!_isGameRunning.load())
		return;
	auto jobQueue = std::make_shared<JobQueue>(_globalQueue);
	{
		std::unique_lock lock(_jobQueuesMutex);
		_sessionJobQueues[sessionID] = std::move(jobQueue);
	}
	{
		std::unique_lock lock(_authStatesMutex);
		_sessionAuthStates[sessionID] = SessionAuthState{};
	}
}

void GameServer::OnClientLeave(SessionID sessionID)
{
	if (!_isGameRunning.load())
		return;
	{
		std::unique_lock lock(_jobQueuesMutex);
		_sessionJobQueues.erase(sessionID);
	}
	{
		std::unique_lock lock(_authStatesMutex);
		_sessionAuthStates.erase(sessionID);
	}
	EnqueueFrameTask({ .type = FrameTaskType::clientLeave, .sessionID = sessionID });
}

void GameServer::OnRecv(SessionID sessionID, Packet* packet)
{
	if (!_isGameRunning.load())
	{
		_packetPool.Free(packet);
		ReleaseContentRef(sessionID);
		return;
	}

	std::shared_ptr<JobQueue> jobQueue;
	{
		std::shared_lock lock(_jobQueuesMutex);
		auto it = _sessionJobQueues.find(sessionID);
		if (it == _sessionJobQueues.end())
		{
			_packetPool.Free(packet);
			ReleaseContentRef(sessionID);
			return;
		}
		jobQueue = it->second;
	}

	if (!jobQueue->isAuthenticated && packet->GetType() != PKT_CS_LOGIN_AUTH)
	{
		_packetPool.Free(packet);
		ReleaseContentRef(sessionID);
		return;
	}

	jobQueue->Post([this, sessionID, packet]()
	{
		HandlePacket(sessionID, packet);
		_packetPool.Free(packet);
		ReleaseContentRef(sessionID);
	});
}

void GameServer::CreateAuthThread(std::stop_token stopToken)
{
	std::stop_callback onStop(stopToken, [this] { _authQueueCv.notify_all(); });

	while (true)
	{
		AuthRequest req;
		{
			std::unique_lock lock(_authQueueMutex);
			_authQueueCv.wait(lock, [&] { return !_authPendingQueue.empty() || stopToken.stop_requested(); });
			if (stopToken.stop_requested() && _authPendingQueue.empty())
				break;
			req = _authPendingQueue.front();
			_authPendingQueue.pop();
		}

		AuthResult auth = VerifyTicket(req.ticket, static_cast<int>(sizeof(req.ticket)));
		ProcessAuthResult(req.sessionID, auth);
	}
}

void GameServer::ProcessAuthResult(SessionID sessionID, const AuthResult& auth)
{
	std::shared_ptr<JobQueue> jobQueue;
	{
		std::shared_lock lock(_jobQueuesMutex);
		auto it = _sessionJobQueues.find(sessionID);
		if (it == _sessionJobQueues.end())
			return;
		jobQueue = it->second;
	}

	jobQueue->Post([this, sessionID, auth]()
	{
		uint32_t    errorCode   = auth.valid ? AUTH_OK : AUTH_ERR_INVALID;
		uint64_t    accountId   = auth.valid ? auth.accountId : 0ULL;
		std::string displayName = auth.valid ? auth.displayName : "";

		if (auth.valid)
		{
			{
				std::shared_lock lock(_jobQueuesMutex);
				auto it = _sessionJobQueues.find(sessionID);
				if (it != _sessionJobQueues.end())
					it->second->isAuthenticated = true;
			}
			{
				std::unique_lock lock(_authStatesMutex);
				auto it = _sessionAuthStates.find(sessionID);
				if (it != _sessionAuthStates.end())
				{
					it->second.accountId   = accountId;
					it->second.displayName = displayName;
				}
			}
		}

		Packet* p = _packetPool.Alloc();
		p->Clear();
		p->SetType(PKT_SC_LOGIN_AUTH_RESULT);
		p->WriteStruct(SC_LOGIN_AUTH_RESULT{
			static_cast<uint8_t>(auth.valid ? 1 : 0),
			errorCode,
			accountId,
			static_cast<uint16_t>(displayName.size())
		});
		p->PutData(displayName.c_str(), static_cast<int>(displayName.size()));
		SendPacket(sessionID, p);
		_packetPool.Free(p);

		if (auth.valid)
			EnqueueFrameTask({ .type        = FrameTaskType::playerAuth,
			                   .sessionID   = sessionID,
			                   .accountId   = accountId,
			                   .displayName = displayName });
		else
			Disconnect(sessionID);
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
		break;
	case FrameTaskType::playerAuth:
	{
		for (auto& [id, player] : _players)
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_SPAWN);
			p->WriteStruct(SC_SPAWN{ id, player->posX, player->posY, player->hp, player->maxHp });
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
		}
		_players[task.sessionID] = std::make_unique<Player>(task.sessionID, task.accountId, task.displayName);
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_SPAWN);
			p->WriteStruct(SC_SPAWN{ task.sessionID, 0.f, 0.f, 100, 100 });
			BroadcastExcept(task.sessionID, p);
			_packetPool.Free(p);
		}
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_WORLD_ENTER);
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
		}
		break;
	}
	case FrameTaskType::clientLeave:
	{
		_players.erase(task.sessionID);
		Packet* p = _packetPool.Alloc();
		p->Clear();
		p->SetType(PKT_SC_DESPAWN);
		p->WriteStruct(SC_DESPAWN{ task.sessionID });
		BroadcastAll(p);
		_packetPool.Free(p);
		break;
	}
	case FrameTaskType::playerMove:
		break;
	case FrameTaskType::playerAttack:
	{
		auto attackerIt = _players.find(task.sessionID);
		auto targetIt   = _players.find(task.targetID);
		if (attackerIt == _players.end() || targetIt == _players.end()) break;

		Player* attacker = attackerIt->second.get();
		Player* target   = targetIt->second.get();

		float dx = attacker->posX - target->posX;
		float dy = attacker->posY - target->posY;
		if (sqrtf(dx * dx + dy * dy) > ATTACK_RANGE) break;

		constexpr int32_t damage = 10;
		target->hp -= damage;
		if (target->hp < 0) target->hp = 0;

		Packet* p = _packetPool.Alloc();
		p->Clear();
		p->SetType(PKT_SC_ATTACK);
		p->WriteStruct(SC_ATTACK{ task.sessionID, task.targetID, damage, target->hp });
		BroadcastAll(p);
		_packetPool.Free(p);
		break;
	}
	case FrameTaskType::playerSkill:
		break;
	}
}

void GameServer::Update(int64_t deltaMs)
{
	{
		std::shared_lock lock(_jobQueuesMutex);
		for (auto& [sessionID, jobQueue] : _sessionJobQueues)
		{
			if (jobQueue->pendingStop.dirty.load(std::memory_order_acquire))
			{
				auto it = _players.find(sessionID);
				if (it != _players.end())
				{
					Player* player = it->second.get();
					player->posX     = jobQueue->pendingStop.curX;
					player->posY     = jobQueue->pendingStop.curY;
					player->destX    = jobQueue->pendingStop.curX;
					player->destY    = jobQueue->pendingStop.curY;
					player->isMoving = false;
				}
				jobQueue->pendingStop.dirty.store(false, std::memory_order_release);
			}

			if (!jobQueue->pendingMove.dirty.load(std::memory_order_acquire))
				continue;

			auto it = _players.find(sessionID);
			if (it == _players.end())
			{
				jobQueue->pendingMove.dirty.store(false, std::memory_order_relaxed);
				continue;
			}

			Player* player = it->second.get();
			player->posX     = jobQueue->pendingMove.curX;
			player->posY     = jobQueue->pendingMove.curY;
			player->destX    = jobQueue->pendingMove.destX;
			player->destY    = jobQueue->pendingMove.destY;
			player->isMoving = true;
			jobQueue->pendingMove.dirty.store(false, std::memory_order_release);

			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_MOVE);
			p->WriteStruct(ScMovePacket{
				sessionID,
				player->posX, player->posY,
				player->destX, player->destY,
				player->speed
			});
			BroadcastExcept(sessionID, p);
			_packetPool.Free(p);
		}
	}

	float dt = static_cast<float>(deltaMs) / 1000.f;
	for (auto& [id, player] : _players)
	{
		if (!player->isMoving) continue;
		float dx   = player->destX - player->posX;
		float dy   = player->destY - player->posY;
		float dist = sqrtf(dx * dx + dy * dy);
		float step = player->speed * dt;
		if (dist <= step)
		{
			player->posX     = player->destX;
			player->posY     = player->destY;
			player->isMoving = false;
		}
		else
		{
			float ratio  = step / dist;
			player->posX += dx * ratio;
			player->posY += dy * ratio;
		}
	}
}

