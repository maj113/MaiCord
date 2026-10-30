#include "maicord/ui/expression_card_scene.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/ui/text_style.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {
namespace {
constexpr float kW = kUiExpressionCardWidth, kP = kUiExpressionCardPadding, kSlide = 8;
// The picture sits 4 DIPs low beside the text (the official primaryEmoji).
constexpr float kPictureTop = 4, kColumnGap = 12, kTitleLine = 20, kLine = 18, kSmallLine = 16;
constexpr float kHeadingGap = 8, kGuildRow = 36, kGuildGap = 8, kListGap = 8, kButton = 32, kSkeleton = 78;
constexpr UiHoverCardMotion kEntrance{{kUiExpressionCardEntranceMs, UiMotionCurve::spring},
                                      0x4558'5052'4341'5244ull};
constexpr u64 kMeasureSource = 0x4558505243415244ull; // "EXPRCARD"

u32 copy_text(char* destination, u32 capacity, Utf8View text) noexcept {
    return text.data ? utf8_copy_truncated(destination, capacity, text) : 0;
}
// The description's plain and bold runs (the pack's name).
u32 description_runs(const UiExpressionCardScene& s, TextStyleSpan* runs) noexcept {
    if (!s.emphasis_bytes || s.emphasis_offset + s.emphasis_bytes > s.description_bytes) {
        runs[0] = {0, s.description_bytes, text_style_subtext};
        return 1;
    }
    u32 count = 0;
    if (s.emphasis_offset)
        runs[count++] = {0, s.emphasis_offset, text_style_subtext};
    runs[count++] = {s.emphasis_offset, s.emphasis_bytes, text_style_subtext | text_style_bold};
    const u32 end = s.emphasis_offset + s.emphasis_bytes;
    if (end < s.description_bytes)
        runs[count++] = {end, s.description_bytes - end, text_style_subtext};
    return count;
}
// Measured height of text width wide (one line when single); an estimate
// without a measurement cache.
float measure(UiTextMeasureCache* cache, Utf8View text, ConstSlice<TextStyleSpan> styles, float width, bool single,
              u16 item, float line) noexcept {
    if (!text.bytes)
        return 0;
    const float estimate = static_cast<float>(text.bytes) * 7.f;
    float height = single ? line : line * static_cast<float>(static_cast<u32>(estimate / maximum(width, 1.f)) + 1);
    if (!cache)
        return height;
    UiTextMeasureValue value{};
    if (ui_text_measure_cache_measure(
            cache,
            {{kMeasureSource,
              hash64_bytes(hash64_bytes(kHash64Seed, text.data, text.bytes), styles.data,
                           styles.count * static_cast<u32>(sizeof(TextStyleSpan))),
              0, 0, item},
             text,
             styles,
             {width, 4096},
             single},
            &value)
            .ok() &&
        ui_coordinate_valid(value.size.height) && value.size.height > 0)
        height = single ? maximum(line, value.size.height) : value.size.height;
    return height;
}
UiRect at(UiRect r, float dx, float dy) noexcept {
    return ui_rect_has_area(r) ? UiRect{r.left + dx, r.top + dy, r.right + dx, r.bottom + dy} : UiRect{};
}
float opacity(const UiExpressionCardScene& s) noexcept {
    return s.progress < 0 ? 0.f : s.progress > 1 ? 1.f : s.progress;
}
// Geometry relative to the card's top-left; returns the card's height.
float arrange(UiExpressionCardScene& s, UiTextMeasureCache* cache) noexcept {
    const auto clear = [](UiRect* r) noexcept { reset_value(r); };
    clear(&s.picture);
    clear(&s.title);
    clear(&s.description);
    clear(&s.guild_section);
    clear(&s.guild_heading);
    clear(&s.guild_icon);
    clear(&s.guild_name);
    clear(&s.guild_subtitle);
    clear(&s.button);
    clear(&s.skeleton);
    for (auto& preview : s.previews)
        clear(&preview);
    if (s.layout == UiExpressionCardLayout::loading) {
        s.skeleton = {kP, kP, kW - kP, kP + kSkeleton};
        return kP + kSkeleton + kP;
    }
    TextStyleSpan runs[3]{};
    const u32 run_count = description_runs(s, runs);
    const TextStyleSpan bold{0, s.title_bytes, text_style_bold};
    const Utf8View title{s.title_text, s.title_bytes}, description{s.description_text, s.description_bytes};
    float bottom = 0;
    if (s.layout == UiExpressionCardLayout::pack) {
        const float inner = kW - kP * 2;
        const float title_height = measure(cache, title, {&bold, 1}, inner, true, 1, kTitleLine);
        s.title = {kP, kP, kW - kP, kP + title_height};
        const float text = measure(cache, description, {runs, run_count}, inner, false, 2, kLine);
        s.description = {kP, s.title.bottom, kW - kP, s.title.bottom + text};
        bottom = s.description.bottom;
        if (s.preview_count) {
            const float top = bottom + kListGap;
            const float step = s.preview_count > 1 ? (inner - kUiExpressionCardPreview) / (s.preview_count - 1) : 0;
            for (u32 i = 0; i < s.preview_count; ++i)
                s.previews[i] = {kP + step * static_cast<float>(i), top,
                                 kP + step * static_cast<float>(i) + kUiExpressionCardPreview,
                                 top + kUiExpressionCardPreview};
            bottom = top + kUiExpressionCardPreview + kListGap;
        }
        if (s.button_bytes) {
            s.button = {kP, bottom, kW - kP, bottom + kButton};
            bottom = s.button.bottom;
        }
        return bottom + kP;
    }
    // Head: the picture beside a column centred on it.
    s.picture = {kP, kP + kPictureTop, kP + kUiExpressionCardPicture, kP + kPictureTop + kUiExpressionCardPicture};
    const float left = s.picture.right + kColumnGap, width = kW - kP - left;
    const float title_height = measure(cache, title, {&bold, 1}, width, true, 1, kTitleLine);
    const float text = measure(cache, description, {runs, run_count}, width, false, 2, kLine);
    const float column = title_height + text, block = maximum(kPictureTop + kUiExpressionCardPicture, column);
    const float top = kP + (block - column) * .5f;
    s.title = {left, top, kW - kP, top + title_height};
    if (text)
        s.description = {left, s.title.bottom, kW - kP, s.title.bottom + text};
    bottom = kP + block + kP;
    if (!s.guild_heading_bytes)
        return bottom;
    s.guild_section = {0, bottom, kW, bottom + kP + kLine + kHeadingGap + kGuildRow + kP};
    s.guild_heading = {kP, bottom + kP, kW - kP, bottom + kP + kLine};
    const float row = s.guild_heading.bottom + kHeadingGap;
    s.guild_icon = {kP, row + (kGuildRow - kUiExpressionCardGuildIcon) * .5f, kP + kUiExpressionCardGuildIcon,
                    row + (kGuildRow + kUiExpressionCardGuildIcon) * .5f};
    s.guild_name = {s.guild_icon.right + kGuildGap, row, kW - kP, row + kTitleLine};
    s.guild_subtitle = {s.guild_name.left, s.guild_name.bottom, kW - kP, s.guild_name.bottom + kSmallLine};
    return s.guild_section.bottom;
}
// Right of the anchor when it fits, else left; centred on it, inside the viewport.
UiRect place(UiRect anchor, UiRect viewport, float height) noexcept {
    float left = anchor.right + kUiExpressionCardGap;
    if (left + kW > viewport.right - kUiExpressionCardGap)
        left = anchor.left - kUiExpressionCardGap - kW;
    left = maximum(viewport.left + kUiExpressionCardGap, left);
    float top = (anchor.top + anchor.bottom - height) * .5f;
    top = minimum(top, viewport.bottom - kUiExpressionCardGap - height);
    top = maximum(viewport.top + kUiExpressionCardGap, top);
    return {left, top, left + kW, top + height};
}
// The settled card's geometry offset by its entrance slide.
struct Frame {
    float dx, dy;
};
Frame frame(const UiExpressionCardScene& s) noexcept {
    return {s.panel.left, s.panel.top};
}
void slide(UiExpressionCardScene& s) noexcept {
    const float side = s.settled.left >= s.anchor.right ? -1.f : 1.f;
    const float dx = side * kSlide * (1 - s.progress);
    s.panel = {s.settled.left + dx, s.settled.top, s.settled.right + dx, s.settled.bottom};
}
bool push(UiTextOutput* text, const UiExpressionCardScene& s, Utf8View value, UiRect rectangle, Color color,
          u32 style, bool wrap = false, UiTextAlignment horizontal = UiTextAlignment::start,
          ConstSlice<TextStyleSpan> styles = {}) noexcept {
    if (!value.bytes || !ui_rect_has_area(rectangle))
        return true;
    return ui_text_output_push(text, {value, s.owner, rectangle, s.panel, color, style, horizontal,
                                      UiTextAlignment::center, wrap ? UiTextWrapping::wrap : UiTextWrapping::no_wrap,
                                      UiTextOverflow::ellipsis, UiTextFace::normal, {}, styles});
}
} // namespace

bool ui_expression_card_show(UiExpressionCardScene* s, u64 identity, const UiExpressionCardContent& content,
                             UiRect anchor, UiRect viewport, u64 now_ms, bool reduced_motion,
                             UiTextMeasureCache* measure_cache) noexcept {
    if (!s || !identity || !ui_rect_valid(viewport) || !ui_rect_has_area(anchor) ||
        content.layout > UiExpressionCardLayout::pack || content.preview_count > kUiExpressionCardPreviews ||
        !ui_hover_card_initialize(*s, utf8_literal("Expression popout"))) {
        if (s && s->active)
            ui_expression_card_close(s);
        return false;
    }
    const bool fresh = !s->active || s->identity != identity;
    s->identity = identity;
    s->anchor = anchor;
    s->viewport = viewport;
    s->layout = content.layout;
    s->picture_key = content.picture;
    s->glyph_bytes = content.glyph.bytes <= sizeof(s->glyph) ? copy_text(s->glyph, sizeof(s->glyph), content.glyph) : 0;
    s->title_bytes = copy_text(s->title_text, sizeof(s->title_text), content.title);
    s->description_bytes = copy_text(s->description_text, sizeof(s->description_text), content.description);
    s->emphasis_offset = content.emphasis_offset;
    s->emphasis_bytes = content.emphasis_bytes;
    s->button_bytes = copy_text(s->button_text, sizeof(s->button_text), content.button);
    s->guild_heading_bytes = copy_text(s->guild_heading_text, sizeof(s->guild_heading_text), content.guild_heading);
    s->guild_name_bytes = copy_text(s->guild_name_text, sizeof(s->guild_name_text), content.guild_name);
    s->guild_subtitle_bytes = copy_text(s->guild_subtitle_text, sizeof(s->guild_subtitle_text), content.guild_subtitle);
    s->guild_acronym_bytes = copy_text(s->guild_acronym_text, sizeof(s->guild_acronym_text), content.guild_acronym);
    s->guild_icon_key = content.guild_icon;
    s->guild_link = content.guild_link;
    s->preview_count = content.preview_count;
    for (u32 i = 0; i < kUiExpressionCardPreviews; ++i)
        s->preview_keys[i] = i < content.preview_count ? content.previews[i] : MediaAssetKey{};
    if (fresh) {
        s->guild_hovered = false;
        ui_hover_card_restart(s, kEntrance, now_ms, reduced_motion);
    }
    const float height = arrange(*s, measure_cache);
    s->settled = place(anchor, viewport, height);
    s->progress = s->entrance.value;
    slide(*s);
    if (!ui_widget_manager_layout_rectangle(&s->widgets, viewport).ok()) {
        ui_expression_card_close(s);
        return false;
    }
    u64 hash = hash64_mix(kHash64Seed, identity);
    hash = hash64_counted_bytes(hash, s->title_text, s->title_bytes);
    hash = hash64_counted_bytes(hash, s->description_text, s->description_bytes);
    hash = hash64_counted_bytes(hash, s->guild_name_text, s->guild_name_bytes);
    hash = hash64_counted_bytes(hash, s->guild_subtitle_text, s->guild_subtitle_bytes);
    hash = hash64_counted_bytes(hash, s->glyph, s->glyph_bytes);
    hash = hash64_bytes(hash, s->preview_keys, sizeof(s->preview_keys));
    hash = hash64_bytes(hash, &s->picture_key, sizeof(s->picture_key));
    hash = hash64_bytes(hash, &s->guild_icon_key, sizeof(s->guild_icon_key));
    hash = hash64_bytes(hash, &s->panel, sizeof(s->panel));
    hash = hash64_mix(hash, static_cast<u64>(s->layout) | u64{s->button_bytes} << 8 | u64{s->guild_link} << 40);
    ui_hover_card_publish(s, hash);
    return true;
}

void ui_expression_card_close(UiExpressionCardScene* s) noexcept {
    ui_hover_card_close(s);
}

bool ui_expression_card_tick(UiExpressionCardScene* s, u64 now_ms, bool reduced_motion) noexcept {
    if (!ui_hover_card_advance(s, kEntrance, now_ms, reduced_motion))
        return false;
    slide(*s);
    ++s->revision;
    return true;
}

u64 ui_expression_card_next_wake(const UiExpressionCardScene& s, u64 now_ms, bool reduced_motion) noexcept {
    return ui_hover_card_next_wake(s, kEntrance, now_ms, reduced_motion);
}

bool ui_expression_card_contains(const UiExpressionCardScene& s, float x, float y) noexcept {
    return ui_hover_card_contains(s, x, y);
}

UiExpressionCardControl ui_expression_card_hit(const UiExpressionCardScene& s, float x, float y) noexcept {
    if (!s.active)
        return UiExpressionCardControl::none;
    const auto f = frame(s);
    if (ui_rect_contains(at(s.button, f.dx, f.dy), x, y))
        return UiExpressionCardControl::button;
    if (s.guild_link && (ui_rect_contains(at(s.guild_icon, f.dx, f.dy), x, y) ||
                         ui_rect_contains(at(s.guild_name, f.dx, f.dy), x, y)))
        return UiExpressionCardControl::guild;
    return UiExpressionCardControl::none;
}

bool ui_expression_card_set_hover(UiExpressionCardScene* s, float x, float y) noexcept {
    if (!s)
        return false;
    const auto hit = ui_expression_card_hit(*s, x, y);
    const bool guild = hit == UiExpressionCardControl::guild;
    const bool changed = guild != s->guild_hovered;
    s->guild_hovered = guild;
    if (changed)
        ++s->revision;
    return ui_hover_card_set_hovered(s, hit == UiExpressionCardControl::button) || changed;
}

bool ui_expression_card_set_pressed(UiExpressionCardScene* s, bool pressed) noexcept {
    return ui_hover_card_set_pressed(s, pressed);
}

bool ui_expression_card_media(const UiExpressionCardScene& s, u32 index, UiExpressionCardMedia* output) noexcept {
    if (!output || !s.active || index >= kUiExpressionCardMediaCount)
        return false;
    const auto f = frame(s);
    const float a = opacity(s);
    if (index == 0) {
        if (!media_asset_key_valid(s.picture_key) || !ui_rect_has_area(s.picture))
            return false;
        *output = {s.picture_key, at(s.picture, f.dx, f.dy), s.panel, 0, a};
        return true;
    }
    if (index <= kUiExpressionCardPreviews) {
        if (index > s.preview_count || !media_asset_key_valid(s.preview_keys[index - 1]))
            return false;
        *output = {s.preview_keys[index - 1], at(s.previews[index - 1], f.dx, f.dy), s.panel, 0, a};
        return true;
    }
    if (!media_asset_key_valid(s.guild_icon_key) || !ui_rect_has_area(s.guild_icon))
        return false;
    // The official card masks the icon to a squircle; a 12 DIP radius approximates it.
    *output = {s.guild_icon_key, at(s.guild_icon, f.dx, f.dy), s.panel, 12, a};
    return true;
}

bool ui_expression_card_paint(const UiExpressionCardScene& s, const Theme& theme, PaintStream* paint) noexcept {
    if (!s.active)
        return true;
    if (!paint)
        return false;
    const float a = opacity(s);
    const auto f = frame(s);
    const float radius = theme.scale.radius_md;
    const Color fill = ui_theme_color_alpha(theme.tokens.background.floating, a);
    const Color border = ui_theme_color_alpha(theme.tokens.border.subtle, a);
    bool ok = ui_surface_paint(paint, s.panel, theme, UiElevation::popover, radius, &fill, &border);
    const Color plate = ui_theme_color_alpha(theme.tokens.fill.muted, a);
    if (ok && ui_rect_has_area(s.skeleton)) {
        // The loader's shape: a sticker square and two lines of text.
        const auto k = at(s.skeleton, f.dx, f.dy);
        ok = paint_stream_push_rounded(paint, {k.left, k.top, k.left + 58, k.top + 58}, plate, theme.scale.radius_sm) &&
             paint_stream_push_rounded(paint, {k.left + 74, k.top, k.left + 151, k.top + 18}, plate,
                                       theme.scale.radius_sm) &&
             paint_stream_push_rounded(paint, {k.left + 74, k.top + 34, k.right, k.top + 52}, plate,
                                       theme.scale.radius_sm);
    }
    if (ok && ui_rect_has_area(s.guild_section)) {
        // The server section fills the card's bottom, inside its rounded corners.
        const auto g = at(s.guild_section, f.dx, f.dy);
        const Color section = ui_theme_color_alpha(theme.tokens.background.secondary, a);
        ok = paint_stream_push_rounded(paint, {g.left, g.top, g.right, g.bottom - radius}, section, 0) &&
             paint_stream_push_rounded(paint, {g.left, g.bottom - radius * 2, g.right, g.bottom}, section, radius);
        if (ok && !media_asset_key_valid(s.guild_icon_key))
            ok = paint_stream_push_rounded(paint, at(s.guild_icon, f.dx, f.dy), plate, theme.scale.radius_lg);
    }
    if (ok && ui_rect_has_area(s.button)) {
        Color button = theme.tokens.fill.muted;
        if (s.hovered) {
            const Color state = s.pressed ? theme.tokens.state.pressed : theme.tokens.state.hover;
            button.red += (state.red - button.red) * state.alpha;
            button.green += (state.green - button.green) * state.alpha;
            button.blue += (state.blue - button.blue) * state.alpha;
            button.alpha = minimum(1.f, button.alpha + state.alpha);
        }
        ok = paint_stream_push_rounded(paint, at(s.button, f.dx, f.dy), ui_theme_color_alpha(button, a), radius);
    }
    return ok;
}

bool ui_expression_card_text_output(const UiExpressionCardScene& s, const Theme& theme, UiTextOutput* text) noexcept {
    if (!s.active)
        return true;
    if (!text)
        return false;
    const float a = opacity(s);
    const auto f = frame(s);
    const Color normal = ui_theme_color_alpha(theme.tokens.text.normal, a);
    const Color muted = ui_theme_color_alpha(theme.tokens.text.muted, a);
    bool ok = true;
    if (s.glyph_bytes && !media_asset_key_valid(s.picture_key))
        ok = push(text, s, {s.glyph, s.glyph_bytes}, at(s.picture, f.dx, f.dy), normal, text_style_jumbo_emoji, false,
                  UiTextAlignment::center);
    TextStyleSpan runs[3]{};
    const u32 run_count = description_runs(s, runs);
    const TextStyleSpan bold{0, s.title_bytes, text_style_bold};
    ok = ok &&
         push(text, s, {s.title_text, s.title_bytes}, at(s.title, f.dx, f.dy), normal, text_style_bold, false,
              UiTextAlignment::start, {&bold, 1}) &&
         push(text, s, {s.description_text, s.description_bytes}, at(s.description, f.dx, f.dy), normal,
              text_style_subtext, true, UiTextAlignment::start, {runs, run_count}) &&
         push(text, s, {s.guild_heading_text, s.guild_heading_bytes}, at(s.guild_heading, f.dx, f.dy), muted,
              text_style_subtext) &&
         push(text, s, {s.guild_name_text, s.guild_name_bytes}, at(s.guild_name, f.dx, f.dy), normal,
              text_style_bold | (s.guild_link && s.guild_hovered ? text_style_underline : 0u)) &&
         push(text, s, {s.guild_subtitle_text, s.guild_subtitle_bytes}, at(s.guild_subtitle, f.dx, f.dy), normal,
              kTextStyleSizeCodeXs);
    if (ok && !media_asset_key_valid(s.guild_icon_key))
        ok = push(text, s, {s.guild_acronym_text, s.guild_acronym_bytes}, at(s.guild_icon, f.dx, f.dy), normal,
                  text_style_subtext, false, UiTextAlignment::center);
    return ok && push(text, s, {s.button_text, s.button_bytes}, at(s.button, f.dx, f.dy), normal,
                      text_style_subtext | text_style_bold, false, UiTextAlignment::center);
}
} // namespace maicord
