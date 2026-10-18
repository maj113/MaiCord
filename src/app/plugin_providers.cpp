#include "maicord/app/plugin_providers.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/app/plugin_index.hpp"
#include "maicord/app/update_fetch.hpp"
#include "maicord/app/update_log.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {
namespace {
using RE = AcceptanceRecordError;
constexpr u8 kMagic[8]{'M','A','I','P','P','R','O','V'};
AcceptanceRecordResult fail(RE error, DWORD native = 0) noexcept { return {error, native}; }
// "/" or "/segment/.../" of RFC 3986 path characters, without empty, "." or ".." segments.
bool base_path_valid(const char* path, u32 bytes) noexcept {
    if (!bytes || bytes > kPluginProviderPathCapacity || path[0] != '/' || path[bytes - 1] != '/') return false;
    u32 start = 1;
    for (u32 i = 1; i < bytes; ++i) {
        const char c = path[i];
        if (c == '/') {
            const u32 n = i - start;
            if (!n || (n == 1 && path[start] == '.') || (n == 2 && path[start] == '.' && path[start + 1] == '.')) return false;
            start = i + 1;
            continue;
        }
        bool allowed = ascii_is_alnum(c);
        for (const char* a = "-._~!$&'()*+,;=:@%"; *a && !allowed; ++a) allowed = c == *a;
        if (!allowed) return false;
    }
    return true;
}
// Field by field: the struct has tail padding.
bool entry_empty(const PluginProviderRecord& e) noexcept {
    return !e.id && !e.added && !e.flags && !e.host_bytes && !e.path_bytes && !e.name_bytes && bytes_zero(e.host, sizeof(e.host)) &&
        bytes_zero(e.path, sizeof(e.path)) && bytes_zero(e.key, sizeof(e.key)) && bytes_zero(e.fingerprint, sizeof(e.fingerprint)) &&
        bytes_zero(e.name, sizeof(e.name));
}
bool entry_valid(const PluginProviderRecord& e) noexcept {
    if (e.id <= kPluginProviderOfficial || (e.flags & ~kPluginProviderKnownFlags) || !e.host_bytes ||
        e.host_bytes > kPluginProviderHostCapacity || e.path_bytes > kPluginProviderPathCapacity ||
        e.name_bytes > kPluginProviderNameCapacity) return false;
    for (u32 i = 0; i < e.host_bytes; ++i) if (e.host[i] >= 'A' && e.host[i] <= 'Z') return false;
    if (!update_origin_host_valid({e.host, e.host_bytes}) || !base_path_valid(e.path, e.path_bytes) ||
        !plugin_index_display_text_valid({e.name, e.name_bytes}, kPluginProviderNameCapacity, true)) return false;
    const bool signed_provider = e.flags & kPluginProviderSigned;
    if (signed_provider ? e.key[0] != 4 || bytes_zero(e.fingerprint, 32) : !bytes_zero(e.key, 65) || !bytes_zero(e.fingerprint, 32)) return false;
    return bytes_zero(e.host + e.host_bytes, kPluginProviderHostCapacity - e.host_bytes) &&
        bytes_zero(e.path + e.path_bytes, kPluginProviderPathCapacity - e.path_bytes) &&
        bytes_zero(e.name + e.name_bytes, kPluginProviderNameCapacity - e.name_bytes);
}

// ---- addresses -------------------------------------------------------------------
using AE = PluginProviderAddressError;
// RFC 3986 pchar without percent-encoding: provider bases are plain paths.
bool path_char(char c) noexcept {
    if (ascii_is_alnum(c)) return true;
    for (const char* a = "-._~!$&'()*+,;=:@"; *a; ++a) if (c == *a) return true;
    return false;
}
bool sha256_parts(KdfSha256Workspace& workspace, Utf8View domain, ConstSlice<u8> data, u8* digest) noexcept {
    const ConstSlice<u8> parts[]{{reinterpret_cast<const u8*>(domain.data), domain.bytes}, data};
    return sha256(workspace, {parts, 2}, {digest, 32}).ok();
}

// ---- probe -----------------------------------------------------------------------
using PE = PluginProviderProbeError;
PluginProviderProbe probe_failed(PluginProviderProbe r, PE error, const char* phase) noexcept {
    r.error = error;
    UpdateLog line("plugins", "provider_probe");
    line.add(" phase=").add(phase);
    line.field("error", static_cast<u8>(error)).field("fetch", static_cast<u8>(r.fetch_error))
        .field("index", static_cast<u8>(r.index_error)).field("status", r.status).field("native", r.native_error);
    if (r.retry_at_unix) line.field("retry_at", r.retry_at_unix);
    line.write(LogLevel::warning);
    return r;
}
bool fetched(PluginProviderProbe* r, const UpdateFetchResult& f) noexcept {
    ++r->requests;
    r->fetch_error = f.error; r->status = f.status; r->native_error = f.native_error; r->retry_at_unix = f.retry_at_unix;
    return f.ok();
}
} // namespace

