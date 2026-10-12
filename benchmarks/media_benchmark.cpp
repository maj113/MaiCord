#include "maicord/app/cdn_media_pipeline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/media/animation_timeline.hpp"
#include "maicord/media/animated_webp.hpp"
#include "maicord/media/encoded_cache.hpp"
#include "maicord/media/lossy_webp.hpp"
#include "maicord/media/presentation_policy.hpp"
#include "maicord/protocol/cdn_url.hpp"
#include "maicord/protocol/cdn_http.hpp"
#include "maicord/render/gpu_timing.hpp"
#include "maicord/render/media_atlas_gpu.hpp"
#include "maicord/render/presentation_damage.hpp"
#include "../tests/jpeg_progressive_fixture.hpp"
#include "../tests/jpeg_reference_fixture.hpp"
#include "../tests/jpeg_cmyk_fixture.hpp"
#include "../tests/media_fixture.hpp"
#include "../tests/webp_fixture.hpp"

#include <d3d11_1.h>

namespace maicord {
namespace {

inline constexpr u32 kQuickWorkloadDivisor = 128;
u32 kTargetIterations = 1'000'000;
u32 kAttachmentTargetIterations = 1'000'000;
u32 kFindIterations = 1'000'000;
u32 kAtlasChurnIterations = 4'096;
u32 kMissingFindIterations = 1'000'000;
u32 kPresentationDamageIterations = 1'000'000;
u32 kVisibilityEpochIterations = 1'000'000;
u32 kPresentationPolicyIterations = 1'000'000;
u32 kUploadIterations = 256;
u32 kPngDecodeIterations = 4'096;
u32 kPngUploadIterations = 256;
u32 kWebpDecodeIterations = 65'536;
u32 kWebpUploadIterations = 256;
u32 kVp8DecodeIterations = 65'536;
u32 kVp8UploadIterations = 256;
u32 kJpegDecodeIterations = 16'384;
u32 kJpegUploadIterations = 256;
u32 kProgressiveJpegDecodeIterations = 4'096;
u32 kProgressiveJpegUploadIterations = 256;
u32 kCmykJpegDecodeIterations = 8'192;
u32 kCmykJpegUploadIterations = 256;
u32 kYcckJpegDecodeIterations = 4'096;
u32 kYcckJpegUploadIterations = 256;
u32 kOrientedJpegDecodeIterations = 8'192;
u32 kOrientedJpegUploadIterations = 256;
u32 kIccJpegDecodeIterations = 8'192;
u32 kIccJpegUploadIterations = 256;
u32 kIccLutJpegDecodeIterations = 8'192;
u32 kIccLutJpegUploadIterations = 256;
u32 kApngDecodeIterations = 16'384;
u32 kApngSequenceIterations = 256;
u32 kGifDecodeIterations = 32'768;
u32 kGifSequenceIterations = 256;
u32 kAnimatedWebpDecodeIterations = 32'768;
u32 kAnimatedWebpSequenceIterations = 256;
u32 kMixedAnimatedWebpDecodeIterations = 8'192;
u32 kMixedAnimatedWebpSequenceIterations = 256;
u32 kCdnRequestIterations = 1'000'000;
u32 kEncodedCacheFindIterations = 1'000'000;
u32 kPipelineAdmitIterations = 1'024;
u32 kAnimationWakeScanIterations = 1'000'000;
u32 kAnimationAdvanceBatchIterations = 32'768;
inline constexpr u32 kAnimationEntryCapacity = 256;
u32 kAnimationScaleWakeScanIterations = 100'000;
inline constexpr u32 kAnimationScaleEntryCapacity = 4'096;
inline constexpr u16 kAtlasExtent = 1'024;
inline constexpr u16 kAssetExtent = 64;

MediaAtlasEntry g_media_entries[256];
u32 g_media_buckets[512];
GlyphAtlasEntry g_atlas_entries[256];
GlyphAtlasRow g_atlas_rows[64];
GlyphAtlasEntry g_visibility_entries[4'096];
GlyphAtlasRow g_visibility_rows[64];
u8 g_pixels[kAssetExtent * kAssetExtent * 4];
u8 g_png_encoded[17'000];
u8 g_png_zlib[17'000];
u8 g_png_scanlines[17'000];
alignas(u32) u8 g_webp_rgba[16 * 16 * 4];
Vp8lWorkspace g_webp_workspace{};
LossyWebpWorkspace g_lossy_webp_workspace{};
JpegWorkspace g_jpeg_workspace{};
EncodedMediaCacheEntry g_encoded_entries[8];
u16 g_encoded_page_owners[64];
u8 g_encoded_storage[64 * kEncodedMediaCachePageBytes];
AnimationTimelineEntry g_animation_entries[kAnimationEntryCapacity];
u32 g_animation_buckets[kAnimationEntryCapacity * 2];
AnimationFrameAdvance g_animation_advances[kAnimationEntryCapacity];
AnimationTimelineEntry g_animation_scale_entries[kAnimationScaleEntryCapacity];
u32 g_animation_scale_buckets[kAnimationScaleEntryCapacity * 2];

// Same field order and widths as the pre-M137 production record. Keeping the
// control in the same binary isolates scan bandwidth from compiler/host drift.
struct LegacyAnimationTimelineEntry80 {
    MediaAssetKey key;
    u64 due_at_us;
    u64 paused_remaining_us;
    u64 pending_scheduled_at_us;
    u32 generation;
    u32 frame_count;
    u32 frame_index;
    u32 play_count;
    u32 play_index;
    u32 seen_epoch;
    u32 pending_frame_index;
    u32 pending_play_index;
    u32 flags;
};

static_assert(sizeof(LegacyAnimationTimelineEntry80) == 80);

LegacyAnimationTimelineEntry80 g_animation_legacy_scale_entries[kAnimationScaleEntryCapacity];
ApngWorkspace g_apng_workspace{};
PngImage g_apng_frame_images[4]{};
ApngFrame g_apng_frames[4]{};
u8 g_apng_frame_scanlines[4][68]{};
GifWorkspace g_gif_workspace{};
GifFrame g_gif_frames[4]{};
GifPalette g_gif_palettes[4]{};
u8 g_gif_frame_indices[4][16]{};
AnimatedWebpWorkspace g_animated_webp_workspace{};
AnimatedWebpFrame g_animated_webp_frames[4]{};
u32 g_animated_webp_frame_bytes[4]{};
alignas(u32) u8 g_animated_webp_rgba[4][64]{};

u32 quick_iteration_count(u32 exhaustive_count) noexcept {
    return maximum(2u, (exhaustive_count + kQuickWorkloadDivisor - 1u) /
                           kQuickWorkloadDivisor);
}

bool configure_benchmark_workload() noexcept {
    wchar_t value[2]{};
    const DWORD length = GetEnvironmentVariableW(
        L"MAICORD_MEDIA_BENCHMARK_EXHAUSTIVE", value, 2);
    const bool exhaustive = length == 1 && value[0] == L'1';
    if (exhaustive) {
        return true;
    }
    u32* counts[] = {
        &kTargetIterations, &kAttachmentTargetIterations, &kFindIterations,
        &kAtlasChurnIterations, &kMissingFindIterations,
        &kPresentationDamageIterations, &kVisibilityEpochIterations,
        &kPresentationPolicyIterations, &kUploadIterations,
        &kPngDecodeIterations, &kPngUploadIterations,
        &kWebpDecodeIterations, &kWebpUploadIterations,
        &kVp8DecodeIterations, &kVp8UploadIterations,
        &kJpegDecodeIterations, &kJpegUploadIterations,
        &kProgressiveJpegDecodeIterations, &kProgressiveJpegUploadIterations,
        &kCmykJpegDecodeIterations, &kCmykJpegUploadIterations,
        &kYcckJpegDecodeIterations, &kYcckJpegUploadIterations,
        &kOrientedJpegDecodeIterations, &kOrientedJpegUploadIterations,
        &kIccJpegDecodeIterations, &kIccJpegUploadIterations,
        &kIccLutJpegDecodeIterations, &kIccLutJpegUploadIterations,
        &kApngDecodeIterations, &kApngSequenceIterations,
        &kGifDecodeIterations, &kGifSequenceIterations,
        &kAnimatedWebpDecodeIterations, &kAnimatedWebpSequenceIterations,
        &kMixedAnimatedWebpDecodeIterations,
        &kMixedAnimatedWebpSequenceIterations, &kCdnRequestIterations,
        &kEncodedCacheFindIterations, &kPipelineAdmitIterations,
        &kAnimationWakeScanIterations, &kAnimationAdvanceBatchIterations,
        &kAnimationScaleWakeScanIterations,
    };
    for (u32* count : counts) {
        *count = quick_iteration_count(*count);
    }
    kAnimationScaleWakeScanIterations =
        (kAnimationScaleWakeScanIterations + 7u) & ~7u;
    return false;
}
AnimatedWebpFrame g_mixed_animated_webp_frames[3]{};
alignas(u32) u8 g_mixed_animated_webp_rgba[64]{};
u8 g_mixed_animated_webp_alpha[16]{};

inline constexpr u8 kVp8ExpectedRgba[] = {
    4,  2,   0,  255, 59,  30,  40,  255, 130, 44,  83,  255, 185, 72,  124, 255,
    32, 59,  34, 255, 87,  86,  75,  255, 157, 100, 117, 255, 212, 127, 158, 255,
    42, 126, 57, 255, 97,  154, 98,  255, 166, 168, 140, 255, 221, 196, 181, 255,
    69, 182, 89, 255, 124, 209, 130, 255, 194, 225, 173, 255, 248, 252, 215, 255,
};

u32 text_length(const char* value) noexcept {
    u32 length = 0;
    while (value[length]) {
        ++length;
    }
    return length;
}

void write_text(const char* value) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), value, text_length(value), &written, nullptr);
}

void write_u64(u64 value) noexcept {
    char digits[24]{};
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    char output[24]{};
    for (u32 index = 0; index < count; ++index) {
        output[index] = digits[count - index - 1];
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, count, &written, nullptr);
}

void write_metric(const char* name, u64 value) noexcept {
    write_text(name);
    write_text("=");
    write_u64(value);
    write_text("\r\n");
}

u64 legacy_animation_next_wake_at_us(
    const LegacyAnimationTimelineEntry80* entries, u32 count) noexcept {
    u64 next = 0;
    for (u32 index = 0; index < count; ++index) {
        const LegacyAnimationTimelineEntry80& entry = entries[index];
        const u32 blocked = animation_timeline_entry_occupied |
                            animation_timeline_entry_visible;
        if ((entry.flags & blocked) != blocked ||
            (entry.flags & (animation_timeline_entry_pending |
                            animation_timeline_entry_finished)) != 0 ||
            entry.due_at_us == 0) {
            continue;
        }
        if (next == 0 || entry.due_at_us < next) {
            next = entry.due_at_us;
        }
    }
    return next;
}

struct AnimationBenchmarkResult {
    u64 scan_us;
    u64 scan_checksum;
    u64 advance_us;
    u64 advance_checksum;
    u64 scale_scan_us;
    u64 scale_scan_checksum;
    u64 legacy_scale_scan_us;
    u64 legacy_scale_scan_checksum;
};

struct AnimationScanMeasurement {
    u64 microseconds;
    u64 checksum;
};

AnimationScanMeasurement measure_animation_scan(
    const Clock& clock, const AnimationTimeline* timeline,
    u32 iterations) noexcept {
    AnimationScanMeasurement result{};
    const i64 begin = clock_now();
    for (u32 iteration = 0; iteration < iterations; ++iteration) {
        result.checksum += animation_timeline_next_wake_at_us(timeline);
    }
    result.microseconds =
        maximum<u64>(1, clock_microseconds(clock, begin, clock_now()));
    return result;
}

AnimationScanMeasurement measure_legacy_animation_scan(
    const Clock& clock, const LegacyAnimationTimelineEntry80* entries,
    u32 count, u32 iterations) noexcept {
    AnimationScanMeasurement result{};
    const i64 begin = clock_now();
    for (u32 iteration = 0; iteration < iterations; ++iteration) {
        result.checksum += legacy_animation_next_wake_at_us(entries, count);
    }
    result.microseconds =
        maximum<u64>(1, clock_microseconds(clock, begin, clock_now()));
    return result;
}

void write_animation_metrics(const AnimationBenchmarkResult& result) noexcept {
    write_metric("animation_timeline_entry_bytes", sizeof(AnimationTimelineEntry));
    write_metric("animation_timeline_capacity_bytes", sizeof(g_animation_entries));
    write_metric("animation_wake_scan_count", kAnimationWakeScanIterations);
    write_metric("animation_wake_scan_per_second",
                 static_cast<u64>(kAnimationWakeScanIterations) * 1'000'000 /
                     result.scan_us);
    write_metric("animation_wake_scan_checksum", result.scan_checksum);
    write_metric("animation_advance_count",
                 static_cast<u64>(kAnimationAdvanceBatchIterations) * kAnimationEntryCapacity);
    write_metric("animation_advance_per_second",
                 static_cast<u64>(kAnimationAdvanceBatchIterations) * kAnimationEntryCapacity *
                     1'000'000 / result.advance_us);
    write_metric("animation_advance_checksum", result.advance_checksum);
    write_metric("animation_timeline_scale_capacity", kAnimationScaleEntryCapacity);
    write_metric("animation_timeline_scale_capacity_bytes", sizeof(g_animation_scale_entries));
    write_metric("animation_timeline_legacy_scale_capacity_bytes",
                 sizeof(g_animation_legacy_scale_entries));
    write_metric("animation_wake_scan_4096_count", kAnimationScaleWakeScanIterations);
    write_metric("animation_wake_scan_4096_per_second",
                 static_cast<u64>(kAnimationScaleWakeScanIterations) * 1'000'000 /
                     result.scale_scan_us);
    write_metric("animation_wake_scan_4096_legacy_per_second",
                 static_cast<u64>(kAnimationScaleWakeScanIterations) * 1'000'000 /
                     result.legacy_scale_scan_us);
    write_metric("animation_wake_scan_4096_checksum", result.scale_scan_checksum);
    write_metric("animation_wake_scan_4096_legacy_checksum",
                 result.legacy_scale_scan_checksum);
}

bool timeline_only_requested() noexcept {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"MAICORD_MEDIA_TIMELINE_ONLY", value,
                                   sizeof(value) / sizeof(value[0])) != 0;
}

bool wait_query(ID3D11DeviceContext* context, ID3D11Query* query, void* output, u32 output_bytes) noexcept {
    const u64 deadline_ms = GetTickCount64() + 2'000u;
    for (;;) {
        const HRESULT result = context->GetData(query, output, output_bytes, 0);
        if (result == S_OK) {
            return true;
        }
        if (FAILED(result)) {
            return false;
        }
        if (GetTickCount64() >= deadline_ms) {
            return false;
        }
        Sleep(0);
    }
}

void build_pixels() noexcept {
    for (u32 y = 0; y < kAssetExtent; ++y) {
        for (u32 x = 0; x < kAssetExtent; ++x) {
            const u32 offset = (y * kAssetExtent + x) * 4;
            g_pixels[offset] = static_cast<u8>(32 + x * 3);
            g_pixels[offset + 1] = static_cast<u8>(48 + y * 3);
            g_pixels[offset + 2] = static_cast<u8>(224 - x * 2);
            g_pixels[offset + 3] = 255;
        }
    }
}

struct JpegReferenceCheck {
    u64 checksum;
    u32 maximum_error;
    bool within_tolerance;
};

JpegReferenceCheck check_jpeg_reference(MediaAtlas& atlas, const D3D11_MAPPED_SUBRESOURCE& mapped,
                                        MediaAssetKey key, ConstSlice<u8> reference_rgb,
                                        JpegOrientation orientation = JpegOrientation::top_left,
                                        bool swap_red_blue = false) noexcept {
    JpegReferenceCheck check{};
    const MediaAtlasResult resident = media_atlas_find(&atlas, key);
    check.within_tolerance = resident.ok() && resident.width == 16 && resident.height == 16 &&
                             reference_rgb.data && reference_rgb.count == 16u * 16u * 3u &&
                             jpeg_orientation_valid(orientation);
    for (u32 y = 0; y < 16 && check.within_tolerance; ++y) {
        const u8* row =
            static_cast<const u8*>(mapped.pData) + (resident.y + y) * mapped.RowPitch + resident.x * 4u;
        for (u32 x = 0; x < 16; ++x) {
            const u8* pixel = row + x * 4u;
            u32 source_x = x;
            u32 source_y = y;
            switch (orientation) {
            case JpegOrientation::top_right:
                source_x = 15u - x;
                break;
            case JpegOrientation::bottom_right:
                source_x = 15u - x;
                source_y = 15u - y;
                break;
            case JpegOrientation::bottom_left:
                source_y = 15u - y;
                break;
            case JpegOrientation::left_top:
                source_x = y;
                source_y = x;
                break;
            case JpegOrientation::right_top:
                source_x = y;
                source_y = 15u - x;
                break;
            case JpegOrientation::right_bottom:
                source_x = 15u - y;
                source_y = 15u - x;
                break;
            case JpegOrientation::left_bottom:
                source_x = 15u - y;
                source_y = x;
                break;
            case JpegOrientation::top_left:
                break;
            }
            for (u32 channel = 0; channel < 3; ++channel) {
                const u32 reference_channel = swap_red_blue && channel != 1 ? 2u - channel : channel;
                const u8 expected = reference_rgb[(source_y * 16u + source_x) * 3u + reference_channel];
                const u32 error =
                    pixel[channel] > expected ? pixel[channel] - expected : expected - pixel[channel];
                check.maximum_error = maximum<u32>(check.maximum_error, error);
                check.checksum += pixel[channel];
            }
            check.checksum += pixel[3];
            check.within_tolerance = check.within_tolerance && pixel[3] == 255;
        }
    }
    check.within_tolerance = check.within_tolerance && check.maximum_error <= 2;
    return check;
}

JpegReferenceCheck check_jpeg_checksum(MediaAtlas& atlas, const D3D11_MAPPED_SUBRESOURCE& mapped,
                                       MediaAssetKey key) noexcept {
    JpegReferenceCheck check{};
    const MediaAtlasResult resident = media_atlas_find(&atlas, key);
    check.within_tolerance = resident.ok() && resident.width == 16 && resident.height == 16;
    for (u32 y = 0; y < 16 && check.within_tolerance; ++y) {
        const u8* row =
            static_cast<const u8*>(mapped.pData) + (resident.y + y) * mapped.RowPitch + resident.x * 4u;
        for (u32 x = 0; x < 16; ++x) {
            const u8* pixel = row + x * 4u;
            check.checksum += pixel[0] + pixel[1] + pixel[2] + pixel[3];
            check.within_tolerance = check.within_tolerance && pixel[3] == 255;
        }
    }
    return check;
}

int run() noexcept {
    const bool exhaustive = configure_benchmark_workload();
    write_text(exhaustive ? "media_benchmark_mode=exhaustive\r\n"
                          : "media_benchmark_mode=quick\r\n");
    const Clock clock = clock_initialize();
    u8 target[128]{};
    u64 target_checksum = 0;
    const i64 target_begin = clock_now();
    for (u32 iteration = 0; iteration < kTargetIterations; ++iteration) {
        const CdnTargetResult result = write_custom_emoji_cdn_target(
            10'000 + iteration, (iteration & 1) != 0, static_cast<u16>(16u << (iteration & 7)),
            Slice<u8>{target, sizeof(target)});
        if (!result.ok()) {
            return 1;
        }
        target_checksum += result.bytes + target[0] + target[result.bytes - 1];
    }
    const u64 target_us = maximum<u64>(1, clock_microseconds(clock, target_begin, clock_now()));

    constexpr Utf8View signed_attachment =
        utf8_literal("https://cdn.discordapp.com/attachments/123/456/benchmark%20image.jpg"
                     "?ex=ffffffff&is=66d3ee00&hm="
                     "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef&");
    u8 attachment_target[kCdnMaximumTargetBytes]{};
    u64 attachment_target_checksum = 0;
    const i64 attachment_target_begin = clock_now();
    for (u32 iteration = 0; iteration < kAttachmentTargetIterations; ++iteration) {
        const AttachmentUrlResult parsed = parse_signed_attachment_url(
            signed_attachment, 456, Slice<u8>{attachment_target, sizeof(attachment_target)});
        if (!parsed.ok()) {
            return 32;
        }
        attachment_target_checksum +=
            parsed.target_bytes + parsed.issued_at_seconds + attachment_target[iteration & 63u];
    }
    const u64 attachment_target_us =
        maximum<u64>(1, clock_microseconds(clock, attachment_target_begin, clock_now()));

    AnimationTimeline animation_timeline{};
    if (!animation_timeline_initialize(
            &animation_timeline,
            Slice<AnimationTimelineEntry>{g_animation_entries,
                                          kAnimationEntryCapacity},
            Slice<u32>{g_animation_buckets,
                       kAnimationEntryCapacity * 2})) {
        return 28;
    }
    constexpr u64 animation_start_us = 1'000'000;
    constexpr u64 animation_frame_us = 16'667;
    for (u32 index = 0; index < kAnimationEntryCapacity; ++index) {
        if (!animation_timeline_publish(
                 &animation_timeline,
                 AnimationTimelineDescriptor{media_asset_custom_emoji(50'000 + index, true),
                                             animation_frame_us, index + 1, 4, 0, true},
                 animation_start_us)
                 .ok()) {
            return 29;
        }
    }
    u64 animation_scan_checksum = 0;
    const i64 animation_scan_begin = clock_now();
    for (u32 iteration = 0; iteration < kAnimationWakeScanIterations; ++iteration) {
        animation_scan_checksum += animation_timeline_next_wake_at_us(&animation_timeline);
    }
    const u64 animation_scan_us =
        maximum<u64>(1, clock_microseconds(clock, animation_scan_begin, clock_now()));

    u64 animation_advance_checksum = 0;
    u64 animation_now_us = animation_start_us;
    const i64 animation_advance_begin = clock_now();
    for (u32 iteration = 0; iteration < kAnimationAdvanceBatchIterations; ++iteration) {
        animation_now_us += animation_frame_us;
        const AnimationTimelineCollectResult due = animation_timeline_collect_due(
            &animation_timeline, animation_now_us,
            Slice<AnimationFrameAdvance>{g_animation_advances, kAnimationEntryCapacity});
        if (!due.ok() || due.advances != kAnimationEntryCapacity) {
            return 30;
        }
        for (u32 index = 0; index < due.advances; ++index) {
            const AnimationFrameAdvance& advance = g_animation_advances[index];
            if (!animation_timeline_commit_advance(&animation_timeline, advance, animation_frame_us).ok()) {
                return 31;
            }
            animation_advance_checksum +=
                advance.frame_index + advance.play_index + advance.scheduled_at_us;
        }
    }
    const u64 animation_advance_us =
        maximum<u64>(1, clock_microseconds(clock, animation_advance_begin, clock_now()));

    AnimationTimeline animation_scale_timeline{};
    if (!animation_timeline_initialize(
            &animation_scale_timeline,
            Slice<AnimationTimelineEntry>{g_animation_scale_entries,
                                          kAnimationScaleEntryCapacity},
            Slice<u32>{g_animation_scale_buckets,
                       kAnimationScaleEntryCapacity * 2})) {
        return 97;
    }
    for (u32 index = 0; index < kAnimationScaleEntryCapacity; ++index) {
        if (!animation_timeline_publish(
                 &animation_scale_timeline,
                 AnimationTimelineDescriptor{media_asset_custom_emoji(100'000 + index, true),
                                             animation_frame_us, index + 1, 4, 0, true},
                 animation_start_us)
                 .ok()) {
            return 98;
        }
        LegacyAnimationTimelineEntry80& legacy = g_animation_legacy_scale_entries[index];
        legacy.key = media_asset_custom_emoji(100'000 + index, true);
        legacy.due_at_us = animation_start_us + animation_frame_us;
        legacy.generation = index + 1;
        legacy.frame_count = 4;
        legacy.flags = animation_timeline_entry_occupied |
                       animation_timeline_entry_visible;
    }
    constexpr u32 kAnimationScaleMeasurementRounds = 8;
    const u32 kAnimationScaleScansPerRound =
        kAnimationScaleWakeScanIterations / kAnimationScaleMeasurementRounds;
    // Prime both working sets, then alternate measurement order so CPU boost
    // and cache warm-up do not consistently favor either record layout.
    const u64 animation_scale_warmup =
        animation_timeline_next_wake_at_us(&animation_scale_timeline) +
        legacy_animation_next_wake_at_us(
            g_animation_legacy_scale_entries, kAnimationScaleEntryCapacity);
    if (animation_scale_warmup == 0) {
        return 99;
    }
    u64 animation_scale_scan_checksum = 0;
    u64 animation_legacy_scale_scan_checksum = 0;
    u64 animation_scale_scan_us = 0;
    u64 animation_legacy_scale_scan_us = 0;
    for (u32 round = 0; round < kAnimationScaleMeasurementRounds; ++round) {
        AnimationScanMeasurement production{};
        AnimationScanMeasurement legacy{};
        if ((round & 1u) == 0) {
            production = measure_animation_scan(
                clock, &animation_scale_timeline, kAnimationScaleScansPerRound);
            legacy = measure_legacy_animation_scan(
                clock, g_animation_legacy_scale_entries,
                kAnimationScaleEntryCapacity, kAnimationScaleScansPerRound);
        } else {
            legacy = measure_legacy_animation_scan(
                clock, g_animation_legacy_scale_entries,
                kAnimationScaleEntryCapacity, kAnimationScaleScansPerRound);
            production = measure_animation_scan(
                clock, &animation_scale_timeline, kAnimationScaleScansPerRound);
        }
        animation_scale_scan_us += production.microseconds;
        animation_scale_scan_checksum += production.checksum;
        animation_legacy_scale_scan_us += legacy.microseconds;
        animation_legacy_scale_scan_checksum += legacy.checksum;
    }

    const AnimationBenchmarkResult animation_result{
        animation_scan_us,
        animation_scan_checksum,
        animation_advance_us,
        animation_advance_checksum,
        animation_scale_scan_us,
        animation_scale_scan_checksum,
        animation_legacy_scale_scan_us,
        animation_legacy_scale_scan_checksum,
    };
    if (timeline_only_requested()) {
        write_animation_metrics(animation_result);
        return 0;
    }

    MediaAtlas atlas{};
    if (!media_atlas_initialize(
            &atlas, kAtlasExtent, kAtlasExtent, 1, Slice<MediaAtlasEntry>{g_media_entries, 256},
            Slice<u32>{g_media_buckets, 512},
            Slice<GlyphAtlasEntry>{g_atlas_entries, 256}, Slice<GlyphAtlasRow>{g_atlas_rows, 64})) {
        return 2;
    }
    for (u32 index = 0; index < 128; ++index) {
        if (!media_atlas_allocate(&atlas, media_asset_custom_emoji(1'000 + index, (index & 1) != 0), 32, 32)
                 .ok()) {
            return 3;
        }
    }
    u64 find_checksum = 0;
    const i64 find_begin = clock_now();
    for (u32 iteration = 0; iteration < kFindIterations; ++iteration) {
        const u32 index = iteration & 127;
        const MediaAtlasResult found =
            media_atlas_find(&atlas, media_asset_custom_emoji(1'000 + index, (index & 1) != 0));
        if (!found.ok()) {
            return 4;
        }
        find_checksum += found.x + found.y + found.handle.generation;
    }
    const u64 find_us = maximum<u64>(1, clock_microseconds(clock, find_begin, clock_now()));

    u64 churn_checksum = 0;
    for (u32 iteration = 0; iteration < kAtlasChurnIterations; ++iteration) {
        media_atlas_begin_visibility(&atlas);
        const MediaAtlasResult inserted = media_atlas_allocate(
            &atlas, media_asset_attachment(100'000 + iteration), 240, 240);
        if (!inserted.ok()) {
            return 84;
        }
        churn_checksum += inserted.handle.generation + inserted.evicted_entries;
    }
    u64 missing_find_checksum = 0;
    const i64 missing_find_begin = clock_now();
    for (u32 iteration = 0; iteration < kMissingFindIterations; ++iteration) {
        const MediaAtlasResult missing = media_atlas_find(
            &atlas, media_asset_attachment(1'000'000 + iteration));
        if (missing.error != MediaAtlasError::not_found) {
            return 85;
        }
        missing_find_checksum += static_cast<u8>(missing.error);
    }
    const u64 missing_find_us = maximum<u64>(
        1, clock_microseconds(clock, missing_find_begin, clock_now()));

    PresentationDamageTracker presentation_damage{};
    presentation_damage_reset(&presentation_damage);
    u64 presentation_damage_checksum = 0;
    const i64 presentation_damage_begin = clock_now();
    for (u32 iteration = 0; iteration < kPresentationDamageIterations;
         ++iteration) {
        const u16 slot = static_cast<u16>(
            iteration & (kPresentationDamageMediaCapacity - 1u));
        const MediaAtlasHandle handle{slot, 1};
        const float left = static_cast<float>(slot & 63u) * 8.0f;
        const float top = static_cast<float>(slot >> 6) * 8.0f;
        if (!presentation_damage_note_media(&presentation_damage, handle) ||
            !presentation_damage_add_media(
                &presentation_damage, handle,
                PresentationDamageRect{left, top, left + 8.0f, top + 8.0f},
                PresentationDamageRect{0.0f, 0.0f, 512.0f, 512.0f})) {
            return 86;
        }
        if ((iteration & (kPresentationDamageMediaCapacity - 1u)) ==
            kPresentationDamageMediaCapacity - 1u) {
            const PresentationDamage current = presentation_damage_current(
                presentation_damage, 512.0f, 512.0f);
            if (!current.valid || current.full) {
                return 87;
            }
            presentation_damage_checksum +=
                static_cast<u64>(current.rectangle.right) +
                static_cast<u64>(current.rectangle.bottom);
            presentation_damage_commit(&presentation_damage);
        }
    }
    const u64 presentation_damage_us = maximum<u64>(
        1, clock_microseconds(clock, presentation_damage_begin, clock_now()));

    GlyphAtlasAllocator visibility_atlas{};
    if (!glyph_atlas_initialize(
            &visibility_atlas, 64, 64, 0,
            Slice<GlyphAtlasEntry>{g_visibility_entries, 4'096},
            Slice<GlyphAtlasRow>{g_visibility_rows, 64})) {
        return 96;
    }
    for (u32 index = 0; index < 4'096; ++index) {
        if (!glyph_atlas_allocate_unique(&visibility_atlas, index + 1, 1, 1)
                 .ok()) {
            return 97;
        }
    }
    const i64 visibility_epoch_begin = clock_now();
    for (u32 iteration = 0; iteration < kVisibilityEpochIterations;
         ++iteration) {
        glyph_atlas_clear_pins(&visibility_atlas);
    }
    const u64 visibility_epoch_us = maximum<u64>(
        1, clock_microseconds(clock, visibility_epoch_begin, clock_now()));
    const u64 visibility_epoch_checksum =
        visibility_atlas.visibility_epoch + visibility_atlas.entry_count +
        visibility_atlas.visibility_rebases;

    u64 presentation_policy_checksum = 0;
    const i64 presentation_policy_begin = clock_now();
    for (u32 iteration = 0; iteration < kPresentationPolicyIterations;
         ++iteration) {
        const float width =
            static_cast<float>(64u + (iteration & 255u));
        const MediaPresentationRequest request = media_presentation_request(
            1'600, 900, width, width * 0.5625f,
            (iteration & 1u) != 0 ? 192u : 96u, 512);
        presentation_policy_checksum +=
            request.width + request.height +
            static_cast<u32>(request.resolution);
    }
    const u64 presentation_policy_us = maximum<u64>(
        1, clock_microseconds(clock, presentation_policy_begin,
                              clock_now()));

    if (!exhaustive) {
        write_metric("cdn_target_count", kTargetIterations);
        write_metric("cdn_target_per_second",
                     static_cast<u64>(kTargetIterations) * 1'000'000 /
                         target_us);
        write_metric("attachment_target_parse_count",
                     kAttachmentTargetIterations);
        write_metric("attachment_target_parse_per_second",
                     static_cast<u64>(kAttachmentTargetIterations) *
                         1'000'000 / attachment_target_us);
        write_animation_metrics(animation_result);
        write_metric("media_find_per_second",
                     static_cast<u64>(kFindIterations) * 1'000'000 / find_us);
        write_metric("presentation_damage_operations",
                     kPresentationDamageIterations);
        write_metric("presentation_damage_per_second",
                     static_cast<u64>(kPresentationDamageIterations) *
                         1'000'000 / presentation_damage_us);
        write_metric("visibility_epoch_per_second",
                     static_cast<u64>(kVisibilityEpochIterations) * 1'000'000 /
                         visibility_epoch_us);
        write_metric("presentation_policy_per_second",
                     static_cast<u64>(kPresentationPolicyIterations) *
                         1'000'000 / presentation_policy_us);
        const bool valid =
            target_checksum != 0 && attachment_target_checksum != 0 &&
            animation_result.scan_checksum != 0 &&
            animation_result.advance_checksum != 0 &&
            animation_result.scale_scan_checksum != 0 &&
            animation_result.legacy_scale_scan_checksum != 0 &&
            find_checksum != 0 && churn_checksum != 0 &&
            missing_find_checksum != 0 && presentation_damage_checksum != 0 &&
            visibility_epoch_checksum != 0 &&
            presentation_policy_checksum != 0;
        return valid ? 0 : 100;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL feature_level{};
    HRESULT result =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                          levels, static_cast<UINT>(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
                          &device, &feature_level, &context);
    if (FAILED(result) || feature_level < D3D_FEATURE_LEVEL_11_0) {
        com_release(context);
        com_release(device);
        return 5;
    }
    ID3D11DeviceContext1* context1 = nullptr;
    result = context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1));
    if (FAILED(result)) {
        com_release(context);
        com_release(device);
        return 6;
    }

    media_atlas_reset(&atlas);
    MediaAtlasGpu gpu{};
    if (!media_atlas_gpu_initialize(&gpu, device, context1, kAtlasExtent, kAtlasExtent).ok()) {
        com_release(context1);
        com_release(context);
        com_release(device);
        return 7;
    }
    build_pixels();

    PngFixtureResult png_fixture{};
    PngImage png_image{};
    DeflateWorkspace deflate_workspace{};
    if (!build_rgba8_png_fixture(kAssetExtent, kAssetExtent,
                                 Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                 Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &png_fixture)) {
        return 8;
    }

    u8 request_head[1'024]{};
    u64 request_checksum = 0;
    const i64 request_begin = clock_now();
    for (u32 iteration = 0; iteration < kCdnRequestIterations; ++iteration) {
        const CdnRequestResult written = write_static_png_cdn_get_request(
            CdnHost::cdn, ConstSlice<u8>{reinterpret_cast<const u8*>("/stickers/100.png"), 17},
            ConstSlice<u8>{reinterpret_cast<const u8*>("\"v1\""), 4},
            Slice<u8>{request_head, sizeof(request_head)});
        if (!written.ok()) {
            return 9;
        }
        request_checksum += written.bytes + request_head[iteration & 63u];
    }
    const u64 request_us = maximum<u64>(1, clock_microseconds(clock, request_begin, clock_now()));

    EncodedMediaCache encoded_cache{};
    CdnMediaPipeline media_pipeline{};
    if (!encoded_media_cache_initialize(&encoded_cache, Slice<EncodedMediaCacheEntry>{g_encoded_entries, 8},
                                        Slice<u16>{g_encoded_page_owners, 64},
                                        Slice<u8>{g_encoded_storage, sizeof(g_encoded_storage)}) ||
        !cdn_media_pipeline_initialize(&media_pipeline, &encoded_cache,
                                       Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)})) {
        return 10;
    }
    constexpr char response_head[] = "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: image/png\r\n"
                                     "Transfer-Encoding: chunked\r\n"
                                     "ETag: \"v1\"\r\n"
                                     "Cache-Control: max-age=604800\r\n\r\n";
    const CdnResponseResult response = validate_static_png_response(
        ConstSlice<u8>{reinterpret_cast<const u8*>(response_head), sizeof(response_head) - 1},
        png_fixture.encoded_bytes, 1);
    const MediaAssetKey pipeline_key = media_asset_sticker(100, 1);
    if (!response.ok() ||
        !cdn_media_pipeline_admit_static_png(&media_pipeline, pipeline_key, response,
                                             ConstSlice<u8>{g_png_encoded, png_fixture.encoded_bytes})
             .decoded()) {
        return 11;
    }
    u64 cache_checksum = 0;
    const i64 cache_begin = clock_now();
    for (u32 iteration = 0; iteration < kEncodedCacheFindIterations; ++iteration) {
        const EncodedMediaCacheLookup found = encoded_media_cache_find(&encoded_cache, pipeline_key, 2);
        if (found.state != EncodedMediaCacheLookupState::fresh) {
            return 12;
        }
        cache_checksum += found.bytes.count + found.bytes[iteration & 255u] + found.generation;
    }
    const u64 cache_us = maximum<u64>(1, clock_microseconds(clock, cache_begin, clock_now()));

    u64 pipeline_checksum = 0;
    const i64 pipeline_begin = clock_now();
    for (u32 iteration = 0; iteration < kPipelineAdmitIterations; ++iteration) {
        const CdnStaticPngResult admitted =
            cdn_media_pipeline_admit_static_png(&media_pipeline, pipeline_key, response,
                                                ConstSlice<u8>{g_png_encoded, png_fixture.encoded_bytes});
        if (!admitted.decoded() || !admitted.store.stored()) {
            return 13;
        }
        pipeline_checksum += admitted.image.inflated_bytes + admitted.store.generation;
    }
    const u64 pipeline_us = maximum<u64>(1, clock_microseconds(clock, pipeline_begin, clock_now()));

    u64 png_decode_checksum = 0;
    const i64 png_decode_begin = clock_now();
    for (u32 iteration = 0; iteration < kPngDecodeIterations; ++iteration) {
        const PngDecodeResult decoded =
            png_decode_scanlines(ConstSlice<u8>{g_png_encoded, png_fixture.encoded_bytes}, &png_image,
                                 &deflate_workspace, Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)});
        if (!decoded.ok()) {
            return 14;
        }
        png_decode_checksum +=
            g_png_scanlines[1 + (iteration & 255u)] + decoded.reconstructed_bytes + decoded.inflate.blocks;
    }
    const u64 png_decode_us = maximum<u64>(1, clock_microseconds(clock, png_decode_begin, clock_now()));

    WebpImage webp_image{};
    u64 webp_decode_checksum = 0;
    const i64 webp_decode_begin = clock_now();
    for (u32 iteration = 0; iteration < kWebpDecodeIterations; ++iteration) {
        const WebpDecodeResult decoded =
            webp_lossless_decode_rgba8(ConstSlice<u8>{kWebpGradient, sizeof(kWebpGradient)}, &webp_image,
                                       &g_webp_workspace, Slice<u8>{g_webp_rgba, sizeof(g_webp_rgba)});
        if (!decoded.ok()) {
            return 25;
        }
        webp_decode_checksum += g_webp_rgba[iteration & (sizeof(g_webp_rgba) - 1)] + decoded.rgba_bytes +
                                decoded.huffman_symbols;
    }
    const u64 webp_decode_us = maximum<u64>(1, clock_microseconds(clock, webp_decode_begin, clock_now()));

    WebpImage vp8_image{};
    Vp8Keyframe vp8_frame{};
    Vp8ControlHeader vp8_header{};
    u64 vp8_decode_checksum = 0;
    const i64 vp8_decode_begin = clock_now();
    for (u32 iteration = 0; iteration < kVp8DecodeIterations; ++iteration) {
        const LossyWebpDecodeResult decoded = lossy_webp_decode_keyframe(
            ConstSlice<u8>{kWebpLossy4x4, sizeof(kWebpLossy4x4)}, &vp8_image, &vp8_frame, &vp8_header,
            &g_lossy_webp_workspace.probabilities, &g_lossy_webp_workspace.modes,
            &g_lossy_webp_workspace.coefficients, nullptr, {}, {});
        if (!decoded.ok()) {
            return 53;
        }
        vp8_decode_checksum +=
            static_cast<u16>(g_lossy_webp_workspace.coefficients.macroblocks[0]
                                 .values[iteration % 24u][iteration & 15u]) +
            static_cast<u8>(g_lossy_webp_workspace.modes.macroblocks[0].luma_subblock[iteration & 15u]) +
            decoded.parse.chunks + decoded.alpha_bytes;
    }
    const u64 vp8_decode_us = maximum<u64>(1, clock_microseconds(clock, vp8_decode_begin, clock_now()));

    JpegImage progressive_jpeg_image{};
    u64 progressive_jpeg_decode_checksum = 0;
    const i64 progressive_jpeg_decode_begin = clock_now();
    for (u32 iteration = 0; iteration < kProgressiveJpegDecodeIterations; ++iteration) {
        const JpegDecodeResult decoded = jpeg_decode_coefficients(
            ConstSlice<u8>{reinterpret_cast<const u8*>(kJpegProgressive420Reference),
                           kJpegProgressive420ReferenceBytes},
            &progressive_jpeg_image, &g_jpeg_workspace);
        if (!decoded.ok() || decoded.decoded_scans != 10 || !progressive_jpeg_image.progressive) {
            return 67;
        }
        progressive_jpeg_decode_checksum +=
            static_cast<u16>(
                g_jpeg_workspace.coefficients.blocks[iteration % progressive_jpeg_image.coefficient_blocks]
                    .values[iteration & 63u]) +
            decoded.huffman_symbols + decoded.decoded_blocks + decoded.decoded_scans;
    }
    const u64 progressive_jpeg_decode_us =
        maximum<u64>(1, clock_microseconds(clock, progressive_jpeg_decode_begin, clock_now()));

    JpegImage cmyk_jpeg_image{};
    u64 cmyk_jpeg_decode_checksum = 0;
    const i64 cmyk_jpeg_decode_begin = clock_now();
    for (u32 iteration = 0; iteration < kCmykJpegDecodeIterations; ++iteration) {
        const JpegDecodeResult decoded = jpeg_decode_coefficients(
            ConstSlice<u8>{kJpegCmykAdobeReference, sizeof(kJpegCmykAdobeReference)}, &cmyk_jpeg_image,
            &g_jpeg_workspace);
        if (!decoded.ok() || cmyk_jpeg_image.component_count != 4 ||
            cmyk_jpeg_image.color_transform != JpegColorTransform::cmyk) {
            return 70;
        }
        cmyk_jpeg_decode_checksum +=
            static_cast<u16>(
                g_jpeg_workspace.coefficients.blocks[iteration % cmyk_jpeg_image.coefficient_blocks]
                    .values[iteration & 63u]) +
            decoded.huffman_symbols + decoded.decoded_blocks;
    }
    const u64 cmyk_jpeg_decode_us =
        maximum<u64>(1, clock_microseconds(clock, cmyk_jpeg_decode_begin, clock_now()));

    JpegImage ycck_jpeg_image{};
    u64 ycck_jpeg_decode_checksum = 0;
    const i64 ycck_jpeg_decode_begin = clock_now();
    for (u32 iteration = 0; iteration < kYcckJpegDecodeIterations; ++iteration) {
        const JpegDecodeResult decoded = jpeg_decode_coefficients(
            ConstSlice<u8>{kJpegYcckProgressiveReference, sizeof(kJpegYcckProgressiveReference)},
            &ycck_jpeg_image, &g_jpeg_workspace);
        if (!decoded.ok() || decoded.decoded_scans <= 1 || ycck_jpeg_image.component_count != 4 ||
            ycck_jpeg_image.color_transform != JpegColorTransform::ycck || !ycck_jpeg_image.progressive) {
            return 71;
        }
        ycck_jpeg_decode_checksum +=
            static_cast<u16>(
                g_jpeg_workspace.coefficients.blocks[iteration % ycck_jpeg_image.coefficient_blocks]
                    .values[iteration & 63u]) +
            decoded.huffman_symbols + decoded.decoded_blocks + decoded.decoded_scans;
    }
    const u64 ycck_jpeg_decode_us =
        maximum<u64>(1, clock_microseconds(clock, ycck_jpeg_decode_begin, clock_now()));

    JpegImage jpeg_image{};
    u64 jpeg_decode_checksum = 0;
    const i64 jpeg_decode_begin = clock_now();
    for (u32 iteration = 0; iteration < kJpegDecodeIterations; ++iteration) {
        const JpegDecodeResult decoded = jpeg_decode_coefficients(
            ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)}, &jpeg_image, &g_jpeg_workspace);
        if (!decoded.ok()) {
            return 63;
        }
        jpeg_decode_checksum +=
            static_cast<u16>(g_jpeg_workspace.coefficients.blocks[iteration % jpeg_image.coefficient_blocks]
                                 .values[iteration & 63u]) +
            decoded.huffman_symbols + decoded.decoded_blocks;
    }
    const u64 jpeg_decode_us = maximum<u64>(1, clock_microseconds(clock, jpeg_decode_begin, clock_now()));

    u8 oriented_jpeg_encoded[sizeof(kJpeg420Reference) + 36]{};
    u32 oriented_jpeg_encoded_bytes = 0;
    if (!build_jpeg_exif_orientation_fixture(
            ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)},
            Slice<u8>{oriented_jpeg_encoded, sizeof(oriented_jpeg_encoded)},
            static_cast<u8>(JpegOrientation::right_top), true, &oriented_jpeg_encoded_bytes)) {
        return 76;
    }
    JpegImage oriented_jpeg_image{};
    u64 oriented_jpeg_decode_checksum = 0;
    const i64 oriented_jpeg_decode_begin = clock_now();
    for (u32 iteration = 0; iteration < kOrientedJpegDecodeIterations; ++iteration) {
        const JpegDecodeResult decoded =
            jpeg_decode_coefficients(ConstSlice<u8>{oriented_jpeg_encoded, oriented_jpeg_encoded_bytes},
                                     &oriented_jpeg_image, &g_jpeg_workspace);
        if (!decoded.ok() || oriented_jpeg_image.orientation != JpegOrientation::right_top ||
            jpeg_display_width(oriented_jpeg_image) != 16 ||
            jpeg_display_height(oriented_jpeg_image) != 16) {
            return 77;
        }
        oriented_jpeg_decode_checksum +=
            static_cast<u16>(
                g_jpeg_workspace.coefficients.blocks[iteration % oriented_jpeg_image.coefficient_blocks]
                    .values[iteration & 63u]) +
            decoded.huffman_symbols + decoded.decoded_blocks +
            static_cast<u8>(oriented_jpeg_image.orientation);
    }
    const u64 oriented_jpeg_decode_us =
        maximum<u64>(1, clock_microseconds(clock, oriented_jpeg_decode_begin, clock_now()));

    u8 icc_jpeg_encoded[sizeof(kJpeg420Reference) + 512]{};
    JpegIccFixtureResult icc_jpeg_fixture{};
    if (!build_jpeg_icc_matrix_trc_fixture(ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)},
                                           Slice<u8>{icc_jpeg_encoded, sizeof(icc_jpeg_encoded)},
                                           JpegIccFixtureCurve::parametric_srgb, true, true,
                                           &icc_jpeg_fixture)) {
        return 80;
    }
    JpegImage icc_jpeg_image{};
    u64 icc_jpeg_decode_checksum = 0;
    const i64 icc_jpeg_decode_begin = clock_now();
    for (u32 iteration = 0; iteration < kIccJpegDecodeIterations; ++iteration) {
        const JpegDecodeResult decoded =
            jpeg_decode_coefficients(ConstSlice<u8>{icc_jpeg_encoded, icc_jpeg_fixture.encoded_bytes},
                                     &icc_jpeg_image, &g_jpeg_workspace);
        if (!decoded.ok() || !icc_jpeg_image.tables || !icc_jpeg_image.tables->icc ||
            icc_jpeg_image.tables->icc->profile.disposition != JpegIccDisposition::applied) {
            return 81;
        }
        icc_jpeg_decode_checksum +=
            static_cast<u16>(
                g_jpeg_workspace.coefficients.blocks[iteration % icc_jpeg_image.coefficient_blocks]
                    .values[iteration & 63u]) +
            decoded.huffman_symbols + decoded.decoded_blocks +
            icc_jpeg_image.tables->icc->profile.profile_bytes;
    }
    const u64 icc_jpeg_decode_us =
        maximum<u64>(1, clock_microseconds(clock, icc_jpeg_decode_begin, clock_now()));

    u8 icc_lut_jpeg_encoded[sizeof(kJpeg420Reference) + 1'024]{};
    JpegIccFixtureResult icc_lut_jpeg_fixture{};
    if (!build_jpeg_icc_lut_fixture(ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)},
                                    Slice<u8>{icc_lut_jpeg_encoded, sizeof(icc_lut_jpeg_encoded)},
                                    JpegIccLutFixtureSpace::rgb, JpegIccLutFixturePrecision::mft2, true,
                                    &icc_lut_jpeg_fixture)) {
        return 85;
    }
    JpegImage icc_lut_jpeg_image{};
    u64 icc_lut_jpeg_decode_checksum = 0;
    const i64 icc_lut_jpeg_decode_begin = clock_now();
    for (u32 iteration = 0; iteration < kIccLutJpegDecodeIterations; ++iteration) {
        const JpegDecodeResult decoded = jpeg_decode_coefficients(
            ConstSlice<u8>{icc_lut_jpeg_encoded, icc_lut_jpeg_fixture.encoded_bytes}, &icc_lut_jpeg_image,
            &g_jpeg_workspace);
        if (!decoded.ok() || !icc_lut_jpeg_image.tables || !icc_lut_jpeg_image.tables->icc ||
            icc_lut_jpeg_image.tables->icc->profile.disposition != JpegIccDisposition::applied ||
            icc_lut_jpeg_image.tables->icc->profile.transform_kind != JpegIccTransformKind::lut ||
            icc_lut_jpeg_image.tables->icc->profile.entry_count != 36) {
            return 86;
        }
        icc_lut_jpeg_decode_checksum +=
            static_cast<u16>(
                g_jpeg_workspace.coefficients.blocks[iteration % icc_lut_jpeg_image.coefficient_blocks]
                    .values[iteration & 63u]) +
            decoded.huffman_symbols + decoded.decoded_blocks +
            icc_lut_jpeg_image.tables->icc->profile.entry_count;
    }
    const u64 icc_lut_jpeg_decode_us =
        maximum<u64>(1, clock_microseconds(clock, icc_lut_jpeg_decode_begin, clock_now()));

