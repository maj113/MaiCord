#include "search_ui_probe.hpp"
#include "probe_present.hpp"
#include "maicord/core/log.hpp"
#include "../../src/app/application_state.hpp"
#include "../../src/render/d3d11_renderer_interaction_state.hpp"

namespace maicord {
bool probe_resize_surface(u32 width, u32 height) noexcept;
namespace {
struct SearchProbeSource {
    u32 calls;
};
bool search_probe_resolve(void* context, u32 index, UiSearchRow* row) noexcept {
    ++static_cast<SearchProbeSource*>(context)->calls;
    const Utf8View authors[] = {utf8_literal("Aster"),   utf8_literal("Mira"), utf8_literal("Aster"),
                                utf8_literal("Juniper"), utf8_literal("Mira"), utf8_literal("Juniper")};
    const Utf8View bodies[] = {
        utf8_literal("The release notes are ready for review."),
        utf8_literal("Native search keeps the original match inside its conversation context."),
        utf8_literal("That makes the next step easier to find."),
        utf8_literal("A second release result has its own channel and message identity."),
        utf8_literal("This is an original, synthetic fixture. No account or network is attached."),
        utf8_literal("Search cards wrap text within the pane and keep context rows distinct.")};
    *row = {index < 3 ? 71u : 72u,
            index + 101,
            authors[index % 6],
            index < 3 ? utf8_literal("# release-planning") : utf8_literal("# native-ui"),
            bodies[index % 6],
            index % 3 == 1 ? utf8_literal("Match 2026-10-03 12:30")
                           : utf8_literal("Context 2026-10-03 12:29"),
            index % 3 == 1,
            index % 3 == 0};
    return true;
}
} // namespace
bool search_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!renderer.ui_scene || !renderer.content_text || !renderer.dpi)
        return false;
    UiSearchScene scene{};
    SearchProbeSource fixture{};
    UiSearchSource source{&fixture, 6, 1, search_probe_resolve};
    UiSearchContent content{};
    content.edit = {utf8_literal("release has:image"), 0, 17, true, false, false};
    content.panel = true;
    content.next = true;
    content.total = 64;
    content.phase = UiSearchPhase::ready;
    content.status = utf8_literal("Click a message to jump to its conversation.");
    content.revision = 1;
    const auto* old_scene = renderer.search_scene;
    const auto* old_focus = renderer.focus_owner;
    const u32 width = renderer.width, height = renderer.height;
    const auto original_content = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::content);
    const float original_width_dips = static_cast<float>(width) * 96.f / renderer.dpi;
    // Match the native minimum: preserve rail, navigation and member panes,
    // leaving the application minimum message width between them.
    const float compact_width_dips = original_content.left +
                                     maximum(0.f, original_width_dips - original_content.right) +
                                     app_detail::kMinimumAccountMessageWidthDips;
    u32 failure_stage = 0, capture_stage = 0;
    bool valid = true;
    const auto show = [&](const wchar_t* name) noexcept {
        const u32 stage = ++capture_stage * 10;
        const auto* title =
            ui_scene_resolve(*renderer.ui_scene, ui_scene_find(*renderer.ui_scene, UiSemantic::title_bar));
        if (!title) {
            failure_stage = stage + 1;
            return false;
        }
        auto viewport = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::content);
        viewport.right = static_cast<float>(renderer.width) * 96.f / renderer.dpi;
        if (!ui_search_scene_update(&scene, &source, &content, viewport, title->rectangle)) {
            failure_stage = stage + 2;
            return false;
        }
        const float header_y = (scene.field.top + scene.field.bottom) * .5f;
        const auto* submit = ui_widget_manager_layout_node(scene.widgets, scene.buttons[0]);
        if (ui_search_scene_hit_test(scene, scene.field.left + 8, header_y).action !=
                UiSearchAction::edit ||
            ui_search_scene_hit_test(scene, scene.field.right - 8, header_y).action !=
                UiSearchAction::submit ||
            !submit || !ui_rect_has_area(ui_rect_intersection(submit->rectangle, submit->clip_rectangle))) {
            failure_stage = stage + 6;
            return false;
        }
        renderer.search_scene = &scene;
        renderer.retained_presentation.valid = false;
        if (!renderer_rebuild_content_text(&renderer, kDefaultTheme) || !renderer.search_layout.active) {
            failure_stage = stage + 3;
            return false;
        }
        if (!probe_present(&renderer, kDefaultTheme)) {
            failure_stage = stage + 4;
            return false;
        }
        if (capture && !capture(name)) {
            failure_stage = stage + 5;
            return false;
        }
        return true;
    };
    valid = show(L"search-results-native.bmp");
    if (valid) {
        const auto baseline = renderer.content_text_prefix;
        content.filters = true;
        ++content.revision;
        valid = show(L"search-filters-native.bmp") &&
                renderer.content_text_prefix.base_builds == baseline.base_builds;
        if (!valid && !failure_stage)
            failure_stage = 26;
    }
    content.filters = false;
    content.next = false;
    source.count = 0;
    ++source.revision;
    content.total = 0;
    content.phase = UiSearchPhase::loading;
    content.status = utf8_literal("Searching…");
    ++content.revision;
    if (valid)
        valid = show(L"search-loading-native.bmp");
    content.phase = UiSearchPhase::indexing;
    content.status = utf8_literal("Indexing messages…");
    ++content.revision;
    if (valid)
        valid = show(L"search-indexing-native.bmp");
    content.phase = UiSearchPhase::ready;
    content.status = utf8_literal("No messages matched this search.");
    ++content.revision;
    if (valid)
        valid = show(L"search-empty-native.bmp");
    source.count = 6;
    ++source.revision;
    content.total = 64;
    content.phase = UiSearchPhase::loading;
    content.stale = true;
    content.status = utf8_literal("Updating results…");
    ++content.revision;
    if (valid)
        valid = show(L"search-stale-native.bmp");
    renderer.search_scene = nullptr;
    if (valid) {
        const float scale = static_cast<float>(renderer.dpi) / 96;
        valid =
            probe_resize_surface(static_cast<u32>(compact_width_dips * scale + 0.999f),
                                 static_cast<u32>(app_detail::kMinimumClientHeightDips * scale + 0.999f));
        if (!valid)
            failure_stage = 81;
    }
    content.phase = UiSearchPhase::ready;
    content.stale = false;
    content.status = utf8_literal("Results remain available while you jump.");
    ++content.revision;
    if (valid) {
        valid = show(L"search-compact-native.bmp") && scene.overlay && ui_rect_width(scene.panel) <= 360;
        if (!valid && !failure_stage)
            failure_stage = 76;
    }
    renderer.search_scene = nullptr;
    const bool restored_size = probe_resize_surface(width, height);
    renderer.search_scene = old_scene;
    renderer_bind_focus_owner(&renderer, old_focus);
    renderer.retained_presentation.valid = false;
    const bool restored = restored_size && renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                          probe_present(&renderer, kDefaultTheme);
    if (!restored && !failure_stage)
        failure_stage = 91;
    ui_search_scene_release(&scene);
    log_u64(LogLevel::info, "probe", "search_scene_owner_bytes", sizeof(UiSearchScene));
    log_u64(LogLevel::info, "probe", "search_native_source_resolutions", fixture.calls);
    log_u64(LogLevel::info, "probe", "search_native_compact_width_dips",
            static_cast<u64>(compact_width_dips));
    if (!valid || !restored)
        log_u64(LogLevel::error, "probe", "search_native_failure_stage", failure_stage);
    return valid && restored;
}
} // namespace maicord
