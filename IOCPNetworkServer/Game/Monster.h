#pragma once
#include <cstdint>
#include <vector>
#include <utility>
#include "Creature.h"

using MonsterID = uint64_t;

class Monster : public Creature
{
public:
    enum class State { idle, chase, attack, returnToSpawn };

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

    State    state           = State::idle;
    uint64_t target          = 0;
    float    attackCooldown  = 0.f;
    float    pathRecalcTimer = 0.f;
    bool     isDead          = false;
    float    respawnTimer    = 0.f;

    std::vector<std::pair<float, float>> path;
    int pathIndex = 0;
    int lastKnownTargetGridX = -1;
    int lastKnownTargetGridY = -1;

private:
    MonsterID _id;
    uint16_t  _templateID;
    float     _spawnX;
    float     _spawnY;
};
