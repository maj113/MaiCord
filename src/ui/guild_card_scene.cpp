#include "maicord/ui/guild_card_scene.hpp"
#include "maicord/ui/background_color_math.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/spinner.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/text_style.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
constexpr float kW = kUiGuildCardWidth, kP = kUiGuildCardPadding, kRadius = 16, kSlide = 8;
constexpr float kIconTop = kUiGuildCardBanner - 34, kRing = 3;
constexpr float kNameHeight = 24, kLine = 18, kChip = 20, kTrait = 24, kButton = 32;
constexpr u32 kDescriptionLines = 5;
constexpr UiMotionStyle kEntranceStyle{kUiGuildCardEntranceMs, UiMotionCurve::spring};
constexpr u64 kEntranceOwner = 0x4743'4152'4400'0001ull;
constexpr u64 kMeasureSource = 0x4743415244ull; // "GCARD"

UiMotionScope scope(const UiGuildCardScene& s) noexcept {
    return {kEntranceOwner, s.entrance_generation};
}
u32 copy_text(char* destination, u32 capacity, Utf8View text) noexcept {
    return text.data ? utf8_copy_truncated(destination, capacity, text) : 0;
}
bool initialize(UiGuildCardScene& s) noexcept {
    return s.owner.manager == &s.widgets ||
           ui_widget_manager_initialize_fill_root(&s.widgets, {s.widget_storage, 1}, {s.layout_storage, 1},
                                                  utf8_literal("Server profile"),
                                                  ui_layout_node_visible | ui_layout_node_allow_overlap,
                                                  &s.owner);
}
// Measured size of text in a box width wide (wrapping unless single).
UiSize measure(UiTextMeasureCache* cache, Utf8View text, u32 style, float width, bool single, u16 item) noexcept {
    const float estimate = static_cast<float>(text.bytes) * (style & text_style_subtext ? 6.5f : 7.5f);
    UiSize size{single ? minimum(width, estimate) : width,
                single ? kLine : kLine * static_cast<float>(static_cast<u32>(estimate / maximum(width, 1.f)) + 1)};
    if (!cache || !text.bytes)
        return text.bytes ? size : UiSize{};
    UiTextMeasureValue value{};
    const TextStyleSpan span{0, text.bytes, style};
    if (ui_text_measure_cache_measure(
            cache, {{kMeasureSource, hash64_bytes(kHash64Seed, text.data, text.bytes), 0, static_cast<u16>(style), item},
                    text,
                    {&span, 1},
                    {width, 4096},
                    single},
            &value)
            .ok())
        size = value.size;
    return size;
}
UiRect offset(UiRect r, float dx, float dy) noexcept {
    return ui_rect_has_area(r) ? UiRect{r.left + dx, r.top + dy, r.right + dx, r.bottom + dy} : UiRect{};
}
float opacity(const UiGuildCardScene& s) noexcept {
    return s.progress < 0 ? 0.f : s.progress > 1 ? 1.f : s.progress;
}
Color packed(u32 rgb, float alpha = 1) noexcept {
    return {static_cast<float>((rgb >> 16) & 0xFF) / 255.f, static_cast<float>((rgb >> 8) & 0xFF) / 255.f,
            static_cast<float>(rgb & 0xFF) / 255.f, alpha};
}
// "Est. Mar 2021" from the guild snowflake (UTC month).
u32 established(u64 guild_id, char* output) noexcept {
    if (!guild_id)
        return 0;
    const u64 unix_ms = (guild_id >> 22) + 1'420'070'400'000ull;
    const u64 days = unix_ms / 86'400'000ull;
    // Civil-from-days (Howard Hinnant), proleptic Gregorian.
    const u64 z = days + 719'468;
    const u64 era = z / 146'097;
    const u64 doe = z - era * 146'097;
    const u64 yoe = (doe - doe / 1'460 + doe / 36'524 - doe / 146'096) / 365;
    const u64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const u64 mp = (5 * doy + 2) / 153;
    const u32 month = static_cast<u32>(mp < 10 ? mp + 3 : mp - 9);
    const u64 year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    static constexpr char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    u32 bytes = 0;
    copy_bytes(output, "Est. ", 5);
    bytes = 5;
    copy_bytes(output + bytes, months + (month - 1) * 3, 3);
    bytes += 3;
    output[bytes++] = ' ';
    char digits[8];
    u32 n = 0;
    u64 y = year;
    do {
        digits[n++] = static_cast<char>('0' + y % 10);
        y /= 10;
    } while (y && n < 8);
    while (n)
        output[bytes++] = digits[--n];
    return bytes;
}
// Settled card beside the anchor (the tag chip): right of it when it fits,
// else left; top-aligned with it and kept inside the viewport.
UiRect place(UiRect anchor, UiRect viewport, float height) noexcept {
    float left = anchor.right + kUiGuildCardGap;
    if (left + kW > viewport.right - kUiGuildCardGap)
        left = anchor.left - kUiGuildCardGap - kW;
    left = maximum(viewport.left + kUiGuildCardGap, minimum(left, viewport.right - kUiGuildCardGap - kW));
    float top = anchor.top - 8;
    top = minimum(top, viewport.bottom - kUiGuildCardGap - height);
    top = maximum(viewport.top + kUiGuildCardGap, top);
    return {left, top, left + kW, top + height};
}
// Panel-relative layout; returns the card height.
float layout_relative(UiGuildCardScene& s, UiTextMeasureCache* cache) noexcept {
    s.banner = {0, 0, kW, kUiGuildCardBanner};
    s.icon_ring = {kP - kRing, kIconTop - kRing, kP + kUiGuildCardIcon + kRing, kIconTop + kUiGuildCardIcon + kRing};
    s.icon = {kP, kIconTop, kP + kUiGuildCardIcon, kIconTop + kUiGuildCardIcon};
    s.name = s.tag_chip = s.tag_badge = s.tag_text = s.online_dot = s.online = s.members_dot = s.members = {};
    s.established = s.description = s.button = s.status_icon = s.status_title = s.status_text = {};
    for (auto& trait : s.traits)
        reset_value(&trait);
    const float content = kW - 2 * kP;
    float y = s.icon.bottom + 12;
    if (s.phase == UiGuildCardPhase::loading) {
        s.status_icon = {kW * .5f - 14, y + 8, kW * .5f + 14, y + 36};
        return y + 52;
    }
    if (s.phase != UiGuildCardPhase::ready) {
        s.status_title = {kP, y, kW - kP, y + kNameHeight};
        y += kNameHeight + 2;
        const auto text = s.phase == UiGuildCardPhase::private_profile
                              ? utf8_literal("The server has limited who can see this profile.")
                              : utf8_literal("Unable to load server profile. Click to try again.");
        const float height = minimum(measure(cache, text, text_style_subtext, content - 22, false, 9).height, kLine * 3);
        s.status_icon = {kP, y + 1, kP + 16, y + 17};
        s.status_text = {kP + 22, y, kW - kP, y + maximum(kLine, height)};
        return s.status_text.bottom + kP;
    }
    s.name = {kP, y, kW - kP, y + kNameHeight};
    y += kNameHeight + 2;
    if (s.tag_bytes) {
        const float text = measure(cache, {s.tag, s.tag_bytes}, text_style_subtext | text_style_bold, 160, true, 1).width;
        const float width = minimum(content, static_cast<float>(static_cast<u32>(text + .999f)) + 2 + 12 + 20);
        s.tag_chip = {kP, y + 1, kP + width, y + 1 + kChip};
        s.tag_badge = {kP + 6, y + 4, kP + 20, y + 18};
        s.tag_text = {kP + 26, y + 1, kP + width - 6, y + 1 + kChip};
        y += kChip + 6;
    }
    const float online = measure(cache, {s.online_text, s.online_bytes}, text_style_subtext, content, true, 2).width;
    s.online_dot = {kP, y + 5, kP + 8, y + 13};
    s.online = {kP + 12, y, minimum(kW - kP, kP + 14 + online), y + kLine};
    s.members_dot = {s.online.right + 8, y + 5, s.online.right + 16, y + 13};
    s.members = {s.members_dot.right + 4, y, kW - kP, y + kLine};
    y += kLine + 2;
    s.established = {kP, y, kW - kP, y + kLine};
    y += kLine;
    if (s.description_bytes || s.trait_count)
        y += 12;
    if (s.description_bytes) {
        const float height =
            minimum(measure(cache, {s.description_text, s.description_bytes}, text_style_subtext, content, false, 3).height,
                    kLine * kDescriptionLines);
        s.description = {kP, y, kW - kP, y + maximum(kLine, height)};
        y = s.description.bottom;
        if (s.trait_count)
            y += 12;
    }
    float x = kP;
    for (u32 i = 0; i < s.trait_count; ++i) {
        const float label = measure(cache, {s.trait_text[i], s.trait_bytes[i]}, text_style_subtext, content - 20, true,
                                    static_cast<u16>(4 + i))
                                .width;
        const float width = minimum(content, static_cast<float>(static_cast<u32>(label + .999f)) + 20);
        if (x > kP && x + width > kW - kP) {
            x = kP;
            y += kTrait + 4;
        }
        s.traits[i] = {x, y, x + width, y + kTrait};
        x += width + 4;
    }
    if (s.trait_count)
        y += kTrait;
    if (s.action != UiGuildCardAction::none) {
        y += kP;
        s.button = {kP, y, kW - kP, y + kButton};
        y += kButton;
    }
    return y + kP;
}
// Absolute rectangles for paint, text and hits: relative ones moved with the
// panel (which carries the entrance slide).
UiRect at(const UiGuildCardScene& s, UiRect r) noexcept {
    return offset(r, s.panel.left, s.panel.top);
}
void place_panel(UiGuildCardScene& s) noexcept {
    const float side = s.settled.left >= s.anchor.right ? -1.f : 1.f;
    const float dx = side * kSlide * (1 - opacity(s));
    s.panel = {s.settled.left + dx, s.settled.top, s.settled.right + dx, s.settled.bottom};
}
// The server name's initials stand in for a missing icon.
u32 acronym(const UiGuildCardScene& s, char* output) noexcept {
    u32 bytes = 0;
    bool start = true;
    for (u32 i = 0; i < s.name_bytes && bytes < 6; ++i) {
        const char c = s.name_text[i];
        if (c == ' ') {
            start = true;
            continue;
        }
        if (start && (static_cast<u8>(c) & 0x80) == 0)
            output[bytes++] = c;
        start = false;
    }
    return bytes;
}
bool push_text(UiTextOutput* text, const UiGuildCardScene& s, Utf8View value, UiRect rectangle, Color color, u32 style,
               UiTextAlignment horizontal = UiTextAlignment::start, bool wrap = false) noexcept {
    if (!value.bytes || !ui_rect_has_area(rectangle))
        return true;
    return ui_text_output_push(text, {value, s.owner, rectangle, s.panel, color, style, horizontal,
                                      wrap ? UiTextAlignment::start : UiTextAlignment::center,
                                      wrap ? UiTextWrapping::wrap : UiTextWrapping::no_wrap, UiTextOverflow::ellipsis});
}
Utf8View action_label(UiGuildCardAction action) noexcept {
    switch (action) {
    case UiGuildCardAction::go_to_server: return utf8_literal("Go to Server");
    case UiGuildCardAction::join: return utf8_literal("Join");
    case UiGuildCardAction::joining: return utf8_literal("Joining\xE2\x80\xA6");
    default: return {};
    }
}
} // namespace

