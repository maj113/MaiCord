#include "maicord/platform/decode/h264_gpu_decoder.hpp"

#include "../h264_gpu_configuration.hpp"
#include "../software/software_decoder.hpp"
#include "../vaapi/va.hpp"
#include "../vaapi/va_file.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/media/containers/mp4_fragment_source.hpp"
#include "maicord/media/containers/mp4_segment_store.hpp"
#include "maicord/media/video/h264_output_queue.hpp"
#include "maicord/media/video/h264_references.hpp"
#include "maicord/media/video/video_display.hpp"
#include "maicord/platform/decode/mp4_decoder.hpp"
#include "stream.hpp"

// The H.264 movie decoder of the Linux build: what src/platform/decode/h264_gpu_decoder.cpp is on Windows (the
// h264_gpu_decoder.hpp interface that mp4_decoder.cpp drives), with the Direct3D 11 video decoder replaced by two tiers
// that write into the same kind of picture slots:
//
//   hardware  VA-API (src/platform/decode/vaapi): the picture and slice buffers of a picture come from the same
//             in-house parsers, the decoder writes a VASurface, and a picture that is output is exported as two
//             dma-buf planes and imported as the EGLImage-backed planes the renderer converts from
//   software  libopenh264 (src/platform/decode/software): the sample as an Annex B access unit, the decoded picture
//             copied into the CPU planes of its slot
//
// The container side is the Windows one: the demuxer (or the fragment source) gives access units with their
// presentation times, h264_reference_plan tracks the references and picture order counts, and the output queue
// (bumping by the SPS's reordering delay) decides which slot is shown next, so a movie with B pictures comes out in
// presentation order on both tiers. A slot is never decoded into while it is a reference, waits for output or is the
// lease of the picture last returned (until the next read or the caller's acknowledgement).
//
// The tier is chosen when the decoder is created: hardware when the driver has a profile for the stream and one real
// picture (the first IDR) decodes, exports and imports, software otherwise. A failure later in the stream is the
// stream's, as on Windows.

