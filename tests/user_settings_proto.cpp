#include "maicord/protocol/user_settings_proto.hpp"
#include "maicord/protocol/account_settings.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
using K = UserSettingsKind;
constexpr UserSettingsField compact{6, 17, 0, K::boolean}, embeds{6, 12, 0, K::boolean},
    emoticons{6, 21, 0, K::boolean}, explicit_guilds{6, 29, 1, K::enumeration},
    explicit_friends{6, 29, 2, K::enumeration}, legacy_explicit{6, 19, 0, K::uint32},
    friend_flags{8, 11, 0, K::uint32},
    hour_cycle{13, 9, 0, K::enumeration}, locale{12, 1, 0, K::string}, theme{13, 1, 0, K::enumeration};
// versions{1,5,42}, guilds (not stored), text_and_images (two occurrences),
// privacy, status (not stored), appearance{theme 3, hour cycle 2}.
constexpr u8 kText[]{98, 2, 8, 1, 0x98, 0x06, 7, 0x8A, 0x01, 0, 0xEA, 0x01, 4, 16, 2, 56, 9};
constexpr u8 kText2[]{0xAA, 0x01, 0};
constexpr u8 kProto[]{10, 6, 8, 1, 16, 5, 24, 42,
                      26, 2, 10, 0,
                      50, 17, 98, 2, 8, 1, 0x98, 0x06, 7, 0x8A, 0x01, 0, 0xEA, 0x01, 4, 16, 2, 56, 9,
                      66, 13, 90, 2, 8, 6, 25, 1, 2, 3, 4, 5, 6, 7, 8,
                      90, 10, 10, 8, 10, 6, 'o', 'n', 'l', 'i', 'n', 'e',
                      106, 4, 8, 3, 72, 2,
                      50, 3, 0xAA, 0x01, 0};
bool contains(ConstSlice<u8> haystack, const u8* needle, u32 bytes) noexcept {
    for (u32 i = 0; i + bytes <= haystack.count; ++i)
        if (bytes_equal(haystack.data + i, needle, bytes))
            return true;
    return false;
}
template <u32 N> bool has(ConstSlice<u8> haystack, const u8 (&needle)[N]) noexcept {
    return contains(haystack, needle, N);
}
bool text_equal(Slice<u8> data, u32 bytes, Utf8View expected) noexcept {
    return bytes == expected.bytes && bytes_equal(data.data, expected.data, bytes);
}
// Settings arrive as base64; encode raw protobuf fixtures first.
UserSettingsExtractResult extract_raw(ConstSlice<u8> proto, Slice<u8> output) noexcept {
    char encoded[256]{};
    const auto base = base64_encode(proto, {encoded, sizeof(encoded)});
    return base.ok() ? user_settings_proto_extract_base64({encoded, base.bytes}, kUserSettingsStoredGroups, {}, output)
                     : UserSettingsExtractResult{0, 0, UserSettingsProtoError::input};
}
} // namespace

