#include "account_media_internal.hpp"
#include "maicord/app/media/local_preview_thumbnail.hpp"
#include "maicord/app/media/local_video_poster.hpp"
#include "maicord/media/image/png_stream.hpp"
#include "maicord/media/image/webp_thumbnail.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

static_assert(kLocalDraftPreviewThumbnailJpegBytes == kJpegThumbnailEncodedMaximumBytes);
// Video posters are reduced on the GPU to the same extent as image thumbnails.
static_assert(kGpuVideoPosterExtent == kLocalPreviewThumbnailExtent);
static_assert(kLocalDraftPreviewThumbnailWebpBytes == kWebpThumbnailEncodedMaximumBytes);
struct ThumbnailMemory {
    u8* base = nullptr;
    ~ThumbnailMemory() noexcept {
        if (base)
            VirtualFree(base, 0, MEM_RELEASE);
    }
    u8* allocate(u64 bytes) noexcept {
        if (!bytes || bytes > 0x7fffffffull)
            return nullptr;
        base = static_cast<u8*>(commit_pages(static_cast<SIZE_T>(bytes)));
        return base;
    }
};
// Output, orientation copy, column sums and one expanded source row.
struct ThumbnailTarget {
    ThumbnailMemory memory;
    Slice<u8> output, rotate, row;
    Slice<u32> sums;
    bool allocate(u32 source_width) noexcept {
        constexpr u32 image = kLocalPreviewThumbnailExtent * kLocalPreviewThumbnailExtent * 4u;
        constexpr u32 sum_bytes = (kLocalPreviewThumbnailExtent + 1u) * 5u * sizeof(u32);
        const u64 row_bytes = static_cast<u64>(source_width) * 4u;
        u8* base = memory.allocate(image * 2ull + sum_bytes + row_bytes);
        if (!base)
            return false;
        output = {base, image};
        rotate = {base + image, image};
        sums = {reinterpret_cast<u32*>(base + image * 2u), sum_bytes / sizeof(u32)};
        row = {base + image * 2u + sum_bytes, static_cast<u32>(row_bytes)};
        return true;
    }
};
AccountMediaRequestResult publish_local_thumbnail(AccountMedia* media, const MediaAssetKey& key, const u8* rgba, u32 width,
                                                  u32 height) noexcept {
    const u32 bytes = width * height * 4u;
    if (!width || !height || bytes > media->pipeline.scanline_capacity || !media->pipeline.scanline_storage)
        return result(AccountMediaRequestState::failed, AccountMediaError::memory);
    copy_bytes(media->pipeline.scanline_storage, rgba, bytes);
    reset_value<WebpImage>(&media->frame_webp_image);
    media->frame_webp_image.width = width;
    media->frame_webp_image.height = height;
    media->frame_key = key;
    media->frame_bytes = bytes;
    media->frame_request_id = 0;
    media->frame_source_message_id = key.id;
    media->frame_source_message_revision = key.variant;
    capture_catalog_source(media, key);
    media->frame_kind = AccountMediaFrameKind::straight_rgba8;
    mark_frame_ready(media, key);
    media->frame_cache_generation = 0;
    media->frame_count = 1;
    media->frame_index = 0;
    media->frame_duration_us = 0;
    media->frame_play_count = 1;
    return result(AccountMediaRequestState::ready);
}
enum class ThumbnailStep : u8 { decoded, legacy, memory, decode };

