#include "maicord/ui/stream_card_scene.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/spinner.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/text_style.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr float kThumbnailHeight = (kUiStreamCardWidth - 2 * kUiStreamCardPadding) * 9.f / 16.f;
constexpr float kNameHeight = 20, kChannelHeight = 18, kButtonHeight = 36, kLiveWidth = 40, kLiveHeight = 20,
                kSlide = 8;
constexpr float kCardHeight = kUiStreamCardPadding + kThumbnailHeight + 10 + kNameHeight + 2 + kChannelHeight + 12 +
                              kButtonHeight + kUiStreamCardPadding;
constexpr UiMotionStyle kEntranceStyle{kUiStreamCardEntranceMs, UiMotionCurve::spring};
constexpr u64 kEntranceOwner = 0x5743'a8d0'0000'0001ull;

UiMotionScope scope(const UiStreamCardScene& s) noexcept {
    return {kEntranceOwner, s.entrance_generation};
}
u32 copy_text(char* destination, u32 capacity, Utf8View text) noexcept {
    if (!text.data)
        return 0;
    u32 bytes = minimum(text.bytes, capacity);
    while (bytes && bytes < text.bytes && (static_cast<u8>(text.data[bytes]) & 0xc0) == 0x80)
        --bytes;
    if (bytes)
        copy_bytes(destination, text.data, bytes);
    return bytes;
}
bool initialize(UiStreamCardScene& s) noexcept {
    if (s.owner.manager == &s.widgets)
        return true;
    if (!ui_widget_manager_initialize(&s.widgets, {s.widget_storage, 1}, {s.layout_storage, 1}))
        return false;
    auto style = ui_layout_style();
    style.flow = UiLayoutFlow::overlay;
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.flags = ui_layout_node_visible | ui_layout_node_allow_overlap;
    const auto root = ui_widget_manager_add(
        &s.widgets, ui_widget_invalid_handle(),
        {1, UiWidgetKind::container, ui_widget_none, {utf8_literal("Stream preview"), style, {}, 0, 1}});
    if (!root.ok())
        return false;
    s.owner = {&s.widgets, root.handle};
    return true;
}
UiRect offset(UiRect r, float dx) noexcept {
    return {r.left + dx, r.top, r.right + dx, r.bottom};
}
// Settled card beside the anchor row: right of it when it fits, else left;
// vertically centered on the row and kept inside the viewport.
UiRect place(UiRect anchor, UiRect viewport) noexcept {
    float left = anchor.right + kUiStreamCardGap;
    if (left + kUiStreamCardWidth > viewport.right - kUiStreamCardGap)
        left = anchor.left - kUiStreamCardGap - kUiStreamCardWidth;
    left = maximum(viewport.left + kUiStreamCardGap, left);
    float top = (anchor.top + anchor.bottom) * .5f - 48;
    top = minimum(top, viewport.bottom - kUiStreamCardGap - kCardHeight);
    top = maximum(viewport.top + kUiStreamCardGap, top);
    return {left, top, left + kUiStreamCardWidth, top + kCardHeight};
}
void layout(UiStreamCardScene& s) noexcept {
    // The entrance slides in from the row's side.
    const float side = s.settled.left >= s.anchor.right ? -1.f : 1.f;
    s.panel = offset(s.settled, side * kSlide * (1 - s.progress));
    const auto p = s.panel;
    s.thumbnail = {p.left + kUiStreamCardPadding, p.top + kUiStreamCardPadding, p.right - kUiStreamCardPadding,
                   p.top + kUiStreamCardPadding + kThumbnailHeight};
    // LIVE leads the name line (paint stays below the thumbnail's media layer).
    const float line = s.thumbnail.bottom + 10;
    s.live = {s.thumbnail.left, line, s.thumbnail.left + kLiveWidth, line + kLiveHeight};
    s.button = {p.left + kUiStreamCardPadding, p.bottom - kUiStreamCardPadding - kButtonHeight,
                p.right - kUiStreamCardPadding, p.bottom - kUiStreamCardPadding};
}
Color fade(Color color, float opacity) noexcept {
    color.alpha *= opacity;
    return color;
}
float opacity(const UiStreamCardScene& s) noexcept {
    return s.progress < 0 ? 0.f : s.progress > 1 ? 1.f : s.progress;
}
UiRect name_line(const UiStreamCardScene& s) noexcept {
    return {s.live.right + 8, s.live.top, s.thumbnail.right, s.live.top + kNameHeight};
}
UiRect channel_line(const UiStreamCardScene& s) noexcept {
    const auto name = name_line(s);
    return {s.thumbnail.left, name.bottom + 2, name.right, name.bottom + 2 + kChannelHeight};
}
bool push_text(UiTextOutput* text, const UiStreamCardScene& s, Utf8View value, UiRect rectangle, Color color,
               u32 style, UiTextAlignment horizontal = UiTextAlignment::start) noexcept {
    if (!value.bytes)
        return true;
    return ui_text_output_push(text, {value, s.owner, rectangle, s.panel, color, style, horizontal,
                                      UiTextAlignment::center, UiTextWrapping::no_wrap, UiTextOverflow::ellipsis});
}
} // namespace

