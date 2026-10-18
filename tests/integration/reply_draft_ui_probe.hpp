#pragma once
#include "reply_draft_ui_adapter.hpp"
#include "probe_present.hpp"
#include "ui_surface_motion_measurement.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord {
namespace reply_draft_probe_detail {
struct NativeMessages {
    MessageScene scene{};
    MessageTextPresentation text{};
    UiWidget widgets[4 * kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode nodes[4 * kMessageSceneWidgetsPerRow + 1]{};
    MessageTextCommand commands[128]{};
    char bytes[8192]{};
    TextStyleSpan styles[128]{};
    MessageMarkdownSemantic semantics[64]{};
    MessageTextMeasurement measurements[8]{};
    MessageTextSpoilerReveal reveals[4]{};
    u32 markdown_scratch[8192]{};
    ~NativeMessages() noexcept { message_scene_release(&scene); }
    bool initialize() noexcept {
        return message_scene_initialize_owned(&scene, reply_draft_ui_adapter_timeline(), 4, 1,
            {widgets, 4 * kMessageSceneWidgetsPerRow + 1}, {nodes, 4 * kMessageSceneWidgetsPerRow + 1}) &&
            message_text_presentation_initialize(&text, {commands, 128}, {bytes, sizeof(bytes)},
                {styles, 128}, {semantics, 64}, {measurements, 8}, {reveals, 4}, {markdown_scratch, 8192});
    }
};

inline bool text_pixels(const surface_motion_probe_detail::Readback& rb, UiRect text, UiRect fill,
    float scale, Color foreground) noexcept {
    const auto region = raster_clip_rectangle(text, scale, rb.width, rb.height);
    const u32 sx = static_cast<u32>((fill.left + 2.f) * scale),
        sy = static_cast<u32>((fill.top + 2.f) * scale);
    if (sx >= rb.width || sy >= rb.height) return false;
    const auto* background = rb.images + (sy * rb.width + sx) * 4;
    const u8 expected[]{static_cast<u8>(foreground.blue * 255.f + .5f),
        static_cast<u8>(foreground.green * 255.f + .5f), static_cast<u8>(foreground.red * 255.f + .5f)};
    float vector[3]{}, length = 0;
    for (u32 component = 0; component < 3; ++component) {
        vector[component] = static_cast<float>(expected[component]) - background[component];
        length += vector[component] * vector[component];
    }
    if (length < 1.f) return false;
    u32 pixels = 0;
    for (i32 y = maximum(0, region.top); y < region.bottom; ++y)
        for (i32 x = maximum(0, region.left); x < region.right; ++x) {
            const auto* pixel = rb.images + (y * rb.width + x) * 4;
            float coverage = 0;
            for (u32 component = 0; component < 3; ++component)
                coverage += (pixel[component] - background[component]) * vector[component];
            coverage /= length;
            if (coverage < .15f || coverage > 1.05f) continue;
            coverage = minimum(coverage, 1.f);
            float residual = 0;
            for (u32 component = 0; component < 3; ++component) {
                const float difference = pixel[component] - (background[component] + coverage * vector[component]);
                residual += difference < 0 ? -difference : difference;
            }
            if (residual <= 24 && surface_motion_probe_detail::distance(pixel, background) >= 24 && ++pixels >= 6)
                return true;
        }
    return false;
}
} // namespace reply_draft_probe_detail

inline bool reply_draft_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache& measure, HWND window,
    bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept {
    using namespace reply_draft_probe_detail;
    if (!window || !renderer.ui_scene || !renderer.content_text || !renderer.message_composer_scene ||
        !renderer.guild_rail_scene || !renderer.shell_navigation_scene) return false;
    struct Context {
        D3D11Renderer& renderer;
        UiTextMeasureCache& measure;
        NativeMessages native{};
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
            auto* state = reply_draft_ui_adapter_state();
            auto* users = reply_draft_ui_adapter_users();
            auto* model = reply_draft_ui_adapter_composer();
            auto* composer = reply_draft_ui_adapter_scene();
            auto* shell = const_cast<UiScene*>(renderer.ui_scene);
            const auto reply = reply_draft_ui_adapter_reply();
            const bool editing = reply_draft_ui_adapter_editing();
            const bool reply_visible = editing || reply.message_id != 0;
            auto metrics = kGuildChannelShellLayoutMetrics;
            metrics.flags = static_cast<u16>(metrics.flags & ~ui_shell_layout_members);
            metrics.member_sidebar_width = 0;
            metrics.composer_height = message_composer_scene_cached_height(
                *composer, reply_visible, false, false,
                ui_scene_composer_width(metrics, renderer.width * 96.f / renderer.dpi));
            if (!state || !users || !model || !navigation_view_rebuild(&navigation, *state).ok())
                return false;
            auto* rail = const_cast<GuildRailScene*>(renderer.guild_rail_scene);
            auto* sidebar = const_cast<ShellNavigationScene*>(renderer.shell_navigation_scene);
            const auto* selected =
                navigation_view_find_channel(navigation, navigation.selected_channel_id);
            const MessageComposerProjectionContext projection{users, state, navigation.selected_guild_id};
            char reply_storage[kMessageComposerReplyLabelBytes]{};
            const auto label = editing ? utf8_literal("Editing message") : reply_draft_ui_adapter_label({reply_storage, sizeof(reply_storage)});
            const auto layout = [&]() noexcept {
                metrics.revision = 1000u + ui_extent_units(metrics.composer_height) * 31u + static_cast<u32>(metrics.flags) * 7u;
                return ui_scene_layout_with_metrics(shell, UiShellView::guild_channel, metrics,
                        renderer.width * 96.f / renderer.dpi, renderer.height * 96.f / renderer.dpi) &&
                    guild_rail_scene_update(rail, &navigation,
                        ui_scene_content_rectangle(*shell, UiSemantic::guild_navigation), rail->scroll_dips).ok() &&
                    shell_navigation_scene_update(sidebar, &navigation,
                        ui_scene_content_rectangle(*shell, UiSemantic::channel_navigation), sidebar->scroll_dips).ok() &&
                    message_composer_scene_update(composer, model, ui_scene_content_rectangle(*shell, UiSemantic::composer),
                        selected && (selected->actions & channel_action_send_messages),
                        editing ? kReplyDraftProbeOwnMessage : reply.message_id, label, &projection, {}, {}, false,
                        reply.mention_reply_author, !editing).ok() &&
                    renderer_bind_message_composer_scene(&renderer, composer);
            };
            // Match the shipping shell's native measurement feedback. Width
            // and text changes can grow or shrink the editor before drawing.
            bool settled = false;
            for (u32 pass = 0; pass < 3; ++pass) {
                if (!layout()) return false;
                float measured = 0;
                if (!renderer_measure_message_composer(&renderer, &measured))
                    return false;
                composer->measured_row_height =
                    measured - message_composer_scene_chrome_height(reply_visible, false, false) -
                    message_composer_scene_toolbar_height(ui_rect_width(composer->viewport));
                if (measured == metrics.composer_height) {
                    settled = true;
                    break;
                }
                metrics.composer_height = measured;
            }
            if (!settled) {
                log_write(LogLevel::error, "probe", "reply_draft_composer_height_did_not_converge"); return false;
            }
            model->focused = false;
            message_composer_scene_set_caret_visible(composer, false);
            if (!message_scene_update(&native.scene, state->selected_channel_id == kReplyDraftProbeChannelA
                    ? ui_scene_content_rectangle(*shell, UiSemantic::message_view) : UiRect{}).ok() ||
                !renderer_bind_message_scene(&renderer, &native.scene)) return false;
            if (renderer.message_text != &native.text) {
                renderer_unbind_message_text_presentation(&renderer);
                if (!renderer_bind_message_text_presentation(&renderer, &native.text)) return false;
            }
            renderer.chat_state = state;
            renderer.user_store = users;
            renderer.navigation_view = &navigation;
            renderer.message_measure_cache = &measure;
            renderer.focus_owner = &composer->widgets;
            if (!renderer_bind_popup_scene(&renderer, reply_draft_ui_adapter_popup())) return false;
            if (!native.scene.active) return renderer_rebuild_content_text(&renderer, theme);
            const auto result = message_text_coordinator_converge({&native.scene, &native.text, &measure, state, users,
                &navigation, &theme, +[](void* owner, const Theme& current) noexcept {
                    return renderer_rebuild_content_text(static_cast<D3D11Renderer*>(owner), current);
                }, &renderer, 1'759'667'400ull, static_cast<u16>(renderer.dpi), nullptr});
            if (!result.ok()) log_u64(LogLevel::error, "probe", "reply_draft_native_coordinator_error", static_cast<u64>(result.error));
            return result.ok();
        }
    } context{renderer, measure};
    struct Restore {
        Context& c;
        const MessageScene* scene;
        const MessageTextPresentation* text;
        const MessageComposerScene* composer;
        const PopupScene* popup;
        const ChatStateStore* state;
        const UserStore* users;
        const NavigationView* navigation;
        const UiWidgetManager* focus;
        UiTextMeasureCache* cache;
        u32 dpi;
        ~Restore() noexcept {
            c.ready = false;
            auto& r = c.renderer;
            r.message_scene = scene; r.message_text = text; r.message_composer_scene = composer;
            r.popup_scene = popup;
            r.chat_state = state; r.user_store = users; r.navigation_view = navigation;
            r.focus_owner = focus; r.message_measure_cache = cache;
            if (r.guild_rail_scene) guild_rail_scene_update(const_cast<GuildRailScene*>(r.guild_rail_scene),
                navigation, ui_scene_content_rectangle(*r.ui_scene, UiSemantic::guild_navigation), 0);
            if (r.shell_navigation_scene) shell_navigation_scene_update(const_cast<ShellNavigationScene*>(r.shell_navigation_scene),
                navigation, ui_scene_content_rectangle(*r.ui_scene, UiSemantic::channel_navigation), 0);
            reply_draft_ui_adapter_end();
            renderer_set_dpi(&r, dpi);
            r.retained_presentation.valid = false;
            content_text_retention_invalidate(&r.content_text_retention);
        }
    } restore{context, renderer.message_scene, renderer.message_text, renderer.message_composer_scene,
        renderer.popup_scene,
        renderer.chat_state, renderer.user_store, renderer.navigation_view, renderer.focus_owner,
        renderer.message_measure_cache, renderer.dpi};
    const auto require = [](bool passed, u32 stage) noexcept {
        if (!passed) log_u64(LogLevel::error, "probe", "reply_draft_native_failure_stage", stage);
        return passed;
    };
    if (!require(reply_draft_ui_adapter_begin(window, renderer.dpi, +[](void* owner) noexcept {
            return static_cast<Context*>(owner)->project();
        }, &context), 1) || !require(context.native.initialize(), 11) ||
        !require(navigation_view_initialize(&context.navigation, {context.guilds, 2}, {context.channels, 8},
            {context.scratch, 8}, {context.visibility, 8}, {context.keys, 8}), 12)) return false;
    context.ready = true;
    const auto resize = [&](u32 width, u32 height, u32 dpi) noexcept {
        renderer.chat_state = restore.state; renderer.user_store = restore.users; renderer.navigation_view = restore.navigation;
        renderer_set_dpi(&renderer, dpi);
        return probe_resize_surface(width, height) && context.project();
    };
    const auto show = [&](const wchar_t* name) noexcept {
        return context.project() && probe_present(&renderer, context.theme) && (!capture || capture(name, context.theme));
    };
    const auto geometry = [&]() noexcept {
        auto& composer = *reply_draft_ui_adapter_scene();
        const auto* ping = message_composer_scene_layout_node(composer, MessageComposerControl::reply_mention);
        const auto* cancel = message_composer_scene_layout_node(composer, MessageComposerControl::cancel_reply);
        const auto text = message_composer_scene_reply_text_rectangle(composer);
        const auto* edit = message_composer_scene_layout_node(composer, MessageComposerControl::edit);
        if (!ping || !cancel || !edit || !ui_rect_has_area(text) || ui_rect_width(text) < 140.f ||
            renderer.composer_layout.layout_height > ui_rect_height(edit->content_rectangle) + .01f ||
            !ui_rect_equal(ping->rectangle, ui_rect_intersection(ping->rectangle, ping->clip_rectangle)) ||
            !ui_rect_equal(cancel->rectangle, ui_rect_intersection(cancel->rectangle, cancel->clip_rectangle)) ||
            ui_rect_has_area(ui_rect_intersection(ping->rectangle, cancel->rectangle)) ||
            text.right > ping->rectangle.left ||
            message_composer_scene_hit_test(composer, text.left + 2.f, (text.top + text.bottom) * .5f) != MessageComposerControl::none ||
            ui_widget_manager_next_focus(composer.widgets, composer.reply_mention, false, false).slot != composer.reply_cancel.slot)
            return false;
        surface_motion_probe_detail::Readback rb{};
        const auto* banner = ui_widget_manager_layout_node(composer.widgets, composer.reply);
        return banner && d3d11_renderer_draw_stream(&renderer, context.theme, false, false, nullptr) &&
            rb.initialize(renderer) && rb.read(renderer, 0) &&
            text_pixels(rb, text, banner->rectangle, renderer.dpi / 96.f, context.theme.text_secondary) &&
            text_pixels(rb, ping->content_rectangle, ping->rectangle, renderer.dpi / 96.f,
                composer.mention_reply_author ? context.theme.accent : context.theme.text_secondary);
    };
    const auto warm = [&](u32 sample) noexcept {
        auto& composer = *reply_draft_ui_adapter_scene();
        if (!context.project() || !probe_present(&renderer, context.theme)) return false;
        const auto before = surface_motion_probe_detail::snapshot(renderer);
        const u32 layout = composer.layout_passes, shell_layout = renderer.ui_scene->layout_passes,
            shape = measure.backend_calls, painted = renderer.painted_frames, skips = renderer.unchanged_present_skips,
            commits = reply_draft_ui_adapter_controller()->draft_commits;
        for (u32 frame = 0; frame < 64; ++frame)
            if (!context.project() || !probe_present(&renderer, context.theme)) return false;
        const bool passed = surface_motion_probe_detail::same(before, surface_motion_probe_detail::snapshot(renderer)) &&
            composer.layout_passes == layout && renderer.ui_scene->layout_passes == shell_layout &&
            measure.backend_calls == shape && renderer.painted_frames == painted &&
            renderer.unchanged_present_skips - skips == 64 && reply_draft_ui_adapter_controller()->draft_commits == commits;
        log_u64(LogLevel::info, "reply_draft_warm", "sample", sample);
        log_u64(LogLevel::info, "reply_draft_warm", "unchanged_skips", renderer.unchanged_present_skips - skips);
        log_u64(LogLevel::info, "reply_draft_warm", "passed", passed);
        return passed;
    };
    if (!require(resize(1100, 760, 96) && reply_draft_ui_adapter_begin_reply(kReplyDraftProbeMessage) &&
        show(L"reply-draft-empty-ping-on-dark.bmp") && geometry(), 2)) return false;
    if (!require(reply_draft_ui_adapter_key(MessageComposerControl::reply_mention, VK_SPACE), 301) ||
        !require(!reply_draft_ui_adapter_reply().mention_reply_author, 302) ||
        !require(reply_draft_ui_adapter_select(kReplyDraftProbeChannelB) && !reply_draft_ui_adapter_reply().message_id, 303) ||
        !require(reply_draft_ui_adapter_select(kReplyDraftProbeChannelA), 304) ||
        !require(reply_draft_ui_adapter_restart(), 305) ||
        !require(reply_draft_ui_adapter_reply().message_id == kReplyDraftProbeMessage &&
            !reply_draft_ui_adapter_reply().mention_reply_author && !reply_draft_ui_adapter_composer()->bytes, 306) ||
        !require(show(L"reply-draft-empty-ping-off-restart.bmp"), 307) ||
        !require(geometry(), 308) || !require(warm(1), 309)) return false;
    const auto draft = utf8_literal("The text survives navigation, edit, restart and reply cancellation.");
    if (!require(reply_draft_ui_adapter_profile(true) && !reply_draft_ui_adapter_reply().message_id, 401) ||
        !require(reply_draft_ui_adapter_text(utf8_literal("Second profile draft")), 402) ||
        !require(reply_draft_ui_adapter_profile(false), 403) ||
        !require(reply_draft_ui_adapter_reply().message_id == kReplyDraftProbeMessage && !reply_draft_ui_adapter_composer()->bytes, 404) ||
        !require(reply_draft_ui_adapter_text(draft), 405) || !require(reply_draft_ui_adapter_edit(true), 406) ||
        !require(!message_composer_scene_reply_mention_label(*reply_draft_ui_adapter_scene()).bytes, 407) ||
        !require(show(L"reply-draft-edit-suspended.bmp"), 408) || !require(reply_draft_ui_adapter_edit(false), 409) ||
        !require(utf8_equal(message_composer_text(*reply_draft_ui_adapter_composer()), draft) &&
            reply_draft_ui_adapter_reply().message_id == kReplyDraftProbeMessage &&
            !reply_draft_ui_adapter_reply().mention_reply_author, 410)) return false;
    char long_author[126]{};
    for (u32 byte = 0; byte < sizeof(long_author); byte += 3) {
        long_author[byte] = static_cast<char>(0xe6); long_author[byte + 1] = static_cast<char>(0x9d); long_author[byte + 2] = static_cast<char>(0x8e);
    }
    char scalar_boundary[17]{};
    if (!require(reply_draft_ui_adapter_author({long_author, sizeof(long_author)}) &&
        utf8_validate(message_composer_scene_reply_label(*reply_draft_ui_adapter_scene())).valid &&
        message_composer_scene_reply_label(*reply_draft_ui_adapter_scene()).bytes > 120 &&
        reply_draft_ui_adapter_label({scalar_boundary, sizeof(scalar_boundary)}).bytes == 15 &&
        utf8_validate(reply_draft_ui_adapter_label({scalar_boundary, sizeof(scalar_boundary)})).valid &&
        show(L"reply-draft-long-author-dark.bmp") && geometry(), 5)) return false;
    context.theme = kLightTheme;
    if (!require(resize(960, 900, 144) && show(L"reply-draft-narrow-light-dpi144.bmp") && geometry() && warm(2), 6)) return false;
    context.theme = kDefaultTheme;
    if (!require(resize(1280, 1000, 192) && show(L"reply-draft-narrow-dark-dpi192.bmp") && geometry() && warm(3), 7)) return false;
    if (!require(reply_draft_ui_adapter_mouse(MessageComposerControl::cancel_reply, renderer.dpi) &&
        !reply_draft_ui_adapter_reply().message_id && utf8_equal(message_composer_text(*reply_draft_ui_adapter_composer()), draft) &&
        show(L"reply-draft-cancel-keeps-text.bmp") &&
        reply_draft_ui_adapter_begin_reply(kReplyDraftProbeMessage) &&
        reply_draft_ui_adapter_mouse(MessageComposerControl::reply_mention, renderer.dpi) &&
        !reply_draft_ui_adapter_reply().mention_reply_author && reply_draft_ui_adapter_evict_source() &&
        reply_draft_ui_adapter_restart() && reply_draft_ui_adapter_reply().message_id == kReplyDraftProbeMessage &&
        utf8_equal(message_composer_scene_reply_label(*reply_draft_ui_adapter_scene()), utf8_literal("Replying to a message")) &&
        message_composer_scene_focus_control(reply_draft_ui_adapter_scene(), MessageComposerControl::cancel_reply) &&
        show(L"reply-draft-uncached-restored.bmp") && geometry() && warm(4), 8)) return false;
    if (!require(reply_draft_ui_adapter_gateway(utf8_literal(R"({"op":0,"d":{"id":"97200","channel_id":"97101"},"s":1,"t":"MESSAGE_DELETE"})")) &&
        reply_draft_ui_adapter_reply().message_id == kReplyDraftProbeMessage &&
        reply_draft_ui_adapter_select(kReplyDraftProbeChannelB) &&
        reply_draft_ui_adapter_gateway(utf8_literal(R"({"op":0,"d":{"ids":["97200","97209"],"channel_id":"97100"},"s":2,"t":"MESSAGE_DELETE_BULK"})")) &&
        reply_draft_ui_adapter_select(kReplyDraftProbeChannelA) && !reply_draft_ui_adapter_reply().message_id &&
        utf8_equal(message_composer_text(*reply_draft_ui_adapter_composer()), draft) &&
        reply_draft_ui_adapter_begin_reply(kReplyDraftProbeOwnMessage), 9)) return false;
    const auto stale_reply = reply_draft_ui_adapter_reply();
    auto* controller = reply_draft_ui_adapter_controller();
    const u32 requests = controller->request_count;
    char external_text[64]{};
    MessageComposer external{};
    struct ReleaseExternal { MessageComposer& model; ~ReleaseExternal() noexcept { message_composer_release(&model); } } release_external{external};
    if (!message_composer_initialize(&external, {external_text, sizeof(external_text)})) return false;
    message_composer_reset(&external, kReplyDraftProbeChannelA, false);
    if (!require(reply_draft_ui_adapter_key(MessageComposerControl::edit, VK_ESCAPE) &&
        !reply_draft_ui_adapter_reply().message_id && reply_draft_ui_adapter_begin_reply(kReplyDraftProbeOwnMessage) &&
        message_composer_insert_utf8(&external, utf8_literal("Independent producer caption")) == MessageComposerResult::changed &&
        !message_composer_controller_queue_send(controller, true, kReplyDraftProbeSelf, 1'759'667'480'000ull,
            1000, stale_reply.message_id, {}, {}, &external, {}, stale_reply.mention_reply_author, &stale_reply).ok() &&
        controller->request_count == requests && external.bytes != 0 &&
        reply_draft_ui_adapter_failed_menu({350, 120, 351, 121},
            {0, 0, renderer.width * 96.f / renderer.dpi, renderer.height * 96.f / renderer.dpi}) &&
        reply_draft_ui_adapter_delete_rest(kReplyDraftProbeOwnMessage) && !reply_draft_ui_adapter_reply().message_id &&
        reply_draft_ui_adapter_deleted_menu() && show(L"reply-draft-queued-target-deleted-menu.bmp") &&
        utf8_equal(message_composer_text(*reply_draft_ui_adapter_composer()), draft), 9)) return false;
    reply_draft_ui_adapter_close_menu();
    if (!require(reply_draft_ui_adapter_key(MessageComposerControl::edit, VK_ESCAPE) &&
        reply_draft_ui_adapter_profile(true) &&
        utf8_equal(message_composer_text(*reply_draft_ui_adapter_composer()), utf8_literal("Second profile draft")) &&
        !reply_draft_ui_adapter_reply().message_id && reply_draft_ui_adapter_profile(false) &&
        !reply_draft_ui_adapter_reply().message_id && utf8_equal(message_composer_text(*reply_draft_ui_adapter_composer()), draft), 10)) return false;
    log_write(LogLevel::info, "probe", "reply_draft_actual_controller_session_shared_reply_activation_pointer_keyboard_gateway_native_passed");
    return true;
}
} // namespace maicord
