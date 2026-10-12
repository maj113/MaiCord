#pragma once
#include "probe_present.hpp"
#include "maicord/ui/quick_switcher_scene.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
bool probe_resize_surface(u32 width, u32 height) noexcept;
namespace quick_switcher_probe_detail {
struct Source { u32 calls{}; bool current{true}, mixed{true}, people{}; };
inline bool current(void* opaque) noexcept { return static_cast<Source*>(opaque)->current; }
inline bool resolve(void* opaque, u32 index, UiQuickSwitcherRow* row) noexcept {
    auto& fixture = *static_cast<Source*>(opaque); ++fixture.calls;
    const UiQuickSwitcherKind kinds[]{UiQuickSwitcherKind::text_channel, UiQuickSwitcherKind::private_channel,
        UiQuickSwitcherKind::guild, UiQuickSwitcherKind::group_private, UiQuickSwitcherKind::user};
    const Utf8View labels[]{utf8_literal("release-planning"), utf8_literal("Avery with a deliberately long original name"),
        utf8_literal("MaiCord Lab"), utf8_literal("Native UI study group"), utf8_literal("Morgan")};
    const Utf8View details[]{utf8_literal("MaiCord Lab • Text channel"), utf8_literal("Direct conversation • Original fixture"),
        utf8_literal("Server • Browse accessible channels"), utf8_literal("Group conversation • 4 participants"),
        utf8_literal("Original user • Open direct conversation")};
    const Utf8View metadata[]{utf8_literal("Channel"), utf8_literal("Recent"), utf8_literal("Server"),
        utf8_literal("Group DM"), utf8_literal("Person")};
    const u32 kind = fixture.mixed ? index % 5 : 0;
    *row = {1000 + index, 71, 500 + index, 900 + index, kinds[kind],
            labels[kind], details[kind], metadata[kind]};
    if (fixture.people) {
        const Utf8View names[]{utf8_literal("Avery"), utf8_literal("Morgan"), utf8_literal("Rowan"), utf8_literal("Kit")};
        *row = {1000 + index, 0, 0, 900 + index, UiQuickSwitcherKind::user, names[index % 4],
                index == 2 ? utf8_literal("Already in the group") : utf8_literal("original_friend"), {}, {}, index != 2};
    }
    return true;
}
} // namespace quick_switcher_probe_detail
inline bool quick_switcher_ui_probe(D3D11Renderer& renderer,
    bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept {
    if (!renderer.content_text || !renderer.dpi) return false;
    UiQuickSwitcherScene scene{};
    quick_switcher_probe_detail::Source fixture{};
    UiQuickSwitcherSource source{&fixture, 5, 1, quick_switcher_probe_detail::resolve, quick_switcher_probe_detail::current};
    UiQuickSwitcherContent content{};
    content.edit = {utf8_literal("a"), 1, 1, true, true, true};
    content.phase = UiQuickSwitcherPhase::ready; content.revision = 1;
    const u32 width = renderer.width, height = renderer.height, dpi = renderer.dpi;
    struct Restore {
        D3D11Renderer& renderer; UiQuickSwitcherScene& scene;
        const UiQuickSwitcherScene* old_scene; const UiWidgetManager* focus;
        ~Restore() noexcept {
            renderer.quick_switcher_scene = old_scene; renderer.focus_owner = focus;
            renderer.retained_presentation.valid = false;
            content_text_retention_invalidate(&renderer.content_text_retention);
            ui_quick_switcher_scene_release(&scene);
        }
    } restore{renderer, scene, renderer.quick_switcher_scene, renderer.focus_owner};
    Theme theme = kDefaultTheme;
    const auto show = [&](const wchar_t* name) noexcept {
        const UiRect viewport{0, 0, renderer.width * 96.f / renderer.dpi, renderer.height * 96.f / renderer.dpi};
        if (!ui_quick_switcher_scene_update(&scene, &source, &content, viewport)) return false;
        renderer.quick_switcher_scene = &scene; renderer.focus_owner = &scene.widgets;
        return renderer_rebuild_content_text(&renderer, theme) && probe_present(&renderer, theme) &&
            (!capture || capture(name, theme));
    };
    if (!show(L"quick-switcher-ready.bmp")) return false;
    const auto edit = scene.edit;
    const u32 calls = fixture.calls, layout = scene.widgets.layout.revision,
        builds = renderer.content_text->rebuilds, raster = renderer.glyph_text.cache_builds,
        paint_uploads = renderer.paint_instance_uploads, text_uploads = renderer.text_instance_uploads,
        media_uploads = renderer.media_instance_uploads, painted = renderer.painted_frames,
        skips = renderer.unchanged_present_skips;
    for (u32 frame = 0; frame < 64; ++frame)
        if (!ui_quick_switcher_scene_update(&scene, &source, &content, scene.viewport) ||
            !renderer_rebuild_content_text(&renderer, theme) || !probe_present(&renderer, theme)) return false;
    if (fixture.calls != calls || scene.widgets.layout.revision != layout || renderer.content_text->rebuilds != builds ||
        renderer.glyph_text.cache_builds != raster || renderer.paint_instance_uploads != paint_uploads ||
        renderer.text_instance_uploads != text_uploads || renderer.media_instance_uploads != media_uploads ||
        renderer.painted_frames != painted || renderer.unchanged_present_skips - skips != 64) return false;
    log_write(LogLevel::info, "probe", "quick_switcher_64_zero_resolve_layout_shape_raster_upload_draw_passed");
    fixture.mixed = false; source.count = 100; ++source.revision; ++content.revision;
    content.edit = {utf8_literal("# release"), 9, 9, true, true, true};
    if (!show(L"quick-switcher-many-results.bmp")) return false;
    if (!ui_quick_switcher_scene_move(&scene, UiQuickSwitcherMove::last) || scene.selected != 99 ||
        !show(L"quick-switcher-keyboard-last.bmp")) return false;
    fixture.current = false;
    if (ui_quick_switcher_scene_selected(scene).action != UiQuickSwitcherAction::none) return false;
    fixture.current = true;
    source.count = 0; ++source.revision; ++content.revision;
    content.phase = UiQuickSwitcherPhase::empty; content.edit = {{}, 0, 0, true, true, true};
    if (!show(L"quick-switcher-empty.bmp")) return false;
    content.phase = UiQuickSwitcherPhase::no_matches;
    content.edit = {utf8_literal("unmatched original query"), 24, 24, true, true, true}; ++content.revision;
    if (!show(L"quick-switcher-no-matches.bmp")) return false;
    source.count = 100; ++source.revision; ++content.revision;
    content.phase = UiQuickSwitcherPhase::ready;
    content.edit = {utf8_literal("# release"), 9, 9, true, true, true};
    const u32 compact_width = static_cast<u32>(632.f * renderer.dpi / 96.f + .5f);
    const u32 compact_height = static_cast<u32>(360.f * renderer.dpi / 96.f + .5f);
    if (!probe_resize_surface(compact_width, compact_height) || !show(L"quick-switcher-narrow.bmp") ||
        scene.edit.slot != edit.slot || scene.edit.generation != edit.generation) return false;
    renderer_set_dpi(&renderer, 144);
    if (!probe_resize_surface(1440, 1050) || !show(L"quick-switcher-dpi144.bmp")) return false;
    theme = kLightTheme;
    if (!show(L"quick-switcher-light.bmp")) return false;
    theme = kDefaultTheme;
    const u64 picked[]{1000, 1003};
    fixture.people = true; source.count = 4; ++source.revision; ++content.revision;
    content.edit = {{}, 0, 0, true, true, true};
    content.heading = utf8_literal("Select friends");
    content.hint = utf8_literal("You can add 6 more friends.");
    content.status = utf8_literal("2 selected");
    content.picker = true; content.can_confirm = true;
    content.action = utf8_literal("Create group DM");
    content.checked = {picked, 2};
    if (!show(L"quick-switcher-picker.bmp") || !scene.accepted[0].enabled || scene.accepted[2].enabled ||
        ui_quick_switcher_scene_target(scene, scene.confirm).action != UiQuickSwitcherAction::confirm) return false;
    content.can_confirm = false; content.checked = {}; ++content.revision;
    if (!show(L"quick-switcher-picker-empty.bmp") ||
        ui_quick_switcher_scene_target(scene, scene.confirm).action != UiQuickSwitcherAction::none) return false;
    fixture.people = false;
    renderer_set_dpi(&renderer, dpi);
    if (!probe_resize_surface(width, height)) return false;
    log_write(LogLevel::info, "probe", "quick_switcher_scope_keyboard_editor_minimum_themes_dpi_passed");
    return true;
}
} // namespace maicord
