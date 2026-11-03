#include "maicord/app/plugins/catalog_state.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using RE = AcceptanceRecordError;
constexpr u8 kMagic[8]{'M', 'A', 'I', 'C', 'A', 'T', 'S', 'T'};
static_assert(sizeof(CatalogStateEntry) == kCatalogStateEntryBytes);

AcceptanceRecordResult fail(RE error) noexcept { return {error, 0}; }
bool zero(const void* data, u32 bytes) noexcept {
    const auto* p = static_cast<const u8*>(data);
    for (u32 i = 0; i < bytes; ++i) if (p[i]) return false;
    return true;
}
bool entry_valid(const CatalogStateEntry& e) noexcept {
    u64 code = 0;
    const bool plugin = e.kind == CatalogKind::plugin;
    if (static_cast<u32>(e.kind) >= kCatalogKindCount || (e.flags & ~kCatalogEntryKnownFlags) ||
        !catalog_slug_valid(catalog_entry_slug(e)) || !zero(e.slug + e.slug_bytes, sizeof(e.slug) - e.slug_bytes) ||
        e.version_bytes > kCatalogVersionTextMaximumBytes || e.previous_version_bytes > kCatalogVersionTextMaximumBytes ||
        !zero(e.version + e.version_bytes, sizeof(e.version) - e.version_bytes) ||
        !zero(e.previous_version + e.previous_version_bytes, sizeof(e.previous_version) - e.previous_version_bytes) ||
        zero(e.sha256, 32) || plugin != (e.owner_id != 0) ||
        (!plugin && (e.host_bits || e.previous_host_bits || (e.flags & (kCatalogEntrySideloaded | kCatalogEntryEnabled)))) ||
        (plugin && (e.flags & kCatalogEntryUnreviewed)))
        return false;
    // Unreviewed themes may carry no version; everything else is MAJOR.MINOR.PATCH.
    if ((e.version_bytes || !(e.flags & kCatalogEntryUnreviewed)) && !catalog_version_code(catalog_entry_version(e), &code))
        return false;
    if (e.previous_version_bytes ? !catalog_version_code({e.previous_version, e.previous_version_bytes}, &code) || zero(e.previous_sha256, 32)
                                 : !zero(e.previous_sha256, 32))
        return false;
    return true;
}
struct Writer {
    u8* at;
    void u8v(u8 v) noexcept { *at++ = v; }
    void u32v(u32 v) noexcept { byte_store_le32(at, v); at += 4; }
    void u64v(u64 v) noexcept { byte_store_le64(at, v); at += 8; }
    void bytes(const void* p, u32 n) noexcept { copy_bytes(at, p, n); at += n; }
};
struct Reader {
    const u8* at;
    u8 u8v() noexcept { return *at++; }
    u32 u32v() noexcept { const u32 v = byte_load_le32(at); at += 4; return v; }
    u64 u64v() noexcept { const u64 v = byte_load_le64(at); at += 8; return v; }
    void bytes(void* p, u32 n) noexcept { copy_bytes(p, at, n); at += n; }
};
void write_entry(Writer& w, const CatalogStateEntry& e) noexcept {
    w.u8v(static_cast<u8>(e.kind)); w.u8v(e.slug_bytes); w.u8v(e.version_bytes); w.u8v(e.previous_version_bytes);
    w.u8v(static_cast<u8>(e.flags)); w.u8v(static_cast<u8>(e.flags >> 8)); w.u8v(e.host_bits); w.u8v(e.previous_host_bits);
    w.u64v(e.owner_id); w.u64v(e.installed_unix); w.u64v(e.approved_permissions);
    for (u64 host : e.approved_hosts) w.u64v(host);
    w.u64v(e.skipped_code);
    w.bytes(e.sha256, 32); w.bytes(e.previous_sha256, 32);
    w.bytes(e.slug, sizeof(e.slug)); w.bytes(e.version, sizeof(e.version)); w.bytes(e.previous_version, sizeof(e.previous_version));
}
void read_entry(Reader& r, CatalogStateEntry& e) noexcept {
    e.kind = static_cast<CatalogKind>(r.u8v()); e.slug_bytes = r.u8v(); e.version_bytes = r.u8v(); e.previous_version_bytes = r.u8v();
    const u8 low = r.u8v();
    e.flags = static_cast<u16>(low | r.u8v() << 8);
    e.host_bits = r.u8v();
    e.previous_host_bits = r.u8v();
    e.owner_id = r.u64v(); e.installed_unix = r.u64v(); e.approved_permissions = r.u64v();
    for (u64& host : e.approved_hosts) host = r.u64v();
    e.skipped_code = r.u64v();
    r.bytes(e.sha256, 32); r.bytes(e.previous_sha256, 32);
    r.bytes(e.slug, sizeof(e.slug)); r.bytes(e.version, sizeof(e.version)); r.bytes(e.previous_version, sizeof(e.previous_version));
}
// FNV-1a over the record without its checksum word (bytes 20..23).
u32 checksum(const u8* record) noexcept {
    u32 hash = 2166136261u;
    for (u32 i = 8; i < kCatalogStateRecordBytes; ++i)
        if (i < 20 || i >= 24) hash = (hash ^ record[i]) * 16777619u;
    return hash;
}
void copy_text(char* target, u8* target_bytes, u32 capacity, const Utf8View& text) noexcept {
    clear_bytes(target, capacity);
    const u32 bytes = text.bytes < capacity ? text.bytes : capacity;
    copy_bytes(target, text.data, bytes);
    *target_bytes = static_cast<u8>(bytes);
}
} // namespace

