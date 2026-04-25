#pragma once
#include <string>
#include <vector>
#include <cstdint>

class GridMap {
public:
    bool Load(const std::string& path);

    bool IsWalkableWorld(float wx, float wy) const;
    bool IsWalkable(int gx, int gy) const;

    int   GetWidth()   const { return _width; }
    int   GetHeight()  const { return _height; }
    float GetOriginX() const { return _originX; }
    float GetOriginY() const { return _originY; }
    bool  IsLoaded()   const { return !_tiles.empty(); }

private:
    std::vector<std::vector<uint8_t>> _tiles;
    int   _width   = 0;
    int   _height  = 0;
    float _originX = 0.f;
    float _originY = 0.f;
};
