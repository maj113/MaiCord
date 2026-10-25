#include "application_state.hpp"
#include "maicord/core/memory.hpp"
#include "application_custom_theme.hpp"
#include "application_theme_publication.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {

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

    Theme palette = document ? document->theme : client_theme_projection(g_app.preferences);
    // Gradient-derived accent tokens live only on the published copy.
    ui_theme_resolve_emphasis(&palette);
    // Imported serialized documents cannot transfer any runtime style pointer.
    palette.styles = nullptr;
    palette.style_revision = 0;
    // Built-in defaults by reference: conditional copies of these banks
    // expand into inline vector loops.
    static constexpr UiThemeTypography default_typography{};
    static constexpr UiThemeMetrics default_metrics{};
    static constexpr UiThemeIcons default_icons{};
    static constexpr UiThemeSettings default_settings{};
    const UiThemeTypography& typography = document ? document->typography : default_typography;
    const UiThemeMetrics& metrics = document ? document->metrics : default_metrics;
    const UiThemeMetrics previous_metrics =
        publication.styles ? publication.styles->metrics : UiThemeMetrics{};
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
        const UiThemeSettings& settings = document ? document->settings : default_settings;
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
            VirtualAlloc(nullptr, sizeof(UiResolvedThemeStyles), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
                                    !ui_theme_palette_equal(publication.theme, palette) ||
                                    publication.theme.appearance != palette.appearance;
    const bool background_changed =
        !publication.ready || !background_theme_equal(publication.theme.background, palette.background);

    if (destination)
        copy_bytes(destination, &candidate, sizeof(candidate));
    palette.styles = destination;
    palette.style_revision = destination ? destination->revision : 0;
    publication.styles = destination;
    publication.theme = palette;
    publication.ready = true;
    publication.custom = document != nullptr;
    remember_preferences(publication, g_app.preferences);
    if (geometry_changed) {
        if (!g_app.channel_pane_width || g_app.channel_pane_width == previous_metrics.shell_sidebar_width)
            g_app.channel_pane_width = metrics.shell_sidebar_width;
        if (!g_app.member_pane_width || g_app.member_pane_width == previous_metrics.shell_member_width)
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
