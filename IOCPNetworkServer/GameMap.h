#pragma once
#include <memory>
#include <unordered_map>
#include "IOCPServer.h"
#include "Player.h"
#include "ServerConfig.h"

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

    std::unordered_map<SessionID, std::unique_ptr<Player>>& GetPlayers() { return _players; }

private:
    MapID _mapID;
    std::unordered_map<SessionID, std::unique_ptr<Player>> _players;
};