// Streams the decoded rows of `encoded` into `sink` (the box filter reduces them as they arrive)
// and reports the source orientation. Legacy sends the request to the ordinary GPU preview path.
ThumbnailStep decode_thumbnail_rows(AccountMedia* media, MediaAttachmentFormat format, const ConstSlice<u8>& encoded,
                                    LocalThumbnailSink* sink, ThumbnailTarget* target, u8* orientation) noexcept {
    auto& pipeline = media->pipeline;
    if (format == MediaAttachmentFormat::png) {
        // Every PNG thumbnail streams: rows are inflated through a sliding
        // window, reconstructed against the previous row and box-filtered at
        // once, so dimensions past the 2048 px GPU path need no scanline copy.
        PngImage image{};
        PngStreamLayout layout{};
        const auto parsed = png_stream_parse(encoded, &image, &layout);
        // Adam7 is decoded whole by the GPU preview path (<= 2048 px).
        if (parsed.error == PngError::unsupported_interlace)
            return ThumbnailStep::legacy;
        if (!parsed.ok())
            return ThumbnailStep::decode;
        // The deflate workspace leads the block, then spans, window and rows.
        const u64 lead = (sizeof(DeflateWorkspace) + 15u) & ~15ull;
        const u64 span_bytes = (static_cast<u64>(layout.idat_chunks) * sizeof(DeflateInputSpan) + 15u) & ~15ull;
        ThumbnailMemory stream;
        u8* block = stream.allocate(lead + span_bytes + kPngStreamWindowBytes + layout.row_buffer_bytes);
        u8* base = block ? block + lead : nullptr;
        if (!base || !target->allocate(image.width) ||
            !local_thumbnail_begin(sink, image.width, image.height, kLocalPreviewThumbnailExtent, target->output,
                                   target->sums))
            return ThumbnailStep::memory;
        struct Rows {
            const PngImage* image;
            LocalThumbnailSink* sink;
            u8* rgba;
        } rows{&image, sink, target->row.data};
        const auto decoded = png_stream_decode(
            encoded, image, layout, {reinterpret_cast<DeflateInputSpan*>(base), layout.idat_chunks},
            {base + span_bytes, kPngStreamWindowBytes}, {base + span_bytes + kPngStreamWindowBytes, layout.row_buffer_bytes},
            [](void* context, u32, const u8* row) noexcept {
                auto& r = *static_cast<Rows*>(context);
                // Reconstructed RGBA8 already has the sink's byte format.
                // The sink consumes the row before the stream reuses it.
                if (r.image->color_type == 6 && r.image->bit_depth == 8)
                    return local_thumbnail_push_row(r.sink, row);
                return local_thumbnail_png_row(*r.image, row, r.rgba) && local_thumbnail_push_row(r.sink, r.rgba);
            },
            &rows, reinterpret_cast<DeflateWorkspace*>(block));
        return decoded.ok() ? ThumbnailStep::decoded : ThumbnailStep::decode;
    }
    if (format == MediaAttachmentFormat::webp) {
        WebpThumbnailLayout layout{};
        if (webp_thumbnail_parse(encoded, &layout) != WebpThumbnailError::none || layout.alpha_chunk.data)
            return ThumbnailStep::decode;
        ThumbnailMemory scratch;
        u8* base = scratch.allocate(layout.workspace_bytes);
        if (!base || !target->allocate(layout.width) ||
            !local_thumbnail_begin(sink, layout.width, layout.height, kLocalPreviewThumbnailExtent, target->output,
                                   target->sums))
            return ThumbnailStep::memory;
        const auto decoded = webp_thumbnail_decode(
            layout, {base, layout.workspace_bytes},
            [](void* context, u32, const u8* rgba) noexcept {
                return local_thumbnail_push_row(static_cast<LocalThumbnailSink*>(context), rgba);
            },
            sink);
        return decoded == WebpThumbnailError::none ? ThumbnailStep::decoded : ThumbnailStep::decode;
    }
    if (format == MediaAttachmentFormat::gif) {
        if (!ensure_gif_workspace(media))
            return ThumbnailStep::memory;
        GifImage animation{};
        pipeline.gif_content_identity = 0;
        const auto parsed = gif_parse(encoded, &animation, media->gif_workspace);
        if (!parsed.ok() || !animation.frame_count)
            return ThumbnailStep::decode;
        // The first frame, like the official static preview; the strip adds a GIF tag.
        GifDecodeResult decoded;
        gif_decode_parsed_frame(&decoded, encoded, animation, 0, media->gif_workspace,
                                {pipeline.scanline_storage, pipeline.scanline_capacity});
        if (!decoded.ok() || !target->allocate(animation.canvas_width) ||
            !local_thumbnail_begin(sink, animation.canvas_width, animation.canvas_height,
                                   kLocalPreviewThumbnailExtent, target->output, target->sums))
            return decoded.ok() ? ThumbnailStep::memory : ThumbnailStep::decode;
        const ConstSlice<u8> indices{pipeline.scanline_storage, decoded.pixels};
        for (u32 y = 0; y < animation.canvas_height; ++y)
            if (!local_thumbnail_gif_row(decoded.frame, decoded.palette, indices, animation.canvas_width, y,
                                         target->row.data) ||
                !local_thumbnail_push_row(sink, target->row.data))
                return ThumbnailStep::decode;
        return ThumbnailStep::decoded;
    }
    if (format == MediaAttachmentFormat::jpeg) {
        if (!ensure_jpeg_workspace(media))
            return ThumbnailStep::memory;
        JpegImage image{};
        // Small baseline/progressive files keep the exact GPU IDCT path.
        if (encoded.count <= kJpegEncodedMaximumBytes && jpeg_parse(encoded, &image, media->jpeg_workspace).ok())
            return ThumbnailStep::legacy;
        JpegThumbnailLayout layout{};
        if (!jpeg_thumbnail_parse(encoded, kLocalPreviewThumbnailExtent, &image, media->jpeg_workspace, &layout).ok())
            return ThumbnailStep::decode;
        ThumbnailMemory scratch;
        u8* scratch_base = scratch.allocate(layout.coefficient_bytes + layout.plane_bytes);
        if (!scratch_base || !target->allocate(layout.width) ||
            !local_thumbnail_begin(sink, layout.width, layout.height, kLocalPreviewThumbnailExtent, target->output,
                                   target->sums))
            return ThumbnailStep::memory;
        const auto decoded = jpeg_thumbnail_decode_rows(
            encoded, image, layout, {scratch_base, static_cast<u32>(layout.coefficient_bytes + layout.plane_bytes)},
            target->row,
            [](void* context, u32, const u8* rgba) noexcept {
                return local_thumbnail_push_row(static_cast<LocalThumbnailSink*>(context), rgba);
            },
            sink);
        if (!decoded.ok())
            return ThumbnailStep::decode;
        *orientation = static_cast<u8>(image.orientation);
        return ThumbnailStep::decoded;
    }
    return ThumbnailStep::decode;
}

