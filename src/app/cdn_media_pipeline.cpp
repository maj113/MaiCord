#include "maicord/app/cdn_media_pipeline.hpp"
#include "maicord/core/byte_order.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/crt.hpp"

namespace maicord {
namespace {

static_assert(is_trivially_default_constructible_v<JpegWorkspace>);
static_assert(is_trivially_default_constructible_v<LossyWebpWorkspace>);

struct CacheLeaseScope {
    EncodedMediaCache* cache;
    EncodedMediaCacheLease lease;

    CacheLeaseScope(EncodedMediaCache* source, MediaAssetKey key, u64 now_seconds) noexcept
        : cache(source), lease(encoded_media_cache_acquire(source, key, now_seconds)) {
    }

    ~CacheLeaseScope() noexcept {
        release();
    }

    void release() noexcept {
        if (lease.acquired) {
            encoded_media_cache_release(cache, &lease);
        }
    }
};

ConstSlice<u8> copy_result_etag(CdnMediaPipeline* pipeline, ConstSlice<u8> etag) noexcept {
    pipeline->result_etag_bytes = 0;
    if (!etag.data || etag.count == 0 || etag.count > sizeof(pipeline->result_etag)) {
        return {};
    }
    CopyMemory(pipeline->result_etag, etag.data, etag.count);
    pipeline->result_etag_bytes = etag.count;
    return ConstSlice<u8>{pipeline->result_etag, pipeline->result_etag_bytes};
}

// The answer for a lookup with nothing to decode: a miss, a stale entry (its ETag and generation
// for the conditional request) or an unusable one.
template <typename Result>
Result unfresh_lookup(CdnMediaPipeline* pipeline, const EncodedMediaCacheLookup& cached) noexcept {
    using State = decltype(Result::state);
    Result result{};
    if (cached.state == EncodedMediaCacheLookupState::miss) {
        result.state = State::cache_miss;
    } else if (cached.state == EncodedMediaCacheLookupState::stale) {
        result.state = State::fetch_required;
        result.etag = copy_result_etag(pipeline, cached.etag);
        if constexpr (requires { result.cache_generation; }) {
            result.cache_generation = cached.generation;
        }
    } else {
        result.state = State::invalid;
    }
    return result;
}

bool static_image_key_valid(MediaAssetKey key, MediaAttachmentFormat format) noexcept {
    if (!media_asset_key_valid(key)) {
        return false;
    }
    bool matches = false;
    switch (key.kind) {
    case MediaAssetKind::catalog_preview:
    case MediaAssetKind::local_draft_preview:
        matches = media_asset_catalog_format(key) == format;
        break;
    case MediaAssetKind::sticker:
        matches = format == MediaAttachmentFormat::png && key.variant == 1;
        break;
    case MediaAssetKind::guild_icon:
    case MediaAssetKind::user_avatar:
    case MediaAssetKind::channel_icon:
        matches = format == MediaAttachmentFormat::webp && key.variant != 0;
        break;
    case MediaAssetKind::custom_emoji:
        matches = format == MediaAttachmentFormat::webp && key.variant == 0;
        break;
    case MediaAssetKind::attachment: {
        const MediaAttachmentFormat stored = media_asset_attachment_format(key);
        // Large JPEG renditions arrive as PNG from the media proxy.
        matches = stored == format ||
                  (format == MediaAttachmentFormat::jpeg && stored == MediaAttachmentFormat::unknown) ||
                  (format == MediaAttachmentFormat::png && stored == MediaAttachmentFormat::jpeg);
        break;
    }
    case MediaAssetKind::profile_image:
    case MediaAssetKind::embed: {
        const MediaAttachmentFormat stored = media_asset_embed_format(key);
        matches = stored == format ||
                  (format == MediaAttachmentFormat::png && stored == MediaAttachmentFormat::jpeg);
        break;
    }
    default:
        break;
    }
    return matches &&
           ((key.kind == MediaAssetKind::embed || key.kind == MediaAssetKind::profile_image ||
             key.kind == MediaAssetKind::catalog_preview || key.kind == MediaAssetKind::local_draft_preview)
                ? (key.flags & media_asset_flag_animated) == 0
                : key.flags == media_asset_flag_none);
}

bool animated_webp_key_valid(MediaAssetKey key) noexcept {
    return media_asset_key_valid(key) &&
           ((key.kind == MediaAssetKind::custom_emoji && key.variant == 0) ||
            (key.kind == MediaAssetKind::catalog_preview &&
             media_asset_catalog_format(key) == MediaAttachmentFormat::webp) ||
            (key.kind == MediaAssetKind::guild_icon && key.variant != 0) ||
            (key.kind == MediaAssetKind::user_avatar && key.variant != 0) ||
            (key.kind == MediaAssetKind::channel_icon && key.variant != 0) ||
            (key.kind == MediaAssetKind::attachment &&
             media_asset_attachment_format(key) == MediaAttachmentFormat::webp) ||
            ((key.kind == MediaAssetKind::embed || key.kind == MediaAssetKind::profile_image) &&
             media_asset_embed_format(key) == MediaAttachmentFormat::webp)) &&
           (key.flags & media_asset_flag_animated) != 0;
}

bool apng_key_valid(MediaAssetKey key) noexcept {
    return media_asset_key_valid(key) &&
           ((key.kind == MediaAssetKind::sticker && key.variant == 2) ||
            (key.kind == MediaAssetKind::catalog_preview &&
             media_asset_catalog_format(key) == MediaAttachmentFormat::png) ||
            (key.kind == MediaAssetKind::profile_image &&
             media_asset_embed_format(key) == MediaAttachmentFormat::png)) &&
           ((key.flags & media_asset_flag_animated) != 0 || key.kind == MediaAssetKind::catalog_preview);
}

bool gif_key_valid(MediaAssetKey key) noexcept {
    return media_asset_key_valid(key) &&
           ((key.kind == MediaAssetKind::local_draft_preview &&
             media_asset_catalog_format(key) == MediaAttachmentFormat::gif) ||
            ((key.kind == MediaAssetKind::sticker && key.variant == 4) ||
             (key.kind == MediaAssetKind::catalog_preview &&
              media_asset_catalog_format(key) == MediaAttachmentFormat::gif) ||
             (key.kind == MediaAssetKind::attachment &&
              media_asset_attachment_format(key) == MediaAttachmentFormat::gif) ||
             ((key.kind == MediaAssetKind::embed || key.kind == MediaAssetKind::profile_image) &&
              media_asset_embed_format(key) == MediaAttachmentFormat::gif)) &&
                (key.flags & media_asset_flag_animated) != 0);
}

CdnStaticPngResult decode_bytes(CdnMediaPipeline* pipeline, ConstSlice<u8> encoded,
                                CdnStaticPngState success_state) noexcept {
    CdnStaticPngResult result{};
    const auto parsed = png_parse(encoded, &result.image);
    // Interlaced files and files with many IDAT chunks need room past the output.
    const u32 required = parsed.ok() ? png_decode_buffer_bytes(result.image) : 0;
    if (pipeline->grow_scanlines && parsed.ok() && required > pipeline->scanline_capacity) {
        const auto grown = pipeline->grow_scanlines(pipeline->scanline_owner, required);
        if (grown.data && grown.count >= required) {
            pipeline->scanline_storage = grown.data;
            pipeline->scanline_capacity = grown.count;
        }
    }
    if (parsed.ok())
        result.decode = png_decode_parsed_scanlines(
            &result.image, &pipeline->workspace, {pipeline->scanline_storage, pipeline->scanline_capacity});
    else
        result.decode.error = parsed.error;
    result.decode.parse = parsed;
    if (!result.decode.ok()) {
        result.state = CdnStaticPngState::decode_rejected;
        return result;
    }
    result.scanlines = ConstSlice<u8>{pipeline->scanline_storage, result.image.inflated_bytes};
    result.state = success_state;
    return result;
}

CdnStaticWebpResult decode_webp_bytes(CdnMediaPipeline* pipeline, ConstSlice<u8> encoded,
                                      CdnStaticWebpState success_state) noexcept {
    CdnStaticWebpResult result{};
    if ((reinterpret_cast<usize>(pipeline->scanline_storage) & 3u) != 0) {
        return {.state = CdnStaticWebpState::invalid};
    }
    WebpImage parsed_image{};
    const WebpParseResult parsed = webp_parse(encoded, &parsed_image);
    if (!parsed.ok()) {
        result.decode.parse = parsed;
        result.decode.error = parsed.error;
        result.state = CdnStaticWebpState::decode_rejected;
        return result;
    }
    if (parsed_image.codec == WebpCodec::vp8l_lossless) {
        if (!pipeline->webp_workspace) {
            return {.state = CdnStaticWebpState::invalid};
        }
        result.decode =
            webp_lossless_decode_rgba8(encoded, &result.image, pipeline->webp_workspace,
                                       Slice<u8>{pipeline->scanline_storage, pipeline->scanline_capacity});
        if (!result.decode.ok()) {
            result.state = CdnStaticWebpState::decode_rejected;
            return result;
        }
        result.rgba8 = ConstSlice<u8>{pipeline->scanline_storage, result.decode.rgba_bytes};
    } else if (parsed_image.codec == WebpCodec::vp8_lossy) {
        if (!pipeline->lossy_webp_workspace) {
            return {.state = CdnStaticWebpState::lossy_workspace_required};
        }
        const u32 pixels = parsed_image.width * parsed_image.height;
        const u32 scratch_bytes = pixels * 4u;
        const u32 required = parsed_image.has_alpha ? scratch_bytes + pixels : 0u;
        if (required > pipeline->scanline_capacity) {
            return {.state = CdnStaticWebpState::invalid};
        }
        pipeline->lossy_webp_workspace = ::new (pipeline->lossy_webp_workspace) LossyWebpWorkspace;
        LossyWebpWorkspace* workspace = pipeline->lossy_webp_workspace;
        result.lossy_decode = lossy_webp_decode_keyframe(
            encoded, &result.image, &result.vp8_frame, &result.vp8_header, &workspace->probabilities,
            &workspace->modes, &workspace->coefficients,
            parsed_image.has_alpha ? pipeline->webp_workspace : nullptr,
            parsed_image.has_alpha ? Slice<u8>{pipeline->scanline_storage, scratch_bytes} : Slice<u8>{},
            parsed_image.has_alpha ? Slice<u8>{pipeline->scanline_storage + scratch_bytes, pixels}
                                   : Slice<u8>{});
        if (!result.lossy_decode.ok()) {
            result.decode.parse = result.lossy_decode.parse;
            result.decode.error = result.lossy_decode.parse.error != WebpError::none
                                      ? result.lossy_decode.parse.error
                                      : WebpError::bitstream;
            result.state = CdnStaticWebpState::decode_rejected;
            return result;
        }
        if (parsed_image.has_alpha) {
            result.alpha =
                ConstSlice<u8>{pipeline->scanline_storage + scratch_bytes, result.lossy_decode.alpha_bytes};
        }
    } else {
        result.decode.parse = parsed;
        result.decode.error = WebpError::unsupported_feature;
        result.state = CdnStaticWebpState::decode_rejected;
        return result;
    }
    result.state = success_state;
    return result;
}

CdnStaticJpegResult decode_jpeg_bytes(CdnMediaPipeline* pipeline, ConstSlice<u8> encoded,
                                      CdnStaticJpegState success_state) noexcept {
    CdnStaticJpegResult result{};
    if (!pipeline->jpeg_workspace) {
        return {.state = CdnStaticJpegState::jpeg_workspace_required};
    }
    pipeline->jpeg_workspace = ::new (pipeline->jpeg_workspace) JpegWorkspace;
    result.decode = jpeg_decode_coefficients(encoded, &result.image, pipeline->jpeg_workspace);
    if (!result.decode.ok()) {
        result.state = CdnStaticJpegState::decode_rejected;
        return result;
    }
    result.coefficients = &pipeline->jpeg_workspace->coefficients;
    result.state = success_state;
    return result;
}

CdnAnimatedWebpResult decode_animated_webp_bytes(CdnMediaPipeline* pipeline, ConstSlice<u8> encoded,
                                                 u32 frame_index, u16 cache_generation,
                                                 CdnAnimatedWebpState success_state, u64 content_identity) noexcept {
    CdnAnimatedWebpResult result{};
    if (!pipeline->webp_workspace || !pipeline->animated_webp_workspace ||
        (reinterpret_cast<usize>(pipeline->scanline_storage) & 3u) != 0) {
        return {.state = CdnAnimatedWebpState::invalid};
    }
    AnimatedWebpImage animation{};
    auto& workspace = *pipeline->animated_webp_workspace;
    auto& saved = workspace.retained;
    const AnimatedWebpFrame* frames = workspace.extended_frames.data ? workspace.extended_frames.data : workspace.frames;
    const u32 capacity = workspace.extended_frames.data ? workspace.extended_frames.count : kAnimatedWebpInlineFrameCapacity;
    const bool retained = content_identity != 0 && saved.content_identity == content_identity &&
                          saved.frames == frames && saved.frame_count <= capacity;
    if (retained) {
        animation = {saved.frames, saved.background_rgba, saved.frame_count, saved.play_count,
                     saved.width, saved.height, saved.has_alpha, saved.has_color_profile};
        result.decode.parse = {saved.chunks, saved.frame_chunks, AnimatedWebpError::none};
    } else {
        result.decode.parse = animated_webp_parse(encoded, &animation, &workspace);
    }
    if (!result.decode.parse.ok() || frame_index >= animation.frame_count) {
        saved.content_identity = 0;
        result.decode.error =
            result.decode.parse.ok() ? AnimatedWebpError::frame_count : result.decode.parse.error;
        result.state = CdnAnimatedWebpState::decode_rejected;
        return result;
    }
    const AnimatedWebpParseResult animation_parse = result.decode.parse;
    bool lossy_present = retained && saved.lossy_present;
    if (!retained) {
        for (u32 index = 0; index < animation.frame_count; ++index)
            lossy_present = lossy_present || animation.frames[index].codec == AnimatedWebpCodec::vp8_lossy;
    }
    if (lossy_present && !pipeline->lossy_webp_workspace) {
        result.state = CdnAnimatedWebpState::lossy_workspace_required;
        return result;
    }
    // All container ranges are validated up front; only the requested frame is decoded. Later
    // corrupt frames are rejected and evicted by the on-demand cache path.
    const AnimatedWebpFrame& requested = animation.frames[frame_index];
    const u32 pixels = static_cast<u32>(requested.width) * requested.height;
    const u32 scratch_bytes = pixels * 4u;
    const bool external_alpha = requested.codec == AnimatedWebpCodec::vp8_lossy && requested.has_alpha;
    const u32 required_bytes = requested.codec == AnimatedWebpCodec::vp8l_lossless ? scratch_bytes
                               : external_alpha                                    ? scratch_bytes + pixels
                                                                                   : 0u;
    if (required_bytes > pipeline->scanline_capacity) {
        saved.content_identity = 0;
        result.decode.error = AnimatedWebpError::output_capacity;
        result.state = CdnAnimatedWebpState::decode_rejected;
        return result;
    }
    if (requested.codec == AnimatedWebpCodec::vp8_lossy) {
        pipeline->lossy_webp_workspace = ::new (pipeline->lossy_webp_workspace) LossyWebpWorkspace;
    }
    result.decode = animated_webp_decode_parsed_frame(
        encoded, animation, frame_index, pipeline->webp_workspace, pipeline->lossy_webp_workspace,
        &result.vp8_frame, &result.vp8_header,
        Slice<u8>{pipeline->scanline_storage, pipeline->scanline_capacity},
        external_alpha ? Slice<u8>{pipeline->scanline_storage, scratch_bytes} : Slice<u8>{},
        external_alpha ? Slice<u8>{pipeline->scanline_storage + scratch_bytes,
                                   pipeline->scanline_capacity - scratch_bytes}
                       : Slice<u8>{});
    result.decode.parse = animation_parse;
    if (!result.decode.ok()) {
        saved.content_identity = 0;
        result.state = CdnAnimatedWebpState::decode_rejected;
        return result;
    }
    if (!retained && content_identity != 0) {
        saved = {content_identity, animation.frames, animation.background_rgba, animation.frame_count,
                 animation.play_count, animation_parse.chunks, animation_parse.frame_chunks,
                 animation.width, animation.height, animation.has_alpha, animation.has_color_profile, lossy_present};
    }
    if (requested.codec == AnimatedWebpCodec::vp8l_lossless) {
        result.rgba8 = ConstSlice<u8>{pipeline->scanline_storage, result.decode.frame_decode.rgba_bytes};
    } else if (requested.has_alpha) {
        result.alpha = ConstSlice<u8>{pipeline->scanline_storage + scratch_bytes,
                                      result.decode.lossy_decode.alpha_bytes};
    }
    result.background_rgba = animation.background_rgba;
    result.frame_count = animation.frame_count;
    result.play_count = animation.play_count;
    result.canvas_width = animation.width;
    result.canvas_height = animation.height;
    result.cache_generation = cache_generation;
    result.state = success_state;
    return result;
}

struct RetainedApngMetadata {
    u32 marker;
    ApngParseResult parse;
    u32 play_count;
    u32 default_compressed_bytes;
    u32 palette_offset;
    u32 transparency_offset;
    u16 frame_count;
    u16 span_count;
    u16 default_first_span;
    u16 default_span_count;
    u16 palette_bytes;
    u16 transparency_bytes;
    u64 content_identity;
    bool default_in_animation;
    bool has_color_metadata;
};
static_assert(sizeof(RetainedApngMetadata) == kApngRetainedMetadataBytes);

bool restore_apng_metadata(CdnMediaPipeline* pipeline, ConstSlice<u8> encoded,
                           u64 content_identity, ApngImage* image, ApngParseResult* parse) noexcept {
    if (!pipeline->apng_metadata_valid || content_identity == 0 ||
        pipeline->apng_workspace->spans[kApngRetainedSpanCapacity].offset != kApngRetainedMarker) {
        return false;
    }
    RetainedApngMetadata saved{};
    CopyMemory(&saved, &pipeline->apng_workspace->spans[kApngRetainedSpanCapacity], sizeof(saved));
    if (saved.content_identity != content_identity) {
        return false;
    }
    // The identity pins the same validated bytes. No encoded pointers are retained.
    *image = ApngImage{};
    image->frames = pipeline->apng_workspace->frames;
    image->spans = pipeline->apng_workspace->spans;
    image->frame_capacity = kApngFrameCapacity;
    image->span_capacity = kApngDataSpanCapacity;
    image->frame_count = saved.frame_count;
    image->span_count = saved.span_count;
    image->play_count = saved.play_count;
    image->default_first_span = saved.default_first_span;
    image->default_compressed_bytes = saved.default_compressed_bytes;
    image->default_span_count = saved.default_span_count;
    image->default_in_animation = saved.default_in_animation;
    auto& png = image->canvas;
    png.transparent_gray = png.transparent_red = png.transparent_green = png.transparent_blue = ~0u;
    if (!png_image_configure(&png, byte_load_be32(encoded.data + 16), byte_load_be32(encoded.data + 20),
                             encoded[24], encoded[25])) {
        return false;
    }
    png.palette_count = static_cast<u16>(saved.palette_bytes / 3);
    for (u32 index = 0; index < png.palette_count; ++index) {
        const u8* color = encoded.data + saved.palette_offset + index * 3;
        png.palette_rgba[index] = u32{color[0]} | u32{color[1]} << 8 | u32{color[2]} << 16 | 0xff000000u;
    }
    if (saved.transparency_bytes != 0) {
        const u8* transparency = encoded.data + saved.transparency_offset;
        png.has_transparency = true;
        if (png.color_type == 0) {
            png.transparent_gray = byte_load_be16(transparency);
        } else if (png.color_type == 2) {
            png.transparent_red = byte_load_be16(transparency);
            png.transparent_green = byte_load_be16(transparency + 2);
            png.transparent_blue = byte_load_be16(transparency + 4);
        } else {
            for (u32 index = 0; index < saved.transparency_bytes; ++index) {
                png.palette_rgba[index] = (png.palette_rgba[index] & 0xffffffu) | u32{transparency[index]} << 24;
            }
        }
    }
    png.has_color_metadata = saved.has_color_metadata;
    *parse = saved.parse;
    return true;
}

void retain_apng_metadata(CdnMediaPipeline* pipeline, ConstSlice<u8> encoded,
                          const ApngImage& image, ApngParseResult parse, u64 content_identity) noexcept {
    if (content_identity == 0 || image.span_count > kApngRetainedSpanCapacity) {
        return;
    }
    RetainedApngMetadata saved{};
    saved.parse = parse;
    saved.play_count = image.play_count;
    saved.default_compressed_bytes = image.default_compressed_bytes;
    saved.frame_count = static_cast<u16>(image.frame_count);
    saved.span_count = static_cast<u16>(image.span_count);
    saved.default_first_span = static_cast<u16>(image.default_first_span);
    saved.default_span_count = image.default_span_count;
    saved.content_identity = content_identity;
    saved.default_in_animation = image.default_in_animation;
    saved.has_color_metadata = image.canvas.has_color_metadata;
    // Only a miss scans these validated chunk headers. PLTE and tRNS precede IDAT.
    for (u32 cursor = 8; cursor + 12 <= encoded.count;) {
        const u32 bytes = byte_load_be32(encoded.data + cursor);
        const u32 type = byte_load_be32(encoded.data + cursor + 4);
        if (type == 0x504c5445u) {
            saved.palette_offset = cursor + 8;
            saved.palette_bytes = static_cast<u16>(bytes);
        } else if (type == 0x74524e53u) {
            saved.transparency_offset = cursor + 8;
            saved.transparency_bytes = static_cast<u16>(bytes);
        } else if (type == 0x49444154u) {
            break;
        }
        cursor += bytes + 12;
    }
    // Copy object bytes, then publish the marker. These slots hold no active spans.
    CopyMemory(&pipeline->apng_workspace->spans[kApngRetainedSpanCapacity], &saved, sizeof(saved));
    pipeline->apng_workspace->spans[kApngRetainedSpanCapacity].offset = kApngRetainedMarker;
    pipeline->apng_metadata_valid = true;
}

CdnApngResult decode_apng_bytes(CdnMediaPipeline* pipeline, ConstSlice<u8> encoded, u32 frame_index,
                                u16 cache_generation, CdnApngState success_state,
                                bool validate_default, u64 content_identity) noexcept {
    CdnApngResult result{};
    if (!pipeline->apng_workspace) {
        return {.state = CdnApngState::invalid};
    }
    ApngImage animation{};
    const bool retained = restore_apng_metadata(pipeline, encoded, content_identity, &animation, &result.decode.parse);
    if (!retained) {
        pipeline->apng_metadata_valid = false;
        result.decode.parse = apng_parse(encoded, &animation, pipeline->apng_workspace);
    }
    if (!result.decode.parse.ok()) {
        result.decode.error = result.decode.parse.error;
        result.state = CdnApngState::decode_rejected;
        return result;
    }
    if (validate_default && !animation.default_in_animation) {
        PngImage default_image{};
        const PngDecodeResult decoded_default =
            apng_decode_parsed_default(encoded, animation, &pipeline->workspace, &default_image,
                                       Slice<u8>{pipeline->scanline_storage, pipeline->scanline_capacity});
        if (!decoded_default.ok()) {
            pipeline->apng_metadata_valid = false;
            result.decode.png = decoded_default;
            result.decode.error = ApngError::inflate;
            result.state = CdnApngState::decode_rejected;
            return result;
        }
    }
    const ApngParseResult parse = result.decode.parse;
    result.decode =
        apng_decode_parsed_frame(encoded, animation, frame_index, &pipeline->workspace, &result.image,
                                 Slice<u8>{pipeline->scanline_storage, pipeline->scanline_capacity});
    result.decode.parse = parse;
    if (!result.decode.ok()) {
        pipeline->apng_metadata_valid = false;
        result.state = CdnApngState::decode_rejected;
        return result;
    }
    if (!retained) {
        retain_apng_metadata(pipeline, encoded, animation, parse, content_identity);
    }
    result.scanlines = ConstSlice<u8>{pipeline->scanline_storage, result.image.inflated_bytes};
    result.frame_count = animation.frame_count;
    result.play_count = animation.play_count;
    result.canvas_width = animation.canvas.width;
    result.canvas_height = animation.canvas.height;
    result.cache_generation = cache_generation;
    result.state = success_state;
    return result;
}

CdnGifResult decode_gif_bytes(CdnMediaPipeline* pipeline, ConstSlice<u8> encoded, u32 frame_index,
                              u16 cache_generation, CdnGifState success_state, u64 content_identity) noexcept {
    CdnGifResult result{};
    if (!pipeline->gif_workspace) {
        return {.state = CdnGifState::invalid};
    }
    const bool retained = content_identity != 0 && content_identity == pipeline->gif_content_identity &&
                          pipeline->gif_image.frames == pipeline->gif_workspace->frames;
    GifImage animation{};
    if (retained) {
        animation = pipeline->gif_image;
        result.decode.parse = pipeline->gif_parse_result;
    } else {
        pipeline->gif_content_identity = 0;
        result.decode.parse = gif_parse(encoded, &animation, pipeline->gif_workspace);
    }
    if (!result.decode.parse.ok()) {
        result.decode.error = result.decode.parse.error;
        result.state = CdnGifState::decode_rejected;
        return result;
    }
    if (frame_index >= animation.frame_count) {
        pipeline->gif_content_identity = 0;
        result.decode.error = GifError::invalid_input;
        result.state = CdnGifState::decode_rejected;
        return result;
    }
    // The container validates all frame ranges above; pixel data is decoded per requested frame.
    const GifParseResult parse = result.decode.parse;
    result.decode =
        gif_decode_parsed_frame(encoded, animation, frame_index, pipeline->gif_workspace,
                                Slice<u8>{pipeline->scanline_storage, pipeline->scanline_capacity});
    result.decode.parse = parse;
    if (!result.decode.ok()) {
        pipeline->gif_content_identity = 0;
        result.state = CdnGifState::decode_rejected;
        return result;
    }
    if (!retained && content_identity != 0) {
        pipeline->gif_image = animation;
        pipeline->gif_parse_result = parse;
        pipeline->gif_content_identity = content_identity;
    }
    result.indices = ConstSlice<u8>{pipeline->scanline_storage, result.decode.pixels};
    result.frame_count = animation.frame_count;
    result.play_count = animation.play_count;
    result.canvas_width = animation.canvas_width;
    result.canvas_height = animation.canvas_height;
    result.cache_generation = cache_generation;
    result.state = success_state;
    return result;
}

// The cached entry carries the ETag a 304 answers.
__declspec(noinline) bool etag_correlated(const EncodedMediaCacheLookup& cached,
                                          ConstSlice<u8> conditional_etag) noexcept {
    if (!cached.found() || cached.etag.count == 0 || cached.etag.count != conditional_etag.count) {
        return false;
    }
    for (u32 index = 0; index < cached.etag.count; ++index) {
        if (cached.etag[index] != conditional_etag[index]) {
            return false;
        }
    }
    return true;
}

__declspec(noinline) bool accepted_body_valid(const CdnResponseResult& response,
                                              ConstSlice<u8> body) noexcept {
    return response.status_code == 200 && body.data && body.count != 0 &&
           body.count <= kCdnMaximumEncodedBodyBytes && body.count == response.body_bytes;
}

__declspec(noinline) bool not_modified_valid(const CdnResponseResult& response, ConstSlice<u8> body,
                                             ConstSlice<u8> conditional_etag) noexcept {
    return response.status_code == 304 && body.count == 0 && conditional_etag.data &&
           conditional_etag.count != 0;
}

__declspec(noinline) EncodedMediaCacheStoreResult
store_decoded_response(CdnMediaPipeline* pipeline, MediaAssetKey key, ConstSlice<u8> body,
                       const CdnResponseResult& response) noexcept {
    return encoded_media_cache_store(
        pipeline->cache, key, body,
        EncodedMediaCachePolicy{response.etag, response.expires_at_seconds, response.store_allowed,
                                response.must_revalidate});
}

__declspec(noinline) void finish_not_modified(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                              CacheLeaseScope* cache_lease,
                                              const CdnResponseResult& response) noexcept {
    cache_lease->release();
    if (response.store_allowed) {
        encoded_media_cache_refresh(pipeline->cache, key, response.expires_at_seconds,
                                    response.must_revalidate, response.etag);
    } else {
        encoded_media_cache_erase(pipeline->cache, key);
    }
}

} // namespace

bool cdn_media_pipeline_initialize(CdnMediaPipeline* pipeline, EncodedMediaCache* cache,
                                   Slice<u8> scanline_storage) noexcept {
    if (!pipeline || !cache || !cache->entries || !cache->storage || !scanline_storage.data ||
        scanline_storage.count == 0) {
        return false;
    }
    *pipeline = CdnMediaPipeline{};
    pipeline->cache = cache;
    pipeline->scanline_storage = scanline_storage.data;
    pipeline->scanline_capacity = scanline_storage.count;
    return true;
}

bool cdn_media_pipeline_bind_webp_workspace(CdnMediaPipeline* pipeline, Vp8lWorkspace* workspace) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage || !workspace ||
        (reinterpret_cast<usize>(pipeline->scanline_storage) & 3u) != 0) {
        return false;
    }
    pipeline->webp_workspace = workspace;
    return true;
}

