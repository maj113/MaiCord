#include "maicord/media/h264_stream.hpp"
#include "maicord/media/video_display.hpp"
#include "maicord/core/memory.hpp"
#include "h264_bits.hpp"

namespace maicord {
namespace {

bool read_sps_id(ConstSlice<u8> nal, u8* id) noexcept {
    if (!nal.data || nal.count < 2) return false;
    H264Bits bits{{nal.data + 1, nal.count - 1}};
    bits.read(24);
    const u32 value = bits.ue(31);
    if (bits.error != H264Error::none) return false;
    *id = static_cast<u8>(value);
    return true;
}

template<class Slot, u32 Count>
Slot* select_slot(Slot (&slots)[Count], u8 id, bool (*matches)(const Slot&, u8)) noexcept {
    Slot* free_slot = nullptr;
    Slot* oldest = &slots[0];
    for (auto& slot : slots) {
        if (slot.valid && matches(slot, id)) return &slot;
        if (!slot.valid && !free_slot) free_slot = &slot;
        if (slot.stored < oldest->stored) oldest = &slot;
    }
    return free_slot ? free_slot : oldest;
}
bool sps_matches(const H264StreamSps& slot, u8 id) noexcept { return slot.parsed.id == id; }
bool pps_matches(const H264StreamPps& slot, u8 id) noexcept { return slot.id == id; }

u32 reference_mask(const H264ReferenceState& references) noexcept {
    u32 mask = 0;
    for (u32 i = 0; i < references.count && i < 16; ++i)
        if (references.pictures[i].surface < 32) mask |= 1u << references.pictures[i].surface;
    return mask;
}

bool stream_supported(const H264Sps& sps) noexcept {
    return sps.coded_width && sps.coded_height && sps.coded_width <= kH264StreamMaximumDimension &&
           sps.coded_height <= kH264StreamMaximumDimension &&
           u64{sps.coded_width} * sps.coded_height <= kH264StreamMaximumPixels && !sps.transform_bypass &&
           sps.max_references <= 16 &&
           video_display_extent(sps.width, sps.height, sps.sar_width, sps.sar_height).width != 0;
}

u8 reference_frames(const H264Sps& sps) noexcept {
    return sps.max_references ? sps.max_references : 1;
}

} // namespace

H264Nal h264_annexb_next(H264AnnexBCursor* cursor) noexcept {
    if (!cursor || (!cursor->bytes.data && cursor->bytes.count) || cursor->offset > cursor->bytes.count)
        return {{}, H264Error::invalid_input};
    const u8* data = cursor->bytes.data;
    const u32 count = cursor->bytes.count;
    for (;;) {
        // Start code prefix: two or more zero bytes then 0x01.
        u32 i = cursor->offset, zeroes = 0;
        bool found = false, garbage = false;
        while (i < count) {
            const u8 value = data[i++];
            if (!value) { ++zeroes; continue; }
            if (value == 1 && zeroes >= 2) { found = true; break; }
            garbage = true;
            zeroes = 0;
        }
        if (!found) {
            cursor->offset = count;
            return {{}, garbage ? H264Error::malformed : H264Error::end};
        }
        if (garbage) {
            // Bytes before the prefix belong to no NAL: report them once, then
            // resume exactly at the 00 00 01 that follows.
            cursor->offset = i - 3u;
            return {{}, H264Error::malformed};
        }
        const u32 start = i;
        u32 end = count;
        // 00 00 00 and 00 00 01 never occur inside a NAL (emulation prevention);
        // a third byte above 1 rules out a pattern at any of the three offsets.
        for (u32 j = start; j + 2u < count;) {
            const u8 third = data[j + 2u];
            if (third > 1) { j += 3; continue; }
            if (!data[j + 1u] && !data[j]) { end = j; break; }
            ++j;
        }
        u32 stop = end;
        while (stop > start && !data[stop - 1u]) --stop; // trailing_zero_8bits
        cursor->offset = end;
        if (stop == start) {
            if (end == count) return {{}, H264Error::end};
            continue;
        }
        const u8 header = data[start];
        H264Nal nal{{data + start, stop - start}, H264Error::none, static_cast<u8>(header & 31u),
                    static_cast<u8>((header >> 5u) & 3u)};
        if (header & 0x80u) nal.error = H264Error::malformed;
        return nal;
    }
}

H264Error h264_parameter_sets_store(H264ParameterSets* sets, ConstSlice<u8> nal) noexcept {
    if (!sets || !nal.data || nal.count < 2) return H264Error::invalid_input;
    const u8 type = nal.data[0] & 31u;
    if (type == 7) {
        H264Sps sps{};
        const H264Error error = h264_parse_sps(nal, &sps);
        if (error != H264Error::none) {
            u8 id = 0;
            if (read_sps_id(nal, &id))
                for (auto& slot : sets->sps)
                    if (slot.valid && slot.parsed.id == id) slot.valid = false;
            sets->last_sps_error = error;
            return error;
        }
        auto* slot = select_slot(sets->sps, sps.id, sps_matches);
        slot->parsed = sps;
        slot->stored = ++sets->clock;
        slot->valid = true;
        return H264Error::none;
    }
    if (type != 8) return H264Error::invalid_input;
    H264Bits bits{{nal.data + 1, nal.count - 1}};
    const u32 id = bits.ue(255), sps_id = bits.ue(31);
    if (bits.error != H264Error::none) return bits.error;
    auto* slot = select_slot(sets->pps, static_cast<u8>(id), pps_matches);
    if (nal.count > kH264StreamMaximumPpsBytes) {
        if (slot->valid && slot->id == id) slot->valid = false;
        return H264Error::unsupported;
    }
    copy_bytes(slot->raw, nal.data, nal.count);
    slot->bytes = nal.count;
    slot->id = static_cast<u8>(id);
    slot->sps_id = static_cast<u8>(sps_id);
    slot->stored = ++sets->clock;
    slot->valid = true;
    return H264Error::none;
}

H264Error h264_parameter_sets_resolve(const H264ParameterSets& sets, u8 pps_id, H264Sps* sps,
                                      H264Pps* pps) noexcept {
    if (!sps || !pps) return H264Error::invalid_input;
    const H264StreamPps* picture = nullptr;
    for (const auto& slot : sets.pps)
        if (slot.valid && slot.id == pps_id) picture = &slot;
    if (!picture) return H264Error::malformed;
    const H264StreamSps* sequence = nullptr;
    for (const auto& slot : sets.sps)
        if (slot.valid && slot.parsed.id == picture->sps_id) sequence = &slot;
    if (!sequence) return sets.last_sps_error == H264Error::unsupported ? H264Error::unsupported
                                                                        : H264Error::malformed;
    H264Pps parsed{};
    const H264Error error = h264_parse_pps({picture->raw, picture->bytes}, sequence->parsed, &parsed);
    if (error != H264Error::none) return error;
    *sps = sequence->parsed;
    *pps = parsed;
    return H264Error::none;
}

bool h264_sps_decode_compatible(const H264Sps& a, const H264Sps& b) noexcept {
    if (a.id != b.id || a.coded_width != b.coded_width || a.coded_height != b.coded_height ||
        a.frame_num_bits != b.frame_num_bits || a.poc_type != b.poc_type || a.poc_bits != b.poc_bits ||
        a.max_references != b.max_references || a.frame_only != b.frame_only || a.mbaff != b.mbaff ||
        a.direct_8x8 != b.direct_8x8 || a.delta_always_zero != b.delta_always_zero || a.gaps != b.gaps ||
        a.transform_bypass != b.transform_bypass || a.scaling_present != b.scaling_present ||
        a.offset_non_reference != b.offset_non_reference ||
        a.offset_top_to_bottom != b.offset_top_to_bottom || a.cycle_count != b.cycle_count ||
        (a.profile == 100) != (b.profile == 100))
        return false;
    return bytes_equal(&a.scaling, &b.scaling, sizeof(a.scaling)) &&
           bytes_equal(a.cycle_offsets, b.cycle_offsets, a.cycle_count * u32{sizeof(i32)});
}

H264ScanStatus h264_stream_scan(H264ParameterSets* sets, H264StreamScanner* scanner,
                                H264StreamPicture* picture) noexcept {
    if (!sets || !scanner || !picture) return H264ScanStatus::end;
    picture->slice_count = 0;
    picture->bitstream_bytes = 0;
    picture->all_intra = true;
    picture->error = H264Error::none;
    const u8* base = scanner->cursor.bytes.data;
    bool vcl = false, damaged = false;
    u32 last_mb = 0;
    const auto damage = [&](H264Error error) noexcept {
        if (!damaged) picture->error = error == H264Error::none ? H264Error::malformed : error;
        damaged = true;
    };
    const auto result = [&]() noexcept { return damaged ? H264ScanStatus::damaged : H264ScanStatus::picture; };
    for (;;) {
        H264Nal nal{};
        if (scanner->has_pending) {
            nal = scanner->pending;
            scanner->has_pending = false;
        } else nal = h264_annexb_next(&scanner->cursor);
        if (nal.error == H264Error::end) break;
        if (nal.error == H264Error::invalid_input) { damage(nal.error); vcl = true; break; }
        if (nal.error != H264Error::none) {
            // Corrupt NAL of unknown role: whatever picture it touches is lost.
            damage(nal.error);
            vcl = true;
            continue;
        }
        switch (nal.type) {
        case 1:
        case 5: {
            u8 id = 0;
            H264Error error = h264_slice_parameter_id(nal.bytes, &id);
            H264Slice slice{};
            H264Sps sps_local;
            H264Pps pps_local;
            const H264Sps* sps = &picture->sps;
            const H264Pps* pps = &picture->pps;
            if (error == H264Error::none && (!vcl || damaged || id != picture->pps.id)) {
                error = h264_parameter_sets_resolve(*sets, id, &sps_local, &pps_local);
                sps = &sps_local;
                pps = &pps_local;
            }
            if (error == H264Error::none) error = h264_parse_slice(nal.bytes, *sps, *pps, &slice);
            if (vcl && error == H264Error::none &&
                (damaged ? slice.first_mb == 0
                         : (id != picture->pps.id || slice.first_mb <= last_mb ||
                            !h264_same_picture(picture->first, slice)))) {
                scanner->pending = nal;
                scanner->has_pending = true;
                return result();
            }
            if (error != H264Error::none) {
                damage(error);
                vcl = true;
                break;
            }
            if (!vcl) {
                if (sps != &picture->sps) {
                    picture->sps = *sps;
                    picture->pps = *pps;
                }
                picture->first = slice;
                // Leading slices were lost: concealment would need references
                // the decoder cannot prove, so the picture is dropped.
                if (slice.first_mb) damage(H264Error::truncated);
            }
            vcl = true;
            last_mb = slice.first_mb;
            if (damaged) break;
            if (picture->slice_count == kH264StreamMaximumSlices ||
                nal.bytes.count > ~0u - 3u - picture->bitstream_bytes) {
                damage(H264Error::unsupported);
                break;
            }
            picture->slices[picture->slice_count++] = {static_cast<u32>(nal.bytes.data - base), nal.bytes.count};
            picture->bitstream_bytes += nal.bytes.count + 3u;
            picture->all_intra &= slice.type == H264SliceType::i;
            break;
        }
        case 2:
        case 3:
        case 4: // Data partitioning (Extended profile) is not decoded.
            damage(H264Error::unsupported);
            vcl = true;
            break;
        case 6:
        case 7:
        case 8:
        case 9:
        case 13:
        case 14:
        case 15:
        case 16:
        case 17:
        case 18:
            if (vcl) {
                scanner->pending = nal;
                scanner->has_pending = true;
                return result();
            }
            if (nal.type == 7 || nal.type == 8) h264_parameter_sets_store(sets, nal.bytes);
            break;
        case 10:
        case 11:
            if (vcl) return result();
            break;
        default: // Filler, auxiliary/extension slices and unspecified types.
            break;
        }
    }
    return vcl ? result() : H264ScanStatus::end;
}

void h264_stream_reset(H264StreamState* state) noexcept {
    if (!state) return;
    state->references = {};
    state->output = {};
    state->output.leased = 255;
    state->waiting = true;
    state->drain = false;
    state->output_started = false;
}

void h264_stream_fail(H264StreamState* state) noexcept {
    if (!state) return;
    h264_stream_reset(state);
    ++state->resets;
}

void h264_stream_lost(H264StreamState* state) noexcept {
    if (!state) return;
    h264_stream_reset(state);
    ++state->dropped;
}

H264StreamAction h264_stream_prepare(H264StreamState* state, const H264StreamPicture& picture,
                                     H264StreamDecode* decode) noexcept {
    if (!state || !decode || !picture.slice_count) return H264StreamAction::request_keyframe;
    H264StreamDecode next{};
    const H264Slice& first = picture.first;
    const H264Sps& sps = picture.sps;
    bool reset = first.idr;
    for (u32 i = 0; i < first.marking_count && i < 32; ++i) reset |= first.marking[i].operation == 5;
    if (first.idr) {
        if (!stream_supported(sps)) {
            state->unsupported = true;
            h264_stream_lost(state);
            return H264StreamAction::unsupported;
        }
        const u32 limit = sps.restriction ? minimum<u32>(sps.max_reorder, 16) : kH264StreamAdaptiveReorder;
        next.surfaces_needed =
            static_cast<u8>(minimum<u32>(kH264StreamMaximumSurfaces, reference_frames(sps) + limit + 1u));
        next.reinitialize = !state->surfaces || !state->active || sps.coded_width != state->sps.coded_width ||
                            sps.coded_height != state->sps.coded_height ||
                            next.surfaces_needed > state->surfaces;
    } else {
        if (state->waiting || !state->active) {
            ++state->dropped;
            return H264StreamAction::request_keyframe;
        }
        if (!h264_sps_decode_compatible(state->sps, sps)) {
            h264_stream_lost(state);
            return H264StreamAction::request_keyframe;
        }
        const u32 modulus = 1u << sps.frame_num_bits;
        const u32 expected = (state->references.last_reference_frame + 1u) % modulus;
        if (first.frame_num != state->references.last_reference_frame && first.frame_num != expected) {
            // A reference picture was lost (H.264 8.2.5.2). Concealing it with
            // "non-existing" frames would show corrupt motion until the next
            // IDR anyway, so freeze on the last good frame and ask for one.
            ++state->gaps;
            h264_stream_lost(state);
            return H264StreamAction::request_keyframe;
        }
        next.surfaces_needed = state->surfaces;
    }
    const u8 capacity = next.reinitialize ? next.surfaces_needed : state->surfaces;
    state->output.leased = 255;
    const u8 surface = h264_free_surface(state->output, first.idr ? 0 : reference_mask(state->references),
                                         capacity);
    if (surface == 255) {
        h264_stream_fail(state);
        return H264StreamAction::request_keyframe;
    }
    const H264Error error = h264_reference_plan(state->references, sps, first, surface, &next.plan);
    if (error != H264Error::none) {
        if (first.idr) {
            ++state->damaged;
            h264_stream_reset(state);
        } else h264_stream_lost(state);
        return H264StreamAction::request_keyframe;
    }
    if (reset && state->output.count) {
        if (first.idr && first.discard_prior) {
            state->output = {};
            state->output.leased = 255;
        } else state->drain = true;
    }
    next.surface = surface;
    *decode = next;
    return H264StreamAction::decode;
}

void h264_stream_set_surfaces(H264StreamState* state, u8 surfaces) noexcept {
    if (state) state->surfaces = surfaces > 32 ? 32 : surfaces;
}

void h264_stream_commit(H264StreamState* state, const H264StreamPicture& picture,
                        const H264StreamDecode& decode, u32 rtp_timestamp) noexcept {
    if (!state || decode.surface >= 32) return;
    const H264Slice& first = picture.first;
    if (first.idr) {
        const H264Sps& sps = picture.sps;
        state->sps = sps;
        state->active = true;
        state->waiting = false;
        state->unsupported = false;
        const u32 surfaces = state->surfaces ? state->surfaces : decode.surfaces_needed;
        const u32 used = reference_frames(sps) + 1u;
        const u32 room = surfaces > used ? surfaces - used : 0;
        const u32 limit = sps.restriction ? minimum<u32>(sps.max_reorder, 16) : kH264StreamAdaptiveReorder;
        state->reorder_limit = static_cast<u8>(minimum(limit, room));
        // Declared reordering is honoured immediately; otherwise start at zero
        // latency and grow only if the stream proves to reorder.
        state->reorder = sps.restriction ? state->reorder_limit : 0;
    }
    state->references = decode.plan.after;
    if (decode.plan.reset) state->output_started = false;
    const auto& order = decode.plan.retained_order;
    state->output.leased = 255;
    if (!h264_output_push(&state->output,
                          {0, order.top < order.bottom ? order.top : order.bottom, decode.surface, 0})) {
        h264_stream_fail(state);
        return;
    }
    state->info[decode.surface] = {rtp_timestamp, first.idr};
    ++state->decoded;
}

bool h264_stream_pop(H264StreamState* state, H264StreamOutput* output) noexcept {
    if (!state || !output) return false;
    while (state->output.count) {
        if (!state->drain && state->output.count <= state->reorder) return false;
        H264Output picture{};
        state->output.leased = 255;
        if (!h264_output_pop(&state->output, &picture)) break;
        state->output.leased = 255;
        if (state->output_started && picture.poc < state->last_output_poc) {
            ++state->late;
            if (state->reorder < state->reorder_limit) ++state->reorder;
            continue;
        }
        state->output_started = true;
        state->last_output_poc = picture.poc;
        const auto& info = state->info[picture.surface & 31u];
        *output = {picture.poc, info.rtp_timestamp, picture.surface, info.keyframe};
        return true;
    }
    state->drain = false;
    return false;
}

} // namespace maicord
