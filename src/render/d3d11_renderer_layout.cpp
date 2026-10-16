#include "maicord/render/raster_clip.hpp"
#include "maicord/ui/profile_media.hpp"
#include "maicord/ui/friends_media.hpp"
#include "maicord/ui/search_avatar_media.hpp"
#include "maicord/ui/pinned_messages_avatar_media.hpp"
#include "maicord/ui/forum_channel_media.hpp"
#include "maicord/ui/inbox_avatar_media.hpp"
#include "maicord/ui/poll_voters_avatar_media.hpp"
#include "maicord/render/d3d11_renderer.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/presentation_policy.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/render/renderer_presentation.hpp"
#include "maicord/ui/login_presentation.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/message_action_bar_presentation.hpp"
#include "maicord/ui/message_author.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/ui/message_component_media.hpp"
#include "maicord/ui/attachment_actions.hpp"
#include "maicord/ui/audio_presentation.hpp"
#include "maicord/ui/poll_presentation.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/ui/media_picker_presentation.hpp"
#include "maicord/ui/reaction_viewer_presentation.hpp"
#include "maicord/ui/shell_presentation.hpp"
#include "maicord/ui/private_channel_rows.hpp"
#include "maicord/ui/voice_channel_rows.hpp"
#include "maicord/ui/voice_panel.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/render/inline_media.hpp"

#include "d3d11_renderer_draw.hpp"
#include "d3d11_renderer_layout.hpp"
#include "d3d11_renderer_edit.hpp"
#include "d3d11_renderer_interaction_state.hpp"
#include "d3d11_renderer_login_form.hpp"

#include <d3d11_1.h>

namespace maicord {
namespace {
// The build_layout line whose step rejected the last build (0: a return
// without a tag), logged with the layout failure.
u32 g_layout_failed_line = 0;
bool layout_step_failed(u32 line) noexcept {
    g_layout_failed_line = line;
    return false;
}


static_assert(sizeof(PaintQuad) == 64);
static_assert(sizeof(PaintBatch) == 12);
static_assert(sizeof(PaintStream) < 8 * 1'024);
static_assert(sizeof(GlyphQuad) == 48);
static_assert(sizeof(MediaQuad) == 64);
static_assert(sizeof(MediaDrawBatch) == 24);

bool paint_threads_launcher(const D3D11Renderer& renderer, const Theme& theme,
                            PaintStream* stream) noexcept {
    const auto* scene = renderer.ui_scene;
    const auto* button =
        scene ? ui_scene_resolve(*scene, ui_scene_find(*scene, UiSemantic::threads_toggle)) : nullptr;
    if (!button || !button->visible)
        return true;
    const auto rectangle = button->rectangle;
    const bool selected = d3d11_renderer_thread_browser_visible(renderer);
    const Color fill = selected ? theme.tokens.emphasis.fill
                                : ui_button_background(scene->widgets, button->widget, theme, {},
                                                       UiButtonSurface::palette);
    const Color ink = selected ? theme.tokens.emphasis.text : theme.text_secondary;
    const float extent = ui_icon_button_extent(minimum(ui_rect_width(rectangle), ui_rect_height(rectangle)));
    const float left = (rectangle.left + rectangle.right - extent) * .5f;
    const float top = (rectangle.top + rectangle.bottom - extent) * .5f;
    return paint_stream_push_clip(stream, rectangle.left, rectangle.top, rectangle.right,
                                  rectangle.bottom) &&
           paint_stream_push_rounded(stream, rectangle.left, rectangle.top, rectangle.right,
                                     rectangle.bottom, fill, 4.f) &&
           ui_icon_paint(stream, UiIcon::thread, {left, top, left + extent, top + extent}, ink) &&
           paint_stream_pop_clip(stream) &&
           paint_stream_record_content(stream, {&scene->widgets, button->widget}, rectangle,
                                       UiLayoutContentKind::control,
                                       ui_layout_content_bounds_allow_overlap);
}

void remove_covered_shell_paint(PaintStream* stream, UiRect panel) noexcept {
    u32 count = 0, batches = 0;
    for (u32 i = 0; i < stream->batch_count; ++i) {
        auto batch = stream->batches[i];
        const auto& clip = stream->clips[batch.clip_index];
        const u32 first = count;
        for (u32 j = 0; j < batch.quad_count; ++j) {
            const auto quad = stream->quads[batch.first_quad + j];
            const auto visible = ui_rect_intersection({quad.left, quad.top, quad.right, quad.bottom},
                                                      {clip.left, clip.top, clip.right, clip.bottom});
            if (visible.left >= panel.left && visible.top >= panel.top && visible.right <= panel.right &&
                visible.bottom <= panel.bottom)
                continue;
            stream->quads[count++] = quad;
        }
        if (first == count)
            continue;
        batch.first_quad = first;
        batch.quad_count = count - first;
        stream->batches[batches++] = batch;
    }
    stream->count = count;
    stream->batch_count = batches;
    stream->sealed_batches = minimum(stream->sealed_batches, batches);
}

bool composer_composition_geometry(const D3D11Renderer& renderer, Slice<TextCompositionMark> marks,
                                   u32* written) noexcept {
    *written = 0;
    const auto* scene = renderer.message_composer_scene;
    if (!scene || !scene->model || !renderer.composer_layout.active)
        return true;
    // An active IME composition replaces the spelling marks; those describe one
    // model text revision and are shown without focus.
    const bool composing = scene->model->focused && scene->composition.count;
    const auto spans = composing ? scene->composition
                       : scene->spelling_text_revision == scene->model->text_revision
                           ? scene->spelling
                           : ConstSlice<TextCompositionSpan>{};
    if ((!spans.data && spans.count) || spans.count > kTextCompositionSpanCapacity)
        return false;
    for (u32 i = 0; i < spans.count; ++i) {
        const auto& span = spans[i];
        u32 first = 0, last = 0;
        if (!message_composer_scene_composition_range(*scene, span, &first, &last))
            continue;
        TextEditMetric metrics[kTextEditSelectionCapacity]{};
        u32 count = 0;
        if (!text_edit_layout_range_geometry(renderer.composer_layout, first, last,
                                               {metrics, kTextEditSelectionCapacity}, &count))
            continue;
        for (u32 j = 0; j < count; ++j) {
            if (*written == marks.count)
                return true;
            const auto& metric = metrics[j];
            marks[(*written)++] = {
                {metric.left, metric.top, metric.left + metric.width, metric.top + metric.height}, span};
        }
    }
    return true;
}
D3D11_RECT physical_damage_rect(const D3D11Renderer& renderer, PresentationDamageRect rectangle) noexcept {
    const float pixel_scale = static_cast<float>(maximum(renderer.dpi, 1u)) / 96.0f;
    const RasterClipRect clip =
        raster_clip_rectangle(UiRect{rectangle.left, rectangle.top, rectangle.right, rectangle.bottom},
                              pixel_scale, renderer.width, renderer.height);
    return D3D11_RECT{clip.left, clip.top, clip.right, clip.bottom};
}

bool physical_rect_has_area(const RECT& rectangle) noexcept {
    return rectangle.left < rectangle.right && rectangle.top < rectangle.bottom;
}

RECT physical_rect_union(const RECT& left, const RECT& right) noexcept {
    return RECT{minimum(left.left, right.left), minimum(left.top, right.top),
                maximum(left.right, right.right), maximum(left.bottom, right.bottom)};
}

PresentationDamageRect media_quad_rectangle(const MediaQuad& quad) noexcept {
    return PresentationDamageRect{quad.left, quad.top, quad.right, quad.bottom};
}

PresentationDamageRect media_batch_clip(const MediaDrawBatch& batch) noexcept {
    return PresentationDamageRect{batch.clip.left, batch.clip.top, batch.clip.right, batch.clip.bottom};
}


bool update_media_quad_source(D3D11Renderer* renderer, const MediaAtlasResult& allocation, u8 domain,
                              MediaQuad* quad, bool* changed) noexcept {
    const bool preview = domain == 1;
    if (!renderer || !quad || !changed || !allocation.ok() || domain > 1 ||
        (preview ? (!renderer->preview_media_atlas || !renderer->media_gpu.preview_texture_view)
                 : (!renderer->media_atlas || !renderer->media_gpu.texture_view))) {
        return false;
    }
    GlyphAtlasAllocation cached_frame{};
    const bool cached_animation =
        !preview && animation_frame_cache_gpu_resolve(&renderer->animation_gpu, allocation, &cached_frame);
    const float source_x = cached_animation ? cached_frame.x : allocation.x;
    const float source_y = cached_animation ? cached_frame.y : allocation.y;
    const float source_width = cached_animation ? cached_frame.width : allocation.width;
    const float source_height = cached_animation ? cached_frame.height : allocation.height;
    // Atlas and frame-cache heights change at runtime (growth/idle trim);
    // UVs always use the current surface extent.
    const float inverse_width = 1.0f / (cached_animation ? renderer->animation_gpu.cache.allocations.width
                                        : preview        ? renderer->media_gpu.preview_width
                                                         : renderer->media_gpu.width);
    const float inverse_height = 1.0f / (cached_animation ? renderer->animation_gpu.cache.allocations.height
                                         : preview        ? renderer->media_gpu.preview_height
                                                          : renderer->media_gpu.height);
    float u0 = (source_x + 0.5f) * inverse_width;
    float v0 = (source_y + 0.5f) * inverse_height;
    float u1 = (source_x + source_width - 0.5f) * inverse_width;
    float v1 = (source_y + source_height - 0.5f) * inverse_height;
    if (cached_animation) {
        u0 = -(u0 + 1.0f);
        u1 = -(u1 + 1.0f);
    } else if (preview) {
        v0 = -(v0 + 1.0f);
        v1 = -(v1 + 1.0f);
    }
    *changed = quad->u0 != u0 || quad->v0 != v0 || quad->u1 != u1 || quad->v1 != v1;
    quad->u0 = u0;
    quad->v0 = v0;
    quad->u1 = u1;
    quad->v1 = v1;
    return true;
}

bool refresh_media_quad_sources(D3D11Renderer* renderer, bool* changed) noexcept {
    if (!renderer || !changed) {
        return false;
    }
    *changed = false;
    if (renderer->media_atlas) {
        media_atlas_begin_visibility(renderer->media_atlas);
    }
    if (renderer->preview_media_atlas) {
        media_atlas_begin_visibility(renderer->preview_media_atlas);
    }
    animation_frame_cache_gpu_begin_visibility(&renderer->animation_gpu);
    renderer_pin_settings_backdrop_media(renderer);
    if (renderer->media_quad_count == 0) {
        ++renderer->media_source_refreshes;
        return true;
    }
    if (!renderer->media_atlas || !renderer->media_gpu.texture_view) {
        return false;
    }
    u32 visited_quads = 0;
    for (u32 batch_index = 0; batch_index < renderer->media_batch_count; ++batch_index) {
        const MediaDrawBatch& batch = renderer->media_draw.batch_data[batch_index];
        if (batch.quad_count == 0 || batch.first_quad != visited_quads ||
            batch.first_quad > renderer->media_quad_count ||
            batch.quad_count > renderer->media_quad_count - batch.first_quad) {
            return false;
        }
        for (u32 offset = 0; offset < batch.quad_count; ++offset) {
            const u32 index = batch.first_quad + offset;
            const u8 domain = renderer->media_draw.domain_data[index];
            MediaAtlas* atlas = domain == 0 ? renderer->media_atlas : renderer->preview_media_atlas;
            MediaAtlasResult allocation{};
            if (!atlas ||
                !media_atlas_resolve_visible(atlas, renderer->media_draw.handle_data[index], &allocation)) {
                return false;
            }
            bool source_changed = false;
            if (!update_media_quad_source(renderer, allocation, domain,
                                          &renderer->media_draw.quad_data[index], &source_changed)) {
                return false;
            }
            if ((source_changed &&
                 !presentation_damage_add(&renderer->presentation_damage,
                                          media_quad_rectangle(renderer->media_draw.quad_data[index]),
                                          media_batch_clip(batch))) ||
                !presentation_damage_add_media(&renderer->presentation_damage,
                                               renderer->media_draw.handle_data[index],
                                               media_quad_rectangle(renderer->media_draw.quad_data[index]),
                                               media_batch_clip(batch), domain)) {
                return false;
            }
            *changed = *changed || source_changed;
        }
        visited_quads += batch.quad_count;
    }
    if (visited_quads != renderer->media_quad_count) {
        return false;
    }
    ++renderer->media_source_refreshes;
    return true;
}

bool queue_media_quad(D3D11Renderer* renderer, const MediaAtlasResult& allocation, float left, float top,
                      float right, float bottom, const UiRect& clip, bool rounded_mask = false,
                      bool force_new_batch = false, u8 domain = 0, float opacity = 1.f, float radius = 0.f,
                      UiRect rounded_clip = {}, float clip_radius = 0.f, float blur = 0.f) noexcept {
    // A retained row can intersect the viewport while its media does not.
    // Cull before pinning or reserving draw storage, including on source refreshes.
    if (!ui_rect_has_area(ui_rect_intersection(UiRect{left, top, right, bottom}, clip))) {
        return true;
    }
    const bool surface_ready =
        renderer && (domain == 0 ? renderer->media_atlas && renderer->media_gpu.texture_view
                                 : domain == 1 && renderer->preview_media_atlas &&
                                       renderer->media_gpu.preview_texture_view);
    if (!renderer || !allocation.ok() || !surface_ready || left >= right || top >= bottom ||
        renderer->media_quad_count >= kRendererMediaQuadCapacity ||
        !renderer_media_draw_storage_reserve_quads(&renderer->media_draw,
                                                   renderer->media_quad_count + 1u)) {
        if (renderer) {
            ++renderer->media_rejected;
        }
        return false;
    }
    MediaAtlas* atlas = domain == 0 ? renderer->media_atlas : renderer->preview_media_atlas;
    MediaAtlasResult visible_allocation{};
    if (!media_atlas_resolve_visible(atlas, allocation.handle, &visible_allocation)) {
        return false;
    }
    MediaDrawBatch* batch = renderer->media_batch_count != 0
                                ? &renderer->media_draw.batch_data[renderer->media_batch_count - 1]
                                : nullptr;
    const bool same_clip = !force_new_batch && batch && ui_rect_equal(batch->clip, clip) &&
                           batch->first_quad + batch->quad_count == renderer->media_quad_count;
    if (!same_clip) {
        if (renderer->media_batch_count >= kRendererMediaBatchCapacity ||
            !renderer_media_draw_storage_reserve_batches(&renderer->media_draw,
                                                         renderer->media_batch_count + 1u)) {
            ++renderer->media_rejected;
            return false;
        }
        batch = &renderer->media_draw.batch_data[renderer->media_batch_count++];
        *batch = MediaDrawBatch{clip, renderer->media_quad_count, 0};
    }
    if (rounded_mask)
        radius = minimum(right - left, bottom - top) * .5f;
    const u32 quad_index = renderer->media_quad_count;
    renderer->media_draw.quad_data[quad_index] = MediaQuad{
        left, top, right, bottom, 0.0f, 0.0f, 0.0f, 0.0f, opacity, radius, clip_radius, blur, rounded_clip,
    };
    bool source_changed = false;
    if (!update_media_quad_source(renderer, allocation, domain, &renderer->media_draw.quad_data[quad_index],
                                  &source_changed)) {
        return false;
    }
    renderer->media_draw.handle_data[quad_index] = allocation.handle;
    renderer->media_draw.domain_data[quad_index] = domain;
    ++renderer->media_quad_count;
    ++batch->quad_count;
    return true;
}

// A rail server icon: the still rendition unless the row animates, when the
// animation replaces it once resident. Either stands in while the other loads.
MediaAtlasResult guild_icon_rail_allocation(MediaAtlas* atlas, const GuildRecord& guild,
                                            bool animate) noexcept {
    const bool animated = asset_hash_animated(chat_state_store_guild_icon_hash(guild));
    const MediaAssetKey still = media_asset_guild_icon(guild.id, guild.icon_variant, false);
    const MediaAssetKey moving = media_asset_guild_icon(guild.id, guild.icon_variant, true);
    // A value-initialized MediaAtlasResult reads as ok (MediaAtlasError::none
    // is zero), so every branch returns a peek, never a default.
    if (animated && animate) {
        const MediaAtlasResult allocation = media_atlas_peek(atlas, moving);
        if (allocation.ok())
            return allocation;
    }
    const MediaAtlasResult allocation = media_atlas_peek(atlas, still);
    return allocation.ok() || !animated || animate ? allocation : media_atlas_peek(atlas, moving);
}

// Collapsed folder tiles preview their first four member icons.
bool queue_folder_preview(D3D11Renderer* renderer, const GuildRailRowWidget& row, UiRect tile) noexcept {
    const auto* view = renderer->navigation_view;
    for (u32 k = 0; view && k < 4 && k < row.view.folder_size; ++k) {
        const u32 source = row.source_index + 1 + k;
        if (source >= view->guild_count || renderer->media_quad_count >= kRendererMediaQuadCapacity)
            break;
        const auto* guild = chat_state_store_find_guild(*renderer->chat_state, view->guild_rows[source].id);
        if (!guild || !guild->icon_variant || !guild->icon_hash_bytes)
            continue;
        const auto allocation = guild_icon_rail_allocation(renderer->media_atlas, *guild, false);
        if (!allocation.ok())
            continue;
        const UiRect mini = guild_rail_folder_preview_rectangle(tile, k);
        if (!queue_media_quad(renderer, allocation, mini.left, mini.top, mini.right, mini.bottom,
                              renderer->guild_rail_scene->viewport, true) ||
            !paint_stream_record_content(&renderer->paint_stream,
                                         UiWidgetReference{&renderer->guild_rail_scene->widgets, row.widget}, mini,
                                         UiLayoutContentKind::media, ui_layout_content_bounds_allow_overlap))
            return false;
    }
    return true;
}
bool find_user_avatar(const D3D11Renderer& renderer, const UserRecord& user,
                      MediaAtlasResult* allocation) noexcept {
    if (!allocation || !renderer.media_atlas || user.avatar_hash_bytes == 0) {
        return false;
    }
    const Utf8View hash = user_store_avatar_hash(user);
    const u32 variant = user_avatar_variant(hash);
    if (variant == 0) {
        return false;
    }
    *allocation = media_atlas_peek(
        renderer.media_atlas, media_asset_user_avatar(user.id, variant, asset_hash_animated(hash)));
    return allocation->ok();
}

void reveal_media_underlay(D3D11Renderer& renderer, u32 media_before, UiRect rectangle,
                           Color fallback) noexcept {
    // Only an accepted native quad replaces a fallback. Keep all retained
    // geometry/batch/clip indices; alpha must reveal the actual surface below
    // the preview, rather than an opaque loading placeholder.
    if (renderer.media_quad_count == media_before)
        return;
    for (u32 i = renderer.paint_stream.count; i; --i) {
        auto& quad = renderer.paint_stream.quads[i - 1];
        if (quad.border_width < 0 ||
            !ui_rect_equal({quad.left, quad.top, quad.right, quad.bottom}, rectangle) ||
            quad.fill.red != fallback.red || quad.fill.green != fallback.green ||
            quad.fill.blue != fallback.blue || quad.fill.alpha != fallback.alpha)
            continue;
        quad.fill.alpha = 0;
        // Profile avatars retain their real panel-colored border/ring.
        if (!quad.border_width)
            quad.border.alpha = 0;
        return;
    }
}

// A turned image's orientation flags (media_draw.hlsl: 1 swaps axes, 2 and 4
// mirror u and v), carried as a negative blur parameter.
float media_orientation(u16 rotation) noexcept {
    return rotation == 90 ? -5.f : rotation == 180 ? -6.f : rotation == 270 ? -3.f : 0.f;
}
// Profile card media (popout, full profile and the profile editor's preview):
// banner, avatar, decoration, badges and activity art, then the profile
// effect layers over the whole card. Local images (a picked avatar or banner)
// come from the preview atlas and are masked to their visible region. The
// banner and effects are masked with the card's own rounded shape, so neither
// shows past its corners (the modal's card included).
bool queue_profile_media(D3D11Renderer* renderer, const ProfileScene& profile, const Theme& theme,
                         u32* effect_first_batch) noexcept {
    bool first = true;
    const ProfileCardMask card = profile_scene_card_mask(profile, theme);
    for (u32 i = 0; i < profile.part_count; ++i) {
        ProfileMedia media{};
        if (!profile_media_resolve(profile, i, &media))
            continue;
        const bool local = media.domain == 1;
        if (local && (!renderer->preview_media_atlas || !renderer->media_gpu.preview_texture_view))
            continue;
        const auto allocation =
            local ? media_atlas_peek(renderer->preview_media_atlas, media.key) : media_atlas_find(renderer->media_atlas, media.key);
        const auto r = media.rectangle;
        const bool masked = ui_rect_has_area(media.mask);
        const u32 media_before = renderer->media_quad_count;
        if (allocation.ok() &&
            !queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip,
                              media.circular && !masked, first, local ? 1 : 0, media.opacity > 0 ? media.opacity : 1,
                              0, masked ? media.mask : card.rectangle, masked ? media.mask_radius : card.radius,
                              media_orientation(media.rotation)))
            return false;
        if (renderer->media_quad_count == media_before)
            continue;
        first = false;
        const auto kind = profile.parts[i].media;
        if (kind == ProfileMediaKind::avatar) {
            const UiRect ring = masked ? media.mask : r;
            reveal_media_underlay(*renderer, media_before, {ring.left - 6, ring.top - 6, ring.right + 6, ring.bottom + 6},
                                  theme.surface_high);
        } else if (kind == ProfileMediaKind::banner) {
            const u32 accent = profile.profile->accent_color;
            const Color fallback = accent ? Color{((accent >> 16) & 255) / 255.f, ((accent >> 8) & 255) / 255.f,
                                                  (accent & 255) / 255.f, 1}
                                          : theme.surface_high;
            reveal_media_underlay(*renderer, media_before, profile.banner, fallback);
        }
    }
    // Effects composite above card glyphs, separately from banner/avatar media.
    *effect_first_batch = renderer->media_batch_count;
    first = true;
    const u64 effect_now_ms = profile.effect && profile.effect_started_ms ? GetTickCount64() : 0;
    for (u32 i = 0; profile.effect && (profile.reduced_motion || profile.effect_started_ms) &&
                    i < maximum<u8>(1, profile.effect->count);
         ++i) {
        ProfileMedia media{};
        if (!profile.effect_started_ms || effect_now_ms < profile.effect_started_ms ||
            !profile_effect_media_resolve(profile, i, effect_now_ms, &media))
            continue;
        const auto allocation = media_atlas_find(renderer->media_atlas, media.key);
        auto r = media.rectangle;
        if (allocation.ok() && allocation.width) {
            r.bottom = r.top + ui_rect_width(r) * allocation.height / allocation.width;
            // Hovering the card dims its effect (ProfileMedia::opacity).
            if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip, false, first, 0,
                                  media.opacity > 0 ? media.opacity : 1, 0, card.rectangle, card.radius))
                return false;
            first = false;
        }
    }
    return true;
}

