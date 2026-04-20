#pragma once
#include <cstdint>
#include <functional>
#include <unordered_map>

#include "IOCPServer.h"
#include "Packet.h"
constexpr uint16_t PKT_ECHO        = 0x0000;

constexpr uint16_t PKT_CS_MOVE     = 0x0201;
constexpr uint16_t PKT_SC_MOVE     = 0x0202;
constexpr uint16_t PKT_SC_SPAWN    = 0x0203;
constexpr uint16_t PKT_SC_DESPAWN  = 0x0204;

class PacketProc
{
public:
    using Handler = std::function<void(SessionID, Packet*)>;

    void Register(uint16_t type, Handler handler);
    void Dispatch(SessionID sessionID, Packet* packet);

private:
    std::unordered_map<uint16_t, Handler> _handlers;
};

