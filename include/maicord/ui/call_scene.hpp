#pragma once
#include "maicord/media/asset.hpp"
#include "maicord/model/stage_policy.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/transition.hpp"

namespace maicord {
// Call view: a header, a stage of participant tiles (a responsive 16:9 grid,
// or one focused tile above a filmstrip) and a control bar. Pure geometry and
// paint; the host projects participants, video residency and voice state and
// draws video frames and avatars into ui_call_scene_tile_media rectangles.
// Go Live streams are tiles of their own (stream != none).
inline constexpr u32 kUiCallMaximumTiles = 64;
inline constexpr u32 kUiCallNameBytes = 96;
inline constexpr u32 kUiCallTitleBytes = 128;
// Soundboard effect label on a tile (emoji and sound name).
inline constexpr u32 kUiCallEffectBytes = 112;
inline constexpr float kUiCallTileAspect = 16.f / 9.f;
inline constexpr float kUiCallHeaderHeight = 48, kUiCallBarHeight = 80;
// Control bar: 44-DIP rounded squares in groups (audio, video, stage), 4 DIP
// apart inside a group and 12 between groups, then the wider red disconnect
// button.
inline constexpr float kUiCallButton = 44, kUiCallDisconnectWidth = 60, kUiCallGroupGap = 4,
                       kUiCallControlGap = 12;
inline constexpr float kUiCallMinimumTileWidth = 192, kUiCallStageInset = 16;
inline constexpr u32 kUiCallMorphMs = 360;

// Responsive grid: the column count that gives the largest 16:9 tile inside
// width x height. Below the minimum tile width the grid keeps that width and
// scrolls vertically instead (content_height > height).
struct UiCallGridLayout {
    u32 columns, rows;
    float tile_width, tile_height, content_height;
};
UiCallGridLayout ui_call_grid_measure(float width, float height, u32 count, float gap,
                                      float minimum_width = kUiCallMinimumTileWidth) noexcept;
// Writes count tile rectangles: rows are centered, a partial last row too;
// the block is centered vertically unless it scrolls (scroll offsets it).
bool ui_call_grid_place(UiRect area, u32 count, float gap, float scroll, Slice<UiRect> tiles,
                        UiCallGridLayout* layout = nullptr, float minimum_width = kUiCallMinimumTileWidth) noexcept;
// Focus: the stage tile fills the area above a filmstrip of 16:9 tiles.
struct UiCallFocusLayout {
    UiRect stage, strip;
    float strip_tile_width, strip_tile_height, strip_content_width;
};
UiCallFocusLayout ui_call_focus_measure(UiRect area, u32 others, float gap) noexcept;
// Picture rectangle of a width x height video in a tile: contain letterboxes,
// cover fills the tile (the host clips to it). Empty for an empty picture.
UiRect ui_call_video_fit(UiRect tile, u32 width, u32 height, bool cover) noexcept;

enum class UiCallControl : u8 {
    none,
    close,      // Header: back to the chat view.
    mute,
    deafen,
    camera,       // Disabled: camera send is not implemented.
    screen_share, // Disabled: Go Live send is not implemented.
    disconnect,
    fullscreen,      // Control bar: toggles borderless fullscreen.
    tile_fullscreen, // Hover button on a tile: focus it and enter fullscreen.
    stream_watch,    // "Watch stream" on an unwatched stream tile.
    stream_stop,     // "Stop watching" on the watched stream's hover bar.
    stream_volume,   // Stream volume (opens the volume menu) on the hover bar.
    stream_dismiss,  // Close an ended stream's tile.
    // Stage channels (UiCallStage).
    stage_hand,     // Raise or lower your hand (audience).
    stage_become,   // Moderators: speak without raising a hand.
    stage_audience, // Speakers: move yourself to the audience.
    stage_accept,   // Invite banner: join the speakers.
    stage_decline,  // Invite banner: stay in the audience.
    stage_start,    // Moderators: start the stage (topic form).
    stage_topic,    // Moderators: edit the live stage's topic.
    stage_end,      // Moderators: end the live stage.
    stage_invite,   // Hand-raise row: invite that user to speak (user_id).
    stage_move,     // Speaker tile: move that user to the audience (user_id).
    chat,           // Header: show or hide the channel's text chat beside the call.
    soundboard,     // Control bar: opens the soundboard picker.
    count
};
enum class UiCallStream : u8 {
    none,         // A participant (camera or avatar) tile.
    available,    // Someone is live; not watched.
    connecting,   // Watch requested; media not attached or no picture yet.
    live,         // Watched and attached.
    reconnecting, // The stream connection is being re-established.
    ended,        // The watched stream ended (see notice).
};
enum class UiCallStreamNotice : u8 {
    none,
    ended,       // "The stream has ended"
    full,        // The stream reached its viewer limit.
    not_allowed, // Missing permission.
    limited,     // Server-side safety rate limit.
    timed_out,   // No media server in time.
    failed,      // The stream connection failed.
    interrupted, // A fresh Gateway session dropped it.
};
// Copy for a notice ("The stream has ended", ...); empty for none.
Utf8View ui_call_stream_notice_text(UiCallStreamNotice) noexcept;
inline constexpr u32 kUiCallControlCount = static_cast<u32>(UiCallControl::count);

struct UiCallParticipant {
    u64 user_id;
    Utf8View name;                  // Copied by ui_call_scene_update.
    u32 video_ssrc;                 // Nonzero while a camera stream is announced.
    u16 video_width, video_height;  // Display extent of a resident frame; zero shows the avatar.
    bool speaking, muted, deafened; // Muted/deafened include server state.
    bool live;                      // LIVE badge on a participant tile.
    bool self;
    // Stream tiles: user_id is the host's stream tile ID; name the streamer.
    UiCallStream stream;
    UiCallStreamNotice notice; // Ended tiles.
    bool paused;               // The streamer paused (live tiles).
    u32 viewers;               // Shown with the LIVE badge when nonzero.
    MediaAssetKey preview;     // Preview thumbnail while no picture is drawn.
    // The user has a custom avatar (drawn by the host over the placeholder);
    // otherwise the tile shows initials on the identity colour.
    bool has_avatar;
    // A soundboard sound (or voice channel emoji) they just played: emoji and
    // sound name, shown briefly on the tile. Copied by ui_call_scene_update.
    Utf8View effect;
};
// Stage channels: the participants are the speakers; everyone else is the
// audience band (count plus the first avatars); moderators see hand raises
// beside the speakers. The header shows the live topic.
inline constexpr u32 kUiCallStageAudienceShown = 32, kUiCallStageRequestsShown = 16;
inline constexpr u32 kUiCallStageTopicBytes = 480, kUiCallStageNoticeBytes = 128;
inline constexpr float kUiCallStageAudienceHeight = 96, kUiCallStageRequestsWidth = 264,
                       kUiCallStageBannerHeight = 56, kUiCallStageRowHeight = 44;
struct UiCallStagePerson {
    u64 user_id;
    Utf8View name; // Copied by ui_call_scene_update.
};
struct UiCallStage {
    bool active, live;
    bool hand_raised; // The local user's request to speak is pending.
    Utf8View topic;   // Live stage topic.
    Utf8View notice;  // Short status (a failed request); empty for none.
    u32 actions;      // StageAction bits for the local user.
    u32 audience_count, request_count;
    ConstSlice<UiCallStagePerson> audience; // Shown avatars, join order.
    ConstSlice<UiCallStagePerson> requests; // Hand raises, oldest first.
};
struct UiCallContent {
    Utf8View title; // Channel or DM name.
    ConstSlice<UiCallParticipant> participants;
    u64 focus_user;
    float gap{8}; // Tile gap (theme space token).
    bool fullscreen, connected;
    bool self_mute, self_deaf, server_mute, server_deaf;
    UiCallStage stage;
    // Text in voice: the header offers the channel chat; open while it shows.
    bool chat_available, chat_open;
    // The bar offers the soundboard; open while its picker shows.
    bool soundboard_available, soundboard_open;
    // Width of the host's channel search field drawn in this header (zero:
    // none). The header reserves it a slot between the chat toggle and close.
    float search_width;
};
struct UiCallTile {
    u64 user_id;
    UiRect target;  // Settled layout (visibility, video extent).
    UiRect display; // Spring-morphed rectangle used for paint and input.
    UiRect from;
    UiRect clip; // Stage or filmstrip viewport.
    u16 participant;
    bool focused, strip;
};
struct UiCallHit {
    UiCallControl control;
    u64 user_id; // Tile under the point (also for tile_fullscreen).
};
struct UiCallTileMedia {
    u64 user_id;
    u32 video_ssrc;     // Nonzero: draw the video picture if resident.
    UiRect video, clip; // Picture rectangle (contain/cover) and its clip.
    UiRect avatar;      // Avatar circle when no picture is drawn (empty on stream tiles).
    UiRect rounded_clip;
    float radius;
    MediaAssetKey preview; // Stream preview behind the scrim when no picture is drawn.
    bool dim;              // Preview sits under a state scrim (dimmed further by the host).
};

struct UiCallScene {
    UiWidgetManager widgets;
    UiWidget widget_storage[1];
    UiLayoutNode layout_storage[1];
    UiWidgetReference owner;
    UiRect viewport, header, stage, strip, bar;
    // Slot of the host's search field in the header (empty: no field). It is
    // the search's own surface: not part of the call view for input.
    UiRect search_field;
    float search_width;
    UiRect controls[kUiCallControlCount];
    UiCallParticipant participants[kUiCallMaximumTiles];
    char names[kUiCallMaximumTiles][kUiCallNameBytes];
    char effects[kUiCallMaximumTiles][kUiCallEffectBytes];
    char viewer_text[kUiCallMaximumTiles][12];
    char title[kUiCallTitleBytes];
    u32 title_bytes;
    UiCallTile tiles[kUiCallMaximumTiles];
    u32 participant_count, tile_count;
    float scroll, content_height, strip_scroll, strip_content_width, gap;
    u64 focus_user, hovered_user;
    UiCallControl hovered, pressed;
    bool fullscreen, connected, self_mute, self_deaf, server_mute, server_deaf;
    bool active, keyboard_focus;
    bool chat_available, chat_open;
    bool soundboard_available, soundboard_open;
    // Stage (content.stage) copy and geometry; empty rectangles when hidden.
    bool stage_active, stage_live, stage_hand;
    u64 stage_hovered; // Hand-raise row under the pointer (user ID).
    u32 stage_actions, stage_audience_count, stage_request_count, stage_audience_shown, stage_request_shown;
    UiCallStagePerson stage_people[kUiCallStageAudienceShown + kUiCallStageRequestsShown]; // Audience, then requests.
    char stage_names[kUiCallStageAudienceShown + kUiCallStageRequestsShown][kUiCallNameBytes];
    char stage_topic[kUiCallStageTopicBytes], stage_notice[kUiCallStageNoticeBytes];
    u32 stage_topic_bytes, stage_notice_bytes;
    char stage_text[3][32]; // "Audience · N", "+K", "Requests to speak · N"
    u8 stage_text_bytes[3];
    UiRect stage_banner, stage_audience, stage_requests, stage_speakers;
    UiMotionTransition morph;
    u64 morph_generation, layout_key, content_hash;
    u32 revision;
    u8 spinner_phase; // Host frame clock (paint only; part of the paint stamp).
};

// Header toolbar, right to left: close, the host's search field, then the text
// chat toggle, 8 DIP apart. The search slot is dropped (empty) when it would
// leave the title less than kUiCallHeaderTitleMinimum or be narrower than
// kUiCallHeaderSearchMinimum.
inline constexpr float kUiCallHeaderControl = 32, kUiCallHeaderGap = 8, kUiCallHeaderSearchHeight = 32,
                       kUiCallHeaderSearchMinimum = 120, kUiCallHeaderTitleMinimum = 96;
struct UiCallHeaderLayout {
    UiRect close, chat, search;
};
UiCallHeaderLayout ui_call_header_layout(UiRect header, bool chat, float search_width) noexcept;

// Projects content into viewport. now_ms/reduced_motion drive the tile morph
// started by a layout change (focus, fullscreen, membership, viewport).
bool ui_call_scene_update(UiCallScene*, const UiCallContent&, UiRect viewport, u64 now_ms,
                          bool reduced_motion) noexcept;
void ui_call_scene_release(UiCallScene*) noexcept;
u64 ui_call_scene_next_wake(const UiCallScene&, u64 now_ms, bool reduced_motion) noexcept;
bool ui_call_scene_contains(const UiCallScene&, float x, float y) noexcept;
UiCallHit ui_call_scene_hit_test(const UiCallScene&, float x, float y) noexcept;
// Hover state for paint; true when it changed.
bool ui_call_scene_set_hover(UiCallScene*, float x, float y) noexcept;
bool ui_call_scene_set_pressed(UiCallScene*, UiCallControl) noexcept;
// Scrolls the grid (or the filmstrip under the point); true when it moved.
bool ui_call_scene_scroll(UiCallScene*, float x, float y, float delta) noexcept;
bool ui_call_control_enabled(const UiCallScene&, UiCallControl) noexcept;
// Bar grouping: controls sharing a group sit kUiCallGroupGap apart.
u32 ui_call_control_group(UiCallControl) noexcept;
// Identity colour for a participant (stable per user): the avatar-less
// initials disc, and at a low alpha the tint over an avatar tile's plate.
Color ui_call_identity_color(const Theme&, u64 user_id) noexcept;
Color ui_call_tile_tint(const Theme&, u64 user_id) noexcept;
// Toggled controls (muted, deafened, fullscreen) paint in their active style.
bool ui_call_control_active(const UiCallScene&, UiCallControl) noexcept;
// Tooltip text for a control at its current state.
Utf8View ui_call_control_label(const UiCallScene&, UiCallControl) noexcept;
Utf8View ui_call_scene_tooltip(const UiCallScene&, float x, float y, UiRect* anchor, u32* identity) noexcept;
// A tile whose settled rectangle intersects its viewport.
bool ui_call_scene_tile_visible(const UiCallScene&, u32 tile) noexcept;
const UiCallTile* ui_call_scene_find_tile(const UiCallScene&, u64 user_id) noexcept;
bool ui_call_scene_tile_media(const UiCallScene&, u32 tile, UiCallTileMedia* output) noexcept;
// Stage avatars outside the tiles (audience band, then hand-raise rows):
// output->avatar is the circle; false past the last one.
bool ui_call_scene_stage_avatar(const UiCallScene&, u32 index, UiCallTileMedia* output) noexcept;
// True while a visible stream tile paints a loading spinner.
bool ui_call_scene_spinner_visible(const UiCallScene&) noexcept;
// Rectangle of a tile control (stream_* or tile_fullscreen) for anchoring
// menus and tooltips; empty when that tile does not show it now.
UiRect ui_call_scene_tile_control(const UiCallScene&, u64 user_id, UiCallControl) noexcept;
// Background, tile plates and avatar placeholders (below media).
bool ui_call_scene_paint(const UiCallScene&, const Theme&, PaintStream*) noexcept;
// Speaking rings, name pills, badges, hover buttons and the control bar (above media).
bool ui_call_scene_paint_foreground(const UiCallScene&, const Theme&, PaintStream*) noexcept;
bool ui_call_scene_text_output(const UiCallScene&, const Theme&, UiTextOutput*) noexcept;
} // namespace maicord
