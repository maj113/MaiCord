#include "application_guild_community.hpp"
#include "maicord/core/memory.hpp"
#include "application_content_point.hpp"
#include "application_caption.hpp"
#include "application_profile_editor.hpp"
#include "application_report.hpp"
#include "application_accessibility.hpp"

#include "application_account_logout.hpp"
#include "application_account_presentation.hpp"
#include "application_attachments_state.hpp"
#include "application_channel_overview.hpp"
#include "application_composer.hpp"
#include "application_composer_input.hpp"
#include "application_composer_text_input.hpp"
#include "application_content_input.hpp"
#include "application_custom_status.hpp"
#include "application_friends.hpp"
#include "application_guild_folders.hpp"
#include "application_guild_settings.hpp"
#include "application_login_input.hpp"
#include "application_login_ui.hpp"
#include "application_message_feedback.hpp"
#include "application_poll_create.hpp"
#include "application_settings_layer.hpp"
#include "application_state.hpp"
#include "maicord/app/voice_socket.hpp"
#include "maicord/app/voice_udp_discovery.hpp"
#include "../render/d3d11_renderer_interaction_state.hpp"
#include "maicord/app/login_continuations.hpp"
#include "maicord/app/message_interaction_rules.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/platform/uia_provider.hpp"
#include "maicord/render/forum_post_surface.hpp"
#include "maicord/render/text_form_surface.hpp"
#include "maicord/ui/accessibility.hpp"
#include "maicord/ui/accessibility_scenes.hpp"
#include "maicord/ui/login_text_presentation.hpp"
#include "maicord/ui/secure_edit.hpp"
#include "account_ui_runtime.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/sys/winsock.hpp"

