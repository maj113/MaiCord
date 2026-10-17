#include "maicord/ui/call_scene.hpp"
#include "call_stage.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/spinner.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/text_style.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr float kTileRadius = 8, kPillHeight = 24,
                kPillInset = 8, kHoverButton = 28, kLiveWidth = 40, kLiveHeight = 20, kNameEm = 6.8f,
                kFullscreenButton = 40, kIndicator = 16, kStopWidth = 136, kWatchWidth = 136, kWatchHeight = 36,
                kViewerHeight = 20, kSpinner = 32;
constexpr UiCallControl kBarOrder[]{UiCallControl::mute, UiCallControl::deafen, UiCallControl::camera,
                                    UiCallControl::screen_share, UiCallControl::disconnect};
constexpr UiMotionStyle kMorphStyle{kUiCallMorphMs, UiMotionCurve::spring};
constexpr u64 kMorphOwner = 0xca11'5ce4'e000'0001ull;

using call_stage::centered;
using call_stage::copy_text;
using call_stage::push_text;

float clampf(float value, float low, float high) noexcept {
    return value < low ? low : value > high ? high : value;
}
u32 codepoints(Utf8View text) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < text.bytes; ++i)
        count += (static_cast<u8>(text.data[i]) & 0xc0) != 0x80;
    return count;
}
UiMotionScope morph_scope(const UiCallScene& s) noexcept {
    return {kMorphOwner, s.morph_generation};
}
bool initialize(UiCallScene& s) noexcept {
    if (s.owner.manager == &s.widgets)
        return true;
    if (!ui_widget_manager_initialize(&s.widgets, {s.widget_storage, 1}, {s.layout_storage, 1}))
        return false;
    auto style = ui_layout_style();
    style.flow = UiLayoutFlow::overlay;
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.flags = ui_layout_node_visible | ui_layout_node_allow_overlap | ui_layout_node_clip_children;
    const auto root = ui_widget_manager_add(
        &s.widgets, ui_widget_invalid_handle(),
        {1, UiWidgetKind::container, ui_widget_none, {utf8_literal("Call"), style, {}, 0, 1}});
    if (!root.ok())
        return false;
    s.owner = {&s.widgets, root.handle};
    return true;
}
const UiCallParticipant* participant_of(const UiCallScene& s, const UiCallTile& tile) noexcept {
    return tile.participant < s.participant_count ? &s.participants[tile.participant] : nullptr;
}
bool stream_tile(const UiCallParticipant& p) noexcept {
    return p.stream != UiCallStream::none;
}
// The watched stream (connecting, live or reconnecting).
bool watched(const UiCallParticipant& p) noexcept {
    return p.stream == UiCallStream::connecting || p.stream == UiCallStream::live ||
           p.stream == UiCallStream::reconnecting;
}
bool video_ready(const UiCallParticipant& p) noexcept {
    if (stream_tile(p) && p.stream != UiCallStream::live && p.stream != UiCallStream::reconnecting)
        return false;
    return p.video_ssrc && p.video_width && p.video_height;
}
UiRect tile_hover_button(const UiCallTile& tile) noexcept {
    const auto r = tile.display;
    if (ui_rect_width(r) < 120 || ui_rect_height(r) < 72)
        return {};
    return {r.right - kPillInset - kHoverButton, r.top + kPillInset, r.right - kPillInset,
            r.top + kPillInset + kHoverButton};
}
// Per-tile buttons. Fullscreen and the watched stream's bar (volume, stop)
// appear on hover; "Watch stream" and an ended tile's close stay visible.
struct TileControls {
    UiRect fullscreen, watch, stop, volume, dismiss;
    UiRect move; // Stage moderators: move this speaker to the audience.
    bool stop_compact; // Icon-only "Stop watching" on narrow tiles.
};
TileControls tile_controls(const UiCallTile& tile, const UiCallParticipant& p, bool hovered) noexcept {
    TileControls c{};
    const auto r = tile.display;
    const float w = ui_rect_width(r), h = ui_rect_height(r);
    if (p.stream == UiCallStream::ended) {
        c.dismiss = tile_hover_button(tile);
        return c;
    }
    if (p.stream == UiCallStream::available) {
        if (w >= 100 && h >= 56) {
            const float bw = minimum(kWatchWidth, w - 2 * kPillInset), bh = h >= 110 ? kWatchHeight : 28.f;
            c.watch = centered((r.left + r.right) * .5f, (r.top + r.bottom) * .5f, bw, bh);
        }
        return c;
    }
    if (!hovered)
        return c;
    c.fullscreen = tile_hover_button(tile);
    if (!watched(p) || w < 120 || h < 72)
        return c;
    c.stop_compact = w < 320;
    const float stop = c.stop_compact ? kHoverButton : kStopWidth;
    const float bottom = r.bottom - kPillInset;
    c.stop = {r.right - kPillInset - stop, bottom - kHoverButton, r.right - kPillInset, bottom};
    if (w >= 180)
        c.volume = {c.stop.left - 6 - kHoverButton, bottom - kHoverButton, c.stop.left - 6, bottom};
    return c;
}
// Stage moderators get "Move to audience" left of a hovered speaker's
// fullscreen button.
TileControls tile_controls(const UiCallScene& s, const UiCallTile& tile, const UiCallParticipant& p,
                           bool hovered) noexcept {
    auto c = tile_controls(tile, p, hovered);
    if (s.stage_active && (s.stage_actions & stage_action_move_others) && hovered && !p.self && !stream_tile(p) &&
        ui_rect_has_area(c.fullscreen))
        c.move = {c.fullscreen.left - 6 - kHoverButton, c.fullscreen.top, c.fullscreen.left - 6, c.fullscreen.bottom};
    return c;
}
UiCallControl tile_control_at(const TileControls& c, float x, float y) noexcept {
    if (ui_rect_contains(c.move, x, y))
        return UiCallControl::stage_move;
    if (ui_rect_contains(c.watch, x, y))
        return UiCallControl::stream_watch;
    if (ui_rect_contains(c.dismiss, x, y))
        return UiCallControl::stream_dismiss;
    if (ui_rect_contains(c.stop, x, y))
        return UiCallControl::stream_stop;
    if (ui_rect_contains(c.volume, x, y))
        return UiCallControl::stream_volume;
    if (ui_rect_contains(c.fullscreen, x, y))
        return UiCallControl::tile_fullscreen;
    return UiCallControl::none;
}
u32 indicator_count(const UiCallParticipant& p) noexcept {
    return (p.muted ? 1u : 0u) + (p.deafened ? 1u : 0u);
}
// Name pill at the bottom-left, left of a stream's hover bar; empty on tiles
// too small to label. Stream pills lead with a screen-share glyph.
UiRect name_pill(const UiCallTile& tile, const UiCallParticipant& p, const TileControls& controls) noexcept {
    const auto r = tile.display;
    if (ui_rect_width(r) < 72 || ui_rect_height(r) < kPillHeight + 2 * kPillInset)
        return {};
    const float icons = stream_tile(p) ? kIndicator + 4 : static_cast<float>(indicator_count(p)) * (kIndicator + 4);
    const float text = static_cast<float>(codepoints(p.name)) * kNameEm;
    float right = r.right - kPillInset;
    if (ui_rect_has_area(controls.volume))
        right = controls.volume.left - 6;
    else if (ui_rect_has_area(controls.stop))
        right = controls.stop.left - 6;
    const float width = minimum(right - r.left - kPillInset, 20 + text + icons);
    if (width < 32)
        return {};
    return {r.left + kPillInset, r.bottom - kPillInset - kPillHeight, r.left + kPillInset + width,
            r.bottom - kPillInset};
}
UiRect live_badge(const UiCallTile& tile, const UiCallParticipant& p) noexcept {
    const auto r = tile.display;
    const bool shown = stream_tile(p) ? p.stream != UiCallStream::ended : p.live;
    if (!shown || ui_rect_width(r) < 72 || ui_rect_height(r) < kLiveHeight + 2 * kPillInset)
        return {};
    return {r.left + kPillInset, r.top + kPillInset, r.left + kPillInset + kLiveWidth,
            r.top + kPillInset + kLiveHeight};
}
// Soundboard effect (emoji and sound name) centred along the tile's top edge.
UiRect effect_pill(const UiCallTile& tile, const UiCallParticipant& p) noexcept {
    const auto r = tile.display;
    if (!p.effect.bytes || ui_rect_width(r) < 96 || ui_rect_height(r) < kPillHeight + 2 * kPillInset)
        return {};
    const float width = minimum(ui_rect_width(r) - 2 * kPillInset - 2 * kLiveWidth,
                                24 + static_cast<float>(codepoints(p.effect)) * kNameEm);
    if (width < 32)
        return {};
    const float middle = (r.left + r.right) * .5f;
    return {middle - width * .5f, r.top + kPillInset, middle + width * .5f, r.top + kPillInset + kPillHeight};
}
u32 digit_count(u32 value) noexcept {
    u32 digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}
