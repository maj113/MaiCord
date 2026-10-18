#include "maicord/render/d3d11_renderer.hpp"

#include "maicord/core/log.hpp"
#include "maicord/render/renderer_presentation.hpp"
#include "d3d11_renderer_caption.hpp"
#include "d3d11_renderer_edit.hpp"
#include "d3d11_renderer_layout.hpp"
#include "d3d11_renderer_login_form.hpp"
#include "shaped_text_cache.hpp"

#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {

bool update_frame_transform(D3D11Renderer* renderer) noexcept {
    renderer->pipeline.backdrop_caption = renderer->caption_height;
    return d3d11_draw_pipeline_update_transform(renderer->pipeline, renderer->graphics.context, renderer->width,
                                                renderer->height, renderer->dpi, renderer->target_origin_y);
}

bool create_glyph_instance_buffer(D3D11Renderer* renderer, u32 capacity) noexcept {
    const D3D11DrawPipelineResult reserved = d3d11_draw_pipeline_reserve_glyph_instances(
        &renderer->pipeline, renderer->graphics.device, sizeof(GlyphQuad), capacity);
    if (!reserved.ok()) {
        log_write(LogLevel::error, "render", "glyph_instance_buffer_growth_failed");
        log_u64(LogLevel::error, "render", "glyph_instance_native_error",
                static_cast<u32>(reserved.native_error));
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
        log_write(LogLevel::error, "render", "gpu_text_resources_creation_failed");
        log_u64(LogLevel::error, "render", "gpu_text_initialize_stage",
                static_cast<u64>(initialized.error));
        log_u64(LogLevel::error, "render", "gpu_text_initialize_native_error",
                static_cast<u32>(initialized.native_error));
        return false;
    }
    if (!initialized.color_glyphs) {
        log_write(LogLevel::warning, "render", "color_glyphs_unavailable");
    }
    return true;
}

constexpr D3D11DrawPipelineConfig kPipelineConfig{sizeof(PaintQuad), kD3D11InitialUiInstanceCapacity,
                                                sizeof(GlyphQuad), kRendererGlyphCapacity,
                                                sizeof(MediaQuad), kRendererMediaQuadCapacity};
static_assert(kMaxPaintQuads == kD3D11MaximumUiInstanceCapacity);

// The glyph atlas and draw-pipeline objects built by the device prestart
// worker while the main thread creates the swap chain, composition tree and
// icon atlas. The atlas comes first: the main thread's first frame needs it,
// and its texture and buffer contend less with the swap chain than shader
// creation does. The device pointers identify which device they belong to;
// they are not owned.
D3D11DrawPipelineOwner g_prestarted_pipeline{};
ID3D11Device* g_prestarted_pipeline_device = nullptr;
GlyphAtlasGpu g_prestarted_glyph_atlas{};
ID3D11Device* g_prestarted_glyph_atlas_device = nullptr;

void prestart_pipeline(ID3D11Device* device, void*) noexcept {
    if (glyph_atlas_gpu_initialize(&g_prestarted_glyph_atlas, device, kRendererGlyphAtlasExtent,
                                   kRendererGlyphAtlasExtent, kRendererPathCapacity)
            .ok())
        g_prestarted_glyph_atlas_device = device;
    if (d3d11_draw_pipeline_create(&g_prestarted_pipeline, device, kPipelineConfig).ok())
        g_prestarted_pipeline_device = device;
}

void release_prestarted_pipeline() noexcept {
    d3d11_device_prestart_finish();
    d3d11_draw_pipeline_release(&g_prestarted_pipeline);
    g_prestarted_pipeline_device = nullptr;
    glyph_atlas_gpu_release(&g_prestarted_glyph_atlas);
    g_prestarted_glyph_atlas_device = nullptr;
}

