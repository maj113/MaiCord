#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "caption.hpp"
#include "state.hpp"
#include "window_state.hpp"
#include "window_events.hpp"
#include "../account/account_presentation.hpp"
#include "../settings/settings.hpp"
#include "../account/friends.hpp"
#include "../chat/threads/browser.hpp"
#include "../media/video.hpp"
#include "../guild/folders.hpp"
#include "../voice/voice.hpp"
#include "../plugins/plugins.hpp"
#include "../chat/messages/reaction_tooltip.hpp"
#include "../media/emoji_tooltip.hpp"
#include "../chat/composer/interaction_card.hpp"
#include "maicord/ui/plugins/slots.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/text/timestamp_format.hpp"
#include "maicord/ui/widgets/tooltip.hpp"
#include "maicord/ui/voice/call_scene.hpp"
#include "maicord/ui/chat/composer/voice_bar.hpp"
#include "maicord/ui/settings/theme_editor.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/chat/pinned_messages_scene.hpp"
#include "maicord/ui/account/profile_scene.hpp"
#include "maicord/ui/account/dm_profile_panel.hpp"
#include "maicord/ui/media/picker.hpp"
#include "maicord/render/d3d11/renderer_settings_visibility.hpp"
#include "maicord/render/text/forum_post_surface.hpp"
#include "maicord/ui/chat/messages/author.hpp"
#include "maicord/ui/guild/card_scene.hpp"
#include "maicord/ui/guild/member_list_scene.hpp"
#include "../account/account_ui_runtime.hpp"
#include "maicord/model/chat/game_store.hpp"

#include "maicord/platform/window/key.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/ui/chat/threads/browser_scene.hpp"
#include "maicord/ui/chat/threads/forum_post_scene.hpp"

