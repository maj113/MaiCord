#include "application_media_animation_ui_probe.hpp"
#include "probe_present.hpp"
#include "../../src/app/application_media_pump.hpp"
#include "../../src/app/application_search.hpp"
#include "../../src/app/application_image_viewer.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/media/presentation_policy.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/ui/message_text_presentation.hpp"

#include "maicord/sys/d3d11.hpp"
#include "maicord/app/account_media.hpp"
#include "../../src/app/account_ui_runtime.hpp"

namespace maicord::app_detail {
// Synthetic Search captures seed renderer resources directly. This offline
// host has no accepted account bank or foreground viewer admission authority.
bool application_search_source_ticket(u32, ApplicationSearchSourceTicket* ticket) noexcept {
    if (ticket) *ticket = {};
    return false;
}
bool application_search_source_resolve(const ApplicationSearchSourceTicket&, bool,
                                       ApplicationSearchSourceView* source) noexcept {
    if (source) *source = {};
    return false;
}
bool application_search_source_resolve(u64, u32, bool, ApplicationSearchSourceView* source) noexcept {
    if (source) *source = {};
    return false;
}
void application_search_media_dimensions_changed() noexcept {
}
bool application_image_viewer_source(ApplicationSearchSourceView* source) noexcept {
    if (source) *source = {};
    return false;
}
u32 application_image_viewer_request_keys(MediaAssetKey* keys, u32 capacity) noexcept {
    if (!keys || !capacity || !g_app.image_viewer.scene.active || !media_asset_key_valid(g_app.image_viewer.key))
        return 0;
    keys[0] = g_app.image_viewer.key;
    return 1;
}
bool application_image_viewer_key_current(const MediaAssetKey& key) noexcept {
    return g_app.image_viewer.scene.active && media_asset_key_equal(key, g_app.image_viewer.key);
}
void application_image_viewer_request_failed(const MediaAssetKey&) noexcept {}
// This offline fixture does not select the ordinary video controller.
bool application_video_ordinary(const MediaAssetKey&) noexcept {
    return false;
}
} // namespace maicord::app_detail

