#include "GameServer.h"
#include "AuthClient.h"
#include "Logger.h"
#include <chrono>
#include <cmath>
#include <Windows.h>

void GameServer::BroadcastAll(MapID mapID, Packet* packet)
{
	auto it = _maps.find(mapID);
	if (it == _maps.end()) return;
	for (auto& [id, player] : it->second->GetPlayers())
		SendPacket(id, packet);
}

void GameServer::BroadcastExcept(MapID mapID, SessionID excludeID, Packet* packet)
{
	auto it = _maps.find(mapID);
	if (it == _maps.end()) return;
	for (auto& [id, player] : it->second->GetPlayers())
		if (id != excludeID)
			SendPacket(id, packet);
}

GameMap* GameServer::FindPlayerMap(SessionID sessionID)
{
	auto mapIt = _sessionToMapID.find(sessionID);
	if (mapIt == _sessionToMapID.end()) return nullptr;
	auto it = _maps.find(mapIt->second);
	return it != _maps.end() ? it->second.get() : nullptr;
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

	_maps.emplace(1, std::make_unique<GameMap>(1));
	_maps.emplace(2, std::make_unique<GameMap>(2));

	{
		DBClient db;
		if (db.Connect(DB_HOST, DB_USER, DB_PASSWORD, DB_NAME, DB_PORT))
			_itemDataMap = db.LoadItemData();
	}

	_isGameRunning.store(true);

	_authThread = std::jthread([this](std::stop_token stopToken)
	{
		CreateAuthThread(stopToken);
	});

	_dbThread = std::jthread([this](std::stop_token stopToken)
	{
		CreateDBThread(stopToken);
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

	_dbQueueCv.notify_all();
	if (_dbThread.joinable())
		_dbThread = std::jthread();

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

	_maps.clear();
	_sessionToMapID.clear();

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

	if (!jobQueue->isAuthenticated && packet->GetType() != PKT_CS_LOGIN_AUTH) //인증 패킷이 아니고, 인증 세션도 아니면 disconnect
	{
		_packetPool.Free(packet);
		ReleaseContentRef(sessionID);
		Disconnect(sessionID);
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
			EnqueueDBRequest({
				.type      = DBRequest::Type::loadPlayer,
				.accountId = accountId,
				.onLoad    = [this, sessionID, accountId, displayName](const PlayerDBData& dbData)
				{
					EnqueueFrameTask({
						.type        = FrameTaskType::playerAuth,
						.sessionID   = sessionID,
						.accountId   = accountId,
						.displayName = displayName,
						.mapID       = dbData.found ? dbData.mapID : DEFAULT_MAP_ID,
						.dbData      = dbData
					});
				}
			});
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

void GameServer::EnqueueDBRequest(DBRequest req)
{
	std::lock_guard lock(_dbQueueMutex);
	_dbPendingQueue.push(std::move(req));
	_dbQueueCv.notify_one();
}

void GameServer::CreateDBThread(std::stop_token stopToken)
{
	std::stop_callback onStop(stopToken, [this] { _dbQueueCv.notify_all(); });

	DBClient db;
	if (!db.Connect(DB_HOST, DB_USER, DB_PASSWORD, DB_NAME, DB_PORT))
		return;

	while (true)
	{
		DBRequest req;
		{
			std::unique_lock lock(_dbQueueMutex);
			_dbQueueCv.wait(lock, [&] { return !_dbPendingQueue.empty() || stopToken.stop_requested(); });
			if (stopToken.stop_requested() && _dbPendingQueue.empty())
				break;
			req = std::move(_dbPendingQueue.front());
			_dbPendingQueue.pop();
		}

		if (!db.Ping())
		{
			db.Disconnect();
			if (!db.Connect(DB_HOST, DB_USER, DB_PASSWORD, DB_NAME, DB_PORT))
				break;
		}

		switch (req.type)
		{
		case DBRequest::Type::loadPlayer:
		{
			PlayerDBData data = db.LoadPlayer(req.accountId);
			if (req.onLoad)
				req.onLoad(data);
			break;
		}
		case DBRequest::Type::savePlayer:
			db.SavePlayer(req.accountId, req.saveData);
			break;
		case DBRequest::Type::saveInventory:
			db.SaveInventory(req.accountId, req.inventoryData);
			break;
		}
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
		auto mapIt = _maps.find(task.mapID);
		if (mapIt == _maps.end()) break;
		GameMap* map = mapIt->second.get();

		auto newPlayerPtr = std::make_unique<Player>(task.sessionID, task.accountId, task.displayName);
		Player* newPlayer = newPlayerPtr.get();
		if (task.dbData.found)
		{
			newPlayer->posX = task.dbData.posX;
			newPlayer->posY = task.dbData.posY;
			newPlayer->hp   = task.dbData.hp;
			for (const auto& slot : task.dbData.inventory)
				if (slot.slotIndex < MAX_INVENTORY_SLOTS)
					newPlayer->inventory[slot.slotIndex] = { slot.itemID, slot.count };
		}
		map->AddPlayer(task.sessionID, std::move(newPlayerPtr));
		_sessionToMapID[task.sessionID] = task.mapID;

		// 1. 맵 정보
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_MAP_INFO);
			p->WriteStruct(SC_MAP_INFO{ task.mapID });
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
		}
		// 2. 맵에 있는 월드 아이템 목록
		for (auto& [uid, worldItem] : map->GetWorldItems())
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_ITEM_APPEAR);
			p->WriteStruct(SC_ITEM_APPEAR{ uid, worldItem.itemID, worldItem.posX, worldItem.posY, worldItem.count });
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
		}
		// 3. 인벤토리
		for (uint16_t i = 0; i < MAX_INVENTORY_SLOTS; ++i)
		{
			const auto& slot = newPlayer->inventory[i];
			if (slot.itemID == 0) continue;
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_INVENTORY_UPD);
			p->WriteStruct(SC_INVENTORY_UPD{ i, slot.itemID, static_cast<uint16_t>(slot.count) });
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
		}
		// 4. 내 캐릭터 정보
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_CREATE_MY_CHARACTER);
			p->WriteStruct(SC_CREATE_MY_CHARACTER{ task.sessionID, newPlayer->posX, newPlayer->posY, newPlayer->hp, newPlayer->maxHp, newPlayer->speed, static_cast<uint16_t>(task.displayName.size()) });
			p->PutData(task.displayName.c_str(), static_cast<int>(task.displayName.size()));
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
		}
		// 5. 같은 맵의 기존 플레이어 목록
		for (auto& [id, player] : map->GetPlayers())
		{
			if (id == task.sessionID) continue;
			const std::string& name = player->GetDisplayName();
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_CREATE_OTHER_CHARACTER);
			p->WriteStruct(SC_CREATE_OTHER_CHARACTER{ id, player->posX, player->posY, player->hp, player->maxHp, player->speed, static_cast<uint16_t>(name.size()) });
			p->PutData(name.c_str(), static_cast<int>(name.size()));
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
		}
		// 4. 같은 맵의 기존 플레이어들에게 신규 플레이어 알림
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_CREATE_OTHER_CHARACTER);
			p->WriteStruct(SC_CREATE_OTHER_CHARACTER{ task.sessionID, 0.f, 0.f, newPlayer->hp, newPlayer->maxHp, newPlayer->speed, static_cast<uint16_t>(task.displayName.size()) });
			p->PutData(task.displayName.c_str(), static_cast<int>(task.displayName.size()));
			BroadcastExcept(task.mapID, task.sessionID, p);
			_packetPool.Free(p);
		}
		// 5. 스폰 완료
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
		GameMap* map = FindPlayerMap(task.sessionID);
		if (map)
		{
			Player* player = map->FindPlayer(task.sessionID);
			if (player)
			{
				EnqueueDBRequest({
					.type      = DBRequest::Type::savePlayer,
					.accountId = player->GetAccountId(),
					.saveData  = { true, player->posX, player->posY, player->hp, map->GetID() }
				});

				std::vector<InventorySlotData> slots;
				for (uint16_t i = 0; i < MAX_INVENTORY_SLOTS; ++i)
				{
					const auto& slot = player->inventory[i];
					if (slot.itemID == 0 || slot.count <= 0) continue;
					slots.push_back({ i, slot.itemID, slot.count });
				}
				EnqueueDBRequest({
					.type          = DBRequest::Type::saveInventory,
					.accountId     = player->GetAccountId(),
					.inventoryData = std::move(slots)
				});
			}
			map->RemovePlayer(task.sessionID);
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_DESPAWN);
			p->WriteStruct(SC_DESPAWN{ task.sessionID });
			BroadcastExcept(map->GetID(), task.sessionID, p);
			_packetPool.Free(p);
		}
		_sessionToMapID.erase(task.sessionID);
		break;
	}
	case FrameTaskType::playerMove:
		break;
	case FrameTaskType::playerAttack:
	{
		GameMap* map = FindPlayerMap(task.sessionID);
		if (!map) break;

		Player* attacker = map->FindPlayer(task.sessionID);
		Player* target   = map->FindPlayer(task.targetID);
		if (!attacker || !target) break;

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
		BroadcastAll(map->GetID(), p);
		_packetPool.Free(p);
		break;
	}
	case FrameTaskType::playerSkill:
		break;
	case FrameTaskType::itemSlotSwap:
	{
		GameMap* map = FindPlayerMap(task.sessionID);
		if (!map) break;
		Player* player = map->FindPlayer(task.sessionID);
		if (!player) break;
		if (task.fromSlot >= MAX_INVENTORY_SLOTS || task.toSlot >= MAX_INVENTORY_SLOTS) break;
		if (task.fromSlot == task.toSlot) break;

		std::swap(player->inventory[task.fromSlot], player->inventory[task.toSlot]);

		auto sendSlot = [&](uint16_t idx)
		{
			const auto& slot = player->inventory[idx];
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_INVENTORY_UPD);
			p->WriteStruct(SC_INVENTORY_UPD{ idx, slot.itemID, static_cast<uint16_t>(slot.count) });
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
		};
		sendSlot(task.fromSlot);
		sendSlot(task.toSlot);
		break;
	}
	case FrameTaskType::itemDrop:
	{
		GameMap* map = FindPlayerMap(task.sessionID);
		if (!map) break;
		Player* player = map->FindPlayer(task.sessionID);
		if (!player) break;
		if (task.fromSlot >= MAX_INVENTORY_SLOTS) break;

		InventorySlot& slot = player->inventory[task.fromSlot];
		if (slot.itemID == 0) break;
		if (_itemDataMap.find(slot.itemID) == _itemDataMap.end()) break;

		uint16_t droppedItemID = slot.itemID;
		int32_t  droppedCount  = slot.count;
		slot = {};

		Log(L"ITEM_DROP", Logger::Level::DEBUG, L"[%llu] itemID=%u count=%d slot=%u pos(%.2f, %.2f)",
			task.sessionID, droppedItemID, droppedCount, task.fromSlot, player->posX, player->posY);

		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_INVENTORY_UPD);
			p->WriteStruct(SC_INVENTORY_UPD{ task.fromSlot, 0, 0 });
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
		}

		uint64_t uid = map->SpawnWorldItem(droppedItemID, droppedCount, player->posX, player->posY);
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_ITEM_APPEAR);
			p->WriteStruct(SC_ITEM_APPEAR{ uid, droppedItemID, player->posX, player->posY, droppedCount });
			BroadcastAll(map->GetID(), p);
			_packetPool.Free(p);
		}
		break;
	}
	case FrameTaskType::itemPickup:
	{
		GameMap* map = FindPlayerMap(task.sessionID);
		if (!map) break;
		Player* player = map->FindPlayer(task.sessionID);
		if (!player) break;

		WorldItem* worldItem = map->FindWorldItem(task.targetID);
		if (!worldItem) break;

		float dx = player->posX - worldItem->posX;
		float dy = player->posY - worldItem->posY;
		if (sqrtf(dx * dx + dy * dy) > ITEM_PICKUP_RANGE) break;

		int emptySlot = -1;
		for (int i = 0; i < MAX_INVENTORY_SLOTS; ++i)
			if (player->inventory[i].itemID == 0) { emptySlot = i; break; }
		if (emptySlot < 0) break;

		uint16_t pickedItemID = worldItem->itemID;
		int32_t  pickedCount  = worldItem->count;
		uint64_t uid          = worldItem->itemUID;

		Log(L"ITEM_PICKUP", Logger::Level::DEBUG, L"[%llu] itemUID=%llu itemID=%u count=%d slot=%d dist=%.2f",
			task.sessionID, uid, pickedItemID, pickedCount, emptySlot, sqrtf(dx * dx + dy * dy));

		player->inventory[emptySlot] = { pickedItemID, pickedCount };
		map->RemoveWorldItem(uid);

		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_ITEM_DISAPPEAR);
			p->WriteStruct(SC_ITEM_DISAPPEAR{ uid });
			BroadcastAll(map->GetID(), p);
			_packetPool.Free(p);
		}
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_INVENTORY_UPD);
			p->WriteStruct(SC_INVENTORY_UPD{ static_cast<uint16_t>(emptySlot), pickedItemID, static_cast<uint16_t>(pickedCount) });
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
		}
		break;
	}
	case FrameTaskType::playerMapChange:
	{
		auto targetMapIt = _maps.find(task.targetMapID);
		if (targetMapIt == _maps.end())
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_MAP_CHANGE);
			p->WriteStruct(SC_MAP_CHANGE{ task.targetMapID, 1 });
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
			break;
		}

		GameMap* oldMap = FindPlayerMap(task.sessionID);
		if (oldMap && oldMap->GetID() == task.targetMapID)
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_MAP_CHANGE);
			p->WriteStruct(SC_MAP_CHANGE{ task.targetMapID, 2 });
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
			break;
		}

		// 기존 맵에서 Player 객체 꺼내기 + DESPAWN 브로드캐스트
		std::unique_ptr<Player> playerOwnership;
		if (oldMap)
		{
			playerOwnership = oldMap->TakePlayer(task.sessionID);
			if (playerOwnership)
			{
				Packet* p = _packetPool.Alloc();
				p->Clear();
				p->SetType(PKT_SC_DESPAWN);
				p->WriteStruct(SC_DESPAWN{ task.sessionID });
				BroadcastExcept(oldMap->GetID(), task.sessionID, p);
				_packetPool.Free(p);
			}
		}

		if (!playerOwnership) break;

		// 새 맵에 삽입
		GameMap* newMap = targetMapIt->second.get();
		Player* player = playerOwnership.get();
		player->posX = player->posY = player->destX = player->destY = 0.f;
		player->isMoving = false;
		newMap->AddPlayer(task.sessionID, std::move(playerOwnership));
		_sessionToMapID[task.sessionID] = task.targetMapID;

		const std::string& name = player->GetDisplayName();

		// 1. 맵 변경 성공 알림
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_MAP_CHANGE);
			p->WriteStruct(SC_MAP_CHANGE{ task.targetMapID, 0 });
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
		}
		// 2. 내 캐릭터 정보
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_CREATE_MY_CHARACTER);
			p->WriteStruct(SC_CREATE_MY_CHARACTER{ task.sessionID, player->posX, player->posY, player->hp, player->maxHp, player->speed, static_cast<uint16_t>(name.size()) });
			p->PutData(name.c_str(), static_cast<int>(name.size()));
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
		}
		// 3. 새 맵의 기존 플레이어 목록
		for (auto& [id, other] : newMap->GetPlayers())
		{
			if (id == task.sessionID) continue;
			const std::string& otherName = other->GetDisplayName();
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_CREATE_OTHER_CHARACTER);
			p->WriteStruct(SC_CREATE_OTHER_CHARACTER{ id, other->posX, other->posY, other->hp, other->maxHp, other->speed, static_cast<uint16_t>(otherName.size()) });
			p->PutData(otherName.c_str(), static_cast<int>(otherName.size()));
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
		}
		// 4. 새 맵 플레이어들에게 입장 알림
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_CREATE_OTHER_CHARACTER);
			p->WriteStruct(SC_CREATE_OTHER_CHARACTER{ task.sessionID, player->posX, player->posY, player->hp, player->maxHp, player->speed, static_cast<uint16_t>(name.size()) });
			p->PutData(name.c_str(), static_cast<int>(name.size()));
			BroadcastExcept(task.targetMapID, task.sessionID, p);
			_packetPool.Free(p);
		}
		// 5. 스폰 완료
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_WORLD_ENTER);
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
		}
		break;
	}
	}
}