// The profile editor's settings overlays, each in its own clip so its first
// quad opens a batch: paint, media (main or preview atlas), foreground. The
// preview card belongs to the page (below a choice popup); the dialog sits
// above the page and its popup, below text forms.
bool queue_settings_profile_overlays(D3D11Renderer* renderer, const Theme& theme, bool dialog) noexcept {
    auto& paint = renderer->paint_stream;
    u32& paint_first =
        dialog ? renderer->settings_dialog_paint_first_batch : renderer->settings_preview_paint_first_batch;
    u32& foreground = dialog ? renderer->settings_dialog_foreground_first_batch
                             : renderer->settings_preview_foreground_first_batch;
    u32& paint_end =
        dialog ? renderer->settings_dialog_paint_end_batch : renderer->settings_preview_paint_end_batch;
    u32& media_first =
        dialog ? renderer->settings_dialog_media_first_batch : renderer->settings_preview_media_first_batch;
    u32& media_end =
        dialog ? renderer->settings_dialog_media_end_batch : renderer->settings_preview_media_end_batch;
    paint_first = foreground = paint_end = paint.batch_count;
    media_first = media_end = renderer->media_batch_count;
    if (!dialog) {
        renderer->settings_preview_effect_media_first_batch = media_end;
        renderer->settings_preview_controls_first_batch = paint_end;
    }
    const auto* preview = renderer->settings_profile_preview;
    const auto* scene = renderer->settings_profile_dialog;
    if (dialog ? !(scene && scene->active) : !(preview && preview->active))
        return true;
    // The preview card is laid out in the page's visible area, so it scrolls
    // under the page's edges instead of over them.
    const auto viewport = dialog ? renderer->settings_scene->viewport : preview->viewport;
    const auto section = [&](auto&& emit) noexcept {
        return paint_stream_push_clip(&paint, viewport.left, viewport.top, viewport.right, viewport.bottom) && emit() &&
               paint_stream_pop_clip(&paint);
    };
    if (!section([&]() noexcept {
            return dialog ? ui_profile_dialog_paint(*scene, theme, &paint) : profile_scene_paint(*preview, theme, &paint);
        }))
        return false;
    media_first = renderer->media_batch_count;
    if (dialog) {
        bool first = true;
        ProfileMedia media{};
        for (u32 i = 0; ui_profile_dialog_media(*scene, i, &media); ++i, media = {}) {
            const bool local = media.domain == 1;
            if (local && (!renderer->preview_media_atlas || !renderer->media_gpu.preview_texture_view))
                continue;
            const auto allocation = local ? media_atlas_peek(renderer->preview_media_atlas, media.key)
                                          : media_atlas_find(renderer->media_atlas, media.key);
            const auto r = media.rectangle;
            const bool masked = ui_rect_has_area(media.mask);
            const u32 before = renderer->media_quad_count;
            if (allocation.ok() &&
                !queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip,
                                  media.circular && !masked, first, local ? 1 : 0,
                                  media.opacity > 0 ? media.opacity : 1, 0, masked ? media.mask : UiRect{},
                                  masked ? media.mask_radius : 0, media_orientation(media.rotation)))
                return false;
            first = first && renderer->media_quad_count == before;
        }
    } else if (!queue_profile_media(renderer, *preview, theme,
                                    &renderer->settings_preview_effect_media_first_batch))
        return false;
    media_end = renderer->media_batch_count;
    foreground = paint.batch_count;
    if (!section([&]() noexcept {
            return dialog ? ui_profile_dialog_foreground(*scene, theme, &paint)
                          : profile_scene_foreground(*preview, theme, &paint, nullptr, renderer->dpi,
                                                     &renderer->settings_preview_controls_first_batch);
        }))
        return false;
    paint_end = paint.batch_count;
    return true;
}

// Go Live preview thumbnails (embed keys): the large preview atlas when the
// image was reduced there, else the primary atlas.
bool find_call_preview(const D3D11Renderer& renderer, MediaAssetKey key, MediaAtlasResult* allocation,
                       u8* domain) noexcept {
    if (!media_asset_key_valid(key))
        return false;
    if (renderer.preview_media_atlas && renderer.media_gpu.preview_texture_view) {
        *allocation = media_atlas_peek(renderer.preview_media_atlas, key);
        *domain = 1;
        if (allocation->ok())
            return true;
    }
    if (renderer.media_atlas && renderer.media_gpu.texture_view) {
        *allocation = media_atlas_peek(renderer.media_atlas, key);
        *domain = 0;
        return allocation->ok();
    }
    return false;
}

// Call tiles: the resident video picture (preview atlas), otherwise the
// participant's avatar over its placeholder circle, or a stream's preview.
bool queue_call_media(D3D11Renderer* renderer, const Theme& theme) noexcept {
    const auto& scene = *renderer->call_scene;
    bool first = true;
    for (u32 i = 0; i < scene.tile_count && renderer->media_quad_count < kRendererMediaQuadCapacity &&
                    renderer->media_batch_count < kRendererMediaBatchCapacity;
         ++i) {
        UiCallTileMedia media{};
        if (!ui_call_scene_tile_media(scene, i, &media))
            continue;
        const u32 before = renderer->media_quad_count;
        if (media.video_ssrc && ui_rect_has_area(media.video) && renderer->preview_media_atlas &&
            renderer->media_gpu.preview_texture_view) {
            const auto allocation = media_atlas_peek(renderer->preview_media_atlas,
                                                     media_asset_call_video(media.user_id, media.video_ssrc));
            const auto r = media.video;
            if (allocation.ok() &&
                !queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip, false, first,
                                  1, 1.f, 0.f, media.rounded_clip, media.radius))
                return false;
        }
        if (renderer->media_quad_count == before && media_asset_key_valid(media.preview)) {
            MediaAtlasResult allocation{};
            u8 domain = 0;
            if (find_call_preview(*renderer, media.preview, &allocation, &domain)) {
                const auto r = ui_call_video_fit(media.rounded_clip, allocation.width, allocation.height, false);
                // Under a state scrim the preview is softened as well.
                if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip, false, first,
                                      domain, 1.f, 0.f, media.rounded_clip, media.radius, media.dim ? 6.f : 0.f))
                    return false;
            }
        }
        if (renderer->media_quad_count == before && ui_rect_has_area(media.avatar) && renderer->user_store &&
            renderer->media_atlas && renderer->media_gpu.texture_view) {
            const auto* user = user_store_find(*renderer->user_store, media.user_id);
            MediaAtlasResult allocation{};
            const auto a = media.avatar;
            if (user && find_user_avatar(*renderer, *user, &allocation)) {
                if (!queue_media_quad(renderer, allocation, a.left, a.top, a.right, a.bottom, media.clip, true, first))
                    return false;
                reveal_media_underlay(*renderer, before, a, theme.avatar_placeholder);
            }
        }
        first = first && renderer->media_quad_count == before;
    }
    // Stage audience and hand-raise avatars over their placeholders.
    UiCallTileMedia media{};
    for (u32 i = 0; renderer->user_store && renderer->media_atlas && renderer->media_gpu.texture_view &&
                    renderer->media_quad_count < kRendererMediaQuadCapacity &&
                    renderer->media_batch_count < kRendererMediaBatchCapacity &&
                    ui_call_scene_stage_avatar(scene, i, &media);
         ++i) {
        const auto* user = user_store_find(*renderer->user_store, media.user_id);
        MediaAtlasResult allocation{};
        const u32 before = renderer->media_quad_count;
        const auto a = media.avatar;
        if (!user || !find_user_avatar(*renderer, *user, &allocation))
            continue;
        if (!queue_media_quad(renderer, allocation, a.left, a.top, a.right, a.bottom, media.clip, true, first))
            return false;
        reveal_media_underlay(*renderer, before, a, theme.avatar_placeholder);
        first = first && renderer->media_quad_count == before;
    }
    return true;
}

bool queue_inline_flags(D3D11Renderer* renderer) noexcept {
    renderer->inline_media_first_batch = renderer->media_batch_count;
    for (u32 layer = 0; layer < static_cast<u32>(RendererInlineMediaLayer::count); ++layer) {
        renderer->inline_media_layer_first[layer] = renderer->media_batch_count;
        bool first = true;
        for (u32 i = 0; renderer->inline_media.flags &&
                        i < minimum(renderer->inline_media.count, kRendererInlineFlagCapacity);
             ++i) {
            const auto& flag = renderer->inline_media.flags[i];
            if (static_cast<u32>(flag.layer) != layer || !renderer_inline_flag_current(*renderer, flag) ||
                !renderer->media_atlas || !renderer->media_gpu.texture_view)
                continue;
            const auto allocation =
                media_atlas_peek(renderer->media_atlas, media_asset_regional_flag(flag.flag));
            if (!allocation.ok())
                continue;
            if (renderer->media_quad_count == kRendererMediaQuadCapacity ||
                renderer->media_batch_count == kRendererMediaBatchCapacity) {
                saturating_increment(renderer->inline_media.rejected);
                break;
            }
            const auto r = flag.rectangle;
            if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, flag.clip, false,
                                  first))
                return false;
            first = false;
        }
        // A reaction tooltip's custom emoji or flag: over its surface, under
        // its text, fitted to its square.
        UiRect square{};
        if (layer == static_cast<u32>(RendererInlineMediaLayer::form) && d3d11_renderer_tooltip_visible(*renderer) &&
            media_asset_key_valid(renderer->tooltip_scene->emoji_media) &&
            ui_tooltip_emoji(*renderer->tooltip_scene, &square) && renderer->media_atlas &&
            renderer->media_gpu.texture_view && renderer->media_quad_count < kRendererMediaQuadCapacity &&
            renderer->media_batch_count < kRendererMediaBatchCapacity) {
            const auto allocation = media_atlas_peek(renderer->media_atlas, renderer->tooltip_scene->emoji_media);
            if (allocation.ok() && allocation.width && allocation.height) {
                const float scale = minimum(ui_rect_width(square) / allocation.width,
                                            ui_rect_height(square) / allocation.height);
                const float width = allocation.width * scale, height = allocation.height * scale;
                const float left = (square.left + square.right - width) * .5f,
                            top = (square.top + square.bottom - height) * .5f;
                if (!queue_media_quad(renderer, allocation, left, top, left + width, top + height,
                                      renderer->tooltip_scene->viewport, false, first))
                    return false;
                first = false;
            }
        }
        if (layer == static_cast<u32>(RendererInlineMediaLayer::forum)) {
            renderer->forum_media_first_batch = renderer->media_batch_count;
            if (renderer->forum_form && renderer->forum_form->scene &&
                renderer->forum_form->scene->active && renderer->preview_media_atlas &&
                renderer->media_gpu.preview_texture_view) {
                const auto& scene = *renderer->forum_form->scene;
                const auto& list = scene.attachment_list;
                for (u32 row = 0; row < list.rows.count; ++row) {
                    const auto& view = list.rows[row];
                    if (!view.preview_ready || view.spoiler)
                        continue;
                    UiRect clip{};
                    const auto tile = ui_attachment_draft_list_preview(list, row, &clip);
                    if (!ui_rect_has_area(clip))
                        continue;
                    const auto allocation = media_atlas_peek(renderer->preview_media_atlas, view.preview);
                    if (!allocation.ok() || !allocation.width || !allocation.height)
                        continue;
                    const float scale = minimum(ui_rect_width(tile) / allocation.width,
                                                ui_rect_height(tile) / allocation.height);
                    const float width = allocation.width * scale, height = allocation.height * scale;
                    const float left = (tile.left + tile.right - width) * .5f,
                                top = (tile.top + tile.bottom - height) * .5f;
                    if (!queue_media_quad(renderer, allocation, left, top, left + width, top + height, clip,
                                          false, first, 1, 1.f, 0, tile, list.style.radius) ||
                        !paint_stream_record_content(
                            &renderer->paint_stream, {&scene.widgets, list.parts[row][2]}, tile,
                            UiLayoutContentKind::media, ui_layout_content_bounds_allow_overlap))
                        return false;
                    first = false;
                }
            }
            renderer->forum_media_end_batch = renderer->media_batch_count;
        }
        renderer->inline_media_layer_end[layer] = renderer->media_batch_count;
    }
    renderer->inline_media_last_batch = renderer->media_batch_count;
    return true;
}
bool split_paint_quad_boundary(PaintStream* paint, u32 quad, u32* batch_index) noexcept {
    if (!paint || !batch_index || quad > paint->count)
        return false;
    for (u32 i = 0; i < paint->batch_count; ++i) {
        auto& batch = paint->batches[i];
        if (quad == batch.first_quad) {
            *batch_index = i;
            return true;
        }
        if (quad > batch.first_quad && quad < batch.first_quad + batch.quad_count) {
            if (paint->batch_count == kMaxPaintBatches)
                return false;
            const auto tail = PaintBatch{quad, batch.first_quad + batch.quad_count - quad, batch.clip_index,
                                         batch.reserved};
            batch.quad_count = quad - batch.first_quad;
            for (u32 j = paint->batch_count; j > i + 1; --j)
                paint->batches[j] = paint->batches[j - 1];
            paint->batches[i + 1] = tail;
            ++paint->batch_count;
            *batch_index = i + 1;
            return true;
        }
    }
    // A boundary at the end of the stream closes the last batch: later
    // quads belong to the next range.
    *batch_index = paint_stream_seal(paint);
    return true;
}
u64 shell_segment_key(const RendererPresentationStamp& stamp, const MessageComposerPresentationInput& composer,
                      const PaintStream& paint) noexcept {
    // The composer's caret and selection reach the chrome as geometry; the
    // stamp sees them only through content text, which is a timeline input.
    u64 key = hash64_mix(stamp.shell, stamp.shell_count);
    key = hash64_counted_bytes(key, composer.selections.data,
                               composer.selections.count * static_cast<u32>(sizeof(UiRect)));
    key = hash64_counted_bytes(key, &composer.caret, sizeof(composer.caret));
    key = hash64_mix(key, (composer.geometry_active ? 1u : 0u) | (composer.caret_visible ? 2u : 0u));
    key = hash64_counted_bytes(key, composer.composition.data,
                               composer.composition.count * static_cast<u32>(sizeof(TextCompositionMark)));
    key = hash64_counted_bytes(key, &paint.scrollbar_viewport, sizeof(paint.scrollbar_viewport));
    return key | 1;
}

