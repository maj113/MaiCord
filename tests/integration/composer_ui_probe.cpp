#include "application_emoji_preferences.hpp"
#include "composer_ui_probe.hpp"
#include "probe_present.hpp"
#include "../../tools/perf/probe_window.hpp"
#include "probe_glyph_ink.hpp"
#include "application_state.hpp"
#include "application_composer.hpp"
#include "application_composer_input.hpp"
#include "application_composer_edit_menu.hpp"
#include "application_reply_draft.hpp"
#include "application_attachments.hpp"
#include "application_commands.hpp"
#include "application_media_picker.hpp"
#include "application_thread_actions.hpp"
#include "application_profile.hpp"
#include "application_composer_text_input.hpp"
#include "application_plugins.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/ui/background_color_math.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/message_author.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"
#include "../../src/render/d3d11_renderer_layout.hpp"
#include "maicord/render/renderer_presentation.hpp"
#include "../application_action_captcha_stubs.hpp"

#include <d3d11_1.h>

namespace maicord::app_detail {
constinit Application g_app{};
}
namespace maicord {
namespace composer_probe_detail {
using namespace app_detail;
constexpr u64 kGuild = 10, kSelf = 99, kSource = 21, kSourceMessage = 909, kTargetMessage = 900;
constexpr u32 kRows = 4;
struct Fixture {
    Theme theme;
    RoleRecord roles[2];
    PermissionOverwriteRecord overwrites[8];
    GuildRailRowWidget rail_rows[2];
    UiWidget rail_widgets[4];
    UiLayoutNode rail_nodes[4];
    ShellNavigationRowWidget navigation_rows[8];
    UiWidget navigation_widgets[34];
    UiLayoutNode navigation_nodes[34];
    UiWidget message_widgets[kRows * kMessageSceneWidgetsPerRow + 1];
    UiLayoutNode message_nodes[kRows * kMessageSceneWidgetsPerRow + 1];
    UiWidget composer_widgets[kMessageComposerWidgetCapacity];
    UiLayoutNode composer_nodes[kMessageComposerWidgetCapacity];
    char composer_text[1024];
    TextHitSpan hit_spans[kTextHitSpanMaximum];
    TextHitRegion hit_scratch[kRendererContentHitRegionCapacity];
    TextDecoration decorations[kRendererTextDecorationCapacity];
    u16 channel_scratch[8];
    u8 visibility[8];
    ChannelSortKey channel_keys[8];
    u32 markdown[kMessageTextByteStyleScratchCapacity];
    TypingRecord typing_records[4];
    Utf8View typing, attachments;
    u32 picks, manages, gifs, stickers, emojis, history_calls, frame_requests;
    u32 attachment_queue_notices, unexpected_completions;
    u32 timestamp_pixel_proofs, clipped_timestamp_headers;
    bool ready, context_rows, refresh_ok, token_check;
    float expected_width;
};
Fixture* fixture;
bool require(bool value, u32 stage) noexcept {
    if (!value)
        log_u64(LogLevel::error, "probe", "composer_native_failure_stage", stage);
    return value;
}

bool project() noexcept {
    if (!fixture || !fixture->ready)
        return true;
    auto& r = g_app.renderer;
    auto& ui = *g_app.account_ui;
    if (!require(navigation_view_rebuild(&ui.navigation_view, ui.chat_state).ok(), 230))
        return false;
    const auto reply = application_reply_draft();
    char reply_storage[kMessageComposerReplyLabelBytes]{};
    const auto reply_label = application_reply_draft_label({reply_storage, sizeof(reply_storage)});
    auto metrics = kGuildChannelShellLayoutMetrics;
    metrics.flags = static_cast<u16>(metrics.flags & ~ui_shell_layout_members);
    metrics.member_sidebar_width = 0;
    // Compact available-width fixture topology only; shipping minimum632 stays
    // enforced by the application window. Default632 retains both rail/sidebar.
    if (r.width * 96.f / r.dpi < 632.f)
        metrics.flags = static_cast<u16>(metrics.flags & ~ui_shell_layout_navigation);
    const float full_width = ui_scene_composer_width(metrics, r.width * 96.f / r.dpi);
    metrics.composer_height = message_composer_scene_cached_height(
        g_app.message_composer_scene, reply.message_id != 0, fixture->typing.bytes != 0,
        fixture->attachments.bytes != 0, full_width);
    const MessageComposerProjectionContext projection{&ui.user_store, &ui.chat_state, kGuild};
    bool settled = false;
    for (u32 pass = 0; pass < 3; ++pass) {
        metrics.revision = 8100u + metrics.flags + ui_extent_units(metrics.composer_height) * 31u;
        if (!require(ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::guild_channel, metrics,
                                                  r.width * 96.f / r.dpi, r.height * 96.f / r.dpi),
                     231) ||
            !guild_rail_scene_update(
                 &g_app.guild_rail_scene, &ui.navigation_view,
                 ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::guild_navigation), 0)
                 .ok() ||
            !shell_navigation_scene_update(
                 &g_app.shell_navigation_scene, &ui.navigation_view,
                 ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::channel_navigation), 0)
                 .ok() ||
            !message_composer_scene_update(
                 &g_app.message_composer_scene, &g_app.message_composer,
                 ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::composer), true,
                 reply.message_id, reply_label, &projection, fixture->typing,
                 {fixture->attachments, true, true}, false, reply.mention_reply_author, true)
                 .ok())
            return false;
        float measured{};
        if (!require(renderer_measure_message_composer(&r, &measured), 232))
            return false;
        const float width = ui_rect_width(g_app.message_composer_scene.viewport);
        g_app.message_composer_scene.measured_row_height =
            measured -
            message_composer_scene_chrome_height(reply.message_id != 0, fixture->typing.bytes != 0,
                                                 fixture->attachments.bytes != 0) -
            message_composer_scene_toolbar_height(width);
        if (measured == metrics.composer_height) {
            settled = true;
            break;
        }
        metrics.composer_height = measured;
    }
    if (!require(settled, 233) ||
        !message_scene_update(&g_app.message_scene,
                              ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::message_view))
             .ok())
        return false;
    renderer_bind_navigation_view(&r, &ui.chat_state, &ui.user_store, &ui.navigation_view);
    r.message_measure_cache = &ui.text_measure_cache;
    renderer_bind_focus_owner(&r, g_app.context_focus);
    return message_text_coordinator_converge(
               {&g_app.message_scene, &ui.message_text, &ui.text_measure_cache, &ui.chat_state,
                &ui.user_store, &ui.navigation_view, &fixture->theme,
                +[](void* owner, const Theme& theme) noexcept {
                    return renderer_rebuild_content_text(static_cast<D3D11Renderer*>(owner), theme);
                },
                &r, 1'759'667'400ull, static_cast<u16>(r.dpi), nullptr})
        .ok();
}

bool initialize(HWND window) noexcept {
    auto& ui = *g_app.account_ui;
    g_app.window = window;
    g_app.account.instance = fixture;
    g_app.account.status.running = true;
    g_app.account.status.profile_id = 9;
    g_app.plugin_account_scope_serial = 7;
    g_app.login.caret_blink_ms = INFINITE;
    if (!require(
            chat_state_store_initialize(&ui.chat_state, {ui.guild_record_storage, 2},
                                           {ui.guild_order_storage, 2}, {ui.channel_record_storage, 8},
                                           {ui.channel_order_storage, 8}, {ui.chat_state_name_storage, 4096}) &&
                chat_state_store_bind_permissions(&ui.chat_state, {fixture->roles, 2},
                                                     {fixture->overwrites, 8}) &&
                user_store_initialize(&ui.user_store, {ui.user_record_storage, 4},
                                              {ui.user_order_storage, 4}, {ui.user_name_storage, 2048}) &&
                navigation_view_initialize(
                    &ui.navigation_view, {ui.navigation_guild_rows, 2}, {ui.navigation_channel_rows, 8},
                    {fixture->channel_scratch, 8}, {fixture->visibility, 8}, {fixture->channel_keys, 8}) &&
                typing_store_initialize(&ui.typing_store, {fixture->typing_records, 4}),
            201))
        return false;
    if (!require(
            message_view_runtime_initialize(&g_app.message_view, ui_extent_units(80), 1, false).ok() &&
                message_rich_store_initialize_owned(&ui.message_rich_store, &ui.message_rich_record_storage,
                                                    kMessageViewItemCapacity,
                                                    {ui.message_rich_order, kMessageViewItemCapacity},
                                                    {ui.message_rich_text, kMessageRichTextCapacity}) &&
                message_view_runtime_bind_rich_store(&g_app.message_view, &ui.message_rich_store).ok() &&
                ui_scene_initialize_bootstrap(&g_app.shell_scene) &&
                ui_scene_layout(&g_app.shell_scene, 632, 600) &&
                guild_rail_scene_initialize(&g_app.guild_rail_scene, {fixture->rail_rows, 2},
                                            {fixture->rail_widgets, 4}, {fixture->rail_nodes, 4}) &&
                shell_navigation_scene_initialize(
                    &g_app.shell_navigation_scene, {fixture->navigation_rows, 8},
                    {fixture->navigation_widgets, 34}, {fixture->navigation_nodes, 34}) &&
                message_scene_initialize_owned(
                    &g_app.message_scene, &g_app.message_view.timeline, kRows, 1,
                    {fixture->message_widgets, kRows * kMessageSceneWidgetsPerRow + 1},
                    {fixture->message_nodes, kRows * kMessageSceneWidgetsPerRow + 1}) &&
                message_composer_initialize(&g_app.message_composer,
                                            {fixture->composer_text, sizeof(fixture->composer_text)}) &&
                message_composer_enable_history(&g_app.message_composer, 8192) &&
                message_composer_scene_initialize(
                    &g_app.message_composer_scene,
                    {fixture->composer_widgets, kMessageComposerWidgetCapacity},
                    {fixture->composer_nodes, kMessageComposerWidgetCapacity}) &&
                message_draft_store_initialize(&ui.message_drafts, {ui.message_draft_records, 4},
                                               {ui.message_draft_text, 8192}) &&
                message_composer_controller_initialize(
                    &ui.message_composer_controller, &g_app.message_composer, &ui.message_drafts,
                    &g_app.message_view.timeline, {ui.message_request_text, kMessageRequestTextCapacity}),
            202))
        return false;
    auto& text = ui.renderer_text;
    text.quads = ui.renderer_text_quads;
    text.batches = ui.renderer_text_batches;
    text.utf16 = ui.response_shape_scratch.shape.utf16;
    text.glyphs = ui.response_shape_scratch.shape.glyphs;
    text.runs = ui.response_shape_scratch.shape.runs;
    text.clusters = ui.response_shape_scratch.shape.clusters;
    text.hit_spans = fixture->hit_spans;
    text.hit_region_scratch = fixture->hit_scratch;
    text.decorations = fixture->decorations;
    text.content_semantics = ui.renderer_content_semantics;
    text.content_hit_regions = ui.renderer_content_hit_regions;
    text.quad_capacity = kRendererContentGlyphCapacity;
    text.batch_capacity = kRendererContentBatchCapacity;
    text.utf16_capacity = text.glyph_capacity = text.cluster_capacity = kRendererContentTextUnitCapacity;
    text.run_capacity = kRendererContentRunCapacity;
    text.hit_span_capacity = kTextHitSpanMaximum;
    text.hit_region_scratch_capacity = text.content_hit_region_capacity = kRendererContentHitRegionCapacity;
    text.decoration_capacity = kRendererTextDecorationCapacity;
    text.content_semantic_capacity = kRendererContentSemanticCapacity;
    if (!require(message_text_presentation_initialize(
                     &ui.message_text, {ui.message_text_commands, kMaximumMessageTextCommands},
                     {ui.message_text_utf8, kMaximumMessageTextUtf8Bytes},
                     {ui.message_text_styles, kMaximumMessageTextStyleSpans},
                     {ui.message_text_semantics, kMaximumMessageTextSemantics},
                     {ui.message_text_measurements, kMaximumMessageTextMeasurements},
                     {ui.message_text_spoiler_reveals, kMaximumMessageTextSpoilerReveals},
                     {fixture->markdown, kMessageTextByteStyleScratchCapacity}) &&
                     ui_text_measure_cache_initialize(
                         &ui.text_measure_cache,
                         {ui.message_text_measure_cache_records, kMaximumMessageTextMeasureCacheRecords},
                         renderer_text_measure_backend, &g_app.renderer) &&
                     renderer_bind_ui_scene(&g_app.renderer, &g_app.shell_scene) &&
                     renderer_bind_guild_rail_scene(&g_app.renderer, &g_app.guild_rail_scene) &&
                     renderer_bind_shell_navigation_scene(&g_app.renderer, &g_app.shell_navigation_scene) &&
                     renderer_bind_message_scene(&g_app.renderer, &g_app.message_scene) &&
                     renderer_bind_message_composer_scene(&g_app.renderer, &g_app.message_composer_scene) &&
                     renderer_bind_text_storage(&g_app.renderer, &text) &&
                     renderer_bind_message_text_presentation(&g_app.renderer, &ui.message_text),
                 203))
        return false;
    if (!require(
            chat_state_store_upsert_guild(&ui.chat_state, {kGuild, utf8_literal("Native composer"),
                                                                 GuildMutationKind::create, 98})
                    .ok() &&
                chat_state_store_upsert_role(
                    &ui.chat_state,
                    {kGuild,
                     kGuild,
                     kPermissionViewChannel | kPermissionReadMessageHistory |
                         kPermissionSendMessages | kPermissionAttachFiles,
                     {}}) &&
                chat_state_store_set_self_roles(&ui.chat_state, kGuild, 98, kSelf, {}) &&
                chat_state_store_upsert_channel(
                    &ui.chat_state, {kSource, kGuild, 0, utf8_literal("compose-review"), 0, 0, 0})
                    .ok() &&
                user_store_upsert(&ui.user_store, kSelf, utf8_literal("Alex"),
                                          utf8_literal("alex"))
                    .ok() &&
                user_store_upsert(&ui.user_store, 98, utf8_literal("Avery"),
                                          utf8_literal("avery"))
                    .ok(),
            204))
        return false;
    chat_state_store_recompute_guild_permissions(&ui.chat_state, kGuild);
    if (!require(
            chat_state_store_select_channel(&ui.chat_state, kSource).ok() &&
                message_composer_controller_select_channel(&ui.message_composer_controller, kSource).ok(),
            205))
        return false;
    ui.active_channel_id = kSource;
    GatewayMessageEvent message{};
    message.id = kSourceMessage;
    message.channel_id = kSource;
    message.author_id = 98;
    message.timestamp_ms = 1'759'667'400'000ull;
    message.content = utf8_literal("Write a clear reply in your preferred language.");
    message.fields = gateway_message_field_id | gateway_message_field_channel_id |
                     gateway_message_field_author_id | gateway_message_field_timestamp |
                     gateway_message_field_content;
    if (!require(message_timeline_apply_created(&g_app.message_view.timeline, message).ok(), 206))
        return false;
    fixture->ready = true;
    return project();
}

bool geometry() noexcept {
    auto& scene = g_app.message_composer_scene;
    const auto* edit = message_composer_scene_layout_node(scene, MessageComposerControl::edit);
    if (!require(edit != nullptr, 220))
        return false;
    if (!require(ui_rect_width(scene.viewport) == fixture->expected_width, 221) ||
        !require(ui_rect_width(edit->content_rectangle) + .01f >=
                     message_composer_scene_text_width(fixture->expected_width),
                 222) ||
        !require(scene.widgets.count == kMessageComposerWidgetCapacity, 223))
        return false;
    const MessageComposerControl controls[]{MessageComposerControl::add_attachment,
                                            MessageComposerControl::edit,
                                            MessageComposerControl::gif,
                                            MessageComposerControl::sticker,
                                            MessageComposerControl::emoji,
                                            MessageComposerControl::send};
    UiRect regions[6]{};
    for (u32 i = 0; i < 6; ++i) {
        if (!message_composer_scene_control_visible(scene, controls[i])) {
            // GIF and sticker fold into the emoji picker between the compact
            // and inline widths; folded buttons take no space or focus.
            if (!require((controls[i] == MessageComposerControl::gif ||
                          controls[i] == MessageComposerControl::sticker) &&
                             fixture->expected_width >= message_composer_scene_minimum_compact_width() &&
                             fixture->expected_width < message_composer_scene_minimum_inline_width() &&
                             !message_composer_scene_focus_control(&scene, controls[i]),
                         246))
                return false;
            continue;
        }
        const auto* node = message_composer_scene_layout_node(scene, controls[i]);
        if (!require(node && ui_rect_has_area(node->rectangle) &&
                         ui_rect_equal(node->rectangle,
                                       ui_rect_intersection(node->rectangle, node->clip_rectangle)),
                     224))
            return false;
        const bool send = controls[i] == MessageComposerControl::send;
        const bool enabled = !send || message_composer_scene_can_send(scene);
        if (send && !require(ui_widget_manager_input_enabled(scene.widgets, scene.send) == enabled, 225))
            return false;
        const auto expected = enabled ? controls[i] : MessageComposerControl::none;
        if (!require(message_composer_scene_hit_test(
                         scene, (node->rectangle.left + node->rectangle.right) * .5f,
                         (node->rectangle.top + node->rectangle.bottom) * .5f) == expected,
                     226))
            return false;
        regions[i] = node->rectangle;
        for (u32 j = 0; j < i; ++j)
            if (ui_rect_has_area(ui_rect_intersection(regions[i], regions[j])))
                return false;
    }
    if (message_composer_scene_toolbar_height(fixture->expected_width) > 0.f) {
        const auto* toolbar = ui_widget_manager_layout_node(scene.widgets, scene.controls);
        if (!require(toolbar != nullptr, 227))
            return false;
        const float gap = toolbar->spec.style.gap;
        if (!require(regions[2].top + .01f >= regions[1].bottom + gap, 228))
            return false;
        for (u32 i = 3; i < 6; ++i) {
            const bool same_row = regions[i].top == regions[i - 1].top;
            if (!require(same_row ? regions[i].left + .01f >= regions[i - 1].right + gap
                                  : regions[i].top + .01f >= regions[i - 1].bottom + gap &&
                                        regions[i].left == regions[2].left,
                         229))
                return false;
        }
        if (fixture->expected_width == 232.f &&
            !require(ui_rect_width(edit->content_rectangle) >= 168.f &&
                         message_composer_scene_toolbar_height(232.f) == 36.f,
                     237))
            return false;
        if (fixture->expected_width == 160.f &&
            !require(regions[2].top == regions[3].top && regions[3].top == regions[4].top &&
                         regions[5].top > regions[4].top &&
                         message_composer_scene_toolbar_height(160.f) == 72.f,
                     238))
            return false;
    }
    // The 632-DIP window minimum keeps a single editor row: attach, editor,
    // emoji and send share it with at least 156 DIP of editable width.
    if (fixture->expected_width == 304.f &&
        !require(message_composer_scene_toolbar_height(304.f) == 0.f &&
                     ui_rect_width(edit->content_rectangle) >= 156.f && regions[4].top == regions[0].top &&
                     regions[5].top == regions[0].top && regions[4].left + .01f >= regions[1].right,
                 245))
        return false;
    if (message_composer_scene_toolbar_height(fixture->expected_width) == 0.f &&
        regions[4].top != regions[0].top)
        return false;
    return true;
}

bool pixel_color(const D3D11_MAPPED_SUBRESOURCE& mapped, UiRect region, Color wanted,
                 u32 minimum_pixels = 24, bool check_contrast = false) noexcept {
    const auto& r = g_app.renderer;
    const float scale = r.dpi / 96.f;
    u32 pixels{}, contrasted{};
    const u32 background_x = minimum(r.width - 1, static_cast<u32>(maximum(0.f, region.left * scale + 1)));
    const u32 background_y = minimum(r.height - 1, static_cast<u32>(maximum(0.f, region.top * scale + 1)));
    const auto* background_pixel =
        static_cast<const u8*>(mapped.pData) + background_y * mapped.RowPitch + background_x * 4;
    const double background = background_color_luminance(
        {background_pixel[2] / 255.f, background_pixel[1] / 255.f, background_pixel[0] / 255.f, 1});
    for (u32 i = 0; i < r.content_text->quad_count; ++i) {
        const auto& q = r.content_text->quads[i];
        if (q.color.red != wanted.red || q.color.green != wanted.green || q.color.blue != wanted.blue)
            continue;
        const auto bounds = ui_rect_intersection(region, {q.left, q.top, q.right, q.bottom});
        if (!ui_rect_has_area(bounds))
            continue;
        const i32 expected[]{static_cast<i32>(wanted.blue * 255 + .5f),
                             static_cast<i32>(wanted.green * 255 + .5f),
                             static_cast<i32>(wanted.red * 255 + .5f)};
        for (i32 y = maximum(0, static_cast<i32>(bounds.top * scale));
             y < minimum(static_cast<i32>(r.height), static_cast<i32>(bounds.bottom * scale + 1)); ++y)
            for (i32 x = maximum(0, static_cast<i32>(bounds.left * scale));
                 x < minimum(static_cast<i32>(r.width), static_cast<i32>(bounds.right * scale + 1)); ++x) {
                const auto* p = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                i32 distance{};
                for (u32 c = 0; c < 3; ++c) {
                    const auto d = p[c] - expected[c];
                    distance += d < 0 ? -d : d;
                }
                if (distance <= 48) {
                    ++pixels;
                    const double ink =
                        background_color_luminance({p[2] / 255.f, p[1] / 255.f, p[0] / 255.f, 1});
                    const double contrast =
                        (maximum(ink, background) + .05) / (minimum(ink, background) + .05);
                    if (contrast >= 4.5)
                        ++contrasted;
                    if (pixels >= minimum_pixels && (!check_contrast || contrasted >= 6))
                        return true;
                }
            }
    }
    return false;
}

bool visual_contract(const D3D11_MAPPED_SUBRESOURCE& mapped) noexcept {
    auto& scene = g_app.message_composer_scene;
    auto& r = g_app.renderer;
    if (!require(geometry(), 210))
        return false;
    const auto* edit = message_composer_scene_layout_node(scene, MessageComposerControl::edit);
    if (!edit)
        return false;
    const auto bounds = ui_rect_intersection(edit->content_rectangle, edit->clip_rectangle);
    if (!g_app.message_composer.bytes) {
        if (!require(pixel_color(mapped, bounds, fixture->theme.text_secondary, 24, true), 211))
            return false;
    } else if (!require(pixel_color(mapped, bounds, fixture->theme.text_primary), 212))
        return false;
    bool timestamp = false;
    bool clipped_timestamp = false;
    const auto& text = g_app.account_ui->message_text;
    for (u32 i = 0; i < text.command_count; ++i) {
        const auto& command = text.commands[i];
        if (command.message_id != kSourceMessage)
            continue;
        MessageAuthorLayout header{};
        if (!message_author_layout(g_app.message_scene, command, g_app.account_ui->chat_state,
                                   g_app.account_ui->user_store, &g_app.account_ui->text_measure_cache,
                                   fixture->theme, &header))
            continue;
        const auto timestamp_bounds =
            ui_rect_intersection(header.timestamp.rectangle, header.timestamp.clip);
        if (fixture->expected_width == 160.f) {
            if (!require(command.styles.count > 1, 241))
                return false;
            const auto& tail = command.styles[1];
            if (!require(tail.byte_count && tail.byte_offset <= command.text.bytes &&
                             tail.byte_count <= command.text.bytes - tail.byte_offset,
                         242))
                return false;
            const Utf8View label{command.text.data + tail.byte_offset, tail.byte_count};
            const TextStyleSpan style{0, label.bytes, tail.flags};
            UiTextMeasureValue measured{};
            const UiTextMeasureRequest request{{command.message_id, 0, command.message_revision, 0, 0},
                                               label,
                                               {&style, 1},
                                               {1024.f, 64.f},
                                               true};
            if (!require(renderer_text_measure_backend(&r, request, &measured) &&
                             measured.line_count == 1 && measured.size.width > 0.f,
                         243))
                return false;
            const bool fits = header.timestamp.text.bytes && ui_rect_has_area(timestamp_bounds) &&
                              measured.size.width <= ui_rect_width(timestamp_bounds) + .01f &&
                              measured.size.height <= ui_rect_height(timestamp_bounds) + .01f;
            u32 secondary_quads{};
            for (u32 q = 0; q < r.content_text->quad_count; ++q) {
                const auto& glyph = r.content_text->quads[q];
                if (glyph.color.red == fixture->theme.text_secondary.red &&
                    glyph.color.green == fixture->theme.text_secondary.green &&
                    glyph.color.blue == fixture->theme.text_secondary.blue &&
                    ui_rect_has_area(ui_rect_intersection(
                        timestamp_bounds, {glyph.left, glyph.top, glyph.right, glyph.bottom})))
                    ++secondary_quads;
            }
            log_u64(LogLevel::info, "composer_timestamp160", "natural_width_milli",
                    static_cast<u64>(measured.size.width * 1000.f));
            log_u64(LogLevel::info, "composer_timestamp160", "visible_width_milli",
                    static_cast<u64>(maximum(0.f, ui_rect_width(timestamp_bounds)) * 1000.f));
            log_u64(LogLevel::info, "composer_timestamp160", "projected_bytes",
                    header.timestamp.text.bytes);
            log_u64(LogLevel::info, "composer_timestamp160", "secondary_quads", secondary_quads);
            if (!fits) {
                // The compact composer fixture also narrows the timeline. Its
                // avatar and measured author may leave only a timestamp
                // fragment, which cannot establish full timestamp contrast.
                if (!require(measured.size.width > ui_rect_width(timestamp_bounds) + .01f &&
                                 (!header.timestamp.text.bytes ||
                                  header.timestamp.rectangle.left >= header.name.rectangle.right + 8.f) &&
                                 (!secondary_quads ||
                                  pixel_color(mapped, timestamp_bounds, fixture->theme.text_secondary, 1)),
                             244))
                    return false;
                clipped_timestamp = true;
                continue;
            }
        }
        if (header.timestamp.text.bytes)
            timestamp =
                timestamp || pixel_color(mapped, timestamp_bounds, fixture->theme.text_secondary, 24, true);
    }
    if (!require(timestamp || clipped_timestamp, 213))
        return false;
    fixture->timestamp_pixel_proofs += timestamp ? 1u : 0u;
    fixture->clipped_timestamp_headers += clipped_timestamp ? 1u : 0u;
    if (g_app.message_composer.anchor != g_app.message_composer.caret &&
        !require(r.composer_layout.selection_count > 0, 214))
        return false;
    if (g_app.message_composer.anchor != g_app.message_composer.caret) {
        const float scale = r.dpi / 96.f;
        const u32 sx = minimum(r.width - 1, static_cast<u32>(maximum(0.f, bounds.right * scale - 1)));
        const u32 sy = minimum(r.height - 1, static_cast<u32>(maximum(0.f, bounds.top * scale + 1)));
        const auto* background = static_cast<const u8*>(mapped.pData) + sy * mapped.RowPitch + sx * 4;
        const i32 expected[]{
            static_cast<i32>(background[0] * .62f + fixture->theme.accent.blue * 255.f * .38f + .5f),
            static_cast<i32>(background[1] * .62f + fixture->theme.accent.green * 255.f * .38f + .5f),
            static_cast<i32>(background[2] * .62f + fixture->theme.accent.red * 255.f * .38f + .5f)};
        u32 selection_pixels{};
        for (u32 i = 0; i < r.content_text->quad_count; ++i) {
            const auto& q = r.content_text->quads[i];
            if (q.color.alpha != .38f || q.color.red != fixture->theme.accent.red ||
                q.color.green != fixture->theme.accent.green || q.color.blue != fixture->theme.accent.blue)
                continue;
            const auto clipped = ui_rect_intersection(bounds, {q.left, q.top, q.right, q.bottom});
            for (i32 y = maximum(0, static_cast<i32>(clipped.top * scale));
                 y < minimum(static_cast<i32>(r.height), static_cast<i32>(clipped.bottom * scale)); ++y)
                for (i32 x = maximum(0, static_cast<i32>(clipped.left * scale));
                     x < minimum(static_cast<i32>(r.width), static_cast<i32>(clipped.right * scale)); ++x) {
                    const auto* pixel = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                    i32 distance{};
                    for (u32 c = 0; c < 3; ++c) {
                        const i32 difference = pixel[c] - expected[c];
                        distance += difference < 0 ? -difference : difference;
                    }
                    if (distance <= 24)
                        ++selection_pixels;
                }
        }
        if (!require(selection_pixels >= 24, 217))
            return false;
    }
    if (g_app.message_composer.focused && !require(scene.caret_visible && r.composer_layout.active, 215))
        return false;
    if (g_app.message_composer.focused) {
        const float scale = r.dpi / 96.f;
        const i32 x = maximum(static_cast<i32>(bounds.left * scale),
                              minimum(static_cast<i32>(bounds.right * scale) - 1,
                                      static_cast<i32>(r.composer_layout.caret.left * scale + .5f)));
        const i32 top = maximum(0, static_cast<i32>(r.composer_layout.caret.top * scale + .5f));
        const i32 bottom = minimum(
            static_cast<i32>(r.height),
            static_cast<i32>((r.composer_layout.caret.top + r.composer_layout.caret.height) * scale));
        u32 caret_pixels{};
        for (i32 y = top; y < bottom; ++y) {
            const auto* pixel = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4;
            const i32 channels[]{static_cast<i32>(fixture->theme.text_primary.blue * 255.f + .5f),
                                 static_cast<i32>(fixture->theme.text_primary.green * 255.f + .5f),
                                 static_cast<i32>(fixture->theme.text_primary.red * 255.f + .5f)};
            i32 delta{};
            for (u32 c = 0; c < 3; ++c) {
                const i32 difference = pixel[c] - channels[c];
                delta += difference < 0 ? -difference : difference;
            }
            if (delta <= 24)
                ++caret_pixels;
        }
        if (!require(caret_pixels >= 2, 216))
            return false;
    }
    return true;
}

bool capture(const wchar_t* filename) noexcept {
    if (!project())
        return false;
    auto& r = g_app.renderer;
    // Flip-model Present advances to another back buffer. Build and draw the
    // current shipping frame, then read this exact target before any Present.
    if (!d3d11_renderer_paint_layout(&r, fixture->theme,
                                     renderer_capture_presentation_stamp(r, fixture->theme)) ||
        !d3d11_renderer_draw_stream(&r, fixture->theme, true, true, nullptr, 10000))
        return require(false, 103);
    const auto notice = fixture->typing;
    UiRect notice_bounds{};
    if (notice.bytes) {
        const auto* node = message_composer_scene_layout_node(g_app.message_composer_scene,
                                                              MessageComposerControl::typing_indicator);
        if (!node)
            return false;
        const auto bounds = ui_rect_intersection(node->content_rectangle, node->clip_rectangle);
        notice_bounds = bounds;
        if (!utf8_equal(message_composer_scene_typing_label(g_app.message_composer_scene), notice))
            return require(false, 104);
        const TextStyleSpan style{0, notice.bytes, text_style_subtext};
        UiTextMeasureValue measured{};
        const UiTextMeasureRequest request{{0x4d53474c494e4b55ull, 1, 1, 0, 1},
                                           notice,
                                           {&style, 1},
                                           {ui_rect_width(bounds), ui_rect_height(bounds)},
                                           true};
        if (!ui_rect_has_area(bounds) ||
            !renderer_text_measure_backend(&g_app.renderer, request, &measured) ||
            measured.line_count != 1 || measured.size.width > ui_rect_width(bounds) + .01f ||
            measured.size.height > ui_rect_height(bounds) + .01f)
            return require(false, 101);
    }
    wchar_t path[1024]{};
    u32 end = GetEnvironmentVariableW(L"MAICORD_UI_PROBE_CAPTURE_DIR", path, 900);
    if (end >= 900)
        return false;
    const bool write_capture = end != 0;
    path[end++] = L'/';
    for (u32 i = 0; filename[i]; ++i) {
        if (end + 1 >= 1024)
            return false;
        path[end++] = filename[i];
    }
    ID3D11Resource* resource{};
    ID3D11Texture2D* source{};
    ID3D11Texture2D* staging{};
    struct Release {
        ID3D11Resource*& resource;
        ID3D11Texture2D*& source;
        ID3D11Texture2D*& staging;
        ~Release() noexcept {
            com_release(resource);
            com_release(source);
            com_release(staging);
        }
    } release{resource, source, staging};
    r.graphics.target->GetResource(&resource);
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source))))
        return false;
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    // Resize may retain a larger backing texture. Evidence represents only
    // the active viewport; each source row still follows the mapped pitch.
    if (!r.width || !r.height || r.width > description.Width || r.height > description.Height ||
        static_cast<u64>(r.width) * r.height * 4 >
            MAXDWORD - sizeof(BITMAPFILEHEADER) - sizeof(BITMAPINFOHEADER))
        return require(false, 102);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = description.MiscFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(r.graphics.device->CreateTexture2D(&description, nullptr, &staging)))
        return false;
    r.graphics.context->CopyResource(staging, source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(r.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    struct Unmap {
        ID3D11DeviceContext* context;
        ID3D11Texture2D* texture;
        ~Unmap() noexcept {
            context->Unmap(texture, 0);
        }
    } unmap{r.graphics.context, staging};
    if (notice.bytes && !probe_glyph_ink(mapped, notice_bounds))
        return require(false, 105);
    bool body_present{}, body_pixels{};
    const auto& text = g_app.account_ui->message_text;
    for (u32 i = 0; i < text.command_count; ++i) {
        const auto& command = text.commands[i];
        if (command.measurement_kind != MessageTextMeasurementKind::content || !command.text.bytes ||
            (command.message_id != kSourceMessage && command.message_id != kTargetMessage))
            continue;
        const auto bounds = ui_rect_intersection(command.rectangle, command.clip);
        if (!ui_rect_has_area(bounds))
            continue;
        body_present = true;
        body_pixels = body_pixels || probe_glyph_ink(mapped, bounds);
    }
    if (g_app.message_view.store.count && (!body_present || !body_pixels))
        return require(false, 106);
    if (!visual_contract(mapped))
        return false;
    if (!write_capture)
        return true;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    struct Close {
        HANDLE file;
        ~Close() noexcept {
            CloseHandle(file);
        }
    } close{file};
    const auto write = [&](const void* data, u32 bytes) noexcept {
        DWORD written{};
        return WriteFile(file, data, bytes, &written, nullptr) && written == bytes;
    };
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + r.width * r.height * 4;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = r.width;
    info.biHeight = -static_cast<LONG>(r.height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    if (!write(&header, sizeof(header)) || !write(&info, sizeof(info)))
        return false;
    for (u32 y = 0; y < r.height; ++y)
        if (!write(static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch, r.width * 4))
            return false;
    return true;
}

struct Keyboard {
    BYTE prior[256];
    bool valid;
    Keyboard(bool control = false, bool shift = false) noexcept : valid(GetKeyboardState(prior) != FALSE) {
        BYTE keys[256]{};
        keys[VK_CONTROL] = control ? 0x80 : 0;
        keys[VK_SHIFT] = shift ? 0x80 : 0;
        valid = valid && SetKeyboardState(keys) != FALSE;
    }
    ~Keyboard() noexcept {
        if (valid)
            SetKeyboardState(prior);
    }
};
bool key(u32 code, bool control = false, bool shift = false) noexcept {
    Keyboard keyboard(control, shift);
    return keyboard.valid && handle_composer_key(code) && fixture->refresh_ok;
}
bool text(Utf8View expected, const wchar_t* value) noexcept {
    if (!focus_message_composer_control(MessageComposerControl::edit) || !key('A', true) || !key(VK_BACK))
        return false;
    Keyboard keyboard;
    if (!keyboard.valid)
        return false;
    for (u32 i = 0; value[i]; ++i)
        if (!handle_composer_character(value[i]))
            return false;
    return utf8_equal(message_composer_text(g_app.message_composer), expected) && project();
}
bool resize(u32 width, u32 height, u32 dpi, float composer_width) noexcept {
    fixture->expected_width = composer_width;
    renderer_set_dpi(&g_app.renderer, dpi);
    return require(renderer_resize(&g_app.renderer, width, height), 234) && require(project(), 235) &&
           require(geometry(), 236);
}
LPARAM center(MessageComposerControl control) noexcept {
    const auto* node = message_composer_scene_layout_node(g_app.message_composer_scene, control);
    if (!node)
        return 0;
    const float scale = g_app.renderer.dpi / 96.f;
    return MAKELPARAM(static_cast<u16>((node->rectangle.left + node->rectangle.right) * .5f * scale),
                      static_cast<u16>((node->rectangle.top + node->rectangle.bottom) * .5f * scale));
}
bool pointer(MessageComposerControl control) noexcept {
    if (!project())
        return false;
    const auto point = center(control);
    if (message_composer_scene_hit_test(
            g_app.message_composer_scene, static_cast<short>(LOWORD(point)) * 96.f / g_app.renderer.dpi,
            static_cast<short>(HIWORD(point)) * 96.f / g_app.renderer.dpi) != control)
        return false;
    const bool result = handle_composer_click(point);
    end_message_composer_pointer_selection();
    return result && fixture->refresh_ok;
}
bool button(MessageComposerControl control) noexcept {
    Keyboard keyboard;
    if (!keyboard.valid || !focus_message_composer_control(control))
        return false;
    return handle_composer_button_key(VK_SPACE, true, false) &&
           handle_composer_button_key(VK_SPACE, true, true) &&
           handle_composer_button_key(VK_SPACE, false, false) && fixture->refresh_ok;
}
bool disabled_send() noexcept {
    const auto queued = g_app.account_ui->message_composer_controller.queued;
    const auto history = fixture->history_calls;
    if (g_app.message_composer.bytes || message_composer_scene_can_send(g_app.message_composer_scene) ||
        handle_composer_click(center(MessageComposerControl::send)) ||
        focus_message_composer_control(MessageComposerControl::send))
        return false;
    Keyboard keyboard;
    if (!keyboard.valid)
        return false;
    handle_composer_button_key(VK_SPACE, true, false);
    handle_composer_button_key(VK_SPACE, false, false);
    return g_app.account_ui->message_composer_controller.queued == queued &&
           fixture->history_calls == history && !g_app.button_keyboard.pressed.manager;
}
bool compact160_routes() noexcept {
    const MessageComposerControl controls[]{MessageComposerControl::add_attachment,
                                            MessageComposerControl::gif, MessageComposerControl::sticker,
                                            MessageComposerControl::emoji};
    u32* counts[]{&fixture->picks, &fixture->gifs, &fixture->stickers, &fixture->emojis};
    const auto queued = g_app.account_ui->message_composer_controller.queued;
    const auto history = fixture->history_calls;
    for (u32 i = 0; i < 4; ++i) {
        const auto before = *counts[i];
        if (!require(pointer(controls[i]) && *counts[i] == before + 1 && button(controls[i]) &&
                         *counts[i] == before + 2 && !g_app.button_keyboard.pressed.manager,
                     239))
            return false;
    }
    // The final wrapped Send target must be reachable, but this viewport
    // case cancels its held key instead of adding another queue request.
    Keyboard keyboard;
    if (!require(keyboard.valid && handle_composer_hover(center(MessageComposerControl::send)) &&
                     focus_message_composer_control(MessageComposerControl::send) &&
                     handle_composer_button_key(VK_SPACE, true, false) &&
                     handle_composer_button_key(VK_SPACE, true, true) &&
                     handle_composer_button_key(VK_ESCAPE, true, false) &&
                     handle_composer_button_key(VK_SPACE, false, false) &&
                     !g_app.button_keyboard.pressed.manager &&
                     g_app.account_ui->message_composer_controller.queued == queued &&
                     fixture->history_calls == history && pointer(MessageComposerControl::edit),
                 240))
        return false;
    return fixture->refresh_ok;
}
bool routing() noexcept {
    const MessageComposerControl controls[]{
        MessageComposerControl::add_attachment, MessageComposerControl::attachments,
        MessageComposerControl::gif, MessageComposerControl::sticker, MessageComposerControl::emoji};
    u32* counts[]{&fixture->picks, &fixture->manages, &fixture->gifs, &fixture->stickers, &fixture->emojis};
    for (u32 i = 0; i < 5; ++i) {
        const auto before = *counts[i];
        if (!pointer(controls[i]) || *counts[i] != before + 1 || !button(controls[i]) ||
            *counts[i] != before + 2 || g_app.button_keyboard.pressed.manager)
            return false;
    }
    if (!application_reply_draft_set(kSourceMessage, true) || !project() ||
        !pointer(MessageComposerControl::reply_mention) || application_reply_draft().mention_reply_author ||
        !button(MessageComposerControl::reply_mention) || !application_reply_draft().mention_reply_author)
        return false;
    if (!pointer(MessageComposerControl::cancel_reply) || application_reply_draft().message_id ||
        !application_reply_draft_set(kSourceMessage) || !project() ||
        !button(MessageComposerControl::cancel_reply) || application_reply_draft().message_id)
        return false;
    fixture->attachments = {};
    fixture->typing = {};
    if (!text(utf8_literal("Pointer send through the real queue"), L"Pointer send through the real queue"))
        return false;
    const u32 queued = g_app.account_ui->message_composer_controller.queued;
    if (!pointer(MessageComposerControl::send) ||
        g_app.account_ui->message_composer_controller.queued != queued + 1 || g_app.message_composer.bytes)
        return false;
    if (!text(utf8_literal("Keyboard send through the real queue"),
              L"Keyboard send through the real queue") ||
        !button(MessageComposerControl::send) ||
        g_app.account_ui->message_composer_controller.queued != queued + 2 || g_app.message_composer.bytes)
        return false;
    return fixture->history_calls == 2;
}

bool stale_geometry() noexcept {
    auto& r = g_app.renderer;
    auto& scene = g_app.message_composer_scene;
    if (!project() || !text_composer_layout_current(r.composer_layout, scene))
        return false;
    const auto prior = scene.viewport;
    auto resized = prior;
    resized.right = resized.left + 232;
    const MessageComposerProjectionContext projection{&g_app.account_ui->user_store,
                                                      &g_app.account_ui->chat_state, kGuild};
    if (!message_composer_scene_update(&scene, &g_app.message_composer, resized, true, 0, {}, &projection)
             .ok() ||
        text_composer_layout_current(r.composer_layout, scene) || !project() ||
        !text_composer_layout_current(r.composer_layout, scene))
        return false;
    if (message_composer_select_all(&g_app.message_composer) != MessageComposerResult::changed ||
        text_composer_layout_current(r.composer_layout, scene) || !refresh_message_composer_ui() ||
        !text_composer_layout_current(r.composer_layout, scene))
        return false;
    const u64 old_document = g_app.message_composer.document_identity;
    if (!message_composer_restore(&g_app.message_composer, kSource, utf8_literal("Restored source"), 0,
                                  0) ||
        old_document == g_app.message_composer.document_identity ||
        text_composer_layout_current(r.composer_layout, scene) || !refresh_message_composer_ui() ||
        !text_composer_layout_current(r.composer_layout, scene))
        return false;
    return true;
}

bool warm(u32 sample) noexcept {
    for (u32 i = 0; i < 4; ++i)
        if (!project() || !probe_present(&g_app.renderer, fixture->theme))
            return false;
    auto& r = g_app.renderer;
    auto& scene = g_app.message_composer_scene;
    auto& controller = g_app.account_ui->message_composer_controller;
    const u32 before[]{scene.projection_passes,
                       scene.layout_passes,
                       g_app.shell_scene.layout_passes,
                       r.composer_layout.builds,
                       r.content_text->rebuilds,
                       r.glyph_text.cache_builds,
                       r.paint_stream_builds,
                       r.paint_instance_uploads,
                       r.text_instance_uploads,
                       r.media_instance_uploads,
                       r.painted_frames,
                       g_app.account_ui->text_measure_cache.backend_calls,
                       controller.draft_commits,
                       controller.queued,
                       fixture->frame_requests};
    const u32 skips = r.unchanged_present_skips;
    const u64 signature = scene.display_signature;
    const auto arena = scene.owned_storage.committed;
    const auto edit_arena = r.composer_layout.storage.arena.committed;
    for (u32 i = 0; i < 64; ++i) {
        application_tick_message_composer_caret(clock_monotonic_milliseconds());
        if (!project() || !probe_present(&r, fixture->theme))
            return false;
    }
    const u32 after[]{scene.projection_passes,
                      scene.layout_passes,
                      g_app.shell_scene.layout_passes,
                      r.composer_layout.builds,
                      r.content_text->rebuilds,
                      r.glyph_text.cache_builds,
                      r.paint_stream_builds,
                      r.paint_instance_uploads,
                      r.text_instance_uploads,
                      r.media_instance_uploads,
                      r.painted_frames,
                      g_app.account_ui->text_measure_cache.backend_calls,
                      controller.draft_commits,
                      controller.queued,
                      fixture->frame_requests};
    constexpr const char* names[]{"projection_delta",   "composer_layout_delta", "shell_layout_delta",
                                  "edit_shape_delta",   "text_build_delta",      "glyph_cache_delta",
                                  "paint_build_delta",  "paint_upload_delta",    "text_upload_delta",
                                  "media_upload_delta", "painted_delta",         "measure_delta",
                                  "draft_commit_delta", "queued_delta",          "frame_request_delta"};
    bool valid = r.unchanged_present_skips - skips == 64 && scene.display_signature == signature &&
                 scene.owned_storage.committed == arena &&
                 r.composer_layout.storage.arena.committed == edit_arena && arena > 0 &&
                 arena <= scene.owned_storage.reserved;
    log_u64(LogLevel::info, "composer_warm", "sample", sample);
    for (u32 i = 0; i < 15; ++i) {
        log_u64(LogLevel::info, "composer_warm", names[i], after[i] - before[i]);
        valid = valid && after[i] == before[i];
    }
    log_u64(LogLevel::info, "composer_warm", "unchanged_skips", r.unchanged_present_skips - skips);
    log_u64(LogLevel::info, "composer_warm", "scene_committed_bytes", scene.owned_storage.committed);
    log_u64(LogLevel::info, "composer_warm", "scene_reserved_bytes", scene.owned_storage.reserved);
    log_u64(LogLevel::info, "composer_warm", "scene_committed_delta",
            scene.owned_storage.committed - arena);
    log_u64(LogLevel::info, "composer_warm", "edit_geometry_committed_delta",
            r.composer_layout.storage.arena.committed - edit_arena);
    log_u64(LogLevel::info, "composer_warm", "display_signature_changed",
            scene.display_signature != signature);
    return valid;
}

// Spelling squiggles: a synthetic misspelling span paints danger-token quads
// under the word inside the edit box (no spell checker involved).
bool spelling_marks() noexcept {
    auto& scene = g_app.message_composer_scene;
    auto& r = g_app.renderer;
    static constexpr TextCompositionSpan span{0, 5, 0, 0, 0, TextCompositionLineStyle::squiggle,
                                              TextCompositionAttribute::input_error, 0, false};
    scene.spelling = {&span, 1};
    scene.spelling_text_revision = g_app.message_composer.text_revision;
    ++scene.composition_revision;
    const auto* edit = message_composer_scene_layout_node(scene, MessageComposerControl::edit);
    const bool painted = project() && edit &&
                         d3d11_renderer_paint_layout(&r, fixture->theme, renderer_capture_presentation_stamp(r, fixture->theme));
    const Color danger = fixture->theme.tokens.accent.danger;
    u32 marks = 0;
    for (u32 i = 0; painted && i < r.paint_stream.count; ++i) {
        const auto& q = r.paint_stream.quads[i];
        if (q.fill.red == danger.red && q.fill.green == danger.green && q.fill.blue == danger.blue &&
            q.top >= edit->content_rectangle.top && q.bottom <= edit->content_rectangle.bottom + 1 &&
            q.left >= edit->content_rectangle.left - 1 && q.right <= edit->content_rectangle.right + 1)
            ++marks;
    }
    scene.spelling = {};
    ++scene.composition_revision;
    return painted && marks >= 3;
}

bool scenarios() noexcept {
    if (!require(resize(632, 600, 96, 304) && disabled_send() &&
                     capture(L"composer-default304-dark-placeholder96.bmp"),
                 1))
        return false;
    if (!require(resize(320, 600, 96, 232) && capture(L"composer-compact232-dark-placeholder96.bmp"), 2))
        return false;
    if (!require(pointer(MessageComposerControl::edit) &&
                     text(utf8_literal("Živjo 日本語 café 😀"), L"Živjo 日本語 café 😀") &&
                     capture(L"composer-compact232-dark-unicode96.bmp") && require(spelling_marks(), 15),
                 3))
        return false;
    if (!require(key(VK_HOME, false, true) &&
                     g_app.message_composer.anchor != g_app.message_composer.caret &&
                     capture(L"composer-compact232-dark-selection96.bmp"),
                 4))
        return false;
    if (!require(key(VK_END) && key(VK_RETURN, false, true) && handle_composer_character('N') &&
                     handle_composer_character('e') && handle_composer_character('w') &&
                     capture(L"composer-compact232-dark-multiline96.bmp") &&
                     g_app.renderer.composer_layout.layout_height >
                         g_app.renderer.composer_layout.em_size &&
                     warm(1),
                 5))
        return false;
    if (!require(resize(248, 600, 96, 160) && text(utf8_literal("Živjo 😀"), L"Živjo 😀") &&
                     compact160_routes() && capture(L"composer-compact160-dark96.bmp"),
                 14))
        return false;
    fixture->theme = kLightTheme;
    if (!require(text({}, L"") && resize(480, 900, 144, 232) &&
                     capture(L"composer-compact232-light-placeholder144.bmp"),
                 6))
        return false;
    if (!require(text(utf8_literal("Unicode stays readable 😀"), L"Unicode stays readable 😀") &&
                     capture(L"composer-compact232-light-unicode144.bmp"),
                 7))
        return false;
    fixture->typing = utf8_literal("Avery is typing…");
    fixture->attachments = utf8_literal("1 file ready");
    if (!require(application_reply_draft_set(kSourceMessage, true) && project() &&
                     capture(L"composer-compact232-light-context144.bmp") && warm(2),
                 8))
        return false;
    fixture->theme = kLightTheme;
    if (!require(message_composer_scene_minimum_inline_width() == 380.f &&
                     resize(468, 600, 96, message_composer_scene_minimum_inline_width()) &&
                     capture(L"composer-boundary380-light96.bmp"),
                 9))
        return false;
    fixture->theme = kDefaultTheme;
    fixture->theme.text_secondary = {.85f, .73f, .42f, 1};
    fixture->attachments = {};
    fixture->typing = {};
    application_reply_draft_cancel();
    if (!require(text({}, L"") && resize(632, 600, 96, 304) &&
                     capture(L"composer-custom-secondary-dark96.bmp"),
                 10))
        return false;
    fixture->theme = kDefaultTheme;
    fixture->theme.background = background_theme_defaults();
    if (!require(resize(480, 900, 144, 232) && capture(L"composer-gradient-dark144.bmp"), 11))
        return false;
    fixture->theme = kDefaultTheme;
    if (!require(resize(1100, 780, 96, 772) &&
                     text(utf8_literal("Native composer restored"), L"Native composer restored") &&
                     stale_geometry() && capture(L"composer-wide-restored-dark96.bmp"),
                 12))
        return false;
    fixture->typing = utf8_literal("Avery is typing…");
    fixture->attachments = utf8_literal("1 file ready");
    if (!require(project() && routing() && fixture->attachment_queue_notices == 2 &&
                     fixture->unexpected_completions == 0 && fixture->refresh_ok &&
                     fixture->timestamp_pixel_proofs == 12 && fixture->clipped_timestamp_headers == 1,
                 13))
        return false;
    log_u64(LogLevel::info, "composer_native", "unexpected_completions", fixture->unexpected_completions);
    log_u64(LogLevel::info, "composer_native", "attachment_queue_notices",
            fixture->attachment_queue_notices);
    log_u64(LogLevel::info, "composer_native", "timestamp_pixel_proofs", fixture->timestamp_pixel_proofs);
    log_u64(LogLevel::info, "composer_native", "clipped_timestamp_headers",
            fixture->clipped_timestamp_headers);
    log_write(LogLevel::info, "probe", "composer_native_unicode_selection_multiline_current_gpu_passed");
    log_write(LogLevel::info, "probe", "composer_native_pointer_keyboard_reply_send_controller_passed");
    log_write(LogLevel::info, "probe", "composer_native_resize_selection_document_geometry_fences_passed");
    log_write(LogLevel::info, "probe", "composer_native_secondary_placeholder_timestamp_gpu_passed");
    log_write(LogLevel::info, "probe", "composer_native_compact_dark_light_64_retained_frames_passed");
    log_write(LogLevel::info, "probe",
              "composer_native_injected_dialog_picker_transport_no_live_ime_clipboard");
    log_write(LogLevel::info, "probe", "composer_native_queue_only_unexpected_completion_zero");
    log_write(LogLevel::info, "probe", "composer_native_compact160_wrapped_controls_cancelled_send_passed");
    log_write(LogLevel::info, "probe", "composer_native_compact160_measured_timestamp_clipped");
    return true;
}
} // namespace composer_probe_detail

bool composer_ui_probe() noexcept {
    using namespace app_detail;
    using namespace composer_probe_detail;
    if (!log_open())
        return false;
    fixture = static_cast<Fixture*>(
        VirtualAlloc(nullptr, sizeof(Fixture), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!fixture || !g_app.account_ui)
        return false;
    fixture->theme = kDefaultTheme;
    fixture->refresh_ok = true;
    fixture->expected_width = 304;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = instance;
    type.lpszClassName = L"MaiCordComposerUiProbe";
    if (!RegisterClassW(&type))
        return false;
    // Never activated, no taskbar button, opened behind every other window.
    constexpr DWORD kWindowExStyle = WS_EX_NOREDIRECTIONBITMAP | probe_window::kExtendedStyle;
    RECT bounds{0, 0, 632, 600};
    if (!AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, kWindowExStyle))
        return false;
    HWND window = CreateWindowExW(kWindowExStyle, type.lpszClassName, L"MaiCord native composer",
                                  WS_OVERLAPPEDWINDOW, 32, 32, bounds.right - bounds.left,
                                  bounds.bottom - bounds.top, nullptr, nullptr, instance, nullptr);
    if (!window)
        return false;
    struct Cleanup {
        HWND window;
        ~Cleanup() noexcept {
            end_message_composer_pointer_selection();
            renderer_shutdown(&g_app.renderer);
            message_scene_release(&g_app.message_scene);
            message_composer_scene_release(&g_app.message_composer_scene);
            message_composer_release(&g_app.message_composer);
            message_view_runtime_release(&g_app.message_view);
            message_rich_store_release(&g_app.account_ui->message_rich_store);
            ui_scene_release(&g_app.shell_scene);
            DestroyWindow(window);
            VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
            VirtualFree(fixture, 0, MEM_RELEASE);
            fixture = nullptr;
            log_close();
        }
    } cleanup{window};
    probe_window::show(window);
    SetFocus(window);
    return require(GetFocus() == window &&
                       renderer_initialize(&g_app.renderer, window) &&
                       initialize(window),
                   100) &&
           scenarios();
}
} // namespace maicord

