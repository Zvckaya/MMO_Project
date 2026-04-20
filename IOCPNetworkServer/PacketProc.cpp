#include "PacketProc.h"

void PacketProc::Register(uint16_t type, Handler handler)
{
    _handlers[type] = std::move(handler);
}

void PacketProc::Dispatch(SessionID sessionID, Packet* packet)
{
    uint16_t type = PKT_ECHO;

    switch (type)
    {
    case PKT_ECHO:
    {
        auto it = _handlers.find(type);
        if (it != _handlers.end())
            it->second(sessionID, packet);
        break;
    }
    default:
        break;
    }
}