bool catalog_state_valid(const CatalogState& state) noexcept {
    if (state.count > kCatalogStateMaximumEntries || (state.flags & ~kCatalogStateKnownFlags)) return false;
    for (u32 i = 0; i < state.count; ++i) {
        const CatalogStateEntry& e = state.entries[i];
        if (!entry_valid(e)) return false;
        for (u32 j = 0; j < i; ++j) {
            const CatalogStateEntry& o = state.entries[j];
            if ((o.kind == e.kind && utf8_equal(catalog_entry_slug(o), catalog_entry_slug(e))) ||
                (e.owner_id && o.owner_id == e.owner_id))
                return false;
        }
    }
    return zero(state.entries + state.count, (kCatalogStateMaximumEntries - state.count) * kCatalogStateEntryBytes);
}

AcceptanceRecordResult catalog_state_encode(const CatalogState& state, Slice<u8> record) noexcept {
    const ConstSlice<u8> ranges[]{{reinterpret_cast<const u8*>(&state), sizeof(state)}, {record.data, record.count}};
    if (!record.data || record.count != kCatalogStateRecordBytes || !byte_ranges_all_disjoint({ranges, 2}) ||
        !catalog_state_valid(state))
        return fail(RE::invalid_input);
    Writer w{record.data};
    w.bytes(kMagic, 8);
    w.u32v(kCatalogStateVersion); w.u32v(state.count); w.u32v(state.flags); w.u32v(0); // checksum below
    w.u64v(state.sequence); w.u64v(state.checked_unix);
    w.bytes(state.digest, 32);
    w.u64v(0);
    for (const CatalogStateEntry& e : state.entries) write_entry(w, e);
    byte_store_le32(record.data + 20, checksum(record.data));
    return {};
}

AcceptanceRecordResult catalog_state_decode(ConstSlice<u8> record, CatalogState* state) noexcept {
    const ConstSlice<u8> ranges[]{{reinterpret_cast<const u8*>(state), sizeof(*state)}, record};
    if (!state || !record.data || !byte_ranges_all_disjoint({ranges, 2})) return fail(RE::invalid_input);
    if (record.count != kCatalogStateRecordBytes) return fail(RE::size);
    if (!bytes_equal(record.data, kMagic, 8)) return fail(RE::format);
    Reader r{record.data + 8};
    if (r.u32v() != kCatalogStateVersion) return fail(RE::version);
    state->count = r.u32v();
    state->flags = r.u32v();
    if (r.u32v() != checksum(record.data)) return fail(RE::checksum);
    state->sequence = r.u64v();
    state->checked_unix = r.u64v();
    r.bytes(state->digest, 32);
    const bool reserved = r.u64v() != 0;
    for (CatalogStateEntry& e : state->entries) read_entry(r, e);
    if (reserved || !catalog_state_valid(*state)) {
        clear_bytes(state, sizeof(*state));
        return fail(RE::format);
    }
    return {};
}

AcceptanceRecordResult catalog_state_load(const wchar_t* path, CatalogState* state) noexcept {
    if (!state || !path) return fail(RE::invalid_input);
    clear_bytes(state, sizeof(*state));
    auto* record = static_cast<u8*>(commit_pages(kCatalogStateRecordBytes));
    if (!record) return fail(RE::invalid_input);
    auto result = acceptance_record_read(path, {record, kCatalogStateRecordBytes});
    if (result.error == RE::missing) result = {};
    else if (result.ok()) result = catalog_state_decode({record, kCatalogStateRecordBytes}, state);
    VirtualFree(record, 0, MEM_RELEASE);
    if (!result.ok()) clear_bytes(state, sizeof(*state));
    return result;
}

AcceptanceRecordResult catalog_state_save(const wchar_t* path, u32 path_characters, const CatalogState& state) noexcept {
    auto* record = static_cast<u8*>(commit_pages(kCatalogStateRecordBytes));
    if (!record) return fail(RE::invalid_input);
    auto result = catalog_state_encode(state, {record, kCatalogStateRecordBytes});
    if (result.ok()) result = acceptance_record_write(path, path_characters, {record, kCatalogStateRecordBytes});
    VirtualFree(record, 0, MEM_RELEASE);
    return result;
}