bool cdn_media_pipeline_bind_animated_webp_workspace(CdnMediaPipeline* pipeline,
                                                     AnimatedWebpWorkspace* workspace) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage || !workspace) {
        return false;
    }
    workspace->retained.content_identity = 0;
    pipeline->animated_webp_workspace = workspace;
    return true;
}

bool cdn_media_pipeline_bind_apng_workspace(CdnMediaPipeline* pipeline, ApngWorkspace* workspace) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage || !workspace) {
        return false;
    }
    pipeline->apng_workspace = workspace;
    pipeline->apng_metadata_valid = false;
    return true;
}

bool cdn_media_pipeline_bind_gif_workspace(CdnMediaPipeline* pipeline, GifWorkspace* workspace) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage || !workspace) {
        return false;
    }
    pipeline->gif_workspace = workspace;
    pipeline->gif_content_identity = 0;
    return true;
}

CdnStaticPngResult cdn_media_pipeline_lookup_static_png(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                        u64 now_seconds) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage ||
        !static_image_key_valid(key, MediaAttachmentFormat::png)) {
        return {.state = CdnStaticPngState::invalid};
    }
    CacheLeaseScope cache_lease(pipeline->cache, key, now_seconds);
    const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
    if (cached.state != EncodedMediaCacheLookupState::fresh) {
        return unfresh_lookup<CdnStaticPngResult>(pipeline, cached);
    }
    CdnStaticPngResult result = decode_bytes(pipeline, cached.bytes, CdnStaticPngState::decoded_cache_hit);
    if (!result.decoded()) {
        cache_lease.release();
        encoded_media_cache_erase(pipeline->cache, key);
        return result;
    }
    result.etag = copy_result_etag(pipeline, cached.etag);
    return result;
}