    D3D11_QUERY_DESC disjoint_description{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC timestamp_description{D3D11_QUERY_TIMESTAMP, 0};
    ID3D11Query* disjoint_query = nullptr;
    ID3D11Query* begin_query = nullptr;
    ID3D11Query* end_query = nullptr;
    result = device->CreateQuery(&disjoint_description, &disjoint_query);
    if (SUCCEEDED(result)) {
        result = device->CreateQuery(&timestamp_description, &begin_query);
    }
    if (SUCCEEDED(result)) {
        result = device->CreateQuery(&timestamp_description, &end_query);
    }
    if (FAILED(result)) {
        return 15;
    }

    const MediaAssetKey upload_key = media_asset_sticker(88, 2);
    const i64 upload_begin = clock_now();
    context->Begin(disjoint_query);
    context->End(begin_query);
    for (u32 iteration = 0; iteration < kUploadIterations; ++iteration) {
        if (!media_atlas_gpu_upload_rgba8(&gpu, &atlas, context1, upload_key, kAssetExtent, kAssetExtent,
                                          kAssetExtent * 4, ConstSlice<u8>{g_pixels, sizeof(g_pixels)})
                 .ok()) {
            return 16;
        }
    }
    context->End(end_query);
    context->End(disjoint_query);
    const u64 upload_cpu_us = clock_microseconds(clock, upload_begin, clock_now());
    context->Flush();

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    u64 gpu_begin = 0;
    u64 gpu_end = 0;
    const bool timed = wait_query(context, disjoint_query, &disjoint, sizeof(disjoint)) &&
                       wait_query(context, begin_query, &gpu_begin, sizeof(gpu_begin)) &&
                       wait_query(context, end_query, &gpu_end, sizeof(gpu_end)) && !disjoint.Disjoint &&
                       disjoint.Frequency != 0 && gpu_end >= gpu_begin;
    const u64 upload_gpu_us =
        timed ? gpu_timestamp_microseconds(gpu_end - gpu_begin, disjoint.Frequency) : 0;

    const MediaAssetKey png_upload_key = media_asset_sticker(89, 1);
    const i64 png_upload_begin = clock_now();
    context->Begin(disjoint_query);
    context->End(begin_query);
    for (u32 iteration = 0; iteration < kPngUploadIterations; ++iteration) {
        if (!media_atlas_gpu_upload_png(&gpu, &atlas, device, context1, png_upload_key, png_image,
                                        ConstSlice<u8>{g_png_scanlines, png_image.inflated_bytes})
                 .ok()) {
            return 18;
        }
    }
    context->End(end_query);
    context->End(disjoint_query);
    const u64 png_upload_cpu_us = clock_microseconds(clock, png_upload_begin, clock_now());
    context->Flush();
    disjoint = D3D11_QUERY_DATA_TIMESTAMP_DISJOINT{};
    gpu_begin = 0;
    gpu_end = 0;
    const bool png_timed = wait_query(context, disjoint_query, &disjoint, sizeof(disjoint)) &&
                           wait_query(context, begin_query, &gpu_begin, sizeof(gpu_begin)) &&
                           wait_query(context, end_query, &gpu_end, sizeof(gpu_end)) &&
                           !disjoint.Disjoint && disjoint.Frequency != 0 && gpu_end >= gpu_begin;
    const u64 png_upload_gpu_us =
        png_timed ? gpu_timestamp_microseconds(gpu_end - gpu_begin, disjoint.Frequency) : 0;

    const MediaAssetKey webp_upload_key = media_asset_custom_emoji(90, false);
    const i64 webp_upload_begin = clock_now();
    context->Begin(disjoint_query);
    context->End(begin_query);
    for (u32 iteration = 0; iteration < kWebpUploadIterations; ++iteration) {
        if (!media_atlas_gpu_upload_straight_rgba8(
                 &gpu, &atlas, device, context1, webp_upload_key, static_cast<u16>(webp_image.width),
                 static_cast<u16>(webp_image.height), webp_image.width * 4,
                 ConstSlice<u8>{g_webp_rgba, sizeof(g_webp_rgba)})
                 .ok()) {
            return 26;
        }
    }
    context->End(end_query);
    context->End(disjoint_query);
    const u64 webp_upload_cpu_us = clock_microseconds(clock, webp_upload_begin, clock_now());
    context->Flush();
    disjoint = D3D11_QUERY_DATA_TIMESTAMP_DISJOINT{};
    gpu_begin = 0;
    gpu_end = 0;
    const bool webp_timed = wait_query(context, disjoint_query, &disjoint, sizeof(disjoint)) &&
                            wait_query(context, begin_query, &gpu_begin, sizeof(gpu_begin)) &&
                            wait_query(context, end_query, &gpu_end, sizeof(gpu_end)) &&
                            !disjoint.Disjoint && disjoint.Frequency != 0 && gpu_end >= gpu_begin;
    const u64 webp_upload_gpu_us =
        webp_timed ? gpu_timestamp_microseconds(gpu_end - gpu_begin, disjoint.Frequency) : 0;

    const MediaAssetKey vp8_upload_key = media_asset_custom_emoji(94, false);
    const i64 vp8_upload_begin = clock_now();
    context->Begin(disjoint_query);
    context->End(begin_query);
    for (u32 iteration = 0; iteration < kVp8UploadIterations; ++iteration) {
        if (!media_atlas_gpu_reconstruct_vp8_keyframe(&gpu, device, context1, vp8_frame,
                                                      g_lossy_webp_workspace.modes,
                                                      g_lossy_webp_workspace.coefficients)
                 .ok() ||
            !media_atlas_gpu_filter_vp8_keyframe(&gpu, device, context1, vp8_frame, vp8_header,
                                                 g_lossy_webp_workspace.modes,
                                                 g_lossy_webp_workspace.coefficients)
                 .ok() ||
            !media_atlas_gpu_upload_vp8_keyframe(&gpu, &atlas, device, context1, vp8_upload_key, vp8_frame,
                                                 {})
                 .ok()) {
            return 54;
        }
    }
    context->End(end_query);
    context->End(disjoint_query);
    const u64 vp8_upload_cpu_us = clock_microseconds(clock, vp8_upload_begin, clock_now());
    context->Flush();
    disjoint = D3D11_QUERY_DATA_TIMESTAMP_DISJOINT{};
    gpu_begin = 0;
    gpu_end = 0;
    const bool vp8_timed = wait_query(context, disjoint_query, &disjoint, sizeof(disjoint)) &&
                           wait_query(context, begin_query, &gpu_begin, sizeof(gpu_begin)) &&
                           wait_query(context, end_query, &gpu_end, sizeof(gpu_end)) &&
                           !disjoint.Disjoint && disjoint.Frequency != 0 && gpu_end >= gpu_begin;
    const u64 vp8_upload_gpu_us =
        vp8_timed ? gpu_timestamp_microseconds(gpu_end - gpu_begin, disjoint.Frequency) : 0;
    const u32 vp8_static_reconstructions = gpu.vp8_reconstructions;
    const u32 vp8_static_reconstruction_dispatches =
        gpu.vp8_reconstruction_dispatches;
    const u32 vp8_static_filter_dispatches = gpu.vp8_filter_dispatches;
    const u32 vp8_static_atlas_uploads = gpu.vp8_atlas_uploads;
    const u64 vp8_static_uploaded_bytes = gpu.vp8_uploaded_bytes;
    const u64 vp8_static_reconstructed_pixels = gpu.vp8_reconstructed_pixels;

    const JpegDecodeResult jpeg_for_upload = jpeg_decode_coefficients(
        ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)}, &jpeg_image, &g_jpeg_workspace);
    if (!jpeg_for_upload.ok()) {
        return 82;
    }
    const MediaAssetKey jpeg_upload_key = media_asset_attachment(96);
    const i64 jpeg_upload_begin = clock_now();
    context->Begin(disjoint_query);
    context->End(begin_query);
    for (u32 iteration = 0; iteration < kJpegUploadIterations; ++iteration) {
        if (!media_atlas_gpu_upload_jpeg(&gpu, &atlas, device, context1, jpeg_upload_key, jpeg_image,
                                         g_jpeg_workspace.coefficients)
                 .ok()) {
            return 64;
        }
    }
    context->End(end_query);
    context->End(disjoint_query);
    const u64 jpeg_upload_cpu_us = clock_microseconds(clock, jpeg_upload_begin, clock_now());
    context->Flush();
    disjoint = D3D11_QUERY_DATA_TIMESTAMP_DISJOINT{};
    gpu_begin = 0;
    gpu_end = 0;
    const bool jpeg_timed = wait_query(context, disjoint_query, &disjoint, sizeof(disjoint)) &&
                            wait_query(context, begin_query, &gpu_begin, sizeof(gpu_begin)) &&
                            wait_query(context, end_query, &gpu_end, sizeof(gpu_end)) &&
                            !disjoint.Disjoint && disjoint.Frequency != 0 && gpu_end >= gpu_begin;
    const u64 jpeg_upload_gpu_us =
        jpeg_timed ? gpu_timestamp_microseconds(gpu_end - gpu_begin, disjoint.Frequency) : 0;
    const u64 jpeg_uploaded_bytes = gpu.jpeg_uploaded_bytes;
    const u64 jpeg_reconstructed_pixels = gpu.jpeg_reconstructed_pixels;
    const u32 jpeg_uploads = gpu.jpeg_uploads;
    const u32 jpeg_idct_dispatches = gpu.jpeg_idct_dispatches;
    const u32 jpeg_atlas_dispatches = gpu.jpeg_atlas_dispatches;

