#pragma once

#include "maicord/model/chat_state.hpp"
#include "maicord/model/voice_state_store.hpp"
#include "maicord/model/guild_display_order.hpp"

namespace maicord {

enum class GuildFolderRole : u8 { none, header, member };
// The scheduled events entry leads a server's channel list: a synthetic row
// with this id and type (never a channel; activating it opens the events).
inline constexpr u64 kNavigationEventsRowId = 0x4556454E54535257ull;
inline constexpr u8 kNavigationEventsRowType = 0xEE;
constexpr bool navigation_events_row(u64 id) noexcept {
    return id == kNavigationEventsRowId;
}
// "Message requests" leads the conversation list while requests exist; the
// row toggles the list between conversations and message requests.
inline constexpr u64 kNavigationRequestsRowId = 0x4D53475245515257ull;
inline constexpr u8 kNavigationRequestsRowType = 0xED;
constexpr bool navigation_requests_row(u64 id) noexcept {
    return id == kNavigationRequestsRowId;
}
inline constexpr u32 kGuildFolderNoColor = ~0u;

// Guild rows follow the display order. A folder header (id carries
// kGuildFolderRowMarker) precedes its members; collapsed members stay in the
// list so guild navigation still reaches them, but the rail hides them.
struct GuildViewRow {
    u64 id;
    u32 revision;
    u32 mention_count;
    u16 generation;
    bool selected;
    bool unavailable;
    bool unread;
    bool has_icon;
    bool muted;
    u64 folder_id;      // header row id shared by the header and its members
    u32 folder_color;   // 0xRRGGBB or kGuildFolderNoColor
    u16 folder_size;    // header: member rows that follow
    u16 folder_index;   // GuildFolderSettings index, kGuildDisplayNoFolder outside folders
    GuildFolderRole folder_role;
    bool folder_expanded;
};

struct ChannelViewRow {
    u64 id;
    u64 parent_id;
    u32 position;
    u32 revision;
    u32 actions;
    u32 mention_count;
    u16 generation;
    u8 type;
    u8 depth;
    bool selected;
    bool selectable;
    bool unread;
    bool collapsed;
    u16 voice_count; // Connected users drawn under a voice channel row.
    bool muted;      // Own or category notification mute, drawn dimmed.
    bool stage_live; // Stage channel with a live stage instance (topic line, green icon).
};

struct ChannelSortKey {
    u64 group_id;
    u64 base_id;
    u64 row_id;
    u32 group_position;
    u32 base_position;
    u32 row_position;
    u8 group_rank;
    u8 row_rank;
    u8 depth;
};

struct NavigationView {
    GuildViewRow* guild_rows;
    ChannelViewRow* channel_rows;
    u16* channel_scratch;
    u8* channel_visibility_scratch;
    ChannelSortKey* channel_sort_keys;
    u32 guild_capacity;
    u32 channel_capacity;
    u32 channel_visibility_capacity;
    u32 guild_count;
    u32 channel_count;
    u32 revision;
    u32 guild_revision;
    u32 channel_revision;
    u32 rejected;
    u32 source_guild_mutations;
    u32 source_channel_mutations;
    u32 source_selection_revision;
    u64 selected_guild_id;
    u64 selected_channel_id;
    const VoiceStateStore* voice_states;
    u64 connected_voice_channel_id; // Local voice session, for the account panel.
    u32 source_voice_revision;
    u32 expanded_folder_count;
    // Borrowed folder layout; null keeps the state's join order without folders.
    const GuildFolderSettings* guild_folders;
    const u64* expanded_folders; // header row ids
    u32 folder_revision;         // host-bumped on layout/expansion changes
    u32 source_folder_revision;  // folder_revision captured by the last rebuild
    // Borrowed voice connection card and account panel voice buttons; null hides the card.
    const struct UiVoicePanelView* voice_panel;
    // Borrowed presence, typing and membership sources for conversation rows.
    const struct UiPrivateRowSource* private_rows;
    // Borrowed live stages (topic line under stage channel rows); may be null.
    const struct StageInstanceStore* stage_instances;
    u32 source_stage_revision;
    // Host-set: show the events row for the selected server with this count
    // ("N Events", or "Events" at zero). Bump events_revision on change.
    bool events_row;
    u32 event_count;
    u32 events_revision;
    u32 source_events_revision;
    char events_label[24];
    u8 events_label_bytes;
    // Host-set (home): the "Message requests" row with this pending count;
    // requests_mode lists only message requests. Bump requests_revision.
    bool requests_row;
    bool requests_mode;
    u32 request_count;
    u32 requests_revision;
    u32 source_requests_revision;
};

enum class NavigationViewError : u8 {
    none,
    invalid_input,
    guild_capacity,
    channel_capacity,
    invalid_hierarchy,
};

struct NavigationViewResult {
    u32 guild_count;
    u32 channel_count;
    NavigationViewError error;
    bool changed;

    constexpr bool ok() const noexcept {
        return error == NavigationViewError::none;
    }
};

bool navigation_view_initialize(NavigationView* view, Slice<GuildViewRow> guild_rows,
                                        Slice<ChannelViewRow> channel_rows,
                                        Slice<u16> channel_scratch,
                                        Slice<u8> channel_visibility_scratch,
                                        Slice<ChannelSortKey> channel_sort_keys) noexcept;
// Requires zero published guild and channel counts and writable caller-owned
// arrays. Unlike navigation_view_initialize it leaves array bytes unchanged:
// readers use published row counts, and rebuild writes each new row and all
// required scratch before use.
bool navigation_view_initialize_sparse(NavigationView* view, Slice<GuildViewRow> guild_rows,
                                      Slice<ChannelViewRow> channel_rows, Slice<u16> channel_scratch,
                                      Slice<u8> channel_visibility_scratch,
                                      Slice<ChannelSortKey> channel_sort_keys) noexcept;
NavigationViewResult navigation_view_rebuild(NavigationView* view,
                                                            const ChatStateStore& state) noexcept;
const GuildViewRow* navigation_view_find_guild(const NavigationView& view,
                                                              u64 guild_id) noexcept;
const ChannelViewRow* navigation_view_find_channel(const NavigationView& view,
                                                                  u64 channel_id) noexcept;
inline constexpr u32 kNavigationVoiceRowsMaximum = 99;
// Recounts voice_count from view->voice_states (and stage_live from
// view->stage_instances) after a voice-state or stage change.
bool navigation_view_refresh_voice(NavigationView* view) noexcept;
// Guild rows that represent selectable guilds (folder headers excluded).
constexpr bool guild_view_row_is_guild(const GuildViewRow& row) noexcept {
    return row.id && row.folder_role != GuildFolderRole::header;
}
constexpr bool guild_view_row_hidden(const GuildViewRow& row) noexcept {
    return row.folder_role == GuildFolderRole::member && !row.folder_expanded;
}

} // namespace maicord
