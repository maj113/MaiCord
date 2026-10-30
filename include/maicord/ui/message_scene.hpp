#pragma once

#include "maicord/ui/list_navigation.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/app/message_timeline.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/ui/message_rich_layout.hpp"
#include "maicord/ui/widget_manager.hpp"
#include "maicord/ui/unread_divider_style.hpp"
#include "maicord/ui/message_result_card.hpp"
#include "maicord/ui/message_media_policy.hpp"
#include "maicord/ui/message_markdown.hpp"
#include "maicord/ui/plugin_render.hpp"

namespace maicord {

struct MessagePollStore;
struct InviteCardCache;
struct ScheduledEventStore;
struct UiTextMeasureCache;
struct UiPollAnswer;
struct UiPollProjection;
struct UiPollState;
struct Theme;
struct UiResolvedThemeStyles;
// Borrowed from the host's lazy poll owner. No storage is acquired for ordinary
// messages; Search and timeline retain their own local message authority.
struct MessageScenePollBinding {
    const MessagePollStore* store{};
    UiTextMeasureCache* measure{};
    u64 now_ms{};
    u16 backend_revision{};
    bool can_vote{}, can_view_voters{};
    bool (*project_state)(void*, u64 message_id, Slice<UiPollAnswer>, UiPollState*) noexcept = nullptr;
    void* context{};
};

inline constexpr u32 kMessageSceneOverscanRows = 1;
// Peak jump-highlight tint over the accent token.
inline constexpr float kMessageSceneHighlightAlpha = 0.18f;
// Ephemeral messages (message flag 1 << 6) sit on a faint accent tint.
inline constexpr u32 kMessageSceneEphemeralFlag = 1u << 6;
inline constexpr float kMessageSceneEphemeralAlpha = 0.05f;
inline constexpr float kMessageScenePreparationMarginDips = 512.0f;
inline constexpr u64 kMessageGroupIntervalMilliseconds = 7ull * 60ull * 1'000ull;
inline constexpr u32 kMessageSceneBaseWidgetsPerRow = 14;
inline constexpr u32 kMessageSceneAttachmentItemOffset = kMessageRichAttachmentItemOffset;
inline constexpr u32 kMessageSceneEmbedItemOffset = kMessageRichEmbedItemOffset;
inline constexpr u32 kMessageSceneStickerItemOffset = kMessageRichStickerItemOffset;
inline constexpr u32 kMessageSceneComponentItemOffset = kMessageRichComponentItemOffset;
inline constexpr u32 kMessageSceneNoticeItemOffset = kMessageRichNoticeItemOffset;
inline constexpr u32 kMessageSceneRichItemMaximum = kMessageRichItemMaximum;
inline constexpr u32 kMessageSceneWidgetsPerContent =
    kMessageSceneBaseWidgetsPerRow + kGatewayMessageReactionMaximum + kMessageSceneRichItemMaximum + 1u;
inline constexpr u32 kMessageSceneWidgetsPerRow = 2u * kMessageSceneWidgetsPerContent;

enum class MessageSceneError : u8 {
    none,
    invalid_input,
    capacity,
    model,
    widget,
    layout,
};

struct MessageSceneResult {
    MessageSceneError error;
    u32 first;
    u32 count;
    bool changed;
    // Only the scroll position moved: the same rows, data and viewport, laid
    // out scroll_shift DIPs from where the previous update put them.
    bool scroll_only;
    float scroll_shift;

