#pragma once
#include <cstdint>

using MonsterID = uint64_t;

class Monster
{
public:
    Monster(MonsterID id, uint16_t templateID, float spawnX, float spawnY)
        : _id(id)
        , _templateID(templateID)
        , _spawnX(spawnX)
        , _spawnY(spawnY)
    {
        posX = spawnX;
        posY = spawnY;
    }

    MonsterID GetID()         const { return _id; }
    uint16_t  GetTemplateID() const { return _templateID; }
    float     GetSpawnX()     const { return _spawnX; }
    float     GetSpawnY()     const { return _spawnY; }

    float   posX = 0.f, posY = 0.f;
    float   destX = 0.f, destY = 0.f;
    float   speed = 3.f;
    bool    isMoving = false;
    int32_t hp    = 100;
    int32_t maxHp = 100;

private:
    MonsterID _id;
    uint16_t  _templateID;
    float     _spawnX;
    float     _spawnY;
};
