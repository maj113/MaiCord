#include "message_link_ui_probe.hpp"
#include "probe_present.hpp"
#include "probe_glyph_ink.hpp"
#include "probe_capture.hpp"
#include "application_state.hpp"
#include "application_content_input.hpp"
#include "application_search_navigation.hpp"
#include "application_thread_actions.hpp"
#include "application_composer.hpp"
#include "application_profile.hpp"
#include "application_guild_card.hpp"
#include "application_plugins.hpp"
#include "application_quick_switcher.hpp"
#include "application_message_forward.hpp"
#include "application_link_trust.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"
#include "../../src/render/d3d11_renderer_layout.hpp"
#include "maicord/render/renderer_presentation.hpp"
#include "maicord/render/message_hit_test.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/app/account_message_search.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/protocol/channel_overview.hpp"
#include "maicord/protocol/message_pins.hpp"

namespace maicord::app_detail {
constinit Application g_app{};
}

namespace maicord {
namespace message_link_probe_detail {
using namespace app_detail;
using Stage = ApplicationMessageNavigationStage;
constexpr u64 kGuild = 10, kSelf = 99, kSource = 21, kSourceMessage = 909, kTargetMessage = 900;
constexpr u32 kRows = 4;
struct Fixture {
    Theme theme{kDefaultTheme};
    RoleRecord roles[2]{};
    PermissionOverwriteRecord overwrites[16]{};
    GuildRailRowWidget rail_rows[2]{};
    UiWidget rail_widgets[4]{};
    UiLayoutNode rail_nodes[4]{};
    ShellNavigationRowWidget navigation_rows[16]{};
    UiWidget navigation_widgets[66]{};
    UiLayoutNode navigation_nodes[66]{};
    UiWidget message_widgets[kRows * kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode message_nodes[kRows * kMessageSceneWidgetsPerRow + 1]{};
    UiWidget composer_widgets[kMessageComposerWidgetCapacity]{};
    UiLayoutNode composer_nodes[kMessageComposerWidgetCapacity]{};
    char composer_text[256]{};
    TextHitSpan hit_spans[kTextHitSpanMaximum]{};
    TextHitRegion hit_scratch[kRendererContentHitRegionCapacity]{};
    TextDecoration decorations[kRendererTextDecorationCapacity]{};
    u16 channel_scratch[16]{};
    u8 visibility[16]{};
    ChannelSortKey channel_keys[16]{};
    u32 markdown[kMessageTextByteStyleScratchCapacity]{};
    u32 page_structurals[2048]{}, message_structurals[1024]{};
    char decoded_content[4096]{};
    GatewayMessageEvent decoded_messages[4]{};
    GatewayMessageNamedUser decoded_mentions[16]{};
    u64 metadata_id{}, metadata_channel{}, member_id{}, member_channel{}, history_id{};
    u32 metadata_submissions{}, member_submissions{}, history_submissions{}, cancelled{}, frames{};
    bool ready{}, refresh_ok{true};
};
Fixture* fixture;

bool require(bool value, u32 stage) noexcept {
    if (!value)
        log_u64(LogLevel::error, "probe", "message_link_native_failure_stage", stage);
    return value;
}

bool project() noexcept {
    if (!fixture || !fixture->ready)
        return true;
    auto& r = g_app.renderer;
    auto& ui = *g_app.account_ui;
    if (!navigation_view_rebuild(&ui.navigation_view, ui.chat_state).ok())
        return false;
    const auto notice = application_message_mutation_notice_text();
    auto metrics = kGuildChannelShellLayoutMetrics;
    metrics.flags = static_cast<u16>(metrics.flags & ~ui_shell_layout_members);
    metrics.member_sidebar_width = 0;
    // Focused responsive fixture topology: hide the channel sidebar at narrow
    // widths so the actual timeline/composer remain usable. This is not a
    // claim about the shipping account's persisted sidebar preferences.
    if (r.width * 96.f / r.dpi < 600.f)
        metrics.flags = static_cast<u16>(metrics.flags & ~ui_shell_layout_navigation);
    metrics.composer_height = message_composer_scene_preferred_height(
        g_app.message_composer, ui_scene_composer_width(metrics, r.width * 96.f / r.dpi), false,
        notice.bytes != 0);
    metrics.revision = 7700u + metrics.flags + (notice.bytes ? 100u : 0u);
    if (!ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::guild_channel, metrics,
                                      r.width * 96.f / r.dpi, r.height * 96.f / r.dpi) ||
        !guild_rail_scene_update(
             &g_app.guild_rail_scene, &ui.navigation_view,
             ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::guild_navigation), 0)
             .ok() ||
        !shell_navigation_scene_update(
             &g_app.shell_navigation_scene, &ui.navigation_view,
             ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::channel_navigation), 0)
             .ok())
        return false;
    const MessageComposerProjectionContext projection{&ui.user_store, &ui.chat_state, kGuild};
    if (!message_composer_scene_update(&g_app.message_composer_scene, &g_app.message_composer,
                                       ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::composer),
                                       true, 0, {}, &projection, notice, {}, false, true, true,
                                       application_message_mutation_notice_is_error())
             .ok() ||
        !message_scene_update(&g_app.message_scene,
                              ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::message_view))
             .ok())
        return false;
    g_app.message_composer.focused = false;
    message_composer_scene_set_caret_visible(&g_app.message_composer_scene, false);
    renderer_bind_navigation_view(&r, &ui.chat_state, &ui.user_store, &ui.navigation_view);
    r.message_measure_cache = &ui.text_measure_cache;
    return message_text_coordinator_converge(
               {&g_app.message_scene, &ui.message_text, &ui.text_measure_cache, &ui.chat_state,
                &ui.user_store, &ui.navigation_view, &fixture->theme,
                +[](void* owner, const Theme& theme) noexcept {
                    return renderer_rebuild_content_text(static_cast<D3D11Renderer*>(owner), theme);
                },
                &r, 1'759'667'400ull, static_cast<u16>(r.dpi), nullptr})
        .ok();
}

RestClientSubmitResult WINAPI metadata(void*, ChannelOverviewOperation operation, u64 channel,
                                       const ChannelOverviewPatch* patch, u64 id) noexcept {
    if (operation != ChannelOverviewOperation::fetch || patch || !id || fixture->metadata_id)
        return {RestClientSubmitError::invalid_input};
    fixture->metadata_id = id;
    fixture->metadata_channel = channel;
    ++fixture->metadata_submissions;
    return {};
}
RestClientSubmitResult WINAPI member(void*, const ThreadActionRequest* request,
                                     u64 id) noexcept {
    if (!request) {
        if (id != fixture->member_id)
            return {RestClientSubmitError::invalid_input};
        ++fixture->cancelled;
        return {};
    }
    if (!id || fixture->member_id || request->operation != ThreadActionOperation::fetch_self ||
        request->self_user_id != kSelf)
        return {RestClientSubmitError::invalid_input};
    fixture->member_id = id;
    fixture->member_channel = request->thread_id;
    ++fixture->member_submissions;
    return {};
}
RestClientSubmitResult WINAPI cancel_metadata(void*, const MessagePinsRequest*,
                                              u64 id) noexcept {
    if (id != fixture->metadata_id)
        return {RestClientSubmitError::invalid_input};
    ++fixture->cancelled;
    return {};
}
RestClientSubmitResult WINAPI cancel_history(void*, AccountMessageSearchOperation operation,
                                             const MessageSearchQuery* query,
                                             u64 id) noexcept {
    if (operation != AccountMessageSearchOperation::cancel || query || id != fixture->history_id)
        return {RestClientSubmitError::invalid_input};
    ++fixture->cancelled;
    return {};
}

bool initialize_step(bool value, u32 stage) noexcept {
    if (!value)
        log_u64(LogLevel::error, "probe", "message_link_native_initialize_stage", stage);
    return value;
}

bool initialize(HWND window) noexcept {
    auto& ui = *g_app.account_ui;
    g_app.window = window;
    g_app.account.instance = fixture;
    g_app.account.status.running = true;
    g_app.account.status.profile_id = 9;
    g_app.plugin_account_scope_serial = 7;
    g_app.account.api.channel_overview = metadata;
    g_app.account.api.thread_action = member;
    g_app.account.api.pinned_messages = cancel_metadata;
    g_app.account.api.message_search = cancel_history;
    if (!initialize_step(
            chat_state_store_initialize(&ui.chat_state, {ui.guild_record_storage, 2},
                                           {ui.guild_order_storage, 2}, {ui.channel_record_storage, 16},
                                           {ui.channel_order_storage, 16}, {ui.chat_state_name_storage, 4096}),
            201))
        return false;
    if (!initialize_step(chat_state_store_bind_permissions(&ui.chat_state, {fixture->roles, 2},
                                                              {fixture->overwrites, 16}),
                         202))
        return false;
    if (!initialize_step(user_store_initialize(&ui.user_store, {ui.user_record_storage, 4},
                                                       {ui.user_order_storage, 4},
                                                       {ui.user_name_storage, 2048}),
                         203))
        return false;
    if (!initialize_step(guild_member_store_initialize(&ui.guild_member_store,
                                                               {ui.guild_member_record_storage, 4},
                                                               {ui.guild_member_order_storage, 4}),
                         204))
        return false;
    if (!initialize_step(navigation_view_initialize(
                             &ui.navigation_view, {ui.navigation_guild_rows, 2},
                             {ui.navigation_channel_rows, 16}, {fixture->channel_scratch, 16},
                             {fixture->visibility, 16}, {fixture->channel_keys, 16}),
                         205))
        return false;
    if (!initialize_step(message_rich_store_initialize_owned_sparse(
                             &ui.message_rich_store, &ui.message_rich_record_storage,
                             kMessageViewItemCapacity, {ui.message_rich_order, kMessageViewItemCapacity},
                             {ui.message_rich_text, kMessageRichTextCapacity}),
                         206))
        return false;
    if (!initialize_step(
            message_view_runtime_initialize(&g_app.message_view, ui_extent_units(80), 1, false).ok(), 207))
        return false;
    if (!initialize_step(
            message_view_runtime_bind_rich_store(&g_app.message_view, &ui.message_rich_store).ok(), 208))
        return false;
    if (!initialize_step(ui_scene_initialize_bootstrap(&g_app.shell_scene), 209))
        return false;
    // Bootstrap reserves the scene storage; the root widget is materialized
    // by layout. The shipping binder requires that actual root before binding.
    if (!initialize_step(ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::guild_channel,
                                         kGuildChannelShellLayoutMetrics,
                                         g_app.renderer.width * 96.f / g_app.renderer.dpi,
                                         g_app.renderer.height * 96.f / g_app.renderer.dpi),
                         215))
        return false;
    if (!initialize_step(guild_rail_scene_initialize(&g_app.guild_rail_scene, {fixture->rail_rows, 2},
                                                     {fixture->rail_widgets, 4}, {fixture->rail_nodes, 4}),
                         210))
        return false;
    if (!initialize_step(shell_navigation_scene_initialize(
                             &g_app.shell_navigation_scene, {fixture->navigation_rows, 16},
                             {fixture->navigation_widgets, 66}, {fixture->navigation_nodes, 66}),
                         211))
        return false;
    if (!initialize_step(message_scene_initialize_owned(
                             &g_app.message_scene, &g_app.message_view.timeline, kRows, 1,
                             {fixture->message_widgets, kRows * kMessageSceneWidgetsPerRow + 1},
                             {fixture->message_nodes, kRows * kMessageSceneWidgetsPerRow + 1}),
                         212))
        return false;
    if (!initialize_step(
            message_composer_initialize(&g_app.message_composer,
                                        {fixture->composer_text, sizeof(fixture->composer_text)}),
            213))
        return false;
    if (!initialize_step(
            message_composer_scene_initialize(&g_app.message_composer_scene,
                                              {fixture->composer_widgets, kMessageComposerWidgetCapacity},
                                              {fixture->composer_nodes, kMessageComposerWidgetCapacity}),
            214))
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
    if (!initialize_step(message_text_presentation_initialize(
                             &ui.message_text, {ui.message_text_commands, kMaximumMessageTextCommands},
                             {ui.message_text_utf8, kMaximumMessageTextUtf8Bytes},
                             {ui.message_text_styles, kMaximumMessageTextStyleSpans},
                             {ui.message_text_semantics, kMaximumMessageTextSemantics},
                             {ui.message_text_measurements, kMaximumMessageTextMeasurements},
                             {ui.message_text_spoiler_reveals, kMaximumMessageTextSpoilerReveals},
                             {fixture->markdown, kMessageTextByteStyleScratchCapacity}),
                         221))
        return false;
    if (!initialize_step(ui_text_measure_cache_initialize(&ui.text_measure_cache,
                                                          {ui.message_text_measure_cache_records,
                                                           kMaximumMessageTextMeasureCacheRecords},
                                                          renderer_text_measure_backend, &g_app.renderer),
                         222))
        return false;
    if (!initialize_step(renderer_bind_ui_scene(&g_app.renderer, &g_app.shell_scene), 223))
        return false;
    if (!initialize_step(renderer_bind_guild_rail_scene(&g_app.renderer, &g_app.guild_rail_scene), 224))
        return false;
    if (!initialize_step(
            renderer_bind_shell_navigation_scene(&g_app.renderer, &g_app.shell_navigation_scene), 225))
        return false;
    if (!initialize_step(renderer_bind_message_scene(&g_app.renderer, &g_app.message_scene), 226))
        return false;
    if (!initialize_step(
            renderer_bind_message_composer_scene(&g_app.renderer, &g_app.message_composer_scene), 227))
        return false;
    if (!initialize_step(renderer_bind_text_storage(&g_app.renderer, &text), 228))
        return false;
    if (!initialize_step(renderer_bind_message_text_presentation(&g_app.renderer, &ui.message_text), 229))
        return false;
    return true;
}

