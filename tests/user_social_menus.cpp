#include "maicord/ui/user_context_menu.hpp"
#include "maicord/ui/message_context_menu.hpp"
#include "maicord/model/user_note_store.hpp"
#include "maicord/model/active_now.hpp"
#include "maicord/ui/friends_scene.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool has(ConstSlice<PopupItem> items, UserContextAction action, u16 flags = popup_item_enabled) noexcept {
    for (u32 i = 0; i < items.count; ++i)
        if (items[i].action == static_cast<u32>(action))
            return (items[i].flags & flags) == flags;
    return false;
}
u32 index_of(ConstSlice<PopupItem> items, u32 action) noexcept {
    for (u32 i = 0; i < items.count; ++i)
        if (items[i].action == action)
            return i;
    return ~0u;
}
} // namespace

void test_user_context_menu(void (*check)(bool, const char*) noexcept) noexcept {
    using A = UserContextAction;
    PopupItem items[kUserContextMaximumItems]{};
    UserContextFacts other{};
    other.relation = UserContextRelation::none;
    other.can_mention = other.can_message = other.relations_known = other.can_invite = true;
    other.can_edit_roles = other.has_apps = other.can_report = other.developer_mode = true;
    u32 count = user_context_menu_build(other, {items, kUserContextMaximumItems});
    ConstSlice<PopupItem> view{items, count};
    check(count == 11 && items[0].action == static_cast<u32>(A::profile) &&
              items[1].action == static_cast<u32>(A::mention) && items[2].action == static_cast<u32>(A::message) &&
              items[3].action == static_cast<u32>(A::add_note) && utf8_equal(items[3].label, utf8_literal("Add note")) &&
              items[4].action == static_cast<u32>(A::apps_menu) && (items[4].flags & popup_item_separator_before) &&
              items[5].action == static_cast<u32>(A::invite_menu) && has(view, A::add_friend) &&
              has(view, A::block, popup_item_danger) && has(view, A::report, popup_item_danger) &&
              items[9].action == static_cast<u32>(A::roles_menu) && (items[9].flags & popup_item_separator_before) &&
              items[10].action == static_cast<u32>(A::copy_id) && (items[10].flags & popup_item_separator_before),
          "user menu: a stranger gets profile, mention, message, note, apps, invite, friend, block, report, roles, copy");
    check(ui_menu_row_icon(items[4].presentation) == UiIcon::chevron_right && items[4].presentation.trailing_icon &&
              ui_menu_row_icon(items[5].presentation) == UiIcon::chevron_right &&
              ui_menu_row_icon(items[9].presentation) == UiIcon::chevron_right,
          "user menu: submenus carry a trailing chevron");
    PopupItem leading{};
    leading.presentation = ui_menu_row_presentation(UiIcon::copy);
    check(popup_item_menu_row_height(items[4], true) == popup_item_menu_row_height(items[0], true) &&
              popup_item_menu_row_height(items[4], true) == kUiMenuCompactRowHeight &&
              popup_item_menu_row_height(leading, true) == kUiMenuRowHeight,
          "user menu: a submenu chevron keeps the compact row rhythm; only leading icons make rows taller");
    {
        // In a menu with an icon column, a submenu row's label starts in that column.
        static PopupItem storage[24]{};
        static PopupRowWidget rows[24]{};
        static UiWidget widgets[2 + 24 * kPopupWidgetsPerItem]{};
        static UiLayoutNode nodes[2 + 24 * kPopupWidgetsPerItem]{};
        PopupScene menu{};
        MessageRecord record{};
        record.id = 77;
        record.channel_id = 7;
        record.author_id = 9;
        record.delivery = MessageDeliveryState::authoritative;
        MessageContextMenuModel model{};
        const bool built = message_context_menu_build(&model, record, false, true, false, nullptr, true, false, true,
                                                      false, false, false, false, {true, false, false, false})
                               .ok();
        const auto menu_items = message_context_menu_items(model);
        const u32 apps = index_of(menu_items, static_cast<u32>(MessageContextAction::apps_menu));
        const u32 copy = index_of(menu_items, static_cast<u32>(MessageContextAction::copy_text));
        UiMenuRowGeometry apps_row{}, copy_row{};
        const bool opened =
            built && apps != ~0u && copy != ~0u &&
            popup_scene_initialize(&menu, {storage, 24}, {rows, 24}, {widgets, 2 + 24 * kPopupWidgetsPerItem},
                                   {nodes, 2 + 24 * kPopupWidgetsPerItem}) &&
            popup_scene_open(&menu, PopupKind::context_menu, 77, menu_items, {100, 100, 101, 101}, {0, 0, 800, 900},
                             280)
                .ok() &&
            popup_scene_menu_row_geometry(menu, apps, &apps_row) && popup_scene_menu_row_geometry(menu, copy, &copy_row);
        check(opened && apps_row.primary.left == copy_row.primary.left && ui_rect_has_area(apps_row.icon) &&
                  apps_row.icon.left > apps_row.primary.left,
              "message menu: Apps keeps its label in the icon column with the chevron at the end");
        // Mention autocomplete: member rows (avatars) and @everyone / role rows
        // (leading @) share one row height and one label column.
        PopupItem mentions[3]{};
        mentions[0] = {utf8_literal("Probe Bot"), 1, static_cast<u16>(popup_item_enabled | popup_item_avatar)};
        mentions[0].detail = utf8_literal("probe.bot");
        mentions[0].media_id = 7;
        mentions[1] = {utf8_literal("@everyone"), 2, popup_item_enabled};
        mentions[1].presentation = ui_menu_row_presentation(UiIcon::mention);
        mentions[1].detail = utf8_literal("Notify everyone who has permission to view this channel.");
        mentions[2] = {utf8_literal("@Moderators"), 3, popup_item_enabled};
        mentions[2].presentation = ui_menu_row_presentation(UiIcon::mention);
        UiMenuRowGeometry member{}, everyone{}, role{};
        PopupItemLayout member_layout{}, everyone_layout{};
        const bool listed =
            popup_scene_open(&menu, PopupKind::autocomplete, 7, {mentions, 3}, {100, 600, 500, 640}, {0, 0, 800, 900},
                             400)
                .ok() &&
            popup_scene_menu_row_geometry(menu, 0, &member) && popup_scene_menu_row_geometry(menu, 1, &everyone) &&
            popup_scene_menu_row_geometry(menu, 2, &role) && popup_scene_resolve_item_layout(menu, 0, &member_layout) &&
            popup_scene_resolve_item_layout(menu, 1, &everyone_layout);
        check(listed && member.primary.left == everyone.primary.left && everyone.primary.left == role.primary.left &&
                  ui_rect_height(member_layout.row->rectangle) == ui_rect_height(everyone_layout.row->rectangle) &&
                  everyone.icon.right <= everyone.primary.left,
              "mention autocomplete: member, global and role rows share height and label column");
        popup_scene_release(&menu);
    }

    UserContextFacts self = other;
    self.self = true;
    count = user_context_menu_build(self, {items, kUserContextMaximumItems});
    view = {items, count};
    check(!has(view, A::message) && !has(view, A::add_note) && !has(view, A::invite_menu) && !has(view, A::block) &&
              !has(view, A::add_friend) && !has(view, A::report) && !has(view, A::apps_menu) &&
              has(view, A::profile) && has(view, A::mention) && has(view, A::roles_menu) && has(view, A::copy_id),
          "user menu: your own entry keeps profile, mention, roles and copy only");

    UserContextFacts blocked = other;
    blocked.relation = UserContextRelation::blocked;
    blocked.can_call = true;
    count = user_context_menu_build(blocked, {items, kUserContextMaximumItems});
    view = {items, count};
    check(has(view, A::unblock) && !has(view, A::block) && !has(view, A::message) && !has(view, A::call) &&
              !has(view, A::invite_menu) && !has(view, A::add_friend),
          "user menu: blocked users offer Unblock and no messaging, calls or invites");

    UserContextFacts friend_ = other;
    friend_.relation = UserContextRelation::friend_;
    friend_.can_call = true;
    friend_.has_note = true;
    friend_.developer_mode = false;
    count = user_context_menu_build(friend_, {items, kUserContextMaximumItems});
    view = {items, count};
    check(has(view, A::remove_friend, popup_item_danger) && has(view, A::call) && !has(view, A::copy_id) &&
              index_of(view, static_cast<u32>(A::call)) == 3 &&
              utf8_equal(items[index_of(view, static_cast<u32>(A::add_note))].label, utf8_literal("Edit note")),
          "user menu: friends offer Call after Message, Remove friend and Edit note; Copy user ID needs Developer Mode");

    UserContextFacts incoming = other, outgoing = other, bot = other, unknown = other;
    incoming.relation = UserContextRelation::incoming;
    outgoing.relation = UserContextRelation::outgoing;
    bot.bot = true;
    bot.can_call = true;
    unknown.relations_known = false;
    count = user_context_menu_build(incoming, {items, kUserContextMaximumItems});
    check(has({items, count}, A::accept_friend), "user menu: an incoming request can be accepted");
    count = user_context_menu_build(outgoing, {items, kUserContextMaximumItems});
    check(has({items, count}, A::cancel_request), "user menu: an outgoing request can be cancelled");
    count = user_context_menu_build(bot, {items, kUserContextMaximumItems});
    view = {items, count};
    check(!has(view, A::invite_menu) && !has(view, A::add_friend) && !has(view, A::block) && !has(view, A::call) &&
              has(view, A::message) && has(view, A::apps_menu),
          "user menu: bots get no invite, friend, block or call rows");
    count = user_context_menu_build(unknown, {items, kUserContextMaximumItems});
    view = {items, count};
    check(!has(view, A::add_friend) && !has(view, A::block) && has(view, A::report),
          "user menu: friend and block rows wait for the relationship store");
    PopupItem small[3]{};
    small[0].action = 77;
    check(user_context_menu_build(other, {small, 3}) == 0 && small[0].action == 77,
          "user menu: too little room leaves the caller's rows untouched");

    const UserContextRole roles[]{{utf8_literal("Moderator"), 0x3498DB, true, true},
                                  {utf8_literal("Owner"), 0, false, false}};
    PopupItem role_items[2]{};
    check(user_context_roles_build({roles, 2}, {role_items, 2}) == 2 &&
              role_items[0].action == static_cast<u32>(A::role_first) &&
              role_items[0].flags == (popup_item_enabled | popup_item_checked) &&
              role_items[0].media_flags == kPopupItemMediaIconTint && role_items[0].media_variant == 0x3498DB &&
              role_items[1].action == static_cast<u32>(A::role_first) + 1 && role_items[1].flags == 0,
          "user menu: roles are checkbox rows in the role colour; roles above yours are disabled");
    const UserContextChoice guilds[]{{utf8_literal("Gaming"), {}, false}, {utf8_literal("Study"), {}, true}};
    PopupItem guild_items[2]{};
    check(user_context_invites_build({guilds, 2}, {guild_items, 2}) == 2 &&
              (guild_items[0].flags & popup_item_enabled) && !(guild_items[1].flags & popup_item_enabled) &&
              utf8_equal(guild_items[1].label, utf8_literal("Invite sent")) &&
              utf8_equal(guild_items[1].detail, utf8_literal("Study")),
          "user menu: a sent invite stays listed and disabled");
    PopupItem app_items[2]{};
    check(user_context_apps_build({}, true, {app_items, 2}) == 1 && !(app_items[0].flags & popup_item_enabled) &&
              utf8_equal(app_items[0].label, utf8_literal("Loading apps\xE2\x80\xA6")),
          "user menu: Apps shows a disabled loading row before commands arrive");
    check(user_context_action(static_cast<u32>(A::app_first) + 3) && !user_context_action(kUserContextActionBase) &&
              !user_context_action(static_cast<u32>(A::last) + 1),
          "user menu: the action range is closed");
}

