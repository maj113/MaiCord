#include "tabs.hpp"
#include "content_input.hpp"
#include "state.hpp"
#include "toast.hpp"
#include "../account/account_ui_runtime.hpp"
#include "../account/friends.hpp"
#include "../chat/application_channel_memory.hpp"
#include "../media/image_viewer.hpp"
#include "../media/media_page.hpp"
#include "../media/video.hpp"

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
// A media tab's key: its page's number under a bit no channel id has.
constexpr u64 kPageKey = 1ull << 63;
static_assert(kUiTabCapacity >= kMediaPageCapacity);

// What is saved of a tab, and where it is drawn. A media tab names a page (session only: never saved) instead of a
// channel, and the channel its media came from; its label is the page's title as it was opened.
struct Tab : ChannelMemoryTab {
    // The left edge and width as drawn last frame, and as they were when the running transition began.
    float shown_left, shown_width, from_left, from_width;
    // kUiTabMissing, kUiTabUnread, kUiTabOpening or kUiTabClosing.
    u8 flags;
    UiIcon icon;
    // The media page the tab shows, 0 for a channel tab.
    u8 page;
};

struct Tabs {
    Tab items[kUiTabCapacity];
    u8 count;
    // A drag moves the tab with this key with the pointer.
    bool dragging;
    u64 drag_key;
    // The account's saved tabs were adopted: until then nothing changes them or the file.
    bool restored;
    // Opening and closing tabs are under way.
    bool busy;
    float drag_left;
    // The memory profile the tabs belong to.
    u64 profile_id;
    u64 generation;
    // What the labels and the pruning last saw change.
    u32 stamp[5];
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

// A tab's identity: the channel of a channel tab, a page key for a media tab.
u64 key(const Tab& tab) noexcept {
    return tab.page ? kPageKey | tab.page : tab.channel_id;
}

// A channel tab that is not closing.
bool live_channel(const Tab& tab) noexcept {
    return !tab.page && !closing(tab);
}

// The strip index of the tab with this key (a channel id or a page key), kNone without one.
u32 find(u64 id) noexcept {
    for (u32 i = 0; i < g_tabs.count; ++i)
        if (key(g_tabs.items[i]) == id && !closing(g_tabs.items[i]))
            return i;
    return kNone;
}

// How many media tabs are open (not closing).
u32 media_count() noexcept {
    u32 pages = 0;
    for (u32 i = 0; i < g_tabs.count; ++i)
        pages += g_tabs.items[i].page && !closing(g_tabs.items[i]);
    return pages;
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
    const bool unread = reachable && channel && chat_state_store_channel_unread(ui.chat_state, *channel);
    tab->flags = static_cast<u8>((tab->flags & ~(kUiTabMissing | kUiTabUnread)) | (reachable ? 0 : kUiTabMissing) |
                                 (unread ? kUiTabUnread : 0));
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

// A media tab's icon and dimming follow its page: a page that cannot be loaded again is dimmed; a page that owns the
// player has a pause/play button for an icon, audio_lines while it plays.
void describe_page(Tab* tab) noexcept {
    const bool missing = application_media_page_missing(tab->page);
    const VideoPagePlayback playback = application_video_page_playback(tab->page);
    tab->flags = static_cast<u8>((tab->flags & ~(kUiTabMissing | kUiTabPlaying)) | (missing ? kUiTabMissing : 0) |
                                 (playback != VideoPagePlayback::none ? kUiTabPlaying : 0));
    switch (application_media_page_kind(tab->page)) {
    case MediaPageKind::video:
        tab->icon = UiIcon::video;
        break;
    case MediaPageKind::audio:
        tab->icon = UiIcon::music;
        break;
    default:
        tab->icon = missing ? UiIcon::image_off : UiIcon::image;
        break;
    }
    if (playback == VideoPagePlayback::playing)
        tab->icon = UiIcon::audio_lines;
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
        if (live_channel(g_tabs.items[i]))
            tabs[count++] = g_tabs.items[i];
    application_channel_memory_set_tabs({tabs, count});
}

// Drops the media tabs (`media`: the strip is hidden or another account's) or the tabs that finished closing; the
// rest keep their order.
void drop(bool media) noexcept {
    u32 kept = 0;
    for (u32 i = 0; i < g_tabs.count; ++i)
        if (!(media ? g_tabs.items[i].page != 0 : closing(g_tabs.items[i])))
            g_tabs.items[kept++] = g_tabs.items[i];
    clear_bytes(&g_tabs.items[kept], (g_tabs.count - kept) * sizeof(Tab));
    g_tabs.count = static_cast<u8>(kept);
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

void settle() noexcept {
    drop(false);
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

// The strip shows, for the account whose memory is loaded (another account's tabs are gone). While it does not, no
// media page stays open: pages live only under their tabs.
bool shown() noexcept {
    const u64 profile = application_channel_memory_profile();
    if (!g_app.preferences.tabs_enabled || !g_app.account_ui || g_app.login.visible || !profile) {
        if (media_count()) {
            drop(true);
            application_media_page_reset();
        }
        return false;
    }
    if (g_tabs.profile_id != profile) {
        application_tabs_reset();
        g_tabs.profile_id = profile;
    }
    return true;
}

// The key of the tab that shows: the media page in the content area, else the tab whose channel is selected, 0 while
// another view (Friends) shows or no tab holds it.
u64 active_key() noexcept {
    if (const u8 page = application_media_page_shown())
        return find(kPageKey | page) != kNone ? kPageKey | page : 0;
    const u64 selected = g_app.account_ui->chat_state.selected_channel_id;
    return selected && !application_friends_active() && find(selected) != kNone ? selected : 0;
}

// What "+" would pin, 0 without one.
u64 pin_target() noexcept {
    const u64 selected = g_app.account_ui->chat_state.selected_channel_id;
    return selected && !application_friends_active() && application_channel_navigable(selected) ? selected : 0;
}

// Whether "+" is on: a message viewer to turn into a page (`viewer`), else a channel without a tab. A page that shows
// is a tab already.
bool can_pin(bool* viewer) noexcept {
    *viewer = false;
    if (application_media_page_shown())
        return false;
    *viewer = application_image_viewer_page_source(nullptr);
    const u64 target = pin_target();
    return *viewer || (target && find(target) == kNone);
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
    const u32 stamp[5]{ui.chat_state.channel_mutations, ui.chat_state.guild_mutations,
                       ui.chat_state.private_channels ? ui.chat_state.private_channels->revision : 0,
                       ui.user_store.revision, ui.chat_state.activity_mutations};
    if (!adopt && bytes_equal(stamp, g_tabs.stamp, sizeof(stamp)))
        return;
    copy_bytes(g_tabs.stamp, stamp, sizeof(stamp));
    bool any = false;
    for (u32 i = 0; i < g_tabs.count; ++i)
        any = any || (live_channel(g_tabs.items[i]) && gone(g_tabs.items[i]));
    if (any && g_tabs.restored && ready) {
        begin();
        for (u32 i = 0; i < g_tabs.count; ++i)
            if (live_channel(g_tabs.items[i]) && gone(g_tabs.items[i]))
                g_tabs.items[i].flags |= kUiTabClosing;
    }
    for (u32 i = 0; i < g_tabs.count; ++i)
        if (live_channel(g_tabs.items[i]))
            describe(&g_tabs.items[i]);
    persist();
}

// The hovered tab's tooltip: a channel's name with its server ("#channel · Server" for a text channel), a media tab's
// title with the line under it. The text stays valid until the next call; an empty view means the label says it all.
char g_tip[240];
Utf8View tooltip(const Tab& tab) noexcept {
    Utf8View detail{};
    if (tab.page) {
        detail = application_media_page_source(tab.page);
    } else if (tab.guild_id) {
        const ChatStateStore& state = g_app.account_ui->chat_state;
        if (const GuildRecord* guild = chat_state_store_find_guild(state, tab.guild_id))
            detail = chat_state_store_guild_name(state, *guild);
    }
    constexpr Utf8View separator = utf8_literal(" \xC2\xB7 ");
    if (!detail.bytes)
        return {};
    // A media tab's label is cut to fit the strip; its tooltip names the title whole.
    const Utf8View title = tab.page ? application_media_page_title(tab.page) : Utf8View{};
    g_tip[0] = '#';
    u32 bytes = !tab.page && tab.icon == UiIcon::channel;
    bytes += utf8_copy_truncated(g_tip + bytes, sizeof(g_tip) - separator.bytes - bytes,
                                 title.bytes ? title : Utf8View{tab.label, tab.label_bytes});
    copy_bytes(g_tip + bytes, separator.data, separator.bytes);
    bytes += separator.bytes;
    bytes += utf8_copy_truncated(g_tip + bytes, sizeof(g_tip) - bytes, detail);
    return {g_tip, bytes};
}

} // namespace

bool application_tabs_visible() noexcept {
    return shown();
}

const UiTabStripView* application_tabs_view(UiTabHit hovered, UiTabHit pressed, u64 now_ms) noexcept {
    if (!shown())
        return nullptr;
    follow_account();
    for (u32 i = 0; i < g_tabs.count; ++i)
        if (g_tabs.items[i].page && !closing(g_tabs.items[i]))
            describe_page(&g_tabs.items[i]);
    if (!g_tabs.motion.scope.owner)
        ui_motion_transition_reset(&g_tabs.motion, {kTabsMotionOwner, ++g_tabs.generation}, 1);
    const float done = application_motion_tick(&g_tabs.motion, g_tabs.motion.scope, 1, now_ms, kUiMotionExpand);
    if (g_tabs.busy && done >= 1)
        settle();
    // The tab the pointer holds in a drag is the hovered and the pressed one, wherever the drag moved it.
    const u32 dragged = g_tabs.dragging ? find(g_tabs.drag_key) : kNone;
    if (dragged != kNone)
        hovered = pressed = {UiTabHitKind::tab, static_cast<u8>(dragged)};
    const u64 active = active_key();
    // The hover fill and the close button's fade ramp for a tab, its X or its play button; "+" and "..." hover no tab.
    const bool over_tab = (hovered.kind == UiTabHitKind::tab || hovered.kind == UiTabHitKind::close ||
                           hovered.kind == UiTabHitKind::icon) &&
                          hovered.index < g_tabs.count;
    const u64 hovered_id = over_tab ? key(g_tabs.items[hovered.index]) : 0;
    application_motion_hover_tick(&g_tabs.hover, hovered_id, now_ms);
    application_motion_hover_tick(&g_tabs.select, active, now_ms);
    UiTabStripView& view = g_tabs.view;
    for (u32 i = 0; i < g_tabs.count; ++i) {
        const Tab& tab = g_tabs.items[i];
        const bool shown_tab = key(tab) == active && !closing(tab);
        view.items[i] = {{tab.label, tab.label_bytes},
                         tab.icon,
                         // The tab that shows is read: no dot on it.
                         static_cast<u8>((tab.flags & ~(shown_tab ? kUiTabUnread : 0)) | (shown_tab ? kUiTabActive : 0) |
                                         (i == dragged ? kUiTabDragging : 0)),
                         tab.from_left,
                         tab.from_width,
                         ui_motion_hover_amount(g_tabs.hover, key(tab)),
                         ui_motion_hover_amount(g_tabs.select, key(tab)),
                         hovered.kind == UiTabHitKind::tab && hovered.index == i && !closing(tab) ? tooltip(tab)
                                                                                                  : Utf8View{}};
    }
    view.count = g_tabs.count;
    view.plus_enabled = can_pin(&view.plus_page);
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
        drop(false);
    begin();
    append(channel_id, true);
    persist();
    return background || application_tabs_activate(g_tabs.count - 1);
}

bool application_tabs_open_media(const MediaPageSource& source, bool background) noexcept {
    if (!shown() || !g_tabs.restored)
        return false;
    // The store hands the page of the same media back: its tab shows instead of a second one for the page.
    const u8 page = application_media_page_open(source);
    if (const u32 at = page ? find(kPageKey | page) : kNone; at != kNone)
        return background || application_tabs_activate(at);
    const bool full = live_count() == kUiTabCapacity;
    if (!page || full) {
        if (page)
            application_media_page_close(page);
        if (full || media_count() == kMediaPageCapacity)
            application_toast_show(UiToastKind::failure, full ? utf8_literal("You can have up to 16 tabs open.")
                                                              : utf8_literal("You can have up to 8 media tabs open."));
        return false;
    }
    // Tabs still closing make room at once.
    if (g_tabs.count == kUiTabCapacity)
        drop(false);
    begin();
    Tab& tab = g_tabs.items[g_tabs.count++];
    clear_bytes(&tab, sizeof(tab));
    tab.page = page;
    tab.channel_id = source.channel_id;
    tab.flags = kUiTabOpening;
    describe_page(&tab);
    // Both producers (application_image_viewer_page_source, _item_page_source) title an untitled item by its kind.
    tab.label_bytes = static_cast<u8>(utf8_copy_truncated(tab.label, kLabelBytes, source.title));
    return background || application_tabs_activate(g_tabs.count - 1);
}

bool application_tabs_pin_current() noexcept {
    if (!shown() || !g_tabs.restored || application_media_page_shown())
        return false;
    // An open message viewer becomes a page; showing the page closes the viewer, after the page copied what it shows.
    MediaPageSource source;
    if (application_image_viewer_page_source(&source))
        return application_tabs_open_media(source, false);
    const u64 target = pin_target();
    return target && find(target) == kNone && application_tabs_open_channel(target, false);
}

bool application_tabs_activate(u32 index) noexcept {
    if (!shown() || index >= g_tabs.count || closing(g_tabs.items[index]))
        return false;
    const Tab& tab = g_tabs.items[index];
    if (!tab.page)
        return application_navigate_known_message(tab.channel_id, 0);
    // A page shows with the channel it came from in the sidebar and the member list (when the account still reaches
    // it); navigating hides the page that shows, so this one shows after it.
    if (application_media_page_shown() != tab.page && tab.channel_id)
        application_navigate_known_message(tab.channel_id, 0);
    return application_media_page_show(tab.page);
}

void application_tabs_note_navigation() noexcept {
    application_media_page_hide();
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

// Closes the tab at `index`, every other tab or the tabs to its right, and the pages of the media tabs among them.
// When the active channel tab goes, the one shown is the neighbour of a single tab, else the tab that stays; a page
// that closes just shows the channel under it again.
bool close_tabs(u32 index, Close how) noexcept {
    if (!shown() || !g_tabs.restored || index >= g_tabs.count || closing(g_tabs.items[index]))
        return false;
    const u64 active = active_key();
    const u32 keep = how == Close::one ? neighbour(index) : index;
    bool any = false, lose_active = false;
    for (u32 i = 0; i < g_tabs.count; ++i) {
        Tab& tab = g_tabs.items[i];
        if (closing(tab) || !(how == Close::one ? i == index : how == Close::others ? i != index : i > index))
            continue;
        tab.flags |= kUiTabClosing;
        any = true;
        lose_active = lose_active || key(tab) == active;
        if (tab.page)
            application_media_page_close(tab.page);
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
    persist();
    return true;
}

u32 application_tabs_drag_begin(u32 index) noexcept {
    if (!shown() || index >= g_tabs.count || closing(g_tabs.items[index]))
        return kNone;
    const u64 id = key(g_tabs.items[index]);
    drop(false);
    const u32 at = find(id);
    if (at == kNone)
        return kNone;
    g_tabs.dragging = true;
    g_tabs.drag_key = id;
    g_tabs.drag_left = g_tabs.items[at].shown_left;
    return at;
}

u32 application_tabs_drag_index() noexcept {
    return g_tabs.dragging ? find(g_tabs.drag_key) : kNone;
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
    application_media_page_reset();
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
    const u64 active = active_key();
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
            if (key(g_tabs.items[live_at(n)]) == active)
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

u64 application_tabs_key(u32 index) noexcept {
    return index < g_tabs.count && !closing(g_tabs.items[index]) ? key(g_tabs.items[index]) : 0;
}

bool application_tabs_active(u32 index) noexcept {
    return index < g_tabs.count && g_app.account_ui && !closing(g_tabs.items[index]) &&
           key(g_tabs.items[index]) == active_key();
}

Utf8View application_tabs_label(u32 index) noexcept {
    return index < g_tabs.count ? Utf8View{g_tabs.items[index].label, g_tabs.items[index].label_bytes} : Utf8View{};
}

UiIcon application_tabs_icon(u32 index) noexcept {
    return index < g_tabs.count ? g_tabs.items[index].icon : UiIcon::channel;
}

bool application_tabs_menu_action(u32 action, u64 id) noexcept {
    // The menu's rows are Close, Close other tabs and Close tabs to the right, in Close's order.
    const u32 how = action - popup_action_tab_close;
    return how <= static_cast<u32>(Close::right) && close_tabs(find(id), static_cast<Close>(how));
}

bool application_tabs_close_page(u8 id) noexcept {
    const u32 at = shown() ? find(kPageKey | id) : kNone;
    return at != kNone && close_tabs(at, Close::one);
}

bool application_tabs_toggle_playback(u32 index) noexcept {
    return shown() && index < g_tabs.count && !closing(g_tabs.items[index]) && g_tabs.items[index].page &&
           application_video_page_toggle(g_tabs.items[index].page);
}

} // namespace maicord::app_detail
