#include "background_theme_internal.hpp"
#include "theme_views_probe.hpp"
#include "maicord/ui/chat/messages/author.hpp"
#include "maicord/ui/guild/member_list_scene.hpp"
#include "maicord/ui/widgets/color_picker.hpp"

namespace maicord {
namespace {
bool near_background(float a, float b) noexcept {
    return a - b < .0001f && b - a < .0001f;
}

} // namespace

namespace background_theme_detail {

bool failure(u32 stage) noexcept {
    log_u64(LogLevel::error, "probe", "background_theme_failure_stage", stage);
    return false;
}

} // namespace background_theme_detail

namespace {

bool chrome_native_pixels(D3D11Renderer& r, const Theme& theme) noexcept {
    if (!r.ui_scene || !r.message_composer_scene || !r.message_composer_scene->active)
        return failure(40);
    const auto node = [&](UiSemantic semantic) noexcept {
        return ui_scene_node(*r.ui_scene, semantic);
    };
    const auto* messages = node(UiSemantic::message_view);
    const auto* root = node(UiSemantic::root);
    const auto* content = node(UiSemantic::content);
    const auto* header = node(UiSemantic::channel_header);
    const auto* footer = node(UiSemantic::channel_footer);
    const auto* composer = node(UiSemantic::composer);
    const auto* surface =
        ui_widget_manager_layout_node(r.message_composer_scene->widgets, r.message_composer_scene->surface);
    const auto* edit =
        message_composer_scene_layout_node(*r.message_composer_scene, MessageComposerControl::edit);
    if (!messages || !root || !header || !footer || !composer || !surface || !edit)
        return failure(41);
    const bool background = background_theme_valid(theme.background);
    const auto exact_quad = [&](UiRect rectangle) noexcept -> const PaintQuad* {
        for (u32 i = 0; i < r.paint_stream.count; ++i) {
            const auto& q = r.paint_stream.quads[i];
            if (ui_rect_equal({q.left, q.top, q.right, q.bottom}, rectangle))
                return &q;
        }
        return nullptr;
    };
    const auto* footer_quad = exact_quad(footer->rectangle);
    const auto* surface_quad = exact_quad(surface->rectangle);
    const auto* edit_quad = exact_quad(edit->rectangle);
    if (!footer_quad || !surface_quad || !edit_quad)
        return failure(42);
    if (background) {
        UiRect column = messages->rectangle;
        column.bottom = content ? content->rectangle.bottom : root->rectangle.bottom;
        const auto* plate = exact_quad(column);
        if (!plate || plate->border_width != -8 || plate->fill.alpha != 1 || plate->radius != 0 ||
            !near_background(plate->reserved_y,
                             1 - ui_background_tier_opacity(theme, UiBackgroundTier::chat)) ||
            footer_quad->border_width != -7 || surface_quad->border_width != -7 ||
            footer_quad->fill.alpha != 1 || surface_quad->fill.alpha != 1 || edit_quad->fill.alpha != 0)
            return failure(43);
    } else if (footer_quad->fill.alpha != 1 || surface_quad->fill.alpha != 1 || edit_quad->fill.alpha != 1)
        return failure(44);
    const float width = r.width * 96.f / r.dpi, height = r.height * 96.f / r.dpi;
    const float gap_x = (messages->rectangle.left + composer->rectangle.left) * .5f;
    const UiPoint points[]{
        {gap_x, (composer->rectangle.top + composer->rectangle.bottom) * .5f},
        {gap_x, messages->rectangle.bottom - 2},
        {gap_x, messages->rectangle.bottom + 2},
        {header->rectangle.right - 12, header->rectangle.top + 6},
        {(footer->rectangle.left + footer->rectangle.right) * .5f, footer->rectangle.top + 6},
        {(surface->rectangle.left + surface->rectangle.right) * .5f, surface->rectangle.top + 2.5f}};
    if (!d3d11_renderer_draw_stream(&r, theme, false, false, nullptr))
        return failure(45);
    PixelReadback pixels{r};
    u8 seam[2][3]{};
    for (u32 sample = 0; sample < 6; ++sample) {
        // Evaluate at the physical pixel center used by readback, so DPI and
        // fractional retained geometry do not introduce an oracle offset.
        const UiPoint point{(static_cast<u32>(points[sample].x * r.dpi / 96.f) + .5f) * 96.f / r.dpi,
                            (static_cast<u32>(points[sample].y * r.dpi / 96.f) + .5f) * 96.f / r.dpi};
        if (!ui_rect_contains(root->rectangle, point.x, point.y))
            return failure(46);
        float rgb[]{theme.base_low.red, theme.base_low.green, theme.base_low.blue};
        if (background) {
            const auto& constants = r.pipeline.background_constants;
            const float half_width = ui_rect_width(root->rectangle) * .5f,
                        half_height = ui_rect_height(root->rectangle) * .5f;
            const float dx = constants.controls[0], dy = constants.controls[1];
            const float denominator =
                2 * (half_width * (dx < 0 ? -dx : dx) + half_height * (dy < 0 ? -dy : dy));
            const float position = .5f + ((point.x - root->rectangle.left - half_width) * dx +
                                          (point.y - root->rectangle.top - half_height) * dy) /
                                             denominator;
            reference(theme, maximum(0.f, minimum(position, 1.f)), rgb);
        }
        u32 plates = 0;
        for (u32 batch = 0; batch < r.paint_stream.batch_count; ++batch) {
            const auto& b = r.paint_stream.batches[batch];
            const auto& clip = r.paint_stream.clips[b.clip_index];
            if (point.x < clip.left || point.x >= clip.right || point.y < clip.top ||
                point.y >= clip.bottom)
                continue;
            for (u32 i = b.first_quad; i < b.first_quad + b.quad_count; ++i) {
                const auto& q = r.paint_stream.quads[i];
                if (!i || q.right - q.left < 48 || q.bottom - q.top < 20 || q.fill.alpha <= 0 ||
                    !ui_rect_contains({q.left, q.top, q.right, q.bottom}, point.x, point.y))
                    continue;
                const float border_width = q.border_width == -8 ? 0
                                           : q.border_width == -7
                                               ? (q.reserved_x < 0 ? -q.reserved_x : q.reserved_x)
                                               : q.border_width;
                if ((q.border_width < 0 && q.border_width != -7 && q.border_width != -8) ||
                    point.x < q.left + border_width + 1 || point.x > q.right - border_width - 1 ||
                    point.y < q.top + border_width + 1 || point.y > q.bottom - border_width - 1) {
                    log_u64(LogLevel::error, "probe", "background_chrome_sample", sample);
                    log_u64(LogLevel::error, "probe", "background_chrome_edge_quad", i);
                    log_u64(LogLevel::error, "probe", "background_chrome_background_enabled", background);
                    log_u64(LogLevel::error, "probe", "background_chrome_point_x_millidip",
                            static_cast<u64>(point.x * 1000));
                    log_u64(LogLevel::error, "probe", "background_chrome_point_y_millidip",
                            static_cast<u64>(point.y * 1000));
                    log_u64(LogLevel::error, "probe", "background_chrome_quad_left_millidip",
                            static_cast<u64>(q.left * 1000));
                    log_u64(LogLevel::error, "probe", "background_chrome_quad_top_millidip",
                            static_cast<u64>(q.top * 1000));
                    log_u64(LogLevel::error, "probe", "background_chrome_quad_right_millidip",
                            static_cast<u64>(q.right * 1000));
                    log_u64(LogLevel::error, "probe", "background_chrome_quad_bottom_millidip",
                            static_cast<u64>(q.bottom * 1000));
                    log_u64(LogLevel::error, "probe", "background_chrome_quad_border_millidip",
                            static_cast<u64>(maximum(0.f, q.border_width) * 1000));
                    return failure(47);
                }
                float fill[]{q.fill.red, q.fill.green, q.fill.blue};
                if (q.border_width == -7 || q.border_width == -8) {
                    const float dx = r.pipeline.background_constants.controls[0],
                                dy = r.pipeline.background_constants.controls[1];
                    const float position =
                        .5f + ((point.x - width * .5f) * dx + (point.y - height * .5f) * dy) /
                                  (width * (dx < 0 ? -dx : dx) + height * (dy < 0 ? -dy : dy));
                    float gradient[3]{};
                    reference(theme, maximum(0.f, minimum(position, 1.f)), gradient);
                    const float mode = theme.appearance == ThemeAppearance::light ? 1.f : 0.f;
                    for (u32 c = 0; c < 3; ++c)
                        fill[c] = mode * (1 - q.reserved_y) + gradient[c] * q.reserved_y;
                }
                for (u32 c = 0; c < 3; ++c)
                    rgb[c] = fill[c] * q.fill.alpha + rgb[c] * (1 - q.fill.alpha);
                ++plates;
            }
        }
        if (background && ((sample <= 2 && plates != 1) || (sample >= 3 && plates != 2))) {
            log_u64(LogLevel::error, "probe", "background_chrome_sample", sample);
            log_u64(LogLevel::error, "probe", "background_chrome_plate_count", plates);
            return failure(48);
        }
        if (!pixels.color(point.x / width, point.y / height, rgb)) {
            log_u64(LogLevel::error, "probe", "background_chrome_sample", sample);
            return failure(49);
        }
        if (sample == 1 || sample == 2)
            if (!pixels.read(point.x / width, point.y / height, seam[sample - 1]))
                return failure(50);
    }
    if (background) {
        u32 discontinuity = 0;
        for (u32 c = 0; c < 3; ++c) {
            const i32 difference = seam[0][c] - seam[1][c];
            discontinuity += difference < 0 ? -difference : difference;
        }
        if (discontinuity > 12)
            return failure(51);
    }
    log_write(LogLevel::info, "probe",
              background ? "background_chrome_composer_gap_header_opaque_single_effective_tier_footer_"
                           "composer_gradient_pixels_passed"
                         : "background_chrome_disabled_original_opaque_controls_pixels_passed");
    return true;
}
struct FixtureMessageColors {
    MessageTextPresentation* messages{};
    Color colors[kMaximumMessageTextCommands]{};
    u32 count{};
    u64 signature{};
    explicit FixtureMessageColors(D3D11Renderer& r) noexcept {
        auto* value = const_cast<MessageTextPresentation*>(r.message_text);
        if (!value || value->command_count > value->commands.count ||
            value->command_count > kMaximumMessageTextCommands)
            return;
        messages = value;
        count = value->command_count;
        signature = value->projection_signature;
        for (u32 i = 0; i < count; ++i)
            colors[i] = value->commands[i].color;
    }
    ~FixtureMessageColors() noexcept {
        if (!messages || messages->command_count != count)
            return;
        for (u32 i = 0; i < count; ++i)
            messages->commands[i].color = colors[i];
        messages->projection_signature = signature;
    }
    u32 project(const Theme& theme) noexcept {
        if (!messages || messages->command_count != count)
            return 0;
        u32 primary = 0;
        for (u32 i = 0; i < count; ++i) {
            auto& color = messages->commands[i].color;
            color = colors[i];
            if (bytes_equal(&colors[i], &kDefaultTheme.text_primary, sizeof(Color))) {
                color = theme.text_primary;
                ++primary;
            } else if (bytes_equal(&colors[i], &kDefaultTheme.text_secondary, sizeof(Color)))
                color = theme.text_secondary;
        }
        messages->projection_signature = message_text_presentation_projection_signature(*messages);
        return primary;
    }
};
bool theme_editor_pixels(BackgroundThemeProbeContext c, Theme theme) noexcept {
    auto& r = c.renderer;
    FixtureMessageColors restore_colors{r};
    UiThemeEditorScene editor{};
    UiSettingsScene authority{};
    authority.active = true;
    authority.revision = 1;
    const auto* old_settings = r.settings_scene;
    const auto* old_editor = r.theme_editor_scene;
    const auto* old_focus = r.focus_owner;
    struct Retire {
        D3D11Renderer& r;
        UiThemeEditorScene& scene;
        const UiSettingsScene* settings;
        const UiThemeEditorScene* editor;
        const UiWidgetManager* focus;
        ~Retire() noexcept {
            renderer_bind_theme_editor_scene(&r, editor);
            r.settings_scene = settings;
            r.focus_owner = focus;
            ui_theme_editor_release(&scene);
        }
    } retire{r, editor, old_settings, old_editor, old_focus};
    const float width = r.width * 96.f / r.dpi, height = r.height * 96.f / r.dpi;
    auto editor_preferences = client_preferences_defaults();
    editor_preferences.theme = ClientTheme::custom_gradient;
    editor_preferences.gradient = {
        {0xcc0000, 0x00cc00, 0x0000cc, 0, 0}, 0, 3, 65, BackgroundEffect::none, 50, 25, 35};
    theme = client_theme_projection(editor_preferences);
    if (!restore_colors.project(theme))
        return failure(36);
    if (!ui_theme_editor_update(&editor, theme.background, 2, {0, 0, width, height}))
        return failure(20);
    r.settings_scene = &authority;
    renderer_bind_theme_editor_scene(&r, &editor);
    r.focus_owner = &editor.widgets;
    if (renderer_settings_page_visible(r) || !renderer_theme_editor_visible(r) || !authority.active ||
        !renderer_rebuild_content_text(&r, theme) || !probe_present(&r, theme) ||
        !d3d11_renderer_draw_stream(&r, theme, false, false, nullptr))
        return failure(21);
    if (r.theme_editor_paint_first_batch >= r.theme_editor_paint_end_batch ||
        r.theme_editor_text_first_batch >= r.theme_editor_text_end_batch ||
        r.theme_editor_paint_end_batch > r.form_paint_first_batch ||
        r.theme_editor_text_end_batch > r.content_text->batch_count)
        return failure(22);
    const auto* sv = ui_theme_editor_node(editor, UiThemeEditorElement::sv);
    const auto* hue = ui_theme_editor_node(editor, UiThemeEditorElement::hue);
    const auto* gradient = ui_theme_editor_node(editor, UiThemeEditorElement::gradient);
    if (!sv || !hue || !gradient)
        return failure(23);
    const auto sample = [&](const PixelReadback& pixels, UiRect rect, float x, float y, u32 rgb) noexcept {
        const float expected[]{((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f,
                               (rgb & 255) / 255.f};
        return pixels.color((rect.left + ui_rect_width(rect) * x) / width,
                            (rect.top + ui_rect_height(rect) * y) / height, expected);
    };
    {
        PixelReadback pixels{r};
        if (!sample(pixels, sv->rectangle, .25f, .25f, color_picker_rgb({editor.hue, .25f, .75f})))
            return failure(24);
        if (!sample(pixels, hue->rectangle, .08f, .5f, color_picker_rgb({.08f, 1, 1})))
            return failure(25);
        if (!sample(pixels, gradient->rectangle, .25f, .5f, 0x666600))
            return failure(26);
    }
    if (!ui_theme_editor_focus(&editor, UiThemeEditorElement::sv))
        return failure(27);
    ui_widget_manager_hover(&editor.widgets, editor.elements[static_cast<u32>(UiThemeEditorElement::sv)]);
    ++editor.revision;
    if (!renderer_rebuild_content_text(&r, theme) || !probe_present(&r, theme) ||
        (c.capture && !c.capture(L"background-theme-editor-dark.bmp", theme)))
        return failure(28);
    // Warm the real prefix after initial glyph-atlas admission, then mutate
    // only draft/background. The base shell/messages must stay retained.
    if (!renderer_rebuild_content_text(&r, theme))
        return failure(29);
    const u32 base_builds = r.content_text_prefix.base_builds;
    const auto base_key = r.content_text_retention.key;
    const auto* before_quads = r.content_text->quads;
    auto changed = editor.draft;
    const u32 text_builds_before = r.content_text->rebuilds;
    const u32 paint_builds_before = r.paint_stream_builds;
    const u32 text_uploads_before = r.text_instance_uploads;
    const u32 paint_uploads_before = r.paint_instance_uploads;
    const UiRect panel_before = editor.panel;
    const u32 generation_before = editor.generation;
    const u32 widgets_before = editor.widgets.count, quads_before = r.content_text->quad_count;
    UiRect rectangles_before[kUiThemeEditorElements]{}, clips_before[kUiThemeEditorElements]{};
    for (u32 i = 0; i < kUiThemeEditorElements; ++i) {
        const auto* node = ui_theme_editor_node(editor, static_cast<UiThemeEditorElement>(i));
        if (node) {
            rectangles_before[i] = node->rectangle;
            clips_before[i] = node->clip_rectangle;
        }
    }
    u32 changed_moves = 0;
    for (u32 i = 0; i < 64; ++i) {
        const UiPoint pointer{sv->rectangle.left + ui_rect_width(sv->rectangle) * (.3f + .5f * i / 63.f),
                              sv->rectangle.top + ui_rect_height(sv->rectangle) * .3f};
        if (ui_theme_editor_pointer(&editor, UiThemeEditorElement::sv, pointer, &changed))
            ++changed_moves;
    }
    if (changed_moves < 32 || !ui_rect_equal(editor.panel, panel_before) ||
        editor.generation != generation_before || editor.widgets.count != widgets_before ||
        r.content_text->quad_count != quads_before || r.content_text->rebuilds != text_builds_before ||
        r.paint_stream_builds != paint_builds_before || r.text_instance_uploads != text_uploads_before ||
        r.paint_instance_uploads != paint_uploads_before ||
        !ui_theme_editor_update(&editor, changed, editor.selected_stop, {0, 0, width, height}))
        return failure(30);
    bool geometry_stable = ui_rect_equal(editor.panel, panel_before) &&
                           editor.generation == generation_before && editor.widgets.count == widgets_before;
    for (u32 i = 0; i < kUiThemeEditorElements; ++i) {
        const auto* node = ui_theme_editor_node(editor, static_cast<UiThemeEditorElement>(i));
        geometry_stable =
            geometry_stable && (node ? ui_rect_equal(node->rectangle, rectangles_before[i]) &&
                                           ui_rect_equal(node->clip_rectangle, clips_before[i])
                                     : !ui_rect_has_area(rectangles_before[i]));
    }
    if (!geometry_stable)
        return failure(104);
    editor_preferences.gradient = changed;
    theme = client_theme_projection(editor_preferences);
    const bool foreground_changed = content_text_theme_signature(theme) != base_key.theme_signature;
    if (!restore_colors.project(theme))
        return failure(36);
    const bool published = renderer_rebuild_content_text(&r, theme) && probe_present(&r, theme);
    const u32 expected_base_builds = base_builds + (foreground_changed ? 1u : 0u);
    if (!published || r.content_text_prefix.base_builds != expected_base_builds ||
        r.content_text->quads != before_quads || r.content_text->quad_count != quads_before ||
        // A changed foreground republishes under the emitter's colour-hashed
        // key (checked for stability below), not the bare theme signature.
        (!foreground_changed &&
         r.content_text_retention.key.theme_signature != content_text_theme_signature(theme)) ||
        r.content_text_retention.key.message_signature != r.message_text->projection_signature ||
        !authority.active) {
        log_u64(LogLevel::error, "probe", "background_editor_prefix_publication_succeeded", published);
        log_u64(LogLevel::error, "probe", "background_editor_prefix_base_before", base_builds);
        log_u64(LogLevel::error, "probe", "background_editor_prefix_base_after",
                r.content_text_prefix.base_builds);
        log_u64(LogLevel::error, "probe", "background_editor_prefix_base_expected", expected_base_builds);
        log_u64(LogLevel::error, "probe", "background_editor_prefix_storage_stable",
                r.content_text->quads == before_quads);
        log_u64(LogLevel::error, "probe", "background_editor_prefix_quad_count_before", quads_before);
        log_u64(LogLevel::error, "probe", "background_editor_prefix_quad_count_after",
                r.content_text->quad_count);
        log_u64(LogLevel::error, "probe", "background_editor_prefix_theme_current",
                r.content_text_retention.key.theme_signature == content_text_theme_signature(theme));
        log_u64(LogLevel::error, "probe", "background_editor_prefix_message_current",
                r.content_text_retention.key.message_signature == r.message_text->projection_signature);
        log_u64(LogLevel::error, "probe", "background_editor_prefix_authority_active", authority.active);
        return failure(31);
    }
    // The shell emitter hashes actual command colors too. A coalesced source
    // foreground publication legitimately replaces that key. Repeating the
    // identical publication must retain its newly committed prefix exactly.
    const auto published_key = r.content_text_retention.key;
    if (!renderer_rebuild_content_text(&r, theme) ||
        r.content_text_prefix.base_builds != expected_base_builds ||
        r.content_text->quads != before_quads || r.content_text->quad_count != quads_before ||
        r.content_text_retention.key.shell_signature != published_key.shell_signature ||
        r.content_text_retention.key.message_signature != published_key.message_signature ||
        r.content_text_retention.key.theme_signature != published_key.theme_signature)
        return failure(103);
    log_write(
        LogLevel::info, "probe",
        "theme_editor_64_moves_zero_work_one_foreground_publication_and_stable_repeated_prefix_passed");
    auto* messages = const_cast<MessageTextPresentation*>(r.message_text);
    if (!messages || messages->command_count > messages->commands.count ||
        messages->command_count > kMaximumMessageTextCommands)
        return failure(36);
    auto light_preferences = client_preferences_defaults();
    light_preferences.theme = ClientTheme::custom_light_gradient;
    light_preferences.gradient = changed;
    const Theme light = client_theme_projection(light_preferences);
    UiRect body_ink{};
    const u32 projected_primary = restore_colors.project(light);
    for (u32 i = 0; i < messages->command_count; ++i) {
        auto& command = messages->commands[i];
        if (!ui_rect_has_area(body_ink) &&
            command.measurement_kind == MessageTextMeasurementKind::content && command.text.bytes) {
            body_ink = ui_rect_intersection(command.rectangle, command.clip);
            body_ink.right = minimum(body_ink.right, minimum(body_ink.left + 300.f, editor.panel.left - 8));
            body_ink.bottom = minimum(body_ink.bottom, body_ink.top + 24);
        }
    }
    UiRect timestamp_ink{};
    if (!r.message_scene || !r.chat_state || !r.user_store)
        return failure(90);
    for (u32 i = 0; i < messages->command_count; ++i) {
        const auto& command = messages->commands[i];
        bool has_subtext = false;
        for (u32 span = 0; span < command.styles.count; ++span)
            has_subtext = has_subtext || (command.styles[span].flags & text_style_subtext) != 0;
        if (!has_subtext)
            continue;
        MessageAuthorLayout header{};
        if (!message_author_layout(*r.message_scene, command, *r.chat_state, *r.user_store,
                                   r.member_list_scene ? r.member_list_scene->measure : nullptr, light,
                                   &header) ||
            !header.timestamp.text.bytes || !(header.timestamp.style_flags & text_style_subtext))
            continue;
        const auto expected_ink = ui_background_muted_text_color(light);
        if (!bytes_equal(&header.timestamp.color, &expected_ink, sizeof(Color)))
            return failure(90);
        timestamp_ink = ui_rect_intersection(header.timestamp.rectangle, header.timestamp.clip);
        timestamp_ink.right = minimum(timestamp_ink.right, editor.panel.left - 8);
        if (ui_rect_has_area(timestamp_ink))
            break;
    }
    if (!ui_rect_has_area(timestamp_ink))
        return failure(90);
    messages->projection_signature = message_text_presentation_projection_signature(*messages);
    if (!projected_primary || !ui_rect_has_area(body_ink))
        return failure(37);
    editor.light_mode = true;
    ++editor.revision;
    if (!ui_theme_editor_focus(&editor, UiThemeEditorElement::hue))
        return failure(32);
    ui_widget_manager_hover(&editor.widgets, editor.elements[static_cast<u32>(UiThemeEditorElement::hue)]);
    if (!renderer_rebuild_content_text(&r, light) || !probe_present(&r, light) ||
        !d3d11_renderer_draw_stream(&r, light, false, false, nullptr))
        return failure(33);
    {
        PixelReadback pixels{r};
        if (!pixels.dark_text_ink(body_ink, light.text_primary))
            return failure(38);
        if (!pixels.secondary_ink(timestamp_ink, ui_background_muted_text_color(light)))
            return failure(91);
    }
    if (c.capture && !c.capture(L"background-theme-editor-light.bmp", light))
        return failure(39);
    // Keep the prior Light view unchanged; the additional view exposes the
    // real inline effect controls through the drawer's own scroll projection.
    BackgroundTheme effect_draft{};
    if (!ui_theme_editor_action(&editor, UiThemeEditorElement::effect_comets, 0, &effect_draft) ||
        !ui_theme_editor_update(&editor, effect_draft, editor.selected_stop, {0, 0, width, height}) ||
        !ui_theme_editor_scroll(&editor, 5000) ||
        !ui_theme_editor_focus(&editor, UiThemeEditorElement::apply))
        return failure(74);
    const auto default_comets = background_effect_settings_defaults(BackgroundEffect::comets);
    if (effect_draft.speed != default_comets.speed || effect_draft.strength != default_comets.strength ||
        effect_draft.density != default_comets.density)
        return failure(74);
    const auto scrollbar = ui_theme_editor_scrollbar(editor);
    if (!scrollbar.visible || !ui_rect_has_area(scrollbar.thumb))
        return failure(75);
    constexpr UiThemeEditorElement effect_ranges[]{
        UiThemeEditorElement::speed, UiThemeEditorElement::strength, UiThemeEditorElement::density};
    for (const auto element : effect_ranges) {
        const auto* range = ui_theme_editor_node(editor, element);
        if (!range || !ui_rect_has_area(ui_rect_intersection(range->rectangle, range->clip_rectangle)) ||
            ui_rect_height(ui_rect_intersection(range->rectangle, range->clip_rectangle)) + .01f <
                ui_rect_height(range->rectangle) ||
            !ui_widget_manager_input_enabled(editor.widgets, editor.elements[static_cast<u32>(element)]))
            return failure(76);
    }
    ui_widget_manager_hover(&editor.widgets,
                            editor.elements[static_cast<u32>(UiThemeEditorElement::apply)]);
    ++editor.revision;
    auto effect_preferences = client_preferences_defaults();
    effect_preferences.theme = ClientTheme::custom_light_gradient;
    effect_preferences.gradient = effect_draft;
    const Theme effect_light = client_theme_projection(effect_preferences);
    if (!restore_colors.project(effect_light))
        return failure(77);
    if (!renderer_set_background_phase(&r, 80) || !renderer_rebuild_content_text(&r, effect_light) ||
        !probe_present(&r, effect_light) ||
        (c.capture && !c.capture(L"background-theme-editor-effects-light.bmp", effect_light)))
        return failure(77);
    renderer_bind_theme_editor_scene(&r, nullptr);
    if (!renderer_settings_page_visible(r) || !authority.active)
        return failure(34);
    // Dummy authority has no UI arena: restore real fixture before any draw.
    r.settings_scene = old_settings;
    ui_theme_editor_close(&editor);
    if (renderer_theme_editor_visible(r))
        return failure(35);
    log_u64(LogLevel::info, "probe", "theme_editor_scene_bytes", sizeof(editor));
    log_u64(LogLevel::info, "probe", "theme_editor_widget_committed_bytes", editor.storage.arena.committed);
    log_write(
        LogLevel::info, "probe",
        "theme_editor_native_SV_hue_stops_live_main_settings_authority_retained_prefix_dark_light_passed");
    return true;
}
// The `/` command picker's category rail (background-base-lowest)
// on every theme: flat Dark, Light and AMOLED paint their darkest base, and a
// background theme the panel's gradient material at its lowest tier, so the
// rail is never an opaque strip over the gradient.
bool command_picker_pixels(BackgroundThemeProbeContext c) noexcept {
    auto& r = c.renderer;
    const auto* composer = r.message_composer_scene;
    const auto* surface =
        composer && composer->active ? ui_widget_manager_layout_node(composer->widgets, composer->surface) : nullptr;
    if (!surface)
        return failure(150);
    PopupItem storage[8]{};
    PopupRowWidget rows[8]{};
    UiWidget widgets[24]{};
    UiLayoutNode nodes[24]{};
    PopupScene picker{};
    struct Restore {
        D3D11Renderer& r;
        PopupScene& picker;
        const PopupScene* original;
        ~Restore() noexcept {
            r.popup_scene = original;
            r.retained_presentation.valid = false;
            popup_scene_release(&picker);
        }
    } restore{r, picker, r.popup_scene};
    // An app section between Frequently Used and Built-in, so the rail draws its separator.
    PopupItem items[]{
        {utf8_literal("Frequently Used"), 200000, popup_item_enabled | popup_item_command_category},
        {utf8_literal("Test app"), 200002, popup_item_enabled | popup_item_command_category},
        {utf8_literal("Built-in"), 200001,
         popup_item_enabled | popup_item_command_category | popup_item_separator_before | popup_item_checked},
        {utf8_literal("Frequently Used"), 600000, popup_item_command_section},
        {utf8_literal("/gif"), 100000, popup_item_enabled | popup_item_command_row},
        {utf8_literal("/shrug"), 100001, popup_item_enabled | popup_item_command_row},
        {utf8_literal("Built-in"), 600001, popup_item_command_section},
        {utf8_literal("/ban"), 100002, popup_item_enabled | popup_item_command_row}};
    items[0].presentation = items[3].presentation = ui_menu_row_presentation(UiIcon::recent);
    items[1].presentation = ui_menu_row_presentation(UiIcon::link);
    items[2].presentation = items[6].presentation = ui_menu_row_presentation(UiIcon::command);
    items[4].detail = utf8_literal("Search Animated GIFs on the Web\x1F" "Built-in\x1Fquery");
    items[5].detail = utf8_literal("Appends \xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF to your message.\x1F" "Built-in\x1F");
    items[7].detail = utf8_literal("Ban user\x1F" "Built-in\x1Fuser");
    const UiRect viewport{0, 0, static_cast<float>(r.width) * 96.f / r.dpi,
                          static_cast<float>(r.height) * 96.f / r.dpi};
    if (!popup_scene_initialize(&picker, {storage, 8}, {rows, 8}, {widgets, 24}, {nodes, 24}) ||
        !popup_scene_open(&picker, PopupKind::autocomplete, 77, {items, 8}, surface->rectangle, viewport,
                          ui_rect_width(surface->rectangle), true)
             .ok() ||
        !renderer_bind_popup_scene(&r, &picker))
        return failure(151);
    // A point inside the rail card below its entries, and the panel showing
    // through the 8 DIP gaps left of the card and between the card and the body.
    const UiRect panel = picker.rectangle, card = popup_scene_picker_rail(picker);
    if (!ui_rect_has_area(card) || card.left <= panel.left || card.top <= panel.top || card.bottom >= panel.bottom ||
        popup_scene_picker_body(picker).left <= card.right)
        return failure(158);
    const float rail_x = (card.left + card.right) * .5f / viewport.right, rail_y = (card.bottom - 24) / viewport.bottom,
                outer_x = (panel.left + card.left) * .5f / viewport.right,
                seam_x = (card.right + popup_scene_picker_body(picker).left) * .5f / viewport.right;
    const auto sample = [&](const Theme& theme, const wchar_t* name, u8 (&rail)[3], u8 (&outer)[3],
                            u8 (&seam)[3]) noexcept {
        if (!renderer_rebuild_content_text(&r, theme) || !probe_present(&r, theme) ||
            !d3d11_renderer_draw_stream(&r, theme, false, false, nullptr))
            return false;
        {
            PixelReadback pixels{r};
            if (!pixels.read(rail_x, rail_y, rail) || !pixels.read(outer_x, rail_y, outer) ||
                !pixels.read(seam_x, rail_y, seam))
                return false;
        }
        return !c.capture || c.capture(name, theme);
    };
    const auto delta = [](const u8 (&a)[3], const u8 (&b)[3]) noexcept {
        u32 sum = 0;
        for (u32 i = 0; i < 3; ++i)
            sum += a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        return sum;
    };
    // Flat themes: the rail is their darkest base.
    const Theme flat[]{kDefaultTheme, kLightTheme, ui_theme_amoled()};
    const wchar_t* flat_names[]{L"commands-picker-dark.bmp", L"commands-picker-light.bmp",
                                L"commands-picker-amoled.bmp"};
    for (u32 i = 0; i < 3; ++i) {
        const Color base = flat[i].base_lowest;
        const u8 expected[3]{static_cast<u8>(base.red * 255.f + .5f), static_cast<u8>(base.green * 255.f + .5f),
                             static_cast<u8>(base.blue * 255.f + .5f)};
        u8 rail[3]{}, outer[3]{}, seam[3]{};
        if (!sample(flat[i], flat_names[i], rail, outer, seam) || delta(rail, expected) > 6)
            return failure(152 + i);
        // The card floats in the lighter panel: both gaps show it, not the rail's tone.
        if (delta(outer, rail) <= 6 || delta(seam, rail) <= 6 || delta(outer, seam) > 6)
            return failure(159 + i);
    }
    // Background themes: the rail shows the gradient through the panel's
    // material, so flipping the gradient's direction (same tokens) changes it;
    // an opaque rail would keep its pixels.
    auto preferences = client_preferences_defaults();
    preferences.theme = ClientTheme::custom_gradient;
    preferences.gradient = background_theme_defaults();
    u8 rail[3]{}, toward_rail[3]{}, away_rail[3]{}, outer[3]{}, seam[3]{};
    if (!sample(client_theme_projection(preferences), L"commands-picker-gradient.bmp", rail, outer, seam))
        return failure(155);
    preferences.gradient = {{0xff00ff, 0x00ffff, 0, 0, 0}, 90, 2, 100, BackgroundEffect::none, 50, 25, 35};
    const Theme toward = client_theme_projection(preferences);
    preferences.gradient.angle_degrees = 270;
    const Theme away = client_theme_projection(preferences);
    if (!bytes_equal(&toward.base_lowest, &away.base_lowest, sizeof(Color)) ||
        !sample(toward, L"commands-picker-magenta-cyan.bmp", toward_rail, outer, seam) ||
        !sample(away, L"commands-picker-cyan-magenta.bmp", away_rail, outer, seam))
        return failure(156);
    const u32 rail_change = delta(toward_rail, away_rail);
    log_u64(LogLevel::info, "probe", "command_picker_gradient_rail_rgb_delta", rail_change);
    if (rail_change < 24)
        return failure(157);
    log_write(LogLevel::info, "probe", "command_picker_rail_flat_and_gradient_pixels_passed");
    return true;
}

} // namespace

bool background_theme_ui_probe(BackgroundThemeProbeContext c) noexcept {
    auto& r = c.renderer;
    if (!c.rebuild || !r.content_text || r.pipeline.background_buffer || !c.rebuild() ||
        !probe_present(&r, kDefaultTheme) || r.pipeline.background_buffer)
        return failure(1);
    const u32 width = r.width, height = r.height;
    struct Restore {
        BackgroundThemeProbeContext& c;
        u32 width, height;
        ~Restore() noexcept {
            renderer_resize(&c.renderer, width, height);
            renderer_set_background_phase(&c.renderer, 0);
            c.rebuild();
            probe_present(&c.renderer, kDefaultTheme);
        }
    } restore{c, width, height};
    if (!renderer_set_background_phase(&r, 0) || !presentation_damage_empty(r.presentation_damage) ||
        renderer_set_background_phase(&r, -1))
        return failure(2);
    Theme theme = kDefaultTheme;
    theme.background = {
        {0xcc0000, 0x00cc00, 0x0000cc, 0, 0}, 0, 3, 100, BackgroundEffect::none, 50, 25, 35};
    if (!shader_pixels(r, theme, 0))
        return failure(3);
    D3D11_BUFFER_DESC buffer{};
    r.pipeline.background_buffer->GetDesc(&buffer);
    if (buffer.ByteWidth != sizeof(D3D11BackgroundConstants) || buffer.Usage != D3D11_USAGE_DEFAULT ||
        buffer.BindFlags != D3D11_BIND_CONSTANT_BUFFER)
        return failure(4);
    theme.background.angle_degrees = 90;
    if (!shader_pixels(r, theme, 0))
        return failure(5);
    theme.background = {
        {0xcc0000, 0xcccc00, 0x00cc00, 0x00cccc, 0x0000cc}, 0, 5, 100, BackgroundEffect::none, 50, 25, 35};
    if (!shader_pixels(r, theme, 0))
        return failure(6);
    theme.background = {{0xffffff, 0xffffff, 0, 0, 0}, 0, 2, 50, BackgroundEffect::none, 50, 25, 35};
    if (!shader_pixels(r, theme, 0))
        return failure(7);
    theme.background.intensity = 0;
    if (!shader_pixels(r, theme, 0))
        return failure(8);
    if (!intensity_pixels(r) || !overlay_surface_pixels(r) || !solid_pixels(r) ||
        !default_effect_pixels(r) || !dither_pixels(r) ||
        !comet_density_pixels(r) || !star_independence_pixels(r) || !effect_canvas_pixels(r) ||
        !effect_package_pixels(r))
        return false;
    theme.background = background_theme_defaults();
    theme.background.intensity = 100;
    theme.background.strength = theme.background.density = 100;
    constexpr BackgroundEffect effects[]{BackgroundEffect::stars, BackgroundEffect::comets,
                                         BackgroundEffect::stars_and_comets};
    for (u32 mode = 0; mode < 2; ++mode) {
        Theme effect_theme = mode ? kLightTheme : kDefaultTheme;
        effect_theme.background = theme.background;
        effect_theme.background.effect = BackgroundEffect::none;
        u64 still = 0;
        if (!shader_pixels(r, effect_theme, 0, &still, kDefaultTheme, true))
            return failure(57);
        for (const auto effect : effects) {
            effect_theme.background.effect = effect;
            u64 first = 0, second = 0;
            if (!shader_pixels(r, effect_theme, 0, &first, kDefaultTheme, true) ||
                !shader_pixels(r, effect_theme, 7, &second, kDefaultTheme, true) || first == second)
                return failure(58 + static_cast<u32>(effect));
            for (u32 disabled = 0; disabled < 2; ++disabled) {
                Theme zero = effect_theme;
                if (disabled)
                    zero.background.density = 0;
                else
                    zero.background.strength = 0;
                u64 inactive = 0;
                if (!shader_pixels(r, zero, 7, &inactive, kDefaultTheme, true) || inactive != still)
                    return failure(64);
            }
        }
    }
    theme.background.effect = BackgroundEffect::comets;
    if (!retained_frames(r, theme))
        return failure(13);
    const u32 updates = r.pipeline.background_updates;
    if (!renderer_set_background_phase(&r, r.background_phase_seconds) ||
        !d3d11_renderer_draw_stream(&r, theme, false, false, nullptr) ||
        r.pipeline.background_updates != updates)
        return failure(14);
    for (u32 step = 0; step < 4; ++step) {
        const u32 w = (step & 1) ? width - 1 : width + 1;
        const u32 h = (step & 1) ? height - 1 : height + 1;
        Theme static_theme = theme;
        static_theme.background.effect = BackgroundEffect::none;
        if (!renderer_resize(&r, w, h) || !c.rebuild() ||
            !renderer_rebuild_content_text(&r, static_theme) || !probe_present(&r, static_theme) ||
            !shader_pixels(r, static_theme, 0, nullptr, static_theme))
            return failure(15);
    }
    if (!renderer_resize(&r, width, height) || !c.rebuild())
        return failure(16);
    FixtureMessageColors capture_colors{r};
    auto capture_preferences = client_preferences_defaults();
    capture_preferences.theme = ClientTheme::custom_gradient;
    capture_preferences.gradient = background_theme_defaults();
    theme = client_theme_projection(capture_preferences);
    if (!capture_colors.project(theme))
        return failure(36);
    if (!full_scene_visible(r, theme))
        return failure(19);
    if (!capture_colors.project(kDefaultTheme))
        return failure(36);
    if (!renderer_rebuild_content_text(&r, kDefaultTheme) || !probe_present(&r, kDefaultTheme) ||
        !chrome_native_pixels(r, kDefaultTheme) || !capture_colors.project(theme) ||
        !renderer_rebuild_content_text(&r, theme) || !probe_present(&r, theme) ||
        !chrome_native_pixels(r, theme))
        return false;
    if (!renderer_rebuild_content_text(&r, theme) || !probe_present(&r, theme) ||
        (c.capture && !c.capture(L"background-theme-gradient.bmp", theme)))
        return failure(17);
    theme.background.effect = BackgroundEffect::stars_and_comets;
    if (!renderer_set_background_phase(&r, 6) || !renderer_rebuild_content_text(&r, theme) ||
        !probe_present(&r, theme) || (c.capture && !c.capture(L"background-theme-stars-comets.bmp", theme)))
        return failure(18);
    auto stars_preferences = client_preferences_defaults();
    stars_preferences.theme = ClientTheme::custom_gradient;
    stars_preferences.gradient = background_theme_defaults();
    stars_preferences.gradient.effect = BackgroundEffect::stars;
    const auto stars_profile = background_effect_settings_defaults(BackgroundEffect::stars);
    stars_preferences.gradient.speed = stars_profile.speed;
    stars_preferences.gradient.strength = stars_profile.strength;
    stars_preferences.gradient.density = stars_profile.density;
    const Theme stars_theme = client_theme_projection(stars_preferences);
    const wchar_t* stars_names[]{L"background-theme-stars-twinkle-a.bmp",
                                 L"background-theme-stars-twinkle-b.bmp"};
    for (u32 frame = 0; frame < 2; ++frame)
        if (!capture_colors.project(stars_theme) || !renderer_set_background_phase(&r, frame ? 7.f : 0.f) ||
            !renderer_rebuild_content_text(&r, stars_theme) || !probe_present(&r, stars_theme) ||
            (c.capture && !c.capture(stars_names[frame], stars_theme)))
            return failure(106 + frame);
    constexpr BackgroundEffect flat_effects[]{BackgroundEffect::stars, BackgroundEffect::comets,
                                              BackgroundEffect::stars_and_comets};
    const wchar_t* flat_names[]{L"background-theme-amoled-stars.bmp", L"background-theme-dark-comets.bmp",
                                L"background-theme-dark-stars-comets-high-strength.bmp"};
    for (u32 i = 0; i < 3; ++i) {
        auto preferences = client_preferences_defaults();
        preferences.theme = i == 0 ? ClientTheme::amoled : ClientTheme::dark;
        preferences.gradient.effect = flat_effects[i];
        const auto settings = background_effect_settings_defaults(flat_effects[i]);
        preferences.gradient.speed = settings.speed;
        preferences.gradient.strength = i == 2 ? 65 : settings.strength;
        preferences.gradient.density = settings.density;
        const Theme flat = client_theme_projection(preferences);
        if (ui_background_active(flat) || !background_effects_valid(flat.background) ||
            !capture_colors.project(flat) || !renderer_set_background_phase(&r, 7) ||
            !renderer_rebuild_content_text(&r, flat) || !probe_present(&r, flat) ||
            (i == 0 && !retained_frames(r, flat)) || (c.capture && !c.capture(flat_names[i], flat)))
            return failure(112 + i);
    }
    constexpr u8 comparison_intensities[]{20, 100};
    const wchar_t* comparison_names[]{L"background-theme-magenta-cyan-20.bmp",
                                      L"background-theme-magenta-cyan-100.bmp"};
    for (u32 i = 0; i < 2; ++i) {
        auto preferences = client_preferences_defaults();
        preferences.theme = ClientTheme::custom_gradient;
        preferences.gradient = {{0xff00ff, 0x00ffff, 0, 0, 0}, 180, 2,  comparison_intensities[i],
                                BackgroundEffect::none,        50,  25, 35};
        const Theme comparison = client_theme_projection(preferences);
        if (!capture_colors.project(comparison) || !renderer_set_background_phase(&r, 0) ||
            !renderer_rebuild_content_text(&r, comparison) || !probe_present(&r, comparison) ||
            !chrome_native_pixels(r, comparison) ||
            (c.capture && !c.capture(comparison_names[i], comparison)))
            return failure(100 + i);
    }
    if (!capture_colors.project(kDefaultTheme))
        return failure(36);
    // The Settings page and the themed Friends/DM/group views are independent
    // fixtures: they run (and capture) even when an editor check fails. The
    // command picker runs first, over the chat's composer.
    const bool picker_passed = command_picker_pixels(c);
    const bool editor_passed = theme_editor_pixels(c, theme);
    const bool settings_passed = settings_theme_ui_probe(c);
    const bool views_passed = theme_views_ui_probe(c);
    if (!editor_passed || !settings_passed || !views_passed || !picker_passed)
        return false;
    log_u64(LogLevel::info, "probe", "background_theme_lazy_gpu_constant_bytes", buffer.ByteWidth);
    log_u64(LogLevel::info, "probe", "background_theme_pipeline_owner_bytes",
            sizeof(D3D11DrawPipelineOwner));
    log_write(LogLevel::info, "probe",
              "background_theme_3_5_stops_direction_intensity_effect_pixels_retention_resize_passed");
    return true;
}

} // namespace maicord
