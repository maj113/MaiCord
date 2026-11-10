#include "maicord/ui/account/activity_card.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/account/profile_scene.hpp"
#include "maicord/ui/text/style.hpp"
#include "maicord/ui/widgets/icon.hpp"

namespace maicord {
namespace {
using F = UserActivityField;
using Role = UiActivityCardRole;
using Tone = UiActivityCardTone;
using Act = UiActivityCardAction;
constexpr u32 kXs = kTextStyleSizeCodeXs, kSm = kTextStyleSizeCodeSm, kBold = text_style_bold;
// Line boxes: the title, the other text lines and the chips; buttons.
constexpr float kTitle = 18, kLine = 16, kButton = 32;
struct Metrics {
    float pad, image, small, gap;
};
Metrics metrics(UiActivityCardSize size) noexcept {
    return size == UiActivityCardSize::full     ? Metrics{12, 100, 32, 12}
           : size == UiActivityCardSize::popout ? Metrics{8, 60, 24, 8}
                                                : Metrics{0, 32, 16, 12};
}
template <u32 N> Utf8View keep(ProfileString<N>* out, Utf8View value) noexcept {
    out->bytes = static_cast<u16>(utf8_truncated_length(value, N));
    copy_bytes(out->data, value.data, out->bytes);
    return out->view();
}
UiRect offset(const UiRect& r, UiPoint o) noexcept {
    return {r.left + o.x, r.top + o.y, r.right + o.x, r.bottom + o.y};
}
struct Builder {
    UiActivityCard& c;
    const UiActivityCardInput& in;

