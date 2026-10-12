#pragma once
#include "scoped_search_ui_probe.hpp"
#include "maicord/ui/scene.hpp"

namespace maicord {
namespace search_polish_probe_detail {
inline bool resolve(void*, u32 i, UiSearchRow* out) noexcept {
    if (!out || i >= 6)
        return false;
    *out = {
        i < 3 ? 710ull : 720ull,
        810ull + i,
        utf8_literal("Native author"),
        i < 3 ? utf8_literal("#release-planning") : utf8_literal("#native-ui"),
        i % 2 ? utf8_literal(
                    "Original offline search text wraps into a taller result. "
                    "Compact cards use the actual supplied text height, keeping each author and timestamp "
                    "together. "
                    "No provider, account, avatar artwork or attachment is used by this synthetic fixture.")
              : utf8_literal("Hello, native search."),
        utf8_literal("10/03 12:30"),
        true,
        i == 0 || i == 3,
        i % 2 ? 120.f : 24.f};
    return true;
}
// Two 512x500-pixel maximum crops together use <=2,048,108 disk bytes.
// Borrow the existing normalized BGRA readback; no new image bank or GPU resource.
inline bool capture_crop(const surface_motion_probe_detail::Readback& rb, u32 image,
                         const wchar_t* filename) noexcept {
    if (!rb.images || image >= 4)
        return false;
    wchar_t path[1024]{};
    const DWORD n = GetEnvironmentVariableW(L"MAICORD_UI_PROBE_CAPTURE_DIR", path, 900);
    if (!n)
        return true;
    if (n >= 900)
        return false;
    u32 end = n;
    path[end++] = L'/';
    for (u32 i = 0; filename[i]; ++i) {
        if (end >= 1023)
            return false;
        path[end++] = filename[i];
    }
    const u32 width = minimum(rb.width, 512u), height = minimum(rb.height, 500u);
    if (!width || !height)
        return false;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    BITMAPFILEHEADER header{};
    BITMAPINFOHEADER info{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info);
    header.bfSize = header.bfOffBits + width * height * 4;
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    DWORD written = 0;
    bool ok = WriteFile(file, &header, sizeof(header), &written, nullptr) && written == sizeof(header) &&
              WriteFile(file, &info, sizeof(info), &written, nullptr) && written == sizeof(info);
    for (u32 y = 0; ok && y < height; ++y) {
        const u8* row = rb.images + image * rb.bytes + (y * rb.width + rb.width - width) * 4;
        ok = WriteFile(file, row, width * 4, &written, nullptr) && written == width * 4;
    }
    CloseHandle(file);
    return ok;
}
} // namespace search_polish_probe_detail

inline bool search_polish_ui_probe(D3D11Renderer& renderer) noexcept {
    using namespace search_polish_probe_detail;
    using scoped_search_probe_detail::text_ink;
    u32 stage = 1;
    const auto fail = [&]() noexcept {
        log_u64(LogLevel::error, "probe", "search_polish_failure_stage", stage);
        return false;
    };
    if (!renderer.ui_scene || !renderer.content_text || !renderer.dpi)
        return fail();
    UiSearchScene scene{};
    struct Restore {
        D3D11Renderer& r;
        UiSearchScene& scene;
        const UiSearchScene* search;
        const UiMediaPickerScene* gallery;
        const UiSettingsScene* settings;
        const PopupScene* popup;
        TextFormSurface* form;
        const UiWidgetManager* focus;
        ~Restore() noexcept {
            r.search_scene = search;
            r.media_picker_scene = gallery;
            renderer_bind_settings_scene(&r, settings);
            renderer_bind_popup_scene(&r, popup);
            renderer_bind_text_form(&r, form);
            renderer_bind_focus_owner(&r, focus);
            r.retained_presentation.valid = false;
            ui_search_scene_release(&scene);
        }
    } restore{renderer,
              scene,
              renderer.search_scene,
              renderer.media_picker_scene,
              renderer.settings_scene,
              renderer.popup_scene,
              renderer.text_form,
              renderer.focus_owner};
    renderer.media_picker_scene = nullptr;
    renderer_bind_settings_scene(&renderer, nullptr);
    renderer_bind_popup_scene(&renderer, nullptr);
    renderer_bind_text_form(&renderer, nullptr);
    renderer_bind_focus_owner(&renderer, nullptr);
    const auto header = ui_scene_header_geometry(*renderer.ui_scene);
    const float scale = static_cast<float>(renderer.dpi) / 96.f;
    auto viewport = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::content);
    viewport.right = renderer.width / scale;
    UiSearchSource source{nullptr, 6, 1, resolve};
    UiSearchContent content{};
    content.edit = {utf8_literal("hello"), 5, 5, true, true, false};
    content.heading = content.placeholder = utf8_literal("Search Native Garden");
    content.status = utf8_literal("Original offline results");
    content.total = 6;
    content.panel = true;
    content.phase = UiSearchPhase::ready;
    content.suggestions = true;
    content.revision = 1;
    surface_motion_probe_detail::Readback rb;
    if (!rb.initialize(renderer))
        return fail();
    const auto render = [&](u32 image) noexcept {
        if (!ui_search_scene_update(&scene, &source, &content, viewport, header.title, &header))
            return false;
        renderer.search_scene = &scene;
        renderer.retained_presentation.valid = false;
        return renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
               probe_present(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
               surface_motion_probe_detail::draw_fixed(renderer) && rb.read(renderer, image);
    };
    stage = 2;
    if (!render(0) || !ui_rect_equal(scene.field, header.search_field) ||
        scene.chooser.top != scene.field.bottom + 8 ||
        !text_ink(scene, renderer, rb, 0, content.edit.text) ||
        !text_ink(scene, renderer, rb, 0, utf8_literal("From a specific user")) ||
        !text_ink(scene, renderer, rb, 0, utf8_literal("from:")) ||
        !text_ink(scene, renderer, rb, 0, utf8_literal("More filters")))
        return fail();
    u32 count = 0;
    bool more = false;
    for (u32 i = 1; i < scene.widgets.count; ++i) {
        const auto a = scene.targets[i].action;
        if ((a >= UiSearchAction::filter_from && a <= UiSearchAction::filter_mentions) ||
            a == UiSearchAction::more_filters) {
            ++count;
            const auto* node = ui_widget_manager_layout_node(
                scene.widgets, {static_cast<u16>(i), scene.widgets.generation});
            if (!node || node->rectangle.bottom > scene.chooser.bottom)
                return fail();
            if (a == UiSearchAction::more_filters)
                more = ui_search_scene_hit_test(scene, node->rectangle.left + 40, node->rectangle.top + 12)
                           .action == a;
        }
    }
    if (count != 5 || !more || !capture_crop(rb, 0, L"search-polish-suggestions-crop.bmp"))
        return fail();
    stage = 3;
    content.suggestions = false;
    content.sort_menu = true;
    ++content.revision;
    if (!render(1) || scene.count < 2 || ui_rect_height(scene.row_rectangles[0]) != 72 ||
        ui_rect_height(scene.row_rectangles[1]) != 168 ||
        scene.group_rectangles[0].bottom != scene.row_rectangles[0].top ||
        !text_ink(scene, renderer, rb, 1, content.edit.text) ||
        !text_ink(scene, renderer, rb, 1, content.heading) ||
        !text_ink(scene, renderer, rb, 1, utf8_literal("6 results")) ||
        !text_ink(scene, renderer, rb, 1, utf8_literal("Newest")) ||
        !text_ink(scene, renderer, rb, 1, utf8_literal("Oldest")) ||
        !text_ink(scene, renderer, rb, 1, utf8_literal("Most relevant")) ||
        !text_ink(scene, renderer, rb, 1, utf8_literal("Native author")))
        return fail();
    const auto* oldest = ui_widget_manager_layout_node(scene.widgets, scene.buttons[4]);
    if (!oldest ||
        ui_search_scene_hit_test(scene, oldest->rectangle.left + 8, oldest->rectangle.top + 8).action !=
            UiSearchAction::oldest ||
        !capture_crop(rb, 1, L"search-polish-sort-crop.bmp"))
        return fail();
    log_write(LogLevel::info, "probe", "search_polish_native_passed");
    return true;
}
} // namespace maicord
