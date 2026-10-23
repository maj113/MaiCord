#include "audio_scene_ui_probe.hpp"
#include "probe_present.hpp"
#include "probe_capture.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/audio_presentation.hpp"
#include "maicord/ui/theme_style_projection.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"
#include "../../src/render/d3d11_renderer_interaction_state.hpp"

#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {
bool require(bool value, u32 stage) noexcept {
    if (!value)
        log_u64(LogLevel::error, "probe", "audio_native_failure_stage", stage);
    return value;
}

bool paint_capacity(D3D11Renderer& renderer) noexcept {
    D3D11DrawPipelineOwner local{};
    const D3D11DrawPipelineConfig config{sizeof(PaintQuad), 512, sizeof(GlyphQuad), 512,
                                         sizeof(MediaQuad), 8};
    bool valid = d3d11_draw_pipeline_initialize(&local, renderer.graphics.device, renderer.graphics.context,
                                                renderer.width, renderer.height, renderer.dpi, config)
                     .ok();
    D3D11_BUFFER_DESC description{};
    if (valid)
        local.ui_instance_buffer->GetDesc(&description);
    valid = valid && local.ui_instance_capacity == 512 && description.ByteWidth == 512 * sizeof(PaintQuad);
    auto* initial = local.ui_instance_buffer;
    valid =
        valid &&
        d3d11_draw_pipeline_reserve_ui_instances(&local, renderer.graphics.device, sizeof(PaintQuad), 513)
            .ok() &&
        local.ui_instance_capacity == 1024 && local.ui_instance_buffer != initial;
    if (valid)
        local.ui_instance_buffer->GetDesc(&description);
    valid = valid && description.ByteWidth == 1024 * sizeof(PaintQuad);
    auto* grown = local.ui_instance_buffer;
    const auto rejected =
        d3d11_draw_pipeline_reserve_ui_instances(&local, renderer.graphics.device, sizeof(PaintQuad), 4097);
    valid = valid && rejected.error == D3D11DrawPipelineError::invalid_input &&
            local.ui_instance_buffer == grown && local.ui_instance_capacity == 1024;
    d3d11_draw_pipeline_release(&local);
    valid = valid && !local.ui_instance_buffer && !local.ui_instance_capacity;
    valid = require(valid &&
                        d3d11_draw_pipeline_update_transform(renderer.pipeline, renderer.graphics.context,
                                                             renderer.width, renderer.height, renderer.dpi),
                    1);
    if (valid)
        log_write(LogLevel::info, "probe", "audio_native_gpu_paint_512_1024_reject4097_passed");
    return valid;
}

// Scan the current DRAW, before another Present can rotate the swap-chain target.
// Map reads the native staging resource directly: no retained CPU image bank.
bool current_ink(D3D11Renderer& renderer, const Theme& theme, UiRect rectangle, Color foreground,
                 u32* count, u32 stage) noexcept {
    *count = 0;
    const float clear[]{theme.base_lowest.red, theme.base_lowest.green, theme.base_lowest.blue, 1};
    renderer.graphics.context->ClearRenderTargetView(renderer.graphics.target, clear);
    if (!require(d3d11_renderer_draw_stream(&renderer, theme, false, false, nullptr), stage))
        return false;
    ProbeTargetReadback readback;
    D3D11_TEXTURE2D_DESC description{};
    const u32 failure = readback.read_target(renderer, description);
    if (!require(failure == 0, stage + failure))
        return false;
    const auto& mapped = readback.mapped;
    const bool bgra = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    if ((!bgra && description.Format != DXGI_FORMAT_R8G8B8A8_UNORM) || description.SampleDesc.Count != 1 ||
        renderer.width > description.Width || renderer.height > description.Height)
        return require(false, stage + 2);
    const float scale = static_cast<float>(renderer.dpi) / 96;
    const auto left = static_cast<u32>(rectangle.left * scale);
    const auto top = static_cast<u32>(rectangle.top * scale);
    const auto right = static_cast<u32>(rectangle.right * scale);
    const auto bottom = static_cast<u32>(rectangle.bottom * scale);
    const int target[]{static_cast<int>(foreground.red * 255), static_cast<int>(foreground.green * 255),
                       static_cast<int>(foreground.blue * 255)};
    if (right <= renderer.width && bottom <= renderer.height) {
        for (u32 y = top; y < bottom; ++y) {
            const auto* row = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch;
            for (u32 x = left; x < right; ++x) {
                const auto* pixel = row + x * 4;
                const int actual[]{pixel[bgra ? 2 : 0], pixel[1], pixel[bgra ? 0 : 2]};
                bool color_matches = true;
                for (u32 channel = 0; channel < 3; ++channel) {
                    const int difference = actual[channel] - target[channel];
                    color_matches = color_matches && difference >= -24 && difference <= 24;
                }
                *count += color_matches ? 1u : 0u;
            }
        }
    }
    return true;
}

