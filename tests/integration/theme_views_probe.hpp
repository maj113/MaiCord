#pragma once
#include "settings_theme_probe.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/shell_text_presentation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/shell_presentation.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/ui/friends_scene.hpp"

namespace maicord {
namespace theme_views_probe_detail {
struct Friends {
    bool pending{};
    static bool resolve(void* opaque, u32 index, UiFriendsRow* out) noexcept {
        if (!opaque || !out || index >= 3)
            return false;
        const auto& source = *static_cast<Friends*>(opaque);
        const Utf8View names[]{utf8_literal("Original Ada"), utf8_literal("Original Lin"),
                               utf8_literal("Original Sam")};
        *out = {101 + index,
                names[index],
                utf8_literal("offline.fixture"),
                utf8_literal("Online"),
                UiPresence::online,
                source.pending ? (index < 2 ? UiFriendsKind::incoming : UiFriendsKind::outgoing)
                               : UiFriendsKind::friend_user,
                false};
        return true;
    }
};
inline bool fail(u32 stage) noexcept {
    log_u64(LogLevel::error, "probe", "theme_views_failure_stage", stage);
    return false;
}
inline const UiNode* node(const UiScene& scene, UiSemantic semantic) noexcept {
    return ui_scene_resolve(scene, ui_scene_find(scene, semantic));
}
} // namespace theme_views_probe_detail

// All navigation and Friends records are explicit synthetic values. The DM
// and group shell/composer fixtures own empty timelines, never borrow messages
// from a different channel. No provider, account or remote media request.
inline bool theme_views_ui_probe(BackgroundThemeProbeContext c) noexcept {
    using namespace theme_views_probe_detail;
    using settings_theme_probe_detail::Pixels;
    auto& r = c.renderer;
    if (!r.ui_scene || !r.message_scene || !r.message_scene->timeline || !r.navigation_view || !r.dpi ||
        !r.message_text || r.message_text->command_count > kMaximumMessageTextCommands)
        return fail(1);
    MessageStore empty_store{};
    MessageRecord empty_records[1]{};
    u32 empty_order[1]{}, item_units[1]{};
    u8 empty_text[512]{};
    u64 fenwick[1]{};
    UiVariableList empty_list{};
    MessageTimeline timeline{};
    UiScene shell{};
    UiFriendsScene friends{};
    MessageScene messages{};
    MessageSceneRowWidget message_rows[4]{};
    UiWidget message_widgets[256]{};
    UiLayoutNode message_nodes[256]{};
    MessageTextPresentation empty_presentation{};
    MessageTextCommand empty_commands[1]{};
    char empty_utf8[128]{};
    TextStyleSpan empty_styles[1]{};
    MessageMarkdownSemantic empty_semantics[1]{};
    MessageTextMeasurement empty_measurements[1]{};
    MessageTextSpoilerReveal empty_reveals[1]{};
    u32 empty_style_scratch[128]{};
    MessageComposer model{};
    char edit_bytes[128]{};
    MessageComposerScene composer{};
    ShellNavigationScene navigation{};
    ShellNavigationRowWidget navigation_rows[2]{};
    UiWidget navigation_widgets[10]{};
    UiLayoutNode navigation_nodes[10]{};
    ChatStateStore state{};
    GuildRecord guilds[1]{};
    ChannelRecord channels[2]{};
    u16 guild_order[1]{}, channel_order[2]{};
    u8 names[512]{};
    NavigationView view{};
    GuildViewRow guild_rows[1]{};
    ChannelViewRow channel_rows[2]{};
    u16 scratch[2]{};
    u8 visibility[2]{};
    ChannelSortKey sort_keys[2]{};
    struct Restore {
        D3D11Renderer& r;
        const UiScene* shell;
        const UiFriendsScene *friends, *requests;
        const UiSettingsScene* settings;
        const UiThemeEditorScene* editor;
        const ShellNavigationScene* navigation;
        const MessageScene* messages;
        const MessageComposerScene* composer;
        const MemberListScene* members;
        const ChatStateStore* state;
        const UserStore* users;
        const NavigationView* view;
        UiScene& owned_shell;
        UiFriendsScene& owned_friends;
        MessageScene& owned_messages;
        MessageComposerScene& owned_composer;
        const MessageTextPresentation* text;
        ~Restore() noexcept {
            renderer_bind_friends_scene(&r, friends);
            renderer_bind_friends_requests_scene(&r, requests);
            renderer_bind_settings_scene(&r, settings);
            renderer_bind_theme_editor_scene(&r, editor);
            renderer_bind_ui_scene(&r, shell);
            renderer_bind_shell_navigation_scene(&r, navigation);
            renderer_bind_message_scene(&r, messages);
            renderer_bind_message_composer_scene(&r, composer);
            // This binder admits only initialized non-null scenes. Restore
            // the borrowed pointer exactly, including an absent member rail.
            r.member_list_scene = members;
            renderer_bind_navigation_view(&r, state, users, view);
            renderer_unbind_message_text_presentation(&r);
            renderer_bind_message_text_presentation(&r, text);
            // Retire borrowed command owners before releasing local storage.
            renderer_rebuild_content_text(&r, kDefaultTheme);
            ui_friends_scene_release(&owned_friends);
            message_composer_scene_release(&owned_composer);
            message_scene_release(&owned_messages);
            ui_scene_release(&owned_shell);
        }
    } restore{r,
              r.ui_scene,
              r.friends_scene,
              r.friends_requests_scene,
              r.settings_scene,
              r.theme_editor_scene,
              r.shell_navigation_scene,
              r.message_scene,
              r.message_composer_scene,
              r.member_list_scene,
              r.chat_state,
              r.user_store,
              r.navigation_view,
              shell,
              friends,
              messages,
              composer,
              r.message_text};
    if (!ui_scene_initialize_bootstrap(&shell) ||
        !message_store_initialize(&empty_store, {empty_records, 1}, {empty_order, 1}, {empty_text, 512}) ||
        !ui_variable_list_initialize(&empty_list, {item_units, 1}, {fenwick, 1}, 0, ui_extent_units(48)) ||
        !message_timeline_initialize(&timeline, &empty_store, &empty_list, ui_extent_units(48),
                                     ui_extent_units(400), false) ||
        !message_scene_initialize(&messages, &timeline, {message_rows, 4}, {message_widgets, 256},
                                  {message_nodes, 256}) ||
        !message_text_presentation_initialize(&empty_presentation, {empty_commands, 1}, {empty_utf8, 128},
                                              {empty_styles, 1}, {empty_semantics, 1},
                                              {empty_measurements, 1}, {empty_reveals, 1},
                                              {empty_style_scratch, 128}) ||
        !message_composer_initialize(&model, {edit_bytes, 128}) ||
        message_composer_insert_utf8(&model, utf8_literal("Original draft")) !=
            MessageComposerResult::changed ||
        !message_composer_scene_initialize_owned(&composer) ||
        !shell_navigation_scene_initialize(&navigation, {navigation_rows, 2}, {navigation_widgets, 10},
                                           {navigation_nodes, 10}) ||
        !chat_state_store_initialize(&state, {guilds, 1}, {guild_order, 1}, {channels, 2},
                                        {channel_order, 2}, {names, 512}) ||
        !navigation_view_initialize(&view, {guild_rows, 1}, {channel_rows, 2}, {scratch, 2},
                                            {visibility, 2}, {sort_keys, 2}) ||
        !chat_state_store_upsert_channel(&state, {201, 0, 0, utf8_literal("Original DM"), 0, 0, 1})
             .ok() ||
        !chat_state_store_upsert_channel(&state, {202, 0, 0, utf8_literal("Original Group"), 1, 0, 3})
             .ok() ||
        !chat_state_store_select_home(&state).ok())
        return fail(2);
    const UiRect viewport{0, 0, r.width * 96.f / r.dpi, r.height * 96.f / r.dpi};
    renderer_bind_settings_scene(&r, nullptr);
    renderer_bind_theme_editor_scene(&r, nullptr);
    renderer_bind_friends_requests_scene(&r, nullptr);
    // The public binder rejects nullptr without retiring the prior guild
    // member scene. These scoped private shells intentionally have no rail.
    r.member_list_scene = nullptr;
    // Bootstrap storage has no published shell nodes yet; bind it only
    // after each phase has completed its actual scoped layout.
    if (!renderer_bind_shell_navigation_scene(&r, &navigation) ||
        !renderer_bind_message_scene(&r, &messages) ||
        !renderer_bind_message_composer_scene(&r, &composer) || r.shell_navigation_scene != &navigation ||
        r.message_scene != &messages || r.message_composer_scene != &composer)
        return fail(3);
    renderer_bind_navigation_view(&r, &state, nullptr, &view);
    // Original body commands belong to the parent fixture's widgets. This
    // probe proves current shell/composer owners, not re-labelled body text.
    renderer_unbind_message_text_presentation(&r);
    Friends source_context{};
    UiFriendsSource source{&source_context, 3, 1, Friends::resolve};
    // An empty focused search has both byte endpoints at zero. Its content
    // revision is separate from the editor's selection offsets.
    UiFriendsContent content{{{}, 0, 0, true, true, false}, 3, 1};
    const UiFriendsSection sections_data[]{{UiFriendsSectionKind::incoming, 0, 2, 2, false},
                                           {UiFriendsSectionKind::outgoing, 2, 1, 1, false}};
    const UiFriendsSections sections{sections_data, 2, 1, false, false, 0};
    const wchar_t* captures[]{L"background-theme-friends-all.bmp", L"background-theme-friends-pending.bmp",
                              L"background-theme-dm.bmp", L"background-theme-group.bmp"};
    float home_navigation_left = 0, home_navigation_right = 0;
    for (u32 phase = 0; phase < 4; ++phase) {
        auto theme_preferences = client_preferences_defaults();
        theme_preferences.theme =
            (phase & 1) ? ClientTheme::custom_light_gradient : ClientTheme::custom_gradient;
        theme_preferences.gradient.intensity = 65;
        const Theme theme = client_theme_projection(theme_preferences);
        const bool is_friends = phase < 2;
        UiShellLayoutMetrics metrics =
            is_friends ? kFriendsShellLayoutMetrics : kGuildChannelShellLayoutMetrics;
        metrics.flags = is_friends
                            ? static_cast<u16>(ui_shell_layout_navigation)
                            : static_cast<u16>(ui_shell_layout_navigation | ui_shell_layout_composer);
        metrics.member_sidebar_width = 0;
        ++metrics.revision;
        if ((!is_friends && !chat_state_store_select_channel(&state, phase == 2 ? 201 : 202).ok()) ||
            !navigation_view_rebuild(&view, state).ok() ||
            !ui_scene_layout_with_metrics(&shell,
                                          is_friends ? UiShellView::friends : UiShellView::private_channels,
                                          metrics, ui_rect_width(viewport), ui_rect_height(viewport)))
            return fail(10 + phase);
        if (!renderer_bind_ui_scene(&r, &shell) || r.ui_scene != &shell)
            return fail(130 + phase);
        const auto* guild_column = node(shell, UiSemantic::guild_rail);
        const auto* navigation_column = node(shell, UiSemantic::channel_sidebar);
        const auto* content_column = node(shell, UiSemantic::content);
        if (!guild_column || !navigation_column || !content_column ||
            guild_column->rectangle.right != navigation_column->rectangle.left ||
            navigation_column->rectangle.right != content_column->rectangle.left)
            return fail(160 + phase);
        if (!phase) {
            home_navigation_left = navigation_column->rectangle.left;
            home_navigation_right = navigation_column->rectangle.right;
        } else if (navigation_column->rectangle.left != home_navigation_left ||
                   navigation_column->rectangle.right != home_navigation_right) {
            return fail(170 + phase);
        }
        const auto* body = node(shell, UiSemantic::message_view);
        const auto* rail = node(shell, UiSemantic::channel_navigation);
        const auto* compose = node(shell, UiSemantic::composer);
        if (!body || !rail || !shell_navigation_scene_update(&navigation, &view, rail->rectangle, 0).ok())
            return fail(20 + phase);
        if (is_friends) {
            source_context.pending = phase == 1;
            ++source.revision;
            if (!ui_friends_scene_update(
                    &friends, &source, phase ? UiFriendsTab::pending : UiFriendsTab::all,
                    UiFriendsPhase::ready, {}, body->rectangle, &content, phase ? &sections : nullptr))
                return fail(30 + phase);
            renderer_bind_friends_scene(&r, &friends);
        } else {
            renderer_bind_friends_scene(&r, nullptr);
            if (!compose || !message_scene_update(&messages, body->rectangle).ok() ||
                !message_composer_scene_update(&composer, &model, compose->rectangle, true).ok())
                return fail(30 + phase);
            if (!message_text_presentation_build(&empty_presentation, messages, state, nullptr, view, theme,
                                                 0)
                     .ok() ||
                empty_presentation.command_count || empty_presentation.measurement_count)
                return fail(140 + phase);
            renderer_unbind_message_text_presentation(&r);
            if (!renderer_bind_message_text_presentation(&r, &empty_presentation) ||
                r.message_text != &empty_presentation)
                return fail(150 + phase);
            PaintStream preflight{};
            ShellPresentationRegions regions{};
            MessageMediaCommand media_commands[64]{};
            MessageMediaCommandStream media{};
            MessagePresentationResult result{};
            const bool shell_ready =
                paint_stream_initialize(&preflight) &&
                shell_presentation_build_chrome(shell, &view, r.guild_rail_scene, &navigation, &composer,
                                                nullptr, theme, ui_rect_width(viewport),
                                                ui_rect_height(viewport), &preflight, &regions, nullptr);
            const bool message_ready =
                shell_ready && ui_rect_equal(messages.viewport, regions.message_view) &&
                message_media_command_stream_initialize(&media, {media_commands, 64}) &&
                message_presentation_build(messages, theme, &preflight, &media, &result);
            paint_stream_release(&preflight);
            if (!shell_ready)
                return fail(110 + phase);
            if (!message_ready)
                return fail(120 + phase);
        }
        const auto draw = [&]() noexcept {
            const bool text_ready = is_friends ? renderer_rebuild_shell_text(&r, theme)
                                               : renderer_rebuild_content_text(&r, theme);
            return text_ready && probe_present(&r, theme) &&
                   d3d11_renderer_draw_stream(&r, theme, true, true, nullptr);
        };
        if (!draw())
            return fail(40 + phase);
        UiTextCommand commands[64]{};
        char owned_chars[1024]{};
        UiTextCommandStream text{};
        if (!ui_text_command_stream_initialize(&text, {commands, 64}, {owned_chars, 1024}))
            return fail(50 + phase);
        if (is_friends) {
            UiFriendsVisibleRow row{};
            if (!ui_friends_scene_visible_row(friends, 0, &row))
                return fail(60 + phase);
            const float x = row.rectangle.right - 4, y = (row.rectangle.top + row.rectangle.bottom) * .5f;
            u32 before = 0, after = 0;
            {
                Pixels pixels(r);
                if (!pixels.point(x, y, &before))
                    return fail(60 + phase);
            }
            ui_button_pointer(&friends.widgets, UiButtonPointerEvent::move,
                              {(row.rectangle.left + row.rectangle.right) * .5f, y});
            if (!friends.widgets.hovered.generation || !draw())
                return fail(70 + phase);
            {
                Pixels pixels(r);
                if (!pixels.point(x, y, &after) || before == after)
                    return fail(70 + phase);
            }
            if (!ui_friends_scene_text(friends, theme, &text))
                return fail(80 + phase);
            u32 names_ink = 0;
            Pixels pixels(r);
            for (u32 i = 0; i < text.count; ++i)
                if (utf8_equal(commands[i].text, utf8_literal("Original Ada")) ||
                    utf8_equal(commands[i].text, utf8_literal("Original Lin")) ||
                    utf8_equal(commands[i].text, utf8_literal("Original Sam"))) {
                    if (!pixels.ink(ui_rect_intersection(commands[i].rectangle, commands[i].clip),
                                    theme.text_primary))
                        return fail(80 + phase);
                    ++names_ink;
                }
            if (names_ink != 3 || friends.count != 3)
                return fail(80 + phase);
            if (!shell_text_presentation_build(shell, state, view, theme, navigation, &text))
                return fail(180 + phase);
            const auto* friends_button = node(shell, UiSemantic::friends_button);
            bool padded_friends_ink = false;
            for (u32 i = 0; friends_button && i < text.count; ++i) {
                const auto& command = commands[i];
                if (command.owner.handle.slot == friends_button->widget.slot &&
                    command.owner.handle.generation == friends_button->widget.generation &&
                    utf8_equal(command.text, utf8_literal("Friends")))
                    padded_friends_ink =
                        command.rectangle.left == friends_button->rectangle.left + 16.f &&
                        command.rectangle.right == friends_button->rectangle.right - 16.f &&
                        pixels.ink(ui_rect_intersection(command.rectangle, command.clip),
                                   theme.text_primary);
            }
            if (!padded_friends_ink)
                return fail(190 + phase);
        } else {
            if (!shell_text_presentation_build(shell, state, view, theme, navigation, &text))
                return fail(90 + phase);
            bool title_match = false, title = false, draft = false, pixels_ready = false;
            const auto expected = phase == 2 ? utf8_literal("Original DM") : utf8_literal("Original Group");
            const auto* edit = message_composer_scene_layout_node(composer, MessageComposerControl::edit);
            {
                Pixels pixels(r);
                pixels_ready = pixels.ready;
                for (u32 i = 0; i < text.count; ++i)
                    if (utf8_equal(commands[i].text, expected)) {
                        title_match = true;
                        title = title ||
                                pixels.ink(ui_rect_intersection(commands[i].rectangle, commands[i].clip),
                                           theme.text_primary);
                    }
                draft = edit && pixels.ink(edit->content_rectangle, theme.text_primary);
            }
            if (!title || !edit || !draft) {
                log_u64(LogLevel::error, "probe", "theme_views_private_title_command_match", title_match);
                log_u64(LogLevel::error, "probe", "theme_views_private_title_ink", title);
                log_u64(LogLevel::error, "probe", "theme_views_private_edit_exists", edit != nullptr);
                log_u64(LogLevel::error, "probe", "theme_views_private_draft_ink", draft);
                log_u64(LogLevel::error, "probe", "theme_views_private_readback_ready", pixels_ready);
                if (c.capture)
                    c.capture(L"background-theme-private-failure.bmp", theme);
                return fail(90 + phase);
            }
        }
        if (r.paint_stream.rejected || r.paint_stream.count > kMaxPaintQuads ||
            (c.capture && !c.capture(captures[phase], theme)))
            return fail(100 + phase);
    }
    log_write(LogLevel::info, "probe",
              "theme_views_home_private_same_columns_zero_gutters_friends_padded_native_ink_passed");
    log_write(LogLevel::info, "probe", "theme_views_friends_all_pending_dm_group_hover_ink_passed");
    return true;
}
} // namespace maicord
