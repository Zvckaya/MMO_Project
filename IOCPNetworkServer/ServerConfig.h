#pragma once
#include <cstdint>

using MapID = uint32_t;
constexpr MapID DEFAULT_MAP_ID = 1;

constexpr int BUFFERSIZE = 20000;
constexpr int MAXSESSIONSIZE = 20000;
constexpr uint16_t PORT =  6000;
constexpr int MAXPAYLOAD = 4096;
constexpr int FRAME_RATE     = 60;
constexpr int PACKET_POOL_SIZE  = 30000;
constexpr int   TLS_CACHE_BATCH = 32;
constexpr float    ATTACK_RANGE        = 2.f;
constexpr float    ATTACK_CONE_COS     = 0.5f; // cos(60°) → 전방 120도

constexpr float TILE_SIZE      = 1.0f;
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

constexpr float MONSTER_AGGRO_RANGE     = 10.f;
constexpr float MONSTER_ATTACK_RANGE    = 1.5f;
constexpr float MONSTER_LEASH_RANGE     = 20.f;
constexpr float MONSTER_ATTACK_COOLDOWN = 5.f;
constexpr float MONSTER_PATH_RECALC_SEC = 0.5f;
constexpr float MONSTER_RESPAWN_SEC     = 10.f;

constexpr uint64_t PERIODIC_SAVE_INTERVAL_MS = 300000;

