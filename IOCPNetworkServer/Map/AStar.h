#pragma once
#include <vector>
#include <utility>

class GridMap;

class AStar
{
public:
    using Path = std::vector<std::pair<float, float>>;

    static bool FindPath(const GridMap& grid,
                         float startX, float startY,
                         float endX,   float endY,
                         Path& outPath);

private:
    static Path Simplify(const Path& raw);
};
