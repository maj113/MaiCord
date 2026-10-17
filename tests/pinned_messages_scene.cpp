#include "maicord/ui/pinned_messages_scene.hpp"
#include "maicord/ui/pinned_messages_avatar_media.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
struct PinsFixture {
    u32 resolves{};
    bool current{true}, manage{true}, pending{}, invalid_avatar{}, authoritative{true};
    float invalid_height{};
    bool override_height{};
};
bool resolve_pin(void* context, u32 index, UiPinnedMessagesRow* row) noexcept {
    auto& f = *static_cast<PinsFixture*>(context);
    ++f.resolves;
    if (!row || index >= kUiPinnedMessagesPageCapacity) return false;
    *row = {71, 100 + index, 200 + index, utf8_literal("Native author"),
            utf8_literal("Pinned Oct 5 at 10:30"),
            utf8_literal("An original pinned note with a useful decision and a clear next step."),
            index % 2 ? utf8_literal("2 attachments · 1 embed") : Utf8View{},
            utf8_literal("0123456789abcdef0123456789abcdef"),
            index % 2 ? 96.f : 24.f, f.manage, f.pending && index == 0};
    if (f.override_height) row->body_height = f.invalid_height;
    if (f.invalid_avatar) row->avatar_hash = utf8_literal("not-an-avatar-hash");
    row->jump_enabled = f.authoritative;
    return true;
}
bool pins_current(void* context) noexcept { return static_cast<PinsFixture*>(context)->current; }
}
void test_pinned_messages_scene(void (*check)(bool, const char*) noexcept) noexcept {
    PinsFixture f{};
    UiPinnedMessagesSource source{&f, 50, 1, resolve_pin, pins_current};
    UiPinnedMessagesContent content{};
    content.channel = utf8_literal("# native-notes");
    content.channel_id = 71;
    content.revision = 1;
    content.phase = UiPinnedMessagesPhase::ready;
    content.has_next = true;
    UiPinnedMessagesScene s{};
    check(ui_pinned_messages_scene_update(&s, &source, &content, {100, 48, 1300, 780}) &&
              s.active && ui_rect_width(s.panel) == kUiPinnedMessagesPreferredWidth &&
              s.count > 0 && s.count < 50 && s.widgets.count <= kUiPinnedMessagesWidgetCapacity &&
              ui_pinned_messages_scene_scroll_maximum(s) > 5000,
          "pins scene: bounded native right pane materializes only clipped accepted-page rows");
    if (!s.active) return;
    const u32 resolves = f.resolves, revision = s.revision;
    const u16 generation = s.widgets.generation;
    check(ui_pinned_messages_scene_update(&s, &source, &content, s.viewport) &&
              f.resolves == resolves && s.revision == revision && s.widgets.generation == generation,
          "pins scene: unchanged publication performs no row resolution or retained rebuild");
    const auto card = s.cards[0];
    PinnedMessagesAvatarMedia avatar{};
    check(pinned_messages_avatar_media_resolve(s, 0, &avatar) &&
              avatar.key.id == 200 && pinned_messages_avatar_media_current(s, avatar.key),
          "pins scene: visible avatar belongs to original accepted author and validated hash");
    check(ui_pinned_messages_scene_scroll(&s, 56) && s.first == 0 &&
              !pinned_messages_avatar_media_resolve(s, 0, &avatar) &&
              ui_pinned_messages_scene_scroll(&s, 0),
          "pins scene: clipped author avatar is culled before media admission while card excerpt remains visible");
    f.invalid_avatar = true;
    ++source.revision;
    check(ui_pinned_messages_scene_update(&s, &source, &content, s.viewport) &&
              !pinned_messages_avatar_media_resolve(s, 0, &avatar),
          "pins scene: malformed accepted avatar hash cannot fabricate a CDN identity");
    f.invalid_avatar = false;
    ++source.revision;
    check(ui_pinned_messages_scene_update(&s, &source, &content, s.viewport),
          "pins scene: valid avatar receipt restores its own copied identity");
    f.pending = true;
    ++source.revision;
    check(ui_pinned_messages_scene_update(&s, &source, &content, s.viewport) &&
              ui_pinned_messages_scene_hit_test(s, s.cards[0].right - 20, s.cards[0].top + 20).action == UiPinnedMessagesAction::none &&
              !ui_pinned_messages_scene_focus(&s, {UiPinnedMessagesAction::unpin, 71, 100}) &&
              ui_pinned_messages_scene_hit_test(s, s.cards[0].left + 20, s.cards[0].top + 64).action == UiPinnedMessagesAction::jump,
          "pins scene: pending Unpin consumes its slot without duplicate mutation or accidental card jump");
    f.pending = false;
    ++source.revision;
    check(ui_pinned_messages_scene_update(&s, &source, &content, s.viewport),
          "pins scene: completed mutation restores available controls");
    f.authoritative = false;
    f.manage = false;
    ++source.revision;
    check(ui_pinned_messages_scene_update(&s, &source, &content, s.viewport) && s.count &&
              ui_pinned_messages_scene_hit_test(s, s.cards[0].left + 20, s.cards[0].top + 64).action == UiPinnedMessagesAction::none &&
              !ui_pinned_messages_scene_focus(&s, {UiPinnedMessagesAction::jump, 71, 100}),
          "pins scene: retained stale previews remain readable while navigation is unavailable");
    f.authoritative = f.manage = true;
    ++source.revision;
    check(ui_pinned_messages_scene_update(&s, &source, &content, s.viewport),
          "pins scene: authoritative replacement restores exact original page navigation");
    check(ui_pinned_messages_scene_hit_test(s, card.left + 20, card.top + 64).action == UiPinnedMessagesAction::jump &&
              ui_pinned_messages_scene_hit_test(s, card.right - 20, card.top + 20).action == UiPinnedMessagesAction::unpin &&
              ui_pinned_messages_scene_hit_test(s, s.footer.left + 24, s.footer.top + 24).action == UiPinnedMessagesAction::none &&
              ui_pinned_messages_scene_hit_test(s, s.footer.right - 24, s.footer.top + 24).action == UiPinnedMessagesAction::next,
          "pins scene: card jump and permission unpin have distinct targets, disabled Newer never activates");
    check(ui_pinned_messages_scene_focus(&s, {UiPinnedMessagesAction::jump, 71, 149}) &&
              ui_pinned_messages_scene_focused_target(s).message_id == 149 && s.scroll_dips > 0 &&
              s.first > 0 && s.visible[s.count - 1].message_id == 149 &&
              ui_pinned_messages_scene_move_focus(&s, UiPinnedMessagesFocusMove::next) &&
              ui_pinned_messages_scene_focused_target(s).action == UiPinnedMessagesAction::unpin &&
              ui_pinned_messages_scene_move_focus(&s, UiPinnedMessagesFocusMove::next) &&
              ui_pinned_messages_scene_focused_target(s).action == UiPinnedMessagesAction::next,
          "pins scene: keyboard traverses offscreen final row, unpin and Older with semantic identities");
    check(ui_pinned_messages_scene_move_focus(&s, UiPinnedMessagesFocusMove::first) &&
              ui_pinned_messages_scene_focused_target(s).action == UiPinnedMessagesAction::close &&
              ui_pinned_messages_scene_move_focus(&s, UiPinnedMessagesFocusMove::next) &&
              ui_pinned_messages_scene_focused_target(s).action == UiPinnedMessagesAction::refresh &&
              ui_pinned_messages_scene_move_focus(&s, UiPinnedMessagesFocusMove::next) && s.scroll_dips == 0,
          "pins scene: keyboard wraps through header and reveals first row again");
    PaintStream paint{};
    UiTextCommand commands[128]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1300, 780) &&
              ui_text_command_stream_initialize(&stream, {commands, 128}) &&
              ui_pinned_messages_scene_paint(s, kDefaultTheme, &paint) &&
              ui_pinned_messages_scene_text_output(s, kDefaultTheme, &output) &&
              paint.rejected == 0 && paint.clip_depth == 1 && stream.rejected == 0 && stream.count > 10,
          "pins scene: native theme paint and clipped UTF8 excerpt text emit without stream rejection");
    bool clipped = true;
    for (u32 i = 0; i < stream.count; ++i) {
        const auto& command = stream.storage[i];
        clipped &= command.clip.left >= s.panel.left && command.clip.right <= s.panel.right &&
                   command.clip.top >= s.panel.top && command.clip.bottom <= s.panel.bottom;
    }
    check(clipped, "pins scene: every author, metadata and wrapped excerpt stays clipped to the pane");
    const u32 before_hover = f.resolves;
    bool changed = false;
    check(ui_pinned_messages_scene_set_hover(&s, s.cards[0].left + 20, s.cards[0].top + 64, &changed) &&
              changed && f.resolves == before_hover,
          "pins scene: pointer hover uses retained targets without resolving or measuring accepted rows");
    f.manage = false;
    ++source.revision;
    ++content.revision;
    check(ui_pinned_messages_scene_update(&s, &source, &content, {0, 48, 300, 420}) && s.overlay &&
              ui_rect_width(s.panel) == 300 && !s.unpin[0].generation &&
              ui_pinned_messages_scene_hit_test(s, s.cards[0].right - 20, s.cards[0].top + 20).action == UiPinnedMessagesAction::jump &&
              !ui_pinned_messages_scene_focus(&s, {UiPinnedMessagesAction::unpin, 71, 100}),
          "pins scene: narrow overlay retains card geometry and hides unavailable Unpin controls");
    check(ui_pinned_messages_scene_scroll(&s, 500) && s.scroll_dips == 500,
          "pins scene: narrow body supports virtual wheel scrolling");
    const float offset = s.scroll_dips;
    check(ui_pinned_messages_scene_focus(&s, {UiPinnedMessagesAction::next, 71}),
          "pins scene: keyboard Older focus owns the pending page request");
    content.loading_more = true;
    ++content.revision;
    check(ui_pinned_messages_scene_update(&s, &source, &content, s.viewport) && s.scroll_dips == offset &&
              !ui_pinned_messages_scene_focus(&s, {UiPinnedMessagesAction::next, 71}),
          "pins scene: pending pagination preserves old page offset and disables duplicate Older requests");
    content.loading_more = false;
    content.has_previous = true;
    content.page_index = 1;
    ++source.revision;
    ++content.revision;
    check(ui_pinned_messages_scene_update(&s, &source, &content, s.viewport) && s.scroll_dips == 0 &&
              ui_pinned_messages_scene_focused_target(s).action == UiPinnedMessagesAction::next &&
              ui_pinned_messages_scene_focus(&s, {UiPinnedMessagesAction::previous, 71}),
          "pins scene: accepted Older page resets scroll, restores semantic pagination focus and exposes Newer");
    f.current = false;
    paint_stream_reset(&paint, 300, 420);
    ui_text_command_stream_reset(&stream);
    check(!ui_pinned_messages_scene_current(s) && !pinned_messages_avatar_media_resolve(s, 0, &avatar) &&
              ui_pinned_messages_scene_hit_test(s, s.panel.right - 28, s.panel.top + 28).action == UiPinnedMessagesAction::none &&
              !ui_pinned_messages_scene_move_focus(&s, UiPinnedMessagesFocusMove::next) &&
              ui_pinned_messages_scene_paint(s, kDefaultTheme, &paint) && paint.count == 0 &&
              ui_pinned_messages_scene_text_output(s, kDefaultTheme, &output) && stream.count == 0,
          "pins scene: retired account/channel receipt cannot expose old actions or borrowed text");
    f.current = true;
    source.count = 0;
    ++source.revision;
    content.phase = UiPinnedMessagesPhase::loading;
    content.has_next = true;
    ++content.revision;
    check(ui_pinned_messages_scene_update(&s, &source, &content, s.viewport) &&
              !s.previous.generation && !s.next.generation &&
              !ui_pinned_messages_scene_focus(&s, {UiPinnedMessagesAction::next, 71}) &&
              !ui_pinned_messages_scene_focus(&s, {UiPinnedMessagesAction::refresh, 71}) &&
              ui_pinned_messages_scene_focus(&s, {UiPinnedMessagesAction::close, 71}),
          "pins scene: initial loading cannot expose stale pagination or duplicate refresh actions");
    content.phase = UiPinnedMessagesPhase::failed;
    content.status = utf8_literal("The request failed. Your messages are unchanged.");
    ++content.revision;
    check(ui_pinned_messages_scene_update(&s, &source, &content, s.viewport) && s.retry.generation &&
              ui_pinned_messages_scene_focus(&s, {UiPinnedMessagesAction::retry, 71}),
          "pins scene: failed empty receipt presents real Retry action without fabricated rows");
    source.count = 1;
    f.override_height = true;
    const u32 nan_bits = 0x7fc00000u;
    copy_bytes(&f.invalid_height, &nan_bits, sizeof(nan_bits));
    ++source.revision;
    check(!ui_pinned_messages_scene_update(&s, &source, &content, s.viewport),
          "pins scene: nonfinite excerpt height fails before modifying retained widget geometry");
    paint_stream_release(&paint);
    ui_pinned_messages_scene_release(&s);
    check(!s.active && !s.storage.arena.base, "pins scene: lazy fixed storage releases on close");

    UiScene shell{};
    auto metrics = kGuildChannelShellLayoutMetrics;
    metrics.flags |= ui_shell_layout_pins;
    ++metrics.revision;
    UiLayoutDiagnostic diagnostics[32]{};
    check(ui_scene_initialize_bootstrap(&shell) &&
              ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, metrics, 1280, 800),
          "pins launcher: permission-enabled native header initializes");
    const auto wide = ui_scene_header_geometry(shell);
    const auto* pins = ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::pins_toggle));
    // The inbox holds the trailing edge after the search field.
    check(pins && ui_rect_equal(wide.member_toggle, {900, 8, 936, 40}) &&
              ui_rect_equal(wide.search_field, {944, 8, 1224, 40}) && ui_rect_equal(wide.inbox, {1228, 8, 1264, 40}) &&
              ui_rect_equal(wide.pins, {856, 8, 892, 40}) && wide.label.right <= wide.pins.left - 8 &&
              ui_widget_manager_hit_test(shell.widgets, 872, 24).slot == pins->widget.slot &&
              ui_layout_engine_validate(shell.widgets.layout, {diagnostics, 32}).clean(),
          "pins launcher: distinct pin target preserves wide member/search anchors and title clearance");
    const u32 rebuilds = shell.layout_rebuilds;
    check(ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, metrics, 640, 800) &&
              shell.layout_rebuilds == rebuilds + 1 &&
              !ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::member_sidebar)) &&
              ui_rect_width(ui_scene_header_geometry(shell).label) >= 72.f &&
              ui_rect_width(ui_scene_header_geometry(shell).search_field) >= 120.f &&
              ui_scene_header_geometry(shell).pins.left >= ui_scene_header_geometry(shell).title.left &&
              ui_scene_header_geometry(shell).pins.right < ui_scene_header_geometry(shell).member_toggle.left &&
              ui_scene_header_geometry(shell).member_toggle.right < ui_scene_header_geometry(shell).search_field.left &&
              ui_layout_engine_validate(shell.widgets.layout, {diagnostics, 32}).clean(),
          "pins launcher: narrow native header bounds all controls without overlapping hit regions");
    check(ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, metrics, 632, 800) &&
              ui_rect_width(ui_scene_header_geometry(shell).label) == 72.f &&
              ui_rect_width(ui_scene_header_geometry(shell).search_field) == 120.f &&
              ui_scene_header_geometry(shell).label.right + 8.f == ui_scene_header_geometry(shell).pins.left &&
              ui_scene_header_geometry(shell).pins.right + 8.f == ui_scene_header_geometry(shell).member_toggle.left &&
              ui_scene_header_geometry(shell).member_toggle.right + 8.f == ui_scene_header_geometry(shell).search_field.left &&
              ui_layout_engine_validate(shell.widgets.layout, {diagnostics, 32}).clean(),
          "pins launcher: minimum message width retains readable channel label and keyboard-sized search");
    metrics.flags &= static_cast<u16>(~ui_shell_layout_pins);
    ++metrics.revision;
    check(ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, metrics, 1280, 800) &&
              !ui_rect_has_area(ui_scene_header_geometry(shell).pins),
          "pins launcher: lost selected-channel authority removes retained action and geometry");
    auto friends_metrics = kFriendsShellLayoutMetrics;
    friends_metrics.flags |= ui_shell_layout_pins;
    ++friends_metrics.revision;
    check(ui_scene_layout_with_metrics(&shell, UiShellView::friends, friends_metrics, 1280, 800) &&
              !ui_rect_has_area(ui_scene_header_geometry(shell).pins),
          "pins launcher: Friends header never exposes channel Pins even with an invalid host flag");
    ui_scene_release(&shell);
}
} // namespace maicord