// Joins the prestart worker and lends its glyph atlas to the text resources
// when it belongs to this device; null otherwise.
GlyphAtlasGpu* prestarted_glyph_atlas(const D3D11Renderer& renderer) noexcept {
    d3d11_device_prestart_finish();
    return g_prestarted_glyph_atlas_device && g_prestarted_glyph_atlas_device == renderer.graphics.device
               ? &g_prestarted_glyph_atlas
               : nullptr;
}

D3D11DrawPipelineResult initialize_pipeline(D3D11Renderer* renderer) noexcept {
    d3d11_device_prestart_finish();
    // An atlas the text resources did not take (another device, or their
    // initialization failed first) is released with the worker's leftovers.
    glyph_atlas_gpu_release(&g_prestarted_glyph_atlas);
    g_prestarted_glyph_atlas_device = nullptr;
    D3D11DrawPipelineOwner staged = g_prestarted_pipeline;
    const bool prestarted = g_prestarted_pipeline_device && g_prestarted_pipeline_device == renderer->graphics.device;
    g_prestarted_pipeline = {};
    g_prestarted_pipeline_device = nullptr;
    if (prestarted) {
        renderer->startup_prestart |= 2;
        return d3d11_draw_pipeline_publish(&renderer->pipeline, &staged, renderer->graphics.context, renderer->width,
                                           renderer->height, renderer->dpi);
    }
    d3d11_draw_pipeline_release(&staged);
    return d3d11_draw_pipeline_initialize(&renderer->pipeline, renderer->graphics.device, renderer->graphics.context,
                                          renderer->width, renderer->height, renderer->dpi, kPipelineConfig);
}

// Drivers finish a graphics pipeline (shaders with input layout, blend,
// rasterizer and target format) on its first draw. Drawing each of the three
// once into a tiny offscreen target, then flushing, moves that work off the
// first presented frame onto the driver while the client waits for its data.
void warm_draw_pipelines(D3D11Renderer* renderer) noexcept {
    auto& pipeline = renderer->pipeline;
    ID3D11DeviceContext* context = renderer->graphics.context;
    if (!context || !pipeline.ui_instance_buffer || !pipeline.glyph_instance_buffer ||
        !pipeline.media_instance_buffer)
        return;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 4;
    description.Height = 4;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    if (FAILED(renderer->graphics.device->CreateTexture2D(&description, nullptr, &texture)) ||
        FAILED(renderer->graphics.device->CreateRenderTargetView(texture, nullptr, &target))) {
        com_release(texture);
        return;
    }
    const auto zero_instance = [&](ID3D11Buffer* buffer, u32 bytes) noexcept {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
            return false;
        clear_bytes(mapped.pData, bytes);
        context->Unmap(buffer, 0);
        return true;
    };
    const D3D11_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
    const D3D11_RECT scissor{0, 0, 4, 4};
    const float blend_factor[4]{};
    const UINT offset = 0;
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->RSSetState(pipeline.ui_rasterizer_state);
    context->RSSetScissorRects(1, &scissor);
    context->OMSetBlendState(pipeline.ui_blend_state, blend_factor, 0xFFFFFFFFu);
    context->VSSetConstantBuffers(0, 1, &pipeline.ui_frame_buffer);
    context->PSSetConstantBuffers(0, 1, &pipeline.ui_frame_buffer);
    if (zero_instance(pipeline.ui_instance_buffer, sizeof(PaintQuad))) {
        const UINT stride = sizeof(PaintQuad);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(pipeline.ui_input_layout);
        context->IASetVertexBuffers(0, 1, &pipeline.ui_instance_buffer, &stride, &offset);
        context->VSSetShader(pipeline.ui_vertex_shader, nullptr, 0);
        context->PSSetShader(pipeline.ui_pixel_shader, nullptr, 0);
        context->DrawInstanced(6, 1, 0, 0);
    }
    // The glyph ring restarts with a discard on the first real upload.
    if (zero_instance(pipeline.glyph_instance_buffer, sizeof(GlyphQuad))) {
        const UINT stride = sizeof(GlyphQuad);
        pipeline.glyph_instance_end = 0;
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->IASetInputLayout(pipeline.glyph_input_layout);
        context->IASetVertexBuffers(0, 1, &pipeline.glyph_instance_buffer, &stride, &offset);
        context->VSSetShader(pipeline.glyph_vertex_shader, nullptr, 0);
        context->PSSetShader(pipeline.glyph_pixel_shader, nullptr, 0);
        context->PSSetSamplers(0, 1, &pipeline.glyph_sampler);
        context->DrawInstanced(4, 1, 0, 0);
    }
    if (zero_instance(pipeline.media_instance_buffer, sizeof(MediaQuad))) {
        const UINT stride = sizeof(MediaQuad);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(pipeline.media_input_layout);
        context->IASetVertexBuffers(0, 1, &pipeline.media_instance_buffer, &stride, &offset);
        context->VSSetShader(pipeline.media_vertex_shader, nullptr, 0);
        context->PSSetShader(pipeline.media_pixel_shader, nullptr, 0);
        context->DrawInstanced(6, 1, 0, 0);
    }
    ID3D11SamplerState* null_sampler = nullptr;
    context->PSSetSamplers(0, 1, &null_sampler);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    // Frames set the viewport only with the transform (startup and resize).
    update_frame_transform(renderer);
    context->Flush();
    com_release(target);
    com_release(texture);
}

} // namespace

