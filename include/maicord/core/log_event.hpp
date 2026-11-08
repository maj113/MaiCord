#pragma once
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/name_list.hpp"

namespace maicord {

// The error codes log lines name, in the order of kLogErrorNames: Win32 errors (the portable error vocabulary of both
// systems), HRESULTs (an HRESULT_FROM_WIN32 value is named by its Win32 error) and, on Linux, the errno a native error
// carries as kErrorNative | errno (os/linux/error.hpp). The common ones only; any other code logs as its number.
inline constexpr u32 kLogErrorCodes[] = {
    2, 3, 5, 6, 8, 14, 32, 87, 112, 122, 183, 995, 1223, 1460, 10054, 10060, 10061, 11001,
    0x80004001u, 0x80004002u, 0x80004005u, 0x8000FFFFu, 0x887A0001u, 0x887A0004u, 0x887A0005u, 0x887A0006u,
    0x887A0007u, 0x887A000Au, 0x887A0020u,
#if !defined(_WIN32)
    0x40000004u, 0x40000005u, 0x4000000Bu, 0x40000010u, 0x40000013u, 0x40000018u, 0x4000001Cu, 0x40000020u,
    0x40000065u, 0x40000068u, 0x4000006Fu, 0x40000071u,
#endif
};
inline constexpr char kLogErrorNames[] =
    "ERROR_FILE_NOT_FOUND\0ERROR_PATH_NOT_FOUND\0ERROR_ACCESS_DENIED\0ERROR_INVALID_HANDLE\0ERROR_NOT_ENOUGH_MEMORY\0"
    "ERROR_OUTOFMEMORY\0ERROR_SHARING_VIOLATION\0ERROR_INVALID_PARAMETER\0ERROR_DISK_FULL\0ERROR_INSUFFICIENT_BUFFER\0"
    "ERROR_ALREADY_EXISTS\0ERROR_OPERATION_ABORTED\0ERROR_CANCELLED\0ERROR_TIMEOUT\0WSAECONNRESET\0WSAETIMEDOUT\0"
    "WSAECONNREFUSED\0WSAHOST_NOT_FOUND\0"
    "E_NOTIMPL\0E_NOINTERFACE\0E_FAIL\0E_UNEXPECTED\0DXGI_ERROR_INVALID_CALL\0DXGI_ERROR_UNSUPPORTED\0"
    "DXGI_ERROR_DEVICE_REMOVED\0DXGI_ERROR_DEVICE_HUNG\0DXGI_ERROR_DEVICE_RESET\0DXGI_ERROR_WAS_STILL_DRAWING\0"
    "DXGI_ERROR_DRIVER_INTERNAL_ERROR\0"
#if !defined(_WIN32)
    "EINTR\0EIO\0EAGAIN\0EBUSY\0ENODEV\0EMFILE\0ENOSPC\0EPIPE\0ENETUNREACH\0ECONNRESET\0ECONNREFUSED\0EHOSTUNREACH\0"
#endif
    ;
static_assert(sizeof(kLogErrorCodes) / sizeof(kLogErrorCodes[0]) == name_list_count(kLogErrorNames));

// The symbolic name of `code` (see kLogErrorCodes), or null.
__declspec(noinline) inline const char* log_error_name(u32 code) noexcept {
    if ((code & 0xFFFF0000u) == 0x80070000u)
        code &= 0xFFFFu;
    NameList names{kLogErrorNames};
    for (const u32 known : kLogErrorCodes) {
        if (known == code)
            return names.at;
        names.next();
    }
    return nullptr;
}

// One log event, "<event> key=value key=value ...", formatted on the caller's stack and written as one line under
// one lock (log_write): a warning or error with its details is one line, never a line per value. Keys and string
// values are static labels and non-sensitive text: never tokens, passwords, signed URLs, user paths or message
// content. What does not fit is dropped.
struct LogEvent {
    char text[960]; // a log line holds 1023 bytes, its time stamp, level and component included
    u32 used = 0;

    explicit LogEvent(const char* event) noexcept {
        append(event);
    }
    // Appends text and keeps the record terminated.
    __declspec(noinline) void append(const char* value) noexcept {
        u32 cursor = used;
        while (value && *value && cursor + 1 < sizeof(text))
            text[cursor++] = *value++;
        text[cursor] = 0;
        used = cursor;
    }
    __declspec(noinline) LogEvent& field(const char* key, const char* value) noexcept {
        append(" ");
        append(key);
        append("=");
        append(value);
        return *this;
    }
    __declspec(noinline) LogEvent& field(const char* key, u64 value) noexcept {
        char digits[21];
        digits[decimal_write(digits, value)] = 0;
        return field(key, digits);
    }
    // `key="value"`, for a value with spaces (a processor or GPU model, a distribution); a '"' in it becomes '\''.
    __declspec(noinline) LogEvent& quoted(const char* key, const char* value) noexcept {
        append(" ");
        append(key);
        append("=\"");
        u32 cursor = used;
        for (; value && *value && cursor + 1 < sizeof(text); ++value)
            text[cursor++] = *value == '"' ? '\'' : *value;
        text[cursor] = 0;
        used = cursor;
        append("\"");
        return *this;
    }
    // "key=0x1F": masks, handles and identifiers that read better in hex.
    __declspec(noinline) LogEvent& hex(const char* key, u64 value) noexcept {
        char digits[19] = {'0', 'x'};
        u32 count = 1;
        while (count < 16 && (value >> (count * 4)) != 0)
            ++count;
        for (u32 index = 0; index < count; ++index)
            digits[2 + index] = ascii_hex_digit_upper(static_cast<u32>(value >> ((count - 1 - index) * 4)));
        digits[2 + count] = 0;
        return field(key, digits);
    }
    // An error code with its name when it is a common one: "native_error=5 ERROR_ACCESS_DENIED",
    // "hr=0x887A0005 DXGI_ERROR_DEVICE_REMOVED". HRESULTs and Linux native errors are hex, Win32 errors decimal.
    __declspec(noinline) LogEvent& error(const char* key, u32 code) noexcept {
        if (code >= 0x10000u) {
            char digits[11] = {'0', 'x'};
            for (u32 index = 0; index < 8; ++index)
                digits[2 + index] = ascii_hex_digit_upper(code >> (28 - index * 4));
            digits[10] = 0;
            field(key, digits);
        } else {
            field(key, u64{code});
        }
        if (const char* name = log_error_name(code)) {
            append(" ");
            append(name);
        }
        return *this;
    }
    // `count` values, each under the next key of the NUL-separated `keys` list ("first\0second": the literal's own
    // terminator closes the last key), so a group of values costs one literal instead of a call each.
    __declspec(noinline) LogEvent& fields(const char* keys, const u64* values, u32 count) noexcept {
        for (u32 index = 0; index < count; ++index) {
            field(keys, values[index]);
            while (*keys++) {
            }
        }
        return *this;
    }
    template <u32 N> LogEvent& fields(const char* keys, const u64 (&values)[N]) noexcept {
        return fields(keys, values, N);
    }
    __declspec(noinline) void write(LogLevel level, const char* component) const noexcept {
        log_write(level, component, text);
    }
};

} // namespace maicord