u32 ui_guild_card_format_count(u32 value, char* output, u32 capacity) noexcept {
    char digits[16];
    u32 n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    u32 bytes = 0;
    while (n && bytes < capacity) {
        output[bytes++] = digits[--n];
        if (n && n % 3 == 0 && bytes < capacity)
            output[bytes++] = ',';
    }
    return bytes;
}

bool ui_guild_card_show(UiGuildCardScene* s, u64 identity, const UiGuildCardContent& content, UiRect anchor,
                        UiRect viewport, u64 now_ms, bool reduced_motion, UiTextMeasureCache* cache) noexcept {
    if (!s || !identity || !ui_rect_valid(viewport) || !ui_rect_has_area(viewport) || !initialize(*s)) {
        if (s && s->active)
            ui_guild_card_close(s);
        return false;
    }
    const bool fresh = !s->active || s->identity != identity;
    s->identity = identity;
    s->anchor = ui_rect_has_area(anchor) ? anchor : UiRect{viewport.left, viewport.top, viewport.left + 1, viewport.top + 1};
    s->viewport = viewport;
    s->phase = content.phase;
    s->action = content.phase == UiGuildCardPhase::ready ? content.action : UiGuildCardAction::none;
    s->name_bytes = copy_text(s->name_text, kUiGuildCardNameBytes, content.name);
    s->description_bytes = copy_text(s->description_text, kUiGuildCardDescriptionBytes, content.description);
    s->tag_bytes = copy_text(s->tag, kUiGuildCardTagBytes, content.tag);
    s->trait_count = minimum(content.trait_count, kUiGuildCardTraits);
    for (u32 i = 0; i < s->trait_count; ++i)
        s->trait_bytes[i] = copy_text(s->trait_text[i], kUiGuildCardTraitBytes, content.traits[i]);
    u32 bytes = ui_guild_card_format_count(content.online_count, s->online_text, 20);
    copy_bytes(s->online_text + bytes, " Online", 7);
    s->online_bytes = bytes + 7;
    bytes = ui_guild_card_format_count(content.member_count, s->members_text, 20);
    copy_bytes(s->members_text + bytes, " Members", 8);
    s->members_bytes = bytes + 8;
    s->established_bytes = established(content.guild_id, s->established_text);
    s->brand_color = content.brand_color;
    s->has_brand_color = content.has_brand_color;
    s->icon_key = content.icon;
    s->badge_key = content.badge;
    s->banner_key = content.phase == UiGuildCardPhase::ready ? content.banner : MediaAssetKey{};
    const float height = layout_relative(*s, cache);
    s->settled = place(s->anchor, viewport, height);
    if (fresh) {
        s->hovered = s->pressed = false;
        ++s->entrance_generation;
        ui_motion_transition_reset(&s->entrance, scope(*s), reduced_motion ? 1.f : 0.f);
        ui_motion_transition_tick(&s->entrance, scope(*s), 1, now_ms, kEntranceStyle, reduced_motion, true);
    }
    s->progress = s->entrance.value;
    place_panel(*s);
    if (!ui_widget_manager_layout_rectangle(&s->widgets, viewport).ok()) {
        ui_guild_card_close(s);
        return false;
    }
    u64 hash = hash64_mix(kHash64Seed, identity);
    hash = hash64_counted_bytes(hash, s->name_text, s->name_bytes);
    hash = hash64_counted_bytes(hash, s->description_text, s->description_bytes);
    hash = hash64_counted_bytes(hash, s->tag, s->tag_bytes);
    hash = hash64_counted_bytes(hash, s->online_text, s->online_bytes);
    hash = hash64_counted_bytes(hash, s->members_text, s->members_bytes);
    for (u32 i = 0; i < s->trait_count; ++i)
        hash = hash64_counted_bytes(hash, s->trait_text[i], s->trait_bytes[i]);
    hash = hash64_bytes(hash, &s->icon_key, sizeof(s->icon_key));
    hash = hash64_bytes(hash, &s->badge_key, sizeof(s->badge_key));
    hash = hash64_bytes(hash, &s->banner_key, sizeof(s->banner_key));
    hash = hash64_bytes(hash, &s->panel, sizeof(s->panel));
    hash = hash64_mix(hash, (u64{s->brand_color} << 16) | (u64{static_cast<u8>(s->phase)} << 8) |
                                (u64{static_cast<u8>(s->action)} << 1) | (s->has_brand_color ? 1u : 0u));
    if (!s->active || hash != s->content_hash) {
        s->content_hash = hash;
        ++s->revision;
    }
    s->active = true;
    return true;
}

