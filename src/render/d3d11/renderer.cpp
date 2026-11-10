#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/log_event.hpp"
#include "maicord/core/time.hpp"
#include "maicord/render/d3d11/renderer_presentation.hpp"
#include "maicord/ui/chat/composer/message_composer_scene.hpp"
#include "maicord/render/text/form_surface.hpp"
#include "maicord/ui/chat/messages/text_presentation.hpp"
#include "renderer_caption.hpp"
#include "effect_inputs.hpp"
#include "renderer_edit.hpp"
#include "renderer_layout.hpp"
#include "renderer_login_form.hpp"
#include "../text/shaped_text_cache.hpp"

#include "maicord/core/virtual_memory.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/ui/text/measure_cache.hpp"

namespace maicord {
namespace {

bool update_frame_transform(D3D11Renderer* renderer) noexcept {
    renderer->pipeline.backdrop_caption = renderer->caption_height;
    return d3d11_draw_pipeline_update_transform(renderer->pipeline, renderer->graphics.context, renderer->width,
                                                renderer->height, renderer->dpi, renderer->target_origin_y);
}

// A failure that may be the device's: a removal or reset is remembered for the host, which recovers.
void note_device_failure(D3D11Renderer* renderer) noexcept {
    if (gpu_device_lost(renderer->graphics.device))
        renderer->device_lost = true;
}

bool create_glyph_instance_buffer(D3D11Renderer* renderer, u32 capacity) noexcept {
    const D3D11DrawPipelineResult reserved = d3d11_draw_pipeline_reserve_glyph_instances(
        &renderer->pipeline, renderer->graphics.device, sizeof(GlyphQuad), capacity);
    if (!reserved.ok()) {
        LogEvent("glyph_instance_buffer_growth_failed")
            .error("hr", static_cast<u32>(reserved.native_error))
            .write(LogLevel::error, "render");
        return false;
    }
    renderer->uploaded_text_storage = nullptr;
    renderer->uploaded_text_rebuild = 0;
    renderer->uploaded_text_quad_count = 0;
    return true;
}

bool create_gpu_text_resources(D3D11Renderer* renderer, GlyphAtlasGpu* prestarted_atlas) noexcept {
    const D3D11GlyphTextInitializeResult initialized =
        d3d11_glyph_text_initialize(&renderer->glyph_text, renderer->graphics.device,
                                    renderer->graphics.context, renderer->dpi, prestarted_atlas);
    if (!initialized.ok()) {
        LogEvent("gpu_text_resources_creation_failed")
            .field("stage", static_cast<u64>(initialized.error))
            .error("hr", static_cast<u32>(initialized.native_error))
            .write(LogLevel::error, "render");
        return false;
    }
    return true;
}

constexpr D3D11DrawPipelineConfig kPipelineConfig{sizeof(PaintQuad), kD3D11InitialUiInstanceCapacity,
                                                sizeof(GlyphQuad), kRendererGlyphCapacity,
                                                sizeof(MediaQuad), kRendererMediaQuadCapacity};
static_assert(kMaxPaintQuads == kD3D11MaximumUiInstanceCapacity);

// The glyph atlas and draw-pipeline objects built by the device prestart
// worker while the main thread creates the swap chain, composition tree and
// icon atlas. The atlas comes first: the main thread's first frame needs it.
// The worker also draws once with each pipeline, so the driver finishes them
// off the main thread. The device pointers identify which device they belong
// to; they are not owned.
D3D11DrawPipelineOwner g_prestarted_pipeline{};
GpuDevice* g_prestarted_pipeline_device = nullptr;
GlyphAtlasGpu g_prestarted_glyph_atlas{};
GpuDevice* g_prestarted_glyph_atlas_device = nullptr;

void prestart_objects(GpuDevice* device, GpuContext* context) noexcept {
    if (glyph_atlas_gpu_initialize_paged(&g_prestarted_glyph_atlas, device, kRendererGlyphAtlasExtent,
                                         kRendererGlyphAtlasPageRows, kRendererGlyphAtlasPages,
                                         kRendererPathCapacity)
            .ok())
        g_prestarted_glyph_atlas_device = device;
    if (d3d11_draw_pipeline_create(&g_prestarted_pipeline, device, kPipelineConfig).ok()) {
        d3d11_draw_pipeline_warm(&g_prestarted_pipeline, device, context, kPipelineConfig);
        g_prestarted_pipeline_device = device;
    }
}

void release_prestarted_pipeline() noexcept {
    // The objects go before the device does: finish releases a device nobody adopted, and a GL object dies with its
    // device, so releasing it afterwards would call into a device that is gone.
    gpu_device_prestart_wait();
    d3d11_draw_pipeline_release(&g_prestarted_pipeline);
    g_prestarted_pipeline_device = nullptr;
    glyph_atlas_gpu_release(&g_prestarted_glyph_atlas);
    g_prestarted_glyph_atlas_device = nullptr;
    gpu_device_prestart_finish();
}

// Joins the prestart worker and lends its glyph atlas to the text resources
// when it belongs to this device; null otherwise.
GlyphAtlasGpu* prestarted_glyph_atlas(const D3D11Renderer& renderer) noexcept {
    gpu_device_prestart_finish();
    return g_prestarted_glyph_atlas_device && g_prestarted_glyph_atlas_device == renderer.graphics.device
               ? &g_prestarted_glyph_atlas
               : nullptr;
}

D3D11DrawPipelineResult initialize_pipeline(D3D11Renderer* renderer) noexcept {
    gpu_device_prestart_finish();
    // An atlas the text resources did not take (another device, or their
    // initialization failed first) is released with the worker's leftovers.
    glyph_atlas_gpu_release(&g_prestarted_glyph_atlas);
    g_prestarted_glyph_atlas_device = nullptr;
    D3D11DrawPipelineOwner staged = g_prestarted_pipeline;
    const bool prestarted = g_prestarted_pipeline_device && g_prestarted_pipeline_device == renderer->graphics.device;
    reset_value(&g_prestarted_pipeline);
    g_prestarted_pipeline_device = nullptr;
    D3D11DrawPipelineResult created{S_OK, D3D11DrawPipelineError::none};
    if (prestarted) {
        renderer->startup_prestart |= 2;
    } else {
        d3d11_draw_pipeline_release(&staged);
        created = d3d11_draw_pipeline_create(&staged, renderer->graphics.device, kPipelineConfig);
        if (created.ok())
            d3d11_draw_pipeline_warm(&staged, renderer->graphics.device, renderer->graphics.context, kPipelineConfig);
    }
    if (!created.ok())
        return created;
    d3d11_draw_pipeline_publish(&renderer->pipeline, &staged);
    // Sets the viewport, which warming left on its scratch target.
    return update_frame_transform(renderer)
               ? created
               : D3D11DrawPipelineResult{E_INVALIDARG, D3D11DrawPipelineError::invalid_input};
}

} // namespace

void renderer_prestart() noexcept {
    gpu_device_prestart(prestart_objects);
}

void renderer_prestart_cancel() noexcept {
    release_prestarted_pipeline();
}

bool renderer_set_background_phase(D3D11Renderer* renderer, float seconds) noexcept {
    if (!renderer || !(seconds >= 0 && seconds <= 3.402823466e38f))
        return false;
    if (renderer->background_phase_seconds == seconds)
        return true;
    renderer->background_phase_seconds = seconds;
    presentation_damage_invalidate_full(&renderer->presentation_damage);
    return true;
}

EffectSelectResult renderer_select_effect(D3D11Renderer* renderer, const BackgroundEffectSpec& spec,
                                          const EffectSettings& settings) noexcept {
    if (!renderer || !renderer->graphics.device)
        return {E_INVALIDARG, EffectSelect::rejected};
    // The glyph grid is rasterized only while this device does not hold the package yet.
    const auto& effect = renderer->effect;
    GpuSrv* glyphs = nullptr;
    if (!(effect.selected && effect.identity == spec.identity && effect.shader) &&
        (spec.uses & effect_use_glyphs) && !effect_glyph_grid_build(renderer, &glyphs))
        return {E_FAIL, EffectSelect::rejected};
    const EffectSelectResult result = d3d11_effect_select(
        &renderer->effect, renderer->graphics.device, renderer->graphics.context, spec, settings,
        renderer->adapter_key, glyphs, renderer->width, renderer->height + renderer->caption_height);
    if (result.state == EffectSelect::rejected)
        gpu_release(glyphs);
    if (result.state == EffectSelect::changed || result.state == EffectSelect::refused) {
        presentation_damage_invalidate_full(&renderer->presentation_damage);
        reset_value(&renderer->settings_backdrop_stamp);
    }
    return result;
}

void renderer_clear_effect(D3D11Renderer* renderer) noexcept {
    if (renderer && d3d11_effect_clear(&renderer->effect)) {
        presentation_damage_invalidate_full(&renderer->presentation_damage);
        reset_value(&renderer->settings_backdrop_stamp);
    }
}

void renderer_set_effect_ambient(D3D11Renderer* renderer, const float (&values)[4]) noexcept {
    if (!renderer || bytes_equal(renderer->effect.ambient, values, sizeof(values)))
        return;
    copy_bytes(renderer->effect.ambient, values, sizeof(values));
    if (renderer->effect.selected) {
        presentation_damage_invalidate_full(&renderer->presentation_damage);
        reset_value(&renderer->settings_backdrop_stamp);
    }
}

void renderer_set_effect_light(D3D11Renderer* renderer, float amount) noexcept {
    amount = amount >= 0 && amount <= 1 ? amount : 0;
    if (!renderer || renderer->effect.light == amount)
        return;
    renderer->effect.light = amount;
    if (renderer->effect.selected) {
        presentation_damage_invalidate_full(&renderer->presentation_damage);
        reset_value(&renderer->settings_backdrop_stamp);
    }
}

void renderer_bind_theme_editor_scene(D3D11Renderer* renderer, const UiThemeEditorScene* scene) noexcept {
    if (!renderer || renderer->theme_editor_scene == scene)
        return;
    renderer->theme_editor_scene = scene;
    renderer->interaction_text_theme_editor_revision = 0;
    renderer->interaction_text_active_mask = ~u32{0};
    presentation_damage_invalidate_full(&renderer->presentation_damage);
}

bool renderer_initialize(D3D11Renderer* renderer, const GpuSurfaceDesc& surface, bool content_text) noexcept {
    if (!renderer || !renderer_media_draw_storage_initialize(&renderer->media_draw)) {
        return false;
    }
    // Identity animation frames (<= 128 px), backed as frames are cached.
    if (!animation_frame_cache_gpu_initialize(&renderer->animation_gpu)) {
        animation_frame_cache_gpu_release(&renderer->animation_gpu);
        renderer_media_draw_storage_release(&renderer->media_draw);
        return false;
    }
    renderer->layer_opacity = {1, 1, 1, 1, 1, 1};
    renderer->settings_modal_progress = 1;
    renderer->draw_layer_scale = 1;
    for (auto& transform : renderer->layer_transform)
        transform.scale = 1;
    presentation_damage_reset(&renderer->presentation_damage);
    reset_value(&renderer->present_dirty_rect);
    reset_value(&renderer->previous_buffer_damage_rect);
    reset_value(&renderer->present_logical_damage_rect);
    renderer->present_required = false;
    renderer->present_full = false;
    renderer->previous_buffer_damage_valid = false;
    renderer->previous_buffer_damage_full = false;
    renderer->present_logical_damage_valid = false;
    renderer->present_logical_damage_full = false;
    renderer->begin_frame_permit = false;

    // The surface is the caller's: its window handle (null for an offscreen surface where the backend has them), size and DPI.
    const D3D11DeviceOwnerResult graphics = d3d11_device_owner_initialize(&renderer->graphics, surface);
    if (!graphics.ok()) {
        // The backend logged the step that failed when it was the surface.
        const char* event = graphics.error == D3D11DeviceOwnerError::hardware_device ? "hardware_d3d11_device_unavailable"
                            : graphics.error == D3D11DeviceOwnerError::surface       ? "d3d11_surface_failed"
                                                                                     : "d3d11_device_owner_initialization_failed";
        LogEvent(event).error("hr", static_cast<u32>(graphics.native_error)).write(LogLevel::error, "render");
        renderer_shutdown(renderer);
        return false;
    }
    renderer->width = surface.width;
    renderer->height =
        surface.height > renderer->caption_height ? surface.height - renderer->caption_height : 1;
    renderer->target_origin_y = renderer->caption_height;
    renderer->dpi = surface.dpi;
    renderer->window = surface.window;
    renderer->adapter_key = graphics.adapter_key;
    renderer->startup_prestart = graphics.prestarted ? 1 : 0;
    // The icon atlas is created here while the prestart worker still builds the
    // glyph atlas and draw pipeline; the text resources then adopt its atlas.
    if (!icon_atlas_initialize(&renderer->icon_atlas, renderer->graphics.device))
        log_write(LogLevel::error, "render", "icon_atlas_creation_failed");
    GlyphAtlasGpu* const prestarted_atlas = prestarted_glyph_atlas(*renderer);
    if (!create_gpu_text_resources(renderer, prestarted_atlas)) {
        renderer_shutdown(renderer);
        return false;
    }
    if (prestarted_atlas && !prestarted_atlas->texture)
        renderer->startup_prestart |= 4;
    // The prestart worker's pipeline, or one built here when it did not run.
    const D3D11DrawPipelineResult pipeline = initialize_pipeline(renderer);
    if (!pipeline.ok()) {
        LogEvent("d3d11_draw_pipeline_initialization_failed")
            .field("stage", static_cast<u64>(pipeline.error))
            .error("hr", static_cast<u32>(pipeline.native_error))
            .write(LogLevel::error, "render");
        renderer_shutdown(renderer);
        return false;
    }
    // Created now, the content glyph ring costs a fraction of growing the
    // pipeline's small ring when content text first binds. A failure grows it then.
    if (content_text)
        d3d11_draw_pipeline_reserve_glyph_instances(&renderer->pipeline, renderer->graphics.device,
                                                    sizeof(GlyphQuad), kRendererContentGlyphCapacity);
    LogEvent("hardware_d3d11_instanced_ui_ready").field("startup_prestart", renderer->startup_prestart)
        .write(LogLevel::info, "render");
    return true;
}

bool renderer_resize(D3D11Renderer* renderer, u32 width, u32 height) noexcept {
    if (!renderer || !gpu_surface_valid(&renderer->graphics.surface) || width == 0 || height == 0) {
        return true;
    }
    const u32 target_height = height + renderer->caption_height;
    if (renderer->width == width && renderer->height == height && gpu_surface_target(&renderer->graphics.surface) &&
        (renderer->live_resize ||
         (gpu_surface_width(&renderer->graphics.surface) == width &&
          gpu_surface_height(&renderer->graphics.surface) == target_height))) {
        return true;
    }
    // Reserve a small growth margin only inside Windows' sizing modal loop.
    // Reallocating a composition flip chain for every pointer pixel piles up
    // work for the compositor while the normal display-clock loop is paused.
    // A client that still fits the current buffers is clipped, not reallocated;
    // finishing the resize trims the buffers to the final client size.
    u32 buffer_width = width, buffer_height = target_height;
    if (renderer->live_resize) {
        buffer_width = gpu_surface_width(&renderer->graphics.surface);
        buffer_height = gpu_surface_height(&renderer->graphics.surface);
        if (width > buffer_width || target_height > buffer_height) {
            buffer_width = maximum((width + 63u) & ~63u, buffer_width);
            buffer_height = maximum((target_height + 63u) & ~63u, buffer_height);
        }
    }
    if (buffer_width != gpu_surface_width(&renderer->graphics.surface) ||
        buffer_height != gpu_surface_height(&renderer->graphics.surface)) {
        const GpuStatus resized = gpu_surface_resize(&renderer->graphics.surface, buffer_width, buffer_height);
        if (!gpu_ok(resized)) {
            // The backend logged whether the buffers or the render target failed.
            log_u64(LogLevel::error, "render", "d3d11_resize_native_error", static_cast<u32>(resized));
            note_device_failure(renderer);
            return false;
        }
        // ResizeBuffers replaces both images, but not the swap chain's frame
        // latency semaphore. Keep an already acquired slot until Present uses it.
        renderer->previous_buffer_damage_valid = false;
        renderer->previous_buffer_damage_full = false;
        reset_value(&renderer->previous_buffer_damage_rect);
        renderer->retained_presentation.valid = false;
        presentation_damage_invalidate_full(&renderer->presentation_damage);
        d3d11_renderer_caption_invalidate(renderer);
    } else {
        // The DirectComposition target clips the natural-size visual to the
        // HWND client. Keeping the source region equal to the backing texture
        // avoids asynchronously scaling a new source against an older DWM
        // target during the sizing modal loop.
        ++renderer->resize_clipped_updates;
    }
    renderer->width = width;
    renderer->height = height;
    return update_frame_transform(renderer);
}

bool renderer_finish_resize(D3D11Renderer* renderer, u32 width, u32 height) noexcept {
    if (!renderer || width == 0 || height == 0) {
        return false;
    }
    renderer->live_resize = false;
    // The sizing reserve must not survive into ordinary presentation, even
    // when the last modal paint already used this logical client size.
    return renderer_resize(renderer, width, height);
}

void renderer_set_live_resize(D3D11Renderer* renderer, bool live_resize) noexcept {
    if (renderer) {
        renderer->live_resize = live_resize;
    }
}

void renderer_accept_begin_frame(D3D11Renderer* renderer) noexcept {
    if (renderer && gpu_frame_clock_waitable(&renderer->graphics.surface) && !renderer->live_resize) {
        renderer->begin_frame_permit = true;
    }
}

bool renderer_present(D3D11Renderer* renderer, const Theme& theme) noexcept {
    if (!gpu_surface_target(&renderer->graphics.surface) || !gpu_surface_valid(&renderer->graphics.surface) ||
        renderer->device_lost) {
        return false;
    }
    if (renderer->has_presented)
        renderer_note_text_effect_damage(renderer, clock_monotonic_milliseconds());
    const RendererPresentationStamp current = renderer_capture_presentation_stamp(*renderer, theme);
    if (renderer->has_presented &&
        (!renderer->login_visible || d3d11_renderer_login_form_text(*renderer)) &&
        !renderer->begin_frame_permit && !renderer_settings_backdrop_stale(renderer, theme) &&
        renderer_presentation_stamp_equal(current, renderer->retained_presentation) &&
        presentation_damage_empty(renderer->presentation_damage) && !renderer_typing_dots_pending(*renderer) &&
        !d3d11_renderer_caption_pending(renderer, theme)) {
        renderer->draw_calls_last_frame = 0;
        renderer->media_draws_last_frame = 0;
        ++renderer->unchanged_present_skips;
        return true;
    }
    // Modal resize presents must consume a slot too. Skipping these waits
    // accumulates semaphore credits, allowing normal frames to queue far ahead
    // of the compositor after the drag ends.
    if (gpu_frame_clock_waitable(&renderer->graphics.surface) && !renderer->begin_frame_permit) {
        if (!gpu_frame_wait(&renderer->graphics.surface, 1'000u)) {
            log_write(LogLevel::error, "render", "frame_latency_wait_failed");
            return false;
        }
        renderer->begin_frame_permit = true;
    }
    const bool first_paint = renderer->painted_frames == 0;
    const Clock clock = clock_initialize();
    const i64 paint_begin = first_paint ? clock_now() : 0;
    if (!d3d11_renderer_paint_layout(renderer, theme, current)) {
        note_device_failure(renderer);
        return false;
    }
    if (first_paint)
        renderer->first_frame_cpu_us = clock_microseconds(clock, paint_begin, clock_now());
    ++renderer->painted_frames;
    if (!renderer->present_required) {
        return true;
    }
    // The first image has no prior frame to pace against. Avoid making startup
    // pay a complete refresh interval; later invalidations remain synchronized.
    const UINT sync_interval = renderer->live_resize || !renderer->has_presented ? 0u : 1u;
    // Content damage is in content rows; the caption is its own rectangle.
    RECT dirty[2]{*renderer->present_dirty_rect.native(), {}};
    dirty[0].top += static_cast<LONG>(renderer->target_origin_y);
    dirty[0].bottom += static_cast<LONG>(renderer->target_origin_y);
    const u32 dirty_count = 1 + d3d11_renderer_caption_dirty_rect(*renderer, &dirty[1]);
    const GpuStatus result = gpu_surface_present(&renderer->graphics.surface, sync_interval,
                                                 renderer->present_full ? nullptr
                                                                        : reinterpret_cast<const GpuRect*>(dirty),
                                                 dirty_count);
    if (!gpu_ok(result)) {
        log_write(LogLevel::error, "render", "present_failed_or_device_removed");
        note_device_failure(renderer);
        return false;
    }
    // A frame-latency permit represents one available swap-chain slot.  The
    // slot is consumed by Present, not by beginning CPU paint.  Retain it
    // across projection/paint failures so a bad frame cannot leave the UI
    // synchronously waiting one second for a signal that requires a Present
    // to be produced.
    renderer->begin_frame_permit = false;
    if (renderer->present_full) {
        ++renderer->full_present_draws;
    } else {
        ++renderer->partial_present_draws;
        const u64 width =
            static_cast<u64>(renderer->present_dirty_rect.right - renderer->present_dirty_rect.left);
        const u64 height =
            static_cast<u64>(renderer->present_dirty_rect.bottom - renderer->present_dirty_rect.top);
        const u64 pixels = width * height;
        saturating_accumulate(renderer->partial_present_pixels, pixels);
    }
    if (!renderer->present_logical_damage_valid) {
        // A present made solely to acknowledge an accepted BeginFrame copies
        // the previous buffer's outstanding damage forward.  Both buffers are
        // then synchronized, so no history remains to carry.
        renderer->previous_buffer_damage_valid = false;
        renderer->previous_buffer_damage_full = false;
        reset_value(&renderer->previous_buffer_damage_rect);
        ++renderer->idle_begin_frame_presents;
    } else {
        renderer->previous_buffer_damage_valid = true;
        renderer->previous_buffer_damage_full = renderer->present_logical_damage_full;
        renderer->previous_buffer_damage_rect = renderer->present_logical_damage_rect;
    }
    presentation_damage_commit(&renderer->presentation_damage);
    renderer_paint_diff_commit(renderer);
    d3d11_renderer_caption_presented(renderer);
    renderer->present_required = false;
    renderer->present_full = false;
    renderer->present_logical_damage_valid = false;
    renderer->present_logical_damage_full = false;
    renderer->has_presented = true;
    return true;
}

bool renderer_set_login_view(D3D11Renderer* renderer, const LoginScene* scene,
                             const RendererLoginView& view, const Theme& theme) noexcept {
    if (!renderer || !renderer->glyph_text.ready || !login_view_valid(view)) {
        return false;
    }
    if (!view.visible) {
        renderer_release_login_form_text(renderer);
        text_edit_layout_invalidate(&renderer->login_edit);
        renderer->login_scene = nullptr;
        renderer->login_visible = false;
        renderer->login_qr_ready = false;
        renderer->login_caret_visible = false;
        renderer->login_composition_metric_count = 0;
        renderer->login_status = RendererLoginStatus::idle;
        shaped_text_reset(&renderer->glyph_text.title);
        return d3d11_glyph_text_rebuild_title(&renderer->glyph_text, renderer->graphics.context,
                                              renderer->dpi, false);
    }
    renderer->login_scene = scene;
    renderer->login_qr_ready = view.qr_code != nullptr;
    if (view.qr_code) {
        renderer->login_qr_code = *view.qr_code;
    }
    renderer->login_status = view.status;
    renderer->login_visible = true;
    UiTextCommand commands[kMaximumLoginTextCommands];
    UiTextCommandStream stream;
    const bool text_ready =
        ui_text_command_stream_initialize(&stream, Slice<UiTextCommand>{commands, kMaximumLoginTextCommands}) &&
        login_text_presentation_build(*scene, view, theme, &stream) &&
        (d3d11_renderer_login_form_text(*renderer)
             ? d3d11_renderer_capture_login_form_text(renderer, stream) &&
                   renderer_rebuild_content_text(renderer, theme)
             : d3d11_renderer_rebuild_login_text(renderer, stream));
    return text_ready && d3d11_renderer_measure_login_edit(renderer, view);
}

bool renderer_prepare_login_form_text(D3D11Renderer* renderer) noexcept {
    if (!renderer || !renderer->login_visible || !renderer->glyph_text.ready)
        return false;
    if (renderer->login_form_text)
        return renderer->content_text == &renderer->login_form_text->text;
    if (renderer->content_text)
        return false;
    constexpr usize bytes =
        sizeof(RendererLoginFormTextStorage) + 512 + sizeof(GlyphQuad) * kRendererContentGlyphCapacity +
        sizeof(GlyphDrawBatch) * kRendererContentBatchCapacity +
        sizeof(TextGlyph) * kRendererLoginFormGlyphCapacity +
        sizeof(TextRun) * kRendererContentRunCapacity + sizeof(u16) * kRendererLoginFormUnitCapacity +
        sizeof(TextHitSpan) + sizeof(TextHitRegion) * 2 + sizeof(TextDecoration) +
        sizeof(MessageTextSemantic);
    Arena arena{};
    if (!arena_initialize(&arena, bytes, 0, 4 * 1024, ArenaTag::ui))
        return false;
    auto* owned = arena_allocate_zero<RendererLoginFormTextStorage>(&arena);
    if (!owned) {
        arena_release(&arena);
        return false;
    }
    auto& text = owned->text;
    text.quads = arena_allocate<GlyphQuad>(&arena, text.quad_capacity = kRendererContentGlyphCapacity);
    text.batches =
        arena_allocate<GlyphDrawBatch>(&arena, text.batch_capacity = kRendererContentBatchCapacity);
    text.glyphs = arena_allocate<TextGlyph>(&arena, text.glyph_capacity = kRendererLoginFormGlyphCapacity);
    text.runs = arena_allocate<TextRun>(&arena, text.run_capacity = kRendererContentRunCapacity);
    text.clusters = arena_allocate<u16>(&arena, text.cluster_capacity = kRendererLoginFormUnitCapacity);
    text.hit_spans = arena_allocate<TextHitSpan>(&arena, text.hit_span_capacity = 1);
    text.hit_region_scratch = arena_allocate<TextHitRegion>(&arena, text.hit_region_scratch_capacity = 1);
    text.decorations = arena_allocate<TextDecoration>(&arena, text.decoration_capacity = 1);
    text.content_semantics =
        arena_allocate<MessageTextSemantic>(&arena, text.content_semantic_capacity = 1);
    text.content_hit_regions = arena_allocate<TextHitRegion>(&arena, text.content_hit_region_capacity = 1);
    if (!renderer_bind_text_storage(renderer, &text)) {
        arena_release(&arena);
        return false;
    }
    owned->arena = arena;
    renderer->login_form_text = owned;
    renderer->retained_presentation.valid = false;
    return true;
}

void renderer_release_login_form_text(D3D11Renderer* renderer) noexcept {
    if (!renderer || !renderer->login_form_text)
        return;
    auto* owned = renderer->login_form_text;
    if (renderer->content_text == &owned->text)
        renderer_unbind_text_storage(renderer);
    if (renderer->login_visible && owned->valid && renderer->glyph_text.ready &&
        !d3d11_renderer_restore_login_form_base(renderer)) {
        renderer->glyph_text.title_quad_count = 0;
        log_write(LogLevel::error, "render", "login_form_base_restore_failed");
    }
    renderer->login_form_text = nullptr;
    renderer->retained_presentation.valid = false;
    Arena arena = owned->arena;
    // The cache contains displayed login identifiers and password masks only.
    // Erase those copies before releasing their bounded renderer allocation.
    SecureZeroMemory(owned->display, sizeof(owned->display));
    arena_release(&arena);
}

bool renderer_bind_text_storage(D3D11Renderer* renderer, RendererTextStorage* storage) noexcept {
    if (!renderer || !storage || !storage->quads || storage->quad_capacity == 0 || !storage->batches ||
        storage->batch_capacity == 0 || !storage->glyphs || storage->glyph_capacity == 0 || !storage->runs ||
        storage->run_capacity == 0 ||
        !storage->clusters || storage->cluster_capacity == 0 || !storage->hit_spans ||
        storage->hit_span_capacity == 0 || storage->hit_span_capacity > kTextHitSpanMaximum ||
        !storage->hit_region_scratch || storage->hit_region_scratch_capacity == 0 ||
        !storage->decorations || storage->decoration_capacity == 0 || !storage->content_semantics ||
        storage->content_semantic_capacity == 0 || storage->content_semantic_capacity > 0xFFFFu ||
        !storage->content_hit_regions || storage->content_hit_region_capacity == 0) {
        return false;
    }
    if (renderer->login_form_text && storage != &renderer->login_form_text->text) {
        if (renderer->text_form && renderer->text_form->scene && renderer->text_form->scene->active)
            return false;
        renderer_release_login_form_text(renderer);
    }
    if (renderer->content_text)
        return false;
    if (!shaped_text_initialize(&storage->scratch,
                                Slice<TextGlyph>{storage->glyphs, storage->glyph_capacity},
                                Slice<TextRun>{storage->runs, storage->run_capacity},
                                Slice<u16>{storage->clusters, storage->cluster_capacity},
                                Slice<TextDecoration>{storage->decorations, storage->decoration_capacity},
                                Slice<TextHitRegion>{storage->hit_region_scratch,
                                                     storage->hit_region_scratch_capacity})) {
        return false;
    }
    if (renderer->pipeline.glyph_instance_capacity < storage->quad_capacity &&
        !create_glyph_instance_buffer(renderer, storage->quad_capacity)) {
        shaped_text_reset(&storage->scratch);
        return false;
    }
    storage->quad_count = 0;
    storage->batch_count = 0;
    storage->content_semantic_count = 0;
    storage->content_hit_region_count = 0;
    storage->message_geometry_quads = 0;
    storage->last_error = RendererTextBuildError::none;
    storage->last_shape_error = TextShapeError::none;
    content_text_retention_reset(&renderer->content_text_retention);
    renderer->content_text_prefix.storage = nullptr;
    renderer->content_text_prefix.shell.storage = nullptr;
    renderer->content_text = storage;
    return true;
}

void renderer_unbind_text_storage(D3D11Renderer* renderer) noexcept {
    if (renderer)
        renderer->message_measure_cache = nullptr;
    if (!renderer || !renderer->content_text) {
        return;
    }
    const bool login_form =
        renderer->login_form_text && renderer->content_text == &renderer->login_form_text->text;
    shaped_text_reset(&renderer->content_text->scratch);
    renderer->content_text->quad_count = 0;
    renderer->content_text->batch_count = 0;
    renderer->content_text = nullptr;
    renderer->uploaded_text_storage = nullptr;
    renderer->uploaded_text_rebuild = 0;
    renderer->uploaded_text_quad_count = 0;
    content_text_retention_invalidate(&renderer->content_text_retention);
    renderer->content_text_prefix.storage = nullptr;
    renderer->content_text_prefix.shell.storage = nullptr;
    text_edit_layout_invalidate(&renderer->composer_layout);
    // The login-form owner restores all copied login commands after detach;
    // rebuilding only glyph_text.title would show only the last label.
    if (login_form)
        return;
    if (renderer->glyph_text.ready &&
        !d3d11_glyph_text_rebuild_title(&renderer->glyph_text, renderer->graphics.context, renderer->dpi,
                                        true)) {
        renderer->glyph_text.ready = false;
        log_write(LogLevel::error, "render", "gpu_text_unbind_rebuild_failed");
    }
}

bool renderer_text_measure_backend(void* context, const UiTextMeasureRequest& request,
                                   UiTextMeasureValue* value) noexcept {
    auto* renderer = static_cast<D3D11Renderer*>(context);
    RendererTextStorage* storage = renderer ? renderer->content_text : nullptr;
    if (!renderer || !storage || !value) {
        return false;
    }
    auto system = renderer_text_system(renderer, request.face);
    // Text with more UTF-16 units than the content storage shapes measures as
    // the overflow sign it draws.
    u32 units = 0;
    const bool oversized = request.text.bytes > storage->cluster_capacity &&
                           utf8_utf16_position_for_byte_offset(request.text, request.text.bytes, &units) &&
                           units > storage->cluster_capacity;
    const TextMeasureResult measured =
        text_measure_utf8(&system, {oversized ? utf8_literal("…") : request.text,
                                    oversized ? ConstSlice<TextStyleSpan>{} : request.styles, request.constraint.width,
                                    request.constraint.height,
                                    request.no_wrap ? TextWrapping::no_wrap : TextWrapping::wrap});
    if (!measured.ok()) {
        storage->last_shape_error = measured.error;
        LogEvent("measure_failed")
            .field("owner", request.key.source_id)
            .field("text_bytes", request.text.bytes)
            .field("style_count", request.styles.count)
            .field("utf16_units", measured.utf16_units)
            .field("shape_error", static_cast<u64>(measured.error))
            .write(LogLevel::error, "text");
        return false;
    }
    *value = UiTextMeasureValue{UiSize{measured.width_including_trailing_whitespace, measured.height},
                                measured.line_count};
    return true;
}

bool renderer_measure_message_composer(D3D11Renderer* renderer, float* preferred_height) noexcept {
    if (!renderer || !preferred_height || !renderer->message_composer_scene ||
        !d3d11_renderer_measure_composer_edit(renderer))
        return false;
    *preferred_height = message_composer_scene_measured_height(*renderer->message_composer_scene,
                                                               renderer->composer_layout.layout_height);
    return true;
}

bool renderer_rebuild_content_text(D3D11Renderer* renderer, const Theme& theme) noexcept {
    return d3d11_renderer_rebuild_text_mode(renderer, theme, true);
}

bool renderer_rebuild_shell_text(D3D11Renderer* renderer, const Theme& theme) noexcept {
    return d3d11_renderer_rebuild_text_mode(renderer, theme, false);
}

void renderer_set_dpi(D3D11Renderer* renderer, u32 dpi) noexcept {
    renderer->dpi = dpi == 0 ? 96 : dpi;
    if (renderer->pipeline.ui_frame_buffer) {
        update_frame_transform(renderer);
    }
    if (renderer->glyph_text.ready && !renderer->content_text && renderer->glyph_text.title.run_count) {
        const bool rebuilt = d3d11_glyph_text_rebuild_title(
            &renderer->glyph_text, renderer->graphics.context, renderer->dpi, true);
        if (!rebuilt) {
            renderer->glyph_text.ready = false;
            log_write(LogLevel::error, "render", "gpu_text_dpi_rebuild_failed");
        }
    }
}

void renderer_set_caption_height(D3D11Renderer* renderer, u32 height) noexcept {
    if (!renderer || renderer->caption_height == height)
        return;
    renderer->caption_height = height;
    renderer->pipeline.backdrop_caption = height;
    renderer_bind_target_origin(renderer, height);
    renderer->previous_buffer_damage_valid = true;
    renderer->previous_buffer_damage_full = true;
    renderer->retained_presentation.valid = false;
    presentation_damage_invalidate_full(&renderer->presentation_damage);
    d3d11_renderer_caption_invalidate(renderer);
}

void renderer_bind_target_origin(D3D11Renderer* renderer, u32 origin_y) noexcept {
    if (!renderer || renderer->target_origin_y == origin_y)
        return;
    renderer->target_origin_y = origin_y;
    if (renderer->pipeline.ui_frame_buffer && renderer->graphics.context)
        update_frame_transform(renderer);
}

void renderer_shutdown(D3D11Renderer* renderer) noexcept {
    release_prestarted_pipeline();
    d3d11_renderer_caption_release(renderer);
    renderer_release_login_form_text(renderer);
    if (renderer->inline_media.flags)
        pages_release(renderer->inline_media.flags);
    reset_value(&renderer->inline_media);
    if (renderer->graphics.context) {
        gpu_clear_state(renderer->graphics.context);
        gpu_flush(renderer->graphics.context);
    }
    d3d11_background_fx_release(&renderer->background_fx);
    d3d11_effect_release(&renderer->effect);
    paint_stream_release(&renderer->paint_stream);
    renderer_paint_diff_release(renderer);
    d3d11_renderer_shell_segment_release(&renderer->shell_segment);
    d3d11_renderer_layout_release(renderer);
    text_edit_layout_release(&renderer->composer_layout);
    gpu_release(renderer->text_effect_buffer);
    renderer_release_settings_backdrop(renderer);
    gpu_release(renderer->settings_backdrop_quad);
    for (auto& handle : renderer->native_text_faces)
        handle = 0;
    text_edit_layout_release(&renderer->popup_edit_layout);
    text_edit_layout_release(&renderer->profile_edit_layout);
    text_edit_layout_release(&renderer->search_layout);
    text_edit_layout_release(&renderer->media_picker_layout);
    renderer->media_picker_scene = nullptr;
    renderer->quick_switcher_scene = nullptr;
    renderer->inbox_scene = nullptr;
    renderer->message_forward_scene = nullptr;
    renderer->download_scene = nullptr;
    renderer->go_live_scene = nullptr;
    renderer->shortcuts_scene = nullptr;
    renderer->call_scene = nullptr;
    renderer->stream_card_scene = nullptr;
    renderer->names_card_scene = nullptr;
    renderer->guild_card_scene = nullptr;
    renderer->expression_card_scene = nullptr;
    renderer->guild_banner = nullptr;
    reset_value(&renderer->media_picker_edit_geometry);
    text_edit_layout_release(&renderer->login_edit);
    renderer->edit_selection_owner = nullptr;
    message_hit_test_cache_release(&renderer->message_hit_test);
    // Cached runs borrow the glyph text system's font session.
    shaped_text_cache_destroy(&renderer->shaped_text_cache);
    d3d11_glyph_text_release(&renderer->glyph_text);
    icon_atlas_release(&renderer->icon_atlas);
    renderer_release_typography(renderer);
    if (renderer->picker_glyph_cache)
        pages_release(renderer->picker_glyph_cache);
    renderer->picker_glyph_cache = nullptr;
    renderer_unbind_media_atlas(renderer);
    animation_frame_cache_gpu_release(&renderer->animation_gpu);
    animation_frame_cache_gpu_release(&renderer->preview_animation_gpu);
    renderer_media_draw_storage_release(&renderer->media_draw);
    if (renderer->content_text) {
        shaped_text_reset(&renderer->content_text->scratch);
        renderer->content_text = nullptr;
    }
    content_text_retention_reset(&renderer->content_text_retention);
    arena_release(&renderer->shell_text_storage);
    renderer->login_scene = nullptr;
    renderer->ui_scene = nullptr;
    d3d11_draw_pipeline_release(&renderer->pipeline);
    d3d11_device_owner_release(&renderer->graphics);
    renderer->width = 0;
    renderer->height = 0;
    renderer->background_phase_seconds = 0;
    renderer->background_next_frame_ms = 0;
    renderer->background_motion_running = false;
    renderer->draw_calls_last_frame = 0;
    renderer->login_caret_visible = false;
    renderer->uploaded_text_storage = nullptr;
    renderer->uploaded_text_rebuild = 0;
    renderer->uploaded_text_quad_count = 0;
    renderer->chat_state = nullptr;
    renderer->user_store = nullptr;
    renderer->navigation_view = nullptr;
    renderer->shell_navigation_scene = nullptr;
    renderer->message_scene = nullptr;
    renderer->message_action_bar = nullptr;
    renderer->message_composer_scene = nullptr;
    renderer->member_list_scene = nullptr;
    renderer->popup_scene = nullptr;
    renderer->theme_editor_scene = nullptr;
    renderer->interaction_text_theme_editor_revision = 0;
    renderer->text_form = nullptr;
    renderer->forum_form = nullptr;
    renderer->reaction_viewer = nullptr;
    renderer->message_text = nullptr;
    renderer->audio_scene = nullptr;
    renderer->interaction_text_audio_revision = 0;
    renderer->poll_scene = nullptr;
    renderer->poll_voters_scene = nullptr;
    renderer->interaction_text_poll_revision = 0;
    renderer->interaction_text_poll_voters_revision = 0;
    renderer->message_measure_cache = nullptr;
    renderer->has_presented = false;
    renderer->live_resize = false;
    renderer->paint_overflow_reported = false;
}

bool renderer_device_lost(D3D11Renderer* renderer) noexcept {
    if (!renderer)
        return false;
    note_device_failure(renderer);
    return renderer->device_lost;
}

// Everything that lived on the lost device is released, then rebuilt on a new one in the order renderer_initialize
// builds it. Nothing the host bound (scenes, text storage, atlases' CPU side) is touched; the GPU contents that
// retained state describes are gone, so the retained frames, uploads and atlas residency are invalidated and the
// host rebuilds text and requests media again. Cold: runs once per reset.
bool renderer_recover_device(D3D11Renderer* renderer) noexcept {
    // A renderer that never finished initialization has nothing to rebuild.
    if (!renderer || !renderer->glyph_text.atlas_origins)
        return false;
    log_write(LogLevel::warning, "render", "device_recovery_begin");
    d3d11_effect_release_device(&renderer->effect);
    d3d11_background_fx_release(&renderer->background_fx);
    d3d11_renderer_caption_release_device(renderer);
    renderer_release_settings_backdrop(renderer);
    gpu_release(renderer->settings_backdrop_quad);
    gpu_release(renderer->text_effect_buffer);
    renderer->text_effect_uploaded_count = 0;
    renderer_release_media_surfaces(renderer);
    icon_atlas_release(&renderer->icon_atlas);
    d3d11_draw_pipeline_release(&renderer->pipeline);
    if (renderer->graphics.context) {
        gpu_clear_state(renderer->graphics.context);
        gpu_flush(renderer->graphics.context);
    }
    d3d11_device_owner_release(&renderer->graphics);

    const GpuSurfaceDesc surface{renderer->window, renderer->width, renderer->height + renderer->caption_height,
                                 renderer->dpi == 0 ? 96 : renderer->dpi};
    const D3D11DeviceOwnerResult graphics = d3d11_device_owner_initialize(&renderer->graphics, surface);
    if (!graphics.ok()) {
        log_u64(LogLevel::error, "render", "device_recovery_device_error", static_cast<u32>(graphics.native_error));
        renderer->device_lost = true;
        return false;
    }
    renderer->width = surface.width;
    renderer->height = surface.height > renderer->caption_height ? surface.height - renderer->caption_height : 1;
    renderer->target_origin_y = renderer->caption_height;
    renderer->dpi = surface.dpi;
    renderer->adapter_key = graphics.adapter_key;
    if (!icon_atlas_initialize(&renderer->icon_atlas, renderer->graphics.device))
        log_write(LogLevel::error, "render", "icon_atlas_creation_failed");
    bool ok = d3d11_glyph_text_recreate_gpu(&renderer->glyph_text, renderer->graphics.device);
    ok = ok && initialize_pipeline(renderer).ok();
    // The glyph ring is as large as the bound text needs.
    const u32 glyph_capacity = renderer->content_text ? renderer->content_text->quad_capacity : 0;
    ok = ok && (!glyph_capacity || renderer->pipeline.glyph_instance_capacity >= glyph_capacity ||
                create_glyph_instance_buffer(renderer, glyph_capacity));
    if (!ok) {
        log_write(LogLevel::error, "render", "device_recovery_resources_failed");
        d3d11_device_owner_release(&renderer->graphics);
        renderer->device_lost = true;
        return false;
    }
    // The new device holds none of what the retained state believes it uploaded.
    renderer->uploaded_text_storage = nullptr;
    renderer->uploaded_text_rebuild = 0;
    renderer->uploaded_text_quad_count = 0;
    renderer->uploaded_text_prefix = 0;
    renderer->text_stable_quads = 0;
    ++renderer->text_ring_epoch;
    renderer->retained_presentation.valid = false;
    renderer->paint_diff.valid = renderer->paint_diff.pending = false;
    renderer->shell_segment.valid = false;
    renderer->typing_dots.upload_pending = false;
    renderer->typing_dots.gpu_first = renderer->typing_dots.ring_end = 0;
    renderer->settings_backdrop_valid = false;
    reset_value(&renderer->settings_backdrop_stamp);
    renderer->previous_buffer_damage_valid = renderer->previous_buffer_damage_full = true;
    renderer->present_required = renderer->present_full = false;
    renderer->begin_frame_permit = false;
    renderer->has_presented = false;
    presentation_damage_invalidate_full(&renderer->presentation_damage);
    d3d11_renderer_caption_invalidate(renderer);
    renderer->device_lost = false;
    ++renderer->device_generation;
    log_u64(LogLevel::warning, "render", "device_recovery_done", renderer->device_generation);
    return true;
}

} // namespace maicord