    const JpegDecodeResult progressive_jpeg_for_upload =
        jpeg_decode_coefficients(ConstSlice<u8>{reinterpret_cast<const u8*>(kJpegProgressive420Reference),
                                                kJpegProgressive420ReferenceBytes},
                                 &progressive_jpeg_image, &g_jpeg_workspace);
    if (!progressive_jpeg_for_upload.ok() || progressive_jpeg_for_upload.decoded_scans != 10) {
        return 68;
    }
    const MediaAssetKey progressive_jpeg_upload_key = media_asset_attachment(97);
    const i64 progressive_jpeg_upload_begin = clock_now();
    context->Begin(disjoint_query);
    context->End(begin_query);
    for (u32 iteration = 0; iteration < kProgressiveJpegUploadIterations; ++iteration) {
        if (!media_atlas_gpu_upload_jpeg(&gpu, &atlas, device, context1, progressive_jpeg_upload_key,
                                         progressive_jpeg_image, g_jpeg_workspace.coefficients)
                 .ok()) {
            return 69;
        }
    }
    context->End(end_query);
    context->End(disjoint_query);
    const u64 progressive_jpeg_upload_cpu_us =
        clock_microseconds(clock, progressive_jpeg_upload_begin, clock_now());
    context->Flush();
    disjoint = D3D11_QUERY_DATA_TIMESTAMP_DISJOINT{};
    gpu_begin = 0;
    gpu_end = 0;
    const bool progressive_jpeg_timed = wait_query(context, disjoint_query, &disjoint, sizeof(disjoint)) &&
                                        wait_query(context, begin_query, &gpu_begin, sizeof(gpu_begin)) &&
                                        wait_query(context, end_query, &gpu_end, sizeof(gpu_end)) &&
                                        !disjoint.Disjoint && disjoint.Frequency != 0 &&
                                        gpu_end >= gpu_begin;
    const u64 progressive_jpeg_upload_gpu_us =
        progressive_jpeg_timed ? gpu_timestamp_microseconds(gpu_end - gpu_begin, disjoint.Frequency) : 0;
    const u64 progressive_jpeg_uploaded_bytes = gpu.jpeg_uploaded_bytes - jpeg_uploaded_bytes;
    const u64 progressive_jpeg_reconstructed_pixels =
        gpu.jpeg_reconstructed_pixels - jpeg_reconstructed_pixels;
    const u32 progressive_jpeg_uploads = gpu.jpeg_uploads - jpeg_uploads;
    const u32 progressive_jpeg_idct_dispatches = gpu.jpeg_idct_dispatches - jpeg_idct_dispatches;
    const u32 progressive_jpeg_atlas_dispatches = gpu.jpeg_atlas_dispatches - jpeg_atlas_dispatches;