// Viewer count (eye + number) beside the watched stream's LIVE badge.
UiRect viewer_pill(const UiCallTile& tile, const UiCallParticipant& p) noexcept {
    const auto live = live_badge(tile, p);
    if (!ui_rect_has_area(live) || !watched(p) || !p.viewers || ui_rect_width(tile.display) < 160)
        return {};
    const float width = 6 + 14 + 4 + static_cast<float>(digit_count(p.viewers)) * 7.5f + 8;
    return {live.right + 4, live.top, live.right + 4 + width, live.top + kViewerHeight};
}
// A spinner while a watched stream has no picture to show yet (or reconnects).
bool stream_spinner(const UiCallParticipant& p) noexcept {
    return p.stream == UiCallStream::connecting || p.stream == UiCallStream::reconnecting ||
           (p.stream == UiCallStream::live && !p.paused && !video_ready(p));
}
// Copy under the state glyph; empty when the state needs none.
Utf8View stream_status(const UiCallParticipant& p) noexcept {
    switch (p.stream) {
    case UiCallStream::reconnecting:
        return utf8_literal("Reconnecting\xE2\x80\xA6");
    case UiCallStream::live:
        return p.paused ? utf8_literal("Stream paused") : Utf8View{};
    case UiCallStream::ended:
        return ui_call_stream_notice_text(p.notice);
    default:
        return {};
    }
}
// A dimming scrim over the stream's media: every state but plain live video.
bool stream_scrim(const UiCallParticipant& p) noexcept {
    return stream_tile(p) && !(p.stream == UiCallStream::live && !p.paused && video_ready(p));
}
UiRect state_glyph(const UiCallTile& tile, bool with_text) noexcept {
    const auto r = tile.display;
    const bool large = ui_rect_height(r) >= 120;
    const float extent = large ? kSpinner : 24.f;
    return centered((r.left + r.right) * .5f, (r.top + r.bottom) * .5f - (with_text && large ? 12.f : 0.f), extent,
                    extent);
}
UiRect status_line(const UiCallTile& tile, bool glyph) noexcept {
    const auto r = tile.display;
    if (ui_rect_height(r) < 72)
        return {};
    const float top = glyph ? state_glyph(tile, true).bottom + 6 : (r.top + r.bottom) * .5f - 10;
    return {r.left + 12, top, r.right - 12, top + 20};
}
// The glyph drawn with the state copy: pause for a paused stream, a muted
// screen-share-off for an ended one; spinners are drawn separately.
bool status_glyph(const UiCallParticipant& p, UiIcon* icon) noexcept {
    if (p.stream == UiCallStream::live && p.paused) {
        *icon = UiIcon::pause;
        return true;
    }
    if (p.stream == UiCallStream::ended) {
        *icon = UiIcon::screen_share_off;
        return true;
    }
    return false;
}
UiRect avatar_rect(const UiCallTile& tile) noexcept {
    const auto r = tile.display;
    const float extent = clampf(minimum(ui_rect_width(r), ui_rect_height(r)) * .4f, 24, tile.focused ? 128.f : 96.f);
    return centered((r.left + r.right) * .5f, (r.top + r.bottom) * .5f, extent, extent);
}
UiRect visible_part(const UiCallTile& tile) noexcept {
    return ui_rect_intersection(tile.display, tile.clip);
}
u64 hash_rect(u64 hash, UiRect r) noexcept {
    return hash64_bytes(hash, &r, sizeof(r));
}
void place_controls(UiCallScene& s) noexcept {
    for (auto& control : s.controls)
        control = {};
    const float middle = (s.bar.top + s.bar.bottom) * .5f;
    UiCallControl order[8];
    u32 order_count = 0;
    if (s.stage_active)
        order_count = call_stage::bar_order(s, order, 8);
    else
        for (const auto control : kBarOrder) {
            // The soundboard sits with camera and screen share, before disconnect.
            if (control == UiCallControl::disconnect && s.soundboard_available)
                order[order_count++] = UiCallControl::soundboard;
            order[order_count++] = control;
        }
    const auto gap_before = [&](u32 i) noexcept {
        return !i ? 0.f
                  : ui_call_control_group(order[i]) == ui_call_control_group(order[i - 1]) ? kUiCallGroupGap
                                                                                           : kUiCallControlGap;
    };
    float width = 0;
    for (u32 i = 0; i < order_count; ++i)
        width += gap_before(i) + (order[i] == UiCallControl::disconnect ? kUiCallDisconnectWidth : kUiCallButton);
    float x = (s.bar.left + s.bar.right - width) * .5f;
    for (u32 i = 0; i < order_count; ++i) {
        const auto control = order[i];
        x += gap_before(i);
        const float w = control == UiCallControl::disconnect ? kUiCallDisconnectWidth : kUiCallButton;
        s.controls[static_cast<u32>(control)] = {x, middle - kUiCallButton * .5f, x + w, middle + kUiCallButton * .5f};
        x += w;
    }
    x += kUiCallControlGap;
    const float right = s.bar.right - kUiCallStageInset;
    if (right - kFullscreenButton > x)
        s.controls[static_cast<u32>(UiCallControl::fullscreen)] = {
            right - kFullscreenButton, middle - kFullscreenButton * .5f, right, middle + kFullscreenButton * .5f};
    s.search_field = {};
    if (!s.fullscreen) {
        const auto header = ui_call_header_layout(s.header, s.chat_available, s.search_width);
        s.controls[static_cast<u32>(UiCallControl::close)] = header.close;
        s.controls[static_cast<u32>(UiCallControl::chat)] = header.chat;
        s.search_field = header.search;
    }
    call_stage::place_controls(s);
}
} // namespace

UiCallHeaderLayout ui_call_header_layout(UiRect header, bool chat, float search_width) noexcept {
    UiCallHeaderLayout layout{};
    if (!ui_rect_has_area(header))
        return layout;
    const float middle = (header.top + header.bottom) * .5f;
    const auto slot = [&](float right, float width, float height) noexcept {
        return UiRect{right - width, middle - height * .5f, right, middle + height * .5f};
    };
    float right = header.right - 12;
    layout.close = slot(right, kUiCallHeaderControl, kUiCallHeaderControl);
    right = layout.close.left - kUiCallHeaderGap;
    // Speaker glyph and title lead the header; the title keeps a readable minimum.
    const float title_floor = header.left + 44 + kUiCallHeaderTitleMinimum + 12;
    const float chat_extent = chat ? kUiCallHeaderControl + kUiCallHeaderGap : 0.f;
    const float room = right - chat_extent - title_floor;
    const float width = search_width > 0 && search_width < room ? search_width : room;
    if (search_width > 0 && width >= kUiCallHeaderSearchMinimum) {
        layout.search = slot(right, width, kUiCallHeaderSearchHeight);
        right = layout.search.left - kUiCallHeaderGap;
    }
    if (chat && right - kUiCallHeaderControl >= header.left + 44)
        layout.chat = slot(right, kUiCallHeaderControl, kUiCallHeaderControl);
    return layout;
}