namespace maicord::app_detail {
struct ApplicationTooltip {
    UiTooltipScene scene;
    UiTooltipKey dismissed;
    UiTooltipKey failed;
    UiRect failed_anchor;
    UiPoint point;
    UiWidgetManager widgets;
    UiWidget root;
    UiLayoutNode root_layout;
    UiRect viewport;
    u32 published_revision;
    bool published_visible;
    // Until then another profile badge's tooltip shows without its delay.
    u64 badge_warm_until_ms;
    bool pointer_valid, keyboard, pressed;
    bool viewport_failed;
    // The shown tooltip's entrance (kUiMotionTooltip), restarted for each newly shown target.
    UiMotionTransition entrance;
    UiTooltipKey entrance_key;
    u64 entrance_generation;
};
static_assert(sizeof(ApplicationTooltip) <= 4096);
namespace {
constexpr u64 kTooltipMotionOwner = 0x544f'4f4c'5449'5031ull; // "TOOLTIP1"
bool content_available() noexcept {
    return g_app.renderer_ready && g_app.account_ui && !g_app.login.visible &&
           !(g_app.renderer.forum_form && g_app.renderer.forum_form->scene && g_app.renderer.forum_form->scene->active) &&
           (!application_settings_active() || renderer_theme_editor_visible(g_app.renderer)) &&
           !g_app.text_form && !g_app.popup_scene.active && !g_app.reaction_viewer.active &&
           !g_app.image_viewer.scene.active &&
           !(g_app.renderer.profile_scene && g_app.renderer.profile_scene->active) &&
           !application_friends_requests_active() && !application_window_minimized();
}
// The media viewer owns the window while open; only its toolbar and chevrons
// offer tooltips, and not while idle playback hides them.
bool viewer_available() noexcept {
    const auto& viewer = g_app.image_viewer;
    return g_app.renderer_ready && viewer.scene.active && !viewer.closing && viewer.scene.chrome >= 1 &&
           !application_window_minimized();
}
bool widget_target(const UiWidgetManager& manager, UiWidgetHandle handle, u64 scope, u64 identity,
                   u64 revision, u32 action, const Utf8View& label, UiTooltipTarget* target) noexcept {
    if (!target || !label.bytes || !ui_widget_manager_input_enabled(manager, handle))
        return false;
    const auto* node = ui_widget_manager_layout_node(manager, handle);
    if (!node)
        return false;
    const auto anchor = ui_rect_intersection(node->rectangle, node->clip_rectangle);
    if (!ui_rect_has_area(anchor))
        return false;
    *target = {{{&manager, handle}, scope, identity, revision, action}, anchor, label};
    return true;
}
bool shell_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto& manager = g_app.shell_scene.widgets;
    UiRect voice_anchor{};
    u32 voice_identity = 0;
    if (const Utf8View voice = keyboard ? Utf8View{}
                                        : application_voice_tooltip(point.x, point.y, &voice_anchor, &voice_identity);
        voice.bytes) {
        const auto footer = ui_widget_manager_find(manager, static_cast<u32>(UiSemantic::channel_footer) + 1);
        *target = {{{&manager, footer}, scope, 0x5600u + voice_identity, voice_identity, 0x5600u + voice_identity},
                   voice_anchor,
                   voice};
        return true;
    }
    const auto handle = keyboard ? manager.focused : ui_widget_manager_hit_test(manager, point.x, point.y);
    const auto* widget = ui_widget_manager_resolve(manager, handle);
    if (!widget)
        return false;
    Utf8View label{};
    if (widget->id == static_cast<u32>(UiSemantic::settings) + 1)
        label = utf8_literal("User settings");
    else if (widget->id == static_cast<u32>(UiSemantic::member_toggle) + 1)
        label = ui_side_panel_toggle_label(g_app.shell_scene.side_panel_profile, g_app.shell_scene.side_panel_unavailable,
                                           g_app.shell_scene.side_panel_profile ? g_app.preferences.dm_profile_visible
                                                                                : g_app.preferences.member_pane_visible);
    else if (widget->id == static_cast<u32>(UiSemantic::pins_toggle) + 1)
        label = utf8_literal("Pinned messages");
    else if (widget->id == static_cast<u32>(UiSemantic::inbox_toggle) + 1)
        label = utf8_literal("Inbox");
    else if (widget->id == static_cast<u32>(UiSemantic::threads_toggle) + 1)
        label = utf8_literal("Browse threads");
    else if (widget->id == static_cast<u32>(UiSemantic::friends_button) + 1)
        label = utf8_literal("Friends");
    else if (widget->id == static_cast<u32>(UiSemantic::guild_invite) + 1)
        label = utf8_literal("Invite people");
    else if (widget->id == static_cast<u32>(UiSemantic::private_create) + 1)
        label = utf8_literal("Create DM");
    else if (widget->id == static_cast<u32>(UiSemantic::private_add) + 1)
        label = utf8_literal("Add friends to DM");
    else if (widget->id == static_cast<u32>(UiSemantic::private_call) + 1)
        label = utf8_literal("Start voice call");
    else if (widget->id == static_cast<u32>(UiSemantic::home_button) + 1)
        label = utf8_literal("Direct messages");
    else if (widget->id == static_cast<u32>(UiSemantic::channel_footer) + 1) {
        // The widget manager skips disabled children. A disabled Settings gear
        // must not inherit its profile-footer parent's hint at the same point.
        const auto settings = ui_widget_manager_find(manager, static_cast<u32>(UiSemantic::settings) + 1);
        const auto* settings_node = ui_widget_manager_layout_node(manager, settings);
        if (!keyboard && settings_node && settings_node->visible &&
            ui_rect_contains(ui_rect_intersection(settings_node->rectangle, settings_node->clip_rectangle),
                             point.x, point.y))
            return false;
        label = utf8_literal("Your account");
    }
    return widget_target(manager, handle, scope, widget->id,
                         g_app.shell_scene.side_panel_profile ? g_app.preferences.dm_profile_visible
                                                              : g_app.preferences.member_pane_visible,
                         widget->id, label, target);
}
// Call view controls; the scene owns its labels and state (mute, fullscreen).
bool call_target(UiPoint point, u64 scope, UiTooltipTarget* target) noexcept {
    const auto* scene = g_app.renderer.call_scene;
    UiRect anchor{};
    u32 identity = 0;
    const Utf8View label = scene ? ui_call_scene_tooltip(*scene, point.x, point.y, &anchor, &identity) : Utf8View{};
    if (!label.bytes || !ui_rect_has_area(anchor))
        return false;
    *target = {{scene->owner, scope, 0xca00u + identity, identity, 0xca00u + identity}, anchor, label};
    return true;
}
bool friends_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto* scene = g_app.renderer.friends_scene;
    if (!scene || !application_friends_active())
        return false;
    UiWidgetHandle widget{};
    UiFriendsTarget source{};
    Utf8View label{};
    return ui_friends_scene_icon_action(*scene, keyboard, point.x, point.y, &widget, &source, &label) &&
           widget_target(scene->widgets, widget, scope, source.user_id, scene->revision,
                         0xe00u + static_cast<u32>(source.action), label, target);
}
bool guild_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto& scene = g_app.guild_rail_scene;
    if (!scene.active)
        return false;
    const auto handle = keyboard ? scene.widgets.focused : ui_widget_manager_hit_test(scene.widgets, point.x, point.y);
    const auto* row = guild_rail_scene_resolve(scene, handle);
    if (!row && ui_widget_handle_matches(scene.add_button, handle)) {
        if (!widget_target(scene.widgets, handle, scope, 0, 1, 0xd01u, utf8_literal("Add a server"), target))
            return false;
        target->placement = UiTooltipPlacement::right;
        return true;
    }
    char name[kUiTooltipMaximumTextBytes];
    const Utf8View label = row ? application_guild_rail_label(row->view, {name, sizeof(name)}) : Utf8View{};
    if (!row || !widget_target(scene.widgets, handle, scope, row->view.id, row->view.revision, 0xd00u, label, target))
        return false;
    target->placement = UiTooltipPlacement::right;
    return true;
}
bool composer_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    if (application_friends_active() || application_thread_browser_read_blocked() || !g_app.message_composer_scene.active)
        return false;
    const auto& scene = g_app.message_composer_scene;
    const auto control = keyboard ? message_composer_scene_focused_control(scene)
                                  : message_composer_scene_hit_test(scene, point.x, point.y);
    Utf8View label{};
    UiWidgetHandle handle{};
    const UiComposerVoiceMode voice = message_composer_scene_voice_mode(scene);
    const bool voice_bar = ui_composer_voice_bar_visible(voice);
    switch (control) {
    case MessageComposerControl::add_attachment:
        label = voice_bar ? utf8_literal("Delete voice message") : utf8_literal("Upload or create");
        handle = scene.add_attachment;
        break;
    case MessageComposerControl::gif:
        label = utf8_literal("GIFs");
        handle = scene.gif;
        break;
    case MessageComposerControl::sticker:
        label = utf8_literal("Stickers");
        handle = scene.sticker;
        break;
    case MessageComposerControl::emoji:
        label = !voice_bar                                ? utf8_literal("Emoji")
                : voice == UiComposerVoiceMode::recording ? utf8_literal("Stop recording")
                : scene.voice && scene.voice->playing     ? utf8_literal("Pause voice message")
                                                          : utf8_literal("Play voice message");
        handle = scene.emoji;
        break;
    case MessageComposerControl::send:
        label = g_app.message_edit.editor_active          ? utf8_literal("Save edit")
                : voice == UiComposerVoiceMode::available ? utf8_literal("Record Voice Message")
                : voice_bar                               ? utf8_literal("Send Voice Message")
                                                          : utf8_literal("Send message");
        handle = scene.send;
        break;
    case MessageComposerControl::cancel_reply:
        label = g_app.message_edit.editor_active ? utf8_literal("Cancel edit") : utf8_literal("Cancel reply");
        handle = scene.reply_cancel;
        break;
    case MessageComposerControl::command_close:
        label = utf8_literal("Close");
        handle = scene.command_close;
        break;
    case MessageComposerControl::reply_mention:
        label = scene.mention_reply_author ? utf8_literal("Click to disable pinging the original author.")
                                          : utf8_literal("Click to enable pinging the original author.");
        handle = scene.reply_mention;
        break;
    case MessageComposerControl::attachments: {
        // Upload tile action bar: the official client's per-button labels.
        if (keyboard)
            return false;
        const auto hit = message_composer_scene_attachment_hit(scene, point.x, point.y);
        const auto tiles = message_composer_scene_attachment_tiles(scene);
        if (hit.index >= tiles.count || hit.part == UiComposerAttachmentPart::tile ||
            hit.part == UiComposerAttachmentPart::none)
            return false;
        const auto& tile = tiles[hit.index];
        label = hit.part == UiComposerAttachmentPart::retry     ? utf8_literal("Retry upload")
                : hit.part == UiComposerAttachmentPart::spoiler ? (tile.spoiler ? utf8_literal("Remove spoiler")
                                                                                : utf8_literal("Spoiler attachment"))
                : hit.part == UiComposerAttachmentPart::edit    ? utf8_literal("Modify attachment")
                                                                : utf8_literal("Remove attachment");
        const auto view = message_composer_scene_attachment_view(scene, false);
        const auto geometry = ui_composer_attachment_tile_geometry(view.strip, tile, hit.index, view.scroll);
        UiRect anchor{};
        for (u32 i = 0; i < geometry.button_count; ++i)
            if (geometry.parts[i] == hit.part)
                anchor = ui_rect_intersection(geometry.buttons[i], view.strip);
        if (!ui_rect_has_area(anchor) || !ui_widget_manager_input_enabled(scene.widgets, scene.attachments))
            return false;
        *target = {{{&scene.widgets, scene.attachments}, scope, tile.id, tile.spoiler ? 1u : 0u,
                    0x300u + static_cast<u32>(hit.part)},
                   anchor,
                   label};
        return true;
    }
    case MessageComposerControl::plugin_slot: {
        // Plugin tooltip text stays borrowed until the next slot revision.
        const u32 index = keyboard ? message_composer_scene_focused_plugin_slot(scene)
                                   : message_composer_scene_plugin_slot(scene, point.x, point.y);
        UiPluginSlotNode nodes[kMessageComposerPluginSlots];
        if (index >= ui_plugin_slot_toolbar(kUiPluginSlotComposerToolbar, {nodes, kMessageComposerPluginSlots}))
            return false;
        return widget_target(scene.widgets, scene.plugin_slots[index], scope, g_app.account_ui->active_channel_id,
                             g_plugin_surface_revision, 0x180u + index,
                             nodes[index].tooltip.bytes ? nodes[index].tooltip : nodes[index].text, target);
    }
    default:
        return false;
    }
    return widget_target(scene.widgets, handle, scope, g_app.account_ui->active_channel_id,
                         scene.reply_message_id, 0x100u + static_cast<u32>(control), label, target);
}
// Labels by action: each list runs over consecutive enum values from the first named one (name_list.hpp; an action
// outside the run has no label and no tooltip).
constexpr char kActionBarLabels[] = "Add reaction\0Reply\0More\0Forward\0Edit\0Copy message link\0Mark unread\0Pin message\0"
                                    "Unpin message\0Delete message\0Copy message ID\0Retry send\0";
