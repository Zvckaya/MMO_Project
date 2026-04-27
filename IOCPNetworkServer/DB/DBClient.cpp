#include "DBClient.h"
#include "Logger.h"
#include <cstdio>

bool DBClient::Connect(const char* host, const char* user,
                       const char* password, const char* db, uint16_t port)
{
    _mysql = mysql_init(nullptr);
    if (!_mysql) return false;

    if (!mysql_real_connect(_mysql, host, user, password, db, port, nullptr, 0))
    {
        Log(L"DB", Logger::Level::ERR, L"Connect failed: %S", mysql_error(_mysql));
        mysql_close(_mysql);
        _mysql = nullptr;
        return false;
    }

    mysql_set_character_set(_mysql, "utf8mb4");
    Log(L"DB", Logger::Level::SYSTEM, L"MySQL connected %S:%d/%S", host, port, db);
    return true;
}

void DBClient::Disconnect()
{
    if (_mysql)
    {
        mysql_close(_mysql);
        _mysql = nullptr;
    }
}

bool DBClient::Ping()
{
    return _mysql && mysql_ping(_mysql) == 0;
}

PlayerDBData DBClient::LoadPlayer(uint64_t accountId)
{
    PlayerDBData result;
    if (!_mysql) return result;

    char query[256];
    snprintf(query, sizeof(query),
        "SELECT pos_x, pos_y, hp, map_id FROM characters WHERE account_id = %llu",
        static_cast<unsigned long long>(accountId));

    if (mysql_query(_mysql, query))
    {
        Log(L"DB", Logger::Level::ERR, L"LoadPlayer failed: %S", mysql_error(_mysql));
        return result;
    }

    MYSQL_RES* res = mysql_store_result(_mysql);
    if (!res) return result;

    MYSQL_ROW row = mysql_fetch_row(res);
    if (row)
    {
        result.found = true;
        result.posX  = row[0] ? static_cast<float>(atof(row[0])) : 0.f;
        result.posY  = row[1] ? static_cast<float>(atof(row[1])) : 0.f;
        result.hp    = row[2] ? atoi(row[2]) : 100;
        result.mapID = row[3] ? static_cast<MapID>(atoi(row[3])) : DEFAULT_MAP_ID;
    }

    mysql_free_result(res);

    if (result.found)
    {
        char invQuery[256];
        snprintf(invQuery, sizeof(invQuery),
            "SELECT slot_index, item_id, count FROM player_inventory WHERE account_id = %llu",
            static_cast<unsigned long long>(accountId));

        if (mysql_query(_mysql, invQuery) == 0)
        {
            MYSQL_RES* invRes = mysql_store_result(_mysql);
            if (invRes)
            {
                while (MYSQL_ROW row = mysql_fetch_row(invRes))
                {
                    InventorySlotData slot;
                    slot.slotIndex = static_cast<uint16_t>(atoi(row[0]));
                    slot.itemID    = static_cast<uint16_t>(atoi(row[1]));
                    slot.count     = atoi(row[2]);
                    result.inventory.push_back(slot);
                }
                mysql_free_result(invRes);
            }
        }
    }

    return result;
}

void DBClient::SavePlayer(uint64_t accountId, const PlayerDBData& data)
{
    if (!_mysql) return;

    char query[512];
    snprintf(query, sizeof(query),
        "INSERT INTO characters (account_id, pos_x, pos_y, hp, map_id) "
        "VALUES (%llu, %.4f, %.4f, %d, %u) "
        "ON DUPLICATE KEY UPDATE pos_x=%.4f, pos_y=%.4f, hp=%d, map_id=%u",
        static_cast<unsigned long long>(accountId),
        data.posX, data.posY, data.hp, data.mapID,
        data.posX, data.posY, data.hp, data.mapID);

    if (mysql_query(_mysql, query))
        Log(L"DB", Logger::Level::ERR, L"SavePlayer failed: %S", mysql_error(_mysql));
}

void DBClient::SaveInventory(uint64_t accountId, const std::vector<InventorySlotData>& slots)
{
    if (!_mysql) return;

    mysql_query(_mysql, "START TRANSACTION");

    char deleteQuery[128];
    snprintf(deleteQuery, sizeof(deleteQuery),
        "DELETE FROM player_inventory WHERE account_id = %llu",
        static_cast<unsigned long long>(accountId));
    mysql_query(_mysql, deleteQuery);

    for (const auto& slot : slots)
    {
        if (slot.itemID == 0 || slot.count <= 0) continue;
        char insertQuery[256];
        snprintf(insertQuery, sizeof(insertQuery),
            "INSERT INTO player_inventory (account_id, slot_index, item_id, count) VALUES (%llu, %u, %u, %d)",
            static_cast<unsigned long long>(accountId),
            slot.slotIndex, slot.itemID, slot.count);
        if (mysql_query(_mysql, insertQuery))
        {
            Log(L"DB", Logger::Level::ERR, L"SaveInventory insert failed: %S", mysql_error(_mysql));
            mysql_query(_mysql, "ROLLBACK");
            return;
        }
    }

    mysql_query(_mysql, "COMMIT");
}
