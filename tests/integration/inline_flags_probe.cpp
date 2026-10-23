#include "probe_capture.hpp"
#include "inline_flags_probe.hpp"
#include "probe_present.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"
#include "maicord/core/log.hpp"
#include "maicord/render/inline_media.hpp"
#include "maicord/render/layer_presentation.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/regional_flag.hpp"

#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {
constexpr Utf8View channel_name = utf8_literal("🇸🇮-𝓢𝓵𝓸-𝓒𝓱𝓪𝓽");
constexpr Utf8View popup_label = utf8_literal("Overlay  🇸🇮  label");
constexpr u16 slovenia = 477;
bool failed(u32 stage) noexcept {
    log_u64(LogLevel::error, "probe", "inline_flags_failure_stage", stage);
    return false;
}
bool close_float(float a, float b) noexcept {
    return a - b < .3f && b - a < .3f;
}
bool media_quad(const D3D11Renderer& r, const RendererInlineFlag& flag) noexcept {
    const auto allocation = media_atlas_peek(r.media_atlas, media_asset_regional_flag(flag.flag));
    if (!allocation.ok())
        return false;
    for (u32 i = 0; i < r.media_quad_count; ++i) {
        const auto handle = r.media_draw.handle_data[i];
        const auto& q = r.media_draw.quad_data[i];
        if (r.media_draw.domain_data[i] || handle.slot != allocation.handle.slot ||
            handle.generation != allocation.handle.generation ||
            !close_float(q.left, flag.rectangle.left) || !close_float(q.top, flag.rectangle.top) ||
            !close_float(q.right, flag.rectangle.right) || !close_float(q.bottom, flag.rectangle.bottom))
            continue;
        for (u32 j = 0; j < r.media_batch_count; ++j) {
            const auto& b = r.media_draw.batch_data[j];
            if (i >= b.first_quad && i < b.first_quad + b.quad_count && ui_rect_equal(b.clip, flag.clip))
                return true;
        }
    }
    return false;
}
bool suppressed(const D3D11Renderer& r, const RendererInlineFlag& flag) noexcept {
    // The reserved em cell must contain no shaped SI letter ink. Neighboring
    // text remains in the retained stream; exclude boundary overhangs here.
    const float inset = ui_rect_width(flag.rectangle) * .2f;
    const UiRect inner{flag.rectangle.left + inset, flag.rectangle.top + inset,
                       flag.rectangle.right - inset, flag.rectangle.bottom - inset};
    const u32 first = flag.layer == RendererInlineMediaLayer::popup ? r.popup_text_first_batch : 0;
    const u32 end = flag.layer == RendererInlineMediaLayer::popup ? r.popup_text_end_batch
                                                                  : r.content_text->batch_count;
    if (first > end || end > r.content_text->batch_count)
        return false;
    // A popup can cover unrelated header glyphs at the same screen position.
    // SI suppression concerns its own retained text layer, not hidden ink in
    // earlier layers (native pixel checks separately establish occlusion).
    for (u32 b = first; b < end; ++b) {
        const auto& batch = r.content_text->batches[b];
        if (batch.first_quad > r.content_text->quad_count ||
            batch.quad_count > r.content_text->quad_count - batch.first_quad)
            return false;
        for (u32 i = batch.first_quad; i < batch.first_quad + batch.quad_count; ++i) {
            const auto& q = r.content_text->quads[i];
            if (ui_rect_has_area(ui_rect_intersection(inner, {q.left, q.top, q.right, q.bottom})))
                return false;
        }
    }
    return true;
}
// Read the actual drawn swap-chain target. This generates no file or image.
bool pixel_is_fixture(D3D11Renderer& r, UiRect rectangle, bool expected) noexcept {
    // Present advances a flip-model buffer; its contents are not the drawn
    // target anymore. Use the same before-flip draw/readback path as UiProbe's
    // native text/pixel assertions, without writing a screenshot.
    if (!d3d11_renderer_draw_stream(&r, kDefaultTheme, true, true, nullptr))
        return false;
    if (!r.graphics.target || !ui_rect_has_area(rectangle))
        return false;
    ProbeTargetReadback readback;
    D3D11_TEXTURE2D_DESC description{};
    if (readback.read_target(r, description) != 0)
        return false;
    const bool bgra = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    if (!bgra && description.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
        return false;
    const auto& mapped = readback.mapped;
    const float scale = r.dpi / 96.f;
    const i32 x = static_cast<i32>((rectangle.left + rectangle.right) * .5f * scale);
    const i32 y = static_cast<i32>((rectangle.top + rectangle.bottom) * .5f * scale);
    bool valid = x >= 0 && y >= 0 && static_cast<u32>(x) < description.Width &&
                 static_cast<u32>(y) < description.Height;
    if (valid) {
        const auto* pixel =
            static_cast<const u8*>(mapped.pData) + static_cast<usize>(y) * mapped.RowPitch + x * 4;
        const u8 red = pixel[bgra ? 2 : 0], green = pixel[1], blue = pixel[bgra ? 0 : 2];
        const bool fixture =
            red >= 18 && red <= 28 && green >= 236 && green <= 246 && blue >= 67 && blue <= 77;
        valid = fixture == expected;
        if (!valid) {
            log_u64(LogLevel::error, "probe", "inline_flags_actual_pixel_red", red);
            log_u64(LogLevel::error, "probe", "inline_flags_actual_pixel_green", green);
            log_u64(LogLevel::error, "probe", "inline_flags_actual_pixel_blue", blue);
            log_u64(LogLevel::error, "probe", "inline_flags_expected_fixture_pixel", expected);
        }
    }
    return valid;
}
bool find_flag(const D3D11Renderer& r, UiWidgetManager* manager, RendererInlineFlag* output) noexcept {
    for (u32 i = 0; i < minimum(r.inline_media.count, kRendererInlineFlagCapacity); ++i) {
        const auto& flag = r.inline_media.flags[i];
        if (flag.flag == slovenia && flag.owner.manager == manager &&
            renderer_inline_flag_current(r, flag)) {
            *output = flag;
            return true;
        }
    }
    return false;
}
} // namespace

