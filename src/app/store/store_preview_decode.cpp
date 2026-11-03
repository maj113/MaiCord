#include "store_preview.hpp"

#include "maicord/app/media/local_preview_thumbnail.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/media/image/png_stream.hpp"

namespace maicord::app_detail {
namespace {

struct Pages {
    u8* base = nullptr;
    ~Pages() noexcept {
        if (base)
            VirtualFree(base, 0, MEM_RELEASE);
    }
    u8* allocate(u64 bytes) noexcept {
        base = bytes && bytes < 0x7fffffffull
                   ? static_cast<u8*>(commit_pages(static_cast<SIZE_T>(bytes)))
                   : nullptr;
        return base;
    }
    u8* release() noexcept {
        u8* result = base;
        base = nullptr;
        return result;
    }
};

constexpr u64 round16(u64 bytes) noexcept {
    return (bytes + 15u) & ~15ull;
}

struct Rows {
    const PngImage* image;
    LocalThumbnailSink* sink;
    u8* rgba;
};

} // namespace

u32 store_preview_pixels(float width_dip, u32 dpi) noexcept {
    if (!(width_dip > 0))
        return 0;
    const u32 pixels = static_cast<u32>(width_dip * static_cast<float>(dpi ? dpi : 96) / 96.f + .5f);
    const u32 rounded = (pixels + 31u) & ~31u;
    return rounded > 1920u ? 1920u : rounded;
}

MediaAssetKey store_preview_key(const u8 (&sha256)[32], u32 pixels, bool loop) noexcept {
    u64 id = 0;
    for (u32 i = 0; i < 8; ++i)
        id = (id << 8) | sha256[i];
    return media_asset_local_draft_preview(id | 1, (loop ? 0x40000000u : 0x20000000u) | pixels, MediaAttachmentFormat::png);
}

bool store_preview_decode(ConstSlice<u8> png, u32 pixels, StorePreviewImage* out) noexcept {
    *out = {};
    PngImage image{};
    PngStreamLayout layout{};
    if (!pixels || !png_stream_parse(png, &image, &layout).ok())
        return false;
    // The largest integer reduction that leaves the picture at least `pixels` across.
    const u32 scale = image.width / pixels ? image.width / pixels : 1;
    const u32 longest = image.width > image.height ? image.width : image.height;
    const u32 extent = (longest + scale - 1) / scale;
    const u32 reduction = local_thumbnail_scale(image.width, image.height, extent);
    const u32 width = local_thumbnail_extent(image.width, reduction), height = local_thumbnail_extent(image.height, reduction);
    Pages output;
    if (!output.allocate(static_cast<u64>(width) * height * 4u))
        return false;
    const u64 lead = round16(sizeof(DeflateWorkspace)), spans = round16(static_cast<u64>(layout.idat_chunks) * sizeof(DeflateInputSpan)),
              sums = round16(static_cast<u64>(width) * 5u * sizeof(u32)), row = round16(static_cast<u64>(image.width) * 4u);
    Pages scratch;
    u8* block = scratch.allocate(lead + spans + kPngStreamWindowBytes + layout.row_buffer_bytes + sums + row);
    if (!block)
        return false;
    u8* at = block + lead;
    auto* span_table = reinterpret_cast<DeflateInputSpan*>(at);
    u8* window = at + spans;
    u8* rows_buffer = window + kPngStreamWindowBytes;
    auto* sum_table = reinterpret_cast<u32*>(rows_buffer + layout.row_buffer_bytes);
    u8* rgba = reinterpret_cast<u8*>(sum_table) + sums;
    LocalThumbnailSink sink{};
    if (!local_thumbnail_begin(&sink, image.width, image.height, extent, {output.base, static_cast<u32>(static_cast<u64>(width) * height * 4u)},
                               {sum_table, width * 5u}))
        return false;
    Rows state{&image, &sink, rgba};
    const auto decoded = png_stream_decode(
        png, image, layout, {span_table, layout.idat_chunks}, {window, kPngStreamWindowBytes}, {rows_buffer, layout.row_buffer_bytes},
        [](void* context, u32, const u8* line) noexcept {
            auto& r = *static_cast<Rows*>(context);
            if (r.image->color_type == 6 && r.image->bit_depth == 8)
                return local_thumbnail_push_row(r.sink, line);
            return local_thumbnail_png_row(*r.image, line, r.rgba) && local_thumbnail_push_row(r.sink, r.rgba);
        },
        &state, reinterpret_cast<DeflateWorkspace*>(block));
    if (!decoded.ok() || !local_thumbnail_complete(sink))
        return false;
    *out = {output.release(), sink.width, sink.height};
    return true;
}

void store_preview_free(StorePreviewImage* image) noexcept {
    if (image->pixels)
        VirtualFree(image->pixels, 0, MEM_RELEASE);
    *image = {};
}

} // namespace maicord::app_detail
