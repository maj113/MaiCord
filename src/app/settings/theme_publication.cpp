#include "../shell/state.hpp"
#include "../shell/window_state.hpp"
#include "../effects/effects.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "custom_theme.hpp"
#include "theme_publication.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {
// The theme's effect's month and phase knobs (application_effects_drawn_knob); a change makes the schedule due at once,
// so the theme glides to a held month or phase as to any change the user makes.
void follow_knobs(ThemePublication& p, const ClientThemeDocument& document) noexcept {
    const auto& effect = document.effect;
    const float month = application_effects_drawn_knob(effect.slug, effect.month_knob),
                phase = application_effects_drawn_knob(effect.slug, effect.phase_knob);
    if (month != p.follow_month || phase != p.follow_phase) {
        p.follow_month = month;
        p.follow_phase = phase;
        p.follow_changed = true;
        p.schedule_next_ms = 1;
    }
}
// A change can be seen moving: a frame has shown, the window shows, and motion is not reduced.
bool glide_allowed() noexcept {
    return g_app.renderer.has_presented && application_window_shown() && !application_reduced_motion();
}
// Glides run on the performance counter: GetTickCount64 (clock_monotonic_milliseconds) moves in 16 ms steps, which
// would hold one colour over two display frames and jump on the next.
u64 glide_now() noexcept {
    return application_now_us() / 1000;
}
// The look the source asks for now: the built-in preferences' projection, a document, or a theme with variants on
// the effects' clock. A publication (glide_ms not null) moves the clock's schedule on and sets how long a scheduled
// look's own change glides, unless the caller set it: a month or phase the knobs hold as the user's change, a jump of
// the clock or a light/dark flip over the effects' appearance glide, and the minutes as they come at once.
Theme look(ThemePublication& p, const ClientThemeDocument* document, u32* glide_ms) noexcept {
    if (!document)
        return client_theme_projection(g_app.preferences);
    if (!client_theme_document_scheduled(*document))
        return document->theme;
    u64 next = 0;
    const EffectLocalTime local = application_effects_local_time(clock_monotonic_milliseconds(), &next);
    follow_knobs(p, *document);
    const ClientThemeClock clock = client_theme_follow(
        document->effect, {local.hour * 60u + local.minute, local.month, local.day}, p.follow_month, p.follow_phase);
    const Theme target = client_theme_document_at(*document, clock.minute, clock.month, clock.day);
    if (!glide_ms)
        return target;
    const u32 sample = clock.minute + 1440u * (clock.day + 32u * clock.month);
    if (!*glide_ms && p.follow_changed)
        *glide_ms = kClientThemeGlideMs;
    else if (!*glide_ms && p.scheduled &&
             (sample - p.schedule_sample > 2u || p.theme().appearance != target.appearance))
        *glide_ms = kEffectAmbientEaseMs;
    p.follow_changed = false;
    p.schedule_sample = sample;
    p.schedule_next_ms = next;
    return target;
}
const Theme& glide_from(const ThemePublication& p) noexcept {
    return *__builtin_launder(reinterpret_cast<const Theme*>(p.ease_from_bytes));
}
// *palette as a glide of ease_ms from `from`, started at start_ms, shows it now. The glide's length while it runs, 0
// once it is over or can't be seen.
u32 glide(Theme* palette, const Theme& from, u64 start_ms, u32 ease_ms, u64 now) noexcept {
    return ease_ms && glide_allowed() && client_theme_glide(palette, from, now - start_ms, ease_ms) ? ease_ms : 0;
}
bool publish(const ClientThemeDocument* document, u32 glide_ms) noexcept;
// One frame of a glide: only the colours and the background change; the published styles and fonts stay.
void glide_frame(ThemePublication& p, const ClientThemeDocument* document) noexcept {
    const u64 now = glide_now();
    Theme palette = look(p, document, nullptr);
    p.ease_ms = glide(&palette, glide_from(p), p.ease_start_ms, p.ease_ms, now);
    ui_theme_resolve_emphasis(&palette);
    palette.styles = p.styles;
    palette.style_revision = p.styles ? p.styles->revision : 0;
    const Theme& shown = p.theme();
    const bool changed = !ui_theme_palette_equal(shown, palette) || shown.appearance != palette.appearance ||
                         !background_theme_equal(shown.background, palette.background);
    p.set_theme(palette);
    if (changed)
        (void)application_refresh_theme_consumers(false, false);
}
} // namespace

