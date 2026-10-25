#include "maicord/app/link_trust.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/win32_process.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
constexpr u8 kMagic[] = {'M', 'C', 'L', 'T'};
constexpr u8 kVersion = 1;
constexpr u32 kMaximumUrlBytes = 2048;

bool ends_with(Utf8View text, Utf8View suffix) noexcept {
    return suffix.bytes <= text.bytes && bytes_equal(text.data + text.bytes - suffix.bytes, suffix.data, suffix.bytes);
}
// A stored host or protocol: valid UTF-8, no ASCII control, space or URL
// delimiter, and no ASCII capitals (callers store the parsed lowercase form).
bool entry_valid(Utf8View text, u32 capacity) noexcept {
    if (!text.data || !text.bytes || text.bytes > capacity || !utf8_validate(text).valid)
        return false;
    for (u32 i = 0; i < text.bytes; ++i) {
        const u8 c = static_cast<u8>(text.data[i]);
        if (c <= 0x20 || c == 0x7f || c == '/' || c == '?' || c == '#' || c == '@' || c == '\\' ||
            (c >= 'A' && c <= 'Z'))
            return false;
    }
    return true;
}
bool protocol_valid(Utf8View protocol) noexcept {
    if (!entry_valid(protocol, kLinkTrustProtocolBytes) || protocol.bytes < 2 ||
        protocol.data[protocol.bytes - 1] != ':')
        return false;
    for (u32 i = 0; i + 1 < protocol.bytes; ++i) {
        const char c = protocol.data[i];
        const bool letter = c >= 'a' && c <= 'z';
        if (!letter && (i == 0 || !((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')))
            return false;
    }
    return true;
}
u32 checksum(const u8* data, u32 bytes) noexcept {
    u32 hash = 2166136261u;
    for (u32 i = 0; i < bytes; ++i)
        hash = (hash ^ data[i]) * 16777619u;
    return hash;
}
// Scheme, then for "//" URLs the authority's start and end.
struct UrlParts {
    u32 scheme_end; // index of ':'
    u32 authority_begin, authority_end;
    bool authority;
};
bool split_url(Utf8View url, UrlParts* parts) noexcept {
    if (!url.data || !url.bytes || url.bytes > kMaximumUrlBytes || !utf8_validate(url).valid)
        return false;
    for (u32 i = 0; i < url.bytes; ++i) {
        const u8 c = static_cast<u8>(url.data[i]);
        if (c <= 0x20 || c == 0x7f)
            return false;
    }
    u32 colon = 0;
    while (colon < url.bytes && url.data[colon] != ':')
        ++colon;
    if (!colon || colon == url.bytes || colon + 1 >= kLinkTrustProtocolBytes)
        return false;
    *parts = {colon, 0, 0, false};
    if (colon + 2 < url.bytes && url.data[colon + 1] == '/' && url.data[colon + 2] == '/') {
        u32 end = colon + 3;
        while (end < url.bytes && url.data[end] != '/' && url.data[end] != '?' && url.data[end] != '#')
            ++end;
        *parts = {colon, colon + 3, end, true};
    }
    return true;
}
} // namespace

bool link_target_parse(Utf8View url, LinkTarget* target) noexcept {
    UrlParts parts{};
    if (!target || !split_url(url, &parts))
        return false;
    LinkTarget parsed{};
    for (u32 i = 0; i < parts.scheme_end; ++i)
        parsed.protocol[i] = ascii_lower(url.data[i]);
    parsed.protocol[parts.scheme_end] = ':';
    parsed.protocol_bytes = static_cast<u8>(parts.scheme_end + 1);
    if (!protocol_valid(link_target_protocol(parsed)))
        return false;
    parsed.web = utf8_equal(link_target_protocol(parsed), utf8_literal("https:")) ||
                 utf8_equal(link_target_protocol(parsed), utf8_literal("http:"));
    if (parts.authority) {
        u32 begin = parts.authority_begin;
        for (u32 i = parts.authority_begin; i < parts.authority_end; ++i)
            if (url.data[i] == '@')
                begin = i + 1;
        u32 end = begin;
        if (end < parts.authority_end && url.data[end] == '[') {
            while (end < parts.authority_end && url.data[end] != ']')
                ++end;
            if (end == parts.authority_end)
                return false;
            ++end;
        } else {
            while (end < parts.authority_end && url.data[end] != ':')
                ++end;
        }
        // "example.com." names the same host.
        u32 host_end = end;
        if (host_end > begin && url.data[host_end - 1] == '.')
            --host_end;
        if (host_end - begin > kLinkTrustHostBytes)
            return false;
        for (u32 i = begin; i < host_end; ++i)
            parsed.host[i - begin] = ascii_lower(url.data[i]);
        parsed.host_bytes = static_cast<u8>(host_end - begin);
    }
    if (parsed.web && (!parsed.host_bytes || !entry_valid(link_target_host(parsed), kLinkTrustHostBytes)))
        return false;
    copy_value(target, parsed);
    return true;
}

bool link_trust_builtin_host(Utf8View host) noexcept {
    static constexpr Utf8View exact[] = {utf8_literal("discord.gg"),           utf8_literal("discord.gift"),
                                  utf8_literal("discord.new"),          utf8_literal("media.discordapp.net"),
                                  utf8_literal("open.spotify.com"),     utf8_literal("www.spotify.com")};
    for (const auto value : exact)
        if (utf8_equal(host, value))
            return true;
    // /(?:^|\.)(?:discordapp|discord|discordmerch)\.com$/
    static constexpr Utf8View owned[] = {utf8_literal("discordapp.com"), utf8_literal("discord.com"),
                                  utf8_literal("discordmerch.com")};
    for (const auto value : owned)
        if (utf8_equal(host, value) ||
            (ends_with(host, value) && host.bytes > value.bytes && host.data[host.bytes - value.bytes - 1] == '.'))
            return true;
    return false;
}

bool link_trust_has_host(const LinkTrustStore& store, Utf8View host) noexcept {
    for (u32 i = 0; i < store.host_count && i < kLinkTrustHostCapacity; ++i)
        if (utf8_equal(host, {store.hosts[i], store.host_bytes[i]}))
            return true;
    return false;
}

bool link_trust_has_protocol(const LinkTrustStore& store, Utf8View protocol) noexcept {
    for (u32 i = 0; i < store.protocol_count && i < kLinkTrustProtocolCapacity; ++i)
        if (utf8_equal(protocol, {store.protocols[i], store.protocol_bytes[i]}))
            return true;
    return false;
}

bool link_trust_add_host(LinkTrustStore* store, Utf8View host) noexcept {
    if (!store || !entry_valid(host, kLinkTrustHostBytes))
        return false;
    if (link_trust_has_host(*store, host))
        return true;
    if (store->host_count >= kLinkTrustHostCapacity)
        return false;
    copy_bytes(store->hosts[store->host_count], host.data, host.bytes);
    store->host_bytes[store->host_count++] = static_cast<u8>(host.bytes);
    ++store->revision;
    return true;
}

bool link_trust_add_protocol(LinkTrustStore* store, Utf8View protocol) noexcept {
    if (!store || !protocol_valid(protocol))
        return false;
    if (link_trust_has_protocol(*store, protocol))
        return true;
    if (store->protocol_count >= kLinkTrustProtocolCapacity)
        return false;
    copy_bytes(store->protocols[store->protocol_count], protocol.data, protocol.bytes);
    store->protocol_bytes[store->protocol_count++] = static_cast<u8>(protocol.bytes);
    ++store->revision;
    return true;
}

LinkOpenDecision link_open_decision(const LinkTrustStore* store, Utf8View url, bool trusted_text) noexcept {
    LinkTarget target{};
    if (!link_target_parse(url, &target))
        return LinkOpenDecision::reject;
    if (!target.web)
        return store && link_trust_has_protocol(*store, link_target_protocol(target)) ? LinkOpenDecision::open
                                                                                       : LinkOpenDecision::warn_protocol;
    const auto host = link_target_host(target);
    return trusted_text || link_trust_builtin_host(host) || (store && link_trust_has_host(*store, host))
               ? LinkOpenDecision::open
               : LinkOpenDecision::warn_website;
}

u32 link_display_url(Utf8View url, Slice<char> output) noexcept {
    UrlParts parts{};
    if (!output.data || !split_url(url, &parts))
        return 0;
    u32 skip_begin = 0, skip_end = 0;
    if (parts.authority)
        for (u32 i = parts.authority_begin; i < parts.authority_end; ++i)
            if (url.data[i] == '@') {
                skip_begin = parts.authority_begin;
                skip_end = i + 1;
            }
    const u32 bytes = url.bytes - (skip_end - skip_begin);
    if (bytes > output.count)
        return 0;
    u32 used = 0;
    for (u32 i = 0; i < url.bytes; ++i)
        if (i < skip_begin || i >= skip_end)
            output[used++] = url.data[i];
    return used;
}

u32 link_trust_encode(const LinkTrustStore& store, Slice<u8> output) noexcept {
    if (store.host_count > kLinkTrustHostCapacity || store.protocol_count > kLinkTrustProtocolCapacity ||
        !output.data)
        return 0;
    u32 used = 0;
    const auto put = [&](const void* data, u32 bytes) noexcept {
        if (bytes > output.count - used)
            return false;
        copy_bytes(output.data + used, data, bytes);
        used += bytes;
        return true;
    };
    const u8 header[] = {kMagic[0], kMagic[1], kMagic[2], kMagic[3], kVersion,
                         static_cast<u8>(store.host_count), store.protocol_count, 0};
    if (!put(header, sizeof(header)))
        return 0;
    for (u32 i = 0; i < store.host_count; ++i) {
        const Utf8View host{store.hosts[i], store.host_bytes[i]};
        const u8 length = store.host_bytes[i];
        if (!entry_valid(host, kLinkTrustHostBytes) || !put(&length, 1) || !put(host.data, host.bytes))
            return 0;
    }
    for (u32 i = 0; i < store.protocol_count; ++i) {
        const Utf8View protocol{store.protocols[i], store.protocol_bytes[i]};
        const u8 length = store.protocol_bytes[i];
        if (!protocol_valid(protocol) || !put(&length, 1) || !put(protocol.data, protocol.bytes))
            return 0;
    }
    const u32 hash = checksum(output.data, used);
    const u8 tail[] = {static_cast<u8>(hash), static_cast<u8>(hash >> 8), static_cast<u8>(hash >> 16),
                       static_cast<u8>(hash >> 24)};
    return put(tail, 4) ? used : 0;
}

bool link_trust_decode(ConstSlice<u8> input, LinkTrustStore* store) noexcept {
    if (!store || !input.data || input.count < 12 || input.count > kLinkTrustFileBytes)
        return false;
    const u32 body = input.count - 4;
    const u32 stored = input[body] | (u32{input[body + 1]} << 8) | (u32{input[body + 2]} << 16) |
                       (u32{input[body + 3]} << 24);
    if (!bytes_equal(input.data, kMagic, 4) || input[4] != kVersion || input[5] > kLinkTrustHostCapacity ||
        input[6] > kLinkTrustProtocolCapacity || input[7] != 0 || stored != checksum(input.data, body))
        return false;
    // Large decode scratch stays off the stack; the caller's store is only
    // replaced once the whole record is valid.
    ScratchPages<LinkTrustStore> scratch;
    if (!scratch.data)
        return false;
    LinkTrustStore& decoded = *scratch.data;
    u32 at = 8;
    const auto entry = [&](Utf8View* value) noexcept {
        if (at >= body)
            return false;
        const u32 bytes = input[at++];
        if (bytes > body - at)
            return false;
        *value = {reinterpret_cast<const char*>(input.data + at), bytes};
        at += bytes;
        return true;
    };
    for (u32 i = 0; i < input[5]; ++i) {
        Utf8View host{};
        if (!entry(&host) || link_trust_has_host(decoded, host) || !link_trust_add_host(&decoded, host))
            return false;
    }
    for (u32 i = 0; i < input[6]; ++i) {
        Utf8View protocol{};
        if (!entry(&protocol) || link_trust_has_protocol(decoded, protocol) ||
            !link_trust_add_protocol(&decoded, protocol))
            return false;
    }
    if (at != body)
        return false;
    decoded.revision = store->revision + 1;
    *store = decoded;
    return true;
}

bool link_trust_path(u64 profile_id, wchar_t* path, u32 capacity) noexcept {
    if (!profile_id)
        return false;
    const DataPathResult built = data_path(path, capacity, L"accounts\\links-");
    u32 length = built.length;
    return built.ok() && wide_append_hex(path, capacity, &length, profile_id) &&
           wide_append(path, capacity, &length, L".trust");
}

bool link_trust_load_file(const wchar_t* path, LinkTrustStore* store) noexcept {
    if (!path || !store)
        return false;
    const u32 revision = store->revision;
    reset_value(store);
    store->revision = revision + 1;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const u32 error = thread_last_error();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    Win32LargeInteger size{};
    ScratchPages<u8[kLinkTrustFileBytes]> record;
    u32 native_error = 0;
    const bool read =
        record.data && GetFileSizeEx(file, size.native()) && size.QuadPart > 0 &&
        size.QuadPart <= kLinkTrustFileBytes &&
        file_persistence_read_complete(file, {*record.data, static_cast<u32>(size.QuadPart)}, &native_error);
    CloseHandle(file);
    return read && link_trust_decode({*record.data, static_cast<u32>(size.QuadPart)}, store);
}

bool link_trust_save_file(const wchar_t* path, const LinkTrustStore& store) noexcept {
    ScratchPages<u8[kLinkTrustFileBytes]> record;
    const u32 bytes = record.data ? link_trust_encode(store, {*record.data, kLinkTrustFileBytes}) : 0;
    const u32 length = wide_length(path, 32768);
    return bytes && length && file_persistence_write_atomic(path, length, {*record.data, bytes}).ok();
}

} // namespace maicord
