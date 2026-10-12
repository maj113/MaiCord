#pragma once
#include "probe_present.hpp"
#include "ui_surface_motion_measurement.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/ui/message_reaction_presentation.hpp"
#include "reaction_intent_ui_adapter.hpp"

namespace maicord {
bool probe_resize_surface(u32 width, u32 height) noexcept;
namespace reaction_intent_probe_detail {
inline constexpr u32 kRows = 4;
// Only the native projection lives here. The shipping App adapter owns the
// actual reaction controller and message/rich stores used by this timeline.
struct NativeFixture {
    u32 heights[kRows]{};
    u64 fenwick[kRows]{};
    UiVariableList list{};
    MessageTimeline timeline{};
    MessageScene scene{};
    UiWidget widgets[kRows * kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode nodes[kRows * kMessageSceneWidgetsPerRow + 1]{};
    MessageTextPresentation text{};
    MessageTextCommand commands[128]{};
    char bytes[8192]{};
    TextStyleSpan styles[128]{};
    MessageMarkdownSemantic semantics[64]{};
    MessageTextMeasurement measurements[8]{};
    MessageTextSpoilerReveal reveals[4]{};
    u32 markdown_scratch[8192]{};
    ~NativeFixture() noexcept {
        message_scene_release(&scene);
    }
    bool initialize(MessageStore* store, MessageRichStore* rich) noexcept {
        return store && rich && store->count && store->count <= kRows &&
               ui_variable_list_initialize(&list, {heights, kRows}, {fenwick, kRows}, store->count,
                                           ui_extent_units(80)) &&
               message_timeline_initialize(&timeline, store, &list, ui_extent_units(80),
                                           ui_extent_units(600), false) &&
               message_timeline_bind_rich_store(&timeline, rich) &&
               message_scene_initialize_owned(&scene, &timeline, kRows, 1,
                                              {widgets, kRows * kMessageSceneWidgetsPerRow + 1},
                                              {nodes, kRows * kMessageSceneWidgetsPerRow + 1}) &&
               message_text_presentation_initialize(&text, {commands, 128}, {bytes, sizeof(bytes)},
                                                    {styles, 128}, {semantics, 64}, {measurements, 8},
                                                    {reveals, 4}, {markdown_scratch, 8192});
    }
};
inline bool ink(const surface_motion_probe_detail::Readback& rb, UiRect rectangle, float scale,
                Color foreground, u32 minimum = 6, u32 tolerance = 48) noexcept {
    const auto region = raster_clip_rectangle(rectangle, scale, rb.width, rb.height);
    const u8 expected[]{static_cast<u8>(foreground.blue * 255.f + .5f),
                        static_cast<u8>(foreground.green * 255.f + .5f),
                        static_cast<u8>(foreground.red * 255.f + .5f)};
    u32 count{};
    for (i32 y = maximum(0, region.top); y < region.bottom; ++y)
        for (i32 x = maximum(0, region.left); x < region.right; ++x)
            if (surface_motion_probe_detail::distance(rb.images + (y * rb.width + x) * 4, expected) <=
                    tolerance &&
                ++count >= minimum)
                return true;
    return false;
}
inline bool read(D3D11Renderer& renderer, const Theme& theme,
                 surface_motion_probe_detail::Readback& rb) noexcept {
    return d3d11_renderer_draw_stream(&renderer, theme, false, false, nullptr) && rb.initialize(renderer) &&
           rb.read(renderer, 0);
}
inline bool toward_ink(const surface_motion_probe_detail::Readback& rb, UiRect area, float scale,
                       Color foreground, UiPoint background, u32 minimum = 6) noexcept {
    const auto region = raster_clip_rectangle(area, scale, rb.width, rb.height);
    const i32 bx = static_cast<i32>(background.x * scale), by = static_cast<i32>(background.y * scale);
    if (bx < 0 || by < 0 || bx >= static_cast<i32>(rb.width) || by >= static_cast<i32>(rb.height))
        return false;
    const auto* base = rb.images + (by * rb.width + bx) * 4;
    const u8 expected[]{static_cast<u8>(foreground.blue * 255.f + .5f),
                        static_cast<u8>(foreground.green * 255.f + .5f),
                        static_cast<u8>(foreground.red * 255.f + .5f)};
    float vector[3]{}, length = 0;
    for (u32 i = 0; i < 3; ++i) {
        vector[i] = static_cast<float>(expected[i]) - base[i];
        length += vector[i] * vector[i];
    }
    if (length < 1)
        return false;
    u32 count{};
    for (i32 y = maximum(0, region.top); y < region.bottom; ++y)
        for (i32 x = maximum(0, region.left); x < region.right; ++x) {
            const auto* pixel = rb.images + (y * rb.width + x) * 4;
            float coverage = 0;
            for (u32 i = 0; i < 3; ++i)
                coverage += (static_cast<float>(pixel[i]) - base[i]) * vector[i];
            coverage /= length;
            if (coverage < .15f || coverage > 1.05f)
                continue;
            coverage = coverage > 1.f ? 1.f : coverage;
            float residual = 0;
            for (u32 i = 0; i < 3; ++i) {
                const float difference = pixel[i] - (base[i] + coverage * vector[i]);
                residual += difference < 0 ? -difference : difference;
            }
            // Quantized pixels must lie near the actual foreground/fill blend
            // segment. A gray border cannot satisfy a selected blue accent.
            if (residual <= 24 && surface_motion_probe_detail::distance(pixel, base) >= 24 &&
                ++count >= minimum)
                return true;
        }
    return false;
}
inline bool pill_pixels(const NativeFixture& fixture, const D3D11Renderer& renderer, const Theme& theme,
                        const surface_motion_probe_detail::Readback& rb, u64 message, u64 emoji, u32 count,
                        bool mine) noexcept {
    const auto* rich = message_rich_store_find(*fixture.timeline.rich_store, message);
    const MessageSceneRowWidget* row{};
    for (u32 i = 0; i < fixture.scene.count; ++i)
        if (fixture.scene.rows[i].message_id == message)
            row = fixture.scene.rows.data + i;
    if (!rich || !row)
        return false;
    for (u32 index = 0; index < rich->reaction_count; ++index) {
        const auto& reaction = rich->reactions[index];
        if (reaction.emoji_id != emoji)
            continue;
        MessageReactionVisualItem visual{};
        if (reaction.count != count || ((reaction.flags & 1u) != 0) != mine ||
            !message_reaction_presentation_resolve(fixture.scene, *row, *rich, index, &visual) ||
            visual.selected != mine)
            return false;
        if (!ui_rect_has_area(visual.emoji_rectangle) ||
            visual.text_rectangle.left <= visual.emoji_rectangle.right)
            return false;
        // The optically lifted emoji reaches control.top + 3. Sample the
        // unobstructed emoji/count gap so neither raster check uses icon ink
        // as its interior fill reference.
        const UiPoint fill_sample{
            (visual.emoji_rectangle.right + visual.text_rectangle.left) * .5f,
            (visual.control_rectangle.top + visual.control_rectangle.bottom) * .5f};
        bool primitive{}, label{};
        for (u32 i = 0; i < renderer.paint_stream.count; ++i) {
            const auto& q = renderer.paint_stream.quads[i];
            if (q.border_width <= 0 ||
                !ui_rect_equal({q.left, q.top, q.right, q.bottom}, visual.control_rectangle))
                continue;
            if (mine && (q.border.red != theme.accent.red || q.border.green != theme.accent.green ||
                         q.border.blue != theme.accent.blue))
                continue;
            if (!mine && q.border.red == theme.accent.red && q.border.green == theme.accent.green &&
                q.border.blue == theme.accent.blue)
                continue;
            const auto visible = ui_rect_intersection(
                {q.left + q.radius, q.top, q.right - q.radius, q.top + 2}, fixture.scene.viewport);
            // A fractional one-DIP stroke spreads across two pixel rows. In
            // the actual 96-DPI image its .72/.28 coverage never reaches the
            // raw accent RGB. Require real raster movement toward that exact
            // semantic border from adjacent interior fill, not raw RGB ink.
            primitive = ui_rect_has_area(visible) && toward_ink(rb, visible, renderer.dpi / 96.f, q.border,
                                                                fill_sample, 6);
        }
        for (u32 i = 0; i < fixture.text.command_count; ++i) {
            const auto& command = fixture.text.commands[i];
            if (command.message_id != message ||
                command.measurement_kind != MessageTextMeasurementKind::reaction ||
                command.measurement_slot != index)
                continue;
            const auto visible = ui_rect_intersection(command.rectangle, command.clip);
            // Native digit 1 has thin antialiased stems (the captured stem is
            // ~.68 foreground coverage). Count six contrasted glyph pixels
            // within the actual text rectangle against the same pill fill.
            label = toward_ink(rb, visible, renderer.dpi / 96.f, command.color, fill_sample, 6);
        }
        if (!primitive)
            log_u64(LogLevel::error, "probe", "reaction_intent_pill_border_pixels_failed", emoji);
        if (!label)
            log_u64(LogLevel::error, "probe", "reaction_intent_pill_count_pixels_failed", emoji);
        if (!primitive || !label) {
            const i32 x = static_cast<i32>(fill_sample.x * renderer.dpi / 96.f);
            const i32 y = static_cast<i32>(fill_sample.y * renderer.dpi / 96.f);
            if (x >= 0 && y >= 0 && x < static_cast<i32>(rb.width) && y < static_cast<i32>(rb.height)) {
                const auto* pixel = rb.images + (y * rb.width + x) * 4;
                log_u64(LogLevel::error, "probe", "reaction_intent_pill_fill_sample_x", static_cast<u32>(x));
                log_u64(LogLevel::error, "probe", "reaction_intent_pill_fill_sample_y", static_cast<u32>(y));
                log_u64(LogLevel::error, "probe", "reaction_intent_pill_fill_sample_rgb",
                        (static_cast<u32>(pixel[2]) << 16) | (static_cast<u32>(pixel[1]) << 8) | pixel[0]);
            }
        }
        return primitive && label;
    }
    return false;
}
} // namespace reaction_intent_probe_detail

// Real shipping reaction host and activation policy, with an offline transport
// adapter. The renderer remains solely owned by the native bootstrap fixture.
inline bool reaction_intent_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache& measure, HWND window,
                                     bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept {
    using namespace reaction_intent_probe_detail;
    using Admission = app_detail::ApplicationMessageReactionEnqueueResult;
    if (!renderer.ui_scene || !renderer.message_scene || !renderer.message_text ||
        !renderer.navigation_view || !renderer.chat_state || !renderer.content_text || !window)
        return false;
    struct Context {
        D3D11Renderer& renderer;
        UiTextMeasureCache& measure;
        NativeFixture native{};
        NavigationView navigation{};
        Theme theme{kDefaultTheme};
        bool ready{};
        GuildViewRow guild_rows[2]{};
        ChannelViewRow channel_rows[4]{};
        u16 channel_scratch[4]{};
        u8 channel_visibility[4]{};
        ChannelSortKey channel_keys[4]{};
        bool project() noexcept {
            if (!ready)
                return true;
            auto* state = reaction_intent_ui_adapter_state();
            if (!state) {
                log_write(LogLevel::error, "probe", "reaction_intent_project_missing_state");
                return false;
            }
            const auto nav = navigation_view_rebuild(&navigation, *state);
            if (!nav.ok()) {
                log_u64(LogLevel::error, "probe", "reaction_intent_project_navigation_error",
                        static_cast<u64>(nav.error));
                return false;
            }
            auto metrics = kGuildChannelShellLayoutMetrics;
            if (state->home_selected) {
                metrics.flags = static_cast<u16>(metrics.flags & ~ui_shell_layout_members);
                metrics.member_sidebar_width = 0;
            }
            metrics.revision += static_cast<u32>(metrics.flags) * 7u;
            auto* shell = const_cast<UiScene*>(renderer.ui_scene);
            if (!ui_scene_layout_with_metrics(
                    shell,
                    state->home_selected ? UiShellView::private_channels : UiShellView::guild_channel,
                    metrics, renderer.width * 96.f / renderer.dpi, renderer.height * 96.f / renderer.dpi))
                return false;
            auto* rail = const_cast<GuildRailScene*>(renderer.guild_rail_scene);
            auto* sidebar = const_cast<ShellNavigationScene*>(renderer.shell_navigation_scene);
            if (!rail || !sidebar ||
                !guild_rail_scene_update(rail, &navigation,
                                         ui_scene_content_rectangle(*shell, UiSemantic::guild_navigation),
                                         rail->scroll_dips)
                     .ok() ||
                !shell_navigation_scene_update(
                     sidebar, &navigation,
                     ui_scene_content_rectangle(*shell, UiSemantic::channel_navigation),
                     sidebar->scroll_dips)
                     .ok()) {
                log_write(LogLevel::error, "probe", "reaction_intent_project_sidebar_failed");
                return false;
            }
            auto* composer = const_cast<MessageComposerScene*>(renderer.message_composer_scene);
            const auto* selected =
                navigation_view_find_channel(navigation, navigation.selected_channel_id);
            const MessageComposerProjectionContext composer_context{reaction_intent_ui_adapter_users(),
                                                                    state, navigation.selected_guild_id};
            if (!composer ||
                !message_composer_scene_update(
                     composer, composer->model, ui_scene_content_rectangle(*shell, UiSemantic::composer),
                     selected && (selected->actions & channel_action_send_messages) != 0, 0, {},
                     &composer_context)
                     .ok()) {
                log_write(LogLevel::error, "probe", "reaction_intent_project_composer_failed");
                return false;
            }
            const auto updated = message_scene_update(
                &native.scene, ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::message_view));
            if (!updated.ok()) {
                log_u64(LogLevel::error, "probe", "reaction_intent_project_scene_error",
                        static_cast<u64>(updated.error));
                return false;
            }
            if (!renderer_bind_message_scene(&renderer, &native.scene)) {
                log_write(LogLevel::error, "probe", "reaction_intent_project_scene_bind_failed");
                return false;
            }
            // Binding requires an unbound bank. Do this only at the actual
            // fixture handoff, preserving retained text on unchanged frames.
            if (renderer.message_text != &native.text) {
                renderer_unbind_message_text_presentation(&renderer);
                if (!renderer_bind_message_text_presentation(&renderer, &native.text)) {
                    log_write(LogLevel::error, "probe", "reaction_intent_project_text_bind_failed");
                    return false;
                }
            }
            renderer.chat_state = state;
            renderer.user_store = reaction_intent_ui_adapter_users();
            renderer.navigation_view = &navigation;
            renderer.message_measure_cache = &measure;
            renderer.popup_scene = nullptr;
            const auto converged = message_text_coordinator_converge(
                {&native.scene, &native.text, &measure, state, renderer.user_store, &navigation, &theme,
                 +[](void* owner, const Theme& current) noexcept {
                     return renderer_rebuild_content_text(static_cast<D3D11Renderer*>(owner), current);
                 },
                 &renderer, 1'759'667'400ull, static_cast<u16>(renderer.dpi), nullptr});
            if (!converged.ok()) {
                log_u64(LogLevel::error, "probe", "reaction_intent_project_coordinator_error",
                        static_cast<u64>(converged.error));
                log_u64(LogLevel::error, "probe", "reaction_intent_project_coordinator_scene_error",
                        static_cast<u64>(converged.scene_error));
                log_u64(LogLevel::error, "probe", "reaction_intent_project_presentation_error",
                        static_cast<u64>(converged.presentation_error));
                log_u64(LogLevel::error, "probe", "reaction_intent_project_passes", converged.passes);
                log_u64(LogLevel::error, "probe", "reaction_intent_project_text_failure_stage",
                        static_cast<u64>(native.text.failure_stage));
                log_u64(LogLevel::error, "probe", "reaction_intent_project_text_failure_operation",
                        static_cast<u64>(native.text.failure_operation));
            }
            return converged.ok();
        }
    } context{renderer, measure};
    const auto* old_scene = renderer.message_scene;
    const auto* old_text = renderer.message_text;
    const auto* old_state = renderer.chat_state;
    const auto* old_users = renderer.user_store;
    const auto* old_navigation = renderer.navigation_view;
    const auto* old_popup = renderer.popup_scene;
    const auto* old_focus = renderer.focus_owner;
    auto* old_cache = renderer.message_measure_cache;
    const u32 width = renderer.width, height = renderer.height, dpi = renderer.dpi;
    struct Restore {
        Context& c;
        const MessageScene* scene;
        const MessageTextPresentation* text;
        const ChatStateStore* state;
        const UserStore* users;
        const NavigationView* navigation;
        const PopupScene* popup;
        const UiWidgetManager* focus;
        UiTextMeasureCache* cache;
        ~Restore() noexcept {
            c.ready = false;
            auto& r = c.renderer;
            r.message_scene = scene;
            r.message_text = text;
            r.chat_state = state;
            r.user_store = users;
            r.navigation_view = navigation;
            r.message_measure_cache = cache;
            r.focus_owner = focus;
            renderer_bind_popup_scene(&r, popup);
            if (r.guild_rail_scene)
                guild_rail_scene_update(
                    const_cast<GuildRailScene*>(r.guild_rail_scene), navigation,
                    ui_scene_content_rectangle(*r.ui_scene, UiSemantic::guild_navigation), 0);
            if (r.shell_navigation_scene)
                shell_navigation_scene_update(
                    const_cast<ShellNavigationScene*>(r.shell_navigation_scene), navigation,
                    ui_scene_content_rectangle(*r.ui_scene, UiSemantic::channel_navigation), 0);
            reaction_intent_ui_adapter_end();
            r.retained_presentation.valid = false;
            content_text_retention_invalidate(&r.content_text_retention);
        }
    } restore{context,        old_scene, old_text,  old_state, old_users,
              old_navigation, old_popup, old_focus, old_cache};
    const auto require = [](bool ok, u32 stage) noexcept {
        if (!ok)
            log_u64(LogLevel::error, "probe", "reaction_intent_native_failure_stage", stage);
        return ok;
    };
    if (!require(reaction_intent_ui_adapter_begin(
                     window, +[](void* owner) noexcept { return static_cast<Context*>(owner)->project(); },
                     &context) &&
                     context.native.initialize(reaction_intent_ui_adapter_store(),
                                               reaction_intent_ui_adapter_rich_store()),
                 1))
        return false;
    auto* store = reaction_intent_ui_adapter_store();
    const auto handle = message_store_ordered_handle(*store, 0);
    const auto* record = message_store_resolve(*store, handle);
    if (!require(record != nullptr, 2))
        return false;
    const u64 message = record->id, channel = record->channel_id;
    if (!require(navigation_view_initialize(&context.navigation, {context.guild_rows, 2},
                                                    {context.channel_rows, 4}, {context.channel_scratch, 4},
                                                    {context.channel_visibility, 4},
                                                    {context.channel_keys, 4}),
                 26))
        return false;
    context.ready = true;
    renderer.focus_owner = nullptr;
    const auto resize = [&](u32 w, u32 h) noexcept {
        // Bootstrap resizing republishes its own main scene. Keep that helper
        // on its own borrowed model, then bind the actual reaction App bank.
        renderer.chat_state = old_state;
        renderer.user_store = old_users;
        renderer.navigation_view = old_navigation;
        if (!probe_resize_surface(w, h)) {
            log_write(LogLevel::error, "probe", "reaction_intent_bootstrap_resize_failed");
            return false;
        }
        return context.project();
    };
    const auto show = [&](const wchar_t* name) noexcept {
        return context.project() && probe_present(&renderer, context.theme) &&
               (!capture || capture(name, context.theme));
    };
    const auto reaction_state = [&](u64 emoji, u32 count, bool mine) noexcept {
        const auto* rich = message_rich_store_find(*reaction_intent_ui_adapter_rich_store(), message);
        if (!rich)
            return false;
        for (u32 i = 0; i < rich->reaction_count; ++i)
            if (rich->reactions[i].emoji_id == emoji)
                return rich->reactions[i].count == count &&
                       (((rich->reactions[i].flags & 1u) != 0) == mine);
        return !count && !mine;
    };
    const auto toggle_pill = [&](u64 emoji) noexcept {
        if (!context.project())
            return Admission::invalid;
        const auto* row = message_scene_find_row(context.native.scene, 0);
        const auto* rich = message_rich_store_find(*reaction_intent_ui_adapter_rich_store(), message);
        if (!row || !rich) {
            log_write(LogLevel::error, "probe", "reaction_intent_pill_missing_row_or_rich");
            return Admission::invalid;
        }
        for (u32 i = 0; i < rich->reaction_count; ++i)
            if (rich->reactions[i].emoji_id == emoji) {
                MessageReactionVisualItem visual{};
                u32 hit_index{};
                if (!message_reaction_presentation_resolve(context.native.scene, *row, *rich, i, &visual)) {
                    log_write(LogLevel::error, "probe", "reaction_intent_pill_geometry_failed");
                    return Admission::invalid;
                }
                if (!message_scene_reaction_index(
                        context.native.scene,
                        message_scene_hit_test(
                            context.native.scene,
                            (visual.control_rectangle.left + visual.control_rectangle.right) * .5f,
                            (visual.control_rectangle.top + visual.control_rectangle.bottom) * .5f),
                        &hit_index) ||
                    hit_index != i) {
                    log_write(LogLevel::error, "probe", "reaction_intent_pill_native_hit_failed");
                    return Admission::invalid;
                }
                return reaction_intent_ui_adapter_toggle(message, hit_index);
            }
        return Admission::invalid;
    };

    // The shared emoji picker's reaction popout admits through this owner.
    const auto react = [&](u64 emoji) noexcept {
        const auto admission = reaction_intent_ui_adapter_add(message, emoji);
        return (admission == Admission::accepted || admission == Admission::coalesced) && context.project();
    };
    const auto warm = [&](u32 case_index) noexcept {
        const auto& scene = context.native.scene;
        const u32 resolves = scene.rebuilds, layouts = scene.layout_passes,
                  prepares = scene.preparation_passes, shape = measure.backend_calls, raster = renderer.glyph_text.cache_builds,
                  builds = renderer.content_text->rebuilds, pu = renderer.paint_instance_uploads,
                  tu = renderer.text_instance_uploads, mu = renderer.media_instance_uploads,
                  draws = renderer.painted_frames, skips = renderer.unchanged_present_skips;
        const auto rich_updated = reaction_intent_ui_adapter_rich_store()->updated;
        const auto owner = reaction_intent_ui_adapter_metrics();
        const u32 shell_layout = renderer.ui_scene->layout_passes,
                  shell_rebuilds = renderer.ui_scene->layout_rebuilds,
                  rail_layout = renderer.guild_rail_scene->layout_passes,
                  rail_rebuilds = renderer.guild_rail_scene->rebuilds,
                  sidebar_layout = renderer.shell_navigation_scene->layout_passes,
                  sidebar_rebuilds = renderer.shell_navigation_scene->rebuilds,
                  composer_layout = renderer.message_composer_scene->layout_passes,
                  nav_revision = context.navigation.revision;
        for (u32 i = 0; i < 64; ++i)
            if (!reaction_intent_ui_adapter_synchronize() || !context.project() ||
                !probe_present(&renderer, context.theme))
                return false;
        const u32 values[]{scene.rebuilds - resolves,
                           scene.layout_passes - layouts,
                           scene.preparation_passes - prepares,
                           measure.backend_calls - shape,
                           renderer.glyph_text.cache_builds - raster,
                           renderer.content_text->rebuilds - builds,
                           renderer.paint_instance_uploads - pu,
                           renderer.text_instance_uploads - tu,
                           renderer.media_instance_uploads - mu,
                           renderer.painted_frames - draws,
                           renderer.unchanged_present_skips - skips,
                           renderer.ui_scene->layout_passes - shell_layout,
                           renderer.ui_scene->layout_rebuilds - shell_rebuilds,
                           renderer.guild_rail_scene->layout_passes - rail_layout,
                           renderer.guild_rail_scene->rebuilds - rail_rebuilds,
                           renderer.shell_navigation_scene->layout_passes - sidebar_layout,
                           renderer.shell_navigation_scene->rebuilds - sidebar_rebuilds,
                           renderer.message_composer_scene->layout_passes - composer_layout,
                           context.navigation.revision - nav_revision};
        const char* names[]{"reaction_intent_warm_resolve",
                            "reaction_intent_warm_layout",
                            "reaction_intent_warm_preparation",
                            "reaction_intent_warm_shape",
                            "reaction_intent_warm_raster",
                            "reaction_intent_warm_text_build",
                            "reaction_intent_warm_paint_upload",
                            "reaction_intent_warm_text_upload",
                            "reaction_intent_warm_media_upload",
                            "reaction_intent_warm_draw",
                            "reaction_intent_warm_skips",
                            "reaction_intent_warm_shell_layout",
                            "reaction_intent_warm_shell_rebuild",
                            "reaction_intent_warm_rail_layout",
                            "reaction_intent_warm_rail_rebuild",
                            "reaction_intent_warm_sidebar_layout",
                            "reaction_intent_warm_sidebar_rebuild",
                            "reaction_intent_warm_composer_layout",
                            "reaction_intent_warm_navigation_revision"};
        log_u64(LogLevel::info, "probe", "reaction_intent_warm_case", case_index);
        const auto current_owner = reaction_intent_ui_adapter_metrics();
        bool valid = reaction_intent_ui_adapter_rich_store()->updated == rich_updated &&
                     owner.owner_bytes == current_owner.owner_bytes &&
                     owner.owner_committed_bytes == current_owner.owner_committed_bytes &&
                     owner.intent_bytes == current_owner.intent_bytes &&
                     owner.intent_committed_bytes == current_owner.intent_committed_bytes &&
                     owner.intent_count == current_owner.intent_count &&
                     owner.backed_slots == current_owner.backed_slots &&
                     owner.revision == current_owner.revision && owner.active == current_owner.active &&
                     owner.flight_active == current_owner.flight_active;
        for (u32 i = 0; i < 19; ++i) {
            log_u64(LogLevel::info, "probe", names[i], values[i]);
            valid &= values[i] == (i == 10 ? 64u : 0u);
        }
        log_u64(LogLevel::info, "probe", "reaction_intent_warm_native_widgets", scene.widgets.count);
        log_u64(LogLevel::info, "probe", "reaction_intent_warm_native_rows_committed",
                scene.owned_rows.capacity);
        log_u64(LogLevel::info, "probe", "reaction_intent_warm_model_records", store->count);
        log_u64(LogLevel::info, "probe", "reaction_intent_warm_owner_bytes", owner.owner_bytes);
        log_u64(LogLevel::info, "probe", "reaction_intent_warm_owner_committed_bytes",
                owner.owner_committed_bytes);
        log_u64(LogLevel::info, "probe", "reaction_intent_warm_intent_bytes", owner.intent_bytes);
        log_u64(LogLevel::info, "probe", "reaction_intent_warm_intent_committed_bytes",
                owner.intent_committed_bytes);
        log_u64(LogLevel::info, "probe", "reaction_intent_warm_intent_count", owner.intent_count);
        log_u64(LogLevel::info, "probe", "reaction_intent_warm_backed_slots", owner.backed_slots);
        log_u64(LogLevel::info, "probe", "reaction_intent_warm_owner_revision", owner.revision);
        return valid;
    };
    renderer_set_dpi(&renderer, 96);
    if (!require(resize(1100, 760), 3))
        return false;
    const u64 emoji_ids[]{101, 102};
    for (u64 id : emoji_ids) {
        u8 pixels[16 * 16 * 4]{};
        for (u32 y = 0; y < 16; ++y)
            for (u32 x = 0; x < 16; ++x) {
                const u32 at = (y * 16 + x) * 4;
                const bool bright = (x / 4 + y / 4) % 2 == 0;
                pixels[at] = id == 101 ? (bright ? 230 : 170) : 55;
                pixels[at + 1] = bright ? 165 : 100;
                pixels[at + 2] = id == 102 ? 220 : 65;
                pixels[at + 3] = 255;
            }
        if (!require(renderer_upload_media_straight_rgba8(&renderer, media_asset_custom_emoji(id, false),
                                                          16, 16, 64, {pixels, sizeof(pixels)})
                         .ok(),
                     4))
            return false;
    }
    const auto first_toggle = toggle_pill(101);
    log_u64(LogLevel::info, "probe", "reaction_intent_first_toggle_admission",
            static_cast<u64>(first_toggle));
    const auto* initial_channel =
        chat_state_store_find_channel(*reaction_intent_ui_adapter_state(), channel);
    log_u64(LogLevel::info, "probe", "reaction_intent_initial_channel_actions",
            initial_channel ? initial_channel->actions : 0);
    log_u64(LogLevel::info, "probe", "reaction_intent_initial_channel_type",
            initial_channel ? initial_channel->type : 0);
    if (!require(first_toggle == Admission::accepted, 51))
        return false;
    if (!require(reaction_state(101, 3, true), 52))
        return false;
    if (!require(reaction_intent_ui_adapter_submit(1001), 53))
        return false;
    if (!require(react(102) && react(101) && reaction_state(101, 3, true) && reaction_state(102, 1, true) &&
                     show(L"reaction-intent-queued.bmp"),
                 6))
        return false;
    {
        surface_motion_probe_detail::Readback rb{};
        if (!require(read(renderer, context.theme, rb), 71) ||
            !require(pill_pixels(context.native, renderer, context.theme, rb, message, 101, 3, true), 72) ||
            !require(pill_pixels(context.native, renderer, context.theme, rb, message, 102, 1, true), 73))
            return false;
    }
    if (!require(context.project() && probe_present(&renderer, context.theme) && warm(1), 8))
        return false;
    if (!require(react(101) && toggle_pill(101) == Admission::coalesced && reaction_state(101, 2, false) &&
                     reaction_intent_ui_adapter_response(204),
                 9))
        return false;
    GatewayMessageReactionEvent old_echo{message,
                                         channel,
                                         reaction_intent_ui_adapter_state()->self_user_id,
                                         101,
                                         utf8_literal("native_orbit"),
                                         MessageReactionType::normal,
                                         GatewayMessageReactionEventKind::add,
                                         false};
    reaction_intent_ui_adapter_observe(old_echo);
    reaction_intent_ui_adapter_observe(old_echo);
    if (!require(reaction_state(101, 2, false), 10))
        return false;
    {
        surface_motion_probe_detail::Readback rb{};
        if (!require(context.project() && probe_present(&renderer, context.theme) &&
                         read(renderer, context.theme, rb) &&
                         pill_pixels(context.native, renderer, context.theme, rb, message, 101, 2, false),
                     27))
            return false;
    }
    bool failed = false;
    for (u32 i = 0; i < 2; ++i) {
        u64 flight{}, target_message{}, emoji{};
        bool add{};
        if (!require(reaction_intent_ui_adapter_submit(1002 + i) &&
                         reaction_intent_ui_adapter_flight(&flight, &target_message, &emoji, &add) &&
                         target_message == message,
                     11))
            return false;
        if (emoji == 102 && add) {
            auto event = old_echo;
            event.emoji_id = 102;
            event.name = utf8_literal("native_wave");
            reaction_intent_ui_adapter_observe(event);
            if (!require(reaction_intent_ui_adapter_response(204) && reaction_state(102, 1, true), 12))
                return false;
        } else if (emoji == 101 && !add) {
            bool error = false;
            if (!require(reaction_intent_ui_adapter_response(403) && reaction_state(101, 3, true) &&
                             app_detail::application_message_reaction_status(message, &error).bytes && error &&
                             show(L"reaction-intent-rollback.bmp"),
                         13))
                return false;
            surface_motion_probe_detail::Readback rb{};
            if (!require(read(renderer, context.theme, rb) &&
                             pill_pixels(context.native, renderer, context.theme, rb, message, 101, 3, true),
                         14))
                return false;
            failed = true;
        } else
            return require(false, 15);
    }
    if (!require(failed && reaction_state(101, 3, true) && reaction_state(102, 1, true), 16))
        return false;
    if (!require(resize(640, 600) && react(102) && react(101) && reaction_state(101, 3, true) &&
                     reaction_state(102, 1, true) && show(L"reaction-intent-narrow.bmp"),
                 20))
        return false;
    context.theme = kLightTheme;
    renderer_set_dpi(&renderer, 144);
    if (!require(resize(1440, 1080) && context.project() && show(L"reaction-intent-light-dpi144.bmp"), 22))
        return false;
    {
        surface_motion_probe_detail::Readback rb{};
        if (!require(read(renderer, context.theme, rb) &&
                         pill_pixels(context.native, renderer, context.theme, rb, message, 101, 3, true) &&
                         pill_pixels(context.native, renderer, context.theme, rb, message, 102, 1, true),
                     23))
            return false;
    }
    if (!require(context.project() && probe_present(&renderer, context.theme) && warm(2), 24))
        return false;
    context.ready = false;
    renderer_set_dpi(&renderer, dpi);
    renderer.chat_state = old_state;
    renderer.user_store = old_users;
    renderer.navigation_view = old_navigation;
    if (!require(probe_resize_surface(width, height), 25))
        return false;
    log_write(LogLevel::info, "probe",
              "reaction_intent_shipping_host_dispatch_late_echo_rollback_source_pixels_two64_passed");
    return true;
}
} // namespace maicord