bool uploaded_paint_current(D3D11Renderer& renderer) noexcept {
    D3D11_BUFFER_DESC description{};
    renderer.pipeline.ui_instance_buffer->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = description.MiscFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* staging{};
    if (!require(SUCCEEDED(renderer.graphics.device->CreateBuffer(&description, nullptr, &staging)), 180))
        return false;
    renderer.graphics.context->CopyResource(staging, renderer.pipeline.ui_instance_buffer);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT read = renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    bool equal{};
    if (SUCCEEDED(read)) {
        // Icon quads are snapped and pointed at their atlas cell on upload;
        // their identity and ink stay those of the CPU stream.
        const auto* uploaded = static_cast<const PaintQuad*>(mapped.pData);
        equal = true;
        for (u32 i = 0; equal && i < renderer.paint_stream.count; ++i) {
            const PaintQuad& source = renderer.paint_stream.quads[i];
            equal = ui_icon_quad(source)
                        ? ui_icon_quad(uploaded[i]) && uploaded[i].reserved_x >= 0 &&
                              bytes_equal(&uploaded[i].fill, &source.fill, sizeof(Color) * 2)
                        : bytes_equal(&uploaded[i], &source, sizeof(PaintQuad));
        }
        log_u64(LogLevel::info, "probe", "audio_native_paint_upload_equal", equal);
        renderer.graphics.context->Unmap(staging, 0);
    }
    staging->Release();
    return require(SUCCEEDED(read), 181) && require(equal, 182);
}

void paint_diagnostics(const D3D11Renderer& renderer, const UiAudioScene& scene) noexcept {
    const auto& seek = scene.projection.geometry.seek;
    const auto played = scene.projection.played;
    u32 bars{};
    for (u32 batch_index = 0; batch_index < renderer.paint_stream.batch_count; ++batch_index) {
        const auto& batch = renderer.paint_stream.batches[batch_index];
        if (batch.clip_index >= renderer.paint_stream.clip_count) {
            require(false, 183);
            continue;
        }
        const auto& clip = renderer.paint_stream.clips[batch.clip_index];
        for (u32 index = batch.first_quad; index < batch.first_quad + batch.quad_count; ++index) {
            const auto& quad = renderer.paint_stream.quads[index];
            const UiRect bounds{quad.left, quad.top, quad.right, quad.bottom};
            if (quad.fill.red != played.red || quad.fill.green != played.green ||
                quad.fill.blue != played.blue || quad.fill.alpha != played.alpha ||
                !ui_rect_equal(ui_rect_intersection(bounds, seek), bounds) || ui_rect_width(bounds) > 4.1f)
                continue;
            ++bars;
            log_u64(LogLevel::info, "probe", "audio_native_played_bar_batch", batch_index);
            log_u64(LogLevel::info, "probe", "audio_native_played_bar_clip_intersects",
                    ui_rect_has_area(
                        ui_rect_intersection(bounds, {clip.left, clip.top, clip.right, clip.bottom})));
        }
    }
    log_u64(LogLevel::info, "probe", "audio_native_played_bar_count", bars);
    log_u64(LogLevel::info, "probe", "audio_native_seek_left_x100", static_cast<u64>(seek.left * 100));
    log_u64(LogLevel::info, "probe", "audio_native_seek_top_x100", static_cast<u64>(seek.top * 100));
    log_u64(LogLevel::info, "probe", "audio_native_seek_right_x100", static_cast<u64>(seek.right * 100));
    log_u64(LogLevel::info, "probe", "audio_native_seek_bottom_x100", static_cast<u64>(seek.bottom * 100));
    log_u64(LogLevel::info, "probe", "audio_native_overlay_paint_first",
            renderer.overlay_paint_first_batch);
    log_u64(LogLevel::info, "probe", "audio_native_menu_paint_first", renderer.menu_paint_first_batch);
    log_u64(LogLevel::info, "probe", "audio_native_paint_batches", renderer.paint_stream.batch_count);
    log_u64(LogLevel::info, "probe", "audio_native_paint_quads", renderer.paint_stream.count);
}