// Unsupported: the caller continues with the ordinary GPU preview path.
AccountMediaRequestResult decode_local_thumbnail(AccountMedia* media, const AccountMediaLocalPreviewRequest& request,
                                                 const ConstSlice<u8>& encoded) noexcept {
    const auto key = request.key;
    const auto format = media_asset_catalog_format(key);
    const auto legacy = [&]() noexcept {
        return request.snapshot.bytes <= local_draft_preview_encoded_limit(format)
                   ? result(AccountMediaRequestState::unsupported)
                   : result(AccountMediaRequestState::failed, AccountMediaError::decode);
    };
    // Stills the GPU VP8 path admits (<= 512 px, <= 2 MiB) keep it; larger
    // lossy WebPs decode on the CPU one macroblock row at a time.
    const bool large_webp =
        format == MediaAttachmentFormat::webp &&
        (request.snapshot.bytes > local_draft_preview_encoded_limit(format) ||
         (encoded.data && [&]() noexcept {
             WebpImage image{};
             return webp_parse(encoded, &image).error == WebpError::dimensions;
         }()));
    if (format == MediaAttachmentFormat::webp && !large_webp)
        return legacy();
    if (!encoded.data)
        // Small JPEGs may already sit in the encoded cache; everything else
        // asks the lane to read the file.
        return format == MediaAttachmentFormat::jpeg ? legacy()
                                                     : result(AccountMediaRequestState::failed, AccountMediaError::cache);
    media->active_catalog_scope = request.owner_scope;
    media->active_catalog_generation = request.owner_generation;
    media->active_source_message_id = key.id;
    media->active_source_message_revision = key.variant;
    LocalThumbnailSink sink{};
    ThumbnailTarget target{};
    u8 orientation = 1;
    switch (decode_thumbnail_rows(media, format, encoded, &sink, &target, &orientation)) {
    case ThumbnailStep::legacy:
        return legacy();
    case ThumbnailStep::memory:
        return result(AccountMediaRequestState::failed, AccountMediaError::memory);
    case ThumbnailStep::decode:
        return result(AccountMediaRequestState::failed, AccountMediaError::decode);
    case ThumbnailStep::decoded:
        break;
    }
    u32 width = sink.width, height = sink.height;
    if (!local_thumbnail_complete(sink) ||
        !local_thumbnail_orient(target.output, &width, &height, orientation, target.rotate))
        return result(AccountMediaRequestState::failed, AccountMediaError::decode);
    return publish_local_thumbnail(media, key, target.output.data, width, height);
}

} // namespace