static_assert(name_list_count(kActionBarLabels) ==
              static_cast<u32>(MessageActionBarAction::retry) - static_cast<u32>(MessageActionBarAction::add_reaction) + 1);
constexpr char kSearchLabels[] = "Search messages\0Search\0Close search\0Search filters\0Previous results\0Next results\0"
                                 "Newest first\0Oldest first\0Most relevant\0Jump to message\0";
static_assert(name_list_count(kSearchLabels) ==
              static_cast<u32>(UiSearchAction::result) - static_cast<u32>(UiSearchAction::edit) + 1);
constexpr char kPinnedLabels[] = "Close pins\0Refresh pins\0Jump to message\0Unpin message\0Newer pins\0Older pins\0Retry loading pins\0";
static_assert(name_list_count(kPinnedLabels) ==
              static_cast<u32>(UiPinnedMessagesAction::retry) - static_cast<u32>(UiPinnedMessagesAction::close) + 1);
// close..retry; open_thread (its label depends on the browser) and create_post (none) follow.
constexpr char kBrowserLabels[] = "Close thread browser\0Active threads\0Archived threads\0Previous threads\0Next threads\0"
                                  "Retry loading threads\0";
static_assert(name_list_count(kBrowserLabels) ==
              static_cast<u32>(UiThreadBrowserAction::retry) - static_cast<u32>(UiThreadBrowserAction::close) + 1);
constexpr char kForumLabels[] = "Sort by\0List\0Gallery\0Filter by tag\0";
static_assert(name_list_count(kForumLabels) ==
              static_cast<u32>(UiThreadBrowserAction::forum_tag) - static_cast<u32>(UiThreadBrowserAction::forum_sort) + 1);
