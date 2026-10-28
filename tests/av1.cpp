// AV1 header parsing and DXVA parameter mapping on the synthetic SVT-AV1
// fixture (tests/data/av1_stream_vectors.hpp), plus truncation and mutation
// fuzzing that every parse stays inside its OBU.
#include "maicord/media/av1.hpp"
#include "maicord/core/memory.hpp"
#include "av1_dxva_parameters.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
#include "data/av1_stream_vectors.hpp"

u32 failures = 0;
void check(bool ok, const char* what) noexcept {
    test_check(&failures, ok, what);
}

u8 unit_storage[kAv1FixtureFrames][2048];
u32 unit_bytes[kAv1FixtureFrames];

struct Walk {
    Av1SequenceHeader sequence;
    Av1References references;
    Av1FrameHeader header;
    Av1Tile tiles[64];
    u32 frames, shown, existing, tiles_seen, hidden;
    bool have_sequence;
};

// The decoder's OBU walk without the GPU: false at the first parse failure.
// Tile ranges must stay inside the unit.
bool walk_unit(Walk* w, ConstSlice<u8> unit) noexcept {
    u32 offset = 0, tile_count = 0;
    bool open = false;
    while (offset < unit.count) {
        Av1Obu obu{};
        if (av1_next_obu(unit, &offset, &obu) != Av1Status::ok)
            return false;
        if (obu.payload.data < unit.data || obu.payload.count > unit.count ||
            obu.payload.data + obu.payload.count > unit.data + unit.count)
            return false;
        if (obu.type == kAv1ObuSequenceHeader) {
            if (av1_parse_sequence_header(obu.payload, &w->sequence) != Av1Status::ok)
                return false;
            w->have_sequence = true;
            continue;
        }
        ConstSlice<u8> group = obu.payload;
        if (obu.type == kAv1ObuFrameHeader || obu.type == kAv1ObuFrame) {
            if (!w->have_sequence || open ||
                av1_parse_frame_header(w->sequence, w->references, obu.payload, obu.temporal_id,
                                       obu.spatial_id, &w->header) != Av1Status::ok)
                return false;
            if (w->header.header_bytes > obu.payload.count)
                return false;
            if (w->header.show_existing_frame) {
                av1_update_references(w->references, w->header);
                ++w->existing;
                ++w->shown;
                continue;
            }
            open = true;
            tile_count = 0;
            if (obu.type == kAv1ObuFrameHeader)
                continue;
            group = {obu.payload.data + w->header.header_bytes, obu.payload.count - w->header.header_bytes};
        } else if (obu.type != kAv1ObuTileGroup) {
            continue;
        } else if (!open) {
            return false;
        }
        u32 start = 0, end = 0;
        if (av1_parse_tile_group(w->header, group, {w->tiles, 64}, &start, &end) != Av1Status::ok ||
            start != tile_count)
            return false;
        for (u32 i = 0; i <= end - start; ++i)
            if (!w->tiles[i].size || w->tiles[i].offset > group.count ||
                w->tiles[i].size > group.count - w->tiles[i].offset)
                return false;
        w->tiles_seen += end - start + 1;
        tile_count = end + 1;
        if (tile_count == u32{w->header.tile_columns} * w->header.tile_rows) {
            av1_update_references(w->references, w->header);
            open = false;
            ++w->frames;
            w->shown += w->header.show_frame;
            w->hidden += !w->header.show_frame;
        }
    }
    return !open;
}

