// The Linux side of the network layer's vocabulary: address literals, the mapping of errno onto Winsock error
// numbers, and the NetworkAddress <-> socket address conversions.
#include "internal.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/os/linux/error.hpp"

namespace maicord {
namespace {

constexpr u32 kLiteralCapacity = 64;

// "a.b.c.d": four decimal numbers up to 255 without leading zeros, as inet_pton reads one. `end` is where the text
// ends; false when the span is not exactly an address.
bool parse_ipv4(const char* begin, const char* end, u8 (&bytes)[4]) noexcept {
    u32 count = 0;
    const char* cursor = begin;
    while (cursor < end) {
        if (count == 4 || *cursor < '0' || *cursor > '9') {
            return false;
        }
        u32 value = 0;
        const char* first = cursor;
        while (cursor < end && *cursor >= '0' && *cursor <= '9') {
            value = value * 10 + static_cast<u32>(*cursor - '0');
            if (value > 255) {
                return false;
            }
            ++cursor;
        }
        if (cursor - first > 1 && *first == '0') {
            return false;
        }
        bytes[count++] = static_cast<u8>(value);
        if (cursor < end) {
            if (*cursor != '.' || cursor + 1 == end) {
                return false;
            }
            ++cursor;
        }
    }
    return count == 4;
}

// An IPv6 address: up to eight groups of one to four hex digits, one "::" for a run of zero groups, and an IPv4
// address in place of the last two groups.
bool parse_ipv6(const char* text, u32 length, u8 (&bytes)[16]) noexcept {
    if (length < 2) {
        return false;
    }
    u16 groups[8]{};
    u32 count = 0;
    i32 gap = -1;
    u32 index = 0;
    if (text[0] == ':') {
        if (text[1] != ':') {
            return false;
        }
        gap = 0;
        index = 2;
    }
    while (index < length) {
        if (count == 8) {
            return false;
        }
        // An embedded IPv4 address ends the text.
        u32 scan = index;
        while (scan < length && text[scan] != ':') {
            ++scan;
        }
        const char* dot = text + index;
        bool has_dot = false;
        for (; dot < text + scan; ++dot) {
            has_dot |= *dot == '.';
        }
        if (has_dot) {
            u8 v4[4];
            if (scan != length || count > 6 || !parse_ipv4(text + index, text + scan, v4)) {
                return false;
            }
            groups[count++] = static_cast<u16>((v4[0] << 8) | v4[1]);
            groups[count++] = static_cast<u16>((v4[2] << 8) | v4[3]);
            index = length;
            break;
        }
        u32 value = 0;
        u32 digits = 0;
        i32 digit = 0;
        while (index < length && (digit = ascii_hex_value(text[index])) >= 0) {
            if (++digits > 4) {
                return false;
            }
            value = (value << 4) | static_cast<u32>(digit);
            ++index;
        }
        if (digits == 0) {
            return false;
        }
        groups[count++] = static_cast<u16>(value);
        if (index == length) {
            break;
        }
        if (text[index] != ':') {
            return false;
        }
        ++index;
        if (index < length && text[index] == ':') {
            if (gap >= 0) {
                return false;
            }
            gap = static_cast<i32>(count);
            ++index;
            if (index == length) {
                break;
            }
        } else if (index == length) {
            // A single trailing colon.
            return false;
        }
    }
    if (gap < 0 ? count != 8 : count > 7) {
        return false;
    }
    u16 full[8]{};
    if (gap < 0) {
        for (u32 group = 0; group < 8; ++group) {
            full[group] = groups[group];
        }
    } else {
        const u32 head = static_cast<u32>(gap);
        for (u32 group = 0; group < head; ++group) {
            full[group] = groups[group];
        }
        for (u32 group = head; group < count; ++group) {
            full[8 - count + group] = groups[group];
        }
    }
    for (u32 group = 0; group < 8; ++group) {
        bytes[group * 2] = static_cast<u8>(full[group] >> 8);
        bytes[group * 2 + 1] = static_cast<u8>(full[group]);
    }
    return true;
}

u16 swap16(u16 value) noexcept {
    return static_cast<u16>((value >> 8) | (value << 8));
}

} // namespace

bool network_address_parse(const char* text, u16 port, NetworkAddress* address) noexcept {
    clear_bytes(address, sizeof(*address));
    if (!text) {
        return false;
    }
    u32 length = 0;
    while (length < kLiteralCapacity && text[length]) {
        ++length;
    }
    if (length == 0 || length >= kLiteralCapacity) {
        return false;
    }
    u8 v4[4];
    u8 v6[16];
    if (parse_ipv4(text, text + length, v4)) {
        address->family = 4;
        copy_bytes(address->ip, v4, 4);
    } else if (parse_ipv6(text, length, v6)) {
        address->family = 6;
        copy_bytes(address->ip, v6, 16);
    } else {
        return false;
    }
    address->port = port;
    return true;
}

bool network_address_is_ipv6_literal(const wchar_t* text) noexcept {
    if (!text) {
        return false;
    }
    char narrow[kLiteralCapacity];
    u32 length = 0;
    for (; text[length]; ++length) {
        if (length + 1 >= kLiteralCapacity || static_cast<u16>(text[length]) >= 0x80) {
            return false;
        }
        narrow[length] = static_cast<char>(text[length]);
    }
    narrow[length] = 0;
    u8 bytes[16];
    return parse_ipv6(narrow, length, bytes);
}

i32 network_last_error() noexcept {
    return static_cast<i32>(thread_last_error());
}

namespace net_detail {

i32 map_errno(int error) noexcept {
    switch (error) {
    case kErrorPermission:
    case kErrorAccess:
        return WSAEACCES;
    case kErrorAgain:
        return WSAEWOULDBLOCK;
    case kErrorInvalid:
        return WSAEINVAL;
    case kErrorNoMemory:
        return ERROR_NOT_ENOUGH_MEMORY;
    case kErrorPipe:
    case kErrorConnectionReset:
        return WSAECONNRESET;
    case kErrorMessageSize:
        return WSAEMSGSIZE;
    case kErrorAddressFamily:
        return WSAEAFNOSUPPORT;
    case kErrorAddressInUse:
        return WSAEADDRINUSE;
    case kErrorAddressNotAvailable:
        return kWsaAddressNotAvailable;
    case kErrorNetworkUnreachable:
        return kWsaNetworkUnreachable;
    case kErrorConnectionAborted:
        return kWsaConnectionAborted;
    case kErrorNotConnected:
        return kWsaNotConnected;
    case kErrorTimedOut:
        return kWsaTimedOut;
    case kErrorConnectionRefused:
        return kWsaConnectionRefused;
    case kErrorHostUnreachable:
        return WSAEHOSTUNREACH;
    case kErrorCanceled:
        return ERROR_OPERATION_ABORTED;
    default:
        return static_cast<i32>(kErrorNative | static_cast<u32>(error));
    }
}

i32 map_result(long result) noexcept {
    const i32 error = map_errno(static_cast<int>(-result));
    set_error(error);
    return error;
}

void set_error(i32 error) noexcept {
    set_thread_last_error(static_cast<u32>(error));
}

u32 to_socket_address(const NetworkAddress& address, SockAddrStorage* storage) noexcept {
    clear_bytes(storage, sizeof(*storage));
    if (address.family == 4) {
        auto* v4 = reinterpret_cast<SockAddrIn*>(storage);
        v4->family = kAfInet;
        v4->port = swap16(address.port);
        copy_bytes(v4->address, address.ip, 4);
        return sizeof(SockAddrIn);
    }
    if (address.family == 6) {
        auto* v6 = reinterpret_cast<SockAddrIn6*>(storage);
        v6->family = kAfInet6;
        v6->port = swap16(address.port);
        copy_bytes(v6->address, address.ip, 16);
        v6->scope = address.scope_id;
        return sizeof(SockAddrIn6);
    }
    return 0;
}

bool from_socket_address(const void* socket_address, u32 bytes, NetworkAddress* address) noexcept {
    clear_bytes(address, sizeof(*address));
    if (!socket_address || bytes < sizeof(u16)) {
        return false;
    }
    const u16 family = *static_cast<const u16*>(socket_address);
    if (family == kAfInet && bytes >= sizeof(SockAddrIn)) {
        const auto* v4 = static_cast<const SockAddrIn*>(socket_address);
        address->family = 4;
        address->port = swap16(v4->port);
        copy_bytes(address->ip, v4->address, 4);
        return true;
    }
    if (family == kAfInet6 && bytes >= sizeof(SockAddrIn6)) {
        const auto* v6 = static_cast<const SockAddrIn6*>(socket_address);
        address->family = 6;
        address->port = swap16(v6->port);
        copy_bytes(address->ip, v6->address, 16);
        address->scope_id = v6->scope;
        return true;
    }
    return false;
}

} // namespace net_detail
} // namespace maicord
