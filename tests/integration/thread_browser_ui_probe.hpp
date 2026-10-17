#pragma once
#include "thread_browser_ui_adapter.hpp"
#include "probe_present.hpp"
#include "ui_surface_motion_measurement.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/shell_text_presentation.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
bool probe_resize_surface(u32, u32) noexcept;
inline bool thread_browser_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache& measure, HWND window,
    bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept {
    using namespace app_detail;
    if (!window || !renderer.ui_scene || !renderer.content_text || !renderer.guild_rail_scene ||
        !renderer.shell_navigation_scene) return false;
    struct Context {
        D3D11Renderer& renderer;
        UiTextMeasureCache& measure;
        NavigationView navigation{};
        GuildViewRow guilds[2]{};
        ChannelViewRow channels[256]{};
        u16 scratch[256]{};
        u8 visibility[256]{};
        ChannelSortKey keys[256]{};
        ShellNavigationScene sidebar{};
        UiVirtualRowStorageOwner sidebar_rows{};
        UiWidgetStorageOwner sidebar_widgets{};
        Theme theme{kDefaultTheme};
        bool ready{};
        ~Context() noexcept {
            ui_virtual_row_storage_release(&sidebar_rows);
            ui_widget_storage_release(&sidebar_widgets);
        }
        bool initialize_sidebar() noexcept {
            if (!ui_virtual_row_storage_initialize(&sidebar_rows, 256, 16,
                    sizeof(ShellNavigationRowWidget), alignof(ShellNavigationRowWidget)) ||
                !ui_widget_storage_initialize(&sidebar_widgets, 4 * 256 + 2, 4 * 16 + 2) ||
                !shell_navigation_scene_initialize(&sidebar,
                    ui_virtual_row_storage_rows<ShellNavigationRowWidget>(sidebar_rows),
                    ui_widget_storage_widgets(sidebar_widgets), ui_widget_storage_layout_nodes(sidebar_widgets)) ||
                !ui_widget_manager_bind_growable_storage(&sidebar.widgets, ui_widget_storage_grow, &sidebar_widgets)) return false;
            sidebar.row_storage = &sidebar_rows;
            return true;
        }
        bool project() noexcept {
            if (!ready) return true;
            const auto failed = [](u32 step) noexcept {
                log_u64(LogLevel::error, "thread_browser", "native_projection_failure_step", step);
                return false;
            };
            auto* state = thread_browser_ui_adapter_state();
            auto* shell = const_cast<UiScene*>(renderer.ui_scene);
            auto metrics = kGuildChannelShellLayoutMetrics;
            metrics.flags = static_cast<u16>(metrics.flags & ~ui_shell_layout_members);
            if (application_thread_browser_available())
                metrics.flags = static_cast<u16>(metrics.flags | ui_shell_layout_threads);
            metrics.member_sidebar_width = 0;
            metrics.revision += static_cast<u32>(metrics.flags) * 7;
            if (!state || !navigation_view_rebuild(&navigation, *state).ok()) return failed(1);
            if (!ui_scene_layout_with_metrics(shell, UiShellView::guild_channel, metrics,
                renderer.width * 96.f / renderer.dpi, renderer.height * 96.f / renderer.dpi)) return failed(2);
            if (!guild_rail_scene_update(const_cast<GuildRailScene*>(renderer.guild_rail_scene), &navigation,
                ui_scene_content_rectangle(*shell, UiSemantic::guild_navigation), 0).ok()) return failed(3);
            const auto navigation_result = shell_navigation_scene_update(&sidebar, &navigation,
                ui_scene_content_rectangle(*shell, UiSemantic::channel_navigation), sidebar.scroll_dips);
            if (!navigation_result.ok()) {
                log_u64(LogLevel::error, "thread_browser", "native_sidebar_error", static_cast<u64>(navigation_result.error));
                return failed(4);
            }
            if (!renderer_bind_shell_navigation_scene(&renderer, &sidebar)) return failed(8);
            renderer.chat_state = state;
            renderer.user_store = thread_browser_ui_adapter_users();
            renderer.navigation_view = &navigation;
            renderer.focus_owner = nullptr;
            // probe_resize_surface rebinds its original conversation owners.
            // This content panel must explicitly remove them on every projection.
            renderer.message_scene = nullptr; renderer.message_text = nullptr;
            renderer.message_composer_scene = nullptr; renderer.popup_scene = nullptr;
            thread_browser_ui_adapter_viewport(renderer, *shell);
            if (!application_thread_browser_synchronize()) return failed(5);
            if (!renderer_bind_thread_browser_scene(&renderer, application_thread_browser_scene())) return failed(6);
            if (!renderer_rebuild_content_text(&renderer, theme)) {
                log_u64(LogLevel::error, "thread_browser", "native_text_build_error", static_cast<u64>(renderer.content_text->last_error));
                log_u64(LogLevel::error, "thread_browser", "native_text_hresult", static_cast<u32>(renderer.content_text->native_error));
                return failed(7);
            }
            return true;
        }
    } context{renderer, measure};
    struct Restore {
        Context& c;
        const ChatStateStore* state;
        const UserStore* users;
        const NavigationView* navigation;
        const UiWidgetManager* focus;
        const ShellNavigationScene* sidebar;
        UiThreadBrowserScene* browser;
        const MessageScene* messages;
        const MessageTextPresentation* text;
        const MessageComposerScene* composer;
        const PopupScene* popup;
        u32 width, height, dpi;
        ~Restore() noexcept {
            c.ready = false;
            auto& r = c.renderer;
            renderer_bind_thread_browser_scene(&r, browser);
            r.chat_state = state; r.user_store = users; r.navigation_view = navigation;
            r.focus_owner = focus; r.message_scene = messages; r.message_text = text;
            r.message_composer_scene = composer; r.popup_scene = popup;
            renderer_bind_shell_navigation_scene(&r, sidebar);
            thread_browser_ui_adapter_end();
            renderer_set_dpi(&r, dpi);
            probe_resize_surface(width, height);
            r.retained_presentation.valid = false;
            content_text_retention_invalidate(&r.content_text_retention);
        }
    } restore{context, renderer.chat_state, renderer.user_store, renderer.navigation_view, renderer.focus_owner, renderer.shell_navigation_scene,
        renderer.thread_browser_scene, renderer.message_scene, renderer.message_text, renderer.message_composer_scene,
        renderer.popup_scene, renderer.width, renderer.height, renderer.dpi};
    const auto require = [](bool passed, u32 stage) noexcept {
        if (!passed) log_u64(LogLevel::error, "thread_browser", "native_failure_stage", stage);
        return passed;
    };
    if (!require(thread_browser_ui_adapter_begin(window, renderer, +[](void* owner) noexcept {
            return static_cast<Context*>(owner)->project();
        }, &context) && navigation_view_initialize(&context.navigation, {context.guilds, 2}, {context.channels, 256},
            {context.scratch, 256}, {context.visibility, 256}, {context.keys, 256}) && context.initialize_sidebar(), 1)) return false;
    context.ready = true;
    // The panel is actual shipping chrome. Its underlying conversation is
    // deliberately absent, so no retained message/composer fixture can leak in.
    renderer.message_scene = nullptr; renderer.message_text = nullptr;
    renderer.message_composer_scene = nullptr; renderer.popup_scene = nullptr;
    const auto resize = [&](u32 width, u32 height, u32 dpi) noexcept {
        renderer.chat_state = restore.state; renderer.user_store = restore.users; renderer.navigation_view = restore.navigation;
        renderer_set_dpi(&renderer, dpi);
        return probe_resize_surface(width, height) && context.project();
    };
    const auto show = [&](const wchar_t* name) noexcept {
        if (!context.project() || !probe_present(&renderer, context.theme)) return false;
        thread_browser_ui_adapter_viewport(renderer, *renderer.ui_scene);
        return !capture || capture(name, context.theme);
    };
    const auto row_pixels = [&]() noexcept {
        const auto* scene = application_thread_browser_scene();
        if (!scene || !scene->count || !scene->backing->rows[0].generation) return false;
        surface_motion_probe_detail::Readback readback{};
        if (!readback.initialize(renderer) || !d3d11_renderer_draw_stream(&renderer, context.theme, false, false, nullptr) ||
            !readback.read(renderer, 0)) return false;
        const auto card = scene->backing->cards[0];
        const auto region = raster_clip_rectangle(ui_rect_intersection(
            {card.left + 38, card.top + 4, card.right - 8, card.top + 28}, scene->body),
            renderer.dpi / 96.f, readback.width, readback.height);
        const auto ink = context.theme.text_primary;
        const u8 expected[]{static_cast<u8>(ink.blue * 255.f + .5f), static_cast<u8>(ink.green * 255.f + .5f),
            static_cast<u8>(ink.red * 255.f + .5f)};
        u32 pixels = 0;
        for (i32 y = maximum(0, region.top); y < region.bottom; ++y)
            for (i32 x = maximum(0, region.left); x < region.right; ++x)
                if (surface_motion_probe_detail::distance(readback.images + (y * readback.width + x) * 4, expected) < 100 &&
                    ++pixels >= 6) return true;
        return false;
    };
    const auto warm = [&](u32 case_id) noexcept {
        if (!context.project() || !probe_present(&renderer, context.theme) || !probe_present(&renderer, context.theme)) return false;
        auto* scene = application_thread_browser_scene();
        if (!scene) return false;
        const auto owner = application_thread_browser_metrics();
        const u32 before[]{scene->rebuilds, scene->layout_passes, renderer.ui_scene->layout_passes,
            renderer.ui_scene->layout_rebuilds, renderer.guild_rail_scene->layout_passes,
            renderer.shell_navigation_scene->layout_passes, context.navigation.revision, measure.backend_calls,
            renderer.glyph_text.cache_builds, renderer.content_text->rebuilds, renderer.paint_stream_builds,
            renderer.paint_instance_uploads, renderer.text_instance_uploads, renderer.media_instance_uploads,
            renderer.painted_frames, renderer.unchanged_present_skips};
        for (u32 i = 0; i < 64; ++i) if (!context.project() || !probe_present(&renderer, context.theme)) return false;
        const u32 after[]{scene->rebuilds, scene->layout_passes, renderer.ui_scene->layout_passes,
            renderer.ui_scene->layout_rebuilds, renderer.guild_rail_scene->layout_passes,
            renderer.shell_navigation_scene->layout_passes, context.navigation.revision, measure.backend_calls,
            renderer.glyph_text.cache_builds, renderer.content_text->rebuilds, renderer.paint_stream_builds,
            renderer.paint_instance_uploads, renderer.text_instance_uploads, renderer.media_instance_uploads,
            renderer.painted_frames, renderer.unchanged_present_skips};
        const char* labels[]{"scene_rebuild", "scene_layout", "shell_layout", "shell_rebuild", "rail_layout",
            "sidebar_layout", "navigation", "shape", "raster", "text_build", "paint_build", "paint_upload",
            "text_upload", "media_upload", "painted_frames", "unchanged_skips"};
        bool unchanged = true;
        log_u64(LogLevel::info, "thread_browser_warm", "case", case_id);
        for (u32 i = 0; i < 16; ++i) {
            log_u64(LogLevel::info, "thread_browser_warm", labels[i], after[i] - before[i]);
            unchanged &= after[i] - before[i] == (i == 15 ? 64u : 0u);
        }
        const auto current = application_thread_browser_metrics();
        log_u64(LogLevel::info, "thread_browser_warm", "owner_bytes", current.owner_bytes);
        log_u64(LogLevel::info, "thread_browser_warm", "owner_committed_bytes", current.owner_committed_bytes);
        log_u64(LogLevel::info, "thread_browser_warm", "bank_committed_bytes", current.bank_committed_bytes);
        log_u64(LogLevel::info, "thread_browser_warm", "scene_committed_bytes", scene->storage.arena.committed);
        log_u64(LogLevel::info, "thread_browser_warm", "scene_abi_bytes", sizeof(UiThreadBrowserScene));
        log_u64(LogLevel::info, "thread_browser_warm", "view_abi_bytes", sizeof(UiThreadBrowserView));
        log_u64(LogLevel::info, "thread_browser_warm", "target_abi_bytes", sizeof(UiThreadBrowserTarget));
        log_u64(LogLevel::info, "thread_browser_warm", "sidebar_source_rows", context.sidebar.source_count);
        log_u64(LogLevel::info, "thread_browser_warm", "sidebar_materialized_rows", context.sidebar.count);
        log_u64(LogLevel::info, "thread_browser_warm", "sidebar_committed_bytes", context.sidebar_rows.rows.arena.committed +
            context.sidebar_widgets.widgets.arena.committed + context.sidebar_widgets.layout_nodes.arena.committed);
        return unchanged && owner.owner_bytes == current.owner_bytes && owner.bank_committed_bytes == current.bank_committed_bytes &&
            owner.source_publication == current.source_publication;
    };
    u8 json[64 * 1024]{};
    Utf8View page{};
    ThreadBrowserRequest flight{};
    u64 flight_id = 0;
    if (!require(resize(1280, 800, 96), 201) || !require(application_thread_browser_available(), 202) ||
        !require(thread_browser_ui_adapter_header_pointer(true) &&
            thread_browser_ui_adapter_header_pointer(false) && context.project(), 203)) return false;
    const auto* active_scene = application_thread_browser_scene();
    if (!require(active_scene && active_scene->count == 5, 204) ||
        !require(active_scene->view->inventory_synced, 205) || !require(show(L"thread-browser-active.bmp"), 206) ||
        !require(row_pixels(), 207) || !require(warm(0), 208)) return false;
    if (!require(thread_browser_ui_adapter_active_inventory(false, true, false) && context.project() &&
        application_thread_browser_scene()->count == 0 && !application_thread_browser_scene()->view->loading &&
        thread_browser_ui_adapter_submissions() == 0 && show(L"thread-browser-empty-offline.bmp") &&
        thread_browser_ui_adapter_active_inventory(true, false, false) && context.project() &&
        application_thread_browser_scene()->count == 5 && !application_thread_browser_scene()->view->inventory_synced &&
        thread_browser_ui_adapter_submissions() == 0 && thread_browser_ui_adapter_active_inventory(true, true, true), 3)) return false;
    auto* scene = application_thread_browser_scene();
    if (!require(thread_browser_ui_adapter_pointer(scene->archived_tab, true) &&
        thread_browser_ui_adapter_pointer({}, false) && scene->view->loading &&
        thread_browser_ui_adapter_submit(98001) && thread_browser_ui_adapter_flight(&flight, &flight_id) &&
        flight_id == 98001 && flight.mode == ThreadBrowserMode::archived && flight.offset == 0 &&
        !thread_browser_ui_adapter_response(200, {}, RestClientError::none, 98999) &&
        show(L"thread-browser-archive-loading.bmp") &&
        thread_browser_ui_adapter_page({json, sizeof(json)}, 25, kThreadBrowserProbeFirstArchived, true, true, &page) &&
        thread_browser_ui_adapter_response(200, page) && context.project() && scene->count == 25 &&
        scene->view->has_next && utf8_equal({scene->page_text, scene->page_bytes}, utf8_literal("Page 1")) &&
        show(L"thread-browser-archive-page.bmp") && row_pixels() && warm(1), 4)) return false;
    const auto captured = ui_thread_browser_scene_target(*scene, scene->backing->rows[0]);
    if (!require(thread_browser_ui_adapter_pointer(scene->backing->rows[0], true), 5)) return false;
    auto* state = thread_browser_ui_adapter_state();
    const auto* old = chat_state_store_find_channel(*state, captured.thread_id);
    ChannelInput renamed{old->id, old->guild_id, old->parent_id, utf8_literal("Gateway-renamed accepted thread"),
        old->position, old->flags, old->type};
    if (!require(chat_state_store_upsert_channel(state, renamed).ok(), 6)) return false;
    application_thread_browser_gateway(kThreadMembershipProbeGuild, kThreadBrowserProbeParent);
    if (!require(context.project() && !application_thread_browser_activate(captured) &&
        thread_browser_ui_adapter_pointer({}, false) && application_thread_browser_active(), 7)) return false;
    if (!require(ui_thread_browser_scene_focus(scene, {UiThreadBrowserAction::open_thread,
            kThreadBrowserProbeFirstArchived + 24, scene->source_publication}) &&
        thread_browser_ui_adapter_key(VK_TAB) && ui_thread_browser_scene_focused_target(*scene).action == UiThreadBrowserAction::next &&
        thread_browser_ui_adapter_key(VK_SPACE) && thread_browser_ui_adapter_submit(98002) &&
        thread_browser_ui_adapter_flight(&flight, &flight_id) && flight.offset == 25 &&
        thread_browser_ui_adapter_response(500) && show(L"thread-browser-retry.bmp") &&
        thread_browser_ui_adapter_action(UiThreadBrowserAction::retry) && thread_browser_ui_adapter_submit(98003) &&
        thread_browser_ui_adapter_page({json, sizeof(json)}, 1, kThreadBrowserProbeFirstArchived + 25, true, false, &page) &&
        thread_browser_ui_adapter_response(200, page) && context.project() && scene->count == 1 && scene->view->has_previous &&
        !scene->view->has_next && thread_browser_ui_adapter_action(UiThreadBrowserAction::previous) &&
        thread_browser_ui_adapter_submit(98004) && thread_browser_ui_adapter_flight(&flight, &flight_id) && flight.offset == 0 &&
        thread_browser_ui_adapter_page({json, sizeof(json)}, 25, kThreadBrowserProbeFirstArchived, true, true, &page) &&
        thread_browser_ui_adapter_response(200, page), 8)) return false;
    // The panel has no search band or field: the status line follows the tabs.
    u32 text_fields = 0;
    for (u32 i = 0; i < scene->widgets.count; ++i)
        text_fields += scene->widgets.storage[i].kind == UiWidgetKind::text_field ? 1u : 0u;
    if (!require(context.project() && !text_fields && scene->status.top == scene->tabs.bottom &&
        scene->body.top - scene->panel.top == scene->style.header_height + scene->style.tabs_height +
            scene->style.status_height, 9)) return false;
    // The header search bar submits thread names into the panel's Search list.
    if (!require(application_thread_browser_search(utf8_literal("Design日")) && thread_browser_ui_adapter_submit(98005) &&
        thread_browser_ui_adapter_flight(&flight, &flight_id) && flight.mode == ThreadBrowserMode::search &&
        flight.offset == 0 && utf8_equal(flight.name, utf8_literal("Design日")) &&
        thread_browser_ui_adapter_page({json, sizeof(json)}, 100, kThreadBrowserProbeFirstSearch, false, false, &page) &&
        thread_browser_ui_adapter_response(200, page) && context.project() && scene->view->total == 100 &&
        scene->count == 25 && scene->view->tab == UiThreadBrowserTab::search, 10)) return false;
    const u32 search_submissions = thread_browser_ui_adapter_submissions();
    if (!require(thread_browser_ui_adapter_key(VK_TAB) && thread_browser_ui_adapter_action(UiThreadBrowserAction::next) &&
        thread_browser_ui_adapter_action(UiThreadBrowserAction::next) && thread_browser_ui_adapter_action(UiThreadBrowserAction::next) &&
        context.project() && scene->view->page_index == 3 && scene->view->rows[0].thread_id == kThreadBrowserProbeFirstSearch + 75 &&
        !scene->view->has_next && thread_browser_ui_adapter_submissions() == search_submissions, 11)) return false;
    context.theme = kLightTheme;
    if (!require(resize(960, 900, 144) && show(L"thread-browser-search-light-144.bmp") && row_pixels() && warm(2), 12)) return false;
    UiLayoutDiagnostic diagnostics[32]{};
    if (!require(ui_layout_engine_validate(scene->widgets.layout, {diagnostics, 32}).clean() &&
        renderer.content_text->quad_count > 0 && renderer.paint_stream.rejected == 0, 13)) return false;
    // Browsing a Forum projects its parent in chrome without changing Core's
    // underlying ordinary conversation selection or submitting a REST request.
    const u64 underlying_channel = state->selected_channel_id;
    const u32 forum_submissions = thread_browser_ui_adapter_submissions();
    context.theme = kDefaultTheme;
    if (!require(resize(1280, 800, 96) && application_thread_browser_available(kThreadBrowserProbeForum) &&
        application_thread_browser_open(kThreadBrowserProbeForum) && context.project() &&
        state->selected_channel_id == underlying_channel && underlying_channel == kThreadBrowserProbeParent &&
        application_thread_browser_scene()->view->parent_id == kThreadBrowserProbeForum &&
        application_thread_browser_scene()->count == 1 && thread_browser_ui_adapter_submissions() == forum_submissions, 16)) return false;
    float forum_top = 8.f;
    bool forum_found = false;
    for (u32 i = 0; i < context.navigation.channel_count; ++i) {
        if (context.navigation.channel_rows[i].id == kThreadBrowserProbeForum) { forum_found = true; break; }
        forum_top += shell_navigation_row_height(context.navigation.channel_rows[i]);
    }
    if (!require(forum_found && shell_navigation_scene_update(&context.sidebar, &context.navigation,
        context.sidebar.viewport, forum_top).ok() && context.project(), 161)) return false;
    struct ForumChrome {
        const UiWidgetManager* shell;
        const UiWidgetManager* sidebar;
        UiWidgetHandle title, forum;
        bool title_seen, forum_seen;
    } forum_chrome{&renderer.ui_scene->widgets, &context.sidebar.widgets};
    const auto* title = ui_scene_resolve(*renderer.ui_scene, ui_scene_find(*renderer.ui_scene, UiSemantic::title_label));
    if (!title) return require(false, 162);
    forum_chrome.title = title->widget;
    for (u32 i = 0; i < context.sidebar.count; ++i)
        if (context.sidebar.rows[i].view.id == kThreadBrowserProbeForum) forum_chrome.forum = context.sidebar.rows[i].label;
    UiTextEmitter chrome_emitter{};
    UiTextOutput chrome_output{nullptr, &chrome_emitter};
    if (!require(ui_text_emitter_initialize(&chrome_emitter, +[](void* opaque, const UiTextCommand& command, u32) noexcept {
        auto& chrome = *static_cast<ForumChrome*>(opaque);
        if (!utf8_equal(command.text, utf8_literal("product-forum"))) return true;
        chrome.title_seen |= command.owner.manager == chrome.shell && command.owner.handle.slot == chrome.title.slot &&
            command.owner.handle.generation == chrome.title.generation;
        chrome.forum_seen |= command.owner.manager == chrome.sidebar && command.owner.handle.slot == chrome.forum.slot &&
            command.owner.handle.generation == chrome.forum.generation;
        return true;
    }, &forum_chrome) && shell_text_presentation_build_output(*renderer.ui_scene, *state, context.navigation, context.theme,
        context.sidebar, nullptr, nullptr, nullptr, &chrome_output, renderer.user_store, renderer.guild_rail_scene,
        kThreadBrowserProbeForum) && forum_chrome.title_seen && forum_chrome.forum_seen &&
        show(L"thread-browser-forum.bmp") && row_pixels() && state->selected_channel_id == underlying_channel, 163)) return false;
    // Forum post cards: one active REST page (posts always load over REST for
    // their tags, counts and starter previews) through the shipping decoder,
    // projection, scene and renderer, in List and then Gallery layout. No tag
    // metadata route is injected here, so the toolbar shows sort and layout only.
    {
        const auto forum_api = g_app.account.api.forum_post;
        g_app.account.api.forum_post = nullptr;
        u8 forum_json[4096];
        ProtocolWriter writer{{forum_json, sizeof(forum_json)}, 0};
        const auto put = [&](const char* text) noexcept { return protocol_writer_append_c_string(&writer, text); };
        const auto number = [&](u64 value) noexcept { return protocol_writer_append_u64(&writer, value); };
        const char* titles[]{"Native build fails on ARM64", "Forum guidelines", "Idea: compact gallery cards"};
        bool written = put("{\"threads\":[");
        for (u32 i = 0; i < 3; ++i) {
            written = written && (!i || put(",")) && put("{\"id\":\"") && number(kThreadBrowserProbeForum + 20 + i) &&
                put("\",\"guild_id\":\"") && number(kThreadMembershipProbeGuild) && put("\",\"parent_id\":\"") &&
                number(kThreadBrowserProbeForum) && put("\",\"type\":11,\"name\":\"") && put(titles[i]) &&
                put("\",\"owner_id\":\"") && number(state->self_user_id) && put("\",\"message_count\":") &&
                number(i == 0 ? 12 : i == 1 ? 3 : 0) && put(i == 1 ? ",\"flags\":2" : ",\"flags\":0") &&
                put(",\"thread_metadata\":{\"archived\":false,\"locked\":false}}");
        }
        written = written && put("],\"first_messages\":[{\"id\":\"") && number(kThreadBrowserProbeForum + 20) &&
            put("\",\"channel_id\":\"") && number(kThreadBrowserProbeForum + 20) &&
            put("\",\"content\":\"The linker reports an unresolved symbol when targeting ARM64.\\nSteps to reproduce are attached.\","
                "\"author\":{\"id\":\"") && number(state->self_user_id) &&
            put("\",\"username\":\"rob\",\"global_name\":\"Rob\",\"avatar\":null},\"attachments\":[{\"id\":\"97900\","
                "\"url\":\"https://cdn.discordapp.com/attachments/1/97900/build.png\",\"content_type\":\"image/png\","
                "\"width\":640,\"height\":360}]},{\"id\":\"") && number(kThreadBrowserProbeForum + 21) &&
            put("\",\"channel_id\":\"") && number(kThreadBrowserProbeForum + 21) &&
            put("\",\"content\":\"Search before posting, keep one problem per post and tag it.\"}]}");
        ThreadBrowserRequest forum_flight{};
        u64 forum_flight_id{};
        const Utf8View forum_page{reinterpret_cast<const char*>(forum_json), writer.used};
        const bool loaded = written && application_thread_browser_pending(clock_monotonic_milliseconds()) &&
            thread_browser_ui_adapter_submit(98100) && thread_browser_ui_adapter_flight(&forum_flight, &forum_flight_id) &&
            forum_flight.mode == ThreadBrowserMode::active && forum_flight.parent_id == kThreadBrowserProbeForum &&
            thread_browser_ui_adapter_response(200, forum_page) && context.project();
        const auto* forum_scene = application_thread_browser_scene();
        UiLayoutDiagnostic forum_diagnostics[32]{};
        const bool cards = loaded && forum_scene && forum_scene->forum && forum_scene->count == 4 &&
            forum_scene->forum_sort.generation && forum_scene->backing->rows[0].generation &&
            forum_scene->view->rows[0].thread_id == kThreadBrowserProbeForum + 21 &&
            show(L"thread-browser-forum-cards.bmp") &&
            ui_layout_engine_validate(forum_scene->widgets.layout, {forum_diagnostics, 32}).clean() &&
            renderer.paint_stream.rejected == 0 && renderer.content_text->quad_count > 0;
        const bool gallery = cards && thread_browser_ui_adapter_action(UiThreadBrowserAction::forum_gallery) &&
            context.project() && application_thread_browser_scene()->backing->grid.columns >= 2 &&
            show(L"thread-browser-forum-gallery.bmp") && renderer.paint_stream.rejected == 0 &&
            thread_browser_ui_adapter_action(UiThreadBrowserAction::forum_list) && context.project();
        g_app.account.api.forum_post = forum_api;
        if (!require(loaded, 164) || !require(cards, 165) || !require(gallery, 166)) return false;
    }
    // Channel list beside the open browser: the panel covers only the content
    // column, wheel input over the list is not routed to the browser, and the
    // list's text follows its rows when it scrolls with the browser open.
    for (u32 i = 0; i < 40; ++i) {
        char name[16] = "lounge-";
        u32 bytes = 7;
        if (i >= 10) name[bytes++] = static_cast<char>('0' + i / 10);
        name[bytes++] = static_cast<char>('0' + i % 10);
        if (!require(chat_state_store_upsert_channel(state, {98000 + i, kThreadMembershipProbeGuild, 0,
                Utf8View{name, bytes}, 10 + i, 0, 0}).ok(), 170)) return false;
    }
    chat_state_store_recompute_guild_permissions(state, kThreadMembershipProbeGuild);
    if (!require(shell_navigation_scene_update(&context.sidebar, &context.navigation, context.sidebar.viewport, 0).ok() &&
        context.project(), 171)) return false;
    struct SidebarText {
        const UiWidgetManager* sidebar;
        UiRect panel, viewport, probe;
        u32 commands, outside, covered;
    };
    const auto sidebar_text = [&](SidebarText* out) noexcept {
        *out = {&context.sidebar.widgets, application_thread_browser_scene()->panel, context.sidebar.viewport};
        UiTextEmitter emitter{};
        UiTextOutput output{nullptr, &emitter};
        return ui_text_emitter_initialize(&emitter, +[](void* opaque, const UiTextCommand& command, u32) noexcept {
            auto& text = *static_cast<SidebarText*>(opaque);
            if (command.owner.manager != text.sidebar) return true;
            const auto visible = ui_rect_intersection(command.rectangle, command.clip);
            if (!ui_rect_has_area(visible)) return true;
            ++text.commands;
            text.outside += visible.top < text.viewport.top - .5f || visible.bottom > text.viewport.bottom + .5f;
            text.covered += ui_rect_has_area(ui_rect_intersection(visible, text.panel));
            // A thread row listed under the parent, mid-list both before and after the scroll.
            if (utf8_equal(command.text, utf8_literal("Design discussion 10 · native review")))
                text.probe = command.rectangle;
            return true;
        }, out) && shell_text_presentation_build_output(*renderer.ui_scene, *state, context.navigation, context.theme,
            context.sidebar, nullptr, nullptr, nullptr, &output, renderer.user_store, renderer.guild_rail_scene,
            application_thread_browser_scene()->view->parent_id);
    };
    const auto* browser_scene = application_thread_browser_scene();
    const auto list = context.sidebar.viewport;
    SidebarText before{}, after{};
    if (!require(browser_scene && ui_rect_has_area(list) &&
        !ui_rect_has_area(ui_rect_intersection(browser_scene->panel, list)) &&
        browser_scene->panel.left >= list.right - .5f &&
        !ui_rect_contains(application_thread_browser_viewport(), (list.left + list.right) * .5f,
            (list.top + list.bottom) * .5f) &&
        context.sidebar.content_height > ui_rect_height(list) + 120.f, 172)) return false;
    const bool sidebar_projected = sidebar_text(&before);
    log_u64(LogLevel::info, "thread_browser", "sidebar_text_commands", before.commands);
    log_u64(LogLevel::info, "thread_browser", "sidebar_text_outside", before.outside);
    log_u64(LogLevel::info, "thread_browser", "sidebar_text_covered", before.covered);
    if (!require(sidebar_projected && before.commands > 0 && !before.outside && !before.covered &&
        ui_rect_has_area(before.probe), 173)) return false;
    if (!require(shell_navigation_scene_scroll(&context.sidebar, &context.navigation, 96.f).ok() &&
        context.sidebar.scroll_dips == 96.f && context.project() && show(L"thread-browser-sidebar-scrolled.bmp") &&
        application_thread_browser_active() && sidebar_text(&after) && after.commands > 0 && !after.outside &&
        !after.covered && ui_rect_has_area(after.probe) &&
        before.probe.top - after.probe.top > 95.5f && before.probe.top - after.probe.top < 96.5f, 174)) return false;
    if (!require(shell_navigation_scene_update(&context.sidebar, &context.navigation, context.sidebar.viewport, 0).ok() &&
        context.project(), 175)) return false;
    application_thread_browser_close();
    if (!require(context.project() && thread_browser_ui_adapter_header_pointer(true) &&
        thread_browser_ui_adapter_header_pointer(false) && context.project() &&
        application_thread_browser_scene()->view->parent_id == kThreadBrowserProbeParent &&
        state->selected_channel_id == underlying_channel, 17)) return false;
    // Supersede one exact immutable flight, then retire another by changing
    // the real account scope. Neither receipt can resurrect its old rows.
    const u32 core_count = state->channel_count;
    if (!require(thread_browser_ui_adapter_action(UiThreadBrowserAction::archived) &&
        thread_browser_ui_adapter_submit(98006) && thread_browser_ui_adapter_action(UiThreadBrowserAction::active) &&
        thread_browser_ui_adapter_cancellations() > 0 &&
        thread_browser_ui_adapter_page({json, sizeof(json)}, 1, 99990, true, false, &page) &&
        thread_browser_ui_adapter_response(200, page) && state->channel_count == core_count &&
        !chat_state_store_find_channel(*state, 99990) &&
        thread_browser_ui_adapter_action(UiThreadBrowserAction::archived) && thread_browser_ui_adapter_submit(98007) &&
        thread_browser_ui_adapter_retire_scope() && !application_thread_browser_active() &&
        thread_browser_ui_adapter_response(200, page) && state->channel_count == core_count &&
        !chat_state_store_find_channel(*state, 99990) && !application_thread_browser_scene(), 14)) return false;
    if (!require(thread_browser_ui_adapter_retire_scope(true) && !application_thread_browser_available(), 15)) return false;
    log_write(LogLevel::info, "thread_browser", "native_shipping_host_core_decoder_header_search_pointer_key_exact_receipt_retention_passed");
    log_write(LogLevel::info, "thread_browser", "fixture_boundaries_injected_typed_rest_and_surrounding_surface_navigation_stubs_no_live_network");
    return true;
}
} // namespace maicord