    // kUiActivityCardItems holds the most a layout adds.
    UiActivityCardItem* add(Role role, const UiRect& r, Utf8View text = {}, u32 flags = 0,
                            Tone tone = Tone::normal) noexcept {
        auto* item = &c.items[c.count++];
        *item = {r, text, {}, flags, role, tone, Act::none, 0, UiIcon::count, false, false};
        return item;
    }
    // The item added last.
    u8 last() const noexcept {
        return static_cast<u8>(c.count - 1);
    }
    UiSize measure(Utf8View text, u32 flags, float width, bool wrap) const noexcept {
        if (in.measure)
            return in.measure(in.context, text, flags, width, wrap);
        const bool xs = (flags & kTextStyleHeadingMask) == kXs;
        return {minimum(width, text.bytes * (xs ? 6.f : 7.f)), xs ? kLine : kTitle};
    }
    // One line's width in whole DIPs, at most limit.
    float width(Utf8View text, u32 flags, float limit = 4096) const noexcept {
        return minimum(limit, static_cast<float>(static_cast<u32>(measure(text, flags, 4096, false).width + .999f)));
    }
    // A changing time string's width at its length (digits as 8s).
    float clock_width(Utf8View text) const noexcept {
        char widest[16];
        const u32 bytes = minimum<u32>(text.bytes, sizeof(widest));
        for (u32 i = 0; i < bytes; ++i)
            widest[i] = text.data[i] >= '0' && text.data[i] <= '9' ? '8' : text.data[i];
        return width({widest, bytes}, kXs) + 1;
    }
    // A text line: the full card wraps it; elsewhere it stays on one line and
    // a cut-off line carries its full text as its hint. Returns its height.
    float text(Role role, float x, float y, float w, Utf8View value, u32 flags, float line, Tone tone,
               Act action = Act::none) noexcept {
        if (!value.bytes)
            return 0;
        const bool wrap = in.size == UiActivityCardSize::full;
        const float h = wrap ? maximum(line, measure(value, flags, w, true).height) : line;
        auto* item = add(role, {x, y, x + w, y + h}, value, flags, tone);
        item->wrap = h > line;
        item->action = action;
        if (!wrap && in.measure && measure(value, flags, 4096, false).width > w)
            item->hint = value;
        return h;
    }
    // A button's plate, glyph and label in the official arrangement: the
    // content centred, at least 16 DIPs from each side.
    void button(const UiRect& r, Utf8View label, UiIcon glyph, Act action, u8 index, Utf8View hint,
                bool disabled) noexcept {
        auto* plate = add(Role::button, r, label);
        plate->action = action;
        plate->index = index;
        plate->hint = hint;
        plate->disabled = disabled;
        const Tone tone = disabled ? Tone::subtle : Tone::normal;
        const float label_width = width(label, kSm | kBold, 400);
        const float content = (glyph != UiIcon::count ? 24.f : 0.f) + label_width;
        float x = r.left + maximum(16.f, (ui_rect_width(r) - content) * .5f);
        if (glyph != UiIcon::count) {
            add(Role::button_glyph, {x, r.top + 8, x + 16, r.top + 24}, {}, 0, tone)->icon = glyph;
            x += 24;
        }
        add(Role::button_label, {x, r.top, maximum(x + 1, minimum(x + label_width + 1, r.right - 16)), r.bottom},
            label, kSm | kBold, tone);
    }
};
Color tone_color(const Theme& theme, Tone tone) noexcept {
    switch (tone) {
    case Tone::subtle:
        return theme.text_secondary;
    case Tone::positive:
        return theme.tokens.text.positive;
    default:
        return theme.text_primary;
    }
}
} // namespace

bool ui_activity_card_layout(const UserActivity& a, const UiActivityCardInput& in, UiActivityCard* out) noexcept {
    reset_value(out);
    auto& c = *out;
    if (!profile_activity_content(a, in.bot, in.self, &c.content))
        return false;
    c.clock = c.clock_glyph = c.elapsed = c.bar = kUiActivityCardNone;
    const auto& t = c.content;
    const auto m = metrics(in.size);
    const bool compact = in.size == UiActivityCardSize::compact, full = in.size == UiActivityCardSize::full;
    Builder b{c, in};
    const float left = m.pad, right = maximum(left + 1, in.width - m.pad);
    float y = m.pad;
    if (!compact && t.heading.bytes) {
        b.add(Role::heading, {left, y, right, y + 16}, t.heading.view(), kXs | kBold, Tone::strong);
        y += 16 + 8;
    }
    // Spotify's track title opens the track, its album art the album.
    char url[128];
    const bool track = t.spotify && in.interactive && user_activity_spotify_url(a, {url, sizeof(url)});
    const float row = y;
    float text_left = left;
    if (t.artwork) {
        c.art = user_activity_artwork(a, in.application_icon);
        const auto hint = c.art.large == UserActivityArt::large         ? user_activity_text(a, F::large_text)
                          : c.art.large == UserActivityArt::small_image ? user_activity_text(a, F::small_text)
                                                                        : Utf8View{};
        auto* art = b.add(Role::art, {left, row, left + m.image, row + m.image});
        art->hint = keep(&c.art_hint, hint);
        art->icon = profile_activity_glyph_icon(profile_activity_placeholder(a.type));
        if (t.spotify && in.interactive && user_activity_text(a, F::session).bytes)
            art->action = Act::spotify_album;
        if (c.art.badge) {
            const float x = left + m.image + 4, sy = row + m.image + 4;
            b.add(Role::badge, {x - m.small, sy - m.small, x, sy})->hint =
                keep(&c.badge_hint, user_activity_text(a, F::small_text));
        }
        text_left += m.image + m.gap;
    }
    // Buttons: the activity's own, Watch for a stream, Play on Spotify or the
    // disabled Ask to Join; Active Now offers Ask to Join alone.
    Utf8View labels[2]{};
    u32 buttons = 0;
    Act action = Act::button;
    UiIcon glyph = UiIcon::count;
    if (in.interactive) {
        switch (t.action) {
        case ProfileActivityAction::custom:
            for (u32 i = 0; i < a.button_count && i < 2 && !compact; ++i)
                labels[buttons++] = keep(&c.labels[i], user_activity_text(a, i ? F::button_1 : F::button_0));
            break;
        case ProfileActivityAction::watch:
            if (!compact)
                labels[buttons++] = utf8_literal("Watch");
            action = Act::watch;
            break;
        case ProfileActivityAction::join:
            labels[buttons++] = utf8_literal("Ask to Join");
            action = Act::join;
            glyph = UiIcon::controller;
            break;
        case ProfileActivityAction::spotify:
            if (!compact)
                labels[buttons++] = utf8_literal("Play on Spotify");
            action = Act::spotify_track;
            glyph = UiIcon::play;
            break;
        default:
            break;
        }
    }
    const bool join = action == Act::join;
    const Utf8View button_hint = join ? t.hint.view() : Utf8View{};
    const auto natural = [&](Utf8View label) noexcept {
        return b.width(label, kSm | kBold, 400) + 32 + (glyph != UiIcon::count ? 24.f : 0.f);
    };
    float text_right = right;
    if (compact && buttons) {
        // The button's own column on the right, top-aligned with the art.
        const float w = minimum(natural(labels[0]), maximum(1.f, (right - text_left) * .5f));
        b.button({right - w, row, right, row + kButton}, labels[0], glyph, action, 0, button_hint, join);
        text_right = right - w - m.gap;
    }
    const float column = maximum(1.f, text_right - text_left);
    // The details column, laid out from the row's top, then centred on the art.
    const u32 first = c.count;
    const Tone line_tone = compact ? Tone::subtle : Tone::normal;
    float cy = row;
    cy += b.text(Role::title, text_left, cy, column, t.title.view(), kSm | kBold, kTitle, Tone::normal,
                 track ? Act::spotify_track : Act::none);
    for (u32 i = 0; i < t.line_count; ++i)
        cy += b.text(Role::line, text_left, cy, column, t.lines[i].view(), kXs, kLine, line_tone);
    const auto clock = t.chips ? profile_activity_clock(a, in.now_ms) : ProfileActivityClock{};
    const bool timed = clock.timer != ProfileActivityTimer::none;
    if (t.chips && (t.party.bytes || t.episode.bytes || timed)) {
        if (cy > row)
            cy += 4;
        float x = 0, line_top = cy;
        const auto chip = [&](ProfileActivityGlyph kind, Utf8View text, bool ticking) noexcept {
            const float label = ticking ? b.clock_width(text) : b.width(text, kXs);
            const float w = minimum(column, 16 + label);
            if (x > 0 && x + w > column) {
                x = 0;
                line_top += kLine;
            }
            const float cx = text_left + x;
            b.add(ticking ? Role::clock_glyph : Role::glyph, {cx, line_top + 2, cx + 12, line_top + 14}, {}, 0,
                  ticking ? Tone::positive : Tone::subtle)
                ->icon = profile_activity_glyph_icon(kind);
            if (ticking)
                c.clock_glyph = b.last();
            b.add(ticking ? Role::clock : Role::chip, {cx + 16, line_top, cx + w, line_top + kLine}, text, kXs,
                  ticking ? Tone::positive : Tone::normal);
            if (ticking)
                c.clock = b.last();
            x += w + 8;
        };
        auto& clock_text = c.time[0];
        clock_text.bytes = static_cast<u16>(
            profile_activity_clock_text(clock.seconds, {clock_text.data, sizeof(clock_text.data)}));
        if (t.party.bytes)
            chip(ProfileActivityGlyph::party, t.party.view(), false);
        if (timed)
            chip(clock.timer == ProfileActivityTimer::countdown ? ProfileActivityGlyph::timer : t.clock_glyph,
                 clock_text.view(), true);
        if (t.episode.bytes)
            chip(ProfileActivityGlyph::episode, t.episode.view(), false);
        cy = line_top + kLine;
    }
    ProfileActivityProgress progress{};
    if (profile_activity_progress(a, in.now_ms, &progress)) {
        if (cy > row)
            cy += 4;
        auto& elapsed = c.time[1];
        auto& total = c.time[2];
        elapsed.bytes = static_cast<u16>(
            profile_activity_track_text(progress.elapsed, {elapsed.data, sizeof(elapsed.data)}));
        total.bytes = static_cast<u16>(profile_activity_track_text(progress.duration, {total.data, sizeof(total.data)}));
        const float label = maximum(b.clock_width(elapsed.view()), b.clock_width(total.view()));
        b.add(Role::elapsed, {text_left, cy, text_left + label, cy + kLine}, elapsed.view(), kXs);
        c.elapsed = b.last();
        b.add(Role::total, {text_right - label, cy, text_right, cy + kLine}, total.view(), kXs);
        b.add(Role::bar, {text_left + label + 8, cy + 7, maximum(text_left + label + 9, text_right - label - 8), cy + 9});
        c.bar = b.last();
        c.fraction = progress.fraction;
        cy += kLine;
    }
    if (full && buttons) {
        cy += cy > row ? 8.f : 0.f;
        float x = 0;
        for (u32 i = 0; i < buttons; ++i) {
            const float w = minimum(column, natural(labels[i]));
            if (x > 0 && x + w > column) {
                x = 0;
                cy += kButton + 8;
            }
            const float bw = minimum(column - x, w);
            b.button({text_left + x, cy, text_left + x + bw, cy + kButton}, labels[i], glyph, action,
                     static_cast<u8>(i), button_hint, join);
            x += bw + 8;
        }
        cy += kButton;
    }
    const float details = cy - row;
    const float body = t.artwork ? maximum(m.image, details) : details;
    const float dy = (body - details) * .5f;
    for (u32 i = first; dy != 0 && i < c.count; ++i) {
        c.items[i].rect.top += dy;
        c.items[i].rect.bottom += dy;
    }
    float bottom = row + maximum(body, compact && buttons ? kButton : 0.f);
    if (in.size == UiActivityCardSize::popout && buttons) {
        bottom += 12;
        for (u32 i = 0; i < buttons; ++i) {
            b.button({left, bottom, right, bottom + kButton}, labels[i], glyph, action, static_cast<u8>(i),
                     button_hint, join);
            bottom += kButton + (i + 1 < buttons ? 8.f : 0.f);
        }
    }
    c.height = bottom + m.pad;
    return true;
}

UiActivityCardTick ui_activity_card_tick(UiActivityCard* c, const UserActivity& a, u64 now_ms) noexcept {
    bool changed = false, relayout = false;
    // Same-length text is rewritten in place (digits keep their width).
    const auto rewrite = [&](ProfileString<16>* stored, Utf8View text) noexcept {
        if (text.bytes != stored->bytes)
            relayout = true;
        else if (!bytes_equal(stored->data, text.data, text.bytes)) {
            copy_bytes(stored->data, text.data, text.bytes);
            changed = true;
        }
    };
    char text[16];
    if (c->clock != kUiActivityCardNone) {
        const auto clock = profile_activity_clock(a, now_ms);
        if (clock.timer == ProfileActivityTimer::none || c->clock_glyph == kUiActivityCardNone)
            return UiActivityCardTick::relayout;
        const auto glyph = profile_activity_glyph_icon(
            clock.timer == ProfileActivityTimer::countdown ? ProfileActivityGlyph::timer : c->content.clock_glyph);
        auto& icon = c->items[c->clock_glyph].icon;
        changed |= icon != glyph;
        icon = glyph;
        rewrite(&c->time[0], {text, profile_activity_clock_text(clock.seconds, {text, sizeof(text)})});
    }
    ProfileActivityProgress progress{};
    if (c->bar != kUiActivityCardNone) {
        if (!profile_activity_progress(a, now_ms, &progress))
            return UiActivityCardTick::relayout;
        rewrite(&c->time[1], {text, profile_activity_track_text(progress.elapsed, {text, sizeof(text)})});
        changed |= c->fraction != progress.fraction;
        c->fraction = progress.fraction;
    }
    return relayout  ? UiActivityCardTick::relayout
           : changed ? UiActivityCardTick::changed
                     : UiActivityCardTick::unchanged;
}
bool ui_activity_card_timed(const UiActivityCard& c) noexcept {
    return c.clock != kUiActivityCardNone || c.bar != kUiActivityCardNone;
}

bool ui_activity_card_paint(const UiActivityCard& c, UiPoint o, const Theme& theme, PaintStream* paint) noexcept {
    UiPaint p{paint, theme};
    for (u32 i = 0; p && i < c.count; ++i) {
        const auto& item = c.items[i];
        const auto r = offset(item.rect, o);
        switch (item.role) {
        case Role::art: {
            // The placeholder; the host draws the decoded image over it.
            const float inset = ui_rect_width(r) * .3f;
            p.rounded(r, theme.tokens.fill.muted, theme.scale.radius_xs)
                .icon(item.icon, ui_rect_inset(r, {inset, inset, inset, inset}), theme.text_secondary);
            break;
        }
        case Role::glyph:
        case Role::clock_glyph:
        case Role::button_glyph:
            p.icon(item.icon, r, tone_color(theme, item.tone));
            break;
        case Role::bar: {
            const float fill = r.left + maximum(4.f, ui_rect_width(r) * maximum(0.f, minimum(1.f, c.fraction)));
            p.rounded(r, theme.tokens.border.subtle, theme.scale.radius_xs)
                .rounded({r.left, r.top, minimum(fill, r.right), r.bottom}, theme.text_primary, theme.scale.radius_xs);
            break;
        }
        case Role::button:
            p.rounded(r, theme.tokens.fill.muted, theme.scale.radius_sm);
            break;
        default:
            break;
        }
    }
    return p.end();
}
bool ui_activity_card_text(const UiActivityCard& c, UiPoint o, const Theme& theme, UiTextPen<>& pen,
                           UiWidgetHandle owner, const UiRect& clip) noexcept {
    auto line = pen.in(clip);
    for (u32 i = 0; pen.ok() && i < c.count; ++i) {
        const auto& item = c.items[i];
        switch (item.role) {
        case Role::heading:
        case Role::title:
        case Role::line:
        case Role::chip:
        case Role::clock:
        case Role::elapsed:
        case Role::total:
        case Role::button_label:
            line(owner, item.text, offset(item.rect, o), tone_color(theme, item.tone), item.flags,
                 (item.role == Role::total ? ui_text_end : 0u) | (item.wrap ? ui_text_wrap : 0u) |
                     (item.role == Role::button_label ? ui_text_middle : 0u));
            break;
        default:
            break;
        }
    }
    return pen.ok();
}
} // namespace maicord