void fixture_tests() noexcept {
    for (u32 i = 0; i < kAv1FixtureFrames; ++i)
        unit_bytes[i] = test_unhex(av1_fixture_units[i], unit_storage[i]);
    // Key unit: a sized sequence header, then a frame OBU without obu_size.
    u32 offset = 0;
    Av1Obu sequence_obu{}, frame_obu{};
    const ConstSlice<u8> key{unit_storage[0], unit_bytes[0]};
    check(av1_next_obu(key, &offset, &sequence_obu) == Av1Status::ok && sequence_obu.type == 1 &&
              av1_next_obu(key, &offset, &frame_obu) == Av1Status::ok && frame_obu.type == 6 &&
              offset == key.count &&
              frame_obu.payload.data + frame_obu.payload.count == key.data + key.count,
          "AV1 OBU walk: the last OBU without obu_size runs to the end of the unit");
    Av1SequenceHeader sequence{};
    check(av1_parse_sequence_header(sequence_obu.payload, &sequence) == Av1Status::ok &&
              sequence.profile == 0 && sequence.bit_depth == 8 && sequence.max_width == kAv1FixtureWidth &&
              sequence.max_height == kAv1FixtureHeight && sequence.subsampling_x &&
              sequence.subsampling_y && !sequence.mono_chrome && !sequence.film_grain_params_present &&
              sequence.enable_order_hint && sequence.order_hint_bits &&
              !sequence.reduced_still_picture_header,
          "AV1 sequence header: profile 0, 8-bit 4:2:0 at the fixture's size");
    Av1References references{};
    Av1FrameHeader header{};
    check(
        av1_parse_frame_header(sequence, references, frame_obu.payload, 0, 0, &header) == Av1Status::ok &&
            header.frame_type == av1_key_frame && header.show_frame && header.refresh_frame_flags == 0xff &&
            header.upscaled_width == kAv1FixtureWidth && header.frame_height == kAv1FixtureHeight &&
            header.tile_columns == 2 && header.tile_rows == 1 && header.tile_widths[0] == 2 &&
            header.tile_widths[1] == 2 && header.tile_heights[0] == 1 && header.tile_size_bytes &&
            header.coded_lossless && !header.base_q_idx && !header.tx_mode && header.qm_y == 0xff &&
            header.primary_ref_frame == kAv1PrimaryRefNone && header.header_bytes < frame_obu.payload.count,
        "AV1 key frame header: lossless, two tile columns, all slots refreshed");
    Av1Tile tiles[4];
    u32 start = 9, end = 9;
    const ConstSlice<u8> group{frame_obu.payload.data + header.header_bytes,
                               frame_obu.payload.count - header.header_bytes};
    check(
        av1_parse_tile_group(header, group, {tiles, 4}, &start, &end) == Av1Status::ok && !start &&
            end == 1 && tiles[0].offset == 1u + header.tile_size_bytes &&
            tiles[1].offset == tiles[0].offset + tiles[0].size &&
            tiles[1].offset + tiles[1].size == group.count && tiles[1].column == 1 && !tiles[1].row,
        "AV1 tile group: after the start/end flag byte, sized tiles back to back; the last takes the rest");
    check(av1_parse_tile_group(header, group, {tiles, 1}, &start, &end) == Av1Status::unsupported,
          "AV1 tile group: more tiles than the caller holds is unsupported");

    u8 slots[kAv1RefFrames];
    for (auto& slot : slots)
        slot = 255;
    DXVA_PicParams_AV1 parameters;
    av1_dxva_picture_parameters(sequence, header, slots, references, 3, &parameters);
    bool refs_empty = true;
    for (const auto& entry : parameters.frame_refs)
        refs_empty = refs_empty && entry.Index == 0xff;
    check(sizeof(parameters) == 912 && sizeof(DXVA_Tile_AV1) == 16 &&
              parameters.width == kAv1FixtureWidth && parameters.height == kAv1FixtureHeight &&
              parameters.CurrPicTextureIndex == 3 && parameters.tiles.cols == 2 &&
              parameters.tiles.widths[1] == 2 && parameters.superres_denom == 8 &&
              parameters.bitdepth == 8 && !parameters.format.frame_type && parameters.format.show_frame &&
              parameters.format.subsampling_x && parameters.coding.reference_frame_update &&
              !parameters.coding.tx_mode && parameters.quantization.qm_v == 0xff && refs_empty &&
              parameters.RefFrameMapTextureIndex[7] == 0xff && parameters.primary_ref_frame == 7 &&
              !parameters.film_grain.apply_grain && !parameters.StatusReportFeedbackNumber,
          "AV1 DXVA key frame parameters: sizes, tiles, no references");

    // The whole fixture: 4 decoded shown frames, 2 hidden, 2 shown existing.
    static Walk walk;
    walk = {};
    bool parsed = true;
    for (u32 i = 0; i < kAv1FixtureFrames && parsed; ++i) {
        parsed = walk_unit(&walk, {unit_storage[i], unit_bytes[i]});
        if (i == 1 && parsed) {
            // The shown inter frame: every reference names a decoded slot.
            for (u32 r = 0; r < kAv1RefsPerFrame; ++r)
                slots[r] = static_cast<u8>(r);
            slots[7] = 7;
            av1_dxva_picture_parameters(walk.sequence, walk.header, slots, walk.references, 2, &parameters);
            bool refs = true;
            for (u32 r = 0; r < kAv1RefsPerFrame; ++r)
                refs = refs && parameters.frame_refs[r].Index < 8 &&
                       parameters.frame_refs[r].width == kAv1FixtureWidth &&
                       parameters.frame_refs[r].height == kAv1FixtureHeight &&
                       (parameters.frame_refs[r].wmtype || !parameters.frame_refs[r].wminvalid);
            check(refs && parameters.format.frame_type == av1_inter_frame && parameters.order_hint &&
                      parameters.primary_ref_frame != 7,
                  "AV1 DXVA inter frame parameters: slot references with their sizes");
        }
    }
    check(parsed && walk.frames == 6 && walk.hidden == 2 && walk.existing == 2 && walk.shown == 6 &&
              walk.tiles_seen == 12,
          "AV1 fixture walk: hidden frames, shown existing frames and all tiles");
}

