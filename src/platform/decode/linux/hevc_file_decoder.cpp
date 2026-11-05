#include "maicord/platform/decode/hevc_gpu_decoder.hpp"

#include "../hevc_gpu_geometry.hpp"
#include "../vaapi/va.hpp"
#include "../vaapi/va_file.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/media/containers/hevc_mp4_demux.hpp"
#include "maicord/media/video/h264_output_queue.hpp"
#include "maicord/media/video/hevc_references.hpp"
#include "maicord/media/video/video_display.hpp"
#include "maicord/platform/decode/mp4_decoder.hpp"
#include "stream.hpp"

// The HEVC movie decoder of the Linux build: what src/platform/decode/hevc_gpu_decoder.cpp is on Windows (the
// hevc_gpu_decoder.hpp interface that mp4_decoder.cpp drives) with the Direct3D 11 video decoder replaced by VA-API
// (src/platform/decode/vaapi): the picture and slice buffers come from the same in-house parsers, the decoder writes a
// VASurface, and a picture that is output is exported as two dma-buf planes and imported as the planes the renderer
// converts from. There is no software HEVC decoder: a machine whose driver has no HEVC Main decoder (or refuses the
// movie's first picture) refuses the movie, as a GPU without the codec does on Windows. The container side, the
// reference tracking and the output queue are the Windows decoder's.

