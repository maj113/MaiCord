#include "maicord/render/text_form_surface.hpp"
#include "maicord/ui/text.hpp"
#include "maicord/core/time.hpp"
#include "../src/app/application_text_form_state.hpp"

namespace maicord {
namespace {
struct MeasureContext { TextSystem text; wchar_t scratch[2048]; };
bool measure(void* opaque, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    auto* context = static_cast<MeasureContext*>(opaque);
    const auto measured = text_measure_utf8_styled(&context->text, request.text, request.styles,
        request.constraint.width, request.constraint.height, {context->scratch, 2048}, TextWrapping::wrap);
    if (!measured.ok()) return false;
    *value = {{measured.width_including_trailing_whitespace, measured.height}, 0, measured.line_count};
    return true;
}
}
bool benchmark_text_form(Clock clock, void (*write)(const char*, u64, const char*) noexcept) noexcept {
    MeasureContext context{};
    if (!text_system_initialize(&context.text, L"Segoe UI", 14, L"en-us").ok()) return false;
    TextFormScene scene{};
    TextFormSurface surface{};
    UiTextMeasureRecord records[32]{};
    UiTextMeasureCache cache{};
    char text[1024]{};
    constexpr auto line = utf8_literal("a\xF0\x9F\x98\x80\xD7\x90\n");
    for (u32 i = 0; i < sizeof(text); ++i) text[i] = line.data[i % line.bytes];
    TextFormContent content{utf8_literal("Attachment description"), utf8_literal("image.png"),
        utf8_literal("Describe the image"), {}, utf8_literal("Cancel"), utf8_literal("Save description"),
        {text, sizeof(text)}, 0, 640, true, true};
    bool valid = ui_text_measure_cache_initialize(&cache, {records, 32}, measure, &context) &&
        text_form_update(&scene, content, {0, 0, 800, 600}, &cache);
    surface.scene = &scene;
    valid = valid && text_form_surface_update(&surface, context.text);
    const auto backend_calls = cache.backend_calls, layout_revision = scene.widgets.layout.revision;
    u64 checksum = 0;
    i64 start = clock_now();
    for (u32 i = 0; valid && i < 10000; ++i) {
        valid = text_form_update(&scene, content, {0, 0, 800, 600}, &cache) && text_form_surface_update(&surface, context.text);
        checksum += surface.layout.selection_count + scene.widgets.count;
    }
    write("ui_text_form_cached_10000_us", clock_microseconds(clock, start, clock_now()), "us");
    valid = valid && backend_calls == cache.backend_calls && layout_revision == scene.widgets.layout.revision;
    start = clock_now();
    for (u32 i = 0; valid && i < 1000; ++i) {
        const float width = i % 2 ? 320.0f : 800.0f;
        valid = text_form_update(&scene, content, {0, 0, width, 600}, &cache) && text_form_surface_update(&surface, context.text);
        checksum += surface.layout.selection_count + scene.widgets.layout.revision;
    }
    write("ui_text_form_reflow_1000_us", clock_microseconds(clock, start, clock_now()), "us");
    start = clock_now();
    for (u32 i = 0; valid && i < 1000; ++i) {
        content.anchor_utf16 = i % 2 ? 0 : 640;
        content.caret_utf16 = i % 2 ? 640 : 0;
        valid = text_form_update(&scene, content, {0, 0, 800, 600}, &cache) && text_form_surface_update(&surface, context.text);
        checksum += surface.layout.selection_count + surface.layout.caret_offset;
    }
    write("ui_text_form_large_selection_1000_us", clock_microseconds(clock, start, clock_now()), "us");
    write("ui_text_form_checksum", checksum, "sum");
    TextCompositionSpan composition{636, 638, 0, 0, 0xE03060,
        TextCompositionLineStyle::solid, TextCompositionAttribute::target_converted,
        text_composition_color_line, true};
    content.anchor_utf16 = content.caret_utf16 = 640;
    content.composition = {&composition, 1};
    valid = valid && text_form_update(&scene, content, {0, 0, 800, 600}, &cache) && text_form_surface_update(&surface, context.text);
    // Composition styling alone must reuse the measured layout.
    const u32 retained_builds = surface.layout.builds;
    u64 composition_checksum = 0;
    start = clock_now();
    for (u32 i = 0; valid && i < 1000; ++i) {
        composition.line_rgb = i % 2 ? 0xE03060 : 0x30E060;
        valid = text_form_update(&scene, content, {0, 0, 800, 600}, &cache) && text_form_surface_update(&surface, context.text) &&
            surface.composition_complete && surface.geometry.composition.count == 1 && surface.layout.builds == retained_builds;
        composition_checksum += surface.geometry.composition.count;
    }
    write("ui_text_form_composition_1000_us", clock_microseconds(clock, start, clock_now()), "us");
    write("ui_text_form_composition_checksum", composition_checksum, "sum");
    write("ui_text_form_scene_bytes", sizeof(TextFormScene), "bytes");
    write("ui_text_form_surface_bytes", sizeof(TextFormSurface), "bytes");
    write("ui_application_text_form_bytes", sizeof(app_detail::ApplicationTextForm), "bytes");
    write("ui_text_form_widgets_committed", scene.storage.arena.committed, "bytes");
    write("ui_text_form_edit_layout_committed", surface.layout.storage.arena.committed, "bytes");
    write("ui_text_form_label_backend_calls", cache.backend_calls, "calls");
    text_form_surface_release(&surface);
    text_form_release(&scene);
    text_system_release(&context.text);
    return valid;
}
} // namespace maicord