    const JpegDecodeResult cmyk_jpeg_for_upload =
        jpeg_decode_coefficients(ConstSlice<u8>{kJpegCmykAdobeReference, sizeof(kJpegCmykAdobeReference)},
                                 &cmyk_jpeg_image, &g_jpeg_workspace);
    if (!cmyk_jpeg_for_upload.ok()) {
        return 72;
    }
    const u64 before_cmyk_jpeg_uploaded_bytes = gpu.jpeg_uploaded_bytes;
    const u64 before_cmyk_jpeg_reconstructed_pixels = gpu.jpeg_reconstructed_pixels;
    const u32 before_cmyk_jpeg_uploads = gpu.jpeg_uploads;
    const u32 before_cmyk_jpeg_idct_dispatches = gpu.jpeg_idct_dispatches;
    const u32 before_cmyk_jpeg_atlas_dispatches = gpu.jpeg_atlas_dispatches;
    const MediaAssetKey cmyk_jpeg_upload_key = media_asset_attachment(98);
    const i64 cmyk_jpeg_upload_begin = clock_now();
    context->Begin(disjoint_query);
    context->End(begin_query);
    for (u32 iteration = 0; iteration < kCmykJpegUploadIterations; ++iteration) {
        if (!media_atlas_gpu_upload_jpeg(&gpu, &atlas, device, context1, cmyk_jpeg_upload_key,
                                         cmyk_jpeg_image, g_jpeg_workspace.coefficients)
                 .ok()) {
            return 73;
        }
    }
    context->End(end_query);
    context->End(disjoint_query);
    const u64 cmyk_jpeg_upload_cpu_us = clock_microseconds(clock, cmyk_jpeg_upload_begin, clock_now());
    context->Flush();
    disjoint = D3D11_QUERY_DATA_TIMESTAMP_DISJOINT{};
    gpu_begin = 0;
    gpu_end = 0;
    const bool cmyk_jpeg_timed = wait_query(context, disjoint_query, &disjoint, sizeof(disjoint)) &&
                                 wait_query(context, begin_query, &gpu_begin, sizeof(gpu_begin)) &&
                                 wait_query(context, end_query, &gpu_end, sizeof(gpu_end)) &&
                                 !disjoint.Disjoint && disjoint.Frequency != 0 && gpu_end >= gpu_begin;
    const u64 cmyk_jpeg_upload_gpu_us =
        cmyk_jpeg_timed ? gpu_timestamp_microseconds(gpu_end - gpu_begin, disjoint.Frequency) : 0;
    const u64 cmyk_jpeg_uploaded_bytes = gpu.jpeg_uploaded_bytes - before_cmyk_jpeg_uploaded_bytes;
    const u64 cmyk_jpeg_reconstructed_pixels =
        gpu.jpeg_reconstructed_pixels - before_cmyk_jpeg_reconstructed_pixels;
    const u32 cmyk_jpeg_uploads = gpu.jpeg_uploads - before_cmyk_jpeg_uploads;
    const u32 cmyk_jpeg_idct_dispatches = gpu.jpeg_idct_dispatches - before_cmyk_jpeg_idct_dispatches;
    const u32 cmyk_jpeg_atlas_dispatches = gpu.jpeg_atlas_dispatches - before_cmyk_jpeg_atlas_dispatches;

    const JpegDecodeResult ycck_jpeg_for_upload = jpeg_decode_coefficients(
        ConstSlice<u8>{kJpegYcckProgressiveReference, sizeof(kJpegYcckProgressiveReference)},
        &ycck_jpeg_image, &g_jpeg_workspace);
    if (!ycck_jpeg_for_upload.ok() || ycck_jpeg_for_upload.decoded_scans <= 1) {
        return 74;
    }
    const u64 before_ycck_jpeg_uploaded_bytes = gpu.jpeg_uploaded_bytes;
    const u64 before_ycck_jpeg_reconstructed_pixels = gpu.jpeg_reconstructed_pixels;
    const u32 before_ycck_jpeg_uploads = gpu.jpeg_uploads;
    const u32 before_ycck_jpeg_idct_dispatches = gpu.jpeg_idct_dispatches;
    const u32 before_ycck_jpeg_atlas_dispatches = gpu.jpeg_atlas_dispatches;
    const MediaAssetKey ycck_jpeg_upload_key = media_asset_attachment(99);
    const i64 ycck_jpeg_upload_begin = clock_now();
    context->Begin(disjoint_query);
    context->End(begin_query);
    for (u32 iteration = 0; iteration < kYcckJpegUploadIterations; ++iteration) {
        if (!media_atlas_gpu_upload_jpeg(&gpu, &atlas, device, context1, ycck_jpeg_upload_key,
                                         ycck_jpeg_image, g_jpeg_workspace.coefficients)
                 .ok()) {
            return 75;
        }
    }
    context->End(end_query);
    context->End(disjoint_query);
    const u64 ycck_jpeg_upload_cpu_us = clock_microseconds(clock, ycck_jpeg_upload_begin, clock_now());
    context->Flush();
    disjoint = D3D11_QUERY_DATA_TIMESTAMP_DISJOINT{};
    gpu_begin = 0;
    gpu_end = 0;
    const bool ycck_jpeg_timed = wait_query(context, disjoint_query, &disjoint, sizeof(disjoint)) &&
                                 wait_query(context, begin_query, &gpu_begin, sizeof(gpu_begin)) &&
                                 wait_query(context, end_query, &gpu_end, sizeof(gpu_end)) &&
                                 !disjoint.Disjoint && disjoint.Frequency != 0 && gpu_end >= gpu_begin;
    const u64 ycck_jpeg_upload_gpu_us =
        ycck_jpeg_timed ? gpu_timestamp_microseconds(gpu_end - gpu_begin, disjoint.Frequency) : 0;
    const u64 ycck_jpeg_uploaded_bytes = gpu.jpeg_uploaded_bytes - before_ycck_jpeg_uploaded_bytes;
    const u64 ycck_jpeg_reconstructed_pixels =
        gpu.jpeg_reconstructed_pixels - before_ycck_jpeg_reconstructed_pixels;
    const u32 ycck_jpeg_uploads = gpu.jpeg_uploads - before_ycck_jpeg_uploads;
    const u32 ycck_jpeg_idct_dispatches = gpu.jpeg_idct_dispatches - before_ycck_jpeg_idct_dispatches;
    const u32 ycck_jpeg_atlas_dispatches = gpu.jpeg_atlas_dispatches - before_ycck_jpeg_atlas_dispatches;

    const JpegDecodeResult oriented_jpeg_for_upload =
        jpeg_decode_coefficients(ConstSlice<u8>{oriented_jpeg_encoded, oriented_jpeg_encoded_bytes},
                                 &oriented_jpeg_image, &g_jpeg_workspace);
    if (!oriented_jpeg_for_upload.ok() || oriented_jpeg_image.orientation != JpegOrientation::right_top) {
        return 78;
    }
    const u64 before_oriented_jpeg_uploaded_bytes = gpu.jpeg_uploaded_bytes;
    const u64 before_oriented_jpeg_reconstructed_pixels = gpu.jpeg_reconstructed_pixels;
    const u32 before_oriented_jpeg_uploads = gpu.jpeg_uploads;
    const u32 before_oriented_jpeg_idct_dispatches = gpu.jpeg_idct_dispatches;
    const u32 before_oriented_jpeg_atlas_dispatches = gpu.jpeg_atlas_dispatches;
    const MediaAssetKey oriented_jpeg_upload_key = media_asset_attachment(100);
    const i64 oriented_jpeg_upload_begin = clock_now();
    context->Begin(disjoint_query);
    context->End(begin_query);
    for (u32 iteration = 0; iteration < kOrientedJpegUploadIterations; ++iteration) {
        if (!media_atlas_gpu_upload_jpeg(&gpu, &atlas, device, context1, oriented_jpeg_upload_key,
                                         oriented_jpeg_image, g_jpeg_workspace.coefficients)
                 .ok()) {
            return 79;
        }
    }
    context->End(end_query);
    context->End(disjoint_query);
    const u64 oriented_jpeg_upload_cpu_us =
        clock_microseconds(clock, oriented_jpeg_upload_begin, clock_now());
    context->Flush();
    disjoint = D3D11_QUERY_DATA_TIMESTAMP_DISJOINT{};
    gpu_begin = 0;
    gpu_end = 0;
    const bool oriented_jpeg_timed = wait_query(context, disjoint_query, &disjoint, sizeof(disjoint)) &&
                                     wait_query(context, begin_query, &gpu_begin, sizeof(gpu_begin)) &&
                                     wait_query(context, end_query, &gpu_end, sizeof(gpu_end)) &&
                                     !disjoint.Disjoint && disjoint.Frequency != 0 && gpu_end >= gpu_begin;
    const u64 oriented_jpeg_upload_gpu_us =
        oriented_jpeg_timed ? gpu_timestamp_microseconds(gpu_end - gpu_begin, disjoint.Frequency) : 0;
    const u64 oriented_jpeg_uploaded_bytes = gpu.jpeg_uploaded_bytes - before_oriented_jpeg_uploaded_bytes;
    const u64 oriented_jpeg_reconstructed_pixels =
        gpu.jpeg_reconstructed_pixels - before_oriented_jpeg_reconstructed_pixels;
    const u32 oriented_jpeg_uploads = gpu.jpeg_uploads - before_oriented_jpeg_uploads;
    const u32 oriented_jpeg_idct_dispatches =
        gpu.jpeg_idct_dispatches - before_oriented_jpeg_idct_dispatches;
    const u32 oriented_jpeg_atlas_dispatches =
        gpu.jpeg_atlas_dispatches - before_oriented_jpeg_atlas_dispatches;

    const JpegDecodeResult icc_jpeg_for_upload =
        jpeg_decode_coefficients(ConstSlice<u8>{icc_jpeg_encoded, icc_jpeg_fixture.encoded_bytes},
                                 &icc_jpeg_image, &g_jpeg_workspace);
    if (!icc_jpeg_for_upload.ok() || !icc_jpeg_image.tables || !icc_jpeg_image.tables->icc ||
        icc_jpeg_image.tables->icc->profile.disposition != JpegIccDisposition::applied) {
        return 83;
    }
    const u64 before_icc_jpeg_uploaded_bytes = gpu.jpeg_uploaded_bytes;
    const u64 before_icc_jpeg_reconstructed_pixels = gpu.jpeg_reconstructed_pixels;
    const u32 before_icc_jpeg_uploads = gpu.jpeg_uploads;
    const u32 before_icc_jpeg_idct_dispatches = gpu.jpeg_idct_dispatches;
    const u32 before_icc_jpeg_atlas_dispatches = gpu.jpeg_atlas_dispatches;
    const MediaAssetKey icc_jpeg_upload_key = media_asset_attachment(101);
    const i64 icc_jpeg_upload_begin = clock_now();
    context->Begin(disjoint_query);
    context->End(begin_query);
    for (u32 iteration = 0; iteration < kIccJpegUploadIterations; ++iteration) {
        if (!media_atlas_gpu_upload_jpeg(&gpu, &atlas, device, context1, icc_jpeg_upload_key,
                                         icc_jpeg_image, g_jpeg_workspace.coefficients)
                 .ok()) {
            return 84;
        }
    }
    context->End(end_query);
    context->End(disjoint_query);
    const u64 icc_jpeg_upload_cpu_us = clock_microseconds(clock, icc_jpeg_upload_begin, clock_now());
    context->Flush();
    disjoint = D3D11_QUERY_DATA_TIMESTAMP_DISJOINT{};
    gpu_begin = 0;
    gpu_end = 0;
    const bool icc_jpeg_timed = wait_query(context, disjoint_query, &disjoint, sizeof(disjoint)) &&
                                wait_query(context, begin_query, &gpu_begin, sizeof(gpu_begin)) &&
                                wait_query(context, end_query, &gpu_end, sizeof(gpu_end)) &&
                                !disjoint.Disjoint && disjoint.Frequency != 0 && gpu_end >= gpu_begin;
    const u64 icc_jpeg_upload_gpu_us =
        icc_jpeg_timed ? gpu_timestamp_microseconds(gpu_end - gpu_begin, disjoint.Frequency) : 0;
    const u64 icc_jpeg_uploaded_bytes = gpu.jpeg_uploaded_bytes - before_icc_jpeg_uploaded_bytes;
    const u64 icc_jpeg_reconstructed_pixels =
        gpu.jpeg_reconstructed_pixels - before_icc_jpeg_reconstructed_pixels;
    const u32 icc_jpeg_uploads = gpu.jpeg_uploads - before_icc_jpeg_uploads;
    const u32 icc_jpeg_idct_dispatches = gpu.jpeg_idct_dispatches - before_icc_jpeg_idct_dispatches;
    const u32 icc_jpeg_atlas_dispatches = gpu.jpeg_atlas_dispatches - before_icc_jpeg_atlas_dispatches;