PluginProviderAddressError plugin_provider_address_parse(Utf8View text, KdfSha256Workspace& workspace,
    PluginProviderAddress* output) noexcept {
    if (!output || (!text.data && text.bytes)) return AE::invalid_input;
    const ConstSlice<u8> ranges[]{{reinterpret_cast<const u8*>(text.data), text.bytes},
        {reinterpret_cast<const u8*>(output), sizeof(*output)}, {reinterpret_cast<const u8*>(&workspace), sizeof(workspace)}};
    if (!byte_ranges_all_disjoint({ranges, 3})) return AE::invalid_input;
    if (text.bytes > kPluginProviderAddressMaximumBytes) return AE::length;
    u32 begin = 0, end = text.bytes;
    while (begin < end && (text.data[begin] == ' ' || text.data[begin] == '\t')) ++begin;
    while (end > begin && (text.data[end - 1] == ' ' || text.data[end - 1] == '\t')) --end;
    if (begin == end) return AE::length;
    const char* s = text.data + begin;
    const u32 n = end - begin;
    for (u32 i = 0; i < n; ++i) if (static_cast<u8>(s[i]) < 0x21 || static_cast<u8>(s[i]) > 0x7E) return AE::character;
    constexpr Utf8View scheme = utf8_literal("https://");
    if (n < scheme.bytes || !ascii_equal_ignore_case({s, scheme.bytes}, scheme)) return AE::scheme;
    u32 at = scheme.bytes;
    while (at < n && s[at] != '/' && s[at] != '?' && s[at] != '#') ++at;
    const Utf8View authority{s + scheme.bytes, at - scheme.bytes};
    for (u32 i = 0; i < authority.bytes; ++i) if (authority.data[i] == '@') return AE::userinfo;
    if (authority.bytes && authority.data[0] == '[') return AE::host;
    for (u32 i = 0; i < authority.bytes; ++i) if (authority.data[i] == ':') return AE::port;
    if (!update_origin_host_valid(authority)) return AE::host;
    PluginProviderAddress a{};
    for (u32 i = 0; i < authority.bytes; ++i) a.host[i] = ascii_lower(authority.data[i]);
    a.host_bytes = authority.bytes;
    u32 path_end = at;
    while (path_end < n && s[path_end] != '?' && s[path_end] != '#') ++path_end;
    if (path_end < n && s[path_end] == '?') return AE::query;
    if (at == path_end) {
        a.path[0] = '/';
        a.path_bytes = 1;
    } else {
        // Segments between slashes: none empty (a final empty one is the
        // trailing '/'), none "." or "..", plain path characters only.
        for (u32 i = at + 1, start = at + 1; i <= path_end; ++i) {
            if (i < path_end && s[i] != '/') {
                if (!path_char(s[i])) return AE::path;
                continue;
            }
            const u32 length = i - start;
            if (!length && i != path_end) return AE::path;
            if ((length == 1 && s[start] == '.') || (length == 2 && s[start] == '.' && s[start + 1] == '.')) return AE::path;
            start = i + 1;
        }
        const bool slash = s[path_end - 1] == '/';
        const u32 bytes = path_end - at + (slash ? 0 : 1);
        if (bytes > kPluginProviderPathCapacity) return AE::length;
        copy_bytes(a.path, s + at, path_end - at);
        if (!slash) a.path[bytes - 1] = '/';
        a.path_bytes = bytes;
    }
    if (path_end < n) { // '#': only the key pin
        constexpr Utf8View pin = utf8_literal("sha256=");
        const char* fragment = s + path_end + 1;
        const u32 fragment_bytes = n - path_end - 1;
        if (fragment_bytes < pin.bytes || !bytes_equal(fragment, pin.data, pin.bytes)) return AE::fragment;
        if (fragment_bytes != pin.bytes + 64) return AE::pin;
        for (u32 i = 0; i < 64; ++i) {
            const i32 v = ascii_hex_value(fragment[pin.bytes + i]);
            if (v < 0) return AE::pin;
            a.pin[i / 2] = static_cast<u8>(i % 2 ? (a.pin[i / 2] << 4) | v : v);
        }
        a.has_pin = true;
    }
    char base[kPluginProviderBaseMaximumBytes];
    const u32 base_bytes = plugin_provider_address_text(a, {base, sizeof(base)});
    u8 digest[32]{};
    if (!base_bytes || !sha256_parts(workspace, kPluginProviderIdDomain, {reinterpret_cast<const u8*>(base), base_bytes}, digest))
        return AE::invalid_input;
    a.id = static_cast<u64>(byte_load_be32(digest)) << 32 | byte_load_be32(digest + 4);
    if (a.id <= kPluginProviderOfficial) return AE::host; // 0 and 1 are reserved (a 2^-63 chance)
    *output = a;
    return AE::none;
}

