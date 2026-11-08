#include "profile_scene_internal.hpp"
#include "maicord/model/guild/role_policy.hpp"
#include "maicord/ui/account/profile_appearance.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/widgets/presence.hpp"
#include "maicord/model/user/store.hpp"

namespace maicord {
using namespace profile_scene_detail;

namespace profile_scene_detail {
Storage& labels(ProfileScene* s) noexcept {
    return *reinterpret_cast<Storage*>(s->storage.base);
}
const Storage& labels(const ProfileScene& s) noexcept {
    return *reinterpret_cast<const Storage*>(s.storage.base);
}
bool interactive(const ProfileScene& s) noexcept {
    return s.presentation != ProfilePresentation::preview;
}
bool modal(const ProfileScene& s) noexcept {
    return s.presentation == ProfilePresentation::modal;
}
UiSize measure(ProfileScene* s, const Utf8View& text, float width, bool wrap, u32 flags, UiTextFace face) noexcept {
    if (!text.bytes)
        return {};
    UiTextMeasureValue result{};
    const TextStyleSpan span{0, text.bytes, flags};
    const u64 hash = hash64_bytes(kHash64Seed, text.data, text.bytes);
    if (s->measure && ui_text_measure_cache_measure(
                          s->measure,
                          {{0x50524f46, hash, 0, static_cast<u16>(face), static_cast<u16>(flags)},
                           text,
                           flags ? ConstSlice<TextStyleSpan>{&span, 1} : ConstSlice<TextStyleSpan>{},
                           {width, 4096},
                           !wrap,
                           face},
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
float text_width(ProfileScene* s, const Utf8View& text, float limit, u32 flags, UiTextFace face) noexcept {
    return minimum(limit,
                   static_cast<float>(static_cast<u32>(measure(s, text, 4096, false, flags, face).width + .999f)));
}
namespace {
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
void prepare_biography(ProfileScene* s, const Utf8View& source) noexcept {
    u64 signature = hash64_bytes(kHash64Seed, source.data, source.bytes);
    signature = hash64_bytes(signature, s->bio_revealed, s->bio_revealed_count * sizeof(u32));
    if (s->bio_signature == signature)
        return;
    // The parser's workspace lives in the card's storage, not on the stack.
    auto& work = labels(s).bio_work;
    reset_value(&work);
    MessageMarkdownOptions options{};
    options.context = s;
    options.profile_bio = true;
    options.spoiler_revealed = biography_revealed;
    const auto result = message_markdown_parse(source, {s->bio_display.data, 1024}, {work.words, 1024},
                                               {work.spans, 128}, options, {s->bio_semantics, 64});
    // An unparsable bio shows its source text plainly.
    const bool parsed = result.ok();
    if (!parsed)
        copy_bytes(s->bio_display.data, source.data, source.bytes);
    s->bio_display.bytes = static_cast<u16>(parsed ? result.text.bytes : source.bytes);
    s->bio_semantic_count = parsed ? result.semantic_count : 0;
    size_biography(s, work.spans, parsed ? result.span_count : 0);
    s->bio_signature = signature;
}
// The stand-in for a part with nothing to show (callers may still decorate
// it) or for a full scene (`failed`: profile_scene_layout reports it).
ProfilePart* sink(ProfileScene* s, bool failed) noexcept {
    static ProfilePart part;
    reset_value(&part);
    labels(s).failed |= failed;
    return &part;
}
} // namespace
ProfilePart* add(ProfileScene* s, UiWidgetHandle parent, const UiRect& r, const Utf8View& label,
                 ProfileAction action, u32 flags,
                 ProfileLook look, ProfileTone tone,
                 bool clip) noexcept {
    if (s->part_count + 1 >= capacity)
        return sink(s, true);
    if (!ui_rect_has_area(r))
        return sink(s, false);
    // The editor preview is a picture of the card: nothing in it is a control.
    if (!interactive(*s))
        action = ProfileAction::none;
    auto style = ui_layout_style();
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
         {utf8_literal("ProfilePart"), style, {}, id}});
    if (!widget.ok())
        return sink(s, true);
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
// A label-less part: the common shape of backgrounds, rules, dots and hit areas.
ProfilePart* blank(ProfileScene* s, UiWidgetHandle parent, const UiRect& r,
                   ProfileAction action, ProfileLook look) noexcept {
    return add(s, parent, r, {}, action, 0, look);
}
ProfilePart* icon(ProfileScene* s, UiWidgetHandle parent, const UiRect& r, UiIcon glyph, ProfileAction action,
                  ProfileTone tone, ProfileLook look) noexcept {
    auto* part = add(s, parent, r, {}, action, 0, look, tone);
    part->icon = static_cast<u16>(glyph);
    return part;
}
// A plain label: no action, the usual shape of a text part.
ProfilePart* caption(ProfileScene* s, UiWidgetHandle parent, const UiRect& r, const Utf8View& label, u32 flags,
                     ProfileTone tone) noexcept {
    return add(s, parent, r, label, ProfileAction::none, flags, ProfileLook::plain, tone);
}
void place(ProfileScene* s, UiWidgetHandle handle, const UiRect& r) noexcept {
    auto* node = ui_widget_manager_layout_node(&s->widgets, handle);
    if (!node || !ui_rect_has_area(r)) {
        labels(s).failed = true;
        return;
    }
    node->spec.style.width = ui_layout_fixed(ui_rect_width(r));
    node->spec.style.height = ui_layout_fixed(ui_rect_height(r));
    node->spec.style.margin.left = r.left;
    node->spec.style.margin.top = r.top;
}
namespace {
// The chip `part` draws: a slot of the scene's badge list.
UiBadgeWidget& attach_badge(ProfileScene* s, ProfilePart* part) noexcept {
    auto& l = labels(s);
    if (l.badge_count >= kBadges) {
        l.failed = true;
        return l.badges[0];
    }
    part->badge = static_cast<u8>(l.badge_count + 1);
    return l.badges[l.badge_count++];
}
template <u32 N> bool append_number(ProfileString<N>* s, u64 n) noexcept {
    if (s->bytes + decimal_digits(n) > N)
        return false;
    s->bytes += static_cast<u16>(decimal_write(s->data + s->bytes, n));
    return true;
}
} // namespace
// "N Mutual Friend(s)" or "N Mutual Server(s)".
void mutual_count(ProfileString<64>* text, u32 n, bool servers) noexcept {
    text->bytes = 0;
    append_number(text, n);
    append(text, servers ? (n == 1 ? utf8_literal(" Mutual Server") : utf8_literal(" Mutual Servers"))
                         : (n == 1 ? utf8_literal(" Mutual Friend") : utf8_literal(" Mutual Friends")));
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

CardMetrics card_metrics(bool large) noexcept {
    return large ? CardMetrics{12, 100, 32, 12, 4} : CardMetrics{8, 60, 24, 8, 2};
}
namespace {
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
// full text as a hint; full profile lines wrap. Returns its height.
float card_text(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, const Utf8View& text, u32 flags,
                float line, u16 item) noexcept {
    if (!text.bytes)
        return 0;
    const bool wrap = modal(*s);
    const float height = wrap ? maximum(line, measure(s, text, width, true, flags).height) : line;
    const bool cut = !wrap && measure(s, text, 4096, false, flags).width > width;
    auto* part = add(s, owner, {left, top, left + width, top + height}, text,
                     cut ? hint_action(*s) : ProfileAction::none, flags, ProfileLook::plain, ProfileTone::normal);
    part->wrap = height > line;
    part->media = ProfileMediaKind::activity_text;
    part->source = item;
    if (cut) {
        part->hint = text;
        unfocusable(s, part);
    }
    return height;
}
// The width a changing time string needs at its length (digits as 8s).
float clock_width(ProfileScene* s, const Utf8View& text, u32 flags) noexcept {
    char widest[16];
    const u32 bytes = minimum<u32>(text.bytes, sizeof(widest));
    for (u32 i = 0; i < bytes; ++i)
        widest[i] = text.data[i] >= '0' && text.data[i] <= '9' ? '8' : text.data[i];
    return text_width(s, {widest, bytes}, 4096, flags) + 1;
}
} // namespace
void shift(ProfileScene* s, u32 first, float dy) noexcept {
    for (u32 i = first; dy != 0 && i < s->part_count; ++i) {
        auto* node = ui_widget_manager_layout_node(&s->widgets, s->parts[i].widget);
        if (node)
            node->spec.style.margin.top += dy;
    }
}
namespace {
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
// column) in the full profile. Returns its width.
float card_button(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, bool full,
                  const Utf8View& label, UiIcon glyph, ProfileAction action, u16 item, u64 index,
                  const Utf8View& hint) noexcept {
    const bool disabled = action == ProfileAction::activity_join;
    const float text = text_width(s, label, 400, kSm | text_style_bold);
    const float content = (glyph != UiIcon::count ? 24.f : 0.f) + text;
    const float w = full ? width : minimum(width, content + 32);
    auto* b = blank(s, owner, {left, top, left + w, top + 32}, action, ProfileLook::secondary);
    b->media = ProfileMediaKind::activity_action;
    b->source = item;
    b->target = index;
    b->hint = hint;
    const ProfileTone tone = disabled ? ProfileTone::muted : ProfileTone::normal;
    float x = left + maximum(16.f, (w - content) * .5f);
    if (glyph != UiIcon::count) {
        icon(s, owner, {x, top + 8, x + 16, top + 24}, glyph, ProfileAction::none, tone);
        x += 24;
    }
    caption(s, owner, {x, top, maximum(x + 1, minimum(x + text + 1, left + w - 16)), top + 32}, label,
            kSm | text_style_bold, tone);
    return w;
}
} // namespace
float activity_card(ProfileScene* s, UiWidgetHandle owner, float left, float top, float right, u32 card, u32 item,
                    u64 now_ms, ProfileAction action) noexcept {
    auto& l = labels(s);
    const auto& a = l.activity_set.items[item];
    auto& c = l.activity_content[card];
    const bool large = modal(*s);
    const auto m = card_metrics(large);
    if (!profile_activity_content(a, bot(*s), self(*s), &c))
        return 0;
    auto* background = blank(s, owner, {left, top, right, top + 1}, action, ProfileLook::card);
    background->media = ProfileMediaKind::activity_card;
    background->source = static_cast<u16>(item);
    const u16 source = static_cast<u16>(item);
    float y = top + m.pad;
    if (c.heading.bytes) {
        caption(s, owner, {left + m.pad, y, right - m.pad, y + 16}, c.heading.view(), kXs | text_style_bold,
                ProfileTone::strong);
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
        auto* image = blank(s, owner, {left + m.pad, row, left + m.pad + m.image, row + m.image},
                            large_hint.bytes ? hint_action(*s) : ProfileAction::none);
        image->media = ProfileMediaKind::activity_large;
        image->source = source;
        image->target = static_cast<u64>(art.large);
        image->hint = large_hint;
        unfocusable(s, image);
        if (art.badge) {
            const float x = left + m.pad + m.image + 4, sy = row + m.image + 4;
            const auto small_hint = user_activity_text(a, UserActivityField::small_text);
            auto* corner = blank(s, owner, {x - m.small, sy - m.small, x, sy},
                                 small_hint.bytes ? hint_action(*s) : ProfileAction::none);
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
    if (c.title.bytes)
        cy += card_text(s, owner, text_left, cy, column, c.title.view(), kSm | text_style_bold, kCardTitle, source);
    for (u32 i = 0; i < c.line_count; ++i)
        cy += card_text(s, owner, text_left, cy, column, c.lines[i].view(), kXs, kCardLine, source);
    ActivityTimer timer{kNoPart, kNoPart, kNoPart, kNoPart, kNoPart, static_cast<u8>(item), static_cast<u8>(card)};
    const auto clock = c.chips ? profile_activity_clock(a, now_ms) : ProfileActivityClock{};
    if (c.chips && (c.party.bytes || c.episode.bytes || clock.timer != ProfileActivityTimer::none)) {
        if (cy > row)
            cy += 4;
        float x = 0, line_top = cy;
        const auto chip = [&](ProfileActivityGlyph glyph, const Utf8View& text, bool timed) noexcept {
            const float label = timed ? clock_width(s, text, kXs) : text_width(s, text, 4096, kXs);
            const float w = minimum(column, 16 + label);
            if (x > 0 && x + w > column) {
                x = 0;
                line_top += kCardLine;
            }
            const float cx = text_left + x;
            const auto tone = timed ? ProfileTone::positive : ProfileTone::normal;
            auto* glyph_part = icon(s, owner, {cx, line_top + 2, cx + 12, line_top + 14},
                                    profile_activity_glyph_icon(glyph), ProfileAction::none,
                                    timed ? ProfileTone::positive : ProfileTone::subtle);
            auto* label_part = caption(s, owner, {cx + 16, line_top, cx + w, line_top + kCardLine}, text, kXs, tone);
            label_part->media = ProfileMediaKind::activity_text;
            label_part->source = source;
            if (timed) {
                timer.clock = part_index(*s, label_part);
                timer.glyph = part_index(*s, glyph_part);
            }
            x += w + 8;
        };
        auto& clock_text = l.activity_time[card][0];
        clock_text.bytes = static_cast<u16>(
            profile_activity_clock_text(clock.seconds, {clock_text.data, sizeof(clock_text.data)}));
        const auto clock_glyph =
            clock.timer == ProfileActivityTimer::countdown ? ProfileActivityGlyph::timer : c.clock_glyph;
        const bool timed = clock.timer != ProfileActivityTimer::none;
        if (c.party.bytes)
            chip(ProfileActivityGlyph::party, c.party.view(), false);
        if (timed)
            chip(clock_glyph, clock_text.view(), true);
        if (c.episode.bytes)
            chip(ProfileActivityGlyph::episode, c.episode.view(), false);
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
        auto* left_label = caption(s, owner, {text_left, cy, text_left + label, cy + kCardLine}, elapsed.view(), flags);
        auto* right_label = caption(s, owner, {text_right - label, cy, text_right, cy + kCardLine}, total.view(), flags);
        auto* bar = blank(s, owner, {text_left + label + 8, cy + 7, maximum(text_left + label + 9, text_right - label - 8), cy + 9});
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
        button_icon = UiIcon::controller;
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
            card_button(s, owner, left + m.pad, bottom, right - left - 2 * m.pad, true, button_labels[i], button_icon,
                        button_action, source, i, button_hint);
            bottom += 32 + (i + 1 < buttons ? 8.f : 0.f);
        }
    }
    bottom += m.pad;
    place(s, background->widget, {left, top, right, bottom});
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
    blank(s, owner, {left, top, left + width, top + height}, ProfileAction::none, ProfileLook::bubble);
    const float x = left + 13, y = top + 9;
    if (emoji) {
        const float size = emoji_only ? 32.f : 16.f;
        auto* e = add(s, owner, {x, y + (emoji_only ? 0.f : 1.f), x + size, y + size + (emoji_only ? 0.f : 1.f)},
                      status->status_emoji_id ? Utf8View{} : status->status_emoji.view(), ProfileAction::none,
                      emoji_only ? kXl : kSm);
        if (status->status_emoji_id)
            e->media = ProfileMediaKind::status_emoji;
    }
    if (text.bytes)
        caption(s, owner, {x + emoji_width, y, left + width - 13, y + content_height}, text, kSm)->wrap =
            content_height > 18;
    return height;
}

namespace {
// Whether a role's pill carries its remove button.
bool role_removable(const ProfileScene& s, const RoleEditPolicy& policy, const RoleRecord& role) noexcept {
    return interactive(s) && !s.roles_busy && role_editable(policy, role);
}
} // namespace
// Whether roles() draws anything: a role of the member the store knows, or
// the add button.
bool roles_shown(const ProfileScene& s) noexcept {
    const auto& p = *s.profile;
    if (!s.state || !p.guild_id)
        return false;
    if (interactive(s) && s.can_manage_roles && !s.roles_busy)
        return true;
    for (u32 i = 0; i < p.role_count && i < sizeof(p.roles) / sizeof(p.roles[0]); ++i)
        if (chat_state_store_find_role(*s.state, p.guild_id, p.roles[i]))
            return true;
    return false;
}
// Role pills. Popouts show at most two lines within the content width and end
// with "+N"; the modal and an expanded popout list every role. Returns the
// block height (0 without roles or add button).
float roles(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, bool clamp) noexcept {
    const auto& p = *s->profile;
    if (!s->state || !p.guild_id)
        return 0;
    const auto policy = role_edit_policy(*s->state, p.guild_id);
    // The profile lists the member's roles in wire order; pills follow the role hierarchy,
    // linked roles last.
    constexpr u32 kRoles = sizeof(p.roles) / sizeof(p.roles[0]);
    const RoleRecord* ranked[kRoles];
    u32 ranked_count = 0;
    for (u32 i = 0; i < p.role_count && i < kRoles; ++i)
        if (const auto* role = chat_state_store_find_role(*s->state, p.guild_id, p.roles[i]))
            ranked[ranked_count++] = role;
    heap_sort_shared(ranked, ranked_count,
              [](const RoleRecord* left, const RoleRecord* right) noexcept { return role_pill_before(*left, *right); });
    // Pill widths beside the ranked roles: the frame stays within a page.
    float widths[kRoles];
    const u32 count = ranked_count;
    for (u32 i = 0; i < count; ++i) {
        const bool removable = role_removable(*s, policy, *ranked[i]);
        const float label = text_width(s, chat_state_store_role_name(*s->state, *ranked[i]), width, kXs);
        widths[i] = minimum(width, 2 + 4 + 12 + 4 + label + (removable ? 24.f : 8.f));
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
            put(widths[i]);
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
    // Up to three parts a pill; parts left for the sections below bound how
    // many show before "+N".
    const u32 room = capacity > s->part_count + 64 ? (capacity - s->part_count - 64) / 3 : 0;
    const bool collapse = clamp && s->roles_expanded && count <= room;
    const auto set_more = [&](u32 hidden_count) noexcept {
        more_text.bytes = 0;
        append(&more_text, utf8_literal("+"));
        append_number(&more_text, hidden_count);
        more_width = maximum(24.f, text_width(s, more_text.view(), width, kXs) + 16);
    };
    if (count > room && (!clamp || s->roles_expanded)) {
        visible = room;
        set_more(count - visible);
    } else if (clamp && !s->roles_expanded && lines(count, false, 0) > 2) {
        for (visible = count; visible > 0; --visible) {
            set_more(count - visible);
            if (lines(visible, true, more_width) <= 2)
                break;
        }
        if (!visible)
            set_more(count);
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
        const auto* role = ranked[i];
        const bool removable = role_removable(*s, policy, *role);
        const auto r = slot(widths[i]);
        auto* pill = blank(s, owner, r, ProfileAction::none, ProfileLook::role);
        pill->color = role->color ? color_from_rgb24(role->color) : Color{};
        pill->target = role->id;
        caption(s, owner, {r.left + 22, r.top + 4, r.right - (removable ? 24.f : 8.f), r.bottom - 4},
                chat_state_store_role_name(*s->state, *role), kXs);
        if (removable) {
            auto* remove = icon(s, owner, {r.right - 24, r.top, r.right, r.bottom}, UiIcon::close,
                                ProfileAction::remove_role);
            remove->target = role->id;
            remove->source = 12;
        }
    }
    if (more_text.bytes || collapse) {
        const auto r = slot(collapse ? 24.f : more_width);
        auto* more = add(s, owner, r, collapse ? Utf8View{} : more_text.view(), ProfileAction::roles_toggle, kXs,
                         ProfileLook::pill);
        more->centered = true;
        if (collapse)
            more->icon = static_cast<u16>(UiIcon::chevron_up);
    }
    if (add_button) {
        const auto r = slot(add_width);
        auto* plus = add(s, owner, r, count ? Utf8View{} : utf8_literal("Add role"), ProfileAction::choose_roles,
                         kXs, ProfileLook::pill, ProfileTone::subtle);
        plus->centered = true;
        if (count)
            plus->icon = static_cast<u16>(UiIcon::plus);
    }
    return y + 24;
}

// Plugin profile-section nodes: chips, then full-width link rows.
float plugin_section(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept {
    UiPluginSlotNode nodes[8];
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
        auto* part = blank(s, owner, {left + x, top + y, left + x + chip, top + y + 26},
                           node.kind == ui_plugin_node_button ? ProfileAction::plugin_slot : ProfileAction::none);
        attach_badge(s, part) = {part->widget, label, {}, text_style_subtext, true, false};
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
    UiBadgeWidget app{};
    float tag = 0;
    if (bot(*s)) {
        ui_app_badge(&app, p.user_appearance.public_flags);
        tag = ui_rect_width(ui_badge_widget_layout(s->measure, app, {0, 0, 96, 16}).outer) + 4;
    }
    // Measured in the face it is drawn in: a custom name font is wider than the default one.
    const auto* name_style = appearance(*s, appearance_name);
    const UiTextFace name_face = name_style ? profile_name_face(name_style->name.font_id) : UiTextFace::normal;
    const float name_width =
        maximum(24.f, text_width(s, display, width - tag - (note ? 28.f : 0.f), name_flags, name_face));
    auto* name = add(s, owner, {left, top, left + name_width, top + line}, display,
                     large ? ProfileAction::none : ProfileAction::expand, name_flags);
    s->name_widget = name->widget;
    float x = left + name_width + 4;
    if (tag) {
        auto* badge = blank(s, owner, {x, top + (line - 16) * .5f, x + tag - 4, top + (line + 16) * .5f});
        app.owner = badge->widget;
        attach_badge(s, badge) = app;
        // The client wraps only the verified check in its "Verified App" tooltip.
        if (app.leading) {
            auto* check = blank(s, owner,
                                ui_app_badge_check_rectangle({x, top + (line - 16) * .5f, x + tag - 4,
                                                              top + (line + 16) * .5f}),
                                ProfileAction::hint);
            check->hint = kUiAppBadgeVerifiedHint;
            unfocusable(s, check);
        }
        x += tag;
    }
    if (note)
        icon(s, owner, {x, top, x + 24, top + 24}, UiIcon::file_text, ProfileAction::open_note);
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
        auto* main = blank(s, owner, slot(size, size));
        main->media = ProfileMediaKind::peer_avatar;
        main->source = 0xFFFF;
    }
    // Bots keep a discriminator: "name#1234" (unique usernames show alone).
    auto username = p.username.view();
    if (p.discriminator) {
        auto& tag = labels(s).username_tag;
        tag.bytes = 0;
        char digits[4];
        u32 n = p.discriminator;
        for (u32 i = 4; i-- > 0; n /= 10)
            digits[i] = static_cast<char>('0' + n % 10);
        append(&tag, username);
        append(&tag, utf8_literal("#"));
        append(&tag, Utf8View{digits, 4});
        username = tag.view();
    }
    add(s, owner, slot(text_width(s, username, width, kSm), 18), username, large ? ProfileAction::none : ProfileAction::expand,
        kSm, ProfileLook::plain, ProfileTone::strong);
    if (pronouns.bytes) {
        auto dot = slot(6, 18);
        blank(s, owner, {dot.left + 1, dot.top + 7, dot.left + 5, dot.top + 11}, ProfileAction::none,
              ProfileLook::dot);
        caption(s, owner, slot(text_width(s, pronouns, width, kSm) + 2, 18), pronouns, kSm, ProfileTone::strong);
    }
    const auto& a = p.user_appearance;
    if (a.primary_enabled && a.primary_tag.bytes) {
        const auto measured = ui_badge_layout(s->measure, a.primary_tag.view(), {0, 0, width, 20}, true,
                                              text_style_subtext);
        const auto r = slot(ui_rect_width(measured.outer), 20);
        // The chip opens the server tag popout (live cards only).
        const auto tag_action = interactive(*s) && a.primary_guild_id ? ProfileAction::guild_tag : ProfileAction::none;
        auto* tag_part = blank(s, owner, r, tag_action);
        attach_badge(s, tag_part) = {tag_part->widget, a.primary_tag.view(), {}, text_style_subtext, true, false};
        tag_part->target = a.primary_guild_id;
        const auto layout = ui_badge_layout(s->measure, a.primary_tag.view(), r, true, text_style_subtext);
        auto* glyph = blank(s, owner, layout.icon, tag_action);
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
            // Bare 20 DIP images, 2 apart; the description is the tooltip.
            auto* b = blank(s, owner, {bx, by, bx + 20, by + 20}, ProfileAction::badge);
            b->media = ProfileMediaKind::badge;
            b->source = static_cast<u16>(i);
            b->hint = p.badges[i].description.view();
        }
    }
    return row_top + row_height - top;
}

// Relationship notices: the incoming friend request
// card with Accept, or "You blocked them".
float notices(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, bool large) noexcept {
    if (self(*s) || (s->relation != 2 && s->relation != 4))
        return 0;
    const float pad = large ? 12.f : 16.f;
    if (s->relation == 4) {
        blank(s, owner, {left, top, left + width, top + 18 + pad * 2}, ProfileAction::none, ProfileLook::card);
        caption(s, owner, {left + pad, top + pad, left + width - pad, top + pad + 18}, utf8_literal("You blocked them"),
                kSm | text_style_bold);
        return 18 + pad * 2;
    }
    const float line = 18, button_top = top + pad + line + 8;
    blank(s, owner, {left, top, left + width, button_top + 32 + pad}, ProfileAction::none, ProfileLook::card);
    const auto name = s->profile->username.view();
    const float name_width = text_width(s, name, width - pad * 2, kSm | text_style_bold);
    caption(s, owner, {left + pad, top + pad, left + pad + name_width, top + pad + line}, name, kSm | text_style_bold);
    caption(s, owner, {left + pad + name_width, top + pad, left + width - pad, top + pad + line},
            utf8_literal(" sent you a friend request."), kSm);
    if (interactive(*s))
        button(s, owner, left + pad, button_top, utf8_literal("Accept"), UiIcon::count, ProfileAction::accept_friend,
               true);
    return button_top + 32 + pad - top;
}
// A mutual peer's name; a server shows the guild's current name.
Utf8View peer_name(const ProfileScene& s, bool friends, const ProfilePeer& peer) noexcept {
    if (!friends && s.state)
        if (const auto* guild = chat_state_store_find_guild(*s.state, peer.id))
            return chat_state_store_guild_name(*s.state, *guild);
    return peer.name.view();
}
// A mutual friend's or server's row with its face; both carry the peer's index.
void peer_row(ProfileScene* s, UiWidgetHandle owner, bool friends, u32 index, const UiRect& row_rect,
              const UiRect& face_rect) noexcept {
    auto* row = blank(s, owner, row_rect, friends ? ProfileAction::open_friend : ProfileAction::open_server,
                      ProfileLook::row);
    auto* face = blank(s, owner, face_rect);
    row->source = face->source = static_cast<u16>(index);
    face->media = friends ? ProfileMediaKind::peer_avatar : ProfileMediaKind::peer_guild;
}
namespace {
// One chip of the mutuals row: its hit area, the stacked faces and the count text.
void mutual_chip(ProfileScene* s, UiWidgetHandle owner, float x, float top, float w, float stack, u32 faces,
                 ProfileAction action, ProfileMediaKind media, const Utf8View& text, u32 flags) noexcept {
    blank(s, owner, {x, top, x + w, top + 24}, action);
    for (u32 i = faces; i > 0; --i) {
        auto* face = blank(s, owner, {x + (i - 1) * 12.f, top + 4, x + (i - 1) * 12.f + 16, top + 20});
        face->media = media;
        face->source = static_cast<u16>(i - 1);
    }
    caption(s, owner, {x + stack, top + 4, x + w, top + 20}, text, flags, ProfileTone::subtle);
}
} // namespace
// The mutuals row of the popout and the DM panel (bots list servers only:
// their profiles carry no mutual friends).
float mutuals(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept {
    const auto& p = *s->profile;
    const u32 friends = p.mutual_friend_count, servers = p.mutual_guild_count;
    if (self(*s) || (!friends && !servers))
        return 0;
    auto& l = labels(s);
    const u32 flags = friends && servers ? kXs : kSm;
    float x = left;
    if (friends) {
        auto& text = l.mutual_text[0];
        mutual_count(&text, friends, false);
        const u32 faces = minimum<u32>(3, p.mutual_friends_count);
        const float stack = faces ? 16 + (faces - 1) * 12.f + 4 : 0;
        const float w = minimum(width, stack + text_width(s, text.view(), width, flags));
        mutual_chip(s, owner, x, top, w, stack, faces, ProfileAction::friends, ProfileMediaKind::peer_avatar, text.view(),
                    flags);
        x += w + 6;
    }
    if (servers) {
        if (friends) {
            blank(s, owner, {x, top + 10, x + 4, top + 14}, ProfileAction::none, ProfileLook::dot);
            x += 10;
        }
        auto& text = l.mutual_text[1];
        mutual_count(&text, servers, true);
        const u32 icons = friends ? 0 : minimum<u32>(3, p.mutual_guild_count);
        const float stack = icons ? 16 + (icons - 1) * 12.f + 4 : 0;
        const float w = minimum(left + width - x, stack + text_width(s, text.view(), width, flags));
        if (w > 0)
            mutual_chip(s, owner, x, top, w, stack, icons, ProfileAction::servers, ProfileMediaKind::peer_guild,
                        text.view(), flags);
    }
    return 24;
}

// Biography: no heading; clamped to three lines in popouts and the DM panel
// (the popout adds "View Full Bio" when it overflows), unclamped in the modal.
float biography(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, bool clamp,
                bool view_bio) noexcept {
    const auto source = profile_display(*s->profile).bio;
    if (!source.bytes)
        return 0;
    prepare_biography(s, source);
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
    // The bio is clamped to three lines, the
    // third ending in an ellipsis when more follows. Without line counts
    // (no measure cache) three text-sm lines stand in.
    const u32 lines = size.line_count;
    const bool overflow = clamp && (lines ? lines > 3 : height > 57.75f);
    if (overflow)
        height = lines ? static_cast<float>(static_cast<u32>(size.size.height * 3 / lines + .999f)) : 54.f;
    // The text-sm size rides on the bio's own style spans.
    auto* text = caption(s, owner, {left, top, left + width, top + height}, bio, 0, ProfileTone::strong);
    text->wrap = true;
    s->bio_widget = text->widget;
    if (!overflow || !view_bio || !interactive(*s))
        return height;
    constexpr auto label = utf8_literal("View Full Bio");
    const float w = text_width(s, label, width, kXs) + 24;
    blank(s, owner, {left, top + height + 8, left + w, top + height + 40}, ProfileAction::view_bio,
          ProfileLook::secondary);
    caption(s, owner, {left + 12, top + height + 8, left + w - 12, top + height + 40}, label, kXs);
    return height + 40;
}

// A primary or secondary button with an optional leading icon; zero-width
// labels make a square icon button. Returns its width.
float button(ProfileScene* s, UiWidgetHandle owner, float left, float top, const Utf8View& label, UiIcon glyph,
             ProfileAction action, bool primary, float width) noexcept {
    const float text = label.bytes ? text_width(s, label, 400, kSm | text_style_bold) : 0;
    const float content = (glyph != UiIcon::count ? 16.f : 0.f) + (label.bytes && glyph != UiIcon::count ? 8.f : 0.f) + text;
    const float w = width ? width : label.bytes ? content + 32 : 32;
    blank(s, owner, {left, top, left + w, top + 32}, action, primary ? ProfileLook::primary : ProfileLook::secondary);
    float x = left + (w - content) * .5f;
    if (glyph != UiIcon::count) {
        icon(s, owner, {x, top + 8, x + 16, top + 24}, glyph, ProfileAction::none,
             primary ? ProfileTone::on_accent : ProfileTone::normal);
        x += 24;
    }
    if (label.bytes)
        caption(s, owner, {x, top, x + text + 1, top + 32}, label, kSm | text_style_bold,
                primary ? ProfileTone::on_accent : ProfileTone::normal);
    return w;
}

// Header geometry shared by popout and modal: banner, avatar with its cut-out
// stroke, decoration at 1.2x and the status dot.
void header_geometry(ProfileScene* s, const UiRect& banner, float avatar_left, float avatar_top, float size) noexcept {
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
// Banner, avatar and decoration parts (relative to the card's panel).
void header_parts(ProfileScene* s, ProfileAction avatar_action) noexcept {
    blank(s, s->panel, s->banner)->media = ProfileMediaKind::banner;
    blank(s, s->panel, s->avatar, avatar_action)->media = ProfileMediaKind::avatar;
    if (profile_display(*s->profile).decoration.bytes)
        blank(s, s->panel, s->decoration)->media = ProfileMediaKind::decoration;
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
// A part that takes text input: its widget becomes a text field.
UiWidgetHandle text_field(ProfileScene* s, const ProfilePart* part) noexcept {
    if (auto* widget = ui_widget_manager_resolve(&s->widgets, part->widget))
        widget->kind = UiWidgetKind::text_field;
    else
        labels(s).failed = true;
    return part->widget;
}
bool can_message(const ProfileScene& s) noexcept {
    return !self(s) && s.relation != 4;
}
bool finish(ProfileScene* s, const UiRect& viewport) noexcept {
    ++s->revision;
    // A part that could not be added drops alone: the parts that were added
    // still get their geometry, so a failed layout never leaves the card's
    // nodes empty. The failure is still reported.
    if (!ui_widget_manager_layout_rectangle(&s->widgets, viewport).ok())
        return false;
    if (labels(s).failed)
        return false;
    if (s->note_editing && s->note_field.generation) {
        // The note field takes the edit view: geometry follows its laid-out row.
        const auto* node = ui_widget_manager_layout_node(s->widgets, s->note_field);
        if (!node)
            return false;
        s->message_field = s->note_field;
        s->edit_rectangle = node->rectangle;
        s->edit_content = ui_rect_inset(s->edit_rectangle, {10, 2, 10, 2});
        reset_value(&s->message_placeholder);
        append(&s->message_placeholder, utf8_literal("Click to add a note"));
        if (s->edit.focused)
            ui_widget_manager_focus(&s->widgets, s->note_field);
    }
    return true;
}

// The header rectangles from card coordinates to screen coordinates.
void header_to_screen(ProfileScene* s, float dx, float dy) noexcept {
    UiRect* const rects[]{&s->banner, &s->avatar, &s->decoration, &s->status};
    for (auto* r : rects)
        *r = {dx + r->left, dy + r->top, dx + r->right, dy + r->bottom};
}
// The More and relationship buttons of a live card; bx is the left of More.
void header_buttons(ProfileScene* s, float bx, float by) noexcept {
    icon(s, s->panel, {bx, by, bx + 32, by + 32}, UiIcon::ellipsis, ProfileAction::more, ProfileTone::normal,
         ProfileLook::header_button);
    bx -= 40;
    if (bot(*s) || s->relation == 4)
        return;
    const UiIcon glyph = s->relation == 1 ? UiIcon::user_check : s->relation >= 2 ? UiIcon::recent : UiIcon::user_plus;
    icon(s, s->panel, {bx, by, bx + 32, by + 32}, glyph,
         s->relation ? ProfileAction::relationship : ProfileAction::add_friend, ProfileTone::normal,
         ProfileLook::header_button);
}
} // namespace profile_scene_detail
} // namespace maicord