bool seed(Utf8View content) noexcept {
    application_message_navigation_release();
    application_thread_actions_release();
    fixture->metadata_id = fixture->member_id = fixture->history_id = 0;
    g_app.message_mutation_notice = {};
    g_app.pending_message_jump_channel_id = g_app.pending_message_jump_message_id = 0;
    auto& ui = *g_app.account_ui;
    auto& state = ui.chat_state;
    if (!chat_state_store_upsert_guild(
             &state, {kGuild, utf8_literal("Native navigation"), GuildMutationKind::create, 98})
             .ok() ||
        !chat_state_store_upsert_role(
            &state, {kGuild,
                     kGuild,
                     kPermissionViewChannel | kPermissionReadMessageHistory |
                         kPermissionSendMessages | kPermissionSendMessagesInThreads,
                     {}}) ||
        !chat_state_store_set_self_roles(&state, kGuild, 98, kSelf, {}) ||
        !chat_state_store_upsert_channel(&state,
                                            {kSource, kGuild, 0, utf8_literal("link-review"), 0, 0, 0})
             .ok() ||
        !chat_state_store_upsert_channel(&state,
                                            {22, kGuild, 0, utf8_literal("private-discussions"), 1, 0, 0})
             .ok() ||
        !user_store_upsert(&ui.user_store, kSelf, utf8_literal("Alex"), utf8_literal("alex"))
             .ok())
        return false;
    chat_state_store_recompute_guild_permissions(&state, kGuild);
    if (!chat_state_store_select_channel(&state, kSource).ok() ||
        !message_timeline_reset(&g_app.message_view.timeline) ||
        !message_store_upsert(&g_app.message_view.store,
                              {kSourceMessage, kSource, kSelf, 1'759'667'400'000ull, 0, content, 0})
             .ok() ||
        !message_view_runtime_reindex(&g_app.message_view, ui_extent_units(80), 1, false).ok())
        return false;
    ui.active_channel_id = kSource;
    ++g_app.load_trace.channel_epoch;
    fixture->ready = true;
    fixture->refresh_ok = true;
    return project();
}

