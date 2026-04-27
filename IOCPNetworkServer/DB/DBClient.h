#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <mysql.h>
#include "ServerConfig.h"

struct ItemData {
    uint16_t    itemID   = 0;
    std::string name;
    uint8_t     type     = 0;
    int32_t     maxStack = 1;
};

struct InventorySlotData {
    uint16_t slotIndex = 0;
    uint16_t itemID    = 0;
    int32_t  count     = 0;
};

struct PlayerDBData {
    bool    found = false;
    float   posX  = 0.f, posY = 0.f;
    int32_t hp    = 100;
    MapID   mapID = DEFAULT_MAP_ID;
    std::vector<InventorySlotData> inventory;
};

class DBClient
{
public:
    DBClient()  = default;
    ~DBClient() { Disconnect(); }

    bool Connect(const char* host, const char* user,
                 const char* password, const char* db, uint16_t port = 3306);
    void Disconnect();
    bool Ping();

    PlayerDBData LoadPlayer(uint64_t accountId);
    void         SavePlayer(uint64_t accountId, const PlayerDBData& data);
    void         SaveInventory(uint64_t accountId, const std::vector<InventorySlotData>& slots);

private:
    MYSQL* _mysql = nullptr;
};
