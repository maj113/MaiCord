#pragma once

#include "ui_surface_motion_measurement.hpp"
#include "../../src/render/d3d11/renderer_layout.hpp"
#include "maicord/app/chat/messages/view_runtime.hpp"
#include "maicord/render/text/content_text_retention.hpp"
#include "maicord/model/chat/message_rich_store.hpp"
#include "maicord/ui/chat/messages/text_presentation.hpp"
#include "maicord/ui/theme/background_presentation.hpp"

namespace maicord {
namespace unplayable_video_probe_detail {
inline constexpr u64 kMessage = 1'271'000'000'800'000'000ull;

inline const MessageTextCommand* label_command(const D3D11Renderer& renderer, Utf8View label, u32 nth) noexcept {
    const auto* text = renderer.message_text;
    for (u32 i = 0; text && i < text->command_count; ++i)
        if (text->commands[i].message_id == kMessage && utf8_equal(text->commands[i].text, label) && !nth--)
            return &text->commands[i];
    return nullptr;
}
} // namespace unplayable_video_probe_detail

// A message with an MP4 attachment and a Tenor GIFV embed that this computer has no decoder for, through the real
// renderer: the boxes keep their size and fill, the attachment loses its play control, and each box shows its label
// centred in the muted text colour. Writes unplayable-video-before.bmp (no marks: the empty boxes of a failed decode),
// unplayable-video-after.bmp (the Windows label) and unplayable-video-linux-label.bmp (the same frame with the Linux
// build's H.264 label in the attachment's command) when MAICORD_UI_PROBE_CAPTURE_DIR names a directory.
inline bool unplayable_video_ui_probe(D3D11Renderer& renderer, MessageViewRuntime& view, MessageRichStore& rich,
                                      MessageScene& scene, UserStore& users, bool (*rebuild_model)() noexcept,
                                      bool (*capture)(const wchar_t*) noexcept) noexcept {
    using namespace surface_motion_probe_detail;
    using namespace unplayable_video_probe_detail;
    u32 stage = 1;
    struct Failure {
        u32& stage;
        ~Failure() noexcept {
            if (stage)
                log_u64(LogLevel::error, "probe", "unplayable_video_failure_stage", stage);
        }
    } failure{stage};
    if (!rebuild_model || !capture || renderer.message_scene != &scene || !renderer.content_text)
        return false;
    MessageInput input{};
    input.id = kMessage;
    input.channel_id = 111;
    input.author_id = kMessage + 1;
    input.timestamp_ms = snowflake_timestamp_ms(input.id);
    input.content = utf8_literal("A clip and a GIF from a computer without a video decoder:");
    GatewayMessageAttachment clip{};
    clip.id = kMessage + 2;
    clip.filename = utf8_literal("clip.mp4");
    clip.content_type = utf8_literal("video/mp4");
    clip.url = utf8_literal("https://cdn.discordapp.com/attachments/111/1271000000800000002/clip.mp4");
    clip.width = 320;
    clip.height = 180;
    clip.has_width = clip.has_height = true;
    GatewayMessageEmbed gif{};
    gif.type = GatewayMessageEmbedType::gifv;
    gif.url = utf8_literal("https://tenor.com/view/cat-123");
    gif.provider_name = utf8_literal("Tenor");
    gif.provider_url = utf8_literal("https://tenor.co");
    gif.thumbnail.url = utf8_literal("https://media.tenor.com/cat.png");
    gif.video.url = utf8_literal("https://media.tenor.com/cat.mp4");
    gif.video.proxy_url = utf8_literal("https://images-ext-1.discordapp.net/external/b/https/media.tenor.com/cat.mp4");
    gif.thumbnail.width = gif.video.width = 300;
    gif.thumbnail.height = gif.video.height = 220;
    gif.thumbnail.has_width = gif.thumbnail.has_height = gif.video.has_width = gif.video.has_height = true;
    GatewayMessageEvent event{};
    event.id = kMessage;
    event.fields = gateway_message_field_attachments | gateway_message_field_embeds;
    event.attachments = {&clip, 1};
    event.embeds = {&gif, 1};
    if (!message_timeline_reset(&view.timeline) || !message_store_upsert(&view.store, input).ok() ||
        !user_store_upsert(&users, input.author_id, utf8_literal("Native Author")).ok())
        return false;
    const auto found = message_store_find(view.store, kMessage);
    const MessageRecord* record = found.ok() ? message_store_resolve(view.store, found.handle) : nullptr;
    if (!record || !message_rich_store_apply(&rich, kMessage, record->revision, event).ok() ||
        !message_view_runtime_reindex(&view, ui_extent_units(kUiBootstrapMessageRowHeight), 1, true).ok() ||
        !message_view_runtime_bind_rich_store(&view, &rich).ok() ||
        !message_timeline_set_scroll(&view.timeline, 0, false) || !rebuild_model() || !rebuild_model())
        return false;
    Readback readback{};
    if (!readback.initialize(renderer))
        return false;
    const float scale = static_cast<float>(renderer.dpi) / 96.f;
    const auto frame = [&](u32 index) noexcept {
        content_text_retention_invalidate(&renderer.content_text_retention);
        renderer.retained_presentation.valid = false;
        const float clear[]{kDefaultTheme.base_lowest.red, kDefaultTheme.base_lowest.green,
                            kDefaultTheme.base_lowest.blue, 1.f};
        if (!renderer_rebuild_content_text(&renderer, kDefaultTheme) ||
            !d3d11_renderer_paint_layout(&renderer, kDefaultTheme,
                                         renderer_capture_presentation_stamp(renderer, kDefaultTheme)))
            return false;
        renderer.graphics.context->ClearRenderTargetView(renderer.graphics.surface.target, clear);
        return d3d11_renderer_draw_stream(&renderer, kDefaultTheme, false, false, nullptr, 10000) &&
               readback.read(renderer, index);
    };
    const auto row_rectangle = [&]() noexcept {
        const auto* row = message_scene_find_row(&scene, found.order_index);
        const auto* node = row ? message_scene_layout_node(scene, row->widget) : nullptr;
        return node ? node->rectangle : UiRect{};
    };
    const Utf8View no_decoder = message_media_unplayable_label(MessageMediaUnplayable::hevc);
    stage = 2;
    if (!frame(0) || label_command(renderer, no_decoder, 0) || !capture(L"unplayable-video-before.bmp"))
        return false;
    const UiRect row_before = row_rectangle();
    stage = 3;
    if (!message_scene_mark_unplayable(&scene, message_media_reveal_key(kMessage, MessageMediaSource::attachment, 0),
                                       MessageMediaUnplayable::h264) ||
        !message_scene_mark_unplayable(&scene, message_media_reveal_key(kMessage, MessageMediaSource::embed, 0),
                                       MessageMediaUnplayable::hevc) ||
        !rebuild_model() || !rebuild_model() || !frame(1) || !capture(L"unplayable-video-after.bmp"))
        return false;
    // The Windows build shows the no-decoder label on both boxes: the attachment's comes first.
    const MessageTextCommand* labels[2]{label_command(renderer, no_decoder, 0),
                                        label_command(renderer, no_decoder, 1)};
    const Color muted = ui_background_muted_text_color(kDefaultTheme);
    stage = 4;
    if (!ui_rect_equal(row_rectangle(), row_before) || !ui_rect_has_area(row_before))
        return log_write(LogLevel::error, "probe", "unplayable_video_row_moved"), false;
    u32 label_pixels[2]{};
    for (u32 i = 0; i < 2; ++i) {
        const auto* label = labels[i];
        if (!label || label->color.red != muted.red || label->color.green != muted.green ||
            label->color.blue != muted.blue || label->horizontal_alignment != UiTextAlignment::center)
            return log_u64(LogLevel::error, "probe", "unplayable_video_label_missing", i), false;
        const auto region = raster_clip_rectangle(label->rectangle, scale, readback.width, readback.height);
        for (i32 y = maximum(0, region.top); y < region.bottom; ++y)
            for (i32 x = maximum(0, region.left); x < region.right; ++x) {
                const u32 at = (y * readback.width + x) * 4;
                label_pixels[i] += distance(readback.images + at, readback.images + readback.bytes + at) > 8 ? 1u : 0u;
            }
        log_u64(LogLevel::info, "probe", "unplayable_video_label_pixels", label_pixels[i]);
    }
    if (label_pixels[0] < 16 || label_pixels[1] < 16)
        return false;
    // The Linux build's H.264 label in the same box: what a computer without libopenh264 shows.
    stage = 5;
    auto* attachment_label = const_cast<MessageTextCommand*>(labels[0]);
    attachment_label->text = utf8_literal("Install OpenH264 to play this video");
    if (!frame(2) || !capture(L"unplayable-video-linux-label.bmp"))
        return false;
    scene.media_unplayable = {};
    stage = 0;
    log_write(LogLevel::info, "probe", "unplayable_video_label_box_and_pixels_passed");
    return true;
}
} // namespace maicord