void test_message_context_extras(void (*check)(bool, const char*) noexcept) noexcept {
    MessageRecord message{};
    message.id = 10;
    message.channel_id = 20;
    message.author_id = 30;
    ReactionPreferenceStore preferences{};
    reaction_preferences_initialize(&preferences);
    MessageContextMenuModel model{};
    check(message_context_menu_build(&model, message, false, false, false, &preferences).ok() &&
              index_of({model.items, model.count}, static_cast<u32>(MessageContextAction::apps_menu)) == ~0u &&
              index_of({model.items, model.count}, static_cast<u32>(MessageContextAction::report_message)) == ~0u,
          "message menu: extras are absent by default");
    MessageContextMenuExtras extras{true, true, true, true};
    check(message_context_menu_build(&model, message, true, true, true, &preferences, true, true, true, true, false,
                                     true, true, extras)
                  .ok() &&
              model.count == kMessageContextMaximumItems,
          "message menu: every row at once fits the menu");
    const ConstSlice<PopupItem> view{model.items, model.count};
    const u32 apps = index_of(view, static_cast<u32>(MessageContextAction::apps_menu));
    const u32 embeds = index_of(view, static_cast<u32>(MessageContextAction::remove_embeds));
    const u32 speak = index_of(view, static_cast<u32>(MessageContextAction::speak_message));
    const u32 report = index_of(view, static_cast<u32>(MessageContextAction::report_message));
    const u32 copy_text = index_of(view, static_cast<u32>(MessageContextAction::copy_text));
    const u32 del = index_of(view, static_cast<u32>(MessageContextAction::delete_message));
    check(apps != ~0u && embeds < apps && apps < copy_text && speak > copy_text && report == del + 1 &&
              (model.items[report].flags & popup_item_danger) && !(model.items[report].flags & popup_item_separator_before) &&
              model.items[apps].presentation.trailing_icon &&
              ui_menu_row_icon(model.items[apps].presentation) == UiIcon::chevron_right,
          "message menu: Remove embeds and Apps join the actions, Speak follows copying, Report follows Delete");
    message.delivery = MessageDeliveryState::pending;
    check(message_context_menu_build(&model, message, true, true, true, &preferences, true, true, true, true, false,
                                     true, true, extras)
                  .ok() &&
              model.count == 1,
          "message menu: a pending message offers only Copy text");
}

