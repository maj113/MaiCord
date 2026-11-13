#include "stream.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/video/av1.hpp"
#include "maicord/platform/decode/video_capability.hpp"
#include "../software/software_decoder.hpp"
#include "../vaapi/va.hpp"

// The Linux half of src/platform/decode/gpu_stream_decoder.cpp: the choice of tier, the switch to software, and the
// picture slots of the backends of stream.hpp.

namespace maicord {

void stream_fall_back_to_software(GpuStream* stream) noexcept {
    stream->backend->release(stream);
    stream->backend = &kSoftwareStreamBackend;
    clear_bytes(&stream->h264, static_cast<u32>(sizeof(GpuStream) - __builtin_offsetof(GpuStream, h264)));
    stream->backend->reset(stream);
    stream->stats.unsupported = false;
    stream->last_request_ms = 0;
    log_u64(LogLevel::info, "video", "hardware_decode_fell_back_to_software", static_cast<u64>(stream->options.codec));
    stream_request_keyframe(stream, false);
}

// ---- Backend services -------------------------------------------------------------------------------------------

void stream_slot_release(StreamSurface* slot) noexcept {
    gpu_release(slot->chroma_view);
    gpu_release(slot->chroma_texture);
    gpu_release(slot->luma_view);
    gpu_release(slot->luma_texture);
}

void stream_surfaces_release(GpuStream* stream) noexcept {
    for (u32 i = 0; i < stream->surfaces.count; ++i)
        stream_slot_release(&stream->surfaces.slot[i]);
    reset_value(&stream->surfaces);
    reset_value(&stream->frame);
    stream->presented = 255;
    stream->coded_width = stream->coded_height = 0;
}

bool stream_slot_export(GpuDevice* device, vaapi::Session* session, u32 surface, StreamSurface* slot) noexcept {
    vaapi::ExportedPicture picture{};
    if (!vaapi::export_picture(device, session, surface, &picture))
        return false;
    stream_slot_release(slot);
    slot->luma_texture = picture.luma_texture;
    slot->luma_view = picture.luma_view;
    slot->chroma_texture = picture.chroma_texture;
    slot->chroma_view = picture.chroma_view;
    return true;
}

bool stream_surfaces_create_cpu(GpuStream* stream, u32 count, u32 coded_width, u32 coded_height) noexcept {
    stream_surfaces_release(stream);
    if (!coded_width || !coded_height || ((coded_width | coded_height) & 1u) ||
        !stream_slots_create_cpu(stream->device, stream->surfaces.slot, &stream->surfaces.count, count, coded_width,
                                 coded_height))
        return false;
    stream->surfaces.width = coded_width;
    stream->surfaces.height = coded_height;
    stream->coded_width = coded_width;
    stream->coded_height = coded_height;
    return true;
}

bool stream_unit_is_keyframe(GpuStreamCodec codec, ConstSlice<u8> bytes) noexcept {
    if (codec == GpuStreamCodec::av1) {
        u32 offset = 0;
        Av1Obu obu{};
        while (offset < bytes.count && av1_next_obu(bytes, &offset, &obu) == Av1Status::ok)
            if (obu.type == kAv1ObuSequenceHeader)
                return true;
        return false;
    }
    H264AnnexBCursor cursor{bytes, 0};
    for (;;) {
        const H264Nal nal = h264_annexb_next(&cursor);
        if (nal.error == H264Error::end || nal.error == H264Error::invalid_input)
            return false;
        if (nal.error == H264Error::none && (codec == GpuStreamCodec::h264 ? nal.type == 5 : false))
            return true;
    }
}

// ---- gpu_stream_decoder.hpp -------------------------------------------------------------------------------------

u32 gpu_stream_decoder_codecs(GpuDevice* device) noexcept {
    VideoDecodeProbe probe{};
    probe.hardware = device ? hardware_probe_codecs(device) : 0;
    for (u8 codec = 0; codec <= static_cast<u8>(GpuStreamCodec::av1); ++codec)
        if (software_codec_available(static_cast<GpuStreamCodec>(codec)))
            probe.software |= 1u << codec;
    return video_decode_codecs_word(probe);
}

const StreamBackend* stream_backend(GpuDevice* device, GpuStreamCodec codec) noexcept {
    if (hardware_probe_codecs(device) & gpu_stream_codec_bit(codec))
        if (const StreamBackend* hardware = hardware_stream_backend(codec))
            return hardware;
    if ((kVideoDecodeSoftwareCapable & gpu_stream_codec_bit(codec)) && software_codec_available(codec))
        return &kSoftwareStreamBackend;
    return nullptr;
}

} // namespace maicord
