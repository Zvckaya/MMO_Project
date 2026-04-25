#include "Packet.h"

Packet::Packet() : _readPos(sizeof(uint16_t)), _writePos(sizeof(uint16_t))
{
    memset(_buffer, 0, sizeof(_buffer));
}

void Packet::Clear()
{
    _readPos  = sizeof(uint16_t);
    _writePos = sizeof(uint16_t);
}

int Packet::GetBufferSize() const
{
    return MAXPAYLOAD;
}

int Packet::GetDataSize() const
{
    return _writePos;
}

char* Packet::GetBufferPtr()
{
    return _buffer;
}

int Packet::GetReadPos() const
{
    return _readPos;
}

int Packet::GetRemainingReadSize() const
{
    return _writePos - _readPos;
}

int Packet::MoveWritePos(int size)
{
    if (size <= 0) return _writePos;
    if (_writePos + size > MAXPAYLOAD) return _writePos;
    _writePos += size;
    return _writePos;
}

int Packet::MoveReadPos(int size)
{
    if (size <= 0) return _readPos;
    if (_readPos + size > _writePos) _readPos = _writePos;
    else _readPos += size;
    return _readPos;
}

int Packet::PutData(const char* src, int size)
{
    if (size <= 0) return 0;
    if (_writePos + size > MAXPAYLOAD) return 0;
    memcpy(_buffer + _writePos, src, size);
    _writePos += size;
    return size;
}

int Packet::GetData(char* dest, int size)
{
    if (size <= 0) return 0;
    if (_readPos + size > _writePos) return 0;
    memcpy(dest, _buffer + _readPos, size);
    _readPos += size;
    return size;
}

void Packet::EncodeHeader()
{
    uint16_t payloadSize = static_cast<uint16_t>(_writePos - sizeof(uint16_t));
    memcpy(_buffer, &payloadSize, sizeof(uint16_t));
}
