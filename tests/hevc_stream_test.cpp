#include "maicord/media/hevc_stream.hpp"
#include "maicord/media/gpu_video_frame.hpp"
#include "hevc_stream_fixture.hpp"

namespace maicord {
namespace {
using namespace hevc_fixture;

u8 g_bits[64 * 1024];
u8 g_stream[128 * 1024];
HevcStreamState g_state;
HevcStreamPicture g_picture;

struct Writer {
    Bits bits{g_bits, sizeof(g_bits), 0, true};
    Output out{g_stream, sizeof(g_stream), 0, true};
    void clear() noexcept {
        out.count = 0;
        out.ok = true;
    }
    ConstSlice<u8> view() const noexcept {
        return {out.data, out.count};
    }
};

constexpr Sps kSmall{0, 64, 48, 0, 4, 8, 2};
constexpr Pps kSmallPps{0, 0};

// Scans one input; counts pictures and damaged ones.
struct Scanned {
    u32 pictures, damaged;
    HevcError error;
};
Scanned scan_all(ConstSlice<u8> bytes) noexcept {
    Scanned result{};
    auto scanner = hevc_stream_scanner(bytes);
    for (;;) {
        const auto status = hevc_stream_scan(&g_state.sets, &scanner, &g_picture);
        if (status == HevcScanStatus::end)
            return result;
        ++result.pictures;
        if (status == HevcScanStatus::damaged) {
            ++result.damaged;
            result.error = g_picture.error;
        }
    }
}

// Decodes every picture of one input as the GPU layer would; returns the
// last action and the number of pictures presented.
struct Decoded {
    HevcStreamAction action;
    u32 decoded, presented, reinitialized;
    u8 surface;
};
Decoded decode(ConstSlice<u8> bytes, u32 rtp) noexcept {
    Decoded result{HevcStreamAction::request_keyframe};
    auto scanner = hevc_stream_scanner(bytes);
    H264StreamOutput output{};
    for (;;) {
        const auto status = hevc_stream_scan(&g_state.sets, &scanner, &g_picture);
        if (status == HevcScanStatus::end)
            break;
        if (status == HevcScanStatus::damaged) {
            result.action = HevcStreamAction::request_keyframe;
            continue;
        }
        HevcStreamDecode plan{};
        result.action = hevc_stream_prepare(&g_state, g_picture, &plan);
        if (result.action != HevcStreamAction::decode)
            continue;
        while (hevc_stream_pop(&g_state, &output))
            ++result.presented;
        if (plan.reinitialize) {
            hevc_stream_set_surfaces(&g_state, plan.surfaces_needed);
            ++result.reinitialized;
        }
        hevc_stream_commit(&g_state, g_picture, plan, rtp);
        result.surface = plan.surface;
        ++result.decoded;
        while (hevc_stream_pop(&g_state, &output))
            ++result.presented;
    }
    return result;
}

int parameter_tests() noexcept {
    Writer w;
    g_state = {};
    hevc_stream_reset(&g_state);
    write_hevc_sps(&w.out, &w.bits, kSmall);
    write_hevc_pps(&w.out, &w.bits, kSmallPps);
    if (scan_all(w.view()).pictures)
        return 600;
    if (hevc_parameter_sets_resolve(&g_state.sets, 0) != HevcError::none ||
        g_state.sets.active.sps.width != 64 || g_state.sets.active.sps.height != 48 ||
        g_state.sets.active.sps.max_buffering != 2)
        return 601;
    // A repeated SPS with the same id and new geometry replaces the stored one;
    // the cached chain is reparsed.
    const u32 stamp = g_state.sets.active_sps;
    w.clear();
    Sps wide = kSmall;
    wide.width = 96;
    write_hevc_sps(&w.out, &w.bits, wide);
    scan_all(w.view());
    if (hevc_parameter_sets_resolve(&g_state.sets, 0) != HevcError::none ||
        g_state.sets.active.sps.width != 96 || g_state.sets.active_sps == stamp)
        return 602;
    // A re-sent PPS reparses only the PPS: the active SPS keeps its parse and stamp.
    const u32 sps_stamp = g_state.sets.active_sps, pps_stamp = g_state.sets.active_pps;
    w.clear();
    write_hevc_pps(&w.out, &w.bits, kSmallPps);
    scan_all(w.view());
    if (hevc_parameter_sets_resolve(&g_state.sets, 0) != HevcError::none ||
        g_state.sets.active_sps != sps_stamp || g_state.sets.active_pps == pps_stamp ||
        g_state.sets.active.sps.width != 96)
        return 606;
    // A PPS naming an absent SPS, and an unknown PPS id.
    w.clear();
    write_hevc_pps(&w.out, &w.bits, {5, 9});
    scan_all(w.view());
    if (hevc_parameter_sets_resolve(&g_state.sets, 5) != HevcError::malformed ||
        hevc_parameter_sets_resolve(&g_state.sets, 63) != HevcError::malformed)
        return 604;
    // Main10 is refused when stored: the old set of that id is no longer trusted.
    w.clear();
    Sps ten = kSmall;
    ten.bit_depth = 10;
    write_hevc_sps(&w.out, &w.bits, ten);
    scan_all(w.view());
    if (g_state.sets.last_sps_error != HevcError::unsupported ||
        hevc_parameter_sets_resolve(&g_state.sets, 0) != HevcError::unsupported)
        return 603;
    // Ids beyond the slot count evict the least recently stored set.
    for (u8 id = 0; id < kHevcStreamSpsSlots + 1; ++id) {
        w.clear();
        Sps sps = kSmall;
        sps.id = id;
        write_hevc_sps(&w.out, &w.bits, sps);
        scan_all(w.view());
    }
    u32 valid = 0;
    for (const auto& slot : g_state.sets.sps)
        valid += slot.valid && slot.id != 0;
    if (valid != kHevcStreamSpsSlots)
        return 605;
    return 0;
}

int scan_tests() noexcept {
    Writer w;
    g_state = {};
    hevc_stream_reset(&g_state);
    // IDR with parameter sets, one slice segment per CTU, then a P picture,
    // in one input: two pictures in bitstream order.
    write_idr(&w.out, &w.bits, kSmall, kSmallPps, 1, 1);
    write_picture(&w.out, &w.bits, kSmall, kSmallPps, kTrailR, 1, 0);
    auto scanner = hevc_stream_scanner(w.view());
    if (hevc_stream_scan(&g_state.sets, &scanner, &g_picture) != HevcScanStatus::picture ||
        g_picture.slice_count != ctb_count(kSmall) || !g_picture.header.idr || !g_picture.intra ||
        g_picture.last_address != ctb_count(kSmall) - 1)
        return 610;
    if (hevc_stream_scan(&g_state.sets, &scanner, &g_picture) != HevcScanStatus::picture ||
        g_picture.slice_count != 1 || g_picture.header.irap || g_picture.intra ||
        g_picture.header.poc_lsb != 1 ||
        hevc_stream_scan(&g_state.sets, &scanner, &g_picture) != HevcScanStatus::end)
        return 611;
    // Lost leading segments: the picture is damaged, the next one is intact.
    w.clear();
    write_slice(&w.out, &w.bits, kSmall, kSmallPps, {kTrailR, 2, 4, 2, 0});
    write_picture(&w.out, &w.bits, kSmall, kSmallPps, kTrailR, 3, 0);
    auto scanned = scan_all(w.view());
    if (scanned.pictures != 2 || scanned.damaged != 1 || scanned.error != HevcError::truncated)
        return 612;
    // Segments out of address order, and a reserved VCL type.
    w.clear();
    write_slice(&w.out, &w.bits, kSmall, kSmallPps, {kTrailR, 0, 4, 4, 0});
    write_slice(&w.out, &w.bits, kSmall, kSmallPps, {kTrailR, 8, 4, 4, 0});
    write_slice(&w.out, &w.bits, kSmall, kSmallPps, {kTrailR, 4, 4, 4, 0});
    scanned = scan_all(w.view());
    if (scanned.pictures != 1 || scanned.damaged != 1 || scanned.error != HevcError::malformed)
        return 613;
    w.clear();
    write_picture(&w.out, &w.bits, kSmall, kSmallPps, 22, 5, 0);
    scanned = scan_all(w.view());
    if (scanned.pictures != 1 || scanned.damaged != 1 || scanned.error != HevcError::unsupported)
        return 614;
    // A slice whose PPS was never received.
    w.clear();
    write_picture(&w.out, &w.bits, kSmall, {7, 0}, kTrailR, 6, 0);
    scanned = scan_all(w.view());
    if (scanned.pictures != 1 || scanned.damaged != 1)
        return 615;
    return 0;
}

int state_tests() noexcept {
    Writer w;
    g_state = {};
    hevc_stream_reset(&g_state);
    // P before any IRAP: dropped, keyframe wanted.
    write_hevc_sps(&w.out, &w.bits, kSmall);
    write_hevc_pps(&w.out, &w.bits, kSmallPps);
    write_picture(&w.out, &w.bits, kSmall, kSmallPps, kTrailR, 1, 0);
    if (decode(w.view(), 10).action != HevcStreamAction::request_keyframe || g_state.dropped != 1)
        return 620;
    // IDR then P pictures: presented at once (no reordering), surfaces sized
    // to the DPB plus the presented and held slices.
    w.clear();
    write_idr(&w.out, &w.bits, kSmall, kSmallPps, 1);
    auto d = decode(w.view(), 100);
    if (d.action != HevcStreamAction::decode || d.reinitialized != 1 || d.presented != 1 ||
        g_state.surfaces != kSmall.max_buffering + kVideoStreamPresentationSurfaces)
        return 621;
    u32 used = 1u << d.surface;
    for (u16 poc = 1; poc <= 3; ++poc) {
        w.clear();
        write_picture(&w.out, &w.bits, kSmall, kSmallPps, kTrailR, poc, 0);
        g_state.held = 1u << d.surface; // The GPU layer presents the previous picture.
        const u8 previous = d.surface;
        d = decode(w.view(), 100u + poc);
        if (d.action != HevcStreamAction::decode || d.presented != 1 || d.surface == previous)
            return 622;
        used |= 1u << d.surface;
    }
    if (g_state.decoded != 4 || used >= 1u << g_state.surfaces)
        return 623;
    // Held slices are never chosen: hold every free slice but the highest.
    w.clear();
    write_picture(&w.out, &w.bits, kSmall, kSmallPps, kTrailR, 4, 0);
    const u32 free = ((1u << g_state.surfaces) - 1u) & ~hevc_reference_surfaces(g_state.references);
    u32 expected = free;
    while (expected & (expected - 1u))
        expected &= expected - 1u;
    g_state.held = free & ~expected;
    d = decode(w.view(), 104);
    if (d.action != HevcStreamAction::decode || (1u << d.surface) != expected)
        return 624;
    g_state.held = 0;
    // A lost reference (POC 5 missing): no decode, one gap, a keyframe request;
    // later pictures wait for the next IRAP.
    w.clear();
    write_picture(&w.out, &w.bits, kSmall, kSmallPps, kTrailR, 6, 0);
    if (decode(w.view(), 106).action != HevcStreamAction::request_keyframe || g_state.gaps != 1 ||
        !g_state.waiting)
        return 625;
    w.clear();
    write_picture(&w.out, &w.bits, kSmall, kSmallPps, kTrailR, 7, 0);
    if (decode(w.view(), 107).action != HevcStreamAction::request_keyframe)
        return 626;
    // Resolution change at the next IDR (a resized window): new surfaces.
    w.clear();
    Sps resized = kSmall;
    resized.width = 128;
    resized.height = 80;
    resized.max_buffering = 3;
    write_idr(&w.out, &w.bits, resized, kSmallPps, 2);
    d = decode(w.view(), 200);
    if (d.action != HevcStreamAction::decode || d.reinitialized != 1 || g_state.coded_width != 128 ||
        g_state.surfaces != 3 + kVideoStreamPresentationSurfaces || g_state.waiting)
        return 627;
    // Same geometry at the next IDR keeps the surfaces.
    w.clear();
    write_idr(&w.out, &w.bits, resized, kSmallPps, 3);
    if (decode(w.view(), 300).reinitialized)
        return 628;
    // A non-IRAP picture whose PPS switches to an SPS of other geometry.
    w.clear();
    write_hevc_sps(&w.out, &w.bits, {1, 64, 64, 0, 4, 8, 2});
    write_hevc_pps(&w.out, &w.bits, {1, 1});
    write_picture(&w.out, &w.bits, {1, 64, 64, 0, 4, 8, 2}, {1, 1}, kTrailR, 1, 0);
    if (decode(w.view(), 301).action != HevcStreamAction::request_keyframe || !g_state.waiting)
        return 629;
    // Decoding starting at a CRA skips its RASL pictures silently.
    w.clear();
    write_idr(&w.out, &w.bits, kSmall, kSmallPps, 4, 0, kCra);
    d = decode(w.view(), 400);
    if (d.action != HevcStreamAction::decode || !g_state.skip_rasl)
        return 630;
    w.clear();
    write_picture(&w.out, &w.bits, kSmall, kSmallPps, kRaslN, 255, 0);
    const u32 requests_before = g_state.dropped;
    if (decode(w.view(), 401).action != HevcStreamAction::skip || g_state.dropped != requests_before)
        return 631;
    w.clear();
    write_picture(&w.out, &w.bits, kSmall, kSmallPps, kTrailR, 1, 0);
    if (decode(w.view(), 402).action != HevcStreamAction::decode || g_state.skip_rasl)
        return 632;
    return 0;
}

} // namespace

int hevc_stream_tests() noexcept {
    int status = parameter_tests();
    if (!status)
        status = scan_tests();
    if (!status)
        status = state_tests();
    return status;
}

} // namespace maicord