// Explicit surrounding boundaries. Shipping composer character/key/pointer
// input, focus/reply ownership, edit commands, draft commits and send queue are
// compiled directly. Picker/file dialog admission is observed without opening
// those unrelated surfaces; no live account transport is installed.
namespace maicord::app_detail {
using namespace composer_probe_detail;
ApplicationEmojiUse application_emoji_preferences_scan(Utf8View) noexcept {
    return {};
}
void application_emoji_preferences_track_message(const ApplicationEmojiUse&) noexcept {
}
bool application_guild_community_composer_press() noexcept {
    return false;
}
bool application_request_frame() noexcept {
    ++fixture->frame_requests;
    return true;
}
const Theme& application_theme() noexcept {
    return fixture->theme;
}
UiRect application_viewport_dips() noexcept {
    return {0, 0, g_app.renderer.width * 96.f / g_app.renderer.dpi,
            g_app.renderer.height * 96.f / g_app.renderer.dpi};
}
bool synchronize_shell_layout() noexcept {
    fixture->refresh_ok = fixture->refresh_ok && project();
    return fixture->refresh_ok;
}
bool refresh_account_frame(bool) noexcept {
    return synchronize_shell_layout();
}
bool refresh_account_text(bool) noexcept {
    return synchronize_shell_layout();
}
bool pump_message_history() noexcept {
    ++fixture->history_calls;
    return true;
}
bool application_commands_sync(bool* handled) noexcept {
    if (handled)
        *handled = false;
    return true;
}
bool application_composer_prepare_send(bool* handled) noexcept {
    *handled = false;
    return true;
}
void application_composer_note_sent(u64) noexcept {}
bool application_clear_channel_attachments() noexcept {
    return false;
}
u64 application_message_edit_latest_own() noexcept {
    return 0;
}
bool begin_message_edit(u64) noexcept {
    return false;
}
bool application_mention_confirmation_request(bool* opened) noexcept {
    *opened = false;
    return true;
}
bool application_commands_send(bool* handled) noexcept {
    if (handled)
        *handled = false;
    return true;
}
bool application_plugins_compose(bool* handled) noexcept {
    *handled = false;
    return true;
}
void application_commands_dismiss() noexcept {
}
void application_plugins_menu_dismissed() noexcept {
}
void application_profile_tick(u64) noexcept {
}
bool application_composer_text_input_composing() noexcept {
    return false;
}
void application_composer_text_input_suspend() noexcept {
}
bool application_pick_attachments() noexcept {
    ++fixture->picks;
    return true;
}
bool application_plugins_slot_action(u64, NativePluginSdkSlotTarget, u64) noexcept {
    return false;
}
bool application_plugins_render_click(const MessageTextHit&, UiRect) noexcept {
    return false;
}
bool application_plugins_slot_activate(u64, NativePluginSdkSlotTarget, u64, UiRect) noexcept {
    return false;
}
// Polls are unavailable here, so the add button goes straight to the picker.
bool application_attachment_add_menu() noexcept {
    return application_pick_attachments();
}
bool application_manage_attachments() noexcept {
    ++fixture->manages;
    return true;
}
bool application_paste_attachments() noexcept {
    return false;
}
// The upload strip is exercised by the unit suite; this fixture has no drafts.
bool application_attachment_strip_activate(UiComposerAttachmentHit) noexcept {
    return true;
}
// Voice messages are covered by maicord_voice_message; none is recorded here.
bool application_voice_message_activate(MessageComposerControl) noexcept {
    return false;
}
bool application_voice_message_bar_click() noexcept {
    return false;
}
bool application_attachment_strip_character(u32) noexcept {
    return false;
}
bool application_media_picker_open(UiMediaPickerKind kind) noexcept {
    if (kind == UiMediaPickerKind::gif)
        ++fixture->gifs;
    else if (kind == UiMediaPickerKind::sticker)
        ++fixture->stickers;
    else if (kind == UiMediaPickerKind::emoji)
        ++fixture->emojis;
    else
        return false;
    return true;
}
bool application_attachment_prepare_send(u64 channel, Slice<UploadedAttachment>, Slice<u64>,
                                         u32* count, Slice<u64>) noexcept {
    if (channel != kSource || !count)
        return false;
    *count = 0;
    return true;
}
void application_attachment_did_queue(ConstSlice<u64> ids, u64 nonce) noexcept {
    // Context-row attachment labels are synthetic presentation. Both actual
    // send queues contain text only, so no upload record can be transferred.
    ++fixture->attachment_queue_notices;
    if (ids.count || !nonce)
        fixture->refresh_ok = false;
}
void application_message_forward_completion(const MessageSendCompletion&) noexcept {
    // No transport/history/Gateway delivery receipt is injected by this probe.
    // An exercised completion would exceed the queue-only fixture contract.
    ++fixture->unexpected_completions;
    fixture->refresh_ok = false;
    log_write(LogLevel::error, "probe", "composer_native_unexpected_forward_completion");
}
void application_attachment_complete(u64) noexcept {
    ++fixture->unexpected_completions;
    fixture->refresh_ok = false;
    log_write(LogLevel::error, "probe", "composer_native_unexpected_attachment_completion");
}
bool application_thread_actions_channel_sendable(u64 channel) noexcept {
    const auto* record = chat_state_store_find_channel(g_app.account_ui->chat_state, channel);
    return channel == kSource && application_channel_readable(g_app, channel) && record &&
           (record->actions & channel_action_send_messages);
}
bool cancel_message_edit_editor() noexcept {
    return false;
}
bool queue_message_edit() noexcept {
    return false;
}
bool dismiss_popup() noexcept {
    return false;
}
} // namespace maicord::app_detail
extern "C" __declspec(noreturn) void WINAPI maicord_composer_ui_probe_entry() noexcept {
    ExitProcess(maicord::composer_ui_probe() ? 0 : 1);
}
