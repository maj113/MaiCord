#include "maicord/ui/theme/surface_elevation.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/account/profile_activity.hpp"
#include "maicord/ui/widgets/badge.hpp"
#include "maicord/ui/accessibility/accessibility.hpp"
#include "maicord/ui/widgets/icon.hpp"
#include "maicord/ui/plugins/slots.hpp"
#include "maicord/model/guild/role_policy.hpp"
#include "maicord/ui/account/profile_appearance.hpp"
#include "maicord/ui/layout/scrollbar.hpp"
#include "maicord/ui/core/transition.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/user/store.hpp"
#include "maicord/ui/widgets/presence.hpp"
#include "maicord/ui/layout/scroll_host.hpp"
#include "maicord/sys/crt.hpp"
#include "profile_scene_internal.hpp"
namespace maicord {
using namespace profile_scene_detail;
namespace {
Color over(const Color& top, const Color& bottom) noexcept {
    const float a = top.alpha;
    return {top.red * a + bottom.red * (1 - a), top.green * a + bottom.green * (1 - a),
            top.blue * a + bottom.blue * (1 - a), 1};
}
Color mix(const Color& a, const Color& b, float t) noexcept {
    return {a.red + (b.red - a.red) * t, a.green + (b.green - a.green) * t, a.blue + (b.blue - a.blue) * t,
            a.alpha + (b.alpha - a.alpha) * t};
}
// Resolved colours of one card: the profile's own theme colours when it has
// one, else the application theme.
struct TextPalette {
    Color text, strong, subtle, muted, border, on_accent;
};
struct Palette : TextPalette {
    Color primary, secondary, overlay, surface, banner, modal;
    Color hover, card, card_hover, input, button, button_hover;
    Color accent, accent_hover, bubble, bubble_border, control, control_border, control_icon;
};
Palette palette(const ProfileScene& s, const Theme& theme) noexcept {
    Palette c{};
    const auto& own = theme.tokens.profile;
    c.control = own.control;
    c.control_border = own.control_border;
    c.control_icon = own.control_icon;
    c.banner = s.has_banner_color ? color_from_rgb24(s.banner_color) : own.banner;
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
    c.primary = color_from_rgb24(s.theme_primary);
    c.secondary = color_from_rgb24(s.theme_secondary);
    c.overlay = tokens.overlay;
    c.surface = over(c.overlay, c.primary);
    c.modal = color_from_rgb24(profile_modal_background(s.theme_primary, s.theme_secondary, s.theme_dark));
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
    c.accent = color_from_rgb24(profile_button_color(s.theme_primary));
    // Hover: the accent mixed 10% toward the darkest (dark theme) or lightest surface.
    c.accent_hover = mix(c.accent, s.theme_dark ? kDefaultTheme.base_lowest : kLightTheme.surface, .1f);
    c.accent_hover.alpha = 1;
    c.on_accent = theme.tokens.text.on_accent;
    c.bubble = over(tokens.card, c.surface);
    c.bubble_border = s.theme_dark ? theme.tokens.border.strong : tokens.border;
    return c;
}
// Only the colors consumed by tone_color and card_theme during text output.
TextPalette text_palette(const ProfileScene& s, const Theme& theme) noexcept {
    TextPalette c{};
    const bool app_dark = theme.appearance == ThemeAppearance::dark;
    const auto& tokens = (app_dark == s.theme_dark ? theme : s.theme_dark ? kDefaultTheme : kLightTheme).tokens.profile;
    c.text = c.strong = s.themed ? tokens.text : theme.text_primary;
    c.subtle = s.themed ? tokens.text_subtle : theme.text_secondary;
    c.muted = s.themed ? tokens.text_muted : theme.tokens.text.muted;
    c.border = s.themed ? tokens.border : theme.tokens.border.subtle;
    c.on_accent = s.themed ? theme.tokens.text.on_accent : theme.tokens.emphasis.text;
    return c;
}
// The application theme with the card's text colours, for shared widgets
// (edit field, badges) drawn inside a themed card.
Theme card_theme(const Theme& theme, const TextPalette& c) noexcept {
    Theme local = theme;
    local.text_primary = local.tokens.text.normal = c.text;
    local.text_secondary = local.tokens.text.muted = c.muted;
    local.tokens.border.strong = c.border;
    return local;
}
// Each helper owns the 2 KB themed copy, so the paint and text entry points
// carry none and a card without badges or an edit field never makes one.
__declspec(noinline) bool paint_badge(const ProfileScene& s, const ProfilePart& p, const UiBadgeWidget& badge,
                                      const Theme& theme, const TextPalette& c, PaintStream* paint) noexcept {
    const Theme local = card_theme(theme, c);
    return p.media == ProfileMediaKind::plugin_slot
               ? ui_plugin_slot_badge_paint({badge, p.source, static_cast<u8>(p.style), 0, 0}, s.widgets, local, paint)
               : ui_badge_paint(badge, s.widgets, local, paint);
}
__declspec(noinline) bool paint_edit(const ProfileScene& s, const Theme& theme, const TextPalette& c,
                                     const UiEditGeometry* edit, u32 dpi, PaintStream* paint) noexcept {
    return ui_edit_field_paint(paint, s.edit_rectangle, s.edit_content, s.edit, edit, Color{}, card_theme(theme, c),
                               dpi, theme.scale.radius_md);
}
__declspec(noinline) bool text_badge(const ProfileScene& s, const UiBadgeWidget& badge, const Theme& theme,
                                     const TextPalette& c, UiTextOutput* text) noexcept {
    return ui_badge_text_output(badge, s.widgets, card_theme(theme, c), text);
}
__declspec(noinline) bool text_edit(const ProfileScene& s, const Theme& theme, const TextPalette& c,
                                    const UiEditGeometry* edit, UiTextOutput* text) noexcept {
    if (!s.edit.active)
        return true;
    return ui_edit_field_text_output(text, {&s.widgets, s.message_field}, s.edit_content, s.edit, edit,
                                     s.message_placeholder.view(), card_theme(theme, c));
}
Color tone_color(const TextPalette& c, const Theme& theme, const ProfilePart& p) noexcept {
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
} // namespace

const ProfilePart* profile_scene_part(const ProfileScene& scene, UiWidgetHandle handle) noexcept {
    const auto* widget = ui_widget_manager_resolve(scene.widgets, handle);
    if (!widget || widget->id < 2 || widget->id - 2 >= scene.part_count)
        return nullptr;
    const auto& part = scene.parts[widget->id - 2];
    return ui_widget_handle_equal(part.widget, handle) ? &part : nullptr;
}
const UiBadgeWidget* profile_scene_badge(const ProfileScene& scene, const ProfilePart& part) noexcept {
    return part.badge ? &labels(scene).badges[part.badge - 1] : nullptr;
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
                        u64 self_id, const UiRect& anchor, const UiRect& viewport) noexcept {
    if (!s || !p || !p->user_id || !ui_rect_has_area(viewport))
        return false;
    if (!s->storage.base && !arena_initialize(&s->storage, sizeof(Storage), sizeof(Storage), 4096, ArenaTag::ui))
        return false;
    auto* storage = reinterpret_cast<Storage*>(s->storage.base);
    if (!ui_widget_manager_initialize(&s->widgets, {storage->widgets, kWidgets}, {storage->nodes, kWidgets}))
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
bool profile_scene_layout(ProfileScene* s, const UiRect& viewport_view, bool force) noexcept {
    const UiRect viewport = viewport_view; // the scene is written below
    if (!s || !s->active || !s->profile || !ui_rect_has_area(viewport))
        return false;
    // Guild presences that skip users->revision still move activities.
    if (!force && ui_rect_equal(s->viewport, viewport) && s->users_revision == (s->users ? s->users->revision : 0) &&
        s->activity_revision == (s->users ? s->users->activities.revision : 0) &&
        s->permission_revision == (s->state ? s->state->permission_mutations : 0))
        return true;
    s->users_revision = s->users ? s->users->revision : 0;
    s->activity_revision = s->users ? s->users->activities.revision : 0;
    s->permission_revision = s->state ? s->state->permission_mutations : 0;
    s->viewport = viewport;
    s->part_count = 0;
    labels(s).failed = false;
    labels(s).badge_count = 0;
    s->text_hit_count = 0;
    ui_widget_manager_reset(&s->widgets);
    s->bio_widget = s->name_widget = s->message_field = s->note_field = ui_widget_invalid_handle();
    reset_value(&s->secondary_viewport);
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
    s->edit.active = interactive(*s) && (s->note_editing || (!modal(*s) && !panel(*s) && can_message(*s)));
    auto root_style = ui_layout_fill_style(ui_layout_node_visible | ui_layout_node_allow_overlap);
    const auto root = ui_widget_manager_add(&s->widgets, ui_widget_invalid_handle(),
                                            {1,
                                             UiWidgetKind::container,
                                             ui_widget_enabled,
                                             {utf8_literal("UserProfile"), root_style, {}, 1}});
    if (!root.ok())
        return false;
    s->root = root.handle;
    if (!(modal(*s) ? layout_modal(s, viewport) : panel(*s) ? layout_panel(s, viewport) : layout_popout(s, viewport)))
        return false;
    // Timers tick only while one shows (the Activity tab, not scrolled away).
    const auto& l = labels(s);
    for (u32 i = 0; i < l.timer_count && !s->activity_timed; ++i) {
        const auto& t = l.timers[i];
        const u16 shown[] = {t.clock, t.bar};
        for (const u16 part : shown) {
            const auto* node = part < s->part_count ? ui_widget_manager_layout_node(s->widgets, s->parts[part].widget)
                                                    : nullptr;
            if (node && ui_rect_intersects(
                            node->rectangle, ui_rect_intersection(node->clip_rectangle, s->rectangle)))
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
            const auto glyph = profile_activity_glyph_icon(clock.timer == ProfileActivityTimer::countdown
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
        reset_value(s);
    }
}
void profile_scene_publish_text_hit(const ProfileScene& scene, UiWidgetReference owner, const UiRect& layout,
                                    const UiRect& ink, const UiRect& clip) noexcept {
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
        const float split = panel(s) ? s.banner.bottom : r.top + 2 + kPopoutBanner;
        const float t = y <= split || r.bottom <= split ? 0.f : minimum(1.f, (y - split) / (r.bottom - split));
        base = over(c.overlay, mix(c.primary, c.secondary, t));
    }
    return over(hot ? c.card_hover : c.card, base);
}
// The rounded-top banner fill (its bottom corners square).
bool banner_fill(PaintStream* paint, const UiRect& r, float radius, const Color& color) noexcept {
    if (!radius)
        return paint_stream_push(paint, r, color);
    return paint_stream_push_rounded(paint, r.left, r.top, r.right, minimum(r.bottom, r.top + radius * 2), color,
                                     radius) &&
           paint_stream_push(paint, r.left, r.top + radius, r.right, r.bottom, color);
}
// Thought bubble: two trailing circles and the rounded body.
bool bubble_paint(PaintStream* paint, const UiRect& r, const Palette& c, float radius) noexcept {
    const UiRect big{r.left + 10, r.top - 8, r.left + 30, r.top + 12}, small{r.left - 3, r.top - 15, r.left + 7, r.top - 5};
    const float small_radius = ui_rect_width(small) * .5f, big_radius = ui_rect_width(big) * .5f;
    return paint_stream_push_rounded_border(paint, small, c.bubble,
                                            c.bubble_border, small_radius, 1) &&
           paint_stream_push_rounded_border(paint, big, c.bubble,
                                            c.bubble_border, big_radius, 1) &&
           paint_stream_push_rounded_border(paint, r, c.bubble, c.bubble_border,
                                            minimum(radius, ui_rect_height(r) * .5f), 1) &&
           // The body covers the big circle's border where they join.
           paint_stream_push_rounded(paint, big.left + 1, r.top + 1, big.right - 1, big.bottom - 1, c.bubble,
                                     big_radius - 1);
}
// The DM panel's card, inline beside the chat (no shadow): the surface in a
// 1 DIP ring, or a Nitro theme's colours (P down to the banner's end, then a
// blend to S) showing as its 2 DIP border around the overlay.
bool panel_background(const ProfileScene& s, const Theme& theme, const Palette& c, PaintStream* paint) noexcept {
    const float radius = theme.scale.radius_md;
    const auto r = s.rectangle;
    if (!s.themed)
        return paint_stream_push_rounded_border(paint, r, c.surface,
                                                theme.tokens.border.subtle, radius, 1);
    const auto inner = profile_scene_card_mask(s, theme);
    const float split = minimum(r.bottom, s.banner.bottom);
    return paint_stream_push_rounded(paint, r, c.primary, radius) &&
           (split >= r.bottom ||
            paint_stream_push_gradient(paint, {r.left, split, r.right, r.bottom}, c.primary, c.secondary, radius)) &&
           paint_stream_push_rounded(paint, inner.rectangle, c.overlay, inner.radius);
}
bool card_background(const ProfileScene& s, const Theme& theme, const Palette& c, PaintStream* paint) noexcept {
    if (panel(s))
        return panel_background(s, theme, c, paint);
    const float radius = theme.scale.radius_md;
    const auto r = s.rectangle;
    if (!s.themed)
        return ui_surface_paint(paint, r, theme, UiElevation::popover, radius, &c.surface, &theme.tokens.border.subtle);
    // A 2 DIP gradient border: P down to the banner's end, then a blend to S;
    // the inner card is the 60% overlay over it.
    const auto inner = profile_scene_card_mask(s, theme);
    const float split = minimum(r.bottom, r.top + 2 + kPopoutBanner);
    return ui_surface_shadow_paint(paint, r, theme, UiElevation::popover, radius) &&
           paint_stream_push_rounded(paint, r, c.primary, radius) &&
           (split >= r.bottom || paint_stream_push_gradient(paint, {r.left, split, r.right, r.bottom}, c.primary,
                                                            c.secondary, radius)) &&
           paint_stream_push_rounded(paint, inner.rectangle, c.overlay, inner.radius);
}
bool modal_background(const ProfileScene& s, const Theme& theme, const Palette& c, PaintStream* paint) noexcept {
    const float radius = s.modal_rounded ? theme.scale.radius_xl : 0.f;
    const auto r = s.rectangle;
    if (!paint_stream_push(paint, s.viewport, theme.tokens.overlay.scrim_strong) ||
        !paint_stream_push_rounded(paint, r, c.modal, radius))
        return false;
    if (s.themed) {
        // Inner veil: the overlay at 80% alpha fading to 20% over the top 300 DIPs.
        Color veil = c.overlay;
        const float low = .2f * veil.alpha, high = .8f * veil.alpha;
        Color constant = veil, top = veil, clear = veil;
        constant.alpha = low;
        top.alpha = (high - low) / (1 - low);
        clear.alpha = 0;
        if (!paint_stream_push_rounded(paint, r, constant, radius) ||
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
UiIcon profile_activity_glyph_icon(ProfileActivityGlyph glyph) noexcept {
    switch (glyph) {
    case ProfileActivityGlyph::music:
        return UiIcon::music;
    case ProfileActivityGlyph::screen:
        return UiIcon::monitor;
    case ProfileActivityGlyph::timer:
        return UiIcon::timer;
    case ProfileActivityGlyph::party:
        return UiIcon::members;
    case ProfileActivityGlyph::episode:
    case ProfileActivityGlyph::stream:
        return UiIcon::video;
    case ProfileActivityGlyph::competition:
        return UiIcon::crown;
    default:
        return UiIcon::controller;
    }
}
ProfileCardMask profile_scene_card_mask(const ProfileScene& s, const Theme& theme) noexcept {
    if (modal(s)) {
        const float radius = theme.scale.radius_xl;
        return {{s.card.left, s.card.top, s.card.right, s.card.bottom + radius}, radius};
    }
    // The panel's card keeps its 2 DIP border themed or not.
    if (panel(s))
        return {s.card, maximum(0.f, theme.scale.radius_md - kPanelBorder)};
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
    if (!paint_stream_push_clip(paint, s.rectangle))
        return false;
    // Banner colour (the first theme colour, else the accent) under its image, and the
    // avatar's cut-out disc in the card colour.
    bool ok = paint_stream_push_clip(paint, s.card) &&
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
        const auto* badge = profile_scene_badge(s, p);
        if (p.look == ProfileLook::plain && !badge && p.media != ProfileMediaKind::activity_large &&
            p.media != ProfileMediaKind::activity_progress)
            continue;
        if (p.look == ProfileLook::header_button || p.look == ProfileLook::icon)
            continue;
        const auto* n = ui_widget_manager_layout_node(s.widgets, p.widget);
        if (!n)
            return false;
        const auto r = n->rectangle;
        const auto visible = ui_rect_intersection(n->clip_rectangle, s.rectangle);
        if (!ui_rect_intersects(r, visible))
            continue;
        if (badge) {
            ok = paint_badge(s, p, *badge, theme, c, paint);
            continue;
        }
        if (!paint_stream_push_clip(paint, visible))
            return false;
        const bool hot = hovered(s, p);
        switch (p.look) {
        case ProfileLook::card: {
            const bool activity = p.media == ProfileMediaKind::activity_card;
            ok = paint_stream_push_rounded(paint, r,
                                           (activity ? card_hot(s, p) : hot) ? c.card_hover : c.card,
                                           activity ? activity_radius : card_radius);
            break;
        }
        case ProfileLook::peek: {
            // Only the strip below the first card shows.
            const float cut = r.top + 8;
            ok = paint_stream_push_clip(paint, r.left, cut, r.right, r.bottom) &&
                 paint_stream_push_rounded(paint, r, hot ? c.card_hover : c.card,
                                           activity_radius) &&
                 paint_stream_pop_clip(paint);
            break;
        }
        case ProfileLook::row:
            ok = !hot || paint_stream_push_rounded(paint, r, c.hover, theme.scale.radius_sm);
            break;
        case ProfileLook::role: {
            const Color border = c.border;
            const float cy = (r.top + r.bottom) * .5f;
            ok = paint_stream_push_rounded_border(paint, r, Color{}, border,
                                                  theme.scale.radius_md, 1) &&
                 paint_stream_push_rounded(paint, r.left + 6, cy - 6, r.left + 18, cy + 6,
                                           p.color.alpha ? p.color : c.subtle, theme.scale.radius_full);
            break;
        }
        case ProfileLook::pill:
            ok = paint_stream_push_rounded_border(paint, r, hot ? c.hover : Color{},
                                                  c.border, theme.scale.radius_md, 1);
            break;
        case ProfileLook::tab:
            if (p.source == s.tab || hot) {
                const Color bar = p.source == s.tab ? (s.themed ? c.strong : theme.tokens.text.link) : c.subtle;
                ok = paint_stream_push_rounded(paint, r.left, r.bottom - 2, r.right, r.bottom, bar, theme.scale.radius_xs);
            }
            break;
        case ProfileLook::dot:
            ok = paint_stream_push_rounded(paint, r, large ? c.subtle : c.text,
                                           theme.scale.radius_full);
            break;
        case ProfileLook::rule:
            // A profile's own theme column keeps its border; the application theme's cards take the separator.
            ok = paint_stream_push(paint, r, s.themed ? c.border : ui_separator_color(theme));
            break;
        case ProfileLook::input:
            ok = paint_stream_push_rounded(paint, r, c.input, theme.scale.radius_md);
            break;
        case ProfileLook::primary:
            ok = paint_stream_push_rounded(paint, r, hot ? c.accent_hover : c.accent,
                                           theme.scale.radius_md);
            break;
        case ProfileLook::secondary: {
            // Ask to Join stays disabled: no hover.
            const bool lit = hot && p.action != ProfileAction::activity_join;
            ok = paint_stream_push_rounded(paint, r, lit ? c.button_hover : c.button,
                                           p.media == ProfileMediaKind::activity_action ? inner_radius
                                                                                        : theme.scale.radius_md);
            break;
        }
        case ProfileLook::bubble:
            ok = bubble_paint(paint, r, c, theme.scale.radius_xl);
            break;
        case ProfileLook::box:
            ok = paint_stream_push_rounded_border(paint, r, c.card, c.border,
                                                  theme.scale.radius_md, 1);
            break;
        default:
            break;
        }
        if (ok && p.media == ProfileMediaKind::activity_large)
            // Artwork placeholder; the image draws over it once decoded.
            ok = paint_stream_push_rounded(paint, r, c.card, inner_radius) &&
                 ui_icon_paint(paint,
                               profile_activity_glyph_icon(
                                   s.activity_set && p.source < s.activity_set->count
                                       ? profile_activity_placeholder(s.activity_set->items[p.source].type)
                                       : ProfileActivityGlyph::game),
                               ui_rect_inset(r, {ui_rect_width(r) * .3f, ui_rect_height(r) * .3f,
                                                 ui_rect_width(r) * .3f, ui_rect_height(r) * .3f}),
                               c.muted);
        else if (ok && p.media == ProfileMediaKind::activity_progress) {
            const float fill = r.left + maximum(4.f, ui_rect_width(r) * maximum(0.f, minimum(1.f, p.color.red)));
            ok = paint_stream_push_rounded(paint, r, c.border, theme.scale.radius_xs) &&
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
    if (!paint_stream_push_clip(paint, s.rectangle))
        return false;
    bool ok = true;
    bool decoration = false;
    for (u32 i = 0; i < s.part_count; ++i)
        decoration |= s.parts[i].media == ProfileMediaKind::decoration;
    // Over the banner image: the avatar's cut-out ring (a decoration covers
    // it) and the status bubble where it overlaps the banner.
    const auto banner = ui_rect_intersection(s.banner, s.card);
    if (ui_rect_has_area(banner)) {
        ok = paint_stream_push_clip(paint, banner);
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
            ok = paint_stream_push_rounded(paint, s.avatar,
                                           theme.tokens.overlay.shadow, ui_rect_width(s.avatar) * .5f);
    }
    // Status: the cut-out (a phone shape when mobile), then the dot.
    if (ok && ui_rect_has_area(s.status)) {
        const float ring = s.avatar_stroke, extent = ui_rect_width(s.status);
        if (s.presence_mobile && s.presence == UiPresence::online) {
            const auto g = ui_presence_mobile_geometry(s.status, ring);
            ok = paint_stream_push_rounded(paint, g.cutout, c.surface,
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
        if (!ui_rect_intersects(ui_rect_inset(r, {-ring, -ring, -ring, -ring}), clip))
            continue;
        bool hot = false;
        for (u32 k = 0; k < s.part_count && !hot; ++k)
            hot = s.parts[k].media == ProfileMediaKind::activity_card && s.parts[k].source == p.source &&
                  card_hot(s, s.parts[k]);
        const float half = ui_rect_width(r) * .5f;
        ok = paint_stream_push_clip(paint, clip) &&
             paint_stream_push_rounded_border(paint, r.left - ring, r.top - ring, r.right + ring, r.bottom + ring,
                                              Color{}, card_backdrop(s, c, (r.top + r.bottom) * .5f, hot),
                                              half + ring, ring) &&
             paint_stream_pop_clip(paint);
    }
    if (ok && s.edit.active)
        ok = paint_edit(s, theme, c, edit, dpi, paint);
    // Ordinary card icons share the content's layer. Only header controls
    // and the focus indicator sit above the profile effect.
    for (u32 header = 0; ok && header < 2; ++header) {
        if (header && controls_first_batch)
            *controls_first_batch = paint_stream_seal(paint);
        for (u32 i = 0; ok && i < s.part_count; ++i) {
            const auto& p = s.parts[i];
            const bool control = p.look == ProfileLook::header_button || p.look == ProfileLook::icon ||
                                 p.look == ProfileLook::pill;
            if (!control || p.icon == kProfileNoIcon ||
                (p.look == ProfileLook::header_button) != (header != 0))
                continue;
            const auto* n = ui_widget_manager_layout_node(s.widgets, p.widget);
            if (!n)
                return false;
            const auto r = n->rectangle;
            const auto visible = ui_rect_intersection(n->clip_rectangle, s.rectangle);
            if (!ui_rect_intersects(r, visible))
                continue;
            const bool hot = hovered(s, p);
            // The note button shows only while the card is hovered or focused.
            if (p.action == ProfileAction::open_note && !s.hovered && !ui_widget_handle_equal(s.widgets.focused, p.widget))
                continue;
            ok = paint_stream_push_clip(paint, visible);
            Color glyph = tone_color(c, theme, p);
            float size = 16;
            if (p.look == ProfileLook::header_button) {
                const float half = ui_rect_width(r) * .5f;
                ok = ok && paint_stream_push_rounded_border(paint, r,
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
    const auto c = text_palette(s, theme);
    for (u32 i = 0; i < s.part_count; ++i) {
        const auto& p = s.parts[i];
        if (const auto* badge = profile_scene_badge(s, p)) {
            if (!text_badge(s, *badge, theme, c, text))
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
        if (!ui_rect_intersects(r, clip))
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
                       p.wrap ? TextWrapping::wrap : TextWrapping::no_wrap,
                       TextOverflow::ellipsis,
                       style ? profile_name_face(style->name.font_id) : UiTextFace::normal,
                       // The always-shown panel animates its name only under the pointer.
                       style ? profile_name_effect(style->name, !panel(s) || s.hovered,
                                                   panel(s) ? s.hover_changed_ms : s.name_started_ms)
                             : UiTextEffect{},
                       bio ? ConstSlice<TextStyleSpan>{s.bio_styles, s.bio_style_count} : ConstSlice<TextStyleSpan>{}}))
            return false;
    }
    return text_edit(s, theme, c, edit, text);
}
namespace {
void describe_access(void* context, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget& widget,
                     const UiLayoutNode&, UiAccessWidgetInfo* info) noexcept {
    const auto& s = *static_cast<const ProfileScene*>(context);
    if (widget.id == 1) {
        info->role = UiAccessRole::dialog;
        info->name = utf8_literal("User profile");
        return;
    }
    const auto* part = profile_scene_part(s, handle);
    if (!part || !part->hint.bytes)
        return;
    info->name = part->hint;
    if (part->media != ProfileMediaKind::badge)
        return;
    const bool link = s.profile && part->source < s.profile->badge_count && s.profile->badges[part->source].link.bytes;
    info->role = link ? UiAccessRole::link : UiAccessRole::image;
    info->actions = ui_access_default_actions(info->role);
}
} // namespace
u32 profile_scene_access(UiAccessTree* tree, u32 parent, const ProfileScene& s, u8 surface,
                         bool focus_owner) noexcept {
    if (!tree || !s.active || !s.widgets.count)
        return kUiAccessNone;
    return ui_access_add_widgets(tree, parent, s.widgets,
                                 {describe_access, const_cast<ProfileScene*>(&s), surface, focus_owner});
}
bool profile_scene_animating(const ProfileScene* scene, u64 now) noexcept {
    if (!scene || !scene->active || !scene->effect || !scene->effect->sku)
        return false;
    // An effect plays only once the media pump found every layer resident and started it. Until
    // then (or when a layer never loads) the card draws no effect, and a looping layer must not
    // keep the frame loop running on its own: a failed or pending asset never holds frames hot.
    if (scene->reduced_motion || scene->effect_started_ms == 0)
        return false;
    if (scene->hover_changed_ms && now < scene->hover_changed_ms + 600)
        return true;
    for (u32 i = 0; i < scene->effect->count; ++i) {
        const auto& layer = scene->effect->layers[i];
        if (layer.loop || now < scene->effect_started_ms + layer.start_ms + layer.duration_ms)
            return true;
    }
    return false;
}

} // namespace maicord