void test_user_note_store(void (*check)(bool, const char*) noexcept) noexcept {
    static UserNoteStore store{};
    user_note_reset(&store);
    check(user_note_request(&store, 7) && user_note_next(store, 0).work == UserNoteWork::load &&
              user_note_next(store, 0).user_id == 7,
          "notes: showing a profile schedules one load");
    user_note_load_begin(&store, 7);
    check(user_note_next(store, 0).work == UserNoteWork::none, "notes: a load in flight is not repeated");
    user_note_loaded(&store, 7, true, utf8_literal("old"));
    check(utf8_equal(user_note_text(store, 7), utf8_literal("old")), "notes: the loaded note is shown");

    check(user_note_edit(&store, 7, utf8_literal("n"), 1000), "notes: typing edits the draft");
    check(user_note_edit(&store, 7, utf8_literal("ne"), 1300) && user_note_edit(&store, 7, utf8_literal("new"), 1600),
          "notes: further keystrokes replace the draft");
    auto next = user_note_next(store, 2000);
    check(next.work == UserNoteWork::none && next.due_ms == 1600 + kUserNoteSaveDelayMs &&
              utf8_equal(user_note_text(store, 7), utf8_literal("new")),
          "notes: the save waits for the typing pause after the last keystroke");
    next = user_note_next(store, 1600 + kUserNoteSaveDelayMs);
    Utf8View sent{};
    check(next.work == UserNoteWork::save && next.user_id == 7 && user_note_save_begin(&store, 7, &sent) &&
              utf8_equal(sent, utf8_literal("new")) && user_note_next(store, 5000).work == UserNoteWork::none,
          "notes: one debounced save carries the final draft");
    check(user_note_edit(&store, 7, utf8_literal("newer"), 2500), "notes: typing continues during the save");
    user_note_save_finished(&store, 7, true, 2600);
    const auto* entry = user_note_find(store, 7);
    check(entry && entry->dirty && utf8_equal({entry->text, entry->bytes}, utf8_literal("new")) &&
              utf8_equal(user_note_text(store, 7), utf8_literal("newer")) &&
              user_note_next(store, 2500 + kUserNoteSaveDelayMs).work == UserNoteWork::save,
          "notes: a completed save keeps the newer draft and saves it next");
    user_note_gateway(&store, 7, utf8_literal("remote"));
    check(utf8_equal(user_note_text(store, 7), utf8_literal("newer")),
          "notes: a Gateway update never overwrites unsaved typing");
    check(user_note_save_begin(&store, 7, &sent), "notes: second save starts");
    user_note_save_finished(&store, 7, false, 4000);
    next = user_note_next(store, 4000);
    check(next.work == UserNoteWork::none && next.due_ms == 4000 + kUserNoteRetryDelayMs,
          "notes: a failed save retries after a delay");
    user_note_save_begin(&store, 7, &sent);
    user_note_save_finished(&store, 7, false, 9000);
    user_note_save_begin(&store, 7, &sent);
    user_note_save_finished(&store, 7, false, 15000);
    entry = user_note_find(store, 7);
    check(entry && entry->save_failed && user_note_next(store, 100000).work == UserNoteWork::none,
          "notes: saving stops after three failures");
    check(user_note_edit(&store, 7, utf8_literal("remote"), 20000) && !user_note_find(store, 7)->dirty,
          "notes: typing back to the saved value cancels the pending save");
    user_note_gateway(&store, 7, utf8_literal("from phone"));
    check(utf8_equal(user_note_text(store, 7), utf8_literal("from phone")),
          "notes: a Gateway update replaces a settled note");
    check(user_note_edit(&store, 8, utf8_literal("x"), 0) && user_note_edit(&store, 8, utf8_literal("xy"), 100),
          "notes: a second user's draft");
    user_note_flush(&store, 8, 200);
    next = user_note_next(store, 200);
    check(next.work == UserNoteWork::save && next.user_id == 8, "notes: closing the card saves at once");
    char long_note[kUserNoteMaximumBytes + 4];
    for (auto& c : long_note)
        c = 'a';
    check(!user_note_edit(&store, 8, {long_note, kUserNoteMaximumCodepoints + 1}, 300),
          "notes: drafts longer than 256 code points are refused");
}