CatalogStateEntry* catalog_state_find(CatalogState& state, CatalogKind kind, const Utf8View& slug) noexcept {
    for (u32 i = 0; i < state.count; ++i)
        if (state.entries[i].kind == kind && utf8_equal(catalog_entry_slug(state.entries[i]), slug)) return &state.entries[i];
    return nullptr;
}

CatalogStateEntry* catalog_state_find_owner(CatalogState& state, u64 owner_id) noexcept {
    for (u32 i = 0; owner_id && i < state.count; ++i)
        if (state.entries[i].owner_id == owner_id) return &state.entries[i];
    return nullptr;
}

CatalogStateEntry* catalog_state_add(CatalogState& state, CatalogKind kind, const Utf8View& slug) noexcept {
    if (state.count == kCatalogStateMaximumEntries || !catalog_slug_valid(slug) || catalog_state_find(state, kind, slug)) return nullptr;
    CatalogStateEntry& e = state.entries[state.count++];
    clear_bytes(&e, sizeof(e));
    e.kind = kind;
    copy_text(e.slug, &e.slug_bytes, sizeof(e.slug), slug);
    return &e;
}

void catalog_state_remove(CatalogState& state, CatalogStateEntry* entry) noexcept {
    const u32 index = static_cast<u32>(entry - state.entries);
    if (index >= state.count) return;
    for (u32 i = index + 1; i < state.count; ++i) copy_value(&state.entries[i - 1], state.entries[i]);
    clear_bytes(&state.entries[--state.count], sizeof(CatalogStateEntry));
}

void catalog_state_install(CatalogStateEntry* e, const Utf8View& version, const u8* sha256, u64 now, bool keep_previous) noexcept {
    if (keep_previous && !zero(e->sha256, 32) && !bytes_equal(e->sha256, sha256, 32) && e->version_bytes) {
        copy_bytes(e->previous_sha256, e->sha256, 32);
        copy_text(e->previous_version, &e->previous_version_bytes, sizeof(e->previous_version), catalog_entry_version(*e));
        e->previous_host_bits = e->host_bits;
    } else if (!keep_previous) {
        clear_bytes(e->previous_sha256, 32);
        copy_text(e->previous_version, &e->previous_version_bytes, sizeof(e->previous_version), {});
        e->previous_host_bits = 0;
    }
    e->host_bits = 0;
    copy_bytes(e->sha256, sha256, 32);
    copy_text(e->version, &e->version_bytes, sizeof(e->version), version);
    e->installed_unix = now;
}

void catalog_state_swap_previous(CatalogStateEntry* e) noexcept {
    u8 sha[32];
    char version[sizeof(e->version)];
    copy_bytes(sha, e->sha256, 32);
    copy_bytes(version, e->version, sizeof(version));
    const u8 bytes = e->version_bytes;
    copy_bytes(e->sha256, e->previous_sha256, 32);
    copy_bytes(e->version, e->previous_version, sizeof(version));
    e->version_bytes = e->previous_version_bytes;
    copy_bytes(e->previous_sha256, sha, 32);
    copy_bytes(e->previous_version, version, sizeof(version));
    e->previous_version_bytes = bytes;
    const u8 hosts = e->host_bits;
    e->host_bits = e->previous_host_bits;
    e->previous_host_bits = hosts;
}

u64 catalog_state_host_hash(const Utf8View& host) noexcept {
    const u64 hash = hash64_counted_bytes(kHash64Seed, host.data, host.bytes);
    return hash ? hash : 1;
}

u32 catalog_state_approved_hosts(const CatalogStateEntry& e, ConstSlice<Utf8View> hosts) noexcept {
    u32 bits = 0;
    for (u32 i = 0; i < hosts.count && i < 32; ++i) {
        const u64 hash = catalog_state_host_hash(hosts[i]);
        for (u64 approved : e.approved_hosts) if (approved == hash) bits |= 1u << i;
    }
    return bits;
}

void catalog_state_approve(CatalogStateEntry* e, u64 permissions, ConstSlice<Utf8View> hosts, u32 bits) noexcept {
    e->approved_permissions |= permissions;
    u64 current[kCatalogStateHostSlots]{};
    u32 count = 0;
    for (u32 i = 0; i < hosts.count && i < 32 && count < kCatalogStateHostSlots; ++i)
        if (bits & (1u << i)) current[count++] = catalog_state_host_hash(hosts[i]);
    // Older approvals fill the slots left over.
    for (u64 old : e->approved_hosts) {
        bool listed = !old;
        for (u32 i = 0; i < count && !listed; ++i) listed = current[i] == old;
        if (!listed && count < kCatalogStateHostSlots) current[count++] = old;
    }
    copy_bytes(e->approved_hosts, current, sizeof(current));
}

} // namespace maicord
