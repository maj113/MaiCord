#include "scoped_read_ui_probe.hpp"
#include "probe_present.hpp"
#include "../../tools/perf/probe_window.hpp"
#include "probe_glyph_ink.hpp"
#include "probe_capture.hpp"
#include "application_state.hpp"
#include "application_scoped_read.hpp"
#include "application_scoped_read_menu.hpp"
#include "application_read_state.hpp"
#include "application_plugins.hpp"
#include "application_message_interaction.hpp"
#include "application_account_presentation.hpp"
#include "application_account_logout.hpp"
#include "application_search.hpp"
#include "application_pinned_messages.hpp"
#include "application_thread_browser.hpp"
#include "../application_downloads_inactive.hpp"
#include "application_message_mutation_notice.hpp"
#include "application_composer.hpp"
#include "application_guild_menu.hpp"
#include "application_guild_settings.hpp"
#include "application_guild_lifecycle.hpp"
#include "application_profile_editor.hpp"
#include "application_notification_controls.hpp"
#include "application_member_moderation.hpp"
#include "application_settings.hpp"
#include "application_guild_community.hpp"
#include "application_scrolling.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/read_state.hpp"
#include "maicord/protocol/bulk_message_ack.hpp"
#include "maicord/protocol/message_ack.hpp"
#include "maicord/app/navigation_shortcuts.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/mention_badge.hpp"
#include "maicord/render/renderer_presentation.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"
#include "../../src/render/d3d11_renderer_layout.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord::app_detail {
constinit Application g_app{};
}
namespace maicord::scoped_read_native {
using namespace app_detail;
constexpr u64 kGuild = 94001, kOtherGuild = 94002, kCategory = 94100;
constexpr u64 kSelected = 94101, kParent = 94200, kThread = 94300, kOther = 94500;
constexpr u64 kRetryCategory = 94110, kRetryParent = 94600, kRetryThread = 94601, kRetryChild = 94602;
constexpr u32 kChannels = 112, kVisible = 16;
struct Fixture {
    Theme theme;
    TextHitSpan hit_spans[kTextHitSpanMaximum];
    TextHitRegion hit_scratch[kRendererContentHitRegionCapacity];
    TextDecoration decorations[kRendererTextDecorationCapacity];
    GuildRailRowWidget rail_rows[3];
    UiWidget rail_widgets[8];
    UiLayoutNode rail_nodes[8];
    ShellNavigationRowWidget navigation_rows[kVisible];
    UiWidget navigation_widgets[kVisible * 8 + 1];
    UiLayoutNode navigation_nodes[kVisible * 8 + 1];
    PopupItem popup_items[24];
    PopupRowWidget popup_rows[24];
    UiWidget popup_widgets[64];
    UiLayoutNode popup_nodes[64];
    u16 channel_scratch[kChannels];
    u8 visibility[kChannels];
    ChannelSortKey sort_keys[kChannels];
    ReadStateEntry read_pending[kChannels];
    MessageSceneRowWidget message_rows[1];
    UiWidget message_widgets[kMessageSceneWidgetsPerRow + 1];
    UiLayoutNode message_nodes[kMessageSceneWidgetsPerRow + 1];
    u32 message_byte_styles[4];
    BulkMessageAckEntry admitted[kBulkMessageAckMaximumEntries];
    u8 wire[kBulkMessageAckMaximumBodyBytes];
    u64 now, request_id, individual_channel, individual_cursor;
    u32 admitted_count, bulk_posts, individual_posts, encoded, frames, captures, failures;
    u32 navigation_cancels, bottom_scrolls;
    // Server menu stage: recorded feature entry points and role storage.
    RoleRecord roles[4];
    PermissionOverwriteRecord overwrites[2];
    u64 settings_guild;
    u32 settings_opens, invites, creations, privacy_opens, profile_opens, leaves;
    u64 profile_guild;
    // Synced user guild settings writes: hide_muted_channels and opt-in.
    u32 hide_writes, opt_in_writes;
    u64 privacy_guild;
    bool hide_written, opt_in_written, opt_in, community;
    u8 settings_page, creation_type;
    bool header_pressed;
    const wchar_t* frame;
    bool refresh_ok;
};
Fixture* fixture;
u32 diagnostics;
bool fail_at(u32 line, u32 stage = 0) noexcept {
    // CTest inherits console pipes even for this WINDOWS-subsystem executable.
    // Failure reporting must not depend on the application log or disk output.
    if (diagnostics++ >= 8)
        return false;
    char message[384]{};
    u32 count{};
    const auto append = [&](const char* text) noexcept {
        for (u32 i = 0; text[i] && count + 1 < sizeof(message); ++i)
            message[count++] = text[i];
    };
    const auto number = [&](u64 value) noexcept {
        char digits[20];
        u32 used{};
        do {
            digits[used++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value && used < sizeof(digits));
        while (used && count + 1 < sizeof(message))
            message[count++] = digits[--used];
    };
    append("FAIL scoped_read_ui_probe.cpp:");
    number(line);
    append(" stage=");
    number(stage);
    append(" win32=");
    number(GetLastError());
    if (fixture) {
        if (fixture->frame) {
            append(" frame=");
            for (u32 i = 0; fixture->frame[i] && i < 80 && count + 1 < sizeof(message); ++i)
                message[count++] = static_cast<char>(fixture->frame[i]);
        }
        append(" frames=");
        number(fixture->captures);
        append(" bulk=");
        number(fixture->bulk_posts);
        append(" individual=");
        number(fixture->individual_posts);
        append(" request=");
        number(fixture->request_id);
    }
    if (g_app.account_ui) {
        constexpr u64 channels[]{kParent, kThread};
        for (u64 id : channels) {
            const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, id);
            if (!channel)
                continue;
            append(" channel=");
            number(id);
            append(" cursor=");
            number(channel->last_read_message_id);
            append(" mentions=");
            number(channel->mention_count);
        }
    }
    append("\r\n");
    DWORD written{};
    if (!WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, count, &written, nullptr))
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, count, &written, nullptr);
    return false;
}
bool require_at(bool valid, u32 stage, u32 line) noexcept {
    if (!valid) {
        if (fixture)
            ++fixture->failures;
        log_u64(LogLevel::error, "scoped_read_native", "failure_stage", stage);
        return fail_at(line, stage);
    }
    return valid;
}
#define SCOPED_READ_REQUIRE(...) require_at(__VA_ARGS__, __LINE__)
bool project() noexcept;
RestClientSubmitResult WINAPI individual(void* context, u64 channel_id, u64 cursor,
                                         ConstSlice<u8> body, u64 request_id) noexcept {
    auto& f = *fixture;
    if (!SCOPED_READ_REQUIRE(context == fixture && channel_id && cursor && body.data && body.count &&
                                 body.count <= sizeof(f.wire) && request_id && !f.request_id,
                             122))
        return {RestClientSubmitError::invalid_input};
    u8 target[kMessageAckMaximumTargetBytes];
    if (!SCOPED_READ_REQUIRE(
            write_message_ack_target(channel_id, cursor, {target, sizeof(target)}).ok(), 123))
        return {RestClientSubmitError::target_capacity};
    copy_bytes(f.wire, body.data, body.count);
    f.individual_channel = channel_id;
    f.individual_cursor = cursor;
    f.request_id = request_id;
    ++f.individual_posts;
    ++f.encoded;
    return {};
}
RestClientSubmitResult WINAPI bulk(void* context, const BulkMessageAckEntry* entries,
                                   u32 count, u64 request_id) noexcept {
    auto& f = *fixture;
    if (!SCOPED_READ_REQUIRE(context == fixture && entries && count &&
                                 count <= kBulkMessageAckMaximumEntries && request_id && !f.request_id,
                             120))
        return {RestClientSubmitError::invalid_input};
    const auto written = write_bulk_message_ack_body({entries, count}, {f.wire, sizeof(f.wire)});
    if (!SCOPED_READ_REQUIRE(written.ok(), 121))
        return {RestClientSubmitError::invalid_input};
    for (u32 i = 0; i < count; ++i)
        f.admitted[i] = entries[i];
    f.admitted_count = count;
    f.request_id = request_id;
    ++f.bulk_posts;
    ++f.encoded;
    log_u64(LogLevel::info, "scoped_read_transport", "bulk_entries", count);
    log_u64(LogLevel::info, "scoped_read_transport", "request_id", request_id);
    log_u64(LogLevel::info, "scoped_read_transport", "at_ms", f.now);
    return {};
}
bool channel(u64 id, Utf8View name, u8 type, u64 parent, u32 position, u64 last, u64 cursor,
             u32 mentions) noexcept {
    auto& state = g_app.account_ui->chat_state;
    ChannelInput input{};
    input.id = id;
    input.guild_id = id == kOther ? kOtherGuild : kGuild;
    input.parent_id = parent;
    input.name = name;
    input.type = type;
    input.position = position;
    input.last_message_id = last;
    input.last_message_known = true;
    if (type == 11) {
        input.thread_archived_known = input.thread_self_joined_known = input.thread_active_known = true;
        input.thread_archived = false;
        input.thread_self_joined = input.thread_listed_active = true;
    }
    if (!chat_state_store_upsert_channel(&state, input).ok())
        return fail_at(__LINE__);
    auto* record = const_cast<ChannelRecord*>(chat_state_store_find_channel(state, id));
    if (!record)
        return fail_at(__LINE__);
    record->permissions_resolved = true;
    record->effective_permissions = kPermissionViewChannel | kPermissionReadMessageHistory;
    record->actions = channel_action_view | channel_action_read_history;
    if (type == 4)
        return true;
    ReadStateEntry entry{};
    entry.channel_id = id;
    entry.message_id = cursor;
    entry.mention_count = mentions;
    entry.fields = read_state_cursor | read_state_mentions;
    return read_state_upsert(&state, entry, 1);
}
bool layout_shell() noexcept {
    const auto& r = g_app.renderer;
    auto metrics = kGuildChannelShellLayoutMetrics;
    metrics.flags &= static_cast<u16>(~(ui_shell_layout_members | ui_shell_layout_composer));
    metrics.member_sidebar_width = 0;
    return SCOPED_READ_REQUIRE(ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::guild_channel,
                                                            metrics, r.width * 96.f / r.dpi,
                                                            r.height * 96.f / r.dpi),
                               359);
}
bool account_storage() noexcept {
    auto& f = *fixture;
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.account_ui)
        return fail_at(__LINE__);
    auto& ui = *g_app.account_ui;
    if (!chat_state_store_initialize(&ui.chat_state, {ui.guild_record_storage, 2},
                                        {ui.guild_order_storage, 2}, {ui.channel_record_storage, kChannels},
                                        {ui.channel_order_storage, kChannels}, {ui.chat_state_name_storage, 8192}) ||
        !read_state_initialize(&ui.read_state_store, {f.read_pending, kChannels}) ||
        !read_state_bind(&ui.chat_state, &ui.read_state_store) ||
        !user_store_initialize(&ui.user_store, {ui.user_record_storage, 2},
                                       {ui.user_order_storage, 2}, {ui.user_name_storage, 512}) ||
        !user_store_upsert(&ui.user_store, 99, utf8_literal("Alex"), utf8_literal("alex"))
             .ok() ||
        !navigation_view_initialize(
            &ui.navigation_view, {ui.navigation_guild_rows, 2}, {ui.navigation_channel_rows, kChannels},
            {f.channel_scratch, kChannels}, {f.visibility, kChannels}, {f.sort_keys, kChannels}) ||
        !ui_text_measure_cache_initialize(&ui.text_measure_cache,
                                          {ui.message_text_measure_cache_records, 128},
                                          renderer_text_measure_backend, &g_app.renderer) ||
        !message_view_runtime_initialize(&g_app.message_view, ui_extent_units(80), 1, false).ok() ||
        !message_scene_initialize(&g_app.message_scene, &g_app.message_view.timeline, {f.message_rows, 1},
                                  {f.message_widgets, kMessageSceneWidgetsPerRow + 1},
                                  {f.message_nodes, kMessageSceneWidgetsPerRow + 1}))
        return fail_at(__LINE__);
    ui.chat_state.self_user_id = 99;
    if (!chat_state_store_upsert_guild(&ui.chat_state, {kGuild, utf8_literal("Read scope QA"),
                                                              GuildMutationKind::create, 99})
             .ok() ||
        !chat_state_store_upsert_guild(&ui.chat_state, {kOtherGuild, utf8_literal("Other server"),
                                                              GuildMutationKind::create, 99})
             .ok() ||
        !channel(kCategory, utf8_literal("COLLAPSED PROJECTS"), 4, 0, 1, 0, 0, 0) ||
        !channel(kSelected, utf8_literal("empty selected channel"), 0, 0, 0, 0, 0, 0) ||
        !channel(kParent, utf8_literal("project-updates"), 0, kCategory, 0, 2000000, 1999900, 2) ||
        !channel(kThread, utf8_literal("joined project thread"), 11, kParent, 0, 3000000, 2999900, 1) ||
        !channel(kOther, utf8_literal("outside this server"), 0, 0, 0, 4000000, 3999900, 4) ||
        !channel(kRetryCategory, utf8_literal("REVIEW QUEUE"), 4, 0, 2, 0, 0, 0) ||
        !channel(kRetryParent, utf8_literal("review-updates"), 0, kRetryCategory, 0, 0, 0, 0) ||
        !channel(kRetryThread, utf8_literal("joined review thread"), 11, kRetryParent, 0, 0, 0, 0) ||
        !channel(kRetryChild, utf8_literal("release-notes"), 0, kRetryCategory, 1, 0, 0, 0))
        return fail_at(__LINE__);
    // Exactly 101 outbound guild targets: 99 ordinary children plus parent/thread.
    // Empty channels have local-only targets; the unrelated guild is excluded.
    // Muted, so the collapsed category folds them away; a mention keeps the first.
    for (u32 i = 0; i < 99; ++i)
        if (!channel(94400 + i, utf8_literal("retained project channel"), 0, kCategory, i + 1,
                     5000000 + i * 1000, 4999900 + i * 1000, i == 0 ? 1200 : 0) ||
            !chat_state_store_set_channel_mute(&ui.chat_state, 94400 + i, true, true))
            return fail_at(__LINE__);
    const_cast<ChannelRecord*>(chat_state_store_find_channel(ui.chat_state, kCategory))
        ->collapsed = true;
    constexpr u64 guild_ids[]{kGuild, kOtherGuild};
    for (u64 id : guild_ids) {
        auto* guild = const_cast<GuildRecord*>(chat_state_store_find_guild(ui.chat_state, id));
        guild->permissions_resolved = true;
        guild->base_permissions = kPermissionViewChannel | kPermissionReadMessageHistory;
    }
    if (!chat_state_store_select_channel(&ui.chat_state, kSelected).ok())
        return fail_at(__LINE__);
    ui.active_channel_id = kSelected;
    auto& text = ui.renderer_text;
    text.quads = ui.renderer_text_quads;
    text.batches = ui.renderer_text_batches;
    text.utf16 = ui.response_shape_scratch.shape.utf16;
    text.glyphs = ui.response_shape_scratch.shape.glyphs;
    text.runs = ui.response_shape_scratch.shape.runs;
    text.clusters = ui.response_shape_scratch.shape.clusters;
    text.hit_spans = f.hit_spans;
    text.hit_region_scratch = f.hit_scratch;
    text.decorations = f.decorations;
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
    renderer_bind_navigation_view(&g_app.renderer, &ui.chat_state, &ui.user_store, &ui.navigation_view);
    g_app.renderer.message_measure_cache = &ui.text_measure_cache;
    g_app.account.instance = fixture;
    g_app.account.status.profile_id = 42;
    g_app.plugin_account_scope_serial = 1;
    g_app.login.visible = false;
    g_app.account.api.bulk_message_ack = bulk;
    g_app.account.api.message_ack = individual;
    // Bootstrap reserves storage only. Materialize the native root before
    // binding it, using the same layout policy as every later projection.
    if (!layout_shell())
        return fail_at(__LINE__);
    // A visible empty timeline still has an active native MessageScene. Bind
    // its real empty presentation so the shipping renderer can validate it.
    const bool bound =
        SCOPED_READ_REQUIRE(message_text_presentation_initialize(
                                &ui.message_text, {ui.message_text_commands, 1}, {ui.message_text_utf8, 1},
                                {ui.message_text_styles, 1}, {ui.message_text_semantics, 1},
                                {ui.message_text_measurements, 1}, {ui.message_text_spoiler_reveals, 1},
                                {f.message_byte_styles, 4}),
                            358) &&
        SCOPED_READ_REQUIRE(renderer_bind_ui_scene(&g_app.renderer, &g_app.shell_scene), 350) &&
        SCOPED_READ_REQUIRE(renderer_bind_guild_rail_scene(&g_app.renderer, &g_app.guild_rail_scene),
                            351) &&
        SCOPED_READ_REQUIRE(
            renderer_bind_shell_navigation_scene(&g_app.renderer, &g_app.shell_navigation_scene), 352) &&
        SCOPED_READ_REQUIRE(renderer_bind_message_scene(&g_app.renderer, &g_app.message_scene), 353) &&
        SCOPED_READ_REQUIRE(renderer_bind_text_storage(&g_app.renderer, &text), 354) &&
        SCOPED_READ_REQUIRE(renderer_bind_message_text_presentation(&g_app.renderer, &ui.message_text),
                            355);
    if (bound)
        g_app.renderer.message_measure_cache = &ui.text_measure_cache;
    return bound;
}
} // namespace maicord::scoped_read_native