bool ui_stream_card_show(UiStreamCardScene* s, u64 identity, const UiStreamCardContent& content, UiRect anchor,
                         UiRect viewport, u64 now_ms, bool reduced_motion) noexcept {
    if (!s || !identity || !ui_rect_valid(viewport) || !ui_rect_has_area(anchor) || !initialize(*s)) {
        if (s && s->active)
            ui_stream_card_close(s);
        return false;
    }
    const bool fresh = !s->active || s->identity != identity;
    s->identity = identity;
    s->anchor = anchor;
    s->viewport = viewport;
    s->name_bytes = copy_text(s->name, kUiStreamCardTextBytes, content.name);
    s->channel_bytes = copy_text(s->channel, kUiStreamCardTextBytes, content.channel);
    s->preview = content.preview;
    s->preview_state = content.preview_state;
    s->watching = content.watching;
    s->settled = place(anchor, viewport);
    if (fresh) {
        s->hovered = s->pressed = false;
        ++s->entrance_generation;
        ui_motion_transition_reset(&s->entrance, scope(*s), reduced_motion ? 1.f : 0.f);
        ui_motion_transition_tick(&s->entrance, scope(*s), 1, now_ms, kEntranceStyle, reduced_motion, true);
    }
    s->progress = s->entrance.value;
    layout(*s);
    if (!ui_widget_manager_layout_rectangle(&s->widgets, viewport).ok()) {
        ui_stream_card_close(s);
        return false;
    }
    u64 hash = hash64_mix(kHash64Seed, identity);
    hash = hash64_counted_bytes(hash, s->name, s->name_bytes);
    hash = hash64_counted_bytes(hash, s->channel, s->channel_bytes);
    hash = hash64_bytes(hash, &s->preview, sizeof(s->preview));
    hash = hash64_bytes(hash, &s->panel, sizeof(s->panel));
    hash = hash64_mix(hash, (u64{static_cast<u8>(s->preview_state)} << 8) | (s->watching ? 1u : 0u));
    if (!s->active || hash != s->content_hash) {
        s->content_hash = hash;
        ++s->revision;
    }
    s->active = true;
    return true;
}

void ui_stream_card_close(UiStreamCardScene* s) noexcept {
    if (!s || !s->active)
        return;
    s->active = s->hovered = s->pressed = false;
    s->identity = 0;
    ++s->entrance_generation;
    ++s->revision;
}

bool ui_stream_card_tick(UiStreamCardScene* s, u64 now_ms, bool reduced_motion) noexcept {
    if (!s || !s->active || (!s->entrance.active && s->entrance.value >= 1))
        return false;
    if (!ui_motion_transition_tick(&s->entrance, scope(*s), 1, now_ms, kEntranceStyle, reduced_motion, true))
        return false;
    s->progress = s->entrance.value;
    layout(*s);
    ++s->revision;
    return true;
}

u64 ui_stream_card_next_wake(const UiStreamCardScene& s, u64 now_ms, bool reduced_motion) noexcept {
    return s.active ? ui_motion_transition_next_wake(s.entrance, scope(s), now_ms, reduced_motion, true) : 0;
}

bool ui_stream_card_contains(const UiStreamCardScene& s, float x, float y) noexcept {
    // The settled card, so a pointer moving onto it during the slide counts.
    return s.active && (ui_rect_contains(s.settled, x, y) || ui_rect_contains(s.panel, x, y));
}

UiStreamCardControl ui_stream_card_hit(const UiStreamCardScene& s, float x, float y) noexcept {
    return s.active && ui_rect_contains(s.button, x, y) ? UiStreamCardControl::watch : UiStreamCardControl::none;
}

bool ui_stream_card_set_hover(UiStreamCardScene* s, float x, float y) noexcept {
    if (!s)
        return false;
    const bool hovered = ui_stream_card_hit(*s, x, y) == UiStreamCardControl::watch;
    if (hovered == s->hovered)
        return false;
    s->hovered = hovered;
    ++s->revision;
    return true;
}

bool ui_stream_card_set_pressed(UiStreamCardScene* s, bool pressed) noexcept {
    if (!s || s->pressed == pressed)
        return false;
    s->pressed = pressed;
    ++s->revision;
    return true;
}

bool ui_stream_card_spinner_visible(const UiStreamCardScene& s) noexcept {
    return s.active && s.preview_state == UiStreamCardPreview::loading;
}

bool ui_stream_card_media(const UiStreamCardScene& s, const Theme& theme, UiStreamCardMedia* output) noexcept {
    if (!output || !s.active || s.preview_state != UiStreamCardPreview::ready || !media_asset_key_valid(s.preview))
        return false;
    *output = {s.preview, s.thumbnail, s.thumbnail, theme.scale.radius_sm, opacity(s)};
    return true;
}

