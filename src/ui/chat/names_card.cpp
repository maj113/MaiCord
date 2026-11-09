#include "maicord/ui/chat/names_card.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/ui/text/measure_cache.hpp"
#include "maicord/ui/text/style.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/media/core/asset.hpp"

namespace maicord {
namespace {
constexpr UiHoverCardMotion kEntrance{{kUiNamesCardEntranceMs, UiMotionCurve::spring}, 0x4e41'4d45'0000'0001ull};
// "AKA" in the muted label style, and "+N" after the pictures.
constexpr float kLabelWidth = 28, kMoreWidth = 24, kAnchorOverlap = 12;
float text_width(UiTextMeasureCache* cache, const Utf8View& text, u32 flags, float limit) noexcept {
    if (!text.bytes)
        return 0.f;
    UiTextMeasureValue measured{};
    const TextStyleSpan span{0, text.bytes, flags};
    if (cache && ui_text_measure_cache_measure(
                     cache,
                     {{0x4e414d45, hash64_bytes(kHash64Seed, text.data, text.bytes), 0, static_cast<u16>(flags), 0},
                      text,
                      flags ? ConstSlice<TextStyleSpan>{&span, 1} : ConstSlice<TextStyleSpan>{},
                      {limit, 40.f},
                      true},
                     &measured)
                     .ok())
        return minimum(limit, measured.size.width + 2.f);
    return minimum(limit, static_cast<float>(text.bytes) * 8.f);
}
float pictures_width(u32 count, bool more) noexcept {
    return static_cast<float>(count) * (kUiNamesCardIcon + kUiNamesCardIconGap) - kUiNamesCardIconGap +
           (more ? kUiNamesCardIconGap + kMoreWidth : 0.f);
}
void layout(UiNamesCardScene& s, float username_width) noexcept {
    const float left = s.panel.left + kUiNamesCardPadding, right = s.panel.right - kUiNamesCardPadding;
    float y = s.panel.top + kUiNamesCardPadding;
    s.username = {left, y, minimum(right, left + username_width), y + kUiNamesCardLine};
    s.label = {minimum(right, s.username.right + kUiNamesCardGap), y, right, y + kUiNamesCardLine};
    y += kUiNamesCardLine + kUiNamesCardGap;
    s.divider = {left, y, right, y + 1.f};
    y += 1.f + kUiNamesCardGap;
    for (u32 row = 0; row < s.row_count; ++row, y += kUiNamesCardLine + kUiNamesCardRowGap) {
        const bool more = s.more_bytes[row] != 0;
        const float strip = pictures_width(s.icon_count[row], more);
        float x = right - strip;
        s.nicknames[row] = {left, y, maximum(left, x - kUiNamesCardNicknameGap), y + kUiNamesCardLine};
        for (u32 i = 0; i < s.icon_count[row]; ++i, x += kUiNamesCardIcon + kUiNamesCardIconGap)
            s.icons[row * kUiNamesCardIcons + i] = {x, y, x + kUiNamesCardIcon, y + kUiNamesCardLine};
        s.more[row] = more ? UiRect{right - kMoreWidth, y, right, y + kUiNamesCardLine} : UiRect{};
    }
}
} // namespace

bool ui_names_card_show(UiNamesCardScene* s, u64 identity, const UiNamesCardContent& content, UiRect anchor,
                        UiRect viewport, u64 now_ms, bool reduced_motion, UiTextMeasureCache* measure) noexcept {
    if (!s || !identity || !content.row_count || !ui_rect_valid(viewport) || !ui_rect_has_area(anchor) ||
        !ui_hover_card_initialize(*s, utf8_literal("Other names"))) {
        if (s && s->active)
            ui_names_card_close(s);
        return false;
    }
    const bool fresh = !s->active || s->identity != identity;
    s->identity = identity;
    s->anchor = anchor;
    s->viewport = viewport;
    s->username_bytes = utf8_copy_truncated(s->username_text, kUiNamesCardTextBytes, content.username);
    s->row_count = minimum(content.row_count, kUiNamesCardRows);
    const Utf8View username{s->username_text, s->username_bytes};
    const float username_width = text_width(measure, username, text_style_bold, kUiNamesCardMaximum - kLabelWidth - kUiNamesCardGap);
    float widest = username_width + kUiNamesCardGap + kLabelWidth;
    u64 hash = hash64_mix(kHash64Seed, identity);
    hash = hash64_counted_bytes(hash, s->username_text, s->username_bytes);
    for (u32 row = 0; row < s->row_count; ++row) {
        const auto& in = content.rows[row];
        s->nickname_bytes[row] = utf8_copy_truncated(s->nickname_text[row], kUiNamesCardTextBytes, in.nickname);
        s->icon_count[row] = minimum(in.server_count, kUiNamesCardIcons);
        const u32 beyond = in.server_count - s->icon_count[row];
        s->more_bytes[row] = 0;
        if (beyond) {
            s->more_text[row][0] = '+';
            s->more_bytes[row] = 1 + decimal_write(s->more_text[row] + 1, beyond);
        }
        for (u32 i = 0; i < kUiNamesCardIcons; ++i) {
            const u32 at = row * kUiNamesCardIcons + i;
            s->keys[at] = i < s->icon_count[row] ? in.icons[i] : MediaAssetKey{};
            s->initial_bytes[at] =
                i < s->icon_count[row] ? utf8_copy_truncated(s->initial_text[at], 8, in.initials[i]) : 0;
            hash = hash64_bytes(hash, &s->keys[at], sizeof(s->keys[at]));
            hash = hash64_counted_bytes(hash, s->initial_text[at], s->initial_bytes[at]);
        }
        const float nickname = text_width(measure, {s->nickname_text[row], s->nickname_bytes[row]}, text_style_subtext,
                                          kUiNamesCardMaximum);
        widest = maximum(widest, nickname + kUiNamesCardNicknameGap +
                                     pictures_width(s->icon_count[row], s->more_bytes[row] != 0));
        hash = hash64_counted_bytes(hash, s->nickname_text[row], s->nickname_bytes[row]);
        hash = hash64_mix(hash, (u64{s->more_bytes[row]} << 32) | s->icon_count[row]);
    }
    const float width = minimum(kUiNamesCardMaximum, maximum(kUiNamesCardMinimum, widest)) + 2 * kUiNamesCardPadding;
    const float rows = static_cast<float>(s->row_count);
    const float height = 2 * kUiNamesCardPadding + kUiNamesCardLine + 2 * kUiNamesCardGap + 1.f +
                         rows * kUiNamesCardLine + (rows - 1.f) * kUiNamesCardRowGap;
    // Under the avatar and 12 DIPs left of it; above it when the window ends first.
    float left = minimum(anchor.left - kAnchorOverlap, viewport.right - kUiNamesCardGap - width);
    left = maximum(viewport.left + kUiNamesCardGap, left);
    float top = anchor.bottom + kUiNamesCardGap;
    if (top + height > viewport.bottom - kUiNamesCardGap)
        top = maximum(viewport.top + kUiNamesCardGap, anchor.top - kUiNamesCardGap - height);
    s->settled = s->panel = {left, top, left + width, top + height};
    if (fresh)
        ui_hover_card_restart(s, kEntrance, now_ms, reduced_motion);
    s->progress = s->entrance.value;
    layout(*s, username_width);
    if (!ui_widget_manager_layout_rectangle(&s->widgets, viewport).ok()) {
        ui_names_card_close(s);
        return false;
    }
    hash = hash64_bytes(hash, &s->panel, sizeof(s->panel));
    ui_hover_card_publish(s, hash);
    return true;
}

void ui_names_card_close(UiNamesCardScene* s) noexcept {
    ui_hover_card_close(s);
}

bool ui_names_card_tick(UiNamesCardScene* s, u64 now_ms, bool reduced_motion) noexcept {
    if (!ui_hover_card_advance(s, kEntrance, now_ms, reduced_motion))
        return false;
    ++s->revision;
    return true;
}

u64 ui_names_card_next_wake(const UiNamesCardScene& s, bool reduced_motion) noexcept {
    return ui_hover_card_next_wake(s, kEntrance, reduced_motion);
}

bool ui_names_card_contains(const UiNamesCardScene& s, float x, float y) noexcept {
    return ui_hover_card_contains(s, x, y);
}

bool ui_names_card_media(const UiNamesCardScene& s, u32 index, UiNamesCardMedia* output) noexcept {
    const u32 row = index / kUiNamesCardIcons, icon = index % kUiNamesCardIcons;
    if (!output || !s.active || row >= s.row_count || icon >= s.icon_count[row] || !media_asset_key_valid(s.keys[index]))
        return false;
    *output = {s.keys[index], s.icons[index], s.panel, clamp_unit(s.progress)};
    return true;
}

bool ui_names_card_paint(const UiNamesCardScene& s, const Theme& theme, PaintStream* paint) noexcept {
    if (!s.active)
        return true;
    UiPaint p{paint, theme};
    const float a = clamp_unit(s.progress);
    p.use<ui_hover_card_paint_surface>(s, theme, theme.scale.radius_md);
    p.fill(s.divider, ui_theme_color_alpha(theme.tokens.border.subtle, a));
    for (u32 row = 0; row < s.row_count; ++row)
        for (u32 i = 0; i < s.icon_count[row]; ++i) {
            const u32 at = row * kUiNamesCardIcons + i;
            // The server's picture covers its disc once resident; a server without one keeps it and its initial.
            p.rounded(s.icons[at], ui_theme_color_alpha(theme.avatar_placeholder, a), ui_rect_width(s.icons[at]) * .5f);
        }
    return p.end();
}

bool ui_names_card_text_output(const UiNamesCardScene& s, const Theme& theme, UiTextOutput* out) noexcept {
    if (!s.active)
        return true;
    UiTextPen<UiTextClip::region> text{out, s.widgets, s.panel};
    const auto owner = s.owner.handle;
    const float a = clamp_unit(s.progress);
    text(owner, {s.username_text, s.username_bytes}, s.username, ui_theme_color_alpha(theme.tokens.text.normal, a),
         text_style_bold);
    text(owner, utf8_literal("AKA"), s.label, ui_theme_color_alpha(theme.tokens.text.muted, a),
         text_style_subtext | text_style_bold);
    for (u32 row = 0; row < s.row_count; ++row) {
        text(owner, {s.nickname_text[row], s.nickname_bytes[row]}, s.nicknames[row],
             ui_theme_color_alpha(theme.tokens.text.normal, a), text_style_subtext);
        if (s.more_bytes[row])
            text(owner, {s.more_text[row], s.more_bytes[row]}, s.more[row],
                 ui_theme_color_alpha(theme.tokens.text.muted, a), text_style_subtext, ui_text_end);
        for (u32 i = 0; i < s.icon_count[row]; ++i) {
            const u32 at = row * kUiNamesCardIcons + i;
            if (s.initial_bytes[at] && !media_asset_key_valid(s.keys[at]))
                text(owner, {s.initial_text[at], s.initial_bytes[at]}, s.icons[at],
                     ui_theme_color_alpha(theme.tokens.text.normal, a), text_style_subtext | text_style_bold,
                     ui_text_center | ui_text_clip);
        }
    }
    return text.ok();
}
} // namespace maicord
