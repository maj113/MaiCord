// Decodes every generated corpus image (tools/generate_image_decode_corpus.py)
// through the same entry points the attachment pipeline uses.
#include "fixtures/image_decode_corpus.hpp"
#include "maicord/media/apng.hpp"
#include "maicord/media/animated_webp.hpp"
#include "maicord/media/jpeg.hpp"
#include "maicord/media/lossy_webp.hpp"
#include "maicord/media/png.hpp"
#include "maicord/media/webp.hpp"
#include <windows.h>

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
u32 fnv(u32 hash, const u8* data, u64 count) noexcept {
    for (u64 i = 0; i < count; ++i) hash = (hash ^ data[i]) * 0x01000193u;
    return hash;
}
u32 rows_hash(const PngImage& image, const u8* scanlines) noexcept {
    u32 hash = 0x811C9DC5u;
    for (u32 y = 0; y < image.height; ++y) hash = fnv(hash, scanlines + u64{y} * image.row_stride + 1, image.row_bytes);
    return hash;
}
struct Arena {
    u8* base; u64 used, capacity;
    template <typename T> T* take(u64 count = 1) noexcept {
        used = (used + 63) & ~u64{63};
        T* result = reinterpret_cast<T*>(base + used);
        used += sizeof(T) * count;
        return used <= capacity ? result : nullptr;
    }
};
bool decode_png(const ImageCorpusEntry& entry, Arena arena) noexcept {
    PngImage image{};
    DeflateWorkspace workspace{};
    if (!png_parse({entry.data, entry.bytes}, &image).ok() || image.width != entry.width || image.height != entry.height) return false;
    const u32 bytes = png_decode_buffer_bytes(image);
    u8* scanlines = bytes ? arena.take<u8>(bytes) : nullptr;
    return scanlines && png_decode_parsed_scanlines(&image, &workspace, {scanlines, bytes}).ok() &&
           rows_hash(image, scanlines) == entry.expected;
}
bool decode_apng(const ImageCorpusEntry& entry, Arena arena) noexcept {
    auto* workspace = arena.take<ApngWorkspace>();
    auto* animation = arena.take<ApngImage>();
    u8* scanlines = arena.take<u8>(64 * 1024);
    DeflateWorkspace deflate{};
    PngImage frame{};
    return scanlines && apng_parse({entry.data, entry.bytes}, animation, workspace).ok() && animation->frame_count == 2 &&
           apng_decode_parsed_default({entry.data, entry.bytes}, *animation, &deflate, &frame, {scanlines, 64 * 1024}).ok() &&
           apng_decode_parsed_frame({entry.data, entry.bytes}, *animation, 1, &deflate, &frame, {scanlines, 64 * 1024}).ok() &&
           frame.width == 5 && frame.height == 4 && rows_hash(frame, scanlines) == entry.expected;
}
bool decode_jpeg(const ImageCorpusEntry& entry, Arena arena) noexcept {
    auto* workspace = arena.take<JpegWorkspace>();
    JpegImage image{};
    if (!workspace || !jpeg_decode_coefficients({entry.data, entry.bytes}, &image, workspace).ok() ||
        image.width != entry.width || image.height != entry.height)
        return false;
    JpegThumbnailLayout layout{};
    if (!jpeg_thumbnail_parse({entry.data, entry.bytes}, maximum(entry.width, entry.height), &image, workspace, &layout).ok() ||
        layout.width != entry.width || layout.height != entry.height)
        return false;
    u8* scratch = arena.take<u8>(layout.coefficient_bytes + layout.plane_bytes);
    u8* rgba = arena.take<u8>(layout.rgba_bytes);
    if (!scratch || !rgba ||
        !jpeg_thumbnail_decode({entry.data, entry.bytes}, image, layout,
                               {scratch, static_cast<u32>(layout.coefficient_bytes + layout.plane_bytes)},
                               {rgba, static_cast<u32>(layout.rgba_bytes)}).ok())
        return false;
    // Pillow's libjpeg oracle uses fancy upsampling; allow for the difference.
    for (u32 j = 0; j < 4; ++j)
        for (u32 i = 0; i < 4; ++i) {
            const u8* p = rgba + (u64{(entry.height - 1) * j / 3} * entry.width + (entry.width - 1) * i / 3) * 4;
            for (u32 c = 0; c < 4; ++c) {
                const i32 difference = i32{p[c]} - entry.samples[j * 4 + i][c];
                if (difference > 24 || difference < -24) return false;
            }
        }
    return true;
}
bool decode_webp_lossless(const ImageCorpusEntry& entry, Arena arena) noexcept {
    auto* workspace = arena.take<Vp8lWorkspace>();
    const u32 bytes = entry.width * entry.height * 4;
    u8* rgba = arena.take<u8>(bytes);
    WebpImage image{};
    return rgba && webp_lossless_decode_rgba8({entry.data, entry.bytes}, &image, workspace, {rgba, bytes}).ok() &&
           fnv(0x811C9DC5u, rgba, bytes) == entry.expected;
}
bool decode_webp_lossy(const ImageCorpusEntry& entry, Arena arena) noexcept {
    auto* lossy = arena.take<LossyWebpWorkspace>();
    auto* lossless = arena.take<Vp8lWorkspace>();
    auto* frame = arena.take<Vp8Keyframe>();
    auto* header = arena.take<Vp8ControlHeader>();
    const u32 pixels = entry.width * entry.height;
    u8* scratch = arena.take<u8>(u64{pixels} * 4);
    u8* alpha = arena.take<u8>(pixels);
    WebpImage image{};
    if (!alpha) return false;
    const auto decoded = lossy_webp_decode_keyframe({entry.data, entry.bytes}, &image, frame, header, &lossy->probabilities,
                                                    &lossy->modes, &lossy->coefficients, lossless, {scratch, pixels * 4}, {alpha, pixels});
    if (!decoded.ok() || image.width != entry.width || image.height != entry.height) return false;
    if (!decoded.alpha_bytes) for (u32 i = 0; i < pixels; ++i) alpha[i] = 255;
    return fnv(0x811C9DC5u, alpha, pixels) == entry.expected;
}
bool decode_webp_animated(const ImageCorpusEntry& entry, Arena arena) noexcept {
    auto* animation = arena.take<AnimatedWebpWorkspace>();
    auto* lossy = arena.take<LossyWebpWorkspace>();
    auto* lossless = arena.take<Vp8lWorkspace>();
    auto* frame = arena.take<Vp8Keyframe>();
    auto* header = arena.take<Vp8ControlHeader>();
    const u32 pixels = entry.width * entry.height;
    u8* rgba = arena.take<u8>(u64{pixels} * 4);
    u8* scratch = arena.take<u8>(u64{pixels} * 4);
    u8* alpha = arena.take<u8>(pixels);
    AnimatedWebpImage image{};
    if (!alpha || !animated_webp_parse({entry.data, entry.bytes}, &image, animation).ok() || image.frame_count != 3 ||
        image.width != entry.width || image.height != entry.height)
        return false;
    for (u32 index = 0; index < image.frame_count; ++index) {
        const auto decoded = animated_webp_decode_frame({entry.data, entry.bytes}, index, &image, animation, lossless, lossy, frame,
                                                        header, {rgba, pixels * 4}, {scratch, pixels * 4}, {alpha, pixels});
        if (!decoded.ok()) return false;
        if (index == 0 && entry.expected && fnv(0x811C9DC5u, rgba, u64{pixels} * 4) != entry.expected) return false;
    }
    return true;
}
} // namespace

void test_image_decode_corpus(Check check) noexcept {
    constexpr u64 capacity = 16ull << 20;
    auto* base = static_cast<u8*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base) { check(false, "image corpus workspace"); return; }
    u32 failures = 0;
    for (const auto& entry : kImageCorpus) {
        const Arena arena{base, 0, capacity};
        bool ok = false;
        switch (entry.kind) {
        case ImageCorpusKind::png: ok = decode_png(entry, arena); break;
        case ImageCorpusKind::apng: ok = decode_apng(entry, arena); break;
        case ImageCorpusKind::jpeg: ok = decode_jpeg(entry, arena); break;
        case ImageCorpusKind::webp_lossless: ok = decode_webp_lossless(entry, arena); break;
        case ImageCorpusKind::webp_lossy: ok = decode_webp_lossy(entry, arena); break;
        case ImageCorpusKind::webp_animated: ok = decode_webp_animated(entry, arena); break;
        }
        if (!ok) {
            ++failures;
            check(false, entry.name);
        }
    }
    check(!failures, "every generated PNG/JPEG/WebP corpus variant decodes to its oracle");
    VirtualFree(base, 0, MEM_RELEASE);
}
} // namespace maicord
