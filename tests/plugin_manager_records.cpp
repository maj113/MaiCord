// Plugin catalog records: index floors (plugin_index_acceptance) on the
// shared acceptance-record machinery, and the providers.bin, state.bin and
// journal.bin codecs with atomic file IO. Files live in an isolated TEMP
// directory; index documents are signed with the test-only scalar-one key.
#include "maicord/app/plugin_index_acceptance.hpp"
#include "maicord/app/plugin_manager_state.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "plugin_test_key.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
using RE = AcceptanceRecordError;
using plugin_test_key::kGenerator;
using plugin_test_key::kGeneratorHex;
constexpr u64 kNow = 1760400000;

struct Fixture {
    KdfSha256Workspace workspace;
    P256Key signer, trusted;
    wchar_t root[MAX_PATH], acceptance[MAX_PATH], lock[MAX_PATH], sibling[MAX_PATH], providers[MAX_PATH], state[MAX_PATH],
        journal[MAX_PATH];
    u32 acceptance_chars, providers_chars, state_chars, journal_chars;
    char document[1024];
    u32 document_bytes;
    u8 signature[64];
    u32 scratch[4096];
    PluginIndex index;
};
Fixture g;

u32 child(const wchar_t* name, wchar_t* out) noexcept {
    u32 n = 0;
    while (g.root[n]) { out[n] = g.root[n]; ++n; }
    out[n++] = L'\\';
    for (u32 i = 0;; ++i) { out[n + i] = name[i]; if (!name[i]) return n + i; }
}
bool write_file(const wchar_t* path, const u8* data, u32 bytes) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, bytes, &written, nullptr) && written == bytes;
    return CloseHandle(file) && ok;
}
bool exists(const wchar_t* path) noexcept { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

// A minimal index (no plugins) of the given provider kind, sequence and issue time.
Utf8View document(bool official, u64 sequence, u64 issued = 1760226600, bool key = true) noexcept {
    u32 n = 0;
    const auto add = [&](const char* s) noexcept { while (*s) g.document[n++] = *s++; };
    const auto number = [&](u64 v) noexcept { n += decimal_write(g.document + n, v); };
    add(official ? R"({"format":1,"provider":{"id":"maicord","name":"MaiCord plugins","homepage":"https://plugins.example.com/maicord"})"
                 : R"({"format":1,"provider":{"id":"example-plugins","name":"Example plugins","homepage":"https://plugins.example.net/")");
    if (!official && key) { add(R"(,"key":")"); add(kGeneratorHex); add("\""); }
    if (!official) add("}");
    add(R"(,"sequence":)"); number(sequence);
    add(R"(,"issued":)"); number(issued);
    add(R"(,"expires":)"); number(issued + 30 * 86400);
    add(R"(,"plugins":[],"revoked":[]})");
    g.document_bytes = n;
    return {g.document, n};
}
bool sign(Utf8View text) noexcept {
    const ConstSlice<u8> parts[]{{reinterpret_cast<const u8*>(kPluginIndexSignatureDomain.data), kPluginIndexSignatureDomain.bytes},
        {reinterpret_cast<const u8*>(text.data), text.bytes}};
    u8 digest[32]{};
    return sha256(g.workspace, {parts, 2}, {digest, 32}).ok() && p256_sign_digest(g.signer, {digest, 32}, {g.signature, 64}).ok();
}
PluginIndexPolicy policy(PluginProviderKind kind) noexcept {
    PluginIndexPolicy p{};
    p.now_unix = kNow; p.kind = kind;
    if (kind == PluginProviderKind::custom_signed) copy_bytes(p.pinned_key, kGenerator, 65);
    return p;
}
PluginIndexAcceptanceResult admit(PluginIndexAcceptanceTransaction* t, u64 provider, Utf8View text, PluginProviderKind kind) noexcept {
    const bool unsigned_kind = kind == PluginProviderKind::custom_unsigned;
    if (!unsigned_kind && !sign(text)) return {RE::invalid_input, 0, PluginIndexError::none, false};
    return plugin_index_acceptance_verify_commit(t, provider, text, {g.signature, 64}, unsigned_kind ? nullptr : &g.trusted,
        g.workspace, policy(kind), {g.scratch, 4096}, &g.index);
}
bool states_equal(const PluginIndexAcceptanceState& a, const PluginIndexAcceptanceState& b) noexcept {
    for (u32 i = 0; i < kPluginIndexAcceptanceSlots; ++i)
        if (a.slots[i].present != b.slots[i].present || a.slots[i].provider != b.slots[i].provider ||
            a.slots[i].sequence != b.slots[i].sequence || !bytes_equal(a.slots[i].index_sha256, b.slots[i].index_sha256, 32))
            return false;
    return true;
}
bool index_zeroed() noexcept {
    const auto* bytes = reinterpret_cast<const u8*>(&g.index);
    for (u32 i = 0; i < sizeof(g.index); ++i) if (bytes[i]) return false;
    return true;
}
void reseal(u8* record, u32 bytes) noexcept { byte_store_le64(record + 24, plugin_record_checksum({record, bytes})); }

// ---- floors ----------------------------------------------------------------------
void test_floor_codec(Check check) noexcept {
    PluginIndexAcceptanceState state{};
    state.slots[0] = {kPluginProviderOfficial, 5, {}, true};
    state.slots[3] = {0x1234, ~u64{0}, {}, true};
    for (u32 i = 0; i < 32; ++i) { state.slots[0].index_sha256[i] = static_cast<u8>(i); state.slots[3].index_sha256[i] = 0xC5; }
    u8 record[kPluginIndexAcceptanceRecordBytes + 1]{};
    PluginIndexAcceptanceState decoded{};
    check(plugin_index_acceptance_encode(state, {record, kPluginIndexAcceptanceRecordBytes}).ok() &&
        bytes_equal(record, "MAIPIACC", 8) && byte_load_le32(record + 16) == kPluginIndexAcceptanceSlots &&
        plugin_index_acceptance_decode({record, kPluginIndexAcceptanceRecordBytes}, &decoded).ok() && states_equal(state, decoded),
        "plugin floors: the official and custom slots survive the 536-byte record");
    const auto sentinel = state;
    const auto rejected = [&](RE expected, u32 count = kPluginIndexAcceptanceRecordBytes) noexcept {
        decoded = sentinel;
        return plugin_index_acceptance_decode({record, count}, &decoded).error == expected && states_equal(decoded, sentinel);
    };
    const auto reset = [&]() noexcept { return plugin_index_acceptance_encode(state, {record, kPluginIndexAcceptanceRecordBytes}).ok(); };
    reset(); record[32 + 16] ^= 1;
    check(rejected(RE::checksum), "plugin floors: a changed digest fails the checksum and leaves the output untouched");
    reset(); record[8] = 2; reseal(record, kPluginIndexAcceptanceRecordBytes);
    check(rejected(RE::version), "plugin floors: an unknown record version is refused");
    reset(); record[0] = 'X'; reseal(record, kPluginIndexAcceptanceRecordBytes);
    check(rejected(RE::format), "plugin floors: a wrong magic is refused");
    reset(); record[32 + 48] = 2; reseal(record, kPluginIndexAcceptanceRecordBytes);
    check(rejected(RE::format), "plugin floors: a present byte other than 0 or 1 is refused");
    reset(); record[32 + 3 * kPluginIndexAcceptanceSlotBytes + 55] = 1; reseal(record, kPluginIndexAcceptanceRecordBytes);
    check(rejected(RE::format), "plugin floors: reserved slot bytes are zero");
    reset(); record[32] = 7; reseal(record, kPluginIndexAcceptanceRecordBytes);
    check(rejected(RE::format), "plugin floors: slot 0 belongs to the official provider");
    reset();
    check(rejected(RE::size, kPluginIndexAcceptanceRecordBytes - 1) && rejected(RE::size, kPluginIndexAcceptanceRecordBytes + 1),
        "plugin floors: truncated and trailing records are refused");
    auto duplicate = state;
    duplicate.slots[5] = state.slots[3];
    auto absent_floor = state;
    absent_floor.slots[1].sequence = 3;
    u8 snapshot[kPluginIndexAcceptanceRecordBytes];
    reset(); copy_bytes(snapshot, record, sizeof(snapshot));
    check(!plugin_index_acceptance_valid(duplicate) && !plugin_index_acceptance_valid(absent_floor) &&
        plugin_index_acceptance_encode(duplicate, {record, kPluginIndexAcceptanceRecordBytes}).error == RE::invalid_input &&
        bytes_equal(record, snapshot, sizeof(snapshot)),
        "plugin floors: duplicate providers or an absent slot with a floor never encode");
    using V = PluginIndexFloorVerdict;
    u8 digest[32]{}, other[32]{};
    for (u32 i = 0; i < 32; ++i) digest[i] = static_cast<u8>(i);
    other[0] = 1;
    check(plugin_index_floor_check(state, kPluginProviderOfficial, 4, digest) == V::replay &&
        plugin_index_floor_check(state, kPluginProviderOfficial, 5, other) == V::replay &&
        plugin_index_floor_check(state, kPluginProviderOfficial, 5, digest) == V::same &&
        plugin_index_floor_check(state, kPluginProviderOfficial, 6, other) == V::advance &&
        plugin_index_floor_check(state, 0x9999, 1, other) == V::advance &&
        plugin_index_floor_check(state, 0x9999, 0, other) == V::replay &&
        plugin_index_floor(state, 0x1234) == &state.slots[3] && !plugin_index_floor(state, 0x9999),
        "plugin floors: lower is refused, the same sequence needs the same digest, higher advances");
}

void test_floor_transaction(Check check) noexcept {
    const ConstSlice<wchar_t> path{g.acceptance, g.acceptance_chars};
    PluginIndexAcceptanceTransaction owner{}, peer{};
    struct Cleanup {
        PluginIndexAcceptanceTransaction& a; PluginIndexAcceptanceTransaction& b;
        ~Cleanup() noexcept { plugin_index_acceptance_release(&a); plugin_index_acceptance_release(&b); }
    } cleanup{owner, peer};
    PluginIndexAcceptanceState loaded{};
    check(plugin_index_acceptance_load(path, &loaded).error == RE::missing &&
        plugin_index_acceptance_begin(path, &owner).error == RE::missing && !owner.lock,
        "plugin floor file: a missing record is explicit unless first use is provisioned");
    const auto begun = plugin_index_acceptance_begin(path, &owner, true);
    check(begun.ok() && owner.lock && !owner.record_present, "plugin floor file: first use begins under an owned lock");
    if (!begun.ok()) return;
    const auto blocked = plugin_index_acceptance_begin(path, &peer, true);
    check(blocked.error == RE::lock && blocked.native_error == ERROR_SHARING_VIOLATION && !peer.lock,
        "plugin floor file: a second lock holder gets lock");

    auto r = admit(&owner, kPluginProviderOfficial, document(true, 100), PluginProviderKind::official);
    u8 admitted[32]{};
    copy_bytes(admitted, g.index.digest, 32);
    check(r.ok() && r.written && owner.record_present && owner.state.slots[0].present && owner.state.slots[0].sequence == 100 &&
        bytes_equal(owner.state.slots[0].index_sha256, admitted, 32) && plugin_index_acceptance_load(path, &loaded).ok() &&
        states_equal(loaded, owner.state) && g.index.sequence == 100,
        "plugin floor file: the first official index sets its floor to the sequence and digest");
    // A stale sibling makes any write attempt fail, so success proves zero writes.
    const u8 stale = 1;
    check(write_file(g.sibling, &stale, 1), "plugin floor file: stale sibling fixture");
    r = admit(&owner, kPluginProviderOfficial, document(true, 100), PluginProviderKind::official);
    check(r.ok() && !r.written && g.index.sequence == 100, "plugin floor file: the same index again is up to date with 0 writes");
    check(DeleteFileW(g.sibling) != 0, "plugin floor file: stale sibling removed");
    const auto before = owner.state;
    r = admit(&owner, kPluginProviderOfficial, document(true, 99), PluginProviderKind::official);
    check(r.error == RE::replay && !r.written && index_zeroed() && states_equal(owner.state, before) &&
        plugin_index_acceptance_load(path, &loaded).ok() && states_equal(loaded, before),
        "plugin floor file: a lower sequence is refused as a replay and nothing changes");
    r = admit(&owner, kPluginProviderOfficial, document(true, 100, 1760226601), PluginProviderKind::official);
    check(r.error == RE::replay && states_equal(owner.state, before), "plugin floor file: the floor's sequence with another digest is refused");
    const Utf8View text = document(true, 101);
    check(sign(text), "plugin floor file: signed");
    g.signature[5] ^= 1;
    r = plugin_index_acceptance_verify_commit(&owner, kPluginProviderOfficial, text, {g.signature, 64}, &g.trusted, g.workspace,
        policy(PluginProviderKind::official), {g.scratch, 4096}, &g.index);
    check(r.error == RE::document && r.index_error == PluginIndexError::signature && states_equal(owner.state, before),
        "plugin floor file: a bad signature is refused under the lock without a write");
    r = admit(&owner, kPluginProviderOfficial, document(true, 101), PluginProviderKind::official);
    check(r.ok() && r.written && owner.state.slots[0].sequence == 101, "plugin floor file: a higher sequence advances the floor");

    // Custom providers take slots 1-8 by id.
    bool filled = true;
    for (u64 i = 0; i < kPluginProviderMaximum; ++i) {
        r = admit(&owner, 0x5000 + i, document(false, 7 + i), PluginProviderKind::custom_signed);
        filled &= r.ok() && r.written && owner.state.slots[1 + i].provider == 0x5000 + i && owner.state.slots[1 + i].sequence == 7 + i;
    }
    check(filled, "plugin floor file: eight custom providers get their own floors");
    r = admit(&owner, 0x6000, document(false, 1), PluginProviderKind::custom_signed);
    check(r.error == RE::capacity && index_zeroed(), "plugin floor file: a ninth custom provider has no slot");
    r = plugin_index_acceptance_forget(&owner, 0x5003);
    const auto forgot_none = plugin_index_acceptance_forget(&owner, 0x7777);
    check(r.ok() && r.written && !owner.state.slots[4].present && forgot_none.ok() && !forgot_none.written,
        "plugin floor file: removing a provider frees its floor");
    r = admit(&owner, 0x6000, document(false, 1, 1760226600, false), PluginProviderKind::custom_unsigned);
    check(r.ok() && r.written && owner.state.slots[4].provider == 0x6000 && !g.index.has_key,
        "plugin floor file: an unsigned Developer Mode provider reuses the free slot");
    check(admit(&owner, kPluginProviderOfficial, document(false, 200), PluginProviderKind::custom_signed).error == RE::invalid_input &&
        admit(&owner, 0x5000, document(true, 200), PluginProviderKind::official).error == RE::invalid_input &&
        admit(&owner, kPluginProviderFile, document(false, 200), PluginProviderKind::custom_signed).error == RE::invalid_input,
        "plugin floor file: the official slot takes only the official kind and provider 0 is never a catalog");

    // Restart: a fresh owner reloads the same floors.
    const auto latest = owner.state;
    check(plugin_index_acceptance_release(&owner).ok() && plugin_index_acceptance_begin(path, &peer).ok() &&
        peer.record_present && states_equal(peer.state, latest) && plugin_index_acceptance_release(&peer).ok(),
        "plugin floor file: a restart reloads every floor");
    u8 record[kPluginIndexAcceptanceRecordBytes]{};
    check(plugin_index_acceptance_encode(latest, {record, sizeof(record)}).ok(), "plugin floor file: snapshot encodes");
    record[40] ^= 1;
    check(write_file(g.acceptance, record, sizeof(record)) && plugin_index_acceptance_begin(path, &peer, true).error == RE::checksum &&
        !peer.lock, "plugin floor file: a corrupt record fails closed even with first use allowed");
    check(write_file(g.acceptance, record, 100) && plugin_index_acceptance_begin(path, &peer, true).error == RE::size && !peer.lock,
        "plugin floor file: a truncated record fails closed");
    DeleteFileW(g.acceptance); DeleteFileW(g.lock);
}

// ---- providers.bin -----------------------------------------------------------------
PluginProviderRecord provider_entry(u64 id, const char* host, const char* path, const char* name, bool signed_provider) noexcept {
    PluginProviderRecord e{};
    e.id = id; e.added = 1760000000 + id;
    e.flags = kPluginProviderEnabled | (signed_provider ? kPluginProviderSigned | kPluginProviderCheckDaily : 0);
    for (; host[e.host_bytes]; ++e.host_bytes) e.host[e.host_bytes] = host[e.host_bytes];
    for (; path[e.path_bytes]; ++e.path_bytes) e.path[e.path_bytes] = path[e.path_bytes];
    for (; name[e.name_bytes]; ++e.name_bytes) e.name[e.name_bytes] = name[e.name_bytes];
    if (signed_provider) {
        copy_bytes(e.key, kGenerator, 65);
        for (u32 i = 0; i < 32; ++i) e.fingerprint[i] = static_cast<u8>(0xA0 + i);
    }
    return e;
}
bool providers_equal(const PluginProviders& a, const PluginProviders& b) noexcept {
    if (a.count != b.count || a.official_flags != b.official_flags) return false;
    for (u32 i = 0; i < kPluginProviderMaximum; ++i) {
        const auto& x = a.entries[i];
        const auto& y = b.entries[i];
        if (x.id != y.id || x.added != y.added || x.flags != y.flags || x.host_bytes != y.host_bytes || x.path_bytes != y.path_bytes ||
            x.name_bytes != y.name_bytes || !bytes_equal(x.host, y.host, sizeof(x.host)) || !bytes_equal(x.path, y.path, sizeof(x.path)) ||
            !bytes_equal(x.key, y.key, 65) || !bytes_equal(x.fingerprint, y.fingerprint, 32) || !bytes_equal(x.name, y.name, sizeof(x.name)))
            return false;
    }
    return true;
}
PluginProviders g_providers, g_providers_decoded, g_providers_sentinel;

void test_providers(Check check) noexcept {
    PluginProviders& p = g_providers;
    p = {};
    p.count = 2; p.official_flags = kPluginOfficialAutoUpdateOff;
    p.entries[0] = provider_entry(0x1111222233334444, "someone.github.io", "/maicord-plugins/", "Example plugins", true);
    p.entries[1] = provider_entry(0xFFFFFFFFFFFFFFFF, "plugins.example.net", "/", "Caf\xC3\xA9 plugins", false);
    static u8 record[kPluginProvidersRecordBytes + 1];
    check(plugin_providers_valid(p) && plugin_providers_encode(p, {record, kPluginProvidersRecordBytes}).ok() &&
        bytes_equal(record, "MAIPPROV", 8) && byte_load_le32(record + 12) == 3616 &&
        plugin_providers_decode({record, kPluginProvidersRecordBytes}, &g_providers_decoded).ok() && providers_equal(p, g_providers_decoded),
        "plugin providers: signed and unsigned entries and the official switches survive the 3616-byte record");
    g_providers_sentinel = p;
    g_providers_sentinel.count = 1; g_providers_sentinel.entries[1] = {};
    const auto rejected = [&](RE expected, u32 count = kPluginProvidersRecordBytes) noexcept {
        g_providers_decoded = g_providers_sentinel;
        return plugin_providers_decode({record, count}, &g_providers_decoded).error == expected &&
            providers_equal(g_providers_decoded, g_providers_sentinel);
    };
    const auto reset = [&]() noexcept { return plugin_providers_encode(p, {record, kPluginProvidersRecordBytes}).ok(); };
    reset(); record[32 + 100] ^= 1;
    check(rejected(RE::checksum), "plugin providers: a changed byte fails the checksum and leaves the output untouched");
    reset(); record[8] = 9; reseal(record, kPluginProvidersRecordBytes);
    check(rejected(RE::version), "plugin providers: an unknown version is refused");
    reset(); record[32 + 23] = 1; reseal(record, kPluginProvidersRecordBytes);
    check(rejected(RE::format), "plugin providers: reserved entry bytes are zero");
    reset(); record[32 + 24] = 'S'; reseal(record, kPluginProvidersRecordBytes);
    check(rejected(RE::format), "plugin providers: an uppercase host is not canonical");
    reset();
    check(rejected(RE::size, kPluginProvidersRecordBytes - 1), "plugin providers: a truncated record is refused");
    struct Invalid { const char* name; void (*mutate)(PluginProviders&) noexcept; };
    constexpr Invalid invalid[]{
        {"duplicate id", [](PluginProviders& v) noexcept { v.entries[1].id = v.entries[0].id; }},
        {"official id", [](PluginProviders& v) noexcept { v.entries[1].id = kPluginProviderOfficial; }},
        {"unknown flag", [](PluginProviders& v) noexcept { v.entries[0].flags |= 1u << 7; }},
        {"official switch", [](PluginProviders& v) noexcept { v.official_flags = 1u << 5; }},
        {"empty segment", [](PluginProviders& v) noexcept { v.entries[0] = provider_entry(9, "a.example", "/a//b/", "A", true); }},
        {"dot segment", [](PluginProviders& v) noexcept { v.entries[0] = provider_entry(9, "a.example", "/a/../", "A", true); }},
        {"no trailing slash", [](PluginProviders& v) noexcept { v.entries[0] = provider_entry(9, "a.example", "/a", "A", true); }},
        {"query", [](PluginProviders& v) noexcept { v.entries[0] = provider_entry(9, "a.example", "/a?b/", "A", true); }},
        {"ip host", [](PluginProviders& v) noexcept { v.entries[0] = provider_entry(9, "10.0.0.1", "/", "A", true); }},
        {"bidi name", [](PluginProviders& v) noexcept { v.entries[0] = provider_entry(9, "a.example", "/", "A\xE2\x80\xAE" "B", true); }},
        {"empty name", [](PluginProviders& v) noexcept { v.entries[0] = provider_entry(9, "a.example", "/", "", true); }},
        {"signed without key", [](PluginProviders& v) noexcept { v.entries[0].key[0] = 0; }},
        {"unsigned with fingerprint", [](PluginProviders& v) noexcept { v.entries[1].fingerprint[3] = 1; }},
        {"text past its length", [](PluginProviders& v) noexcept { v.entries[1].host[40] = 'x'; }},
        {"unused entry", [](PluginProviders& v) noexcept { v.entries[5].added = 1; }},
    };
    u32 refused = 0;
    for (const auto& row : invalid) {
        PluginProviders& mutated = g_providers_decoded;
        mutated = p;
        row.mutate(mutated);
        u8 before = record[0];
        refused += !plugin_providers_valid(mutated) && plugin_providers_encode(mutated, {record, kPluginProvidersRecordBytes}).error == RE::invalid_input &&
            record[0] == before;
    }
    check(refused == sizeof(invalid) / sizeof(invalid[0]), "plugin providers: every invalid entry is refused before encoding");

    const ConstSlice<wchar_t> path{g.providers, g.providers_chars};
    g_providers_decoded = g_providers_sentinel;
    check(plugin_providers_load(path, &g_providers_decoded).error == RE::missing && providers_equal(g_providers_decoded, g_providers_sentinel),
        "plugin providers: a missing file is reported and leaves the output untouched");
    check(plugin_providers_save(path, p).ok() && plugin_providers_load(path, &g_providers_decoded).ok() &&
        providers_equal(g_providers_decoded, p), "plugin providers: save and load round trip through the atomic writer");
    // A reader holding the file without delete sharing blocks the replacement; the old file survives.
    const HANDLE holder = CreateFileW(g.providers, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    const auto blocked = holder != INVALID_HANDLE_VALUE ? plugin_providers_save(path, g_providers_sentinel) : AcceptanceRecordResult{};
    if (holder != INVALID_HANDLE_VALUE) CloseHandle(holder);
    wchar_t temporary[MAX_PATH]{};
    child(L"providers.bin.new", temporary);
    check(blocked.error == RE::commit && plugin_providers_load(path, &g_providers_decoded).ok() && providers_equal(g_providers_decoded, p) &&
        !exists(temporary), "plugin providers: a failed replacement keeps the previous file and no sibling");
    check(plugin_providers_save({L"providers.bin", 13}, p).error == RE::invalid_input &&
        plugin_providers_load({L"C:\\NUL\\providers.bin", 20}, &g_providers_decoded).error == RE::invalid_input,
        "plugin providers: relative and device paths are refused");
    DeleteFileW(g.providers);
}

// ---- state.bin and journal.bin -----------------------------------------------------
PluginManagerState g_state, g_state_decoded;
bool state_equal(const PluginManagerState& a, const PluginManagerState& b) noexcept {
    return a.count == b.count && bytes_equal(a.entries, b.entries, sizeof(a.entries));
}
void test_state(Check check) noexcept {
    PluginManagerState& s = g_state;
    s = {};
    s.count = 3;
    s.entries[0] = {0x4D43534844574341, kPluginProviderOfficial, kPluginStateHeld, 0, 1001000999, {}, 1000000999, {}, 1001000999, 1760227200, {}};
    copy_bytes(s.entries[0].slug, "showcase", 8);
    s.entries[0].installed_sha256[0] = 1; s.entries[0].previous_sha256[0] = 2;
    s.entries[1] = {0xFEDCBA9876543210, 0x1111222233334444, kPluginStateRevokedMalicious | kPluginStateIncompatibleHost, 7,
        3000999, {}, 0, {}, 0, 42, {}};
    copy_bytes(s.entries[1].slug, "dev-tools", 9);
    s.entries[2] = {0x77, kPluginProviderFile, kPluginStateAutoUpdateOff, 0, 0, {}, 0, {}, 0, 0, {}};
    static u8 record[kPluginManagerStateRecordBytes];
    check(plugin_manager_state_valid(s) && plugin_manager_state_encode(s, {record, sizeof(record)}).ok() &&
        bytes_equal(record, "MAIPSTAT", 8) && byte_load_le32(record + 12) == 5152 &&
        plugin_manager_state_decode({record, sizeof(record)}, &g_state_decoded).ok() && state_equal(s, g_state_decoded),
        "plugin state: catalog, custom and file-installed entries survive the 5152-byte record");
    PluginManagerState sentinel{};
    sentinel.count = 1; sentinel.entries[0] = s.entries[2];
    const auto rejected = [&](RE expected) noexcept {
        g_state_decoded = sentinel;
        return plugin_manager_state_decode({record, sizeof(record)}, &g_state_decoded).error == expected && state_equal(g_state_decoded, sentinel);
    };
    const auto reset = [&]() noexcept { return plugin_manager_state_encode(s, {record, sizeof(record)}).ok(); };
    reset(); record[32 + 40] ^= 1;
    check(rejected(RE::checksum), "plugin state: a changed byte fails the checksum and leaves the output untouched");
    reset(); record[8] = 2; reseal(record, sizeof(record));
    check(rejected(RE::version), "plugin state: an unknown version is refused");
    reset(); record[20] = 1; reseal(record, sizeof(record));
    check(rejected(RE::format), "plugin state: the reserved header word is zero");
    reset(); record[32 + 16] = 0x80; reseal(record, sizeof(record));
    check(rejected(RE::format), "plugin state: unknown entry flags are refused");
    struct Invalid { void (*mutate)(PluginManagerState&) noexcept; };
    constexpr Invalid invalid[]{
        {[](PluginManagerState& v) noexcept { v.entries[1].owner = v.entries[0].owner; }},
        {[](PluginManagerState& v) noexcept { v.entries[1].owner = 1; }},
        {[](PluginManagerState& v) noexcept { v.entries[1].max_host_abi = 0x10000; }},
        {[](PluginManagerState& v) noexcept { v.entries[1].previous_sha256[0] = 1; }},
        {[](PluginManagerState& v) noexcept { v.entries[1].slug[0] = '-'; }},
        {[](PluginManagerState& v) noexcept { v.entries[0].slug[20] = 'x'; }},
        {[](PluginManagerState& v) noexcept { v.entries[0].slug[2] = 'O'; }},
        {[](PluginManagerState& v) noexcept { v.entries[9].held_code = 1; }},
        {[](PluginManagerState& v) noexcept { v.count = kPluginManagerStateMaximumEntries + 1; }},
    };
    u32 refused = 0;
    for (const auto& row : invalid) {
        g_state_decoded = s;
        row.mutate(g_state_decoded);
        refused += !plugin_manager_state_valid(g_state_decoded) &&
            plugin_manager_state_encode(g_state_decoded, {record, sizeof(record)}).error == RE::invalid_input;
    }
    check(refused == sizeof(invalid) / sizeof(invalid[0]), "plugin state: every invalid entry is refused before encoding");
    const ConstSlice<wchar_t> path{g.state, g.state_chars};
    check(plugin_manager_state_load(path, &g_state_decoded).error == RE::missing && plugin_manager_state_save(path, s).ok() &&
        plugin_manager_state_load(path, &g_state_decoded).ok() && state_equal(g_state_decoded, s),
        "plugin state: save and load round trip through the atomic writer");
    DeleteFileW(g.state);
}

bool journal_equal(const PluginJournal& a, const PluginJournal& b) noexcept {
    return a.operation == b.operation && a.phase == b.phase && a.enable_after == b.enable_after && a.had_trust == b.had_trust &&
        a.owner == b.owner && a.provider == b.provider && a.to_code == b.to_code && a.approved_permissions == b.approved_permissions &&
        bytes_equal(a.from_sha256, b.from_sha256, 32) && bytes_equal(a.to_sha256, b.to_sha256, 32);
}
void test_journal(Check check) noexcept {
    using Op = PluginJournalOperation;
    using Ph = PluginJournalPhase;
    u8 record[kPluginJournalRecordBytes]{};
    PluginJournal cleared{}, decoded{};
    check(plugin_journal_encode(cleared, {record, sizeof(record)}).ok() && byte_load_le32(record + 16) == 0 &&
        plugin_journal_decode({record, sizeof(record)}, &decoded).ok() && journal_equal(decoded, cleared),
        "plugin journal: the cleared journal is a count-0 record");
    u32 round_trips = 0;
    for (u32 op = 1; op <= 4; ++op) {
        for (u32 phase = 1; phase <= 3; ++phase) {
            PluginJournal j{};
            j.operation = static_cast<Op>(op); j.phase = static_cast<Ph>(phase);
            j.enable_after = phase == 2; j.had_trust = op == 2;
            j.owner = 0x1122334455667788; j.provider = op == 4 ? kPluginProviderFile : kPluginProviderOfficial;
            j.to_code = 2000000999; j.approved_permissions = kNativePluginPermissionsAll;
            j.from_sha256[31] = static_cast<u8>(op); j.to_sha256[0] = static_cast<u8>(phase);
            decoded = {};
            round_trips += plugin_journal_encode(j, {record, sizeof(record)}).ok() && byte_load_le32(record + 16) == 1 &&
                plugin_journal_decode({record, sizeof(record)}, &decoded).ok() && journal_equal(decoded, j);
        }
    }
    check(round_trips == 12, "plugin journal: every operation and phase survives the 160-byte record");
    PluginJournal valid{};
    valid.operation = Op::update; valid.phase = Ph::staged; valid.owner = 0x99; valid.provider = kPluginProviderOfficial;
    const auto reset = [&]() noexcept { return plugin_journal_encode(valid, {record, sizeof(record)}).ok(); };
    const auto rejected = [&](RE expected) noexcept {
        decoded = cleared;
        return plugin_journal_decode({record, sizeof(record)}, &decoded).error == expected && journal_equal(decoded, cleared);
    };
    reset(); record[32 + 10] ^= 1;
    check(rejected(RE::checksum), "plugin journal: a changed byte fails the checksum and leaves the output untouched");
    reset(); record[8] = 3; reseal(record, sizeof(record));
    check(rejected(RE::version), "plugin journal: an unknown version is refused");
    reset(); record[16] = 0; reseal(record, sizeof(record));
    check(rejected(RE::format), "plugin journal: the count must match the operation");
    reset(); record[32] = 9; reseal(record, sizeof(record));
    check(rejected(RE::format), "plugin journal: an unknown operation is refused");
    reset(); record[32 + 120] = 1; reseal(record, sizeof(record));
    check(rejected(RE::format), "plugin journal: reserved bytes are zero");
    PluginJournal bad = valid; bad.phase = Ph::none;
    PluginJournal bad_owner = valid; bad_owner.owner = 1;
    PluginJournal bad_mask = valid; bad_mask.approved_permissions = 1ull << 40;
    PluginJournal stray{}; stray.to_code = 5;
    check(!plugin_journal_valid(bad) && !plugin_journal_valid(bad_owner) && !plugin_journal_valid(bad_mask) && !plugin_journal_valid(stray) &&
        plugin_journal_encode(stray, {record, sizeof(record)}).error == RE::invalid_input,
        "plugin journal: missing phase, reserved owners, unknown permissions and stray cleared fields are refused");
    const ConstSlice<wchar_t> path{g.journal, g.journal_chars};
    check(plugin_journal_load(path, &decoded).error == RE::missing && plugin_journal_save(path, valid).ok() &&
        plugin_journal_load(path, &decoded).ok() && journal_equal(decoded, valid) && plugin_journal_save(path, cleared).ok() &&
        plugin_journal_load(path, &decoded).ok() && decoded.operation == Op::none,
        "plugin journal: an operation and its clearing round trip through the atomic writer");
    DeleteFileW(g.journal);
}
} // namespace

void test_plugin_manager_records(Check check) noexcept {
    wchar_t temp[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, temp);
    const bool directory = n && n < MAX_PATH - 64 && GetTempFileNameW(temp, L"MPC", 0, g.root) && DeleteFileW(g.root) &&
        CreateDirectoryW(g.root, nullptr);
    check(directory, "plugin records: isolated temporary directory");
    if (!directory) return;
    g.acceptance_chars = child(L"acceptance.bin", g.acceptance);
    child(L"acceptance.bin.lock", g.lock);
    child(L"acceptance.bin.new", g.sibling);
    g.providers_chars = child(L"providers.bin", g.providers);
    g.state_chars = child(L"state.bin", g.state);
    g.journal_chars = child(L"journal.bin", g.journal);
    u8 scalar[32]{};
    scalar[31] = 1; // test-only private key; its public point is the generator
    const bool crypto = kdf_sha256_initialize(&g.workspace).ok() && p256_import_private(&g.signer, P256Kind::ecdsa, {scalar, 32}).ok() &&
        p256_import_public(&g.trusted, P256Kind::ecdsa, {kGenerator, 65}).ok();
    check(crypto, "plugin records: test-only signer");
    if (crypto) {
        test_floor_codec(check);
        test_floor_transaction(check);
    }
    test_providers(check);
    test_state(check);
    test_journal(check);
    p256_release(&g.trusted); p256_release(&g.signer);
    kdf_sha256_release(&g.workspace);
    const wchar_t* const files[]{g.acceptance, g.lock, g.sibling, g.providers, g.state, g.journal};
    for (const wchar_t* path : files) DeleteFileW(path);
    check(RemoveDirectoryW(g.root) != 0, "plugin records: every test file was removed");
}

} // namespace maicord