    const JpegDecodeResult icc_lut_jpeg_for_upload =
        jpeg_decode_coefficients(ConstSlice<u8>{icc_lut_jpeg_encoded, icc_lut_jpeg_fixture.encoded_bytes},
                                 &icc_lut_jpeg_image, &g_jpeg_workspace);
    if (!icc_lut_jpeg_for_upload.ok() || !icc_lut_jpeg_image.tables || !icc_lut_jpeg_image.tables->icc ||
        icc_lut_jpeg_image.tables->icc->profile.disposition != JpegIccDisposition::applied ||
        icc_lut_jpeg_image.tables->icc->profile.transform_kind != JpegIccTransformKind::lut) {
        return 87;
    }
    const u64 before_icc_lut_jpeg_uploaded_bytes = gpu.jpeg_uploaded_bytes;
    const u64 before_icc_lut_jpeg_reconstructed_pixels = gpu.jpeg_reconstructed_pixels;
    const u32 before_icc_lut_jpeg_uploads = gpu.jpeg_uploads;
    const u32 before_icc_lut_jpeg_idct_dispatches = gpu.jpeg_idct_dispatches;
    const u32 before_icc_lut_jpeg_atlas_dispatches = gpu.jpeg_atlas_dispatches;
    const MediaAssetKey icc_lut_jpeg_upload_key = media_asset_attachment(102);
    const i64 icc_lut_jpeg_upload_begin = clock_now();
    context->Begin(disjoint_query);
    context->End(begin_query);
    for (u32 iteration = 0; iteration < kIccLutJpegUploadIterations; ++iteration) {
        if (!media_atlas_gpu_upload_jpeg(&gpu, &atlas, device, context1, icc_lut_jpeg_upload_key,
                                         icc_lut_jpeg_image, g_jpeg_workspace.coefficients)
                 .ok()) {
            return 88;
        }
    }
    context->End(end_query);
    context->End(disjoint_query);
    const u64 icc_lut_jpeg_upload_cpu_us =
        clock_microseconds(clock, icc_lut_jpeg_upload_begin, clock_now());
    context->Flush();
    disjoint = D3D11_QUERY_DATA_TIMESTAMP_DISJOINT{};
    gpu_begin = 0;
    gpu_end = 0;
    const bool icc_lut_jpeg_timed = wait_query(context, disjoint_query, &disjoint, sizeof(disjoint)) &&
                                    wait_query(context, begin_query, &gpu_begin, sizeof(gpu_begin)) &&
                                    wait_query(context, end_query, &gpu_end, sizeof(gpu_end)) &&
                                    !disjoint.Disjoint && disjoint.Frequency != 0 && gpu_end >= gpu_begin;
    const u64 icc_lut_jpeg_upload_gpu_us =
        icc_lut_jpeg_timed ? gpu_timestamp_microseconds(gpu_end - gpu_begin, disjoint.Frequency) : 0;
    const u64 icc_lut_jpeg_uploaded_bytes = gpu.jpeg_uploaded_bytes - before_icc_lut_jpeg_uploaded_bytes;
    const u64 icc_lut_jpeg_reconstructed_pixels =
        gpu.jpeg_reconstructed_pixels - before_icc_lut_jpeg_reconstructed_pixels;
    const u32 icc_lut_jpeg_uploads = gpu.jpeg_uploads - before_icc_lut_jpeg_uploads;
    const u32 icc_lut_jpeg_idct_dispatches = gpu.jpeg_idct_dispatches - before_icc_lut_jpeg_idct_dispatches;
    const u32 icc_lut_jpeg_atlas_dispatches =
        gpu.jpeg_atlas_dispatches - before_icc_lut_jpeg_atlas_dispatches;

