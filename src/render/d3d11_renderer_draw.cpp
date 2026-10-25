#include "d3d11_renderer_draw.hpp"
#include "maicord/core/memory.hpp"
#include "d3d11_renderer_interaction_state.hpp"
#include "d3d11_renderer_layout.hpp"
#include "d3d11_renderer_caption.hpp"
#include "maicord/render/renderer_presentation.hpp"

#include "maicord/core/log.hpp"
#include "maicord/render/draw_submission.hpp"
#include "maicord/render/raster_clip.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/icon.hpp"

#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {

struct LayerBatchRange {
    u32 first, end;
    float opacity;
    // Optional uniform scale about a DIP origin; nested opacities multiply.
    float scale = 1, origin_x = 0, origin_y = 0;
};

// Binds one layer transform unless it is already bound.
bool bind_layer(D3D11Renderer* renderer, float& current_opacity, float opacity, float scale, float origin_x,
                float origin_y) noexcept {
    if (opacity == current_opacity && scale == renderer->draw_layer_scale &&
        origin_x == renderer->draw_layer_origin_x && origin_y == renderer->draw_layer_origin_y)
        return true;
    if (!d3d11_draw_pipeline_update_layer(renderer->pipeline, renderer->graphics.context, renderer->width,
                                          renderer->height, renderer->dpi, opacity, scale, origin_x,
                                          origin_y))
        return false;
    current_opacity = opacity;
    renderer->background_fx.layer_opacity = opacity;
    renderer->draw_layer_scale = scale;
    renderer->draw_layer_origin_x = origin_x;
    renderer->draw_layer_origin_y = origin_y;
    return true;
}
// Split only at a moving surface's existing batch boundaries. Geometry and
// instance buffers remain retained; one existing frame constant serves paint,
// glyphs and media. Identity layers use the original draw ranges directly.
template <usize Count, class Draw>
bool draw_opacity_batches(D3D11Renderer* renderer, u32 first, u32 end,
                          const LayerBatchRange (&layers)[Count], float& current_opacity,
                          Draw draw) noexcept {
    if (first == end)
        return draw(first, end);
    while (first < end) {
        u32 next = end;
        float opacity = 1, scale = 1, origin_x = 0, origin_y = 0;
        for (const auto& layer : layers) {
            const float value = layer.opacity >= 0 && layer.opacity <= 1 ? layer.opacity : 1;
            const float factor = layer.scale > 0 && layer.scale <= 2 ? layer.scale : 1;
            if ((value == 1 && factor == 1) || layer.first >= layer.end)
                continue;
            if (first >= layer.first && first < layer.end) {
                opacity *= value;
                if (factor != 1)
                    scale = factor, origin_x = layer.origin_x, origin_y = layer.origin_y;
                next = minimum(next, layer.end);
            } else if (first < layer.first) {
                next = minimum(next, layer.first);
            }
        }
        if (!bind_layer(renderer, current_opacity, opacity, scale, origin_x, origin_y) ||
            !draw(first, next))
            return false;
        first = next;
    }
    return true;
}

// Scissors of a scaled layer follow its geometry.
UiRect layer_clip(const D3D11Renderer& renderer, UiRect rect) noexcept {
    const float s = renderer.draw_layer_scale;
    if (s == 1)
        return rect;
    const float x = renderer.draw_layer_origin_x, y = renderer.draw_layer_origin_y;
    return {x + (rect.left - x) * s, y + (rect.top - y) * s, x + (rect.right - x) * s, y + (rect.bottom - y) * s};
}

bool intersect_scissor(D3D11_RECT* rectangle, const D3D11_RECT* damage) noexcept {
    if (damage) {
        rectangle->left = maximum(rectangle->left, damage->left);
        rectangle->top = maximum(rectangle->top, damage->top);
        rectangle->right = minimum(rectangle->right, damage->right);
        rectangle->bottom = minimum(rectangle->bottom, damage->bottom);
    }
    return rectangle->left < rectangle->right && rectangle->top < rectangle->bottom;
}

const UiNode* scene_node(const D3D11Renderer* renderer, UiSemantic semantic) noexcept {
    return renderer->ui_scene
               ? ui_scene_resolve(*renderer->ui_scene, ui_scene_find(*renderer->ui_scene, semantic))
               : nullptr;
}

