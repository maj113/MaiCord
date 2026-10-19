#include "maicord/media/h264_stream.hpp"
#include "maicord/media/gpu_video_frame.hpp"
#include "h264_stream_fixture.hpp"

namespace maicord {
namespace {
using namespace h264_fixture;

u8 g_bits[64 * 1024];
u8 g_stream[128 * 1024];
H264StreamState g_state;
H264StreamPicture g_picture;

struct Writer {
    Bits bits{g_bits, sizeof(g_bits), 0, true};
    Output out{g_stream, sizeof(g_stream), 0, true};
    void clear() noexcept { out.count = 0; out.ok = true; }
    ConstSlice<u8> view() const noexcept { return {out.data, out.count}; }
};

bool nal_is(const H264Nal& nal, u8 type, const u8* bytes, u32 count) noexcept {
    return nal.error == H264Error::none && nal.type == type && nal.bytes.count == count &&
           bytes_equal(nal.bytes.data, bytes, count);
}

int annexb_tests() noexcept {
    constexpr u8 mixed[] = {0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 0, 0, 3, 1, 0, 0,
                            0, 0, 0, 1, 0,    0, 1, 0x65, 9, 0, 0};
    H264AnnexBCursor cursor{{mixed, sizeof(mixed)}, 0};
    constexpr u8 sps[] = {0x67, 1, 2}, pps[] = {0x68, 0, 0, 3, 1}, idr[] = {0x65, 9};
    if (!nal_is(h264_annexb_next(&cursor), 7, sps, 3)) return 100;
    if (!nal_is(h264_annexb_next(&cursor), 8, pps, 5)) return 101; // escape kept, trailing zeros dropped
    const auto slice = h264_annexb_next(&cursor);                     // empty NAL skipped
    if (!nal_is(slice, 5, idr, 2) || slice.reference != 3) return 102;
    if (h264_annexb_next(&cursor).error != H264Error::end || h264_annexb_next(&cursor).error != H264Error::end)
        return 103;
    constexpr u8 garbage[] = {0xAA, 0, 0, 1, 0x41, 7};
    cursor = {{garbage, sizeof(garbage)}, 0};
    constexpr u8 p[] = {0x41, 7};
    if (h264_annexb_next(&cursor).error != H264Error::malformed || !nal_is(h264_annexb_next(&cursor), 1, p, 2) ||
        h264_annexb_next(&cursor).error != H264Error::end)
        return 104;
    constexpr u8 forbidden[] = {0, 0, 1, 0xE5, 1, 0, 0, 1, 0x41, 2};
    cursor = {{forbidden, sizeof(forbidden)}, 0};
    const auto bad = h264_annexb_next(&cursor);
    constexpr u8 p2[] = {0x41, 2};
    if (bad.error != H264Error::malformed || bad.type != 5 || !nal_is(h264_annexb_next(&cursor), 1, p2, 2))
        return 105;
    constexpr u8 none[] = {1, 2, 3}, zeros[] = {0, 0, 0, 0};
    cursor = {{none, sizeof(none)}, 0};
    if (h264_annexb_next(&cursor).error != H264Error::malformed || h264_annexb_next(&cursor).error != H264Error::end)
        return 106;
    cursor = {{zeros, sizeof(zeros)}, 0};
    if (h264_annexb_next(&cursor).error != H264Error::end) return 107;
    cursor = {{nullptr, 0}, 0};
    if (h264_annexb_next(&cursor).error != H264Error::end) return 108;
    cursor = {{nullptr, 4}, 0};
    if (h264_annexb_next(&cursor).error != H264Error::invalid_input || h264_annexb_next(nullptr).error != H264Error::invalid_input)
        return 109;
    // A start code split across the end of input yields no phantom NAL.
    constexpr u8 tail[] = {0, 0, 1, 0x41, 5, 0, 0};
    cursor = {{tail, sizeof(tail)}, 0};
    constexpr u8 p3[] = {0x41, 5};
    if (!nal_is(h264_annexb_next(&cursor), 1, p3, 2) || h264_annexb_next(&cursor).error != H264Error::end) return 110;
    return 0;
}

// Stores every SPS/PPS of a generated buffer.
void store_all(H264ParameterSets* sets, ConstSlice<u8> bytes) noexcept {
    H264AnnexBCursor cursor{bytes, 0};
    for (auto nal = h264_annexb_next(&cursor); nal.error != H264Error::end; nal = h264_annexb_next(&cursor))
        if (nal.error == H264Error::none && (nal.type == 7 || nal.type == 8)) h264_parameter_sets_store(sets, nal.bytes);
}

int parameter_tests() noexcept {
    Writer w;
    auto* sets = &g_state.sets;
    *sets = {};
    H264Sps sps{};
    H264Pps pps{};
    const Sps base{0, 66, 2, 2, 2, 1, false, 0, 0, 0};
    write_sps(&w.out, &w.bits, base);
    write_pps(&w.out, &w.bits, {0, 0});
    store_all(sets, w.view());
    if (h264_parameter_sets_resolve(*sets, 0, &sps, &pps) != H264Error::none || sps.coded_width != 32 || pps.id)
        return 120;
    // New PPS id for a not-yet-seen SPS id, then that SPS arrives.
    w.clear();
    write_pps(&w.out, &w.bits, {5, 1});
    store_all(sets, w.view());
    if (h264_parameter_sets_resolve(*sets, 5, &sps, &pps) == H264Error::none) return 121;
    w.clear();
    write_sps(&w.out, &w.bits, {1, 100, 4, 2, 0, 2, true, 0, 0, 40});
    store_all(sets, w.view());
    if (h264_parameter_sets_resolve(*sets, 5, &sps, &pps) != H264Error::none || sps.coded_width != 64 ||
        sps.profile != 100 || pps.sps_id != 1 || !sps.restriction || sps.max_reorder)
        return 122;
    // Same id, changed content: the replacement wins; PPS 0 is reparsed against it.
    w.clear();
    write_sps(&w.out, &w.bits, {0, 66, 3, 3, 2, 1, false, 0, 0, 0});
    store_all(sets, w.view());
    if (h264_parameter_sets_resolve(*sets, 0, &sps, &pps) != H264Error::none || sps.coded_width != 48) return 123;
    H264Sps cropped = sps;
    cropped.crop_y = 2; cropped.height -= 2; cropped.matrix = 1;
    if (!h264_sps_decode_compatible(sps, cropped)) return 124;
    cropped.max_references = 4;
    if (h264_sps_decode_compatible(sps, cropped)) return 125;
    // Twenty-one PPS ids through sixteen slots: the oldest are evicted.
    w.clear();
    for (u8 id = 10; id <= 30; ++id) write_pps(&w.out, &w.bits, {id, 0});
    store_all(sets, w.view());
    if (h264_parameter_sets_resolve(*sets, 30, &sps, &pps) != H264Error::none || pps.id != 30 ||
        h264_parameter_sets_resolve(*sets, 15, &sps, &pps) != H264Error::none ||
        h264_parameter_sets_resolve(*sets, 0, &sps, &pps) == H264Error::none ||
        h264_parameter_sets_resolve(*sets, 10, &sps, &pps) == H264Error::none)
        return 126;
    // A malformed replacement invalidates the id instead of keeping stale data.
    w.clear();
    write_sps(&w.out, &w.bits, {0, 66, 2, 2, 2, 1, false, 0, 0, 0});
    H264AnnexBCursor cursor{w.view(), 0};
    const auto nal = h264_annexb_next(&cursor);
    if (h264_parameter_sets_store(sets, {nal.bytes.data, nal.bytes.count - 1u}) == H264Error::none ||
        h264_parameter_sets_resolve(*sets, 30, &sps, &pps) == H264Error::none)
        return 127;
    // Unsupported chroma format (High 4:4:4) is reported as unsupported.
    constexpr u8 high444[] = {0x67, 244, 0, 40, 0x90}; // id 0, chroma_format_idc 3
    if (h264_parameter_sets_store(sets, {high444, sizeof(high444)}) != H264Error::unsupported) return 128;
    u8 oversized[kH264StreamMaximumPpsBytes + 8]{};
    oversized[0] = 0x68; oversized[1] = 0xC0; // pps 0, sps 0
    if (h264_parameter_sets_store(sets, {oversized, sizeof(oversized)}) != H264Error::unsupported) return 129;
    if (h264_parameter_sets_store(nullptr, {oversized, 2}) != H264Error::invalid_input) return 130;
    return 0;
}

int scan_tests() noexcept {
    Writer w;
    auto* sets = &g_state.sets;
    *sets = {};
    const Sps sps{0, 66, 4, 2, 2, 1, false, 0, 0, 0};
    const Pps pps{0, 0};
    write_idr(&w.out, &w.bits, sps, pps, 1, 0, 2);
    write_p(&w.out, &w.bits, sps, pps, 1, 0, true, 2); // no AUD: boundary from the slice header
    write_aud(&w.out, &w.bits, false);
    write_p(&w.out, &w.bits, sps, pps, 2, 0);
    if (!w.out.ok) return 140;
    auto scanner = h264_stream_scanner(w.view());
    auto* p = &g_picture;
    if (h264_stream_scan(sets, &scanner, p) != H264ScanStatus::picture || !p->first.idr || p->slice_count != 2 ||
        !p->all_intra || p->sps.coded_width != 64 || p->slices[1].size + p->slices[0].size + 6 != p->bitstream_bytes ||
        g_stream[p->slices[0].offset] != 0x65 || g_stream[p->slices[1].offset] != 0x65)
        return 141;
    if (h264_stream_scan(sets, &scanner, p) != H264ScanStatus::picture || p->first.idr || p->first.frame_num != 1 ||
        p->slice_count != 2 || p->all_intra)
        return 142;
    if (h264_stream_scan(sets, &scanner, p) != H264ScanStatus::picture || p->first.frame_num != 2 ||
        p->slice_count != 1 || h264_stream_scan(sets, &scanner, p) != H264ScanStatus::end)
        return 143;
    // Missing leading slice, unknown PPS, truncated header, data partitioning.
    w.clear();
    write_slice(&w.out, &w.bits, sps, pps, {false, 2, 3, 0, 0, 4, 4, 0});
    scanner = h264_stream_scanner(w.view());
    if (h264_stream_scan(sets, &scanner, p) != H264ScanStatus::damaged || p->slice_count ||
        h264_stream_scan(sets, &scanner, p) != H264ScanStatus::end)
        return 144;
    w.clear();
    write_p(&w.out, &w.bits, sps, {9, 0}, 3, 0);
    scanner = h264_stream_scanner(w.view());
    if (h264_stream_scan(sets, &scanner, p) != H264ScanStatus::damaged) return 145;
    w.clear();
    write_p(&w.out, &w.bits, sps, pps, 3, 0);
    scanner = h264_stream_scanner({g_stream, 6});
    if (h264_stream_scan(sets, &scanner, p) != H264ScanStatus::damaged) return 146;
    constexpr u8 partition[] = {0, 0, 1, 0x42, 0x80};
    scanner = h264_stream_scanner({partition, sizeof(partition)});
    if (h264_stream_scan(sets, &scanner, p) != H264ScanStatus::damaged || p->error != H264Error::unsupported)
        return 147;
    // SEI after a picture starts the next access unit; end of sequence ends one.
    w.clear();
    write_idr(&w.out, &w.bits, sps, pps, 1, 1, 1, false);
    constexpr u8 sei[] = {0, 0, 1, 0x06, 0x05, 0x01, 0x00, 0x80, 0, 0, 1, 0x0A};
    for (u8 value : sei) w.out.byte(value);
    write_p(&w.out, &w.bits, sps, pps, 1, 0);
    scanner = h264_stream_scanner(w.view());
    if (h264_stream_scan(sets, &scanner, p) != H264ScanStatus::picture || !p->first.idr ||
        h264_stream_scan(sets, &scanner, p) != H264ScanStatus::picture || p->first.idr ||
        h264_stream_scan(sets, &scanner, p) != H264ScanStatus::end)
        return 148;
    // A damaged picture does not swallow the intact one behind it.
    w.clear();
    write_slice(&w.out, &w.bits, sps, pps, {false, 2, 1, 0, 0, 4, 4, 0});
    write_p(&w.out, &w.bits, sps, pps, 1, 0);
    scanner = h264_stream_scanner(w.view());
    if (h264_stream_scan(sets, &scanner, p) != H264ScanStatus::damaged ||
        h264_stream_scan(sets, &scanner, p) != H264ScanStatus::picture || p->slice_count != 1)
        return 149;
    return 0;
}

struct Run {
    H264StreamOutput outputs[32];
    u32 output_count, requests, unsupported, reinitializations;
};

// Mirrors the platform decoder's sequencing with an always-accepting GPU.
void run(Run* r, ConstSlice<u8> bytes, u32 rtp) noexcept {
    auto* state = &g_state;
    auto scanner = h264_stream_scanner(bytes);
    const auto drain = [&]() noexcept {
        H264StreamOutput output{};
        while (h264_stream_pop(state, &output))
            if (r->output_count < 32) r->outputs[r->output_count++] = output;
    };
    for (;;) {
        const auto scanned = h264_stream_scan(&state->sets, &scanner, &g_picture);
        if (scanned == H264ScanStatus::end) return;
        if (scanned == H264ScanStatus::damaged) {
            ++r->requests;
            if (state->active && !state->waiting) h264_stream_lost(state);
            continue;
        }
        H264StreamDecode decode{};
        const auto action = h264_stream_prepare(state, g_picture, &decode);
        if (action == H264StreamAction::request_keyframe) { ++r->requests; continue; }
        if (action == H264StreamAction::unsupported) { ++r->unsupported; continue; }
        drain();
        if (decode.reinitialize) {
            ++r->reinitializations;
            h264_stream_set_surfaces(state, decode.surfaces_needed);
        }
        h264_stream_commit(state, g_picture, decode, rtp);
        drain();
    }
}

int state_tests() noexcept {
    Writer w;
    auto* state = &g_state;
    *state = {};
    h264_stream_reset(state);
    Run r{};
    Sps sps{0, 66, 2, 2, 2, 1, false, 0, 0, 0};
    const Pps pps{0, 0};
    // Inter picture before any IDR: dropped with a keyframe request.
    write_sps(&w.out, &w.bits, sps);
    write_pps(&w.out, &w.bits, pps);
    write_p(&w.out, &w.bits, sps, pps, 1, 0);
    run(&r, w.view(), 500);
    if (r.requests != 1 || r.output_count) return 160;
    w.clear(); write_idr(&w.out, &w.bits, sps, pps, 1, 0); run(&r, w.view(), 1000);
    w.clear(); write_p(&w.out, &w.bits, sps, pps, 1, 0); run(&r, w.view(), 2000);
    w.clear(); write_p(&w.out, &w.bits, sps, pps, 2, 0); run(&r, w.view(), 3000);
    // Zero-latency output: each picture leaves as soon as it is decoded.
    if (r.output_count != 3 || !r.outputs[0].keyframe || r.outputs[1].keyframe || r.outputs[0].rtp_timestamp != 1000 ||
        r.outputs[2].rtp_timestamp != 3000 || r.reinitializations != 1 || state->surfaces != 6 + kVideoStreamPresentationSurfaces || state->reorder)
        return 161;
    // Repeated identical parameter sets mid-stream are harmless.
    w.clear();
    write_sps(&w.out, &w.bits, sps); write_pps(&w.out, &w.bits, pps);
    write_p(&w.out, &w.bits, sps, pps, 3, 0);
    run(&r, w.view(), 4000);
    if (r.output_count != 4 || r.requests != 1) return 162;
    // frame_num gap (picture 4 lost): freeze, request, ignore inter pictures.
    w.clear(); write_p(&w.out, &w.bits, sps, pps, 5, 0); run(&r, w.view(), 6000);
    w.clear(); write_p(&w.out, &w.bits, sps, pps, 6, 0); run(&r, w.view(), 7000);
    if (r.output_count != 4 || r.requests != 3 || state->gaps != 1 || !state->waiting) return 163;
    // A lost non-reference picture is not a gap.
    w.clear(); write_idr(&w.out, &w.bits, sps, pps, 2, 1); run(&r, w.view(), 8000);
    w.clear(); write_p(&w.out, &w.bits, sps, pps, 1, 0); run(&r, w.view(), 9000);
    w.clear(); write_p(&w.out, &w.bits, sps, pps, 2, 0, true); run(&r, w.view(), 11000);
    if (r.output_count != 7 || r.reinitializations != 1 || state->gaps != 1) return 164;
    // Changed SPS without an IDR is rejected; the IDR that follows reinitializes.
    const Sps wide{0, 66, 4, 2, 2, 1, false, 0, 0, 0};
    w.clear();
    write_sps(&w.out, &w.bits, wide); write_pps(&w.out, &w.bits, pps);
    write_p(&w.out, &w.bits, wide, pps, 3, 0);
    run(&r, w.view(), 12000);
    if (r.output_count != 7 || r.requests != 4) return 165;
    w.clear(); write_idr(&w.out, &w.bits, wide, pps, 3, 2); run(&r, w.view(), 13000);
    if (r.output_count != 8 || r.reinitializations != 2 || state->sps.coded_width != 64) return 166;
    // New SPS/PPS ids and High profile with declared zero reordering.
    const Sps high{2, 100, 2, 2, 2, 1, true, 0, 0, 40};
    const Pps high_pps{7, 2};
    w.clear(); write_idr(&w.out, &w.bits, high, high_pps, 4, 0); run(&r, w.view(), 14000);
    w.clear(); write_p(&w.out, &w.bits, high, high_pps, 1, 0); run(&r, w.view(), 15000);
    if (r.output_count != 10 || r.reinitializations != 3 || state->reorder || state->reorder_limit) return 167;
    // Undeclared reordering: the first late picture is dropped and the delay grows.
    const Sps reorder{3, 100, 2, 2, 0, 2, false, 0, 0, 40};
    const Pps reorder_pps{8, 3};
    r = {};
    w.clear(); write_idr(&w.out, &w.bits, reorder, reorder_pps, 5, 0); run(&r, w.view(), 1);
    w.clear(); write_p(&w.out, &w.bits, reorder, reorder_pps, 1, 8); run(&r, w.view(), 2);
    w.clear(); write_p(&w.out, &w.bits, reorder, reorder_pps, 2, 4, false); run(&r, w.view(), 3);
    w.clear(); write_p(&w.out, &w.bits, reorder, reorder_pps, 2, 16); run(&r, w.view(), 4);
    w.clear(); write_p(&w.out, &w.bits, reorder, reorder_pps, 3, 12, false); run(&r, w.view(), 5);
    w.clear(); write_p(&w.out, &w.bits, reorder, reorder_pps, 3, 24); run(&r, w.view(), 6);
    if (state->late != 1 || state->reorder != 1 || r.output_count != 4 || r.outputs[0].poc != 0 ||
        r.outputs[1].poc != 8 || r.outputs[2].poc != 12 || r.outputs[3].poc != 16)
        return 168;
    // Declared reordering (max_num_reorder_frames 1) outputs in POC order.
    const Sps declared{4, 100, 2, 2, 0, 2, true, 1, 0, 40};
    const Pps declared_pps{9, 4};
    r = {};
    w.clear(); write_idr(&w.out, &w.bits, declared, declared_pps, 6, 1); run(&r, w.view(), 1);
    w.clear(); write_p(&w.out, &w.bits, declared, declared_pps, 1, 8); run(&r, w.view(), 2);
    w.clear(); write_p(&w.out, &w.bits, declared, declared_pps, 2, 4, false); run(&r, w.view(), 3);
    w.clear(); write_p(&w.out, &w.bits, declared, declared_pps, 2, 16); run(&r, w.view(), 4);
    w.clear(); write_p(&w.out, &w.bits, declared, declared_pps, 3, 12, false); run(&r, w.view(), 5);
    w.clear(); write_idr(&w.out, &w.bits, declared, declared_pps, 6, 2); run(&r, w.view(), 6);
    // The first IDR drains the previous stream's held picture (POC 24) first.
    constexpr i32 expected[] = {24, 0, 4, 8, 12, 16};
    if (r.output_count != 6 || state->late != 1 || state->reorder != 1 || state->output.count != 1) return 169;
    for (u32 i = 0; i < 6; ++i)
        if (r.outputs[i].poc != expected[i]) return 170;
    if (r.outputs[2].rtp_timestamp != 3 || r.outputs[3].rtp_timestamp != 2 || !r.outputs[1].keyframe) return 171;
    // Geometry beyond the live budget (4800x4800) is refused at the IDR.
    const Sps huge{5, 66, 300, 300, 2, 1, false, 0, 0, 52};
    w.clear();
    write_sps(&w.out, &w.bits, huge); write_pps(&w.out, &w.bits, {10, 5});
    write_slice(&w.out, &w.bits, huge, {10, 5}, {true, 3, 0, 0, 0, 0, 1, 0});
    run(&r, w.view(), 7);
    if (r.unsupported != 1 || !state->unsupported || !state->waiting) return 172;
    // Damaged input after decoding resumed freezes until the next IDR.
    w.clear(); write_idr(&w.out, &w.bits, sps, pps, 1, 3); run(&r, w.view(), 8);
    const u32 before = r.requests;
    w.clear(); write_slice(&w.out, &w.bits, sps, pps, {false, 2, 1, 0, 0, 2, 2, 0}); run(&r, w.view(), 9);
    w.clear(); write_p(&w.out, &w.bits, sps, pps, 2, 0); run(&r, w.view(), 10);
    if (r.requests != before + 2 || !state->waiting || state->unsupported) return 173;
    return 0;
}

// Surface pools are sized from the DPB the stream may actually use: the
// signalled max_dec_frame_buffering, else the level limit (Table A-1), never
// the 16-frame syntax maximum unless the level is unknown or contradicted.
int dpb_tests() noexcept {
    H264Sps sps{};
    sps.coded_width = 1920;
    sps.coded_height = 1088;
    sps.level = 41;
    sps.max_references = 1;
    sps.max_reorder = sps.max_buffering = 16;
    if (h264_sps_dpb_frames(sps) != 4 || h264_sps_output_reorder(sps) != 4) return 180;
    sps.restriction = true;
    sps.max_reorder = 0;
    sps.max_buffering = 2;
    if (h264_sps_dpb_frames(sps) != 2 || h264_sps_output_reorder(sps) != 0) return 181;
    sps.restriction = false;
    sps.max_reorder = sps.max_buffering = 16;
    sps.level = 30; // Mislabelled: level 3.0 cannot hold one 1080p frame.
    if (h264_sps_dpb_frames(sps) != 16 || h264_sps_output_reorder(sps) != 16) return 182;
    sps.level = 41;
    sps.max_references = 5; // Contradicts the level limit.
    if (h264_sps_dpb_frames(sps) != 16) return 183;
    sps.max_references = 1;
    sps.coded_width = 1280;
    sps.coded_height = 720;
    sps.level = 31;
    if (h264_sps_dpb_frames(sps) != 5) return 184;
    sps.level = 0;
    if (h264_sps_dpb_frames(sps) != 16) return 185;
    return 0;
}

} // namespace

int h264_stream_tests() noexcept {
    int status = annexb_tests();
    if (!status) status = parameter_tests();
    if (!status) status = scan_tests();
    if (!status) status = state_tests();
    if (!status) status = dpb_tests();
    return status;
}

} // namespace maicord