void ui_guild_card_close(UiGuildCardScene* s) noexcept {
    if (!s || !s->active)
        return;
    s->active = s->hovered = s->pressed = false;
    s->identity = 0;
    ++s->entrance_generation;
    ++s->revision;
}

bool ui_guild_card_tick(UiGuildCardScene* s, u64 now_ms, bool reduced_motion) noexcept {
    if (!s || !s->active || (!s->entrance.active && s->entrance.value >= 1))
        return false;
    if (!ui_motion_transition_tick(&s->entrance, scope(*s), 1, now_ms, kEntranceStyle, reduced_motion, true))
        return false;
    s->progress = s->entrance.value;
    place_panel(*s);
    ++s->revision;
    return true;
}

u64 ui_guild_card_next_wake(const UiGuildCardScene& s, u64 now_ms, bool reduced_motion) noexcept {
    return s.active ? ui_motion_transition_next_wake(s.entrance, scope(s), now_ms, reduced_motion, true) : 0;
}

bool ui_guild_card_contains(const UiGuildCardScene& s, float x, float y) noexcept {
    return s.active && (ui_rect_contains(s.settled, x, y) || ui_rect_contains(s.panel, x, y));
}

UiGuildCardControl ui_guild_card_hit(const UiGuildCardScene& s, float x, float y) noexcept {
    if (!s.active)
        return UiGuildCardControl::none;
    if (s.action == UiGuildCardAction::go_to_server || s.action == UiGuildCardAction::join)
        if (ui_rect_contains(at(s, s.button), x, y))
            return UiGuildCardControl::action;
    if (s.phase == UiGuildCardPhase::failed && ui_rect_contains(s.panel, x, y))
        return UiGuildCardControl::retry;
    return UiGuildCardControl::none;
}

