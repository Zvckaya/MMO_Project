#include "AuthClient.h"
#include "ServerConfig.h"
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include <string>
#pragma comment(lib, "winhttp.lib")

static void ParseSuccessBody(const std::string& body, AuthResult& out)
{
    size_t idPos = body.find("\"accountId\":");
    if (idPos != std::string::npos)
    {
        idPos += 12;
        try { out.accountId = std::stoull(body.substr(idPos)); }
        catch (...) {}
    }

    size_t namePos = body.find("\"displayName\":\"");
    if (namePos != std::string::npos)
    {
        namePos += 15;
        size_t nameEnd = body.find('"', namePos);
        if (nameEnd != std::string::npos)
            out.displayName = body.substr(namePos, nameEnd - namePos);
    }
}

static HINTERNET s_hSession = nullptr;

AuthResult VerifyTicket(const char* ticket, int ticketLen)
{
    AuthResult result;

    if (!s_hSession)
        s_hSession = WinHttpOpen(L"GameServer/1.0",
            WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    if (!s_hSession)
        return result;

    HINTERNET hConnect = WinHttpConnect(s_hSession, AUTH_SERVER_HOST, AUTH_SERVER_PORT, 0);
    if (!hConnect)
        return result;

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", AUTH_VERIFY_PATH,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        return result;
    }

    std::string body = "{\"ticket\":\"";
    body.append(ticket, ticketLen);
    body += "\"}";

    BOOL ok = WinHttpSendRequest(hRequest,
        L"Content-Type: application/json",
        static_cast<DWORD>(-1L),
        const_cast<char*>(body.c_str()),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()),
        0);

    if (ok)
        ok = WinHttpReceiveResponse(hRequest, nullptr);

    if (ok)
    {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode, &statusSize,
            WINHTTP_NO_HEADER_INDEX);

        std::string response;
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0)
        {
            std::string chunk(available, '\0');
            DWORD read = 0;
            WinHttpReadData(hRequest, chunk.data(), available, &read);
            response.append(chunk, 0, read);
        }

        if (statusCode == 200)
        {
            result.valid = true;
            ParseSuccessBody(response, result);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    return result;
}
