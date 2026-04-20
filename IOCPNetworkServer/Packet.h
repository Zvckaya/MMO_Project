#pragma once
#include <cstdint>
#include <cstring>
#include "ServerConfig.h"

class Packet
{
public:
    Packet() : _readPos(sizeof(uint16_t)), _writePos(sizeof(uint16_t))
    {
        memset(_buffer, 0, sizeof(_buffer));
    }
    ~Packet() = default;

    void Clear()
    {
        _readPos  = sizeof(uint16_t);
        _writePos = sizeof(uint16_t);
    }

    int   GetBufferSize()        const { return MAXPAYLOAD; }
    int   GetDataSize()          const { return _writePos; }
    char* GetBufferPtr()               { return _buffer; }
    int   GetReadPos()           const { return _readPos; }
    int   GetRemainingReadSize() const { return _writePos - _readPos; }

    int MoveWritePos(int size)
    {
        if (size <= 0 || _writePos + size > MAXPAYLOAD) return _writePos;
        _writePos += size;
        return _writePos;
    }

    int MoveReadPos(int size)
    {
        if (size <= 0) return _readPos;
        _readPos = (_readPos + size > _writePos) ? _writePos : _readPos + size;
        return _readPos;
    }

    int PutData(const char* src, int size)
    {
        if (size <= 0 || _writePos + size > MAXPAYLOAD) return 0;
        memcpy(_buffer + _writePos, src, size);
        _writePos += size;
        return size;
    }

    int GetData(char* dest, int size)
    {
        if (size <= 0 || _readPos + size > _writePos) return 0;
        memcpy(dest, _buffer + _readPos, size);
        _readPos += size;
        return size;
    }
    void EncodeHeader()
    {
        uint16_t payloadSize = static_cast<uint16_t>(_writePos - sizeof(uint16_t));
        memcpy(_buffer, &payloadSize, sizeof(uint16_t));
    }

    Packet& operator<<(bool     value) { PutData(reinterpret_cast<const char*>(&value), sizeof(value)); return *this; }
    Packet& operator<<(int8_t   value) { PutData(reinterpret_cast<const char*>(&value), sizeof(value)); return *this; }
    Packet& operator<<(uint8_t  value) { PutData(reinterpret_cast<const char*>(&value), sizeof(value)); return *this; }
    Packet& operator<<(int16_t  value) { PutData(reinterpret_cast<const char*>(&value), sizeof(value)); return *this; }
    Packet& operator<<(uint16_t value) { PutData(reinterpret_cast<const char*>(&value), sizeof(value)); return *this; }
    Packet& operator<<(int32_t  value) { PutData(reinterpret_cast<const char*>(&value), sizeof(value)); return *this; }
    Packet& operator<<(uint32_t value) { PutData(reinterpret_cast<const char*>(&value), sizeof(value)); return *this; }
    Packet& operator<<(int64_t  value) { PutData(reinterpret_cast<const char*>(&value), sizeof(value)); return *this; }
    Packet& operator<<(uint64_t value) { PutData(reinterpret_cast<const char*>(&value), sizeof(value)); return *this; }
    Packet& operator<<(float    value) { PutData(reinterpret_cast<const char*>(&value), sizeof(value)); return *this; }
    Packet& operator<<(double   value) { PutData(reinterpret_cast<const char*>(&value), sizeof(value)); return *this; }

    Packet& operator>>(bool&     value) { GetData(reinterpret_cast<char*>(&value), sizeof(value)); return *this; }
    Packet& operator>>(int8_t&   value) { GetData(reinterpret_cast<char*>(&value), sizeof(value)); return *this; }
    Packet& operator>>(uint8_t&  value) { GetData(reinterpret_cast<char*>(&value), sizeof(value)); return *this; }
    Packet& operator>>(int16_t&  value) { GetData(reinterpret_cast<char*>(&value), sizeof(value)); return *this; }
    Packet& operator>>(uint16_t& value) { GetData(reinterpret_cast<char*>(&value), sizeof(value)); return *this; }
    Packet& operator>>(int32_t&  value) { GetData(reinterpret_cast<char*>(&value), sizeof(value)); return *this; }
    Packet& operator>>(uint32_t& value) { GetData(reinterpret_cast<char*>(&value), sizeof(value)); return *this; }
    Packet& operator>>(int64_t&  value) { GetData(reinterpret_cast<char*>(&value), sizeof(value)); return *this; }
    Packet& operator>>(uint64_t& value) { GetData(reinterpret_cast<char*>(&value), sizeof(value)); return *this; }
    Packet& operator>>(float&    value) { GetData(reinterpret_cast<char*>(&value), sizeof(value)); return *this; }
    Packet& operator>>(double&   value) { GetData(reinterpret_cast<char*>(&value), sizeof(value)); return *this; }

private:
    char _buffer[MAXPAYLOAD];
    int  _readPos;
    int  _writePos;
};

