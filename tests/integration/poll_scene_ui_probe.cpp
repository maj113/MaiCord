#include "poll_scene_ui_probe.hpp"
#include "probe_present.hpp"
#include "maicord/core/log.hpp"
#include "maicord/model/message_poll_store.hpp"
#include "maicord/ui/poll_presentation.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"
#include "../../src/render/d3d11_renderer_interaction_state.hpp"

#include <d3d11_1.h>

namespace maicord {
namespace {
bool require(bool value, u32 stage) noexcept {
    if (!value)
        log_u64(LogLevel::error, "probe", "poll_native_failure_stage", stage);
    return value;
}
// Preserve borrowed modal bindings individually, without reverting any GPU
// buffers or retained-text allocations that the renderer legitimately grows.
#define POLL_BINDINGS(X)                                                                                   \
    X(ui_scene)                                                                                            \
    X(message_scene)                                                                                       \
    X(message_text) X(guild_rail_scene) X(shell_navigation_scene) X(message_composer_scene)                \
        X(member_list_scene) X(thread_header) X(focus_owner) X(video_scene) X(audio_scene) X(poll_scene)   \
            X(poll_voters_scene) X(profile_scene) X(image_viewer) X(settings_scene) X(theme_editor_scene)  \
                X(friends_scene) X(friends_requests_scene) X(media_picker_scene) X(quick_switcher_scene)   \
                    X(download_scene) X(popup_scene) X(reaction_viewer) X(text_form) X(forum_form)         \
                        X(thread_browser_scene) X(search_scene) X(pinned_messages_scene)                   \
                            X(message_forward_scene) X(tooltip_scene) X(message_action_bar)
struct Borrowed {
#define FIELD(name) decltype(D3D11Renderer::name) name{};
    POLL_BINDINGS(FIELD)
#undef FIELD
};
struct Fixture {
    MessageRecord records[1]{};
    u32 order[1]{};
    u8 bytes[128]{};
    u32 heights[1]{};
    u64 fenwick[1]{};
    UiWidget widgets[kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode nodes[kMessageSceneWidgetsPerRow + 1]{};
    MessageStore store{};
    MessagePollStore polls{};
    UiVariableList list{};
    MessageTimeline timeline{};
    MessageScene messages{};
    MessageScenePollBinding binding{};
    MessageTextCommand commands[48]{};
    char display[4096]{};
    TextStyleSpan styles[48]{};
    MessageMarkdownSemantic semantics[8]{};
    MessageTextMeasurement measurements[4]{};
    MessageTextSpoilerReveal reveals[2]{};
    u32 scratch[512]{};
    MessageTextPresentation text{};
    UiScene shell{};
    GuildRailScene guilds{};
    GuildRailRowWidget guild_rows[1]{};
    UiWidget guild_widgets[1]{};
    UiLayoutNode guild_nodes[1]{};
    ShellNavigationScene navigation{};
    ShellNavigationRowWidget navigation_rows[1]{};
    UiWidget navigation_widgets[1]{};
    UiLayoutNode navigation_nodes[1]{};
    UiPollAnswer answers[10]{};
    UiPollScene card{};
    UiPollVoter voters[25]{};
    UiPollVotersScene inspector{};
    UiWidgetManager inspector_owner{};
    UiWidget inspector_widgets[1]{};
    UiLayoutNode inspector_nodes[1]{};
    UiWidgetHandle inspector_root{};
    bool initialize(UiTextMeasureCache* cache) noexcept {
        const GatewayMessagePollAnswer input[]{{7, utf8_literal("River walk"), 0, {}, false},
                                               {42, utf8_literal("Shared meal"), 0, {}, false}};
        const GatewayMessagePollAnswerCount counts[]{{7, 3, true}, {42, 1, false}};
        GatewayMessagePoll poll{};
        poll.present = poll.results_known = true;
        poll.layout_type = 1;
        poll.question = utf8_literal("Which plan fits us?");
        poll.answers = {input, 2};
        poll.answer_counts = {counts, 2};
        poll.fields = gateway_message_poll_field_question | gateway_message_poll_field_answers |
                      gateway_message_poll_field_layout | gateway_message_poll_field_results;
        if (!message_poll_store_initialize_owned(&polls, 1, 2048) ||
            !message_poll_store_apply(&polls, 4400, 55, poll, 1).ok() ||
            !message_store_initialize(&store, {records, 1}, {order, 1}, {bytes, sizeof(bytes)}) ||
            !message_store_upsert(&store, {4400, 55, 91, 0, 0, utf8_literal(""), 0}).ok() ||
            !ui_variable_list_initialize(&list, {heights, 1}, {fenwick, 1}, 1, ui_extent_units(96)) ||
            !message_timeline_initialize(&timeline, &store, &list, ui_extent_units(96),
                                         ui_extent_units(1000), false) ||
            !message_scene_initialize_owned(&messages, &timeline, 1, 1,
                                            {widgets, kMessageSceneWidgetsPerRow + 1},
                                            {nodes, kMessageSceneWidgetsPerRow + 1}))
            return false;
        binding.store = &polls;
        binding.measure = cache;
        binding.backend_revision = 1;
        binding.can_vote = binding.can_view_voters = true;
        if (!message_scene_bind_poll_source(&messages, &binding) ||
            !message_text_presentation_initialize(&text, {commands, 48}, {display, sizeof(display)},
                                                  {styles, 48}, {semantics, 8}, {measurements, 4},
                                                  {reveals, 2}, {scratch, 512}) ||
            !ui_scene_initialize_bootstrap(&shell) ||
            !guild_rail_scene_initialize(&guilds, {guild_rows, 1}, {guild_widgets, 1}, {guild_nodes, 1}) ||
            !shell_navigation_scene_initialize(&navigation, {navigation_rows, 1}, {navigation_widgets, 1},
                                               {navigation_nodes, 1}) ||
            !ui_widget_manager_initialize(&inspector_owner, {inspector_widgets, 1}, {inspector_nodes, 1}))
            return false;
        const auto added = ui_widget_manager_add(
            &inspector_owner, ui_widget_invalid_handle(),
            {1,
             UiWidgetKind::button,
             static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable),
             {utf8_literal("Poll voters"), ui_layout_style(), {}, 0, 1}});
        inspector_root = added.handle;
        for (u32 i = 0; i < 25; ++i) {
            voters[i].id = 1000 + i;
            voters[i].name = utf8_literal("Original participant");
        }
        return added.ok();
    }
    void release() noexcept {
        message_scene_release(&messages);
        message_poll_store_release(&polls);
        ui_scene_release(&shell);
    }
};
// Read the current draw before Present. No screenshots or output buffers survive
// this map, and the inherited capture environment is deliberately irrelevant.
bool ink(D3D11Renderer& r, const Theme& theme, UiRect rectangle, Color color, u32* pixels) noexcept {
    *pixels = 0;
    const float clear[]{theme.base_lowest.red, theme.base_lowest.green, theme.base_lowest.blue, 1};
    r.graphics.context->ClearRenderTargetView(r.graphics.target, clear);
    if (!require(d3d11_renderer_draw_stream(&r, theme, false, false, nullptr), 70))
        return false;
    ID3D11Resource* resource{};
    r.graphics.target->GetResource(&resource);
    ID3D11Texture2D* source{};
    const HRESULT queried =
        resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source));
    resource->Release();
    if (!require(SUCCEEDED(queried), 71))
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    if (!require((bgra || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) && desc.SampleDesc.Count == 1, 72)) {
        source->Release();
        return false;
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging{};
    const HRESULT created = r.graphics.device->CreateTexture2D(&desc, nullptr, &staging);
    if (!require(SUCCEEDED(created), 73)) {
        source->Release();
        return false;
    }
    r.graphics.context->CopyResource(staging, source);
    source->Release();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT read = r.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    bool valid = SUCCEEDED(read);
    if (valid) {
        const float scale = static_cast<float>(r.dpi) / 96;
        const auto left = static_cast<u32>(rectangle.left * scale),
                   top = static_cast<u32>(rectangle.top * scale);
        const auto right = static_cast<u32>(rectangle.right * scale),
                   bottom = static_cast<u32>(rectangle.bottom * scale);
        valid = right <= r.width && bottom <= r.height && right > left && bottom > top;
        const int expected[]{static_cast<int>(color.red * 255), static_cast<int>(color.green * 255),
                             static_cast<int>(color.blue * 255)};
        for (u32 y = top; valid && y < bottom; ++y) {
            const auto* row = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch;
            for (u32 x = left; x < right; ++x) {
                const auto* pixel = row + x * 4;
                const int channels[]{pixel[bgra ? 2 : 0], pixel[1], pixel[bgra ? 0 : 2]};
                bool match = true;
                for (u32 c = 0; c < 3; ++c) {
                    const int delta = channels[c] - expected[c];
                    match = match && delta >= -24 && delta <= 24;
                }
                *pixels += match ? 1u : 0u;
            }
        }
        r.graphics.context->Unmap(staging, 0);
    }
    staging->Release();
    return require(valid, 74);
}
} // namespace
bool poll_scene_ui_probe(D3D11Renderer& renderer) noexcept {
    if (!require(renderer.graphics.device && renderer.graphics.context && renderer.graphics.target &&
                     renderer.message_measure_cache && renderer.content_text && renderer.chat_state &&
                     renderer.navigation_view,
                 1))
        return false;
    const Borrowed old{
#define SAVE(name) renderer.name,
        POLL_BINDINGS(SAVE)
#undef SAVE
    };
    const auto dpi = renderer.dpi;
    const bool login = renderer.login_visible;
    const auto original_width = renderer.width, original_height = renderer.height;
    Fixture f{};
    bool valid = require(f.initialize(renderer.message_measure_cache), 2);
#define CLEAR(name) renderer.name = nullptr;
    POLL_BINDINGS(CLEAR)
#undef CLEAR
    renderer.login_visible = false;
    renderer.ui_scene = &f.shell;
    renderer.message_scene = &f.messages;
    renderer.message_text = &f.text;
    renderer.guild_rail_scene = &f.guilds;
    renderer.shell_navigation_scene = &f.navigation;
    for (u32 sample = 0; valid && sample < 2; ++sample) {
        const bool compact = sample == 1;
        const Theme& theme = compact ? kLightTheme : kDefaultTheme;
        renderer_set_dpi(&renderer, compact ? 144 : 96);
        const float scale = static_cast<float>(renderer.dpi) / 96;
        // A focused fixture keeps the complete wrapped card and footer visible.
        // Restore the original native dimensions before returning.
        valid = require(renderer_resize(&renderer, original_width,
                                        maximum(original_height, static_cast<u32>(720 * scale))),
                        17);
        if (!valid)
            break;
        const float width = static_cast<float>(renderer.width) / scale,
                    height = static_cast<float>(renderer.height) / scale;
        auto metrics = kGuildChannelShellLayoutMetrics;
        metrics.flags = compact ? static_cast<u16>(ui_shell_layout_navigation) : u16{};
        metrics.navigation_min_viewport_width = 0;
        metrics.navigation_width = compact ? width - metrics.guild_rail_width - 248 : 0;
        metrics.member_sidebar_width = metrics.composer_height = metrics.message_bottom_margin = 0;
        valid = require(
            ui_scene_layout_with_metrics(&f.shell, UiShellView::guild_channel, metrics, width, height), 3);
        const auto viewport = ui_scene_content_rectangle(f.shell, UiSemantic::message_view);
        valid =
            valid && require(message_scene_update(&f.messages, viewport).ok() && f.messages.count == 1, 4);
        UiPollProjection static_card{};
        valid = valid && require(message_scene_poll_projection(f.messages, f.messages.rows[0], theme,
                                                               {f.answers, 10}, &static_card),
                                 5);
        if (!valid)
            break;
        auto state = static_card.state;
        state.results = compact;
        f.answers[0].selected = true;
        // The wide sample is a ballot (no recorded vote: Vote beside the "Show
        // results" text action); the compact one keeps the fixture's own vote,
        // so it shows results with Remove Vote in the primary slot.
        f.answers[0].me_voted = compact;
        const UiWidgetReference root{&f.messages.widgets, f.messages.rows[0].poll};
        valid = require(ui_poll_scene_project(&f.card, state, static_card.geometry.bounds, viewport, root,
                                              UiPollSourceSurface::timeline, kDefaultUiPollStyle, theme,
                                              renderer.message_measure_cache, 1),
                        6);
        valid = valid && require(!compact || (ui_rect_width(f.card.projection.geometry.bounds) >= 151 &&
                                              ui_rect_width(f.card.projection.geometry.bounds) <= 153 &&
                                              ui_rect_height(f.card.projection.geometry.question) > 24),
                                 7);
        valid = valid && require(message_text_presentation_build(
                                     &f.text, f.messages, *renderer.chat_state, renderer.user_store,
                                     *renderer.navigation_view, theme, 0)
                                         .ok() &&
                                     renderer_bind_poll_scene(&renderer, &f.card) &&
                                     d3d11_renderer_poll_scene_visible(renderer) &&
                                     renderer_rebuild_content_text(&renderer, theme),
                                 8);
        renderer.retained_presentation.valid = false;
        valid = valid && require(probe_present(&renderer, theme), 9);
        const auto& card = f.card.projection;
        const auto& g = card.geometry;
        const auto& style = kDefaultUiPollStyle;
        const auto same_dip = [](float a, float b) noexcept { return a - b < .01f && b - a < .01f; };
        // The official card: equal answer rows of at least 50 DIPs, 8 apart, the
        // label at the 16-DIP inline padding and the 24-DIP selection icon
        // right-aligned inside it.
        bool rows = same_dip(g.answers[1].bounds.top, g.answers[0].bounds.bottom + style.answer_gap);
        for (u32 i = 0; i < 2; ++i) {
            const auto& a = g.answers[i];
            rows = rows && ui_rect_height(a.bounds) >= style.answer_minimum_height &&
                   same_dip(ui_rect_height(a.bounds), ui_rect_height(g.answers[0].bounds)) &&
                   same_dip(ui_rect_width(a.marker), style.marker) &&
                   same_dip(a.marker.right, a.bounds.right - style.answer_padding) &&
                   same_dip(a.text.left, a.bounds.left + style.answer_padding) &&
                   a.text.right <= a.marker.left;
        }
        valid = valid && require(rows, 22);
        // Footer: the ballot's enabled Vote sits right of "Show results"; with
        // results and an own vote, Remove Vote alone takes that primary slot.
        // Either way the primary action is a 38-DIP button at the padding edge.
        const UiPollTarget vote_target{UiPollControl::vote}, remove_target{UiPollControl::remove_vote},
            show_target{UiPollControl::results};
        const UiRect primary = compact ? g.remove_vote : g.vote;
        const bool footer =
            (compact
                 ? ui_poll_target_enabled(card, remove_target) &&
                       !ui_poll_target_visible(card, vote_target) &&
                       !ui_poll_target_visible(card, show_target)
                 : ui_poll_target_enabled(card, vote_target) && ui_poll_target_enabled(card, show_target) &&
                       !card.results_primary && !ui_poll_target_visible(card, remove_target) &&
                       g.results.right <= g.vote.left) &&
            same_dip(primary.right, g.bounds.right - style.padding) &&
            same_dip(ui_rect_height(primary), style.target) && primary.top >= g.answers[1].bounds.bottom;
        valid = valid && require(footer, 23);
        u32 question_pixels{}, footer_pixels{}, show_pixels{}, marker_pixels{};
        // Vote's label is drawn on the brand fill; Remove Vote and "Show
        // results" use the primary text colour. The radio's brand dot and the
        // own-vote check both fill with the brand accent.
        if (valid)
            valid = ink(renderer, theme, g.question, card.foreground, &question_pixels) &&
                    ink(renderer, theme, primary, compact ? card.foreground : card.primary_foreground,
                        &footer_pixels) &&
                    (compact || ink(renderer, theme, g.results, card.foreground, &show_pixels)) &&
                    ink(renderer, theme, g.answers[0].marker, card.accent, &marker_pixels);
        log_u64(LogLevel::info, "probe", "poll_native_question_pixels", question_pixels);
        log_u64(LogLevel::info, "probe", "poll_native_footer_pixels", footer_pixels);
        log_u64(LogLevel::info, "probe", "poll_native_show_results_pixels", show_pixels);
        log_u64(LogLevel::info, "probe", "poll_native_marker_pixels", marker_pixels);
        valid = require(valid && question_pixels > 3 && footer_pixels > 3 && (compact || show_pixels > 3) &&
                            marker_pixels > 3 && renderer.paint_stream.count < 512 &&
                            f.messages.widgets.count == kMessageSceneBaseWidgetsPerRow + 2,
                        10);
        const auto live = f.card.owner.handle;
        ++f.card.owner.handle.generation;
        valid = valid && require(!d3d11_renderer_poll_scene_visible(renderer) &&
                                     !ui_poll_scene_key(&f.card, UiPollKey::activate).action_requested,
                                 11);
        f.card.owner.handle = live;
        UiPollVotersState voters{};
        voters.owner = 4400;
        voters.generation = voters.revision = voters.content_revision = 1;
        voters.question = state.question;
        voters.answer_id = 7;
        voters.answers = {f.answers, 2};
        voters.voters = {f.voters, 25};
        voters.has_next = true;
        valid = valid && require(ui_poll_voters_scene_project(&f.inspector, voters, {0, 0, width, height},
                                                              {&f.inspector_owner, f.inspector_root},
                                                              kDefaultUiPollVotersStyle, theme,
                                                              renderer.message_measure_cache, 1),
                                 12);
        valid = valid && require(renderer_bind_poll_voters_scene(&renderer, &f.inspector) &&
                                     d3d11_renderer_poll_voters_visible(renderer) &&
                                     !d3d11_renderer_poll_scene_visible(renderer) &&
                                     renderer_rebuild_content_text(&renderer, theme),
                                 13);
        renderer.focus_owner = &f.messages.widgets;
        valid = valid && require(!d3d11_renderer_visible_focus_owner(renderer), 18);
        renderer.focus_owner = &f.inspector_owner;
        valid = valid && require(d3d11_renderer_visible_focus_owner(renderer) == &f.inspector_owner, 19);
        UiTooltipScene tooltip{};
        const UiTooltipTarget tooltip_target{{root, 1, 1, 1, 1},
                                             f.card.projection.geometry.question,
                                             utf8_literal("Poll answer"),
                                             UiTooltipPlacement::above};
        valid =
            valid &&
            require(
                ui_tooltip_offer(&tooltip, tooltip_target, viewport, UiTooltipTrigger::pointer, 0) &&
                    ui_tooltip_bind_text_owner(&tooltip, {&f.messages.widgets, f.messages.root}) &&
                    ui_tooltip_tick(&tooltip, kUiTooltipPointerDelayMs, renderer.message_measure_cache) &&
                    ui_tooltip_visible(tooltip),
                20);
        renderer.tooltip_scene = &tooltip;
        valid = valid && require(!d3d11_renderer_tooltip_visible(renderer), 21);
        renderer.retained_presentation.valid = false;
        valid = valid && require(probe_present(&renderer, theme), 14);
        u32 voter_pixels{};
        const auto row = ui_poll_voters_row_rectangle(f.inspector, 0);
        const UiRect name{row.left + f.inspector.style.avatar_extent + f.inspector.style.gap, row.top + 4,
                          row.right, row.bottom - 4};
        valid = valid && ink(renderer, theme, name, f.inspector.foreground, &voter_pixels);
        log_u64(LogLevel::info, "probe", "poll_native_voter_pixels", voter_pixels);
        valid = require(valid && voter_pixels > 3 && f.inspector.visible <= 25 &&
                            renderer.paint_stream.clip_depth == 1,
                        15);
        renderer_bind_poll_voters_scene(&renderer, nullptr);
        renderer.tooltip_scene = nullptr;
        renderer.focus_owner = nullptr;
    }
#define RESTORE(name) renderer.name = old.name;
    POLL_BINDINGS(RESTORE)
#undef RESTORE
    renderer.login_visible = login;
    renderer_set_dpi(&renderer, dpi);
    const bool resized_back = renderer_resize(&renderer, original_width, original_height);
    renderer.retained_presentation.valid = false;
    content_text_retention_invalidate(&renderer.content_text_retention);
    const bool restored = resized_back && renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                          probe_present(&renderer, kDefaultTheme);
    f.release();
    valid = require(restored, 16) && valid;
    log_write(valid ? LogLevel::info : LogLevel::error, "probe",
              valid ? "poll_native_renderer_no_output_passed" : "poll_native_renderer_no_output_failed");
    return valid;
}
#undef POLL_BINDINGS
} // namespace maicord
