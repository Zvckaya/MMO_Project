#include "PacketProc.h"

void PacketProc::Register(uint16_t type, Handler handler)
{
    _handlers[type] = std::move(handler);
}

void PacketProc::Dispatch(SessionID sessionID, Packet* packet)
{
    uint16_t type = packet->GetType();
    auto it = _handlers.find(type);
    if (it != _handlers.end())
        it->second(sessionID, packet);
}

