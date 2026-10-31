#pragma once

#include "ui_surface_motion_measurement.hpp"
#include "../../src/render/d3d11_renderer_layout.hpp"
#include "maicord/app/message_view_runtime.hpp"
#include "maicord/render/content_text_retention.hpp"
#include "maicord/model/message_rich_store.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/ui/message_author.hpp"
#include "maicord/ui/message_interaction.hpp"
#include "maicord/ui/profile_media.hpp"

namespace maicord {
namespace message_header_clip_probe_detail {
enum class Line : u8 { server_tag, app_badge, interaction };

// The header line command of a message: the author line carrying a server tag or
// the APP badge, or the interaction line carrying the user's tag and the command chip.
inline const MessageTextCommand* find_line(const D3D11Renderer& renderer, const MessageScene& scene,
                                           const UserStore& users, u64 message_id, Line line) noexcept {
    const auto* text = renderer.message_text;
    for (u32 i = 0; text && i < text->command_count; ++i) {
        const auto& command = text->commands[i];
        if (command.message_id != message_id)
            continue;
        if (line == Line::interaction) {
            MessageInteractionLayout layout{};
            if (message_interaction_layout(scene, command, users, nullptr, kDefaultTheme, &layout) &&
                layout.tag.label.bytes && ui_rect_has_area(layout.chip))
                return &command;
            continue;
        }
        MessageAuthorLayout header{};
        if (message_author_layout(scene, command, *renderer.chat_state, users, nullptr, kDefaultTheme, &header) &&
            header.badge.label.bytes && header.badge.accent == (line == Line::app_badge))
            return &command;
    }
    return nullptr;
}

template <class Predicate>
inline u32 count_pixels(const surface_motion_probe_detail::Readback& rb, u32 image, UiRect area, float scale,
                        Predicate&& predicate) noexcept {
    const auto region = raster_clip_rectangle(area, scale, rb.width, rb.height);
    u32 count = 0;
    for (i32 y = maximum(0, region.top); y < region.bottom; ++y)
        for (i32 x = maximum(0, region.left); x < region.right; ++x)
            count += predicate(rb.images + image * rb.bytes + (y * rb.width + x) * 4) ? 1u : 0u;
    return count;
}

// Pixels of `area` that differ between two captured frames.
inline u32 differing(const surface_motion_probe_detail::Readback& rb, u32 first, u32 second, UiRect area,
                     float scale) noexcept {
    const auto region = raster_clip_rectangle(area, scale, rb.width, rb.height);
    u32 count = 0;
    for (i32 y = maximum(0, region.top); y < region.bottom; ++y)
        for (i32 x = maximum(0, region.left); x < region.right; ++x) {
            const u32 at = (y * rb.width + x) * 4;
            count += surface_motion_probe_detail::distance(rb.images + first * rb.bytes + at,
                                                           rb.images + second * rb.bytes + at) > 8
                         ? 1u
                         : 0u;
        }
    return count;
}

inline bool solid_magenta(const u8* pixel) noexcept {
    return pixel[0] > 170 && pixel[1] < 80 && pixel[2] > 170;
}
inline bool solid_green(const u8* pixel) noexcept {
    return pixel[0] < 60 && pixel[1] > 200 && pixel[2] < 60;
}
} // namespace message_header_clip_probe_detail

// A message whose header line is scrolled half under the channel header must
// draw nothing of that line above the message viewport: not its server tag
// image, the tag and APP chips (with the verified check), or the command chip
// of an interaction header. Each is compared above the viewport edge against
// the same frame without it, and the tag image's queued quad must clip there.
inline bool message_header_clip_ui_probe(D3D11Renderer& renderer, MessageViewRuntime& view, MessageRichStore& rich,
                                         MessageScene& scene, UserStore& users,
                                         bool (*rebuild_model)() noexcept) noexcept {
    using namespace surface_motion_probe_detail;
    using namespace message_header_clip_probe_detail;
    u32 stage = 1;
    struct Failure {
        u32& stage;
        ~Failure() noexcept {
            if (stage)
                log_u64(LogLevel::error, "probe", "message_header_clip_failure_stage", stage);
        }
    } failure{stage};
    if (!rebuild_model || renderer.message_scene != &scene || renderer.user_store != &users || !renderer.chat_state ||
        !renderer.media_atlas)
        return false;
    constexpr u64 base = 1'270'000'000'400'000'000ull, guild = 0x7a67, bot = base + 20'000, caller = base + 30'000;
    constexpr u32 rows = 24, command_index = 3, authors = 5;
    constexpr u64 plain_row = base + 1, command_row = base + command_index;
    const auto badge_hash = utf8_literal("0123456789abcdef0123456789abcdef");
    const auto tag = [&](u64 user, bool shown) noexcept {
        UserAppearance appearance{};
        appearance.user_id = user;
        appearance.fields = appearance_guild_tag;
        appearance.primary_enabled = shown;
        appearance.primary_guild_id = guild;
        copy_bytes(appearance.primary_tag.data, "TAG", 3);
        appearance.primary_tag.bytes = 3;
        copy_bytes(appearance.primary_badge.data, badge_hash.data, badge_hash.bytes);
        appearance.primary_badge.bytes = static_cast<u16>(badge_hash.bytes);
        return appearance;
    };
    // Every author carries a server tag, the bot of the command row the verified APP badge.
    const auto show_headers = [&](bool shown) noexcept {
        for (u32 i = 0; i < authors; ++i)
            if (!user_appearance_upsert(&users.appearance, tag(base + 10'000 + i, shown)))
                return false;
        UserAppearance app{};
        app.user_id = bot;
        app.fields = appearance_bot | appearance_flags;
        app.bot = shown;
        app.public_flags = shown ? 1ull << 16 : 0;
        return user_appearance_upsert(&users.appearance, tag(caller, shown)) &&
               user_appearance_upsert(&users.appearance, app);
    };
    for (u32 i = 0; i < rows; ++i) {
        MessageInput input{};
        input.id = base + i;
        input.channel_id = 111;
        input.author_id = i == command_index ? bot : base + 10'000 + i % authors;
        input.timestamp_ms = snowflake_timestamp_ms(input.id);
        input.content = utf8_literal("Header clipping row.");
        input.type = i == command_index ? 20 : 0;
        if (!message_store_upsert(&view.store, input).ok() ||
            !user_store_upsert(&users, input.author_id, utf8_literal("Native Author")).ok())
            return false;
    }
    if (!user_store_upsert(&users, caller, utf8_literal("Mai")).ok())
        return false;
    const auto found = message_store_find(view.store, command_row);
    const auto* record = found.ok() ? message_store_resolve(view.store, found.handle) : nullptr;
    GatewayMessageEvent event{};
    event.id = command_row;
    event.fields = gateway_message_field_interaction;
    event.interaction = {command_row + 1000, caller, utf8_literal("Mai"), {}, utf8_literal("release"), 2};
    if (!record || !message_rich_store_apply(&rich, command_row, record->revision, event).ok())
        return false;
    // The server tag image: solid magenta, premultiplied.
    ProfileMedia tag_media{};
    u8 square[32 * 32 * 4];
    for (u32 i = 0; i < 32 * 32; ++i) {
        square[i * 4] = 255;
        square[i * 4 + 1] = 0;
        square[i * 4 + 2] = 255;
        square[i * 4 + 3] = 255;
    }
    stage = 2;
    if (!user_guild_badge_media(tag(caller, true), {}, {}, &tag_media) ||
        !renderer_upload_media_rgba8(&renderer, tag_media.key, 32, 32, 32 * 4, {square, sizeof(square)}).ok() ||
        !show_headers(true) ||
        !message_view_runtime_reindex(&view, ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height), 1, true)
             .ok() ||
        !rebuild_model() || !rebuild_model())
        return false;
    Readback readback{};
    if (!readback.initialize(renderer))
        return false;
    const float scale = static_cast<float>(renderer.dpi) / 96.f;
    // Draws and reads back the current layout under `theme` as image `index`.
    const auto frame = [&](const Theme& theme, u32 index) noexcept {
        content_text_retention_invalidate(&renderer.content_text_retention);
        renderer.retained_presentation.valid = false;
        const float clear[]{theme.base_lowest.red, theme.base_lowest.green, theme.base_lowest.blue, 1.f};
        if (!renderer_rebuild_content_text(&renderer, theme) ||
            !d3d11_renderer_paint_layout(&renderer, theme, renderer_capture_presentation_stamp(renderer, theme)))
            return false;
        renderer.graphics.context->ClearRenderTargetView(renderer.graphics.target, clear);
        return d3d11_renderer_draw_stream(&renderer, theme, false, false, nullptr, 10000) &&
               readback.read(renderer, index);
    };
    // The command chip and its icon in solid green.
    Theme witness = kDefaultTheme;
    witness.tokens.mention.inline_background = Color{0.f, 1.f, 0.f, 1.f};
    witness.tokens.mention.text = Color{0.f, 1.f, 0.f, 1.f};
    struct Target {
        u64 id;
        Line line;
        bool tag_image, command_chip;
    };
    constexpr Target targets[]{{plain_row, Line::server_tag, true, false},
                               {command_row, Line::interaction, true, true},
                               {command_row, Line::app_badge, false, false}};
    for (u32 t = 0; t < 3; ++t) {
        const Target& target = targets[t];
        stage = 10 + t * 10;
        // From the top, scroll the line's middle onto the viewport's top edge.
        if (!message_timeline_set_scroll(&view.timeline, 0, false) || !rebuild_model())
            return false;
        const auto* line = find_line(renderer, scene, users, target.id, target.line);
        const float middle = line ? (line->rectangle.top + line->rectangle.bottom) * .5f : 0.f;
        if (!line || !(middle > scene.viewport.top) ||
            !message_timeline_set_scroll(&view.timeline, ui_extent_units(middle - scene.viewport.top), false) ||
            !rebuild_model() || !rebuild_model())
            return false;
        line = find_line(renderer, scene, users, target.id, target.line);
        stage = 11 + t * 10;
        if (!line || !(line->rectangle.top + 3.f < scene.viewport.top) ||
            !(line->rectangle.bottom - 3.f > scene.viewport.top))
            return false;
        const UiRect strip{scene.viewport.left, line->rectangle.top - 2.f, scene.viewport.right,
                           scene.viewport.top - 1.f};
        const UiRect lower{scene.viewport.left, scene.viewport.top, scene.viewport.right, line->rectangle.bottom};
        stage = 12 + t * 10;
        if (!frame(kDefaultTheme, 0))
            return false;
        // The tag images of the visible rows are on screen, and none reaches above the viewport.
        const u32 images = count_pixels(readback, 0, scene.viewport, scale, solid_magenta);
        const u32 leaked = count_pixels(readback, 0, strip, scale, solid_magenta);
        log_u64(LogLevel::info, "probe", "message_header_clip_tag_image_pixels", images);
        log_u64(LogLevel::info, "probe", "message_header_clip_tag_image_pixels_above", leaked);
        if (images < 8 || leaked)
            return false;
        // The queued tag quads: every visible part inside the viewport; the straddling one is cut by it.
        stage = 13 + t * 10;
        const auto allocation = media_atlas_peek(renderer.media_atlas, tag_media.key);
        u32 quads = 0, cut = 0;
        for (u32 b = 0; allocation.ok() && b < renderer.media_batch_count; ++b) {
            const auto& batch = renderer.media_draw.batch_data[b];
            for (u32 q = batch.first_quad; q < batch.first_quad + batch.quad_count; ++q) {
                const auto handle = renderer.media_draw.handle_data[q];
                if (renderer.media_draw.domain_data[q] || handle.slot != allocation.handle.slot ||
                    handle.generation != allocation.handle.generation)
                    continue;
                const auto& quad = renderer.media_draw.quad_data[q];
                const UiRect drawn = ui_rect_intersection({quad.left, quad.top, quad.right, quad.bottom}, batch.clip);
                if (!ui_rect_has_area(drawn))
                    continue;
                ++quads;
                if (drawn.top < scene.viewport.top - .01f)
                    return false;
                cut += quad.top < scene.viewport.top - .01f ? 1u : 0u;
            }
        }
        log_u64(LogLevel::info, "probe", "message_header_clip_tag_quads", quads);
        if (!quads || (target.tag_image && !cut))
            return false;
        // The same frame without any tag or APP badge: nothing above the viewport changes.
        stage = 14 + t * 10;
        if (!show_headers(false) || !rebuild_model() || !rebuild_model() || !frame(kDefaultTheme, 1))
            return false;
        const u32 changed = differing(readback, 0, 1, strip, scale);
        log_u64(LogLevel::info, "probe", "message_header_clip_chip_pixels_changed_above", changed);
        if (changed || !show_headers(true) || !rebuild_model() || !rebuild_model())
            return false;
        if (!target.command_chip)
            continue;
        // The command chip, painted green, shows below the edge and never above it.
        stage = 15 + t * 10;
        if (!frame(witness, 2))
            return false;
        const u32 chip = count_pixels(readback, 2, lower, scale, solid_green);
        const u32 chip_above = count_pixels(readback, 2, strip, scale, solid_green);
        log_u64(LogLevel::info, "probe", "message_header_clip_command_chip_pixels", chip);
        log_u64(LogLevel::info, "probe", "message_header_clip_command_chip_pixels_above", chip_above);
        if (chip < 8 || chip_above)
            return false;
    }
    stage = 0;
    log_write(LogLevel::info, "probe", "message_header_clip_tag_chip_app_and_command_above_viewport_passed");
    return true;
}
} // namespace maicord
