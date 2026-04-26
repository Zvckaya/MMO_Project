#pragma once
#include <string>
#include <vector>
#include <cstdint>

class GridMap {
public:
    bool Load(const std::string& path);

    bool IsWalkableWorld(float wx, float wy) const;
    bool IsWalkable(int gx, int gy) const;
    bool HasClearance(int gx, int gy) const;
    bool HasLOS(float ax, float ay, float bx, float by) const;

    int  GetWidth()  const { return _width; }
    int  GetHeight() const { return _height; }
    bool IsLoaded()  const { return !_tiles.empty(); }

private:
    std::vector<std::vector<uint8_t>> _tiles;
    int _width  = 0;
    int _height = 0;
};
