#pragma once
#include <cstdint>

using MapID = uint32_t;
constexpr MapID DEFAULT_MAP_ID = 0;

constexpr int BUFFERSIZE = 20000;
constexpr int MAXSESSIONSIZE = 20000;
constexpr uint16_t PORT =  6000;
constexpr int MAXPAYLOAD = 100;
constexpr int FRAME_RATE     = 60;
constexpr int PACKET_POOL_SIZE  = 30000;
constexpr int   TLS_CACHE_BATCH = 32;
constexpr float ATTACK_RANGE   = 2.f;

constexpr const wchar_t* AUTH_SERVER_HOST = L"127.0.0.1";
constexpr uint16_t       AUTH_SERVER_PORT = 5105;
constexpr const wchar_t* AUTH_VERIFY_PATH = L"/api/auth/tickets/consume";

