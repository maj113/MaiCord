#include "application_inbox.hpp"
#include "application_cursor.hpp"
#include "maicord/ui/scene_cursor.hpp"
#include "application_account_presentation.hpp"
#include "application_composer_text_input.hpp"
#include "application_content_input.hpp"
#include "application_read_state.hpp"
#include "application_scoped_read.hpp"
#include "application_scrolling.hpp"
#include "application_settings.hpp"
#include "application_state.hpp"
#include "maicord/protocol/message_card_excerpt.hpp"
#include "maicord/protocol/user_guild_settings.hpp"
#include "maicord/ui/timestamp_format.hpp"
#include "maicord/ui/inbox_scene.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"

namespace maicord::app_detail {
namespace {
constexpr u32 kMentionCapacity = 200;
constexpr u32 kUnreadCapacity = kUiInboxMaximumRows;
struct MentionRow {
    u64 message_id, channel_id, guild_id, author_id;
    char author[128], excerpt[480], detail[192], time[64];
    u16 author_bytes, excerpt_bytes, detail_bytes, time_bytes;
    // Copied from the decoded page: the response body is transient.
    char avatar[kGatewayMessageAvatarHashMaximumBytes];
    u8 avatar_bytes;
};
struct UnreadRow {
    UiInboxRowKind kind;
    u64 guild_id, channel_id;
    u32 mentions;
    char title[160];
    u16 title_bytes;
    bool muted;
};
struct Inbox {
    UiInboxScene scene;
    UiInboxSource source;
    UiInboxContent content;
    MentionRow mentions[kMentionCapacity];
    UnreadRow unreads[kUnreadCapacity];
    GatewayMessageEvent decoded[kMentionsPageLimit];
    MentionsQuery submitted;
    UiInboxTarget pressed;
    u64 account_scope, request_id, retry_at, unread_key, opens;
    u32 mention_count, unread_count, revision;
    float thumb_grab;
    u8 attempts;
    bool open, queued, has_more, failed, loaded, captured, this_server, everyone, roles, cancelled;
    // The thumb drag owns the capture until release, even outside the panel.
    bool thumb_dragging;
    UiInboxTab tab;
};
Inbox* inbox;
// Kept while the popout's storage is released between opens: preferences, the
// open count (it keys the entrance motion) and the unreads scroll position.
struct InboxRetained {
    u64 account_scope, opens;
    float scroll_dips;
    UiInboxTab tab;
    bool this_server, everyone, roles, valid;
} retained;

bool front() noexcept {
    return g_app.account_ui && !g_app.login.visible && !application_settings_active() && !g_app.text_form &&
           !g_app.popup_scene.active && !g_app.image_viewer.scene.active && !g_app.reaction_viewer.active &&
           !(g_app.renderer.profile_scene && g_app.renderer.profile_scene->active);
}
bool connected() noexcept {
    return g_app.account.instance && g_app.account.status.running && g_app.account.api.mentions;
}
u64 server_filter() noexcept {
    const auto& state = g_app.account_ui->chat_state;
    return !state.home_selected ? state.selected_guild_id : 0;
}
void append(char* output, u32 capacity, u32& used, Utf8View text) noexcept {
    used += utf8_copy_truncated(output + used, capacity - used, text);
}
// Mentions read as names in a preview: <@id>, <@!id>, <@&id> and <#id>.
u32 readable(Utf8View source, char* output, u32 capacity) noexcept {
    const auto& ui = *g_app.account_ui;
    u32 used = 0, at = 0;
    while (at < source.bytes && used < capacity) {
        if (source.data[at] == '<' && at + 2 < source.bytes && (source.data[at + 1] == '@' || source.data[at + 1] == '#')) {
            u32 scan = at + 2;
            const bool role = source.data[at + 1] == '@' && source.data[scan] == '&';
            if (source.data[at + 1] == '@' && (source.data[scan] == '!' || role))
                ++scan;
            u64 id = 0;
            const u32 digits = scan;
            while (scan < source.bytes && source.data[scan] >= '0' && source.data[scan] <= '9' && id < ~u64{0} / 10)
                id = id * 10 + static_cast<u64>(source.data[scan++] - '0');
            if (scan < source.bytes && source.data[scan] == '>' && scan > digits && id) {
                Utf8View name{};
                if (source.data[at + 1] == '#') {
                    if (const auto* channel = chat_state_store_find_channel(ui.chat_state, id))
                        name = chat_state_store_channel_name(ui.chat_state, *channel);
                    append(output, capacity, used, utf8_literal("#"));
                } else if (role) {
                    for (u32 g = 0; !name.bytes && g < ui.chat_state.guild_count; ++g) {
                        const u16 slot = ui.chat_state.guild_order[g];
                        if (const auto* record = slot < ui.chat_state.guild_capacity
                                                     ? chat_state_store_find_role(ui.chat_state,
                                                                                     ui.chat_state.guilds[slot].id, id)
                                                     : nullptr)
                            name = chat_state_store_role_name(ui.chat_state, *record);
                    }
                    append(output, capacity, used, utf8_literal("@"));
                } else {
                    if (const auto* user = user_store_find(ui.user_store, id))
                        name = user_store_name(ui.user_store, *user);
                    append(output, capacity, used, utf8_literal("@"));
                }
                append(output, capacity, used, name.bytes ? name : utf8_literal("unknown"));
                at = scan + 1;
                continue;
            }
        }
        const auto decoded = utf8_decode_one(source.data + at, source.bytes - at);
        const u32 width = decoded.valid && decoded.width ? decoded.width : 1;
        if (used + width > capacity)
            break;
        copy_bytes(output + used, source.data + at, width);
        used += width;
        at += width;
    }
    return used;
}
void store_mentions(u32 count) noexcept {
    auto& ui = *g_app.account_ui;
    const u64 now = clock_unix_milliseconds() / 1000;
    for (u32 i = 0; i < count && inbox->mention_count < kMentionCapacity; ++i) {
        const auto& message = inbox->decoded[i];
        bool duplicate = false;
        for (u32 k = 0; k < inbox->mention_count && !duplicate; ++k)
            duplicate = inbox->mentions[k].message_id == message.id;
        if (duplicate)
            continue;
        auto& row = inbox->mentions[inbox->mention_count++];
        row = {};
        row.message_id = message.id;
        row.channel_id = message.channel_id;
        row.author_id = message.author_id;
        {
            // The user cache may hold a newer hash than this page.
            Utf8View hash = message.author_avatar_hash;
            if (const auto* user = user_store_find(ui.user_store, message.author_id))
                if (user->avatar_hash_bytes)
                    hash = user_store_avatar_hash(*user);
            if (hash.bytes <= sizeof(row.avatar) && user_avatar_variant(hash)) {
                copy_bytes(row.avatar, hash.data, hash.bytes);
                row.avatar_bytes = static_cast<u8>(hash.bytes);
            }
        }
        const auto* channel = chat_state_store_find_channel(ui.chat_state, message.channel_id);
        row.guild_id = channel ? channel->guild_id : 0;
        u32 used = 0;
        Utf8View author = message.author_name;
        if (const auto* user = user_store_find(ui.user_store, message.author_id))
            author = user_store_name(ui.user_store, *user);
        append(row.author, sizeof(row.author), used, author.bytes ? author : utf8_literal("Unknown user"));
        row.author_bytes = static_cast<u16>(used);
        char raw[kMessageCardExcerptMaximumBytes];
        const auto card = message_card_excerpt({message.content, message.snapshot.present ? message.snapshot.content : Utf8View{},
                                                message.snapshot.present, true, message.attachments.count,
                                                message.embeds.count, message.stickers.count, message.components.count},
                                               {raw, sizeof(raw)});
        row.excerpt_bytes = static_cast<u16>(readable({raw, card.bytes}, row.excerpt, sizeof(row.excerpt)));
        used = 0;
        if (channel) {
            if (!channel_is_private(channel->type))
                append(row.detail, sizeof(row.detail), used, utf8_literal("#"));
            append(row.detail, sizeof(row.detail), used, chat_state_store_channel_name(ui.chat_state, *channel));
            if (const auto* guild = channel->guild_id ? chat_state_store_find_guild(ui.chat_state, channel->guild_id)
                                                      : nullptr) {
                append(row.detail, sizeof(row.detail), used, utf8_literal(" · "));
                append(row.detail, sizeof(row.detail), used, chat_state_store_guild_name(ui.chat_state, *guild));
            }
        } else
            append(row.detail, sizeof(row.detail), used, utf8_literal("Unavailable channel"));
        row.detail_bytes = static_cast<u16>(used);
        const auto time = format_timestamp_local(message.timestamp_ms / 1000, 'R', now,
                                                         {row.time, sizeof(row.time)});
        row.time_bytes = time.ok ? static_cast<u16>(time.bytes) : 0;
    }
}
u64 unread_key() noexcept {
    const auto& s = g_app.account_ui->chat_state;
    return (static_cast<u64>(s.activity_mutations) << 32) ^ s.channel_mutations ^
           (static_cast<u64>(s.guild_mutations) << 16);
}
bool push_unread(UnreadRow row) noexcept {
    if (inbox->unread_count == kUnreadCapacity)
        return false;
    inbox->unreads[inbox->unread_count++] = row;
    return true;
}
void title(UnreadRow& row, Utf8View text) noexcept {
    u32 used = 0;
    append(row.title, sizeof(row.title), used, text);
    row.title_bytes = static_cast<u16>(used);
}
void build_unreads() noexcept {
    auto& s = g_app.account_ui->chat_state;
    inbox->unread_count = 0;
    inbox->unread_key = unread_key();
    const auto channels = [&](u64 guild_id) noexcept {
        u32 added = 0;
        for (u32 order = 0; order < s.channel_count; ++order) {
            const auto& c = s.channels[s.channel_order[order]];
            if (c.guild_id != guild_id || (guild_id == 0) != channel_is_private(c.type) ||
                !chat_state_store_channel_unread(s, c))
                continue;
            if (!added) {
                UnreadRow header{UiInboxRowKind::group, guild_id ? guild_id : ~u64{0}, 0};
                const auto* guild = guild_id ? chat_state_store_find_guild(s, guild_id) : nullptr;
                title(header, guild ? chat_state_store_guild_name(s, *guild) : utf8_literal("Direct messages"));
                if (!push_unread(header))
                    return;
            }
            UnreadRow row{UiInboxRowKind::channel, guild_id, c.id, c.mention_count};
            const auto name = chat_state_store_channel_name(s, c);
            title(row, name.bytes ? name : utf8_literal("Conversation"));
            row.muted = c.notification_dimmed;
            if (!push_unread(row))
                return;
            ++added;
        }
    };
    channels(0);
    for (u32 order = 0; order < s.guild_count; ++order) {
        const u16 slot = s.guild_order[order];
        if (slot < s.guild_capacity && !s.guilds[slot].unavailable && s.guilds[slot].unread_channels)
            channels(s.guilds[slot].id);
    }
    ++inbox->revision;
}
bool resolve(void* context, u32 index, UiInboxRow* output) noexcept {
    auto& value = *static_cast<Inbox*>(context);
    if (!output)
        return false;
    if (value.tab == UiInboxTab::mentions) {
        if (index >= value.mention_count)
            return false;
        const auto& row = value.mentions[index];
        *output = {UiInboxRowKind::mention, row.message_id, row.channel_id, row.guild_id,
                   {row.author, row.author_bytes}, {row.detail, row.detail_bytes}, {row.excerpt, row.excerpt_bytes},
                   {row.time, row.time_bytes}};
        output->author_id = row.author_id;
        output->avatar_hash = {row.avatar, row.avatar_bytes};
        return true;
    }
    if (index >= value.unread_count)
        return false;
    const auto& row = value.unreads[index];
    *output = {row.kind, row.kind == UiInboxRowKind::group ? row.guild_id : row.channel_id, row.channel_id,
               row.guild_id, {row.title, row.title_bytes}, {}, {}, {}, row.mentions, row.muted};
    return true;
}
bool source_current(void* context) noexcept {
    return context && static_cast<Inbox*>(context)->open;
}
Inbox* ensure() noexcept {
    if (!inbox) {
        inbox = static_cast<Inbox*>(VirtualAlloc(nullptr, sizeof(Inbox), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!inbox)
            return nullptr;
        inbox->everyone = inbox->roles = true;
        inbox->source = {inbox, 0, 1, resolve, source_current};
        if (retained.valid) {
            inbox->account_scope = retained.account_scope;
            inbox->opens = retained.opens;
            inbox->tab = retained.tab;
            inbox->this_server = retained.this_server;
            inbox->everyone = retained.everyone;
            inbox->roles = retained.roles;
        }
    }
    return inbox;
}
void cancel_request() noexcept {
    if (inbox->request_id && !inbox->cancelled && g_app.account.instance && g_app.account.api.mentions) {
        g_app.account.api.mentions(g_app.account.instance, nullptr, inbox->request_id);
        inbox->cancelled = true;
    }
}
void restart_mentions() noexcept {
    cancel_request();
    inbox->mention_count = 0;
    inbox->has_more = inbox->failed = inbox->loaded = false;
    inbox->queued = true;
    inbox->retry_at = 0;
    inbox->attempts = 0;
    ++inbox->revision;
}
void release_capture() noexcept {
    inbox->pressed = {};
    const bool captured = inbox->captured || inbox->thumb_dragging;
    inbox->captured = false;
    inbox->thumb_dragging = false;
    ui_inbox_scene_set_scrollbar_state(&inbox->scene, UiScrollbarVisualState::normal);
    if (captured && GetCapture() == g_app.window)
        ReleaseCapture();
}
bool repaint() noexcept {
    return application_inbox_synchronize() && refresh_account_frame(true);
}
bool activate(UiInboxTarget target) noexcept {
    auto& value = *inbox;
    switch (target.action) {
    case UiInboxAction::close:
        application_inbox_close();
        return refresh_account_frame(true);
    case UiInboxAction::tab_mentions:
    case UiInboxAction::tab_unreads:
        value.tab = target.action == UiInboxAction::tab_mentions ? UiInboxTab::mentions : UiInboxTab::unreads;
        if (value.tab == UiInboxTab::unreads)
            build_unreads();
        else if (!value.loaded && !value.request_id)
            restart_mentions();
        ++value.revision;
        break;
    case UiInboxAction::filter_server:
        value.this_server = !value.this_server;
        restart_mentions();
        break;
    case UiInboxAction::filter_everyone:
        value.everyone = !value.everyone;
        restart_mentions();
        break;
    case UiInboxAction::filter_roles:
        value.roles = !value.roles;
        restart_mentions();
        break;
    case UiInboxAction::load_more:
        if (value.has_more && !value.request_id) {
            value.queued = true;
            value.attempts = 0;
            ++value.revision;
        }
        break;
    case UiInboxAction::retry:
        value.failed = false;
        value.queued = true;
        value.attempts = 0;
        ++value.revision;
        break;
    case UiInboxAction::jump:
        application_inbox_close();
        return application_navigate_notification(target.channel_id, target.id);
    case UiInboxAction::open_channel:
        application_inbox_close();
        return application_navigate_notification(target.channel_id, 0);
    case UiInboxAction::mark_channel:
        application_read_state_mark(target.id, true);
        build_unreads();
        break;
    case UiInboxAction::mark_guild:
        if (target.id == ~u64{0}) {
            for (u32 i = 0; i < value.unread_count; ++i)
                if (value.unreads[i].kind == UiInboxRowKind::channel && !value.unreads[i].guild_id)
                    application_read_state_mark(value.unreads[i].channel_id, true);
        } else {
            ApplicationScopedReadTicket ticket{};
            if (application_scoped_read_capture(ReadScopeKind::guild, target.id, &ticket))
                application_scoped_read_begin(ticket);
        }
        build_unreads();
        break;
    default:
        return true;
    }
    return repaint();
}
} // namespace

bool application_inbox_available() noexcept {
    return g_app.account_ui && !g_app.login.visible && g_app.account_ui->chat_state.self_user_id &&
           g_app.plugin_account_scope_serial;
}
bool application_inbox_active() noexcept {
    return inbox && inbox->open;
}
bool application_inbox_toggle() noexcept {
    if (application_inbox_active()) {
        application_inbox_close();
        return refresh_account_frame(true);
    }
    if (!application_inbox_available() || !ensure())
        return false;
    auto& value = *inbox;
    if (value.account_scope != g_app.plugin_account_scope_serial) {
        const UiInboxSource source = value.source;
        cancel_request();
        ui_inbox_scene_release(&value.scene);
        clear_bytes(&value, sizeof(value));
        value.source = source;
        value.everyone = value.roles = true;
        value.account_scope = g_app.plugin_account_scope_serial;
    }
    application_composer_text_input_suspend();
    g_app.message_composer.focused = false;
    value.open = true;
    ++value.opens;
    // Mentions reload on each open, as the client's popout refetches.
    if (value.tab == UiInboxTab::mentions)
        restart_mentions();
    else
        build_unreads();
    if (!repaint())
        return false;
    if (retained.valid) {
        // Reapplied after the first layout bounds it; a reloading mentions list clamps to the top.
        if (retained.scroll_dips > 0 && ui_inbox_scene_scroll(&value.scene, retained.scroll_dips) && g_app.window)
            application_request_frame();
        retained.valid = false;
    }
    g_app.context_focus = &value.scene.widgets;
    ui_inbox_scene_focus(&value.scene, {value.tab == UiInboxTab::mentions ? UiInboxAction::tab_mentions
                                                                          : UiInboxAction::tab_unreads});
    return true;
}
void application_inbox_close() noexcept {
    g_app.renderer.inbox_scene = nullptr;
    if (!inbox)
        return;
    inbox->open = false;
    release_capture();
    if (g_app.context_focus == &inbox->scene.widgets)
        g_app.context_focus = nullptr;
    application_request_frame();
}
void application_inbox_trim() noexcept {
    if (!inbox || inbox->open || inbox->request_id || inbox->captured || inbox->thumb_dragging)
        return;
    retained = {inbox->account_scope, inbox->opens, inbox->scene.scroll_dips, inbox->tab,
                inbox->this_server, inbox->everyone, inbox->roles, true};
    ui_inbox_scene_release(&inbox->scene);
    VirtualFree(inbox, 0, MEM_RELEASE);
    inbox = nullptr;
}
void application_inbox_release() noexcept {
    retained = {};
    application_inbox_close();
    if (!inbox)
        return;
    cancel_request();
    ui_inbox_scene_release(&inbox->scene);
    VirtualFree(inbox, 0, MEM_RELEASE);
    inbox = nullptr;
}
bool application_inbox_synchronize() noexcept {
    if (!inbox || !inbox->open) {
        g_app.renderer.inbox_scene = nullptr;
        return true;
    }
    auto& value = *inbox;
    if (!application_inbox_available() || value.account_scope != g_app.plugin_account_scope_serial) {
        application_inbox_close();
        return true;
    }
    if (value.tab == UiInboxTab::unreads && value.unread_key != unread_key())
        build_unreads();
    const bool server = server_filter() != 0;
    if (value.this_server && !server) {
        value.this_server = false;
        restart_mentions();
    }
    value.source.count = value.tab == UiInboxTab::mentions ? value.mention_count : value.unread_count;
    value.source.revision = value.revision;
    const bool loading = value.queued || value.request_id;
    value.content = {value.failed           ? utf8_literal("Check your connection and try again.")
                     : !connected()          ? utf8_literal("Your inbox loads when MaiCord is connected.")
                                             : Utf8View{},
                     value.revision,
                     value.tab,
                     value.tab == UiInboxTab::unreads ? UiInboxPhase::ready
                     : value.failed                   ? UiInboxPhase::failed
                     : value.loaded || !connected()   ? UiInboxPhase::ready
                                                      : UiInboxPhase::loading,
                     value.this_server,
                     server,
                     value.everyone,
                     value.roles,
                     value.has_more,
                     loading && value.mention_count != 0};
    auto viewport = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::content);
    viewport.top = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::title_bar).bottom;
    viewport.right = static_cast<float>(g_app.renderer.width) * 96.f / maximum(g_app.renderer.dpi, 1u);
    if (!ui_inbox_scene_update(&value.scene, &value.source, &value.content, viewport))
        return false;
    g_app.renderer.inbox_scene = front() ? &value.scene : nullptr;
    return true;
}
bool application_inbox_pending(u64 now_ms) noexcept {
    return inbox && inbox->open && inbox->queued && !inbox->request_id && connected() &&
           (!inbox->retry_at || inbox->retry_at <= now_ms) &&
           inbox->account_scope == g_app.plugin_account_scope_serial;
}
bool application_inbox_submit(u64 request_id, u64 now_ms) noexcept {
    if (!request_id || !application_inbox_pending(now_ms))
        return false;
    auto& value = *inbox;
    const u64 oldest = value.mention_count ? value.mentions[value.mention_count - 1].message_id : 0;
    value.submitted = {oldest, value.this_server ? server_filter() : 0, kMentionsPageLimit, value.roles,
                       value.everyone};
    const auto submitted = g_app.account.api.mentions(g_app.account.instance, &value.submitted, request_id);
    if (submitted.error == RestClientSubmitError::busy) {
        value.retry_at = deadline_after(now_ms, 50);
        return false;
    }
    value.queued = false;
    if (!submitted.ok()) {
        value.failed = true;
        repaint();
        return false;
    }
    value.request_id = request_id;
    value.cancelled = false;
    ++value.attempts;
    repaint();
    return true;
}
bool application_inbox_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!inbox || !inbox->request_id || response.request_id != inbox->request_id)
        return false;
    auto& value = *inbox;
    value.request_id = 0;
    if (value.cancelled) {
        value.cancelled = false;
        return true;
    }
    if (response.transport_ok() && response.rate_limited && response.status_code == 429 &&
        response.retry_at_ms != 0 && value.attempts < 3) {
        value.queued = true;
        value.retry_at = response.retry_at_ms;
        return true;
    }
    bool ok = response.transport_ok() && response.status_code == 200 && g_app.account_ui;
    if (ok) {
        MessageHistoryDecodeScratch scratch{};
        ok = application_message_decode_scratch(*g_app.account_ui, &scratch, body);
        const auto decoded = ok ? decode_message_mentions_page(body, scratch.page_structurals,
                                                               scratch.message_structurals,
                                                               scratch.maximum_depth, scratch.content,
                                                               {value.decoded, kMentionsPageLimit},
                                                               scratch.mention_users)
                                : MessageHistoryDecodeResult{};
        ok = ok && decoded.ok();
        if (ok) {
            store_mentions(decoded.page.count);
            value.has_more = mentions_has_more(decoded.page.count, value.submitted.limit) &&
                             value.mention_count < kMentionCapacity;
            value.loaded = true;
        }
    }
    if (!ok) {
        log_u64(LogLevel::warning, "inbox", "mentions_response_status", response.status_code);
        value.failed = true;
    }
    ++value.revision;
    return repaint();
}
u64 application_inbox_opens() noexcept {
    return inbox ? inbox->opens : retained.valid ? retained.opens : 0;
}
u64 application_inbox_next_wake() noexcept {
    return inbox && inbox->open && inbox->queued && !inbox->request_id && connected()
               ? (inbox->retry_at ? inbox->retry_at : 1)
               : 0;
}
bool application_inbox_scroll_range(UiScrollRange* output) noexcept {
    if (!output || !application_inbox_active() || !front())
        return false;
    *output = {inbox->scene.body, inbox->scene.scroll_dips, ui_inbox_scene_scroll_maximum(inbox->scene), 40., 1., 0.,
               inbox->revision};
    return true;
}
// A scroll moves only the popout's retained rows: its text suffix is rebuilt
// from the scene revision at present, never by a forced account refresh.
bool application_inbox_scroll_to(double offset) noexcept {
    return application_inbox_active() && ui_inbox_scene_scroll(&inbox->scene, static_cast<float>(offset)) &&
           (!g_app.window || application_request_frame() != FALSE);
}
bool application_inbox_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (!g_app.account_ui || g_app.login.visible)
        return false;
    if (message == WM_SETCURSOR) {
        // The open popout owns its panel; elsewhere the shell resolves as usual.
        UiPoint point{};
        if (!application_inbox_active() || !front() ||
            !application_cursor_point(g_app.window, g_app.renderer.dpi, lparam, &point) ||
            !ui_inbox_scene_contains(inbox->scene, point.x, point.y))
            return false;
        application_set_cursor(ui_inbox_scene_cursor(inbox->scene, point.x, point.y));
        return true;
    }
    const float scale = 96.f / maximum(g_app.renderer.dpi, 1u);
    float x = static_cast<short>(LOWORD(lparam)) * scale;
    float y = static_cast<short>(HIWORD(lparam)) * scale;
    if (message == WM_MOUSEWHEEL) {
        POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        ScreenToClient(g_app.window, &point);
        x = point.x * scale;
        y = point.y * scale;
    }
    const bool header = front() && ui_rect_contains(ui_scene_header_geometry(g_app.shell_scene).inbox, x, y);
    if (message == WM_LBUTTONDOWN && header) {
        if (!ensure())
            return true;
        inbox->pressed = {UiInboxAction::none, ~u64{0}};
        inbox->captured = true;
        SetCapture(g_app.window);
        return true;
    }
    if (!inbox)
        return false;
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE) {
        if (inbox->captured || inbox->thumb_dragging)
            release_capture();
        return false;
    }
    if (inbox->thumb_dragging) {
        if (message == WM_MOUSEMOVE) {
            if (!(wparam & MK_LBUTTON) || !application_inbox_active()) {
                release_capture();
                application_request_frame();
                return true;
            }
            const auto geometry = ui_inbox_scene_scrollbar(inbox->scene);
            if (geometry.visible)
                application_inbox_scroll_to(ui_scrollbar_scroll_from_pointer(geometry, y, inbox->thumb_grab));
            return true;
        }
        if (message == WM_LBUTTONUP) {
            release_capture();
            application_request_frame();
            return true;
        }
    }
    const bool inside = application_inbox_active() && front() && ui_inbox_scene_contains(inbox->scene, x, y);
    if (message == WM_LBUTTONUP && inbox->captured) {
        const auto pressed = inbox->pressed;
        const auto hit = inside ? ui_inbox_scene_hit_test(inbox->scene, x, y) : UiInboxTarget{};
        release_capture();
        if (pressed.id == ~u64{0} && pressed.action == UiInboxAction::none) {
            if (header)
                application_inbox_toggle();
            return true;
        }
        if (ui_inbox_target_equal(pressed, hit) && hit.action != UiInboxAction::none)
            activate(hit);
        return true;
    }
    if (!application_inbox_active() || !front())
        return false;
    if (!inside) {
        // Popouts close on an outside click, letting the click through.
        if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) {
            application_inbox_close();
            refresh_account_frame(true);
        } else if (message == WM_MOUSEMOVE) {
            bool changed = false;
            ui_inbox_scene_set_hover(&inbox->scene, -1.f, -1.f, &changed);
            if (ui_inbox_scene_set_scrollbar_state(&inbox->scene, UiScrollbarVisualState::normal) || changed)
                application_request_frame();
        }
        return false;
    }
    if (message == WM_MOUSEWHEEL) {
        // The shared scroll host animates the popout like every other list.
        handle_scroll_wheel(wparam, lparam);
        return true;
    }
    const auto scrollbar = ui_inbox_scene_scrollbar(inbox->scene);
    if (message == WM_MOUSEMOVE) {
        // The covered timeline must not keep its hover-only scrollbar lit.
        update_scrollbar_hover(x, y);
        bool changed = false;
        ui_inbox_scene_set_hover(&inbox->scene, x, y, &changed);
        changed = ui_inbox_scene_set_scrollbar_state(&inbox->scene, ui_scrollbar_track_hit(scrollbar, x, y)
                                                                        ? UiScrollbarVisualState::hovered
                                                                        : UiScrollbarVisualState::normal) ||
                  changed;
        if (changed)
            application_request_frame();
        return true;
    }
    if (message == WM_LBUTTONDOWN && ui_scrollbar_track_hit(scrollbar, x, y)) {
        // Grab the thumb where it was pressed; a track press centres it.
        ui_scroll_host_cancel(&g_app.scroll_host);
        inbox->thumb_grab = ui_scrollbar_thumb_hit(scrollbar, x, y) ? y - scrollbar.thumb.top
                                                                    : ui_rect_height(scrollbar.thumb) * .5f;
        inbox->thumb_dragging = true;
        ui_inbox_scene_set_scrollbar_state(&inbox->scene, UiScrollbarVisualState::pressed);
        SetCapture(g_app.window);
        application_inbox_scroll_to(ui_scrollbar_scroll_from_pointer(scrollbar, y, inbox->thumb_grab));
        application_request_frame();
        return true;
    }
    if (message == WM_LBUTTONDOWN) {
        inbox->pressed = ui_inbox_scene_hit_test(inbox->scene, x, y);
        if (inbox->pressed.action != UiInboxAction::none)
            ui_inbox_scene_focus(&inbox->scene, inbox->pressed);
        inbox->captured = true;
        g_app.context_focus = &inbox->scene.widgets;
        SetFocus(g_app.window);
        SetCapture(g_app.window);
        application_request_frame();
        return true;
    }
    return message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
           message == WM_MBUTTONDOWN || message == WM_MBUTTONUP;
}
bool application_inbox_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    if (!application_inbox_active() || !front())
        return false;
    if (kind != UiInputKind::key_down && kind != UiInputKind::system_key_down)
        return (kind == UiInputKind::character || kind == UiInputKind::system_character) &&
               g_app.context_focus == &inbox->scene.widgets;
    if (code == VK_ESCAPE) {
        application_inbox_close();
        refresh_account_frame(true);
        return true;
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000) ||
        g_app.context_focus != &inbox->scene.widgets)
        return false;
    if (code == VK_RETURN || code == VK_SPACE) {
        if (!repeated)
            activate(ui_inbox_scene_focused_target(inbox->scene));
        return true;
    }
    UiInboxFocusMove move{};
    if (code == VK_TAB)
        move = (GetKeyState(VK_SHIFT) & 0x8000) ? UiInboxFocusMove::previous : UiInboxFocusMove::next;
    else if (code == VK_DOWN)
        move = UiInboxFocusMove::next;
    else if (code == VK_UP)
        move = UiInboxFocusMove::previous;
    else if (code == VK_HOME)
        move = UiInboxFocusMove::first;
    else if (code == VK_END)
        move = UiInboxFocusMove::last;
    else
        return false;
    ui_inbox_scene_move_focus(&inbox->scene, move);
    application_request_frame();
    return true;
}
} // namespace maicord::app_detail