u32 plugin_provider_address_text(const PluginProviderAddress& a, Slice<char> output) noexcept {
    constexpr Utf8View scheme = utf8_literal("https://");
    if (!output.data || !a.host_bytes || a.host_bytes > kPluginProviderHostCapacity || !a.path_bytes ||
        a.path_bytes > kPluginProviderPathCapacity) return 0;
    const u32 bytes = scheme.bytes + a.host_bytes + a.path_bytes;
    if (bytes > output.count) return 0;
    copy_bytes(output.data, scheme.data, scheme.bytes);
    copy_bytes(output.data + scheme.bytes, a.host, a.host_bytes);
    copy_bytes(output.data + scheme.bytes + a.host_bytes, a.path, a.path_bytes);
    return bytes;
}

bool plugin_provider_index_location(Utf8View host, Utf8View path, bool signature, UpdateLocation* output) noexcept {
    constexpr Utf8View leaf = utf8_literal("v1/index.json.sig");
    const u32 leaf_bytes = signature ? leaf.bytes : leaf.bytes - 4;
    if (!output || !path.data || !base_path_valid(path.data, path.bytes)) return false;
    char target[kPluginProviderPathCapacity + 17];
    copy_bytes(target, path.data, path.bytes);
    copy_bytes(target + path.bytes, leaf.data, leaf_bytes);
    return update_provider_location(host, {target, path.bytes + leaf_bytes}, output);
}

bool plugin_provider_fingerprint(KdfSha256Workspace& workspace, const u8* key, u8* fingerprint) noexcept {
    if (!key || !fingerprint || key[0] != 4) return false;
    u8 digest[32]{};
    if (!sha256_parts(workspace, kPluginProviderKeyDomain, {key, 65}, digest)) return false;
    copy_bytes(fingerprint, digest, 32);
    return true;
}

u32 plugin_provider_fingerprint_text(const u8* fingerprint, Slice<char> output) noexcept {
    if (!fingerprint || !output.data || output.count < kPluginProviderFingerprintTextBytes) return 0;
    constexpr char digits[] = "0123456789ABCDEF";
    u32 at = 0;
    for (u32 i = 0; i < 16; ++i) {
        if (i && !(i % 2)) output[at++] = ' ';
        output[at++] = digits[fingerprint[i] >> 4];
        output[at++] = digits[fingerprint[i] & 15];
    }
    return at;
}

