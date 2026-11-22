#include "maicord/media/image/animated_webp.hpp"
#include "maicord/core/memory.hpp"
#include "webp_chunks.hpp"

// The chunk walk runs once per image and frame: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {

inline AnimatedWebpParseResult parse_failure(AnimatedWebpParseResult value, AnimatedWebpError error) noexcept {
    value.error = error;
    return value;
}

bool chunk_bounds(ConstSlice<u8> encoded, u32 cursor, u32 limit, u32* type, u32* size, u32* payload,
                  u32* end) noexcept {
    if (cursor > limit || limit - cursor < 8) {
        return false;
    }
    *type = byte_load_le32(encoded.data + cursor);
    *size = byte_load_le32(encoded.data + cursor + 4);
    const u64 payload_end = static_cast<u64>(cursor) + 8u + *size;
    const u64 chunk_end = payload_end + (*size & 1u);
    if (payload_end > limit || chunk_end > limit ||
        ((*size & 1u) != 0 && encoded[static_cast<u32>(payload_end)] != 0)) {
        return false;
    }
    *payload = cursor + 8;
    *end = static_cast<u32>(chunk_end);
    return true;
}

bool top_level_type(u32 type) noexcept {
    return type == kVp8x || type == kAnim || type == kAnmf || type == kIccp || type == kExif ||
           type == kXmp;
}

u32 bgra_to_rgba(const u8* bytes) noexcept {
    return static_cast<u32>(bytes[2]) | static_cast<u32>(bytes[1]) << 8 | static_cast<u32>(bytes[0]) << 16 |
           static_cast<u32>(bytes[3]) << 24;
}

AnimatedWebpError parse_frame(ConstSlice<u8> encoded, u32 payload, u32 size, AnimatedWebpImage* image,
                              AnimatedWebpFrame* frame) noexcept {
    if (size <= 16) {
        return AnimatedWebpError::chunk;
    }
    const u8* control = encoded.data + payload;
    const u32 stored_x = byte_load_le24(control);
    const u32 stored_y = byte_load_le24(control + 3);
    const u32 width = byte_load_le24(control + 6) + 1;
    const u32 height = byte_load_le24(control + 9) + 1;
    const u32 duration_ms = byte_load_le24(control + 12);
    const u8 flags = control[15];
    if ((flags & 0xFCu) != 0) {
        return AnimatedWebpError::unsupported_feature;
    }
    if (stored_x > kWebpDimensionMaximum / 2 || stored_y > kWebpDimensionMaximum / 2 ||
        width > kWebpDimensionMaximum || height > kWebpDimensionMaximum) {
        return AnimatedWebpError::dimensions;
    }
    const u32 x = stored_x * 2;
    const u32 y = stored_y * 2;
    if (x > image->width || y > image->height || width > static_cast<u32>(image->width) - x ||
        height > static_cast<u32>(image->height) - y) {
        return AnimatedWebpError::rectangle;
    }

    reset_value<AnimatedWebpFrame>(frame);
    frame->duration_ms = duration_ms;
    frame->x_offset = static_cast<u16>(x);
    frame->y_offset = static_cast<u16>(y);
    frame->width = static_cast<u16>(width);
    frame->height = static_cast<u16>(height);
    frame->dispose = (flags & 1u) != 0 ? AnimatedWebpDispose::background : AnimatedWebpDispose::none;
    frame->blend = (flags & 2u) != 0 ? AnimatedWebpBlend::source : AnimatedWebpBlend::over;

    const u32 limit = payload + size;
    u32 cursor = payload + 16;
    frame->payload_offset = cursor;
    bool image_seen = false;
    bool alpha_seen = false;
    bool unknown_seen = false;
    while (cursor < limit) {
        u32 type = 0;
        u32 child_size = 0;
        u32 child_payload = 0;
        u32 child_end = 0;
        if (!chunk_bounds(encoded, cursor, limit, &type, &child_size, &child_payload, &child_end)) {
            return AnimatedWebpError::chunk;
        }
        if (type == kVp8l) {
            if (image_seen || alpha_seen || unknown_seen || child_size < 5) {
                return image_seen ? AnimatedWebpError::duplicate_chunk : AnimatedWebpError::order;
            }
            const u8* vp8l = encoded.data + child_payload;
            const u32 header = byte_load_le32(vp8l + 1);
            const u32 bitstream_width = (header & 0x3FFFu) + 1;
            const u32 bitstream_height = (header >> 14 & 0x3FFFu) + 1;
            if (vp8l[0] != 0x2Fu || (header >> 29) != 0) {
                return AnimatedWebpError::chunk;
            }
            if (bitstream_width != width || bitstream_height != height) {
                return AnimatedWebpError::dimensions;
            }
            frame->has_alpha = (header & (1u << 28)) != 0;
            frame->codec = AnimatedWebpCodec::vp8l_lossless;
            image_seen = true;
        } else if (type == kAlph) {
            if (image_seen || alpha_seen || unknown_seen || child_size < 1) {
                return alpha_seen ? AnimatedWebpError::duplicate_chunk : AnimatedWebpError::order;
            }
            alpha_seen = true;
        } else if (type == kVp8) {
            if (image_seen || unknown_seen || child_size < 10) {
                return image_seen ? AnimatedWebpError::duplicate_chunk : AnimatedWebpError::order;
            }
            Vp8Keyframe keyframe{};
            const Vp8ParseResult parsed =
                vp8_parse_keyframe(ConstSlice<u8>{encoded.data + child_payload, child_size}, &keyframe);
            if (!parsed.ok()) {
                return AnimatedWebpError::chunk;
            }
            if (keyframe.width != width || keyframe.height != height) {
                return AnimatedWebpError::dimensions;
            }
            frame->has_alpha = alpha_seen;
            frame->codec = AnimatedWebpCodec::vp8_lossy;
            image_seen = true;
        } else {
            if (!image_seen || top_level_type(type)) {
                return AnimatedWebpError::order;
            }
            unknown_seen = true;
        }
        cursor = child_end;
    }
    return image_seen && cursor == limit ? AnimatedWebpError::none : AnimatedWebpError::chunk;
}

} // namespace