void renderer_prestart() noexcept {
    d3d11_device_prestart(prestart_pipeline, nullptr);
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

HRESULT renderer_select_plugin_effect(D3D11Renderer* renderer, u64 owner_id, u64 effect_id, u64 revision,
                                      ConstSlice<u8> bytecode, const float* parameters) noexcept {
    if (!renderer || !renderer->graphics.device)
        return E_INVALIDARG;
    bool changed = false;
    const HRESULT result =
        d3d11_background_fx_select_plugin(&renderer->background_fx, renderer->graphics.device, owner_id, effect_id,
                                          revision, bytecode, parameters, renderer->background_phase_seconds,
                                          &changed);
    if (changed) {
        presentation_damage_invalidate_full(&renderer->presentation_damage);
        renderer->settings_backdrop_stamp = {};
    }
    return result;
}

void renderer_clear_plugin_effect(D3D11Renderer* renderer) noexcept {
    if (renderer && d3d11_background_fx_clear_plugin(&renderer->background_fx)) {
        presentation_damage_invalidate_full(&renderer->presentation_damage);
        renderer->settings_backdrop_stamp = {};
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

bool renderer_initialize(D3D11Renderer* renderer, HWND window) noexcept {
    if (!renderer || !window || !renderer_media_draw_storage_initialize(&renderer->media_draw)) {
        return false;
    }
    // Identity animation frames (<= 128 px): start at 256 rows (2 MiB) and
    // grow by replayed working set up to 2048 rows.
    if (!animation_frame_cache_gpu_initialize(&renderer->animation_gpu) ||
        !animation_frame_cache_gpu_configure_growth(&renderer->animation_gpu, kAnimationFrameCacheGpuGrowthStep)) {
        animation_frame_cache_gpu_release(&renderer->animation_gpu);
        renderer_media_draw_storage_release(&renderer->media_draw);
        return false;
    }
    renderer->layer_opacity = {1, 1, 1, 1, 1, 1, 1};
    renderer->settings_modal_progress = 1;
    renderer->draw_layer_scale = 1;
    renderer->popup_layer_scale = 1;
    presentation_damage_reset(&renderer->presentation_damage);
    renderer->present_dirty_rect = {};
    renderer->previous_buffer_damage_rect = {};
    renderer->present_logical_damage_rect = {};
    renderer->present_required = false;
    renderer->present_full = false;
    renderer->previous_buffer_damage_valid = false;
    renderer->previous_buffer_damage_full = false;
    renderer->present_logical_damage_valid = false;
    renderer->present_logical_damage_full = false;
    renderer->begin_frame_permit = false;
    LARGE_INTEGER performance_frequency{};
    QueryPerformanceFrequency(&performance_frequency);
    renderer->performance_frequency = performance_frequency.QuadPart;
    wchar_t gpu_metrics[2]{};
    const bool gpu_timestamps_enabled =
        GetEnvironmentVariableW(L"MAICORD_GPU_METRICS", gpu_metrics,
                                static_cast<DWORD>(sizeof(gpu_metrics) / sizeof(gpu_metrics[0]))) == 1 &&
        gpu_metrics[0] == L'1';

    const D3D11DeviceOwnerResult graphics = d3d11_device_owner_initialize(&renderer->graphics, window);
    if (!graphics.ok()) {
        switch (graphics.error) {
        case D3D11DeviceOwnerError::hardware_device:
            log_write(LogLevel::error, "render", "hardware_d3d11_device_unavailable");
            break;
        case D3D11DeviceOwnerError::context_11_1:
            log_write(LogLevel::error, "render", "d3d11_1_context_unavailable");
            break;
        case D3D11DeviceOwnerError::dxgi_factory:
            log_write(LogLevel::error, "render", "dxgi_factory_unavailable");
            break;
        case D3D11DeviceOwnerError::composition_swap_chain:
            log_write(LogLevel::error, "render", "direct_composition_swap_chain_creation_failed");
            break;
        case D3D11DeviceOwnerError::frame_latency:
            log_write(LogLevel::error, "render", "dxgi_1_3_resize_surface_unavailable");
            break;
        case D3D11DeviceOwnerError::render_target:
            log_write(LogLevel::error, "render", "create_render_target_failed");
            break;
        default:
            log_write(LogLevel::error, "render", "d3d11_device_owner_initialization_failed");
            break;
        }
        log_u64(LogLevel::error, "render", "d3d11_device_native_error",
                static_cast<u32>(graphics.native_error));
        renderer_shutdown(renderer);
        return false;
    }
    renderer->width = graphics.width;
    renderer->height =
        graphics.height > renderer->caption_height ? graphics.height - renderer->caption_height : 1;
    renderer->target_origin_y = renderer->caption_height;
    renderer->dpi = graphics.dpi;
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
    const D3D11GpuTimerResult gpu_timer =
        d3d11_gpu_timer_initialize(&renderer->gpu_timer, renderer->graphics.device, gpu_timestamps_enabled);
    if (!gpu_timer.ok()) {
        log_write(LogLevel::error, "render", "gpu_timestamp_query_creation_failed");
        log_u64(LogLevel::error, "render", "gpu_timestamp_query_stage", static_cast<u32>(gpu_timer.error));
        log_u64(LogLevel::error, "render", "gpu_timestamp_native_error",
                static_cast<u32>(gpu_timer.native_error));
        renderer_shutdown(renderer);
        return false;
    }
    // Last: the prestart worker builds these objects meanwhile when it ran.
    const D3D11DrawPipelineResult pipeline = initialize_pipeline(renderer);
    if (!pipeline.ok()) {
        log_write(LogLevel::error, "render", "d3d11_draw_pipeline_initialization_failed");
        log_u64(LogLevel::error, "render", "d3d11_draw_pipeline_stage", static_cast<u32>(pipeline.error));
        log_u64(LogLevel::error, "render", "d3d11_draw_pipeline_native_error",
                static_cast<u32>(pipeline.native_error));
        renderer_shutdown(renderer);
        return false;
    }

    warm_draw_pipelines(renderer);
    log_u64(LogLevel::info, "render", "startup_prestart", renderer->startup_prestart);
    log_write(LogLevel::info, "render", "hardware_d3d11_instanced_ui_ready");
    return true;
}

bool renderer_resize(D3D11Renderer* renderer, u32 width, u32 height) noexcept {
    if (!renderer || !renderer->graphics.swap_chain || !renderer->graphics.swap_chain2 || width == 0 ||
        height == 0) {
        return true;
    }
    const u32 target_height = height + renderer->caption_height;
    if (renderer->width == width && renderer->height == height && renderer->graphics.target &&
        (renderer->live_resize ||
         (renderer->graphics.buffer_width == width && renderer->graphics.buffer_height == target_height))) {
        return true;
    }
    // Reserve a small growth margin only inside Windows' sizing modal loop.
    // Reallocating a composition flip chain for every pointer pixel piles up
    // work for the compositor while the normal display-clock loop is paused.
    // A client that still fits the current buffers is clipped, not reallocated;
    // finishing the resize trims the buffers to the final client size.
    u32 buffer_width = width, buffer_height = target_height;
    if (renderer->live_resize) {
        buffer_width = renderer->graphics.buffer_width;
        buffer_height = renderer->graphics.buffer_height;
        if (width > buffer_width || target_height > buffer_height) {
            buffer_width = maximum((width + 63u) & ~63u, buffer_width);
            buffer_height = maximum((target_height + 63u) & ~63u, buffer_height);
        }
    }
    if (buffer_width != renderer->graphics.buffer_width ||
        buffer_height != renderer->graphics.buffer_height) {
        const D3D11DeviceOwnerResult resized =
            d3d11_device_owner_resize(&renderer->graphics, buffer_width, buffer_height);
        if (!resized.ok()) {
            log_write(LogLevel::error, "render",
                      resized.error == D3D11DeviceOwnerError::render_target ? "create_render_target_failed"
                                                                            : "swap_chain_resize_failed");
            log_u64(LogLevel::error, "render", "d3d11_resize_native_error",
                    static_cast<u32>(resized.native_error));
            return false;
        }
        // ResizeBuffers replaces both images, but not the swap chain's frame
        // latency semaphore. Keep an already acquired slot until Present uses it.
        renderer->previous_buffer_damage_valid = false;
        renderer->previous_buffer_damage_full = false;
        renderer->previous_buffer_damage_rect = {};
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
    if (renderer && renderer->graphics.frame_latency_waitable && !renderer->live_resize) {
        renderer->begin_frame_permit = true;
    }
}

bool renderer_present(D3D11Renderer* renderer, const Theme& theme) noexcept {
    if (!renderer->graphics.target || !renderer->graphics.swap_chain) {
        return false;
    }
    if (renderer->has_presented)
        renderer_note_text_effect_damage(renderer, GetTickCount64());
    const RendererPresentationStamp current = renderer_capture_presentation_stamp(*renderer, theme);
    if (renderer->has_presented &&
        (!renderer->login_visible || d3d11_renderer_login_form_text(*renderer)) &&
        !renderer->begin_frame_permit && !renderer_settings_backdrop_stale(renderer, theme) &&
        renderer_presentation_stamp_equal(current, renderer->retained_presentation) &&
        presentation_damage_empty(renderer->presentation_damage) &&
        !d3d11_renderer_caption_pending(renderer, theme)) {
        renderer->draw_calls_last_frame = 0;
        renderer->media_draws_last_frame = 0;
        ++renderer->unchanged_present_skips;
        return true;
    }
    // Modal resize presents must consume a slot too. Skipping these waits
    // accumulates semaphore credits, allowing normal frames to queue far ahead
    // of the compositor after the drag ends.
    if (renderer->graphics.frame_latency_waitable && !renderer->begin_frame_permit) {
        const DWORD waited = d3d11_device_owner_wait(renderer->graphics, 1'000u);
        if (waited != WAIT_OBJECT_0) {
            log_write(LogLevel::error, "render", "frame_latency_wait_failed");
            return false;
        }
        renderer->begin_frame_permit = true;
    }
    LARGE_INTEGER paint_begin{};
    LARGE_INTEGER paint_end{};
    QueryPerformanceCounter(&paint_begin);
    if (!d3d11_renderer_paint_layout(renderer, theme, current)) {
        return false;
    }
    QueryPerformanceCounter(&paint_end);
    u64 paint_us = 0;
    if (renderer->performance_frequency > 0 && paint_end.QuadPart > paint_begin.QuadPart) {
        paint_us = (static_cast<u64>(paint_end.QuadPart - paint_begin.QuadPart) * 1'000'000ull) /
                   static_cast<u64>(renderer->performance_frequency);
    }
    renderer->paint_cpu_us_total += paint_us;
    renderer->paint_cpu_us_max = maximum(renderer->paint_cpu_us_max, paint_us);
    if (renderer->painted_frames == 0) {
        renderer->first_frame_cpu_us = paint_us;
    }
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
    const HRESULT result = d3d11_device_owner_present(&renderer->graphics, sync_interval,
                                                      renderer->present_full ? nullptr : dirty,
                                                      dirty_count);
    if (FAILED(result)) {
        log_write(LogLevel::error, "render", "present_failed_or_device_removed");
        return false;
    }
    // A frame-latency permit represents one available swap-chain slot.  The
    // slot is consumed by Present, not by beginning CPU paint.  Retain it
    // across projection/paint failures so a bad frame cannot leave the UI
    // synchronously waiting one second for a signal that requires a Present
    // to be produced.
    renderer->begin_frame_permit = false;
    d3d11_gpu_timer_collect(&renderer->gpu_timer, renderer->graphics.context);
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
        renderer->previous_buffer_damage_rect = {};
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
    if (!renderer || !renderer->glyph_text.ready || !login_view_matches_scene(view, scene)) {
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
    const bool text_ready = d3d11_renderer_login_form_text(*renderer)
                                ? d3d11_renderer_capture_login_form_text(renderer, view, theme) &&
                                      renderer_rebuild_content_text(renderer, theme)
                                : d3d11_renderer_rebuild_login_text(renderer, view, theme);
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
        sizeof(wchar_t) * kRendererLoginFormUnitCapacity +
        sizeof(TextGlyph) * kRendererLoginFormGlyphCapacity +
        sizeof(TextRun) * kRendererContentRunCapacity + sizeof(u16) * kRendererLoginFormUnitCapacity +
        sizeof(TextHitSpan) + sizeof(TextHitRegion) * 2 + sizeof(TextDecoration) +
        sizeof(MessageTextSemantic);
    Arena arena{};
    if (!arena_initialize(&arena, bytes, 0, 4 * 1024))
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
    text.utf16 = arena_allocate<wchar_t>(&arena, text.utf16_capacity = kRendererLoginFormUnitCapacity);
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

RendererLoginFormTextMetrics renderer_login_form_text_metrics(const D3D11Renderer& renderer) noexcept {
    const auto* owned = renderer.login_form_text;
    return owned
               ? RendererLoginFormTextMetrics{sizeof(*owned), owned->arena.reserved, owned->arena.committed,
                                              owned->projection.count, owned->projection.owned_bytes}
               : RendererLoginFormTextMetrics{};
}

bool renderer_bind_text_storage(D3D11Renderer* renderer, RendererTextStorage* storage) noexcept {
    if (!renderer || !storage || !storage->quads || storage->quad_capacity == 0 || !storage->batches ||
        storage->batch_capacity == 0 || !storage->utf16 || storage->utf16_capacity == 0 ||
        !storage->glyphs || storage->glyph_capacity == 0 || !storage->runs || storage->run_capacity == 0 ||
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
                                Slice<u16>{storage->clusters, storage->cluster_capacity})) {
        return false;
    }
    if (!shaped_text_bind_decorations(
            &storage->scratch, Slice<TextDecoration>{storage->decorations, storage->decoration_capacity})) {
        shaped_text_reset(&storage->scratch);
        return false;
    }
    if (!shaped_text_bind_hit_regions(
            &storage->scratch,
            Slice<TextHitRegion>{storage->hit_region_scratch, storage->hit_region_scratch_capacity})) {
        shaped_text_reset(&storage->scratch);
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
    storage->native_error = S_OK;
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
    if (!renderer || !storage || !storage->utf16 || !value) {
        return false;
    }
    auto system = renderer_text_system(renderer, request.face);
    TextMeasureResult measured = text_measure_utf8_styled(
        &system, request.text, request.styles, request.constraint.width, request.constraint.height,
        Slice<wchar_t>{storage->utf16, storage->utf16_capacity},
        request.no_wrap ? TextWrapping::no_wrap : TextWrapping::wrap);
    if (measured.error == TextShapeError::output_too_small) {
        measured = text_measure_utf8_styled(&system, utf8_literal("…"),
                                            ConstSlice<TextStyleSpan>{}, request.constraint.width,
                                            request.constraint.height,
                                            Slice<wchar_t>{storage->utf16, storage->utf16_capacity},
                                            request.no_wrap ? TextWrapping::no_wrap : TextWrapping::wrap);
    }
    if (!measured.ok()) {
        storage->last_shape_error = measured.error;
        storage->native_error = measured.native_error;
        log_u64(LogLevel::error, "text", "measure_owner", request.key.source_id);
        log_u64(LogLevel::error, "text", "measure_text_bytes", request.text.bytes);
        log_u64(LogLevel::error, "text", "measure_style_count", request.styles.count);
        log_u64(LogLevel::error, "text", "measure_utf16_units", measured.utf16_units);
        log_u64(LogLevel::error, "text", "measure_shape_error", static_cast<u32>(measured.error));
        return false;
    }
    *value = UiTextMeasureValue{UiSize{measured.width_including_trailing_whitespace, measured.height}, 0.0f,
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
        VirtualFree(renderer->inline_media.flags, 0, MEM_RELEASE);
    renderer->inline_media = {};
    if (renderer->graphics.context) {
        renderer->graphics.context->ClearState();
        renderer->graphics.context->Flush();
    }
    d3d11_gpu_timer_release(&renderer->gpu_timer, renderer->graphics.context);
    d3d11_background_fx_release(&renderer->background_fx);
    paint_stream_release(&renderer->paint_stream);
    renderer_paint_diff_release(renderer);
    d3d11_renderer_shell_segment_release(&renderer->shell_segment);
    text_edit_layout_release(&renderer->composer_layout);
    if (renderer->text_effect_buffer) {
        renderer->text_effect_buffer->Release();
        renderer->text_effect_buffer = nullptr;
    }
    renderer_release_settings_backdrop(renderer);
    if (renderer->settings_backdrop_quad) {
        renderer->settings_backdrop_quad->Release();
        renderer->settings_backdrop_quad = nullptr;
    }
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
    renderer->shortcuts_scene = nullptr;
    renderer->call_scene = nullptr;
    renderer->stream_card_scene = nullptr;
    renderer->guild_card_scene = nullptr;
    renderer->guild_banner = nullptr;
    renderer->media_picker_edit_geometry = {};
    text_edit_layout_release(&renderer->login_edit);
    renderer->edit_selection_owner = nullptr;
    message_hit_test_cache_release(&renderer->message_hit_test);
    // Cached runs borrow the glyph text system's font session.
    shaped_text_cache_destroy(&renderer->shaped_text_cache);
    d3d11_glyph_text_release(&renderer->glyph_text);
    icon_atlas_release(&renderer->icon_atlas);
    renderer_release_typography(renderer);
    if (renderer->picker_glyph_cache)
        VirtualFree(renderer->picker_glyph_cache, 0, MEM_RELEASE);
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
    renderer->draw_calls_max = 0;
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

} // namespace maicord
