#include "maicord/platform/browser_sign_in_launcher.hpp"
#include "maicord/sys/shell.hpp"

namespace maicord {
namespace {
bool locked_url(const SecureBuffer* url) noexcept {
    return url && url->locked && url->data && url->bytes && url->bytes <= url->capacity;
}
bool matches(const SecureBuffer* url, u32 offset, const char* literal, u32 bytes) noexcept {
    if (offset > url->bytes || bytes > url->bytes - offset) return false;
    for (u32 i = 0; i < bytes; ++i)
        if (url->data[offset + i] != static_cast<u8>(literal[i])) return false;
    return true;
}
// The validated URL is ASCII; widen it into locked storage for the shell.
bool open_validated(HWND window, const SecureBuffer* url) noexcept {
    SecureBuffer wide{};
    if (!secure_buffer_initialize(&wide, (url->bytes + 1) * sizeof(wchar_t)).ok())
        return false;
    auto* characters = reinterpret_cast<wchar_t*>(wide.data);
    for (u32 i = 0; i < url->bytes; ++i)
        characters[i] = static_cast<wchar_t>(url->data[i]);
    characters[url->bytes] = 0;
    const auto opened = reinterpret_cast<usize>(
        ShellExecuteW(window, L"open", characters, nullptr, nullptr, SW_SHOWNORMAL));
    secure_buffer_release(&wide);
    return opened > 32;
}
} // namespace

bool browser_sign_in_launch(HWND window, const SecureBuffer* url) noexcept {
    constexpr char prefix[] = "https://discord.com/login?redirect_to=%2Fhandoff%3Frpc%3D";
    constexpr char middle[] = "%26key%3D";
    constexpr u32 prefix_bytes = sizeof(prefix) - 1;
    constexpr u32 middle_bytes = sizeof(middle) - 1;
    constexpr u32 url_bytes = prefix_bytes + 4 + middle_bytes + 36;
    if (!locked_url(url) || url->bytes != url_bytes || !matches(url, 0, prefix, prefix_bytes))
        return false;
    u32 port = 0;
    for (u32 i = 0; i < 4; ++i) {
        const u8 digit = url->data[prefix_bytes + i];
        if (digit < '0' || digit > '9')
            return false;
        port = port * 10 + digit - '0';
    }
    if (port < 6463 || port > 6472 || !matches(url, prefix_bytes + 4, middle, middle_bytes))
        return false;
    for (u32 i = 0; i < 36; ++i) {
        const u8 c = url->data[prefix_bytes + 4 + middle_bytes + i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return open_validated(window, url);
}

bool captcha_bridge_url_valid(const SecureBuffer* url) noexcept {
    constexpr char prefix[] = "http://127.0.0.1:";
    constexpr u32 prefix_bytes = sizeof(prefix) - 1;
    if (!locked_url(url) || !matches(url, 0, prefix, prefix_bytes))
        return false;
    u32 cursor = prefix_bytes, port = 0;
    while (cursor < url->bytes && url->data[cursor] >= '0' && url->data[cursor] <= '9' && cursor - prefix_bytes < 5)
        port = port * 10 + url->data[cursor++] - '0';
    const u32 digits = cursor - prefix_bytes;
    if (!digits || url->data[prefix_bytes] == '0' || port < 1024 || port > 65535 ||
        !matches(url, cursor, "/c/", 3) || url->bytes != cursor + 3 + 32)
        return false;
    for (u32 i = cursor + 3; i < url->bytes; ++i) {
        const u8 c = url->data[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

bool captcha_bridge_launch(HWND window, const SecureBuffer* url) noexcept {
    return captcha_bridge_url_valid(url) && open_validated(window, url);
}
} // namespace maicord
