#include "../../support/test_main_shared.hpp"

#include "../../support/ui_scene_fixtures.hpp"
#include "maicord/model/guild/member_store.hpp"
#include "../../support/layout_validation.hpp"
#include "../../../tools/font_reference/native_run_reference.hpp"
#include "maicord/app/guild/member_list_channel_binding.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "maicord/ui/account/profile_media.hpp"
#include "maicord/render/text/glyph_quad_bounds.hpp"
#include "maicord/render/text/glyph_raster.hpp"
#include "../../support/message_background_surfaces.inc"
#include "maicord/ui/chat/composer/message_composer_scene.hpp"
#include "maicord/ui/guild/member_list_scene.hpp"
#include "maicord/ui/chat/messages/mention_badge.hpp"
#include "maicord/ui/plugins/slot_surfaces.hpp"
#include "maicord/ui/voice/panel.hpp"
#include "maicord/ui/shell/popup_presentation.hpp"
#include "maicord/ui/chat/messages/text_presentation.hpp"
#include "maicord/ui/layout/scroll_host.hpp"
#include "maicord/ui/shell/presentation.hpp"
#include "maicord/ui/text/flow_layout.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/model/guild/permission_bits.hpp"
#include "maicord/render/text/glyph_atlas.hpp"

namespace maicord {
MessageRichRecord g_message_scene_rich_records[32];
u16 g_message_scene_rich_order[32];
u8 g_message_scene_rich_text[16 * 1024];

namespace {

#pragma float_control(precise, on, push)
__declspec(noinline) bool reference_coordinate_valid(float value) noexcept {
    return value == value && value >= -kMaximumUiCoordinate && value <= kMaximumUiCoordinate;
}
__declspec(noinline) bool reference_rectangle_valid(const UiRect& r) noexcept {
    const float width = r.right - r.left, height = r.bottom - r.top;
    return reference_coordinate_valid(r.left) && reference_coordinate_valid(r.top) &&
           reference_coordinate_valid(r.right) && reference_coordinate_valid(r.bottom) &&
           r.right >= r.left && r.bottom >= r.top &&
           width == width && width >= 0 && width <= kMaximumUiCoordinate &&
           height == height && height >= 0 && height <= kMaximumUiCoordinate;
}
#pragma float_control(pop)

} // namespace

void test_paint_stream() noexcept {
    static_assert(ui_coordinate_valid(kMaximumUiCoordinate) && ui_coordinate_valid(-kMaximumUiCoordinate));
    const u32 coordinate_patterns[]{0, 0x80000000u, 1, 0x80000001u, 0x49800000u, 0xc9800000u,
                                    0x49800001u, 0xc9800001u, 0x7f800000u, 0xff800000u,
                                    0x7fc00000u, 0xffc00000u, 0x7f800001u, 0xffffffffu};
    bool equivalent = true;
    for (u32 bits : coordinate_patterns) {
        const float value = __builtin_bit_cast(float, bits);
        equivalent = equivalent && ui_coordinate_valid(value) == reference_coordinate_valid(value) &&
                     paint_coordinate_valid(value) == reference_coordinate_valid(value);
    }
    u32 random = 0x5314ab91u;
    for (u32 i = 0; i < 65536; ++i) {
        random = random * 1664525u + 1013904223u;
        const float value = __builtin_bit_cast(float, random);
        equivalent = equivalent && ui_coordinate_valid(value) == reference_coordinate_valid(value) &&
                     paint_coordinate_valid(value) == reference_coordinate_valid(value);
    }
    expect(equivalent, "coordinate validation preserves IEEE range boundaries, signed zero, infinities and NaNs");
    bool rectangles = true;
    for (u32 first : coordinate_patterns) {
        for (u32 last : coordinate_patterns) {
            const float a = __builtin_bit_cast(float, first), b = __builtin_bit_cast(float, last);
            const UiRect wide{a, 0, b, 1}, tall{0, a, 1, b};
            rectangles = rectangles && ui_rect_valid_or_empty(wide) == reference_rectangle_valid(wide) &&
                         ui_rect_valid_or_empty(tall) == reference_rectangle_valid(tall);
        }
    }
    expect(rectangles, "rectangle validation preserves empty, reversed, oversized and non-finite bounds");
    PaintStream stream{};
    expect(paint_stream_reset(&stream, 100.0f, 80.0f) && stream.quad_capacity == kInitialPaintQuads,
           "paint_stream_demand_commits_initial_geometry");
    PaintQuad* const stable_quads = stream.quads;
    expect(paint_stream_push_rounded_border(&stream, 0, 0, 10, 6, kDefaultTheme.accent,
                                            kDefaultTheme.interactive, 20.0f, 20.0f) &&
               stream.quads[0].radius == 3.0f && stream.quads[0].border_width == 3.0f &&
               stream.batch_count == 1 && stream.batches[0].quad_count == 1,
           "paint_stream_rounded_border_clamp");
    expect(paint_stream_push(&stream, 4, 4, 4, 9, kDefaultTheme.accent), "paint_stream_ignore_empty");
    expect(stream.count == 1, "paint_stream_empty_not_counted");

    expect(paint_stream_push_clip(&stream, 10, 10, 90, 70) &&
               paint_stream_push(&stream, 20, 20, 30, 30, kDefaultTheme.accent) &&
               paint_stream_push(&stream, 0, 0, 5, 5, kDefaultTheme.accent) && stream.count == 2 &&
               stream.clip_count == 2 && stream.batch_count == 2 && stream.batches[1].clip_index == 1,
           "paint_stream_clip_batch_and_cull");
    expect(paint_stream_push_clip(&stream, 0, 0, 20, 20) && stream.clips[2].left == 10.0f &&
               stream.clips[2].top == 10.0f && stream.clips[2].right == 20.0f &&
               stream.clips[2].bottom == 20.0f && paint_stream_pop_clip(&stream) &&
               paint_stream_pop_clip(&stream),
           "paint_stream_nested_clip_intersection");
    expect(!paint_stream_pop_clip(&stream) && stream.rejected == 1, "paint_stream_reject_root_pop");
    expect(!paint_stream_push(&stream, -kMaximumPaintCoordinate - 1.0f, 0, 1, 1, kDefaultTheme.accent) &&
               stream.rejected == 2,
           "paint_stream_coordinate_bound");

    // A layer marker closes the batch before it. The search field opens its
    // layer at the root clip: with the caret blinked off its clip holds no
    // quad, and the field and the popout below it must still start a new
    // batch rather than join the shell's last one (drawn under message text).
    bool layered = true;
    for (u32 caret = 0; caret < 2; ++caret) {
        layered = layered && paint_stream_reset(&stream, 100.0f, 80.0f) &&
                  paint_stream_push(&stream, 0, 0, 100, 80, kDefaultTheme.accent);
        const u32 overlay = paint_stream_seal(&stream);
        layered = layered && paint_stream_push(&stream, 10, 4, 60, 14, kDefaultTheme.accent) &&
                  paint_stream_push_clip(&stream, 12, 6, 58, 12) &&
                  (!caret || paint_stream_push(&stream, 20, 6, 21, 12, kDefaultTheme.accent)) &&
                  paint_stream_pop_clip(&stream) &&
                  paint_stream_push(&stream, 10, 16, 70, 60, kDefaultTheme.accent) && overlay == 1 &&
                  stream.batches[0].quad_count == 1 && stream.batches[1].first_quad == 1 &&
                  stream.batch_count == (caret ? 4u : 2u) && stream.count == (caret ? 4u : 3u);
        // Quads after the layer's head share batches again.
        layered = layered && paint_stream_push(&stream, 12, 18, 30, 30, kDefaultTheme.accent) &&
                  stream.batch_count == (caret ? 4u : 2u) && paint_stream_seal(&stream) == stream.batch_count;
    }
    expect(layered, "paint_stream_seal_starts_a_layer_whether_the_caret_is_drawn_or_not");

    paint_stream_reset(&stream);
    bool filled = true;
    while (filled && stream.count < kMaxPaintQuads) {
        filled = paint_stream_push(&stream, 0, 0, 1, 1, kDefaultTheme.accent);
    }
    expect(filled && stream.quads == stable_quads && stream.quad_capacity == kMaxPaintQuads,
           "paint_stream_growth_preserves_geometry_address");
    expect(!paint_stream_push(&stream, 0, 0, 1, 1, kDefaultTheme.accent), "paint_stream_reject_overflow");
    expect(stream.rejected == 1, "paint_stream_overflow_counter");
    paint_stream_release(&stream);
    expect(!stream.quads && !stream.quad_storage.arena.base && stream.quad_capacity == 0,
           "paint_stream_release");
}

void test_ui_scene() noexcept {
    UiScene scene{};
    expect(ui_scene_initialize_bootstrap(&scene) &&
               ui_scene_layout_with_metrics(&scene, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics,
                                            1280.0f, 800.0f),
           "ui_scene_bootstrap_layout");
    const UiLayoutNode* guild = ui_scene_node(scene, UiSemantic::guild_rail);
    const UiLayoutNode* channels = ui_scene_node(scene, UiSemantic::channel_sidebar);
    const UiLayoutNode* content = ui_scene_node(scene, UiSemantic::content);
    const UiLayoutNode* members = ui_scene_node(scene, UiSemantic::member_sidebar);
    const UiLayoutNode* composer = ui_scene_node(scene, UiSemantic::composer);
    const UiLayoutNode* messages = ui_scene_node(scene, UiSemantic::message_view);
    expect(guild && guild->rectangle.left == 0.0f && guild->rectangle.right == 64.0f && channels &&
               channels->rectangle.left == 64.0f && channels->rectangle.right == 304.0f && content &&
               content->rectangle.left == 304.0f && content->rectangle.right == 1040.0f && members &&
               members->rectangle.left == 1040.0f && members->rectangle.top == 48.0f,
           "ui_scene_semantic_docks");
    expect(composer && composer->rectangle.left == 312.0f && composer->rectangle.top == 744.0f &&
               composer->rectangle.right == 1032.0f && composer->rectangle.bottom == 792.0f && messages &&
               messages->rectangle.top == 48.0f && messages->rectangle.bottom == 726.0f,
           "ui_scene_nested_margin_fill");
    const auto* footer = ui_scene_node(scene, UiSemantic::channel_footer);
    const auto* guild_list = ui_scene_node(scene, UiSemantic::guild_navigation);
    const auto* channel_list = ui_scene_node(scene, UiSemantic::channel_navigation);
    expect(footer && guild && channels && composer && guild_list && channel_list &&
               footer->rectangle.left - guild->rectangle.left == kUiFloatingSurfaceInset &&
               channels->rectangle.right - footer->rectangle.right == kUiFloatingSurfaceInset &&
               footer->rectangle.bottom == composer->rectangle.bottom &&
               ui_rect_height(footer->rectangle) == ui_rect_height(composer->rectangle) &&
               guild_list->rectangle.bottom == footer->rectangle.top - kUiFloatingSurfaceInset &&
               channel_list->rectangle.bottom == guild_list->rectangle.bottom,
           "ui_scene_account_surface_spans_both_columns_and_aligns_with_composer");
    // In a call the voice card above the account panel spans it too, and both
    // lists end above the card.
    UiScene call_scene{};
    UiShellLayoutMetrics call_metrics = kGuildChannelShellLayoutMetrics;
    call_metrics.navigation_footer_reserve = kUiVoicePanelHeight + kUiVoicePanelGap;
    bool call_layout = ui_scene_initialize_bootstrap(&call_scene) &&
                       ui_scene_layout_with_metrics(&call_scene, UiShellView::guild_channel, call_metrics, 1280.0f,
                                                    800.0f);
    if (call_layout) {
        const auto* call_footer = ui_scene_node(call_scene, UiSemantic::channel_footer);
        const auto* call_guilds = ui_scene_node(call_scene, UiSemantic::guild_navigation);
        const auto* call_channels =
            ui_scene_node(call_scene, UiSemantic::channel_navigation);
        const UiRect card = call_footer ? ui_voice_panel_rectangle(call_footer->rectangle) : UiRect{};
        call_layout = call_footer && call_guilds && call_channels && footer &&
                      call_footer->rectangle.left == footer->rectangle.left &&
                      call_footer->rectangle.right == footer->rectangle.right &&
                      call_footer->rectangle.top == footer->rectangle.top && card.left == footer->rectangle.left &&
                      card.right == footer->rectangle.right &&
                      call_guilds->rectangle.bottom == card.top - kUiFloatingSurfaceInset &&
                      call_channels->rectangle.bottom == call_guilds->rectangle.bottom;
    }
    UiShellLayoutMetrics bare_metrics = kGuildChannelShellLayoutMetrics;
    bare_metrics.flags = 0;
    const bool bare_layout =
        call_layout && ui_scene_layout_with_metrics(&call_scene, UiShellView::guild_channel, bare_metrics, 1280.0f,
                                                    800.0f);
    const auto* bare_guilds = ui_scene_node(call_scene, UiSemantic::guild_navigation);
    expect(bare_layout && bare_guilds && bare_guilds->rectangle.bottom == 800.0f &&
               !ui_scene_node(call_scene, UiSemantic::channel_footer),
           "ui_scene_voice_card_spans_both_columns_and_lists_end_above_it");
    ui_scene_release(&call_scene);
    // Speaking in a connected call rings the account avatar.
    PaintStream ring_paint{};
    UiVoicePanelView speaking_view{};
    speaking_view.state = UiVoicePanelState::connected;
    speaking_view.speaking = true;
    const UiRect account_avatar = footer ? ui_account_avatar_rectangle(footer->rectangle) : UiRect{};
    bool ring = footer && paint_stream_initialize(&ring_paint) && paint_stream_reset(&ring_paint, 1280, 800) &&
                account_avatar.left == footer->rectangle.left + 10 && ui_rect_width(account_avatar) == 32 &&
                ui_account_speaking_ring_paint(&ring_paint, &speaking_view, account_avatar, kDefaultTheme) &&
                ring_paint.count == 1 && ring_paint.quads[0].left == account_avatar.left - 2 &&
                ring_paint.quads[0].bottom == account_avatar.bottom + 2;
    speaking_view.state = UiVoicePanelState::connecting;
    ring = ring && ui_account_speaking_ring_paint(&ring_paint, &speaking_view, account_avatar, kDefaultTheme);
    speaking_view.state = UiVoicePanelState::connected;
    speaking_view.speaking = false;
    ring = ring && ui_account_speaking_ring_paint(&ring_paint, &speaking_view, account_avatar, kDefaultTheme) &&
           ui_account_speaking_ring_paint(&ring_paint, nullptr, account_avatar, kDefaultTheme) &&
           ring_paint.count == 1;
    paint_stream_release(&ring_paint);
    expect(ring, "account_avatar_speaking_ring_only_while_speaking_in_a_connected_call");
    UiLayoutDiagnostic scene_diagnostics[32]{};
    const u32 scene_rebuilds = scene.layout_rebuilds;
    const u32 scene_layout_passes = scene.layout_passes;
    const u16 scene_generation = scene.widgets.generation;
    expect(ui_layout_engine_validate(scene.widgets.layout, Slice<UiLayoutDiagnostic>{scene_diagnostics, 32}).clean() &&
               ui_scene_layout_with_metrics(&scene, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics,
                                            1280.0f, 800.0f) &&
               scene.layout_rebuilds == scene_rebuilds,
           "ui_scene_widget_validation_and_invalidation_cache");
    expect(ui_scene_layout_with_metrics(&scene, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics,
                                        1200.0f, 800.0f) &&
               scene.layout_rebuilds == scene_rebuilds &&
               scene.layout_passes == scene_layout_passes + 1 && scene.widgets.generation == scene_generation &&
               ui_scene_layout_with_metrics(&scene, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics,
                                            1280.0f, 800.0f) &&
               scene.layout_rebuilds == scene_rebuilds &&
               scene.layout_passes == scene_layout_passes + 2,
           "ui_scene_same_variant_rearrange_without_tree_rebuild");

    GuildViewRow guild_rows[1]{};
    guild_rows[0].id = 7;
    guild_rows[0].selected = true;
    guild_rows[0].mention_count = 2;
    ChannelViewRow channel_rows[2]{};
    channel_rows[0].id = 11;
    channel_rows[0].selected = true;
    channel_rows[0].selectable = true;
    channel_rows[0].mention_count = 1;
    channel_rows[0].actions = channel_action_send_messages;
    channel_rows[1].id = 12;
    channel_rows[1].type = 4;
    NavigationView navigation{};
    navigation.guild_rows = guild_rows;
    navigation.channel_rows = channel_rows;
    navigation.guild_capacity = 1;
    navigation.channel_capacity = 2;
    navigation.guild_count = 1;
    navigation.channel_count = 2;
    navigation.selected_channel_id = 11;
    float navigation_scroll = 500.0f;
    PaintStream shell_paint{};
    bool shell_paint_recorder_ready = true;
    ShellPresentationRegions shell_regions{};
    GuildRailScene chrome_guild_scene{};
    GuildRailRowWidget chrome_guild_rows[2]{};
    UiWidget chrome_guild_widgets[4]{};
    UiLayoutNode chrome_guild_layout[4]{};
    ShellNavigationScene chrome_navigation_scene{};
    ShellNavigationRowWidget chrome_navigation_rows[4]{};
    UiWidget chrome_navigation_widgets[18]{};
    UiLayoutNode chrome_navigation_layout[18]{};
    const UiLayoutNode* chrome_navigation_viewport =
        ui_scene_node(scene, UiSemantic::channel_navigation);
    const UiLayoutNode* chrome_guild_viewport =
        ui_scene_node(scene, UiSemantic::guild_navigation);
    const bool chrome_navigation_ready =
        guild_rail_scene_initialize(&chrome_guild_scene, Slice<GuildRailRowWidget>{chrome_guild_rows, 2},
                                    Slice<UiWidget>{chrome_guild_widgets, 4},
                                    Slice<UiLayoutNode>{chrome_guild_layout, 4}) &&
        chrome_guild_viewport &&
        guild_rail_scene_update(&chrome_guild_scene, &navigation, chrome_guild_viewport->rectangle, 0.0f)
            .ok() &&
        shell_navigation_scene_initialize(&chrome_navigation_scene,
                                          Slice<ShellNavigationRowWidget>{chrome_navigation_rows, 4},
                                          Slice<UiWidget>{chrome_navigation_widgets, 18},
                                          Slice<UiLayoutNode>{chrome_navigation_layout, 18}) &&
        chrome_navigation_viewport &&
        shell_navigation_scene_update(&chrome_navigation_scene, &navigation,
                                      chrome_navigation_viewport->rectangle, navigation_scroll)
            .ok();
    navigation_scroll = chrome_navigation_scene.scroll_dips;
    expect(chrome_navigation_ready && shell_paint_recorder_ready &&
               shell_presentation_build_chrome(scene, &navigation, &chrome_guild_scene,
                                               &chrome_navigation_scene, kDefaultTheme, 1280.0f, 800.0f,
                                               &shell_paint, &shell_regions) &&
               shell_paint.rejected == 0 && shell_paint.count >= 16 && navigation_scroll == 0.0f &&
               shell_regions.member_sidebar_visible && shell_regions.message_view.left == 304.0f &&
               shell_regions.message_view.right == 1040.0f,
           "shell_presentation_renderer_neutral_chrome");
    {
        // The selected channel's highlight takes radius_md.
        Theme square_theme = kDefaultTheme;
        square_theme.scale.radius_md = 0;
        PaintStream square_paint{};
        bool same_geometry = paint_stream_initialize(&square_paint) &&
            shell_presentation_build_chrome(scene, &navigation, &chrome_guild_scene,
                &chrome_navigation_scene, square_theme, 1280, 800, &square_paint, &shell_regions) &&
            square_paint.count == shell_paint.count;
        u32 changed_corners = 0;
        for (u32 i = 0; same_geometry && i < square_paint.count; ++i) {
            const auto& before = shell_paint.quads[i];
            const auto& after = square_paint.quads[i];
            same_geometry = ui_rect_equal({before.left,before.top,before.right,before.bottom},
                                          {after.left,after.top,after.right,after.bottom});
            if (before.radius != after.radius) {
                same_geometry &= before.radius <= kDefaultTheme.scale.radius_md && after.radius == 0;
                ++changed_corners;
            }
        }
        expect(same_geometry && changed_corners != 0,
               "shell_small_radius_theme_changes_corners_without_changing_geometry");
        paint_stream_release(&square_paint);
    }
    {
        // The channel list's bar through the shared pointer tracker: hovering the track widens it, a captured
        // press holds it wide and pressed while the drag leaves the track, and release or leaving shrinks it.
        ShellNavigationScene tall = chrome_navigation_scene;
        tall.content_height = ui_rect_height(tall.viewport) * 4;
        const auto bar = ui_scrollbar_geometry(tall.viewport, tall.scroll_dips, tall.content_height);
        PaintStream paint{};
        const auto& tokens = kDefaultTheme.tokens;
        const auto pill = [&](float width, const Color& fill) noexcept {
            if (!shell_presentation_build_chrome(scene, &navigation, &chrome_guild_scene, &tall, kDefaultTheme, 1280,
                                                 800, &paint, &shell_regions))
                return false;
            u32 found = 0;
            for (u32 i = 0; i < paint.count; ++i) {
                const auto& q = paint.quads[i];
                found += q.left == bar.thumb.right - width && q.right == bar.thumb.right && q.top == bar.thumb.top &&
                         q.bottom == bar.thumb.bottom && q.radius == width * .5f &&
                         bytes_equal(&q.fill, &fill, sizeof(Color));
            }
            return found == 1;
        };
        UiScrollbarPointer pointer{};
        const float x = (bar.track.left + bar.track.right) * .5f, y = (bar.thumb.top + bar.thumb.bottom) * .5f;
        const auto update = [&](UiScrollbarPointerEvent event, float px, float py, bool captured) noexcept {
            paint.scrollbar_hot = ui_scrollbar_pointer_update(&pointer, paint.scrollbar_tracks, event, px, py, captured);
            return paint.scrollbar_hot;
        };
        bool tracked = paint_stream_initialize(&paint) && bar.visible && pill(4, tokens.fill.muted);
        bool recorded = false;
        for (u32 i = 0; tracked && i < paint.scrollbar_tracks.count; ++i)
            recorded = recorded || ui_rect_equal(paint.scrollbar_tracks.tracks[i], bar.track);
        tracked = tracked && recorded &&
                  update(UiScrollbarPointerEvent::move, bar.track.left - 20, y, false).state ==
                      UiScrollbarVisualState::normal &&
                  pill(4, tokens.fill.muted);
        const auto hovered = update(UiScrollbarPointerEvent::move, x, y, false);
        tracked = tracked && hovered.state == UiScrollbarVisualState::hovered && ui_rect_equal(hovered.track, bar.track) &&
                  pill(8, tokens.state.pressed);
        // A press the owner did not capture (a page click) stays hovered.
        tracked = tracked && update(UiScrollbarPointerEvent::press, x, y, false).state == UiScrollbarVisualState::hovered;
        tracked = tracked && update(UiScrollbarPointerEvent::press, x, y, true).state == UiScrollbarVisualState::pressed &&
                  pill(8, tokens.text.muted) &&
                  update(UiScrollbarPointerEvent::move, bar.track.left - 200, bar.track.bottom + 50, true).state ==
                      UiScrollbarVisualState::pressed &&
                  pill(8, tokens.text.muted);
        tracked = tracked && update(UiScrollbarPointerEvent::release, bar.track.left - 200, y, false).state ==
                                 UiScrollbarVisualState::normal &&
                  pill(4, tokens.fill.muted);
        // Capture lost mid-drag over the track falls back to hover; leaving the window clears it.
        tracked = tracked && update(UiScrollbarPointerEvent::press, x, y, true).state == UiScrollbarVisualState::pressed &&
                  update(UiScrollbarPointerEvent::move, x, y, false).state == UiScrollbarVisualState::hovered &&
                  update(UiScrollbarPointerEvent::leave, x, y, false).state == UiScrollbarVisualState::normal &&
                  pill(4, tokens.fill.muted);
        expect(tracked, "shell_channel_scrollbar_widens_on_hover_and_holds_pressed_through_shared_tracker");
        paint_stream_release(&paint);
    }

    char shell_composer_text[64]{};
    MessageComposer shell_composer_model{};
    MessageComposerScene shell_composer_scene{};
    const bool shell_composer_ready =
        composer &&
        message_composer_initialize(&shell_composer_model,
                                    Slice<char>{shell_composer_text, sizeof(shell_composer_text)}) &&
        (message_composer_reset(&shell_composer_model, 11, false), true) &&
        message_composer_insert_utf8(&shell_composer_model, utf8_literal("draft")) ==
            MessageComposerResult::changed &&
        message_composer_set_selection(&shell_composer_model, 1, 4) == MessageComposerResult::changed &&
        message_composer_scene_initialize_owned(&shell_composer_scene) &&
        message_composer_scene_update(&shell_composer_scene, &shell_composer_model, composer->rectangle,
                                      true)
            .ok();
    shell_composer_model.focused = true;
    const UiLayoutNode* shell_composer_edit =
        message_composer_scene_layout_node(shell_composer_scene, MessageComposerControl::edit);
    const UiRect edit_content = shell_composer_edit ? shell_composer_edit->content_rectangle : UiRect{};
    const UiRect selection_rect{edit_content.left + 2.0f, edit_content.top + 2.0f,
                                edit_content.left + 22.0f, edit_content.top + 18.0f};
    const MessageComposerPresentationInput shell_composer_presentation{
        ConstSlice<UiRect>{&selection_rect, 1},
        UiRect{edit_content.left + 24.0f, edit_content.top + 2.0f, edit_content.left + 25.0f,
               edit_content.top + 18.0f},
        true, true};
    UiLayoutDiagnostic shell_composer_diagnostics[8]{};
    const UiLayoutValidationResult shell_composer_validation = ui_layout_engine_validate(
        shell_composer_scene.widgets.layout, Slice<UiLayoutDiagnostic>{shell_composer_diagnostics, 8});
    paint_stream_reset(&shell_paint, 1280.0f, 800.0f);
    const bool shell_composer_painted =
        shell_composer_ready && shell_composer_edit &&
        shell_presentation_build_chrome(scene, &navigation, &chrome_guild_scene, &chrome_navigation_scene,
                                        &shell_composer_scene, nullptr, kDefaultTheme, 1280.0f, 800.0f,
                                        &shell_paint, &shell_regions, &shell_composer_presentation) &&
        shell_presentation_build_composer_overlay(&shell_composer_scene, shell_composer_presentation, kDefaultTheme, &shell_paint, 96);
    const PaintQuad* painted_caret =
        shell_paint.count >= 1 ? &shell_paint.quads[shell_paint.count - 1] : nullptr;
    expect(shell_composer_painted && shell_paint.rejected == 0 && shell_composer_validation.clean() &&
               painted_caret &&
               painted_caret->right - painted_caret->left == 1.0f &&
               painted_caret->bottom > painted_caret->top,
           "shell_composer_selection_caret_paint_and_layout_validate");
    bool crisp_carets = true;
    const u32 caret_dpis[] = {96, 120, 144, 192};
    for (u32 dpi : caret_dpis) {
        const float scale = dpi / 96.0f;
        for (u32 step = 0; step < 20; ++step) {
            const auto snapped = caret_pixel_rectangle({12.1f + step * .17f, 3.2f, 13.1f + step * .17f, 22.6f},
                {0, 0, 100, 40}, dpi);
            const float width = ui_rect_width(snapped) * scale;
            const float x = snapped.left * scale;
            crisp_carets &= width > .9999f && width < 1.0001f &&
                x - static_cast<u32>(x + .0001f) < .0001f;
        }
    }
    expect(crisp_carets, "caret_is_one_physical_pixel_at_fractional_positions_and_all_supported_DPI_scales");

    const bool composer_banners =
        message_composer_scene_update(&shell_composer_scene, &shell_composer_model,
                                      {composer->rectangle.left, composer->rectangle.top - 80,
                                       composer->rectangle.right, composer->rectangle.bottom},
                                      true, 72, utf8_literal("Replying to user - click to cancel"), nullptr,
                                      {}, {utf8_literal("image.png · Ready"), true, true})
            .ok();
    Theme banner_theme = kDefaultTheme;
    banner_theme.background = background_theme_defaults();
    bool themed_banners = composer_banners;
    const UiWidgetHandle banner_handles[] = {shell_composer_scene.reply, shell_composer_scene.attachments};
    for (const auto handle : banner_handles) {
        const auto* node = ui_widget_manager_layout_node(shell_composer_scene.widgets, handle);
        shell_composer_scene.widgets.hovered = handle;
        paint_stream_reset(&shell_paint, 1280, 800);
        bool found = false;
        themed_banners = themed_banners && shell_presentation_build_chrome(
                                               scene, &navigation, &chrome_guild_scene,
                                               &chrome_navigation_scene, &shell_composer_scene, nullptr,
                                               banner_theme, 1280, 800, &shell_paint, &shell_regions);
        for (u32 i = 0; node && i < shell_paint.count; ++i) {
            const auto& q = shell_paint.quads[i];
            if (ui_rect_equal({q.left, q.top, q.right, q.bottom}, node->rectangle))
                found = q.border_width == -7 && q.reserved_y > 0;
        }
        themed_banners = themed_banners && found;
    }
    expect(themed_banners, "composer_reply_and_attachment_banners_retain_gradient_when_hovered");

    GuildViewRow virtual_guild_rows[256]{};
    for (u32 index = 0; index < 256; ++index) {
        GuildViewRow& row = virtual_guild_rows[index];
        row.id = 50'000 + index;
        row.revision = 1;
        row.generation = 1;
        row.selected = index == 12;
        row.unavailable = (index % 31u) == 0;
        row.unread = (index % 5u) == 0;
        row.mention_count = (index % 13u) == 0 ? 1 : 0;
    }
    NavigationView virtual_guild_navigation{};
    virtual_guild_navigation.guild_rows = virtual_guild_rows;
    virtual_guild_navigation.guild_capacity = 256;
    virtual_guild_navigation.guild_count = 256;
    virtual_guild_navigation.selected_guild_id = 50'012;
    virtual_guild_navigation.revision = 1;
    virtual_guild_navigation.guild_revision = 1;
    GuildRailScene virtual_guild_scene{};
    GuildRailRowWidget virtual_guild_widget_rows[24]{};
    UiWidget virtual_guild_widgets[26]{};
    UiLayoutNode virtual_guild_layout[26]{};
    const UiRect virtual_guild_viewport{10.0f, 20.0f, 82.0f, 240.0f};
    const bool virtual_guild_initialized = guild_rail_scene_initialize(
        &virtual_guild_scene, Slice<GuildRailRowWidget>{virtual_guild_widget_rows, 24},
        Slice<UiWidget>{virtual_guild_widgets, 26}, Slice<UiLayoutNode>{virtual_guild_layout, 26});
    const GuildRailSceneResult virtual_guild_first = guild_rail_scene_update(
        &virtual_guild_scene, &virtual_guild_navigation, virtual_guild_viewport, 600.0f);
    const GuildRailRowWidget* virtual_guild_hit_row = nullptr;
    const UiLayoutNode* virtual_guild_hit_layout = nullptr;
    for (u32 index = 0; index < virtual_guild_scene.count; ++index) {
        const GuildRailRowWidget& candidate = virtual_guild_scene.rows[index];
        const UiLayoutNode* layout = guild_rail_scene_layout_node(virtual_guild_scene, candidate);
        if (!candidate.view.unavailable && layout && layout->rectangle.top >= virtual_guild_viewport.top &&
            layout->rectangle.bottom <= virtual_guild_viewport.bottom) {
            virtual_guild_hit_row = &candidate;
            virtual_guild_hit_layout = layout;
            break;
        }
    }
    const GuildRailHit virtual_guild_hit =
        virtual_guild_hit_layout
            ? guild_rail_scene_hit_test(
                  virtual_guild_scene,
                  (virtual_guild_hit_layout->rectangle.left + virtual_guild_hit_layout->rectangle.right) *
                      0.5f,
                  (virtual_guild_hit_layout->rectangle.top + virtual_guild_hit_layout->rectangle.bottom) *
                      0.5f)
            : GuildRailHit{};
    const UiWidgetHandle virtual_guild_retained_handle =
        virtual_guild_hit_row ? virtual_guild_hit_row->widget : ui_widget_invalid_handle();
    const u64 virtual_guild_retained_id = virtual_guild_hit_row ? virtual_guild_hit_row->view.id : 0;
    const u32 virtual_guild_retained_source =
        virtual_guild_hit_row ? virtual_guild_hit_row->source_index : 0;
    const bool virtual_guild_focused =
        virtual_guild_retained_id != 0 &&
        guild_rail_scene_focus_guild(&virtual_guild_scene, virtual_guild_retained_id);
    const u32 guild_rebuilds_before_data = virtual_guild_scene.rebuilds;
    const u32 guild_layouts_before_data = virtual_guild_scene.layout_passes;
    virtual_guild_rows[virtual_guild_retained_source].mention_count += 1;
    virtual_guild_rows[virtual_guild_retained_source].revision += 1;
    virtual_guild_navigation.revision += 1;
    virtual_guild_navigation.guild_revision += 1;
    const GuildRailSceneResult virtual_guild_data_update = guild_rail_scene_update(
        &virtual_guild_scene, &virtual_guild_navigation, virtual_guild_viewport, 600.0f);
    const GuildRailRowWidget* guild_after_data =
        guild_rail_scene_resolve(virtual_guild_scene, virtual_guild_retained_handle);
    const GuildRailSceneResult virtual_guild_reflow = guild_rail_scene_update(
        &virtual_guild_scene, &virtual_guild_navigation, virtual_guild_viewport, 601.0f);
    const GuildRailRowWidget* guild_after_reflow =
        guild_rail_scene_resolve(virtual_guild_scene, virtual_guild_retained_handle);
    UiLayoutDiagnostic virtual_guild_diagnostics[16]{};
    const UiLayoutValidationResult virtual_guild_structure = ui_layout_engine_validate(
        virtual_guild_scene.widgets.layout, Slice<UiLayoutDiagnostic>{virtual_guild_diagnostics, 16});
    const UiWidget* virtual_guild_content_widget =
        guild_after_reflow
            ? ui_widget_manager_resolve(virtual_guild_scene.widgets, guild_after_reflow->widget)
            : nullptr;
    const UiLayoutNode* virtual_guild_content_layout =
        guild_after_reflow ? guild_rail_scene_layout_node(virtual_guild_scene, *guild_after_reflow)
                           : nullptr;
    const UiLayoutContentBounds virtual_guild_content{
        virtual_guild_content_widget
            ? ui_widget_manager_layout_handle(virtual_guild_scene.widgets, guild_after_reflow->widget)
            : ui_layout_invalid_handle(),
        virtual_guild_content_layout ? virtual_guild_content_layout->rectangle : UiRect{},
        UiLayoutContentKind::control, ui_layout_content_bounds_collision};
    const UiLayoutValidationResult virtual_guild_emitted = ui_layout_engine_validate_content_bounds(
        virtual_guild_scene.widgets.layout, ConstSlice<UiLayoutContentBounds>{&virtual_guild_content, 1},
        Slice<UiLayoutDiagnostic>{virtual_guild_diagnostics, 16});
    expect(virtual_guild_initialized && virtual_guild_first.ok() && virtual_guild_first.first != 0 &&
               virtual_guild_first.count < 24 &&
               virtual_guild_scene.widgets.layout.storage[0].rectangle.left ==
                   virtual_guild_viewport.left &&
               virtual_guild_hit.id == virtual_guild_retained_id && virtual_guild_focused &&
               virtual_guild_data_update.ok() && guild_after_data &&
               guild_after_data->view.mention_count ==
                   virtual_guild_rows[virtual_guild_retained_source].mention_count &&
               virtual_guild_scene.rebuilds == guild_rebuilds_before_data &&
               virtual_guild_scene.data_updates == 1 &&
               guild_layouts_before_data + 1 == virtual_guild_scene.layout_passes &&
               virtual_guild_reflow.ok() && guild_after_reflow && virtual_guild_content_layout &&
               virtual_guild_content_layout->rectangle.left == virtual_guild_viewport.left + 16.0f &&
               ui_rect_width(virtual_guild_content_layout->rectangle) == 40.0f &&
               virtual_guild_structure.clean() && virtual_guild_emitted.clean(),
           "guild_rail_virtual_rows_retain_data_identity_and_validate");
    const GuildRailSceneResult virtual_guild_displaced = guild_rail_scene_update(
        &virtual_guild_scene, &virtual_guild_navigation, virtual_guild_viewport, 8'000.0f);
    const bool virtual_guild_stale_rejected =
        !guild_rail_scene_resolve(virtual_guild_scene, virtual_guild_retained_handle);
    const GuildRailSceneResult virtual_guild_restored = guild_rail_scene_update(
        &virtual_guild_scene, &virtual_guild_navigation, virtual_guild_viewport, 601.0f);
    const GuildRailRowWidget* virtual_guild_restored_focus =
        guild_rail_scene_resolve(virtual_guild_scene, virtual_guild_scene.widgets.focused);
    expect(virtual_guild_displaced.ok() && virtual_guild_stale_rejected && virtual_guild_restored.ok() &&
               virtual_guild_restored_focus &&
               virtual_guild_restored_focus->view.id == virtual_guild_retained_id,
           "guild_rail_virtual_focus_restores_after_rematerialization");

    GuildRailScene tiny_guild_scene{};
    GuildRailRowWidget tiny_guild_rows[2]{};
    UiWidget tiny_guild_widgets[4]{};
    UiLayoutNode tiny_guild_layout[4]{};
    const bool tiny_guild_initialized = guild_rail_scene_initialize(
        &tiny_guild_scene, Slice<GuildRailRowWidget>{tiny_guild_rows, 2},
        Slice<UiWidget>{tiny_guild_widgets, 4}, Slice<UiLayoutNode>{tiny_guild_layout, 4});
    const GuildRailSceneResult tiny_guild_result = guild_rail_scene_update(
        &tiny_guild_scene, &virtual_guild_navigation, UiRect{0.0f, 0.0f, 72.0f, 800.0f}, 0.0f);
    expect(tiny_guild_initialized && tiny_guild_result.error == GuildRailSceneError::capacity &&
               tiny_guild_scene.widgets.count == 0 && tiny_guild_scene.count == 0,
           "guild_rail_capacity_failure_does_not_publish_partial_tree");

    ChannelViewRow virtual_channel_rows[128]{};
    for (u32 index = 0; index < 128; ++index) {
        ChannelViewRow& row = virtual_channel_rows[index];
        row.id = 10'000 + index;
        row.type = static_cast<u8>((index % 9u) == 0 ? 4 : 0);
        row.depth = static_cast<u8>(index % 3u);
        row.mention_count = (index % 11u) == 0 ? 1 : 0;
        row.selectable = row.type != 4;
    }
    NavigationView virtual_navigation{};
    virtual_navigation.channel_rows = virtual_channel_rows;
    virtual_navigation.channel_capacity = 128;
    virtual_navigation.channel_count = 128;
    virtual_navigation.revision = 1;
    virtual_navigation.channel_revision = 1;
    ShellNavigationScene virtual_navigation_scene{};
    ShellNavigationRowWidget virtual_navigation_widgets_rows[24]{};
    UiWidget virtual_navigation_widgets[98]{};
    UiLayoutNode virtual_navigation_layout[98]{};
    const UiRect virtual_viewport{72.0f, 48.0f, 312.0f, 248.0f};
    const bool virtual_navigation_initialized = shell_navigation_scene_initialize(
        &virtual_navigation_scene, Slice<ShellNavigationRowWidget>{virtual_navigation_widgets_rows, 24},
        Slice<UiWidget>{virtual_navigation_widgets, 98},
        Slice<UiLayoutNode>{virtual_navigation_layout, 98});
    const ShellNavigationSceneResult virtual_navigation_first = shell_navigation_scene_update(
        &virtual_navigation_scene, &virtual_navigation, virtual_viewport, 400.0f);
    const ShellNavigationRowWidget* hit_row = nullptr;
    const UiLayoutNode* hit_layout = nullptr;
    for (u32 index = 0; index < virtual_navigation_scene.count; ++index) {
        const ShellNavigationRowWidget& candidate = virtual_navigation_scene.rows[index];
        const UiLayoutNode* candidate_layout =
            shell_navigation_scene_layout_node(virtual_navigation_scene, candidate);
        if (candidate.view.selectable && candidate_layout &&
            candidate_layout->rectangle.top >= virtual_viewport.top &&
            candidate_layout->rectangle.bottom <= virtual_viewport.bottom) {
            hit_row = &candidate;
            hit_layout = candidate_layout;
            break;
        }
    }
    const ShellNavigationHit virtual_hit =
        hit_row && hit_layout ? shell_navigation_scene_hit_test(
                                    virtual_navigation_scene,
                                    (hit_layout->rectangle.left + hit_layout->rectangle.right) * 0.5f,
                                    (hit_layout->rectangle.top + hit_layout->rectangle.bottom) * 0.5f)
                              : ShellNavigationHit{};
    const UiWidgetHandle retained_handle = hit_row ? hit_row->widget : ui_widget_invalid_handle();
    const u16 retained_generation = retained_handle.generation;
    const u64 retained_channel_id = hit_row ? hit_row->view.id : 0;
    const bool focused = retained_channel_id != 0 && shell_navigation_scene_focus_channel(
                                                         &virtual_navigation_scene, retained_channel_id);
    const u32 materializations_before = virtual_navigation_scene.rebuilds;
    const ShellNavigationSceneResult retained_reflow = shell_navigation_scene_update(
        &virtual_navigation_scene, &virtual_navigation, virtual_viewport, 401.0f);
    const ShellNavigationRowWidget* retained_row =
        shell_navigation_scene_resolve(virtual_navigation_scene, retained_handle);
    UiLayoutDiagnostic virtual_diagnostics[16]{};
    const UiLayoutValidationResult virtual_structure_validation = ui_layout_engine_validate(
        virtual_navigation_scene.widgets.layout, Slice<UiLayoutDiagnostic>{virtual_diagnostics, 16});
    const UiLayoutContentBounds virtual_text_bounds{
        retained_row ? ui_widget_manager_layout_handle(virtual_navigation_scene.widgets, retained_row->widget)
                     : ui_layout_invalid_handle(),
        retained_row
            ? shell_navigation_scene_layout_node(virtual_navigation_scene, *retained_row)->content_rectangle
            : UiRect{},
        UiLayoutContentKind::text, ui_layout_content_bounds_collision};
    const UiLayoutValidationResult virtual_content_validation = ui_layout_engine_validate_content_bounds(
        virtual_navigation_scene.widgets.layout, ConstSlice<UiLayoutContentBounds>{&virtual_text_bounds, 1},
        Slice<UiLayoutDiagnostic>{virtual_diagnostics, 16});
    expect(virtual_navigation_initialized && virtual_navigation_first.ok() &&
               virtual_navigation_first.first != 0 && virtual_navigation_first.count < 24 &&
               virtual_navigation_scene.widgets.layout.storage[0].rectangle.left == virtual_viewport.left &&
               virtual_hit.id == retained_channel_id &&
               virtual_hit.widget.generation == retained_generation && focused && retained_reflow.ok() &&
               retained_row && retained_row->widget.generation == retained_generation &&
               virtual_navigation_scene.rebuilds == materializations_before &&
               virtual_navigation_scene.layout_passes > virtual_navigation_scene.rebuilds &&
               virtual_structure_validation.clean() && virtual_content_validation.clean(),
           "shell_navigation_virtual_rows_retain_widget_identity_and_validate");
    const ShellNavigationSceneResult displaced = shell_navigation_scene_update(
        &virtual_navigation_scene, &virtual_navigation, virtual_viewport, 1'400.0f);
    const bool stale_rejected = !shell_navigation_scene_resolve(virtual_navigation_scene, retained_handle);
    const ShellNavigationSceneResult restored = shell_navigation_scene_update(
        &virtual_navigation_scene, &virtual_navigation, virtual_viewport, 401.0f);
    const ShellNavigationRowWidget* restored_focus =
        shell_navigation_scene_resolve(virtual_navigation_scene, virtual_navigation_scene.widgets.focused);
    expect(displaced.ok() && stale_rejected && restored.ok() && restored_focus &&
               restored_focus->view.id == retained_channel_id,
           "shell_navigation_virtual_focus_restores_after_rematerialization");

    ShellNavigationScene tiny_virtual_scene{};
    ShellNavigationRowWidget tiny_virtual_rows[2]{};
    UiWidget tiny_virtual_widgets[10]{};
    UiLayoutNode tiny_virtual_layout[10]{};
    const bool tiny_virtual_initialized = shell_navigation_scene_initialize(
        &tiny_virtual_scene, Slice<ShellNavigationRowWidget>{tiny_virtual_rows, 2},
        Slice<UiWidget>{tiny_virtual_widgets, 10}, Slice<UiLayoutNode>{tiny_virtual_layout, 10});
    const ShellNavigationSceneResult tiny_virtual_result = shell_navigation_scene_update(
        &tiny_virtual_scene, &virtual_navigation, UiRect{72.0f, 48.0f, 312.0f, 848.0f}, 0.0f);
    expect(tiny_virtual_initialized && tiny_virtual_result.error == ShellNavigationSceneError::capacity &&
               tiny_virtual_scene.widgets.count == 0 && tiny_virtual_scene.count == 0,
           "shell_navigation_capacity_failure_does_not_publish_partial_tree");

    MessageRecord grouping_records[5]{};
    u32 grouping_order[5]{};
    u8 grouping_text[128]{};
    MessageStore grouping_store{};
    constexpr u64 grouping_time = 1'700'000'000'000ull;
    bool grouping_seeded =
        message_store_initialize(&grouping_store, Slice<MessageRecord>{grouping_records, 5},
                                 Slice<u32>{grouping_order, 5}, Slice<u8>{grouping_text, 128}) &&
        message_store_upsert(&grouping_store,
                             MessageInput{1, 7, 9, grouping_time, 0, utf8_literal("one"), 0})
            .ok() &&
        message_store_upsert(&grouping_store, MessageInput{2, 7, 9, grouping_time + 4ull * 60ull * 1'000ull,
                                                           0, utf8_literal("two"), 0})
            .ok() &&
        message_store_upsert(&grouping_store, MessageInput{3, 7, 9, grouping_time + 8ull * 60ull * 1'000ull,
                                                           0, utf8_literal("three"), 0})
            .ok() &&
        message_store_upsert(&grouping_store,
                             MessageInput{4, 7, 10, grouping_time + 8ull * 60ull * 1'000ull + 1u, 0,
                                          utf8_literal("four"), 0})
            .ok() &&
        message_store_upsert(&grouping_store,
                             MessageInput{5, 7, 10, grouping_time + 8ull * 60ull * 1'000ull + 2u, 0,
                                          utf8_literal("five"), 0})
            .ok();
    u32 grouping_units[5]{};
    u64 grouping_fenwick[5]{};
    UiVariableList grouping_list{};
    MessageTimeline grouping_timeline{};
    MessageScene grouping_scene{};
    UiWidget grouping_widgets[5 * kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode grouping_layout[5 * kMessageSceneWidgetsPerRow + 1]{};
    const u32 grouping_default_units = ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height);
    grouping_seeded =
        grouping_seeded &&
        ui_variable_list_initialize(&grouping_list, Slice<u32>{grouping_units, 5},
                                    Slice<u64>{grouping_fenwick, 5}, 5, grouping_default_units) &&
        message_timeline_initialize(&grouping_timeline, &grouping_store, &grouping_list,
                                    grouping_default_units, ui_extent_units(1'000.0f), true) &&
        message_scene_initialize_owned(
            &grouping_scene, &grouping_timeline, 5, 1,
            Slice<UiWidget>{grouping_widgets, 5 * kMessageSceneWidgetsPerRow + 1},
            Slice<UiLayoutNode>{grouping_layout, 5 * kMessageSceneWidgetsPerRow + 1});
    const MessageSceneResult grouping_arranged =
        grouping_seeded ? message_scene_update(&grouping_scene, UiRect{0.0f, 0.0f, 720.0f, 1'000.0f})
                        : MessageSceneResult{};
    bool grouping_height_changed = false;
    bool grouping_header_height_changed = false;
    const bool grouping_measured =
        grouping_arranged.ok() && grouping_scene.count == 5 &&
        message_scene_set_text_metrics(&grouping_scene, 1, 0.0f, 16.0f, true, nullptr, nullptr, {},
                                       &grouping_height_changed) &&
        message_scene_set_text_metrics(&grouping_scene, 0, 0.0f, 16.0f, true, nullptr, nullptr, {},
                                       &grouping_header_height_changed);
    const MessageSceneResult grouping_rearranged =
        grouping_measured ? message_scene_update(&grouping_scene, UiRect{0.0f, 0.0f, 720.0f, 1'000.0f})
                          : MessageSceneResult{};
    const UiLayoutNode* grouping_first_content =
        grouping_rearranged.ok() ? message_scene_layout_node(grouping_scene, grouping_scene.rows[0].content)
                                 : nullptr;
    const UiLayoutNode* grouping_second_content =
        grouping_rearranged.ok() ? message_scene_layout_node(grouping_scene, grouping_scene.rows[1].content)
                                 : nullptr;
    expect(grouping_arranged.ok() && grouping_scene.count == 5 && grouping_scene.owns_rows &&
               grouping_scene.rows.count == 5 && !grouping_scene.rows[0].grouped &&
               grouping_scene.rows[1].grouped && !grouping_scene.rows[2].grouped &&
               !grouping_scene.rows[3].grouped && grouping_scene.rows[4].grouped && grouping_measured &&
               grouping_height_changed && grouping_header_height_changed && grouping_rearranged.ok() &&
               grouping_first_content && grouping_second_content &&
               grouping_second_content->content_rectangle.top -
                       grouping_first_content->content_rectangle.top ==
                   22.0f &&
               ui_variable_list_item_units(grouping_list, 0) == ui_extent_units(57.0f) &&
               ui_variable_list_item_units(grouping_list, 1) == ui_extent_units(22.0f),
           "message_scene_groups_from_start_anchor_within_seven_minutes");
    message_scene_release(&grouping_scene);

    MessageRecord message_scene_records[32]{};
    u32 message_scene_order[32]{};
    u8 message_scene_text[2 * 1024]{};
    MessageStore message_scene_store{};
    bool message_scene_seeded = message_store_initialize(
        &message_scene_store, Slice<MessageRecord>{message_scene_records, 32},
        Slice<u32>{message_scene_order, 32}, Slice<u8>{message_scene_text, 2 * 1024});
    for (u32 index = 0; index < 32 && message_scene_seeded; ++index) {
        message_scene_seeded =
            message_store_upsert(&message_scene_store,
                                 MessageInput{80'000 + index, 7, 9, 1'700'000'000'000ull + index, 0,
                                              utf8_literal("message"), 0})
                .ok();
    }
    u32 message_scene_item_units[32]{};
    u64 message_scene_fenwick[32]{};
    UiVariableList message_scene_list{};
    MessageTimeline message_scene_timeline{};
    const u32 message_default_units = ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height);
    const bool message_scene_model_ready =
        message_scene_seeded &&
        ui_variable_list_initialize(&message_scene_list, Slice<u32>{message_scene_item_units, 32},
                                    Slice<u64>{message_scene_fenwick, 32}, 32, message_default_units) &&
        message_timeline_initialize(&message_scene_timeline, &message_scene_store, &message_scene_list,
                                    message_default_units, ui_extent_units(320.0f), false) &&
        message_timeline_set_scroll(&message_scene_timeline, static_cast<u64>(message_default_units) * 10u,
                                    false);
    MessageScene virtual_message_scene{};
    // This full rich-capacity fixture exceeds a Windows thread's default stack
    // after adding independent forwarded rows. Keep the test banks off-stack;
    // production still admits only required rows through its existing storage.
    static MessageSceneRowWidget virtual_message_rows[16]{};
    static UiWidget virtual_message_widgets[16 * kMessageSceneWidgetsPerRow + 1]{};
    static UiLayoutNode virtual_message_layout[16 * kMessageSceneWidgetsPerRow + 1]{};
    const UiRect message_viewport{312.0f, 48.0f, 1'040.0f, 368.0f};
    const bool virtual_message_initialized =
        message_scene_model_ready &&
        message_scene_initialize(
            &virtual_message_scene, &message_scene_timeline,
            Slice<MessageSceneRowWidget>{virtual_message_rows, 16},
            Slice<UiWidget>{virtual_message_widgets, 16 * kMessageSceneWidgetsPerRow + 1},
            Slice<UiLayoutNode>{virtual_message_layout, 16 * kMessageSceneWidgetsPerRow + 1});
    const MessageSceneResult virtual_message_first =
        message_scene_update(&virtual_message_scene, message_viewport);
    const MessageSceneRowWidget* visible_message_row = nullptr;
    const UiLayoutNode* visible_message_layout = nullptr;
    for (u32 index = 0; index < virtual_message_scene.count; ++index) {
        const MessageSceneRowWidget& candidate = virtual_message_scene.rows[index];
        const UiLayoutNode* layout = message_scene_layout_node(virtual_message_scene, candidate.widget);
        if (layout && layout->rectangle.top >= message_viewport.top &&
            layout->rectangle.bottom <= message_viewport.bottom) {
            visible_message_row = &candidate;
            visible_message_layout = layout;
            break;
        }
    }
    const MessageSceneHit message_hit =
        visible_message_layout
            ? message_scene_hit_test(
                  virtual_message_scene,
                  (visible_message_layout->rectangle.left + visible_message_layout->rectangle.right) * 0.5f,
                  (visible_message_layout->rectangle.top + visible_message_layout->rectangle.bottom) * 0.5f)
            : MessageSceneHit{};
    const UiWidgetHandle retained_message_handle =
        visible_message_row ? visible_message_row->widget : ui_widget_invalid_handle();
    const u64 retained_message_id = visible_message_row ? visible_message_row->message_id : 0;
    const u32 retained_message_source = visible_message_row ? visible_message_row->source_index : 0;
    const u32 message_rebuilds_before_data = virtual_message_scene.rebuilds;
    const u32 message_layouts_before_cached = virtual_message_scene.layout_passes;
    const MessageSceneResult virtual_message_cached =
        message_scene_update(&virtual_message_scene, message_viewport);
    const u32 message_layouts_after_cached = virtual_message_scene.layout_passes;
    const bool virtual_message_data_applied =
        retained_message_id != 0 &&
        message_store_upsert(&message_scene_store,
                             MessageInput{retained_message_id, 7, 9,
                                          1'700'000'000'000ull + retained_message_source, 0,
                                          utf8_literal("message updated"), 0})
            .ok();
    const MessageSceneResult virtual_message_data =
        message_scene_update(&virtual_message_scene, message_viewport);
    const MessageSceneRowWidget* message_after_data =
        message_scene_resolve(virtual_message_scene, retained_message_handle);
    const bool message_data_identity_retained =
        message_after_data && message_after_data->widget.slot == retained_message_handle.slot &&
        message_after_data->widget.generation == retained_message_handle.generation &&
        virtual_message_scene.rebuilds == message_rebuilds_before_data;
    const u32 message_data_updates_after_data = virtual_message_scene.data_updates;
    bool message_height_changed = false;
    const bool message_measured =
        message_after_data &&
        message_scene_set_text_metrics(&virtual_message_scene, retained_message_source, 0.0f, 180.0f, true,
                                       nullptr, nullptr, {}, &message_height_changed);
    const MessageSceneResult virtual_message_reflow =
        message_scene_update(&virtual_message_scene, message_viewport);
    const MessageSceneRowWidget* message_after_reflow =
        message_scene_find_row(virtual_message_scene, retained_message_source);
    const UiLayoutNode* message_content_layout =
        message_after_reflow
            ? message_scene_layout_node(virtual_message_scene, message_after_reflow->content)
            : nullptr;
    UiLayoutDiagnostic message_scene_diagnostics[32]{};

    MessageRichStore message_scene_rich_store{};
    GatewayMessageReaction presentation_reactions[1]{};
    presentation_reactions[0].emoji_id = 44;
    presentation_reactions[0].name = utf8_literal("party");
    presentation_reactions[0].count = 3;
    presentation_reactions[0].normal_count = 3;
    presentation_reactions[0].me = true;
    presentation_reactions[0].animated = true;
    GatewayMessageAttachment presentation_attachments[1]{};
    presentation_attachments[0].id = 70;
    presentation_attachments[0].filename = utf8_literal("preview.webp");
    presentation_attachments[0].content_type = utf8_literal("image/webp");
    presentation_attachments[0].width = 320;
    presentation_attachments[0].height = 180;
    presentation_attachments[0].has_width = true;
    presentation_attachments[0].has_height = true;
    GatewayMessageEmbed presentation_embeds[1]{};
    presentation_embeds[0].title = utf8_literal("Build");
    presentation_embeds[0].description = utf8_literal("Layout-owned rich content");
    presentation_embeds[0].type = GatewayMessageEmbedType::rich;
    presentation_embeds[0].color = 0x5865F2u;
    presentation_embeds[0].has_color = true;
    presentation_embeds[0].image.url = utf8_literal("https://example.com/presentation.webp");
    presentation_embeds[0].image.proxy_url =
        utf8_literal("https://media.discordapp.net/external/presentation/image.webp");
    presentation_embeds[0].image.content_type = utf8_literal("image/webp");
    presentation_embeds[0].image.width = 640;
    presentation_embeds[0].image.height = 360;
    presentation_embeds[0].image.has_width = true;
    presentation_embeds[0].image.has_height = true;
    GatewayMessageSticker presentation_stickers[1]{};
    presentation_stickers[0].id = 88;
    presentation_stickers[0].name = utf8_literal("Wave");
    presentation_stickers[0].format_type = 2;
    GatewayMessageComponent presentation_components[1]{};
    presentation_components[0].label = utf8_literal("Open");
    presentation_components[0].type = 2;
    presentation_components[0].style = 5;
    presentation_components[0].url = utf8_literal("https://example.com");
    presentation_components[0].parent = 0xFFFFu;
    GatewayMessageEvent presentation_rich{};
    presentation_rich.id = retained_message_id;
    presentation_rich.fields = gateway_message_field_message_reference |
                               gateway_message_field_referenced_message | gateway_message_field_reactions |
                               gateway_message_field_attachments | gateway_message_field_embeds |
                               gateway_message_field_sticker_items | gateway_message_field_components;
    presentation_rich.reference.message_id = 79'999;
    presentation_rich.reference.channel_id = 7;
    presentation_rich.reference.author_id = 8;
    presentation_rich.reference.author_name = utf8_literal("Source");
    presentation_rich.reference.content = utf8_literal("Referenced text");
    presentation_rich.reference.state = GatewayMessageReferenceState::available;
    presentation_rich.reactions = ConstSlice<GatewayMessageReaction>{presentation_reactions, 1};
    presentation_rich.attachments = ConstSlice<GatewayMessageAttachment>{presentation_attachments, 1};
    presentation_rich.embeds = ConstSlice<GatewayMessageEmbed>{presentation_embeds, 1};
    presentation_rich.stickers = ConstSlice<GatewayMessageSticker>{presentation_stickers, 1};
    presentation_rich.components = ConstSlice<GatewayMessageComponent>{presentation_components, 1};
    const MessageStoreResult presentation_message =
        message_store_find(message_scene_store, retained_message_id);
    const MessageRecord* presentation_record =
        presentation_message.ok() ? message_store_resolve(message_scene_store, presentation_message.handle)
                                  : nullptr;
    const bool presentation_rich_ready =
        presentation_record &&
        message_rich_store_initialize(
            &message_scene_rich_store, Slice<MessageRichRecord>{g_message_scene_rich_records, 32},
            Slice<u16>{g_message_scene_rich_order, 32}, Slice<u8>{g_message_scene_rich_text, 16 * 1024}) &&
        message_timeline_bind_rich_store(&message_scene_timeline, &message_scene_rich_store) &&
        message_rich_store_apply(&message_scene_rich_store, retained_message_id,
                                 presentation_record->revision, presentation_rich)
            .ok();
    const MessageSceneResult presentation_rich_refresh =
        message_scene_update(&virtual_message_scene, message_viewport);
    const MessageSceneRowWidget* presentation_row =
        message_scene_find_row(virtual_message_scene, retained_message_source);
    bool presentation_measure_changed = false;
    const bool presentation_measured =
        presentation_row &&
        message_scene_set_text_metrics(&virtual_message_scene, retained_message_source, 36.0f, 180.0f, true,
                                       nullptr, nullptr, {}, &presentation_measure_changed);
    const MessageSceneResult presentation_rich_reflow =
        message_scene_update(&virtual_message_scene, message_viewport);
    const MessageSceneRowWidget* presentation_owned_row =
        presentation_rich_reflow.ok()
            ? message_scene_find_row(virtual_message_scene, retained_message_source)
            : nullptr;
    const UiWidgetHandle presentation_attachment_item =
        presentation_owned_row ? message_scene_rich_item_widget(*presentation_owned_row,
                                                                MessageSceneRichItemKind::attachment, 0)
                               : ui_widget_invalid_handle();
    const UiWidgetHandle presentation_embed_item =
        presentation_owned_row
            ? message_scene_rich_item_widget(*presentation_owned_row, MessageSceneRichItemKind::embed, 0)
            : ui_widget_invalid_handle();
    const UiWidgetHandle presentation_sticker_item =
        presentation_owned_row
            ? message_scene_rich_item_widget(*presentation_owned_row, MessageSceneRichItemKind::sticker, 0)
            : ui_widget_invalid_handle();
    PaintStream message_presentation_paint{};
    test_message_background_surfaces(virtual_message_scene, retained_message_id, expect);
    paint_stream_reset(&message_presentation_paint, message_viewport.right, message_viewport.bottom);
    MessageMediaCommand message_media_commands[8]{};
    MessageMediaCommandStream message_media_stream{};
    MessagePresentationResult message_presentation_result{};
    const bool message_presentation_ready =
        message_media_command_stream_initialize(&message_media_stream,
                                                Slice<MessageMediaCommand>{message_media_commands, 8}) &&
        message_presentation_build(virtual_message_scene, kDefaultTheme, &message_presentation_paint,
                                   &message_media_stream, &message_presentation_result);
    u32 attachment_commands = 0;
    u32 embed_commands = 0;
    u32 sticker_commands = 0;
    u32 emoji_commands = 0;
    bool attachment_format_projected = false;
    bool embed_identity_projected = false;
    bool message_media_item_owned = presentation_owned_row != nullptr;
    for (u32 index = 0; index < message_media_stream.count; ++index) {
        attachment_commands += message_media_commands[index].kind == MessageMediaCommandKind::attachment;
        embed_commands += message_media_commands[index].kind == MessageMediaCommandKind::embed;
        sticker_commands += message_media_commands[index].kind == MessageMediaCommandKind::sticker;
        emoji_commands += message_media_commands[index].kind == MessageMediaCommandKind::custom_emoji;
        UiWidgetHandle expected = ui_widget_invalid_handle();
        if (message_media_commands[index].kind == MessageMediaCommandKind::attachment) {
            expected = presentation_attachment_item;
            attachment_format_projected =
                message_media_commands[index].format == static_cast<u8>(MediaAttachmentFormat::webp);
        } else if (message_media_commands[index].kind == MessageMediaCommandKind::embed) {
            expected = presentation_embed_item;
            embed_identity_projected =
                message_media_commands[index].id == retained_message_id && presentation_record &&
                message_media_commands[index].message_revision == presentation_record->revision &&
                message_media_commands[index].embed_index == 0 &&
                message_media_commands[index].embed_resource ==
                    static_cast<u8>(MediaEmbedResource::image) &&
                message_media_commands[index].format == static_cast<u8>(MediaAttachmentFormat::webp);
        } else if (message_media_commands[index].kind == MessageMediaCommandKind::sticker) {
            expected = presentation_sticker_item;
        } else if (presentation_owned_row) {
            expected = presentation_owned_row->reaction_buttons[0];
        }
        message_media_item_owned =
            message_media_item_owned &&
            message_media_commands[index].owner.manager == &virtual_message_scene.widgets &&
            message_media_commands[index].owner.handle.slot == expected.slot &&
            message_media_commands[index].owner.handle.generation == expected.generation;
    }
    MessageMediaCommand tiny_message_media_commands[2]{};
    MessageMediaCommandStream tiny_message_media_stream{};
    const u32 paint_count_before_tiny = message_presentation_paint.count;
    MessagePresentationResult tiny_message_presentation{};
    const bool tiny_message_media_initialized = message_media_command_stream_initialize(
        &tiny_message_media_stream, Slice<MessageMediaCommand>{tiny_message_media_commands, 2});
    const bool tiny_message_presentation_failed =
        tiny_message_media_initialized &&
        !message_presentation_build(virtual_message_scene, kDefaultTheme, &message_presentation_paint,
                                    &tiny_message_media_stream, &tiny_message_presentation);

    const UiLayoutValidationResult message_structure = ui_layout_engine_validate(
        virtual_message_scene.widgets.layout, Slice<UiLayoutDiagnostic>{message_scene_diagnostics, 32});
    UiLayoutValidationResult message_content_clean{};
    UiLayoutValidationResult message_content_overflow{};
    if (message_after_reflow && message_content_layout) {
        const UiWidget* content_widget =
            ui_widget_manager_resolve(virtual_message_scene.widgets, message_after_reflow->content);
        if (content_widget) {
            UiLayoutContentBounds emitted{
                ui_widget_manager_layout_handle(virtual_message_scene.widgets, message_after_reflow->content),
                message_content_layout->content_rectangle,
                                          UiLayoutContentKind::text, ui_layout_content_bounds_collision};
            message_content_clean = ui_layout_engine_validate_content_bounds(
                virtual_message_scene.widgets.layout, ConstSlice<UiLayoutContentBounds>{&emitted, 1},
                Slice<UiLayoutDiagnostic>{message_scene_diagnostics, 32});
            emitted.rectangle.right += 12.0f;
            message_content_overflow = ui_layout_engine_validate_content_bounds(
                virtual_message_scene.widgets.layout, ConstSlice<UiLayoutContentBounds>{&emitted, 1},
                Slice<UiLayoutDiagnostic>{message_scene_diagnostics, 32});
        }
    }
    expect(virtual_message_initialized && virtual_message_first.ok() && virtual_message_first.first != 0 &&
               virtual_message_first.count < 16 && message_hit.message_id == retained_message_id,
           "message_scene_virtual_hit_and_range");
    expect(virtual_message_cached.ok() && !virtual_message_cached.changed &&
               message_layouts_after_cached == message_layouts_before_cached,
           "message_scene_cached_update_skips_layout");
    expect(virtual_message_data_applied && virtual_message_data.ok(), "message_scene_data_update_applies");
    expect(message_data_identity_retained, "message_scene_data_update_retains_widget_identity");
    expect(message_data_updates_after_data == 1, "message_scene_data_update_counter");
    expect(message_measured && message_height_changed && virtual_message_reflow.ok() &&
               message_content_layout &&
               ui_variable_list_item_units(message_scene_list, retained_message_source) >
                   message_default_units,
           "message_scene_dynamic_text_measurement_reflows_row");
    expect(message_structure.clean() && message_content_clean.clean() &&
               message_content_overflow.count != 0,
           "message_scene_structure_and_content_bounds_diagnostics");
    expect(presentation_rich_ready && presentation_rich_refresh.ok() && presentation_measured &&
               presentation_measure_changed && presentation_rich_reflow.ok(),
           "message_presentation_rich_scene_reflow");
    expect(message_presentation_ready,
           "message_presentation_builds_owned_streams");
    expect(message_presentation_result.geometry_quads != 0, "message_presentation_geometry_count");
    expect(message_presentation_result.media_commands == 4 && attachment_commands == 1 &&
               embed_commands == 1 && sticker_commands == 1 && emoji_commands == 1 &&
               attachment_format_projected && embed_identity_projected && message_media_item_owned,
           "message_presentation_media_projection_uses_exact_retained_items");
    expect(tiny_message_presentation_failed && tiny_message_media_stream.count == 0 &&
               tiny_message_media_stream.rejected != 0 && tiny_message_presentation.geometry_quads == 0 &&
               message_presentation_paint.count == paint_count_before_tiny,
           "message_presentation_capacity_failure_is_atomic");
    const u64 restored_scroll = message_scene_timeline.scroll_units;
    const bool message_focused = message_scene_focus_message(&virtual_message_scene, retained_message_id);
    UiRect message_context_anchor{};
    expect(message_focused && ui_widget_manager_context_anchor(virtual_message_scene.widgets, &message_context_anchor) &&
               message_context_anchor.left >= message_viewport.left && message_context_anchor.top >= message_viewport.top &&
               message_context_anchor.right <= message_viewport.right && message_context_anchor.bottom <= message_viewport.bottom,
           "message_context_anchor_uses_visible_focused_row");
    const bool message_scrolled_away =
        message_timeline_set_scroll(&message_scene_timeline, message_scene_list.total_units, false);
    const MessageSceneResult message_displaced =
        message_scene_update(&virtual_message_scene, message_viewport);
    const bool message_stale = !message_scene_resolve(virtual_message_scene, retained_message_handle);
    expect(!ui_widget_manager_context_anchor(virtual_message_scene.widgets, &message_context_anchor),
           "virtualized_away_message_cannot_supply_a_stale_context_target");
    const bool message_scrolled_back =
        message_timeline_set_scroll(&message_scene_timeline, restored_scroll, false);
    const MessageSceneResult message_restored =
        message_scene_update(&virtual_message_scene, message_viewport);
    const MessageSceneRowWidget* message_restored_focus =
        message_scene_resolve(virtual_message_scene, virtual_message_scene.widgets.focused);
    expect(message_focused && message_scrolled_away && message_displaced.ok() && message_stale &&
               message_scrolled_back && message_restored.ok() && message_restored_focus &&
               message_restored_focus->message_id == retained_message_id,
           "message_scene_stale_handle_and_focus_restoration");
    expect(ui_widget_manager_context_anchor(virtual_message_scene.widgets, &message_context_anchor),
           "restored_message_focus_restores_its_context_anchor");
    const auto keyboard_first = message_scene_navigate(&virtual_message_scene, UiListMove::first);
    bool keyboard_walk = keyboard_first.message_id && keyboard_first.source_index == 0;
    for (u32 i = 1; i < message_scene_store.count; ++i) {
        const auto next = message_scene_navigate(&virtual_message_scene, UiListMove::next);
        keyboard_walk = keyboard_walk && next.message_id && next.source_index == i &&
            virtual_message_scene.count <= 16 &&
            ui_widget_manager_context_anchor(virtual_message_scene.widgets, &message_context_anchor);
    }
    expect(keyboard_walk && !message_scene_timeline.follow_bottom &&
               !message_scene_navigate(&virtual_message_scene, UiListMove::next).message_id,
           "message_keyboard_walk_crosses_virtual_windows_without_wrapping_or_following_new_messages");
    expect(message_scene_navigate(&virtual_message_scene, UiListMove::previous).source_index == message_scene_store.count - 2 &&
               message_scene_navigate(&virtual_message_scene, UiListMove::first).source_index == 0 &&
               !message_scene_navigate(&virtual_message_scene, UiListMove::previous).message_id &&
               message_scene_navigate(&virtual_message_scene, UiListMove::last).source_index == message_scene_store.count - 1,
           "message_keyboard_home_end_and_reverse_focus_use_logical_order");
    bool message_pages = message_scene_navigate(&virtual_message_scene, UiListMove::first).message_id != 0;
    u32 previous_page_index = 0;
    // 32 rows always reach the end within 31 pages; the remaining pages exercise the stop at the end.
    for (u32 i = 0; i < 40; ++i) {
        const u64 before = message_scene_timeline.scroll_units;
        const auto page = message_scene_page(&virtual_message_scene, false);
        message_pages = message_pages && page.message_id && page.source_index >= previous_page_index &&
            message_scene_timeline.scroll_units >= before && virtual_message_scene.count <= 16 &&
            ui_widget_manager_context_anchor(virtual_message_scene.widgets, &message_context_anchor);
        previous_page_index = page.source_index;
    }
    expect(message_pages && previous_page_index == message_scene_store.count - 1 &&
               message_scene_page(&virtual_message_scene, true).message_id != 0,
           "message_pages_preserve_visible_focus_through_mixed_height_virtual_windows_and_stop_at_end");

    MessageScene tiny_message_scene{};
    MessageSceneRowWidget tiny_message_rows[1]{};
    UiWidget tiny_message_widgets[kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode tiny_message_layout[kMessageSceneWidgetsPerRow + 1]{};
    const bool tiny_message_initialized = message_scene_initialize(
        &tiny_message_scene, &message_scene_timeline, Slice<MessageSceneRowWidget>{tiny_message_rows, 1},
        Slice<UiWidget>{tiny_message_widgets, kMessageSceneWidgetsPerRow + 1},
        Slice<UiLayoutNode>{tiny_message_layout, kMessageSceneWidgetsPerRow + 1});
    const MessageSceneResult tiny_message_result =
        message_scene_update(&tiny_message_scene, UiRect{312.0f, 48.0f, 1'040.0f, 848.0f});
    expect(tiny_message_initialized && tiny_message_result.error == MessageSceneError::capacity &&
               tiny_message_scene.widgets.count == 0 && tiny_message_scene.count == 0,
           "message_scene_capacity_failure_does_not_publish_partial_tree");

    expect(
        ui_scene_layout_with_metrics(&scene, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics,
                                     632.0f, 480.0f) &&
            ui_scene_node(scene, UiSemantic::channel_sidebar) &&
            !ui_scene_node(scene, UiSemantic::member_sidebar) &&
            ui_scene_node(scene, UiSemantic::member_toggle) &&
            ui_scene_node(scene, UiSemantic::content)->rectangle.left == 304.0f &&
            ui_scene_node(scene, UiSemantic::content)->rectangle.right == 632.0f,
        "ui_scene_narrow_transition_suppresses_members_and_preserves_toggle");

    UiScene friends_scene{};
    expect(ui_scene_initialize_bootstrap(&friends_scene) &&
               ui_scene_layout_with_metrics(&friends_scene, UiShellView::friends,
                                            kFriendsShellLayoutMetrics, 480.0f, 930.0f),
           "ui_scene_friends_policy_layout");
    const UiLayoutNode* friends_guild =
        ui_scene_node(friends_scene, UiSemantic::guild_rail);
    const UiLayoutNode* friends_navigation =
        ui_scene_node(friends_scene, UiSemantic::channel_sidebar);
    const UiLayoutNode* friends_content =
        ui_scene_node(friends_scene, UiSemantic::content);
    const UiLayoutNode* friends_messages =
        ui_scene_node(friends_scene, UiSemantic::message_view);
    expect(friends_guild && friends_guild->rectangle.right == 64.0f && friends_navigation &&
               friends_navigation->rectangle.left == 64.0f &&
               friends_navigation->rectangle.right == 304.0f && friends_content &&
               friends_content->rectangle.left == 304.0f && friends_content->rectangle.right == 480.0f &&
               friends_messages && friends_messages->rectangle.top == 48.0f &&
               friends_messages->rectangle.bottom == 930.0f &&
               !ui_scene_node(friends_scene, UiSemantic::member_sidebar) &&
               !ui_scene_node(friends_scene, UiSemantic::composer),
           "ui_scene_friends_confirmed_fixed_columns");
    const u32 friends_rebuilds = friends_scene.layout_rebuilds;
    const u16 friends_generation = friends_scene.widgets.generation;
    expect(ui_scene_layout_with_metrics(&friends_scene, UiShellView::friends, kFriendsShellLayoutMetrics,
                                        1102.0f, 930.0f) &&
               friends_scene.layout_rebuilds == friends_rebuilds &&
               friends_scene.widgets.generation == friends_generation &&
               ui_scene_node(friends_scene, UiSemantic::content)
                       ->rectangle.left == 304.0f,
           "ui_scene_friends_resize_retains_fixed_policy_tree");
    UiShellLayoutMetrics revised_friends = kFriendsShellLayoutMetrics;
    revised_friends.revision = 2;
    revised_friends.navigation_width = 300.0f;
    expect(ui_scene_layout_with_metrics(&friends_scene, UiShellView::friends, revised_friends, 1102.0f,
                                        930.0f) &&
               friends_scene.layout_rebuilds == friends_rebuilds + 1 &&
               ui_scene_node(friends_scene, UiSemantic::content)
                       ->rectangle.left == 364.0f,
           "ui_scene_metrics_revision_rebuilds_structure");
    UiShellLayoutMetrics invalid_friends = revised_friends;
    invalid_friends.revision = 0;
    const u32 friends_rejected = friends_scene.rejected;
    expect(!ui_scene_layout_with_metrics(&friends_scene, UiShellView::friends, invalid_friends, 1102.0f,
                                         930.0f) &&
               friends_scene.rejected == friends_rejected + 1 &&
               friends_scene.layout_rebuilds == friends_rebuilds + 1,
           "ui_scene_invalid_metrics_reject_without_publish");
    bool home_columns_stable = true;
    const float home_navigation_widths[]{240.f, 300.f};
    const UiShellView home_views[]{UiShellView::friends, UiShellView::private_channels};
    for (float navigation_width : home_navigation_widths) {
        for (UiShellView home_view : home_views) {
            auto home_metrics = home_view == UiShellView::friends ? kFriendsShellLayoutMetrics
                                                                  : kGuildChannelShellLayoutMetrics;
            home_metrics.navigation_width = navigation_width;
            home_metrics.flags = ui_shell_layout_navigation;
            home_metrics.member_sidebar_width = 0;
            home_columns_stable =
                home_columns_stable &&
                ui_scene_layout_with_metrics(&friends_scene, home_view, home_metrics, 1102.f, 930.f);
            const auto* home_rail =
                ui_scene_node(friends_scene, UiSemantic::guild_rail);
            const auto* home_navigation =
                ui_scene_node(friends_scene, UiSemantic::channel_sidebar);
            const auto* home_content =
                ui_scene_node(friends_scene, UiSemantic::content);
            home_columns_stable = home_columns_stable && home_rail && home_navigation && home_content &&
                                  home_rail->rectangle.right == home_navigation->rectangle.left &&
                                  home_navigation->rectangle.right == home_content->rectangle.left &&
                                  ui_rect_width(home_navigation->rectangle) == navigation_width;
        }
    }
    expect(home_columns_stable,
           "ui_scene_home_private_switch_preserves_width_and_has_no_exposed_root_gutters");
    expect(ui_scene_node(scene, UiSemantic::title_bar) &&
               !ui_scene_layout_with_metrics(&scene, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics,
                                             0.0f, 480.0f) &&
               scene.rejected == 1,
           "ui_scene_node_and_bounds");
    ui_scene_release(&friends_scene);
    ui_scene_release(&scene);
    expect(!scene.storage.arena.base && !scene.count, "ui_scene_owned_storage_release");
}

void test_text_flow_layout() noexcept {
    UiTextFlowLayout flow{};
    UiRect first{};
    UiRect second{};
    const bool initialized = ui_text_flow_initialize(&flow, UiRect{10.0f, 20.0f, 210.0f, 120.0f},
                                                     UiRect{0.0f, 0.0f, 240.0f, 160.0f});
    const bool first_placed = initialized && ui_text_flow_begin_item(&flow, 8.0f, 12.0f, &first) &&
                              ui_text_flow_commit_item(&flow, 64.0f, 16.0f, 3.0f);
    const bool second_placed = first_placed && ui_text_flow_begin_item(&flow, 0.0f, 0.0f, &second) &&
                               ui_text_flow_commit_item(&flow, 10.0f, 16.0f, 0.0f);
    expect(second_placed && first.left == 18.0f && first.right == 198.0f && first.top == 20.0f &&
               second.top == 87.0f && second.top >= first.top + 67.0f && flow.item_count == 2 &&
               ui_text_flow_required_height(flow) == 83.0f && !flow.overflow,
           "text_flow_measured_items_do_not_overlap");

    const bool extended = second_placed && ui_text_flow_ensure_extent(&flow, 120.0f);
    expect(extended && ui_text_flow_required_height(flow) == 120.0f && flow.overflow &&
               flow.spacer_count == 1,
           "text_flow_reports_parent_overflow");

    UiTextFlowLayout invalid{};
    UiRect ignored{};
    const bool invalid_ready = ui_text_flow_initialize(&invalid, UiRect{0.0f, 0.0f, 100.0f, 40.0f},
                                                       UiRect{0.0f, 0.0f, 100.0f, 40.0f});
    expect(invalid_ready && !ui_text_flow_begin_item(&invalid, 60.0f, 40.0f, &ignored) &&
               invalid.last_error == UiTextFlowError::invalid_item && invalid.item_count == 0 &&
               !invalid.item_open,
           "text_flow_invalid_width_rejects_atomically");

    const float widths[] = {280.0f, 420.0f, 720.0f};
    const float dpi_scales[] = {1.0f, 1.25f, 1.5f, 2.0f};
    bool matrix_clean = true;
    u32 matrix_cases = 0;
    for (u32 width_index = 0; width_index < 3; ++width_index) {
        for (u32 scale_index = 0; scale_index < 4; ++scale_index) {
            UiTextFlowLayout matrix{};
            const float scale = dpi_scales[scale_index];
            matrix_clean = matrix_clean && ui_text_flow_initialize(
                                               &matrix, UiRect{0.0f, 0.0f, widths[width_index], 1'024.0f},
                                               UiRect{0.0f, 0.0f, widths[width_index], 1'024.0f});
            float expected_top = 0.0f;
            for (u32 item = 0; item < 4 && matrix_clean; ++item) {
                UiRect placed{};
                const float measured = (widths[width_index] < 320.0f ? 32.0f : 16.0f) * scale;
                const float gap = (item + 1u == 4u ? 0.0f : 3.0f) * scale;
                matrix_clean = ui_text_flow_begin_item(&matrix, 8.0f * scale, 8.0f * scale, &placed) &&
                               placed.top == expected_top &&
                               ui_text_flow_commit_item(&matrix, measured, 16.0f * scale, gap);
                expected_top += measured + gap;
            }
            matrix_clean = matrix_clean && !matrix.overflow && matrix.item_count == 4 &&
                           ui_text_flow_required_height(matrix) == expected_top;
            ++matrix_cases;
        }
    }
    expect(matrix_clean && matrix_cases == 12, "text_flow_width_font_density_dpi_matrix");
}

void test_text_measure_cache() noexcept {
    struct MeasureFixture {
        u32 calls;
    } fixture{};
    const auto backend =
        +[](void* opaque, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept -> bool {
        auto* state = static_cast<MeasureFixture*>(opaque);
        if (!state || !value || (request.text.bytes != 0 && request.text.data[0] == '!')) {
            return false;
        }
        ++state->calls;
        const u32 columns = maximum<u32>(1, static_cast<u32>(request.constraint.width / 8.0f));
        const u32 lines = request.text.bytes == 0 ? 0 : (request.text.bytes + columns - 1u) / columns;
        const float height = static_cast<float>(lines) * 18.0f;
        *value = UiTextMeasureValue{
            UiSize{minimum(request.constraint.width, static_cast<float>(request.text.bytes) * 8.0f),
                   height},
            lines};
        return true;
    };

    UiTextMeasureRecord records[8]{};
    UiTextMeasureCache cache{};
    expect(
        ui_text_measure_cache_initialize(&cache, Slice<UiTextMeasureRecord>{records, 8}, backend, &fixture),
        "text_measure_cache_initialize");
    const UiTextMeasureRequest narrow{UiTextMeasureKey{1, 2, 3, 4, 5}, utf8_literal("twenty byte text row"),
                                      ConstSlice<TextStyleSpan>{}, UiSize{40.0f, 200.0f}};
    UiTextMeasureValue measured{};
    const UiTextMeasureResult first = ui_text_measure_cache_measure(&cache, narrow, &measured);
    const u32 first_calls = fixture.calls;
    UiTextMeasureValue cached{};
    const UiTextMeasureResult second = ui_text_measure_cache_measure(&cache, narrow, &cached);
    expect(first.ok() && !first.cached && second.ok() && second.cached && first_calls == 1 &&
               fixture.calls == first_calls && measured.size.height == cached.size.height,
           "text_measure_cache_hit_avoids_backend");

    UiTextMeasureRequest wide = narrow;
    wide.constraint.width = 160.0f;
    expect(ui_text_measure_cache_measure(&cache, wide, &measured).ok() && fixture.calls == 2 &&
               measured.line_count < cached.line_count,
           "text_measure_cache_constraint_is_identity");

    ui_text_measure_cache_invalidate(&cache);
    const u32 calls_before_invalidate_miss = fixture.calls;
    expect(ui_text_measure_cache_measure(&cache, narrow, &measured).ok() &&
               fixture.calls == calls_before_invalidate_miss + 1,
           "text_measure_cache_constant_time_invalidation");

    for (u32 index = 0; index < 32; ++index) {
        UiTextMeasureRequest request = narrow;
        request.key.source_id = 100 + index;
        request.key.item = static_cast<u16>(index);
        expect(ui_text_measure_cache_measure(&cache, request, &measured).ok(),
               "text_measure_cache_bounded_fill");
    }
    u32 resident_hits = 0;
    for (u32 index = 32; index-- != 0;) {
        UiTextMeasureRequest request = narrow;
        request.key.source_id = 100 + index;
        request.key.item = static_cast<u16>(index);
        const UiTextMeasureResult again = ui_text_measure_cache_measure(&cache, request, &measured);
        resident_hits += again.ok() && again.cached ? 1u : 0u;
    }
    expect(resident_hits != 0 && resident_hits <= 8, "text_measure_cache_four_way_bounded_eviction");

    UiTextMeasureRequest failing = narrow;
    failing.key.source_revision = 99;
    failing.text = utf8_literal("!backend failure");
    UiTextMeasureValue sentinel{UiSize{77.0f, 88.0f}, 10};
    const UiTextMeasureResult failed = ui_text_measure_cache_measure(&cache, failing, &sentinel);
    expect(!failed.ok() && failed.error == UiTextMeasureError::backend && sentinel.size.width == 77.0f &&
               sentinel.size.height == 88.0f && sentinel.line_count == 10,
           "text_measure_cache_backend_failure_does_not_publish");
    const u32 calls_before_reinitialize = fixture.calls;
    expect(ui_text_measure_cache_initialize(&cache, Slice<UiTextMeasureRecord>{records, 8}, backend,
                                            &fixture) &&
               ui_text_measure_cache_measure(&cache, narrow, &measured).ok() &&
               fixture.calls == calls_before_reinitialize + 1,
           "text_measure_cache_reinitialize_discards_resident_generation");

    MessageTextCommand commands[2]{};
    char utf8[8]{};
    TextStyleSpan styles[1]{};
    MessageMarkdownSemantic semantics[1]{};
    MessageTextMeasurement measurements[1]{};
    MessageTextSpoilerReveal reveals[1]{};
    u32 scratch[8]{};
    MessageTextPresentation presentation{};
    UiTextMeasureRecord message_records[16]{};
    UiTextMeasureCache message_cache{};
    fixture.calls = 0;
    expect(message_text_presentation_initialize(
               &presentation, Slice<MessageTextCommand>{commands, 2}, Slice<char>{utf8, 8},
               Slice<TextStyleSpan>{styles, 1}, Slice<MessageMarkdownSemantic>{semantics, 1},
               Slice<MessageTextMeasurement>{measurements, 1}, Slice<MessageTextSpoilerReveal>{reveals, 1},
               Slice<u32>{scratch, 8}) &&
               ui_text_measure_cache_initialize(
                   &message_cache, Slice<UiTextMeasureRecord>{message_records, 16}, backend, &fixture),
           "message_text_measure_cache_fixture_initialize");
    presentation.active = true;
    presentation.command_count = 2;
    presentation.measurement_count = 1;
    commands[0].text = utf8_literal("reply");
    commands[0].rectangle = UiRect{0.0f, 0.0f, 40.0f, 100.0f};
    commands[0].message_id = 900;
    commands[0].message_revision = 1;
    commands[0].minimum_height = 16.0f;
    commands[0].gap_after = 3.0f;
    commands[0].measurement_index = 0;
    commands[0].measurement_kind = MessageTextMeasurementKind::reply;
    commands[1].text = utf8_literal("01234567890123456789");
    commands[1].rectangle = UiRect{0.0f, 0.0f, 32.0f, 200.0f};
    commands[1].message_id = 900;
    commands[1].message_revision = 1;
    commands[1].measurement_index = 0;
    commands[1].measurement_kind = MessageTextMeasurementKind::content;
    bool measurement_changed = false;
    const bool first_measure =
        message_text_presentation_measure(&presentation, &message_cache, 1, &measurement_changed);
    const u32 message_backend_calls = fixture.calls;
    expect(first_measure && measurement_changed && commands[0].measured_height == 18.0f &&
               commands[1].measured_height == 90.0f && measurements[0].reply_height == 21.0f &&
               measurements[0].content_height == 90.0f && message_backend_calls == 2,
           "message_text_measure_cache_aggregates_ui_owned_metrics");
    expect(message_text_presentation_measure(&presentation, &message_cache, 1, &measurement_changed) &&
               !measurement_changed && fixture.calls == message_backend_calls,
           "message_text_measure_cache_reuses_stable_commands");
    const u32 backend_calls_before_malformed = message_cache.backend_calls;
    const float aggregate_before_malformed = measurements[0].content_height;
    commands[1].measurement_index = 1;
    expect(!message_text_presentation_measure(&presentation, &message_cache, 1, &measurement_changed) &&
               !measurement_changed && message_cache.backend_calls == backend_calls_before_malformed &&
               measurements[0].content_height == aggregate_before_malformed,
           "message_text_measure_invalid_batch_rejects_before_cache_access");
    commands[1].measurement_index = 0;
    const Utf8View valid_content = commands[1].text;
    commands[1].text = Utf8View{nullptr, 1};
    expect(!message_text_presentation_measure(&presentation, &message_cache, 1, &measurement_changed) &&
               !measurement_changed && message_cache.backend_calls == backend_calls_before_malformed &&
               measurements[0].content_height == aggregate_before_malformed,
           "message_text_measure_invalid_text_rejects_before_signature_hash");
    commands[1].text = valid_content;
    const float reply_before_failure = commands[0].measured_height;
    const float content_before_failure = commands[1].measured_height;
    const float aggregate_before_failure = measurements[0].content_height;
    commands[1].text = utf8_literal("!failure");
    ++commands[1].message_revision;
    expect(!message_text_presentation_measure(&presentation, &message_cache, 1, &measurement_changed) &&
               commands[0].measured_height == reply_before_failure &&
               commands[1].measured_height == content_before_failure &&
               measurements[0].content_height == aggregate_before_failure,
           "message_text_measure_backend_failure_is_presentation_atomic");

    // Supplied identities and zero-identity content hashing both invalidate
    // changed text without requiring a new message revision.
    for (u32 supplied = 0; supplied < 2; ++supplied) {
        commands[1].text = valid_content;
        commands[1].measurement_identity = supplied ? 0x1234u : 0u;
        const u32 calls_before = fixture.calls;
        expect(message_text_presentation_measure(&presentation, &message_cache, 1, &measurement_changed) &&
                   commands[1].measured_height == 90.0f && fixture.calls == calls_before + 1,
               "message_text_measure_identity_initial_extent");
        expect(message_text_presentation_measure(&presentation, &message_cache, 1, &measurement_changed) &&
                   !measurement_changed && fixture.calls == calls_before + 1,
               "message_text_measure_identity_reuses_extent");
        commands[1].text = utf8_literal("tiny");
        commands[1].measurement_identity = supplied ? 0x5678u : 0u;
        expect(message_text_presentation_measure(&presentation, &message_cache, 1, &measurement_changed) &&
                   measurement_changed && commands[1].measured_height == 18.0f &&
                   fixture.calls == calls_before + 2,
               "message_text_measure_identity_refreshes_changed_text");
    }
}

void test_layout_engine_and_widget_manager() noexcept {
    UiWidget widgets[12]{};
    UiLayoutNode layout_nodes[12]{};
    UiWidgetManager manager{};
    expect(ui_widget_manager_initialize(&manager, Slice<UiWidget>{widgets, 12},
                                        Slice<UiLayoutNode>{layout_nodes, 12}),
           "widget_manager_initialize");

    UiLayoutStyle root_style = ui_layout_style();
    root_style.flow = UiLayoutFlow::vertical;
    root_style.width = ui_layout_fill();
    root_style.height = ui_layout_fill();
    root_style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children);
    const UiWidgetResult root = ui_widget_manager_add(
        &manager, ui_widget_invalid_handle(),
        UiWidgetDesc{1, UiWidgetKind::container, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("Root"), root_style, UiSize{}, 1}});

    UiLayoutStyle header_style = ui_layout_style();
    header_style.width = ui_layout_fill();
    header_style.height = ui_layout_fixed(20.0f);
    const UiWidgetResult header = ui_widget_manager_add(
        &manager, root.handle,
        UiWidgetDesc{2, UiWidgetKind::container, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("Header"), header_style, UiSize{}, 2}});

    UiLayoutStyle body_style = ui_layout_style();
    body_style.width = ui_layout_fill();
    body_style.height = ui_layout_fill();
    body_style.horizontal_alignment = UiLayoutAlignment::stretch;
    body_style.vertical_alignment = UiLayoutAlignment::stretch;
    const UiWidgetResult body = ui_widget_manager_add(
        &manager, root.handle,
        UiWidgetDesc{3, UiWidgetKind::container, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("Body"), body_style, UiSize{}, 3}});

    UiLayoutStyle button_style = ui_layout_style();
    button_style.width = ui_layout_fixed(80.0f);
    button_style.height = ui_layout_fixed(30.0f);
    button_style.horizontal_alignment = UiLayoutAlignment::center;
    button_style.vertical_alignment = UiLayoutAlignment::center;
    const UiWidgetResult button = ui_widget_manager_add(
        &manager, body.handle,
        UiWidgetDesc{4, UiWidgetKind::button,
                     static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable),
                     UiLayoutNodeSpec{utf8_literal("Button"), button_style, UiSize{}, 4}});
    expect(root.ok() && header.ok() && body.ok() && button.ok() &&
               ui_widget_manager_layout_rectangle(&manager, {0, 0, 200.0f, 100.0f}).ok(),
           "layout_engine_widget_tree_layout");

    const UiLayoutNode* header_layout = ui_widget_manager_layout_node(manager, header.handle);
    const UiLayoutNode* body_layout = ui_widget_manager_layout_node(manager, body.handle);
    const UiLayoutNode* button_layout = ui_widget_manager_layout_node(manager, button.handle);
    expect(header_layout && body_layout && button_layout && header_layout->rectangle.top == 0.0f &&
               header_layout->rectangle.bottom == 20.0f && body_layout->rectangle.top == 20.0f &&
               body_layout->rectangle.bottom == 100.0f && button_layout->rectangle.left == 60.0f &&
               button_layout->rectangle.top == 45.0f,
           "layout_engine_measure_arrange_fill_center");
    expect(ui_widget_manager_hit_test(manager, 61.0f, 46.0f).slot == button.handle.slot &&
               ui_widget_manager_move_focus(&manager, false).slot == button.handle.slot &&
               manager.focused.slot == button.handle.slot,
           "widget_manager_hit_and_focus_ownership");

    UiLayoutDiagnostic diagnostics[16]{};
    const UiLayoutValidationResult clean =
        ui_layout_engine_validate(manager.layout, Slice<UiLayoutDiagnostic>{diagnostics, 16});
    expect(clean.clean(), "layout_engine_clean_validation");
    const UiLayoutNodeHandle header_owner = ui_widget_manager_layout_handle(manager, header.handle);
    const UiLayoutNodeHandle body_owner = ui_widget_manager_layout_handle(manager, body.handle);
    const UiLayoutNodeHandle button_owner = ui_widget_manager_layout_handle(manager, button.handle);
    const UiLayoutContentBounds contained_text{button_owner, UiRect{68.0f, 50.0f, 132.0f, 70.0f},
                                               UiLayoutContentKind::text,
                                               ui_layout_content_bounds_collision};
    expect(ui_layout_engine_validate_content_bounds(manager.layout,
                                                    ConstSlice<UiLayoutContentBounds>{&contained_text, 1},
                                                    Slice<UiLayoutDiagnostic>{diagnostics, 16})
               .clean(),
           "layout_engine_render_bounds_contained");
    const UiLayoutContentBounds rendered_content[3]{
        UiLayoutContentBounds{button_owner, UiRect{50.0f, 50.0f, 150.0f, 70.0f}, UiLayoutContentKind::text,
                              ui_layout_content_bounds_collision},
        UiLayoutContentBounds{header_owner, UiRect{10.0f, 18.0f, 50.0f, 22.0f},
                              UiLayoutContentKind::control, ui_layout_content_bounds_collision},
        UiLayoutContentBounds{body_owner, UiRect{10.0f, 18.0f, 50.0f, 22.0f}, UiLayoutContentKind::text,
                              ui_layout_content_bounds_collision},
    };
    const UiLayoutValidationResult rendered_validation = ui_layout_engine_validate_content_bounds(
        manager.layout, ConstSlice<UiLayoutContentBounds>{rendered_content, 3},
        Slice<UiLayoutDiagnostic>{diagnostics, 16});
    bool found_rendered_text_overflow = false;
    bool found_rendered_collision = false;
    for (u32 index = 0; index < rendered_validation.count; ++index) {
        found_rendered_text_overflow = found_rendered_text_overflow ||
                                       diagnostics[index].kind == UiLayoutDiagnosticKind::text_overflow;
        found_rendered_collision = found_rendered_collision ||
                                   diagnostics[index].kind == UiLayoutDiagnosticKind::content_collision;
    }
    expect(found_rendered_text_overflow && found_rendered_collision,
           "layout_engine_reports_actual_render_bounds_failures");
    UiLayoutNode* mutable_header = ui_widget_manager_layout_node(&manager, header.handle);
    mutable_header->rectangle.bottom = 30.0f;
    const UiLayoutValidationResult collision =
        ui_layout_engine_validate(manager.layout, Slice<UiLayoutDiagnostic>{diagnostics, 16});
    bool found_collision = false;
    for (u32 index = 0; index < collision.count; ++index) {
        found_collision = found_collision || diagnostics[index].kind == UiLayoutDiagnosticKind::sibling_collision;
    }
    expect(found_collision, "layout_engine_reports_sibling_collision");

    ui_widget_manager_reset(&manager);
    expect(!ui_widget_manager_resolve(manager, button.handle) && manager.count == 0 &&
               manager.layout.count == 0,
           "widget_manager_generation_reset");

    struct MeasureFixture {
        u32 calls;
        bool oscillate;
        bool fail;
    } measure_fixture{};
    const auto measure_callback = +[](void* opaque, const UiLayoutMeasureRequest& request,
                                      UiSize* size) noexcept -> bool {
        MeasureFixture* fixture = static_cast<MeasureFixture*>(opaque);
        if (!fixture || !size || fixture->fail) {
            return false;
        }
        ++fixture->calls;
        if (fixture->oscillate) {
            size->width = request.constraint.width > 50.0f ? 40.0f : 80.0f;
            size->height = 20.0f;
        } else {
            size->width = request.constraint.width;
            size->height = request.constraint.width < 100.0f ? 40.0f : 20.0f;
        }
        return true;
    };
    expect(ui_widget_manager_set_measure_callback(&manager, measure_callback, &measure_fixture),
           "layout_engine_attach_constrained_measure_callback");
    root_style.flow = UiLayoutFlow::vertical;
    const UiWidgetResult measure_root = ui_widget_manager_add(
        &manager, ui_widget_invalid_handle(),
        UiWidgetDesc{10, UiWidgetKind::container, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("MeasureRoot"), root_style, UiSize{}, 10}});
    UiLayoutStyle text_style = ui_layout_style();
    text_style.width = ui_layout_fill();
    text_style.height = ui_layout_content();
    text_style.horizontal_alignment = UiLayoutAlignment::stretch;
    text_style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_text_content);
    const UiWidgetResult measured_text = ui_widget_manager_add(
        &manager, measure_root.handle,
        UiWidgetDesc{11, UiWidgetKind::text, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("WrappedText"), text_style, UiSize{}, 11}});
    expect(measure_root.ok() && measured_text.ok() &&
               ui_widget_manager_layout_rectangle(&manager, {0, 0, 80.0f, 100.0f}).ok(),
           "layout_engine_constrained_text_layout");
    const UiLayoutNode* measured_layout = ui_widget_manager_layout_node(manager, measured_text.handle);
    const u32 narrow_measure_passes = manager.layout.measure_passes;
    expect(measured_layout && measured_layout->rectangle.right == 80.0f &&
               measured_layout->rectangle.bottom == 40.0f &&
               measured_layout->spec.intrinsic_size.height == 40.0f,
           "layout_engine_constrained_text_narrow_measurement");
    expect(ui_widget_manager_layout_rectangle(&manager, {0, 0, 160.0f, 100.0f}).ok() &&
               manager.layout.measure_passes == narrow_measure_passes + 2,
           "layout_engine_width_change_remeasures_until_stable");
    measured_layout = ui_widget_manager_layout_node(manager, measured_text.handle);
    expect(measured_layout && measured_layout->rectangle.right == 160.0f &&
               measured_layout->rectangle.bottom == 20.0f &&
               measured_layout->measure_constraint.width == 160.0f &&
               measured_layout->spec.intrinsic_size.height == 20.0f,
           "layout_engine_constrained_text_wide_measurement");

    ui_widget_manager_reset(&manager);
    measure_fixture.oscillate = true;
    const UiWidgetResult unstable_root = ui_widget_manager_add(
        &manager, ui_widget_invalid_handle(),
        UiWidgetDesc{20, UiWidgetKind::container, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("UnstableRoot"), root_style, UiSize{}, 20}});
    text_style.width = ui_layout_content();
    text_style.horizontal_alignment = UiLayoutAlignment::start;
    const UiWidgetResult unstable_text = ui_widget_manager_add(
        &manager, unstable_root.handle,
        UiWidgetDesc{21, UiWidgetKind::text, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("UnstableText"), text_style, UiSize{}, 21}});
    const UiLayoutResult unstable_result = ui_widget_manager_layout_rectangle(&manager, {0, 0, 100.0f, 100.0f});
    expect(unstable_root.ok() && unstable_text.ok() &&
               unstable_result.error == UiLayoutError::unstable_layout &&
               manager.layout.unstable_measurements == 1,
           "layout_engine_reports_nonconvergent_measurement");

    ui_widget_manager_reset(&manager);
    expect(ui_widget_manager_set_measure_callback(&manager, nullptr, nullptr),
           "layout_engine_clear_measure_callback_for_wrap");
    UiLayoutStyle wrap_root_style = ui_layout_style();
    wrap_root_style.flow = UiLayoutFlow::vertical;
    wrap_root_style.width = ui_layout_fill();
    wrap_root_style.height = ui_layout_fill();
    const UiWidgetResult wrap_root = ui_widget_manager_add(
        &manager, ui_widget_invalid_handle(),
        UiWidgetDesc{30, UiWidgetKind::container, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("WrapRoot"), wrap_root_style, UiSize{}, 30}});
    UiLayoutStyle wrap_style = ui_layout_style();
    wrap_style.flow = UiLayoutFlow::horizontal_wrap;
    wrap_style.width = ui_layout_fill();
    wrap_style.height = ui_layout_content();
    wrap_style.gap = 6.0f;
    wrap_style.horizontal_alignment = UiLayoutAlignment::stretch;
    const UiWidgetResult wrap = ui_widget_manager_add(
        &manager, wrap_root.handle,
        UiWidgetDesc{31, UiWidgetKind::container, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("Wrap"), wrap_style, UiSize{}, 31}});
    UiLayoutStyle wrap_item_style = ui_layout_style();
    wrap_item_style.width = ui_layout_fixed(80.0f);
    wrap_item_style.height = ui_layout_fixed(24.0f);
    UiWidgetResult wrap_items[4]{};
    for (u32 index = 0; index < 4; ++index) {
        wrap_items[index] = ui_widget_manager_add(
            &manager, wrap.handle,
            UiWidgetDesc{
                32 + index, UiWidgetKind::button,
                static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable),
                UiLayoutNodeSpec{utf8_literal("WrapItem"), wrap_item_style, UiSize{}, 32 + index}});
    }
    const bool wrap_added = wrap_root.ok() && wrap.ok() && wrap_items[0].ok() && wrap_items[1].ok() &&
                            wrap_items[2].ok() && wrap_items[3].ok();
    const UiLayoutResult two_column_wrap = ui_widget_manager_layout_rectangle(&manager, {0, 0, 180.0f, 200.0f});
    const UiLayoutNode* wrap_layout = ui_widget_manager_layout_node(manager, wrap.handle);
    const UiLayoutNode* wrap_first = ui_widget_manager_layout_node(manager, wrap_items[0].handle);
    const UiLayoutNode* wrap_second = ui_widget_manager_layout_node(manager, wrap_items[1].handle);
    const UiLayoutNode* wrap_third = ui_widget_manager_layout_node(manager, wrap_items[2].handle);
    const UiLayoutNode* wrap_fourth = ui_widget_manager_layout_node(manager, wrap_items[3].handle);
    expect(
        wrap_added && two_column_wrap.ok() && wrap_layout && wrap_first && wrap_second && wrap_third &&
            wrap_fourth && ui_rect_height(wrap_layout->rectangle) == 54.0f &&
            wrap_first->rectangle.left == 0.0f && wrap_second->rectangle.left == 86.0f &&
            wrap_third->rectangle.top == 30.0f && wrap_fourth->rectangle.left == 86.0f &&
            ui_layout_engine_validate(manager.layout, Slice<UiLayoutDiagnostic>{diagnostics, 16}).clean(),
        "layout_engine_horizontal_wrap_two_rows_clean");
    const UiLayoutResult single_column_wrap = ui_widget_manager_layout_rectangle(&manager, {0, 0, 80.0f, 200.0f});
    wrap_layout = ui_widget_manager_layout_node(manager, wrap.handle);
    wrap_fourth = ui_widget_manager_layout_node(manager, wrap_items[3].handle);
    expect(
        single_column_wrap.ok() && wrap_layout && wrap_fourth &&
            ui_rect_height(wrap_layout->rectangle) == 114.0f && wrap_fourth->rectangle.top == 90.0f &&
            ui_layout_engine_validate(manager.layout, Slice<UiLayoutDiagnostic>{diagnostics, 16}).clean(),
        "layout_engine_horizontal_wrap_width_reflow");
}

void test_widget_storage_growth() noexcept {
    UiWidgetStorageOwner storage{};
    expect(ui_widget_storage_initialize(&storage, 512, 1), "widget_storage_reserve_and_initial_commit");
    const u32 initial_capacity = storage.capacity;
    u8* const widget_base = storage.widgets.arena.base;
    u8* const layout_base = storage.layout_nodes.arena.base;
    UiWidgetManager manager{};
    expect(ui_widget_manager_initialize(&manager, ui_widget_storage_widgets(storage),
                                        ui_widget_storage_layout_nodes(storage)) &&
               ui_widget_manager_bind_growable_storage(&manager, ui_widget_storage_grow, &storage),
           "widget_manager_bind_growable_storage");
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    const UiWidgetResult root = ui_widget_manager_add(
        &manager, ui_widget_invalid_handle(),
        UiWidgetDesc{1, UiWidgetKind::container, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("Root"), style, UiSize{}, 1}});
    bool children_added = root.ok();
    for (u32 index = 0; index < 200 && children_added; ++index) {
        children_added =
            ui_widget_manager_add_prevalidated(
                &manager, root.handle,
                UiWidgetDesc{index + 2u, UiWidgetKind::container, ui_widget_enabled,
                             UiLayoutNodeSpec{utf8_literal("Child"), style, UiSize{}, index + 2u}})
                .ok();
    }
    expect(children_added && manager.count == 201 && storage.capacity > initial_capacity &&
               storage.capacity >= manager.count && storage.capacity < storage.reserved_capacity,
           "widget_storage_commits_only_on_demand");
    expect(storage.widgets.arena.base == widget_base && storage.layout_nodes.arena.base == layout_base &&
               reinterpret_cast<u8*>(manager.storage.data) == widget_base &&
               reinterpret_cast<u8*>(manager.layout.storage.data) == layout_base &&
               ui_widget_manager_resolve(manager, root.handle) != nullptr,
           "widget_storage_growth_preserves_addresses_and_handles");
    expect(!ui_widget_storage_reserve(&storage, storage.reserved_capacity + 1u),
           "widget_storage_growth_hard_bound");
    ui_widget_storage_release(&storage);
    expect(!storage.widgets.arena.base && !storage.layout_nodes.arena.base && storage.capacity == 0,
           "widget_storage_release");

    UiVirtualRowStorageOwner row_storage{};
    expect(ui_virtual_row_storage_initialize(&row_storage, 256, 1, sizeof(u64), alignof(u64)),
           "virtual_row_storage_initial_commit");
    u64* const row_base = ui_virtual_row_storage_rows<u64>(row_storage).data;
    row_base[0] = 0xA5A5A5A5A5A5A5A5ull;
    expect(ui_virtual_row_storage_grow(&row_storage, 129) && row_storage.rows.capacity >= 129 &&
               ui_virtual_row_storage_rows<u64>(row_storage).data == row_base &&
               row_base[0] == 0xA5A5A5A5A5A5A5A5ull,
           "virtual_row_storage_growth_preserves_address_and_content");
    expect(!ui_virtual_row_storage_grow(&row_storage, 257), "virtual_row_storage_hard_bound");
    ui_virtual_row_storage_release(&row_storage);
    expect(!row_storage.rows.arena.base, "virtual_row_storage_release");

    UiWidgetStorageOwner popup_storage{};
    PopupScene popup{};
    PopupItem popup_items[4]{};
    PopupRowWidget popup_rows[4]{};
    const PopupItem popup_source[] = {
        PopupItem{utf8_literal("First"), 1, popup_item_enabled, 0, 0, 0},
        PopupItem{utf8_literal("Second"), 2, popup_item_enabled, 0, 0, 0},
        PopupItem{utf8_literal("Third"), 3, popup_item_enabled, 0, 0, 0},
    };
    const bool popup_ready =
        ui_widget_storage_initialize(&popup_storage, 10, 2) &&
        popup_scene_initialize(
            &popup, Slice<PopupItem>{popup_items, 4}, Slice<PopupRowWidget>{popup_rows, 4},
            ui_widget_storage_widgets(popup_storage), ui_widget_storage_layout_nodes(popup_storage)) &&
        ui_widget_manager_bind_growable_storage(&popup.widgets, ui_widget_storage_grow, &popup_storage);
    const PopupSceneResult popup_opened =
        popup_ready
            ? popup_scene_open(&popup, PopupKind::context_menu, 1, ConstSlice<PopupItem>{popup_source, 3},
                               UiRect{16.0f, 16.0f, 20.0f, 20.0f}, UiRect{0.0f, 0.0f, 640.0f, 480.0f},
                               220.0f)
            : PopupSceneResult{PopupSceneError::invalid_input, false};
    expect(popup_ready && popup_opened.ok() && popup_storage.capacity >= 8 && popup.widgets.count == 8,
           "popup_scene_grows_stable_widget_storage_on_demand");
    ui_widget_storage_release(&popup_storage);
}

void test_member_list_scene() noexcept {
    constexpr u32 source_count = 100'000;
    auto* records = static_cast<UserRecord*>(VirtualAlloc(
        nullptr, sizeof(UserRecord) * source_count, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* order = static_cast<u32*>(
        VirtualAlloc(nullptr, sizeof(u32) * source_count, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* member_records = static_cast<GuildMemberRecord*>(
        VirtualAlloc(nullptr, sizeof(GuildMemberRecord) * source_count, MEM_RESERVE | MEM_COMMIT,
                     PAGE_READWRITE));
    auto* member_order = static_cast<u32*>(
        VirtualAlloc(nullptr, sizeof(u32) * source_count, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* projected_rows = static_cast<MemberListRow*>(VirtualAlloc(
        nullptr, sizeof(MemberListRow) * source_count, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    u8 names[2]{};
    void* const sources[]{records, order, member_records, member_order, projected_rows};
    const auto release_sources = [&sources]() noexcept {
        for (void* source : sources)
            if (source)
                VirtualFree(source, 0, MEM_RELEASE);
    };
    if (!records || !order || !member_records || !member_order || !projected_rows) {
        release_sources();
        expect(false, "member_list_100k_source_allocation");
        return;
    }
    for (u32 index = 0; index < source_count; ++index) {
        records[index].id = static_cast<u64>(index) + 1;
        records[index].occupied = true;
        order[index] = index;
        member_records[index].guild_id = 1;
        member_records[index].user_id = static_cast<u64>(index) + 1;
        member_records[index].occupied = true;
        member_order[index] = index;
        projected_rows[index] = MemberListRow{
            static_cast<u64>(index) + 1,      kMemberListOnlineGroup,   index, 0, 0,
            GuildMemberStatus::online, MemberListRowKind::member};
    }
    UserStore users{};
    users.records = records;
    users.order = order;
    users.names = names;
    users.capacity = source_count;
    users.count = source_count;
    users.name_bank_bytes = 1;
    users.revision = 1;
    GuildMemberStore members{};
    members.records = member_records;
    members.order = member_order;
    members.capacity = source_count;
    members.slot_extent = source_count;
    members.count = source_count;
    members.revision = 1;
    MemberListView projection{};
    projection.rows = Slice<MemberListRow>{projected_rows, source_count};
    projection.count = source_count;
    projection.member_count = source_count;
    projection.revision = 1;
    GuildRecord guild{};
    ChatStateStore state{};
    state.guilds = &guild;

    UiVirtualRowStorageOwner row_storage{};
    UiWidgetStorageOwner widget_storage{};
    MemberListScene scene{};
    const UiRect viewport{1'000.0f, 48.0f, 1'240.0f, 768.0f};
    bool initialized =
        ui_virtual_row_storage_initialize(&row_storage, 32, 1, sizeof(MemberListRowWidget),
                                          alignof(MemberListRowWidget)) &&
        ui_widget_storage_initialize(&widget_storage, 1 + 32 * kMemberListWidgetsPerRow, 1) &&
        member_list_scene_initialize(&scene, ui_virtual_row_storage_rows<MemberListRowWidget>(row_storage),
                                     ui_widget_storage_widgets(widget_storage),
                                     ui_widget_storage_layout_nodes(widget_storage));
    if (initialized) {
        scene.row_storage = &row_storage;
        initialized = ui_widget_manager_bind_growable_storage(&scene.widgets, ui_widget_storage_grow,
                                                              &widget_storage);
    }
    const MemberListSceneResult first =
        initialized
            ? member_list_scene_update(&scene, &projection, &members, &users, &state, viewport, 0.0f)
            : MemberListSceneResult{};
    const MemberListSceneResult tail =
        first.ok() ? member_list_scene_update(&scene, &projection, &members, &users, &state, viewport,
                                              static_cast<float>(source_count) * kMemberListRowHeight)
                   : MemberListSceneResult{};
    const UiLayoutNode* row_layout = nullptr;
    const UiLayoutNode* status_layout = nullptr;
    if (tail.ok()) {
        for (u32 index = 0; index < scene.count; ++index) {
            const UiLayoutNode* candidate = member_list_scene_layout_node(scene, scene.rows[index].widget);
            if (candidate && candidate->rectangle.top >= viewport.top &&
                candidate->rectangle.bottom <= viewport.bottom) {
                row_layout = candidate;
                status_layout = member_list_scene_layout_node(scene, scene.rows[index].status);
                break;
            }
        }
    }
    const MemberListHit hit =
        row_layout
            ? member_list_scene_hit_test(scene,
                                         (row_layout->rectangle.left + row_layout->rectangle.right) * 0.5f,
                                         (row_layout->rectangle.top + row_layout->rectangle.bottom) * 0.5f)
            : MemberListHit{};
    if (hit.user_id) {
        UserAppearance appearance{};
        appearance.user_id = hit.user_id;
        appearance.fields = appearance_nameplate;
        appearance.nameplate_sku = 123;
        const bool inserted = user_appearance_upsert(&users.appearance, appearance);
        u32 index = 0;
        while (index < scene.count && scene.rows[index].user_id != hit.user_id) ++index;
        const UiWidgetHandle old_hover = scene.widgets.hovered;
        scene.widgets.hovered = ui_widget_invalid_handle();
        ProfileMedia idle{}, hovered{}, child_hover{}, stale{};
        const bool idle_ok = inserted && member_media_resolve(scene, index, MemberMedia::nameplate, &idle);
        if (index < scene.count) scene.widgets.hovered = scene.rows[index].widget;
        BOOL motion = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &motion, 0);
        scene.system_animation_off = motion == FALSE;
        const bool hover_ok = inserted && member_media_resolve(scene, index, MemberMedia::nameplate, &hovered);
        scene.system_animation_off = true;
        ProfileMedia motion_off{};
        const bool off_ok = inserted && member_media_resolve(scene, index, MemberMedia::nameplate, &motion_off);
        scene.system_animation_off = motion == FALSE;
        if (index < scene.count) scene.widgets.hovered = scene.rows[index].avatar;
        const bool child_ok = inserted && member_media_resolve(scene, index, MemberMedia::nameplate, &child_hover);
        ++scene.widgets.hovered.generation;
        const bool stale_ok = inserted && member_media_resolve(scene, index, MemberMedia::nameplate, &stale);
        expect(idle_ok && !(idle.key.flags & media_asset_flag_animated) &&
                   utf8_equal(idle.url.view(), utf8_literal("https://cdn.discordapp.com/media/v1/collectibles-shop/123/static")),
               "member_nameplate_without_hover_uses_static_media");
        expect(hover_ok && ((hovered.key.flags & media_asset_flag_animated) != 0) == (motion != FALSE),
               "member_nameplate_hover_follows_published_windows_motion_preference");
        expect(off_ok && !(motion_off.key.flags & media_asset_flag_animated),
               "member_nameplate_hover_stays_static_when_windows_motion_is_off");
        expect(child_ok && media_asset_key_equal(hovered.key, child_hover.key),
               "member_nameplate_child_hover_matches_row_hover");
        expect(stale_ok && media_asset_key_equal(idle.key, stale.key),
               "member_nameplate_stale_hover_uses_static_media");
        // Avatar decorations: a still at the drawn pixel size at rest (an animated preset is returned at its
        // native 288 px whatever size says), the animation only under the pointer, its still kept behind it.
        {
            UserAppearance ornament{};
            ornament.user_id = hit.user_id;
            ornament.fields = appearance_decoration;
            constexpr auto asset = utf8_literal("a_0123456789abcdef0123456789abcdef");
            copy_bytes(ornament.decoration.data, asset.data, asset.bytes);
            ornament.decoration.bytes = static_cast<u16>(asset.bytes);
            const bool decorated = user_appearance_upsert(&users.appearance, ornament);
            const auto drawn_size = [](const ProfileMedia& media) noexcept {
                const Utf8View url = media.url.view();
                u32 size = 0;
                for (u32 i = 0; i + 5 < url.bytes; ++i)
                    if (url.data[i] == 's' && url.data[i + 1] == 'i' && url.data[i + 2] == 'z' &&
                        url.data[i + 3] == 'e' && url.data[i + 4] == '=')
                        for (u32 j = i + 5; j < url.bytes && url.data[j] >= '0' && url.data[j] <= '9'; ++j)
                            size = size * 10 + static_cast<u32>(url.data[j] - '0');
                return size;
            };
            const auto ends_with = [](const ProfileMedia& media, Utf8View tail) noexcept {
                const Utf8View url = media.url.view();
                return url.bytes >= tail.bytes &&
                       utf8_equal({url.data + url.bytes - tail.bytes, tail.bytes}, tail);
            };
            const bool saved_motion_off = scene.system_animation_off;
            scene.system_animation_off = false;
            scene.widgets.hovered = ui_widget_invalid_handle();
            ProfileMedia rest{}, rest_still{}, under{}, under_still{}, high_dpi{};
            const bool rest_ok = decorated && member_media_resolve(scene, index, MemberMedia::decoration, &rest);
            const bool rest_has_no_still =
                !member_media_resolve(scene, index, MemberMedia::decoration_still, &rest_still);
            scene.widgets.hovered = scene.rows[index].widget;
            const bool under_ok = decorated && member_media_resolve(scene, index, MemberMedia::decoration, &under);
            const bool still_ok = member_media_resolve(scene, index, MemberMedia::decoration_still, &under_still);
            scene.widgets.hovered = ui_widget_invalid_handle();
            scene.media_dpi = 192;
            const bool dpi_ok = member_media_resolve(scene, index, MemberMedia::decoration, &high_dpi);
            scene.media_dpi = 96;
            scene.system_animation_off = saved_motion_off;
            const float width = ui_rect_width(rest.rectangle);
            const u32 at_rest = drawn_size(rest);
            expect(rest_ok && rest_has_no_still && ends_with(rest, utf8_literal("&passthrough=false")) &&
                       !(rest.key.flags & media_asset_flag_animated) && at_rest >= width && at_rest < 2 * width &&
                       at_rest < 288,
                   "member_decoration_at_rest_is_a_still_at_the_drawn_size_not_the_native_288");
            expect(under_ok && still_ok && ends_with(under, utf8_literal("&passthrough=true")) &&
                       (under.key.flags & media_asset_flag_animated) && ends_with(under_still, utf8_literal("&passthrough=false")) &&
                       media_asset_key_equal(under_still.key, rest.key),
                   "member_decoration_animates_under_the_pointer_and_keeps_its_still_key");
            expect(dpi_ok && drawn_size(high_dpi) >= 2 * width && drawn_size(high_dpi) > at_rest &&
                       !media_asset_key_equal(high_dpi.key, rest.key),
                   "member_decoration_size_follows_the_display_dpi_with_its_own_key");
        }
        scene.widgets.hovered = old_hover;
        user_appearance_release(&users.appearance);
    }
    // A plugin member badge is a click target of its own, ahead of the row's profile.
    g_ui_plugin_slot_query = [](u32 surface, u64, u64, u64 user, u64, Slice<UiPluginSlotNode> output) noexcept -> u32 {
        if (surface != kUiPluginSlotMemberBadge || !user || !output.count) return 0;
        output[0] = {61, 5, utf8_literal("Mod"), {}, kUiPluginSlotNoIcon, ui_plugin_node_button, 1};
        return 1;
    };
    u32 badge_index = 0;
    while (badge_index < scene.count && scene.rows[badge_index].user_id != hit.user_id) ++badge_index;
    const auto member_badge = member_list_plugin_badge(scene, badge_index);
    const UiRect badge_rect = member_badge.badge.rectangle;
    u64 badge_user = 0;
    const auto badge_hit = member_list_plugin_hit(scene, (badge_rect.left + badge_rect.right) * .5f,
                                                  (badge_rect.top + badge_rect.bottom) * .5f, &badge_user);
    const bool name_misses =
        row_layout && !member_list_plugin_hit(scene, row_layout->rectangle.left + 4,
                                              (row_layout->rectangle.top + row_layout->rectangle.bottom) * .5f,
                                              nullptr).action_id;
    g_ui_plugin_slot_query = nullptr;
    expect(hit.user_id && ui_rect_has_area(badge_rect) && badge_hit.action_id == 5 && badge_hit.owner_id == 61 &&
               badge_user == hit.user_id && name_misses,
           "member_plugin_badge_hit_reports_its_action_and_row_user_only_inside_the_badge");
    {
        // Navigation surfaces: channel row badge hides when the name would
        // drop under its keep width; the rail badge takes the corner the
        // mention badge leaves free; account buttons stay left of the mic.
        g_ui_plugin_slot_query = [](u32 surface, u64 channel, u64, u64, u64 guild,
                                    Slice<UiPluginSlotNode> output) noexcept -> u32 {
            if (!output.count) return 0;
            if (surface == kUiPluginSlotChannelRow && channel == 7)
                output[0] = {70, 3, utf8_literal("Live"), utf8_literal("Streaming now"), kUiPluginSlotNoIcon,
                             ui_plugin_node_button, 2};
            else if (surface == kUiPluginSlotGuildRail && guild == 9)
                output[0] = {71, 0, {}, utf8_literal("Pinned server"), static_cast<u16>(UiIcon::pin),
                             ui_plugin_node_icon, 1};
            else if (surface == kUiPluginSlotAccountPanel && output.count >= 2) {
                output[0] = {72, 1, {}, utf8_literal("A"), kUiPluginSlotNoIcon, ui_plugin_node_button, 0};
                output[1] = {72, 2, {}, utf8_literal("B"), kUiPluginSlotNoIcon, ui_plugin_node_button, 0};
                return 2;
            } else
                return 0;
            return 1;
        };
        const UiRect label{40, 0, 220, 32};
        UiPluginSlotBadge row_badge{}, narrow{}, other{};
        const bool row_ok = ui_plugin_slot_row_badge(kUiPluginSlotChannelRow, 7, {}, label, 16,
                                                     kUiPluginSlotRowNameKeep, &row_badge);
        const bool narrow_hidden = !ui_plugin_slot_row_badge(kUiPluginSlotChannelRow, 7, {}, {40, 0, 110, 32}, 16,
                                                             kUiPluginSlotRowNameKeep, &narrow);
        const bool other_empty = !ui_plugin_slot_row_badge(kUiPluginSlotChannelRow, 8, {}, label, 16,
                                                           kUiPluginSlotRowNameKeep, &other);
        expect(row_ok && row_badge.badge.rectangle.right == label.right && row_badge.action_id == 3 &&
                   row_badge.badge.rectangle.top == 6 && row_badge.badge.rectangle.bottom == 26 &&
                   row_badge.badge.rectangle.left >= label.left + kUiPluginSlotRowNameKeep && narrow_hidden &&
                   !narrow.badge.label.bytes && other_empty,
               "channel_row_plugin_badge_right_aligns_and_hides_when_the_name_needs_the_room");
        const UiRect icon{12, 100, 60, 148};
        UiPluginSlotCorner corner{};
        const bool corner_ok = ui_plugin_slot_corner(kUiPluginSlotGuildRail, 9, icon, &corner);
        const UiRect mention = ui_mention_badge_guild_rectangle(icon, 120);
        expect(corner_ok && corner.action_id == 0 && corner.icon == static_cast<u16>(UiIcon::pin) &&
                   utf8_equal(corner.label, utf8_literal("Pinned server")) && corner.rectangle.right == icon.right &&
                   corner.rectangle.top == icon.top && mention.right == icon.right && mention.bottom == icon.bottom &&
                   corner.rectangle.bottom <= mention.top && !ui_plugin_slot_corner(kUiPluginSlotGuildRail, 8, icon, &corner),
               "guild_rail_plugin_badge_takes_the_top_corner_clear_of_the_mention_badge");
        const UiRect footer{0, 500, 240, 552};
        UiPluginSlotNode account[kUiPluginSlotAccountButtons]{};
        const u32 account_count = ui_plugin_slot_account_buttons({account, kUiPluginSlotAccountButtons});
        const UiRect nearest = ui_plugin_slot_account_button(footer, 0), next = ui_plugin_slot_account_button(footer, 1);
        const UiRect mic = ui_voice_account_button(footer, UiVoiceControl::mute);
        expect(account_count == 2 && nearest.right + 4 == mic.left && next.right + 4 == nearest.left &&
                   ui_rect_height(nearest) == 32 && ui_plugin_slot_account_width() == 72,
               "account_panel_plugin_buttons_sit_left_of_the_microphone");
        g_ui_plugin_slot_query = nullptr;
    }
    UiRect member_context_anchor{};
    expect(hit.user_id && member_list_scene_focus_user(&scene, hit.user_id) &&
               ui_widget_manager_context_anchor(scene.widgets, &member_context_anchor) && row_layout &&
               ui_rect_equal(member_context_anchor, row_layout->rectangle),
           "member_context_anchor_uses_focused_virtual_row");
    UiLayoutDiagnostic diagnostics[64]{};
    const UiLayoutValidationResult validation =
        ui_layout_engine_validate(scene.widgets.layout, Slice<UiLayoutDiagnostic>{diagnostics, 64});
    PaintStream status_paint{};
    paint_stream_reset(&status_paint, viewport.right, viewport.bottom);
    const bool status_overlay =
        shell_presentation_build_member_status_overlay(&scene, kDefaultTheme, &status_paint);
    bool status_badge_above_avatar_media = false;
    if (status_layout) {
        for (u32 index = 0; index < status_paint.count; ++index) {
            const PaintQuad& quad = status_paint.quads[index];
            status_badge_above_avatar_media =
                status_badge_above_avatar_media ||
                (quad.left == status_layout->rectangle.left - 2.0f &&
                 quad.top == status_layout->rectangle.top - 2.0f &&
                 quad.right == status_layout->rectangle.right + 2.0f &&
                 quad.bottom == status_layout->rectangle.bottom + 2.0f && quad.border_width == 2.0f);
        }
    }
    expect(first.ok() && tail.ok(), "member_list_100k_tail_materialization");
    expect(scene.source_count == source_count && scene.first > 99'000, "member_list_100k_source_range");
    expect(scene.count <= 32 && scene.widgets.count <= 1 + 32 * kMemberListWidgetsPerRow,
           "member_list_100k_bounded_widgets");
    expect(row_storage.rows.capacity >= scene.count &&
               row_storage.rows.capacity < row_storage.rows.reserved_capacity,
           "member_list_scene_rows_commit_on_viewport_demand");
    expect(validation.clean(), "member_list_100k_layout_validation");
    expect(status_overlay && status_paint.rejected == 0 && status_badge_above_avatar_media,
           "member_status_badge_has_deterministic_post_media_geometry");

    const GuildMemberStatus badge_statuses[] = {GuildMemberStatus::unknown,
        GuildMemberStatus::online, GuildMemberStatus::idle,
        GuildMemberStatus::dnd, GuildMemberStatus::offline};
    const Color badge_colors[] = {kDefaultTheme.tokens.status.offline, kDefaultTheme.tokens.status.online,
        kDefaultTheme.tokens.status.idle, kDefaultTheme.tokens.status.dnd, kDefaultTheme.tokens.status.offline};
    for (u32 test = 0; test < 5; ++test) {
        for (u32 row = 0; row < scene.count; ++row) {
            if (scene.rows[row].kind == MemberListRowKind::member)
                members.records[scene.rows[row].source_row.member_slot].status = badge_statuses[test];
        }
        paint_stream_reset(&status_paint, viewport.right, viewport.bottom);
        bool colored = shell_presentation_build_member_status_overlay(&scene, kDefaultTheme, &status_paint) &&
                       status_paint.count != 0;
        bool found_status=false;
        for (u32 quad = 0; quad < status_paint.count; ++quad) {
            const auto& fill=status_paint.quads[quad].fill;
            const bool status_color=bytes_equal(&fill,&badge_colors[test],sizeof(Color));
            found_status=found_status||status_color;
            colored=colored && (status_color || bytes_equal(&fill,&kDefaultTheme.base_lower,sizeof(Color)));
        }
        colored=colored && found_status;
        expect(colored, "member_status_gpu_paint_matches_presence_unknown_is_not_green");
    }

    const auto member_first = member_list_scene_navigate(&scene, UiListMove::first);
    bool member_keyboard_walk = member_first.user_id == 1 && member_first.source_index == 0;
    for (u32 i = 1; i < 64; ++i) {
        const auto next = member_list_scene_navigate(&scene, UiListMove::next);
        member_keyboard_walk = member_keyboard_walk && next.user_id == i + 1 &&
            scene.count <= 32 && ui_widget_manager_context_anchor(scene.widgets, &member_context_anchor);
    }
    expect(member_keyboard_walk && member_list_scene_navigate(&scene, UiListMove::last).user_id == source_count &&
               scene.first > 99'000 && scene.count <= 32 &&
               !member_list_scene_navigate(&scene, UiListMove::next).user_id,
           "member_keyboard_navigation_crosses_windows_and_reveals_100k_tail_with_bounded_widgets");
    projected_rows[1].kind = MemberListRowKind::group;
    projected_rows[1].user_id = 0;
    ++projection.revision;
    expect(member_list_scene_navigate(&scene, UiListMove::first).user_id == 1 &&
               member_list_scene_navigate(&scene, UiListMove::next).user_id == 3 &&
               member_list_scene_navigate(&scene, UiListMove::previous).user_id == 1,
           "member_keyboard_focus_skips_group_headers_in_both_directions");
    expect(member_list_scene_reveal_user(&scene, source_count) &&
               scene.focused_user_id == source_count &&
               ui_widget_manager_context_anchor(scene.widgets, &member_context_anchor) &&
               !member_list_scene_reveal_user(&scene, source_count + 1),
           "member_context_reveal_restores_far_logical_identity_and_rejects_removed_users");
    bool member_pages = member_list_scene_navigate(&scene, UiListMove::first).user_id == 1;
    u64 previous_member = 1;
    for (u32 i = 0; i < 30; ++i) {
        const float before = scene.scroll_dips;
        const auto page = member_list_scene_page(&scene, false);
        member_pages = member_pages && page.user_id > previous_member && scene.scroll_dips >= before &&
            scene.count <= 32 && ui_widget_manager_context_anchor(scene.widgets, &member_context_anchor);
        previous_member = page.user_id;
    }
    expect(member_pages && member_list_scene_page(&scene, true).user_id < previous_member &&
               member_list_scene_navigate(&scene, UiListMove::last).user_id == source_count &&
               member_list_scene_page(&scene, false).user_id == source_count &&
               member_list_scene_page(&scene, true).user_id < source_count,
           "member_page_navigation_uses_viewport_extent_and_clamps_100k_source_boundaries");
    projection.count = source_count + 1;
    expect(!member_list_scene_navigate(&scene, UiListMove::first).user_id,
           "member_navigation_rejects_inconsistent_source_capacity");
    release_sources();
    ui_virtual_row_storage_release(&row_storage);
    ui_widget_storage_release(&widget_storage);
}

void test_popup_scene() noexcept {
    PopupItem item_storage[8]{};
    PopupRowWidget row_storage[8]{};
    UiWidget widgets[2 + 8 * kPopupWidgetsPerItem]{};
    UiLayoutNode layout[2 + 8 * kPopupWidgetsPerItem]{};
    PopupScene scene{};
    const PopupItem items[] = {
        PopupItem{utf8_literal("Reply"), 11, popup_item_enabled, 0},
        PopupItem{utf8_literal("Copy text"), 12, popup_item_enabled, 0},
        PopupItem{utf8_literal("Delete message"), 13,
                  static_cast<u16>(popup_item_danger | popup_item_separator_before), 0},
    };
    const UiRect viewport{0.0f, 0.0f, 320.0f, 240.0f};
    const UiRect anchor{300.0f, 218.0f, 302.0f, 220.0f};
    const bool initialized = popup_scene_initialize(
        &scene, Slice<PopupItem>{item_storage, 8}, Slice<PopupRowWidget>{row_storage, 8},
        Slice<UiWidget>{widgets, 2 + 8 * kPopupWidgetsPerItem},
        Slice<UiLayoutNode>{layout, 2 + 8 * kPopupWidgetsPerItem});
    const PopupSceneResult opened =
        initialized ? popup_scene_open(&scene, PopupKind::context_menu, 44, ConstSlice<PopupItem>{items, 3},
                                       anchor, viewport, 180.0f, false)
                    : PopupSceneResult{};
    expect(opened.ok() && opened.changed && scene.active &&
               scene.rectangle.left >= viewport.left + kPopupOuterMargin &&
               scene.rectangle.right <= viewport.right - kPopupOuterMargin &&
               ui_rect_width(scene.rectangle) == 180.0f && scene.rectangle.bottom <= anchor.top,
           "popup_scene_clamped_above_anchor");

    const UiLayoutNode* second =
        opened.ok() ? popup_scene_layout_node(scene, scene.rows[1].widget) : nullptr;
    const PopupHit hit =
        second ? popup_scene_hit_test(scene, (second->rectangle.left + second->rectangle.right) * 0.5f,
                                      (second->rectangle.top + second->rectangle.bottom) * 0.5f)
               : PopupHit{};
    bool hover_changed = false;
    const bool hovered =
        second &&
        popup_scene_set_hover(&scene, (second->rectangle.left + second->rectangle.right) * 0.5f,
                              (second->rectangle.top + second->rectangle.bottom) * 0.5f, &hover_changed);
    expect(hit.inside && hit.enabled && hit.action == 12 && hovered && hover_changed &&
               popup_scene_selected_action(scene) == 12,
           "popup_scene_hit_hover_action");

    UiRect hovered_anchor{};
    const auto* disabled = popup_scene_layout_node(scene, scene.rows[2].widget);
    expect(ui_widget_manager_context_anchor(scene.widgets, &hovered_anchor) && second &&
               ui_rect_equal(hovered_anchor, second->rectangle) && disabled &&
               popup_scene_set_hover(&scene, disabled->rectangle.left + 1,
                                     disabled->rectangle.top + 1, &hover_changed) &&
               !hover_changed && !scene.widgets.hovered.generation &&
               popup_scene_selected_action(scene) == 12 &&
               popup_scene_set_hover(&scene, -1, -1, &hover_changed) && !hover_changed &&
               popup_scene_selected_action(scene) == 12 &&
               ui_widget_manager_context_anchor(scene.widgets, &hovered_anchor) &&
               ui_rect_equal(hovered_anchor, second->rectangle),
           "popup_pointer_disabled_row_and_leave_preserve_visible_keyboard_selection_and_focus");

    bool selection_changed = false;
    expect(popup_scene_move_selection(&scene, 1, &selection_changed) && selection_changed &&
               scene.selected_index == 0 && popup_scene_selected_action(scene) == 11,
           "popup_scene_keyboard_skips_disabled");

    PaintStream paint{};
    paint_stream_reset(&paint, viewport.right, viewport.bottom);
    UiTextCommand commands[8]{};
    UiTextCommandStream text{};
    UiTextOutput text_output{&text, nullptr};
    const bool presented = ui_text_command_stream_initialize(&text, Slice<UiTextCommand>{commands, 8}) &&
                           popup_presentation_build(scene, kDefaultTheme, &paint) &&
                           popup_text_presentation_build_output(scene, kDefaultTheme, &text_output);
    UiLayoutDiagnostic diagnostics[32]{};
    const UiLayoutValidationResult validation =
        ui_layout_engine_validate(scene.widgets.layout, Slice<UiLayoutDiagnostic>{diagnostics, 32});
    expect(presented && paint.count >= 3 && text.count == 3 && validation.clean(),
           "popup_scene_presentation_and_layout_validation");
    expect(popup_scene_close(&scene) && !scene.active && scene.closes == 1, "popup_scene_retained_close");

    PopupItem command_items[8]{};
    command_items[0] = {utf8_literal("All"), 31, popup_item_enabled | popup_item_command_category};
    for (u32 index = 1; index < 8; ++index)
        command_items[index] = {utf8_literal("/command"), 31 + index,
                               popup_item_enabled | popup_item_command_row};
    const auto retained = popup_scene_open(&scene, PopupKind::context_menu, 44, {items, 3},
                                           anchor, viewport, 180.0f);
    const UiWidgetHandle retained_row = scene.rows[0].widget;
    const u32 retained_revision = scene.revision;
    const UiRect command_anchor{16, 540, 304, 588};
    const UiRect command_viewport{0, 0, 320, 600};
    const auto rejected_commands = popup_scene_open(&scene, PopupKind::autocomplete, 77,
        {command_items, 8}, command_anchor, command_viewport, 288.0f, true);
    expect(retained.ok() && rejected_commands.error == PopupSceneError::capacity &&
               !rejected_commands.changed && scene.active && scene.owner_id == 44 &&
               scene.item_count == 3 && scene.revision == retained_revision &&
               popup_scene_selected_action(scene) == 11 &&
               ui_widget_manager_resolve(scene.widgets, retained_row),
           "popup_command_viewports_preflight_capacity_without_destroying_retained_popup");

    UiWidget command_widgets[4 + 8 * kPopupWidgetsPerItem]{};
    UiLayoutNode command_layout[4 + 8 * kPopupWidgetsPerItem]{};
    PopupItem command_item_storage[8]{};
    PopupRowWidget command_row_storage[8]{};
    PopupScene command_scene{};
    const bool command_initialized = popup_scene_initialize(&command_scene, {command_item_storage, 8},
        {command_row_storage, 8}, {command_widgets, 4 + 8 * kPopupWidgetsPerItem},
        {command_layout, 4 + 8 * kPopupWidgetsPerItem});
    const auto opened_commands = command_initialized ? popup_scene_open(&command_scene,
        PopupKind::autocomplete, 77, {command_items, 8}, command_anchor, command_viewport,
        288.0f, true) : PopupSceneResult{PopupSceneError::invalid_input, false};
    PopupItemLayout command_last{};
    expect(opened_commands.ok() && command_scene.widgets.count == 4 + 8 * kPopupWidgetsPerItem &&
               popup_scene_resolve_item_layout(command_scene, 7, &command_last) && command_last.visible,
           "popup_command_viewports_fit_exact_full_tree_capacity");
    command_items[6].flags = command_items[7].flags =
        popup_item_enabled | popup_item_command_category;
    const auto command_open_lifetime = command_scene.opens;
    const auto compact_commands =
        popup_scene_open(&command_scene, PopupKind::autocomplete, 77, {command_items, 8},
                         {16, 112, 304, 160}, {0, 0, 320, 128}, 288.0f, true);
    expect(compact_commands.ok() && command_scene.opens == command_open_lifetime,
           "popup_live_suggestion_resize_preserves_entrance_lifetime");
    const UiWidget* command_body_row =
        ui_widget_manager_resolve(command_scene.widgets, command_scene.rows[5].widget);
    const UiWidget* command_rail_row = ui_widget_manager_resolve(command_scene.widgets,
                                                                command_scene.rows[7].widget);
    const auto* command_body = command_body_row ? popup_scene_layout_node(command_scene,
        ui_widget_manager_parent(command_scene.widgets, command_scene.rows[5].widget)) : nullptr;
    const auto* command_rail = command_rail_row ? popup_scene_layout_node(command_scene,
        ui_widget_manager_parent(command_scene.widgets, command_scene.rows[7].widget)) : nullptr;
    const u16 command_generation = command_scene.widgets.generation;
    bool command_revealed = false;
    expect(compact_commands.ok() && command_body && command_rail &&
               popup_scene_select_index(&command_scene, 5, &command_revealed) && command_revealed &&
               command_body->scroll_offset.y > 0 && command_rail->scroll_offset.y == 0 &&
               command_scene.scroll_offset == 0 &&
               popup_scene_resolve_item_layout(command_scene, 5, &command_last) && command_last.visible &&
               command_last.row->rectangle.top >= command_last.clip_rectangle.top &&
               command_last.row->rectangle.bottom <= command_last.clip_rectangle.bottom,
           "popup_command_body_selection_reveals_retained_row_without_scrolling_category_rail");
    const float command_body_offset = command_body ? command_body->scroll_offset.y : 0;
    expect(popup_scene_select_index(&command_scene, 7, &command_revealed) && command_revealed &&
               command_rail && command_rail->scroll_offset.y > 0 && command_body &&
               command_body->scroll_offset.y == command_body_offset &&
               popup_scene_resolve_item_layout(command_scene, 7, &command_last) && command_last.visible &&
               command_last.row->rectangle.top >= command_last.clip_rectangle.top &&
               command_last.row->rectangle.bottom <= command_last.clip_rectangle.bottom &&
               popup_scene_move_selection(&command_scene, 1, &command_revealed) && command_revealed &&
               command_scene.selected_index == 0 && command_rail->scroll_offset.y <= kCommandRailLead &&
               command_body->scroll_offset.y == command_body_offset &&
               command_scene.widgets.generation == command_generation &&
               !popup_scene_scroll(&command_scene, 10, &command_revealed) &&
               !popup_scene_page_selection(&command_scene, false, &command_revealed),
           "popup_command_rail_selection_wrap_reveals_independently_with_stable_handles");
    popup_scene_close(&command_scene);
    expect(popup_scene_open(&command_scene, PopupKind::autocomplete, 77, {command_items, 8}, command_anchor,
                            command_viewport, 288, true)
                   .ok() &&
               command_scene.opens == command_open_lifetime + 1 &&
               popup_scene_open(&command_scene, PopupKind::autocomplete, 78, {command_items, 8},
                                command_anchor, command_viewport, 288, true)
                   .ok() &&
               command_scene.opens == command_open_lifetime + 2,
           "popup_suggestion_close_reopen_and_channel_replacement_start_fresh_lifetimes");
    popup_scene_close(&command_scene);

    expect(popup_scene_open(nullptr, PopupKind::context_menu, 44, {items, 3}, anchor,
                            viewport, 180.0f).error == PopupSceneError::invalid_input &&
               popup_scene_open(&scene, static_cast<PopupKind>(255), 44, {items, 3}, anchor,
                                viewport, 180.0f).error == PopupSceneError::invalid_input,
           "popup_invalid_scene_and_unknown_kind_fail_without_mutating_tree");

    const PopupItem autocomplete_items[] = {
        PopupItem{utf8_literal("first"), 21, popup_item_enabled, 0},
        PopupItem{utf8_literal("second"), 22, popup_item_enabled, 0},
        PopupItem{utf8_literal("third"), 23, popup_item_enabled, 0},
        PopupItem{utf8_literal("fourth"), 24, popup_item_enabled, 0},
        PopupItem{utf8_literal("fifth"), 25, popup_item_enabled, 0},
        PopupItem{utf8_literal("sixth"), 26, popup_item_enabled, 0},
        PopupItem{utf8_literal("seventh"), 27, popup_item_enabled, 0},
        PopupItem{utf8_literal("eighth"), 28, popup_item_enabled, 0},
    };
    const UiRect compact_viewport{0.0f, 0.0f, 320.0f, 128.0f};
    const UiRect composer_anchor{16.0f, 112.0f, 304.0f, 113.0f};
    const PopupSceneResult autocomplete_opened =
        popup_scene_open(&scene, PopupKind::autocomplete, 77, ConstSlice<PopupItem>{autocomplete_items, 8},
                         composer_anchor, compact_viewport, 288.0f, true);
    bool scrolled = false;
    PopupItemLayout first_layout{};
    PopupItemLayout last_layout{};
    const bool autocomplete_scrolled = autocomplete_opened.ok() && scene.scroll_max > 0.0f &&
                                       popup_scene_scroll(&scene, scene.scroll_max, &scrolled) &&
                                       scrolled &&
                                       popup_scene_resolve_item_layout(scene, 0, &first_layout) &&
                                       popup_scene_resolve_item_layout(scene, 7, &last_layout);
    expect(autocomplete_scrolled && !first_layout.visible && last_layout.visible &&
               scene.scroll_offset == scene.scroll_max,
           "popup_scene_autocomplete_scroll_clips_rows_and_reaches_end");
    bool revealed_selection = false;
    expect(popup_scene_select_index(&scene, 0, &revealed_selection) && revealed_selection &&
               scene.scroll_offset == 0 && popup_scene_resolve_item_layout(scene, 0, &first_layout) &&
               first_layout.visible,
           "popup_scene_explicit_selection_reveals_first_row");
    expect(popup_scene_move_selection(&scene, -1, &revealed_selection) && revealed_selection &&
               scene.selected_index == 7 && scene.scroll_offset == scene.scroll_max &&
               popup_scene_resolve_item_layout(scene, 7, &last_layout) && last_layout.visible,
           "popup_scene_reverse_keyboard_wrap_reveals_last_row");
    const float autocomplete_end_offset = scene.scroll_offset;
    expect(popup_scene_page_selection(&scene, true, &revealed_selection) && revealed_selection &&
               scene.selected_index < 7 && scene.scroll_offset < autocomplete_end_offset,
           "popup_regular_autocomplete_uses_shared_viewport_paging");

    // A long retained menu must expose every action in a short fractional-DIP
    // viewport without rebuilding its bounded widget tree for each movement.
    constexpr u32 long_count = 256;
    PopupItem long_source[long_count]{};
    PopupItem long_items[long_count]{};
    PopupRowWidget long_rows[long_count]{};
    UiWidget long_widgets[2 + long_count * kPopupWidgetsPerItem]{};
    UiLayoutNode long_nodes[2 + long_count * kPopupWidgetsPerItem]{};
    for (u32 index = 0; index < long_count; ++index) {
        u16 flags = popup_item_enabled;
        if (index && index % 7 == 0) flags |= popup_item_separator_before;
        if (index % 5 == 0) flags |= popup_item_avatar;
        long_source[index] = {utf8_literal("Action"), 1000 + index, flags, 0, 2000ull + index};
    }
    PopupScene long_scene{};
    const bool long_initialized = popup_scene_initialize(&long_scene, {long_items, long_count},
        {long_rows, long_count}, {long_widgets, 2 + long_count * kPopupWidgetsPerItem},
        {long_nodes, 2 + long_count * kPopupWidgetsPerItem});
    const UiRect long_viewport{10.5f, 20.25f, 330.5f, 148.25f};
    const auto long_opened = long_initialized ? popup_scene_open(&long_scene,
        PopupKind::context_menu, 88, {long_source, long_count}, {24.5f, 30.25f, 26.5f, 32.25f},
        long_viewport, 180.0f) : PopupSceneResult{PopupSceneError::invalid_input, false};
    const UiWidgetHandle long_first = long_scene.rows[0].widget;
    const u16 long_generation = long_scene.widgets.generation;
    bool long_changed = false;
    PopupItemLayout long_first_layout{}, long_last_layout{};
    const bool long_selected = long_opened.ok() && long_scene.scroll_max > 1'000.0f &&
        popup_scene_select_index(&long_scene, long_count - 1, &long_changed) && long_changed &&
        popup_scene_resolve_item_layout(long_scene, 0, &long_first_layout) &&
        popup_scene_resolve_item_layout(long_scene, long_count - 1, &long_last_layout);
    expect(long_selected && !long_first_layout.visible && long_last_layout.visible &&
               long_scene.scroll_offset == long_scene.scroll_max &&
               long_last_layout.row->rectangle.top >= long_last_layout.clip_rectangle.top &&
               long_last_layout.row->rectangle.bottom <= long_last_layout.clip_rectangle.bottom &&
               long_scene.widgets.count == 2 + long_count * kPopupWidgetsPerItem &&
               long_scene.widgets.generation == long_generation,
           "popup_long_context_menu_selection_reveals_last_mixed_height_row_in_short_viewport");
    const PopupHit long_hit = long_selected ? popup_scene_hit_test(long_scene,
        (long_last_layout.row->rectangle.left + long_last_layout.row->rectangle.right) * .5f,
        (long_last_layout.row->rectangle.top + long_last_layout.row->rectangle.bottom) * .5f) : PopupHit{};
    expect(long_hit.enabled && long_hit.action == 1000 + long_count - 1 &&
               popup_scene_move_selection(&long_scene, 1, &long_changed) && long_changed &&
               long_scene.selected_index == 0 && long_scene.scroll_offset == 0 &&
               ui_widget_manager_resolve(long_scene.widgets, long_first),
           "popup_long_context_menu_hit_and_keyboard_wrap_preserve_retained_identity");
    expect(popup_scene_scroll(&long_scene, long_scene.scroll_max * 2, &long_changed) && long_changed &&
               long_scene.scroll_offset == long_scene.scroll_max,
           "popup_long_context_menu_scroll_clamps_to_complete_range");
    const u32 long_end_revision = long_scene.revision;
    expect(popup_scene_scroll(&long_scene, 1, &long_changed) && !long_changed &&
               long_scene.revision == long_end_revision &&
               popup_scene_select_index(&long_scene, 0, &long_changed) && long_changed &&
               long_scene.scroll_offset == 0 && long_scene.revision > long_end_revision,
           "popup_long_context_menu_same_selection_reveals_after_wheel_scroll_and_noop_is_stable");

    long_source[2].flags &= static_cast<u16>(~popup_item_enabled);
    const auto paged_menu = popup_scene_open(&long_scene, PopupKind::context_menu, 88,
        {long_source, long_count}, {24.5f, 30.25f, 26.5f, 32.25f}, long_viewport, 180.0f);
    const auto* paged_panel = popup_scene_layout_node(long_scene, long_scene.panel);
    const float menu_page_height = paged_panel ? ui_rect_height(paged_panel->content_rectangle) : 0;
    expect(paged_menu.ok() && popup_scene_page_selection(&long_scene, false, &long_changed) &&
               long_changed && long_scene.scroll_offset == menu_page_height &&
               long_scene.selected_index > 2 &&
               (long_scene.items[long_scene.selected_index].flags & popup_item_enabled),
           "popup_context_paging_moves_one_content_viewport_and_skips_disabled_actions");
    bool pages_forward = true;
    for (u32 step = 0; step < long_count && long_scene.scroll_offset < long_scene.scroll_max; ++step) {
        const float previous_offset = long_scene.scroll_offset;
        pages_forward = pages_forward && popup_scene_page_selection(&long_scene, false, &long_changed) &&
            long_scene.scroll_offset >= previous_offset;
    }
    expect(pages_forward && long_scene.scroll_offset == long_scene.scroll_max &&
               long_scene.selected_index == long_count - 1 &&
               popup_scene_page_selection(&long_scene, false, &long_changed) && !long_changed,
           "popup_context_repeated_paging_reaches_last_action_without_boundary_wrap");
    bool pages_reverse = true;
    for (u32 step = 0; step < long_count && long_scene.scroll_offset > 0; ++step) {
        const float previous_offset = long_scene.scroll_offset;
        pages_reverse = pages_reverse && popup_scene_page_selection(&long_scene, true, &long_changed) &&
            long_scene.scroll_offset <= previous_offset;
    }
    expect(pages_reverse && long_scene.scroll_offset == 0 && long_scene.selected_index == 0 &&
               popup_scene_page_selection(&long_scene, true, &long_changed) && !long_changed &&
               long_scene.widgets.count == 2 + long_count * kPopupWidgetsPerItem,
           "popup_context_reverse_paging_reaches_first_action_without_rebuilding_or_wrapping");

    // First enabled action can follow many unavailable entries. Opening must
    // reveal the same action Enter will invoke, before the first keyboard move.
    for (u32 index = 0; index < 20; ++index)
        long_source[index].flags &= static_cast<u16>(~popup_item_enabled);
    const auto disabled_prefix_opened = popup_scene_open(&long_scene, PopupKind::context_menu, 88,
        {long_source, long_count}, {24.5f, 30.25f, 26.5f, 32.25f}, long_viewport, 180.0f);
    PopupItemLayout initial_action{};
    expect(disabled_prefix_opened.ok() && long_scene.selected_index == 20 &&
               popup_scene_selected_action(long_scene) == 1020 && long_scene.scroll_offset > 0 &&
               popup_scene_resolve_item_layout(long_scene, 20, &initial_action) && initial_action.visible &&
               initial_action.row->rectangle.top >= initial_action.clip_rectangle.top &&
               initial_action.row->rectangle.bottom <= initial_action.clip_rectangle.bottom,
           "popup_open_reveals_first_enabled_action_after_unavailable_prefix");
    for (u32 index = 0; index < 20; ++index)
        long_source[index].flags |= popup_item_enabled;

    // Account and attachment menus share the retained popover policy. A short
    // viewport must keep every action reachable with focus and viewport paging.
    const auto compact_popover = popup_scene_open(&long_scene, PopupKind::popover, 89,
        {long_source, long_count}, {24.5f, 132.25f, 204.5f, 144.25f}, long_viewport, 180.0f, true);
    expect(compact_popover.ok() && long_scene.rectangle.bottom <= long_viewport.bottom - kPopupOuterMargin &&
               long_scene.scroll_max > 0 &&
               popup_scene_select_index(&long_scene, long_count - 1, &long_changed) && long_changed &&
               popup_scene_resolve_item_layout(long_scene, long_count - 1, &long_last_layout) &&
               long_last_layout.visible && long_scene.scroll_offset == long_scene.scroll_max &&
               long_last_layout.row->rectangle.bottom <= long_last_layout.clip_rectangle.bottom &&
               popup_scene_page_selection(&long_scene, true, &long_changed) && long_changed &&
               long_scene.selected_index < long_count - 1 &&
               popup_scene_scroll(&long_scene, -long_scene.scroll_max, &long_changed) && long_changed &&
               long_scene.scroll_offset == 0 &&
               popup_scene_move_selection(&long_scene, 1, &long_changed) &&
               popup_scene_resolve_item_layout(long_scene, long_scene.selected_index, &initial_action) &&
               initial_action.visible,
           "popup_short_popover_keeps_all_actions_reachable_by_focus_paging_and_scroll");

    for (u32 index = 0; index < kPopupQuickReactionCount; ++index)
        long_source[index].flags = popup_item_enabled | popup_item_emoji | popup_item_quick_reaction;
    const auto quick_long_opened = popup_scene_open(&long_scene, PopupKind::context_menu, 88,
        {long_source, long_count}, {24.5f, 30.25f, 26.5f, 32.25f}, long_viewport, 200.0f);
    expect(quick_long_opened.ok() && popup_scene_select_index(&long_scene, long_count - 1,
               &long_changed) && long_changed &&
               popup_scene_resolve_item_layout(long_scene, long_count - 1, &long_last_layout) &&
               long_last_layout.visible && long_scene.scroll_offset == long_scene.scroll_max &&
               popup_scene_select_index(&long_scene, 0, &long_changed) && long_changed &&
               popup_scene_resolve_item_layout(long_scene, 0, &long_first_layout) && long_first_layout.visible,
           "popup_quick_reaction_context_menu_scroll_reveals_actions_and_returns_to_strip");
    expect(popup_scene_select_index(&long_scene, 3, &long_changed) &&
               popup_scene_page_selection(&long_scene, true, &long_changed) &&
               long_scene.selected_index == 3 &&
               popup_scene_page_selection(&long_scene, false, &long_changed) && long_changed &&
               long_scene.selected_index >= kPopupQuickReactionCount &&
               popup_scene_page_selection(&long_scene, true, &long_changed) && long_changed &&
               long_scene.selected_index < kPopupQuickReactionCount,
           "popup_context_paging_treats_overlapping_quick_reactions_as_one_logical_strip");
    expect(!popup_scene_page_selection(&command_scene, false, &long_changed) &&
               !popup_scene_page_selection(nullptr, false, &long_changed) &&
               !popup_scene_page_selection(&long_scene, false, nullptr),
           "popup_paging_rejects_inactive_scene_and_missing_arguments");
    PaintStream long_paint{};
    paint_stream_reset(&long_paint, long_viewport.right, long_viewport.bottom);
    const bool long_presented = popup_presentation_build(long_scene, kDefaultTheme, &long_paint);
    const auto* long_panel = popup_scene_layout_node(long_scene, long_scene.panel);
    const bool expected_scrollbar = long_presented && long_paint.count && long_panel;
    const PaintQuad* long_thumb = expected_scrollbar ? &long_paint.quads[long_paint.count - 1] : nullptr;
    expect(expected_scrollbar && long_thumb && long_thumb->right <= long_scene.rectangle.right &&
               long_thumb->left >= long_scene.rectangle.right - 12 && long_thumb->radius == 2 &&
               long_thumb->top >= long_scene.rectangle.top &&
               long_thumb->bottom <= long_scene.rectangle.bottom,
           "popup_long_context_menu_paints_bounded_scrollbar_thumb");
    paint_stream_release(&long_paint);
    popup_scene_close(&long_scene);

    // The same placement applies to text, mentions and command rails, at
    // fractional DIP origins and with insufficient height for the full list.
    PopupItem variants[3][2]{
        {{utf8_literal(":smile:"), 1, popup_item_enabled},
         {utf8_literal(":smiley:"), 2, popup_item_enabled}},
        {{utf8_literal("Member"), 1, popup_item_enabled | popup_item_avatar, 0, 42},
         {utf8_literal("Another member"), 2, popup_item_enabled | popup_item_avatar, 0, 43}},
        {{utf8_literal("All"), 1, popup_item_enabled | popup_item_command_category},
         {utf8_literal("/wordle"), 2, popup_item_enabled | popup_item_command_row}}};
    const float anchor_tops[]{72.5f, 540.25f};
    for (float top : anchor_tops) {
        const UiRect surface{8.25f, top, 311.75f, top + 48};
        for (const auto& variant : variants) {
            const bool variant_opened = popup_scene_open(&scene, PopupKind::autocomplete, 77, {variant, 2},
                surface, {0, 0, 320, 600}, ui_rect_width(surface), true).ok();
            const auto* panel = popup_scene_layout_node(scene, scene.panel);
            expect(variant_opened && scene.active && panel &&
                       scene.rectangle.left == surface.left && scene.rectangle.right == surface.right &&
                       scene.rectangle.bottom == surface.top - kUiAutocompleteGap &&
                       scene.rectangle.top >= kPopupOuterMargin &&
                       ui_rect_equal(panel->rectangle, scene.rectangle),
                   "popup_autocomplete_variants_share_surface_width_gap_and_height_bounds");
        }
    }

    UiWidget owned_widgets[2 + 8 * kPopupWidgetsPerItem]{};
    UiLayoutNode owned_layout[2 + 8 * kPopupWidgetsPerItem]{};
    PopupScene owned{};
    const bool owned_initialized = popup_scene_initialize_owned(
        &owned, 8, Slice<UiWidget>{owned_widgets, 2 + 8 * kPopupWidgetsPerItem},
        Slice<UiLayoutNode>{owned_layout, 2 + 8 * kPopupWidgetsPerItem});
    expect(owned_initialized && owned.owned_storage.base && owned.owned_storage.committed == 0 &&
               !owned.items.data && !owned.rows.data,
           "popup_scene_owned_storage_is_dormant_until_open");
    const PopupSceneResult owned_opened =
        owned_initialized
            ? popup_scene_open(&owned, PopupKind::context_menu, 45, ConstSlice<PopupItem>{items, 3}, anchor,
                               viewport, 180.0f, false)
            : PopupSceneResult{};
    expect(owned_opened.ok() && owned.items.data && owned.rows.data && owned.owned_storage.committed != 0,
           "popup_scene_owned_storage_materializes_on_first_open");
    popup_scene_release(&owned);
    expect(!owned.owned_storage.base && !owned.items.data && !owned.rows.data,
           "popup_scene_owned_storage_release");
}

void test_ui_scroll_host() noexcept {
    struct Model { float offset; double maximum; bool alive; double origin = 0; u64 generation = 0; } model{2400, 20000, true};
    const UiScrollBinding binding{&model, 1,
        [](void* context, u64, UiScrollRange* range) noexcept {
            auto& value = *static_cast<Model*>(context);
            *range = {UiRect{0, 0, 200, 200}, value.offset, value.maximum, 40, 1, value.origin, value.generation};
            return value.alive;
        },
        [](void* context, u64, double offset, bool* changed) noexcept {
            auto& value = *static_cast<Model*>(context);
            const float next = static_cast<float>(offset);
            *changed = next != value.offset; value.offset = next; return true;
        }};
    UiScrollHost host{};
    bool changed = false;
    expect(ui_scroll_host_reveal(&host, binding, 2420, 2480, 100'000) == UiScrollDispatch::handled &&
        model.offset == 2400 && !ui_scroll_host_active(host), "scroll_reveal_visible_row_does_no_work");
    bool reveal = ui_scroll_host_reveal(&host, binding, 2800, 2856, 200'000) == UiScrollDispatch::handled &&
        model.offset == 2400 && ui_scroll_host_active(host) &&
        ui_scroll_host_tick(&host, 220'000, &changed) && changed && model.offset > 2400 && model.offset < 2656;
    reveal = reveal && ui_scroll_host_reveal(&host, binding, 2856, 2912, 220'000) == UiScrollDispatch::handled &&
        ui_scroll_host_reveal(&host, binding, 2856, 2912, 230'000) == UiScrollDispatch::handled &&
        ui_scroll_host_tick(&host, 500'000, &changed) && model.offset == 2712 && !ui_scroll_host_active(host);
    expect(reveal, "scroll_reveal_rapid_navigation_retargets_without_accumulating_or_jumping");
    expect(ui_scroll_host_reveal(&host, binding, 2856, 2912, 510'000) == UiScrollDispatch::handled &&
        !ui_scroll_host_active(host), "scroll_reveal_already_reached_target_stays_idle");
    expect(ui_scroll_host_reveal(&host, binding, 1000, 1300, 520'000) == UiScrollDispatch::handled &&
        ui_scroll_host_tick(&host, 800'000, &changed) && model.offset == 1000,
        "scroll_reveal_oversized_row_aligns_start");
    expect(ui_scroll_host_reveal(&host, binding, 20100, 20400, 810'000) == UiScrollDispatch::handled &&
        ui_scroll_host_tick(&host, 1'100'000, &changed) && model.offset == 20000 && !ui_scroll_host_active(host),
        "scroll_reveal_clamps_to_content_boundary");
    expect(ui_scroll_host_reveal(&host, binding, 10, 5, 1'200'000) == UiScrollDispatch::failed &&
        ui_scroll_host_reveal(&host, binding, -1, 5, 1'200'000) == UiScrollDispatch::failed &&
        !ui_scroll_host_active(host), "scroll_reveal_rejects_invalid_interval_without_work");
    ui_scroll_host_reveal(&host, binding, 1000, 1056, 1'300'000);
    ++model.generation;
    expect(ui_scroll_host_tick(&host, 1'320'000, &changed) && !changed && model.offset == 20000 &&
        !ui_scroll_host_active(host), "scroll_reveal_replaced_content_cancels_pending_navigation");
    model = {2400, 20000, true};
    host = {};
    const auto queued = ui_scroll_host_wheel(&host, binding, UiPoint{50, 50}, -120, 3, false, 1'000'000);
    const bool tiny = ui_scroll_host_tick(&host, 1'000'001, &changed);
    expect(queued == UiScrollDispatch::handled && tiny && !changed && ui_scroll_host_active(host),
        "scroll_host_subpixel_first_tick_preserves_wheel_input");
    expect(ui_scroll_host_tick(&host, 1'250'000, &changed) && changed && model.offset == 2520 &&
        !ui_scroll_host_active(host), "scroll_host_reaches_target_after_tiny_first_tick");
    bool repeated = true;
    constexpr u32 rates[] = {60, 144, 360};
    constexpr u32 phases[] = {1, 10, 50, 100, 400, 1000};
    for (u32 rate : rates) for (u32 phase : phases) {
        model.offset = 2400;
        u64 now = 2'000'000;
        for (u32 notch = 0; notch < 32; ++notch) {
            repeated = repeated && ui_scroll_host_wheel(&host, binding, UiPoint{50, 50}, -120, 3, false, now) == UiScrollDispatch::handled &&
                ui_scroll_host_tick(&host, now + phase, &changed) && ui_scroll_host_active(host);
            for (u64 elapsed = 1'000'000 / rate; elapsed < 250'000; elapsed += 1'000'000 / rate)
                repeated = repeated && ui_scroll_host_tick(&host, now + elapsed, &changed);
            repeated = repeated && ui_scroll_host_tick(&host, now + 250'000, &changed);
            now += 300'000;
        }
        repeated = repeated && model.offset == 6240 && !ui_scroll_host_active(host);
    }
    expect(repeated, "scroll_host_repeated_wheel_at_60_144_360hz_all_phases");
    bool rebased = true;
    for (u32 rate : rates) {
        model = {2400, 20000, true};
        host = {};
        rebased = rebased && ui_scroll_host_wheel(&host, binding, {50, 50}, -120, 3, false,
            10'000'000) == UiScrollDispatch::handled;
        for (u64 elapsed = 1'000'000 / rate; elapsed < 80'000; elapsed += 1'000'000 / rate) {
            rebased = rebased && ui_scroll_host_tick(&host, 10'000'000 + elapsed, &changed);
            // A media/text measurement or history insertion translates the anchor
            // after painting, but must not consume any remaining wheel distance.
            model.offset += 1;
            model.origin += 1;
            model.maximum += 1;
        }
        rebased = rebased && ui_scroll_host_wheel(&host, binding, {50, 50}, -120, 3, false,
            10'080'000) == UiScrollDispatch::handled;
        model.offset -= 50;
        model.origin -= 50;
        model.maximum -= 50;
        rebased = rebased && ui_scroll_host_tick(&host, 10'400'000, &changed) &&
            model.offset - model.origin == 2640 && !ui_scroll_host_active(host);
    }
    expect(rebased, "scroll_host_reflow_and_history_rebase_preserve_every_notch_at_all_refresh_rates");
    model = {2400, 20000, true};
    host = {};
    ui_scroll_host_wheel(&host, binding, {50, 50}, -120, 3, false, 11'000'000);
    model.offset = 5000;
    expect(ui_scroll_host_tick(&host, 11'020'000, &changed) && model.offset == 5000 &&
        !ui_scroll_host_active(host), "scroll_host_intentional_jump_still_cancels_animation");
    ui_scroll_host_wheel(&host, binding, {50, 50}, -120, 3, false, 12'000'000);
    ++model.generation;
    expect(ui_scroll_host_tick(&host, 12'020'000, &changed) && model.offset == 5000 &&
        !ui_scroll_host_active(host), "scroll_host_replaced_source_cancels_even_when_offset_matches");
    model = {2400, 20000, true};
    host = {};
    model.offset = 20000;
    const bool edge = ui_scroll_host_wheel(&host, binding, {50, 50}, -120, 3, false, 20'000'000) == UiScrollDispatch::handled &&
        ui_scroll_host_tick(&host, 20'000'001, &changed) && !ui_scroll_host_active(host) &&
        ui_scroll_host_wheel(&host, binding, {50, 50}, 120, 3, false, 21'000'000) == UiScrollDispatch::handled &&
        ui_scroll_host_tick(&host, 21'000'001, &changed) && ui_scroll_host_active(host) &&
        ui_scroll_host_tick(&host, 21'250'000, &changed) && model.offset == 19880;
    expect(edge, "scroll_host_real_boundary_and_reverse");
    ui_scroll_host_wheel(&host, binding, {50, 50}, -60, 3, false, 22'000'000);
    ui_scroll_host_tick(&host, 22'250'000, &changed);
    expect(model.offset == 19940, "scroll_host_high_resolution_half_notch");
    ui_scroll_host_wheel(&host, binding, {50, 50}, 120, 3, false, 23'000'000);
    model.maximum = 10000;
    expect(ui_scroll_host_tick(&host, 23'020'000, &changed) && changed && model.offset == 10000 &&
        !ui_scroll_host_active(host), "scroll_host_content_shrink_clamps_and_cancels");
    ui_scroll_host_wheel(&host, binding, {50, 50}, 120, 3, false, 24'000'000);
    model.alive = false;
    expect(ui_scroll_host_tick(&host, 24'020'000, &changed) && !changed && !ui_scroll_host_active(host),
        "scroll_host_removed_owner_cancels_without_writing");

    UiWidget widgets[5]{};
    UiLayoutNode nodes[5]{};
    UiWidgetManager manager{};
    bool tree_ok = ui_widget_manager_initialize(&manager, {widgets, 5}, {nodes, 5});
    auto style = ui_layout_style();
    style.flow = UiLayoutFlow::vertical;
    style.width = ui_layout_fill(); style.height = ui_layout_fill();
    const auto root = ui_widget_manager_add(&manager, ui_widget_invalid_handle(),
        {1, UiWidgetKind::container, ui_widget_enabled, {utf8_literal("root"), style, {}, 1}});
    style.width = ui_layout_fixed(200); style.height = ui_layout_fixed(200);
    const auto outer = ui_widget_manager_add(&manager, root.handle,
        {2, UiWidgetKind::scroll_view, ui_widget_enabled, {utf8_literal("outer"), style, {}, 2}});
    style.height = ui_layout_fixed(100);
    const auto inner = ui_widget_manager_add(&manager, outer.handle,
        {3, UiWidgetKind::scroll_view, ui_widget_enabled, {utf8_literal("inner"), style, {}, 3}});
    style.height = ui_layout_fixed(500);
    style.flags &= ~(ui_layout_node_scroll_vertical | ui_layout_node_clip_children);
    const auto content = ui_widget_manager_add(&manager, inner.handle,
        {4, UiWidgetKind::container, ui_widget_enabled, {utf8_literal("content"), style, {}, 4}});
    const auto sibling = ui_widget_manager_add(&manager, outer.handle,
        {5, UiWidgetKind::container, ui_widget_enabled, {utf8_literal("sibling"), style, {}, 5}});
    tree_ok = tree_ok && root.ok() && outer.ok() && inner.ok() && content.ok() && sibling.ok() &&
        ui_widget_manager_layout_rectangle(&manager, UiRect{0, 0, 400, 300}).ok();
    const auto auto_wheel = ui_scroll_host_wheel_widgets(&host, &manager, {20, 20}, -120, 3, false, 30'000'000);
    const bool auto_tick = ui_scroll_host_tick(&host, 30'250'000, &changed);
    const auto* inner_node = ui_widget_manager_layout_node(manager, inner.handle);
    const auto* outer_node = ui_widget_manager_layout_node(manager, outer.handle);
    expect(tree_ok && auto_wheel == UiScrollDispatch::handled && auto_tick && changed &&
        inner_node && outer_node && inner_node->scroll_offset.y == 96 && outer_node->scroll_offset.y == 0,
        "scroll_host_discovers_nested_scroll_view_without_registration");
    ui_scroll_host_wheel_widgets(&host, &manager, {20, 150}, -120, 3, false, 31'000'000);
    expect(ui_scroll_host_tick(&host, 31'250'000, &changed) && changed && outer_node->scroll_offset.y == 96,
        "scroll_host_routes_parent_outside_nested_clip");
    ui_scroll_host_wheel_widgets(&host, &manager, {20, 150}, -120, 3, false, 32'000'000);
    ui_widget_manager_reset(&manager);
    expect(ui_scroll_host_tick(&host, 32'250'000, &changed) && !changed && !ui_scroll_host_active(host),
        "scroll_host_widget_generation_invalidates_pending_animation");
}

void test_ui_smooth_scroll() noexcept {
    UiSmoothScroll scroll{};
    double delta = -1.0;
    double total = 0.0;
    bool forward_only = ui_smooth_scroll_queue(&scroll, 120.0, 1'000'000) && scroll.active &&
                        ui_smooth_scroll_step(&scroll, 1'000'000, &delta) && delta == 0.0;
    for (u32 frame = 1; frame <= 500 && forward_only && scroll.active; ++frame) {
        forward_only =
            ui_smooth_scroll_step(&scroll, 1'000'000 + frame * 16'000ull, &delta) && delta >= 0.0;
        total += delta;
    }
    expect(forward_only && !scroll.active && total > 119.9 && total < 120.1 && scroll.remaining == 0.0 &&
               scroll.segment_distance == 0.0 && scroll.segment_emitted == 0.0,
           "ui_smooth_scroll_converges_without_overshoot");

    UiSmoothScroll reversed{};
    double first = 0.0;
    double preserved = 0.0;
    double reverse = 0.0;
    bool reversed_smoothly = ui_smooth_scroll_queue(&reversed, 100.0, 2'000'000) &&
                             ui_smooth_scroll_step(&reversed, 2'016'000, &first) && first > 0.0 &&
                             ui_smooth_scroll_queue(&reversed, -200.0, 2'016'000) &&
                             ui_smooth_scroll_step(&reversed, 2'032'000, &preserved) && preserved > 0.0;
    for (u32 frame = 2; frame <= 20 && reversed_smoothly && reverse >= 0.0; ++frame) {
        reversed_smoothly = ui_smooth_scroll_step(&reversed, 2'016'000 + frame * 16'000ull, &reverse);
    }
    ui_smooth_scroll_cancel(&reversed);
    expect(reversed_smoothly && reverse < 0.0 && !reversed.active && reversed.remaining == 0.0 &&
               reversed.segment_distance == 0.0 && reversed.segment_emitted == 0.0,
           "ui_smooth_scroll_preserves_velocity_then_reverses");

    UiSmoothScroll high_refresh{};
    double high_refresh_first = 0.0;
    double high_refresh_second = 0.0;
    const bool samples_360_hz =
        ui_smooth_scroll_queue(&high_refresh, 120.0, 3'000'000) &&
        ui_smooth_scroll_step(&high_refresh, 3'002'778, &high_refresh_first) &&
        ui_smooth_scroll_step(&high_refresh, 3'005'556, &high_refresh_second) &&
        high_refresh_first > 0.0 && high_refresh_second > high_refresh_first;
    expect(samples_360_hz, "ui_smooth_scroll_preserves_sub_millisecond_frame_phase");

    UiSmoothScroll chromium_curve{};
    double quarter = 0.0;
    const bool curve_matches = ui_smooth_scroll_queue(&chromium_curve, 120.0, 4'000'000) &&
                               chromium_curve.duration_us == 200'000 &&
                               ui_smooth_scroll_step(&chromium_curve, 4'050'000, &quarter) &&
                               quarter > 15.4 && quarter < 15.7;
    expect(curve_matches, "ui_smooth_scroll_matches_chromium_ease_in_out_curve");

    auto cadence_total = [](u64 period_us) noexcept {
        UiSmoothScroll paced{};
        double emitted = 0.0;
        double step = 0.0;
        const u64 start = 5'000'000;
        if (!ui_smooth_scroll_queue(&paced, 480.0, start)) {
            return -1.0;
        }
        for (u64 now = start + period_us; paced.active; now += period_us) {
            if (!ui_smooth_scroll_step(&paced, now, &step)) {
                return -1.0;
            }
            emitted += step;
        }
        return emitted;
    };
    const double total_60_hz = cadence_total(16'667);
    const double total_144_hz = cadence_total(6'944);
    const double total_360_hz = cadence_total(2'778);
    expect(total_60_hz > 479.99 && total_60_hz < 480.01 && total_144_hz > 479.99 &&
               total_144_hz < 480.01 && total_360_hz > 479.99 && total_360_hz < 480.01,
           "ui_smooth_scroll_is_refresh_rate_independent");

    UiSmoothScroll missed{};
    double missed_first = 0.0;
    double missed_last = 0.0;
    const bool missed_frame_ok = ui_smooth_scroll_queue(&missed, 120.0, 6'000'000) &&
                                 ui_smooth_scroll_step(&missed, 6'016'667, &missed_first) &&
                                 ui_smooth_scroll_step(&missed, 6'250'000, &missed_last) &&
                                 !missed.active && missed_first > 0.0 &&
                                 missed_first + missed_last > 119.99 &&
                                 missed_first + missed_last < 120.01;
    expect(missed_frame_ok, "ui_smooth_scroll_missed_frame_advances_to_current_time");
}

void test_member_list_channel_binding() noexcept {
    ChatStateStore state{};
    ChannelRecord channels[3]{};
    u16 channel_order[]{0, 1, 2};
    RoleRecord everyone{};
    everyone.id = everyone.guild_id = 5;
    everyone.permissions = kPermissionViewChannel;
    PermissionOverwriteRecord overwrites[]{
        {2, kPermissionViewChannel, 0, 0},
        {5, 0, kPermissionViewChannel, 0},
        {10, kPermissionViewChannel, 0, 0},
    };
    state.channels = channels;
    state.channel_order = channel_order;
    state.channel_capacity = state.channel_count = 3;
    state.roles = &everyone;
    state.role_count = state.role_capacity = 1;
    state.role_backed_count = 1;
    state.overwrites = overwrites;
    state.overwrite_count = state.overwrite_capacity = 3;
    state.overwrite_backed_count = 3;
    for (u32 i = 0; i < 3; ++i) {
        channels[i].id = 70 + i;
        channels[i].guild_id = 5;
        channels[i].occupied = true;
    }
    expect(member_list_channel_id(state, 70) == kMemberListEveryone &&
           member_list_channel_id(state, 71) == kMemberListEveryone,
           "member_list_public_channels_share_everyone_identity");
    channels[0].overwrite_count = 3;
    channels[1].overwrite_count = 3;
    // Oracle values obtained from the official client, not this implementation.
    expect(member_list_channel_id(state, 70) == 3'738'272'248ull &&
           member_list_channel_id(state, 71) == 3'738'272'248ull,
           "member_list_visibility_hash_matches_official_textual_sort");
    channels[1].member_list_id = 123;
    expect(member_list_channel_id(state, 71) == 123,
           "member_list_server_identity_overrides_permission_hash");
    MemberListIdentityCache identity{};
    expect(member_list_channel_cached_id(&identity, state, 70) == 3'738'272'248ull &&
           member_list_channel_cached_id(&identity, state, 71) == 123 &&
           member_list_channel_cached_id(&identity, state, 70) == 3'738'272'248ull,
           "member_list_switch_back_does_not_depend_on_subscription_or_response_order");
    channels[0].overwrite_count = 1;
    channels[0].overwrite_offset = 1;
    ++state.permission_mutations;
    expect(member_list_channel_cached_id(&identity, state, 70) == 147'178'806ull,
           "member_list_cached_hash_refreshes_on_permission_changes");
    everyone.permissions = 0;
    expect(member_list_channel_id(state, 72) == kMemberListZeroHash,
           "member_list_zero_hash_is_distinct_from_missing_id");
    channels[2].type = 10;
    channels[2].parent_id = 71;
    expect(member_list_channel_id(state, 72) == 123,
           "member_list_announcement_thread_uses_parent_identity");

    MemberListSubscriptionRetry retry{};
    const bool selected = member_list_subscription_select(&retry, 6, 80, 2'000);
    const bool first_due = member_list_subscription_due(retry, 2'000);
    const bool first_failed = member_list_subscription_fail(&retry, 2'000);
    const u64 first_retry_at = retry.retry_at_ms;
    const bool waiting = !member_list_subscription_due(retry, 2'099);
    const bool retry_due = member_list_subscription_due(retry, 2'100);
    member_list_subscription_complete(&retry);
    expect(selected && first_due && first_failed && first_retry_at == 2'100 && waiting && retry_due &&
               retry.channel_id == 0 && retry.retry_at_ms == 0,
           "member_list_subscription_busy_retries_independently");
}

void test_ui_variable_list() noexcept {
    const UiScrollbarGeometry middle_scrollbar =
        ui_scrollbar_geometry(UiRect{0.0f, 0.0f, 200.0f, 100.0f}, 450.0f, 1'000.0f);
    const UiScrollbarGeometry end_scrollbar =
        ui_scrollbar_geometry(UiRect{0.0f, 0.0f, 200.0f, 100.0f}, 2'000.0f, 1'000.0f);
    const UiScrollbarGeometry unnecessary_scrollbar =
        ui_scrollbar_geometry(UiRect{0.0f, 0.0f, 200.0f, 100.0f}, 0.0f, 100.0f);
    expect(middle_scrollbar.visible && middle_scrollbar.maximum_scroll == 900.0f &&
               middle_scrollbar.track.left == 188.0f && middle_scrollbar.thumb.left == 190.0f &&
               middle_scrollbar.thumb.top == 38.0f && middle_scrollbar.thumb.right == 198.0f &&
               middle_scrollbar.thumb.bottom == 62.0f && end_scrollbar.visible &&
               end_scrollbar.thumb.top == 72.0f && end_scrollbar.thumb.bottom == 96.0f &&
               !unnecessary_scrollbar.visible,
           "ui_scrollbar_proportional_clamped_geometry");
    expect(ui_scrollbar_track_hit(middle_scrollbar, 195.0f, 50.0f) &&
               ui_scrollbar_thumb_hit(middle_scrollbar, 195.0f, 50.0f) &&
               !ui_scrollbar_thumb_hit(middle_scrollbar, 189.0f, 50.0f) &&
               ui_scrollbar_scroll_from_pointer(middle_scrollbar, 50.0f, 12.0f) == 450.0f &&
               ui_scrollbar_scroll_from_pointer(middle_scrollbar, -100.0f, 12.0f) == 0.0f &&
               ui_scrollbar_scroll_from_pointer(middle_scrollbar, 500.0f, 12.0f) == 900.0f,
           "ui_scrollbar_shared_hit_test_and_drag_mapping");
    const auto large_scrollbar = ui_scrollbar_geometry(
        {0.0f, 0.0f, 200.0f, 100.0f}, 2'199'950.0f, 4'400'000.0f);
    expect(large_scrollbar.visible && large_scrollbar.maximum_scroll == 4'399'900.0f &&
        large_scrollbar.thumb.top == 38.0f && large_scrollbar.thumb.bottom == 62.0f &&
        ui_scrollbar_scroll_from_pointer(large_scrollbar, 50.0f, 12.0f) == 2'199'950.0f &&
        ui_scrollbar_scroll_from_pointer(large_scrollbar, 500.0f, 12.0f) == 4'399'900.0f,
        "ui_scrollbar_large_logical_member_list_keeps_local_thumb_and_drag_mapping");
    constexpr float maximum_scroll_scalar = 3.402823466e38f;
    const auto extreme_scrollbar = ui_scrollbar_geometry(
        {0.0f, 0.0f, 200.0f, 100.0f}, maximum_scroll_scalar * 0.5f, maximum_scroll_scalar);
    expect(extreme_scrollbar.visible && extreme_scrollbar.thumb.top == 38.0f &&
        ui_scrollbar_scroll_from_pointer(extreme_scrollbar, 50.0f, 12.0f) == maximum_scroll_scalar * 0.5f &&
        ui_scrollbar_scroll_from_pointer(extreme_scrollbar, 500.0f, 12.0f) == maximum_scroll_scalar,
        "ui_scrollbar_normalized_math_avoids_overflow_for_finite_logical_lengths");
    bool invalid_scrollbars_rejected = true;
    constexpr u32 invalid_scalar_bits[]{0x7f800000u, 0xff800000u, 0x7fc00000u};
    for (const u32 scalar_bits : invalid_scalar_bits) {
        float invalid_scalar = 0.0f;
        copy_bytes(&invalid_scalar, &scalar_bits, sizeof(invalid_scalar));
        invalid_scrollbars_rejected &=
            !ui_scrollbar_geometry({0.0f, 0.0f, 200.0f, 100.0f}, 0.0f, invalid_scalar).visible &&
            !ui_scrollbar_geometry({0.0f, 0.0f, 200.0f, 100.0f}, invalid_scalar, 1'000.0f).visible;
    }
    expect(invalid_scrollbars_rejected &&
        !ui_scrollbar_geometry({0.0f, 0.0f, 200.0f, 2'000'000.0f}, 0.0f, 4'400'000.0f).visible,
        "ui_scrollbar_rejects_nonfinite_scalars_and_preserves_local_layout_bounds");
    PaintStream scrollbar_paint{};
    const bool scrollbar_painted = paint_stream_reset(&scrollbar_paint, 200, 100) &&
        ui_scrollbar_paint(&scrollbar_paint, middle_scrollbar, kDefaultTheme);
    expect(scrollbar_painted && scrollbar_paint.count == 1 &&
        scrollbar_paint.quads[0].left == middle_scrollbar.thumb.right - 4.0f &&
        scrollbar_paint.quads[0].top == middle_scrollbar.thumb.top &&
        scrollbar_paint.quads[0].right == middle_scrollbar.thumb.right &&
        scrollbar_paint.quads[0].bottom == middle_scrollbar.thumb.bottom &&
        scrollbar_paint.quads[0].radius == 2.0f && scrollbar_paint.scrollbar_tracks.count == 1 &&
        ui_rect_equal(scrollbar_paint.scrollbar_tracks.tracks[0], middle_scrollbar.track),
        "ui_scrollbar_paints_only_thumb_preserving_pane_background");
    expect(ui_scrollbar_paint(&scrollbar_paint, unnecessary_scrollbar, kDefaultTheme) && scrollbar_paint.count == 1 &&
        scrollbar_paint.scrollbar_tracks.count == 1,
        "ui_scrollbar_hidden_emits_no_background_or_thumb");
    paint_stream_release(&scrollbar_paint);

    u32 heights[8]{};
    u64 fenwick[8]{};
    UiVariableList list{};
    expect(ui_variable_list_initialize(&list, Slice<u32>{heights, 8}, Slice<u64>{fenwick, 8}, 5, 100) &&
               list.total_units == 500 && list.generation == 1,
           "ui_variable_list_initialize");
    expect(ui_variable_list_set_height(&list, 1, 200) && ui_variable_list_set_height(&list, 2, 50) &&
               list.total_units == 550 && ui_variable_list_prefix_units(list, 0) == 0 &&
               ui_variable_list_prefix_units(list, 1) == 100 &&
               ui_variable_list_prefix_units(list, 2) == 300 &&
               ui_variable_list_prefix_units(list, 3) == 350 &&
               ui_variable_list_prefix_units(list, 99) == 550,
           "ui_variable_list_fenwick_updates");
    const UiVariableRange range = ui_variable_list_range(list, 120, 200, 1);
    expect(range.first == 0 && range.count == 4 && range.first_offset_units == -120,
           "ui_variable_list_range_variable_heights");
    const auto saturated_range = ui_variable_list_range(list, 120, ~u64{0}, ~u32{0});
    expect(saturated_range.first == 0 && saturated_range.count == 5 &&
               saturated_range.first_offset_units == -120,
           "ui_variable_list_overscan_saturates_without_wrapping");

    UiScrollAnchor anchor{};
    u64 restored = 0;
    expect(ui_variable_list_capture_anchor(list, 120, &anchor) && anchor.valid && anchor.index == 1 &&
               anchor.within_item_units == 20 && ui_variable_list_set_height(&list, 0, 150) &&
               ui_variable_list_restore_anchor(list, anchor, &restored) && restored == 170,
           "ui_variable_list_anchor_above_resize");
    expect(ui_variable_list_set_height(&list, 1, 10) &&
               ui_variable_list_restore_anchor(list, anchor, &restored) && restored == 159,
           "ui_variable_list_anchor_inside_clamp");

    expect(ui_variable_list_insert(&list, 0, 2, 40, &anchor) && list.count == 7 && list.generation == 2 &&
               anchor.index == 3 && anchor.generation == 2 &&
               ui_variable_list_restore_anchor(list, anchor, &restored) && restored == 239,
           "ui_variable_list_prepend_anchor_restore");
    expect(ui_variable_list_remove(&list, 0, 1, &anchor) && list.count == 6 && list.generation == 3 &&
               anchor.index == 2 && ui_variable_list_restore_anchor(list, anchor, &restored) &&
               restored == 199,
           "ui_variable_list_remove_anchor_restore");
    expect(ui_variable_list_remove(&list, 0, 3, &anchor) && list.count == 3 && !anchor.valid &&
               list.total_units == 250 && !ui_variable_list_restore_anchor(list, anchor, &restored),
           "ui_variable_list_remove_anchor_invalidate");

    expect(!ui_variable_list_set_height(&list, 99, 100) && !ui_variable_list_set_height(&list, 0, 0) &&
               list.rejected == 2 && ui_extent_units(1.5f) == 96 && ui_extent_dips(96) == 1.5f &&
               ui_signed_extent_dips(-64) == -1.0f,
           "ui_variable_list_rejection_and_units");
    u64 short_fenwick[7]{};
    expect(
        !ui_variable_list_initialize(&list, Slice<u32>{heights, 8}, Slice<u64>{short_fenwick, 7}, 5, 100),
        "ui_variable_list_reject_storage_mismatch");

    u32 indexed_heights[10]{};
    u64 indexed_fenwick[10]{};
    UiVariableList indexed{};
    expect(
        ui_variable_list_initialize(&indexed, Slice<u32>{indexed_heights, 10},
                                    Slice<u64>{indexed_fenwick, 10}, 6, 100) &&
            ui_variable_list_set_height(&indexed, 0, 10) && ui_variable_list_set_height(&indexed, 1, 20) &&
            ui_variable_list_set_height(&indexed, 2, 30) && ui_variable_list_set_height(&indexed, 3, 40) &&
            ui_variable_list_set_height(&indexed, 4, 50) && ui_variable_list_set_height(&indexed, 5, 60),
        "ui_variable_list_indexed_seed");
    UiScrollAnchor indexed_anchor{};
    expect(ui_variable_list_capture_anchor(indexed, 65, &indexed_anchor) && indexed_anchor.index == 3 &&
               indexed_anchor.within_item_units == 5 &&
               ui_variable_list_insert(&indexed, 1, 2, 15, &indexed_anchor) && indexed.count == 8 &&
               indexed_anchor.index == 5 && ui_variable_list_item_units(indexed, 0) == 10 &&
               ui_variable_list_item_units(indexed, 1) == 15 &&
               ui_variable_list_item_units(indexed, 2) == 15 &&
               ui_variable_list_item_units(indexed, 5) == 40 &&
               ui_variable_list_restore_anchor(indexed, indexed_anchor, &restored) && restored == 95,
           "ui_variable_list_indexed_insert_before_anchor");
    expect(ui_variable_list_insert(&indexed, indexed.count, 2, 25, &indexed_anchor) &&
               indexed.count == 10 && indexed_anchor.index == 5 &&
               indexed_anchor.generation == indexed.generation &&
               ui_variable_list_restore_anchor(indexed, indexed_anchor, &restored) && restored == 95,
           "ui_variable_list_indexed_append_after_anchor");
    expect(ui_variable_list_remove(&indexed, 1, 2, &indexed_anchor) && indexed.count == 8 &&
               indexed_anchor.index == 3 && ui_variable_list_item_units(indexed, 1) == 20 &&
               ui_variable_list_item_units(indexed, 3) == 40 &&
               ui_variable_list_restore_anchor(indexed, indexed_anchor, &restored) && restored == 65,
           "ui_variable_list_indexed_remove_before_anchor");
    expect(ui_variable_list_remove(&indexed, 3, 1, &indexed_anchor) && indexed.count == 7 &&
               !indexed_anchor.valid &&
               !ui_variable_list_restore_anchor(indexed, indexed_anchor, &restored),
           "ui_variable_list_indexed_remove_anchor_invalidate");
    expect(!ui_variable_list_insert(&indexed, indexed.count + 1, 1, 10) &&
               !ui_variable_list_insert(&indexed, indexed.count, 0, 10) &&
               !ui_variable_list_insert(&indexed, indexed.count, 4, 10) &&
               !ui_variable_list_remove(&indexed, indexed.count, 1) &&
               !ui_variable_list_remove(&indexed, 0, 0) && indexed.rejected == 5,
           "ui_variable_list_indexed_mutation_rejections");
}

void test_glyph_quad_bounds() noexcept {
    const Color white{1.0f, 1.0f, 1.0f, 1.0f};
    GlyphQuad quads[5]{
        GlyphQuad{2.0f, 2.0f, 4.0f, 4.0f, 0.1f, 0.2f, 0.3f, 0.4f, white},
        GlyphQuad{-2.0f, 6.0f, 6.0f, 14.0f, 0.0f, 0.0f, 1.0f, 1.0f, white},
        GlyphQuad{11.0f, 1.0f, 12.0f, 2.0f, 0.0f, 0.0f, 1.0f, 1.0f, white},
        GlyphQuad{-2.0f, -2.0f, 14.0f, 14.0f, 2.0f, 0.0f, 3.0f, 1.0f, white},
        GlyphQuad{3.0f, 4.0f, 8.0f, 7.0f, 0.1f, 0.2f, 0.3f, 0.4f, white},
    };
    u32 count = 5;
    const bool bounded = glyph_quad_clip_range(quads, 0, &count, UiRect{0.0f, 0.0f, 10.0f, 10.0f});
    expect(bounded &&
               count == 4 && quads[0].left == 2.0f && quads[0].u0 == 0.1f && quads[1].left == 0.0f &&
               quads[1].top == 6.0f && quads[1].right == 6.0f && quads[1].bottom == 10.0f &&
               quads[1].u0 == 0.25f && quads[1].v0 == 0.0f && quads[1].u1 == 1.0f && quads[1].v1 == 0.5f &&
               quads[2].left == 0.0f && quads[2].top == 0.0f && quads[2].right == 10.0f &&
               quads[2].bottom == 10.0f && quads[2].u0 == 2.125f && quads[2].v0 == 0.125f &&
               quads[2].u1 == 2.75f && quads[2].v1 == 0.75f &&
               quads[3].left == 3.0f && quads[3].top == 4.0f && quads[3].right == 8.0f &&
               quads[3].bottom == 7.0f && quads[3].u0 == 0.1f && quads[3].v1 == 0.4f,
           "glyph_quad_bounds_clip_uv_and_cull");

    quads[1].left = 5.0f;
    quads[1].right = 4.0f;
    const u32 invalid_count = count;
    const bool invalid_quad = glyph_quad_clip_range(quads, 0, &count, UiRect{0.0f, 0.0f, 10.0f, 10.0f});
    const bool invalid_clip = glyph_quad_clip_range(quads, 0, &count, UiRect{0.0f, 0.0f, 0.0f, 10.0f});
    expect(!invalid_quad && !invalid_clip && count == invalid_count,
           "glyph_quad_bounds_transactional_rejection");
    GlyphQuad layers[2]{
        GlyphQuad{10, 20, 22, 34, 0, 0, 1, 1, white},
        GlyphQuad{12, 22, 18, 28, 0.1f, 0.2f, 0.3f, 0.4f, white}};
    expect(glyph_quad_center_range(layers, 0, 2, UiRect{0, 0, 28, 28}) &&
        layers[0].left == 8 && layers[0].top == 7 && layers[0].right == 20 && layers[0].bottom == 21 &&
        layers[1].left == 10 && layers[1].top == 9 && layers[1].u0 == 0.1f && layers[1].v1 == 0.4f,
        "glyph_quad_ink_center_preserves_color_layer_geometry_and_uvs");
    layers[1].right = layers[1].left - 1;
    expect(!glyph_quad_center_range(layers, 0, 2, UiRect{0, 0, 40, 40}) && layers[0].left == 8,
        "glyph_quad_ink_center_transactional_validation");
}

void test_glyph_atlas_allocator() noexcept {
    GlyphAtlasEntry entries[8]{};
    GlyphAtlasRow rows[4]{};
    GlyphAtlasAllocator atlas{};
    expect(glyph_atlas_initialize(&atlas, 64, 32, 1, Slice<GlyphAtlasEntry>{entries, 8},
                                  Slice<GlyphAtlasRow>{rows, 4}),
           "glyph_atlas_initialize");
    expect(!glyph_atlas_initialize(nullptr, 64, 32, 1, Slice<GlyphAtlasEntry>{entries, 8},
                                   Slice<GlyphAtlasRow>{rows, 4}),
           "glyph_atlas_reject_null");

    const GlyphAtlasAllocation first = glyph_atlas_allocate(&atlas, 101, 10, 10);
    const GlyphAtlasAllocation second = glyph_atlas_allocate(&atlas, 202, 10, 10);
    const GlyphAtlasAllocation cached = glyph_atlas_allocate(&atlas, 101, 10, 10);
    GlyphAtlasAllocation validated{};
    expect(first.ok() && second.ok() && !first.cached && first.x == 1 && first.y == 1 && second.x == 13 &&
               cached.ok() && cached.cached && cached.handle.slot == first.handle.slot &&
               cached.handle.generation == first.handle.generation &&
               glyph_atlas_validate(atlas, first.handle, &validated) && validated.x == first.x &&
               validated.width == 10,
           "glyph_atlas_pack_cache_validate");
    expect(glyph_atlas_find(&atlas, 999).error == GlyphAtlasError::not_found &&
               glyph_atlas_allocate(&atlas, 101, 11, 10).error == GlyphAtlasError::invalid_input &&
               glyph_atlas_allocate(&atlas, 303, 64, 32).error == GlyphAtlasError::too_large,
           "glyph_atlas_lookup_dimension_bounds");

    GlyphAtlasEntry lru_entries[4]{};
    GlyphAtlasRow lru_rows[2]{};
    GlyphAtlasAllocator lru{};
    expect(glyph_atlas_initialize(&lru, 16, 16, 0, Slice<GlyphAtlasEntry>{lru_entries, 4},
                                  Slice<GlyphAtlasRow>{lru_rows, 2}),
           "glyph_atlas_lru_initialize");
    const GlyphAtlasAllocation old_row = glyph_atlas_allocate(&lru, 1, 16, 8);
    const GlyphAtlasAllocation live_row = glyph_atlas_allocate(&lru, 2, 16, 8);
    expect(glyph_atlas_find(&lru, 2).ok(), "glyph_atlas_lru_touch");
    const GlyphAtlasAllocation replacement = glyph_atlas_allocate(&lru, 3, 16, 8);
    expect(replacement.ok() && replacement.evicted_entries == 1 &&
               !glyph_atlas_validate(lru, old_row.handle) && glyph_atlas_validate(lru, live_row.handle) &&
               lru.evicted_rows == 1,
           "glyph_atlas_row_lru_eviction");

    // Two shelves holding nothing visible become one for a taller glyph; no full reset.
    const GlyphAtlasHandle stale_live = live_row.handle;
    const GlyphAtlasAllocation reset_allocation = glyph_atlas_allocate(&lru, 4, 16, 16);
    expect(reset_allocation.ok() && !reset_allocation.atlas_reset && reset_allocation.evicted_entries == 2 &&
               lru.full_resets == 0 && lru.row_count == 1 && !glyph_atlas_validate(lru, stale_live) &&
               glyph_atlas_validate(lru, reset_allocation.handle),
           "glyph_atlas_invisible_shelves_merge_without_reset");

    GlyphAtlasEntry pinned_entries[4]{};
    GlyphAtlasRow pinned_rows[2]{};
    GlyphAtlasAllocator pinned{};
    const bool pinned_ready =
        glyph_atlas_initialize(&pinned, 16, 16, 0, Slice<GlyphAtlasEntry>{pinned_entries, 4},
                               Slice<GlyphAtlasRow>{pinned_rows, 2});
    const GlyphAtlasAllocation pinned_old =
        pinned_ready ? glyph_atlas_allocate(&pinned, 11, 16, 8) : GlyphAtlasAllocation{};
    const GlyphAtlasAllocation pinned_live =
        pinned_old.ok() ? glyph_atlas_allocate(&pinned, 12, 16, 8) : GlyphAtlasAllocation{};
    glyph_atlas_clear_pins(&pinned);
    const bool live_pinned = glyph_atlas_pin(&pinned, pinned_live.handle);
    const GlyphAtlasAllocation pinned_pressure =
        live_pinned ? glyph_atlas_allocate(&pinned, 13, 16, 16) : GlyphAtlasAllocation{};
    expect(pinned_ready && pinned_old.ok() && pinned_live.ok() && live_pinned &&
               pinned_pressure.error == GlyphAtlasError::no_capacity && !pinned_pressure.atlas_reset &&
               pinned.full_resets == 0 && glyph_atlas_validate(pinned, pinned_old.handle) &&
               glyph_atlas_validate(pinned, pinned_live.handle),
           "glyph_atlas_pinned_row_blocks_destructive_reset");
    glyph_atlas_clear_pins(&pinned);
    const GlyphAtlasAllocation pressure_after_visibility = glyph_atlas_allocate(&pinned, 13, 16, 16);
    expect(pressure_after_visibility.ok() && !pressure_after_visibility.atlas_reset &&
               pinned.full_resets == 0 && !glyph_atlas_validate(pinned, pinned_live.handle),
           "glyph_atlas_unpinned_shelves_are_reclaimed_without_reset");

    GlyphAtlasEntry epoch_entries[8]{};
    GlyphAtlasRow epoch_rows[2]{};
    GlyphAtlasAllocator epoch_atlas{};
    bool epoch_ok =
        glyph_atlas_initialize(&epoch_atlas, 64, 64, 0, Slice<GlyphAtlasEntry>{epoch_entries, 8},
                               Slice<GlyphAtlasRow>{epoch_rows, 2});
    GlyphAtlasHandle epoch_first{};
    for (u32 index = 0; index < 8 && epoch_ok; ++index) {
        const GlyphAtlasAllocation inserted = glyph_atlas_allocate_unique(&epoch_atlas, index + 1, 1, 1);
        epoch_ok = inserted.ok();
        if (index == 0) {
            epoch_first = inserted.handle;
        }
    }
    epoch_ok = epoch_ok && glyph_atlas_pin(&epoch_atlas, epoch_first) &&
               glyph_atlas_is_pinned(epoch_atlas, epoch_first);
    for (u32 index = 0; index < 65'535; ++index) {
        glyph_atlas_clear_pins(&epoch_atlas);
    }
    const bool stale_pin_cleared = !glyph_atlas_is_pinned(epoch_atlas, epoch_first);
    const bool repinned_after_wrap =
        glyph_atlas_pin(&epoch_atlas, epoch_first) && glyph_atlas_is_pinned(epoch_atlas, epoch_first);
    const u32 pinned_row = epoch_entries[epoch_first.slot].row;
    const u32 pinned_count = epoch_rows[pinned_row].pinned_entries;
    const bool duplicate_pin = glyph_atlas_pin(&epoch_atlas, epoch_first);
    expect(epoch_ok && epoch_atlas.entry_count == 8 && epoch_atlas.visibility_epoch == 1 &&
               stale_pin_cleared && repinned_after_wrap && duplicate_pin && pinned_count == 1 &&
               epoch_atlas.visibility_rebases == 1 && epoch_rows[pinned_row].pinned_entries == 1,
           "glyph_atlas_visibility_epoch_wrap_is_bounded_and_alias_safe");

    glyph_atlas_clear_pins(&lru);
    const bool clock_wrap_pin = glyph_atlas_pin(&lru, reset_allocation.handle);
    lru.clock = ~0ull;
    lru.rows[0].last_used = 1;
    const GlyphAtlasAllocation wrapped = glyph_atlas_find(&lru, 4);
    expect(clock_wrap_pin && wrapped.ok() && lru.clock == 1 && lru.rows[0].last_used == 1 &&
               glyph_atlas_is_pinned(lru, reset_allocation.handle) &&
               lru.rows[lru.entries[reset_allocation.handle.slot].row].pinned_entries == 1,
           "glyph_atlas_clock_restart_preserves_current_visibility");

    const GlyphAtlasHandle before_reset = reset_allocation.handle;
    glyph_atlas_reset(&lru);
    expect(!glyph_atlas_validate(lru, before_reset) && lru.entry_count == 0 && lru.row_count == 0 &&
               lru.full_resets == 1,
           "glyph_atlas_explicit_reset");

    // The key index must agree with the linear scan through fill, row
    // eviction, index rebuilds and reset, and it never needs eviction hooks.
    GlyphAtlasEntry indexed_entries[64]{};
    GlyphAtlasRow indexed_rows[16]{};
    u16 index_buckets[128]{};
    GlyphAtlasAllocator indexed{};
    GlyphAtlasIndex index{};
    bool index_ok = !glyph_atlas_index_initialize(&index, Slice<u16>{index_buckets, 96}) &&
                    glyph_atlas_initialize(&indexed, 64, 64, 0, Slice<GlyphAtlasEntry>{indexed_entries, 64},
                                           Slice<GlyphAtlasRow>{indexed_rows, 16}) &&
                    glyph_atlas_index_initialize(&index, Slice<u16>{index_buckets, 128});
    for (u32 key = 1; key <= 64 && index_ok; ++key)
        index_ok = glyph_atlas_allocate_unique_indexed(&indexed, &index, key * 0x10001ull, 4, 4).ok();
    index_ok = index_ok && indexed.entry_count == 64 &&
               !glyph_atlas_find_indexed(&indexed, &index, 65 * 0x10001ull).ok();
    for (u32 key = 1; key <= 64 && index_ok; ++key) {
        const GlyphAtlasAllocation hit = glyph_atlas_find_indexed(&indexed, &index, key * 0x10001ull);
        index_ok = hit.ok() && hit.cached && indexed_entries[hit.handle.slot].key == key * 0x10001ull;
    }
    for (u32 key = 65; key <= 1'064 && index_ok; ++key) {
        const GlyphAtlasAllocation inserted =
            glyph_atlas_allocate_unique_indexed(&indexed, &index, key * 0x10001ull, 4, 4);
        const u64 probe = (key - (key % 97)) * 0x10001ull;
        const GlyphAtlasAllocation fast = glyph_atlas_find_indexed(&indexed, &index, probe);
        const GlyphAtlasAllocation slow = glyph_atlas_find(&indexed, probe);
        index_ok = inserted.ok() && glyph_atlas_find_indexed(&indexed, &index, key * 0x10001ull).ok() &&
                   fast.ok() == slow.ok() && (!fast.ok() || fast.handle.slot == slow.handle.slot);
    }
    const GlyphAtlasAllocation cached_indexed = glyph_atlas_find_indexed(&indexed, &index, 1'064 * 0x10001ull);
    index_ok = index_ok && index.rebuilds != 0 && indexed.evicted_entries != 0 && cached_indexed.ok() &&
               cached_indexed.cached && cached_indexed.width == 4 && cached_indexed.height == 4;
    glyph_atlas_reset(&indexed);
    index_ok = index_ok && !glyph_atlas_find_indexed(&indexed, &index, 1'064 * 0x10001ull).ok() &&
               glyph_atlas_allocate_unique_indexed(&indexed, &index, 7, 4, 4).ok() &&
               glyph_atlas_find_indexed(&indexed, &index, 7).ok();
    expect(index_ok, "glyph_atlas_key_index_matches_linear_lookup_through_eviction_and_reset");

    // A full shelf hands out its least recently used slot, one at a time, in order.
    GlyphAtlasEntry wrap_entries[3]{};
    GlyphAtlasRow wrap_row[1]{};
    GlyphAtlasAllocator wrap{};
    bool cursor_wrapped = glyph_atlas_initialize(&wrap, 3, 1, 0, {wrap_entries, 3}, {wrap_row, 1});
    for (u32 i = 0; i < 12 && cursor_wrapped; ++i) {
        const auto allocation = glyph_atlas_allocate_unique(&wrap, i + 1, 1, 1);
        cursor_wrapped = allocation.ok() && allocation.handle.slot == i % 3 &&
                         allocation.handle.generation == i / 3 + 1 && allocation.x == i % 3 &&
                         allocation.evicted_entries == (i >= 3 ? 1u : 0u) && wrap.entry_count == minimum(i + 1, 3u) &&
                         (i >= 3 || wrap.free_cursor == (i + 1) % 3);
    }
    expect(cursor_wrapped, "glyph_atlas_full_shelf_reuses_least_recent_slots_in_order");
}

void test_glyph_raster_bounds() noexcept {
    GlyphPathCommand command{};
    GlyphOutline outline{};
    expect(glyph_outline_initialize(&outline, Slice<GlyphPathCommand>{&command, 1}),
           "glyph_raster_outline_initialize");
    outline.count = 1;
    outline.has_bounds = true;
    outline.minimum_x = -0.125f;
    outline.minimum_y = -0.75f;
    outline.maximum_x = 0.625f;
    outline.maximum_y = 0.25f;
    GlyphRasterBounds bounds{};
    expect(glyph_raster_bounds(outline, 16.0f, 1, &bounds) && bounds.left == -3 && bounds.top == -13 &&
               bounds.right == 11 && bounds.bottom == 5 && bounds.width == 14 && bounds.height == 18,
           "glyph_raster_bounds_fractional_padding");
    expect(!glyph_raster_bounds(outline, 0.0f, 1, &bounds) &&
               !glyph_raster_bounds(outline, 16.0f, 65, &bounds),
           "glyph_raster_bounds_reject_scale_padding");
    outline.minimum_x = 2.0f;
    outline.maximum_x = 1.0f;
    expect(!glyph_raster_bounds(outline, 16.0f, 1, &bounds), "glyph_raster_bounds_reject_inverted");
}

void test_text_shaping() noexcept {
    TextSystem system{};
    const TextShapeResult initialized = text_system_initialize(&system, L"Segoe UI", 14.0f);
    expect(initialized.ok() && system.native_text && system.native_format, "text_system_initialize");
    if (!initialized.ok()) {
        return;
    }
    // Test-only DirectWrite oracle for native run font identity, color layers and outlines.
    IDWriteFactory* oracle = nullptr;
    expect(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(&oracle))),
           "text_shaping_directwrite_oracle");

    wchar_t utf16[256]{};
    TextGlyph glyphs[512]{};
    TextRun runs[32]{};
    u16 clusters[256]{};
    TextDecoration decorations[64]{};
    TextHitRegion hit_regions[32]{};
    ShapedText shaped{};
    expect(shaped_text_initialize(&shaped, Slice<TextGlyph>{glyphs, 512}, Slice<TextRun>{runs, 32},
                                  Slice<u16>{clusters, 256}, Slice<TextDecoration>{decorations, 64},
                                  Slice<TextHitRegion>{hit_regions, 32}),
           "shaped_text_initialize");

    TextShapeResult result = text_shape_utf8(&system, utf8_literal("office"), {}, 300.0f, 100.0f,
                                             Slice<wchar_t>{utf16, 256}, &shaped);
    bool clusters_written = result.ok() && shaped.cluster_count == 6;
    for (u32 index = 0; index < shaped.cluster_count; ++index) {
        clusters_written = clusters_written && shaped.clusters[index] != 0xFFFF;
    }
    expect(result.ok() && result.utf16_units == 6 && shaped.glyph_count != 0 && shaped.glyph_count <= 6 &&
               shaped.run_count != 0 && shaped.runs[0].native_font && shaped.width > 0.0f &&
               shaped.height > 0.0f && shaped.line_count == 1 && clusters_written,
           "text_shape_latin_bounded_runs");

    const Utf8View explicit_emoji_samples[]{
        utf8_literal("\xE2\x98\x9D"),                         // bare text-default hand
        utf8_literal("\xE2\x98\x9D\xEF\xB8\x8F"),           // VS16
        utf8_literal("\xE2\x98\x9D\xF0\x9F\x8F\xBD"),       // skin tone
        utf8_literal("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB"), // ZWJ
        utf8_literal("1\xEF\xB8\x8F\xE2\x83\xA3"),          // keycap
        utf8_literal("\xF0\x9F\x92\xAF"),                    // 100
        utf8_literal("\xF0\x9F\xAB\xAA"),                    // no other-font substitution
    };
    bool emoji_family_exact = true;
    system.independent_letters = true;
    system.tracking = .2f;
    for (const auto sample : explicit_emoji_samples) {
        const TextStyleSpan style{0, sample.bytes, text_style_bold | text_style_italic | text_style_monospace};
        const auto shaped_emoji = text_shape_utf8(&system, sample, {&style, 1}, 300, 100,
                                                        {utf16, 256}, &shaped);
        emoji_family_exact &= shaped_emoji.ok() && shaped.run_count != 0;
        for (u32 run = 0; run < shaped.run_count; ++run) {
            IDWriteFontFace* reference = nullptr;
            IDWriteFontFace3* face = nullptr; IDWriteLocalizedStrings* names = nullptr;
            wchar_t family[64]{};
            const bool named = native_run_reference(oracle,shaped,shaped.runs[run],&reference) && SUCCEEDED(reference->QueryInterface(
                __uuidof(IDWriteFontFace3), reinterpret_cast<void**>(&face))) &&
                SUCCEEDED(face->GetFamilyNames(&names)) && SUCCEEDED(names->GetString(0, family, 64));
            constexpr wchar_t expected[] = L"Segoe UI Emoji";
            for (u32 i = 0; i < sizeof(expected)/sizeof(expected[0]); ++i)
                emoji_family_exact &= named && family[i] == expected[i];
            com_release(names); com_release(face);com_release(reference);
        }
        const auto measured = text_measure_utf8(&system, sample, {&style,1}, 300,100,
                                                        {utf16,256},TextWrapping::no_wrap);
        emoji_family_exact &= measured.ok() && measured.width == shaped.width;
    }
    system.independent_letters = false;
    system.tracking = 0;
    expect(emoji_family_exact, "explicit_emoji_font_all_sequences_and_measurement_without_substitution");
    const auto ascii_text = text_shape_utf8(&system, utf8_literal("123*#"), {}, 300, 100,
                                           {utf16,256}, &shaped);
    expect(ascii_text.ok() && shaped.glyph_count == 5,
           "emoji_selection_preserves_plain_digits_and_punctuation");

    result = text_shape_utf8(&system, utf8_literal("\xF0\x9F\x98\x84"), {}, 100.0f, 100.0f,
                             Slice<wchar_t>{utf16, 256}, &shaped);
    IDWriteFactory2* color_factory = nullptr;
    IDWriteColorGlyphRunEnumerator* color_layers = nullptr;
    bool color_layer_found = false;
    if (result.ok() && shaped.run_count == 1 && shaped.glyph_count <= 16 &&
        oracle && SUCCEEDED(oracle->QueryInterface(__uuidof(IDWriteFactory2),
                                                 reinterpret_cast<void**>(&color_factory))) &&
        color_factory) {
        u16 indices[16]{};
        float advances[16]{};
        DWRITE_GLYPH_OFFSET offsets[16]{};
        const TextRun& run = shaped.runs[0];
        for (u32 index = 0; index < run.glyph_count; ++index) {
            const TextGlyph& glyph = shaped.glyphs[run.first_glyph + index];
            indices[index] = glyph.index;
            advances[index] = glyph.advance;
            offsets[index] = DWRITE_GLYPH_OFFSET{glyph.advance_offset, glyph.ascender_offset};
        }
        IDWriteFontFace* reference=nullptr;
        const bool resolved=native_run_reference(oracle,shaped,run,&reference);
        const DWRITE_GLYPH_RUN glyph_run{reference, run.em_size, run.glyph_count, indices,
                                         advances,      offsets,     run.sideways,    run.bidi_level};
        const HRESULT translated = resolved?
            color_factory->TranslateColorGlyphRun(run.baseline_x, run.baseline_y, &glyph_run, nullptr,
                                                  DWRITE_MEASURING_MODE_NATURAL, nullptr, 0, &color_layers):E_FAIL;
        com_release(reference);
        BOOL has_layer = FALSE;
        if (SUCCEEDED(translated) && color_layers && SUCCEEDED(color_layers->MoveNext(&has_layer)) &&
            has_layer) {
            const DWRITE_COLOR_GLYPH_RUN* layer = nullptr;
            color_layer_found = SUCCEEDED(color_layers->GetCurrentRun(&layer)) && layer &&
                                layer->paletteIndex != DWRITE_NO_PALETTE_INDEX;
        }
    }
    if (color_layers) {
        color_layers->Release();
    }
    if (color_factory) {
        color_factory->Release();
    }
    expect(result.ok() && color_layer_found, "text_shape_segoe_emoji_exposes_directwrite_color_layers");

    result = text_shape_utf8(
        &system, utf8_literal("A reply preview that is deliberately wider than its retained row"), {}, 72.0f,
        20.0f, Slice<wchar_t>{utf16, 256}, &shaped, TextWrapping::no_wrap, TextOverflow::ellipsis);
    expect(result.ok() && shaped.line_count == 1 && shaped.glyph_count != 0 && shaped.width <= 72.0f,
           "text_shape_single_line_character_ellipsis");

    constexpr TextStyleSpan styled_spans[] = {
        {0, 4, text_style_bold},
        {5, 4, text_style_monospace},
        {10, 4, text_style_mention},
        {15, 6, text_style_underline | text_style_strikethrough},
    };
    result = text_shape_utf8(&system, utf8_literal("Bold code link strike"),
                             ConstSlice<TextStyleSpan>{styled_spans, 4}, 300.0f, 100.0f,
                             Slice<wchar_t>{utf16, 256}, &shaped);
    bool saw_bold = false;
    bool saw_monospace = false;
    bool saw_mention = false;
    bool saw_lines = false;
    for (u32 index = 0; index < shaped.run_count; ++index) {
        const TextRun& run = shaped.runs[index];
        saw_bold = saw_bold || (run.style_flags & text_style_bold) != 0;
        saw_monospace = saw_monospace ||
                        ((run.style_flags & text_style_monospace) != 0 && run.em_size < system.em_size);
        saw_mention = saw_mention || (run.style_flags & text_style_mention) != 0;
        saw_lines = saw_lines || (run.style_flags & (text_style_underline | text_style_strikethrough)) ==
                                     (text_style_underline | text_style_strikethrough);
    }
    u32 background_count = 0;
    u32 line_count = 0;
    for (u32 index = 0; index < shaped.decoration_count; ++index) {
        background_count += shaped.decorations[index].kind == TextDecorationKind::background ? 1u : 0u;
        line_count += shaped.decorations[index].kind == TextDecorationKind::underline ||
                              shaped.decorations[index].kind == TextDecorationKind::strikethrough
                          ? 1u
                          : 0u;
    }
    expect(result.ok() && saw_bold && saw_monospace && saw_mention && saw_lines && background_count >= 2 &&
               line_count >= 2,
           "text_shape_styled_runs_and_decorations");

    constexpr TextStyleSpan fenced_code_spans[] = {
        {0, 3, text_style_monospace | text_style_code_block | text_style_syntax_type},
        {3, 3, text_style_monospace | text_style_code_block},
        {6, 1, text_style_monospace | text_style_code_block | text_style_syntax_operator},
        {7, 1, text_style_monospace | text_style_code_block},
        {8, 1, text_style_monospace | text_style_code_block | text_style_syntax_number},
        {9, 12, text_style_monospace | text_style_code_block},
    };
    result = text_shape_utf8(&system, utf8_literal("int a = 1;\nreturn a;\n"),
                             ConstSlice<TextStyleSpan>{fenced_code_spans, 6}, 300.0f, 200.0f,
                             Slice<wchar_t>{utf16, 256}, &shaped);
    u32 fenced_backgrounds = 0;
    float fenced_background_height = 0.0f;
    for (u32 index = 0; index < shaped.decoration_count; ++index) {
        const TextDecoration& decoration = shaped.decorations[index];
        if (decoration.kind == TextDecorationKind::background &&
            (decoration.style_flags & text_style_code_block) != 0) {
            ++fenced_backgrounds;
            fenced_background_height = decoration.bottom - decoration.top;
        }
    }
    expect(result.ok() && fenced_backgrounds == 1 && fenced_background_height > system.em_size * 1.5f,
           "text_shape_fenced_code_uses_one_multiline_container");

    constexpr TextHitSpan interactive_hits[] = {
        {0, 10, 7, 0},
        {6, 10, 9, 0},
    };
    result = text_shape_utf8(
        &system, utf8_literal("alpha beta gamma delta"), ConstSlice<TextStyleSpan>{},
        70.0f, 200.0f, Slice<wchar_t>{utf16, 256}, &shaped, TextWrapping::wrap, TextOverflow::clip, ConstSlice<TextHitSpan>{interactive_hits, 2});
    bool saw_hit_7 = false;
    bool saw_hit_9 = false;
    bool positive_hits = result.ok() && shaped.hit_region_count >= 2;
    for (u32 index = 0; index < shaped.hit_region_count; ++index) {
        const TextHitRegion& region = shaped.hit_regions[index];
        saw_hit_7 = saw_hit_7 || region.semantic_index == 7;
        saw_hit_9 = saw_hit_9 || region.semantic_index == 9;
        positive_hits = positive_hits && region.right > region.left && region.bottom > region.top;
    }
    expect(result.ok() && saw_hit_7 && saw_hit_9 && positive_hits,
           "text_shape_interactive_overlapping_wrapped_ranges");

    constexpr TextHitSpan split_hit[] = {{1, 1, 1, 0}};
    const TextShapeResult split_hit_result = text_shape_utf8(
        &system, utf8_literal("\xC3\xA9x"), ConstSlice<TextStyleSpan>{},
        300.0f, 100.0f, Slice<wchar_t>{utf16, 256}, &shaped, TextWrapping::wrap, TextOverflow::clip, ConstSlice<TextHitSpan>{split_hit, 1});
    expect(split_hit_result.error == TextShapeError::invalid_input, "text_shape_hit_rejects_utf8_split");

    constexpr TextStyleSpan list_span[] = {
        {0, 6, text_style_list | text_style_list_bullet | text_style_list_depth_1},
    };
    result =
        text_shape_utf8(&system, utf8_literal("  list"), ConstSlice<TextStyleSpan>{list_span, 1},
                        300.0f, 100.0f, Slice<wchar_t>{utf16, 256}, &shaped);
    bool list_bullet = false;
    for (u32 index = 0; index < shaped.decoration_count; ++index) {
        list_bullet = list_bullet || shaped.decorations[index].kind == TextDecorationKind::list_bullet;
    }
    expect(result.ok() && list_bullet, "text_shape_list_bullet_decoration");

    constexpr TextStyleSpan split_list_spans[] = {
        {0, 2, text_style_list | text_style_list_bullet},
        {2, 4, text_style_list | text_style_list_bullet | text_style_mention},
        {7, 8, text_style_list | text_style_list_bullet | text_style_mention},
    };
    result = text_shape_utf8(&system, utf8_literal("  @Mai #general"),
                             ConstSlice<TextStyleSpan>{split_list_spans, 3}, 300.0f, 100.0f,
                             Slice<wchar_t>{utf16, 256}, &shaped);
    u32 split_list_bullets = 0;
    for (u32 index = 0; index < shaped.decoration_count; ++index) {
        split_list_bullets += shaped.decorations[index].kind == TextDecorationKind::list_bullet ? 1u : 0u;
    }
    expect(result.ok() && split_list_bullets == 1, "text_shape_list_bullet_once_across_style_partitions");

    // Chat lists: every unordered item (any marker or depth, after a paragraph) gets its own bullet at the
    // item's line, none for ordered items.
    {
        struct ListCase {
            Utf8View source;
            u32 bullets;
        };
        const ListCase list_cases[] = {
            {utf8_literal("- hi\n- hi\n- hi"), 3},
            {utf8_literal("* a\n* b\n* c\n* d"), 4},
            {utf8_literal("1. one\n2. two\n3. three"), 0},
            {utf8_literal("- a\n  - b\n  - c\n- d"), 4},
            {utf8_literal("intro\n- one\n- **two**\n- three `x`\nafter"), 3},
            {utf8_literal("- one\n1. two\n- three"), 2},
        };
        for (const ListCase& item : list_cases) {
            char list_text[256]{};
            u32 list_bytes[256]{};
            TextStyleSpan list_styles[64]{};
            const MessageMarkdownResult parsed =
                message_markdown_parse(item.source, Slice<char>{list_text, 256}, Slice<u32>{list_bytes, 256},
                                       Slice<TextStyleSpan>{list_styles, 64});
            result = text_shape_utf8(&system, parsed.text, ConstSlice<TextStyleSpan>{list_styles, parsed.span_count},
                                     300.0f, 400.0f, Slice<wchar_t>{utf16, 256}, &shaped);
            u32 bullets = 0;
            float previous_top = -1.0f;
            bool ordered = parsed.ok() && result.ok();
            for (u32 index = 0; index < shaped.decoration_count; ++index) {
                if (shaped.decorations[index].kind != TextDecorationKind::list_bullet) continue;
                ++bullets;
                ordered = ordered && shaped.decorations[index].top > previous_top;
                previous_top = shaped.decorations[index].top;
            }
            expect(ordered && bullets == item.bullets, "text_shape_list_bullet_per_item");
        }
    }

    constexpr TextStyleSpan overlap_spans[] = {
        {0, 4, text_style_bold},
        {3, 2, text_style_italic},
    };
    result =
        text_shape_utf8(&system, utf8_literal("abcdef"), ConstSlice<TextStyleSpan>{overlap_spans, 2},
                        300.0f, 100.0f, Slice<wchar_t>{utf16, 256}, &shaped);
    constexpr TextStyleSpan split_unicode[] = {
        {1, 1, text_style_bold},
    };
    const TextShapeResult split_result = text_shape_utf8(
        &system, utf8_literal("\xC3\xA9x"), ConstSlice<TextStyleSpan>{split_unicode, 1}, 300.0f, 100.0f,
        Slice<wchar_t>{utf16, 256}, &shaped);
    expect(result.error == TextShapeError::invalid_input &&
               split_result.error == TextShapeError::invalid_input,
           "text_shape_style_ranges_reject_overlap_and_utf8_split");

    result = text_shape_utf8(&system, utf8_literal("office"), {}, 300.0f, 100.0f, Slice<wchar_t>{utf16, 256},
                             &shaped);
    expect(result.ok() && shaped.run_count != 0, "text_shape_plain_after_styled_reset");

    GlyphPathCommand path_commands[256]{};
    IDWriteFontFace* outline_reference=nullptr;
    expect(native_run_reference(oracle,shaped,shaped.runs[0],&outline_reference),"native_run_outline_reference");
    GlyphOutline outline{};
    expect(glyph_outline_initialize(&outline, Slice<GlyphPathCommand>{path_commands, 256}),
           "glyph_outline_initialize");
    const GlyphOutlineResult outlined =
        text_outline_glyph(outline_reference, shaped.glyphs[0].index, &outline);
    const bool begins =
        outline.count != 0 && (outline.commands[0].kind == GlyphPathCommandKind::begin_filled ||
                               outline.commands[0].kind == GlyphPathCommandKind::begin_hollow);
    const bool ends = outline.count != 0 &&
                      (outline.commands[outline.count - 1].kind == GlyphPathCommandKind::end_closed ||
                       outline.commands[outline.count - 1].kind == GlyphPathCommandKind::end_open);
    expect(outlined.ok() && outline.count != 0 && outline.has_bounds &&
               outline.maximum_x > outline.minimum_x && outline.maximum_y > outline.minimum_y && begins &&
               ends,
           "glyph_outline_normalized_contours");

    GlyphPathCommand tiny_path[1]{};
    GlyphOutline tiny_outline{};
    expect(glyph_outline_initialize(&tiny_outline, Slice<GlyphPathCommand>{tiny_path, 1}),
           "glyph_outline_tiny_initialize");
    const GlyphOutlineResult tiny_outlined =
        text_outline_glyph(outline_reference, shaped.glyphs[0].index, &tiny_outline);
    expect(tiny_outlined.error == GlyphOutlineError::output_too_small && tiny_outline.count == 0 &&
               !tiny_outline.has_bounds && tiny_outline.rejected == 1,
           "glyph_outline_atomic_bound");
    com_release(outline_reference);
    shaped_text_reset(&shaped);
    expect(shaped.glyph_count == 0 && shaped.run_count == 0 && shaped.cluster_count == 0,
           "shaped_text_reset_releases_runs");

    const char malformed[] = {static_cast<char>(0xC0), static_cast<char>(0x80)};
    result = text_shape_utf8(&system, Utf8View{malformed, 2}, {}, 300.0f, 100.0f, Slice<wchar_t>{utf16, 256},
                             &shaped);
    expect(result.error == TextShapeError::invalid_utf8 && shaped.run_count == 0,
           "text_shape_reject_invalid_utf8");

    result = text_shape_utf8(&system, Utf8View{"\xF0\x9F\x98\x80", 4}, {}, 300.0f, 100.0f,
                             Slice<wchar_t>{utf16, 1}, &shaped);
    expect(result.error == TextShapeError::output_too_small && result.utf16_units == 2,
           "text_shape_utf16_bound");

    TextGlyph tiny_glyph[1]{};
    TextRun tiny_run[1]{};
    u16 tiny_clusters[32]{};
    ShapedText tiny{};
    expect(shaped_text_initialize(&tiny, Slice<TextGlyph>{tiny_glyph, 1}, Slice<TextRun>{tiny_run, 1},
                                  Slice<u16>{tiny_clusters, 32}),
           "text_shape_tiny_initialize");
    result = text_shape_utf8(&system, utf8_literal("bounded output"), {}, 300.0f, 100.0f,
                             Slice<wchar_t>{utf16, 256}, &tiny);
    expect(result.error == TextShapeError::output_too_small && tiny.glyph_count == 0 && tiny.run_count == 0,
           "text_shape_glyph_bound_atomic");

    result = text_shape_utf8(&system, utf8_literal("first\nsecond"), {}, 300.0f, 100.0f,
                             Slice<wchar_t>{utf16, 256}, &shaped);
    bool multiline_clusters = result.ok() && shaped.cluster_count == 12 && shaped.clusters[5] == 0xFFFF;
    for (u32 index = 0; index < shaped.cluster_count && multiline_clusters; ++index) {
        multiline_clusters = index == 5 || shaped.clusters[index] != 0xFFFF;
    }
    expect(result.ok() && result.utf16_units == 12 && shaped.glyph_count != 0 && shaped.line_count == 2 &&
               multiline_clusters,
           "text_shape_hard_break_non_rendering_cluster");

    const Utf8View arabic{"\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7", 10};
    result = text_shape_utf8(&system, arabic, {}, 300.0f, 100.0f, Slice<wchar_t>{utf16, 256}, &shaped);
    bool right_to_left = false;
    for (u32 index = 0; index < shaped.run_count; ++index) {
        right_to_left = right_to_left || (shaped.runs[index].bidi_level & 1u) != 0;
    }
    expect(result.ok() && result.utf16_units == 5 && shaped.glyph_count != 0 && right_to_left,
           "text_shape_arabic_bidi");
    shaped_text_reset(&shaped);
    text_system_release(&system);
    com_release(oracle);
    expect(!system.native_text && !system.native_fonts, "text_system_release");
}

} // namespace maicord
