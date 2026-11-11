#include "tabs.hpp"
#include "content_input.hpp"
#include "state.hpp"
#include "toast.hpp"
#include "../account/account_ui_runtime.hpp"
#include "../account/friends.hpp"
#include "../chat/application_channel_memory.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/user/private_channel_store.hpp"
#include "maicord/model/user/store.hpp"

namespace maicord::app_detail {
namespace {

constexpr u32 kLabelBytes = kChannelMemoryTabLabelBytes;
constexpr u32 kNone = 0xff;
constexpr u64 kTabsMotionOwner = 0x5441'4253'4d4f'5431ull; // "TABSMOT1"
static_assert(kUiTabCapacity == kChannelMemoryTabCapacity);

// What is saved of a tab, and where it is drawn.
struct Tab : ChannelMemoryTab {
    // The left edge and width as drawn last frame, and as they were when the running transition began.
    float shown_left, shown_width, from_left, from_width;
    // kUiTabMissing, kUiTabOpening or kUiTabClosing.
    u8 flags;
    UiIcon icon;
};

struct Tabs {
    Tab items[kUiTabCapacity];
    u8 count;
    // A drag moves the tab at drag_index with the pointer.
    bool dragging;
    u8 drag_index;
    // The account's saved tabs were adopted: until then nothing changes them or the file.
    bool restored;
    // Opening and closing tabs are under way.
    bool busy;
    float drag_left;
    // The memory profile the tabs belong to.
    u64 profile_id;
    u64 generation;
    // What the labels and the pruning last saw change.
    u32 stamp[4];
    // One transition for opening, closing and the neighbours' slides; two fill groups for the pointer's hover and
    // the active tab.
    UiMotionTransition motion;
    UiMotionHover hover, select;
    UiTabStripView view;
} g_tabs;

bool closing(const Tab& tab) noexcept {
    return (tab.flags & kUiTabClosing) != 0;
}

u32 live_count() noexcept {
    u32 live = 0;
    for (u32 i = 0; i < g_tabs.count; ++i)
        live += !closing(g_tabs.items[i]);
    return live;
}

// The strip index of the channel's tab, kNone without one.
u32 find(u64 channel_id) noexcept {
    for (u32 i = 0; i < g_tabs.count; ++i)
        if (g_tabs.items[i].channel_id == channel_id && !closing(g_tabs.items[i]))
            return i;
    return kNone;
}

// The strip index of the nth tab that is not closing, kNone past the last.
u32 live_at(u32 nth) noexcept {
    for (u32 i = 0; i < g_tabs.count; ++i)
        if (!closing(g_tabs.items[i]) && nth-- == 0)
            return i;
    return kNone;
}

// What a DM or group DM is called: its own name, else the people in it as the sidebar names them.
u32 private_label(const AccountUiRuntime& ui, const ChannelRecord& channel, char* label) noexcept {
    const auto& state = ui.chat_state;
    const auto* record = state.private_channels ? private_channel_store_find(*state.private_channels, channel.id) : nullptr;
    if (!record)
        return 0;
    u32 bytes = 0;
    const auto recipients = private_channel_store_recipients(*state.private_channels, *record);
    for (u32 i = 0; i < recipients.count && bytes < kLabelBytes; ++i) {
        if (recipients[i] == state.self_user_id)
            continue;
        const auto* user = user_store_find(ui.user_store, recipients[i]);
        const auto nickname = private_channel_store_nickname(*state.private_channels, channel.id, recipients[i]);
        const Utf8View name = nickname.bytes ? nickname : user ? user_store_name(ui.user_store, *user) : Utf8View{};
        if (!name.bytes)
            continue;
        if (bytes && bytes + 2 < kLabelBytes) {
            label[bytes++] = ',';
            label[bytes++] = ' ';
        }
        bytes += utf8_copy_truncated(label + bytes, kLabelBytes - bytes, name);
    }
    return bytes;
}

// The tab's guild, label, icon and reachability from the channel as the account knows it now. A channel that is not
// loaded keeps the label it last had.
void describe(Tab* tab) noexcept {
    const AccountUiRuntime& ui = *g_app.account_ui;
    const ChannelRecord* channel = chat_state_store_find_channel(ui.chat_state, tab->channel_id);
    const bool direct = channel && channel_is_private(channel->type);
    // A channel of an unavailable guild is dimmed even while its record is still there.
    const GuildRecord* guild =
        channel && channel->guild_id ? chat_state_store_find_guild(ui.chat_state, channel->guild_id) : nullptr;
    const bool unavailable = channel && channel->guild_id && (!guild || guild->unavailable);
    const bool reachable = application_channel_navigable(tab->channel_id) && !unavailable;
    tab->flags = static_cast<u8>((tab->flags & ~kUiTabMissing) | (reachable ? 0 : kUiTabMissing));
    tab->icon = direct                                  ? UiIcon::direct_message
                : channel && channel_is_thread(channel->type) ? UiIcon::thread
                : channel && channel_is_voice_chat(channel->type) ? UiIcon::speaker
                                                                  : UiIcon::channel;
    if (channel)
        tab->guild_id = channel->guild_id;
    u32 bytes = channel ? utf8_copy_truncated(tab->label, kLabelBytes, chat_state_store_channel_name(ui.chat_state, *channel))
                        : 0;
    if (!bytes && direct)
        bytes = private_label(ui, *channel, tab->label);
    if (!bytes && (channel || !tab->label_bytes))
        bytes = utf8_copy_truncated(tab->label, kLabelBytes,
                                    direct ? utf8_literal("Direct message") : utf8_literal("Channel"));
    if (bytes)
        tab->label_bytes = static_cast<u8>(bytes);
}

void append(u64 channel_id, bool opening) noexcept {
    Tab& tab = g_tabs.items[g_tabs.count++];
    clear_bytes(&tab, sizeof(tab));
    tab.channel_id = channel_id;
    tab.flags = opening ? kUiTabOpening : 0;
    describe(&tab);
}

// The channel tabs, to the file (once the saved ones are adopted).
void persist() noexcept {
    if (!g_tabs.restored)
        return;
    ChannelMemoryTab tabs[kUiTabCapacity];
    u32 count = 0;
    for (u32 i = 0; i < g_tabs.count; ++i)
        if (!closing(g_tabs.items[i]))
            tabs[count++] = g_tabs.items[i];
    application_channel_memory_set_tabs({tabs, count});
}

// A change starts: every tab slides from where it is drawn now.
void begin() noexcept {
    for (u32 i = 0; i < g_tabs.count; ++i) {
        g_tabs.items[i].from_left = g_tabs.items[i].shown_left;
        g_tabs.items[i].from_width = g_tabs.items[i].shown_width;
    }
    g_tabs.busy = true;
    ui_motion_transition_reset(&g_tabs.motion, {kTabsMotionOwner, ++g_tabs.generation}, 0);
    application_request_frame();
}

// Drops the tabs that finished closing; the rest keep their order.
void drop_closing() noexcept {
    u32 kept = 0;
    for (u32 i = 0; i < g_tabs.count; ++i)
        if (!closing(g_tabs.items[i]))
            g_tabs.items[kept++] = g_tabs.items[i];
    clear_bytes(&g_tabs.items[kept], (g_tabs.count - kept) * sizeof(Tab));
    g_tabs.count = static_cast<u8>(kept);
}

void settle() noexcept {
    drop_closing();
    for (u32 i = 0; i < g_tabs.count; ++i)
        g_tabs.items[i].flags &= static_cast<u8>(~kUiTabOpening);
    g_tabs.busy = false;
}

// READY has landed and the remembered selection was placed: the channels the tabs name are known.
bool settled() noexcept {
    const AccountUiRuntime* ui = g_app.account_ui;
    return ui && g_app.account.instance && g_app.account.status.running && !g_app.startup_shell_provisional &&
           ui->chat_state.self_user_id && !ui->chat_state.restore_channel_id && !ui->chat_state.selection_held;
}

// The strip shows, for the account whose memory is loaded (another account's tabs are gone).
bool shown() noexcept {
    const u64 profile = application_channel_memory_profile();
    if (!g_app.preferences.tabs_enabled || !g_app.account_ui || g_app.login.visible || !profile)
        return false;
    if (g_tabs.profile_id != profile) {
        application_tabs_reset();
        g_tabs.profile_id = profile;
    }
    return true;
}

// The tab whose channel is selected, 0 while another view (Friends) shows or no tab holds it.
u64 active_channel() noexcept {
    const u64 selected = g_app.account_ui->chat_state.selected_channel_id;
    return selected && !application_friends_active() && find(selected) != kNone ? selected : 0;
}

// What "+" would pin, 0 without one.
u64 pin_target() noexcept {
    const u64 selected = g_app.account_ui->chat_state.selected_channel_id;
    return selected && !application_friends_active() && application_channel_navigable(selected) ? selected : 0;
}

// Whether the tab's channel is certainly gone: a DM or group DM the account no longer has, a channel missing from
// a guild that is loaded, or a guild the account left. A channel of an unavailable guild may only be late (its
// channels arrive with its GUILD_CREATE), and its tab stays, dimmed, under its last label.
bool gone(const Tab& tab) noexcept {
    const ChatStateStore& state = g_app.account_ui->chat_state;
    if (chat_state_store_find_channel(state, tab.channel_id))
        return false;
    const GuildRecord* guild = tab.guild_id ? chat_state_store_find_guild(state, tab.guild_id) : nullptr;
    return !tab.guild_id || !guild || !guild->unavailable;
}

// Adopts the saved tabs once READY settled, keeps labels current and closes the tabs whose channel is gone.
void follow_account() noexcept {
    const AccountUiRuntime& ui = *g_app.account_ui;
    const bool ready = settled();
    const bool adopt = ready && !g_tabs.restored;
    if (adopt) {
        const ConstSlice<ChannelMemoryTab> saved = application_channel_memory_tabs();
        for (u32 i = 0; i < saved.count && g_tabs.count < kUiTabCapacity; ++i) {
            append(saved[i].channel_id, false);
            static_cast<ChannelMemoryTab&>(g_tabs.items[g_tabs.count - 1]) = saved[i];
        }
        g_tabs.restored = true;
    }
    const u32 stamp[4]{ui.chat_state.channel_mutations, ui.chat_state.guild_mutations,
                       ui.chat_state.private_channels ? ui.chat_state.private_channels->revision : 0,
                       ui.user_store.revision};
    if (!adopt && bytes_equal(stamp, g_tabs.stamp, sizeof(stamp)))
        return;
    copy_bytes(g_tabs.stamp, stamp, sizeof(stamp));
    bool any = false;
    for (u32 i = 0; i < g_tabs.count; ++i)
        any = any || (!closing(g_tabs.items[i]) && gone(g_tabs.items[i]));
    if (any && g_tabs.restored && ready) {
        begin();
        for (u32 i = 0; i < g_tabs.count; ++i)
            if (!closing(g_tabs.items[i]) && gone(g_tabs.items[i]))
                g_tabs.items[i].flags |= kUiTabClosing;
    }
    for (u32 i = 0; i < g_tabs.count; ++i)
        if (!closing(g_tabs.items[i]))
            describe(&g_tabs.items[i]);
    persist();
}

} // namespace

bool application_tabs_visible() noexcept {
    return shown();
}

const UiTabStripView* application_tabs_view(UiTabHit hovered, UiTabHit pressed, u64 now_ms) noexcept {
    if (!shown())
        return nullptr;
    follow_account();
    if (!g_tabs.motion.scope.owner)
        ui_motion_transition_reset(&g_tabs.motion, {kTabsMotionOwner, ++g_tabs.generation}, 1);
    const float done = application_motion_tick(&g_tabs.motion, g_tabs.motion.scope, 1, now_ms, kUiMotionExpand);
    if (g_tabs.busy && done >= 1)
        settle();
    const u64 active = active_channel();
    // The hover fill and the close button's fade ramp for a tab or its X; "+" and "..." hover no tab.
    const bool over_tab = (hovered.kind == UiTabHitKind::tab || hovered.kind == UiTabHitKind::close) &&
                          hovered.index < g_tabs.count;
    const u64 hovered_id = over_tab ? g_tabs.items[hovered.index].channel_id : 0;
    application_motion_hover_tick(&g_tabs.hover, hovered_id, now_ms);
    application_motion_hover_tick(&g_tabs.select, active, now_ms);
    UiTabStripView& view = g_tabs.view;
    for (u32 i = 0; i < g_tabs.count; ++i) {
        const Tab& tab = g_tabs.items[i];
        view.items[i] = {{tab.label, tab.label_bytes},
                         tab.icon,
                         static_cast<u8>(tab.flags | (tab.channel_id == active && !closing(tab) ? kUiTabActive : 0) |
                                         (g_tabs.dragging && g_tabs.drag_index == i ? kUiTabDragging : 0)),
                         tab.from_left,
                         tab.from_width,
                         ui_motion_hover_amount(g_tabs.hover, tab.channel_id),
                         ui_motion_hover_amount(g_tabs.select, tab.channel_id)};
    }
    view.count = g_tabs.count;
    const u64 target = pin_target();
    view.plus_enabled = target && find(target) == kNone;
    view.hovered = hovered.index < g_tabs.count || hovered.kind == UiTabHitKind::plus ||
                           hovered.kind == UiTabHitKind::overflow
                       ? hovered
                       : UiTabHit{};
    view.pressed = pressed.index < g_tabs.count || pressed.kind == UiTabHitKind::plus ||
                           pressed.kind == UiTabHitKind::overflow
                       ? pressed
                       : UiTabHit{};
    view.remaining = g_tabs.busy ? 1 - done : 0;
    view.drag_left = g_tabs.drag_left;
    return &view;
}

void application_tabs_note_layout(const UiTabStripLayout& layout) noexcept {
    for (u32 i = 0; i < g_tabs.count; ++i) {
        g_tabs.items[i].shown_left = layout.left[i];
        g_tabs.items[i].shown_width = layout.width[i];
    }
}

bool application_tabs_open_channel(u64 channel_id, bool background) noexcept {
    if (!shown() || !g_tabs.restored)
        return false;
    if (const u32 at = find(channel_id); at != kNone)
        return background || application_tabs_activate(at);
    if (!application_channel_navigable(channel_id))
        return false;
    if (live_count() == kUiTabCapacity) {
        application_toast_show(UiToastKind::failure, utf8_literal("You can have up to 16 tabs open."));
        return false;
    }
    // Tabs still closing make room at once.
    if (g_tabs.count == kUiTabCapacity)
        drop_closing();
    begin();
    append(channel_id, true);
    persist();
    return background || application_tabs_activate(g_tabs.count - 1);
}

bool application_tabs_pin_current() noexcept {
    const u64 target = shown() && g_tabs.restored ? pin_target() : 0;
    return target && find(target) == kNone && application_tabs_open_channel(target, false);
}

bool application_tabs_activate(u32 index) noexcept {
    return shown() && index < g_tabs.count && !closing(g_tabs.items[index]) &&
           application_navigate_known_message(g_tabs.items[index].channel_id, 0);
}

namespace {
// The tab shown when `index` closes: the next one, else the one before.
u32 neighbour(u32 index) noexcept {
    u32 before = kNone;
    for (u32 i = 0; i < g_tabs.count; ++i) {
        if (i == index || closing(g_tabs.items[i]))
            continue;
        if (i > index)
            return i;
        before = i;
    }
    return before;
}

enum class Close : u8 { one, others, right };

// Closes the tab at `index`, every other tab or the tabs to its right. When the active tab goes, the one shown is the
// neighbour of a single tab, else the tab that stays.
bool close_tabs(u32 index, Close how) noexcept {
    if (!shown() || !g_tabs.restored || index >= g_tabs.count || closing(g_tabs.items[index]))
        return false;
    const u64 active = active_channel();
    const u32 keep = how == Close::one ? neighbour(index) : index;
    bool any = false, lose_active = false;
    for (u32 i = 0; i < g_tabs.count; ++i) {
        Tab& tab = g_tabs.items[i];
        if (closing(tab) || !(how == Close::one ? i == index : how == Close::others ? i != index : i > index))
            continue;
        tab.flags |= kUiTabClosing;
        any = true;
        lose_active = lose_active || tab.channel_id == active;
    }
    if (!any)
        return false;
    begin();
    persist();
    if (lose_active && keep != kNone)
        application_tabs_activate(keep);
    return true;
}
} // namespace

bool application_tabs_close(u32 index) noexcept {
    return close_tabs(index, Close::one);
}

bool application_tabs_close_others(u32 index) noexcept {
    return close_tabs(index, Close::others);
}

bool application_tabs_close_right(u32 index) noexcept {
    return close_tabs(index, Close::right);
}

bool application_tabs_move(u32 from, u32 to) noexcept {
    if (!shown() || from >= g_tabs.count || to >= g_tabs.count || from == to)
        return false;
    begin();
    const Tab moved = g_tabs.items[from];
    if (from < to)
        copy_bytes(&g_tabs.items[from], &g_tabs.items[from + 1], (to - from) * sizeof(Tab));
    else
        copy_bytes(&g_tabs.items[to + 1], &g_tabs.items[to], (from - to) * sizeof(Tab));
    g_tabs.items[to] = moved;
    if (g_tabs.dragging && g_tabs.drag_index == from)
        g_tabs.drag_index = static_cast<u8>(to);
    persist();
    return true;
}

u32 application_tabs_drag_begin(u32 index) noexcept {
    if (!shown() || index >= g_tabs.count || closing(g_tabs.items[index]))
        return kNone;
    const u64 channel_id = g_tabs.items[index].channel_id;
    drop_closing();
    const u32 at = find(channel_id);
    if (at == kNone)
        return kNone;
    g_tabs.dragging = true;
    g_tabs.drag_index = static_cast<u8>(at);
    g_tabs.drag_left = g_tabs.items[at].shown_left;
    return at;
}

void application_tabs_drag_to(float left) noexcept {
    if (g_tabs.dragging && left != g_tabs.drag_left) {
        g_tabs.drag_left = left;
        application_request_frame();
    }
}

void application_tabs_drag_end() noexcept {
    if (!g_tabs.dragging)
        return;
    g_tabs.dragging = false;
    begin();
}

void application_tabs_reset() noexcept {
    clear_bytes(&g_tabs, sizeof(g_tabs));
}

bool application_tabs_shortcut(ShortcutAction action, bool repeated) noexcept {
    if (!shown())
        return false;
    using A = ShortcutAction;
    // Only cycling repeats; the other keys act once.
    if (repeated && action != A::tab_next && action != A::tab_previous)
        return true;
    if (!g_tabs.restored || g_tabs.dragging)
        return true;
    const u32 live = live_count();
    const u64 active = active_channel();
    switch (action) {
    case A::tab_new:
        application_tabs_pin_current();
        break;
    case A::tab_close:
        if (const u32 at = find(active); at != kNone)
            application_tabs_close(at);
        break;
    case A::tab_next:
    case A::tab_previous: {
        if (!live)
            break;
        // Without an active tab the cycle starts at its first or last.
        u32 position = live;
        for (u32 n = 0; n < live; ++n)
            if (g_tabs.items[live_at(n)].channel_id == active)
                position = n;
        const bool forward = action == A::tab_next;
        application_tabs_activate(
            live_at(position == live ? (forward ? 0 : live - 1) : (position + (forward ? 1 : live - 1)) % live));
        break;
    }
    default: {
        const u32 nth = action == A::tab_last ? live - 1 : static_cast<u32>(action) - static_cast<u32>(A::tab_first);
        if (nth < live)
            application_tabs_activate(live_at(nth));
        break;
    }
    }
    return true;
}

bool application_tabs_contains(u64 channel_id) noexcept {
    return shown() && find(channel_id) != kNone;
}

u32 application_tabs_count() noexcept {
    return g_tabs.count;
}

u64 application_tabs_channel(u32 index) noexcept {
    return index < g_tabs.count && !closing(g_tabs.items[index]) ? g_tabs.items[index].channel_id : 0;
}

Utf8View application_tabs_label(u32 index) noexcept {
    return index < g_tabs.count ? Utf8View{g_tabs.items[index].label, g_tabs.items[index].label_bytes} : Utf8View{};
}

UiIcon application_tabs_icon(u32 index) noexcept {
    return index < g_tabs.count ? g_tabs.items[index].icon : UiIcon::channel;
}

bool application_tabs_menu_action(u32 action, u64 channel_id) noexcept {
    const u32 at = find(channel_id);
    switch (action) {
    case popup_action_tab_close:
        return application_tabs_close(at);
    case popup_action_tab_close_others:
        return application_tabs_close_others(at);
    case popup_action_tab_close_right:
        return application_tabs_close_right(at);
    default:
        return false;
    }
}

} // namespace maicord::app_detail
