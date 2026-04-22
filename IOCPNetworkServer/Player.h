#pragma once
#include <cstdint>
#include <string>

using PlayerID = uint64_t;

class Player
{
public:
    Player(PlayerID playerID, uint64_t accountId, std::string displayName)
        : _playerID(playerID)
        , _accountId(accountId)
        , _displayName(std::move(displayName))
    {}
    ~Player() = default;

    uint64_t           GetAccountId()   const { return _accountId; }
    const std::string& GetDisplayName() const { return _displayName; }

    float    posX  = 0.f,  posY  = 0.f;
    float    destX = 0.f,  destY = 0.f;
    float    speed = 5.f;
    bool     isMoving     = false;
    uint64_t lastMoveTime = 0;
    int32_t  hp    = 100;
    int32_t  maxHp = 100;

private:
    PlayerID    _playerID;
    uint64_t    _accountId;
    std::string _displayName;
};