void GameServer::Update(int64_t deltaMs)
{
	{
		std::shared_lock lock(_jobQueuesMutex);
		for (auto& [sessionID, jobQueue] : _sessionJobQueues)
		{
			GameMap* map = FindPlayerMap(sessionID);

			if (jobQueue->pendingStop.dirty.load(std::memory_order_acquire))
			{
				if (map)
				{
					Player* player = map->FindPlayer(sessionID);
					if (player)
					{
						Log(L"STOP", Logger::Level::DEBUG, L"[%llu] client(%.2f, %.2f) server(%.2f, %.2f)", sessionID, jobQueue->pendingStop.curX, jobQueue->pendingStop.curY, player->posX, player->posY);
						player->posX     = jobQueue->pendingStop.curX;
						player->posY     = jobQueue->pendingStop.curY;
						player->destX    = jobQueue->pendingStop.curX;
						player->destY    = jobQueue->pendingStop.curY;
						player->isMoving = false;
					}
				}
				jobQueue->pendingStop.dirty.store(false, std::memory_order_release);
			}

			if (!jobQueue->pendingMove.dirty.load(std::memory_order_acquire))
				continue;

			if (!map)
			{
				jobQueue->pendingMove.dirty.store(false, std::memory_order_relaxed);
				continue;
			}

			Player* player = map->FindPlayer(sessionID);
			if (!player)
			{
				jobQueue->pendingMove.dirty.store(false, std::memory_order_relaxed);
				continue;
			}

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
			BroadcastExcept(map->GetID(), sessionID, p);
			_packetPool.Free(p);
		}
	}

	uint64_t now = ::GetTickCount64();
	float dt = static_cast<float>(deltaMs) / 1000.f;
	for (auto& [mapID, map] : _maps)
	{
		auto& worldItems = map->GetWorldItems();
		for (auto it = worldItems.begin(); it != worldItems.end(); )
		{
			if (now - it->second.spawnTick >= ITEM_DESPAWN_MS)
			{
				uint64_t uid = it->second.itemUID;
				it = worldItems.erase(it);
				Packet* p = _packetPool.Alloc();
				p->Clear();
				p->SetType(PKT_SC_ITEM_DISAPPEAR);
				p->WriteStruct(SC_ITEM_DISAPPEAR{ uid });
				BroadcastAll(mapID, p);
				_packetPool.Free(p);
			}
			else ++it;
		}

		for (auto& [id, player] : map->GetPlayers())
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
}

