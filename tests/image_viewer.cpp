#include "maicord/ui/image_viewer.hpp"
#include "maicord/app/account_media.hpp"
#include "png_growth_fixture.hpp"
#include "maicord/media/png.hpp"
#include "maicord/platform/clipboard_image.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/text_presentation.hpp"
namespace maicord {
namespace {
void test_image_gallery(void (*check)(bool, const char*) noexcept) noexcept {
    UiImageViewer viewer{};
    check(ui_image_viewer_open(&viewer, {0, 0, 1200, 800}, {1600, 900}) &&
              !ui_rect_has_area(ui_image_viewer_button(viewer, ui_image_viewer_previous)) &&
              !ui_rect_has_area(ui_image_viewer_button(viewer, ui_image_viewer_open_original)),
          "gallery: a single item has no chevrons and optional actions stay hidden");
    const auto close = ui_image_viewer_button(viewer, ui_image_viewer_close);
    check(ui_image_viewer_set_actions(&viewer, ui_image_viewer_action_open | ui_image_viewer_action_copy) &&
              ui_rect_width(ui_image_viewer_button(viewer, ui_image_viewer_copy)) == 32 &&
              ui_image_viewer_button(viewer, ui_image_viewer_copy).right < close.left &&
              ui_image_viewer_button(viewer, ui_image_viewer_open_original).right <
                  ui_image_viewer_button(viewer, ui_image_viewer_copy).left &&
              ui_image_viewer_button(viewer, ui_image_viewer_fit).right <
                  ui_image_viewer_button(viewer, ui_image_viewer_open_original).left &&
              ui_rect_equal(ui_image_viewer_button(viewer, ui_image_viewer_close), close),
          "gallery: Copy and Open original join the toolbar left of Close without moving it");
    ui_image_viewer_zoom(&viewer, 2, {600, 400});
    check(ui_image_viewer_set_item(&viewer, {800, 800}, 1, 3, false) && viewer.zoom == 1 && viewer.pan.x == 0 &&
              ui_rect_width(viewer.image) == ui_rect_height(viewer.image) &&
              ui_rect_has_area(ui_image_viewer_button(viewer, ui_image_viewer_previous)) &&
              ui_rect_has_area(ui_image_viewer_button(viewer, ui_image_viewer_next)) &&
              ui_image_viewer_button(viewer, ui_image_viewer_previous).right <= viewer.body.left &&
              ui_image_viewer_button(viewer, ui_image_viewer_next).left >= viewer.body.right &&
              utf8_equal({viewer.footer, 6}, utf8_literal("2 / 3 ")),
          "gallery: a new item resets zoom and pan, and chevrons own gutters outside the media body");
    const auto previous = ui_image_viewer_button(viewer, ui_image_viewer_previous);
    const UiPoint at{(previous.left + previous.right) * .5f, (previous.top + previous.bottom) * .5f};
    ui_button_pointer(&viewer.widgets, UiButtonPointerEvent::press, at);
    const auto release = ui_button_pointer(&viewer.widgets, UiButtonPointerEvent::release, at);
    check(release.activate && viewer.widgets.storage[release.target.slot].id - 10 == ui_image_viewer_previous,
          "gallery: the previous chevron is an ordinary button target");
    check(ui_image_viewer_set_item(&viewer, {1920, 1080}, 2, 3, true) &&
              !ui_rect_has_area(ui_image_viewer_button(viewer, ui_image_viewer_zoom_in)) &&
              !ui_rect_has_area(ui_image_viewer_button(viewer, ui_image_viewer_copy)),
          "gallery: videos have no zoom or Copy");
    ui_image_viewer_zoom(&viewer, 2, {600, 400});
    check(viewer.zoom == 1, "gallery: video items ignore zoom requests");
    PaintStream paint{};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1200, 800) &&
              ui_image_viewer_paint_overlay(viewer, kDefaultTheme, &paint) && paint.count == 4 &&
              ui_icon_quad(paint.quads[1]) && ui_icon_quad(paint.quads[3]) && paint.clip_depth == 1,
          "gallery: chevrons paint above media as two round targets with icons");
    const UiRect origin{100, 600, 260, 690};
    ui_image_viewer_set_reveal(&viewer, 0, origin);
    const auto settled = viewer.image;
    check(ui_rect_equal(viewer.display, origin), "gallery: closed reveal places media at its inline origin");
    ui_image_viewer_set_reveal(&viewer, .5f, origin);
    const float midpoint = (origin.left + settled.left) * .5f - viewer.display.left;
    check(midpoint < .01f && midpoint > -.01f && ui_rect_equal(viewer.image, settled),
          "gallery: reveal interpolates only the painted rectangle, never layout or hit geometry");
    paint_stream_reset(&paint, 1200, 800);
    check(ui_image_viewer_paint(viewer, kDefaultTheme, &paint) &&
              paint.quads[0].fill.alpha == kUiImageViewerScrimOpacity * .5f,
          "gallery: the backdrop fades with the reveal toward its 92 percent scrim");
    ui_image_viewer_set_reveal(&viewer, 1, {});
    check(ui_rect_equal(viewer.display, viewer.image) && !ui_image_viewer_set_item(&viewer, {10, 10}, 3, 3, false),
          "gallery: a settled reveal paints at layout and out-of-range items reject");
    paint_stream_release(&paint);
}
void test_clipboard_image(void (*check)(bool, const char*) noexcept) noexcept {
    // Pure encoders only: tests never replace the real system clipboard.
    constexpr u32 width = 3, height = 2;
    const u8 rgba[width * height * 4]{255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0,
                                      10, 20, 30, 40, 200, 150, 100, 255, 1, 2, 3, 4};
    u8 png[512]{};
    const u64 png_bytes = clipboard_image_encode_png(rgba, width, height, width * 4, {png, sizeof(png)});
    static PngImage image{};
    static DeflateWorkspace workspace{};
    u8 scanlines[64]{};
    const auto decoded = png_decode_scanlines({png, static_cast<u32>(png_bytes)}, &image, &workspace,
                                              {scanlines, sizeof(scanlines)});
    bool same = decoded.ok() && image.width == width && image.height == height && image.color_type == 6;
    for (u32 y = 0; same && y < height; ++y)
        for (u32 x = 0; x < width * 4; ++x)
            same = same && scanlines[y * image.row_stride + 1 + x] == rgba[y * width * 4 + x];
    check(png_bytes == clipboard_image_png_bytes(width, height) && same,
          "clipboard image: stored-deflate PNG decodes back to identical straight RGBA through the native decoder");
    u8 dib[sizeof(BITMAPV5HEADER) + sizeof(rgba)]{};
    const u64 dib_bytes = clipboard_image_build_dib(rgba, width, height, width * 4, {dib, sizeof(dib)});
    BITMAPV5HEADER header{};
    copy_bytes(&header, dib, sizeof(header));
    bool bottom_up = dib_bytes == sizeof(dib) && header.bV5Height > 0 && header.bV5AlphaMask == 0xff000000u &&
                     header.bV5Compression == BI_BITFIELDS;
    for (u32 y = 0; bottom_up && y < height; ++y)
        for (u32 x = 0; bottom_up && x < width; ++x) {
            const u8* source = rgba + ((height - 1 - y) * width + x) * 4;
            const u8* pixel = dib + sizeof(header) + (y * width + x) * 4;
            bottom_up = pixel[0] == source[2] && pixel[1] == source[1] && pixel[2] == source[0] && pixel[3] == source[3];
        }
    check(bottom_up, "clipboard image: CF_DIBV5 payload is bottom-up BGRA with alpha");
    check(!clipboard_image_encode_png(rgba, width, height, width * 4, {png, 16}) && !clipboard_image_png_bytes(0, 1),
          "clipboard image: capacity limits are handled explicitly");
}
// Captures clipboard transactions in memory. Tests install it so the user's
// real clipboard is never opened, emptied or replaced.
struct CapturedClipboard {
    u32 opens, closes, publishes, refuse_at;
    UINT formats[4];
    u8 bytes[4][2048];
    u32 sizes[4];
};
CapturedClipboard g_captured{};
bool capture_open(void* context, HWND) noexcept {
    auto& captured = *static_cast<CapturedClipboard*>(context);
    ++captured.opens;
    captured.publishes = 0;
    return true;
}
bool capture_publish(void* context, UINT format, HGLOBAL data) noexcept {
    auto& captured = *static_cast<CapturedClipboard*>(context);
    if (captured.publishes >= 4 || captured.publishes + 1 == captured.refuse_at)
        return false;
    const SIZE_T bytes = GlobalSize(data);
    const void* source = GlobalLock(data);
    if (!source || bytes > sizeof(captured.bytes[0])) {
        if (source)
            GlobalUnlock(data);
        return false;
    }
    const u32 slot = captured.publishes++;
    captured.formats[slot] = format;
    captured.sizes[slot] = static_cast<u32>(bytes);
    copy_bytes(captured.bytes[slot], source, static_cast<u32>(bytes));
    GlobalUnlock(data);
    GlobalFree(data); // the sink owns accepted handles
    return true;
}
void capture_close(void* context) noexcept {
    ++static_cast<CapturedClipboard*>(context)->closes;
}
u32 decimal_at(const char* text, u32 at) noexcept {
    u32 value = 0;
    for (u32 index = 0; index < 10; ++index)
        value = value * 10 + static_cast<u32>(text[at + index] - '0');
    return value;
}
void test_clipboard_publish(void (*check)(bool, const char*) noexcept) noexcept {
    g_captured = {};
    const ClipboardSink previous =
        clipboard_sink_install({&g_captured, capture_open, capture_publish, capture_close});
    // Never a real window: if the sink were bypassed, OpenClipboard would fail.
    const HWND owner = reinterpret_cast<HWND>(usize{0x10});
    constexpr u32 width = 3, height = 2;
    const u8 rgba[width * height * 4]{255, 0,  0,  255, 0,   255, 0,   128, 0, 0, 255, 0,
                                      10,  20, 30, 40,  200, 150, 100, 255, 1, 2, 3,   4};
    const auto url = utf8_literal("https://cdn.discordapp.com/attachments/1/2/a\"b.png?ex=1&is=2");
    const UINT png_format = RegisterClipboardFormatW(L"PNG");
    const UINT html_format = RegisterClipboardFormatW(L"HTML Format");
    u8 dib[sizeof(BITMAPV5HEADER) + sizeof(rgba)]{};
    u8 png[512]{};
    const u64 dib_bytes = clipboard_image_build_dib(rgba, width, height, width * 4, {dib, sizeof(dib)});
    const u64 png_bytes = clipboard_image_encode_png(rgba, width, height, width * 4, {png, sizeof(png)});
    const bool written = clipboard_write_image(owner, rgba, width, height, width * 4, url);
    check(written && g_captured.opens == 1 && g_captured.closes == 1 && g_captured.publishes == 3 &&
              g_captured.formats[0] == CF_DIBV5 && g_captured.formats[1] == png_format &&
              g_captured.formats[2] == html_format && g_captured.sizes[0] >= dib_bytes &&
              bytes_equal(g_captured.bytes[0], dib, static_cast<u32>(dib_bytes)) &&
              g_captured.sizes[1] >= png_bytes &&
              bytes_equal(g_captured.bytes[1], png, static_cast<u32>(png_bytes)),
          "clipboard publish: Copy Image is one transaction of CF_DIBV5, PNG and HTML Format");
    const auto* html = reinterpret_cast<const char*>(g_captured.bytes[2]);
    const u32 start_html = decimal_at(html, 23), end_html = decimal_at(html, 43);
    const u32 start_fragment = decimal_at(html, 69), end_fragment = decimal_at(html, 93);
    constexpr char fragment[] =
        "<img src=\"https://cdn.discordapp.com/attachments/1/2/a&quot;b.png?ex=1&amp;is=2\">";
    check(written && bytes_equal(html, "Version:0.9\r\n", 13) && start_html == 105 &&
              bytes_equal(html + start_html, "<html>", 6) && end_html + 1 <= g_captured.sizes[2] &&
              html[end_html] == 0 && end_fragment - start_fragment == sizeof(fragment) - 1 &&
              bytes_equal(html + start_fragment, fragment, sizeof(fragment) - 1) &&
              bytes_equal(html + end_fragment, "<!--EndFragment-->", 18),
          "clipboard publish: CF_HTML offsets frame an escaped <img> of the source URL");
    char small[64]{};
    check(!clipboard_html_image_fragment(url, {small, sizeof(small)}) &&
              !clipboard_html_image_fragment(utf8_literal("https://x/\n"), {small, sizeof(small)}),
          "clipboard publish: CF_HTML rejects short capacity and control characters");
    g_captured.publishes = 0;
    check(clipboard_write_utf8(owner, utf8_literal("h\xC3\xA9llo")) && g_captured.opens == 2 &&
              g_captured.publishes == 1 && g_captured.formats[0] == CF_UNICODETEXT &&
              g_captured.sizes[0] >= 6 * sizeof(wchar_t) &&
              reinterpret_cast<const wchar_t*>(g_captured.bytes[0])[1] == L'\x00e9' &&
              reinterpret_cast<const wchar_t*>(g_captured.bytes[0])[5] == 0,
          "clipboard publish: Copy Link writes terminated UTF-16 text through the same sink");
    g_captured.refuse_at = 2;
    check(!clipboard_write_image(owner, rgba, width, height, width * 4, url) && g_captured.opens == 3 &&
              g_captured.closes == 3 && g_captured.publishes == 1,
          "clipboard publish: a refused format fails the transaction and closes the sink");
    g_captured.refuse_at = 0;
    check(!clipboard_write_image(owner, rgba, 0, height, 0, url) &&
              !clipboard_write_image(nullptr, rgba, width, height, width * 4) && g_captured.opens == 3,
          "clipboard publish: invalid images and owners never open the clipboard");
    clipboard_sink_install(previous);
}
} // namespace
void test_image_viewer(void (*check)(bool, const char*) noexcept) noexcept {
    UiImageViewer viewer{};
    check(ui_image_viewer_open(&viewer, {0, 0, 800, 600}, {2048, 1024}) &&
              ui_rect_width(viewer.image) == 752 && ui_rect_height(viewer.image) == 376,
          "viewer fits large image within viewport without changing aspect ratio");
    const UiPoint center{400, 312};
    ui_image_viewer_zoom(&viewer, 2, center);
    check(ui_rect_width(viewer.image) == 1504 && viewer.pan.x == 0 && viewer.pan.y == 0,
          "viewer zoom preserves centered anchor");
    ui_image_viewer_pan(&viewer, {10000, -10000});
    check(viewer.image.left == viewer.body.left && viewer.image.bottom == viewer.body.bottom,
          "viewer pan clamps both edges without losing the image");
    ui_image_viewer_zoom(&viewer, .01f, center);
    check(viewer.zoom == 1 && viewer.pan.x == 0 && viewer.pan.y == 0, "fit command resets zoom and pan");
    check(ui_image_viewer_layout(&viewer, {0, 0, 1200, 800}) && ui_rect_width(viewer.image) == 1152,
          "viewer responds to viewport resize");
    const u32 revision = viewer.revision;
    check(ui_image_viewer_layout(&viewer, {0, 0, 1200, 800}) && viewer.revision == revision,
          "unchanged viewer layout preserves retention");
    const auto save_handle = viewer.buttons[4];
    const auto close_rectangle = ui_image_viewer_button(viewer,3);
    check(!viewer.can_save && viewer.widgets.count == ui_image_viewer_button_count + 1 && !ui_rect_has_area(ui_image_viewer_button(viewer,4)) &&
              !ui_widget_manager_input_enabled(viewer.widgets,save_handle),
          "viewer fifth Save control is preallocated but hidden and unfocusable by default");
    check(ui_image_viewer_set_save_available(&viewer,true) && viewer.can_save &&
              ui_widget_manager_input_enabled(viewer.widgets,save_handle) &&
              ui_rect_width(ui_image_viewer_button(viewer,4)) == 32 &&
              ui_rect_height(ui_image_viewer_button(viewer,4)) == 32 &&
              ui_rect_equal(ui_image_viewer_button(viewer,3),close_rectangle) &&
              viewer.buttons[4].slot == save_handle.slot && viewer.buttons[4].generation == save_handle.generation,
          "viewer Save availability exposes one exact stable native action without moving existing toolbar controls");
    const auto save_revision = viewer.revision, save_layout = viewer.widgets.layout.revision;
    bool save_retained = true;
    for (u32 i = 0; i < 64; ++i) save_retained &= ui_image_viewer_set_save_available(&viewer,true);
    check(save_retained && viewer.revision == save_revision && viewer.widgets.layout.revision == save_layout,
          "viewer unchanged Save availability performs no layout or presentation work");
    auto save_rectangle = ui_image_viewer_button(viewer,4);
    auto save_point = UiPoint{save_rectangle.left + 16,save_rectangle.top + 16};
    const auto save_hit = ui_widget_manager_hit_test(viewer.widgets,save_point.x,save_point.y);
    const auto save_press = ui_button_pointer(&viewer.widgets,UiButtonPointerEvent::press,save_point);
    const auto save_release = ui_button_pointer(&viewer.widgets,UiButtonPointerEvent::release,save_point);
    check(save_hit.slot == save_handle.slot && save_hit.generation == save_handle.generation && save_press.handled &&
              save_release.activate && save_release.target.slot == save_handle.slot,
          "viewer Save activates only its exact fifth-button target after a matching release");
    ui_widget_manager_focus(&viewer.widgets,save_handle);
    ui_button_pointer(&viewer.widgets,UiButtonPointerEvent::press,save_point);
    check(ui_image_viewer_set_save_available(&viewer,false) && !viewer.can_save &&
              !viewer.widgets.captured.generation && !viewer.widgets.focused.generation &&
              !ui_rect_has_area(ui_image_viewer_button(viewer,4)) &&
              !ui_button_pointer(&viewer.widgets,UiButtonPointerEvent::release,save_point).activate,
          "viewer removing Save authority cancels pressed and focused Save without activating a replacement control");
    ui_image_viewer_set_save_available(&viewer,true);
    bool save_minimum = ui_image_viewer_layout(&viewer,{0,0,240,160});
    for (u32 i = 0; i < 5; ++i) {
        const auto a = ui_image_viewer_button(viewer,i);
        save_minimum &= ui_rect_has_area(a) && a.left >= 0 && a.right <= 240 && a.top >= 0 && a.bottom <= 160;
        for (u32 j = 0; j < i; ++j)
            save_minimum &= !ui_rect_has_area(ui_rect_intersection(a,ui_image_viewer_button(viewer,j)));
    }
    check(save_minimum,"viewer five-button toolbar stays disjoint and reachable at the minimum viewport");
    ui_image_viewer_layout(&viewer,{0,0,1200,800});
    ui_image_viewer_set_save_available(&viewer,false);
    ui_widget_manager_focus(&viewer.widgets,viewer.buttons[3]);
    UiImageViewer inactive{};
    check(!ui_image_viewer_set_save_available(&inactive,true),"inactive viewer cannot acquire Save authority");
    PaintStream paint{};
    UiTextCommand commands[5]{};
    UiTextCommandStream text{};
    UiTextOutput text_output{&text, nullptr};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1200, 800) &&
              ui_image_viewer_paint(viewer, kDefaultTheme, &paint) && paint.count == 10 &&
              ui_icon_quad(paint.quads[2]) && ui_icon_quad(paint.quads[8]) &&
              ui_text_command_stream_initialize(&text, {commands, 5}) &&
              ui_image_viewer_text_output(viewer, kDefaultTheme, &text_output) && text.count == 1,
          "viewer uses shared paint and text streams for modal controls");
    const auto outlines = [&]() noexcept {
        u32 count = 0;
        for (u32 i = 0; i < paint.count; ++i)
            if (paint.quads[i].border_width > 0)
                ++count;
        return count;
    };
    check(outlines() == 0, "pointer opening does not paint a keyboard focus outline");
    Theme light_gradient = kLightTheme;
    light_gradient.background = background_theme_defaults();
    light_gradient.background.intensity = 100;
    ui_text_command_stream_reset(&text);
    paint_stream_reset(&paint, 1200, 800);
    bool light_controls = ui_image_viewer_paint(viewer, light_gradient, &paint) &&
                          ui_image_viewer_text_output(viewer, light_gradient, &text_output) && text.count == 1 &&
                          text.storage[0].color.red > .9f;
    u32 bright_icons = 0;
    for (u32 i = 0; light_controls && i < paint.count; ++i) {
        const auto& quad = paint.quads[i];
        if (ui_icon_quad(quad) && quad.fill.red > .8f && quad.fill.green > .8f && quad.fill.blue > .8f)
            ++bright_icons;
    }
    check(
        light_controls && bright_icons == 4,
        "viewer keeps actual toolbar strokes and footer light against its black scrim in Light gradients");
    viewer.keyboard_focus = true;
    paint_stream_reset(&paint, 1200, 800);
    ui_image_viewer_paint(viewer, kDefaultTheme, &paint);
    check(outlines() == 1, "keyboard navigation exposes focused viewer control");
    const auto control = ui_image_viewer_button(viewer, 1);
    const UiPoint point{control.left + 16, control.top + 16};
    const auto hover = ui_button_pointer(&viewer.widgets, UiButtonPointerEvent::move, point);
    const auto down = ui_button_pointer(&viewer.widgets, UiButtonPointerEvent::press, point);
    check(hover.handled && hover.changed && down.handled && !down.activate &&
              viewer.widgets.captured.slot == viewer.buttons[1].slot,
          "shared button owns hover and defers pointer activation until release");
    ui_button_pointer(&viewer.widgets, UiButtonPointerEvent::move, {0, 0});
    check(!ui_button_pointer(&viewer.widgets, UiButtonPointerEvent::release, {0, 0}).activate &&
              !viewer.widgets.captured.generation,
          "dragging off a pressed button cancels activation and releases ownership");
    ui_button_pointer(&viewer.widgets, UiButtonPointerEvent::press, point);
    check(ui_button_pointer(&viewer.widgets, UiButtonPointerEvent::release, point).activate,
          "shared button activates exactly on matching release");
    ui_button_pointer(&viewer.widgets, UiButtonPointerEvent::press, point);
    auto* button = ui_widget_manager_resolve(&viewer.widgets, viewer.buttons[1]);
    button->flags &= ~ui_widget_enabled;
    const auto disabled_release = ui_button_pointer(&viewer.widgets, UiButtonPointerEvent::release, point);
    check(disabled_release.handled && !disabled_release.activate && !viewer.widgets.captured.generation,
          "disabling a pressed button cancels activation without leaking capture");
    button->flags |= ui_widget_enabled;
    const auto target = ui_widget_manager_hit_test(viewer.widgets, point.x, point.y);
    check(ui_button_pointer_target(&viewer.widgets, UiButtonPointerEvent::press, target).handled &&
              ui_button_pointer_target(&viewer.widgets, UiButtonPointerEvent::release, target).activate,
          "pretested button target preserves matching release activation");
    ui_button_pointer_target(&viewer.widgets, UiButtonPointerEvent::press, target);
    auto stale = target;
    ++stale.generation;
    check(!ui_button_pointer_target(&viewer.widgets, UiButtonPointerEvent::release, stale).activate &&
              !viewer.widgets.captured.generation,
          "pretested stale target cannot activate and releases capture");
    ui_button_pointer_target(&viewer.widgets, UiButtonPointerEvent::press, target);
    check(!ui_button_pointer_target(&viewer.widgets, UiButtonPointerEvent::cancel, target).activate &&
              !viewer.widgets.captured.generation && !viewer.widgets.hovered.generation,
          "cancel ignores pretested target and clears button ownership");
    paint_stream_release(&paint);
    test_image_gallery(check);
    test_clipboard_image(check);
    test_clipboard_publish(check);

    AccountMedia media{};

    check(account_media_initialize(&media).ok() && !media.expanded_scanlines,
          "large PNG decode scratch remains absent until needed");
    if (media.started) {
        const auto key = media_asset_attachment(1, MediaAttachmentFormat::png, MediaResolutionTier::px2048);
        const CdnResponseResult policy{{},
                                       {},
                                       2000,
                                       0,
                                       sizeof(kPngGrowthFixture),
                                       200,
                                       CdnResponseDisposition::accept_body,
                                       CdnHttpError::none,
                                       true,
                                       false};
        const auto decoded = cdn_media_pipeline_admit_static_png(
            &media.pipeline, key, policy, {kPngGrowthFixture, sizeof(kPngGrowthFixture)});
        check(decoded.decoded() && decoded.image.width == 1536 && decoded.image.height == 1536 &&
                  media.expanded_scanlines &&
                  media.pipeline.scanline_capacity >= decoded.image.inflated_bytes &&
                  media.pipeline.scanline_capacity < 7u * 1024u * 1024u,
              "large inline/viewer PNG grows decode scratch to demand without clipboard conversion");
        if (decoded.decoded()) {
            const auto* last =
                decoded.scanlines.data + (1536 - 1) * decoded.image.row_stride + 1 + (1536 - 1) * 3;
            check(last[0] == 255 && last[1] == 0 && last[2] == 0,
                  "large PNG preserves pixels beyond the former preview ceiling");
        }
        check(account_media_stop(&media, 1000), "large PNG scratch owner releases cleanly");
    }
}
} // namespace maicord