namespace maicord::scoped_read_native {
bool solid_pixels(const D3D11_MAPPED_SUBRESOURCE& mapped, UiRect region, Color wanted) noexcept {
    const auto& r = g_app.renderer;
    const float scale = r.dpi / 96.f;
    const i32 colors[]{static_cast<i32>(wanted.blue * 255 + .5f),
                       static_cast<i32>(wanted.green * 255 + .5f),
                       static_cast<i32>(wanted.red * 255 + .5f)};
    u32 pixels{};
    for (i32 y = maximum(0, static_cast<i32>(region.top * scale));
         y < minimum(static_cast<i32>(r.height), static_cast<i32>(region.bottom * scale)); ++y)
        for (i32 x = maximum(0, static_cast<i32>(region.left * scale));
             x < minimum(static_cast<i32>(r.width), static_cast<i32>(region.right * scale)); ++x) {
            const auto* pixel = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4;
            i32 distance{};
            for (u32 c = 0; c < 3; ++c) {
                const i32 difference = pixel[c] - colors[c];
                distance += difference < 0 ? -difference : difference;
            }
            if (distance <= 36 && ++pixels >= 16)
                return true;
        }
    return false;
}

bool visual(const D3D11_MAPPED_SUBRESOURCE& mapped) noexcept {
    auto& r = g_app.renderer;
    auto& p = g_app.popup_scene;
    if (!SCOPED_READ_REQUIRE(r.content_text && r.content_text->quad_count, 200))
        return fail_at(__LINE__);
    const auto& rail = g_app.guild_rail_scene;
    const auto navigation = g_app.shell_navigation_scene.viewport;
    bool selected_indicator{}, badge_ink{};
    const auto rail_rectangle = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::guild_rail);
    for (u32 i = 0; i < rail.count; ++i) {
        const auto& row = rail.rows[i];
        const auto* node = guild_rail_scene_layout_node(rail, row);
        if (!node)
            return SCOPED_READ_REQUIRE(false, 205);
        if (row.view.selected) {
            const float center = (node->rectangle.top + node->rectangle.bottom) * .5f;
            // The selected guild indicator is native paint geometry, not a
            // text glyph. Its inner strip remains visible beside context menus.
            selected_indicator =
                solid_pixels(mapped, {rail_rectangle.left, center - 8, rail_rectangle.left + 3, center + 8},
                             fixture->theme.text_primary);
        }
        if (!p.active && row.view.mention_count) {
            const auto badge = ui_rect_intersection(
                ui_mention_badge_guild_rectangle(node->rectangle, row.view.mention_count), rail.viewport);
            // Numeric rail badges are authored opaque white on the red badge
            // in both themes. Popup-covered glyphs cannot establish ink proof.
            badge_ink = badge_ink || probe_ink(mapped, badge, {1, 1, 1, 1});
        }
    }
    if (!SCOPED_READ_REQUIRE(selected_indicator, 201) || !SCOPED_READ_REQUIRE(p.active || badge_ink, 206) ||
        !SCOPED_READ_REQUIRE(probe_ink(mapped, navigation, fixture->theme.text_primary), 207))
        return fail_at(__LINE__);
    if (!p.active)
        return true;
    const auto viewport = UiRect{0, 0, r.width * 96.f / r.dpi, r.height * 96.f / r.dpi};
    if (!SCOPED_READ_REQUIRE(p.item_count && ui_rect_has_area(p.rectangle) &&
                                 p.rectangle.left >= viewport.left && p.rectangle.top >= viewport.top &&
                                 p.rectangle.right <= viewport.right &&
                                 p.rectangle.bottom <= viewport.bottom,
                             202))
        return fail_at(__LINE__);
    bool label_ink = false;
    for (u32 i = 0; i < p.item_count; ++i) {
        PopupItemLayout layout{};
        if (!popup_scene_resolve_item_layout(p, i, &layout) || !layout.visible)
            continue;
        const auto color = p.items[i].flags & popup_item_enabled ? fixture->theme.text_primary
                                                                 : fixture->theme.text_secondary;
        label_ink = label_ink || probe_ink(mapped, layout.visible_rectangle, color);
    }
    if (!SCOPED_READ_REQUIRE(label_ink, 203))
        return fail_at(__LINE__);
    if (p.feedback.text.bytes &&
        !SCOPED_READ_REQUIRE(
            probe_ink(mapped, popup_feedback_rectangle(p),
                ui_foreground_color(fixture->theme, p.feedback.error ? fixture->theme.notification
                                                                     : fixture->theme.text_secondary)),
            204))
        return fail_at(__LINE__);
    return true;
}
bool capture(const wchar_t* filename) noexcept {
    fixture->frame = filename;
    if (!project())
        return SCOPED_READ_REQUIRE(false, 230);
    auto& r = g_app.renderer;
    if (!d3d11_renderer_paint_layout(&r, fixture->theme,
                                     renderer_capture_presentation_stamp(r, fixture->theme)) ||
        !d3d11_renderer_draw_stream(&r, fixture->theme, true, true, nullptr, 10000))
        return SCOPED_READ_REQUIRE(false, 231);
    ProbeTargetReadback readback;
    if (const u32 failure = probe_target_readback(r, &readback))
        return SCOPED_READ_REQUIRE(false, 231 + failure);
    const auto& mapped = readback.mapped;
    if (!visual(mapped))
        return fail_at(__LINE__);
    // Verify the current native frame in memory; never write capture artifacts.
    ++fixture->captures;
    return true;
}