void unit_tests() noexcept {
    // Shear validity (spec 7.11.3.6).
    const i32 identity[6]{0, 0, 1 << 16, 0, 0, 1 << 16};
    const i32 sheared[6]{0, 0, 1 << 16, 1 << 14, 0, 1 << 16};
    const i32 degenerate[6]{0, 0, 0, 0, 0, 1 << 16};
    const i32 rotation[6]{0, 0, (1 << 16) - 300, 200, -200, (1 << 16) - 300};
    check(av1_warp_valid(identity) && !av1_warp_valid(sheared) && !av1_warp_valid(degenerate) &&
              av1_warp_valid(rotation),
          "AV1 warp validity follows setup_shear");
    // OBU framing errors.
    const u8 forbidden[]{0x80, 0x00}, short_size[]{0x0a, 0x05, 0x00}, endless_leb[]{0x0a, 0x80, 0x80},
        extension_only[]{0x0c};
    u32 offset = 0;
    Av1Obu obu{};
    check(av1_next_obu({forbidden, 2}, &offset, &obu) == Av1Status::malformed &&
              av1_next_obu({short_size, 3}, &(offset = 0), &obu) == Av1Status::truncated &&
              av1_next_obu({endless_leb, 3}, &(offset = 0), &obu) == Av1Status::truncated &&
              av1_next_obu({extension_only, 1}, &(offset = 0), &obu) == Av1Status::truncated,
          "AV1 OBU framing rejects the forbidden bit and sizes past the data");
    // A sequence header that is not 8-bit profile 0 still parses; the decoder refuses it.
    const u8 high[]{0x40}; // seq_profile 2: truncated right after.
    Av1SequenceHeader sequence{};
    check(av1_parse_sequence_header({high, 1}, &sequence) == Av1Status::truncated &&
              av1_parse_sequence_header({}, &sequence) == Av1Status::truncated,
          "AV1 sequence header reports truncation");
    // An inter frame naming an empty slot is malformed.
    Av1References empty{};
    Av1FrameHeader header{};
    const u8 inter[]{0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    offset = 0;
    check(av1_next_obu({unit_storage[0], unit_bytes[0]}, &offset, &obu) == Av1Status::ok &&
              av1_parse_sequence_header(obu.payload, &sequence) == Av1Status::ok &&
              av1_parse_frame_header(sequence, empty, {inter, sizeof(inter)}, 0, 0, &header) ==
                  Av1Status::malformed &&
              !header.frame_width,
          "AV1 inter frame without valid references is rejected and leaves the header clear");
}

// Truncations and seeded byte mutations: every outcome is a status, and a
// successful walk keeps all tiles inside the unit.
void fuzz_tests() noexcept {
    static Walk walk;
    static u8 mutated[2048];
    u32 survived = 0, rejected = 0;
    for (u32 unit = 0; unit < kAv1FixtureFrames; ++unit) {
        for (u32 length = 0; length <= unit_bytes[unit]; ++length) {
            walk = {};
            // The key unit primes the sequence and references for later units.
            if (unit && !walk_unit(&walk, {unit_storage[0], unit_bytes[0]}))
                continue;
            for (u32 previous = 1; previous < unit; ++previous)
                walk_unit(&walk, {unit_storage[previous], unit_bytes[previous]});
            walk_unit(&walk, {unit_storage[unit], length}) ? ++survived : ++rejected;
        }
    }
    u32 state = 0x2545f491u;
    for (u32 round = 0; round < 3000; ++round) {
        const u32 unit = round % kAv1FixtureFrames;
        copy_bytes(mutated, unit_storage[unit], unit_bytes[unit]);
        for (u32 flips = 0; flips < 1 + round % 4; ++flips) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            // Headers live in the first bytes; bias mutations there.
            const u32 span = round & 1 ? minimum<u32>(unit_bytes[unit], 48) : unit_bytes[unit];
            mutated[state % span] ^= static_cast<u8>(1u << (state >> 29));
        }
        walk = {};
        if (unit)
            walk_unit(&walk, {unit_storage[0], unit_bytes[0]});
        walk_unit(&walk, {mutated, unit_bytes[unit]}) ? ++survived : ++rejected;
    }
    check(survived && rejected, "AV1 fuzzing: truncated and mutated units parse or fail cleanly");
}
} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_av1_tests_entry() noexcept {
    maicord::fixture_tests();
    maicord::unit_tests();
    maicord::fuzz_tests();
    maicord::test_exit(maicord::failures, "av1");
}
