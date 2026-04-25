#include "GameServer.h"
#include "AuthClient.h"
#include "AStar.h"
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
		GameMap* map1 = _maps[1].get();
		map1->SpawnMonster(++_monsterIDCounter, 1,  5.5f,  4.5f);
		map1->SpawnMonster(++_monsterIDCounter, 1, 25.0f,  5.0f);
		map1->SpawnMonster(++_monsterIDCounter, 1,  6.0f, 12.0f);
		map1->SpawnMonster(++_monsterIDCounter, 1, 20.0f, 10.0f);
		map1->SpawnMonster(++_monsterIDCounter, 1, 14.0f, 20.0f);
	}

	if (_gridMap.Load("maps/map_001.txt"))
		Log(L"GridMap", Logger::Level::SYSTEM, L"GridMap loaded (%d x %d)", _gridMap.GetWidth(), _gridMap.GetHeight());
	else
		Log(L"GridMap", Logger::Level::WARN, L"maps/map_001.txt not found — wall check disabled");

	{
		DBClient db;
		if (db.Connect(GAME_DB_HOST, GAME_DB_USER, GAME_DB_PASSWORD, GAME_DB_NAME, GAME_DB_PORT))
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
		std::lock_guard lock(_unauthMutex);
		_unauthSessions.clear();
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
		std::lock_guard lock(_unauthMutex);
		_unauthSessions.insert(sessionID);
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
		std::lock_guard lock(_unauthMutex);
		_unauthSessions.erase(sessionID);
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

	{
		std::shared_lock lock(_authStatesMutex);
		bool isAuth = _sessionAuthStates.count(sessionID) > 0;
		if (!isAuth && packet->GetType() != PKT_CS_LOGIN_AUTH)
		{
			_packetPool.Free(packet);
			ReleaseContentRef(sessionID);
			Disconnect(sessionID);
			return;
		}
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
				std::lock_guard lock(_unauthMutex);
				_unauthSessions.erase(sessionID);
			}
			{
				std::unique_lock lock(_authStatesMutex);
				_sessionAuthStates[sessionID] = SessionAuthState{ accountId, displayName };
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
	if (!db.Connect(GAME_DB_HOST, GAME_DB_USER, GAME_DB_PASSWORD, GAME_DB_NAME, GAME_DB_PORT))
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
			if (!db.Connect(GAME_DB_HOST, GAME_DB_USER, GAME_DB_PASSWORD, GAME_DB_NAME, GAME_DB_PORT))
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
		// 3. 맵 몬스터 목록
		for (auto& monster : map->GetMonsters())
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_NPC_SPAWN);
			p->WriteStruct(SC_NPC_SPAWN{ monster->GetID(), monster->GetTemplateID(), monster->posX, monster->posY, monster->hp, monster->maxHp });
			SendPacket(task.sessionID, p);
			_packetPool.Free(p);
		}
		// 4. 인벤토리
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
			p->WriteStruct(SC_CREATE_OTHER_CHARACTER{ task.sessionID, newPlayer->posX,newPlayer->posY, newPlayer->hp, newPlayer->maxHp, newPlayer->speed, static_cast<uint16_t>(task.displayName.size()) });
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
		if (!attacker) break;

		Monster* monsterTarget = map->FindMonster(task.targetID);
		if (!monsterTarget || monsterTarget->isDead) break;

		float dx = attacker->posX - monsterTarget->posX;
		float dy = attacker->posY - monsterTarget->posY;
		if (sqrtf(dx * dx + dy * dy) > ATTACK_RANGE) break;

		constexpr int32_t damage = 10;
		monsterTarget->hp -= damage;
		if (monsterTarget->hp < 0) monsterTarget->hp = 0;

		Packet* p = _packetPool.Alloc();
		p->Clear();
		p->SetType(PKT_SC_ATTACK);
		p->WriteStruct(SC_ATTACK{ task.sessionID, task.targetID, damage, monsterTarget->hp });
		BroadcastAll(map->GetID(), p);
		_packetPool.Free(p);

		if (monsterTarget->hp == 0)
		{
			monsterTarget->isDead       = true;
			monsterTarget->respawnTimer = MONSTER_RESPAWN_SEC;
			monsterTarget->state        = Monster::State::idle;
			monsterTarget->target       = 0;
			monsterTarget->isMoving     = false;
			monsterTarget->path.clear();

			Packet* dp = _packetPool.Alloc();
			dp->Clear();
			dp->SetType(PKT_SC_NPC_DESPAWN);
			dp->WriteStruct(SC_NPC_DESPAWN{ monsterTarget->GetID() });
			BroadcastAll(map->GetID(), dp);
			_packetPool.Free(dp);
		}
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
		player->posX = player->destX = task.spawnX;
		player->posY = player->destY = task.spawnY;
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
		// 새 맵 몬스터 목록
		for (auto& monster : newMap->GetMonsters())
		{
			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_NPC_SPAWN);
			p->WriteStruct(SC_NPC_SPAWN{ monster->GetID(), monster->GetTemplateID(), monster->posX, monster->posY, monster->hp, monster->maxHp });
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
						player->posX = jobQueue->pendingStop.curX;
						player->posY = jobQueue->pendingStop.curY;

						float sdx  = jobQueue->pendingStop.curX - player->destX;
						float sdy  = jobQueue->pendingStop.curY - player->destY;
						bool  atDest = (sdx * sdx + sdy * sdy) < 0.1f * 0.1f;
						if (atDest)
						{
							player->destX    = jobQueue->pendingStop.curX;
							player->destY    = jobQueue->pendingStop.curY;
							player->isMoving = false;
						}
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

			jobQueue->pendingMove.dirty.store(false, std::memory_order_release);

			if (_gridMap.IsLoaded() && !_gridMap.IsWalkableWorld(jobQueue->pendingMove.destX, jobQueue->pendingMove.destY))
			{
				Packet* corr = _packetPool.Alloc();
				corr->Clear();
				corr->SetType(PKT_SC_MOVE_CORRECT);
				corr->WriteStruct(SC_MOVE_CORRECT{ player->posX, player->posY });
				SendPacket(sessionID, corr);
				_packetPool.Free(corr);
				continue;
			}

			{
				uint64_t now     = ::GetTickCount64();
				float    elapsed = (player->lastMoveTime > 0)
				                 ? static_cast<float>(now - player->lastMoveTime) / 1000.f
				                 : 1.f;
				float cdx        = jobQueue->pendingMove.curX - player->posX;
				float cdy        = jobQueue->pendingMove.curY - player->posY;
				float clientDist = sqrtf(cdx * cdx + cdy * cdy);
				float maxAllowed = player->speed * elapsed * 1.5f;

				if (clientDist > maxAllowed)
				{
					Packet* corr = _packetPool.Alloc();
					corr->Clear();
					corr->SetType(PKT_SC_MOVE_CORRECT);
					corr->WriteStruct(SC_MOVE_CORRECT{ player->posX, player->posY });
					SendPacket(sessionID, corr);
					_packetPool.Free(corr);
				}
				else
				{
					player->posX = jobQueue->pendingMove.curX;
					player->posY = jobQueue->pendingMove.curY;
				}
				player->lastMoveTime = now;
			}

			player->destX    = jobQueue->pendingMove.destX;
			player->destY    = jobQueue->pendingMove.destY;
			player->isMoving = true;
			// SC_MOVE는 posX/posY 기준으로 전송 — 보정됐으면 서버 위치, 수용됐으면 클라 위치

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

		for (auto& monster : map->GetMonsters())
			UpdateMonsterFSM(map.get(), monster.get(), dt);
	}
}

