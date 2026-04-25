#pragma once
#include <cstdint>

class Creature
{
public:
    virtual ~Creature() = default;

    float   posX  = 0.f, posY  = 0.f;
    float   destX = 0.f, destY = 0.f;
    float   speed = 5.f;
    bool    isMoving = false;
    int32_t hp    = 100;
    int32_t maxHp = 100;
};
