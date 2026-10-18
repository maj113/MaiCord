#include "maicord/media/h264_parameters.hpp"
#include "maicord/media/h264_slice.hpp"
#include "maicord/core/memory.hpp"
#include "../src/media/h264_bits.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
int h264_state_tests() noexcept;
int h264_stream_tests() noexcept;
namespace {

struct BitsWriter {
    u8 bytes[4'096]{};
    u32 count{};
    bool ok{true};
    void put(u32 value, u32 bits) noexcept {
        if (bits > 32 || count + bits > sizeof(bytes) * 8u) { ok = false; return; }
        for (u32 i = bits; i; --i, ++count)
            bytes[count / 8u] |= static_cast<u8>(((value >> (i - 1u)) & 1u) << (7u - count % 8u));
    }
    void ue(u64 value) noexcept {
        const u64 code = value + 1u;
        u32 length = 0;
        for (u64 n = code; n > 1; n >>= 1u) ++length;
        put(0, length); put(1, 1); put(static_cast<u32>(code), length);
    }
    void se(i32 value) noexcept {
        ue(value <= 0 ? static_cast<u64>(-static_cast<i64>(value) * 2)
                      : static_cast<u64>(value) * 2u - 1u);
    }
    void end() noexcept { put(1, 1); if (count % 8u) put(0, 8u - count % 8u); }
};
struct NalWriter {
    u8 bytes[8'192]{};
    u32 count{};
    void encode(const BitsWriter& bits, u8 header) noexcept {
        count = 0;
        if (!bits.ok || bits.count % 8u) return;
        bytes[count++] = header;
        u32 zeroes = 0;
        for (u32 i = 0; i < bits.count / 8u; ++i) {
            const u8 byte = bits.bytes[i];
            if (zeroes == 2 && byte <= 3) { bytes[count++] = 3; zeroes = 0; }
            bytes[count++] = byte;
            zeroes = byte ? 0 : zeroes + 1u;
        }
    }
    ConstSlice<u8> view() const noexcept { return {bytes, count}; }
};

void make_sps(NalWriter* nal, bool high, bool matrix, bool custom,
              u32 poc, bool fields, bool vui, u32 reorder = 2, u32 buffering = 4) noexcept {
    BitsWriter b;
    b.put(high ? 100 : 66, 8); b.put(0, 8); b.put(31, 8); b.ue(3);
    if (high) {
        b.ue(1); b.ue(0); b.ue(0); b.put(0, 1); b.put(matrix, 1);
        if (matrix) for (u32 i = 0; i < 8; ++i) {
            b.put(custom && i == 0, 1);
            if (custom && i == 0) { b.se(56); b.se(-64); }
        }
    }
    b.ue(0); b.ue(poc);
    if (poc == 0) b.ue(2);
    if (poc == 1) { b.put(0, 1); b.se(-2); b.se(1); b.ue(3); b.se(-1); b.se(2); b.se(-3); }
    b.ue(4); b.put(0, 1); b.ue(4); b.ue(3);
    b.put(!fields, 1); if (fields) b.put(1, 1); b.put(1, 1);
    b.put(1, 1); b.ue(1); b.ue(2); b.ue(1); b.ue(3);
    b.put(vui, 1);
    if (vui) {
        b.put(1, 1); b.put(255, 8); b.put(297, 16); b.put(296, 16);
        b.put(0, 1); // no overscan
        b.put(1, 1); b.put(5, 3); b.put(1, 1); b.put(1, 1);
        b.put(1, 8); b.put(1, 8); b.put(1, 8);
        b.put(0, 1); b.put(1, 1); b.put(1'001, 32); b.put(60'000, 32); b.put(1, 1);
        b.put(0, 1); b.put(0, 1); b.put(0, 1); // no HRD/pic_struct
        b.put(1, 1); b.put(1, 1); b.ue(2); b.ue(1); b.ue(16); b.ue(16);
        b.ue(reorder); b.ue(buffering);
    }
    b.end(); nal->encode(b, 0x67);
}

void make_pps(NalWriter* nal, bool extension, bool matrix, u32 groups = 0) noexcept {
    BitsWriter b;
    b.ue(7); b.ue(3); b.put(1, 1); b.put(0, 1); b.ue(groups);
    b.ue(3); b.ue(1); b.put(1, 1); b.put(2, 2);
    b.se(-3); b.se(0); b.se(-2); b.put(1, 1); b.put(0, 1); b.put(0, 1);
    if (extension) { b.put(1, 1); b.put(matrix, 1); if (matrix) b.put(0, 8); b.se(3); }
    b.end(); nal->encode(b, 0x68);
}

void make_slice(NalWriter* nal, const H264Sps& sps, const H264Pps& pps, H264Slice slice,
                bool extra_marking = false) noexcept {
    BitsWriter b;
    b.ue(slice.first_mb); b.ue(static_cast<u32>(slice.type) + (slice.all_same_type ? 5u : 0u)); b.ue(pps.id);
    b.put(slice.frame_num, sps.frame_num_bits);
    if (!sps.frame_only) b.put(0, 1);
    if (slice.idr) b.ue(slice.idr_id);
    if (sps.poc_type == 0) { b.put(slice.poc_lsb, sps.poc_bits); if (pps.bottom_poc_present) b.se(slice.delta_bottom); }
    if (sps.poc_type == 1 && !sps.delta_always_zero) {
        b.se(slice.delta_poc0); if (pps.bottom_poc_present) b.se(slice.delta_poc1);
    }
    const bool inter = slice.type != H264SliceType::i, bipred = slice.type == H264SliceType::b;
    if (bipred) b.put(1, 1);
    if (inter) {
        b.put(1, 1); b.ue(1); if (bipred) b.ue(0); // override 2 / 1 references
        b.put(1, 1); b.ue(0); b.ue(0); b.ue(2); b.ue(0); b.ue(3);
        if (bipred) { b.put(1, 1); b.ue(1); b.ue(1); b.ue(3); }
    }
    if ((slice.type == H264SliceType::p && pps.weighted_pred) || (bipred && pps.weighted_bipred == 1)) {
        b.ue(7); b.ue(1);
        for (u32 i = 0; i < (bipred ? 3u : 2u); ++i) {
            b.put(1, 1); b.se(-128); b.se(127); b.put(1, 1);
            b.se(-128); b.se(127); b.se(126); b.se(-127);
        }
    }
    if (slice.reference) {
        if (slice.idr) { b.put(slice.discard_prior, 1); b.put(slice.long_term, 1); }
        else {
            b.put(slice.adaptive_marking, 1);
            if (slice.adaptive_marking) {
                for (u32 i = 0; i < slice.marking_count; ++i) {
                    const auto m = slice.marking[i]; b.ue(m.operation);
                    if (m.operation == 1 || m.operation == 3) b.ue(m.difference_minus1);
                    if (m.operation == 2 || m.operation == 3 || m.operation == 4 || m.operation == 6) b.ue(m.long_term);
                }
                if (extra_marking) { b.ue(1); b.ue(0); }
                b.ue(0);
            }
        }
    }
    if (pps.cabac && inter) b.ue(2);
    b.se(0);
    if (pps.deblocking_control) { b.ue(0); b.se(-6); b.se(6); }
    if (pps.cabac) while (b.count % 8u) b.put(1, 1);
    b.put(0xAB, 8); b.end(); // opaque slice data; parser must not read it.
    nal->encode(b, static_cast<u8>((slice.reference << 5u) | (slice.idr ? 5u : 1u)));
}

int slice_tests() noexcept {
    NalWriter nal;
    H264Sps sps{};
    sps.coded_width = 64; sps.coded_height = 64; sps.frame_num_bits = 4; sps.poc_bits = 4;
    sps.id = 3; sps.profile = 100;
    H264Pps pps{};
    pps.id = 7; pps.sps_id = 3; pps.references_l0 = pps.references_l1 = 4;
    pps.bottom_poc_present = true; pps.weighted_pred = true; pps.weighted_bipred = 1;
    pps.deblocking_control = true;
    H264Slice wanted{};
    wanted.pps_id = 7; wanted.frame_num = 3; wanted.poc_lsb = 6; wanted.delta_bottom = -2;
    wanted.delta_poc0 = -3; wanted.delta_poc1 = 2; wanted.all_same_type = true;
    auto* pages = static_cast<u8*>(VirtualAlloc(nullptr, 8'192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!pages) return 30;
    DWORD old = 0;
    if (!VirtualProtect(pages + 4'096, 4'096, PAGE_NOACCESS, &old)) { VirtualFree(pages, 0, MEM_RELEASE); return 31; }
    bool ok = true;
    for (u32 mode = 0; mode < 72; ++mode) {
        wanted.type = static_cast<H264SliceType>(mode % 3);
        sps.poc_type = static_cast<u8>((mode / 3) % 3);
        pps.cabac = (mode / 9) % 2 != 0;
        sps.frame_only = (mode / 18) % 2 != 0; sps.mbaff = !sps.frame_only;
        wanted.reference = mode / 36 ? 3 : 0;
        wanted.adaptive_marking = wanted.reference != 0;
        wanted.marking_count = wanted.reference ? 6 : 0;
        for (u32 i = 0; i < 6; ++i) wanted.marking[i] = {0, 0, static_cast<u8>(i + 1)};
        make_slice(&nal, sps, pps, wanted);
        u8* input = pages + 4'096 - nal.count;
        copy_bytes(input, nal.bytes, nal.count);
        H264Slice actual{};
        u8 selected_id = 255;
        ok = ok && h264_slice_parameter_id({input, nal.count}, &selected_id) == H264Error::none && selected_id == 7;
        ok = ok && h264_parse_slice({input, nal.count}, sps, pps, &actual) == H264Error::none &&
            actual.type == wanted.type && actual.frame_num == wanted.frame_num && actual.reference == wanted.reference &&
            actual.pps_id == 7 && actual.all_same_type && actual.marking_count == wanted.marking_count &&
            (!actual.marking_count || bytes_equal(actual.marking, wanted.marking, 6 * sizeof(H264Marking))) &&
            (sps.poc_type != 0 || (actual.poc_lsb == 6 && actual.delta_bottom == -2)) &&
            (sps.poc_type != 1 || (actual.delta_poc0 == -3 && actual.delta_poc1 == 2)) &&
            h264_same_picture(actual, actual);
        if (wanted.type != H264SliceType::i) ok = ok && actual.references_l0 == 2;
        for (u32 count = 0; count < nal.count; ++count) {
            input = pages + 4'096 - count; copy_bytes(input, nal.bytes, count);
            H264Slice result = actual;
            const auto error = h264_parse_slice({input, count}, sps, pps, &result);
            // A slice header may be complete before the final payload byte.
            if (error != H264Error::none) ok = ok && bytes_equal(&actual, &result, sizeof(result));
        }
        for (u32 bit = 0; bit < nal.count * 8u; ++bit) {
            input = pages + 4'096 - nal.count; copy_bytes(input, nal.bytes, nal.count);
            input[bit / 8u] ^= static_cast<u8>(1u << (bit % 8u));
            H264Slice result = actual;
            const auto error = h264_parse_slice({input, nal.count}, sps, pps, &result);
            if (error != H264Error::none) ok = ok && bytes_equal(&actual, &result, sizeof(result));
            else ok = ok && result.marking_count <= 32 && result.references_l0 <= 16 && result.references_l1 <= 16;
        }
    }
    VirtualFree(pages, 0, MEM_RELEASE);
    if (!ok) return 32;
    wanted = {}; wanted.type = H264SliceType::i; wanted.reference = 3; wanted.idr = true;
    wanted.idr_id = 65'535; wanted.discard_prior = true; wanted.long_term = true;
    make_slice(&nal, sps, pps, wanted);
    H264Slice result{};
    if (h264_parse_slice(nal.view(), sps, pps, &result) != H264Error::none ||
        !result.idr || !result.long_term || !result.discard_prior || result.idr_id != 65'535) return 33;
    H264Slice other = result; other.idr_id = 1;
    if (h264_same_picture(result, other)) return 34;
    other = result; other.marking_count = 255;
    if (h264_same_picture(other, other)) return 35;
    wanted = {}; wanted.type = H264SliceType::i; wanted.reference = 3;
    wanted.frame_num = 3; wanted.adaptive_marking = true; wanted.marking_count = 32;
    for (auto& item : wanted.marking) item.operation = 1;
    make_slice(&nal, sps, pps, wanted);
    if (h264_parse_slice(nal.view(), sps, pps, &result) != H264Error::none || result.marking_count != 32) return 60;
    const auto saved = result;
    make_slice(&nal, sps, pps, wanted, true);
    if (h264_parse_slice(nal.view(), sps, pps, &result) != H264Error::unsupported ||
        !bytes_equal(&result, &saved, sizeof(result))) return 61;
    // Generic multi-slice access units: mixed I/P is legal unless slice_type
    // explicitly states that all slices share its type. No per-slice storage.
    u8 unit[512]{};
    auto append = [&](u32 offset) noexcept {
        unit[offset] = static_cast<u8>(nal.count >> 24u);
        unit[offset + 1] = static_cast<u8>(nal.count >> 16u);
        unit[offset + 2] = static_cast<u8>(nal.count >> 8u);
        unit[offset + 3] = static_cast<u8>(nal.count);
        copy_bytes(unit + offset + 4, nal.bytes, nal.count);
        return offset + 4 + nal.count;
    };
    wanted = {};
    wanted.type = H264SliceType::i;
    wanted.reference = 3;
    wanted.frame_num = 3;
    make_slice(&nal, sps, pps, wanted);
    u32 bytes = append(0);
    wanted.type = H264SliceType::p;
    wanted.first_mb = 1;
    make_slice(&nal, sps, pps, wanted);
    bytes = append(bytes);
    H264Picture picture{};
    if (h264_parse_picture({unit, bytes}, 4, sps, pps, &picture) != H264Error::none ||
        picture.slice_count != 2 || picture.all_intra || picture.bitstream_bytes != bytes - 2)
        return 67;
    const auto saved_picture = picture;
    const u32 picture_bytes = bytes;
    u8 saved_unit[512]{};
    copy_bytes(saved_unit, unit, picture_bytes);
    const auto restore_picture = [&]() noexcept {
        copy_bytes(unit, saved_unit, picture_bytes);
        bytes = picture_bytes;
    };
    bytes = append(bytes); // Repeated first_mb, not an additional valid slice.
    if (h264_parse_picture({unit, bytes}, 4, sps, pps, &picture) != H264Error::unsupported ||
        !bytes_equal(&picture, &saved_picture, sizeof(picture)))
        return 68;
    // A real sequence boundary can append a header-only type10 marker to
    // the last predictive picture, then begin the next sample with an IDR.
    // Terminal NALs must not inflate the DXVA slice or compressed budgets.
    const auto terminal = [&](u8 header, bool payload = false) noexcept {
        unit[bytes++] = 0;
        unit[bytes++] = 0;
        unit[bytes++] = 0;
        unit[bytes++] = payload ? 2 : 1;
        unit[bytes++] = header;
        if (payload)
            unit[bytes++] = 0x80;
    };
    constexpr u8 endings[] = {10, 11};
    for (const u8 ending : endings) {
        restore_picture();
        terminal(ending);
        if (h264_parse_picture({unit, bytes}, 4, sps, pps, &picture) != H264Error::none ||
            !bytes_equal(&picture, &saved_picture, sizeof(picture)))
            return 77;
        if (ending == 10) {
            terminal(11);
            if (h264_parse_picture({unit, bytes}, 4, sps, pps, &picture) != H264Error::none ||
                !bytes_equal(&picture, &saved_picture, sizeof(picture)))
                return 78;
        }
        bytes = 0;
        terminal(ending);
        if (h264_parse_picture({unit, bytes}, 4, sps, pps, &picture) != H264Error::malformed ||
            !bytes_equal(&picture, &saved_picture, sizeof(picture)))
            return 79;
        for (u32 malformed = 0; malformed < 3; ++malformed) {
            restore_picture();
            terminal(static_cast<u8>(ending | (malformed == 0   ? 0x20
                                               : malformed == 1 ? 0x80
                                                                : 0)),
                     malformed == 2);
            if (h264_parse_picture({unit, bytes}, 4, sps, pps, &picture) != H264Error::malformed ||
                !bytes_equal(&picture, &saved_picture, sizeof(picture)))
                return 80;
        }
        restore_picture();
        terminal(ending);
        terminal(ending);
        if (h264_parse_picture({unit, bytes}, 4, sps, pps, &picture) != H264Error::malformed ||
            !bytes_equal(&picture, &saved_picture, sizeof(picture)))
            return 81;
        restore_picture();
        terminal(ending);
        bytes = append(bytes);
        if (h264_parse_picture({unit, bytes}, 4, sps, pps, &picture) != H264Error::malformed ||
            !bytes_equal(&picture, &saved_picture, sizeof(picture)))
            return 82;
    }
    return 0;
}

int configuration_tests() noexcept {
    NalWriter sequence, picture;
    make_sps(&sequence, true, true, false, 0, false, true);
    make_pps(&picture, true, true);
    H264Sps expected_sps{};
    H264Pps expected_pps{};
    if (h264_parse_sps(sequence.view(), &expected_sps) != H264Error::none ||
        h264_parse_pps(picture.view(), expected_sps, &expected_pps) != H264Error::none)
        return 36;
    u8 avcc[512]{1, 100, 0, 31, 255, 225};
    u32 count = 6;
    avcc[count++] = static_cast<u8>(sequence.count >> 8u);
    avcc[count++] = static_cast<u8>(sequence.count);
    copy_bytes(avcc + count, sequence.bytes, sequence.count);
    count += sequence.count;
    avcc[count++] = 1;
    avcc[count++] = static_cast<u8>(picture.count >> 8u);
    avcc[count++] = static_cast<u8>(picture.count);
    copy_bytes(avcc + count, picture.bytes, picture.count); count += picture.count;
    auto* pages = static_cast<u8*>(VirtualAlloc(nullptr, 8'192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!pages) return 37;
    DWORD old = 0;
    if (!VirtualProtect(pages + 4'096, 4'096, PAGE_NOACCESS, &old)) { VirtualFree(pages, 0, MEM_RELEASE); return 37; }
    bool ok = true;
    for (u32 bytes = 0; bytes <= count; ++bytes) {
        u8* input = pages + 4'096 - bytes; copy_bytes(input, avcc, bytes);
        H264Sps sps{}; H264Pps pps{};
        sps.id = 31; pps.id = 255;
        const auto saved_sps = sps; const auto saved_pps = pps;
        const auto error = h264_select_parameters({input, bytes}, 7, &sps, &pps);
        if (bytes == count) ok = ok && error == H264Error::none &&
            bytes_equal(&sps, &expected_sps, sizeof(sps)) && bytes_equal(&pps, &expected_pps, sizeof(pps));
        else ok = ok && error != H264Error::none && bytes_equal(&sps, &saved_sps, sizeof(sps)) &&
            bytes_equal(&pps, &saved_pps, sizeof(pps));
    }
    VirtualFree(pages, 0, MEM_RELEASE);
    if (!ok) return 38;
    H264Sps sps{}; H264Pps pps{};
    if (h264_select_parameters({avcc, count}, 8, &sps, &pps) != H264Error::malformed) return 39;
    // Parameter repeats at initial and later IDRs must not become slices or
    // inflate DXVA compressed/slice budgets. No changed parameters are adopted.
    u8 repeated[2048]{};
    u32 repeated_count = 0;
    const auto append_repeat = [&](ConstSlice<u8> input) noexcept {
        if (input.count > sizeof(repeated) - repeated_count - 4)
            return false;
        repeated[repeated_count++] = static_cast<u8>(input.count >> 24);
        repeated[repeated_count++] = static_cast<u8>(input.count >> 16);
        repeated[repeated_count++] = static_cast<u8>(input.count >> 8);
        repeated[repeated_count++] = static_cast<u8>(input.count);
        copy_bytes(repeated + repeated_count, input.data, input.count);
        repeated_count += input.count;
        return true;
    };
    NalWriter idr;
    H264Slice wanted{};
    wanted.type = H264SliceType::i;
    wanted.reference = 3;
    wanted.idr = true;
    H264Picture accepted{};
    for (u16 gop = 0; gop < 2; ++gop) {
        wanted.idr_id = gop;
        make_slice(&idr, expected_sps, expected_pps, wanted);
        repeated_count = 0;
        if (!append_repeat(sequence.view()) || !append_repeat(picture.view()) ||
            !append_repeat(idr.view()) ||
            h264_parse_picture({repeated, repeated_count}, 4, expected_sps, expected_pps, &accepted,
                               {avcc, count}) != H264Error::none ||
            accepted.slice_count != 1 || accepted.bitstream_bytes != idr.count + 3 ||
            accepted.first.idr_id != gop)
            return 69;
    }
    const auto accepted_saved = accepted;
    if (h264_parse_picture({repeated, repeated_count}, 4, expected_sps, expected_pps, &accepted) !=
            H264Error::unsupported ||
        !bytes_equal(&accepted, &accepted_saved, sizeof(accepted)))
        return 70;
    NalWriter changed;
    make_sps(&changed, true, true, false, 0, false, true, 1, 4);
    repeated_count = 0;
    if (!append_repeat(changed.view()) || !append_repeat(idr.view()) ||
        h264_parse_picture({repeated, repeated_count}, 4, expected_sps, expected_pps, &accepted,
                           {avcc, count}) != H264Error::unsupported ||
        !bytes_equal(&accepted, &accepted_saved, sizeof(accepted)))
        return 71;
    make_pps(&changed, true, false);
    repeated_count = 0;
    if (!append_repeat(changed.view()) || !append_repeat(idr.view()) ||
        h264_parse_picture({repeated, repeated_count}, 4, expected_sps, expected_pps, &accepted,
                           {avcc, count}) != H264Error::unsupported ||
        !bytes_equal(&accepted, &accepted_saved, sizeof(accepted)))
        return 72;
    constexpr u8 parameter_types[]{0x67, 0x68};
    for (const u8 parameter_type : parameter_types) {
        repeated_count = 0;
        // Public parameter parsers require a header plus RBSP byte; this
        // header-only source is invalid_input, not a changed configuration.
        const u8 truncated[]{parameter_type};
        if (!append_repeat({truncated, 1}) || !append_repeat(idr.view()))
            return 73;
        const auto error = h264_parse_picture({repeated, repeated_count}, 4, expected_sps, expected_pps,
                                              &accepted, {avcc, count});
        if (error != H264Error::invalid_input || !bytes_equal(&accepted, &accepted_saved, sizeof(accepted)))
            return 74;
    }
    repeated_count = 0;
    if (!append_repeat(sequence.view()) || !append_repeat(idr.view()))
        return 75;
    repeated[4] |= 0x80; // Forbidden NAL header bit, not a changed configuration.
    if (h264_parse_picture({repeated, repeated_count}, 4, expected_sps, expected_pps, &accepted,
                           {avcc, count}) != H264Error::malformed ||
        !bytes_equal(&accepted, &accepted_saved, sizeof(accepted)))
        return 76;
    return 0;
}

int run() noexcept {
    // Independent bit writer: full unsigned/signed boundary values and EPBs.
    BitsWriter b;
    constexpr u32 unsigned_values[] = {0, 1, 2, 31, 32, 65'535, 0xFFFF'FFFFu};
    constexpr i32 signed_values[] = {0, 1, -1, 127, -128, 2'147'483'647, (-2'147'483'647 - 1)};
    for (u32 n : unsigned_values)
        b.ue(n);
    for (i32 n : signed_values)
        b.se(n);
    b.put(0, 32);
    b.put(1, 32);
    b.end();
    NalWriter nal;
    nal.encode(b, 0x67);
    H264Bits reader{{nal.bytes + 1, nal.count - 1}};
    for (u32 n : unsigned_values) if (reader.ue() != n) return 1;
    for (i32 n : signed_values) if (reader.se() != n) return 2;
    if (reader.read(32) || reader.read(32) != 1 || !reader.finish()) return 3;
    constexpr u8 bad_epb[] = {0,0,3,4};
    H264Bits bad{{bad_epb, sizeof(bad_epb)}};
    bad.read(24); if (bad.error != H264Error::malformed) return 4;
    constexpr u8 missing_epb[] = {0,0,1};
    bad = {{missing_epb, sizeof(missing_epb)}};
    bad.read(24); if (bad.error != H264Error::malformed) return 5;
    constexpr u8 dangling_epb[] = {0,0,3};
    bad = {{dangling_epb, sizeof(dangling_epb)}};
    bad.read(24); if (bad.error != H264Error::truncated) return 6;

    H264Sps sps{};
    constexpr bool field_modes[] = {false, true};
    for (u32 poc = 0; poc < 3; ++poc) for (bool fields : field_modes) {
        make_sps(&nal, false, false, false, poc, fields, true);
        if (h264_parse_sps(nal.view(), &sps) != H264Error::none ||
            sps.id != 3 || sps.width != 74 || sps.height != (fields ? 112u : 56u) ||
            sps.coded_width != 80 || sps.coded_height != (fields ? 128u : 64u) ||
            sps.crop_x != 2 || sps.crop_y != (fields ? 4u : 2u) ||
            sps.sar_width != 297 || sps.sar_height != 296 || !sps.full_range ||
            sps.matrix != 1 || sps.units_in_tick != 1'001 || sps.time_scale != 60'000 ||
            sps.max_references != 4 || sps.max_reorder != 2 || sps.frame_num_bits != 4 ||
            sps.poc_type != poc || (poc == 1 && sps.cycle_delta != -2)) return 10;
    }
    make_sps(&nal, true, true, true, 0, false, false);
    if (h264_parse_sps(nal.view(), &sps) != H264Error::none) return 11;
    for (u32 i = 0; i < 16; ++i) if (sps.scaling.list4x4[0][i] != 64 || sps.scaling.list4x4[1][i] != 64) return 12;
    if (sps.scaling.list4x4[3][0] != 10 || sps.scaling.list8x8[0][63] != 42) return 13;
    H264Pps pps{};
    NalWriter pps_nal;
    make_pps(&pps_nal, true, true);
    if (h264_parse_pps(pps_nal.view(), sps, &pps) != H264Error::none ||
        pps.id != 7 || pps.sps_id != 3 || !pps.cabac || !pps.transform_8x8 ||
        pps.initial_qp != -3 || pps.chroma_qp != -2 || pps.second_chroma_qp != 3 ||
        pps.references_l0 != 4 || pps.references_l1 != 2 ||
        !bytes_equal(&sps.scaling, &pps.scaling, sizeof(pps.scaling))) return 14;
    make_pps(&pps_nal, false, false, 1);
    if (h264_parse_pps(pps_nal.view(), sps, &pps) != H264Error::unsupported) return 15;

    // Every truncated SPS/PPS leaves caller state unchanged, with the source
    // ending directly at an inaccessible page (no implicit decoder padding).
    make_pps(&pps_nal, true, true);
    auto* pages = static_cast<u8*>(VirtualAlloc(nullptr, 8'192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!pages) return 16;
    DWORD old = 0;
    if (!VirtualProtect(pages + 4'096, 4'096, PAGE_NOACCESS, &old)) { VirtualFree(pages, 0, MEM_RELEASE); return 17; }
    bool guarded_ok = true;
    const H264Sps saved = sps;
    for (u32 count = 0; count < nal.count; ++count) {
        copy_bytes(pages + 4'096 - count, nal.bytes, count);
        guarded_ok = guarded_ok && h264_parse_sps({pages + 4'096 - count, count}, &sps) != H264Error::none &&
            bytes_equal(&saved, &sps, sizeof(sps));
    }
    const H264Pps saved_pps = pps;
    for (u32 count = 0; count < pps_nal.count; ++count) {
        copy_bytes(pages + 4'096 - count, pps_nal.bytes, count);
        guarded_ok = guarded_ok && h264_parse_pps({pages + 4'096 - count, count}, sps, &pps) != H264Error::none &&
            bytes_equal(&saved_pps, &pps, sizeof(pps));
    }
    // Also exercise complete and single-bit-mutated syntax at the guard page.
    // Mutations may describe valid streams; only errors must be transactional.
    for (u32 i = 0; i <= nal.count * 8u; ++i) {
        u8* input = pages + 4'096 - nal.count;
        copy_bytes(input, nal.bytes, nal.count);
        if (i < nal.count * 8u) input[i / 8u] ^= static_cast<u8>(1u << (i % 8u));
        H264Sps result = saved;
        const auto error = h264_parse_sps({input, nal.count}, &result);
        if (error == H264Error::none) {
            guarded_ok = guarded_ok && result.width && result.height && result.id < 32 &&
                result.crop_x + result.width <= result.coded_width &&
                result.crop_y + result.height <= result.coded_height && result.max_references <= 16;
        } else guarded_ok = guarded_ok && bytes_equal(&saved, &result, sizeof(result));
        if (i == nal.count * 8u) guarded_ok = guarded_ok && error == H264Error::none;
    }
    for (u32 i = 0; i <= pps_nal.count * 8u; ++i) {
        u8* input = pages + 4'096 - pps_nal.count;
        copy_bytes(input, pps_nal.bytes, pps_nal.count);
        if (i < pps_nal.count * 8u) input[i / 8u] ^= static_cast<u8>(1u << (i % 8u));
        H264Pps result = saved_pps;
        const auto error = h264_parse_pps({input, pps_nal.count}, saved, &result);
        if (error == H264Error::none) {
            guarded_ok = guarded_ok && result.sps_id == saved.id && result.references_l0 &&
                result.references_l0 <= 32 && result.references_l1 && result.references_l1 <= 32;
        } else guarded_ok = guarded_ok && bytes_equal(&saved_pps, &result, sizeof(result));
        if (i == pps_nal.count * 8u) guarded_ok = guarded_ok && error == H264Error::none;
    }
    VirtualFree(pages, 0, MEM_RELEASE);
    if (!guarded_ok) return 18;
    make_sps(&nal, false, false, false, 0, false, true, 5, 4);
    if (h264_parse_sps(nal.view(), &sps) != H264Error::malformed) return 19;

    // Length parsing is transactional; malformed/truncated input cannot advance.
    constexpr u8 access_unit[] = {0,0,0,2,0x65,0x88,0,0,0,1,0x09};
    H264NalCursor cursor{{access_unit, sizeof(access_unit)}, 0, 4};
    const H264Nal first = h264_next_nal(&cursor), second = h264_next_nal(&cursor);
    if (first.error != H264Error::none || first.type != 5 || first.reference != 3 ||
        first.bytes.data != access_unit + 4 || second.type != 9 ||
        h264_next_nal(&cursor).error != H264Error::end) return 20;
    cursor = {{access_unit, 5}, 0, 4};
    if (h264_next_nal(&cursor).error != H264Error::truncated || cursor.offset) return 21;
    cursor.length_bytes = 3;
    if (h264_next_nal(&cursor).error != H264Error::invalid_input || cursor.offset) return 22;
    constexpr u8 lengths[] = {1,2,4};
    for (u8 length : lengths) {
        u8 sample[6]{};
        sample[length - 1u] = 2; sample[length] = 0x65; sample[length + 1u] = 0x88;
        cursor = {{sample, static_cast<u32>(length) + 2u}, 0, length};
        if (h264_next_nal(&cursor).error != H264Error::none ||
            h264_next_nal(&cursor).error != H264Error::end) return 23;
        cursor.offset = 0; sample[length - 1u] = 0;
        if (h264_next_nal(&cursor).error != H264Error::malformed || cursor.offset) return 24;
        sample[length - 1u] = 2; sample[length] |= 0x80u;
        if (h264_next_nal(&cursor).error != H264Error::malformed || cursor.offset) return 25;
    }
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_h264_tests_entry() noexcept {
    int status = maicord::run();
    if (!status) status = maicord::slice_tests();
    if (!status) status = maicord::configuration_tests();
    if (!status) status = maicord::h264_state_tests();
    if (!status) status = maicord::h264_stream_tests();
    if (status) {
        char text[] = "h264 tests failed: 000\n";
        text[19] = static_cast<char>('0' + status / 100 % 10);
        text[20] = static_cast<char>('0' + status / 10 % 10);
        text[21] = static_cast<char>('0' + status % 10);
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, sizeof(text) - 1, &written, nullptr);
    }
    ExitProcess(static_cast<UINT>(status));
}