namespace {
// Speakers' area on a stage, otherwise the whole stage rectangle.
UiRect tile_area(const UiCallScene& s) noexcept {
    return s.stage_active ? s.stage_speakers : s.stage;
}
// Settled tile rectangles for the current participants, focus and scroll.
void place_tiles(UiCallScene& s, UiRect area) noexcept {
    const u32 count = s.participant_count;
    s.tile_count = 0;
    s.content_height = s.strip_content_width = 0;
    if (!count || !ui_rect_has_area(area))
        return;
    u32 focused = count;
    for (u32 i = 0; s.focus_user && i < count; ++i)
        if (s.participants[i].user_id == s.focus_user)
            focused = i;
    if (focused == count) {
        s.focus_user = 0;
        UiRect rects[kUiCallMaximumTiles];
        UiCallGridLayout layout{};
        const auto measured = ui_call_grid_measure(ui_rect_width(area), ui_rect_height(area), count, s.gap);
        s.scroll = clampf(s.scroll, 0, maximum(0.f, measured.content_height - ui_rect_height(area)));
        if (!ui_call_grid_place(area, count, s.gap, s.scroll, {rects, count}, &layout))
            return;
        s.content_height = layout.content_height;
        for (u32 i = 0; i < count; ++i)
            s.tiles[s.tile_count++] = {s.participants[i].user_id, rects[i], rects[i], rects[i], area,
                                       static_cast<u16>(i), false, false};
        s.strip = {};
        return;
    }
    s.scroll = 0;
    if (count == 1) {
        s.tiles[s.tile_count++] = {s.focus_user, area, area, area, area, static_cast<u16>(focused), true, false};
        s.strip = {};
        return;
    }
    const auto focus = ui_call_focus_measure(area, count - 1, s.gap);
    s.strip = focus.strip;
    s.tiles[s.tile_count++] = {s.focus_user, focus.stage, focus.stage, focus.stage, focus.stage,
                               static_cast<u16>(focused), true, false};
    s.strip_content_width = focus.strip_content_width;
    const float overflow = maximum(0.f, focus.strip_content_width - ui_rect_width(focus.strip));
    s.strip_scroll = clampf(s.strip_scroll, 0, overflow);
    float x = overflow > 0 ? focus.strip.left - s.strip_scroll
                           : (focus.strip.left + focus.strip.right - focus.strip_content_width) * .5f;
    for (u32 i = 0; i < count; ++i) {
        if (i == focused)
            continue;
        const UiRect r{x, focus.strip.top, x + focus.strip_tile_width, focus.strip.top + focus.strip_tile_height};
        s.tiles[s.tile_count++] = {s.participants[i].user_id, r, r, r, focus.strip, static_cast<u16>(i), false, true};
        x += focus.strip_tile_width + s.gap;
    }
}
void apply_morph(UiCallScene& s) noexcept {
    const float t = s.morph.active || s.morph.value < 1 ? clampf(s.morph.value, 0, 1) : 1;
    for (u32 i = 0; i < s.tile_count; ++i)
        s.tiles[i].display = t >= 1 ? s.tiles[i].target : ui_rect_lerp(s.tiles[i].from, s.tiles[i].target, t);
}
// Controls drawn on a tile (hit with its user ID) rather than in the chrome.
bool per_tile(UiCallControl control) noexcept {
    return control == UiCallControl::tile_fullscreen || control == UiCallControl::stream_watch ||
           control == UiCallControl::stream_stop || control == UiCallControl::stream_volume ||
           control == UiCallControl::stream_dismiss || control == UiCallControl::stage_move ||
           control == UiCallControl::stage_invite;
}
// Stage buttons painted (with their labels) by call_stage, not the bar loop.
bool stage_chrome(UiCallControl control) noexcept {
    return control == UiCallControl::stage_start || control == UiCallControl::stage_topic ||
           control == UiCallControl::stage_end || control == UiCallControl::stage_accept ||
           control == UiCallControl::stage_decline;
}
bool control_visible(const UiCallScene& s, UiCallControl control) noexcept {
    return control != UiCallControl::none && control < UiCallControl::count &&
           ui_rect_has_area(s.controls[static_cast<u32>(control)]);
}
} // namespace

