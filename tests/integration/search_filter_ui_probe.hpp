#pragma once

#include "scoped_search_ui_probe.hpp"
#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/ui/menu_row.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/theme_style_projection.hpp"
#include "maicord/ui/native_text_candidate.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace search_filter_probe_detail {
inline void failure_scalar(const char* label, u64 value) noexcept {
    char message[128]{};
    u32 bytes{};
    constexpr char prefix[] = "Search filter ";
    copy_bytes(message, prefix, sizeof(prefix) - 1);
    bytes = static_cast<u32>(sizeof(prefix) - 1);
    for (u32 i = 0; label[i] && bytes < 90; ++i)
        message[bytes++] = label[i];
    message[bytes++] = '=';
    char digits[20]{};
    u32 count{};
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits));
    while (count)
        message[bytes++] = digits[--count];
    message[bytes++] = '\n';
    DWORD written{};
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, bytes, &written, nullptr);
}
inline bool require(bool value, u32 stage) noexcept {
    if (!value) {
        log_u64(LogLevel::error, "probe", "search_filter_failure_stage", stage);
        constexpr char prefix[] = "Native Search filter probe failed: ";
        char message[64]{};
        u32 bytes = static_cast<u32>(sizeof(prefix) - 1);
        copy_bytes(message, prefix, bytes);
        char digits[10]{};
        u32 count{};
        do {
            digits[count++] = static_cast<char>('0' + stage % 10);
            stage /= 10;
        } while (stage && count < sizeof(digits));
        while (count)
            message[bytes++] = digits[--count];
        message[bytes++] = '\n';
        DWORD written{};
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, bytes, &written, nullptr);
    }
    return value;
}
struct Fixture {
    UiScene shell;
    UiSearchScene scene;
    MessageScene messages;
    GuildRailScene rail;
    ShellNavigationScene navigation;
    UiSearchFilterChoiceView rows[kUiSearchFilterChoiceMaximum];
    char labels[kUiSearchFilterChoiceMaximum][96];
    char details[kUiSearchFilterChoiceMaximum][96];
    char values[kUiSearchFilterChoiceMaximum][32];
    char heading[64], hint[96];
    bool authority;
    u32 resolves;
    UiSearchFilterChoiceSource choices;
    UiSearchSource results;
    UiSearchContent content;
    UiResolvedThemeStyles styles;
};
inline void failure_state(const D3D11Renderer& r, const Fixture& f, u32 sample,
                          const UiShellHeaderGeometry& header) noexcept {
    failure_scalar("sample", sample);
    failure_scalar("dpi", r.dpi);
    failure_scalar("shell_width", static_cast<u32>(f.shell.width));
    failure_scalar("shell_height", static_cast<u32>(f.shell.height));
    failure_scalar("title_width", static_cast<u32>(ui_rect_width(header.title)));
    failure_scalar("search_field_width", static_cast<u32>(ui_rect_width(header.search_field)));
    failure_scalar("scene_active", f.scene.active);
    failure_scalar("scene_current", ui_search_scene_current(f.scene));
    failure_scalar("scene_revision", f.scene.revision);
    failure_scalar("scene_widgets", f.scene.widgets.count);
    failure_scalar("source_revision", f.choices.revision);
    failure_scalar("visible_choices", f.scene.choice_count);
    failure_scalar("result_resolves", f.resolves);
    failure_scalar("glyph_ready", r.glyph_text.ready);
    failure_scalar("has_state", r.chat_state != nullptr);
    failure_scalar("has_navigation", r.navigation_view != nullptr);
    if (r.content_text) {
        failure_scalar("text_error", static_cast<u32>(r.content_text->last_error));
        failure_scalar("shape_error", static_cast<u32>(r.content_text->last_shape_error));
        failure_scalar("native_error", static_cast<u32>(r.content_text->native_error));
        failure_scalar("text_quads", r.content_text->quad_count);
        failure_scalar("text_batches", r.content_text->batch_count);
        failure_scalar("text_quad_capacity", r.content_text->quad_capacity);
        failure_scalar("text_batch_capacity", r.content_text->batch_capacity);
        failure_scalar("text_rejected", r.content_text->rejected);
    }
    failure_scalar("paint_quads", r.paint_stream.count);
    failure_scalar("paint_rejected", r.paint_stream.rejected);
    failure_scalar("frame_draws", r.draw_calls_last_frame);
}
template <u32 Size> inline Utf8View own(char (&destination)[Size], Utf8View source) noexcept {
    if (source.bytes > Size)
        return {};
    copy_bytes(destination, source.data, source.bytes);
    return {destination, source.bytes};
}
inline bool current(void* context) noexcept {
    return context && static_cast<Fixture*>(context)->authority;
}
inline bool result(void* context, u32 index, UiSearchRow* row) noexcept {
    if (!row || index >= 3)
        return false;
    ++static_cast<Fixture*>(context)->resolves;
    *row = {740,
            840 + index,
            utf8_literal("Underlying author"),
            utf8_literal("#native-garden"),
            utf8_literal("Original result underneath the contextual chooser."),
            utf8_literal("12:30"),
            true,
            index == 0,
            24};
    return true;
}
inline void restore_bindings(D3D11Renderer& r, const D3D11Renderer& saved) noexcept {
    r.ui_scene = saved.ui_scene;
    r.guild_rail_scene = saved.guild_rail_scene;
    r.shell_navigation_scene = saved.shell_navigation_scene;
    r.message_scene = saved.message_scene;
    r.message_text = saved.message_text;
    r.message_composer_scene = saved.message_composer_scene;
    r.member_list_scene = saved.member_list_scene;
    r.thread_header = saved.thread_header;
    r.search_scene = saved.search_scene;
    r.search_message_text = saved.search_message_text;
    r.search_message_measure_cache = saved.search_message_measure_cache;
    r.search_message_users = saved.search_message_users;
    r.focus_owner = saved.focus_owner;
    r.video_scene = saved.video_scene;
    r.audio_scene = saved.audio_scene;
    r.poll_scene = saved.poll_scene;
    r.poll_voters_scene = saved.poll_voters_scene;
    r.profile_scene = saved.profile_scene;
    r.image_viewer = saved.image_viewer;
    r.settings_scene = saved.settings_scene;
    r.theme_editor_scene = saved.theme_editor_scene;
    r.friends_scene = saved.friends_scene;
    r.friends_requests_scene = saved.friends_requests_scene;
    r.media_picker_scene = saved.media_picker_scene;
    r.quick_switcher_scene = saved.quick_switcher_scene;
    r.download_scene = saved.download_scene;
    r.popup_scene = saved.popup_scene;
    r.reaction_viewer = saved.reaction_viewer;
    r.text_form = saved.text_form;
    r.forum_form = saved.forum_form;
    r.thread_browser_scene = saved.thread_browser_scene;
    r.pinned_messages_scene = saved.pinned_messages_scene;
    r.message_forward_scene = saved.message_forward_scene;
    r.tooltip_scene = saved.tooltip_scene;
    r.message_action_bar = saved.message_action_bar;
}
inline UiWidgetHandle row_widget(const UiSearchScene& scene, u32 index) noexcept {
    for (u32 slot = 1; slot < scene.widgets.count; ++slot)
        if (scene.targets[slot].action == UiSearchAction::filter_value &&
            scene.targets[slot].source_index == index)
            return {static_cast<u16>(slot), scene.widgets.generation};
    return ui_widget_invalid_handle();
}
inline bool row_ink(const Fixture& f, const D3D11Renderer& renderer,
                    const surface_motion_probe_detail::Readback& rb, u32 index,
                    const Theme& theme) noexcept {
    UiRect rectangle{}, clip{};
    if (!ui_search_scene_filter_choice_geometry(f.scene, index, &rectangle, &clip) ||
        !ui_rect_equal(ui_rect_intersection(rectangle, f.scene.chooser), clip))
        return require(false, 1001 + index * 10);
    UiTextCommand commands[128]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    if (!ui_text_command_stream_initialize(&stream, {commands, 128}) ||
        !ui_search_scene_text_output(f.scene, theme, &output, &renderer.search_edit_geometry))
        return require(false, 1002 + index * 10);
    const auto widget = row_widget(f.scene, index);
    const UiTextCommand* primary{};
    const UiTextCommand* secondary{};
    for (u32 command = 0; command < stream.count; ++command) {
        const auto& text = commands[command];
        if (text.owner.manager != &f.scene.widgets || text.owner.handle.slot != widget.slot ||
            text.owner.handle.generation != widget.generation)
            continue;
        if (utf8_equal(text.text, f.rows[index].label))
            primary = &text;
        if (utf8_equal(text.text, f.rows[index].detail))
            secondary = &text;
    }
    if (!primary || !secondary || primary->rectangle.bottom > secondary->rectangle.top ||
        primary->wrapping != UiTextWrapping::no_wrap || secondary->wrapping != UiTextWrapping::no_wrap)
        return require(false, 1005 + index * 10);
    const UiTextCommand* lines[]{primary, secondary};
    u32 line_index{};
    for (const auto* line : lines) {
        const auto visible = ui_rect_intersection(line->rectangle, line->clip);
        if (!ui_rect_has_area(visible) || !ui_rect_equal(ui_rect_intersection(visible, clip), visible) ||
            !channel_overview_probe_detail::ink(rb, 0, visible, renderer.dpi / 96.f, line->color, 8))
            return require(false, 1003 + index * 10 + line_index * 3);
        bool native_quads{};
        for (u32 batch = 0; renderer.content_text && batch < renderer.content_text->batch_count; ++batch) {
            const auto& b = renderer.content_text->batches[batch];
            // A shared viewport scissor is valid when it leaves this label's ink unchanged.
            for (u32 q = 0; q < b.quad_count; ++q) {
                const auto& quad = renderer.content_text->quads[b.first_quad + q];
                const UiRect ink = ui_rect_intersection({quad.left, quad.top, quad.right, quad.bottom}, b.clip);
                if (!ui_rect_has_area(ui_rect_intersection(ink, visible)))
                    continue;
                if (!ui_rect_equal(ui_rect_intersection(ink, line->clip), ink) ||
                    quad.color.red != line->color.red || quad.color.green != line->color.green ||
                    quad.color.blue != line->color.blue || quad.color.alpha != line->color.alpha)
                    return require(false, 1004 + index * 10 + line_index * 3);
                native_quads = true;
            }
        }
        if (!native_quads)
            return require(false, 1004 + index * 10 + line_index * 3);
        ++line_index;
    }
    return primary->color.red != secondary->color.red || primary->color.green != secondary->color.green ||
           primary->color.blue != secondary->color.blue;
}
inline bool retired(const Fixture& f, UiSearchTarget target, UiPoint point, const Theme& theme,
                    PaintStream& paint, u32 dpi) noexcept {
    UiRect rectangle{}, clip{};
    UiTextCommand commands[128]{};
    UiTextCommandStream stream{};
    UiTextOutput text{&stream, nullptr};
    const u32 quads = paint.count, batches = paint.batch_count, depth = paint.clip_depth;
    return !ui_search_scene_current(f.scene) && !ui_search_scene_target_current(f.scene, target) &&
           ui_search_scene_hit_test(f.scene, point.x, point.y).action == UiSearchAction::none &&
           !ui_search_scene_filter_choice_geometry(f.scene, target.source_index, &rectangle, &clip) &&
           ui_text_command_stream_initialize(&stream, {commands, 128}) &&
           ui_search_scene_text_output(f.scene, theme, &text, nullptr) && !stream.count &&
           ui_search_scene_paint(f.scene, theme, &paint, nullptr, dpi, nullptr) && paint.count == quads &&
           paint.batch_count == batches && paint.clip_depth == depth;
}
inline bool row_states(const D3D11Renderer& r, const surface_motion_probe_detail::Readback& rb,
                       UiRect selected, UiRect hovered, const Theme& theme) noexcept {
    const auto selected_fill = ui_background_hover_color(theme, theme.interactive, .28f);
    const auto hovered_fill = ui_background_hover_color(theme, theme.interactive_hover, .20f);
    bool selected_quad{}, hovered_quad{};
    for (u32 i = 0; i < r.paint_stream.count; ++i) {
        const auto& q = r.paint_stream.quads[i];
        const UiRect rectangle{q.left, q.top, q.right, q.bottom};
        if (ui_rect_equal(rectangle, selected) && q.fill.red == selected_fill.red &&
            q.fill.green == selected_fill.green && q.fill.blue == selected_fill.blue &&
            q.fill.alpha == selected_fill.alpha)
            selected_quad = true;
        if (ui_rect_equal(rectangle, hovered) && q.fill.red == hovered_fill.red &&
            q.fill.green == hovered_fill.green && q.fill.blue == hovered_fill.blue &&
            q.fill.alpha == hovered_fill.alpha)
            hovered_quad = true;
    }
    const float scale = r.dpi / 96.f;
    const auto pixel = [&](UiRect rectangle) noexcept {
        const u32 x = static_cast<u32>((rectangle.right - 8) * scale);
        const u32 y = static_cast<u32>((rectangle.top + 4) * scale);
        return x < rb.width && y < rb.height ? rb.images + (y * rb.width + x) * 4 : nullptr;
    };
    const auto* a = pixel(selected);
    const auto* b = pixel(hovered);
    return selected_quad && hovered_quad && a && b && surface_motion_probe_detail::distance(a, b) > 0;
}
// The search scene is one layer above the shell: every quad it paints lies in
// [overlay_paint_first_batch, friends_requests_paint_end_batch), which draws
// after the shell's text. None may join the shell's last batch (drawn first).
inline bool search_layer_whole(const D3D11Renderer& r, const UiSearchScene& scene, const Theme& theme) noexcept {
    static PaintStream scratch{};
    static MessageMediaCommand commands[kRendererMessageMediaCommandCapacity];
    MessageMediaCommandStream media{};
    const float scale = static_cast<float>(maximum(r.dpi, 1u)) / 96.f;
    // The renderer's stream drops scrollbars outside its scrollbar viewport.
    scratch.filter_scrollbars = r.paint_stream.filter_scrollbars;
    scratch.scrollbar_viewport = r.paint_stream.scrollbar_viewport;
    const bool painted =
        paint_stream_reset(&scratch, static_cast<float>(r.width) / scale, static_cast<float>(r.height) / scale) &&
        message_media_command_stream_initialize(&media, {commands, kRendererMessageMediaCommandCapacity}) &&
        ui_search_scene_paint(scene, theme, &scratch, &r.search_edit_geometry, r.dpi, &media);
    const u32 expected = scratch.count;
    paint_stream_release(&scratch);
    u32 layer = 0;
    for (u32 b = r.overlay_paint_first_batch; b < r.friends_requests_paint_end_batch && b < r.paint_stream.batch_count;
         ++b)
        layer += r.paint_stream.batches[b].quad_count;
    if (painted && expected && layer != expected) {
        failure_scalar("search_layer_quads", layer);
        failure_scalar("search_scene_quads", expected);
    }
    return painted && expected && layer == expected;
}
inline bool warm(D3D11Renderer& renderer, Fixture& f, const Theme& theme, UiRect viewport,
                 const UiShellHeaderGeometry& header) noexcept {
    const auto before = surface_motion_probe_detail::snapshot(renderer);
    const auto native = native_text_candidate_counts(renderer.glyph_text.system);
    const u32 scene_revision = f.scene.revision, layout_revision = f.scene.widgets.layout.revision;
    const u32 resolves = f.resolves;
    const auto painted = renderer.painted_frames, skips = renderer.unchanged_present_skips;
    for (u32 frame = 0; frame < 64; ++frame)
        if (!ui_search_scene_update(&f.scene, &f.results, &f.content, viewport, header.title, &header) ||
            !probe_present(&renderer, theme) || renderer.draw_calls_last_frame ||
            !surface_motion_probe_detail::same(before, surface_motion_probe_detail::snapshot(renderer)))
            return false;
    const auto after = native_text_candidate_counts(renderer.glyph_text.system);
    return f.scene.revision == scene_revision && f.scene.widgets.layout.revision == layout_revision &&
           after.shaped == native.shaped && after.measured == native.measured &&
           after.paragraphs_shaped == native.paragraphs_shaped &&
           after.paragraphs_measured == native.paragraphs_measured &&
           after.arabic_shaped == native.arabic_shaped && after.arabic_measured == native.arabic_measured &&
           after.latin_shaped == native.latin_shaped && after.latin_measured == native.latin_measured &&
           f.resolves == resolves && renderer.painted_frames == painted &&
           renderer.unchanged_present_skips - skips == 64 &&
           renderer.media_source_refreshes == before.source_refreshes;
}
} // namespace search_filter_probe_detail