bool click() noexcept {
    if (!project() || !probe_present(&g_app.renderer, fixture->theme))
        return false;
    auto& r = g_app.renderer;
    const auto& text = *r.content_text;
    for (u32 i = 0; i < text.content_hit_region_count; ++i) {
        const auto& region = text.content_hit_regions[i];
        if (region.semantic_index >= text.content_semantic_count)
            return false;
        const auto& semantic = text.content_semantics[region.semantic_index];
        if (semantic.message_id != kSourceMessage ||
            (semantic.kind != MessageMarkdownSemanticKind::message_link &&
             semantic.kind != MessageMarkdownSemanticKind::link))
            continue;
        const i32 x = static_cast<i32>((region.left + region.right) * .5f * r.dpi / 96.f);
        const i32 y = static_cast<i32>((region.top + region.bottom) * .5f * r.dpi / 96.f);
        const auto hit = renderer_content_hit_test(r, x, y);
        const auto position = renderer_message_text_position_hit_test(r, x, y);
        const auto found = message_store_find(g_app.message_view.store, kSourceMessage);
        const auto* record =
            found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
        if (!record || !hit.valid || hit.message_id != kSourceMessage ||
            hit.message_revision != record->revision || !position.valid ||
            position.message_id != kSourceMessage || position.message_revision != record->revision)
            return false;
        // The Win32 boundary passes the physical point to shipping input. It
        // never fabricates a MessageTextHit, Source ticket or navigation result.
        return handle_content_click(MAKELPARAM(static_cast<u16>(x), static_cast<u16>(y))) &&
               fixture->refresh_ok;
    }
    return false;
}