void test_user_settings_proto(void (*check)(bool, const char*) noexcept) noexcept {
    u8 extracted[512]{};
    const auto ex = extract_raw({kProto, sizeof(kProto)}, {extracted, sizeof(extracted)});
    const ConstSlice<u8> message{extracted, ex.bytes};
    const auto text = user_settings_proto_group(message, 6);
    check(ex.ok() && ex.present_mask == ((1u << 1) | (1u << 6) | (1u << 8) | (1u << 13)) &&
              !user_settings_proto_group(message, 3).count && !user_settings_proto_group(message, 11).count &&
              text.count == sizeof(kText) + sizeof(kText2) && bytes_equal(text.data, kText, sizeof(kText)) &&
              bytes_equal(text.data + sizeof(kText), kText2, sizeof(kText2)),
          "user settings: extraction keeps stored groups only and merges repeated groups in order");
    char encoded[256]{};
    const u32 encoded_bytes = base64_encoded_size(sizeof(kProto));
    base64_encode({kProto, sizeof(kProto)}, {encoded, encoded_bytes});
    u8 from_text[512]{};
    const auto ex64 = user_settings_proto_extract_base64({encoded, encoded_bytes}, kUserSettingsStoredGroups,
                                                         utf8_literal("hash+/="), {from_text, sizeof(from_text)});
    check(ex64.ok() && ex64.bytes == ex.bytes + 10 && bytes_equal(from_text, extracted, ex.bytes) &&
              user_settings_proto_group({from_text, ex64.bytes}, kUserSettingsSessionHashField).count == 7,
          "user settings: base64 extraction with a session hash keeps the hashless groups and carries the hash");
    const u8 marked[]{66, 0, 0xC8, 0x3E, 1};
    check(user_settings_proto_partial({marked, sizeof(marked)}) && !user_settings_proto_partial(message),
          "user settings: the gateway hand-off marks partial updates");
    u8 bad[]{50, 9, 1};
    check(extract_raw({bad, 3}, {from_text, sizeof(from_text)}).error == UserSettingsProtoError::protobuf &&
              extract_raw({kProto, sizeof(kProto)}, {from_text, 8}).error == UserSettingsProtoError::capacity,
          "user settings: malformed input and short output are rejected");

    UserSettingsValue v{};
    const u8 legacy_zero[]{0x9A, 0x01, 0}, legacy_all[]{0x9A, 0x01, 2, 8, 2};
    check(!user_settings_proto_read({}, legacy_explicit, &v) &&
              user_settings_proto_read({legacy_zero, sizeof(legacy_zero)}, legacy_explicit, &v) &&
              v.present && v.number == 0 &&
              user_settings_proto_read({legacy_all, sizeof(legacy_all)}, legacy_explicit, &v) && v.number == 2,
          "user settings: legacy explicit filter distinguishes missing from present zero UInt32Value");
    check(user_settings_proto_read(text, embeds, &v) && v.number == 1 &&
              user_settings_proto_read(text, compact, &v) && v.number == 0 &&
              user_settings_proto_read(text, emoticons, &v) && v.number == 0 &&
              user_settings_proto_read(text, explicit_friends, &v) && v.number == 2 &&
              !user_settings_proto_read(text, explicit_guilds, &v) &&
              user_settings_proto_read(user_settings_proto_group(message, 13), hour_cycle, &v) && v.number == 2 &&
              user_settings_proto_read(user_settings_proto_group(message, 8), friend_flags, &v) && v.number == 6,
          "user settings: typed reads cover wrappers, empty wrappers, enums and nested fields");

    u8 rewritten[256]{}, nested[256]{};
    const u32 r = user_settings_proto_rewrite(text, compact, user_settings_number(1), {rewritten, sizeof(rewritten)});
    const ConstSlice<u8> changed{rewritten, r};
    const u8 compact_true[]{0x8A, 0x01, 2, 8, 1}, compact_empty[]{0x8A, 0x01, 0}, unknown[]{0x98, 0x06, 7},
        explicit_old[]{0xEA, 0x01, 4, 16, 2, 56, 9}, embeds_true[]{98, 2, 8, 1};
    check(r != ~0u && has(changed, compact_true) && !has(changed, compact_empty) && has(changed, unknown) &&
              has(changed, explicit_old) && has(changed, embeds_true) && has(changed, kText2) &&
              user_settings_proto_read(changed, compact, &v) && v.number == 1,
          "user settings: rewrite replaces one wrapper and preserves unknown and sibling fields byte for byte");
    const u32 n = user_settings_proto_rewrite(changed, explicit_guilds, user_settings_number(3), {nested, sizeof(nested)});
    const u8 explicit_new[]{0xEA, 0x01, 6, 16, 2, 56, 9, 8, 3};
    check(n != ~0u && has({nested, n}, explicit_new) && has({nested, n}, unknown) &&
              user_settings_proto_read({nested, n}, explicit_guilds, &v) && v.number == 3,
          "user settings: nested rewrite keeps unknown sub-message fields");
    const u32 cleared = user_settings_proto_rewrite({nested, n}, compact, {}, {rewritten, sizeof(rewritten)});
    check(cleared != ~0u && !user_settings_proto_read({rewritten, cleared}, compact, &v) &&
              user_settings_proto_rewrite(text, hour_cycle, user_settings_number(0), {nested, 4}) == ~0u,
          "user settings: absent values clear the field and short output fails");
    const u32 strings = user_settings_proto_rewrite({}, locale, user_settings_text(utf8_literal("sl")), {nested, 16});
    const u8 locale_wire[]{10, 4, 10, 2, 's', 'l'};
    check(strings == 6 && bytes_equal(nested, locale_wire, 6),
          "user settings: string wrappers encode as StringValue messages");

    u8 json[256]{};
    const u8 partial[]{106, 2, 72, 1};
    const u32 j0 = user_settings_proto_write_patch_json({partial, 4}, 0, {json, sizeof(json)});
    const u32 j1 = user_settings_proto_write_patch_json({partial, 4}, 42, {json + 128, 128});
    const u8 status_group[]{90, 0}, versions_group[]{10, 0}, scalar[]{8, 1};
    check(text_equal({json, 128}, j0, utf8_literal("{\"settings\":\"agJIAQ==\"}")) &&
              text_equal({json + 128, 128}, j1, utf8_literal("{\"settings\":\"agJIAQ==\",\"required_data_version\":42}")) &&
              !user_settings_proto_patch_valid({status_group, 2}) &&
              !user_settings_proto_patch_valid({versions_group, 2}) && !user_settings_proto_patch_valid({scalar, 2}) &&
              !user_settings_proto_write_patch_json({partial, 4}, 0, {json, 20}),
          "user settings: PATCH JSON envelope, data version and partial message validation");

    constexpr char response[] = "{\"settings\":\"agJIAQ==\",\"out_of_date\":true}";
    char unescape[64];
    u8 decoded[64]{};
    const auto parsed = user_settings_proto_decode_response({response, sizeof(response) - 1}, kUserSettingsStoredGroups,
                                                            {unescape, sizeof(unescape)}, {decoded, sizeof(decoded)});
    check(parsed.extracted.ok() && parsed.out_of_date && parsed.settings_present && parsed.extracted.bytes == 4 &&
              bytes_equal(decoded, partial, 4),
          "user settings: GET/PATCH response envelope decodes settings and out_of_date");

    auto* store = static_cast<UserSettingsStore*>(
        VirtualAlloc(nullptr, sizeof(UserSettingsStore), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!store) {
        check(false, "user settings: store allocation");
        return;
    }
    user_settings_store_reset(store);
    check(!user_settings_store_set(store, compact, user_settings_number(1), 0),
          "user settings: edits are refused before a full snapshot so groups are never rebuilt from nothing");
    check(user_settings_store_apply(store, {from_text, ex64.bytes}, false) && store->loaded &&
              store->data_version == 42 && store->session_hash_bytes == 7,
          "user settings: full snapshot loads groups, data version and session hash");
    store->live = true;
    check(user_settings_store_set(store, compact, user_settings_number(1), 100) && store->due_ms == 1100 &&
              !user_settings_store_due(*store, 1099) && user_settings_store_due(*store, 1100) &&
              user_settings_store_read(*store, compact, &v) && v.number == 1,
          "user settings: optimistic edit applies immediately and batches for one second");
    check(user_settings_store_set(store, compact, user_settings_number(0), 200) && !store->dirty_count &&
              !store->due_ms,
          "user settings: reverting to the server value cancels the pending edit");
    user_settings_store_set(store, compact, user_settings_number(1), 300);
    user_settings_store_set(store, hour_cycle, user_settings_number(1), 400);
    user_settings_store_set(store, explicit_guilds, user_settings_number(2), 500);
    u8 patch[1024]{};
    u32 required = 99;
    const u32 sent = user_settings_store_begin(store, {patch, sizeof(patch)}, &required);
    const ConstSlice<u8> sent_message{patch, sent};
    const auto sent_text = user_settings_proto_group(sent_message, 6);
    check(sent && !required && user_settings_proto_patch_valid(sent_message) &&
              !user_settings_proto_group(sent_message, 8).count && !user_settings_proto_group(sent_message, 1).count &&
              user_settings_proto_read(sent_text, compact, &v) && v.number == 1 &&
              user_settings_proto_read(sent_text, explicit_guilds, &v) && v.number == 2 &&
              user_settings_proto_read(sent_text, explicit_friends, &v) && v.number == 2 &&
              has(sent_text, unknown) && has(sent_text, kText2) &&
              user_settings_proto_read(user_settings_proto_group(sent_message, 13), hour_cycle, &v) && v.number == 1 &&
              user_settings_proto_read(user_settings_proto_group(sent_message, 13), theme, &v) && v.number == 3,
          "user settings: PATCH carries whole touched groups only, with edits and unknown fields");
    check(store->inflight_active && !user_settings_store_due(*store, 10'000) &&
              user_settings_store_read(*store, compact, &v) && v.number == 1,
          "user settings: in-flight edits stay visible and block a second request");
    user_settings_store_set(store, emoticons, user_settings_number(1), 600);
    const u8 privacy_update[]{66, 4, 90, 2, 8, 2};
    check(user_settings_store_apply(store, {privacy_update, sizeof(privacy_update)}, true) &&
              user_settings_store_read(*store, friend_flags, &v) && v.number == 2 &&
              user_settings_store_read(*store, compact, &v) && v.number == 1 &&
              user_settings_store_server_group(*store, 6).count == text.count,
          "user settings: a partial gateway update replaces only its groups and pending edits rebase on it");
    auto completion = user_settings_store_complete(store, UserSettingsOutcome::failed, {}, 0);
    check(completion.rolled_back && user_settings_store_read(*store, compact, &v) && v.number == 0 &&
              user_settings_store_read(*store, hour_cycle, &v) && v.number == 2 &&
              user_settings_store_read(*store, emoticons, &v) && v.number == 1 && store->dirty_count == 1 &&
              store->due_ms == 1600,
          "user settings: a failed PATCH rolls back in-flight edits but keeps newer pending ones");
    const u32 again = user_settings_store_begin(store, {patch, sizeof(patch)}, &required);
    completion = user_settings_store_complete(store, UserSettingsOutcome::rate_limited, {}, 5000);
    check(again && completion.retry && store->dirty_count == 1 && store->due_ms == 5000 &&
              user_settings_store_read(*store, emoticons, &v) && v.number == 1,
          "user settings: rate limiting re-queues the edits for the retry time");
    user_settings_store_begin(store, {patch, sizeof(patch)}, &required);
    const u8 echo[]{10, 2, 24, 43, 50, 5, 0xAA, 0x01, 2, 8, 1};
    completion = user_settings_store_complete(store, UserSettingsOutcome::saved, {echo, sizeof(echo)}, 0);
    check(!completion.rolled_back && store->data_version == 43 && !store->dirty_count && !store->inflight_active &&
              user_settings_store_read(*store, emoticons, &v) && v.number == 1,
          "user settings: a saved PATCH adopts the returned settings and data version");
    user_settings_store_set(store, emoticons, user_settings_number(0), 0);
    user_settings_store_begin(store, {patch, sizeof(patch)}, &required);
    completion = user_settings_store_complete(store, UserSettingsOutcome::out_of_date, {echo, sizeof(echo)}, 0);
    check(completion.rolled_back && user_settings_store_read(*store, emoticons, &v) && v.number == 1,
          "user settings: out_of_date discards the edit in favour of the newer server settings");
    store->live = false;
    user_settings_store_set(store, emoticons, user_settings_number(0), 0);
    store->live = true;
    user_settings_store_begin(store, {patch, sizeof(patch)}, &required);
    check(required == 43, "user settings: offline edits pin required_data_version for the conflict check");
    completion = user_settings_store_complete(store, UserSettingsOutcome::invalid_data, {}, 0);
    check(completion.rolled_back && store->refetch, "user settings: invalid data rolls back and asks for a refetch");
    VirtualFree(store, 0, MEM_RELEASE);
}

// Per-server privacy lists: privacy.restricted_guild_ids (3) and friends are
// repeated fixed64, read packed or unpacked and written packed.
void test_user_settings_guild_lists(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr UserSettingsField restricted{8, 3, 0, K::id_member}, requests{8, 16, 0, K::id_member},
        activity{8, 13, 0, K::id_member}, flags{8, 11, 0, K::uint32};
    constexpr u64 a = 0x0102030405060708ull, b = 0x1112131415161718ull, c = 0x2122232425262728ull,
                  d = 0x3132333435363738ull;
    // restricted [a, b] packed, default_guilds_restricted, requests [c] unpacked,
    // friend_source_flags {6}, restricted again [a] (a duplicate in a second run).
    constexpr u8 privacy[]{26,   16,   8,    7,    6,    5,    4,    3,    2,    1,    0x18, 0x17, 0x16, 0x15, 0x14,
                           0x13, 0x12, 0x11, 32,   1,    0x81, 0x01, 0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
                           90,   2,    8,    6,    26,   8,    8,    7,    6,    5,    4,    3,    2,    1};
    const ConstSlice<u8> group{privacy, sizeof(privacy)};
    check(user_settings_proto_contains(group, restricted, a) && user_settings_proto_contains(group, restricted, b) &&
              !user_settings_proto_contains(group, restricted, c) && user_settings_proto_contains(group, requests, c) &&
              !user_settings_proto_contains(group, activity, a) && !user_settings_proto_contains(group, restricted, 0),
          "user settings: guild ID lists read packed and unpacked runs");
    UserSettingsValue v{};
    check(!user_settings_proto_read(group, restricted, &v), "user settings: ID lists are not scalar reads");
    u8 out[256]{}, out2[256]{};
    const u32 added = user_settings_proto_rewrite(group, restricted, user_settings_member(d, true), {out, sizeof(out)});
    const ConstSlice<u8> with_d{out, added};
    const u8 packed_abd[]{26, 24, 8, 7, 6, 5, 4, 3, 2, 1, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
                          0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31};
    const u8 unpacked_c[]{0x81, 0x01, 0x28}, default_flag[]{32, 1}, wrapper[]{90, 2, 8, 6};
    check(added != ~0u && has(with_d, packed_abd) && has(with_d, unpacked_c) && has(with_d, default_flag) &&
              has(with_d, wrapper) && user_settings_proto_read(with_d, flags, &v) && v.number == 6 &&
              added == sizeof(privacy) - 18 - 10 + 26,
          "user settings: adding an ID writes one deduplicated packed run, appended, siblings intact");
    const u32 removed = user_settings_proto_rewrite(with_d, restricted, user_settings_member(a, false), {out2, sizeof(out2)});
    const u8 packed_bd[]{26, 16, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
                         0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31};
    check(removed != ~0u && has({out2, removed}, packed_bd) && !user_settings_proto_contains({out2, removed}, restricted, a),
          "user settings: removing an ID keeps the others in order");
    u32 bytes = removed;
    u8* source = out2;
    u8* target = out;
    const u64 drops[]{b, d};
    for (u64 id : drops) {
        bytes = user_settings_proto_rewrite({source, bytes}, restricted, user_settings_member(id, false), {target, 256});
        u8* swap = source;
        source = target;
        target = swap;
    }
    bool tag = false;
    for (u32 i = 0; i < bytes; ++i)
        tag = tag || source[i] == 26;
    check(bytes != ~0u && !tag && user_settings_proto_contains({source, bytes}, requests, c),
          "user settings: an emptied list is omitted");
    const u32 converted = user_settings_proto_rewrite(group, requests, user_settings_member(a, true), {out, sizeof(out)});
    const u8 packed_ca[]{0x82, 0x01, 16, 0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21, 8, 7, 6, 5, 4, 3, 2, 1};
    check(converted != ~0u && has({out, converted}, packed_ca) && !has({out, converted}, unpacked_c),
          "user settings: an unpacked list is rewritten packed");
    const u32 same = user_settings_proto_rewrite(group, restricted, user_settings_member(b, true), {out, sizeof(out)});
    const u8 packed_ab[]{26, 16, 8, 7, 6, 5, 4, 3, 2, 1, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11};
    check(same != ~0u && has({out, same}, packed_ab) &&
              user_settings_proto_rewrite(group, restricted, user_settings_member(0, true), {out, sizeof(out)}) == ~0u,
          "user settings: re-adding a listed ID keeps its place; ID zero is refused");

    auto* store = static_cast<UserSettingsStore*>(
        VirtualAlloc(nullptr, sizeof(UserSettingsStore), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!store) {
        check(false, "user settings: list store allocation");
        return;
    }
    user_settings_store_reset(store);
    u8 snapshot[128]{};
    snapshot[0] = 66;
    snapshot[1] = sizeof(privacy);
    copy_bytes(snapshot + 2, privacy, sizeof(privacy));
    check(user_settings_store_apply(store, {snapshot, sizeof(privacy) + 2u}, false) &&
              user_settings_store_contains(*store, restricted, a) && !user_settings_store_contains(*store, activity, d),
          "user settings: store membership reads the server lists");
    store->live = true;
    check(user_settings_store_set(store, restricted, user_settings_member(a, false), 10) &&
              user_settings_store_set(store, restricted, user_settings_member(d, true), 20) &&
              user_settings_store_set(store, activity, user_settings_member(d, true), 30) && store->dirty_count == 3 &&
              !user_settings_store_contains(*store, restricted, a) && user_settings_store_contains(*store, restricted, d) &&
              user_settings_store_contains(*store, restricted, b) && user_settings_store_contains(*store, activity, d),
          "user settings: membership edits are keyed per ID and apply optimistically");
    check(user_settings_store_set(store, restricted, user_settings_member(a, true), 40) && store->dirty_count == 2 &&
              user_settings_store_contains(*store, restricted, a) &&
              !user_settings_store_set(store, restricted, user_settings_member(0, true), 50),
          "user settings: toggling an ID back cancels its pending edit");
    u8 patch[512]{};
    u32 required = 0;
    const u32 sent = user_settings_store_begin(store, {patch, sizeof(patch)}, &required);
    const auto sent_privacy = user_settings_proto_group({patch, sent}, 8);
    check(sent && user_settings_proto_patch_valid({patch, sent}) &&
              user_settings_proto_contains(sent_privacy, restricted, a) &&
              user_settings_proto_contains(sent_privacy, restricted, b) &&
              user_settings_proto_contains(sent_privacy, restricted, d) &&
              user_settings_proto_contains(sent_privacy, activity, d) &&
              user_settings_proto_contains(sent_privacy, requests, c) && has(sent_privacy, wrapper),
          "user settings: the PATCH carries the whole privacy group with every list edit");
    const auto completion = user_settings_store_complete(store, UserSettingsOutcome::failed, {}, 0);
    check(completion.rolled_back && !user_settings_store_contains(*store, restricted, d) &&
              !user_settings_store_contains(*store, activity, d),
          "user settings: a failed list PATCH rolls the memberships back");
    // USER_SETTINGS_PROTO_UPDATE (partial): the server now restricts d.
    const u8 update[]{66, 10, 26, 8, 0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31};
    check(user_settings_store_apply(store, {update, sizeof(update)}, true) &&
              user_settings_store_contains(*store, restricted, d) && !user_settings_store_contains(*store, restricted, a),
          "user settings: a gateway update replaces the lists live");
    VirtualFree(store, 0, MEM_RELEASE);
}

void test_account_settings_rest(void (*check)(bool, const char*) noexcept) noexcept {
    u8 target[kAccountSettingsMaximumTargetBytes]{}, body[kAccountSettingsMaximumBodyBytes]{};
    const auto view = [&](u32 bytes) noexcept { return Utf8View{reinterpret_cast<const char*>(body), bytes}; };
    const auto path = [&](u32 bytes) noexcept { return Utf8View{reinterpret_cast<const char*>(target), bytes}; };
    const auto bytes_of = [](Utf8View s) noexcept { return ConstSlice<u8>{reinterpret_cast<const u8*>(s.data), s.bytes}; };
    AccountSettingsRequest r{};
    r.operation = AccountSettingsOperation::session_logout;
    r.payload = bytes_of(utf8_literal("aBc/12+3="));
    auto w = account_settings_write_request(r, {target, sizeof(target)}, {body, sizeof(body)});
    check(w.ok() && w.method == RestMethod::post &&
              utf8_equal(path(w.target_bytes), utf8_literal("/api/v10/auth/sessions/logout")) &&
              utf8_equal(view(w.body_bytes), utf8_literal("{\"session_id_hashes\":[\"aBc/12+3=\"]}")) &&
              !account_settings_primary(r.operation),
          "account settings: device log out posts exactly the selected session hash");
    r.payload = bytes_of(utf8_literal("a\",\"b"));
    check(!account_settings_write_request(r, {target, sizeof(target)}, {body, sizeof(body)}).ok(),
          "account settings: session hashes outside the token alphabet are rejected");
    r = {};
    r.operation = AccountSettingsOperation::token_revoke;
    r.id = 1234567890123ull;
    w = account_settings_write_request(r, {target, sizeof(target)}, {body, sizeof(body)});
    check(w.ok() && w.method == RestMethod::delete_method && !w.body_bytes &&
              utf8_equal(path(w.target_bytes), utf8_literal("/api/v10/oauth2/tokens/1234567890123")),
          "account settings: deauthorize deletes one token id without a body");
    r.id = 0;
    check(!account_settings_write_request(r, {target, sizeof(target)}, {body, sizeof(body)}).ok(),
          "account settings: a zero token id is rejected");
    r = {};
    r.operation = AccountSettingsOperation::profile_patch;
    r.field = AccountProfileField::pronouns;
    r.payload = bytes_of(utf8_literal("they/them"));
    w = account_settings_write_request(r, {target, sizeof(target)}, {body, sizeof(body)});
    check(w.ok() && w.method == RestMethod::patch &&
              utf8_equal(path(w.target_bytes), utf8_literal("/api/v10/users/@me/profile")) &&
              utf8_equal(view(w.body_bytes), utf8_literal("{\"pronouns\":\"they/them\"}")),
          "account settings: pronouns PATCH body");
    r.field = AccountProfileField::bio;
    r.payload = bytes_of(utf8_literal("Line \"one\"\nline two"));
    w = account_settings_write_request(r, {target, sizeof(target)}, {body, sizeof(body)});
    check(w.ok() && utf8_equal(view(w.body_bytes), utf8_literal("{\"bio\":\"Line \\\"one\\\"\\nline two\"}")),
          "account settings: about me PATCH body escapes quotes and newlines");
    static const char long_text[kAccountSettingsPronounsBytes + 1]{};
    r.field = AccountProfileField::pronouns;
    r.payload = {reinterpret_cast<const u8*>(long_text), kAccountSettingsPronounsBytes + 1};
    const u8 invalid_utf8[]{0xC3};
    AccountSettingsRequest bad_utf8 = r;
    bad_utf8.payload = {invalid_utf8, 1};
    check(!account_settings_request_valid(r) && !account_settings_request_valid(bad_utf8),
          "account settings: over-long and invalid UTF-8 profile text is rejected");
    r = {};
    r.operation = AccountSettingsOperation::display_name;
    w = account_settings_write_request(r, {target, sizeof(target)}, {body, sizeof(body)});
    check(w.ok() && utf8_equal(path(w.target_bytes), utf8_literal("/api/v10/users/@me")) &&
              utf8_equal(view(w.body_bytes), utf8_literal("{\"global_name\":null}")),
          "account settings: an empty display name resets to the username");
    r = {};
    r.operation = AccountSettingsOperation::profile;
    r.id = 42;
    w = account_settings_write_request(r, {target, sizeof(target)}, {body, sizeof(body)});
    check(w.ok() && w.method == RestMethod::get &&
              utf8_equal(path(w.target_bytes), utf8_literal("/api/v10/users/42/profile")),
          "account settings: profile read target");
    r = {};
    const u8 partial[]{106, 2, 72, 1};
    r.operation = AccountSettingsOperation::settings_patch;
    r.payload = {partial, 4};
    r.required_data_version = 7;
    w = account_settings_write_request(r, {target, sizeof(target)}, {body, sizeof(body)});
    check(w.ok() && account_settings_primary(r.operation) && w.method == RestMethod::patch &&
              utf8_equal(path(w.target_bytes), utf8_literal("/api/v10/users/@me/settings-proto/1")) &&
              utf8_equal(view(w.body_bytes), utf8_literal("{\"settings\":\"agJIAQ==\",\"required_data_version\":7}")),
          "account settings: settings-proto PATCH serializes the partial message");
    r.reserved[0] = 1;
    check(!account_settings_request_valid(r), "account settings: reserved bytes must be zero");

    u32 structurals[512];
    constexpr char sessions[] =
        "{\"user_sessions\":[{\"id_hash\":\"h1\",\"approx_last_used_time\":\"2026-10-01T12:00:00.000000+00:00\","
        "\"client_info\":{\"os\":\"Windows\",\"platform\":\"Discord Client\",\"location\":\"Copenhagen, Denmark\"}},"
        "{\"id_hash\":\"bad hash\",\"client_info\":{}}]}";
    AccountSession list[4]{};
    const u32 count = account_settings_decode_sessions({sessions, sizeof(sessions) - 1}, {structurals, 512}, {list, 4});
    check(count == 1 && list[0].id_hash_bytes == 2 && list[0].os_bytes == 7 && list[0].location_bytes == 19 &&
              list[0].last_used_ms == 1'790'856'000'000ull,
          "account settings: sessions decode client info and last use, dropping invalid hashes");
    constexpr char tokens[] =
        "[{\"id\":\"11\",\"scopes\":[\"identify\",\"guilds\"],\"application\":{\"id\":\"22\",\"name\":\"App\"}}]";
    AccountAuthorizedApp apps[2]{};
    check(account_settings_decode_tokens({tokens, sizeof(tokens) - 1}, {structurals, 512}, {apps, 2}) == 1 &&
              apps[0].token_id == 11 && apps[0].application_id == 22 && apps[0].name_bytes == 3 &&
              utf8_equal({apps[0].scopes, apps[0].scopes_bytes}, utf8_literal("identify, guilds")),
          "account settings: authorized apps decode token, application and scopes");
    constexpr char self[] = "{\"id\":\"1\",\"username\":\"rob\",\"global_name\":null,\"email\":\"rob@example.com\","
                            "\"phone\":null,\"verified\":true,\"mfa_enabled\":false}";
    AccountSelfUser user{};
    check(account_settings_decode_self({self, sizeof(self) - 1}, {structurals, 512}, &user) &&
              user.username_bytes == 3 && !user.global_name_bytes && user.email_bytes == 15 && user.email_known &&
              user.phone_known && !user.phone_bytes && user.verified,
          "account settings: self user decodes email and phone presence");
    constexpr char profile[] = "{\"user\":{\"id\":\"1\"},\"user_profile\":{\"bio\":\"hi\",\"pronouns\":\"she/her\"}}";
    AccountProfileText text{};
    check(account_settings_decode_profile({profile, sizeof(profile) - 1}, {structurals, 512}, &text) &&
              text.bio_bytes == 2 && text.pronouns_bytes == 7,
          "account settings: profile text decodes bio and pronouns");
    constexpr char error[] = "{\"message\":\"Two factor is required\",\"code\":60003}";
    char masked[32];
    const u32 email = account_settings_mask_email(utf8_literal("rob@example.com"), {masked, 32});
    const u32 phone = account_settings_mask_phone(utf8_literal("+15550100123"), {masked + 16, 16});
    check(account_settings_error_code({error, sizeof(error) - 1}, {structurals, 512}) == 60003 &&
              utf8_equal({masked, email}, utf8_literal("r**@example.com")) &&
              utf8_equal({masked + 16, phone}, utf8_literal("********0123")),
          "account settings: error codes and masked email and phone");
}
} // namespace maicord

namespace {
maicord::u32 failures = 0;
void check(bool success, const char* name) noexcept {
    if (success)
        return;
    ++failures;
    maicord::test_write_text("FAIL ");
    maicord::test_write_line(name);
}
} // namespace

extern "C" __declspec(noreturn) void WINAPI maicord_user_settings_tests_entry() noexcept {
    maicord::test_user_settings_proto(check);
    maicord::test_user_settings_guild_lists(check);
    maicord::test_account_settings_rest(check);
    constexpr char pass[] = "PASS user settings\n";
    constexpr char fail[] = "FAIL user settings\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failures ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failures ? 1 : 0);
}