CdnStaticPngResult cdn_media_pipeline_admit_static_png(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                       const CdnResponseResult& response,
                                                       ConstSlice<u8> decoded_body,
                                                       ConstSlice<u8> conditional_etag) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage ||
        !static_image_key_valid(key, MediaAttachmentFormat::png) || !response.ok()) {
        return {.state = response.ok() ? CdnStaticPngState::invalid : CdnStaticPngState::http_rejected};
    }
    if (response.disposition == CdnResponseDisposition::accept_body) {
        if (!accepted_body_valid(response, decoded_body)) {
            return {.state = CdnStaticPngState::invalid};
        }
        CdnStaticPngResult result =
            decode_bytes(pipeline, decoded_body,
                         response.store_allowed ? CdnStaticPngState::decoded_response
                                                : CdnStaticPngState::decoded_transient);
        if (!result.decoded()) {
            return result;
        }
        result.store = store_decoded_response(pipeline, key, decoded_body, response);
        if (!result.store.stored()) {
            // Cache pressure never invalidates already decoded scanlines.
            result.state = CdnStaticPngState::decoded_transient;
        }
        return result;
    }
    if (response.disposition == CdnResponseDisposition::not_modified) {
        if (!not_modified_valid(response, decoded_body, conditional_etag)) {
            return {.state = CdnStaticPngState::invalid};
        }
        CacheLeaseScope cache_lease(pipeline->cache, key, response.expires_at_seconds);
        const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
        if (!etag_correlated(cached, conditional_etag)) {
            return {.state = CdnStaticPngState::uncorrelated_not_modified};
        }
        CdnStaticPngResult result =
            decode_bytes(pipeline, cached.bytes,
                         response.store_allowed ? CdnStaticPngState::decoded_cache_hit
                                                : CdnStaticPngState::decoded_transient);
        if (!result.decoded()) {
            cache_lease.release();
            encoded_media_cache_erase(pipeline->cache, key);
            return result;
        }
        finish_not_modified(pipeline, key, &cache_lease, response);
        return result;
    }
    return {.state = CdnStaticPngState::http_rejected};
}