u64 application_theme_schedule_wake() noexcept {
    auto& p = theme_publication;
    const auto* document = application_custom_theme_document();
    if (!p.scheduled || !document)
        return 0;
    follow_knobs(p, *document);
    return p.schedule_next_ms;
}
bool application_theme_transition_active() noexcept {
    return theme_publication.ease_ms != 0;
}
void application_theme_schedule_tick(u64 now_ms, bool frame) noexcept {
    auto& p = theme_publication;
    const auto* document = application_custom_theme_document();
    if (p.scheduled && document) {
        follow_knobs(p, *document);
        if (now_ms >= p.schedule_next_ms) {
            if (!publish(document, 0))
                p.schedule_next_ms = now_ms + 1000; // a refused publication tries again in a second, not in a spin
            return;
        }
    }
    // A glide steps once per display frame (or at once where it can't be seen); the source it started from must
    // still be the one published.
    if (p.ease_ms && (frame || !glide_allowed())) {
        if ((document != nullptr) == p.custom)
            glide_frame(p, document);
        else
            p.ease_ms = 0;
    }
}

bool application_custom_theme_publish(const ClientThemeDocument* document, bool animate) noexcept {
    return publish(document, animate ? kClientThemeGlideMs : 0);
}

namespace {
// Publishes the source's look (application_custom_theme_publish). A change of colours from what shows glides for
// glide_ms (0: only as a scheduled look's own change asks, see look()); a glide under way carries on to the new look.
bool publish(const ClientThemeDocument* document, u32 glide_ms) noexcept {
    auto& publication = theme_publication;
    if (publication.publishing ||
        (document ? !client_theme_document_valid(*document) : !client_preferences_valid(g_app.preferences)))
        return false;
    publication.publishing = true;
    struct EndPublication {
        bool& flag;
        ~EndPublication() noexcept {
            flag = false;
        }
    } end{publication.publishing};

    const u64 now = glide_now();
    Theme palette = look(publication, document, &glide_ms);
    // The glide this publication shows, kept only if it succeeds. Only colours glide: a change of background alone
    // (the theme editor's gradient, a slider) follows the pointer.
    alignas(Theme) u8 started_from[sizeof(Theme)];
    const Theme* from = &glide_from(publication);
    u64 ease_start_ms = publication.ease_start_ms;
    u32 ease_ms = publication.ease_ms;
    if (publication.ready && glide_ms && glide_allowed() && !ui_theme_palette_equal(publication.theme(), palette)) {
        from = ::new (static_cast<void*>(started_from)) Theme(publication.theme());
        ease_start_ms = now;
        ease_ms = glide_ms;
    }
    ease_ms = glide(&palette, *from, ease_start_ms, ease_ms, now);
    // Gradient-derived accent tokens live only on the published copy.
    ui_theme_resolve_emphasis(&palette);
    // Imported serialized documents cannot transfer any runtime style pointer.
    palette.styles = nullptr;
    palette.style_revision = 0;
    // Built-in defaults by reference: conditional copies of these banks
    // expand into inline vector loops.
    static constexpr UiThemeTypography default_typography{};
    static constexpr UiThemeIcons default_icons{};
    const UiThemeTypography& typography = document ? document->typography : default_typography;
    const UiThemeMetrics& metrics = document ? document->metrics : kUiThemeMetricsDefault;
    // Read before the commit below overwrites the published bank.
    const UiThemeMetrics& previous = publication.styles ? publication.styles->metrics : kUiThemeMetricsDefault;
    const float previous_sidebar_width = previous.shell_sidebar_width,
                previous_member_width = previous.shell_member_width;
    UiResolvedThemeStyles candidate;
    if (publication.styles)
        copy_value(&candidate, *publication.styles);
    else
        reset_value(&candidate);
    UiThemeStylesResult styles_result{};
    if (document || publication.styles) {
        styles_result = ui_theme_styles_apply(&candidate, typography, metrics);
        if (!styles_result.ok())
            return false;
        // Component tokens are paint-only data beside the typed banks.
        const UiThemeIcons& icons = document ? document->icons : default_icons;
        const UiThemeSettings& settings = document ? document->settings : kUiThemeSettingsDefault;
        if (!bytes_equal(&candidate.icons, &icons, sizeof(icons)) ||
            !bytes_equal(&candidate.settings, &settings, sizeof(settings))) {
            candidate.icons = icons;
            candidate.settings = settings;
            candidate.revision = candidate.revision == ~0u ? 1 : candidate.revision + 1;
            candidate.paint_revision = candidate.paint_revision == ~0u ? 1 : candidate.paint_revision + 1;
            styles_result.changes |= ui_theme_style_paint;
        }
    }
    auto* destination = publication.styles;
    bool allocated = false;
    if (document && !destination) {
        destination = static_cast<UiResolvedThemeStyles*>(
            commit_pages(sizeof(UiResolvedThemeStyles)));
        if (!destination)
            return false;
        allocated = true;
    }
    // Every fallible operation finishes while the published palette/bank and
    // renderer remain intact. Commit validates the exact prepared source owner.
    RendererTypographyCandidate fonts{};
    const u32 previous_typography = renderer_typography_revision(g_app.renderer);
    if (!renderer_prepare_typography(&g_app.renderer, typography, &fonts) ||
        !renderer_commit_typography(&g_app.renderer, &fonts)) {
        renderer_discard_typography(&fonts);
        if (allocated)
            VirtualFree(destination, 0, MEM_RELEASE);
        return false;
    }
    const bool typography_changed = previous_typography != renderer_typography_revision(g_app.renderer);
    const bool geometry_changed = (styles_result.changes & ui_theme_style_geometry) != 0;
    const bool foreground_changed = !publication.ready ||
                                    !ui_theme_palette_equal(publication.theme(), palette) ||
                                    publication.theme().appearance != palette.appearance;
    const bool background_changed =
        !publication.ready || !background_theme_equal(publication.theme().background, palette.background);

    if (destination)
        copy_bytes(destination, &candidate, sizeof(candidate));
    palette.styles = destination;
    palette.style_revision = destination ? destination->revision : 0;
    publication.styles = destination;
    publication.set_theme(palette);
    publication.ready = true;
    publication.custom = document != nullptr;
    publication.scheduled = document && client_theme_document_scheduled(*document);
    remember_preferences(publication, g_app.preferences);
    if (from != &glide_from(publication))
        copy_bytes(publication.ease_from_bytes, started_from, sizeof(Theme));
    publication.ease_start_ms = ease_start_ms;
    publication.ease_ms = ease_ms;
    if (geometry_changed) {
        if (!g_app.channel_pane_width || g_app.channel_pane_width == previous_sidebar_width)
            g_app.channel_pane_width = metrics.shell_sidebar_width;
        if (!g_app.member_pane_width || g_app.member_pane_width == previous_member_width)
            g_app.member_pane_width = metrics.shell_member_width;
    }
    if (typography_changed || styles_result.changed() || foreground_changed || background_changed) {
        // This hook only withdraws stale geometry/cache authority and schedules
        // refresh. It must not recursively synchronize the custom owner or
        // reject a font transaction which is already committed.
        (void)application_refresh_theme_consumers(typography_changed, geometry_changed);
        application_request_frame();
    }
    return true;
}
} // namespace

} // namespace maicord::app_detail