AnimatedWebpParseResult animated_webp_parse(ConstSlice<u8> encoded, AnimatedWebpImage* image,
                                            AnimatedWebpWorkspace* workspace) noexcept {
    AnimatedWebpParseResult result{};
    // Serialized direct callers can replace this table between pipeline calls.
    // Invalidate before any frame write or growth callback, including failure.
    workspace->retained.content_identity = 0;
    reset_value<AnimatedWebpImage>(image);
    AnimatedWebpFrame* frames = workspace->extended_frames.data
                                    ? workspace->extended_frames.data : workspace->frames;
    u32 capacity = workspace->extended_frames.data
                       ? workspace->extended_frames.count : kAnimatedWebpInlineFrameCapacity;
    image->frames = frames;
    if (encoded.count > kAnimatedWebpEncodedMaximumBytes) {
        return parse_failure(result, AnimatedWebpError::encoded_limit);
    }
    if (encoded.count < 12) {
        return parse_failure(result, AnimatedWebpError::truncated);
    }
    if (byte_load_le32(encoded.data) != kRiff || byte_load_le32(encoded.data + 8) != kWebp ||
        byte_load_le32(encoded.data + 4) != encoded.count - 8 || (encoded.count & 1u) != 0) {
        return parse_failure(result, AnimatedWebpError::riff);
    }

    bool vp8x_seen = false;
    bool anim_seen = false;
    bool iccp_seen = false;
    bool exif_seen = false;
    bool xmp_seen = false;
    bool trailing_seen = false;
    bool unknown_seen = false;
    bool alpha_flag = false;
    bool iccp_flag = false;
    bool exif_flag = false;
    bool xmp_flag = false;
    bool frame_alpha = false;
    u32 cursor = 12;
    while (cursor < encoded.count) {
        u32 type = 0;
        u32 size = 0;
        u32 payload = 0;
        u32 end = 0;
        if (!chunk_bounds(encoded, cursor, encoded.count, &type, &size, &payload, &end)) {
            return parse_failure(result, AnimatedWebpError::chunk);
        }
        const u8* data = encoded.data + payload;
        if (type == kVp8x) {
            if (vp8x_seen || cursor != 12 || size != 10) {
                return parse_failure(result, vp8x_seen ? AnimatedWebpError::duplicate_chunk
                                                       : AnimatedWebpError::order);
            }
            const u8 flags = data[0];
            if ((flags & 0xC1u) != 0 || data[1] != 0 || data[2] != 0 || data[3] != 0 ||
                (flags & 0x02u) == 0) {
                return parse_failure(result, AnimatedWebpError::unsupported_feature);
            }
            const u32 width = byte_load_le24(data + 4) + 1;
            const u32 height = byte_load_le24(data + 7) + 1;
            if (width > kWebpDimensionMaximum || height > kWebpDimensionMaximum ||
                static_cast<u64>(width) * height > kWebpPixelMaximum) {
                return parse_failure(result, AnimatedWebpError::dimensions);
            }
            image->width = static_cast<u16>(width);
            image->height = static_cast<u16>(height);
            alpha_flag = (flags & 0x10u) != 0;
            iccp_flag = (flags & 0x20u) != 0;
            exif_flag = (flags & 0x08u) != 0;
            xmp_flag = (flags & 0x04u) != 0;
            vp8x_seen = true;
        } else if (type == kIccp) {
            if (!vp8x_seen || anim_seen || iccp_seen || !iccp_flag || size == 0) {
                return parse_failure(result, iccp_seen ? AnimatedWebpError::duplicate_chunk
                                                       : AnimatedWebpError::order);
            }
            iccp_seen = true;
        } else if (type == kAnim) {
            if (!vp8x_seen || anim_seen || image->frame_count != 0 || trailing_seen || size != 6 ||
                iccp_seen != iccp_flag) {
                return parse_failure(result, anim_seen ? AnimatedWebpError::duplicate_chunk
                                                       : AnimatedWebpError::order);
            }
            image->background_rgba = bgra_to_rgba(data);
            image->play_count = byte_load_le16(data + 4);
            anim_seen = true;
        } else if (type == kAnmf) {
            if (!anim_seen || trailing_seen) {
                return parse_failure(result, AnimatedWebpError::order);
            }
            if (image->frame_count == kAnimatedWebpFrameCapacity) {
                return parse_failure(result, AnimatedWebpError::frame_count);
            }
            if (image->frame_count >= capacity) {
                if (!workspace->grow_frames ||
                    !workspace->grow_frames(workspace, image->frame_count + 1) ||
                    !workspace->extended_frames.data ||
                    workspace->extended_frames.count <= image->frame_count) {
                    return parse_failure(result, AnimatedWebpError::output_capacity);
                }
                if (frames == workspace->frames) {
                    for (u32 index = 0; index < image->frame_count; ++index)
                        workspace->extended_frames[index] = frames[index];
                }
                frames = workspace->extended_frames.data;
                capacity = workspace->extended_frames.count;
                image->frames = frames;
            }
            AnimatedWebpFrame& frame = frames[image->frame_count];
            const AnimatedWebpError frame_error =
                parse_frame(encoded, payload, size, image, &frame);
            if (frame_error != AnimatedWebpError::none) {
                return parse_failure(result, frame_error);
            }
            frame_alpha = frame_alpha || frame.has_alpha;
            ++image->frame_count;
        } else if (type == kExif || type == kXmp) {
            bool* seen = type == kExif ? &exif_seen : &xmp_seen;
            const bool present = type == kExif ? exif_flag : xmp_flag;
            if (!anim_seen || image->frame_count == 0 || *seen || !present || size == 0 || unknown_seen) {
                return parse_failure(result,
                                     *seen ? AnimatedWebpError::duplicate_chunk : AnimatedWebpError::order);
            }
            *seen = true;
            trailing_seen = true;
        } else if (type == kVp8 || type == kVp8l || type == kAlph) {
            return parse_failure(result, AnimatedWebpError::order);
        } else {
            if (!anim_seen || image->frame_count == 0) {
                return parse_failure(result, AnimatedWebpError::order);
            }
            trailing_seen = true;
            unknown_seen = true;
        }
        cursor = end;
    }
    // Some animation producers retain the VP8X alpha hint when every frame is
    // opaque. Decode alpha from each frame's payload, while still rejecting
    // actual alpha that the container did not advertise.
    if (cursor != encoded.count || !vp8x_seen || !anim_seen || image->frame_count == 0 ||
        iccp_seen != iccp_flag || exif_seen != exif_flag || xmp_seen != xmp_flag ||
        (frame_alpha && !alpha_flag)) {
        return parse_failure(result, image->frame_count == 0 ? AnimatedWebpError::frame_count
                                                             : AnimatedWebpError::order);
    }
    image->has_alpha = alpha_flag;
    result.error = AnimatedWebpError::none;
    return result;
}