AccountMediaRequestResult account_media_decode_local_preview(AccountMedia* media,
                                                             const AccountMediaLocalPreviewRequest& request,
                                                             const ConstSlice<u8>& encoded) noexcept {
    if (!media || !media->started || media->active || media->frame_ready ||
        !local_draft_preview_request_valid(request) ||
        ((encoded.data || encoded.count) && (!encoded.data || encoded.count != request.snapshot.bytes)))
        return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    if (local_draft_preview_thumbnail(request.key)) {
        const auto thumbnail = decode_local_thumbnail(media, request, encoded);
        if (thumbnail.state != AccountMediaRequestState::unsupported)
            return thumbnail;
    }
    const auto key = request.key;
    const auto format = media_asset_catalog_format(key);
    // This cache entry is local typed data, not an HTTP response. The parser
    // enforces each codec's encoded and decoded size limits.
    EncodedMediaCacheStoreResult stored{};
    if (encoded.data)
        stored = encoded_media_cache_store(media->pipeline.cache, key, encoded, {{}, ~0ull, true, false});
    else {
        const auto resident = encoded_media_cache_find(media->pipeline.cache, key, 0);
        if (resident.found() && resident.bytes.count == request.snapshot.bytes)
            stored = {EncodedMediaCacheStoreState::stored, resident.generation};
    }
    if (!stored.stored())
        return result(AccountMediaRequestState::failed, AccountMediaError::cache);
    media->active_catalog_scope = request.owner_scope;
    media->active_catalog_generation = request.owner_generation;
    media->active_source_message_id = key.id;
    media->active_source_message_revision = key.variant;
    AccountMediaRequestResult decoded = result(AccountMediaRequestState::failed, AccountMediaError::decode);
    if (format == MediaAttachmentFormat::png) {
        CdnStaticPngResult value;
        cdn_media_pipeline_lookup_static_png(&value, &media->pipeline, key, 0);
        decoded.png_error = value.decode.error;
        if (value.decoded())
            publish_png_frame(media, key, value, 0, key.id, key.variant);
    } else if (format == MediaAttachmentFormat::jpeg) {
        if (!ensure_jpeg_workspace(media))
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        CdnStaticJpegResult value;
        cdn_media_pipeline_lookup_static_jpeg(&value, &media->pipeline, key, 0);
        decoded.jpeg_error = value.decode.error;
        if (value.decoded())
            publish_jpeg_frame(media, key, value, 0, key.id, key.variant);
    } else if (format == MediaAttachmentFormat::webp) {
        if (!ensure_webp_workspace(media))
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        CdnStaticWebpResult value;
        cdn_media_pipeline_lookup_static_webp(&value, &media->pipeline, key, 0);
        if (value.state == CdnStaticWebpState::lossy_workspace_required &&
            ensure_lossy_webp_workspace(media))
            cdn_media_pipeline_lookup_static_webp(&value, &media->pipeline, key, 0);
        decoded.webp_error = value.decode.error;
        decoded.lossy_webp_error = value.lossy_decode.error;
        if (value.decoded())
            publish_webp_frame(media, key, value, 0, key.id, key.variant);
    } else if (format == MediaAttachmentFormat::gif) {
        if (!ensure_gif_workspace(media))
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        CdnGifResult value;
        cdn_media_pipeline_lookup_gif(&value, &media->pipeline, key, 0);
        decoded.gif_error = value.decode.error;
        if (value.decoded())
            publish_gif_frame(media, key, value, 0);
    }
    if (!media->frame_ready) {
        encoded_media_cache_erase(media->pipeline.cache, key);
        return decoded;
    }
    media->frame_cache_generation = stored.generation;
    media->frame_count = 1;
    media->frame_index = 0;
    media->frame_duration_us = 0;
    media->frame_play_count = 1;
    return result(AccountMediaRequestState::ready);
}

namespace {

AccountMediaRequestResult poster_failure(LocalVideoPosterStep step) noexcept {
    // A file that cannot become a poster stays a file card; only a failed
    // read (cancellation, a busy disk) may be asked for again.
    auto failure = result(step == LocalVideoPosterStep::read_failed ? AccountMediaRequestState::failed
                                                                    : AccountMediaRequestState::unsupported,
                          step == LocalVideoPosterStep::read_failed ? AccountMediaError::invalid_input
                                                                    : AccountMediaError::decode);
    failure.video_failure = step == LocalVideoPosterStep::unsupported ? AccountMediaVideoFailure::demux_unsupported
                            : step == LocalVideoPosterStep::malformed ? AccountMediaVideoFailure::demux_malformed
                            : step == LocalVideoPosterStep::budget    ? AccountMediaVideoFailure::memory
                                                                      : AccountMediaVideoFailure::none;
    if (step != LocalVideoPosterStep::read_failed)
        failure.retry_at_ms = ~0ull;
    return failure;
}

} // namespace