struct Fixture {
    MessageRecord records[2]{};
    u32 order[2]{};
    u8 text[128]{};
    MessageRichRecord rich_records[2]{};
    u16 rich_order[2]{};
    u8 rich_text[2048]{};
    u32 heights[2]{};
    u64 fenwick[2]{};
    MessageSceneRowWidget rows[2]{};
    UiWidget widgets[2 * kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode nodes[2 * kMessageSceneWidgetsPerRow + 1]{};
    MessageTextCommand commands[32]{};
    char display[2048]{};
    TextStyleSpan styles[32]{};
    MessageMarkdownSemantic semantics[8]{};
    MessageTextMeasurement measurements[8]{};
    MessageTextSpoilerReveal reveals[2]{};
    u32 scratch[512]{};
    MessageStore store{};
    MessageRichStore rich{};
    UiVariableList list{};
    MessageTimeline timeline{};
    MessageScene messages{};
    MessageTextPresentation message_text{};
    UiScene shell{};
    GuildRailScene guilds{};
    GuildRailRowWidget guild_rows[1]{};
    UiWidget guild_widgets[1]{};
    UiLayoutNode guild_nodes[1]{};
    ShellNavigationScene navigation{};
    ShellNavigationRowWidget navigation_rows[1]{};
    UiWidget navigation_widgets[1]{};
    UiLayoutNode navigation_nodes[1]{};
    UiAudioScene audio{};
    u8 waveform[8]{0, 48, 180, 255, 128, 32, 200, 64};
};

bool initialize(Fixture& fixture) noexcept {
    if (!require(message_store_initialize(&fixture.store, {fixture.records, 2}, {fixture.order, 2},
                                          {fixture.text, sizeof(fixture.text)}) &&
                     message_rich_store_initialize(&fixture.rich, {fixture.rich_records, 2},
                                                   {fixture.rich_order, 2},
                                                   {fixture.rich_text, sizeof(fixture.rich_text)}),
                 301))
        return false;
    for (u32 index = 0; index < 2; ++index) {
        const u64 id = 0x415544494f00ull + index;
        if (!require(message_store_upsert(&fixture.store, {id, 7, 9, 0, 0, utf8_literal(""), 0}).ok(), 302))
            return false;
        GatewayMessageAttachment attachment{};
        attachment.id = 0x415454414300ull + index;
        attachment.filename = index ? utf8_literal("Original audio.opus") : utf8_literal("voice.ogg");
        attachment.content_type = utf8_literal("audio/ogg");
        attachment.url = utf8_literal("https://example.com/original.ogg");
        attachment.duration_ms = 7200000;
        attachment.has_duration = true;
        attachment.waveform = utf8_literal("AID/");
        GatewayMessageEvent event{};
        event.id = id;
        event.flags = index ? 0 : 8192;
        event.fields = gateway_message_field_attachments | gateway_message_field_flags;
        event.attachments = {&attachment, 1};
        if (!require(
                message_rich_store_apply(&fixture.rich, id, fixture.records[index].revision, event).ok(),
                303))
            return false;
    }
    // Timeline admission requires the model and height list to have identical
    // counts. Populate both resident records before binding the two-row list.
    return require(fixture.store.count == 2 && fixture.rich.count == 2 &&
                       ui_variable_list_initialize(&fixture.list, {fixture.heights, 2},
                                                   {fixture.fenwick, 2}, 2, ui_extent_units(180)) &&
                       message_timeline_initialize(&fixture.timeline, &fixture.store, &fixture.list,
                                                   ui_extent_units(180), ui_extent_units(1000), false) &&
                       message_timeline_bind_rich_store(&fixture.timeline, &fixture.rich),
                   304) &&
           require(message_scene_initialize(&fixture.messages, &fixture.timeline, {fixture.rows, 2},
                                            {fixture.widgets, 2 * kMessageSceneWidgetsPerRow + 1},
                                            {fixture.nodes, 2 * kMessageSceneWidgetsPerRow + 1}),
                   305) &&
           require(message_text_presentation_initialize(&fixture.message_text, {fixture.commands, 32},
                                                        {fixture.display, sizeof(fixture.display)},
                                                        {fixture.styles, 32}, {fixture.semantics, 8},
                                                        {fixture.measurements, 8}, {fixture.reveals, 2},
                                                        {fixture.scratch, 512}),
                   306) &&
           require(ui_scene_initialize_bootstrap(&fixture.shell) &&
                       guild_rail_scene_initialize(&fixture.guilds, {fixture.guild_rows, 1},
                                                   {fixture.guild_widgets, 1}, {fixture.guild_nodes, 1}) &&
                       shell_navigation_scene_initialize(&fixture.navigation, {fixture.navigation_rows, 1},
                                                         {fixture.navigation_widgets, 1},
                                                         {fixture.navigation_nodes, 1}),
                   307);
}
} // namespace

