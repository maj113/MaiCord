#include "maicord/app/cdn_media_pipeline.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/webp_alpha.hpp"
#include "maicord/media/webp_thumbnail.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/crt.hpp"
#include "maicord/media/asset.hpp"
#include "maicord/protocol/cdn_http.hpp"

namespace maicord {
namespace {

static_assert(is_trivially_default_constructible_v<JpegWorkspace>);
static_assert(is_trivially_default_constructible_v<LossyWebpWorkspace>);

struct CacheLeaseScope {
    EncodedMediaCache* cache;
    EncodedMediaCacheLease lease;

    __declspec(noinline) CacheLeaseScope(EncodedMediaCache* source, const MediaAssetKey& key,
                                         u64 now_seconds) noexcept
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

// Results are reset and given a state out of line: a braced result written
// at each return would be zeroed in a temporary and copied.
template <typename Result> __declspec(noinline) void reset(Result* out) noexcept {
    reset_value<Result>(out);
}
template <typename Result>
__declspec(noinline) void set_state(Result* out, decltype(Result::state) state) noexcept {
    reset_value<Result>(out);
    out->state = state;
}

// The answer for a lookup with nothing to decode: a miss, a stale entry (its ETag and generation
// for the conditional request) or an unusable one.
template <typename Result>
void unfresh_lookup(CdnMediaPipeline* pipeline, const EncodedMediaCacheLookup& cached,
                    Result* out) noexcept {
    reset(out);
    using State = decltype(Result::state);
    if (cached.state == EncodedMediaCacheLookupState::miss) {
        out->state = State::cache_miss;
    } else if (cached.state == EncodedMediaCacheLookupState::stale) {
        out->state = State::fetch_required;
        out->etag = copy_result_etag(pipeline, cached.etag);
        if constexpr (requires { out->cache_generation; }) {
            out->cache_generation = cached.generation;
        }
    } else {
        out->state = State::invalid;
    }
}

bool static_image_key_valid(const MediaAssetKey& key, MediaAttachmentFormat format) noexcept {
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
        matches = stored == format ||
                  (format == MediaAttachmentFormat::jpeg && stored == MediaAttachmentFormat::unknown);
        break;
    }
    case MediaAssetKind::profile_image:
    case MediaAssetKind::embed:
        matches = media_asset_embed_format(key) == format;
        break;
    default:
        break;
    }
    return matches &&
           ((key.kind == MediaAssetKind::embed || key.kind == MediaAssetKind::profile_image ||
             key.kind == MediaAssetKind::catalog_preview || key.kind == MediaAssetKind::local_draft_preview)
                ? (key.flags & media_asset_flag_animated) == 0
                : key.flags == media_asset_flag_none);
}

bool animated_webp_key_valid(const MediaAssetKey& key) noexcept {
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

bool apng_key_valid(const MediaAssetKey& key) noexcept {
    return media_asset_key_valid(key) &&
           ((key.kind == MediaAssetKind::sticker && key.variant == 2) ||
            (key.kind == MediaAssetKind::catalog_preview &&
             media_asset_catalog_format(key) == MediaAttachmentFormat::png) ||
            (key.kind == MediaAssetKind::profile_image &&
             media_asset_embed_format(key) == MediaAttachmentFormat::png)) &&
           ((key.flags & media_asset_flag_animated) != 0 || key.kind == MediaAssetKind::catalog_preview);
}

bool gif_key_valid(const MediaAssetKey& key) noexcept {
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

bool ensure_scanlines(CdnMediaPipeline* pipeline, u64 required) noexcept {
    if (required > 0xFFFFFFFFull)
        return false;
    if (pipeline->grow_scanlines && required > pipeline->scanline_capacity) {
        const auto grown = pipeline->grow_scanlines(pipeline->scanline_owner, static_cast<u32>(required));
        if (grown.data && grown.count >= required) {
            pipeline->scanline_storage = grown.data;
            pipeline->scanline_capacity = grown.count;
        }
    }
    return required <= pipeline->scanline_capacity;
}

void decode_bytes(CdnMediaPipeline* pipeline, ConstSlice<u8> encoded, CdnStaticPngState success_state,
                  CdnStaticPngResult* out) noexcept {
    reset(out);
    const auto parsed = png_parse(encoded, &out->image);
    if (parsed.ok()) {
        // Interlaced files and files with many IDAT chunks need room past the output.
        ensure_scanlines(pipeline, png_decode_buffer_bytes(out->image));
        out->decode = png_decode_parsed_scanlines(
            &out->image, &pipeline->workspace, {pipeline->scanline_storage, pipeline->scanline_capacity});
    } else {
        out->decode.error = parsed.error;
    }
    out->decode.parse = parsed;
    if (!out->decode.ok()) {
        out->state = CdnStaticPngState::decode_rejected;
        return;
    }
    out->scanlines = ConstSlice<u8>{pipeline->scanline_storage, out->image.inflated_bytes};
    out->state = success_state;
}

// Rows box-filtered by an integer factor into straight RGBA8; colour is
// alpha-weighted so transparent pixels do not darken edges.
struct ReducedWebpRows {
    u8* output;
    u32* sums; // per output column: weighted R, G, B, alpha
    u32 source_width, source_height, scale, width;
};

bool reduced_webp_row(void* context, u32 y, const u8* rgba) noexcept {
    auto& r = *static_cast<ReducedWebpRows*>(context);
    if (r.scale == 1) {
        copy_bytes(r.output + static_cast<usize>(y) * r.width * 4u, rgba, r.width * 4u);
        return true;
    }
    for (u32 column = 0, x = 0; column < r.width; ++column) {
        u32* sum = r.sums + column * 4u;
        const u32 end = minimum(x + r.scale, r.source_width);
        for (; x < end; ++x) {
            const u32 alpha = rgba[x * 4u + 3u];
            sum[0] += rgba[x * 4u] * alpha;
            sum[1] += rgba[x * 4u + 1u] * alpha;
            sum[2] += rgba[x * 4u + 2u] * alpha;
            sum[3] += alpha;
        }
    }
    if ((y + 1u) % r.scale != 0 && y + 1u != r.source_height)
        return true;
    u8* out = r.output + static_cast<usize>(y / r.scale) * r.width * 4u;
    const u32 rows = y % r.scale + 1u;
    for (u32 x = 0; x < r.width; ++x) {
        u32* sum = r.sums + x * 4u;
        const u32 alpha = sum[3];
        for (u32 c = 0; c < 3; ++c)
            out[x * 4u + c] = alpha ? static_cast<u8>((sum[c] + alpha / 2u) / alpha) : 0;
        const u32 samples = minimum(r.scale, r.source_width - x * r.scale) * rows;
        out[x * 4u + 3u] = static_cast<u8>((alpha + samples / 2u) / samples);
        for (u32 c = 0; c < 4; ++c)
            sum[c] = 0;
    }
    return true;
}

// A still lossy WebP past the GPU path decodes on the CPU into straight RGBA8
// at most `extent` px per side; a compressed ALPH plane decodes whole first.
void reduce_webp_bytes(CdnMediaPipeline* pipeline, ConstSlice<u8> encoded, u32 extent,
                       CdnStaticWebpState success_state, CdnStaticWebpResult* out) noexcept {
    reset(out);
    out->state = CdnStaticWebpState::decode_rejected;
    out->decode.error = WebpError::dimensions;
    WebpThumbnailLayout layout{};
    if (webp_thumbnail_parse(encoded, &layout) != WebpThumbnailError::none)
        return;
    const u32 longest = maximum(layout.width, layout.height);
    const u32 scale = longest > extent ? (longest + extent - 1u) / extent : 1u;
    const u32 width = (layout.width + scale - 1u) / scale, height = (layout.height + scale - 1u) / scale;
    const auto align = [](u64 value) noexcept { return (value + 15u) & ~15ull; };
    const u64 pixels = u64{layout.width} * layout.height;
    const u64 output_bytes = align(u64{width} * height * 4u);
    const u64 alpha_bytes = layout.alpha_chunk.data ? align(pixels) : 0;
    const u64 row_bytes = align(u64{width} * 4u * sizeof(u32)) + layout.workspace_bytes;
    // The ALPH decoder's ARGB scratch is free again before the rows decode.
    const u64 tail = maximum(layout.alpha_chunk.data ? pixels * 4u : 0ull, row_bytes);
    if (!ensure_scanlines(pipeline, output_bytes + alpha_bytes + tail + 16u)) {
        out->decode.error = WebpError::output_capacity;
        return;
    }
    u8* base = reinterpret_cast<u8*>((reinterpret_cast<usize>(pipeline->scanline_storage) + 15u) & ~usize{15});
    u8* alpha = base + output_bytes;
    u8* scratch = alpha + alpha_bytes;
    if (layout.alpha_chunk.data) {
        if (!pipeline->webp_workspace)
            return;
        const auto decoded = webp_alpha_decode(layout.alpha_chunk, layout.width, layout.height,
                                               pipeline->webp_workspace, {scratch, static_cast<u32>(pixels * 4u)},
                                               {alpha, static_cast<u32>(pixels)});
        if (!decoded.ok()) {
            out->decode.error =
                decoded.lossless.error != WebpError::none ? decoded.lossless.error : WebpError::transform;
            return;
        }
        layout.alpha = {alpha, static_cast<u32>(pixels)};
        layout.alpha_filter = 0;
    }
    u32* sums = reinterpret_cast<u32*>(scratch);
    clear_bytes(sums, width * 4u * sizeof(u32));
    ReducedWebpRows rows{base, sums, layout.width, layout.height, scale, width};
    const u32 sums_bytes = static_cast<u32>(align(u64{width} * 4u * sizeof(u32)));
    if (webp_thumbnail_decode(layout, {scratch + sums_bytes, layout.workspace_bytes}, reduced_webp_row, &rows) !=
        WebpThumbnailError::none) {
        out->decode.error = WebpError::bitstream;
        return;
    }
    out->image.width = width;
    out->image.height = height;
    out->image.has_alpha = layout.alpha.data != nullptr;
    out->rgba8 = {base, width * height * 4u};
    out->decode.rgba_bytes = width * height * 4u;
    out->decode.error = WebpError::none;
    out->state = success_state;
}

void decode_webp_bytes(CdnMediaPipeline* pipeline, ConstSlice<u8> encoded, CdnStaticWebpState success_state,
                       CdnStaticWebpResult* out) noexcept {
    reset(out);
    if ((reinterpret_cast<usize>(pipeline->scanline_storage) & 3u) != 0) {
        return set_state(out, CdnStaticWebpState::invalid);
    }
    WebpImage parsed_image{};
    const WebpParseResult parsed = webp_parse(encoded, &parsed_image);
    if (parsed.error == WebpError::dimensions)
        return reduce_webp_bytes(pipeline, encoded, kReducedWebpExtent, success_state, out);
    if (!parsed.ok()) {
        out->decode.parse = parsed;
        out->decode.error = parsed.error;
        out->state = CdnStaticWebpState::decode_rejected;
        return;
    }
    if (parsed_image.codec == WebpCodec::vp8l_lossless) {
        if (!pipeline->webp_workspace) {
            return set_state(out, CdnStaticWebpState::invalid);
        }
        ensure_scanlines(pipeline, u64{parsed_image.width} * parsed_image.height * 4u);
        out->decode =
            webp_lossless_decode_rgba8(encoded, &out->image, pipeline->webp_workspace,
                                       Slice<u8>{pipeline->scanline_storage, pipeline->scanline_capacity});
        if (!out->decode.ok()) {
            out->state = CdnStaticWebpState::decode_rejected;
            return;
        }
        out->rgba8 = ConstSlice<u8>{pipeline->scanline_storage, out->decode.rgba_bytes};
    } else if (parsed_image.codec == WebpCodec::vp8_lossy) {
        if (!pipeline->lossy_webp_workspace) {
            return set_state(out, CdnStaticWebpState::lossy_workspace_required);
        }
        const u32 pixels = parsed_image.width * parsed_image.height;
        const u32 scratch_bytes = pixels * 4u;
        const u32 required = parsed_image.has_alpha ? scratch_bytes + pixels : 0u;
        if (required > pipeline->scanline_capacity) {
            return set_state(out, CdnStaticWebpState::invalid);
        }
        pipeline->lossy_webp_workspace = ::new (pipeline->lossy_webp_workspace) LossyWebpWorkspace;
        LossyWebpWorkspace* workspace = pipeline->lossy_webp_workspace;
        out->lossy_decode = lossy_webp_decode_keyframe(
            encoded, &out->image, &out->vp8_frame, &out->vp8_header, &workspace->probabilities,
            &workspace->modes, &workspace->coefficients,
            parsed_image.has_alpha ? pipeline->webp_workspace : nullptr,
            parsed_image.has_alpha ? Slice<u8>{pipeline->scanline_storage, scratch_bytes} : Slice<u8>{},
            parsed_image.has_alpha ? Slice<u8>{pipeline->scanline_storage + scratch_bytes, pixels}
                                   : Slice<u8>{});
        if (!out->lossy_decode.ok()) {
            out->decode.parse = out->lossy_decode.parse;
            out->decode.error = out->lossy_decode.parse.error != WebpError::none
                                    ? out->lossy_decode.parse.error
                                    : WebpError::bitstream;
            out->state = CdnStaticWebpState::decode_rejected;
            return;
        }
        if (parsed_image.has_alpha) {
            out->alpha =
                ConstSlice<u8>{pipeline->scanline_storage + scratch_bytes, out->lossy_decode.alpha_bytes};
        }
    } else {
        out->decode.parse = parsed;
        out->decode.error = WebpError::unsupported_feature;
        out->state = CdnStaticWebpState::decode_rejected;
        return;
    }
    out->state = success_state;
}

struct ReducedJpegRows {
    u8* output;
    u32 width, height; // As stored, before orientation.
    JpegOrientation orientation;
};

bool reduced_jpeg_row(void* context, u32 y, const u8* rgba) noexcept {
    const auto& r = *static_cast<const ReducedJpegRows*>(context);
    const u32 w = r.width, h = r.height;
    if (r.orientation == JpegOrientation::top_left || r.orientation == JpegOrientation::bottom_left) {
        const u32 row = r.orientation == JpegOrientation::top_left ? y : h - 1u - y;
        copy_bytes(r.output + static_cast<usize>(row) * w * 4u, rgba, w * 4u);
        return true;
    }
    const u32 pitch = jpeg_orientation_swaps_dimensions(r.orientation) ? h : w;
    for (u32 x = 0; x < w; ++x) {
        u32 dx = x, dy = y;
        switch (r.orientation) {
        case JpegOrientation::top_right: dx = w - 1 - x; break;
        case JpegOrientation::bottom_right: dx = w - 1 - x; dy = h - 1 - y; break;
        case JpegOrientation::bottom_left: dy = h - 1 - y; break;
        case JpegOrientation::left_top: dx = y; dy = x; break;
        case JpegOrientation::right_top: dx = h - 1 - y; dy = x; break;
        case JpegOrientation::right_bottom: dx = h - 1 - y; dy = w - 1 - x; break;
        case JpegOrientation::left_bottom: dx = y; dy = w - 1 - x; break;
        default: break;
        }
        copy_bytes(r.output + (static_cast<usize>(dy) * pitch + dx) * 4u, rgba + static_cast<usize>(x) * 4u,
                   4);
    }
    return true;
}

// Decodes a JPEG past the GPU coefficient path's extent into oriented
// straight RGBA8 at the coarsest DCT scale reaching `target`, at most
// `limit` px per side.
void reduce_jpeg_bytes(CdnMediaPipeline* pipeline, ConstSlice<u8> encoded, u32 target, u32 limit,
                       CdnStaticJpegState success_state, CdnStaticJpegResult* out) noexcept {
    reset(out);
    out->state = CdnStaticJpegState::decode_rejected;
    pipeline->jpeg_workspace = ::new (pipeline->jpeg_workspace) JpegWorkspace;
    JpegThumbnailLayout layout{};
    out->decode.parse =
        jpeg_thumbnail_parse(encoded, target, &out->image, pipeline->jpeg_workspace, &layout);
    out->decode.error = out->decode.parse.error;
    if (!out->decode.ok())
        return;
    if (layout.width > limit || layout.height > limit) {
        out->decode.error = JpegError::dimensions;
        return;
    }
    const u32 rgba_bytes = static_cast<u32>(layout.rgba_bytes);
    if (!ensure_scanlines(pipeline, rgba_bytes)) {
        out->decode.error = JpegError::sink;
        return;
    }
    // Scratch (16-byte aligned) and one row: the idle coefficient workspace,
    // else a temporary allocation for large progressive files.
    const u64 needed = layout.coefficient_bytes + layout.plane_bytes + u64{layout.width} * 4u + 15u;
    constexpr u64 kTemporaryScratchMaximum = 16ull * 1024u * 1024u;
    u8* workspace = reinterpret_cast<u8*>(&pipeline->jpeg_workspace->coefficients);
    void* temporary = nullptr;
    if (needed > sizeof(JpegCoefficientWorkspace)) {
        if (needed <= kTemporaryScratchMaximum)
            temporary =
                VirtualAlloc(nullptr, static_cast<SIZE_T>(needed), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!temporary) {
            out->decode.error = JpegError::sink;
            return;
        }
        workspace = static_cast<u8*>(temporary);
    }
    u8* scratch =
        reinterpret_cast<u8*>((reinterpret_cast<usize>(workspace) + 15u) & ~static_cast<usize>(15u));
    const u32 scratch_bytes = static_cast<u32>(layout.coefficient_bytes + layout.plane_bytes);
    ReducedJpegRows rows{pipeline->scanline_storage, layout.width, layout.height, out->image.orientation};
    out->decode =
        jpeg_thumbnail_decode_rows(encoded, out->image, layout, {scratch, scratch_bytes},
                                   {scratch + scratch_bytes, layout.width * 4u}, reduced_jpeg_row, &rows);
    if (temporary)
        VirtualFree(temporary, 0, MEM_RELEASE);
    if (!out->decode.ok())
        return;
    const bool swap = jpeg_orientation_swaps_dimensions(out->image.orientation);
    out->rgba8 = {pipeline->scanline_storage, rgba_bytes};
    out->rgba_width = static_cast<u16>(swap ? layout.height : layout.width);
    out->rgba_height = static_cast<u16>(swap ? layout.width : layout.height);
    out->state = success_state;
}

void decode_jpeg_bytes(CdnMediaPipeline* pipeline, const MediaAssetKey& key, ConstSlice<u8> encoded,
                       CdnStaticJpegState success_state, CdnStaticJpegResult* out) noexcept {
    reset(out);
    if (!pipeline->jpeg_workspace) {
        return set_state(out, CdnStaticJpegState::jpeg_workspace_required);
    }
    pipeline->jpeg_workspace = ::new (pipeline->jpeg_workspace) JpegWorkspace;
    out->decode = jpeg_decode_coefficients(encoded, &out->image, pipeline->jpeg_workspace);
    if (out->decode.error == JpegError::dimensions || out->decode.error == JpegError::encoded_limit) {
        const bool profile = key.kind == MediaAssetKind::profile_image;
        return reduce_jpeg_bytes(
            pipeline, encoded, profile ? kReducedJpegExtent : kReducedJpegPreviewExtent,
            profile ? kJpegDimensionMaximum : kReducedJpegPreviewExtent, success_state, out);
    }
    if (!out->decode.ok()) {
        out->state = CdnStaticJpegState::decode_rejected;
        return;
    }
    out->coefficients = &pipeline->jpeg_workspace->coefficients;
    out->state = success_state;
}

void decode_animated_webp_bytes(CdnMediaPipeline* pipeline, ConstSlice<u8> encoded, u32 frame_index,
                                u16 cache_generation, CdnAnimatedWebpState success_state,
                                u64 content_identity, CdnAnimatedWebpResult* out) noexcept {
    reset(out);
    if (!pipeline->webp_workspace || !pipeline->animated_webp_workspace ||
        (reinterpret_cast<usize>(pipeline->scanline_storage) & 3u) != 0) {
        return set_state(out, CdnAnimatedWebpState::invalid);
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
        out->decode.parse = {saved.chunks, saved.frame_chunks, AnimatedWebpError::none};
    } else {
        out->decode.parse = animated_webp_parse(encoded, &animation, &workspace);
    }
    if (!out->decode.parse.ok() || frame_index >= animation.frame_count) {
        saved.content_identity = 0;
        out->decode.error =
            out->decode.parse.ok() ? AnimatedWebpError::frame_count : out->decode.parse.error;
        out->state = CdnAnimatedWebpState::decode_rejected;
        return;
    }
    const AnimatedWebpParseResult animation_parse = out->decode.parse;
    bool lossy_present = retained && saved.lossy_present;
    if (!retained) {
        for (u32 index = 0; index < animation.frame_count; ++index)
            lossy_present = lossy_present || animation.frames[index].codec == AnimatedWebpCodec::vp8_lossy;
    }
    if (lossy_present && !pipeline->lossy_webp_workspace) {
        out->state = CdnAnimatedWebpState::lossy_workspace_required;
        return;
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
        out->decode.error = AnimatedWebpError::output_capacity;
        out->state = CdnAnimatedWebpState::decode_rejected;
        return;
    }
    if (requested.codec == AnimatedWebpCodec::vp8_lossy) {
        pipeline->lossy_webp_workspace = ::new (pipeline->lossy_webp_workspace) LossyWebpWorkspace;
    }
    out->decode = animated_webp_decode_parsed_frame(
        encoded, animation, frame_index, pipeline->webp_workspace, pipeline->lossy_webp_workspace,
        &out->vp8_frame, &out->vp8_header,
        Slice<u8>{pipeline->scanline_storage, pipeline->scanline_capacity},
        external_alpha ? Slice<u8>{pipeline->scanline_storage, scratch_bytes} : Slice<u8>{},
        external_alpha ? Slice<u8>{pipeline->scanline_storage + scratch_bytes,
                                   pipeline->scanline_capacity - scratch_bytes}
                       : Slice<u8>{});
    out->decode.parse = animation_parse;
    if (!out->decode.ok()) {
        saved.content_identity = 0;
        out->state = CdnAnimatedWebpState::decode_rejected;
        return;
    }
    if (!retained && content_identity != 0) {
        saved = {content_identity, animation.frames, animation.background_rgba, animation.frame_count,
                 animation.play_count, animation_parse.chunks, animation_parse.frame_chunks,
                 animation.width, animation.height, animation.has_alpha, animation.has_color_profile, lossy_present};
    }
    if (requested.codec == AnimatedWebpCodec::vp8l_lossless) {
        out->rgba8 = ConstSlice<u8>{pipeline->scanline_storage, out->decode.frame_decode.rgba_bytes};
    } else if (requested.has_alpha) {
        out->alpha = ConstSlice<u8>{pipeline->scanline_storage + scratch_bytes,
                                    out->decode.lossy_decode.alpha_bytes};
    }
    out->background_rgba = animation.background_rgba;
    out->frame_count = animation.frame_count;
    out->play_count = animation.play_count;
    out->canvas_width = animation.width;
    out->canvas_height = animation.height;
    out->cache_generation = cache_generation;
    out->state = success_state;
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
    reset_value<ApngImage>(image);
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

void decode_apng_bytes(CdnMediaPipeline* pipeline, ConstSlice<u8> encoded, u32 frame_index,
                       u16 cache_generation, CdnApngState success_state, bool validate_default,
                       u64 content_identity, CdnApngResult* out) noexcept {
    reset(out);
    if (!pipeline->apng_workspace) {
        return set_state(out, CdnApngState::invalid);
    }
    ApngImage animation{};
    const bool retained =
        restore_apng_metadata(pipeline, encoded, content_identity, &animation, &out->decode.parse);
    if (!retained) {
        pipeline->apng_metadata_valid = false;
        out->decode.parse = apng_parse(encoded, &animation, pipeline->apng_workspace);
    }
    if (!out->decode.parse.ok()) {
        out->decode.error = out->decode.parse.error;
        out->state = CdnApngState::decode_rejected;
        return;
    }
    if (validate_default && !animation.default_in_animation) {
        PngImage default_image{};
        const PngDecodeResult decoded_default =
            apng_decode_parsed_default(encoded, animation, &pipeline->workspace, &default_image,
                                       Slice<u8>{pipeline->scanline_storage, pipeline->scanline_capacity});
        if (!decoded_default.ok()) {
            pipeline->apng_metadata_valid = false;
            out->decode.png = decoded_default;
            out->decode.error = ApngError::inflate;
            out->state = CdnApngState::decode_rejected;
            return;
        }
    }
    const ApngParseResult parse = out->decode.parse;
    out->decode =
        apng_decode_parsed_frame(encoded, animation, frame_index, &pipeline->workspace, &out->image,
                                 Slice<u8>{pipeline->scanline_storage, pipeline->scanline_capacity});
    out->decode.parse = parse;
    if (!out->decode.ok()) {
        pipeline->apng_metadata_valid = false;
        out->state = CdnApngState::decode_rejected;
        return;
    }
    if (!retained) {
        retain_apng_metadata(pipeline, encoded, animation, parse, content_identity);
    }
    out->scanlines = ConstSlice<u8>{pipeline->scanline_storage, out->image.inflated_bytes};
    out->frame_count = animation.frame_count;
    out->play_count = animation.play_count;
    out->canvas_width = animation.canvas.width;
    out->canvas_height = animation.canvas.height;
    out->cache_generation = cache_generation;
    out->state = success_state;
}

void decode_gif_bytes(CdnMediaPipeline* pipeline, ConstSlice<u8> encoded, u32 frame_index,
                      u16 cache_generation, CdnGifState success_state, u64 content_identity,
                      CdnGifResult* out) noexcept {
    reset(out);
    if (!pipeline->gif_workspace) {
        return set_state(out, CdnGifState::invalid);
    }
    const bool retained = content_identity != 0 && content_identity == pipeline->gif_content_identity &&
                          pipeline->gif_image.frames == pipeline->gif_workspace->frames;
    GifImage animation{};
    if (retained) {
        animation = pipeline->gif_image;
        out->decode.parse = pipeline->gif_parse_result;
    } else {
        pipeline->gif_content_identity = 0;
        out->decode.parse = gif_parse(encoded, &animation, pipeline->gif_workspace);
    }
    if (!out->decode.parse.ok()) {
        out->decode.error = out->decode.parse.error;
        out->state = CdnGifState::decode_rejected;
        return;
    }
    if (frame_index >= animation.frame_count) {
        pipeline->gif_content_identity = 0;
        out->decode.error = GifError::invalid_input;
        out->state = CdnGifState::decode_rejected;
        return;
    }
    // The container validates all frame ranges above; pixel data is decoded per requested frame.
    const GifParseResult parse = out->decode.parse;
    gif_decode_parsed_frame(&out->decode, encoded, animation, frame_index, pipeline->gif_workspace,
                            Slice<u8>{pipeline->scanline_storage, pipeline->scanline_capacity});
    out->decode.parse = parse;
    if (!out->decode.ok()) {
        pipeline->gif_content_identity = 0;
        out->state = CdnGifState::decode_rejected;
        return;
    }
    if (!retained && content_identity != 0) {
        pipeline->gif_image = animation;
        pipeline->gif_parse_result = parse;
        pipeline->gif_content_identity = content_identity;
    }
    out->indices = ConstSlice<u8>{pipeline->scanline_storage, out->decode.pixels};
    out->frame_count = animation.frame_count;
    out->play_count = animation.play_count;
    out->canvas_width = animation.canvas_width;
    out->canvas_height = animation.canvas_height;
    out->cache_generation = cache_generation;
    out->state = success_state;
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
store_decoded_response(CdnMediaPipeline* pipeline, const MediaAssetKey& key, ConstSlice<u8> body,
                       const CdnResponseResult& response) noexcept {
    return encoded_media_cache_store(
        pipeline->cache, key, body,
        EncodedMediaCachePolicy{response.etag, response.expires_at_seconds, response.store_allowed,
                                response.must_revalidate});
}

__declspec(noinline) void finish_not_modified(CdnMediaPipeline* pipeline, const MediaAssetKey& key,
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

bool usable(const CdnMediaPipeline* pipeline) noexcept {
    return pipeline && pipeline->cache && pipeline->scanline_storage;
}

// A failed decode evicts the cache entry unless the result is the one state
// (a codec workspace the caller can still bind) that says the entry is fine.
template <typename State> inline constexpr State kEraseOnFailure = static_cast<State>(0xFF);

// Decodes a fresh cached entry; a stale one answers with its ETag instead.
template <typename Result, typename Decode>
void lookup_cached(Result* out, CdnMediaPipeline* pipeline, const MediaAssetKey& key, u64 now_seconds, bool ready,
                   decltype(Result::state) keep, Decode decode) noexcept {
    using State = decltype(Result::state);
    reset(out);
    if (!ready)
        return set_state(out, State::invalid);
    CacheLeaseScope cache_lease(pipeline->cache, key, now_seconds);
    const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
    if (cached.state != EncodedMediaCacheLookupState::fresh)
        return unfresh_lookup(pipeline, cached, out);
    decode(cached);
    if (!out->decoded()) {
        if (out->state != keep) {
            cache_lease.release();
            encoded_media_cache_erase(pipeline->cache, key);
        }
        return;
    }
    out->etag = copy_result_etag(pipeline, cached.etag);
}

// Decodes one frame of the cache entry whose generation the caller holds.
template <typename Result, typename Decode>
void decode_cached_frame(Result* out, CdnMediaPipeline* pipeline, const MediaAssetKey& key, u16 cache_generation,
                         u64 now_seconds, bool ready, decltype(Result::state) keep, Decode decode) noexcept {
    using State = decltype(Result::state);
    reset(out);
    if (!ready || cache_generation == 0)
        return set_state(out, State::invalid);
    CacheLeaseScope cache_lease(pipeline->cache, key, now_seconds);
    const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
    if (!cached.found())
        return set_state(out, State::cache_miss);
    if (cached.generation != cache_generation)
        return set_state(out, State::generation_mismatch);
    decode(cached);
    if (!out->decoded() && out->state != keep) {
        cache_lease.release();
        encoded_media_cache_erase(pipeline->cache, key);
    }
}

// Admits a validated HTTP response: a 200 body is decoded before it is cached, a 304 must
// correlate to a cached entry. decode(bytes, state, cached) fills *out (cached is null for a body);
// forget_retained drops codec state tied to an entry a no-store 304 just retired.
template <typename Result, typename Decode, typename ForgetRetained>
void admit_response(Result* out, CdnMediaPipeline* pipeline, const MediaAssetKey& key,
                    const CdnResponseResult& response, ConstSlice<u8> body, ConstSlice<u8> conditional_etag,
                    bool ready, decltype(Result::state) keep, Decode decode, ForgetRetained forget_retained) noexcept {
    using State = decltype(Result::state);
    reset(out);
    if (!ready || !response.ok())
        return set_state(out, response.ok() ? State::invalid : State::http_rejected);
    if (response.disposition == CdnResponseDisposition::accept_body) {
        if (!accepted_body_valid(response, body))
            return set_state(out, State::invalid);
        decode(body, response.store_allowed ? State::decoded_response : State::decoded_transient, nullptr);
        if (!out->decoded())
            return;
        out->store = store_decoded_response(pipeline, key, body, response);
        // Cache pressure never invalidates already decoded pixels.
        const bool stored = out->store.stored();
        if (!stored)
            out->state = State::decoded_transient;
        if constexpr (requires { out->cache_generation; }) {
            out->cache_generation = 0;
            if (stored)
                out->cache_generation = out->store.generation;
        }
        return;
    }
    if (response.disposition == CdnResponseDisposition::not_modified) {
        if (!not_modified_valid(response, body, conditional_etag))
            return set_state(out, State::invalid);
        CacheLeaseScope cache_lease(pipeline->cache, key, response.expires_at_seconds);
        const EncodedMediaCacheLookup& cached = cache_lease.lease.lookup;
        if (!etag_correlated(cached, conditional_etag))
            return set_state(out, State::uncorrelated_not_modified);
        decode(cached.bytes, response.store_allowed ? State::decoded_cache_hit : State::decoded_transient, &cached);
        if (!out->decoded()) {
            if (out->state != keep) {
                cache_lease.release();
                encoded_media_cache_erase(pipeline->cache, key);
            }
            return;
        }
        if (!response.store_allowed) {
            if constexpr (requires { out->cache_generation; })
                out->cache_generation = 0;
            forget_retained();
        }
        finish_not_modified(pipeline, key, &cache_lease, response);
        return;
    }
    return set_state(out, State::http_rejected);
}

} // namespace

bool cdn_media_pipeline_initialize(CdnMediaPipeline* pipeline, EncodedMediaCache* cache,
                                   Slice<u8> scanline_storage) noexcept {
    if (!pipeline || !cache || !cache->entries || !cache->storage || !scanline_storage.data ||
        scanline_storage.count == 0) {
        return false;
    }
    reset_value<CdnMediaPipeline>(pipeline);
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

void cdn_media_pipeline_lookup_static_png(CdnStaticPngResult* out, CdnMediaPipeline* pipeline,
                                          const MediaAssetKey& key, u64 now_seconds) noexcept {
    lookup_cached(out, pipeline, key, now_seconds,
                  usable(pipeline) && static_image_key_valid(key, MediaAttachmentFormat::png),
                  kEraseOnFailure<CdnStaticPngState>, [&](const EncodedMediaCacheLookup& cached) {
                      decode_bytes(pipeline, cached.bytes, CdnStaticPngState::decoded_cache_hit, out);
                  });
}

CdnStaticPngResult cdn_media_pipeline_lookup_static_png(CdnMediaPipeline* pipeline, const MediaAssetKey& key,
                                                        u64 now_seconds) noexcept {
    CdnStaticPngResult result;
    cdn_media_pipeline_lookup_static_png(&result, pipeline, key, now_seconds);
    return result;
}

void cdn_media_pipeline_admit_static_png(CdnStaticPngResult* out, CdnMediaPipeline* pipeline,
                                         const MediaAssetKey& key, const CdnResponseResult& response,
                                         ConstSlice<u8> decoded_body,
                                         ConstSlice<u8> conditional_etag) noexcept {
    admit_response(
        out, pipeline, key, response, decoded_body, conditional_etag,
        usable(pipeline) && static_image_key_valid(key, MediaAttachmentFormat::png),
        kEraseOnFailure<CdnStaticPngState>,
        [&](ConstSlice<u8> bytes, CdnStaticPngState state, const EncodedMediaCacheLookup*) {
            decode_bytes(pipeline, bytes, state, out);
        },
        [] {});
}

CdnStaticPngResult cdn_media_pipeline_admit_static_png(CdnMediaPipeline* pipeline, const MediaAssetKey& key,
                                                       const CdnResponseResult& response,
                                                       ConstSlice<u8> decoded_body,
                                                       ConstSlice<u8> conditional_etag) noexcept {
    CdnStaticPngResult result;
    cdn_media_pipeline_admit_static_png(&result, pipeline, key, response, decoded_body, conditional_etag);
    return result;
}

void cdn_media_pipeline_lookup_static_webp(CdnStaticWebpResult* out, CdnMediaPipeline* pipeline,
                                           const MediaAssetKey& key, u64 now_seconds) noexcept {
    lookup_cached(
        out, pipeline, key, now_seconds,
        usable(pipeline) && pipeline->webp_workspace && static_image_key_valid(key, MediaAttachmentFormat::webp),
        CdnStaticWebpState::lossy_workspace_required, [&](const EncodedMediaCacheLookup& cached) {
            decode_webp_bytes(pipeline, cached.bytes, CdnStaticWebpState::decoded_cache_hit, out);
        });
}

void cdn_media_pipeline_admit_static_webp(CdnStaticWebpResult* out, CdnMediaPipeline* pipeline,
                                          const MediaAssetKey& key, const CdnResponseResult& response,
                                          ConstSlice<u8> decoded_body,
                                          ConstSlice<u8> conditional_etag) noexcept {
    admit_response(
        out, pipeline, key, response, decoded_body, conditional_etag,
        usable(pipeline) && pipeline->webp_workspace && static_image_key_valid(key, MediaAttachmentFormat::webp),
        kEraseOnFailure<CdnStaticWebpState>,
        [&](ConstSlice<u8> bytes, CdnStaticWebpState state, const EncodedMediaCacheLookup*) {
            decode_webp_bytes(pipeline, bytes, state, out);
        },
        [] {});
}

void cdn_media_pipeline_lookup_static_jpeg(CdnStaticJpegResult* out, CdnMediaPipeline* pipeline,
                                           const MediaAssetKey& key, u64 now_seconds) noexcept {
    lookup_cached(out, pipeline, key, now_seconds,
                  usable(pipeline) && static_image_key_valid(key, MediaAttachmentFormat::jpeg),
                  CdnStaticJpegState::jpeg_workspace_required, [&](const EncodedMediaCacheLookup& cached) {
                      decode_jpeg_bytes(pipeline, key, cached.bytes, CdnStaticJpegState::decoded_cache_hit, out);
                  });
}

void cdn_media_pipeline_admit_static_jpeg(CdnStaticJpegResult* out, CdnMediaPipeline* pipeline,
                                          const MediaAssetKey& key, const CdnResponseResult& response,
                                          ConstSlice<u8> decoded_body,
                                          ConstSlice<u8> conditional_etag) noexcept {
    admit_response(
        out, pipeline, key, response, decoded_body, conditional_etag,
        usable(pipeline) && static_image_key_valid(key, MediaAttachmentFormat::jpeg),
        CdnStaticJpegState::jpeg_workspace_required,
        [&](ConstSlice<u8> bytes, CdnStaticJpegState state, const EncodedMediaCacheLookup*) {
            decode_jpeg_bytes(pipeline, key, bytes, state, out);
        },
        [] {});
}

void cdn_media_pipeline_lookup_apng(CdnApngResult* out, CdnMediaPipeline* pipeline, const MediaAssetKey& key,
                                    u64 now_seconds) noexcept {
    lookup_cached(out, pipeline, key, now_seconds, usable(pipeline) && pipeline->apng_workspace && apng_key_valid(key),
                  kEraseOnFailure<CdnApngState>, [&](const EncodedMediaCacheLookup& cached) {
                      decode_apng_bytes(pipeline, cached.bytes, 0, cached.generation,
                                        CdnApngState::decoded_cache_hit, false, cached.content_identity, out);
                  });
}

void cdn_media_pipeline_admit_apng(CdnApngResult* out, CdnMediaPipeline* pipeline, const MediaAssetKey& key,
                                   const CdnResponseResult& response, ConstSlice<u8> decoded_body,
                                   ConstSlice<u8> conditional_etag) noexcept {
    admit_response(
        out, pipeline, key, response, decoded_body, conditional_etag,
        usable(pipeline) && pipeline->apng_workspace && apng_key_valid(key), kEraseOnFailure<CdnApngState>,
        [&](ConstSlice<u8> bytes, CdnApngState state, const EncodedMediaCacheLookup* cached) {
            decode_apng_bytes(pipeline, bytes, 0, cached ? cached->generation : u16{0}, state, !cached,
                              cached ? cached->content_identity : 0, out);
        },
        [&] { pipeline->apng_metadata_valid = false; });
}

void cdn_media_pipeline_decode_apng_frame(CdnApngResult* out, CdnMediaPipeline* pipeline, const MediaAssetKey& key,
                                          u16 cache_generation, u32 frame_index, u64 now_seconds) noexcept {
    decode_cached_frame(out, pipeline, key, cache_generation, now_seconds,
                        usable(pipeline) && pipeline->apng_workspace && apng_key_valid(key),
                        kEraseOnFailure<CdnApngState>, [&](const EncodedMediaCacheLookup& cached) {
                            decode_apng_bytes(pipeline, cached.bytes, frame_index, cached.generation,
                                              CdnApngState::decoded_cache_hit, false, cached.content_identity, out);
                        });
}

void cdn_media_pipeline_lookup_gif(CdnGifResult* out, CdnMediaPipeline* pipeline, const MediaAssetKey& key,
                                   u64 now_seconds) noexcept {
    lookup_cached(out, pipeline, key, now_seconds, usable(pipeline) && pipeline->gif_workspace && gif_key_valid(key),
                  kEraseOnFailure<CdnGifState>, [&](const EncodedMediaCacheLookup& cached) {
                      decode_gif_bytes(pipeline, cached.bytes, 0, cached.generation, CdnGifState::decoded_cache_hit,
                                       cached.content_identity, out);
                  });
}

void cdn_media_pipeline_admit_gif(CdnGifResult* out, CdnMediaPipeline* pipeline, const MediaAssetKey& key,
                                  const CdnResponseResult& response, ConstSlice<u8> decoded_body,
                                  ConstSlice<u8> conditional_etag) noexcept {
    admit_response(
        out, pipeline, key, response, decoded_body, conditional_etag,
        usable(pipeline) && pipeline->gif_workspace && gif_key_valid(key), kEraseOnFailure<CdnGifState>,
        [&](ConstSlice<u8> bytes, CdnGifState state, const EncodedMediaCacheLookup* cached) {
            decode_gif_bytes(pipeline, bytes, 0, cached ? cached->generation : u16{0}, state,
                             cached ? cached->content_identity : 0, out);
        },
        [&] { pipeline->gif_content_identity = 0; });
}

void cdn_media_pipeline_decode_gif_frame(CdnGifResult* out, CdnMediaPipeline* pipeline, const MediaAssetKey& key,
                                         u16 cache_generation, u32 frame_index, u64 now_seconds) noexcept {
    decode_cached_frame(out, pipeline, key, cache_generation, now_seconds,
                        usable(pipeline) && pipeline->gif_workspace && gif_key_valid(key),
                        kEraseOnFailure<CdnGifState>, [&](const EncodedMediaCacheLookup& cached) {
                            decode_gif_bytes(pipeline, cached.bytes, frame_index, cached.generation,
                                             CdnGifState::decoded_cache_hit, cached.content_identity, out);
                        });
}

void cdn_media_pipeline_lookup_animated_webp(CdnAnimatedWebpResult* out, CdnMediaPipeline* pipeline,
                                             const MediaAssetKey& key, u64 now_seconds) noexcept {
    lookup_cached(
        out, pipeline, key, now_seconds,
        usable(pipeline) && pipeline->webp_workspace && pipeline->animated_webp_workspace &&
            animated_webp_key_valid(key),
        CdnAnimatedWebpState::lossy_workspace_required, [&](const EncodedMediaCacheLookup& cached) {
            decode_animated_webp_bytes(pipeline, cached.bytes, 0, cached.generation,
                                       CdnAnimatedWebpState::decoded_cache_hit, cached.content_identity, out);
        });
}

void cdn_media_pipeline_admit_animated_webp(CdnAnimatedWebpResult* out, CdnMediaPipeline* pipeline,
                                            const MediaAssetKey& key, const CdnResponseResult& response,
                                            ConstSlice<u8> decoded_body,
                                            ConstSlice<u8> conditional_etag) noexcept {
    admit_response(
        out, pipeline, key, response, decoded_body, conditional_etag,
        usable(pipeline) && pipeline->webp_workspace && pipeline->animated_webp_workspace &&
            animated_webp_key_valid(key),
        CdnAnimatedWebpState::lossy_workspace_required,
        [&](ConstSlice<u8> bytes, CdnAnimatedWebpState state, const EncodedMediaCacheLookup* cached) {
            decode_animated_webp_bytes(pipeline, bytes, 0, cached ? cached->generation : u16{0}, state,
                                       cached ? cached->content_identity : 0, out);
        },
        [&] { pipeline->animated_webp_workspace->retained.content_identity = 0; });
}

void cdn_media_pipeline_decode_animated_webp_frame(CdnAnimatedWebpResult* out, CdnMediaPipeline* pipeline,
                                                   const MediaAssetKey& key, u16 cache_generation, u32 frame_index,
                                                   u64 now_seconds) noexcept {
    decode_cached_frame(
        out, pipeline, key, cache_generation, now_seconds,
        usable(pipeline) && pipeline->webp_workspace && pipeline->animated_webp_workspace &&
            animated_webp_key_valid(key),
        CdnAnimatedWebpState::lossy_workspace_required, [&](const EncodedMediaCacheLookup& cached) {
            decode_animated_webp_bytes(pipeline, cached.bytes, frame_index, cached.generation,
                                       CdnAnimatedWebpState::decoded_cache_hit, cached.content_identity, out);
        });
}

namespace {
bool audio_key_valid(const MediaAssetKey& key) noexcept {
    return media_asset_key_valid(key) &&
           media_attachment_format_audio_supported(media_asset_audio_format(key));
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
CdnImageType audio_http_type(const MediaAssetKey& key) noexcept {
    const auto format = media_asset_audio_format(key);
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
        if (error == EncodedAudioError::none) {
            metadata->opus = source;
            metadata->format = MediaAttachmentFormat::ogg_opus;
        }
        return error;
    }
    case MediaAttachmentFormat::mp3: {
        Mp3Stream source{};
        const auto error = audio_error(mp3_stream_initialize(&source, bytes, cancelled, context));
        if (error == EncodedAudioError::none) {
            metadata->mp3 = source;
            metadata->format = MediaAttachmentFormat::mp3;
        }
        return error;
    }
    case MediaAttachmentFormat::wav: {
        WavePcmSource source{};
        const auto error = audio_error(wave_pcm_parse(&source, bytes));
        if (cancelled && cancelled(context))
            return EncodedAudioError::cancelled;
        if (error == EncodedAudioError::none) {
            metadata->wave = source;
            metadata->format = MediaAttachmentFormat::wav;
        }
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
        if (error == EncodedAudioError::none) {
            metadata->m4a = source;
            metadata->format = MediaAttachmentFormat::m4a;
        }
        return error;
    }
    default:
        return EncodedAudioError::unsupported;
    }
}
} // namespace

void cdn_media_pipeline_lookup_audio(CdnEncodedAudioResult* out, CdnMediaPipeline* pipeline,
                                     const MediaAssetKey& key, u64 now_seconds, OggOpusCancelled cancelled,
                                     void* context) noexcept {
    reset_value(out);
    if (!pipeline || !pipeline->cache || !audio_key_valid(key)) {
        out->error = EncodedAudioError::invalid_input;
        return;
    }
    out->lease = encoded_media_cache_acquire(pipeline->cache, key, now_seconds);
    if (!out->lease.acquired) {
        out->state = CdnEncodedAudioState::cache_miss;
        return;
    }
    if (out->lease.lookup.state == EncodedMediaCacheLookupState::stale) {
        out->etag = copy_result_etag(pipeline, out->lease.lookup.etag);
        encoded_media_cache_release(pipeline->cache, &out->lease);
        out->state = CdnEncodedAudioState::fetch_required;
        return;
    }
    out->error = parse_audio(&out->metadata, media_asset_audio_format(key), out->lease.lookup.bytes,
                             cancelled, context);
    if (out->error != EncodedAudioError::none) {
        if (out->error != EncodedAudioError::cancelled)
            encoded_media_cache_retire(pipeline->cache, key, out->lease.generation);
        encoded_media_cache_release(pipeline->cache, &out->lease);
        out->state = CdnEncodedAudioState::decode_rejected;
        return;
    }
    out->state = CdnEncodedAudioState::ready_cache_hit;
}

CdnEncodedAudioResult cdn_media_pipeline_lookup_audio(CdnMediaPipeline* pipeline, const MediaAssetKey& key,
                                                      u64 now_seconds, OggOpusCancelled cancelled,
                                                      void* context) noexcept {
    CdnEncodedAudioResult result;
    cdn_media_pipeline_lookup_audio(&result, pipeline, key, now_seconds, cancelled, context);
    return result;
}

void cdn_media_pipeline_admit_audio(CdnEncodedAudioResult* out, CdnMediaPipeline* pipeline,
                                    const MediaAssetKey& key, const CdnResponseResult& response,
                                    ConstSlice<u8> body, ConstSlice<u8> conditional_etag,
                                    OggOpusCancelled cancelled, void* context) noexcept {
    reset_value(out);
    if (!pipeline || !pipeline->cache || !audio_key_valid(key)) {
        out->error = EncodedAudioError::invalid_input;
        return;
    }
    if (!response.ok() ||
        (response.disposition != CdnResponseDisposition::accept_body &&
         response.disposition != CdnResponseDisposition::not_modified) ||
        response.body_bytes != body.count ||
        body.count > cdn_encoded_body_limit(audio_http_type(key))) {
        out->state = CdnEncodedAudioState::http_rejected;
        out->error = EncodedAudioError::invalid_input;
        return;
    }
    if (response.disposition == CdnResponseDisposition::not_modified) {
        out->lease = encoded_media_cache_acquire(pipeline->cache, key, 0);
        if (body.count != 0 || !conditional_etag.data ||
            !etag_correlated(out->lease.lookup, conditional_etag)) {
            if (out->lease.acquired)
                encoded_media_cache_release(pipeline->cache, &out->lease);
            out->state = CdnEncodedAudioState::uncorrelated_not_modified;
            return;
        }
        if (!response.store_allowed)
            encoded_media_cache_retire(pipeline->cache, key, out->lease.generation);
        out->error = parse_audio(&out->metadata, media_asset_audio_format(key), out->lease.lookup.bytes,
                                 cancelled, context);
        if (out->error == EncodedAudioError::none) {
            if (response.store_allowed) {
                const u64 content_identity = out->lease.lookup.content_identity;
                const u16 generation = out->lease.generation;
                encoded_media_cache_release(pipeline->cache, &out->lease);
                encoded_media_cache_refresh(pipeline->cache, key, response.expires_at_seconds,
                                            response.must_revalidate, response.etag, generation);
                out->lease = encoded_media_cache_acquire(pipeline->cache, key, 0);
                if (!out->lease.acquired || out->lease.generation != generation ||
                    out->lease.lookup.content_identity != content_identity) {
                    if (out->lease.acquired)
                        encoded_media_cache_release(pipeline->cache, &out->lease);
                    reset_value(&out->metadata);
                    out->state = CdnEncodedAudioState::uncorrelated_not_modified;
                    return;
                }
                if (!encoded_audio_rebind(&out->metadata, out->lease.lookup.bytes)) {
                    encoded_media_cache_release(pipeline->cache, &out->lease);
                    reset_value(&out->metadata);
                    out->state = CdnEncodedAudioState::uncorrelated_not_modified;
                    return;
                }
            }
            out->state = CdnEncodedAudioState::ready_response;
            // A no-store 304 must not extend reusable cache ownership. Its
            // existing lease keeps bytes alive through joined playback release.
            if (!response.store_allowed)
                out->state = CdnEncodedAudioState::ready_transient;
            return;
        }
    } else {
        if (!response.store_allowed)
            encoded_media_cache_retire(pipeline->cache, key);
        out->error = parse_audio(&out->metadata, media_asset_audio_format(key), body, cancelled, context);
        if (out->error == EncodedAudioError::none) {
            const auto stored =
                encoded_media_cache_store(pipeline->cache, key, body,
                                          {response.etag, response.expires_at_seconds,
                                           response.store_allowed, response.must_revalidate});
            if (stored.stored()) {
                out->lease = encoded_media_cache_acquire(pipeline->cache, key, 0);
                if (out->lease.acquired && out->lease.generation == stored.generation &&
                    encoded_audio_rebind(&out->metadata, out->lease.lookup.bytes)) {
                    out->state = CdnEncodedAudioState::ready_response;
                    return;
                }
                if (out->lease.acquired)
                    encoded_media_cache_release(pipeline->cache, &out->lease);
            }
            out->state = CdnEncodedAudioState::ready_transient;
            return;
        }
    }
    if (out->lease.acquired) {
        if (out->error != EncodedAudioError::cancelled)
            encoded_media_cache_retire(pipeline->cache, key, out->lease.generation);
        encoded_media_cache_release(pipeline->cache, &out->lease);
    }
    out->state = CdnEncodedAudioState::decode_rejected;
}

CdnEncodedAudioResult cdn_media_pipeline_admit_audio(CdnMediaPipeline* pipeline, const MediaAssetKey& key,
                                                     const CdnResponseResult& response, ConstSlice<u8> body,
                                                     ConstSlice<u8> conditional_etag,
                                                     OggOpusCancelled cancelled, void* context) noexcept {
    CdnEncodedAudioResult result;
    cdn_media_pipeline_admit_audio(&result, pipeline, key, response, body, conditional_etag, cancelled,
                                   context);
    return result;
}

} // namespace maicord