bool metadata_response(u64 expected_channel, Utf8View body, u16 status = 200) noexcept {
    const auto id = fixture->metadata_id;
    if (!id || fixture->metadata_channel != expected_channel)
        return false;
    fixture->metadata_id = 0;
    RestClientResponse response{};
    response.request_id = id;
    response.status_code = status;
    response.body_bytes = body.bytes;
    return application_search_navigation_response(response, body) && fixture->refresh_ok;
}
bool submit_metadata(u64 id, u64 channel) noexcept {
    return application_search_navigation_submit(id, clock_monotonic_milliseconds()) &&
           fixture->metadata_id == id && fixture->metadata_channel == channel;
}
bool stage(Stage expected, u64 channel = kSource, bool error = false) noexcept {
    return fixture->refresh_ok && g_app.account_ui->active_channel_id == channel &&
           g_app.account_ui->chat_state.selected_channel_id == channel &&
           g_app.message_mutation_notice.navigation_stage == expected &&
           application_message_mutation_notice_text().bytes &&
           application_message_mutation_notice_is_error() == error;
}

// Account transport is injected; this is the real atomic around-history
// decoder/apply boundary and the shared owner's real receipt fence.
bool history_response(u64 channel, Utf8View body) noexcept {
    constexpr u64 id = 800;
    fixture->history_id = id;
    application_search_navigation_history_submitted(channel, kTargetMessage, id);
    if (!application_message_navigation_history_current(id, channel, kTargetMessage))
        return false;
    RestClientResponse response{};
    response.request_id = id;
    response.status_code = 200;
    response.body_bytes = body.bytes;
    MessageHistoryDecodeScratch scratch{{fixture->page_structurals, 2048},
                                        {fixture->message_structurals, 1024},
                                        {fixture->decoded_content, sizeof(fixture->decoded_content)},
                                        {fixture->decoded_messages, 4},
                                        {fixture->decoded_mentions, 16},
                                        {},
                                        32};
    auto& ui = *g_app.account_ui;
    const auto accepted = message_history_jump_accept_rest(
        &g_app.message_view.timeline, &ui.user_store, &ui.guild_member_store, &ui.chat_state, response,
        body, channel, kTargetMessage, scratch, MessageContentAvailability::available);
    application_search_navigation_history_response(response, accepted);
    fixture->history_id = 0;
    g_app.pending_message_jump_channel_id = g_app.pending_message_jump_message_id = 0;
    return accepted.accepted() && accepted.disposition == MessageHistoryRestDisposition::applied &&
           message_store_find(g_app.message_view.store, kTargetMessage).ok() &&
           !application_message_navigation_active() && !application_message_mutation_notice_text().bytes &&
           project();
}

