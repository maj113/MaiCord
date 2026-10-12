#include "maicord/ui/inbox_scene.hpp"
#include "maicord/ui/inbox_avatar_media.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
struct InboxFixture {
    UiInboxTab tab{};
    bool current{true};
};
bool resolve(void* context, u32 index, UiInboxRow* row) noexcept {
    const auto& f = *static_cast<InboxFixture*>(context);
    if (!row)
        return false;
    if (f.tab == UiInboxTab::mentions) {
        *row = {UiInboxRowKind::mention, 500 + index, 70 + index % 3, 9, utf8_literal("Ana"),
                utf8_literal("#general · Native Lab"), utf8_literal("@you the build is green, ship it"),
                utf8_literal("5 minutes ago")};
        // Odd authors have no custom avatar (default avatar, no media).
        row->author_id = 40 + index;
        row->avatar_hash = index % 2 ? Utf8View{} : utf8_literal("0123456789abcdef0123456789abcdef");
        return true;
    }
    // Two servers: a header then two channels each.
    if (index % 3 == 0)
        *row = {UiInboxRowKind::group, 9 + index / 3, 0, 9 + index / 3, utf8_literal("Native Lab")};
    else
        *row = {UiInboxRowKind::channel, 80 + index, 80 + index, 9 + index / 3, utf8_literal("design"), {}, {}, {},
                index == 1 ? 3u : 0u, index == 2};
    return true;
}
bool current(void* context) noexcept {
    return static_cast<InboxFixture*>(context)->current;
}
} // namespace