bool message_target(UiPoint point, u64 scope, UiTooltipTarget* target) noexcept {
    const auto& bar = g_app.message_action_bar;
    if (application_friends_active() || application_thread_browser_read_blocked() || !bar.active || bar.owner.manager != &g_app.message_scene.widgets)
        return false;
    const auto hit = message_action_bar_hit_test(bar, point.x, point.y);
    const Utf8View label =
        hit.action == MessageActionBarAction::plugin_0 || hit.action == MessageActionBarAction::plugin_1
            ? application_plugins_hover_label(hit.catalog_index)
            : name_list_at(kActionBarLabels,
                           static_cast<u32>(hit.action) - static_cast<u32>(MessageActionBarAction::add_reaction));
    if (!label.bytes)
        return false;
    const auto* message = application_message_record(bar.message_id);
    if (!message || message->channel_id != g_app.account_ui->active_channel_id ||
        !ui_widget_manager_resolve(g_app.message_scene.widgets, bar.owner.handle))
        return false;
    const auto anchor = ui_rect_intersection(hit.rectangle, bar.clip);
    if (!ui_rect_has_area(anchor))
        return false;
    *target = {{bar.owner, scope, message->id, message->revision, 0x200u + static_cast<u32>(hit.action)},
               anchor,
               label};
    return true;
}
// The check of a verified app's APP tag beside a message's author.
bool app_check_target(UiPoint point, u64 scope, UiTooltipTarget* target) noexcept {
    const auto& renderer = g_app.renderer;
    if (application_friends_active() || !renderer.message_text || !renderer.chat_state || !renderer.user_store)
        return false;
    const auto hit = message_author_app_check_hit_test(g_app.message_scene, *renderer.message_text, *renderer.chat_state,
                                                       *renderer.user_store, renderer.message_measure_cache,
                                                       application_theme(), point.x, point.y);
    if (!hit.message_id || !ui_rect_has_area(hit.check))
        return false;
    *target = {{hit.owner, scope, hit.message_id, hit.revision, 0x5900u}, hit.check, kUiAppBadgeVerifiedHint};
    return true;
}
// The "(edited)" label after a message's text, in the timeline or a search result: when it was edited, in full.
char g_edited_label[kMessageMarkdownTimestampMaximumBytes];
bool edited_target(UiPoint point, u64 scope, UiTooltipTarget* target) noexcept {
    const auto& renderer = g_app.renderer;
    if (application_friends_active() || !g_app.account_ui)
        return false;
    const auto hit = renderer_edited_label_hit_test(renderer, point.x, point.y);
    if (!hit.valid)
        return false;
    UiWidgetReference owner{};
    if (hit.search) {
        owner = {&renderer.search_scene->widgets, renderer.search_scene->root};
    } else {
        const auto* record = application_message_record(hit.message_id);
        if (!record || record->revision != hit.message_revision ||
            record->channel_id != g_app.account_ui->active_channel_id)
            return false;
        owner = {&g_app.message_scene.widgets, g_app.message_scene.root};
    }
    const auto formatted = format_timestamp_local(hit.edited_unix_seconds, 'F', clock_unix_milliseconds() / 1'000u,
                                                  {g_edited_label, sizeof(g_edited_label)});
    if (!formatted.ok || !formatted.bytes)
        return false;
    *target = {{owner, scope, hit.message_id, hit.message_revision, 0x5a00u}, hit.rectangle,
               {g_edited_label, formatted.bytes}};
    return true;
}
// A known game's mention chip: "@game <name>" after 750 ms, as the official
// client's (an unknown game has none).
char g_game_label[6 + kGuildAdminGameNameBytes];
bool game_mention_target(UiPoint point, u64 scope, UiTooltipTarget* target, UiTooltipStyle* style) noexcept {
    const auto* text = g_app.renderer.content_text;
    if (application_friends_active() || !g_app.account_ui || !text || !g_app.renderer.game_store)
        return false;
    for (u32 i = text->content_hit_region_count; i; --i) {
        const TextHitRegion& region = text->content_hit_regions[i - 1];
        if (point.x < region.left || point.x >= region.right || point.y < region.top || point.y >= region.bottom ||
            region.semantic_index >= text->content_semantic_count)
            continue;
        const auto& semantic = text->content_semantics[region.semantic_index];
        if (semantic.kind != MessageMarkdownSemanticKind::game_mention ||
            (semantic.reserved & kRendererSearchSemanticFlag))
            continue;
        const Utf8View name = game_store_name(*g_app.renderer.game_store, semantic.target);
        if (!name.bytes)
            return false;
        copy_bytes(g_game_label, "@game ", 6);
        copy_bytes(g_game_label + 6, name.data, name.bytes);
        *target = {{{&g_app.message_scene.widgets, g_app.message_scene.root}, scope, semantic.message_id,
                    semantic.message_revision, 0x5b00u ^ static_cast<u32>(semantic.target)},
                   {region.left, region.top, region.right, region.bottom},
                   {g_game_label, 6 + name.bytes}};
        style->pointer_delay_ms = 750;
        return true;
    }
    return false;
}
// An attachment's "ALT" badge: clicking it pins its description under "Image Description (Alt Text)", as the official
// client's popout; any press (another click on the badge included) or Escape closes it.
struct AltCard {
    bool open;
    u64 message_id, target, closed_by_press;
    u32 revision;
};
AltCard g_alt;
char g_alt_label[32 + 2048];
// The badge's hit region under `point` (8 DIPs of slack, the pill around the label), or the open card's badge when
// `point` is null.
const TextHitRegion* alt_region(const UiPoint* point, const MessageTextSemantic** found) noexcept {
    const auto* text = g_app.renderer.content_text;
    if (application_friends_active() || !g_app.account_ui || !text)
        return nullptr;
    for (u32 i = text->content_hit_region_count; i; --i) {
        const TextHitRegion& region = text->content_hit_regions[i - 1];
        if (region.semantic_index >= text->content_semantic_count)
            continue;
        const auto& semantic = text->content_semantics[region.semantic_index];
        if (semantic.kind != MessageMarkdownSemanticKind::alt_text || (semantic.reserved & kRendererSearchSemanticFlag))
            continue;
        if (point ? point->x < region.left - 8 || point->x >= region.right + 8 || point->y < region.top - 8 ||
                        point->y >= region.bottom + 8
                  : semantic.message_id != g_alt.message_id || semantic.target != g_alt.target)
            continue;
        *found = &semantic;
        return &region;
    }
    return nullptr;
}
// The open card's text: false when its attachment or description is gone.
bool alt_text(const MessageTextSemantic& semantic, Utf8View* label) noexcept {
    const auto& store = g_app.account_ui->message_rich_store;
    const bool snapshot = (semantic.target & kMessageAltSnapshot) != 0;
    const u64 index = semantic.target & ~kMessageAltSnapshot;
    const auto* rich = snapshot ? message_rich_store_snapshot(store, semantic.message_id)
                                : message_rich_store_find(store, semantic.message_id);
    if (!rich || rich->message_revision != semantic.message_revision || index >= rich->attachment_count)
        return false;
    const Utf8View description = message_rich_store_string(store, rich->attachments[index].description);
    constexpr Utf8View title = utf8_literal("Image Description (Alt Text)\n");
    if (!description.bytes)
        return false;
    copy_bytes(g_alt_label, title.data, title.bytes);
    *label = {g_alt_label, title.bytes + utf8_copy_truncated(g_alt_label + title.bytes, sizeof(g_alt_label) - title.bytes,
                                                            description)};
    return true;
}
void alt_close() noexcept {
    if (!g_alt.open)
        return;
    g_alt.open = false;
    application_request_frame();
}
bool alt_card_target(u64 scope, UiTooltipTarget* target, UiTooltipStyle* style) noexcept {
    if (!g_alt.open)
        return false;
    const MessageTextSemantic* semantic = nullptr;
    const auto* region = alt_region(nullptr, &semantic);
    Utf8View label{};
    if (!region || !alt_text(*semantic, &label)) {
        alt_close();
        return false;
    }
    *target = {{{&g_app.message_scene.widgets, g_app.message_scene.root}, scope, semantic->message_id,
                g_alt.revision, 0x5c00u},
               {region->left - 8, region->top - 8, region->right + 8, region->bottom + 8},
               label,
               UiTooltipPlacement::above};
    const auto& scale = application_theme().scale;
    style->pointer_delay_ms = 0;
    style->padding_x = scale.space_4 + scale.space_2;
    style->padding_y = scale.space_4;
    style->maximum_width = 360;
    style->maximum_height = 240;
    return true;
}
// The same check on a member row's APP tag.
bool member_app_check_target(UiPoint point, u64 scope, UiTooltipTarget* target) noexcept {
    const auto& s = g_app.member_list_scene;
    u64 user = 0;
    UiWidgetHandle row{};
    const auto check = s.active ? member_list_app_check_hit(s, point.x, point.y, &user, &row) : UiRect{};
    if (!user || !ui_rect_has_area(check))
        return false;
    *target = {{{&s.widgets, row}, scope, user, 0, 0x5901u}, check, kUiAppBadgeVerifiedHint};
    return true;
}
bool search_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto* scene = g_app.renderer.search_scene;
    if (!scene || !scene->active)
        return false;
    const auto& manager = scene->widgets;
    const auto handle = keyboard ? manager.focused : ui_widget_manager_hit_test(manager, point.x, point.y);
    if (!handle.generation || handle.slot >= 48)
        return false;
    const auto source = scene->targets[handle.slot];
    const Utf8View label = source.action == UiSearchAction::scope
                ? utf8_literal("Search the whole server")
                : name_list_at(kSearchLabels, static_cast<u32>(source.action) - static_cast<u32>(UiSearchAction::edit));
    if (!label.bytes)
        return false;
    return widget_target(manager, handle, scope,
                         source.message_id ? source.message_id : static_cast<u64>(source.action),
                         scene->source_revision, 0x400u + static_cast<u32>(source.action), label, target);
}
bool pinned_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto* scene = g_app.renderer.pinned_messages_scene;
    if (!scene || !ui_pinned_messages_scene_current(*scene)) return false;
    const auto handle = keyboard ? scene->widgets.focused
                                : ui_widget_manager_hit_test(scene->widgets, point.x, point.y);
    const auto source = ui_pinned_messages_scene_target(*scene, handle);
    const Utf8View label = name_list_at(kPinnedLabels, static_cast<u32>(source.action) - static_cast<u32>(UiPinnedMessagesAction::close));
    if (!label.bytes)
        return false;
    return widget_target(scene->widgets, handle, scope,
                         source.message_id ? source.message_id : source.channel_id,
                         scene->source_revision, 0x900u + static_cast<u32>(source.action), label, target);
}
bool browser_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto* scene = application_thread_browser_scene();
    if (!scene || !ui_thread_browser_scene_current(*scene)) return false;
    const auto handle = keyboard ? scene->widgets.focused
                                : ui_widget_manager_hit_test(scene->widgets, point.x, point.y);
    const auto source = ui_thread_browser_scene_target(*scene, handle);
    if (!ui_thread_browser_scene_target_current(*scene, source)) return false;
    const Utf8View label = source.action == UiThreadBrowserAction::open_thread
                ? (scene->forum ? utf8_literal("Open post") : utf8_literal("Open thread"))
            : source.action >= UiThreadBrowserAction::forum_sort
                ? name_list_at(kForumLabels, static_cast<u32>(source.action) - static_cast<u32>(UiThreadBrowserAction::forum_sort))
                : name_list_at(kBrowserLabels, static_cast<u32>(source.action) - static_cast<u32>(UiThreadBrowserAction::close));
    if (!label.bytes)
        return false;
    return widget_target(scene->widgets, handle, scope,
                         source.thread_id ? source.thread_id : static_cast<u64>(source.action),
                         source.source_publication, 0xb00u + static_cast<u32>(source.action), label, target);
}
bool gallery_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto* scene = g_app.renderer.media_picker_scene;
    // Inline /gif and /sticker results carry no tooltips.
    if (!scene || !ui_media_picker_scene_current(*scene) || scene->variant == UiMediaPickerVariant::inline_results)
        return false;
    const auto handle =
        keyboard ? scene->widgets.focused : ui_widget_manager_hit_test(scene->widgets, point.x, point.y);
    if (!handle.generation || handle.slot >= kUiMediaPickerWidgetCapacity)
        return false;
    const auto source = scene->projection->targets[handle.slot];
    Utf8View label{};
    auto action = source.action;
    UiRect favorite_anchor{};
    switch (source.action) {
    case UiMediaPickerAction::edit:
        label = scene->content->placeholder;
        break;
    case UiMediaPickerAction::back:
        label = utf8_literal("Back to GIF categories");
        break;
    case UiMediaPickerAction::tab:
        label = source.tab == UiMediaPickerKind::gif       ? utf8_literal("GIFs")
                : source.tab == UiMediaPickerKind::sticker ? utf8_literal("Stickers")
                                                           : utf8_literal("Emoji");
        break;
    case UiMediaPickerAction::item:
    case UiMediaPickerAction::category:
        for (u32 i = 0; i < scene->cell_count; ++i) {
            const auto& cell = scene->projection->cells[i];
            if (cell.widget.slot == handle.slot) {
                if (!keyboard && ui_media_picker_scene_favorite(*scene, i, &favorite_anchor) &&
                    ui_rect_contains(favorite_anchor, point.x, point.y)) {
                    action = UiMediaPickerAction::favorite;
                    label = cell.item.favorite_pending ? utf8_literal("Updating favorite")
                            : cell.item.favorite_failed
                                ? utf8_literal("Couldn't update favorite. Try again (F)")
                            : cell.item.favorite ? utf8_literal("Remove from favorites (F)")
                                                 : utf8_literal("Add to favorites (F)");
                } else
                    label = cell.item.detail.bytes ? cell.item.detail : cell.item.label;
                break;
            }
        }
        break;
    case UiMediaPickerAction::section:
    case UiMediaPickerAction::section_jump:
        if (source.section_index < scene->source->sections.count)
            label = scene->source->sections[source.section_index].label;
        break;
    default:
        return false;
    }
    if (!label.bytes ||
        !widget_target(scene->widgets, handle, scope, source.source_index + 1, scene->source_revision,
                       0x600u + static_cast<u32>(action), label, target))
        return false;
    if (action == UiMediaPickerAction::favorite)
        target->anchor = favorite_anchor;
    return true;
}
bool theme_editor_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto* scene = g_app.renderer.theme_editor_scene;
    if (!scene || !renderer_theme_editor_visible(g_app.renderer))
        return false;
    const auto handle =
        keyboard ? scene->widgets.focused : ui_widget_manager_hit_test(scene->widgets, point.x, point.y);
    for (u32 i = 0; i < kUiThemeEditorElements; ++i) {
        if (!ui_widget_handle_equal(handle, scene->elements[i]))
            continue;
        const auto element = static_cast<UiThemeEditorElement>(i);
        Utf8View label{};
        switch (element) {
        case UiThemeEditorElement::close:
            label = utf8_literal("Back to settings");
            break;
        case UiThemeEditorElement::dark:
            label = utf8_literal("Dark appearance");
            break;
        case UiThemeEditorElement::light:
            label = utf8_literal("Light appearance");
            break;
        case UiThemeEditorElement::add:
            label = utf8_literal("Add a gradient color");
            break;
        case UiThemeEditorElement::remove:
            label = utf8_literal("Remove selected color");
            break;
        case UiThemeEditorElement::hex:
            label = utf8_literal("Edit color value");
            break;
        case UiThemeEditorElement::surprise:
            label = utf8_literal("Create a coordinated palette");
            break;
        case UiThemeEditorElement::reset:
            label = utf8_literal("Reset your gradient");
            break;
        default:
            return false;
        }
        return widget_target(scene->widgets, handle, scope, g_app.theme_editor_lifetime_serial,
                             scene->generation, 0x700u + i, label, target);
    }
    return false;
}
// Plugin badges on channel/DM rows and server icons, account panel buttons.
bool plugin_surface_target(UiPoint point, u64 scope, UiTooltipTarget* target) noexcept {
    ApplicationPluginSurfaceHit hit{};
    if (!application_plugins_surface_hit(point.x, point.y, &hit) || !hit.label.bytes || !hit.manager ||
        !ui_rect_has_area(hit.anchor))
        return false;
    const u64 identity = hit.target.channel_id ? hit.target.channel_id : hit.target.guild_id;
    *target = {{{hit.manager, hit.owner}, scope, identity, g_plugin_surface_revision,
                0x5700u + hit.target.surface * 4 + hit.index},
               hit.anchor,
               hit.label};
    if (hit.target.surface == kUiPluginSlotGuildRail || hit.target.surface == kUiPluginSlotServerTop ||
        hit.target.surface == kUiPluginSlotServerBottom)
        target->placement = UiTooltipPlacement::right;
    return true;
}
bool viewer_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto& scene = g_app.image_viewer.scene;
    const auto handle = !keyboard                ? ui_widget_manager_hit_test(scene.widgets, point.x, point.y)
                        : scene.keyboard_focus ? scene.widgets.focused
                                               : ui_widget_invalid_handle();
    const auto* widget = ui_widget_manager_resolve(scene.widgets, handle);
    if (!widget || widget->kind != UiWidgetKind::button || widget->id < 10)
        return false;
    const u32 index = widget->id - 10;
    return widget_target(scene.widgets, handle, scope, g_app.image_viewer.reveal_generation, 0, index,
                         ui_image_viewer_tooltip(scene, index), target);
}
// The profile card covers the content; only its own hints show (activity
// artwork text, cut-off activity lines, a disabled join button's reason).
bool profile_available() noexcept {
    const auto* scene = g_app.renderer.profile_scene;
    return g_app.renderer_ready && scene && scene->active && !g_app.popup_scene.active && !g_app.text_form &&
           !application_window_minimized();
}
// Profile badges show after Discord's 300 ms, and at once when moving
// from one badge to the next.
constexpr u32 kProfileBadgeTooltip = 0x5801u;
constexpr u32 kProfileBadgeDelayMs = 300;
// `plain_action` is the tooltip action of every hint except a badge's.
bool profile_hint_target(const ProfileScene& scene, UiPoint point, bool keyboard, u64 scope, u32 plain_action,
                         UiTooltipTarget* target) noexcept {
    const auto handle = keyboard ? scene.widgets.focused : profile_scene_hit_test(scene, point.x, point.y);
    UiRect anchor{};
    const Utf8View label = profile_scene_hint(scene, handle, &anchor);
    if (!label.bytes)
        return false;
    const auto* part = profile_scene_part(scene, handle);
    const u32 action = part && part->media == ProfileMediaKind::badge ? kProfileBadgeTooltip : plain_action;
    *target = {{{&scene.widgets, handle}, scope, scene.opens, handle.slot, action}, anchor, label};
    return true;
}
// A command picker rail entry under the pointer, or the one the keyboard
// moved into (the selected entry while it is a rail entry).
bool command_rail_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto& scene = g_app.popup_scene;
    const u32 index = keyboard ? scene.selected_index : popup_scene_hit_test(scene, point.x, point.y).source_index;
    if (index >= scene.item_count || !(scene.items[index].flags & popup_item_command_category))
        return false;
    const auto& item = scene.items[index];
    if (!widget_target(scene.widgets, scene.rows[index].widget, scope, item.action, scene.owner_id, 0xc0d0u,
                       item.label, target))
        return false;
    target->placement = UiTooltipPlacement::right;
    return true;
}
// The settings modal's close button names its key.
bool settings_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto* scene = application_settings_scene();
    if (!scene || g_app.text_form || g_app.popup_scene.active || renderer_theme_editor_visible(g_app.renderer))
        return false;
    const auto hit = keyboard ? ui_settings_scene_focused(*scene) : ui_settings_scene_hit_test(*scene, point.x, point.y);
    return hit.kind == UiSettingsTargetKind::close &&
           widget_target(scene->widgets, scene->close, scope, 1, scene->revision, 0x5900u,
                         utf8_literal("Close \xC2\xB7 Esc"), target);
}
// The server tag popout's tooltips: the server badge's card (its kind in bold, then who can join and its boosts),
// "+ N more" (the folded traits as chips), a game icon's name and "+N"'s other games beside their pictures, below
// their sources but the chips.
bool guild_card_target(UiPoint point, u64 scope, UiTooltipTarget* target, UiTooltipStyle* style) noexcept {
    const auto* card = g_app.renderer.guild_card_scene;
    UiGuildCardHint hint{};
    if (!card || !ui_guild_card_hint(*card, point.x, point.y, &hint))
        return false;
    const u32 action = 0x4743'0000u | static_cast<u32>(hint.kind) << 8 | hint.index;
    *target = {{card->owner, scope, card->identity, card->revision, action}, hint.anchor, hint.text};
    target->placement =
        hint.kind == UiGuildCardHintKind::traits ? UiTooltipPlacement::above : UiTooltipPlacement::below;
    target->layout = hint.kind == UiGuildCardHintKind::traits         ? UiTooltipLayout::chips
                     : hint.kind == UiGuildCardHintKind::server_badge ? UiTooltipLayout::card
                     : hint.kind == UiGuildCardHintKind::games        ? UiTooltipLayout::rows
                                                                      : UiTooltipLayout::label;
    target->row_media = hint.media;
    // A card's lines, rows of chips or of games.
    style->maximum_height = 240;
    return true;
}
// The resolver chooses the style; reactions also report themselves so their
// reactor fetch keeps its interest.
bool resolve_target(UiPoint point, bool keyboard, UiTooltipTarget* target, UiTooltipStyle* style,
                    bool* reaction) noexcept {
    const u64 scope = application_account_scope();
    if (g_app.image_viewer.scene.active)
        return viewer_available() && !window_pointer_captured(g_app.window) && scope &&
               (!keyboard || g_app.keyboard_focus_visible || g_app.image_viewer.scene.keyboard_focus) &&
               viewer_target(point, keyboard, scope, target);
    if (g_app.renderer.profile_scene && g_app.renderer.profile_scene->active)
        return profile_available() && !window_pointer_captured(g_app.window) && scope &&
               (!keyboard || g_app.keyboard_focus_visible) &&
               profile_hint_target(*g_app.renderer.profile_scene, point, keyboard, scope, 0x5800u, target);
    // The command picker's rail entries name their sections at the right.
    if (g_app.popup_scene.active && g_app.popup_owner_kind == PopupOwnerKind::commands)
        return g_app.renderer_ready && scope && !window_pointer_captured(g_app.window) && !application_window_minimized() &&
               command_rail_target(point, keyboard, scope, target);
    if (application_settings_scene() && !renderer_theme_editor_visible(g_app.renderer))
        return g_app.renderer_ready && g_app.account_ui && !g_app.login.visible && scope && !window_pointer_captured(g_app.window) &&
               !application_window_minimized() && (!keyboard || g_app.keyboard_focus_visible) &&
               settings_target(point, keyboard, scope, target);
    // Check current host ownership before touching any old scene descriptor.
    if (!content_available() || window_pointer_captured(g_app.window))
        return false;
    if (!scope)
        return false;
    if (renderer_theme_editor_visible(g_app.renderer))
        return (!keyboard || g_app.keyboard_focus_visible) &&
               theme_editor_target(point, keyboard, scope, target);
    if (keyboard) {
        if (!g_app.keyboard_focus_visible)
            return false;
        if (application_friends_active())
            return friends_target(point, true, scope, target);
        if (g_app.renderer.media_picker_scene &&
            g_app.context_focus == &g_app.renderer.media_picker_scene->widgets)
            return gallery_target(point, true, scope, target);
        if (const auto* browser = application_thread_browser_scene();
            browser && g_app.context_focus == &browser->widgets)
            return browser_target(point, true, scope, target);
        if (g_app.renderer.search_scene && g_app.context_focus == &g_app.renderer.search_scene->widgets)
            return search_target(point, true, scope, target);
        if (g_app.renderer.pinned_messages_scene &&
            g_app.context_focus == &g_app.renderer.pinned_messages_scene->widgets)
            return pinned_target(point, true, scope, target);
        if (!application_thread_browser_read_blocked() &&
            application_video_tooltip_target(point, true, scope, target))
            return true;
        if (g_app.context_focus == &g_app.shell_scene.widgets)
            return shell_target(point, true, scope, target);
        if (g_app.context_focus == &g_app.guild_rail_scene.widgets)
            return guild_target(point, true, scope, target);
        if (g_app.context_focus == &g_app.message_composer_scene.widgets)
            return composer_target(point, true, scope, target);
        return false;
    }
    if (const auto* card = g_app.renderer.guild_card_scene; card && ui_guild_card_contains(*card, point.x, point.y))
        return guild_card_target(point, scope, target, style);
    if (g_app.renderer.media_picker_scene &&
        ui_rect_contains(g_app.renderer.media_picker_scene->panel, point.x, point.y))
        return gallery_target(point, false, scope, target);
    if (const auto* call = g_app.renderer.call_scene; call && ui_call_scene_contains(*call, point.x, point.y))
        return call_target(point, scope, target);
    if (friends_target(point, false, scope, target))
        return true;
    if (const auto* browser = application_thread_browser_scene();
        browser && ui_thread_browser_scene_contains(*browser, point.x, point.y))
        return browser_target(point, false, scope, target);
    if (g_app.renderer.search_scene &&
        ui_search_scene_contains(*g_app.renderer.search_scene, point.x, point.y))
        return edited_target(point, scope, target) || search_target(point, false, scope, target);
    if (g_app.renderer.pinned_messages_scene &&
        ui_pinned_messages_scene_contains(*g_app.renderer.pinned_messages_scene, point.x, point.y))
        return pinned_target(point, false, scope, target);
    // The DM profile panel's hints (badges, cut-off activity lines).
    if (const auto* panel = g_app.renderer.dm_profile_panel;
        panel && panel->active && ui_rect_contains(panel->rectangle, point.x, point.y))
        return profile_hint_target(*panel, point, false, scope, 0x5802u, target);
    if (!application_thread_browser_read_blocked()) {
        if (application_video_tooltip_target(point, false, scope, target) || message_target(point, scope, target) ||
            app_check_target(point, scope, target) || member_app_check_target(point, scope, target) ||
            edited_target(point, scope, target) || game_mention_target(point, scope, target, style))
            return true;
        if (!application_friends_active() && application_reaction_tooltip_target(point, scope, target)) {
            // Reaction pills lead with their emoji; their names wrap beside it.
            *reaction = media_asset_key_valid(target->emoji.media) || target->emoji.glyph.bytes;
            if (*reaction) {
                style->maximum_width = 260;
                style->maximum_height = 120;
            }
            return true;
        }
        if (!application_friends_active() && application_emoji_tooltip_target(point, scope, target, style))
            return true;
    }
    return plugin_surface_target(point, scope, target) || composer_target(point, false, scope, target) ||
           guild_target(point, false, scope, target) || shell_target(point, false, scope, target);
}
ApplicationTooltip* ensure() noexcept {
    if (g_app.tooltip)
        return g_app.tooltip;
    auto* owner = commit_pages<ApplicationTooltip>();
    if (!owner)
        return nullptr;
    UiWidgetReference root{};
    if (!ui_widget_manager_initialize_fill_root(&owner->widgets, {&owner->root, 1}, {&owner->root_layout, 1},
                                                utf8_literal("Tooltip overlay"),
                                                ui_layout_node_visible | ui_layout_node_clip_children, &root) ||
        !ui_tooltip_bind_text_owner(&owner->scene, root)) {
        pages_release(owner);
        return nullptr;
    }
    g_app.tooltip = owner;
    g_app.renderer.tooltip_scene = &owner->scene;
    return owner;
}
bool refresh(ApplicationTooltip& owner, u64 now_ms) noexcept {
    const bool old_visible = owner.published_visible;
    const u32 old_revision = owner.published_revision;
    const auto viewport = application_viewport_dips();
    if (owner.viewport_failed && ui_rect_equal(viewport, owner.viewport))
        return true;
    if (!ui_rect_equal(viewport, owner.viewport)) {
        const bool observed = ui_rect_has_area(owner.viewport);
        // A resize invalidates the old pointer/focus observation and its delay.
        // A fresh native input event may offer the newly laid-out source.
        if (observed) {
            ui_tooltip_clear(&owner.scene);
            owner.pointer_valid = owner.keyboard = false;
            owner.dismissed = owner.failed = {};
        }
        if (!ui_widget_manager_layout_rectangle(&owner.widgets, viewport).ok()) {
            ui_tooltip_clear(&owner.scene);
            owner.viewport = viewport;
            owner.viewport_failed = true;
            owner.pointer_valid = owner.keyboard = false;
            owner.published_visible = false;
            owner.published_revision = owner.scene.revision;
            application_repaint();
            return false;
        }
        owner.viewport = viewport;
        owner.viewport_failed = false;
    }
    UiTooltipTarget target{};
    UiTooltipStyle style{};
    bool reaction = false;
    // The command card and the ALT popout are pinned: they stay up while the pointer moves away.
    const bool pinned = content_available() &&
                        (application_interaction_card_target(application_account_scope(), &target, &style) ||
                         alt_card_target(application_account_scope(), &target, &style));
    const bool resolved = pinned || (!owner.pressed && (owner.keyboard || owner.pointer_valid) &&
                                     resolve_target(owner.point, owner.keyboard, &target, &style, &reaction));
    const bool badge_shown = owner.scene.visible && owner.scene.key.action == kProfileBadgeTooltip;
    if (!reaction)
        application_reaction_tooltip_leave();
    if (!resolved || (!pinned && (ui_tooltip_key_equal(owner.dismissed, target.key) ||
        (ui_tooltip_key_equal(owner.failed, target.key) &&
         ui_rect_equal(owner.failed_anchor, target.anchor))))) {
        ui_tooltip_clear(&owner.scene);
    } else {
        reset_value(&owner.dismissed);
        reset_value(&owner.failed);
        if (target.key.action == kProfileBadgeTooltip) {
            if ((owner.scene.pending || owner.scene.visible) && ui_tooltip_key_equal(owner.scene.key, target.key))
                style = owner.scene.style;
            else
                style.pointer_delay_ms = badge_shown || now_ms < owner.badge_warm_until_ms ? 0 : kProfileBadgeDelayMs;
        }
        ui_tooltip_offer(&owner.scene, target, viewport,
                         owner.keyboard ? UiTooltipTrigger::keyboard : UiTooltipTrigger::pointer, now_ms, style);
        const bool due = owner.scene.pending && now_ms >= owner.scene.show_at_ms;
        ui_tooltip_tick(&owner.scene, now_ms,
                        g_app.account_ui ? &g_app.account_ui->text_measure_cache : nullptr);
        if (due && !owner.scene.pending && !owner.scene.visible)
            owner.dismissed = target.key;
    }
    if (old_visible != owner.scene.visible || old_revision != owner.scene.revision) {
        g_app.renderer.retained_presentation.valid = false;
        if (!renderer_rebuild_content_text(&g_app.renderer, application_theme())) {
            // Failed text publication must not remain a dirty visible revision
            // that requests and retries another frame forever. Suppress the exact
            // failed source until its identity/geometry changes or explicit clear.
            owner.failed = target.key;
            owner.failed_anchor = target.anchor;
            ui_tooltip_clear(&owner.scene);
            owner.published_visible = false;
            owner.published_revision = owner.scene.revision;
            application_request_frame();
            return false;
        }
        application_request_frame();
    }
    // A newly shown tooltip pops in on its spring: opacity and scale(.95) drawn by the renderer's form layer.
    if (owner.scene.visible) {
        if (!old_visible || !ui_tooltip_key_equal(owner.entrance_key, owner.scene.key)) {
            owner.entrance_key = owner.scene.key;
            ui_motion_transition_reset(&owner.entrance, {kTooltipMotionOwner, ++owner.entrance_generation}, 0);
        }
        const float shown =
            application_motion_tick(&owner.entrance, owner.entrance.scope, 1, now_ms, kUiMotionTooltip);
        if (renderer_set_tooltip_entrance(&g_app.renderer, 1 - shown))
            application_request_frame();
    } else {
        renderer_set_tooltip_entrance(&g_app.renderer, 0);
    }
    if (badge_shown && !(owner.scene.visible && owner.scene.key.action == kProfileBadgeTooltip))
        owner.badge_warm_until_ms = now_ms + kProfileBadgeDelayMs;
    owner.published_visible = owner.scene.visible;
    owner.published_revision = owner.scene.revision;
    application_reaction_tooltip_pump();
    return true;
}
} // namespace
bool application_alt_text_toggle(float x, float y) noexcept {
    const UiPoint point{x, y};
    const MessageTextSemantic* semantic = nullptr;
    if (!alt_region(&point, &semantic))
        return false;
    // The press of this very click closed this badge's popout.
    const bool same = g_alt.closed_by_press == semantic->message_id && g_alt.target == semantic->target;
    g_alt.closed_by_press = 0;
    Utf8View label{};
    if (same || !alt_text(*semantic, &label))
        return true;
    g_alt = {true, semantic->message_id, semantic->target, 0, g_alt.revision + 1};
    if (ensure())
        application_request_frame();
    return true;
}
void application_tooltip_clear() noexcept {
    application_reaction_tooltip_leave();
    if (!g_app.tooltip)
        return;
    auto& owner = *g_app.tooltip;
    const bool visible = owner.scene.visible;
    ui_tooltip_clear(&owner.scene);
    owner.pointer_valid = owner.keyboard = owner.pressed = false;
    reset_value(&owner.dismissed);
    reset_value(&owner.failed);
    if (visible) {
        application_repaint();
    }
}
void application_tooltip_wake() noexcept {
    if (ensure())
        application_request_frame();
}
void application_tooltip_release() noexcept {
    application_tooltip_clear();
    auto* owner = g_app.tooltip;
    g_app.tooltip = nullptr;
    g_app.renderer.tooltip_scene = nullptr;
    if (owner)
        pages_release(owner);
}
u64 application_tooltip_next_wake() noexcept {
    return earliest_deadline(g_app.tooltip ? ui_tooltip_next_wake(g_app.tooltip->scene) : 0,
                             application_caption_tooltip_wake());
}
bool application_tooltip_synchronize(u64 now_ms) noexcept {
    return !g_app.tooltip || refresh(*g_app.tooltip, now_ms);
}
void application_tooltip_input(const WindowEvent& event) noexcept {
    switch (event.type) {
    case EventType::pointer_leave:
    case EventType::capture_lost:
    case EventType::cancel_input:
    case EventType::resize:
    case EventType::scale_changed:
    case EventType::resize_begin:
    case EventType::resize_end:
        application_tooltip_clear();
        return;
    case EventType::focus_changed:
        if (!event.focus.gained) {
            application_tooltip_clear();
            return;
        }
        break;
    default:
        break;
    }
    const bool pointer = input_move(event);
    const bool keyboard = input_key_down(event);
    const bool press = ((input_press(event, PointerButton::left) || input_press(event, PointerButton::right) ||
                         input_press(event, PointerButton::middle))) ||
                       event.type == EventType::wheel;
    if (!pointer && !keyboard && !press)
        return;
    if (press) {
        application_reaction_tooltip_leave();
        application_interaction_card_press();
        g_alt.closed_by_press = g_alt.open ? g_alt.message_id : 0;
        alt_close();
        if (g_app.tooltip) {
            g_app.tooltip->pressed = true;
            ui_tooltip_clear(&g_app.tooltip->scene);
            application_request_frame();
        }
        return;
    }
    const float scale = 96.f / maximum(g_app.renderer.dpi, 1u);
    const UiPoint point = pointer ? UiPoint{event.pointer.x * scale, event.pointer.y * scale} : UiPoint{};
    UiTooltipTarget candidate{};
    UiTooltipStyle candidate_style{};
    bool candidate_reaction = false;
    const u32 code = keyboard ? event.key.key : 0;
    const bool navigation =
        keyboard && (code == key::tab || code == key::up || code == key::down || code == key::left ||
                     code == key::right || code == key::home || code == key::end);
    // Retain the first eligible pointer observation even when dispatch has not
    // published its action bar/video control yet. Synchronize resolves the new
    // typed source after dispatch without requiring a second mouse movement.
    const bool available = content_available() || viewer_available() || profile_available() ||
                           (application_settings_scene() && g_app.renderer_ready && !g_app.login.visible);
    const bool first_pointer = pointer && available;
    if (!g_app.tooltip && !first_pointer &&
        !resolve_target(point, keyboard, &candidate, &candidate_style, &candidate_reaction) &&
        !(navigation && available))
        return;
    auto* owner = ensure();
    if (!owner)
        return;
    owner->keyboard = keyboard;
    owner->pressed = false;
    if (pointer) {
        owner->point = point;
        owner->pointer_valid = true;
        reset_value(&owner->dismissed);
    }
    if (keyboard && (code == key::enter || code == key::space || code == key::escape)) {
        application_reaction_tooltip_leave();
        if (code == key::escape)
            application_interaction_card_close();
        if (code == key::escape)
            alt_close();
        owner->dismissed = owner->scene.key;
        ui_tooltip_clear(&owner->scene);
        application_request_frame();
    }
    refresh(*owner, clock_monotonic_milliseconds());
}
} // namespace maicord::app_detail