bool shell_segment_storage(RendererShellSegment* segment) noexcept {
    if (segment->paint_quads.arena.base)
        return true;
    if (stable_storage_initialize(&segment->paint_quads, kMaxPaintQuads, 0, sizeof(PaintQuad), alignof(PaintQuad)) &&
        stable_storage_initialize(&segment->media_quads, kRendererMediaQuadCapacity, 0, sizeof(MediaQuad),
                                  alignof(MediaQuad)) &&
        stable_storage_initialize(&segment->media_handles, kRendererMediaQuadCapacity, 0,
                                  sizeof(MediaAtlasHandle), alignof(MediaAtlasHandle)) &&
        stable_storage_initialize(&segment->media_domains, kRendererMediaQuadCapacity, 0, 1, 1) &&
        stable_storage_initialize(&segment->media_batches, kRendererMediaBatchCapacity, 0, sizeof(MediaDrawBatch),
                                  alignof(MediaDrawBatch)))
        return true;
    d3d11_renderer_shell_segment_release(segment);
    return false;
}

// Records the shell segment just built: paint [0, count) with its clips and
// batches, and media [0, count). The chrome regions are those before the
// thread header narrowed the message view.
void shell_segment_capture(D3D11Renderer* renderer, u64 key, const ShellPresentationRegions& regions) noexcept {
    RendererShellSegment& segment = renderer->shell_segment;
    segment.valid = false;
    const PaintStream& paint = renderer->paint_stream;
    if (!key || paint.rejected || paint.clip_depth != 1 || !shell_segment_storage(&segment))
        return;
    const u32 media = renderer->media_quad_count, media_batches = renderer->media_batch_count;
    if ((paint.count && !stable_storage_reserve(&segment.paint_quads, paint.count)) ||
        (media && (!stable_storage_reserve(&segment.media_quads, media) ||
                   !stable_storage_reserve(&segment.media_handles, media) ||
                   !stable_storage_reserve(&segment.media_domains, media))) ||
        (media_batches && !stable_storage_reserve(&segment.media_batches, media_batches)))
        return;
    CopyMemory(stable_storage_data<PaintQuad>(segment.paint_quads), paint.quads,
               static_cast<usize>(paint.count) * sizeof(PaintQuad));
    CopyMemory(segment.clips, paint.clips, static_cast<usize>(paint.clip_count) * sizeof(PaintClip));
    CopyMemory(segment.batches, paint.batches, static_cast<usize>(paint.batch_count) * sizeof(PaintBatch));
    CopyMemory(stable_storage_data<MediaQuad>(segment.media_quads), renderer->media_draw.quad_data,
               static_cast<usize>(media) * sizeof(MediaQuad));
    CopyMemory(stable_storage_data<MediaAtlasHandle>(segment.media_handles), renderer->media_draw.handle_data,
               static_cast<usize>(media) * sizeof(MediaAtlasHandle));
    CopyMemory(stable_storage_data<u8>(segment.media_domains), renderer->media_draw.domain_data, media);
    CopyMemory(stable_storage_data<MediaDrawBatch>(segment.media_batches), renderer->media_draw.batch_data,
               static_cast<usize>(media_batches) * sizeof(MediaDrawBatch));
    segment.message_view = regions.message_view;
    segment.member_sidebar = regions.member_sidebar;
    segment.member_sidebar_visible = regions.member_sidebar_visible;
    segment.paint_count = paint.count;
    segment.clip_count = paint.clip_count;
    segment.batch_count = paint.batch_count;
    segment.media_count = media;
    segment.media_batch_count = media_batches;
    segment.key = key;
    segment.valid = true;
    saturating_increment(segment.builds);
}

// Restores the recorded shell segment into the reset streams. Its media keep
// the per-frame work of queue_media_quad: atlas pins (eviction safety) and
// current sources (animation frames, atlas growth).
bool shell_segment_replay(D3D11Renderer* renderer, u64 key, float width_dip, float height_dip,
                          ShellPresentationRegions* regions) noexcept {
    RendererShellSegment& segment = renderer->shell_segment;
    if (!segment.valid || segment.key != key)
        return false;
    PaintStream& paint = renderer->paint_stream;
    const auto abandon = [&]() noexcept {
        segment.valid = false;
        renderer->media_quad_count = 0;
        renderer->media_batch_count = 0;
        return false;
    };
    if (!paint_stream_reset(&paint, width_dip, height_dip) ||
        (segment.paint_count > paint.quad_capacity &&
         !stable_storage_grow(&paint.quad_storage, segment.paint_count)) ||
        !renderer_media_draw_storage_reserve_quads(&renderer->media_draw, maximum(segment.media_count, 1u)) ||
        !renderer_media_draw_storage_reserve_batches(&renderer->media_draw, maximum(segment.media_batch_count, 1u)))
        return abandon();
    paint.quad_capacity = paint.quad_storage.capacity;
    CopyMemory(paint.quads, stable_storage_data<PaintQuad>(segment.paint_quads),
               static_cast<usize>(segment.paint_count) * sizeof(PaintQuad));
    CopyMemory(paint.clips, segment.clips, static_cast<usize>(segment.clip_count) * sizeof(PaintClip));
    CopyMemory(paint.batches, segment.batches, static_cast<usize>(segment.batch_count) * sizeof(PaintBatch));
    paint.count = segment.paint_count;
    paint.clip_count = segment.clip_count;
    paint.batch_count = segment.batch_count;
    CopyMemory(renderer->media_draw.quad_data, stable_storage_data<MediaQuad>(segment.media_quads),
               static_cast<usize>(segment.media_count) * sizeof(MediaQuad));
    CopyMemory(renderer->media_draw.handle_data, stable_storage_data<MediaAtlasHandle>(segment.media_handles),
               static_cast<usize>(segment.media_count) * sizeof(MediaAtlasHandle));
    CopyMemory(renderer->media_draw.domain_data, stable_storage_data<u8>(segment.media_domains),
               segment.media_count);
    CopyMemory(renderer->media_draw.batch_data, stable_storage_data<MediaDrawBatch>(segment.media_batches),
               static_cast<usize>(segment.media_batch_count) * sizeof(MediaDrawBatch));
    renderer->media_quad_count = segment.media_count;
    renderer->media_batch_count = segment.media_batch_count;
    for (u32 i = 0; i < segment.media_count; ++i) {
        const u8 domain = renderer->media_draw.domain_data[i];
        MediaAtlas* atlas = domain == 0 ? renderer->media_atlas : renderer->preview_media_atlas;
        MediaAtlasResult visible{};
        bool changed = false;
        if (domain > 1 || !media_atlas_resolve_visible(atlas, renderer->media_draw.handle_data[i], &visible) ||
            !update_media_quad_source(renderer, visible, domain, &renderer->media_draw.quad_data[i], &changed))
            return abandon();
    }
    *regions = ShellPresentationRegions{segment.message_view, segment.member_sidebar,
                                        segment.member_sidebar_visible};
    saturating_increment(segment.replays);
    return true;
}

