#include "maicord/platform/encode/gpu_stream_encoder.hpp"

#include "../access_unit.hpp"
#include "../av1_headers.hpp"
#include "../../decode/vaapi/va.hpp"
#include "maicord/core/interlocked.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/sys/linux/calls.hpp"

// The Linux Go Live encoder, on the VA display the hardware decoders opened (src/platform/decode/vaapi): the capture's
// dma-buf is imported as an RGB surface and scaled and converted to NV12 by the driver's video processing (on the
// capture's thread, so nothing of the frame is kept), and the encoder thread encodes the newest picture with the VA
// encode entry point (EncSlice, or the low-power EncSliceLP). Inter pictures reference the picture before them,
// nothing else.
//
// AV1 (Profile 0), H.265 Main and H.264 (Constrained Baseline, else Main, else High), as the display reports them.
// AV1 takes its sequence and frame header OBUs from the encoder as packed headers (av1_headers.hpp), with the bit
// positions the driver rewrites after rate control. For H.264 and H.265 the driver writes the parameter sets and slice
// headers itself (no packed headers): every keyframe is an IDR with its SPS/PPS (and VPS), which the encoder repeats
// from the first keyframe when a driver leaves them out.

namespace maicord {
namespace {

using namespace vaapi;

constexpr i32 kProfileNone = -1; // VAProfileNone, for video processing
// DRM fourccs of the capture's packed RGB planes and the VA fourcc of each (the same byte order in memory).
constexpr u32 kDrmXrgb8888 = 0x34325258, kDrmArgb8888 = 0x34325241, kDrmXbgr8888 = 0x34324258,
              kDrmAbgr8888 = 0x34324241;
constexpr u32 kSlots = 6;
constexpr u8 kNone = 255;
constexpr u32 kParameterBytes = 1024;
constexpr u64 kStillUs = 100'000;
constexpr u32 kStackBytes = 256u * 1024u;
// Keyframes come from requests (PLI/FIR); the period only bounds how long a lost request can go unanswered.
constexpr u32 kIdrPeriod = 3000;
// H.264 slice types and HEVC NAL unit and slice types.
constexpr u8 kH264SliceP = 0, kH264SliceI = 2;
constexpr u8 kHevcIdrWRadl = 19, kHevcTrailR = 1, kHevcSliceP = 1, kHevcSliceI = 2;

constexpr i32 kH264Profiles[]{kProfileH264ConstrainedBaseline, kProfileH264Main, kProfileH264High};
constexpr i32 kEncodeEntrypoints[]{kEntrypointEncSlice, kEntrypointEncSliceLp};
// Probed in this order; the stream picks among them by tier (gpu_stream_encoder_pick).
constexpr GpuStreamCodec kEncodedCodecs[]{GpuStreamCodec::av1, GpuStreamCodec::h265, GpuStreamCodec::h264};
constexpr u32 kAv1PackedHeaders = kEncPackedHeaderSequenceFlag | kEncPackedHeaderPictureFlag;

u32 va_fourcc(u32 drm) noexcept {
    switch (drm) {
    case kDrmXrgb8888: return kFourccBgrx;
    case kDrmArgb8888: return kFourccBgra;
    case kDrmXbgr8888: return kFourccRgbx;
    case kDrmAbgr8888: return kFourccRgba;
    default: return 0;
    }
}

bool has_entrypoint(const Driver& d, i32 profile, i32 entrypoint) noexcept {
    const i32 capacity = d.api.vaMaxNumEntrypoints(d.display);
    i32 entrypoints[64];
    i32 count = 0;
    if (capacity <= 0 || capacity > 64 ||
        d.api.vaQueryConfigEntrypoints(d.display, profile, entrypoints, &count) != kStatusSuccess)
        return false;
    for (i32 i = 0; i < count && i < 64; ++i)
        if (entrypoints[i] == entrypoint)
            return true;
    return false;
}

// The encode entry point of `profile` that takes 8-bit 4:2:0 input under CBR (and, for `packed` headers, takes those
// packed headers): EncSlice, else EncSliceLP; 0 for none.
i32 encode_entrypoint(const Driver& d, i32 profile, u32 packed = 0) noexcept {
    for (const i32 entrypoint : kEncodeEntrypoints) {
        if (!has_entrypoint(d, profile, entrypoint))
            continue;
        ConfigAttrib attributes[3]{{kConfigAttribRtFormat, 0}, {kConfigAttribRateControl, 0},
                                   {kConfigAttribEncPackedHeaders, 0}};
        if (d.api.vaGetConfigAttributes(d.display, profile, entrypoint, attributes, 3) == kStatusSuccess &&
            attributes[0].value != kAttribNotSupported && (attributes[0].value & kRtFormatYuv420) &&
            attributes[1].value != kAttribNotSupported && (attributes[1].value & kRcCbr) &&
            (!packed || (attributes[2].value != kAttribNotSupported && (attributes[2].value & packed) == packed)))
            return entrypoint;
    }
    return 0;
}

struct Choice {
    i32 profile, entrypoint;
    // AV1: the interpolation filter (4 switchable), transform mode and obu_size field width the driver takes.
    u8 av1_filter, av1_size_bytes;
    bool av1_tx_select;
};

// What the driver's AV1 encoder takes of the tools the frame header chooses (VAConfigAttribEncAV1Ext1 and Ext2).
void av1_tools(const Driver& d, i32 entrypoint, Choice* choice) noexcept {
    ConfigAttrib attributes[2]{{kConfigAttribEncAv1Ext1, 0}, {kConfigAttribEncAv1Ext2, 0}};
    const bool reported = d.api.vaGetConfigAttributes(d.display, kProfileAv1Profile0, entrypoint, attributes, 2) ==
                          kStatusSuccess;
    EncAv1Ext1 ext1{};
    EncAv1Ext2 ext2{};
    if (reported && attributes[0].value != kAttribNotSupported)
        ext1.value = attributes[0].value;
    if (reported && attributes[1].value != kAttribNotSupported)
        ext2.value = attributes[1].value;
    // Switchable filters unless the driver names the ones it takes without them.
    const u32 filters = ext1.bits.interpolation_filter;
    choice->av1_filter = 4;
    for (u8 filter = 0; filters && !(filters & 0x10) && filter < 4; ++filter)
        if (filters >> filter & 1) {
            choice->av1_filter = filter;
            break;
        }
    // TX_MODE_SELECT unless the driver takes only TX_MODE_LARGEST.
    choice->av1_tx_select = !ext2.bits.tx_mode_support || (ext2.bits.tx_mode_support & 4);
    choice->av1_size_bytes = static_cast<u8>(ext2.value ? ext2.bits.obu_size_bytes_minus1 + 1 : 4);
}

// What the display encodes `codec` with; profile 0 when it does not (or has no video processing to feed it).
Choice choose(const Driver& d, GpuStreamCodec codec) noexcept {
    if (!has_entrypoint(d, kProfileNone, kEntrypointVideoProc))
        return {};
    if (codec == GpuStreamCodec::av1) {
        Choice choice{kProfileAv1Profile0, encode_entrypoint(d, kProfileAv1Profile0, kAv1PackedHeaders), 0, 0, false};
        if (!choice.entrypoint)
            return {};
        av1_tools(d, choice.entrypoint, &choice);
        return choice;
    }
    if (codec == GpuStreamCodec::h265) {
        const i32 entrypoint = encode_entrypoint(d, kProfileHevcMain);
        return entrypoint ? Choice{kProfileHevcMain, entrypoint, 0, 0, false} : Choice{};
    }
    for (const i32 profile : kH264Profiles)
        if (const i32 entrypoint = encode_entrypoint(d, profile))
            return {profile, entrypoint, 0, 0, false};
    return {};
}

SurfaceAttrib integer_attribute(i32 type, u32 value) noexcept {
    SurfaceAttrib attribute{};
    attribute.type = type;
    attribute.flags = kSurfaceAttribSettable;
    attribute.value.type = kGenericValueInteger;
    attribute.value.value.i = static_cast<i32>(value);
    return attribute;
}

void close_descriptor_objects(const DrmPrimeDescriptor& descriptor) noexcept {
    for (u32 i = 0; i < descriptor.num_objects && i < 4; ++i)
        if (descriptor.objects[i].fd >= 0)
            sys_close(descriptor.objects[i].fd);
}

struct Slot {
    SurfaceId surface;
    u64 timestamp_us;
    bool in_flight; // the encoder thread is encoding it
};

// A VAEncMiscParameterBuffer: its type, then the parameters.
template <typename T> struct Misc {
    u32 type;
    T parameters;
};

} // namespace

struct GpuStreamEncoder {
    GpuStreamEncoderOptions options; // settings under lock
    SRWLOCK lock;
    const Driver* driver;
    GpuDevice* device;
    Choice choice;
    ConfigId vpp_config, encode_config;
    ContextId vpp_context, encode_context;
    // The NV12 pictures (video processing writes, the encoder reads) and the two reconstructed pictures.
    Slot slots[kSlots];
    SurfaceId reconstructed[2];
    BufferId coded;
    u32 width, height; // of the surfaces and contexts above (0: none)
    u8 newest, held, pending;
    u64 last_accepted_us;
    u32 serial; // pictures made the newest, for the preview's sequence
    bool accepted, keyframe_requested, stop, ended;
    Waitable wake;
    Thread* thread;
    GpuStreamEncoderStats stats;
    // Encoder thread only.
    u32 frames_since_idr, idr_count, bitrate_sent, fps_sent, current;
    EncodedUnitBuffer unit;
    u8 parameters[kParameterBytes]; // the last keyframe's parameter sets (Annex B) or AV1 sequence header OBU
    u32 parameter_bytes;
};

namespace {

u64 now_us() noexcept {
    return clock_microseconds(clock_initialize(), 0, clock_now());
}

u32 fps_of(const GpuStreamEncoderSettings& s) noexcept {
    return s.max_fps ? s.max_fps : 60;
}

// The matrix of the converted pictures: AV1 signals BT.709 in its sequence header. H.264 and H.265 streams carry no
// colour description (the driver writes their parameter sets), and the decoders assume BT.709 from 720 lines and
// BT.601 below, so their pictures are converted to match.
bool picture_bt709(const GpuStreamEncoder* e) noexcept {
    return e->options.codec == GpuStreamCodec::av1 || e->height >= 720;
}

// The extent the driver encodes: H.264 crops to any even size, H.265 (whose driver-written SPS has no conformance
// window) to multiples of its 8-pixel minimum coding block.
void encoded_extent(const GpuStreamEncoder* e, u32* width, u32* height) noexcept {
    const u32 mask = e->options.codec == GpuStreamCodec::h265 ? ~7u : ~1u;
    *width = e->options.settings.width & mask;
    *height = e->options.settings.height & mask;
}

void release_pipeline(GpuStreamEncoder* e) noexcept {
    const Driver* d = e->driver;
    if (e->coded != kInvalidId)
        d->api.vaDestroyBuffer(d->display, e->coded);
    if (e->encode_context != kInvalidId)
        d->api.vaDestroyContext(d->display, e->encode_context);
    if (e->vpp_context != kInvalidId)
        d->api.vaDestroyContext(d->display, e->vpp_context);
    for (Slot& slot : e->slots)
        if (slot.surface != kInvalidId)
            d->api.vaDestroySurfaces(d->display, &slot.surface, 1);
    for (SurfaceId& surface : e->reconstructed)
        if (surface != kInvalidId)
            d->api.vaDestroySurfaces(d->display, &surface, 1);
    for (Slot& slot : e->slots)
        slot = {kInvalidId, 0, false};
    e->reconstructed[0] = e->reconstructed[1] = kInvalidId;
    e->coded = kInvalidId;
    e->encode_context = e->vpp_context = kInvalidId;
    e->width = e->height = 0;
    e->newest = e->held = e->pending = kNone;
}

// Under the lock: surfaces, contexts and the coded buffer for the current extent.
bool create_pipeline(GpuStreamEncoder* e) noexcept {
    const Driver* d = e->driver;
    u32 width = 0, height = 0;
    encoded_extent(e, &width, &height);
    SurfaceId surfaces[kSlots + 2];
    const SurfaceAttrib attributes[]{
        integer_attribute(kSurfaceAttribPixelFormat, kFourccNv12),
        integer_attribute(kSurfaceAttribUsageHint,
                          kSurfaceUsageHintEncoder | kSurfaceUsageHintVppWrite | kSurfaceUsageHintExport)};
    bool ok = false;
    for (u32 count = 2; count && !ok; --count)
        ok = d->api.vaCreateSurfaces(d->display, kRtFormatYuv420, width, height, surfaces, kSlots + 2,
                                     const_cast<SurfaceAttrib*>(attributes), count) == kStatusSuccess;
    if (!ok)
        return false;
    for (u32 i = 0; i < kSlots; ++i)
        e->slots[i] = {surfaces[i], 0, false};
    e->reconstructed[0] = surfaces[kSlots];
    e->reconstructed[1] = surfaces[kSlots + 1];
    e->width = width;
    e->height = height;
    ok = d->api.vaCreateContext(d->display, e->vpp_config, static_cast<i32>(width), static_cast<i32>(height),
                                kProgressive, surfaces, kSlots, &e->vpp_context) == kStatusSuccess &&
         d->api.vaCreateContext(d->display, e->encode_config, static_cast<i32>(width), static_cast<i32>(height),
                                kProgressive, surfaces, kSlots + 2, &e->encode_context) == kStatusSuccess &&
         d->api.vaCreateBuffer(d->display, e->encode_context, kEncCodedBuffer, width * height * 3 / 2 + 65536, 1,
                               nullptr, &e->coded) == kStatusSuccess;
    if (!ok)
        release_pipeline(e);
    e->keyframe_requested = true; // A new extent starts a new stream.
    return ok;
}

// A slot the next picture may be written into: never the newest (a keyframe request encodes it again), the one the
// preview holds, the pending one or the one being encoded.
Slot* free_slot(GpuStreamEncoder* e) noexcept {
    for (u8 i = 0; i < kSlots; ++i)
        if (i != e->newest && i != e->held && i != e->pending && !e->slots[i].in_flight)
            return &e->slots[i];
    return nullptr;
}

// The capture's picture, scaled and converted into `slot` by the driver's video processing. The imported surface is
// destroyed once the work is submitted: the driver keeps the buffer until it has read it.
bool convert(GpuStreamEncoder* e, const CaptureFrame& frame, Slot* slot) noexcept {
    const Driver* d = e->driver;
    const GpuDmabufPlane& plane = frame.plane;
    const u32 fourcc = va_fourcc(plane.fourcc);
    if (!fourcc || plane.fd < 0 || !frame.width || !frame.height || frame.width > plane.width ||
        frame.height > plane.height)
        return false;
    // The dma-buf's size (lseek to its end); a compression metadata plane (aux_pitch set) is a second plane, in a
    // second object when it has a buffer of its own.
    const long size = sys_seek(plane.fd, 0, kSeekEnd);
    const bool aux = plane.aux_pitch != 0;
    const bool aux_object = aux && plane.aux_fd >= 0 && plane.aux_fd != plane.fd;
    const long aux_size = aux_object ? sys_seek(plane.aux_fd, 0, kSeekEnd) : 0;
    if (syscall_failed(size) || syscall_failed(aux_size))
        return false;
    DrmPrimeDescriptor descriptor{};
    descriptor.fourcc = fourcc;
    descriptor.width = plane.width;
    descriptor.height = plane.height;
    descriptor.num_objects = aux_object ? 2 : 1;
    descriptor.objects[0] = {plane.fd, static_cast<u32>(size), plane.modifier};
    descriptor.objects[1] = {plane.aux_fd, static_cast<u32>(aux_size), plane.modifier};
    descriptor.num_layers = 1;
    descriptor.layers[0].drm_format = plane.fourcc;
    descriptor.layers[0].num_planes = aux ? 2 : 1;
    descriptor.layers[0].offset[0] = plane.offset;
    descriptor.layers[0].pitch[0] = plane.pitch;
    descriptor.layers[0].object_index[1] = aux_object ? 1 : 0;
    descriptor.layers[0].offset[1] = plane.aux_offset;
    descriptor.layers[0].pitch[1] = plane.aux_pitch;
    SurfaceAttrib attributes[3]{integer_attribute(kSurfaceAttribMemoryType, kSurfaceMemoryTypeDrmPrime2),
                                {}, integer_attribute(kSurfaceAttribUsageHint, kSurfaceUsageHintVppRead)};
    attributes[1].type = kSurfaceAttribExternalBufferDescriptor;
    attributes[1].flags = kSurfaceAttribSettable;
    attributes[1].value.type = kGenericValuePointer;
    attributes[1].value.value.p = &descriptor;
    SurfaceId source = kInvalidId;
    if (d->api.vaCreateSurfaces(d->display, kRtFormatRgb32, plane.width, plane.height, &source, 1, attributes, 3) !=
        kStatusSuccess)
        return false;
    const Rectangle input{0, 0, static_cast<u16>(frame.width), static_cast<u16>(frame.height)};
    const Rectangle output{0, 0, static_cast<u16>(e->width), static_cast<u16>(e->height)};
    ProcPipelineParameter pipeline{};
    pipeline.surface = source;
    pipeline.surface_region = &input;
    pipeline.surface_color_standard = kColorStandardSrgb;
    pipeline.output_region = &output;
    pipeline.output_background_color = 0xff000000;
    // Limited range, in the matrix the stream signals or its decoders assume (picture_bt709).
    pipeline.output_color_standard = picture_bt709(e) ? kColorStandardBt709 : kColorStandardBt601;
    pipeline.filter_flags = kFilterScalingHq;
    pipeline.input_color_properties.color_range = kSourceRangeFull;
    pipeline.output_color_properties.color_range = kSourceRangeReduced;
    BufferId buffer = kInvalidId;
    bool ok = d->api.vaCreateBuffer(d->display, e->vpp_context, kProcPipelineParameterBuffer, sizeof(pipeline), 1,
                                    &pipeline, &buffer) == kStatusSuccess;
    if (ok) {
        ok = d->api.vaBeginPicture(d->display, e->vpp_context, slot->surface) == kStatusSuccess;
        if (ok) {
            ok = d->api.vaRenderPicture(d->display, e->vpp_context, &buffer, 1) == kStatusSuccess;
            ok = d->api.vaEndPicture(d->display, e->vpp_context) == kStatusSuccess && ok;
        }
        d->api.vaDestroyBuffer(d->display, buffer);
    }
    d->api.vaDestroySurfaces(d->display, &source, 1);
    return ok;
}

// Under the lock: a converted picture becomes the newest and the pending one (the newest pending wins).
void enqueue(GpuStreamEncoder* e, u8 index, u64 timestamp_us) noexcept {
    if (e->pending != kNone && e->pending != index)
        saturating_increment(e->stats.dropped);
    e->slots[index].timestamp_us = timestamp_us;
    e->newest = e->pending = index;
    e->last_accepted_us = timestamp_us;
    e->accepted = true;
    ++e->serial;
    event_set(e->wake);
}

// Under the lock: as on Windows, a keyframe request with a capture that has sent nothing for kStillUs (or a frame
// interval, whichever is longer) encodes the newest picture again, now.
void repeat_newest(GpuStreamEncoder* e) noexcept {
    if (!e->keyframe_requested || e->pending != kNone || e->newest == kNone || e->ended ||
        e->slots[e->newest].in_flight)
        return;
    const u64 interval = 1'000'000u / fps_of(e->options.settings);
    const u64 now = now_us();
    if (now >= e->last_accepted_us + (interval > kStillUs ? interval : kStillUs))
        enqueue(e, e->newest, now);
}

bool render(GpuStreamEncoder* e, i32 type, const void* data, u32 bytes) noexcept {
    const Driver* d = e->driver;
    BufferId buffer = kInvalidId;
    if (d->api.vaCreateBuffer(d->display, e->encode_context, type, bytes, 1, const_cast<void*>(data), &buffer) !=
        kStatusSuccess)
        return false;
    const bool ok = d->api.vaRenderPicture(d->display, e->encode_context, &buffer, 1) == kStatusSuccess;
    d->api.vaDestroyBuffer(d->display, buffer);
    return ok;
}

bool rate_control(GpuStreamEncoder* e, u32 bitrate, u32 fps) noexcept {
    Misc<EncMiscRateControl> rate{kEncMiscRateControl, {}};
    rate.parameters.bits_per_second = bitrate;
    rate.parameters.target_percentage = 100;
    rate.parameters.window_size = 1000;
    rate.parameters.rc_flags.bits.reset = 1;
    Misc<EncMiscFrameRate> frame_rate{kEncMiscFrameRate, {}};
    frame_rate.parameters.framerate = fps;
    return render(e, kEncMiscParameterBuffer, &rate, sizeof(rate)) &&
           render(e, kEncMiscParameterBuffer, &frame_rate, sizeof(frame_rate));
}

PictureH264 h264_picture(SurfaceId surface, u32 frame_num, i32 order, u32 flags) noexcept {
    return {surface, frame_num, flags, order, order, {}};
}

bool h264_parameters(GpuStreamEncoder* e, bool idr, u32 bitrate, u32 fps, SurfaceId current, SurfaceId previous) noexcept {
    const bool baseline = e->choice.profile == kProfileH264ConstrainedBaseline;
    const u32 width_mbs = (e->width + 15) / 16, height_mbs = (e->height + 15) / 16;
    const u32 frame_num = e->frames_since_idr % 256;
    const i32 order = static_cast<i32>(e->frames_since_idr * 2); // picture order: twice the frame count, from the IDR
    if (idr) {
        auto* sequence = commit_pages<EncSequenceParameterH264>();
        if (!sequence)
            return false;
        // Level 4.2 takes 1080p at 60 fps; larger pictures need 5.2.
        sequence->level_idc = width_mbs * height_mbs <= 8160 ? 42 : 52;
        sequence->intra_period = sequence->intra_idr_period = kIdrPeriod;
        sequence->ip_period = 1;
        sequence->bits_per_second = bitrate;
        sequence->max_num_ref_frames = 1;
        sequence->picture_width_in_mbs = static_cast<u16>(width_mbs);
        sequence->picture_height_in_mbs = static_cast<u16>(height_mbs);
        sequence->seq_fields.bits.chroma_format_idc = 1;
        sequence->seq_fields.bits.frame_mbs_only_flag = 1;
        sequence->seq_fields.bits.direct_8x8_inference_flag = 1;
        sequence->seq_fields.bits.log2_max_frame_num_minus4 = 4;
        sequence->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 4;
        sequence->frame_cropping_flag = width_mbs * 16 != e->width || height_mbs * 16 != e->height;
        sequence->frame_crop_right_offset = (width_mbs * 16 - e->width) / 2;
        sequence->frame_crop_bottom_offset = (height_mbs * 16 - e->height) / 2;
        sequence->vui_parameters_present_flag = 1;
        sequence->vui_fields.bits.timing_info_present_flag = 1;
        sequence->vui_fields.bits.bitstream_restriction_flag = 1;
        sequence->vui_fields.bits.log2_max_mv_length_horizontal = 15;
        sequence->vui_fields.bits.log2_max_mv_length_vertical = 15;
        sequence->vui_fields.bits.motion_vectors_over_pic_boundaries_flag = 1;
        sequence->num_units_in_tick = 1;
        sequence->time_scale = 2 * fps;
        const bool ok = render(e, kEncSequenceParameterBuffer, sequence, sizeof(*sequence));
        pages_release(sequence, sizeof(*sequence));
        if (!ok)
            return false;
    }
    auto* picture = commit_pages<EncPictureParameterH264>();
    auto* slice = commit_pages<EncSliceParameterH264>();
    bool ok = picture && slice;
    if (ok) {
        picture->curr_pic = h264_picture(current, frame_num, order, 0);
        for (PictureH264& reference : picture->reference_frames)
            reference = h264_picture(kInvalidId, 0, 0, kPictureH264Invalid);
        const PictureH264 last =
            h264_picture(previous, (frame_num + 255) % 256, order - 2, kPictureH264ShortTermReference);
        if (!idr)
            picture->reference_frames[0] = last;
        picture->coded_buf = e->coded;
        picture->frame_num = static_cast<u16>(frame_num);
        picture->pic_init_qp = 26;
        picture->pic_fields.bits.idr_pic_flag = idr;
        picture->pic_fields.bits.reference_pic_flag = 1;
        picture->pic_fields.bits.entropy_coding_mode_flag = !baseline;
        picture->pic_fields.bits.deblocking_filter_control_present_flag = 1;
        slice->num_macroblocks = width_mbs * height_mbs;
        slice->macroblock_info = kInvalidId;
        slice->slice_type = idr ? kH264SliceI : kH264SliceP;
        slice->idr_pic_id = static_cast<u16>(e->idr_count);
        slice->pic_order_cnt_lsb = static_cast<u16>(order % 256);
        for (u32 i = 0; i < 32; ++i)
            slice->ref_pic_list0[i] = slice->ref_pic_list1[i] = h264_picture(kInvalidId, 0, 0, kPictureH264Invalid);
        if (!idr)
            slice->ref_pic_list0[0] = last;
        ok = render(e, kEncPictureParameterBuffer, picture, sizeof(*picture)) &&
             render(e, kEncSliceParameterBuffer, slice, sizeof(*slice));
    }
    if (picture)
        pages_release(picture, sizeof(*picture));
    if (slice)
        pages_release(slice, sizeof(*slice));
    return ok;
}

PictureHevc hevc_picture(SurfaceId surface, i32 order, u32 flags) noexcept {
    PictureHevc picture{};
    picture.picture_id = surface;
    picture.pic_order_cnt = order;
    picture.flags = flags;
    return picture;
}

bool hevc_parameters(GpuStreamEncoder* e, bool idr, u32 bitrate, u32 fps, SurfaceId current, SurfaceId previous) noexcept {
    const i32 order = static_cast<i32>(e->frames_since_idr);
    if (idr) {
        EncSequenceParameterHevc sequence{};
        sequence.general_profile_idc = 1;
        // Level 4.1 takes 1080p at 60 fps; larger pictures need 5.1 (levels are 30 times the number).
        sequence.general_level_idc = u64{e->width} * e->height <= 2'228'224 ? 123 : 153;
        sequence.intra_period = sequence.intra_idr_period = kIdrPeriod;
        sequence.ip_period = 1;
        sequence.bits_per_second = bitrate;
        sequence.pic_width_in_luma_samples = static_cast<u16>(e->width);
        sequence.pic_height_in_luma_samples = static_cast<u16>(e->height);
        sequence.seq_fields.bits.chroma_format_idc = 1;
        sequence.seq_fields.bits.amp_enabled_flag = 1;
        sequence.seq_fields.bits.low_delay_seq = 1;
        sequence.log2_diff_max_min_luma_coding_block_size = 3; // 8x8 to 64x64 coding blocks
        sequence.log2_diff_max_min_transform_block_size = 3;   // 4x4 to 32x32 transforms
        sequence.max_transform_hierarchy_depth_inter = 3;
        sequence.max_transform_hierarchy_depth_intra = 3;
        sequence.vui_parameters_present_flag = 1;
        sequence.vui_fields.bits.vui_timing_info_present_flag = 1;
        sequence.vui_fields.bits.bitstream_restriction_flag = 1;
        sequence.vui_fields.bits.motion_vectors_over_pic_boundaries_flag = 1;
        sequence.vui_fields.bits.log2_max_mv_length_horizontal = 15;
        sequence.vui_fields.bits.log2_max_mv_length_vertical = 15;
        sequence.vui_num_units_in_tick = 1;
        sequence.vui_time_scale = fps;
        if (!render(e, kEncSequenceParameterBuffer, &sequence, sizeof(sequence)))
            return false;
    }
    auto* picture = commit_pages<EncPictureParameterHevc>();
    auto* slice = commit_pages<EncSliceParameterHevc>();
    bool ok = picture && slice;
    if (ok) {
        picture->decoded_curr_pic = hevc_picture(current, order, 0);
        for (PictureHevc& reference : picture->reference_frames)
            reference = hevc_picture(kInvalidId, 0, kPictureHevcInvalid);
        const PictureHevc last = hevc_picture(previous, order - 1, 0);
        if (!idr)
            picture->reference_frames[0] = last;
        picture->coded_buf = e->coded;
        picture->collocated_ref_pic_index = 0xFF;
        picture->pic_init_qp = 30;
        picture->nal_unit_type = idr ? kHevcIdrWRadl : kHevcTrailR;
        picture->pic_fields.bits.idr_pic_flag = idr;
        picture->pic_fields.bits.coding_type = idr ? 1 : 2;
        picture->pic_fields.bits.reference_pic_flag = 1;
        picture->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
        slice->num_ctu_in_slice = ((e->width + 63) / 64) * ((e->height + 63) / 64);
        slice->slice_type = idr ? kHevcSliceI : kHevcSliceP;
        for (u32 i = 0; i < 15; ++i)
            slice->ref_pic_list0[i] = slice->ref_pic_list1[i] = hevc_picture(kInvalidId, 0, kPictureHevcInvalid);
        if (!idr)
            slice->ref_pic_list0[0] = last;
        slice->max_num_merge_cand = 5;
        slice->slice_fields.bits.last_slice_of_pic_flag = 1;
        slice->slice_fields.bits.slice_loop_filter_across_slices_enabled_flag = 1;
        ok = render(e, kEncPictureParameterBuffer, picture, sizeof(*picture)) &&
             render(e, kEncSliceParameterBuffer, slice, sizeof(*slice));
    }
    if (picture)
        pages_release(picture, sizeof(*picture));
    if (slice)
        pages_release(slice, sizeof(*slice));
    return ok;
}

// A packed header: its parameter buffer, then its bytes.
bool packed_header(GpuStreamEncoder* e, u32 type, const u8* data, u32 bytes) noexcept {
    EncPackedHeaderParameter parameter{type, bytes * 8, 0, {}};
    return render(e, kEncPackedHeaderParameterBuffer, &parameter, sizeof(parameter)) &&
           render(e, kEncPackedHeaderDataBuffer, data, bytes);
}

// AV1: the sequence (on keyframes) and picture parameters, the packed sequence and frame header OBUs (av1_headers.hpp)
// and the one tile group. Every slot holds the picture before; every picture refreshes them all.
bool av1_parameters(GpuStreamEncoder* e, bool key, u32 bitrate, SurfaceId current, SurfaceId previous) noexcept {
    const Av1SequenceSettings sequence{e->width, e->height,
                                       static_cast<u8>(u64{e->width} * e->height <= 1920u * 1080u ? 9 : 13), true};
    if (key) {
        EncSequenceParameterAv1 parameters{};
        parameters.seq_level_idx = sequence.level;
        parameters.intra_period = kIdrPeriod;
        parameters.ip_period = 1;
        parameters.bits_per_second = bitrate;
        parameters.seq_fields.bits.enable_order_hint = 1;
        parameters.seq_fields.bits.enable_cdef = sequence.enable_cdef;
        parameters.seq_fields.bits.subsampling_x = parameters.seq_fields.bits.subsampling_y = 1;
        parameters.order_hint_bits_minus_1 = 7;
        const u32 bytes = av1_write_sequence_header(sequence, e->parameters, kParameterBytes);
        e->parameter_bytes = bytes;
        if (!bytes || !render(e, kEncSequenceParameterBuffer, &parameters, sizeof(parameters)) ||
            !packed_header(e, kEncPackedHeaderSequence, e->parameters, bytes))
            return false;
    }
    // Starting values: the driver's rate control rewrites the quantizer, loop filter and CDEF fields at the positions
    // the layout gives.
    Av1FrameSettings frame{};
    frame.key = key;
    frame.order_hint = static_cast<u8>(e->frames_since_idr);
    frame.primary_ref_frame = key ? 7 : 0;
    frame.interpolation_filter = e->choice.av1_filter;
    frame.allow_high_precision_mv = true;
    frame.base_q_idx = 128;
    frame.loop_filter_level[0] = frame.loop_filter_level[1] = frame.loop_filter_level[2] =
        frame.loop_filter_level[3] = 10;
    frame.cdef_y_strengths[0] = frame.cdef_uv_strengths[0] = 4 << 2 | 1;
    frame.tx_mode_select = e->choice.av1_tx_select;
    u8 header[256];
    Av1FrameHeaderLayout layout{};
    const u32 bytes = av1_write_frame_header(sequence, frame, e->choice.av1_size_bytes, header, sizeof(header), &layout);
    auto* picture = commit_pages<EncPictureParameterAv1>();
    bool ok = bytes && picture;
    if (ok) {
        picture->frame_width_minus_1 = static_cast<u16>(e->width - 1);
        picture->frame_height_minus_1 = static_cast<u16>(e->height - 1);
        picture->reconstructed_frame = current;
        picture->coded_buf = e->coded;
        for (SurfaceId& slot : picture->reference_frames)
            slot = key ? kInvalidId : previous;
        picture->primary_ref_frame = frame.primary_ref_frame;
        picture->order_hint = frame.order_hint;
        picture->refresh_frame_flags = 0xFF;
        if (!key)
            picture->ref_frame_ctrl_l0.fields.search_idx0 = 1; // LAST_FRAME
        picture->picture_flags.bits.frame_type = key ? 0 : 1;
        picture->picture_flags.bits.error_resilient_mode = key;
        picture->picture_flags.bits.allow_high_precision_mv = frame.allow_high_precision_mv && !key;
        picture->filter_level[0] = frame.loop_filter_level[0];
        picture->filter_level[1] = frame.loop_filter_level[1];
        picture->filter_level_u = frame.loop_filter_level[2];
        picture->filter_level_v = frame.loop_filter_level[3];
        picture->superres_scale_denominator = 8;
        picture->interpolation_filter = frame.interpolation_filter;
        picture->base_qindex = frame.base_q_idx;
        picture->min_base_qindex = 1;
        picture->max_base_qindex = 255;
        picture->mode_control_flags.bits.tx_mode = frame.tx_mode_select ? 2 : 1;
        picture->tile_cols = picture->tile_rows = 1;
        picture->width_in_sbs_minus_1[0] = static_cast<u16>(av1_superblocks(e->width) - 1);
        picture->height_in_sbs_minus_1[0] = static_cast<u16>(av1_superblocks(e->height) - 1);
        picture->cdef_y_strengths[0] = frame.cdef_y_strengths[0];
        picture->cdef_uv_strengths[0] = frame.cdef_uv_strengths[0];
        picture->bit_offset_qindex = layout.qindex;
        picture->bit_offset_segmentation = layout.segmentation;
        picture->bit_offset_loopfilter_params = layout.loop_filter;
        picture->bit_offset_cdef_params = layout.cdef;
        picture->size_in_bits_cdef_params = layout.cdef_size;
        picture->byte_offset_frame_hdr_obu_size = layout.obu_size_byte;
        picture->size_in_bits_frame_hdr_obu = layout.bits;
        picture->tile_group_obu_hdr_info.bits.obu_has_size_field = 1;
        const EncTileGroupAv1 tile_group{0, 0, {}};
        ok = render(e, kEncPictureParameterBuffer, picture, sizeof(*picture)) &&
             packed_header(e, kEncPackedHeaderPicture, header, bytes) &&
             render(e, kEncSliceParameterBuffer, &tile_group, sizeof(tile_group));
    }
    if (picture)
        pages_release(picture, sizeof(*picture));
    return ok;
}

// Keeps the parameter sets (H.264 SPS and PPS; H.265 VPS, SPS and PPS) at the start of a keyframe the driver wrote, for
// keyframes it writes without them.
void keep_parameters(GpuStreamEncoder* e, ConstSlice<u8> unit) noexcept {
    const bool h264 = e->options.codec == GpuStreamCodec::h264;
    u32 end = 0;
    for (u32 i = 0; i + 4 < unit.count;) {
        if (!(unit.data[i] == 0 && unit.data[i + 1] == 0 && unit.data[i + 2] == 0 && unit.data[i + 3] == 1)) {
            ++i;
            continue;
        }
        const u8 type = h264 ? unit.data[i + 4] & 0x1F : (unit.data[i + 4] >> 1) & 0x3F;
        if (h264 ? type != 7 && type != 8 : type < 32 || type > 34)
            break;
        end = i += 4;
        while (end < unit.count && !(end + 3 < unit.count && unit.data[end] == 0 && unit.data[end + 1] == 0 &&
                                     unit.data[end + 2] == 0 && unit.data[end + 3] == 1))
            ++end;
        i = end;
    }
    if (end && end <= kParameterBytes) {
        copy_bytes(e->parameters, unit.data, end);
        e->parameter_bytes = end;
    }
}

// Encoder thread: encodes slot `index`, waits for it and delivers the unit. False when the driver failed.
bool encode(GpuStreamEncoder* e, u8 index, GpuStreamEncoderSettings settings, bool idr) noexcept {
    const Driver* d = e->driver;
    Slot& slot = e->slots[index];
    if (idr) {
        e->frames_since_idr = 0;
        ++e->idr_count;
    }
    const SurfaceId current = e->reconstructed[e->current], previous = e->reconstructed[e->current ^ 1];
    const u32 fps = fps_of(settings);
    bool ok = d->api.vaSyncSurface(d->display, slot.surface) == kStatusSuccess &&
              d->api.vaBeginPicture(d->display, e->encode_context, slot.surface) == kStatusSuccess;
    if (ok) {
        if (idr || settings.bitrate != e->bitrate_sent || fps != e->fps_sent) {
            ok = rate_control(e, settings.bitrate, fps);
            e->bitrate_sent = settings.bitrate;
            e->fps_sent = fps;
        }
        if (e->options.codec == GpuStreamCodec::av1)
            ok = ok && av1_parameters(e, idr, settings.bitrate, current, previous);
        else
            ok = ok && (e->options.codec == GpuStreamCodec::h264
                            ? h264_parameters(e, idr, settings.bitrate, fps, current, previous)
                            : hevc_parameters(e, idr, settings.bitrate, fps, current, previous));
        ok = d->api.vaEndPicture(d->display, e->encode_context) == kStatusSuccess && ok;
    }
    void* mapped = nullptr;
    ok = ok && d->api.vaSyncSurface(d->display, slot.surface) == kStatusSuccess &&
         d->api.vaMapBuffer(d->display, e->coded, &mapped) == kStatusSuccess;
    if (!ok)
        return false;
    // The coded segments, joined.
    e->unit.count = 0;
    EncodedUnitBuffer joined{};
    for (auto* segment = static_cast<CodedBufferSegment*>(mapped); ok && segment;
         segment = static_cast<CodedBufferSegment*>(segment->next)) {
        ok = encoded_unit_reserve(&joined, joined.count + segment->size);
        if (ok) {
            copy_bytes(joined.data + joined.count, segment->buf, segment->size);
            joined.count += segment->size;
        }
    }
    d->api.vaUnmapBuffer(d->display, e->coded);
    bool written = false;
    if (ok && e->options.codec == GpuStreamCodec::av1) {
        ConstSlice<u8> found{};
        written = encoded_av1_unit({joined.data, joined.count}, {e->parameters, e->parameter_bytes}, idr, &e->unit,
                                   &found);
    } else if (ok) {
        if (idr)
            keep_parameters(e, {joined.data, joined.count});
        written = encoded_annexb_unit(e->options.codec, {joined.data, joined.count},
                                      {e->parameters, e->parameter_bytes}, idr, &e->unit);
    }
    encoded_unit_release(&joined);
    if (!ok)
        return false;
    e->current ^= 1;
    ++e->frames_since_idr;
    if (!written)
        return true; // The driver skipped the picture (rate control): nothing to send.
    const GpuStreamAccessUnit unit{{e->unit.data, e->unit.count},
                                   slot.timestamp_us,
                                   static_cast<u32>(slot.timestamp_us * 9 / 100),
                                   static_cast<u16>(e->width),
                                   static_cast<u16>(e->height),
                                   e->options.codec,
                                   idr};
    AcquireSRWLockExclusive(&e->lock);
    saturating_increment(e->stats.encoded);
    saturating_increment_if(idr, e->stats.keyframes);
    e->stats.bytes += e->unit.count;
    ReleaseSRWLockExclusive(&e->lock);
    e->options.sink.unit(e->options.sink.context, unit);
    return true;
}

u32 encoder_main(void* parameter) noexcept {
    auto* e = static_cast<GpuStreamEncoder*>(parameter);
    for (;;) {
        wait_one(e->wake, INFINITE);
        AcquireSRWLockExclusive(&e->lock);
        if (e->stop) {
            ReleaseSRWLockExclusive(&e->lock);
            return 0;
        }
        const u8 index = e->pending;
        const GpuStreamEncoderSettings settings = e->options.settings;
        const bool idr = e->keyframe_requested || e->frames_since_idr >= kIdrPeriod;
        if (index != kNone) {
            e->pending = kNone;
            e->slots[index].in_flight = true;
            e->keyframe_requested = false;
        }
        ReleaseSRWLockExclusive(&e->lock);
        if (index == kNone)
            continue;
        const bool ok = encode(e, index, settings, idr);
        AcquireSRWLockExclusive(&e->lock);
        e->slots[index].in_flight = false;
        if (!ok)
            e->ended = true;
        else
            repeat_newest(e);
        ReleaseSRWLockExclusive(&e->lock);
        if (!ok) {
            e->options.sink.ended(e->options.sink.context,
                                  gpu_device_lost(e->device) ? GpuStreamEncoderEnd::device_lost
                                                             : GpuStreamEncoderEnd::failed);
            return 0;
        }
    }
}

bool settings_valid(const GpuStreamEncoderSettings& s) noexcept {
    return s.width >= 16 && s.height >= 16 && !((s.width | s.height) & 1) && s.width <= 4096 && s.height <= 4096 &&
           u64{s.width} * s.height <= kGpuVideoVisiblePixels && s.bitrate;
}

void release(GpuStreamEncoder* e) noexcept {
    const Driver* d = e->driver;
    if (d) {
        release_pipeline(e);
        if (e->encode_config != kInvalidId)
            d->api.vaDestroyConfig(d->display, e->encode_config);
        if (e->vpp_config != kInvalidId)
            d->api.vaDestroyConfig(d->display, e->vpp_config);
    }
    if (e->wake)
        waitable_close(e->wake);
    encoded_unit_release(&e->unit);
    pages_release(e, sizeof(GpuStreamEncoder));
}

} // namespace

u32 gpu_stream_encoder_codecs(GpuDevice* device) noexcept {
    const Driver* d = device ? driver(device) : nullptr;
    if (!d)
        return 0;
    u32 codecs = 0;
    for (const GpuStreamCodec codec : kEncodedCodecs)
        if (choose(*d, codec).profile)
            codecs |= gpu_stream_codec_bit(codec);
    return codecs;
}

GpuStreamEncoder* gpu_stream_encoder_create(const GpuStreamEncoderOptions& options) noexcept {
    if (!options.device || options.codec > GpuStreamCodec::av1 || !options.sink.unit || !options.sink.ended ||
        !settings_valid(options.settings))
        return nullptr;
    const Driver* d = driver(options.device);
    const Choice choice = d ? choose(*d, options.codec) : Choice{};
    if (!choice.profile)
        return nullptr;
    auto* e = commit_pages<GpuStreamEncoder>();
    if (!e)
        return nullptr;
    e->options = options;
    e->driver = d;
    e->device = options.device;
    e->choice = choice;
    e->vpp_config = e->encode_config = kInvalidId;
    e->vpp_context = e->encode_context = kInvalidId;
    e->coded = kInvalidId;
    for (Slot& slot : e->slots)
        slot.surface = kInvalidId;
    e->reconstructed[0] = e->reconstructed[1] = kInvalidId;
    e->newest = e->held = e->pending = kNone;
    ConfigAttrib rate{kConfigAttribRateControl, kRcCbr};
    ConfigAttrib encode_attributes[]{
        {kConfigAttribRtFormat, kRtFormatYuv420}, rate,
        {kConfigAttribEncPackedHeaders, options.codec == GpuStreamCodec::av1 ? kAv1PackedHeaders : kEncPackedHeaderNone}};
    bool ok = d->api.vaCreateConfig(d->display, kProfileNone, kEntrypointVideoProc, nullptr, 0, &e->vpp_config) ==
                  kStatusSuccess &&
              d->api.vaCreateConfig(d->display, choice.profile, choice.entrypoint, encode_attributes, 3,
                                    &e->encode_config) == kStatusSuccess &&
              create_pipeline(e);
    e->wake = ok ? event_create(false, false) : nullptr;
    if (e->wake)
        e->thread = thread_start(encoder_main, e, kStackBytes, nullptr);
    if (!e->thread) {
        release(e);
        return nullptr;
    }
    return e;
}

bool gpu_stream_encoder_submit(GpuStreamEncoder* e, const CaptureFrame& frame) noexcept {
    if (!e)
        return false;
    AcquireSRWLockExclusive(&e->lock);
    saturating_increment(e->stats.submitted);
    const GpuStreamEncoderSettings& s = e->options.settings;
    const u64 interval = 1'000'000u / fps_of(s);
    bool accept = !e->ended && !e->stop &&
                  (!e->accepted || (frame.timestamp_us > e->last_accepted_us &&
                                    frame.timestamp_us - e->last_accepted_us + interval / 4 >= interval));
    u32 width = 0, height = 0;
    encoded_extent(e, &width, &height);
    // A new extent: the encoder thread is idle once nothing is pending or in flight; wait for it to finish its picture.
    if (accept && (width != e->width || height != e->height)) {
        bool busy = false;
        for (const Slot& slot : e->slots)
            busy = busy || slot.in_flight;
        if (busy) {
            accept = false;
        } else {
            release_pipeline(e);
            saturating_increment(e->stats.reconfigurations);
            accept = create_pipeline(e);
        }
    }
    Slot* slot = accept ? free_slot(e) : nullptr;
    const bool queued = slot && convert(e, frame, slot);
    if (queued)
        enqueue(e, static_cast<u8>(slot - e->slots), frame.timestamp_us);
    else
        saturating_increment(e->stats.dropped);
    ReleaseSRWLockExclusive(&e->lock);
    return queued;
}

void gpu_stream_encoder_configure(GpuStreamEncoder* e, const GpuStreamEncoderSettings& settings) noexcept {
    if (!e || !settings_valid(settings))
        return;
    AcquireSRWLockExclusive(&e->lock);
    e->options.settings = settings;
    ReleaseSRWLockExclusive(&e->lock);
}

void gpu_stream_encoder_set_bitrate(GpuStreamEncoder* e, u32 bitrate) noexcept {
    if (!e || !bitrate)
        return;
    AcquireSRWLockExclusive(&e->lock);
    e->options.settings.bitrate = bitrate;
    ReleaseSRWLockExclusive(&e->lock);
}

void gpu_stream_encoder_request_keyframe(GpuStreamEncoder* e) noexcept {
    if (!e)
        return;
    AcquireSRWLockExclusive(&e->lock);
    e->keyframe_requested = true;
    repeat_newest(e);
    ReleaseSRWLockExclusive(&e->lock);
}

bool gpu_stream_encoder_preview(GpuStreamEncoder* e, GpuVideoFrame* frame, u32* sequence) noexcept {
    if (!e || !frame || !sequence)
        return false;
    const Driver* d = e->driver;
    AcquireSRWLockExclusive(&e->lock);
    bool ok = e->newest != kNone;
    if (ok) {
        e->held = e->newest;
        DrmPrimeDescriptor descriptor{};
        ExportLayout layout{};
        GpuTexture *luma_texture = nullptr, *chroma_texture = nullptr;
        GpuSrv *luma = nullptr, *chroma = nullptr;
        ok = d->api.vaSyncSurface(d->display, e->slots[e->held].surface) == kStatusSuccess &&
             d->api.vaExportSurfaceHandle(d->display, e->slots[e->held].surface, kSurfaceMemoryTypeDrmPrime2,
                                          kExportSurfaceReadOnly | kExportSurfaceSeparateLayers,
                                          &descriptor) == kStatusSuccess;
        ok = ok && export_layout(descriptor, &layout) &&
             gpu_ok(gpu_video_plane_import(e->device, layout.plane[0], &luma_texture, &luma)) &&
             gpu_ok(gpu_video_plane_import(e->device, layout.plane[1], &chroma_texture, &chroma));
        close_descriptor_objects(descriptor);
        if (ok) {
            *frame = {};
            frame->texture = luma_texture;
            frame->width = e->width;
            frame->height = e->height;
            frame->bt709 = picture_bt709(e);
            frame->luma = luma;
            frame->chroma = chroma;
            *sequence = e->serial;
            gpu_release(chroma_texture); // The view keeps its plane.
        } else {
            gpu_release(luma);
            gpu_release(luma_texture);
            gpu_release(chroma);
            gpu_release(chroma_texture);
        }
    }
    ReleaseSRWLockExclusive(&e->lock);
    return ok;
}

GpuStreamEncoderStats gpu_stream_encoder_stats(const GpuStreamEncoder* e) noexcept {
    if (!e)
        return {};
    auto* lock = const_cast<SRWLOCK*>(&e->lock);
    AcquireSRWLockShared(lock);
    const GpuStreamEncoderStats stats = e->stats;
    ReleaseSRWLockShared(lock);
    return stats;
}

void gpu_stream_encoder_destroy(GpuStreamEncoder* e) noexcept {
    if (!e)
        return;
    AcquireSRWLockExclusive(&e->lock);
    e->stop = true;
    ReleaseSRWLockExclusive(&e->lock);
    event_set(e->wake);
    thread_wait(e->thread, INFINITE);
    thread_close(e->thread);
    release(e);
}

} // namespace maicord
