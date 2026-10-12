#pragma once
#include "probe_present.hpp"
#include "ui_surface_motion_measurement.hpp"
#include "thread_membership_ui_adapter.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/ui/thread_header.hpp"
#include "maicord/ui/layout_debug.hpp"
#include "maicord/media/asset.hpp"
#include "application_thread_header.hpp"

namespace maicord {
namespace thread_membership_probe_detail {
inline constexpr u32 kRows = 4;
// Native projection only: all authoritative state and host actions belong to
// the real App adapter. No independent GPU owner or per-card worker exists.
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
    ~NativeFixture() noexcept { message_scene_release(&scene); }
    bool initialize(MessageStore* store, MessageRichStore* rich) noexcept {
        return store && rich && store->count && store->count <= kRows &&
            ui_variable_list_initialize(&list, {heights, kRows}, {fenwick, kRows}, store->count,
                ui_extent_units(80)) &&
            message_timeline_initialize(&timeline, store, &list, ui_extent_units(80),
                ui_extent_units(600), false) && message_timeline_bind_rich_store(&timeline, rich) &&
            message_scene_initialize_owned(&scene, &timeline, kRows, 1,
                {widgets, kRows * kMessageSceneWidgetsPerRow + 1},
                {nodes, kRows * kMessageSceneWidgetsPerRow + 1}) &&
            message_text_presentation_initialize(&text, {commands, 128}, {bytes, sizeof(bytes)},
                {styles, 128}, {semantics, 64}, {measurements, 8}, {reveals, 4},
                {markdown_scratch, 8192});
    }
};

inline bool blended_ink(const surface_motion_probe_detail::Readback& readback, UiRect rectangle,
    float scale, Color foreground, UiPoint fill_sample, u32 minimum_pixels = 6) noexcept {
    const auto region = raster_clip_rectangle(rectangle, scale, readback.width, readback.height);
    const i32 sample_x = static_cast<i32>(fill_sample.x * scale);
    const i32 sample_y = static_cast<i32>(fill_sample.y * scale);
    if (sample_x < 0 || sample_y < 0 || sample_x >= static_cast<i32>(readback.width) ||
        sample_y >= static_cast<i32>(readback.height))
        return false;
    const auto* background = readback.images + (sample_y * readback.width + sample_x) * 4;
    const u8 expected[]{static_cast<u8>(foreground.blue * 255.f + .5f),
        static_cast<u8>(foreground.green * 255.f + .5f),
        static_cast<u8>(foreground.red * 255.f + .5f)};
    float vector[3]{}, length = 0;
    for (u32 component = 0; component < 3; ++component) {
        vector[component] = static_cast<float>(expected[component]) - background[component];
        length += vector[component] * vector[component];
    }
    if (length < 1)
        return false;
    u32 pixels = 0;
    for (i32 y = maximum(0, region.top); y < region.bottom; ++y)
        for (i32 x = maximum(0, region.left); x < region.right; ++x) {
            const auto* pixel = readback.images + (y * readback.width + x) * 4;
            float coverage = 0;
            for (u32 component = 0; component < 3; ++component)
                coverage += (static_cast<float>(pixel[component]) - background[component]) * vector[component];
            coverage /= length;
            if (coverage < .15f || coverage > 1.05f)
                continue;
            coverage = coverage > 1.f ? 1.f : coverage;
            float residual = 0;
            for (u32 component = 0; component < 3; ++component) {
                const float difference = pixel[component] -
                    (background[component] + coverage * vector[component]);
                residual += difference < 0 ? -difference : difference;
            }
            // Native antialiasing must follow the authored foreground hue,
            // including negative Light-theme vectors. Contrast alone cannot
            // mistake an unrelated gray edge for a colored action.
            if (residual <= 24 && surface_motion_probe_detail::distance(pixel, background) >= 24 &&
                ++pixels >= minimum_pixels)
                return true;
        }
    return false;
}
} // namespace thread_membership_probe_detail

bool probe_resize_surface(u32 width, u32 height) noexcept;