bool build_layout(D3D11Renderer* renderer, const Theme& theme,
                  const RendererPresentationStamp* stamp = nullptr, bool replay_shell = false) noexcept {
    renderer->theme_editor_paint_first_batch = renderer->theme_editor_paint_end_batch = 0;
    renderer->quick_switcher_status_first_batch = renderer->quick_switcher_status_end_batch = 0;
    renderer->settings_page_paint_first_batch = renderer->settings_page_paint_end_batch = 0;
    renderer->settings_modal_paint_first_batch = renderer->settings_modal_paint_end_batch = 0;
    renderer->popup_paint_first_batch = renderer->popup_paint_end_batch = 0;
    renderer->popup_scene_media_first_batch = renderer->popup_scene_media_end_batch = 0;
    renderer->settings_choice_paint_first_batch = renderer->settings_choice_paint_end_batch = 0;
    renderer->settings_choice_media_first_batch = renderer->settings_choice_media_end_batch = 0;
    renderer->settings_page_media_first_batch = renderer->settings_page_media_end_batch = 0;
    renderer->media_picker_paint_first_batch = renderer->media_picker_paint_end_batch = 0;
    renderer->media_picker_foreground_first_batch = 0;
    renderer->media_picker_media_first_batch = renderer->media_picker_media_end_batch = 0;
    // Fractional-DIP viewport, as in the retained scene: integer division would
    // disagree at 125%/150% scales and the rectangle guard below would reject
    // valid frames.
    const float width_dip =
        static_cast<float>(renderer->width) * 96.0f / static_cast<float>(maximum<u32>(renderer->dpi, 1));
    const float height_dip =
        static_cast<float>(renderer->height) * 96.0f / static_cast<float>(maximum<u32>(renderer->dpi, 1));
    if (renderer->media_atlas) {
        media_atlas_begin_visibility(renderer->media_atlas);
    }
    if (renderer->preview_media_atlas) {
        media_atlas_begin_visibility(renderer->preview_media_atlas);
    }
    animation_frame_cache_gpu_begin_visibility(&renderer->animation_gpu);
    renderer->paint_stream.filter_scrollbars = true;
    if (!renderer->login_visible && renderer_settings_page_visible(*renderer)) {
        // The shell beneath is the backdrop texture; keep what it shows resident.
        renderer_pin_settings_backdrop_media(renderer);
        renderer->paint_stream.filter_scrollbars = false;
        paint_stream_reset(&renderer->paint_stream);
        renderer->media_quad_count = 0;
        renderer->media_batch_count = 0;
        renderer->media_overlay_paint_first_batch = 0;
        renderer->profile_foreground_first_batch = 0;
        renderer->popup_media_first_batch = 0;
        renderer->menu_paint_first_batch = 0;
        renderer->menu_media_first_batch = 0;
        renderer->overlay_paint_first_batch = 0;
        renderer->edit_overlay_paint_first_batch = 0;
        renderer->form_paint_first_batch = 0;
        UiSettingsPaintRanges ranges{};
        // Split in ascending quad order so earlier batch indices stay valid.
        if (!ui_settings_scene_paint(*renderer->settings_scene, theme, &renderer->paint_stream,
                                     renderer->dpi, &ranges) ||
            !split_paint_quad_boundary(&renderer->paint_stream, ranges.modal_first_quad,
                                       &renderer->settings_modal_paint_first_batch) ||
            !split_paint_quad_boundary(&renderer->paint_stream, ranges.page_first_quad,
                                       &renderer->settings_page_paint_first_batch) ||
            !split_paint_quad_boundary(&renderer->paint_stream, ranges.page_end_quad,
                                       &renderer->settings_page_paint_end_batch) ||
            !split_paint_quad_boundary(&renderer->paint_stream, ranges.modal_end_quad,
                                       &renderer->settings_modal_paint_end_batch))
            return layout_step_failed(__LINE__);
        // Resident avatars replace their rows' loading placeholder, which a
        // transparent image must not show through; misses keep it.
        renderer->settings_page_media_first_batch = renderer->media_batch_count;
        if (renderer->media_atlas && renderer->media_gpu.texture_view) {
            UiSettingsAvatarGeometry avatar{};
            bool first = true;
            for (u32 cursor = 0; ui_settings_scene_next_avatar(*renderer->settings_scene, &cursor, &avatar);) {
                const auto allocation = media_atlas_peek(renderer->media_atlas, avatar.media->key);
                if (!allocation.ok())
                    continue;
                const auto r = avatar.avatar;
                const u32 before = renderer->media_quad_count;
                // Emoji and sticker previews keep their own shape.
                const auto kind = avatar.media->key.kind;
                const bool round = kind != MediaAssetKind::custom_emoji && kind != MediaAssetKind::sticker;
                if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, avatar.clip, round,
                                      first))
                    return layout_step_failed(__LINE__);
                reveal_media_underlay(*renderer, before, r, theme.avatar_placeholder);
                first = first && renderer->media_quad_count == before;
            }
        }
        renderer->settings_page_media_end_batch = renderer->media_batch_count;
        if (!queue_settings_profile_overlays(renderer, theme, false))
            return layout_step_failed(__LINE__);
        renderer->form_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
        const auto* popup = d3d11_renderer_settings_popup(*renderer);
        renderer->settings_choice_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
        if (popup && !popup_presentation_build(*popup, theme, &renderer->paint_stream))
            return layout_step_failed(__LINE__);
        renderer->settings_choice_paint_end_batch = paint_stream_seal(&renderer->paint_stream);
        if (!queue_settings_profile_overlays(renderer, theme, true))
            return layout_step_failed(__LINE__);
        renderer->form_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
        if (renderer->forum_form && renderer->forum_form->scene && renderer->forum_form->scene->active &&
            !forum_post_paint(*renderer->forum_form->scene, renderer->forum_form->geometry, theme,
                              &renderer->paint_stream))
            return layout_step_failed(__LINE__);
        renderer->forum_paint_end_batch = paint_stream_seal(&renderer->paint_stream);
        return (!renderer->text_form || !renderer->text_form->scene ||
                !renderer->text_form->scene->active ||
                text_form_paint(*renderer->text_form->scene, renderer->text_form->geometry, theme,
                                &renderer->paint_stream)) &&
               queue_inline_flags(renderer);
    }
    renderer->paint_stream.scrollbar_viewport = renderer->scrollbar_viewport;
    if (renderer->popup_scene && renderer->popup_scene->active) {
        renderer->paint_stream.scrollbar_viewport = renderer->popup_scene->rectangle;
    } else if ((renderer->image_viewer && renderer->image_viewer->active) ||
               ((renderer->text_form && renderer->text_form->scene && renderer->text_form->scene->active) ||
                (renderer->forum_form && renderer->forum_form->scene &&
                 renderer->forum_form->scene->active)) ||
               (renderer->reaction_viewer && renderer->reaction_viewer->active))
        renderer->paint_stream.scrollbar_viewport = {};
    if (renderer->profile_scene && renderer->profile_scene->active &&
        !ui_rect_equal(renderer->paint_stream.scrollbar_viewport,
                       renderer->profile_scene->scroll_viewport) &&
        !ui_rect_equal(renderer->paint_stream.scrollbar_viewport,
                       renderer->profile_scene->secondary_viewport))
        renderer->paint_stream.scrollbar_viewport = {};
    if (renderer->login_visible) {
        if (!renderer->login_scene || renderer->login_scene->widgets.count == 0) {
            return layout_step_failed(__LINE__);
        }
        renderer->media_quad_count = 0;
        renderer->media_batch_count = 0;
        renderer->popup_media_first_batch = 0;
        const LoginScene& login = *renderer->login_scene;
        UiRect selections[kRendererLoginSelectionCapacity]{};
        for (u32 index = 0; index < renderer->login_edit.selection_count; ++index) {
            const TextEditMetric& metric = renderer->login_edit.selections[index];
            selections[index] =
                UiRect{metric.left, metric.top, metric.left + metric.width, metric.top + metric.height};
        }
        LoginCompositionGeometry composition[kRendererLoginCompositionCapacity]{};
        for (u32 index = 0; index < renderer->login_composition_metric_count; ++index) {
            const RendererLoginCompositionMetric& source = renderer->login_composition_metrics[index];
            composition[index] = LoginCompositionGeometry{UiRect{source.metric.left, source.metric.top,
                                                                 source.metric.left + source.metric.width,
                                                                 source.metric.top + source.metric.height},
                                                          source.span};
        }
        const TextEditMetric& caret = renderer->login_edit.caret;
        const bool form =
            renderer->text_form && renderer->text_form->scene && renderer->text_form->scene->active;
        const LoginPresentationInput presentation{
            ConstSlice<UiRect>{selections, renderer->login_edit.selection_count},
            ConstSlice<LoginCompositionGeometry>{composition, renderer->login_composition_metric_count},
            UiRect{caret.left, caret.top, caret.left + caret.width, caret.top + caret.height},
            renderer->login_qr_ready ? &renderer->login_qr_code : nullptr,
            !form && renderer->login_edit.active,
            !form && renderer->login_caret_visible};
        if (!login_presentation_build(login, presentation, theme, width_dip, height_dip, renderer->dpi,
                                      &renderer->paint_stream)) {
            return layout_step_failed(__LINE__);
        }
        const u32 base_quad = renderer->paint_stream.count;
        if (form && (!renderer->text_form->scene->content.confirmation_only ||
                     !text_form_paint(*renderer->text_form->scene, renderer->text_form->geometry, theme,
                                      &renderer->paint_stream)))
            return layout_step_failed(__LINE__);
        u32 base = renderer->paint_stream.batch_count;
        if (!split_paint_quad_boundary(&renderer->paint_stream, base_quad, &base))
            return layout_step_failed(__LINE__);
        renderer->media_overlay_paint_first_batch = base;
        renderer->profile_foreground_first_batch = base;
        renderer->menu_paint_first_batch = base;
        renderer->overlay_paint_first_batch = base;
        renderer->edit_overlay_paint_first_batch = base;
        renderer->form_paint_first_batch = base;
        renderer->forum_paint_end_batch = base;
        return true;
    }
    if (!renderer->ui_scene || renderer->ui_scene->count == 0) {
        return layout_step_failed(__LINE__);
    }
    if (!renderer->guild_rail_scene || !renderer->shell_navigation_scene) {
        return layout_step_failed(__LINE__);
    }
    renderer->media_quad_count = 0;
    renderer->media_batch_count = 0;
    renderer->popup_media_first_batch = 0;
    if (renderer->content_text) {
        renderer->content_text->message_geometry_quads = 0;
    }
    ShellPresentationRegions shell{};
    const bool settings_active = renderer_settings_page_visible(*renderer);
    const bool friends_active =
        !settings_active && renderer->friends_scene && renderer->friends_scene->active;
    const bool friends_requests_active =
        !settings_active && renderer->friends_requests_scene && renderer->friends_requests_scene->active;
    const bool browser_active = d3d11_renderer_thread_browser_visible(*renderer);
    const bool content_hidden = friends_active || settings_active || browser_active;
    UiRect composer_selections[kMessageComposerSelectionRectangleCapacity]{};
    for (u32 index = 0; !content_hidden && index < renderer->composer_layout.selection_count; ++index) {
        const TextEditMetric& metric = renderer->composer_layout.selections[index];
        composer_selections[index] =
            UiRect{metric.left, metric.top, metric.left + metric.width, metric.top + metric.height};
    }
    const TextEditMetric& composer_caret = renderer->composer_layout.caret;
    TextCompositionMark composer_marks[kTextCompositionSpanCapacity]{};
    u32 composer_mark_count = 0;
    if (!content_hidden &&
        !composer_composition_geometry(*renderer, {composer_marks, kTextCompositionSpanCapacity},
                                       &composer_mark_count))
        return layout_step_failed(__LINE__);
    const MessageComposerPresentationInput composer_presentation{
        ConstSlice<UiRect>{composer_selections,
                           content_hidden ? 0u : renderer->composer_layout.selection_count},
        UiRect{composer_caret.left, composer_caret.top, composer_caret.left + composer_caret.width,
               composer_caret.top + composer_caret.height},
        !content_hidden && renderer->composer_layout.active,
        !content_hidden && renderer->composer_layout.active && renderer->message_composer_scene &&
            renderer->message_composer_scene->caret_visible,
        {composer_marks, composer_mark_count}};
    // A frame whose inputs differ from the last build only in the message
    // timeline replays everything painted before it (see RendererShellSegment).
    const u64 segment_key =
        stamp && stamp->valid && !content_hidden && !friends_requests_active
            ? shell_segment_key(*stamp, composer_presentation, renderer->paint_stream)
            : 0;
    const bool segment_replayed = replay_shell && segment_key &&
                                  shell_segment_replay(renderer, segment_key, width_dip, height_dip, &shell);
    if (!segment_replayed) {
        if (!shell_presentation_build_chrome(
                *renderer->ui_scene, renderer->navigation_view, renderer->guild_rail_scene,
                renderer->shell_navigation_scene, content_hidden ? nullptr : renderer->message_composer_scene,
                content_hidden ? nullptr : renderer->member_list_scene, theme, width_dip, height_dip,
                &renderer->paint_stream, &shell, &composer_presentation,
                browser_active ? renderer->thread_browser_scene->view->parent_id : 0)) {
            return layout_step_failed(__LINE__);
        }
        if (!paint_threads_launcher(*renderer, theme, &renderer->paint_stream))
            return layout_step_failed(__LINE__);
        if (browser_active)
            remove_covered_shell_paint(&renderer->paint_stream, renderer->thread_browser_scene->panel);
    }
    const ShellPresentationRegions chrome_regions = shell;
    const MessageScene* message_scene = renderer->message_scene;
    const bool messages_visible = !content_hidden && message_scene && message_scene->active;
    if (!content_hidden && renderer->thread_header && renderer->thread_header->active) {
        const auto& header = *renderer->thread_header;
        if (header.shell != renderer->ui_scene || !renderer->chat_state ||
            header.view.thread_id != renderer->chat_state->selected_channel_id ||
            header.geometry.rectangle.left != shell.message_view.left ||
            header.geometry.rectangle.top != shell.message_view.top ||
            header.geometry.rectangle.right != shell.message_view.right ||
            header.geometry.rectangle.bottom > shell.message_view.bottom)
            return layout_step_failed(__LINE__);
        shell.message_view.top = header.geometry.rectangle.bottom;
    }
    if (!content_hidden &&
        (!message_scene || (!message_scene->active && ui_rect_has_area(message_scene->viewport)) ||
         (message_scene->active && (message_scene->viewport.left != shell.message_view.left ||
                                    message_scene->viewport.top != shell.message_view.top ||
                                    message_scene->viewport.right != shell.message_view.right ||
                                    message_scene->viewport.bottom != shell.message_view.bottom)))) {
        return layout_step_failed(__LINE__);
    }

    // Shell media (server icons, conversation, voice, account and member
    // avatars) belongs to the shell segment; the block is left unindented.
    if (!segment_replayed) {
    if (!settings_active && renderer->guild_rail_scene->active && renderer->chat_state &&
        renderer->media_atlas && renderer->media_gpu.texture_view) {
        for (u32 index = 0; index < renderer->guild_rail_scene->count; ++index) {
            if (renderer->media_quad_count >= kRendererMediaQuadCapacity) {
                break;
            }
            const GuildRailRowWidget& row = renderer->guild_rail_scene->rows[index];
            const UiLayoutNode* layout = guild_rail_scene_layout_node(*renderer->guild_rail_scene, row);
            if (layout && row.view.folder_role == GuildFolderRole::header && !row.view.folder_expanded &&
                !queue_folder_preview(renderer, row, guild_rail_icon_rectangle(layout->rectangle)))
                return layout_step_failed(__LINE__);
            const GuildRecord* guild =
                chat_state_store_find_guild(*renderer->chat_state, row.view.id);
            if (!guild || !layout || guild->icon_variant == 0 || guild->icon_hash_bytes == 0) {
                continue;
            }
            const bool hovered = guild_rail_scene_resolve(*renderer->guild_rail_scene,
                                                          renderer->guild_rail_scene->widgets.hovered) == &row;
            const MediaAtlasResult allocation = guild_icon_rail_allocation(renderer->media_atlas, *guild, hovered);
            if (!allocation.ok()) {
                continue;
            }
            // Server artwork is a cover surface, not a glyph inset inside a
            // second placeholder. The media takes the row's silhouette: a
            // circle morphing to a rounded square while hovered or selected.
            const UiRect icon = guild_rail_icon_rectangle(layout->rectangle);
            // Muted servers sit back in the rail until hovered or selected.
            // A dragged server sits back too.
            const auto& drag = renderer->guild_rail_scene->drag;
            const bool dimmed = (drag.active && drag.source_id == row.view.id) ||
                                (row.view.muted && !row.view.selected && !hovered);
            if (!queue_media_quad(renderer, allocation, icon.left, icon.top, icon.right, icon.bottom,
                                  renderer->guild_rail_scene->viewport, false, false, 0, dimmed ? .5f : 1.f,
                                  guild_rail_icon_radius(icon, &row.indicator, row.view.selected, hovered)) ||
                !paint_stream_record_content(
                    &renderer->paint_stream,
                    UiWidgetReference{&renderer->guild_rail_scene->widgets, row.widget}, icon,
                    UiLayoutContentKind::media, ui_layout_content_bounds_allow_overlap)) {
                return layout_step_failed(__LINE__);
            }
        }
    }

    // The selected server's banner over the top of the channel sidebar,
    // header included: cover-fit in its fixed 130 DIP area, clipped by its
    // sliding container and faded with the list's scroll. The header glass is
    // the same picture blurred under the header while scrolled or hovered.
    if (!settings_active && renderer->guild_banner && renderer->ui_scene && renderer->shell_navigation_scene &&
        renderer->shell_navigation_scene->banner_inset > 0 && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        const auto& shell_scene = *renderer->ui_scene;
        const UiNode* sidebar = ui_scene_resolve(shell_scene, ui_scene_find(shell_scene, UiSemantic::channel_sidebar));
        const UiNode* header = ui_scene_resolve(shell_scene, ui_scene_find(shell_scene, UiSemantic::channel_header));
        MediaAtlasResult allocation = media_atlas_find(renderer->media_atlas, renderer->guild_banner->key);
        if (!allocation.ok())
            allocation = media_atlas_find(renderer->media_atlas, renderer->guild_banner->fallback);
        if (sidebar && header && allocation.ok() && allocation.width && allocation.height) {
            const auto& channels = *renderer->shell_navigation_scene;
            const UiGuildBannerGeometry banner = ui_guild_banner_geometry(
                sidebar->rectangle, header->rectangle, channels.scroll_dips, channels.banner_header_hovered);
            const UiRect picture = ui_call_video_fit(banner.image, allocation.width, allocation.height, true);
            const UiRect clip = ui_rect_intersection(banner.clip, sidebar->rectangle);
            const UiRect glass = ui_rect_intersection(
                clip, {banner.header.left, banner.header.top, banner.header.right, banner.header.bottom + 1});
            const float glass_opacity = banner.glass * banner.opacity;
            if ((banner.opacity > 0 && ui_rect_has_area(clip) &&
                 (!queue_media_quad(renderer, allocation, picture.left, picture.top, picture.right, picture.bottom,
                                    clip, false, false, 0, banner.opacity) ||
                  !paint_stream_record_content(&renderer->paint_stream,
                                               UiWidgetReference{&shell_scene.widgets, sidebar->widget}, clip,
                                               UiLayoutContentKind::media, ui_layout_content_bounds_allow_overlap))) ||
                (glass_opacity > 0 && ui_rect_has_area(glass) &&
                 !queue_media_quad(renderer, allocation, picture.left, picture.top, picture.right, picture.bottom,
                                   glass, false, false, 0, glass_opacity, 0.f, {}, 0.f, kUiGuildBannerGlassBlur)))
                return layout_step_failed(__LINE__);
        }
    }

    if (!settings_active && renderer->shell_navigation_scene && renderer->shell_navigation_scene->active &&
        renderer->chat_state && renderer->user_store && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        const auto& scene = *renderer->shell_navigation_scene;
        // One avatar per DM; a generated group icon stacks its first two members.
        const auto private_avatars = [&](u8 type, u64 channel_id, UiRect icon, UiRect clip,
                                         UiWidgetReference owner) noexcept {
            // A custom group icon replaces the generated member stack.
            if (type == 3) {
                const Utf8View custom = ui_private_row_custom_icon(*renderer->chat_state, channel_id);
                if (custom.bytes) {
                    const MediaAtlasResult allocation = media_atlas_peek(
                        renderer->media_atlas,
                        media_asset_channel_icon(channel_id, user_avatar_variant(custom), asset_hash_animated(custom)));
                    const u32 media_before = renderer->media_quad_count;
                    if (allocation.ok() &&
                        (!queue_media_quad(renderer, allocation, icon.left, icon.top, icon.right, icon.bottom, clip,
                                           true) ||
                         !paint_stream_record_content(&renderer->paint_stream, owner, icon,
                                                      UiLayoutContentKind::media,
                                                      ui_layout_content_bounds_allow_overlap)))
                        return layout_step_failed(__LINE__);
                    reveal_media_underlay(*renderer, media_before, icon, theme.avatar_placeholder);
                    return true;
                }
            }
            u64 people[2]{};
            const u32 count = ui_private_row_group_members(*renderer->chat_state, channel_id, people);
            for (u32 j = 0; j < count && renderer->media_quad_count < kRendererMediaQuadCapacity; ++j) {
                const auto r = type == 3 ? ui_private_row_group_avatar(icon, j) : icon;
                const auto* user = user_store_find(*renderer->user_store, people[j]);
                MediaAtlasResult allocation{};
                if (!user || !find_user_avatar(*renderer, *user, &allocation))
                    continue;
                const u32 media_before = renderer->media_quad_count;
                if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, clip, true) ||
                    !paint_stream_record_content(&renderer->paint_stream, owner, r, UiLayoutContentKind::media,
                                                 ui_layout_content_bounds_allow_overlap))
                    return layout_step_failed(__LINE__);
                reveal_media_underlay(*renderer, media_before, r, theme.avatar_placeholder);
                if (type != 3)
                    break;
            }
            return true;
        };
        for (u32 i = 0; i < scene.count && renderer->media_quad_count < kRendererMediaQuadCapacity; ++i) {
            const auto& row = scene.rows[i];
            if (!channel_is_private(row.view.type))
                continue;
            const auto* avatar = ui_widget_manager_layout_node(scene.widgets, row.icon);
            if (!avatar || !ui_rect_has_area(ui_rect_intersection(avatar->rectangle, scene.viewport)))
                continue;
            if (!private_avatars(row.view.type, row.view.id, avatar->rectangle, scene.viewport,
                                 {&scene.widgets, row.icon}))
                return layout_step_failed(__LINE__);
        }
        const auto* header_channel =
            renderer->ui_scene && renderer->ui_scene->view == UiShellView::private_channels
                ? chat_state_store_find_channel(*renderer->chat_state,
                                                   renderer->chat_state->selected_channel_id)
                : nullptr;
        if (header_channel && channel_is_private(header_channel->type)) {
            const auto avatar = ui_private_header_avatar(*renderer->ui_scene);
            const auto* title = ui_scene_resolve(*renderer->ui_scene,
                                                 ui_scene_find(*renderer->ui_scene, UiSemantic::title_bar));
            if (title && ui_rect_has_area(avatar) &&
                (!ui_private_avatar_placeholder_paint(
                     &renderer->paint_stream,
                     ui_private_avatar_shape(renderer->chat_state, header_channel->id, header_channel->type), avatar,
                     theme) ||
                 !private_avatars(header_channel->type, header_channel->id, avatar, title->rectangle,
                                  {&renderer->ui_scene->widgets, title->widget})))
                return layout_step_failed(__LINE__);
        }
        const auto* voice_states = renderer->navigation_view ? renderer->navigation_view->voice_states : nullptr;
        for (u32 i = 0; voice_states && i < scene.count; ++i) {
            const auto& row = scene.rows[i];
            const auto* layout = row.view.voice_count ? shell_navigation_scene_layout_node(scene, row) : nullptr;
            UiVoiceParticipantRow voice{};
            for (u32 j = 0; layout && renderer->media_quad_count < kRendererMediaQuadCapacity &&
                            ui_voice_participant_row(row.view, layout->rectangle, *voice_states, j, &voice);
                 ++j) {
                const auto* user = user_store_find(*renderer->user_store, voice.state->user_id);
                MediaAtlasResult allocation{};
                if (!ui_rect_has_area(ui_rect_intersection(voice.avatar, scene.viewport)) || !user ||
                    !find_user_avatar(*renderer, *user, &allocation))
                    continue;
                const u32 media_before = renderer->media_quad_count;
                if (!queue_media_quad(renderer, allocation, voice.avatar.left, voice.avatar.top, voice.avatar.right,
                                      voice.avatar.bottom, scene.viewport, true) ||
                    !paint_stream_record_content(&renderer->paint_stream, {&scene.widgets, row.widget},
                                                 voice.avatar, UiLayoutContentKind::media,
                                                 ui_layout_content_bounds_allow_overlap))
                    return layout_step_failed(__LINE__);
                reveal_media_underlay(*renderer, media_before, voice.avatar, theme.avatar_placeholder);
            }
        }
    }
    if (!settings_active && renderer->chat_state && renderer->user_store && renderer->ui_scene) {
        const auto* footer = ui_scene_resolve(
            *renderer->ui_scene, ui_scene_find(*renderer->ui_scene, UiSemantic::channel_footer));
        const auto* user =
            user_store_find(*renderer->user_store, renderer->chat_state->self_user_id);
        MediaAtlasResult allocation{};
        if (footer && user) {
            // The name column always reserves this slot: keep a placeholder
            // until the avatar is resident, as member and message rows do.
            const UiRect avatar = ui_account_avatar_rectangle(footer->rectangle);
            if (!ui_account_speaking_ring_paint(&renderer->paint_stream,
                                                renderer->navigation_view ? renderer->navigation_view->voice_panel
                                                                          : nullptr,
                                                avatar, theme))
                return layout_step_failed(__LINE__);
            if (!paint_stream_push_rounded(&renderer->paint_stream, avatar.left, avatar.top, avatar.right,
                                           avatar.bottom, theme.avatar_placeholder, ui_rect_width(avatar) * .5f))
                return layout_step_failed(__LINE__);
            const u32 media_before = renderer->media_quad_count;
            if (find_user_avatar(*renderer, *user, &allocation) &&
                !queue_media_quad(renderer, allocation, avatar.left, avatar.top, avatar.right, avatar.bottom,
                                  footer->rectangle, true))
                return layout_step_failed(__LINE__);
            reveal_media_underlay(*renderer, media_before, avatar, theme.avatar_placeholder);
        }
    }
    if (browser_active &&
        !ui_thread_browser_scene_paint(*renderer->thread_browser_scene, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    // Forum post cards: starter-image thumbnails, aspect-fitted in their media wells.
    if (browser_active && renderer->thread_browser_scene->forum) {
        const auto& browser = *renderer->thread_browser_scene;
        for (u32 i = 0; i < browser.count && i < kUiThreadBrowserPageCapacity &&
                        renderer->media_quad_count < kRendererMediaQuadCapacity &&
                        renderer->media_batch_count < kRendererMediaBatchCapacity;
             ++i) {
            ForumChannelThumbnailMedia media{};
            MediaAtlasResult allocation{};
            u8 domain = 0;
            if (!forum_channel_thumbnail_resolve(browser, i, renderer->dpi, &media) ||
                !find_call_preview(*renderer, media.key, &allocation, &domain))
                continue;
            const auto r = media.rectangle;
            if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip, false, false,
                                  domain) ||
                !paint_stream_record_content(&renderer->paint_stream, media.owner, r, UiLayoutContentKind::media,
                                             ui_layout_content_bounds_allow_overlap))
                return layout_step_failed(__LINE__);
        }
    }
    // Forum post cards: author avatars over their placeholders, from the shared avatar cache.
    if (browser_active && renderer->thread_browser_scene->forum && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        const auto& browser = *renderer->thread_browser_scene;
        for (u32 i = 0; i < browser.count && i < kUiThreadBrowserPageCapacity &&
                        renderer->media_quad_count < kRendererMediaQuadCapacity &&
                        renderer->media_batch_count < kRendererMediaBatchCapacity;
             ++i) {
            ForumChannelAvatarMedia media{};
            if (!forum_channel_avatar_media_resolve(browser, i, &media))
                continue;
            const auto allocation = media_atlas_peek(renderer->media_atlas, media.key);
            if (!allocation.ok())
                continue;
            const auto r = media.rectangle;
            const u32 before = renderer->media_quad_count;
            if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip, true) ||
                !paint_stream_record_content(&renderer->paint_stream, media.owner, r, UiLayoutContentKind::media,
                                             ui_layout_content_bounds_allow_overlap))
                return layout_step_failed(__LINE__);
            reveal_media_underlay(*renderer, before, r, theme.avatar_placeholder);
        }
    }
    if (friends_active && !ui_friends_scene_paint(*renderer->friends_scene, theme, &renderer->paint_stream,
                                                  &renderer->friends_search_geometry, renderer->dpi, false))
        return layout_step_failed(__LINE__);
    if (friends_active && renderer->user_store && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        const auto& scene = *renderer->friends_scene;
        const u32 count = friends_avatar_media_count(scene);
        for (u32 i = 0; i < count && renderer->media_quad_count < kRendererMediaQuadCapacity; ++i) {
            FriendsAvatarMedia media{};
            if (!friends_avatar_media_resolve(scene, *renderer->user_store, i, &media))
                continue;
            const auto allocation = media_atlas_peek(renderer->media_atlas, media.key);
            if (!allocation.ok())
                continue;
            const auto r = media.rectangle;
            const u32 media_before = renderer->media_quad_count;
            if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip,
                                  true) ||
                !paint_stream_record_content(&renderer->paint_stream, media.owner, r,
                                             UiLayoutContentKind::media,
                                             ui_layout_content_bounds_allow_overlap))
                return layout_step_failed(__LINE__);
            reveal_media_underlay(*renderer, media_before, r, theme.interactive);
        }
    }
    if (!content_hidden && renderer->member_list_scene && renderer->member_list_scene->active &&
        renderer->member_list_scene->users && renderer->media_atlas && renderer->media_gpu.texture_view) {
        const MemberListScene& member_scene = *renderer->member_list_scene;
        const MemberListRowWidget* hovered_row =
            member_list_scene_resolve(member_scene, member_scene.widgets.hovered);
        for (u32 index = 0; index < member_scene.count; ++index) {
            if (renderer->media_quad_count >= kRendererMediaQuadCapacity) {
                break;
            }
            const MemberListRowWidget& row = member_scene.rows[index];
            if (!member_list_scene_row_resident(member_scene, index) ||
                row.kind != MemberListRowKind::member || row.user_id == 0) {
                continue;
            }
            for (u32 layer = 0; layer < 5; ++layer) {
                // Resolved once per user and moved with its row (member_media_place).
                MemberMediaPlacement media{};
                MediaAtlasResult allocation{};
                if (member_media_place(member_scene, index, static_cast<MemberMedia>(layer), &media)) {
                    allocation = media_atlas_find(renderer->media_atlas, media.key);
                    if (!allocation.ok() && layer == static_cast<u32>(MemberMedia::avatar)) {
                        // A guild avatar still loading falls back to the shared one.
                        const auto* user = user_store_find(*member_scene.users, row.user_id);
                        if (user)
                            find_user_avatar(*renderer, *user, &allocation);
                    }
                } else if (layer == static_cast<u32>(MemberMedia::avatar)) {
                    if (!member_plain_avatar_place(member_scene, index, &media))
                        continue;
                    allocation = media_atlas_peek(renderer->media_atlas, media.key);
                } else {
                    continue;
                }
                const auto r = media.rectangle;
                const bool offline = row.source_row.status == GuildMemberStatus::offline;
                const bool hovered = hovered_row == &row;
                const u32 media_before = renderer->media_quad_count;
                if (allocation.ok() &&
                    !queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip,
                                      media.circular, false, 0, offline && !hovered ? .4f : 1.f,
                                      layer == 0 ? 8.f : 0.f))
                    return layout_step_failed(__LINE__);
                if (layer == static_cast<u32>(MemberMedia::avatar))
                    reveal_media_underlay(*renderer, media_before, r, theme.avatar_placeholder);
            }
        }
    }

    if (segment_key)
        shell_segment_capture(renderer, segment_key, chrome_regions);
    } // !segment_replayed

    MessageMediaCommand message_media_commands[kRendererMessageMediaCommandCapacity]{};
    MessageMediaCommandStream message_media_stream{};
    MessagePresentationResult message_presentation{};
    if (messages_visible &&
        (!message_media_command_stream_initialize(
             &message_media_stream,
             Slice<MessageMediaCommand>{message_media_commands, kRendererMessageMediaCommandCapacity}) ||
         !message_presentation_build(*renderer->message_scene, theme, &renderer->paint_stream,
                                     &message_media_stream, &message_presentation))) {
        return layout_step_failed(__LINE__);
    }
    if (messages_visible && renderer->user_store && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        if (renderer->message_text && renderer->chat_state) {
            for (u32 i = 0; i < renderer->message_text->command_count; ++i) {
                const auto& command = renderer->message_text->commands[i];
                MessageAuthorLayout header{};
                if (!ui_rect_has_area(ui_rect_intersection(command.rectangle, message_scene->viewport)) ||
                    !message_author_layout(*message_scene, command, *renderer->chat_state,
                                           *renderer->user_store, renderer->message_measure_cache, theme,
                                           &header) ||
                    (!header.badge.label.bytes && !header.plugin.badge.label.bytes && !header.decoration_count))
                    continue;
                if (!ui_plugin_slot_badge_paint(header.plugin, message_scene->widgets, theme, &renderer->paint_stream))
                    return layout_step_failed(__LINE__);
                for (u32 d = 0; d < header.decoration_count; ++d)
                    if (!ui_plugin_slot_badge_paint(header.decorations[d], message_scene->widgets, theme,
                                                    &renderer->paint_stream))
                        return layout_step_failed(__LINE__);
                if (!header.badge.label.bytes)
                    continue;
                if (!ui_badge_paint(header.badge, message_scene->widgets, theme, &renderer->paint_stream))
                    return layout_step_failed(__LINE__);
                ProfileMedia badge_media{};
                if (header.badge.leading && header.appearance &&
                    user_guild_badge_media(*header.appearance,
                                           ui_badge_geometry(header.badge, message_scene->widgets).icon,
                                           command.clip, &badge_media)) {
                    const auto allocation = media_atlas_find(renderer->media_atlas, badge_media.key);
                    const auto r = badge_media.rectangle;
                    if (allocation.ok() && !queue_media_quad(renderer, allocation, r.left, r.top, r.right,
                                                             r.bottom, badge_media.clip, false))
                        return layout_step_failed(__LINE__);
                }
            }
        }
        for (u32 index = 0; index < message_scene->count; ++index) {
            const MessageSceneRowWidget& row = message_scene->rows[index];
            if (!message_scene_avatar_visible(*message_scene, row) ||
                renderer->media_quad_count >= kRendererMediaQuadCapacity) {
                continue;
            }
            const MessageRecord* message =
                message_store_resolve(*message_scene->timeline->store, row.message);
            const UserRecord* user =
                message ? user_store_find(*renderer->user_store, message->author_id) : nullptr;
            const UiLayoutNode* avatar = message_scene_layout_node(*message_scene, row.avatar);
            MediaAtlasResult allocation{};
            if (!user || !avatar || !find_user_avatar(*renderer, *user, &allocation)) {
                continue;
            }
            const u32 media_before = renderer->media_quad_count;
            if (!queue_media_quad(renderer, allocation, avatar->rectangle.left, avatar->rectangle.top,
                                  avatar->rectangle.right, avatar->rectangle.bottom,
                                  message_scene->viewport, true) ||
                !paint_stream_record_content(&renderer->paint_stream,
                                             UiWidgetReference{&message_scene->widgets, row.avatar},
                                             avatar->rectangle, UiLayoutContentKind::media,
                                             ui_layout_content_bounds_allow_overlap)) {
                return layout_step_failed(__LINE__);
            }
            reveal_media_underlay(*renderer, media_before, avatar->rectangle, theme.avatar_placeholder);
        }
        // A DM's channel-beginning header shows the recipient's avatar.
        if (MessageSceneIntroGeometry intro{};
            message_scene->intro.kind == MessageSceneIntroKind::direct_message && message_scene->intro.user_id &&
            renderer->user_store && renderer->media_quad_count < kRendererMediaQuadCapacity &&
            message_scene_intro_geometry(*message_scene, &intro) && ui_rect_has_area(intro.icon)) {
            const UserRecord* user = user_store_find(*renderer->user_store, message_scene->intro.user_id);
            MediaAtlasResult allocation{};
            const UiRect clip = ui_rect_intersection(message_scene->viewport, intro.area);
            if (user && ui_rect_has_area(clip) && find_user_avatar(*renderer, *user, &allocation)) {
                const u32 media_before = renderer->media_quad_count;
                if (!queue_media_quad(renderer, allocation, intro.icon.left, intro.icon.top, intro.icon.right,
                                      intro.icon.bottom, clip, true))
                    return layout_step_failed(__LINE__);
                reveal_media_underlay(*renderer, media_before, intro.icon, theme.avatar_placeholder);
            }
        }
        for (u32 index = 0; index < message_scene->count; ++index) {
            if (renderer->media_quad_count >= kRendererMediaQuadCapacity) {
                break;
            }
            const MessageSceneRowWidget& row = message_scene->rows[index];
            const MessageRecord* message =
                message_store_resolve(*message_scene->timeline->store, row.message);
            const MessageRichStore* rich_store = message_scene->timeline->rich_store;
            const MessageRichRecord* rich =
                message && rich_store ? message_rich_store_find(*rich_store, message->id) : nullptr;
            const UserRecord* user =
                rich && rich->message_revision == message->revision && message_rich_has_header(*rich)
                    ? user_store_find(*renderer->user_store, message_rich_header_user(*rich))
                    : nullptr;
            const UiLayoutNode* body = message_scene_layout_node(*message_scene, row.body);
            const UiLayoutNode* reply = message_scene_layout_node(*message_scene, row.reply);
            MediaAtlasResult allocation{};
            if (!user || !body || !reply || !find_user_avatar(*renderer, *user, &allocation)) {
                continue;
            }
            const UiRect reference_avatar{body->content_rectangle.left, reply->rectangle.top + 1.0f,
                                          body->content_rectangle.left + 16.0f,
                                          reply->rectangle.top + 17.0f};
            const u32 media_before = renderer->media_quad_count;
            if (!queue_media_quad(renderer, allocation, reference_avatar.left, reference_avatar.top,
                                  reference_avatar.right, reference_avatar.bottom, message_scene->viewport,
                                  true)) {
                return layout_step_failed(__LINE__);
            }
            reveal_media_underlay(*renderer, media_before, reference_avatar, theme.avatar_placeholder);
        }
    }
    if (!content_hidden && renderer->message_composer_scene && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        const auto& composer = *renderer->message_composer_scene;
        const auto avatar = message_composer_scene_command_avatar(composer);
        if (ui_rect_has_area(avatar)) {
            const auto allocation = media_atlas_peek(renderer->media_atlas,
                media_asset_user_avatar(composer.command.bot_id, composer.command.avatar_variant,
                                        composer.command.animated));
            if (allocation.ok() &&
                !queue_media_quad(renderer, allocation, avatar.left, avatar.top, avatar.right, avatar.bottom,
                                  composer.viewport, true))
                return layout_step_failed(__LINE__);
        }
        for (u32 i = 0; i < composer.projection_span_count; ++i) {
            UiRect rectangle{}, clip{};
            if (!text_composer_emoji_rectangle(renderer->composer_layout, composer, i, &rectangle, &clip))
                continue;
            const auto& span = composer.projection_spans[i];
            const auto allocation = media_atlas_peek(
                renderer->media_atlas, span.kind == MessageComposerProjectionKind::regional_flag
                                           ? media_asset_regional_flag(static_cast<u16>(span.target))
                                           : media_asset_custom_emoji(span.target, span.animated));
            if (allocation.ok() && !queue_media_quad(renderer, allocation, rectangle.left, rectangle.top,
                                                     rectangle.right, rectangle.bottom, clip))
                return layout_step_failed(__LINE__);
        }
    }
    if (!content_hidden && renderer->content_text && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        const RendererTextStorage& text = *renderer->content_text;
        for (u32 index = 0; index < text.content_hit_region_count; ++index) {
            if (renderer->media_quad_count >= kRendererMediaQuadCapacity) {
                break;
            }
            const TextHitRegion& region = text.content_hit_regions[index];
            if (region.semantic_index >= text.content_semantic_count) {
                continue;
            }
            const MessageTextSemantic& semantic = text.content_semantics[region.semantic_index];
            const bool flag = semantic.kind == MessageMarkdownSemanticKind::regional_flag;
            if ((semantic.kind != MessageMarkdownSemanticKind::custom_emoji && !flag) ||
                semantic.target == 0 || (semantic.reserved & (2u | kRendererSearchSemanticFlag))) {
                continue;
            }
            const bool animated = (semantic.reserved & 1u) != 0;
            const MediaAtlasResult allocation = media_atlas_peek(
                renderer->media_atlas, flag ? media_asset_regional_flag(static_cast<u16>(semantic.target))
                                            : media_asset_custom_emoji(semantic.target, animated));
            if (!allocation.ok()) {
                continue;
            }
            const float height = maximum(1.0f, region.bottom - region.top);
            const float extent =
                flag ? minimum(48.0f, region.right - region.left) : minimum(48.0f, maximum(16.0f, height));
            const float center_x = (region.left + region.right) * 0.5f;
            const float center_y = (region.top + region.bottom) * 0.5f;
            if (!queue_media_quad(renderer, allocation, center_x - extent * 0.5f, center_y - extent * 0.5f,
                                  center_x + extent * 0.5f, center_y + extent * 0.5f,
                                  message_scene->viewport, false)) {
                return layout_step_failed(__LINE__);
            }
        }
    }
    const auto queue_message_media = [&](const MessageMediaCommandStream& stream, bool first) noexcept {
        for (u32 index = 0; index < stream.count; ++index) {
            const MessageMediaCommand& command = stream.storage[index];
            MediaAssetKey key{};
            const bool large_preview = command.kind == MessageMediaCommandKind::attachment ||
                                       command.kind == MessageMediaCommandKind::embed;
            const MediaResolutionTier resolution =
                large_preview && renderer->preview_media_atlas
                    ? media_resolution_tier_for_display(
                          command.rectangle.right - command.rectangle.left,
                          command.rectangle.bottom - command.rectangle.top, renderer->dpi,
                          media_preview_extent_cap(static_cast<MediaAttachmentFormat>(command.format)))
                    : MediaResolutionTier::original;
            switch (command.kind) {
            case MessageMediaCommandKind::attachment:
                key = command.search_result
                          ? media_asset_search_attachment(
                                command.id, static_cast<MediaAttachmentFormat>(command.format), resolution,
                                command.message_revision, command.snapshot)
                          : media_asset_attachment(
                                command.id, static_cast<MediaAttachmentFormat>(command.format), resolution);
                break;
            case MessageMediaCommandKind::embed:
                key = command.search_result
                          ? media_asset_search_embed(
                                command.id, command.message_revision, command.embed_index,
                                static_cast<MediaEmbedResource>(command.embed_resource),
                                static_cast<MediaAttachmentFormat>(command.format), command.animated,
                                resolution, command.snapshot)
                          : media_asset_embed(command.id, command.message_revision, command.embed_index,
                                              static_cast<MediaEmbedResource>(command.embed_resource),
                                              static_cast<MediaAttachmentFormat>(command.format),
                                              command.animated, resolution, command.snapshot);
                break;
            case MessageMediaCommandKind::sticker:
                key = media_asset_sticker(command.id, command.format);
                break;
            case MessageMediaCommandKind::custom_emoji:
                key = media_asset_custom_emoji(command.id, command.animated);
                break;
            case MessageMediaCommandKind::regional_flag:
                key = media_asset_regional_flag(static_cast<u16>(command.id));
                break;
            case MessageMediaCommandKind::component:
                key = message_component_media_key(command.id, static_cast<MediaAttachmentFormat>(command.format));
                break;
            }
            const u8 domain = large_preview && renderer->preview_media_atlas ? 1 : 0;
            MediaAtlas* atlas = domain == 1 ? renderer->preview_media_atlas : renderer->media_atlas;
            ID3D11ShaderResourceView* texture_view =
                domain == 1 ? renderer->media_gpu.preview_texture_view : renderer->media_gpu.texture_view;
            if (!atlas || !texture_view) {
                continue;
            }
            const MediaAtlasResult allocation = media_atlas_peek(atlas, key);
            if (!allocation.ok()) {
                continue;
            }
            if (renderer->media_quad_count >= kRendererMediaQuadCapacity) {
                continue;
            }
            const u32 media_before = renderer->media_quad_count;
            // Grid tiles: the cover rectangle's centred square is the tile itself.
            UiRect tile{};
            if (command.tile_radius) {
                const float side = minimum(ui_rect_width(command.rectangle), ui_rect_height(command.rectangle));
                const float cx = (command.rectangle.left + command.rectangle.right) * .5f,
                            cy = (command.rectangle.top + command.rectangle.bottom) * .5f;
                tile = {cx - side * .5f, cy - side * .5f, cx + side * .5f, cy + side * .5f};
            }
            // Spoiler and explicit previews blur in the media shader until revealed.
            const float blur = command.obscure != static_cast<u8>(MessageMediaObscure::none)
                                   ? minimum(kMessageMediaObscureBlur,
                                             minimum(ui_rect_width(command.rectangle),
                                                     ui_rect_height(command.rectangle)) * .25f)
                                   : 0.f;
            // Gallery tiles: the image covers its tile, cropped by the tile clip.
            const UiRect cover = command.cover ? ui_call_video_fit(command.rectangle, allocation.width,
                                                                   allocation.height, true)
                                               : UiRect{};
            const UiRect quad = ui_rect_has_area(cover) ? cover : command.rectangle;
            const UiRect quad_clip =
                ui_rect_has_area(cover) ? ui_rect_intersection(command.clip, command.rectangle) : command.clip;
            if (!queue_media_quad(renderer, allocation, quad.left, quad.top, quad.right, quad.bottom, quad_clip,
                                  false, first, domain, 1.f,
                                  command.ordinary_video ? UiVideoSceneStyle{}.preview_radius : 0.f, tile,
                                  static_cast<float>(command.tile_radius), blur) ||
                !paint_stream_record_content(&renderer->paint_stream, command.owner, command.rectangle,
                                             UiLayoutContentKind::media,
                                             ui_layout_content_bounds_allow_overlap)) {
                return layout_step_failed(__LINE__);
            }
            if (large_preview || command.kind == MessageMediaCommandKind::sticker)
                reveal_media_underlay(*renderer, media_before, command.rectangle, theme.surface_high);
            if (renderer->media_quad_count != media_before)
                first = false;
        }
        return true;
    };
    if (!queue_message_media(message_media_stream, false))
        return layout_step_failed(__LINE__);
    // Composer upload thumbnails join the ordinary preview layer so the strip
    // overlay (tags, progress and the action bar) paints above them.
    if (!content_hidden && renderer->message_composer_scene && renderer->message_composer_scene->active &&
        renderer->preview_media_atlas && renderer->media_gpu.preview_texture_view) {
        const auto strip = message_composer_scene_attachment_view(*renderer->message_composer_scene, false);
        bool first_thumbnail = true;
        for (u32 i = 0; i < strip.tiles.count; ++i) {
            UiRect media{}, clip{};
            if (!ui_composer_attachment_thumbnail_visible(strip, i, &media, &clip) ||
                renderer->media_quad_count >= kRendererMediaQuadCapacity)
                continue;
            const auto allocation = media_atlas_peek(renderer->preview_media_atlas, strip.tiles[i].preview);
            if (!allocation.ok() || !allocation.width || !allocation.height)
                continue;
            const UiRect fitted = ui_composer_attachment_fit(media, allocation.width, allocation.height);
            // Spoilered uploads keep their thumbnail, blurred in the media shader.
            const float blur = ui_composer_attachment_thumbnail_blur(strip.tiles[i], fitted);
            if (!queue_media_quad(renderer, allocation, fitted.left, fitted.top, fitted.right, fitted.bottom, clip,
                                  false, first_thumbnail, 1, 1.f, 0.f, fitted, theme.scale.radius_sm, blur) ||
                !paint_stream_record_content(&renderer->paint_stream, strip.owner,
                                             ui_rect_intersection(fitted, clip), UiLayoutContentKind::media,
                                             ui_layout_content_bounds_allow_overlap))
                return layout_step_failed(__LINE__);
            first_thumbnail = false;
        }
    }
    if (renderer->content_text) {
        renderer->content_text->message_geometry_quads = message_presentation.geometry_quads;
    }

    // Avatar textures are submitted as media after the base shell geometry.
    // Start a separate paint layer for badges that must remain above those
    // textures regardless of media residency.
    renderer->media_overlay_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
    if (!content_hidden) {
        const UiVideoPlayAffordanceStyle affordance{};
        for (u32 i = 0; i < message_media_stream.count; ++i) {
            const auto& command = message_media_stream.storage[i];
            if (command.obscure != static_cast<u8>(MessageMediaObscure::none)) {
                // A centred scrim pill above the blurred preview; the text
                // pass draws its label. Obscured media offers no save action.
                const UiRect pill = message_media_obscure_pill(command.rectangle,
                                                                static_cast<MessageMediaObscure>(command.obscure));
                if (ui_rect_has_area(pill) &&
                    (!paint_stream_push_rounded_border(&renderer->paint_stream, pill.left, pill.top, pill.right,
                                                       pill.bottom, theme.tokens.overlay.scrim_strong,
                                                       theme.tokens.overlay.scrim_strong,
                                                       ui_rect_height(pill) * .5f, 0) ||
                     !paint_stream_record_content(&renderer->paint_stream, command.owner,
                                                  ui_rect_intersection(pill, command.clip),
                                                  UiLayoutContentKind::decoration,
                                                  ui_layout_content_bounds_allow_overlap)))
                    return layout_step_failed(__LINE__);
                continue;
            }
            if (command.kind == MessageMediaCommandKind::attachment &&
                !d3d11_renderer_message_modal_active(*renderer) &&
                !ui_attachment_save_paint(
                    &renderer->paint_stream, command.owner,
                    ui_attachment_save_rectangle(command.rectangle, false), command.clip, theme, false,
                    d3d11_renderer_video_scene_visible(*renderer) &&
                        ui_video_scene_covers_preview(*renderer->video_scene, command.rectangle)))
                return layout_step_failed(__LINE__);
            const auto star_owner = renderer->gif_favorite_owner;
            if (command.gif && !d3d11_renderer_message_modal_active(*renderer) && star_owner.generation &&
                command.owner.handle.slot == star_owner.slot &&
                command.owner.handle.generation == star_owner.generation &&
                !ui_gif_favorite_paint(
                    &renderer->paint_stream, command.owner,
                    ui_gif_favorite_rectangle(command.rectangle,
                                              command.kind == MessageMediaCommandKind::attachment),
                    command.clip, theme, renderer->gif_favorite, renderer->gif_favorite_pending))
                return layout_step_failed(__LINE__);
            if (!command.ordinary_video ||
                (d3d11_renderer_video_scene_visible(*renderer) &&
                 ui_video_scene_covers_preview(*renderer->video_scene, command.rectangle)))
                continue;
            if (!ui_video_play_affordance_paint(command.rectangle, command.clip, affordance, theme,
                                                &renderer->paint_stream))
                return layout_step_failed(__LINE__);
        }
    }
    if (!content_hidden && renderer->message_composer_scene && renderer->message_composer_scene->active &&
        !ui_composer_attachment_strip_paint_overlay(
            message_composer_scene_attachment_view(*renderer->message_composer_scene, true), theme,
            &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    if (messages_visible && d3d11_renderer_video_scene_visible(*renderer) &&
        !renderer->video_scene_search_result &&
        !ui_video_scene_paint(*renderer->video_scene, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    if (friends_active &&
        !ui_friends_scene_presence_paint(*renderer->friends_scene, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    if (!content_hidden && renderer->thread_header &&
        !ui_thread_header_paint(*renderer->thread_header, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    if (!content_hidden && !shell_presentation_build_member_status_overlay(
                               renderer->member_list_scene, theme, &renderer->paint_stream)) {
        return layout_step_failed(__LINE__);
    }
    // Rail mention badges and their cutout ring sit above the server icons.
    if (!settings_active && renderer->ui_scene &&
        !shell_presentation_build_guild_rail_overlay(*renderer->ui_scene, renderer->guild_rail_scene, theme,
                                                     &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    // The server banner's header scrim, glass edge and dropdown sit above its picture.
    if (!settings_active && renderer->ui_scene &&
        !shell_presentation_build_guild_banner_overlay(*renderer->ui_scene, renderer->navigation_view,
                                                       renderer->shell_navigation_scene, theme,
                                                       &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    if (!settings_active && renderer->navigation_view && renderer->navigation_view->private_rows &&
        renderer->ui_scene) {
        const auto& rows = *renderer->navigation_view->private_rows;
        const auto* navigation = ui_scene_resolve(*renderer->ui_scene,
                                                  ui_scene_find(*renderer->ui_scene, UiSemantic::channel_navigation));
        const u64 selected = renderer->chat_state ? renderer->chat_state->selected_channel_id : 0;
        if ((navigation && !ui_private_rows_overlay_paint(*renderer->shell_navigation_scene,
                                                          ui_rect_intersection(navigation->rectangle,
                                                                               renderer->shell_navigation_scene->viewport),
                                                          rows, selected, theme, &renderer->paint_stream)) ||
            (renderer->ui_scene->view == UiShellView::private_channels &&
             !ui_private_header_overlay_paint(*renderer->ui_scene, rows, selected, theme, &renderer->paint_stream)) ||
            (!content_hidden && renderer->member_list_scene && rows.state &&
             !ui_private_member_owner_paint(*renderer->member_list_scene, *rows.state, theme,
                                            &renderer->paint_stream)))
            return layout_step_failed(__LINE__);
    }

    const auto* focus_owner = d3d11_renderer_visible_focus_owner(*renderer);
    if (focus_owner && !renderer->login_visible && !friends_requests_active &&
        !d3d11_renderer_media_picker_visible(*renderer) &&
        !(renderer->popup_scene && renderer->popup_scene->active) &&
        !((renderer->text_form && renderer->text_form->scene && renderer->text_form->scene->active) ||
          (renderer->forum_form && renderer->forum_form->scene && renderer->forum_form->scene->active)) &&
        !(renderer->reaction_viewer && renderer->reaction_viewer->active) &&
        !paint_stream_push_focus(&renderer->paint_stream, *focus_owner, theme.tokens.border.focus))
        return layout_step_failed(__LINE__);
    renderer->edit_overlay_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
    if (!content_hidden &&
        !shell_presentation_build_composer_overlay(renderer->message_composer_scene, composer_presentation,
                                                   theme, &renderer->paint_stream, renderer->dpi))
        return layout_step_failed(__LINE__);
    renderer->overlay_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
    // Call view: background and tile plates lead the side-panel layer; its
    // media and foreground (pills, rings, controls) follow below.
    const bool call_visible = d3d11_renderer_call_visible(*renderer);
    if (call_visible && !ui_call_scene_paint(*renderer->call_scene, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    if (d3d11_renderer_unread_banner_visible(*renderer) &&
        !ui_unread_banner_paint(*renderer->unread_banner_scene, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    const u32 pins_paint_first = renderer->paint_stream.count;
    if (d3d11_renderer_pinned_messages_visible(*renderer) &&
        !ui_pinned_messages_scene_paint(*renderer->pinned_messages_scene, theme, &renderer->paint_stream,
                                        renderer->dpi))
        return layout_step_failed(__LINE__);
    const u32 pins_paint_end = renderer->paint_stream.count;
    const u32 search_paint_first = renderer->paint_stream.count;
    // Timeline commands have been consumed. Search reuses the same bounded
    // per-frame storage instead of retaining another command bank.
    MessageMediaCommandStream search_media_stream{};
    if (!message_media_command_stream_initialize(
            &search_media_stream, {message_media_commands, kRendererMessageMediaCommandCapacity}))
        return layout_step_failed(__LINE__);
    if (d3d11_renderer_search_visible(*renderer) &&
        !ui_search_scene_paint(*renderer->search_scene, theme, &renderer->paint_stream,
                               &renderer->search_edit_geometry, renderer->dpi, &search_media_stream))
        return layout_step_failed(__LINE__);
    renderer->friends_requests_media_first_batch = renderer->media_batch_count;
    if (!queue_message_media(search_media_stream, true))
        return layout_step_failed(__LINE__);
    const u32 search_foreground_quad = renderer->paint_stream.count;
    if (d3d11_renderer_search_visible(*renderer))
        renderer->friends_requests_foreground_first_batch = paint_stream_seal(&renderer->paint_stream);
    if (d3d11_renderer_search_visible(*renderer)) {
        const UiVideoPlayAffordanceStyle affordance{};
        for (u32 i = 0; i < search_media_stream.count; ++i) {
            const auto& command = search_media_stream.storage[i];
            const bool covered = d3d11_renderer_video_scene_visible(*renderer) &&
                                 renderer->video_scene_search_result &&
                                 ui_video_scene_covers_preview(*renderer->video_scene, command.rectangle);
            // Media grid tiles are owned by the search scene itself and have no Save action.
            if (command.kind == MessageMediaCommandKind::attachment &&
                command.owner.manager != &renderer->search_scene->widgets &&
                !d3d11_renderer_message_modal_active(*renderer) &&
                !ui_attachment_save_paint(&renderer->paint_stream, command.owner,
                                          ui_attachment_save_rectangle(command.rectangle, false),
                                          command.clip, theme, false, covered))
                return layout_step_failed(__LINE__);
            if (command.ordinary_video && !covered &&
                !ui_video_play_affordance_paint(command.rectangle, command.clip, affordance, theme,
                                                &renderer->paint_stream))
                return layout_step_failed(__LINE__);
        }
        if (d3d11_renderer_video_scene_visible(*renderer) && renderer->video_scene_search_result &&
            !ui_video_scene_paint(*renderer->video_scene, theme, &renderer->paint_stream))
            return layout_step_failed(__LINE__);
        // Rich inline emoji cells belong above Search card geometry.
        if (renderer->content_text && renderer->media_atlas && renderer->media_gpu.texture_view) {
            const auto& text = *renderer->content_text;
            bool first = true;
            for (u32 i = 0; i < text.content_hit_region_count; ++i) {
                const auto& region = text.content_hit_regions[i];
                if (region.semantic_index >= text.content_semantic_count)
                    continue;
                const auto& semantic = text.content_semantics[region.semantic_index];
                const bool flag = semantic.kind == MessageMarkdownSemanticKind::regional_flag;
                if (!(semantic.reserved & kRendererSearchSemanticFlag) || (semantic.reserved & 2u) ||
                    !semantic.target ||
                    (!flag && semantic.kind != MessageMarkdownSemanticKind::custom_emoji))
                    continue;
                const auto clip = ui_search_scene_content_clip(
                    *renderer->search_scene, {region.left, region.top, region.right, region.bottom});
                if (!ui_rect_has_area(clip))
                    continue;
                const auto allocation = media_atlas_peek(
                    renderer->media_atlas,
                    flag ? media_asset_regional_flag(static_cast<u16>(semantic.target))
                         : media_asset_custom_emoji(semantic.target, (semantic.reserved & 1u) != 0));
                if (!allocation.ok() || renderer->media_quad_count >= kRendererMediaQuadCapacity)
                    continue;
                const float height = maximum(1.f, region.bottom - region.top);
                const float extent =
                    flag ? minimum(48.f, region.right - region.left) : minimum(48.f, maximum(16.f, height));
                const float cx = (region.left + region.right) * .5f,
                            cy = (region.top + region.bottom) * .5f;
                const u32 before = renderer->media_quad_count;
                if (!queue_media_quad(renderer, allocation, cx - extent * .5f, cy - extent * .5f,
                                      cx + extent * .5f, cy + extent * .5f, clip, false, first))
                    return layout_step_failed(__LINE__);
                if (renderer->media_quad_count != before)
                    first = false;
            }
        }
    }
    if (d3d11_renderer_search_visible(*renderer) &&
        !split_paint_quad_boundary(&renderer->paint_stream, search_foreground_quad,
                                   &renderer->friends_requests_foreground_first_batch))
        return layout_step_failed(__LINE__);
    if (friends_requests_active) {
        // The modal owns its scroll viewport even when the main pane's hover
        // policy filters background scrollbars. Keep that shared policy for
        // all other surfaces, and restore it before leaving this paint scope.
        const bool filter_scrollbars = renderer->paint_stream.filter_scrollbars;
        renderer->paint_stream.filter_scrollbars = false;
        const bool painted = ui_friends_scene_paint(*renderer->friends_requests_scene, theme,
                                                    &renderer->paint_stream, nullptr, renderer->dpi, false);
        renderer->paint_stream.filter_scrollbars = filter_scrollbars;
        if (!painted)
            return layout_step_failed(__LINE__);
    }
    if (!d3d11_renderer_search_visible(*renderer))
        renderer->friends_requests_foreground_first_batch = paint_stream_seal(&renderer->paint_stream);
    if (d3d11_renderer_pinned_messages_visible(*renderer) && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        const auto& pins = *renderer->pinned_messages_scene;
        bool first = true;
        for (u32 i = 0; i < pins.count && i < kUiPinnedMessagesVisibleRows &&
                        renderer->media_quad_count < kRendererMediaQuadCapacity &&
                        renderer->media_batch_count < kRendererMediaBatchCapacity;
             ++i) {
            PinnedMessagesAvatarMedia media{};
            if (!pinned_messages_avatar_media_resolve(pins, i, &media))
                continue;
            const auto allocation = media_atlas_peek(renderer->media_atlas, media.key);
            if (!allocation.ok())
                continue;
            const auto r = media.rectangle;
            const u32 before = renderer->media_quad_count;
            if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip, true,
                                  first) ||
                !paint_stream_record_content(&renderer->paint_stream, media.owner, r,
                                             UiLayoutContentKind::media,
                                             ui_layout_content_bounds_allow_overlap))
                return layout_step_failed(__LINE__);
            if (renderer->media_quad_count != before) {
                // Clear only the scene's three neutral avatar shapes, once a loaded
                // quad is queued, so transparent image pixels reveal the result card.
                for (u32 q = pins_paint_first; q < pins_paint_end; ++q) {
                    auto& quad = renderer->paint_stream.quads[q];
                    const UiRect bounds{quad.left, quad.top, quad.right, quad.bottom};
                    if (quad.border_width >= 0 &&
                        (ui_rect_equal(bounds, r) ||
                         ui_rect_equal(bounds, {r.left + 10, r.top + 6, r.right - 10, r.top + 14}) ||
                         ui_rect_equal(bounds, {r.left + 6, r.top + 16, r.right - 6, r.bottom - 4}))) {
                        quad.fill.alpha = 0;
                        quad.border.alpha = 0;
                    }
                }
                first = false;
            }
        }
    }
    if (d3d11_renderer_search_visible(*renderer) && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        const auto& search = *renderer->search_scene;
        bool first = true;
        // Result avatars, then the filter chooser's people suggestions.
        for (u32 i = 0; i < kSearchAvatarMediaSlots &&
                        renderer->media_quad_count < kRendererMediaQuadCapacity &&
                        renderer->media_batch_count < kRendererMediaBatchCapacity;
             ++i) {
            SearchAvatarMedia media{};
            if (!search_avatar_media_resolve(search, i, &media))
                continue;
            const auto allocation = media_atlas_peek(renderer->media_atlas, media.key);
            if (!allocation.ok())
                continue;
            const auto r = media.rectangle;
            const u32 before = renderer->media_quad_count;
            if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip, true,
                                  first) ||
                !paint_stream_record_content(&renderer->paint_stream, media.owner, r,
                                             UiLayoutContentKind::media,
                                             ui_layout_content_bounds_allow_overlap))
                return layout_step_failed(__LINE__);
            if (renderer->media_quad_count != before) {
                // Clear only the neutral circle/person inside this exact slot.
                // Transparent profile pixels reveal the actual result card.
                for (u32 q = search_paint_first; q < renderer->paint_stream.count; ++q) {
                    auto& quad = renderer->paint_stream.quads[q];
                    const UiRect bounds{quad.left, quad.top, quad.right, quad.bottom};
                    if (quad.border_width >= 0 &&
                        (ui_rect_equal(bounds, r) ||
                         (!ui_search_scene_message_scene(search) &&
                          (ui_rect_equal(bounds, {r.left + 11, r.top + 7, r.left + 21, r.top + 17}) ||
                           ui_rect_equal(bounds, {r.left + 7, r.top + 19, r.left + 25, r.top + 27}))))) {
                        quad.fill.alpha = 0;
                        quad.border.alpha = 0;
                    }
                }
            }
            first = false;
        }
    }
    if (friends_requests_active && renderer->user_store && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        const auto& scene = *renderer->friends_requests_scene;
        const u32 count = ui_friends_scene_visible_count(scene);
        bool first_media = true;
        for (u32 i = 0; i < count && renderer->media_quad_count < kRendererMediaQuadCapacity; ++i) {
            FriendsAvatarMedia media{};
            if (!friends_avatar_media_resolve(scene, *renderer->user_store, i, &media))
                continue;
            const auto allocation = media_atlas_peek(renderer->media_atlas, media.key);
            if (!allocation.ok())
                continue;
            const auto r = media.rectangle;
            const u32 media_before = renderer->media_quad_count;
            if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip, true,
                                  first_media) ||
                !paint_stream_record_content(&renderer->paint_stream, media.owner, r,
                                             UiLayoutContentKind::media,
                                             ui_layout_content_bounds_allow_overlap))
                return layout_step_failed(__LINE__);
            reveal_media_underlay(*renderer, media_before, r, theme.interactive);
            first_media = false;
        }
    }
    if (call_visible && !queue_call_media(renderer, theme))
        return layout_step_failed(__LINE__);
    renderer->friends_requests_media_end_batch = renderer->media_batch_count;
    if (call_visible) {
        const u32 call_foreground = renderer->paint_stream.count;
        if (!ui_call_scene_paint_foreground(*renderer->call_scene, theme, &renderer->paint_stream) ||
            !split_paint_quad_boundary(&renderer->paint_stream, call_foreground,
                                       &renderer->friends_requests_foreground_first_batch))
            return layout_step_failed(__LINE__);
    }
    if (friends_requests_active &&
        !ui_friends_scene_presence_paint(*renderer->friends_requests_scene, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    if (friends_requests_active && focus_owner == &renderer->friends_requests_scene->widgets &&
        !(renderer->popup_scene && renderer->popup_scene->active) &&
        !((renderer->text_form && renderer->text_form->scene && renderer->text_form->scene->active) ||
          (renderer->forum_form && renderer->forum_form->scene && renderer->forum_form->scene->active)) &&
        !paint_stream_push_focus(&renderer->paint_stream, *focus_owner, theme.tokens.border.focus))
        return layout_step_failed(__LINE__);
    renderer->friends_requests_paint_end_batch = paint_stream_seal(&renderer->paint_stream);
    renderer->media_picker_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
    renderer->media_picker_media_first_batch = renderer->media_batch_count;
    if (d3d11_renderer_media_picker_visible(*renderer)) {
        const auto& gallery = *renderer->media_picker_scene;
        const bool filter_scrollbars = renderer->paint_stream.filter_scrollbars;
        renderer->paint_stream.filter_scrollbars = false;
        const bool painted = ui_media_picker_scene_paint(
            gallery, theme, &renderer->paint_stream, &renderer->media_picker_edit_geometry, renderer->dpi);
        renderer->paint_stream.filter_scrollbars = filter_scrollbars;
        if (!painted)
            return layout_step_failed(__LINE__);
        bool first_media = true;
        for (u32 i = 0; i < gallery.cell_count; ++i) {
            UiMediaPickerMediaSlot slot{};
            if (!ui_media_picker_scene_media(gallery, i, &slot))
                continue;
            const bool emoji = slot.kind == UiMediaPickerItemKind::emoji;
            const MediaAssetKey key =
                slot.regional_flag ? media_asset_regional_flag(static_cast<u16>(slot.regional_flag))
                : emoji ? media_asset_custom_emoji(slot.asset_id, (slot.asset_flags & 1u) != 0)
                        : MediaAssetKey{slot.asset_id, slot.variant, MediaAssetKind::catalog_preview,
                                        slot.asset_flags};
            auto* atlas = emoji ? renderer->media_atlas : renderer->preview_media_atlas;
            if (!atlas ||
                !(emoji ? renderer->media_gpu.texture_view : renderer->media_gpu.preview_texture_view))
                continue;
            const auto allocation = media_atlas_peek(atlas, key);
            if (!allocation.ok() || !allocation.width || !allocation.height)
                continue;
            const auto tile = slot.rectangle;
            const float scale_x = ui_rect_width(tile) / allocation.width;
            const float scale_y = ui_rect_height(tile) / allocation.height;
            const float scale =
                slot.fit == UiMediaPickerFit::cover ? maximum(scale_x, scale_y) : minimum(scale_x, scale_y);
            const float width = allocation.width * scale, height = allocation.height * scale;
            const float left = (tile.left + tile.right - width) * .5f;
            const float top = (tile.top + tile.bottom - height) * .5f;
            if (!queue_media_quad(renderer, allocation, left, top, left + width, top + height,
                                  ui_rect_intersection(slot.clip, tile), false, first_media, emoji ? 0 : 1,
                                  slot.opacity, 0, tile, slot.radius))
                return layout_step_failed(__LINE__);
            first_media = false;
        }
        for (u32 i = 0; i < gallery.rail_count; ++i) {
            UiMediaPickerRailMediaSlot slot{};
            if (!ui_media_picker_scene_rail_media(gallery, i, &slot) || !renderer->media_atlas ||
                !renderer->media_gpu.texture_view)
                continue;
            const auto allocation = media_atlas_peek(
                renderer->media_atlas, media_asset_guild_icon(slot.guild_id, slot.variant, slot.animated));
            if (!allocation.ok())
                continue;
            const auto r = slot.rectangle;
            if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, slot.clip, false,
                                  first_media, 0, 1.f, slot.radius))
                return layout_step_failed(__LINE__);
            first_media = false;
        }
    }
    renderer->media_picker_foreground_first_batch = paint_stream_seal(&renderer->paint_stream);
    if (d3d11_renderer_media_picker_visible(*renderer) &&
        !ui_media_picker_scene_paint_foreground(*renderer->media_picker_scene, theme,
                                                &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    renderer->media_picker_paint_end_batch = paint_stream_seal(&renderer->paint_stream);
    renderer->media_picker_media_end_batch = renderer->media_batch_count;
    // Beside a call, its text chat keeps the message hover toolbar.
    if (!content_hidden && (!call_visible || renderer->call_scene->chat_open) &&
        !(renderer->profile_scene && renderer->profile_scene->active) &&
        !d3d11_renderer_media_picker_visible(*renderer) && renderer->message_action_bar &&
        !d3d11_renderer_message_modal_active(*renderer) && renderer->message_action_bar->active &&
        !message_action_bar_presentation_build(*renderer->message_action_bar, theme,
                                               &renderer->paint_stream)) {
        return layout_step_failed(__LINE__);
    }
    renderer->profile_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
    if (renderer->profile_scene &&
        !profile_scene_paint(*renderer->profile_scene, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    renderer->profile_foreground_first_batch = ~0u;
    renderer->profile_controls_first_batch = ~0u;
    renderer->popup_media_first_batch = renderer->media_batch_count;
    renderer->profile_media_first_batch = renderer->media_batch_count;
    renderer->profile_effect_media_first_batch = renderer->media_batch_count;
    if (!settings_active && renderer->profile_scene && renderer->profile_scene->active &&
        !queue_profile_media(renderer, *renderer->profile_scene, theme,
                             &renderer->profile_effect_media_first_batch))
        return layout_step_failed(__LINE__);
    if (renderer->profile_scene && renderer->profile_scene->active) {
        renderer->profile_foreground_first_batch = paint_stream_seal(&renderer->paint_stream);
        const auto edit = d3d11_renderer_profile_edit_geometry(*renderer);
        if (!profile_scene_foreground(*renderer->profile_scene, theme, &renderer->paint_stream, &edit,
                                      renderer->dpi, &renderer->profile_controls_first_batch))
            return layout_step_failed(__LINE__);
    }
    renderer->profile_paint_end_batch = paint_stream_seal(&renderer->paint_stream);
    renderer->profile_media_end_batch = renderer->media_batch_count;
    renderer->menu_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
    renderer->menu_media_first_batch = renderer->media_batch_count;
    renderer->popup_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
    renderer->popup_scene_media_first_batch = renderer->popup_scene_media_end_batch =
        renderer->media_batch_count;
    const auto popup_edit = d3d11_renderer_popup_edit_geometry(*renderer);
    if (d3d11_renderer_download_visible(*renderer)) {
        const bool filter_scrollbars = renderer->paint_stream.filter_scrollbars;
        renderer->paint_stream.filter_scrollbars = false;
        const bool painted = ui_download_scene_paint(*renderer->download_scene, theme,
                                                     &renderer->paint_stream, renderer->dpi);
        renderer->paint_stream.filter_scrollbars = filter_scrollbars;
        if (!painted)
            return layout_step_failed(__LINE__);
    }
    if (d3d11_renderer_message_forward_visible(*renderer)) {
        const bool filter_scrollbars = renderer->paint_stream.filter_scrollbars;
        renderer->paint_stream.filter_scrollbars = false;
        const bool painted = ui_message_forward_scene_paint(
            *renderer->message_forward_scene, theme, &renderer->paint_stream,
            &renderer->media_picker_edit_geometry, &popup_edit, renderer->dpi);
        renderer->paint_stream.filter_scrollbars = filter_scrollbars;
        if (!painted)
            return layout_step_failed(__LINE__);
    }
    if (d3d11_renderer_audio_scene_visible(*renderer) &&
        !ui_audio_scene_paint(*renderer->audio_scene, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    if (d3d11_renderer_poll_scene_visible(*renderer) &&
        !ui_poll_scene_paint(*renderer->poll_scene, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    if (d3d11_renderer_poll_voters_visible(*renderer) &&
        !ui_poll_voters_scene_paint(*renderer->poll_voters_scene, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    if (d3d11_renderer_quick_switcher_visible(*renderer)) {
        const bool filter_scrollbars = renderer->paint_stream.filter_scrollbars;
        renderer->paint_stream.filter_scrollbars = false;
        const bool painted =
            ui_quick_switcher_scene_paint(*renderer->quick_switcher_scene, theme, &renderer->paint_stream,
                                          &renderer->media_picker_edit_geometry, renderer->dpi);
        renderer->paint_stream.filter_scrollbars = filter_scrollbars;
        if (!painted)
            return layout_step_failed(__LINE__);
    }
    if (d3d11_renderer_inbox_visible(*renderer)) {
        // The popout owns its scroll viewport: like the other popouts its
        // overlay scrollbar always shows where a long mention history is.
        const bool filter_scrollbars = renderer->paint_stream.filter_scrollbars;
        renderer->paint_stream.filter_scrollbars = false;
        const bool painted = ui_inbox_scene_paint(*renderer->inbox_scene, theme, &renderer->paint_stream);
        renderer->paint_stream.filter_scrollbars = filter_scrollbars;
        if (!painted)
            return layout_step_failed(__LINE__);
    }
    if (d3d11_renderer_stream_card_visible(*renderer) &&
        !ui_stream_card_paint(*renderer->stream_card_scene, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    // The server tag popout: card and banner colour, then (after its banner
    // picture, see below) the foreground under its icon and badge.
    renderer->guild_card_foreground_first_batch = ~0u;
    renderer->guild_card_banner_media_end_batch = 0;
    if (d3d11_renderer_guild_card_visible(*renderer)) {
        if (!ui_guild_card_paint(*renderer->guild_card_scene, theme, &renderer->paint_stream))
            return layout_step_failed(__LINE__);
        renderer->guild_card_foreground_first_batch = paint_stream_seal(&renderer->paint_stream);
        if (!ui_guild_card_paint_foreground(*renderer->guild_card_scene, theme, &renderer->paint_stream))
            return layout_step_failed(__LINE__);
    }
    if (d3d11_renderer_shortcuts_visible(*renderer)) {
        const bool filter_scrollbars = renderer->paint_stream.filter_scrollbars;
        renderer->paint_stream.filter_scrollbars = false;
        const bool painted = ui_shortcuts_scene_paint(*renderer->shortcuts_scene, theme, &renderer->paint_stream);
        renderer->paint_stream.filter_scrollbars = filter_scrollbars;
        if (!painted)
            return layout_step_failed(__LINE__);
    }
    if (renderer->popup_scene && renderer->popup_scene->active &&
        !popup_presentation_build(*renderer->popup_scene, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    renderer->popup_paint_end_batch = paint_stream_seal(&renderer->paint_stream);
    if (renderer->reaction_viewer && renderer->reaction_viewer->active &&
        !reaction_viewer_presentation_build(*renderer->reaction_viewer, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    if (!settings_active && renderer->image_viewer && renderer->image_viewer->active) {
        const auto& viewer = *renderer->image_viewer;
        if (!ui_image_viewer_paint(viewer, theme, &renderer->paint_stream))
            return layout_step_failed(__LINE__);
        if (renderer->preview_media_atlas && renderer->media_gpu.preview_texture_view) {
            u8 domain = 1;
            auto allocation = media_atlas_peek(renderer->preview_media_atlas, renderer->image_viewer_key);
            if (!allocation.ok())
                allocation =
                    media_atlas_peek(renderer->preview_media_atlas, renderer->image_viewer_fallback);
            // Original-tier video frames are presented into the primary atlas.
            if (!allocation.ok() && renderer->media_atlas && renderer->media_gpu.texture_view) {
                allocation = media_atlas_peek(renderer->media_atlas, renderer->image_viewer_key);
                domain = 0;
            }
            // The open/close motion travels from the inline preview, outside the body.
            const auto r = viewer.display;
            if (allocation.ok() &&
                !queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom,
                                  viewer.reveal < 1 ? viewer.viewport : viewer.body, false, true, domain))
                return layout_step_failed(__LINE__);
        }

        // This exclusive overlay returns before the normal tail initializes
        // these boundaries. The editor layer is empty while the viewer owns the
        // window, so it carries the chrome that must sit above the media quad
        // (gallery chevrons, hosted video controls) and their glyphs.
        renderer->theme_editor_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
        if (!ui_image_viewer_paint_overlay(viewer, theme, &renderer->paint_stream) ||
            (d3d11_renderer_viewer_video_visible(*renderer) &&
             !ui_video_scene_paint(*renderer->video_scene, theme, &renderer->paint_stream)))
            return layout_step_failed(__LINE__);
        renderer->theme_editor_paint_end_batch = paint_stream_seal(&renderer->paint_stream);
        renderer->form_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
        return queue_inline_flags(renderer);

    }
    bool first_interaction_media = true;
    renderer->form_paint_first_batch = paint_stream_seal(&renderer->paint_stream);

    // The server tag popout's banner picture leads the interaction media: it
    // draws over the card, under its foreground (icon ring) and icon.
    if (!settings_active && d3d11_renderer_guild_card_visible(*renderer) && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        UiGuildCardMedia media{};
        if (ui_guild_card_media(*renderer->guild_card_scene, kUiGuildCardBannerMedia, &media)) {
            const MediaAtlasResult allocation = media_atlas_find(renderer->media_atlas, media.key);
            if (allocation.ok() && allocation.width && allocation.height) {
                const auto r = ui_call_video_fit(media.rectangle, allocation.width, allocation.height, true);
                if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip, false, true,
                                      0, media.opacity, 0.f, media.mask, media.mask_radius))
                    return layout_step_failed(__LINE__);
            }
        }
        renderer->guild_card_banner_media_end_batch = renderer->media_batch_count;
    }

    // Quick switcher / people picker identities cover their kind glyphs.
    if (!settings_active && d3d11_renderer_quick_switcher_visible(*renderer) && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        const auto& switcher = *renderer->quick_switcher_scene;
        for (u32 slot = 0; slot < switcher.count; ++slot) {
            UiQuickSwitcherMedia media{};
            if (!ui_quick_switcher_scene_media(switcher, slot, renderer->chat_state, &media))
                continue;
            const MediaAtlasResult allocation = media_atlas_peek(renderer->media_atlas, media.key);
            if (!allocation.ok())
                continue;
            const auto r = media.rectangle;
            if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip, true,
                                  first_interaction_media))
                return layout_step_failed(__LINE__);
            first_interaction_media = false;
        }
    }
    // Forward destinations: server icons and DM avatars cover their tile glyphs.
    if (!settings_active && d3d11_renderer_message_forward_visible(*renderer) && renderer->media_atlas &&
        renderer->media_gpu.texture_view && renderer->chat_state) {
        const auto& forward = *renderer->message_forward_scene;
        for (u32 slot = 0; slot < forward.count; ++slot) {
            UiMessageForwardMedia media{};
            if (!ui_message_forward_scene_media(forward, slot, renderer->chat_state, &media))
                continue;
            const MediaAtlasResult allocation = media_atlas_peek(renderer->media_atlas, media.key);
            if (!allocation.ok())
                continue;
            const auto r = media.rectangle;
            if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip, media.circle,
                                  first_interaction_media, 0, 1.f, media.radius))
                return layout_step_failed(__LINE__);
            first_interaction_media = false;
        }
    }
    // Inbox mention authors and poll voters cover their neutral avatar circles.
    if (!settings_active && d3d11_renderer_inbox_visible(*renderer) && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        const auto& inbox = *renderer->inbox_scene;
        for (u32 slot = 0; slot < inbox.count && slot < kUiInboxVisibleRows; ++slot) {
            InboxAvatarMedia media{};
            if (!inbox_avatar_media_resolve(inbox, slot, &media))
                continue;
            const MediaAtlasResult allocation = media_atlas_peek(renderer->media_atlas, media.key);
            if (!allocation.ok())
                continue;
            const auto r = media.rectangle;
            const u32 before = renderer->media_quad_count;
            if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip, true,
                                  first_interaction_media))
                return layout_step_failed(__LINE__);
            reveal_media_underlay(*renderer, before, r, theme.avatar_placeholder);
            first_interaction_media = first_interaction_media && renderer->media_quad_count == before;
        }
    }
    // The stream card's preview covers its thumbnail plate.
    if (!settings_active && d3d11_renderer_stream_card_visible(*renderer)) {
        UiStreamCardMedia media{};
        if (ui_stream_card_media(*renderer->stream_card_scene, theme, &media)) {
            u8 domain = 0;
            MediaAtlasResult allocation{};
            if (!find_call_preview(*renderer, media.key, &allocation, &domain))
                allocation.error = MediaAtlasError::not_found;
            if (allocation.ok()) {
                const auto r = ui_call_video_fit(media.rectangle, allocation.width, allocation.height, true);
                if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip, false,
                                      first_interaction_media, domain, media.opacity, 0.f, media.clip, media.radius))
                    return layout_step_failed(__LINE__);
                first_interaction_media = false;
            }
        }
    }
    // The server tag popout's icon and tag badge cover their plates.
    if (!settings_active && d3d11_renderer_guild_card_visible(*renderer) && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        UiGuildCardMedia media{};
        for (u32 index = 0; index < 2; ++index, media = {}) {
            if (!ui_guild_card_media(*renderer->guild_card_scene, index, &media))
                continue;
            const MediaAtlasResult allocation = media_atlas_find(renderer->media_atlas, media.key);
            if (!allocation.ok())
                continue;
            const auto r = media.rectangle;
            if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip, false,
                                  first_interaction_media, 0, media.opacity, media.radius))
                return layout_step_failed(__LINE__);
            first_interaction_media = false;
        }
    }
    if (!settings_active && d3d11_renderer_poll_voters_visible(*renderer) && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        const auto& voters = *renderer->poll_voters_scene;
        for (u32 index = voters.first; index < voters.first + voters.visible; ++index) {
            PollVotersAvatarMedia media{};
            if (!poll_voters_avatar_media_resolve(voters, index, &media))
                continue;
            const MediaAtlasResult allocation = media_atlas_peek(renderer->media_atlas, media.key);
            if (!allocation.ok())
                continue;
            const auto r = media.rectangle;
            const u32 before = renderer->media_quad_count;
            if (!queue_media_quad(renderer, allocation, r.left, r.top, r.right, r.bottom, media.clip, true,
                                  first_interaction_media))
                return layout_step_failed(__LINE__);
            reveal_media_underlay(*renderer, before, r, theme.avatar_placeholder);
            first_interaction_media = first_interaction_media && renderer->media_quad_count == before;
        }
    }
    if (!settings_active && renderer->popup_scene && renderer->popup_scene->active &&
        renderer->media_atlas && renderer->media_gpu.texture_view) {
        const PopupScene& popup = *renderer->popup_scene;
        for (u32 index = 0; index < popup.item_count; ++index) {
            PopupMediaCommand command{};
            const PopupMediaPresentationStatus resolved =
                popup_media_presentation_resolve(popup, index, &command);
            if (resolved == PopupMediaPresentationStatus::skip) {
                continue;
            }
            if (resolved != PopupMediaPresentationStatus::ready) {
                saturating_increment(renderer->media_rejected);
                log_u64(LogLevel::warning, "render", "popup_emoji_layout_rejected", index);
                continue;
            }
            const MediaAssetKey key =
                command.guild_icon ? media_asset_guild_icon(command.media_id, command.media_variant,
                                                            (command.media_flags & 1u) != 0)
                : command.avatar   ? media_asset_user_avatar(command.media_id, command.media_variant,
                                                             (command.media_flags & 1u) != 0)
                : command.regional_flag
                    ? media_asset_regional_flag(command.regional_flag)
                    : media_asset_custom_emoji(command.media_id, (command.media_flags & 1u) != 0);
            const MediaAtlasResult allocation = media_atlas_peek(renderer->media_atlas, key);
            if (!allocation.ok()) {
                continue;
            }
            const u32 before = renderer->media_quad_count;
            if (!queue_media_quad(renderer, allocation, command.rectangle.left, command.rectangle.top,
                                  command.rectangle.right, command.rectangle.bottom, command.clip,
                                  command.avatar || command.guild_icon, first_interaction_media)) {
                return layout_step_failed(__LINE__);
            }
            if (command.avatar && (popup.items[index].flags &
                                   (popup_item_command_row | popup_item_command_category)))
                reveal_media_underlay(*renderer, before, command.rectangle, theme.interactive);
            first_interaction_media = false;
        }
    }
    renderer->popup_scene_media_end_batch = renderer->media_batch_count;
    // Reaction viewer may share the same clip rectangle, but never a fading
    // popup's media batch. The next loaded quad starts a new batch.
    first_interaction_media = true;
    if (!settings_active && renderer->reaction_viewer && renderer->reaction_viewer->active &&
        renderer->media_atlas && renderer->media_gpu.texture_view) {
        const ReactionViewer& viewer = *renderer->reaction_viewer;
        for (u32 index = viewer.first_aggregate; index < viewer.aggregate_count; ++index) {
            const UiRect row = reaction_viewer_aggregate_rectangle(viewer, index);
            if (!ui_rect_has_area(row)) {
                break;
            }
            const ReactionViewerAggregate& aggregate = viewer.aggregates[index];
            const u16 flag = aggregate.emoji_id == 0
                                 ? regional_flag_sequence(reaction_viewer_aggregate_name(viewer, index))
                                 : 0;
            if (aggregate.emoji_id == 0 && !flag) {
                continue;
            }
            const MediaAtlasResult allocation = media_atlas_peek(
                renderer->media_atlas,
                flag ? media_asset_regional_flag(flag)
                     : media_asset_custom_emoji(aggregate.emoji_id, (aggregate.flags & 1u) != 0));
            if (!allocation.ok()) {
                continue;
            }
            const float extent = 24.0f;
            const float left = row.left + 8.0f;
            const float top = (row.top + row.bottom - extent) * 0.5f;
            if (!queue_media_quad(renderer, allocation, left, top, left + extent, top + extent, viewer.body,
                                  false, first_interaction_media)) {
                return layout_step_failed(__LINE__);
            }
            first_interaction_media = false;
        }
        for (u32 index = viewer.first_user; index < viewer.user_count; ++index) {
            const UiRect row = reaction_viewer_user_rectangle(viewer, index);
            if (!ui_rect_has_area(row)) {
                break;
            }
            const ReactionViewerUser& user = viewer.users[index];
            const Utf8View hash = reaction_viewer_user_avatar_hash(user);
            const u32 variant = user_avatar_variant(hash);
            if (variant == 0) {
                continue;
            }
            const bool animated = asset_hash_animated(hash);
            const MediaAtlasResult allocation = media_atlas_peek(
                renderer->media_atlas, media_asset_user_avatar(user.id, variant, animated));
            if (!allocation.ok()) {
                continue;
            }
            constexpr float extent = 32.0f;
            const float top = (row.top + row.bottom - extent) * 0.5f;
            if (!queue_media_quad(renderer, allocation, row.left, top, row.left + extent, top + extent,
                                  viewer.body, true, first_interaction_media)) {
                return layout_step_failed(__LINE__);
            }
            first_interaction_media = false;
        }
    }
    renderer->theme_editor_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
    if (renderer_theme_editor_visible(*renderer)) {
        if (!paint_stream_push_clip(&renderer->paint_stream, 0, 0, width_dip, height_dip))
            return layout_step_failed(__LINE__);
        renderer->theme_editor_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
        if (!ui_theme_editor_paint(*renderer->theme_editor_scene, theme, &renderer->paint_stream) ||
            !paint_stream_pop_clip(&renderer->paint_stream))
            return layout_step_failed(__LINE__);
    }
    renderer->theme_editor_paint_end_batch = paint_stream_seal(&renderer->paint_stream);
    // Quick switcher status dots sit on the avatars queued above.
    renderer->quick_switcher_status_first_batch = paint_stream_seal(&renderer->paint_stream);
    if (!settings_active && d3d11_renderer_quick_switcher_visible(*renderer) &&
        !ui_quick_switcher_scene_presence_paint(*renderer->quick_switcher_scene, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    renderer->quick_switcher_status_end_batch = paint_stream_seal(&renderer->paint_stream);
    renderer->form_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
    if (renderer->forum_form && renderer->forum_form->scene && renderer->forum_form->scene->active &&
        !forum_post_paint(*renderer->forum_form->scene, renderer->forum_form->geometry, theme,
                          &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    renderer->forum_paint_end_batch = paint_stream_seal(&renderer->paint_stream);
    if (renderer->text_form && renderer->text_form->scene && renderer->text_form->scene->active &&
        !text_form_paint(*renderer->text_form->scene, renderer->text_form->geometry, theme,
                         &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    if (renderer_settings_page_visible(*renderer) &&
        !ui_settings_scene_paint(*renderer->settings_scene, theme, &renderer->paint_stream, renderer->dpi))
        return layout_step_failed(__LINE__);
    if (d3d11_renderer_tooltip_visible(*renderer)) {
        renderer->form_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
        if (!ui_tooltip_paint(*renderer->tooltip_scene, theme, &renderer->paint_stream))
            return layout_step_failed(__LINE__);
    }
    return queue_inline_flags(renderer);
}

// Stamped inputs that build_layout itself moves: the timeline geometry count
// it publishes and media atlas entries a lookup erases as stale. While they
// hold, the stamp captured before the build still describes the frame.
struct BuildMarks {
    u64 values[14];
};
BuildMarks build_marks(const D3D11Renderer& renderer) noexcept {
    BuildMarks marks{};
    marks.values[0] = renderer.content_text ? renderer.content_text->message_geometry_quads : ~0ull;
    const MediaAtlas* atlases[2]{renderer.media_atlas, renderer.preview_media_atlas};
    for (u32 i = 0; i < 2; ++i) {
        if (const MediaAtlas* atlas = atlases[i]) {
            u64* value = marks.values + 1 + i * 6;
            value[0] = atlas->entry_count;
            value[1] = atlas->insertions;
            value[2] = atlas->stale_entries;
            value[3] = atlas->allocations.evicted_entries;
            value[4] = atlas->allocations.full_resets;
            value[5] = atlas->next_allocation_key;
        }
    }
    // Edit geometry that scene paint publishes for the stamp.
    const UiEditGeometry* geometry[]{&renderer.search_edit_geometry, &renderer.media_picker_edit_geometry,
                                     &renderer.friends_search_geometry};
    u64 hash = kHash64Seed;
    for (const auto* edit : geometry) {
        const u32 selections = minimum<u32>(edit->selection_count, 48);
        hash = hash64_bytes(hash, &edit->text, sizeof(edit->text) + sizeof(edit->caret));
        hash = hash64_bytes(hash64_mix(hash, edit->selection_count), edit->selections, selections * sizeof(UiRect));
    }
    marks.values[13] = hash;
    return marks;
}

bool retained_root_is_opaque(const D3D11Renderer& renderer, float viewport_width,
                             float viewport_height) noexcept {
    if (renderer.paint_stream.count == 0 || renderer.paint_stream.batch_count == 0 ||
        renderer.paint_stream.clip_count == 0) {
        return false;
    }
    const PaintQuad& root = renderer.paint_stream.quads[0];
    const PaintBatch& batch = renderer.paint_stream.batches[0];
    if (batch.clip_index >= renderer.paint_stream.clip_count) {
        return false;
    }
    const PaintClip& clip = renderer.paint_stream.clips[batch.clip_index];
    return batch.first_quad == 0 && batch.quad_count != 0 &&
           batch.clip_index < renderer.paint_stream.clip_count && root.left <= 0.0f && root.top <= 0.0f &&
           root.right >= viewport_width && root.bottom >= viewport_height && root.fill.alpha >= 1.0f &&
           root.radius == 0.0f && clip.left <= 0.0f && clip.top <= 0.0f && clip.right >= viewport_width &&
           clip.bottom >= viewport_height;
}

} // namespace

void d3d11_renderer_shell_segment_release(RendererShellSegment* segment) noexcept {
    if (!segment)
        return;
    stable_storage_release(&segment->paint_quads);
    stable_storage_release(&segment->media_quads);
    stable_storage_release(&segment->media_handles);
    stable_storage_release(&segment->media_domains);
    stable_storage_release(&segment->media_batches);
    *segment = RendererShellSegment{};
}

bool d3d11_renderer_build_paint(D3D11Renderer* renderer, const Theme& theme) noexcept {
    return build_layout(renderer, theme);
}

bool d3d11_renderer_paint_layout(D3D11Renderer* renderer, const Theme& theme,
                                 const RendererPresentationStamp& current) noexcept {
    // The shell beneath a modal is redrawn offscreen first; the modal's own
    // stream then rebuilds over it and presents in full.
    RendererPresentationStamp refreshed{};
    const RendererPresentationStamp* stamp = &current;
    if (renderer_settings_backdrop_stale(renderer, theme)) {
        if (!renderer_refresh_settings_backdrop(renderer, theme))
            return false;
        refreshed = renderer_capture_presentation_stamp(*renderer, theme);
        stamp = &refreshed;
    }
    renderer->present_required = false;
    renderer->present_full = false;
    renderer->present_logical_damage_valid = false;
    renderer->present_logical_damage_full = false;
    renderer->present_logical_damage_rect = {};
    bool media_source_changed = false;
    const bool retained = (!renderer->login_visible || d3d11_renderer_login_form_text(*renderer)) &&
                          renderer_presentation_stamp_equal(*stamp, renderer->retained_presentation) &&
                          refresh_media_quad_sources(renderer, &media_source_changed);
    if (!retained) {
        // Consecutive rejected builds; logged once per power of two with the
        // stream's bounds, so a capacity overflow names itself without
        // writing a line per frame.
        static u32 layout_failures = 0;
        const BuildMarks marks = build_marks(*renderer);
        // An invalidated stamp means an input it does not hash moved (a
        // server pill animating, a rebinding): such a build paints the shell
        // afresh and records it again.
        const bool replay_shell = renderer->retained_presentation.valid;
        g_layout_failed_line = 0;
        if (!build_layout(renderer, theme, stamp, replay_shell)) {
            renderer->retained_presentation.valid = false;
            saturating_increment(layout_failures);
            if ((layout_failures & (layout_failures - 1u)) == 0) {
                const auto& paint = renderer->paint_stream;
                log_write(LogLevel::error, "ui", "semantic_scene_layout_failed");
                log_u64(LogLevel::error, "ui", "semantic_scene_layout_failures", layout_failures);
                log_u64(LogLevel::error, "ui", "semantic_scene_layout_failed_line", g_layout_failed_line);
                log_u64(LogLevel::error, "ui", "semantic_scene_layout_paint_quads", paint.count);
                log_u64(LogLevel::error, "ui", "semantic_scene_layout_paint_clips", paint.clip_count);
                log_u64(LogLevel::error, "ui", "semantic_scene_layout_paint_batches", paint.batch_count);
                log_u64(LogLevel::error, "ui", "semantic_scene_layout_paint_rejected", paint.rejected);
            }
            return false;
        }
        layout_failures = 0;
        const BuildMarks built = build_marks(*renderer);
        renderer->retained_presentation = bytes_equal(&built, &marks, sizeof(marks))
                                              ? *stamp
                                              : renderer_capture_presentation_stamp(*renderer, theme);
        ++renderer->paint_stream_builds;
        if (!renderer_paint_diff_damage(renderer, theme))
            presentation_damage_invalidate_full(&renderer->presentation_damage);
    } else {
        ++renderer->paint_stream_reuses;
        if (!media_source_changed) {
            ++renderer->media_source_upload_skips;
        }
    }
    if (renderer->paint_stream.rejected != 0 && !renderer->paint_overflow_reported) {
        renderer->paint_overflow_reported = true;
        log_write(LogLevel::error, "render", "ui_paint_stream_capacity_exceeded");
    }
    const float viewport_width =
        static_cast<float>(renderer->width) * 96.0f / static_cast<float>(maximum<u32>(renderer->dpi, 1));
    const float viewport_height =
        static_cast<float>(renderer->height) * 96.0f / static_cast<float>(maximum<u32>(renderer->dpi, 1));
    PresentationDamage damage =
        presentation_damage_current(renderer->presentation_damage, viewport_width, viewport_height);
    if (damage.valid && !damage.full &&
        !retained_root_is_opaque(*renderer, viewport_width, viewport_height)) {
        presentation_damage_invalidate_full(&renderer->presentation_damage);
        damage =
            presentation_damage_current(renderer->presentation_damage, viewport_width, viewport_height);
    }
    if (!damage.valid && !renderer->begin_frame_permit) {
        // The only outstanding mutations were for non-visible atlas slots.
        // They do not require a swap-chain present and can become baseline;
        // navigation/layout changes already force a complete scene rebuild.
        presentation_damage_commit(&renderer->presentation_damage);
        renderer_paint_diff_commit(renderer);
        renderer->draw_calls_last_frame = 0;
        renderer->media_draws_last_frame = 0;
        ++renderer->unchanged_present_skips;
        return true;
    }

    RECT logical_rectangle{};
    bool logical_valid = damage.valid;
    if (logical_valid && !damage.full) {
        logical_rectangle = physical_damage_rect(*renderer, damage.rectangle);
        if (!physical_rect_has_area(logical_rectangle)) {
            // The only current mutations mapped outside the physical client.
            presentation_damage_commit(&renderer->presentation_damage);
            logical_valid = false;
        }
    }
    renderer->present_logical_damage_valid = logical_valid;
    renderer->present_logical_damage_full = logical_valid && damage.full;
    *renderer->present_logical_damage_rect.native() = logical_rectangle;

    // DXGI flip-sequential does not make every back buffer inherit the last
    // presented image.  Repaint the union of changes since the current buffer
    // was last displayed (one prior Present for this two-buffer chain).
    renderer->present_full = (logical_valid && damage.full) || (renderer->previous_buffer_damage_valid &&
                                                                renderer->previous_buffer_damage_full);
    renderer->present_required = true;
    if (!renderer->present_full) {
        const bool previous_valid = renderer->previous_buffer_damage_valid &&
                                    physical_rect_has_area(*renderer->previous_buffer_damage_rect.native());
        if (logical_valid && previous_valid) {
            *renderer->present_dirty_rect.native() =
                physical_rect_union(logical_rectangle, *renderer->previous_buffer_damage_rect.native());
        } else if (logical_valid) {
            *renderer->present_dirty_rect.native() = logical_rectangle;
        } else if (previous_valid) {
            renderer->present_dirty_rect = renderer->previous_buffer_damage_rect;
        } else {
            // Preserve the present-driven DXGI clock without rotating an
            // untouched/stale buffer.  One repainted pixel is sufficient to
            // acknowledge a BeginFrame when rounding produced no new damage.
            renderer->present_dirty_rect = {0, 0, 1, 1};
        }
    }
    const D3D11_RECT* scissor = renderer->present_full ? nullptr : renderer->present_dirty_rect.native();
    return d3d11_renderer_draw_stream(renderer, theme, !retained, !retained || media_source_changed,
                                      scissor);
}


} // namespace maicord
