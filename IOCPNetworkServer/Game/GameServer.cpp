#include "GameServer.h"
#include "AuthClient.h"
#include "AStar.h"
#include "Logger.h"
#include <chrono>
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>
#include <Windows.h>

static std::unordered_map<uint16_t, ItemData, std::hash<uint16_t>> LoadItemDataFromFile(const std::string& path)
{
	std::unordered_map<uint16_t, ItemData, std::hash<uint16_t>> result;
	std::ifstream file(path);
	if (!file.is_open()) return result;

	std::string line;
	while (std::getline(file, line))
	{
		if (line.empty()) continue;
		std::istringstream ss(line);
		std::string idStr, name, typeStr, maxStackStr;
		std::getline(ss, idStr,      '\t');
		std::getline(ss, name,       '\t');
		std::getline(ss, typeStr,    '\t');
		std::getline(ss, maxStackStr);
		if (idStr.empty() || name.empty() || typeStr.empty() || maxStackStr.empty()) continue;
		if (!maxStackStr.empty() && maxStackStr.back() == '\r') maxStackStr.pop_back();

		ItemData item;
		item.itemID   = static_cast<uint16_t>(std::stoul(idStr));
		item.name     = std::move(name);
		item.type     = static_cast<uint8_t>(std::stoul(typeStr));
		item.maxStack = std::stoi(maxStackStr);
		result.emplace(item.itemID, std::move(item));
	}

	Log(L"ItemData", Logger::Level::SYSTEM, L"ItemData loaded: %zu items", result.size());
	return result;
}

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

	//맵 데이터 파일을 순회
	for (auto& [mapID, gameMap] : _maps) 
	{
		char filename[64];
		snprintf(filename, sizeof(filename), "maps/map_%03u.txt", mapID); 
		GridMap gm;
		if (gm.Load(filename)) //Load안에서 맵 데이터를 읽어 spawnpoint 추가 
		{
			for (auto& [sx, sy] : gm.GetSpawnPoints()) //스폰 포인트에서 몬스터 생성 
				gameMap->SpawnMonster(++_monsterIDCounter, 1, sx, sy);
		}
		else
			Log(L"GridMap", Logger::Level::WARN, L"maps/map_%03u.txt not found — wall check disabled for mapID=%u", mapID, mapID);
		_gridMaps.emplace(mapID, std::move(gm));
	}

	_itemDataMap = LoadItemDataFromFile("data/items.txt");

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

	if (!jobQueue->isAuthenticated.load(std::memory_order_acquire) && //미인증 세션 disconnect
		packet->GetType() != PKT_CS_LOGIN_AUTH)
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
	}); //정상처리
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

	jobQueue->Post([this, sessionID, auth, jobQueue]()
	{
		uint32_t    errorCode   = auth.valid ? AUTH_OK : AUTH_ERR_INVALID;
		uint64_t    accountId   = auth.valid ? auth.accountId : 0ULL;
		std::string displayName = auth.valid ? auth.displayName : "";

		if (auth.valid)
		{
			jobQueue->isAuthenticated.store(true, std::memory_order_release);
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
			EnqueueDBRequest({ //db에 요청 
				.type      = DBRequest::Type::loadPlayer,
				.accountId = accountId,
				.onLoad    = [this, sessionID, accountId, displayName](const PlayerDBData& dbData)
				{ //로드시 완료될 콜백 
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
		HandlePlayerAuth(task);
		break;
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

		float dirX = task.curX;
		float dirY = task.curY;
		float dirLen = sqrtf(dirX * dirX + dirY * dirY);
		if (dirLen < 0.001f) break;
		dirX /= dirLen;
		dirY /= dirLen;

		constexpr int32_t damage = 10;
		bool anyHit = false;

		for (auto& monster : map->GetMonsters())
		{
			if (monster->isDead) continue;

			float dx   = monster->posX - attacker->posX;
			float dy   = monster->posY - attacker->posY;
			float dist = sqrtf(dx * dx + dy * dy);
			if (dist > ATTACK_RANGE) continue;

			float dot = (dist > 0.001f) ? (dx / dist * dirX + dy / dist * dirY) : 1.f;
			if (dot < ATTACK_CONE_COS) continue;

			monster->hp -= damage;
			if (monster->hp < 0) monster->hp = 0;
			anyHit = true;

			BroadcastTo(map->GetID(), PKT_SC_ATTACK, SC_ATTACK{ task.sessionID, dirX, dirY, monster->GetID(), damage, monster->hp });

			if (monster->hp > 0)
			{
				monster->target     = task.sessionID;
				monster->state      = Monster::State::stun;
				monster->stunTimer  = 1.f;
				monster->isMoving   = false;
				monster->path.clear();
				BroadcastNpcMove(map, monster.get());
			}

			if (monster->hp == 0)
			{
				monster->isDead       = true;
				monster->respawnTimer = MONSTER_RESPAWN_SEC;
				monster->state        = Monster::State::idle;
				monster->target       = 0;
				monster->isMoving     = false;
				monster->path.clear();

				BroadcastTo(map->GetID(), PKT_SC_NPC_DESPAWN, SC_NPC_DESPAWN{ monster->GetID() });

				static thread_local std::mt19937 rng{ std::random_device{}() };
				constexpr uint16_t dropTable[] = { 2001, 2002, 2003 };
				uint16_t dropItemID = dropTable[std::uniform_int_distribution<int>(0, 2)(rng)];
				uint64_t uid = map->SpawnWorldItem(dropItemID, 1, monster->posX, monster->posY);
				BroadcastTo(map->GetID(), PKT_SC_ITEM_APPEAR, SC_ITEM_APPEAR{ uid, dropItemID, monster->posX, monster->posY, 1 });
			}
		}

		if (!anyHit)
			BroadcastTo(map->GetID(), PKT_SC_ATTACK, SC_ATTACK{ task.sessionID, dirX, dirY, 0ULL, 0, 0 });
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
		player->isDirty = true;

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
		player->isDirty = true;

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
		if (dx * dx + dy * dy > ITEM_PICKUP_RANGE * ITEM_PICKUP_RANGE) break;

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
		player->isDirty = true;
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
			playerOwnership = oldMap->TakePlayer(task.sessionID); //플레이어 소유권 획득 
			if (playerOwnership)
			{
				Packet* p = _packetPool.Alloc();
				p->Clear();
				p->SetType(PKT_SC_DESPAWN);
				p->WriteStruct(SC_DESPAWN{ task.sessionID });
				BroadcastExcept(oldMap->GetID(), task.sessionID, p);
				_packetPool.Free(p);
			} //예전 맵에 플레이어 삭제-> 브로드캐스트 
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

		{
			std::vector<InventorySlotData> slots;
			for (uint16_t i = 0; i < MAX_INVENTORY_SLOTS; ++i)
			{
				const auto& slot = player->inventory[i];
				if (slot.itemID == 0 || slot.count <= 0) continue;
				slots.push_back({ i, slot.itemID, slot.count });
			}

			EnqueueDBRequest({
				.type      = DBRequest::Type::savePlayer,
				.accountId = player->GetAccountId(),
				.saveData  = { true, player->posX, player->posY, player->hp, task.targetMapID }
			});

			EnqueueDBRequest({
				.type          = DBRequest::Type::saveInventory,
				.accountId     = player->GetAccountId(),
				.inventoryData = std::move(slots)
			});
			player->isDirty = false;
		}

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

			const GridMap* gridMap = map ? FindGridMap(map->GetID()) : nullptr;
			if (gridMap && (!gridMap->IsWalkableWorld(jobQueue->pendingMove.destX, jobQueue->pendingMove.destY) ||
				!gridMap->HasLOS(player->posX, player->posY, jobQueue->pendingMove.destX, jobQueue->pendingMove.destY)))
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


	_saveTimer += deltaMs;


	if (_saveTimer >= static_cast<int64_t>(PERIODIC_SAVE_INTERVAL_MS)) 
	{
		_saveTimer = 0;
		for (auto& [mapID, map] : _maps)
		{
			for (auto& [sessionID, player] : map->GetPlayers())
			{
				if (!player->isDirty) continue; //아이템 변경 x 
				player->isDirty = false; 

				std::vector<InventorySlotData> slots;
				for (uint16_t i = 0; i < MAX_INVENTORY_SLOTS; ++i)
				{
					const auto& slot = player->inventory[i];
					if (slot.itemID == 0 || slot.count <= 0) continue;
					slots.push_back({ i, slot.itemID, slot.count });
				}
				EnqueueDBRequest({ //db에 인벤토리 저장 요청 
					.type          = DBRequest::Type::saveInventory,
					.accountId     = player->GetAccountId(),
					.inventoryData = std::move(slots)
				});
			}
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

const GridMap* GameServer::FindGridMap(MapID id) const
{
	auto it = _gridMaps.find(id);
	return (it != _gridMaps.end() && it->second.IsLoaded()) ? &it->second : nullptr;
}

uint64_t GameServer::FindNearestPlayerInAggro(GameMap* map, const Monster* monster, const GridMap* gridMap) const
{
	uint64_t nearestID     = 0;
	float    nearestDistSq = MONSTER_AGGRO_RANGE * MONSTER_AGGRO_RANGE; //인지거리 

	for (auto& [id, player] : map->GetPlayers()) //맵 플레이어 순회 
	{
		float dx     = player->posX - monster->posX;
		float dy     = player->posY - monster->posY;
		float distSq = dx * dx + dy * dy; //플레이어와 거리 
		if (distSq >= nearestDistSq) continue;
		//Bresenham 알고리즘으로 직선상의 타일 순회하여 벽을 검사 
		if (gridMap && !gridMap->HasLOS(monster->posX, monster->posY, player->posX, player->posY)) continue;
		nearestDistSq = distSq; 
		nearestID     = id;// 추적할 플레이어 갱신 
	}
	return nearestID;
}

void GameServer::RecalcMonsterPath(Monster* monster, const Player* target, const GridMap* gridMap)
{
	if (!gridMap) return;

	AStar::Path newPath;
	if (AStar::FindPath(*gridMap, monster->posX, monster->posY, target->posX, target->posY, newPath) &&
		!newPath.empty())
	{
		if (newPath.size() > 1)
		{
			float dx = newPath[0].first  - monster->posX;
			float dy = newPath[0].second - monster->posY;
			if (dx * dx + dy * dy < TILE_SIZE * TILE_SIZE)
				newPath.erase(newPath.begin());
		}
		std::wstring pts;
		for (auto& [x, y] : newPath)
		{
			wchar_t buf[32];
			swprintf_s(buf, L"(%.1f,%.1f) ", x, y);
			pts += buf;
		}
		Log(L"Monster", Logger::Level::DEBUG, L"[%llu] path(%zu): %s", monster->GetID(), newPath.size(), pts.c_str());
		monster->path      = std::move(newPath);
		monster->pathIndex = 0;
	}
}

void GameServer::RecalcMonsterPathToSpawn(Monster* monster, const GridMap* gridMap)
{
	if (!gridMap) return;
	AStar::Path newPath;
	if (AStar::FindPath(*gridMap, monster->posX, monster->posY, monster->GetSpawnX(), monster->GetSpawnY(), newPath) &&
		!newPath.empty())
	{
		if (newPath.size() > 1)
		{
			float dx = newPath[0].first  - monster->posX;
			float dy = newPath[0].second - monster->posY;
			if (dx * dx + dy * dy < TILE_SIZE * TILE_SIZE)
				newPath.erase(newPath.begin());
		}
		monster->path      = std::move(newPath);
		monster->pathIndex = 0;
	}
	else
	{
		monster->path.clear();
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
	const GridMap* gridMap = FindGridMap(map->GetID());

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

			BroadcastTo(map->GetID(), PKT_SC_NPC_SPAWN, SC_NPC_SPAWN{ monster->GetID(), monster->GetTemplateID(), monster->posX, monster->posY, monster->hp, monster->maxHp });
		}
		return;
	}

	//다음 경로로 도착했는지 확인 
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
		uint64_t targetID = FindNearestPlayerInAggro(map, monster, gridMap);
		if (targetID == 0) break;
		Player* target = map->FindPlayer(targetID);
		if (!target) break;

		monster->target = targetID;
		monster->state  = Monster::State::chase;
		monster->pathRecalcTimer = 0.f;
		monster->lastKnownTargetGridX = static_cast<int>(target->posX / TILE_SIZE);
		monster->lastKnownTargetGridY = static_cast<int>(target->posY / TILE_SIZE);
		RecalcMonsterPath(monster, target, gridMap);
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
			monster->target = 0;
			RecalcMonsterPathToSpawn(monster, gridMap);
			monster->isMoving = true;
			BroadcastNpcMove(map, monster);
			break;
		}

		float dx              = target->posX - monster->posX;
		float dy              = target->posY - monster->posY;
		float distToTargetSq  = dx * dx + dy * dy; //공격 범위 확인 
		float sdx             = monster->posX - monster->GetSpawnX();
		float sdy             = monster->posY - monster->GetSpawnY();
		float distFromSpawnSq = sdx * sdx + sdy * sdy; //스폰 지점까지 거리 확인 

		if (distFromSpawnSq > MONSTER_LEASH_RANGE * MONSTER_LEASH_RANGE || distToTargetSq > MONSTER_AGGRO_RANGE * MONSTER_AGGRO_RANGE)
		{
			monster->state    = Monster::State::returnToSpawn;
			monster->target   = 0;
			RecalcMonsterPathToSpawn(monster, gridMap);
			monster->isMoving = true;
			BroadcastNpcMove(map, monster);
			break;
		}

		if (distToTargetSq <= MONSTER_ATTACK_RANGE * MONSTER_ATTACK_RANGE)
		{
			monster->state        = Monster::State::attack;
			monster->attackCooldown = 0.f;
			monster->isMoving     = false;
			monster->path.clear();
			break;
		}

		monster->pathRecalcTimer -= dt;

		bool exhausted = monster->pathIndex >= static_cast<int>(monster->path.size());

		//타겟의 flaot 좌표를 정규화 
		int tgx = static_cast<int>(target->posX / TILE_SIZE);
		int tgy = static_cast<int>(target->posY / TILE_SIZE);

		//타겟이 움직였나?
		bool targetMoved = (tgx != monster->lastKnownTargetGridX || tgy != monster->lastKnownTargetGridY);
		//움직였을때만 경로 재탐색 
		if (exhausted || (monster->pathRecalcTimer <= 0.f && targetMoved)) 
		{
			monster->lastKnownTargetGridX = tgx;
			monster->lastKnownTargetGridY = tgy;
			RecalcMonsterPath(monster, target, gridMap);
			monster->pathRecalcTimer = MONSTER_PATH_RECALC_SEC;
			monster->isMoving = !monster->path.empty();
			BroadcastNpcMove(map, monster);
		}
		else if (monster->pathRecalcTimer <= 0.f) //아니 면 
		{
			monster->pathRecalcTimer = MONSTER_PATH_RECALC_SEC;
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
			Log(L"Monster", Logger::Level::DEBUG, L"[%llu] target lost — returning to spawn (%.1f,%.1f)",
				monster->GetID(), monster->GetSpawnX(), monster->GetSpawnY());
			monster->state = Monster::State::returnToSpawn;
			RecalcMonsterPathToSpawn(monster, gridMap);
			monster->isMoving = true;
			BroadcastNpcMove(map, monster);
			break;
		}

		float dx             = target->posX - monster->posX;
		float dy             = target->posY - monster->posY;
		float distToTargetSq = dx * dx + dy * dy;

		if (distToTargetSq > MONSTER_ATTACK_RANGE * MONSTER_ATTACK_RANGE)
		{
			monster->state           = Monster::State::chase;
			monster->pathRecalcTimer = 0.f;
			RecalcMonsterPath(monster, target, gridMap);
			monster->isMoving = !monster->path.empty();
			BroadcastNpcMove(map, monster);
			break;
		}

		monster->attackCooldown -= dt;
		if (monster->attackCooldown <= 0.f)
		{
			monster->attackCooldown = MONSTER_ATTACK_COOLDOWN;
			constexpr int32_t monsterDamage = 5;
			target->hp -= monsterDamage;
			if (target->hp < 0) target->hp = 0;

			BroadcastTo(map->GetID(), PKT_SC_NPC_ATTACK, SC_NPC_ATTACK{ monster->GetID(), monster->target, monsterDamage, target->hp });
		}
		break;
	}
	case Monster::State::stun:
	{
		monster->stunTimer -= dt;
		if (monster->stunTimer <= 0.f)
		{
			if (monster->target != 0 && map->FindPlayer(monster->target))
			{
				monster->state           = Monster::State::chase;
				monster->pathRecalcTimer = 0.f;
			}
			else
			{
				monster->target = 0;
				monster->state  = Monster::State::idle;
			}
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

		monster->pathRecalcTimer -= dt;
		if (monster->pathRecalcTimer <= 0.f)
		{
			monster->pathRecalcTimer = MONSTER_PATH_RECALC_SEC;
			BroadcastNpcMove(map, monster);
		}

		if (advancePath() && monster->pathIndex < static_cast<int>(monster->path.size()))
			BroadcastNpcMove(map, monster);
		break;
	}
	}
}

void GameServer::HandlePlayerAuth(const FrameTask& task)
{
	auto mapIt = _maps.find(task.mapID);
	if (mapIt == _maps.end()) return;
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

	SendTo(task.sessionID, PKT_SC_MAP_INFO, SC_MAP_INFO{ task.mapID });

	for (auto& [uid, worldItem] : map->GetWorldItems())
		SendTo(task.sessionID, PKT_SC_ITEM_APPEAR, SC_ITEM_APPEAR{ uid, worldItem.itemID, worldItem.posX, worldItem.posY, worldItem.count });

	for (auto& monster : map->GetMonsters())
		SendTo(task.sessionID, PKT_SC_NPC_SPAWN, SC_NPC_SPAWN{ monster->GetID(), monster->GetTemplateID(), monster->posX, monster->posY, monster->hp, monster->maxHp });

	for (uint16_t i = 0; i < MAX_INVENTORY_SLOTS; ++i)
	{
		const auto& slot = newPlayer->inventory[i];
		if (slot.itemID == 0) continue;
		SendTo(task.sessionID, PKT_SC_INVENTORY_UPD, SC_INVENTORY_UPD{ i, slot.itemID, static_cast<uint16_t>(slot.count) });
	}

	{
		Packet* p = _packetPool.Alloc();
		p->Clear();
		p->SetType(PKT_SC_CREATE_MY_CHARACTER);
		p->WriteStruct(SC_CREATE_MY_CHARACTER{ task.sessionID, newPlayer->posX, newPlayer->posY, newPlayer->hp, newPlayer->maxHp, newPlayer->speed, static_cast<uint16_t>(task.displayName.size()) });
		p->PutData(task.displayName.c_str(), static_cast<int>(task.displayName.size()));
		SendPacket(task.sessionID, p);
		_packetPool.Free(p);
	}

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

	{
		Packet* p = _packetPool.Alloc();
		p->Clear();
		p->SetType(PKT_SC_CREATE_OTHER_CHARACTER);
		p->WriteStruct(SC_CREATE_OTHER_CHARACTER{ task.sessionID, newPlayer->posX, newPlayer->posY, newPlayer->hp, newPlayer->maxHp, newPlayer->speed, static_cast<uint16_t>(task.displayName.size()) });
		p->PutData(task.displayName.c_str(), static_cast<int>(task.displayName.size()));
		BroadcastExcept(task.mapID, task.sessionID, p);
		_packetPool.Free(p);
	}

	SendTo(task.sessionID, PKT_SC_WORLD_ENTER);
}