CdnStaticWebpResult cdn_media_pipeline_lookup_static_webp(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                          u64 now_seconds) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage || !pipeline->webp_workspace ||
        !static_image_key_valid(key, MediaAttachmentFormat::webp)) {
        return {.state = CdnStaticWebpState::invalid};
    }
    CacheLeaseScope cache_lease(pipeline->cache, key, now_seconds);
    const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
    if (cached.state != EncodedMediaCacheLookupState::fresh) {
        return unfresh_lookup<CdnStaticWebpResult>(pipeline, cached);
    }
    CdnStaticWebpResult result =
        decode_webp_bytes(pipeline, cached.bytes, CdnStaticWebpState::decoded_cache_hit);
    if (!result.decoded() && result.state != CdnStaticWebpState::lossy_workspace_required) {
        cache_lease.release();
        encoded_media_cache_erase(pipeline->cache, key);
        return result;
    }
    result.etag = copy_result_etag(pipeline, cached.etag);
    return result;
}

CdnStaticWebpResult cdn_media_pipeline_admit_static_webp(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                         const CdnResponseResult& response,
                                                         ConstSlice<u8> decoded_body,
                                                         ConstSlice<u8> conditional_etag) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage || !pipeline->webp_workspace ||
        !static_image_key_valid(key, MediaAttachmentFormat::webp) || !response.ok()) {
        return {.state = response.ok() ? CdnStaticWebpState::invalid : CdnStaticWebpState::http_rejected};
    }
    if (response.disposition == CdnResponseDisposition::accept_body) {
        if (!accepted_body_valid(response, decoded_body)) {
            return {.state = CdnStaticWebpState::invalid};
        }
        CdnStaticWebpResult result =
            decode_webp_bytes(pipeline, decoded_body,
                              response.store_allowed ? CdnStaticWebpState::decoded_response
                                                     : CdnStaticWebpState::decoded_transient);
        if (!result.decoded()) {
            return result;
        }
        result.store = store_decoded_response(pipeline, key, decoded_body, response);
        if (!result.store.stored()) {
            result.state = CdnStaticWebpState::decoded_transient;
        }
        return result;
    }
    if (response.disposition == CdnResponseDisposition::not_modified) {
        if (!not_modified_valid(response, decoded_body, conditional_etag)) {
            return {.state = CdnStaticWebpState::invalid};
        }
        CacheLeaseScope cache_lease(pipeline->cache, key, response.expires_at_seconds);
        const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
        if (!etag_correlated(cached, conditional_etag)) {
            return {.state = CdnStaticWebpState::uncorrelated_not_modified};
        }
        CdnStaticWebpResult result =
            decode_webp_bytes(pipeline, cached.bytes,
                              response.store_allowed ? CdnStaticWebpState::decoded_cache_hit
                                                     : CdnStaticWebpState::decoded_transient);
        if (!result.decoded()) {
            cache_lease.release();
            encoded_media_cache_erase(pipeline->cache, key);
            return result;
        }
        finish_not_modified(pipeline, key, &cache_lease, response);
        return result;
    }
    return {.state = CdnStaticWebpState::http_rejected};
}

