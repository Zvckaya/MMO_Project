#pragma once
#include <memory>
#include <unordered_map>
#include "IOCPServer.h"
#include "Player.h"
#include "ServerConfig.h"

struct WorldItem {
    uint64_t itemUID   = 0;
    uint16_t itemID    = 0;
    int32_t  count     = 0;
    float    posX      = 0.f, posY = 0.f;
    uint64_t spawnTick = 0;
};

class GameMap
{
public:
    explicit GameMap(MapID id) : _mapID(id) {}

    MapID   GetID() const { return _mapID; }

    Player* FindPlayer(SessionID id) const
    {
        auto it = _players.find(id);
        return it != _players.end() ? it->second.get() : nullptr;
    }

    void AddPlayer(SessionID id, std::unique_ptr<Player> player) { _players.emplace(id, std::move(player)); }
    void RemovePlayer(SessionID id)                              { _players.erase(id); }

    std::unique_ptr<Player> TakePlayer(SessionID id)
    {
        auto it = _players.find(id);
        if (it == _players.end()) return nullptr;
        auto player = std::move(it->second);
        _players.erase(it);
        return player;
    }

    std::unordered_map<SessionID, std::unique_ptr<Player>>& GetPlayers() { return _players; }

    uint64_t SpawnWorldItem(uint16_t itemID, int32_t count, float posX, float posY)
    {
        uint64_t uid = ++_itemUIDCounter;
        _worldItems.emplace(uid, WorldItem{ uid, itemID, count, posX, posY, ::GetTickCount64() });
        return uid;
    }

    WorldItem* FindWorldItem(uint64_t uid)
    {
        auto it = _worldItems.find(uid);
        return it != _worldItems.end() ? &it->second : nullptr;
    }

    void RemoveWorldItem(uint64_t uid) { _worldItems.erase(uid); }

    std::unordered_map<uint64_t, WorldItem>& GetWorldItems() { return _worldItems; }

private:
    MapID    _mapID;
    uint64_t _itemUIDCounter = 0;
    std::unordered_map<SessionID, std::unique_ptr<Player>> _players;
    std::unordered_map<uint64_t, WorldItem>                _worldItems;
};