bool ui_guild_card_set_hover(UiGuildCardScene* s, float x, float y) noexcept {
    if (!s)
        return false;
    const bool hovered = ui_guild_card_hit(*s, x, y) == UiGuildCardControl::action;
    if (hovered == s->hovered)
        return false;
    s->hovered = hovered;
    ++s->revision;
    return true;
}

bool ui_guild_card_set_pressed(UiGuildCardScene* s, bool pressed) noexcept {
    if (!s || s->pressed == pressed)
        return false;
    s->pressed = pressed;
    ++s->revision;
    return true;
}

bool ui_guild_card_spinner_visible(const UiGuildCardScene& s) noexcept {
    return s.active && s.phase == UiGuildCardPhase::loading;
}

bool ui_guild_card_media(const UiGuildCardScene& s, u32 index, UiGuildCardMedia* output) noexcept {
    if (!output || !s.active)
        return false;
    if (index == 0 && media_asset_key_valid(s.icon_key) && s.phase != UiGuildCardPhase::loading) {
        const auto r = at(s, s.icon);
        *output = {s.icon_key, r, s.panel, 18, opacity(s)};
        return true;
    }
    if (index == 1 && media_asset_key_valid(s.badge_key) && ui_rect_has_area(s.tag_badge)) {
        const auto r = at(s, s.tag_badge);
        *output = {s.badge_key, r, s.panel, 0, opacity(s)};
        return true;
    }
    if (index == kUiGuildCardBannerMedia && media_asset_key_valid(s.banner_key)) {
        // Centred cover fit; the card rounds its top corners.
        const auto r = at(s, s.banner);
        *output = {s.banner_key, r, ui_rect_intersection(r, s.panel), 0, opacity(s),
                   {r.left, r.top, r.right, r.bottom + kRadius}, kRadius, true};
        return true;
    }
    return false;
}