AccountMediaRequestResult account_media_decode_local_video_preview(AccountMedia* media,
                                                                   const AccountMediaLocalPreviewRequest& request,
                                                                   AttachmentProbeRead read,
                                                                   void* context) noexcept {
    if (!media || !media->started || media->active || media->frame_ready || !read ||
        !local_draft_preview_request_valid(request) || !local_draft_preview_thumbnail(request.key) ||
        media_asset_catalog_format(request.key) != MediaAttachmentFormat::mp4 ||
        request.snapshot.bytes > kMediaMaximumProgressiveVideoBytes)
        return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    if (!request.video_device)
        return poster_failure(LocalVideoPosterStep::unsupported);
    account_media_release_video(media);
    media->progressive = allocate_fetch();
    auto* scratch = static_cast<u8*>(commit_pages(kLocalVideoPosterScratchBytes));
    if (!media->progressive || !scratch ||
        !progressive_source_initialize(&media->progressive->source, static_cast<u32>(request.snapshot.bytes))) {
        if (scratch)
            VirtualFree(scratch, 0, MEM_RELEASE);
        account_media_release_video(media);
        return result(AccountMediaRequestState::failed, AccountMediaError::memory);
    }
    auto* source = &media->progressive->source;
    LocalVideoPosterReader reader{read, context, scratch, kLocalVideoPosterReadBudget, 0, 0};
    const auto finish = [&](AccountMediaRequestResult value) noexcept {
        VirtualFree(scratch, 0, MEM_RELEASE);
        if (value.state != AccountMediaRequestState::ready)
            account_media_release_video(media);
        reset_value<MediaAssetKey>(&media->active_key);
        return value;
    };
    auto step = local_video_poster_prepare(source, &reader);
    if (step != LocalVideoPosterStep::ready)
        return finish(poster_failure(step));
    const auto bytes = progressive_source_bytes(*source);
    const auto availability = progressive_source_reader(source, ProgressiveTrack::video);
    media->video_device = request.video_device;
    // Native extent: the renderer reduces the poster to a thumbnail on the GPU.
    if (!mp4_decoder_initialize(ensure_mp4_decoder(media), bytes, 0, 0, request.video_device, availability)) {
        auto failure = video_failure(media->mp4_decoder);
        failure.retry_at_ms = ~0ull;
        if (failure.state == AccountMediaRequestState::failed)
            failure.state = AccountMediaRequestState::unsupported;
        return finish(failure);
    }
    media->active_key = request.key;
    media->active_catalog_scope = request.owner_scope;
    media->active_catalog_generation = request.owner_generation;
    media->active_source_message_id = request.key.id;
    media->active_source_message_revision = request.key.variant;
    adopt_mp4_source(media);
    Mp4DecodedFrame decoded{};
    // Reordered streams may submit a few access units before the first
    // picture; each pending one costs a bounded read from the file.
    for (u32 attempt = 0; attempt < 16; ++attempt) {
        decoded = mp4_decoder_read(media->mp4_decoder, false);
        if (decoded.error != Mp4DecodeError::data_pending)
            break;
        if (!local_video_poster_service(source, &reader, &step))
            return finish(poster_failure(step == LocalVideoPosterStep::ready ? LocalVideoPosterStep::malformed : step));
    }
    if (!decoded.ok() || !decoded.gpu) {
        auto failure = video_failure(media->mp4_decoder);
        failure.retry_at_ms = ~0ull;
        if (failure.state == AccountMediaRequestState::failed)
            failure.state = AccountMediaRequestState::unsupported;
        return finish(failure);
    }
    // The poster never plays and the GPU holds the picture: drop the sample
    // payload, keeping only the pinned box headers and moov.
    progressive_source_trim(source);
    if (!decoded.duration_us)
        decoded.duration_us = 1;
    publish_mp4_frame(media, request.key, decoded, 0, request.key.id, request.key.variant, 0);
    return finish(result(AccountMediaRequestState::ready));
}

bool account_media_local_video_resident(const AccountMedia* media) noexcept {
    return media && media->mp4_key.kind == MediaAssetKind::local_draft_preview &&
           ((media->mp4_decoder && media->mp4_decoder->ready) || media->progressive);
}

} // namespace maicord