namespace maicord {
namespace {

constexpr u32 kSlotLimit = kStreamSurfaceLimit;
// Units whose pictures libopenh264 has not shown yet: far more than a DPB (16) holds.
constexpr u32 kUnitMetaCount = 64;

struct Meta {
    i64 presentation_time_us;
    u64 duration_us;
};

struct State {
    H264GpuPicture picture;
    Mp4Demuxer demux;
    Mp4FragmentSource* fragmented;
    i64 first_presentation;
    H264ReferenceState references;
    H264OutputQueue output;
    GpuDevice* device;
    vaapi::Session* session;     // the hardware tier
    SoftwareDecoder* software;   // the software tier
    u8* annexb;                  // the access unit as Annex B (software tier)
    u32 annexb_capacity;
    StreamSurface slot[kSlotLimit];
    u32 slots;
    GpuVideoFrame frame;
    VideoDisplayExtent display;
    usize allocation;
    i64 fragment_seek_target;
    u32 fragment_seek_candidate, fragment_seek_attempts;
    bool fragment_seek_pending, fragment_seek_start;
    // The software tier: the time of each unit in flight, and the counter that orders the output queue.
    Meta meta[kUnitMetaCount];
    u32 unit_ordinal;
    i32 order;
};

// ---- Fragment cursors --------------------------------------------------------------------------------------------

Mp4DemuxError seek_fragment(State* s, i64 target, Mp4SeekPoint* point, bool start) noexcept {
    auto& stream = *s->fragmented;
    Mp4FragmentRange range{};
    u32 selected = 0;
    if (stream.segments) {
        if (s->fragment_seek_pending && s->fragment_seek_target == target && s->fragment_seek_start == start)
            selected = s->fragment_seek_candidate;
        else {
            if (!start) {
                i64 empty{};
                if (target < 0 || !mp4_fragment_source_time(stream, stream.track.media_start, &empty))
                    return Mp4DemuxError::unsupported;
                const i64 relative = target - empty;
                const u64 absolute = relative < 0 ? static_cast<u64>(-relative) : static_cast<u64>(relative);
                if (absolute > 86400000000ull)
                    return Mp4DemuxError::unsupported;
                const u64 ticks = absolute / 1000000 * stream.track.timescale +
                                  (absolute % 1000000) * stream.track.timescale / 1000000;
                const u64 origin = static_cast<u64>(stream.track.media_start);
                if ((relative < 0 && ticks > origin) || ticks > 0x7fffffffffffffffull - origin)
                    return Mp4DemuxError::unsupported;
                u64 time = relative < 0 ? origin - ticks : origin + ticks;
                time = minimum(time, stream.index.first_time + stream.index.duration - 1);
                const auto found = mp4_fragment_source_find(&stream, time, &range);
                if (found != Mp4DemuxError::none)
                    return found;
                selected = range.index;
            }
            s->fragment_seek_target = target;
            s->fragment_seek_start = start;
            s->fragment_seek_pending = true;
            s->fragment_seek_candidate = selected;
            s->fragment_seek_attempts = 0;
        }
    } else {
        if (!mp4_fragment_index_range(stream.index, 0, &range))
            return Mp4DemuxError::malformed;
        if (!start) {
            i64 time{};
            if (!mp4_fragment_source_time(stream, static_cast<i64>(range.time), &time))
                return Mp4DemuxError::unsupported;
            if (time > target)
                return Mp4DemuxError::unsupported;
            for (u32 i = 1; i < stream.index.count; ++i) {
                if (!mp4_fragment_index_follow(stream.index, range, &range))
                    return Mp4DemuxError::malformed;
                if (!mp4_fragment_source_time(stream, static_cast<i64>(range.time), &time))
                    return Mp4DemuxError::unsupported;
                if (time > target)
                    break;
                selected = i;
            }
        }
    }
    // Index SAP flags are hints. Validate the real access unit against the active codec configuration before
    // replacing any cursor or references.
    for (u32 attempts = 0; attempts < 64; ++attempts) {
        if (stream.segments && s->fragment_seek_attempts == 64)
            break;
        Mp4FragmentPosition candidate{};
        Mp4AccessUnit unit{};
        auto error = mp4_fragment_source_prepare(&stream, selected, 0, &candidate);
        if (error != Mp4DemuxError::none)
            return error;
        error = mp4_fragment_position_unit(candidate, &unit);
        if (error != Mp4DemuxError::none) {
            mp4_fragment_source_cancel_prepare(&stream);
            return error;
        }
        if (start || unit.presentation_time_us_signed <= target) {
            H264Picture picture{};
            const auto parsed = h264_parse_picture(unit.bytes, s->picture.length_bytes, s->picture.sps, s->picture.pps,
                                                   &picture, stream.track.configuration);
            if (parsed != H264Error::none) {
                mp4_fragment_source_cancel_prepare(&stream);
                return parsed == H264Error::unsupported ? Mp4DemuxError::unsupported : Mp4DemuxError::malformed;
            }
            if (picture.first.idr) {
                if (!mp4_fragment_source_adopt(&stream, candidate))
                    return Mp4DemuxError::data_pending;
                s->fragment_seek_pending = false;
                *point = {unit.sample_index, unit.decode_time_us, unit.presentation_time_us_signed};
                return Mp4DemuxError::none;
            }
        }
        if (start || !selected)
            break;
        --selected;
        s->fragment_seek_candidate = selected;
        ++s->fragment_seek_attempts;
    }
    mp4_fragment_source_cancel_prepare(&stream);
    s->fragment_seek_pending = false;
    return Mp4DemuxError::unsupported;
}

// ---- Slots -------------------------------------------------------------------------------------------------------

void slots_release(State* s) noexcept {
    for (u32 i = 0; i < s->slots; ++i)
        stream_slot_release(&s->slot[i]);
    clear_bytes(s->slot, sizeof(s->slot));
    s->slots = 0;
}

// `count` CPU-backed pictures of width x height (both even).
bool slots_create_cpu(State* s, u32 count, u32 width, u32 height) noexcept {
    if (!count || count > kSlotLimit)
        return false;
    for (u32 i = 0; i < count; ++i) {
        StreamSurface& slot = s->slot[i];
        const GpuStatus luma = gpu_video_plane_create(s->device, GpuVideoPlaneFormat::r8, width, height,
                                                      &slot.luma_texture, &slot.luma_view, &slot.luma_pixels,
                                                      &slot.luma_pitch);
        s->slots = i + (gpu_ok(luma) ? 1u : 0u);
        const GpuStatus chroma = gpu_ok(luma) ? gpu_video_plane_create(s->device, GpuVideoPlaneFormat::rg8, width / 2,
                                                                        height / 2, &slot.chroma_texture,
                                                                        &slot.chroma_view, &slot.chroma_pixels,
                                                                        &slot.chroma_pitch)
                                              : luma;
        if (!gpu_ok(chroma)) {
            s->slots = i + 1u; // the luma plane made above is released with the rest
            slots_release(s);
            return false;
        }
    }
    s->slots = count;
    return true;
}

// ---- Tiers -------------------------------------------------------------------------------------------------------

bool decode_hardware(State* s, const H264ReferencePlan& plan, u8 surface) noexcept {
    const H264GpuPicture& picture = s->picture;
    return vaapi::submit_h264_sample(s->session, picture.sps, picture.pps, picture.header, picture.unit.bytes,
                                     picture.length_bytes, s->references, plan.decode_order, surface);
}

bool reserve_annexb(State* s, u32 bytes) noexcept {
    if (s->annexb_capacity >= bytes)
        return true;
    const u32 capacity = (bytes + 0xFFFFu) & ~0xFFFFu;
    auto* data = commit_pages<u8>(capacity);
    if (!data)
        return false;
    if (s->annexb)
        pages_release(s->annexb, s->annexb_capacity);
    s->annexb = data;
    s->annexb_capacity = capacity;
    return true;
}

void put_start_code(u8* out, u32* at) noexcept {
    out[(*at)++] = 0;
    out[(*at)++] = 0;
    out[(*at)++] = 0;
    out[(*at)++] = 1;
}

// The avcC parameter sets as Annex B NAL units; false when the record is not one.
bool put_parameter_sets(ConstSlice<u8> avcc, u8* out, u32* at) noexcept {
    if (avcc.count < 7 || avcc.data[0] != 1)
        return false;
    u32 offset = 5;
    for (u32 pass = 0; pass < 2; ++pass) {
        if (offset >= avcc.count)
            return false;
        const u32 count = pass ? avcc.data[offset] : (avcc.data[offset] & 31u);
        ++offset;
        for (u32 i = 0; i < count; ++i) {
            if (avcc.count - offset < 2)
                return false;
            const u32 length = (u32{avcc.data[offset]} << 8) | avcc.data[offset + 1];
            offset += 2;
            if (length > avcc.count - offset)
                return false;
            put_start_code(out, at);
            copy_bytes(out + *at, avcc.data + offset, length);
            *at += length;
            offset += length;
        }
    }
    return true;
}

// The sample as Annex B: the avcC parameter sets first when the picture is an IDR (libopenh264 has no other way to
// learn them), then each NAL unit behind a start code. False when it does not fit or does not parse.
bool annexb_unit(State* s, u32* bytes) noexcept {
    const H264GpuPicture& picture = s->picture;
    const ConstSlice<u8> avcc = mp4_demux_avcc(s->demux);
    // Each NAL unit trades a length prefix of 1 to 4 bytes for a start code of 4: at most twice the sample.
    if (!reserve_annexb(s, 2u * picture.unit.bytes.count + 2u * avcc.count + 64u))
        return false;
    u32 at = 0;
    if (picture.header.first.idr && !put_parameter_sets(avcc, s->annexb, &at))
        return false;
    H264NalCursor cursor{picture.unit.bytes, 0, picture.length_bytes};
    for (;;) {
        const H264Nal nal = h264_next_nal(&cursor);
        if (nal.error == H264Error::end)
            break;
        if (nal.error != H264Error::none)
            return false;
        put_start_code(s->annexb, &at);
        copy_bytes(s->annexb + at, nal.bytes.data, nal.bytes.count);
        at += nal.bytes.count;
    }
    *bytes = at;
    return at != 0;
}

// The picture libopenh264 completed, copied into a free slot and queued for output with the presentation time of the
// unit that carried it. libopenh264 shows pictures in presentation order, so the queue is a first in, first out line
// (increasing counts stand for picture order).
bool queue_software_picture(State* s, const SoftwarePicture& picture) noexcept {
    const u8 surface = h264_free_surface(s->output, 0, static_cast<u8>(s->slots));
    // The picture is the stream's visible extent, which the slots were made for.
    if (surface == 255 || picture.width != s->picture.sps.width || picture.height != s->picture.sps.height ||
        !s->slot[surface].luma_pixels)
        return false;
    stream_copy_picture(s->slot[surface], picture);
    const Meta& meta = s->meta[picture.timestamp % kUnitMetaCount];
    return h264_output_push(&s->output, {meta.duration_us, ++s->order, surface, meta.presentation_time_us});
}

// The end of the stream: every picture libopenh264 still holds, queued in presentation order. libopenh264 shows the
// last unit's picture before one its reorder buffer still holds, so the batch is ordered by the movie's times.
bool drain_software(State* s) noexcept {
    const u32 first = s->output.count;
    SoftwarePicture picture{};
    for (u32 held = 0; held < 40 && software_decoder_drain(s->software, &picture); ++held)
        if (!queue_software_picture(s, picture))
            return false;
    const i32 base = s->order - static_cast<i32>(s->output.count - first);
    for (u32 i = first; i < s->output.count; ++i) {
        i32 rank = 0;
        for (u32 j = first; j < s->output.count; ++j)
            rank += s->output.pictures[j].presentation_time_us < s->output.pictures[i].presentation_time_us;
        s->output.pictures[i].poc = base + 1 + rank;
    }
    return true;
}

// Decodes the unit: the pictures it completes join the queue. False on a sample the decoder refuses
// (probe->parse_error says when the stream is outside what it decodes).
bool decode_software(H264GpuDecoder* probe, State* s, const Mp4AccessUnit& unit) noexcept {
    u32 bytes = 0;
    if (!annexb_unit(s, &bytes)) {
        probe->parse_error = H264Error::malformed;
        return false;
    }
    const u32 ordinal = s->unit_ordinal++;
    s->meta[ordinal % kUnitMetaCount] = {unit.presentation_time_us_signed, unit.duration_us};
    const SoftwareStatus status = software_decoder_push(s->software, {s->annexb, bytes}, ordinal);
    if (status == SoftwareStatus::unsupported)
        probe->parse_error = H264Error::unsupported;
    SoftwarePicture picture{};
    return status == SoftwareStatus::ok && (!software_decoder_pop(s->software, &picture) || queue_software_picture(s, picture));
}

// The slot count of a decoder: the DPB plus the lease and the picture being decoded.
u32 surface_count(const H264Sps& sps) noexcept {
    return maximum<u32>(sps.max_references, h264_sps_dpb_frames(sps)) + 2u;
}

void release_tiers(State* s) noexcept {
    vaapi::session_destroy(s->session);
    s->session = nullptr;
    software_decoder_destroy(s->software);
    s->software = nullptr;
    slots_release(s);
    if (s->annexb)
        pages_release(s->annexb, s->annexb_capacity);
    s->annexb = nullptr;
    s->annexb_capacity = 0;
}

bool create_software(State* s) noexcept {
    const H264Sps& a = s->picture.sps;
    s->software = software_decoder_create(GpuStreamCodec::h264, true);
    if (!s->software)
        return false;
    if (!slots_create_cpu(s, surface_count(a), (a.width + 1u) & ~1u, (a.height + 1u) & ~1u)) {
        release_tiers(s);
        return false;
    }
    return true;
}

// One real picture through the hardware tier: decode, export, import. Whatever it learns is discarded.
bool probe_hardware(State* s) noexcept {
    H264ReferencePlan plan{};
    if (h264_reference_plan(s->references, s->picture.sps, s->picture.header.first, 0, &plan) != H264Error::none ||
        !decode_hardware(s, plan, 0))
        return false;
    StreamSurface exported{};
    if (!stream_slot_export(s->device, s->session, 0, &exported))
        return false;
    stream_slot_release(&exported);
    return true;
}

bool create_hardware(State* s) noexcept {
    const H264Sps& a = s->picture.sps;
    const vaapi::Driver* driver = vaapi::driver(s->device);
    if (!driver || !(driver->codecs & gpu_stream_codec_bit(GpuStreamCodec::h264)) || a.coded_width > driver->max_width[0] ||
        a.coded_height > driver->max_height[0])
        return false;
    const i32 profile = vaapi::h264_profile_for(*driver, a.profile);
    const u32 count = surface_count(a);
    if (!profile || count > vaapi::kSurfaceLimit)
        return false;
    s->session = vaapi::session_create(driver, profile, a.coded_width, a.coded_height, count);
    if (!s->session)
        return false;
    s->slots = count; // the planes are made when a picture is output
    if (!probe_hardware(s)) {
        release_tiers(s);
        return false;
    }
    return true;
}

// Fills the frame of the picture the output queue shows next. A hardware picture is exported now (drivers differ in
// when a surface's dma-buf is complete, and a fresh export is always right) into its slot.
Mp4DecodedFrame output_frame(State* s) noexcept {
    Mp4DecodedFrame decoded{};
    H264Output picture{};
    if (!h264_output_pop(&s->output, &picture)) {
        decoded.error = Mp4DecodeError::sample;
        return decoded;
    }
    StreamSurface& slot = s->slot[picture.surface];
    if (s->session && !stream_slot_export(s->device, s->session, picture.surface, &slot)) {
        decoded.error = Mp4DecodeError::sample;
        return decoded;
    }
    s->frame.texture = slot.luma_texture;
    s->frame.subresource = picture.surface;
    s->frame.luma = slot.luma_view;
    s->frame.chroma = slot.chroma_view;
    decoded.gpu = &s->frame;
    decoded.width = s->frame.width;
    decoded.height = s->frame.height;
    decoded.display_width = s->display.width;
    decoded.display_height = s->display.height;
    decoded.duration_us = decoded.source_duration_us = picture.duration_us;
    decoded.presentation_time_us = picture.presentation_time_us;
    return decoded;
}

u32 reference_mask(const State& s) noexcept {
    u32 mask = 0;
    for (u32 i = 0; i < s.references.count; ++i)
        mask |= 1u << s.references.pictures[i].surface;
    return mask;
}

bool initialize_decoder(H264GpuDecoder* probe, ConstSlice<u8> encoded, GpuDevice* device,
                        Mp4SourceAvailability availability, Mp4FragmentSource* fragmented) noexcept {
    if (!probe || !device || !encoded.data || !encoded.count)
        return false;
    h264_gpu_decoder_release(probe);
    // A progressive source is borrowed in place; its owner outlives release.
    const bool borrowed = availability.available != nullptr;
    if (encoded.count > (borrowed ? kMediaMaximumProgressiveVideoBytes : kMp4DemuxMaximumEncodedBytes)) {
        probe->status = E_INVALIDARG;
        probe->parse_error = H264Error::unsupported;
        probe->demux_error = Mp4DemuxError::unsupported;
        return false;
    }
    constexpr u32 input_offset = (sizeof(State) + 15u) & ~15u;
    const usize bytes = usize{input_offset} + (borrowed ? 0 : encoded.count);
    auto* s = static_cast<State*>(commit_pages(bytes));
    if (!s) {
        probe->status = E_OUTOFMEMORY;
        return false;
    }
    probe->state = s;
    s->allocation = bytes;
    s->fragmented = fragmented;
    s->output.leased = 255;
    s->device = device;
    const u8* owned = encoded.data;
    if (!borrowed) {
        auto* copy = reinterpret_cast<u8*>(s) + input_offset;
        copy_bytes(copy, encoded.data, encoded.count);
        owned = copy;
    }
    if (fragmented) {
        const auto& track = fragmented->track;
        auto& demux = s->demux;
        demux.encoded = encoded;
        demux.availability = availability;
        demux.width = track.width;
        demux.height = track.height;
        demux.sar_width = track.sar_width;
        demux.sar_height = track.sar_height;
        demux.rotation = track.rotation;
        demux.nal_length_bytes = track.nal_length_bytes;
        demux.timescale = track.timescale;
        demux.has_edit_list = track.edited;
        demux.avcc_offset = static_cast<u32>(track.configuration.data - encoded.data);
        demux.avcc_bytes = track.configuration.count;
        Mp4AccessUnit first{};
        probe->demux_error = mp4_fragment_source_unit(fragmented, &first);
        if (probe->demux_error == Mp4DemuxError::none && !mp4_fragment_source_duration(*fragmented, &demux.duration_us))
            probe->demux_error = Mp4DemuxError::unsupported;
        probe->parse_error = probe->demux_error == Mp4DemuxError::none
                                 ? h264_gpu_configuration_from_unit(first, track.configuration, track.nal_length_bytes,
                                                                    track.width, track.height, &s->picture)
                                 : H264Error::malformed;
        demux.ready = probe->parse_error == H264Error::none;
        s->first_presentation = first.presentation_time_us_signed;
    } else {
        probe->demux_error = mp4_demux_initialize(&s->demux, {owned, encoded.count}, availability);
        probe->parse_error = probe->demux_error == Mp4DemuxError::none
                                 ? h264_gpu_configuration_from_demux(s->demux, &s->picture, &probe->demux_error)
                                 : H264Error::malformed;
    }
    if (probe->parse_error == H264Error::none) {
        const auto& picture = s->picture.sps;
        s->display = video_display_rotated(
            video_display_extent(picture.width, picture.height,
                                 s->demux.sar_width ? s->demux.sar_width : picture.sar_width,
                                 s->demux.sar_height ? s->demux.sar_height : picture.sar_height),
            s->demux.rotation);
        if (!s->display.width)
            probe->parse_error = H264Error::unsupported;
    }
    if (probe->parse_error == H264Error::none &&
        (!gpu_video_frame_visible_valid(s->picture.sps.width, s->picture.sps.height) ||
         s->picture.sps.coded_width > kMp4DecodeMaximumDimension + 32u ||
         s->picture.sps.coded_height > kMp4DecodeMaximumDimension + 32u || s->picture.sps.transform_bypass))
        probe->parse_error = H264Error::unsupported;
    bool created = false;
    if (probe->parse_error == H264Error::none) {
        created = create_hardware(s) || create_software(s);
        probe->status = created ? S_OK : DXGI_ERROR_UNSUPPORTED;
    } else {
        probe->status = E_INVALIDARG;
    }
    if (!created) {
        const H264Error parse_error = probe->parse_error;
        const Mp4DemuxError demux_error = probe->demux_error;
        const i32 status = probe->status;
        h264_gpu_decoder_release(probe);
        probe->status = status;
        probe->parse_error = parse_error;
        probe->demux_error = demux_error;
        return false;
    }
    const auto& a = s->picture.sps;
    // A hardware picture is exported whole (crop applied by the conversion); a software one is the visible extent.
    s->frame = {nullptr,
                0,
                s->session ? a.crop_x : 0u,
                s->session ? a.crop_y : 0u,
                a.width,
                a.height,
                video_matrix_bt709(a.matrix, a.height),
                a.full_range,
                s->demux.rotation,
                {}};
    return true;
}

} // namespace

bool h264_gpu_decoder_initialize(H264GpuDecoder* probe, ConstSlice<u8> encoded, GpuDevice* device, GpuContext*,
                                 Mp4SourceAvailability availability) noexcept {
    return initialize_decoder(probe, encoded, device, availability, nullptr);
}

bool h264_gpu_decoder_initialize_fragmented(H264GpuDecoder* decoder, Mp4FragmentSource* source, GpuDevice* device,
                                            GpuContext*) noexcept {
    if (!decoder || decoder->state || !device || !source || !mp4_fragment_source_valid(*source) || source->track.audio ||
        source->ended || source->segment.index || (source->loaded && source->fragment.index))
        return false;
    Mp4AccessUnit first{};
    const auto error = mp4_fragment_source_unit(source, &first);
    if (error != Mp4DemuxError::none) {
        decoder->demux_error = error;
        return false;
    }
    const Mp4SourceAvailability header{[](void* context, u64 offset, u64 bytes) noexcept {
                                           const auto span = mp4_fragment_source_bytes(*static_cast<Mp4FragmentSource*>(context));
                                           return offset <= span.count && bytes <= span.count - offset;
                                       },
                                       source};
    return initialize_decoder(decoder, mp4_fragment_source_bytes(*source), device,
                              source->segments ? header : progressive_source_reader(source->source, source->reader),
                              source);
}

Mp4DecodedFrame h264_gpu_decoder_read(H264GpuDecoder* probe, u32 max_submissions) noexcept {
    Mp4DecodedFrame frame{};
    frame.error = Mp4DecodeError::invalid_input;
    if (!probe || !probe->state)
        return frame;
    auto* s = static_cast<State*>(probe->state);
    s->fragment_seek_pending = false;
    if (max_submissions && s->output.leased != 255)
        return frame;
    // The caller has enqueued its copy before requesting another frame.
    s->output.leased = 255;
    u32 submissions{};
    for (;;) {
        const auto& a = s->picture.sps;
        const bool ready = s->session ? h264_output_ready(s->output, reference_mask(*s), h264_sps_output_reorder(a),
                                                          maximum<u8>(h264_sps_dpb_frames(a), 1))
                                      : h264_output_ready(s->output, 0, 0, 1); // libopenh264 reordered already
        if (s->output.count && ready)
            return output_frame(s);
        Mp4Demuxer next;
        copy_value(&next, s->demux);
        Mp4AccessUnit unit{};
        if (s->fragmented)
            unit.error = mp4_fragment_source_unit(s->fragmented, &unit);
        else
            unit = mp4_demux_next(&next);
        if (unit.error == Mp4DemuxError::data_pending) {
            frame.error = Mp4DecodeError::data_pending;
            return frame;
        }
        if (unit.error == Mp4DemuxError::end_of_stream) {
            // The end of the stream releases what libopenh264 still holds back.
            if (s->software && !drain_software(s)) {
                probe->status = E_FAIL;
                frame.error = Mp4DecodeError::sample;
                return frame;
            }
            if (s->output.count)
                return output_frame(s);
            frame.error = Mp4DecodeError::end_of_stream;
            return frame;
        }
        // Fragment cursors already lease and validate their payload. Separate files do not belong to the
        // initialization span used for avcC.
        if (unit.ok() && !s->fragmented && !mp4_demux_available(s->demux, unit.bytes)) {
            frame.error = Mp4DecodeError::data_pending;
            return frame;
        }
        if (max_submissions && submissions >= max_submissions) {
            frame.error = Mp4DecodeError::needs_service;
            return frame;
        }
        H264Picture header{};
        probe->parse_error = unit.ok() ? h264_parse_picture(unit.bytes, s->picture.length_bytes, a, s->picture.pps,
                                                            &header, mp4_demux_avcc(s->demux))
                                       : H264Error::malformed;
        if (probe->parse_error != H264Error::none) {
            probe->status = E_INVALIDARG;
            frame.error = Mp4DecodeError::stream;
            return frame;
        }
        bool reset = header.first.idr;
        for (u32 i = 0; i < header.first.marking_count; ++i)
            reset |= header.first.marking[i].operation == 5;
        const bool discard = header.first.idr && header.first.discard_prior;
        // (libopenh264 outputs the pictures before an IDR picture itself, in order; draining it there leaks its buffers.)
        if (reset && s->output.count) {
            if (discard)
                reset_value(&s->output);
            else
                return output_frame(s);
        }
        s->picture.unit = unit;
        s->picture.header = header;
        u8 surface = 255;
        H264ReferencePlan plan{};
        bool decoded = false;
        if (s->session) {
            surface = h264_free_surface(s->output, header.first.idr ? 0 : reference_mask(*s), static_cast<u8>(s->slots));
            probe->parse_error = h264_reference_plan(s->references, a, header.first, surface, &plan);
            if (probe->parse_error != H264Error::none) {
                probe->status = E_INVALIDARG;
                frame.error = Mp4DecodeError::stream;
                return frame;
            }
            decoded = decode_hardware(s, plan, surface);
        } else
            decoded = decode_software(probe, s, unit);
        probe->status = decoded ? S_OK : E_FAIL;
        if (!decoded) {
            frame.error = Mp4DecodeError::sample;
            return frame;
        }
        if (s->session)
            copy_value(&s->references, plan.after);
        if (s->fragmented) {
            if (!mp4_fragment_source_commit(s->fragmented)) {
                probe->status = E_UNEXPECTED;
                frame.error = Mp4DecodeError::stream;
                return frame;
            }
            ++s->demux.sample_index;
        } else
            copy_value(&s->demux, next);
        ++submissions;
        if (s->session) {
            const auto order = plan.retained_order;
            if (!h264_output_push(&s->output, {unit.duration_us, order.top < order.bottom ? order.top : order.bottom,
                                               surface, unit.presentation_time_us_signed})) {
                probe->status = E_UNEXPECTED;
                frame.error = Mp4DecodeError::sample;
                return frame;
            }
        }
    }
}

H264GpuMovie h264_gpu_decoder_movie(const H264GpuDecoder& decoder) noexcept {
    const auto* s = static_cast<const State*>(decoder.state);
    if (!s || !s->demux.ready)
        return {};
    H264GpuMovie movie{s->demux.duration_us, s->demux.sample_count, s->demux.has_edit_list};
    if (s->fragmented) {
        if (const auto* store = s->fragmented->segments; store && store->live) {
            if (!mp4_fragment_source_duration(*s->fragmented, &movie.duration_us))
                return {};
            movie.live = true;
            movie.duration_complete = store->ended && store->received_count == store->count;
        }
        movie.first_presentation_time_us = s->first_presentation;
        movie.first_presentation_valid = true;
        return movie;
    }
    Mp4Demuxer first;
    copy_value(&first, s->demux);
    mp4_demux_reset(&first);
    const auto unit = mp4_demux_next(&first);
    if (unit.ok()) {
        movie.first_presentation_time_us = unit.presentation_time_us_signed;
        movie.first_presentation_valid = true;
    }
    return movie;
}

ConstSlice<u8> h264_gpu_decoder_source(const H264GpuDecoder& decoder) noexcept {
    const auto* s = static_cast<const State*>(decoder.state);
    return s && s->demux.ready ? s->demux.encoded : ConstSlice<u8>{};
}

bool h264_gpu_decoder_acknowledge_frame(H264GpuDecoder* decoder) noexcept {
    if (!decoder || !decoder->state)
        return false;
    auto* s = static_cast<State*>(decoder->state);
    if (!s->demux.ready)
        return false;
    s->output.leased = 255;
    return true;
}

Mp4DemuxError h264_gpu_decoder_seek(H264GpuDecoder* decoder, i64 target, Mp4SeekPoint* point) noexcept {
    if (!decoder || !decoder->state || !point)
        return Mp4DemuxError::invalid_input;
    auto* s = static_cast<State*>(decoder->state);
    const ConstSlice<u8> output{reinterpret_cast<const u8*>(point), sizeof(*point)};
    if (!s->demux.ready || s->output.leased != 255 || !byte_range_valid(output) ||
        byte_ranges_overlap(output, {reinterpret_cast<const u8*>(decoder), sizeof(*decoder)}) ||
        byte_ranges_overlap(output, {reinterpret_cast<const u8*>(s), sizeof(*s)}) ||
        byte_ranges_overlap(output, s->demux.encoded))
        return Mp4DemuxError::invalid_input;
    Mp4Demuxer next;
    copy_value(&next, s->demux);
    Mp4SeekPoint selected{};
    const auto result = s->fragmented ? seek_fragment(s, target, &selected, false) : mp4_demux_seek_sync(&next, target, &selected);
    if (result != Mp4DemuxError::none)
        return result;
    if (s->fragmented)
        next.sample_index = selected.sample_index;
    copy_value(&s->demux, next);
    reset_value(&s->references);
    reset_value(&s->output);
    software_decoder_reset(s->software);
    decoder->status = S_OK;
    decoder->parse_error = H264Error::none;
    decoder->demux_error = Mp4DemuxError::none;
    *point = selected;
    return Mp4DemuxError::none;
}

bool h264_gpu_decoder_rewind(H264GpuDecoder* decoder) noexcept {
    if (!decoder || !decoder->state)
        return false;
    auto* s = static_cast<State*>(decoder->state);
    if (s->fragmented) {
        Mp4SeekPoint point{};
        decoder->demux_error = seek_fragment(s, 0, &point, true);
        if (decoder->demux_error != Mp4DemuxError::none)
            return false;
        s->demux.sample_index = 0;
        reset_value(&s->references);
        reset_value(&s->output);
        software_decoder_reset(s->software);
        return true;
    }
    Mp4Demuxer next;
    copy_value(&next, s->demux);
    mp4_demux_reset(&next);
    if (!next.ready)
        return false;
    copy_value(&s->demux, next);
    reset_value(&s->references);
    reset_value(&s->output);
    software_decoder_reset(s->software);
    return true;
}

bool h264_gpu_decoder_seek_start(H264GpuDecoder* decoder) noexcept {
    if (!decoder || !decoder->state)
        return false;
    const auto* s = static_cast<const State*>(decoder->state);
    if (!s->demux.ready || s->output.leased != 255)
        return false;
    if (!h264_gpu_decoder_rewind(decoder))
        return false;
    decoder->status = S_OK;
    decoder->parse_error = H264Error::none;
    return true;
}

void h264_gpu_decoder_release(H264GpuDecoder* probe) noexcept {
    if (!probe)
        return;
    if (auto* s = static_cast<State*>(probe->state)) {
        release_tiers(s);
        pages_release(s, s->allocation);
    }
    reset_value(probe);
}

// Both tiers decode on the calling thread: there is no device backpressure to wait for.
bool h264_gpu_decoder_busy(const H264GpuDecoder&) noexcept {
    return false;
}

bool h264_gpu_decoder_wait(H264GpuDecoder* decoder, u32) noexcept {
    return decoder != nullptr;
}

} // namespace maicord