struct Counters {
    u32 paint, text, paint_upload, text_upload, media_upload, painted, skips, measures, glyphs, position,
        metadata, membership, history, frames;
    u64 layout, projection;
    u32 owner_bytes, notice_stage, state_revision, message_revision;
};
Counters counters() noexcept {
    const auto& r = g_app.renderer;
    return {r.paint_stream_builds,
            r.content_text->rebuilds,
            r.paint_instance_uploads,
            r.text_instance_uploads,
            r.media_instance_uploads,
            r.painted_frames,
            r.unchanged_present_skips,
            g_app.account_ui->text_measure_cache.backend_calls,
            r.glyph_text.cache_builds,
            r.message_hit_test.builds,
            fixture->metadata_submissions,
            fixture->member_submissions,
            fixture->history_submissions,
            fixture->frames,
            g_app.message_scene.widgets.layout.revision,
            g_app.account_ui->message_text.projection_signature,
            application_message_navigation_owner_bytes(),
            static_cast<u32>(g_app.message_mutation_notice.navigation_stage),
            g_app.account_ui->chat_state.channel_mutations +
                g_app.account_ui->chat_state.permission_mutations +
                g_app.account_ui->chat_state.guild_mutations +
                g_app.account_ui->chat_state.activity_mutations,
            g_app.message_view.store.created + g_app.message_view.store.updated +
                g_app.message_view.store.deleted};
}
bool warm(u32 sample) noexcept {
    for (u32 i = 0; i < 4; ++i)
        if (!project() || !probe_present(&g_app.renderer, fixture->theme))
            return false;
    const auto before = counters();
    for (u32 i = 0; i < 64; ++i) {
        application_search_navigation_synchronize();
        application_search_navigation_pending(clock_monotonic_milliseconds());
        application_search_navigation_next_wake();
        application_thread_actions_synchronize();
        if (!project() || !probe_present(&g_app.renderer, fixture->theme))
            return false;
    }
    const auto after = counters();
    const u32 deltas[]{after.paint - before.paint,
                       after.text - before.text,
                       after.paint_upload - before.paint_upload,
                       after.text_upload - before.text_upload,
                       after.media_upload - before.media_upload,
                       after.painted - before.painted,
                       after.measures - before.measures,
                       after.glyphs - before.glyphs,
                       after.position - before.position,
                       after.metadata - before.metadata,
                       after.membership - before.membership,
                       after.history - before.history,
                       after.frames - before.frames};
    constexpr const char* labels[]{
        "paint_build_delta",     "text_build_delta",      "paint_upload_delta",      "text_upload_delta",
        "media_upload_delta",    "painted_delta",         "measure_delta",           "glyph_cache_delta",
        "position_layout_delta", "metadata_submit_delta", "membership_submit_delta", "history_submit_delta",
        "frame_request_delta"};
    log_u64(LogLevel::info, "message_link_warm", "sample", sample);
    bool valid = before.layout == after.layout && before.projection == after.projection &&
                 before.owner_bytes == after.owner_bytes && before.notice_stage == after.notice_stage &&
                 before.state_revision == after.state_revision &&
                 before.message_revision == after.message_revision && after.skips - before.skips == 64;
    for (u32 i = 0; i < 13; ++i) {
        log_u64(LogLevel::info, "message_link_warm", labels[i], deltas[i]);
        valid = valid && !deltas[i];
    }
    log_u64(LogLevel::info, "message_link_warm", "layout_delta", after.layout - before.layout);
    log_u64(LogLevel::info, "message_link_warm", "projection_changed",
            before.projection != after.projection);
    log_u64(LogLevel::info, "message_link_warm", "owner_bytes", after.owner_bytes);
    log_u64(LogLevel::info, "message_link_warm", "owner_bytes_delta",
            after.owner_bytes - before.owner_bytes);
    log_u64(LogLevel::info, "message_link_warm", "notice_stage_changed",
            before.notice_stage != after.notice_stage);
    log_u64(LogLevel::info, "message_link_warm", "state_revision_delta",
            after.state_revision - before.state_revision);
    log_u64(LogLevel::info, "message_link_warm", "message_revision_delta",
            after.message_revision - before.message_revision);
    log_u64(LogLevel::info, "message_link_warm", "unchanged_skips", after.skips - before.skips);
    return valid;
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
    const auto notice = application_message_mutation_notice_text();
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
    ProbeTargetReadback readback;
    if (const u32 failure = probe_target_readback(r, &readback))
        return failure == 2 ? require(false, 102) : false;
    const auto& mapped = readback.mapped;
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
    return probe_write_capture(readback, filename);
}

