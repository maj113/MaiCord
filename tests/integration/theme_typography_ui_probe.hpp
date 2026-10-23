#pragma once

#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
inline bool theme_typography_probe_require(bool valid, const char* label) noexcept {
    if (valid)
        return true;
    DWORD written{};
    const auto output = GetStdHandle(STD_ERROR_HANDLE);
    constexpr char prefix[] = "Native typography probe failed: ";
    WriteFile(output, prefix, static_cast<DWORD>(sizeof(prefix) - 1), &written, nullptr);
    u32 bytes{};
    while (label[bytes])
        ++bytes;
    WriteFile(output, label, bytes, &written, nullptr);
    WriteFile(output, "\n", 1, &written, nullptr);
    return false;
}

// Uses the existing native renderer/session. All measurements/hits stay in RAM;
// the caller subsequently projects its pixel fixture with restored typography.
inline bool theme_typography_ui_probe(D3D11Renderer& renderer) noexcept {
    const UiThemeTypography original = renderer.glyph_text.system.typography
                                           ? renderer.glyph_text.system.typography->value
                                           : UiThemeTypography{};
    struct Restore {
        D3D11Renderer& renderer;
        UiThemeTypography original;
        UiTextMeasureCache* cache;
        u16 backend;
        TextEditLayoutOwner edit{};
        TextEditMetric selections[kTextEditSelectionCapacity]{};
        RendererTypographyCandidate candidate{}, stale{};
        ~Restore() noexcept {
            renderer.message_measure_cache = cache;
            renderer_discard_typography(&candidate);
            renderer_discard_typography(&stale);
            text_edit_layout_release(&edit);
            RendererTypographyCandidate restore{};
            const bool prepared = renderer_prepare_typography(&renderer, original, &restore);
            const bool applied = prepared && renderer_commit_typography(&renderer, &restore);
            theme_typography_probe_require(applied, "emergency restore");
            renderer_discard_typography(&restore);
            renderer.message_measure_cache = cache;
            renderer.text_measure_revision = backend;
        }
    } scope{renderer, original, renderer.message_measure_cache, renderer.text_measure_revision};
    UiTextMeasureRecord records[4]{};
    UiTextMeasureCache cache{};
    if (!theme_typography_probe_require(ui_text_measure_cache_initialize(
                                            &cache, {records, 4}, renderer_text_measure_backend, &renderer),
                                        "native cache initialize"))
        return false;
    renderer.message_measure_cache = &cache;
    const UiTextMeasureRequest request{
        {0x7479706f, 0x7265616c, 1, 0, 1}, utf8_literal("Wiiii WWWWW 012345"), {}, {4096, 4096}, true};
    UiTextMeasureValue before{}, after{};
    if (!theme_typography_probe_require(ui_text_measure_cache_measure(&cache, request, &before).ok(),
                                        "initial real measure"))
        return false;
    const u32 initial_revision = renderer_typography_revision(renderer);
    auto invalid = original;
    invalid.body_size = 100;
    if (!theme_typography_probe_require(
            !renderer_prepare_typography(&renderer, invalid, &scope.candidate) &&
                !scope.candidate.prepared && renderer_typography_revision(renderer) == initial_revision &&
                renderer.glyph_text.system.em_size == original.body_size,
            "invalid preparation preserves active resources"))
        return false;
    UiThemeTypography changed = original;
    changed.body_size = 18;
    changed.body_face = UiTextFace::serif;
    changed.code_face = UiTextFace::normal;
    changed.subtext_ratio = .9f;
    changed.mono_ratio = 1;
    if (!theme_typography_probe_require(renderer_prepare_typography(&renderer, changed, &scope.candidate) &&
                                            renderer_typography_revision(renderer) == initial_revision &&
                                            renderer.glyph_text.system.em_size == original.body_size,
                                        "prepared candidate leaves live font unchanged"))
        return false;
    renderer_discard_typography(&scope.candidate);
    if (!theme_typography_probe_require(renderer_typography_revision(renderer) == initial_revision &&
                                            renderer.glyph_text.system.em_size == original.body_size,
                                        "discard restores candidate resources only"))
        return false;
    constexpr auto edit_text =
        utf8_literal("Native editor wrapping keeps the existing selection and scroll position.");
    const TextEditLayoutInput input{
        edit_text, text_edit_signature(edit_text),
        3,         8,
        0,         0,
        84,        30,
        4096,      text_edit_layout_wrap | text_edit_layout_scroll_to_caret,
        {}};
    scope.edit.selections = scope.selections;
    scope.edit.selection_capacity = kTextEditSelectionCapacity;
    if (!theme_typography_probe_require(
            text_edit_layout_measure(&scope.edit, input, renderer.glyph_text.system),
            "initial native editor measurement"))
        return false;
    if (!theme_typography_probe_require(
            text_edit_layout_current_font(scope.edit, renderer.glyph_text.system),
            "initial native editor font fence"))
        return false;
    if (!theme_typography_probe_require(scope.edit.geometry &&
                                            scope.edit.selection_count &&
                                            scope.edit.utf16_units == edit_text.bytes,
                                        "initial editor owns native selection geometry"))
        return false;
    scope.edit.manual_scroll = true;
    scope.edit.scroll_y = 5;
    const u32 cached_generation = cache.generation;
    if (!theme_typography_probe_require(
            renderer_prepare_typography(&renderer, original, &scope.stale) &&
                renderer_prepare_typography(&renderer, changed, &scope.candidate) &&
                renderer_commit_typography(&renderer, &scope.candidate) &&
                renderer_typography_revision(renderer) == initial_revision + 1 &&
                renderer.glyph_text.system.em_size == 18 && cache.generation != cached_generation &&
                !renderer.glyph_text.system.independent_letters &&
                !renderer.glyph_text.system.message_hit_policy &&
                !renderer.glyph_text.system.fixed_weight &&
                !renderer.glyph_text.system.typography->code_fixed_weight,
            "real font commit invalidates measured geometry"))
        return false;
    if (!theme_typography_probe_require(
            !renderer_commit_typography(&renderer, &scope.stale) &&
                !text_edit_layout_current_font(scope.edit, renderer.glyph_text.system) &&
                scope.edit.anchor == 3 && scope.edit.caret_offset == 8 && scope.edit.manual_scroll &&
                scope.edit.scroll_y == 5,
            "stale candidate and old-font TSF reject without resetting editor"))
        return false;
    renderer_discard_typography(&scope.stale);
    const auto measured = ui_text_measure_cache_measure(&cache, request, &after);
    if (!theme_typography_probe_require(
            measured.ok() && !measured.cached && after.size.width != before.size.width &&
                text_system_font_size(renderer.glyph_text.system, text_style_subtext) == 18 * .9f &&
                text_edit_layout_measure(&scope.edit, input, renderer.glyph_text.system) &&
                text_edit_layout_current_font(scope.edit, renderer.glyph_text.system) &&
                scope.edit.anchor == 3 && scope.edit.caret_offset == 8 && scope.edit.manual_scroll &&
                scope.edit.scroll_y == 5,
            "new native measure and editor preserve logical state"))
        return false;
    auto inline_code = request;
    const TextStyleSpan code_span{0, request.text.bytes, text_style_monospace};
    inline_code.key.item = 1;
    inline_code.styles = {&code_span, 1};
    auto code_alias = request;
    code_alias.key.item = 2;
    code_alias.face = UiTextFace::mono;
    UiTextMeasureValue inline_value{}, alias_value{};
    if (!theme_typography_probe_require(
            ui_text_measure_cache_measure(&cache, inline_code, &inline_value).ok() &&
                ui_text_measure_cache_measure(&cache, code_alias, &alias_value).ok() &&
                maximum(inline_value.size.width, alias_value.size.width) -
                        minimum(inline_value.size.width, alias_value.size.width) <
                    .01f &&
                alias_value.size.width != after.size.width,
            "inline code and mono role use the same selected real family"))
        return false;
    TextGlyph glyphs[32]{};
    TextRun runs[8]{};
    u16 clusters[32]{};
    ShapedText shaped{};
    if (!theme_typography_probe_require(
            shaped_text_initialize(&shaped, {glyphs, 32}, {runs, 8}, {clusters, 32}),
            "bounded native shaping storage"))
        return false;
    auto shape_system = renderer.glyph_text.system;
    wchar_t shape_units[32]{};
    const TextStyleSpan subtext_span{0, 4, text_style_subtext};
    const auto shape_result = text_shape_utf8_styled(
        &shape_system, utf8_literal("Size"), {&subtext_span, 1}, 256, 128, {shape_units, 32}, &shaped);
    const bool shaped_size = shape_result.ok() && shaped.run_count &&
                             shaped.runs[0].em_size == changed.body_size * changed.subtext_ratio;
    shaped_text_reset(&shaped);
    if (!theme_typography_probe_require(shaped_size, "semantic size reaches native glyph runs"))
        return false;
    const u32 unchanged_generation = cache.generation;
    const u32 unchanged_calls = cache.backend_calls;
    if (!theme_typography_probe_require(
            renderer_prepare_typography(&renderer, changed, &scope.candidate) && !scope.candidate.owner &&
                renderer_commit_typography(&renderer, &scope.candidate) &&
                renderer_typography_revision(renderer) == initial_revision + 1 &&
                cache.generation == unchanged_generation && cache.backend_calls == unchanged_calls,
            "identical apply has no owner allocation or invalidation"))
        return false;
    renderer.text_measure_revision = 65535;
    changed.body_size = 19;
    if (!theme_typography_probe_require(renderer_prepare_typography(&renderer, changed, &scope.candidate) &&
                                            renderer_commit_typography(&renderer, &scope.candidate) &&
                                            renderer_text_measure_revision(renderer) == 1 &&
                                            cache.generation != unchanged_generation &&
                                            ui_text_measure_cache_measure(&cache, request, &after).ok() &&
                                            cache.backend_calls == unchanged_calls + 1,
                                        "backend-token exhaustion clears real cached entries before reuse"))
        return false;
    if (!theme_typography_probe_require(
            renderer_prepare_typography(&renderer, original, &scope.candidate) &&
                renderer_commit_typography(&renderer, &scope.candidate) &&
                renderer.glyph_text.system.em_size == original.body_size &&
                ui_theme_typography_equal(renderer.glyph_text.system.typography->value, original),
            "original typography restored"))
        return false;
    return true;
}
} // namespace maicord
