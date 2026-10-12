#include "h264_state_probe.hpp"
#include "maicord/media/h264_references.hpp"
#include "maicord/media/mp4_demux.hpp"
#include "maicord/core/heap_sort.hpp"

namespace maicord {
namespace {
struct Output { u64 time; i32 poc; u32 epoch; };
struct DisplayOrder {
    bool operator()(const Output& a, const Output& b) const noexcept {
        return a.epoch < b.epoch || (a.epoch == b.epoch && a.poc < b.poc);
    }
};
}

bool h264_state_probe(ConstSlice<u8> encoded, void (*metric)(const char*, u64)) noexcept {
    Mp4Demuxer demux{};
    if (mp4_demux_initialize(&demux, encoded) != Mp4DemuxError::none || demux.sample_count > 4'096) return false;
    Output outputs[4'096]{}; // Validation only, not a retained playback queue.
    H264Sps sps{}; H264Pps pps{};
    H264ReferenceState state{};
    u32 counts[3]{}, slices = 0, marking_mask = 0, epoch = 0, maximum_references = 0;
    for (u32 index = 0; index < demux.sample_count; ++index) {
        const auto unit = mp4_demux_next(&demux);
        if (!unit.ok()) return false;
        H264NalCursor cursor{unit.bytes, 0, demux.nal_length_bytes};
        H264Slice first{};
        u32 count = 0, last_mb = 0;
        H264Error error = H264Error::none;
        for (;;) {
            const auto nal = h264_next_nal(&cursor);
            if (nal.error == H264Error::end) break;
            if (nal.error != H264Error::none) { error = nal.error; break; }
            if (nal.type == 6 || nal.type == 9 || nal.type == 12) continue;
            u8 id = 0;
            error = h264_slice_parameter_id(nal.bytes, &id);
            if (error != H264Error::none) break;
            if (!index && !count) error = h264_select_parameters(mp4_demux_avcc(demux), id, &sps, &pps);
            // Mid-stream configuration changes need a decoder/surface reset.
            if (id != pps.id) error = H264Error::unsupported;
            H264Slice slice{};
            if (error == H264Error::none) error = h264_parse_slice(nal.bytes, sps, pps, &slice);
            if (error != H264Error::none) break;
            if ((!count && slice.first_mb) || (count &&
                (slice.first_mb <= last_mb || !h264_same_picture(first, slice)))) {
                error = H264Error::unsupported; break;
            }
            if (!count) first = slice;
            last_mb = slice.first_mb; ++count;
        }
        H264ReferencePlan plan{};
        u32 busy = 0;
        for (u32 i = 0; i < state.count; ++i) busy |= 1u << state.pictures[i].surface;
        u8 surface = 0;
        while (surface < 31 && (busy & (1u << surface))) ++surface;
        if (!count && error == H264Error::none) error = H264Error::malformed;
        if (error == H264Error::none) error = h264_reference_plan(state, sps, first, surface, &plan);
        if (error != H264Error::none) {
            metric("h264_state_error", static_cast<u32>(error)); metric("h264_state_sample", index);
            metric("h264_state_frame_num", first.frame_num); metric("h264_state_slice_count", count);
            return false;
        }
        state = plan.after;
        maximum_references = state.count > maximum_references ? state.count : maximum_references;
        if (plan.reset) ++epoch;
        const auto order = plan.retained_order;
        outputs[index] = {unit.presentation_time_us, order.top < order.bottom ? order.top : order.bottom, epoch};
        ++counts[static_cast<u32>(first.type)]; slices += count;
        for (u32 i = 0; i < first.marking_count; ++i) marking_mask |= 1u << first.marking[i].operation;
    }
    heap_sort(outputs, demux.sample_count, DisplayOrder{});
    for (u32 i = 1; i < demux.sample_count; ++i) {
        if (outputs[i].time < outputs[i - 1].time || (outputs[i].epoch == outputs[i - 1].epoch &&
            outputs[i].poc == outputs[i - 1].poc)) {
            metric("h264_state_order_mismatch", i); return false;
        }
    }
    metric("h264_state_pictures", demux.sample_count);
    metric("h264_state_slices", slices);
    metric("h264_state_p_pictures", counts[0]); metric("h264_state_b_pictures", counts[1]); metric("h264_state_i_pictures", counts[2]);
    metric("h264_state_marking_mask", marking_mask);
    metric("h264_state_max_references", maximum_references);
    return true;
}

} // namespace maicord