bool gallery_activation() noexcept {
    if (!seed({}))
        return false;
    auto& scene = g_app.message_scene;
    auto& store = g_app.account_ui->message_rich_store;
    const auto found = message_store_find(g_app.message_view.store, kSourceMessage);
    const auto* record = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    GatewayMessageEmbed embeds[3]{};
    for (auto& embed : embeds) {
        embed.type = GatewayMessageEmbedType::rich;
        embed.url = utf8_literal("https://gallery.example/post");
        embed.image.width = 400;
        embed.image.height = 300;
    }
    embeds[0].description = utf8_literal("Gallery caption");
    embeds[0].image.url = utf8_literal("https://gallery.example/clear.png");
    embeds[1].image.url = utf8_literal("https://gallery.example/first.png");
    embeds[2].image.url = utf8_literal("https://gallery.example/second.png");
    embeds[1].flags = embeds[2].flags = kGatewayMediaExplicitFlag;
    GatewayMessageEvent event{};
    event.id = kSourceMessage;
    event.fields = gateway_message_field_embeds;
    event.embeds = {embeds, 3};
    if (!record || !message_rich_store_apply(&store, record->id, record->revision, event).ok())
        return false;
    static MessageMediaPolicyResult explicit_policy{};
    explicit_policy = {MessageExplicitFilter::blur, true};
    constexpr auto explicit_query = +[](const void* context, const MessageMediaOwner&) noexcept {
        return *static_cast<const MessageMediaPolicyResult*>(context);
    };
    const MessageMediaPolicy previous_policy = scene.media_policy;
    message_scene_set_media_policy(&scene, {&explicit_policy, explicit_query, previous_policy.revision + 1});
    if (!project())
        return false;
    const auto* row = message_scene_find_row(scene, 0);
    const auto* node = row ? message_scene_rich_item_layout_node(scene, *row, MessageSceneRichItemKind::embed, 0)
                           : nullptr;
    if (!node)
        return false;
    const auto block = message_rich_embed_media_rect(row->rich_item_layout, 0, node->content_rectangle, false);
    UiRect cells[3]{};
    for (u32 tile = 0; tile < 3; ++tile)
        cells[tile] = ui_rect_intersection(message_rich_embed_gallery_tile(block, 3, tile),
                                           ui_rect_intersection(node->clip_rectangle, scene.viewport));
    const auto activate = [&](u32 tile) noexcept {
        return application_message_media_reveal_at((cells[tile].left + cells[tile].right) * .5f,
                                                    (cells[tile].top + cells[tile].bottom) * .5f);
    };
    const auto first_key = message_media_reveal_key(kSourceMessage, MessageMediaSource::embed, 1),
               second_key = message_media_reveal_key(kSourceMessage, MessageMediaSource::embed, 2);
    bool ok = ui_rect_has_area(cells[0]) && ui_rect_has_area(cells[1]) && ui_rect_has_area(cells[2]) &&
              activate(1) && message_media_revealed(scene.media_reveals, first_key) &&
              !message_media_revealed(scene.media_reveals, second_key) && !activate(1) && !activate(0) &&
              !application_message_media_reveal_at((cells[0].right + cells[1].left) * .5f,
                                                    (cells[1].top + cells[1].bottom) * .5f);
    explicit_policy.filter = MessageExplicitFilter::block;
    message_scene_set_media_policy(&scene, {&explicit_policy, explicit_query, previous_policy.revision + 2});
    const auto reveals = scene.media_reveals.count;
    ok = ok && activate(2) && scene.media_reveals.count == reveals &&
         !message_media_revealed(scene.media_reveals, second_key) && !activate(0);
    message_scene_set_media_policy(&scene, previous_policy);
    scene.media_reveals = {};
    return ok;
}

bool message_style_boundaries() noexcept {
    MessageHitTestCacheOwner cache{};
    struct Release {
        MessageHitTestCacheOwner& cache;
        ~Release() noexcept { message_hit_test_cache_release(&cache); }
    } release{cache};
    MessageTextCommand command{};
    command.text = utf8_literal("a😀b");
    command.rectangle = command.clip = {0, 0, 240, 40};
    command.message_id = 1;
    MessageTextPresentation presentation{};
    presentation.commands = {&command, 1};
    presentation.command_count = 1;
    presentation.active = true;
    // Zero-flag spans still require validation before the unstyled fallback.
    const TextStyleSpan spans[]{{0, 1}, {1, 4}, {5, 1}, {0, 6},
                               {2, 3}, {1, 3}, {6, 1}, {7, 1}, {0, 0}};
    for (u32 i = 0; i < sizeof(spans) / sizeof(spans[0]); ++i) {
        command.styles = {spans + i, 1};
        message_hit_test_cache_invalidate(&cache);
        const auto hit = message_position_hit_test(&cache, presentation, g_app.renderer.glyph_text.system,
                                                   96, 8, 8);
        if (hit.valid != (i < 4)) return false;
    }
    command.text = utf8_literal("a\x80" "b");
    command.styles = {spans, 1};
    message_hit_test_cache_invalidate(&cache);
    return !message_position_hit_test(&cache, presentation, g_app.renderer.glyph_text.system, 96, 8, 8).valid;
}