CdnStaticJpegResult cdn_media_pipeline_lookup_static_jpeg(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                          u64 now_seconds) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage ||
        !static_image_key_valid(key, MediaAttachmentFormat::jpeg)) {
        return {.state = CdnStaticJpegState::invalid};
    }
    CacheLeaseScope cache_lease(pipeline->cache, key, now_seconds);
    const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
    if (cached.state != EncodedMediaCacheLookupState::fresh) {
        return unfresh_lookup<CdnStaticJpegResult>(pipeline, cached);
    }
    CdnStaticJpegResult result =
        decode_jpeg_bytes(pipeline, cached.bytes, CdnStaticJpegState::decoded_cache_hit);
    if (!result.decoded() && result.state != CdnStaticJpegState::jpeg_workspace_required) {
        cache_lease.release();
        encoded_media_cache_erase(pipeline->cache, key);
        return result;
    }
    result.etag = copy_result_etag(pipeline, cached.etag);
    return result;
}

CdnStaticJpegResult cdn_media_pipeline_admit_static_jpeg(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                         const CdnResponseResult& response,
                                                         ConstSlice<u8> decoded_body,
                                                         ConstSlice<u8> conditional_etag) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage ||
        !static_image_key_valid(key, MediaAttachmentFormat::jpeg) || !response.ok()) {
        return {.state = response.ok() ? CdnStaticJpegState::invalid : CdnStaticJpegState::http_rejected};
    }
    if (response.disposition == CdnResponseDisposition::accept_body) {
        if (!accepted_body_valid(response, decoded_body)) {
            return {.state = CdnStaticJpegState::invalid};
        }
        CdnStaticJpegResult result =
            decode_jpeg_bytes(pipeline, decoded_body,
                              response.store_allowed ? CdnStaticJpegState::decoded_response
                                                     : CdnStaticJpegState::decoded_transient);
        if (!result.decoded()) {
            return result;
        }
        result.store = store_decoded_response(pipeline, key, decoded_body, response);
        if (!result.store.stored()) {
            result.state = CdnStaticJpegState::decoded_transient;
        }
        return result;
    }
    if (response.disposition == CdnResponseDisposition::not_modified) {
        if (!not_modified_valid(response, decoded_body, conditional_etag)) {
            return {.state = CdnStaticJpegState::invalid};
        }
        CacheLeaseScope cache_lease(pipeline->cache, key, response.expires_at_seconds);
        const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
        if (!etag_correlated(cached, conditional_etag)) {
            return {.state = CdnStaticJpegState::uncorrelated_not_modified};
        }
        CdnStaticJpegResult result =
            decode_jpeg_bytes(pipeline, cached.bytes,
                              response.store_allowed ? CdnStaticJpegState::decoded_cache_hit
                                                     : CdnStaticJpegState::decoded_transient);
        if (!result.decoded()) {
            if (result.state != CdnStaticJpegState::jpeg_workspace_required) {
                cache_lease.release();
                encoded_media_cache_erase(pipeline->cache, key);
            }
            return result;
        }
        finish_not_modified(pipeline, key, &cache_lease, response);
        return result;
    }
    return {.state = CdnStaticJpegState::http_rejected};
}

