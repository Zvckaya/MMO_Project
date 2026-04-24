#pragma once
#include <cstdint>

using MapID = uint32_t;
constexpr MapID DEFAULT_MAP_ID = 1;

constexpr int BUFFERSIZE = 20000;
constexpr int MAXSESSIONSIZE = 20000;
constexpr uint16_t PORT =  6000;
constexpr int MAXPAYLOAD = 100;
constexpr int FRAME_RATE     = 60;
constexpr int PACKET_POOL_SIZE  = 30000;
constexpr int   TLS_CACHE_BATCH = 32;
constexpr float    ATTACK_RANGE        = 2.f;

constexpr float TILE_SIZE      = 0.5f;
constexpr float WORLD_ORIGIN_X = -100.f;
constexpr float WORLD_ORIGIN_Y = -100.f;
constexpr int      MAX_INVENTORY_SLOTS = 30;
constexpr float    ITEM_PICKUP_RANGE   = 3.f;
constexpr uint64_t ITEM_DESPAWN_MS     = 30000;

constexpr const wchar_t* AUTH_SERVER_HOST = L"127.0.0.1";
constexpr uint16_t       AUTH_SERVER_PORT = 5105;
constexpr const wchar_t* AUTH_VERIFY_PATH = L"/api/auth/tickets/consume";

constexpr const char* GAME_DB_HOST     = "127.0.0.1";
constexpr uint16_t    GAME_DB_PORT     = 3307;
constexpr const char* GAME_DB_USER     = "root";
constexpr const char* GAME_DB_PASSWORD = "ak47qmffor";
constexpr const char* GAME_DB_NAME     = "gameserver";

