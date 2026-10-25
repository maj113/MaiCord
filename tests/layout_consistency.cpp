// Internal layout consistency across representative scenes: nested rounded
// rectangles stay concentric, highlights stay inside their clips and each
// component type keeps one padding. Synthetic fixtures only.
#include "layout_consistency.hpp"
#include "../src/app/application_plugin_browser_projection.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/user_profile.hpp"
#include "maicord/ui/background_theme.hpp"
#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/ui/member_list_scene.hpp"
#include "maicord/ui/page_registry.hpp"
#include "maicord/ui/profile_scene.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/shell_presentation.hpp"
#include "maicord/ui/call_scene.hpp"
#include "maicord/ui/image_viewer.hpp"
#include "maicord/ui/message_context_menu.hpp"
#include "maicord/ui/message_forward_scene.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/text_form.hpp"
#include "maicord/ui/tooltip.hpp"
#include "maicord/ui/video_scene.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/ui/message_scene.hpp"

namespace maicord {
namespace layout_consistency_test {
u32 failures;
void check(bool passed, const char* name) noexcept {
    test_check(&failures, passed, name);
}

struct World {
    PaintStream paint;
    LayoutConsistencyReport report;
    LayoutConsistencyPadding padding;
    u32 scenes;
};
World g;

bool begin(float width, float height) noexcept {
    return paint_stream_reset(&g.paint, width, height);
}
void finish(const char* scene, const UiLayoutEngine* layout = nullptr) noexcept {
    layout_consistency_check_paint(g.paint, 0, g.paint.count, scene, &g.report);
    if (layout)
        layout_consistency_record_layout(*layout, scene, &g.padding);
    check(!g.paint.rejected && g.paint.clip_depth == 1, scene);
    ++g.scenes;
}

bool measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    const auto valid = utf8_validate(request.text);
    if (!valid.valid || request.constraint.width <= 0)
        return false;
    const float natural = static_cast<float>(valid.codepoints) * 8;
    const u32 columns = maximum(1u, static_cast<u32>(request.constraint.width / 8));
    const u32 lines = request.no_wrap ? 1u : maximum(1u, (valid.codepoints + columns - 1) / columns);
    *value = {{minimum(natural, request.constraint.width), static_cast<float>(lines) * 20}, 16, lines};
    return true;
}

void media_viewer() noexcept {
    static UiImageViewer viewer{};
    bool ok = ui_image_viewer_open(&viewer, {0, 0, 1280, 800}, {1600, 900}) &&
              ui_image_viewer_set_save_available(&viewer, true) &&
              ui_image_viewer_set_actions(&viewer, ui_image_viewer_action_open | ui_image_viewer_action_copy) &&
              ui_image_viewer_set_item(&viewer, {1600, 900}, 1, 3, false);
    viewer.keyboard_focus = true;
    ui_widget_manager_focus(&viewer.widgets, viewer.buttons[ui_image_viewer_next]);
    ok = ok && begin(1280, 800) && ui_image_viewer_paint(viewer, kDefaultTheme, &g.paint) &&
         ui_image_viewer_paint_overlay(viewer, kDefaultTheme, &g.paint);
    check(ok, "media viewer fixture");
    finish("media viewer", &viewer.widgets.layout);
}

void video_player() noexcept {
    UiVideoPlaybackState playback{7, 8, 9, 3000, 120000, 40, true, false, false, false, true, true, true, true};
    playback.can_expand = true;
    static UiVideoScene scene{};
    const UiRect preview{100, 100, 580, 370};
    bool ok = ui_video_scene_project(&scene, playback, preview, preview, {}, {}, kDefaultTheme) &&
              ui_video_scene_hover(&scene, {300, 200}) && ui_video_scene_focus(&scene, UiVideoControl::mute, true);
    ok = ok && begin(800, 600) && ui_video_scene_paint(scene, kDefaultTheme, &g.paint);
    check(ok, "video player fixture");
    finish("video player");
}

struct Menu {
    PopupItem items[kMessageContextMaximumItems];
    PopupRowWidget rows[kMessageContextMaximumItems];
    UiWidget widgets[2 * kMessageContextMaximumItems + 2];
    UiLayoutNode nodes[2 * kMessageContextMaximumItems + 2];
    PopupScene scene;
};
void context_menus() noexcept {
    static Menu menu{};
    MessageRecord message{};
    message.id = 10;
    message.channel_id = 7;
    message.author_id = 9;
    static MessageContextMenuModel context{};
    bool ok = message_context_menu_build(&context, message, true, true, true, nullptr, true).ok() &&
              popup_scene_initialize(&menu.scene, {menu.items, kMessageContextMaximumItems},
                                     {menu.rows, kMessageContextMaximumItems},
                                     {menu.widgets, 2 * kMessageContextMaximumItems + 2},
                                     {menu.nodes, 2 * kMessageContextMaximumItems + 2}) &&
              popup_scene_open(&menu.scene, PopupKind::context_menu, 7, {context.items, context.count},
                               {40, 40, 41, 41}, {0, 0, 640, 1000}, 300)
                  .ok();
    bool changed = false;
    ok = ok && popup_scene_select_index(&menu.scene, kPopupQuickReactionCount, &changed) && begin(640, 1000) &&
         popup_presentation_build(menu.scene, kDefaultTheme, &g.paint);
    check(ok, "context menu fixture");
    finish("context menu", &menu.scene.widgets.layout);
    popup_scene_release(&menu.scene);

    // A role picker: avatars, presence, a checked row and the selected row.
    static Menu roles{};
    PopupItem items[5]{};
    items[0] = {utf8_literal("Moderator"), 1, popup_item_enabled | popup_item_checked};
    items[1] = {utf8_literal("Avery"), 2, popup_item_enabled | popup_item_avatar, 0, 501};
    items[1].status = 1;
    items[2] = {utf8_literal("Blake"), 3, popup_item_enabled | popup_item_avatar, 0, 502};
    items[2].status = 4;
    items[3] = {utf8_literal("Copy ID"), 4, popup_item_enabled | popup_item_separator_before, popup_shortcut_copy};
    items[4] = {utf8_literal("Remove"), 5, popup_item_enabled | popup_item_danger};
    ok = popup_scene_initialize(&roles.scene, {roles.items, 5}, {roles.rows, 5}, {roles.widgets, 12},
                                {roles.nodes, 12}) &&
         popup_scene_open(&roles.scene, PopupKind::context_menu, 8, {items, 5}, {200, 200, 201, 201},
                          {0, 0, 800, 600}, 220)
             .ok() &&
         popup_scene_select_index(&roles.scene, 2, &changed) && begin(800, 600) &&
         popup_presentation_build(roles.scene, kDefaultTheme, &g.paint);
    check(ok, "member menu fixture");
    finish("member menu", &roles.scene.widgets.layout);

    // A theme with rounder menus: rows follow the panel's corners.
    static UiResolvedThemeStyles styles{};
    styles.ready = true;
    styles.geometry_revision = 1;
    styles.metrics.menu_radius = 16;
    styles.metrics.menu_padding = 6;
    Theme themed = kDefaultTheme;
    themed.styles = &styles;
    ok = popup_scene_bind_theme_styles(&roles.scene, &styles) && roles.scene.themed_menu &&
         popup_scene_select_index(&roles.scene, 0, &changed) && begin(800, 600) &&
         popup_presentation_build(roles.scene, themed, &g.paint);
    check(ok, "themed menu fixture");
    finish("themed menu", &roles.scene.widgets.layout);
    popup_scene_release(&roles.scene);
}

void tooltip() noexcept {
    static UiWidget widgets[2]{};
    static UiLayoutNode nodes[2]{};
    static UiWidgetManager manager{};
    auto root_style = ui_layout_style();
    root_style.width = ui_layout_fill();
    root_style.height = ui_layout_fill();
    auto button_style = ui_layout_style();
    button_style.width = button_style.height = ui_layout_fixed(32);
    button_style.margin = {200, 200, 0, 0};
    bool ok = ui_widget_manager_initialize(&manager, {widgets, 2}, {nodes, 2});
    const auto root = ok ? ui_widget_manager_add(&manager, ui_widget_invalid_handle(),
                                                 {1, UiWidgetKind::container, ui_widget_none,
                                                  {utf8_literal("Overlay"), root_style}})
                         : UiWidgetResult{};
    const auto button = root.ok() ? ui_widget_manager_add(&manager, root.handle,
                                                          {2, UiWidgetKind::button,
                                                           ui_widget_enabled | ui_widget_hit_test,
                                                           {utf8_literal("Button"), button_style}})
                                  : UiWidgetResult{};
    static UiTooltipScene scene{};
    static UiTextMeasureRecord records[4]{};
    static UiTextMeasureCache cache{};
    const UiRect viewport{0, 0, 640, 480};
    ok = button.ok() && ui_widget_manager_layout_rectangle(&manager, viewport).ok() &&
         ui_tooltip_bind_text_owner(&scene, {&manager, root.handle}) &&
         ui_text_measure_cache_initialize(&cache, {records, 4}, measure, nullptr) &&
         ui_tooltip_offer(&scene, {{{&manager, button.handle}, 7, 1, 2, 3}, {200, 200, 232, 232},
                                   utf8_literal("Notification settings")},
                          viewport, UiTooltipTrigger::pointer, 100) &&
         ui_tooltip_tick(&scene, 10'000, &cache) && ui_tooltip_visible(scene) && begin(640, 480) &&
         ui_tooltip_paint(scene, kDefaultTheme, &g.paint);
    check(ok, "tooltip fixture");
    finish("tooltip");
    ui_tooltip_clear(&scene);
}

void dialogs() noexcept {
    static TextFormScene form{};
    static UiTextMeasureRecord records[32]{};
    static UiTextMeasureCache cache{};
    TextFormContent content{};
    content.title = utf8_literal("Leaving MaiCord");
    content.context = utf8_literal("This link goes to example.com. Are you sure you want to go there?");
    content.cancel = utf8_literal("Go back");
    content.accept = utf8_literal("Visit site");
    content.option = utf8_literal("Trust example.com links from now on");
    content.can_accept = content.can_cancel = content.confirmation_only = content.focus_visible = true;
    bool ok = ui_text_measure_cache_initialize(&cache, {records, 32}, measure, nullptr) &&
              text_form_update(&form, content, {0, 0, 1280, 800}, &cache) && form.active &&
              text_form_focus(&form, TextFormElement::accept) && begin(1280, 800) &&
              text_form_paint(form, {}, kDefaultTheme, &g.paint);
    check(ok, "confirmation dialog fixture");
    finish("confirmation dialog", &form.widgets.layout);

    TextFormContent editing{};
    editing.title = utf8_literal("Edit channel topic");
    editing.label = utf8_literal("Topic");
    editing.text = utf8_literal("Release planning");
    editing.cancel = utf8_literal("Cancel");
    editing.accept = utf8_literal("Save");
    editing.can_accept = editing.can_cancel = editing.focus_visible = true;
    ok = text_form_update(&form, editing, {0, 0, 1280, 800}, &cache) && form.active && begin(1280, 800) &&
         text_form_paint(form, {}, kDefaultTheme, &g.paint);
    check(ok, "edit dialog fixture");
    finish("edit dialog", &form.widgets.layout);
    text_form_release(&form);
}

struct ForwardSource {
    u32 calls;
};
bool forward_current(void*) noexcept {
    return true;
}
bool forward_row(void*, u32 index, UiMessageForwardRow* row) noexcept {
    const u64 id = 1000 + index;
    *row = {id, 71, id, 0, index % 3 == 0 ? UiQuickSwitcherKind::private_channel : UiQuickSwitcherKind::text_channel,
            utf8_literal("release-planning"), utf8_literal("Fixture server"), {}, {}, index != 1};
    return true;
}
void forward_dialog() noexcept {
    static UiMessageForwardScene scene{};
    static ForwardSource fixture{};
    UiMessageForwardSource source{&fixture, 12, 1, forward_row, forward_current};
    UiMessageForwardContent content{};
    content.revision = 1;
    content.author = utf8_literal("Avery");
    content.preview = utf8_literal("The prototype is ready to share.");
    content.media_summary = utf8_literal("1 image");
    content.edit = {utf8_literal("release"), 7, 7, true, true, true};
    content.caption = {utf8_literal("For our next review"), 19, 19, true, false, false};
    bool ok = ui_message_forward_scene_update(&scene, &source, &content, {0, 0, 1280, 800}) && begin(1280, 800) &&
              ui_message_forward_scene_paint(scene, kDefaultTheme, &g.paint);
    check(ok, "forward dialog fixture");
    finish("forward dialog", &scene.widgets.layout);
    ui_message_forward_scene_release(&scene);
}

struct Timeline {
    MessageRecord record[2];
    u32 order[2];
    u8 text[256];
    MessageRichRecord rich_record[2];
    u16 rich_order[2];
    u8 rich_text[4096];
    u32 heights[2];
    u64 fenwick[2];
    UiWidget widgets[2 * kMessageSceneWidgetsPerRow + 1];
    UiLayoutNode nodes[2 * kMessageSceneWidgetsPerRow + 1];
    MessageStore store;
    MessageRichStore rich;
    UiVariableList list;
    MessageTimeline timeline;
    MessageScene scene;
};
void message_list() noexcept {
    static Timeline f{};
    constexpr u32 widgets = 2 * kMessageSceneWidgetsPerRow + 1;
    bool ok = message_store_initialize(&f.store, {f.record, 2}, {f.order, 2}, {f.text, sizeof(f.text)}) &&
              message_store_upsert(&f.store, {1, 7, 9, 0, 0, utf8_literal("Here are the files"), 0}).ok() &&
              message_store_upsert(&f.store, {2, 7, 9, 0, 0, utf8_literal("Link preview"), 0}).ok() &&
              message_rich_store_initialize(&f.rich, {f.rich_record, 2}, {f.rich_order, 2},
                                            {f.rich_text, sizeof(f.rich_text)}) &&
              ui_variable_list_initialize(&f.list, {f.heights, 2}, {f.fenwick, 2}, 2, ui_extent_units(57)) &&
              message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57),
                                          ui_extent_units(1000), false) &&
              message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
              message_scene_initialize_owned(&f.scene, &f.timeline, 2, 2, {f.widgets, widgets}, {f.nodes, widgets});
    GatewayMessageAttachment attachments[2]{};
    attachments[0].id = 70;
    attachments[0].filename = utf8_literal("diagram.png");
    attachments[0].content_type = utf8_literal("image/png");
    attachments[0].url = utf8_literal("https://example.com/diagram.png");
    attachments[0].width = 640;
    attachments[0].height = 360;
    attachments[0].has_width = attachments[0].has_height = true;
    attachments[1].id = 71;
    attachments[1].filename = utf8_literal("notes.txt");
    attachments[1].content_type = utf8_literal("text/plain");
    attachments[1].url = utf8_literal("https://example.com/notes.txt");
    attachments[1].size = 2048;
    GatewayMessageEvent first{};
    first.id = 1;
    first.fields = gateway_message_field_attachments;
    first.attachments = {attachments, 2};
    GatewayMessageEmbed embeds[2]{};
    embeds[0].type = GatewayMessageEmbedType::rich;
    embeds[0].title = utf8_literal("Fixture article");
    embeds[0].has_title = true;
    embeds[0].description = utf8_literal("A synthetic link preview with a thumbnail.");
    embeds[0].url = utf8_literal("https://example.com/article");
    embeds[0].thumbnail.url = utf8_literal("https://example.com/thumb.png");
    embeds[0].thumbnail.width = embeds[0].thumbnail.height = 80;
    embeds[0].thumbnail.has_width = embeds[0].thumbnail.has_height = true;
    embeds[0].color = 0x3366CC;
    embeds[0].has_color = true;
    embeds[1].type = GatewayMessageEmbedType::image;
    embeds[1].thumbnail.url = utf8_literal("https://example.com/photo.png");
    embeds[1].thumbnail.width = 400;
    embeds[1].thumbnail.height = 300;
    embeds[1].thumbnail.has_width = embeds[1].thumbnail.has_height = true;
    GatewayMessageEvent second{};
    second.id = 2;
    second.fields = gateway_message_field_embeds;
    second.embeds = {embeds, 2};
    ok = ok && message_rich_store_apply(&f.rich, 1, f.record[0].revision, first).ok() &&
         message_rich_store_apply(&f.rich, 2, f.record[1].revision, second).ok() &&
         message_scene_update(&f.scene, {0, 0, 740, 1800}).ok();
    static MessageMediaCommand commands[16]{};
    MessageMediaCommandStream media{};
    MessagePresentationResult result{};
    ok = ok && begin(740, 1800) && message_media_command_stream_initialize(&media, {commands, 16}) &&
         message_presentation_build(f.scene, kDefaultTheme, &g.paint, &media, &result);
    check(ok, "message list fixture");
    finish("message list", &f.scene.widgets.layout);
}

