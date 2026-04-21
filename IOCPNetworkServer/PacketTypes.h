#pragma once
#include <cstdint>

// =====================================================================
//  Packet Type IDs
// =====================================================================

constexpr uint16_t PKT_ECHO                  = 0x0000;

constexpr uint16_t PKT_CS_LOGIN_AUTH         = 0x0101;
constexpr uint16_t PKT_SC_LOGIN_AUTH_RESULT  = 0x0102;

constexpr uint16_t PKT_CS_MOVE               = 0x0201;
constexpr uint16_t PKT_SC_MOVE               = 0x0202;
constexpr uint16_t PKT_SC_SPAWN              = 0x0203;
constexpr uint16_t PKT_SC_DESPAWN            = 0x0204;
constexpr uint16_t PKT_SC_MOVE_CORRECT       = 0x0205;
constexpr uint16_t PKT_SC_WORLD_ENTER        = 0x0206;
constexpr uint16_t PKT_CS_STOP               = 0x0207;

constexpr uint16_t PKT_CS_ATTACK             = 0x0301;
constexpr uint16_t PKT_SC_ATTACK             = 0x0302;

constexpr uint16_t PKT_CS_SKILL              = 0x0401;
constexpr uint16_t PKT_SC_SKILL              = 0x0402;

constexpr uint16_t PKT_CS_ITEM_PICKUP        = 0x0501;
constexpr uint16_t PKT_CS_ITEM_DROP          = 0x0502;
constexpr uint16_t PKT_SC_ITEM_APPEAR        = 0x0503;
constexpr uint16_t PKT_SC_ITEM_DISAPPEAR     = 0x0504;
constexpr uint16_t PKT_SC_INVENTORY_UPD      = 0x0505;

constexpr uint16_t PKT_SC_NPC_SPAWN          = 0x0601;
constexpr uint16_t PKT_SC_NPC_DESPAWN        = 0x0602;
constexpr uint16_t PKT_SC_NPC_MOVE           = 0x0603;
constexpr uint16_t PKT_SC_NPC_ATTACK         = 0x0604;

// =====================================================================
//  Auth Error Codes
// =====================================================================

constexpr uint32_t AUTH_OK               = 0;
constexpr uint32_t AUTH_ERR_INVALID      = 1;
constexpr uint32_t AUTH_ERR_EXPIRED      = 2;
constexpr uint32_t AUTH_ERR_CONSUMED     = 3;
constexpr uint32_t AUTH_ERR_SERVER_FAIL  = 4;

// =====================================================================
//  Packet Payload Structs  (wire order, 1-byte aligned)
//  variable-length tail은 고정 헤더 구조체 뒤에 이어서 직렬화
// =====================================================================

#pragma pack(push, 1)

// [0x0101] CS_LOGIN_AUTH
// payload: ticket(32) fixed-size ASCII
struct CS_LOGIN_AUTH        { char ticket[32]; };

// [0x0102] SC_LOGIN_AUTH_RESULT
// payload: success(1) + errorCode(4) + accountId(8) + displayNameLength(2) + displayName(N)
struct SC_LOGIN_AUTH_RESULT { uint8_t success; uint32_t errorCode; uint64_t accountId; uint16_t displayNameLength; };

// [0x0201] CS_MOVE
struct CS_MOVE              { float curX; float curY; float destX; float destY; };

// [0x0202] SC_MOVE
struct ScMovePacket         { uint64_t sessionID; float curX; float curY; float destX; float destY; float speed; };

// [0x0203] SC_SPAWN
struct SC_SPAWN             { uint64_t sessionID; float x; float y; int32_t hp; int32_t maxHp; };

// [0x0204] SC_DESPAWN
struct SC_DESPAWN           { uint64_t sessionID; };

// [0x0205] SC_MOVE_CORRECT
struct SC_MOVE_CORRECT      { float curX; float curY; };

// [0x0207] CS_STOP
struct CS_STOP              { float curX; float curY; };

// [0x0301] CS_ATTACK
struct CS_ATTACK            { uint64_t targetID; };

// [0x0302] SC_ATTACK
struct SC_ATTACK            { uint64_t attackerID; uint64_t targetID; int32_t damage; int32_t targetHp; };

// [0x0401] CS_SKILL
// single-target: targetID 사용 / AoE: targetX,targetY 사용
struct CS_SKILL             { uint16_t skillID; uint64_t targetID; float targetX; float targetY; };

// [0x0402] SC_SKILL
struct SC_SKILL             { uint64_t casterID; uint16_t skillID; uint64_t targetID; float targetX; float targetY; int32_t damage; int32_t targetHp; };

// [0x0501] CS_ITEM_PICKUP
struct CS_ITEM_PICKUP       { uint64_t itemUID; };

// [0x0502] CS_ITEM_DROP
struct CS_ITEM_DROP         { uint16_t slotIndex; };

// [0x0503] SC_ITEM_APPEAR
struct SC_ITEM_APPEAR       { uint64_t itemUID; uint16_t itemID; float x; float y; };

// [0x0504] SC_ITEM_DISAPPEAR
struct SC_ITEM_DISAPPEAR    { uint64_t itemUID; };

// [0x0505] SC_INVENTORY_UPD
struct SC_INVENTORY_UPD     { uint16_t slotIndex; uint16_t itemID; uint16_t count; };

// [0x0601] SC_NPC_SPAWN
struct SC_NPC_SPAWN         { uint64_t npcUID; uint16_t npcType; float x; float y; int32_t hp; int32_t maxHp; };

// [0x0602] SC_NPC_DESPAWN
struct SC_NPC_DESPAWN       { uint64_t npcUID; };

// [0x0603] SC_NPC_MOVE
struct SC_NPC_MOVE          { uint64_t npcUID; float curX; float curY; float destX; float destY; float speed; };

// [0x0604] SC_NPC_ATTACK
struct SC_NPC_ATTACK        { uint64_t npcUID; uint64_t targetID; int32_t damage; int32_t targetHp; };

#pragma pack(pop)
