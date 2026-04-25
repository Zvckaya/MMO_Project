#pragma once
#include <cstdint>
#include <vector>
#include <utility>
#include "Creature.h"

using MonsterID = uint64_t;

class Monster : public Creature
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
        speed = 3.f;
    }

    MonsterID GetID()         const { return _id; }
    uint16_t  GetTemplateID() const { return _templateID; }
    float     GetSpawnX()     const { return _spawnX; }
    float     GetSpawnY()     const { return _spawnY; }

    std::vector<std::pair<float, float>> path;
    int pathIndex = 0;

private:
    MonsterID _id;
    uint16_t  _templateID;
    float     _spawnX;
    float     _spawnY;
};
