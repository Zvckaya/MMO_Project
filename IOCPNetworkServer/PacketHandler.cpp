#include "GameServer.h"

void GameServer::HandlePacket(SessionID sessionID, Packet* packet)
{
    switch (packet->GetType())
    {
    case PKT_ECHO:
        OnEcho(sessionID, packet);
        break;
    case PKT_CS_LOGIN_AUTH:
        OnCS_LoginAuth(sessionID, packet);
        break;
    case PKT_CS_MOVE:
        OnCS_Move(sessionID, packet);
        break;
    case PKT_CS_ATTACK:
        OnCS_Attack(sessionID, packet);
        break;
    case PKT_CS_STOP:
        OnCS_Stop(sessionID, packet);
        break;
    case PKT_CS_SKILL:
        OnCS_Skill(sessionID, packet);
        break;
    case PKT_CS_ITEM_PICKUP:
        OnCS_ItemPickup(sessionID, packet);
        break;
    case PKT_CS_ITEM_DROP:
        OnCS_ItemDrop(sessionID, packet);
        break;
    case PKT_CS_ITEM_MOVE:
        OnCS_ItemMove(sessionID, packet);
        break;
    case PKT_CS_MAP_CHANGE_REQ:
        OnCS_MapChangeReq(sessionID, packet);
        break;
    default:
        break;
    }
}

void GameServer::OnEcho(SessionID sessionID, Packet* packet)
{
    SendPacket(sessionID, packet);
}

void GameServer::OnCS_LoginAuth(SessionID sessionID, Packet* packet)
{
    CS_LOGIN_AUTH data;
    if (!packet->ReadStruct(data))
        return;

    AuthRequest req{ sessionID };
    memcpy(req.ticket, data.ticket, sizeof(req.ticket));

    std::lock_guard lock(_authQueueMutex);
    _authPendingQueue.push(req);
    _authQueueCv.notify_one();
}

void GameServer::OnCS_Move(SessionID sessionID, Packet* packet)
{
    CS_MOVE data;
    if (!packet->ReadStruct(data)) return;

    std::shared_lock lock(_jobQueuesMutex);
    auto it = _sessionJobQueues.find(sessionID);
    if (it == _sessionJobQueues.end()) return;

    it->second->pendingMove.curX  = data.curX;
    it->second->pendingMove.curY  = data.curY;
    it->second->pendingMove.destX = data.destX;
    it->second->pendingMove.destY = data.destY;
    it->second->pendingMove.dirty.store(true, std::memory_order_release);
}

void GameServer::OnCS_Stop(SessionID sessionID, Packet* packet)
{
    CS_STOP data;
    if (!packet->ReadStruct(data)) return;

    std::shared_lock lock(_jobQueuesMutex);
    auto it = _sessionJobQueues.find(sessionID);
    if (it == _sessionJobQueues.end()) return;

    it->second->pendingStop.curX = data.curX;
    it->second->pendingStop.curY = data.curY;
    it->second->pendingStop.dirty.store(true, std::memory_order_release);
}

void GameServer::OnCS_Attack(SessionID sessionID, Packet* packet)
{
    CS_ATTACK data;
    packet->ReadStruct(data);

    EnqueueFrameTask({
        .type = FrameTaskType::playerAttack,
        .sessionID = sessionID,
        .targetID = data.targetID
    });
}

void GameServer::OnCS_Skill(SessionID sessionID, Packet* packet)
{
    CS_SKILL data;
    packet->ReadStruct(data);

    EnqueueFrameTask({
        .type = FrameTaskType::playerSkill,
        .sessionID = sessionID,
        .targetID = data.targetID,
        .curX = data.targetX,
        .curY = data.targetY,
        .skillID = data.skillID
    });
}

void GameServer::OnCS_ItemPickup(SessionID sessionID, Packet* packet)
{
    CS_ITEM_PICKUP data;
    if (!packet->ReadStruct(data)) return;

    EnqueueFrameTask({
        .type      = FrameTaskType::itemPickup,
        .sessionID = sessionID,
        .targetID  = data.itemUID
    });
}

void GameServer::OnCS_ItemDrop(SessionID sessionID, Packet* packet)
{
    CS_ITEM_DROP data;
    if (!packet->ReadStruct(data)) return;

    EnqueueFrameTask({
        .type      = FrameTaskType::itemDrop,
        .sessionID = sessionID,
        .fromSlot  = data.slotIndex
    });
}

void GameServer::OnCS_ItemMove(SessionID sessionID, Packet* packet)
{
    CS_ITEM_MOVE data;
    if (!packet->ReadStruct(data)) return;

    EnqueueFrameTask({
        .type      = FrameTaskType::itemSlotSwap,
        .sessionID = sessionID,
        .fromSlot  = data.fromSlot,
        .toSlot    = data.toSlot
    });
}

void GameServer::OnCS_MapChangeReq(SessionID sessionID, Packet* packet)
{
    CS_MAP_CHANGE_REQ data;
    if (!packet->ReadStruct(data)) return;

    EnqueueFrameTask({
        .type        = FrameTaskType::playerMapChange,
        .sessionID   = sessionID,
        .targetMapID = data.targetMapID
    });
}
