#pragma once
#include "forum_post_ui_adapter.hpp"
#include "forum_media_ui_probe.hpp"
#include "reply_draft_ui_probe.hpp"
#include "maicord/core/time.hpp"
#include "application_forum_form_state.hpp"
#include "application_thread_browser.hpp"
namespace maicord {
inline bool forum_post_ui_probe(D3D11Renderer& external, UiTextMeasureCache& measure, HWND window,
                                bool (*capture)(const wchar_t*, const Theme&) noexcept,
                                bool media_probe = false) noexcept {
    using namespace app_detail;
    using namespace reply_draft_probe_detail;
    if (!window || !external.ui_scene || !external.guild_rail_scene || !external.shell_navigation_scene)
        return false;
    struct Context {
        D3D11Renderer& external;
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
            if (!ready)
                return true;
            auto& r = forum_post_ui_adapter_renderer(external);
            auto* state = reply_draft_ui_adapter_state();
            auto* users = reply_draft_ui_adapter_users();
            auto* model = reply_draft_ui_adapter_composer();
            auto* composer = reply_draft_ui_adapter_scene();
            if (!state || !users || !model || !navigation_view_rebuild(&navigation, *state).ok())
                return false;
            auto* shell = const_cast<UiScene*>(r.ui_scene);
            auto metrics = kGuildChannelShellLayoutMetrics;
            metrics.flags = static_cast<u16>(metrics.flags & ~ui_shell_layout_members);
            metrics.member_sidebar_width = 0;
            metrics.composer_height = 48;
            if (r.width * 96.f / r.dpi < 600.f)
                metrics.flags = static_cast<u16>(metrics.flags & ~ui_shell_layout_navigation);
            metrics.revision = 5500 + metrics.flags;
            if (!ui_scene_layout_with_metrics(shell, UiShellView::guild_channel, metrics,
                                              r.width * 96.f / r.dpi, r.height * 96.f / r.dpi) ||
                !guild_rail_scene_update(const_cast<GuildRailScene*>(r.guild_rail_scene), &navigation,
                                         ui_scene_content_rectangle(*shell, UiSemantic::guild_navigation),
                                         0)
                     .ok() ||
                !shell_navigation_scene_update(
                     const_cast<ShellNavigationScene*>(r.shell_navigation_scene), &navigation,
                     ui_scene_content_rectangle(*shell, UiSemantic::channel_navigation), 0)
                     .ok())
                return false;
            const MessageComposerProjectionContext projection{users, state, navigation.selected_guild_id};
            if (!message_composer_scene_update(composer, model,
                                               ui_scene_content_rectangle(*shell, UiSemantic::composer),
                                               false, 0, {}, &projection)
                     .ok() ||
                !renderer_bind_message_composer_scene(&r, composer))
                return false;
            model->focused = false;
            message_composer_scene_set_caret_visible(composer, false);
            const auto message_viewport = !application_thread_browser_active() &&
                                                  (state->selected_channel_id == kReplyDraftProbeChannelA ||
                                                   state->selected_channel_id == kForumPostProbeThread)
                                              ? ui_scene_content_rectangle(*shell, UiSemantic::message_view)
                                              : UiRect{};
            const bool project_messages = ui_rect_has_area(message_viewport) || native.scene.active ||
                                          !ui_rect_equal(native.scene.viewport, message_viewport);
            if ((project_messages && !message_scene_update(&native.scene, message_viewport).ok()) ||
                !renderer_bind_message_scene(&r, &native.scene))
                return false;
            if (r.message_text != &native.text) {
                renderer_unbind_message_text_presentation(&r);
                if (!renderer_bind_message_text_presentation(&r, &native.text))
                    return false;
            }
            r.chat_state = state;
            r.user_store = users;
            r.navigation_view = &navigation;
            r.message_measure_cache = &measure;
            auto* form = forum_post_ui_adapter_scene();
            auto* browser = application_thread_browser_scene();
            auto* child_focus = forum_media_ui_adapter_focus();
            r.focus_owner = child_focus ? child_focus
                            : form      ? &form->widgets
                            : browser   ? &browser->widgets
                                        : &composer->widgets;
            forum_post_ui_adapter_viewport(r, *shell);
            if (r.popup_scene != reply_draft_ui_adapter_popup() &&
                !renderer_bind_popup_scene(&r, reply_draft_ui_adapter_popup()))
                return false;
            if (!native.scene.active)
                return renderer_rebuild_content_text(&r, theme);
            return message_text_coordinator_converge(
                       {&native.scene, &native.text, &measure, state, users, &navigation, &theme,
                        +[](void* owner, const Theme& current) noexcept {
                            return renderer_rebuild_content_text(static_cast<D3D11Renderer*>(owner),
                                                                 current);
                        },
                        &r, 1'759'667'400ull, static_cast<u16>(r.dpi), nullptr})
                .ok();
        }
    } context{external, measure};
    struct Restore {
        Context& c;
        const MessageScene* scene;
        const MessageTextPresentation* text;
        const MessageComposerScene* composer;
        const PopupScene* popup;
        TextFormSurface* form;
        ForumPostSurface* forum;
        UiThreadBrowserScene* browser;
        const ChatStateStore* state;
        const UserStore* users;
        const NavigationView* navigation;
        const UiWidgetManager* focus;
        UiTextMeasureCache* cache;
        u32 dpi;
        ~Restore() noexcept {
            c.ready = false;
            forum_post_ui_adapter_end();
            auto& r = c.external;
            r.message_scene = scene;
            r.message_text = text;
            r.message_composer_scene = composer;
            r.popup_scene = popup;
            r.text_form = form;
            r.forum_form = forum;
            r.thread_browser_scene = browser;
            r.chat_state = state;
            r.user_store = users;
            r.navigation_view = navigation;
            r.focus_owner = focus;
            r.message_measure_cache = cache;
            if (r.guild_rail_scene)
                guild_rail_scene_update(
                    const_cast<GuildRailScene*>(r.guild_rail_scene), navigation,
                    ui_scene_content_rectangle(*r.ui_scene, UiSemantic::guild_navigation), 0);
            if (r.shell_navigation_scene)
                shell_navigation_scene_update(
                    const_cast<ShellNavigationScene*>(r.shell_navigation_scene), navigation,
                    ui_scene_content_rectangle(*r.ui_scene, UiSemantic::channel_navigation), 0);
            renderer_set_dpi(&r, dpi);
            r.retained_presentation.valid = false;
            content_text_retention_invalidate(&r.content_text_retention);
        }
    } restore{context,
              external.message_scene,
              external.message_text,
              external.message_composer_scene,
              external.popup_scene,
              external.text_form,
              external.forum_form,
              external.thread_browser_scene,
              external.chat_state,
              external.user_store,
              external.navigation_view,
              external.focus_owner,
              external.message_measure_cache,
              external.dpi};
    const auto require = [](bool value, u32 stage) noexcept {
        if (!value) {
            log_u64(LogLevel::error, "probe", "forum_post_native_failure_stage", stage);
            const auto view = application_forum_post_view();
            log_u64(LogLevel::error, "forum_post_failure", "phase", static_cast<u8>(view.phase));
            log_u64(LogLevel::error, "forum_post_failure", "available", view.available);
            log_u64(LogLevel::error, "forum_post_failure", "error", view.error);
            log_u64(LogLevel::error, "forum_post_failure", "can_edit", view.can_edit);
            log_u64(LogLevel::error, "forum_post_failure", "can_submit", view.can_submit);
            log_u64(LogLevel::error, "forum_post_failure", "can_retry", view.can_retry);
            log_u64(LogLevel::error, "forum_post_failure", "metadata_revision", view.metadata_revision);
            log_u64(LogLevel::error, "forum_post_failure", "selected_tags", view.selected_tags.count);
            log_u64(LogLevel::error, "forum_post_failure", "submissions",
                    forum_post_ui_adapter_submissions());
            log_u64(LogLevel::error, "forum_post_failure", "flight_active",
                    application_forum_post_metrics().flight_active);
            for (u32 tag = 0; tag < view.tags.count; ++tag) {
                log_u64(LogLevel::error, "forum_post_failure", "tag_id", view.tags[tag].id);
                log_u64(LogLevel::error, "forum_post_failure", "tag_selected", view.tags[tag].selected);
                log_u64(LogLevel::error, "forum_post_failure", "tag_enabled", view.tags[tag].enabled);
            }
            if (auto* scene = forum_post_ui_adapter_scene()) {
                const auto focus = forum_post_focused(*scene);
                log_u64(LogLevel::error, "forum_post_failure", "focus_element",
                        static_cast<u8>(focus.element));
                log_u64(LogLevel::error, "forum_post_failure", "focus_tag", focus.tag);
                log_u64(LogLevel::error, "forum_post_failure", "scene_revision", scene->revision);
                log_u64(LogLevel::error, "forum_post_failure", "widget_generation",
                        scene->widgets.generation);
            }
        }
        return value;
    };
    const auto begin = [&]() noexcept {
        context.ready = false;
        if (!forum_post_ui_adapter_begin(
                window, external, measure,
                +[](void* value) noexcept { return static_cast<Context*>(value)->project(); }, &context))
            return false;
        message_scene_release(&context.native.scene);
        if (!context.native.initialize() ||
            !navigation_view_initialize(&context.navigation, {context.guilds, 2},
                                                {context.channels, 8}, {context.scratch, 8},
                                                {context.visibility, 8}, {context.keys, 8}))
            return false;
        context.ready = true;
        return context.project();
    };
    const auto resize = [&](u32 width, u32 height, u32 dpi) noexcept {
        external.chat_state = restore.state;
        external.user_store = restore.users;
        external.navigation_view = restore.navigation;
        renderer_set_dpi(&external, dpi);
        return renderer_resize(&external, width, height) && context.project() &&
               forum_post_ui_adapter_synchronize();
    };
    const auto show = [&](const wchar_t* name) noexcept {
        return context.project() && probe_present(&external, context.theme) &&
               (!capture || capture(name, context.theme));
    };
    const auto geometry = [&]() noexcept {
        auto* scene = forum_post_ui_adapter_scene();
        auto* surface = forum_post_ui_adapter_surface();
        if (!scene || !surface || !scene->active)
            return false;
        const auto* title = forum_post_node(*scene, ForumPostElement::title);
        const auto* body = forum_post_node(*scene, ForumPostElement::body);
        if (!title || !body || ui_rect_height(title->rectangle) != 44 ||
            ui_rect_height(body->rectangle) != 176 ||
            ui_rect_has_area(ui_rect_intersection(title->rectangle, body->rectangle)))
            return false;
        const ForumPostElement actions[]{ForumPostElement::cancel, ForumPostElement::inspect,
                                         ForumPostElement::accept};
        const Utf8View labels[]{scene->content.cancel, scene->content.inspect, scene->content.accept};
        UiRect previous{};
        float first_top = 0, first_bottom = 0;
        for (u32 item = 0; item < 3; ++item) {
            if (!labels[item].bytes)
                continue;
            const auto* node = forum_post_node(*scene, actions[item]);
            if (!node)
                return false;
            const auto clip = ui_rect_intersection(node->content_rectangle, node->clip_rectangle);
            UiTextMeasureValue measured{};
            const UiTextMeasureRequest request{
                {0x464f52554d514155ull, item + 1ull, scene->revision, static_cast<u16>(item), 1},
                labels[item],
                {},
                {ui_rect_width(clip), ui_rect_height(clip)},
                true};
            if (!ui_rect_has_area(clip) || !renderer_text_measure_backend(&external, request, &measured) ||
                measured.line_count != 1 || measured.size.width > ui_rect_width(clip) + .01f ||
                measured.size.height > ui_rect_height(clip) + .01f ||
                ui_rect_has_area(ui_rect_intersection(previous, node->rectangle)))
                return false;
            if (!item) {
                first_top = node->rectangle.top;
                first_bottom = node->rectangle.bottom;
            }
            if (ui_rect_width(scene->viewport) >= 640 &&
                (node->rectangle.top != first_top || node->rectangle.bottom != first_bottom))
                return false;
            previous = node->rectangle;
        }
        return true;
    };
    const auto text = [](const SecureEdit* edit) noexcept {
        if (!edit)
            return Utf8View{};
        const auto bytes = secure_edit_view(*edit);
        return Utf8View{reinterpret_cast<const char*>(bytes.data), bytes.count};
    };
    const auto type = [&]() noexcept {
        if (!forum_post_ui_adapter_pointer({ForumPostElement::title, 0}))
            return false;
        const u32 name[]{'R', 'e',    'l',    'e',    'a', 's',    'e',
                         ' ', 0x65e5, 0x672c, 0x8a9e, ' ', 0xd83d, 0xde80};
        for (u32 unit : name)
            if (!forum_post_ui_adapter_input(UiInputKind::character, unit))
                return false;
        const u32 title_bytes = forum_post_ui_adapter_edit(0)->storage.bytes;
        if (!forum_post_ui_adapter_input(UiInputKind::key_down, VK_RETURN) ||
            !forum_post_ui_adapter_input(UiInputKind::character, '\r') ||
            forum_post_ui_adapter_edit(0)->storage.bytes != title_bytes ||
            !forum_post_ui_adapter_pointer({ForumPostElement::body, 0}))
            return false;
        const u32 first[]{'F', 'i', 'r', 's', 't', ' ', 'l', 'i', 'n', 'e', ' ', 0x65e5, 0x672c, 0x8a9e};
        for (u32 unit : first)
            if (!forum_post_ui_adapter_input(UiInputKind::character, unit))
                return false;
        const u32 submissions = forum_post_ui_adapter_submissions(),
                  body_bytes = forum_post_ui_adapter_edit(1)->storage.bytes;
        if (!forum_post_ui_adapter_input(UiInputKind::key_down, VK_RETURN) ||
            !forum_post_ui_adapter_input(UiInputKind::character, '\r') ||
            forum_post_ui_adapter_submissions() != submissions ||
            forum_post_ui_adapter_edit(1)->storage.bytes != body_bytes + 1)
            return false;
        const u32 second[]{'S', 'e', 'c', 'o', 'n', 'd', ' ', 'l', 'i', 'n', 'e', ' ', 0xd83d, 0xde80};
        for (u32 unit : second)
            if (!forum_post_ui_adapter_input(UiInputKind::character, unit))
                return false;
        return forum_post_ui_adapter_input(UiInputKind::key_down, 'A', false, true) &&
               secure_edit_has_selection(*forum_post_ui_adapter_edit(1)) &&
               forum_post_ui_adapter_tsf_paste_composition() &&
               forum_post_ui_adapter_input(UiInputKind::key_down, 'Z', false, true) &&
               forum_post_ui_adapter_input(UiInputKind::key_down, 'Y', false, true) &&
               utf8_equal(text(forum_post_ui_adapter_edit(0)), utf8_literal("Release 日本語 🚀")) &&
               utf8_equal(text(forum_post_ui_adapter_edit(1)),
                          utf8_literal("First line 日本語\nSecond line 🚀\nTSF paste か"));
    };
    const auto metadata = [&](u64 id, bool required) noexcept {
        Utf8View body{};
        return forum_post_ui_adapter_submit(id) && forum_post_ui_adapter_metadata(required, &body) &&
               forum_post_ui_adapter_response(200, body);
    };
    const auto tag = [&](u32 index) noexcept {
        return forum_post_ui_adapter_pointer({ForumPostElement::tags, index});
    };
    const auto warm = [&](u32 sample) noexcept {
        if (!context.project() || !probe_present(&external, context.theme))
            return false;
        ForumPostProbeCosts before_costs{};
        if (!forum_post_ui_adapter_costs(&before_costs))
            return false;
        const auto before = surface_motion_probe_detail::snapshot(external);
        const auto owner = application_forum_post_metrics();
        const auto preview_before = application_forum_preview_metrics();
        const auto files_before = application_forum_attachments_metrics();
        const auto view_revision = application_forum_post_view().revision;
        const u32 source_before[]{context.native.scene.rebuilds,
                                  context.native.scene.layout_passes,
                                  reply_draft_ui_adapter_scene()->layout_passes,
                                  external.guild_rail_scene->layout_passes,
                                  external.shell_navigation_scene->layout_passes,
                                  context.navigation.revision};
        const u32 shell = external.ui_scene->layout_passes, shapes = measure.backend_calls,
                  painted = external.painted_frames, skips = external.unchanged_present_skips,
                  submits = forum_post_ui_adapter_submissions(),
                  cancels = forum_post_ui_adapter_cancellations();
        for (u32 frame = 0; frame < 64; ++frame)
            if (!forum_post_ui_adapter_synchronize() || !context.project() ||
                !probe_present(&external, context.theme))
                return false;
        const auto after = surface_motion_probe_detail::snapshot(external);
        ForumPostProbeCosts after_costs{};
        if (!forum_post_ui_adapter_costs(&after_costs))
            return false;
        const u32 source_after[]{context.native.scene.rebuilds,
                                 context.native.scene.layout_passes,
                                 reply_draft_ui_adapter_scene()->layout_passes,
                                 external.guild_rail_scene->layout_passes,
                                 external.shell_navigation_scene->layout_passes,
                                 context.navigation.revision};
        bool stable_costs = before_costs.form_bytes == after_costs.form_bytes &&
                            before_costs.form_committed_bytes == after_costs.form_committed_bytes &&
                            before_costs.scene_committed_bytes == after_costs.scene_committed_bytes &&
                            before_costs.form_revision == after_costs.form_revision &&
                            before_costs.form_serial == after_costs.form_serial &&
                            before_costs.scene_revision == after_costs.scene_revision &&
                            before_costs.layout_revision == after_costs.layout_revision &&
                            before_costs.measure_calls == after_costs.measure_calls &&
                            before_costs.scene_commit_count == after_costs.scene_commit_count;
        for (u32 field = 0; field < 2; ++field) {
            const auto a = before_costs.history[field], b = after_costs.history[field];
            stable_costs &=
                before_costs.edit_committed_bytes[field] == after_costs.edit_committed_bytes[field] &&
                before_costs.history_owner_committed_bytes[field] ==
                    after_costs.history_owner_committed_bytes[field] &&
                a.owner_bytes == b.owner_bytes && a.payload_bytes == b.payload_bytes &&
                a.used_bytes == b.used_bytes && a.undo_steps == b.undo_steps &&
                a.redo_steps == b.redo_steps && a.group_bytes == b.group_bytes;
        }
        const auto preview_after = application_forum_preview_metrics();
        const auto files_after = application_forum_attachments_metrics();
        const bool passed =
            preview_before.admissions == preview_after.admissions &&
            preview_before.committed_bytes == preview_after.committed_bytes &&
            files_before.committed_bytes == files_after.committed_bytes &&
            files_before.draft_bytes == files_after.draft_bytes &&
            surface_motion_probe_detail::same(before, after) &&
            before.source_refreshes == after.source_refreshes && stable_costs &&
            bytes_equal(source_before, source_after, sizeof(source_before)) &&
            external.ui_scene->layout_passes == shell && measure.backend_calls == shapes &&
            external.painted_frames == painted && external.unchanged_present_skips - skips == 64 &&
            forum_post_ui_adapter_submissions() == submits &&
            forum_post_ui_adapter_cancellations() == cancels &&
            application_forum_post_metrics().publication_revision == owner.publication_revision &&
            application_forum_post_metrics().owner_bytes == owner.owner_bytes &&
            application_forum_post_metrics().owner_committed_bytes == owner.owner_committed_bytes &&
            application_forum_post_view().revision == view_revision;
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "sample", sample);
        if (media_probe) {
            log_u64(LogLevel::info, "forum_media_warm", "preview_admission_delta",
                    preview_after.admissions - preview_before.admissions);
            log_u64(LogLevel::info, "forum_media_warm", "preview_committed_bytes",
                    preview_before.committed_bytes);
            log_u64(LogLevel::info, "forum_media_warm", "attachment_committed_bytes",
                    files_before.committed_bytes);
            log_u64(LogLevel::info, "forum_media_warm", "attachment_draft_bytes", files_before.draft_bytes);
        }
        const char* source_labels[]{"message_rebuild_delta", "message_layout_delta",
                                    "composer_layout_delta", "rail_layout_delta",
                                    "sidebar_layout_delta",  "navigation_delta"};
        for (u32 item = 0; item < sizeof(source_before) / sizeof(source_before[0]); ++item)
            log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm",
                    source_labels[item], source_after[item] - source_before[item]);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "paint_build_delta",
                after.builds - before.builds);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "text_build_delta",
                after.text_rebuilds - before.text_rebuilds);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "glyph_build_delta",
                after.glyph_builds - before.glyph_builds);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "paint_upload_delta",
                after.paint_uploads - before.paint_uploads);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "text_upload_delta",
                after.text_uploads - before.text_uploads);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "media_upload_delta",
                after.media_uploads - before.media_uploads);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm",
                "source_refresh_delta", after.source_refreshes - before.source_refreshes);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "shell_layout_delta",
                external.ui_scene->layout_passes - shell);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "measure_delta",
                measure.backend_calls - shapes);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "form_measure_delta",
                after_costs.measure_calls - before_costs.measure_calls);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "scene_commit_delta",
                after_costs.scene_commit_count - before_costs.scene_commit_count);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm",
                "transport_submit_delta", forum_post_ui_adapter_submissions() - submits);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "painted_frame_delta",
                external.painted_frames - painted);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "unchanged_skips",
                external.unchanged_present_skips - skips);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "owner_bytes",
                owner.owner_bytes);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm",
                "owner_committed_bytes", owner.owner_committed_bytes);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "form_bytes",
                before_costs.form_bytes);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm",
                "form_committed_bytes", before_costs.form_committed_bytes);
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm",
                "scene_committed_bytes", before_costs.scene_committed_bytes);
        for (u32 field = 0; field < 2; ++field) {
            log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm",
                    field ? "body_edit_committed_bytes" : "title_edit_committed_bytes",
                    before_costs.edit_committed_bytes[field]);
            log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm",
                    field ? "body_history_owner_committed_bytes" : "title_history_owner_committed_bytes",
                    before_costs.history_owner_committed_bytes[field]);
            log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm",
                    field ? "body_history_owner_bytes" : "title_history_owner_bytes",
                    before_costs.history[field].owner_bytes);
            log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm",
                    field ? "body_history_payload_bytes" : "title_history_payload_bytes",
                    before_costs.history[field].payload_bytes);
        }
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "request_bytes",
                sizeof(ForumPostRequest));
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm",
                "metadata_snapshot_bytes", sizeof(ForumParentSnapshot));
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "post_snapshot_bytes",
                sizeof(ForumPostSnapshot));
        log_u64(LogLevel::info, media_probe ? "forum_media_warm" : "forum_post_warm", "passed", passed);
        return passed;
    };
    const auto fade = [&](bool media) noexcept {
        auto* scene = forum_post_ui_adapter_scene();
        auto* surface = forum_post_ui_adapter_surface();
        if (!(scene && surface && context.project()))
            return false;
        const u64 serial = scene->content.owner_serial;
        const u32 revision = scene->revision, layout = scene->widgets.layout.revision;
        const u64 signatures[]{scene->text_signature[0], scene->text_signature[1]};
        const UiRect texts[]{surface->geometry[0].text_rectangle, surface->geometry[1].text_rectangle};
        const UiRect carets[]{surface->geometry[0].caret, surface->geometry[1].caret};
        const wchar_t* names[]{media ? L"forum-media-opacity-0.bmp" : L"forum-post-opacity-0.bmp",
                               media ? L"forum-media-opacity-half.bmp" : L"forum-post-opacity-half.bmp",
                               media ? L"forum-media-opacity-1.bmp" : L"forum-post-opacity-1.bmp"};
        const u64 preview_uploads = external.preview_media_uploaded_pixels;
        UiRect preview_clip{};
        const auto preview_rectangle =
            media ? ui_attachment_draft_list_preview(scene->attachment_list, 0, &preview_clip) : UiRect{};
        return surface_motion_probe_detail::measure(
            external, RendererLayerSurface::forum_form, scene->viewport, media, names, [&]() noexcept {
                if (forum_post_ui_adapter_scene() != scene || forum_post_ui_adapter_surface() != surface ||
                    !scene->active || scene->content.owner_serial != serial ||
                    scene->revision != revision || scene->widgets.layout.revision != layout)
                    return false;
                for (u32 field = 0; field < 2; ++field)
                    if (scene->text_signature[field] != signatures[field] ||
                        !ui_rect_equal(surface->geometry[field].text_rectangle, texts[field]) ||
                        !ui_rect_equal(surface->geometry[field].caret, carets[field]))
                        return false;
                if (external.preview_media_uploaded_pixels != preview_uploads)
                    return false;
                if (media) {
                    UiRect clip{};
                    const auto rectangle =
                        ui_attachment_draft_list_preview(scene->attachment_list, 0, &clip);
                    if (!ui_rect_equal(rectangle, preview_rectangle) || !ui_rect_equal(clip, preview_clip))
                        return false;
                }
                return true;
            });
    };
    if (media_probe)
        return forum_media_ui_scenarios(external, context.theme, begin, resize, show, warm, metadata, type,
                                        [&]() noexcept { return fade(true); });
    if (!require(begin(), 1) || !require(resize(1100, 1000, 96), 2) ||
        !require(forum_post_ui_adapter_browser() && show(L"forum-post-browser-create.bmp"), 3) ||
        !require(forum_post_ui_adapter_browser_create() &&
                     application_forum_post_view().ticket.parent_id == kForumPostProbeParent &&
                     reply_draft_ui_adapter_state()->selected_channel_id == kReplyDraftProbeChannelA,
                 4) ||
        !require(forum_post_ui_adapter_submit(90001) && show(L"forum-post-metadata-loading.bmp") &&
                     geometry() && warm(1),
                 5))
        return false;
    Utf8View body{};
    ForumPostRequest flight{};
    u64 id{};
    if (!require(forum_post_ui_adapter_flight(&flight, &id) &&
                     flight.operation == ForumPostOperation::parent_metadata &&
                     !forum_post_ui_adapter_response(200, {}, RestClientError::none, 90099) &&
                     application_forum_post_metrics().flight_active &&
                     forum_post_ui_adapter_metadata(true, &body) &&
                     forum_post_ui_adapter_response(200, body) &&
                     application_forum_post_view().require_tag && type(),
                 6) ||
        !require(forum_post_ui_adapter_input(UiInputKind::key_down, VK_RETURN, false, true) &&
                     application_forum_post_view().error &&
                     application_forum_post_view().phase == ApplicationForumPostPhase::editing &&
                     !application_forum_post_view().can_retry && application_forum_post_view().can_submit &&
                     application_forum_post_view().selected_tags.count == 0 &&
                     forum_post_ui_adapter_submissions() == 1 &&
                     show(L"forum-post-required-tag-validation.bmp"),
                 7) ||
        !require(!application_forum_post_pending(clock_monotonic_milliseconds()) &&
                     forum_post_ui_adapter_submissions() == 1,
                 8))
        return false;
    for (u32 index = 0; index < 5; ++index)
        if (!require(tag(index), 90 + index))
            return false;
    if (!require(application_forum_post_view().selected_tags.count == 5 && tag(5) &&
                     application_forum_post_view().selected_tags.count == 5 && tag(6) &&
                     application_forum_post_view().selected_tags.count == 5 &&
                     !application_forum_post_view().tags[6].enabled,
                 9) ||
        !require(forum_post_ui_adapter_manage_threads(true) && metadata(90003, true) && tag(4) && tag(6) &&
                     application_forum_post_view().tags[6].selected &&
                     application_forum_post_view().selected_tags.count == 5,
                 10) ||
        !require(forum_post_ui_adapter_manage_threads(false), 1101) ||
        !require(metadata(90004, true), 1102) ||
        !require(application_forum_post_view().tags[6].selected &&
                     !application_forum_post_view().tags[6].enabled,
                 1103) ||
        !require(forum_post_ui_adapter_input(UiInputKind::key_down, VK_RETURN, false, true), 1104) ||
        !require(application_forum_post_view().error &&
                     application_forum_post_view().phase == ApplicationForumPostPhase::editing &&
                     !application_forum_post_view().can_retry && application_forum_post_view().can_submit &&
                     !application_forum_post_pending(clock_monotonic_milliseconds()),
                 1105) ||
        !require(tag(6), 1106) ||
        !require(!application_forum_post_view().tags[6].selected && !application_forum_post_view().error &&
                     application_forum_post_view().selected_tags.count == 4,
                 1107) ||
        !require(tag(4), 1108) ||
        !require(application_forum_post_view().selected_tags.count == 5 &&
                     !application_forum_post_view().error,
                 1109) ||
        !require(show(L"forum-post-unicode-tags-dark.bmp"), 1110) || !require(geometry(), 1111))
        return false;
    if (!require(fade(false), 112))
        return false;
    context.theme = kLightTheme;
    if (!require(resize(960, 1440, 144) && show(L"forum-post-light-640-dpi144.bmp") && geometry() &&
                     warm(4),
                 12) ||
        !require(resize(480, 1440, 144) && show(L"forum-post-light-320-dpi144.bmp") && geometry(), 13))
        return false;
    context.theme = kDefaultTheme;
    if (!require(resize(1100, 1000, 96) && forum_post_ui_adapter_pointer({ForumPostElement::accept, 0}) &&
                     forum_post_ui_adapter_submit(90005) && forum_post_ui_adapter_flight(&flight, &id) &&
                     flight.operation == ForumPostOperation::create &&
                     flight.auto_archive_duration == 4320 && flight.tag_count == 5 &&
                     utf8_equal(flight.title, utf8_literal("Release 日本語 🚀")) &&
                     utf8_equal(flight.content,
                                utf8_literal("First line 日本語\nSecond line 🚀\nTSF paste か")) &&
                     show(L"forum-post-creating.bmp") && geometry() && warm(2),
                 14))
        return false;
    const u32 title_bytes = forum_post_ui_adapter_edit(0)->storage.bytes,
              body_bytes = forum_post_ui_adapter_edit(1)->storage.bytes;
    if (!require(forum_post_ui_adapter_input(UiInputKind::character, 'X') &&
                     forum_post_ui_adapter_edit(0)->storage.bytes == title_bytes &&
                     forum_post_ui_adapter_edit(1)->storage.bytes == body_bytes &&
                     forum_post_ui_adapter_response(0, {}, RestClientError::timeout) &&
                     application_forum_post_view().uncertain && !application_forum_post_next_wake() &&
                     show(L"forum-post-ambiguous-duplicate-risk.bmp") && geometry() && warm(3),
                 15))
        return false;
    const u32 uncertain_submits = forum_post_ui_adapter_submissions();
    if (!require(forum_post_ui_adapter_pointer({ForumPostElement::accept, 0}) &&
                     !application_forum_post_pending(clock_monotonic_milliseconds()) &&
                     forum_post_ui_adapter_submissions() == uncertain_submits &&
                     forum_post_ui_adapter_pointer({ForumPostElement::inspect, 0}) &&
                     !forum_post_ui_adapter_scene() && application_thread_browser_active() &&
                     utf8_equal(application_forum_post_view().title, utf8_literal("Release 日本語 🚀")) &&
                     forum_post_ui_adapter_browser_create() && application_forum_post_view().uncertain &&
                     utf8_equal(text(forum_post_ui_adapter_edit(1)),
                                utf8_literal("First line 日本語\nSecond line 🚀\nTSF paste か")) &&
                     forum_post_ui_adapter_pointer({ForumPostElement::risk, 0}) &&
                     forum_post_ui_adapter_pointer({ForumPostElement::accept, 0}) &&
                     metadata(90006, true) &&
                     forum_post_ui_adapter_pointer({ForumPostElement::accept, 0}) &&
                     forum_post_ui_adapter_submit(90007) && forum_post_ui_adapter_response(429) &&
                     !application_forum_post_view().uncertain && show(L"forum-post-explicit-retry.bmp") &&
                     geometry(),
                 16))
        return false;
    if (!require(forum_post_ui_adapter_pointer({ForumPostElement::accept, 0}) &&
                     forum_post_ui_adapter_reject_before_wake(90008),
                 17))
        return false;
    Sleep(60); // The actual typed 429 receipt owns a production 50ms floor.
    if (!require(metadata(90008, true) && forum_post_ui_adapter_pointer({ForumPostElement::accept, 0}) &&
                     forum_post_ui_adapter_submit(90009) && forum_post_ui_adapter_response(403) &&
                     !application_forum_post_view().can_retry &&
                     show(L"forum-post-terminal-permission.bmp") && geometry() &&
                     forum_post_ui_adapter_pointer({ForumPostElement::cancel, 0}) &&
                     !forum_post_ui_adapter_scene(),
                 18))
        return false;
    context.ready = false;
    forum_post_ui_adapter_end();
    if (!require(begin() && forum_post_ui_adapter_open() && metadata(91001, false) &&
                     !application_forum_post_view().require_tag && type() &&
                     application_forum_post_view().selected_tags.count == 0 &&
                     forum_post_ui_adapter_input(UiInputKind::key_down, VK_RETURN, false, true) &&
                     forum_post_ui_adapter_submit(91002) && forum_post_ui_adapter_success(&body) &&
                     forum_post_ui_adapter_response(201, body) && !forum_post_ui_adapter_scene() &&
                     reply_draft_ui_adapter_state()->selected_channel_id == kForumPostProbeThread,
                 19))
        return false;
    const auto* timeline = reply_draft_ui_adapter_timeline();
    const auto found = message_store_find(*timeline->store, kForumPostProbeMessage);
    const auto* message = found.ok() ? message_store_resolve(*timeline->store, found.handle) : nullptr;
    if (!require(message && message->channel_id == kForumPostProbeThread &&
                     message->author_id == kReplyDraftProbeSelf &&
                     message->delivery == MessageDeliveryState::authoritative &&
                     timeline->store->count == 1 &&
                     utf8_equal(message_store_content(*timeline->store, found.handle),
                                utf8_literal("First line 日本語\nSecond line 🚀\nTSF paste か")) &&
                     show(L"forum-post-created-thread-starter.bmp"),
                 20))
        return false;
    // This extra metadata publication is outside every planned capture state.
    // The Core/host keeps the empty name; only native presentation supplies a label.
    context.ready = false;
    forum_post_ui_adapter_end();
    if (!require(begin() && forum_post_ui_adapter_open() && forum_post_ui_adapter_submit(94001) &&
                     forum_post_ui_adapter_metadata(false, &body, true) &&
                     forum_post_ui_adapter_response(200, body),
                 60))
        return false;
    const auto unnamed = application_forum_post_view();
    auto* unnamed_scene = forum_post_ui_adapter_scene();
    if (!require(unnamed.tags.count == 7 && unnamed.tags[1].id == 97502 && !unnamed.tags[1].name.bytes &&
                     unnamed_scene && unnamed_scene->content.tags.count == 7 &&
                     unnamed_scene->content.tags[1].id == unnamed.tags[1].id &&
                     utf8_equal(unnamed_scene->content.tags[1].name, utf8_literal("Unnamed tag")) &&
                     tag(1) && application_forum_post_view().selected_tags.count == 1 &&
                     application_forum_post_view().selected_tags[0] == unnamed.tags[1].id && tag(1) &&
                     !application_forum_post_view().selected_tags.count &&
                     forum_post_ui_adapter_drop_files(),
                 61))
        return false;
    log_write(LogLevel::info, "probe",
              "forum_post_native_empty_tag_projection_and_drop_handle_release_passed");
    for (u8 reason = 0; reason < 4; ++reason) {
        context.ready = false;
        forum_post_ui_adapter_end();
        if (!require(begin() && forum_post_ui_adapter_open() && metadata(92000 + reason, false) && type() &&
                         forum_post_ui_adapter_input(UiInputKind::key_down, VK_RETURN, false, true) &&
                         forum_post_ui_adapter_submit(93000 + reason) &&
                         forum_post_ui_adapter_success(&body),
                     30 + reason))
            return false;
        const auto retired = application_forum_post_view().ticket;
        if (reason == 1 && !require(forum_post_ui_adapter_pointer_down({ForumPostElement::cancel, 0}), 40))
            return false;
        const bool withdrawn =
            reason == 3 ? forum_post_ui_adapter_escape_tails() : forum_post_ui_adapter_retire(reason);
        if (!require(
                withdrawn && !forum_post_ui_adapter_scene() && forum_post_ui_adapter_cancellations() == 1 &&
                    !application_forum_post_current(retired) &&
                    !forum_post_ui_adapter_response(201, body, RestClientError::none, 99999) &&
                    application_forum_post_metrics().flight_active &&
                    forum_post_ui_adapter_response(201, body) &&
                    !application_forum_post_metrics().flight_active &&
                    !chat_state_store_find_channel(*reply_draft_ui_adapter_state(),
                                                      kForumPostProbeThread) &&
                    !message_store_find(*reply_draft_ui_adapter_timeline()->store, kForumPostProbeMessage)
                         .ok() &&
                    reply_draft_ui_adapter_state()->selected_channel_id == kReplyDraftProbeChannelA,
                50 + reason))
            return false;
        if (reason == 1 &&
            !require(forum_post_ui_adapter_open() && forum_post_ui_adapter_pointer_up() &&
                         forum_post_ui_adapter_scene() && forum_post_ui_adapter_submissions() == 2 &&
                         forum_post_ui_adapter_escape_tails(),
                     41))
            return false;
    }
    log_write(
        LogLevel::info, "probe",
        "forum_post_actual_host_browser_launcher_decoder_native_fields_tags_tsf_exact_receipts_passed");
    log_write(LogLevel::info, "probe",
              "forum_post_fixture_boundaries_typed_offline_transport_borrowed_shell_projection_core_"
              "composer_timeline_navigation_pending_jump_seam_starter_published_by_production_host");
    log_write(LogLevel::info, "probe",
              "forum_post_native_tsf_paste_selection_composition_uses_actual_textstore_with_test_"
              "composition_view_os_sink_detached_and_restored");
    return true;
}
} // namespace maicord