UiCallGridLayout ui_call_grid_measure(float width, float height, u32 count, float gap, float minimum_width) noexcept {
    UiCallGridLayout best{};
    if (!count || !(width > 0) || !(height > 0) || !(gap >= 0))
        return best;
    for (u32 columns = 1; columns <= count; ++columns) {
        const u32 rows = (count + columns - 1) / columns;
        float tile_width = (width - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
        float tile_height = tile_width / kUiCallTileAspect;
        const float rows_height = tile_height * static_cast<float>(rows) + gap * static_cast<float>(rows - 1);
        if (rows_height > height) {
            tile_height = (height - gap * static_cast<float>(rows - 1)) / static_cast<float>(rows);
            tile_width = tile_height * kUiCallTileAspect;
        }
        if (tile_width <= 0 || tile_height <= 0)
            continue;
        if (tile_width > best.tile_width + .01f)
            best = {columns, rows, tile_width, tile_height, 0};
    }
    if (count > 1 && best.tile_width < minimum_width && minimum_width > 0) {
        // Too many to fit: keep the minimum tile and scroll vertically.
        u32 columns = static_cast<u32>((width + gap) / (minimum_width + gap));
        columns = columns < 1 ? 1 : columns > count ? count : columns;
        best.columns = columns;
        best.rows = (count + columns - 1) / columns;
        best.tile_width = (width - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
        best.tile_height = best.tile_width / kUiCallTileAspect;
    }
    if (best.columns)
        best.content_height =
            best.tile_height * static_cast<float>(best.rows) + gap * static_cast<float>(best.rows - 1);
    return best;
}

bool ui_call_grid_place(UiRect area, u32 count, float gap, float scroll, Slice<UiRect> tiles,
                        UiCallGridLayout* output, float minimum_width) noexcept {
    if (!count || tiles.count < count || !tiles.data || !ui_rect_has_area(area))
        return false;
    const auto layout = ui_call_grid_measure(ui_rect_width(area), ui_rect_height(area), count, gap, minimum_width);
    if (!layout.columns)
        return false;
    const float height = ui_rect_height(area);
    const bool scrolls = layout.content_height > height + .5f;
    const float top = scrolls ? area.top - clampf(scroll, 0, layout.content_height - height)
                              : area.top + (height - layout.content_height) * .5f;
    for (u32 i = 0; i < count; ++i) {
        const u32 row = i / layout.columns, column = i % layout.columns;
        const u32 in_row = row + 1 < layout.rows ? layout.columns : count - row * layout.columns;
        const float row_width =
            layout.tile_width * static_cast<float>(in_row) + gap * static_cast<float>(in_row - 1);
        const float left = area.left + (ui_rect_width(area) - row_width) * .5f +
                           static_cast<float>(column) * (layout.tile_width + gap);
        const float y = top + static_cast<float>(row) * (layout.tile_height + gap);
        tiles[i] = {left, y, left + layout.tile_width, y + layout.tile_height};
    }
    if (output)
        *output = layout;
    return true;
}

UiCallFocusLayout ui_call_focus_measure(UiRect area, u32 others, float gap) noexcept {
    UiCallFocusLayout layout{area, {}, 0, 0, 0};
    if (!others || !ui_rect_has_area(area))
        return layout;
    const float strip_height = clampf(ui_rect_height(area) * .18f, 64, 120);
    if (ui_rect_height(area) < strip_height * 2 + gap)
        return layout;
    layout.strip = {area.left, area.bottom - strip_height, area.right, area.bottom};
    layout.stage = {area.left, area.top, area.right, layout.strip.top - gap};
    layout.strip_tile_height = strip_height;
    layout.strip_tile_width = strip_height * kUiCallTileAspect;
    layout.strip_content_width =
        layout.strip_tile_width * static_cast<float>(others) + gap * static_cast<float>(others - 1);
    return layout;
}

UiRect ui_call_video_fit(UiRect tile, u32 width, u32 height, bool cover) noexcept {
    if (!width || !height || !ui_rect_has_area(tile))
        return {};
    const float w = ui_rect_width(tile), h = ui_rect_height(tile);
    const float sx = w / static_cast<float>(width), sy = h / static_cast<float>(height);
    const float scale = cover ? maximum(sx, sy) : minimum(sx, sy);
    return centered((tile.left + tile.right) * .5f, (tile.top + tile.bottom) * .5f,
                    static_cast<float>(width) * scale, static_cast<float>(height) * scale);
}

bool ui_call_scene_update(UiCallScene* s, const UiCallContent& content, UiRect viewport, u64 now_ms,
                          bool reduced_motion) noexcept {
    if (!s || !ui_rect_valid(viewport) || ui_rect_width(viewport) < 320 || ui_rect_height(viewport) < 240 ||
        !initialize(*s)) {
        if (s)
            s->active = false;
        return false;
    }
    UiRect previous[kUiCallMaximumTiles], previous_from[kUiCallMaximumTiles];
    u64 previous_users[kUiCallMaximumTiles];
    const u32 previous_count = s->active ? s->tile_count : 0;
    for (u32 i = 0; i < previous_count; ++i) {
        previous[i] = s->tiles[i].display;
        previous_from[i] = s->tiles[i].from;
        previous_users[i] = s->tiles[i].user_id;
    }
    s->participant_count = minimum<u32>(content.participants.count, kUiCallMaximumTiles);
    for (u32 i = 0; i < s->participant_count; ++i) {
        auto p = content.participants[i];
        const u32 bytes = copy_text(s->names[i], kUiCallNameBytes, p.name);
        p.name = {s->names[i], bytes};
        p.effect = {s->effects[i], copy_text(s->effects[i], kUiCallEffectBytes, p.effect)};
        s->participants[i] = p;
        // Viewer counts are formatted once per projection, not per paint.
        u32 value = p.viewers, digits = digit_count(value);
        s->viewer_text[i][digits] = 0;
        for (u32 d = digits; d > 0; --d, value /= 10)
            s->viewer_text[i][d - 1] = static_cast<char>('0' + value % 10);
    }
    s->title_bytes = copy_text(s->title, kUiCallTitleBytes, content.title);
    s->focus_user = content.focus_user;
    s->fullscreen = content.fullscreen;
    s->connected = content.connected;
    s->self_mute = content.self_mute;
    s->self_deaf = content.self_deaf;
    s->server_mute = content.server_mute;
    s->server_deaf = content.server_deaf;
    s->chat_available = content.chat_available;
    s->chat_open = content.chat_available && content.chat_open;
    s->soundboard_available = content.soundboard_available;
    s->soundboard_open = content.soundboard_available && content.soundboard_open;
    s->search_width = content.search_width > 0 && content.search_width < 4096 ? content.search_width : 0;
    s->gap = content.gap >= 0 && content.gap <= 48 ? content.gap : 8;
    s->viewport = viewport;
    s->header = {viewport.left, viewport.top, viewport.right, viewport.top + kUiCallHeaderHeight};
    s->bar = {viewport.left, viewport.bottom - kUiCallBarHeight, viewport.right, viewport.bottom};
    s->stage = {viewport.left + kUiCallStageInset, s->header.bottom + kUiCallStageInset * .5f,
                viewport.right - kUiCallStageInset, s->bar.top};
    call_stage::copy(*s, content.stage);
    call_stage::layout(*s, s->stage, s->gap);
    place_controls(*s);
    place_tiles(*s, tile_area(*s));
    if (s->pressed != UiCallControl::none && !control_visible(*s, s->pressed) && !per_tile(s->pressed))
        s->pressed = UiCallControl::none;
    if (s->hovered != UiCallControl::none && !control_visible(*s, s->hovered) && !per_tile(s->hovered))
        s->hovered = UiCallControl::none;
    if (!ui_widget_manager_layout_rectangle(&s->widgets, viewport).ok()) {
        s->active = false;
        return false;
    }
    // A layout change (not a scroll) morphs every surviving tile from where it is drawn.
    u64 key = hash64_mix(kHash64Seed, s->focus_user);
    key = hash64_mix(key, s->fullscreen);
    key = hash_rect(key, s->stage);
    for (u32 i = 0; i < s->tile_count; ++i)
        key = hash64_mix(key, s->tiles[i].user_id);
    const bool relayout = s->active && key != s->layout_key && previous_count;
    const bool morphing = s->active && (s->morph.active || s->morph.value < 1);
    s->layout_key = key;
    const auto source = [&](u64 user, const UiRect* rects, UiRect fallback) noexcept {
        for (u32 j = 0; j < previous_count; ++j)
            if (previous_users[j] == user)
                return rects[j];
        return fallback;
    };
    if (relayout && !reduced_motion) {
        for (u32 i = 0; i < s->tile_count; ++i)
            s->tiles[i].from = source(s->tiles[i].user_id, previous, s->tiles[i].target);
        ++s->morph_generation;
        ui_motion_transition_reset(&s->morph, morph_scope(*s), 0);
        ui_motion_transition_tick(&s->morph, morph_scope(*s), 1, now_ms, kMorphStyle, reduced_motion, true);
    } else if (morphing && !reduced_motion) {
        // A re-projection keeps an in-flight morph's sources.
        for (u32 i = 0; i < s->tile_count; ++i)
            s->tiles[i].from = source(s->tiles[i].user_id, previous_from, s->tiles[i].target);
        ui_motion_transition_tick(&s->morph, morph_scope(*s), 1, now_ms, kMorphStyle, reduced_motion, true);
    } else {
        ++s->morph_generation;
        ui_motion_transition_reset(&s->morph, morph_scope(*s), 1);
    }
    apply_morph(*s);
    u64 hash = kHash64Seed;
    for (u32 i = 0; i < s->participant_count; ++i) {
        const auto& p = s->participants[i];
        hash = hash64_mix(hash, p.user_id);
        hash = hash64_mix(hash, (u64{p.video_ssrc} << 32) | (u64{p.video_width} << 16) | p.video_height);
        hash = hash64_mix(hash, (u64{p.speaking} << 0) | (u64{p.muted} << 1) | (u64{p.deafened} << 2) |
                                    (u64{p.live} << 3) | (u64{p.self} << 4) | (u64{p.paused} << 5) |
                                    (u64{static_cast<u8>(p.stream)} << 8) | (u64{static_cast<u8>(p.notice)} << 16) |
                                    (u64{p.viewers} << 32));
        hash = hash64_bytes(hash, &p.preview, sizeof(p.preview));
        hash = hash64_mix(hash, p.has_avatar);
        hash = hash64_counted_bytes(hash, s->names[i], p.name.bytes);
        hash = hash64_counted_bytes(hash, s->effects[i], p.effect.bytes);
    }
    hash = hash64_counted_bytes(hash, s->title, s->title_bytes);
    hash = hash_rect(hash, viewport);
    for (u32 i = 0; i < s->tile_count; ++i)
        hash = hash_rect(hash, s->tiles[i].display);
    hash = hash64_mix(hash, (u64{s->fullscreen} << 0) | (u64{s->connected} << 1) | (u64{s->self_mute} << 2) |
                                (u64{s->self_deaf} << 3) | (u64{s->server_mute} << 4) | (u64{s->server_deaf} << 5) |
                                (u64{s->chat_available} << 6) | (u64{s->chat_open} << 7) |
                                (u64{s->soundboard_available} << 8) | (u64{s->soundboard_open} << 9));
    hash = hash_rect(hash, s->search_field);
    hash = call_stage::hash(*s, hash);
    if (!s->active || hash != s->content_hash) {
        s->content_hash = hash;
        ++s->revision;
    }
    s->active = true;
    return true;
}

void ui_call_scene_release(UiCallScene* s) noexcept {
    if (!s)
        return;
    const u32 revision = s->revision + 1;
    const u64 generation = s->morph_generation + 1;
    s->active = false;
    s->tile_count = s->participant_count = 0;
    s->hovered = s->pressed = UiCallControl::none;
    s->hovered_user = s->stage_hovered = 0;
    s->scroll = s->strip_scroll = 0;
    s->revision = revision;
    s->morph_generation = generation;
    ui_motion_transition_reset(&s->morph, morph_scope(*s), 1);
}

u64 ui_call_scene_next_wake(const UiCallScene& s, u64 now_ms, bool reduced_motion) noexcept {
    return s.active ? ui_motion_transition_next_wake(s.morph, morph_scope(s), now_ms, reduced_motion, true) : 0;
}

bool ui_call_scene_contains(const UiCallScene& s, float x, float y) noexcept {
    return s.active && ui_rect_contains(s.viewport, x, y) && !ui_rect_contains(s.search_field, x, y);
}

UiCallHit ui_call_scene_hit_test(const UiCallScene& s, float x, float y) noexcept {
    UiCallHit hit{};
    if (!ui_call_scene_contains(s, x, y))
        return hit;
    for (u32 i = 1; i < kUiCallControlCount; ++i)
        if (ui_rect_contains(s.controls[i], x, y)) {
            hit.control = static_cast<UiCallControl>(i);
            return hit;
        }
    if (const auto row = call_stage::hit(s, x, y); row.control != UiCallControl::none)
        return row;
    for (u32 i = 0; i < s.tile_count; ++i) {
        const auto& tile = s.tiles[i];
        if (!ui_rect_contains(visible_part(tile), x, y))
            continue;
        hit.user_id = tile.user_id;
        if (const auto* p = participant_of(s, tile))
            hit.control = tile_control_at(tile_controls(s, tile, *p, s.hovered_user == tile.user_id), x, y);
        return hit;
    }
    return hit;
}

bool ui_call_scene_set_hover(UiCallScene* s, float x, float y) noexcept {
    if (!s)
        return false;
    UiCallControl control = UiCallControl::none;
    u64 user = 0, request = 0;
    if (s->active && ui_rect_contains(s->viewport, x, y)) {
        for (u32 i = 0; i < s->tile_count && !user; ++i)
            if (ui_rect_contains(visible_part(s->tiles[i]), x, y))
                user = s->tiles[i].user_id;
        const u64 previous = s->hovered_user;
        s->hovered_user = user;
        const auto hit = ui_call_scene_hit_test(*s, x, y);
        control = hit.control;
        s->hovered_user = previous;
        if (control == UiCallControl::stage_invite)
            request = hit.user_id;
    }
    if (control == s->hovered && user == s->hovered_user && request == s->stage_hovered)
        return false;
    s->hovered = control;
    s->hovered_user = user;
    s->stage_hovered = request;
    ++s->revision;
    return true;
}

bool ui_call_scene_set_pressed(UiCallScene* s, UiCallControl control) noexcept {
    if (!s || s->pressed == control)
        return false;
    s->pressed = control;
    ++s->revision;
    return true;
}

bool ui_call_scene_scroll(UiCallScene* s, float x, float y, float delta) noexcept {
    if (!s || !s->active || !ui_coordinate_valid(delta))
        return false;
    if (s->focus_user) {
        const float overflow = maximum(0.f, s->strip_content_width - ui_rect_width(s->strip));
        if (!overflow || !ui_rect_contains(s->strip, x, y))
            return false;
        const float next = clampf(s->strip_scroll + delta, 0, overflow);
        if (next == s->strip_scroll)
            return false;
        s->strip_scroll = next;
    } else {
        const float overflow = maximum(0.f, s->content_height - ui_rect_height(tile_area(*s)));
        const float next = clampf(s->scroll + delta, 0, overflow);
        if (!overflow || next == s->scroll)
            return false;
        s->scroll = next;
    }
    // Re-place from the settled targets; a scroll never starts a morph.
    const u64 key = s->layout_key;
    place_tiles(*s, tile_area(*s));
    s->layout_key = key;
    for (u32 i = 0; i < s->tile_count; ++i)
        s->tiles[i].from = s->tiles[i].display = s->tiles[i].target;
    ++s->morph_generation;
    ui_motion_transition_reset(&s->morph, morph_scope(*s), 1);
    ++s->revision;
    return true;
}

u32 ui_call_control_group(UiCallControl control) noexcept {
    switch (control) {
    case UiCallControl::mute:
    case UiCallControl::deafen:
        return 1;
    case UiCallControl::camera:
    case UiCallControl::screen_share:
    case UiCallControl::soundboard:
        return 2;
    case UiCallControl::stage_hand:
    case UiCallControl::stage_become:
    case UiCallControl::stage_audience:
        return 3;
    default:
        return 0; // Ungrouped (disconnect, fullscreen, header).
    }
}

Color ui_call_identity_color(const Theme& theme, u64 user_id) noexcept {
    // A stable hue per user at a fixed chroma; dark enough for white initials.
    // Fibonacci hashing spreads consecutive snowflakes across the hue circle.
    const u64 mixed = (user_id * 0x9E3779B97F4A7C15ull) >> 40;
    const float hue = static_cast<float>(mixed % 360u) / 60.f;
    // Value .55 keeps white bold initials at about 3:1 even on the yellow hues.
    const float value = .55f, chroma = value * .55f;
    // HSV to RGB: the second-largest channel follows the hue's position in its sextant pair.
    const float pair = hue - 2 * static_cast<float>(static_cast<u32>(hue * .5f)); // [0, 2)
    const float secondary = chroma * (1 - (pair > 1 ? pair - 1 : 1 - pair));
    float red = 0, green = 0, blue = 0;
    switch (static_cast<u32>(hue) % 6u) {
    case 0: red = chroma, green = secondary; break;
    case 1: red = secondary, green = chroma; break;
    case 2: green = chroma, blue = secondary; break;
    case 3: green = secondary, blue = chroma; break;
    case 4: red = secondary, blue = chroma; break;
    default: red = chroma, blue = secondary; break;
    }
    const float base = value - chroma;
    Color color = theme.tokens.accent.brand;
    color.red = red + base;
    color.green = green + base;
    color.blue = blue + base;
    color.alpha = 1;
    return color;
}
Color ui_call_tile_tint(const Theme& theme, u64 user_id) noexcept {
    return ui_theme_color_alpha(ui_call_identity_color(theme, user_id),
                                theme.appearance == ThemeAppearance::light ? .20f : .26f);
}

bool ui_call_control_enabled(const UiCallScene&, UiCallControl control) noexcept {
    return control != UiCallControl::none && control != UiCallControl::camera &&
           control != UiCallControl::screen_share && control < UiCallControl::count;
}

bool ui_call_control_active(const UiCallScene& s, UiCallControl control) noexcept {
    switch (control) {
    case UiCallControl::mute:
        return s.self_mute || s.self_deaf || s.server_mute;
    case UiCallControl::deafen:
        return s.self_deaf || s.server_deaf;
    case UiCallControl::fullscreen:
        return s.fullscreen;
    case UiCallControl::stage_hand:
        return s.stage_hand;
    case UiCallControl::chat:
        return s.chat_open;
    case UiCallControl::soundboard:
        return s.soundboard_open;
    default:
        return false;
    }
}

Utf8View ui_call_control_label(const UiCallScene& s, UiCallControl control) noexcept {
    switch (control) {
    case UiCallControl::close:
        return utf8_literal("Close call view");
    case UiCallControl::chat:
        return s.chat_open ? utf8_literal("Hide Chat") : utf8_literal("Show Chat");
    case UiCallControl::mute:
        return s.server_mute                ? utf8_literal("Server muted")
               : s.self_mute || s.self_deaf ? utf8_literal("Unmute")
                                            : utf8_literal("Mute");
    case UiCallControl::deafen:
        return s.server_deaf ? utf8_literal("Server deafened")
               : s.self_deaf ? utf8_literal("Undeafen")
                             : utf8_literal("Deafen");
    case UiCallControl::camera:
        return utf8_literal("Camera isn't available yet");
    case UiCallControl::screen_share:
        return utf8_literal("Screen share isn't available yet");
    case UiCallControl::disconnect:
        return utf8_literal("Disconnect");
    case UiCallControl::fullscreen:
        return s.fullscreen ? utf8_literal("Exit full screen") : utf8_literal("Full screen");
    case UiCallControl::tile_fullscreen:
        return s.fullscreen && s.focus_user == s.hovered_user ? utf8_literal("Exit full screen")
                                                              : utf8_literal("Full screen");
    case UiCallControl::stream_watch:
        return utf8_literal("Watch stream");
    case UiCallControl::stream_stop:
        return utf8_literal("Stop watching");
    case UiCallControl::stream_volume:
        return utf8_literal("Stream volume");
    case UiCallControl::stream_dismiss:
        return utf8_literal("Close");
    case UiCallControl::soundboard:
        return utf8_literal("Soundboard");
    default:
        return call_stage::label(s, control);
    }
}

Utf8View ui_call_stream_notice_text(UiCallStreamNotice notice) noexcept {
    switch (notice) {
    case UiCallStreamNotice::ended:
        return utf8_literal("The stream has ended");
    case UiCallStreamNotice::full:
        return utf8_literal("This stream is full");
    case UiCallStreamNotice::not_allowed:
        return utf8_literal("You can't watch this stream");
    case UiCallStreamNotice::limited:
        return utf8_literal("Streams are limited in this server right now");
    case UiCallStreamNotice::timed_out:
        return utf8_literal("Couldn't connect to the stream");
    case UiCallStreamNotice::failed:
        return utf8_literal("The stream connection failed");
    case UiCallStreamNotice::interrupted:
        return utf8_literal("The stream was interrupted");
    default:
        return {};
    }
}

UiRect ui_call_scene_tile_control(const UiCallScene& s, u64 user_id, UiCallControl control) noexcept {
    const auto* tile = ui_call_scene_find_tile(s, user_id);
    const auto* p = tile ? participant_of(s, *tile) : nullptr;
    if (!p || !s.active)
        return {};
    const auto c = tile_controls(s, *tile, *p, s.hovered_user == user_id);
    switch (control) {
    case UiCallControl::tile_fullscreen:
        return c.fullscreen;
    case UiCallControl::stream_watch:
        return c.watch;
    case UiCallControl::stream_stop:
        return c.stop;
    case UiCallControl::stream_volume:
        return c.volume;
    case UiCallControl::stream_dismiss:
        return c.dismiss;
    case UiCallControl::stage_move:
        return c.move;
    default:
        return {};
    }
}

Utf8View ui_call_scene_tooltip(const UiCallScene& s, float x, float y, UiRect* anchor, u32* identity) noexcept {
    const auto hit = ui_call_scene_hit_test(s, x, y);
    if (hit.control == UiCallControl::none || hit.control == UiCallControl::stage_invite ||
        (stage_chrome(hit.control) && hit.control != UiCallControl::stage_topic))
        return {};
    UiRect area = s.controls[static_cast<u32>(hit.control)];
    if (per_tile(hit.control)) {
        // Labeled buttons need no tooltip; icon-only ones do.
        const auto* tile = ui_call_scene_find_tile(s, hit.user_id);
        const auto* p = tile ? participant_of(s, *tile) : nullptr;
        if (!p || hit.control == UiCallControl::stream_watch ||
            (hit.control == UiCallControl::stream_stop && !tile_controls(*tile, *p, true).stop_compact))
            return {};
        area = ui_call_scene_tile_control(s, hit.user_id, hit.control);
    }
    if (anchor)
        *anchor = area;
    if (identity)
        *identity = static_cast<u32>(hit.control) * 16 + (ui_call_control_active(s, hit.control) ? 1u : 0u) +
                    (s.server_mute ? 2u : 0u) + (s.server_deaf ? 4u : 0u) + (s.self_deaf ? 8u : 0u);
    return ui_call_control_label(s, hit.control);
}

bool ui_call_scene_tile_visible(const UiCallScene& s, u32 tile) noexcept {
    return s.active && tile < s.tile_count &&
           ui_rect_has_area(ui_rect_intersection(s.tiles[tile].target, s.tiles[tile].clip));
}

const UiCallTile* ui_call_scene_find_tile(const UiCallScene& s, u64 user_id) noexcept {
    for (u32 i = 0; user_id && i < s.tile_count; ++i)
        if (s.tiles[i].user_id == user_id)
            return &s.tiles[i];
    return nullptr;
}

bool ui_call_scene_tile_media(const UiCallScene& s, u32 index, UiCallTileMedia* output) noexcept {
    if (!output || !s.active || index >= s.tile_count)
        return false;
    const auto& tile = s.tiles[index];
    const auto* p = participant_of(s, tile);
    const UiRect clip = visible_part(tile);
    if (!p || !ui_rect_has_area(clip))
        return false;
    *output = {tile.user_id, 0, {}, clip, stream_tile(*p) ? UiRect{} : avatar_rect(tile), tile.display, kTileRadius,
               {}, false};
    if (video_ready(*p)) {
        output->video_ssrc = p->video_ssrc;
        // Shared screens are never cropped; cameras fill grid tiles.
        output->video =
            ui_call_video_fit(tile.display, p->video_width, p->video_height, !tile.focused && !stream_tile(*p));
    } else if (stream_tile(*p) && p->stream != UiCallStream::ended && media_asset_key_valid(p->preview)) {
        output->preview = p->preview;
        output->dim = p->stream != UiCallStream::available;
    }
    return true;
}

bool ui_call_scene_stage_avatar(const UiCallScene& s, u32 index, UiCallTileMedia* output) noexcept {
    return call_stage::avatar(s, index, output);
}

bool ui_call_scene_spinner_visible(const UiCallScene& s) noexcept {
    for (u32 i = 0; s.active && i < s.tile_count; ++i) {
        const auto* p = participant_of(s, s.tiles[i]);
        if (p && stream_spinner(*p) && ui_rect_has_area(visible_part(s.tiles[i])))
            return true;
    }
    return false;
}

bool ui_call_scene_paint(const UiCallScene& s, const Theme& theme, PaintStream* paint) noexcept {
    if (!s.active)
        return true;
    if (!paint)
        return false;
    const auto v = s.viewport;
    if (!paint_stream_push_clip(paint, v.left, v.top, v.right, v.bottom))
        return false;
    bool ok = ui_background_paint_canvas(paint, v, theme, theme.base_lowest, UiBackgroundTier::lowest);
    const float middle = (s.header.top + s.header.bottom) * .5f;
    ok = ok && ui_icon_paint(paint, s.stage_active ? UiIcon::podcast : UiIcon::speaker,
                             {s.header.left + 16, middle - 10, s.header.left + 36, middle + 10},
                             s.stage_live ? ui_icon_role_color(theme, UiIconRole::success) : theme.text_secondary);
    ok = ok && paint_stream_push(paint, s.header.left, s.header.bottom - 1, s.header.right, s.header.bottom,
                                 theme.tokens.border.subtle);
    // Letterbox behind pictures: the opaque media scrim.
    Color black = theme.tokens.overlay.media;
    black.alpha = 1;
    for (u32 i = 0; ok && i < s.tile_count; ++i) {
        const auto& tile = s.tiles[i];
        const auto* p = participant_of(s, tile);
        if (!p || !ui_rect_has_area(visible_part(tile)))
            continue;
        const auto r = tile.display;
        ok = paint_stream_push_clip(paint, tile.clip.left, tile.clip.top, tile.clip.right, tile.clip.bottom);
        if (stream_tile(*p)) {
            // Streams sit on the media plate; a screen glyph shows until a
            // preview or picture covers it.
            ok = ok && paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, black, kTileRadius);
            const bool glyph = !video_ready(*p) && !stream_spinner(*p) && p->stream != UiCallStream::ended &&
                               !(p->stream == UiCallStream::live && p->paused);
            const float extent = ui_rect_height(r) >= 120 ? 40.f : 24.f;
            Color ink = kDefaultTheme.text_secondary;
            ink.alpha *= .6f;
            if (ok && glyph && p->stream != UiCallStream::available)
                ok = ui_icon_paint(paint, UiIcon::screen_share,
                                   centered((r.left + r.right) * .5f, (r.top + r.bottom) * .5f, extent, extent), ink);
            else if (ok && glyph) {
                // Above the watch button, so both read without a preview.
                const auto watch = tile_controls(tile, *p, false).watch;
                const float bottom = ui_rect_has_area(watch) ? watch.top - 8 : (r.top + r.bottom + extent) * .5f;
                if (bottom - extent >= r.top + kPillInset + kLiveHeight)
                    ok = ui_icon_paint(paint, UiIcon::screen_share,
                                       {(r.left + r.right - extent) * .5f, bottom - extent,
                                        (r.left + r.right + extent) * .5f, bottom},
                                       ink);
            }
        } else if (video_ready(*p))
            ok = ok && paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, black, kTileRadius);
        else {
            // Avatar tiles: the card plate tinted with the participant's identity
            // colour. The host draws a custom avatar over the placeholder.
            const Color fill = theme.surface;
            ok = ok && ui_surface_paint(paint, r, theme, UiElevation::card, kTileRadius, &fill,
                                        &theme.tokens.border.subtle) &&
                 paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom,
                                           ui_call_tile_tint(theme, p->user_id), kTileRadius);
            const auto a = avatar_rect(tile);
            ok = ok && paint_stream_push_rounded(paint, a.left, a.top, a.right, a.bottom,
                                                 p->has_avatar ? theme.avatar_placeholder
                                                               : ui_call_identity_color(theme, p->user_id),
                                                 ui_rect_width(a) * .5f);
        }
        ok = paint_stream_pop_clip(paint) && ok;
    }
    ok = ok && call_stage::paint(s, theme, paint);
    return paint_stream_pop_clip(paint) && ok;
}