inline bool search_filter_ui_probe(D3D11Renderer& renderer,
                                   surface_motion_probe_detail::Readback& rb) noexcept {
    using namespace search_filter_probe_detail;
    Fixture f{};
    // This copy restores only borrowed bindings, never resource/GPU owners.
    const D3D11Renderer saved = renderer;
    const auto system = renderer_text_system(&renderer, UiTextFace::normal);
    const UiThemeTypography original = system.typography ? system.typography->value : UiThemeTypography{};
    struct Restore {
        D3D11Renderer& renderer;
        const D3D11Renderer& bindings;
        Fixture& fixture;
        UiThemeTypography typography;
        ~Restore() noexcept {
            RendererTypographyCandidate candidate{};
            const bool prepared = renderer_prepare_typography(&renderer, typography, &candidate);
            require(prepared && renderer_commit_typography(&renderer, &candidate), 99);
            renderer_discard_typography(&candidate);
            renderer_set_dpi(&renderer, bindings.dpi);
            restore_bindings(renderer, bindings);
            renderer.retained_presentation.valid = false;
            content_text_retention_invalidate(&renderer.content_text_retention);
            ui_search_scene_release(&fixture.scene);
            ui_scene_release(&fixture.shell);
        }
    } restore{renderer, saved, f, original};
    if (!require(ui_scene_initialize_bootstrap(&f.shell), 9))
        return false;
    constexpr Utf8View labels[]{utf8_literal("Mira"),
                                utf8_literal("Mira"),
                                utf8_literal("\xC5\xBDiva Garden"),
                                utf8_literal("Niko Meadow"),
                                utf8_literal("Robin Oak"),
                                utf8_literal("Lena Stone"),
                                utf8_literal("Toma Bay"),
                                utf8_literal("Aya Reed"),
                                utf8_literal("Eli Willow"),
                                utf8_literal("\xC5\xBDiva Orchard")};
    constexpr Utf8View details[]{utf8_literal("Garden artist"),
                                 utf8_literal("Build reviewer"),
                                 utf8_literal("Community gardener"),
                                 utf8_literal("Release writer"),
                                 utf8_literal("Local planner"),
                                 utf8_literal("Helpful member"),
                                 utf8_literal("Park caretaker"),
                                 utf8_literal("Native UI contributor"),
                                 utf8_literal("Seed collector"),
                                 utf8_literal("Orchard grower with an original garden description")};
    constexpr Utf8View values[]{utf8_literal("3000"), utf8_literal("3001"), utf8_literal("3002"),
                                utf8_literal("3003"), utf8_literal("3004"), utf8_literal("3005"),
                                utf8_literal("3006"), utf8_literal("3007"), utf8_literal("3008"),
                                utf8_literal("3009")};
    for (u32 row = 0; row < kUiSearchFilterChoiceMaximum; ++row) {
        f.rows[row].id = 3000 + row;
        f.rows[row].label = own(f.labels[row], labels[row]);
        f.rows[row].detail = own(f.details[row], details[row]);
        f.rows[row].value = own(f.values[row], values[row]);
    }
    f.authority = true;
    f.choices = {&f,
                 {f.rows, 10},
                 1,
                 0,
                 own(f.heading, utf8_literal("Choose an author")),
                 own(f.hint, utf8_literal("Choose a person. Friendly labels keep the exact saved query.")),
                 current};
    f.choices.kind = UiSearchFilterKind::from;
    f.results = {&f, 3, 1, result};
    f.content.edit = {utf8_literal("from:mi"), 7, 7, true, true, false};
    f.content.heading = utf8_literal("Search Native Garden");
    f.content.placeholder = utf8_literal("Search garden messages");
    f.content.status = utf8_literal("Original offline results");
    f.content.total = 3;
    f.content.panel = true;
    f.content.phase = UiSearchPhase::ready;
    f.content.revision = 1;
    f.content.filter_choices = &f.choices;
    renderer.ui_scene = &f.shell;
    renderer.guild_rail_scene = &f.rail;
    renderer.shell_navigation_scene = &f.navigation;
    // Ordinary shell presentation requires a scene owner even with no active
    // conversation. Its empty inactive viewport is the admitted no-model state.
    renderer.message_scene = &f.messages;
    renderer.message_text = nullptr;
    renderer.message_composer_scene = nullptr;
    renderer.member_list_scene = nullptr;
    renderer.thread_header = nullptr;
    renderer.search_scene = &f.scene;
    renderer.search_message_text = nullptr;
    renderer.search_message_measure_cache = nullptr;
    renderer.search_message_users = nullptr;
    renderer.focus_owner = nullptr;
    renderer.video_scene = nullptr;
    renderer.audio_scene = nullptr;
    renderer.poll_scene = nullptr;
    renderer.poll_voters_scene = nullptr;
    renderer.profile_scene = nullptr;
    renderer.image_viewer = nullptr;
    renderer.settings_scene = nullptr;
    renderer.theme_editor_scene = nullptr;
    renderer.friends_scene = nullptr;
    renderer.friends_requests_scene = nullptr;
    renderer.media_picker_scene = nullptr;
    renderer.quick_switcher_scene = nullptr;
    renderer.download_scene = nullptr;
    renderer.popup_scene = nullptr;
    renderer.reaction_viewer = nullptr;
    renderer.text_form = nullptr;
    renderer.forum_form = nullptr;
    renderer.thread_browser_scene = nullptr;
    renderer.pinned_messages_scene = nullptr;
    renderer.message_forward_scene = nullptr;
    renderer.tooltip_scene = nullptr;
    renderer.message_action_bar = nullptr;
    for (u32 sample = 0; sample < 4; ++sample) {
        const bool compact = sample >= 2, light = sample == 1 || sample == 3;
        const u32 dpi = sample == 1 || sample == 2 ? 144 : 96;
        renderer_set_dpi(&renderer, dpi);
        const float scale = dpi / 96.f;
        const float width = compact ? 320.f : renderer.width / scale;
        const float height = renderer.height / scale;
        auto theme = light ? kLightTheme : kDefaultTheme;
        if (sample == 2) {
            auto typography = original;
            typography.body_size = 18;
            if (!require(ui_theme_styles_apply(&f.styles, typography, UiThemeMetrics{}).ok(), 10))
                return false;
            RendererTypographyCandidate candidate{};
            const bool prepared = renderer_prepare_typography(&renderer, typography, &candidate);
            const bool applied = prepared && renderer_commit_typography(&renderer, &candidate);
            renderer_discard_typography(&candidate);
            if (!require(applied, 11))
                return false;
        }
        if (compact) {
            theme.styles = &f.styles;
            theme.style_revision = f.styles.revision;
            ui_search_scene_bind_theme_styles(&f.scene, &f.styles);
            if (!require(text_system_font_size(renderer.glyph_text.system, 0) == 18, 15 + sample))
                return false;
        }
        auto metrics = kGuildChannelShellLayoutMetrics;
        metrics.flags = 0;
        if (!require(
                ui_scene_layout_with_metrics(&f.shell, UiShellView::guild_channel, metrics, width, height),
                12))
            return false;
        const auto header = ui_scene_header_geometry(f.shell);
        const auto viewport = ui_scene_content_rectangle(f.shell, UiSemantic::content);
        f.choices.selected = sample == 0 ? 0 : 9;
        ++f.choices.revision;
        ++f.content.revision;
        if (!require(
                ui_search_scene_update(&f.scene, &f.results, &f.content, viewport, header.title, &header),
                200 + sample)) {
            failure_state(renderer, f, sample, header);
            return false;
        }
        if (!require(renderer_rebuild_content_text(&renderer, theme), 210 + sample)) {
            failure_state(renderer, f, sample, header);
            return false;
        }
        if (!require(probe_present(&renderer, theme), 220 + sample)) {
            failure_state(renderer, f, sample, header);
            return false;
        }
        const auto selected = static_cast<u32>(f.choices.selected);
        UiRect selected_rect{}, selected_clip{};
        if (!require(f.scene.choice_total == 10 && f.scene.choice_count >= 2 && f.scene.choice_count < 10 &&
                         f.scene.choice_first <= selected &&
                         selected < static_cast<u32>(f.scene.choice_first + f.scene.choice_count) &&
                         f.scene.chooser.left >= viewport.left && f.scene.chooser.right <= viewport.right &&
                         f.scene.chooser.top >= viewport.top && f.scene.chooser.bottom <= viewport.bottom &&
                         ui_search_scene_filter_choice_geometry(f.scene, selected, &selected_rect,
                                                                &selected_clip),
                     30 + sample))
            return false;
        UiRect hidden_rectangle{}, hidden_clip{};
        const u32 hidden_index = selected == 0 ? 9 : 0;
        u32 choice_widgets{};
        for (u32 slot = 1; slot < f.scene.widgets.count; ++slot)
            choice_widgets += f.scene.targets[slot].action == UiSearchAction::filter_value;
        if (!require(choice_widgets == f.scene.choice_count &&
                         (selected == 0 || f.scene.choice_first > 0) &&
                         !ui_search_scene_filter_choice_geometry(f.scene, hidden_index, &hidden_rectangle,
                                                                 &hidden_clip),
                     35 + sample))
            return false;
        const u32 hovered_index = selected == 0 ? 1 : selected - 1;
        const auto hovered = row_widget(f.scene, hovered_index);
        const auto* hovered_node = ui_widget_manager_layout_node(f.scene.widgets, hovered);
        const auto pointer =
            hovered_node
                ? ui_widget_manager_pointer_target(&f.scene.widgets, hovered_node->rectangle.left + 16,
                                                   hovered_node->rectangle.top + 16)
                : ui_widget_invalid_handle();
        if (!require(hovered_node && pointer.slot == hovered.slot &&
                         pointer.generation == hovered.generation &&
                         ui_widget_manager_hover(&f.scene.widgets, pointer) &&
                         f.scene.widgets.hovered.slot == hovered.slot &&
                         f.scene.widgets.hovered.generation == hovered.generation,
                     40 + sample))
            return false;
        if (!require(probe_present(&renderer, theme) && renderer_rebuild_content_text(&renderer, theme),
                     50 + sample))
            return false;
        const float clear[]{theme.base_lowest.red, theme.base_lowest.green, theme.base_lowest.blue, 1};
        renderer.graphics.context->ClearRenderTargetView(renderer.graphics.target, clear);
        if (!require(d3d11_renderer_draw_stream(&renderer, theme, false, false, nullptr, 10000) &&
                         rb.read(renderer, 0),
                     60 + sample))
            return false;
        if (!require(row_states(renderer, rb, selected_rect, hovered_node->rectangle, theme), 65 + sample))
            return false;
        for (u32 visible = 0; visible < f.scene.choice_count; ++visible) {
            const u32 index = f.scene.choice_first + visible;
            UiRect rectangle{}, clip{};
            if (!require(row_ink(f, renderer, rb, index, theme) &&
                             ui_search_scene_filter_choice_geometry(f.scene, index, &rectangle, &clip),
                         70 + sample))
                return false;
            const auto point = channel_overview_probe_detail::center(clip);
            const auto hit = ui_search_scene_hit_test(f.scene, point.x, point.y);
            const auto widget = row_widget(f.scene, index);
            if (!require(hit.action == UiSearchAction::filter_value && hit.source_index == index &&
                             hit.source_revision == f.choices.revision && !hit.channel_id &&
                             !hit.message_id && ui_search_scene_target_current(f.scene, hit) &&
                             widget.slot < f.scene.widgets.count &&
                             !(f.scene.widgets.storage[widget.slot].flags & ui_widget_focusable),
                         80 + sample))
                return false;
        }
        const UiPoint chrome{f.scene.chooser.left + 2, f.scene.chooser.top + 2};
        if (!require(ui_search_scene_contains(f.scene, chrome.x, chrome.y) &&
                         ui_search_scene_hit_test(f.scene, chrome.x, chrome.y).action ==
                             UiSearchAction::none,
                     90 + sample))
            return false;
        bool covered_result{};
        for (u32 row = 0; row < f.scene.count; ++row) {
            const auto covered = ui_rect_intersection(f.scene.row_rectangles[row], f.scene.chooser);
            if (!ui_rect_has_area(covered))
                continue;
            covered_result = true;
            const auto center = channel_overview_probe_detail::center(covered);
            const auto hit = ui_search_scene_hit_test(f.scene, center.x, center.y);
            if (!require(ui_search_scene_contains(f.scene, center.x, center.y) &&
                             hit.action != UiSearchAction::result &&
                             hit.action != UiSearchAction::rich_item,
                         95 + sample))
                return false;
        }
        if (!require(covered_result, 94 + sample))
            return false;
        // Frames on both sides of a caret blink: the focused field's caret
        // quad comes and goes, and the field, panel and chooser still paint
        // after the shell (above message text) in each.
        for (u32 blink = 0; blink < 2; ++blink) {
            f.content.edit.caret_visible = blink == 0;
            ++f.content.revision;
            if (!require(ui_search_scene_update(&f.scene, &f.results, &f.content, viewport, header.title,
                                                &header) &&
                             renderer_rebuild_content_text(&renderer, theme) && probe_present(&renderer, theme) &&
                             search_layer_whole(renderer, f.scene, theme),
                         160 + sample * 2 + blink)) {
                failure_state(renderer, f, sample, header);
                return false;
            }
        }
        if (sample == 0 || sample == 3) {
            if (!require(probe_present(&renderer, theme) && warm(renderer, f, theme, viewport, header),
                         100 + sample))
                return false;
        }
        const auto point = channel_overview_probe_detail::center(selected_clip);
        const auto ticket = ui_search_scene_hit_test(f.scene, point.x, point.y);
        const auto prior_stamp = renderer_capture_presentation_stamp(renderer, theme);
        ++f.choices.revision;
        if (!require(retired(f, ticket, point, theme, renderer.paint_stream, dpi) &&
                         !renderer_presentation_stamp_equal(
                             prior_stamp, renderer_capture_presentation_stamp(renderer, theme)) &&
                         renderer_refresh_interaction_text(&renderer, theme) &&
                         probe_present(&renderer, theme),
                     110 + sample))
            return false;
        ++f.content.revision;
        if (!require(
                ui_search_scene_update(&f.scene, &f.results, &f.content, viewport, header.title, &header) &&
                    !ui_search_scene_target_current(f.scene, ticket),
                120 + sample))
            return false;
        const auto fresh = ui_search_scene_hit_test(f.scene, point.x, point.y);
        if (!require(fresh.action == UiSearchAction::filter_value && fresh.source_index == selected &&
                         fresh.source_revision == f.choices.revision &&
                         ui_search_scene_target_current(f.scene, fresh),
                     125 + sample))
            return false;
        f.authority = false;
        if (!require(retired(f, fresh, point, theme, renderer.paint_stream, dpi), 130 + sample))
            return false;
        f.authority = true;
        UiSearchFilterChoiceView replacement[kUiSearchFilterChoiceMaximum]{};
        copy_bytes(replacement, f.rows, sizeof(replacement));
        f.choices.rows = {replacement, kUiSearchFilterChoiceMaximum};
        if (!require(retired(f, fresh, point, theme, renderer.paint_stream, dpi), 140 + sample))
            return false;
        f.choices.rows = {f.rows, kUiSearchFilterChoiceMaximum};
        const u16 selection = f.choices.selected;
        f.choices.selected = kUiSearchFilterNoSelection;
        if (!require(retired(f, fresh, point, theme, renderer.paint_stream, dpi), 145 + sample))
            return false;
        f.choices.selected = selection;
    }
    f.choices.rows = {};
    f.choices.selected = kUiSearchFilterNoSelection;
    ++f.choices.revision;
    ++f.content.revision;
    const auto header = ui_scene_header_geometry(f.shell);
    const auto viewport = ui_scene_content_rectangle(f.shell, UiSemantic::content);
    UiTextCommand empty_commands[128]{};
    UiTextCommandStream empty_stream{};
    UiTextOutput empty_text{&empty_stream, nullptr};
    if (!require(
            ui_search_scene_update(&f.scene, &f.results, &f.content, viewport, header.title, &header) &&
                ui_text_command_stream_initialize(&empty_stream, {empty_commands, 128}) &&
                ui_search_scene_text_output(f.scene, kLightTheme, &empty_text, nullptr) &&
                !f.scene.choice_total && !f.scene.choice_count,
            150))
        return false;
    bool heading{}, hint{};
    for (u32 command = 0; command < empty_stream.count; ++command) {
        heading = heading || utf8_equal(empty_commands[command].text, f.choices.heading);
        hint = hint || utf8_equal(empty_commands[command].text, f.choices.hint);
    }
    for (u32 slot = 1; slot < f.scene.widgets.count; ++slot)
        if (!require(f.scene.targets[slot].action != UiSearchAction::filter_value &&
                         f.scene.targets[slot].action != UiSearchAction::filter_from &&
                         f.scene.targets[slot].action != UiSearchAction::more_filters,
                     151))
            return false;
    if (!require(heading && hint, 152))
        return false;
    RendererTypographyCandidate original_candidate{};
    const bool prepared = renderer_prepare_typography(&renderer, original, &original_candidate);
    const bool restored = prepared && renderer_commit_typography(&renderer, &original_candidate);
    renderer_discard_typography(&original_candidate);
    if (!require(restored, 153))
        return false;
    log_write(LogLevel::info, "probe", "search_filter_native_four_palettes_dpi_custom18_two64_passed");
    return true;
}
} // namespace maicord
