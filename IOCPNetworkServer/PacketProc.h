#pragma once
#include <cstdint>
#include <functional>
#include <unordered_map>

#include "IOCPServer.h"
#include "Packet.h"
constexpr uint16_t PKT_ECHO = 0;

class PacketProc
{
public:
    using Handler = std::function<void(SessionID, Packet*)>;

    void Register(uint16_t type, Handler handler);
    void Dispatch(SessionID sessionID, Packet* packet);

private:
    std::unordered_map<uint16_t, Handler> _handlers;
};

