#pragma once
#include <string>
#include <utility>
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

    const std::vector<std::pair<float, float>>& GetSpawnPoints() const { return _spawnPoints; }

private:
    std::vector<std::vector<uint8_t>>    _tiles;
    std::vector<std::pair<float, float>> _spawnPoints;
    int _width  = 0;
    int _height = 0;
};
