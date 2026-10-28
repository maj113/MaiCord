#include "maicord/media/hevc_stream.hpp"
#include "maicord/media/gpu_video_frame.hpp"
#include "maicord/media/video_display.hpp"
#include "maicord/core/memory.hpp"
#include "hevc_bits.hpp"

namespace maicord {
namespace {

constexpr u8 kEos = 36, kEob = 37, kPrefixSei = 39;

u8 nal_type(ConstSlice<u8> nal) noexcept {
    return static_cast<u8>((nal[0] >> 1) & 63u);
}

template <class Slot, u32 Count> Slot* select_slot(Slot (&slots)[Count], u8 id) noexcept {
    Slot* free_slot = nullptr;
    Slot* oldest = &slots[0];
    for (auto& slot : slots) {
        if (slot.valid && slot.id == id)
            return &slot;
        if (!slot.valid && !free_slot)
            free_slot = &slot;
        if (slot.stored < oldest->stored)
            oldest = &slot;
    }
    return free_slot ? free_slot : oldest;
}

template <class Slot, u32 Count> void invalidate(Slot (&slots)[Count], u8 id) noexcept {
    for (auto& slot : slots)
        if (slot.valid && slot.id == id)
            slot.valid = false;
}

// first_slice_segment_in_pic_flag and slice_pic_parameter_set_id.
bool slice_start(ConstSlice<u8> nal, u8 type, bool* first, u8* pps_id) noexcept {
    auto b = hevc_payload(nal, type);
    *first = b.bit();
    if (type >= 16 && type <= 23)
        b.bit();
    *pps_id = static_cast<u8>(b.ue(63));
    return b.error == HevcError::none;
}

bool stream_supported(const HevcSps& sps) noexcept {
    return sps.coded_width <= 4096 && sps.coded_height <= 4096 && sps.max_buffering &&
           sps.max_buffering <= 15 && sps.max_reorder < sps.max_buffering &&
           video_display_extent(sps.width, sps.height, sps.sar_width, sps.sar_height).width != 0;
}

} // namespace

HevcError hevc_parameter_sets_store(HevcParameterSets* sets, ConstSlice<u8> nal) noexcept {
    if (!sets || !nal.data || nal.count < 3)
        return HevcError::invalid_input;
    const u8 type = nal_type(nal);
    if (type == 33) {
        u8 id = 0;
        HevcError error = hevc_sps_id(nal, &id);
        if (error != HevcError::none)
            return sets->last_sps_error = error;
        // Validated now so an unsupported stream (Main10, 4:4:4) is known at
        // once; the chain is parsed again when a picture activates it.
        HevcSps parsed;
        error =
            nal.count > kHevcStreamMaximumSpsBytes ? HevcError::unsupported : hevc_parse_sps(nal, &parsed);
        if (error != HevcError::none) {
            invalidate(sets->sps, id);
            return sets->last_sps_error = error;
        }
        auto* slot = select_slot(sets->sps, id);
        copy_bytes(slot->raw, nal.data, nal.count);
        slot->bytes = nal.count;
        slot->id = id;
        slot->stored = ++sets->clock;
        slot->valid = true;
        sets->last_sps_error = HevcError::none;
        return HevcError::none;
    }
    if (type != 34)
        return HevcError::invalid_input;
    auto b = hevc_payload(nal, 34);
    const u32 id = b.ue(63), sps_id = b.ue(15);
    if (b.error != HevcError::none)
        return b.error;
    if (nal.count > kHevcStreamMaximumPpsBytes) {
        invalidate(sets->pps, static_cast<u8>(id));
        return HevcError::unsupported;
    }
    auto* slot = select_slot(sets->pps, static_cast<u8>(id));
    copy_bytes(slot->raw, nal.data, nal.count);
    slot->bytes = nal.count;
    slot->id = static_cast<u8>(id);
    slot->sps_id = static_cast<u8>(sps_id);
    slot->stored = ++sets->clock;
    slot->valid = true;
    return HevcError::none;
}

HevcError hevc_parameter_sets_resolve(HevcParameterSets* sets, u8 pps_id) noexcept {
    if (!sets)
        return HevcError::invalid_input;
    const HevcStreamPps* picture = nullptr;
    for (const auto& slot : sets->pps)
        if (slot.valid && slot.id == pps_id)
            picture = &slot;
    if (!picture)
        return HevcError::malformed;
    const HevcStreamSps* sequence = nullptr;
    for (const auto& slot : sets->sps)
        if (slot.valid && slot.id == picture->sps_id)
            sequence = &slot;
    if (!sequence)
        return sets->last_sps_error == HevcError::unsupported ? HevcError::unsupported
                                                              : HevcError::malformed;
    if (sets->active_sps == sequence->stored && sets->active_pps == picture->stored)
        return HevcError::none;
    sets->active_sps = sets->active_pps = 0;
    auto& active = sets->active;
    HevcError error = hevc_parse_sps({sequence->raw, sequence->bytes}, &active.sps);
    if (error == HevcError::none)
        error = hevc_parse_pps({picture->raw, picture->bytes}, active.sps, &active.pps);
    if (error != HevcError::none)
        return error;
    sets->active_sps = sequence->stored;
    sets->active_pps = picture->stored;
    return HevcError::none;
}

HevcScanStatus hevc_stream_scan(HevcParameterSets* sets, HevcStreamScanner* scanner,
                                HevcStreamPicture* picture) noexcept {
    if (!sets || !scanner || !picture)
        return HevcScanStatus::end;
    picture->slice_count = 0;
    picture->bitstream_bytes = 0;
    picture->last_address = 0;
    picture->intra = true;
    picture->error = HevcError::none;
    const u8* base = scanner->cursor.bytes.data;
    bool vcl = false, damaged = false;
    const auto damage = [&](HevcError error) noexcept {
        if (!damaged)
            picture->error = error == HevcError::none ? HevcError::malformed : error;
        damaged = true;
        vcl = true;
    };
    const auto result = [&]() noexcept {
        return damaged ? HevcScanStatus::damaged : HevcScanStatus::picture;
    };
    for (;;) {
        H264Nal nal{};
        if (scanner->has_pending) {
            nal = scanner->pending;
            scanner->has_pending = false;
        } else
            nal = h264_annexb_next(&scanner->cursor);
        if (nal.error == H264Error::end)
            break;
        if (nal.error == H264Error::invalid_input) {
            damage(HevcError::invalid_input);
            break;
        }
        if (nal.error != H264Error::none || nal.bytes.count < 2 || !(nal.bytes[1] & 7u)) {
            damage(HevcError::malformed); // Corrupt NAL of unknown role.
            continue;
        }
        if (hevc_nal_layer(nal.bytes))
            continue; // Only the base layer is decoded.
        const u8 type = nal_type(nal.bytes);
        if (type <= 31) {
            bool first = false;
            u8 pps_id = 0;
            const bool started = nal.bytes.count > 2 && slice_start(nal.bytes, type, &first, &pps_id);
            if (vcl && started && first) {
                scanner->pending = nal;
                scanner->has_pending = true;
                return result();
            }
            if (damaged)
                continue;
            if (!started) {
                damage(HevcError::malformed);
                continue;
            }
            const bool leading = !vcl;
            if ((type >= 10 && type <= 15) || type >= 22) {
                damage(HevcError::unsupported); // Reserved VCL types.
                continue;
            }
            if (leading && !first) {
                damage(HevcError::truncated); // The picture's first segments were lost.
                continue;
            }
            HevcError error = HevcError::none;
            if (leading)
                error = hevc_parameter_sets_resolve(sets, pps_id);
            else if (pps_id != picture->header.pps_id)
                error = HevcError::malformed; // One PPS per picture.
            HevcSlice slice{};
            const HevcNal parsed{nal.bytes, HevcError::none, type,
                                 static_cast<u8>((nal.bytes[1] & 7u) - 1u)};
            if (error == HevcError::none && parsed.temporal_id > sets->active.sps.max_sub_layers)
                error = HevcError::unsupported;
            if (error == HevcError::none)
                error =
                    hevc_parse_slice(parsed, sets->active, leading ? nullptr : &picture->header, &slice);
            if (error == HevcError::none && !leading &&
                (slice.address <= picture->last_address ||
                 !hevc_picture_header_same(picture->header, slice.header)))
                error = HevcError::malformed;
            if (error == HevcError::none && (picture->slice_count == kHevcStreamMaximumSlices ||
                                             nal.bytes.count > ~0u - 3u - picture->bitstream_bytes))
                error = HevcError::unsupported;
            if (error != HevcError::none) {
                damage(error);
                continue;
            }
            if (leading)
                picture->header = slice.header;
            vcl = true;
            picture->last_address = slice.address;
            picture->intra &= slice.header.slice_type == 2;
            picture->slices[picture->slice_count++] = {static_cast<u32>(nal.bytes.data - base),
                                                       nal.bytes.count};
            picture->bitstream_bytes += nal.bytes.count + 3u;
            continue;
        }
        if (type == kEos || type == kEob) {
            if (vcl)
                return result();
            continue;
        }
        // VPS/SPS/PPS, AUD, prefix SEI and types 41-44 and 48-55 start the next
        // access unit; suffix SEI, filler data and the rest are skipped.
        const bool starts_unit = (type >= 32 && type <= 35) || type == kPrefixSei ||
                                 (type >= 41 && type <= 44) || (type >= 48 && type <= 55);
        if (!starts_unit)
            continue;
        if (vcl) {
            scanner->pending = nal;
            scanner->has_pending = true;
            return result();
        }
        if (type == 33 || type == 34)
            hevc_parameter_sets_store(sets, nal.bytes);
    }
    return vcl ? result() : HevcScanStatus::end;
}

void hevc_stream_reset(HevcStreamState* state) noexcept {
    if (!state)
        return;
    reset_value(&state->references);
    reset_value(&state->output);
    state->waiting = true;
    state->drain = false;
    state->skip_rasl = false;
}

void hevc_stream_lost(HevcStreamState* state) noexcept {
    if (!state)
        return;
    hevc_stream_reset(state);
    ++state->dropped;
}

HevcStreamAction hevc_stream_prepare(HevcStreamState* state, const HevcStreamPicture& picture,
                                     HevcStreamDecode* decode) noexcept {
    if (!state || !decode || !picture.slice_count)
        return HevcStreamAction::request_keyframe;
    const HevcPictureHeader& header = picture.header;
    const HevcSps& sps = state->sets.active.sps;
    const u8 type = header.nal_type;
    // Callers read *decode only after a decode action; it is built in place.
    reset_value(decode);
    HevcStreamDecode& next = *decode;
    if (header.irap) {
        if (!stream_supported(sps)) {
            state->unsupported = true;
            hevc_stream_lost(state);
            return HevcStreamAction::unsupported;
        }
        next.surfaces_needed = static_cast<u8>(sps.max_buffering + kVideoStreamPresentationSurfaces);
        next.reinitialize = !state->surfaces || !state->active || sps.coded_width != state->coded_width ||
                            sps.coded_height != state->coded_height ||
                            next.surfaces_needed > state->surfaces;
        // IDR and BLA always restart; a CRA restarts when decoding begins there.
        next.restart = header.idr || type <= 18 || state->waiting || !state->active || next.reinitialize;
    } else {
        if (state->waiting || !state->active) {
            ++state->dropped;
            return HevcStreamAction::request_keyframe;
        }
        if (type == 8 || type == 9) {
            if (state->skip_rasl)
                return HevcStreamAction::skip;
        } else if (type <= 5)
            state->skip_rasl = false; // Trailing pictures follow every leading one.
        if (sps.coded_width != state->coded_width || sps.coded_height != state->coded_height ||
            sps.max_buffering > state->max_buffering) {
            hevc_stream_lost(state);
            return HevcStreamAction::request_keyframe;
        }
        next.surfaces_needed = state->surfaces;
    }
    const u8 capacity = next.reinitialize ? next.surfaces_needed : state->surfaces;
    const HevcReferenceState none{};
    const HevcReferenceState& references = next.restart ? none : state->references;
    const u32 busy =
        (next.restart ? 0u : hevc_reference_surfaces(references)) | (next.reinitialize ? 0u : state->held);
    // Prior pictures are discarded, or drained before new surfaces replace theirs.
    const bool discard = next.restart && header.no_output_prior && (header.idr || type <= 18);
    u8 surface;
    if (discard || next.reinitialize) {
        const H264OutputQueue emptied{};
        surface = h264_free_surface(emptied, busy, capacity);
    } else
        surface = h264_free_surface(state->output, busy, capacity);
    if (surface == 255) {
        hevc_stream_reset(state);
        return HevcStreamAction::request_keyframe;
    }
    const HevcError error = hevc_reference_plan(references, sps, header, surface, &next.plan);
    if (error != HevcError::none) {
        // Missing references: freezing beats showing corrupt motion until the next IRAP.
        if (header.irap)
            ++state->damaged;
        else
            ++state->gaps;
        hevc_stream_lost(state);
        return HevcStreamAction::request_keyframe;
    }
    if (next.restart && state->output.count) {
        if (discard) {
            reset_value(&state->output);
        } else
            state->drain = true;
    }
    next.surface = surface;
    return HevcStreamAction::decode;
}

void hevc_stream_set_surfaces(HevcStreamState* state, u8 surfaces) noexcept {
    if (state)
        state->surfaces = surfaces > 32 ? 32 : surfaces;
}

void hevc_stream_commit(HevcStreamState* state, const HevcStreamPicture& picture,
                        const HevcStreamDecode& decode, u32 rtp_timestamp) noexcept {
    if (!state || decode.surface >= 32)
        return;
    const HevcPictureHeader& header = picture.header;
    if (header.irap) {
        const HevcSps& sps = state->sets.active.sps;
        state->coded_width = sps.coded_width;
        state->coded_height = sps.coded_height;
        state->max_buffering = sps.max_buffering;
        state->reorder = sps.max_reorder;
        state->active = true;
        state->waiting = false;
        state->unsupported = false;
        if (decode.restart)
            state->skip_rasl = !header.idr;
    }
    state->references = decode.plan.after;
    ++state->decoded;
    if (!header.output)
        return;
    if (!h264_output_push(&state->output, {0, decode.plan.poc, decode.surface, 0})) {
        hevc_stream_reset(state);
        return;
    }
    state->info[decode.surface] = {rtp_timestamp, header.irap};
}

bool hevc_stream_pop(HevcStreamState* state, H264StreamOutput* output) noexcept {
    if (!state || !output)
        return false;
    if (state->output.count && (state->drain || state->output.count > state->reorder)) {
        H264Output picture{};
        if (h264_output_pop(&state->output, &picture)) {
            state->output.leased = 255;
            const auto& info = state->info[picture.surface & 31u];
            *output = {picture.poc, info.rtp_timestamp, picture.surface, info.keyframe};
            return true;
        }
    }
    state->drain = false;
    return false;
}

} // namespace maicord