CdnApngResult cdn_media_pipeline_lookup_apng(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                             u64 now_seconds) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage || !pipeline->apng_workspace ||
        !apng_key_valid(key)) {
        return {.state = CdnApngState::invalid};
    }
    CacheLeaseScope cache_lease(pipeline->cache, key, now_seconds);
    const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
    if (cached.state != EncodedMediaCacheLookupState::fresh) {
        return unfresh_lookup<CdnApngResult>(pipeline, cached);
    }
    CdnApngResult result = decode_apng_bytes(pipeline, cached.bytes, 0, cached.generation,
                                             CdnApngState::decoded_cache_hit, false, cached.content_identity);
    if (!result.decoded()) {
        cache_lease.release();
        encoded_media_cache_erase(pipeline->cache, key);
        return result;
    }
    result.etag = copy_result_etag(pipeline, cached.etag);
    return result;
}

CdnApngResult cdn_media_pipeline_admit_apng(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                            const CdnResponseResult& response,
                                            ConstSlice<u8> decoded_body,
                                            ConstSlice<u8> conditional_etag) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage || !pipeline->apng_workspace ||
        !apng_key_valid(key) || !response.ok()) {
        return {.state = response.ok() ? CdnApngState::invalid : CdnApngState::http_rejected};
    }
    if (response.disposition == CdnResponseDisposition::accept_body) {
        if (!accepted_body_valid(response, decoded_body)) {
            return {.state = CdnApngState::invalid};
        }
        CdnApngResult result = decode_apng_bytes(pipeline, decoded_body, 0, 0,
                                                 response.store_allowed ? CdnApngState::decoded_response
                                                                        : CdnApngState::decoded_transient,
                                                 true, 0);
        if (!result.decoded()) {
            return result;
        }
        result.store = store_decoded_response(pipeline, key, decoded_body, response);
        if (!result.store.stored()) {
            result.state = CdnApngState::decoded_transient;
            result.cache_generation = 0;
        } else {
            result.cache_generation = result.store.generation;
        }
        return result;
    }
    if (response.disposition == CdnResponseDisposition::not_modified) {
        if (!not_modified_valid(response, decoded_body, conditional_etag)) {
            return {.state = CdnApngState::invalid};
        }
        CacheLeaseScope cache_lease(pipeline->cache, key, response.expires_at_seconds);
        const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
        if (!etag_correlated(cached, conditional_etag)) {
            return {.state = CdnApngState::uncorrelated_not_modified};
        }
        CdnApngResult result = decode_apng_bytes(pipeline, cached.bytes, 0, cached.generation,
                                                 response.store_allowed ? CdnApngState::decoded_cache_hit
                                                                        : CdnApngState::decoded_transient,
                                                 false, cached.content_identity);
        if (!result.decoded()) {
            cache_lease.release();
            encoded_media_cache_erase(pipeline->cache, key);
            return result;
        }
        if (!response.store_allowed) {
            result.cache_generation = 0;
            pipeline->apng_metadata_valid = false;
        }
        finish_not_modified(pipeline, key, &cache_lease, response);
        return result;
    }
    return {.state = CdnApngState::http_rejected};
}

CdnApngResult cdn_media_pipeline_decode_apng_frame(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                   u16 cache_generation, u32 frame_index,
                                                   u64 now_seconds) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage || !pipeline->apng_workspace ||
        !apng_key_valid(key) || cache_generation == 0) {
        return {.state = CdnApngState::invalid};
    }
    CacheLeaseScope cache_lease(pipeline->cache, key, now_seconds);
    const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
    if (!cached.found()) {
        return {.state = CdnApngState::cache_miss};
    }
    if (cached.generation != cache_generation) {
        return {.state = CdnApngState::generation_mismatch};
    }
    CdnApngResult result = decode_apng_bytes(pipeline, cached.bytes, frame_index, cached.generation,
                                             CdnApngState::decoded_cache_hit, false, cached.content_identity);
    if (!result.decoded()) {
        cache_lease.release();
        encoded_media_cache_erase(pipeline->cache, key);
    }
    return result;
}

CdnGifResult cdn_media_pipeline_lookup_gif(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                           u64 now_seconds) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage || !pipeline->gif_workspace ||
        !gif_key_valid(key)) {
        return {.state = CdnGifState::invalid};
    }
    CacheLeaseScope cache_lease(pipeline->cache, key, now_seconds);
    const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
    if (cached.state != EncodedMediaCacheLookupState::fresh) {
        return unfresh_lookup<CdnGifResult>(pipeline, cached);
    }
    CdnGifResult result =
        decode_gif_bytes(pipeline, cached.bytes, 0, cached.generation, CdnGifState::decoded_cache_hit,
                         cached.content_identity);
    if (!result.decoded()) {
        cache_lease.release();
        encoded_media_cache_erase(pipeline->cache, key);
        return result;
    }
    result.etag = copy_result_etag(pipeline, cached.etag);
    return result;
}

CdnGifResult cdn_media_pipeline_admit_gif(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                          const CdnResponseResult& response,
                                          ConstSlice<u8> decoded_body,
                                          ConstSlice<u8> conditional_etag) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage || !pipeline->gif_workspace ||
        !gif_key_valid(key) || !response.ok()) {
        return {.state = response.ok() ? CdnGifState::invalid : CdnGifState::http_rejected};
    }
    if (response.disposition == CdnResponseDisposition::accept_body) {
        if (!accepted_body_valid(response, decoded_body)) {
            return {.state = CdnGifState::invalid};
        }
        CdnGifResult result = decode_gif_bytes(pipeline, decoded_body, 0, 0,
                                               response.store_allowed ? CdnGifState::decoded_response
                                                                      : CdnGifState::decoded_transient, 0);
        if (!result.decoded()) {
            return result;
        }
        result.store = store_decoded_response(pipeline, key, decoded_body, response);
        if (!result.store.stored()) {
            result.state = CdnGifState::decoded_transient;
            result.cache_generation = 0;
        } else {
            result.cache_generation = result.store.generation;
        }
        return result;
    }
    if (response.disposition == CdnResponseDisposition::not_modified) {
        if (!not_modified_valid(response, decoded_body, conditional_etag)) {
            return {.state = CdnGifState::invalid};
        }
        CacheLeaseScope cache_lease(pipeline->cache, key, response.expires_at_seconds);
        const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
        if (!etag_correlated(cached, conditional_etag)) {
            return {.state = CdnGifState::uncorrelated_not_modified};
        }
        CdnGifResult result = decode_gif_bytes(pipeline, cached.bytes, 0, cached.generation,
                                               response.store_allowed ? CdnGifState::decoded_cache_hit
                                                                      : CdnGifState::decoded_transient,
                                               cached.content_identity);
        if (!result.decoded()) {
            cache_lease.release();
            encoded_media_cache_erase(pipeline->cache, key);
            return result;
        }
        if (!response.store_allowed) {
            result.cache_generation = 0;
            pipeline->gif_content_identity = 0;
        }
        finish_not_modified(pipeline, key, &cache_lease, response);
        return result;
    }
    return {.state = CdnGifState::http_rejected};
}

CdnGifResult cdn_media_pipeline_decode_gif_frame(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                 u16 cache_generation, u32 frame_index,
                                                 u64 now_seconds) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage || !pipeline->gif_workspace ||
        !gif_key_valid(key) || cache_generation == 0) {
        return {.state = CdnGifState::invalid};
    }
    CacheLeaseScope cache_lease(pipeline->cache, key, now_seconds);
    const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
    if (!cached.found()) {
        return {.state = CdnGifState::cache_miss};
    }
    if (cached.generation != cache_generation) {
        return {.state = CdnGifState::generation_mismatch};
    }
    CdnGifResult result = decode_gif_bytes(pipeline, cached.bytes, frame_index, cached.generation,
                                           CdnGifState::decoded_cache_hit, cached.content_identity);
    if (!result.decoded()) {
        cache_lease.release();
        encoded_media_cache_erase(pipeline->cache, key);
    }
    return result;
}