struct WarmSnapshot {
    u32 values[19];
    ApplicationScopedReadMetrics owner;
    ApplicationReadStateMetrics broker;
};
WarmSnapshot snapshot() noexcept {
    const auto& r = g_app.renderer;
    const auto m = application_scoped_read_metrics();
    const auto b = application_read_state_metrics();
    return {{g_app.shell_scene.layout_passes, g_app.guild_rail_scene.layout_passes,
             g_app.shell_navigation_scene.layout_passes, g_app.popup_scene.revision,
             r.content_text->rebuilds, r.glyph_text.cache_builds, r.paint_stream_builds,
             r.paint_instance_uploads, r.text_instance_uploads, r.media_instance_uploads, r.painted_frames,
             fixture->frames, fixture->encoded, g_app.account_ui->text_measure_cache.backend_calls,
             m.plan_builds, m.target_checks, m.encodings, m.commits, b.commits},
            m,
            b};
}
bool storage_equal(const ApplicationScopedReadMetrics& a, const ApplicationScopedReadMetrics& b) noexcept {
    return a.owner_bytes == b.owner_bytes && a.owner_reserved_bytes == b.owner_reserved_bytes &&
           a.owner_committed_bytes == b.owner_committed_bytes &&
           a.queue_reserved_bytes == b.queue_reserved_bytes &&
           a.queue_committed_bytes == b.queue_committed_bytes &&
           a.targets_reserved_bytes == b.targets_reserved_bytes &&
           a.targets_committed_bytes == b.targets_committed_bytes &&
           a.plan_reserved_bytes == b.plan_reserved_bytes &&
           a.plan_committed_bytes == b.plan_committed_bytes &&
           a.jobs_reserved_bytes == b.jobs_reserved_bytes &&
           a.jobs_committed_bytes == b.jobs_committed_bytes &&
           a.flight_reserved_bytes == b.flight_reserved_bytes &&
           a.flight_committed_bytes == b.flight_committed_bytes &&
           a.candidate_reserved_bytes == b.candidate_reserved_bytes &&
           a.candidate_committed_bytes == b.candidate_committed_bytes &&
           a.publication_revision == b.publication_revision && a.flight_active == b.flight_active;
}
bool broker_equal(const ApplicationReadStateMetrics& a, const ApplicationReadStateMetrics& b) noexcept {
    return a.owner_bytes == b.owner_bytes && a.owner_reserved_bytes == b.owner_reserved_bytes &&
           a.owner_committed_bytes == b.owner_committed_bytes &&
           a.intents_reserved_bytes == b.intents_reserved_bytes &&
           a.intents_committed_bytes == b.intents_committed_bytes &&
           a.token_reserved_bytes == b.token_reserved_bytes &&
           a.token_committed_bytes == b.token_committed_bytes &&
           a.actions_reserved_bytes == b.actions_reserved_bytes &&
           a.actions_committed_bytes == b.actions_committed_bytes &&
           a.echoes_reserved_bytes == b.echoes_reserved_bytes &&
           a.echoes_committed_bytes == b.echoes_committed_bytes && a.action_serial == b.action_serial;
}
bool warm(u32 sample) noexcept {
    for (u32 i = 0; i < 4; ++i)
        if (!project() || !probe_present(&g_app.renderer, fixture->theme))
            return fail_at(__LINE__);
    const auto before = snapshot();
    const u64 scoped_wake = application_scoped_read_next_wake();
    const u64 read_wake = application_read_state_next_wake();
    auto& r = g_app.renderer;
    const auto skips = r.unchanged_present_skips;
    for (u32 i = 0; i < 64; ++i)
        if (!project() || !probe_present(&r, fixture->theme) ||
            application_scoped_read_next_wake() != scoped_wake ||
            application_read_state_next_wake() != read_wake)
            return fail_at(__LINE__);
    const auto after = snapshot();
    constexpr const char* names[]{
        "shell_layout_delta",   "rail_layout_delta",  "sidebar_layout_delta", "popup_revision_delta",
        "text_build_delta",     "glyph_cache_delta",  "paint_build_delta",    "paint_upload_delta",
        "text_upload_delta",    "media_upload_delta", "painted_delta",        "frame_request_delta",
        "encoding_delta",       "measure_delta",      "plan_build_delta",     "target_checks_delta",
        "scope_encoding_delta", "scope_commit_delta", "broker_commit_delta"};
    log_u64(LogLevel::info, "scoped_read_warm", "sample", sample);
    bool valid = r.unchanged_present_skips - skips == 64 && storage_equal(before.owner, after.owner) &&
                 broker_equal(before.broker, after.broker);
    for (u32 i = 0; i < 19; ++i) {
        log_u64(LogLevel::info, "scoped_read_warm", names[i], after.values[i] - before.values[i]);
        valid = valid && before.values[i] == after.values[i];
    }
    log_u64(LogLevel::info, "scoped_read_warm", "unchanged_skips", r.unchanged_present_skips - skips);
    log_u64(LogLevel::info, "scoped_read_warm", "owner_publication_delta",
            after.owner.publication_revision - before.owner.publication_revision);
    log_u64(LogLevel::info, "scoped_read_warm", "source_action_serial_changed",
            after.broker.action_serial != before.broker.action_serial);
    return SCOPED_READ_REQUIRE(valid, 250 + sample);
}
bool project() noexcept {
    auto& ui = *g_app.account_ui;
    auto& r = g_app.renderer;
    if (!navigation_view_rebuild(&ui.navigation_view, ui.chat_state).ok() || !layout_shell())
        return fail_at(__LINE__);
    const auto rail = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::guild_navigation);
    const auto channels = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::channel_navigation);
    const auto messages = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::message_view);
    if (!guild_rail_scene_update(&g_app.guild_rail_scene, &ui.navigation_view, rail, 0).ok() ||
        !shell_navigation_scene_update(&g_app.shell_navigation_scene, &ui.navigation_view, channels, 0)
             .ok() ||
        !message_scene_update(&g_app.message_scene, messages).ok())
        return fail_at(__LINE__);
    if (!ui.message_text.active &&
        !SCOPED_READ_REQUIRE(message_text_presentation_build(&ui.message_text, g_app.message_scene,
                                                             ui.chat_state, &ui.user_store,
                                                             ui.navigation_view, fixture->theme, 0)
                                 .ok(),
                             360))
        return fail_at(__LINE__);
    if (!SCOPED_READ_REQUIRE(g_app.message_scene.active && !g_app.message_scene.count &&
                                 g_app.message_scene.timeline && g_app.message_scene.timeline->list &&
                                 !g_app.message_scene.timeline->list->count && ui.message_text.active &&
                                 ui.message_text.builds == 1 && !ui.message_text.command_count &&
                                 r.message_text == &ui.message_text,
                             361))
        return fail_at(__LINE__);
    application_scoped_read_synchronize();
    if (!application_scoped_read_menu_refresh())
        return fail_at(__LINE__);
    renderer_bind_focus_owner(&r, g_app.popup_scene.active       ? &g_app.popup_scene.widgets
                                  : g_app.keyboard_focus_visible ? g_app.context_focus
                                                                 : nullptr);
    return SCOPED_READ_REQUIRE(renderer_rebuild_content_text(&r, fixture->theme), 362);
}
UiRect row_rectangle(ReadScopeKind kind, u64 id) noexcept {
    if (kind == ReadScopeKind::guild) {
        const auto& scene = g_app.guild_rail_scene;
        for (u32 i = 0; i < scene.count; ++i)
            if (scene.rows[i].view.id == id) {
                const auto* node = guild_rail_scene_layout_node(scene, scene.rows[i]);
                return node ? ui_rect_intersection(node->rectangle, node->clip_rectangle) : UiRect{};
            }
    } else {
        const auto& scene = g_app.shell_navigation_scene;
        for (u32 i = 0; i < scene.count; ++i)
            if (scene.rows[i].view.id == id) {
                const auto* node = shell_navigation_scene_layout_node(scene, scene.rows[i]);
                return node ? ui_rect_intersection(node->rectangle, node->clip_rectangle) : UiRect{};
            }
    }
    return {};
}
LPARAM center(UiRect rectangle) noexcept {
    const float scale = g_app.renderer.dpi / 96.f;
    return MAKELPARAM(static_cast<u16>((rectangle.left + rectangle.right) * .5f * scale),
                      static_cast<u16>((rectangle.top + rectangle.bottom) * .5f * scale));
}
// This is a test window's native message route. Production policy, popup,
// focus, hit testing, ownership and activation run unchanged. It is not an
// assertion that operating-system injection or the application's global
// dispatcher was exercised.
LRESULT CALLBACK window_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (!fixture || !g_app.renderer_ready)
        return DefWindowProcW(window, message, wparam, lparam);
    const float scale = 96.f / g_app.renderer.dpi;
    const float x = static_cast<short>(LOWORD(lparam)) * scale;
    const float y = static_cast<short>(HIWORD(lparam)) * scale;
    if (message == WM_RBUTTONUP) {
        const auto guild = guild_rail_scene_hit_test(g_app.guild_rail_scene, x, y);
        if (guild.id)
            return application_scoped_read_menu_open(ReadScopeKind::guild, guild.id, {x, y, x + 1, y + 1});
        const auto category = shell_navigation_scene_hit_test(g_app.shell_navigation_scene, x, y);
        if (category.id)
            return application_scoped_read_menu_open(ReadScopeKind::category, category.id,
                                                     {x, y, x + 1, y + 1});
    }
    if (message == WM_KEYDOWN) {
        if (wparam == VK_APPS) {
            const bool guild = g_app.context_focus == &g_app.guild_rail_scene.widgets;
            const auto kind = guild ? ReadScopeKind::guild : ReadScopeKind::category;
            const auto id = guild ? g_app.guild_rail_scene.focused_guild_id
                                  : g_app.shell_navigation_scene.focused_channel_id;
            return application_scoped_read_menu_open(kind, id, row_rectangle(kind, id));
        }
        if (g_app.popup_scene.active) {
            if (wparam == VK_ESCAPE)
                return dismiss_popup();
            if (wparam == VK_UP || wparam == VK_DOWN) {
                bool changed{};
                return popup_scene_move_selection(&g_app.popup_scene, wparam == VK_UP ? -1 : 1, &changed) &&
                       (!changed || project());
            }
            if (wparam == VK_RETURN) {
                const u32 selected = popup_scene_selected_action(g_app.popup_scene);
                return application_guild_menu_action(selected) ? application_guild_menu_activate(selected)
                                                               : application_scoped_read_menu_activate(selected);
            }
        }
    }
    if (message == WM_MOUSEMOVE && g_app.popup_scene.active) {
        bool changed{};
        return popup_scene_set_hover(&g_app.popup_scene, x, y, &changed) && (!changed || project());
    }
    // Production order: the popup consumes a press (outside closes it); a
    // shell button activates on the release of its own press.
    if (message == WM_LBUTTONDOWN && g_app.popup_scene.active) {
        const auto hit = popup_scene_hit_test(g_app.popup_scene, x, y);
        if (!hit.inside)
            return dismiss_popup();
        if (!hit.enabled)
            return true;
        return application_guild_menu_action(hit.action) ? application_guild_menu_activate(hit.action)
                                                         : application_scoped_read_menu_activate(hit.action);
    }
    const auto* shell = ui_widget_manager_resolve(g_app.shell_scene.widgets,
                                                  ui_widget_manager_hit_test(g_app.shell_scene.widgets, x, y));
    const bool header = shell && shell->id == static_cast<u32>(UiSemantic::guild_menu) + 1u;
    if (message == WM_LBUTTONDOWN) {
        fixture->header_pressed = header;
        return true;
    }
    if (message == WM_LBUTTONUP) {
        const bool activate = header && fixture->header_pressed;
        fixture->header_pressed = false;
        return !activate || application_guild_menu_toggle(false);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
bool menu(ReadScopeKind kind, u64 id, bool keyboard) noexcept {
    dismiss_popup();
    if (!project())
        return fail_at(__LINE__);
    const auto rectangle = row_rectangle(kind, id);
    if (!SCOPED_READ_REQUIRE(ui_rect_has_area(rectangle), 260))
        return fail_at(__LINE__);
    if (keyboard) {
        if (kind == ReadScopeKind::guild) {
            guild_rail_scene_focus_guild(&g_app.guild_rail_scene, id);
            g_app.context_focus = &g_app.guild_rail_scene.widgets;
        } else {
            const auto& view = g_app.account_ui->navigation_view;
            auto target = navigation_focus_target(view, false, 0, UiListMove::first);
            // Follow the same pure focus policy used by F6/Tab/arrows, then
            // reveal its genuine native row. Category context remains distinct
            // from Alt/unread/mention conversation navigation.
            for (u32 i = 0; target.id && target.id != id && i < view.channel_count; ++i)
                target = navigation_focus_target(view, false, target.id, UiListMove::next);
            if (!SCOPED_READ_REQUIRE(
                    target.id == id && !target.guild && !target.home &&
                        shell_navigation_scene_reveal_channel(&g_app.shell_navigation_scene, view, id),
                    262))
                return fail_at(__LINE__);
            g_app.context_focus = &g_app.shell_navigation_scene.widgets;
        }
        g_app.keyboard_focus_visible = true;
        SendMessageW(g_app.window, WM_KEYDOWN, VK_APPS, 0);
    } else {
        g_app.keyboard_focus_visible = false;
        SendMessageW(g_app.window, WM_RBUTTONUP, 0, center(rectangle));
    }
    return SCOPED_READ_REQUIRE(g_app.popup_scene.active && g_app.popup_scene.owner_id == id &&
                                   g_app.popup_scene.feedback_reserved,
                               261) &&
           project();
}
bool action(u32 action_id, bool keyboard) noexcept {
    auto& popup = g_app.popup_scene;
    for (u32 i = 0; i < popup.item_count; ++i) {
        if (popup.items[i].action != action_id || !(popup.items[i].flags & popup_item_enabled))
            continue;
        bool changed{};
        if (!popup_scene_select_index(&popup, i, &changed))
            return fail_at(__LINE__);
        PopupItemLayout layout{};
        if (!popup_scene_resolve_item_layout(popup, i, &layout) || !layout.visible)
            return fail_at(__LINE__);
        if (keyboard)
            SendMessageW(g_app.window, WM_KEYDOWN, VK_RETURN, 0);
        else {
            SendMessageW(g_app.window, WM_MOUSEMOVE, 0, center(layout.visible_rectangle));
            SendMessageW(g_app.window, WM_LBUTTONDOWN, MK_LBUTTON, center(layout.visible_rectangle));
            SendMessageW(g_app.window, WM_LBUTTONUP, 0, center(layout.visible_rectangle));
        }
        return project();
    }
    return fail_at(__LINE__);
}
} // namespace maicord::scoped_read_native

