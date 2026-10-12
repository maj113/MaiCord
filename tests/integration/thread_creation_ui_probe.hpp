#pragma once
#include "thread_creation_ui_adapter.hpp"
#include "reply_draft_ui_probe.hpp"
#include "maicord/core/time.hpp"
namespace maicord {
inline bool thread_creation_ui_probe(D3D11Renderer& external, UiTextMeasureCache& measure, HWND window,
    bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept {
    using namespace app_detail;
    using namespace reply_draft_probe_detail;
    if (!window || !external.ui_scene || !external.guild_rail_scene || !external.shell_navigation_scene) return false;
    struct Context {
        D3D11Renderer& external; UiTextMeasureCache& measure;
        NativeMessages native{}; NavigationView navigation{};
        GuildViewRow guilds[2]{}; ChannelViewRow channels[8]{};
        u16 scratch[8]{}; u8 visibility[8]{}; ChannelSortKey keys[8]{};
        Theme theme{kDefaultTheme}; bool ready{};
        bool project() noexcept {
            if (!ready) return true;
            auto& r = thread_creation_ui_adapter_renderer(external);
            auto* state = reply_draft_ui_adapter_state(); auto* users = reply_draft_ui_adapter_users();
            auto* model = reply_draft_ui_adapter_composer(); auto* composer = reply_draft_ui_adapter_scene();
            if (!state || !users || !model || !navigation_view_rebuild(&navigation, *state).ok()) return false;
            auto* shell = const_cast<UiScene*>(r.ui_scene);
            auto metrics = kGuildChannelShellLayoutMetrics;
            metrics.flags = static_cast<u16>(metrics.flags & ~ui_shell_layout_members);
            metrics.member_sidebar_width = 0; metrics.composer_height = 48;
            metrics.revision = 4400;
            if (!ui_scene_layout_with_metrics(shell, UiShellView::guild_channel, metrics,
                r.width * 96.f / r.dpi, r.height * 96.f / r.dpi) ||
                !guild_rail_scene_update(const_cast<GuildRailScene*>(r.guild_rail_scene), &navigation,
                    ui_scene_content_rectangle(*shell, UiSemantic::guild_navigation), 0).ok() ||
                !shell_navigation_scene_update(const_cast<ShellNavigationScene*>(r.shell_navigation_scene), &navigation,
                    ui_scene_content_rectangle(*shell, UiSemantic::channel_navigation), 0).ok()) return false;
            const MessageComposerProjectionContext projection{users, state, navigation.selected_guild_id};
            if (!message_composer_scene_update(composer, model, ui_scene_content_rectangle(*shell, UiSemantic::composer),
                false, 0, {}, &projection).ok() || !renderer_bind_message_composer_scene(&r, composer)) return false;
            model->focused = false; message_composer_scene_set_caret_visible(composer, false);
            if (!message_scene_update(&native.scene, state->selected_channel_id == kReplyDraftProbeChannelA
                ? ui_scene_content_rectangle(*shell, UiSemantic::message_view) : UiRect{}).ok() ||
                !renderer_bind_message_scene(&r, &native.scene)) return false;
            if (r.message_text != &native.text) {
                renderer_unbind_message_text_presentation(&r);
                if (!renderer_bind_message_text_presentation(&r, &native.text)) return false;
            }
            r.chat_state = state; r.user_store = users; r.navigation_view = &navigation;
            r.message_measure_cache = &measure;
            auto* form = thread_creation_ui_adapter_form();
            r.focus_owner = form ? &form->scene.widgets : &composer->widgets;
            thread_creation_ui_adapter_viewport(r, *shell);
            if (r.popup_scene != reply_draft_ui_adapter_popup() &&
                !renderer_bind_popup_scene(&r, reply_draft_ui_adapter_popup())) return false;
            if (!native.scene.active) return renderer_rebuild_content_text(&r, theme);
            return message_text_coordinator_converge({&native.scene, &native.text, &measure, state, users,
                &navigation, &theme, +[](void* owner, const Theme& current) noexcept {
                    return renderer_rebuild_content_text(static_cast<D3D11Renderer*>(owner), current);
                }, &r, 1'759'667'400ull, static_cast<u16>(r.dpi), nullptr}).ok();
        }
    } context{external, measure};
    struct Restore {
        Context& c; const MessageScene* scene; const MessageTextPresentation* text;
        const MessageComposerScene* composer; const PopupScene* popup; TextFormSurface* form;
        const ChatStateStore* state; const UserStore* users; const NavigationView* navigation;
        const UiWidgetManager* focus; UiTextMeasureCache* cache; u32 dpi;
        ~Restore() noexcept {
            c.ready = false; thread_creation_ui_adapter_end();
            auto& r = c.external;
            r.message_scene = scene; r.message_text = text; r.message_composer_scene = composer;
            r.popup_scene = popup; r.text_form = form; r.chat_state = state; r.user_store = users;
            r.navigation_view = navigation; r.focus_owner = focus; r.message_measure_cache = cache;
            if (r.guild_rail_scene) guild_rail_scene_update(const_cast<GuildRailScene*>(r.guild_rail_scene),
                navigation, ui_scene_content_rectangle(*r.ui_scene, UiSemantic::guild_navigation), 0);
            if (r.shell_navigation_scene) shell_navigation_scene_update(const_cast<ShellNavigationScene*>(r.shell_navigation_scene),
                navigation, ui_scene_content_rectangle(*r.ui_scene, UiSemantic::channel_navigation), 0);
            renderer_set_dpi(&r, dpi); r.retained_presentation.valid = false;
            content_text_retention_invalidate(&r.content_text_retention);
        }
    } restore{context, external.message_scene, external.message_text, external.message_composer_scene,
        external.popup_scene, external.text_form, external.chat_state, external.user_store,
        external.navigation_view, external.focus_owner, external.message_measure_cache, external.dpi};
    const auto require = [](bool value, u32 stage) noexcept {
        if (!value) log_u64(LogLevel::error, "probe", "thread_creation_native_failure_stage", stage);
        return value;
    };
    const auto begin = [&]() noexcept {
        context.ready = false;
        if (!thread_creation_ui_adapter_begin(window, external, measure, +[](void* value) noexcept {
                return static_cast<Context*>(value)->project();
            }, &context)) return false;
        message_scene_release(&context.native.scene);
        if (!context.native.initialize() || !navigation_view_initialize(&context.navigation,
            {context.guilds, 2}, {context.channels, 8}, {context.scratch, 8}, {context.visibility, 8}, {context.keys, 8})) return false;
        context.ready = true; return context.project();
    };
    const auto resize = [&](u32 width, u32 height, u32 dpi) noexcept {
        external.chat_state = restore.state; external.user_store = restore.users; external.navigation_view = restore.navigation;
        renderer_set_dpi(&external, dpi);
        return probe_resize_surface(width, height) && context.project() && thread_creation_ui_adapter_synchronize();
    };
    const auto show = [&](const wchar_t* name) noexcept {
        return context.project() && probe_present(&external, context.theme) && (!capture || capture(name, context.theme));
    };
    const auto geometry = [&]() noexcept {
        auto* form = thread_creation_ui_adapter_form();
        if (!form || !form->request.single_line || form->request.maximum_bytes != 400 || form->request.maximum_codepoints != 100) return false;
        const auto* edit = text_form_node(form->scene, TextFormElement::edit);
        const auto* accept = text_form_node(form->scene, TextFormElement::accept);
        const auto* cancel = text_form_node(form->scene, TextFormElement::cancel);
        if (!edit || !accept || !cancel || ui_rect_height(edit->rectangle) != 44 ||
            !ui_rect_equal(edit->rectangle, ui_rect_intersection(edit->rectangle, edit->clip_rectangle)) ||
            ui_rect_has_area(ui_rect_intersection(accept->rectangle, cancel->rectangle)) ||
            ui_rect_has_area(ui_rect_intersection(edit->rectangle, accept->rectangle)) ||
            accept->rectangle.top != cancel->rectangle.top || accept->rectangle.bottom != cancel->rectangle.bottom)
            return false;
        const UiLayoutNode* buttons[]{cancel, accept};
        const Utf8View labels[]{form->scene.content.cancel, form->scene.content.accept};
        float normal_height = 36.f;
        for (u32 item = 0; item < 2; ++item) {
            const auto text_rectangle = ui_rect_intersection(buttons[item]->content_rectangle, buttons[item]->clip_rectangle);
            UiTextMeasureValue native_label{};
            const UiTextMeasureRequest request{{0x4352454154455549ull, item + 1ull, form->scene.revision,
                static_cast<u16>(item), 1}, labels[item], {},
                {ui_rect_width(text_rectangle), ui_rect_height(text_rectangle)}, true};
            if (!ui_rect_has_area(text_rectangle) || !renderer_text_measure_backend(&external, request, &native_label) ||
                native_label.line_count != 1 || native_label.size.width > ui_rect_width(text_rectangle) + .01f ||
                native_label.size.height > ui_rect_height(text_rectangle) + .01f) return false;
            normal_height = maximum(normal_height, native_label.size.height + 16.f);
        }
        return ui_rect_height(accept->rectangle) >= normal_height - .01f &&
            ui_rect_height(accept->rectangle) <= normal_height + .01f;
    };
    const auto type = [&]() noexcept {
        const u32 name[]{'R','e','l','e','a','s','e',' ',0x65e5,0x672c,0x8a9e,' ',0xd83d,0xde80};
        for (u32 unit : name) if (!thread_creation_ui_adapter_input(UiInputKind::character, unit)) return false;
        return true;
    };
    const auto warm = [&](u32 sample) noexcept {
        if (!context.project() || !probe_present(&external, context.theme)) return false;
        auto* form = thread_creation_ui_adapter_form(); if (!form) return false;
        const auto before = surface_motion_probe_detail::snapshot(external);
        const u32 layouts = form->scene.widgets.layout.revision, shell = external.ui_scene->layout_passes,
            shapes = measure.backend_calls, painted = external.painted_frames, skips = external.unchanged_present_skips,
            revision = form->scene.revision, submissions = thread_creation_ui_adapter_submissions();
        const auto owner = application_thread_creation_metrics();
        const u32 form_shapes = form->scene.measure->backend_calls;
        const usize form_committed = form->scene.storage.arena.committed;
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(form, &memory, sizeof(memory)) || memory.State != MEM_COMMIT) return false;
        for (u32 frame = 0; frame < 64; ++frame)
            if (!thread_creation_ui_adapter_synchronize() || !context.project() || !probe_present(&external, context.theme)) return false;
        const auto after = surface_motion_probe_detail::snapshot(external);
        const bool passed = surface_motion_probe_detail::same(before, after) && before.source_refreshes == after.source_refreshes &&
            form->scene.widgets.layout.revision == layouts && form->scene.revision == revision &&
            external.ui_scene->layout_passes == shell && measure.backend_calls == shapes && external.painted_frames == painted &&
            external.unchanged_present_skips - skips == 64 && thread_creation_ui_adapter_submissions() == submissions &&
            application_thread_creation_metrics().publication_revision == owner.publication_revision &&
            form->scene.measure->backend_calls == form_shapes && form->scene.storage.arena.committed == form_committed;
        log_u64(LogLevel::info, "thread_creation_warm", "sample", sample);
        log_u64(LogLevel::info, "thread_creation_warm", "unchanged_skips", external.unchanged_present_skips - skips);
        log_u64(LogLevel::info, "thread_creation_warm", "owner_bytes", owner.owner_bytes);
        log_u64(LogLevel::info, "thread_creation_warm", "owner_committed_bytes", owner.owner_committed_bytes);
        log_u64(LogLevel::info, "thread_creation_warm", "form_bytes", sizeof(ApplicationTextForm));
        log_u64(LogLevel::info, "thread_creation_warm", "form_scene_committed_bytes", form_committed);
        log_u64(LogLevel::info, "thread_creation_warm", "form_owner_committed_bytes", memory.RegionSize);
        log_u64(LogLevel::info, "thread_creation_warm", "request_bytes", sizeof(ThreadCreationRequest));
        log_u64(LogLevel::info, "thread_creation_warm", "passed", passed); return passed;
    };
    if (!require(begin(), 1) || !require(resize(1100, 760, 96), 2) ||
        !require(thread_creation_ui_adapter_menu() && show(L"thread-creation-menu.bmp"), 3)) return false;
    reply_draft_ui_adapter_close_menu();
    if (!require(thread_creation_ui_adapter_open() && geometry(), 4) ||
        !require(thread_creation_ui_adapter_input(UiInputKind::key_down, VK_RETURN) &&
            application_thread_creation_view().error && !thread_creation_ui_adapter_submissions() &&
            show(L"thread-creation-empty-validation.bmp"), 5) ||
        !require(type() && show(L"thread-creation-unicode.bmp"), 6) ||
        !require(thread_creation_ui_adapter_input(UiInputKind::key_down, VK_RETURN) &&
            thread_creation_ui_adapter_submit(81001) && thread_creation_ui_adapter_synchronize(), 7)) return false;
    ThreadCreationRequest flight{}; u64 id{};
    if (!require(thread_creation_ui_adapter_flight(&flight, &id) && id == 81001 &&
        flight.operation == ThreadCreationOperation::from_message && flight.auto_archive_duration == 4320 &&
        flight.source_message_id == kReplyDraftProbeMessage && utf8_equal(flight.name, utf8_literal("Release 日本語 🚀")), 8)) return false;
    auto* busy = thread_creation_ui_adapter_form();
    const u32 bytes = busy ? busy->edit.storage.bytes : 0;
    if (!require(busy && !busy->editable && !busy->can_accept &&
        thread_creation_ui_adapter_input(UiInputKind::character, 'X') && busy->edit.storage.bytes == bytes &&
        show(L"thread-creation-submitting.bmp") && warm(1), 9) ||
        !require(!thread_creation_ui_adapter_response(200, {}, RestClientError::none, 81002) &&
            application_thread_creation_metrics().flight_active, 10) ||
        !require(thread_creation_ui_adapter_response(0, {}, RestClientError::timeout) &&
            application_thread_creation_view().uncertain && thread_creation_ui_adapter_pointer(TextFormElement::accept) &&
            thread_creation_ui_adapter_submit(81003) && thread_creation_ui_adapter_synchronize() &&
            thread_creation_ui_adapter_flight(&flight, &id) && flight.operation == ThreadCreationOperation::reconcile &&
            thread_creation_ui_adapter_form() &&
            utf8_equal(thread_creation_ui_adapter_form()->scene.content.accept, utf8_literal("Check again")) &&
            show(L"thread-creation-checking.bmp") && geometry() && warm(2), 11) ||
        !require(thread_creation_ui_adapter_response(404) && thread_creation_ui_adapter_submit(81004) &&
            thread_creation_ui_adapter_response(429) && !application_thread_creation_view().uncertain &&
            application_thread_creation_view().can_retry && show(L"thread-creation-explicit-retry.bmp"), 12)) return false;
    // The production floor is 50ms for this typed 429 receipt.
    Sleep(60);
    if (!require(thread_creation_ui_adapter_pointer(TextFormElement::accept) && thread_creation_ui_adapter_submit(81005) &&
        thread_creation_ui_adapter_response(403) && !application_thread_creation_view().can_retry &&
        show(L"thread-creation-terminal.bmp") && thread_creation_ui_adapter_pointer(TextFormElement::cancel) &&
        !thread_creation_ui_adapter_form(), 13)) return false;
    context.ready = false; thread_creation_ui_adapter_end();
    if (!require(begin() && resize(960, 900, 144) && thread_creation_ui_adapter_open() && type(), 14)) return false;
    context.theme = kLightTheme;
    if (!require(show(L"thread-creation-narrow-light-144.bmp") && geometry() &&
        thread_creation_ui_adapter_input(UiInputKind::key_down, VK_TAB) && warm(3) &&
        thread_creation_ui_adapter_pointer(TextFormElement::accept) && thread_creation_ui_adapter_submit(81006), 15)) return false;
    const auto success = utf8_literal(R"({"id":"97200","guild_id":"97000","parent_id":"97100","type":11,"name":"Release 日本語 🚀","thread_metadata":{"archived":false,"locked":false,"archive_timestamp":"2026-10-05T12:00:00.000Z","auto_archive_duration":4320},"member":{"id":"97200","user_id":"97005","join_timestamp":"2026-10-05T12:00:00.000Z","flags":0}})");
    if (!require(thread_creation_ui_adapter_response(201, success) && !thread_creation_ui_adapter_form() &&
        chat_state_store_find_channel(*reply_draft_ui_adapter_state(), kReplyDraftProbeMessage) &&
        reply_draft_ui_adapter_state()->selected_channel_id == kReplyDraftProbeMessage &&
        show(L"thread-creation-created-core-thread.bmp"), 16)) return false;
    // Each withdrawal runs against a fresh real actor/store, not the completed source.
    for (u8 reason = 0; reason < 4; ++reason) {
        context.ready = false; thread_creation_ui_adapter_end();
        if (!require(begin() && thread_creation_ui_adapter_open() && type() &&
            thread_creation_ui_adapter_input(UiInputKind::key_down, VK_RETURN) && thread_creation_ui_adapter_submit(82000 + reason), 20 + reason)) return false;
        const auto retired = application_thread_creation_view().ticket;
        if (reason == 1 && !require(thread_creation_ui_adapter_pointer_down(TextFormElement::cancel), 41)) return false;
        const bool withdrawn = reason == 3 ? thread_creation_ui_adapter_input(UiInputKind::key_down, VK_ESCAPE) :
            thread_creation_ui_adapter_retire(reason);
        if (!require(withdrawn && !thread_creation_ui_adapter_form() &&
            thread_creation_ui_adapter_cancellations() == 1 && !application_thread_creation_current(retired) &&
            thread_creation_ui_adapter_response(201, success) && !application_thread_creation_metrics().flight_active &&
            !chat_state_store_find_channel(*reply_draft_ui_adapter_state(), kReplyDraftProbeMessage) &&
            reply_draft_ui_adapter_state()->selected_channel_id == kReplyDraftProbeChannelA, 30 + reason)) return false;
        if (reason == 1 && !require(thread_creation_ui_adapter_open() && thread_creation_ui_adapter_pointer_up() &&
            thread_creation_ui_adapter_form() && thread_creation_ui_adapter_submissions() == 1 &&
            thread_creation_ui_adapter_input(UiInputKind::key_down, VK_ESCAPE), 42)) return false;
    }
    log_write(LogLevel::info, "probe", "thread_creation_actual_host_decoder_form_native_input_exact_receipts_passed");
    log_write(LogLevel::info, "probe", "thread_creation_fixture_boundaries_typed_offline_transport_menu_controller_direct_host_activation_core_navigation_stub");
    return true;
}
} // namespace maicord