CdnAnimatedWebpResult cdn_media_pipeline_lookup_animated_webp(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                              u64 now_seconds) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage || !pipeline->webp_workspace ||
        !pipeline->animated_webp_workspace || !animated_webp_key_valid(key)) {
        return {.state = CdnAnimatedWebpState::invalid};
    }
    CacheLeaseScope cache_lease(pipeline->cache, key, now_seconds);
    const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
    if (cached.state != EncodedMediaCacheLookupState::fresh) {
        return unfresh_lookup<CdnAnimatedWebpResult>(pipeline, cached);
    }
    CdnAnimatedWebpResult result = decode_animated_webp_bytes(pipeline, cached.bytes, 0, cached.generation,
                                                              CdnAnimatedWebpState::decoded_cache_hit, cached.content_identity);
    if (!result.decoded() && result.state != CdnAnimatedWebpState::lossy_workspace_required) {
        cache_lease.release();
        encoded_media_cache_erase(pipeline->cache, key);
        return result;
    }
    if (!result.decoded()) {
        return result;
    }
    result.etag = copy_result_etag(pipeline, cached.etag);
    return result;
}

CdnAnimatedWebpResult cdn_media_pipeline_admit_animated_webp(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                             const CdnResponseResult& response,
                                                             ConstSlice<u8> decoded_body,
                                                             ConstSlice<u8> conditional_etag) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage || !pipeline->webp_workspace ||
        !pipeline->animated_webp_workspace || !animated_webp_key_valid(key) || !response.ok()) {
        return {.state = response.ok() ? CdnAnimatedWebpState::invalid
                                       : CdnAnimatedWebpState::http_rejected};
    }
    if (response.disposition == CdnResponseDisposition::accept_body) {
        if (!accepted_body_valid(response, decoded_body)) {
            return {.state = CdnAnimatedWebpState::invalid};
        }
        CdnAnimatedWebpResult result =
            decode_animated_webp_bytes(pipeline, decoded_body, 0, 0,
                                       response.store_allowed ? CdnAnimatedWebpState::decoded_response
                                                              : CdnAnimatedWebpState::decoded_transient, 0);
        if (!result.decoded()) {
            return result;
        }
        result.store = store_decoded_response(pipeline, key, decoded_body, response);
        if (!result.store.stored()) {
            result.state = CdnAnimatedWebpState::decoded_transient;
            result.cache_generation = 0;
        } else {
            result.cache_generation = result.store.generation;
        }
        return result;
    }
    if (response.disposition == CdnResponseDisposition::not_modified) {
        if (!not_modified_valid(response, decoded_body, conditional_etag)) {
            return {.state = CdnAnimatedWebpState::invalid};
        }
        CacheLeaseScope cache_lease(pipeline->cache, key, response.expires_at_seconds);
        const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
        if (!etag_correlated(cached, conditional_etag)) {
            return {.state = CdnAnimatedWebpState::uncorrelated_not_modified};
        }
        CdnAnimatedWebpResult result =
            decode_animated_webp_bytes(pipeline, cached.bytes, 0, cached.generation,
                                       response.store_allowed ? CdnAnimatedWebpState::decoded_cache_hit
                                                              : CdnAnimatedWebpState::decoded_transient, cached.content_identity);
        if (!result.decoded() && result.state != CdnAnimatedWebpState::lossy_workspace_required) {
            cache_lease.release();
            encoded_media_cache_erase(pipeline->cache, key);
            return result;
        }
        if (!result.decoded()) {
            return result;
        }
        if (!response.store_allowed) {
            result.cache_generation = 0;
            pipeline->animated_webp_workspace->retained.content_identity = 0;
        }
        finish_not_modified(pipeline, key, &cache_lease, response);
        return result;
    }
    return {.state = CdnAnimatedWebpState::http_rejected};
}

CdnAnimatedWebpResult cdn_media_pipeline_decode_animated_webp_frame(CdnMediaPipeline* pipeline,
                                                                    MediaAssetKey key, u16 cache_generation,
                                                                    u32 frame_index,
                                                                    u64 now_seconds) noexcept {
    if (!pipeline || !pipeline->cache || !pipeline->scanline_storage || !pipeline->webp_workspace ||
        !pipeline->animated_webp_workspace || !animated_webp_key_valid(key) || cache_generation == 0) {
        return {.state = CdnAnimatedWebpState::invalid};
    }
    CacheLeaseScope cache_lease(pipeline->cache, key, now_seconds);
    const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
    if (!cached.found()) {
        return {.state = CdnAnimatedWebpState::cache_miss};
    }
    if (cached.generation != cache_generation) {
        return {.state = CdnAnimatedWebpState::generation_mismatch};
    }
    CdnAnimatedWebpResult result = decode_animated_webp_bytes(
        pipeline, cached.bytes, frame_index, cached.generation, CdnAnimatedWebpState::decoded_cache_hit, cached.content_identity);
    if (!result.decoded() && result.state != CdnAnimatedWebpState::lossy_workspace_required) {
        cache_lease.release();
        encoded_media_cache_erase(pipeline->cache, key);
    }
    return result;
}

namespace {
bool audio_key_valid(MediaAssetKey key) noexcept {
    return media_asset_key_valid(key) && key.kind == MediaAssetKind::attachment &&
           media_attachment_format_audio_supported(media_asset_attachment_format(key));
}
EncodedAudioError audio_error(OggOpusError error) noexcept {
    switch (error) {
    case OggOpusError::none:
        return EncodedAudioError::none;
    case OggOpusError::invalid_input:
        return EncodedAudioError::invalid_input;
    case OggOpusError::unsupported:
        return EncodedAudioError::unsupported;
    case OggOpusError::truncated:
        return EncodedAudioError::truncated;
    case OggOpusError::checksum:
        return EncodedAudioError::checksum;
    case OggOpusError::capacity:
        return EncodedAudioError::capacity;
    case OggOpusError::cancelled:
        return EncodedAudioError::cancelled;
    default:
        return EncodedAudioError::malformed;
    }
}
EncodedAudioError audio_error(WavePcmError error) noexcept {
    switch (error) {
    case WavePcmError::none:
        return EncodedAudioError::none;
    case WavePcmError::invalid_input:
        return EncodedAudioError::invalid_input;
    case WavePcmError::unsupported:
        return EncodedAudioError::unsupported;
    case WavePcmError::truncated:
        return EncodedAudioError::truncated;
    case WavePcmError::capacity:
        return EncodedAudioError::capacity;
    default:
        return EncodedAudioError::malformed;
    }
}
EncodedAudioError audio_error(Mp3Error error) noexcept {
    switch (error) {
    case Mp3Error::none:
        return EncodedAudioError::none;
    case Mp3Error::invalid_input:
        return EncodedAudioError::invalid_input;
    case Mp3Error::unsupported:
        return EncodedAudioError::unsupported;
    case Mp3Error::truncated:
        return EncodedAudioError::truncated;
    case Mp3Error::capacity:
        return EncodedAudioError::capacity;
    case Mp3Error::cancelled:
        return EncodedAudioError::cancelled;
    default:
        return EncodedAudioError::malformed;
    }
}
CdnImageType audio_http_type(MediaAssetKey key) noexcept {
    const auto format = media_asset_attachment_format(key);
    return format == MediaAttachmentFormat::mp3   ? CdnImageType::mp3
           : format == MediaAttachmentFormat::wav ? CdnImageType::wav
           : format == MediaAttachmentFormat::m4a ? CdnImageType::m4a
                                                  : CdnImageType::ogg_opus;
}
EncodedAudioError parse_audio(EncodedAudioMetadata* metadata, MediaAttachmentFormat format,
                              ConstSlice<u8> bytes, OggOpusCancelled cancelled, void* context) noexcept {
    if (!bytes.data || !bytes.count)
        return EncodedAudioError::invalid_input;
    if (cancelled && cancelled(context))
        return EncodedAudioError::cancelled;
    if (bytes.count > kCdnMaximumEncodedBodyBytes)
        return EncodedAudioError::capacity;
    switch (format) {
    case MediaAttachmentFormat::ogg_opus: {
        OggOpusDemuxer source{};
        u8 scratch[kOggOpusMaximumPacketBytes];
        const auto error = audio_error(
            ogg_opus_demux_initialize(&source, bytes, {scratch, sizeof(scratch)}, cancelled, context));
        if (error == EncodedAudioError::none)
            *metadata = EncodedAudioMetadata{source};
        return error;
    }
    case MediaAttachmentFormat::mp3: {
        Mp3Stream source{};
        const auto error = audio_error(mp3_stream_initialize(&source, bytes, cancelled, context));
        if (error == EncodedAudioError::none)
            *metadata = EncodedAudioMetadata{source};
        return error;
    }
    case MediaAttachmentFormat::wav: {
        WavePcmSource source{};
        const auto error = audio_error(wave_pcm_parse(&source, bytes));
        if (cancelled && cancelled(context))
            return EncodedAudioError::cancelled;
        if (error == EncodedAudioError::none)
            *metadata = EncodedAudioMetadata{source};
        return error;
    }
    case MediaAttachmentFormat::m4a: {
        Mp4AudioDemuxer source{};
        const auto demuxed = mp4_audio_demux_initialize(&source, bytes);
        if (cancelled && cancelled(context))
            return EncodedAudioError::cancelled;
        const auto error = demuxed == Mp4DemuxError::none          ? EncodedAudioError::none
                           : demuxed == Mp4DemuxError::unsupported ||
                                   demuxed == Mp4DemuxError::no_audio
                               ? EncodedAudioError::unsupported
                               : EncodedAudioError::malformed;
        if (error == EncodedAudioError::none)
            *metadata = EncodedAudioMetadata{source};
        return error;
    }
    default:
        return EncodedAudioError::unsupported;
    }
}
} // namespace