uint64_t GameServer::FindNearestPlayerInAggro(GameMap* map, const Monster* monster) const
{
	uint64_t nearestID   = 0;
	float    nearestDist = MONSTER_AGGRO_RANGE;

	for (auto& [id, player] : map->GetPlayers())
	{
		float dx   = player->posX - monster->posX;
		float dy   = player->posY - monster->posY;
		float dist = sqrtf(dx * dx + dy * dy);
		if (dist >= nearestDist) continue;
		if (_gridMap.IsLoaded() && !_gridMap.HasLOS(monster->posX, monster->posY, player->posX, player->posY)) continue;
		nearestDist = dist;
		nearestID   = id;
	}
	return nearestID;
}

void GameServer::RecalcMonsterPath(Monster* monster, const Player* target)
{
	AStar::Path newPath;
	if (_gridMap.IsLoaded() &&
		AStar::FindPath(_gridMap, monster->posX, monster->posY, target->posX, target->posY, newPath) &&
		!newPath.empty())
	{
		monster->path      = std::move(newPath);
		monster->pathIndex = 0;
	}
	else
	{
		monster->path      = {{ target->posX, target->posY }};
		monster->pathIndex = 0;
	}
}

void GameServer::RecalcMonsterPathToSpawn(Monster* monster)
{
	AStar::Path newPath;
	if (_gridMap.IsLoaded() &&
		AStar::FindPath(_gridMap, monster->posX, monster->posY, monster->GetSpawnX(), monster->GetSpawnY(), newPath) &&
		!newPath.empty())
	{
		monster->path      = std::move(newPath);
		monster->pathIndex = 0;
	}
	else
	{
		monster->path      = {{ monster->GetSpawnX(), monster->GetSpawnY() }};
		monster->pathIndex = 0;
	}
}