namespace {
struct ActivityFixture {
    UserActivitySet sets[4];
    u32 count;
};
bool lookup(const void* context, u64 user, UserActivitySet* out) noexcept {
    const auto& f = *static_cast<const ActivityFixture*>(context);
    for (u32 i = 0; i < f.count; ++i)
        if (f.sets[i].user_id == user) {
            *out = f.sets[i];
            return true;
        }
    return false;
}
void playing(UserActivitySet* set, u64 user, u64 application, Utf8View name, u64 start) noexcept {
    clear_bytes(set, static_cast<u32>(sizeof(*set)));
    set->user_id = user;
    set->count = 1;
    set->items[0].type = UserActivityType::playing;
    set->items[0].application_id = application;
    set->items[0].start_ms = start;
    copy_bytes(set->items[0].text, name.data, name.bytes);
    set->items[0].lengths[static_cast<u32>(UserActivityField::name)] = static_cast<u16>(name.bytes);
}
} // namespace

void test_active_now(void (*check)(bool, const char*) noexcept) noexcept {
    static ActivityFixture fixture{};
    fixture.count = 4;
    playing(&fixture.sets[0], 1, 500, utf8_literal("Valorant"), 3000);
    playing(&fixture.sets[1], 2, 500, utf8_literal("Valorant"), 1000);
    playing(&fixture.sets[2], 3, 0, utf8_literal("Minecraft"), 500);
    playing(&fixture.sets[3], 5, 0, utf8_literal("Chess"), 2000);
    static VoiceState records[4]{};
    VoiceStateStore voice{};
    voice_state_store_initialize(&voice, {records, 4});
    voice_state_store_apply(&voice, 9, 90, 5, 0);
    voice_state_store_apply(&voice, 9, 90, 6, 0);
    voice_state_store_apply(&voice, 9, 91, 99, 0); // not a friend
    const u64 friends[]{1, 2, 3, 4, 5, 6, 1};
    ActiveNowCard cards[kActiveNowMaximumCards]{};
    const ActiveNowInput input{{friends, 7}, &voice, &fixture, lookup};
    const u32 count = active_now_project(input, {cards, kActiveNowMaximumCards});
    check(count == 3, "active now: two activity cards and one voice card; idle friends are left out");
    check(cards[0].kind == ActiveNowKind::voice && cards[0].channel_id == 90 && cards[0].guild_id == 9 &&
              cards[0].total == 2 && cards[0].members[0] == 5 && cards[0].members[1] == 6 &&
              utf8_equal({cards[0].name, cards[0].name_bytes}, utf8_literal("Chess")),
          "active now: friends in one voice channel share a card subtitled with a member's game");
    check(cards[1].kind == ActiveNowKind::activity && cards[1].application_id == 500 && cards[1].total == 2 &&
              cards[1].started_ms == 1000 && utf8_equal({cards[1].name, cards[1].name_bytes}, utf8_literal("Valorant")),
          "active now: friends playing the same application share a card started at the earliest member");
    check(cards[2].kind == ActiveNowKind::activity && cards[2].total == 1 && cards[2].members[0] == 3,
          "active now: single players follow larger groups; a duplicate friend counts once");
    ActiveNowCard again[kActiveNowMaximumCards]{};
    const u32 repeated = active_now_project(input, {again, kActiveNowMaximumCards});
    check(active_now_equal({cards, count}, {again, repeated}), "active now: projection is deterministic");
    ActiveNowCard one[1]{};
    check(active_now_project(input, {one, 1}) == 1 && one[0].kind == ActiveNowKind::voice,
          "active now: output capacity keeps the highest-ranked cards");
    const ActiveNowInput nobody{{}, &voice, &fixture, lookup};
    check(active_now_project(nobody, {cards, kActiveNowMaximumCards}) == 0, "active now: no friends, no cards");
}
namespace {
bool active_resolve(void*, u32 index, UiFriendsRow* out) noexcept {
    *out = {index + 1u, utf8_literal("Friend"), utf8_literal("friend"), {}, UiPresence::online,
            UiFriendsKind::friend_user, false};
    return index < 3;
}
} // namespace