Color ui_guild_card_banner_highlight(Color base) noexcept {
    // chroma.brighten(1.75): CIELAB L* + 31.5, about +0.27 Oklab lightness.
    auto lab = background_color_oklab(base);
    lab.lightness = lab.lightness + .27 < 1 ? lab.lightness + .27 : 1.;
    return background_color_from_oklab(lab, base.alpha);
}

bool ui_guild_card_paint(const UiGuildCardScene& s, const Theme& theme, PaintStream* paint) noexcept {
    if (!s.active)
        return true;
    if (!paint)
        return false;
    const float a = opacity(s);
    const Color fill = ui_theme_color_alpha(theme.tokens.background.floating, a);
    const Color border = ui_theme_color_alpha(theme.tokens.border.subtle, a);
    bool ok = ui_surface_paint(paint, s.panel, theme, UiElevation::popover, kRadius, &fill, &border);
    if (!ok || !paint_stream_push_clip(paint, s.panel.left, s.panel.top, s.panel.right, s.panel.bottom))
        return false;
    // Banner: the brand colour (else the accent) brightening towards the
    // bottom centre, its top corners rounded with the card and its bottom
    // edge square. A custom banner picture (media) covers it.
    const auto banner = at(s, s.banner);
    const Color top = s.has_brand_color ? packed(s.brand_color, a) : ui_theme_color_alpha(theme.tokens.accent.brand, a);
    const Color bottom = ui_guild_card_banner_highlight(top);
    ok = paint_stream_push_clip(paint, banner.left, banner.top, banner.right, banner.bottom) &&
         paint_stream_push_gradient(paint, {banner.left, banner.top, banner.right, banner.bottom + kRadius}, top, bottom,
                                    kRadius) &&
         paint_stream_pop_clip(paint);
    return paint_stream_pop_clip(paint) && ok;
}

