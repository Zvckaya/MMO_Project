#include "AStar.h"
#include "GridMap.h"
#include "ServerConfig.h"
#include <queue>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>

bool AStar::FindPath(const GridMap& grid,
                     float startX, float startY,
                     float endX,   float endY,
                     Path& outPath)
{
    const int width  = grid.GetWidth();
    const int height = grid.GetHeight();

    const int sgx = static_cast<int>(startX / TILE_SIZE);
    const int sgy = static_cast<int>(startY  / TILE_SIZE);
    const int egx = static_cast<int>(endX   / TILE_SIZE);
    const int egy = static_cast<int>(endY   / TILE_SIZE);

    if (!grid.IsWalkable(egx, egy)) return false;
    if (sgx == egx && sgy == egy)
    {
        outPath.clear();
        outPath.push_back({ endX, endY });
        return true;
    }

    struct NodeInfo
    {
        float g          = std::numeric_limits<float>::max();
        float f          = std::numeric_limits<float>::max();
        int   parentIdx  = -1;
        bool  closed     = false;
    };

    std::vector<NodeInfo> nodes(width * height);

    auto toIdx = [width](int gx, int gy) { return gy * width + gx; };

    auto heuristic = [](int x1, int y1, int x2, int y2) -> float {
        const int dx = std::abs(x1 - x2);
        const int dy = std::abs(y1 - y2);
        return static_cast<float>(std::min(dx, dy)) * 1.414f
             + static_cast<float>(std::abs(dx - dy));
    };

    using PQEntry = std::pair<float, int>;
    std::priority_queue<PQEntry, std::vector<PQEntry>, std::greater<PQEntry>> openSet;

    const int startIdx = toIdx(sgx, sgy);
    nodes[startIdx].g = 0.f;
    nodes[startIdx].f = heuristic(sgx, sgy, egx, egy);
    openSet.push({ nodes[startIdx].f, startIdx });

    constexpr int   DDX[8]  = { 1, -1,  0,  0,  1, -1,  1, -1 };
    constexpr int   DDY[8]  = { 0,  0,  1, -1,  1, -1, -1,  1 };
    constexpr float COST[8] = { 1.f, 1.f, 1.f, 1.f, 1.414f, 1.414f, 1.414f, 1.414f };

    while (!openSet.empty())
    {
        auto [f, curIdx] = openSet.top();
        openSet.pop();

        if (nodes[curIdx].closed) continue;
        nodes[curIdx].closed = true;

        const int cx = curIdx % width;
        const int cy = curIdx / width;

        if (cx == egx && cy == egy)
        {
            Path raw;
            int nIdx = curIdx;
            while (nIdx != -1)
            {
                const int nx = nIdx % width;
                const int ny = nIdx / width;
                raw.push_back({ (nx + 0.5f) * TILE_SIZE, (ny + 0.5f) * TILE_SIZE });
                nIdx = nodes[nIdx].parentIdx;
            }
            std::reverse(raw.begin(), raw.end());
            outPath = Simplify(raw);
            return true;
        }

        for (int i = 0; i < 8; i++)
        {
            const int nx = cx + DDX[i];
            const int ny = cy + DDY[i];

            if (!grid.IsWalkable(nx, ny)) continue;

            if (i >= 4)
            {
                if (!grid.IsWalkable(cx + DDX[i], cy) ||
                    !grid.IsWalkable(cx, cy + DDY[i])) continue;
            }

            const int nIdx = toIdx(nx, ny);
            if (nodes[nIdx].closed) continue;

            const float newG = nodes[curIdx].g + COST[i];
            if (newG < nodes[nIdx].g)
            {
                nodes[nIdx].g         = newG;
                nodes[nIdx].f         = newG + heuristic(nx, ny, egx, egy);
                nodes[nIdx].parentIdx = curIdx;
                openSet.push({ nodes[nIdx].f, nIdx });
            }
        }
    }

    return false;
}

AStar::Path AStar::Simplify(const Path& raw)
{
    if (raw.size() <= 2) return raw;

    Path result;
    result.push_back(raw.front());

    for (size_t i = 1; i + 1 < raw.size(); i++)
    {
        const float dx1 = raw[i].first  - raw[i - 1].first;
        const float dy1 = raw[i].second - raw[i - 1].second;
        const float dx2 = raw[i + 1].first  - raw[i].first;
        const float dy2 = raw[i + 1].second - raw[i].second;

        if (std::abs(dx1 * dy2 - dy1 * dx2) > 1e-4f)
            result.push_back(raw[i]);
    }

    result.push_back(raw.back());
    return result;
}