UiCallParticipant person(u64 id, Utf8View name, u32 ssrc = 0) noexcept {
    UiCallParticipant p{};
    p.user_id = id;
    p.name = name;
    p.video_ssrc = ssrc;
    return p;
}
void call_view() noexcept {
    static UiCallScene scene{};
    UiCallParticipant people[4]{person(1, utf8_literal("Ana"), 101), person(2, utf8_literal("Bo")),
                                person(3, utf8_literal("Cy"), 103), person(4, utf8_literal("Di"))};
    people[0].video_width = 1280;
    people[0].video_height = 720;
    people[1].muted = people[1].speaking = true;
    people[3].live = true;
    UiCallContent content{};
    content.title = utf8_literal("General");
    content.participants = {people, 4};
    content.connected = true;
    const UiRect viewport{300, 0, 1500, 900};
    bool ok = ui_call_scene_update(&scene, content, viewport, 1000, true) &&
              (ui_call_scene_set_hover(&scene, 600, 300), true) && begin(1500, 900) &&
              ui_call_scene_paint(scene, kDefaultTheme, &g.paint) &&
              ui_call_scene_paint_foreground(scene, kDefaultTheme, &g.paint);
    check(ok, "call view fixture");
    finish("call view", &scene.widgets.layout);
    // The control bar under the pointer.
    ui_call_scene_set_hover(&scene, (scene.bar.left + scene.bar.right) * .5f, (scene.bar.top + scene.bar.bottom) * .5f);
    ok = begin(1500, 900) && ui_call_scene_paint(scene, kDefaultTheme, &g.paint) &&
         ui_call_scene_paint_foreground(scene, kDefaultTheme, &g.paint);
    check(ok, "call bar fixture");
    finish("call bar");
    ui_call_scene_release(&scene);
}

