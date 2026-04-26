#include "GridMap.h"
#include "ServerConfig.h"
#include <fstream>
#include <cmath>

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
    if (gx < 0 || gy < 0 || gx >= _width || gy >= _height) return true;
    return _tiles[gy][gx] == 0;
}

bool GridMap::HasLOS(float ax, float ay, float bx, float by) const
{
    int x0 = static_cast<int>(ax / TILE_SIZE);
    int y0 = static_cast<int>(ay / TILE_SIZE);
    int x1 = static_cast<int>(bx / TILE_SIZE);
    int y1 = static_cast<int>(by / TILE_SIZE);

    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    while (true)
    {
        if (!IsWalkable(x0, y0)) return false;
        if (x0 == x1 && y0 == y1) return true;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

bool GridMap::HasClearance(int gx, int gy) const
{
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if ((dx != 0 || dy != 0) && !IsWalkable(gx + dx, gy + dy))
                return false;
    return true;
}

bool GridMap::IsWalkableWorld(float wx, float wy) const
{
    if (wx < 0.f || wy < 0.f) return true;
    int gx = static_cast<int>(wx / TILE_SIZE);
    int gy = static_cast<int>(wy / TILE_SIZE);
    return IsWalkable(gx, gy);
}
