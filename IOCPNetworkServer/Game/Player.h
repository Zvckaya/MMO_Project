#pragma once
#include <array>
#include <cstdint>
#include <string>
#include "Creature.h"
#include "ServerConfig.h"

using PlayerID = uint64_t;

struct InventorySlot {
    uint16_t itemID = 0;
    int32_t  count  = 0;
};

class Player : public Creature
{
public:
    Player(PlayerID playerID, uint64_t accountId, std::string displayName)
        : _playerID(playerID)
        , _accountId(accountId)
        , _displayName(std::move(displayName))
    {}

    uint64_t           GetAccountId()   const { return _accountId; }
    const std::string& GetDisplayName() const { return _displayName; }

    uint64_t lastMoveTime = 0;
    std::array<InventorySlot, MAX_INVENTORY_SLOTS> inventory{};

private:
    PlayerID    _playerID;
    uint64_t    _accountId;
    std::string _displayName;
};