PluginProviderProbe plugin_provider_probe(const PluginProviderAddress& address, const UpdateTransport& transport,
    KdfSha256Workspace& workspace, PluginIndexWork* work, bool allow_unsigned, u64 now) noexcept {
    PluginProviderProbe r{};
    UpdateLocation location{};
    const Utf8View host{address.host, address.host_bytes}, path{address.path, address.path_bytes};
    if (!work || !transport.get || !now || address.id <= kPluginProviderOfficial || address.host_bytes > kPluginProviderHostCapacity ||
        !plugin_provider_index_location(host, path, false, &location)) { r.error = PE::invalid_input; return r; }
    UpdateTransport plugins = transport;
    plugins.log_category = "plugins";
    clear_bytes(work->record, 64);
    work->document_bytes = 0;
    UpdateBufferSink document{work->record + 64, kPluginIndexMaximumBytes, 0};
    if (!fetched(&r, update_fetch(plugins, location, kPluginIndexMaximumBytes, 0, update_buffer_sink, &document, now)))
        return probe_failed(r, PE::fetch, "index");
    work->document_bytes = document.used;
    const Utf8View text = plugin_index_work_document(*work);
    const Slice<u32> scratch{work->scratch, kPluginIndexStructuralCapacity};
    PluginIndexPolicy policy{};
    policy.now_unix = now;
    const auto peeked = plugin_index_unverified_key(text, scratch, r.key);
    if (peeked == PluginIndexKeyPeek::malformed) return probe_failed(r, PE::unreadable, "key");
    if (peeked == PluginIndexKeyPeek::absent) {
        // A share link's pin promises a signing key.
        if (address.has_pin) return probe_failed(r, PE::pin, "pin");
        if (!allow_unsigned) return probe_failed(r, PE::unsigned_provider, "unsigned");
        policy.kind = PluginProviderKind::custom_unsigned;
        r.index_error = plugin_index_parse_unsigned(text, workspace, policy, scratch, &work->index).error;
    } else {
        if (!plugin_provider_fingerprint(workspace, r.key, r.fingerprint)) return probe_failed(r, PE::invalid_input, "fingerprint");
        if (address.has_pin && !bytes_equal(address.pin, r.fingerprint, 32)) return probe_failed(r, PE::pin, "pin");
        if (!plugin_provider_index_location(host, path, true, &location)) return probe_failed(r, PE::invalid_input, "location");
        UpdateBufferSink signature{work->record, 64, 0};
        if (!fetched(&r, update_fetch(plugins, location, 64, 64, update_buffer_sink, &signature, now)))
            return probe_failed(r, PE::fetch, "signature");
        policy.kind = PluginProviderKind::custom_signed;
        copy_bytes(policy.pinned_key, r.key, 65);
        P256Key key{};
        if (!p256_import_public(&key, P256Kind::ecdsa, {r.key, 65}).ok()) return probe_failed(r, PE::unreadable, "key");
        r.index_error = plugin_index_verify(text, {work->record, 64}, key, workspace, policy, scratch, &work->index).error;
        p256_release(&key);
        r.signed_provider = r.index_error == PluginIndexError::none;
        r.pin_matched = r.signed_provider && address.has_pin;
    }
    if (r.index_error != PluginIndexError::none) return probe_failed(r, PE::document, "verify");
    r.sequence = work->index.sequence;
    r.plugin_count = work->index.plugin_count;
    UpdateLog("plugins", "provider_probe").host("host", host).field("signed", r.signed_provider).field("pinned", r.pin_matched)
        .field("sequence", r.sequence).field("plugins", r.plugin_count).field("requests", r.requests).write();
    return r;
}

const PluginProviderRecord* plugin_providers_find(const PluginProviders& providers, u64 id) noexcept {
    for (u32 i = 0; i < providers.count && i < kPluginProviderMaximum; ++i)
        if (providers.entries[i].id == id) return &providers.entries[i];
    return nullptr;
}

