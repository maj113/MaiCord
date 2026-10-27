#include "maicord/app/profile_image.hpp"

#include "maicord/app/local_preview_thumbnail.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/gif.hpp"
#include "maicord/media/jpeg.hpp"
#include "maicord/media/png.hpp"
#include "maicord/media/png_encode.hpp"
#include "maicord/media/png_stream.hpp"
#include "maicord/media/webp.hpp"
#include "maicord/media/webp_alpha.hpp"
#include "maicord/media/webp_thumbnail.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

// One VirtualAlloc block, released on scope exit unless taken.
struct Block {
    u8* data = nullptr;
    u64 bytes = 0;
    Block() noexcept = default;
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    ~Block() noexcept {
        reset();
    }
    u8* allocate(u64 size) noexcept {
        reset();
        if (!size || size > 0x7FFF'FFFFull)
            return nullptr;
        data = static_cast<u8*>(
            VirtualAlloc(nullptr, static_cast<SIZE_T>(size), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        bytes = data ? size : 0;
        return data;
    }
    void reset() noexcept {
        if (data)
            VirtualFree(data, 0, MEM_RELEASE);
        data = nullptr;
        bytes = 0;
    }
    u8* take() noexcept {
        u8* d = data;
        data = nullptr;
        bytes = 0;
        return d;
    }
};

constexpr u64 align16(u64 bytes) noexcept {
    return (bytes + 15u) & ~15ull;
}
u32 tag(const u8* p) noexcept {
    return byte_load_le32(p);
}
constexpr u32 fourcc(char a, char b, char c, char d) noexcept {
    return static_cast<u32>(static_cast<u8>(a)) | static_cast<u32>(static_cast<u8>(b)) << 8 |
           static_cast<u32>(static_cast<u8>(c)) << 16 | static_cast<u32>(static_cast<u8>(d)) << 24;
}

// WebP container.

// The first frame's bitstream chunks and its rectangle on the canvas.
struct WebpParts {
    ConstSlice<u8> alpha, lossy, lossless;
    u32 canvas_width, canvas_height, frames;
    u32 x, y, width, height;
    bool animated;
};

bool bitstream_size(ConstSlice<u8> lossy, ConstSlice<u8> lossless, u32* width, u32* height) noexcept {
    if (lossy.data) {
        if (lossy.count < 10 || lossy[3] != 0x9D || lossy[4] != 0x01 || lossy[5] != 0x2A)
            return false;
        *width = byte_load_le16(lossy.data + 6) & 0x3FFFu;
        *height = byte_load_le16(lossy.data + 8) & 0x3FFFu;
    } else {
        if (lossless.count < 5 || lossless[0] != 0x2F)
            return false;
        const u32 bits = byte_load_le32(lossless.data + 1);
        *width = (bits & 0x3FFFu) + 1;
        *height = (bits >> 14 & 0x3FFFu) + 1;
    }
    return *width && *height;
}

// Image chunks of one chunk list (top level of a still, or an ANMF payload).
void scan_image_chunks(ConstSlice<u8> s, u64 at, u64 end, WebpParts* p) noexcept {
    while (at + 8 <= end) {
        const u32 type = tag(s.data + at), bytes = byte_load_le32(s.data + at + 4);
        if (bytes > end - at - 8)
            return;
        const ConstSlice<u8> payload{s.data + at + 8, bytes};
        if (type == fourcc('A', 'L', 'P', 'H') && !p->alpha.data)
            p->alpha = payload;
        else if (type == fourcc('V', 'P', '8', ' ')) {
            p->lossy = payload;
            return;
        } else if (type == fourcc('V', 'P', '8', 'L')) {
            p->lossless = payload;
            return;
        }
        at += 8ull + bytes + (bytes & 1u);
    }
}

bool webp_parts(ConstSlice<u8> s, WebpParts* p) noexcept {
    reset_value(p);
    if (s.count < 20 || tag(s.data) != fourcc('R', 'I', 'F', 'F') || tag(s.data + 8) != fourcc('W', 'E', 'B', 'P'))
        return false;
    const u64 end = minimum<u64>(8ull + byte_load_le32(s.data + 4), s.count);
    bool extended = false;
    for (u64 at = 12; at + 8 <= end;) {
        const u32 type = tag(s.data + at), bytes = byte_load_le32(s.data + at + 4);
        if (bytes > end - at - 8)
            return false;
        const u8* payload = s.data + at + 8;
        if (type == fourcc('V', 'P', '8', 'X')) {
            if (bytes < 10 || at != 12)
                return false;
            extended = true;
            p->animated = (payload[0] & 0x02u) != 0;
            p->canvas_width = byte_load_le24(payload + 4) + 1;
            p->canvas_height = byte_load_le24(payload + 7) + 1;
        } else if (type == fourcc('A', 'N', 'M', 'F')) {
            if (!p->animated || bytes < 16)
                return false;
            if (++p->frames == 1) {
                p->x = byte_load_le24(payload) * 2;
                p->y = byte_load_le24(payload + 3) * 2;
                p->width = byte_load_le24(payload + 6) + 1;
                p->height = byte_load_le24(payload + 9) + 1;
                scan_image_chunks(s, at + 8 + 16, at + 8 + bytes, p);
            }
        } else if (!p->animated && !p->lossy.data && !p->lossless.data) {
            scan_image_chunks(s, at, at + 8 + bytes, p);
        }
        at += 8ull + bytes + (bytes & 1u);
    }
    if (!p->lossy.data && !p->lossless.data)
        return false;
    u32 width = 0, height = 0;
    if (!bitstream_size(p->lossy, p->lossless, &width, &height))
        return false;
    if (p->animated) {
        if (!p->frames || width != p->width || height != p->height)
            return false;
    } else {
        if (extended && (width != p->canvas_width || height != p->canvas_height))
            return false;
        p->frames = 1;
        p->canvas_width = p->width = width;
        p->canvas_height = p->height = height;
    }
    return p->canvas_width && p->canvas_height && p->x <= p->canvas_width && p->y <= p->canvas_height &&
           p->width <= p->canvas_width - p->x && p->height <= p->canvas_height - p->y;
}

// Source inspection.

struct Source {
    GuildImageKind kind;
    u32 width, height;               // display orientation
    u32 stored_width, stored_height; // decoder orientation
    u32 frames;
    u8 orientation; // EXIF 1-8
};

// acTL frame count of an APNG (1 for a plain PNG).
u32 png_frames(ConstSlice<u8> s) noexcept {
    u64 at = 8;
    while (at + 12 <= s.count) {
        const u32 length = byte_load_be32(s.data + at);
        const u8* type = s.data + at + 4;
        if (bytes_equal(type, "acTL", 4) && length >= 8 && at + 16 <= s.count)
            return maximum(1u, byte_load_be32(s.data + at + 8));
        if (bytes_equal(type, "IDAT", 4) || bytes_equal(type, "IEND", 4))
            break;
        at += 12ull + length;
    }
    return 1;
}

ProfileImageError inspect(ConstSlice<u8> s, Source* src) noexcept {
    reset_value(src);
    src->orientation = 1;
    if (!byte_range_valid(s) || !s.count)
        return ProfileImageError::input;
    if (s.count > kProfileImageSourceMaximumBytes)
        return ProfileImageError::too_large;
    src->kind = guild_image_sniff(s);
    switch (src->kind) {
    case GuildImageKind::png:
    case GuildImageKind::apng: {
        PngImage image{};
        PngStreamLayout layout{};
        const auto parsed = png_stream_parse(s, &image, &layout);
        if (!parsed.ok() && (parsed.error != PngError::unsupported_interlace || !png_parse(s, &image).ok()))
            return ProfileImageError::decode;
        src->stored_width = image.width;
        src->stored_height = image.height;
        src->frames = src->kind == GuildImageKind::apng ? png_frames(s) : 1;
        break;
    }
    case GuildImageKind::jpeg: {
        Block memory;
        void* storage = memory.allocate(sizeof(JpegWorkspace));
        if (!storage)
            return ProfileImageError::memory;
        auto* workspace = reinterpret_cast<JpegWorkspace*>(storage);
        JpegImage image{};
        JpegThumbnailLayout layout{};
        if (!jpeg_thumbnail_parse(s, 1, &image, workspace, &layout).ok())
            return ProfileImageError::decode;
        src->stored_width = image.width;
        src->stored_height = image.height;
        src->orientation = static_cast<u8>(jpeg_orientation_valid(image.orientation) ? static_cast<u8>(image.orientation) : 1);
        src->frames = 1;
        break;
    }
    case GuildImageKind::gif: {
        Block memory;
        void* storage = memory.allocate(sizeof(GifWorkspace));
        if (!storage)
            return ProfileImageError::memory;
        auto* workspace = reinterpret_cast<GifWorkspace*>(storage);
        GifImage image{};
        image.frames = workspace->frames;
        image.frame_capacity = kGifFrameCapacity;
        if (!gif_parse(s, &image, workspace).ok() || !image.frame_count)
            return ProfileImageError::decode;
        src->stored_width = image.canvas_width;
        src->stored_height = image.canvas_height;
        src->frames = image.frame_count;
        break;
    }
    case GuildImageKind::webp: {
        WebpParts parts{};
        if (!webp_parts(s, &parts))
            return ProfileImageError::decode;
        if (parts.lossless.data && (parts.width > kWebpDimensionMaximum || parts.height > kWebpDimensionMaximum))
            return ProfileImageError::decode;
        WebpThumbnailLayout layout{};
        const bool direct = parts.lossy.data && !parts.animated && (!parts.alpha.data || !(parts.alpha[0] & 3u));
        if (direct && webp_thumbnail_parse(s, &layout) != WebpThumbnailError::none)
            return ProfileImageError::decode;
        // Separately decoded alpha planes share the VP8L decoder's 512 px limit.
        if (!direct && parts.lossy.data && parts.alpha.data &&
            (parts.width > kWebpDimensionMaximum || parts.height > kWebpDimensionMaximum))
            return ProfileImageError::decode;
        src->stored_width = parts.canvas_width;
        src->stored_height = parts.canvas_height;
        src->frames = parts.frames;
        break;
    }
    default: return ProfileImageError::format;
    }
    if (!src->stored_width || !src->stored_height)
        return ProfileImageError::decode;
    const bool swap = src->orientation >= 5;
    src->width = swap ? src->stored_height : src->stored_width;
    src->height = swap ? src->stored_width : src->stored_height;
    return ProfileImageError::none;
}

ProfileImageInfo describe(const Source& src) noexcept {
    ProfileImageInfo info{};
    info.width = src.width;
    info.height = src.height;
    info.frames = src.frames;
    info.kind = src.kind;
    info.animated = src.frames > 1;
    info.output_kind = src.kind == GuildImageKind::gif && info.animated ? GuildImageKind::gif : GuildImageKind::png;
    return info;
}

ProfileImageInfo fail(ProfileImageInfo info, ProfileImageError error) noexcept {
    info.error = error;
    info.bytes = info.output_width = info.output_height = 0;
    return info;
}

// Reduction: decoder rows -> crop band -> integer box filter.

// Display axes against the decoder's stored orientation (EXIF 1-8).
struct Orientation {
    bool swap, flip_x, flip_y; // flip_*: the display axis runs backwards in storage
};
Orientation orientation_axes(u8 o) noexcept {
    const bool swap = o >= 5;
    return {swap, swap ? (o == 6 || o == 7) : (o == 2 || o == 3), swap ? (o == 7 || o == 8) : (o == 3 || o == 4)};
}

// The crop rectangle in stored source pixels and the integer reduction the
// output size still allows.
struct Plan {
    u32 sx, sy, sw, sh;
    u32 stored_width, stored_height;
    u32 factor;
};

struct Reduce {
    LocalThumbnailSink sink;
    Block memory;
    u8* band_row;
    u32 r;                  // source pixels per grid pixel (JPEG DCT scaling)
    u32 bx0, bx1, by0, by1; // band on the grid
    u32 fx, fy, fw, fh;     // decoded frame on the grid
    u32 next;               // next grid row
    bool failed;
};

bool reduce_begin(const Plan& plan, u32 r, u32 grid_width, u32 grid_height, u32 fx, u32 fy, u32 fw, u32 fh,
                  Reduce* d) noexcept {
    d->r = r;
    d->bx0 = plan.sx / r;
    d->by0 = plan.sy / r;
    d->bx1 = minimum(grid_width, static_cast<u32>((static_cast<u64>(plan.sx) + plan.sw + r - 1) / r));
    d->by1 = minimum(grid_height, static_cast<u32>((static_cast<u64>(plan.sy) + plan.sh + r - 1) / r));
    d->fx = fx;
    d->fy = fy;
    d->fw = fw;
    d->fh = fh;
    d->next = 0;
    d->failed = false;
    if (d->bx0 >= d->bx1 || d->by0 >= d->by1)
        return false;
    const u32 bw = d->bx1 - d->bx0, bh = d->by1 - d->by0, longest = maximum(bw, bh);
    const u32 k = minimum(maximum(plan.factor / r, 1u), 200u);
    const u32 extent = (longest + k - 1) / k;
    const u32 scale = local_thumbnail_scale(bw, bh, extent);
    const u64 rw = local_thumbnail_extent(bw, scale), rh = local_thumbnail_extent(bh, scale);
    const u64 output = rw * rh * 4u, sums = rw * 5u * sizeof(u32);
    u8* base = d->memory.allocate(output + sums + static_cast<u64>(bw) * 4u);
    if (!base)
        return false;
    d->band_row = base + output + sums;
    return local_thumbnail_begin(&d->sink, bw, bh, extent, {base, static_cast<u32>(output)},
                                 {reinterpret_cast<u32*>(base + output), static_cast<u32>(rw * 5u)});
}

// One grid row of the band from a frame row (or transparent when null).
bool push_grid_row(Reduce* d, u32 gy, const u8* frame_row) noexcept {
    if (gy < d->by0 || gy >= d->by1)
        return true;
    const u8* row = nullptr;
    if (frame_row && d->fx <= d->bx0 && d->fx + d->fw >= d->bx1) {
        row = frame_row + static_cast<u64>(d->bx0 - d->fx) * 4u;
    } else {
        const u32 bw = d->bx1 - d->bx0;
        clear_bytes(d->band_row, bw * 4u);
        const u32 x0 = maximum(d->bx0, d->fx), x1 = minimum(d->bx1, d->fx + d->fw);
        if (frame_row && x0 < x1)
            copy_bytes(d->band_row + (x0 - d->bx0) * 4u, frame_row + static_cast<u64>(x0 - d->fx) * 4u,
                       (x1 - x0) * 4u);
        row = d->band_row;
    }
    if (!local_thumbnail_push_row(&d->sink, row)) {
        d->failed = true;
        return false;
    }
    return true;
}

bool reduce_done(const Reduce& d) noexcept {
    return !d.failed && d.next >= d.by1;
}

// Frame row `y` (frame coordinates); false stops the decoder once the band
// is complete or on failure.
bool reduce_row(Reduce* d, u32 y, const u8* rgba) noexcept {
    const u32 gy = d->fy + y;
    if (d->failed || y >= d->fh || gy < d->next) {
        d->failed = true;
        return false;
    }
    for (; d->next < gy; ++d->next)
        if (!push_grid_row(d, d->next, nullptr))
            return false;
    if (!push_grid_row(d, gy, rgba))
        return false;
    ++d->next;
    return d->next < d->by1;
}

bool reduce_finish(Reduce* d) noexcept {
    for (; !d->failed && d->next < d->by1; ++d->next)
        push_grid_row(d, d->next, nullptr);
    return !d->failed && local_thumbnail_complete(d->sink);
}

// Decoders.

ProfileImageError decode_png(ConstSlice<u8> s, const Plan& plan, Reduce* d) noexcept {
    PngImage image{};
    PngStreamLayout layout{};
    DeflateWorkspace workspace{};
    struct Rows {
        const PngImage* image;
        Reduce* reduce;
        u8* rgba;
    };
    const auto row_sink = [](void* context, u32 y, const u8* row) noexcept {
        auto& r = *static_cast<Rows*>(context);
        if (r.image->color_type == 6 && r.image->bit_depth == 8)
            return reduce_row(r.reduce, y, row);
        if (!local_thumbnail_png_row(*r.image, row, r.rgba)) {
            r.reduce->failed = true;
            return false;
        }
        return reduce_row(r.reduce, y, r.rgba);
    };
    const auto parsed = png_stream_parse(s, &image, &layout);
    if (parsed.ok()) {
        if (!reduce_begin(plan, 1, image.width, image.height, 0, 0, image.width, image.height, d))
            return ProfileImageError::memory;
        const u64 spans = align16(static_cast<u64>(layout.idat_chunks) * sizeof(DeflateInputSpan));
        Block memory;
        u8* base = memory.allocate(spans + kPngStreamWindowBytes + layout.row_buffer_bytes + image.width * 4ull);
        if (!base)
            return ProfileImageError::memory;
        Rows rows{&image, d, base + spans + kPngStreamWindowBytes + layout.row_buffer_bytes};
        const auto decoded = png_stream_decode(
            s, image, layout, {reinterpret_cast<DeflateInputSpan*>(base), layout.idat_chunks},
            {base + spans, kPngStreamWindowBytes}, {base + spans + kPngStreamWindowBytes, layout.row_buffer_bytes},
            row_sink, &rows, &workspace);
        return decoded.ok() || reduce_done(*d) ? ProfileImageError::none : ProfileImageError::decode;
    }
    // Adam7 streams are not supported; png_parse decodes them whole (<= 2048 px).
    if (parsed.error != PngError::unsupported_interlace || !png_parse(s, &image).ok())
        return ProfileImageError::decode;
    if (!reduce_begin(plan, 1, image.width, image.height, 0, 0, image.width, image.height, d))
        return ProfileImageError::memory;
    const u32 buffer = png_decode_buffer_bytes(image);
    Block memory;
    u8* base = memory.allocate(static_cast<u64>(buffer) + image.width * 4ull);
    if (!base)
        return ProfileImageError::memory;
    if (!png_decode_parsed_scanlines(&image, &workspace, {base, buffer}).ok())
        return ProfileImageError::decode;
    Rows rows{&image, d, base + buffer};
    for (u32 y = 0; y < image.height; ++y)
        if (!row_sink(&rows, y, base + static_cast<u64>(y) * image.row_stride + 1))
            break;
    return d->failed ? ProfileImageError::decode : ProfileImageError::none;
}

ProfileImageError decode_jpeg(ConstSlice<u8> s, const Source& src, const Plan& plan, Reduce* d) noexcept {
    Block workspace_memory;
    void* storage = workspace_memory.allocate(sizeof(JpegWorkspace));
    if (!storage)
        return ProfileImageError::memory;
    auto* workspace = reinterpret_cast<JpegWorkspace*>(storage);
    // The DCT keeps 8, 4, 2 or 1 samples per block: the coarsest the output
    // allows, coarser still when the planes would not fit.
    u32 r = 1;
    while (r < 8 && r * 2 <= plan.factor)
        r *= 2;
    const u32 longest = maximum(src.stored_width, src.stored_height);
    JpegImage image{};
    JpegThumbnailLayout layout{};
    for (;;) {
        const u32 target = maximum(1u, static_cast<u32>(static_cast<u64>(longest) * (8 / r) / 8));
        const auto parsed = jpeg_thumbnail_parse(s, target, &image, workspace, &layout);
        if (parsed.ok())
            break;
        if (parsed.error != JpegError::dimensions || r == 8)
            return ProfileImageError::decode;
        r *= 2;
    }
    r = 8u / layout.samples;
    if (!reduce_begin(plan, r, layout.width, layout.height, 0, 0, layout.width, layout.height, d))
        return ProfileImageError::memory;
    const u64 scratch = align16(layout.coefficient_bytes + layout.plane_bytes);
    Block memory;
    u8* base = memory.allocate(scratch + layout.width * 4ull);
    if (!base || layout.coefficient_bytes + layout.plane_bytes > 0xFFFF'FFFFull)
        return ProfileImageError::memory;
    const auto decoded = jpeg_thumbnail_decode_rows(
        s, image, layout, {base, static_cast<u32>(layout.coefficient_bytes + layout.plane_bytes)},
        {base + scratch, layout.width * 4u},
        [](void* context, u32 y, const u8* rgba) noexcept { return reduce_row(static_cast<Reduce*>(context), y, rgba); },
        d);
    return decoded.ok() || reduce_done(*d) ? ProfileImageError::none : ProfileImageError::decode;
}

// The first frame composited on a transparent canvas, like a canvas drawImage.
ProfileImageError decode_gif_still(ConstSlice<u8> s, const Plan& plan, Reduce* d) noexcept {
    Block workspace_memory;
    void* storage = workspace_memory.allocate(sizeof(GifWorkspace));
    if (!storage)
        return ProfileImageError::memory;
    auto* workspace = reinterpret_cast<GifWorkspace*>(storage);
    GifImage image{};
    image.frames = workspace->frames;
    image.frame_capacity = kGifFrameCapacity;
    if (!gif_parse(s, &image, workspace).ok() || !image.frame_count)
        return ProfileImageError::decode;
    const GifFrame& first = image.frames[0];
    const u64 pixels = static_cast<u64>(first.width) * first.height;
    Block memory;
    u8* base = memory.allocate(pixels + first.width * 4ull);
    if (!base)
        return ProfileImageError::memory;
    GifDecodeResult decoded;
    gif_decode_parsed_frame(&decoded, s, image, 0, workspace, {base, static_cast<u32>(pixels)});
    if (!decoded.ok())
        return ProfileImageError::decode;
    const GifFrame& f = decoded.frame;
    if (!reduce_begin(plan, 1, image.canvas_width, image.canvas_height, f.x_offset, f.y_offset, f.width, f.height, d))
        return ProfileImageError::memory;
    u8* row = base + pixels;
    for (u32 y = 0; y < f.height; ++y) {
        const u8* indices = base + static_cast<u64>(y) * f.width;
        for (u32 x = 0; x < f.width; ++x) {
            const u32 color = decoded.palette.rgba[indices[x]];
            row[x * 4] = static_cast<u8>(color);
            row[x * 4 + 1] = static_cast<u8>(color >> 8);
            row[x * 4 + 2] = static_cast<u8>(color >> 16);
            row[x * 4 + 3] = static_cast<u8>(color >> 24);
        }
        if (!reduce_row(d, y, row))
            break;
    }
    return d->failed ? ProfileImageError::decode : ProfileImageError::none;
}

ProfileImageError decode_webp(ConstSlice<u8> s, const Plan& plan, Reduce* d) noexcept {
    WebpParts parts{};
    if (!webp_parts(s, &parts))
        return ProfileImageError::decode;
    const u32 w = parts.width, h = parts.height;
    const u64 pixels = static_cast<u64>(w) * h;
    if (parts.lossless.data) {
        if (w > kWebpDimensionMaximum || h > kWebpDimensionMaximum)
            return ProfileImageError::decode;
        Block memory;
        const u64 workspace_bytes = align16(sizeof(Vp8lWorkspace));
        u8* base = memory.allocate(workspace_bytes + pixels * 4u);
        if (!base)
            return ProfileImageError::memory;
        auto* workspace = reinterpret_cast<Vp8lWorkspace*>(base);
        WebpImage image{};
        u8* rgba = base + workspace_bytes;
        if (!webp_lossless_decode_vp8l_frame_rgba8(parts.lossless, w, h, &image, workspace,
                                                   {rgba, static_cast<u32>(pixels * 4u)})
                 .ok())
            return ProfileImageError::decode;
        if (!reduce_begin(plan, 1, parts.canvas_width, parts.canvas_height, parts.x, parts.y, w, h, d))
            return ProfileImageError::memory;
        for (u32 y = 0; y < h; ++y)
            if (!reduce_row(d, y, rgba + static_cast<u64>(y) * w * 4u))
                break;
        return d->failed ? ProfileImageError::decode : ProfileImageError::none;
    }
    // The row decoder takes a still container with raw alpha; animation
    // frames and VP8L-coded alpha are rewrapped as a bare VP8 still and the
    // alpha plane is decoded separately.
    const bool raw_alpha = !parts.alpha.data || !(parts.alpha[0] & 3u);
    const bool direct = !parts.animated && raw_alpha;
    ConstSlice<u8> input = s;
    Block wrapped;
    if (!direct) {
        const u32 bytes = parts.lossy.count;
        u8* out = wrapped.allocate(20ull + bytes + 1);
        if (!out)
            return ProfileImageError::memory;
        copy_bytes(out, "RIFF", 4);
        byte_store_le32(out + 4, 12 + bytes + (bytes & 1u));
        copy_bytes(out + 8, "WEBPVP8 ", 8);
        byte_store_le32(out + 16, bytes);
        copy_bytes(out + 20, parts.lossy.data, bytes);
        input = {out, 20 + bytes + (bytes & 1u)};
    }
    WebpThumbnailLayout layout{};
    if (webp_thumbnail_parse(input, &layout) != WebpThumbnailError::none || layout.width != w || layout.height != h)
        return ProfileImageError::decode;
    Block alpha_memory;
    const u8* plane = nullptr;
    if (!direct && parts.alpha.data) {
        const u64 workspace_bytes = align16(sizeof(Vp8lWorkspace));
        u8* base = alpha_memory.allocate(workspace_bytes + pixels * 5u);
        if (!base)
            return ProfileImageError::memory;
        auto* workspace = reinterpret_cast<Vp8lWorkspace*>(base);
        u8* scratch = base + workspace_bytes;
        u8* alpha = scratch + pixels * 4u;
        if (!webp_alpha_decode(parts.alpha, w, h, workspace, {scratch, static_cast<u32>(pixels * 4u)},
                               {alpha, static_cast<u32>(pixels)})
                 .ok())
            return ProfileImageError::decode;
        plane = alpha;
    }
    Block memory;
    const u64 scratch = align16(layout.workspace_bytes);
    u8* base = memory.allocate(scratch + w * 4ull);
    if (!base)
        return ProfileImageError::memory;
    if (!reduce_begin(plan, 1, parts.canvas_width, parts.canvas_height, parts.x, parts.y, w, h, d))
        return ProfileImageError::memory;
    struct Rows {
        Reduce* reduce;
        const u8* plane;
        u8* row;
        u32 width;
    } rows{d, plane, base + scratch, w};
    const auto decoded = webp_thumbnail_decode(
        layout, {base, layout.workspace_bytes},
        [](void* context, u32 y, const u8* rgba) noexcept {
            auto& r = *static_cast<Rows*>(context);
            if (!r.plane)
                return reduce_row(r.reduce, y, rgba);
            copy_bytes(r.row, rgba, r.width * 4u);
            const u8* alpha = r.plane + static_cast<u64>(y) * r.width;
            for (u32 x = 0; x < r.width; ++x)
                r.row[x * 4 + 3] = alpha[x];
            return reduce_row(r.reduce, y, r.row);
        },
        &rows);
    return decoded == WebpThumbnailError::none || reduce_done(*d) ? ProfileImageError::none
                                                                  : ProfileImageError::decode;
}

// Area resample of the reduced band into the rotated output.

// Per output pixel along one axis: the reduced cells it covers and their
// area weights in 14-bit fixed point (summing to 1 << 14).
struct Taps {
    u32* first;
    u32* count;
    u32* offset;
    u16* weight;
};
constexpr u32 kTapOne = 1u << 14;

// Output pixel i (display order) covers display source [c0 + i*len/n, ...);
// along the stored axis that is s0 onwards, reversed when flipped. Cell j
// covers stored source [o + j*cell, min(o + (j+1)*cell, end)). Everything is
// scaled by n so the bounds are integers.
bool build_taps(u32 n, u64 s0, u64 len, bool flip, u64 o, u64 cell, u64 end, u32 cells, u32 capacity,
                Taps* t) noexcept {
    u32 used = 0;
    const u64 base = n * o, step = n * cell, stop = n * end;
    for (u32 i = 0; i < n; ++i) {
        const u64 p = flip ? n - 1u - i : i;
        const u64 a = n * s0 + p * len, b = a + len;
        if (a < base || !cells)
            return false;
        const u64 j0 = (a - base) / step;
        const u64 j1 = minimum<u64>((b - 1 - base) / step, cells - 1u);
        if (j0 > j1)
            return false;
        const auto overlap = [&](u64 j) noexcept {
            const u64 lo = maximum(a, base + j * step), hi = minimum(minimum(b, base + (j + 1) * step), stop);
            return hi > lo ? hi - lo : 0;
        };
        u64 total = 0;
        for (u64 j = j0; j <= j1; ++j)
            total += overlap(j);
        const u32 count = static_cast<u32>(j1 - j0 + 1);
        if (!total || count > capacity - used)
            return false;
        t->first[i] = static_cast<u32>(j0);
        t->count[i] = count;
        t->offset[i] = used;
        u64 cumulative = 0;
        u32 assigned = 0;
        for (u64 j = j0; j <= j1; ++j) {
            cumulative += overlap(j);
            const u32 target = static_cast<u32>((cumulative * kTapOne + total / 2) / total);
            t->weight[used++] = static_cast<u16>(target - assigned);
            assigned = target;
        }
    }
    return true;
}

struct Resample {
    const u8* reduced;
    u32 reduced_width;
    bool swap;
    Taps x, y;
};

void resample_pixel(const Resample& r, u32 u, u32 v, u8* out) noexcept {
    u64 alpha = 0, color[3]{};
    for (u32 a = 0; a < r.y.count[v]; ++a) {
        const u32 jy = r.y.first[v] + a;
        const u64 wy = r.y.weight[r.y.offset[v] + a];
        for (u32 b = 0; b < r.x.count[u]; ++b) {
            const u32 jx = r.x.first[u] + b;
            const u64 w = wy * r.x.weight[r.x.offset[u] + b];
            const u8* p = r.reduced + (r.swap ? static_cast<u64>(jx) * r.reduced_width + jy
                                              : static_cast<u64>(jy) * r.reduced_width + jx) *
                                          4u;
            const u64 aw = w * p[3];
            alpha += aw;
            color[0] += aw * p[0];
            color[1] += aw * p[1];
            color[2] += aw * p[2];
        }
    }
    for (u32 c = 0; c < 3; ++c)
        out[c] = static_cast<u8>(alpha ? (color[c] + alpha / 2) / alpha : 0);
    out[3] = static_cast<u8>((alpha + (1ull << 27)) >> 28);
}

// Still crop.

ProfileImageError crop_still(ConstSlice<u8> s, const Source& src, const ImageCropOutput& crop, GuildImageBuffer* output,
                             u32* bytes) noexcept {
    const bool turned = crop.rotation % 180 != 0;
    const u32 vw = crop.output_width, vh = crop.output_height;
    const u32 ouw = turned ? vh : vw, ouh = turned ? vw : vh;
    const Orientation axes = orientation_axes(src.orientation);
    // Display crop -> stored crop.
    const u32 start_x = axes.flip_x ? src.width - (crop.x + crop.width) : crop.x;
    const u32 start_y = axes.flip_y ? src.height - (crop.y + crop.height) : crop.y;
    Plan plan{};
    plan.stored_width = src.stored_width;
    plan.stored_height = src.stored_height;
    plan.sx = axes.swap ? start_y : start_x;
    plan.sw = axes.swap ? crop.height : crop.width;
    plan.sy = axes.swap ? start_x : start_y;
    plan.sh = axes.swap ? crop.width : crop.height;
    plan.factor = maximum(1u, minimum(crop.width / ouw, crop.height / ouh));
    Block pixels;
    {
        Reduce reduce{};
        ProfileImageError error = ProfileImageError::decode;
        switch (src.kind) {
        case GuildImageKind::png:
        case GuildImageKind::apng: error = decode_png(s, plan, &reduce); break;
        case GuildImageKind::jpeg: error = decode_jpeg(s, src, plan, &reduce); break;
        case GuildImageKind::gif: error = decode_gif_still(s, plan, &reduce); break;
        case GuildImageKind::webp: error = decode_webp(s, plan, &reduce); break;
        default: break;
        }
        if (error != ProfileImageError::none)
            return error;
        if (!reduce_finish(&reduce))
            return ProfileImageError::decode;
        // Cells in stored source pixels along each stored axis.
        const u64 cell = static_cast<u64>(reduce.r) * reduce.sink.scale;
        const u64 ox = static_cast<u64>(reduce.bx0) * reduce.r, oy = static_cast<u64>(reduce.by0) * reduce.r;
        const u64 ex = minimum<u64>(static_cast<u64>(reduce.bx1) * reduce.r, src.stored_width);
        const u64 ey = minimum<u64>(static_cast<u64>(reduce.by1) * reduce.r, src.stored_height);
        // Display x runs along stored y when the orientation swaps axes.
        const u32 cells_x = axes.swap ? reduce.sink.height : reduce.sink.width;
        const u32 cells_y = axes.swap ? reduce.sink.width : reduce.sink.height;
        const u32 capacity_x = ouw + cells_x + 1, capacity_y = ouh + cells_y + 1;
        Block tap_memory;
        const u64 tap_bytes = (static_cast<u64>(ouw) + ouh) * 12u + (static_cast<u64>(capacity_x) + capacity_y) * 2u;
        u8* t = tap_memory.allocate(tap_bytes);
        if (!t)
            return ProfileImageError::memory;
        Resample r{reduce.sink.output, reduce.sink.width, axes.swap, {}, {}};
        u32* words = reinterpret_cast<u32*>(t);
        r.x = {words, words + ouw, words + 2 * ouw, nullptr};
        words += 3 * ouw;
        r.y = {words, words + ouh, words + 2 * ouh, nullptr};
        words += 3 * ouh;
        r.x.weight = reinterpret_cast<u16*>(words);
        r.y.weight = r.x.weight + capacity_x;
        const bool built =
            build_taps(ouw, axes.swap ? plan.sy : plan.sx, crop.width, axes.flip_x,
                       axes.swap ? oy : ox, cell, axes.swap ? ey : ex, cells_x, capacity_x, &r.x) &&
            build_taps(ouh, axes.swap ? plan.sx : plan.sy, crop.height, axes.flip_y, axes.swap ? ox : oy, cell,
                       axes.swap ? ex : ey, cells_y, capacity_y, &r.y);
        if (!built)
            return ProfileImageError::decode;
        u8* out = pixels.allocate(static_cast<u64>(vw) * vh * 4u);
        if (!out)
            return ProfileImageError::memory;
        for (u32 y = 0; y < vh; ++y)
            for (u32 x = 0; x < vw; ++x) {
                u32 u = x, v = y;
                switch (crop.rotation) {
                case 90: u = y, v = ouh - 1 - x; break;
                case 180: u = ouw - 1 - x, v = ouh - 1 - y; break;
                case 270: u = ouw - 1 - y, v = x; break;
                default: break;
                }
                resample_pixel(r, u, v, out + (static_cast<u64>(y) * vw + x) * 4u);
            }
    }
    const u64 bound = png_encode_bound(vw, vh), workspace_bytes = png_encode_workspace_bytes(vw, vh);
    Block workspace, encoded;
    if (!bound || !workspace_bytes || bound > 0xFFFF'FFFFull)
        return ProfileImageError::input;
    u8* work = workspace.allocate(workspace_bytes);
    u8* out = encoded.allocate(bound);
    if (!work || !out)
        return ProfileImageError::memory;
    const u32 written = png_encode_rgba8(pixels.data, vw, vh, {work, static_cast<u32>(workspace_bytes)},
                                         {out, static_cast<u32>(bound)});
    if (!written)
        return ProfileImageError::decode;
    *output = {encoded.take(), static_cast<u32>(bound), written};
    *bytes = written;
    return ProfileImageError::none;
}

// Animated GIF crop.

struct Writer {
    u8* data;
    u32 capacity, at;
    bool overflow;

    void byte(u32 value) noexcept {
        if (at < capacity)
            data[at++] = static_cast<u8>(value);
        else
            overflow = true;
    }
    void bytes(const u8* source, u32 count) noexcept {
        if (count > capacity - at) {
            overflow = true;
            return;
        }
        copy_bytes(data + at, source, count);
        at += count;
    }
    void le16(u32 value) noexcept {
        byte(value & 0xFFu);
        byte(value >> 8 & 0xFFu);
    }
    void patch(u32 where, u32 value) noexcept {
        if (where < at)
            data[where] = static_cast<u8>(value);
    }
};

constexpr u32 kLzwSlots = 8192;
struct LzwTable {
    u32 keys[kLzwSlots];
    u16 codes[kLzwSlots];
};

// GIF LZW (variable-width codes, clear when the 4096-entry table fills) in
// 255-byte sub-blocks, starting with the minimum code size byte.
void lzw_encode(Writer* w, const u8* pixels, u32 count, u32 minimum_code_size, LzwTable* table) noexcept {
    w->byte(minimum_code_size);
    u32 block_at = w->at, block = 0;
    w->byte(0);
    u64 accumulator = 0;
    u32 bits = 0;
    const auto put_byte = [&](u32 value) noexcept {
        w->byte(value);
        if (++block == 255) {
            w->patch(block_at, 255);
            block_at = w->at;
            w->byte(0);
            block = 0;
        }
    };
    const auto emit = [&](u32 code, u32 size) noexcept {
        accumulator |= static_cast<u64>(code) << bits;
        bits += size;
        while (bits >= 8) {
            put_byte(static_cast<u32>(accumulator & 0xFFu));
            accumulator >>= 8;
            bits -= 8;
        }
    };
    const u32 clear = 1u << minimum_code_size, end = clear + 1;
    u32 next = clear + 2, size = minimum_code_size + 1;
    clear_bytes(table->keys, kLzwSlots * 4u);
    emit(clear, size);
    u32 prefix = pixels[0];
    for (u32 i = 1; i < count; ++i) {
        const u32 c = pixels[i];
        const u32 key = (prefix << 8 | c) + 1;
        u32 slot = (key * 2'654'435'761u) >> 19;
        while (table->keys[slot] && table->keys[slot] != key)
            slot = (slot + 1) & (kLzwSlots - 1);
        if (table->keys[slot]) {
            prefix = table->codes[slot];
            continue;
        }
        emit(prefix, size);
        // The decoder adds each entry one code later; widening when the
        // newest entry needs another bit keeps both sides in step.
        table->keys[slot] = key;
        table->codes[slot] = static_cast<u16>(next++);
        if (next - 1 == (1u << size) && size < 12)
            ++size;
        if (next == 4096) {
            emit(clear, size);
            clear_bytes(table->keys, kLzwSlots * 4u);
            next = clear + 2;
            size = minimum_code_size + 1;
        }
        prefix = c;
    }
    emit(prefix, size);
    if (next < 4096 && ++next - 1 == (1u << size) && size < 12)
        ++size;
    emit(end, size);
    if (bits)
        put_byte(static_cast<u32>(accumulator & 0xFFu));
    w->patch(block_at, block);
    if (block)
        w->byte(0);
}

u64 lzw_bound(u64 pixels) noexcept {
    const u64 bytes = (pixels + 3 + pixels / 2000) * 12 / 8 + 2;
    return bytes + bytes / 255 + 3;
}

// Each frame cropped to the rectangle and rotated at source resolution, with
// palettes, transparency, disposal, delays and the loop count kept. Frames
// that miss the rectangle become one transparent pixel to keep their timing.
ProfileImageError crop_gif(ConstSlice<u8> s, const ImageCropOutput& crop, GuildImageBuffer* output, u32* bytes,
                           u32* out_width, u32* out_height) noexcept {
    Block workspace_memory;
    void* storage = workspace_memory.allocate(sizeof(GifWorkspace) + sizeof(LzwTable));
    if (!storage)
        return ProfileImageError::memory;
    auto* workspace = reinterpret_cast<GifWorkspace*>(storage);
    auto* table = reinterpret_cast<LzwTable*>(static_cast<u8*>(storage) + sizeof(GifWorkspace));
    GifImage image{};
    image.frames = workspace->frames;
    image.frame_capacity = kGifFrameCapacity;
    if (!gif_parse(s, &image, workspace).ok() || image.frame_count < 2)
        return ProfileImageError::decode;
    const u32 cx = crop.x, cy = crop.y, cw = crop.width, ch = crop.height;
    const bool turned = crop.rotation % 180 != 0;
    const u32 width = turned ? ch : cw, height = turned ? cw : ch;
    const u32 global_colors = image.has_global_palette ? image.global_palette_count : 0;
    u64 bound = 13 + 3ull * global_colors + 19 + 1;
    for (u32 i = 0; i < image.frame_count; ++i) {
        const GifFrame& f = image.frames[i];
        const u32 x0 = maximum<u32>(f.x_offset, cx), x1 = minimum<u32>(f.x_offset + f.width, cx + cw);
        const u32 y0 = maximum<u32>(f.y_offset, cy), y1 = minimum<u32>(f.y_offset + f.height, cy + ch);
        const u64 area = x0 < x1 && y0 < y1 ? static_cast<u64>(x1 - x0) * (y1 - y0) : 1;
        bound += 8 + 10 + 3ull * 256 + lzw_bound(area);
    }
    const u64 canvas = static_cast<u64>(image.canvas_width) * image.canvas_height;
    Block scratch, encoded;
    u8* indices = scratch.allocate(canvas + static_cast<u64>(cw) * ch);
    u8* out = encoded.allocate(minimum<u64>(bound, kProfileImageGifOutputMaximumBytes));
    if (!indices || !out)
        return ProfileImageError::memory;
    u8* rotated = indices + canvas;
    Writer w{out, static_cast<u32>(encoded.bytes), 0, false};
    w.bytes(reinterpret_cast<const u8*>("GIF89a"), 6);
    w.le16(width);
    w.le16(height);
    w.byte(s[10]);
    w.byte(s[11]);
    w.byte(0);
    if (global_colors)
        w.bytes(s.data + image.global_palette_offset, global_colors * 3);
    if (image.play_count != 1) {
        w.bytes(reinterpret_cast<const u8*>("\x21\xFF\x0BNETSCAPE2.0\x03\x01"), 16);
        w.le16(image.play_count ? image.play_count - 1 : 0);
        w.byte(0);
    }
    for (u32 i = 0; i < image.frame_count && !w.overflow; ++i) {
        GifDecodeResult decoded;
        gif_decode_parsed_frame(&decoded, s, image, i, workspace, {indices, static_cast<u32>(canvas)});
        if (!decoded.ok())
            return ProfileImageError::decode;
        const GifFrame& f = decoded.frame;
        const u32 x0 = maximum<u32>(f.x_offset, cx), x1 = minimum<u32>(f.x_offset + f.width, cx + cw);
        const u32 y0 = maximum<u32>(f.y_offset, cy), y1 = minimum<u32>(f.y_offset + f.height, cy + ch);
        const bool empty = x0 >= x1 || y0 >= y1;
        const bool transparent = empty || (f.flags & gif_frame_transparent);
        const u32 disposal = empty ? 1 : f.dispose == GifDispose::background ? 2 : f.dispose == GifDispose::previous ? 3 : 1;
        w.bytes(reinterpret_cast<const u8*>("\x21\xF9\x04"), 3);
        w.byte(disposal << 2 | (transparent ? 1u : 0u));
        w.le16(f.delay_cs);
        w.byte(empty ? 0 : f.transparent_index);
        w.byte(0);
        if (empty) {
            // One transparent pixel; frames need some palette to index.
            w.byte(0x2C);
            w.le16(0);
            w.le16(0);
            w.le16(1);
            w.le16(1);
            w.byte(global_colors ? 0 : 0x80);
            if (!global_colors)
                for (u32 b = 0; b < 6; ++b)
                    w.byte(0);
            const u8 pixel = 0;
            lzw_encode(&w, &pixel, 1, 2, table);
            continue;
        }
        // The intersection in crop coordinates, then where rotation puts it.
        const u32 iw = x1 - x0, ih = y1 - y0, lx = x0 - cx, ly = y0 - cy;
        u32 ox = lx, oy = ly;
        switch (crop.rotation) {
        case 90: ox = ch - (ly + ih), oy = lx; break;
        case 180: ox = cw - (lx + iw), oy = ch - (ly + ih); break;
        case 270: ox = ly, oy = cw - (lx + iw); break;
        default: break;
        }
        const u32 rw = turned ? ih : iw, rh = turned ? iw : ih;
        for (u32 y = 0; y < rh; ++y)
            for (u32 x = 0; x < rw; ++x) {
                const u32 X = ox + x, Y = oy + y;
                u32 px = X, py = Y;
                switch (crop.rotation) {
                case 90: px = Y, py = ch - 1 - X; break;
                case 180: px = cw - 1 - X, py = ch - 1 - Y; break;
                case 270: px = cw - 1 - Y, py = X; break;
                default: break;
                }
                rotated[static_cast<u64>(y) * rw + x] =
                    indices[static_cast<u64>(py + cy - f.y_offset) * f.width + (px + cx - f.x_offset)];
            }
        const bool local = (f.flags & gif_frame_local_palette) != 0;
        u32 size_bits = 0;
        while ((2u << size_bits) < f.palette_count)
            ++size_bits;
        w.byte(0x2C);
        w.le16(ox);
        w.le16(oy);
        w.le16(rw);
        w.le16(rh);
        w.byte(local ? 0x80u | size_bits : 0u);
        if (local)
            w.bytes(s.data + f.palette_offset, f.palette_count * 3u);
        lzw_encode(&w, rotated, rw * rh, f.minimum_code_size, table);
    }
    w.byte(0x3B);
    if (w.overflow)
        return ProfileImageError::too_large;
    *output = {encoded.data, static_cast<u32>(encoded.bytes), w.at};
    encoded.take();
    *bytes = w.at;
    *out_width = width;
    *out_height = height;
    return ProfileImageError::none;
}

} // namespace

ProfileImageInfo profile_image_probe(ConstSlice<u8> source) noexcept {
    Source src{};
    const ProfileImageError error = inspect(source, &src);
    ProfileImageInfo info = describe(src);
    if (error != ProfileImageError::none) {
        info = fail(ProfileImageInfo{}, error);
        info.kind = src.kind;
    }
    return info;
}

ProfileImageInfo profile_image_crop(ConstSlice<u8> source, const ImageCropOutput& crop,
                                    GuildImageBuffer* output) noexcept {
    if (!output)
        return fail({}, ProfileImageError::input);
    reset_value(output);
    Source src{};
    const ProfileImageError error = inspect(source, &src);
    if (error != ProfileImageError::none) {
        ProfileImageInfo info = fail({}, error);
        info.kind = src.kind;
        return info;
    }
    ProfileImageInfo info = describe(src);
    if ((crop.rotation != 0 && crop.rotation != 90 && crop.rotation != 180 && crop.rotation != 270) || !crop.width ||
        !crop.height || static_cast<u64>(crop.x) + crop.width > src.width ||
        static_cast<u64>(crop.y) + crop.height > src.height)
        return fail(info, ProfileImageError::input);
    if (info.output_kind == GuildImageKind::gif) {
        u32 width = 0, height = 0;
        const ProfileImageError gif = crop_gif(source, crop, output, &info.bytes, &width, &height);
        if (gif == ProfileImageError::none) {
            info.output_width = width;
            info.output_height = height;
            return info;
        }
        if (gif == ProfileImageError::memory)
            return fail(info, gif);
        // Anything this encoder cannot carry over becomes a still of the first frame.
        info.output_kind = GuildImageKind::png;
    }
    if (!crop.output_width || !crop.output_height || crop.output_width > kProfileImageOutputDimensionMaximum ||
        crop.output_height > kProfileImageOutputDimensionMaximum)
        return fail(info, ProfileImageError::input);
    const ProfileImageError still = crop_still(source, src, crop, output, &info.bytes);
    if (still != ProfileImageError::none) {
        guild_image_release(output);
        return fail(info, still);
    }
    info.output_width = crop.output_width;
    info.output_height = crop.output_height;
    return info;
}

} // namespace maicord
