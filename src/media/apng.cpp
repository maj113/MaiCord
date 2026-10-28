#include "maicord/media/apng.hpp"
#include "maicord/core/memory.hpp"

#include "png_common.hpp"

namespace maicord {
namespace {

ApngParseResult fail(ApngParseResult result, ApngError error) noexcept {
    result.error = error;
    return result;
}

void initialize_image(ApngImage* image,
                      ApngWorkspace* workspace) noexcept {
    reset_value<ApngImage>(image);
    image->frames = workspace->frames;
    image->spans = workspace->spans;
    image->frame_capacity = kApngFrameCapacity;
    image->span_capacity = kApngDataSpanCapacity;
    image->canvas.transparent_gray = ~0u;
    image->canvas.transparent_red = ~0u;
    image->canvas.transparent_green = ~0u;
    image->canvas.transparent_blue = ~0u;
}

bool append_span(ApngImage* image, ApngFrame* frame, u32 offset,
                 u32 bytes, bool default_data) noexcept {
    if (bytes == 0) {
        return true;
    }
    if (image->span_count >= image->span_capacity ||
        (frame && frame->span_count >= kApngFrameDataSpanCapacity)) {
        return false;
    }
    if (image->frame_count == 0 && image->span_count == 0) {
        image->spans[kApngRetainedSpanCapacity].offset = 0;
    }
    image->spans[image->span_count++] = ApngDataSpan{offset, bytes};
    if (frame) {
        ++frame->span_count;
        if (bytes > kPngEncodedMaximumBytes - frame->compressed_bytes) {
            return false;
        }
        frame->compressed_bytes += bytes;
    }
    if (default_data) {
        ++image->default_span_count;
        if (bytes > kPngEncodedMaximumBytes -
                        image->default_compressed_bytes) {
            return false;
        }
        image->default_compressed_bytes += bytes;
    }
    return true;
}

bool frame_complete(const ApngFrame& frame) noexcept {
    return frame.span_count != 0 && frame.compressed_bytes >= 6;
}

u64 apng_frame_duration_us(const ApngFrame& frame) noexcept {
    const u32 denominator = frame.delay_den == 0 ? 100 : frame.delay_den;
    const u64 scaled = static_cast<u64>(frame.delay_num) * 1'000'000ull;
    u64 duration = scaled / denominator;
    if (scaled % denominator != 0) {
        ++duration;
    }
    duration = maximum(duration, kApngCompatibilityFrameFloorUs);
    return minimum(duration, kApngFrameDurationMaximumUs);
}

bool bind_spans(ConstSlice<u8> encoded, const ApngImage& image, u32 first, u32 count, u32 compressed_bytes,
                PngImage* out) noexcept {
    if (count == 0 || count > kPngIdatSpanCapacity || first > image.span_count ||
        count > image.span_count - first) {
        return false;
    }
    out->compressed_bytes = 0;
    for (u32 index = 0; index < count; ++index) {
        const ApngDataSpan span = image.spans[first + index];
        if (span.offset > encoded.count || span.bytes > encoded.count - span.offset) {
            return false;
        }
        out->idat_spans[index] = DeflateInputSpan{encoded.data + span.offset, span.bytes};
        out->compressed_bytes += span.bytes;
    }
    out->idat_span_count = count;
    return out->compressed_bytes == compressed_bytes;
}

bool configure_frame_image(ConstSlice<u8> encoded, const ApngImage& image, const ApngFrame& frame,
                           PngImage* frame_image) noexcept {
    *frame_image = image.canvas;
    return png_image_configure(frame_image, frame.width, frame.height, image.canvas.bit_depth,
                               image.canvas.color_type) &&
           bind_spans(encoded, image, frame.first_span, frame.span_count, frame.compressed_bytes, frame_image);
}

bool configure_default_image(ConstSlice<u8> encoded, const ApngImage& image, PngImage* default_image) noexcept {
    *default_image = image.canvas;
    return bind_spans(encoded, image, image.default_first_span, image.default_span_count,
                      image.default_compressed_bytes, default_image);
}

} // namespace

ApngParseResult apng_parse(ConstSlice<u8> encoded, ApngImage* image,
                           ApngWorkspace* workspace) noexcept {
    ApngParseResult result{};
    if (!image || !workspace || (!encoded.data && encoded.count != 0)) {
        return fail(result, ApngError::invalid_input);
    }
    initialize_image(image, workspace);
    if (encoded.count > kPngEncodedMaximumBytes) {
        return fail(result, ApngError::encoded_limit);
    }
    if (encoded.count < sizeof(kPngSignature)) {
        return fail(result, ApngError::truncated);
    }
    for (u32 index = 0; index < sizeof(kPngSignature); ++index) {
        if (encoded[index] != kPngSignature[index]) {
            return fail(result, ApngError::signature);
        }
    }

    bool header_seen = false;
    bool animation_seen = false;
    bool palette_seen = false;
    bool transparency_seen = false;
    bool idat_seen = false;
    bool idat_closed = false;
    bool end_seen = false;
    u32 declared_frames = 0;
    u32 expected_sequence = 0;
    i32 current_frame = -1;
    u32 cursor = sizeof(kPngSignature);
    while (cursor < encoded.count) {
        if (encoded.count - cursor < 12) {
            return fail(result, ApngError::truncated);
        }
        const u32 length = byte_load_be32(encoded.data + cursor);
        const u64 end = static_cast<u64>(cursor) + 12u + length;
        if (length > 0x7FFFFFFFu || end > encoded.count) {
            return fail(result, ApngError::truncated);
        }
        const u8* type_bytes = encoded.data + cursor + 4;
        if (!png_chunk_type_valid(type_bytes) || (type_bytes[2] & 0x20u) != 0) {
            return fail(result, ApngError::chunk_type);
        }
        const u32 type = byte_load_be32(type_bytes);
        const u8* data = type_bytes + 4;
        if (png_crc32(type_bytes, length + 4) !=
            byte_load_be32(data + length)) {
            return fail(result, ApngError::chunk_crc);
        }
        ++result.chunks;
        if (!header_seen && type != kChunkIhdr) {
            return fail(result, ApngError::chunk_order);
        }
        if (idat_seen && type != kChunkIdat) {
            idat_closed = true;
        }

        if (type == kChunkIhdr) {
            if (header_seen || length != 13) {
                return fail(result, header_seen
                                        ? ApngError::duplicate_chunk
                                        : ApngError::unsupported_format);
            }
            if (data[12] != 0) {
                return fail(result, data[12] == 1
                                        ? ApngError::unsupported_interlace
                                        : ApngError::unsupported_format);
            }
            if (data[10] != 0 || data[11] != 0 ||
                !png_image_configure(&image->canvas, byte_load_be32(data),
                                     byte_load_be32(data + 4), data[8],
                                     data[9])) {
                return fail(result,
                            byte_load_be32(data) > kPngDimensionMaximum ||
                                    byte_load_be32(data + 4) >
                                        kPngDimensionMaximum
                                ? ApngError::dimensions
                                : ApngError::unsupported_format);
            }
            header_seen = true;
        } else if (type == kChunkActl) {
            if (!header_seen || animation_seen || idat_seen ||
                length != 8) {
                return fail(result, animation_seen
                                        ? ApngError::duplicate_chunk
                                        : ApngError::chunk_order);
            }
            declared_frames = byte_load_be32(data);
            if (declared_frames == 0 ||
                declared_frames > image->frame_capacity) {
                return fail(result, ApngError::frame_limit);
            }
            image->play_count = byte_load_be32(data + 4);
            animation_seen = true;
        } else if (type == kChunkPlte) {
            if (idat_seen || palette_seen || transparency_seen ||
                !png_apply_palette(&image->canvas, data, length)) {
                return fail(result,
                            palette_seen ? ApngError::duplicate_chunk
                                         : ApngError::palette);
            }
            palette_seen = true;
        } else if (type == kChunkTrns) {
            if (idat_seen || transparency_seen ||
                !png_apply_transparency(&image->canvas, data, length)) {
                return fail(result,
                            transparency_seen
                                ? ApngError::duplicate_chunk
                                : ApngError::transparency);
            }
            transparency_seen = true;
        } else if (type == kChunkFctl) {
            if (!animation_seen || length != 26 ||
                image->frame_count >= declared_frames ||
                (current_frame >= 0 &&
                 !frame_complete(image->frames[current_frame]))) {
                return fail(result, ApngError::frame_control);
            }
            if (byte_load_be32(data) != expected_sequence) {
                return fail(result, ApngError::frame_sequence);
            }
            ++expected_sequence;
            ++result.sequence_chunks;
            ApngFrame frame{};
            frame.first_span = image->span_count;
            frame.width = byte_load_be32(data + 4);
            frame.height = byte_load_be32(data + 8);
            frame.x_offset = byte_load_be32(data + 12);
            frame.y_offset = byte_load_be32(data + 16);
            frame.delay_num = byte_load_be16(data + 20);
            frame.delay_den = byte_load_be16(data + 22);
            const u8 dispose = data[24];
            const u8 blend = data[25];
            if (frame.width == 0 || frame.height == 0 ||
                frame.x_offset > image->canvas.width ||
                frame.y_offset > image->canvas.height ||
                frame.width > image->canvas.width - frame.x_offset ||
                frame.height > image->canvas.height - frame.y_offset ||
                dispose > 2 || blend > 1) {
                return fail(result, ApngError::frame_control);
            }
            frame.dispose = static_cast<ApngDispose>(dispose);
            frame.blend = static_cast<ApngBlend>(blend);
            if (image->frame_count == 0 && !idat_seen) {
                image->default_in_animation = true;
                if (frame.x_offset != 0 || frame.y_offset != 0 ||
                    frame.width != image->canvas.width ||
                    frame.height != image->canvas.height) {
                    return fail(result, ApngError::frame_control);
                }
            }
            if (image->frame_count == 0 &&
                frame.dispose == ApngDispose::previous) {
                frame.dispose = ApngDispose::background;
            }
            // Invalidate before the first table write. Earlier errors leave a
            // previous table intact and do not touch another workspace page.
            if (image->frame_count == 0 && image->span_count == 0) {
                image->spans[kApngRetainedSpanCapacity].offset = 0;
            }
            image->frames[image->frame_count] = frame;
            current_frame = static_cast<i32>(image->frame_count++);
        } else if (type == kChunkIdat) {
            if (!animation_seen || idat_closed ||
                (image->canvas.color_type == 3 && !palette_seen) ||
                (current_frame >= 0 &&
                 (!image->default_in_animation || current_frame != 0))) {
                return fail(result, ApngError::chunk_order);
            }
            if (!idat_seen) {
                image->default_first_span = image->span_count;
            }
            ApngFrame* frame =
                image->default_in_animation
                    ? &image->frames[0]
                    : nullptr;
            if (!append_span(image, frame, cursor + 8, length, true)) {
                return fail(result, ApngError::span_limit);
            }
            idat_seen = true;
        } else if (type == kChunkFdat) {
            if (!animation_seen || !idat_seen || current_frame < 0 ||
                (image->default_in_animation && current_frame == 0) ||
                length < 4) {
                return fail(result, ApngError::frame_data);
            }
            if (byte_load_be32(data) != expected_sequence) {
                return fail(result, ApngError::frame_sequence);
            }
            ++expected_sequence;
            ++result.sequence_chunks;
            if (!append_span(image, &image->frames[current_frame],
                             cursor + 12, length - 4, false)) {
                return fail(result, ApngError::span_limit);
            }
        } else if (type == kChunkIend) {
            if (!animation_seen || !idat_seen || length != 0 ||
                end != encoded.count || end_seen) {
                return fail(result, ApngError::chunk_order);
            }
            end_seen = true;
        } else {
            const bool ancillary = (type_bytes[0] & 0x20u) != 0;
            if (!ancillary) {
                return fail(result, ApngError::unknown_critical_chunk);
            }
            image->canvas.has_color_metadata =
                image->canvas.has_color_metadata || png_color_metadata_chunk(type);
        }
        cursor = static_cast<u32>(end);
        if (end_seen) {
            break;
        }
    }
    if (!animation_seen) {
        return fail(result, ApngError::missing_animation);
    }
    if (!end_seen || !idat_seen || image->frame_count != declared_frames ||
        current_frame < 0 ||
        !frame_complete(image->frames[current_frame]) ||
        image->default_span_count == 0 ||
        image->default_span_count > kPngIdatSpanCapacity ||
        image->default_compressed_bytes < 6 ||
        (image->canvas.color_type == 3 &&
         image->canvas.palette_count == 0)) {
        return fail(result, ApngError::frame_data);
    }
    result.error = ApngError::none;
    return result;
}

ApngDecodeResult apng_decode_parsed_frame(
    ConstSlice<u8> encoded, const ApngImage& image, u32 frame_index,
    DeflateWorkspace* deflate_workspace, PngImage* frame_image,
    Slice<u8> scanlines) noexcept {
    ApngDecodeResult result{};
    result.frame_index = frame_index;
    if (!deflate_workspace || !frame_image || !scanlines.data ||
        frame_index >= image.frame_count || !image.frames ||
        !image.spans ||
        !configure_frame_image(encoded, image, image.frames[frame_index],
                               frame_image)) {
        result.error = ApngError::invalid_input;
        return result;
    }
    result.frame = image.frames[frame_index];
    result.duration_us = apng_frame_duration_us(result.frame);
    result.png = png_decode_parsed_scanlines(
        frame_image, deflate_workspace, scanlines);
    result.error = result.png.ok() ? ApngError::none
                                   : ApngError::inflate;
    return result;
}

PngDecodeResult apng_decode_parsed_default(
    ConstSlice<u8> encoded, const ApngImage& image,
    DeflateWorkspace* deflate_workspace, PngImage* default_image,
    Slice<u8> scanlines) noexcept {
    PngDecodeResult result{};
    if (!deflate_workspace || !default_image || !scanlines.data ||
        !configure_default_image(encoded, image, default_image)) {
        result.error = PngError::invalid_input;
        return result;
    }
    return png_decode_parsed_scanlines(default_image, deflate_workspace,
                                       scanlines);
}

} // namespace maicord

