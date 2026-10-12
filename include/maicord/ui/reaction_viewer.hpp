#pragma once

#include "maicord/model/message_rich_store.hpp"
#include "maicord/protocol/reaction_users.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/widget_manager.hpp"

namespace maicord {

// Sized to fit the account arena's 8 MiB limit; additional users are paged in.
inline constexpr u32 kReactionViewerUserCapacity = 2'048;
inline constexpr u32 kReactionViewerTextCapacity = 256 * 1'024;
inline constexpr float kReactionViewerPreferredWidth = 480.0f;
inline constexpr float kReactionViewerPreferredHeight = 784.0f;
inline constexpr float kReactionViewerViewportMargin = 16.0f;
inline constexpr float kReactionViewerHeaderHeight = 48.0f;
inline constexpr float kReactionViewerBodyTop = 80.0f;
inline constexpr float kReactionViewerUserRowHeight = 52.0f;
inline constexpr float kReactionViewerAggregateRowHeight = 40.0f;
inline constexpr u32 kReactionViewerNoIndex = ~0u;

enum ReactionViewerUserFlag : u8 {
    reaction_viewer_user_bot = 1u << 0,
};

struct ReactionViewerAggregate {
    u64 emoji_id;
    u32 name_offset;
    u32 count;
    u32 query_count;
    u16 name_bytes;
    MessageReactionType type;
    u8 flags;
    char count_text[12];
    u8 count_text_bytes;
};

struct ReactionViewerUser {
    u64 id;
    u32 username_offset;
    u32 display_name_offset;
    u16 username_bytes;
    u16 display_name_bytes;
    u8 avatar_hash_bytes;
    u8 flags;
    char avatar_hash[kReactionUserMaximumAvatarHashBytes];
};

struct ReactionViewer {
    ReactionViewerAggregate aggregates[kGatewayMessageReactionMaximum];
    char aggregate_names[kGatewayMessageReactionMaximum *
                         kMessageReactionMaximumEmojiNameBytes];
    UiWidgetManager widgets;
    UiWidget widget_storage[1];
    UiLayoutNode layout_storage[1];
    UiWidgetReference text_owner;
    ReactionViewerUser* users;
    char* text;
    UiRect viewport;
    UiRect panel;
    UiRect header;
    UiRect body;
    UiRect close_button;
    u64 guild_id;
    u64 channel_id;
    u64 message_id;
    u64 after_user_id;
    u32 user_capacity;
    u32 text_capacity;
    u32 user_count;
    u32 text_used;
    u32 expected_user_count;
    u32 selected_aggregate;
    u32 first_user;
    u32 first_aggregate;
    float user_scroll;
    float aggregate_scroll;
    u32 hovered_user;
    u32 hovered_aggregate;
    u32 revision;
    u32 opens;
    u32 closes;
    u32 rejected;
    u8 aggregate_count;
    bool active;
    bool loading;
    bool complete;
    bool failed;
    bool truncated;
};

enum class ReactionViewerError : u8 {
    none,
    invalid_input,
    no_reactions,
    capacity,
    layout,
    stale_page,
};

struct ReactionViewerResult {
    ReactionViewerError error;
    bool changed;

    constexpr bool ok() const noexcept {
        return error == ReactionViewerError::none;
    }
};

enum class ReactionViewerHitKind : u8 {
    outside,
    panel,
    close,
    aggregate,
    user,
};

struct ReactionViewerHit {
    UiRect rectangle;
    u32 index;
    ReactionViewerHitKind kind;
};

bool reaction_viewer_initialize(ReactionViewer* viewer,
                                Slice<ReactionViewerUser> users,
                                Slice<char> text) noexcept;
ReactionViewerResult reaction_viewer_open(
    ReactionViewer* viewer, u64 guild_id, u64 channel_id, u64 message_id,
    const MessageRichStore& rich_store, const MessageRichRecord& rich,
    UiRect viewport) noexcept;
bool reaction_viewer_close(ReactionViewer* viewer,
                           bool* changed = nullptr) noexcept;
bool reaction_viewer_layout(ReactionViewer* viewer, UiRect viewport,
                            bool* changed = nullptr) noexcept;
ReactionViewerResult reaction_viewer_select_aggregate(
    ReactionViewer* viewer, u32 aggregate_index) noexcept;
Slice<char> reaction_viewer_page_text_output(
    ReactionViewer& viewer) noexcept;
ReactionViewerResult reaction_viewer_accept_page(
    ReactionViewer* viewer, ConstSlice<ReactionUser> users,
    u32 text_bytes) noexcept;
void reaction_viewer_set_loading(ReactionViewer* viewer,
                                 bool loading) noexcept;
void reaction_viewer_set_failed(ReactionViewer* viewer) noexcept;
bool reaction_viewer_request_more(ReactionViewer* viewer,
                                  bool* requested) noexcept;
bool reaction_viewer_scroll_users(ReactionViewer* viewer, i32 rows,
                                  bool* changed) noexcept;
float reaction_viewer_scroll_maximum(const ReactionViewer& viewer, bool users) noexcept;
bool reaction_viewer_set_scroll(ReactionViewer* viewer, bool users, float offset, bool* changed) noexcept;
ReactionViewerHit reaction_viewer_hit_test(const ReactionViewer& viewer,
                                           float x, float y) noexcept;
bool reaction_viewer_set_hover(ReactionViewer* viewer, float x, float y,
                               bool* changed) noexcept;
u32 reaction_viewer_visible_user_count(const ReactionViewer& viewer) noexcept;
u32 reaction_viewer_visible_aggregate_count(
    const ReactionViewer& viewer) noexcept;
UiRect reaction_viewer_user_rectangle(const ReactionViewer& viewer,
                                      u32 user_index) noexcept;
UiRect reaction_viewer_aggregate_rectangle(const ReactionViewer& viewer,
                                           u32 aggregate_index) noexcept;
Utf8View reaction_viewer_aggregate_name(const ReactionViewer& viewer,
                                        u32 aggregate_index) noexcept;
Utf8View reaction_viewer_user_name(const ReactionViewer& viewer,
                                   const ReactionViewerUser& user) noexcept;
Utf8View reaction_viewer_user_username(const ReactionViewer& viewer,
                                       const ReactionViewerUser& user) noexcept;
Utf8View reaction_viewer_user_avatar_hash(
    const ReactionViewerUser& user) noexcept;

static_assert(sizeof(ReactionViewerUser) == 56);

} // namespace maicord
