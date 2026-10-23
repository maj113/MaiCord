#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/sys/winsock.hpp"

namespace maicord {

// The caller closes handle before releasing its NetworkLoop.
struct TestLoopbackListener {
    SOCKET handle = INVALID_SOCKET;
    char authority[32] = "127.0.0.1:";
    u32 bytes = 10;

    bool start(int backlog) noexcept {
        handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int length = sizeof(address);
        if (handle == INVALID_SOCKET ||
            bind(handle, reinterpret_cast<const sockaddr*>(&address), length) != 0 ||
            getsockname(handle, reinterpret_cast<sockaddr*>(&address), &length) != 0 ||
            listen(handle, backlog) != 0)
            return false;
        char reverse[6]{};
        u32 digits = 0, port = ntohs(address.sin_port);
        do {
            reverse[digits++] = static_cast<char>('0' + port % 10);
            port /= 10;
        } while (port);
        for (u32 i = 0; i < digits; ++i)
            authority[10 + i] = reverse[digits - 1 - i];
        bytes = 10 + digits;
        return true;
    }
    Utf8View endpoint() const noexcept { return {authority, bytes}; }
};

} // namespace maicord