PluginProviderInsertError plugin_providers_insert(PluginProviders* providers, const PluginProviderAddress& address,
    const PluginProviderProbe& probe, Utf8View name, u64 now) noexcept {
    using IE = PluginProviderInsertError;
    if (!providers || probe.error != PE::none || !now || !name.data || !name.bytes || name.bytes > kPluginProviderNameCapacity ||
        address.host_bytes > kPluginProviderHostCapacity || address.path_bytes > kPluginProviderPathCapacity)
        return IE::invalid_input;
    if (plugin_providers_find(*providers, address.id)) return IE::duplicate;
    if (providers->count >= kPluginProviderMaximum) return IE::capacity;
    PluginProviders next = *providers;
    PluginProviderRecord& e = next.entries[next.count++];
    e = {};
    e.id = address.id;
    e.added = now;
    e.flags = kPluginProviderEnabled | kPluginProviderCheckDaily | (probe.signed_provider ? kPluginProviderSigned : 0);
    e.host_bytes = address.host_bytes;
    e.path_bytes = address.path_bytes;
    e.name_bytes = name.bytes;
    copy_bytes(e.host, address.host, address.host_bytes);
    copy_bytes(e.path, address.path, address.path_bytes);
    copy_bytes(e.name, name.data, name.bytes);
    if (probe.signed_provider) {
        copy_bytes(e.key, probe.key, 65);
        copy_bytes(e.fingerprint, probe.fingerprint, 32);
    }
    if (!plugin_providers_valid(next)) return IE::invalid_input;
    *providers = next;
    return IE::none;
}

u64 plugin_record_checksum(ConstSlice<u8> record) noexcept {
    return hash64_bytes(hash64_bytes(kHash64Seed, record.data, 24), record.data + 32, record.count - 32);
}

AcceptanceRecordResult plugin_record_read(ConstSlice<wchar_t> path, Slice<u8> record) noexcept {
    if (!record.data || !acceptance_record_path_valid(path) ||
        !byte_ranges_disjoint({reinterpret_cast<const u8*>(path.data), path.count * static_cast<u32>(sizeof(wchar_t))}, {record.data, record.count}))
        return fail(RE::invalid_input);
    wchar_t terminated[kAcceptanceRecordMaximumPathCharacters + 1];
    acceptance_record_terminate(path, terminated);
    const auto result = acceptance_record_ancestors(terminated, path.count);
    return result.ok() ? acceptance_record_read(terminated, record) : result;
}

AcceptanceRecordResult plugin_record_write(ConstSlice<wchar_t> path, ConstSlice<u8> record) noexcept {
    if (!record.data || !record.count || !acceptance_record_path_valid(path)) return fail(RE::invalid_input);
    wchar_t terminated[kAcceptanceRecordMaximumPathCharacters + 1];
    acceptance_record_terminate(path, terminated);
    const auto ancestors = acceptance_record_ancestors(terminated, path.count);
    if (!ancestors.ok()) return ancestors;
    const auto written = file_persistence_write_atomic(terminated, path.count, record);
    switch (written.error) {
    case FilePersistenceWriteError::none: return {};
    case FilePersistenceWriteError::open: return fail(RE::open, written.native_error);
    case FilePersistenceWriteError::write: return fail(RE::write, written.native_error);
    case FilePersistenceWriteError::flush: return fail(RE::flush, written.native_error);
    default: return fail(RE::commit, written.native_error);
    }
}

bool plugin_providers_valid(const PluginProviders& providers) noexcept {
    if (providers.count > kPluginProviderMaximum || (providers.official_flags & ~kPluginOfficialKnownFlags)) return false;
    for (u32 i = 0; i < providers.count; ++i) {
        if (!entry_valid(providers.entries[i])) return false;
        for (u32 j = 0; j < i; ++j) if (providers.entries[j].id == providers.entries[i].id) return false;
    }
    for (u32 i = providers.count; i < kPluginProviderMaximum; ++i)
        if (!entry_empty(providers.entries[i])) return false;
    return true;
}

