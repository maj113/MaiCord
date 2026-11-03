#include "maicord/ui/voice/stream_card_scene.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/ui/text/style.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/media/core/asset.hpp"

namespace maicord {
namespace {
constexpr float kThumbnailHeight = (kUiStreamCardWidth - 2 * kUiStreamCardPadding) * 9.f / 16.f;
constexpr float kNameHeight = 20, kChannelHeight = 18, kButtonHeight = 36, kLiveWidth = 40, kLiveHeight = 20,
                kSlide = 8;
constexpr float kCardHeight = kUiStreamCardPadding + kThumbnailHeight + 10 + kNameHeight + 2 + kChannelHeight + 12 +
                              kButtonHeight + kUiStreamCardPadding;
constexpr UiHoverCardMotion kEntrance{{kUiStreamCardEntranceMs, UiMotionCurve::spring}, 0x5743'a8d0'0000'0001ull};
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
UiRect name_line(const UiStreamCardScene& s) noexcept {
    return {s.live.right + 8, s.live.top, s.thumbnail.right, s.live.top + kNameHeight};
}
UiRect channel_line(const UiStreamCardScene& s) noexcept {
    const auto name = name_line(s);
    return {s.thumbnail.left, name.bottom + 2, name.right, name.bottom + 2 + kChannelHeight};
}
} // namespace

bool ui_stream_card_show(UiStreamCardScene* s, u64 identity, const UiStreamCardContent& content, UiRect anchor,
                         UiRect viewport, u64 now_ms, bool reduced_motion) noexcept {
    if (!s || !identity || !ui_rect_valid(viewport) || !ui_rect_has_area(anchor) ||
        !ui_hover_card_initialize(*s, utf8_literal("Stream preview"))) {
        if (s && s->active)
            ui_stream_card_close(s);
        return false;
    }
    const bool fresh = !s->active || s->identity != identity;
    s->identity = identity;
    s->anchor = anchor;
    s->viewport = viewport;
    s->name_bytes = utf8_copy_truncated(s->name, kUiStreamCardTextBytes, content.name);
    s->channel_bytes = utf8_copy_truncated(s->channel, kUiStreamCardTextBytes, content.channel);
    s->preview = content.preview;
    s->preview_state = content.preview_state;
    s->watching = content.watching;
    s->settled = place(anchor, viewport);
    if (fresh)
        ui_hover_card_restart(s, kEntrance, now_ms, reduced_motion);
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
    ui_hover_card_publish(s, hash);
    return true;
}

void ui_stream_card_close(UiStreamCardScene* s) noexcept {
    ui_hover_card_close(s);
}

bool ui_stream_card_tick(UiStreamCardScene* s, u64 now_ms, bool reduced_motion) noexcept {
    if (!ui_hover_card_advance(s, kEntrance, now_ms, reduced_motion))
        return false;
    layout(*s);
    ++s->revision;
    return true;
}

u64 ui_stream_card_next_wake(const UiStreamCardScene& s, bool reduced_motion) noexcept {
    return ui_hover_card_next_wake(s, kEntrance, reduced_motion);
}

bool ui_stream_card_contains(const UiStreamCardScene& s, float x, float y) noexcept {
    return ui_hover_card_contains(s, x, y);
}

UiStreamCardControl ui_stream_card_hit(const UiStreamCardScene& s, float x, float y) noexcept {
    return s.active && ui_rect_contains(s.button, x, y) ? UiStreamCardControl::watch : UiStreamCardControl::none;
}

bool ui_stream_card_set_hover(UiStreamCardScene* s, float x, float y) noexcept {
    return s && ui_hover_card_set_hovered(s, ui_stream_card_hit(*s, x, y) == UiStreamCardControl::watch);
}

bool ui_stream_card_set_pressed(UiStreamCardScene* s, bool pressed) noexcept {
    return ui_hover_card_set_pressed(s, pressed);
}

bool ui_stream_card_spinner_visible(const UiStreamCardScene& s) noexcept {
    return s.active && s.preview_state == UiStreamCardPreview::loading;
}

bool ui_stream_card_media(const UiStreamCardScene& s, const Theme& theme, UiStreamCardMedia* output) noexcept {
    if (!output || !s.active || s.preview_state != UiStreamCardPreview::ready || !media_asset_key_valid(s.preview))
        return false;
    *output = {s.preview, s.thumbnail, s.thumbnail, theme.scale.radius_sm, clamp_unit(s.progress)};
    return true;
}

bool ui_stream_card_paint(const UiStreamCardScene& s, const Theme& theme, PaintStream* paint) noexcept {
    if (!s.active)
        return true;
    UiPaint p{paint, theme};
    const float a = clamp_unit(s.progress);
    p.use<ui_hover_card_paint_surface>(s, theme, theme.scale.radius_md);
    // Thumbnail plate (media scrim): the preview covers it once resident.
    Color plate = theme.tokens.overlay.media;
    plate.alpha = a;
    const auto t = s.thumbnail;
    p.rounded(t, plate, theme.scale.radius_sm);
    if (s.preview_state == UiStreamCardPreview::unavailable) {
        Color ink = kDefaultTheme.text_secondary;
        ink.alpha *= .6f * a;
        const float cx = (t.left + t.right) * .5f, cy = (t.top + t.bottom) * .5f - 10;
        p.icon(UiIcon::screen_share, {cx - 16, cy - 16, cx + 16, cy + 16}, ink);
    }
    if (s.preview_state == UiStreamCardPreview::loading) {
        const float cx = (s.thumbnail.left + s.thumbnail.right) * .5f;
        const float cy = (s.thumbnail.top + s.thumbnail.bottom) * .5f;
        p.spinner({cx - 14, cy - 14, cx + 14, cy + 14}, ui_theme_color_alpha(kDefaultTheme.text_primary, a),
                  s.spinner_phase);
    }
    p.rounded(s.live, ui_theme_color_alpha(theme.tokens.accent.danger, a), theme.scale.radius_sm);
    const auto b = s.button;
    p.rounded(b, ui_theme_color_alpha(ui_hover_card_button_color(s, theme), a), theme.scale.radius_sm);
    const float y = (b.top + b.bottom) * .5f;
    const float label = s.watching ? 96.f : 104.f; // Estimated label width keeps the glyph beside it.
    const float x = (b.left + b.right - label) * .5f - 12;
    return p.icon(s.watching ? UiIcon::maximize : UiIcon::screen_share, {x - 18, y - 9, x, y + 9},
                  ui_theme_color_alpha(theme.tokens.text.on_accent, a))
        .end();
}

bool ui_stream_card_text_output(const UiStreamCardScene& s, const Theme& theme, UiTextOutput* out) noexcept {
    if (!s.active)
        return true;
    UiTextPen<UiTextClip::region> text{out, s.widgets, s.panel};
    const auto owner = s.owner.handle;
    const float a = clamp_unit(s.progress);
    text(owner, utf8_literal("LIVE"), s.live, ui_theme_color_alpha(theme.tokens.text.on_accent, a),
         text_style_subtext | text_style_bold, ui_text_center);
    if (s.preview_state == UiStreamCardPreview::unavailable) {
        const auto t = s.thumbnail;
        const float cy = (t.top + t.bottom) * .5f + 18;
        Color ink = kDefaultTheme.text_secondary;
        ink.alpha *= a;
        text(owner, utf8_literal("No preview yet"), {t.left, cy - 10, t.right, cy + 10}, ink, text_style_subtext,
             ui_text_center);
    }
    text(owner, {s.name, s.name_bytes}, name_line(s), ui_theme_color_alpha(theme.tokens.text.normal, a),
         text_style_bold);
    if (s.channel_bytes && text.ok()) {
        // "Live in <channel>"; the prefix and name share one line.
        char line[kUiStreamCardTextBytes + 8];
        static constexpr char prefix[] = "Live in ";
        u32 bytes = sizeof(prefix) - 1;
        copy_bytes(line, prefix, bytes);
        copy_bytes(line + bytes, s.channel, s.channel_bytes);
        bytes += s.channel_bytes;
        if (!ui_text_output_push_owned(out, {{line, bytes}, s.owner, channel_line(s), s.panel,
                                             ui_theme_color_alpha(theme.tokens.text.muted, a), text_style_subtext,
                                             UiTextAlignment::start, UiTextAlignment::center,
                                             TextWrapping::no_wrap, TextOverflow::ellipsis}))
            text.fail();
    }
    const auto b = s.button;
    return text(owner, s.watching ? utf8_literal("Open stream") : utf8_literal("Watch stream"),
                {b.left + 12, b.top, b.right - 12, b.bottom}, ui_theme_color_alpha(theme.tokens.text.on_accent, a),
                text_style_bold, ui_text_center)
        .ok();
}
} // namespace maicord
