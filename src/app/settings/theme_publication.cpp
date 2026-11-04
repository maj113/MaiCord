#include "../shell/state.hpp"
#include "../effects/effects.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "custom_theme.hpp"
#include "theme_publication.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {
// The scheduled look now, on the effects' clock. After a change the clock doesn't make minute by minute (a change of
// base or month, a jump of the clock) it glides from what showed, as the effects' appearance does beside it.
Theme scheduled_palette(ThemePublication& p, const ClientThemeDocument& document) noexcept {
    const u64 now = clock_monotonic_milliseconds();
    u64 next = 0;
    const EffectLocalTime local = application_effects_local_time(now, &next);
    const u32 minute = local.hour * 60u + local.minute, sample = minute + 1440u * (local.day + 32u * local.month);
    Theme target = client_theme_document_at(document, minute, local.month, local.day);
    const bool reduced = g_app.preferences.reduced_motion || g_app.renderer.reduced_motion;
    if (p.scheduled && p.ready && !reduced &&
        (sample - p.schedule_sample > 2u || p.theme().appearance != target.appearance)) {
        ::new (static_cast<void*>(p.ease_from_bytes)) Theme(p.theme());
        p.ease_start_ms = now;
    }
    p.schedule_sample = sample;
    p.schedule_next_ms = next;
    const u64 elapsed = now - p.ease_start_ms;
    if (p.ease_start_ms && !reduced && elapsed < kEffectAmbientEaseMs) {
        const float t = static_cast<float>(elapsed) / static_cast<float>(kEffectAmbientEaseMs);
        client_theme_mix(&target, *__builtin_launder(reinterpret_cast<const Theme*>(p.ease_from_bytes)),
                         t * t * (3 - 2 * t));
        p.schedule_next_ms = now + 16;
    } else {
        p.ease_start_ms = 0;
    }
    return target;
}
} // namespace

u64 application_theme_schedule_wake() noexcept {
    return theme_publication.scheduled ? theme_publication.schedule_next_ms : 0;
}
void application_theme_schedule_tick(u64 now_ms) noexcept {
    auto& p = theme_publication;
    const auto* document = application_custom_theme_document();
    if (p.scheduled && now_ms >= p.schedule_next_ms && document && !application_custom_theme_publish(document))
        p.schedule_next_ms = now_ms + 1000; // a refused publication tries again in a second, not in a spin
}

bool application_custom_theme_publish(const ClientThemeDocument* document) noexcept {
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

    Theme palette = !document ? client_theme_projection(g_app.preferences)
                    : client_theme_document_scheduled(*document) ? scheduled_palette(publication, *document)
                                                                 : document->theme;
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

} // namespace maicord::app_detail