namespace maicord::app_detail {
namespace {

// Surfaces tag nodes so deferred actions reach the owning controller.
enum AccessSurface : u8 {
    surface_shell = 1,
    surface_guilds,
    surface_channels,
    surface_messages,
    surface_composer,
    surface_members,
    surface_popup,
    surface_settings,
    surface_login,
    surface_friends,
    // Overlays (accessibility_scenes).
    surface_quick,
    surface_search,
    surface_pins,
    surface_threads,
    surface_inbox,
    surface_call,
    surface_forum,
    surface_form,
    surface_viewer,
    surface_picker,
    surface_caption,
    surface_profile,
};

constexpr u32 kNodeCapacity = 8192;
constexpr u32 kTextCapacity = 768u * 1024u;
constexpr u32 kSlotCapacity = 65536;
constexpr u32 kKeyIndexCapacity = 16384;
constexpr u32 kObservationCapacity = 4096;
// Nodes, slots and text are reserved at the capacities above and committed on
// demand: from these sizes, doubling when a snapshot exhausts a region.
constexpr u32 kInitialNodeCapacity = 1024;
constexpr u32 kInitialTextCapacity = 64u * 1024u;
constexpr u32 kInitialSlotCapacity = 8192;
constexpr u32 kPendingCapacity = 8;
// Deferred snapshot rebuild after coalesced animation frames.
constexpr UINT_PTR kRebuildTimer = 0x4D414343u;
// UiaClientsAreListening is sampled, not called per frame.
constexpr u64 kListenSampleMs = 250;
constexpr u64 kMentionSampleMs = 1000;
static_assert(kKeyIndexCapacity >= kNodeCapacity * 2);

struct PendingAction {
    u64 key;
    double number;
    UiAccessAction action;
};

// Status sources observed between published snapshots for announcements.
struct UploadObservation {
    u64 nonce;
    u32 generation;
    MessageAttachmentDraftPhase phase;
    bool occupied;
};

struct StatusObserver {
    UploadObservation uploads[kMessageAttachmentDraftCapacity];
    const void* account_instance;
    u64 voice_channel;
    u64 failed_channel;
    u64 mention_total;
    u64 mention_sampled_ms;
    u64 quick_summary, search_summary;
    const char* connection;
    u32 failed_count;
    u32 toast_serial;
    MessageComposerPhase composer_phase;
    bool voice_connected;
    bool primed, mentions_primed;
};

struct AccessibilityRuntime {
    UiaProviderHost host;
    UiAccessTree tree;
    UiAccessEventState events;
    UiAccessMessageCursor messages;
    UiAccessThrottle throttle;
    UiAccessAnnouncer announcer;
    StatusObserver status;
    void* storage;
    usize committed_bytes;
    PendingAction pending[kPendingCapacity];
    char pending_text[kUiAccessValueMaximumBytes];
    u64 listen_sampled_ms;
    u32 pending_text_bytes;
    u32 pending_count;
    bool initialized;
    bool dirty;
    bool posted;
    bool shut_down;
    bool listening;
    bool timer;
};

AccessibilityRuntime g_access;

// Describers compose names here; ui_access_add copies them immediately.
using NameBuilder = UiAccessNameBuilder;

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
        info->actions = ui_access_action_select | ui_access_action_invoke | ui_access_action_focus;
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
    if (view.muted)
        g_name.add(", muted");
    add_unread(&g_name, view.unread, view.mention_count);
    info->name = g_name.view();
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
    if (!row || !ui_widget_handle_equal(row->widget, handle))
        return;
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
        info->key = row->group_role_id ^ 0x47524F5550ull;
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
        if (item.detail.bytes)
            g_name.add(", ").add(item.detail);
        info->name = g_name.view();
        if (item.flags & popup_item_command_category) {
            info->role = UiAccessRole::header;
            info->actions = ui_access_action_none;
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
    case UiSemantic::private_create:
        info->name = utf8_literal("Create direct message");
        info->leaf = true;
        break;
    case UiSemantic::private_add:
        info->name = utf8_literal("Add people");
        info->leaf = true;
        break;
    case UiSemantic::private_call:
        info->name = utf8_literal("Start call");
        info->leaf = true;
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
            // A loading notice is announced as live status.
            info->role = control->checked ? UiAccessRole::status : UiAccessRole::text;
            if (control->checked)
                info->state |= ui_access_live;
            info->actions = ui_access_action_invoke;
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

void attribute_message_bodies(UiAccessTree* tree) noexcept {
    const auto* presentation = g_app.renderer.message_text;
    if (!presentation)
        return;
    for (u32 index = 0; index < presentation->command_count && index < presentation->commands.count; ++index) {
        const auto& command = presentation->commands[index];
        if (command.owner.manager)
            ui_access_attribute_text(tree, command.owner, command.text, command.rectangle);
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
    view.remote_user = {g_app.login.remote_user, g_app.login.remote_user_bytes};
    view.focus = g_app.login.focus;
    view.status = g_app.login.status;
    view.mfa_capabilities = scene.mfa_capabilities;
    view.selected_mfa_capability = scene.selected_mfa_capability;
    view.backup_acknowledgement_seconds = g_app.login.backup_acknowledgement_seconds;
    view.sms_sending = g_app.login.sms_sending;
    view.sms_sent = g_app.login.sms_requested;
    view.sms_failed = g_app.login.sms_failed;
    view.visible = true;
    view.remote_status = g_app.login.remote_status;
    view.reset_mode = g_app.login.reset_mode;
    view.captcha_status = g_app.login.captcha_status;
    UiTextCommand commands[kMaximumLoginTextCommands]{};
    UiTextCommandStream stream{};
    if (ui_text_command_stream_initialize(&stream, {commands, kMaximumLoginTextCommands}) &&
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
        if (renderer.call_scene && d3d11_renderer_call_visible(renderer))
            ui_access_add_call(tree, 0, *renderer.call_scene, surface_call);
        if (renderer.thread_browser_scene && d3d11_renderer_thread_browser_visible(renderer))
            ui_access_add_thread_browser(tree, 0, *renderer.thread_browser_scene, surface_threads);
        if (renderer.search_scene && d3d11_renderer_search_visible(renderer))
            ui_access_add_search(tree, 0, *renderer.search_scene, surface_search);
        if (renderer.pinned_messages_scene && d3d11_renderer_pinned_messages_visible(renderer))
            ui_access_add_pinned_messages(tree, 0, *renderer.pinned_messages_scene, surface_pins);
        if (renderer.inbox_scene && d3d11_renderer_inbox_visible(renderer))
            ui_access_add_inbox(tree, 0, *renderer.inbox_scene, surface_inbox);
        if (renderer.media_picker_scene && d3d11_renderer_media_picker_visible(renderer))
            ui_access_add_media_picker(tree, 0, *renderer.media_picker_scene, surface_picker);
        if (renderer.quick_switcher_scene && d3d11_renderer_quick_switcher_visible(renderer))
            ui_access_add_quick_switcher(tree, 0, *renderer.quick_switcher_scene, surface_quick);
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
}

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
        else if (g_app.account_ui)
            build_account(&tree);
    }
    if (!ui_access_tree_finish(&tree))
        log_write(LogLevel::warning, "accessibility", "snapshot_rejected");
}

// Storage layout: observations (current, previous) and the key index are
// committed whole; nodes, slots and text follow, reserved at full capacity.
constexpr usize kObservationBytes = sizeof(UiAccessObservation) * kObservationCapacity * 2;
constexpr usize kKeyBytes = sizeof(u32) * kKeyIndexCapacity;
constexpr usize kNodeOffset = kObservationBytes + kKeyBytes;
constexpr usize kSlotOffset = kNodeOffset + sizeof(UiAccessNode) * kNodeCapacity;
constexpr usize kTextOffset = kSlotOffset + sizeof(u32) * kSlotCapacity;
constexpr usize kStorageBytes = kTextOffset + kTextCapacity;

bool commit_range(u8* base, usize offset, usize bytes) noexcept {
    if (!bytes || VirtualAlloc(base + offset, bytes, MEM_COMMIT, PAGE_READWRITE))
        return true;
    log_write(LogLevel::warning, "accessibility", "storage_commit_failed");
    return false;
}

// Grows the regions the last snapshot exhausted; false when nothing can grow.
bool grow_exhausted() noexcept {
    auto& tree = g_access.tree;
    auto* base = static_cast<u8*>(g_access.storage);
    const auto grow = [&](u32 region, u32& count, u32 capacity, usize offset, usize unit) noexcept {
        if (!(tree.exhausted & region) || count >= capacity)
            return false;
        const u32 next = minimum(count * 2, capacity);
        if (!commit_range(base, offset, unit * next))
            return false;
        count = next;
        return true;
    };
    const bool nodes = grow(ui_access_exhausted_nodes, tree.nodes.count, kNodeCapacity, kNodeOffset,
                            sizeof(UiAccessNode));
    const bool slots = grow(ui_access_exhausted_slots, tree.slot_map.count, kSlotCapacity, kSlotOffset,
                            sizeof(u32));
    const bool text = grow(ui_access_exhausted_text, tree.text.count, kTextCapacity, kTextOffset, 1);
    if (!nodes && !slots && !text)
        return false;
    g_access.committed_bytes = kNodeOffset + sizeof(UiAccessNode) * tree.nodes.count +
                               sizeof(u32) * tree.slot_map.count + tree.text.count;
    return true;
}

void build_tree() noexcept {
    g_access.dirty = false;
    // A snapshot that ran out of committed storage is rebuilt after growing,
    // so the published tree matches one built at full capacity.
    do
        build_snapshot();
    while (g_access.tree.exhausted && grow_exhausted());
}

bool ensure_storage() noexcept {
    if (g_access.storage)
        return true;
    auto* base = static_cast<u8*>(VirtualAlloc(nullptr, kStorageBytes, MEM_RESERVE, PAGE_READWRITE));
    if (!base)
        return false;
    auto* observations = reinterpret_cast<UiAccessObservation*>(base);
    auto* keys = reinterpret_cast<u32*>(base + kObservationBytes);
    auto* nodes = reinterpret_cast<UiAccessNode*>(base + kNodeOffset);
    auto* slots = reinterpret_cast<u32*>(base + kSlotOffset);
    auto* text = reinterpret_cast<char*>(base + kTextOffset);
    if (!commit_range(base, 0, kNodeOffset) ||
        !commit_range(base, kNodeOffset, sizeof(UiAccessNode) * kInitialNodeCapacity) ||
        !commit_range(base, kSlotOffset, sizeof(u32) * kInitialSlotCapacity) ||
        !commit_range(base, kTextOffset, kInitialTextCapacity) ||
        !ui_access_tree_initialize(&g_access.tree, {nodes, kInitialNodeCapacity}, {text, kInitialTextCapacity},
                                   {slots, kInitialSlotCapacity}, {keys, kKeyIndexCapacity}) ||
        !ui_access_event_state_initialize(&g_access.events, {observations, kObservationCapacity},
                                          {observations + kObservationCapacity, kObservationCapacity})) {
        VirtualFree(base, 0, MEM_RELEASE);
        return false;
    }
    g_access.storage = base;
    g_access.committed_bytes = kNodeOffset + sizeof(UiAccessNode) * kInitialNodeCapacity +
                               sizeof(u32) * kInitialSlotCapacity + kInitialTextCapacity;
    g_access.dirty = true;
    return true;
}

const UiAccessTree* access_tree(void*) noexcept {
    if (!ensure_storage())
        return nullptr;
    if (g_access.dirty || !g_access.tree.finished)
        build_tree();
    return g_access.tree.finished ? &g_access.tree : nullptr;
}

// Value.SetValue and text selection reach only editors with an app edit path.
bool editable_by_automation(const UiAccessNode& node, UiAccessAction action) noexcept {
    if (node.role != UiAccessRole::edit && node.role != UiAccessRole::combo_box)
        return false;
    switch (node.surface) {
    case surface_composer:
        return true;
    case surface_settings:
        return node.item != 0; // inline page editors (not the settings search)
    case surface_login:
        return action == UiAccessAction::set_value;
    default:
        return false;
    }
}

bool access_act(void*, u64 key, UiAccessAction action, double number, Utf8View text) noexcept {
    if (g_access.shut_down || !g_app.window)
        return false;
    if (action == UiAccessAction::set_value || action == UiAccessAction::select_text) {
        const u32 index = g_access.tree.finished ? ui_access_find(g_access.tree, key) : kUiAccessNone;
        if (index == kUiAccessNone || !editable_by_automation(g_access.tree.nodes[index], action))
            return false;
    }
    if (action == UiAccessAction::set_value) {
        const u32 bytes = minimum<u32>(text.bytes, static_cast<u32>(sizeof(g_access.pending_text)));
        copy_bytes(g_access.pending_text, text.data, bytes);
        g_access.pending_text_bytes = bytes;
    }
    if (g_access.pending_count >= kPendingCapacity)
        return false;
    g_access.pending[g_access.pending_count++] = {key, number, action};
    if (!g_access.posted)
        g_access.posted = PostMessageW(g_app.window, kApplicationAccessibilityMessage, 0, 0) != FALSE;
    return true;
}

// Activates through the same pointer route a mouse click takes.
// Pointer messages carry client coordinates: content rows start below the caption.
bool click_at(float x, float y) noexcept {
    if (!g_app.window)
        return false;
    const float scale = access_scale(nullptr);
    const int caption = static_cast<int>(g_app.renderer.caption_height);
    const LPARAM point = MAKELPARAM(static_cast<int>(x * scale), static_cast<int>(y * scale) + caption);
    SendMessageW(g_app.window, WM_MOUSEMOVE, 0, point);
    SendMessageW(g_app.window, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(g_app.window, WM_LBUTTONUP, 0, point);
    POINT cursor{};
    RECT client{};
    if (GetCursorPos(&cursor) && ScreenToClient(g_app.window, &cursor) && GetClientRect(g_app.window, &client) &&
        cursor.x >= client.left && cursor.y >= client.top + caption && cursor.x < client.right &&
        cursor.y < client.bottom)
        SendMessageW(g_app.window, WM_MOUSEMOVE, 0, MAKELPARAM(cursor.x, cursor.y));
    application_request_frame();
    return true;
}

bool click_node(const UiAccessNode& node) noexcept {
    if ((node.state & ui_access_offscreen) || !ui_rect_has_area(node.visible))
        return false;
    const UiPoint point = center(node.visible);
    return click_at(point.x, point.y);
}

template <typename T> T* mutable_scene(const T* scene) noexcept {
    return const_cast<T*>(scene); // renderer bindings borrow scenes owned mutably by their controllers
}

// Scroll offset that brings [top, top + height) inside a viewport of extent.
float reveal_offset(float scroll, float top, float height, float extent) noexcept {
    if (top < scroll)
        return top;
    if (top + height > scroll + extent)
        return top + height - extent;
    return scroll;
}

// ScrollItem.ScrollIntoView: each list scrolls through its own reveal routine.
bool reveal_item(const UiAccessNode& node) noexcept {
    const u64 item = node.item;
    if (!item)
        return false;
    auto& renderer = g_app.renderer;
    const auto* view = g_app.account_ui ? &g_app.account_ui->navigation_view : nullptr;
    switch (node.surface) {
    case surface_guilds:
        return view && guild_rail_scene_reveal_guild(&g_app.guild_rail_scene, *view, item);
    case surface_channels:
        return view && shell_navigation_scene_reveal_channel(&g_app.shell_navigation_scene, *view, item);
    case surface_messages:
        return message_scene_reveal_message(&g_app.message_scene, item);
    case surface_members:
        return member_list_scene_reveal_user(&g_app.member_list_scene, item);
    case surface_settings: {
        auto* scene = mutable_scene(renderer.settings_scene);
        return scene && ui_settings_scene_reveal(scene, {UiSettingsTargetKind::control, scene->selected_page, item});
    }
    case surface_quick: {
        auto* scene = mutable_scene(renderer.quick_switcher_scene);
        if (!scene || item - 1 >= scene->accepted_count)
            return false;
        const float top = static_cast<float>(item - 1) * kUiQuickSwitcherRowHeight;
        return ui_quick_switcher_scene_scroll(
            scene, reveal_offset(scene->scroll_dips, top, kUiQuickSwitcherRowHeight, ui_rect_height(scene->body)));
    }
    case surface_search: {
        auto* scene = mutable_scene(renderer.search_scene);
        const u64 index = item - 1;
        if (!scene || scene->layout != UiSearchLayout::list || !scene->source || index >= scene->source->count ||
            index + 1 >= sizeof(scene->row_offsets) / sizeof(scene->row_offsets[0]))
            return false;
        const float top = scene->row_offsets[index], bottom = scene->row_offsets[index + 1];
        return ui_search_scene_scroll(
            scene, reveal_offset(scene->scroll_dips, top, bottom - top, ui_rect_height(scene->body)));
    }
    case surface_pins: {
        auto* scene = mutable_scene(renderer.pinned_messages_scene);
        return scene && scene->content &&
               ui_pinned_messages_scene_focus(
                   scene, {UiPinnedMessagesAction::jump, scene->content->channel_id, item});
    }
    case surface_threads: {
        auto* scene = renderer.thread_browser_scene;
        return scene && ui_thread_browser_scene_focus(
                            scene, {UiThreadBrowserAction::open_thread, item, scene->source_publication, {}});
    }
    case surface_inbox: {
        auto* scene = mutable_scene(renderer.inbox_scene);
        if (!scene)
            return false;
        for (u32 slot = 0; slot < scene->count && slot < kUiInboxVisibleRows; ++slot)
            if (scene->first + slot + 1 == item && scene->rows[slot].generation &&
                scene->rows[slot].slot < kUiInboxWidgetCapacity)
                return ui_inbox_scene_focus(scene, scene->targets[scene->rows[slot].slot]);
        return false;
    }
    case surface_picker: {
        auto* scene = mutable_scene(renderer.media_picker_scene);
        return scene && ui_media_picker_scene_select(scene, static_cast<u32>(item - 1));
    }
    default:
        return false;
    }
}

bool account_surface(u8 surface) noexcept {
    return surface == surface_guilds || surface == surface_channels || surface == surface_messages ||
           surface == surface_members;
}

// Reveals the node, republishes the frame and returns the node at its new place.
bool reveal_and_resolve(UiAccessNode* node) noexcept {
    if (!reveal_item(*node))
        return false;
    if (account_surface(node->surface) && !refresh_account_frame(true))
        log_write(LogLevel::warning, "accessibility", "reveal_refresh_failed");
    application_request_frame();
    g_access.dirty = true;
    const UiAccessTree* tree = access_tree(nullptr);
    const u32 index = tree ? ui_access_find(*tree, node->key) : kUiAccessNone;
    if (index == kUiAccessNone)
        return false;
    *node = tree->nodes[index];
    return true;
}

SettingsLayer* active_settings_layer() noexcept {
    SettingsLayer* const layers[]{application_channel_overview_settings_layer(),
                                  application_custom_status_settings_layer(),
                                  application_guild_settings_settings_layer(),
                                  application_poll_create_settings_layer(),
                                  application_guild_community_settings_layer(),
                                  application_profile_editor_settings_layer(),
                                  application_report_settings_layer()};
    for (auto* layer : layers)
        if (layer && layer->scene && layer->scene == g_app.renderer.settings_scene && layer->scene->active)
            return layer;
    return nullptr;
}

// Focuses the editor through the pointer path, then replaces its text.
bool set_settings_text(const UiAccessNode& node, Utf8View text) noexcept {
    SettingsLayer* layer = active_settings_layer();
    if (!layer)
        return false;
    u64 control = 0;
    MessageComposer* model = nullptr;
    BoundedEdit* binding = nullptr;
    if (!settings_layer_editor(layer, &control, &model, &binding, nullptr) || control != node.item) {
        click_node(node);
        if (!settings_layer_editor(layer, &control, &model, &binding, nullptr) || control != node.item)
            return false;
    }
    return settings_layer_replace_text(layer, text);
}

bool select_settings_text(const UiAccessNode& node, u32 anchor, u32 caret) noexcept {
    SettingsLayer* layer = active_settings_layer();
    u64 control = 0;
    MessageComposer* model = nullptr;
    BoundedEdit* binding = nullptr;
    if (!layer || !settings_layer_editor(layer, &control, &model, &binding, nullptr) || control != node.item)
        return false;
    message_composer_set_selection(model, anchor, caret);
    return settings_layer_edit_changed(layer, false);
}

// Login fields: the field is focused through the pointer path first; the
// identifier keeps the paste path's rules (no links, recovery validation).
bool set_login_text(const UiAccessNode& node, Utf8View text) noexcept {
    auto& login = g_app.login;
    if (login_phone_mode(login.status))
        return false;
    click_node(node);
    SecureEdit* edit = application_active_login_edit();
    if (!edit)
        return false;
    const bool identifier = edit == &login.login;
    if (identifier && (text.bytes > kUserAuthMaximumLoginBytes || login_verification_link_candidate(text) ||
                       login_reset_link_candidate(text) ||
                       (login_recovery_mode(login.status) && text.bytes && !user_auth_recovery_identifier_valid(text))))
        return false;
    SecureEditResult result = SecureEditResult::unchanged;
    if (text.bytes) {
        secure_edit_select_all(edit);
        result = secure_edit_insert_utf8(edit, text);
    } else {
        result = secure_edit_clear(edit);
    }
    if (identifier) {
        if (result == SecureEditResult::changed)
            edit->password = false;
        application_login_prepare_identifier();
    } else if (edit == &login.mfa && result == SecureEditResult::changed &&
               login.status == RendererLoginStatus::mfa_input_invalid) {
        login.status = RendererLoginStatus::mfa_required;
    }
    return result != SecureEditResult::changed || application_refresh_login_view();
}

bool select_composer_text(u32 anchor, u32 caret) noexcept {
    if (!g_app.account_ui || !g_app.message_composer_scene.active)
        return false;
    return composer_apply_result(message_composer_set_selection(&g_app.message_composer, anchor, caret), false);
}

// Overlay rows that select without activating.
bool select_overlay_item(const UiAccessNode& node) noexcept {
    switch (node.surface) {
    case surface_quick: {
        auto* scene = mutable_scene(g_app.renderer.quick_switcher_scene);
        const bool selected = scene && node.item && ui_quick_switcher_scene_select(scene, static_cast<u32>(node.item - 1));
        application_request_frame();
        return selected;
    }
    case surface_picker:
    case surface_pins:
    case surface_threads:
    case surface_inbox:
        if (node.role != UiAccessRole::list_item)
            return false;
        reveal_item(node);
        application_request_frame();
        return true;
    default:
        return false;
    }
}

bool wheel_node(const UiAccessNode& node, UiAccessScroll amount) noexcept {
    if (!ui_rect_has_area(node.visible) || amount == UiAccessScroll::none)
        return false;
    const bool up = amount == UiAccessScroll::small_decrement || amount == UiAccessScroll::large_decrement;
    const bool large = amount == UiAccessScroll::large_decrement || amount == UiAccessScroll::large_increment;
    const int delta = (large ? 5 : 1) * (up ? WHEEL_DELTA : -WHEEL_DELTA);
    const UiPoint point = center(node.visible);
    const float scale = access_scale(nullptr);
    POINT screen{static_cast<LONG>(point.x * scale), static_cast<LONG>(point.y * scale)};
    application_content_to_screen(&screen);
    SendMessageW(g_app.window, WM_MOUSEWHEEL, MAKEWPARAM(0, delta), MAKELPARAM(screen.x, screen.y));
    application_request_frame();
    return true;
}

void focus_owner(UiWidgetManager* owner) noexcept {
    ui_button_keyboard_cancel(&g_app.button_keyboard);
    if (owner != &g_app.message_composer_scene.widgets && g_app.message_composer.focused)
        release_message_composer_focus();
    g_app.context_focus = owner;
    g_app.keyboard_focus_visible = true;
    SetFocus(g_app.window);
    if (!refresh_account_frame(true))
        log_write(LogLevel::warning, "accessibility", "focus_refresh_failed");
}

bool focus_item(u8 surface, u64 item) noexcept {
    if (!g_app.account_ui || !item)
        return false;
    const auto& view = g_app.account_ui->navigation_view;
    switch (surface) {
    case surface_guilds:
        if (!guild_rail_scene_reveal_guild(&g_app.guild_rail_scene, view, item))
            return false;
        focus_owner(&g_app.guild_rail_scene.widgets);
        return true;
    case surface_channels:
        if (!shell_navigation_scene_reveal_channel(&g_app.shell_navigation_scene, view, item))
            return false;
        focus_owner(&g_app.shell_navigation_scene.widgets);
        return true;
    case surface_messages:
        if (!message_scene_reveal_message(&g_app.message_scene, item) &&
            !message_scene_focus_message(&g_app.message_scene, item))
            return false;
        focus_owner(&g_app.message_scene.widgets);
        return true;
    case surface_members:
        if (!member_list_scene_reveal_user(&g_app.member_list_scene, item))
            return false;
        focus_owner(&g_app.member_list_scene.widgets);
        return true;
    default:
        return false;
    }
}

bool set_composer_text(Utf8View text) noexcept {
    if (!g_app.account_ui || !g_app.message_composer_scene.active)
        return false;
    const Utf8View current = message_composer_text(g_app.message_composer);
    return composer_apply_result(message_composer_replace_range(&g_app.message_composer, 0, current.bytes, text), true);
}

bool set_settings_slider(const UiAccessNode& node, double value) noexcept {
    const auto* scene = g_app.renderer.settings_scene;
    if (!scene || !ui_rect_has_area(node.visible) || node.range.maximum <= node.range.minimum)
        return false;
    const UiPoint point = center(node.visible);
    const UiSettingsTarget target = ui_settings_scene_hit_test(*scene, point.x, point.y);
    UiSettingsSliderGeometry geometry{};
    if (!ui_settings_scene_slider_geometry(*scene, target, &geometry) || !ui_rect_has_area(geometry.track))
        return false;
    const float fraction = static_cast<float>((value - node.range.minimum) / (node.range.maximum - node.range.minimum));
    const float x = geometry.track.left + fraction * ui_rect_width(geometry.track);
    return click_at(minimum(geometry.track.right - 0.5f, maximum(geometry.track.left + 0.5f, x)),
                    (geometry.track.top + geometry.track.bottom) * 0.5f);
}

void run_action(const PendingAction& pending) noexcept {
    const UiAccessTree* tree = access_tree(nullptr);
    if (!tree)
        return;
    const u32 index = ui_access_find(*tree, pending.key);
    if (index == kUiAccessNone)
        return;
    UiAccessNode node = tree->nodes[index];
    const bool locked = application_account_logout_blocked() || application_account_logout_host_owned();
    const bool row = node.role == UiAccessRole::list_item;
    const bool edit = node.role == UiAccessRole::edit || node.role == UiAccessRole::combo_box;
    const Utf8View text{g_access.pending_text, g_access.pending_text_bytes};
    switch (pending.action) {
    case UiAccessAction::focus:
        if (!locked && row && focus_item(node.surface, node.item))
            return;
        if (node.surface == surface_composer && node.role == UiAccessRole::edit && !locked) {
            if (focus_message_composer_control(MessageComposerControl::edit))
                focus_owner(&g_app.message_composer_scene.widgets);
            return;
        }
        if (!locked && row && select_overlay_item(node))
            return;
        if (edit) {
            if (ui_access_needs_reveal(node) && !reveal_and_resolve(&node))
                return;
            click_node(node);
        }
        return;
    case UiAccessAction::select:
    case UiAccessAction::invoke:
    case UiAccessAction::toggle:
        if (node.surface == surface_caption) {
            application_caption_command(static_cast<UiCaptionPart>(node.item));
            return;
        }
        if (!locked && row && (node.surface == surface_guilds || node.surface == surface_channels) &&
            pending.action != UiAccessAction::toggle && !(node.state & (ui_access_expanded | ui_access_collapsed))) {
            if (focus_item(node.surface, node.item) && !activate_focused_navigation())
                log_write(LogLevel::warning, "accessibility", "navigation_activation_failed");
            return;
        }
        if (!locked && row && node.surface == surface_messages) {
            focus_item(node.surface, node.item);
            return;
        }
        if (!locked && pending.action == UiAccessAction::select && select_overlay_item(node))
            return;
        // Offscreen (or mostly clipped) items scroll into view before the click.
        if (ui_access_needs_reveal(node) &&
            (!(node.actions & ui_access_action_scroll_item) || !reveal_and_resolve(&node)))
            return;
        click_node(node);
        return;
    case UiAccessAction::expand:
    case UiAccessAction::collapse: {
        if (node.surface == surface_quick)
            return; // the switcher's list is open for as long as the switcher
        const bool expanded = (node.state & ui_access_expanded) != 0;
        if (expanded != (pending.action == UiAccessAction::expand))
            click_node(node);
        return;
    }
    case UiAccessAction::scroll:
        wheel_node(node, static_cast<UiAccessScroll>(static_cast<u32>(pending.number)));
        return;
    case UiAccessAction::scroll_into_view:
        if (!reveal_item(node))
            return;
        if (account_surface(node.surface) && !refresh_account_frame(true))
            log_write(LogLevel::warning, "accessibility", "reveal_refresh_failed");
        application_request_frame();
        return;
    case UiAccessAction::set_value:
        if (locked)
            return;
        if (node.surface == surface_composer && node.role == UiAccessRole::edit)
            set_composer_text(text);
        else if (node.surface == surface_settings)
            set_settings_text(node, text);
        else if (node.surface == surface_login)
            set_login_text(node, text);
        return;
    case UiAccessAction::select_text: {
        u32 anchor = 0, caret = 0;
        ui_access_text_selection_unpack(pending.number, &anchor, &caret);
        if (locked)
            return;
        if (node.surface == surface_composer)
            select_composer_text(anchor, caret);
        else if (node.surface == surface_settings)
            select_settings_text(node, anchor, caret);
        return;
    }
    case UiAccessAction::set_range:
        if (node.surface == surface_settings)
            set_settings_slider(node, pending.number);
        return;
    default:
        return;
    }
}

void run_pending() noexcept {
    PendingAction actions[kPendingCapacity];
    const u32 count = g_access.pending_count;
    for (u32 index = 0; index < count; ++index)
        actions[index] = g_access.pending[index];
    g_access.pending_count = 0;
    for (u32 index = 0; index < count; ++index) {
        g_access.dirty = true;
        run_action(actions[index]);
    }
    g_access.dirty = true;
    // The next frame reports the action's result at once.
    ui_access_throttle_mark_urgent(&g_access.throttle);
}

// Text pattern geometry for the edit that owns native text input.
u32 access_text_bounds(void*, u64 key, u32 start, u32 end, UiRect* rectangles, u32 capacity) noexcept {
    const auto& tree = g_access.tree;
    const u32 index = tree.finished && capacity ? ui_access_find(tree, key) : kUiAccessNone;
    if (index == kUiAccessNone || !(tree.nodes[index].state & ui_access_focused))
        return 0;
    return application_composer_text_input_range_bounds(ui_access_value(tree, index), start, end, rectangles) ? 1 : 0;
}


u64 event_key(const UiAccessTree& tree, u32 node) noexcept {
    return node == 0 || node >= tree.count ? 0 : tree.nodes[node].key;
}

void announce_new_message(const UiAccessTree& tree) noexcept {
    const auto& scene = g_app.message_scene;
    const auto* state = chat_state();
    if (!state || g_app.login.visible || application_friends_active() || !scene.active || !scene.timeline ||
        !scene.timeline->store)
        return;
    const u32 count = message_scene_source_count(scene);
    const MessageRecord* record =
        count ? message_store_resolve(*scene.timeline->store, message_scene_source_handle(scene, count - 1)) : nullptr;
    const bool local = record && (record->author_id == state->self_user_id ||
                                  record->delivery != MessageDeliveryState::authoritative);
    if (!ui_access_message_should_announce(&g_access.messages, state->selected_channel_id, record ? record->id : 0,
                                           local) ||
        GetForegroundWindow() != g_app.window)
        return;
    const auto& users = g_app.account_ui->user_store;
    const auto* user = user_store_find(users, record->author_id);
    g_name.bytes = 0;
    if (user)
        g_name.add(user_store_name(users, *user)).add(": ");
    // Prefer the drawn (markdown-resolved) content of a materialized row.
    const u32 before = g_name.bytes;
    const auto* presentation = g_app.renderer.message_text;
    for (u32 row = 0; presentation && row < scene.count; ++row) {
        if (scene.rows[row].message_id != record->id)
            continue;
        for (u32 index = 0; index < presentation->command_count && index < presentation->commands.count; ++index) {
            const auto& command = presentation->commands[index];
            if (command.message_id == record->id && command.owner.manager == &scene.widgets &&
                ui_widget_handle_equal(command.owner.handle, scene.rows[row].content))
                g_name.add(command.text).add(" ");
        }
        break;
    }
    if (g_name.bytes == before) {
        const auto found = message_store_find(*scene.timeline->store, record->id);
        if (found.ok())
            g_name.add(message_store_content(*scene.timeline->store, found.handle));
    }
    const u32 list = ui_access_widget_node(tree, {&scene.widgets, scene.root});
    uia_host_raise_notification(&g_access.host, list == kUiAccessNone ? 0 : event_key(tree, list),
                                UiaNotificationKind::item_added, UiaNotificationProcessing::all, g_name.view(),
                                utf8_literal("MaiCord.NewMessage"));
}

void announce(Utf8View text, bool assertive, Utf8View activity = utf8_literal("MaiCord.Status")) noexcept {
    if (!ui_access_announce_admit(&g_access.announcer, text,
                                  assertive ? UiAccessPoliteness::assertive : UiAccessPoliteness::polite,
                                  GetTickCount64()))
        return;
    uia_host_raise_notification(&g_access.host, 0, UiaNotificationKind::other,
                                assertive ? UiaNotificationProcessing::important_all
                                          : UiaNotificationProcessing::most_recent,
                                text, activity);
}

u64 text_hash(Utf8View text) noexcept {
    return text.bytes ? hash64_bytes(kHash64Seed, text.data, text.bytes) : 0;
}

// Reconnecting / offline strip above the composer.
void observe_connection(StatusObserver& status, bool primed) noexcept {
    const Utf8View notice = application_composer_status_notice();
    const Utf8View reconnecting = connection_notice_text(ConnectionNoticeState::reconnecting);
    const Utf8View offline = connection_notice_text(ConnectionNoticeState::offline);
    const char* shown = notice.data == reconnecting.data || notice.data == offline.data ? notice.data : nullptr;
    if (primed && shown != status.connection) {
        if (shown == reconnecting.data)
            announce(reconnecting, false);
        else if (shown == offline.data)
            announce(offline, true);
        else if (status.connection)
            announce(utf8_literal("Connected"), false);
    }
    status.connection = shown;
}

// Composer send failures and failed deliveries in the open conversation.
void observe_sends(StatusObserver& status, bool primed) noexcept {
    const MessageComposerPhase phase = g_app.message_composer.phase;
    if (primed && phase == MessageComposerPhase::failed && status.composer_phase != MessageComposerPhase::failed)
        announce(utf8_literal("Message failed to send"), true);
    status.composer_phase = phase;
    const auto& scene = g_app.message_scene;
    const auto* state = chat_state();
    u32 failed = 0;
    if (state && scene.active && scene.timeline && scene.timeline->store) {
        const u32 count = message_scene_source_count(scene);
        for (u32 index = count > 8 ? count - 8 : 0; index < count; ++index) {
            const auto* record = message_store_resolve(*scene.timeline->store, message_scene_source_handle(scene, index));
            failed += record && record->delivery == MessageDeliveryState::failed ? 1u : 0u;
        }
    }
    const u64 channel = state ? state->selected_channel_id : 0;
    if (primed && channel == status.failed_channel && failed > status.failed_count)
        announce(utf8_literal("Message failed to send"), true);
    status.failed_channel = channel;
    status.failed_count = failed;
}

// Attachment uploads finishing or failing.
void observe_uploads(StatusObserver& status, bool primed) noexcept {
    const auto* attachments = g_app.attachments;
    for (u32 index = 0; index < kMessageAttachmentDraftCapacity; ++index) {
        UploadObservation now{};
        if (attachments && attachments->drafts.records[index].occupied) {
            const auto& record = attachments->drafts.records[index];
            now = {record.nonce, record.generation, record.phase, true};
        }
        auto& before = status.uploads[index];
        const bool same = now.occupied && before.occupied && before.nonce == now.nonce &&
                          before.generation == now.generation;
        const bool in_flight = before.phase == MessageAttachmentDraftPhase::uploading ||
                               before.phase == MessageAttachmentDraftPhase::waiting_url;
        if (primed && same && in_flight && now.phase == MessageAttachmentDraftPhase::ready)
            announce(utf8_literal("Upload complete"), false);
        else if (primed && same && now.phase == MessageAttachmentDraftPhase::failed &&
                 before.phase != MessageAttachmentDraftPhase::failed)
            announce(utf8_literal("Upload failed"), true);
        before = now;
    }
}

// Each toast is read once as it appears; failures interrupt.
void observe_toast(StatusObserver& status, bool primed) noexcept {
    const auto& toasts = g_app.toasts;
    const Utf8View text = ui_toast_current_text(toasts);
    if (!text.bytes || toasts.serial == status.toast_serial)
        return;
    status.toast_serial = toasts.serial;
    if (primed)
        announce(text, ui_toast_current_kind(toasts) == UiToastKind::failure,
                 utf8_literal("MaiCord.Toast"));
}

// Joining and leaving voice.
void observe_voice(StatusObserver& status, bool primed) noexcept {
    const auto* session = g_app.account_ui ? &g_app.account_ui->voice_session : nullptr;
    const u64 channel = session && session->phase != VoiceSetupPhase::idle ? session->channel_id : 0;
    const bool connected = channel && session->socket_phase == VoiceSocketPhase::open &&
                           session->udp_phase == VoiceUdpPhase::ready;
    if (primed && connected && (!status.voice_connected || channel != status.voice_channel)) {
        g_name.bytes = 0;
        g_name.add("Voice connected");
        const Utf8View name = channel_name(channel);
        if (name.bytes)
            g_name.add(", ").add(name);
        announce(g_name.view(), false);
    } else if (primed && !channel && status.voice_channel) {
        announce(utf8_literal("Voice disconnected"), false);
    }
    status.voice_channel = channel;
    status.voice_connected = connected;
}

// New mentions anywhere (sampled; the unread counters are authoritative).
void observe_mentions(StatusObserver& status, u64 now_ms) noexcept {
    const auto* state = chat_state();
    if (!state || (status.mentions_primed && now_ms - status.mention_sampled_ms < kMentionSampleMs))
        return;
    status.mention_sampled_ms = now_ms;
    u64 total = 0, newest = 0;
    const ChannelRecord* latest = nullptr;
    for (u32 index = 0; index < state->channel_count; ++index) {
        const auto& channel = state->channels[index];
        if (!channel.occupied || !channel.mention_count)
            continue;
        total += channel.mention_count;
        if (channel.last_message_id >= newest) {
            newest = channel.last_message_id;
            latest = &channel;
        }
    }
    if (status.mentions_primed && total > status.mention_total && latest) {
        g_name.bytes = 0;
        g_name.add("New mention");
        const Utf8View name = chat_state_store_channel_name(*state, *latest);
        if (name.bytes)
            g_name.add(channel_is_private(latest->type) ? utf8_literal(" from ") : utf8_literal(" in #")).add(name);
        announce(g_name.view(), false, utf8_literal("MaiCord.Mention"));
    }
    status.mention_total = total;
    status.mentions_primed = true;
}

// Result counts of the quick switcher and search while they are open.
void observe_results(StatusObserver& status, bool primed) noexcept {
    const auto& renderer = g_app.renderer;
    UiAccessNameBuilder summary{};
    u64 quick = 0, search = 0;
    if (renderer.quick_switcher_scene && d3d11_renderer_quick_switcher_visible(renderer) &&
        ui_access_quick_switcher_summary(*renderer.quick_switcher_scene, &summary)) {
        quick = text_hash(summary.view());
        if (primed && quick != status.quick_summary)
            announce(summary.view(), false, utf8_literal("MaiCord.Results"));
    }
    if (renderer.search_scene && d3d11_renderer_search_visible(renderer) &&
        ui_access_search_summary(*renderer.search_scene, &summary)) {
        search = text_hash(summary.view());
        if (primed && search != status.search_summary)
            announce(summary.view(), false, utf8_literal("MaiCord.Results"));
    }
    status.quick_summary = quick;
    status.search_summary = search;
}

void observe_status(u64 now_ms) noexcept {
    auto& status = g_access.status;
    if (g_app.login.visible || !g_app.account_ui) {
        status.primed = status.mentions_primed = false;
        return;
    }
    // A new account session starts from its current state, silently.
    if (status.account_instance != g_app.account.instance) {
        reset_value(&status);
        status.account_instance = g_app.account.instance;
    }
    const bool primed = status.primed;
    observe_connection(status, primed);
    observe_sends(status, primed);
    observe_uploads(status, primed);
    observe_toast(status, primed);
    observe_voice(status, primed);
    observe_results(status, primed);
    observe_mentions(status, now_ms);
    status.primed = true;
}

void publish_events() noexcept {
    const UiAccessTree* tree = access_tree(nullptr);
    if (!tree)
        return;
    UiAccessEventPlan plan{};
    if (!ui_access_plan_events(&g_access.events, *tree, &plan))
        return;
    if (plan.structure_root)
        uia_host_raise_structure(&g_access.host, 0);
    else
        for (u32 index = 0; index < plan.structure_count; ++index)
            uia_host_raise_structure(&g_access.host, event_key(*tree, plan.structures[index]));
    for (u32 index = 0; index < plan.property_count; ++index) {
        const auto& change = plan.properties[index];
        const u64 key = event_key(*tree, change.node);
        uia_host_raise_property(&g_access.host, key, change);
        if (change.property == UiAccessProperty::name && (tree->nodes[change.node].state & ui_access_live))
            uia_host_raise_live_region(&g_access.host, key);
    }
    if (plan.focus_changed && GetFocus() == g_app.window)
        uia_host_raise_focus(&g_access.host, event_key(*tree, plan.focus_node));
    announce_new_message(*tree);
    observe_status(GetTickCount64());
}

bool sample_listening(u64 now_ms) noexcept {
    if (!g_access.listen_sampled_ms || now_ms - g_access.listen_sampled_ms >= kListenSampleMs) {
        g_access.listening = uia_host_listening(g_access.host);
        g_access.listen_sampled_ms = now_ms ? now_ms : 1;
    }
    return g_access.listening;
}

void arm_rebuild_timer(u64 now_ms) noexcept {
    const u64 delay = ui_access_throttle_delay(g_access.throttle, now_ms);
    if (g_app.window && SetTimer(g_app.window, kRebuildTimer, static_cast<UINT>(delay ? delay : 1), nullptr))
        g_access.timer = true;
}

// Input that can move focus or change structure makes the next frame urgent.
bool urgent_message(UINT message) noexcept {
    switch (message) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_UNICHAR:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ACTIVATE:
    case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION:
        return true;
    default:
        return false;
    }
}

} // namespace

bool application_accessibility_window_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) noexcept {
    if (message == kApplicationAccessibilityMessage) {
        g_access.posted = false;
        run_pending();
        *result = 0;
        return true;
    }
    if (g_access.initialized && urgent_message(message))
        ui_access_throttle_mark_urgent(&g_access.throttle);
    if (message == WM_TIMER && wparam == kRebuildTimer) {
        KillTimer(g_app.window, kRebuildTimer);
        g_access.timer = false;
        if (g_access.initialized && !g_access.shut_down && g_access.throttle.deferred) {
            const u64 now = GetTickCount64();
            const auto decision = ui_access_throttle_frame(&g_access.throttle, now, sample_listening(now));
            if (decision == UiAccessThrottleDecision::build)
                publish_events();
            else if (decision == UiAccessThrottleDecision::defer)
                arm_rebuild_timer(now);
        }
        *result = 0;
        return true;
    }
    if (message == WM_DESTROY) {
        application_accessibility_shutdown();
        return false;
    }
    // Provider calls marshaled from UI Automation threads onto this thread.
    if (g_access.initialized && g_access.host.call_message && message == g_access.host.call_message)
        return uia_host_window_message(&g_access.host, message, wparam, lparam, result);
    if (message != WM_GETOBJECT || static_cast<long>(lparam) != -25 /* UiaRootObjectId */ || g_access.shut_down ||
        !g_app.window)
        return false;
    if (!g_access.initialized) {
        const UiaHostCallbacks callbacks{nullptr, access_tree, access_act, access_scale, access_text_bounds};
        if (!uia_host_initialize(&g_access.host, g_app.window, callbacks))
            return false;
        g_access.initialized = true;
    }
    if (!ensure_storage())
        return false;
    g_access.dirty = true;
    return uia_host_window_message(&g_access.host, message, wparam, lparam, result);
}

void application_accessibility_frame_presented() noexcept {
    // A store and a flag test until a UI Automation client first asks.
    g_access.dirty = true;
    if (!g_access.initialized || g_access.shut_down)
        return;
    const u64 now = GetTickCount64();
    switch (ui_access_throttle_frame(&g_access.throttle, now, sample_listening(now))) {
    case UiAccessThrottleDecision::build:
        publish_events();
        return;
    case UiAccessThrottleDecision::defer:
        // Animation frames coalesce; the timer publishes the trailing state.
        if (!g_access.timer)
            arm_rebuild_timer(now);
        return;
    default:
        // Nobody listens: status observations restart silently later.
        g_access.status.primed = g_access.status.mentions_primed = false;
        return;
    }
}

usize application_accessibility_committed_bytes() noexcept {
    return g_access.storage ? g_access.committed_bytes : 0;
}

void application_accessibility_shutdown() noexcept {
    if (g_access.shut_down)
        return;
    g_access.shut_down = true;
    if (g_access.timer && g_app.window)
        KillTimer(g_app.window, kRebuildTimer);
    g_access.timer = false;
    if (g_access.initialized)
        uia_host_shutdown(&g_access.host);
}


} // namespace maicord::app_detail
