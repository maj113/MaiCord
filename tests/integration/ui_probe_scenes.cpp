#include "ui_probe_internal.hpp"
#include "../support/model_store_fixtures.hpp"
#include "probe_capture.hpp"
#include "maicord/ui/chat/composer/voice_bar.hpp"
#include "maicord/ui/widgets/presence.hpp"
#include "maicord/ui/account/profile_appearance.hpp"
#include "maicord/ui/chat/private_channel_title.hpp"
#include "maicord/ui/widgets/activity_status.hpp"
#include "maicord/ui/account/friends_media.hpp"
#include "native_renderer_formats_probe.hpp"
#include "maicord/render/text/native_glyph_outlines.hpp"
#include "../support/media_fixture.hpp"
#include "unread_boundary_ui_probe.hpp"
#include "thread_browser_ui_probe.hpp"
#include "message_header_clip_ui_probe.hpp"
#include "maicord/ui/widgets/settings_choice.hpp"

namespace maicord {
namespace ui_probe_detail {

bool probe_login_edit_centered() noexcept {
    const auto& edit = g_probe.renderer.login_edit;
    const auto field =
        login_scene_content_rectangle(g_probe.login_scene, g_probe.login_view.focus == LoginControl::login
                                                               ? LoginElement::login_field
                                                               : LoginElement::password_field);
    const float center = (field.top + field.bottom) * 0.5f;
    const auto near_center = [&](float top, float height) noexcept {
        const float offset = top + height * 0.5f - center;
        return offset >= -0.25f && offset <= 0.25f;
    };
    if (!edit.active || edit.width != ui_rect_width(field) ||
        !near_center(edit.caret.top, edit.caret.height))
        return false;
    for (u32 n = 0; n < edit.selection_count; ++n)
        if (!near_center(edit.selections[n].top, edit.selections[n].height))
            return false;
    for (u32 n = 0; n < g_probe.renderer.login_composition_metric_count; ++n) {
        const auto& metric = g_probe.renderer.login_composition_metrics[n].metric;
        if (!near_center(metric.top, metric.height))
            return false;
    }
    return true;
}

bool probe_text_pixels(ConstSlice<UiRect> rectangles, u8 threshold) noexcept {
    // Glyph counts alone do not prove a draw occurred. Read the actual render
    // target, excluding the QR region, before the flip-model buffer advances.
    if (!d3d11_renderer_draw_stream(&g_probe.renderer, kDefaultTheme, true, true, nullptr))
        return false;
    ProbeTargetReadback readback;
    D3D11_TEXTURE2D_DESC description{};
    bool visible = readback.read_target(g_probe.renderer, description) == 0;
    const auto& mapped = readback.mapped;
    if (visible) {
        const float scale = g_probe.renderer.dpi / 96.0f;
        for (u32 index = 0; index < rectangles.count; ++index) {
            const auto rectangle = rectangles[index];
            const u32 left = static_cast<u32>(maximum(0.0f, rectangle.left * scale));
            const u32 top = static_cast<u32>(maximum(0.0f, rectangle.top * scale));
            const u32 right =
                minimum(description.Width, static_cast<u32>(maximum(0.0f, rectangle.right * scale)));
            const u32 bottom =
                minimum(description.Height, static_cast<u32>(maximum(0.0f, rectangle.bottom * scale)));
            u32 ink = 0;
            for (u32 y = top; y < bottom; ++y)
                for (u32 x = left; x < right; ++x) {
                    const u8* pixel = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                    if (pixel[0] > threshold && pixel[1] > threshold && pixel[2] > threshold)
                        ++ink;
                }
            visible = visible && ink >= 8;
            if (ink < 8) {
                log_u64(LogLevel::error, "probe", "missing_text_rectangle", index);
                log_u64(LogLevel::error, "probe", "missing_text_width", right - left);
                log_u64(LogLevel::error, "probe", "missing_text_height", bottom - top);
            }
        }
    }
    return visible;
}

bool probe_login_recovery_text_pixels(ConstSlice<UiRect> rectangles) noexcept {
    if (!rectangles.data || rectangles.count != 4)
        return false;
    const UiRect neutral[]{rectangles[0], rectangles[3]};
    // Keep heading/button ink distinct from the button fill. Back and challenge
    // guidance use accent/error colors against the dark card background.
    return probe_text_pixels({neutral, 2}) && probe_text_pixels({rectangles.data + 1, 2}, 70);
}

bool probe_empty_outline_cache() noexcept {
    auto& renderer = g_probe.renderer;
    auto& owner = renderer.glyph_text;
    u32 selected = owner.font_face_count;
    for (u32 i = 0; i < owner.font_face_count; ++i)
        if (!(owner.font_faces[i].flags & 2u)) {
            selected = i;
            break;
        }
    if (selected == owner.font_face_count)
        return false;
    auto& retained = owner.font_faces[selected];
    TextRun run{};
    run.em_size = 16;
    run.glyph_count = 1;
    ShapedText text{};
    text.runs = &run;
    text.run_count = 1;
    u16 space = 0, letter = 0;
    run.native_font = true;
    run.native_face = retained.native_face;
    text.set_native_fonts(owner.system.native_fonts);
    const auto* record =
        native_font_run_get(*native_font_session_runs(owner.system.native_fonts), retained.native_face);
    const auto* font =
        record ? native_font_get(*native_font_session_fonts(owner.system.native_fonts), record->source.face)
               : nullptr;
    if (!font || sfnt_cmap_glyph(font->cmap, 32, 0, &space) != SfntStatus::ok ||
        sfnt_cmap_glyph(font->cmap, 'A', 0, &letter) != SfntStatus::ok)
        return false;
    if (!space || !letter)
        return false;
    TextGlyph glyphs[]{{9.25f, 0, 0, space, 0}, {11.f, 0, 0, letter, 0}};
    text.glyphs = glyphs;
    text.glyph_count = 1;
    GlyphQuad quads[2]{};
    u32 count = 0;
    bool invalidated = false;
    D3D11GlyphTextAppendRequest request{};
    request.text = &text;
    request.output = quads;
    request.capacity = 2;
    request.count = &count;
    request.atlas_invalidated = &invalidated;
    request.dpi = renderer.dpi;
    request.color = {1, 1, 1, 1};
    retained.empty_glyph_plus_one = 0;
    const u32 entries = owner.allocator.entry_count, builds = owner.cache_builds;
    const auto dispatches = owner.gpu.dispatch_count;
    if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) || count || invalidated ||
        retained.empty_glyph_plus_one != static_cast<u32>(space) + 1 ||
        owner.allocator.entry_count != entries || owner.cache_builds != builds ||
        owner.gpu.dispatch_count != dispatches)
        return false;
    // A cache hit must not even instantiate an outline provider. Restore the
    // real provider on both success and failure, releasing any unexpected one.
    auto* saved = owner.native_outlines;
    owner.native_outlines = nullptr;
    run.em_size = 32;
    request.dpi = renderer.dpi * 2;
    glyphs[0].advance_offset = -1024;
    glyphs[0].ascender_offset = 4096;
    const bool appended = d3d11_glyph_text_append(&owner, renderer.graphics.context, request);
    const bool skipped = owner.native_outlines == nullptr;
    native_glyph_outlines_release(&owner.native_outlines);
    owner.native_outlines = saved;
    if (!appended || !skipped || count || invalidated || owner.allocator.entry_count != entries ||
        owner.cache_builds != builds || owner.gpu.dispatch_count != dispatches)
        return false;
    // Positive advance on an empty glyph still moves the following ink.
    run.em_size = 16;
    request.dpi = renderer.dpi;
    glyphs[0].advance_offset = glyphs[0].ascender_offset = 0;
    text.glyph_count = run.glyph_count = 2;
    retained.empty_glyph_plus_one = 0;
    if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) || count != 1 || invalidated)
        return false;
    const auto expected = quads[0];
    count = 0;
    if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) || count != 1 || invalidated ||
        !bytes_equal(&expected, quads, sizeof(expected)))
        return false;
    log_write(LogLevel::info, "probe", "native_empty_outline_skips_provider_and_atlas");
    return true;
}

bool probe_color_foreground_cache(bool native) noexcept {
    auto& renderer = g_probe.renderer;
    auto& owner = renderer.glyph_text;
    NativeFontRunHandle color_face{};
    for (u32 i = 0; i < owner.font_face_count && !color_face.slot; ++i)
        if (owner.font_faces[i].flags & 2u)
            color_face = owner.font_faces[i].native_face;
    if (!color_face.slot)
        return false;
    auto* fonts = owner.system.native_fonts;
    const auto* record = native_font_run_get(*native_font_session_runs(fonts), color_face);
    if (!record)
        return false;
    // Glyph zero in the selected emoji font has ordinary contours. Exercise
    // foreground-dependent output without asking the shaper to change fonts.
    TextGlyph glyph{32, 0, 0, 0, 0};
    TextRun run{};
    run.native_font = true;
    run.native_face = color_face;
    run.em_size = 32;
    run.glyph_count = 1;
    ShapedText text{};
    text.glyphs = &glyph;
    text.glyph_count = 1;
    text.runs = &run;
    text.run_count = 1;
    // Borrowed view; the renderer owns the session and its face record.
    text.set_native_fonts(fonts);
    GlyphQuad quads[5]{};
    u32 count = 0;
    bool invalidated = false;
    D3D11GlyphTextAppendRequest request{};
    request.text = &text;
    request.output = quads;
    request.capacity = 5;
    request.count = &count;
    request.atlas_invalidated = &invalidated;
    request.dpi = renderer.dpi;
    const u32 before = owner.cache_builds;
    request.color = {native ? .9375f : .875f, 0, 0, 1};
    if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) || count != 1 ||
        owner.cache_builds != before + 1)
        return false;
    request.color = {0, 0, native ? .8125f : .75f, 1};
    if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) || count != 2 ||
        owner.cache_builds != before + 2)
        return false;
    request.color = {native ? .9375f : .875f, 0, 0, 1};
    if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) || count != 3 ||
        owner.cache_builds != before + 2 || invalidated || quads[0].u0 != quads[2].u0 ||
        quads[0].v0 != quads[2].v0)
        return false;
    if (native) {
        const auto* font = native_font_get(*native_font_session_fonts(fonts), record->source.face);
        if (!font || sfnt_cmap_glyph(font->cmap, 0x1f604, 0, &glyph.index) != SfntStatus::ok ||
            !glyph.index)
            return false;
        // A real COLR glyph builds once; repeating it is an atlas hit.
        if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) || count != 4)
            return false;
        const u32 native_builds = owner.cache_builds, native_faces = owner.font_face_count;
        if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) || count != 5 ||
            owner.cache_builds != native_builds || owner.font_face_count != native_faces ||
            quads[3].u0 != quads[4].u0 || quads[3].v0 != quads[4].v0 || quads[3].u1 != quads[4].u1 ||
            quads[3].v1 != quads[4].v1)
            return false;
    }
    auto* view = d3d11_glyph_text_color_view(owner);
    if (!view)
        return false;
    ID3D11Resource* resource = nullptr;
    ID3D11Texture2D* source = nullptr;
    view->GetResource(&resource);
    const bool texture =
        SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source)));
    com_release(resource);
    ProbeTargetReadback readback;
    D3D11_TEXTURE2D_DESC desc{};
    const u32 read_state = texture ? readback.read(renderer, source, desc) : 1;
    com_release(source);
    if (read_state || desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
        return false;
    const auto& mapped = readback.mapped;
    bool valid = true;
    for (u32 i = 0; i < 2; ++i) {
        const auto& q = quads[i];
        u32 ink = 0;
        const u32 left = static_cast<u32>((q.u0 - 2) * desc.Width),
                  right = static_cast<u32>((q.u1 - 2) * desc.Width);
        const u32 top = static_cast<u32>(q.v0 * desc.Height), bottom = static_cast<u32>(q.v1 * desc.Height);
        if (right > desc.Width || bottom > desc.Height) {
            valid = false;
            break;
        }
        for (u32 y = top; y < bottom; ++y)
            for (u32 x = left; x < right; ++x) {
                const auto* p = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                if (p[i ? 2 : 0] > 128)
                    ++ink;
                if (p[i ? 0 : 2] || p[1])
                    valid = false;
            }
        valid = valid && ink >= 8;
    }
    if (native) {
        const auto& a = quads[3];
        const auto& b = quads[4];
        valid = valid && a.u0 >= 2 && b.u0 >= 2 && a.u1 >= a.u0 && b.u1 >= b.u0 && a.v0 >= 0 && b.v0 >= 0 &&
                a.v1 >= a.v0 && b.v1 >= b.v0 && a.v1 <= 1 && b.v1 <= 1 &&
                bytes_equal(&a.left, &b.left, 4 * sizeof(float)) &&
                bytes_equal(&a.color, &b.color, sizeof(Color));
        if (!valid)
            return false;
        const u32 ax = static_cast<u32>((a.u0 - 2) * desc.Width), ay = static_cast<u32>(a.v0 * desc.Height),
                  bx = static_cast<u32>((b.u0 - 2) * desc.Width), by = static_cast<u32>(b.v0 * desc.Height);
        const u32 aw = static_cast<u32>((a.u1 - 2) * desc.Width) - ax,
                  ah = static_cast<u32>(a.v1 * desc.Height) - ay,
                  bw = static_cast<u32>((b.u1 - 2) * desc.Width) - bx,
                  bh = static_cast<u32>(b.v1 * desc.Height) - by;
        valid = valid && aw && ah && aw == bw && ah == bh && ax + aw <= desc.Width &&
                bx + bw <= desc.Width && ay + ah <= desc.Height && by + bh <= desc.Height;
        if (valid)
            for (u32 y = 0; y < ah; ++y)
                valid =
                    valid &&
                    bytes_equal(static_cast<const u8*>(mapped.pData) + (ay + y) * mapped.RowPitch + ax * 4,
                                static_cast<const u8*>(mapped.pData) + (by + y) * mapped.RowPitch + bx * 4,
                                aw * 4);
    }
    if (valid)
        log_write(LogLevel::info, "probe",
                  native ? "native_colr_run_pixels_and_cache_reuse_passed"
                         : "color_foreground_pixels_and_cache_reuse_passed");
    return valid;
}

} // namespace ui_probe_detail

namespace {

struct ProbePixelSample {
    UiPoint point;
    Color expected;
    u32 tolerance;
};
bool probe_exact_pixels(ConstSlice<ProbePixelSample> samples) noexcept {
    if (!d3d11_renderer_draw_stream(&g_probe.renderer, kDefaultTheme, true, true, nullptr))
        return false;
    ProbeTargetReadback readback;
    D3D11_TEXTURE2D_DESC description{};
    bool ok = readback.read_target(g_probe.renderer, description) == 0;
    const auto& mapped = readback.mapped;
    const bool bgra = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    if (!bgra && description.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
        return false;
    if (ok) {
        const float scale = g_probe.renderer.dpi / 96.f;
        for (u32 i = 0; i < samples.count; ++i) {
            const auto& sample = samples[i];
            const i32 x = static_cast<i32>(sample.point.x * scale),
                      y = static_cast<i32>(sample.point.y * scale);
            if (x < 0 || y < 0 || static_cast<u32>(x) >= description.Width ||
                static_cast<u32>(y) >= description.Height) {
                ok = false;
                continue;
            }
            const u8* pixel = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4;
            const u32 expected[]{static_cast<u32>(sample.expected.red * 255.f + .5f),
                                 static_cast<u32>(sample.expected.green * 255.f + .5f),
                                 static_cast<u32>(sample.expected.blue * 255.f + .5f)};
            for (u32 channel = 0; channel < 3; ++channel) {
                const u32 actual = pixel[bgra ? 2 - channel : channel];
                if (maximum(actual, expected[channel]) - minimum(actual, expected[channel]) >
                    sample.tolerance) {
                    ok = false;
                    log_u64(LogLevel::error, "probe", "layer_pixel_sample", i);
                    log_u64(LogLevel::error, "probe", "layer_pixel_expected", expected[channel]);
                    log_u64(LogLevel::error, "probe", "layer_pixel_actual", actual);
                }
            }
        }
    }
    return ok;
}

} // namespace

namespace ui_probe_detail {

bool probe_login_pixels(bool restoring) noexcept {
    const LoginElement elements[]{LoginElement::heading, LoginElement::login_field, LoginElement::submit};
    UiRect rectangles[3]{};
    for (u32 i = 0; i < (restoring ? 1u : 3u); ++i)
        rectangles[i] = login_scene_content_rectangle(g_probe.login_scene,
                                                      restoring ? LoginElement::root : elements[i]);
    return probe_text_pixels({rectangles, restoring ? 1u : 3u});
}

bool probe_prepare_capture() noexcept {
    // Pixel fixtures read the fully drawn target directly. Display pacing and
    // swap-chain presentation add no coverage here and can stall an occluded
    // or low-refresh desktop. Keep the production layout/paint path intact.
    return d3d11_renderer_paint_layout(
        &g_probe.renderer, kDefaultTheme,
        renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme));
}
bool probe_capture_frame(const wchar_t* filename) noexcept {
    return probe_capture_frame(filename, kDefaultTheme);
}
bool probe_capture_frame(const wchar_t* filename, const Theme& theme) noexcept {
    wchar_t prefix[80]{};
    const DWORD prefix_chars = GetEnvironmentVariableW(L"MAICORD_UI_PROBE_CAPTURE_PREFIX", prefix, 80);
    if (prefix_chars >= 80)
        return false;
    for (u32 i = 0; i < prefix_chars; ++i)
        if (filename[i] != prefix[i])
            return true;
    if (scene_memory && !scene_memory->snapshot(filename, &g_probe.renderer))
        return false;
    wchar_t directory[1024]{};
    const DWORD chars = GetEnvironmentVariableW(L"MAICORD_UI_PROBE_CAPTURE_DIR", directory, 900);
    if (!chars)
        return true;
    if (chars >= 900)
        return false;
    // Capture every animated text material at the same 420 ms phase. Keep
    // animation enabled; restoring the start times leaves the live probe's
    // scheduling/retention checks intact.
    struct RestoreEffectStarts {
        D3D11Renderer& renderer;
        float starts[kTextEffectCapacity];
        ~RestoreEffectStarts() noexcept {
            for (u32 i = 0; i < renderer.text_effect_count; ++i)
                renderer.text_effects.materials[i].reserved[0] = starts[i];
            renderer.text_effect_uploaded_count = 0;
        }
    } restore{g_probe.renderer, {}};
    g_probe.renderer.text_effect_uploaded_count = 0;
    for (u32 i = 0; i < g_probe.renderer.text_effect_count; ++i) {
        auto& start = g_probe.renderer.text_effects.materials[i].reserved[0];
        restore.starts[i] = start;
        start = 9.58f;
    }
    // Read a fully drawn frame before the flip-model swapchain advances.
    if (!d3d11_renderer_draw_stream(&g_probe.renderer, theme, true, true, nullptr, 10000))
        return false;
    u32 end = chars;
    directory[end++] = L'/';
    for (u32 i = 0; filename[i]; ++i)
        directory[end++] = filename[i];
    D3D11_TEXTURE2D_DESC desc{};
    ProbeTargetReadback readback;
    if (readback.read_target(g_probe.renderer, desc))
        return false;
    HANDLE file = CreateFileW(directory, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    BITMAPFILEHEADER header{};
    const u32 capture_width = minimum(desc.Width, g_probe.renderer.width);
    const u32 capture_height = minimum(desc.Height, g_probe.renderer.height);
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + capture_width * capture_height * 4;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = capture_width;
    info.biHeight = -static_cast<LONG>(capture_height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    DWORD written = 0;
    bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, &header, sizeof(header), &written, nullptr) &&
              written == sizeof(header) && WriteFile(file, &info, sizeof(info), &written, nullptr) &&
              written == sizeof(info);
    for (u32 y = 0; ok && y < capture_height; ++y)
        ok = WriteFile(file, static_cast<const u8*>(readback.mapped.pData) + y * readback.mapped.RowPitch,
                       capture_width * 4, &written, nullptr) &&
             written == capture_width * 4;
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    return ok;
}

} // namespace ui_probe_detail

namespace {

struct MediaGalleryProbeSource {
    UiMediaPickerItemKind kind = UiMediaPickerItemKind::category;
    u64 first_asset = 0x6f000;
    u32 calls = 0;
    bool favorites = false;
};
bool media_gallery_probe_resolve(void* context, u32 index, UiMediaPickerItem* item) noexcept {
    auto& fixture = *static_cast<MediaGalleryProbeSource*>(context);
    ++fixture.calls;
    const Utf8View categories[]{
        utf8_literal("Favorites"),    utf8_literal("Trending"), utf8_literal("Hello"),
        utf8_literal("Celebrate"),    utf8_literal("Laugh"),    utf8_literal("Thank you"),
        utf8_literal("Good morning"), utf8_literal("Wow"),      utf8_literal("Happy dance"),
        utf8_literal("Thinking"),     utf8_literal("Love"),     utf8_literal("Good night")};
    const bool sticker = fixture.kind == UiMediaPickerItemKind::sticker;
    const bool disabled = (sticker && index == 10) || (fixture.favorites && index == 5);
    const auto key = media_asset_catalog_preview(fixture.first_asset + index, 1,
                                                 fixture.kind == UiMediaPickerItemKind::gif && !index &&
                                                         !fixture.favorites
                                                     ? MediaAttachmentFormat::webp
                                                     : MediaAttachmentFormat::png);
    *item = {fixture.first_asset + index,
             disabled ? 0 : key.id,
             fixture.kind == UiMediaPickerItemKind::category ? categories[index % 12]
             : disabled                                      ? utf8_literal("Vector sticker")
             : sticker                                       ? utf8_literal("Little comet")
                                                             : utf8_literal("Hello, friend"),
             disabled  ? utf8_literal("Vector stickers cannot be sent")
             : sticker ? utf8_literal("Original native fixture pack")
                       : utf8_literal("Original synthetic gallery fixture"),
             key.variant,
             sticker ? 64u : 96u,
             sticker ? 64u : 48u,
             fixture.kind,
             !disabled,
             key.flags};
    if (fixture.favorites) {
        const Utf8View labels[]{utf8_literal("Saved little comet"),   utf8_literal("Favorite this garden"),
                                utf8_literal("Saving favorite"),      utf8_literal("Removal failed"),
                                utf8_literal("Preview not supplied"), utf8_literal("Unsupported preview")};
        const Utf8View details[]{utf8_literal("Confirmed favorite - original offline artwork"),
                                 utf8_literal("Available favorite action - original offline artwork"),
                                 utf8_literal("Pending addition - confirmation has not arrived"),
                                 utf8_literal("Failed removal - confirmed favorite is retained"),
                                 utf8_literal("Neutral source placeholder - no residency claim"),
                                 utf8_literal("Known unavailable format - Send and Favorite disabled")};
        if (index >= 10)
            return false;
        // Records past the sixth are ordinary available favorites.
        item->label = labels[index < 6 ? index : 1];
        item->detail = details[index < 6 ? index : 1];
        item->favoritable = true;
        item->favorite = index == 0 || index == 3;
        item->favorite_pending = index == 2;
        item->favorite_failed = index == 3;
        if (index >= 4)
            item->asset_id = 0;
    }
    return true;
}

} // namespace

namespace ui_probe_detail {

bool probe_ui_surface_motion() noexcept {
    auto& renderer = g_probe.renderer;
    if (!renderer.content_text || !renderer.ui_scene || !synchronize_probe_shell_layout())
        return false;
    UiMediaPickerScene gallery{};
    PopupScene popup{};
    PopupItem items[4]{};
    PopupRowWidget rows[4]{};
    UiWidget widgets[10]{};
    UiLayoutNode nodes[10]{};
    UiSettingsScene settings{};
    UiPageRegistry registry{};
    UiPage page{};
    const auto* old_gallery = renderer.media_picker_scene;
    const auto* old_popup = renderer.popup_scene;
    const auto* old_settings = renderer.settings_scene;
    const auto* old_focus = renderer.focus_owner;
    const auto old_opacity = renderer.layer_opacity;
    struct Restore {
        D3D11Renderer& renderer;
        UiMediaPickerScene& gallery;
        PopupScene& popup;
        UiSettingsScene& settings;
        const UiMediaPickerScene* old_gallery;
        const PopupScene* old_popup;
        const UiSettingsScene* old_settings;
        const UiWidgetManager* old_focus;
        RendererLayerOpacity opacity;
        ~Restore() noexcept {
            renderer.media_picker_scene = old_gallery;
            renderer_bind_popup_scene(&renderer, old_popup);
            renderer_bind_settings_scene(&renderer, old_settings);
            renderer_bind_focus_owner(&renderer, old_focus);
            renderer.layer_opacity = opacity;
            renderer.retained_presentation.valid = false;
            ui_media_picker_scene_release(&gallery);
            popup_scene_release(&popup);
            ui_settings_scene_release(&settings);
        }
    } restore{renderer,  gallery,      popup,     settings,   old_gallery,
              old_popup, old_settings, old_focus, old_opacity};
    renderer.layer_opacity = {1, 1, 1};
    const float scale = static_cast<float>(renderer.dpi) / 96.f;
    const UiRect viewport{0, 0, renderer.width / scale, renderer.height / scale};
    const auto content_viewport = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::content);
    const auto* anchor =
        message_composer_scene_layout_node(g_probe.message_composer_scene, MessageComposerControl::gif);
    if (!anchor || !popup_scene_initialize(&popup, {items, 4}, {rows, 4}, {widgets, 10}, {nodes, 10}))
        return false;
    // Original solid artwork has deliberately distinctive channels. Native
    // media raster pixels can be identified independently of panels and ink.
    u8 pixels[32 * 32 * 4]{};
    for (u32 i = 0; i < 32 * 32; ++i) {
        pixels[i * 4] = 240;
        pixels[i * 4 + 1] = 64;
        pixels[i * 4 + 2] = 180;
        pixels[i * 4 + 3] = 255;
    }
    MediaGalleryProbeSource fixture{UiMediaPickerItemKind::gif, 0xaf000, 0, true};
    UiMediaPickerSource source{&fixture, 29, 1, 4, 1, {}, media_gallery_probe_resolve};
    UiMediaPickerContent content{};
    content.account_scope = source.account_scope;
    content.query_generation = source.query_generation;
    content.revision = 1;
    content.kind = UiMediaPickerKind::gif;
    content.phase = UiMediaPickerPhase::ready;
    content.edit.active = true;
    content.placeholder = utf8_literal("Search original fixtures");
    content.available_tabs = ui_media_picker_tab_bit(UiMediaPickerKind::gif);
    for (u32 i = 0; i < source.count; ++i) {
        UiMediaPickerItem item{};
        if (!source.resolve(source.context, i, &item) ||
            !renderer_upload_media_straight_rgba8(
                 &renderer,
                 {item.asset_id, item.variant, MediaAssetKind::catalog_preview, item.asset_flags}, 32, 32,
                 128, {pixels, sizeof(pixels)})
                 .ok())
            return false;
    }
    if (!ui_media_picker_scene_update(&gallery, &source, &content, content_viewport, anchor->rectangle) ||
        !gallery.cell_count)
        return false;
    renderer.media_picker_scene = &gallery;
    renderer_bind_focus_owner(&renderer, &gallery.widgets);
    const auto cell = gallery.projection->cells[0];
    const float hit_x = (cell.rectangle.left + cell.rectangle.right) * .5f;
    const float hit_y = (cell.rectangle.top + cell.rectangle.bottom) * .5f;
    const auto target = ui_media_picker_scene_hit_test(gallery, hit_x, hit_y);
    if (target.action != UiMediaPickerAction::item || target.source_index != 0)
        return false;
    const u32 calls = fixture.calls, revision = gallery.revision, generation = gallery.generation;
    const auto gallery_guard = [&]() noexcept {
        return renderer.layer_opacity.popup == 1 && renderer.layer_opacity.settings_choice == 1 &&
               fixture.calls == calls && gallery.revision == revision && gallery.generation == generation &&
               ui_media_picker_scene_current(gallery) &&
               ui_rect_equal(cell.rectangle, gallery.projection->cells[0].rectangle) &&
               ui_rect_equal(cell.clip, gallery.projection->cells[0].clip) &&
               ui_media_picker_target_equal(target, ui_media_picker_scene_hit_test(gallery, hit_x, hit_y));
    };
    const wchar_t* gallery_names[]{L"surface-motion-gallery-zero.bmp", L"surface-motion-gallery-half.bmp",
                                   L"surface-motion-gallery-one.bmp"};
    if (!surface_motion_probe_detail::measure(renderer, RendererLayerSurface::gallery, gallery.panel, true,
                                              gallery_names, gallery_guard))
        return false;
    renderer_bind_focus_owner(&renderer, nullptr);
    renderer.retained_presentation.valid = false;
    constexpr u64 emoji_id = 0xaf099;
    if (!renderer_upload_media_straight_rgba8(&renderer, media_asset_custom_emoji(emoji_id, false), 32, 32,
                                              128, {pixels, sizeof(pixels)})
             .ok())
        return false;
    items[0] = {utf8_literal("Original native preview"), 7, popup_item_enabled | popup_item_emoji_preview,
                0, emoji_id};
    items[1] = {utf8_literal("Second native action"), 8, popup_item_enabled};
    const UiRect popup_anchor{content_viewport.left + 80, content_viewport.top + 120,
                              content_viewport.left + 100, content_viewport.top + 140};
    if (!popup_scene_open(&popup, PopupKind::popover, 1, {items, 2}, popup_anchor, viewport, 280, true)
             .ok() ||
        !renderer_bind_popup_scene(&renderer, &popup))
        return false;
    PopupItemLayout row{};
    if (!popup_scene_resolve_item_layout(popup, 0, &row) || !row.visible)
        return false;
    const float popup_x = (row.visible_rectangle.left + row.visible_rectangle.right) * .5f;
    const float popup_y = (row.visible_rectangle.top + row.visible_rectangle.bottom) * .5f;
    const auto popup_hit = popup_scene_hit_test(popup, popup_x, popup_y);
    if (!popup_hit.inside || !popup_hit.enabled || popup_hit.action != 7)
        return false;
    const UiRect popup_rectangle = popup.rectangle;
    const u32 popup_revision = popup.revision;
    const auto popup_guard = [&]() noexcept {
        const auto hit = popup_scene_hit_test(popup, popup_x, popup_y);
        return renderer.layer_opacity.gallery == 1 && renderer.layer_opacity.settings_choice == 1 &&
               fixture.calls == calls && gallery.revision == revision && gallery.generation == generation &&
               popup.revision == popup_revision && ui_rect_equal(popup.rectangle, popup_rectangle) &&
               hit.inside == popup_hit.inside && hit.enabled == popup_hit.enabled &&
               hit.action == popup_hit.action && hit.source_index == popup_hit.source_index;
    };
    const wchar_t* popup_names[]{L"surface-motion-popup-zero.bmp", L"surface-motion-popup-half.bmp",
                                 L"surface-motion-popup-one.bmp"};
    if (!surface_motion_probe_detail::measure(renderer, RendererLayerSurface::popup, popup.rectangle, true,
                                              popup_names, popup_guard))
        return false;
    renderer.media_picker_scene = nullptr;
    ui_media_picker_scene_close(&gallery);
    popup_scene_close(&popup);
    const Utf8View choices[]{utf8_literal("Original option"), utf8_literal("Second option"),
                             utf8_literal("Third option")};
    UiSettingsControl control{1,
                              utf8_literal("Native choice"),
                              utf8_literal("The page remains opaque."),
                              UiSettingsControlKind::choice,
                              true,
                              false,
                              {choices, 3},
                              0,
                              {}};
    UiSettingsPageContent page_content{utf8_literal("Surface motion fixture"),
                                       utf8_literal("Original offline native page and choice popup."),
                                       {&control, 1},
                                       1};
    if (!ui_page_registry_initialize(&registry, {&page, 1}) ||
        !ui_page_registry_register(&registry, {{1, 7}, utf8_literal("Native choices"), &page_content})
             .ok() ||
        !ui_settings_scene_update(&settings, &registry, viewport))
        return false;
    renderer_bind_settings_scene(&renderer, &settings);
    UiRect choice_anchor{};
    if (!ui_settings_scene_control_anchor(settings, {UiSettingsTargetKind::control, {1, 7}, 1},
                                          &choice_anchor))
        return false;
    const u32 count = ui_settings_choice_items(control, 0, {items, 4});
    if (!count ||
        !popup_scene_open(&popup, PopupKind::settings_choice, 1, {items, count}, choice_anchor, viewport,
                          240, true)
             .ok() ||
        !popup_scene_resolve_item_layout(popup, 0, &row) || !row.visible)
        return false;
    const float choice_x = (row.visible_rectangle.left + row.visible_rectangle.right) * .5f;
    const float choice_y = (row.visible_rectangle.top + row.visible_rectangle.bottom) * .5f;
    const auto choice_hit = popup_scene_hit_test(popup, choice_x, choice_y);
    if (!choice_hit.inside || !choice_hit.enabled || choice_hit.action != kUiSettingsChoiceActionFirst)
        return false;
    const u32 choice_revision = popup.revision, settings_revision = settings.revision;
    const UiRect choice_rectangle = popup.rectangle;
    const auto choice_guard = [&]() noexcept {
        UiRect current_anchor{};
        const auto hit = popup_scene_hit_test(popup, choice_x, choice_y);
        return renderer.layer_opacity.gallery == 1 && renderer.layer_opacity.popup == 1 &&
               settings.revision == settings_revision && popup.revision == choice_revision &&
               ui_rect_equal(popup.rectangle, choice_rectangle) &&
               ui_settings_scene_control_anchor(settings, {UiSettingsTargetKind::control, {1, 7}, 1},
                                                &current_anchor) &&
               ui_rect_equal(choice_anchor, current_anchor) && hit.inside == choice_hit.inside &&
               hit.enabled == choice_hit.enabled && hit.action == choice_hit.action &&
               hit.source_index == choice_hit.source_index;
    };
    const wchar_t* choice_names[]{L"surface-motion-settings-choice-zero.bmp",
                                  L"surface-motion-settings-choice-half.bmp",
                                  L"surface-motion-settings-choice-one.bmp"};
    return surface_motion_probe_detail::measure(renderer, RendererLayerSurface::settings_choice,
                                                popup.rectangle, false, choice_names, choice_guard);
}

bool probe_media_underlay_and_quick_reactions() noexcept {
    using namespace surface_motion_probe_detail;
    auto& renderer = g_probe.renderer;
    const auto draw_changed = [&]() noexcept {
        const float clear[]{kDefaultTheme.base_lowest.red, kDefaultTheme.base_lowest.green,
                            kDefaultTheme.base_lowest.blue, 1};
        renderer.graphics.context->ClearRenderTargetView(renderer.graphics.target, clear);
        return d3d11_renderer_draw_stream(&renderer, kDefaultTheme, true, true, nullptr, 10000);
    };
    u32 stage = 1;
    struct Failure {
        u32& stage;
        ~Failure() noexcept {
            if (stage)
                log_u64(LogLevel::error, "probe", "media_underlay_failure_stage", stage);
        }
    } failure{stage};
    if (!g_probe.message_scene.count)
        return false;
    const auto first = g_probe.message_scene.rows[0];
    const auto* record = message_store_resolve(g_probe.message_view.store, first.message);
    const auto hash = utf8_literal("0123456789abcdef0123456789abcdef");
    if (!record || !user_store_set_avatar(&g_probe.users, record->author_id, hash))
        return false;
    const auto avatar_key =
        media_asset_user_avatar(record->author_id, user_avatar_variant(hash), false);
    GatewayMessageAttachment attachments[2]{};
    for (u32 i = 0; i < 2; ++i) {
        attachments[i].id = 0xba000 + i;
        attachments[i].filename = i ? utf8_literal("native-poster.mp4") : utf8_literal("native-alpha.png");
        attachments[i].content_type = i ? utf8_literal("video/mp4") : utf8_literal("image/png");
        attachments[i].url =
            i ? utf8_literal("https://cdn.discordapp.com/attachments/111/761857/native-poster.mp4")
              : utf8_literal("https://cdn.discordapp.com/attachments/111/761856/native-alpha.png");
        attachments[i].width = 192;
        attachments[i].height = 64;
        attachments[i].has_width = attachments[i].has_height = true;
    }
    GatewayMessageSticker sticker{};
    sticker.id = 0xba010;
    sticker.name = utf8_literal("Original transparent sticker");
    sticker.format_type = 1;
    GatewayMessageEvent event{};
    event.id = record->id;
    event.fields = gateway_message_field_attachments | gateway_message_field_sticker_items;
    event.attachments = {attachments, 2};
    event.stickers = {&sticker, 1};
    if (!message_rich_store_apply(&g_probe.rich_store, record->id, record->revision, event).ok() ||
        !rebuild_probe_content_text() || !probe_prepare_capture())
        return false;
    stage = 2;
    PaintStream paint{};
    struct ReleasePaint {
        PaintStream& paint;
        ~ReleasePaint() noexcept {
            paint_stream_release(&paint);
        }
    } release_paint{paint};
    MessageMediaCommand commands[8]{};
    MessageMediaCommandStream stream{};
    MessagePresentationResult presentation{};
    if (!paint_stream_initialize(&paint) || !paint_stream_reset(&paint) ||
        !message_media_command_stream_initialize(&stream, {commands, 8}) ||
        !message_presentation_build(g_probe.message_scene, kDefaultTheme, &paint, &stream, &presentation) ||
        stream.count != 3)
        return false;
    const auto& row = g_probe.message_scene.rows[0];
    const auto* avatar = message_scene_layout_node(g_probe.message_scene, row.avatar);
    if (!avatar || row.grouped)
        return false;
    const UiRect avatar_rectangle = avatar->rectangle;
    const auto fallback_alpha = [&](UiRect rectangle) noexcept {
        for (u32 i = 0; i < renderer.paint_stream.count; ++i) {
            const auto& q = renderer.paint_stream.quads[i];
            if (q.border_width == 0 && ui_rect_equal({q.left, q.top, q.right, q.bottom}, rectangle))
                return q.fill.alpha;
        }
        return -1.f;
    };
    if (fallback_alpha(avatar_rectangle) != 1 || renderer.media_quad_count)
        return false;
    for (u32 i = 0; i < stream.count; ++i)
        if (fallback_alpha(commands[i].rectangle) != 1)
            return false;
    Readback readback{};
    if (!readback.initialize(renderer) || !draw_changed() || !readback.read(renderer, 0))
        return false;
    stage = 3;
    // Original straight RGBA bands: absent, half-magenta, solid magenta.
    // The native upload path premultiplies them before the final draw.
    // The MP4 item exercises native poster composition, not an alpha video codec.
    u8 rgba[24 * 24 * 4]{};
    for (u32 y = 0; y < 24; ++y)
        for (u32 x = 0; x < 24; ++x) {
            const u8 alpha = x < 8 ? 0 : x < 16 ? 128 : 255;
            const u32 p = (y * 24 + x) * 4;
            rgba[p] = rgba[p + 2] = 255;
            rgba[p + 3] = alpha;
        }
    MediaAssetKey keys[3]{};
    for (u32 i = 0; i < stream.count; ++i) {
        const auto& command = commands[i];
        keys[i] =
            command.kind == MessageMediaCommandKind::sticker
                ? media_asset_sticker(command.id, command.format)
                : media_asset_attachment(
                      command.id, static_cast<MediaAttachmentFormat>(command.format),
                      media_resolution_tier_for_display(
                          ui_rect_width(command.rectangle), ui_rect_height(command.rectangle), renderer.dpi,
                          media_preview_extent_cap(static_cast<MediaAttachmentFormat>(command.format))));
        if (!renderer_upload_media_straight_rgba8(&renderer, keys[i], 24, 24, 24 * 4, {rgba, sizeof(rgba)})
                 .ok())
            return false;
    }
    if (!renderer_upload_media_straight_rgba8(&renderer, avatar_key, 24, 24, 24 * 4, {rgba, sizeof(rgba)})
             .ok() ||
        !probe_prepare_capture() || renderer.media_quad_count != 4)
        return false;
    if (fallback_alpha(avatar_rectangle) != 0)
        return false;
    for (u32 i = 0; i < stream.count; ++i)
        if (fallback_alpha(commands[i].rectangle) != 0)
            return false;
    if (!draw_changed() || !readback.read(renderer, 1))
        return false;
    // A message sticker: a 160 DIP square at the content's
    // start without a caption line.
    for (u32 i = 0; i < stream.count; ++i)
        if (commands[i].kind == MessageMediaCommandKind::sticker &&
            (ui_rect_width(commands[i].rectangle) != kDefaultMessageStickerLayoutMetrics.extent ||
             ui_rect_height(commands[i].rectangle) != kDefaultMessageStickerLayoutMetrics.extent))
            return false;
    for (u32 i = 0; i < g_probe.message_text.command_count; ++i)
        if (ui_widget_handle_equal(g_probe.message_text.commands[i].owner.handle,
                                   message_scene_rich_item_widget(row, MessageSceneRichItemKind::sticker, 0)))
            return false;
    if (!probe_capture_frame(L"media-underlay-sticker.bmp"))
        return false;
    // Obtain the actual native underlay with the admitted media hidden. No
    // theme/color assumption can stand in for the final composited pixels.
    for (u32 i = 0; i < renderer.media_quad_count; ++i)
        renderer.media_draw.quad_data[i].opacity = 0;
    const float underlay_clear[]{kDefaultTheme.base_lowest.red, kDefaultTheme.base_lowest.green,
                                 kDefaultTheme.base_lowest.blue, 1};
    renderer.graphics.context->ClearRenderTargetView(renderer.graphics.target, underlay_clear);
    if (!d3d11_renderer_draw_stream(&renderer, kDefaultTheme, false, true, nullptr, 10000) ||
        !readback.read(renderer, 2))
        return false;
    for (u32 i = 0; i < renderer.media_quad_count; ++i)
        renderer.media_draw.quad_data[i].opacity = 1;
    stage = 4;
    const UiRect rectangles[]{avatar_rectangle, commands[0].rectangle, commands[1].rectangle,
                              commands[2].rectangle};
    const float scale = renderer.dpi / 96.f;
    for (u32 item = 0; item < 4; ++item) {
        const auto rectangle = rectangles[item];
        const bool poster = item && commands[item - 1].ordinary_video;
        const auto play = poster ? ui_video_play_affordance_rectangle(rectangle, {}) : UiRect{};
        // The real poster retains its translucent Play control. Check media
        // composition in the exposed strip above that foreground control.
        const float sample_y = poster && ui_rect_has_area(play) ? (rectangle.top + play.top) * .5f
                                                                : (rectangle.top + rectangle.bottom) * .5f;
        for (u32 band = 0; band < 3; ++band) {
            const u32 x = static_cast<u32>(
                (rectangle.left + ui_rect_width(rectangle) * (1 + band * 2) / 6.f) * scale);
            const u32 y = static_cast<u32>(sample_y * scale);
            if (poster && ui_rect_contains(play, x / scale, y / scale))
                return false;
            if (x >= readback.width || y >= readback.height)
                return false;
            const u32 p = (y * readback.width + x) * 4;
            const auto* loaded = readback.images + readback.bytes + p;
            const auto* underlay = readback.images + readback.bytes * 2 + p;
            const u32 alpha = band == 0 ? 0 : band == 1 ? 128 : 255;
            for (u32 channel = 0; channel < 3; ++channel) {
                const u32 expected =
                    (channel == 1 ? 0 : alpha) + (underlay[channel] * (255 - alpha) + 127) / 255;
                if (maximum<u32>(loaded[channel], expected) - minimum<u32>(loaded[channel], expected) > 2) {
                    log_u64(LogLevel::error, "probe", "media_underlay_pixel_band", band);
                    log_u64(LogLevel::error, "probe", "media_underlay_pixel_item", item);
                    log_u64(LogLevel::error, "probe", "media_underlay_pixel_channel", channel);
                    log_u64(LogLevel::error, "probe", "media_underlay_pixel_expected", expected);
                    log_u64(LogLevel::error, "probe", "media_underlay_pixel_actual", loaded[channel]);
                    log_u64(LogLevel::error, "probe", "media_underlay_pixel_underlay", underlay[channel]);
                    log_u64(LogLevel::error, "probe", "media_underlay_pixel_width",
                            static_cast<u32>(ui_rect_width(rectangle)));
                    return false;
                }
            }
            if (!band && distance(loaded, readback.images + p) < 6) {
                log_u64(LogLevel::error, "probe", "media_underlay_missing_loaded_distance",
                        distance(loaded, readback.images + p));
                return false;
            }
        }
    }
    stage = 5;
    if (!message_action_bar_open(&g_probe.message_action_bar, g_probe.message_scene,
                                 {row.message_id, row.widget, row.source_index}, *record)
             .ok() ||
        !renderer_bind_message_action_bar(&renderer, &g_probe.message_action_bar) ||
        !renderer_refresh_interaction_text(&renderer, kDefaultTheme) || !probe_prepare_capture() ||
        !draw_changed() || !readback.read(renderer, 3))
        return false;
    for (u32 cell = 0; cell < kMessageActionBarQuickReactionCount; ++cell) {
        const auto clip = raster_clip_rectangle(g_probe.message_action_bar.items[cell].rectangle, scale,
                                                readback.width, readback.height);
        u32 ink = 0;
        const u8 background[]{static_cast<u8>(kDefaultTheme.base_lower.blue * 255.f + .5f),
                              static_cast<u8>(kDefaultTheme.base_lower.green * 255.f + .5f),
                              static_cast<u8>(kDefaultTheme.base_lower.red * 255.f + .5f)};
        for (i32 y = clip.top + 3; y < clip.bottom - 3; ++y)
            for (i32 x = clip.left + 3; x < clip.right - 3; ++x)
                if (distance(readback.images + readback.bytes * 3 + (y * readback.width + x) * 4,
                             background) > 90)
                    ++ink;
        if (ink < 20)
            return false;
    }
    stage = 6;
    const auto retained = snapshot(renderer);
    for (u32 i = 0; i < 3; ++i)
        if (!probe_prepare_capture() || !draw_fixed(renderer) || !same(retained, snapshot(renderer)))
            return false;
    stage = 7;
    // More only replaces the retained interaction suffix. It must not rerun
    // message preparation, change heights, or move the base native glyphs.
    PopupScene menu{};
    PopupItem menu_items[kMessageContextMaximumItems]{};
    PopupRowWidget menu_rows[kMessageContextMaximumItems]{};
    UiWidget menu_widgets[2 + kMessageContextMaximumItems * kPopupWidgetsPerItem]{};
    UiLayoutNode menu_nodes[2 + kMessageContextMaximumItems * kPopupWidgetsPerItem]{};
    const auto* previous_popup = renderer.popup_scene;
    struct ReleaseMenu {
        D3D11Renderer& renderer;
        PopupScene& menu;
        const PopupScene* previous;
        ~ReleaseMenu() noexcept {
            renderer.popup_scene = previous;
            popup_scene_release(&menu);
        }
    } release_menu{renderer, menu, previous_popup};
    MessageContextMenuModel model{};
    const u32 prefix_quads = renderer.content_text_prefix.quads;
    const u64 prefix_hash =
        bytes_hash(1469598103934665603ull, g_probe.text.quads, prefix_quads * sizeof(GlyphQuad));
    const u32 measurements = g_probe.text_measure_cache.backend_calls;
    const u32 preparations = g_probe.message_scene.preparation_passes;
    const u32 layouts = g_probe.message_scene.layout_passes;
    const auto row_rectangle = message_scene_layout_node(g_probe.message_scene, row.widget)->rectangle;
    if (!message_context_menu_build(&model, *record, false, false, false).ok() ||
        !popup_scene_initialize(&menu, {menu_items, kMessageContextMaximumItems},
                                {menu_rows, kMessageContextMaximumItems},
                                {menu_widgets, sizeof(menu_widgets) / sizeof(menu_widgets[0])},
                                {menu_nodes, sizeof(menu_nodes) / sizeof(menu_nodes[0])}) ||
        !popup_scene_open(&menu, PopupKind::context_menu, record->id, {model.items, model.count},
                          g_probe.message_action_bar.items[5].rectangle, g_probe.message_scene.viewport,
                          260)
             .ok() ||
        !renderer_bind_popup_scene(&renderer, &menu))
        return false;
    const auto base_current = [&]() noexcept {
        return renderer.content_text_prefix.quads == prefix_quads &&
               bytes_hash(1469598103934665603ull, g_probe.text.quads, prefix_quads * sizeof(GlyphQuad)) ==
                   prefix_hash &&
               g_probe.text_measure_cache.backend_calls == measurements &&
               g_probe.message_scene.preparation_passes == preparations &&
               g_probe.message_scene.layout_passes == layouts &&
               ui_rect_equal(message_scene_layout_node(g_probe.message_scene, row.widget)->rectangle,
                             row_rectangle);
    };
    for (u32 i = 0; i < 3; ++i) {
        bool changed = false;
        if (!popup_scene_select_index(&menu, i, &changed) ||
            !renderer_rebuild_content_text(&renderer, kDefaultTheme) || !probe_prepare_capture() ||
            !draw_changed() || !base_current())
            return false;
    }
    if (!popup_scene_close(&menu) || !renderer_rebuild_content_text(&renderer, kDefaultTheme) ||
        !probe_prepare_capture() || !draw_changed() || !base_current())
        return false;
    stage = 0;
    log_write(LogLevel::info, "probe",
              "media_underlay_alpha_three_quick_emoji_and_more_retention_native_passed");
    return true;
}

bool probe_media_gallery(bool favorites) noexcept {
    auto& renderer = g_probe.renderer;
    if (!renderer.preview_media_atlas || !renderer.content_text || !renderer.ui_scene)
        return false;
    if (!synchronize_probe_shell_layout() || !g_probe.message_composer_scene.active ||
        renderer.message_composer_scene != &g_probe.message_composer_scene) {
        log_write(LogLevel::error, "probe", "media_gallery_bound_composer_setup_failed");
        return false;
    }
    // Only this selected offline gallery probe uses the larger surface/bank.
    // It retains the prior category/GIF identities plus30 sticker records;
    // ordinary codec fixtures still initialize32 entries in256x256 pixels.
    constexpr u16 gallery_preview_extent = 512;
    if (renderer.preview_media_atlas != &g_probe.preview_media_atlas ||
        renderer.media_gpu.preview_texture || g_probe.preview_media_atlas.entry_count ||
        !media_atlas_initialize(&g_probe.preview_media_atlas, gallery_preview_extent,
                                gallery_preview_extent, 1,
                                {g_probe.preview_media_entries, kProbeGalleryMediaEntryCapacity},
                                {g_probe.preview_media_buckets, kProbeGalleryMediaEntryCapacity * 2},
                                {g_probe.preview_media_allocations, kProbeGalleryMediaEntryCapacity},
                                {g_probe.preview_media_rows, kProbeMediaRowCapacity})) {
        log_write(LogLevel::error, "probe", "media_gallery_preview_fixture_storage_not_fresh");
        return false;
    }
    UiMediaPickerScene gallery{};
    MediaGalleryProbeSource fixture{};
    fixture.favorites = favorites;
    if (favorites) {
        fixture.kind = UiMediaPickerItemKind::gif;
        fixture.first_asset = 0x9f000;
    }
    UiMediaPickerSource source{&fixture, 17, 1, 12, 1, {}, media_gallery_probe_resolve};
    // Enough favorites to scroll a partly clipped star into the taller GIF body.
    if (favorites)
        source.count = 10;
    UiMediaPickerContent content{};
    content.account_scope = source.account_scope;
    content.query_generation = source.query_generation;
    content.revision = 1;
    content.kind = UiMediaPickerKind::gif;
    content.phase = UiMediaPickerPhase::ready;
    content.edit.active = true;
    content.placeholder = utf8_literal("Search GIFs");
    if (favorites) {
        content.placeholder = utf8_literal("Search favorites");
        content.status = utf8_literal("Favorites - original offline fixtures");
    }
    content.available_tabs = ui_media_picker_tab_bit(UiMediaPickerKind::gif) |
                             ui_media_picker_tab_bit(UiMediaPickerKind::sticker) |
                             ui_media_picker_tab_bit(UiMediaPickerKind::emoji);
    const auto* previous_gallery = renderer.media_picker_scene;
    const auto* previous_focus = renderer.focus_owner;
    const u32 original_width = renderer.width, original_height = renderer.height,
              original_dpi = renderer.dpi;
    const auto original_content = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::content);
    const float minimum_width_dips = original_content.left + app_detail::kMinimumAccountMessageWidthDips;
    u32 stage = 0, failure = 0;
    bool layers = false, resident = false, animated = false, missing = false, stale = false;
    bool outline = false, search = false, disabled = false, bounded = false, narrow = false;
    const auto require = [&](bool value, u32 checkpoint) noexcept {
        if (!value && !failure)
            failure = checkpoint;
        return value;
    };
    const auto update = [&]() noexcept {
        auto viewport = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::content);
        viewport.right = static_cast<float>(renderer.width) * 96.f / maximum(renderer.dpi, 1u);
        const auto* anchor = message_composer_scene_layout_node(g_probe.message_composer_scene,
                                                                content.kind == UiMediaPickerKind::gif
                                                                    ? MessageComposerControl::gif
                                                                    : MessageComposerControl::sticker);
        bool anchored = anchor && ui_rect_valid(anchor->rectangle) &&
                        ui_widget_manager_input_enabled(g_probe.message_composer_scene.widgets,
                                                        content.kind == UiMediaPickerKind::gif
                                                            ? g_probe.message_composer_scene.gif
                                                            : g_probe.message_composer_scene.sticker);
        if (!anchored) {
            // A compact composer folds the GIF and sticker buttons into the emoji button.
            anchor = message_composer_scene_layout_node(g_probe.message_composer_scene,
                                                        MessageComposerControl::emoji);
            anchored = anchor && ui_rect_valid(anchor->rectangle) &&
                       ui_widget_manager_input_enabled(g_probe.message_composer_scene.widgets,
                                                       g_probe.message_composer_scene.emoji);
        }
        if (!anchored) {
            log_write(LogLevel::error, "probe", "media_gallery_native_anchor_unavailable");
            return false;
        }
        const bool accepted =
            ui_media_picker_scene_update(&gallery, &source, &content, viewport, anchor->rectangle);
        if (!accepted) {
            log_u64(LogLevel::error, "probe", "media_gallery_edit_active", content.edit.active);
            log_u64(LogLevel::error, "probe", "media_gallery_available_width_dips",
                    static_cast<u64>(ui_rect_width(viewport)));
            log_u64(LogLevel::error, "probe", "media_gallery_available_height_dips",
                    static_cast<u64>(ui_rect_height(viewport)));
        }
        return accepted;
    };
    const auto show = [&](const wchar_t* name, bool hover = false) noexcept {
        ++stage;
        if (!require(update(), stage * 10 + 1))
            return false;
        if (hover && gallery.cell_count) {
            const auto r = gallery.projection->cells[0].rectangle;
            ui_media_picker_scene_hover(&gallery, (r.left + r.right) * .5f, (r.top + r.bottom) * .5f, 1000);
        }
        renderer.media_picker_scene = &gallery;
        renderer_bind_focus_owner(&renderer, &gallery.widgets);
        renderer.retained_presentation.valid = false;
        if (!require(renderer_rebuild_content_text(&renderer, kDefaultTheme), stage * 10 + 2) ||
            !require(probe_present(&renderer, kDefaultTheme), stage * 10 + 3))
            return false;
        layers = renderer.overlay_paint_first_batch <= renderer.media_picker_paint_first_batch &&
                 renderer.media_picker_paint_first_batch <= renderer.media_picker_foreground_first_batch &&
                 renderer.media_picker_foreground_first_batch <= renderer.media_picker_paint_end_batch &&
                 renderer.media_picker_paint_end_batch <= renderer.menu_paint_first_batch &&
                 renderer.media_picker_media_first_batch <= renderer.media_picker_media_end_batch &&
                 renderer.media_picker_media_end_batch <= renderer.popup_media_first_batch &&
                 renderer.overlay_text_first_batch <= renderer.media_picker_text_first_batch &&
                 renderer.media_picker_text_first_batch < renderer.media_picker_text_end_batch &&
                 renderer.media_picker_text_end_batch <= renderer.menu_text_first_batch;
        bool catalog_only = true;
        for (u32 b = renderer.media_picker_media_first_batch; b < renderer.media_picker_media_end_batch;
             ++b) {
            const auto& batch = renderer.media_draw.batch_data[b];
            for (u32 q = batch.first_quad; q < batch.first_quad + batch.quad_count; ++q) {
                const auto& quad = renderer.media_draw.quad_data[q];
                catalog_only = catalog_only && renderer.media_draw.domain_data[q] == 1 &&
                               quad.clip_radius == kUiControlCornerRadius &&
                               ui_rect_has_area(quad.rounded_clip) && quad.v0 < 0 && quad.v1 < 0;
            }
        }
        return require(layers && catalog_only && d3d11_renderer_interaction_text_state_current(renderer) &&
                           gallery.cell_count <= kUiMediaPickerVisibleCells &&
                           renderer.media_picker_layout.active,
                       stage * 10 + 4) &&
               require(probe_capture_frame(name), stage * 10 + 5);
    };
    const auto upload_art = [&](u32 count, bool sticker) noexcept {
        // The source phase has changed before this synchronous preload. The
        // previous view releases its visibility pins before the next bank's
        // bounded fixture records are uploaded. No draw runs between phases.
        media_atlas_begin_visibility(renderer.preview_media_atlas);
        for (u32 i = 0; i < count; ++i) {
            // Keep one deliberate missing thumbnail. No file, URL or account is used.
            if ((!favorites && i == 3) || (sticker && i == 10))
                continue;
            UiMediaPickerItem item{};
            if (!source.resolve(source.context, i, &item))
                return false;
            if (!item.enabled || !item.asset_id ||
                (fixture.kind == UiMediaPickerItemKind::gif && !i && !favorites))
                continue;
            for (u32 y = 0; y < 64; ++y)
                for (u32 x = 0; x < 64; ++x) {
                    auto* p = g_probe.png_scanlines + (y * 64 + x) * 4;
                    const i32 dx = static_cast<i32>(x) - 32, dy = static_cast<i32>(y) - 32;
                    const bool ink = !sticker || dx * dx + dy * dy < 760;
                    const bool eye =
                        sticker && y >= 23 && y <= 28 && ((x >= 20 && x <= 25) || (x >= 40 && x <= 45));
                    p[0] = eye ? 32 : static_cast<u8>(80 + (i * 31 + x * 2) % 150);
                    p[1] = eye ? 32 : static_cast<u8>(100 + (i * 53 + y) % 130);
                    p[2] = eye ? 32 : static_cast<u8>(130 + (i * 17 + x + y) % 110);
                    p[3] = ink ? 255 : 0;
                }
            const MediaAssetKey key{item.asset_id, item.variant, MediaAssetKind::catalog_preview,
                                    item.asset_flags};
            const auto uploaded = renderer_upload_media_straight_rgba8(
                &renderer, key, 64, 64, 256, {g_probe.png_scanlines, 64 * 64 * 4});
            if (!uploaded.ok()) {
                log_u64(LogLevel::error, "probe", "media_gallery_upload_error",
                        static_cast<u8>(uploaded.error));
                log_u64(LogLevel::error, "probe", "media_gallery_upload_native_error",
                        static_cast<u32>(uploaded.native_error));
                log_u64(LogLevel::error, "probe", "media_gallery_upload_allocation_error",
                        static_cast<u8>(uploaded.allocation.error));
                log_u64(LogLevel::error, "probe", "media_gallery_upload_key_valid",
                        media_asset_key_valid(key));
                log_u64(LogLevel::error, "probe", "media_gallery_upload_flags", key.flags);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_kind",
                        media_asset_large_preview(key));
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_tier",
                        static_cast<u8>(media_asset_resolution_tier(key)));
                log_u64(LogLevel::error, "probe", "media_gallery_upload_dimensions", 64);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_payload_bytes", 64 * 64 * 4);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_primary_atlas_width",
                        renderer.media_atlas ? renderer.media_atlas->allocations.width : 0);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_atlas_width",
                        renderer.preview_media_atlas->allocations.width);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_atlas_height",
                        renderer.preview_media_atlas->allocations.height);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_padding",
                        renderer.preview_media_atlas->allocations.padding);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_entries",
                        renderer.preview_media_atlas->entry_count);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_capacity",
                        renderer.preview_media_atlas->entry_capacity);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_gpu_width",
                        renderer.media_gpu.preview_width);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_gpu_ready",
                        renderer.media_gpu.preview_texture && renderer.media_gpu.preview_target &&
                            renderer.media_gpu.preview_texture_uav);
                return false;
            }
        }
        return true;
    };
    if (favorites) {
        bool confirmed_hover = false, keyboard_favorite = false, pending_no_send = false;
        bool failed_confirmed = false, neutral_source = false, disabled_source = false;
        bool clipped_star = false, stale_favorite = false, scene_bounded = false;
        bool valid = require(upload_art(10, false) && update() && gallery.cell_count >= 6, 101);
        UiRect button{}, full_button{};
        if (valid) {
            const auto r = gallery.projection->cells[0].rectangle;
            ui_media_picker_scene_hover(&gallery, (r.left + r.right) * .5f, (r.top + r.bottom) * .5f, 1000);
            valid = require(ui_media_picker_scene_favorite(gallery, 0, &button, &full_button), 102);
        }
        if (valid) {
            const float x = (button.left + button.right) * .5f, y = (button.top + button.bottom) * .5f;
            ui_media_picker_scene_hover(&gallery, x, y, 1000);
            confirmed_hover =
                gallery.projection->cells[0].item.favorite &&
                !gallery.projection->cells[0].item.favorite_pending &&
                ui_media_picker_scene_hit_test(gallery, x, y).action == UiMediaPickerAction::favorite;
            valid = require(confirmed_hover, 103) && show(L"media-favorites-20261003-hover-confirmed.bmp");
            const ProbePixelSample sample{{x, y}, kDefaultTheme.accent, 35};
            valid = valid && require(probe_exact_pixels({&sample, 1}), 104);
        }
        if (valid) {
            ui_media_picker_scene_hover(&gallery, gallery.panel.left - 1, gallery.panel.top - 1, 1100);
            valid = require(ui_media_picker_scene_select(&gallery, 0), 105);
            keyboard_favorite =
                valid &&
                ui_media_picker_scene_selected_favorite_target(gallery).action == UiMediaPickerAction::favorite;
            valid = valid && require(keyboard_favorite, 106) &&
                    show(L"media-favorites-20261003-keyboard-focus.bmp");
        }
        if (valid) {
            valid = require(ui_media_picker_scene_select(&gallery, 2) &&
                                ui_media_picker_scene_favorite(gallery, 2, &button),
                            107);
            if (valid) {
                const auto& cell = gallery.projection->cells[2];
                pending_no_send =
                    cell.item.favorite_pending && !cell.item.favorite &&
                    ui_media_picker_scene_selected_favorite_target(gallery).action ==
                        UiMediaPickerAction::none &&
                    ui_media_picker_scene_hit_test(gallery, (button.left + button.right) * .5f,
                                                   (button.top + button.bottom) * .5f)
                            .action == UiMediaPickerAction::none &&
                    ui_media_picker_scene_hit_test(gallery, cell.rectangle.left + 12,
                                                   cell.rectangle.top + 12)
                            .action == UiMediaPickerAction::item;
                valid = require(pending_no_send, 108) && show(L"media-favorites-20261003-pending.bmp");
            }
        }
        if (valid) {
            valid = require(ui_media_picker_scene_select(&gallery, 3) &&
                                ui_media_picker_scene_favorite(gallery, 3, &button),
                            109);
            if (valid) {
                failed_confirmed = gallery.projection->cells[3].item.favorite &&
                                   gallery.projection->cells[3].item.favorite_failed &&
                                   ui_media_picker_scene_selected_favorite_target(gallery).action ==
                                       UiMediaPickerAction::favorite;
                valid = require(failed_confirmed, 110) &&
                        show(L"media-favorites-20261003-failed-confirmed.bmp");
                const ProbePixelSample sample{
                    {(button.left + button.right) * .5f, (button.top + button.bottom) * .5f},
                    kDefaultTheme.accent,
                    35};
                valid = valid && require(probe_exact_pixels({&sample, 1}), 111);
            }
        }
        if (valid) {
            valid = require(ui_media_picker_scene_select(&gallery, 4), 112);
            UiMediaPickerMediaSlot slot{};
            neutral_source = valid && gallery.projection->cells[4].item.enabled &&
                             !gallery.projection->cells[4].item.asset_id &&
                             !ui_media_picker_scene_media(gallery, 4, &slot);
            const auto& unavailable = gallery.projection->cells[5];
            disabled_source =
                !unavailable.item.enabled && !unavailable.item.asset_id &&
                !ui_media_picker_scene_media(gallery, 5, &slot) &&
                !ui_media_picker_scene_favorite(gallery, 5, &button) &&
                ui_media_picker_scene_hit_test(gallery, unavailable.clip.left + 1, unavailable.clip.top + 1)
                        .action == UiMediaPickerAction::none;
            valid = valid && require(neutral_source && disabled_source, 113) &&
                    show(L"media-favorites-20261003-missing-preview.bmp");
        }
        if (valid) {
            valid = require(ui_media_picker_scene_select(&gallery, 0) &&
                                ui_media_picker_scene_scroll(
                                    &gallery, gallery.style.metrics.content_inset +
                                                  maximum(4.f, gallery.style.metrics.gap) + 10.f) &&
                                ui_media_picker_scene_favorite(gallery, 0, &button, &full_button),
                            114);
            if (valid) {
                const auto& cell = gallery.projection->cells[0];
                clipped_star =
                    ui_rect_equal(button, ui_rect_intersection(full_button, cell.clip)) &&
                    full_button.top < button.top && ui_rect_height(button) < ui_rect_height(full_button) &&
                    ui_media_picker_scene_hit_test(gallery, (button.left + button.right) * .5f,
                                                   (button.top + button.bottom) * .5f)
                            .action == UiMediaPickerAction::favorite &&
                    ui_media_picker_scene_hit_test(gallery, (full_button.left + full_button.right) * .5f,
                                                   button.top - .5f)
                            .action != UiMediaPickerAction::favorite;
                valid = require(clipped_star, 115) && show(L"media-favorites-20261003-partial-clip.bmp");
            }
        }
        if (valid) {
            const float x = (button.left + button.right) * .5f, y = (button.top + button.bottom) * .5f;
            valid = require(ui_media_picker_scene_pointer_down(&gallery, x, y), 116);
            ++source.revision;
            stale_favorite =
                !ui_media_picker_scene_favorite(gallery, 0, &button) &&
                ui_media_picker_scene_selected_favorite_target(gallery).action ==
                    UiMediaPickerAction::none &&
                ui_media_picker_scene_pointer_up(&gallery, x, y).action == UiMediaPickerAction::none;
            // The picker's storage is fixed-capacity (kUiMediaPickerWidgetCapacity), not per item.
            scene_bounded = gallery.cell_count <= 48 && gallery.widgets.count <= 96 &&
                            gallery.storage.arena.committed <= 128 * 1024;
            valid = valid && require(stale_favorite && scene_bounded, 117);
        }
        renderer.media_picker_scene = previous_gallery;
        renderer_bind_focus_owner(&renderer, previous_focus);
        renderer.retained_presentation.valid = false;
        ui_media_picker_scene_release(&gallery);
        valid = valid && layers && confirmed_hover && keyboard_favorite && pending_no_send &&
                failed_confirmed && neutral_source && disabled_source && clipped_star && stale_favorite &&
                scene_bounded;
        log_u64(valid ? LogLevel::info : LogLevel::error, "probe", "media_favorites_native_failure_stage",
                failure);
        return valid;
    }
    bool valid = require(show(L"media-gallery-20261003-gif-categories-missing.bmp"), 1);
    missing = valid && renderer.media_picker_media_first_batch == renderer.media_picker_media_end_batch;
    valid = valid && require(upload_art(12, false), 2) &&
            show(L"media-gallery-20261003-gif-categories.bmp", true);
    resident = valid && renderer.media_picker_media_first_batch < renderer.media_picker_media_end_batch;
    if (valid) {
        const auto& cell = gallery.projection->cells[0];
        const auto click =
            ui_media_picker_scene_hit_test(gallery, cell.rectangle.left + 12, cell.rectangle.top + 12);
        const auto stamp = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
        ui_media_picker_scene_pointer_down(&gallery, cell.rectangle.left + 12, cell.rectangle.top + 12);
        ++source.query_generation;
        ++content.query_generation;
        ++source.revision;
        ++content.revision;
        UiMediaPickerMediaSlot slot{};
        stale =
            click.action == UiMediaPickerAction::category && !ui_media_picker_scene_current(gallery) &&
            !ui_media_picker_scene_media(gallery, 0, &slot) &&
            !d3d11_renderer_interaction_text_state_current(renderer) &&
            !renderer_presentation_stamp_equal(
                stamp, renderer_capture_presentation_stamp(renderer, kDefaultTheme)) &&
            ui_media_picker_scene_pointer_up(&gallery, cell.rectangle.left + 12, cell.rectangle.top + 12)
                    .action == UiMediaPickerAction::none;
        valid = require(stale, 3);
    }
    fixture.kind = UiMediaPickerItemKind::gif;
    fixture.first_asset = 0x7f000;
    source.count = 40;
    content.edit = {utf8_literal("hello"), 0, 5, true, true, false};
    content.back = true;
    ++content.revision;
    valid = valid && require(upload_art(10, false), 4);
    AnimatedWebpFixtureResult encoded{};
    AnimatedWebpImage animation{};
    AnimatedWebpFrame previous{};
    if (valid)
        valid = require(build_lossless_animated_webp_fixture(
                            {g_probe.png_encoded, sizeof(g_probe.png_encoded)}, &encoded) &&
                            animated_webp_parse({g_probe.png_encoded, encoded.encoded_bytes}, &animation,
                                                &g_probe.animated_webp_workspace)
                                .ok(),
                        5);
    for (u32 frame = 0; valid && frame < 2; ++frame) {
        const auto decoded = animated_webp_decode_frame_rgba8(
            {g_probe.png_encoded, encoded.encoded_bytes}, frame, &animation,
            &g_probe.animated_webp_workspace, &g_probe.vp8l_workspace,
            {g_probe.png_scanlines, sizeof(g_probe.png_scanlines)});
        const auto key =
            media_asset_catalog_preview(fixture.first_asset, 1, MediaAttachmentFormat::webp, true);
        if (!require(decoded.ok(), 6 + frame)) {
            valid = false;
            break;
        }
        const auto uploaded =
            frame == 0
                ? renderer_begin_animated_webp(&renderer, key, animation.width, animation.height,
                                               animation.background_rgba, decoded.frame,
                                               {g_probe.png_scanlines, decoded.frame_decode.rgba_bytes})
                : renderer_advance_animated_webp(&renderer, key, animation.background_rgba, previous,
                                                 decoded.frame,
                                                 {g_probe.png_scanlines, decoded.frame_decode.rgba_bytes});
        valid = require(uploaded.ok() && animated_webp_atlas_pixels_exact(uploaded.allocation, frame, true),
                        6 + frame) &&
                show(frame ? L"media-gallery-20261003-gif-search-frame-1.bmp"
                           : L"media-gallery-20261003-gif-search-frame-0.bmp",
                     true);
        previous = decoded.frame;
    }
    animated = valid && previous.duration_ms == 0 && animation.frame_count >= 2;
    search = valid && renderer.media_picker_edit_geometry.selection_count > 0;
    if (valid) {
        content.edit.focused = false;
        ++content.revision;
        valid = require(update() && ui_media_picker_scene_select(&gallery, 0), 8) &&
                show(L"media-gallery-20261003-gif-keyboard-selected.bmp");
    }
    if (valid) {
        const auto r = gallery.projection->cells[0].rectangle;
        const ProbePixelSample sample{{r.left + .5f, (r.top + r.bottom) * .5f}, kDefaultTheme.accent, 35};
        outline = probe_exact_pixels({&sample, 1});
        valid = require(search && outline, 8);
        const u32 calls = fixture.calls;
        valid = valid &&
                require(ui_media_picker_scene_scroll(&gallery, 600) &&
                            fixture.calls - calls <= kUiMediaPickerVisibleCells,
                        9) &&
                show(L"media-gallery-20261003-gif-search-scroll.bmp");
        bounded = valid && fixture.calls - calls <= 2 * kUiMediaPickerVisibleCells;
    }
    fixture.kind = UiMediaPickerItemKind::sticker;
    fixture.first_asset = 0x8f000;
    source.count = 32;
    UiMediaPickerSection sections[]{{31, 0, 12, utf8_literal("Original comet pack"), true},
                                    {32, 12, 20, utf8_literal("Native garden pack"), true}};
    source.sections = {sections, 2};
    ++source.query_generation;
    ++source.revision;
    content.query_generation = source.query_generation;
    content.kind = UiMediaPickerKind::sticker;
    content.placeholder = utf8_literal("Search stickers");
    content.edit = {};
    content.edit.active = true;
    content.back = false;
    ++content.revision;
    valid = valid && require(upload_art(32, true), 10) &&
            show(L"media-gallery-20261003-sticker-sections.bmp", true);
    if (valid) {
        const auto cells = ui_media_picker_scene_cells(gallery);
        for (u32 i = 0; i < cells.count; ++i)
            if (cells[i].source_index == 10) {
                const auto& cell = cells[i];
                disabled = !cell.item.enabled && !cell.item.asset_id &&
                           ui_media_picker_scene_hit_test(gallery, cell.rectangle.left + 12,
                                                          cell.rectangle.top + 12)
                                   .action == UiMediaPickerAction::none;
            }
        valid = require(disabled, 11);
    }
    content.edit = {utf8_literal("comet"), 5, 5, true, true, false};
    source.sections = {};
    source.count = 16;
    ++source.revision;
    ++content.revision;
    valid = valid && show(L"media-gallery-20261003-sticker-search.bmp", true);
    renderer.media_picker_scene = nullptr;
    renderer_set_dpi(&renderer, 120);
    valid = valid &&
            require(renderer_resize(&renderer, 1125, 1250) && synchronize_probe_shell_layout(), 12) &&
            show(L"media-gallery-20261003-sticker-tall-125.bmp", true);
    renderer.media_picker_scene = nullptr;
    valid = valid &&
            require(renderer_resize(&renderer, static_cast<u32>(minimum_width_dips * 1.25f + .999f), 680) &&
                        synchronize_probe_shell_layout({}, {}, 0, 0, false),
                    13) &&
            show(L"media-gallery-20261003-sticker-narrow-125.bmp", true);
    narrow = valid && gallery.panel.left >= gallery.viewport.left &&
             ui_rect_width(gallery.panel) < kUiMediaPickerWidth &&
             gallery.panel.right <= static_cast<float>(renderer.width) * 96.f / renderer.dpi &&
             gallery.panel.top >= gallery.viewport.top && gallery.panel.bottom <= gallery.viewport.bottom;
    renderer.media_picker_scene = nullptr;
    renderer_set_dpi(&renderer, original_dpi);
    const bool restored =
        renderer_resize(&renderer, original_width, original_height) && synchronize_probe_shell_layout();
    renderer.media_picker_scene = previous_gallery;
    renderer_bind_focus_owner(&renderer, previous_focus);
    renderer.retained_presentation.valid = false;
    ui_media_picker_scene_release(&gallery);
    valid = valid && restored && layers && resident && animated && missing && stale && outline && search &&
            disabled && bounded && narrow;
    log_u64(valid ? LogLevel::info : LogLevel::error, "probe", "media_gallery_native_failure_stage",
            failure);
    log_u64(LogLevel::info, "probe", "media_gallery_visible_source_resolutions", fixture.calls);
    return valid;
}

} // namespace ui_probe_detail

namespace {

// The chat selection and renderer size a Friends probe starts from.
struct ProbeSelectionSnapshot {
    u32 width = g_probe.renderer.width, height = g_probe.renderer.height;
    u32 selection_revision = g_probe.chat_state.selection_revision,
        navigation_intent_revision = g_probe.chat_state.navigation_intent_revision;
    u64 guild = g_probe.chat_state.selected_guild_id, channel = g_probe.chat_state.selected_channel_id,
        last_private = g_probe.chat_state.last_private_channel_id,
        restore_channel = g_probe.chat_state.restore_channel_id,
        restore_guild = g_probe.chat_state.restore_guild_id,
        restore_deadline = g_probe.chat_state.restore_deadline_ms;
    bool home = g_probe.chat_state.home_selected;
    // Reinstates the selection, then redraws the shell at the saved size.
    bool restore() const noexcept {
        g_probe.chat_state.home_selected = home;
        g_probe.chat_state.selected_guild_id = guild;
        g_probe.chat_state.selected_channel_id = channel;
        g_probe.chat_state.selection_revision = selection_revision;
        g_probe.chat_state.navigation_intent_revision = navigation_intent_revision;
        g_probe.chat_state.last_private_channel_id = last_private;
        g_probe.chat_state.restore_channel_id = restore_channel;
        g_probe.chat_state.restore_guild_id = restore_guild;
        g_probe.chat_state.restore_deadline_ms = restore_deadline;
        return renderer_resize(&g_probe.renderer, width, height) &&
               navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok() &&
               synchronize_probe_shell_layout() && rebuild_probe_content_text() &&
               probe_present(&g_probe.renderer, kDefaultTheme);
    }
};

// Selects Home without a channel so the shell shows Friends.
bool probe_select_friends_home() noexcept {
    if (!chat_state_store_select_home(&g_probe.chat_state).ok())
        return false;
    g_probe.chat_state.selected_channel_id = 0;
    return navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok() &&
           synchronize_probe_shell_layout();
}

} // namespace

namespace ui_probe_detail {

bool probe_friends_scene() noexcept {
    g_probe.failure_stage = 270;
    UiFriendsScene friends{};
    TextFormScene form{};
    TextFormSurface surface{};
    UiSettingsScene settings{};
    struct RestoreFriendsProbe {
        UiFriendsScene& friends;
        TextFormScene& form;
        TextFormSurface& surface;
        UiSettingsScene& settings;
        const UiFriendsScene* previous_friends;
        TextFormSurface* previous_form;
        const UiSettingsScene* previous_settings;
        ProbeSelectionSnapshot saved;
        bool restored = false;
        bool restore() noexcept {
            if (restored)
                return true;
            restored = true;
            renderer_bind_friends_scene(&g_probe.renderer, previous_friends);
            renderer_bind_text_form(&g_probe.renderer, previous_form);
            renderer_bind_settings_scene(&g_probe.renderer, previous_settings);
            ui_friends_scene_release(&friends);
            text_form_surface_release(&surface);
            text_form_release(&form);
            ui_settings_scene_release(&settings);
            return saved.restore();
        }
        ~RestoreFriendsProbe() noexcept {
            if (!restored)
                restore();
        }
    } restore{friends, form, surface, settings, g_probe.renderer.friends_scene, g_probe.renderer.text_form,
              g_probe.renderer.settings_scene};
    if (!probe_select_friends_home())
        return false;
    struct Fixture {
        UiFriendsTab tab;
        u32 count, calls;
        bool peer_avatar = false;
    };
    Fixture fixture{UiFriendsTab::online, 2, 0};
    const auto resolve = +[](void* opaque, u32 index, UiFriendsRow* out) noexcept {
        auto& f = *static_cast<Fixture*>(opaque);
        ++f.calls;
        if (index >= f.count)
            return false;
        const auto kind = f.tab == UiFriendsTab::pending
                              ? (index & 1 ? UiFriendsKind::outgoing : UiFriendsKind::incoming)
                          : f.tab == UiFriendsTab::blocked ? UiFriendsKind::blocked
                                                           : UiFriendsKind::friend_user;
        *out = {f.peer_avatar && index == 0 ? 123ull : index + 1ull,
                index + 1 == f.count ? utf8_literal("Last native friend") : utf8_literal("Native friend"),
                utf8_literal("native.friend"),
                {},
                f.tab == UiFriendsTab::online || !(index & 1) ? UiPresence::online : UiPresence::offline,
                kind,
                false};
        return true;
    };
    const auto find = +[](void* opaque, u64 id, u32* index) noexcept {
        auto& f = *static_cast<Fixture*>(opaque);
        if (f.peer_avatar) {
            if (id == 123 && f.count) {
                *index = 0;
                return true;
            }
            return false;
        }
        if (!id || id > f.count)
            return false;
        *index = static_cast<u32>(id) - 1;
        return true;
    };
    UiFriendsSource source{&fixture, fixture.count, 1, resolve, find};
    const auto center = []() noexcept {
        const auto* node = ui_scene_node(g_probe.shell_scene, UiSemantic::message_view);
        return node ? node->rectangle : UiRect{};
    };
    // Bind an initialized scene before selecting the production Friends shell.
    if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center()))
        return false;
    renderer_bind_friends_scene(&g_probe.renderer, &friends);
    if (!synchronize_probe_shell_layout() || g_probe.shell_scene.view != UiShellView::friends)
        return false;
    const auto pixels = [](const UiFriendsScene& scene, bool rows, bool last) noexcept {
        UiTextCommand commands[kUiFriendsMaximumTextCommands]{};
        UiTextCommandStream text{};
        UiTextOutput text_output{&text, nullptr};
        if (!ui_text_command_stream_initialize(&text, {commands, kUiFriendsMaximumTextCommands}) ||
            !ui_friends_scene_text_output(scene, kDefaultTheme, &text_output, nullptr))
            return false;
        const Utf8View labels[]{utf8_literal("Friends"), utf8_literal("Pending"),
                                last ? utf8_literal("Last native friend") : utf8_literal("Native friend")};
        UiRect rectangles[5]{};
        const u32 count = rows ? 3 : 2;
        for (u32 n = 0; n < count; ++n) {
            bool found = false;
            for (u32 i = 0; i < text.count; ++i)
                if (commands[i].text.bytes == labels[n].bytes &&
                    bytes_equal(commands[i].text.data, labels[n].data, labels[n].bytes)) {
                    rectangles[n] = ui_rect_intersection(commands[i].rectangle, commands[i].clip);
                    found = ui_rect_has_area(rectangles[n]);
                    if (found)
                        break;
                }
            if (!found)
                return false;
        }
        u32 rectangle_count = count;
        bool chat = false;
        for (u32 i = 0; i < text.count; ++i)
            if (commands[i].text.bytes == 4 && bytes_equal(commands[i].text.data, "Chat", 4))
                return false;
        // Friend rows end in round 36-DIP Message/More icon buttons; the
        // Message glyph must reach the target with visible ink.
        for (u32 slot = 0; !chat && slot < ui_friends_scene_visible_count(scene); ++slot) {
            UiFriendsVisibleRow row{};
            if (!ui_friends_scene_visible_row(scene, slot, &row) || row.row.kind != UiFriendsKind::friend_user)
                continue;
            const float y = (row.rectangle.top + row.rectangle.bottom) * .5f;
            for (float x = row.rectangle.right; !chat && x > row.rectangle.left; x -= 2) {
                UiWidgetHandle widget{};
                UiFriendsTarget target{};
                Utf8View tooltip{};
                const auto* node = ui_friends_scene_icon_action(scene, false, x, y, &widget, &target, &tooltip)
                                       ? ui_widget_manager_layout_node(scene.widgets, widget)
                                       : nullptr;
                if (!node || target.action != UiFriendsAction::open_dm)
                    continue;
                const auto r = node->rectangle;
                if (ui_rect_width(r) != 36 || ui_rect_height(r) != 36)
                    return false;
                if (!ui_rect_equal(ui_rect_intersection(r, node->clip_rectangle), r))
                    continue; // a partly scrolled row; probe a fully visible one
                rectangles[rectangle_count++] = {r.left + 8, r.top + 8, r.right - 8, r.bottom - 8};
                chat = true;
            }
        }
        if (rows && (scene.tab == UiFriendsTab::online || scene.tab == UiFriendsTab::all) && !chat)
            return false;
        rectangles[rectangle_count] =
            ui_scene_content_rectangle(g_probe.shell_scene, UiSemantic::channel_header);
        return ui_rect_has_area(rectangles[rectangle_count]) &&
               probe_text_pixels({rectangles, rectangle_count + 1});
    };
    const wchar_t* captures[]{L"friends-online.bmp", L"friends-all.bmp", L"friends-pending.bmp",
                              L"friends-blocked.bmp", L"friends-add-friend.bmp"};
    for (u32 tab = 0; tab < 5; ++tab) {
        fixture.tab = static_cast<UiFriendsTab>(tab);
        fixture.count = tab == 4 ? 0 : tab == 3 ? 1 : tab == 1 ? 4 : 2;
        source.count = fixture.count;
        ++source.revision;
        if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center()))
            return false;
        renderer_bind_friends_scene(&g_probe.renderer, &friends);
        ui_friends_scene_set_focus_visible(&friends, true);
        if (!ui_friends_scene_focus(&friends, {UiFriendsAction::select_tab, 0, fixture.tab}) ||
            g_probe.shell_scene.view != UiShellView::friends)
            return false;
        if (!renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
            !probe_present(&g_probe.renderer, kDefaultTheme) || g_probe.text.message_geometry_quads ||
            !pixels(friends, fixture.count > 0, fixture.count == 1) || !probe_capture_frame(captures[tab]))
            return false;
    }
    // Active Now: the column renders beside the list on a wide content area.
    g_probe.failure_stage = 2741;
    {
        const UiRect wide = center();
        if (ui_rect_width(wide) >= kUiFriendsActiveNowMinimumList + kUiFriendsActiveNowWidth) {
            const UiFriendsActiveCard cards[2]{
                {utf8_literal("Playing Native Quest"), utf8_literal("Native friend and Last native friend"), {}, {1, 2},
                 2, 2, UiFriendsActiveKind::activity},
                {utf8_literal("Lounge"), utf8_literal("Native friend \xC2\xB7 Probe server"), {}, {1}, 1, 1,
                 UiFriendsActiveKind::voice}};
            const UiFriendsActiveNow active{cards, 2, 1, false};
            fixture.tab = UiFriendsTab::online;
            fixture.count = 2;
            source.count = 2;
            ++source.revision;
            if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, wide, nullptr,
                                         nullptr, &active) ||
                !friends.active_visible)
                return false;
            renderer_bind_friends_scene(&g_probe.renderer, &friends);
            const UiRect column = friends.active_rect;
            const UiRect title{column.left + 16, column.top + 12, column.right - 52, column.top + 44};
            const UiRect card{column.left + 30, column.top + 108, column.right - 40, column.top + 150};
            if (!renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
                !probe_present(&g_probe.renderer, kDefaultTheme) || !probe_text_pixels({&title, 1}) ||
                !probe_text_pixels({&card, 1}) || !probe_capture_frame(L"friends-active-now.bmp") ||
                !ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, wide))
                return false;
        }
    }
    g_probe.failure_stage = 271;
    fixture.tab = UiFriendsTab::all;
    fixture.count = 10000;
    source.count = fixture.count;
    ++source.revision;
    fixture.calls = 0;
    if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center()) ||
        friends.count > 32 || fixture.calls > friends.count + 2 ||
        !ui_friends_scene_focus(&friends, {UiFriendsAction::open_dm, 10000, UiFriendsTab::all}) ||
        friends.scroll_dips < 550000 || !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) || !pixels(friends, true, true) ||
        !probe_capture_frame(L"friends-last-row.bmp"))
        return false;
    const auto generation = friends.widgets.generation;
    const auto source_calls = fixture.calls;
    const auto text_builds = g_probe.renderer.content_text_retention.builds;
    const auto paints = g_probe.renderer.paint_stream_builds;
    if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center()) ||
        fixture.calls != source_calls || friends.widgets.generation != generation ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.content_text_retention.builds != text_builds ||
        !probe_present(&g_probe.renderer, kDefaultTheme) || g_probe.renderer.paint_stream_builds != paints)
        return false;
    // Message/composer state is genuinely hidden; sidebar media stays live.
    {
        const auto before = renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme);
        const auto old_updates = g_probe.message_scene.data_updates,
                   old_composer = g_probe.message_composer_scene.observed_revision;
        const auto old_hover = g_probe.message_scene.widgets.hover_started_ms;
        ++g_probe.message_scene.data_updates;
        ++g_probe.message_scene.widgets.hover_started_ms;
        ++g_probe.message_composer_scene.observed_revision;
        const bool retained =
            renderer_presentation_stamp_equal(
                before, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)) &&
            renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            g_probe.renderer.content_text_retention.builds == text_builds &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            g_probe.renderer.paint_stream_builds == paints;
        g_probe.message_scene.data_updates = old_updates;
        g_probe.message_scene.widgets.hover_started_ms = old_hover;
        g_probe.message_composer_scene.observed_revision = old_composer;
        if (!retained)
            return false;
    }
    // A real, focused native composer overlaps Friends but is hidden. Verify
    // both retained dependency checks and a forced fresh paint omit its outline.
    {
        g_probe.failure_stage = 275;
        MessageComposer hidden_model{};
        MessageComposerScene hidden_scene{};
        char hidden_text[64]{};
        struct RestoreHiddenFocus {
            MessageComposerScene& scene;
            const MessageComposerScene* previous_composer;
            const UiWidgetManager* previous_focus;
            ~RestoreHiddenFocus() noexcept {
                g_probe.renderer.message_composer_scene = previous_composer;
                g_probe.renderer.focus_owner = previous_focus;
                message_composer_scene_release(&scene);
            }
        } restore_hidden{hidden_scene, g_probe.renderer.message_composer_scene,
                         g_probe.renderer.focus_owner};
        if (!message_composer_initialize(&hidden_model, {hidden_text, 64}) ||
            !message_composer_scene_initialize_owned(&hidden_scene))
            return false;
        message_composer_reset(&hidden_model, 42, false);
        const auto area = center();
        const UiRect hidden_view{area.left + 20, area.top + 120, area.left + 300, area.top + 168};
        if (!message_composer_scene_update(&hidden_scene, &hidden_model, hidden_view, true).ok() ||
            !message_composer_scene_focus_edit(&hidden_scene))
            return false;
        UiRect anchor{};
        if (!ui_widget_manager_context_anchor(hidden_scene.widgets, &anchor) || !ui_rect_has_area(anchor))
            return false;
        g_probe.renderer.message_composer_scene = &hidden_scene;
        g_probe.renderer.focus_owner = nullptr;
        if (!probe_present(&g_probe.renderer, kDefaultTheme))
            return false;
        const auto before = renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme);
        const auto geometry = probe_resize_geometry_signature();
        const auto baseline_paints = g_probe.renderer.paint_stream_builds;
        const UiWidgetManager* owners[]{
            &g_probe.message_scene.widgets, &hidden_scene.widgets,
            g_probe.renderer.member_list_scene ? &g_probe.renderer.member_list_scene->widgets : nullptr};
        for (const auto* owner : owners) {
            if (!owner)
                continue;
            g_probe.renderer.focus_owner = owner;
            if (!renderer_presentation_stamp_equal(
                    before, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)) ||
                !probe_present(&g_probe.renderer, kDefaultTheme) ||
                g_probe.renderer.paint_stream_builds != baseline_paints)
                return false;
        }
        g_probe.renderer.focus_owner = &hidden_scene.widgets;
        hidden_scene.widgets.hovered = hidden_scene.widgets.focused;
        if (!renderer_presentation_stamp_equal(
                before, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)) ||
            !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.content_text_retention.builds != text_builds)
            return false;
        g_probe.renderer.retained_presentation.valid = false;
        if (!probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.paint_stream_builds != baseline_paints + 1 ||
            probe_resize_geometry_signature() != geometry || !pixels(friends, true, true) ||
            !probe_capture_frame(L"friends-hidden-focus.bmp"))
            return false;
        // A visible Friends owner remains a dependency, unlike the hidden owners.
        g_probe.renderer.focus_owner = &friends.widgets;
        if (renderer_presentation_stamp_equal(
                before, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)))
            return false;
    }
    g_probe.failure_stage = 272;
    const float scale = static_cast<float>(g_probe.renderer.dpi) / 96.f;
    if (!renderer_resize(&g_probe.renderer, static_cast<u32>((center().left + 320) * scale + .999f),
                         static_cast<u32>(360 * scale + .999f)) ||
        !synchronize_probe_shell_layout() ||
        !ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center()) ||
        !ui_friends_scene_focus(&friends, {UiFriendsAction::open_dm, 10000, UiFriendsTab::all}) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) || !pixels(friends, true, true) ||
        !probe_capture_frame(L"friends-compact.bmp"))
        return false;
    // Restore a roomy center for an actual native editor above the base list.
    if (!renderer_resize(&g_probe.renderer, restore.saved.width, restore.saved.height) ||
        !synchronize_probe_shell_layout() ||
        !ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center()))
        return false;
    g_probe.failure_stage = 273;
    const float dip = 96.f / static_cast<float>(g_probe.renderer.dpi);
    const UiRect viewport{0, 0, g_probe.renderer.width * dip, g_probe.renderer.height * dip};
    const TextFormContent content{utf8_literal("Friend overlay"),
                                  {},
                                  utf8_literal("Exact username"),
                                  {},
                                  utf8_literal("Cancel"),
                                  utf8_literal("Send request"),
                                  utf8_literal("native.friend"),
                                  0,
                                  13,
                                  true,
                                  true};
    if (!text_form_update(&form, content, viewport, &g_probe.text_measure_cache))
        return false;
    surface.scene = &form;
    renderer_bind_text_form(&g_probe.renderer, &surface);
    if (!renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) || !surface.layout.active ||
        g_probe.renderer.form_text_first_batch < g_probe.renderer.overlay_text_first_batch ||
        g_probe.renderer.form_text_first_batch >= g_probe.text.batch_count ||
        !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    UiTextCommand form_commands[32]{};
    UiTextCommandStream form_text{};
    UiTextOutput form_text_output{&form_text, nullptr};
    UiRect title{};
    if (!ui_text_command_stream_initialize(&form_text, {form_commands, 32}) ||
        !text_form_text_output(form, surface.geometry, kDefaultTheme, &form_text_output))
        return false;
    for (u32 i = 0; i < form_text.count; ++i)
        if (form_commands[i].text.bytes == content.title.bytes &&
            bytes_equal(form_commands[i].text.data, content.title.data, content.title.bytes))
            title = ui_rect_intersection(form_commands[i].rectangle, form_commands[i].clip);
    if (!ui_rect_has_area(title) || !probe_text_pixels({&title, 1}) ||
        !probe_capture_frame(L"friends-native-form.bmp"))
        return false;
    renderer_bind_text_form(&g_probe.renderer, restore.previous_form);
    text_form_surface_release(&surface);
    text_form_release(&form);
    g_probe.failure_stage = 274;
    UiPageRegistry registry{};
    UiPage pages[1]{};
    UiSettingsControl control{1,   utf8_literal("Visible setting"), {}, UiSettingsControlKind::toggle, true,
                              true};
    UiSettingsPageContent settings_content{
        utf8_literal("Friends covered by Settings"), {}, {&control, 1}, 1};
    if (!ui_page_registry_initialize(&registry, {pages, 1}) ||
        !ui_page_registry_register(&registry, {{1, 1}, utf8_literal("Appearance"), &settings_content})
             .ok() ||
        !ui_settings_scene_update(&settings, &registry, viewport))
        return false;
    renderer_bind_settings_scene(&g_probe.renderer, &settings);
    if (!renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) || g_probe.renderer.media_quad_count ||
        g_probe.text.message_geometry_quads)
        return false;
    const auto settings_stamp = renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme);
    const auto settings_text = g_probe.renderer.content_text_retention.builds,
               settings_paint = g_probe.renderer.paint_stream_builds;
    const auto old_friends_revision = friends.revision, old_source_revision = source.revision;
    ++friends.revision;
    ++source.revision;
    const bool covered =
        renderer_presentation_stamp_equal(
            settings_stamp, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)) &&
        renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
        g_probe.renderer.content_text_retention.builds == settings_text &&
        probe_present(&g_probe.renderer, kDefaultTheme) &&
        g_probe.renderer.paint_stream_builds == settings_paint;
    friends.revision = old_friends_revision;
    source.revision = old_source_revision;
    if (!covered)
        return false;
    UiTextCommand settings_commands[32]{};
    UiTextCommandStream settings_text_stream{};
    UiTextOutput settings_text_stream_output{&settings_text_stream, nullptr};
    UiRect settings_title{};
    if (!ui_text_command_stream_initialize(&settings_text_stream, {settings_commands, 32}) ||
        !ui_settings_scene_text_output(settings, kDefaultTheme, &settings_text_stream_output))
        return false;
    for (u32 i = 0; i < settings_text_stream.count; ++i)
        if (settings_commands[i].text.data == settings_content.title.data)
            settings_title =
                ui_rect_intersection(settings_commands[i].rectangle, settings_commands[i].clip);
    if (!ui_rect_has_area(settings_title) || !probe_text_pixels({&settings_title, 1}) ||
        !probe_capture_frame(L"friends-settings-cover.bmp"))
        return false;
    renderer_bind_settings_scene(&g_probe.renderer, restore.previous_settings);
    ui_settings_scene_release(&settings);
    // Six bounded additions use immutable borrowed projections for each
    // synchronous capture. No controller, credentials, CDN or dialog is used.
    g_probe.failure_stage = 276;
    fixture.tab = UiFriendsTab::all;
    fixture.count = 4;
    source.count = 4;
    ++source.revision;
    friends.scroll_dips = 0;
    const auto unicode_query = utf8_literal("\xC5\xBD"
                                            "iva \xF0\x9F\x98\x80");
    UiFriendsContent search_content{{unicode_query, 0, unicode_query.bytes, true, true, true}, 4, 1};
    if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center(),
                                 &search_content) ||
        !ui_friends_scene_focus(&friends, {UiFriendsAction::search, 0, fixture.tab}) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !g_probe.renderer.composer_layout.active || g_probe.renderer.composer_layout.caret_offset != 7 ||
        !g_probe.renderer.friends_search_geometry.selection_count ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"friends-search-unicode-selected.bmp"))
        return false;
    const auto search_stamp = renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme);
    search_content.search.caret_visible = false;
    ++search_content.revision;
    if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center(),
                                 &search_content) ||
        renderer_presentation_stamp_equal(
            search_stamp, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)))
        return false;
    const auto long_query = utf8_literal("native.search.with.a.long.single.line.query.native.search.with.a."
                                         "long.single.line.query.\xC5\xBD\xF0\x9F\x98\x80");
    search_content.search = {long_query, long_query.bytes, long_query.bytes, true, true, true};
    ++search_content.revision;
    if (!renderer_resize(&g_probe.renderer, static_cast<u32>((center().left + 320) * scale + .999f),
                         static_cast<u32>(360 * scale + .999f)) ||
        !synchronize_probe_shell_layout() ||
        !ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center(),
                                 &search_content) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.composer_layout.scroll_x <= 0 ||
        !ui_rect_has_area(ui_rect_intersection(g_probe.renderer.friends_search_geometry.caret,
                                               ui_friends_scene_search_geometry(friends).text)) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"friends-search-compact-scroll.bmp"))
        return false;
    if (!renderer_resize(&g_probe.renderer, restore.saved.width, restore.saved.height) ||
        !synchronize_probe_shell_layout())
        return false;
    fixture.count = 0;
    source.count = 0;
    ++source.revision;
    search_content.total_count = 4;
    search_content.search = {utf8_literal("no.match.\xF0\x9F\x98\x80"), 0, 13, true, false, false};
    ++search_content.revision;
    if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center(),
                                 &search_content) ||
        friends.count_heading_bytes != 5 || !bytes_equal(friends.count_heading, "0 / 4", 5) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"friends-search-no-results.bmp"))
        return false;
    fixture.count = 4;
    source.count = 4;
    ++source.revision;
    search_content.search = {{}, 0, 0, true, false, false};
    ++search_content.revision;
    if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center(),
                                 &search_content))
        return false;
    {
        PopupScene menu{};
        PopupItem storage[3]{};
        PopupRowWidget rows[3]{};
        UiWidget widgets[16]{};
        UiLayoutNode nodes[16]{};
        const auto* previous_menu = g_probe.renderer.popup_scene;
        struct RestoreMenu {
            PopupScene& menu;
            const PopupScene* previous;
            ~RestoreMenu() noexcept {
                renderer_bind_popup_scene(&g_probe.renderer, previous);
                popup_scene_release(&menu);
            }
        } restore_menu{menu, previous_menu};
        const PopupItem items[]{{utf8_literal("Profile"), 1, popup_item_enabled},
                                {utf8_literal("Remove friend"), 2,
                                 popup_item_enabled | popup_item_danger | popup_item_separator_before},
                                {utf8_literal("Block"), 3, popup_item_enabled | popup_item_danger}};
        UiFriendsVisibleRow row{};
        if (!ui_friends_scene_visible_row(friends, 0, &row) ||
            !ui_friends_scene_focus(&friends, {UiFriendsAction::more, row.row.user_id, fixture.tab}) ||
            !popup_scene_initialize(&menu, {storage, 3}, {rows, 3}, {widgets, 16}, {nodes, 16}) ||
            !popup_scene_open(
                 &menu, PopupKind::context_menu, row.row.user_id, {items, 3},
                 {row.rectangle.right - 32, row.rectangle.top, row.rectangle.right, row.rectangle.bottom},
                 viewport, 240)
                 .ok() ||
            !renderer_bind_popup_scene(&g_probe.renderer, &menu) ||
            !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
            !probe_present(&g_probe.renderer, kDefaultTheme) ||
            !probe_capture_frame(L"friends-more-menu.bmp"))
            return false;
    }
    TextFormContent confirmation{};
    confirmation.title = utf8_literal("Remove friend");
    confirmation.context = utf8_literal(
        "Remove \xC5\xBD"
        "iva \xF0\x9F\x98\x80 from your friends? This native confirmation keeps the full account name "
        "visible before an action. A long name and explanatory context wrap inside the bounded modal "
        "instead of creating an editor or clipping the choice buttons.");
    confirmation.cancel = utf8_literal("Cancel");
    confirmation.accept = utf8_literal("Remove friend");
    confirmation.can_accept = true;
    confirmation.focus_visible = true;
    confirmation.confirmation_only = true;
    if (!text_form_update(&form, confirmation, viewport, &g_probe.text_measure_cache) ||
        !text_form_focus(&form, TextFormElement::cancel))
        return false;
    surface.scene = &form;
    renderer_bind_text_form(&g_probe.renderer, &surface);
    if (!renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) || surface.layout.active ||
        text_form_focused(form) != TextFormElement::cancel ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"friends-confirmation-cancel.bmp"))
        return false;
    renderer_bind_text_form(&g_probe.renderer, restore.previous_form);
    text_form_surface_release(&surface);
    text_form_release(&form);
    fixture.peer_avatar = true;
    fixture.count = 1;
    source.count = 1;
    ++source.revision;
    friends.scroll_dips = 0;
    if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center()) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    AvatarMedia avatar{};
    bool avatar_quad = false;
    if (!friends_avatar_media_resolve(friends, g_probe.users, 0, &avatar) ||
        !media_atlas_peek(&g_probe.media_atlas, avatar.key).ok())
        return false;
    for (u32 i = 0; i < g_probe.renderer.media_quad_count; ++i) {
        const auto& q = g_probe.renderer.media_draw.quad_data[i];
        if (q.left == avatar.rectangle.left && q.top == avatar.rectangle.top &&
            q.right == avatar.rectangle.right && q.bottom == avatar.rectangle.bottom && q.radius == 16)
            avatar_quad = true;
    }
    if (!avatar_quad || !probe_capture_frame(L"friends-native-avatar-presence.bmp"))
        return false;
    if (!restore.restore())
        return false;
    log_write(LogLevel::info, "probe",
              "friends_tabs_virtualization_retention_compact_native_form_hardware_passed");
    return true;
}

bool probe_friends_requests_scene() noexcept {
    g_probe.failure_stage = 277;
    UiFriendsScene main{}, requests{};
    TextFormScene form{};
    TextFormSurface form_surface{};
    ProfileScene profile{};
    struct Fixture {
        u32 count, calls;
        bool hidden;
        char received[32] = "Received native request", sent[32] = "Sent native request";
        char ignored[32] = "Ignored native request", spam[32] = "Spam native request",
             username[32] = "native.request";
    } main_fixture{8, 0, false}, hidden_fixture{4, 0, true};
    const auto resolve = +[](void* opaque, u32 index, UiFriendsRow* out) noexcept {
        auto& f = *static_cast<Fixture*>(opaque);
        ++f.calls;
        if (!out || index >= f.count)
            return false;
        const bool outgoing = !f.hidden && index >= f.count / 2, ignored = f.hidden && index < 2;
        const char* name = f.hidden ? (ignored ? f.ignored : f.spam) : (outgoing ? f.sent : f.received);
        u32 bytes = 0;
        while (name[bytes])
            ++bytes;
        *out = {f.hidden && index == 0 ? 123ull : (f.hidden ? 50000ull : 40000ull) + index,
                {name, bytes},
                {f.username, 14},
                {},
                UiPresence::online,
                outgoing ? UiFriendsKind::outgoing : UiFriendsKind::incoming,
                false,
                f.hidden && !ignored,
                ignored};
        return true;
    };
    const auto find = +[](void* opaque, u64 id, u32* index) noexcept {
        const auto& f = *static_cast<Fixture*>(opaque);
        if (!index)
            return false;
        if (f.hidden && id == 123 && f.count) {
            *index = 0;
            return true;
        }
        const u64 first = f.hidden ? 50000ull : 40000ull;
        if (id < first || id - first >= f.count || (f.hidden && id == first))
            return false;
        *index = static_cast<u32>(id - first);
        return true;
    };
    UiFriendsSource main_source{&main_fixture, 8, 1, resolve, find},
        hidden_source{&hidden_fixture, 4, 1, resolve, find};
    UiFriendsSection ranges[]{{UiFriendsSectionKind::incoming, 0, 4, 4, false, true},
                              {UiFriendsSectionKind::outgoing, 4, 4, 4, false, false}};
    UiFriendsSections sections{ranges, 2, 1, true, false, 4}, hidden_sections{nullptr, 0, 1, true, true, 4};
    struct Restore {
        UiFriendsScene& main;
        UiFriendsScene& requests;
        TextFormScene& form;
        TextFormSurface& surface;
        ProfileScene& profile;
        const UiFriendsScene* old_main;
        const UiFriendsScene* old_requests;
        TextFormSurface* old_form;
        ProfileScene* old_profile;
        const UiWidgetManager* old_focus;
        ProbeSelectionSnapshot saved;
        bool restored = false;
        bool finish() noexcept {
            if (restored)
                return true;
            restored = true;
            renderer_bind_friends_requests_scene(&g_probe.renderer, old_requests);
            renderer_bind_friends_scene(&g_probe.renderer, old_main);
            renderer_bind_text_form(&g_probe.renderer, old_form);
            g_probe.renderer.profile_scene = old_profile;
            g_probe.renderer.focus_owner = old_focus;
            ui_friends_scene_release(&requests);
            ui_friends_scene_release(&main);
            text_form_surface_release(&surface);
            text_form_release(&form);
            profile_scene_release(&profile);
            return saved.restore();
        }
        ~Restore() noexcept {
            if (!restored)
                finish();
        }
    } restore{main,
              requests,
              form,
              form_surface,
              profile,
              g_probe.renderer.friends_scene,
              g_probe.renderer.friends_requests_scene,
              g_probe.renderer.text_form,
              g_probe.renderer.profile_scene,
              g_probe.renderer.focus_owner};
    const float normal_scale = g_probe.renderer.dpi / 96.f;
    if (!renderer_resize(&g_probe.renderer, static_cast<u32>(1200 * normal_scale + .999f),
                         static_cast<u32>(970 * normal_scale + .999f)))
        return false;
    if (!probe_select_friends_home())
        return false;
    const auto center = []() noexcept {
        const auto* node = ui_scene_node(g_probe.shell_scene, UiSemantic::message_view);
        return node ? node->rectangle : UiRect{};
    };
    const auto viewport = []() noexcept {
        const float dip = 96.f / g_probe.renderer.dpi;
        return UiRect{0, 0, g_probe.renderer.width * dip, g_probe.renderer.height * dip};
    };
    const auto update_main = [&]() noexcept {
        return ui_friends_scene_update(&main, &main_source, UiFriendsTab::pending, UiFriendsPhase::ready,
                                       {}, center(), nullptr, &sections);
    };
    const auto text_rect = [](const UiFriendsScene& scene, Utf8View label, bool prefix = false) noexcept {
        UiTextCommand commands[128]{};
        UiTextCommandStream stream{};
        UiTextOutput stream_output{&stream, nullptr};
        if (!ui_text_command_stream_initialize(&stream, {commands, 128}) ||
            !ui_friends_scene_text_output(scene, kDefaultTheme, &stream_output, nullptr))
            return UiRect{};
        for (u32 i = 0; i < stream.count; ++i)
            if ((prefix ? commands[i].text.bytes >= label.bytes : commands[i].text.bytes == label.bytes) &&
                bytes_equal(commands[i].text.data, label.data, label.bytes)) {
                const auto rect = ui_rect_intersection(commands[i].rectangle, commands[i].clip);
                if (ui_rect_has_area(rect))
                    return rect;
            }
        return UiRect{};
    };
    const auto paint = [&]() noexcept {
        return renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
               probe_present(&g_probe.renderer, kDefaultTheme) &&
               g_probe.renderer.paint_stream.count <= kMaxPaintQuads &&
               g_probe.renderer.paint_stream.clip_count <= kMaxPaintClips &&
               !g_probe.renderer.paint_stream.rejected && !g_probe.text.message_geometry_quads &&
               d3d11_renderer_interaction_text_state_current(g_probe.renderer);
    };
    if (!update_main())
        return false;
    renderer_bind_friends_scene(&g_probe.renderer, &main);
    if (!synchronize_probe_shell_layout() || !update_main() || !paint())
        return false;
    UiRect headings[]{text_rect(main, utf8_literal("Received"), true),
                      text_rect(main, utf8_literal("Sent"), true)};
    if (!probe_text_pixels({headings, 2}) || !probe_capture_frame(L"friends-pending-sections.bmp"))
        return false;
    g_probe.failure_stage = 279;
    ranges[0].collapsed = true;
    ++sections.revision;
    if (!update_main() || ui_friends_scene_visible_count(main) != 4 || !paint() ||
        !probe_capture_frame(L"friends-pending-collapse.bmp"))
        return false;
    // Page a real bounded Pending projection, preserving O(1) source lookup.
    main_fixture.count = 4096;
    main_source.count = 4096;
    ++main_source.revision;
    ranges[0] = {UiFriendsSectionKind::incoming, 0, 2048, 2048, false, true};
    ranges[1] = {UiFriendsSectionKind::outgoing, 2048, 2048, 2048, false, false};
    ++sections.revision;
    if (!update_main() ||
        !ui_friends_scene_focus(&main, {UiFriendsAction::cancel, 44095, UiFriendsTab::pending}) ||
        main.scroll_dips < 200000 || ui_friends_scene_page(&main, true).action == UiFriendsAction::none)
        return false;
    // Stress both actual scene paint producers at the original shared bounds.
    PaintStream budget{};
    struct ReleasePaint {
        PaintStream& value;
        ~ReleasePaint() noexcept {
            paint_stream_release(&value);
        }
    } release_paint{budget};
    const UiRect tall{0, 0, 1600, 8192};
    main.scroll_dips = 0;
    if (!paint_stream_initialize(&budget) ||
        !ui_friends_scene_update(&main, &main_source, UiFriendsTab::pending, UiFriendsPhase::ready, {},
                                 tall, nullptr, &sections) ||
        !ui_friends_scene_update(&requests, &hidden_source, UiFriendsTab::pending, UiFriendsPhase::ready,
                                 {}, tall, nullptr, &hidden_sections) ||
        ui_friends_scene_visible_count(main) < 140 || !paint_stream_reset(&budget) ||
        !ui_friends_scene_paint(main, kDefaultTheme, &budget) ||
        !ui_friends_scene_paint(requests, kDefaultTheme, &budget) || budget.count > kMaxPaintQuads ||
        budget.clip_count > kMaxPaintClips || budget.rejected)
        return false;
    main_fixture.count = main_source.count = 8;
    ++main_source.revision;
    ranges[0] = {UiFriendsSectionKind::incoming, 0, 4, 4, false, true};
    ranges[1] = {UiFriendsSectionKind::outgoing, 4, 4, 4, false, false};
    ++sections.revision;
    main.scroll_dips = requests.scroll_dips = 0;
    if (!update_main() ||
        !ui_friends_scene_update(&requests, &hidden_source, UiFriendsTab::pending, UiFriendsPhase::ready,
                                 {}, viewport(), nullptr, &hidden_sections))
        return false;
    renderer_bind_friends_requests_scene(&g_probe.renderer, &requests);
    g_probe.failure_stage = 280;
    ui_friends_scene_set_focus_visible(&requests, true);
    if (!ui_friends_scene_focus(&requests, {UiFriendsAction::accept, 123, UiFriendsTab::pending}))
        return false;
    g_probe.renderer.focus_owner = &requests.widgets;
    if (!paint() || g_probe.renderer.composer_layout.active ||
        g_probe.renderer.friends_search_geometry.selection_count)
        return false;
    const auto& renderer = g_probe.renderer;
    if (renderer.overlay_paint_first_batch >= renderer.friends_requests_foreground_first_batch ||
        renderer.friends_requests_foreground_first_batch >= renderer.friends_requests_paint_end_batch ||
        renderer.friends_requests_media_first_batch >= renderer.friends_requests_media_end_batch ||
        renderer.friends_requests_media_end_batch > renderer.popup_media_first_batch ||
        renderer.overlay_text_first_batch >= renderer.friends_requests_text_end_batch ||
        renderer.friends_requests_text_end_batch > renderer.menu_text_first_batch)
        return false;
    UiFriendsVisibleRow row{};
    AvatarMedia avatar{};
    if (!ui_friends_scene_visible_row(requests, 0, &row) || !row.row.ignored || row.row.spam ||
        !friends_avatar_media_resolve(requests, g_probe.users, 0, &avatar) ||
        !media_atlas_peek(&g_probe.media_atlas, avatar.key).ok())
        return false;
    UiFriendsVisibleRow spam{};
    if (!ui_friends_scene_visible_row(requests, 2, &spam) || !spam.row.spam || spam.row.ignored)
        return false;
    const Color dark{kDefaultTheme.base_lowest.red * .35f, kDefaultTheme.base_lowest.green * .35f,
                     kDefaultTheme.base_lowest.blue * .35f, 1};
    const ProbePixelSample samples[]{
        {{center().right - 4, center().bottom - 4}, dark, 3},
        {{requests.surface.left + 5, requests.surface.top + 5}, kDefaultTheme.base_lowest, 3},
        {{(row.avatar.left + row.avatar.right) * .5f, (row.avatar.top + row.avatar.bottom) * .5f},
         {120.f / 255, 80.f / 255, 180.f / 255, 1},
         3},
        {{(row.presence.left + row.presence.right) * .5f, (row.presence.top + row.presence.bottom) * .5f},
         kDefaultTheme.tokens.status.online,
         3}};
    UiRect modal_title = text_rect(requests, utf8_literal("Other requests"));
    g_probe.failure_stage = 281;
    if (!probe_exact_pixels({samples, 4}) || !probe_text_pixels({&modal_title, 1}) ||
        !probe_capture_frame(L"friends-requests-modal.bmp"))
        return false;
    const u32 calls = hidden_fixture.calls, main_calls = main_fixture.calls,
              builds = renderer.content_text_retention.builds, paints = renderer.paint_stream_builds;
    g_probe.failure_stage = 282;
    if (!paint() || hidden_fixture.calls != calls || main_fixture.calls != main_calls ||
        renderer.content_text_retention.builds != builds || renderer.paint_stream_builds != paints)
        return false;
    const auto stamp = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
    g_probe.renderer.focus_owner = &main.widgets;
    if (d3d11_renderer_visible_focus_owner(renderer) ||
        renderer_presentation_stamp_equal(stamp,
                                          renderer_capture_presentation_stamp(renderer, kDefaultTheme)))
        return false;
    const auto hidden_stamp = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
    g_probe.renderer.focus_owner = &g_probe.message_scene.widgets;
    if (!renderer_presentation_stamp_equal(hidden_stamp,
                                           renderer_capture_presentation_stamp(renderer, kDefaultTheme)))
        return false;
    g_probe.renderer.focus_owner = &requests.widgets;
    ++requests.revision;
    if (d3d11_renderer_interaction_text_state_current(renderer) ||
        renderer_presentation_stamp_equal(stamp,
                                          renderer_capture_presentation_stamp(renderer, kDefaultTheme)))
        return false;
    if (!paint())
        return false;
    g_probe.failure_stage = 278;
    // A real native profile covers Requests; no seventh capture is needed.
    UserProfile profile_data{};
    profile_data.user_id = 123;
    copy_bytes(profile_data.display_name.data, "Requests profile", 16);
    profile_data.display_name.bytes = 16;
    profile.phase = ProfilePhase::ready;
    profile.measure = &g_probe.text_measure_cache;
    if (!profile_scene_open(&profile, &profile_data, &g_probe.chat_state, &g_probe.users, 1, row.avatar,
                            viewport()))
        return false;
    g_probe.renderer.profile_scene = &profile;
    g_probe.renderer.focus_owner = &profile.widgets;
    if (!paint() ||
        g_probe.renderer.popup_media_first_batch < g_probe.renderer.friends_requests_media_end_batch ||
        g_probe.renderer.profile_foreground_first_batch <
            g_probe.renderer.friends_requests_paint_end_batch ||
        d3d11_renderer_visible_focus_owner(renderer) != &profile.widgets)
        return false;
    UiTextCommand profile_commands[128]{};
    UiTextCommandStream profile_text{};
    UiTextOutput profile_text_output{&profile_text, nullptr};
    UiRect profile_name{};
    if (!ui_text_command_stream_initialize(&profile_text, {profile_commands, 128}) ||
        !profile_scene_text_output(profile, kDefaultTheme, &profile_text_output, nullptr))
        return false;
    for (u32 i = 0; i < profile_text.count; ++i)
        if (utf8_equal(profile_commands[i].text, profile_data.display_name.view()))
            profile_name = ui_rect_intersection(profile_commands[i].rectangle, profile_commands[i].clip);
    if (!ui_rect_has_area(profile_name) || !probe_text_pixels({&profile_name, 1}))
        return false;
    g_probe.renderer.profile_scene = restore.old_profile;
    profile_scene_release(&profile);
    {
        PopupScene menu{};
        PopupItem storage[2]{};
        PopupRowWidget menu_rows[2]{};
        UiWidget widgets[12]{};
        UiLayoutNode nodes[12]{};
        struct RestoreMenu {
            PopupScene& scene;
            const PopupScene* previous;
            ~RestoreMenu() noexcept {
                renderer_bind_popup_scene(&g_probe.renderer, previous);
                popup_scene_release(&scene);
            }
        } menu_restore{menu, g_probe.renderer.popup_scene};
        const PopupItem items[]{{utf8_literal("Profile"), 1, popup_item_enabled},
                                {utf8_literal("Block"), 2, popup_item_enabled | popup_item_danger}};
        if (!popup_scene_initialize(&menu, {storage, 2}, {menu_rows, 2}, {widgets, 12}, {nodes, 12}) ||
            !popup_scene_open(&menu, PopupKind::context_menu, 123, {items, 2}, row.rectangle, viewport(),
                              200)
                 .ok() ||
            !renderer_bind_popup_scene(&g_probe.renderer, &menu))
            return false;
        g_probe.renderer.focus_owner = &menu.widgets;
        if (!paint() ||
            g_probe.renderer.menu_paint_first_batch < g_probe.renderer.friends_requests_paint_end_batch ||
            d3d11_renderer_visible_focus_owner(renderer) != &menu.widgets ||
            !probe_capture_frame(L"friends-requests-menu.bmp"))
            return false;
    }
    TextFormContent confirmation{};
    confirmation.title = utf8_literal("Ignore user");
    g_probe.failure_stage = 283;
    confirmation.context = utf8_literal("Ignore this person? Their friend requests will appear in Other "
                                        "requests, and existing friendships are preserved.");
    confirmation.cancel = utf8_literal("Cancel");
    confirmation.accept = utf8_literal("Ignore");
    confirmation.can_accept = true;
    confirmation.focus_visible = true;
    confirmation.confirmation_only = true;
    if (!text_form_update(&form, confirmation, viewport(), &g_probe.text_measure_cache) ||
        !text_form_focus(&form, TextFormElement::cancel))
        return false;
    form_surface.scene = &form;
    renderer_bind_text_form(&g_probe.renderer, &form_surface);
    g_probe.renderer.focus_owner = &form.widgets;
    if (!paint() || form_surface.layout.active ||
        d3d11_renderer_visible_focus_owner(renderer) != &form.widgets ||
        g_probe.renderer.form_paint_first_batch < g_probe.renderer.friends_requests_paint_end_batch ||
        !probe_capture_frame(L"friends-requests-confirmation.bmp"))
        return false;
    const auto* form_title = text_form_node(form, TextFormElement::title);
    const UiRect form_title_rect =
        form_title ? ui_rect_intersection(form_title->content_rectangle, form_title->clip_rectangle)
                   : UiRect{};
    if (!ui_rect_has_area(form_title_rect) || !probe_text_pixels({&form_title_rect, 1}))
        return false;
    renderer_bind_text_form(&g_probe.renderer, restore.old_form);
    text_form_surface_release(&form_surface);
    text_form_release(&form);
    g_probe.renderer.focus_owner = &requests.widgets;
    const float scale = renderer.dpi / 96.f;
    g_probe.failure_stage = 284;
    // Preserve the same 320-DIP center-pane minimum used by the native
    // window/scene contract. The former 540-DIP host left only 228 DIPs after
    // the sidebar and failed before a modal could be rendered.
    const float compact_width = maximum(640.f, center().left + 320.f);
    const auto compact_check = [](bool success, const char* label) noexcept {
        if (!success)
            log_write(LogLevel::error, "probe", label);
        return success;
    };
    if (!compact_check(renderer_resize(&g_probe.renderer, static_cast<u32>(compact_width * scale + .999f),
                                       static_cast<u32>(360 * scale + .999f)),
                       "requests_compact_resize_failed"))
        return false;
    if (!compact_check(synchronize_probe_shell_layout(), "requests_compact_shell_layout_failed"))
        return false;
    if (!compact_check(update_main(), "requests_compact_main_scene_failed")) {
        log_u64(LogLevel::error, "probe", "requests_compact_main_width",
                static_cast<u64>(ui_rect_width(center())));
        log_u64(LogLevel::error, "probe", "requests_compact_main_height",
                static_cast<u64>(ui_rect_height(center())));
        return false;
    }
    if (!compact_check(ui_friends_scene_update(&requests, &hidden_source, UiFriendsTab::pending,
                                               UiFriendsPhase::ready, {}, viewport(), nullptr,
                                               &hidden_sections),
                       "requests_compact_modal_scene_failed"))
        return false;
    if (!compact_check(ui_rect_width(requests.surface) <= 480 && ui_rect_height(requests.surface) <= 328,
                       "requests_compact_card_bounds_failed"))
        return false;
    const auto last_fully_visible = [&]() noexcept {
        UiFriendsVisibleRow last{};
        const u32 count = ui_friends_scene_visible_count(requests);
        for (u32 i = 0; i < count; ++i)
            if (ui_friends_scene_visible_row(requests, i, &last) && last.row.user_id == 50003)
                return ui_rect_equal(last.rectangle, last.clip) && last.row.spam;
        return false;
    };
    const auto initial_scrollbar = ui_friends_scene_scrollbar(requests);
    if (!compact_check(initial_scrollbar.visible && initial_scrollbar.maximum_scroll > 0,
                       "requests_compact_scrollbar_geometry_failed"))
        return false;
    if (!compact_check(ui_friends_scene_page(&requests, false).user_id == 50003 &&
                           requests.scroll_dips > 0 && last_fully_visible(),
                       "requests_compact_page_last_row_failed"))
        return false;
    if (!compact_check(ui_friends_scene_navigate(&requests, UiListMove::first).user_id == 123 &&
                           requests.scroll_dips == 0 &&
                           ui_friends_scene_navigate(&requests, UiListMove::last).user_id == 50003 &&
                           last_fully_visible(),
                       "requests_compact_end_last_row_failed"))
        return false;
    // Held by a drag, the bar is the opaque 8 DIP pressed pill (at rest it is translucent).
    g_probe.renderer.scrollbar_hot = {ui_friends_scene_scrollbar(requests).track, UiScrollbarVisualState::pressed};
    if (!compact_check(renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme),
                       "requests_compact_text_failed"))
        return false;
    if (!compact_check(probe_present(&g_probe.renderer, kDefaultTheme),
                       "requests_compact_gpu_paint_failed"))
        return false;
    if (!compact_check(g_probe.renderer.paint_stream.count <= kMaxPaintQuads &&
                           g_probe.renderer.paint_stream.clip_count <= kMaxPaintClips &&
                           !g_probe.renderer.paint_stream.rejected &&
                           !g_probe.text.message_geometry_quads &&
                           g_probe.renderer.paint_stream.filter_scrollbars &&
                           d3d11_renderer_interaction_text_state_current(g_probe.renderer),
                       "requests_compact_budget_or_currentness_failed"))
        return false;
    const auto compact_scrollbar = ui_friends_scene_scrollbar(requests);
    const ProbePixelSample thumb_pixel{
        {compact_scrollbar.thumb.right - 4, (compact_scrollbar.thumb.top + compact_scrollbar.thumb.bottom) * .5f},
        kDefaultTheme.tokens.text.muted,
        3};
    if (!compact_check(compact_scrollbar.visible && probe_exact_pixels({&thumb_pixel, 1}),
                       "requests_compact_scrollbar_gpu_pixels_failed"))
        return false;
    g_probe.renderer.scrollbar_hot = {};
    if (!compact_check(probe_capture_frame(L"friends-requests-compact.bmp"),
                       "requests_compact_capture_failed"))
        return false;
    if (!compact_check(restore.finish(), "requests_compact_restore_failed"))
        return false;
    log_write(LogLevel::info, "probe", "friends_pending_requests_gpu_layers_modal_owners_compact_passed");
    return true;
}

// Account pages as application_account_settings projects them: an avatar
// card, masked contact rows, the current device (accent, no action) before
// other devices (critical Log out), and synced Content & social controls.
bool probe_account_settings_pages() noexcept {
    UiSettingsScene scene{};
    UiPageRegistry registry{};
    UiPage pages[4]{};
    struct Restore {
        UiSettingsScene& scene;
        ~Restore() noexcept {
            renderer_bind_settings_scene(&g_probe.renderer, nullptr);
            ui_settings_scene_release(&scene);
        }
    } restore{scene};
    using Kind = UiSettingsControlKind;
    using Variant = UiSettingsVariant;
    const UiSettingsControl account[]{
        {1, utf8_literal("Rob"), utf8_literal("rob"), Kind::action, true, false, {}, 0, utf8_literal("Edit profile")},
        {2, utf8_literal("Account details"), {}, Kind::category, true, false},
        {3, utf8_literal("Display name"), utf8_literal("Rob"), Kind::action, true, false, {}, 0, utf8_literal("Edit")},
        {4, utf8_literal("Username"), utf8_literal("rob"), Kind::action, true, false, {}, 0,
         utf8_literal("Edit in browser")},
        {5, utf8_literal("Email"), utf8_literal("r**@example.com"), Kind::action, true, false, {}, 0,
         utf8_literal("Reveal")},
        {6, utf8_literal("Phone number"), utf8_literal("Not added"), Kind::action, true, false, {}, 0,
         utf8_literal("Add in browser")},
        {7, utf8_literal("Password and authentication"), {}, Kind::category, true, false},
        {8, utf8_literal("Two-factor authentication is on"),
         utf8_literal("Your account asks for a code when you sign in."), Kind::notice, true, false},
    };
    const UiSettingsControlPresentation account_looks[]{
        {Variant::standard, ui_settings_presentation_avatar, kUiSettingsNoDefault},
        {}, {}, {}, {}, {}, {},
        {Variant::positive, 0, kUiSettingsNoDefault}};
    const UiSettingsControl devices[]{
        {1, utf8_literal("Signed-in devices"), utf8_literal("Log out of any device you don't recognize."),
         Kind::category, true, false},
        {2, utf8_literal("Windows · MaiCord"), utf8_literal("This device · Bergen, Norway"), Kind::action, true,
         false},
        {3, utf8_literal("Android · Discord Android"), utf8_literal("Oslo, Norway · Active 2 hours ago"),
         Kind::action, true, false, {}, 0, utf8_literal("Log out")},
        {4, utf8_literal("Windows · Discord Client"), utf8_literal("Active 3 days ago"), Kind::action, true, false,
         {}, 0, utf8_literal("Log out")},
    };
    const UiSettingsControlPresentation device_looks[]{
        {}, {Variant::standard, ui_settings_presentation_accent, kUiSettingsNoDefault},
        {Variant::critical, 0, kUiSettingsNoDefault}, {Variant::critical, 0, kUiSettingsNoDefault}};
    const Utf8View spam[]{utf8_literal("Filter all"), utf8_literal("Non-friends only"), utf8_literal("Off")};
    const Utf8View media[]{utf8_literal("Show"), utf8_literal("Blur"), utf8_literal("Block")};
    const UiSettingsControl content[]{
        {1, utf8_literal("Direct messages"), {}, Kind::category, true, false},
        {2, utf8_literal("Allow DMs from server members"), utf8_literal("Applies to servers you join from now on."),
         Kind::toggle, true, true},
        {3, utf8_literal("Message requests"),
         utf8_literal("Enable requests from server members you may not know. Applies to servers you join from now on."),
         Kind::toggle, true, true},
        {4, utf8_literal("DM spam filter"), utf8_literal("Messages that look like spam go to a separate inbox."),
         Kind::radio, true, false, {spam, 3}, 1},
        {5, utf8_literal("Sensitive media"), utf8_literal("Media that may be sexually explicit."), Kind::category,
         true, false},
        {6, utf8_literal("DMs from friends"), {}, Kind::choice, true, false, {media, 3}, 0},
        {7, utf8_literal("Friend requests"), utf8_literal("Who can send you a friend request."), Kind::category,
         true, false},
        {8, utf8_literal("Everyone"), {}, Kind::toggle, true, true},
        {9, utf8_literal("Friends of friends"), {}, Kind::toggle, false, true},
    };
    UiSettingsPageContent account_page{utf8_literal("My account"), {}, {account, 8}, 1};
    account_page.presentation = account_looks;
    UiSettingsPageContent device_page{utf8_literal("Devices"), {}, {devices, 4}, 1};
    device_page.presentation = device_looks;
    UiSettingsPageContent content_page{utf8_literal("Content & social"), {}, {content, 9}, 1};
    // The Chat page as settings_catalog lays it out (text_and_images).
    const Utf8View stickers[]{utf8_literal("Always animate"), utf8_literal("Animate on hover"),
                              utf8_literal("Never animate")};
    const Utf8View spoilers[]{utf8_literal("On click"), utf8_literal("On servers I moderate"), utf8_literal("Always")};
    const UiSettingsControl chat[]{
        {1, utf8_literal("Images, videos and GIFs"), {}, Kind::category, true, false},
        {2, utf8_literal("When posted as links to chat"), {}, Kind::toggle, true, true},
        {3, utf8_literal("When uploaded directly to Discord"), utf8_literal("Off shows uploads as file cards."),
         Kind::toggle, true, false},
        {4, utf8_literal("Embeds and link previews"), {}, Kind::category, true, false},
        {5, utf8_literal("Show embeds and link previews"), {}, Kind::toggle, true, true},
        {6, utf8_literal("Emoji"), {}, Kind::category, true, false},
        {7, utf8_literal("Show emoji reactions"), {}, Kind::toggle, true, true},
        {8, utf8_literal("Convert emoticons to emoji"), utf8_literal("For example, :) becomes \xF0\x9F\x99\x82 when you send it."),
         Kind::toggle, true, true},
        {9, utf8_literal("Animation"), utf8_literal("Off keeps media still until you hover or focus its message."),
         Kind::category, true, false},
        {10, utf8_literal("Play GIFs automatically"), {}, Kind::toggle, true, false},
        {11, utf8_literal("Play animated emoji"), {}, Kind::toggle, true, true},
        {12, utf8_literal("Sticker animation"), {}, Kind::radio, true, false, {stickers, 3}, 1},
        {13, utf8_literal("Spoilers"), {}, Kind::category, true, false},
        {14, utf8_literal("Show spoiler content"), {}, Kind::radio, true, false, {spoilers, 3}, 0},
    };
    UiSettingsPageContent chat_page{utf8_literal("Chat"), {}, {chat, 14}, 1};
    const UiPageKey account_key{1, 1}, device_key{1, 2}, content_key{1, 3}, chat_key{1, 4};
    const float scale = 96.0f / g_probe.renderer.dpi;
    const UiRect viewport{0, 0, g_probe.renderer.width * scale, g_probe.renderer.height * scale};
    bool valid = ui_page_registry_initialize(&registry, {pages, 4}) &&
                 ui_page_registry_register(&registry, {account_key, utf8_literal("My account"), &account_page}).ok() &&
                 ui_page_registry_register(&registry, {device_key, utf8_literal("Devices"), &device_page}).ok() &&
                 ui_page_registry_register(&registry, {content_key, utf8_literal("Content & social"), &content_page})
                     .ok() &&
                 ui_page_registry_register(&registry, {chat_key, utf8_literal("Chat"), &chat_page}).ok() &&
                 ui_settings_scene_update(&scene, &registry, viewport);
    renderer_bind_settings_scene(&g_probe.renderer, &scene);
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) && probe_capture_frame(L"user-settings-account.bmp");
    const auto show = [&](UiPageKey key, const wchar_t* capture) noexcept {
        return ui_page_registry_select(&registry, key).ok() && ui_settings_scene_update(&scene, &registry, viewport) &&
               scene.content.controls.count && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
               probe_present(&g_probe.renderer, kDefaultTheme) && probe_capture_frame(capture);
    };
    valid = valid && show(device_key, L"user-settings-devices.bmp") && show(content_key, L"user-settings-content.bmp") &&
            show(chat_key, L"user-settings-chat.bmp");
    if (valid)
        log_write(LogLevel::info, "probe", "account_settings_pages_passed");
    else
        log_write(LogLevel::error, "probe", "account_settings_pages_failed");
    return valid;
}

// Reads pixels (BGRA) at DIP points from one staging copy: of the drawn
// swap-chain target, or of another texture of the same size.
bool probe_read_pixels(const Theme& theme, ConstSlice<UiPoint> points, u8 (*output)[4],
                       ID3D11Texture2D* texture) noexcept {
    ID3D11Texture2D* source = nullptr;
    if (texture) {
        texture->AddRef();
        source = texture;
    } else {
        if (!d3d11_renderer_draw_stream(&g_probe.renderer, theme, true, true, nullptr))
            return false;
        ID3D11Resource* resource = nullptr;
        g_probe.renderer.graphics.target->GetResource(&resource);
        const HRESULT queried =
            resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source));
        resource->Release();
        if (FAILED(queried))
            return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    const float scale = g_probe.renderer.dpi / 96.0f;
    bool inside = true;
    for (u32 i = 0; i < points.count; ++i)
        inside = inside && points[i].x >= 0 && points[i].y >= 0 &&
                 static_cast<u32>(points[i].x * scale) < description.Width &&
                 static_cast<u32>(points[i].y * scale) < description.Height;
    ProbeTargetReadback readback;
    const bool read = inside && readback.read(g_probe.renderer, source, description) == 0;
    source->Release();
    const auto& mapped = readback.mapped;
    if (read) {
        for (u32 i = 0; i < points.count; ++i)
            copy_bytes(output[i],
                       static_cast<const u8*>(mapped.pData) +
                           static_cast<u32>(points[i].y * scale) * mapped.RowPitch +
                           static_cast<u32>(points[i].x * scale) * 4,
                       4);
    }
    return read;
}

// A modal draws the live shell beneath its scrim. The backdrop follows the
// theme and the window size (identical to the shell's own frame at that
// size, message wrapping included), redraws for each new frame of media it
// shows, and costs nothing while nothing behind changes.
bool probe_settings_backdrop() noexcept {
    g_probe.failure_stage = 733;
    UiSettingsScene scene{};
    UiPageRegistry registry{};
    UiPage pages[1]{};
    const u32 old_width = g_probe.renderer.width, old_height = g_probe.renderer.height;
    struct RestoreBackdropProbe {
        UiSettingsScene& scene;
        u32 width, height;
        ~RestoreBackdropProbe() noexcept {
            renderer_bind_settings_scene(&g_probe.renderer, nullptr);
            ui_settings_scene_release(&scene);
            if ((g_probe.renderer.width != width || g_probe.renderer.height != height) &&
                renderer_resize(&g_probe.renderer, width, height))
                synchronize_probe_shell_layout();
            if (rebuild_probe_content_text())
                probe_present(&g_probe.renderer, kDefaultTheme);
        }
    } restore{scene, old_width, old_height};
    UiSettingsControl control{1, utf8_literal("Backdrop setting"), {}, UiSettingsControlKind::toggle, true, false};
    UiSettingsPageContent content{utf8_literal("Backdrop"), {}, {&control, 1}, 1};
    const auto viewport = []() noexcept -> UiRect {
        const float scale = 96.0f / g_probe.renderer.dpi;
        return {0, 0, g_probe.renderer.width * scale, g_probe.renderer.height * scale};
    };
    ui_settings_scene_set_presentation(&scene, UiSettingsPresentation::modal);
    if (!ui_page_registry_initialize(&registry, {pages, 1}) ||
        !ui_page_registry_register(&registry, {{1, 1}, utf8_literal("Backdrop"), &content}).ok() ||
        !ui_settings_scene_update(&scene, &registry, viewport()))
        return false;
    // The scrim area above the dialog and beside it, where the shell shows.
    const auto samples = [&](const Theme& theme, u8 (*pixels)[4]) noexcept {
        const auto modal = scene.layout.modal;
        const UiPoint points[]{{(modal.left + modal.right) * .5f, modal.top * .5f},
                               {modal.left * .5f, (modal.top + modal.bottom) * .5f},
                               {(modal.right + viewport().right) * .5f, (modal.top + modal.bottom) * .5f}};
        return modal.top > 4 && modal.left > 4 && probe_read_pixels(theme, {points, 3}, pixels);
    };
    const auto luminance = [](const u8 (*pixels)[4]) noexcept {
        u32 sum = 0;
        for (u32 i = 0; i < 3; ++i)
            sum += pixels[i][0] + pixels[i][1] + pixels[i][2];
        return sum;
    };
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    // Mean CPU microseconds per present; animate notes a new frame of media
    // first, exactly as an uploaded GIF, emoji or avatar frame does.
    MediaAtlasHandle frame_handle{};
    u8 frame_domain = 0;
    const auto present_us = [&](u32 count, bool animate) noexcept -> u64 {
        u64 total = 0;
        for (u32 i = 0; i < count; ++i) {
            if (animate &&
                !presentation_damage_note_media(&g_probe.renderer.presentation_damage, frame_handle, frame_domain))
                return ~0ull;
            LARGE_INTEGER begin{}, end{};
            QueryPerformanceCounter(&begin);
            if (!probe_present(&g_probe.renderer, kDefaultTheme))
                return ~0ull;
            QueryPerformanceCounter(&end);
            total += static_cast<u64>(end.QuadPart - begin.QuadPart) * 1'000'000ull /
                     static_cast<u64>(frequency.QuadPart);
        }
        return total / count;
    };
    renderer_bind_settings_scene(&g_probe.renderer, &scene);
    const u32 renders = g_probe.renderer.settings_backdrop_renders;
    u8 dark[3][4]{};
    if (!renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.settings_backdrop_renders != renders + 1 || !g_probe.renderer.settings_backdrop_valid ||
        !samples(kDefaultTheme, dark) || !g_probe.renderer.settings_backdrop_media_count)
        return false;
    frame_handle = g_probe.renderer.settings_backdrop_media[0];
    frame_domain = g_probe.renderer.settings_backdrop_media_domain[0];
    g_probe.failure_stage = 734;
    // Idle: modal-only frames reuse the backdrop and the retained stream.
    const u32 paints = g_probe.renderer.paint_stream_builds;
    const u64 idle_us = present_us(8, false);
    if (idle_us == ~0ull || g_probe.renderer.settings_backdrop_renders != renders + 1 ||
        g_probe.renderer.paint_stream_builds != paints)
        return false;
    g_probe.failure_stage = 735;
    // Animated media behind: every new frame redraws the backdrop once, and
    // nothing is replayed afterwards.
    const u64 animated_us = present_us(8, true);
    if (animated_us == ~0ull || g_probe.renderer.settings_backdrop_renders != renders + 9 ||
        present_us(2, false) == ~0ull || g_probe.renderer.settings_backdrop_renders != renders + 9)
        return false;
    g_probe.failure_stage = 736;
    // A theme change re-themes the shell beneath the modal.
    u8 light[3][4]{};
    if (!renderer_rebuild_content_text(&g_probe.renderer, kLightTheme) ||
        !probe_present(&g_probe.renderer, kLightTheme) ||
        g_probe.renderer.settings_backdrop_renders != renders + 10 || !samples(kLightTheme, light) ||
        luminance(light) < luminance(dark) + 3 * 3 * 24 ||
        !probe_capture_frame(L"settings-backdrop-light.bmp", kLightTheme))
        return false;
    g_probe.failure_stage = 737;
    // A resize lays the whole shell out again beneath the modal, message text
    // re-converged at the new width: the backdrop equals the shell's frame.
    const u32 wide_width = old_width + old_width / 4, wide_height = old_height + old_height / 8;
    if (!renderer_resize(&g_probe.renderer, wide_width, wide_height) || !synchronize_probe_shell_layout() ||
        !ui_settings_scene_update(&scene, &registry, viewport()) || !rebuild_probe_content_text() ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.settings_backdrop_renders != renders + 11 ||
        g_probe.renderer.settings_backdrop_width != wide_width ||
        !probe_capture_frame(L"settings-backdrop-resized.bmp"))
        return false;
    constexpr u32 kColumns = 8, kRows = 6;
    UiPoint grid[kColumns * kRows]{};
    for (u32 row = 0; row < kRows; ++row)
        for (u32 column = 0; column < kColumns; ++column)
            grid[row * kColumns + column] = {viewport().right * (column + .5f) / kColumns,
                                             viewport().bottom * (row + .5f) / kRows};
    u8 backdrop[kColumns * kRows][4]{}, shell[kColumns * kRows][4]{};
    if (!probe_read_pixels(kDefaultTheme, {grid, kColumns * kRows}, backdrop, g_probe.renderer.settings_backdrop))
        return false;
    renderer_bind_settings_scene(&g_probe.renderer, nullptr);
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_read_pixels(kDefaultTheme, {grid, kColumns * kRows}, shell))
        return false;
    u32 matching = 0;
    for (u32 i = 0; i < kColumns * kRows; ++i) {
        u32 worst = 0;
        for (u32 c = 0; c < 3; ++c)
            worst = maximum<u32>(worst, backdrop[i][c] > shell[i][c] ? backdrop[i][c] - shell[i][c]
                                                                     : shell[i][c] - backdrop[i][c]);
        matching += worst <= 2;
    }
    if (matching * 10 < kColumns * kRows * 9)
        return false;
    g_probe.failure_stage = 738;
    // The same media frame with no modal open, for the cost comparison.
    const u64 shell_us = present_us(8, true);
    if (shell_us == ~0ull)
        return false;
    log_u64(LogLevel::info, "probe", "settings_backdrop_idle_present_us", idle_us);
    log_u64(LogLevel::info, "probe", "settings_backdrop_animated_present_us", animated_us);
    log_u64(LogLevel::info, "probe", "shell_animated_present_us", shell_us);
    log_u64(LogLevel::info, "probe", "settings_backdrop_matching_samples", matching);
    log_write(LogLevel::info, "probe", "settings_live_backdrop_theme_resize_animation_retention_passed");
    return true;
}

} // namespace ui_probe_detail

namespace {

// Opening Settings fades a scrim in and grows the dialog (or settles a
// full-window layer) over the live shell. Each entrance frame is read as
// presented: two targets stand in for the flip-model back buffers. For one
// progress, a frame that reuses the backdrop, one that redraws it for a new
// media frame, one after the shell's message text converged beneath and one
// whose own stream rebuilt are identical; the first frame equals the shell
// before opening; a dialog's scrim only deepens. Behind an open dialog the
// backdrop's media stay pinned across modal-only frames, so an upload that
// needs the whole atlas is refused instead of resetting it, which made each
// backdrop redraw drop and restore the shell's images (the reported flicker).
bool probe_settings_entrance(UiSettingsPresentation presentation, u32 stage) noexcept {
    g_probe.failure_stage = stage;
    auto& renderer = g_probe.renderer;
    UiSettingsScene scene{};
    UiPageRegistry registry{};
    UiPage pages[1]{};
    ID3D11Texture2D* buffers[2]{};
    ID3D11RenderTargetView* targets[2]{};
    ID3D11RenderTargetView* const presented = renderer.graphics.target;
    struct RestoreEntranceProbe {
        UiSettingsScene& scene;
        ID3D11Texture2D** buffers;
        ID3D11RenderTargetView** targets;
        ID3D11RenderTargetView* presented;
        ~RestoreEntranceProbe() noexcept {
            g_probe.renderer.graphics.target = presented;
            renderer_bind_settings_scene(&g_probe.renderer, nullptr);
            renderer_set_settings_modal_progress(&g_probe.renderer, 1);
            ui_settings_scene_release(&scene);
            for (u32 i = 0; i < 2; ++i) {
                if (targets[i])
                    targets[i]->Release();
                if (buffers[i])
                    buffers[i]->Release();
            }
            if (rebuild_probe_content_text())
                probe_present(&g_probe.renderer, kDefaultTheme);
        }
    } restore{scene, buffers, targets, presented};
    ID3D11Resource* resource = nullptr;
    presented->GetResource(&resource);
    ID3D11Texture2D* back = nullptr;
    const bool typed =
        resource && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)));
    if (resource)
        resource->Release();
    if (!typed)
        return false;
    D3D11_TEXTURE2D_DESC description{};
    back->GetDesc(&description);
    back->Release();
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.CPUAccessFlags = description.MiscFlags = 0;
    description.MipLevels = description.ArraySize = 1;
    for (u32 i = 0; i < 2; ++i)
        if (FAILED(renderer.graphics.device->CreateTexture2D(&description, nullptr, &buffers[i])) ||
            FAILED(renderer.graphics.device->CreateRenderTargetView(buffers[i], nullptr, &targets[i])))
            return false;
    UiSettingsControl control{1, utf8_literal("Entrance setting"), {}, UiSettingsControlKind::toggle, true, false};
    UiSettingsPageContent content{utf8_literal("Entrance"), {}, {&control, 1}, 1};
    const float dips = 96.0f / renderer.dpi;
    const UiRect viewport{0, 0, renderer.width * dips, renderer.height * dips};
    ui_settings_scene_set_presentation(&scene, presentation);
    if (!ui_page_registry_initialize(&registry, {pages, 1}) ||
        !ui_page_registry_register(&registry, {{1, 1}, utf8_literal("Entrance"), &content}).ok() ||
        !ui_settings_scene_update(&scene, &registry, viewport))
        return false;
    const bool dialog = presentation != UiSettingsPresentation::layer;
    const UiRect modal = scene.layout.modal;
    if (dialog && (modal.left <= 8 || modal.top <= 8))
        return false;
    // A dialog: the shell beside, above and right of it, and its centre. A
    // layer: points across the window.
    constexpr u32 kPoints = 4, kScrimPoints = 3;
    const UiPoint points[kPoints]{
        dialog ? UiPoint{modal.left * .5f, (modal.top + modal.bottom) * .5f} : UiPoint{viewport.right * .1f, viewport.bottom * .5f},
        dialog ? UiPoint{(modal.left + modal.right) * .5f, modal.top * .5f} : UiPoint{viewport.right * .5f, viewport.bottom * .08f},
        dialog ? UiPoint{(modal.right + viewport.right) * .5f, modal.top + 24} : UiPoint{viewport.right * .9f, viewport.bottom * .3f},
        dialog ? UiPoint{(modal.left + modal.right) * .5f, (modal.top + modal.bottom) * .5f}
               : UiPoint{viewport.right * .5f, viewport.bottom * .5f}};
    u32 frame = 0;
    const auto render = [&](u8 (*pixels)[4]) noexcept {
        renderer.graphics.target = targets[frame & 1u];
        const bool presented_frame = probe_present(&renderer, kDefaultTheme);
        renderer.graphics.target = presented;
        const bool read =
            presented_frame && probe_read_pixels(kDefaultTheme, {points, kPoints}, pixels, buffers[frame & 1u]);
        ++frame;
        return read;
    };
    const auto distance = [](const u8 (*a)[4], const u8 (*b)[4], u32 count) noexcept {
        u32 worst = 0;
        for (u32 i = 0; i < count; ++i)
            for (u32 c = 0; c < 3; ++c)
                worst = maximum<u32>(worst, a[i][c] > b[i][c] ? a[i][c] - b[i][c] : b[i][c] - a[i][c]);
        return worst;
    };
    const auto luminance = [](const u8 (*pixels)[4], u32 count) noexcept {
        u32 sum = 0;
        for (u32 i = 0; i < count; ++i)
            sum += pixels[i][0] + pixels[i][1] + pixels[i][2];
        return sum;
    };
    u8 shell[kPoints][4]{};
    renderer.retained_presentation.valid = false;
    presentation_damage_invalidate_full(&renderer.presentation_damage);
    if (!rebuild_probe_content_text() || !render(shell))
        return false;
    renderer_bind_settings_scene(&renderer, &scene);
    renderer_set_settings_modal_progress(&renderer, 0);
    if (!renderer_rebuild_content_text(&renderer, kDefaultTheme))
        return false;
    g_probe.failure_stage = stage + 1;
    constexpr u32 kSteps = 8;
    const u32 renders = renderer.settings_backdrop_renders;
    MediaAtlasHandle media{};
    u8 domain = 0;
    u8 previous[kPoints][4]{}, first[kPoints][4]{};
    u32 mismatches = 0, brighter = 0, invalid = 0, worst = 0, failed = 0;
    for (u32 step = 0; step <= kSteps; ++step) {
        u8 reused[kPoints][4]{}, redrawn[kPoints][4]{}, converged[kPoints][4]{}, rebuilt[kPoints][4]{};
        renderer_set_settings_modal_progress(&renderer, static_cast<float>(step) / kSteps);
        if (!render(reused))
            return false;
        if (step == 0) {
            if (!renderer.settings_backdrop_media_count)
                return false;
            media = renderer.settings_backdrop_media[0];
            domain = renderer.settings_backdrop_media_domain[0];
            copy_bytes(first, reused, sizeof(first));
        }
        // A new frame of animated media behind the dialog redraws the backdrop.
        if (!presentation_damage_note_media(&renderer.presentation_damage, media, domain) || !render(redrawn))
            return false;
        // The shell's text converges beneath (a gateway update): the account
        // path rebuilds content text with the dialog bound.
        if (!rebuild_probe_content_text() || !render(converged))
            return false;
        // The dialog's own stream rebuilds over the reused backdrop.
        renderer.retained_presentation.valid = false;
        presentation_damage_invalidate_full(&renderer.presentation_damage);
        if (!render(rebuilt))
            return false;
        invalid += renderer.settings_backdrop_valid || (!dialog && step == kSteps) ? 0u : 1u;
        failed += renderer.settings_backdrop_failed ? 1u : 0u;
        const u32 frame_worst =
            maximum(distance(reused, redrawn, kPoints),
                    maximum(distance(reused, converged, kPoints), distance(reused, rebuilt, kPoints)));
        worst = maximum(worst, frame_worst);
        mismatches += frame_worst > 2 ? 1u : 0u;
        if (dialog && step && luminance(reused, kScrimPoints) > luminance(previous, kScrimPoints) + 3)
            ++brighter;
        copy_bytes(previous, reused, sizeof(previous));
    }
    const u32 redraws = renderer.settings_backdrop_renders - renders;
    u32 backdrop_media = 0, pinned = 0, survivors = 0;
    bool reset = false, unpinned_reset = false;
    if (dialog) {
        const MediaAtlas& atlas = *renderer.media_atlas;
        if (atlas.allocations.entry_capacity > kProbeMediaEntryCapacity ||
            atlas.allocations.row_capacity > kProbeMediaRowCapacity)
            return false;
        // An upload needing the whole atlas, taken by a copy of the allocator
        // so the probe's atlas stays intact.
        GlyphAtlasEntry entries[kProbeMediaEntryCapacity]{};
        GlyphAtlasRow rows[kProbeMediaRowCapacity]{};
        GlyphAtlasAllocator trial{};
        const auto whole_upload = [&]() noexcept {
            copy_bytes(entries, atlas.allocations.entries, atlas.allocations.entry_capacity * sizeof(GlyphAtlasEntry));
            copy_bytes(rows, atlas.allocations.rows, atlas.allocations.row_capacity * sizeof(GlyphAtlasRow));
            trial = atlas.allocations;
            trial.entries = entries;
            trial.rows = rows;
            return glyph_atlas_allocate(&trial, ~u64{0} - 7, static_cast<u16>(trial.width - 2 * trial.padding),
                                        static_cast<u16>(trial.height - 2 * trial.padding))
                .atlas_reset;
        };
        // How every modal-only frame left the shell's media before: unpinned,
        // so the upload resets the atlas beneath the backdrop.
        media_atlas_begin_visibility(renderer.media_atlas);
        unpinned_reset = whole_upload();
        // Modal-only frames (a caret, a hover, the dialog's own media) begin
        // the next epochs; the backdrop's media stay pinned through them.
        u8 settled[kPoints][4]{};
        for (u32 i = 0; i < 2; ++i) {
            presentation_damage_invalidate_full(&renderer.presentation_damage);
            if (!render(settled))
                return false;
        }
        reset = whole_upload();
        for (u32 i = 0; i < renderer.settings_backdrop_media_count; ++i) {
            const MediaAtlasHandle handle = renderer.settings_backdrop_media[i];
            if (renderer.settings_backdrop_media_domain[i] != 0 || !media_atlas_validate(atlas, handle))
                continue;
            const GlyphAtlasHandle allocation = atlas.entries[handle.slot].allocation;
            GlyphAtlasAllocation kept{};
            ++backdrop_media;
            pinned += glyph_atlas_is_pinned(atlas.allocations, allocation) ? 1u : 0u;
            survivors += glyph_atlas_validate(trial, allocation, &kept) ? 1u : 0u;
        }
    }
    const bool resident = !dialog || (backdrop_media && pinned == backdrop_media &&
                                      survivors == backdrop_media && !reset && unpinned_reset);
    const bool paced = redraws == (dialog ? kSteps + 2 : kSteps + 1);
    const bool opened_on_shell = distance(first, shell, kPoints) <= 2;
    const bool deepened =
        !dialog || luminance(previous, kScrimPoints) + 3 * kScrimPoints * 8 < luminance(first, kScrimPoints);
    log_u64(LogLevel::info, "probe", "settings_entrance_presentation", static_cast<u64>(presentation));
    log_u64(LogLevel::info, "probe", "settings_entrance_frames", frame);
    log_u64(LogLevel::info, "probe", "settings_entrance_backdrop_redraws", redraws);
    log_u64(LogLevel::info, "probe", "settings_entrance_mismatched_steps", mismatches);
    log_u64(LogLevel::info, "probe", "settings_entrance_worst_channel_delta", worst);
    log_u64(LogLevel::info, "probe", "settings_entrance_brighter_steps", brighter);
    log_u64(LogLevel::info, "probe", "settings_entrance_invalid_backdrop_steps", invalid);
    log_u64(LogLevel::info, "probe", "settings_entrance_failed_backdrop_steps", failed);
    log_u64(LogLevel::info, "probe", "settings_entrance_opened_on_shell", opened_on_shell);
    log_u64(LogLevel::info, "probe", "settings_entrance_first_vs_shell", distance(first, shell, kPoints));
    log_u64(LogLevel::info, "probe", "settings_entrance_scrim_deepened", deepened);
    log_u64(LogLevel::info, "probe", "settings_entrance_backdrop_media", backdrop_media);
    log_u64(LogLevel::info, "probe", "settings_entrance_backdrop_media_pinned", pinned);
    log_u64(LogLevel::info, "probe", "settings_entrance_backdrop_media_survivors", survivors);
    log_u64(LogLevel::info, "probe", "settings_entrance_whole_atlas_reset", reset);
    log_u64(LogLevel::info, "probe", "settings_entrance_unpinned_atlas_reset", unpinned_reset);
    if (mismatches || brighter || invalid || failed || !opened_on_shell || !deepened || !resident || !paced)
        return false;
    log_write(LogLevel::info, "probe", "settings_entrance_frames_consistent_passed");
    return true;
}

} // namespace

namespace ui_probe_detail {

bool probe_settings_entrance_frames() noexcept {
    return probe_settings_entrance(UiSettingsPresentation::modal, 3739) &&
           probe_settings_entrance(UiSettingsPresentation::layer, 3741);
}

bool probe_settings_scene() noexcept {
    g_probe.failure_stage = 246;
    UiSettingsScene scene{};
    UiPageRegistry registry{};
    UiPage pages[3]{};
    const u32 old_width = g_probe.renderer.width, old_height = g_probe.renderer.height;
    struct RestoreSettingsProbe {
        UiSettingsScene& scene;
        u32 width, height;
        ~RestoreSettingsProbe() noexcept {
            renderer_bind_settings_scene(&g_probe.renderer, nullptr);
            ui_settings_scene_release(&scene);
            renderer_resize(&g_probe.renderer, width, height);
        }
    } restore{scene, old_width, old_height};
    const Utf8View themes[]{utf8_literal("Dark"), utf8_literal("AMOLED")};
    UiSettingsControl appearance_controls[]{
        {1,
         utf8_literal("Theme"),
         utf8_literal("Choose the colors used by the native client."),
         UiSettingsControlKind::choice,
         true,
         false,
         {themes, 2},
         0},
        {2, utf8_literal("Smooth scrolling"), utf8_literal("Animate scrolling between positions."),
         UiSettingsControlKind::toggle, true, true},
        {3, utf8_literal("Member list"), utf8_literal("Show the member pane in server channels."),
         UiSettingsControlKind::toggle, true, true},
        {4, utf8_literal("Unavailable setting"),
         utf8_literal("Disabled controls keep their label and description readable."),
         UiSettingsControlKind::toggle, false, false},
    };
    UiSettingsControl accessibility_controls[]{
        {1, utf8_literal("Reduced motion"),
         utf8_literal("Reduce decorative animations and scrolling motion."), UiSettingsControlKind::toggle,
         true, false},
    };
    UiSettingsControl plugin_controls[70]{};
    for (u32 i = 0; i < 70; ++i)
        plugin_controls[i] = {
            i + 1,
            utf8_literal("Plugin setting with a descriptive label"),
            utf8_literal("A borrowed plugin descriptor uses the same native row and scrolling surface."),
            UiSettingsControlKind::toggle,
            true,
            (i & 1u) != 0};
    plugin_controls[69] = {70,
                           utf8_literal("Last plugin setting"),
                           utf8_literal("Keyboard focus reveals this virtualized row."),
                           UiSettingsControlKind::toggle,
                           true,
                           true};
    UiSettingsPageContent appearance{utf8_literal("Appearance"),
                                     utf8_literal("Make MaiCord look and feel right for you."),
                                     {appearance_controls, 4},
                                     1};
    UiSettingsPageContent accessibility{utf8_literal("Accessibility"),
                                        utf8_literal("Control motion and interaction preferences."),
                                        {accessibility_controls, 1},
                                        1};
    UiSettingsPageContent plugin{
        utf8_literal("Plugin preferences"),
        utf8_literal(
            "A long modular page owned by a plugin. Its controls share the native settings framework."),
        {plugin_controls, 70},
        1};
    const UiPageKey appearance_key{1, 1}, accessibility_key{1, 2}, plugin_key{77, 1};
    const auto viewport = []() noexcept -> UiRect {
        const float scale = 96.0f / g_probe.renderer.dpi;
        return {0, 0, g_probe.renderer.width * scale, g_probe.renderer.height * scale};
    };
    const auto settings_pixels = [](const UiSettingsScene& settings, bool amoled) noexcept {
        const Theme scene_theme = amoled ? ui_theme_amoled() : kDefaultTheme;
        UiTextCommand commands[64]{};
        UiTextCommandStream stream{};
        UiTextOutput stream_output{&stream, nullptr};
        if (!ui_text_command_stream_initialize(&stream, {commands, 64}) ||
            !ui_settings_scene_text_output(settings, scene_theme, &stream_output))
            return false;
        const Utf8View labels[]{utf8_literal("Theme"), utf8_literal("Smooth scrolling"),
                                amoled ? utf8_literal("AMOLED") : utf8_literal("Dark"),
                                utf8_literal("Accessibility")};
        UiRect rectangles[4]{};
        for (u32 n = 0; n < 4; ++n) {
            bool found = false;
            for (u32 i = 0; i < stream.count; ++i) {
                const auto& command = commands[i];
                if (command.text.bytes == labels[n].bytes &&
                    bytes_equal(command.text.data, labels[n].data, labels[n].bytes)) {
                    rectangles[n] = ui_rect_intersection(command.rectangle, command.clip);
                    found = true;
                    break;
                }
            }
            if (!found || !ui_rect_has_area(rectangles[n]))
                return false;
        }
        return probe_text_pixels({rectangles, 4});
    };
    if (!ui_page_registry_initialize(&registry, {pages, 3}) ||
        !ui_page_registry_register(&registry, {appearance_key, utf8_literal("Appearance"), &appearance})
             .ok() ||
        !ui_page_registry_register(&registry,
                                   {accessibility_key, utf8_literal("Accessibility"), &accessibility})
             .ok() ||
        !ui_page_registry_register(&registry, {plugin_key, utf8_literal("Plugin preferences"), &plugin})
             .ok() ||
        !ui_settings_scene_update(&scene, &registry, viewport()))
        return false;
    renderer_bind_settings_scene(&g_probe.renderer, &scene);
    ui_settings_scene_set_focus_visible(&scene, true);
    if (!ui_settings_scene_focus(&scene, {UiSettingsTargetKind::control, appearance_key, 1}) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) || !settings_pixels(scene, false) ||
        g_probe.renderer.media_quad_count || g_probe.text.message_geometry_quads ||
        !probe_capture_frame(L"settings-dark.bmp"))
        return false;
    const u32 text_builds = g_probe.renderer.content_text_retention.builds;
    const u32 widget_generation = scene.widgets.generation;
    if (!ui_settings_scene_focus(&scene, {UiSettingsTargetKind::control, appearance_key, 2}) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        scene.widgets.generation != widget_generation ||
        g_probe.renderer.content_text_retention.builds != text_builds ||
        !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    UiSettingsActivation activation{};
    if (ui_settings_scene_activation(scene, {UiSettingsTargetKind::control, appearance_key, 4}, false,
                                     &activation))
        return false;
    const u32 paints = g_probe.renderer.paint_stream_builds;
    if (!probe_present(&g_probe.renderer, kDefaultTheme) || g_probe.renderer.paint_stream_builds != paints)
        return false;
    {
        // Pointer state is paint-only: retained text and layout stay reusable,
        // while the native frame must observe hover/capture on the top page.
        UiRect anchor{};
        if (!ui_settings_scene_control_anchor(scene, {UiSettingsTargetKind::control, appearance_key, 2},
                                              &anchor))
            return false;
        const UiPoint point{(anchor.left + anchor.right) * .5f, (anchor.top + anchor.bottom) * .5f};
        const u32 revision = scene.revision, generation = scene.widgets.generation;
        const auto layout = scene.widgets.layout.revision;
        const u32 text_rebuilds = g_probe.renderer.content_text_retention.builds;
        if (!ui_button_pointer(&scene.widgets, UiButtonPointerEvent::move, point).changed ||
            !probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.paint_stream_builds != paints + 1 ||
            !probe_capture_frame(L"settings-pointer-hover.bmp"))
            return false;
        if (!ui_button_pointer(&scene.widgets, UiButtonPointerEvent::press, point).handled ||
            !probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.paint_stream_builds != paints + 2 ||
            !probe_capture_frame(L"settings-pointer-pressed.bmp"))
            return false;
        if (!ui_button_pointer(&scene.widgets, UiButtonPointerEvent::release, point).activate ||
            !probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.paint_stream_builds != paints + 3 || scene.revision != revision ||
            scene.widgets.generation != generation || scene.widgets.layout.revision != layout ||
            g_probe.renderer.content_text_retention.builds != text_rebuilds)
            return false;
        ui_button_pointer(&scene.widgets, UiButtonPointerEvent::cancel, {});
        if (!probe_present(&g_probe.renderer, kDefaultTheme))
            return false;
        const u32 retained = g_probe.renderer.paint_stream_builds;
        if (!probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.paint_stream_builds != retained)
            return false;
        log_write(LogLevel::info, "probe", "settings_native_pointer_hover_press_retention_passed");
    }
    {
        // Real, bounded native overlays keep valid borrowed storage while
        // Settings owns the screen. Background revisions must not enter its
        // text stream, stamp, edit measurement, or glyph visibility guard.
        const u32 hidden_paints = g_probe.renderer.paint_stream_builds;
        UiImageViewer hidden_image{};
        PopupScene hidden_popup{};
        PopupItem popup_items[1]{};
        PopupRowWidget popup_rows[1]{};
        UiWidget popup_widgets[8]{};
        UiLayoutNode popup_nodes[8]{};
        const PopupItem popup_item{utf8_literal("Hidden background menu"), 1, popup_item_enabled};
        TextFormScene hidden_form{};
        TextFormSurface hidden_surface{};
        ProfileScene hidden_profile{};
        UserProfile hidden_profile_data{};
        hidden_profile_data.user_id = 1;
        hidden_profile.measure = &g_probe.text_measure_cache;
        MessageActionBar hidden_bar{};
        ReactionViewer hidden_reactions{};
        ReactionViewerUser reaction_users[1]{};
        char reaction_text[64]{};
        MessageRichStore reaction_store{};
        MessageRichRecord reaction_record[1]{};
        u16 reaction_order[1]{};
        u8 reaction_names[64]{};
        struct RestoreHidden {
            const UiImageViewer* image;
            const PopupScene* popup;
            TextFormSurface* form;
            ProfileScene* profile;
            const MessageActionBar* bar;
            const ReactionViewer* reactions;
            TextFormScene& form_scene;
            TextFormSurface& surface;
            ProfileScene& profile_scene;
            u32 appearance, navigation, messages, media, preview, composer;
            u64 hover;
            bool message_text_active;
            ~RestoreHidden() noexcept {
                auto& renderer = g_probe.renderer;
                renderer.image_viewer = image;
                renderer.popup_scene = popup;
                renderer.text_form = form;
                renderer.profile_scene = profile;
                renderer.message_action_bar = bar;
                renderer.reaction_viewer = reactions;
                g_probe.users.appearance.revision = appearance;
                g_probe.navigation.revision = navigation;
                g_probe.message_scene.data_updates = messages;
                g_probe.message_scene.widgets.hover_started_ms = hover;
                g_probe.media_atlas.insertions = media;
                g_probe.preview_media_atlas.insertions = preview;
                g_probe.message_composer_scene.observed_revision = composer;
                g_probe.message_text.active = message_text_active;
                text_form_surface_release(&surface);
                text_form_release(&form_scene);
                profile_scene_release(&profile_scene);
            }
        } hidden_restore{g_probe.renderer.image_viewer,
                         g_probe.renderer.popup_scene,
                         g_probe.renderer.text_form,
                         g_probe.renderer.profile_scene,
                         g_probe.renderer.message_action_bar,
                         g_probe.renderer.reaction_viewer,
                         hidden_form,
                         hidden_surface,
                         hidden_profile,
                         g_probe.users.appearance.revision,
                         g_probe.navigation.revision,
                         g_probe.message_scene.data_updates,
                         g_probe.media_atlas.insertions,
                         g_probe.preview_media_atlas.insertions,
                         g_probe.message_composer_scene.observed_revision,
                         g_probe.message_scene.widgets.hover_started_ms,
                         g_probe.message_text.active};
        const TextFormContent form_content{utf8_literal("Hidden background form"),
                                           {},
                                           utf8_literal("Value"),
                                           {},
                                           utf8_literal("Cancel"),
                                           utf8_literal("Save"),
                                           utf8_literal("Borrowed text"),
                                           0,
                                           0,
                                           true,
                                           false};
        if (!ui_image_viewer_open(&hidden_image, viewport(), {64, 64}) ||
            !popup_scene_initialize(&hidden_popup, {popup_items, 1}, {popup_rows, 1}, {popup_widgets, 8},
                                    {popup_nodes, 8}) ||
            !popup_scene_open(&hidden_popup, PopupKind::context_menu, 1, {&popup_item, 1}, {20, 20, 60, 60},
                              viewport(), 240)
                 .ok() ||
            !text_form_update(&hidden_form, form_content, viewport(), &g_probe.text_measure_cache) ||
            !profile_scene_open(&hidden_profile, &hidden_profile_data, &g_probe.chat_state, &g_probe.users, 1,
                                {20, 20, 60, 60}, viewport()) ||
            !reaction_viewer_initialize(&hidden_reactions, {reaction_users, 1}, {reaction_text, 64}) ||
            !message_rich_store_initialize(&reaction_store, {reaction_record, 1}, {reaction_order, 1},
                                           {reaction_names, 64}))
            return false;
        const auto reaction_name = utf8_literal("Hidden");
        copy_bytes(reaction_names, reaction_name.data, reaction_name.bytes);
        reaction_store.text_used = reaction_name.bytes;
        reaction_record[0].occupied = true;
        reaction_record[0].message_id = 1;
        static MessageRichReaction reaction_items[1];
        reaction_record[0].reactions = reaction_items;
        reaction_record[0].reaction_count = 1;
        reaction_record[0].reactions[0] = {19, {0, 6, 0}, 1, 1};
        if (!reaction_viewer_open(&hidden_reactions, 0, 1, 1, reaction_store, reaction_record[0],
                                  viewport())
                 .ok())
            return false;
        hidden_bar.owner = {&hidden_popup.widgets, {0, hidden_popup.widgets.generation}};
        hidden_bar.message_id = 1;
        hidden_bar.active = true;
        hidden_bar.item_count = kMessageActionBarItemCount;
        hidden_bar.rectangle = hidden_bar.clip = {20, 20, 220, 52};
        for (u32 i = 0; i < kMessageActionBarQuickReactionCount; ++i)
            hidden_bar.items[i] = {{20.f + i * 28, 20, 48.f + i * 28, 48},
                                   utf8_literal("H"),
                                   0,
                                   MessageActionBarAction::quick_reaction_0,
                                   MessageActionBarItemKind::quick_reaction};
        hidden_surface.scene = &hidden_form;
        const auto settings_stamp = renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme);
        const auto composer_layout = g_probe.renderer.composer_layout;
        const auto popup_layout = g_probe.renderer.popup_edit_layout;
        const auto profile_layout = g_probe.renderer.profile_edit_layout;
        const auto edit_selection = g_probe.renderer.edit_selection_owner;
        const u32 retained_builds = g_probe.renderer.content_text_retention.builds;
        const u32 text_rebuilds = g_probe.text.rebuilds, quads = g_probe.text.quad_count,
                  batches = g_probe.text.batch_count;
        const u32 stale_suppressions = g_probe.renderer.stale_interaction_text_suppressions;
        g_probe.renderer.image_viewer = &hidden_image;
        g_probe.renderer.popup_scene = &hidden_popup;
        // An active bound form is now a foreground Settings confirmation.
        // This unrelated background form stays unbound; Overview's dedicated
        // fixture verifies the foreground modal paint, glyphs and focus.
        g_probe.renderer.profile_scene = &hidden_profile;
        g_probe.renderer.message_action_bar = &hidden_bar;
        g_probe.renderer.reaction_viewer = &hidden_reactions;
        ++g_probe.users.appearance.revision;
        ++g_probe.navigation.revision;
        ++g_probe.message_scene.data_updates;
        ++g_probe.message_scene.widgets.hover_started_ms;
        ++g_probe.message_composer_scene.observed_revision;
        ++g_probe.media_atlas.insertions;
        ++g_probe.preview_media_atlas.insertions;
        g_probe.message_text.active = false;
        if (!renderer_presentation_stamp_equal(
                settings_stamp, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)) ||
            !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.content_text_retention.builds != retained_builds ||
            g_probe.text.rebuilds != text_rebuilds || g_probe.text.quad_count != quads ||
            g_probe.text.batch_count != batches ||
            g_probe.renderer.interaction_text_active_mask != (1u << 6) || hidden_surface.layout.active ||
            !bytes_equal(&composer_layout, &g_probe.renderer.composer_layout, sizeof(composer_layout)) ||
            !bytes_equal(&popup_layout, &g_probe.renderer.popup_edit_layout, sizeof(popup_layout)) ||
            !bytes_equal(&profile_layout, &g_probe.renderer.profile_edit_layout, sizeof(profile_layout)) ||
            g_probe.renderer.edit_selection_owner != edit_selection ||
            !probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.paint_stream_builds != hidden_paints)
            return false;
        ++hidden_popup.revision;
        ++hidden_image.revision;
        ++hidden_form.revision;
        ++hidden_profile.revision;
        ++hidden_bar.revision;
        ++hidden_reactions.revision;
        if (!settings_pixels(scene, false) ||
            g_probe.renderer.stale_interaction_text_suppressions != stale_suppressions ||
            !renderer_presentation_stamp_equal(
                settings_stamp, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)))
            return false;
        log_write(LogLevel::info, "probe",
                  "settings_exclusive_hidden_overlays_text_paint_retained_pixels_passed");
    }
    // Theme-only changes still invalidate the visible Settings text and paint.
    const Theme amoled_theme = ui_theme_amoled();
    const u32 theme_text_builds = g_probe.renderer.content_text_retention.builds;
    const u32 theme_paints = g_probe.renderer.paint_stream_builds;
    if (!renderer_rebuild_content_text(&g_probe.renderer, amoled_theme) ||
        g_probe.renderer.content_text_retention.builds <= theme_text_builds ||
        !probe_present(&g_probe.renderer, amoled_theme) ||
        g_probe.renderer.paint_stream_builds <= theme_paints ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    const u32 scene_text_builds = g_probe.renderer.content_text_retention.builds;
    const u32 scene_paints = g_probe.renderer.paint_stream_builds;
    appearance_controls[0].selected_choice = 1;
    ++appearance.revision;
    if (!ui_settings_scene_update(&scene, &registry, viewport()) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.content_text_retention.builds <= scene_text_builds ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream_builds <= scene_paints ||
        !renderer_rebuild_content_text(&g_probe.renderer, amoled_theme) ||
        !probe_present(&g_probe.renderer, amoled_theme) || !settings_pixels(scene, true) ||
        !probe_capture_frame(L"settings-amoled.bmp", amoled_theme))
        return false;
    bool black_background = false;
    for (u32 i = 0; i < g_probe.renderer.paint_stream.count; ++i) {
        const auto& quad = g_probe.renderer.paint_stream.quads[i];
        black_background =
            black_background || (quad.left == 0 && quad.top == 0 && quad.right == viewport().right &&
                                 quad.bottom == viewport().bottom && quad.fill.red == 0 &&
                                 quad.fill.green == 0 && quad.fill.blue == 0 && quad.fill.alpha == 1);
    }
    if (!black_background)
        return false;
    g_probe.failure_stage = 247;
    const auto allocation = scene.storage.arena.base;
    if (!ui_settings_scene_select(&scene, plugin_key) ||
        !ui_settings_scene_focus(&scene, {UiSettingsTargetKind::control, plugin_key, 70}) ||
        scene.content_offset < 4000 || scene.widgets.count >= 70 ||
        scene.storage.arena.base != allocation ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"settings-plugin-scrolled.bmp"))
        return false;
    g_probe.failure_stage = 248;
    const auto resize_stamp = renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme);
    const u32 resize_text_builds = g_probe.renderer.content_text_retention.builds;
    const u32 resize_paints = g_probe.renderer.paint_stream_builds;
    const u32 narrow_width = static_cast<u32>(480.0f * g_probe.renderer.dpi / 96.0f);
    const u32 narrow_height = static_cast<u32>(360.0f * g_probe.renderer.dpi / 96.0f);
    if (!renderer_resize(&g_probe.renderer, narrow_width, narrow_height) ||
        !ui_settings_scene_update(&scene, &registry, viewport()) ||
        !ui_settings_scene_focus(&scene, {UiSettingsTargetKind::control, plugin_key, 70}) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.content_text_retention.builds <= resize_text_builds ||
        renderer_presentation_stamp_equal(
            resize_stamp, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream_builds <= resize_paints ||
        !probe_capture_frame(L"settings-compact.bmp"))
        return false;
    if (ui_page_registry_unregister_owner(&registry, 77) != 1 ||
        ui_settings_scene_focused(scene).kind != UiSettingsTargetKind::none ||
        !ui_settings_scene_update(&scene, &registry, viewport()) ||
        !ui_page_key_equal(scene.selected_page, accessibility_key) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"settings-owner-removed.bmp"))
        return false;
    renderer_bind_settings_scene(&g_probe.renderer, nullptr);
    ui_settings_scene_close(&scene);
    if (scene.storage.arena.base || scene.registry ||
        !renderer_resize(&g_probe.renderer, old_width, old_height) || !synchronize_probe_shell_layout() ||
        !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    log_write(LogLevel::info, "probe",
              "settings_dark_amoled_virtualized_owner_rebind_resize_hardware_passed");
    return true;
}

bool probe_autocomplete_identity() noexcept {
    g_probe.failure_stage = 145;
    const float scale = 96.0f / g_probe.renderer.dpi;
    const UiRect viewport{0, 0, g_probe.renderer.width * scale, g_probe.renderer.height * scale};
    const auto* surface = ui_widget_manager_layout_node(g_probe.message_composer_scene.widgets,
                                                        g_probe.message_composer_scene.surface);
    if (!surface)
        return false;
    const UiRect anchor = surface->rectangle;
    PopupItem item{.label = utf8_literal("𝕼𝖚𝖎𝖑𝖑𝕱𝖊𝖗𝖓"),
                   .action = 1,
                   .flags = popup_item_enabled | popup_item_avatar,
                   .media_id = 9917,
                   .detail = utf8_literal("quillfern"),
                   .media_variant = 117,
                   .status = 1};
    u8 pixels[24 * 24 * 4];
    for (u32 i = 0; i < 24 * 24; ++i) {
        pixels[i * 4] = 80;
        pixels[i * 4 + 1] = (i % 24 < 12) ? 140 : 90;
        pixels[i * 4 + 2] = 220;
        pixels[i * 4 + 3] = 255;
    }
    const auto key = media_asset_user_avatar(item.media_id, item.media_variant, false);
    if (!renderer_upload_media_rgba8(&g_probe.renderer, key, 24, 24, 96, {pixels, sizeof(pixels)}).ok() ||
        !popup_scene_open(&g_probe.popup_scene, PopupKind::autocomplete, 77, {&item, 1}, anchor, viewport,
                          ui_rect_width(anchor), true)
             .ok() ||
        !renderer_bind_popup_scene(&g_probe.renderer, &g_probe.popup_scene) ||
        !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    PopupMediaCommand media{};
    g_probe.failure_stage = 146;
    UiTextCommand commands[4]{};
    UiTextCommandStream text{};
    UiTextOutput text_output{&text, nullptr};
    if (popup_media_presentation_resolve(g_probe.popup_scene, 0, &media) !=
            PopupMediaPresentationStatus::ready ||
        !media.avatar || media.media_variant != 117 ||
        !ui_text_command_stream_initialize(&text, {commands, 4}) ||
        !popup_text_presentation_build_output(g_probe.popup_scene, kDefaultTheme, &text_output) || text.count != 2 ||
        !utf8_equal(commands[0].text, item.detail) ||
        commands[0].horizontal_alignment != UiTextAlignment::end ||
        !utf8_equal(commands[1].text, item.label) ||
        commands[1].rectangle.right >= commands[0].rectangle.left ||
        commands[1].rectangle.left <= media.rectangle.right || !g_probe.renderer.media_quad_count)
        return false;
    const auto& quad = g_probe.renderer.media_draw.quad_data[g_probe.renderer.media_quad_count - 1];
    g_probe.failure_stage = 147;
    const ProbePixelSample user_status{{media.rectangle.right + 4.5f, media.rectangle.bottom - 3.5f},
                                       kDefaultTheme.tokens.status.online,
                                       3};
    if (quad.left != media.rectangle.left || quad.right != media.rectangle.right ||
        !probe_exact_pixels({&user_status, 1}) || !probe_capture_frame(L"autocomplete.bmp"))
        return false;
    popup_scene_close(&g_probe.popup_scene);
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    log_write(LogLevel::info, "probe",
              "autocomplete_avatar_status_and_distinct_name_columns_hardware_passed");
    {
        g_probe.failure_stage = 148;
        // Application artwork has no presence. Once resident, its alpha reveals
        // the actual rail/row surface rather than the loading placeholder.
        PopupItem apps[]{{.label = utf8_literal("Test app"),
                          .action = 200000,
                          .flags = popup_item_enabled | popup_item_command_category | popup_item_avatar,
                          .media_id = 9918,
                          .media_variant = 118},
                         {.label = utf8_literal("/wordle"),
                          .action = 100000,
                          .flags = popup_item_enabled | popup_item_command_row | popup_item_avatar,
                          .media_id = 9918,
                          .detail = utf8_literal("Play wordle"),
                          .media_variant = 118}};
        if (!popup_scene_open(&g_probe.popup_scene, PopupKind::autocomplete, 77, {apps, 2}, anchor,
                              viewport, ui_rect_width(anchor), true)
                 .ok() ||
            !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
            return false;
        ProbePixelSample missing[2]{};
        PopupMediaCommand artwork[2]{};
        for (u32 i = 0; i < 2; ++i) {
            if (popup_media_presentation_resolve(g_probe.popup_scene, i, &artwork[i]) !=
                PopupMediaPresentationStatus::ready)
                return false;
            const auto r = artwork[i].rectangle;
            missing[i] = {
                {(r.left + r.right) * .5f, (r.top + r.bottom) * .5f}, kDefaultTheme.interactive, 3};
        }
        if (!probe_exact_pixels({missing, 2}))
            return false;
        for (u32 y = 0; y < 24; ++y)
            for (u32 x = 0; x < 24; ++x) {
                const u32 at = (y * 24 + x) * 4;
                const u8 alpha = x < 8 ? 0 : x < 16 ? 128 : 255;
                // Original premultiplied magenta artwork: transparent, half,
                // opaque thirds, all sampled well inside the circular mask.
                pixels[at] = pixels[at + 2] = static_cast<u8>((240 * alpha + 127) / 255);
                pixels[at + 1] = static_cast<u8>((80 * alpha + 127) / 255);
                pixels[at + 3] = alpha;
            }
        if (!renderer_upload_media_rgba8(&g_probe.renderer, media_asset_user_avatar(9918, 118, false), 24,
                                         24, 96, {pixels, sizeof(pixels)})
                 .ok())
            return false;
        for (u32 selected = 0; selected < 2; ++selected) {
            g_probe.popup_scene.selected_index = selected;
            ++g_probe.popup_scene.revision;
            if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
                return false;
            UiPoint points[10]{};
            u8 actual[10][4]{};
            for (u32 i = 0; i < 2; ++i) {
                const auto r = artwork[i].rectangle;
                const float y = (r.top + r.bottom) * .5f, w = ui_rect_width(r);
                points[i * 5] = {r.left - 4, y};
                points[i * 5 + 1] = {r.left + w * .25f, y};
                points[i * 5 + 2] = {r.left + w * .5f, y};
                points[i * 5 + 3] = {r.left + w * .75f, y};
                points[i * 5 + 4] = {r.right + 4.5f, r.bottom - 6};
            }
            if (!probe_read_pixels(kDefaultTheme, {points, 10}, actual))
                return false;
            // A selected rail entry sits on its own selection plate, which
            // only the row's surface (full width) shares with its outside.
            for (u32 i = selected == 0 ? 1u : 0u; i < 2; ++i)
                for (u32 channel = 0; channel < 3; ++channel) {
                    const u32 background = actual[i * 5][channel];
                    const u32 opaque = channel == 1 ? 80 : 240;
                    const u32 half = (opaque * 128 + 127) / 255 + (background * 127 + 127) / 255;
                    const u32 expected[]{background, half, opaque, background};
                    for (u32 sample = 0; sample < 4; ++sample) {
                        const u32 value = actual[i * 5 + sample + 1][channel];
                        if (maximum(value, expected[sample]) - minimum(value, expected[sample]) > 3)
                            return false;
                    }
                }
        }
        popup_scene_close(&g_probe.popup_scene);
    }
    {
        // The discovery browser: clock, app and Built-in rail; section
        // headers carry the pictures, rows don't (Frequently Used rows show their
        // app); the selected row adds its usage summary.
        PopupItem picker[]{
            {utf8_literal("Frequently Used"), 200000, popup_item_enabled | popup_item_command_category},
            {.label = utf8_literal("Test app"), .action = 200001,
             .flags = popup_item_enabled | popup_item_command_category | popup_item_avatar | popup_item_checked,
             .media_id = 9918, .media_variant = 118},
            {utf8_literal("Built-in"), 200002,
             popup_item_enabled | popup_item_command_category | popup_item_separator_before},
            {utf8_literal("Frequently Used"), 600000, popup_item_command_section},
            {.label = utf8_literal("/wordle"), .action = 100000,
             .flags = popup_item_enabled | popup_item_command_row | popup_item_avatar, .media_id = 9918,
             .detail = utf8_literal("Play wordle\x1FTest app\x1F"), .media_variant = 118},
            {utf8_literal("/shrug"), 100001, popup_item_enabled | popup_item_command_row},
            {.label = utf8_literal("Test app"), .action = 600001, .flags = popup_item_command_section | popup_item_avatar,
             .media_id = 9918, .media_variant = 118},
            {utf8_literal("/echo"), 100002, popup_item_enabled | popup_item_command_row},
            {utf8_literal("/wordle"), 100003, popup_item_enabled | popup_item_command_row},
            {utf8_literal("Built-in"), 600002, popup_item_command_section},
            {utf8_literal("/gif"), 100004, popup_item_enabled | popup_item_command_row}};
        picker[0].presentation = ui_menu_row_presentation(UiIcon::recent);
        picker[2].presentation = ui_menu_row_presentation(UiIcon::command);
        picker[3].presentation = picker[0].presentation;
        picker[5].presentation = picker[2].presentation;
        picker[5].detail = utf8_literal("Appends \xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF to your message.\x1F" "Built-in\x1F");
        picker[7].detail = utf8_literal("Echo\x1FTest app\x1Ftext\x1E+2 optional");
        picker[8].detail = utf8_literal("Play wordle\x1FTest app\x1F");
        picker[9].presentation = picker[2].presentation;
        picker[10].detail = utf8_literal("Search Animated GIFs on the Web\x1F" "Built-in\x1Fquery");
        PopupItem results[]{
            {utf8_literal("COMMANDS MATCHING /W"), 599999, popup_item_command_section}, picker[4], picker[7]};
        results[2].flags |= popup_item_avatar;
        results[2].media_id = 9918;
        results[2].media_variant = 118;
        PopupItem failed[]{picker[0], picker[1], picker[2],
                           {utf8_literal("Couldn't load commands"), 99999, popup_item_command_row}};
        PopupItem loading[]{{utf8_literal("COMMANDS MATCHING /W"), 599999, popup_item_command_section}, picker[4],
                            popup_placeholder_row(800000, 0, popup_item_command_row),
                            popup_placeholder_row(800001, 1, popup_item_command_row),
                            popup_placeholder_row(800002, 2, popup_item_command_row)};
        loading[1].flags |= popup_item_avatar;
        const struct {
            ConstSlice<PopupItem> items;
            u32 selected;
            float scroll_extent;
            const wchar_t* capture;
        } passes[]{{{picker, 11}, 7, 900, L"commands-picker.bmp"},
                   {{picker, 11}, 0, 900, L"commands-picker-rail-focus.bmp"},
                   {{results, 3}, 1, 0, L"commands-query.bmp"},
                   {{loading, 5}, ~0u, 0, L"commands-loading.bmp"},
                   {{failed, 4}, ~0u, 0, L"commands-error.bmp"}};
        for (const auto& pass : passes) {
            if (!popup_scene_open(&g_probe.popup_scene, PopupKind::autocomplete, 77, pass.items, anchor, viewport,
                                  ui_rect_width(anchor), true)
                     .ok())
                return false;
            g_probe.popup_scene.selected_index = pass.selected;
            g_probe.popup_scene.picker_scroll_extent = pass.scroll_extent;
            ++g_probe.popup_scene.revision;
            if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
                return false;
            UiTextCommand text_commands[24]{};
            UiTextCommandStream stream{};
            UiTextOutput stream_output{&stream, nullptr};
            UiRect rectangles[24]{};
            if (!ui_text_command_stream_initialize(&stream, {text_commands, 24}) ||
                !popup_text_presentation_build_output(g_probe.popup_scene, kDefaultTheme, &stream_output,
                                                      g_probe.renderer.message_measure_cache) ||
                !stream.count)
                return false;
            u32 visible = 0;
            for (u32 i = 0; i < stream.count; ++i)
                if (ui_rect_has_area(text_commands[i].clip))
                    rectangles[visible++] = ui_rect_intersection(text_commands[i].rectangle, text_commands[i].clip);
            if (!probe_capture_frame(pass.capture) || !probe_text_pixels({rectangles, visible}, 70))
                return false;
        }
    }
    popup_scene_close(&g_probe.popup_scene);
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    PopupItem emojis[]{{utf8_literal(":saluting_face:"), 1, popup_item_enabled},
                       {utf8_literal(":sad_but_relieved_face:"), 2, popup_item_enabled},
                       {utf8_literal(":santa:"), 3, popup_item_enabled}};
    if (!popup_scene_open(&g_probe.popup_scene, PopupKind::autocomplete, 77, {emojis, 3}, anchor, viewport,
                          ui_rect_width(anchor), true)
             .ok() ||
        !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"emoji-autocomplete.bmp"))
        return false;
    popup_scene_close(&g_probe.popup_scene);
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    log_write(LogLevel::info, "probe", "command_picker_and_error_text_pixels_passed");
    return true;
}

bool probe_glyph_atlas_gutters() noexcept {
    g_probe.failure_stage = 148;
    GlyphAtlasGpu gpu{};
    auto* device = g_probe.renderer.graphics.device;
    auto* context = g_probe.renderer.graphics.context;
    if (!glyph_atlas_gpu_initialize(&gpu, device, 32, 32, 8).ok())
        return false;
    GlyphPathCommand commands[] = {
        {0, 0, 0, 0, 0, 0, GlyphPathCommandKind::begin_filled},
        {1, 0, 0, 0, 0, 0, GlyphPathCommandKind::line},
        {1, 1, 0, 0, 0, 0, GlyphPathCommandKind::line},
        {0, 1, 0, 0, 0, 0, GlyphPathCommandKind::line},
        {0, 0, 0, 0, 0, 0, GlyphPathCommandKind::end_closed},
    };
    GlyphOutline outline{};
    glyph_outline_initialize(&outline, {commands, 5});
    outline.count = 5;
    outline.has_bounds = true;
    outline.maximum_x = outline.maximum_y = 1;
    GlyphRasterBounds bounds{};
    GlyphAtlasAllocation allocation{};
    allocation.x = allocation.y = 4;
    const float poison[4] = {1, 1, 1, 1};
    context->ClearUnorderedAccessViewFloat(gpu.texture_uav, poison);
    D3D11_TEXTURE2D_DESC description{};
    gpu.texture->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    bool valid = SUCCEEDED(device->CreateTexture2D(&description, nullptr, &staging));
    u8 previous[32 * 32]{};
    for (u32 pass = 0; valid && pass < 3; ++pass) {
        const u32 extent = pass == 2 ? 4 : 8;
        const u16 padding = pass == 0 ? 0 : 1;
        valid = glyph_raster_bounds(outline, static_cast<float>(extent), 1, &bounds);
        allocation.width = bounds.width;
        allocation.height = bounds.height;
        GlyphRasterBatch batch(&gpu, context);
        valid = valid && batch.add(outline, bounds, allocation, static_cast<float>(extent), padding).ok() &&
                batch.finish();
        if (!valid)
            break;
        context->CopyResource(staging, gpu.texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        valid = SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
        if (!valid)
            break;
        for (u32 y = 0; y < 32; ++y)
            for (u32 x = 0; x < 32; ++x) {
                const u8 actual = static_cast<const u8*>(mapped.pData)[y * mapped.RowPitch + x];
                const bool inside = x >= 4u - padding && y >= 4u - padding &&
                                    x < 4u + bounds.width + padding && y < 4u + bounds.height + padding;
                const bool ink = x >= 5 && y >= 5 && x < 5 + extent && y < 5 + extent;
                const u8 expected = inside ? (ink ? 255 : 0) : pass ? previous[y * 32 + x] : 255;
                valid &= actual == expected;
                previous[y * 32 + x] = actual;
            }
        context->Unmap(staging, 0);
        // The control pass leaves opaque pixels at the sampling border; the
        // real raster clears it, including when a smaller glyph reuses it.
        valid &= previous[4 * 32 + 3] == (pass ? 0 : 255);
    }
    if (staging)
        staging->Release();
    glyph_atlas_gpu_release(&gpu);
    if (valid)
        log_write(LogLevel::info, "probe", "glyph_gutters_poisoned_and_reused_hardware_readback_passed");
    return valid;
}

// Compact message display and group spacing (settings-proto
// message_display_compact plus the local spacing slider).
bool probe_message_display() noexcept {
    auto& scene = g_probe.message_scene;
    const MessageSceneDisplay restore = scene.display;
    const auto apply = [&](MessageSceneDisplay display) noexcept {
        message_scene_set_display(&scene, display);
        return synchronize_probe_shell_layout() && rebuild_probe_content_text() &&
               probe_present(&g_probe.renderer, kDefaultTheme);
    };
    const auto first_header = [&]() noexcept -> const MessageSceneRowWidget* {
        for (u32 i = 0; i < scene.count; ++i) {
            const auto* row = message_scene_layout_node(scene, scene.rows[i].widget);
            if (!scene.rows[i].grouped && !scene.rows[i].forwarded_snapshot && row &&
                ui_rect_has_area(ui_rect_intersection(row->rectangle, scene.viewport)))
                return &scene.rows[i];
        }
        return nullptr;
    };
    const auto command_for = [&](UiWidgetHandle owner) noexcept -> const MessageTextCommand* {
        for (u32 i = 0; i < g_probe.message_text.command_count; ++i) {
            const auto& command = g_probe.message_text.commands[i];
            if (command.owner.manager == &scene.widgets && command.owner.handle.slot == owner.slot &&
                command.owner.handle.generation == owner.generation)
                return &command;
        }
        return nullptr;
    };
    bool valid = apply({0, true});
    const auto* row = valid ? first_header() : nullptr;
    const auto* record = row ? message_store_resolve(g_probe.message_view.store, row->message) : nullptr;
    const auto* user = record ? user_store_find(g_probe.users, record->author_id) : nullptr;
    const Utf8View name = user ? user_store_name(g_probe.users, *user) : Utf8View{};
    const auto* row_node = row ? message_scene_layout_node(scene, row->widget) : nullptr;
    const auto* author = row ? message_scene_layout_node(scene, row->author) : nullptr;
    const auto* content = row ? message_scene_layout_node(scene, row->content) : nullptr;
    const auto* body = row ? command_for(row->content) : nullptr;
    const auto* stamp = row ? command_for(row->avatar) : nullptr;
    const float em = scene.theme_styles && scene.theme_styles->ready ? scene.theme_styles->typography.body_size : 16.f;
    const float indent = content && row_node ? content->rectangle.left - row_node->rectangle.left -
                                                   ui_theme_metrics(scene.theme_styles).message_left
                                             : 0.f;
    const float column =
        em * (message_scene_compact_clock_em(MessageSceneClock::meridiem) + kMessageSceneCompactGapEm);
    valid = valid && row && name.bytes && author && ui_rect_height(author->rectangle) == 0.f &&
            !message_scene_avatar_visible(scene, *row) && indent > column - .5f && indent < column + .5f && body &&
            body->text.bytes > name.bytes && bytes_equal(body->text.data, name.data, name.bytes) &&
            body->text.data[name.bytes] == ' ' && stamp && stamp->text.bytes &&
            stamp->horizontal_alignment == UiTextAlignment::end && probe_capture_frame(L"user-settings-compact.bmp");
    // The fixture gives every message its own author; make the message after the first header a follow-up of
    // that author so grouped compact rows are exercised, and restore it afterwards.
    MessageRecord* follower = nullptr;
    u64 saved_author = 0, saved_time = 0;
    for (u32 i = 0; valid && row && !follower && i + 1 < scene.count; ++i) {
        if (scene.rows[i].widget.slot != row->widget.slot || scene.rows[i].widget.generation != row->widget.generation)
            continue;
        const auto* head = message_store_resolve(g_probe.message_view.store, scene.rows[i].message);
        auto* next = const_cast<MessageRecord*>(message_store_resolve(g_probe.message_view.store, scene.rows[i + 1].message));
        if (!head || !next)
            continue;
        follower = next;
        saved_author = next->author_id;
        saved_time = next->timestamp_ms;
        next->author_id = head->author_id;
        next->timestamp_ms = head->timestamp_ms + 1'000;
        ++next->revision;
        message_scene_invalidate_geometry(&scene);
        valid = synchronize_probe_shell_layout() && rebuild_probe_content_text() &&
                probe_present(&g_probe.renderer, kDefaultTheme);
    }
    // A grouped row names its author too; its timestamp appears on hover only.
    u32 grouped_index = scene.count;
    for (u32 i = 0; valid && i < scene.count; ++i) {
        const auto* node = message_scene_layout_node(scene, scene.rows[i].widget);
        if (scene.rows[i].grouped && !scene.rows[i].forwarded_snapshot && node &&
            ui_rect_has_area(ui_rect_intersection(node->rectangle, scene.viewport))) {
            grouped_index = i;
            break;
        }
    }
    valid = valid && (!follower || grouped_index < scene.count);
    if (valid && grouped_index < scene.count) {
        const auto handle = scene.rows[grouped_index].widget;
        const auto* inline_name = command_for(scene.rows[grouped_index].content);
        valid = inline_name && inline_name->styles.count &&
                (inline_name->styles[0].flags & text_style_inline_role_color) &&
                !command_for(scene.rows[grouped_index].avatar) &&
                ui_widget_manager_hover(&scene.widgets, handle) && rebuild_probe_content_text() &&
                probe_present(&g_probe.renderer, kDefaultTheme) && command_for(scene.rows[grouped_index].avatar) &&
                probe_capture_frame(L"user-settings-compact-grouped-hover.bmp") &&
                ui_widget_manager_hover(&scene.widgets, ui_widget_invalid_handle()) && rebuild_probe_content_text() &&
                probe_present(&g_probe.renderer, kDefaultTheme) && !command_for(scene.rows[grouped_index].avatar) &&
                probe_capture_frame(L"user-settings-compact-grouped.bmp");
    }
    if (follower) {
        follower->author_id = saved_author;
        follower->timestamp_ms = saved_time;
        ++follower->revision;
        message_scene_invalidate_geometry(&scene);
    }
    if (!valid)
        log_write(LogLevel::error, "probe", "compact_geometry_failed");
    // The 24-hour clock narrows the column; the name follows it closely.
    valid = valid && apply({0, true, MessageSceneClock::short_time}) &&
            probe_capture_frame(L"user-settings-compact-24-hour.bmp") && apply({0, true});
    valid = valid && apply({5, true});
    row = valid ? first_header() : nullptr;
    row_node = row ? message_scene_layout_node(scene, row->widget) : nullptr;
    valid = valid && row_node && row_node->spec.style.padding.top >= 25.f &&
            probe_capture_frame(L"user-settings-compact-spacing.bmp");
    valid = valid && apply({1, false});
    row = valid ? first_header() : nullptr;
    row_node = row ? message_scene_layout_node(scene, row->widget) : nullptr;
    author = row ? message_scene_layout_node(scene, row->author) : nullptr;
    valid = valid && row_node && author && ui_rect_height(author->rectangle) > 0.f &&
            message_scene_avatar_visible(scene, *row) && row_node->spec.style.padding.top >= 1.f &&
            row_node->spec.style.padding.top < 2.f && probe_capture_frame(L"user-settings-cozy-tight.bmp");
    const bool restored = apply(restore);
    if (valid)
        log_write(LogLevel::info, "probe", "message_display_compact_and_spacing_passed");
    return valid && restored;
}

namespace plugin_surface_probe {
constexpr u64 kOwner = 0x50524F4245000001ull;
u64 g_messages[4];

u32 length(const char* text) noexcept {
    u32 bytes = 0;
    while (text[bytes]) ++bytes;
    return bytes;
}

UiPluginSlotNode node(u64 action, const char* text, u16 icon, u8 kind, u8 tone, u16 badge = 0, u8 state = 0) noexcept {
    UiPluginSlotNode out{};
    out.owner_id = kOwner;
    out.action_id = action;
    out.text = text ? Utf8View{text, static_cast<u32>(length(text))} : Utf8View{};
    out.icon = icon;
    out.kind = kind;
    out.tone = tone;
    out.badge = badge;
    out.state = state;
    return out;
}

// What a plugin runtime would answer from its caches, fixed: the scenes under test only read it.
u32 slot_query(u32 surface, u64, u64, u64, u64, Slice<UiPluginSlotNode> out) noexcept {
    const u16 puzzle = static_cast<u16>(UiIcon::puzzle);
    UiPluginSlotNode nodes[4]{};
    u32 count = 0;
    switch (surface) {
    case kUiPluginSlotServerTop:
        nodes[count++] = node(1, nullptr, puzzle, ui_plugin_node_button, 1, 3);
        nodes[count++] = node(2, nullptr, puzzle, ui_plugin_node_button, 0, 0, kUiPluginSlotActive);
        break;
    case kUiPluginSlotServerBottom:
        nodes[count++] = node(3, nullptr, puzzle, ui_plugin_node_button, 0, 0, kUiPluginSlotDisabled);
        break;
    case kUiPluginSlotComposerToolbar:
        nodes[count++] = node(4, nullptr, puzzle, ui_plugin_node_button, 0);
        nodes[count++] = node(5, nullptr, puzzle, ui_plugin_node_button, 1, 0, kUiPluginSlotActive);
        nodes[count++] = node(6, nullptr, puzzle, ui_plugin_node_button, 0, 5, kUiPluginSlotDisabled);
        break;
    case kUiPluginSlotAccountPanel:
        nodes[count++] = node(7, nullptr, puzzle, ui_plugin_node_button, 0, 2);
        nodes[count++] = node(8, nullptr, puzzle, ui_plugin_node_button, 3, 0, kUiPluginSlotActive);
        break;
    case kUiPluginSlotMessageHover:
        nodes[count++] = node(9, "Plugin one", puzzle, ui_plugin_node_button, 0);
        nodes[count++] = node(10, "Plugin two", puzzle, ui_plugin_node_button, 4, 0, kUiPluginSlotDisabled);
        break;
    case kUiPluginSlotChannelRow:
        nodes[count++] = node(0, "new", kUiPluginSlotNoIcon, ui_plugin_node_badge, 2);
        break;
    case kUiPluginSlotGuildRail:
        nodes[count++] = node(0, "mark", puzzle, ui_plugin_node_icon, 3);
        break;
    default: return 0;
    }
    const u32 used = minimum(count, out.count);
    for (u32 i = 0; i < used; ++i) out[i] = nodes[i];
    return used;
}

// Message 0 carries every kind of addition, 1 is collapsed, 2 replaced, 3 has its media blurred.
bool render_query(u64 message_id, u32, UiPluginRenderDecoration* out) noexcept {
    u32 which = 4;
    for (u32 i = 0; i < 4; ++i)
        if (g_messages[i] == message_id) which = i;
    const auto text = [](const char* value) noexcept { return Utf8View{value, static_cast<u32>(length(value))}; };
    switch (which) {
    case 0:
        out->name_prefix = text("[plugin]");
        out->name_suffix = text("(alias)");
        out->name_tone = kUiPluginToneRole;
        out->badge_count = 1;
        out->badges[0] = {kOwner, 0, text("tag"), {}, {}, static_cast<u16>(UiIcon::puzzle), 2, 1};
        out->accessory_count = 2;
        out->accessories[0] = {kOwner, 0, text("Accessory label"), {}, {}, kUiPluginRenderNoIcon, kUiPluginRenderLabel, 0};
        out->accessories[1] = {kOwner, 11, text("Accessory button"), {}, {}, kUiPluginRenderNoIcon, kUiPluginRenderButton, 0};
        out->has_card = true;
        out->card.owner_id = kOwner;
        out->card.title = text("Translation");
        out->card.text = text("A card is plain text the application wraps to its own width, so a long answer from a plugin never "
                              "overflows the message column or runs off the edge.");
        out->card.caption = text("Translated from French");
        out->card.buttons[0] = {21, text("Hide"), 0};
        out->card.buttons[1] = {22, text("Copy"), 0};
        out->card.button_count = 2;
        out->card.tone = 1;
        return true;
    case 1:
        out->visibility = ui_plugin_render_collapsed;
        out->collapsed_label = text("Hidden by a plugin");
        return true;
    case 2:
        out->replace_content = true;
        out->content = text("This text replaces the message **content**.");
        return true;
    case 3:
        out->media = ui_plugin_media_blur;
        return true;
    default: return false;
    }
}
} // namespace plugin_surface_probe

// Every plugin surface the scenes draw, driven by fixed queries (the runtime's own behaviour is covered by
// MaiCordPluginSurfaceTests): server list tiles, toolbar button states, row and rail badges, the hover bar's
// per-message buttons, and message decorations: name affixes, badges, accessories, a card, a collapsed message,
// replaced content.
bool probe_plugin_surfaces() noexcept {
    using namespace plugin_surface_probe;
    auto& scene = g_probe.message_scene;
    u32 found = 0;
    for (u32 i = 0; i < scene.count && found < 4; ++i) {
        const auto* record = message_store_resolve(g_probe.message_view.store, scene.rows[i].message);
        if (record && !scene.rows[i].forwarded_snapshot) g_messages[found++] = record->id;
    }
    if (found < 4) return false;
    const auto saved_render = g_ui_plugin_render_query;
    const auto saved_slot = g_ui_plugin_slot_query;
    const u32 saved_surfaces = g_ui_plugin_slot_surfaces;
    const bool saved_decorations = scene.plugin_decorations;
    const u64 saved_revision = g_ui_plugin_render_revision;
    g_ui_plugin_render_query = render_query;
    g_ui_plugin_slot_query = slot_query;
    g_ui_plugin_slot_surfaces = ~0u;
    scene.plugin_decorations = true;
    g_ui_plugin_render_revision = saved_revision + 1;
    message_scene_invalidate_geometry(&scene);
    const auto& rail = g_probe.guild_rail_scene;
    bool valid = synchronize_probe_shell_layout() && rebuild_probe_content_text() &&
                 probe_present(&g_probe.renderer, kDefaultTheme);
    valid = valid && rail.plugin_top == 2 && rail.plugin_bottom == 1 && probe_capture_frame(L"plugin-surfaces.bmp");
    // The hover bar lists the message's plugin buttons after its own.
    const auto* row = &scene.rows[0];
    for (u32 i = 0; valid && i < scene.count; ++i)
        if (const auto* record = message_store_resolve(g_probe.message_view.store, scene.rows[i].message);
            record && record->id == g_messages[0])
            row = &scene.rows[i];
    const auto* record = message_store_resolve(g_probe.message_view.store, row->message);
    valid = valid && record &&
            message_action_bar_open(&g_probe.message_action_bar, scene, {record->id, row->widget, row->source_index}, *record).ok() &&
            renderer_bind_message_action_bar(&g_probe.renderer, &g_probe.message_action_bar) &&
            renderer_refresh_interaction_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) && probe_capture_frame(L"plugin-surfaces-hover-bar.bmp");
    message_action_bar_close(&g_probe.message_action_bar);
    renderer_bind_message_action_bar(&g_probe.renderer, nullptr);
    g_ui_plugin_render_query = saved_render;
    g_ui_plugin_slot_query = saved_slot;
    g_ui_plugin_slot_surfaces = saved_surfaces;
    scene.plugin_decorations = saved_decorations;
    g_ui_plugin_render_revision = saved_revision + 2;
    message_scene_invalidate_geometry(&scene);
    const bool restored = synchronize_probe_shell_layout() && rebuild_probe_content_text() &&
                          probe_present(&g_probe.renderer, kDefaultTheme) && rail.plugin_top == 0 && rail.plugin_bottom == 0;
    if (valid && restored) log_write(LogLevel::info, "probe", "plugin_surfaces_passed");
    else log_write(LogLevel::error, "probe", "plugin_surfaces_failed");
    return valid && restored;
}

bool probe_chat_name_styles() noexcept {
    const auto& scene = g_probe.message_scene;
    const MessageSceneRowWidget* row = nullptr;
    for (u32 i = 0; i < scene.count; ++i)
        if (!scene.rows[i].grouped) {
            const auto* author = message_scene_layout_node(scene, scene.rows[i].author);
            if (author && ui_rect_has_area(ui_rect_intersection(author->rectangle, scene.viewport))) {
                row = &scene.rows[i];
                break;
            }
        }
    if (!row)
        return false;
    const auto* record = message_store_resolve(g_probe.message_view.store, row->message);
    if (!record)
        return false;
    UserAppearance style{};
    const auto* old = user_appearance_find(g_probe.users.appearance, record->author_id);
    if (old)
        user_appearance_expand(*old, &style);
    else
        style.user_id = record->author_id;
    const auto original = style;
    style.fields |= appearance_name;
    style.name.font_id = 14;
    style.name.effect_id = 5;
    style.name.color_count = 1;
    style.name.colors[0] = 0xAB66FF;
    bool valid = user_appearance_upsert(&g_probe.users.appearance, style) &&
                 ui_widget_manager_hover(&g_probe.message_scene.widgets, ui_widget_invalid_handle()) &&
                 rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
                 probe_capture_frame(L"chat-name-static.bmp");
    valid = valid && ui_widget_manager_hover(&g_probe.message_scene.widgets, row->widget) &&
            rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            g_probe.renderer.text_effect_animated && probe_capture_frame(L"chat-name-hover.bmp");
    const auto rebuilt = g_probe.renderer.content_text->rebuilds,
               uploaded = g_probe.renderer.text_instance_uploads;
    Sleep(50);
    valid = valid && probe_present(&g_probe.renderer, kDefaultTheme) &&
            g_probe.renderer.content_text->rebuilds == rebuilt &&
            g_probe.renderer.text_instance_uploads == uploaded;
    auto restore = original;
    restore.fields |= appearance_name;
    valid = user_appearance_upsert(&g_probe.users.appearance, restore) &&
            ui_widget_manager_hover(&g_probe.message_scene.widgets, ui_widget_invalid_handle()) &&
            rebuild_probe_content_text() && valid;
    return valid;
}
bool probe_dm_name_styles() noexcept {
    const ShellNavigationRowWidget* row = nullptr;
    u64 peer = 0;
    for (u32 i = 0; i < g_probe.shell_navigation_scene.count; ++i) {
        const auto& candidate = g_probe.shell_navigation_scene.rows[i];
        const auto* channel = chat_state_store_find_channel(g_probe.chat_state, candidate.view.id);
        if (channel && channel->type == 1 && (peer = private_channel_peer(g_probe.chat_state, channel->id))) {
            row = &candidate;
            break;
        }
    }
    if (!row)
        return false;
    UserAppearance style{};
    const auto* old = user_appearance_find(g_probe.users.appearance, peer);
    if (old)
        user_appearance_expand(*old, &style);
    else
        style.user_id = peer;
    const auto original = style;
    style.fields |= appearance_name;
    style.name.font_id = 14;
    style.name.effect_id = 7;
    style.name.color_count = 2;
    style.name.colors[0] = 0xE076FF;
    style.name.colors[1] = 0x69B5D0;
    bool valid =
        user_appearance_upsert(&g_probe.users.appearance, style) &&
        ui_widget_manager_hover(&g_probe.shell_navigation_scene.widgets, ui_widget_invalid_handle()) &&
        rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
        probe_capture_frame(L"dm-name-static.bmp");
    bool still = false;
    for (u32 i = 0; i < g_probe.renderer.text_effect_count; ++i)
        still |= g_probe.renderer.text_effects.materials[i].kind == 7 &&
                 !g_probe.renderer.text_effects.materials[i].animate;
    valid = valid && still &&
            ui_widget_manager_hover(&g_probe.shell_navigation_scene.widgets, row->widget) &&
            rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            g_probe.renderer.text_effect_animated && probe_capture_frame(L"dm-name-hover.bmp");
    auto restore = original;
    restore.fields |= appearance_name;
    valid = user_appearance_upsert(&g_probe.users.appearance, restore) &&
            ui_widget_manager_hover(&g_probe.shell_navigation_scene.widgets, ui_widget_invalid_handle()) &&
            rebuild_probe_content_text() && valid;
    return valid;
}

} // namespace ui_probe_detail

namespace {

bool probe_profile_hover(ProfileScene& scene) noexcept {
    // The full profile's name and username are plain text (as in the official
    // client); the popout's open the full profile.
    if (scene.presentation == ProfilePresentation::modal)
        return true;
    for (u32 kind = 0; kind < 2; ++kind) {
        UiWidgetHandle target{};
        for (u32 i = 0; i < scene.part_count; ++i) {
            const auto& part = scene.parts[i];
            const bool name = part.widget.slot == scene.name_widget.slot &&
                              part.widget.generation == scene.name_widget.generation;
            if (part.action == ProfileAction::expand && part.text.bytes && name == !kind) {
                target = part.widget;
                break;
            }
        }
        const auto* node = ui_widget_manager_layout_node(scene.widgets, target);
        if (!node)
            return false;
        UiRect ink{};
        for (u32 i = 0; i < scene.text_hit_count; ++i)
            if (scene.text_hits[i].owner.slot == target.slot &&
                scene.text_hits[i].owner.generation == target.generation)
                ink = scene.text_hits[i].ink;
        if (!ui_rect_has_area(ink))
            return false;
        const float center_y = (ink.top + ink.bottom) * .5f;
        // Both points are in the name widget, but only one is over painted text
        // (the popout sizes its name and username to their text, so the
        // outside point exists only when the widget has slack).
        const UiPoint outside{ink.right + 3, center_y};
        if (ink.right + 3 < node->rectangle.right &&
            (profile_scene_hit_test(scene, outside.x, outside.y).generation ||
             ui_widget_manager_hit_test(scene.widgets, outside.x, outside.y).slot != target.slot))
            return false;
        ui_button_pointer_target(&scene.widgets, UiButtonPointerEvent::move,
                                 profile_scene_hit_test(scene, outside.x, outside.y));
        UiTextCommand outside_commands[320]{};
        UiTextCommandStream outside_stream{};
        UiTextOutput outside_stream_output{&outside_stream, nullptr};
        if (!ui_text_command_stream_initialize(&outside_stream, {outside_commands, 320}) ||
            !profile_scene_text_output(scene, kDefaultTheme, &outside_stream_output, nullptr))
            return false;
        for (u32 i = 0; i < outside_stream.count; ++i)
            if (outside_commands[i].owner.handle.slot == target.slot &&
                (outside_commands[i].style_flags & text_style_underline))
                return false;
        if (ink.top > node->rectangle.top + 1 &&
            profile_scene_hit_test(scene, (ink.left + ink.right) * .5f, ink.top - 1).generation)
            return false;
        const auto moved =
            ui_button_pointer_target(&scene.widgets, UiButtonPointerEvent::move,
                                     profile_scene_hit_test(scene, (ink.left + ink.right) * .5f, center_y));
        if (!moved.changed || moved.target.slot != target.slot)
            return false;
        ++scene.revision;
        UiTextCommand commands[320]{};
        UiTextCommandStream stream{};
        UiTextOutput stream_output{&stream, nullptr};
        if (!ui_text_command_stream_initialize(&stream, {commands, 320}) ||
            !profile_scene_text_output(scene, kDefaultTheme, &stream_output, nullptr))
            return false;
        bool underline = false;
        for (u32 i = 0; i < stream.count; ++i)
            if (commands[i].owner.handle.slot == target.slot)
                underline |= (commands[i].style_flags & text_style_underline) != 0;
        if (!underline || !rebuild_probe_content_text() ||
            !probe_present(&g_probe.renderer, kDefaultTheme) ||
            !probe_capture_frame(kind ? L"profile-inline-hit-username-hover.bmp"
                                      : L"profile-inline-hit-name-hover.bmp"))
            return false;
    }
    ui_button_pointer(&scene.widgets, UiButtonPointerEvent::move,
                      {scene.rectangle.left - 1, scene.rectangle.top});
    ++scene.revision;
    const bool passed = rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme);
    if (passed)
        log_write(LogLevel::info, "probe", "profile_inline_native_ink_hover_inside_outside_passed");
    return passed;
}

} // namespace

namespace ui_probe_detail {

bool probe_transparent_guild_icon() noexcept {
    g_probe.failure_stage = 271;
    if (!chat_state_store_upsert_guild(&g_probe.chat_state,
            {200, {}, GuildMutationKind::update, 0,
             utf8_literal("abcdefabcdefabcdefabcdefabcdefab"), true}).ok() ||
        !navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok() ||
        !probe_sync_shell_and_text())
        return false;
    const auto* guild = chat_state_store_find_guild(g_probe.chat_state, 200);
    if (!guild) return false;
    u8 pixels[32 * 32 * 4]{};
    for (u32 y = 0; y < 32; ++y)
        for (u32 x = 0; x < 32; ++x) {
            const u32 at = (y * 32 + x) * 4;
            const u32 alpha = x < 12 ? 0 : x < 20 ? 255 : 128;
            // Media uploads consume the decoder's premultiplied RGBA output.
            pixels[at] = static_cast<u8>((240 * alpha + 127) / 255);
            pixels[at + 1] = static_cast<u8>((220 * alpha + 127) / 255);
            pixels[at + 2] = static_cast<u8>((20 * alpha + 127) / 255);
            pixels[at + 3] = static_cast<u8>(alpha);
        }
    if (!renderer_upload_media_rgba8(&g_probe.renderer,
            media_asset_guild_icon(200, guild->icon_variant, false), 32, 32, 128,
            {pixels, sizeof(pixels)}).ok())
        return false;
    GuildRailRowWidget* row = nullptr;
    for (u32 i = 0; i < g_probe.guild_rail_scene.count; ++i)
        if (g_probe.guild_rail_scene.rows[i].view.id == 200) row = &g_probe.guild_rail_scene.rows[i];
    const auto* layout = row ? guild_rail_scene_layout_node(g_probe.guild_rail_scene, *row) : nullptr;
    if (!layout) return false;
    const UiRect icon = layout->rectangle;
    const float width = ui_rect_width(icon), y = (icon.top + icon.bottom) * .5f;
    const Color art{240.f / 255, 220.f / 255, 20.f / 255, 1};
    const wchar_t* captures[]{L"transparent-guild-idle.bmp", L"transparent-guild-hover.bmp",
        L"transparent-guild-selected.bmp", L"colored-transparent-guild-idle.bmp",
        L"colored-transparent-guild-hover.bmp", L"colored-transparent-guild-selected.bmp"};
    for (u32 color = 0; color < 2; ++color) {
        Theme theme = kDefaultTheme;
        if (color) theme.base_lowest = {.04f, .25f, .30f, 1};
        const Color back = theme.base_lowest;
        const float alpha = 128.f / 255;
        const Color blend{art.red * alpha + back.red * (1 - alpha),
            art.green * alpha + back.green * (1 - alpha),
            art.blue * alpha + back.blue * (1 - alpha), 1};
        const ProbePixelSample samples[]{
            {{icon.left + width * .25f, y}, back, 3},
            {{icon.left + width * .50f, y}, art, 3},
            {{icon.left + width * .75f, y}, blend, 3}};
        for (u32 state = 0; state < 3; ++state) {
            row->view.selected = state == 2;
            ui_widget_manager_hover(&g_probe.guild_rail_scene.widgets,
                state == 1 ? row->widget : ui_widget_invalid_handle());
            if (!probe_present(&g_probe.renderer, theme) || !probe_exact_pixels({samples, 3}) ||
                !probe_capture_frame(captures[color * 3 + state]))
                return false;
        }
    }
    // Removing artwork must restore the fallback even if the old texture is
    // still resident in the atlas.
    if (!chat_state_store_upsert_guild(&g_probe.chat_state,
            {200, {}, GuildMutationKind::update, 0, {}, true}).ok() ||
        !navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok() ||
        !probe_sync_shell_and_text())
        return false;
    const ProbePixelSample fallback{{icon.left + width * .5f, y}, kDefaultTheme.surface_high, 3};
    return probe_present(&g_probe.renderer, kDefaultTheme) && probe_exact_pixels({&fallback, 1}) &&
        probe_capture_frame(L"guild-icon-removed-fallback.bmp");
}
bool probe_braille_profile() noexcept {
    g_probe.failure_stage = 270;
    UserProfile data{};
    data.user_id = 4242;
    const auto name = utf8_literal("Heroic");
    copy_bytes(data.username.data, name.data, name.bytes);
    data.username.bytes = static_cast<u16>(name.bytes);
    // 44 braille cells and seven line breaks: the failing bio's 139 UTF-8
    // bytes / 51 UTF-16 units. Include the blank braille cell.
    constexpr u8 cells[]{0x0f,0xff,0xff,0x39, 0x1f,0xff,0xff,0xff,0xff,0x3b,
        0x3f,0x00,0xff,0xff,0x00,0x3f, 0x3f,0x00,0xff,0xff,0x00,0x3f,
        0x3f,0xff,0x00,0x00,0xff,0x3f, 0x3f,0xff,0x0e,0x07,0xff,0x3f,
        0x08,0xff,0x36,0x36,0xff,0x01, 0x00,0x36,0x36,0x00};
    constexpr u8 widths[]{4,6,6,6,6,6,6,4};
    u32 cell = 0;
    for (u32 row = 0; row < 8; ++row) {
        if (row) data.bio.data[data.bio.bytes++] = '\n';
        for (u32 col = 0; col < widths[row]; ++col) {
            const u32 cp = 0x2800 + cells[cell++];
            data.bio.data[data.bio.bytes++] = static_cast<char>(0xe0 | (cp >> 12));
            data.bio.data[data.bio.bytes++] = static_cast<char>(0x80 | ((cp >> 6) & 63));
            data.bio.data[data.bio.bytes++] = static_cast<char>(0x80 | (cp & 63));
        }
    }
    ProfileScene scene{};
    scene.phase = ProfilePhase::ready;
    scene.measure = &g_probe.text_measure_cache;
    const UiRect viewport{0, 0, static_cast<float>(g_probe.renderer.width) * 96 / g_probe.renderer.dpi,
        static_cast<float>(g_probe.renderer.height) * 96 / g_probe.renderer.dpi};
    g_probe.renderer.profile_scene = &scene;
    const UiRect shell_labels[]{
        ui_scene_content_rectangle(g_probe.shell_scene, UiSemantic::channel_header),
        ui_scene_content_rectangle(g_probe.shell_scene, UiSemantic::title_label)};
    bool valid = data.bio.bytes == 139 && profile_scene_open(&scene, &data, &g_probe.chat_state,
        &g_probe.users, 1, {viewport.right - 100, 90, viewport.right, 134}, viewport);
    for (u32 pass = 0; valid && pass < 4; ++pass) {
        if (pass == 2) {
            profile_scene_release(&scene);
            scene.phase = ProfilePhase::ready;
            scene.measure = &g_probe.text_measure_cache;
            valid = profile_scene_open(&scene, &data, &g_probe.chat_state, &g_probe.users, 1,
                {viewport.right - 100, 90, viewport.right, 134}, viewport);
        }
        scene.presentation = (pass & 1) != 0 ? ProfilePresentation::modal : ProfilePresentation::popout;
        valid = profile_scene_layout(&scene, viewport, true) && scene.part_count > 5 &&
            utf8_equal(scene.bio_display.view(), data.bio.view()) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(pass & 1 ? (pass == 1 ? L"braille-profile-expanded.bmp" : L"braille-profile-expanded-warm.bmp")
                : (pass == 0 ? L"braille-profile.bmp" : L"braille-profile-warm.bmp"));
        const auto* node = ui_widget_manager_layout_node(scene.widgets, scene.bio_widget);
        valid = valid && node && probe_text_pixels({&node->rectangle, 1});
        if (scene.presentation == ProfilePresentation::popout)
            valid = valid && probe_text_pixels({shell_labels, 2});
    }
    // Inject a biography-only measurement failure while the card is open.
    // Verify real GPU text in the recovery card and the unaffected shell.
    UiTextMeasureRecord records[16]{};
    UiTextMeasureCache failed_measure{};
    const auto backend = [](void* context, const UiTextMeasureRequest& request,
                            UiTextMeasureValue* value) noexcept {
        auto& original = *static_cast<UiTextMeasureCache*>(context);
        return request.key.source_id != 0x42494F &&
            original.backend(original.backend_context, request, value);
    };
    scene.presentation = ProfilePresentation::popout;
    scene.measure = &failed_measure;
    valid = valid && ui_text_measure_cache_initialize(&failed_measure, {records, 16}, backend,
        &g_probe.text_measure_cache) && profile_scene_layout(&scene, viewport, true) &&
        utf8_equal(scene.bio_display.view(), utf8_literal("About me unavailable")) &&
        rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
        probe_text_pixels({shell_labels, 2}) && probe_capture_frame(L"braille-profile-failure-contained.bmp");
    const auto* recovered_bio = ui_widget_manager_layout_node(scene.widgets, scene.bio_widget);
    valid = valid && recovered_bio && probe_text_pixels({&recovered_bio->rectangle, 1});
    scene.measure = &g_probe.text_measure_cache;
    valid = valid && profile_scene_layout(&scene, viewport, true) &&
        utf8_equal(scene.bio_display.view(), data.bio.view()) && rebuild_probe_content_text() &&
        probe_present(&g_probe.renderer, kDefaultTheme) && probe_text_pixels({shell_labels, 2}) &&
        probe_capture_frame(L"braille-profile-recovered.bmp");
    profile_scene_release(&scene);
    g_probe.renderer.profile_scene = nullptr;
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
        probe_text_pixels({shell_labels, 2}) &&
        probe_capture_frame(L"braille-profile-closed.bmp");
    return valid;
}

} // namespace ui_probe_detail

namespace {

bool probe_profile_effect_layers(ProfileScene& scene) noexcept {
    // Original solid artwork makes every glyph leaking over the effect
    // observable in a native render-target readback, without saved captures.
    ProfileEffect effect{};
    effect.sku = 987654;
    effect.count = 1;
    effect.layers[0].loop = true;
    effect.layers[0].duration_ms = 60000;
    const auto source = utf8_literal("https://example.invalid/native-profile-effect.png");
    copy_bytes(effect.layers[0].source.data, source.data, source.bytes);
    effect.layers[0].source.bytes = static_cast<u16>(source.bytes);
    const auto* saved_effect = scene.effect;
    const u64 saved_started = scene.effect_started_ms;
    const u64 saved_hover_changed = scene.hover_changed_ms;
    const bool saved_hovered = scene.hovered;
    const auto saved_presentation = scene.presentation;
    scene.effect = &effect;
    scene.effect_started_ms = GetTickCount64();
    scene.hovered = false;
    scene.hover_changed_ms = 0;
    ProfileMedia media{};
    u8 pixels[2 * 8 * 4];
    for (u32 i = 0; i < sizeof(pixels); i += 4) {
        pixels[i] = pixels[i + 2] = pixels[i + 3] = 255;
        pixels[i + 1] = 0;
    }
    bool valid =
        profile_effect_media_resolve(scene, 0, scene.effect_started_ms, &media) &&
        renderer_upload_media_rgba8(&g_probe.renderer, media.key, 2, 8, 8, {pixels, sizeof(pixels)}).ok();
    for (u32 mode = 0; valid && mode < 2; ++mode) {
        scene.presentation = mode ? ProfilePresentation::modal : ProfilePresentation::popout;
        scene.effect = nullptr;
        valid = profile_scene_layout(&scene, scene.viewport, true) && rebuild_probe_content_text() &&
                probe_present(&g_probe.renderer, kDefaultTheme);
        const auto* name = ui_widget_manager_layout_node(scene.widgets, scene.name_widget);
        const auto r = name ? ui_rect_intersection(name->rectangle, name->clip_rectangle) : UiRect{};
        const auto mask = profile_scene_card_mask(scene, kDefaultTheme);
        const UiPoint corners[]{{mask.rectangle.left + .5f, mask.rectangle.top + .5f},
                                {mask.rectangle.left - 1, mask.rectangle.top + mask.radius}};
        u8 below[2][4]{}, above[2][4]{};
        valid = valid && ui_rect_has_area(r) && probe_text_pixels({&r, 1}) &&
                probe_read_pixels(kDefaultTheme, {corners, 2}, below);
        scene.effect = &effect;
        ++scene.revision;
        valid = valid && probe_present(&g_probe.renderer, kDefaultTheme) &&
                probe_read_pixels(kDefaultTheme, {corners, 2}, above) &&
                bytes_equal(below, above, sizeof(below));
        ProbePixelSample samples[32 * 12];
        for (u32 y = 0; y < 12; ++y)
            for (u32 x = 0; x < 32; ++x)
                samples[y * 32 + x] = {{r.left + (x + .5f) * ui_rect_width(r) / 32,
                                        r.top + (y + .5f) * ui_rect_height(r) / 12},
                                       {1, 0, 1, 1},
                                       2};
        valid =
            valid && ui_rect_has_area(r) &&
            g_probe.renderer.profile_effect_media_first_batch >
                g_probe.renderer.profile_media_first_batch &&
            g_probe.renderer.profile_effect_media_first_batch < g_probe.renderer.profile_media_end_batch &&
            g_probe.renderer.profile_controls_first_batch >
                g_probe.renderer.profile_foreground_first_batch &&
            probe_exact_pixels({samples, 32 * 12});
        // The top controls must survive the effect, while avatar media stays below.
        const auto avatar = scene.avatar;
        const ProbePixelSample avatar_sample{
            {(avatar.left + avatar.right) * .5f, (avatar.top + avatar.bottom) * .5f}, {1, 0, 1, 1}, 2};
        valid = valid && probe_exact_pixels({&avatar_sample, 1});
        bool control_visible = false;
        for (u32 i = 0; valid && i < scene.part_count; ++i) {
            const auto& p = scene.parts[i];
            if (p.look != ProfileLook::header_button)
                continue;
            const auto* node = ui_widget_manager_layout_node(scene.widgets, p.widget);
            if (!node)
                continue;
            const UiPoint point{(node->rectangle.left + node->rectangle.right) * .5f,
                                (node->rectangle.top + node->rectangle.bottom) * .5f};
            u8 color[1][4]{};
            valid = probe_read_pixels(kDefaultTheme, {&point, 1}, color);
            control_visible |= color[0][1] > 2 || color[0][0] < 253 || color[0][2] < 253;
        }
        valid = valid && control_visible;
    }
    // A later modal must cover even an opaque profile effect.
    TextFormScene form{};
    TextFormSurface surface{};
    TextFormContent content{};
    content.title = utf8_literal("Profile overlay");
    content.context = utf8_literal("This form appears above the profile effect.");
    content.cancel = utf8_literal("Close");
    content.accept = utf8_literal("Done");
    content.confirmation_only = true;
    auto* saved_form = g_probe.renderer.text_form;
    valid = valid && text_form_update(&form, content, scene.viewport, &g_probe.text_measure_cache);
    surface.scene = &form;
    renderer_bind_text_form(&g_probe.renderer, &surface);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme);
    const auto* panel = text_form_node(form, TextFormElement::panel);
    const auto overlap = panel
                             ? ui_rect_intersection(panel->rectangle,
                                                    profile_scene_card_mask(scene, kDefaultTheme).rectangle)
                             : UiRect{};
    const UiPoint covered{(overlap.left + overlap.right) * .5f, (overlap.top + overlap.bottom) * .5f};
    u8 covered_pixel[1][4]{};
    valid = valid && ui_rect_width(overlap) > kDefaultTheme.scale.radius_md * 2 &&
            ui_rect_height(overlap) > kDefaultTheme.scale.radius_md * 2 &&
            probe_read_pixels(kDefaultTheme, {&covered, 1}, covered_pixel) &&
            (covered_pixel[0][1] > 2 || covered_pixel[0][0] < 253 || covered_pixel[0][2] < 253);
    renderer_bind_text_form(&g_probe.renderer, saved_form);
    text_form_surface_release(&surface);
    text_form_release(&form);
    scene.effect = saved_effect;
    scene.effect_started_ms = saved_started;
    scene.hover_changed_ms = saved_hover_changed;
    scene.hovered = saved_hovered;
    scene.presentation = saved_presentation;
    const bool restored = profile_scene_layout(&scene, scene.viewport, true) &&
                          rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme);
    if (valid && restored)
        log_write(LogLevel::info, "probe", "profile_effect_above_glyphs_avatar_below_controls_passed");
    return valid && restored;
}

} // namespace

namespace ui_probe_detail {

bool probe_profile_card() noexcept {
    g_probe.failure_stage = 151;
    auto* data = static_cast<UserProfile*>(
        VirtualAlloc(nullptr, sizeof(UserProfile), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!data)
        return false;
    u32 scratch[2048];
    const auto json = utf8_literal(
        R"({"user":{"id":"4242","username":"native.test","global_name":"Native Profile","avatar":"abc123","display_name_styles":{"font_id":14,"effect_id":5,"colors":[16736371,6879487]}},"user_profile":{"bio":"A profile card with a wrapped biography, separate guild appearance and account badges. All images use the shared asynchronous media cache.","pronouns":"they/them","theme_colors":[16711680,65535],"banner":"a_testbanner"},"guild_member":{"nick":"Robin-san","roles":["55","56","57"],"joined_at":"2021-03-14T12:00:00.000Z"},"mutual_guilds":[{"id":"1","name":"Native test server"}],"mutual_friends":[{"id":"2","username":"Friend"}],"premium_type":2,"badges":[{"id":"premium","icon":"abcdef","description":"Nitro subscriber"}]})");
    bool valid = user_profile_decode(json, {scratch, 2048}, 4242, 100, data);
    data->user_appearance.primary_enabled = true;
    data->user_appearance.primary_guild_id = 100;
    data->user_appearance.fields |= appearance_guild_tag;
    copy_bytes(data->user_appearance.primary_tag.data, "GEAR", 4);
    data->user_appearance.primary_tag.bytes = 4;
    copy_bytes(data->user_appearance.primary_badge.data, "abc123", 6);
    data->user_appearance.primary_badge.bytes = 6;
    valid =
        valid &&
        chat_state_store_upsert_role(&g_probe.chat_state,
                                        {56, 100, 0, utf8_literal("HELPER"), 1, 0xB88A22}) &&
        chat_state_store_upsert_role(
            &g_probe.chat_state,
            {57, 100, 0,
             utf8_literal("A very long role name that must fit the column and truncate only at its edge"),
             1, 0x5588AA});
    valid = valid && chat_state_store_upsert_role(
                         &g_probe.chat_state, {58, 100, 0, utf8_literal("Assignable role"), 1, 0x5588AA});
    auto* guild = const_cast<GuildRecord*>(chat_state_store_find_guild(g_probe.chat_state, 100));
    const u64 previous_owner = guild ? guild->owner_id : 0;
    if (guild)
        guild->owner_id = g_probe.chat_state.self_user_id;
    ProfileScene scene{};
    scene.phase = ProfilePhase::ready;
    scene.measure = &g_probe.text_measure_cache;
    const UiRect viewport{0, 0, static_cast<float>(g_probe.renderer.width) * 96 / g_probe.renderer.dpi,
                          static_cast<float>(g_probe.renderer.height) * 96 / g_probe.renderer.dpi};
    valid = valid && user_appearance_upsert(&g_probe.users.appearance, data->user_appearance) &&
            profile_scene_open(&scene, data, &g_probe.chat_state, &g_probe.users, 1,
                               {viewport.right - 100, 90, viewport.right, 134}, viewport);
    PngImage image{};
    u8 pixels[32 * (32 * 4 + 1)]{};
    valid = valid && png_image_configure(&image, 32, 32, 8, 6);
    for (u32 y = 0; y < 32; ++y)
        for (u32 x = 0; x < 32; ++x) {
            const u32 at = y * 129 + 1 + x * 4;
            pixels[at] = static_cast<u8>(x * 7);
            pixels[at + 1] = static_cast<u8>(y * 7);
            pixels[at + 2] = 180;
            pixels[at + 3] = 255;
        }
    for (u32 i = 0; valid && i < scene.part_count; ++i) {
        ProfileMedia media{};
        if (profile_media_resolve(scene, i, &media))
            valid =
                renderer_upload_png_scanlines(&g_probe.renderer, media.key, image, {pixels, sizeof(pixels)})
                    .ok();
    }
    const auto saved_banner = data->banner, saved_guild_banner = data->guild_banner;
    data->guild_banner = {};
    copy_bytes(data->banner.data, "staticbanner", 12);
    data->banner.bytes = 12;
    bool static_banner = false, animated_banner = false, gif_banner = false;
    for (u32 i = 0; i < scene.part_count; ++i)
        if (scene.parts[i].media == ProfileMediaKind::banner) {
            ProfileMedia check{};
            static_banner = profile_media_resolve(scene, i, &check) &&
                            media_asset_embed_format(check.key) == MediaAttachmentFormat::png;
            copy_bytes(data->banner.data, "a_animatedbanner", 16);
            data->banner.bytes = 16;
            animated_banner = profile_media_resolve(scene, i, &check) &&
                              media_asset_embed_format(check.key) == MediaAttachmentFormat::webp &&
                              (check.key.flags & media_asset_flag_animated);
            valid = valid && profile_media_use_compatibility_banner(&scene);
            gif_banner = profile_media_resolve(scene, i, &check) &&
                         media_asset_embed_format(check.key) == MediaAttachmentFormat::gif &&
                         (check.key.flags & media_asset_flag_animated);
        }
    valid = valid && static_banner && animated_banner && gif_banner;
    scene.banner_compatibility = false;
    data->banner = saved_banner;
    data->guild_banner = saved_guild_banner;
    scene.presence = UiPresence::busy;
    scene.edit = {utf8_literal("test test test"), 5, 9, true, true, true};
    ++scene.revision;
    g_probe.renderer.profile_scene = &scene;
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"member-profile-card.bmp");
    valid = valid && probe_profile_effect_layers(scene);
    const wchar_t* const profile_motion_captures[]{L"profile-motion-zero.bmp", L"profile-motion-half.bmp", L"profile-motion-full.bmp"};
    const auto profile_motion_rectangle = scene.rectangle;
    const auto motion_saved_user_name = data->user_appearance.name;
    const auto motion_saved_guild_name = data->guild_appearance.name;
    const auto motion_saved_edit = scene.edit;
    data->user_appearance.name.effect_id = 0;
    data->guild_appearance.name.effect_id = 0;
    valid = valid && user_appearance_upsert(&g_probe.users.appearance, data->user_appearance);
    if (data->guild_appearance.user_id)
        valid = valid && user_appearance_upsert(&g_probe.users.appearance, data->guild_appearance);
    scene.edit = {};
    ++scene.revision;
    // The shared pixel check identifies original solid-magenta media ink.
    u8 motion_pixels[sizeof(pixels)]{};
    for (u32 y = 0; y < 32; ++y)
        for (u32 x = 0; x < 32; ++x) {
            const u32 at = y * 129 + 1 + x * 4;
            motion_pixels[at] = 255;
            motion_pixels[at + 2] = 220;
            motion_pixels[at + 3] = 255;
        }
    for (u32 i = 0; valid && i < scene.part_count; ++i) {
        ProfileMedia media{};
        if (profile_media_resolve(scene, i, &media))
            valid = renderer_upload_png_scanlines(&g_probe.renderer, media.key, image,
                                                  {motion_pixels, sizeof(motion_pixels)}).ok();
    }
    valid = valid && surface_motion_probe_detail::measure(g_probe.renderer, RendererLayerSurface::profile,
        scene.rectangle, true, profile_motion_captures, [&]() noexcept {
            return scene.active && ui_rect_equal(scene.rectangle, profile_motion_rectangle);
        });
    data->user_appearance.name = motion_saved_user_name;
    data->guild_appearance.name = motion_saved_guild_name;
    valid = valid && user_appearance_upsert(&g_probe.users.appearance, data->user_appearance);
    if (data->guild_appearance.user_id)
        valid = valid && user_appearance_upsert(&g_probe.users.appearance, data->guild_appearance);
    scene.edit = motion_saved_edit;
    ++scene.revision;
    for (u32 i = 0; valid && i < scene.part_count; ++i) {
        ProfileMedia media{};
        if (profile_media_resolve(scene, i, &media))
            valid = renderer_upload_png_scanlines(&g_probe.renderer, media.key, image, {pixels, sizeof(pixels)}).ok();
    }
    // Exercise an actual painted Arabic line, including a join control, through
    // the same profile-bio path used by the application. Restore the fixture
    // before the existing interaction/retention probes continue.
    const auto arabic_saved_bio = data->bio;
    const auto arabic_bio =
        utf8_literal("\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85 \xd8\xa8\xd8\xa8 \xd8\xa8\xe2\x80\x8d\xd8\xa8");
    const auto arabic_before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    copy_bytes(data->bio.data, arabic_bio.data, arabic_bio.bytes);
    data->bio.bytes = static_cast<u16>(arabic_bio.bytes);
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) && probe_capture_frame(L"arabic-profile.bmp");
    const auto arabic_after = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    // Bios carry text-sm style spans, so a plain Arabic bio shapes as a
    // styled native paragraph.
    valid = valid && (arabic_after.arabic_shaped > arabic_before.arabic_shaped ||
                      arabic_after.paragraphs_shaped > arabic_before.paragraphs_shaped);
    // Paint the native Hebrew path through the real profile-bio renderer.
    // Include a repeated vowel and reordered shin marks in the captured image.
    const auto hebrew_bio = utf8_literal("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\x90\xd6\xb0\xd6\xb0 "
                                         "\xd7\xa9\xd6\xb8\xd6\xbc\xd7\x81 \xd7\x95\xd6\xb9");
    auto hebrew_system = g_probe.renderer.glyph_text.system;
    TextGlyph hebrew_glyphs[128];
    TextRun hebrew_runs[16];
    u16 hebrew_clusters[128];
    ShapedText hebrew_output{};
    wchar_t hebrew_utf16[128];
    valid = valid &&
            shaped_text_initialize(&hebrew_output, {hebrew_glyphs, 128}, {hebrew_runs, 16},
                                   {hebrew_clusters, 128}) &&
            text_shape_utf8(&hebrew_system, hebrew_bio, {}, 10000, 10000, {hebrew_utf16, 128}, &hebrew_output)
                .ok() &&
            hebrew_output.native_fonts();
    shaped_text_reset(&hebrew_output);
    const auto hebrew_before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    copy_bytes(data->bio.data, hebrew_bio.data, hebrew_bio.bytes);
    data->bio.bytes = static_cast<u16>(hebrew_bio.bytes);
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) && probe_capture_frame(L"hebrew-profile.bmp");
    valid = valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_shaped >
                         hebrew_before.paragraphs_shaped;
    data->bio.bytes = 0;
    for (u32 repeat = 0; repeat < 8; ++repeat) {
        copy_bytes(data->bio.data + data->bio.bytes, hebrew_bio.data, hebrew_bio.bytes);
        data->bio.bytes += static_cast<u16>(hebrew_bio.bytes);
        data->bio.data[data->bio.bytes++] = ' ';
    }
    TextGlyph wrapped_glyphs[512];
    TextRun wrapped_runs[128];
    u16 wrapped_clusters[512];
    ShapedText wrapped_output{};
    wchar_t wrapped_utf16[512];
    valid = valid &&
            shaped_text_initialize(&wrapped_output, {wrapped_glyphs, 512}, {wrapped_runs, 128},
                                   {wrapped_clusters, 512}) &&
            text_shape_utf8(&hebrew_system, {data->bio.data, data->bio.bytes}, {}, 180, 10000,
                            {wrapped_utf16, 512}, &wrapped_output)
                .ok() &&
            wrapped_output.native_fonts() && wrapped_output.line_count > 1;
    shaped_text_reset(&wrapped_output);
    const auto wrapped_before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"hebrew-wrapped-profile.bmp");
    valid = valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_shaped >
                         wrapped_before.paragraphs_shaped;
    const auto mixed_bio = utf8_literal(
        "Hello (\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d) 123 \xce\xb1 \xd0\x91 \xd7\x90\xd6\xb0\xd6\xb0 world ");
    data->bio.bytes = 0;
    for (u32 repeat = 0; repeat < 2; ++repeat) {
        copy_bytes(data->bio.data + data->bio.bytes, mixed_bio.data, mixed_bio.bytes);
        data->bio.bytes += static_cast<u16>(mixed_bio.bytes);
    }
    valid = valid && profile_scene_layout(&scene, viewport, true);
    // Validate the exact draw command, including the real ellipsis bounds.
    // A long unconstrained preflight can succeed while the actual bio falls
    // back for height trimming, so it cannot prove native drawing admission.
    UiTextCommand mixed_commands[320]{};
    UiTextCommandStream mixed_stream{};
    UiTextOutput mixed_stream_output{&mixed_stream, nullptr};
    TextDecoration bio_decorations[128];
    TextHitRegion bio_hits[256];
    valid = valid && ui_text_command_stream_initialize(&mixed_stream, {mixed_commands, 320}) &&
            shaped_text_initialize(&wrapped_output, {wrapped_glyphs, 512}, {wrapped_runs, 128},
                                   {wrapped_clusters, 512}, {bio_decorations, 128}, {bio_hits, 256});
    const auto native_bio_command = [&](bool expect_height_trim = false) noexcept {
        ui_text_command_stream_reset(&mixed_stream);
        if (!profile_scene_text_output(scene, kDefaultTheme, &mixed_stream_output, nullptr))
            return false;
        bool found = false;
        for (u32 i = 0; i < mixed_stream.count; ++i) {
            const auto& command = mixed_commands[i];
            if (command.owner.handle.slot != scene.bio_widget.slot)
                continue;
            found = true;
            TextHitSpan hits[64];
            for (u32 h = 0; h < scene.bio_semantic_count; ++h) {
                const auto& semantic = scene.bio_semantics[h];
                hits[h] = {semantic.byte_offset, semantic.byte_count, static_cast<u16>(h), 0};
            }
            const bool native =
                command.face == UiTextFace::normal && !command.effect.kind && !command.style_flags &&
                text_shape_utf8(&hebrew_system, command.text, command.styles, ui_rect_width(command.rectangle),
                                ui_rect_height(command.rectangle), {wrapped_utf16, 512}, &wrapped_output,
                                command.wrapping, command.overflow, {hits, scene.bio_semantic_count})
                    .ok() &&
                wrapped_output.native_fonts() && wrapped_output.line_count > 1 &&
                (!expect_height_trim || wrapped_output.glyph_count < 16) &&
                (!scene.bio_semantic_count || wrapped_output.hit_region_count);
            if (!native)
                log_u64(LogLevel::error, "probe", "native_bio_preflight_lines", wrapped_output.line_count);
            shaped_text_reset(&wrapped_output);
            if (!native)
                return false;
        }
        return found;
    };
    valid = valid && native_bio_command();
    const auto mixed_before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"mixed-bidi-profile.bmp");
    valid = valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_shaped >
                         mixed_before.paragraphs_shaped;
    struct BioCase {
        Utf8View text;
        u32 repeats, styles;
        bool lines, trim;
        const wchar_t* capture;
    };
    const BioCase bio_cases[]{
        {utf8_literal("Hello **\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d** *world* __\xd7\x90\xd7\x91__ ~~xyz~~ `\xd7\x90 abc` "), 2, 5, false, false, L"mixed-bidi-styles-profile.bmp"},
        {utf8_literal("(**\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7) 12 \\[\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85\\]**"), 2, 1, true, false, L"arabic-bracket-styles-profile.bmp"},
        {utf8_literal("`(\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7) 12 [\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85]`"), 2, 1, true, false, L"arabic-bracket-mono-profile.bmp"},
        {utf8_literal("\x2a\x2a\xe4\xb8\xad\xe6\x96\x87\x2a\x2a\x20\xe3\x82\xab\xe3\x82\x99\x20\xef\xbd\xb6"
                      "\xef\xbe\x9e"), 2, 1, true, false, L"cjk-styles-profile.bmp"},
        {utf8_literal("\x2a\x2a\xe4\xb8\xad\xf0\xa0\x80\x80\xe6\x96\x87\x2a\x2a\x20\xf0\x9f\x98\x80"), 2, 1, true, false, L"cjk-supplementary-profile.bmp"},
        {utf8_literal("\x2a\x2a\xe3\x82\xab\xe2\x80\x8d\xe3\x82\x99\x20\xe3\x82\xab\xe2\x80\x8c\xe3\x82\x99"
                      "\x20\xe3\x82\xab\xcd\x8f\xe3\x82\x99\x2a\x2a\x20\xef\xbd\xb6\xef\xbe\x9e"), 2, 1, true, false, L"cjk-controls-profile.bmp"},
        {utf8_literal("\x2a\x2a\xe4\xb8\xad\xe6\x96\x87\x2a\x2a"), 8, 1, true, true, L"height-overflow-profile.bmp"},
        {utf8_literal("A **\xd7\x90\xe2\x80\x8d\xd7\x91** __\xd7\x90\xcd\x8f \xd7\x91__ "
                      "*world* `\xd7\x90\xe2\x80\x8d \xd7\x91` "), 3, 4, false, false, L"mixed-join-styles-profile.bmp"},
        {utf8_literal("A **\xd7\x90 \xf0\x9f\x98\x80\xf0\x9f\x98\x83 \xd7\x91** __\xd7\x90 "
                      "\xe2\x9d\xa4\xef\xb8\x8f \xd7\x91__ `\xd7\x90 1\xef\xb8\x8f\xe2\x83\xa3 \xd7\x91` "), 2, 3, false, false, L"mixed-bidi-emoji-profile.bmp"},
        {utf8_literal("A **\xe2\x9d\xa4\xef\xb8\x8e** __\xce\xb1 \xe2\x9d\xa4\xef\xb8\x8e \xd0\x91__ "
                      "`\xe2\x99\xa5\xef\xb8\x8e \xe2\x99\xa6\xef\xb8\x8e` \xe2\x88\xab x \xe2\x89\xa4 y "
                      "\xc2\xa9\xef\xb8\x8e A \xef\xb8\x8f B "), 2, 3, false, false, L"symbols-profile.bmp"},
        {utf8_literal("Text **\xe2\x9d\xa4\xef\xb8\x8e** *\xe2\x9d\xa4\xef\xb8\x8e* ***\xe2\x9d\xa4\xef\xb8\x8e*** "
                      "__\xe2\x98\x80\xef\xb8\x8e \xe2\x98\x82\xef\xb8\x8e__ "), 3, 4, false, false, L"simulated-color-symbols-profile.bmp"}};
    for (const auto& bio : bio_cases) {
        if (!valid)
            break;
        data->bio.bytes = 0;
        for (u32 repeat = 0; repeat < bio.repeats; ++repeat) {
            if (bio.lines && repeat)
                data->bio.data[data->bio.bytes++] = '\n';
            copy_bytes(data->bio.data + data->bio.bytes, bio.text.data, bio.text.bytes);
            data->bio.bytes += static_cast<u16>(bio.text.bytes);
        }
        valid = profile_scene_layout(&scene, viewport, true) && scene.bio_style_count >= bio.styles &&
                native_bio_command(bio.trim);
        const auto before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
        valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
                probe_capture_frame(bio.capture);
        valid = valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_shaped >
                             before.paragraphs_shaped;
    }
    const auto interactive_bio =
        utf8_literal("A [\xd7\x90 abc \xe2\x9d\xa4\xef\xb8\x8f \xd7\x91](https://example.com/native-text) "
                     "**[\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d](https://example.com/native-bold)**\n");
    data->bio.bytes = 0;
    for (u32 repeat = 0; repeat < 2; ++repeat) {
        copy_bytes(data->bio.data + data->bio.bytes, interactive_bio.data, interactive_bio.bytes);
        data->bio.bytes += static_cast<u16>(interactive_bio.bytes);
    }
    valid = valid && profile_scene_layout(&scene, viewport, true) && scene.bio_semantic_count == 4;
    valid = valid && native_bio_command();
    const auto interactive_before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"mixed-bidi-links-profile.bmp");
    for (u32 s = 0; valid && s < scene.bio_semantic_count; ++s) {
        const auto& expected = scene.bio_semantics[s];
        bool found = false;
        const auto& storage = *g_probe.renderer.content_text;
        for (u32 h = 0; h < storage.content_hit_region_count; ++h) {
            const auto& region = storage.content_hit_regions[h];
            const auto& semantic = storage.content_semantics[region.semantic_index];
            if (semantic.message_id || semantic.source_offset != expected.source_offset ||
                semantic.source_bytes != expected.source_bytes || semantic.kind != expected.kind)
                continue;
            const auto hit = renderer_content_hit_test(
                g_probe.renderer,
                static_cast<i32>((region.left + region.right) * .5f * g_probe.renderer.dpi / 96.f),
                static_cast<i32>((region.top + region.bottom) * .5f * g_probe.renderer.dpi / 96.f));
            valid = valid && hit.valid && !hit.message_id && hit.source_offset == expected.source_offset &&
                    hit.source_bytes == expected.source_bytes && hit.kind == expected.kind;
            found = true;
        }
        valid = valid && found;
    }
    if (valid)
        log_write(LogLevel::info, "probe", "mixed_bidi_profile_links_hit_geometry_passed");
    valid = valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_shaped >
                         interactive_before.paragraphs_shaped;
    data->bio = arabic_saved_bio;
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme);
    const u32 banner_text_rebuilds = g_probe.text.rebuilds;
    const u32 banner_text_suppressions = g_probe.renderer.stale_interaction_text_suppressions;
    const u32 banner_text_batches = g_probe.text.batch_count;
    valid = valid && profile_media_use_compatibility_banner(&scene) &&
            !profile_media_use_compatibility_banner(&scene) &&
            banner_text_batches > g_probe.renderer.overlay_text_first_batch;
    // Exercise the shipping transition without a text rebuild, then continue
    // presenting media frames. This was an indefinitely blank profile before.
    for (u32 frame = 0; valid && frame < 12; ++frame)
        valid = probe_present(&g_probe.renderer, kDefaultTheme) &&
                g_probe.text.rebuilds == banner_text_rebuilds &&
                g_probe.text.batch_count == banner_text_batches &&
                g_probe.renderer.stale_interaction_text_suppressions == banner_text_suppressions;
    scene.banner_compatibility = false;
    if (valid)
        log_write(LogLevel::info, "probe", "banner_format_switch_preserves_profile_text_passed");
    valid = valid && probe_native_profile_asset(renderer_text_system(&g_probe.renderer, UiTextFace::techno), 14);
    bool rounded_media = false;
    const auto card_mask = profile_scene_card_mask(scene, kDefaultTheme);
    for (u32 i = 0; i < g_probe.renderer.media_quad_count; ++i) {
        const auto& q = g_probe.renderer.media_draw.quad_data[i];
        rounded_media |= ui_rect_equal(q.rounded_clip, card_mask.rectangle) && q.clip_radius == card_mask.radius;
    }
    valid = valid && rounded_media;
    // The official client's footer: 16 DIPs in (plus the theme border) and 16 up.
    const float inset = 16 + (scene.themed ? 2.f : 0.f);
    valid = valid && scene.edit_rectangle.left == scene.rectangle.left + inset &&
            scene.edit_rectangle.right == scene.rectangle.right - inset &&
            scene.edit_rectangle.bottom == scene.rectangle.bottom - inset;
    // Role pills (expanded past the popout's two lines): the removable HELPER
    // pill ends in its always-visible x; the long name fits the content column.
    scene.roles_expanded = true;
    valid = valid && profile_scene_layout(&scene, viewport, true);
    bool helper = false, long_role = false, remove = false;
    for (u32 i = 0; i < scene.part_count; ++i) {
        const auto& part = scene.parts[i];
        const auto* node = ui_widget_manager_layout_node(scene.widgets, part.widget);
        if (!node)
            continue;
        const auto r = node->rectangle;
        if (part.look == ProfileLook::role && part.target == 56)
            helper = ui_rect_width(r) > 40 && ui_rect_height(r) == 24;
        if (part.look == ProfileLook::role && part.target == 57)
            long_role = r.right <= scene.rectangle.right - inset && ui_rect_width(r) > 250;
        if (part.action == ProfileAction::remove_role && part.target == 56) {
            const auto clicked =
                ui_button_pointer(&scene.widgets, UiButtonPointerEvent::press, {r.left + 12, r.top + 12});
            const auto released =
                ui_button_pointer(&scene.widgets, UiButtonPointerEvent::release, {r.left + 12, r.top + 12});
            remove = clicked.handled && released.activate &&
                     profile_scene_action(scene, released.target) == ProfileAction::remove_role;
        }
    }
    valid = valid && helper && long_role && remove;
    scene.roles_expanded = false;
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme);
    const auto& edit_layout = g_probe.renderer.profile_edit_layout;
    valid = valid && edit_layout.active && edit_layout.selection_count && edit_layout.text_bytes == 14;
    valid = valid && probe_profile_hover(scene);
    const u32 shaped = g_probe.renderer.content_text->rebuilds,
              uploads = g_probe.renderer.text_instance_uploads;
    const u64 deadline = g_probe.renderer.text_effect_next_ms;
    const u32 effect_builds = g_probe.renderer.paint_stream_builds;
    const bool effect_tick = valid && renderer_note_text_effect_damage(&g_probe.renderer, deadline) &&
                             g_probe.renderer.text_effect_next_ms == deadline + 42 &&
                             probe_present(&g_probe.renderer, kDefaultTheme) &&
                             g_probe.renderer.content_text->rebuilds == shaped &&
                             g_probe.renderer.text_instance_uploads == uploads &&
                             // Shader-time effects repaint through damage, not a scene rebuild.
                             g_probe.renderer.paint_stream_builds == effect_builds &&
                             probe_capture_frame(L"member-profile-name-animated.bmp");
    if (valid && !effect_tick)
        log_write(LogLevel::error, "probe", "profile_name_effect_damage_retention_failed");
    valid = valid && effect_tick;
    {
        // The next tick, with both flip buffers synchronized, repaints only
        // the animated name's bounds.
        const auto& r = g_probe.renderer;
        const u32 full = r.full_present_draws, partial = r.partial_present_draws,
                  builds = r.paint_stream_builds;
        const u64 damaged = r.partial_present_pixels;
        const u64 next_tick = r.text_effect_next_ms;
        const bool partial_tick =
            valid && renderer_note_text_effect_damage(&g_probe.renderer, next_tick) &&
            r.text_effect_next_ms == next_tick + 42 && probe_present(&g_probe.renderer, kDefaultTheme) &&
            r.paint_stream_builds == builds && r.content_text->rebuilds == shaped &&
            r.text_instance_uploads == uploads && r.full_present_draws == full &&
            r.partial_present_draws == partial + 1 &&
            r.partial_present_pixels - damaged < static_cast<u64>(r.width) * r.height / 16;
        if (valid && !partial_tick)
            log_write(LogLevel::error, "probe", "profile_name_effect_partial_damage_failed");
        valid = valid && partial_tick;
    }
    scene.edit.caret_visible = false;
    valid = valid && probe_present(&g_probe.renderer, kDefaultTheme) &&
            g_probe.renderer.content_text->rebuilds == shaped &&
            g_probe.renderer.text_instance_uploads == uploads;
    // Exercise every released font and effect through the native renderer, including
    // loading the intended family rather than silently accepting system fallback.
    const auto original_style = data->user_appearance.name;
    const wchar_t* font_captures[] = {L"font-01.bmp", L"font-02.bmp", L"font-03.bmp", L"font-04.bmp",
                                      L"font-05.bmp", L"font-06.bmp", L"font-07.bmp", L"font-08.bmp",
                                      L"font-09.bmp", L"font-10.bmp", L"font-11.bmp", L"font-12.bmp",
                                      L"font-13.bmp", L"font-14.bmp", L"font-15.bmp", L"font-16.bmp"};
    for (u16 id = 1; valid && id <= 16; ++id) {
        g_probe.failure_stage = 170 + id;
        data->user_appearance.name.font_id = id;
        data->user_appearance.name.effect_id = 1;
        valid = user_appearance_upsert(&g_probe.users.appearance, data->user_appearance) &&
                profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
                probe_present(&g_probe.renderer, kDefaultTheme);
        if (valid)
            g_probe.failure_stage = 190 + id;
        const auto face = profile_name_face(id);
        const auto system = renderer_text_system(&g_probe.renderer, face);
        if (id != 11)
            valid = valid && probe_native_profile_asset(system, id);
        valid = valid && probe_capture_frame(font_captures[id - 1]);
        if (!valid)
            log_u64(LogLevel::error, "probe", "profile_font_failed", id);
    }
    const wchar_t* effect_captures[] = {L"effect-01.bmp", L"effect-02.bmp", L"effect-03.bmp",
                                        L"effect-04.bmp", L"effect-05.bmp", L"effect-06.bmp",
                                        L"effect-07.bmp", L"effect-08.bmp"};
    for (u16 id = 1; valid && id <= 8; ++id) {
        auto& style = data->user_appearance.name;
        style.font_id = 14;
        style.effect_id = id;
        style.color_count = id == 7 ? 5 : id == 8 ? 4 : id == 2 || id == 6 ? 2 : 1;
        const u32 palette[] = {0xAB66FF, 0x55DACC, 0xF5CD79, 0xF392A9, 0x73B5FA};
        for (u32 c = 0; c < style.color_count; ++c)
            style.colors[c] = palette[c];
        scene.name_started_ms = GetTickCount64() - 420;
        BOOL motion = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &motion, 0);
        g_probe.renderer.system_animation_off = motion == FALSE;
        valid = user_appearance_upsert(&g_probe.users.appearance, data->user_appearance) &&
                profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
                probe_present(&g_probe.renderer, kDefaultTheme);
        bool found = false;
        for (u32 m = 0; m < g_probe.renderer.text_effect_count; ++m)
            if (g_probe.renderer.text_effects.materials[m].kind == id) {
                const bool moving = id == 3 || id == 4 || id == 5 || id == 7 || id == 8;
                found = true;
                valid = valid &&
                        (g_probe.renderer.text_effects.materials[m].animate != 0) == (moving && motion);
            }
        const u32 retained = g_probe.renderer.content_text->rebuilds,
                  uploaded = g_probe.renderer.text_instance_uploads;
        valid = valid && found && probe_present(&g_probe.renderer, kDefaultTheme) &&
                g_probe.renderer.content_text->rebuilds == retained &&
                g_probe.renderer.text_instance_uploads == uploaded &&
                probe_capture_frame(effect_captures[id - 1]);
    }
    data->user_appearance.name = original_style;
    valid = valid && user_appearance_upsert(&g_probe.users.appearance, data->user_appearance);
    const auto original_bio = data->bio;
    const auto styled_bio = utf8_literal(
        "A **bold** and *italic* bio with ~~struck text~~, ||hidden words|| and `inline code`.");
    copy_bytes(data->bio.data, styled_bio.data, styled_bio.bytes);
    data->bio.bytes = static_cast<u16>(styled_bio.bytes);
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"profile-biography.bmp");
    bool spoiler = false;
    u32 source = 0;
    for (u32 i = 0; i < scene.bio_semantic_count; ++i)
        if (scene.bio_semantics[i].kind == MessageMarkdownSemanticKind::spoiler) {
            spoiler = true;
            source = scene.bio_semantics[i].source_offset;
        }
    valid = valid && spoiler && scene.bio_style_count >= 4;
    scene.bio_revealed[scene.bio_revealed_count++] = source;
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"profile-biography-revealed.bmp");
    TextFormScene top_form{};
    TextFormSurface top_surface{};
    const TextFormContent form_content{utf8_literal("Edit profile"),
                                       utf8_literal("The profile remains below this form."),
                                       utf8_literal("Display name"),
                                       {},
                                       utf8_literal("Cancel"),
                                       utf8_literal("Save"),
                                       utf8_literal("Native edited name"),
                                       0,
                                       0,
                                       true,
                                       true,
                                       {}};
    valid = valid && text_form_update(&top_form, form_content, viewport, &g_probe.text_measure_cache);
    top_surface.scene = &top_form;
    renderer_bind_text_form(&g_probe.renderer, &top_surface);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            g_probe.renderer.form_text_first_batch > g_probe.renderer.overlay_text_first_batch &&
            g_probe.renderer.form_paint_first_batch > g_probe.renderer.profile_foreground_first_batch &&
            g_probe.renderer.media_quad_count > 0 && probe_capture_frame(L"profile-edit-layer.bmp");
    renderer_bind_text_form(&g_probe.renderer, nullptr);
    text_form_surface_release(&top_surface);
    text_form_release(&top_form);
    data->bio = original_bio;
    scene.bio_revealed_count = 0;
    scene.presentation = ProfilePresentation::modal;
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"member-profile-expanded.bmp");
    valid = valid && probe_profile_hover(scene);
    UiRect tab_rectangles[3]{};
    for (u32 selected_tab = 0; selected_tab < 3; ++selected_tab) {
        scene.tab = static_cast<u8>(selected_tab);
        valid = valid && profile_scene_layout(&scene, viewport, true);
        for (u32 i = 0; i < scene.part_count; ++i)
            if (scene.parts[i].look == ProfileLook::tab) {
                const auto* node = ui_widget_manager_layout_node(scene.widgets, scene.parts[i].widget);
                const u32 index = scene.parts[i].source;
                if (!selected_tab)
                    tab_rectangles[index] = node->rectangle;
                else
                    valid = valid && ui_rect_equal(tab_rectangles[index], node->rectangle);
            }
    }
    scene.tab = 0;
    valid = valid && profile_scene_layout(&scene, viewport, true);
    const auto fixed_banner = scene.banner, fixed_avatar = scene.avatar;
    // Add enough details to require scrolling without making the fixed header move.
    data->connection_count = 16;
    for (u32 i = 0; i < 16; ++i) {
        copy_bytes(data->connections[i].type.data, "Service", 7);
        data->connections[i].type.bytes = 7;
        copy_bytes(data->connections[i].name.data, "Connected account", 17);
        data->connections[i].name.bytes = 17;
    }
    if (valid && !profile_scene_layout(&scene, viewport, true)) {
        g_probe.failure_stage = 152;
        valid = false;
    }
    if (valid && !profile_scene_scroll(&scene, 100)) {
        g_probe.failure_stage = 153;
        valid = false;
    }
    if (valid && !(scene.scroll > 0)) {
        g_probe.failure_stage = 154;
        valid = false;
    }
    if (valid &&
        !(ui_rect_equal(scene.banner, fixed_banner) && ui_rect_equal(scene.avatar, fixed_avatar))) {
        g_probe.failure_stage = 155;
        valid = false;
    }
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"member-profile-scrolled.bmp");
    // The owner may edit roles: removable pills and the "+" role picker.
    scene.presentation = ProfilePresentation::popout;
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"member-profile-roles.bmp");
    bool assignable = false, removable = false;
    for (u32 i = 0; i < scene.part_count; ++i) {
        assignable |= scene.parts[i].action == ProfileAction::choose_roles;
        removable |= scene.parts[i].action == ProfileAction::remove_role && scene.parts[i].target == 56;
    }
    valid = valid && assignable && removable;
    // A narrow window: the full profile fills it.
    scene.presentation = ProfilePresentation::modal;
    scene.tab = 1;
    valid = valid && profile_scene_layout(&scene, {0, 0, 640, 600}, true) &&
            ui_rect_width(scene.rectangle) == 640 && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"member-profile-narrow.bmp");
    // Self: the popout's Edit Profile footer.
    scene.presentation = ProfilePresentation::popout;
    scene.self_id = data->user_id;
    bool edit = false;
    valid = valid && profile_scene_layout(&scene, viewport, true);
    for (u32 i = 0; i < scene.part_count; ++i)
        edit |= scene.parts[i].action == ProfileAction::edit;
    valid = valid && edit && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"member-profile-edit.bmp");
    if (guild)
        guild->owner_id = previous_owner;
    scene.active = false;
    g_probe.renderer.profile_scene = nullptr;
    profile_scene_release(&scene);
    const bool was_home = g_probe.chat_state.home_selected;
    g_probe.chat_state.home_selected = false;
    valid = valid && synchronize_probe_shell_layout({}, {}, 0, 320);
    const auto* member_pane = ui_scene_node(g_probe.shell_scene, UiSemantic::member_sidebar);
    // Same Mainframe family, plain role color, and a measured adjacent guild tag.
    MemberListScene members_scene{};
    MemberListRowWidget member_rows[2]{};
    UiWidget member_widgets[10]{};
    UiLayoutNode member_nodes[10]{};
    GuildMemberStore member_store{};
    MemberListRow source_row{4242,
                                    kMemberListPrivateGroup,
                                    ~0u,
                                    0xB88A22,
                                    0,
                                    GuildMemberStatus::online,
                                    MemberListRowKind::member};
    MemberListView projection{};
    projection.rows = {&source_row, 1};
    projection.count = projection.member_count = projection.revision = 1;
    valid = valid && user_store_upsert(&g_probe.users, 4242, utf8_literal("Robin-san")).ok() &&
            member_list_scene_initialize(&members_scene, {member_rows, 2}, {member_widgets, 10},
                                         {member_nodes, 10});
    members_scene.measure = &g_probe.text_measure_cache;
    valid = valid &&
            member_list_scene_update(&members_scene, &projection, &member_store, &g_probe.users,
                                     &g_probe.chat_state, member_pane ? member_pane->rectangle : UiRect{}, 0)
                .ok();
    if (!valid)
        g_probe.failure_stage = 231;
    UiTextCommand member_commands[kMaximumShellChromeTextCommands]{};
    UiTextCommandStream member_text{};
    char member_text_bytes[4096]{};
    valid =
        valid &&
        ui_text_command_stream_initialize(&member_text, {member_commands, kMaximumShellChromeTextCommands},
                                          {member_text_bytes, 4096}) &&
        shell_text_presentation_build(g_probe.shell_scene, g_probe.chat_state, g_probe.navigation,
                                      kDefaultTheme, g_probe.shell_navigation_scene,
                                      &g_probe.message_composer_scene, &members_scene, &member_text);
    UiTextMeasureRequest measured_name{
        {0x50524F46494C45, 1, 0, 0, 0}, utf8_literal("Robin-san"), {}, {400, 40}, true, UiTextFace::normal};
    UiTextMeasureValue normal_width{}, custom_width{}, cached_width{};
    valid = valid &&
            ui_text_measure_cache_measure(&g_probe.text_measure_cache, measured_name, &normal_width).ok();
    measured_name.face = UiTextFace::techno;
    valid = valid &&
            ui_text_measure_cache_measure(&g_probe.text_measure_cache, measured_name, &custom_width).ok() &&
            custom_width.size.width != normal_width.size.width;
    measured_name.face = UiTextFace::normal;
    valid = valid &&
            ui_text_measure_cache_measure(&g_probe.text_measure_cache, measured_name, &cached_width).ok() &&
            cached_width.size.width == normal_width.size.width;
    bool member_plain = false;
    for (u32 i = 0; i < member_text.count; ++i)
        if (utf8_equal(member_commands[i].text, utf8_literal("Robin-san"))) {
            const auto& c = member_commands[i];
            const auto badge = member_list_badge(members_scene, 0);
            member_plain = c.face == UiTextFace::techno && !c.effect.kind && c.color.red > 0.71f &&
                           c.color.red < .73f && badge.rectangle.left > c.rectangle.left + 70 &&
                           badge.rectangle.right < viewport.right - 40;
        }
    if (valid)
        g_probe.failure_stage = 230;
    g_probe.renderer.member_list_scene = &members_scene;
    if (valid) {
        g_probe.failure_stage = 232;
        valid = rebuild_probe_content_text();
    }
    if (valid) {
        g_probe.failure_stage = 233;
        valid = probe_present(&g_probe.renderer, kDefaultTheme);
    }
    if (valid) {
        g_probe.failure_stage = 234;
        valid = probe_capture_frame(L"member-name-and-tag.bmp");
    }
    if (valid)
        g_probe.failure_stage = 235;
    valid = valid && member_plain;
    g_probe.renderer.member_list_scene = nullptr;
    g_probe.chat_state.home_selected = was_home;
    valid = synchronize_probe_shell_layout() && valid;
    VirtualFree(data, 0, MEM_RELEASE);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme);
    if (valid)
        g_probe.failure_stage = 0;
    return valid;
}

// Rich presence: the popout's first card (a game with both images, party,
// timer and buttons) with the next one peeking, then every card in the full
// profile's Activity tab (Spotify with its progress bar, an episode), from
// synthetic activities (no network).
bool probe_profile_activity_card() noexcept {
    g_probe.failure_stage = 740;
    auto* data = static_cast<UserProfile*>(
        VirtualAlloc(nullptr, sizeof(UserProfile), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!data)
        return false;
    u32 scratch[2048];
    bool valid = user_profile_decode(
        utf8_literal(R"({"user":{"id":"4343","username":"activity.test","global_name":"Activity Probe"},"user_profile":{"bio":"Listening and playing."}})"),
        {scratch, 2048}, 4343, 0, data);
    const auto presence = utf8_literal(R"({"activities":[
        {"type":2,"name":"Spotify","details":"Native Song Title","state":"First Artist; Second Artist",
         "created_at":1700000000300,"party":{"id":"spotify:probe"},
         "timestamps":{"start":1700000000000,"end":1700000200000},
         "assets":{"large_image":"spotify:ab67616d0000b2730000000000000000000000aa","large_text":"Probe Album"}},
        {"type":0,"name":"Mossrun","application_id":"500000000000000001","details":"Chapter 7","state":"Ascent",
         "created_at":1700000000100,"timestamps":{"start":1700000000000},"party":{"id":"p","size":[2,4]},
         "buttons":["Open Map","Party Finder"],
         "assets":{"large_image":"500000000000000002","large_text":"High Ridge",
                   "small_image":"500000000000000003","small_text":"Wanderer"}},
        {"type":3,"name":"Cinema","details":"A Long Synthetic Film Title That Runs Past The Card","state":"Part One",
         "created_at":1700000000200,"timestamps":{"start":1700000000000},
         "assets":{"large_image":"mp:external/probe/https/example.invalid/poster.png",
                   "large_text":"Season 2, Episode 5"}}]})");
    UserActivitySet activities{};
    bool has = false;
    const auto scan = json_scan_structurals_avx1(presence, {scratch, 2048});
    valid = valid && scan.ok() &&
            user_activities_decode({presence, {scratch, scan.count}}, {presence, 0, JsonValueKind::object}, 4343,
                                   &activities, &has) &&
            has && activities.count == 3;
    // Spotify half way through, the game 12:34 in, the film an hour in.
    const u64 now = clock_unix_milliseconds();
    activities.items[0].start_ms = now - 100'000;
    activities.items[0].end_ms = now + 100'000;
    activities.items[1].start_ms = now - 754'000;
    activities.items[2].start_ms = now - 3'723'000;
    valid = valid && user_activity_upsert(&g_probe.users.activities, activities);
    ProfileScene scene{};
    scene.phase = ProfilePhase::ready;
    scene.measure = &g_probe.text_measure_cache;
    const UiRect viewport{0, 0, static_cast<float>(g_probe.renderer.width) * 96 / g_probe.renderer.dpi,
                          static_cast<float>(g_probe.renderer.height) * 96 / g_probe.renderer.dpi};
    valid = valid && profile_scene_open(&scene, data, &g_probe.chat_state, &g_probe.users, 1,
                                        {viewport.right - 100, 90, viewport.right, 134}, viewport);
    PngImage image{};
    u8 pixels[32 * (32 * 4 + 1)]{};
    valid = valid && png_image_configure(&image, 32, 32, 8, 6);
    for (u32 y = 0; y < 32; ++y)
        for (u32 x = 0; x < 32; ++x) {
            const u32 at = y * 129 + 1 + x * 4;
            pixels[at] = static_cast<u8>(40 + x * 6);
            pixels[at + 1] = 160;
            pixels[at + 2] = static_cast<u8>(40 + y * 6);
            pixels[at + 3] = 255;
        }
    u32 cards = 0, progress = 0, artwork = 0;
    // Every resolved image is resident, the avatar included.
    const auto upload = [&]() noexcept {
        cards = progress = artwork = 0;
        for (u32 i = 0; i < scene.part_count; ++i) {
            const auto media = scene.parts[i].media;
            progress += media == ProfileMediaKind::activity_progress;
            cards += media == ProfileMediaKind::activity_large;
            ProfileMedia resolved{};
            if (!profile_media_resolve(scene, i, &resolved))
                continue;
            artwork += media == ProfileMediaKind::activity_large || media == ProfileMediaKind::activity_small;
            if (!renderer_upload_png_scanlines(&g_probe.renderer, resolved.key, image, {pixels, sizeof(pixels)}).ok())
                return false;
        }
        return true;
    };
    // The avatar's whole circle is queued with the profile media, which draws
    // after the card body's paint: one quad on the avatar, its clip and its
    // rounded mask both holding the full circle.
    const auto avatar_above_body = [&]() noexcept {
        const auto& r = g_probe.renderer;
        const UiRect a = scene.avatar;
        const auto holds = [&](UiRect outer) noexcept {
            return outer.left <= a.left && outer.top <= a.top && outer.right >= a.right && outer.bottom >= a.bottom;
        };
        if (r.profile_paint_first_batch >= r.profile_foreground_first_batch)
            return false;
        for (u32 b = r.profile_media_first_batch; b < r.profile_effect_media_first_batch && b < r.media_batch_count;
             ++b) {
            const auto& batch = r.media_draw.batch_data[b];
            for (u32 q = batch.first_quad; q < batch.first_quad + batch.quad_count; ++q) {
                const auto& quad = r.media_draw.quad_data[q];
                if (ui_rect_equal({quad.left, quad.top, quad.right, quad.bottom}, a) && holds(batch.clip) &&
                    holds(quad.rounded_clip) && quad.radius == ui_rect_width(a) * .5f)
                    return true;
            }
        }
        return false;
    };
    valid = valid && upload() && cards == 1 && progress == 0 && artwork == 2 && scene.activity_timed;
    g_probe.renderer.profile_scene = &scene;
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            avatar_above_body() && probe_capture_frame(L"profile-activity-card.bmp");
    if (valid)
        g_probe.failure_stage = 741;
    // The full profile lists every activity under Activity.
    scene.presentation = ProfilePresentation::modal;
    scene.tab = 0;
    valid = valid && profile_scene_layout(&scene, viewport, true) && upload() && cards == 3 && progress == 1 &&
            artwork == 4 &&
            rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) && avatar_above_body() &&
            probe_capture_frame(L"profile-activity-tab.bmp");
    if (valid)
        g_probe.failure_stage = 743;
    // The popout with Spotify alone: album art, track, artists and the bar.
    UserActivitySet song{};
    song.user_id = 4343;
    song.count = 1;
    song.items[0] = activities.items[0];
    scene.presentation = ProfilePresentation::popout;
    valid = valid && user_activity_upsert(&g_probe.users.activities, song) &&
            profile_scene_layout(&scene, viewport, true) && upload() && cards == 1 && progress == 1 &&
            rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"profile-activity-spotify.bmp");
    g_probe.renderer.profile_scene = nullptr;
    profile_scene_release(&scene);
    if (valid)
        g_probe.failure_stage = 744;
    // Member rows: a glyph, then the short activity text or the custom status.
    const bool was_home = g_probe.chat_state.home_selected;
    g_probe.chat_state.home_selected = false;
    valid = valid && synchronize_probe_shell_layout({}, {}, 0, 320);
    const auto* member_pane = ui_scene_node(g_probe.shell_scene, UiSemantic::member_sidebar);
    const auto row_presence = utf8_literal(R"({"activities":[
        {"type":4,"name":"Custom Status","state":"Back at six"},
        {"type":0,"name":"Turbo Ball","details":"Duel"}]})");
    UserActivitySet game{}, custom_game{};
    UserAppearance custom_line{};
    custom_line.user_id = 4346;
    const auto row_scan = json_scan_structurals_avx1(row_presence, {scratch, 2048});
    valid = valid && row_scan.ok() &&
            user_activities_decode({row_presence, {scratch, row_scan.count}},
                                   {row_presence, 0, JsonValueKind::object}, 4346, &custom_game, &has) &&
            activity_decode({row_presence, {scratch, row_scan.count}}, {row_presence, 0, JsonValueKind::object},
                            &custom_line);
    game = custom_game;
    game.user_id = 4345;
    song.user_id = 4344;
    valid = valid && user_store_upsert(&g_probe.users, 4344, utf8_literal("Listener")).ok() &&
            user_store_upsert(&g_probe.users, 4345, utf8_literal("Player")).ok() &&
            user_store_upsert(&g_probe.users, 4346, utf8_literal("Busy Player")).ok() &&
            user_activity_upsert(&g_probe.users.activities, song) &&
            user_activity_upsert(&g_probe.users.activities, game) &&
            user_activity_upsert(&g_probe.users.activities, custom_game) &&
            user_appearance_upsert(&g_probe.users.appearance, custom_line);
    MemberListScene members_scene{};
    MemberListRowWidget member_rows[3]{};
    UiWidget member_widgets[16]{};
    UiLayoutNode member_nodes[16]{};
    GuildMemberStore member_store{};
    MemberListRow source_rows[3]{};
    for (u32 i = 0; i < 3; ++i)
        source_rows[i] = {4344 + i, kMemberListPrivateGroup, ~0u, 0, 0, GuildMemberStatus::online,
                          MemberListRowKind::member};
    MemberListView projection{};
    projection.rows = {source_rows, 3};
    projection.count = projection.member_count = 3;
    projection.revision = 1;
    valid = valid && member_list_scene_initialize(&members_scene, {member_rows, 3}, {member_widgets, 16},
                                                  {member_nodes, 16});
    members_scene.measure = &g_probe.text_measure_cache;
    valid = valid && member_list_scene_update(&members_scene, &projection, &member_store, &g_probe.users,
                                              &g_probe.chat_state, member_pane ? member_pane->rectangle : UiRect{}, 0)
                         .ok();
    UiActivityStatus lines[3]{};
    char line_text[3][256];
    for (u32 i = 0; valid && i < 3; ++i)
        valid = member_list_row_status(members_scene, i, {line_text[i], 256}, &lines[i]);
    valid = valid && lines[0].glyph == UiActivityGlyph::music &&
            utf8_equal(lines[0].text, utf8_literal("First Artist, Second Artist")) &&
            lines[1].glyph == UiActivityGlyph::game && utf8_equal(lines[1].text, utf8_literal("Turbo Ball")) &&
            lines[2].glyph == UiActivityGlyph::game && utf8_equal(lines[2].text, utf8_literal("Back at six"));
    g_probe.renderer.member_list_scene = &members_scene;
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"member-activity-lines.bmp");
    g_probe.renderer.member_list_scene = nullptr;
    g_probe.chat_state.home_selected = was_home;
    valid = synchronize_probe_shell_layout() && valid;
    UserActivitySet cleared{};
    for (u64 user = 4343; user <= 4346; ++user) {
        cleared.user_id = user;
        user_activity_upsert(&g_probe.users.activities, cleared);
    }
    VirtualFree(data, 0, MEM_RELEASE);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme);
    if (valid)
        g_probe.failure_stage = 0;
    return valid;
}

bool probe_rich_composer() noexcept {
    g_probe.failure_stage = 140;
    char buffer[2048]{};
    MessageComposer model{};
    MessageComposerScene scene{};
    bool valid = message_composer_initialize(&model, {buffer, sizeof(buffer)}) &&
                 message_composer_scene_initialize_owned(&scene);
    message_composer_reset(&model, 42, false);
    UiRect viewport{50, 100, 550, 148};
    float preferred = 48;
    const auto refresh = [&]() noexcept {
        if (!message_composer_scene_update(&scene, &model, viewport, true).ok() ||
            !renderer_bind_message_composer_scene(&g_probe.renderer, &scene) ||
            !renderer_measure_message_composer(&g_probe.renderer, &preferred))
            return false;
        viewport.bottom = viewport.top + preferred;
        return message_composer_scene_update(&scene, &model, viewport, true).ok() &&
               renderer_measure_message_composer(&g_probe.renderer, &preferred);
    };
    valid = valid && refresh();
    bool wrapped = false;
    for (u32 i = 0; valid && i < 256 && !wrapped; ++i) {
        valid = message_composer_insert_scalar(&model, 'i') == MessageComposerResult::changed && refresh();
        const u32 line_count = text_edit_layout_line_count(g_probe.renderer.composer_layout);
        valid = valid && line_count;
        wrapped = line_count > 1;
        valid = valid && (wrapped ? preferred > 48 : preferred == 48);
    }
    valid = valid && wrapped;
    if (valid)
        log_write(LogLevel::info, "probe", "composer_height_changes_only_on_native_wrap");
    g_probe.failure_stage = 141;
    const auto rich = utf8_literal("**Bold** *italic* `code` @everyone\n```cpp\nint x = "
                                   "1;\n```\none\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n");
    valid = valid && message_composer_restore(&model, 42, rich, rich.bytes, rich.bytes) && refresh() &&
            preferred == 208;
    auto& layout = g_probe.renderer.composer_layout;
    // Styled composer geometry is native; its parity with DirectWrite edit
    // layouts is covered by the native app-line editor corpora.
    valid = valid && layout.geometry;
    const float bottom = layout.scroll_y;
    g_probe.failure_stage = 142;
    const auto native_measures =
        native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_measured;
    valid = valid && bottom > 0 && text_edit_layout_scroll(&layout, -bottom) && layout.scroll_y == 0 &&
            refresh() && layout.scroll_y == 0 && layout.geometry;
    valid = valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_measured ==
                         native_measures;
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme);
    bool blue_mention = false;
    const auto* edit_node = message_composer_scene_layout_node(scene, MessageComposerControl::edit);
    const auto* text_storage = g_probe.renderer.content_text;
    for (u32 i = 0; valid && edit_node && text_storage && i < text_storage->quad_count; ++i) {
        const auto& quad = text_storage->quads[i];
        blue_mention |= quad.left >= edit_node->content_rectangle.left &&
                        quad.right <= edit_node->content_rectangle.right &&
                        quad.top >= edit_node->content_rectangle.top &&
                        quad.bottom <= edit_node->content_rectangle.bottom &&
                        bytes_equal(&quad.color, &kDefaultTheme.accent, sizeof(Color));
    }
    valid = valid && blue_mention;
    // Moving the viewport without editing must not undo an intentional wheel scroll.
    viewport.top += 4;
    viewport.bottom += 4;
    valid = valid && refresh() && layout.scroll_y == 0 && layout.geometry;
    g_probe.failure_stage = 143;
    u32 hit = ~0u;
    valid = valid && text_edit_layout_hit_test(layout, layout.origin_x, layout.viewport_top + 2, &hit) &&
            hit == 0 && message_composer_set_selection(&model, 0, 0) == MessageComposerResult::changed &&
            refresh() &&
            message_composer_move(&model, MessageComposerMove::document_end, false) ==
                MessageComposerResult::changed &&
            refresh() && layout.scroll_y > 0 &&
            layout.caret.top + layout.caret.height <= layout.viewport_top + layout.height + .01f;
    g_probe.failure_stage = 144;
    if (!valid)
        return false;
    const auto code = utf8_literal("```cpp\nint x = 1;\n```");
    model.focused = true;
    message_composer_scene_set_caret_visible(&scene, true);
    valid = valid && message_composer_restore(&model, 42, code, 7, 10) && refresh() &&
            renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme);
    u32 background = ~0u, selected = ~0u, glyph = ~0u;
    for (u32 i = 0; valid && i < text_storage->quad_count; ++i) {
        const auto& q = text_storage->quads[i];
        if (q.left < viewport.left || q.right > viewport.right || q.top < viewport.top ||
            q.bottom > viewport.bottom)
            continue;
        if (q.u0 == -1 && q.color.alpha == .38f)
            selected = i;
        else if (q.u0 == -1)
            background = i;
        else if (selected != ~0u)
            glyph = i;
    }
    valid = valid && background != ~0u && selected != ~0u && glyph != ~0u && background < selected &&
            selected < glyph &&
            g_probe.renderer.edit_overlay_paint_first_batch < g_probe.renderer.paint_stream.batch_count &&
            probe_capture_frame(L"code-selection.bmp");
    const auto text_builds = g_probe.text.rebuilds;
    message_composer_scene_set_caret_visible(&scene, false);
    valid =
        valid && probe_present(&g_probe.renderer, kDefaultTheme) && g_probe.text.rebuilds == text_builds;
    message_composer_scene_set_caret_visible(&scene, true);
    valid = valid && message_composer_set_selection(&model, 10, 10) == MessageComposerResult::changed &&
            refresh() && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) && probe_capture_frame(L"code-caret.bmp");
    g_probe.failure_stage = 150;
    // A caret blink rebuilds paint only: after one present synchronizes the
    // other flip buffer, each toggle presents just the caret's damage.
    for (u32 toggle = 0; valid && toggle < 3; ++toggle) {
        const auto& r = g_probe.renderer;
        const u32 builds = r.paint_stream_builds, partial = r.partial_present_draws, full = r.full_present_draws,
                  diffs = r.paint_diff.partial_rebuilds, shaped = g_probe.text.rebuilds;
        const u64 pixels = r.partial_present_pixels;
        message_composer_scene_set_caret_visible(&scene, toggle % 2 != 0);
        valid = probe_present(&g_probe.renderer, kDefaultTheme) && r.paint_stream_builds == builds + 1 &&
                r.paint_diff.partial_rebuilds == diffs + 1 && g_probe.text.rebuilds == shaped &&
                (toggle == 0 || (r.full_present_draws == full && r.partial_present_draws == partial + 1 &&
                                 r.partial_present_pixels - pixels < static_cast<u64>(r.width) * r.height / 16));
        log_u64(valid ? LogLevel::info : LogLevel::error, "probe", "caret_blink_partial_present_pixels",
                r.partial_present_pixels - pixels);
        if (!valid) {
            log_u64(LogLevel::error, "probe", "caret_blink_toggle", toggle);
            log_u64(LogLevel::error, "probe", "caret_blink_builds", r.paint_stream_builds - builds);
            log_u64(LogLevel::error, "probe", "caret_blink_diff_partials", r.paint_diff.partial_rebuilds - diffs);
            log_u64(LogLevel::error, "probe", "caret_blink_full_presents", r.full_present_draws - full);
        }
    }
    g_probe.failure_stage = 151;
    const auto directional_edit =
        utf8_literal("A \xd7\x90\xd7\x91\xd7\x92 \xf0\x9f\x98\x80 B\n\xd7\x90\xd7\x91 123 CD");
    valid = valid && message_composer_restore(&model, 42, directional_edit, 2, 8) && refresh();
    // Native directional editor geometry; its parity with DirectWrite edit
    // layouts is covered by the native app-line directional editor corpora.
    const auto native_editor_geometry = [&]() noexcept {
        if (!layout.geometry || !layout.selection_count)
            return false;
        for (u32 position = 0; position <= layout.utf16_units; ++position) {
            TextEditMetric metric{};
            if (!text_edit_layout_position_geometry(layout, position, &metric) || !(metric.height > 0))
                return false;
        }
        return true;
    };
    valid = valid && native_editor_geometry();
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"composer-bidi-selection.bmp");
    valid = valid && message_composer_set_selection(&model, 8, 8) == MessageComposerResult::changed &&
            refresh() && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"composer-bidi-caret.bmp");
    TextCompositionSpan directional_composition{};
    directional_composition.start = 2;
    directional_composition.end = 5;
    directional_composition.line_style = TextCompositionLineStyle::solid;
    scene.composition = {&directional_composition, 1};
    ++scene.composition_revision;
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"composer-bidi-ime.bmp");
    scene.composition = {};
    ++scene.composition_revision;
    g_probe.failure_stage = 152;
    const auto cluster_text = utf8_literal("A \xd7\xa9\xd6\xb8\xd6\xbc\xd7\x81 \xf0\x9f\x98\x80 B");
    valid = valid && message_composer_restore(&model, 42, cluster_text, 0, 6) && refresh() &&
            scene.display_styles;
    // Exercise the renderer's projected-style boundary directly, including a
    // property range inside a grapheme; source parsing is tested separately.
    if (valid) {
        scene.display_styles[0] = {8, cluster_text.bytes - 8, text_style_underline};
        scene.display_style_count = 1;
        valid = renderer_measure_message_composer(&g_probe.renderer, &preferred);
    }
    valid = valid && native_editor_geometry();
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"composer-cluster-cut-selection.bmp");
    directional_composition.start = 2;
    directional_composition.end = 6;
    scene.composition = {&directional_composition, 1};
    ++scene.composition_revision;
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"composer-cluster-cut-ime.bmp");
    scene.composition = {};
    ++scene.composition_revision;
    const auto selector_text = utf8_literal("A\xef\xb8\x8f B \xf0\x9f\x98\x80");
    valid = valid && message_composer_restore(&model, 42, selector_text, 0, 4) && refresh() &&
            scene.display_styles;
    if (valid) {
        scene.display_styles[0] = {0, 1, text_style_underline};
        scene.display_style_count = 1;
        valid = renderer_measure_message_composer(&g_probe.renderer, &preferred);
    }
    valid = valid && native_editor_geometry();
    if (valid)
        log_write(LogLevel::info, "probe", "native_composer_cluster_cuts_match_reference_geometry");
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"composer-selector-cut.bmp");
    g_probe.failure_stage = 153;
    const auto arabic_text =
        utf8_literal("\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7\x20\xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7"
                     "\xd9\x84\xd9\x85\x20\xd9\xa1\xd9\xa2\xd9\xa3\x20\xf0\x9f\x98\x80\x0a\x48\x65\x6c\x6c"
                     "\x6f\x20\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d\x20\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7"
                     "\x20\xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85");
    valid = valid && message_composer_restore(&model, 42, arabic_text, 0, arabic_text.bytes) && refresh();
    valid = valid && renderer_measure_message_composer(&g_probe.renderer, &preferred);
    valid = valid && native_editor_geometry();
    if (valid)
        log_write(LogLevel::info, "probe", "native_arabic_composer_matches_reference_geometry");
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"composer-arabic-selection.bmp");
    directional_composition.start = 0;
    directional_composition.end = 5;
    scene.composition = {&directional_composition, 1};
    ++scene.composition_revision;
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"composer-arabic-ime.bmp");
    scene.composition = {};
    ++scene.composition_revision;
    g_probe.failure_stage = 150;
    const auto inline_emoji = utf8_literal("A <:party:31337> B");
    u8 emoji_pixels[16 * 16 * 4]{};
    for (u32 i = 0; i < 16 * 16; ++i) {
        emoji_pixels[i * 4] = 220;
        emoji_pixels[i * 4 + 1] = 160;
        emoji_pixels[i * 4 + 3] = 255;
    }
    valid = valid &&
            renderer_upload_media_rgba8(&g_probe.renderer, media_asset_custom_emoji(31337, false), 16, 16,
                                        64, {emoji_pixels, sizeof(emoji_pixels)})
                .ok() &&
            message_composer_restore(&model, 42, inline_emoji, inline_emoji.bytes, inline_emoji.bytes) &&
            refresh() && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme);
    UiRect emoji_rect{}, emoji_clip{};
    valid = valid && scene.projection_span_count == 1 &&
            text_composer_emoji_rectangle(layout, scene, 0, &emoji_rect, &emoji_clip);
    bool drawn_emoji = false;
    for (u32 i = 0; valid && i < g_probe.renderer.media_quad_count; ++i) {
        const auto& q = g_probe.renderer.media_draw.quad_data[i];
        drawn_emoji |= q.left == emoji_rect.left && q.top == emoji_rect.top &&
                       q.right == emoji_rect.right && q.bottom == emoji_rect.bottom;
    }
    valid = valid && drawn_emoji && probe_capture_frame(L"composer-custom-emoji.bmp");
    renderer_bind_message_composer_scene(&g_probe.renderer, &g_probe.message_composer_scene);
    message_composer_scene_release(&scene);
    message_composer_release(&model);
    if (valid)
        log_write(LogLevel::info, "probe",
                  "composer_rich_fonts_manual_scroll_hit_test_and_caret_reveal_hardware_passed");
    return valid;
}

bool probe_attachment_save_affordances() noexcept {
    if (!g_probe.message_scene.count) return false;
    const auto* record = message_store_resolve(g_probe.message_view.store, g_probe.message_scene.rows[0].message);
    if (!record) return false;
    GatewayMessageAttachment attachments[3]{};
    const Utf8View names[]{utf8_literal("release-notes-with-a-long-filename.txt"),
                           utf8_literal("original-native-art.png"), utf8_literal("native-preview.mp4")};
    const Utf8View types[]{utf8_literal("text/plain"),utf8_literal("image/png"),utf8_literal("video/mp4")};
    const Utf8View urls[]{
        utf8_literal("https://cdn.discordapp.com/attachments/111/811001/release.txt?ex=ffffffff&is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
        utf8_literal("https://cdn.discordapp.com/attachments/111/811002/art.png?ex=ffffffff&is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
        utf8_literal("https://cdn.discordapp.com/attachments/111/811003/preview.mp4?ex=ffffffff&is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")};
    u8 pixels[48 * 16 * 4]{};
    for (u32 i = 0; i < 48 * 16; ++i) {
        pixels[i * 4] = static_cast<u8>(40 + i % 48 * 4);
        pixels[i * 4 + 1] = 150; pixels[i * 4 + 2] = 205; pixels[i * 4 + 3] = 255;
    }
    for (u32 i = 0; i < 3; ++i) {
        attachments[i].id = 811001 + i; attachments[i].filename = names[i];
        attachments[i].content_type = types[i]; attachments[i].url = urls[i];
        attachments[i].size = 1024;
        if (i) {
            attachments[i].width = 192; attachments[i].height = 64;
            attachments[i].has_width = attachments[i].has_height = true;
            const auto format = i == 1 ? MediaAttachmentFormat::png : MediaAttachmentFormat::mp4;
            if (!renderer_upload_media_rgba8(&g_probe.renderer,
                media_asset_attachment(attachments[i].id, format,
                    media_resolution_tier_for_display(192,64,g_probe.renderer.dpi,media_preview_extent_cap(format))),
                48,16,192,{pixels,sizeof(pixels)}).ok()) return false;
        }
    }
    GatewayMessageEvent event{}; event.id = record->id; event.fields = gateway_message_field_attachments;
    event.attachments = {attachments,3};
    if (!message_rich_store_apply(&g_probe.rich_store,record->id,record->revision,event).ok() ||
        !rebuild_probe_content_text()) return false;
    for (u32 i = 0; i < 3; ++i) {
        const auto handle = message_scene_rich_item_widget(g_probe.message_scene.rows[0],MessageSceneRichItemKind::attachment,i);
        if (!ui_widget_manager_focus(&g_probe.message_scene.widgets,handle)) return false;
        renderer_bind_focus_owner(&g_probe.renderer,&g_probe.message_scene.widgets);
        if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer,kDefaultTheme) ||
            !probe_capture_frame(i == 0 ? L"attachment-save-file.bmp" : i == 1 ? L"attachment-save-image.bmp" : L"attachment-save-video.bmp")) return false;
    }
    log_write(LogLevel::info,"probe","attachment_save_file_image_video_original_affordances_passed");
    return true;
}

bool probe_image_viewer() noexcept {
    const float scale = 96.0f / g_probe.renderer.dpi;
    const UiRect viewport{0, 0, g_probe.renderer.width * scale, g_probe.renderer.height * scale};
    g_probe.failure_stage = 130;
    UiImageViewer image_viewer{};
    PngImage expanded_image{};
    u8 expanded_pixels[16 * (32 * 4 + 1)]{};
    const auto expanded_key =
        media_asset_attachment(991991, MediaAttachmentFormat::png, MediaResolutionTier::px2048);
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 32; ++x) {
            const u32 offset = y * 129 + 1 + x * 4;
            expanded_pixels[offset] = 240;
            expanded_pixels[offset + 3] = 255;
        }
    if (!png_image_configure(&expanded_image, 32, 16, 8, 6) ||
        !renderer_upload_png_scanlines(&g_probe.renderer, expanded_key, expanded_image,
                                       {expanded_pixels, sizeof(expanded_pixels)})
             .ok() ||
        !ui_image_viewer_open(&image_viewer, viewport, {1024, 512}) ||
        !ui_image_viewer_set_caption(&image_viewer, utf8_literal("probe-image.png")))
        return false;
    g_probe.failure_stage = 131;
    g_probe.renderer.image_viewer = &image_viewer;
    g_probe.renderer.image_viewer_key = expanded_key;
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.popup_media_first_batch >= g_probe.renderer.media_batch_count ||
        g_probe.renderer.interaction_text_image_viewer_revision != image_viewer.revision)
        return false;
    g_probe.failure_stage = 132;
    const auto viewer_quad = g_probe.renderer.media_draw.quad_data[g_probe.renderer.media_quad_count - 1];
    if (viewer_quad.left != image_viewer.image.left || viewer_quad.bottom != image_viewer.image.bottom)
        return false;
    const ProbePixelSample viewed_image{
        {(image_viewer.image.left + image_viewer.image.right) * .5f,
         (image_viewer.image.top + image_viewer.image.bottom) * .5f}, {240.f / 255.f, 0, 0, 1}, 2};
    const UiRect viewer_footer{image_viewer.body.left, image_viewer.body.bottom + 8,
                               image_viewer.body.right, viewport.bottom};
    if (!probe_exact_pixels({&viewed_image, 1}) || !probe_text_pixels({&viewer_footer, 1}) ||
        !probe_capture_frame(L"viewer-full-brightness.bmp"))
        return false;
    g_probe.failure_stage = 133;
    const u32 viewer_builds = g_probe.renderer.paint_stream_builds;
    if (!probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream_builds != viewer_builds || !probe_exact_pixels({&viewed_image, 1}))
        return false;
    const auto button_rectangle = ui_image_viewer_button(image_viewer, 1);
    const UiPoint button_point{button_rectangle.left + 16, button_rectangle.top + 16};
    const auto hover = ui_button_pointer(&image_viewer.widgets, UiButtonPointerEvent::move, button_point);
    if (!hover.changed)
        return false;
    ++image_viewer.revision;
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream_builds <= viewer_builds || !probe_exact_pixels({&viewed_image, 1}))
        return false;
    const auto down = ui_button_pointer(&image_viewer.widgets, UiButtonPointerEvent::press, button_point);
    if (!down.handled || down.activate)
        return false;
    ++image_viewer.revision;
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"viewer-buttons.bmp"))
        return false;
    if (!ui_button_pointer(&image_viewer.widgets, UiButtonPointerEvent::release, button_point).activate)
        return false;
    ++image_viewer.revision;
    g_probe.failure_stage = 134;
    if (!ui_image_viewer_set_save_available(&image_viewer, true) ||
        !ui_widget_manager_focus(&image_viewer.widgets, image_viewer.buttons[4])) return false;
    image_viewer.keyboard_focus = true;
    ++image_viewer.revision;
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"viewer-save-original.bmp")) return false;
    ui_image_viewer_zoom(&image_viewer, 2,
                         {(viewport.left + viewport.right) * .5f, (viewport.top + viewport.bottom) * .5f});
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream_builds <= viewer_builds || !probe_exact_pixels({&viewed_image, 1}))
        return false;
    // The magnifier over a left-red, right-blue picture: 40 DIPs left of the
    // split the bare image is still red, but the lens shows its red half
    // magnified past where the bare image blends toward blue.
    g_probe.failure_stage = 136;
    PngImage split_image{};
    u8 split_pixels[16 * (32 * 4 + 1)]{};
    const auto split_key = media_asset_attachment(991992, MediaAttachmentFormat::png, MediaResolutionTier::px2048);
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 32; ++x) {
            const u32 offset = y * 129 + 1 + x * 4;
            split_pixels[offset + (x < 16 ? 0 : 2)] = 240;
            split_pixels[offset + 3] = 255;
        }
    if (!png_image_configure(&split_image, 32, 16, 8, 6) ||
        !renderer_upload_png_scanlines(&g_probe.renderer, split_key, split_image,
                                       {split_pixels, sizeof(split_pixels)})
             .ok() ||
        !ui_image_viewer_apply(&image_viewer, UiImageViewerCommand::fit))
        return false;
    g_probe.renderer.image_viewer_key = split_key;
    UiZoomLens lens{};
    ui_zoom_lens_configure(&lens, 6, 20, false);
    const float split_x = (image_viewer.image.left + image_viewer.image.right) * .5f;
    const float middle_y = (image_viewer.image.top + image_viewer.image.bottom) * .5f;
    lens.active = true;
    lens.pointer = {split_x - 40, middle_y};
    ++lens.revision;
    g_probe.renderer.zoom_lens = &lens;
    ++image_viewer.revision;
    const ProbePixelSample lens_samples[]{
        {{lens.pointer.x + 50, middle_y}, {240.f / 255.f, 0, 0, 1}, 3},
        {{lens.pointer.x + 80, middle_y + 80}, {0, 0, 240.f / 255.f, 1}, 3}};
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !g_probe.renderer.zoom_lens_source.ready || !probe_exact_pixels({lens_samples, 2}) ||
        !probe_capture_frame(L"viewer-zoom-lens.bmp"))
        return false;
    g_probe.failure_stage = 137;
    lens.square = true;
    ++lens.revision;
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !g_probe.renderer.zoom_lens_source.ready || !probe_capture_frame(L"viewer-zoom-lens-square.bmp"))
        return false;
    // A pointer off the picture hides the lens; the bare image remains.
    lens.pointer = {image_viewer.image.left - 8, middle_y};
    ++lens.revision;
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.zoom_lens_source.ready)
        return false;
    g_probe.renderer.zoom_lens = nullptr;
    g_probe.renderer.image_viewer_key = expanded_key;
    g_probe.failure_stage = 135;
    g_probe.renderer.image_viewer = nullptr;
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.interaction_text_image_viewer_revision)
        return false;
    log_write(LogLevel::info, "probe", "image_viewer_texture_controls_zoom_and_retention_hardware_passed");
    return true;
}

// The composer's voice message states over a live renderer: the microphone in
// place of Send, the recording bar with its timer and live bars, the review
// bar with the recorded waveform, then the upload and failure states. Each
// state is captured; the timer's glyphs are checked on screen.
bool probe_voice_message_bar() noexcept {
    g_probe.failure_stage = 160;
    char buffer[256]{};
    MessageComposer model{};
    MessageComposerScene scene{};
    bool valid = message_composer_initialize(&model, {buffer, sizeof(buffer)}) &&
                 message_composer_scene_initialize_owned(&scene);
    message_composer_reset(&model, 43, false);
    UiRect viewport{50, 100, 550, 148};
    float preferred = 48;
    UiComposerVoiceView view{};
    const auto show = [&]() noexcept {
        message_composer_scene_set_voice(&scene, &view);
        if (!message_composer_scene_update(&scene, &model, viewport, true).ok() ||
            !renderer_bind_message_composer_scene(&g_probe.renderer, &scene) ||
            !renderer_measure_message_composer(&g_probe.renderer, &preferred))
            return false;
        return renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
               probe_present(&g_probe.renderer, kDefaultTheme);
    };
    const auto bar = [&]() noexcept {
        const auto* edit = message_composer_scene_layout_node(scene, MessageComposerControl::edit);
        return edit ? edit->content_rectangle : UiRect{};
    };
    view.mode = UiComposerVoiceMode::available;
    valid = valid && show() && message_composer_scene_voice_mode(scene) == UiComposerVoiceMode::available &&
            probe_capture_frame(L"composer-voice-available.bmp");
    g_probe.failure_stage = 161;
    view.mode = UiComposerVoiceMode::recording;
    view.pulse = true;
    view.elapsed_ms = 7200;
    view.level_count = kUiComposerVoiceLevels;
    for (u32 i = 0; i < kUiComposerVoiceLevels; ++i)
        view.levels[i] = static_cast<u8>(30 + (i * 53) % 200);
    view.status = utf8_literal("Click the microphone to stop");
    valid = valid && show();
    const auto recording = ui_composer_voice_bar_geometry(bar(), view);
    valid = valid && ui_rect_has_area(recording.dot) && probe_text_pixels({&recording.time, 1}) &&
            probe_text_pixels({&recording.status, 1}) && probe_capture_frame(L"composer-voice-recording.bmp");
    g_probe.failure_stage = 162;
    view.mode = UiComposerVoiceMode::review;
    view.pulse = false;
    view.position_ms = 2300;
    view.playing = true;
    for (u32 i = 0; i < kUiComposerVoiceWaveform; ++i)
        view.waveform[i] = static_cast<u8>(20 + (i * 37) % 220);
    view.status = {};
    valid = valid && show();
    const auto review = ui_composer_voice_bar_geometry(bar(), view);
    valid = valid && probe_text_pixels({&review.time, 1}) && probe_capture_frame(L"composer-voice-review.bmp");
    g_probe.failure_stage = 163;
    view.mode = UiComposerVoiceMode::sending;
    view.playing = false;
    view.progress = .6f;
    valid = valid && show() && probe_capture_frame(L"composer-voice-sending.bmp");
    view.mode = UiComposerVoiceMode::failed;
    view.error = true;
    view.status = utf8_literal("Couldn't upload the voice message");
    valid = valid && show() && probe_capture_frame(L"composer-voice-failed.bmp");
    message_composer_scene_set_voice(&scene, nullptr);
    valid = valid && renderer_bind_message_composer_scene(&g_probe.renderer, &g_probe.message_composer_scene);
    if (valid)
        log_write(LogLevel::info, "probe", "composer_voice_message_states_hardware_passed");
    return valid;
}

} // namespace ui_probe_detail

namespace {

bool reaction_picker_probe_resolve(void*, u32 index, UiMediaPickerItem* item) noexcept {
    *item = {};
    item->id = 0x5e000ull + index;
    item->label = utf8_literal("Original grin");
    item->kind = UiMediaPickerItemKind::emoji;
    item->enabled = true;
    item->glyph = utf8_literal("\xF0\x9F\x98\x80");
    return true;
}

} // namespace

namespace ui_probe_detail {

// Reactions open the unified media picker's Emoji component (reaction
// variant). A server section shows its icon in the category rail: the icon
// draws from the shared media atlas and stays clipped to the rail while the
// rail scrolls independently of the grid.
bool probe_reaction_picker_server_icon() noexcept {
    auto& renderer = g_probe.renderer;
    g_probe.failure_stage = 72;
    if (!renderer.ui_scene || !renderer.media_atlas || !synchronize_probe_shell_layout())
        return false;
    constexpr u64 guild_id = 0x5e77;
    constexpr u32 icon_variant = 3;
    UiMediaPickerSection sections[20]{};
    for (u32 i = 0; i < 20; ++i) {
        sections[i] = {0x5e100ull + i, i * 8, 8, utf8_literal("Original Emoji"), true};
        sections[i].visual = static_cast<UiMediaPickerSectionVisual>(1 + i % 10);
    }
    sections[0].label = utf8_literal("Native art studio");
    sections[0].visual = UiMediaPickerSectionVisual::guild;
    sections[0].guild_id = guild_id;
    sections[0].icon_variant = icon_variant;
    const UiMediaPickerSource source{
        nullptr, 0x5e, 1, 160, 1, {sections, 20}, reaction_picker_probe_resolve};
    UiMediaPickerContent content{};
    content.edit = {utf8_literal(""), 0, 0, true, false, false};
    content.placeholder = utf8_literal("Find the perfect emoji");
    content.account_scope = source.account_scope;
    content.query_generation = source.query_generation;
    content.revision = 1;
    content.kind = UiMediaPickerKind::emoji;
    content.phase = UiMediaPickerPhase::ready;
    content.available_tabs = ui_media_picker_tab_bit(UiMediaPickerKind::emoji);
    content.variant = UiMediaPickerVariant::reaction;
    // Solid original artwork, distinct from every panel and glyph colour.
    u8 pixels[32 * 32 * 4]{};
    for (u32 i = 0; i < 32 * 32; ++i) {
        pixels[i * 4] = 36;
        pixels[i * 4 + 1] = 196;
        pixels[i * 4 + 2] = 160;
        pixels[i * 4 + 3] = 255;
    }
    const Color icon{36 / 255.f, 196 / 255.f, 160 / 255.f, 1};
    if (!renderer_upload_media_rgba8(&renderer, media_asset_guild_icon(guild_id, icon_variant, false), 32,
                                     32, 128, {pixels, sizeof(pixels)})
             .ok())
        return false;
    UiMediaPickerScene scene{};
    const auto present = [&]() noexcept {
        renderer.media_picker_scene = &scene;
        renderer.retained_presentation.valid = false;
        return rebuild_probe_content_text() && probe_present(&renderer, kDefaultTheme);
    };
    // The rail icon is one primary-atlas quad over exactly its slot, batched
    // under the slot's rail clip, and its pixels show the uploaded artwork.
    const auto drawn = [&](const UiMediaPickerRailMediaSlot& slot) noexcept {
        bool queued = false;
        for (u32 b = renderer.media_picker_media_first_batch; b < renderer.media_picker_media_end_batch;
             ++b) {
            const auto& batch = renderer.media_draw.batch_data[b];
            if (!ui_rect_equal(batch.clip, slot.clip))
                continue;
            for (u32 q = batch.first_quad; q < batch.first_quad + batch.quad_count; ++q) {
                const auto& quad = renderer.media_draw.quad_data[q];
                queued |= renderer.media_draw.domain_data[q] == 0 &&
                          ui_rect_equal({quad.left, quad.top, quad.right, quad.bottom}, slot.rectangle);
            }
        }
        const ProbePixelSample sample{
            {(slot.clip.left + slot.clip.right) * .5f, (slot.clip.top + slot.clip.bottom) * .5f}, icon, 8};
        return queued && probe_exact_pixels({&sample, 1});
    };
    const auto run = [&]() noexcept {
        const auto viewport = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::content);
        const UiRect anchor{viewport.right - 48, viewport.top + 24, viewport.right - 24, viewport.top + 48};
        g_probe.failure_stage = 73;
        UiMediaPickerRailMediaSlot slot{};
        if (!ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) ||
            scene.variant != UiMediaPickerVariant::reaction || !ui_rect_has_area(scene.rail) ||
            !ui_media_picker_scene_rail_media(scene, 0, &slot) || slot.guild_id != guild_id ||
            slot.variant != icon_variant || slot.animated || !ui_rect_equal(slot.clip, slot.rectangle) ||
            ui_media_picker_scene_rail_media(scene, 1, &slot))
            return false;
        g_probe.failure_stage = 74;
        if (!ui_media_picker_scene_rail_media(scene, 0, &slot) || !present() || !drawn(slot))
            return false;
        // Scroll only the rail, until the icon is cut in half at its top edge.
        g_probe.failure_stage = 75;
        const float grid_scroll = scene.scroll_dips;
        const float cut = slot.rectangle.top - scene.rail.top + ui_rect_height(slot.rectangle) * .5f;
        UiMediaPickerRailMediaSlot clipped{};
        if (!ui_media_picker_scene_scroll(&scene, cut, true) || scene.scroll_dips != grid_scroll ||
            !ui_media_picker_scene_rail_media(scene, 0, &clipped) || clipped.clip.top != scene.rail.top ||
            clipped.rectangle.top >= scene.rail.top || clipped.clip.bottom != clipped.rectangle.bottom)
            return false;
        g_probe.failure_stage = 76;
        return present() && drawn(clipped);
    };
    const auto* previous = renderer.media_picker_scene;
    const bool passed = run();
    renderer.media_picker_scene = previous;
    renderer.retained_presentation.valid = false;
    ui_media_picker_scene_release(&scene);
    if (!passed || !rebuild_probe_content_text() || !probe_present(&renderer, kDefaultTheme))
        return false;
    log_write(LogLevel::info, "probe", "reaction_picker_server_icon_rail_media_and_clip_passed");
    return true;
}

} // namespace ui_probe_detail
} // namespace maicord
