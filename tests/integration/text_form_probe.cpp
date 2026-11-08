#include "probe_present.hpp"
#include "../support/layout_validation.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/render/d3d11/renderer_presentation.hpp"
#include "maicord/ui/text/secure_edit.hpp"
#include "maicord/core/log.hpp"
#include "maicord/render/text/form_surface.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
bool text_form_probe(D3D11Renderer& renderer, UiTextMeasureCache& cache) noexcept {
    TextFormScene scene{};
    TextFormSurface surface{};
    SecureEdit edit{};
    if (!secure_edit_initialize(&edit, 4096, false, {1024, true})) return false;
    char display[4096]{};
    TextCompositionSpan composition{};
    u32 composition_count = 0;
    auto refresh = [&]() noexcept {
        u32 bytes = 0;
        SecureEditUtf16Selection selection{};
        SecureEditRenderWindow window{};
        if (secure_edit_render_window(edit, sizeof(display), {display, sizeof(display)}, &bytes, &window) !=
                SecureEditResult::unchanged ||
            secure_edit_utf16_selection(edit, &selection) != SecureEditResult::unchanged) return false;
        const float scale = 96.0f / static_cast<float>(renderer.dpi);
        const TextFormContent content{utf8_literal("Attachment description"), utf8_literal("\xE6\x97\x85\xE8\xA1\x8C.png"),
            utf8_literal("Describe the image"), utf8_literal("Description is stored when you choose Save"),
            utf8_literal("Cancel"), utf8_literal("Save description"), {display, bytes},
            selection.anchor, selection.caret, true, true, {&composition, composition_count}};
        if (!text_form_update(&scene, content, {0, 0, renderer.width * scale, renderer.height * scale}, &cache)) return false;
        surface.scene = &scene;
        renderer_bind_text_form(&renderer, &surface);
        return renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
    };
    u32 stage=__LINE__;bool valid = secure_edit_assign(&edit, utf8_literal("one\r\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n\xF0\x9F\x98\x80")) == SecureEditResult::changed && refresh();
    if(valid)stage=__LINE__;valid = valid && surface.layout.active && surface.layout.scroll_y > 0 &&
        surface.layout.caret.top >= surface.layout.viewport_top &&
        surface.layout.caret.top + surface.layout.caret.height <= surface.layout.viewport_top + surface.layout.height + 0.01f;
    composition = {surface.layout.utf16_units - 2, surface.layout.utf16_units, 0, 0, 0xE03060,
        TextCompositionLineStyle::solid, TextCompositionAttribute::target_converted, text_composition_color_line, true};
    composition_count = 1;
    const u32 composition_builds = surface.layout.builds;
    if(valid)stage=__LINE__;valid = valid && refresh() && surface.composition_complete && surface.geometry.composition.count == 1 &&
        surface.layout.builds == composition_builds && surface.geometry.composition[0].attributes.line_rgb == 0xE03060;
    composition_count = 0;
    u32 hit = 0;
    if(valid)stage=__LINE__;valid = valid && text_edit_layout_hit_test(surface.layout, surface.layout.caret.left,
        surface.layout.caret.top + surface.layout.caret.height * 0.5f, &hit) && hit == surface.layout.utf16_units;
    if(valid)stage=__LINE__;valid = valid && secure_edit_select_all(&edit) == SecureEditResult::changed && refresh() && surface.layout.selection_count > 1;
    const auto before = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
    if(valid)stage=__LINE__;valid = valid && text_form_focus(&scene, TextFormElement::accept) && probe_present(&renderer, kDefaultTheme);
    const auto after = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
    if(valid)stage=__LINE__;valid = valid && !renderer_presentation_stamp_equal(before, after);
    char long_text[1024]{};
    constexpr auto line = utf8_literal("a\xF0\x9F\x98\x80\xD7\x90\n");
    for (u32 i = 0; i < sizeof(long_text); ++i) long_text[i] = line.data[i % line.bytes];
    if(valid)stage=__LINE__;valid = valid && secure_edit_assign(&edit, {long_text, sizeof(long_text)}) == SecureEditResult::changed &&
        secure_edit_select_all(&edit) == SecureEditResult::changed && refresh();
    TextEditMetric reference[1024]{};
    u32 actual = 0;
    const auto contains = [](const auto* metrics, u32 count, float x, float y) noexcept {
        for (u32 i = 0; i < count; ++i) if (x >= metrics[i].left && x < metrics[i].left + metrics[i].width &&
            y >= metrics[i].top && y < metrics[i].top + metrics[i].height) return true;
        return false;
    };
    const SecureEditUtf16Selection ranges[] = {{0, 640}, {640, 0}, {313, 588}, {588, 313}};
    // The reference is the editor's own unclipped range geometry of the same layout, one line ("a😀א\n", five
    // units) at a time: the retained selection, subdivided past its capacity and clipped to the viewport, must cover
    // exactly the same pixels in it.
    for (const auto range : ranges) {
        const auto selected = secure_edit_set_utf16_selection(&edit, range);
        if(valid)stage=__LINE__;valid = valid && (selected == SecureEditResult::changed || selected == SecureEditResult::unchanged) && refresh();
        // Selection changes reuse the retained geometry (one key commit each, no reshape).
        if(valid)stage=__LINE__;valid = valid && surface.layout.geometry;
        const u32 first = minimum(range.anchor, range.caret), last = maximum(range.anchor, range.caret);
        actual = 0;
        for (u32 at = first; valid && at < last;) {
            const u32 end = minimum(last, (at / 5 + 1) * 5);
            u32 count = 0;
            if(valid)stage=__LINE__;valid = valid && actual + kTextEditSelectionCapacity <= 1024 &&
                text_edit_layout_raw_range_geometry(surface.layout, at, end, {reference + actual, kTextEditSelectionCapacity}, &count);
            actual += count;
            at = end;
        }
        if(valid)stage=__LINE__;valid = valid && actual > kTextEditSelectionCapacity && surface.layout.selection_count > 0;
        for (float y = surface.layout.viewport_top + 0.5f; valid && y < surface.layout.viewport_top + surface.layout.height; y += 1) {
            for (float x = surface.layout.origin_x + 0.5f; x < surface.layout.origin_x + surface.layout.width; x += 1) {
                if (contains(reference, actual, x, y) != contains(surface.layout.selections, surface.layout.selection_count, x, y)) {
                    valid = false;
                    break;
                }
            }
        }
    }
    if(!valid)log_u64(LogLevel::error,"probe","text_form_failure_line",stage);
    if (valid) log_write(LogLevel::info, "probe", "text_form_large_bidi_selection_matches_full_reference");
    UiLayoutDiagnostic diagnostics[32]{};
    if(valid)stage=__LINE__;valid = valid && ui_layout_engine_validate(scene.widgets.layout, {diagnostics, 32}).clean();
    renderer_bind_text_form(&renderer, nullptr);
    text_form_surface_release(&surface);
    text_form_release(&scene);
    secure_edit_release(&edit);
    valid = renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) && valid;
    log_write(valid ? LogLevel::info : LogLevel::error, "probe", valid ? "text_form_hardware_passed" : "text_form_hardware_failed");
    return valid;
}
} // namespace maicord