void GameServer::BroadcastNpcMove(GameMap* map, const Monster* monster)
{
	float destX = monster->posX;
	float destY = monster->posY;
	if (monster->pathIndex < static_cast<int>(monster->path.size()))
	{
		destX = monster->path[monster->pathIndex].first;
		destY = monster->path[monster->pathIndex].second;
	}

	Packet* p = _packetPool.Alloc();
	p->Clear();
	p->SetType(PKT_SC_NPC_MOVE);
	p->WriteStruct(SC_NPC_MOVE{ monster->GetID(), monster->posX, monster->posY, destX, destY, monster->speed });
	BroadcastAll(map->GetID(), p);
	_packetPool.Free(p);
}

void GameServer::UpdateMonsterFSM(GameMap* map, Monster* monster, float dt)
{
	if (monster->isDead)
	{
		monster->respawnTimer -= dt;
		if (monster->respawnTimer <= 0.f)
		{
			monster->isDead   = false;
			monster->hp       = monster->maxHp;
			monster->posX     = monster->GetSpawnX();
			monster->posY     = monster->GetSpawnY();
			monster->state    = Monster::State::idle;
			monster->target   = 0;
			monster->isMoving = false;
			monster->path.clear();

			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_NPC_SPAWN);
			p->WriteStruct(SC_NPC_SPAWN{ monster->GetID(), monster->GetTemplateID(), monster->posX, monster->posY, monster->hp, monster->maxHp });
			BroadcastAll(map->GetID(), p);
			_packetPool.Free(p);
		}
		return;
	}

	auto advancePath = [&]() -> bool
	{
		if (monster->pathIndex >= static_cast<int>(monster->path.size())) return false;
		float tx   = monster->path[monster->pathIndex].first;
		float ty   = monster->path[monster->pathIndex].second;
		float pdx  = tx - monster->posX;
		float pdy  = ty - monster->posY;
		float dist = sqrtf(pdx * pdx + pdy * pdy);
		float step = monster->speed * dt;
		if (dist <= step)
		{
			monster->posX = tx;
			monster->posY = ty;
			monster->pathIndex++;
			return true;
		}
		monster->posX += pdx / dist * step;
		monster->posY += pdy / dist * step;
		return false;
	};

	switch (monster->state)
	{
	case Monster::State::idle:
	{
		uint64_t targetID = FindNearestPlayerInAggro(map, monster);
		if (targetID == 0) break;
		Player* target = map->FindPlayer(targetID);
		if (!target) break;

		monster->target = targetID;
		monster->state  = Monster::State::chase;
		monster->pathRecalcTimer = 0.f;
		RecalcMonsterPath(monster, target);
		monster->isMoving = !monster->path.empty();
		BroadcastNpcMove(map, monster);
		break;
	}
	case Monster::State::chase:
	{
		Player* target = map->FindPlayer(monster->target);
		if (!target)
		{
			monster->state = Monster::State::returnToSpawn;
			RecalcMonsterPathToSpawn(monster);
			monster->isMoving = true;
			BroadcastNpcMove(map, monster);
			break;
		}

		float dx            = target->posX - monster->posX;
		float dy            = target->posY - monster->posY;
		float distToTarget  = sqrtf(dx * dx + dy * dy);
		float sdx           = monster->posX - monster->GetSpawnX();
		float sdy           = monster->posY - monster->GetSpawnY();
		float distFromSpawn = sqrtf(sdx * sdx + sdy * sdy);

		if (distFromSpawn > MONSTER_LEASH_RANGE)
		{
			monster->state = Monster::State::returnToSpawn;
			RecalcMonsterPathToSpawn(monster);
			monster->isMoving = true;
			BroadcastNpcMove(map, monster);
			break;
		}

		if (distToTarget <= MONSTER_ATTACK_RANGE)
		{
			monster->state        = Monster::State::attack;
			monster->attackCooldown = 0.f;
			monster->isMoving     = false;
			monster->path.clear();
			break;
		}

		monster->pathRecalcTimer -= dt;
		bool exhausted = monster->pathIndex >= static_cast<int>(monster->path.size());
		if (monster->pathRecalcTimer <= 0.f || exhausted)
		{
			RecalcMonsterPath(monster, target);
			monster->pathRecalcTimer = MONSTER_PATH_RECALC_SEC;
			monster->isMoving = !monster->path.empty();
			BroadcastNpcMove(map, monster);
		}

		if (advancePath() && monster->pathIndex < static_cast<int>(monster->path.size()))
			BroadcastNpcMove(map, monster);
		break;
	}
	case Monster::State::attack:
	{
		Player* target = map->FindPlayer(monster->target);
		if (!target)
		{
			monster->state = Monster::State::returnToSpawn;
			RecalcMonsterPathToSpawn(monster);
			monster->isMoving = true;
			BroadcastNpcMove(map, monster);
			break;
		}

		float dx           = target->posX - monster->posX;
		float dy           = target->posY - monster->posY;
		float distToTarget = sqrtf(dx * dx + dy * dy);

		if (distToTarget > MONSTER_ATTACK_RANGE)
		{
			monster->state = Monster::State::chase;
			monster->pathRecalcTimer = 0.f;
			break;
		}

		monster->attackCooldown -= dt;
		if (monster->attackCooldown <= 0.f)
		{
			monster->attackCooldown = MONSTER_ATTACK_COOLDOWN;
			constexpr int32_t monsterDamage = 5;
			target->hp -= monsterDamage;
			if (target->hp < 0) target->hp = 0;

			Packet* p = _packetPool.Alloc();
			p->Clear();
			p->SetType(PKT_SC_NPC_ATTACK);
			p->WriteStruct(SC_NPC_ATTACK{ monster->GetID(), monster->target, monsterDamage, target->hp });
			BroadcastAll(map->GetID(), p);
			_packetPool.Free(p);
		}
		break;
	}
	case Monster::State::returnToSpawn:
	{
		if (monster->pathIndex >= static_cast<int>(monster->path.size()))
		{
			monster->posX     = monster->GetSpawnX();
			monster->posY     = monster->GetSpawnY();
			monster->hp       = monster->maxHp;
			monster->state    = Monster::State::idle;
			monster->target   = 0;
			monster->isMoving = false;
			monster->path.clear();
			BroadcastNpcMove(map, monster);
			break;
		}

		if (advancePath() && monster->pathIndex < static_cast<int>(monster->path.size()))
			BroadcastNpcMove(map, monster);
		break;
	}
	}
}
