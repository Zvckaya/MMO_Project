#pragma once
#include <cstdint>
#include <string>

struct AuthResult
{
    bool        valid       = false;
    uint64_t    accountId   = 0;
    std::string displayName;
};

AuthResult VerifyTicket(const char* ticket, int ticketLen);