bool scenarios() noexcept {
    if (!require(gallery_activation(), 11))
        return false;
    constexpr auto ordinary = utf8_literal(
        R"({"id":"70","guild_id":"10","type":0,"name":"release-notes","permission_overwrites":[]})");
    constexpr auto child = utf8_literal(
        R"({"id":"71","guild_id":"10","parent_id":"72","type":11,"name":"Review thread","thread_metadata":{"archived":true,"locked":false}})");
    constexpr auto parent = utf8_literal(
        R"({"id":"72","guild_id":"10","type":15,"name":"design-forum","permission_overwrites":[]})");
    if (!require(seed(utf8_literal("[Open the release message](https://discord.com/channels/10/70/900)")) &&
                     capture(L"message-link-source-wide.bmp") && click() && submit_metadata(100, 70) &&
                     stage(Stage::loading_channel) && capture(L"message-link-loading-channel.bmp") &&
                     warm(1),
                 1))
        return false;
    if (!require(click() && stage(Stage::busy) && fixture->metadata_id == 100 &&
                     capture(L"message-link-busy.bmp") && metadata_response(70, ordinary) &&
                     stage(Stage::opening_history, 70) && capture(L"message-link-opening-history.bmp"),
                 2))
        return false;
    if (!require(
            history_response(
                70,
                utf8_literal(
                    R"([{"id":"900","channel_id":"70","author":{"id":"99","username":"alex"},"content":"Release message opened from its permalink.","timestamp":"2025-10-05T09:00:00.000Z","type":0}])")) &&
                capture(L"message-link-success.bmp") && warm(2),
            3))
        return false;
    if (!require(
            seed(utf8_literal("[Read the archived discussion](https://discord.com/channels/10/71/900)")) &&
                click() && submit_metadata(200, 71) && metadata_response(71, child) &&
                stage(Stage::loading_parent) && submit_metadata(201, 72) &&
                capture(L"message-link-loading-parent.bmp") && metadata_response(72, parent) &&
                submit_metadata(202, 71) && metadata_response(71, child) &&
                stage(Stage::opening_history, 71) && !fixture->member_submissions,
            4))
        return false;
    if (!require(
            seed(utf8_literal("[Read the private discussion](https://discord.com/channels/10/73/900)")) &&
                click() && submit_metadata(300, 73) &&
                metadata_response(
                    73,
                    utf8_literal(
                        R"({"id":"73","guild_id":"10","parent_id":"22","type":12,"name":"Private review","thread_metadata":{"archived":true,"locked":false}})")) &&
                stage(Stage::waiting_membership) && capture(L"message-link-membership.bmp") &&
                application_thread_actions_submit(301, clock_monotonic_milliseconds()) &&
                fixture->member_channel == 73,
            5))
        return false;
    RestClientResponse member_response{};
    member_response.request_id = fixture->member_id;
    member_response.status_code = 403;
    fixture->member_id = 0;
    if (!require(application_thread_actions_response(member_response, {}) &&
                     (application_search_navigation_synchronize(),
                      stage(Stage::failure_membership, kSource, true)) &&
                     capture(L"message-link-membership-denied.bmp"),
                 6))
        return false;
    if (!require(seed(utf8_literal("[Read a withdrawn message](https://discord.com/channels/10/74/900)")) &&
                     click() && submit_metadata(400, 74) && metadata_response(74, {}, 403) &&
                     stage(Stage::permission_denied, kSource, true) &&
                     capture(L"message-link-permission-denied.bmp"),
                 7))
        return false;
    if (!require(seed(utf8_literal(
                     "[Read a malformed channel receipt](https://discord.com/channels/10/75/900)")) &&
                     click() && submit_metadata(500, 75) &&
                     metadata_response(75, utf8_literal("{broken")) && stage(Stage::error, kSource, true) &&
                     capture(L"message-link-malformed.bmp"),
                 8))
        return false;
    if (!require(
            seed(utf8_literal("[Read another discussion](https://discord.com/channels/10/76/900)")) &&
                click() && submit_metadata(600, 76) && application_message_navigation_cancel() &&
                stage(Stage::cancelled) && !application_message_navigation_active() && fixture->cancelled &&
                metadata_response(
                    76,
                    utf8_literal(
                        R"({"id":"76","guild_id":"10","type":0,"name":"late","permission_overwrites":[]})")) &&
                !chat_state_store_find_channel(g_app.account_ui->chat_state, 76) &&
                capture(L"message-link-cancelled.bmp"),
            9))
        return false;
    fixture->theme = kLightTheme;
    g_app.preferences.reduced_motion = true;
    renderer_set_dpi(&g_app.renderer, 144);
    if (!require(renderer_resize(&g_app.renderer, 480, 960) &&
                     seed(utf8_literal("[Open a message](https://discord.com/channels/10/77/900)")) &&
                     click() && submit_metadata(700, 77) &&
                     capture(L"message-link-narrow-light-loading.bmp") && warm(3) &&
                     metadata_response(77, {}, 404) && stage(Stage::message_missing, kSource, true) &&
                     capture(L"message-link-narrow-light-missing.bmp") && warm(4),
                 10))
        return false;
    log_write(LogLevel::info, "probe",
              "message_link_native_click_controller_metadata_membership_history_passed");
    log_write(LogLevel::info, "probe",
              "message_link_native_injected_transport_no_live_account_or_external_launcher");
    log_write(LogLevel::info, "probe", "message_link_native_pending_settled_64_retained_frames_passed");
    return true;
}
} // namespace message_link_probe_detail

bool message_link_ui_probe() noexcept {
    using namespace app_detail;
    using namespace message_link_probe_detail;
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
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = instance;
    type.lpszClassName = L"MaiCordMessageLinkUiProbe";
    if (!RegisterClassW(&type))
        return false;
    RECT bounds{0, 0, 1100, 780};
    if (!AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0))
        return false;
    HWND window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, type.lpszClassName, L"MaiCord message links",
                                  WS_OVERLAPPEDWINDOW, 32, 32, bounds.right - bounds.left,
                                  bounds.bottom - bounds.top, nullptr, nullptr, instance, nullptr);
    if (!window)
        return false;
    struct Cleanup {
        HWND window;
        ~Cleanup() noexcept {
            application_message_navigation_release();
            application_thread_actions_release();
            renderer_shutdown(&g_app.renderer);
            message_scene_release(&g_app.message_scene);
            message_composer_scene_release(&g_app.message_composer_scene);
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
    return require(renderer_initialize(&g_app.renderer, window) &&
                       initialize(window),
                   100) &&
           require(message_style_boundaries(), 101) && scenarios();
}
} // namespace maicord

