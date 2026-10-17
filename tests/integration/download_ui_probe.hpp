#pragma once
#include "probe_present.hpp"
#include "maicord/ui/download_scene.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
namespace download_probe_detail {
struct Source { UiDownloadRow rows[kUiDownloadMaximumRows]{}; u32 calls{}; bool current{true}; };
inline bool current(void* context) noexcept { return static_cast<Source*>(context)->current; }
inline bool resolve(void* context,u32 index,UiDownloadRow* row) noexcept {
    auto& s = *static_cast<Source*>(context); ++s.calls; *row = s.rows[index]; return true;
}
}
inline bool download_ui_probe(D3D11Renderer& renderer,bool (*capture)(const wchar_t*,const Theme&) noexcept) noexcept {
    if (!renderer.content_text || !renderer.dpi) return false;
    UiDownloadScene scene{}; download_probe_detail::Source fixture{};
    fixture.rows[0] = {100,utf8_literal("interface-study.png"),utf8_literal("MaiCord Lab • release-planning"),{},
        262144,1048576,UiDownloadPhase::downloading,false,true};
    fixture.rows[1] = {101,utf8_literal("prototype-walkthrough.mp4"),utf8_literal("MaiCord Lab • design-studio"),
        utf8_literal("Waiting for the current download"),0,20971520,UiDownloadPhase::queued,false,true};
    fixture.rows[2] = {102,utf8_literal("research-notes.zip"),utf8_literal("Workshop • project-notes • Forwarded attachment"),
        utf8_literal("Connection lost. Try again."),524288,2097152,UiDownloadPhase::failed,false,false,true,false,true};
    fixture.rows[3] = {103,utf8_literal("ui-colors.webp"),utf8_literal("MaiCord Lab • release-planning"),{},
        81920,81920,UiDownloadPhase::saved,false,false,false,true,true};
    fixture.rows[4] = {104,utf8_literal("agenda.txt"),utf8_literal("Direct conversation • Avery Chen"),
        utf8_literal("Cancelled • No file was saved"),0,4096,UiDownloadPhase::cancelled,false,false,false,false,true};
    fixture.rows[5] = {105,utf8_literal("review-report.pdf"),utf8_literal("MaiCord Lab • project-notes"),{},
        131072,131072,UiDownloadPhase::committing,false,true};
    fixture.rows[6] = {106,utf8_literal("original-recording.mp4"),utf8_literal("Workshop • Native UI study group"),{},
        327680,0,UiDownloadPhase::downloading,true,true};
    fixture.rows[7] = {107,utf8_literal("inline-document.png"),utf8_literal("MaiCord Lab • Forwarded attachment"),{},
        0,24000,UiDownloadPhase::preparing,false,true};
    UiDownloadSource source{&fixture,8,1,download_probe_detail::resolve,download_probe_detail::current};
    UiDownloadContent content{}; content.revision = 1;
    const u32 width = renderer.width,height = renderer.height,dpi = renderer.dpi;
    struct Restore {
        D3D11Renderer& renderer; UiDownloadScene& scene;
        const UiDownloadScene* old_scene; const UiWidgetManager* focus;
        ~Restore() noexcept {
            renderer.download_scene = old_scene; renderer.focus_owner = focus;
            renderer.retained_presentation.valid = false;
            content_text_retention_invalidate(&renderer.content_text_retention);
            ui_download_scene_release(&scene);
        }
    } restore{renderer,scene,renderer.download_scene,renderer.focus_owner};
    Theme theme = kDefaultTheme;
    const auto show = [&](const wchar_t* name) noexcept {
        const UiRect v{0,0,renderer.width * 96.f / renderer.dpi,renderer.height * 96.f / renderer.dpi};
        if (!ui_download_scene_update(&scene,&source,&content,v)) return false;
        renderer.download_scene = &scene; renderer.focus_owner = &scene.widgets;
        return renderer_rebuild_content_text(&renderer,theme) && probe_present(&renderer,theme) &&
            (!capture || capture(name,theme));
    };
    const auto warm = [&]() noexcept {
        const u32 calls = fixture.calls,layout = scene.widgets.layout.revision,builds = renderer.content_text->rebuilds,
            raster = renderer.glyph_text.cache_builds,paint_uploads = renderer.paint_instance_uploads,
            text_uploads = renderer.text_instance_uploads,media_uploads = renderer.media_instance_uploads,
            painted = renderer.painted_frames,skips = renderer.unchanged_present_skips;
        for (u32 i = 0; i < 64; ++i)
            if (!ui_download_scene_update(&scene,&source,&content,scene.viewport) ||
                !renderer_rebuild_content_text(&renderer,theme) || !probe_present(&renderer,theme)) return false;
        return fixture.calls == calls && scene.widgets.layout.revision == layout && renderer.content_text->rebuilds == builds &&
            renderer.glyph_text.cache_builds == raster && renderer.paint_instance_uploads == paint_uploads &&
            renderer.text_instance_uploads == text_uploads && renderer.media_instance_uploads == media_uploads &&
            renderer.painted_frames == painted && renderer.unchanged_present_skips - skips == 64;
    };
    if (!show(L"download-active.bmp") || !warm()) return false;
    log_write(LogLevel::info,"probe","download_64_zero_resolve_layout_shape_raster_upload_draw_passed");
    const auto first_handle = scene.primary[0];
    if (!ui_download_scene_page(&scene,1) || !show(L"download-terminal-and-committing.bmp") ||
        ui_download_scene_target(scene,scene.primary[0]).action != UiDownloadAction::reveal ||
        ui_download_scene_target(scene,scene.primary[1]).action != UiDownloadAction::none ||
        ui_download_scene_target(scene,scene.primary[2]).action != UiDownloadAction::cancel || !warm()) return false;
    if (!ui_download_scene_page(&scene,2) || !show(L"download-unknown-size-last-page.bmp") ||
        scene.first != 6 || scene.count != 2) return false;
    source.count = 0; ++source.revision; ++content.revision;
    if (!show(L"download-empty.bmp")) return false;
    content.mode = UiDownloadMode::confirmation; content.confirmation_id = 500;
    content.filename = utf8_literal("review-helper.exe");
    content.warning = utf8_literal("This file can run software on your computer. Save it only if you trust its source.");
    ++content.revision;
    if (!show(L"download-warning.bmp") || scene.widgets.focused.slot != scene.cancel.slot ||
        ui_download_scene_target(scene,scene.confirm).id != 500) return false;
    if (!probe_resize_surface(static_cast<u32>(320.f * renderer.dpi / 96.f + .5f),
        static_cast<u32>(420.f * renderer.dpi / 96.f + .5f)) || !show(L"download-warning-narrow.bmp")) return false;
    content.mode = UiDownloadMode::receipts; source.count = 8; ++source.revision; ++content.revision;
    if (!show(L"download-narrow.bmp") || scene.page_size != 1 ||
        scene.primary[0].slot != first_handle.slot || scene.primary[0].generation != first_handle.generation) return false;
    for (u32 page = 0; page < 8; ++page) {
        if (!ui_download_scene_page(&scene,page)) return false;
        const wchar_t* names[]{L"download-narrow-active.bmp",L"download-narrow-queued.bmp",L"download-narrow-failed.bmp",
            L"download-narrow-saved.bmp",L"download-narrow-cancelled.bmp",L"download-narrow-committing.bmp",
            L"download-narrow-unknown-size.bmp",L"download-narrow-preparing.bmp"};
        if (!show(names[page])) return false;
    }
    // The component's 240x300 layout is covered in its unit fixture. Keep this
    // complete shell capture tall enough for the application's native footer.
    if (!probe_resize_surface(static_cast<u32>(320.f * renderer.dpi / 96.f + .5f),
        static_cast<u32>(480.f * renderer.dpi / 96.f + .5f)) || !ui_download_scene_page(&scene,3) ||
        !show(L"download-short-saved.bmp")) return false;
    renderer_set_dpi(&renderer,144);
    if (!probe_resize_surface(1440,1080) || !ui_download_scene_page(&scene,0) || !show(L"download-dpi144.bmp")) return false;
    theme = kLightTheme;
    if (!show(L"download-light.bmp")) return false;
    theme.accent = {.95f,.85f,.4f,1};
    theme = ui_theme_resolve_defaults(theme); // semantic tokens follow the core accent
    if (!show(L"download-light-bright-accent.bmp")) return false;
    UiTextCommand commands[64]{}; UiTextCommandStream stream{}; UiTextOutput output{&stream,nullptr};
    if (!ui_text_command_stream_initialize(&stream,{commands,64}) || !ui_download_scene_text_output(scene,theme,&output)) return false;
    const auto foreground = ui_surface_foreground(theme,theme.accent);
    bool contrast = false;
    for (u32 i = 0; i < stream.count; ++i) if (commands[i].owner.manager == &scene.widgets &&
        commands[i].owner.handle.slot == scene.primary[2].slot) {
        const auto color = commands[i].color;
        contrast = color.red == foreground.red && color.green == foreground.green && color.blue == foreground.blue;
    }
    if (!contrast) return false;
    fixture.current = false;
    if (ui_download_scene_target(scene,scene.primary[0]).action != UiDownloadAction::none) return false;
    fixture.current = true;
    renderer_set_dpi(&renderer,dpi);
    if (!probe_resize_surface(width,height)) return false;
    log_write(LogLevel::info,"probe","download_bounds_pagination_progress_commit_confirm_cancel_retry_reveal_themes_dpi_passed");
    return true;
}
} // namespace maicord