bool ui_call_scene_paint_foreground(const UiCallScene& s, const Theme& theme, PaintStream* paint) noexcept {
    if (!s.active)
        return true;
    if (!paint)
        return false;
    const auto v = s.viewport;
    if (!paint_stream_push_clip(paint, v.left, v.top, v.right, v.bottom))
        return false;
    bool ok = true;
    const Color success = ui_icon_role_color(theme, UiIconRole::success);
    const Color danger = ui_icon_role_color(theme, UiIconRole::danger);
    const Color light = kDefaultTheme.text_primary; // Pills and badges sit on a dark scrim.
    for (u32 i = 0; ok && i < s.tile_count; ++i) {
        const auto& tile = s.tiles[i];
        const auto* p = participant_of(s, tile);
        if (!p || !ui_rect_has_area(visible_part(tile)))
            continue;
        const auto r = tile.display;
        ok = paint_stream_push_clip(paint, tile.clip.left, tile.clip.top, tile.clip.right, tile.clip.bottom);
        const bool hovered = s.hovered_user == tile.user_id;
        const auto controls = tile_controls(s, tile, *p, hovered);
        if (ok && stream_scrim(*p) && p->stream != UiCallStream::ended)
            ok = paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, theme.tokens.overlay.scrim_light,
                                           kTileRadius);
        if (ok && stream_spinner(*p))
            ok = ui_spinner_paint(paint, state_glyph(tile, ui_rect_has_area(status_line(tile, true)) &&
                                                               stream_status(*p).bytes),
                                  light, s.spinner_phase);
        UiIcon glyph{};
        if (ok && status_glyph(*p, &glyph)) {
            Color ink = light;
            if (p->stream == UiCallStream::ended)
                ink.alpha *= .6f;
            ok = ui_icon_paint(paint, glyph, state_glyph(tile, ui_rect_has_area(status_line(tile, true))), ink);
        }
        if (ok && p->speaking)
            ok = paint_stream_push_rounded_border(paint, r.left, r.top, r.right, r.bottom, {0, 0, 0, 0}, success,
                                                  kTileRadius, 2);
        const auto pill = name_pill(tile, *p, controls);
        if (ok && ui_rect_has_area(pill)) {
            ok = paint_stream_push_rounded(paint, pill.left, pill.top, pill.right, pill.bottom,
                                           theme.tokens.overlay.scrim, theme.scale.radius_md);
            float x = pill.right - 6;
            const float y = (pill.top + pill.bottom) * .5f;
            if (ok && stream_tile(*p))
                ok = ui_icon_paint(paint, UiIcon::screen_share, {x - kIndicator, y - 8, x, y + 8}, light);
            if (ok && !stream_tile(*p) && p->deafened) {
                ok = ui_icon_paint(paint, UiIcon::headphones_off, {x - kIndicator, y - 8, x, y + 8}, danger);
                x -= kIndicator + 4;
            }
            if (ok && !stream_tile(*p) && p->muted)
                ok = ui_icon_paint(paint, UiIcon::mic_off, {x - kIndicator, y - 8, x, y + 8}, danger);
        }
        const auto live = live_badge(tile, *p);
        if (ok && ui_rect_has_area(live))
            ok = paint_stream_push_rounded(paint, live.left, live.top, live.right, live.bottom,
                                           theme.tokens.accent.danger, theme.scale.radius_sm);
        const auto effect = effect_pill(tile, *p);
        if (ok && ui_rect_has_area(effect))
            ok = paint_stream_push_rounded(paint, effect.left, effect.top, effect.right, effect.bottom,
                                           theme.tokens.overlay.scrim_strong, theme.scale.radius_md);
        const auto viewers = viewer_pill(tile, *p);
        if (ok && ui_rect_has_area(viewers)) {
            const float y = (viewers.top + viewers.bottom) * .5f;
            ok = paint_stream_push_rounded(paint, viewers.left, viewers.top, viewers.right, viewers.bottom,
                                           theme.tokens.overlay.scrim, theme.scale.radius_sm) &&
                 ui_icon_paint(paint, UiIcon::eye, {viewers.left + 6, y - 7, viewers.left + 20, y + 7}, light);
        }
        const auto round_button = [&](UiRect button, UiCallControl control, UiIcon icon) noexcept {
            if (!ui_rect_has_area(button))
                return true;
            const bool hot = hovered && s.hovered == control;
            return paint_stream_push_rounded(paint, button.left, button.top, button.right, button.bottom,
                                             hot ? theme.tokens.overlay.scrim_strong : theme.tokens.overlay.scrim,
                                             ui_rect_height(button) * .5f) &&
                   ui_icon_paint(paint, icon, ui_rect_inset(button, {7, 7, 7, 7}), light);
        };
        // Filled pills: "Watch stream" (brand) and "Stop watching" (danger).
        const auto filled_button = [&](UiRect button, UiCallControl control, Color fill) noexcept {
            if (!ui_rect_has_area(button))
                return true;
            const bool hot = hovered && s.hovered == control;
            const float shade = hot && s.pressed == control ? .8f : hot ? .9f : 1.f;
            fill.red *= shade;
            fill.green *= shade;
            fill.blue *= shade;
            return paint_stream_push_rounded(paint, button.left, button.top, button.right, button.bottom, fill,
                                             theme.scale.radius_sm);
        };
        if (ok)
            ok = round_button(controls.fullscreen, UiCallControl::tile_fullscreen,
                       s.fullscreen && tile.focused ? UiIcon::minimize : UiIcon::maximize) &&
                 round_button(controls.volume, UiCallControl::stream_volume, UiIcon::speaker) &&
                 round_button(controls.dismiss, UiCallControl::stream_dismiss, UiIcon::close) &&
                 round_button(controls.move, UiCallControl::stage_move, UiIcon::user_minus) &&
                 filled_button(controls.watch, UiCallControl::stream_watch, theme.tokens.accent.brand);
        if (ok && ui_rect_has_area(controls.stop)) {
            ok = filled_button(controls.stop, UiCallControl::stream_stop, theme.tokens.accent.danger);
            const auto c = controls.stop;
            const float y = (c.top + c.bottom) * .5f;
            const UiRect icon = controls.stop_compact ? ui_rect_inset(c, {7, 7, 7, 7})
                                                      : UiRect{c.left + 10, y - 8, c.left + 26, y + 8};
            ok = ok && ui_icon_paint(paint, UiIcon::close, icon, theme.tokens.text.on_accent);
        }
        ok = paint_stream_pop_clip(paint) && ok;
    }
    // Control bar: round buttons consistent with the voice card's controls.
    for (u32 i = 1; ok && i < kUiCallControlCount; ++i) {
        const auto control = static_cast<UiCallControl>(i);
        const auto r = s.controls[i];
        if (!ui_rect_has_area(r) || control == UiCallControl::tile_fullscreen || stage_chrome(control))
            continue;
        const bool enabled = ui_call_control_enabled(s, control);
        const bool active = ui_call_control_active(s, control);
        const bool hovered = enabled && s.hovered == control;
        const bool pressed = hovered && s.pressed == control;
        const float radius = theme.scale.radius_md;
        UiIcon icon = UiIcon::close;
        Color ink = theme.text_primary;
        switch (control) {
        case UiCallControl::mute:
            icon = active ? UiIcon::mic_off : UiIcon::mic;
            break;
        case UiCallControl::deafen:
            icon = active ? UiIcon::headphones_off : UiIcon::headphones;
            break;
        case UiCallControl::camera:
            icon = UiIcon::video_off;
            break;
        case UiCallControl::screen_share:
            icon = UiIcon::screen_share;
            break;
        case UiCallControl::disconnect:
            icon = UiIcon::phone_off;
            ink = theme.tokens.text.on_accent;
            break;
        case UiCallControl::fullscreen:
            icon = s.fullscreen ? UiIcon::minimize : UiIcon::maximize;
            break;
        case UiCallControl::chat:
            icon = UiIcon::message_text;
            break;
        case UiCallControl::soundboard:
            icon = UiIcon::audio_lines;
            break;
        case UiCallControl::stage_hand:
        case UiCallControl::stage_become:
        case UiCallControl::stage_audience:
            icon = call_stage::bar_icon(s, control);
            break;
        default:
            break;
        }
        if (control == UiCallControl::disconnect) {
            Color fill = theme.tokens.accent.danger;
            const float shade = pressed ? .8f : hovered ? .9f : 1.f;
            fill.red *= shade;
            fill.green *= shade;
            fill.blue *= shade;
            ok = paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, fill, radius);
        } else if (control == UiCallControl::close || control == UiCallControl::fullscreen ||
                   control == UiCallControl::chat) {
            if (hovered || active)
                ok = paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom,
                                               pressed ? theme.tokens.state.pressed : theme.tokens.state.hover,
                                               theme.scale.radius_sm);
            ink = hovered || active ? theme.text_primary : theme.text_secondary;
        } else {
            // Idle: the subtle plate; hover/press add the state overlay.
            // Active (muted, deafened): a danger tint with the red glyph; a
            // raised hand the brand tint. Disabled: faded, no hover. Tints are
            // overlays so gradient themes keep their plate material.
            Color fill = theme.surface_high, tint{};
            if (active) {
                const bool brand = control == UiCallControl::stage_hand || control == UiCallControl::soundboard;
                tint = brand ? theme.tokens.accent.brand : danger;
                tint.alpha = pressed ? .38f : hovered ? .30f : .22f;
                ink = brand ? ui_icon_role_color(theme, UiIconRole::accent) : danger;
            } else if (hovered)
                tint = pressed ? theme.tokens.state.pressed : theme.tokens.state.hover;
            if (!enabled) {
                fill.alpha *= .5f;
                ink = theme.text_secondary;
                ink.alpha *= .45f;
            }
            ok = ui_surface_paint(paint, r, theme, UiElevation::banner, radius, &fill, &theme.tokens.border.subtle);
            if (ok && tint.alpha > 0)
                ok = paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, tint, radius);
        }
        const float extent = control == UiCallControl::close || control == UiCallControl::fullscreen ||
                                     control == UiCallControl::chat
                                 ? 20.f
                                 : 24.f;
        const float dy = pressed ? 1.f : 0.f;
        ok = ok && ui_icon_paint(paint, icon,
                                 centered((r.left + r.right) * .5f, (r.top + r.bottom) * .5f + dy, extent, extent),
                                 ink);
    }
    ok = ok && call_stage::paint_foreground(s, theme, paint);
    return paint_stream_pop_clip(paint) && ok;
}