namespace maicord {
namespace {
using namespace app_detail;
constexpr u32 kCanvas = 128;
constexpr u64 kMessage = 0x414e494d415445;
constexpr u64 kAttachment = 0x4749464f574e4552;

struct AnimationUiFixture {
    MessageRecord record[1]{};
    u32 order[1]{};
    u8 text[64]{};
    MessageRichRecord rich_record[1]{};
    u16 rich_order[1]{};
    u8 rich_text[1024]{};
    u32 heights[1]{};
    u64 fenwick[1]{};
    MessageSceneRowWidget row[1]{};
    UiWidget widgets[kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode nodes[kMessageSceneWidgetsPerRow + 1]{};
    MessageTextPresentation message_text{};
    AnimationTimelineEntry entries[1]{};
    u32 buckets[2]{};
    AnimationVisualState visuals[1]{};
    u8 indices[kCanvas * kCanvas]{};
    GuildRecord guilds[1]{};
    u16 guild_order[1]{};
    ChannelRecord channels[1]{};
    u16 channel_order[1]{};
    u8 names[64]{};
};

void rebase_renderer_views(D3D11Renderer& renderer) noexcept {
    renderer.composer_layout.selections = renderer.edit_selections;
    renderer.login_edit.selections = renderer.edit_selections;
    renderer.edit_selection_owner = &renderer.composer_layout;
    if (renderer.animation_gpu.cache.allocations.rows)
        renderer.animation_gpu.cache.allocations.rows = renderer.animation_gpu.rows;
    if (renderer.preview_animation_gpu.cache.allocations.rows)
        renderer.preview_animation_gpu.cache.allocations.rows = renderer.preview_animation_gpu.rows;
}

bool attachment_presentation(const MessageScene& scene, MessageMediaCommand* output) noexcept {
    PaintStream paint{};
    MessageMediaCommand commands[2]{};
    MessageMediaCommandStream media{};
    MessagePresentationResult result{};
    const bool built = paint_stream_initialize(&paint) &&
                       paint_stream_reset(&paint, scene.viewport.right, scene.viewport.bottom) &&
                       message_media_command_stream_initialize(&media, {commands, 2}) &&
                       message_presentation_build(scene, kDefaultTheme, &paint, &media, &result);
    bool found = false;
    if (built && output) {
        for (u32 index = 0; index < media.count; ++index) {
            const auto& command = commands[index];
            if (command.kind == MessageMediaCommandKind::attachment && command.id == kAttachment &&
                command.owner.manager == &scene.widgets) {
                *output = command;
                found = true;
            }
        }
    }
    paint_stream_release(&paint);
    return found;
}

void take_renderer(Application& application, D3D11Renderer& renderer) noexcept {
    application.renderer = renderer;
    renderer = {};
    rebase_renderer_views(application.renderer);
}

void return_renderer(Application& application, D3D11Renderer& renderer) noexcept {
    renderer = application.renderer;
    application.renderer = {};
    rebase_renderer_views(renderer);
}

bool seed_fixture(Application& application, AccountUiRuntime& ui, AnimationUiFixture& fixture,
                  UiRect viewport) noexcept {
    auto& view = application.message_view;
    // Frames are current only for a message in a readable active channel
    // (application_channel_readable): a direct message carries its own access.
    ChannelInput channel{};
    channel.id = 7;
    channel.type = 1;
    channel.name = utf8_literal("Ana");
    if (!chat_state_store_initialize(&ui.chat_state, {fixture.guilds, 1}, {fixture.guild_order, 1},
                                     {fixture.channels, 1}, {fixture.channel_order, 1}, {fixture.names, 64}) ||
        !chat_state_store_upsert_channel(&ui.chat_state, channel).ok())
        return false;
    if (!message_store_initialize(&view.store, {fixture.record, 1}, {fixture.order, 1},
                                  {fixture.text, 64}) ||
        !message_store_upsert(&view.store, {kMessage, 7, 9, 0, 0, utf8_literal(""), 0}).ok() ||
        !message_rich_store_initialize(&ui.message_rich_store, {fixture.rich_record, 1},
                                       {fixture.rich_order, 1}, {fixture.rich_text, 1024}) ||
        !ui_variable_list_initialize(&view.list, {fixture.heights, 1}, {fixture.fenwick, 1}, 1,
                                     ui_extent_units(57)) ||
        !message_timeline_initialize(&view.timeline, &view.store, &view.list, ui_extent_units(57),
                                     ui_extent_units(1000), false) ||
        !message_timeline_bind_rich_store(&view.timeline, &ui.message_rich_store) ||
        !message_scene_initialize(&application.message_scene, &view.timeline, {fixture.row, 1},
                                  {fixture.widgets, kMessageSceneWidgetsPerRow + 1},
                                  {fixture.nodes, kMessageSceneWidgetsPerRow + 1}) ||
        !animation_timeline_initialize(&application.animations.timeline, {fixture.entries, 1},
                                       {fixture.buckets, 2}))
        return false;
    GatewayMessageAttachment attachment{};
    attachment.id = kAttachment;
    attachment.filename = utf8_literal("original-two-frame.gif");
    attachment.content_type = utf8_literal("image/gif");
    attachment.url = utf8_literal("https://example.com/original-two-frame.gif");
    attachment.width = attachment.height = kCanvas;
    attachment.has_width = attachment.has_height = true;
    GatewayMessageEvent event{};
    event.id = kMessage;
    event.fields = gateway_message_field_attachments;
    event.attachments = {&attachment, 1};
    bool metrics_changed = false;
    const bool ready =
        message_rich_store_apply(&ui.message_rich_store, kMessage, fixture.record[0].revision, event)
            .ok() &&
        message_scene_update(&application.message_scene, viewport).ok() &&
        message_scene_set_text_metrics(&application.message_scene, 0, 0, 0, false, nullptr, nullptr, {},
                                       &metrics_changed) &&
        message_scene_update(&application.message_scene, viewport).ok();
    application.animations.visuals = fixture.visuals;
    application.account_ui = &ui;
    ui.active_channel_id = 7;
    fixture.message_text.active = true;
    fixture.message_text.projection_signature =
        message_text_presentation_projection_signature(fixture.message_text);
    return ready;
}

bool preview_pixels_equal(D3D11Renderer& renderer, MediaAssetKey key, u32 expected) noexcept {
    const auto allocation = media_atlas_find(renderer.preview_media_atlas, key);
    if (!allocation.ok() || !renderer.media_gpu.preview_texture)
        return false;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = description.Height = 4;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging)))
        return false;
    const D3D11_BOX box{allocation.x,
                        allocation.y,
                        0,
                        static_cast<UINT>(allocation.x + 4),
                        static_cast<UINT>(allocation.y + 4),
                        1};
    renderer.graphics.context1->CopySubresourceRegion(staging, 0, 0, 0, 0,
                                                      renderer.media_gpu.preview_texture, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    bool valid = SUCCEEDED(renderer.graphics.context1->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
    if (valid) {
        for (u32 y = 0; y < 4; ++y) {
            const auto* pixels =
                reinterpret_cast<const u32*>(static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch);
            for (u32 x = 0; x < 4; ++x)
                valid = valid && pixels[x] == expected;
        }
        renderer.graphics.context1->Unmap(staging, 0);
    }
    staging->Release();
    return valid;
}
} // namespace

bool application_media_animation_ui_probe(D3D11Renderer& renderer,
                                          bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!capture || renderer.login_visible || !renderer.message_scene || !renderer.preview_media_atlas)
        return false;
    auto* application = static_cast<Application*>(
        VirtualAlloc(nullptr, sizeof(Application), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!application || !ui) {
        if (application)
            VirtualFree(application, 0, MEM_RELEASE);
        if (ui)
            VirtualFree(ui, 0, MEM_RELEASE);
        return false;
    }
    AnimationUiFixture fixture{};
    const auto* original_messages = renderer.message_scene;
    const auto* original_text = renderer.message_text;
    const auto* original_video = renderer.video_scene;
    const auto viewport = renderer.message_scene->viewport;
    application->clock = clock_initialize();
    application->process_start = clock_now();
    u32 failure_stage = 0;
    const auto require = [&failure_stage](bool accepted, u32 stage) noexcept {
        if (!accepted && !failure_stage)
            failure_stage = stage;
        return accepted;
    };
    bool valid = require(seed_fixture(*application, *ui, fixture, viewport), 1);
    MessageMediaCommand preview{};
    valid = valid && require(attachment_presentation(application->message_scene, &preview) &&
                                 preview.format == static_cast<u8>(MediaAttachmentFormat::gif) &&
                                 ui_rect_equal(ui_rect_intersection(preview.rectangle, preview.clip),
                                               preview.rectangle),
                             2);
    const auto presentation = media_presentation_request(
        kCanvas, kCanvas, ui_rect_width(preview.rectangle), ui_rect_height(preview.rectangle), renderer.dpi,
        media_preview_extent_cap(MediaAttachmentFormat::gif));
    const auto key =
        media_asset_attachment(kAttachment, MediaAttachmentFormat::gif, presentation.resolution);
    GifPalette palette{};
    palette.count = 4;
    palette.rgba[0] = 0xff5050d0;
    palette.rgba[1] = 0xff40b060;
    palette.rgba[2] = 0xffd08040;
    palette.rgba[3] = 0xff40c8d8;
    AccountMediaFrame frame{};
    frame.key = key;
    frame.source_message_id = kMessage;
    frame.source_message_revision = fixture.record[0].revision;
    frame.duration_us = 100000;
    frame.width = kCanvas;
    frame.height = kCanvas;
    frame.canvas_width = kCanvas;
    frame.canvas_height = kCanvas;
    frame.row_pitch = kCanvas;
    frame.frame_count = 2;
    frame.cache_generation = 11;
    frame.kind = AccountMediaFrameKind::gif_indices;
    frame.gif_frame.width = frame.gif_frame.height = kCanvas;
    frame.gif_frame.palette_count = 4;
    frame.gif_palette = &palette;
    frame.bytes = {fixture.indices, sizeof(fixture.indices)};
    for (u32 y = 0; y < kCanvas; ++y)
        for (u32 x = 0; x < kCanvas; ++x)
            fixture.indices[y * kCanvas + x] = ((x / 8 + y / 8) % 2) ? 2 : 0;
    renderer.message_scene = &application->message_scene;
    renderer.message_text = &fixture.message_text;
    renderer.video_scene = nullptr;
    take_renderer(*application, renderer);
    ApplicationMediaPump pump{*application};
    valid = valid && require(pump.message_image_frame_is_current(frame), 3) &&
            require(pump.upload_account_media_frame(frame) ==
                        ApplicationMediaPump::MediaFrameUploadResult::uploaded,
                    4);
    const auto published = animation_timeline_find(&application->animations.timeline, key);
    const u64 now = clock_microseconds(application->clock, application->process_start, clock_now());
    valid = valid && require(published.ok() && animation_timeline_observe_visible(
                                                   &application->animations.timeline, key, now),
                             5);
    return_renderer(*application, renderer);
    renderer.retained_presentation.valid = false;
    valid = valid && require(renderer_rebuild_content_text(&renderer, kDefaultTheme), 6) &&
            require(probe_present(&renderer, kDefaultTheme), 7) &&
            require(renderer.media_quad_count != 0, 17) &&
            require(preview_pixels_equal(renderer, key, palette.rgba[0]), 8) &&
            require(capture(L"animation-owned-first-frame.bmp"), 9);
    if (valid) {
        take_renderer(*application, renderer);
        const u32 before_uploads = application->renderer.media_gpu.gif_frames;
        const auto collected = animation_timeline_collect_due(
            &application->animations.timeline,
            application->animations.timeline.entries[published.slot].due_at_us,
            {application->animations.pending_advances, 1});
        application->animations.pending_advance_count = collected.advances;
        auto wrong_token = application->animations.pending_advances[0];
        ++wrong_token.generation;
        auto* owned_visual = pump.find_animation_visual(key, wrong_token.generation - 1);
        bool stale_repaint = false;
        const bool stale_cached_rejected =
            owned_visual &&
            pump.restore_cached_animation_frame(wrong_token, owned_visual, &stale_repaint) &&
            !stale_repaint && application->renderer.media_gpu.gif_frames == before_uploads &&
            application->animations.pending_advance_count == 1;
        frame.frame_index = 1;
        frame.source_message_id = 0;
        frame.source_message_revision = 0;
        for (u32 y = 0; y < kCanvas; ++y)
            for (u32 x = 0; x < kCanvas; ++x)
                fixture.indices[y * kCanvas + x] = ((x / 8 + y / 8) % 2) ? 3 : 1;
        bool repaint = false;
        bool consumed = false;
        valid = require(stale_cached_rejected, 16) &&
                require(collected.ok() && collected.advances == 1 &&
                            !pump.message_image_frame_is_current(frame),
                        10) &&
                require(pump.consume_pending_animation_frame(frame, &repaint, &consumed), 11) &&
                require(consumed && repaint && application->animations.pending_advance_count == 0 &&
                            application->animations.timeline.entries[published.slot].frame_index == 1 &&
                            !(application->animations.timeline.entries[published.slot].flags &
                              animation_timeline_entry_pending) &&
                            application->renderer.media_gpu.gif_frames == before_uploads + 1,
                        12);
        return_renderer(*application, renderer);
        valid = valid && require(probe_present(&renderer, kDefaultTheme), 13) &&
                require(renderer.media_quad_count != 0, 18) &&
                require(preview_pixels_equal(renderer, key, palette.rgba[1]), 14) &&
                require(capture(L"animation-owned-next-frame.bmp"), 15);
    }
    log_write(valid ? LogLevel::info : LogLevel::error, "probe",
              valid ? "animation_owned_next_frame_ui_passed" : "animation_owned_next_frame_ui_failed");
    log_u64(LogLevel::info, "probe", "animation_visual_owner_slot_bytes", sizeof(AnimationVisualState));
    if (failure_stage)
        log_u64(LogLevel::error, "probe", "animation_owned_next_frame_ui_failure_stage", failure_stage);
    message_scene_release(&application->message_scene);
    renderer.message_scene = original_messages;
    renderer.message_text = original_text;
    renderer.video_scene = original_video;
    renderer.retained_presentation.valid = false;
    content_text_retention_invalidate(&renderer.content_text_retention);
    const bool restored =
        renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
    VirtualFree(ui, 0, MEM_RELEASE);
    VirtualFree(application, 0, MEM_RELEASE);
    return valid && restored;
}
} // namespace maicord