    struct FormatCase {
        u8 color_type;
        u8 bit_depth;
    };
    constexpr FormatCase formats[] = {
        {0, 1}, {0, 2}, {0, 4}, {0, 8}, {0, 16}, {2, 8}, {2, 16}, {3, 1},
        {3, 2}, {3, 4}, {3, 8}, {4, 8}, {4, 16}, {6, 8}, {6, 16},
    };
    constexpr u32 format_count = static_cast<u32>(sizeof(formats) / sizeof(formats[0]));
    const u32 timed_png_inflated_bytes = png_image.inflated_bytes;
    u64 format_uploaded_bytes = 0;
    for (u32 index = 0; index < format_count; ++index) {
        PngFixtureResult format_fixture{};
        if (!build_png_format_fixture(formats[index].color_type, formats[index].bit_depth, 9, 3,
                                      Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                      Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &format_fixture) ||
            !png_decode_scanlines(ConstSlice<u8>{g_png_encoded, format_fixture.encoded_bytes}, &png_image,
                                  &deflate_workspace, Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)})
                 .ok() ||
            !media_atlas_gpu_upload_png(&gpu, &atlas, device, context1, media_asset_attachment(200 + index),
                                        png_image,
                                        ConstSlice<u8>{g_png_scanlines, png_image.inflated_bytes})
                 .ok()) {
            return 20;
        }
        format_uploaded_bytes += png_image.inflated_bytes;
    }

    ApngFixtureResult apng_fixture{};
    if (!build_rgba8_apng_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                  Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &apng_fixture)) {
        return 32;
    }
    u64 apng_decode_checksum = 0;
    const i64 apng_decode_begin = clock_now();
    for (u32 iteration = 0; iteration < kApngDecodeIterations; ++iteration) {
        PngImage frame_image{};
        ApngImage decoded_animation{};
        const ApngDecodeResult decoded =
            apng_decode_frame(ConstSlice<u8>{g_png_encoded, apng_fixture.encoded_bytes}, iteration & 3u,
                              &decoded_animation, &g_apng_workspace, &deflate_workspace, &frame_image,
                              Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)});
        if (!decoded.ok()) {
            return 33;
        }
        apng_decode_checksum += decoded.duration_us + decoded.parse.sequence_chunks + g_png_scanlines[1];
    }
    const u64 apng_decode_us = maximum<u64>(1, clock_microseconds(clock, apng_decode_begin, clock_now()));

    ApngImage apng_image{};
    if (!apng_parse(ConstSlice<u8>{g_png_encoded, apng_fixture.encoded_bytes}, &apng_image,
                    &g_apng_workspace)
             .ok()) {
        return 34;
    }
    for (u32 frame_index = 0; frame_index < 4; ++frame_index) {
        const ApngDecodeResult decoded = apng_decode_parsed_frame(
            ConstSlice<u8>{g_png_encoded, apng_fixture.encoded_bytes}, apng_image, frame_index,
            &deflate_workspace, &g_apng_frame_images[frame_index],
            Slice<u8>{g_apng_frame_scanlines[frame_index], sizeof(g_apng_frame_scanlines[frame_index])});
        if (!decoded.ok()) {
            return 35;
        }
        g_apng_frames[frame_index] = decoded.frame;
    }
    const MediaAssetKey apng_upload_key = media_asset_sticker(91, 2);
    const i64 apng_upload_begin = clock_now();
    context->Begin(disjoint_query);
    context->End(begin_query);
    for (u32 iteration = 0; iteration < kApngSequenceIterations; ++iteration) {
        if (!media_atlas_gpu_begin_apng(
                 &gpu, &atlas, device, context1, apng_upload_key, 4, 4, g_apng_frames[0],
                 g_apng_frame_images[0],
                 ConstSlice<u8>{g_apng_frame_scanlines[0], g_apng_frame_images[0].inflated_bytes})
                 .ok()) {
            return 36;
        }
        for (u32 frame_index = 1; frame_index < 4; ++frame_index) {
            if (!media_atlas_gpu_advance_apng(
                     &gpu, &atlas, device, context1, apng_upload_key, g_apng_frames[frame_index - 1],
                     g_apng_frames[frame_index], g_apng_frame_images[frame_index],
                     ConstSlice<u8>{g_apng_frame_scanlines[frame_index],
                                    g_apng_frame_images[frame_index].inflated_bytes})
                     .ok()) {
                return 37;
            }
        }
    }
    context->End(end_query);
    context->End(disjoint_query);
    const u64 apng_upload_cpu_us = clock_microseconds(clock, apng_upload_begin, clock_now());
    context->Flush();
    disjoint = D3D11_QUERY_DATA_TIMESTAMP_DISJOINT{};
    gpu_begin = 0;
    gpu_end = 0;
    const bool apng_timed = wait_query(context, disjoint_query, &disjoint, sizeof(disjoint)) &&
                            wait_query(context, begin_query, &gpu_begin, sizeof(gpu_begin)) &&
                            wait_query(context, end_query, &gpu_end, sizeof(gpu_end)) &&
                            !disjoint.Disjoint && disjoint.Frequency != 0 && gpu_end >= gpu_begin;
    const u64 apng_upload_gpu_us =
        apng_timed ? gpu_timestamp_microseconds(gpu_end - gpu_begin, disjoint.Frequency) : 0;

    GifFixtureResult gif_fixture{};
    if (!build_indexed_gif_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                   Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &gif_fixture)) {
        return 39;
    }
    u64 gif_decode_checksum = 0;
    const i64 gif_decode_begin = clock_now();
    for (u32 iteration = 0; iteration < kGifDecodeIterations; ++iteration) {
        GifImage decoded_animation{};
        const GifDecodeResult decoded = gif_decode_frame(
            ConstSlice<u8>{g_png_encoded, gif_fixture.encoded_bytes}, iteration & 3u, &decoded_animation,
            &g_gif_workspace, Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)});
        if (!decoded.ok()) {
            return 40;
        }
        gif_decode_checksum += decoded.duration_us + decoded.parse.data_subblocks + g_png_scanlines[0];
    }
    const u64 gif_decode_us = maximum<u64>(1, clock_microseconds(clock, gif_decode_begin, clock_now()));
    GifImage gif_image{};
    if (!gif_parse(ConstSlice<u8>{g_png_encoded, gif_fixture.encoded_bytes}, &gif_image, &g_gif_workspace)
             .ok()) {
        return 41;
    }
    for (u32 frame_index = 0; frame_index < 4; ++frame_index) {
        const GifDecodeResult decoded = gif_decode_parsed_frame(
            ConstSlice<u8>{g_png_encoded, gif_fixture.encoded_bytes}, gif_image, frame_index,
            &g_gif_workspace,
            Slice<u8>{g_gif_frame_indices[frame_index], sizeof(g_gif_frame_indices[frame_index])});
        if (!decoded.ok()) {
            return 42;
        }
        g_gif_frames[frame_index] = decoded.frame;
        g_gif_palettes[frame_index] = decoded.palette;
    }
    const MediaAssetKey gif_upload_key = media_asset_sticker(92, 4);
    const i64 gif_upload_begin = clock_now();
    context->Begin(disjoint_query);
    context->End(begin_query);
    for (u32 iteration = 0; iteration < kGifSequenceIterations; ++iteration) {
        if (!media_atlas_gpu_begin_gif(&gpu, &atlas, device, context1, gif_upload_key, 4, 4,
                                       g_gif_frames[0], g_gif_palettes[0],
                                       ConstSlice<u8>{g_gif_frame_indices[0], 16})
                 .ok()) {
            return 43;
        }
        for (u32 frame_index = 1; frame_index < 4; ++frame_index) {
            if (!media_atlas_gpu_advance_gif(
                     &gpu, &atlas, device, context1, gif_upload_key, g_gif_frames[frame_index - 1],
                     g_gif_frames[frame_index], g_gif_palettes[frame_index],
                     ConstSlice<u8>{g_gif_frame_indices[frame_index],
                                    g_gif_frames[frame_index].width * g_gif_frames[frame_index].height})
                     .ok()) {
                return 44;
            }
        }
    }
    context->End(end_query);
    context->End(disjoint_query);
    const u64 gif_upload_cpu_us = clock_microseconds(clock, gif_upload_begin, clock_now());
    context->Flush();
    disjoint = D3D11_QUERY_DATA_TIMESTAMP_DISJOINT{};
    gpu_begin = 0;
    gpu_end = 0;
    const bool gif_timed = wait_query(context, disjoint_query, &disjoint, sizeof(disjoint)) &&
                           wait_query(context, begin_query, &gpu_begin, sizeof(gpu_begin)) &&
                           wait_query(context, end_query, &gpu_end, sizeof(gpu_end)) &&
                           !disjoint.Disjoint && disjoint.Frequency != 0 && gpu_end >= gpu_begin;
    const u64 gif_upload_gpu_us =
        gif_timed ? gpu_timestamp_microseconds(gpu_end - gpu_begin, disjoint.Frequency) : 0;

    AnimatedWebpFixtureResult animated_webp_fixture{};
    if (!build_lossless_animated_webp_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                              &animated_webp_fixture)) {
        return 46;
    }
    u64 animated_webp_decode_checksum = 0;
    const i64 animated_webp_decode_begin = clock_now();
    for (u32 iteration = 0; iteration < kAnimatedWebpDecodeIterations; ++iteration) {
        AnimatedWebpImage decoded_animation{};
        const AnimatedWebpDecodeResult decoded = animated_webp_decode_frame_rgba8(
            ConstSlice<u8>{g_png_encoded, animated_webp_fixture.encoded_bytes}, iteration & 3u,
            &decoded_animation, &g_animated_webp_workspace, &g_webp_workspace,
            Slice<u8>{g_png_scanlines, 64});
        if (!decoded.ok()) {
            return 47;
        }
        animated_webp_decode_checksum += decoded.duration_us + decoded.parse.chunks + g_png_scanlines[0];
    }
    const u64 animated_webp_decode_us =
        maximum<u64>(1, clock_microseconds(clock, animated_webp_decode_begin, clock_now()));
    AnimatedWebpImage animated_webp_image{};
    if (!animated_webp_parse(ConstSlice<u8>{g_png_encoded, animated_webp_fixture.encoded_bytes},
                             &animated_webp_image, &g_animated_webp_workspace)
             .ok()) {
        return 48;
    }
    for (u32 frame_index = 0; frame_index < 4; ++frame_index) {
        const AnimatedWebpDecodeResult decoded = animated_webp_decode_frame_rgba8(
            ConstSlice<u8>{g_png_encoded, animated_webp_fixture.encoded_bytes}, frame_index,
            &animated_webp_image, &g_animated_webp_workspace, &g_webp_workspace,
            Slice<u8>{g_animated_webp_rgba[frame_index], 64});
        if (!decoded.ok()) {
            return 49;
        }
        g_animated_webp_frames[frame_index] = decoded.frame;
        g_animated_webp_frame_bytes[frame_index] = decoded.frame_decode.rgba_bytes;
    }
    const MediaAssetKey animated_webp_upload_key = media_asset_custom_emoji(93, true);
    const i64 animated_webp_upload_begin = clock_now();
    context->Begin(disjoint_query);
    context->End(begin_query);
    for (u32 iteration = 0; iteration < kAnimatedWebpSequenceIterations; ++iteration) {
        if (!media_atlas_gpu_begin_animated_webp(
                 &gpu, &atlas, device, context1, animated_webp_upload_key, animated_webp_image.width,
                 animated_webp_image.height, animated_webp_image.background_rgba, g_animated_webp_frames[0],
                 ConstSlice<u8>{g_animated_webp_rgba[0], g_animated_webp_frame_bytes[0]})
                 .ok()) {
            return 50;
        }
        for (u32 frame_index = 1; frame_index < 4; ++frame_index) {
            if (!media_atlas_gpu_advance_animated_webp(
                     &gpu, &atlas, device, context1, animated_webp_upload_key,
                     animated_webp_image.background_rgba, g_animated_webp_frames[frame_index - 1],
                     g_animated_webp_frames[frame_index],
                     ConstSlice<u8>{g_animated_webp_rgba[frame_index],
                                    g_animated_webp_frame_bytes[frame_index]})
                     .ok()) {
                return 51;
            }
        }
    }
    context->End(end_query);
    context->End(disjoint_query);
    const u64 animated_webp_upload_cpu_us =
        clock_microseconds(clock, animated_webp_upload_begin, clock_now());
    context->Flush();
    disjoint = D3D11_QUERY_DATA_TIMESTAMP_DISJOINT{};
    gpu_begin = 0;
    gpu_end = 0;
    const bool animated_webp_timed = wait_query(context, disjoint_query, &disjoint, sizeof(disjoint)) &&
                                     wait_query(context, begin_query, &gpu_begin, sizeof(gpu_begin)) &&
                                     wait_query(context, end_query, &gpu_end, sizeof(gpu_end)) &&
                                     !disjoint.Disjoint && disjoint.Frequency != 0 && gpu_end >= gpu_begin;
    const u64 animated_webp_upload_gpu_us =
        animated_webp_timed ? gpu_timestamp_microseconds(gpu_end - gpu_begin, disjoint.Frequency) : 0;
    const u32 animated_lossless_frames = gpu.animated_webp_frames;
    const u32 animated_lossless_source_blends = gpu.animated_webp_source_blends;
    const u32 animated_lossless_over_blends = gpu.animated_webp_over_blends;
    const u32 animated_lossless_background_disposals = gpu.animated_webp_background_disposals;
    const u64 animated_lossless_uploaded_bytes = gpu.animated_webp_uploaded_bytes;
    const u64 animated_lossless_dispatched_pixels = gpu.animated_webp_dispatched_pixels;

    AnimatedWebpFixtureResult mixed_webp_fixture{};
    if (!build_mixed_animated_webp_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                           &mixed_webp_fixture)) {
        return 56;
    }
    u64 mixed_webp_decode_checksum = 0;
    const i64 mixed_webp_decode_begin = clock_now();
    for (u32 iteration = 0; iteration < kMixedAnimatedWebpDecodeIterations; ++iteration) {
        AnimatedWebpImage decoded_animation{};
        if (!animated_webp_parse(ConstSlice<u8>{g_png_encoded, mixed_webp_fixture.encoded_bytes},
                                 &decoded_animation, &g_animated_webp_workspace)
                 .ok()) {
            return 57;
        }
        for (u32 frame_index = 0; frame_index < 3; ++frame_index) {
            Vp8Keyframe decoded_vp8{};
            Vp8ControlHeader decoded_header{};
            const AnimatedWebpDecodeResult decoded = animated_webp_decode_parsed_frame(
                ConstSlice<u8>{g_png_encoded, mixed_webp_fixture.encoded_bytes}, decoded_animation,
                frame_index, &g_webp_workspace, &g_lossy_webp_workspace, &decoded_vp8, &decoded_header,
                Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)}, Slice<u8>{g_png_scanlines, 64},
                Slice<u8>{g_png_scanlines + 64, sizeof(g_png_scanlines) - 64});
            if (!decoded.ok()) {
                return 58;
            }
            mixed_webp_decode_checksum += decoded.duration_us + static_cast<u8>(decoded.frame.codec) +
                                          decoded.lossy_decode.alpha_bytes +
                                          g_png_scanlines[(iteration + frame_index) & 63u];
        }
    }
    const u64 mixed_webp_decode_us =
        maximum<u64>(1, clock_microseconds(clock, mixed_webp_decode_begin, clock_now()));

    AnimatedWebpImage mixed_webp_image{};
    if (!animated_webp_parse(ConstSlice<u8>{g_png_encoded, mixed_webp_fixture.encoded_bytes},
                             &mixed_webp_image, &g_animated_webp_workspace)
             .ok()) {
        return 59;
    }
    Vp8Keyframe mixed_vp8_frame{};
    Vp8ControlHeader mixed_vp8_header{};
    for (u32 frame_index = 0; frame_index < 3; ++frame_index) {
        const AnimatedWebpDecodeResult decoded = animated_webp_decode_parsed_frame(
            ConstSlice<u8>{g_png_encoded, mixed_webp_fixture.encoded_bytes}, mixed_webp_image, frame_index,
            &g_webp_workspace, &g_lossy_webp_workspace, &mixed_vp8_frame, &mixed_vp8_header,
            Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)}, Slice<u8>{g_png_scanlines, 64},
            Slice<u8>{g_png_scanlines + 64, sizeof(g_png_scanlines) - 64});
        if (!decoded.ok()) {
            return 60;
        }
        g_mixed_animated_webp_frames[frame_index] = decoded.frame;
        if (frame_index == 0) {
            for (u32 index = 0; index < 64; ++index) {
                g_mixed_animated_webp_rgba[index] = g_png_scanlines[index];
            }
        } else if (frame_index == 2) {
            for (u32 index = 0; index < 16; ++index) {
                g_mixed_animated_webp_alpha[index] = g_png_scanlines[64 + index];
            }
        }
    }

    const MediaAssetKey mixed_webp_upload_key = media_asset_custom_emoji(95, true);
    const i64 mixed_webp_upload_begin = clock_now();
    context->Begin(disjoint_query);
    context->End(begin_query);
    for (u32 iteration = 0; iteration < kMixedAnimatedWebpSequenceIterations; ++iteration) {
        if (!media_atlas_gpu_begin_animated_webp(
                 &gpu, &atlas, device, context1, mixed_webp_upload_key, mixed_webp_image.width,
                 mixed_webp_image.height, mixed_webp_image.background_rgba, g_mixed_animated_webp_frames[0],
                 ConstSlice<u8>{g_mixed_animated_webp_rgba, 64})
                 .ok() ||
            !media_atlas_gpu_reconstruct_vp8_keyframe(&gpu, device, context1, mixed_vp8_frame,
                                                      g_lossy_webp_workspace.modes,
                                                      g_lossy_webp_workspace.coefficients)
                 .ok() ||
            !media_atlas_gpu_filter_vp8_keyframe(&gpu, device, context1, mixed_vp8_frame, mixed_vp8_header,
                                                 g_lossy_webp_workspace.modes,
                                                 g_lossy_webp_workspace.coefficients)
                 .ok() ||
            !media_atlas_gpu_advance_animated_webp_vp8(
                 &gpu, &atlas, device, context1, mixed_webp_upload_key, mixed_webp_image.background_rgba,
                 g_mixed_animated_webp_frames[0], g_mixed_animated_webp_frames[1], mixed_vp8_frame, {})
                 .ok() ||
            !media_atlas_gpu_reconstruct_vp8_keyframe(&gpu, device, context1, mixed_vp8_frame,
                                                      g_lossy_webp_workspace.modes,
                                                      g_lossy_webp_workspace.coefficients)
                 .ok() ||
            !media_atlas_gpu_filter_vp8_keyframe(&gpu, device, context1, mixed_vp8_frame, mixed_vp8_header,
                                                 g_lossy_webp_workspace.modes,
                                                 g_lossy_webp_workspace.coefficients)
                 .ok() ||
            !media_atlas_gpu_advance_animated_webp_vp8(
                 &gpu, &atlas, device, context1, mixed_webp_upload_key, mixed_webp_image.background_rgba,
                 g_mixed_animated_webp_frames[1], g_mixed_animated_webp_frames[2], mixed_vp8_frame,
                 ConstSlice<u8>{g_mixed_animated_webp_alpha, 16})
                 .ok()) {
            return 61;
        }
    }
    context->End(end_query);
    context->End(disjoint_query);
    const u64 mixed_webp_upload_cpu_us = clock_microseconds(clock, mixed_webp_upload_begin, clock_now());
    context->Flush();
    disjoint = D3D11_QUERY_DATA_TIMESTAMP_DISJOINT{};
    gpu_begin = 0;
    gpu_end = 0;
    const bool mixed_webp_timed = wait_query(context, disjoint_query, &disjoint, sizeof(disjoint)) &&
                                  wait_query(context, begin_query, &gpu_begin, sizeof(gpu_begin)) &&
                                  wait_query(context, end_query, &gpu_end, sizeof(gpu_end)) &&
                                  !disjoint.Disjoint && disjoint.Frequency != 0 && gpu_end >= gpu_begin;
    const u64 mixed_webp_upload_gpu_us =
        mixed_webp_timed ? gpu_timestamp_microseconds(gpu_end - gpu_begin, disjoint.Frequency) : 0;
    const u32 mixed_webp_gpu_frames = gpu.animated_webp_frames - animated_lossless_frames;
    const u32 mixed_webp_gpu_source_blends =
        gpu.animated_webp_source_blends - animated_lossless_source_blends;
    const u32 mixed_webp_gpu_over_blends = gpu.animated_webp_over_blends - animated_lossless_over_blends;
    const u32 mixed_webp_gpu_background_disposals =
        gpu.animated_webp_background_disposals - animated_lossless_background_disposals;
    const u64 mixed_webp_gpu_uploaded_bytes =
        gpu.animated_webp_uploaded_bytes - animated_lossless_uploaded_bytes;
    const u64 mixed_webp_gpu_dispatched_pixels =
        gpu.animated_webp_dispatched_pixels - animated_lossless_dispatched_pixels;
    const u32 mixed_webp_gpu_reconstructions = gpu.vp8_reconstructions - vp8_static_reconstructions;
    const u32 mixed_webp_gpu_filter_dispatches = gpu.vp8_filter_dispatches - vp8_static_filter_dispatches;
    const u32 mixed_webp_gpu_atlas_uploads = gpu.vp8_atlas_uploads - vp8_static_atlas_uploads;

    D3D11_TEXTURE2D_DESC staging_description{};
    staging_description.Width = kAtlasExtent;
    staging_description.Height = kAtlasExtent;
    staging_description.MipLevels = 1;
    staging_description.ArraySize = 1;
    staging_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    staging_description.SampleDesc.Count = 1;
    staging_description.Usage = D3D11_USAGE_STAGING;
    staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    result = device->CreateTexture2D(&staging_description, nullptr, &staging);
    if (FAILED(result)) {
        return 21;
    }
    context->CopyResource(staging, gpu.texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        return 22;
    }
    const MediaAtlasResult resident = media_atlas_find(&atlas, upload_key);
    if (!resident.ok()) {
        return 23;
    }
    u64 pixel_checksum = 0;
    for (u32 y = 0; y < kAssetExtent; ++y) {
        const u8* row =
            static_cast<const u8*>(mapped.pData) + (resident.y + y) * mapped.RowPitch + resident.x * 4;
        for (u32 x = 0; x < kAssetExtent * 4; ++x) {
            pixel_checksum += row[x];
        }
    }
    const MediaAtlasResult png_resident = media_atlas_find(&atlas, png_upload_key);
    bool png_pixels_exact = png_resident.ok();
    u64 png_pixel_checksum = 0;
    for (u32 y = 0; y < kAssetExtent && png_pixels_exact; ++y) {
        const u8* row = static_cast<const u8*>(mapped.pData) + (png_resident.y + y) * mapped.RowPitch +
                        png_resident.x * 4;
        for (u32 x = 0; x < kAssetExtent; ++x) {
            const u32 source = png_fixture_rgba(x, y, kAssetExtent, kAssetExtent);
            const u8 alpha = static_cast<u8>(source >> 24);
            const u8 expected_red = alpha == 0 ? 0 : static_cast<u8>(source);
            const u8 expected_green = alpha == 0 ? 0 : static_cast<u8>(source >> 8);
            const u8 expected_blue = alpha == 0 ? 0 : static_cast<u8>(source >> 16);
            const u8* pixel = row + x * 4;
            png_pixels_exact = png_pixels_exact && pixel[0] == expected_red && pixel[1] == expected_green &&
                               pixel[2] == expected_blue && pixel[3] == alpha;
            png_pixel_checksum += pixel[0] + pixel[1] + pixel[2] + pixel[3];
        }
    }
    const MediaAtlasResult webp_resident = media_atlas_find(&atlas, webp_upload_key);
    bool webp_pixels_exact = webp_resident.ok();
    u64 webp_pixel_checksum = 0;
    for (u32 y = 0; y < webp_image.height && webp_pixels_exact; ++y) {
        const u8* row = static_cast<const u8*>(mapped.pData) + (webp_resident.y + y) * mapped.RowPitch +
                        webp_resident.x * 4;
        for (u32 x = 0; x < webp_image.width; ++x) {
            const u8* source = g_webp_rgba + (y * webp_image.width + x) * 4;
            const u8 alpha = source[3];
            const u8 expected[4] = {
                static_cast<u8>((static_cast<u32>(source[0]) * alpha + 127u) / 255u),
                static_cast<u8>((static_cast<u32>(source[1]) * alpha + 127u) / 255u),
                static_cast<u8>((static_cast<u32>(source[2]) * alpha + 127u) / 255u),
                alpha,
            };
            const u8* pixel = row + x * 4;
            for (u32 channel = 0; channel < 4; ++channel) {
                webp_pixels_exact = webp_pixels_exact && pixel[channel] == expected[channel];
                webp_pixel_checksum += pixel[channel];
            }
        }
    }
    const MediaAtlasResult vp8_resident = media_atlas_find(&atlas, vp8_upload_key);
    bool vp8_pixels_exact = vp8_resident.ok() && vp8_resident.width == 4 && vp8_resident.height == 4;
    u64 vp8_pixel_checksum = 0;
    for (u32 y = 0; y < 4 && vp8_pixels_exact; ++y) {
        const u8* row = static_cast<const u8*>(mapped.pData) + (vp8_resident.y + y) * mapped.RowPitch +
                        vp8_resident.x * 4u;
        for (u32 x = 0; x < 4; ++x) {
            const u32 offset = (y * 4u + x) * 4u;
            for (u32 channel = 0; channel < 4; ++channel) {
                vp8_pixels_exact =
                    vp8_pixels_exact && row[x * 4u + channel] == kVp8ExpectedRgba[offset + channel];
                vp8_pixel_checksum += row[x * 4u + channel];
            }
        }
    }
    const MediaAtlasResult jpeg_resident = media_atlas_find(&atlas, jpeg_upload_key);
    bool jpeg_pixels_within_tolerance =
        jpeg_resident.ok() && jpeg_resident.width == 16 && jpeg_resident.height == 16;
    u64 jpeg_pixel_checksum = 0;
    u32 jpeg_maximum_error = 0;
    for (u32 y = 0; y < 16 && jpeg_pixels_within_tolerance; ++y) {
        const u8* row = static_cast<const u8*>(mapped.pData) + (jpeg_resident.y + y) * mapped.RowPitch +
                        jpeg_resident.x * 4u;
        for (u32 x = 0; x < 16; ++x) {
            const u8* pixel = row + x * 4u;
            for (u32 channel = 0; channel < 3; ++channel) {
                const u8 expected = kJpeg420ReferenceRgb[(y * 16u + x) * 3u + channel];
                const u32 error =
                    pixel[channel] > expected ? pixel[channel] - expected : expected - pixel[channel];
                jpeg_maximum_error = maximum<u32>(jpeg_maximum_error, error);
                jpeg_pixel_checksum += pixel[channel];
            }
            jpeg_pixel_checksum += pixel[3];
            jpeg_pixels_within_tolerance = jpeg_pixels_within_tolerance && pixel[3] == 255;
        }
    }
    jpeg_pixels_within_tolerance = jpeg_pixels_within_tolerance && jpeg_maximum_error <= 2;
    const MediaAtlasResult progressive_jpeg_resident =
        media_atlas_find(&atlas, progressive_jpeg_upload_key);
    bool progressive_jpeg_pixels_within_tolerance = progressive_jpeg_resident.ok() &&
                                                    progressive_jpeg_resident.width == 16 &&
                                                    progressive_jpeg_resident.height == 16;
    u64 progressive_jpeg_pixel_checksum = 0;
    u32 progressive_jpeg_maximum_error = 0;
    for (u32 y = 0; y < 16 && progressive_jpeg_pixels_within_tolerance; ++y) {
        const u8* row = static_cast<const u8*>(mapped.pData) +
                        (progressive_jpeg_resident.y + y) * mapped.RowPitch +
                        progressive_jpeg_resident.x * 4u;
        for (u32 x = 0; x < 16; ++x) {
            const u8* pixel = row + x * 4u;
            for (u32 channel = 0; channel < 3; ++channel) {
                const u8 expected =
                    static_cast<u8>(kJpegProgressive420ReferenceRgb[(y * 16u + x) * 3u + channel]);
                const u32 error =
                    pixel[channel] > expected ? pixel[channel] - expected : expected - pixel[channel];
                progressive_jpeg_maximum_error = maximum<u32>(progressive_jpeg_maximum_error, error);
                progressive_jpeg_pixel_checksum += pixel[channel];
            }
            progressive_jpeg_pixel_checksum += pixel[3];
            progressive_jpeg_pixels_within_tolerance =
                progressive_jpeg_pixels_within_tolerance && pixel[3] == 255;
        }
    }
    progressive_jpeg_pixels_within_tolerance =
        progressive_jpeg_pixels_within_tolerance && progressive_jpeg_maximum_error <= 2;
    const JpegReferenceCheck cmyk_jpeg_reference = check_jpeg_reference(
        atlas, mapped, cmyk_jpeg_upload_key,
        ConstSlice<u8>{kJpegCmykAdobeReferenceRgb, sizeof(kJpegCmykAdobeReferenceRgb)});
    const JpegReferenceCheck ycck_jpeg_reference = check_jpeg_reference(
        atlas, mapped, ycck_jpeg_upload_key,
        ConstSlice<u8>{kJpegYcckProgressiveReferenceRgb, sizeof(kJpegYcckProgressiveReferenceRgb)});
    const JpegReferenceCheck oriented_jpeg_reference = check_jpeg_reference(
        atlas, mapped, oriented_jpeg_upload_key,
        ConstSlice<u8>{reinterpret_cast<const u8*>(kJpeg420ReferenceRgb), sizeof(kJpeg420ReferenceRgb)},
        JpegOrientation::right_top);
    const JpegReferenceCheck icc_jpeg_reference = check_jpeg_reference(
        atlas, mapped, icc_jpeg_upload_key,
        ConstSlice<u8>{reinterpret_cast<const u8*>(kJpeg420ReferenceRgb), sizeof(kJpeg420ReferenceRgb)},
        JpegOrientation::top_left, true);
    const JpegReferenceCheck icc_lut_jpeg_reference =
        check_jpeg_checksum(atlas, mapped, icc_lut_jpeg_upload_key);
    bool format_pixels_exact = true;
    u64 format_pixel_checksum = 0;
    for (u32 index = 0; index < format_count && format_pixels_exact; ++index) {
        const MediaAtlasResult format_resident =
            media_atlas_find(&atlas, media_asset_attachment(200 + index));
        format_pixels_exact = format_resident.ok();
        for (u32 y = 0; y < 3 && format_pixels_exact; ++y) {
            const u8* row = static_cast<const u8*>(mapped.pData) +
                            (format_resident.y + y) * mapped.RowPitch + format_resident.x * 4;
            for (u32 x = 0; x < 9; ++x) {
                const u32 straight =
                    png_format_fixture_rgba(formats[index].color_type, formats[index].bit_depth, x, y);
                const u32 alpha = straight >> 24;
                const u8 expected[4] = {
                    static_cast<u8>(((straight & 255u) * alpha + 127u) / 255u),
                    static_cast<u8>((((straight >> 8u) & 255u) * alpha + 127u) / 255u),
                    static_cast<u8>((((straight >> 16u) & 255u) * alpha + 127u) / 255u),
                    static_cast<u8>(alpha),
                };
                const u8* pixel = row + x * 4;
                for (u32 channel = 0; channel < 4; ++channel) {
                    format_pixels_exact = format_pixels_exact && pixel[channel] == expected[channel];
                    format_pixel_checksum += pixel[channel];
                }
            }
        }
    }
    const MediaAtlasResult apng_resident = media_atlas_find(&atlas, apng_upload_key);
    bool apng_pixels_exact = apng_resident.ok();
    u64 apng_pixel_checksum = 0;
    for (u32 y = 0; y < 4 && apng_pixels_exact; ++y) {
        const u8* row = static_cast<const u8*>(mapped.pData) + (apng_resident.y + y) * mapped.RowPitch +
                        apng_resident.x * 4;
        for (u32 x = 0; x < 4; ++x) {
            const u8* pixel = row + x * 4;
            const u32 actual = static_cast<u32>(pixel[0]) | static_cast<u32>(pixel[1]) << 8 |
                               static_cast<u32>(pixel[2]) << 16 | static_cast<u32>(pixel[3]) << 24;
            apng_pixels_exact = apng_pixels_exact && actual == apng_fixture_canvas_rgba(3, x, y);
            apng_pixel_checksum += pixel[0] + pixel[1] + pixel[2] + pixel[3];
        }
    }
    const MediaAtlasResult gif_resident = media_atlas_find(&atlas, gif_upload_key);
    bool gif_pixels_exact = gif_resident.ok();
    u64 gif_pixel_checksum = 0;
    for (u32 y = 0; y < 4 && gif_pixels_exact; ++y) {
        const u8* row = static_cast<const u8*>(mapped.pData) + (gif_resident.y + y) * mapped.RowPitch +
                        gif_resident.x * 4;
        for (u32 x = 0; x < 4; ++x) {
            const u8* pixel = row + x * 4;
            const u32 actual = static_cast<u32>(pixel[0]) | static_cast<u32>(pixel[1]) << 8 |
                               static_cast<u32>(pixel[2]) << 16 | static_cast<u32>(pixel[3]) << 24;
            gif_pixels_exact = gif_pixels_exact && actual == gif_fixture_canvas_rgba(3, x, y);
            gif_pixel_checksum += pixel[0] + pixel[1] + pixel[2] + pixel[3];
        }
    }
    const MediaAtlasResult animated_webp_resident = media_atlas_find(&atlas, animated_webp_upload_key);
    bool animated_webp_pixels_exact = animated_webp_resident.ok();
    u64 animated_webp_pixel_checksum = 0;
    for (u32 y = 0; y < 4 && animated_webp_pixels_exact; ++y) {
        const u8* row = static_cast<const u8*>(mapped.pData) +
                        (animated_webp_resident.y + y) * mapped.RowPitch + animated_webp_resident.x * 4;
        for (u32 x = 0; x < 4; ++x) {
            const u8* pixel = row + x * 4;
            const u32 actual = static_cast<u32>(pixel[0]) | static_cast<u32>(pixel[1]) << 8 |
                               static_cast<u32>(pixel[2]) << 16 | static_cast<u32>(pixel[3]) << 24;
            animated_webp_pixels_exact =
                animated_webp_pixels_exact && actual == animated_webp_fixture_canvas_rgba(3, x, y);
            animated_webp_pixel_checksum += pixel[0] + pixel[1] + pixel[2] + pixel[3];
        }
    }
    const MediaAtlasResult mixed_webp_resident = media_atlas_find(&atlas, mixed_webp_upload_key);
    bool mixed_webp_pixels_exact = mixed_webp_resident.ok();
    u64 mixed_webp_pixel_checksum = 0;
    for (u32 y = 0; y < 4 && mixed_webp_pixels_exact; ++y) {
        const u8* row = static_cast<const u8*>(mapped.pData) +
                        (mixed_webp_resident.y + y) * mapped.RowPitch + mixed_webp_resident.x * 4;
        for (u32 x = 0; x < 4; ++x) {
            const u8* pixel = row + x * 4;
            const u32 actual = static_cast<u32>(pixel[0]) | static_cast<u32>(pixel[1]) << 8 |
                               static_cast<u32>(pixel[2]) << 16 | static_cast<u32>(pixel[3]) << 24;
            mixed_webp_pixels_exact =
                mixed_webp_pixels_exact && actual == mixed_animated_webp_fixture_canvas_rgba(2, x, y);
            mixed_webp_pixel_checksum += pixel[0] + pixel[1] + pixel[2] + pixel[3];
        }
    }
    context->Unmap(staging, 0);

    write_metric("cdn_target_count", kTargetIterations);
    write_metric("cdn_target_per_second", static_cast<u64>(kTargetIterations) * 1'000'000 / target_us);
    write_metric("cdn_target_checksum", target_checksum);
    write_metric("attachment_target_parse_count", kAttachmentTargetIterations);
    write_metric("attachment_target_parse_per_second",
                 static_cast<u64>(kAttachmentTargetIterations) * 1'000'000 / attachment_target_us);
    write_metric("attachment_target_parse_checksum", attachment_target_checksum);
    write_animation_metrics(animation_result);
    write_metric("cdn_request_count", kCdnRequestIterations);
    write_metric("cdn_request_per_second",
                 static_cast<u64>(kCdnRequestIterations) * 1'000'000 / request_us);
    write_metric("cdn_request_checksum", request_checksum);
    write_metric("encoded_cache_find_count", kEncodedCacheFindIterations);
    write_metric("encoded_cache_find_per_second",
                 static_cast<u64>(kEncodedCacheFindIterations) * 1'000'000 / cache_us);
    write_metric("encoded_cache_find_checksum", cache_checksum);
    write_metric("cdn_pipeline_admit_count", kPipelineAdmitIterations);
    write_metric("cdn_pipeline_admit_per_second",
                 static_cast<u64>(kPipelineAdmitIterations) * 1'000'000 / pipeline_us);
    write_metric("cdn_pipeline_admit_bytes_per_second", static_cast<u64>(png_fixture.encoded_bytes) *
                                                            kPipelineAdmitIterations * 1'000'000 /
                                                            pipeline_us);
    write_metric("cdn_pipeline_admit_checksum", pipeline_checksum);
    write_metric("media_find_count", kFindIterations);
    write_metric("media_find_per_second", static_cast<u64>(kFindIterations) * 1'000'000 / find_us);
    write_metric("media_find_checksum", find_checksum);
    write_metric("media_atlas_churn_count", kAtlasChurnIterations);
    write_metric("media_atlas_churn_checksum", churn_checksum);
    write_metric("presentation_damage_operations", kPresentationDamageIterations);
    write_metric("presentation_damage_per_second",
                 static_cast<u64>(kPresentationDamageIterations) * 1'000'000 /
                     presentation_damage_us);
    write_metric("presentation_damage_checksum", presentation_damage_checksum);
    write_metric("visibility_epoch_count", kVisibilityEpochIterations);
    write_metric("visibility_epoch_per_second",
                 static_cast<u64>(kVisibilityEpochIterations) * 1'000'000 /
                     visibility_epoch_us);
    write_metric("visibility_epoch_checksum", visibility_epoch_checksum);
    write_metric("presentation_policy_count",
                 kPresentationPolicyIterations);
    write_metric(
        "presentation_policy_per_second",
        static_cast<u64>(kPresentationPolicyIterations) * 1'000'000 /
            presentation_policy_us);
    write_metric("presentation_policy_checksum",
                 presentation_policy_checksum);
    write_metric("media_missing_find_count", kMissingFindIterations);
    write_metric("media_missing_find_per_second",
                 static_cast<u64>(kMissingFindIterations) * 1'000'000 /
                     missing_find_us);
    write_metric("media_missing_find_checksum", missing_find_checksum);
    write_metric("media_upload_count", gpu.uploads);
    write_metric("media_upload_cpu_batch_us", upload_cpu_us);
    write_metric("media_upload_gpu_batch_us", upload_gpu_us);
    write_metric("media_upload_gpu_timing_valid", timed ? 1 : 0);
    write_metric("media_uploaded_bytes", gpu.uploaded_bytes);
    write_metric("media_pixel_checksum", pixel_checksum);
    write_metric("png_decode_count", kPngDecodeIterations);
    write_metric("png_decode_per_second",
                 static_cast<u64>(kPngDecodeIterations) * 1'000'000 / png_decode_us);
    write_metric("png_decode_bytes_per_second", static_cast<u64>(png_fixture.encoded_bytes) *
                                                    kPngDecodeIterations * 1'000'000 / png_decode_us);
    write_metric("png_decode_checksum", png_decode_checksum);
    write_metric("apng_decode_count", kApngDecodeIterations);
    write_metric("apng_workspace_bytes", sizeof(ApngWorkspace));
    write_metric("apng_decode_per_second",
                 static_cast<u64>(kApngDecodeIterations) * 1'000'000 / apng_decode_us);
    write_metric("apng_decode_bytes_per_second", static_cast<u64>(apng_fixture.encoded_bytes) *
                                                     kApngDecodeIterations * 1'000'000 / apng_decode_us);
    write_metric("apng_decode_checksum", apng_decode_checksum);
    write_metric("gif_decode_count", kGifDecodeIterations);
    write_metric("gif_workspace_bytes", sizeof(GifWorkspace));
    write_metric("gif_decode_per_second",
                 static_cast<u64>(kGifDecodeIterations) * 1'000'000 / gif_decode_us);
    write_metric("gif_decode_bytes_per_second", static_cast<u64>(gif_fixture.encoded_bytes) *
                                                    kGifDecodeIterations * 1'000'000 / gif_decode_us);
    write_metric("gif_decode_checksum", gif_decode_checksum);
    write_metric("animated_webp_decode_count", kAnimatedWebpDecodeIterations);
    write_metric("animated_webp_workspace_bytes", sizeof(AnimatedWebpWorkspace));
    write_metric("animated_webp_decode_per_second",
                 static_cast<u64>(kAnimatedWebpDecodeIterations) * 1'000'000 / animated_webp_decode_us);
    write_metric("animated_webp_decode_bytes_per_second",
                 static_cast<u64>(animated_webp_fixture.encoded_bytes) * kAnimatedWebpDecodeIterations *
                     1'000'000 / animated_webp_decode_us);
    write_metric("animated_webp_decode_checksum", animated_webp_decode_checksum);
    write_metric("webp_vp8l_decode_count", kWebpDecodeIterations);
    write_metric("webp_vp8l_workspace_bytes", sizeof(Vp8lWorkspace));
    write_metric("webp_vp8l_decode_per_second",
                 static_cast<u64>(kWebpDecodeIterations) * 1'000'000 / webp_decode_us);
    write_metric("webp_vp8l_decode_bytes_per_second", static_cast<u64>(sizeof(kWebpGradient)) *
                                                          kWebpDecodeIterations * 1'000'000 /
                                                          webp_decode_us);
    write_metric("webp_vp8l_decode_checksum", webp_decode_checksum);
    write_metric("webp_vp8_decode_count", kVp8DecodeIterations);
    write_metric("webp_vp8_workspace_bytes", sizeof(LossyWebpWorkspace));
    write_metric("webp_vp8_decode_per_second",
                 static_cast<u64>(kVp8DecodeIterations) * 1'000'000 / vp8_decode_us);
    write_metric("webp_vp8_decode_bytes_per_second", static_cast<u64>(sizeof(kWebpLossy4x4)) *
                                                         kVp8DecodeIterations * 1'000'000 / vp8_decode_us);
    write_metric("webp_vp8_decode_checksum", vp8_decode_checksum);
    write_metric("jpeg_decode_count", kJpegDecodeIterations);
    write_metric("jpeg_workspace_bytes", sizeof(JpegWorkspace));
    write_metric("jpeg_decode_per_second",
                 static_cast<u64>(kJpegDecodeIterations) * 1'000'000 / jpeg_decode_us);
    write_metric("jpeg_decode_bytes_per_second", static_cast<u64>(sizeof(kJpeg420Reference)) *
                                                     kJpegDecodeIterations * 1'000'000 / jpeg_decode_us);
    write_metric("jpeg_decode_checksum", jpeg_decode_checksum);
    write_metric("progressive_jpeg_decode_count", kProgressiveJpegDecodeIterations);
    write_metric("progressive_jpeg_scan_count", 10);
    write_metric("progressive_jpeg_decode_per_second", static_cast<u64>(kProgressiveJpegDecodeIterations) *
                                                           1'000'000 / progressive_jpeg_decode_us);
    write_metric("progressive_jpeg_decode_bytes_per_second",
                 static_cast<u64>(kJpegProgressive420ReferenceBytes) * kProgressiveJpegDecodeIterations *
                     1'000'000 / progressive_jpeg_decode_us);
    write_metric("progressive_jpeg_decode_checksum", progressive_jpeg_decode_checksum);
    write_metric("cmyk_jpeg_decode_count", kCmykJpegDecodeIterations);
    write_metric("cmyk_jpeg_decode_per_second",
                 static_cast<u64>(kCmykJpegDecodeIterations) * 1'000'000 / cmyk_jpeg_decode_us);
    write_metric("cmyk_jpeg_decode_bytes_per_second", static_cast<u64>(sizeof(kJpegCmykAdobeReference)) *
                                                          kCmykJpegDecodeIterations * 1'000'000 /
                                                          cmyk_jpeg_decode_us);
    write_metric("cmyk_jpeg_decode_checksum", cmyk_jpeg_decode_checksum);
    write_metric("ycck_jpeg_decode_count", kYcckJpegDecodeIterations);
    write_metric("ycck_jpeg_scan_count", ycck_jpeg_image.scan_count);
    write_metric("ycck_jpeg_decode_per_second",
                 static_cast<u64>(kYcckJpegDecodeIterations) * 1'000'000 / ycck_jpeg_decode_us);
    write_metric("ycck_jpeg_decode_bytes_per_second",
                 static_cast<u64>(sizeof(kJpegYcckProgressiveReference)) * kYcckJpegDecodeIterations *
                     1'000'000 / ycck_jpeg_decode_us);
    write_metric("ycck_jpeg_decode_checksum", ycck_jpeg_decode_checksum);
    write_metric("oriented_jpeg_decode_count", kOrientedJpegDecodeIterations);
    write_metric("oriented_jpeg_decode_per_second",
                 static_cast<u64>(kOrientedJpegDecodeIterations) * 1'000'000 / oriented_jpeg_decode_us);
    write_metric("oriented_jpeg_decode_bytes_per_second", static_cast<u64>(oriented_jpeg_encoded_bytes) *
                                                              kOrientedJpegDecodeIterations * 1'000'000 /
                                                              oriented_jpeg_decode_us);
    write_metric("oriented_jpeg_decode_checksum", oriented_jpeg_decode_checksum);
    write_metric("icc_jpeg_decode_count", kIccJpegDecodeIterations);
    write_metric("icc_jpeg_decode_per_second",
                 static_cast<u64>(kIccJpegDecodeIterations) * 1'000'000 / icc_jpeg_decode_us);
    write_metric("icc_jpeg_decode_bytes_per_second", static_cast<u64>(icc_jpeg_fixture.encoded_bytes) *
                                                         kIccJpegDecodeIterations * 1'000'000 /
                                                         icc_jpeg_decode_us);
    write_metric("icc_jpeg_decode_checksum", icc_jpeg_decode_checksum);
    write_metric("icc_lut_jpeg_decode_count", kIccLutJpegDecodeIterations);
    write_metric("icc_lut_jpeg_decode_per_second",
                 static_cast<u64>(kIccLutJpegDecodeIterations) * 1'000'000 / icc_lut_jpeg_decode_us);
    write_metric("icc_lut_jpeg_decode_bytes_per_second",
                 static_cast<u64>(icc_lut_jpeg_fixture.encoded_bytes) * kIccLutJpegDecodeIterations *
                     1'000'000 / icc_lut_jpeg_decode_us);
    write_metric("icc_lut_jpeg_decode_checksum", icc_lut_jpeg_decode_checksum);
    write_metric("png_gpu_upload_count", gpu.png_uploads);
    write_metric("png_gpu_cpu_batch_us", png_upload_cpu_us);
    write_metric("png_gpu_batch_us", png_upload_gpu_us);
    write_metric("png_gpu_timing_valid", png_timed ? 1 : 0);
    write_metric("png_gpu_uploaded_bytes", gpu.png_uploaded_bytes);
    write_metric("png_gpu_dispatched_pixels", gpu.png_dispatched_pixels);
    write_metric("png_gpu_pixel_checksum", png_pixel_checksum);
    write_metric("png_gpu_pixels_exact", png_pixels_exact ? 1 : 0);
    write_metric("webp_gpu_upload_count", gpu.straight_rgba8_uploads);
    write_metric("webp_gpu_cpu_batch_us", webp_upload_cpu_us);
    write_metric("webp_gpu_batch_us", webp_upload_gpu_us);
    write_metric("webp_gpu_timing_valid", webp_timed ? 1 : 0);
    write_metric("webp_gpu_uploaded_bytes", gpu.straight_rgba8_uploaded_bytes);
    write_metric("webp_gpu_dispatched_pixels", gpu.straight_rgba8_dispatched_pixels);
    write_metric("webp_gpu_pixel_checksum", webp_pixel_checksum);
    write_metric("webp_gpu_pixels_exact", webp_pixels_exact ? 1 : 0);
    write_metric("webp_vp8_gpu_upload_count", vp8_static_atlas_uploads);
    write_metric("webp_vp8_gpu_cpu_batch_us", vp8_upload_cpu_us);
    write_metric("webp_vp8_gpu_batch_us", vp8_upload_gpu_us);
    write_metric("webp_vp8_gpu_timing_valid", vp8_timed ? 1 : 0);
    write_metric("webp_vp8_gpu_reconstructions", vp8_static_reconstructions);
    write_metric("webp_vp8_gpu_reconstruction_dispatches",
                 vp8_static_reconstruction_dispatches);
    write_metric("webp_vp8_gpu_filter_dispatches", vp8_static_filter_dispatches);
    write_metric("webp_vp8_gpu_uploaded_bytes", vp8_static_uploaded_bytes);
    write_metric("webp_vp8_gpu_reconstructed_pixels", vp8_static_reconstructed_pixels);
    write_metric("webp_vp8_gpu_pixel_checksum", vp8_pixel_checksum);
    write_metric("webp_vp8_gpu_pixels_exact", vp8_pixels_exact ? 1 : 0);
    write_metric("jpeg_gpu_upload_count", jpeg_uploads);
    write_metric("jpeg_gpu_cpu_batch_us", jpeg_upload_cpu_us);
    write_metric("jpeg_gpu_batch_us", jpeg_upload_gpu_us);
    write_metric("jpeg_gpu_timing_valid", jpeg_timed ? 1 : 0);
    write_metric("jpeg_gpu_uploaded_bytes", jpeg_uploaded_bytes);
    write_metric("jpeg_gpu_reconstructed_pixels", jpeg_reconstructed_pixels);
    write_metric("jpeg_gpu_idct_dispatches", jpeg_idct_dispatches);
    write_metric("jpeg_gpu_atlas_dispatches", jpeg_atlas_dispatches);
    write_metric("jpeg_gpu_pixel_checksum", jpeg_pixel_checksum);
    write_metric("jpeg_gpu_maximum_reference_error", jpeg_maximum_error);
    write_metric("jpeg_gpu_pixels_within_tolerance", jpeg_pixels_within_tolerance ? 1 : 0);
    write_metric("progressive_jpeg_gpu_upload_count", progressive_jpeg_uploads);
    write_metric("progressive_jpeg_gpu_cpu_batch_us", progressive_jpeg_upload_cpu_us);
    write_metric("progressive_jpeg_gpu_batch_us", progressive_jpeg_upload_gpu_us);
    write_metric("progressive_jpeg_gpu_timing_valid", progressive_jpeg_timed ? 1 : 0);
    write_metric("progressive_jpeg_gpu_uploaded_bytes", progressive_jpeg_uploaded_bytes);
    write_metric("progressive_jpeg_gpu_reconstructed_pixels", progressive_jpeg_reconstructed_pixels);
    write_metric("progressive_jpeg_gpu_idct_dispatches", progressive_jpeg_idct_dispatches);
    write_metric("progressive_jpeg_gpu_atlas_dispatches", progressive_jpeg_atlas_dispatches);
    write_metric("progressive_jpeg_gpu_pixel_checksum", progressive_jpeg_pixel_checksum);
    write_metric("progressive_jpeg_gpu_maximum_reference_error", progressive_jpeg_maximum_error);
    write_metric("progressive_jpeg_gpu_pixels_within_tolerance",
                 progressive_jpeg_pixels_within_tolerance ? 1 : 0);
    write_metric("cmyk_jpeg_gpu_upload_count", cmyk_jpeg_uploads);
    write_metric("cmyk_jpeg_gpu_cpu_batch_us", cmyk_jpeg_upload_cpu_us);
    write_metric("cmyk_jpeg_gpu_batch_us", cmyk_jpeg_upload_gpu_us);
    write_metric("cmyk_jpeg_gpu_timing_valid", cmyk_jpeg_timed ? 1 : 0);
    write_metric("cmyk_jpeg_gpu_uploaded_bytes", cmyk_jpeg_uploaded_bytes);
    write_metric("cmyk_jpeg_gpu_reconstructed_pixels", cmyk_jpeg_reconstructed_pixels);
    write_metric("cmyk_jpeg_gpu_idct_dispatches", cmyk_jpeg_idct_dispatches);
    write_metric("cmyk_jpeg_gpu_atlas_dispatches", cmyk_jpeg_atlas_dispatches);
    write_metric("cmyk_jpeg_gpu_pixel_checksum", cmyk_jpeg_reference.checksum);
    write_metric("cmyk_jpeg_gpu_maximum_reference_error", cmyk_jpeg_reference.maximum_error);
    write_metric("cmyk_jpeg_gpu_pixels_within_tolerance", cmyk_jpeg_reference.within_tolerance ? 1 : 0);
    write_metric("ycck_jpeg_gpu_upload_count", ycck_jpeg_uploads);
    write_metric("ycck_jpeg_gpu_cpu_batch_us", ycck_jpeg_upload_cpu_us);
    write_metric("ycck_jpeg_gpu_batch_us", ycck_jpeg_upload_gpu_us);
    write_metric("ycck_jpeg_gpu_timing_valid", ycck_jpeg_timed ? 1 : 0);
    write_metric("ycck_jpeg_gpu_uploaded_bytes", ycck_jpeg_uploaded_bytes);
    write_metric("ycck_jpeg_gpu_reconstructed_pixels", ycck_jpeg_reconstructed_pixels);
    write_metric("ycck_jpeg_gpu_idct_dispatches", ycck_jpeg_idct_dispatches);
    write_metric("ycck_jpeg_gpu_atlas_dispatches", ycck_jpeg_atlas_dispatches);
    write_metric("ycck_jpeg_gpu_pixel_checksum", ycck_jpeg_reference.checksum);
    write_metric("ycck_jpeg_gpu_maximum_reference_error", ycck_jpeg_reference.maximum_error);
    write_metric("ycck_jpeg_gpu_pixels_within_tolerance", ycck_jpeg_reference.within_tolerance ? 1 : 0);
    write_metric("oriented_jpeg_gpu_upload_count", oriented_jpeg_uploads);
    write_metric("oriented_jpeg_gpu_cpu_batch_us", oriented_jpeg_upload_cpu_us);
    write_metric("oriented_jpeg_gpu_batch_us", oriented_jpeg_upload_gpu_us);
    write_metric("oriented_jpeg_gpu_timing_valid", oriented_jpeg_timed ? 1 : 0);
    write_metric("oriented_jpeg_gpu_uploaded_bytes", oriented_jpeg_uploaded_bytes);
    write_metric("oriented_jpeg_gpu_reconstructed_pixels", oriented_jpeg_reconstructed_pixels);
    write_metric("oriented_jpeg_gpu_idct_dispatches", oriented_jpeg_idct_dispatches);
    write_metric("oriented_jpeg_gpu_atlas_dispatches", oriented_jpeg_atlas_dispatches);
    write_metric("oriented_jpeg_gpu_pixel_checksum", oriented_jpeg_reference.checksum);
    write_metric("oriented_jpeg_gpu_maximum_reference_error", oriented_jpeg_reference.maximum_error);
    write_metric("oriented_jpeg_gpu_pixels_within_tolerance",
                 oriented_jpeg_reference.within_tolerance ? 1 : 0);
    write_metric("icc_jpeg_gpu_upload_count", icc_jpeg_uploads);
    write_metric("icc_jpeg_gpu_cpu_batch_us", icc_jpeg_upload_cpu_us);
    write_metric("icc_jpeg_gpu_batch_us", icc_jpeg_upload_gpu_us);
    write_metric("icc_jpeg_gpu_timing_valid", icc_jpeg_timed ? 1 : 0);
    write_metric("icc_jpeg_gpu_uploaded_bytes", icc_jpeg_uploaded_bytes);
    write_metric("icc_jpeg_gpu_reconstructed_pixels", icc_jpeg_reconstructed_pixels);
    write_metric("icc_jpeg_gpu_idct_dispatches", icc_jpeg_idct_dispatches);
    write_metric("icc_jpeg_gpu_atlas_dispatches", icc_jpeg_atlas_dispatches);
    write_metric("icc_jpeg_gpu_pixel_checksum", icc_jpeg_reference.checksum);
    write_metric("icc_jpeg_gpu_maximum_reference_error", icc_jpeg_reference.maximum_error);
    write_metric("icc_jpeg_gpu_pixels_within_tolerance", icc_jpeg_reference.within_tolerance ? 1 : 0);
    write_metric("icc_lut_jpeg_gpu_upload_count", icc_lut_jpeg_uploads);
    write_metric("icc_lut_jpeg_gpu_cpu_batch_us", icc_lut_jpeg_upload_cpu_us);
    write_metric("icc_lut_jpeg_gpu_batch_us", icc_lut_jpeg_upload_gpu_us);
    write_metric("icc_lut_jpeg_gpu_timing_valid", icc_lut_jpeg_timed ? 1 : 0);
    write_metric("icc_lut_jpeg_gpu_uploaded_bytes", icc_lut_jpeg_uploaded_bytes);
    write_metric("icc_lut_jpeg_gpu_reconstructed_pixels", icc_lut_jpeg_reconstructed_pixels);
    write_metric("icc_lut_jpeg_gpu_idct_dispatches", icc_lut_jpeg_idct_dispatches);
    write_metric("icc_lut_jpeg_gpu_atlas_dispatches", icc_lut_jpeg_atlas_dispatches);
    write_metric("icc_lut_jpeg_gpu_pixel_checksum", icc_lut_jpeg_reference.checksum);
    write_metric("icc_lut_jpeg_gpu_alpha_exact", icc_lut_jpeg_reference.within_tolerance ? 1 : 0);
    write_metric("png_format_gpu_upload_count", format_count);
    write_metric("png_format_gpu_pixel_checksum", format_pixel_checksum);
    write_metric("png_format_gpu_pixels_exact", format_pixels_exact ? 1 : 0);
    write_metric("apng_gpu_sequence_count", kApngSequenceIterations);
    write_metric("apng_gpu_cpu_batch_us", apng_upload_cpu_us);
    write_metric("apng_gpu_batch_us", apng_upload_gpu_us);
    write_metric("apng_gpu_timing_valid", apng_timed ? 1 : 0);
    write_metric("apng_gpu_frame_count", gpu.apng_frames);
    write_metric("apng_gpu_dispatched_pixels", gpu.apng_dispatched_pixels);
    write_metric("apng_gpu_source_blends", gpu.apng_source_blends);
    write_metric("apng_gpu_over_blends", gpu.apng_over_blends);
    write_metric("apng_gpu_background_disposals", gpu.apng_background_disposals);
    write_metric("apng_gpu_previous_disposals", gpu.apng_previous_disposals);
    write_metric("apng_gpu_pixel_checksum", apng_pixel_checksum);
    write_metric("apng_gpu_pixels_exact", apng_pixels_exact ? 1 : 0);
    write_metric("gif_gpu_sequence_count", kGifSequenceIterations);
    write_metric("gif_gpu_cpu_batch_us", gif_upload_cpu_us);
    write_metric("gif_gpu_batch_us", gif_upload_gpu_us);
    write_metric("gif_gpu_timing_valid", gif_timed ? 1 : 0);
    write_metric("gif_gpu_frame_count", gpu.gif_frames);
    write_metric("gif_gpu_uploaded_bytes", gpu.gif_uploaded_bytes);
    write_metric("gif_gpu_dispatched_pixels", gpu.gif_dispatched_pixels);
    write_metric("gif_gpu_background_disposals", gpu.gif_background_disposals);
    write_metric("gif_gpu_previous_disposals", gpu.gif_previous_disposals);
    write_metric("gif_gpu_pixel_checksum", gif_pixel_checksum);
    write_metric("gif_gpu_pixels_exact", gif_pixels_exact ? 1 : 0);
    write_metric("animated_webp_gpu_sequence_count", kAnimatedWebpSequenceIterations);
    write_metric("animated_webp_gpu_cpu_batch_us", animated_webp_upload_cpu_us);
    write_metric("animated_webp_gpu_batch_us", animated_webp_upload_gpu_us);
    write_metric("animated_webp_gpu_timing_valid", animated_webp_timed ? 1 : 0);
    write_metric("animated_webp_gpu_frame_count", animated_lossless_frames);
    write_metric("animated_webp_gpu_uploaded_bytes", animated_lossless_uploaded_bytes);
    write_metric("animated_webp_gpu_dispatched_pixels", animated_lossless_dispatched_pixels);
    write_metric("animated_webp_gpu_source_blends", animated_lossless_source_blends);
    write_metric("animated_webp_gpu_over_blends", animated_lossless_over_blends);
    write_metric("animated_webp_gpu_background_disposals", animated_lossless_background_disposals);
    write_metric("animated_webp_gpu_pixel_checksum", animated_webp_pixel_checksum);
    write_metric("animated_webp_gpu_pixels_exact", animated_webp_pixels_exact ? 1 : 0);
    write_metric("mixed_animated_webp_decode_count", kMixedAnimatedWebpDecodeIterations);
    write_metric("mixed_animated_webp_decode_per_second",
                 static_cast<u64>(kMixedAnimatedWebpDecodeIterations) * 1'000'000 / mixed_webp_decode_us);
    write_metric("mixed_animated_webp_decode_bytes_per_second",
                 static_cast<u64>(mixed_webp_fixture.encoded_bytes) * kMixedAnimatedWebpDecodeIterations *
                     1'000'000 / mixed_webp_decode_us);
    write_metric("mixed_animated_webp_decode_checksum", mixed_webp_decode_checksum);
    write_metric("mixed_animated_webp_gpu_sequence_count", kMixedAnimatedWebpSequenceIterations);
    write_metric("mixed_animated_webp_gpu_cpu_batch_us", mixed_webp_upload_cpu_us);
    write_metric("mixed_animated_webp_gpu_batch_us", mixed_webp_upload_gpu_us);
    write_metric("mixed_animated_webp_gpu_timing_valid", mixed_webp_timed ? 1 : 0);
    write_metric("mixed_animated_webp_gpu_frame_count", mixed_webp_gpu_frames);
    write_metric("mixed_animated_webp_gpu_uploaded_bytes", mixed_webp_gpu_uploaded_bytes);
    write_metric("mixed_animated_webp_gpu_dispatched_pixels", mixed_webp_gpu_dispatched_pixels);
    write_metric("mixed_animated_webp_gpu_source_blends", mixed_webp_gpu_source_blends);
    write_metric("mixed_animated_webp_gpu_over_blends", mixed_webp_gpu_over_blends);
    write_metric("mixed_animated_webp_gpu_background_disposals", mixed_webp_gpu_background_disposals);
    write_metric("mixed_animated_webp_gpu_vp8_reconstructions", mixed_webp_gpu_reconstructions);
    write_metric("mixed_animated_webp_gpu_vp8_filter_dispatches", mixed_webp_gpu_filter_dispatches);
    write_metric("mixed_animated_webp_gpu_vp8_atlas_uploads", mixed_webp_gpu_atlas_uploads);
    write_metric("mixed_animated_webp_gpu_pixel_checksum", mixed_webp_pixel_checksum);
    write_metric("mixed_animated_webp_gpu_pixels_exact", mixed_webp_pixels_exact ? 1 : 0);

    const bool valid =
        gpu.uploads == kUploadIterations &&
        gpu.uploaded_bytes == static_cast<u64>(sizeof(g_pixels)) * kUploadIterations &&
        pixel_checksum != 0 && target_checksum != 0 && attachment_target_checksum != 0 &&
        find_checksum != 0 && churn_checksum != 0 && missing_find_checksum != 0 &&
        presentation_damage_checksum != 0 &&
        presentation_policy_checksum != 0 &&
        png_decode_checksum != 0 && webp_decode_checksum != 0 &&
        vp8_decode_checksum != 0 && jpeg_decode_checksum != 0 && progressive_jpeg_decode_checksum != 0 &&
        cmyk_jpeg_decode_checksum != 0 && ycck_jpeg_decode_checksum != 0 &&
        oriented_jpeg_decode_checksum != 0 && icc_jpeg_decode_checksum != 0 &&
        icc_lut_jpeg_decode_checksum != 0 && apng_decode_checksum != 0 && gif_decode_checksum != 0 &&
        animated_webp_decode_checksum != 0 && gpu.png_uploads == kPngUploadIterations + format_count &&
        gpu.png_uploaded_bytes ==
            static_cast<u64>(timed_png_inflated_bytes) * kPngUploadIterations + format_uploaded_bytes &&
        gpu.png_dispatched_pixels == static_cast<u64>(kAssetExtent) * kAssetExtent * kPngUploadIterations +
                                         static_cast<u64>(9 * 3) * format_count &&
        png_pixels_exact && png_pixel_checksum != 0 &&
        gpu.straight_rgba8_uploads == kWebpUploadIterations &&
        gpu.straight_rgba8_uploaded_bytes ==
            static_cast<u64>(sizeof(g_webp_rgba)) * kWebpUploadIterations &&
        gpu.straight_rgba8_dispatched_pixels ==
            static_cast<u64>(webp_image.width) * webp_image.height * kWebpUploadIterations &&
        webp_pixels_exact && webp_pixel_checksum != 0 &&
        vp8_static_reconstructions == kVp8UploadIterations &&
        vp8_static_filter_dispatches == kVp8UploadIterations &&
        vp8_static_atlas_uploads == kVp8UploadIterations &&
        vp8_static_reconstructed_pixels == static_cast<u64>(kVp8UploadIterations) * 16u &&
        vp8_pixels_exact && vp8_pixel_checksum != 0 && jpeg_uploads == kJpegUploadIterations &&
        jpeg_uploaded_bytes == static_cast<u64>(kJpegUploadIterations) * 1'280u &&
        jpeg_reconstructed_pixels == static_cast<u64>(kJpegUploadIterations) * 384u &&
        jpeg_idct_dispatches == kJpegUploadIterations && jpeg_atlas_dispatches == kJpegUploadIterations &&
        jpeg_pixels_within_tolerance && jpeg_pixel_checksum != 0 &&
        progressive_jpeg_uploads == kProgressiveJpegUploadIterations &&
        progressive_jpeg_uploaded_bytes == static_cast<u64>(kProgressiveJpegUploadIterations) * 1'280u &&
        progressive_jpeg_reconstructed_pixels ==
            static_cast<u64>(kProgressiveJpegUploadIterations) * 384u &&
        progressive_jpeg_idct_dispatches == kProgressiveJpegUploadIterations &&
        progressive_jpeg_atlas_dispatches == kProgressiveJpegUploadIterations &&
        progressive_jpeg_pixels_within_tolerance && progressive_jpeg_pixel_checksum != 0 &&
        cmyk_jpeg_uploads == kCmykJpegUploadIterations &&
        cmyk_jpeg_uploaded_bytes == static_cast<u64>(kCmykJpegUploadIterations) * 2'560u &&
        cmyk_jpeg_reconstructed_pixels == static_cast<u64>(kCmykJpegUploadIterations) * 1'024u &&
        cmyk_jpeg_idct_dispatches == kCmykJpegUploadIterations &&
        cmyk_jpeg_atlas_dispatches == kCmykJpegUploadIterations && cmyk_jpeg_reference.within_tolerance &&
        cmyk_jpeg_reference.checksum != 0 && ycck_jpeg_uploads == kYcckJpegUploadIterations &&
        ycck_jpeg_uploaded_bytes == static_cast<u64>(kYcckJpegUploadIterations) * 1'792u &&
        ycck_jpeg_reconstructed_pixels == static_cast<u64>(kYcckJpegUploadIterations) * 640u &&
        ycck_jpeg_idct_dispatches == kYcckJpegUploadIterations &&
        ycck_jpeg_atlas_dispatches == kYcckJpegUploadIterations && ycck_jpeg_reference.within_tolerance &&
        ycck_jpeg_reference.checksum != 0 && oriented_jpeg_uploads == kOrientedJpegUploadIterations &&
        oriented_jpeg_uploaded_bytes == static_cast<u64>(kOrientedJpegUploadIterations) * 1'280u &&
        oriented_jpeg_reconstructed_pixels == static_cast<u64>(kOrientedJpegUploadIterations) * 384u &&
        oriented_jpeg_idct_dispatches == kOrientedJpegUploadIterations &&
        oriented_jpeg_atlas_dispatches == kOrientedJpegUploadIterations &&
        oriented_jpeg_reference.within_tolerance && oriented_jpeg_reference.checksum != 0 &&
        icc_jpeg_uploads == kIccJpegUploadIterations &&
        icc_jpeg_uploaded_bytes == static_cast<u64>(kIccJpegUploadIterations) * 1'280u &&
        icc_jpeg_reconstructed_pixels == static_cast<u64>(kIccJpegUploadIterations) * 384u &&
        icc_jpeg_idct_dispatches == kIccJpegUploadIterations &&
        icc_jpeg_atlas_dispatches == kIccJpegUploadIterations && icc_jpeg_reference.within_tolerance &&
        icc_jpeg_reference.checksum != 0 && icc_lut_jpeg_uploads == kIccLutJpegUploadIterations &&
        icc_lut_jpeg_uploaded_bytes == static_cast<u64>(kIccLutJpegUploadIterations) * 1'352u &&
        icc_lut_jpeg_reconstructed_pixels == static_cast<u64>(kIccLutJpegUploadIterations) * 384u &&
        icc_lut_jpeg_idct_dispatches == kIccLutJpegUploadIterations &&
        icc_lut_jpeg_atlas_dispatches == kIccLutJpegUploadIterations &&
        icc_lut_jpeg_reference.within_tolerance && icc_lut_jpeg_reference.checksum != 0 &&
        format_pixels_exact && format_pixel_checksum != 0 &&
        gpu.apng_frames == kApngSequenceIterations * 4u &&
        gpu.apng_dispatched_pixels == static_cast<u64>(kApngSequenceIterations) * 25u &&
        gpu.apng_source_blends == kApngSequenceIterations * 3u &&
        gpu.apng_over_blends == kApngSequenceIterations &&
        gpu.apng_background_disposals == kApngSequenceIterations &&
        gpu.apng_previous_disposals == kApngSequenceIterations && gpu.apng_backup_texture &&
        apng_pixels_exact && apng_pixel_checksum != 0 && gpu.gif_frames == kGifSequenceIterations * 4u &&
        gpu.gif_uploaded_bytes == static_cast<u64>(kGifSequenceIterations) * 29u &&
        gpu.gif_dispatched_pixels == static_cast<u64>(kGifSequenceIterations) * 29u &&
        gpu.gif_background_disposals == kGifSequenceIterations &&
        gpu.gif_previous_disposals == kGifSequenceIterations && gif_pixels_exact &&
        gif_pixel_checksum != 0 && animated_lossless_frames == kAnimatedWebpSequenceIterations * 4u &&
        animated_lossless_uploaded_bytes == static_cast<u64>(kAnimatedWebpSequenceIterations) * 128u &&
        animated_lossless_dispatched_pixels == static_cast<u64>(kAnimatedWebpSequenceIterations) * 32u &&
        animated_lossless_source_blends == kAnimatedWebpSequenceIterations * 2u &&
        animated_lossless_over_blends == kAnimatedWebpSequenceIterations * 2u &&
        animated_lossless_background_disposals == kAnimatedWebpSequenceIterations &&
        animated_webp_pixels_exact && animated_webp_pixel_checksum != 0 &&
        mixed_webp_decode_checksum != 0 &&
        mixed_webp_gpu_frames == kMixedAnimatedWebpSequenceIterations * 3u &&
        mixed_webp_gpu_uploaded_bytes == static_cast<u64>(kMixedAnimatedWebpSequenceIterations) * 80u &&
        mixed_webp_gpu_dispatched_pixels == static_cast<u64>(kMixedAnimatedWebpSequenceIterations) * 48u &&
        mixed_webp_gpu_source_blends == kMixedAnimatedWebpSequenceIterations * 2u &&
        mixed_webp_gpu_over_blends == kMixedAnimatedWebpSequenceIterations &&
        mixed_webp_gpu_background_disposals == kMixedAnimatedWebpSequenceIterations &&
        mixed_webp_gpu_reconstructions == kMixedAnimatedWebpSequenceIterations * 2u &&
        mixed_webp_gpu_filter_dispatches == kMixedAnimatedWebpSequenceIterations * 2u &&
        mixed_webp_gpu_atlas_uploads == kMixedAnimatedWebpSequenceIterations * 2u &&
        mixed_webp_pixels_exact && mixed_webp_pixel_checksum != 0 && request_checksum != 0 &&
        cache_checksum != 0 && pipeline_checksum != 0 && animation_scan_checksum != 0 &&
        animation_advance_checksum != 0 &&
        animation_timeline.counters.due_advances ==
            static_cast<u64>(kAnimationAdvanceBatchIterations) * kAnimationEntryCapacity;
    com_release(staging);
    com_release(end_query);
    com_release(begin_query);
    com_release(disjoint_query);
    media_atlas_gpu_release(&gpu);
    com_release(context1);
    com_release(context);
    com_release(device);
    return valid ? 0 : 24;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_media_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