    constexpr bool ok() const noexcept {
        return error == MessageSceneError::none;
    }
};

using MessageSceneRichLayout = MessageRichLayout;
using MessageSceneRichItemKind = MessageRichItemKind;
using MessageSceneRichItemLayout = MessageRichItemLayout;

struct MessageSceneRowWidget {
    MessageHandle message;
    UiWidgetHandle widget;
    UiWidgetHandle avatar;
    UiWidgetHandle body;
    UiWidgetHandle reply;
    UiWidgetHandle author;
    UiWidgetHandle content;
    UiWidgetHandle attachments;
    UiWidgetHandle embeds;
    UiWidgetHandle stickers;
    UiWidgetHandle components;
    UiWidgetHandle reactions;
    UiWidgetHandle notice;
    UiWidgetHandle poll;
    UiWidgetHandle thread; // thread starter card; zero height without a thread
    UiWidgetHandle invites; // invite cards for invite links in the content
    UiWidgetHandle reaction_buttons[kGatewayMessageReactionMaximum];
    float reaction_widths[kGatewayMessageReactionMaximum];
    UiWidgetHandle rich_items[kMessageSceneRichItemMaximum];
    MessageSceneRichLayout rich_layout;
    MessageSceneRichItemLayout rich_item_layout;
    u64 message_id;
    u64 rich_signature;
    u64 component_structure;
    u32 message_revision;
    u32 source_index;
    float measured_width;
    float measured_reply_height;
    float measured_content_height;
    float snapshot_height;
    float poll_height;
    u32 poll_content_revision, poll_result_revision;
    u16 poll_backend_revision;
    u8 reaction_button_count;
    u8 attachment_item_count;
    u8 embed_item_count;
    u8 sticker_item_count;
    u8 component_item_count;
    u8 notice_item_count;
    u8 invite_item_count;
    bool rich_layout_measured;
    bool text_measured;
    bool grouped;
    bool forwarded_snapshot;
    bool has_snapshot;
    // Host presentation authority; borrowed immutable Search content may stay
    // resident after access is revoked. Ordinary timeline rows default false.
    bool redacted;
    bool poll_unavailable;
    u16 snapshot_slot;
    // Valid only for the current message identity and scene reset generation.
    bool invite_scan_negative;
};

// Component interaction feedback, owned by the host: the control awaiting a
// response draws a loading state, and a failed interaction annotates its
// message with the "This interaction failed" notice.
struct MessageSceneInteraction {
    u64 message_id;
    u16 component_index;
    bool pending;
    bool failed;
};

struct MessageSceneHit {
    u64 message_id;
    UiWidgetHandle widget;
    u32 source_index;
};

struct MessageReplyHit {
    u64 channel_id;
    u64 message_id;
};

// A sticker preview under a point: its message, rich item widget, the
// sticker's index in its record (a forwarded message's snapshot record when
// snapshot) and the preview square, clipped. The caption below misses.
struct MessageStickerHit {
    u64 message_id;
    UiRect preview;
    UiWidgetHandle widget;
    u32 index;
    bool snapshot;
};

// Message display (text_and_images.message_display_compact) and the
// local group spacing code: 0 = mode default, 1..5 = 0, 4, 8, 16, 24 px.
// Compact rows drop the avatar, put the timestamp in a 5em hanging-indent
// gutter and run the author name inline before the content.
struct MessageSceneDisplay {
    u8 group_spacing;
    bool compact;
};
inline constexpr float kMessageSceneCompactIndentEm = 5.0f;

// Channel-beginning header (the "empty message" block), shown above the first
// message once the loaded window reaches the channel start. The host supplies
// the text; the scene folds the measured block into the first row's extent so
// scrolling, anchoring and bottom placement treat it as timeline content.
enum class MessageSceneIntroKind : u8 {
    none,
    channel,              // # glyph in a 68 px circle, "Welcome to #name!"
    thread,               // thread glyph in a 64 px circle, thread name
    direct_message,       // 80 px recipient avatar, display name, username
    group_direct_message, // 80 px group icon, group name
    no_history,           // description only: history is not readable
};
inline constexpr u32 kMessageSceneIntroTextBytes = 1024;
inline constexpr u32 kMessageSceneIntroSpanMaximum = 4;
struct MessageSceneIntro {
    MessageSceneIntroKind kind;
    bool locked; // private channel / private thread glyph
    u64 user_id; // DM recipient whose avatar fills the circle; zero otherwise
    u64 channel_id;
    // text = header | subtitle | description, without separators.
    u32 header_bytes, subtitle_bytes, description_bytes;
    // Bold runs inside the description, offsets relative to its first byte.
    TextStyleSpan description_spans[kMessageSceneIntroSpanMaximum];
    u32 description_span_count;
    char text[kMessageSceneIntroTextBytes];
};
// Geometry of the intro block in scene coordinates (zero rects when absent).
struct MessageSceneIntroGeometry {
    UiRect area, icon, header, subtitle, description;
};
// The message list's horizontal margin.
inline constexpr float kMessageSceneIntroMargin = 16.0f;
inline constexpr float kMessageSceneIntroHeaderLine = 40.0f;      // 32 px font * 1.25
inline constexpr float kMessageSceneIntroHeaderMargin = 8.0f;     // 8 px above and below the header
inline constexpr float kMessageSceneIntroSubtitleLine = 24.0f;    // 20 px font * 1.2
inline constexpr float kMessageSceneIntroDescriptionLine = 20.0f; // 16 px font * 1.25

struct MessageScene {
    const UiResolvedThemeStyles* theme_styles;
    u32 theme_geometry_revision;
    const MessageScenePollBinding* polls;
    UiWidgetManager widgets;
    Slice<MessageSceneRowWidget> rows;
    StableStorage owned_rows;
    StableStorage owned_measurements;
    Slice<MessageSceneRowWidget> snapshot_rows;
    u32 snapshot_storage_offset;
    u32 snapshot_count;
    u64 prepared_signature;
    u32 preparation_passes;
    u32 prepared_rows;
    MessageTimeline* timeline;
    UiWidgetHandle root;
    UiRect viewport;
    u64 focused_message_id;
    u64 observed_scroll_units;
    u32 first;
    u32 count;
    u32 source_count;
    u32 observed_created;
    u32 observed_updated;
    u32 observed_deleted;
    u32 observed_rich_updated;
    u32 observed_height_updates;
    u32 observed_structural_updates;
    u32 rebuilds;
    u32 layout_passes;
    u32 data_updates;
    u32 measurement_updates;
    u32 rejected;
    u64 unread_channel_id, unread_last_read_message_id, unread_first_message_id;
    UiUnreadDividerStyle unread_divider_style;
    float pixel_scale; // dpi / 96 for device-pixel hairlines; 0 leaves DIP geometry
    float unread_prepared_height;
    u32 unread_revision;
    bool unread_cursor_known;
    bool active;
    bool dirty;
    // A scroll moved the timeline since the last update; kept apart from dirty
    // so an update can tell a pure scroll from other changes.
    bool scroll_dirty;
    bool owns_rows;
    MessageResultCardStyle result_card_style;
    u32 result_card_revision;
    bool result_cards;
    ConstSlice<MessageHandle> source_order;
    u32 source_order_revision;
    // Jump target tint; the host animates the level (0..1).
    u64 highlight_message_id;
    float highlight_level;
    u32 highlight_revision;
    MessageSceneDisplay display;
    // Session reveals and a borrowed per-author policy; scenes without a policy
    // query show explicit media.
    MessageMediaReveals media_reveals;
    MessageMediaPolicy media_policy;
    MessageSceneInteraction interaction;
    // Borrowed invite resolutions; null hosts show no invite cards.
    const InviteCardCache* invite_cards;
    // Borrowed scheduled events for event link cards; may be null.
    const ScheduledEventStore* scheduled_events;
    // Age-restricted channel not yet confirmed: rows paint no content.
    bool content_gated;
    u32 invite_scan_reset_generation;
    // Applies plugin message decorations (the timeline scene only): hidden
    // rows collapse to nothing, collapsed rows drop their rich content.
    bool plugin_decorations;
    // The chat scroller anchors short content to the composer: when the
    // resident rows (and intro) are shorter than the viewport they are laid out
    // against its bottom edge. The leading gap is lead_dips.
    bool bottom_anchored;
    float lead_dips;
    // Where the last layout put the top of the first materialized row.
    float rows_origin_dips;
    MessageSceneIntro intro;
    UiTextMeasureCache* intro_measure;
    u64 intro_signature;
    float intro_measured_width;
    float intro_height; // folded into the first row; zero when hidden
    float intro_header_height, intro_subtitle_height, intro_description_height;
    // Whole-run styles of the header (bold, 32 px) and subtitle (bold, 20 px).
    TextStyleSpan intro_header_style, intro_subtitle_style;
};
// Replaces the channel-beginning header; null or kind none hides it. The
// measure cache (may be null: estimated lines) is borrowed. Returns true when
// the content changed.
bool message_scene_set_intro(MessageScene* scene, const MessageSceneIntro* intro,
                             UiTextMeasureCache* measure) noexcept;
// The channel-beginning strings for one kind (name is the channel or
// conversation display name; username is a DM recipient's username). Text that
// does not fit is truncated at a UTF-8 boundary. False on bad input.
bool message_scene_intro_compose(MessageSceneIntro* intro, MessageSceneIntroKind kind, u64 channel_id,
                                 Utf8View name, Utf8View username, u64 user_id, bool locked) noexcept;
// Text layout of the intro block, valid after message_scene_update.
bool message_scene_intro_geometry(const MessageScene& scene, MessageSceneIntroGeometry* geometry) noexcept;
inline Utf8View message_scene_intro_header(const MessageSceneIntro& intro) noexcept {
    return {intro.text, intro.header_bytes};
}
inline Utf8View message_scene_intro_subtitle(const MessageSceneIntro& intro) noexcept {
    return {intro.text + intro.header_bytes, intro.subtitle_bytes};
}
inline Utf8View message_scene_intro_description(const MessageSceneIntro& intro) noexcept {
    return {intro.text + intro.header_bytes + intro.subtitle_bytes, intro.description_bytes};
}
// Returns true when the gate changed (the host repaints).
bool message_scene_set_content_gated(MessageScene* scene, bool gated) noexcept;
// Invite links found in a message's content, in order (at most
// kMessageInviteCardMaximum). Zero without a bound invite cache.
u32 message_scene_invite_codes(const MessageScene& scene, const MessageRecord& record,
                               Slice<InviteLink> output) noexcept;
bool message_scene_bind_invite_cards(MessageScene* scene, const InviteCardCache* cache) noexcept;
// Event cards read this store; callers repaint text when it changes.
bool message_scene_bind_scheduled_events(MessageScene* scene, const ScheduledEventStore* events) noexcept;
MessageMediaPolicyResult message_scene_media_policy(const MessageScene&, const MessageMediaOwner&) noexcept;
bool message_scene_set_media_policy(MessageScene*, const MessageMediaPolicy&) noexcept;
MessageMediaObscure message_scene_media_obscure(const MessageScene&, const MessageMediaOwner&,
                                                u64 plugin_message_id, u64 reveal_key, bool spoiler,
                                                bool explicit_media) noexcept;
MessageMediaObscure message_scene_media_obscure(const MessageScene&, MessageMediaPolicyResult,
                                                u64 plugin_message_id, u64 reveal_key, bool spoiler,
                                                bool explicit_media) noexcept;
// Cached plugin decoration of a message revision (never runs plugin code).
inline bool message_scene_plugin_decoration(const MessageScene& scene, u64 message_id, u32 revision,
                                            UiPluginRenderDecoration* output) noexcept {
    reset_value(output);
    return scene.plugin_decorations && ui_plugin_render(message_id, revision, output);
}
inline u8 message_scene_plugin_visibility(const MessageScene& scene, const MessageRecord& record) noexcept {
    UiPluginRenderDecoration decoration{};
    return message_scene_plugin_decoration(scene, record.id, record.revision, &decoration) ? decoration.visibility
                                                                                           : ui_plugin_render_normal;
}
// Returns true when the item was newly revealed; the host repaints.
bool message_scene_reveal_media(MessageScene* scene, u64 reveal_key) noexcept;
// Returns true when the feedback changed; failure notices change row height.
bool message_scene_set_interaction(MessageScene* scene, const MessageSceneInteraction& interaction) noexcept;
// Returns true when the display changed; every row is remeasured.
bool message_scene_set_display(MessageScene*, MessageSceneDisplay) noexcept;
// Remeasures every row, e.g. after message_rich_set_display changed geometry.
void message_scene_invalidate_geometry(MessageScene*) noexcept;
constexpr bool message_scene_avatar_visible(const MessageScene& scene, const MessageSceneRowWidget& row) noexcept {
    return !row.grouped && !row.forwarded_snapshot && !scene.display.compact;
}

bool message_scene_bind_poll_source(MessageScene*, const MessageScenePollBinding*) noexcept;
bool message_scene_bind_theme_styles(MessageScene*, const UiResolvedThemeStyles*) noexcept;
// The returned state borrows the caller's answer scratch and model strings only
// until the next model mutation. Paint/text consumers use it synchronously.
bool message_scene_poll_projection(const MessageScene&, const MessageSceneRowWidget&, const Theme&,
                                   Slice<UiPollAnswer>, UiPollProjection*) noexcept;

bool message_scene_initialize(MessageScene* scene, MessageTimeline* timeline,
                              Slice<MessageSceneRowWidget> row_storage, Slice<UiWidget> widget_storage,
                              Slice<UiLayoutNode> layout_storage) noexcept;
// defer_measurements commits retained measurements on demand (as snapshot
// slots always do) instead of one per store slot up front; see
// message_scene_reserve_measurements.
bool message_scene_initialize_owned(MessageScene* scene, MessageTimeline* timeline, u32 maximum_rows,
                                    u32 initial_rows, Slice<UiWidget> widget_storage,
                                    Slice<UiLayoutNode> layout_storage, bool defer_measurements = false) noexcept;
// Commits one retained measurement per store slot (the non-deferred layout).
bool message_scene_reserve_measurements(MessageScene* scene) noexcept;
void message_scene_release(MessageScene* scene) noexcept;
// Only the retained owned-row mode supports result cards. Null restores normal
// timeline presentation; body/snapshot measurements remain independently owned.
bool message_scene_set_result_card_style(MessageScene*, const MessageResultCardStyle*) noexcept;
// Immutable result order may differ from canonical MessageStore lookup order.
// The host owns handles under revision; empty restores canonical timeline order.
bool message_scene_set_source_order(MessageScene*, ConstSlice<MessageHandle>, u32 revision) noexcept;
MessageHandle message_scene_source_handle(const MessageScene&, u32 source_index) noexcept;
u32 message_scene_source_count(const MessageScene&) noexcept;
MessageSceneResult message_scene_update(MessageScene* scene, const UiRect& viewport) noexcept;
// Whether an update has a change to apply, a scroll included.
constexpr bool message_scene_pending(const MessageScene& scene) noexcept {
    return scene.dirty || scene.scroll_dirty;
}
// Cursor projection owns no read-state model. Unknown clears the decoration;
// known zero marks the first authoritative resident in this channel.
// Returns true when the tint changed; a zero id or level clears it.
bool message_scene_set_highlight(MessageScene* scene, u64 message_id, float level) noexcept;
bool message_scene_set_unread_cursor(MessageScene* scene, u64 channel_id, u64 last_read_message_id,
                                     bool known, bool* changed = nullptr,
                                     const UiUnreadDividerStyle* style = nullptr) noexcept;
UiRect message_scene_unread_divider_rectangle(const MessageScene& scene,
                                              const MessageSceneRowWidget& row) noexcept;
UiRect message_scene_message_rectangle(const MessageScene& scene,
                                       const MessageSceneRowWidget& row) noexcept;
bool message_scene_scroll_by(MessageScene* scene, i64 delta_units, u64 bottom_tolerance_units,
                             bool* changed) noexcept;
bool message_scene_scroll_to_start(MessageScene* scene, bool* changed) noexcept;
bool message_scene_scroll_to_bottom(MessageScene* scene, bool* changed) noexcept;
bool message_scene_set_text_metrics(MessageScene* scene, u32 source_index, float reply_height,
                                    float content_height, bool content_present,
                                    const MessageSceneRichLayout* rich_layout,
                                    const MessageSceneRichItemLayout* rich_item_layout,
                                    ConstSlice<float> reaction_widths, bool* changed) noexcept;
const MessageSceneRowWidget* message_scene_snapshot_row(const MessageScene& scene,
                                                        const MessageSceneRowWidget& outer) noexcept;
bool message_scene_set_snapshot_text_metrics(MessageScene* scene, u32 source_index, float content_height,
                                             bool content_present,
                                             const MessageSceneRichLayout* rich_layout,
                                             const MessageSceneRichItemLayout* rich_item_layout,
                                             bool* changed) noexcept;
// Fixed-storage hosts can supply child rows; the owned host uses its existing row allocator.
bool message_scene_bind_snapshot_rows(MessageScene* scene, Slice<MessageSceneRowWidget> rows) noexcept;
UiWidgetHandle message_scene_rich_item_widget(const MessageSceneRowWidget& row,
                                              MessageSceneRichItemKind kind, u32 index) noexcept;
const UiLayoutNode* message_scene_rich_item_layout_node(const MessageScene& scene,
                                                        const MessageSceneRowWidget& row,
                                                        MessageSceneRichItemKind kind, u32 index) noexcept;
const MessageSceneRowWidget* message_scene_resolve(const MessageScene& scene,
                                                   UiWidgetHandle widget) noexcept;
MessageSceneRowWidget* message_scene_find_row(MessageScene* scene, u32 source_index) noexcept;
const MessageSceneRowWidget* message_scene_find_row(const MessageScene& scene, u32 source_index) noexcept;
const UiLayoutNode* message_scene_layout_node(const MessageScene& scene, UiWidgetHandle widget) noexcept;
MessageSceneHit message_scene_hit_test(const MessageScene& scene, float x, float y) noexcept;
MessageReplyHit message_scene_reply_hit_test(const MessageScene& scene, float x, float y) noexcept;
bool message_scene_reaction_index(const MessageScene& scene, MessageSceneHit hit, u32* index) noexcept;
bool message_scene_sticker_hit(const MessageScene& scene, float x, float y, MessageStickerHit* hit) noexcept;
bool message_scene_focus_message(MessageScene* scene, u64 message_id) noexcept;
bool message_scene_reveal_message(MessageScene* scene, u64 message_id) noexcept;
MessageSceneHit message_scene_navigate(MessageScene* scene, UiListMove move) noexcept;
MessageSceneHit message_scene_page(MessageScene* scene, bool reverse) noexcept;

} // namespace maicord