bool ui_guild_card_paint_foreground(const UiGuildCardScene& s, const Theme& theme, PaintStream* paint) noexcept {
    if (!s.active)
        return true;
    if (!paint)
        return false;
    const float a = opacity(s);
    const Color fill = ui_theme_color_alpha(theme.tokens.background.floating, a);
    bool ok = paint_stream_push_clip(paint, s.panel.left, s.panel.top, s.panel.right, s.panel.bottom);
    if (!ok)
        return false;
    // Icon: a ring in the card colour, then the plate the host's icon covers.
    const auto ring = at(s, s.icon_ring), icon = at(s, s.icon);
    ok = ok && paint_stream_push_rounded(paint, ring.left, ring.top, ring.right, ring.bottom, fill, theme.scale.radius_xl + kRing) &&
         paint_stream_push_rounded(paint, icon.left, icon.top, icon.right, icon.bottom,
                                   ui_theme_color_alpha(theme.surface_high, a), theme.scale.radius_xl);
    if (ok && s.phase == UiGuildCardPhase::loading) {
        const auto spinner = at(s, s.status_icon);
        ok = ui_spinner_paint(paint, spinner, ui_theme_color_alpha(theme.tokens.text.normal, a), s.spinner_phase);
    }
    if (ok && (s.phase == UiGuildCardPhase::private_profile || s.phase == UiGuildCardPhase::failed))
        ok = ui_icon_paint(paint, s.phase == UiGuildCardPhase::private_profile ? UiIcon::lock : UiIcon::warning,
                           at(s, s.status_icon), ui_theme_color_alpha(theme.tokens.text.muted, a));
    if (ok && ui_rect_has_area(s.tag_chip)) {
        const auto chip = at(s, s.tag_chip);
        ok = paint_stream_push_rounded_border(paint, chip.left, chip.top, chip.right, chip.bottom, Color{},
                                              ui_theme_color_alpha(theme.tokens.border.strong, a), theme.scale.radius_md, 1);
    }
    if (ok && ui_rect_has_area(s.online_dot)) {
        const auto on = at(s, s.online_dot), off = at(s, s.members_dot);
        ok = paint_stream_push_rounded(paint, on.left, on.top, on.right, on.bottom, ui_theme_color_alpha(theme.tokens.status.online, a),
                                       theme.scale.radius_full) &&
             paint_stream_push_rounded(paint, off.left, off.top, off.right, off.bottom,
                                       ui_theme_color_alpha(theme.tokens.status.offline, a), theme.scale.radius_full);
    }
    for (u32 i = 0; ok && i < s.trait_count; ++i) {
        const auto r = at(s, s.traits[i]);
        ok = paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, ui_theme_color_alpha(theme.surface_high, a), theme.scale.radius_md);
    }
    if (ok && ui_rect_has_area(s.button)) {
        Color button = theme.tokens.accent.brand;
        const float shade = s.pressed && s.hovered ? .8f : s.hovered ? .9f : 1.f;
        button.red *= shade;
        button.green *= shade;
        button.blue *= shade;
        const auto b = at(s, s.button);
        ok = paint_stream_push_rounded(paint, b.left, b.top, b.right, b.bottom,
                                       ui_theme_color_alpha(button, s.action == UiGuildCardAction::joining ? a * .5f : a), theme.scale.radius_md);
    }
    return paint_stream_pop_clip(paint) && ok;
}