bool ui_stream_card_paint(const UiStreamCardScene& s, const Theme& theme, PaintStream* paint) noexcept {
    if (!s.active)
        return true;
    if (!paint)
        return false;
    const float a = opacity(s);
    const Color fill = fade(theme.tokens.background.floating, a);
    const Color border = fade(theme.tokens.border.subtle, a);
    bool ok = ui_surface_paint(paint, s.panel, theme, UiElevation::popover, theme.scale.radius_md, &fill, &border);
    // Thumbnail plate (media scrim): the preview covers it once resident.
    Color plate = theme.tokens.overlay.media;
    plate.alpha = a;
    const auto t = s.thumbnail;
    ok = ok && paint_stream_push_rounded(paint, t.left, t.top, t.right, t.bottom, plate, theme.scale.radius_sm);
    if (ok && s.preview_state == UiStreamCardPreview::unavailable) {
        Color ink = kDefaultTheme.text_secondary;
        ink.alpha *= .6f * a;
        const float cx = (t.left + t.right) * .5f, cy = (t.top + t.bottom) * .5f - 10;
        ok = ui_icon_paint(paint, UiIcon::screen_share, {cx - 16, cy - 16, cx + 16, cy + 16}, ink);
    }
    if (ok && s.preview_state == UiStreamCardPreview::loading) {
        const float cx = (s.thumbnail.left + s.thumbnail.right) * .5f;
        const float cy = (s.thumbnail.top + s.thumbnail.bottom) * .5f;
        ok = ui_spinner_paint(paint, {cx - 14, cy - 14, cx + 14, cy + 14}, fade(kDefaultTheme.text_primary, a),
                              s.spinner_phase);
    }
    ok = ok && paint_stream_push_rounded(paint, s.live.left, s.live.top, s.live.right, s.live.bottom,
                                         fade(theme.tokens.accent.danger, a), theme.scale.radius_sm);
    Color button = theme.tokens.accent.brand;
    const float shade = s.pressed && s.hovered ? .8f : s.hovered ? .9f : 1.f;
    button.red *= shade;
    button.green *= shade;
    button.blue *= shade;
    const auto b = s.button;
    ok = ok && paint_stream_push_rounded(paint, b.left, b.top, b.right, b.bottom, fade(button, a),
                                         theme.scale.radius_sm);
    const float y = (b.top + b.bottom) * .5f;
    const float label = s.watching ? 96.f : 104.f; // Estimated label width keeps the glyph beside it.
    const float x = (b.left + b.right - label) * .5f - 12;
    return ok && ui_icon_paint(paint, s.watching ? UiIcon::maximize : UiIcon::screen_share,
                               {x - 18, y - 9, x, y + 9}, fade(theme.tokens.text.on_accent, a));
}

bool ui_stream_card_text_output(const UiStreamCardScene& s, const Theme& theme, UiTextOutput* text) noexcept {
    if (!s.active)
        return true;
    if (!text)
        return false;
    const float a = opacity(s);
    bool ok = push_text(text, s, utf8_literal("LIVE"), s.live, fade(theme.tokens.text.on_accent, a),
                        text_style_subtext | text_style_bold, UiTextAlignment::center);
    if (ok && s.preview_state == UiStreamCardPreview::unavailable) {
        const auto t = s.thumbnail;
        const float cy = (t.top + t.bottom) * .5f + 18;
        Color ink = kDefaultTheme.text_secondary;
        ink.alpha *= a;
        ok = push_text(text, s, utf8_literal("No preview yet"), {t.left, cy - 10, t.right, cy + 10}, ink,
                       text_style_subtext, UiTextAlignment::center);
    }
    ok = ok && push_text(text, s, {s.name, s.name_bytes}, name_line(s), fade(theme.tokens.text.normal, a),
                         text_style_bold);
    if (ok && s.channel_bytes) {
        // "Live in <channel>"; the prefix and name share one line.
        char line[kUiStreamCardTextBytes + 8];
        constexpr char prefix[] = "Live in ";
        u32 bytes = sizeof(prefix) - 1;
        copy_bytes(line, prefix, bytes);
        copy_bytes(line + bytes, s.channel, s.channel_bytes);
        bytes += s.channel_bytes;
        ok = ui_text_output_push_owned(text, {{line, bytes}, s.owner, channel_line(s), s.panel,
                                              fade(theme.tokens.text.muted, a), text_style_subtext,
                                              UiTextAlignment::start, UiTextAlignment::center,
                                              UiTextWrapping::no_wrap, UiTextOverflow::ellipsis});
    }
    const auto b = s.button;
    return ok && push_text(text, s, s.watching ? utf8_literal("Open stream") : utf8_literal("Watch stream"),
                           {b.left + 12, b.top, b.right - 12, b.bottom}, fade(theme.tokens.text.on_accent, a),
                           text_style_bold, UiTextAlignment::center);
}
} // namespace maicord
