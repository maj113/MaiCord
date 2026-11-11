#include "accessibility_internal.hpp"
#include "../chat/private_header.hpp"
#include "../shell/caption.hpp"
#include "../shell/notice.hpp"
#include "../account/friends.hpp"
#include "../guild/folders.hpp"
#include "../voice/channels.hpp"
#include "../voice/voice.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/voice/channel_info_store.hpp"
#include "maicord/model/chat/game_store.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/store/store_scene.hpp"
#include "maicord/ui/account/login_text_presentation.hpp"
#include "../account/login/ui.hpp"

namespace maicord::app_detail {
namespace {

static_assert(kKeyIndexCapacity >= kNodeCapacity * 2);

} // namespace
namespace accessibility_detail {

AccessibilityRuntime g_access;

NameBuilder g_name;

float access_scale(void*) noexcept {
    return static_cast<float>(g_app.renderer.dpi ? g_app.renderer.dpi : 96u) / 96.0f;
}

UiPoint center(const UiRect& rectangle) noexcept {
    return {(rectangle.left + rectangle.right) * 0.5f, (rectangle.top + rectangle.bottom) * 0.5f};
}

const ChatStateStore* chat_state() noexcept {
    return g_app.account_ui ? &g_app.account_ui->chat_state : nullptr;
}

Utf8View channel_name(u64 channel_id) noexcept {
    const auto* state = chat_state();
    const auto* channel = state ? chat_state_store_find_channel(*state, channel_id) : nullptr;
    return channel ? chat_state_store_channel_name(*state, *channel) : Utf8View{};
}

} // namespace accessibility_detail
namespace {

void add_unread(NameBuilder* name, bool unread, u32 mentions) noexcept {
    if (mentions)
        name->add(", ").number(mentions).add(mentions == 1 ? utf8_literal(" mention")
                                                                          : utf8_literal(" mentions"));
    else if (unread)
        name->add(", unread");
}

UiAccessScrollState scroll_state(float offset, float content, float viewport) noexcept {
    if (content <= viewport + 0.5f || viewport <= 0)
        return {-1.0f, 100.0f};
    const float range = content - viewport;
    return {minimum(100.0f, maximum(0.0f, offset * 100.0f / range)), viewport * 100.0f / content};
}

void describe_guild(void*, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&, const UiLayoutNode&,
                    UiAccessWidgetInfo* info) noexcept {
    const auto& scene = g_app.guild_rail_scene;
    if (ui_widget_handle_equal(handle, scene.root)) {
        info->role = UiAccessRole::list;
        info->name = utf8_literal("Servers");
        info->actions = ui_access_action_scroll;
        info->scroll = scroll_state(scene.scroll_dips, scene.content_height, ui_rect_height(scene.viewport));
        return;
    }
    if (ui_widget_handle_equal(handle, scene.add_button)) {
        info->leaf = true;
        info->role = UiAccessRole::button;
        info->name = utf8_literal("Add a server");
        info->actions = ui_access_action_invoke;
        return;
    }
    const auto* row = guild_rail_scene_resolve(scene, handle);
    if (!row)
        return;
    const auto& view = row->view;
    info->leaf = true;
    info->key = view.id;
    info->item = view.id;
    info->role = UiAccessRole::list_item;
    info->state |= ui_access_selectable | ui_access_focusable;
    if (view.selected)
        info->state |= ui_access_selected;
    if (g_app.context_focus == &scene.widgets && scene.focused_guild_id == view.id)
        info->state |= ui_access_focused;
    g_name.bytes = 0;
    if (view.folder_role == GuildFolderRole::header) {
        g_name.add("Server folder, ").number(view.folder_size).add(" servers");
        info->state |= view.folder_expanded ? ui_access_expanded : ui_access_collapsed;
        info->actions = ui_access_action_invoke | ui_access_action_expand | ui_access_action_focus;
    } else if (view.folder_role == GuildFolderRole::conversation) {
        char title[128];
        const Utf8View label = application_guild_rail_label(view, {title, sizeof(title)});
        g_name.add(label.bytes ? label : utf8_literal("Direct message"));
        add_unread(&g_name, view.unread, view.mention_count);
        info->actions = ui_access_action_invoke | ui_access_action_focus;
    } else {
        const auto* state = chat_state();
        const auto* guild = state ? chat_state_store_find_guild(*state, view.id) : nullptr;
        g_name.add(guild ? chat_state_store_guild_name(*state, *guild) : utf8_literal("Server"));
        if (view.unavailable)
            g_name.add(", unavailable");
        add_unread(&g_name, view.unread, view.mention_count);
        info->actions = ui_access_action_select | ui_access_action_invoke | ui_access_action_focus;
    }
    info->name = g_name.view();
}

Utf8View channel_kind(u8 type) noexcept {
    switch (type) {
    case 0: return utf8_literal("text channel");
    case 2: return utf8_literal("voice channel");
    case 4: return utf8_literal("category");
    case 5: return utf8_literal("announcement channel");
    case 10:
    case 11:
    case 12: return utf8_literal("thread");
    case 13: return utf8_literal("stage channel");
    case 15: return utf8_literal("forum channel");
    case 16: return utf8_literal("media channel");
    default: return {};
    }
}

void describe_channel(void*, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&, const UiLayoutNode&,
                      UiAccessWidgetInfo* info) noexcept {
    const auto& scene = g_app.shell_navigation_scene;
    if (ui_widget_handle_equal(handle, scene.root)) {
        info->role = UiAccessRole::list;
        info->name = utf8_literal("Channels");
        info->actions = ui_access_action_scroll;
        info->scroll = scroll_state(scene.scroll_dips, scene.content_height, ui_rect_height(scene.viewport));
        return;
    }
    const auto* row = shell_navigation_scene_resolve(scene, handle);
    if (!row)
        return;
    const auto& view = row->view;
    info->leaf = true;
    info->key = view.id;
    info->item = view.id;
    info->role = UiAccessRole::list_item;
    info->state |= ui_access_focusable;
    if (g_app.context_focus == &scene.widgets && scene.focused_channel_id == view.id)
        info->state |= ui_access_focused;
    const Utf8View name = channel_name(view.id);
    if (view.type == 4) {
        info->state |= view.collapsed ? ui_access_collapsed : ui_access_expanded;
        info->actions = ui_access_action_invoke | ui_access_action_expand | ui_access_action_focus;
    } else {
        if (view.selectable)
            info->state |= ui_access_selectable;
        if (view.selected)
            info->state |= ui_access_selected;
        info->actions = view.locked ? ui_access_action_focus
                                    : ui_access_action_select | ui_access_action_invoke | ui_access_action_focus;
    }
    // Conversations without a stored name keep the drawn title (name from content).
    if (!name.bytes)
        return;
    g_name.bytes = 0;
    g_name.add(name);
    const Utf8View kind = channel_kind(view.type);
    if (kind.bytes)
        g_name.add(", ").add(kind);
    if (view.voice_count)
        g_name.add(", ").number(view.voice_count).add(" connected");
    // A voice channel with a session reads how long it has run, as the official client's row does.
    if (view.type == 2) {
        const auto& extras = application_voice_channels_extras();
        const auto* info = extras.info ? voice_channel_info_store_find(*extras.info, view.id) : nullptr;
        const u64 now_s = clock_unix_milliseconds() / 1'000;
        char duration[kUiVoiceDurationBytes];
        if (const u32 bytes = info && info->start_s
                                  ? ui_voice_duration_text(now_s > info->start_s ? now_s - info->start_s : 0, duration,
                                                           sizeof(duration))
                                  : 0)
            g_name.add(", call duration ").add(Utf8View{duration, bytes});
    }
    if (view.locked)
        g_name.add(", locked");
    if (view.muted)
        g_name.add(", muted");
    add_unread(&g_name, view.unread, view.mention_count);
    info->name = g_name.view();
}

// A message sticker draws no caption; its image is named "Sticker, <name>".
void describe_sticker(const MessageScene& scene, const MessageSceneRowWidget& row, UiWidgetHandle handle,
                      UiAccessWidgetInfo* info) noexcept {
    if (!g_app.account_ui)
        return;
    const auto& store = g_app.account_ui->message_rich_store;
    for (u32 snapshot = 0; snapshot < 2; ++snapshot) {
        const auto* items = snapshot ? message_scene_snapshot_row(scene, row) : &row;
        for (u32 index = 0; items && index < items->sticker_item_count; ++index) {
            if (!ui_widget_handle_equal(message_scene_rich_item_widget(*items, MessageSceneRichItemKind::sticker, index),
                                        handle))
                continue;
            const auto* rich = snapshot ? message_rich_store_snapshot(store, row.message_id)
                                        : message_rich_store_find(store, row.message_id);
            if (!rich || index >= rich->sticker_count)
                return;
            g_name.bytes = 0;
            g_name.add("Sticker, ").add(message_rich_store_string(store, rich->stickers[index].name));
            info->role = UiAccessRole::image;
            info->name = g_name.view();
            info->leaf = true;
            return;
        }
    }
}

void describe_message(void*, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&, const UiLayoutNode&,
                      UiAccessWidgetInfo* info) noexcept {
    const auto& scene = g_app.message_scene;
    if (ui_widget_handle_equal(handle, scene.root)) {
        info->role = UiAccessRole::list;
        g_name.bytes = 0;
        const auto* state = chat_state();
        const Utf8View name = state ? channel_name(state->selected_channel_id) : Utf8View{};
        g_name.add("Messages");
        if (name.bytes)
            g_name.add(" in ").add(name);
        info->name = g_name.view();
        info->actions = ui_access_action_scroll;
        if (scene.timeline && scene.timeline->list) {
            const auto& timeline = *scene.timeline;
            info->scroll = scroll_state(static_cast<float>(timeline.scroll_units),
                                        static_cast<float>(timeline.list->total_units),
                                        static_cast<float>(timeline.viewport_units));
        } else {
            info->scroll = {-1.0f, 100.0f};
        }
        return;
    }
    const auto* row = message_scene_resolve(scene, handle);
    if (!row)
        return;
    if (!ui_widget_handle_equal(row->widget, handle)) {
        describe_sticker(scene, *row, handle, info);
        return;
    }
    info->key = row->message_id;
    info->item = row->message_id;
    info->role = UiAccessRole::list_item;
    info->state |= ui_access_focusable;
    info->actions = ui_access_action_focus;
    if (g_app.context_focus == &scene.widgets && scene.focused_message_id == row->message_id)
        info->state |= ui_access_focused | ui_access_selected;
}

void describe_member(void*, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&, const UiLayoutNode&,
                     UiAccessWidgetInfo* info) noexcept {
    const auto& scene = g_app.member_list_scene;
    if (ui_widget_handle_equal(handle, scene.root)) {
        info->role = UiAccessRole::list;
        info->name = utf8_literal("Members");
        info->actions = ui_access_action_scroll;
        info->scroll = scroll_state(scene.scroll_dips, scene.content_height, ui_rect_height(scene.viewport));
        return;
    }
    const auto* row = member_list_scene_resolve(scene, handle);
    if (!row || !ui_widget_handle_equal(row->widget, handle))
        return;
    info->leaf = true;
    if (row->kind == MemberListRowKind::group) {
        info->role = UiAccessRole::header;
        info->key = row->source_row.group_role_id ^ 0x47524F5550ull;
        info->actions = ui_access_action_none;
        return;
    }
    info->key = row->user_id;
    info->item = row->user_id;
    info->role = UiAccessRole::list_item;
    info->state |= ui_access_focusable;
    info->actions = ui_access_action_invoke | ui_access_action_focus;
    if (g_app.context_focus == &scene.widgets && scene.focused_user_id == row->user_id)
        info->state |= ui_access_focused | ui_access_selected;
}

void describe_composer(void*, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&, const UiLayoutNode&,
                       UiAccessWidgetInfo* info) noexcept {
    const auto& scene = g_app.message_composer_scene;
    if (ui_widget_handle_equal(handle, scene.root)) {
        info->role = UiAccessRole::group;
        info->name = utf8_literal("Message composer");
        return;
    }
    if (ui_widget_handle_equal(handle, scene.edit)) {
        info->role = UiAccessRole::edit;
        info->state |= ui_access_multiline | ui_access_focusable;
        if (g_app.message_composer.focused)
            info->state |= ui_access_focused;
        // Value and Text: screen readers read and navigate the draft.
        info->actions = ui_access_action_value | ui_access_action_text | ui_access_action_focus;
        info->placement.selection_anchor = g_app.message_composer.anchor;
        info->placement.selection_caret = g_app.message_composer.caret;
        g_name.bytes = 0;
        const auto* state = chat_state();
        const Utf8View name = state ? channel_name(g_app.message_composer.channel_id) : Utf8View{};
        g_name.add("Message");
        if (name.bytes) {
            const auto* channel = chat_state_store_find_channel(*state, g_app.message_composer.channel_id);
            g_name.add(channel && channel_is_private(channel->type) ? utf8_literal(" @") : utf8_literal(" #"))
                .add(name);
        }
        info->name = g_name.view();
        info->value = message_composer_text(g_app.message_composer);
        return;
    }
    if (ui_widget_handle_equal(handle, scene.typing_indicator)) {
        info->role = UiAccessRole::status;
        info->leaf = true;
    }
    if (ui_widget_handle_equal(handle, scene.reply_mention)) {
        // A role="switch", as in the official client: its @ icon is labelled "Mention"; checked pings.
        info->role = UiAccessRole::toggle_button;
        info->name = utf8_literal("Mention");
        info->actions = ui_access_action_toggle;
        info->state |= ui_access_focusable | ui_access_toggleable |
                       (scene.mention_reply_author ? ui_access_checked : 0);
        info->leaf = true;
    }
}

Utf8View popup_name(PopupKind kind) noexcept {
    switch (kind) {
    case PopupKind::context_menu: return utf8_literal("Menu");
    case PopupKind::autocomplete: return utf8_literal("Suggestions");
    case PopupKind::settings_choice: return utf8_literal("Options");
    default: return utf8_literal("Popup");
    }
}

void describe_popup(void*, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&, const UiLayoutNode&,
                    UiAccessWidgetInfo* info) noexcept {
    const auto& scene = g_app.popup_scene;
    if (ui_widget_handle_equal(handle, scene.root)) {
        info->transparent = true;
        return;
    }
    if (ui_widget_handle_equal(handle, scene.panel)) {
        info->role = scene.kind == PopupKind::context_menu ? UiAccessRole::menu : UiAccessRole::list;
        info->name = popup_name(scene.kind);
        return;
    }
    for (u32 index = 0; index < scene.rows.count; ++index) {
        const auto& row = scene.rows[index];
        if (!ui_widget_handle_equal(row.widget, handle) || row.source_index >= scene.item_count)
            continue;
        const auto& item = scene.items[row.source_index];
        info->leaf = true;
        info->key = hash64_mix(hash64_mix(kHash64Seed, item.action), row.source_index);
        info->item = row.source_index;
        g_name.bytes = 0;
        g_name.add(item.label);
        // Command rows join description, app and option summary with U+001F,
        // the summary's option names and optional count with U+001E.
        for (u32 at = 0, begin = 0; item.detail.bytes && at <= item.detail.bytes; ++at)
            if (at == item.detail.bytes || item.detail.data[at] == '\x1F' || item.detail.data[at] == '\x1E') {
                if (at > begin)
                    g_name.add(", ").add(Utf8View{item.detail.data + begin, at - begin});
                begin = at + 1;
            }
        info->name = g_name.view();
        // The command picker's section headers and query title are headings;
        // its rail entries are labelled section buttons.
        if (item.flags & popup_item_command_section) {
            info->role = UiAccessRole::header;
            info->actions = ui_access_action_none;
            return;
        }
        if (item.flags & popup_item_command_category) {
            info->role = UiAccessRole::button;
            info->actions = ui_access_action_invoke;
            info->state |= ui_access_focusable;
            if (scene.selected_index == row.source_index)
                info->state |= ui_access_focused;
            return;
        }
        info->role = scene.kind == PopupKind::context_menu ? UiAccessRole::menu_item : UiAccessRole::list_item;
        info->actions = scene.kind == PopupKind::context_menu ? ui_access_action_invoke
                                                              : ui_access_action_select | ui_access_action_invoke;
        info->state |= ui_access_focusable;
        if (scene.kind != PopupKind::context_menu)
            info->state |= ui_access_selectable;
        if (!(item.flags & popup_item_enabled))
            info->state |= ui_access_disabled;
        if (item.flags & popup_item_checked) {
            info->state |= ui_access_checked | ui_access_toggleable;
            info->actions |= ui_access_action_toggle;
        }
        if (scene.selected_index == row.source_index)
            info->state |= ui_access_focused | ui_access_selected;
        return;
    }
}

// Adds a scene whose keyboard focus is the application's.
void add_scene(UiAccessTree* tree, u32 parent, const UiWidgetManager& widgets, UiAccessDescribe describe,
               u8 surface) noexcept {
    ui_access_add_widgets(tree, parent, widgets,
                          {describe, nullptr, surface, g_app.context_focus == &widgets});
}

void mount_guilds(void*, UiAccessTree* tree, u32 node) noexcept {
    if (g_app.guild_rail_scene.active)
        add_scene(tree, node, g_app.guild_rail_scene.widgets, describe_guild, surface_guilds);
}

void mount_channels(void*, UiAccessTree* tree, u32 node) noexcept {
    if (g_app.shell_navigation_scene.active)
        add_scene(tree, node, g_app.shell_navigation_scene.widgets, describe_channel, surface_channels);
}

void describe_friends(void*, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&, const UiLayoutNode&,
                      UiAccessWidgetInfo* info) noexcept {
    if (handle.slot == 0) {
        info->role = UiAccessRole::pane;
        info->name = utf8_literal("Friends");
    }
}

void mount_messages(void*, UiAccessTree* tree, u32 node) noexcept {
    const auto* friends = g_app.renderer.friends_scene;
    if (application_friends_active() && friends && friends->active)
        add_scene(tree, node, friends->widgets, describe_friends, surface_friends);
    else if (g_app.message_scene.active)
        add_scene(tree, node, g_app.message_scene.widgets, describe_message, surface_messages);
}

void mount_composer(void*, UiAccessTree* tree, u32 node) noexcept {
    if (g_app.message_composer_scene.active && !application_friends_active())
        add_scene(tree, node, g_app.message_composer_scene.widgets, describe_composer, surface_composer);
}

void mount_members(void*, UiAccessTree* tree, u32 node) noexcept {
    if (g_app.member_list_scene.active && !application_friends_active())
        add_scene(tree, node, g_app.member_list_scene.widgets, describe_member, surface_members);
}

// The voice card above the account panel: Show call, the screen share button and Disconnect.
void mount_voice_card(void*, UiAccessTree* tree, u32 node) noexcept {
    VoiceCardControl controls[3];
    const u32 count = application_voice_card_controls(controls, 3);
    for (u32 i = 0; i < count; ++i) {
        UiAccessNodeDesc description{};
        description.key = ui_access_key(surface_voice_card, 0x564f4943u + controls[i].control);
        description.rectangle = description.visible = controls[i].rectangle;
        description.role = UiAccessRole::button;
        description.name = controls[i].name;
        description.state = static_cast<u16>(ui_access_focusable | (controls[i].enabled ? 0 : ui_access_disabled));
        description.actions = ui_access_action_invoke;
        description.surface = surface_voice_card;
        ui_access_add(tree, node, description);
    }
}
void describe_shell(void*, const UiWidgetManager&, UiWidgetHandle, const UiWidget& widget, const UiLayoutNode&,
                    UiAccessWidgetInfo* info) noexcept {
    if (widget.id == 0 || widget.id > static_cast<u32>(UiSemantic::count))
        return; // thread header and other contributed widgets keep the default projection
    switch (static_cast<UiSemantic>(widget.id - 1)) {
    case UiSemantic::root:
    case UiSemantic::content:
        info->transparent = true;
        break;
    case UiSemantic::guild_rail:
        info->role = UiAccessRole::pane;
        info->name = utf8_literal("Servers sidebar");
        break;
    case UiSemantic::home_button:
        info->role = UiAccessRole::button;
        info->name = utf8_literal("Direct Messages");
        info->leaf = true;
        if (g_app.account_ui && !g_app.account_ui->navigation_view.selected_guild_id)
            info->state |= ui_access_selected;
        break;
    case UiSemantic::guild_navigation:
        info->transparent = true;
        info->mount = mount_guilds;
        break;
    case UiSemantic::channel_sidebar:
        info->role = UiAccessRole::pane;
        info->name = utf8_literal("Channels sidebar");
        break;
    case UiSemantic::channel_header:
        info->role = UiAccessRole::header;
        break;
    case UiSemantic::guild_menu: {
        // As in the official client: "<server>, server actions"; expands to the header popout.
        const auto* state = chat_state();
        const auto* guild = state && !state->home_selected
                                ? chat_state_store_find_guild(*state, state->selected_guild_id)
                                : nullptr;
        g_name.bytes = 0;
        g_name.add(guild ? chat_state_store_guild_name(*state, *guild) : utf8_literal("Server"))
            .add(", server actions");
        info->role = UiAccessRole::button;
        info->name = g_name.view();
        info->actions = ui_access_action_invoke | ui_access_action_expand;
        info->state |= g_app.shell_scene.guild_menu_expanded ? ui_access_expanded : ui_access_collapsed;
        info->leaf = true;
        break;
    }
    case UiSemantic::channel_navigation:
        info->transparent = true;
        info->mount = mount_channels;
        break;
    case UiSemantic::channel_footer:
        info->role = UiAccessRole::button;
        info->help = utf8_literal("Account and status");
        info->mount = mount_voice_card;
        break;
    case UiSemantic::title_bar:
        info->role = UiAccessRole::group;
        info->name = utf8_literal("Channel header");
        break;
    case UiSemantic::title_label:
        info->role = UiAccessRole::header;
        break;
    case UiSemantic::member_toggle:
        info->role = UiAccessRole::toggle_button;
        info->name = g_app.shell_scene.side_panel_profile ? utf8_literal("User profile") : utf8_literal("Member list");
        info->actions = ui_access_action_toggle;
        info->state |= ui_access_toggleable | ((g_app.shell_scene.side_panel_profile ? g_app.preferences.dm_profile_visible
                                                                                    : g_app.member_pane_visible)
                                                   ? ui_access_checked
                                                   : 0);
        info->leaf = true;
        break;
    case UiSemantic::pins_toggle:
        info->name = utf8_literal("Pinned messages");
        info->leaf = true;
        break;
    case UiSemantic::threads_toggle:
        info->name = utf8_literal("Threads");
        info->leaf = true;
        break;
    case UiSemantic::inbox_toggle:
        info->name = utf8_literal("Inbox");
        info->leaf = true;
        break;
    case UiSemantic::settings:
        info->name = utf8_literal("User settings");
        info->leaf = true;
        break;
    case UiSemantic::friends_button:
        info->name = utf8_literal("Friends");
        info->leaf = true;
        break;
    case UiSemantic::guild_invite:
        info->name = utf8_literal("Invite people");
        info->leaf = true;
        break;
    case UiSemantic::private_create:
        info->name = utf8_literal("Create direct message");
        info->leaf = true;
        break;
    case UiSemantic::private_search:
        info->name = utf8_literal("Find or start a conversation");
        info->leaf = true;
        break;
    case UiSemantic::private_heading:
        info->name = utf8_literal("Direct Messages");
        break;
    case UiSemantic::private_add:
        info->name = utf8_literal("Add people");
        info->leaf = true;
        break;
    case UiSemantic::private_call:
        info->name = utf8_literal("Start call");
        info->leaf = true;
        break;
    case UiSemantic::private_identity:
        application_private_header_access(info);
        break;
    case UiSemantic::message_view:
        info->transparent = true;
        info->mount = mount_messages;
        break;
    case UiSemantic::composer:
        info->transparent = true;
        info->mount = mount_composer;
        break;
    case UiSemantic::member_sidebar:
        info->role = UiAccessRole::pane;
        info->name = utf8_literal("Members sidebar");
        info->mount = mount_members;
        break;
    default:
        break;
    }
}

bool rect_inside(const UiRect& inner, const UiRect& outer) noexcept {
    return inner.left >= outer.left - 0.5f && inner.top >= outer.top - 0.5f && inner.right <= outer.right + 0.5f &&
           inner.bottom <= outer.bottom + 0.5f;
}

// Radio and segment rows: one RadioButton per option beneath the group node.
struct SettingsRadioMount {
    UiSettingsTarget target;
    const UiSettingsControl* control;
};
SettingsRadioMount g_settings_radio;

void mount_settings_radio(void*, UiAccessTree* tree, u32 node) noexcept {
    const auto* scene = g_app.renderer.settings_scene;
    const auto* control = g_settings_radio.control;
    if (!scene || !control)
        return;
    const UiAccessNode group = tree->nodes[node];
    for (u32 choice = 0; choice < control->choices.count; ++choice) {
        UiRect rectangle{};
        if (!ui_settings_scene_choice_rectangle(*scene, g_settings_radio.target, choice, &rectangle))
            continue;
        UiAccessNodeDesc option{};
        option.key = ui_access_key(surface_settings, hash64_mix(control->id, 0x524144494F00ull + choice));
        option.item = control->id;
        option.rectangle = rectangle;
        option.visible = ui_rect_intersection(rectangle, group.visible);
        option.role = UiAccessRole::radio_button;
        option.name = control->choices[choice];
        option.surface = surface_settings;
        option.state = ui_access_selectable | ui_access_focusable;
        if (choice == control->selected_choice)
            option.state |= ui_access_selected | ui_access_checked;
        if (!control->enabled)
            option.state |= ui_access_disabled;
        if (!ui_rect_has_area(option.visible))
            option.state |= ui_access_offscreen;
        option.actions = ui_access_action_select | ui_access_action_scroll_item;
        option.placement.position = choice + 1;
        option.placement.set_size = control->choices.count;
        option.ignore_text = true;
        ui_access_add(tree, node, option);
    }
}

void describe_settings(void*, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget& widget,
                       const UiLayoutNode& layout, UiAccessWidgetInfo* info) noexcept {
    const auto* scene = g_app.renderer.settings_scene;
    if (!scene)
        return;
    if (handle.slot == 0) {
        info->role = UiAccessRole::dialog;
        info->name = utf8_literal("Settings");
        info->state |= ui_access_modal;
        return;
    }
    const UiRect visible = ui_rect_intersection(layout.rectangle, layout.clip_rectangle);
    if (!ui_rect_has_area(visible))
        return;
    const UiPoint point = center(visible);
    const UiSettingsTarget target = ui_settings_scene_hit_test(*scene, point.x, point.y);
    const UiSettingsTarget focused = ui_settings_scene_focused(*scene);
    const bool is_focused = ui_settings_target_equal(target, focused) && target.kind != UiSettingsTargetKind::none;
    const bool button_like = widget.kind == UiWidgetKind::button || widget.kind == UiWidgetKind::text_field;
    switch (target.kind) {
    case UiSettingsTargetKind::control: {
        UiSettingsControlGeometry geometry{};
        const auto* control = ui_settings_scene_control(*scene, target);
        if (!control || !ui_settings_scene_control_geometry(*scene, target, &geometry) ||
            !rect_inside(layout.rectangle, geometry.control))
            return;
        info->leaf = true;
        info->key = control->id;
        info->item = control->id;
        info->name = control->label;
        info->help = control->description;
        info->state |= ui_access_focusable;
        if (!control->enabled)
            info->state |= ui_access_disabled;
        const auto choice = [&]() noexcept {
            return control->selected_choice < control->choices.count ? control->choices[control->selected_choice]
                                                                    : Utf8View{};
        };
        switch (control->kind) {
        case UiSettingsControlKind::toggle:
            info->role = UiAccessRole::check_box;
            info->actions = ui_access_action_toggle;
            info->state |= ui_access_toggleable | (control->checked ? ui_access_checked : 0);
            break;
        case UiSettingsControlKind::radio:
        case UiSettingsControlKind::segments:
        case UiSettingsControlKind::tabs:
            // Radio group: Selection over RadioButton children (mount_settings_radio).
            info->role = UiAccessRole::group;
            info->actions = ui_access_action_select | ui_access_action_scroll_item;
            info->ignore_text = true;
            g_settings_radio = {target, control};
            info->mount = mount_settings_radio;
            if (!info->name.bytes)
                info->name = utf8_literal("Options");
            break;
        case UiSettingsControlKind::choice:
        case UiSettingsControlKind::palette:
        case UiSettingsControlKind::swatches:
            info->role = UiAccessRole::combo_box;
            info->value = choice();
            info->actions = ui_access_action_value | ui_access_action_invoke | ui_access_action_scroll_item;
            info->state |= ui_access_read_only;
            break;
        case UiSettingsControlKind::action:
        case UiSettingsControlKind::buttons:
        case UiSettingsControlKind::colors:
            info->role = UiAccessRole::button;
            info->actions = ui_access_action_invoke;
            if (!control->label.bytes)
                info->name = control->action_label;
            // A grid tile (a Server Profile game) toggles: chosen when checked.
            if (control->kind == UiSettingsControlKind::action && scene->content.presentation) {
                u32 index = 0;
                if (ui_settings_scene_control(*scene, target, &index) &&
                    (scene->content.presentation[index].more & ui_settings_more_cell)) {
                    info->role = UiAccessRole::check_box;
                    info->actions = ui_access_action_toggle;
                    info->state |= ui_access_toggleable | (control->checked ? ui_access_checked : 0);
                }
            }
            break;
        case UiSettingsControlKind::edit:
        case UiSettingsControlKind::multiline:
            // Value.SetValue replaces the text through the page's editor (paste path).
            info->role = UiAccessRole::edit;
            info->actions = ui_access_action_value | ui_access_action_text | ui_access_action_focus |
                            ui_access_action_scroll_item;
            if (control->kind == UiSettingsControlKind::multiline)
                info->state |= ui_access_multiline;
            if (control->editor) {
                info->value = control->editor->view.text;
                info->placement.selection_anchor = control->editor->view.anchor;
                info->placement.selection_caret = control->editor->view.caret;
                if (!control->description.bytes)
                    info->help = control->editor->placeholder;
            }
            if (!control->enabled)
                info->state |= ui_access_read_only;
            break;
        case UiSettingsControlKind::slider: {
            info->role = UiAccessRole::slider;
            info->actions = ui_access_action_range;
            const float last = control->choices.count ? static_cast<float>(control->choices.count - 1) : 0.0f;
            info->range = {static_cast<float>(minimum<u32>(control->selected_choice,
                                                           control->choices.count ? control->choices.count - 1 : 0)),
                           0.0f, last, 1.0f, static_cast<float>(ui_slider_page_step(control->choices.count))};
            info->value = choice();
            break;
        }
        case UiSettingsControlKind::category:
            info->role = UiAccessRole::header;
            info->actions = ui_access_action_invoke;
            break;
        case UiSettingsControlKind::notice:
        case UiSettingsControlKind::status:
            // A loading notice or a running check is announced as live status; a check's button is the row's one
            // action.
            info->role = control->checked ? UiAccessRole::status : UiAccessRole::text;
            if (control->checked)
                info->state |= ui_access_live;
            info->actions = ui_access_action_invoke;
            if (control->kind == UiSettingsControlKind::status && control->action_label.bytes)
                info->value = control->action_label;
            break;
        case UiSettingsControlKind::disclosure:
            info->role = UiAccessRole::button;
            info->actions = ui_access_action_toggle;
            info->state |= ui_access_toggleable | (control->checked ? ui_access_checked : 0);
            break;
        case UiSettingsControlKind::markdown:
            // The raw markdown stands as the row's text; the link beneath expands it.
            info->role = UiAccessRole::text;
            info->name = control->description;
            info->help = {};
            info->actions = ui_access_action_invoke;
            break;
        case UiSettingsControlKind::footnote:
            info->role = UiAccessRole::text;
            info->actions = ui_access_action_invoke;
            if (control->action_label.bytes)
                info->value = control->action_label;
            break;
        default:
            info->role = UiAccessRole::text;
            info->actions = ui_access_action_invoke;
            break;
        }
        // Rows scroll into view through ui_settings_scene_reveal.
        info->actions |= ui_access_action_scroll_item;
        if (is_focused)
            info->state |= ui_access_focused;
        return;
    }
    case UiSettingsTargetKind::page:
        if (!button_like)
            return;
        info->leaf = true;
        info->role = UiAccessRole::tab;
        info->actions = ui_access_action_select;
        info->state |= ui_access_selectable | ui_access_focusable;
        if (ui_page_key_equal(target.page, scene->selected_page))
            info->state |= ui_access_selected;
        break;
    case UiSettingsTargetKind::close:
        if (!button_like)
            return;
        info->leaf = true;
        info->role = UiAccessRole::button;
        info->name = utf8_literal("Close settings");
        break;
    case UiSettingsTargetKind::save:
    case UiSettingsTargetKind::reset:
    case UiSettingsTargetKind::action:
    case UiSettingsTargetKind::search_clear:
        if (!button_like)
            return;
        info->leaf = true;
        info->role = UiAccessRole::button;
        info->actions = ui_access_action_invoke;
        break;
    case UiSettingsTargetKind::search:
        if (!button_like)
            return;
        info->leaf = true;
        info->role = UiAccessRole::edit;
        info->name = utf8_literal("Search settings");
        info->value = scene->search.view.text;
        info->placement.selection_anchor = scene->search.view.anchor;
        info->placement.selection_caret = scene->search.view.caret;
        info->actions = ui_access_action_value | ui_access_action_text | ui_access_action_focus;
        break;
    default:
        return;
    }
    if (is_focused)
        info->state |= ui_access_focused;
}

void describe_login(void*, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&, const UiLayoutNode& layout,
                    UiAccessWidgetInfo* info) noexcept {
    const auto& scene = g_app.login_scene;
    u32 element = static_cast<u32>(LoginElement::count);
    for (u32 index = 0; index < static_cast<u32>(LoginElement::count); ++index)
        if (ui_widget_handle_equal(scene.elements[index], handle)) {
            element = index;
            break;
        }
    const UiRect visible = ui_rect_intersection(layout.rectangle, layout.clip_rectangle);
    const UiPoint point = center(visible);
    const LoginControl control =
        ui_rect_has_area(visible) ? login_scene_hit_test(scene, point.x, point.y) : LoginControl::none;
    if (control != LoginControl::none && control == g_app.login.focus)
        info->state |= ui_access_focused;
    switch (static_cast<LoginElement>(element)) {
    case LoginElement::root:
        info->transparent = true;
        break;
    case LoginElement::card:
        info->role = UiAccessRole::dialog;
        info->name = utf8_literal("Log in");
        break;
    case LoginElement::heading:
        info->role = UiAccessRole::header;
        break;
    case LoginElement::login_field: {
        info->role = UiAccessRole::edit;
        info->name = utf8_literal("Email or phone number");
        info->actions = ui_access_action_value | ui_access_action_focus;
        info->leaf = true;
        info->ignore_text = true;
        const SecureEdit& edit = g_app.login.login;
        if (edit.password) {
            // A pasted verification or reset link stays masked.
            info->state |= ui_access_password;
        } else {
            const ConstSlice<u8> bytes = secure_edit_view(edit);
            info->value = {reinterpret_cast<const char*>(bytes.data), static_cast<u32>(bytes.count)};
            info->placement.selection_anchor = edit.anchor;
            info->placement.selection_caret = edit.caret;
            info->actions |= ui_access_action_text;
        }
        break;
    }
    case LoginElement::password_field:
        info->role = UiAccessRole::edit;
        info->name = scene.mfa_capabilities ? utf8_literal("Authentication code") : utf8_literal("Password");
        info->actions = ui_access_action_value | ui_access_action_focus;
        info->leaf = true;
        info->ignore_text = true;
        if (scene.mfa_capabilities) {
            const ConstSlice<u8> bytes = secure_edit_view(g_app.login.mfa);
            info->value = {reinterpret_cast<const char*>(bytes.data), static_cast<u32>(bytes.count)};
        } else {
            // Protected: IsPassword and no value in the snapshot, even while revealed.
            info->state |= ui_access_password;
        }
        break;

    case LoginElement::submit:
        info->leaf = true;
        if (scene.submit_disabled)
            info->state |= ui_access_disabled;
        break;
    case LoginElement::status:
        info->role = UiAccessRole::status;
        info->state |= ui_access_live;
        info->leaf = true;
        break;
    case LoginElement::qr:
        info->role = UiAccessRole::image;
        info->name = utf8_literal("Log in with QR code");
        info->help = utf8_literal("Scan it with the Discord mobile app.");
        info->leaf = true;
        break;
    default:
        break;
    }
}

// A known game's mention reads as the official client labels it, "@game
// <name>": its icon cell (an em, a 3 px gap and the glue) reads "@game ".
Utf8View accessible_body(const MessageTextCommand& command, char* buffer, u32 capacity) noexcept {
    u32 used = 0, from = 0;
    const GameStore* games = g_app.renderer.game_store;
    for (u32 i = 0; games && i < command.semantics.count; ++i) {
        const auto& semantic = command.semantics[i];
        if (semantic.kind != MessageMarkdownSemanticKind::game_icon || !game_store_name(*games, semantic.target).bytes)
            continue;
        const u32 cell = semantic.byte_offset, after = cell + 9; // U+2003 U+2006 U+200D
        if (cell < from || after > command.text.bytes || used + (cell - from) + 6 > capacity)
            return command.text;
        copy_bytes(buffer + used, command.text.data + from, cell - from);
        used += cell - from;
        copy_bytes(buffer + used, "@game ", 6);
        used += 6;
        from = after;
    }
    if (!from || used + command.text.bytes - from > capacity)
        return command.text;
    copy_bytes(buffer + used, command.text.data + from, command.text.bytes - from);
    return {buffer, used + command.text.bytes - from};
}

void attribute_message_bodies(UiAccessTree* tree) noexcept {
    const auto* presentation = g_app.renderer.message_text;
    if (!presentation)
        return;
    char body[4096];
    for (u32 index = 0; index < presentation->command_count && index < presentation->commands.count; ++index) {
        const auto& command = presentation->commands[index];
        if (command.owner.manager)
            ui_access_attribute_text(tree, command.owner, accessible_body(command, body, sizeof(body)),
                                     command.rectangle);
    }
}

// Grouped messages draw no author; the list item still starts with it.
void prefix_grouped_authors(UiAccessTree* tree) noexcept {
    const auto& scene = g_app.message_scene;
    if (!scene.active || !scene.timeline || !scene.timeline->store || !g_app.account_ui)
        return;
    const auto& users = g_app.account_ui->user_store;
    for (u32 index = 0; index < scene.count; ++index) {
        const auto& row = scene.rows[index];
        if (!row.grouped)
            continue;
        const u32 node = ui_access_widget_node(*tree, {&scene.widgets, row.widget});
        if (node == kUiAccessNone)
            continue;
        const auto found = message_store_find(*scene.timeline->store, row.message_id);
        const auto* record = found.ok() ? message_store_resolve(*scene.timeline->store, found.handle) : nullptr;
        const auto* user = record ? user_store_find(users, record->author_id) : nullptr;
        if (!user)
            continue;
        g_name.bytes = 0;
        g_name.add(user_store_name(users, *user)).add(": ").add(ui_access_name(*tree, node));
        ui_access_set_name(tree, node, g_name.view());
    }
}

void build_login(UiAccessTree* tree) noexcept {
    auto& scene = g_app.login_scene;
    if (!scene.widgets.count)
        return;
    ui_access_add_widgets(tree, 0, scene.widgets, {describe_login, nullptr, surface_login, false});
    // Labels, headings and status without any credential text.
    LoginView view{};
    application_login_view_state(&view);
    UiTextCommand commands[kMaximumLoginTextCommands]{};
    UiTextCommandStream stream{};
    if (login_view_valid(view) && ui_text_command_stream_initialize(&stream, {commands, kMaximumLoginTextCommands}) &&
        login_text_presentation_build(scene, view, application_theme(), &stream))
        for (u32 index = 0; index < stream.count; ++index)
            ui_access_attribute_text(tree, stream.storage[index].owner, stream.storage[index].text,
                                     stream.storage[index].rectangle);
}

// Overlay scenes above the shell, in paint order (later ones are on top and
// win keyboard focus when several report it).
void build_overlays(UiAccessTree* tree, bool settings) noexcept {
    const auto& renderer = g_app.renderer;
    if (!settings) {
        const u32 shown = d3d11_renderer_surfaces(renderer);
        if (shown & d3d11_surface_call)
            ui_access_add_call(tree, 0, *renderer.call_scene, surface_call);
        if (shown & d3d11_surface_thread_browser)
            ui_access_add_thread_browser(tree, 0, *renderer.thread_browser_scene, surface_threads);
        if (shown & d3d11_surface_search)
            ui_access_add_search(tree, 0, *renderer.search_scene, surface_search);
        if (shown & d3d11_surface_pinned_messages)
            ui_access_add_pinned_messages(tree, 0, *renderer.pinned_messages_scene, surface_pins);
        if (shown & d3d11_surface_inbox)
            ui_access_add_inbox(tree, 0, *renderer.inbox_scene, surface_inbox);
        if (shown & d3d11_surface_media_picker)
            ui_access_add_media_picker(tree, 0, *renderer.media_picker_scene, surface_picker);
        if (shown & d3d11_surface_quick_switcher)
            ui_access_add_quick_switcher(tree, 0, *renderer.quick_switcher_scene, surface_quick);
        if (shown & d3d11_surface_go_live)
            ui_access_add_go_live(tree, 0, *renderer.go_live_scene, surface_go_live);
        if (g_app.image_viewer.scene.active)
            ui_access_add_image_viewer(tree, 0, g_app.image_viewer.scene, surface_viewer);
    }
    if (renderer.profile_scene && renderer.profile_scene->active)
        profile_scene_access(tree, 0, *renderer.profile_scene, surface_profile,
                             !g_app.popup_scene.active && !g_app.text_form);
    if (renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active)
        ui_access_add_forum_post(tree, 0, *renderer.forum_form->scene, surface_forum);
    if (renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active)
        ui_access_add_text_form(tree, 0, *renderer.text_form->scene, surface_form);
}

void build_account(UiAccessTree* tree) noexcept {
    const bool settings = renderer_settings_page_visible(g_app.renderer);
    if (settings) {
        ui_access_add_widgets(tree, 0, g_app.renderer.settings_scene->widgets,
                              {describe_settings, nullptr, surface_settings, false});
        if (g_app.renderer.settings_store && g_app.renderer.settings_store->active)
            ui_access_add_store(tree, 0, *g_app.renderer.settings_store, surface_store);
    } else if (g_app.shell_scene.count) {
        add_scene(tree, 0, g_app.shell_scene.widgets, describe_shell, surface_shell);
    }
    build_overlays(tree, settings);
    if (g_app.popup_scene.active && g_app.popup_scene.widgets.count)
        ui_access_add_widgets(tree, 0, g_app.popup_scene.widgets, {describe_popup, nullptr, surface_popup, false});
    renderer_project_accessible_text(&g_app.renderer, application_theme(), ui_access_text_consumer, tree);
    if (!settings) {
        attribute_message_bodies(tree);
        prefix_grouped_authors(tree);
    }
}

// The window caption's buttons, above the content origin.
void build_caption(UiAccessTree* tree) noexcept {
    const UiCaptionBarLayout layout = application_caption_layout();
    const UiCaptionBarState state = application_caption_state();
    UiAccessNodeDesc bar{};
    bar.key = ui_access_key(surface_caption, 0);
    bar.rectangle = {0, layout.buttons[0].top, layout.width, 0};
    bar.name = utf8_literal("Title bar");
    bar.role = UiAccessRole::group;
    bar.surface = surface_caption;
    const u32 parent = ui_access_add(tree, 0, bar);
    if (parent == kUiAccessNone)
        return;
    static constexpr UiCaptionPart kParts[]{UiCaptionPart::icon,     UiCaptionPart::update,
                                     UiCaptionPart::settings, UiCaptionPart::minimize,
                                     UiCaptionPart::maximize, UiCaptionPart::close};
    for (const UiCaptionPart part : kParts) {
        const UiRect rectangle = ui_caption_bar_part_rect(layout, part);
        const Utf8View name = ui_caption_bar_part_name(part, state);
        if (!ui_rect_has_area(rectangle) || !name.bytes)
            continue;
        UiAccessNodeDesc button{};
        button.key = ui_access_key(surface_caption, static_cast<u64>(part));
        button.item = static_cast<u64>(part);
        button.rectangle = rectangle;
        button.name = name;
        button.role = UiAccessRole::button;
        button.actions = ui_access_action_invoke;
        button.surface = surface_caption;
        ui_access_add(tree, parent, button);
    }
    application_caption_describe_tabs(tree, parent, surface_caption);
}

// The app-wide notice band: a status line named by its text, with its button and its X.
void build_notice(UiAccessTree* tree) noexcept {
    const auto* band = application_notice_band();
    const auto* root = band ? ui_scene_node(g_app.shell_scene, UiSemantic::root) : nullptr;
    if (!root)
        return;
    const auto g = ui_notice_band_geometry(*band, root->rectangle, &g_app.account_ui->text_measure_cache);
    UiAccessNodeDesc line{};
    line.key = ui_access_key(surface_notice, 0);
    line.rectangle = g.band;
    line.name = band->text;
    line.role = UiAccessRole::status;
    line.surface = surface_notice;
    const u32 parent = ui_access_add(tree, 0, line);
    if (parent == kUiAccessNone)
        return;
    const auto button = [&](UiNoticeBandPart part, const UiRect& rectangle, Utf8View name) noexcept {
        if (!ui_rect_has_area(rectangle) || !name.bytes)
            return;
        UiAccessNodeDesc node{};
        node.key = ui_access_key(surface_notice, static_cast<u64>(part));
        node.item = static_cast<u64>(part);
        node.rectangle = rectangle;
        node.name = name;
        node.role = UiAccessRole::button;
        node.actions = ui_access_action_invoke;
        node.surface = surface_notice;
        ui_access_add(tree, parent, node);
    };
    button(UiNoticeBandPart::action, g.action, band->action);
    button(UiNoticeBandPart::dismiss, g.dismiss, utf8_literal("Dismiss"));
}

} // namespace
namespace accessibility_detail {

void build_snapshot() noexcept {
    auto& tree = g_access.tree;
    const float scale = access_scale(nullptr);
    const float caption = static_cast<float>(g_app.renderer.caption_height) / scale;
    const UiRect bounds{0, -caption, static_cast<float>(g_app.renderer.width) / scale,
                        static_cast<float>(g_app.renderer.height) / scale};
    g_access.host.content_offset_y = static_cast<i32>(g_app.renderer.caption_height);
    if (!ui_access_tree_reset(&tree, utf8_literal("MaiCord"), bounds))
        return;
    if (g_app.renderer.caption_height)
        build_caption(&tree);
    if (g_app.renderer_ready) {
        if (g_app.login.visible)
            build_login(&tree);
        else if (g_app.account_ui) {
            build_notice(&tree);
            build_account(&tree);
        }
    }
    if (!ui_access_tree_finish(&tree))
        log_write(LogLevel::warning, "accessibility", "snapshot_rejected");
}

} // namespace accessibility_detail
} // namespace maicord::app_detail