bool inline_flags_ui_probe(InlineFlagsProbeContext c) noexcept {
    auto& r = c.renderer;
    const auto* channel = chat_state_store_find_channel(c.state, c.state.selected_channel_id);
    if (!c.rebuild || !r.content_text || !r.shell_navigation_scene || !channel || channel->type != 0 ||
        r.inline_media.flags || r.inline_media.count ||
        regional_flag_sequence(utf8_literal("🇸🇮")) != slovenia)
        return failed(1);
    const auto previous_channel = *channel;
    const auto original = chat_state_store_channel_name(c.state, *channel);
    char original_bytes[1024]{};
    if (original.bytes > sizeof(original_bytes))
        return failed(2);
    copy_bytes(original_bytes, original.data, original.bytes);
    const auto* previous_popup = r.popup_scene;
    const u32 original_width = r.width, original_height = r.height;
    PopupScene popup{};
    PopupItem item_storage[1]{};
    PopupRowWidget row_storage[1]{};
    UiWidget widgets[8]{};
    UiLayoutNode nodes[8]{};
    struct Restore {
        InlineFlagsProbeContext& c;
        const ChannelRecord& channel;
        Utf8View name;
        const PopupScene* previous_popup;
        PopupScene& popup;
        u32 width, height;
        ~Restore() noexcept {
            popup_scene_close(&popup);
            c.renderer.popup_scene = previous_popup;
            renderer_set_layer_opacity(&c.renderer, RendererLayerSurface::popup, 1.f);
            popup_scene_release(&popup);
            renderer_resize(&c.renderer, width, height);
            chat_state_store_upsert_channel(&c.state,
                                               {channel.id, channel.guild_id, channel.parent_id, name,
                                                channel.position, channel.flags, channel.type});
            navigation_view_rebuild(&c.navigation, c.state);
            c.rebuild();
        }
    } restore{c,
              previous_channel,
              {original_bytes, original.bytes},
              previous_popup,
              popup,
              original_width,
              original_height};
    if (!chat_state_store_upsert_channel(&c.state, {channel->id, channel->guild_id, channel->parent_id,
                                                       channel_name, channel->position, channel->flags,
                                                       channel->type})
             .ok() ||
        !navigation_view_rebuild(&c.navigation, c.state).ok() || !c.rebuild())
        return failed(3);
    RendererInlineFlag header{}, navigation{};
    if (!find_flag(r, &c.shell.widgets, &header) ||
        !find_flag(r, const_cast<UiWidgetManager*>(&r.shell_navigation_scene->widgets), &navigation) ||
        header.layer != RendererInlineMediaLayer::base ||
        navigation.layer != RendererInlineMediaLayer::base || !suppressed(r, header) ||
        !suppressed(r, navigation) ||
        !close_float(ui_rect_width(header.rectangle), ui_rect_height(header.rectangle)) ||
        !close_float(ui_rect_width(navigation.rectangle), ui_rect_height(navigation.rectangle)) ||
        ui_rect_width(header.rectangle) < 8 || ui_rect_width(header.rectangle) > 32 ||
        ui_rect_width(navigation.rectangle) < 8 || ui_rect_width(navigation.rectangle) > 32)
        return failed(4);
    MEMORY_BASIC_INFORMATION bank{};
    if (!VirtualQuery(r.inline_media.flags, &bank, sizeof(bank)) || bank.State != MEM_COMMIT ||
        bank.RegionSize < kRendererInlineFlagCapacity * sizeof(RendererInlineFlag))
        return failed(5);
    log_u64(LogLevel::info, "probe", "inline_flags_lazy_committed_bytes", bank.RegionSize);
    log_u64(LogLevel::info, "probe", "inline_flags_record_capacity", kRendererInlineFlagCapacity);
    log_u64(LogLevel::info, "probe", "inline_flags_actual_em_millidip",
            static_cast<u64>(ui_rect_width(header.rectangle) * 1000));
    u8 artwork[32 * 32 * 4]{};
    for (u32 i = 0; i < 32 * 32; ++i) {
        artwork[i * 4] = 23;
        artwork[i * 4 + 1] = 241;
        artwork[i * 4 + 2] = 72;
        artwork[i * 4 + 3] = 255;
    }
    const auto uploaded = renderer_upload_media_straight_rgba8(&r, media_asset_regional_flag(slovenia), 32,
                                                               32, 128, {artwork, sizeof(artwork)});
    if (!uploaded.ok()) {
        log_u64(LogLevel::error, "probe", "inline_flags_upload_error", static_cast<u64>(uploaded.error));
        log_u64(LogLevel::error, "probe", "inline_flags_allocation_error",
                static_cast<u64>(uploaded.allocation.error));
        return failed(61);
    }
    if (!c.rebuild())
        return failed(62);
    if (!probe_present(&r, kDefaultTheme))
        return failed(63);
    if (!media_quad(r, header))
        return failed(64);
    if (!media_quad(r, navigation))
        return failed(65);
    if (!pixel_is_fixture(r, header.rectangle, true))
        return failed(66);
    if (!pixel_is_fixture(r, navigation.rectangle, true))
        return failed(67);
    auto* bank_pointer = r.inline_media.flags;
    const u32 retained_count = r.inline_media.count, text_builds = r.content_text->rebuilds;
    if (!c.rebuild() || !probe_present(&r, kDefaultTheme) || r.inline_media.flags != bank_pointer ||
        r.inline_media.count != retained_count || r.content_text->rebuilds != text_builds)
        return failed(7);
    // Exercise the production resize/layout/shape path with the exact name
    // that rejected Main's full frame. Every step reads refreshed cells; old
    // placements are never treated as authority after geometry changes.
    if (original_width < 2 || original_height < 2)
        return failed(70);
    for (u32 step = 0; step < 6; ++step) {
        const u32 width = (step & 1) ? original_width - 1 : original_width + 1;
        const u32 height = (step & 1) ? original_height - 1 : original_height + 1;
        if (!renderer_resize(&r, width, height) || !c.rebuild() ||
            !find_flag(r, &c.shell.widgets, &header) ||
            !find_flag(r, const_cast<UiWidgetManager*>(&r.shell_navigation_scene->widgets), &navigation) ||
            !suppressed(r, header) || !suppressed(r, navigation) || r.inline_media.flags != bank_pointer ||
            r.inline_media.count > kRendererInlineFlagCapacity || !probe_present(&r, kDefaultTheme) ||
            !media_quad(r, header) || !media_quad(r, navigation) ||
            !pixel_is_fixture(r, header.rectangle, true) ||
            !pixel_is_fixture(r, navigation.rectangle, true)) {
            log_u64(LogLevel::error, "probe", "inline_flags_resize_step", step);
            return failed(71);
        }
    }
    if (!renderer_resize(&r, original_width, original_height) || !c.rebuild() ||
        !find_flag(r, &c.shell.widgets, &header) ||
        !find_flag(r, const_cast<UiWidgetManager*>(&r.shell_navigation_scene->widgets), &navigation) ||
        !probe_present(&r, kDefaultTheme) || !pixel_is_fixture(r, header.rectangle, true) ||
        !pixel_is_fixture(r, navigation.rectangle, true))
        return failed(72);
    log_u64(LogLevel::info, "probe", "inline_flags_actual_small_resize_steps", 6);
    if (!popup_scene_initialize(&popup, {item_storage, 1}, {row_storage, 1}, {widgets, 8}, {nodes, 8}) ||
        !renderer_bind_popup_scene(&r, &popup))
        return failed(8);
    const PopupItem item{popup_label, 1, popup_item_enabled};
    const UiRect anchor{header.rectangle.left - 12, header.rectangle.top - 8, header.rectangle.left - 12,
                        header.rectangle.top - 8};
    const UiRect viewport{0, 0, r.width * 96.f / r.dpi, r.height * 96.f / r.dpi};
    if (!popup_scene_open(&popup, PopupKind::context_menu, 1, {&item, 1}, anchor, viewport, 240).ok() ||
        !c.rebuild() || !probe_present(&r, kDefaultTheme))
        return failed(9);
    RendererInlineFlag popup_flag{};
    if (!find_flag(r, &popup.widgets, &popup_flag))
        return failed(101);
    if (popup_flag.layer != RendererInlineMediaLayer::popup)
        return failed(102);
    if (!media_quad(r, popup_flag))
        return failed(103);
    if (!suppressed(r, popup_flag))
        return failed(104);
    if (!pixel_is_fixture(r, popup_flag.rectangle, true))
        return failed(105);
    if (!pixel_is_fixture(r, header.rectangle, false))
        return failed(106);
    const u32 popup_text_builds = r.content_text->rebuilds;
    if (!renderer_set_layer_opacity(&r, RendererLayerSurface::popup, 0.f) ||
        !probe_present(&r, kDefaultTheme) || r.content_text->rebuilds != popup_text_builds ||
        !pixel_is_fixture(r, popup_flag.rectangle, false) || !pixel_is_fixture(r, header.rectangle, true))
        return failed(11);
    renderer_set_layer_opacity(&r, RendererLayerSurface::popup, 1.f);
    popup_scene_close(&popup);
    if (renderer_inline_flag_current(r, popup_flag) || !c.rebuild() || !probe_present(&r, kDefaultTheme) ||
        !pixel_is_fixture(r, header.rectangle, true))
        return failed(12);
    log_write(LogLevel::info, "probe",
              "inline_flags_header_channel_pixels_em_clip_si_suppression_retention_occlusion_opacity_"
              "lifetime_passed");
    return true;
}
} // namespace maicord