AcceptanceRecordResult plugin_providers_encode(const PluginProviders& providers, Slice<u8> record) noexcept {
    if (record.count != kPluginProvidersRecordBytes || !record.data || !plugin_providers_valid(providers) ||
        !byte_ranges_disjoint({record.data, record.count}, {reinterpret_cast<const u8*>(&providers), sizeof(providers)}))
        return fail(RE::invalid_input);
    clear_bytes(record.data, record.count);
    copy_bytes(record.data, kMagic, 8);
    byte_store_le32(record.data + 8, kPluginProvidersVersion);
    byte_store_le32(record.data + 12, record.count);
    byte_store_le32(record.data + 16, providers.count);
    byte_store_le32(record.data + 20, providers.official_flags);
    for (u32 i = 0; i < providers.count; ++i) {
        const PluginProviderRecord& e = providers.entries[i];
        u8* row = record.data + 32 + i * kPluginProvidersEntryBytes;
        byte_store_le64(row, e.id);
        byte_store_le64(row + 8, e.added);
        byte_store_le32(row + 16, e.flags);
        row[20] = static_cast<u8>(e.host_bytes); row[21] = static_cast<u8>(e.path_bytes); row[22] = static_cast<u8>(e.name_bytes);
        copy_bytes(row + 24, e.host, kPluginProviderHostCapacity);
        copy_bytes(row + 88, e.path, kPluginProviderPathCapacity);
        copy_bytes(row + 280, e.key, 65);
        copy_bytes(row + 348, e.fingerprint, 32);
        copy_bytes(row + 380, e.name, kPluginProviderNameCapacity);
    }
    byte_store_le64(record.data + 24, plugin_record_checksum({record.data, record.count}));
    return {};
}

AcceptanceRecordResult plugin_providers_decode(ConstSlice<u8> record, PluginProviders* providers) noexcept {
    if (!providers || !record.data || !byte_ranges_disjoint(record, {reinterpret_cast<const u8*>(providers), sizeof(*providers)}))
        return fail(RE::invalid_input);
    if (record.count != kPluginProvidersRecordBytes) return fail(RE::size);
    if (!bytes_equal(record.data, kMagic, 8) || byte_load_le32(record.data + 12) != record.count) return fail(RE::format);
    if (byte_load_le32(record.data + 8) != kPluginProvidersVersion) return fail(RE::version);
    if (byte_load_le64(record.data + 24) != plugin_record_checksum(record)) return fail(RE::checksum);
    PluginProviders candidate{};
    candidate.count = byte_load_le32(record.data + 16);
    candidate.official_flags = byte_load_le32(record.data + 20);
    for (u32 i = 0; i < kPluginProviderMaximum; ++i) {
        const u8* row = record.data + 32 + i * kPluginProvidersEntryBytes;
        PluginProviderRecord& e = candidate.entries[i];
        if (row[23] || !bytes_zero(row + 345, 3) || !bytes_zero(row + 444, 4)) return fail(RE::format);
        e.id = byte_load_le64(row);
        e.added = byte_load_le64(row + 8);
        e.flags = byte_load_le32(row + 16);
        e.host_bytes = row[20]; e.path_bytes = row[21]; e.name_bytes = row[22];
        copy_bytes(e.host, row + 24, kPluginProviderHostCapacity);
        copy_bytes(e.path, row + 88, kPluginProviderPathCapacity);
        copy_bytes(e.key, row + 280, 65);
        copy_bytes(e.fingerprint, row + 348, 32);
        copy_bytes(e.name, row + 380, kPluginProviderNameCapacity);
    }
    if (!plugin_providers_valid(candidate)) return fail(RE::format);
    *providers = candidate;
    return {};
}

AcceptanceRecordResult plugin_providers_load(ConstSlice<wchar_t> path, PluginProviders* providers) noexcept {
    if (!providers) return fail(RE::invalid_input);
    u8 record[kPluginProvidersRecordBytes];
    const auto result = plugin_record_read(path, {record, sizeof(record)});
    return result.ok() ? plugin_providers_decode({record, sizeof(record)}, providers) : result;
}

AcceptanceRecordResult plugin_providers_save(ConstSlice<wchar_t> path, const PluginProviders& providers) noexcept {
    u8 record[kPluginProvidersRecordBytes];
    const auto result = plugin_providers_encode(providers, {record, sizeof(record)});
    return result.ok() ? plugin_record_write(path, {record, sizeof(record)}) : result;
}

} // namespace maicord
