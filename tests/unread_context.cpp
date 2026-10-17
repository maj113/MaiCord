#include "maicord/ui/unread_context.hpp"
#include "maicord/ui/message_context_controller.hpp"
#include "maicord/ui/channel_context_policy.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/app/message_timeline.hpp"
#include "maicord/model/message_store.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/ui/message_scene.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/virtual_list.hpp"

namespace maicord {
namespace {
struct UnreadFixture {
    ChatStateStore state; GuildRecord guilds[2]; u16 guild_order[2];
    ChannelRecord channels[3]; u16 channel_order[3]; u8 names[2048];
    MessageStore store; MessageRecord records[2]; u32 order[2], heights[2]; u64 fenwick[2]; u8 text[2048];
    UiVariableList list; MessageTimeline timeline; MessageScene scene;
    MessageSceneRowWidget message_rows[2]; UiWidget message_widgets[64]; UiLayoutNode message_nodes[64];
    NavigationView view; ChannelViewRow channel_rows[2];
    ShellNavigationScene navigation; ShellNavigationRowWidget navigation_rows[2];
    UiWidget navigation_widgets[12]; UiLayoutNode navigation_nodes[12];
    PopupScene popup; PopupItem items[24]; PopupRowWidget popup_rows[24];
    UiWidget popup_widgets[2 + 24 * kPopupWidgetsPerItem]; UiLayoutNode popup_nodes[2 + 24 * kPopupWidgetsPerItem];
    u32 instance;
    UnreadContextSource source() const noexcept {
        return {1, 2, 3, &instance, &state, &store, &scene, &view, &navigation, &popup, true};
    }
    ChannelRecord* channel(u64 id) noexcept { return const_cast<ChannelRecord*>(chat_state_store_find_channel(state, id)); }
    MessageRecord* message(u64 id = 900) noexcept {
        const auto found = message_store_find(store, id);
        return found.ok() ? const_cast<MessageRecord*>(message_store_resolve(store, found.handle)) : nullptr;
    }
    bool initialize() noexcept {
        if (!chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 3}, {channel_order, 3}, {names, sizeof(names)}) ||
            !chat_state_store_upsert_guild(&state, {10, utf8_literal("Server"), GuildMutationKind::create, 3}).ok()) return false;
        state.self_user_id = 3;
        for (u32 i = 0; i < 2; ++i) {
            ChannelInput input{}; input.id = 70 + i; input.guild_id = i ? 10 : 0; input.type = i ? 0 : 1;
            input.name = utf8_literal("Conversation"); input.last_message_id = 901; input.last_message_known = true;
            if (!chat_state_store_upsert_channel(&state, input).ok()) return false;
            auto* c = channel(input.id); c->permissions_resolved = true;
            c->effective_permissions = kPermissionViewChannel | kPermissionReadMessageHistory;
            c->actions = channel_action_view | channel_action_read_history;
            channel_rows[i] = {}; channel_rows[i].id = c->id; channel_rows[i].type = c->type;
            channel_rows[i].selectable = true; channel_rows[i].generation = c->generation; channel_rows[i].revision = c->revision;
        }
        state.selected_channel_id = 70;
        if (!message_store_initialize(&store, {records, 2}, {order, 2}, {text, sizeof(text)}) ||
            !ui_variable_list_initialize(&list, {heights, 2}, {fenwick, 2}, 0, 200) ||
            !message_timeline_initialize(&timeline, &store, &list, 100, 200, true)) return false;
        GatewayMessageEvent event{}; event.id = 900; event.channel_id = 70; event.author_id = 3;
        event.content = utf8_literal("Native unread target"); event.timestamp_ms = 1000;
        event.fields = gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_author_id |
            gateway_message_field_content | gateway_message_field_timestamp;
        if (!message_timeline_apply_created(&timeline, event).ok() ||
            !message_scene_initialize(&scene, &timeline, {message_rows, 2}, {message_widgets, 64}, {message_nodes, 64}) ||
            !message_scene_update(&scene, {300, 40, 900, 500}).ok()) return false;
        view.channel_rows = channel_rows; view.channel_count = view.channel_capacity = 2; view.channel_revision = 1;
        view.selected_channel_id = 70;
        return shell_navigation_scene_initialize(&navigation, {navigation_rows, 2}, {navigation_widgets, 12}, {navigation_nodes, 12}) &&
            shell_navigation_scene_update(&navigation, &view, {70, 40, 290, 500}, 0).ok() &&
            popup_scene_initialize(&popup, {items, 24}, {popup_rows, 24}, {popup_widgets, 2 + 24 * kPopupWidgetsPerItem},
                {popup_nodes, 2 + 24 * kPopupWidgetsPerItem});
    }
    bool open(u64 id) noexcept {
        const PopupItem item{utf8_literal("Mark unread"), 99, popup_item_enabled, 0};
        return popup_scene_open(&popup, PopupKind::context_menu, id, {&item, 1}, {400, 100, 401, 101}, {0, 0, 1000, 800}, 224).ok();
    }
} unread_fixture{};
} // namespace
void test_unread_context(void (*check)(bool, const char*) noexcept) noexcept {
    auto& f = unread_fixture;
    check(f.initialize(), "unread context actual state timeline message row navigation and popup initialize");
    if (!f.scene.active || !f.navigation.active) return;
    auto source = f.source(); UnreadContextTarget target{};
    check(unread_context_available(source, UnreadContextKind::message, 900) && f.open(900) &&
        unread_context_capture(source, UnreadContextKind::message, 900, &target, 99) && unread_context_current(target, source),
        "native message context captures a current authoritative visible row and account owner");
    bool changed = false;
    check(popup_scene_move_selection(&f.popup, 1, &changed) && unread_context_current(target, source),
        "keyboard popup selection does not retire a current unread target");
    auto retired = source; ++retired.account_serial;
    check(!unread_context_current(target, retired), "unread context rejects a retired account epoch");
    retired = source; ++retired.profile_key;
    check(!unread_context_current(target, retired), "unread context rejects another profile");
    retired = source; ++retired.self_user_id;
    check(!unread_context_current(target, retired), "unread context rejects another self user");
    u32 another_instance = 0; retired = source; retired.account_instance = &another_instance;
    check(!unread_context_current(target, retired), "unread context rejects replacement account instance");
    retired = source; retired.available = false;
    check(!unread_context_current(target, retired), "unread context rejects retired host or account API availability");
    auto replacement_state = f.state; retired = source; retired.state = &replacement_state;
    check(!unread_context_current(target, retired), "unread context rejects replacement model owner even under identical account IDs");
    auto replacement_store = f.store; retired = source; retired.messages = &replacement_store;
    check(!unread_context_current(target, retired), "unread context rejects replacement message owner under an identical snapshot");
    UnreadContextTarget absent_action{};
    check(!unread_context_capture(source, UnreadContextKind::message, 900, &absent_action, 100) && !absent_action.id,
        "unread cannot capture an action absent from the published native menu");
    f.popup.items[0].flags = 0;
    check(!unread_context_current(target, source), "unread activation rejects a disabled captured menu action");
    f.popup.items[0].flags = popup_item_enabled; f.popup.items[0].action = 100;
    check(!unread_context_current(target, source), "unread activation cannot substitute a retry or unrelated action for its captured ID");
    f.popup.items[0].action = 99;
    auto* message = f.message(); ++message->revision;
    check(!unread_context_current(target, source), "unread context rejects changed authoritative message revision");
    --message->revision; ++message->generation;
    check(!unread_context_current(target, source), "unread context rejects reused message slot generation");
    --message->generation; message->delivery = MessageDeliveryState::pending;
    check(!unread_context_current(target, source) && !unread_context_available(source, UnreadContextKind::message, 900),
        "unread context never accepts a pending local send");
    message->delivery = MessageDeliveryState::authoritative;
    const auto row_count = f.scene.count; f.scene.count = 0;
    check(!unread_context_current(target, source), "unread context rejects a target removed from visible message rows");
    f.scene.count = row_count;
    const auto row_handle = f.message_rows[0].widget; f.message_rows[0].widget.generation = 0;
    check(!unread_context_current(target, source), "unread context rejects retired message widget handles");
    f.message_rows[0].widget = row_handle;
    f.state.selected_channel_id = 71;
    check(!unread_context_current(target, source), "unread message cannot activate after channel navigation");
    f.state.selected_channel_id = 70;
    auto* channel = f.channel(70); const u32 actions = channel->actions;
    channel->actions &= ~channel_action_read_history;
    check(!unread_context_current(target, source), "unread context rechecks current readable-history permission");
    channel->actions = actions; ++channel->generation;
    check(!unread_context_current(target, source), "unread context rejects reused containing channel generation");
    --channel->generation; ++channel->revision;
    check(!unread_context_current(target, source), "unread context rejects changed containing channel revision");
    --channel->revision;
    check(f.open(900) && !unread_context_current(target, source), "reopening an identical popup expires the previous unread capture");
    check(f.open(70) && unread_context_capture(source, UnreadContextKind::channel, 70, &target, 99) &&
        unread_context_current(target, source), "native channel context captures a current projected readable conversation");
    ++channel->generation;
    check(!unread_context_available(source, UnreadContextKind::channel, 70),
        "unread channel cannot first capture an old navigation projection after model slot reuse");
    --channel->generation;
    f.channel_rows[0].selectable = false;
    check(!unread_context_current(target, source), "unread channel rejects a no-longer-selectable visible row");
    f.channel_rows[0].selectable = true; ++f.view.channel_revision;
    check(!unread_context_current(target, source), "unread channel rejects retired navigation source revision");
    check(shell_navigation_scene_update(&f.navigation, &f.view, {70, 40, 290, 500}, 0).ok() &&
        !unread_context_current(target, source) && unread_context_capture(source, UnreadContextKind::channel, 70, &target, 99),
        "fresh navigation can recapture but cannot revive the prior row identity");
    const u64 last_message = channel->last_message_id; channel->last_message_id = 0;
    check(!unread_context_current(target, source), "empty channel never exposes a synthetic unread cursor");
    channel->last_message_id = last_message; channel->flags |= kChannelObfuscatedFlag;
    check(!unread_context_current(target, source), "unread context rejects an obfuscated conversation");
    channel->flags &= ~kChannelObfuscatedFlag;
    check(f.open(71) && unread_context_capture(source, UnreadContextKind::channel, 71, &target, 99),
        "native guild channel uses the same bounded unread admission helper");
    auto* guild_channel = f.channel(71); guild_channel->permissions_resolved = false;
    check(!unread_context_current(target, source), "unread guild channel rejects unresolved permissions despite stale action bits");
    guild_channel->permissions_resolved = true; guild_channel->effective_permissions &= ~kPermissionReadMessageHistory;
    check(!unread_context_current(target, source), "unread guild channel rejects stale readable action without effective permission");
    guild_channel->effective_permissions |= kPermissionReadMessageHistory;
    auto* guild = const_cast<GuildRecord*>(chat_state_store_find_guild(f.state, 10)); guild->unavailable = true;
    check(!unread_context_current(target, source), "unread guild channel rejects an unavailable server");
    // Voice channels carry chat (text in voice); a category never holds messages.
    guild->unavailable = false; guild_channel->type = 4;
    check(!unread_context_current(target, source), "category container cannot acquire a message unread action");
    guild_channel->type = 0;
    popup_scene_close(&f.popup);
    check(!unread_context_current(target, source), "dismissed popup cannot dispatch a retained unread action");
}
void test_message_unread_context_menu(void (*check)(bool, const char*) noexcept) noexcept {
    static_assert(static_cast<u32>(MessageContextAction::forward_message) == 18);
    static_assert(static_cast<u32>(MessageContextAction::mark_unread) == 19);
    MessageRecord message{}; message.id = 900; message.channel_id = 70; message.author_id = 3;
    MessageContextMenuModel menu{};
    const auto legacy = message_context_menu_build(&menu, message, true, true, true, nullptr, true, true, true);
    bool absent = true;
    for (u32 i = 0; i < menu.count; ++i) absent &= menu.items[i].action != static_cast<u32>(MessageContextAction::mark_unread);
    check(legacy.ok() && menu.count == 15 && absent, "legacy menu callers retain fifteen actions without opting into unread");
    check(message_context_menu_build(&menu, message, true, true, true, nullptr, true, true, true, true).ok() && menu.count == 16,
        "full authoritative message menu retains all actions plus Mark unread without truncation");
    u32 unread_index = ~0u;
    for (u32 i = 0; i < menu.count; ++i) if (menu.items[i].action == static_cast<u32>(MessageContextAction::mark_unread)) unread_index = i;
    check(unread_index < menu.count && utf8_equal(menu.items[unread_index].label, utf8_literal("Mark unread")) &&
        menu.items[unread_index].flags == popup_item_enabled,
        "message unread uses a real native enabled menu action");
    const MessageDeliveryState deliveries[]{MessageDeliveryState::pending, MessageDeliveryState::failed};
    for (const auto delivery : deliveries) {
        message.delivery = delivery; message_context_menu_build(&menu, message, true, true, true, nullptr, true, true, true, true);
        absent = true; for (u32 i = 0; i < menu.count; ++i) absent &= menu.items[i].action != static_cast<u32>(MessageContextAction::mark_unread);
        check(absent, "local pending and failed sends never expose Mark unread even with a permissive caller");
    }
    message.delivery = MessageDeliveryState::authoritative;
    auto& f = unread_fixture; MessageContextController controller{};
    check(f.initialize(), "unread menu initializes its own native popup fixture independently");
    if (!f.scene.active || !f.navigation.active) return;
    PopupItem extras[8]{}; for (u32 i = 0; i < 8; ++i) extras[i] = {utf8_literal("Plugin action"), 1000 + i, popup_item_enabled, 0};
    check(message_context_controller_open(&controller, &f.popup, message, true, true, true, nullptr,
        {400, 50, 401, 51}, {0, 0, 1000, 1200}, true, {extras, 8}, true, true, true).ok() && f.popup.item_count == 24,
        "actual popup fits sixteen builtins and eight plugin rows with full unread policy");
    unread_index = ~0u;
    for (u32 i = 0; i < f.popup.item_count; ++i) if (f.popup.items[i].action == static_cast<u32>(MessageContextAction::mark_unread)) unread_index = i;
    bool changed = false;
    check(unread_index < f.popup.item_count && popup_scene_select_index(&f.popup, unread_index, &changed) &&
        popup_scene_selected_action(f.popup) == static_cast<u32>(MessageContextAction::mark_unread),
        "native keyboard selection resolves the appended unread action under full plugin capacity");
    PopupItemLayout layout{};
    check(popup_scene_resolve_item_layout(f.popup, unread_index, &layout) && layout.visible &&
        popup_scene_hit_test(f.popup, (layout.visible_rectangle.left + layout.visible_rectangle.right) * .5f,
            (layout.visible_rectangle.top + layout.visible_rectangle.bottom) * .5f).action == static_cast<u32>(MessageContextAction::mark_unread),
        "native pointer hit resolves the same unread row that keyboard focus selects");
    const auto prior_revision = f.popup.revision;
    const PopupItem collision{utf8_literal("Impersonated"), static_cast<u32>(MessageContextAction::mark_unread), popup_item_enabled, 0};
    check(message_context_controller_open(&controller, &f.popup, message, false, false, false, nullptr,
        {400, 50, 401, 51}, {0, 0, 1000, 1200}, false, {&collision, 1}).error == MessageContextControllerError::invalid_input &&
        f.popup.revision == prior_revision && f.popup.item_count == 24,
        "plugin cannot impersonate absent unread namespace and failed open preserves current native popup");
    popup_scene_close(&f.popup);
}
} // namespace maicord