// The shell: server rail with badges and a selected server, the channel list,
// the account panel and the member list with a hovered row.
struct Shell {
    UiScene scene;
    ChatStateStore state;
    GuildRecord guild;
    ChannelRecord channels[6];
    u16 guild_order[1], channel_order[6];
    u8 names[1024];
    NavigationView view;
    GuildViewRow guild_rows[5];
    ChannelViewRow channel_rows[6];
    GuildRailScene rail;
    GuildRailRowWidget rail_rows[5];
    UiWidget rail_widgets[7];
    UiLayoutNode rail_nodes[7];
    ShellNavigationScene navigation;
    ShellNavigationRowWidget navigation_rows[6];
    UiWidget navigation_widgets[26];
    UiLayoutNode navigation_nodes[26];
    UserRecord users[6];
    u32 user_order[6];
    u8 user_names[512];
    UserStore user_store;
    MemberListRow member_rows[7];
    MemberListView members;
    GuildMemberStore guild_members;
    MemberListScene member_scene;
    MemberListRowWidget member_widgets[8];
    UiWidget member_ui[1 + 8 * kMemberListWidgetsPerRow];
    UiLayoutNode member_nodes[1 + 8 * kMemberListWidgetsPerRow];
};
void shell() noexcept {
    static Shell f{};
    bool ok = ui_scene_initialize_bootstrap(&f.scene) &&
              ui_scene_layout_with_metrics(&f.scene, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics, 1280,
                                           800) &&
              chat_state_store_initialize(&f.state, {&f.guild, 1}, {f.guild_order, 1}, {f.channels, 6},
                                          {f.channel_order, 6}, {f.names, sizeof(f.names)}) &&
              chat_state_store_upsert_guild(&f.state, {7, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok() &&
              guild_rail_scene_initialize(&f.rail, {f.rail_rows, 5}, {f.rail_widgets, 7}, {f.rail_nodes, 7}) &&
              shell_navigation_scene_initialize(&f.navigation, {f.navigation_rows, 6}, {f.navigation_widgets, 26},
                                                {f.navigation_nodes, 26});
    f.view.guild_rows = f.guild_rows;
    f.view.guild_count = f.view.guild_capacity = 5;
    f.view.channel_rows = f.channel_rows;
    f.view.channel_count = f.view.channel_capacity = 6;
    f.view.selected_guild_id = 7;
    f.view.selected_channel_id = 81;
    f.view.guild_revision = f.view.channel_revision = 1;
    constexpr u32 mentions[]{1, 12, 999, 0, 3};
    for (u32 index = 0; ok && index < 6; ++index) {
        ChannelInput input{};
        input.id = 80 + index;
        input.guild_id = 7;
        input.name = utf8_literal("channel");
        input.name_known = true;
        ok = chat_state_store_upsert_channel(&f.state, input).ok();
        f.channel_rows[index] = {};
        f.channel_rows[index].id = input.id;
        f.channel_rows[index].position = index;
        f.channel_rows[index].selectable = true;
        f.channel_rows[index].unread = index == 5;
        f.channel_rows[index].mention_count = index < 5 ? mentions[index] : 0;
        if (index < 5) {
            f.guild_rows[index] = {};
            f.guild_rows[index].id = 7 + index;
            f.guild_rows[index].mention_count = mentions[index];
        }
    }
    ok = ok && user_store_initialize(&f.user_store, {f.users, 6}, {f.user_order, 6}, {f.user_names, sizeof(f.user_names)});
    constexpr GuildMemberStatus statuses[]{GuildMemberStatus::online, GuildMemberStatus::idle, GuildMemberStatus::dnd,
                                           GuildMemberStatus::offline, GuildMemberStatus::online,
                                           GuildMemberStatus::online};
    f.member_rows[0] = {0, kMemberListOnlineGroup, 0, 0, 0, GuildMemberStatus::online, MemberListRowKind::group};
    f.member_rows[0].group_count = 6;
    for (u32 i = 0; ok && i < 6; ++i) {
        ok = user_store_upsert(&f.user_store, 500 + i, utf8_literal("Member"), utf8_literal("member")).ok();
        f.member_rows[i + 1] = {500 + i, kMemberListOnlineGroup, 0, 0, 0, statuses[i], MemberListRowKind::member};
        f.member_rows[i + 1].logical_index = i;
    }
    f.members.rows = {f.member_rows, 7};
    f.members.count = 7;
    f.members.member_count = 6;
    f.members.revision = 1;
    ok = ok && member_list_scene_initialize(&f.member_scene, {f.member_widgets, 8},
                                            {f.member_ui, 1 + 8 * kMemberListWidgetsPerRow},
                                            {f.member_nodes, 1 + 8 * kMemberListWidgetsPerRow});
    const auto* rail = ui_scene_resolve(f.scene, ui_scene_find(f.scene, UiSemantic::guild_navigation));
    const auto* channels = ui_scene_resolve(f.scene, ui_scene_find(f.scene, UiSemantic::channel_navigation));
    const auto* members = ui_scene_resolve(f.scene, ui_scene_find(f.scene, UiSemantic::member_sidebar));
    ok = ok && rail && channels && members && guild_rail_scene_update(&f.rail, &f.view, rail->rectangle, 0).ok() &&
         shell_navigation_scene_update(&f.navigation, &f.view, channels->rectangle, 0).ok() &&
         member_list_scene_update(&f.member_scene, &f.members, &f.guild_members, &f.user_store, &f.state,
                                  members->rectangle, 0)
             .ok();
    if (ok && f.member_scene.count > 2)
        ui_widget_manager_hover(&f.member_scene.widgets, f.member_scene.rows[2].widget);
    if (ok && f.rail.count > 1)
        ui_widget_manager_hover(&f.rail.widgets, f.rail.rows[1].widget);
    if (ok && f.navigation.count > 3)
        ui_widget_manager_hover(&f.navigation.widgets, f.navigation.rows[3].widget);
    ShellPresentationRegions regions{};
    ok = ok && shell_presentation_build_chrome(f.scene, &f.view, &f.rail, &f.navigation, nullptr, &f.member_scene,
                                               kDefaultTheme, 1280, 800, &g.paint, &regions, nullptr);
    check(ok, "shell fixture");
    finish("shell chrome", &f.scene.widgets.layout);
    layout_consistency_record_layout(f.rail.widgets.layout, "server rail", &g.padding);
    layout_consistency_record_layout(f.navigation.widgets.layout, "channel list", &g.padding);
    layout_consistency_record_layout(f.member_scene.widgets.layout, "member list", &g.padding);
    ok = ok && begin(1280, 800) &&
         shell_presentation_build_guild_rail_overlay(f.scene, &f.rail, kDefaultTheme, &g.paint) &&
         shell_presentation_build_member_status_overlay(&f.member_scene, kDefaultTheme, &g.paint);
    check(ok, "shell overlay fixture");
    finish("shell overlays");
    // A background theme paints the add-server tile with the rail's surface
    // material, like the Home button, rather than a flat grey quad.
    // The fixture's rail stores only its rows; this one has room for the button.
    static GuildRailScene add_rail{};
    static GuildRailRowWidget add_rows[5]{};
    static UiWidget add_widgets[8]{};
    static UiLayoutNode add_nodes[8]{};
    Theme background = kDefaultTheme;
    background.background = background_theme_defaults();
    ok = ok &&
         guild_rail_scene_initialize(&add_rail, {add_rows, 5}, {add_widgets, 8}, {add_nodes, 8}) &&
         guild_rail_scene_update(&add_rail, &f.view, rail->rectangle, 0).ok();
    const UiRect add_tile =
        ok ? guild_rail_icon_rectangle(guild_rail_scene_add_rectangle(add_rail)) : UiRect{};
    bool add_material = false;
    ok = ok && ui_rect_has_area(add_tile) && begin(1280, 800) &&
         shell_presentation_build_chrome(f.scene, &f.view, &add_rail, &f.navigation, nullptr,
                                         &f.member_scene, background, 1280, 800, &g.paint, &regions,
                                         nullptr);
    for (u32 i = 0; ok && i < g.paint.count; ++i) {
        const auto& quad = g.paint.quads[i];
        add_material |= quad.border_width == -7 && quad.left == add_tile.left && quad.top == add_tile.top &&
                        quad.right == add_tile.right && quad.bottom == add_tile.bottom;
    }
    check(ok && add_material, "shell chrome: the add-server tile takes the background theme's surface material");
}

void server_settings() noexcept {
    static UiSettingsScene scene{};
    static UiPage page{};
    static UiPageRegistry registry{};
    static UiSettingsEditor name{{utf8_literal("native-room"), 2, 5, true, true, true}, nullptr,
                                 utf8_literal("Channel name")};
    static UiSettingsEditor topic{{utf8_literal("Topic"), 0, 0, true, false, false}, nullptr, utf8_literal("Add a topic")};
    static const Utf8View choices[]{utf8_literal("Off"), utf8_literal("5 seconds")};
    static const Utf8View segments[]{utf8_literal("Text"), utf8_literal("Voice")};
    static UiSettingsControl controls[]{
        {1, utf8_literal("Name"), {}, UiSettingsControlKind::edit, true, false, {}, 0, {}, &name},
        {2, utf8_literal("Topic"), {}, UiSettingsControlKind::multiline, true, false, {}, 0, {}, &topic},
        {3, utf8_literal("Slowmode"), {}, UiSettingsControlKind::choice, true, false, {choices, 2}, 0, {}},
        {4, utf8_literal("Age-restricted"), utf8_literal("Viewers confirm their age."), UiSettingsControlKind::toggle,
         true, true},
        {5, utf8_literal("Channel type"), {}, UiSettingsControlKind::segments, true, false, {segments, 2}, 0},
        {6, utf8_literal("Delete channel"), {}, UiSettingsControlKind::action, true, false, {}, 0,
         utf8_literal("Delete")}};
    static UiSettingsFooter footer{{}, utf8_literal("Save changes"), utf8_literal("Reset"), true, true, false, false};
    static UiSettingsPageContent content{utf8_literal("Overview"), {}, {controls, 6}, 1, kUiSettingsPageFormat, nullptr,
                                         &footer};
    content.navigation_label = utf8_literal("#native-room");
    bool ok = ui_page_registry_initialize(&registry, {&page, 1}) &&
              ui_page_registry_register(&registry, {{9, 1}, utf8_literal("Overview"), &content}).ok() &&
              ui_settings_scene_update(&scene, &registry, {0, 0, 1280, 900}) && begin(1280, 900) &&
              ui_settings_scene_paint(scene, kDefaultTheme, &g.paint);
    check(ok, "server settings fixture");
    finish("server settings", &scene.widgets.layout);
    // Keyboard focus on a toggle and an action button.
    constexpr u64 focused[]{4, 6};
    for (const u64 control : focused) {
        ui_settings_scene_set_focus_visible(&scene, true);
        ok = ui_settings_scene_focus(&scene, {UiSettingsTargetKind::control, {9, 1}, control}) && begin(1280, 900) &&
             ui_settings_scene_paint(scene, kDefaultTheme, &g.paint);
        check(ok, "server settings focus fixture");
        finish("server settings focus");
    }
    ui_settings_scene_release(&scene);
}

// Settings > Plugins from the real catalog projection over a small snapshot.
struct Catalog {
    PluginCatalogSnapshot snapshot;
    PluginCatalogEntryView entries[4];
    PluginCatalogVersionView versions[4];
    char documents[4096];
    app_detail::PluginBrowserBank bank;
    UiSettingsScene scene;
    UiPage page;
    UiPageRegistry registry;
    UiSettingsPageContent content;
};
PluginIndexText catalog_text(Catalog& c, const char* value) noexcept {
    auto& provider = c.snapshot.providers[0];
    const u32 offset = provider.document_bytes;
    u32 n = 0;
    while (value[n]) {
        c.documents[offset + n] = value[n];
        ++n;
    }
    provider.document_bytes += n;
    c.snapshot.documents_bytes = provider.document_bytes;
    return {offset, n};
}
void plugin_browser() noexcept {
    static Catalog c{};
    auto& snapshot = c.snapshot;
    snapshot.entries = c.entries;
    snapshot.versions = c.versions;
    snapshot.documents = c.documents;
    snapshot.configured = true;
    snapshot.provider_count = 1;
    auto& official = snapshot.providers[0];
    official.id = kPluginProviderOfficial;
    official.kind = PluginProviderKind::official;
    official.status = PluginCatalogStatus::ok;
    official.installable = official.enabled = official.check_daily = true;
    for (u32 i = 0; i < 4; ++i) {
        char name[16] = "Plugin ";
        name[7 + decimal_write(name + 7, i)] = 0;
        auto& e = c.entries[i];
        e.owner = 1000 + i;
        e.name = catalog_text(c, name);
        e.summary = catalog_text(c, "Does useful things.");
        e.author = catalog_text(c, "Author");
        e.license = catalog_text(c, "MPL-2.0");
        e.author_count = 1;
        e.categories = 1u << i;
        e.first_version = i;
        e.version_count = 1;
        e.installed_index = ~0u;
        c.versions[i].code = 1000000999;
        c.versions[i].released = 1760200000;
        c.versions[i].bytes = 48640;
        c.versions[i].permissions = 1;
        c.versions[i].compatibility = PluginCompatibility::compatible;
    }
    snapshot.entry_count = snapshot.version_count = 4;
    app_detail::PluginBrowserFacts facts{};
    facts.snapshot = &snapshot;
    facts.configured = true;
    facts.now_unix = 1760400000;
    facts.checked_unix = facts.now_unix - 300;
    const u32 count = app_detail::plugin_browser_project({}, facts, &c.bank);
    c.content = {utf8_literal("Plugins"), utf8_literal("Add features to MaiCord."), {c.bank.controls, count}, 1};
    c.content.presentation = c.bank.presentation;
    bool ok = count && ui_page_registry_initialize(&c.registry, {&c.page, 1}) &&
              ui_page_registry_register(&c.registry, {{1, 4}, utf8_literal("Plugins"), &c.content}).ok() &&
              ui_settings_scene_update(&c.scene, &c.registry, {0, 0, 1280, 900}) && begin(1280, 900) &&
              ui_settings_scene_paint(c.scene, kDefaultTheme, &g.paint);
    check(ok, "plugin browser fixture");
    finish("plugin browser", &c.scene.widgets.layout);
    ui_settings_scene_release(&c.scene);
}

void profile_popout() noexcept {
    static UserProfile profile{};
    profile.user_id = 42;
    profile.guild_id = 7;
    const auto set = [](auto* out, Utf8View text) noexcept {
        copy_bytes(out->data, text.data, text.bytes);
        out->bytes = static_cast<u16>(text.bytes);
    };
    set(&profile.username, utf8_literal("popout"));
    set(&profile.display_name, utf8_literal("Popout Person"));
    static GuildRecord guilds[1]{};
    static u16 order[1]{};
    static RoleRecord roles[3]{};
    static u8 names[64]{};
    static ChatStateStore state{};
    state.guilds = guilds;
    state.guild_order = order;
    state.guild_count = 1;
    state.roles = roles;
    state.role_count = 3;
    state.names = names;
    state.name_bank_bytes = 64;
    state.self_user_id = 1;
    guilds[0].id = 7;
    guilds[0].owner_id = 99;
    for (u32 i = 0; i < 3; ++i) {
        roles[i] = {};
        roles[i].id = 100 + i;
        roles[i].guild_id = 7;
        roles[i].position = i + 1;
        roles[i].color = 0x3366CC;
        roles[i].name_offset = i * 16;
        roles[i].name_bytes = 9;
        copy_bytes(names + i * 16, "Role name", 9);
        profile.roles[profile.role_count++] = roles[i].id;
    }
    static UserStore users{};
    static ProfileScene scene{};
    bool ok = profile_scene_open(&scene, &profile, &state, &users, 1, {300, 200, 340, 240}, {0, 0, 1280, 720}) &&
              begin(1280, 720) && profile_scene_paint(scene, kDefaultTheme, &g.paint);
    check(ok, "profile popout fixture");
    finish("profile popout", &scene.widgets.layout);
    profile_scene_release(&scene);
}

void run() noexcept {
    if (!paint_stream_initialize(&g.paint)) {
        check(false, "paint stream");
        return;
    }
    media_viewer();
    video_player();
    context_menus();
    tooltip();
    dialogs();
    forward_dialog();
    message_list();
    call_view();
    shell();
    server_settings();
    plugin_browser();
    profile_popout();
    layout_consistency_check_padding(g.padding, &g.report);
    layout_consistency_print(g.report);
    test_write_metric("layout consistency scenes: ", g.scenes);
    test_write_metric("radius violations: ", g.report.radius);
    test_write_metric("clip violations: ", g.report.clip);
    test_write_metric("padding outliers: ", g.report.padding);
    check(g.report.radius == 0, "nested rounded rectangles are concentric within 0.5 DIP");
    check(g.report.clip == 0, "rounded highlights stay inside their clips");
    check(g.report.padding == 0, "each component type keeps one padding");
    paint_stream_release(&g.paint);
}
} // namespace layout_consistency_test
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_layout_consistency_tests_entry() noexcept {
    maicord::layout_consistency_test::run();
    ExitProcess(maicord::layout_consistency_test::failures ? 1 : 0);
}