CdnEncodedAudioResult cdn_media_pipeline_lookup_audio(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                      u64 now_seconds, OggOpusCancelled cancelled,
                                                      void* context) noexcept {
    CdnEncodedAudioResult result{};
    if (!pipeline || !pipeline->cache || !audio_key_valid(key)) {
        result.error = EncodedAudioError::invalid_input;
        return result;
    }
    result.lease = encoded_media_cache_acquire(pipeline->cache, key, now_seconds);
    if (!result.lease.acquired) {
        result.state = CdnEncodedAudioState::cache_miss;
        return result;
    }
    if (result.lease.lookup.state == EncodedMediaCacheLookupState::stale) {
        result.etag = copy_result_etag(pipeline, result.lease.lookup.etag);
        encoded_media_cache_release(pipeline->cache, &result.lease);
        result.state = CdnEncodedAudioState::fetch_required;
        return result;
    }
    result.error = parse_audio(&result.metadata, media_asset_attachment_format(key),
                               result.lease.lookup.bytes, cancelled, context);
    if (result.error != EncodedAudioError::none) {
        if (result.error != EncodedAudioError::cancelled)
            encoded_media_cache_retire(pipeline->cache, key, result.lease.generation);
        encoded_media_cache_release(pipeline->cache, &result.lease);
        result.state = CdnEncodedAudioState::decode_rejected;
        return result;
    }
    result.state = CdnEncodedAudioState::ready_cache_hit;
    return result;
}

CdnEncodedAudioResult cdn_media_pipeline_admit_audio(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                     const CdnResponseResult& response,
                                                     ConstSlice<u8> body, ConstSlice<u8> conditional_etag,
                                                     OggOpusCancelled cancelled, void* context) noexcept {
    CdnEncodedAudioResult result{};
    if (!pipeline || !pipeline->cache || !audio_key_valid(key)) {
        result.error = EncodedAudioError::invalid_input;
        return result;
    }
    if (!response.ok() ||
        (response.disposition != CdnResponseDisposition::accept_body &&
         response.disposition != CdnResponseDisposition::not_modified) ||
        response.body_bytes != body.count ||
        body.count > cdn_encoded_body_limit(audio_http_type(key))) {
        result.state = CdnEncodedAudioState::http_rejected;
        result.error = EncodedAudioError::invalid_input;
        return result;
    }
    if (response.disposition == CdnResponseDisposition::not_modified) {
        result.lease = encoded_media_cache_acquire(pipeline->cache, key, 0);
        if (body.count != 0 || !conditional_etag.data ||
            !etag_correlated(result.lease.lookup, conditional_etag)) {
            if (result.lease.acquired)
                encoded_media_cache_release(pipeline->cache, &result.lease);
            result.state = CdnEncodedAudioState::uncorrelated_not_modified;
            return result;
        }
        if (!response.store_allowed)
            encoded_media_cache_retire(pipeline->cache, key, result.lease.generation);
        result.error = parse_audio(&result.metadata, media_asset_attachment_format(key),
                                   result.lease.lookup.bytes, cancelled, context);
        if (result.error == EncodedAudioError::none) {
            if (response.store_allowed) {
                const u64 content_identity = result.lease.lookup.content_identity;
                const u16 generation = result.lease.generation;
                encoded_media_cache_release(pipeline->cache, &result.lease);
                encoded_media_cache_refresh(pipeline->cache, key, response.expires_at_seconds,
                                            response.must_revalidate, response.etag, generation);
                result.lease = encoded_media_cache_acquire(pipeline->cache, key, 0);
                if (!result.lease.acquired || result.lease.generation != generation ||
                    result.lease.lookup.content_identity != content_identity) {
                    if (result.lease.acquired)
                        encoded_media_cache_release(pipeline->cache, &result.lease);
                    result.metadata = {};
                    result.state = CdnEncodedAudioState::uncorrelated_not_modified;
                    return result;
                }
                if (!encoded_audio_rebind(&result.metadata, result.lease.lookup.bytes)) {
                    encoded_media_cache_release(pipeline->cache, &result.lease);
                    result.metadata = {};
                    result.state = CdnEncodedAudioState::uncorrelated_not_modified;
                    return result;
                }
            }
            result.state = CdnEncodedAudioState::ready_response;
            // A no-store 304 must not extend reusable cache ownership. Its
            // existing lease keeps bytes alive through joined playback release.
            if (!response.store_allowed)
                result.state = CdnEncodedAudioState::ready_transient;
            return result;
        }
    } else {
        if (!response.store_allowed)
            encoded_media_cache_retire(pipeline->cache, key);
        result.error =
            parse_audio(&result.metadata, media_asset_attachment_format(key), body, cancelled, context);
        if (result.error == EncodedAudioError::none) {
            const auto stored =
                encoded_media_cache_store(pipeline->cache, key, body,
                                          {response.etag, response.expires_at_seconds,
                                           response.store_allowed, response.must_revalidate});
            if (stored.stored()) {
                result.lease = encoded_media_cache_acquire(pipeline->cache, key, 0);
                if (result.lease.acquired && result.lease.generation == stored.generation &&
                    encoded_audio_rebind(&result.metadata, result.lease.lookup.bytes)) {
                    result.state = CdnEncodedAudioState::ready_response;
                    return result;
                }
                if (result.lease.acquired)
                    encoded_media_cache_release(pipeline->cache, &result.lease);
            }
            result.state = CdnEncodedAudioState::ready_transient;
            return result;
        }
    }
    if (result.lease.acquired) {
        if (result.error != EncodedAudioError::cancelled)
            encoded_media_cache_retire(pipeline->cache, key, result.lease.generation);
        encoded_media_cache_release(pipeline->cache, &result.lease);
    }
    result.state = CdnEncodedAudioState::decode_rejected;
    return result;
}

} // namespace maicord