// Surrounding account/UI integration seams. The real navigator, click routing,
// notice, Core selection/permissions, native geometry, membership service and
// around-history decoder remain linked from their shipping translation units.
namespace maicord::app_detail {
using namespace message_link_probe_detail;
// application_content_input.cpp opens the server header popout.
bool application_guild_menu_toggle(bool) noexcept {
    return false;
}
bool application_private_channel_row_close(u64, float, float) noexcept {
    return false;
}
bool application_private_channel_requests_mode() noexcept {
    return false;
}
void application_private_channel_set_requests_mode(bool) noexcept {}
bool application_request_frame() noexcept {
    ++fixture->frames;
    return true;
}
const Theme& application_theme() noexcept {
    return fixture->theme;
}
bool refresh_message_composer_ui() noexcept {
    fixture->refresh_ok = fixture->refresh_ok && project();
    return fixture->refresh_ok;
}
bool refresh_account_text(bool) noexcept {
    return refresh_message_composer_ui();
}
bool synchronize_navigation_view() noexcept {
    return navigation_view_rebuild(&g_app.account_ui->navigation_view,
                                           g_app.account_ui->chat_state)
        .ok();
}
bool synchronize_selected_channel() noexcept {
    g_app.account_ui->active_channel_id = g_app.account_ui->chat_state.selected_channel_id;
    ++g_app.load_trace.channel_epoch;
    return message_timeline_reset(&g_app.message_view.timeline);
}
bool synchronize_shell_layout() noexcept {
    return project();
}
bool pump_message_history() noexcept {
    // The native fixture scheduler submits the typed history receipt after the
    // shared owner has completed synchronous destination admission.
    ++fixture->history_submissions;
    return true;
}
bool animate_guild_rail_scroll_from(float) noexcept {
    return true;
}
bool scroll_to_resident_message(u64 id) noexcept {
    return message_store_find(g_app.message_view.store, id).ok();
}
bool application_stop_account() noexcept {
    g_app.account.status.running = false;
    return true;
}
void application_account_stop_recoverable(const char*) noexcept {
    application_stop_account();
}
float client_dip_x(LPARAM p) noexcept {
    return static_cast<short>(LOWORD(p)) * 96.f / g_app.renderer.dpi;
}
float client_dip_y(LPARAM p) noexcept {
    return static_cast<short>(HIWORD(p)) * 96.f / g_app.renderer.dpi;
}
bool application_search_navigation_source(const ApplicationSearchSourceTicket&,
                                          ApplicationSearchNavigationSource*) noexcept {
    return false;
}
bool application_search_source_notice(const ApplicationSearchSourceTicket&, Utf8View) noexcept {
    return false;
}
bool application_profile_open(u64, UiRect, ProfileAnchor) noexcept {
    return false;
}
bool application_guild_card_open(u64, UiRect) noexcept {
    return false;
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
bool application_friends_active() noexcept {
    return false;
}
bool application_friends_show() noexcept {
    return false;
}
void application_friends_hide() noexcept {
}
bool application_thread_browser_active() noexcept {
    return false;
}
bool application_thread_browser_read_blocked() noexcept {
    return false;
}
bool application_thread_browser_open(u64) noexcept {
    return false;
}
void application_thread_browser_close() noexcept {
}
void application_read_state_activity() noexcept {
}
void application_read_state_log_guild_unread(u64) noexcept {}
bool application_message_action_bar_activate(const MessageActionBarHit&, bool) noexcept {
    return false;
}
bool application_message_forward_open(u64) noexcept {
    return false;
}
bool open_reaction_picker(u64, UiRect) noexcept {
    return false;
}
bool application_downloads_save_target(float, float) noexcept {
    return false;
}
bool application_downloads_save_press(float, float) noexcept {
    return false;
}
bool application_video_open_focused() noexcept {
    return false;
}
bool application_video_cursor(UiPoint, UiCursor*) noexcept {
    return false;
}
bool application_poll_cursor(UiPoint, UiCursor*) noexcept {
    return false;
}
bool application_gif_favorite_star_target(float, float) noexcept {
    return false;
}
bool application_image_viewer_open_at(float, float, bool) noexcept {
    return false;
}
bool application_composer_fill_command(Utf8View) noexcept {
    return true;
}
bool begin_message_reply(u64) noexcept {
    return false;
}
bool queue_message_reaction(u64, u32) noexcept {
    return false;
}
bool queue_message_reaction_emoji(u64, u64, Utf8View, bool) noexcept {
    return false;
}
bool application_quick_switcher_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool dismiss_popup() noexcept {
    return false;
}
bool open_message_context_popup(u64, UiRect) noexcept {
    return false;
}
bool application_invite_card_click(u64, UiWidgetHandle, float, float) noexcept {
    return false;
}
bool application_message_component_click(u64, UiWidgetHandle, UiRect) noexcept {
    return false;
}
bool application_pinned_messages_toggle() noexcept {
    return true;
}
bool refresh_account_frame(bool) noexcept {
    return true;
}
void application_user_settings_request_content_confirmation() noexcept {}
bool application_voice_join(u64, u64) noexcept {
    return true;
}
bool application_guild_invite_open_url(Utf8View) noexcept {
    return false;
}
bool application_link_open(Utf8View, bool) noexcept {
    return true;
}
bool application_link_channel_trusted(u64) noexcept {
    return true;
}
bool application_guild_folders_toggle(u64) noexcept {
    return false;
}
bool application_navigation_drag_press(LPARAM) noexcept {
    return false;
}
bool application_guild_lifecycle_open_create() noexcept {
    return false;
}
bool application_guild_community_open_events(u64) noexcept {
    return false;
}
} // namespace maicord::app_detail

extern "C" __declspec(noreturn) void WINAPI maicord_message_link_ui_probe_entry() noexcept {
    ExitProcess(maicord::message_link_ui_probe() ? 0 : 1);
}