namespace maicord::app_detail {
using namespace scoped_read_native;
bool application_request_frame() noexcept {
    ++fixture->frames;
    return true;
}
const Theme& application_theme() noexcept {
    return fixture->theme;
}
UiRect application_viewport_dips() noexcept {
    return {0, 0, g_app.renderer.width * 96.f / g_app.renderer.dpi,
            g_app.renderer.height * 96.f / g_app.renderer.dpi};
}
bool refresh_account_text(bool) noexcept {
    return fixture->refresh_ok = fixture->refresh_ok && project();
}
bool refresh_account_frame(bool) noexcept {
    return refresh_account_text(true);
}
bool refresh_popup_text() noexcept {
    renderer_bind_focus_owner(&g_app.renderer, &g_app.popup_scene.widgets);
    return renderer_rebuild_content_text(&g_app.renderer, fixture->theme) && application_request_frame();
}
bool synchronize_navigation_view() noexcept {
    return project();
}
bool refresh_message_composer_ui() noexcept {
    return true;
}
bool application_account_logout_blocked() noexcept {
    return false;
}
bool application_search_read_blocked() noexcept {
    return false;
}
bool application_pinned_messages_read_blocked() noexcept {
    return false;
}
bool application_thread_browser_read_blocked() noexcept {
    return false;
}
u32 application_notification_menu_items(u64, u64, Slice<PopupItem>) noexcept {
    return 0;
}
// The notification controls own the user guild settings writes; here they
// record the write and project it as the real owner does.
bool application_notification_controls_set_hide_muted(u64 guild_id, bool hide) noexcept {
    ++fixture->hide_writes;
    fixture->hide_written = hide;
    chat_state_store_set_guild_hide_muted(&g_app.account_ui->chat_state, guild_id, hide);
    return true;
}
GuildChannelOptIn application_notification_controls_channel_opt_in(u64) noexcept {
    return {fixture->community, fixture->opt_in};
}
bool application_notification_controls_set_channel_opt_in(u64 guild_id, bool enabled) noexcept {
    ++fixture->opt_in_writes;
    fixture->opt_in_written = enabled;
    fixture->opt_in = enabled;
    chat_state_store_set_guild_channel_opt_in(&g_app.account_ui->chat_state, guild_id, enabled);
    return true;
}
bool application_guild_lifecycle_open_privacy(u64 guild_id) noexcept {
    ++fixture->privacy_opens;
    fixture->privacy_guild = guild_id;
    return true;
}
u32 application_guild_community_menu_items(u64, u32, u32, Slice<PopupItem>) noexcept {
    return 0;
}
// Server menu feature entry points: recorded, never opened.
bool application_guild_settings_open(u64 guild_id, u8 page) noexcept {
    ++fixture->settings_opens;
    fixture->settings_guild = guild_id;
    fixture->settings_page = page;
    return true;
}
u32 application_guild_settings_page_items(u64, u32 action_base, Slice<PopupItem> output) noexcept {
    if (output.count < 2)
        return 0;
    output[0] = {utf8_literal("Overview"), action_base + 1, popup_item_enabled};
    output[1] = {utf8_literal("Roles"), action_base + 2, popup_item_enabled};
    return 2;
}
bool application_guild_settings_create_channel(u64, u64, u8 type) noexcept {
    ++fixture->creations;
    fixture->creation_type = type;
    return true;
}
bool application_guild_lifecycle_open_invite(u64, u64) noexcept {
    ++fixture->invites;
    return true;
}
bool application_guild_lifecycle_leave(u64) noexcept {
    ++fixture->leaves;
    return true;
}
bool application_guild_community_can_create(u64) noexcept {
    return false;
}
bool application_guild_community_open_create(u64) noexcept {
    return false;
}
bool application_profile_editor_open(ProfileEditorTab tab, u64 guild) noexcept {
    fixture->profile_guild = tab == ProfileEditorTab::guild ? guild : 0;
    ++fixture->profile_opens;
    return true;
}
bool application_settings_open_key(Utf8View) noexcept {
    ++fixture->privacy_opens;
    return true;
}
u32 application_plugins_menu_items(ApplicationPluginMenuSurface, u64, Slice<PopupItem>, u64,
                                   const ApplicationPluginMenuExtra*) noexcept {
    return 0;
}
bool application_plugins_menu_bind() noexcept {
    return true;
}
void application_plugins_menu_dismissed() noexcept {
}
bool dismiss_popup() noexcept {
    application_scoped_read_menu_clear();
    application_plugins_menu_dismissed();
    const bool changed = popup_scene_close(&g_app.popup_scene);
    g_app.popup_owner_kind = PopupOwnerKind::none;
    return !changed ||
           (renderer_rebuild_content_text(&g_app.renderer, fixture->theme) && application_request_frame());
}
} // namespace maicord::app_detail
namespace maicord {
u64 scoped_native_clock_monotonic_milliseconds() noexcept {
    return scoped_read_native::fixture->now;
}
u64 scoped_native_clock_unix_milliseconds() noexcept {
    return 1'791'158'400'000ull + scoped_read_native::fixture->now;
}
} // namespace maicord
namespace maicord::app_detail {
void set_popup_owner(PopupOwnerKind owner) noexcept {
    // Surrounding composer/scroll owner transition only. All scope menu source
    // capture and row/popup/widget generation checks remain production code.
    ui_button_keyboard_cancel(&g_app.button_keyboard);
    g_app.message_composer_caret_visible = false;
    g_app.popup_owner_kind = owner;
}
bool application_message_navigation_cancel() noexcept {
    ++scoped_read_native::fixture->navigation_cancels;
    return true;
}
bool scroll_message_view_to(bool bottom) noexcept {
    bool changed{};
    if (bottom) {
        ++scoped_read_native::fixture->bottom_scrolls;
        return message_timeline_scroll_to_bottom(&g_app.message_view.timeline, &changed);
    }
    return message_timeline_scroll_to_start(&g_app.message_view.timeline, &changed);
}
} // namespace maicord::app_detail
namespace maicord::scoped_read_native {
bool resize(u32 width, u32 height, u32 dpi, const Theme& theme) noexcept {
    dismiss_popup();
    fixture->theme = theme;
    renderer_set_dpi(&g_app.renderer, dpi);
    return renderer_resize(&g_app.renderer, width, height) && project();
}
bool admitted(u64 id) noexcept {
    if (fixture->request_id)
        return false;
    return application_scoped_read_submit(id, fixture->now) ||
           application_read_state_submit(id, fixture->now);
}
bool receipt(u16 status, bool individual_request = false) noexcept {
    auto& f = *fixture;
    if (!f.request_id)
        return fail_at(__LINE__);
    RestClientResponse response{};
    response.request_id = f.request_id;
    response.status_code = status;
    const auto body =
        individual_request ? utf8_literal("{\"token\":\"native-fixture-token\"}") : Utf8View{};
    const bool valid = individual_request
                           ? application_read_state_response(
                                 response, reinterpret_cast<const u8*>(body.data), body.bytes, f.now)
                           : application_scoped_read_response(response, body, f.now);
    f.request_id = 0;
    g_app.primary_request_id = 0; // Synthetic surrounding PRIMARY completion.
    return valid && project();
}
bool advance_message(u64 id, u64 last) noexcept {
    const auto* old = chat_state_store_find_channel(g_app.account_ui->chat_state, id);
    if (!old)
        return fail_at(__LINE__);
    ChannelInput input{};
    input.id = old->id;
    input.guild_id = old->guild_id;
    input.parent_id = old->parent_id;
    input.type = old->type;
    input.position = old->position;
    input.flags = old->flags;
    input.last_message_id = last;
    input.last_message_known = true;
    return chat_state_store_upsert_channel(&g_app.account_ui->chat_state, input).ok() && project();
}
bool read(u64 id, u64 cursor, u32 mentions) noexcept {
    const auto* c = chat_state_store_find_channel(g_app.account_ui->chat_state, id);
    return c && c->last_read_message_id == cursor && c->mention_count == mentions;
}
bool mention(u64 id, u32 count) noexcept {
    const auto* c = chat_state_store_find_channel(g_app.account_ui->chat_state, id);
    if (!c)
        return fail_at(__LINE__);
    ReadStateEntry entry{};
    entry.channel_id = id;
    entry.message_id = c->last_read_message_id;
    entry.mention_count = count;
    entry.fields = read_state_cursor | read_state_mentions;
    return read_state_upsert(&g_app.account_ui->chat_state, entry, 2) && project();
}
bool stale_refresh_contract() noexcept {
    auto& f = *fixture;
    for (u32 sample = 0; sample < 3; ++sample) {
        const auto kind = sample == 2 ? ReadScopeKind::category : ReadScopeKind::guild;
        const auto id = sample == 2 ? kRetryCategory : kGuild;
        if (!menu(kind, id, true))
            return fail_at(__LINE__);
        const auto scope = application_scoped_read_metrics();
        const auto broker = application_read_state_metrics();
        const u32 posts = f.bulk_posts + f.individual_posts;
        const u64 cursor = chat_state_store_find_channel(g_app.account_ui->chat_state, kParent)
                               ->last_read_message_id;
        if (sample == 0) {
            ++g_app.plugin_account_scope_serial;
        } else if (sample == 1) {
            read_state_clear(&g_app.account_ui->read_state_store);
        } else {
            auto& state = g_app.account_ui->chat_state;
            const auto* source = chat_state_store_find_channel(state, kRetryCategory);
            const u16 generation = source ? source->generation : 0;
            if (!SCOPED_READ_REQUIRE(
                    source && chat_state_store_remove_channel(&state, kRetryCategory) &&
                        channel(kRetryCategory, utf8_literal("REVIEW QUEUE"), 4, 0, 2, 0, 0, 0),
                    330))
                return fail_at(__LINE__);
            const auto* replacement = chat_state_store_find_channel(state, kRetryCategory);
            if (!SCOPED_READ_REQUIRE(replacement && replacement->generation != generation, 331))
                return fail_at(__LINE__);
        }
        // Refresh must dismiss the original lifetime. It must not publish an
        // enabled row bound to a fresh actor/read epoch/recycled category.
        if (!application_scoped_read_menu_refresh() ||
            !SCOPED_READ_REQUIRE(!g_app.popup_scene.active, 332 + sample) ||
            !application_scoped_read_menu_activate(popup_action_mark_scope_read) ||
            !application_scoped_read_menu_activate(popup_action_retry_scope_read))
            return fail_at(__LINE__);
        const auto after = application_scoped_read_metrics();
        const auto read_after = application_read_state_metrics();
        if (!SCOPED_READ_REQUIRE(
                !g_app.popup_scene.active && f.bulk_posts + f.individual_posts == posts &&
                    after.plan_builds == scope.plan_builds && after.encodings == scope.encodings &&
                    read_after.action_serial == broker.action_serial && read(kParent, cursor, 0),
                335 + sample))
            return fail_at(__LINE__);
        log_u64(LogLevel::info, "scoped_read_stale_refresh", "sample", sample + 1);
        log_u64(LogLevel::info, "scoped_read_stale_refresh", "post_delta",
                f.bulk_posts + f.individual_posts - posts);
        log_u64(LogLevel::info, "scoped_read_stale_refresh", "plan_delta",
                after.plan_builds - scope.plan_builds);
        log_u64(LogLevel::info, "scoped_read_stale_refresh", "encoding_delta",
                after.encodings - scope.encodings);
        log_u64(LogLevel::info, "scoped_read_stale_refresh", "source_action_serial_changed",
                read_after.action_serial != broker.action_serial);
    }
    return project();
}
void costs() noexcept {
    const auto m = application_scoped_read_metrics();
    constexpr const char* names[]{"owner_bytes",       "owner_reserved",     "owner_committed",
                                  "queue_reserved",    "queue_committed",    "targets_reserved",
                                  "targets_committed", "plan_reserved",      "plan_committed",
                                  "jobs_reserved",     "jobs_committed",     "flight_reserved",
                                  "flight_committed",  "candidate_reserved", "candidate_committed"};
    const u32 values[]{m.owner_bytes,
                       m.owner_reserved_bytes,
                       m.owner_committed_bytes,
                       m.queue_reserved_bytes,
                       m.queue_committed_bytes,
                       m.targets_reserved_bytes,
                       m.targets_committed_bytes,
                       m.plan_reserved_bytes,
                       m.plan_committed_bytes,
                       m.jobs_reserved_bytes,
                       m.jobs_committed_bytes,
                       m.flight_reserved_bytes,
                       m.flight_committed_bytes,
                       m.candidate_reserved_bytes,
                       m.candidate_committed_bytes};
    for (u32 i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        log_u64(LogLevel::info, "scoped_read_cost", names[i], values[i]);
    log_u64(LogLevel::info, "scoped_read_cost", "menu_owner_bytes",
            application_scoped_read_menu_owner_bytes());
    log_u64(LogLevel::info, "scoped_read_cost", "read_broker_owner_bytes",
            application_read_state_owner_bytes());
    const auto broker = application_read_state_metrics();
    constexpr const char* broker_names[]{
        "broker_owner_reserved", "broker_owner_committed", "intents_reserved",     "intents_committed",
        "token_reserved",        "token_committed",        "source_bank_reserved", "source_bank_committed",
        "echo_bank_reserved",    "echo_bank_committed"};
    const u32 broker_values[]{broker.owner_reserved_bytes,   broker.owner_committed_bytes,
                              broker.intents_reserved_bytes, broker.intents_committed_bytes,
                              broker.token_reserved_bytes,   broker.token_committed_bytes,
                              broker.actions_reserved_bytes, broker.actions_committed_bytes,
                              broker.echoes_reserved_bytes,  broker.echoes_committed_bytes};
    for (u32 i = 0; i < sizeof(broker_values) / sizeof(broker_values[0]); ++i)
        log_u64(LogLevel::info, "scoped_read_cost", broker_names[i], broker_values[i]);
    log_u64(LogLevel::info, "scoped_read_cost", "fixture_bytes", sizeof(Fixture));
    log_u64(LogLevel::info, "scoped_read_cost", "account_ui_bytes", sizeof(AccountUiRuntime));
    const void* owners[]{fixture, g_app.account_ui};
    constexpr const char* committed_names[]{"fixture_committed_bytes", "account_ui_committed_bytes"};
    constexpr const char* reserved_names[]{"fixture_reserved_bytes", "account_ui_reserved_bytes"};
    for (u32 i = 0; i < 2; ++i) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(owners[i], &region, sizeof(region)) == sizeof(region)) {
            log_u64(LogLevel::info, "scoped_read_cost", committed_names[i],
                    region.State == MEM_COMMIT ? region.RegionSize : 0);
            log_u64(LogLevel::info, "scoped_read_cost", reserved_names[i], region.RegionSize);
        }
    }
    log_u64(LogLevel::info, "scoped_read_cost", "glyph_instance_capacity",
            g_app.renderer.pipeline.glyph_instance_capacity);
    ID3D11Buffer* buffers[]{g_app.renderer.pipeline.ui_instance_buffer,
                            g_app.renderer.pipeline.glyph_instance_buffer,
                            g_app.renderer.pipeline.media_instance_buffer};
    constexpr const char* buffer_names[]{"paint_gpu_capacity_bytes", "text_gpu_capacity_bytes",
                                         "media_gpu_capacity_bytes"};
    for (u32 i = 0; i < 3; ++i) {
        D3D11_BUFFER_DESC description{};
        if (buffers[i])
            buffers[i]->GetDesc(&description);
        log_u64(LogLevel::info, "scoped_read_cost", buffer_names[i], description.ByteWidth);
    }
}
// ---- server menus --------------------------------------------------------------------
UiRect header_rectangle() noexcept {
    const auto* node =
        ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::guild_menu));
    return node ? node->rectangle : UiRect{};
}
bool click_header() noexcept {
    const auto r = header_rectangle();
    if (!SCOPED_READ_REQUIRE(ui_rect_has_area(r), 400))
        return false;
    SendMessageW(g_app.window, WM_LBUTTONDOWN, MK_LBUTTON, center(r));
    SendMessageW(g_app.window, WM_LBUTTONUP, 0, center(r));
    application_guild_menu_synchronize();
    return project();
}
Utf8View text(const char* label) noexcept {
    u32 bytes = 0;
    while (label[bytes])
        ++bytes;
    return {label, bytes};
}
template <usize Count>
bool rows(const char* const (&labels)[Count]) noexcept {
    const auto& popup = g_app.popup_scene;
    if (!popup.active || popup.item_count != Count)
        return false;
    for (u32 i = 0; i < Count; ++i)
        if (!utf8_equal(popup.items[i].label, text(labels[i])))
            return false;
    return true;
}
u32 row_action(const char* label) noexcept {
    for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
        if (utf8_equal(g_app.popup_scene.items[i].label, text(label)))
            return g_app.popup_scene.items[i].action;
    return 0;
}
bool has_row(const char* label) noexcept {
    return row_action(label) != 0;
}
// The popout sits 4 DIP below the header, centred on it, and the chevron
// becomes an X while it is open.
bool header_popout() noexcept {
    const auto r = header_rectangle();
    const auto p = g_app.popup_scene.rectangle;
    const float offset = (p.left + p.right - r.left - r.right) * .5f;
    return application_guild_menu_header_open() && g_app.shell_scene.guild_menu_expanded &&
           g_app.popup_owner_kind == PopupOwnerKind::guild && g_app.popup_scene.owner_id == kGuild &&
           p.top >= r.bottom + kGuildHeaderMenuSpacing - .5f && p.top <= r.bottom + kGuildHeaderMenuSpacing + .5f &&
           offset > -1 && offset < 1 && ui_rect_width(p) > kGuildHeaderMenuWidth - 1;
}
bool server_menus() noexcept {
    auto& f = *fixture;
    auto& state = g_app.account_ui->chat_state;
    dismiss_popup();
    g_app.preferences.developer_mode = true;
    auto* selected = const_cast<ChannelRecord*>(chat_state_store_find_channel(state, kSelected));
    if (!selected || !chat_state_store_select_channel(&state, kSelected).ok() || !project())
        return fail_at(__LINE__);
    selected->effective_permissions |= kPermissionCreateInvite;
    // Owner: every server row the official client offers here, without Leave server.
    constexpr const char* owner[]{"Invite people",    "Server settings",         "Create channel",
                                  "Create category",  "Privacy settings",        "Edit per-server profile",
                                  "Hide muted channels", "Copy server ID"};
    if (!click_header() || !SCOPED_READ_REQUIRE(header_popout() && rows(owner), 401) ||
        !capture(L"guild-header-owner.bmp"))
        return fail_at(__LINE__);
    // A second click on the header closes it and the chevron returns.
    if (!click_header() ||
        !SCOPED_READ_REQUIRE(!g_app.popup_scene.active && !g_app.shell_scene.guild_menu_expanded, 402))
        return fail_at(__LINE__);
    if (!click_header() || !action(row_action("Server settings"), false))
        return fail_at(__LINE__);
    application_guild_menu_synchronize();
    if (!SCOPED_READ_REQUIRE(f.settings_opens == 1 && f.settings_guild == kGuild && f.settings_page == 0 &&
                                 !g_app.popup_scene.active && !g_app.shell_scene.guild_menu_expanded,
                             403))
        return fail_at(__LINE__);
    // Keyboard: the first row is selected; arrows and Enter pick a row.
    if (!application_guild_menu_toggle(true) ||
        !SCOPED_READ_REQUIRE(header_popout() && g_app.popup_scene.selected_index == 0, 404))
        return fail_at(__LINE__);
    for (u32 i = 0; i < 3; ++i)
        SendMessageW(g_app.window, WM_KEYDOWN, VK_DOWN, 0);
    SendMessageW(g_app.window, WM_KEYDOWN, VK_RETURN, 0);
    if (!SCOPED_READ_REQUIRE(f.creations == 1 && f.creation_type == 4 && !g_app.popup_scene.active, 405) ||
        !application_guild_menu_toggle(true))
        return fail_at(__LINE__);
    SendMessageW(g_app.window, WM_KEYDOWN, VK_ESCAPE, 0);
    application_guild_menu_synchronize();
    if (!SCOPED_READ_REQUIRE(!g_app.popup_scene.active && !g_app.shell_scene.guild_menu_expanded, 406))
        return fail_at(__LINE__);
    // Hide muted channels: a muted channel leaves the list until shown again.
    const auto& navigation = g_app.account_ui->navigation_view;
    if (!chat_state_store_set_channel_mute(&state, kRetryChild, true, true) || !project() ||
        !SCOPED_READ_REQUIRE(navigation_view_find_channel(navigation, kRetryChild) != nullptr, 407) ||
        !click_header() || !action(row_action("Hide muted channels"), false) ||
        !SCOPED_READ_REQUIRE(chat_state_store_find_guild(state, kGuild)->hide_muted_channels &&
                                 !navigation_view_find_channel(navigation, kRetryChild) && f.hide_writes == 1 &&
                                 f.hide_written,
                             408) ||
        !click_header())
        return fail_at(__LINE__);
    const u32 hide = row_action("Hide muted channels");
    bool checked = false;
    for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
        checked = checked ||
                  (g_app.popup_scene.items[i].action == hide && (g_app.popup_scene.items[i].flags & popup_item_checked));
    if (!SCOPED_READ_REQUIRE(checked, 409) || !action(hide, true) ||
        !SCOPED_READ_REQUIRE(navigation_view_find_channel(navigation, kRetryChild) != nullptr &&
                                 f.hide_writes == 2 && !f.hide_written,
                             410) ||
        !chat_state_store_set_channel_mute(&state, kRetryChild, false, false))
        return fail_at(__LINE__);
    // Community channel opt-in: "Show all channels" leads the notification
    // group, checked while every channel shows; unchecking turns opt-in on.
    f.community = true;
    f.opt_in = false;
    if (!click_header() ||
        !SCOPED_READ_REQUIRE(has_row("Show all channels") && row_action("Show all channels") != 0, 4101))
        return fail_at(__LINE__);
    const u32 show_all = row_action("Show all channels");
    bool all_checked = false;
    for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
        all_checked = all_checked || (g_app.popup_scene.items[i].action == show_all &&
                                      (g_app.popup_scene.items[i].flags & popup_item_checked));
    if (!SCOPED_READ_REQUIRE(all_checked, 4102) || !action(show_all, true) ||
        !SCOPED_READ_REQUIRE(f.opt_in_writes == 1 && f.opt_in_written &&
                                 chat_state_store_find_guild(state, kGuild)->channel_opt_in &&
                                 !navigation_view_find_channel(navigation, kRetryChild),
                             4103) ||
        !click_header() || !action(row_action("Show all channels"), true) ||
        !SCOPED_READ_REQUIRE(f.opt_in_writes == 2 && !f.opt_in_written &&
                                 navigation_view_find_channel(navigation, kRetryChild) != nullptr,
                             4104))
        return fail_at(__LINE__);
    f.community = false;
    // Administrator through real role permissions: Leave server joins.
    auto* guild = const_cast<GuildRecord*>(chat_state_store_find_guild(state, kGuild));
    constexpr u64 kAdminRole = 94900;
    const u64 self_roles[]{kAdminRole};
    if (!chat_state_store_bind_permissions(&state, {f.roles, 4}, {f.overwrites, 2}) ||
        !chat_state_store_upsert_role(&state, {kGuild, kGuild, kPermissionViewChannel | kPermissionReadMessageHistory,
                                               utf8_literal("@everyone"), 0}) ||
        !chat_state_store_upsert_role(&state, {kAdminRole, kGuild, kPermissionAdministrator, utf8_literal("admin"), 1}) ||
        !chat_state_store_set_self_roles(&state, kGuild, 1234, 99, {self_roles, 1}))
        return fail_at(__LINE__);
    guild->owner_id = 1234;
    chat_state_store_recompute_guild_permissions(&state, kGuild);
    constexpr const char* admin[]{"Invite people",    "Server settings",         "Create channel",
                                  "Create category",  "Privacy settings",        "Edit per-server profile",
                                  "Hide muted channels", "Leave server", "Copy server ID"};
    if (!project() || !click_header() || !SCOPED_READ_REQUIRE(header_popout() && rows(admin), 411) ||
        !action(row_action("Leave server"), false) || !SCOPED_READ_REQUIRE(f.leaves == 1, 412))
        return fail_at(__LINE__);
    // The server icon's context menu offers the same server rows; Server
    // settings lists its pages and a page opens Server Settings there.
    if (!menu(ReadScopeKind::guild, kGuild, false) ||
        !SCOPED_READ_REQUIRE(has_row("Invite people") && has_row("Hide muted channels") &&
                                 has_row("Server settings") && has_row("Privacy settings") &&
                                 has_row("Edit per-server profile") && has_row("Create channel") &&
                                 has_row("Create category") && has_row("Leave server") && has_row("Copy server ID"),
                             413) ||
        !action(row_action("Server settings"), false))
        return fail_at(__LINE__);
    constexpr const char* pages[]{"Overview", "Roles"};
    if (!SCOPED_READ_REQUIRE(rows(pages) && g_app.popup_owner_kind == PopupOwnerKind::guild, 414) ||
        !action(row_action("Roles"), true) ||
        !SCOPED_READ_REQUIRE(f.settings_opens == 2 && f.settings_page == 2 && !g_app.popup_scene.active, 415))
        return fail_at(__LINE__);
    if (!menu(ReadScopeKind::guild, kGuild, false) || !action(row_action("Privacy settings"), false) ||
        !menu(ReadScopeKind::guild, kGuild, false) || !action(row_action("Edit per-server profile"), false) ||
        !SCOPED_READ_REQUIRE(f.privacy_opens == 1 && f.privacy_guild == kGuild && f.profile_opens == 1 && f.profile_guild == kGuild, 416))
        return fail_at(__LINE__);
    // Plain member: no invite, settings, creation or profile rows.
    if (!chat_state_store_upsert_role(&state, {kAdminRole, kGuild, 0, utf8_literal("admin"), 1}))
        return fail_at(__LINE__);
    chat_state_store_recompute_guild_permissions(&state, kGuild);
    constexpr const char* member[]{"Privacy settings", "Hide muted channels", "Leave server", "Copy server ID"};
    if (!project() || !click_header() || !SCOPED_READ_REQUIRE(header_popout() && rows(member), 417) ||
        !capture(L"guild-header-member.bmp"))
        return fail_at(__LINE__);
    // A stale activation after the popout closed is consumed without effect.
    const u32 leave = row_action("Leave server");
    dismiss_popup();
    if (!application_guild_menu_activate(leave) || !SCOPED_READ_REQUIRE(f.leaves == 1, 418))
        return fail_at(__LINE__);
    g_app.preferences.developer_mode = false;
    application_guild_menu_synchronize();
    return project();
}
// The header dropdown's wheel route (scoped_read_menu_scroll.cpp).
bool menu_scroll() noexcept;
bool scenarios() noexcept {
    auto& f = *fixture;
    f.now = 1000;
    f.refresh_ok = true;
    if (!SCOPED_READ_REQUIRE(account_storage(), 390) || !SCOPED_READ_REQUIRE(project(), 391) ||
        !SCOPED_READ_REQUIRE(capture(L"scoped-read-before-dark96.bmp"), 392) ||
        !SCOPED_READ_REQUIRE(menu(ReadScopeKind::category, kCategory, false), 393) ||
        !SCOPED_READ_REQUIRE(menu(ReadScopeKind::category, kCategory, true), 394) ||
        !SCOPED_READ_REQUIRE(capture(L"scoped-read-category-keyboard-dark96.bmp"), 395) ||
        !SCOPED_READ_REQUIRE(menu(ReadScopeKind::guild, kGuild, false), 396) ||
        !SCOPED_READ_REQUIRE(capture(L"scoped-read-server-menu-dark96.bmp"), 397) ||
        !SCOPED_READ_REQUIRE(action(popup_action_mark_scope_read, false), 398))
        return fail_at(__LINE__);
    const auto pending = application_scoped_read_view(ReadScopeKind::guild, kGuild);
    if (!SCOPED_READ_REQUIRE(pending.running && pending.pending == 101 && !pending.can_retry, 300) ||
        !capture(L"scoped-read-server-pending-dark96.bmp") || !warm(1))
        return fail_at(__LINE__);
    const u32 posts = f.bulk_posts;
    PopupItemLayout disabled{};
    if (!popup_scene_resolve_item_layout(g_app.popup_scene, 0, &disabled))
        return fail_at(__LINE__);
    SendMessageW(g_app.window, WM_LBUTTONDOWN, MK_LBUTTON, center(disabled.visible_rectangle));
    if (!SCOPED_READ_REQUIRE(f.bulk_posts == posts && g_app.popup_scene.active &&
                                 !(g_app.popup_scene.items[0].flags & popup_item_enabled),
                             301))
        return fail_at(__LINE__);
    // A later message may change navigation presentation; it must not widen
    // the accepted scope's captured message cursor.
    if (!advance_message(kParent, 2000100) || !admitted(7001) ||
        !SCOPED_READ_REQUIRE(f.admitted_count == 100 && f.bulk_posts == 1 &&
                                 f.admitted[0].channel_id == kParent &&
                                 f.admitted[0].message_id == 2000000 &&
                                 f.admitted[1].channel_id == kThread && f.admitted[1].message_id == 3000000,
                             302) ||
        !capture(L"scoped-read-server-first100-dark96.bmp") || !receipt(204))
        return fail_at(__LINE__);
    f.now = 1999;
    if (!SCOPED_READ_REQUIRE(!admitted(7002) && f.bulk_posts == 1, 303) ||
        !capture(L"scoped-read-server-waiting-dark96.bmp"))
        return fail_at(__LINE__);
    f.now = 2000;
    if (!admitted(7003) ||
        !SCOPED_READ_REQUIRE(f.admitted_count == 1 && f.bulk_posts == 2 &&
                                 f.admitted[0].channel_id == 94498 && f.admitted[0].message_id == 5098000,
                             304) ||
        !capture(L"scoped-read-server-final1-dark96.bmp") || !receipt(204) ||
        !SCOPED_READ_REQUIRE(read(kParent, 2000000, 0) && read(kThread, 3000000, 0) &&
                                 read(94400, 5000000, 0) && read(kOther, 3999900, 4),
                             305) ||
        !capture(L"scoped-read-server-success-dark96.bmp"))
        return fail_at(__LINE__);
    const auto server_done = application_scoped_read_view(ReadScopeKind::guild, kGuild);
    if (!SCOPED_READ_REQUIRE(!server_done.running && server_done.total == 101 &&
                                 server_done.completed == 101 && !server_done.pending &&
                                 !server_done.failed && !server_done.can_retry,
                             322))
        return fail_at(__LINE__);
    for (u32 i = 0; i < 99; ++i)
        if (!SCOPED_READ_REQUIRE(read(94400 + i, 5000000 + i * 1000, 0), 323))
            return fail_at(__LINE__);
    const auto* parent = chat_state_store_find_channel(g_app.account_ui->chat_state, kParent);
    if (!SCOPED_READ_REQUIRE(parent && parent->last_message_id > parent->last_read_message_id, 306))
        return fail_at(__LINE__);
    if (!advance_message(kRetryParent, 6000000) || !advance_message(kRetryThread, 6001000) ||
        !advance_message(kRetryChild, 6002000) || !mention(kRetryParent, 2) || !mention(kRetryThread, 1) ||
        !mention(kRetryChild, 142) || !resize(1440, 1140, 144, kLightTheme) ||
        !menu(ReadScopeKind::category, kRetryCategory, true) ||
        !capture(L"scoped-read-category-menu-light144.bmp") ||
        !action(popup_action_mark_scope_read, true) ||
        !capture(L"scoped-read-category-pending-light144.bmp"))
        return fail_at(__LINE__);
    const u32 first_individual = f.individual_posts;
    u32 failed{};
    for (u32 i = 0; i < 3; ++i) {
        if (!admitted(7100 + i))
            return SCOPED_READ_REQUIRE(false, 310 + i);
        const bool fail = f.individual_channel == kRetryThread;
        failed += fail ? 1u : 0u;
        ++f.now;
        if (!receipt(fail ? 403 : 200, true))
            return fail_at(__LINE__);
    }
    const auto error = application_scoped_read_view(ReadScopeKind::category, kRetryCategory);
    if (!SCOPED_READ_REQUIRE(failed == 1 && f.individual_posts - first_individual == 3 && !error.running &&
                                 error.failed == 1 && error.can_retry && error.error,
                             314) ||
        !capture(L"scoped-read-category-failure-light144.bmp") || !warm(2))
        return fail_at(__LINE__);
    // Root's production popup refresh retains the selected action where it
    // remains enabled. Explicit Retry uses its latest failed-only ticket.
    if (!action(popup_action_retry_scope_read, true) ||
        !capture(L"scoped-read-category-retry-light144.bmp") || !admitted(7200) ||
        !SCOPED_READ_REQUIRE(
            f.individual_channel == kRetryThread && f.individual_posts - first_individual == 4, 315) ||
        !receipt(200, true))
        return fail_at(__LINE__);
    const auto done = application_scoped_read_view(ReadScopeKind::category, kRetryCategory);
    if (!SCOPED_READ_REQUIRE(!done.running && !done.failed && !done.can_retry &&
                                 read(kRetryParent, 6000000, 0) && read(kRetryThread, 6001000, 0) &&
                                 read(kRetryChild, 6002000, 0),
                             316) ||
        !capture(L"scoped-read-category-success-light144.bmp") || !resize(960, 1050, 144, kLightTheme) ||
        !menu(ReadScopeKind::guild, kGuild, true) || !capture(L"scoped-read-narrow-menu-light144.bmp"))
        return fail_at(__LINE__);
    // A new actor scope invalidates an already-open popup. A recognized stale
    // action is consumed without transport, optimistic mutation or new plan.
    ApplicationScopedReadTicket stale{};
    if (!SCOPED_READ_REQUIRE(
            !application_scoped_read_shortcut() && !f.navigation_cancels && !f.bottom_scrolls, 319) ||
        !application_scoped_read_capture(ReadScopeKind::guild, kGuild, &stale))
        return fail_at(__LINE__);
    const u32 total_posts = f.bulk_posts + f.individual_posts;
    ++g_app.plugin_account_scope_serial;
    if (!application_scoped_read_menu_activate(popup_action_mark_scope_read) ||
        !SCOPED_READ_REQUIRE(!application_scoped_read_current(stale) &&
                                 f.bulk_posts + f.individual_posts == total_posts &&
                                 read(kParent, 2000000, 0),
                             317))
        return fail_at(__LINE__);
    costs();
    if (!stale_refresh_contract())
        return fail_at(__LINE__);
    // Directly exercise the production shortcut owner. Its surrounding history
    // cancellation and scrolling seams are synthetic; modifier/repeat/modal
    // input arbitration and OS Shift+Escape injection are separate evidence.
    dismiss_popup();
    f.now += 1000;
    if (!application_scoped_read_shortcut() ||
        !SCOPED_READ_REQUIRE(f.navigation_cancels == 1 && f.bottom_scrolls == 1 &&
                                 read(kParent, 2000100, 0) && !application_scoped_read_shortcut(),
                             320) ||
        !admitted(7300) ||
        !SCOPED_READ_REQUIRE(f.admitted_count == 1 && f.admitted[0].channel_id == kParent &&
                                 f.admitted[0].message_id == 2000100,
                             321) ||
        !receipt(204))
        return fail_at(__LINE__);
    if (!SCOPED_READ_REQUIRE(server_menus(), 420) || !SCOPED_READ_REQUIRE(menu_scroll(), 421))
        return fail_at(__LINE__);
    log_u64(LogLevel::info, "scoped_read_native", "captures", f.captures);
    log_u64(LogLevel::info, "scoped_read_native", "bulk_posts", f.bulk_posts);
    log_u64(LogLevel::info, "scoped_read_native", "individual_posts", f.individual_posts);
    log_write(LogLevel::info, "scoped_read_native",
              "fixture_window_native_routes_production_scope_menu_no_os_injection");
    log_write(LogLevel::info, "scoped_read_native",
              "production_shortcut_owner_direct_no_global_key_dispatch");
    return SCOPED_READ_REQUIRE(
        f.captures == 16 && f.bulk_posts == 3 && f.individual_posts == 4 && f.refresh_ok, 318);
}
} // namespace maicord::scoped_read_native
namespace maicord {
bool scoped_read_ui_probe() noexcept {
    using namespace app_detail;
    using namespace scoped_read_native;
    diagnostics = 0;
    fixture = static_cast<Fixture*>(
        VirtualAlloc(nullptr, sizeof(Fixture), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!fixture)
        return fail_at(__LINE__);
    fixture->theme = kDefaultTheme;
    fixture->refresh_ok = true;
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.lpfnWndProc = window_message;
    type.hInstance = instance;
    type.lpszClassName = L"MaiCordScopedReadUiProbe";
    if (!RegisterClassW(&type))
        return fail_at(__LINE__);
    // Never activated, no taskbar button, opened behind every other window.
    constexpr DWORD kWindowExStyle = WS_EX_NOREDIRECTIONBITMAP | probe_window::kExtendedStyle;
    RECT bounds{0, 0, 960, 760};
    if (!AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, kWindowExStyle))
        return fail_at(__LINE__);
    HWND window =
        CreateWindowExW(kWindowExStyle, type.lpszClassName, L"MaiCord native scoped read",
                        WS_OVERLAPPEDWINDOW, 32, 32, bounds.right - bounds.left, bounds.bottom - bounds.top,
                        nullptr, nullptr, instance, nullptr);
    if (!window)
        return fail_at(__LINE__);
    g_app.window = window;
    struct Cleanup {
        HWND window;
        bool apartment;
        ~Cleanup() noexcept {
            application_scoped_read_menu_clear();
            popup_scene_close(&g_app.popup_scene);
            application_scoped_read_release();
            application_read_state_release(g_app);
            renderer_unbind_text_storage(&g_app.renderer);
            renderer_bind_navigation_view(&g_app.renderer, nullptr, nullptr, nullptr);
            message_scene_release(&g_app.message_scene);
            message_view_runtime_release(&g_app.message_view);
            popup_scene_release(&g_app.popup_scene);
            ui_scene_release(&g_app.shell_scene);
            renderer_shutdown(&g_app.renderer);
            if (g_app.account_ui)
                VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
            DestroyWindow(window);
            if (apartment)
                CoUninitialize();
            VirtualFree(fixture, 0, MEM_RELEASE);
            fixture = nullptr;
        }
    } cleanup{window, SUCCEEDED(apartment)};
    probe_window::show(window);
    SetFocus(window);
    if (!SCOPED_READ_REQUIRE(SUCCEEDED(apartment) &&
                                 renderer_initialize(&g_app.renderer, window),
                             100))
        return fail_at(__LINE__);
    g_app.renderer_ready = true;
    auto& f = *fixture;
    if (!SCOPED_READ_REQUIRE(
            ui_scene_initialize_bootstrap(&g_app.shell_scene) &&
                guild_rail_scene_initialize(&g_app.guild_rail_scene, {f.rail_rows, 3}, {f.rail_widgets, 8},
                                            {f.rail_nodes, 8}) &&
                shell_navigation_scene_initialize(
                    &g_app.shell_navigation_scene, {f.navigation_rows, kVisible},
                    {f.navigation_widgets, kVisible * 8 + 1}, {f.navigation_nodes, kVisible * 8 + 1}) &&
                popup_scene_initialize(&g_app.popup_scene, {f.popup_items, 24}, {f.popup_rows, 24},
                                       {f.popup_widgets, 64}, {f.popup_nodes, 64}) &&
                renderer_bind_popup_scene(&g_app.renderer, &g_app.popup_scene),
            101))
        return fail_at(__LINE__);
    return scenarios();
}
} // namespace maicord
#undef SCOPED_READ_REQUIRE
extern "C" __declspec(noreturn) void WINAPI maicord_scoped_read_ui_probe_entry() noexcept {
    wchar_t enabled[4]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_SCOPED_READ_PROBE", enabled, 4) != 1 || enabled[0] != L'1') {
        maicord::scoped_read_native::fail_at(__LINE__, 90);
        ExitProcess(2);
    }
    ExitProcess(maicord::scoped_read_ui_probe() ? 0 : 1);
}