// State every pipeline shares: target, topology, rasterizer, blend, frame
// constants (VS b0) and sampler. Text and media rely on it as much as paint does.
void bind_shared_state(D3D11Renderer* renderer, DrawSubmissionState* submission) noexcept {
    auto* context = renderer->graphics.context;
    const auto& pipeline = renderer->pipeline;
    const float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (!draw_submission_bind_shared(submission)) {
        if (draw_submission_rebind_blend(submission))
            context->OMSetBlendState(pipeline.ui_blend_state, blend_factor, 0xFFFFFFFFu);
        return;
    }
    context->OMSetRenderTargets(1, &renderer->graphics.target, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->RSSetState(pipeline.ui_rasterizer_state);
    context->OMSetBlendState(pipeline.ui_blend_state, blend_factor, 0xFFFFFFFFu);
    context->VSSetConstantBuffers(0, 1, &pipeline.ui_frame_buffer);
    context->PSSetSamplers(0, 1, &pipeline.glyph_sampler);
}

// Binds a pipeline right before its first draw in a pass, unless it is the
// bound one. Paint, media and glyphs all draw four-vertex strips.
void select_pipeline(D3D11Renderer* renderer, DrawSubmissionState* submission, DrawPipeline next) noexcept {
    bind_shared_state(renderer, submission);
    if (!draw_submission_select(submission, next))
        return;
    auto* context = renderer->graphics.context;
    const auto& pipeline = renderer->pipeline;
    const UINT offset = 0;
    if (next == DrawPipeline::paint) {
        const UINT stride = sizeof(PaintQuad);
        context->IASetInputLayout(pipeline.ui_input_layout);
        context->IASetVertexBuffers(0, 1, &pipeline.ui_instance_buffer, &stride, &offset);
        context->VSSetShader(pipeline.ui_vertex_shader, nullptr, 0);
        context->PSSetShader(pipeline.ui_pixel_shader, nullptr, 0);
        // Opaque gradient surfaces sample global DIP dimensions (b0) and the
        // background palette (b1) in UiPS; b1 is empty without a background.
        ID3D11Buffer* constants[2]{pipeline.ui_frame_buffer,
                                   pipeline.background_active ? pipeline.background_buffer : nullptr};
        context->PSSetConstantBuffers(0, 2, constants);
        ID3D11ShaderResourceView* icon_view = renderer->icon_atlas.gpu.texture_view;
        context->PSSetShaderResources(0, 1, &icon_view);
    } else if (next == DrawPipeline::glyph) {
        const UINT stride = sizeof(GlyphQuad);
        context->IASetInputLayout(pipeline.glyph_input_layout);
        context->IASetVertexBuffers(0, 1, &pipeline.glyph_instance_buffer, &stride, &offset);
        context->VSSetShader(pipeline.glyph_vertex_shader, nullptr, 0);
        context->PSSetShader(pipeline.glyph_pixel_shader, nullptr, 0);
        // UiBackground shares b1 with the optional glyph appearance constants.
        ID3D11Buffer* effects = renderer->text_effect_count ? renderer->text_effect_buffer : nullptr;
        context->PSSetConstantBuffers(1, 1, &effects);
        ID3D11ShaderResourceView* glyph_views[]{renderer->glyph_text.gpu.texture_view,
                                                d3d11_glyph_text_color_view(renderer->glyph_text)};
        context->PSSetShaderResources(0, 2, glyph_views);
    } else {
        const UINT stride = sizeof(MediaQuad);
        context->IASetInputLayout(pipeline.media_input_layout);
        context->IASetVertexBuffers(0, 1, &pipeline.media_instance_buffer, &stride, &offset);
        context->VSSetShader(pipeline.media_vertex_shader, nullptr, 0);
        context->PSSetShader(pipeline.media_pixel_shader, nullptr, 0);
        ID3D11ShaderResourceView* media_views[3]{renderer->media_gpu.texture_view,
                                                 renderer->animation_gpu.texture_view,
                                                 renderer->media_gpu.preview_texture_view};
        context->PSSetShaderResources(0, 3, media_views);
    }
}

void set_scissor(D3D11Renderer* renderer, DrawSubmissionState* submission, const RasterClipRect& clip) noexcept {
    if (draw_submission_scissor(submission, clip)) {
        const LONG origin = static_cast<LONG>(renderer->target_origin_y);
        const D3D11_RECT scissor{clip.left, clip.top + origin, clip.right, clip.bottom + origin};
        renderer->graphics.context->RSSetScissorRects(1, &scissor);
    }
}

// A batch's clip in target pixels, limited to the damage; false when empty.
bool batch_scissor(const D3D11Renderer& renderer, UiRect clip, float pixel_scale, const D3D11_RECT* damage,
                   RasterClipRect* scissor) noexcept {
    const RasterClipRect pixels =
        raster_clip_rectangle(layer_clip(renderer, clip), pixel_scale, renderer.width, renderer.height);
    D3D11_RECT rectangle{pixels.left, pixels.top, pixels.right, pixels.bottom};
    if (!intersect_scissor(&rectangle, damage))
        return false;
    *scissor = {rectangle.left, rectangle.top, rectangle.right, rectangle.bottom};
    return true;
}

// Where a pass's instances live: glyph instances [0, prefix) start at
// prefix_base in the ring and the rest at suffix_base; paint and media keep
// their whole stream at the buffer start (prefix ~0u), except paint
// [moved_first, moved_end) (stepped typing dots) at moved_base.
struct RunTarget {
    D3D11Renderer* renderer;
    DrawSubmissionState* submission;
    DrawPipeline pipeline;
    u32 prefix, prefix_base, suffix_base;
    u32 moved_first = 0, moved_end = 0, moved_base = 0;
};

void draw_instances(const RunTarget& target, u32 first, u32 end) noexcept {
    D3D11Renderer* renderer = target.renderer;
    auto* context = renderer->graphics.context;
    if (first < end && first < target.prefix) {
        context->DrawInstanced(4, minimum(end, target.prefix) - first, 0, target.prefix_base + first);
        ++renderer->draw_calls_last_frame;
    }
    if (first < end && end > target.prefix) {
        const u32 from = maximum(first, target.prefix);
        context->DrawInstanced(4, end - from, 0, target.suffix_base + (from - target.prefix));
        ++renderer->draw_calls_last_frame;
    }
}

// One merged run: binds what its pipeline and scissor still need, then draws
// it (a run straddling the glyph ring's retained prefix or moved paint takes
// more calls).
void draw_run(const RunTarget& target, const DrawRun& run) noexcept {
    if (run.end <= run.first)
        return;
    D3D11Renderer* renderer = target.renderer;
    select_pipeline(renderer, target.submission, target.pipeline);
    set_scissor(renderer, target.submission, run.scissor);
    if (run.first >= target.moved_end || run.end <= target.moved_first) {
        draw_instances(target, run.first, run.end);
        return;
    }
    const u32 first = maximum(run.first, target.moved_first), end = minimum(run.end, target.moved_end);
    draw_instances(target, run.first, first);
    renderer->graphics.context->DrawInstanced(4, end - first, 0,
                                              target.moved_base + (first - target.moved_first));
    ++renderer->draw_calls_last_frame;
    draw_instances(target, end, run.end);
}

// Continues `run` with [first, end) under `scissor`, or draws `run` first and
// starts a new one.
void append_run(const RunTarget& target, DrawRun* run, const RasterClipRect& scissor, u32 first,
                u32 end) noexcept {
    if (first >= end || draw_run_extend(run, scissor, first, end - first))
        return;
    draw_run(target, *run);
    *run = {scissor, first, end};
}

bool draw_gpu_media(D3D11Renderer* renderer, DrawSubmissionState* submission, u32 first_batch, u32 end_batch,
                    bool upload, const D3D11_RECT* damage) noexcept {
    if (renderer->media_quad_count == 0) {
        return true;
    }
    if (!renderer->media_atlas || !renderer->media_gpu.texture_view ||
        !renderer->pipeline.media_pixel_shader || !renderer->pipeline.media_instance_buffer ||
        renderer->media_quad_count > kRendererMediaQuadCapacity || first_batch > end_batch ||
        end_batch > renderer->media_batch_count) {
        ++renderer->media_rejected;
        return false;
    }
    if (upload) {
        // The media stream is immutable between the base and popup passes.
        // Validate it once with the single per-frame upload, rather than
        // rescanning every batch again for the overlay draw.
        for (u32 index = 0; index < renderer->media_batch_count; ++index) {
            const MediaDrawBatch& batch = renderer->media_draw.batch_data[index];
            if (batch.quad_count == 0 || batch.first_quad > renderer->media_quad_count ||
                batch.quad_count > renderer->media_quad_count - batch.first_quad) {
                ++renderer->media_rejected;
                return false;
            }
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT map_result = renderer->graphics.context->Map(renderer->pipeline.media_instance_buffer,
                                                                   0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(map_result)) {
            ++renderer->media_rejected;
            log_write(LogLevel::error, "render", "media_instance_buffer_map_failed");
            return false;
        }
        auto* destination = static_cast<MediaQuad*>(mapped.pData);
        CopyMemory(destination, renderer->media_draw.quad_data,
                   static_cast<usize>(renderer->media_quad_count) * sizeof(MediaQuad));
        renderer->graphics.context->Unmap(renderer->pipeline.media_instance_buffer, 0);
        ++renderer->media_instance_uploads;
        renderer->media_upload_bytes += static_cast<u64>(renderer->media_quad_count) * sizeof(MediaQuad);
    }

    const float pixel_scale = static_cast<float>(renderer->dpi) / 96.0f;
    const RunTarget target{renderer, submission, DrawPipeline::media, ~0u, 0, 0};
    DrawRun run{};
    for (u32 index = first_batch; index < end_batch; ++index) {
        const MediaDrawBatch& batch = renderer->media_draw.batch_data[index];
        RasterClipRect scissor{};
        if (batch_scissor(*renderer, batch.clip, pixel_scale, damage, &scissor)) {
            append_run(target, &run, scissor, batch.first_quad, batch.first_quad + batch.quad_count);
            renderer->media_draws_last_frame += batch.quad_count;
        }
    }
    draw_run(target, run);
    return true;
}

bool draw_gpu_text(D3D11Renderer* renderer, DrawSubmissionState* submission, const Theme& theme, u32 first_batch,
                   u32 end_batch, bool upload, const D3D11_RECT* damage) noexcept {
    const bool content = renderer->content_text != nullptr;
    const u32 quad_count =
        content ? renderer->content_text->quad_count : renderer->glyph_text.title_quad_count;
    const u32 batch_count = content ? renderer->content_text->batch_count : 1;
    if (first_batch > end_batch || end_batch > batch_count) {
        return false;
    }
    if (first_batch == end_batch) {
        return true;
    }
    if (!renderer->glyph_text.ready || quad_count == 0) {
        return true;
    }
    if (quad_count > renderer->pipeline.glyph_instance_capacity ||
        (content && (!renderer->content_text->quads || !renderer->content_text->batches))) {
        return false;
    }
    const bool retained_content = content && renderer->uploaded_text_storage == renderer->content_text &&
                                  renderer->uploaded_text_rebuild == renderer->content_text->rebuilds &&
                                  renderer->uploaded_text_quad_count == quad_count;
    upload = upload && !retained_content;
    if (upload) {
        // Leading glyphs unchanged since the ring last received them (the
        // shell while only messages moved) stay where the GPU holds them;
        // only the rest of the stream is written ahead in the ring.
        const auto* text = renderer->content_text;
        const u32 kept =
            content && renderer->uploaded_text_storage == text && renderer->text_stable_quads <= quad_count &&
                    renderer->text_stable_quads <= renderer->uploaded_text_prefix &&
                    renderer->uploaded_text_prefix_epoch == renderer->text_ring_epoch &&
                    renderer->uploaded_text_prefix_serial >= renderer->text_stable_serial
                ? renderer->text_stable_quads
                : 0u;
        const u32 written = quad_count - kept;
        const u32 remaining =
            renderer->pipeline.glyph_instance_capacity - renderer->pipeline.glyph_instance_end;
        const bool fits = written <= remaining && renderer->pipeline.glyph_instance_end != 0;
        // A discard gives the ring new storage: nothing written before survives.
        const u32 first = fits ? renderer->pipeline.glyph_instance_end : 0;
        const u32 reused = fits ? kept : 0u;
        const u32 count = quad_count - reused;
        const D3D11_MAP mode = first ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT map_result =
            renderer->graphics.context->Map(renderer->pipeline.glyph_instance_buffer, 0, mode, 0, &mapped);
        if (FAILED(map_result)) {
            log_write(LogLevel::error, "render", "glyph_instance_buffer_map_failed");
            return false;
        }
        if (mode == D3D11_MAP_WRITE_DISCARD)
            ++renderer->text_ring_epoch;
        GlyphQuad* destination = static_cast<GlyphQuad*>(mapped.pData) + first;
        if (content) {
            CopyMemory(destination, text->quads + reused, static_cast<usize>(count) * sizeof(GlyphQuad));
        } else {
            const UiNode* title =
                renderer->login_visible ? nullptr : scene_node(renderer, UiSemantic::title_bar);
            if (!renderer->login_visible && !title) {
                renderer->graphics.context->Unmap(renderer->pipeline.glyph_instance_buffer, 0);
                return false;
            }
            const UiRect title_content =
                renderer->login_visible
                    ? UiRect{}
                    : ui_scene_content_rectangle(*renderer->ui_scene, UiSemantic::title_bar);
            const float origin_x = renderer->login_visible ? 0.0f : title_content.left;
            const float origin_y = renderer->login_visible ? 0.0f : title_content.top;
            for (u32 index = 0; index < quad_count; ++index) {
                destination[index] = renderer->glyph_text.title_quads[index];
                destination[index].left += origin_x;
                destination[index].right += origin_x;
                destination[index].top += origin_y;
                destination[index].bottom += origin_y;
                if (!renderer->login_visible) {
                    destination[index].color = theme.text_primary;
                }
            }
        }
        renderer->graphics.context->Unmap(renderer->pipeline.glyph_instance_buffer, 0);
        renderer->pipeline.glyph_instance_end = first + count;
        ++renderer->text_instance_uploads;
        renderer->text_upload_bytes += static_cast<u64>(count) * sizeof(GlyphQuad);
        if (reused) {
            renderer->uploaded_text_prefix = reused;
            renderer->uploaded_text_suffix_base = first;
        } else {
            renderer->uploaded_text_prefix = quad_count;
            renderer->uploaded_text_prefix_base = renderer->uploaded_text_suffix_base = first;
            renderer->uploaded_text_prefix_serial = content ? text->rebuilds : 0;
            renderer->uploaded_text_prefix_epoch = renderer->text_ring_epoch;
        }
        if (content) {
            renderer->uploaded_text_storage = renderer->content_text;
            renderer->uploaded_text_rebuild = renderer->content_text->rebuilds;
            renderer->uploaded_text_quad_count = quad_count;
        } else {
            renderer->uploaded_text_storage = nullptr;
        }
    }

    // Instances [0, prefix) live at prefix_base, the rest at suffix_base.
    const u32 prefix = minimum(renderer->uploaded_text_prefix, quad_count);
    const u32 prefix_base = renderer->uploaded_text_prefix_base;
    const u32 suffix_base = renderer->uploaded_text_suffix_base;
    if (prefix_base + prefix > renderer->pipeline.glyph_instance_capacity ||
        suffix_base + (quad_count - prefix) > renderer->pipeline.glyph_instance_end) {
        return false;
    }
    const UiNode* title =
        content || renderer->login_visible ? nullptr : scene_node(renderer, UiSemantic::title_bar);
    if (!content && !renderer->login_visible && !title) {
        return false;
    }
    const float pixel_scale = static_cast<float>(renderer->dpi) / 96.0f;
    const RunTarget target{renderer, submission, DrawPipeline::glyph, prefix, prefix_base, suffix_base};
    DrawRun run{};
    for (u32 index = first_batch; index < end_batch; ++index) {
        const GlyphDrawBatch batch =
            content ? renderer->content_text->batches[index]
                    : GlyphDrawBatch{renderer->login_visible
                                         ? login_scene_rectangle(*renderer->login_scene, LoginElement::root)
                                         : title->rectangle,
                                     0, quad_count};
        if (batch.quad_count == 0 || batch.first_quad > quad_count ||
            batch.quad_count > quad_count - batch.first_quad) {
            return false;
        }
        RasterClipRect scissor{};
        if (batch_scissor(*renderer, batch.clip, pixel_scale, damage, &scissor))
            append_run(target, &run, scissor, batch.first_quad, batch.first_quad + batch.quad_count);
    }
    draw_run(target, run);
    return true;
}

bool draw_paint_batches(D3D11Renderer* renderer, DrawSubmissionState* submission, u32 first_batch,
                        u32 end_batch, float pixel_scale, const D3D11_RECT* damage) noexcept {
    if (first_batch > end_batch || end_batch > renderer->paint_stream.batch_count) {
        return false;
    }
    if (first_batch == end_batch) {
        return true;
    }
    auto* context = renderer->graphics.context;
    const auto& dots = renderer->typing_dots;
    const bool moved = dots.paint.quads && dots.gpu_first != dots.paint.first_quad;
    const RunTarget target{renderer,
                           submission,
                           DrawPipeline::paint,
                           ~0u,
                           0,
                           0,
                           moved ? dots.paint.first_quad : 0,
                           moved ? dots.paint.first_quad + dots.paint.quads : 0,
                           dots.gpu_first};
    // The background effect draws over each canvas right after its base fill,
    // before the quads (and later text and media) that follow it.
    const bool effects = renderer->background_fx.source != BackgroundFxSource::none;
    DrawRun run{};
    for (u32 index = first_batch; index < end_batch; ++index) {
        const PaintBatch& batch = renderer->paint_stream.batches[index];
        if (batch.first_quad > renderer->paint_stream.count ||
            batch.quad_count > renderer->paint_stream.count - batch.first_quad ||
            batch.first_quad > renderer->pipeline.ui_instance_capacity ||
            batch.quad_count > renderer->pipeline.ui_instance_capacity - batch.first_quad)
            return false;
        if (batch.quad_count == 0 || batch.clip_index >= renderer->paint_stream.clip_count) {
            continue;
        }
        const PaintClip& clip = renderer->paint_stream.clips[batch.clip_index];
        // A scrim over the whole content opens its batch (it follows the
        // overlay's clip); the window caption dims with it.
        const PaintQuad& lead = renderer->paint_stream.quads[batch.first_quad];
        if (lead.left <= 0 && lead.top <= 0 && lead.right * pixel_scale >= static_cast<float>(renderer->width) &&
            lead.bottom * pixel_scale >= static_cast<float>(renderer->height) && lead.radius == 0 &&
            lead.border_width == 0 && lead.reserved_x != kUiBackgroundCanvasMarker)
            d3d11_renderer_caption_note_scrim(renderer, lead.fill, renderer->background_fx.layer_opacity);
        RasterClipRect scissor{};
        if (!batch_scissor(*renderer, UiRect{clip.left, clip.top, clip.right, clip.bottom}, pixel_scale, damage,
                           &scissor)) {
            continue;
        }
        u32 first = batch.first_quad;
        const u32 end = batch.first_quad + batch.quad_count;
        for (u32 quad = first; effects && quad < end; ++quad) {
            const PaintQuad& canvas = renderer->paint_stream.quads[quad];
            if (!ui_background_canvas_quad(canvas))
                continue;
            append_run(target, &run, scissor, first, quad + 1);
            draw_run(target, run);
            reset_value(&run);
            first = quad + 1;
            if (d3d11_background_fx_draw(&renderer->background_fx, context,
                                         {canvas.left, canvas.top, canvas.right, canvas.bottom}, quad)) {
                ++renderer->draw_calls_last_frame;
                // The effect bound its own shaders, constants and blend state.
                draw_submission_reset_pipeline(submission);
            }
        }
        append_run(target, &run, scissor, first, end);
    }
    draw_run(target, run);
    return true;
}

bool settings_modal_visible(const D3D11Renderer& renderer) noexcept {
    return renderer_settings_page_visible(renderer) &&
           renderer.settings_scene->presentation != UiSettingsPresentation::layer;
}
// Full-window layers enter over the frozen shell, which recedes and fades.
bool settings_layer_entrance(const D3D11Renderer& renderer) noexcept {
    return renderer_settings_page_visible(renderer) &&
           renderer.settings_scene->presentation == UiSettingsPresentation::layer &&
           renderer.settings_modal_progress >= 0 && renderer.settings_modal_progress < 1;
}
bool settings_backdrop_wanted(const D3D11Renderer& renderer) noexcept {
    return !renderer.login_visible && (settings_modal_visible(renderer) || settings_layer_entrance(renderer));
}
// The shell as it draws with no modal open: the modal, its choice popup and
// forms, and the transient overlays that opening a modal dismisses are
// unbound for the scope. hide_text also hides the modal's text bank, which
// the shell stamp must not observe.
struct ShellBackdropView {
    D3D11Renderer* renderer;
    const UiSettingsScene* settings;
    const UiThemeEditorScene* editor;
    const PopupScene* popup;
    TextFormSurface* text_form;
    ForumPostSurface* forum_form;
    const UiTooltipScene* tooltip;
    const UiToastScene* toast;
    const UiWidgetManager* focus;
    const UiImageViewer* image;
    ProfileScene* profile;
    const MessageActionBar* bar;
    const ReactionViewer* reactions;
    RendererTextStorage* text;
    ShellBackdropView(D3D11Renderer* owner, bool hide_text) noexcept
        : renderer(owner), settings(owner->settings_scene), editor(owner->theme_editor_scene),
          popup(owner->popup_scene), text_form(owner->text_form), forum_form(owner->forum_form),
          tooltip(owner->tooltip_scene), toast(owner->toast_scene), focus(owner->focus_owner),
          image(owner->image_viewer),
          profile(owner->profile_scene), bar(owner->message_action_bar), reactions(owner->reaction_viewer),
          text(owner->content_text) {
        owner->settings_scene = nullptr;
        owner->theme_editor_scene = nullptr;
        owner->popup_scene = nullptr;
        owner->text_form = nullptr;
        owner->forum_form = nullptr;
        owner->tooltip_scene = nullptr;
        owner->toast_scene = nullptr;
        owner->focus_owner = nullptr;
        owner->image_viewer = nullptr;
        owner->profile_scene = nullptr;
        owner->message_action_bar = nullptr;
        owner->reaction_viewer = nullptr;
        if (hide_text)
            owner->content_text = nullptr;
    }
    ~ShellBackdropView() noexcept {
        renderer->settings_scene = settings;
        renderer->theme_editor_scene = editor;
        renderer->popup_scene = popup;
        renderer->text_form = text_form;
        renderer->forum_form = forum_form;
        renderer->tooltip_scene = tooltip;
        renderer->toast_scene = toast;
        renderer->focus_owner = focus;
        renderer->image_viewer = image;
        renderer->profile_scene = profile;
        renderer->message_action_bar = bar;
        renderer->reaction_viewer = reactions;
        renderer->content_text = text;
    }
    ShellBackdropView(const ShellBackdropView&) = delete;
    ShellBackdropView& operator=(const ShellBackdropView&) = delete;
};
// Every input the shell frame depends on, as its own present would observe
// it (spinner phases included), plus the message text projection: the modal
// owns the text bank, so the shell's text revision is not observable here.
RendererPresentationStamp shell_backdrop_stamp(D3D11Renderer* renderer, const Theme& theme) noexcept {
    const ShellBackdropView view(renderer, true);
    auto stamp = renderer_capture_presentation_stamp(*renderer, theme, false);
    const u64 projection = renderer->message_text ? renderer->message_text->projection_signature : 0;
    stamp.primary = (stamp.primary ^ projection) * 0x100000001B3ull;
    stamp.secondary ^= (projection << 31) | (projection >> 33);
    return stamp;
}
// A new frame of media the backdrop drew, pending since its last present.
bool shell_backdrop_media_changed(const D3D11Renderer& renderer) noexcept {
    if (!renderer.presentation_damage.dirty_media_count)
        return false;
    for (u32 i = 0; i < renderer.settings_backdrop_media_count; ++i)
        if (presentation_damage_media_pending(renderer.presentation_damage, renderer.settings_backdrop_media[i],
                                              renderer.settings_backdrop_media_domain[i]))
            return true;
    return false;
}
void release_backdrop_target(D3D11Renderer* renderer) noexcept {
    if (renderer->settings_backdrop_target)
        renderer->settings_backdrop_target->Release();
    if (renderer->settings_backdrop_view)
        renderer->settings_backdrop_view->Release();
    if (renderer->settings_backdrop)
        renderer->settings_backdrop->Release();
    renderer->settings_backdrop_target = nullptr;
    renderer->settings_backdrop_view = nullptr;
    renderer->settings_backdrop = nullptr;
    renderer->settings_backdrop_valid = false;
}
// Clearing the swap chain target also wipes the caption; it redraws after
// the stream.
void clear_target(D3D11Renderer* renderer, const float color[4]) noexcept {
    renderer->graphics.context->ClearRenderTargetView(renderer->graphics.target, color);
    d3d11_renderer_caption_invalidate(renderer);
}
// One render target the size of the content, in the back buffer's format.
bool create_backdrop_target(D3D11Renderer* renderer) noexcept {
    if (renderer->settings_backdrop && renderer->settings_backdrop_width == renderer->width &&
        renderer->settings_backdrop_height == renderer->height)
        return true;
    release_backdrop_target(renderer);
    ID3D11Resource* source = nullptr;
    renderer->graphics.target->GetResource(&source);
    ID3D11Texture2D* back_buffer = nullptr;
    const bool typed = source && SUCCEEDED(source->QueryInterface(__uuidof(ID3D11Texture2D),
                                                                  reinterpret_cast<void**>(&back_buffer)));
    D3D11_TEXTURE2D_DESC description{};
    if (typed)
        back_buffer->GetDesc(&description);
    if (back_buffer)
        back_buffer->Release();
    if (source)
        source->Release();
    bool ok = typed && description.Width == renderer->width &&
              description.Height == renderer->height + renderer->target_origin_y;
    if (ok) {
        description.Height = renderer->height;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.CPUAccessFlags = 0;
        description.MiscFlags = 0;
        description.MipLevels = 1;
        description.ArraySize = 1;
        ok = SUCCEEDED(renderer->graphics.device->CreateTexture2D(&description, nullptr, &renderer->settings_backdrop)) &&
             SUCCEEDED(renderer->graphics.device->CreateShaderResourceView(renderer->settings_backdrop, nullptr,
                                                                           &renderer->settings_backdrop_view)) &&
             SUCCEEDED(renderer->graphics.device->CreateRenderTargetView(renderer->settings_backdrop, nullptr,
                                                                         &renderer->settings_backdrop_target));
    }
    if (ok && !renderer->settings_backdrop_quad) {
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(MediaQuad);
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(renderer->graphics.device->CreateBuffer(&buffer, nullptr, &renderer->settings_backdrop_quad));
    }
    if (!ok) {
        release_backdrop_target(renderer);
        return false;
    }
    renderer->settings_backdrop_width = renderer->width;
    renderer->settings_backdrop_height = renderer->height;
    return true;
}
bool draw_settings_backdrop(D3D11Renderer* renderer, DrawSubmissionState* submission, const Theme& theme,
                            const D3D11_RECT* damage) noexcept {
    const bool layer = settings_layer_entrance(*renderer);
    if (!settings_modal_visible(*renderer) && !layer)
        return true;
    // A shell that has not drawn yet leaves the theme's base. A backdrop from
    // before a resize stretches until the shell draws at the new size.
    if (!renderer->settings_backdrop_valid) {
        if (!damage) {
            const float color[4]{theme.base_lowest.red, theme.base_lowest.green, theme.base_lowest.blue, 1};
            clear_target(renderer, color);
        }
        return true;
    }
    if (layer) {
        const float color[4]{theme.base_lowest.red, theme.base_lowest.green, theme.base_lowest.blue, 1};
        clear_target(renderer, color);
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(renderer->graphics.context->Map(renderer->settings_backdrop_quad, 0, D3D11_MAP_WRITE_DISCARD, 0,
                                               &mapped)) ||
        !mapped.pData)
        return false;
    const float dips = 96.f / static_cast<float>(maximum<u32>(renderer->dpi, 1));
    const float width = static_cast<float>(renderer->width) * dips;
    const float height = static_cast<float>(renderer->height) * dips;
    // Layer entrance: the shell shrinks to 93% about the centre while fading.
    const float progress = layer ? renderer->settings_modal_progress : 0;
    const float inset = .035f * progress;
    const UiRect shell{width * inset, height * inset, width * (1 - inset), height * (1 - inset)};
    *static_cast<MediaQuad*>(mapped.pData) = {shell.left, shell.top, shell.right, shell.bottom, 0, 0, 1, 1,
                                             layer ? 1 - progress : 1, 0, 0, 0, shell};
    renderer->graphics.context->Unmap(renderer->settings_backdrop_quad, 0);
    const UINT stride = sizeof(MediaQuad), offset = 0;
    auto* context = renderer->graphics.context;
    // The backdrop binds its own quad and view over the shared media pipeline.
    draw_submission_reset(submission);
    context->OMSetRenderTargets(1, &renderer->graphics.target, nullptr);
    const float blend_factor[4]{0, 0, 0, 0};
    context->OMSetBlendState(renderer->pipeline.ui_blend_state, blend_factor, 0xFFFFFFFFu);
    context->RSSetState(renderer->pipeline.ui_rasterizer_state);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->IASetInputLayout(renderer->pipeline.media_input_layout);
    context->IASetVertexBuffers(0, 1, &renderer->settings_backdrop_quad, &stride, &offset);
    context->VSSetShader(renderer->pipeline.media_vertex_shader, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &renderer->pipeline.ui_frame_buffer);
    context->PSSetShader(renderer->pipeline.media_pixel_shader, nullptr, 0);
    ID3D11ShaderResourceView* views[3]{renderer->settings_backdrop_view, nullptr, nullptr};
    context->PSSetShaderResources(0, 3, views);
    context->PSSetSamplers(0, 1, &renderer->pipeline.glyph_sampler);
    D3D11_RECT scissor{0, 0, static_cast<LONG>(renderer->width), static_cast<LONG>(renderer->height)};
    if (intersect_scissor(&scissor, damage)) {
        scissor.top += static_cast<LONG>(renderer->target_origin_y);
        scissor.bottom += static_cast<LONG>(renderer->target_origin_y);
        context->RSSetScissorRects(1, &scissor);
        context->DrawInstanced(4, 1, 0, 0);
        ++renderer->draw_calls_last_frame;
    }
    ID3D11ShaderResourceView* null_views[3]{};
    context->PSSetShaderResources(0, 3, null_views);
    return true;
}

} // namespace

void renderer_release_settings_backdrop(D3D11Renderer* renderer) noexcept {
    if (!renderer)
        return;
    release_backdrop_target(renderer);
    reset_value(&renderer->settings_backdrop_stamp);
    renderer->settings_backdrop_media_count = 0;
}

// Frames that draw only the modal begin a visibility epoch that pins just the
// modal's media. Pinning the media the backdrop drew keeps it from being
// evicted when a full atlas resets on the next upload (a GIF frame), which
// would make the backdrop flicker while anything animates.
void renderer_pin_settings_backdrop_media(D3D11Renderer* renderer) noexcept {
    if (!renderer || !renderer->settings_backdrop_valid || !settings_backdrop_wanted(*renderer))
        return;
    const u32 count = minimum(renderer->settings_backdrop_media_count, kRendererMediaQuadCapacity);
    for (u32 i = 0; i < count; ++i) {
        const u8 domain = renderer->settings_backdrop_media_domain[i];
        MediaAtlas* atlas = domain == 0 ? renderer->media_atlas : domain == 1 ? renderer->preview_media_atlas : nullptr;
        MediaAtlasResult visible{};
        if (!atlas || !media_atlas_resolve_visible(atlas, renderer->settings_backdrop_media[i], &visible))
            continue;
        GlyphAtlasAllocation frame{};
        if (domain == 0)
            static_cast<void>(animation_frame_cache_gpu_resolve(&renderer->animation_gpu, visible, &frame));
    }
}

bool renderer_settings_backdrop_stale(D3D11Renderer* renderer, const Theme& theme) noexcept {
    if (!renderer || !renderer->graphics.target || !renderer->glyph_text.ready ||
        !settings_backdrop_wanted(*renderer))
        return false;
    return !renderer->settings_backdrop_stamp.valid || shell_backdrop_media_changed(*renderer) ||
           !renderer_presentation_stamp_equal(shell_backdrop_stamp(renderer, theme), renderer->settings_backdrop_stamp);
}

bool renderer_refresh_settings_backdrop(D3D11Renderer* renderer, const Theme& theme) noexcept {
    if (!renderer || !renderer->graphics.target)
        return false;
    bool drawn = false;
    {
        // The shell's own text, paint and media, drawn in full into the
        // backdrop target exactly as a frame with no modal would draw them.
        // Message text projected for another width still draws; a projection
        // the shell cannot place yet falls back to the chrome's own text.
        ShellBackdropView view(renderer, false);
        if (renderer->content_text &&
            (d3d11_renderer_rebuild_text_mode(renderer, theme, true) ||
             d3d11_renderer_rebuild_text_mode(renderer, theme, false)) &&
            d3d11_renderer_build_paint(renderer, theme) && create_backdrop_target(renderer)) {
            const float color[4]{theme.base_lowest.red, theme.base_lowest.green, theme.base_lowest.blue, 1};
            ID3D11RenderTargetView* presented = renderer->graphics.target;
            const u32 origin = renderer->target_origin_y;
            renderer->graphics.target = renderer->settings_backdrop_target;
            renderer_bind_target_origin(renderer, 0);
            renderer->graphics.context->ClearRenderTargetView(renderer->graphics.target, color);
            drawn = d3d11_renderer_draw_stream(renderer, theme, true, true, nullptr);
            renderer->graphics.target = presented;
            renderer_bind_target_origin(renderer, origin);
            const u32 media = drawn ? minimum(renderer->media_quad_count, kRendererMediaQuadCapacity) : 0;
            for (u32 i = 0; i < media; ++i) {
                renderer->settings_backdrop_media[i] = renderer->media_draw.handle_data[i];
                renderer->settings_backdrop_media_domain[i] = renderer->media_draw.domain_data[i];
            }
            if (drawn)
                renderer->settings_backdrop_media_count = media;
            renderer->graphics.context->OMSetRenderTargets(1, &renderer->graphics.target, nullptr);
        }
    }
    if (drawn) {
        renderer->settings_backdrop_valid = true;
        ++renderer->settings_backdrop_renders;
    }
    renderer->settings_backdrop_failed = !drawn;
    // The modal's text and paint rebuild over the shell's and present whole.
    renderer->retained_presentation.valid = false;
    presentation_damage_invalidate_full(&renderer->presentation_damage);
    const bool text = !renderer->content_text || d3d11_renderer_rebuild_text_mode(renderer, theme, true);
    renderer->settings_backdrop_stamp = shell_backdrop_stamp(renderer, theme);
    return text;
}

namespace {
// Views stay bound across passes and are released once per stream: the icon,
// glyph and media atlases become compute targets between frames.
void release_stream_views(D3D11Renderer* renderer, const DrawSubmissionState& submission) noexcept {
    if (!submission.pipeline_binds)
        return;
    ID3D11ShaderResourceView* null_views[3]{};
    renderer->graphics.context->PSSetShaderResources(0, 3, null_views);
}
bool prepare_text_effects(D3D11Renderer* renderer, u64 effect_ms) noexcept {
    if (!renderer->text_effect_buffer) {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = sizeof(TextEffectConstants);
        description.Usage = D3D11_USAGE_DYNAMIC;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(renderer->graphics.device->CreateBuffer(&description, nullptr,
                                                           &renderer->text_effect_buffer)))
            return false;
        renderer->text_effect_uploaded_count = 0;
    }
    const float seconds = static_cast<float>(effect_ms % 60000) / 1000.f;
    const float scale = static_cast<float>(renderer->dpi) / 96.f;
    if (text_effect_upload_required(renderer->text_effects, renderer->text_effect_uploaded_count,
                                    renderer->text_effect_count, renderer->text_effect_animated, seconds, scale)) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(renderer->graphics.context->Map(renderer->text_effect_buffer, 0,
                                                   D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
        renderer->text_effects.seconds = seconds;
        renderer->text_effects.scale = scale;
        CopyMemory(mapped.pData, &renderer->text_effects,
                   offsetof(TextEffectConstants, materials) +
                       renderer->text_effect_count * sizeof(TextEffectMaterial));
        renderer->graphics.context->Unmap(renderer->text_effect_buffer, 0);
        renderer->text_effect_uploaded_count = static_cast<u8>(renderer->text_effect_count);
    }
    // The glyph pipeline binds PS b1 when it is selected.
    renderer->graphics.context->VSSetConstantBuffers(1, 1, &renderer->text_effect_buffer);
    return true;
}

} // namespace

bool d3d11_renderer_draw_stream(D3D11Renderer* renderer, const Theme& theme, bool upload_paint,
                                bool upload_media, const D3D11_RECT* damage, u64 effect_time_ms) noexcept {
    if (!renderer || renderer->paint_stream.count > renderer->paint_stream.quad_capacity ||
        renderer->paint_stream.count > kMaxPaintQuads ||
        renderer->paint_stream.clip_count > kMaxPaintClips ||
        renderer->paint_stream.batch_count > kMaxPaintBatches ||
        (renderer->paint_stream.count && !renderer->paint_stream.quads))
        return false;
    if (!d3d11_draw_pipeline_update_background(&renderer->pipeline, renderer->graphics.device,
                                               renderer->graphics.context, theme,
                                               renderer->background_phase_seconds))
        return false;
    if (!renderer->content_text && !renderer->login_visible && renderer->glyph_text.ready &&
        !renderer->glyph_text.title_quad_count &&
        !d3d11_glyph_text_rebuild_title(&renderer->glyph_text, renderer->graphics.context, renderer->dpi,
                                        false)) {
        return false;
    }
    const u32 glyph_count =
        renderer->content_text ? renderer->content_text->quad_count : renderer->glyph_text.title_quad_count;
    if (renderer->paint_stream.count == 0 && glyph_count == 0 && renderer->media_quad_count == 0) {
        return true;
    }

    // Icon cells depend on DPI, icon weight and the active pack; re-resolve
    // the retained paint copy when any of them changed.
    const float icon_weight = ui_icon_theme_weight(theme);
    upload_paint = upload_paint || icon_atlas_stale(renderer->icon_atlas, renderer->dpi, icon_weight);
    // A typing dots step writes its quads past the stream's end in the
    // instance buffer; once that tail is full the whole stream uploads again.
    auto& dots = renderer->typing_dots;
    const u32 dots_base = maximum(dots.ring_end, renderer->paint_stream.count);
    upload_paint = upload_paint || (dots.upload_pending && (dots_base > renderer->pipeline.ui_instance_capacity ||
                                                            renderer->pipeline.ui_instance_capacity - dots_base <
                                                                dots.paint.quads));
    D3D11_MAPPED_SUBRESOURCE step_mapped{};
    if (!upload_paint && dots.upload_pending && renderer->pipeline.ui_instance_buffer) {
        const HRESULT mapped_result = renderer->graphics.context->Map(
            renderer->pipeline.ui_instance_buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &step_mapped);
        if (SUCCEEDED(mapped_result) && !step_mapped.pData)
            renderer->graphics.context->Unmap(renderer->pipeline.ui_instance_buffer, 0);
        // A refused no-overwrite map: the whole stream uploads instead.
        if (FAILED(mapped_result) || !step_mapped.pData) {
            reset_value(&step_mapped);
            upload_paint = true;
        }
    }
    if (step_mapped.pData) {
        const usize bytes = static_cast<usize>(dots.paint.quads) * sizeof(PaintQuad);
        CopyMemory(static_cast<PaintQuad*>(step_mapped.pData) + dots_base,
                   renderer->paint_stream.quads + dots.paint.first_quad, bytes);
        renderer->graphics.context->Unmap(renderer->pipeline.ui_instance_buffer, 0);
        dots.gpu_first = dots_base;
        dots.ring_end = dots_base + dots.paint.quads;
        dots.upload_pending = false;
        ++dots.uploads;
        renderer->paint_upload_bytes += bytes;
    }
    if (upload_paint && renderer->paint_stream.count != 0) {
        const auto reserved =
            d3d11_draw_pipeline_reserve_ui_instances(&renderer->pipeline, renderer->graphics.device,
                                                     sizeof(PaintQuad), renderer->paint_stream.count);
        if (!reserved.ok()) {
            log_write(LogLevel::error, "render", "ui_instance_buffer_growth_failed");
            log_u64(LogLevel::error, "render", "ui_instance_native_error",
                    static_cast<u32>(reserved.native_error));
            return false;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT map_result = renderer->graphics.context->Map(renderer->pipeline.ui_instance_buffer, 0,
                                                                   D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(map_result)) {
            log_write(LogLevel::error, "render", "ui_instance_buffer_map_failed");
            return false;
        }
        if (!mapped.pData) {
            renderer->graphics.context->Unmap(renderer->pipeline.ui_instance_buffer, 0);
            return false;
        }
        auto* destination = static_cast<PaintQuad*>(mapped.pData);
        if (!icon_atlas_resolve(&renderer->icon_atlas, renderer->graphics.context, renderer->paint_stream.quads,
                                destination, renderer->paint_stream.count, renderer->dpi, icon_weight))
            log_write(LogLevel::warning, "render", "icon_atlas_raster_failed");
        renderer->graphics.context->Unmap(renderer->pipeline.ui_instance_buffer, 0);
        ++renderer->paint_instance_uploads;
        renderer->paint_upload_bytes += static_cast<u64>(renderer->paint_stream.count) * sizeof(PaintQuad);
        dots.gpu_first = dots.paint.first_quad;
        dots.ring_end = renderer->paint_stream.count;
        dots.upload_pending = false;
    }
    if (renderer->paint_stream.count &&
        (!renderer->pipeline.ui_instance_buffer ||
         renderer->paint_stream.count > renderer->pipeline.ui_instance_capacity))
        return false;

    D3D11GpuTimerFrame* gpu_timestamp =
        d3d11_gpu_timer_begin(&renderer->gpu_timer, renderer->graphics.context);
    // Uploads and compute work before this point bound over the context.
    DrawSubmissionState submission{};
    draw_submission_begin(&submission);
    const u64 frame_ms = GetTickCount64();
    // Effects are decoration: without their resources the frame draws plain.
    static_cast<void>(d3d11_background_fx_begin_frame(&renderer->background_fx, renderer->graphics.device,
                                                      renderer->graphics.context, theme,
                                                      renderer->background_phase_seconds, renderer->width,
                                                      renderer->height, renderer->dpi, frame_ms));
    renderer->text_effect_next_ms = frame_ms + 42;
    d3d11_renderer_caption_begin_scrim(renderer);
    renderer->draw_calls_last_frame = 0;
    renderer->media_draws_last_frame = 0;
    const float pixel_scale = static_cast<float>(renderer->dpi) / 96.0f;
    // A visible toast's batches close both streams; every layer below ends
    // before them and the toast draws last.
    const bool settings_active = renderer_settings_page_visible(*renderer);
    const bool toast = d3d11_renderer_toast_visible(*renderer);
    const u32 stream_paint_batches = renderer->paint_stream.batch_count;
    const u32 paint_batch_count =
        toast ? minimum(renderer->toast_paint_first_batch, stream_paint_batches) : stream_paint_batches;
    const u32 text_batch_count = renderer->content_text ? renderer->content_text->batch_count : 1u;
    const bool login_form = renderer->login_visible && renderer->login_form_text && renderer->text_form &&
                            renderer->text_form->scene && renderer->text_form->scene->active;
    const bool interaction_overlay =
        login_form ||
        (!renderer->login_visible &&
         (renderer_theme_editor_visible(*renderer) || settings_active ||
          d3d11_renderer_active(renderer->profile_scene) ||
          d3d11_renderer_active(renderer->friends_requests_scene) ||
          d3d11_renderer_active(renderer->image_viewer) ||
          d3d11_renderer_video_scene_visible(*renderer) || d3d11_renderer_audio_scene_visible(*renderer) ||
          d3d11_renderer_poll_scene_visible(*renderer) || d3d11_renderer_poll_voters_visible(*renderer) ||
          d3d11_renderer_tooltip_visible(*renderer) || d3d11_renderer_search_visible(*renderer) ||
          d3d11_renderer_unread_banner_visible(*renderer) || toast ||
          d3d11_renderer_pinned_messages_visible(*renderer) ||
          d3d11_renderer_quick_switcher_visible(*renderer) || d3d11_renderer_inbox_visible(*renderer) ||
          d3d11_renderer_message_forward_visible(*renderer) || d3d11_renderer_download_visible(*renderer) ||
          d3d11_renderer_shortcuts_visible(*renderer) || d3d11_renderer_media_picker_visible(*renderer) ||
          d3d11_renderer_call_visible(*renderer) || d3d11_renderer_stream_card_visible(*renderer) ||
          d3d11_renderer_guild_card_visible(*renderer) ||
          d3d11_renderer_active(renderer->message_action_bar) ||
          d3d11_renderer_active(renderer->popup_scene) ||
          d3d11_renderer_form_active(*renderer) ||
          d3d11_renderer_active(renderer->reaction_viewer)));
    const bool interaction_text_current = d3d11_renderer_interaction_text_state_current(*renderer);
    const u32 base_paint_end =
        interaction_overlay ? renderer->overlay_paint_first_batch : paint_batch_count;
    // Login/bootstrap text has one standalone batch, outside the account
    // content stream. Its visibility must not depend on popup batch markers.
    const u32 base_text_end = renderer->content_text
                                  ? minimum(renderer->overlay_text_first_batch, text_batch_count)
                                  : text_batch_count;
    const u32 stream_text_end =
        interaction_overlay && interaction_text_current ? text_batch_count : base_text_end;
    const u32 visible_text_end =
        toast ? maximum(base_text_end, minimum(renderer->toast_text_first_batch, stream_text_end))
              : stream_text_end;
    if (!interaction_text_current && text_batch_count > base_text_end) {
        saturating_increment(renderer->stale_interaction_text_suppressions);
    }
    const u32 profile_foreground =
        d3d11_renderer_active(renderer->profile_scene)
            ? minimum(renderer->profile_foreground_first_batch, paint_batch_count)
            : paint_batch_count;
    const u32 profile_controls =
        d3d11_renderer_active(renderer->profile_scene)
            ? maximum(profile_foreground,
                      minimum(renderer->profile_controls_first_batch, paint_batch_count))
            : paint_batch_count;
    const u32 media_overlay_begin =
        renderer->content_text && renderer->media_overlay_paint_first_batch <= base_paint_end
            ? renderer->media_overlay_paint_first_batch
            : base_paint_end;
    const bool inline_tail = renderer->inline_media_last_batch == renderer->media_batch_count &&
                             renderer->inline_media_first_batch <= renderer->inline_media_last_batch;
    const u32 ordinary_media_end =
        inline_tail ? renderer->inline_media_first_batch : renderer->media_batch_count;
    const u32 popup_media_first = minimum(renderer->popup_media_first_batch, ordinary_media_end);
    const u32 gallery_media_first = minimum(renderer->media_picker_media_first_batch, popup_media_first);
    const u32 gallery_media_end =
        maximum(gallery_media_first, minimum(renderer->media_picker_media_end_batch, popup_media_first));
    const bool friends_requests_active =
        !renderer->login_visible && !settings_active &&
        ((d3d11_renderer_active(renderer->friends_requests_scene)) ||
         d3d11_renderer_search_visible(*renderer) || d3d11_renderer_pinned_messages_visible(*renderer) ||
         d3d11_renderer_call_visible(*renderer));
    const u32 requests_foreground =
        friends_requests_active
            ? maximum(base_paint_end,
                      minimum(renderer->friends_requests_foreground_first_batch, paint_batch_count))
            : base_paint_end;
    const u32 requests_paint_end =
        friends_requests_active
            ? maximum(requests_foreground,
                      minimum(renderer->friends_requests_paint_end_batch, paint_batch_count))
            : base_paint_end;
    const u32 requests_media_first =
        friends_requests_active ? minimum(renderer->friends_requests_media_first_batch, gallery_media_first)
                                : gallery_media_first;
    const u32 requests_media_end =
        friends_requests_active
            ? maximum(requests_media_first,
                      minimum(renderer->friends_requests_media_end_batch, gallery_media_first))
            : gallery_media_first;
    const u32 requests_text_end =
        friends_requests_active
            ? maximum(base_text_end, minimum(renderer->friends_requests_text_end_batch, visible_text_end))
            : base_text_end;
    const u32 edit_overlay =
        renderer->login_visible || !renderer->content_text
            ? base_paint_end
            : maximum(media_overlay_begin,
                      minimum(base_paint_end, renderer->edit_overlay_paint_first_batch));
    const bool form_active =
        d3d11_renderer_tooltip_visible(*renderer) || d3d11_renderer_form_active(*renderer) ||
        settings_active;
    const u32 form_paint =
        form_active ? minimum(renderer->form_paint_first_batch, paint_batch_count) : paint_batch_count;
    const u32 form_text =
        form_active ? minimum(renderer->form_text_first_batch, visible_text_end) : visible_text_end;
    const u32 menu_paint = maximum(base_paint_end, minimum(renderer->menu_paint_first_batch, form_paint));
    const u32 menu_media =
        maximum(popup_media_first, minimum(renderer->menu_media_first_batch, ordinary_media_end));
    const u32 profile_effect_media =
        maximum(popup_media_first, minimum(renderer->profile_effect_media_first_batch, menu_media));
    const u32 menu_text = maximum(base_text_end, minimum(renderer->menu_text_first_batch, form_text));
    const u32 gallery_paint_first =
        maximum(requests_paint_end,
                minimum(renderer->media_picker_paint_first_batch, minimum(profile_foreground, menu_paint)));
    const u32 gallery_paint_end =
        maximum(gallery_paint_first,
                minimum(renderer->media_picker_paint_end_batch, minimum(profile_foreground, menu_paint)));
    const u32 gallery_text_first =
        maximum(requests_text_end, minimum(renderer->media_picker_text_first_batch, menu_text));
    const u32 gallery_text_end =
        maximum(gallery_text_first, minimum(renderer->media_picker_text_end_batch, menu_text));
    const bool gallery_current = d3d11_renderer_media_picker_visible(*renderer);
    const u32 gallery_foreground = maximum(
        gallery_paint_first, minimum(renderer->media_picker_foreground_first_batch, gallery_paint_end));
    const bool popup_current = d3d11_renderer_quick_switcher_visible(*renderer) ||
                               d3d11_renderer_inbox_visible(*renderer) ||
                               d3d11_renderer_message_forward_visible(*renderer) ||
                               d3d11_renderer_download_visible(*renderer) ||
                               d3d11_renderer_shortcuts_visible(*renderer) ||
                               (d3d11_renderer_active(renderer->popup_scene) &&
                                !renderer->login_visible && !settings_active);
    const bool choice_current = settings_active && d3d11_renderer_settings_popup(*renderer);
    const float popup_opacity = popup_current ? renderer->layer_opacity.popup : 1;
    // Scale popouts (the server header menu) grow from their anchor edge.
    const float popup_scale = popup_current ? renderer->popup_layer_scale : 1;
    const float popup_x = renderer->popup_layer_origin_x, popup_y = renderer->popup_layer_origin_y;
    const float profile_opacity = d3d11_renderer_active(renderer->profile_scene) &&
                                          !renderer->login_visible && !settings_active
                                      ? renderer->layer_opacity.profile
                                      : 1;
    const float choice_opacity = choice_current ? renderer->layer_opacity.settings_choice : 1;
    const float gallery_opacity = gallery_current ? renderer->layer_opacity.gallery : 1;
    const float page_opacity = settings_active ? renderer->layer_opacity.settings_page : 1;
    // Dialog entrance: the scrim fades in while the dialog fades and grows
    // from 90% about its centre, trailing the scrim slightly.
    const bool layer_entrance = settings_layer_entrance(*renderer);
    const bool modal_active = settings_modal_visible(*renderer) || layer_entrance;
    float modal_progress = modal_active ? renderer->settings_modal_progress : 1;
    modal_progress = modal_progress >= 0 && modal_progress <= 1 ? modal_progress : 1;
    // Layers fade in while settling from 110%; dialogs grow from 90%.
    const float modal_opacity = modal_progress >= 1 ? 1
                                : layer_entrance  ? modal_progress
                                                  : maximum(0.f, (modal_progress - .12f) / .88f);
    const float modal_scale = modal_progress >= 1 ? 1
                              : layer_entrance  ? 1.1f - .1f * modal_progress
                                                : .9f + .1f * modal_progress;
    const UiRect modal_rect = modal_active ? renderer->settings_scene->layout.modal : UiRect{};
    const float modal_x = (modal_rect.left + modal_rect.right) * .5f;
    const float modal_y = (modal_rect.top + modal_rect.bottom) * .5f;
    const u32 modal_paint_first = modal_active ? renderer->settings_modal_paint_first_batch : 0;
    const u32 modal_paint_end = modal_active ? renderer->settings_modal_paint_end_batch : 0;
    const u32 modal_text_end = modal_active ? renderer->settings_choice_text_first_batch : 0;
    const bool forum_current = !renderer->login_visible && renderer->forum_form &&
                               renderer->forum_form->scene && renderer->forum_form->scene->active;
    const float forum_opacity = forum_current ? renderer->layer_opacity.forum_form : 1;
    const float editor_opacity =
        renderer_theme_editor_visible(*renderer) ? renderer->layer_opacity.theme_editor : 1;
    const u32 editor_paint_first =
        maximum(menu_paint, minimum(renderer->theme_editor_paint_first_batch, form_paint));
    const u32 editor_paint_end =
        maximum(editor_paint_first, minimum(renderer->theme_editor_paint_end_batch, form_paint));
    const u32 editor_text_first =
        maximum(menu_text, minimum(renderer->theme_editor_text_first_batch, form_text));
    const u32 editor_text_end =
        maximum(editor_text_first, minimum(renderer->theme_editor_text_end_batch, form_text));
    // A server tag popout with a banner picture splits the popup layer:
    // paint up to its foreground, the banner media, then the rest.
    const bool card_split = !settings_active && d3d11_renderer_guild_card_visible(*renderer) &&
                            renderer->guild_card_foreground_first_batch >= menu_paint &&
                            renderer->guild_card_foreground_first_batch <= editor_paint_first &&
                            renderer->guild_card_banner_media_end_batch >= menu_media &&
                            renderer->guild_card_banner_media_end_batch <= ordinary_media_end;
    const u32 card_paint = card_split ? renderer->guild_card_foreground_first_batch : editor_paint_first;
    const u32 card_media = card_split ? renderer->guild_card_banner_media_end_batch : menu_media;
    const LayerBatchRange paint_layers[10]{
        {0, modal_paint_first, modal_progress},
        {modal_paint_first, modal_paint_end, modal_opacity, modal_scale, modal_x, modal_y},
        {renderer->profile_paint_first_batch, renderer->profile_paint_end_batch, profile_opacity},
        {gallery_paint_first, gallery_paint_end, gallery_opacity},
        {renderer->popup_paint_first_batch, renderer->popup_paint_end_batch, popup_opacity, popup_scale, popup_x,
         popup_y},
        {renderer->settings_choice_paint_first_batch, renderer->settings_choice_paint_end_batch,
         choice_opacity},
        {renderer->settings_page_paint_first_batch, renderer->settings_page_paint_end_batch, page_opacity},
        {renderer->theme_editor_paint_first_batch, renderer->theme_editor_paint_end_batch, editor_opacity},
        {renderer->quick_switcher_status_first_batch, renderer->quick_switcher_status_end_batch, popup_opacity,
         popup_scale, popup_x, popup_y},
        {forum_current ? form_paint : paint_batch_count,
         forum_current ? renderer->forum_paint_end_batch : paint_batch_count, forum_opacity}};
    const LayerBatchRange text_layers[8]{
        {0, modal_text_end, modal_opacity, modal_scale, modal_x, modal_y},
        {renderer->profile_text_first_batch, renderer->profile_text_end_batch, profile_opacity},
        {gallery_text_first, gallery_text_end, gallery_opacity},
        {renderer->popup_text_first_batch, renderer->popup_text_end_batch, popup_opacity, popup_scale, popup_x,
         popup_y},
        {renderer->settings_choice_text_first_batch, renderer->settings_choice_text_end_batch,
         choice_opacity},
        {renderer->settings_page_text_first_batch, renderer->settings_page_text_end_batch, page_opacity},
        {renderer->theme_editor_text_first_batch, renderer->theme_editor_text_end_batch, editor_opacity},
        {forum_current ? form_text : visible_text_end,
         forum_current ? renderer->forum_text_end_batch : visible_text_end, forum_opacity}};
    const LayerBatchRange media_layers[14]{
        // Settings avatars move with the modal and fade with the page, like their rows.
        {renderer->settings_page_media_first_batch, renderer->settings_page_media_end_batch, modal_opacity,
         modal_scale, modal_x, modal_y},
        {renderer->settings_page_media_first_batch, renderer->settings_page_media_end_batch, page_opacity},
        {renderer->profile_media_first_batch, renderer->profile_media_end_batch, profile_opacity},
        {renderer->inline_media_layer_first[static_cast<u32>(RendererInlineMediaLayer::profile)],
         renderer->inline_media_layer_end[static_cast<u32>(RendererInlineMediaLayer::profile)],
         profile_opacity},
        {gallery_media_first, gallery_media_end, gallery_opacity},
        {renderer->popup_scene_media_first_batch, renderer->popup_scene_media_end_batch, popup_opacity, popup_scale,
         popup_x, popup_y},
        {renderer->settings_choice_media_first_batch, renderer->settings_choice_media_end_batch,
         choice_opacity},
        {renderer->inline_media_layer_first[static_cast<u32>(RendererInlineMediaLayer::gallery)],
         renderer->inline_media_layer_end[static_cast<u32>(RendererInlineMediaLayer::gallery)],
         gallery_opacity},
        {renderer->inline_media_layer_first[static_cast<u32>(RendererInlineMediaLayer::popup)],
         renderer->inline_media_layer_end[static_cast<u32>(RendererInlineMediaLayer::popup)],
         popup_opacity, popup_scale, popup_x, popup_y},
        {renderer->inline_media_layer_first[static_cast<u32>(RendererInlineMediaLayer::settings_choice)],
         renderer->inline_media_layer_end[static_cast<u32>(RendererInlineMediaLayer::settings_choice)],
         choice_opacity},
        {renderer->inline_media_layer_first[static_cast<u32>(RendererInlineMediaLayer::settings_page)],
         renderer->inline_media_layer_end[static_cast<u32>(RendererInlineMediaLayer::settings_page)],
         page_opacity},
        {renderer->inline_media_layer_first[static_cast<u32>(RendererInlineMediaLayer::theme_editor)],
         renderer->inline_media_layer_end[static_cast<u32>(RendererInlineMediaLayer::theme_editor)],
         editor_opacity},
        {renderer->inline_media_layer_first[static_cast<u32>(RendererInlineMediaLayer::forum)],
         renderer->inline_media_layer_end[static_cast<u32>(RendererInlineMediaLayer::forum)],
         forum_opacity},
        {renderer->inline_media_layer_first[static_cast<u32>(RendererInlineMediaLayer::form)],
         renderer->inline_media_layer_end[static_cast<u32>(RendererInlineMediaLayer::form)], 1}};
    float current_opacity = 1;
    const auto paint = [&, renderer](u32 first, u32 end) noexcept {
        return draw_opacity_batches(
            renderer, first, end, paint_layers, current_opacity, [&, renderer](u32 begin, u32 limit) noexcept {
                return draw_paint_batches(renderer, &submission, begin, limit, pixel_scale, damage);
            });
    };
    const auto text = [&, renderer](u32 first, u32 end, bool upload) noexcept {
        return draw_opacity_batches(renderer, first, end, text_layers, current_opacity,
                                    [&, renderer](u32 begin, u32 limit) noexcept {
                                        return draw_gpu_text(renderer, &submission, theme, begin, limit, upload,
                                                             damage);
                                    });
    };
    const auto media = [&, renderer](u32 first, u32 end, bool upload) noexcept {
        return draw_opacity_batches(
            renderer, first, end, media_layers, current_opacity, [&, renderer](u32 begin, u32 limit) noexcept {
                const bool ok = draw_gpu_media(renderer, &submission, begin, limit, upload, damage);
                upload = false;
                return ok;
            });
    };
    const auto inline_media = [&, renderer](RendererInlineMediaLayer layer) noexcept {
        if (!inline_tail ||
            ((settings_active || layer != RendererInlineMediaLayer::base) && !interaction_text_current))
            return true;
        const u32 i = static_cast<u32>(layer);
        return media(renderer->inline_media_layer_first[i], renderer->inline_media_layer_end[i], false);
    };
    const auto forms = [&, renderer](u32 first_paint, u32 first_text, bool upload) noexcept {
        if (!forum_current)
            return paint(first_paint, paint_batch_count) && inline_media(RendererInlineMediaLayer::form) &&
                   text(first_text, visible_text_end, upload);
        const u32 forum_paint_end =
            maximum(first_paint, minimum(renderer->forum_paint_end_batch, paint_batch_count));
        const u32 forum_text_end =
            maximum(first_text, minimum(renderer->forum_text_end_batch, visible_text_end));
        return paint(first_paint, forum_paint_end) && text(first_text, forum_text_end, upload) &&
               inline_media(RendererInlineMediaLayer::forum) && paint(forum_paint_end, paint_batch_count) &&
               inline_media(RendererInlineMediaLayer::form) &&
               text(forum_text_end, visible_text_end, upload && forum_text_end == first_text);
    };
    // Materials and buffer bindings stay unchanged across text layers.
    if (renderer->text_effect_count && renderer->glyph_text.ready && glyph_count && visible_text_end &&
        !prepare_text_effects(renderer, effect_time_ms == ~0ull ? frame_ms : effect_time_ms))
        goto draw_failed;
    if (settings_active) {
        // The profile editor's preview card follows the page's own media; its
        // dialog follows the page's choice popup. Each draws paint, media,
        // foreground, text, effect, then controls, like the profile popout.
        const auto clamp_paint = [&](u32 batch) noexcept { return minimum(batch, paint_batch_count); };
        const auto clamp_text = [&](u32 batch) noexcept { return minimum(batch, visible_text_end); };
        const u32 preview_paint = clamp_paint(renderer->settings_preview_paint_first_batch);
        const u32 preview_foreground =
            maximum(preview_paint, clamp_paint(renderer->settings_preview_foreground_first_batch));
        const u32 preview_end =
            maximum(preview_foreground, clamp_paint(renderer->settings_preview_paint_end_batch));
        const u32 preview_controls =
            maximum(preview_foreground,
                    minimum(clamp_paint(renderer->settings_preview_controls_first_batch), preview_end));
        const u32 preview_effect = maximum(renderer->settings_preview_media_first_batch,
                                           minimum(renderer->settings_preview_effect_media_first_batch,
                                                   renderer->settings_preview_media_end_batch));
        const u32 page_paint_end =
            minimum(preview_paint, clamp_paint(renderer->settings_choice_paint_first_batch));
        const u32 choice_paint_end =
            maximum(preview_end, clamp_paint(renderer->settings_choice_paint_end_batch));
        const u32 dialog_paint =
            maximum(choice_paint_end, clamp_paint(renderer->settings_dialog_paint_first_batch));
        const u32 dialog_foreground =
            maximum(dialog_paint, clamp_paint(renderer->settings_dialog_foreground_first_batch));
        const u32 dialog_end =
            maximum(dialog_foreground, clamp_paint(renderer->settings_dialog_paint_end_batch));
        const u32 preview_text = clamp_text(renderer->settings_preview_text_first_batch);
        const u32 preview_text_end =
            maximum(preview_text, clamp_text(renderer->settings_preview_text_end_batch));
        const u32 page_text_end =
            minimum(preview_text, clamp_text(renderer->settings_choice_text_first_batch));
        const u32 choice_text_end =
            maximum(preview_text_end, clamp_text(renderer->settings_choice_text_end_batch));
        const u32 dialog_text =
            maximum(choice_text_end, clamp_text(renderer->settings_dialog_text_first_batch));
        const u32 dialog_text_end =
            maximum(dialog_text, clamp_text(renderer->settings_dialog_text_end_batch));
        if (!draw_settings_backdrop(renderer, &submission, theme, damage))
            goto draw_failed;
        if (!media(0, 0, upload_media) || !paint(0, page_paint_end) ||
            !inline_media(RendererInlineMediaLayer::base) ||
            !inline_media(RendererInlineMediaLayer::settings_page) || !text(0, page_text_end, true) ||
            !media(renderer->settings_page_media_first_batch, renderer->settings_page_media_end_batch,
                   false) ||
            !paint(preview_paint, preview_foreground) ||
            !media(renderer->settings_preview_media_first_batch, preview_effect, false) ||
            !paint(preview_foreground, preview_controls) || !text(preview_text, preview_text_end, false) ||
            !media(preview_effect, renderer->settings_preview_media_end_batch, false) ||
            !paint(preview_controls, preview_end) || !paint(preview_end, choice_paint_end) ||
            !inline_media(RendererInlineMediaLayer::settings_choice) ||
            !text(preview_text_end, choice_text_end, false) || !paint(dialog_paint, dialog_foreground) ||
            !media(renderer->settings_dialog_media_first_batch, renderer->settings_dialog_media_end_batch,
                   false) ||
            !paint(dialog_foreground, dialog_end) || !text(dialog_text, dialog_text_end, false) ||
            !forms(dialog_end, dialog_text_end, false))
            goto draw_failed;
    } else if (!paint(0, media_overlay_begin) || !media(0, requests_media_first, upload_media) ||
               !paint(media_overlay_begin, edit_overlay) || !inline_media(RendererInlineMediaLayer::base) ||
               !text(0, base_text_end, true) || !paint(edit_overlay, base_paint_end) ||
               !paint(base_paint_end, requests_foreground) ||
               !media(requests_media_first, requests_media_end, false) ||
               !paint(requests_foreground, requests_paint_end) ||
               !inline_media(RendererInlineMediaLayer::search) ||
               !text(base_text_end, requests_text_end, base_text_end == 0) ||
               !paint(requests_paint_end, gallery_paint_first) ||
               !media(requests_media_end, gallery_media_first, false) ||
               !text(requests_text_end, gallery_text_first, requests_text_end == 0) ||
               (gallery_current && !paint(gallery_paint_first, gallery_foreground)) ||
               (gallery_current && !media(gallery_media_first, gallery_media_end, false)) ||
               (gallery_current && !paint(gallery_foreground, gallery_paint_end)) ||
               (gallery_current && !inline_media(RendererInlineMediaLayer::gallery)) ||
               (gallery_current && !text(gallery_text_first, gallery_text_end, gallery_text_first == 0)) ||
               !paint(gallery_paint_end, minimum(profile_foreground, menu_paint)) ||
               !media(gallery_media_end, popup_media_first, false) ||
               !media(popup_media_first, profile_effect_media, false) ||
               !paint(minimum(profile_foreground, menu_paint), minimum(profile_controls, menu_paint)) ||
               !inline_media(RendererInlineMediaLayer::profile) ||
               !text(gallery_text_end, menu_text, gallery_text_end == 0) ||
               !media(profile_effect_media, menu_media, false) ||
               !paint(minimum(profile_controls, menu_paint), menu_paint) ||
               !paint(menu_paint, card_paint) || !media(menu_media, card_media, false) ||
               !paint(card_paint, editor_paint_first) || !media(card_media, ordinary_media_end, false) ||
               !inline_media(RendererInlineMediaLayer::popup) ||
               // Settings glyphs precede their popup. Earlier text ranges are empty
               // in this mode, so this first visible pass must admit the GPU upload.
               // draw_gpu_text skips it when the complete stream is already retained.
               !text(menu_text, editor_text_first, settings_active) ||
               !paint(editor_paint_first, editor_paint_end) ||
               !inline_media(RendererInlineMediaLayer::theme_editor) ||
               !text(editor_text_first, editor_text_end, false) || !paint(editor_paint_end, form_paint) ||
               !text(editor_text_end, form_text, false) || !forms(form_paint, form_text, true)) {
        goto draw_failed;
    }
    if (toast) {
        // renderer_set_toast_opacity keeps it within [0, 1].
        if (!bind_layer(renderer, current_opacity, renderer->toast_opacity, 1, 0, 0) ||
            !draw_paint_batches(renderer, &submission, paint_batch_count, stream_paint_batches, pixel_scale,
                                damage) ||
            !draw_gpu_text(renderer, &submission, theme, visible_text_end, stream_text_end, false, damage))
            goto draw_failed;
    }
    if ((current_opacity != 1 || renderer->draw_layer_scale != 1) &&
        !d3d11_draw_pipeline_update_opacity(renderer->pipeline, renderer->graphics.context, renderer->width,
                                            renderer->height, renderer->dpi, 1))
        goto draw_failed;
    renderer->draw_layer_scale = 1;
    release_stream_views(renderer, submission);
    saturating_accumulate(renderer->media_draws_total, renderer->media_draws_last_frame);
    d3d11_background_fx_end_frame(&renderer->background_fx, renderer->graphics.context);
    d3d11_gpu_timer_end(&renderer->gpu_timer, renderer->graphics.context, gpu_timestamp);
    renderer->draw_calls_max = maximum(renderer->draw_calls_max, renderer->draw_calls_last_frame);
    return true;

draw_failed:
    // A failed surface draw must not leave its factor bound for the next frame.
    if (current_opacity != 1 || renderer->draw_layer_scale != 1)
        d3d11_draw_pipeline_update_opacity(renderer->pipeline, renderer->graphics.context, renderer->width,
                                           renderer->height, renderer->dpi, 1);
    renderer->draw_layer_scale = 1;
    release_stream_views(renderer, submission);
    d3d11_background_fx_end_frame(&renderer->background_fx, renderer->graphics.context);
    d3d11_gpu_timer_end(&renderer->gpu_timer, renderer->graphics.context, gpu_timestamp);
    return false;
}

} // namespace maicord