inline bool thread_membership_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache& measure, HWND window,
    bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept {
    using namespace thread_membership_probe_detail;
    using namespace app_detail;
    using Admission = ApplicationThreadActionEnqueueResult;
    if (!renderer.ui_scene || !renderer.message_scene || !renderer.message_text ||
        !renderer.navigation_view || !renderer.chat_state || !renderer.content_text || !window)
        return false;
    struct Context {
        D3D11Renderer& renderer;
        UiTextMeasureCache& measure;
        NativeFixture native{};
        NavigationView navigation{};
        GuildViewRow guilds[2]{};
        ChannelViewRow channels[8]{};
        u16 scratch[8]{};
        u8 visibility[8]{};
        ChannelSortKey keys[8]{};
        Theme theme{kDefaultTheme};
        bool ready{};
        bool project() noexcept {
            if (!ready) return true;
            auto* state = thread_membership_ui_adapter_state();
            auto* users = thread_membership_ui_adapter_users();
            auto* header = thread_membership_ui_adapter_header();
            auto* shell = const_cast<UiScene*>(renderer.ui_scene);
            auto metrics = kGuildChannelShellLayoutMetrics;
            metrics.flags = static_cast<u16>(metrics.flags & ~ui_shell_layout_members);
            metrics.member_sidebar_width = 0;
            metrics.revision += static_cast<u32>(metrics.flags) * 7u;
            if (!state || !users || !header || !navigation_view_rebuild(&navigation, *state).ok() ||
                !ui_scene_layout_with_metrics(shell, UiShellView::guild_channel, metrics,
                    renderer.width * 96.f / renderer.dpi, renderer.height * 96.f / renderer.dpi)) return false;
            auto* rail = const_cast<GuildRailScene*>(renderer.guild_rail_scene);
            auto* sidebar = const_cast<ShellNavigationScene*>(renderer.shell_navigation_scene);
            auto* composer = const_cast<MessageComposerScene*>(renderer.message_composer_scene);
            const auto* selected = navigation_view_find_channel(navigation, navigation.selected_channel_id);
            const MessageComposerProjectionContext composer_context{users, state, navigation.selected_guild_id};
            if (!rail || !sidebar || !composer ||
                !guild_rail_scene_update(rail, &navigation,
                    ui_scene_content_rectangle(*shell, UiSemantic::guild_navigation), rail->scroll_dips).ok() ||
                !shell_navigation_scene_update(sidebar, &navigation,
                    ui_scene_content_rectangle(*shell, UiSemantic::channel_navigation), sidebar->scroll_dips).ok() ||
                !message_composer_scene_update(composer, composer->model,
                    ui_scene_content_rectangle(*shell, UiSemantic::composer),
                    selected && (selected->actions & channel_action_send_messages), 0, {}, &composer_context).ok())
                return false;
            UiRect available = ui_scene_content_rectangle(*shell, UiSemantic::message_view);
            const float occupied = application_thread_header_project(header, shell,
                state->selected_channel_id, available, available);
            thread_membership_ui_adapter_bind_header(header,
                {0, 0, renderer.width * 96.f / renderer.dpi, renderer.height * 96.f / renderer.dpi});
            available.top = minimum(available.bottom, available.top + occupied);
            if (!application_thread_actions_channel_readable(state->selected_channel_id)) available = {};
            if (!header->active || !renderer_bind_thread_header(&renderer, header) ||
                !message_scene_update(&native.scene, available).ok() ||
                !renderer_bind_message_scene(&renderer, &native.scene)) return false;
            if (renderer.message_text != &native.text) {
                renderer_unbind_message_text_presentation(&renderer);
                if (!renderer_bind_message_text_presentation(&renderer, &native.text)) return false;
            }
            renderer.chat_state = state;
            renderer.user_store = users;
            renderer.navigation_view = &navigation;
            renderer.message_measure_cache = &measure;
            if (!renderer_bind_popup_scene(&renderer, thread_membership_ui_adapter_popup())) return false;
            const auto result = message_text_coordinator_converge({&native.scene, &native.text, &measure,
                state, users, &navigation, &theme,
                +[](void* owner, const Theme& current) noexcept {
                    return renderer_rebuild_content_text(static_cast<D3D11Renderer*>(owner), current);
                }, &renderer, 1'759'667'400ull, static_cast<u16>(renderer.dpi), nullptr});
            if (!result.ok()) log_u64(LogLevel::error, "probe", "thread_membership_coordinator_error",
                static_cast<u64>(result.error));
            return result.ok();
        }
    } context{renderer, measure};
    const auto* old_scene = renderer.message_scene;
    const auto* old_text = renderer.message_text;
    const auto* old_state = renderer.chat_state;
    const auto* old_users = renderer.user_store;
    const auto* old_navigation = renderer.navigation_view;
    const auto* old_popup = renderer.popup_scene;
    const auto* old_header = renderer.thread_header;
    const auto* old_focus = renderer.focus_owner;
    auto* old_cache = renderer.message_measure_cache;
    const u32 width = renderer.width, height = renderer.height, dpi = renderer.dpi;
    struct Restore {
        Context& c;
        const MessageScene* scene; const MessageTextPresentation* text;
        const ChatStateStore* state; const UserStore* users;
        const NavigationView* navigation; const PopupScene* popup;
        const UiThreadHeader* header; const UiWidgetManager* focus; UiTextMeasureCache* cache;
        ~Restore() noexcept {
            c.ready = false;
            auto& r = c.renderer;
            if (auto* active = thread_membership_ui_adapter_header())
                ui_thread_header_update(active, {}, {}, {}, ui_thread_header_style());
            r.message_scene = scene; r.message_text = text; r.chat_state = state; r.user_store = users;
            r.navigation_view = navigation; r.message_measure_cache = cache; r.focus_owner = focus;
            renderer_bind_thread_header(&r, header); renderer_bind_popup_scene(&r, popup);
            if (r.guild_rail_scene) guild_rail_scene_update(const_cast<GuildRailScene*>(r.guild_rail_scene),
                navigation, ui_scene_content_rectangle(*r.ui_scene, UiSemantic::guild_navigation), 0);
            if (r.shell_navigation_scene) shell_navigation_scene_update(const_cast<ShellNavigationScene*>(r.shell_navigation_scene),
                navigation, ui_scene_content_rectangle(*r.ui_scene, UiSemantic::channel_navigation), 0);
            thread_membership_ui_adapter_end();
            r.retained_presentation.valid = false;
            content_text_retention_invalidate(&r.content_text_retention);
        }
    } restore{context, old_scene, old_text, old_state, old_users, old_navigation, old_popup, old_header, old_focus, old_cache};
    const auto require = [](bool ok, u32 stage) noexcept {
        if (!ok) log_u64(LogLevel::error, "probe", "thread_membership_native_failure_stage", stage);
        return ok;
    };
    if (!require(thread_membership_ui_adapter_begin(window, +[](void* owner) noexcept {
            return static_cast<Context*>(owner)->project();
        }, &context) && context.native.initialize(thread_membership_ui_adapter_store(), thread_membership_ui_adapter_rich_store()) &&
        navigation_view_initialize(&context.navigation, {context.guilds, 2}, {context.channels, 8},
            {context.scratch, 8}, {context.visibility, 8}, {context.keys, 8}), 1)) return false;
    context.ready = true;
    renderer.focus_owner = nullptr;
    const auto resize = [&](u32 w, u32 h) noexcept {
        renderer.chat_state = old_state; renderer.user_store = old_users; renderer.navigation_view = old_navigation;
        return probe_resize_surface(w, h) && context.project();
    };
    const auto show = [&](const wchar_t* name) noexcept {
        if (!context.project()) return false;
        if (!probe_present(&renderer, context.theme)) {
            const auto shell_view = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::message_view);
            log_u64(LogLevel::error, "probe", "thread_membership_shell_message_top_units", ui_extent_units(shell_view.top));
            log_u64(LogLevel::error, "probe", "thread_membership_native_message_top_units", ui_extent_units(context.native.scene.viewport.top));
            const auto* native_header = thread_membership_ui_adapter_header();
            log_u64(LogLevel::error, "probe", "thread_membership_header_top_units", ui_extent_units(native_header->geometry.rectangle.top));
            log_u64(LogLevel::error, "probe", "thread_membership_header_bottom_units", ui_extent_units(native_header->geometry.rectangle.bottom));
            const UiWidgetManager* managers[]{&renderer.ui_scene->widgets, &context.native.scene.widgets,
                &thread_membership_ui_adapter_popup()->widgets};
            for (u32 index = 0; index < 3; ++index) {
                UiLayoutDiagnostic diagnostics[16]{};
                const auto validation = ui_layout_engine_validate(managers[index]->layout, {diagnostics, 16});
                log_u64(LogLevel::error, "probe", "thread_membership_failed_layout_manager", index);
                log_u64(LogLevel::error, "probe", "thread_membership_failed_layout_count", validation.count);
                log_u64(LogLevel::error, "probe", "thread_membership_failed_layout_dropped", validation.dropped);
                for (u32 entry = 0; entry < validation.count; ++entry) {
                    char text[512]{};
                    ui_widget_manager_format_diagnostic(*managers[index], diagnostics[entry], {text, sizeof(text)});
                    log_write(LogLevel::error, "probe", text);
                }
            }
            return false;
        }
        return !capture || capture(name, context.theme);
    };
    const auto header = [&]() noexcept -> UiThreadHeader& { return *thread_membership_ui_adapter_header(); };
    const auto pointer = [&](UiWidgetHandle widget) noexcept {
        const auto* node = ui_widget_manager_layout_node(*header().manager, widget);
        if (!node) return ApplicationThreadHeaderActivationResult{Admission::invalid, false, false};
        const UiPoint point{(node->rectangle.left + node->rectangle.right) * .5f,
            (node->rectangle.top + node->rectangle.bottom) * .5f};
        const auto press = ui_thread_header_pointer(&header(), UiButtonPointerEvent::press, point);
        const auto release = ui_thread_header_pointer(&header(), UiButtonPointerEvent::release, point);
        return press.handled && release.handled && release.activate
            ? thread_membership_ui_adapter_activate(release.target)
            : ApplicationThreadHeaderActivationResult{Admission::invalid, false, false};
    };
    const auto keyboard = [&](UiWidgetHandle widget) noexcept {
        if (!ui_widget_manager_focus(header().manager, widget))
            return ApplicationThreadHeaderActivationResult{Admission::invalid, false, false};
        return thread_membership_ui_adapter_activate(ui_thread_header_widget_target(header(), widget));
    };
    const auto pixels = [&]() noexcept {
        surface_motion_probe_detail::Readback rb{};
        UiTextCommand commands[12]{}; UiTextCommandStream stream{}; UiTextOutput output{&stream, nullptr};
        if (!d3d11_renderer_draw_stream(&renderer, context.theme, false, false, nullptr) ||
            !rb.initialize(renderer) || !rb.read(renderer, 0) ||
            !ui_text_command_stream_initialize(&stream, {commands, 12}) ||
            !ui_thread_header_text(header(), context.theme, &output)) return false;
        u32 checked = 0;
        for (u32 index = 0; index < stream.count; ++index) {
            const auto& command = commands[index];
            const auto* node = ui_widget_manager_layout_node(*header().manager, command.owner.handle);
            if (!node) return false;
            const bool canvas_text = command.owner.handle.slot == header().widgets.status.slot ||
                command.owner.handle.slot == header().widgets.feedback.slot;
            const UiPoint sample{canvas_text ? header().geometry.rectangle.left + 2.f
                                            : (node->rectangle.left + node->rectangle.right) * .5f,
                canvas_text ? (node->rectangle.top + node->rectangle.bottom) * .5f
                            : node->rectangle.top + 2.f};
            if (!blended_ink(rb, ui_rect_intersection(command.rectangle, command.clip),
                    renderer.dpi / 96.f, command.color, sample)) {
                log_u64(LogLevel::error, "probe", "thread_membership_header_glyph_failed", index); return false;
            }
            ++checked;
        }
        return checked && context.native.scene.viewport.top >= header().geometry.rectangle.bottom &&
            ui_rect_width(header().geometry.status) > 0 &&
            (!ui_rect_has_area(header().geometry.primary) || header().geometry.status.right <= header().geometry.primary.left);
    };
    const auto warm = [&](u32 case_index) noexcept {
        const auto& r = renderer;
        const auto& scene = context.native.scene;
        const auto& state = *thread_membership_ui_adapter_state();
        const auto& popup = *thread_membership_ui_adapter_popup();
        const u32 before[]{scene.rebuilds, scene.layout_passes, scene.preparation_passes,
            r.ui_scene->layout_passes, r.ui_scene->layout_rebuilds,
            r.guild_rail_scene->layout_passes, r.guild_rail_scene->rebuilds,
            r.shell_navigation_scene->layout_passes, r.shell_navigation_scene->rebuilds,
            r.message_composer_scene->layout_passes, header().revision, header().projections,
            context.navigation.revision, measure.backend_calls, r.glyph_text.cache_builds,
            r.content_text->rebuilds, r.paint_instance_uploads, r.text_instance_uploads,
            r.media_instance_uploads, r.painted_frames, r.unchanged_present_skips,
            state.guild_mutations, state.channel_mutations, state.selection_revision,
            thread_membership_ui_adapter_rich_store()->updated, popup.revision, popup.widgets.layout.revision};
        const auto owner = thread_membership_ui_adapter_metrics();
        for (u32 index = 0; index < 64; ++index)
            if (!thread_membership_ui_adapter_synchronize() || !context.project() ||
                !probe_present(&renderer, context.theme)) return false;
        const u32 after[]{scene.rebuilds, scene.layout_passes, scene.preparation_passes,
            r.ui_scene->layout_passes, r.ui_scene->layout_rebuilds,
            r.guild_rail_scene->layout_passes, r.guild_rail_scene->rebuilds,
            r.shell_navigation_scene->layout_passes, r.shell_navigation_scene->rebuilds,
            r.message_composer_scene->layout_passes, header().revision, header().projections,
            context.navigation.revision, measure.backend_calls, r.glyph_text.cache_builds,
            r.content_text->rebuilds, r.paint_instance_uploads, r.text_instance_uploads,
            r.media_instance_uploads, r.painted_frames, r.unchanged_present_skips,
            state.guild_mutations, state.channel_mutations, state.selection_revision,
            thread_membership_ui_adapter_rich_store()->updated, popup.revision, popup.widgets.layout.revision};
        const char* names[]{"resolve", "layout", "preparation", "shell_layout", "shell_rebuild",
            "rail_layout", "rail_rebuild", "sidebar_layout", "sidebar_rebuild", "composer_layout",
            "header_revision", "header_projection", "navigation", "shape", "raster", "text_build",
            "paint_upload", "text_upload", "media_upload", "draw", "skips", "guild_publication",
            "channel_publication", "selection", "rich_publication", "popup_revision", "popup_layout"};
        log_u64(LogLevel::info, "thread_membership_warm", "case", case_index);
        bool valid = true;
        for (u32 index = 0; index < 27; ++index) {
            const u32 delta = after[index] - before[index];
            log_u64(LogLevel::info, "thread_membership_warm", names[index], delta);
            valid &= delta == (index == 20 ? 64u : 0u);
        }
        const auto current = thread_membership_ui_adapter_metrics();
        log_u64(LogLevel::info, "thread_membership_warm", "owner_bytes", current.owner_bytes);
        log_u64(LogLevel::info, "thread_membership_warm", "owner_committed_bytes", current.owner_committed_bytes);
        log_u64(LogLevel::info, "thread_membership_warm", "entries", current.entry_count);
        log_u64(LogLevel::info, "thread_membership_warm", "owner_publication", current.publication_revision);
        log_u64(LogLevel::info, "thread_membership_warm", "native_widgets", scene.widgets.count);
        log_u64(LogLevel::info, "thread_membership_warm", "native_rows", scene.owned_rows.capacity);
        log_u64(LogLevel::info, "thread_membership_warm", "model_records", thread_membership_ui_adapter_store()->count);
        return valid && current.owner_bytes == owner.owner_bytes &&
            current.owner_committed_bytes == owner.owner_committed_bytes && current.entry_count == owner.entry_count &&
            current.publication_revision == owner.publication_revision && current.flight_active == owner.flight_active;
    };
    const auto menu_click = [&](u32 action) noexcept {
        auto& popup = *thread_membership_ui_adapter_popup();
        for (u32 index = 0; index < popup.item_count; ++index) {
            if (popup.items[index].action != action) continue;
            PopupItemLayout geometry{}; bool changed{};
            if (!popup_scene_resolve_item_layout(popup, index, &geometry) || !geometry.visible)
                return ApplicationThreadHeaderActivationResult{Admission::invalid, false, false};
            const float x = (geometry.visible_rectangle.left + geometry.visible_rectangle.right) * .5f;
            const float y = (geometry.visible_rectangle.top + geometry.visible_rectangle.bottom) * .5f;
            if (!popup_scene_set_hover(&popup, x, y, &changed))
                return ApplicationThreadHeaderActivationResult{Admission::invalid, false, false};
            const auto hit = popup_scene_hit_test(popup, x, y);
            return hit.enabled && hit.action == action ? thread_membership_ui_adapter_menu_activate(action)
                : ApplicationThreadHeaderActivationResult{Admission::invalid, false, false};
        }
        return ApplicationThreadHeaderActivationResult{Admission::invalid, false, false};
    };
    const auto exact_submit = [&](u64 id, ThreadActionOperation operation) noexcept {
        ThreadActionRequest request{}; u64 actual{};
        return thread_membership_ui_adapter_submit(id) &&
            thread_membership_ui_adapter_flight(&actual, &request) && actual == id &&
            request.thread_id == kThreadMembershipProbePublic && request.self_user_id == kThreadMembershipProbeSelf &&
            request.operation == operation;
    };
    renderer_set_dpi(&renderer, 96);
    if (!require(resize(1100, 760), 2)) return false;
    const auto menu = thread_membership_ui_adapter_menu_actions();
    const auto opened = pointer(header().widgets.overflow);
    if (!require(opened.handled && opened.accepted &&
        show(L"thread-membership-joined.bmp") && pixels(), 3)) return false;
    popup_scene_close(thread_membership_ui_adapter_popup());
    if (!require(context.project() && probe_present(&renderer, context.theme) && warm(1), 4)) return false;
    if (!require(pointer(header().widgets.overflow).accepted, 5)) return false;
    const auto leave = menu_click(menu.leave);
    popup_scene_close(thread_membership_ui_adapter_popup());
    if (!require(leave.handled && leave.accepted && exact_submit(2001, ThreadActionOperation::leave) &&
        thread_membership_ui_adapter_view(kThreadMembershipProbePublic).membership == ApplicationThreadMembershipStatus::joined &&
        thread_membership_ui_adapter_response(204) &&
        thread_membership_ui_adapter_view(kThreadMembershipProbePublic).membership == ApplicationThreadMembershipStatus::not_joined &&
        show(L"thread-membership-unjoined.bmp") && pixels(), 6)) return false;
    if (!require(thread_membership_ui_adapter_observe_membership(kThreadMembershipProbePublic, false, false) &&
        context.project() && ui_thread_header_primary_action(header().view) == UiThreadAction::check_status &&
        !application_thread_actions_channel_readable(kThreadMembershipProbePrivate) &&
        !application_thread_actions_channel_sendable(kThreadMembershipProbePrivate) &&
        !navigation_view_find_channel(context.navigation, kThreadMembershipProbePrivate), 7)) return false;
    const UiRect menu_anchor = header().geometry.overflow;
    if (!require(thread_membership_ui_adapter_open_menu(kThreadMembershipProbePrivate, menu_anchor,
            {0, 0, renderer.width * 96.f / renderer.dpi, renderer.height * 96.f / renderer.dpi}), 8)) return false;
    bool disabled_private_join = false;
    const auto& blocked = *thread_membership_ui_adapter_popup();
    for (u32 index = 0; index < blocked.item_count; ++index)
        disabled_private_join |= blocked.items[index].action == menu.join &&
            !(blocked.items[index].flags & popup_item_enabled);
    const auto private_refused = thread_membership_ui_adapter_menu_activate(menu.join);
    if (!require(disabled_private_join && private_refused.handled && !private_refused.accepted &&
        show(L"thread-membership-unknown-private.bmp") && pixels(), 9)) return false;
    popup_scene_close(thread_membership_ui_adapter_popup());
    if (!require(context.project() && keyboard(header().widgets.primary).accepted &&
        exact_submit(2002, ThreadActionOperation::fetch_self) &&
        thread_membership_ui_adapter_response(200, utf8_literal(
            R"({"id":"96110","user_id":"96005","join_timestamp":"2026-10-05T00:00:00Z","flags":0})")) &&
        thread_membership_ui_adapter_view(kThreadMembershipProbePublic).membership == ApplicationThreadMembershipStatus::joined &&
        thread_membership_ui_adapter_observe_archive(kThreadMembershipProbePublic, true) &&
        application_thread_actions_channel_readable(kThreadMembershipProbePublic) &&
        !application_thread_actions_channel_sendable(kThreadMembershipProbePublic) && context.project(), 10)) return false;
    const auto unarchive = keyboard(header().widgets.primary);
    if (!require(unarchive.handled && unarchive.accepted && exact_submit(2003, ThreadActionOperation::unarchive) &&
        thread_membership_ui_adapter_view(kThreadMembershipProbePublic).archived &&
        show(L"thread-membership-unarchive-pending.bmp") && pixels() &&
        context.project() && probe_present(&renderer, context.theme) && warm(2), 11)) return false;
    if (!require(thread_membership_ui_adapter_response(403) &&
        thread_membership_ui_adapter_view(kThreadMembershipProbePublic).archived &&
        thread_membership_ui_adapter_view(kThreadMembershipProbePublic).phase == ApplicationThreadActionPhase::failed &&
        show(L"thread-membership-rollback.bmp") && pixels(), 12)) return false;
    const auto stale = ui_thread_header_widget_target(header(), header().widgets.primary);
    if (!require(chat_state_store_set_thread_locked(thread_membership_ui_adapter_state(),
            kThreadMembershipProbePublic, kThreadMembershipProbeSelf, true) && context.project(), 13)) return false;
    const auto refused = thread_membership_ui_adapter_activate(stale);
    if (!require(refused.handled && !refused.accepted && refused.admission == Admission::stale &&
        chat_state_store_set_thread_locked(thread_membership_ui_adapter_state(),
            kThreadMembershipProbePublic, kThreadMembershipProbeSelf, false), 14)) return false;
    context.theme = kLightTheme;
    context.theme.accent = {.95f, .85f, .4f, 1.f};
    renderer_set_dpi(&renderer, 144);
    if (!require(resize(960, 900) && show(L"thread-membership-narrow-light-dpi144.bmp") && pixels(), 15)) return false;
    const auto retry = keyboard(header().widgets.primary);
    const auto cancel = pointer(header().widgets.cancel);
    if (!require(retry.handled && retry.accepted && cancel.handled && cancel.accepted &&
        thread_membership_ui_adapter_view(kThreadMembershipProbePublic).phase == ApplicationThreadActionPhase::cancelled &&
        !application_thread_actions_pending(clock_monotonic_milliseconds()) && context.project(), 16)) return false;
    if (!require(keyboard(header().widgets.primary).accepted && exact_submit(2004, ThreadActionOperation::unarchive) &&
        thread_membership_ui_adapter_response(200, utf8_literal(
            R"({"id":"96110","guild_id":"96000","parent_id":"96100","type":11,"name":"Native thread review","thread_metadata":{"archived":false,"locked":false}})")) &&
        !thread_membership_ui_adapter_view(kThreadMembershipProbePublic).archived &&
        application_thread_actions_channel_sendable(kThreadMembershipProbePublic), 17)) return false;
    {
        // Seed through the actual model/upload APIs only after the six visual
        // captures. A real link source and original avatar tile make stale
        // native hit/media disappearance measurable rather than vacuous.
        auto* store = thread_membership_ui_adapter_store();
        const auto found = message_store_find(*store, 96200);
        const auto* prior = found.ok() ? message_store_resolve(*store, found.handle) : nullptr;
        if (!require(prior && store->count == 2, 19)) return false;
        GatewayMessageEvent event{};
        event.id = prior->id; event.channel_id = prior->channel_id;
        event.content = utf8_literal("Retained [native link](https://example.invalid/native-thread) and original avatar.");
        event.fields = gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_content;
        const auto accepted = message_store_upsert(store, {prior->id, prior->channel_id, prior->author_id,
            prior->timestamp_ms, prior->edited_timestamp_ms, event.content, prior->flags});
        const auto* current = accepted.ok() ? message_store_resolve(*store, accepted.handle) : nullptr;
        constexpr auto avatar_hash = utf8_literal("1234567890abcdef1234567890abcdef");
        u8 tile[16 * 16 * 4]{};
        for (u32 index = 0; index < 16 * 16; ++index) {
            tile[index * 4] = 240; tile[index * 4 + 1] = 64;
            tile[index * 4 + 2] = 32; tile[index * 4 + 3] = 255;
        }
        if (!require(current && message_rich_store_apply(thread_membership_ui_adapter_rich_store(),
                event.id, current->revision, event).ok() &&
            user_store_set_avatar(thread_membership_ui_adapter_users(), 96006, avatar_hash) &&
            renderer_upload_media_straight_rgba8(&renderer,
                media_asset_user_avatar(96006, user_avatar_variant(avatar_hash), false),
                16, 16, 64, {tile, sizeof(tile)}).ok() && context.project() &&
            probe_present(&renderer, context.theme), 20)) return false;
        const auto* row = message_scene_find_row(context.native.scene, 0);
        const auto* avatar = row ? message_scene_layout_node(context.native.scene, row->avatar) : nullptr;
        UiRect body{}, avatar_patch{}; Color body_ink{};
        for (u32 index = 0; index < context.native.text.command_count; ++index) {
            const auto& command = context.native.text.commands[index];
            if (command.message_id == event.id && command.measurement_kind == MessageTextMeasurementKind::content) {
                body = ui_rect_intersection(command.rectangle, command.clip); body_ink = command.color; break;
            }
        }
        if (avatar) {
            const float x = (avatar->rectangle.left + avatar->rectangle.right) * .5f;
            const float y = (avatar->rectangle.top + avatar->rectangle.bottom) * .5f;
            avatar_patch = {x - 2.f, y - 2.f, x + 2.f, y + 2.f};
        }
        UiPoint link{}; bool link_found = false;
        const auto& text = *renderer.content_text;
        for (u32 index = 0; index < text.content_hit_region_count; ++index) {
            const auto& region = text.content_hit_regions[index];
            if (region.semantic_index >= text.content_semantic_count) continue;
            const auto& semantic = text.content_semantics[region.semantic_index];
            if (semantic.message_id == event.id && semantic.kind == MessageMarkdownSemanticKind::link) {
                link = {(region.left + region.right) * .5f, (region.top + region.bottom) * .5f};
                link_found = true; break;
            }
        }
        const i32 link_x = static_cast<i32>(link.x * renderer.dpi / 96.f);
        const i32 link_y = static_cast<i32>(link.y * renderer.dpi / 96.f);
        const auto semantic_before = renderer_content_hit_test(renderer, link_x, link_y);
        const auto position_before = renderer_message_text_position_hit_test(renderer, link_x, link_y);
        surface_motion_probe_detail::Readback readback{};
        const auto read_native = [&]() noexcept {
            return d3d11_renderer_draw_stream(&renderer, context.theme, false, false, nullptr) && readback.read(renderer, 0);
        };
        const auto tile_pixels = [&]() noexcept {
            const auto area = raster_clip_rectangle(avatar_patch, renderer.dpi / 96.f, readback.width, readback.height);
            constexpr u8 expected[]{32, 64, 240}; u32 count = 0;
            for (i32 y = maximum(0, area.top); y < area.bottom; ++y)
                for (i32 x = maximum(0, area.left); x < area.right; ++x)
                    if (surface_motion_probe_detail::distance(readback.images + (y * readback.width + x) * 4,
                            expected) <= 24 && ++count >= 6) return true;
            return false;
        };
        const UiPoint body_fill{body.left - 2.f, (body.top + body.bottom) * .5f};
        if (!require(avatar && ui_rect_has_area(body) && link_found && semantic_before.valid &&
            semantic_before.message_id == event.id && position_before.valid && position_before.message_id == event.id &&
            text.message_geometry_quads > 0 && renderer.media_quad_count > 0 &&
            readback.initialize(renderer) && read_native() && tile_pixels() &&
            blended_ink(readback, body, renderer.dpi / 96.f, body_ink, body_fill), 21)) return false;
        // Preserve both authoritative records and the retained text source;
        // only the real native scene is deactivated by an empty viewport.
        const u32 retained_commands = context.native.text.command_count;
        if (!require(message_scene_update(&context.native.scene, {}).ok() &&
            !context.native.scene.active && !context.native.scene.count &&
            !ui_rect_has_area(context.native.scene.viewport) && store->count == 2 &&
            context.native.text.command_count == retained_commands && retained_commands &&
            renderer_rebuild_content_text(&renderer, context.theme) && probe_present(&renderer, context.theme) &&
            read_native() && text.message_geometry_quads == 0 && text.content_semantic_count == 0 &&
            text.content_hit_region_count == 0 && renderer.media_quad_count == 0 &&
            !renderer_content_hit_test(renderer, link_x, link_y).valid &&
            !renderer_message_text_position_hit_test(renderer, link_x, link_y).valid &&
            !tile_pixels() && !blended_ink(readback, body, renderer.dpi / 96.f, body_ink, body_fill), 22)) return false;
        if (!require(context.project() && probe_present(&renderer, context.theme) && read_native() &&
            context.native.scene.active && store->count == 2 && text.message_geometry_quads > 0 &&
            text.content_semantic_count > 0 && text.content_hit_region_count > 0 && tile_pixels(), 23)) return false;
        log_write(LogLevel::info, "probe", "thread_membership_inactive_cached_main_geometry_text_media_hits_pixels_cleared_restored");
    }
    context.ready = false;
    renderer.chat_state = old_state; renderer.user_store = old_users; renderer.navigation_view = old_navigation;
    renderer_set_dpi(&renderer, dpi);
    const bool restored = probe_resize_surface(width, height);
    if (restored) log_write(LogLevel::info, "probe",
        "thread_membership_production_dispatch_exact_receipts_private_scope_pixels_two64_passed");
    return require(restored, 18);
}
} // namespace maicord