bool ui_call_scene_text_output(const UiCallScene& s, const Theme& theme, UiTextOutput* text) noexcept {
    if (!s.active)
        return true;
    if (!text)
        return false;
    const auto close = s.controls[static_cast<u32>(UiCallControl::close)];
    const auto chat = s.controls[static_cast<u32>(UiCallControl::chat)];
    float title_right = ui_rect_has_area(chat)             ? chat.left - 12
                        : ui_rect_has_area(s.search_field) ? s.search_field.left - 12
                        : ui_rect_has_area(close)          ? close.left - 12
                                                           : s.header.right - 16;
    title_right = minimum(title_right, call_stage::header_left(s) - 12);
    const UiRect status{title_right - 160, s.header.top, title_right, s.header.bottom};
    const Utf8View title = s.stage_topic_bytes ? Utf8View{s.stage_topic, s.stage_topic_bytes}
                                               : Utf8View{s.title, s.title_bytes};
    const Utf8View status_text = s.stage_notice_bytes ? Utf8View{s.stage_notice, s.stage_notice_bytes}
                                 : !s.connected        ? utf8_literal("Connecting\xE2\x80\xA6")
                                                       : Utf8View{};
    bool ok = push_text(text, s, title,
                        {s.header.left + 44, s.header.top, status_text.bytes ? status.left - 8 : title_right,
                         s.header.bottom},
                        s.header, theme.tokens.text.normal, text_style_bold);
    if (ok && status_text.bytes)
        ok = push_text(text, s, status_text, status, s.header, theme.tokens.text.muted, text_style_subtext,
                       UiTextAlignment::end);
    ok = ok && call_stage::text(s, theme, text);
    const Color light = kDefaultTheme.text_primary;
    for (u32 i = 0; ok && i < s.tile_count; ++i) {
        const auto& tile = s.tiles[i];
        const auto* p = participant_of(s, tile);
        if (!p)
            continue;
        const UiRect clip = visible_part(tile);
        const auto controls = tile_controls(s, tile, *p, s.hovered_user == tile.user_id);
        const auto pill = name_pill(tile, *p, controls);
        if (ui_rect_has_area(pill)) {
            const float icons = stream_tile(*p) ? kIndicator + 4
                                                : static_cast<float>(indicator_count(*p)) * (kIndicator + 4);
            const UiRect label{pill.left + 10, pill.top, pill.right - 6 - icons, pill.bottom};
            const Utf8View name = p->name.bytes ? p->name : utf8_literal("Unknown user");
            ok = push_text(text, s, name, label, ui_rect_intersection(clip, pill), light, text_style_subtext);
        }
        if (ok && !stream_tile(*p) && !video_ready(*p) && !p->has_avatar) {
            char storage[16];
            const auto initials =
                ui_admin_initials(p->name.bytes ? p->name : utf8_literal("?"), {storage, sizeof(storage)});
            const auto a = avatar_rect(tile);
            const u32 size = ui_rect_width(a) >= 80   ? text_style_heading_1
                             : ui_rect_width(a) >= 48 ? text_style_heading_2
                                                      : text_style_none;
            if (initials.bytes && ui_rect_has_area(ui_rect_intersection(a, clip)))
                ok = ui_text_output_push_owned(text, {initials, s.owner, a, ui_rect_intersection(clip, a),
                                                      theme.tokens.text.on_accent, size | text_style_bold,
                                                      UiTextAlignment::center, UiTextAlignment::center,
                                                      UiTextWrapping::no_wrap, UiTextOverflow::clip});
        }
        const auto live = live_badge(tile, *p);
        if (ok && ui_rect_has_area(live))
            ok = push_text(text, s, utf8_literal("LIVE"), live, ui_rect_intersection(clip, live),
                           theme.tokens.text.on_accent, text_style_subtext | text_style_bold,
                           UiTextAlignment::center);
        const auto effect = effect_pill(tile, *p);
        if (ok && ui_rect_has_area(effect))
            ok = push_text(text, s, p->effect, {effect.left + 8, effect.top, effect.right - 8, effect.bottom},
                           ui_rect_intersection(clip, effect), light, text_style_subtext | text_style_bold,
                           UiTextAlignment::center);
        const auto viewers = viewer_pill(tile, *p);
        if (ok && ui_rect_has_area(viewers)) {
            const UiRect count{viewers.left + 24, viewers.top, viewers.right - 4, viewers.bottom};
            const char* digits = s.viewer_text[tile.participant];
            u32 bytes = 0;
            while (bytes < sizeof(s.viewer_text[0]) && digits[bytes])
                ++bytes;
            ok = push_text(text, s, {digits, bytes}, count, ui_rect_intersection(clip, viewers), light,
                           text_style_subtext | text_style_bold);
        }
        if (ok && ui_rect_has_area(controls.watch))
            ok = push_text(text, s, utf8_literal("Watch stream"), controls.watch,
                           ui_rect_intersection(clip, controls.watch), theme.tokens.text.on_accent,
                           text_style_subtext | text_style_bold, UiTextAlignment::center);
        if (ok && ui_rect_has_area(controls.stop) && !controls.stop_compact) {
            const auto c = controls.stop;
            ok = push_text(text, s, utf8_literal("Stop watching"), {c.left + 30, c.top, c.right - 8, c.bottom},
                           ui_rect_intersection(clip, c), theme.tokens.text.on_accent,
                           text_style_subtext | text_style_bold);
        }
        const Utf8View state_text = stream_status(*p);
        const auto line = state_text.bytes ? status_line(tile, true) : UiRect{};
        if (ok && ui_rect_has_area(line))
            ok = push_text(text, s, state_text, line, ui_rect_intersection(clip, line), light, text_style_subtext,
                           UiTextAlignment::center);
    }
    return ok;
}
} // namespace maicord
