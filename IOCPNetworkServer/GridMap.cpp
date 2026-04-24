#include "GridMap.h"
#include "ServerConfig.h"
#include <fstream>

bool GridMap::Load(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) return false;

    file >> _width >> _height;
    if (_width <= 0 || _height <= 0) return false;

    _tiles.assign(_height, std::vector<uint8_t>(_width, 0));

    for (int y = 0; y < _height; y++)
        for (int x = 0; x < _width; x++) {
            int val;
            file >> val;
            _tiles[y][x] = static_cast<uint8_t>(val != 0 ? 1 : 0);
        }

    return true;
}

bool GridMap::IsWalkable(int gx, int gy) const
{
    if (gx < 0 || gy < 0 || gx >= _width || gy >= _height) return false;
    return _tiles[gy][gx] == 0;
}

bool GridMap::IsWalkableWorld(float wx, float wy) const
{
    int gx = static_cast<int>((wx - WORLD_ORIGIN_X) / TILE_SIZE);
    int gy = static_cast<int>((wy - WORLD_ORIGIN_Y) / TILE_SIZE);
    return IsWalkable(gx, gy);
}