bool audio_scene_ui_probe(D3D11Renderer& renderer) noexcept {
    if (!require(renderer.graphics.device && renderer.graphics.context && renderer.graphics.target &&
                     renderer.message_scene && renderer.chat_state && renderer.navigation_view &&
                     renderer.content_text && renderer.message_measure_cache,
                 2) ||
        !paint_capacity(renderer))
        return false;
    Fixture fixture{};
    const auto* old_shell = renderer.ui_scene;
    const auto* old_messages = renderer.message_scene;
    const auto* old_text = renderer.message_text;
    const auto* old_rail = renderer.guild_rail_scene;
    const auto* old_navigation = renderer.shell_navigation_scene;
    const auto* old_composer = renderer.message_composer_scene;
    const auto* old_members = renderer.member_list_scene;
    const auto* old_header = renderer.thread_header;
    const auto* old_focus = renderer.focus_owner;
    const auto* old_video = renderer.video_scene;
    const auto* old_audio = renderer.audio_scene;
    const auto* old_poll = renderer.poll_scene;
    const auto* old_voters = renderer.poll_voters_scene;
    auto* old_profile = renderer.profile_scene;
    auto* old_viewer = renderer.image_viewer;
    const auto* old_settings = renderer.settings_scene;
    const auto* old_theme_editor = renderer.theme_editor_scene;
    const auto* old_friends = renderer.friends_scene;
    const auto* old_requests = renderer.friends_requests_scene;
    const auto* old_picker = renderer.media_picker_scene;
    const auto* old_switcher = renderer.quick_switcher_scene;
    const auto* old_download = renderer.download_scene;
    const auto* old_popup = renderer.popup_scene;
    auto* old_reactions = renderer.reaction_viewer;
    auto* old_form = renderer.text_form;
    auto* old_forum = renderer.forum_form;
    auto* old_browser = renderer.thread_browser_scene;
    const auto* old_search = renderer.search_scene;
    const auto* old_pins = renderer.pinned_messages_scene;
    const auto* old_forward = renderer.message_forward_scene;
    const auto* old_tooltip = renderer.tooltip_scene;
    auto* old_action_bar = renderer.message_action_bar;
    const u32 old_dpi = renderer.dpi;
    const auto system = renderer_text_system(&renderer, UiTextFace::normal);
    const UiThemeTypography original_typography =
        system.typography ? system.typography->value : UiThemeTypography{};
    UiResolvedThemeStyles custom_bank{};
    auto custom_metrics = UiThemeMetrics{};
    custom_metrics.message_left = 22;
    custom_metrics.message_right = 30;
    custom_metrics.message_avatar = 44;
    custom_metrics.message_avatar_gap = 16;
    custom_metrics.message_top = 18;
    custom_metrics.audio_padding = 10;
    custom_metrics.audio_gap = 8;
    custom_metrics.audio_target = 44;
    custom_metrics.audio_title_height = 24;
    custom_metrics.audio_time_width = 140;
    custom_metrics.audio_volume_width = 72;
    custom_metrics.audio_radius = 12;
    auto custom_typography = original_typography;
    custom_typography.body_size = 18;
    custom_typography.body_face = UiTextFace::serif;
    custom_typography.subtext_ratio = .9f;
    bool valid = require(ui_theme_styles_apply(&custom_bank, custom_typography, custom_metrics).ok() &&
                             initialize(fixture),
                         3);
    renderer.ui_scene = &fixture.shell;
    renderer.message_scene = &fixture.messages;
    renderer.message_text = &fixture.message_text;
    // Ordinary native paint/text requires valid rail and navigation bindings
    // even when the focused fixture intentionally has no materialized rows.
    renderer.guild_rail_scene = &fixture.guilds;
    renderer.shell_navigation_scene = &fixture.navigation;
    renderer.message_composer_scene = nullptr;
    renderer.member_list_scene = nullptr;
    renderer.thread_header = nullptr;
    renderer.focus_owner = nullptr;
    renderer.video_scene = nullptr;
    renderer.audio_scene = nullptr;
    renderer.poll_scene = nullptr;
    renderer.poll_voters_scene = nullptr;
    renderer.profile_scene = nullptr;
    renderer.image_viewer = nullptr;
    renderer.settings_scene = nullptr;
    renderer.theme_editor_scene = nullptr;
    renderer.friends_scene = nullptr;
    renderer.friends_requests_scene = nullptr;
    renderer.media_picker_scene = nullptr;
    renderer.quick_switcher_scene = nullptr;
    renderer.download_scene = nullptr;
    renderer.popup_scene = nullptr;
    renderer.reaction_viewer = nullptr;
    renderer.text_form = nullptr;
    renderer.forum_form = nullptr;
    renderer.thread_browser_scene = nullptr;
    renderer.search_scene = nullptr;
    renderer.pinned_messages_scene = nullptr;
    renderer.message_forward_scene = nullptr;
    renderer.tooltip_scene = nullptr;
    renderer.message_action_bar = nullptr;
    for (u32 sample = 0; valid && sample < 8; ++sample) {
        log_u64(LogLevel::info, "probe", "audio_native_sample", sample + 1);
        const bool custom = sample >= 6;
        const bool standalone = sample >= 4;
        const bool narrow = sample == 2 || sample == 3 || sample == 5 || sample == 7;
        const bool voice = !standalone && sample % 2 == 0;
        Theme theme = narrow ? kLightTheme : kDefaultTheme;
        if (custom) {
            theme.styles = &custom_bank;
            theme.style_revision = custom_bank.revision;
            theme.accent = narrow ? Color{.55f, .12f, .35f, 1} : Color{.92f, .35f, .62f, 1};
            theme.text_secondary = narrow ? Color{.18f, .24f, .45f, 1} : Color{.68f, .85f, .78f, 1};
            RendererTypographyCandidate candidate{};
            valid = require(renderer_prepare_typography(&renderer, custom_typography, &candidate) &&
                                renderer_commit_typography(&renderer, &candidate),
                            310);
            renderer_discard_typography(&candidate);
            if (!valid)
                break;
        }
        message_scene_bind_theme_styles(&fixture.messages, theme.styles);
        const auto audio_style = ui_theme_audio_style(theme);
        if (standalone) {
            // Republish the same ordinary record: no extra row, widget bank,
            // decoder, transport or native player owner is needed for this UI proof.
            GatewayMessageAttachment attachment{};
            attachment.id = 0x415454414300ull + 1;
            attachment.filename = (sample == 4 || sample == 6) ? utf8_literal("Original song.mp3")
                                                               : utf8_literal("Original recording.wav");
            attachment.content_type =
                (sample == 4 || sample == 6) ? utf8_literal("audio/mpeg") : utf8_literal("audio/wav");
            attachment.url = (sample == 4 || sample == 6)
                                 ? utf8_literal("https://example.com/original.mp3")
                                 : utf8_literal("https://example.com/original.wav");
            attachment.duration_ms = 7200000;
            attachment.has_duration = true;
            GatewayMessageEvent event{};
            event.id = fixture.records[1].id;
            event.fields = gateway_message_field_attachments | gateway_message_field_flags;
            event.attachments = {&attachment, 1};
            valid = require(
                message_rich_store_apply(&fixture.rich, event.id, fixture.records[1].revision, event)
                        .ok() &&
                    message_rich_attachment_format(fixture.rich, fixture.rich_records[1].attachments[0]) ==
                        ((sample == 4 || sample == 6) ? MediaAttachmentFormat::mp3
                                                      : MediaAttachmentFormat::wav),
                21);
            if (!valid)
                break;
        }
        // Publish the DPI through the native API so GPU frame constants and
        // pixel-space scissor/readback agree with this fixture's DIP layout.
        renderer_set_dpi(&renderer, narrow ? 144 : 96);
        const float scale = static_cast<float>(renderer.dpi) / 96;
        UiShellLayoutMetrics metrics = kGuildChannelShellLayoutMetrics;
        metrics.flags = narrow ? static_cast<u16>(ui_shell_layout_navigation) : u16{};
        metrics.navigation_min_viewport_width = 0;
        // The real row reserves 96 DIPs for its avatar, gap and side padding.
        // A 280-DIP focused message pane therefore leaves a genuine 184-DIP
        // attachment: above the 160-DIP minimum and still compact.
        const float narrow_message_width = custom ? 328.f : 280.f;
        metrics.navigation_width = narrow ? static_cast<float>(renderer.width) / scale -
                                                metrics.guild_rail_width - narrow_message_width
                                          : 0;
        metrics.member_sidebar_width = metrics.composer_height = metrics.message_bottom_margin = 0;
        valid = require(ui_scene_layout_with_metrics(&fixture.shell, UiShellView::guild_channel, metrics,
                                                     static_cast<float>(renderer.width) / scale,
                                                     static_cast<float>(renderer.height) / scale),
                        4);
        const UiRect viewport = ui_scene_content_rectangle(fixture.shell, UiSemantic::message_view);
        valid = valid && require(message_scene_update(&fixture.messages, viewport).ok(), 5);
        bool changed{};
        for (u32 index = 0; valid && index < 2; ++index)
            valid = require(message_scene_set_text_metrics(&fixture.messages, index, 0, 0, false, nullptr,
                                                           nullptr, {}, &changed),
                            6);
        valid = valid && require(message_scene_update(&fixture.messages, viewport).ok() &&
                                     message_text_presentation_build(
                                         &fixture.message_text, fixture.messages, *renderer.chat_state,
                                         renderer.user_store, *renderer.navigation_view, theme, 0)
                                         .ok(),
                                 7);
        valid =
            valid &&
            require(message_text_presentation_measure(&fixture.message_text, renderer.message_measure_cache,
                                                      renderer_text_measure_revision(renderer), &changed) &&
                        message_text_presentation_apply_measurements(&fixture.messages,
                                                                     fixture.message_text, &changed) &&
                        message_scene_update(&fixture.messages, viewport).ok() &&
                        message_text_presentation_build(&fixture.message_text, fixture.messages,
                                                        *renderer.chat_state, renderer.user_store,
                                                        *renderer.navigation_view, theme, 0)
                            .ok(),
                    16);
        if (!valid)
            break;
        const u32 row_index = voice ? 0 : 1;
        const auto& row = fixture.messages.rows[row_index];
        const auto* node = message_scene_rich_item_layout_node(fixture.messages, row,
                                                               MessageSceneRichItemKind::attachment, 0);
        if (node) {
            log_u64(LogLevel::info, "probe", "audio_native_attachment_width_x100",
                    static_cast<u64>(ui_rect_width(node->content_rectangle) * 100));
            log_u64(LogLevel::info, "probe", "audio_native_attachment_height_x100",
                    static_cast<u64>(ui_rect_height(node->content_rectangle) * 100));
        }
        const auto owner =
            UiWidgetReference{&fixture.messages.widgets,
                              message_scene_rich_item_widget(row, MessageSceneRichItemKind::attachment, 0)};
        UiAudioPlaybackState playback{};
        playback.owner = fixture.rich_records[row_index].attachments[0].id;
        playback.generation = owner.handle.generation;
        playback.revision = fixture.records[row_index].revision;
        playback.elapsed_ms = 3600000;
        playback.duration_ms = 7200000;
        playback.volume = 65;
        playback.playing = playback.has_duration = true;
        playback.can_play = playback.can_seek = playback.can_volume = playback.can_download = true;
        playback.voice = voice;
        playback.title = message_rich_store_string(fixture.rich,
                                                   fixture.rich_records[row_index].attachments[0].filename);
        if (!standalone)
            playback.waveform = {fixture.waveform, sizeof(fixture.waveform)};
        valid = require(node && ui_audio_scene_project(&fixture.audio, playback, node->content_rectangle,
                                                       viewport, owner, UiAudioSourceSurface::timeline,
                                                       audio_style, theme),
                        8);
        if (!valid)
            break;
        const auto& geometry = fixture.audio.projection.geometry;
        valid = require(
            geometry.voice == voice && geometry.compact == narrow &&
                ui_rect_height(geometry.bounds) ==
                    ui_audio_card_height(audio_style, ui_rect_width(node->content_rectangle), voice) &&
                ui_rect_width(geometry.bounds) >= 160 &&
                (!narrow || custom ||
                 (ui_rect_width(geometry.bounds) >= 183 && ui_rect_width(geometry.bounds) <= 185)) &&
                ui_rect_equal(ui_rect_intersection(geometry.bounds, viewport), geometry.bounds) &&
                utf8_equal(ui_audio_controls_label(fixture.audio.projection, UiAudioControl::time),
                           narrow ? utf8_literal("1:00:00\n2:00:00") : utf8_literal("1:00:00 / 2:00:00")),
            9);
        valid = valid && require(row.rich_item_layout.heights[kMessageRichAttachmentItemOffset] ==
                                         (ui_audio_card_height(
                                              audio_style, ui_rect_width(node->content_rectangle), voice) +
                                          6) &&
                                     fixture.message_text.command_count >= 3,
                                 17);
        fixture.audio.keyboard_focus = true;
        fixture.audio.focus = UiAudioControl::play_pause;
        const auto activated = ui_audio_scene_key(&fixture.audio, UiAudioKey::activate);
        valid = valid &&
                require(activated.handled && activated.action_requested &&
                            activated.action.control == UiAudioControl::play_pause &&
                            !activated.action.value && ui_audio_action_current(playback, activated.action),
                        20);
        if (standalone) {
            const UiPoint download_point{(geometry.download.left + geometry.download.right) * .5f,
                                         (geometry.download.top + geometry.download.bottom) * .5f};
            const auto pressed =
                ui_audio_scene_pointer(&fixture.audio, UiAudioPointerEvent::down, download_point);
            const auto released =
                ui_audio_scene_pointer(&fixture.audio, UiAudioPointerEvent::up, download_point);
            valid = valid && require(pressed.request_capture && !pressed.action_requested &&
                                         released.release_capture && released.action_requested &&
                                         released.action.control == UiAudioControl::download &&
                                         ui_audio_action_current(playback, released.action),
                                     23);
            ui_audio_scene_focus(&fixture.audio, UiAudioControl::play_pause, true);
        }
        valid = valid && require(renderer_bind_audio_scene(&renderer, &fixture.audio), 101) &&
                require(d3d11_renderer_audio_scene_visible(renderer), 102) &&
                require(renderer_rebuild_content_text(&renderer, theme), 103);
        renderer.retained_presentation.valid = false;
        valid = valid && require(probe_present(&renderer, theme), 11);
        if (valid) {
            paint_diagnostics(renderer, fixture.audio);
            valid = uploaded_paint_current(renderer);
            if (standalone) {
                // Missing peaks are a real 4DIP rail with elapsed progress,
                // never a generated voice waveform.
                u32 progress{};
                const auto& seek = geometry.seek;
                const auto played = fixture.audio.projection.played;
                for (u32 i = 0; i < renderer.paint_stream.count; ++i) {
                    const auto& quad = renderer.paint_stream.quads[i];
                    progress += quad.fill.red == played.red && quad.fill.green == played.green &&
                                        quad.fill.blue == played.blue && quad.fill.alpha == played.alpha &&
                                        quad.left == seek.left &&
                                        quad.right == seek.left + ui_rect_width(seek) * .5f &&
                                        quad.bottom - quad.top == 4
                                    ? 1u
                                    : 0u;
                }
                valid = valid && require(!playback.waveform.count && progress == 1, 22);
            }
        }
        u32 clock_pixels{}, waveform_pixels{};
        if (valid) {
            const bool clock_read =
                current_ink(renderer, theme, geometry.time, theme.text_primary, &clock_pixels, 120);
            const bool waveform_read = current_ink(renderer, theme, geometry.seek,
                                                   fixture.audio.projection.played, &waveform_pixels, 130);
            log_u64(LogLevel::info, "probe", "audio_native_clock_pixels", clock_pixels);
            log_u64(LogLevel::info, "probe", "audio_native_waveform_pixels", waveform_pixels);
            if (standalone)
                log_u64(LogLevel::info, "probe", "audio_native_neutral_seek_pixels", waveform_pixels);
            log_u64(LogLevel::info, "probe", "audio_native_interaction_text_current",
                    d3d11_renderer_interaction_text_state_current(renderer));
            log_u64(LogLevel::info, "probe", "audio_native_overlay_text_batches",
                    renderer.content_text->batch_count - renderer.overlay_text_first_batch);
            valid = require(clock_read, 125) && require(clock_pixels > 3, 126) &&
                    require(waveform_read, 135) && require(waveform_pixels > 3, 136);
            if (standalone) {
                u32 title_pixels{};
                const bool title_read =
                    current_ink(renderer, theme, geometry.title, theme.text_primary, &title_pixels, 260);
                log_u64(LogLevel::info, "probe", "audio_native_standalone_title_pixels", title_pixels);
                valid = require(title_read && title_pixels > 3, 265) && valid;
            }
            if (narrow) {
                const float middle = (geometry.time.top + geometry.time.bottom) * .5f;
                const UiRect upper{geometry.time.left, geometry.time.top, geometry.time.right, middle};
                const UiRect lower{geometry.time.left, middle, geometry.time.right, geometry.time.bottom};
                u32 elapsed_pixels{}, duration_pixels{};
                const bool elapsed_read =
                    current_ink(renderer, theme, upper, theme.text_primary, &elapsed_pixels, 240);
                const bool duration_read =
                    current_ink(renderer, theme, lower, theme.text_primary, &duration_pixels, 250);
                log_u64(LogLevel::info, "probe", "audio_native_compact_elapsed_pixels", elapsed_pixels);
                log_u64(LogLevel::info, "probe", "audio_native_compact_duration_pixels", duration_pixels);
                valid = require(elapsed_read && duration_read, 255) &&
                        require(elapsed_pixels > 3 && duration_pixels > 3, 256) && valid;
            }
        }
        if (valid && custom) {
            // Same-bank publication and font prepare must remain a retained frame.
            const auto layout = fixture.messages.layout_passes;
            const auto shapes = renderer.glyph_text.cache_builds;
            const auto builds = renderer.content_text->rebuilds;
            const auto paint = renderer.paint_stream_builds;
            const auto paint_uploads = renderer.paint_instance_uploads;
            const auto text_uploads = renderer.text_instance_uploads;
            const auto media_uploads = renderer.media_instance_uploads;
            const auto painted = renderer.painted_frames;
            const auto skips = renderer.unchanged_present_skips;
            for (u32 frame = 0; valid && frame < 64; ++frame) {
                valid = !message_scene_bind_theme_styles(&fixture.messages, &custom_bank) &&
                        message_scene_update(&fixture.messages, viewport).ok() &&
                        probe_present(&renderer, theme);
            }
            valid = require(
                valid && fixture.messages.layout_passes == layout &&
                    renderer.glyph_text.cache_builds == shapes &&
                    renderer.content_text->rebuilds == builds && renderer.paint_stream_builds == paint &&
                    renderer.paint_instance_uploads == paint_uploads &&
                    renderer.text_instance_uploads == text_uploads &&
                    renderer.media_instance_uploads == media_uploads &&
                    renderer.painted_frames == painted && renderer.unchanged_present_skips - skips == 64,
                311);
            if (valid)
                log_write(LogLevel::info, "probe",
                          narrow ? "custom_theme_light144_64_zero_layout_shape_upload_draw_passed"
                                 : "custom_theme_dark96_64_zero_layout_shape_upload_draw_passed");
        }
        const auto live = fixture.audio.owner.handle;
        ++fixture.audio.owner.handle.generation;
        const auto center = UiPoint{geometry.play_pause.left + 16, geometry.play_pause.top + 16};
        valid =
            valid && require(!d3d11_renderer_audio_scene_visible(renderer) &&
                                 ui_audio_scene_hit(fixture.audio, center) == UiAudioControl::none &&
                                 !ui_audio_scene_key(&fixture.audio, UiAudioKey::activate).action_requested,
                             13);
        fixture.audio.owner.handle = live;
        const auto clip = fixture.audio.clip;
        fixture.audio.clip.right = geometry.play_pause.left;
        valid = valid && require(ui_audio_scene_hit(fixture.audio, center) == UiAudioControl::none, 14);
        fixture.audio.clip = clip;
        if (valid && sample == 0) {
            // The inactive source still paints a neutral waveform. It must not
            // restore the selected player's played color or elapsed label
            // outside the selected projection's clip.
            fixture.audio.clip.right = geometry.seek.left + ui_rect_width(geometry.seek) * .25f;
            renderer.retained_presentation.valid = false;
            valid = require(
                renderer_rebuild_content_text(&renderer, theme) && probe_present(&renderer, theme), 18);
            const UiRect outside{fixture.audio.clip.right + 1, geometry.seek.top,
                                 geometry.seek.left + ui_rect_width(geometry.seek) * .5f,
                                 geometry.seek.bottom};
            u32 escaped{};
            valid = valid && require(current_ink(renderer, theme, outside, fixture.audio.projection.played,
                                                 &escaped, 190) &&
                                         !escaped,
                                     19);
            fixture.audio.clip = clip;
        }
    }
    RendererTypographyCandidate restore_typography{};
    const bool typography_restored =
        renderer_prepare_typography(&renderer, original_typography, &restore_typography) &&
        renderer_commit_typography(&renderer, &restore_typography);
    renderer_discard_typography(&restore_typography);
    valid = require(typography_restored, 312) && valid;
    // Restore all borrowed bindings before releasing fixture storage.
    renderer.audio_scene = old_audio;
    renderer.poll_scene = old_poll;
    renderer.poll_voters_scene = old_voters;
    renderer.video_scene = old_video;
    renderer_set_dpi(&renderer, old_dpi);
    renderer.ui_scene = old_shell;
    renderer.message_scene = old_messages;
    renderer.message_text = old_text;
    renderer.guild_rail_scene = old_rail;
    renderer.shell_navigation_scene = old_navigation;
    renderer.message_composer_scene = old_composer;
    renderer.member_list_scene = old_members;
    renderer.thread_header = old_header;
    renderer.focus_owner = old_focus;
    renderer.profile_scene = old_profile;
    renderer.image_viewer = old_viewer;
    renderer.settings_scene = old_settings;
    renderer.theme_editor_scene = old_theme_editor;
    renderer.friends_scene = old_friends;
    renderer.friends_requests_scene = old_requests;
    renderer.media_picker_scene = old_picker;
    renderer.quick_switcher_scene = old_switcher;
    renderer.download_scene = old_download;
    renderer.popup_scene = old_popup;
    renderer.reaction_viewer = old_reactions;
    renderer.text_form = old_form;
    renderer.forum_form = old_forum;
    renderer.thread_browser_scene = old_browser;
    renderer.search_scene = old_search;
    renderer.pinned_messages_scene = old_pins;
    renderer.message_forward_scene = old_forward;
    renderer.tooltip_scene = old_tooltip;
    renderer.message_action_bar = old_action_bar;
    renderer.retained_presentation.valid = false;
    content_text_retention_invalidate(&renderer.content_text_retention);
    const bool restored =
        renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
    message_scene_release(&fixture.messages);
    ui_scene_release(&fixture.shell);
    valid = require(restored, 15) && valid;
    log_write(valid ? LogLevel::info : LogLevel::error, "probe",
              valid ? "audio_native_renderer_no_output_passed" : "audio_native_renderer_no_output_failed");
    return valid;
}
} // namespace maicord