bool ui_guild_card_text_output(const UiGuildCardScene& s, const Theme& theme, UiTextOutput* text) noexcept {
    if (!s.active)
        return true;
    if (!text)
        return false;
    const float a = opacity(s);
    const Color strong = ui_theme_color_alpha(theme.tokens.text.normal, a), muted = ui_theme_color_alpha(theme.tokens.text.muted, a);
    bool ok = true;
    if (!media_asset_key_valid(s.icon_key) && s.phase == UiGuildCardPhase::ready) {
        char initials[8];
        const u32 bytes = acronym(s, initials);
        ok = !bytes || ui_text_output_push_owned(text, {{initials, bytes}, s.owner, at(s, s.icon), s.panel, strong,
                                                        text_style_bold, UiTextAlignment::center,
                                                        UiTextAlignment::center, UiTextWrapping::no_wrap,
                                                        UiTextOverflow::ellipsis});
    }
    if (s.phase == UiGuildCardPhase::private_profile || s.phase == UiGuildCardPhase::failed) {
        const bool closed = s.phase == UiGuildCardPhase::private_profile;
        return ok &&
               push_text(text, s, closed ? utf8_literal("Private Server") : utf8_literal("Unknown Server"),
                         at(s, s.status_title), strong, text_style_bold) &&
               push_text(text, s,
                         closed ? utf8_literal("The server has limited who can see this profile.")
                                : utf8_literal("Unable to load server profile. Click to try again."),
                         at(s, s.status_text), muted, text_style_subtext, UiTextAlignment::start, true);
    }
    if (s.phase != UiGuildCardPhase::ready)
        return ok;
    ok = ok && push_text(text, s, {s.name_text, s.name_bytes}, at(s, s.name), strong, text_style_bold) &&
         push_text(text, s, {s.tag, s.tag_bytes}, at(s, s.tag_text), strong, text_style_subtext | text_style_bold) &&
         push_text(text, s, {s.online_text, s.online_bytes}, at(s, s.online), muted, text_style_subtext) &&
         push_text(text, s, {s.members_text, s.members_bytes}, at(s, s.members), muted, text_style_subtext) &&
         push_text(text, s, {s.established_text, s.established_bytes}, at(s, s.established), muted,
                   text_style_subtext) &&
         push_text(text, s, {s.description_text, s.description_bytes}, at(s, s.description), strong,
                   text_style_subtext, UiTextAlignment::start, true);
    for (u32 i = 0; ok && i < s.trait_count; ++i) {
        const auto r = at(s, s.traits[i]);
        ok = push_text(text, s, {s.trait_text[i], s.trait_bytes[i]}, {r.left + 10, r.top, r.right - 10, r.bottom},
                       strong, text_style_subtext);
    }
    if (ok && ui_rect_has_area(s.button))
        ok = push_text(text, s, action_label(s.action), at(s, s.button), ui_theme_color_alpha(theme.tokens.text.on_accent, a),
                       text_style_bold, UiTextAlignment::center);
    return ok;
}
} // namespace maicord
