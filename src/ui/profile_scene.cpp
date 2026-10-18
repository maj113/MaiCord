#include "maicord/ui/timestamp_format.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/profile_scene.hpp"
#include "maicord/ui/profile_activity.hpp"
#include "maicord/ui/badge.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/plugin_slots.hpp"
#include "maicord/model/role_policy.hpp"
#include "maicord/ui/profile_appearance.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/transition.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/ui/presence.hpp"
#include "maicord/ui/scroll_host.hpp"
#include <new>
namespace maicord {
namespace {
constexpr u32 capacity = 320;
// Text variants as size codes (1rem = the body size).
constexpr u32 kXs = kTextStyleSizeCodeXs, kSm = kTextStyleSizeCodeSm, kMd = kTextStyleSizeCodeMd,
              kLg = text_style_heading_2, kXl = text_style_heading_1;
constexpr float kPopoutWidth = 300, kPopoutBanner = 105, kModalBanner = 140;
constexpr u16 kNoPart = 0xFFFF;
// A card's changing parts (indices into parts): the timer chip's label and
// glyph, the progress bar's ends and track.
struct ActivityTimer {
    u16 clock, glyph, elapsed, total, bar;
    u8 item, card;
};
struct Storage {
    UiWidget widgets[capacity * 2];
    UiLayoutNode nodes[capacity * 2];
    ProfilePart parts[capacity];
    // Owned labels; other parts borrow profile text.
    ProfileString<64> mutual_text[2], tab_text[2], roles_more, member_since[2];
    ProfileString<64> connection_line[16];
    char connection_initial[16][2];
    // The laid-out activities and their cards' text (the store changes under
    // them): per card the timer chip and the progress bar's ends.
    UserActivitySet activity_set;
    ProfileActivityContent activity_content[kUserActivityMaximum];
    ProfileString<16> activity_time[kUserActivityMaximum][3];
    ActivityTimer timers[kUserActivityMaximum];
    u32 timer_count;
    // A tick's time while it lays the cards out again (0: the clock).
    u64 tick_ms;
};
static_assert(sizeof(Storage::connection_line) / sizeof(ProfileString<64>) ==
              sizeof(UserProfile::connections) / sizeof(ProfileConnection));
Storage& labels(ProfileScene* s) noexcept {
    return *reinterpret_cast<Storage*>(s->storage.base);
}
Color packed(u32 color, float alpha = 1) noexcept {
    return {((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, alpha};
}
Color over(Color top, Color bottom) noexcept {
    const float a = top.alpha;
    return {top.red * a + bottom.red * (1 - a), top.green * a + bottom.green * (1 - a),
            top.blue * a + bottom.blue * (1 - a), 1};
}
Color mix(Color a, Color b, float t) noexcept {
    return {a.red + (b.red - a.red) * t, a.green + (b.green - a.green) * t, a.blue + (b.blue - a.blue) * t,
            a.alpha + (b.alpha - a.alpha) * t};
}
bool interactive(const ProfileScene& s) noexcept {
    return s.presentation != ProfilePresentation::preview;
}
bool modal(const ProfileScene& s) noexcept {
    return s.presentation == ProfilePresentation::modal;
}
// Resolved colours of one card: the profile's own theme colours when it has
// one, else the application theme.
struct Palette {
    Color primary, secondary, overlay, surface, banner, modal;
    Color text, strong, subtle, muted, border, hover, card, card_hover, input, button, button_hover;
    Color accent, accent_hover, on_accent, bubble, bubble_border, control, control_border, control_icon;
};
Palette palette(const ProfileScene& s, const Theme& theme) noexcept {
    Palette c{};
    const auto& own = theme.tokens.profile;
    c.control = own.control;
    c.control_border = own.control_border;
    c.control_icon = own.control_icon;
    c.banner = s.has_banner_color ? packed(s.banner_color) : own.banner;
    if (!s.themed) {
        c.surface = c.primary = c.secondary = own.surface;
        c.modal = own.modal;
        c.text = c.strong = theme.text_primary;
        c.subtle = theme.text_secondary;
        c.muted = theme.tokens.text.muted;
        c.border = theme.tokens.border.subtle;
        c.hover = theme.tokens.state.hover;
        c.card = theme.tokens.fill.subtle;
        c.card_hover = theme.tokens.fill.muted;
        c.input = theme.tokens.fill.muted;
        c.button = theme.tokens.fill.muted;
        c.button_hover = theme.tokens.state.pressed;
        c.accent = theme.tokens.emphasis.fill;
        c.accent_hover = mix(c.accent, theme.tokens.overlay.dense, .1f);
        c.on_accent = theme.tokens.emphasis.text;
        c.bubble = theme.surface;
        c.bubble_border = theme.tokens.border.subtle;
        return c;
    }
    // The profile's own theme column: the application theme when it has
    // the same appearance, else the built-in theme of that appearance.
    const bool app_dark = theme.appearance == ThemeAppearance::dark;
    const auto& tokens = (app_dark == s.theme_dark ? theme : s.theme_dark ? kDefaultTheme : kLightTheme).tokens.profile;
    c.primary = packed(s.theme_primary);
    c.secondary = packed(s.theme_secondary);
    c.overlay = tokens.overlay;
    c.surface = over(c.overlay, c.primary);
    c.modal = packed(profile_modal_background(s.theme_primary, s.theme_secondary, s.theme_dark));
    c.text = c.strong = tokens.text;
    c.subtle = tokens.text_subtle;
    c.muted = tokens.text_muted;
    c.border = tokens.border;
    c.hover = tokens.hover;
    c.card = tokens.card;
    c.card_hover = tokens.card_hover;
    c.input = tokens.input;
    c.button = tokens.button;
    c.button_hover = tokens.button_hover;
    c.accent = packed(profile_button_color(s.theme_primary));
    // Hover: the accent mixed 10% toward the darkest (dark theme) or lightest surface.
    c.accent_hover = mix(c.accent, s.theme_dark ? kDefaultTheme.base_lowest : kLightTheme.surface, .1f);
    c.accent_hover.alpha = 1;
    c.on_accent = theme.tokens.text.on_accent;
    c.bubble = over(tokens.card, c.surface);
    c.bubble_border = s.theme_dark ? theme.tokens.border.strong : tokens.border;
    return c;
}
// The application theme with the card's text colours, for shared widgets
// (edit field, badges) drawn inside a themed card.
Theme card_theme(const Theme& theme, const Palette& c) noexcept {
    Theme local = theme;
    local.text_primary = local.tokens.text.normal = c.text;
    local.text_secondary = local.tokens.text.muted = c.muted;
    local.tokens.border.strong = c.border;
    return local;
}
Color tone_color(const Palette& c, const Theme& theme, const ProfilePart& p) noexcept {
    switch (p.tone) {
    case ProfileTone::strong:
        return c.strong;
    case ProfileTone::subtle:
        return c.subtle;
    case ProfileTone::muted:
        return c.muted;
    case ProfileTone::positive:
        return theme.tokens.text.positive;
    case ProfileTone::link:
        return theme.tokens.text.link;
    case ProfileTone::on_accent:
        return c.on_accent;
    case ProfileTone::custom:
        return p.color;
    default:
        return c.text;
    }
}
const ProfilePart* inline_text_part(const ProfileScene& scene, UiWidgetHandle handle) noexcept {
    const auto* part = profile_scene_part(scene, handle);
    return part && part->text.bytes && part->action != ProfileAction::none && part->look == ProfileLook::plain &&
                   !part->wrap
               ? part
               : nullptr;
}
UiSize measure(ProfileScene* s, Utf8View text, float width, bool wrap, u32 flags = 0) noexcept {
    if (!text.bytes)
        return {};
    UiTextMeasureValue result{};
    const TextStyleSpan span{0, text.bytes, flags};
    const u64 hash = hash64_bytes(kHash64Seed, text.data, text.bytes);
    if (s->measure && ui_text_measure_cache_measure(
                          s->measure,
                          {{0x50524f46, hash, 0, static_cast<u16>(flags), 0},
                           text,
                           flags ? ConstSlice<TextStyleSpan>{&span, 1} : ConstSlice<TextStyleSpan>{},
                           {width, 4096},
                           !wrap},
                          &result)
                          .ok())
        return result.size;
    const float em = (flags & kTextStyleHeadingMask) == kXs ? 6.f : 7.f;
    const float line = (flags & kTextStyleHeadingMask) == kXs ? 16.f : 18.f;
    const float natural = text.bytes * em;
    if (!wrap || natural <= width)
        return {minimum(width, natural), line};
    return {width, line * (1 + static_cast<u32>(natural / maximum(1.f, width)))};
}
float text_width(ProfileScene* s, Utf8View text, float limit, u32 flags) noexcept {
    return minimum(limit, static_cast<float>(static_cast<u32>(measure(s, text, 4096, false, flags).width + .999f)));
}
bool biography_revealed(void* context, u32 offset) noexcept {
    const auto& s = *static_cast<ProfileScene*>(context);
    for (u32 i = 0; i < s.bio_revealed_count; ++i)
        if (s.bio_revealed[i] == offset)
            return true;
    return false;
}
// Bios read as text-sm: every byte gets a span with that size, keeping the
// markdown styles and filling the gaps between them.
void size_biography(ProfileScene* s, const TextStyleSpan* styles, u32 count) noexcept {
    u32 used = 0, cursor = 0;
    const u32 bytes = s->bio_display.bytes;
    const auto push = [&](u32 offset, u32 length, u32 flags) noexcept {
        if (length && used < sizeof(s->bio_styles) / sizeof(s->bio_styles[0]))
            s->bio_styles[used++] = {offset, length, text_style_fold_size(flags, text_size_sm), 0, {}};
    };
    for (u32 i = 0; i < count; ++i) {
        const auto& span = styles[i];
        if (span.byte_offset < cursor || span.byte_offset + span.byte_count > bytes)
            continue;
        push(cursor, span.byte_offset - cursor, 0);
        push(span.byte_offset, span.byte_count, span.flags);
        cursor = span.byte_offset + span.byte_count;
    }
    push(cursor, bytes - cursor, 0);
    s->bio_style_count = used;
}
bool prepare_biography(ProfileScene* s, Utf8View source) noexcept {
    u64 signature = hash64_bytes(kHash64Seed, source.data, source.bytes);
    signature = hash64_bytes(signature, s->bio_revealed, s->bio_revealed_count * sizeof(u32));
    if (s->bio_signature == signature)
        return true;
    u32 scratch[1024]{};
    TextStyleSpan styles[128];
    MessageMarkdownOptions options{};
    options.context = s;
    options.profile_bio = true;
    options.spoiler_revealed = biography_revealed;
    const auto result = message_markdown_parse(source, {s->bio_display.data, 1024}, {scratch, 1024},
                                               {styles, 128}, options, {s->bio_semantics, 64});
    if (!result.ok()) {
        copy_bytes(s->bio_display.data, source.data, source.bytes);
        s->bio_display.bytes = static_cast<u16>(source.bytes);
        s->bio_semantic_count = 0;
        size_biography(s, nullptr, 0);
        s->bio_signature = signature;
        return true;
    }
    s->bio_display.bytes = static_cast<u16>(result.text.bytes);
    s->bio_semantic_count = result.semantic_count;
    size_biography(s, styles, result.span_count);
    s->bio_signature = signature;
    return true;
}
ProfilePart* add(ProfileScene* s, UiWidgetHandle parent, UiRect r, Utf8View label = {},
                 ProfileAction action = ProfileAction::none, u32 flags = 0,
                 ProfileLook look = ProfileLook::plain, ProfileTone tone = ProfileTone::normal,
                 bool clip = false) noexcept {
    if (s->part_count + 1 >= capacity)
        return nullptr;
    if (!ui_rect_has_area(r)) {
        // Nothing to show (an empty label): callers may still decorate it.
        static ProfilePart empty;
        empty = {};
        return &empty;
    }
    // The editor preview is a picture of the card: nothing in it is a control.
    if (!interactive(*s))
        action = ProfileAction::none;
    auto style = ui_layout_style();
    style.flow = UiLayoutFlow::overlay;
    style.width = ui_layout_fixed(ui_rect_width(r));
    style.height = ui_layout_fixed(ui_rect_height(r));
    style.margin.left = r.left;
    style.margin.top = r.top;
    style.horizontal_alignment = UiLayoutAlignment::start;
    style.vertical_alignment = UiLayoutAlignment::start;
    style.flags = ui_layout_node_visible | ui_layout_node_allow_overlap |
                  (clip ? ui_layout_node_clip_children : 0) |
                  (label.bytes ? ui_layout_node_text_content : 0);
    const u32 id = 2 + s->part_count;
    const auto widget = ui_widget_manager_add(
        &s->widgets, parent,
        {id,
         clip                            ? UiWidgetKind::container
         : action == ProfileAction::none ? UiWidgetKind::text
                                         : UiWidgetKind::button,
         static_cast<u16>(ui_widget_enabled |
                          (action == ProfileAction::none ? 0 : ui_widget_hit_test | ui_widget_focusable)),
         {utf8_literal("ProfilePart"), style, {}, 0, id}});
    if (!widget.ok())
        return nullptr;
    auto& part = s->parts[s->part_count++];
    new (&part) ProfilePart{};
    part.widget = widget.handle;
    part.text = label;
    part.action = action;
    part.style = flags;
    part.look = look;
    part.tone = tone;
    part.icon = kProfileNoIcon;
    return &part;
}
ProfilePart* icon(ProfileScene* s, UiWidgetHandle parent, UiRect r, UiIcon glyph, ProfileAction action,
                  ProfileTone tone = ProfileTone::subtle, ProfileLook look = ProfileLook::icon) noexcept {
    auto* part = add(s, parent, r, {}, action, 0, look, tone);
    if (part)
        part->icon = static_cast<u16>(glyph);
    return part;
}
bool place(ProfileScene* s, UiWidgetHandle handle, UiRect r) noexcept {
    auto* widget = ui_widget_manager_resolve(&s->widgets, handle);
    auto* node = widget ? ui_layout_engine_resolve(&s->widgets.layout, widget->layout) : nullptr;
    if (!node || !ui_rect_has_area(r))
        return false;
    node->spec.style.width = ui_layout_fixed(ui_rect_width(r));
    node->spec.style.height = ui_layout_fixed(ui_rect_height(r));
    node->spec.style.margin.left = r.left;
    node->spec.style.margin.top = r.top;
    return true;
}
bool scroll_container(ProfileScene* scene, UiWidgetHandle handle, float offset) noexcept {
    auto* widget = ui_widget_manager_resolve(&scene->widgets, handle);
    auto* node = widget ? ui_layout_engine_resolve(&scene->widgets.layout, widget->layout) : nullptr;
    if (!node)
        return false;
    node->spec.style.flags |= ui_layout_node_scroll_vertical;
    return ui_layout_engine_set_scroll(&scene->widgets.layout, widget->layout, {0, offset});
}
void copy_label(ProfileString<128>* out, Utf8View text) noexcept {
    const u32 bytes = utf8_truncated_length(text, 128);
    copy_bytes(out->data, text.data, bytes);
    out->bytes = static_cast<u16>(bytes);
}
template <u32 N> bool append(ProfileString<N>* s, Utf8View text) noexcept {
    const u32 bytes = utf8_truncated_length(text, N - s->bytes);
    copy_bytes(s->data + s->bytes, text.data, bytes);
    s->bytes += static_cast<u16>(bytes);
    return bytes == text.bytes;
}
template <u32 N> bool append_number(ProfileString<N>* s, u64 n) noexcept {
    if (s->bytes + decimal_digits(n) > N)
        return false;
    s->bytes += static_cast<u16>(decimal_write(s->data + s->bytes, n));
    return true;
}
template <u32 N> bool append_date(ProfileString<N>* out, u64 unix_seconds) noexcept {
    char text[96];
    const auto formatted = format_profile_date(unix_seconds, {text, 96});
    return formatted.ok && append(out, {text, formatted.bytes});
}
const UserAppearanceRecord* appearance(const ProfileScene& s, u32 field) noexcept {
    const auto& p = *s.profile;
    const auto* guild = s.users ? user_appearance_find(s.users->appearance, p.user_id, p.guild_id) : nullptr;
    if (guild && (guild->fields & field) && (field != appearance_name || guild->name.font_id || guild->name.effect_id))
        return guild;
    return s.users ? user_appearance_find(s.users->appearance, p.user_id) : nullptr;
}
bool bot(const ProfileScene& s) noexcept {
    return s.profile->user_appearance.bot;
}
bool self(const ProfileScene& s) noexcept {
    return s.profile->user_id == s.self_id;
}

u64 activity_now(ProfileScene& s) noexcept {
    const u64 tick = labels(&s).tick_ms;
    return tick ? tick : clock_unix_milliseconds();
}
// Rich presence cards: a header line, then the artwork (the small image on its
// corner) beside the title, text lines, chips (party, timer, episode) and the
// progress bar. Buttons follow the card's body in the popout and the text in
// the full profile, whose cards use the larger metrics.
struct CardMetrics {
    float pad, image, small, gap, ring;
};
CardMetrics card_metrics(bool large) noexcept {
    return large ? CardMetrics{12, 100, 32, 12, 4} : CardMetrics{8, 60, 24, 8, 2};
}
constexpr float kCardTitle = 18, kCardLine = 16;
void unfocusable(ProfileScene* s, const ProfilePart* part) noexcept {
    if (auto* widget = part ? ui_widget_manager_resolve(&s->widgets, part->widget) : nullptr)
        widget->flags = static_cast<u16>(widget->flags & ~ui_widget_focusable);
}
// The action a part takes only to show its hint: the popout card's own click,
// or none in the full profile.
ProfileAction hint_action(const ProfileScene& s) noexcept {
    return modal(s) ? ProfileAction::hint : ProfileAction::card;
}
u16 part_index(const ProfileScene& s, const ProfilePart* part) noexcept {
    return part && part >= s.parts && part < s.parts + s.part_count ? static_cast<u16>(part - s.parts) : kNoPart;
}
// One text line. Popout lines stay on one line and a cut-off line carries its
// full text as a hint; full profile lines wrap. Returns its height, -1 on failure.
float card_text(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, Utf8View text, u32 flags,
                float line, u16 item) noexcept {
    if (!text.bytes)
        return 0;
    const bool wrap = modal(*s);
    const float height = wrap ? maximum(line, measure(s, text, width, true, flags).height) : line;
    const bool cut = !wrap && measure(s, text, 4096, false, flags).width > width;
    auto* part = add(s, owner, {left, top, left + width, top + height}, text,
                     cut ? hint_action(*s) : ProfileAction::none, flags, ProfileLook::plain, ProfileTone::normal);
    if (!part)
        return -1;
    part->wrap = height > line;
    part->media = ProfileMediaKind::activity_text;
    part->source = item;
    if (cut) {
        part->hint = text;
        unfocusable(s, part);
    }
    return height;
}
UiIcon chip_icon(ProfileActivityGlyph glyph) noexcept {
    switch (glyph) {
    case ProfileActivityGlyph::headphones:
        return UiIcon::headphones;
    case ProfileActivityGlyph::screen:
        return UiIcon::monitor;
    case ProfileActivityGlyph::timer:
        return UiIcon::timer;
    case ProfileActivityGlyph::party:
        return UiIcon::members;
    case ProfileActivityGlyph::episode:
        return UiIcon::video;
    default:
        return UiIcon::activity;
    }
}
// The width a changing time string needs at its length (digits as 8s).
float clock_width(ProfileScene* s, Utf8View text, u32 flags) noexcept {
    char widest[16];
    const u32 bytes = minimum<u32>(text.bytes, sizeof(widest));
    for (u32 i = 0; i < bytes; ++i)
        widest[i] = text.data[i] >= '0' && text.data[i] <= '9' ? '8' : text.data[i];
    return text_width(s, {widest, bytes}, 4096, flags) + 1;
}
void shift(ProfileScene* s, u32 first, float dy) noexcept {
    for (u32 i = first; dy != 0 && i < s->part_count; ++i) {
        auto* widget = ui_widget_manager_resolve(&s->widgets, s->parts[i].widget);
        auto* node = widget ? ui_layout_engine_resolve(&s->widgets.layout, widget->layout) : nullptr;
        if (node)
            node->spec.style.margin.top += dy;
    }
}
bool application_icon(const ProfileScene& s, u64 application, bool* known) noexcept {
    *known = false;
    for (u32 i = 0; i < s.applications.count; ++i)
        if (s.applications[i].id == application) {
            *known = true;
            return s.applications[i].icon.bytes != 0;
        }
    return false;
}
// A card's button: full width in the popout, its natural width (at most the
// column) in the full profile. Returns its width, -1 on failure.
float card_button(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, bool full, Utf8View label,
                  UiIcon glyph, ProfileAction action, u16 item, u64 index, Utf8View hint) noexcept {
    const bool disabled = action == ProfileAction::activity_join;
    const float text = text_width(s, label, 400, kSm | text_style_bold);
    const float content = (glyph != UiIcon::count ? 24.f : 0.f) + text;
    const float w = full ? width : minimum(width, content + 32);
    auto* b = add(s, owner, {left, top, left + w, top + 32}, {}, action, 0, ProfileLook::secondary);
    if (!b)
        return -1;
    b->media = ProfileMediaKind::activity_action;
    b->source = item;
    b->target = index;
    b->hint = hint;
    const ProfileTone tone = disabled ? ProfileTone::muted : ProfileTone::normal;
    float x = left + maximum(16.f, (w - content) * .5f);
    if (glyph != UiIcon::count) {
        if (!icon(s, owner, {x, top + 8, x + 16, top + 24}, glyph, ProfileAction::none, tone))
            return -1;
        x += 24;
    }
    if (!add(s, owner, {x, top, maximum(x + 1, minimum(x + text + 1, left + w - 16)), top + 32}, label,
             ProfileAction::none, kSm | text_style_bold, ProfileLook::plain, tone))
        return -1;
    return w;
}
float activity_card(ProfileScene* s, UiWidgetHandle owner, float left, float top, float right, u32 card, u32 item,
                    u64 now_ms, ProfileAction action) noexcept {
    auto& l = labels(s);
    const auto& a = l.activity_set.items[item];
    auto& c = l.activity_content[card];
    const bool large = modal(*s);
    const auto m = card_metrics(large);
    if (!profile_activity_content(a, bot(*s), self(*s), &c))
        return 0;
    auto* background = add(s, owner, {left, top, right, top + 1}, {}, action, 0, ProfileLook::card);
    if (!background)
        return -1;
    background->media = ProfileMediaKind::activity_card;
    background->source = static_cast<u16>(item);
    const u16 source = static_cast<u16>(item);
    float y = top + m.pad;
    if (c.heading.bytes) {
        if (!add(s, owner, {left + m.pad, y, right - m.pad, y + 16}, c.heading.view(), ProfileAction::none,
                 kXs | text_style_bold, ProfileLook::plain, ProfileTone::strong))
            return -1;
        y += 16 + 8;
    }
    // Artwork, top-aligned in the body row.
    const float row = y;
    float text_left = left + m.pad;
    if (c.artwork) {
        bool known = false;
        const bool icon_ready = application_icon(*s, a.application_id, &known);
        const auto art = user_activity_artwork(a, icon_ready);
        if (a.application_id && !known && art.large != UserActivityArt::large &&
            s->missing_icon_count < kUserActivityMaximum)
            s->missing_icons[s->missing_icon_count++] = a.application_id;
        const Utf8View large_hint = art.large == UserActivityArt::large ? user_activity_text(a, UserActivityField::large_text)
                                    : art.large == UserActivityArt::small_image
                                        ? user_activity_text(a, UserActivityField::small_text)
                                        : Utf8View{};
        auto* image = add(s, owner, {left + m.pad, row, left + m.pad + m.image, row + m.image}, {},
                          large_hint.bytes ? hint_action(*s) : ProfileAction::none);
        if (!image)
            return -1;
        image->media = ProfileMediaKind::activity_large;
        image->source = source;
        image->target = static_cast<u64>(art.large);
        image->hint = large_hint;
        unfocusable(s, image);
        if (art.badge) {
            const float x = left + m.pad + m.image + 4, sy = row + m.image + 4;
            const auto small_hint = user_activity_text(a, UserActivityField::small_text);
            auto* corner = add(s, owner, {x - m.small, sy - m.small, x, sy}, {},
                               small_hint.bytes ? hint_action(*s) : ProfileAction::none);
            if (!corner)
                return -1;
            corner->media = ProfileMediaKind::activity_small;
            corner->source = source;
            corner->hint = small_hint;
            unfocusable(s, corner);
        }
        text_left += m.image + m.gap;
    }
    const float text_right = right - m.pad, column = maximum(1.f, text_right - text_left);
    // The details column, laid out from the row's top, then centred on the artwork.
    const u32 column_first = s->part_count;
    float cy = row;
    if (c.title.bytes) {
        const float h = card_text(s, owner, text_left, cy, column, c.title.view(), kSm | text_style_bold, kCardTitle,
                                  source);
        if (h < 0)
            return -1;
        cy += h;
    }
    for (u32 i = 0; i < c.line_count; ++i) {
        const float h = card_text(s, owner, text_left, cy, column, c.lines[i].view(), kXs, kCardLine, source);
        if (h < 0)
            return -1;
        cy += h;
    }
    ActivityTimer timer{kNoPart, kNoPart, kNoPart, kNoPart, kNoPart, static_cast<u8>(item), static_cast<u8>(card)};
    const auto clock = c.chips ? profile_activity_clock(a, now_ms) : ProfileActivityClock{};
    if (c.chips && (c.party.bytes || c.episode.bytes || clock.timer != ProfileActivityTimer::none)) {
        if (cy > row)
            cy += 4;
        float x = 0, line_top = cy;
        const auto chip = [&](ProfileActivityGlyph glyph, Utf8View text, bool timed) noexcept {
            const float label = timed ? clock_width(s, text, kXs) : text_width(s, text, 4096, kXs);
            const float w = minimum(column, 16 + label);
            if (x > 0 && x + w > column) {
                x = 0;
                line_top += kCardLine;
            }
            const float cx = text_left + x;
            const auto tone = timed ? ProfileTone::positive : ProfileTone::normal;
            auto* glyph_part = icon(s, owner, {cx, line_top + 2, cx + 12, line_top + 14}, chip_icon(glyph),
                                    ProfileAction::none, timed ? ProfileTone::positive : ProfileTone::subtle);
            auto* label_part = glyph_part ? add(s, owner, {cx + 16, line_top, cx + w, line_top + kCardLine}, text,
                                                ProfileAction::none, kXs, ProfileLook::plain, tone)
                                          : nullptr;
            if (!label_part)
                return false;
            label_part->media = ProfileMediaKind::activity_text;
            label_part->source = source;
            if (timed) {
                timer.clock = part_index(*s, label_part);
                timer.glyph = part_index(*s, glyph_part);
            }
            x += w + 8;
            return true;
        };
        auto& clock_text = l.activity_time[card][0];
        clock_text.bytes = static_cast<u16>(
            profile_activity_clock_text(clock.seconds, {clock_text.data, sizeof(clock_text.data)}));
        const auto clock_glyph =
            clock.timer == ProfileActivityTimer::countdown ? ProfileActivityGlyph::timer : c.clock_glyph;
        const bool timed = clock.timer != ProfileActivityTimer::none;
        if ((c.party.bytes && !chip(ProfileActivityGlyph::party, c.party.view(), false)) ||
            (timed && !chip(clock_glyph, clock_text.view(), true)) ||
            (c.episode.bytes && !chip(ProfileActivityGlyph::episode, c.episode.view(), false)))
            return -1;
        cy = line_top + kCardLine;
    }
    ProfileActivityProgress progress{};
    if (profile_activity_progress(a, now_ms, &progress)) {
        if (cy > row)
            cy += 4;
        auto& elapsed = l.activity_time[card][1];
        auto& total = l.activity_time[card][2];
        elapsed.bytes = static_cast<u16>(
            profile_activity_track_text(progress.elapsed, {elapsed.data, sizeof(elapsed.data)}));
        total.bytes = static_cast<u16>(profile_activity_track_text(progress.duration, {total.data, sizeof(total.data)}));
        // Digits as 8s size the ends; monospace would draw an inline-code box.
        const u32 flags = kXs;
        const float label = maximum(clock_width(s, elapsed.view(), flags), clock_width(s, total.view(), flags));
        auto* left_label = add(s, owner, {text_left, cy, text_left + label, cy + kCardLine}, elapsed.view(),
                               ProfileAction::none, flags);
        auto* right_label = add(s, owner, {text_right - label, cy, text_right, cy + kCardLine}, total.view(),
                                ProfileAction::none, flags);
        auto* bar = add(s, owner, {text_left + label + 8, cy + 7, maximum(text_left + label + 9, text_right - label - 8), cy + 9});
        if (!left_label || !right_label || !bar)
            return -1;
        left_label->media = ProfileMediaKind::activity_text;
        right_label->media = ProfileMediaKind::activity_clock;
        bar->media = ProfileMediaKind::activity_progress;
        left_label->source = right_label->source = bar->source = source;
        bar->color = {progress.fraction, 0, 0, 1};
        timer.elapsed = part_index(*s, left_label);
        timer.total = part_index(*s, right_label);
        timer.bar = part_index(*s, bar);
        cy += kCardLine;
    }
    // Buttons: the activity's own, Watch for a stream, or the disabled Ask to Join.
    Utf8View button_labels[2]{};
    u32 buttons = 0;
    ProfileAction button_action = ProfileAction::activity_button;
    UiIcon button_icon = UiIcon::count;
    if (interactive(*s) && c.action == ProfileActivityAction::custom) {
        for (u32 i = 0; i < a.button_count && i < 2; ++i)
            button_labels[buttons++] = user_activity_text(a, i ? UserActivityField::button_1 : UserActivityField::button_0);
    } else if (interactive(*s) && c.action == ProfileActivityAction::watch) {
        button_labels[buttons++] = utf8_literal("Watch");
        button_action = ProfileAction::activity_watch;
    } else if (interactive(*s) && c.action == ProfileActivityAction::join) {
        button_labels[buttons++] = utf8_literal("Ask to Join");
        button_action = ProfileAction::activity_join;
        button_icon = UiIcon::activity;
    }
    const Utf8View button_hint = button_action == ProfileAction::activity_join ? c.hint.view() : Utf8View{};
    if (large && buttons) {
        cy += cy > row ? 8.f : 0.f;
        float x = 0;
        for (u32 i = 0; i < buttons; ++i) {
            const float natural = minimum(column, text_width(s, button_labels[i], 400, kSm | text_style_bold) + 32 +
                                                      (button_icon != UiIcon::count ? 24.f : 0.f));
            if (x > 0 && x + natural > column) {
                x = 0;
                cy += 32 + 8;
            }
            const float w = card_button(s, owner, text_left + x, cy, column - x, false, button_labels[i], button_icon,
                                        button_action, source, i, button_hint);
            if (w < 0)
                return -1;
            x += w + 8;
        }
        cy += 32;
    }
    const float details = cy - row;
    const float body = c.artwork ? maximum(m.image, details) : details;
    shift(s, column_first, (body - details) * .5f);
    float bottom = row + body;
    if (!large && buttons) {
        bottom += 12;
        for (u32 i = 0; i < buttons; ++i) {
            if (card_button(s, owner, left + m.pad, bottom, right - left - 2 * m.pad, true, button_labels[i],
                            button_icon, button_action, source, i, button_hint) < 0)
                return -1;
            bottom += 32 + (i + 1 < buttons ? 8.f : 0.f);
        }
    }
    bottom += m.pad;
    if (!place(s, background->widget, {left, top, right, bottom}))
        return -1;
    if ((timer.clock != kNoPart || timer.bar != kNoPart) && l.timer_count < kUserActivityMaximum)
        l.timers[l.timer_count++] = timer;
    return bottom - top;
}
// The profile user's cards in the official order; the set is copied into the
// scene first. Offline users show none.
u32 activity_cards(ProfileScene* s, u8* order) noexcept {
    auto& l = labels(s);
    s->activity_set = nullptr;
    if (!s->users || !s->profile || s->presence == UiPresence::offline ||
        !user_activity_find(s->users->activities, s->profile->user_id, &l.activity_set) || !l.activity_set.count)
        return 0;
    s->activity_set = &l.activity_set;
    return profile_activity_order(l.activity_set, {order, kUserActivityMaximum});
}

// The custom status thought bubble. Returns its height (0 when there is no
// custom status). max_width bounds the whole bubble.
float status_bubble(ProfileScene* s, UiWidgetHandle owner, float left, float top, float max_width) noexcept {
    const auto* status = appearance(*s, appearance_activity);
    if (!status || (!status->activity.bytes && !status->status_emoji.bytes && !status->status_emoji_id))
        return 0;
    // Only custom status shows here: a "Playing X" line stands in for it in
    // the member list when there is none, and the cards already say that.
    const bool emoji = status->status_emoji_id || status->status_emoji.bytes;
    for (u32 i = 0; !emoji && s->activity_set && i < s->activity_set->count; ++i) {
        char line[160];
        const u32 bytes = user_activity_summary(s->activity_set->items[i], {line, sizeof(line)});
        if (bytes && utf8_equal(status->activity.view(), {line, bytes}))
            return 0;
    }
    const Utf8View text = status->activity.view();
    const float content_limit = minimum(155.f, max_width - 26);
    if (content_limit < 32)
        return 0;
    const bool emoji_only = emoji && !text.bytes;
    const float emoji_width = emoji_only ? 32.f : emoji ? 20.f : 0.f;
    float content_width = emoji_width, content_height = emoji_only ? 32.f : 18.f;
    if (text.bytes) {
        const float natural = text_width(s, text, 4096, kSm);
        if (natural + emoji_width <= content_limit)
            content_width += natural;
        else {
            content_width = content_limit;
            content_height = minimum(36.f, measure(s, text, content_limit - emoji_width, true, kSm).height);
            content_height = maximum(18.f, content_height);
        }
    }
    const float width = maximum(42.f, minimum(max_width, content_width + 26)),
                height = content_height + 18;
    if (!add(s, owner, {left, top, left + width, top + height}, {}, ProfileAction::none, 0, ProfileLook::bubble))
        return -1;
    const float x = left + 13, y = top + 9;
    if (emoji) {
        const float size = emoji_only ? 32.f : 16.f;
        auto* e = add(s, owner, {x, y + (emoji_only ? 0.f : 1.f), x + size, y + size + (emoji_only ? 0.f : 1.f)},
                      status->status_emoji_id ? Utf8View{} : status->status_emoji.view(), ProfileAction::none,
                      emoji_only ? kXl : kSm);
        if (!e)
            return -1;
        if (status->status_emoji_id)
            e->media = ProfileMediaKind::status_emoji;
    }
    if (text.bytes) {
        auto* line = add(s, owner, {x + emoji_width, y, left + width - 13, y + content_height}, text,
                         ProfileAction::none, kSm);
        if (!line)
            return -1;
        line->wrap = content_height > 18;
    }
    return height;
}

// Role pills. Popouts show at most two lines within the content width and end
// with "+N"; the modal and an expanded popout list every role. Returns the
// block height (0 without roles or add button).
struct RoleItem {
    const RoleRecord* role;
    Utf8View name;
    float width;
    bool removable;
};
float roles(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, bool clamp) noexcept {
    const auto& p = *s->profile;
    if (!s->state || !p.guild_id)
        return 0;
    const auto policy = role_edit_policy(*s->state, p.guild_id);
    // The profile lists the member's roles in wire order; pills follow the role hierarchy,
    // linked roles last.
    const RoleRecord* ranked[128];
    u32 ranked_count = 0;
    for (u32 i = 0; i < p.role_count && i < 128; ++i)
        if (const auto* role = chat_state_store_find_role(*s->state, p.guild_id, p.roles[i]))
            ranked[ranked_count++] = role;
    heap_sort(ranked, ranked_count,
              [](const RoleRecord* left, const RoleRecord* right) noexcept { return role_pill_before(*left, *right); });
    RoleItem items[128];
    u32 count = 0;
    for (u32 i = 0; i < ranked_count; ++i) {
        const auto* role = ranked[i];
        const auto name = chat_state_store_role_name(*s->state, *role);
        const bool removable = interactive(*s) && !s->roles_busy && role_editable(policy, *role);
        const float label = text_width(s, name, width, kXs);
        items[count++] = {role, name, minimum(width, 2 + 4 + 12 + 4 + label + (removable ? 24.f : 8.f)), removable};
    }
    const bool add_button = interactive(*s) && s->can_manage_roles && !s->roles_busy;
    if (!count && !add_button)
        return 0;
    const float add_width = count ? 24.f : text_width(s, utf8_literal("Add role"), width, kXs) + 16;
    // Lines used by the first k pills followed by the tail controls.
    const auto lines = [&](u32 k, bool more, float more_width) noexcept {
        u32 used = 1;
        float x = 0;
        const auto put = [&](float w) noexcept {
            if (x > 0 && x + w > width) {
                ++used;
                x = 0;
            }
            x += w + 4;
        };
        for (u32 i = 0; i < k; ++i)
            put(items[i].width);
        if (more)
            put(more_width);
        if (add_button)
            put(add_width);
        return used;
    };
    u32 visible = count;
    auto& more_text = labels(s).roles_more;
    more_text.bytes = 0;
    float more_width = 0;
    const bool collapse = clamp && s->roles_expanded;
    if (clamp && !s->roles_expanded && lines(count, false, 0) > 2) {
        for (visible = count; visible > 0; --visible) {
            more_text.bytes = 0;
            append(&more_text, utf8_literal("+"));
            append_number(&more_text, count - visible);
            more_width = maximum(24.f, text_width(s, more_text.view(), width, kXs) + 16);
            if (lines(visible, true, more_width) <= 2)
                break;
        }
        if (!visible) {
            more_text.bytes = 0;
            append(&more_text, utf8_literal("+"));
            append_number(&more_text, count);
            more_width = maximum(24.f, text_width(s, more_text.view(), width, kXs) + 16);
        }
    }
    float x = 0, y = 0;
    const auto slot = [&](float w) noexcept {
        if (x > 0 && x + w > width) {
            x = 0;
            y += 28;
        }
        const UiRect r{left + x, top + y, left + x + minimum(w, width - x), top + y + 24};
        x += w + 4;
        return r;
    };
    for (u32 i = 0; i < visible; ++i) {
        const auto& item = items[i];
        const auto r = slot(item.width);
        auto* pill = add(s, owner, r, {}, ProfileAction::none, 0, ProfileLook::role);
        if (!pill)
            return -1;
        pill->color = item.role->color ? packed(item.role->color) : Color{};
        pill->target = item.role->id;
        if (!add(s, owner, {r.left + 22, r.top + 4, r.right - (item.removable ? 24.f : 8.f), r.bottom - 4},
                 item.name, ProfileAction::none, kXs))
            return -1;
        if (item.removable) {
            auto* remove = icon(s, owner, {r.right - 24, r.top, r.right, r.bottom}, UiIcon::close,
                                ProfileAction::remove_role);
            if (!remove)
                return -1;
            remove->target = item.role->id;
            remove->source = 12;
        }
    }
    if (more_text.bytes || collapse) {
        const auto r = slot(collapse ? 24.f : more_width);
        auto* more = add(s, owner, r, collapse ? Utf8View{} : more_text.view(), ProfileAction::roles_toggle, kXs,
                         ProfileLook::pill);
        if (!more)
            return -1;
        more->centered = true;
        if (collapse)
            more->icon = static_cast<u16>(UiIcon::chevron_up);
    }
    if (add_button) {
        const auto r = slot(add_width);
        auto* plus = add(s, owner, r, count ? Utf8View{} : utf8_literal("Add role"), ProfileAction::choose_roles,
                         kXs, ProfileLook::pill, ProfileTone::subtle);
        if (!plus)
            return -1;
        plus->centered = true;
        if (count)
            plus->icon = static_cast<u16>(UiIcon::plus);
    }
    return y + 24;
}

// Plugin profile-section nodes: chips, then full-width link rows.
float plugin_section(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept {
    UiPluginSlotNode nodes[8]{};
    const u32 count = ui_plugin_slots(kUiPluginSlotProfileSection, 0, 0, s->profile->user_id, {nodes, 8});
    float x = 0, y = 0;
    bool chips = false;
    for (u32 i = 0; i < count; ++i) {
        const auto& node = nodes[i];
        const Utf8View label = node.text.bytes ? node.text : node.tooltip;
        if (!label.bytes || node.kind == ui_plugin_node_link)
            continue;
        const float chip =
            ui_rect_width(ui_badge_layout(s->measure, label, {0, 0, width, 26}, true, text_style_subtext).outer);
        if (x > 0 && x + chip > width) {
            x = 0;
            y += 30;
        }
        auto* part = add(s, owner, {left + x, top + y, left + x + chip, top + y + 26}, {},
                         node.kind == ui_plugin_node_button ? ProfileAction::plugin_slot : ProfileAction::none);
        if (!part || !ui_badge_attach(&part->badge, &s->widgets, part->widget, label, {}, true, false,
                                      text_style_subtext))
            return -1;
        part->target = i;
        part->media = ProfileMediaKind::plugin_slot;
        part->source = node.icon;
        part->style = node.tone;
        x += chip + 6;
        chips = true;
    }
    if (chips)
        y += 26;
    // Link rows open their URL through the link confirmation (application side).
    for (u32 i = 0; i < count; ++i) {
        const auto& node = nodes[i];
        const Utf8View label = node.text.bytes ? node.text : node.tooltip;
        if (!label.bytes || node.kind != ui_plugin_node_link)
            continue;
        if (y > 0)
            y += 4;
        auto* part = add(s, owner, {left, top + y, left + width, top + y + 20}, label, ProfileAction::plugin_slot,
                         kSm, ProfileLook::plain, ProfileTone::link);
        if (!part)
            return -1;
        part->target = i;
        part->media = ProfileMediaKind::plugin_slot;
        y += 20;
    }
    return y;
}

// Display name row, then username, pronouns, guild tag and badges in one
// wrapping row. Returns the block height.
float name_block(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, bool large) noexcept {
    const auto& p = *s->profile;
    const auto display = p.nickname.bytes ? p.nickname.view() : p.display_name.view();
    const float line = large ? 30.f : 24.f;
    const u32 name_flags = (large ? kXl : kLg) | text_style_bold;
    const bool note = !large && interactive(*s) && s->note_visible && !self(*s);
    float tag = 0;
    if (bot(*s))
        tag = ui_rect_width(ui_badge_layout(s->measure, utf8_literal("APP"), {0, 0, 64, 20}, false).outer) + 4;
    const float name_width = maximum(24.f, text_width(s, display, width - tag - (note ? 28.f : 0.f), name_flags));
    auto* name = add(s, owner, {left, top, left + name_width, top + line}, display,
                     large ? ProfileAction::none : ProfileAction::expand, name_flags);
    if (!name)
        return -1;
    s->name_widget = name->widget;
    float x = left + name_width + 4;
    if (tag) {
        auto* badge = add(s, owner, {x, top + (line - 20) * .5f, x + tag - 4, top + (line + 20) * .5f});
        if (!badge || !ui_badge_attach(&badge->badge, &s->widgets, badge->widget, utf8_literal("APP"), {}, false,
                                       false, text_style_subtext | text_style_bold))
            return -1;
        badge->badge.accent = true;
        x += tag;
    }
    if (note && !icon(s, owner, {x, top, x + 24, top + 24}, UiIcon::file_text, ProfileAction::open_note))
        return -1;
    float y = top + line;
    // Username row wraps; column gap is 6 with pronouns or a bot tag, else 9.33.
    const auto pronouns = p.guild_pronouns.bytes ? p.guild_pronouns.view() : p.pronouns.view();
    const float gap = pronouns.bytes || bot(*s) ? 6.f : 9.33f;
    float cx = 0, row_top = y, row_height = 0;
    const auto slot = [&](float w, float h) noexcept {
        if (cx > 0 && cx + w > width) {
            row_top += row_height + (large ? 4.f : 2.f);
            cx = 0;
            row_height = 0;
        }
        const float x0 = left + cx;
        cx += w + gap;
        row_height = maximum(row_height, h);
        return UiRect{x0, row_top, x0 + minimum(w, width), row_top + h};
    };
    const auto profile_display_info = profile_display(p);
    if (profile_display_info.guild_avatar && p.avatar.bytes) {
        const float size = p.nickname.bytes ? 16.f : 24.f;
        auto* main = add(s, owner, slot(size, size));
        if (!main)
            return -1;
        main->media = ProfileMediaKind::peer_avatar;
        main->source = 0xFFFF;
    }
    if (!add(s, owner, slot(text_width(s, p.username.view(), width, kSm), 18), p.username.view(),
             large ? ProfileAction::none : ProfileAction::expand, kSm, ProfileLook::plain, ProfileTone::strong))
        return -1;
    if (pronouns.bytes) {
        auto dot = slot(6, 18);
        if (!add(s, owner, {dot.left + 1, dot.top + 7, dot.left + 5, dot.top + 11}, {}, ProfileAction::none, 0,
                 ProfileLook::dot))
            return -1;
        if (!add(s, owner, slot(text_width(s, pronouns, width, kSm) + 2, 18), pronouns, ProfileAction::none, kSm,
                 ProfileLook::plain, ProfileTone::strong))
            return -1;
    }
    const auto& a = p.user_appearance;
    if (a.primary_enabled && a.primary_tag.bytes) {
        const auto measured = ui_badge_layout(s->measure, a.primary_tag.view(), {0, 0, width, 20}, true,
                                              text_style_subtext);
        const auto r = slot(ui_rect_width(measured.outer), 20);
        // The chip opens the server tag popout (live cards only).
        const auto tag_action = interactive(*s) && a.primary_guild_id ? ProfileAction::guild_tag : ProfileAction::none;
        auto* tag_part = add(s, owner, r, {}, tag_action);
        if (!tag_part || !ui_badge_attach(&tag_part->badge, &s->widgets, tag_part->widget, a.primary_tag.view(), {},
                                          true, false, text_style_subtext))
            return -1;
        tag_part->target = a.primary_guild_id;
        const auto layout = ui_badge_layout(s->measure, a.primary_tag.view(), r, true, text_style_subtext);
        auto* glyph = add(s, owner, layout.icon, {}, tag_action);
        if (!glyph)
            return -1;
        glyph->media = ProfileMediaKind::guild_icon;
        glyph->target = a.primary_guild_id;
    }
    if (p.badge_count) {
        // Popouts chunk 12 badges a row; the modal keeps one row.
        const u32 per_row = large ? p.badge_count : 12;
        const u32 shown = minimum<u32>(p.badge_count, 24);
        const u32 rows = (shown + per_row - 1) / per_row;
        const u32 first_row = minimum(shown, per_row);
        const float block_w = first_row * 22.f - 2, block_h = rows * 22.f;
        const auto block = slot(minimum(block_w, width), block_h);
        for (u32 i = 0; i < shown; ++i) {
            const float bx = block.left + (i % per_row) * 22.f, by = block.top + 1 + (i / per_row) * 22.f;
            if (bx + 20 > left + width)
                continue;
            if (!add(s, owner, {bx, by, bx + 20, by + 20}, {}, ProfileAction::none, 0, ProfileLook::badge))
                return -1;
            auto* b = add(s, owner, {bx, by, bx + 20, by + 20});
            if (!b)
                return -1;
            b->media = ProfileMediaKind::badge;
            b->source = static_cast<u16>(i);
        }
    }
    return row_top + row_height - top;
}

float button(ProfileScene* s, UiWidgetHandle owner, float left, float top, Utf8View label, UiIcon glyph,
             ProfileAction action, bool primary, float width = 0) noexcept;
// Relationship notices: the incoming friend request
// card with Accept, or "You blocked them".
float notices(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, bool large) noexcept {
    if (self(*s) || (s->relation != 2 && s->relation != 4))
        return 0;
    const float pad = large ? 12.f : 16.f;
    if (s->relation == 4) {
        if (!add(s, owner, {left, top, left + width, top + 18 + pad * 2}, {}, ProfileAction::none, 0, ProfileLook::card) ||
            !add(s, owner, {left + pad, top + pad, left + width - pad, top + pad + 18}, utf8_literal("You blocked them"),
                 ProfileAction::none, kSm | text_style_bold))
            return -1;
        return 18 + pad * 2;
    }
    const float line = 18, button_top = top + pad + line + 8;
    if (!add(s, owner, {left, top, left + width, button_top + 32 + pad}, {}, ProfileAction::none, 0, ProfileLook::card))
        return -1;
    const auto name = s->profile->username.view();
    const float name_width = text_width(s, name, width - pad * 2, kSm | text_style_bold);
    if (!add(s, owner, {left + pad, top + pad, left + pad + name_width, top + pad + line}, name, ProfileAction::none,
             kSm | text_style_bold) ||
        !add(s, owner, {left + pad + name_width, top + pad, left + width - pad, top + pad + line},
             utf8_literal(" sent you a friend request."), ProfileAction::none, kSm))
        return -1;
    if (interactive(*s) &&
        button(s, owner, left + pad, button_top, utf8_literal("Accept"), UiIcon::count, ProfileAction::accept_friend,
               true) < 0)
        return -1;
    return button_top + 32 + pad - top;
}
// The popout's mutuals row.
float mutuals(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept {
    const auto& p = *s->profile;
    const u32 friends = p.mutual_friend_count, servers = p.mutual_guild_count;
    if (self(*s) || bot(*s) || (!friends && !servers))
        return 0;
    auto& l = labels(s);
    const u32 flags = friends && servers ? kXs : kSm;
    float x = left;
    if (friends) {
        auto& text = l.mutual_text[0];
        text.bytes = 0;
        append_number(&text, friends);
        append(&text, friends == 1 ? utf8_literal(" Mutual Friend") : utf8_literal(" Mutual Friends"));
        const u32 faces = minimum<u32>(3, p.mutual_friends_count);
        const float stack = faces ? 16 + (faces - 1) * 12.f + 4 : 0;
        const float w = minimum(width, stack + text_width(s, text.view(), width, flags));
        if (!add(s, owner, {x, top, x + w, top + 24}, {}, ProfileAction::friends, 0, ProfileLook::plain))
            return -1;
        for (u32 i = faces; i > 0; --i) {
            auto* face = add(s, owner, {x + (i - 1) * 12.f, top + 4, x + (i - 1) * 12.f + 16, top + 20});
            if (!face)
                return -1;
            face->media = ProfileMediaKind::peer_avatar;
            face->source = static_cast<u16>(i - 1);
        }
        if (!add(s, owner, {x + stack, top + 4, x + w, top + 20}, text.view(), ProfileAction::none, flags,
                 ProfileLook::plain, ProfileTone::subtle))
            return -1;
        x += w + 6;
    }
    if (servers) {
        if (friends) {
            if (!add(s, owner, {x, top + 10, x + 4, top + 14}, {}, ProfileAction::none, 0, ProfileLook::dot))
                return -1;
            x += 10;
        }
        auto& text = l.mutual_text[1];
        text.bytes = 0;
        append_number(&text, servers);
        append(&text, servers == 1 ? utf8_literal(" Mutual Server") : utf8_literal(" Mutual Servers"));
        const u32 icons = friends ? 0 : minimum<u32>(3, p.mutual_guild_count);
        const float stack = icons ? 16 + (icons - 1) * 12.f + 4 : 0;
        const float w = minimum(left + width - x, stack + text_width(s, text.view(), width, flags));
        if (w > 0) {
            if (!add(s, owner, {x, top, x + w, top + 24}, {}, ProfileAction::servers, 0, ProfileLook::plain))
                return -1;
            for (u32 i = icons; i > 0; --i) {
                auto* face = add(s, owner, {x + (i - 1) * 12.f, top + 4, x + (i - 1) * 12.f + 16, top + 20});
                if (!face)
                    return -1;
                face->media = ProfileMediaKind::peer_guild;
                face->source = static_cast<u16>(i - 1);
            }
            if (!add(s, owner, {x + stack, top + 4, x + w, top + 20}, text.view(), ProfileAction::none, flags,
                     ProfileLook::plain, ProfileTone::subtle))
                return -1;
        }
    }
    return 24;
}

// Biography: no heading; clamped to three lines in popouts (with "View Full
// Bio" when it overflows), unclamped in the modal.
float biography(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, bool clamp) noexcept {
    const auto source = profile_display(*s->profile).bio;
    if (!source.bytes || !prepare_biography(s, source))
        return 0;
    auto bio = s->bio_display.view();
    UiTextMeasureValue size{};
    if (bio.bytes && s->measure &&
        !ui_text_measure_cache_measure(s->measure,
                                       {{0x42494F, s->bio_signature, 0, 0, 0},
                                        bio,
                                        {s->bio_styles, s->bio_style_count},
                                        {width, 4096},
                                        false},
                                       &size)
             .ok()) {
        // A rejected biography must not abort the shared text projection after
        // this card's widgets were cleared: show a placeholder, leave the
        // source untouched so a later layout can retry.
        constexpr auto unavailable = utf8_literal("About me unavailable");
        copy_bytes(s->bio_display.data, unavailable.data, unavailable.bytes);
        s->bio_display.bytes = static_cast<u16>(unavailable.bytes);
        s->bio_semantic_count = 0;
        size_biography(s, nullptr, 0);
        s->bio_signature = 0;
        bio = s->bio_display.view();
        size.size = measure(s, bio, width, true, kSm);
    } else if (!s->measure)
        size.size = measure(s, bio, width, true, kSm);
    if (!bio.bytes)
        return 0;
    float height = maximum(18.f, size.size.height);
    const bool overflow = clamp && height > 57.75f;
    if (overflow)
        height = 54;
    // The text-sm size rides on the bio's own style spans.
    auto* text = add(s, owner, {left, top, left + width, top + height}, bio, ProfileAction::none, 0,
                     ProfileLook::plain, ProfileTone::strong);
    if (!text)
        return -1;
    text->wrap = true;
    s->bio_widget = text->widget;
    if (!overflow || !interactive(*s))
        return height;
    constexpr auto label = utf8_literal("View Full Bio");
    const float w = text_width(s, label, width, kXs) + 24;
    auto* button = add(s, owner, {left, top + height + 8, left + w, top + height + 40}, {}, ProfileAction::view_bio,
                       0, ProfileLook::secondary);
    if (!button || !add(s, owner, {left + 12, top + height + 8, left + w - 12, top + height + 40}, label,
                        ProfileAction::none, kXs))
        return -1;
    return height + 40;
}

// A primary or secondary button with an optional leading icon; zero-width
// labels make a square icon button. Returns its width.
float button(ProfileScene* s, UiWidgetHandle owner, float left, float top, Utf8View label, UiIcon glyph,
             ProfileAction action, bool primary, float width) noexcept {
    const float text = label.bytes ? text_width(s, label, 400, kSm | text_style_bold) : 0;
    const float content = (glyph != UiIcon::count ? 16.f : 0.f) + (label.bytes && glyph != UiIcon::count ? 8.f : 0.f) + text;
    const float w = width ? width : label.bytes ? content + 32 : 32;
    auto* b = add(s, owner, {left, top, left + w, top + 32}, {}, action, 0,
                  primary ? ProfileLook::primary : ProfileLook::secondary);
    if (!b)
        return -1;
    float x = left + (w - content) * .5f;
    if (glyph != UiIcon::count) {
        if (!icon(s, owner, {x, top + 8, x + 16, top + 24}, glyph, ProfileAction::none,
                  primary ? ProfileTone::on_accent : ProfileTone::normal))
            return -1;
        x += 24;
    }
    if (label.bytes && !add(s, owner, {x, top, x + text + 1, top + 32}, label, ProfileAction::none,
                            kSm | text_style_bold, ProfileLook::plain,
                            primary ? ProfileTone::on_accent : ProfileTone::normal))
        return -1;
    return w;
}

// Header geometry shared by popout and modal: banner, avatar with its cut-out
// stroke, decoration at 1.2x and the status dot.
void header_geometry(ProfileScene* s, UiRect banner, float avatar_left, float avatar_top, float size) noexcept {
    s->banner = banner;
    s->avatar = {avatar_left, avatar_top, avatar_left + size, avatar_top + size};
    const float grow = size * .1f;
    s->decoration = ui_rect_inset(s->avatar, {-grow, -grow, -grow, -grow});
    const float status = size >= 120 ? 24.f : size >= 96 ? 20.f : 16.f;
    const float offset = size >= 120 ? 8.f : size >= 96 ? 6.f : 4.f;
    s->avatar_stroke = size >= 96 ? 8.f : 6.f;
    const float at = size - status - offset;
    s->status = {avatar_left + at, avatar_top + at, avatar_left + at + status, avatar_top + at + status};
}
// Banner, avatar and decoration parts (relative to owner at origin).
bool header_parts(ProfileScene* s, UiWidgetHandle owner, UiRect origin, ProfileAction avatar_action) noexcept {
    const auto rel = [&](UiRect r) noexcept {
        return UiRect{r.left - origin.left, r.top - origin.top, r.right - origin.left, r.bottom - origin.top};
    };
    auto* banner = add(s, owner, rel(s->banner));
    auto* avatar = add(s, owner, rel(s->avatar), {}, avatar_action);
    if (!banner || !avatar)
        return false;
    banner->media = ProfileMediaKind::banner;
    avatar->media = ProfileMediaKind::avatar;
    if (!profile_display(*s->profile).decoration.bytes)
        return true;
    auto* decoration = add(s, owner, rel(s->decoration));
    if (!decoration)
        return false;
    decoration->media = ProfileMediaKind::decoration;
    return true;
}
void resolve_theme(ProfileScene* s) noexcept {
    const auto d = profile_display(*s->profile, s->presentation == ProfilePresentation::preview);
    s->themed = d.themed;
    s->theme_primary = d.theme[0];
    s->theme_secondary = d.theme[1];
    s->theme_dark = d.themed && profile_theme_dark(d.theme[0]);
    s->has_banner_color = d.has_color;
    s->banner_color = d.color;
}
bool can_message(const ProfileScene& s) noexcept {
    return !self(s) && s.relation != 4;
}
bool finish(ProfileScene* s, UiRect viewport) noexcept {
    ++s->revision;
    if (!ui_widget_manager_layout_rectangle(&s->widgets, viewport).ok())
        return false;
    if (s->note_editing && s->note_field.generation) {
        // The note field takes the edit view: geometry follows its laid-out row.
        const auto* node = ui_widget_manager_layout_node(s->widgets, s->note_field);
        if (!node)
            return false;
        s->message_field = s->note_field;
        s->edit_rectangle = node->rectangle;
        s->edit_content = ui_rect_inset(s->edit_rectangle, {10, 2, 10, 2});
        s->message_placeholder = {};
        append(&s->message_placeholder, utf8_literal("Click to add a note"));
        if (s->edit.focused)
            ui_widget_manager_focus(&s->widgets, s->note_field);
    }
    return true;
}

// The popout (and the editor preview, which is the same card).
bool layout_popout(ProfileScene* s, UiRect viewport) noexcept {
    const auto& p = *s->profile;
    const bool live = interactive(*s);
    const float W = kPopoutWidth, pad = s->themed ? 2.f : 0.f;
    const float inner = W - 2 * pad, content = inner - 32, x0 = pad + 16;
    auto* panel = add(s, s->root, {0, 0, W, 1});
    if (!panel)
        return false;
    s->panel = panel->widget;
    // Header (relative to the card).
    header_geometry(s, {pad, pad, W - pad, pad + kPopoutBanner}, 16, 61, 80);
    u8 order[kUserActivityMaximum];
    const u32 cards = activity_cards(s, order);
    const float bubble_max = W - pad - 12 - (pad + kPopoutBanner);
    const float bubble =
        status_bubble(s, s->panel, pad + kPopoutBanner, pad + kPopoutBanner - 7, minimum(181.f, bubble_max));
    if (bubble < 0)
        return false;
    const float header_bottom = maximum(140.f, bubble ? pad + kPopoutBanner - 10 + bubble : 0.f);
    // Body (scroller): padding 4 16 8, sections 12 apart.
    const float body_top = header_bottom + 8;
    auto* body = add(s, s->panel, {0, body_top, W, body_top + 1}, {}, ProfileAction::none, 0, ProfileLook::plain,
                     ProfileTone::normal, true);
    if (!body)
        return false;
    s->body = body->widget;
    float y = 4;
    const auto section = [&](float height) noexcept {
        if (height > 0)
            y += height + 12;
        return height >= 0;
    };
    if (!section(name_block(s, s->body, x0, y, content, false)))
        return false;
    if (s->phase == ProfilePhase::failed &&
        !section(add(s, s->body, {x0, y, x0 + content, y + 18}, utf8_literal("Unable to load parts of profile"),
                     ProfileAction::retry, kSm, ProfileLook::plain, ProfileTone::muted)
                     ? 18.f
                     : -1.f))
        return false;
    if (!section(notices(s, s->body, x0, y, content, false)) || !section(mutuals(s, s->body, x0, y, content)) ||
        !section(biography(s, s->body, x0, y, content, true)))
        return false;
    // Only the first card; more peek 8 DIPs below it ("View More").
    if (cards) {
        const bool peek = cards > 1;
        const float h = activity_card(s, s->body, x0, y, x0 + content, 0, order[0], activity_now(*s), ProfileAction::card);
        if (h < 0)
            return false;
        if (peek && h > 0 &&
            !add(s, s->body, {x0 + 8, y + h - 8, x0 + content - 8, y + h + 8}, {}, ProfileAction::activity, 0,
                 ProfileLook::peek))
            return false;
        // The peek paints only below the first card, so its order is free.
        if (!section(h + (peek && h > 0 ? 8.f : 0.f)))
            return false;
    }
    if (!section(roles(s, s->body, x0, y, content, true)) || !section(plugin_section(s, s->body, x0, y, content)))
        return false;
    const float body_content = y > 4 ? y - 12 + 8 : 12;
    // Footer: Message @name for others, Edit Profile for self.
    const bool self_card = self(*s);
    const bool footer = live && (self_card || can_message(*s));
    const float footer_height = !footer ? 0.f : self_card ? 32.f : 40.f;
    const float natural = body_top + body_content + (footer ? 8 + footer_height + 12 : 0.f) + 4 + pad;
    const float limit = s->presentation == ProfilePresentation::preview ? natural
                                                                        : maximum(200.f, ui_rect_height(viewport) - 20);
    const float H = minimum(natural, limit);
    const float footer_top = H - pad - 4 - 12 - footer_height;
    const float body_bottom = footer ? footer_top - 8 : H - pad - 4;
    if (footer && self_card) {
        if (button(s, s->panel, x0, footer_top, utf8_literal("Edit Profile"), UiIcon::edit, ProfileAction::edit, true,
                   content) < 0)
            return false;
    } else if (footer) {
        auto* field = add(s, s->panel, {x0, footer_top, x0 + content, footer_top + footer_height}, {},
                          ProfileAction::message, 0, ProfileLook::input);
        auto* widget = field ? ui_widget_manager_resolve(&s->widgets, field->widget) : nullptr;
        if (!widget)
            return false;
        widget->kind = UiWidgetKind::text_field;
        s->message_field = field->widget;
    }
    if (s->message_failed && footer &&
        !add(s, s->panel, {x0, footer_top - 26, x0 + content, footer_top - 6},
             utf8_literal("Could not send. Press Enter to retry."), ProfileAction::none, kXs, ProfileLook::plain,
             ProfileTone::muted))
        return false;
    // Placement: beside the anchor with 8 DIPs spacing,
    // inverted when it does not fit and nudged into the viewport.
    float x = s->anchor.left, top = s->anchor.top;
    const float margin = 10;
    if (s->presentation != ProfilePresentation::preview) {
        if (s->placement == ProfilePlacement::above) {
            top = s->anchor.top - 8 - H;
            if (top < viewport.top + margin)
                top = s->anchor.bottom + 8;
        } else {
            const float spacing = s->placement == ProfilePlacement::left ? 16.f : 8.f;
            const bool right_fits = s->anchor.right + spacing + W <= viewport.right - margin;
            const bool left_fits = s->anchor.left - spacing - W >= viewport.left + margin;
            const bool right = s->placement == ProfilePlacement::right ? right_fits || !left_fits : !left_fits && right_fits;
            x = right ? s->anchor.right + spacing : s->anchor.left - spacing - W;
        }
        x = maximum(viewport.left + margin, minimum(x, viewport.right - margin - W));
        top = maximum(viewport.top + margin, minimum(top, viewport.bottom - margin - H));
        const float shift = s->intro_offset;
        if (s->placement == ProfilePlacement::above)
            top -= shift;
        else
            x += s->placement == ProfilePlacement::left ? -shift : shift;
    }
    s->rectangle = {x, top, x + W, top + H};
    s->preview_height = H;
    s->card = s->rectangle;
    s->effect_area = ui_rect_inset(s->rectangle, {pad, pad, pad, pad});
    const auto absolute = [&](UiRect r) noexcept {
        return UiRect{x + r.left, top + r.top, x + r.right, top + r.bottom};
    };
    // Header media and controls (relative to the card).
    if (!header_parts(s, s->panel, {0, 0, 0, 0}, ProfileAction::expand))
        return false;
    s->banner = absolute(s->banner);
    s->avatar = absolute(s->avatar);
    s->decoration = absolute(s->decoration);
    s->status = absolute(s->status);
    if (live && !self_card) {
        float bx = W - pad - 12 - 32;
        const float by = pad + 8;
        if (!icon(s, s->panel, {bx, by, bx + 32, by + 32}, UiIcon::ellipsis, ProfileAction::more, ProfileTone::normal,
                  ProfileLook::header_button))
            return false;
        bx -= 40;
        if (!bot(*s) && s->relation != 4) {
            const UiIcon glyph = s->relation == 1 ? UiIcon::user_check
                                 : s->relation >= 2 ? UiIcon::recent
                                                    : UiIcon::user_plus;
            if (!icon(s, s->panel, {bx, by, bx + 32, by + 32}, glyph,
                      s->relation ? ProfileAction::relationship : ProfileAction::add_friend, ProfileTone::normal,
                      ProfileLook::header_button))
                return false;
        }
    }
    if (!place(s, s->panel, {x - viewport.left, top - viewport.top, x - viewport.left + W, top - viewport.top + H}) ||
        !place(s, s->body, {0, body_top, W, maximum(body_top + 1, body_bottom)}))
        return false;
    s->scroll_viewport = absolute({0, body_top, W, body_bottom});
    s->content_height = body_content;
    s->scroll = minimum(s->scroll, maximum(0.f, s->content_height - ui_rect_height(s->scroll_viewport)));
    if (!scroll_container(s, s->body, s->scroll))
        return false;
    if (s->edit.active && !s->note_editing && s->message_field.generation) {
        s->edit_rectangle = absolute({x0, footer_top, x0 + content, footer_top + footer_height});
        s->edit_content = ui_rect_inset(s->edit_rectangle, {12, 2, 12, 2});
        s->message_placeholder = {};
        append(&s->message_placeholder, utf8_literal("Message @"));
        append(&s->message_placeholder, p.nickname.bytes ? p.nickname.view() : p.display_name.view());
        if (s->edit.focused)
            ui_widget_manager_focus(&s->widgets, s->message_field);
    }
    return finish(s, viewport);
}

// Body sections
float heading(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, Utf8View text) noexcept {
    return add(s, owner, {left, top, left + width, top + 16}, text, ProfileAction::none, kXs | text_style_bold,
               ProfileLook::plain, ProfileTone::subtle)
               ? 24.f
               : -1.f;
}
Utf8View platform_name(Utf8View type) noexcept {
    struct Name {
        Utf8View type, name;
    };
    static constexpr Name names[] = {
        {utf8_literal("battlenet"), utf8_literal("Battle.net")}, {utf8_literal("bluesky"), utf8_literal("Bluesky")},
        {utf8_literal("bungie"), utf8_literal("Bungie.net")},    {utf8_literal("crunchyroll"), utf8_literal("Crunchyroll")},
        {utf8_literal("domain"), utf8_literal("Domain")},        {utf8_literal("ebay"), utf8_literal("eBay")},
        {utf8_literal("epicgames"), utf8_literal("Epic Games")}, {utf8_literal("facebook"), utf8_literal("Facebook")},
        {utf8_literal("github"), utf8_literal("GitHub")},        {utf8_literal("instagram"), utf8_literal("Instagram")},
        {utf8_literal("leagueoflegends"), utf8_literal("League of Legends")},
        {utf8_literal("mastodon"), utf8_literal("Mastodon")},    {utf8_literal("paypal"), utf8_literal("PayPal")},
        {utf8_literal("playstation"), utf8_literal("PlayStation")},
        {utf8_literal("reddit"), utf8_literal("Reddit")},        {utf8_literal("riotgames"), utf8_literal("Riot Games")},
        {utf8_literal("roblox"), utf8_literal("Roblox")},        {utf8_literal("skype"), utf8_literal("Skype")},
        {utf8_literal("spotify"), utf8_literal("Spotify")},      {utf8_literal("steam"), utf8_literal("Steam")},
        {utf8_literal("tiktok"), utf8_literal("TikTok")},        {utf8_literal("twitch"), utf8_literal("Twitch")},
        {utf8_literal("twitter"), utf8_literal("X")},            {utf8_literal("xbox"), utf8_literal("Xbox")},
        {utf8_literal("youtube"), utf8_literal("YouTube")}};
    for (const auto& n : names)
        if (utf8_equal(n.type, type))
            return n.name;
    return type;
}
float member_since(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept {
    const auto& p = *s->profile;
    float h = heading(s, owner, left, top, width, utf8_literal("Member Since"));
    if (h < 0)
        return -1;
    auto& l = labels(s);
    const float y = top + h;
    float x = left;
    // A generic calendar icon marks the account date; no brand asset is shipped.
    if (!icon(s, owner, {x, y + 1, x + 16, y + 17}, UiIcon::calendar, ProfileAction::none))
        return -1;
    x += 20;
    l.member_since[0].bytes = 0;
    append_date(&l.member_since[0], ((p.user_id >> 22) + 1420070400000ull) / 1000);
    const float first = text_width(s, l.member_since[0].view(), width - (x - left), kSm);
    if (!add(s, owner, {x, y, x + first, y + 18}, l.member_since[0].view(), ProfileAction::none, kSm))
        return -1;
    x += first;
    u64 joined = 0;
    const auto* guild = s->state && p.guild_id ? chat_state_store_find_guild(*s->state, p.guild_id) : nullptr;
    if (guild && p.joined_at.bytes && parse_iso_timestamp({p.joined_at.view(), 0, false}, &joined)) {
        x += 8;
        if (!add(s, owner, {x, y + 7, x + 4, y + 11}, {}, ProfileAction::none, 0, ProfileLook::dot))
            return -1;
        x += 12;
        auto* glyph = add(s, owner, {x, y + 1, x + 16, y + 17});
        if (!glyph)
            return -1;
        glyph->media = ProfileMediaKind::member_guild;
        x += 20;
        l.member_since[1].bytes = 0;
        append_date(&l.member_since[1], joined / 1000);
        if (left + width > x &&
            !add(s, owner, {x, y, left + width, y + 18}, l.member_since[1].view(), ProfileAction::none, kSm))
            return -1;
    }
    return h + 18;
}
float connections(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept {
    const auto& p = *s->profile;
    if (!p.connection_count)
        return 0;
    float y = top + heading(s, owner, left, top, width, utf8_literal("Connections"));
    if (y < top)
        return -1;
    auto& l = labels(s);
    for (u32 i = 0; i < p.connection_count && i < 16; ++i) {
        const auto& c = p.connections[i];
        if (i)
            y += 16;
        // No platform icons are shipped: the box carries the platform initial.
        auto* box = add(s, owner, {left, y, left + 24, y + 24}, {}, ProfileAction::none, 0, ProfileLook::box);
        const auto platform = platform_name(c.type.view());
        l.connection_initial[i][0] = platform.bytes ? platform.data[0] : '?';
        if (l.connection_initial[i][0] >= 'a' && l.connection_initial[i][0] <= 'z')
            l.connection_initial[i][0] = static_cast<char>(l.connection_initial[i][0] - 32);
        auto* initial = box ? add(s, owner, {left, y, left + 24, y + 24}, {l.connection_initial[i], 1},
                                  ProfileAction::none, kXs | text_style_bold)
                            : nullptr;
        if (!initial)
            return -1;
        initial->centered = true;
        const float info = left + 32;
        const float name = text_width(s, c.name.view(), width - 32 - (c.verified ? 20.f : 0.f), kSm);
        if (!add(s, owner, {info, y, info + name, y + 18}, c.name.view(), ProfileAction::none, kSm))
            return -1;
        if (c.verified && !icon(s, owner, {info + name + 4, y + 1, info + name + 20, y + 17}, UiIcon::badge_check,
                                ProfileAction::none))
            return -1;
        auto& line = l.connection_line[i];
        line.bytes = 0;
        u64 created = 0;
        if (c.created_at.bytes && parse_iso_timestamp({c.created_at.view(), 0, false}, &created)) {
            append(&line, utf8_literal("Member since "));
            append_date(&line, created / 1000);
        } else
            append(&line, platform);
        if (!add(s, owner, {info, y + 22, left + width, y + 38}, line.view(), ProfileAction::none, kXs,
                 ProfileLook::plain, ProfileTone::subtle))
            return -1;
        y += 38;
    }
    return y - top;
}
float note_section(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept {
    if (!s->note_visible || self(*s))
        return 0;
    const float h = heading(s, owner, left, top, width, utf8_literal("Note (only visible to you)"));
    if (h < 0)
        return -1;
    s->note_offset = top;
    const float y = top + h;
    if (s->note_editing) {
        auto* field = add(s, owner, {left - 4, y, left + width, y + 34}, {}, ProfileAction::note, 0, ProfileLook::input);
        auto* widget = field ? ui_widget_manager_resolve(&s->widgets, field->widget) : nullptr;
        if (!widget)
            return -1;
        widget->kind = UiWidgetKind::text_field;
        s->note_field = field->widget;
        return h + 34;
    }
    const Utf8View text = s->note_text.bytes ? s->note_text
                          : s->note_loading ? utf8_literal("Loading Note")
                                            : utf8_literal("Click to add a note");
    const float height = maximum(18.f, measure(s, text, width, true, kSm).height);
    auto* note = add(s, owner, {left, y, left + width, y + height}, text,
                     s->note_loading ? ProfileAction::none : ProfileAction::note, kSm, ProfileLook::plain,
                     s->note_text.bytes ? ProfileTone::normal : ProfileTone::muted);
    if (!note)
        return -1;
    note->wrap = true;
    return h + height;
}
// Buttons row by relationship.
float modal_buttons(ProfileScene* s, UiWidgetHandle owner, float left, float top) noexcept {
    if (s->relation == 4)
        return 0;
    float x = left;
    const auto next = [&](float w) noexcept {
        if (w < 0)
            return false;
        x += w + 8;
        return true;
    };
    if (self(*s)) {
        if (!next(button(s, owner, x, top, utf8_literal("Edit Profile"), UiIcon::edit, ProfileAction::edit, true)))
            return -1;
    } else if (!bot(*s) && s->relation == 0) {
        if (!next(button(s, owner, x, top, utf8_literal("Add Friend"), UiIcon::user_plus, ProfileAction::add_friend, true)) ||
            !next(button(s, owner, x, top, {}, UiIcon::direct_message, ProfileAction::message, false)))
            return -1;
    } else {
        if (!next(button(s, owner, x, top, utf8_literal("Message"), UiIcon::direct_message, ProfileAction::message, true)))
            return -1;
        if (!bot(*s) && (s->relation == 1 || s->relation == 3) &&
            !next(button(s, owner, x, top, {}, s->relation == 1 ? UiIcon::user_check : UiIcon::recent,
                         ProfileAction::relationship, false)))
            return -1;
    }
    if (!next(button(s, owner, x, top, {}, UiIcon::ellipsis, ProfileAction::more, false)))
        return -1;
    return 32;
}
// The tab panel.
bool tab_panel(ProfileScene* s, UiRect panel, float bottom_pad, const u8* order, u32 cards) noexcept {
    const auto& p = *s->profile;
    auto& l = labels(s);
    const bool mutuals = !self(*s) && !bot(*s);
    Utf8View titles[3]{utf8_literal("Activity")};
    u32 count = 1;
    if (mutuals) {
        for (u32 i = 0; i < 2; ++i) {
            auto& text = l.tab_text[i];
            text.bytes = 0;
            const u32 n = i ? p.mutual_guild_count : p.mutual_friend_count;
            if (!n)
                append(&text, i ? utf8_literal("No Mutual Servers") : utf8_literal("No Mutual Friends"));
            else {
                append_number(&text, n);
                append(&text, i ? (n == 1 ? utf8_literal(" Mutual Server") : utf8_literal(" Mutual Servers"))
                                : (n == 1 ? utf8_literal(" Mutual Friend") : utf8_literal(" Mutual Friends")));
            }
            titles[count++] = text.view();
        }
    }
    if (s->tab >= count)
        s->tab = 0;
    const ProfileAction actions[] = {ProfileAction::activity, ProfileAction::friends, ProfileAction::servers};
    float x = panel.left;
    const float tab_top = panel.top + 20;
    for (u32 i = 0; i < count; ++i) {
        const float w = text_width(s, titles[i], panel.right - x, kSm | text_style_bold);
        if (w <= 0)
            break;
        auto* tab = add(s, s->panel, {x, tab_top, x + w, tab_top + 30}, {}, actions[i], 0, ProfileLook::tab);
        if (!tab || !add(s, s->panel, {x, tab_top, x + w + 1, tab_top + 14}, titles[i], ProfileAction::none,
                         kSm | text_style_bold, ProfileLook::plain,
                         i == s->tab ? (s->themed ? ProfileTone::strong : ProfileTone::link) : ProfileTone::subtle))
            return false;
        tab->source = static_cast<u16>(i);
        x += w + 24;
    }
    if (!add(s, s->panel, {panel.left, tab_top + 30, panel.right, tab_top + 31}, {}, ProfileAction::none, 0,
             ProfileLook::rule))
        return false;
    const UiRect list{panel.left, tab_top + 31 + 20, panel.right, panel.bottom - bottom_pad};
    if (!ui_rect_has_area(list))
        return true;
    s->secondary_viewport = {s->rectangle.left + list.left, s->rectangle.top + list.top,
                             s->rectangle.left + list.right, s->rectangle.top + list.bottom};
    auto* container = add(s, s->panel, list, {}, ProfileAction::none, 0, ProfileLook::plain, ProfileTone::normal, true);
    if (!container)
        return false;
    const float width = ui_rect_width(list) - 16;
    if (!s->tab) {
        float y = 0;
        // Cards 8 apart.
        for (u32 i = 0; i < cards; ++i) {
            const float used = activity_card(s, container->widget, 0, y, width, i, order[i], activity_now(*s),
                                             ProfileAction::none);
            if (used < 0)
                return false;
            if (used > 0)
                y += used + 8;
        }
        if (y > 0)
            y -= 8;
        else {
            if (!add(s, container->widget, {0, 0, width, 18}, utf8_literal("No current activity"), ProfileAction::none,
                     kSm, ProfileLook::plain, ProfileTone::muted))
                return false;
            y = 18;
        }
        s->secondary_height = y;
    } else {
        const bool friends = s->tab == 1;
        const u32 n = friends ? p.mutual_friends_count : p.mutual_guild_count;
        s->secondary_height = n * 50.f;
        s->secondary_scroll =
            maximum(0.f, minimum(s->secondary_scroll, maximum(0.f, s->secondary_height - ui_rect_height(list))));
        const u32 first = static_cast<u32>(s->secondary_scroll / 50),
                  last = minimum(n, first + static_cast<u32>(ui_rect_height(list) / 50) + 2);
        for (u32 i = first; i < last; ++i) {
            const auto& peer = friends ? p.mutual_friends[i] : p.mutual_guilds[i];
            Utf8View name = peer.name.view();
            if (!friends && s->state)
                if (const auto* guild = chat_state_store_find_guild(*s->state, peer.id))
                    name = chat_state_store_guild_name(*s->state, *guild);
            const float y = i * 50.f + 1;
            auto* row = add(s, container->widget, {0, y, width, y + 48}, {},
                            friends ? ProfileAction::open_friend : ProfileAction::open_server, 0, ProfileLook::row);
            if (!row)
                return false;
            row->source = static_cast<u16>(i);
            auto* face = add(s, container->widget, {4, y + 4, 44, y + 44});
            if (!face)
                return false;
            face->media = friends ? ProfileMediaKind::peer_avatar : ProfileMediaKind::peer_guild;
            face->source = static_cast<u16>(i);
            const auto* status =
                friends && s->users ? user_appearance_find_activity(s->users->appearance, peer.id) : nullptr;
            const bool second = status && status->activity.bytes;
            const float name_top = second ? y + 6 : y + 14;
            if (name.bytes &&
                !add(s, container->widget, {54, name_top, width - 4, name_top + 20}, name, ProfileAction::none, kMd))
                return false;
            if (second && !add(s, container->widget, {54, y + 26, width - 4, y + 42}, status->activity.view(),
                               ProfileAction::none, kXs, ProfileLook::plain, ProfileTone::subtle))
                return false;
        }
        if (!n && !add(s, container->widget, {0, 0, width, 18},
                       friends ? utf8_literal("No Mutual Friends") : utf8_literal("No Mutual Servers"),
                       ProfileAction::none, kSm, ProfileLook::plain, ProfileTone::muted))
            return false;
    }
    s->secondary_scroll =
        maximum(0.f, minimum(s->secondary_scroll, maximum(0.f, s->secondary_height - ui_rect_height(list))));
    return scroll_container(s, container->widget, s->secondary_scroll);
}

// The full profile modal.
bool layout_modal(ProfileScene* s, UiRect viewport) noexcept {
    const float vw = ui_rect_width(viewport), vh = ui_rect_height(viewport);
    const bool wide = vw >= 929, tall = vh >= 785;
    const float mw = wide ? minimum(962.f, vw - 96) : vw;
    float mh = tall ? minimum(800.f, vh - 96) : vh;
    mh = maximum(mh, minimum(550.f, vh));
    s->modal_rounded = wide && tall;
    const float x = viewport.left + (vw - mw) * .5f, top = viewport.top + (vh - mh) * .5f;
    s->rectangle = {x, top, x + mw, top + mh};
    const float pad = vw >= 1025 ? 48.f : 32.f;
    const float card_w = minimum(maximum(300.f, .455f * vw - (wide ? .91f * 48 : 0.f)), 400.f);
    const float cp = card_w <= 380 ? 24.f : 32.f;
    const float size = card_w <= 350 ? 96.f : 120.f;
    auto* panel = add(s, s->root, {x - viewport.left, top - viewport.top, x - viewport.left + mw, top - viewport.top + mh},
                      {}, ProfileAction::none, 0, ProfileLook::plain, ProfileTone::normal, true);
    if (!panel)
        return false;
    s->panel = panel->widget;
    const UiRect card{pad, pad, pad + card_w, mh};
    s->card = {x + card.left, top + card.top, x + card.right, top + card.bottom};
    s->effect_area = s->card;
    header_geometry(s, {card.left, card.top, card.right, card.top + kModalBanner}, card.left + cp,
                    card.top + kModalBanner - size * .5f, size);
    // The card's header: banner, avatar, decoration, then the status bubble.
    if (!header_parts(s, s->panel, {0, 0, 0, 0}, ProfileAction::none))
        return false;
    const float bubble_left = card.left + cp + size + 20, bubble_top = card.top + kModalBanner - 8;
    u8 order[kUserActivityMaximum];
    const u32 cards = activity_cards(s, order);
    const float bubble = status_bubble(s, s->panel, bubble_left, bubble_top, card.right - 16 - bubble_left);
    if (bubble < 0)
        return false;
    const float header_bottom = maximum(s->avatar.bottom + 12, bubble ? bubble_top + bubble + 8 : 0.f);
    // Card body: gap 24, padding 4 cp cp (end cp - 8), scrolled with fade.
    auto* body = add(s, s->panel, {card.left, header_bottom, card.right, mh}, {}, ProfileAction::none, 0,
                     ProfileLook::plain, ProfileTone::normal, true);
    if (!body)
        return false;
    s->body = body->widget;
    const float content = card_w - cp - (cp - 8);
    float y = 0;
    const auto section = [&](float height) noexcept {
        if (height > 0)
            y += height + 24;
        return height >= 0;
    };
    if (!section(name_block(s, s->body, cp, y, content, true)))
        return false;
    if (s->phase == ProfilePhase::failed &&
        !section(add(s, s->body, {cp, y, cp + content, y + 18}, utf8_literal("Unable to load parts of profile"),
                     ProfileAction::retry, kSm, ProfileLook::plain, ProfileTone::muted)
                     ? 18.f
                     : -1.f))
        return false;
    if (!section(notices(s, s->body, cp, y, content, true)) ||
        !section(interactive(*s) ? modal_buttons(s, s->body, cp, y) : 0.f) ||
        !section(biography(s, s->body, cp, y, content, false)) || !section(member_since(s, s->body, cp, y, content)))
        return false;
    if (s->profile->guild_id && s->state) {
        const float before = y;
        const float h = heading(s, s->body, cp, y, content, utf8_literal("Roles"));
        const float r = h < 0 ? -1.f : roles(s, s->body, cp, y + h, content, false);
        if (r < 0)
            return false;
        if (r == 0) {
            // No roles to show: drop the heading again.
            --s->part_count;
            y = before;
        } else
            y += h + r + 24;
    }
    if (!section(plugin_section(s, s->body, cp, y, content)) || !section(connections(s, s->body, cp, y, content)) ||
        !section(note_section(s, s->body, cp, y, content)))
        return false;
    s->content_height = (y > 0 ? y - 24 : 0) + cp;
    s->scroll_viewport = {s->card.left, top + header_bottom, s->card.right, s->card.bottom};
    if (s->scroll_to_note) {
        s->scroll_to_note = false;
        s->scroll = s->note_offset - 8;
    }
    s->scroll = maximum(0.f, minimum(s->scroll, maximum(0.f, s->content_height - ui_rect_height(s->scroll_viewport))));
    if (!scroll_container(s, s->body, s->scroll))
        return false;
    // Right: the tab panel beside the card (gutter 32).
    s->secondary_viewport = {};
    s->secondary_height = 0;
    const UiRect panel_rect{card.right + 32, pad, mw - (pad - 16), mh};
    if (ui_rect_width(panel_rect) > 120 && !tab_panel(s, panel_rect, wide ? 16.f : 32.f, order, cards))
        return false;
    // Close (X): top 8, right 8.
    if (interactive(*s) && !icon(s, s->panel, {mw - 40, 8, mw - 8, 40}, UiIcon::close, ProfileAction::close,
                                 ProfileTone::normal, ProfileLook::header_button))
        return false;
    const auto absolute = [&](UiRect r) noexcept {
        return UiRect{x + r.left, top + r.top, x + r.right, top + r.bottom};
    };
    s->banner = absolute(s->banner);
    s->avatar = absolute(s->avatar);
    s->decoration = absolute(s->decoration);
    s->status = absolute(s->status);
    s->preview_height = mh;
    return finish(s, viewport);
}
} // namespace

const ProfilePart* profile_scene_part(const ProfileScene& scene, UiWidgetHandle handle) noexcept {
    const auto* widget = ui_widget_manager_resolve(scene.widgets, handle);
    if (!widget || widget->id < 2 || widget->id - 2 >= scene.part_count)
        return nullptr;
    const auto& part = scene.parts[widget->id - 2];
    return ui_widget_handle_equal(part.widget, handle) ? &part : nullptr;
}
float profile_scene_intro_offset(const ProfileScene& s, u64 now) noexcept {
    if (s.presentation != ProfilePresentation::popout || s.reduced_motion || !s.opened_ms || now < s.opened_ms)
        return 0;
    const float t = static_cast<float>(now - s.opened_ms) / 200.f;
    return t >= 1 ? 0.f : 10.f * (1.f - ui_motion_curve_sample(UiMotionCurve::css_ease_out, t));
}
float profile_scene_effect_opacity(const ProfileScene& s, u64 now) noexcept {
    // The effect dims to .35 over 600 ms while the card is hovered.
    float t = 1;
    if (!s.reduced_motion && s.hover_changed_ms && now < s.hover_changed_ms + 600)
        t = static_cast<float>(now - minimum(now, s.hover_changed_ms)) / 600.f;
    const float from = s.hovered ? 1.f : .35f, to = s.hovered ? .35f : 1.f;
    return from + (to - from) * ui_motion_curve_sample(UiMotionCurve::linear, t);
}
bool profile_scene_open(ProfileScene* s, const UserProfile* p, const ChatStateStore* state, const UserStore* users,
                        u64 self_id, UiRect anchor, UiRect viewport) noexcept {
    if (!s || !p || !p->user_id || !ui_rect_has_area(viewport))
        return false;
    if (!s->storage.base && !arena_initialize(&s->storage, sizeof(Storage), sizeof(Storage), 4096))
        return false;
    auto* storage = reinterpret_cast<Storage*>(s->storage.base);
    if (!ui_widget_manager_initialize(&s->widgets, {storage->widgets, capacity * 2}, {storage->nodes, capacity * 2}))
        return false;
    s->parts = storage->parts;
    s->profile = p;
    s->state = state;
    s->users = users;
    s->self_id = self_id;
    s->anchor = anchor;
    s->active = true;
    ++s->opens;
    s->bio_signature = 0;
    s->bio_revealed_count = 0;
    s->bio_pressed = 0;
    s->banner_compatibility = false;
    s->roles_expanded = false;
    s->hovered = false;
    s->hover_changed_ms = 0;
    s->scroll = 0;
    s->secondary_scroll = 0;
    s->name_started_ms = clock_monotonic_milliseconds();
    s->tab = 0;
    return profile_scene_layout(s, viewport, true);
}
bool profile_scene_layout(ProfileScene* s, UiRect viewport, bool force) noexcept {
    if (!s || !s->active || !s->profile || !ui_rect_has_area(viewport))
        return false;
    if (!force && ui_rect_equal(s->viewport, viewport) && s->users_revision == (s->users ? s->users->revision : 0) &&
        s->permission_revision == (s->state ? s->state->permission_mutations : 0))
        return true;
    s->users_revision = s->users ? s->users->revision : 0;
    s->permission_revision = s->state ? s->state->permission_mutations : 0;
    s->viewport = viewport;
    s->part_count = 0;
    s->text_hit_count = 0;
    ui_widget_manager_reset(&s->widgets);
    s->bio_widget = s->name_widget = s->message_field = s->note_field = ui_widget_invalid_handle();
    s->secondary_viewport = {};
    s->secondary_height = 0;
    s->activity_timed = false;
    s->activity_set = nullptr;
    s->missing_icon_count = 0;
    labels(s).timer_count = 0;
    s->activity_clock_s = activity_now(*s) / 1000;
    s->intro_offset = profile_scene_intro_offset(*s, clock_monotonic_milliseconds());
    resolve_theme(s);
    s->can_manage_roles = false;
    if (s->state && s->profile->guild_id) {
        const auto policy = role_edit_policy(*s->state, s->profile->guild_id);
        for (u32 i = 0; i < s->state->role_count && !s->can_manage_roles; ++i)
            s->can_manage_roles = s->state->roles[i].guild_id == s->profile->guild_id &&
                                  role_editable(policy, s->state->roles[i]);
    }
    s->note_editing = s->note_editing && s->note_visible && !self(*s) && modal(*s);
    s->edit.active = interactive(*s) && (s->note_editing || (!modal(*s) && can_message(*s)));
    auto root_style = ui_layout_style();
    root_style.flow = UiLayoutFlow::overlay;
    root_style.width = root_style.height = ui_layout_fill();
    root_style.flags = ui_layout_node_visible | ui_layout_node_allow_overlap;
    const auto root = ui_widget_manager_add(&s->widgets, ui_widget_invalid_handle(),
                                            {1,
                                             UiWidgetKind::container,
                                             ui_widget_enabled,
                                             {utf8_literal("UserProfile"), root_style, {}, 0, 1}});
    if (!root.ok())
        return false;
    s->root = root.handle;
    if (!(modal(*s) ? layout_modal(s, viewport) : layout_popout(s, viewport)))
        return false;
    // Timers tick only while one shows (the Activity tab, not scrolled away).
    const auto& l = labels(s);
    for (u32 i = 0; i < l.timer_count && !s->activity_timed; ++i) {
        const auto& t = l.timers[i];
        const u16 shown[] = {t.clock, t.bar};
        for (const u16 part : shown) {
            const auto* node = part < s->part_count ? ui_widget_manager_layout_node(s->widgets, s->parts[part].widget)
                                                    : nullptr;
            if (node && ui_rect_has_area(ui_rect_intersection(
                            node->rectangle, ui_rect_intersection(node->clip_rectangle, s->rectangle))))
                s->activity_timed = true;
        }
    }
    return true;
}
bool profile_scene_tick(ProfileScene* s, u64 now) noexcept {
    if (!s || !s->active || !s->activity_set || !s->activity_timed || now / 1000 == s->activity_clock_s)
        return false;
    s->activity_clock_s = now / 1000;
    auto& l = labels(s);
    bool relayout = false, changed = false;
    // Same-length text is rewritten in place (time digits keep their width);
    // a new length or a timer that ends lays the card out again.
    const auto rewrite = [&](u16 part, ProfileString<16>* stored, Utf8View text) noexcept {
        if (part >= s->part_count || text.bytes != stored->bytes) {
            relayout = true;
            return;
        }
        if (!bytes_equal(stored->data, text.data, text.bytes)) {
            copy_bytes(stored->data, text.data, text.bytes);
            changed = true;
        }
    };
    for (u32 i = 0; i < l.timer_count && !relayout; ++i) {
        const auto& t = l.timers[i];
        const auto& a = l.activity_set.items[t.item];
        char text[16];
        if (t.clock != kNoPart) {
            const auto clock = profile_activity_clock(a, now);
            if (clock.timer == ProfileActivityTimer::none || t.glyph >= s->part_count) {
                relayout = true;
                break;
            }
            const auto glyph = chip_icon(clock.timer == ProfileActivityTimer::countdown
                                             ? ProfileActivityGlyph::timer
                                             : l.activity_content[t.card].clock_glyph);
            if (s->parts[t.glyph].icon != static_cast<u16>(glyph)) {
                s->parts[t.glyph].icon = static_cast<u16>(glyph);
                changed = true;
            }
            rewrite(t.clock, &l.activity_time[t.card][0],
                    {text, profile_activity_clock_text(clock.seconds, {text, sizeof(text)})});
        }
        ProfileActivityProgress progress{};
        if (t.bar != kNoPart && t.bar < s->part_count && profile_activity_progress(a, now, &progress)) {
            rewrite(t.elapsed, &l.activity_time[t.card][1],
                    {text, profile_activity_track_text(progress.elapsed, {text, sizeof(text)})});
            auto& bar = s->parts[t.bar].color.red;
            changed |= bar != progress.fraction;
            bar = progress.fraction;
        }
    }
    if (relayout) {
        l.tick_ms = now;
        const bool ok = profile_scene_layout(s, s->viewport, true);
        l.tick_ms = 0;
        return ok;
    }
    if (changed)
        ++s->revision;
    return changed;
}
Utf8View profile_scene_hint(const ProfileScene& s, UiWidgetHandle widget, UiRect* anchor) noexcept {
    const auto* part = s.active ? profile_scene_part(s, widget) : nullptr;
    const auto* node = part && part->hint.bytes ? ui_widget_manager_layout_node(s.widgets, part->widget) : nullptr;
    if (!node)
        return {};
    const auto visible = ui_rect_intersection(node->rectangle, ui_rect_intersection(node->clip_rectangle, s.rectangle));
    if (!ui_rect_has_area(visible))
        return {};
    if (anchor)
        *anchor = visible;
    return part->hint;
}
namespace {
bool read_profile_scroll(void* context, u64 key, UiScrollRange* range) noexcept {
    const auto* s = static_cast<ProfileScene*>(context);
    if (!s || !s->active)
        return false;
    const auto viewport = key ? s->secondary_viewport : s->scroll_viewport;
    if (!ui_rect_has_area(viewport))
        return false;
    *range = {viewport,
              key ? s->secondary_scroll : s->scroll,
              maximum(0.f, (key ? s->secondary_height : s->content_height) - ui_rect_height(viewport)),
              44,
              1,
              0,
              s->name_started_ms};
    return true;
}
bool write_profile_scroll(void* context, u64 key, double offset, bool* changed) noexcept {
    UiScrollRange before{};
    if (!read_profile_scroll(context, key, &before))
        return false;
    *changed = before.offset != offset;
    return profile_scene_scroll(static_cast<ProfileScene*>(context), static_cast<float>(offset - before.offset),
                                key != 0);
}
} // namespace
UiScrollBinding profile_scene_scroll_binding(ProfileScene* s, bool secondary) noexcept {
    return {s, secondary ? 1ull : 0ull, read_profile_scroll, write_profile_scroll};
}
bool profile_scene_scroll(ProfileScene* s, float delta, bool right) noexcept {
    if (s && right && ui_rect_has_area(s->secondary_viewport)) {
        s->secondary_scroll = maximum(
            0.f, minimum(s->secondary_scroll + delta,
                         maximum(0.f, s->secondary_height - ui_rect_height(s->secondary_viewport))));
        return profile_scene_layout(s, s->viewport, true);
    }
    if (!s || !s->active || !ui_coordinate_valid(delta))
        return false;
    const float next = maximum(
        0.f, minimum(s->scroll + delta, maximum(0.f, s->content_height - ui_rect_height(s->scroll_viewport))));
    if (next == s->scroll)
        return true;
    s->scroll = next;
    return profile_scene_layout(s, s->viewport, true);
}
void profile_scene_release(ProfileScene* s) noexcept {
    if (s) {
        arena_release(&s->storage);
        *s = {};
    }
}
void profile_scene_publish_text_hit(const ProfileScene& scene, UiWidgetReference owner, UiRect layout, UiRect ink,
                                    UiRect clip) noexcept {
    if (!scene.active || owner.manager != &scene.widgets || !inline_text_part(scene, owner.handle))
        return;
    const auto* node = ui_widget_manager_layout_node(scene.widgets, owner.handle);
    if (!node || !ui_rect_equal(node->rectangle, layout))
        return;
    u32 slot = 0;
    while (slot < scene.text_hit_count && !ui_widget_handle_equal(scene.text_hits[slot].owner, owner.handle))
        ++slot;
    if (slot == sizeof(scene.text_hits) / sizeof(scene.text_hits[0]))
        return;
    if (slot == scene.text_hit_count)
        ++scene.text_hit_count;
    scene.text_hits[slot] = {owner.handle, layout,
                             ui_rect_intersection(ui_rect_intersection(ink, clip), scene.rectangle)};
}
UiWidgetHandle profile_scene_hit_test(const ProfileScene& scene, float x, float y) noexcept {
    if (!scene.active || !interactive(scene) || !ui_rect_contains(scene.rectangle, x, y))
        return ui_widget_invalid_handle();
    const auto target = ui_widget_manager_hit_test(scene.widgets, x, y);
    if (!inline_text_part(scene, target))
        return target;
    const auto* node = ui_widget_manager_layout_node(scene.widgets, target);
    for (u32 i = 0; node && i < scene.text_hit_count; ++i) {
        const auto& hit = scene.text_hits[i];
        if (ui_widget_handle_equal(hit.owner, target) && ui_rect_equal(node->rectangle, hit.layout) &&
            ui_rect_contains(ui_rect_intersection(hit.ink, node->clip_rectangle), x, y))
            return target;
    }
    return ui_widget_invalid_handle();
}
ProfileAction profile_scene_action(const ProfileScene& s, UiWidgetHandle widget) noexcept {
    const auto* part = profile_scene_part(s, widget);
    return part ? part->action : ProfileAction::none;
}
namespace {
bool hovered(const ProfileScene& s, const ProfilePart& p) noexcept {
    return p.action != ProfileAction::none && ui_widget_handle_equal(s.widgets.hovered, p.widget);
}
// A popout activity card lights up under any of its parts (they share its click).
bool card_hot(const ProfileScene& s, const ProfilePart& card) noexcept {
    if (hovered(s, card))
        return true;
    const auto* part = card.action == ProfileAction::card ? profile_scene_part(s, s.widgets.hovered) : nullptr;
    return part && part->source == card.source &&
           (part->media == ProfileMediaKind::activity_large || part->media == ProfileMediaKind::activity_small ||
            part->media == ProfileMediaKind::activity_text || part->media == ProfileMediaKind::activity_action);
}
// The opaque colour under an activity card at y: its fill over the popout's
// surface (a Nitro theme's gradient) or the full profile's background.
Color card_backdrop(const ProfileScene& s, const Palette& c, float y, bool hot) noexcept {
    const auto r = s.rectangle;
    Color base = c.surface;
    if (modal(s)) {
        base = c.modal;
        if (s.themed) {
            const float low = .2f * c.overlay.alpha, high = .8f * c.overlay.alpha;
            Color veil = c.overlay;
            veil.alpha = low;
            base = over(veil, base);
            veil.alpha = (high - low) / (1 - low) * (1 - maximum(0.f, minimum(1.f, (y - r.top) / 300)));
            base = over(veil, base);
        }
    } else if (s.themed) {
        const float split = r.top + 2 + kPopoutBanner;
        const float t = y <= split || r.bottom <= split ? 0.f : minimum(1.f, (y - split) / (r.bottom - split));
        base = over(c.overlay, mix(c.primary, c.secondary, t));
    }
    return over(hot ? c.card_hover : c.card, base);
}
// The rounded-top banner fill (its bottom corners square).
bool banner_fill(PaintStream* paint, UiRect r, float radius, Color color) noexcept {
    if (!radius)
        return paint_stream_push(paint, r.left, r.top, r.right, r.bottom, color);
    return paint_stream_push_rounded(paint, r.left, r.top, r.right, minimum(r.bottom, r.top + radius * 2), color,
                                     radius) &&
           paint_stream_push(paint, r.left, r.top + radius, r.right, r.bottom, color);
}
// Thought bubble: two trailing circles and the rounded body.
bool bubble_paint(PaintStream* paint, UiRect r, const Palette& c, float radius) noexcept {
    const UiRect big{r.left + 10, r.top - 8, r.left + 30, r.top + 12}, small{r.left - 3, r.top - 15, r.left + 7, r.top - 5};
    const float small_radius = ui_rect_width(small) * .5f, big_radius = ui_rect_width(big) * .5f;
    return paint_stream_push_rounded_border(paint, small.left, small.top, small.right, small.bottom, c.bubble,
                                            c.bubble_border, small_radius, 1) &&
           paint_stream_push_rounded_border(paint, big.left, big.top, big.right, big.bottom, c.bubble,
                                            c.bubble_border, big_radius, 1) &&
           paint_stream_push_rounded_border(paint, r.left, r.top, r.right, r.bottom, c.bubble, c.bubble_border,
                                            minimum(radius, ui_rect_height(r) * .5f), 1) &&
           // The body covers the big circle's border where they join.
           paint_stream_push_rounded(paint, big.left + 1, r.top + 1, big.right - 1, big.bottom - 1, c.bubble,
                                     big_radius - 1);
}
bool card_background(const ProfileScene& s, const Theme& theme, const Palette& c, PaintStream* paint) noexcept {
    const float radius = theme.scale.radius_md;
    const auto r = s.rectangle;
    if (!s.themed)
        return ui_surface_paint(paint, r, theme, UiElevation::popover, radius, &c.surface, &theme.tokens.border.subtle);
    // A 2 DIP gradient border: P down to the banner's end, then a blend to S;
    // the inner card is the 60% overlay over it.
    const auto inner = profile_scene_card_mask(s, theme);
    const float split = minimum(r.bottom, r.top + 2 + kPopoutBanner);
    return ui_surface_shadow_paint(paint, r, theme, UiElevation::popover, radius) &&
           paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, c.primary, radius) &&
           (split >= r.bottom || paint_stream_push_gradient(paint, {r.left, split, r.right, r.bottom}, c.primary,
                                                            c.secondary, radius)) &&
           paint_stream_push_rounded(paint, inner.rectangle.left, inner.rectangle.top, inner.rectangle.right,
                                     inner.rectangle.bottom, c.overlay, inner.radius);
}
bool modal_background(const ProfileScene& s, const Theme& theme, const Palette& c, PaintStream* paint) noexcept {
    const float radius = s.modal_rounded ? theme.scale.radius_xl : 0.f;
    const auto r = s.rectangle;
    if (!paint_stream_push(paint, s.viewport.left, s.viewport.top, s.viewport.right, s.viewport.bottom,
                           theme.tokens.overlay.scrim_strong) ||
        !paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, c.modal, radius))
        return false;
    if (s.themed) {
        // Inner veil: the overlay at 80% alpha fading to 20% over the top 300 DIPs.
        Color veil = c.overlay;
        const float low = .2f * veil.alpha, high = .8f * veil.alpha;
        Color constant = veil, top = veil, clear = veil;
        constant.alpha = low;
        top.alpha = (high - low) / (1 - low);
        clear.alpha = 0;
        if (!paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, constant, radius) ||
            !paint_stream_push_gradient(paint, {r.left, r.top, r.right, minimum(r.bottom, r.top + 300)}, top, clear,
                                        radius))
            return false;
    }
    // The profile card runs to the modal's bottom edge: radius 16 16 0 0.
    const auto card = s.card;
    const auto mask = profile_scene_card_mask(s, theme);
    const float card_radius = mask.radius, bottom = mask.rectangle.bottom;
    if (!paint_stream_push_clip(paint, card.left - 1, card.top - 1, card.right + 1, card.bottom))
        return false;
    bool ok = paint_stream_push_rounded_border(paint, card.left - 1, card.top - 1, card.right + 1, bottom + 1,
                                               theme.tokens.border.subtle, theme.tokens.border.subtle, card_radius + 1, 1);
    if (!s.themed)
        ok = ok && paint_stream_push_rounded(paint, card.left, card.top, card.right, bottom, c.surface, card_radius);
    else {
        const float split = minimum(card.bottom, card.top + kModalBanner);
        ok = ok && paint_stream_push_rounded(paint, card.left, card.top, card.right, bottom, c.primary, card_radius) &&
             (split >= card.bottom ||
              paint_stream_push_gradient(paint, {card.left, split, card.right, bottom}, c.primary, c.secondary,
                                         card_radius)) &&
             paint_stream_push_rounded(paint, card.left, card.top, card.right, bottom, c.overlay, card_radius);
    }
    return paint_stream_pop_clip(paint) && ok;
}
} // namespace
ProfileCardMask profile_scene_card_mask(const ProfileScene& s, const Theme& theme) noexcept {
    if (modal(s)) {
        const float radius = theme.scale.radius_xl;
        return {{s.card.left, s.card.top, s.card.right, s.card.bottom + radius}, radius};
    }
    if (!s.themed)
        return {s.rectangle, theme.scale.radius_md};
    return {ui_rect_inset(s.rectangle, {2, 2, 2, 2}), maximum(0.f, theme.scale.radius_md - 2)};
}
bool profile_scene_paint(const ProfileScene& s, const Theme& theme, PaintStream* paint) noexcept {
    if (!s.active)
        return true;
    const auto c = palette(s, theme);
    const bool large = modal(s);
    if (!(large ? modal_background(s, theme, c, paint) : card_background(s, theme, c, paint)))
        return false;
    if (!paint_stream_push_clip(paint, s.rectangle.left, s.rectangle.top, s.rectangle.right, s.rectangle.bottom))
        return false;
    // Banner colour (the first theme colour, else the accent) under its image, and the
    // avatar's cut-out disc in the card colour.
    bool ok = paint_stream_push_clip(paint, s.card.left, s.card.top, s.card.right, s.card.bottom) &&
              banner_fill(paint, s.banner, profile_scene_card_mask(s, theme).radius, c.banner);
    const float ring = s.avatar_stroke, half = ui_rect_width(s.avatar) * .5f + ring;
    ok = ok && paint_stream_push_rounded(paint, s.avatar.left - ring, s.avatar.top - ring, s.avatar.right + ring,
                                         s.avatar.bottom + ring, c.surface, half);
    ok = paint_stream_pop_clip(paint) && ok;
    const float card_radius = large ? theme.scale.radius_lg : theme.scale.radius_md;
    // Activity cards round so their artwork and buttons stay concentric:
    // inner radius = card radius - padding.
    const float activity_radius = large ? theme.scale.radius_xl : theme.scale.radius_lg;
    const float inner_radius = maximum(0.f, activity_radius - card_metrics(large).pad);
    s.activity_art_radius = inner_radius;
    for (u32 i = 0; ok && i < s.part_count; ++i) {
        const auto& p = s.parts[i];
        if (p.look == ProfileLook::plain && !p.badge.owner.generation && p.media != ProfileMediaKind::activity_large &&
            p.media != ProfileMediaKind::activity_progress)
            continue;
        if (p.look == ProfileLook::header_button || p.look == ProfileLook::icon)
            continue;
        const auto* n = ui_widget_manager_layout_node(s.widgets, p.widget);
        if (!n)
            return false;
        const auto r = n->rectangle;
        const auto visible = ui_rect_intersection(n->clip_rectangle, s.rectangle);
        if (!ui_rect_has_area(ui_rect_intersection(r, visible)))
            continue;
        if (p.badge.owner.generation) {
            const Theme local = card_theme(theme, c);
            ok = p.media == ProfileMediaKind::plugin_slot
                     ? ui_plugin_slot_badge_paint({p.badge, p.source, static_cast<u8>(p.style), 0, 0}, s.widgets, local,
                                                  paint)
                     : ui_badge_paint(p.badge, s.widgets, local, paint, false);
            continue;
        }
        if (!paint_stream_push_clip(paint, visible.left, visible.top, visible.right, visible.bottom))
            return false;
        const bool hot = hovered(s, p);
        switch (p.look) {
        case ProfileLook::card: {
            const bool activity = p.media == ProfileMediaKind::activity_card;
            ok = paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom,
                                           (activity ? card_hot(s, p) : hot) ? c.card_hover : c.card,
                                           activity ? activity_radius : card_radius);
            break;
        }
        case ProfileLook::peek: {
            // Only the strip below the first card shows.
            const float cut = r.top + 8;
            ok = paint_stream_push_clip(paint, r.left, cut, r.right, r.bottom) &&
                 paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, hot ? c.card_hover : c.card,
                                           activity_radius) &&
                 paint_stream_pop_clip(paint);
            break;
        }
        case ProfileLook::row:
            ok = !hot || paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, c.hover, theme.scale.radius_sm);
            break;
        case ProfileLook::role: {
            const Color border = c.border;
            const float cy = (r.top + r.bottom) * .5f;
            ok = paint_stream_push_rounded_border(paint, r.left, r.top, r.right, r.bottom, Color{}, border,
                                                  theme.scale.radius_md, 1) &&
                 paint_stream_push_rounded(paint, r.left + 6, cy - 6, r.left + 18, cy + 6,
                                           p.color.alpha ? p.color : c.subtle, theme.scale.radius_full);
            break;
        }
        case ProfileLook::pill:
            ok = paint_stream_push_rounded_border(paint, r.left, r.top, r.right, r.bottom, hot ? c.hover : Color{},
                                                  c.border, theme.scale.radius_md, 1);
            break;
        case ProfileLook::tab:
            if (p.source == s.tab || hot) {
                const Color bar = p.source == s.tab ? (s.themed ? c.strong : theme.tokens.text.link) : c.subtle;
                ok = paint_stream_push_rounded(paint, r.left, r.bottom - 2, r.right, r.bottom, bar, theme.scale.radius_xs);
            }
            break;
        case ProfileLook::dot:
            ok = paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, large ? c.subtle : c.text,
                                           theme.scale.radius_full);
            break;
        case ProfileLook::badge:
            ok = paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, c.border, theme.scale.radius_full);
            break;
        case ProfileLook::rule:
            ok = paint_stream_push(paint, r.left, r.top, r.right, r.bottom, c.border);
            break;
        case ProfileLook::input:
            ok = paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, c.input, theme.scale.radius_md);
            break;
        case ProfileLook::primary:
            ok = paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, hot ? c.accent_hover : c.accent,
                                           theme.scale.radius_md);
            break;
        case ProfileLook::secondary: {
            // Ask to Join stays disabled: no hover.
            const bool lit = hot && p.action != ProfileAction::activity_join;
            ok = paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, lit ? c.button_hover : c.button,
                                           p.media == ProfileMediaKind::activity_action ? inner_radius
                                                                                        : theme.scale.radius_md);
            break;
        }
        case ProfileLook::bubble:
            ok = bubble_paint(paint, r, c, theme.scale.radius_xl);
            break;
        case ProfileLook::box:
            ok = paint_stream_push_rounded_border(paint, r.left, r.top, r.right, r.bottom, c.card, c.border,
                                                  theme.scale.radius_md, 1);
            break;
        default:
            break;
        }
        if (ok && p.media == ProfileMediaKind::activity_large)
            // Artwork placeholder; the image draws over it once decoded.
            ok = paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, c.card, inner_radius) &&
                 ui_icon_paint(paint, UiIcon::activity,
                               ui_rect_inset(r, {ui_rect_width(r) * .3f, ui_rect_height(r) * .3f,
                                                 ui_rect_width(r) * .3f, ui_rect_height(r) * .3f}),
                               c.muted);
        else if (ok && p.media == ProfileMediaKind::activity_progress) {
            const float fill = r.left + maximum(4.f, ui_rect_width(r) * maximum(0.f, minimum(1.f, p.color.red)));
            ok = paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, c.border, theme.scale.radius_xs) &&
                 paint_stream_push_rounded(paint, r.left, r.top, minimum(fill, r.right), r.bottom, c.strong,
                                           theme.scale.radius_xs);
        }
        if (!paint_stream_pop_clip(paint))
            return false;
    }
    if (ok)
        ok = ui_scrollbar_paint(paint, ui_scrollbar_geometry(s.scroll_viewport, s.scroll, s.content_height), theme) &&
             (!ui_rect_has_area(s.secondary_viewport) ||
              ui_scrollbar_paint(paint, ui_scrollbar_geometry(s.secondary_viewport, s.secondary_scroll, s.secondary_height),
                                 theme));
    return paint_stream_pop_clip(paint) && ok;
}
bool profile_scene_foreground(const ProfileScene& s, const Theme& theme, PaintStream* paint,
                              const UiEditGeometry* edit, u32 dpi, u32* controls_first_batch) noexcept {
    if (controls_first_batch)
        *controls_first_batch = paint->batch_count;
    if (!s.active)
        return true;
    const auto c = palette(s, theme);
    if (!paint_stream_push_clip(paint, s.rectangle.left, s.rectangle.top, s.rectangle.right, s.rectangle.bottom))
        return false;
    bool ok = true;
    bool decoration = false;
    for (u32 i = 0; i < s.part_count; ++i)
        decoration |= s.parts[i].media == ProfileMediaKind::decoration;
    // Over the banner image: the avatar's cut-out ring (a decoration covers
    // it) and the status bubble where it overlaps the banner.
    const auto banner = ui_rect_intersection(s.banner, s.card);
    if (ui_rect_has_area(banner)) {
        ok = paint_stream_push_clip(paint, banner.left, banner.top, banner.right, banner.bottom);
        const float ring = s.avatar_stroke, half = ui_rect_width(s.avatar) * .5f + ring;
        if (ok && !decoration)
            ok = paint_stream_push_rounded_border(paint, s.avatar.left - ring, s.avatar.top - ring, s.avatar.right + ring,
                                                  s.avatar.bottom + ring, Color{}, c.surface, half, ring);
        for (u32 i = 0; ok && i < s.part_count; ++i)
            if (s.parts[i].look == ProfileLook::bubble)
                if (const auto* n = ui_widget_manager_layout_node(s.widgets, s.parts[i].widget))
                    ok = bubble_paint(paint, n->rectangle, c, theme.scale.radius_xl);
        ok = paint_stream_pop_clip(paint) && ok;
    }
    // The avatar's hover veil (it opens the full profile).
    for (u32 i = 0; ok && i < s.part_count; ++i) {
        const auto& p = s.parts[i];
        if (p.media == ProfileMediaKind::avatar && hovered(s, p))
            ok = paint_stream_push_rounded(paint, s.avatar.left, s.avatar.top, s.avatar.right, s.avatar.bottom,
                                           theme.tokens.overlay.shadow, ui_rect_width(s.avatar) * .5f);
    }
    // Status: the cut-out (a phone shape when mobile), then the dot.
    if (ok && ui_rect_has_area(s.status)) {
        const float ring = s.avatar_stroke, extent = ui_rect_width(s.status);
        if (s.presence_mobile && s.presence == UiPresence::online) {
            const auto g = ui_presence_mobile_geometry(s.status, ring);
            ok = paint_stream_push_rounded(paint, g.cutout.left, g.cutout.top, g.cutout.right, g.cutout.bottom, c.surface,
                                           g.cutout_radius);
        } else
            ok = paint_stream_push_rounded(paint, s.status.left - ring, s.status.top - ring, s.status.right + ring,
                                           s.status.bottom + ring, c.surface, extent * .5f + ring);
        ok = ok && ui_presence_paint(paint, s.status, s.presence, c.surface, theme, s.presence_mobile);
    }
    // The small activity image's cut-out: a ring of the card's colour over the
    // artwork beneath it.
    const float ring = card_metrics(modal(s)).ring;
    for (u32 i = 0; ok && i < s.part_count; ++i) {
        const auto& p = s.parts[i];
        const auto* n = p.media == ProfileMediaKind::activity_small ? ui_widget_manager_layout_node(s.widgets, p.widget)
                                                                    : nullptr;
        if (!n)
            continue;
        const auto clip = ui_rect_intersection(n->clip_rectangle, s.rectangle);
        const auto r = n->rectangle;
        if (!ui_rect_has_area(ui_rect_intersection(ui_rect_inset(r, {-ring, -ring, -ring, -ring}), clip)))
            continue;
        bool hot = false;
        for (u32 k = 0; k < s.part_count && !hot; ++k)
            hot = s.parts[k].media == ProfileMediaKind::activity_card && s.parts[k].source == p.source &&
                  card_hot(s, s.parts[k]);
        const float half = ui_rect_width(r) * .5f;
        ok = paint_stream_push_clip(paint, clip.left, clip.top, clip.right, clip.bottom) &&
             paint_stream_push_rounded_border(paint, r.left - ring, r.top - ring, r.right + ring, r.bottom + ring,
                                              Color{}, card_backdrop(s, c, (r.top + r.bottom) * .5f, hot),
                                              half + ring, ring) &&
             paint_stream_pop_clip(paint);
    }
    if (ok && s.edit.active)
        ok = ui_edit_field_paint(paint, s.edit_rectangle, s.edit_content, s.edit, edit, Color{},
                                 card_theme(theme, c), dpi, theme.scale.radius_md);
    // Ordinary card icons share the content's layer. Only header controls
    // and the focus indicator sit above the profile effect.
    for (u32 header = 0; ok && header < 2; ++header) {
        if (header && controls_first_batch)
            *controls_first_batch = paint_stream_seal(paint);
        for (u32 i = 0; ok && i < s.part_count; ++i) {
            const auto& p = s.parts[i];
            const bool control = p.look == ProfileLook::header_button || p.look == ProfileLook::icon ||
                                 ((p.look == ProfileLook::pill) && p.icon != kProfileNoIcon);
            if (!control || p.icon == kProfileNoIcon ||
                (p.look == ProfileLook::header_button) != (header != 0))
                continue;
            const auto* n = ui_widget_manager_layout_node(s.widgets, p.widget);
            if (!n)
                return false;
            const auto r = n->rectangle;
            const auto visible = ui_rect_intersection(n->clip_rectangle, s.rectangle);
            if (!ui_rect_has_area(ui_rect_intersection(r, visible)))
                continue;
            const bool hot = hovered(s, p);
            // The note button shows only while the card is hovered or focused.
            if (p.action == ProfileAction::open_note && !s.hovered && !ui_widget_handle_equal(s.widgets.focused, p.widget))
                continue;
            ok = paint_stream_push_clip(paint, visible.left, visible.top, visible.right, visible.bottom);
            Color glyph = tone_color(c, theme, p);
            float size = 16;
            if (p.look == ProfileLook::header_button) {
                const float half = ui_rect_width(r) * .5f;
                ok = ok && paint_stream_push_rounded_border(paint, r.left, r.top, r.right, r.bottom,
                                                            c.control, c.control_border, half, 1);
                glyph = c.control_icon;
                size = p.action == ProfileAction::close ? 20.f : 16.f;
            } else if (p.action != ProfileAction::none)
                glyph = hot ? c.strong : c.subtle;
            if (p.source == 12)
                size = 12;
            if (ui_rect_width(r) < size)
                size = ui_rect_width(r);
            const float cx = (r.left + r.right) * .5f, cy = (r.top + r.bottom) * .5f;
            ok = ok &&
                 ui_icon_paint(paint, static_cast<UiIcon>(p.icon),
                               {cx - size * .5f, cy - size * .5f, cx + size * .5f, cy + size * .5f}, glyph);
            ok = paint_stream_pop_clip(paint) && ok;
        }
    }
    ok = paint_stream_pop_clip(paint) && ok;
    return ok && (!s.focus_visible || paint_stream_push_focus(paint, s.widgets, theme.tokens.border.focus));
}
bool profile_scene_text_output(const ProfileScene& s, const Theme& theme, UiTextOutput* text,
                               const UiEditGeometry* edit) noexcept {
    if (!s.active)
        return true;
    const auto c = palette(s, theme);
    const Theme local = card_theme(theme, c);
    for (u32 i = 0; i < s.part_count; ++i) {
        const auto& p = s.parts[i];
        if (p.badge.owner.generation) {
            if (!ui_badge_text_output(p.badge, s.widgets, local, text))
                return false;
            continue;
        }
        if (!p.text.bytes)
            continue;
        const auto* n = ui_widget_manager_layout_node(s.widgets, p.widget);
        if (!n)
            return false;
        const auto r = n->rectangle;
        const auto clip = ui_rect_intersection(s.rectangle, n->clip_rectangle);
        if (!ui_rect_has_area(ui_rect_intersection(r, clip)))
            continue;
        const bool name = ui_widget_handle_equal(p.widget, s.name_widget);
        const auto* style = name ? appearance(s, appearance_name) : nullptr;
        // Activity lines share the card's click and only carry hints.
        const bool underline = p.look == ProfileLook::plain && hovered(s, p) && p.action != ProfileAction::card &&
                               p.action != ProfileAction::hint;
        const bool bio = ui_widget_handle_equal(p.widget, s.bio_widget);
        if (!ui_text_output_push(
                text, {p.text,
                       {&s.widgets, p.widget},
                       r,
                       clip,
                       p.media == ProfileMediaKind::plugin_slot ? theme.tokens.text.link : tone_color(c, theme, p),
                       p.style | (underline ? text_style_underline : 0),
                       p.centered || p.look == ProfileLook::pill ? UiTextAlignment::center
                       : p.media == ProfileMediaKind::activity_clock ? UiTextAlignment::end
                                                                     : UiTextAlignment::start,
                       p.wrap ? UiTextAlignment::start : UiTextAlignment::center,
                       p.wrap ? UiTextWrapping::wrap : UiTextWrapping::no_wrap,
                       UiTextOverflow::ellipsis,
                       style ? profile_name_face(style->name.font_id) : UiTextFace::normal,
                       style ? profile_name_effect(style->name, true, s.name_started_ms) : UiTextEffect{},
                       bio ? ConstSlice<TextStyleSpan>{s.bio_styles, s.bio_style_count} : ConstSlice<TextStyleSpan>{}}))
            return false;
    }
    if (!s.edit.active)
        return true;
    return ui_edit_field_text_output(text, {&s.widgets, s.message_field}, s.edit_content, s.edit, edit,
                                     s.message_placeholder.view(), local);
}
} // namespace maicord
