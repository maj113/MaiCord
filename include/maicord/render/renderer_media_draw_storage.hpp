#pragma once

#include "maicord/core/stable_storage.hpp"
#include "maicord/render/media_atlas.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/theme.hpp"

namespace maicord {

inline constexpr u32 kRendererMediaQuadCapacity = 4'096;
inline constexpr u32 kRendererMediaBatchCapacity = 512;
inline constexpr u32 kRendererMediaInitialQuadCapacity = 128;
inline constexpr u32 kRendererMediaInitialBatchCapacity = 32;

struct MediaQuad {
    float left;
    float top;
    float right;
    float bottom;
    float u0;
    float v0;
    float u1;
    float v1;
    // Media is untinted: only opacity varies. One 16-byte input also carries
    // the image and enclosing rounded-clip radii and a spoiler blur deviation
    // in layout units (zero: sharp).
    float opacity, radius, clip_radius, blur;
    UiRect rounded_clip;
};

struct MediaDrawBatch {
    UiRect clip;
    u32 first_quad;
    u32 quad_count;
};

// Stable-address, page-committed per-frame media draw scratch. It retains the
// 4,096-instance ceiling without permanently committing the uncommon tail.
struct RendererMediaDrawStorage {
    StableStorage quads;
    StableStorage handles;
    StableStorage domains;
    StableStorage batches;
    MediaQuad* quad_data;
    MediaAtlasHandle* handle_data;
    u8* domain_data;
    MediaDrawBatch* batch_data;
    u32 quad_capacity;
    u32 batch_capacity;
};

bool renderer_media_draw_storage_initialize(RendererMediaDrawStorage* storage) noexcept;
bool renderer_media_draw_storage_reserve_quads(RendererMediaDrawStorage* storage,
                                               u32 required_capacity) noexcept;
bool renderer_media_draw_storage_reserve_batches(RendererMediaDrawStorage* storage,
                                                 u32 required_capacity) noexcept;
void renderer_media_draw_storage_release(RendererMediaDrawStorage* storage) noexcept;

static_assert(sizeof(MediaQuad) == 64);
static_assert(sizeof(MediaDrawBatch) == 24);
static_assert(sizeof(RendererMediaDrawStorage) < 512);

} // namespace maicord