namespace maicord {
namespace {

constexpr u32 kSlotLimit = kStreamSurfaceLimit;

struct State {
    HevcConfiguration config;
    Mp4Demuxer demux;
    Mp4AccessUnit first_unit; // the first picture, kept for the probe
    HevcReferenceState references;
    H264OutputQueue output;
    GpuDevice* device;
    vaapi::Session* session;
    StreamSurface slot[kSlotLimit];
    u32 slots;
    GpuVideoFrame frame;
    VideoDisplayExtent display;
    usize allocation;
};

H264Error mapped(HevcError error) noexcept {
    return static_cast<H264Error>(static_cast<u8>(error));
}

void slots_release(State* s) noexcept {
    for (u32 i = 0; i < s->slots; ++i)
        stream_slot_release(&s->slot[i]);
    clear_bytes(s->slot, sizeof(s->slot));
    s->slots = 0;
}

bool decode_picture(State* s, const Mp4AccessUnit& unit, const HevcPicture& picture, const HevcReferencePlan& plan,
                    u8 surface) noexcept {
    return vaapi::submit_hevc_sample(s->session, s->config, picture, unit.bytes, s->references, plan, surface);
}

// One real picture (the first IRAP) through the driver: decode, export, import. Whatever it learns is discarded.
bool probe_hardware(State* s, const HevcPicture& picture) noexcept {
    HevcReferencePlan plan{};
    if (hevc_reference_plan(s->references, s->config.sps, picture, 0, &plan) != HevcError::none ||
        !decode_picture(s, s->first_unit, picture, plan, 0))
        return false;
    StreamSurface exported{};
    if (!stream_slot_export(s->device, s->session, 0, &exported))
        return false;
    stream_slot_release(&exported);
    return true;
}

void release_state(State* s) noexcept {
    vaapi::session_destroy(s->session);
    s->session = nullptr;
    slots_release(s);
}

bool reset_cursor(H264GpuDecoder& handle) noexcept {
    auto& s = *static_cast<State*>(handle.state);
    auto cursor = s.demux;
    mp4_demux_reset(&cursor);
    if (!cursor.ready)
        return false;
    copy_value(&s.demux, cursor);
    reset_value(&s.references);
    reset_value(&s.output);
    return true;
}

// Fills the frame of the picture the output queue shows next, exporting it into its slot now.
Mp4DecodedFrame output_frame(State& s) noexcept {
    Mp4DecodedFrame decoded{};
    H264Output picture{};
    if (!h264_output_pop(&s.output, &picture)) {
        decoded.error = Mp4DecodeError::sample;
        return decoded;
    }
    StreamSurface& slot = s.slot[picture.surface];
    if (!stream_slot_export(s.device, s.session, picture.surface, &slot)) {
        decoded.error = Mp4DecodeError::sample;
        return decoded;
    }
    s.frame.texture = slot.luma_texture;
    s.frame.subresource = picture.surface;
    s.frame.luma = slot.luma_view;
    s.frame.chroma = slot.chroma_view;
    decoded.gpu = &s.frame;
    decoded.width = s.frame.width;
    decoded.height = s.frame.height;
    decoded.display_width = s.display.width;
    decoded.display_height = s.display.height;
    decoded.duration_us = decoded.source_duration_us = picture.duration_us;
    decoded.presentation_time_us = picture.presentation_time_us;
    return decoded;
}

} // namespace

bool hevc_gpu_decoder_initialize(H264GpuDecoder* handle, ConstSlice<u8> encoded, GpuDevice* device, GpuContext*,
                                 Mp4SourceAvailability availability) noexcept {
    if (!handle || !device || !encoded.data || !encoded.count)
        return false;
    hevc_gpu_decoder_release(handle);
    // A progressive source is borrowed in place; its owner outlives release.
    const bool borrowed = availability.available != nullptr;
    if (encoded.count > (borrowed ? kMediaMaximumProgressiveVideoBytes : kMp4DemuxMaximumEncodedBytes)) {
        handle->status = E_INVALIDARG;
        handle->demux_error = Mp4DemuxError::unsupported;
        handle->parse_error = H264Error::unsupported;
        return false;
    }
    constexpr u32 offset = (sizeof(State) + 15u) & ~15u;
    const usize bytes = usize{offset} + (borrowed ? 0 : encoded.count);
    auto* s = static_cast<State*>(commit_pages(bytes));
    if (!s) {
        handle->status = E_OUTOFMEMORY;
        return false;
    }
    handle->state = s;
    s->allocation = bytes;
    s->output.leased = 255;
    s->device = device;
    const u8* owned = encoded.data;
    if (!borrowed) {
        auto* copy = reinterpret_cast<u8*>(s) + offset;
        copy_bytes(copy, encoded.data, encoded.count);
        owned = copy;
    }
    handle->demux_error = hevc_demux_initialize(&s->demux, {owned, encoded.count}, availability);
    bool ok = handle->demux_error == Mp4DemuxError::none;
    HevcPicture picture;
    if (ok) {
        Mp4Demuxer cursor;
        copy_value(&cursor, s->demux);
        s->first_unit = mp4_demux_next_ready(&cursor);
        u8 pps{};
        auto error = s->first_unit.ok() ? hevc_picture_parameter_id(s->first_unit.bytes, s->demux.nal_length_bytes, &pps)
                                        : HevcError::malformed;
        if (error == HevcError::none)
            error = hevc_select_parameters(hevc_demux_hvcc(s->demux), pps, &s->config);
        if (error == HevcError::none)
            error = hevc_parse_picture(s->first_unit.bytes, s->config, &picture);
        if (error == HevcError::none && !picture.irap)
            error = HevcError::unsupported;
        handle->parse_error = mapped(error);
        ok = error == HevcError::none;
    }
    const auto& a = s->config.sps;
    const auto surface_extent = hevc_gpu_surface_extent(a.coded_width, a.coded_height);
    if (ok && (a.profile != 1 || !surface_extent.width || !a.max_buffering || a.max_buffering > 15 || a.max_reorder > 15 ||
               a.log2_min_cb_minus3 > 3 || !a.width || !a.height || a.crop_x > a.coded_width ||
               a.width > a.coded_width - a.crop_x || a.crop_y > a.coded_height || a.height > a.coded_height - a.crop_y)) {
        handle->parse_error = H264Error::unsupported;
        ok = false;
    }
    s->display = video_display_rotated(video_display_extent(a.width, a.height, a.sar_width, a.sar_height), s->demux.rotation);
    if (ok && !s->display.width) {
        handle->parse_error = H264Error::unsupported;
        ok = false;
    }
    i32 status = ok ? S_OK : E_INVALIDARG;
    if (ok) {
        const vaapi::Driver* driver = vaapi::driver(device);
        const u32 count = maximum<u32>(a.max_buffering, 1) + 2u;
        if (!driver || !(driver->codecs & gpu_stream_codec_bit(GpuStreamCodec::h265)) ||
            surface_extent.width > driver->max_width[1] || surface_extent.height > driver->max_height[1] ||
            count > vaapi::kSurfaceLimit)
            status = DXGI_ERROR_UNSUPPORTED;
        else {
            s->session = vaapi::session_create(driver, vaapi::kProfileHevcMain, surface_extent.width, surface_extent.height,
                                               count);
            s->slots = s->session ? count : 0;
            if (!s->session || !probe_hardware(s, picture))
                status = DXGI_ERROR_UNSUPPORTED;
        }
    }
    handle->status = status;
    if (status != S_OK) {
        const auto parse = handle->parse_error;
        const auto demux = handle->demux_error;
        hevc_gpu_decoder_release(handle);
        handle->status = status;
        handle->parse_error = parse;
        handle->demux_error = demux;
        return false;
    }
    s->frame = {nullptr,
                0,
                a.crop_x,
                a.crop_y,
                a.width,
                a.height,
                video_matrix_bt709(a.matrix, a.height),
                a.full_range,
                s->demux.rotation,
                {}};
    return true;
}

Mp4DecodedFrame hevc_gpu_decoder_read(H264GpuDecoder* handle, u32 budget) noexcept {
    Mp4DecodedFrame frame{};
    frame.error = Mp4DecodeError::invalid_input;
    if (!handle || !handle->state)
        return frame;
    auto& s = *static_cast<State*>(handle->state);
    if (budget && s.output.leased != 255)
        return frame;
    s.output.leased = 255;
    u32 submissions{};
    for (;;) {
        if (s.output.count && h264_output_ready(s.output, hevc_reference_surfaces(s.references), s.config.sps.max_reorder,
                                                s.config.sps.max_buffering))
            return output_frame(s);
        auto next = s.demux;
        const auto unit = mp4_demux_next(&next);
        if (unit.error == Mp4DemuxError::end_of_stream) {
            if (s.output.count)
                return output_frame(s);
            frame.error = Mp4DecodeError::end_of_stream;
            return frame;
        }
        if (unit.ok() && !mp4_demux_available(s.demux, unit.bytes)) {
            frame.error = Mp4DecodeError::data_pending;
            return frame;
        }
        if (budget && submissions >= budget) {
            frame.error = Mp4DecodeError::needs_service;
            return frame;
        }
        HevcPicture picture;
        const auto error = unit.ok() ? hevc_parse_picture(unit.bytes, s.config, &picture) : HevcError::malformed;
        handle->parse_error = mapped(error);
        if (error != HevcError::none) {
            handle->status = E_INVALIDARG;
            frame.error = Mp4DecodeError::stream;
            return frame;
        }
        if (picture.irap && s.output.count) {
            if (picture.no_output_prior)
                reset_value(&s.output);
            else
                return output_frame(s);
        }
        const u8 surface = h264_free_surface(s.output, picture.idr ? 0 : hevc_reference_surfaces(s.references),
                                             static_cast<u8>(s.slots));
        HevcReferencePlan plan{};
        handle->parse_error = mapped(hevc_reference_plan(s.references, s.config.sps, picture, surface, &plan));
        if (handle->parse_error != H264Error::none) {
            handle->status = E_INVALIDARG;
            frame.error = Mp4DecodeError::stream;
            return frame;
        }
        const bool decoded = decode_picture(&s, unit, picture, plan, surface);
        handle->status = decoded ? S_OK : E_FAIL;
        if (!decoded) {
            frame.error = Mp4DecodeError::sample;
            return frame;
        }
        copy_value(&s.demux, next);
        s.references = plan.after;
        ++submissions;
        if (picture.output &&
            !h264_output_push(&s.output, {unit.duration_us, plan.poc, surface, unit.presentation_time_us_signed})) {
            handle->status = E_UNEXPECTED;
            frame.error = Mp4DecodeError::sample;
            return frame;
        }
    }
}

bool hevc_gpu_decoder_acknowledge_frame(H264GpuDecoder* handle) noexcept {
    if (!handle || !handle->state)
        return false;
    static_cast<State*>(handle->state)->output.leased = 255;
    return true;
}

ConstSlice<u8> hevc_gpu_decoder_source(const H264GpuDecoder& handle) noexcept {
    const auto* s = static_cast<const State*>(handle.state);
    return s ? s->demux.encoded : ConstSlice<u8>{};
}

H264GpuMovie hevc_gpu_decoder_movie(const H264GpuDecoder& handle) noexcept {
    const auto* s = static_cast<const State*>(handle.state);
    if (!s || !s->demux.ready)
        return {};
    Mp4Demuxer cursor;
    copy_value(&cursor, s->demux);
    mp4_demux_reset(&cursor);
    const auto first = mp4_demux_next(&cursor);
    return {s->demux.duration_us, s->demux.sample_count, s->demux.has_edit_list, first.presentation_time_us_signed,
            first.ok()};
}

Mp4DemuxError hevc_gpu_decoder_seek(H264GpuDecoder* handle, i64 target, Mp4SeekPoint* point) noexcept {
    if (!handle || !handle->state || !point)
        return Mp4DemuxError::invalid_input;
    auto& s = *static_cast<State*>(handle->state);
    const ConstSlice<u8> out{reinterpret_cast<const u8*>(point), sizeof(*point)};
    if (s.output.leased != 255 || !byte_range_valid(out) ||
        byte_ranges_overlap(out, {reinterpret_cast<const u8*>(handle), sizeof(*handle)}) ||
        byte_ranges_overlap(out, {reinterpret_cast<const u8*>(&s), sizeof(s)}) ||
        byte_ranges_overlap(out, s.demux.encoded))
        return Mp4DemuxError::invalid_input;
    auto next = s.demux;
    Mp4SeekPoint selected{};
    const auto result = hevc_demux_seek_sync(&next, target, &selected);
    if (result != Mp4DemuxError::none)
        return result;
    auto probe = next;
    const auto unit = mp4_demux_next_ready(&probe);
    if (unit.error == Mp4DemuxError::data_pending)
        return unit.error;
    HevcPicture picture;
    if (!unit.ok() || hevc_parse_picture(unit.bytes, s.config, &picture) != HevcError::none || !picture.irap)
        return Mp4DemuxError::unsupported;
    copy_value(&s.demux, next);
    reset_value(&s.references);
    reset_value(&s.output);
    handle->status = S_OK;
    handle->parse_error = H264Error::none;
    *point = selected;
    return result;
}

bool hevc_gpu_decoder_seek_start(H264GpuDecoder* handle) noexcept {
    if (!handle || !handle->state || static_cast<State*>(handle->state)->output.leased != 255)
        return false;
    if (!reset_cursor(*handle))
        return false;
    handle->status = S_OK;
    handle->parse_error = H264Error::none;
    return true;
}

void hevc_gpu_decoder_release(H264GpuDecoder* handle) noexcept {
    if (!handle)
        return;
    if (auto* s = static_cast<State*>(handle->state)) {
        release_state(s);
        pages_release(s, s->allocation);
    }
    reset_value(handle);
}

// The decoder runs on the calling thread: there is no device backpressure to wait for.
bool hevc_gpu_decoder_wait(H264GpuDecoder* handle, u32) noexcept {
    return handle && handle->state;
}

} // namespace maicord
