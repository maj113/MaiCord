#include "maicord/media/hevc.hpp"
#include "maicord/core/memory.hpp"
#include "../src/media/hevc_bits.hpp"
#include "../src/platform/hevc_gpu_geometry.hpp"
#include "../src/platform/hevc_gpu_references.hpp"

namespace maicord {
int hevc_reference_tests() noexcept;
namespace {
u32 reference_hevc_read(HevcBits& b, u32 count) noexcept {
    if (count > 32) {
        b.error = HevcError::invalid_input;
        return 0;
    }
    u32 result{};
    for (u32 i = 0; i < count && b.error == HevcError::none; ++i) {
        if (!b.remaining) {
            b.current = b.byte();
            b.remaining = 8;
        }
        result = (result << 1) | ((b.current >> --b.remaining) & 1u);
        ++b.position;
    }
    return result;
}
bool hevc_reader_state_regression() noexcept {
    u32 state = 0xA355F123u;
    const auto next = [&]() noexcept {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    u8 bytes[64];
    for (u32 sample = 0; sample < 8192; ++sample) {
        for (auto& value : bytes)
            value = static_cast<u8>(next());
        if (sample % 3 == 0) {
            bytes[0] = bytes[1] = 0;
            bytes[2] = static_cast<u8>(sample % 8);
        }
        HevcBits actual{}, reference{};
        actual.source = reference.source = {bytes, sample % 65};
        for (u32 operation = 0; operation < 12; ++operation) {
            const u32 count = next() % 35;
            if (actual.read(count) != reference_hevc_read(reference, count) ||
                actual.cursor != reference.cursor || actual.position != reference.position ||
                actual.current != reference.current || actual.remaining != reference.remaining ||
                actual.zeros != reference.zeros || actual.error != reference.error)
                return false;
        }
    }
    return true;
}
struct TestBits {
    u8 data[256]{};
    u32 bits{};
    void write(u32 value, u32 count) noexcept {
        for (u32 i = count; i; --i) {
            if ((value >> (i - 1)) & 1u)
                data[bits / 8] |= static_cast<u8>(1u << (7 - bits % 8));
            ++bits;
        }
    }
    void ue(u32 value) noexcept {
        u32 n = value + 1, width{};
        for (u32 v = n; v; v >>= 1)
            ++width;
        write(0, width - 1);
        write(n, width);
    }
    void se(i32 value) noexcept {
        ue(value <= 0 ? static_cast<u32>(-value) * 2 : static_cast<u32>(value) * 2 - 1);
    }
    void align() noexcept {
        write(1, 1);
        while (bits % 8)
            write(0, 1);
    }
};
u32 nal(u8* out, u8 type, const TestBits& b) noexcept {
    out[0] = static_cast<u8>(type << 1);
    out[1] = 1;
    u32 used = 2, zeros{};
    for (u32 i = 0; i < (b.bits + 7) / 8; ++i) {
        const u8 value = b.data[i];
        if (zeros >= 2 && value <= 3) {
            out[used++] = 3;
            zeros = 0;
        }
        out[used++] = value;
        zeros = value == 0 ? zeros + 1 : 0;
    }
    return used;
}
u32 unit(u8* out, u8 type, const TestBits& b) noexcept {
    const u32 count = nal(out + 4, type, b);
    out[0] = out[1] = 0;
    out[2] = static_cast<u8>(count >> 8);
    out[3] = static_cast<u8>(count);
    return count + 4;
}
u32 hex(u8* out, const char* text) noexcept {
    u32 used{};
    for (u32 i = 0; text[i] && text[i + 1]; i += 2) {
        u32 byte{};
        for (u32 j = 0; j < 2; ++j) {
            const char c = text[i + j];
            byte = (byte << 4) | static_cast<u32>(c <= '9' ? c - '0' : c - 'A' + 10);
        }
        out[used++] = static_cast<u8>(byte);
    }
    return used;
}
} // namespace
void test_hevc(void (*check)(bool, const char*) noexcept) noexcept {
    check(hevc_reader_state_regression(),
          "HEVC reader: byte chunks preserve exact values, cursors, bit positions, and error states");
    HevcReferenceState prior{};
    prior.count = 2;
    prior.pictures[0] = {49, 1, false};
    prior.pictures[1] = {50, 2, false};
    HevcReferencePlan retained{};
    retained.poc = 51;
    retained.after.count = 2;
    retained.after.pictures[0] = prior.pictures[1];
    retained.after.pictures[1] = {51, 0, false};
    retained.before_count = 1;
    retained.before[0] = 1;
    HevcGpuReferences native_refs{};
    check(hevc_gpu_reference_map(prior, retained, 0, &native_refs) && native_refs.count == 1 &&
              native_refs.pictures[0].poc == 50 && native_refs.pictures[0].surface == 2 &&
              native_refs.before[0] == 0 && native_refs.before[1] == 255,
          "HEVC DXVA references: actual sample51 drops prior POC49 and remaps current POC50 index1 to0");
    prior.count = retained.after.count = 3;
    prior.pictures[2] = retained.after.pictures[2] = {70, 3, true};
    check(hevc_gpu_reference_map(prior, retained, 0, &native_refs) && native_refs.count == 2 &&
              native_refs.pictures[1].poc == 70 && native_refs.pictures[1].long_term &&
              native_refs.long_count == 0,
          "HEVC DXVA references: future unused RPS references retain their surface and long-term marking");
    const auto unchanged_refs = native_refs;
    auto invalid_refs = retained;
    invalid_refs.before[0] = 0;
    bool reference_rejection = !hevc_gpu_reference_map(prior, invalid_refs, 0, &native_refs) &&
                               bytes_equal(&native_refs, &unchanged_refs, sizeof(native_refs));
    invalid_refs = retained;
    invalid_refs.before_count = 2;
    invalid_refs.before[1] = 1;
    reference_rejection = reference_rejection &&
                          !hevc_gpu_reference_map(prior, invalid_refs, 0, &native_refs) &&
                          bytes_equal(&native_refs, &unchanged_refs, sizeof(native_refs));
    invalid_refs = retained;
    invalid_refs.after.pictures[2] = {71, 3, true};
    reference_rejection = reference_rejection &&
                          !hevc_gpu_reference_map(prior, invalid_refs, 0, &native_refs) &&
                          bytes_equal(&native_refs, &unchanged_refs, sizeof(native_refs));
    auto duplicate_prior = prior;
    duplicate_prior.pictures[0] = duplicate_prior.pictures[1];
    reference_rejection = reference_rejection &&
                          !hevc_gpu_reference_map(duplicate_prior, retained, 0, &native_refs) &&
                          bytes_equal(&native_refs, &unchanged_refs, sizeof(native_refs));
    check(reference_rejection, "HEVC DXVA references: removed, duplicate, unmatched, and invalid source "
                               "authorities reject atomically");
    prior.count = retained.after.count = 4;
    prior.pictures[2] = {52, 3, false};
    prior.pictures[3] = {70, 4, true};
    retained.after.pictures[0] = prior.pictures[1];
    retained.after.pictures[1] = prior.pictures[2];
    retained.after.pictures[2] = prior.pictures[3];
    retained.after.pictures[3] = {51, 0, false};
    retained.following_count = retained.long_count = 1;
    retained.following[0] = 2;
    retained.long_term[0] = 3;
    check(hevc_gpu_reference_map(prior, retained, 0, &native_refs) && native_refs.count == 3 &&
              native_refs.before[0] == 0 && native_refs.following[0] == 1 &&
              native_refs.long_term[0] == 2 && native_refs.following[1] == 255 &&
              native_refs.long_term[1] == 255,
          "HEVC DXVA references: negative, positive, and long-term current lists share one compact DPB "
          "authority");
    u8 raster4[16]{}, raster8[64]{}, diagonal4[16]{}, diagonal8[64]{};
    for (u32 i = 0; i < 16; ++i)
        raster4[i] = static_cast<u8>(i);
    for (u32 i = 0; i < 64; ++i)
        raster8[i] = static_cast<u8>(i + 64);
    hevc_gpu_scaling_diagonal<4>(raster4, diagonal4);
    hevc_gpu_scaling_diagonal<8>(raster8, diagonal8);
    constexpr u8 expected4[]{0, 4, 1, 8, 5, 2, 12, 9, 6, 3, 13, 10, 7, 14, 11, 15};
    constexpr u8 expected8[]{0,  8,  1,  16, 9,  2,  24, 17, 10, 3,  32, 25, 18, 11, 4,  40,
                             33, 26, 19, 12, 5,  48, 41, 34, 27, 20, 13, 6,  56, 49, 42, 35,
                             28, 21, 14, 7,  57, 50, 43, 36, 29, 22, 15, 58, 51, 44, 37, 30,
                             23, 59, 52, 45, 38, 31, 60, 53, 46, 39, 61, 54, 47, 62, 55, 63};
    bool diagonal_order = bytes_equal(diagonal4, expected4, sizeof(expected4));
    for (u32 i = 0; i < 64; ++i)
        diagonal_order = diagonal_order && diagonal8[i] == expected8[i] + 64;
    check(diagonal_order && raster4[4] == 4 && raster8[8] == 72,
          "HEVC DXVA scaling: asymmetric raster coefficients become diagonal syntax sequences without "
          "mutating authority");
    const auto recording_surface = hevc_gpu_surface_extent(2560, 1352);
    const auto boundary_surface = hevc_gpu_surface_extent(2048, 2048);
    const auto wide_surface = hevc_gpu_surface_extent(4096, 1024);
    check(recording_surface.width == 2560 && recording_surface.height == 1408 &&
              boundary_surface.width == 2048 && boundary_surface.height == 2048 &&
              wide_surface.width == 4096 && wide_surface.height == 1024,
          "HEVC DXVA surface: original cropped recording and aligned boundary retain 64-pixel allocation");
    check(!hevc_gpu_surface_extent(0, 1352).width && !hevc_gpu_surface_extent(4097, 1).width &&
              !hevc_gpu_surface_extent(2048, 2049).width && !hevc_gpu_surface_extent(4096, 1025).width &&
              !hevc_gpu_surface_extent(4000, 1048).width,
          "HEVC DXVA surface: invalid logical and padded pixel budgets are rejected before GPU allocation");
    check(hevc_gpu_bitstream_extent(127) == 128 && hevc_gpu_bitstream_extent(128) == 256 &&
              hevc_gpu_bitstream_extent(129) == 256 &&
              hevc_gpu_bitstream_extent(0xffffffffu) == 0x100000000ull,
          "HEVC DXVA bitstream: positive zero tail and checked 64-bit capacity survive aligned and maximum "
          "input");
    TestBits unspecified{};
    unspecified.write(0, 4);
    unspecified.write(0, 3);
    unspecified.write(1, 1);
    unspecified.write(0, 2);
    unspecified.write(0, 1);
    unspecified.write(1, 5);
    unspecified.write(0, 32);
    unspecified.write(0, 32);
    unspecified.write(0, 16);
    unspecified.write(120, 8);
    unspecified.ue(0);
    unspecified.ue(1);
    unspecified.ue(64);
    unspecified.ue(64);
    unspecified.write(0, 1);
    unspecified.ue(0);
    unspecified.ue(0);
    unspecified.ue(4);
    unspecified.write(0, 1);
    unspecified.ue(3);
    unspecified.ue(0);
    unspecified.ue(0);
    unspecified.ue(0);
    unspecified.ue(3);
    unspecified.ue(0);
    unspecified.ue(3);
    unspecified.ue(0);
    unspecified.ue(0);
    unspecified.write(0, 4);
    unspecified.ue(0);
    unspecified.write(0, 5);
    unspecified.align();
    u8 unspecified_nal[512]{};
    const u32 unspecified_bytes = nal(unspecified_nal, 33, unspecified);
    HevcSps unspecified_sps{};
    check(hevc_parse_sps({unspecified_nal, unspecified_bytes}, &unspecified_sps) == HevcError::none &&
              unspecified_sps.matrix == 2 && unspecified_sps.primaries == 2 &&
              unspecified_sps.transfer == 2 && !unspecified_sps.full_range,
          "HEVC colour authority: absent VUI retains normative unspecified defaults compatible with native "
          "NV12 admission");
    // Original supplied recording metadata only; no image, audio, or coded picture is copied.
    constexpr char hvcc_hex[] =
        "010160000000B000000000009CF000FCFDF8F800000F03A00001001740010C01FFFF016000000300B0000003000003009C"
        "2C09A100010034420101016000000300B0000003000003009CA00140200551F372E490A564B8FFC00040005A8080810200"
        "00030002000003003D88A2000100074401E0F7C04C90";
    u8 config_bytes[256]{};
    const u32 config_count = hex(config_bytes, hvcc_hex);
    HevcConfiguration original{};
    const auto parsed = hevc_select_parameters({config_bytes, config_count}, 0, &original);
    check(
        parsed == HevcError::none && original.nal_length_bytes == 4 && original.sps.width == 2560 &&
            original.sps.height == 1350 && original.sps.coded_width <= 4096 &&
            u64{original.sps.coded_width} * original.sps.coded_height <= 2048u * 2048u &&
            original.sps.max_buffering <= 15,
        "HEVC parameters: actual Main8 hvcC resolves coded crop and bounded DPB without a native resource");
    HevcConfiguration sentinel{};
    sentinel.sps.width = 77;
    const auto before = sentinel;
    bool transactional = true;
    for (u32 count = 0; count < config_count; ++count) {
        const auto e = hevc_select_parameters({config_bytes, count}, 0, &sentinel);
        transactional =
            transactional && e != HevcError::none && bytes_equal(&sentinel, &before, sizeof(before));
    }
    check(transactional, "HEVC parameters: every truncated hvcC prefix preserves the prior configuration");
    config_bytes[17] = 0xf9;
    check(hevc_select_parameters({config_bytes, config_count}, 0, &sentinel) == HevcError::unsupported &&
              bytes_equal(&sentinel, &before, sizeof(before)),
          "HEVC parameters: Main10 bit depth is rejected transactionally instead of approximated as NV12");
    config_bytes[17] = 0xf8;
    check(hevc_select_parameters({config_bytes, config_count}, 63, &sentinel) != HevcError::none &&
              bytes_equal(&sentinel, &before, sizeof(before)),
          "HEVC parameters: absent PPS authority cannot silently select another parameter chain");
    // HEVC with alpha (Apple): the alpha layer's SPS and PPS (nuh_layer_id 1,
    // the PPS reusing id 0) sit in the same hvcC; the base-layer chain is
    // unchanged and the alpha layer is ignored.
    constexpr u8 alpha_arrays[]{0xA1, 0x00, 0x01, 0x00, 0x05, 0x42, 0x09, 0x01, 0x02, 0x03,
                                0xA2, 0x00, 0x01, 0x00, 0x04, 0x44, 0x09, 0x80, 0x00};
    u8 layered_config[256]{};
    copy_bytes(layered_config, config_bytes, config_count);
    copy_bytes(layered_config + config_count, alpha_arrays, sizeof(alpha_arrays));
    layered_config[22] = static_cast<u8>(config_bytes[22] + 2);
    HevcConfiguration layered{};
    check(hevc_select_parameters({layered_config, config_count + u32{sizeof(alpha_arrays)}}, 0, &layered) ==
                  HevcError::none &&
              bytes_equal(&layered, &original, sizeof(layered)),
          "HEVC parameters: an alpha layer's parameter sets in hvcC leave the base-layer chain intact");
    // Its VPS: two layers, a layer set naming both, and a vps_extension.
    const auto layered_vps = [](bool base_internal) noexcept {
        TestBits v{};
        v.write(0, 4);
        v.write(base_internal, 1);
        v.write(1, 1);
        v.write(1, 6);
        v.write(0, 3);
        v.write(1, 1);
        v.write(0xffff, 16);
        v.write(0, 2);
        v.write(0, 1);
        v.write(1, 5);
        v.write(0x60000000u, 32);
        v.write(0, 32);
        v.write(0, 16);
        v.write(93, 8);
        v.write(1, 1);
        v.ue(4);
        v.ue(2);
        v.ue(0);
        v.write(1, 6);
        v.ue(1);
        v.write(3, 2);
        v.write(0, 1);
        v.write(1, 1);
        v.write(5, 3);
        v.align();
        return v;
    };
    u8 vps_nal[256]{};
    u32 vps_bytes = nal(vps_nal, 32, layered_vps(true));
    const bool layered_vps_valid = hevc_validate_parameter({vps_nal, vps_bytes}, original) == HevcError::none;
    vps_bytes = nal(vps_nal, 32, layered_vps(false));
    check(layered_vps_valid &&
              hevc_validate_parameter({vps_nal, vps_bytes}, original) == HevcError::unsupported,
          "HEVC parameters: a multi-layer VPS with an internal base layer is accepted; an external base "
          "layer is not");
    HevcBits rps_bits{};
    TestBits explicit_rps{};
    explicit_rps.ue(2);
    explicit_rps.ue(1);
    explicit_rps.ue(0);
    explicit_rps.write(1, 1);
    explicit_rps.ue(1);
    explicit_rps.write(0, 1);
    explicit_rps.ue(1);
    explicit_rps.write(1, 1);
    rps_bits.source = {explicit_rps.data, (explicit_rps.bits + 7) / 8};
    HevcReferenceSet references[2]{};
    check(hevc_parse_reference_set(rps_bits, references, 0, 1, &references[0]) == HevcError::none &&
              references[0].negative == 2 && references[0].positive == 1 && references[0].delta[0] == -1 &&
              references[0].delta[1] == -3 && references[0].delta[2] == 2 && references[0].used == 5,
          "HEVC reference syntax: explicit RPS retains used and unused negative and positive pictures");
    TestBits predicted{};
    predicted.write(1, 1);
    predicted.write(0, 1);
    predicted.ue(0);
    predicted.write(1, 1);
    predicted.write(0, 1);
    predicted.write(1, 1);
    predicted.write(1, 1);
    predicted.write(1, 1);
    rps_bits = {};
    rps_bits.source = {predicted.data, (predicted.bits + 7) / 8};
    u8 delta_count{};
    // Prediction excludes delta zero, preserves unused references, and orders
    // negatives nearest-first followed by increasing positive deltas.
    HevcReferenceSet predicted_set{};
    check(hevc_parse_reference_set(rps_bits, references, 1, 2, &predicted_set, &delta_count) ==
                  HevcError::none &&
              predicted_set.negative == 1 && predicted_set.positive == 2 && predicted_set.delta[0] == -2 &&
              predicted_set.delta[1] == 1 && predicted_set.delta[2] == 3 && predicted_set.used == 6 &&
              delta_count == 3,
          "HEVC reference syntax: predicted current-picture zero is omitted without losing ordered used "
          "and unused references");
    HevcReferenceSet unchanged{};
    unchanged.delta[0] = 9;
    const auto prior_rps = unchanged;
    TestBits excessive{};
    excessive.ue(16);
    excessive.ue(0);
    rps_bits = {};
    rps_bits.source = {excessive.data, (excessive.bits + 7) / 8};
    check(hevc_parse_reference_set(rps_bits, references, 0, 1, &unchanged) == HevcError::malformed &&
              bytes_equal(&unchanged, &prior_rps, sizeof(unchanged)),
          "HEVC reference syntax: out-of-bound reference syntax cannot replace prior authority");
    HevcConfiguration simple{};
    simple.nal_length_bytes = 4;
    simple.sps.coded_width = simple.sps.width = 64;
    simple.sps.coded_height = simple.sps.height = 64;
    simple.sps.max_buffering = 4;
    simple.sps.poc_bits = 8;
    simple.sps.log2_diff_cb = 3;
    simple.pps.references_l0 = simple.pps.references_l1 = 1;
    TestBits intra{};
    intra.write(1, 1);
    intra.write(0, 1);
    intra.ue(0);
    intra.ue(2);
    intra.se(0);
    intra.align();
    intra.write(0x80, 8);
    u8 bytes[512]{};
    const u32 size = unit(bytes, 20, intra);
    HevcPicture picture{};
    u8 pps = 99;
    check(hevc_picture_parameter_id({bytes, size}, 4, &pps) == HevcError::none && pps == 0 &&
              hevc_parse_picture({bytes, size}, simple, &picture) == HevcError::none && picture.idr &&
              picture.irap && picture.reference && picture.slice_type == 2 && picture.slice_count == 1 &&
              picture.slices[0].offset == 4 && picture.bitstream_bytes == size - 1,
          "HEVC picture: original IDR header preserves exact NAL span and bounded AnnexB size");
    const auto picture_before = picture;
    bytes[5] = 0;
    check(hevc_parse_picture({bytes, size}, simple, &picture) == HevcError::malformed &&
              bytes_equal(&picture, &picture_before, sizeof(picture)),
          "HEVC picture: invalid temporal header cannot replace a previously validated picture");
    bytes[5] = 1;
    // The alpha layer's slice in the same access unit (nuh_layer_id 1) is
    // skipped: one base-layer slice and the same AnnexB span.
    const u32 alpha_slice = unit(bytes + size, 20, intra);
    bytes[size + 5] = 0x09;
    u8 alpha_pps = 99;
    check(hevc_picture_parameter_id({bytes, size + alpha_slice}, 4, &alpha_pps) == HevcError::none &&
              alpha_pps == 0 &&
              hevc_parse_picture({bytes, size + alpha_slice}, simple, &picture) == HevcError::none &&
              picture.slice_count == 1 && picture.slices[0].offset == 4 && picture.bitstream_bytes == size - 1,
          "HEVC picture: an alpha-layer slice is ignored, leaving the base-layer picture exact");
    simple.sps.coded_width = 128;
    const u32 doubled = unit(bytes + size, 20, intra);
    check(hevc_parse_picture({bytes, size + doubled}, simple, &picture) == HevcError::malformed &&
              bytes_equal(&picture, &picture_before, sizeof(picture)),
          "HEVC picture: a second first-slice header in one access unit is rejected atomically");
    check(hevc_reference_tests() == 0,
          "HEVC native references: bounded POC and DPB policy regressions pass");
}
} // namespace maicord