void test_friends_active_now(void (*check)(bool, const char*) noexcept) noexcept {
    UiFriendsScene scene{};
    UiFriendsSource source{nullptr, 3, 1, active_resolve, nullptr};
    UiFriendsActiveCard cards[2]{};
    cards[0] = {utf8_literal("Playing Valorant"), utf8_literal("Ada and Bo"), {}, {11, 12}, 2, 2,
                UiFriendsActiveKind::activity};
    cards[1] = {utf8_literal("General"), utf8_literal("Cy \xC2\xB7 Server"), {}, {13}, 1, 1, UiFriendsActiveKind::voice};
    UiFriendsActiveNow active{cards, 2, 1, false};
    const UiRect wide{0, 0, 1100, 700}, narrow{0, 0, 700, 700};
    check(ui_friends_scene_update(&scene, &source, UiFriendsTab::online, UiFriendsPhase::ready, {}, wide, nullptr,
                                  nullptr, &active) &&
              scene.active_visible && scene.surface.right == wide.right - kUiFriendsActiveNowWidth &&
              scene.active_rect.left == scene.surface.right,
          "active now: a wide window shows the column beside a narrower list");
    const auto card = ui_friends_scene_hit_test(scene, scene.active_rect.left + 40, scene.active_rect.top + 100);
    UiFriendsTarget checked{};
    check(card.action == UiFriendsAction::active_card && card.user_id == 11 &&
              ui_friends_scene_activation(scene, card, &checked),
          "active now: a card opens its first member");
    check(ui_friends_scene_active_avatar_count(scene) == 3, "active now: each card's members get avatars");
    UiFriendsActiveAvatar avatar{};
    check(ui_friends_scene_active_avatar(scene, 2, &avatar) && avatar.user_id == 13 &&
              ui_rect_has_area(avatar.rectangle),
          "active now: avatars follow the cards in order");
    const auto toggle = ui_friends_scene_hit_test(scene, scene.active_rect.right - 30, scene.active_rect.top + 28);
    check(toggle.action == UiFriendsAction::active_toggle && ui_friends_scene_activation(scene, toggle, &checked),
          "active now: the heading has a collapse button");
    check(ui_friends_scene_active_hover(&scene, scene.active_rect.left + 40, scene.active_rect.top + 100) &&
              scene.active_hover == 1 && !ui_friends_scene_active_hover(&scene, scene.active_rect.left + 40,
                                                                        scene.active_rect.top + 101),
          "active now: hover tracks the card under the pointer");
    UiTextCommand commands[64]{};
    UiTextCommandStream stream{};
    bool heading = false;
    if (ui_text_command_stream_initialize(&stream, {commands, 64}) && ui_friends_scene_text(scene, kDefaultTheme, &stream))
        for (u32 i = 0; i < stream.count; ++i)
            heading = heading || utf8_equal(commands[i].text, utf8_literal("Active now"));
    check(heading, "active now: the column is titled");
    active.collapsed = true;
    ++active.revision;
    check(ui_friends_scene_update(&scene, &source, UiFriendsTab::online, UiFriendsPhase::ready, {}, wide, nullptr,
                                  nullptr, &active) &&
              scene.active_collapsed && scene.surface.right == wide.right - kUiFriendsActiveNowCollapsedWidth &&
              ui_friends_scene_active_avatar_count(scene) == 0,
          "active now: collapsing leaves a slim strip and gives the list its width back");
    active.collapsed = false;
    ++active.revision;
    check(ui_friends_scene_update(&scene, &source, UiFriendsTab::online, UiFriendsPhase::ready, {}, narrow, nullptr,
                                  nullptr, &active) &&
              !scene.active_visible && scene.surface.right == narrow.right &&
              ui_friends_scene_hit_test(scene, 690, 100).action != UiFriendsAction::active_card,
          "active now: narrow windows hide the column");
    ui_friends_scene_release(&scene);
}
} // namespace maicord