// The frame comes from animated_webp_parse over these bytes, or from the table retained for their content
// identity, which the encoded cache gives only to equal bytes. parse_frame admitted the frame's chunks only as an
// optional ALPH chunk (lossy frames, alpha_seen = has_alpha), then the image chunk at once, then other chunks.
void animated_webp_frame_payloads(ConstSlice<u8> encoded, const AnimatedWebpFrame& frame,
                                  ConstSlice<u8>* image_payload, ConstSlice<u8>* alpha_payload) noexcept {
    u32 cursor = frame.payload_offset;
    *alpha_payload = {};
    if (frame.codec == AnimatedWebpCodec::vp8_lossy && frame.has_alpha) {
        const u32 size = byte_load_le32(encoded.data + cursor + 4);
        *alpha_payload = ConstSlice<u8>{encoded.data + cursor + 8, size};
        cursor += 8 + size + (size & 1u);
    }
    *image_payload = ConstSlice<u8>{encoded.data + cursor + 8, byte_load_le32(encoded.data + cursor + 4)};
}

// The media pipeline asks for a frame below the frame count, with the workspaces that frame's codec needs.
AnimatedWebpDecodeResult animated_webp_decode_parsed_frame(
    ConstSlice<u8> encoded, const AnimatedWebpImage& image, u32 frame_index, Vp8lWorkspace* vp8l_workspace,
    LossyWebpWorkspace* lossy_workspace, Vp8Keyframe* vp8_frame, Vp8ControlHeader* vp8_header,
    Slice<u8> rgba_output, Slice<u8> alpha_rgba_scratch, Slice<u8> alpha_output) noexcept {
    AnimatedWebpDecodeResult result{};
    result.frame = image.frames[frame_index];
    result.frame_index = frame_index;
    ConstSlice<u8> image_payload{};
    ConstSlice<u8> alpha_payload{};
    animated_webp_frame_payloads(encoded, result.frame, &image_payload, &alpha_payload);

    if (result.frame.codec == AnimatedWebpCodec::vp8l_lossless) {
        result.frame_decode =
            webp_lossless_decode_vp8l_frame_rgba8(image_payload, result.frame.width, result.frame.height,
                                                  &result.frame_image, vp8l_workspace, rgba_output);
        if (!result.frame_decode.ok()) {
            result.error = result.frame_decode.error == WebpError::output_capacity
                               ? AnimatedWebpError::output_capacity
                               : AnimatedWebpError::frame_decode;
            return result;
        }
    } else {
        result.lossy_decode = lossy_webp_decode_frame_keyframe(
            image_payload, alpha_payload, result.frame.width, result.frame.height, vp8_frame, vp8_header,
            &lossy_workspace->probabilities, &lossy_workspace->modes, &lossy_workspace->coefficients,
            vp8l_workspace, alpha_rgba_scratch, alpha_output);
        if (!result.lossy_decode.ok()) {
            result.error = AnimatedWebpError::frame_decode;
            return result;
        }
        result.frame_image.width = result.frame.width;
        result.frame_image.height = result.frame.height;
        result.frame_image.codec = WebpCodec::vp8_lossy;
        result.frame_image.has_alpha = result.frame.has_alpha;
    }
    const u64 source_us = static_cast<u64>(result.frame.duration_ms) * 1'000;
    result.duration_us = source_us < kAnimatedWebpFrameFloorUs ? kAnimatedWebpFrameFloorUs : source_us;
    result.error = AnimatedWebpError::none;
    return result;
}

} // namespace maicord
#pragma clang attribute pop