void test_inbox_scene(void (*check)(bool, const char*) noexcept) noexcept {
    InboxFixture f{};
    UiInboxSource source{&f, 12, 1, resolve, current};
    UiInboxContent content{};
    content.revision = 1;
    content.phase = UiInboxPhase::ready;
    content.this_server_available = true;
    content.has_more = true;
    UiInboxScene scene{};
    const UiRect viewport{240, 48, 1280, 800};
    check(ui_inbox_scene_update(&scene, &source, &content, viewport) && ui_inbox_scene_current(scene) &&
              ui_rect_equal(scene.panel, {792, 52, 1272, 652}) && ui_rect_has_area(scene.filters) &&
              ui_rect_has_area(scene.footer) && scene.count > 0 && scene.count <= kUiInboxVisibleRows,
          "inbox scene: mentions popout hangs from the header's right edge with filters and Load more");
    const auto tab = ui_inbox_scene_hit_test(scene, 930, 76);
    const auto chip = ui_inbox_scene_hit_test(scene, scene.filters.left + 20, (scene.filters.top + scene.filters.bottom) * .5f);
    const auto row = ui_inbox_scene_hit_test(scene, 900, scene.body.top + 40);
    const auto more = ui_inbox_scene_hit_test(scene, (scene.footer.left + scene.footer.right) * .5f,
                                              (scene.footer.top + scene.footer.bottom) * .5f);
    check(tab.action == UiInboxAction::tab_mentions && chip.action == UiInboxAction::filter_server &&
              row.action == UiInboxAction::jump && row.id == 500 && row.channel_id == 70 &&
              more.action == UiInboxAction::load_more &&
              ui_inbox_scene_hit_test(scene, 100, 100).action == UiInboxAction::none,
          "inbox scene: tabs, filters, mention cards and Load more map to their actions");
    // A one-line message gets a one-line card: no empty band under the text.
    const auto first_card = scene.row_rectangles[0], second_card = scene.row_rectangles[1];
    check(ui_rect_height(first_card) == 48 + 22 + 10 && second_card.top - first_card.bottom == 8 &&
              ui_rect_width(first_card) == ui_rect_width(scene.panel) - 28,
          "inbox scene: mention cards fit a one-line message with an 8 DIP gap between cards");
    InboxAvatarMedia avatar{}, blank{};
    UiRect avatar_rect{}, avatar_clip{};
    const bool avatar_ok = inbox_avatar_media_resolve(scene, 0, &avatar);
    check(avatar_ok && avatar.key.kind == MediaAssetKind::user_avatar && avatar.key.id == 40 &&
              avatar.key.variant == user_avatar_variant(utf8_literal("0123456789abcdef0123456789abcdef")) &&
              ui_rect_width(avatar.rectangle) == kUiInboxAvatarExtent &&
              ui_rect_equal(avatar.rectangle, ui_rect_intersection(avatar.rectangle, scene.row_rectangles[0])) &&
              inbox_avatar_media_current(scene, avatar.key) && !inbox_avatar_media_resolve(scene, 1, &blank) &&
              ui_inbox_scene_avatar(scene, 1, &avatar_rect, &avatar_clip),
          "inbox scene: mention cards resolve their author's avatar identity; default avatars keep the placeholder");
    check(ui_inbox_scene_scroll(&scene, 10000) && scene.scroll_dips == ui_inbox_scene_scroll_maximum(scene) &&
              !inbox_avatar_media_current(scene, avatar.key) && ui_inbox_scene_scroll(&scene, 0),
          "inbox scene: a scrolled-out card's avatar is no longer current media");
    check(ui_inbox_scene_scroll(&scene, 10000) && scene.scroll_dips == ui_inbox_scene_scroll_maximum(scene) &&
              scene.scroll_dips > 0 && ui_inbox_scene_scroll(&scene, 0) && scene.first == 0,
          "inbox scene: scrolling clamps to the measured list");
    check(ui_inbox_scene_focus(&scene, {UiInboxAction::jump, 511, 70 + 11 % 3}) && scene.scroll_dips > 0 &&
              ui_inbox_scene_focused_target(scene).id == 511,
          "inbox scene: focusing an offscreen mention reveals it");
    check(ui_inbox_scene_move_focus(&scene, UiInboxFocusMove::first) &&
              ui_inbox_scene_focused_target(scene).action == UiInboxAction::tab_mentions &&
              ui_inbox_scene_move_focus(&scene, UiInboxFocusMove::last) &&
              ui_inbox_scene_focused_target(scene).action == UiInboxAction::close,
          "inbox scene: keyboard focus walks tabs, filters, rows and close");
    PaintStream paint{};
    UiTextCommand commands[256]{};
    UiTextCommandStream text{};
    UiTextOutput output{&text};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1280, 800) &&
              ui_inbox_scene_paint(scene, kDefaultTheme, &paint) && paint.count > 0 && paint.clip_depth == 1 && paint.rejected == 0 &&
              ui_text_command_stream_initialize(&text, {commands, 256}) &&
              ui_inbox_scene_text_output(scene, kDefaultTheme, &output) && text.count > 6,
          "inbox scene: paints and emits text inside its panel");
    bool clipped = true;
    for (u32 i = 0; i < text.count; ++i)
        clipped = clipped && ui_rect_equal(text.storage[i].clip, ui_rect_intersection(text.storage[i].clip, scene.panel));
    check(clipped, "inbox scene: every text command stays clipped to the panel");

    f.tab = UiInboxTab::unreads;
    content.tab = UiInboxTab::unreads;
    content.has_more = false;
    ++content.revision;
    source.count = 6;
    ++source.revision;
    check(ui_inbox_scene_update(&scene, &source, &content, viewport) && !ui_rect_has_area(scene.filters) &&
              !ui_rect_has_area(scene.footer) && scene.scroll_dips == 0,
          "inbox scene: Unreads has no filters or footer and starts at the top");
    const auto group = scene.row_rectangles[0];
    const auto mark_guild = ui_inbox_scene_hit_test(scene, group.right - 20, (group.top + group.bottom) * .5f);
    const auto channel = scene.row_rectangles[1];
    const auto open = ui_inbox_scene_hit_test(scene, channel.left + 40, (channel.top + channel.bottom) * .5f);
    const auto mark = ui_inbox_scene_hit_test(scene, channel.right - 20, (channel.top + channel.bottom) * .5f);
    check(mark_guild.action == UiInboxAction::mark_guild && mark_guild.id == 9 &&
              open.action == UiInboxAction::open_channel && open.id == 81 && mark.action == UiInboxAction::mark_channel &&
              mark.id == 81,
          "inbox scene: server headers mark the server read; channel rows open or mark the channel");
    // Channel mark-read buttons stay out of sight until the row is hovered or
    // the keyboard focuses the row or the button; the server header keeps its
    // quiet "Mark all read" text button.
    bool hover_changed = false;
    const auto check_button = ui_inbox_scene_action_rectangle(scene, 1);
    const auto all_read = ui_inbox_scene_action_rectangle(scene, 0);
    const bool hidden = !ui_inbox_scene_channel_action_visible(scene, 1) &&
                        !ui_inbox_scene_channel_action_visible(scene, 2) &&
                        !ui_inbox_scene_channel_action_visible(scene, 0);
    const bool hovered_row = ui_inbox_scene_set_hover(&scene, channel.left + 40, (channel.top + channel.bottom) * .5f,
                                                      &hover_changed) &&
                             hover_changed && ui_inbox_scene_channel_action_visible(scene, 1) &&
                             !ui_inbox_scene_channel_action_visible(scene, 2);
    const bool hovered_away = ui_inbox_scene_set_hover(&scene, 10, 10, &hover_changed) &&
                              !ui_inbox_scene_channel_action_visible(scene, 1);
    const bool keyboard = ui_inbox_scene_focus(&scene, {UiInboxAction::mark_channel, 82, 82}) &&
                          ui_inbox_scene_channel_action_visible(scene, 2) &&
                          !ui_inbox_scene_channel_action_visible(scene, 1);
    check(hidden && hovered_row && hovered_away && keyboard &&
              ui_rect_width(check_button) == ui_rect_height(check_button) && ui_rect_width(check_button) == 28 &&
              ui_rect_equal(check_button, ui_rect_intersection(check_button, channel)) &&
              ui_rect_width(all_read) >= 96 && ui_rect_equal(all_read, ui_rect_intersection(all_read, group)),
          "inbox scene: channel mark-read check buttons appear on row hover or keyboard focus only; servers keep "
          "Mark all read");
    check(paint_stream_reset(&paint, 1280, 800) && ui_inbox_scene_paint(scene, kDefaultTheme, &paint) &&
              ui_text_command_stream_initialize(&text, {commands, 256}) &&
              ui_inbox_scene_text_output(scene, kDefaultTheme, &output),
          "inbox scene: Unreads paints");
    source.count = 0;
    ++source.revision;
    ++content.revision;
    bool caught_up = false;
    if (ui_inbox_scene_update(&scene, &source, &content, viewport) &&
        ui_text_command_stream_initialize(&text, {commands, 256}) &&
        ui_inbox_scene_text_output(scene, kDefaultTheme, &output))
        for (u32 i = 0; i < text.count; ++i)
            caught_up = caught_up || utf8_equal(text.storage[i].text, utf8_literal("You're all caught up"));
    check(caught_up, "inbox scene: an empty Unreads tab says the account is caught up");
    f.current = false;
    check(!ui_inbox_scene_current(scene) && ui_inbox_scene_hit_test(scene, 900, 300).action == UiInboxAction::none,
          "inbox scene: a stale source disables input");
    paint_stream_release(&paint);
    ui_inbox_scene_release(&scene);
    check(!scene.active && !scene.storage.arena.base, "inbox scene: release frees widget storage");
}
} // namespace maicord
