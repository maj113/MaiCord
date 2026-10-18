#include "maicord/protocol/gif_favorites.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/memory.hpp"
namespace maicord {
namespace {
struct FixtureWire {
    Slice<u8> bytes;
    u32 used = 0;
    void number(u64 n) noexcept {
        do {
            const u8 b = static_cast<u8>(n & 127);
            n >>= 7;
            if (used < bytes.count)
                bytes[used++] = b | (n ? 128 : 0);
        } while (n);
    }
    void scalar(u32 field, u64 n) noexcept {
        number(field * 8);
        number(n);
    }
    void message(u32 field, ConstSlice<u8> input) noexcept {
        number(field * 8 + 2);
        number(input.count);
        if (input.count <= bytes.count - used) {
            copy_bytes(bytes.data + used, input.data, input.count);
            used += input.count;
        }
    }
    void string(u32 field, Utf8View input) noexcept {
        message(field, {reinterpret_cast<const u8*>(input.data), input.bytes});
    }
    ConstSlice<u8> view() const noexcept {
        return {bytes.data, used};
    }
};
constexpr auto key_a = utf8_literal("https://example.test/\xE9\x9B\xAA.gif?x=\xE2\x9C\x93");
constexpr auto key_b = utf8_literal("https://example.test/legacy");
constexpr auto src_a = utf8_literal("https://example.test/\xE9\x9B\xAA.webp");
void favorite(FixtureWire& group, Utf8View key, Utf8View src, u32 order, u32 format, u32 unknown) noexcept {
    u8 value_bytes[256]{}, entry_bytes[512]{};
    FixtureWire value{{value_bytes, sizeof(value_bytes)}}, entry{{entry_bytes, sizeof(entry_bytes)}};
    value.scalar(1, format);
    value.string(2, src);
    value.scalar(3, 320);
    value.scalar(4, 180);
    value.scalar(5, order);
    value.scalar(9, unknown);
    entry.string(1, key);
    entry.message(2, value.view());
    entry.scalar(8, unknown + 1);
    group.message(1, entry.view());
}
bool contains(ConstSlice<u8> bytes, ConstSlice<u8> needle) noexcept {
    for (u32 i = 0; i + needle.count <= bytes.count; ++i)
        if (bytes_equal(bytes.data + i, needle.data, needle.count))
            return true;
    return false;
}
Utf8View patch_base64(ConstSlice<u8> json) noexcept {
    // Test the writer's fixed conditional envelope independently of its JSON reader.
    u32 end = 13;
    while (end < json.count && json.data[end] != '"')
        ++end;
    return end < json.count ? Utf8View{reinterpret_cast<const char*>(json.data + 13), end - 13}
                            : Utf8View{};
}
// Snapshots arrive as base64; encode raw protobuf fixtures first. An encoding
// failure yields text the decoder rejects.
char g_encoded[4096];
Utf8View encode(ConstSlice<u8> proto) noexcept {
    const auto base = base64_encode(proto, {g_encoded, sizeof(g_encoded)});
    return base.ok() ? Utf8View{g_encoded, base.bytes} : utf8_literal("*");
}
GifFavoritesResult measure(ConstSlice<u8> proto) noexcept {
    return gif_favorites_measure_base64(encode(proto));
}
GifFavoritesResult decode(ConstSlice<u8> proto, Slice<u8> owned, Slice<GifFavoriteRecord> records,
                          Slice<u32> index) noexcept {
    return gif_favorites_decode_base64(encode(proto), owned, records, index);
}
} // namespace
void test_gif_favorites(void (*check)(bool, const char*) noexcept) noexcept {
    u8 first_bytes[1024]{}, second_bytes[512]{}, root_bytes[2048]{}, version_bytes[32]{};
    FixtureWire first{{first_bytes, sizeof(first_bytes)}}, second{{second_bytes, sizeof(second_bytes)}},
        root{{root_bytes, sizeof(root_bytes)}}, version{{version_bytes, sizeof(version_bytes)}};
    favorite(first, key_a, utf8_literal("https://example.test/old.webp"), 100, 1, 31);
    favorite(first, key_b, utf8_literal("https://example.test/legacy.webm"), 8, 2, 41);
    first.scalar(2, 0);
    first.scalar(29, 77);
    favorite(second, key_a, src_a, 2, 1, 51);
    version.scalar(1, 12);
    version.scalar(2, 13);
    version.scalar(3, 14);
    root.message(1, version.view());
    root.message(2, first.view());
    root.message(2, second.view());
    const u8 unrelated[]{8, 17};
    root.message(3, {unrelated, sizeof(unrelated)});
    root.scalar(28, 123);
    const auto measured = measure(root.view());
    check(measured.ok() && measured.entry_count == 3 && measured.group_bytes == first.used + second.used &&
              measured.group_present && measured.versions_present &&
              measured.data_version == 14 && !measured.hide_tooltip,
          "GIF favorites: real proto2 versions/map fields and repeated group merge measure completely");
    u8 owned[2048]{}, changed[2048]{}, scratch[4096]{}, json[4096]{};
    GifFavoriteRecord records[16]{};
    u32 index[64]{};
    auto decoded = decode(root.view(), {owned, sizeof(owned)}, {records, 16},
                                              {index, measured.index_words});
    check(
        decoded.ok() && decoded.record_count == 2 && records[0].order == 8 &&
            records[0].format == GifFavoriteFormat::video && records[1].order == 2 &&
            utf8_equal(records[1].url, key_a) && utf8_equal(records[1].src, src_a) &&
            records[1].url.data >= reinterpret_cast<const char*>(owned) &&
            records[1].src.data < reinterpret_cast<const char*>(owned + decoded.group_bytes) &&
            bytes_equal(owned, first_bytes, first.used) &&
            bytes_equal(owned + first.used, second_bytes, second.used),
        "GIF favorites: canonical last-key-wins records sort saved order and borrow one opaque owned bank");
    const auto* found =
        gif_favorites_find({records, decoded.record_count}, {index, decoded.index_words}, key_a);
    check(
        found && found->order == 2 &&
            !gif_favorites_find({records, decoded.record_count}, {index, decoded.index_words},
                                utf8_literal("https://example.test/not-saved")),
        "GIF favorites: permanent exact UTF8 lookup indexes canonical records without URL identity hashes");
    GifFavoriteMutation add{utf8_literal("https://example.test/new"),
                            utf8_literal("https://example.test/new.webp"),
                            640,
                            360,
                            999,
                            GifFavoriteFormat::image,
                            GifFavoriteMutationKind::add};
    auto written = gif_favorites_write_patch({owned, decoded.group_bytes}, 14, add,
                                             {scratch, sizeof(scratch)}, {json, sizeof(json)});
    check(written.ok() &&
              gif_favorites_validate_patch_json({reinterpret_cast<const char*>(json), written.bytes}) ==
                  GifFavoritesError::none &&
              contains({json, written.bytes},
                       {reinterpret_cast<const u8*>("\"required_data_version\":14"), 26}),
          "GIF favorites: native conditional PATCH validates exact favorite-only envelope and observed "
          "dataVersion");
    auto updated = gif_favorites_decode_base64(patch_base64({json, written.bytes}),
                                               {changed, sizeof(changed)}, {records, 16}, {index, 16});
    check(updated.ok() && updated.record_count == 3 && records[0].order == 9 &&
              utf8_equal(records[0].url, add.url) && records[0].width == 640 && records[0].height == 360 &&
              updated.hide_tooltip && !updated.versions_present,
          "GIF favorites: add uses canonical maxorder plus1, sets tooltip policy above2 and omits "
          "unrelated root groups");
    const u8 group_unknown[]{232, 1, 77}, value_unknown[]{72, 51}, entry_unknown[]{64, 52};
    add.url = key_a;
    add.src = utf8_literal("https://example.test/replaced.webp");
    written = gif_favorites_write_patch({owned, decoded.group_bytes}, 14, add, {scratch, sizeof(scratch)},
                                        {json, sizeof(json)});
    updated = gif_favorites_decode_base64(patch_base64({json, written.bytes}), {changed, sizeof(changed)},
                                          {records, 16}, {index, measured.index_words});
    check(written.ok() && updated.ok() && updated.record_count == 2 && records[0].order == 9 &&
              utf8_equal(records[0].url, key_a) && utf8_equal(records[0].src, add.src) &&
              contains({changed, updated.group_bytes}, {group_unknown, sizeof(group_unknown)}) &&
              contains({changed, updated.group_bytes}, {value_unknown, sizeof(value_unknown)}) &&
              contains({changed, updated.group_bytes}, {entry_unknown, sizeof(entry_unknown)}) &&
              contains({changed, updated.group_bytes}, {first_bytes + 2, 20}),
          "GIF favorites: re-add replaces only winning value while retaining opaque group/map/value and "
          "older duplicate bytes");
    GifFavoriteMutation reorder{
        key_b, {}, 0, 0, 1, GifFavoriteFormat::none, GifFavoriteMutationKind::reorder};
    written = gif_favorites_write_patch({owned, decoded.group_bytes}, 14, reorder,
                                        {scratch, sizeof(scratch)}, {json, sizeof(json)});
    updated = gif_favorites_decode_base64(patch_base64({json, written.bytes}), {changed, sizeof(changed)},
                                          {records, 16}, {index, measured.index_words});
    const auto* legacy =
        gif_favorites_find({records, updated.record_count}, {index, updated.index_words}, key_b);
    check(
        written.ok() && updated.ok() && legacy && legacy->order == 1 &&
            legacy->format == GifFavoriteFormat::video && legacy->width == 320 &&
            utf8_equal(legacy->src, utf8_literal("https://example.test/legacy.webm")),
        "GIF favorites: explicit reorder changes order only and retains unsupported legacy video metadata");
    GifFavoriteMutation remove{
        key_a, {}, 0, 0, 0, GifFavoriteFormat::none, GifFavoriteMutationKind::remove};
    written = gif_favorites_write_patch({owned, decoded.group_bytes}, 14, remove,
                                        {scratch, sizeof(scratch)}, {json, sizeof(json)});
    updated = gif_favorites_decode_base64(patch_base64({json, written.bytes}), {changed, sizeof(changed)},
                                          {records, 16}, {index, 4});
    check(written.ok() && updated.ok() && updated.entry_count == 1 && updated.record_count == 1 &&
              utf8_equal(records[0].url, key_b) &&
              contains({changed, updated.group_bytes}, {group_unknown, sizeof(group_unknown)}),
          "GIF favorites: remove deletes all exact Unicode-key duplicates and preserves every unrelated "
          "favorite");
    remove.url = utf8_literal("https://example.test/not-saved");
    json[0] = 93;
    check(gif_favorites_write_patch({owned, decoded.group_bytes}, 14, remove, {scratch, sizeof(scratch)},
                                    {json, sizeof(json)})
                      .error == GifFavoritesError::missing &&
              json[0] == 93,
          "GIF favorites: absent-key removal produces no fake successful or destructive patch");
    json[0] = 93;
    check(gif_favorites_write_patch({owned, decoded.group_bytes}, 14, add, {scratch, sizeof(scratch)},
                                    {json, 1})
                      .error == GifFavoritesError::capacity &&
              json[0] == 93,
          "GIF favorites: insufficient JSON capacity leaves complete publication untouched");
    owned[0] = 91;
    records[0].width = 987;
    check(decode(root.view(), {owned, 1}, {records, 16}, {index, measured.index_words})
                      .error == GifFavoritesError::capacity &&
              owned[0] == 91 && records[0].width == 987 &&
              decode(root.view(), {owned, sizeof(owned)}, {records, 1},
                                         {index, measured.index_words})
                      .error == GifFavoritesError::capacity &&
              owned[0] == 91 && records[0].width == 987,
          "GIF favorites: owned group and records remain atomic on either capacity failure");
    root.bytes[root.used++] = 0;
    check(
        decode(root.view(), {owned, sizeof(owned)}, {records, 16},
                                   {index, measured.index_words})
                    .error == GifFavoritesError::protobuf &&
            owned[0] == 91 && records[0].width == 987 &&
            gif_favorites_measure_base64(utf8_literal("AB==")).error == GifFavoritesError::base64,
        "GIF favorites: malformed unrelated protobuf and noncanonical base64 reject the complete snapshot");
    --root.used;
    const u8 invalid_utf8[]{18, 9, 10, 7, 10, 1, 255, 18, 2, 8, 1};
    check(decode({invalid_utf8, sizeof(invalid_utf8)}, {owned, sizeof(owned)},
                                     {records, 16}, {index, 4})
                      .error == GifFavoritesError::utf8 &&
              owned[0] == 91,
          "GIF favorites: invalid UTF8 map key cannot enter owned favorite records");
    check(gif_favorites_validate_patch_json(utf8_literal(
              "{\"settings\":\"EgA=\",\"required_data_version\":0}")) == GifFavoritesError::none &&
              gif_favorites_validate_patch_json(utf8_literal("{\"settings\":\"EgA=\"}")) ==
                  GifFavoritesError::envelope &&
              gif_favorites_validate_patch_json(utf8_literal(
                  "{\"settings\":\"CgA=\",\"required_data_version\":0}")) == GifFavoritesError::envelope &&
              gif_favorites_validate_patch_json(
                  utf8_literal("{\"settings\":\"EgA=\",\"required_data_version\":0,\"extra\":1}")) ==
                  GifFavoritesError::envelope &&
              gif_favorites_validate_patch_json(utf8_literal(
                  "{\"settings\":\"EgA=\",\"required_data_version\":0,\"required_data_version\":1}")) ==
                  GifFavoritesError::envelope,
          "GIF favorites: thin-service envelope excludes missing guard, arbitrary root writes and "
          "duplicate/unknown JSON keys");
    const u8 absent_group[]{10, 2, 24, 0}, empty_group[]{18, 0};
    check(measure({absent_group, sizeof(absent_group)}).ok() &&
              !measure({absent_group, sizeof(absent_group)}).group_present &&
              measure({empty_group, sizeof(empty_group)}).group_present &&
              measure({empty_group, sizeof(empty_group)}).entry_count == 0,
          "GIF favorites: absent lazy group stays distinct from an observed empty favorites group");
    u8 collision_group[512]{}, collision_root[1024]{};
    FixtureWire collisions{{collision_group, sizeof(collision_group)}},
        collision_top{{collision_root, sizeof(collision_root)}};
    constexpr auto collision_a = utf8_literal("https://example.test/collision-0");
    constexpr auto collision_b = utf8_literal("https://example.test/collision-4");
    constexpr auto collision_absent = utf8_literal("https://example.test/collision-8");
    favorite(collisions, collision_a, src_a, 4, 1, 1);
    favorite(collisions, collision_b, src_a, 4, 1, 2);
    collision_top.message(2, collisions.view());
    const auto collision_result = decode(
        collision_top.view(), {changed, sizeof(changed)}, {records, 16}, {index, 8});
    check(collision_result.ok() && collision_result.record_count == 2 &&
              utf8_equal(records[0].url, collision_b) &&
              gif_favorites_find({records, 2}, {index, 8}, collision_a) &&
              gif_favorites_find({records, 2}, {index, 8}, collision_b) &&
              !gif_favorites_find({records, 2}, {index, 8}, collision_absent) &&
              !gif_favorites_find({records, 2}, {index, 3}, collision_a),
          "GIF favorites: real colliding buckets resolve exact keys and saved-order ties without hash-only "
          "matches");
    changed[0] = 95;
    records[0].width = 987;
    const Utf8View collision_text = encode(collision_top.view());
    const char collision_first = collision_text.data[0];
    check(gif_favorites_decode_base64(collision_text, {changed, sizeof(changed)}, {records, 16}, {index, 1})
                      .error == GifFavoritesError::capacity &&
              changed[0] == 95 && records[0].width == 987 &&
              gif_favorites_decode_base64(collision_text, {reinterpret_cast<u8*>(g_encoded), sizeof(g_encoded)},
                                          {records, 16}, {index, 8})
                      .error == GifFavoritesError::input &&
              g_encoded[0] == collision_first,
          "GIF favorites: short index and aliased source bank reject before owned publication");
    u8 overflow_group[512]{};
    FixtureWire overflow{{overflow_group, sizeof(overflow_group)}};
    favorite(overflow, key_b, src_a, ~u32{0}, 1, 1);
    check(
        gif_favorites_write_patch(overflow.view(), 7, add, {scratch, sizeof(scratch)}, {json, sizeof(json)})
                .error == GifFavoritesError::order_overflow,
        "GIF favorites: max uint32 saved order cannot wrap and reorder cloud favorites unexpectedly");
    Arena arena{};
    check(arena_initialize(&arena, 4 * 1024 * 1024, 0),
          "GIF favorites: large protocol-cap fixture allocates only on use");
    if (!arena.base)
        return;
    auto* large_group = arena_allocate<u8>(&arena, kGifFavoritesMaximumGroupBytes);
    auto* large_proto = arena_allocate<u8>(&arena, kGifFavoritesMaximumGroupBytes + 16);
    auto* large_scratch = arena_allocate<u8>(&arena, kGifFavoritesMaximumGroupBytes + 512);
    auto* large_json = arena_allocate<u8>(&arena, kGifFavoritesMaximumPatchBytes);
    if (!large_group || !large_proto || !large_scratch || !large_json) {
        check(false, "GIF favorites: complete bounded cap fixture allocation succeeds");
        arena_release(&arena);
        return;
    }
    FixtureWire large{{large_group, kGifFavoritesMaximumGroupBytes}};
    large.number(29 * 8 + 2);
    large.number(kGifFavoritesMaximumGroupBytes - 5);
    for (u32 i = large.used; i < kGifFavoritesMaximumGroupBytes; ++i)
        large_group[i] = static_cast<u8>(i);
    large.used = kGifFavoritesMaximumGroupBytes;
    FixtureWire top{{large_proto, kGifFavoritesMaximumGroupBytes + 16}};
    top.message(2, large.view());
    const auto large_text =
        base64_encode(top.view(), {reinterpret_cast<char*>(large_json), kGifFavoritesMaximumPatchBytes});
    const auto large_measured =
        gif_favorites_measure_base64({reinterpret_cast<const char*>(large_json), large_text.bytes});
    check(large_text.ok() && large_measured.ok() && large_measured.group_bytes == kGifFavoritesMaximumGroupBytes,
          "GIF favorites: official raw group cap accepts complete opaque fields without counting base64 "
          "overhead");
    large_json[0] = 89;
    check(gif_favorites_write_patch(large.view(), 1, add,
                                    {large_scratch, kGifFavoritesMaximumGroupBytes + 512},
                                    {large_json, kGifFavoritesMaximumPatchBytes})
                      .error == GifFavoritesError::capacity &&
              large_json[0] == 89,
          "GIF favorites: add beyond official group cap rejects rather than truncating cloud favorites");
    large.used = 0;
    large.number(29 * 8 + 2);
    large.number(70'000);
    for (u32 i = 0; i < 70'000; ++i)
        large.bytes[large.used++] = static_cast<u8>(i);
    written = gif_favorites_write_patch(large.view(), ~u32{0}, add,
                                        {large_scratch, kGifFavoritesMaximumGroupBytes + 512},
                                        {large_json, kGifFavoritesMaximumPatchBytes});
    check(written.ok() && written.bytes > 64 * 1024 &&
              gif_favorites_validate_patch_json(
                  {reinterpret_cast<const char*>(large_json), written.bytes}) == GifFavoritesError::none,
          "GIF favorites: complete conditional patch above ordinary REST64KiB retains opaque cloud data");
    arena_release(&arena);
}
} // namespace maicord
