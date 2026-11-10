// The DM view's chrome: the side slot plan (DM profile vs member list), the
// shell layout around it, the title bar centre per view, the DM header's
// activity line, the typing dots' clock, and the DM profile panel's runtime
// (fetch dedupe, caching, rate limits, toggles, card geometry, activity art,
// timers, failures). Synthetic fixtures only.
#include "account/dm_profile.hpp"
#include "../../support/model_store_fixtures.hpp"
#include "../../support/account_module_fake.hpp"
#include "chat/composer/commands.hpp"
#include "account/profile.hpp"
#include "shell/state.hpp"
#include "account/friends.hpp"
#include "guild/card.hpp"
#include "chat/search/search.hpp"
#include "account/user_actions.hpp"
#include "voice/voice.hpp"
#include "shell/content_input.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/user/private_channel_store.hpp"
#include "maicord/model/user/relationship_store.hpp"
#include "maicord/protocol/gateway/client_status.hpp"
#include "maicord/ui/widgets/presence.hpp"
#include "maicord/ui/widgets/activity_status.hpp"
#include "maicord/ui/shell/caption_bar.hpp"
#include "maicord/ui/account/dm_profile_panel.hpp"
#include "maicord/ui/core/paint.hpp"
#include "maicord/ui/account/profile_media.hpp"
#include "maicord/ui/chat/private_channel_rows.hpp"
#include "maicord/ui/chat/names_card.hpp"
#include "chat/private_header.hpp"
#include "maicord/ui/shell/popup_scene.hpp"
#include "maicord/ui/accessibility/accessibility.hpp"
#include "maicord/ui/core/scene.hpp"
#include "maicord/ui/text/measure_cache.hpp"
#include "maicord/ui/chat/messages/typing_indicator.hpp"
#include "../../../src/app/account/account_ui_runtime.hpp"
#include "../../support/fake_text_measure.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::dm_profile_test {
using namespace app_detail;
u32 failures, reads, full_opens, menus;
// Shell text rebuilds and the panel text runs the last one emitted.
u32 text_builds, text_runs;
ProfileTab expanded_tab;
u32 view_full_opens;
u64 view_full_user;
UiRect view_full_anchor;
// The command index the Commands section reads, and the last insertion.
u32 command_revision = 1;
u64 inserted_application, inserted_command, asked_channel;
// The index reports itself still loading (no commands yet).
bool commands_loading;
char inserted_name[32];
u32 inserted_name_bytes;
u64 read_user, read_guild, request_id;
u8 read_operation;
char read_value[256];
u32 read_value_bytes;
RestClientSubmitError admission;

void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
bool near(float a, float b) noexcept {
    return a - b < .01f && b - a < .01f;
}
RestClientSubmitResult WINAPI read_profile(void*, u64 user, u64 guild, u64 id, u8 operation, Utf8View value) noexcept {
    if (admission == RestClientSubmitError::none) {
        ++reads;
        read_user = user;
        read_guild = guild;
        read_operation = operation;
        request_id = id;
        read_value_bytes = minimum<u32>(value.bytes, sizeof(read_value));
        copy_bytes(read_value, value.data, read_value_bytes);
    }
    return {admission};
}

UiShellLayoutMetrics shell_metrics(const UiSidePanelInput& in, float window) noexcept {
    UiShellLayoutMetrics metrics = kGuildChannelShellLayoutMetrics;
    metrics.flags = static_cast<u16>(metrics.flags | ui_shell_layout_private);
    const auto plan = ui_side_panel_plan(in);
    metrics.member_sidebar_width = plan.width;
    if (plan.panel == UiSidePanel::none)
        metrics.flags = static_cast<u16>(metrics.flags & ~static_cast<u16>(ui_shell_layout_members));
    metrics.revision = 1000 + static_cast<u32>(plan.width) + static_cast<u32>(window) * 7 + metrics.flags;
    return metrics;
}

void side_panel_cases() noexcept {
    UiSidePanelInput dm{1, true, false, true, true, 0, 1280};
    auto plan = ui_side_panel_plan(dm);
    check(plan.panel == UiSidePanel::profile && plan.width == kUiDmProfilePanelWidth &&
              !plan.unavailable,
          "side panel: a one-to-one DM shows the recipient's 316 DIP profile slot");
    check(utf8_equal(ui_side_panel_toggle_label(true, false, true), utf8_literal("Hide User Profile")) &&
              utf8_equal(ui_side_panel_toggle_label(true, false, false), utf8_literal("Show User Profile")) &&
              utf8_equal(ui_side_panel_toggle_label(true, true, false),
                         utf8_literal("Show User Profile (Unavailable)")) &&
              utf8_equal(ui_side_panel_toggle_label(false, false, true), utf8_literal("Hide member list")),
          "side panel: the header toggle names what it switches");
    dm.profile_visible = false;
    plan = ui_side_panel_plan(dm);
    check(plan.panel == UiSidePanel::none && plan.width == kUiDmProfilePanelWidth,
          "side panel: the hidden profile keeps its toggle");
    dm.profile_visible = true;
    dm.members_visible = false;
    check(ui_side_panel_plan(dm).panel == UiSidePanel::profile,
          "side panel: the profile choice is independent of the member list's");
    dm.window_width = kUiDmProfilePanelMinimumWindowWidth - 1;
    plan = ui_side_panel_plan(dm);
    check(plan.panel == UiSidePanel::none && plan.unavailable && plan.width > 0,
          "side panel: below 1132 DIPs the profile gives way and its toggle is unavailable");
    const UiSidePanelInput group{3, true, false, true, true, 0, 1280};
    const UiSidePanelInput server{0xFE, false, false, false, true, 260, 1280};
    const UiSidePanelInput friends{0, true, true, true, true, 0, 1280};
    const UiSidePanelInput empty_home{0, true, false, true, true, 0, 1280};
    check(ui_side_panel_plan(group).panel == UiSidePanel::members &&
              ui_side_panel_plan(server).panel == UiSidePanel::none && ui_side_panel_plan(server).width == 260 &&
              !ui_side_panel_plan(friends).width && !ui_side_panel_plan(empty_home).width,
          "side panel: group DMs and servers keep the member list; Friends and an empty Home have no toggle");
}

void layout_cases() noexcept {
    static UiScene scene{};
    if (!ui_scene_initialize_bootstrap(&scene)) {
        check(false, "side panel layout: shell scene");
        return;
    }
    const auto measure = [&](const UiSidePanelInput& in, float window, UiRect* chat, UiRect* slot) noexcept {
        if (!ui_scene_layout_with_metrics(&scene, UiShellView::private_channels, shell_metrics(in, window), window,
                                          800))
            return false;
        const auto* messages = ui_scene_node(scene, UiSemantic::message_view);
        const auto* members = ui_scene_node(scene, UiSemantic::member_sidebar);
        *chat = messages ? messages->rectangle : UiRect{};
        *slot = members ? members->rectangle : UiRect{};
        return messages != nullptr;
    };
    UiSidePanelInput dm{1, true, false, false, true, 0, 1280};
    UiRect chat{}, slot{}, wide_chat{}, none{};
    const bool shown = measure(dm, 1280, &chat, &slot);
    dm.profile_visible = false;
    const bool hidden = measure(dm, 1280, &wide_chat, &none);
    check(shown && hidden && near(ui_rect_width(slot), kUiDmProfilePanelWidth) && near(slot.right, 1280) &&
              !ui_rect_has_area(none) && chat.right <= slot.left &&
              near(ui_rect_width(wide_chat) - ui_rect_width(chat), kUiDmProfilePanelWidth),
          "side panel layout: the profile takes its slot at the right and the chat narrows by exactly that");
    check(ui_scene_node(scene, UiSemantic::member_toggle) != nullptr,
          "side panel layout: the hidden profile keeps its header toggle");
    dm.profile_visible = true;
    dm.window_width = 1131;
    const bool narrow = measure(dm, 1131, &chat, &slot);
    check(narrow && !ui_rect_has_area(slot) && ui_scene_node(scene, UiSemantic::member_toggle),
          "side panel layout: a narrower window drops the panel and keeps the toggle");
    // A wide channel list leaves less than a readable chat: the shell drops it too.
    auto metrics = shell_metrics({1, true, false, false, true, 0, 1200}, 1200);
    metrics.navigation_width = 600;
    const bool squeezed = ui_scene_layout_with_metrics(&scene, UiShellView::private_channels, metrics, 1200, 800);
    const auto* members = ui_scene_node(scene, UiSemantic::member_sidebar);
    const auto* messages = ui_scene_node(scene, UiSemantic::message_view);
    check(squeezed && (!members || !ui_rect_has_area(members->rectangle)) && messages &&
              ui_rect_width(messages->rectangle) >= kUiShellMinimumMessageWidthDips,
          "side panel layout: the chat keeps its minimum width before the panel");
    ui_scene_release(&scene);
}

void fetch_policy_cases() noexcept {
    check(ui_profile_fetch_due(0, 0, 5) && !ui_profile_fetch_due(1'000, 0, 60'999) &&
              ui_profile_fetch_due(1'000, 0, 61'000) && !ui_profile_fetch_due(0, 2'000, 30'000) &&
              ui_profile_fetch_due(0, 2'000, 62'000) && !ui_profile_fetch_due(1'000, 50'000, 70'000),
          "profile fetch: due when never fetched or a minute after the last load or failure");
}

void caption_cases() noexcept {
    UiCaptionTitleInput in{};
    auto title = ui_caption_title(in);
    check(utf8_equal(title.text, utf8_literal("MaiCord")) && title.glyph == UiIcon::count,
          "title bar: signed out shows the app's name alone");
    in.signed_in = true;
    in.guild_id = 7;
    in.guild_icon_variant = 3;
    in.guild_name = utf8_literal("Native Lab");
    title = ui_caption_title(in);
    check(utf8_equal(title.text, utf8_literal("Native Lab")) && title.glyph == UiIcon::guild &&
              media_asset_key_equal(title.image, media_asset_guild_icon(7, 3, false)),
          "title bar: a server shows its icon and name");
    in.guild_icon_variant = 0;
    check(!media_asset_key_valid(ui_caption_title(in).image), "title bar: a server without an icon shows a glyph");
    in.home = true;
    in.private_type = 1;
    title = ui_caption_title(in);
    check(utf8_equal(title.text, utf8_literal("Direct Messages")) && title.glyph == UiIcon::direct_message,
          "title bar: a DM shows Direct Messages with the DM icon");
    in.private_type = 3;
    check(utf8_equal(ui_caption_title(in).text, utf8_literal("Direct Messages")),
          "title bar: a group DM shows Direct Messages too");
    in.private_type = 0;
    check(!ui_caption_title(in).text.bytes, "title bar: Home without a conversation (message requests) shows nothing");
    in.friends = true;
    title = ui_caption_title(in);
    check(utf8_equal(title.text, utf8_literal("Friends")) && title.glyph == UiIcon::members,
          "title bar: Friends shows Friends");
    const auto layout = ui_caption_bar_layout(kDefaultTheme, 1000, {}, 32);
    const float limit = ui_caption_title_text_limit(layout, true, kUiCaptionTitleGlyph);
    const auto placed = ui_caption_title_place(layout, 100, true, kUiCaptionTitleGlyph);
    const float group = kUiCaptionTitleGlyph + kUiCaptionTitleGap + 100;
    check(near(limit, 500 - kUiCaptionTitleGlyph - kUiCaptionTitleGap) &&
              near(placed.icon.left, (1000 - group) * .5f) &&
              near(placed.text_left, placed.icon.right + kUiCaptionTitleGap) &&
              near(ui_rect_height(placed.icon), kUiCaptionTitleGlyph) &&
              near((placed.icon.top + placed.icon.bottom) * .5f, 16),
          "title bar: icon and text centre as one group, 8 apart, within half the bar");
    const auto plain = ui_caption_title_place(layout, 100, false, 0);
    check(near(plain.text_left, 450) && !ui_rect_has_area(plain.icon), "title bar: a title alone stays centred");
}

void typing_cases() noexcept {
    float scale = 0, opacity = 0;
    ui_typing_dot_pose(0, 0, &scale, &opacity);
    const bool rest = near(scale, .8f) && near(opacity, .3f);
    ui_typing_dot_pose(0, kUiTypingDotsPhases / 2, &scale, &opacity);
    const bool peak = near(scale, 1) && near(opacity, 1);
    ui_typing_dot_pose(1, 3 + kUiTypingDotsPhases / 2, &scale, &opacity);
    const bool lagged = near(scale, 1) && near(opacity, 1);
    ui_typing_dot_pose(2, kUiTypingDotsRest, &scale, &opacity);
    check(rest && peak && lagged && near(scale, 1) && near(opacity, 1),
          "typing dots: 80%/30% at rest, full at the pulse's middle, 150 ms apart; held full at rest");
    PaintQuad dots[kUiTypingDotCount];
    ui_typing_dots_quads({10, 0, 40, 20}, kDefaultTheme.text_primary, kUiTypingDotsRest, dots);
    check(near(dots[0].left, 10) && near(dots[1].right - dots[1].left, 7) &&
              near(dots[2].left - dots[1].left, 8.75f) && near(dots[2].top, 6.5f) &&
              near(kUiTypingLabelInset, 8 + 24.5f + 4),
          "typing dots: three 7 DIP dots 8.75 apart, the label 4 after them");
    PaintStream paint{};
    if (!paint_stream_initialize(&paint) || !paint_stream_reset(&paint, 100, 40)) {
        check(false, "typing dots: paint stream");
        return;
    }
    bool three = true, patched = true;
    for (u16 phase = 0; phase < kUiTypingDotsPhases; ++phase) {
        const u32 before = paint.count;
        three = three && ui_typing_dots_paint(&paint, {0, 0, 40, 20}, kDefaultTheme.text_primary, phase) &&
                paint.count == before + 3;
        PaintQuad quads[kUiTypingDotCount];
        ui_typing_dots_quads({0, 0, 40, 20}, kDefaultTheme.text_primary, phase, quads);
        patched = patched && three && bytes_equal(paint.quads + before, quads, sizeof(quads));
    }
    check(three, "typing dots: every phase paints the same three quads (damage stays on them)");
    check(patched, "typing dots: a step's patched quads are the ones a rebuild paints");
    paint_stream_release(&paint);
    // The host loop: wake at the clock's deadline, tick once a frame.
    const auto run = [](u64 from, u64 to, bool visible, bool animate, u32* frames, u32* changes) noexcept {
        UiTypingDotsClock clock{};
        *frames = *changes = 0;
        u64 now = from;
        if (ui_typing_dots_clock_tick(&clock, now, visible, animate))
            ++*changes;
        while (clock.wake_ms && clock.wake_ms <= to) {
            now = clock.wake_ms;
            ++*frames;
            if (ui_typing_dots_clock_due(clock, now) && ui_typing_dots_clock_tick(&clock, now, visible, animate))
                ++*changes;
        }
        return clock;
    };
    u32 frames = 0, changes = 0;
    run(1'000, 2'200, true, true, &frames, &changes);
    check(frames == kUiTypingDotsPhases && changes == kUiTypingDotsPhases + 1,
          "typing dots: one frame per 50 ms step while someone types and they show");
    const auto hidden = run(1'000, 60'000, false, true, &frames, &changes);
    check(!frames && !changes && !hidden.wake_ms, "typing dots: no frames while hidden or nobody types");
    const auto inactive = run(1'000, 60'000, true, false, &frames, &changes);
    check(!frames && changes == 1 && inactive.phase == kUiTypingDotsRest && !inactive.wake_ms,
          "typing dots: an inactive or minimized window holds them at rest without wakes");
}

// Fixture: Home with DM 9998 to user 123, an online friend listening to Spotify.
struct Fixture {
    GuildRecord guilds[2];
    u16 guild_order[2];
    ChannelRecord channels[8];
    u16 channel_order[8];
    u8 names[1024];
    UserRecord users[8];
    u32 user_order[8];
    u8 user_names[1024];
    PrivateChannelRecord private_records[4];
    u64 private_ids[8];
    RelationshipRecord relation_records[4];
    UiTextMeasureRecord measure_records[128];
    u32 structurals[512];
};
Fixture* g_fixture;
const RelationshipStore* g_relations;

bool fixture_setup() noexcept {
    g_fixture = static_cast<Fixture*>(VirtualAlloc(nullptr, sizeof(Fixture), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_fixture || !g_app.account_ui)
        return false;
    auto& f = *g_fixture;
    auto& ui = *g_app.account_ui;
    auto& state = ui.chat_state;
    static PrivateChannelStore private_store{};
    static RelationshipStore relations{};
    const u64 peer[] = {123}, other[] = {124};
    if (!chat_state_store_initialize(&state, {f.guilds, 2}, {f.guild_order, 2}, {f.channels, 8},
                                     {f.channel_order, 8}, {f.names, sizeof(f.names)}) ||
        !user_store_initialize(&ui.user_store, {f.users, 8}, {f.user_order, 8}, {f.user_names, sizeof(f.user_names)}) ||
        !private_channel_store_initialize(&private_store, {f.private_records, 4}, {f.private_ids, 8}) ||
        !private_channel_store_replace(&private_store, {9998, 0, {peer, 1}, {}, true, false, false}) ||
        !private_channel_store_replace(&private_store, {9996, 0, {other, 1}, {}, true, false, false}) ||
        !relationship_store_initialize(&relations, {f.relation_records, 4}) ||
        !ui_text_measure_cache_initialize(&ui.text_measure_cache, {f.measure_records, 128}, fake_text_measure<>, nullptr))
        return false;
    RelationshipRecord friend_record{};
    friend_record.user_id = 123;
    friend_record.type = RelationshipType::friend_;
    friend_record.presence = RelationshipPresence::online;
    state.private_channels = &private_store;
    state.self_user_id = 99;
    if (!relationship_store_upsert(&relations, friend_record).ok() ||
        !user_store_upsert(&ui.user_store, 123, utf8_literal("Mai"), utf8_literal("mai")).ok() ||
        !chat_state_store_upsert_channel(&state, {9998, 0, 0, {}, 0, 0, 1}).ok() ||
        !chat_state_store_upsert_channel(&state, {9996, 0, 0, {}, 0, 0, 1}).ok() ||
        !user_store_upsert(&ui.user_store, 124, utf8_literal("Ren"), utf8_literal("ren")).ok() ||
        !chat_state_store_upsert_channel(&state, {9997, 0, 0, utf8_literal("Crew"), 0, 0, 3}).ok() ||
        !chat_state_store_select_home(&state).ok() || !chat_state_store_select_channel(&state, 9998).ok())
        return false;
    state.home_selected = true;
    ui.active_channel_id = 9998;
    constexpr auto presence = utf8_literal(
        R"({"activities":[{"type":2,"name":"Spotify","details":"Stop Making Sense","state":"Panchiko","party":{"id":"spotify:1"}}]})");
    const auto scan = json_scan_structurals_avx1(presence, {f.structurals, 512});
    UserActivitySet set{};
    bool has = false;
    if (!scan.ok() ||
        !user_activities_decode({presence, {f.structurals, scan.count}}, {presence, 0, JsonValueKind::object}, 123,
                                &set, &has) ||
        !user_activity_upsert(&ui.user_store.activities, set))
        return false;
    g_relations = &relations;
    g_app.preferences.dm_profile_visible = true;
    g_app.renderer.width = 1280;
    g_app.renderer.dpi = 96;
    g_account_fake.profile_get = read_profile;
    return ui_scene_initialize_bootstrap(&g_app.shell_scene);
}

bool layout_shell() noexcept {
    const auto in = application_side_panel_input();
    g_app.shell_scene.side_panel_profile = in.home && in.channel_type == 1;
    auto metrics = shell_metrics(in, 1280);
    // The one-to-one DM header's identity button, as the shell builds it (account_presentation).
    auto& ui = *g_app.account_ui;
    const UiPrivateRowSource source{&ui.chat_state, &ui.user_store, g_relations, nullptr, &ui.text_measure_cache};
    metrics.private_identity_width = ui_private_header_identity_width(source, 9998);
    metrics.revision += 1 + static_cast<u32>(metrics.private_identity_width);
    return ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::private_channels, metrics, 1280, 800);
}

void header_cases() noexcept {
    auto& ui = *g_app.account_ui;
    const UiPrivateRowSource source{&ui.chat_state, &ui.user_store, g_relations, nullptr, &ui.text_measure_cache};
    char line[192];
    UiPrivateHeaderActivity activity{};
    const bool shown = layout_shell() &&
                       ui_private_header_activity(g_app.shell_scene, source, 9998, {line, sizeof(line)}, &activity);
    const auto* label = ui_scene_node(g_app.shell_scene, UiSemantic::title_label);
    // "Mai" is 24 DIPs wide after the 24 DIP avatar and its 8 DIP gap.
    const float name_end = label ? label->rectangle.left + kUiPrivateHeaderAvatar + 8 + 24 : 0;
    check(shown && activity.glyph == UiActivityGlyph::music && utf8_equal(activity.line, utf8_literal("Panchiko")) &&
              near(activity.icon.left, name_end + kUiPrivateHeaderActivityGap) &&
              near(activity.text.left, activity.icon.right + kUiActivityGlyphGap) &&
              near(activity.text.right, label->rectangle.right),
          "DM header: the peer's activity follows the name with its glyph (8 after, glyph then 4)");
    UiPrivateRowSource offline = source;
    RelationshipRecord gone{};
    gone.user_id = 123;
    gone.type = RelationshipType::friend_;
    gone.presence = RelationshipPresence::offline;
    static RelationshipRecord offline_records[2]{};
    static RelationshipStore offline_store{};
    const bool off = relationship_store_initialize(&offline_store, {offline_records, 2}) &&
                     relationship_store_upsert(&offline_store, gone).ok();
    offline.relationships = &offline_store;
    check(off && !ui_private_header_activity(g_app.shell_scene, offline, 9998, {line, sizeof(line)}, &activity),
          "DM header: an offline peer shows no activity");
    UiPrivateRowSource unmeasured = source;
    unmeasured.measure = nullptr;
    check(!ui_private_header_activity(g_app.shell_scene, unmeasured, 9998, {line, sizeof(line)}, &activity) &&
              !ui_private_header_activity(g_app.shell_scene, source, 9997, {line, sizeof(line)}, &activity),
          "DM header: group DMs and unmeasured names show none");
    // A stranger (a bot) reports through the guild presences an open DM
    // subscribes to: the user record's platforms carry the status.
    check(ui_private_row_status(source, 9996) == UiPrivateRowStatus::offline &&
              ui_user_presence(source, 124) == UiPresence::offline,
          "DM presence: a stranger without a presence shows offline");
    auto& users = g_app.account_ui->user_store;
    const bool set = user_store_set_client_status(&users, 124,
                                                  client_status_with(0, ClientPlatform::web, ClientPlatformStatus::online));
    check(set && ui_private_row_status(source, 9996) == UiPrivateRowStatus::online &&
              ui_user_presence(source, 124) == UiPresence::online,
          "DM presence: a stranger's guild presence (web) shows online in the DM row");
    user_store_set_client_status(&users, 124,
                                 client_status_with(client_status_with(0, ClientPlatform::web, ClientPlatformStatus::idle),
                                                    ClientPlatform::desktop, ClientPlatformStatus::dnd));
    check(ui_user_presence(source, 124) == UiPresence::busy && ui_private_row_status(source, 9997) == UiPrivateRowStatus::none,
          "DM presence: do not disturb on any platform wins; an unknown group shows none");
    // A group is online (only online) while any other member is online.
    auto& state = g_app.account_ui->chat_state;
    const u64 crew[] = {99, 124};
    const bool group = chat_state_store_upsert_channel(&state, {9995, 0, 0, utf8_literal("Duo"), 0, 0, 3}).ok() &&
                       private_channel_store_replace(state.private_channels, {9995, 0, {crew, 2}, {}, true, false, false});
    check(group && ui_private_row_status(source, 9995) == UiPrivateRowStatus::none,
          "DM presence: a group whose members are busy or idle shows no status");
    user_store_set_client_status(&users, 124, client_status_with(0, ClientPlatform::web, ClientPlatformStatus::online));
    check(ui_private_row_status(source, 9995) == UiPrivateRowStatus::online,
          "DM presence: a group with an online member shows online");
    check(user_store_clear_client_status(&users) && ui_user_presence(source, 124) == UiPresence::offline &&
              !user_store_clear_client_status(&users),
          "DM presence: a fresh session clears every platform status once");
}

void response(u16 status, Utf8View json = {}) noexcept {
    RestClientResponse reply{};
    reply.request_id = request_id;
    reply.status_code = status;
    check(application_dm_profile_response(reply, json), "DM panel: the controller takes its response");
}
const ProfilePart* find_part(const ProfileScene& s, ProfileAction action) noexcept {
    for (u32 i = 0; i < s.part_count; ++i)
        if (s.parts[i].action == action)
            return &s.parts[i];
    return nullptr;
}
bool click(ProfileAction action) noexcept {
    const auto* s = application_dm_profile_scene();
    const auto* part = s ? find_part(*s, action) : nullptr;
    const auto* node = part ? ui_widget_manager_layout_node(s->widgets, part->widget) : nullptr;
    // Text actions hit their ink, which the renderer publishes as it draws.
    if (node)
        profile_scene_publish_text_hit(*s, {&s->widgets, part->widget}, node->rectangle, node->rectangle,
                                       node->clip_rectangle);
    return node && application_dm_profile_click({(node->rectangle.left + node->rectangle.right) * .5f,
                                                 (node->rectangle.top + node->rectangle.bottom) * .5f});
}

void panel_cases() noexcept {
    auto& state = g_app.account_ui->chat_state;
    check(layout_shell(), "DM panel: shell with the profile slot");
    const u32 first_builds = text_builds;
    application_dm_profile_synchronize();
    auto* s = application_dm_profile_scene();
    check(s && g_app.renderer.dm_profile_panel == s && s->presentation == ProfilePresentation::panel &&
              near(ui_rect_width(s->rectangle), kUiDmProfilePanelWidth - 16) && s->phase == ProfilePhase::loading &&
              g_app.dm_profile_wake_ms == 1 && application_dm_profile_pending(10),
          "DM panel: showing a stale profile queues one fetch and wakes the scheduler");
    check(text_builds == first_builds + 1 && text_runs > 0,
          "DM panel text: the frame that shows the panel rebuilds the shell text with its runs");
    check(application_dm_profile_submit(500, 10) && reads == 1 && read_user == 123 && !read_guild &&
              !read_operation && !application_dm_profile_pending(20) && !g_app.dm_profile_wake_ms,
          "DM panel: one GET /users/123/profile, deduped while in flight");
    application_dm_profile_synchronize();
    check(!application_dm_profile_pending(30) && reads == 1, "DM panel: the next frame does not fetch again");
    check(text_builds == first_builds + 1, "DM panel text: an unchanged panel leaves the shell text alone");
    response(200, utf8_literal(R"({"user":{"id":"123","username":"mai","global_name":"Mai","bio":"hello"},)"
                               R"("user_profile":{"bio":"hello","pronouns":"she/her"},)"
                               R"("mutual_guilds":[{"id":"7"},{"id":"8"}],"mutual_friends":[],)"
                               R"("mutual_friends_count":0})"));
    s = application_dm_profile_scene();
    check(s && s->phase == ProfilePhase::ready && s->profile->mutual_guild_count == 2 &&
              find_part(*s, ProfileAction::expand) && find_part(*s, ProfileAction::servers) &&
              !find_part(*s, ProfileAction::open_server) && !find_part(*s, ProfileAction::command),
          "DM panel: the loaded profile lays out with View Full Profile and the mutuals row");
    g_app.dm_profile_clock_ms = 5;
    application_dm_profile_tick(4);
    check(g_app.dm_profile_clock_ms == 5, "DM panel: the loop tick waits for the activity clock");
    application_dm_profile_tick(5);
    check(!g_app.dm_profile_clock_ms,
          "DM panel: a due clock without a timed activity is consumed outside a frame");
    u32 opens = full_opens;
    check(click(ProfileAction::servers) && full_opens == opens + 1 && expanded_tab == ProfileTab::servers,
          "DM panel: the mutuals row opens the full profile on Mutual Servers");
    opens = full_opens;
    check(click(ProfileAction::expand) && full_opens == opens + 1 && expanded_tab == ProfileTab::widgets,
          "DM panel: View Full Profile opens the full profile");
    // Leaving for a server and coming back within the minute reuses the cache.
    state.home_selected = false;
    layout_shell();
    const u32 builds = text_builds;
    application_dm_profile_synchronize();
    check(!application_dm_profile_scene() && !g_app.renderer.dm_profile_panel,
          "DM panel: a server channel hides it");
    check(text_builds == builds + 1 && text_runs == 0,
          "DM panel text: the frame that hides the panel rebuilds the shell text without its runs");
    state.home_selected = true;
    layout_shell();
    application_dm_profile_synchronize();
    check(application_dm_profile_scene() && !application_dm_profile_pending(40) && reads == 1 &&
              application_dm_profile_scene()->phase == ProfilePhase::ready,
          "DM panel: a fresh cached profile shows again without a fetch");
    check(text_builds == builds + 2 && text_runs > 0,
          "DM panel text: showing it again brings its runs back in the same frame");
    // The preference hides it; the shell stops offering the slot.
    g_app.preferences.dm_profile_visible = false;
    layout_shell();
    application_dm_profile_synchronize();
    check(!application_dm_profile_scene() && text_runs == 0,
          "DM panel: the remembered choice hides it, its text in the same frame");
    g_app.preferences.dm_profile_visible = true;
    // A narrow window gives it no slot.
    g_app.renderer.width = 1100;
    layout_shell();
    application_dm_profile_synchronize();
    check(!application_dm_profile_scene(), "DM panel: below 1132 DIPs it does not show");
    g_app.renderer.width = 1280;
    application_dm_profile_release();
    // Rate limited: the failure waits the minute; a busy lane retries shortly.
    layout_shell();
    application_dm_profile_synchronize();
    admission = RestClientSubmitError::busy;
    check(!application_dm_profile_submit(501, 100) && reads == 1 && g_app.dm_profile_wake_ms == 200,
          "DM panel: a busy REST lane retries after 100 ms");
    admission = RestClientSubmitError::none;
    check(application_dm_profile_submit(502, 200) && reads == 2, "DM panel: the retry is admitted");
    response(429);
    s = application_dm_profile_scene();
    check(s != nullptr, "DM panel: a 429 keeps the panel");
    check(s && s->phase == ProfilePhase::failed, "DM panel: a 429 shows the failure");
    check(!application_dm_profile_pending(300), "DM panel: a 429 does not retry");
    application_dm_profile_release();
    layout_shell();
    application_dm_profile_synchronize();
    check(application_dm_profile_pending(400), "DM panel: a released cache fetches again");
    application_dm_profile_release();
}

const ProfilePart* find_text(const ProfileScene& s, Utf8View text) noexcept {
    for (u32 i = 0; i < s.part_count; ++i)
        if (utf8_equal(s.parts[i].text, text))
            return &s.parts[i];
    return nullptr;
}
// A bot: "name#1234", its servers in the mutuals row, the bio's markdown,
// "Created On", and its popular commands as chips that fill the composer.
void bot_cases() noexcept {
    // The DM to 124, who is no friend.
    chat_state_store_select_channel(&g_app.account_ui->chat_state, 9996);
    g_app.account_ui->active_channel_id = 9996;
    layout_shell();
    application_dm_profile_synchronize();
    const u32 before = reads;
    check(application_dm_profile_submit(510, 500) && reads == before + 1 && read_user == 124,
          "DM bot: the profile is fetched");
    // The conversation's command index is still on its way when the profile lands.
    commands_loading = true;
    response(200, utf8_literal(R"({"user":{"id":"124","username":"BlueprintBot","discriminator":"8797",)"
                               R"("global_name":null,"bot":true,"public_flags":65536},)"
                               R"("user_profile":{"bio":"*** NOW UPDATED FOR SOME MODS! ***"},)"
                               R"("mutual_guilds":[{"id":"7"}],"application":{"id":"55","flags":0,)"
                               R"("popular_application_command_ids":["901","902","903","904"]}})"));
    application_dm_profile_synchronize();
    const u32 loading_revision = application_dm_profile_scene()->revision;
    asked_channel = 0;
    application_dm_profile_synchronize();
    check(asked_channel == 9996 && !application_dm_profile_scene()->commands.count &&
              application_dm_profile_scene()->revision == loading_revision,
          "DM bot: an index on its way is asked again each frame without laying the panel out");
    commands_loading = false;
    ++command_revision;
    application_dm_profile_synchronize();
    const auto* s = application_dm_profile_scene();
    check(s && s->profile->discriminator == 8797 && s->profile->application_id == 55 &&
              s->profile->popular_command_count == 4 && s->profile->popular_commands[3] == 904,
          "DM bot: the profile keeps the discriminator and the application's popular commands");
    check(s && find_text(*s, utf8_literal("BlueprintBot#8797")) && find_text(*s, utf8_literal("Created On")) &&
              !find_text(*s, utf8_literal("Member Since")) && find_text(*s, utf8_literal("1 Mutual Server")) &&
              find_part(*s, ProfileAction::servers),
          "DM bot: name#1234, Created On and the mutual servers row");
    check(click(ProfileAction::servers) && expanded_tab == ProfileTab::servers,
          "DM bot: its servers open the full profile on Mutual Servers");
    check(s && utf8_equal(s->bio_display.view(), utf8_literal(" NOW UPDATED FOR SOME MODS! ")) &&
              s->bio_style_count && (s->bio_styles[0].flags & text_style_bold) && (s->bio_styles[0].flags & text_style_italic),
          "DM bot: *** around spaced text renders bold italic");
    u32 chips = 0;
    for (u32 i = 0; s && i < s->part_count; ++i)
        chips += s->parts[i].action == ProfileAction::command;
    check(s && asked_channel == 9996 && s->commands.count == 2 && s->commands[0].id == 901 &&
              s->commands[1].id == 902 && chips == 2 && find_text(*s, utf8_literal("Commands")) &&
              find_text(*s, utf8_literal("/info")) && find_text(*s, utf8_literal("/json")) &&
              utf8_equal(find_part(*s, ProfileAction::command)->hint, utf8_literal("About the bot")),
          "DM bot: Commands lists the popular ones in order, without NSFW or subcommand commands");
    check(click(ProfileAction::command) && inserted_application == 55 && inserted_command == 901 &&
              utf8_equal({inserted_name, inserted_name_bytes}, utf8_literal("info")),
          "DM bot: a command chip puts the command in the composer");
    ++command_revision;
    asked_channel = 0;
    application_dm_profile_synchronize();
    check(asked_channel == 9996 && application_dm_profile_scene()->commands.count == 2,
          "DM bot: a landed command index resolves the section again");
    asked_channel = 0;
    application_dm_profile_synchronize();
    check(!asked_channel, "DM bot: an unchanged index is not read again");
    application_dm_profile_release();
    chat_state_store_select_channel(&g_app.account_ui->chat_state, 9998);
    g_app.account_ui->active_channel_id = 9998;
    // The bot's profile renamed the user; later cases know them as Ren.
    check(user_store_upsert(&g_app.account_ui->user_store, 124, utf8_literal("Ren"), utf8_literal("ren")).ok(),
          "DM bot: fixture restores the user");
}

// The user's presence (activities JSON) replaces their activities.
bool set_activities(u64 user, Utf8View presence) noexcept {
    auto& f = *g_fixture;
    const auto scan = json_scan_structurals_avx1(presence, {f.structurals, 512});
    UserActivitySet set{};
    bool has = false;
    return scan.ok() &&
           user_activities_decode({presence, {f.structurals, scan.count}}, {presence, 0, JsonValueKind::object}, user,
                                  &set, &has) &&
           user_activity_upsert(&g_app.account_ui->user_store.activities, set);
}
// A JSON body with numbers spliced in.
struct Json {
    char data[1024];
    u32 bytes;
    Json& text(Utf8View value) noexcept {
        copy_bytes(data + bytes, value.data, value.bytes);
        bytes += value.bytes;
        return *this;
    }
    Json& number(u64 value) noexcept {
        bytes += decimal_write(data + bytes, value);
        return *this;
    }
};
bool rect_near(UiRect a, UiRect b) noexcept {
    return near(a.left, b.left) && near(a.top, b.top) && near(a.right, b.right) && near(a.bottom, b.bottom);
}
// Shows the DM in channel and loads its profile from body.
ProfileScene* show_loaded(u64 channel, Utf8View body) noexcept {
    chat_state_store_select_channel(&g_app.account_ui->chat_state, channel);
    g_app.account_ui->chat_state.home_selected = true;
    layout_shell();
    application_dm_profile_synchronize();
    if (application_dm_profile_pending(1000) && application_dm_profile_submit(600, 1000) && !read_operation)
        response(200, body);
    application_dm_profile_synchronize();
    return application_dm_profile_scene();
}
constexpr auto kLoaded = utf8_literal(R"({"user":{"id":"123","username":"mai","global_name":"Mai"},)"
                                      R"("user_profile":{"bio":"hello"},"mutual_guilds":[]})");

void geometry_cases() noexcept {
    application_dm_profile_release();
    auto* s = show_loaded(9998, kLoaded);
    const auto* node = ui_scene_node(g_app.shell_scene, UiSemantic::member_sidebar);
    const UiRect slot = node ? node->rectangle : UiRect{};
    const auto& theme = kDefaultTheme;
    check(s && near(ui_rect_width(slot), kUiDmProfilePanelWidth) &&
              rect_near(s->rectangle, ui_rect_inset(slot, {8, 8, 8, 8})) && near(ui_rect_width(s->rectangle), 300) &&
              rect_near(s->card, ui_rect_inset(s->rectangle, {2, 2, 2, 2})),
          "DM panel geometry: a 300 DIP card inset 8 DIPs in its slot, content inside a 2 DIP border");
    const auto mask = s ? profile_scene_card_mask(*s, theme) : ProfileCardMask{};
    check(s && rect_near(mask.rectangle, s->card) && near(mask.radius, theme.scale.radius_md - 2) &&
              near(s->banner.top, s->card.top) && near(ui_rect_height(s->banner), 105) &&
              near(s->avatar.left - s->rectangle.left, 16) && near(s->avatar.top - s->rectangle.top, 57),
          "DM panel geometry: banner and media round concentrically inside (radius 8 - 2); avatar at 16, 57");
    PaintStream paint{};
    const bool stream = paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1280, 800);
    bool plain = false;
    if (stream && s && profile_scene_paint(*s, theme, &paint) && paint.count) {
        const auto& q = paint.quads[0];
        plain = rect_near({q.left, q.top, q.right, q.bottom}, s->rectangle) && near(q.radius, theme.scale.radius_md) &&
                near(q.border_width, 1);
    }
    check(plain, "DM panel geometry: without a theme the card is rounded with a 1 DIP ring");
    // A Nitro theme: the primary colour shows as the 2 DIP border around the overlay.
    application_dm_profile_release();
    s = show_loaded(9998, utf8_literal(R"({"user":{"id":"123","username":"mai","global_name":"Mai"},)"
                                       R"("user_profile":{"theme_colors":[1193046,6636321]},"premium_type":2,)"
                                       R"("mutual_guilds":[]})"));
    bool outer = false, inner = false;
    if (stream && s && s->themed && paint_stream_reset(&paint, 1280, 800) && profile_scene_paint(*s, theme, &paint)) {
        for (u32 i = 0; i < paint.count; ++i) {
            const auto& q = paint.quads[i];
            const UiRect r{q.left, q.top, q.right, q.bottom};
            outer |= i == 0 && rect_near(r, s->rectangle) && near(q.radius, theme.scale.radius_md) &&
                     near(q.fill.red, 0x12 / 255.f) && near(q.fill.green, 0x34 / 255.f);
            inner |= rect_near(r, s->card) && near(q.radius, theme.scale.radius_md - 2);
        }
    }
    check(outer && inner, "DM panel geometry: a themed card paints its primary border, then the overlay 2 DIPs in");
    paint_stream_release(&paint);
    // A long bio: three lines (20 DIPs each here), no "View Full Bio", and no
    // rule above "View Full Profile".
    application_dm_profile_release();
    s = show_loaded(9998, utf8_literal(R"({"user":{"id":"123","username":"mai","global_name":"Mai"},)"
                                       R"("user_profile":{"bio":"A bio long enough to wrap onto many more than )"
                                       R"(three lines of the panel, so that the clamp applies to it and only )"
                                       R"(the first three lines show with an ellipsis at the end."},)"
                                       R"("mutual_guilds":[]})"));
    const auto* bio = s ? ui_widget_manager_layout_node(s->widgets, s->bio_widget) : nullptr;
    bool rule = false;
    for (u32 i = 0; s && i < s->part_count; ++i)
        rule |= s->parts[i].look == ProfileLook::rule;
    check(bio && near(ui_rect_height(bio->rectangle), 60) && !find_part(*s, ProfileAction::view_bio) &&
              find_part(*s, ProfileAction::expand) && !rule,
          "DM panel: a long bio clamps to three lines without View Full Bio; no rule above View Full Profile");
    application_dm_profile_release();
}

// The art the popout resolves for the same profile, activities and icons.
MediaAssetKey popout_art(const UserProfile& profile, ConstSlice<ProfileApplication> applications, u32 card) noexcept {
    static ProfileScene popout{};
    auto& ui = *g_app.account_ui;
    popout.presentation = ProfilePresentation::popout;
    popout.presence = UiPresence::online;
    popout.applications = applications;
    popout.measure = &ui.text_measure_cache;
    MediaAssetKey key{};
    if (profile_scene_open(&popout, &profile, &ui.chat_state, &ui.user_store, 99, {300, 100, 340, 140},
                           {0, 0, 1280, 800})) {
        u32 seen = 0;
        for (u32 i = 0; i < popout.part_count; ++i) {
            ProfileMedia media{};
            if (popout.parts[i].media == ProfileMediaKind::activity_large && seen++ == card &&
                profile_media_resolve(popout, i, &media))
                key = media.key;
        }
    }
    profile_scene_release(&popout);
    return key;
}
void activity_art_cases() noexcept {
    // A detected game without assets (its application icon stands in) and Spotify.
    check(set_activities(123, utf8_literal(R"({"activities":[)"
                                           R"({"type":2,"name":"Spotify","details":"the feeling","state":"Steve Lacy",)"
                                           R"("assets":{"large_image":"spotify:ab67616d0000b273"},"party":{"id":"spotify:1"}},)"
                                           R"({"type":0,"name":"Arknights: Endfield","application_id":"777",)"
                                           R"("details":"Lv.12","state":"Region"}]})")),
          "DM panel art: presence");
    auto* s = show_loaded(9998, kLoaded);
    const u32 reads_before = reads;
    const bool asked = s && application_dm_profile_pending(2000) && application_dm_profile_submit(601, 2000) &&
                       reads == reads_before + 1 && read_operation == 5 &&
                       utf8_equal({read_value, read_value_bytes}, utf8_literal("777"));
    check(asked, "DM panel art: a game card without art asks for its application icon, as the popout does");
    check(!application_dm_profile_pending(2001), "DM panel art: the icon request is deduped while in flight");
    response(200, utf8_literal(R"([{"id":"777","name":"Arknights","icon":"0123456789abcdef0123456789abcdef"}])"));
    s = application_dm_profile_scene();
    MediaAssetKey panel[2]{};
    char url[2][384]{};
    u32 found = 0;
    for (u32 i = 0; s && i < s->part_count && found < 2; ++i) {
        ProfileMedia media{};
        if (s->parts[i].media == ProfileMediaKind::activity_large && profile_media_resolve(*s, i, &media)) {
            panel[found] = media.key;
            copy_bytes(url[found++], media.url.data, minimum<u32>(media.url.bytes, 383));
        }
    }
    // Cards put playing first: the game, then Spotify.
    check(found == 2 && utf8_starts_with({url[0], 384}, utf8_literal("https://cdn.discordapp.com/app-icons/777/")) &&
              utf8_starts_with({url[1], 384}, utf8_literal("https://i.scdn.co/image/")) &&
              !application_dm_profile_pending(3000),
          "DM panel art: the game shows its application icon and Spotify its album art");
    // The popout shows the first card.
    const bool same = s && media_asset_key_valid(panel[0]) &&
                      media_asset_key_equal(panel[0], popout_art(*s->profile, s->applications, 0));
    check(same, "DM panel art: the panel requests the game art with the popout's media key");
    application_dm_profile_release();
}

u32 progress_bar(const ProfileScene* s) noexcept {
    for (u32 i = 2; s && i < s->part_count; ++i)
        if (s->parts[i].media == ProfileMediaKind::activity_progress)
            return i;
    return 0;
}
void spotify(Json* presence, u64 start, u64 end) noexcept {
    presence->bytes = 0;
    presence->text(utf8_literal(R"({"activities":[{"type":2,"name":"Spotify","details":"the feeling",)"
                                R"("state":"Steve Lacy","party":{"id":"spotify:1"},"timestamps":{"start":)"))
        .number(start)
        .text(utf8_literal(R"(,"end":)"))
        .number(end)
        .text(utf8_literal("}}]}"));
}
void progress_cases() noexcept {
    const u64 now = clock_unix_milliseconds();
    static Json presence{};
    spotify(&presence, now - 65'000, now + 210'000);
    check(set_activities(123, {presence.data, presence.bytes}), "DM panel progress: presence");
    auto* s = show_loaded(9998, kLoaded);
    u32 bar = progress_bar(s);
    // The bar's ends precede it: elapsed, then the track's length.
    const auto elapsed = [&]() noexcept { return bar ? s->parts[bar - 2].text : Utf8View{}; };
    const u64 after = clock_unix_milliseconds();
    const bool initial = bar && (utf8_equal(elapsed(), utf8_literal("01:05")) ||
                                 (after / 1000 != now / 1000 && utf8_equal(elapsed(), utf8_literal("01:06")))) &&
                         utf8_equal(s->parts[bar - 1].text, utf8_literal("04:35"));
    check(initial, "DM panel progress: the track time comes from timestamps.start/end at layout");
    check(s && s->activity_timed && g_app.dm_profile_clock_ms, "DM panel progress: a shown bar arms the second clock");
    const float before = bar ? s->parts[bar].color.red : 0;
    const bool t1 = s && profile_scene_tick(s, now + 10'000) && utf8_equal(elapsed(), utf8_literal("01:15"));
    const bool t2 = s && profile_scene_tick(s, now + 11'000) && utf8_equal(elapsed(), utf8_literal("01:16")) &&
                    s->parts[bar].color.red > before;
    check(t1 && t2, "DM panel progress: at t and t+1 s the time and the bar advance");
    // A start ahead of the local clock holds 00:00, as the official bar clamps it.
    spotify(&presence, now + 60'000, now + 335'000);
    set_activities(123, {presence.data, presence.bytes});
    application_dm_profile_synchronize();
    s = application_dm_profile_scene();
    bar = progress_bar(s);
    const bool held = bar && utf8_equal(elapsed(), utf8_literal("00:00")) && !profile_scene_tick(s, now + 1'000) &&
                      utf8_equal(elapsed(), utf8_literal("00:00"));
    check(held, "DM panel progress: a start in the future shows 00:00 until it passes");
    g_app.preferences.dm_profile_visible = false;
    layout_shell();
    application_dm_profile_synchronize();
    check(!g_app.dm_profile_clock_ms, "DM panel progress: a hidden panel arms no clock");
    g_app.preferences.dm_profile_visible = true;
    application_dm_profile_release();
}

void header_order_cases() noexcept {
    // Presence order lists Spotify first; the header leads with the game.
    set_activities(123, utf8_literal(R"({"activities":[)"
                                     R"({"type":2,"name":"Spotify","details":"the feeling","state":"Steve Lacy",)"
                                     R"("party":{"id":"spotify:1"}},)"
                                     R"({"type":0,"name":"Arknights: Endfield","application_id":"777"}]})"));
    chat_state_store_select_channel(&g_app.account_ui->chat_state, 9998);
    auto& ui = *g_app.account_ui;
    const UiPrivateRowSource source{&ui.chat_state, &ui.user_store, g_relations, nullptr, &ui.text_measure_cache};
    char line[192];
    UiPrivateHeaderActivity activity{};
    const bool shown = layout_shell() &&
                       ui_private_header_activity(g_app.shell_scene, source, 9998, {line, sizeof(line)}, &activity);
    check(shown && activity.glyph == UiActivityGlyph::game &&
              utf8_equal(activity.line, utf8_literal("Arknights: Endfield +1")),
          "DM header: playing ranks before listening; the other activities count as +N");
    set_activities(123, utf8_literal(R"({"activities":[{"type":4,"name":"Custom Status","state":"busy"},)"
                                     R"({"type":2,"name":"Spotify","details":"the feeling","state":"Steve Lacy",)"
                                     R"("party":{"id":"spotify:1"}}]})"));
    check(ui_private_header_activity(g_app.shell_scene, source, 9998, {line, sizeof(line)}, &activity) &&
              activity.glyph == UiActivityGlyph::music && utf8_equal(activity.line, utf8_literal("Steve Lacy")),
          "DM header: custom status is never counted");
}

void failure_cases() noexcept {
    set_activities(123, utf8_literal(R"({"activities":[]})"));
    application_dm_profile_release();
    chat_state_store_select_channel(&g_app.account_ui->chat_state, 9998);
    layout_shell();
    application_dm_profile_synchronize();
    auto* s = application_dm_profile_scene();
    check(s && s->phase == ProfilePhase::loading && find_text(*s, utf8_literal("Mai")),
          "DM panel failure: the user's cached name shows before the profile arrives");
    check(application_dm_profile_submit(700, 5000), "DM panel failure: fetch");
    response(404);
    s = application_dm_profile_scene();
    check(s && s->phase == ProfilePhase::failed && find_text(*s, utf8_literal("Mai")) &&
              find_part(*s, ProfileAction::retry) && !application_dm_profile_pending(6000),
          "DM panel failure: a 404 keeps the name and offers a retry");
    check(click(ProfileAction::retry) && application_dm_profile_pending(6000),
          "DM panel failure: Retry fetches again at once");
    check(application_dm_profile_submit(701, 6000), "DM panel failure: refetch");
    response(200, utf8_literal(R"({"user":{"id":"999","username":"x"}})"));
    s = application_dm_profile_scene();
    check(s && s->phase == ProfilePhase::failed && find_text(*s, utf8_literal("Mai")),
          "DM panel failure: a body the decoder rejects keeps the name");
    check(click(ProfileAction::retry) && application_dm_profile_submit(702, 7000), "DM panel failure: retry");
    // Pronouns up to the API's 40 code points (20 emoji: 80 bytes) and an
    // oversized connection name.
    static Json body{};
    body.bytes = 0;
    body.text(utf8_literal(R"({"user":{"id":"123","username":"mai","global_name":"Mai"},"user_profile":{"pronouns":")"));
    for (u32 i = 0; i < 20; ++i)
        body.text(utf8_literal("\xF0\x9F\x8C\xB8"));
    body.text(utf8_literal(R"("},"connected_accounts":[{"type":"domain","name":")"));
    for (u32 i = 0; i < 20; ++i)
        body.text(utf8_literal("abcdefghi."));
    body.text(utf8_literal(R"(example"}],"mutual_guilds":[]})"));
    response(200, {body.data, body.bytes});
    s = application_dm_profile_scene();
    check(s && s->phase == ProfilePhase::ready && s->profile->pronouns.bytes == 80 && !s->profile->connection_count,
          "DM panel failure: long pronouns load; an oversized connection is left out, not the profile");
    // Switching DMs while a fetch is in flight: its answer wakes the next.
    application_dm_profile_release();
    layout_shell();
    application_dm_profile_synchronize();
    check(application_dm_profile_submit(703, 8000) && read_user == 123, "DM panel failure: first fetch");
    chat_state_store_select_channel(&g_app.account_ui->chat_state, 9996);
    layout_shell();
    application_dm_profile_synchronize();
    s = application_dm_profile_scene();
    check(s && !application_dm_profile_pending(8000) && find_text(*s, utf8_literal("Ren")),
          "DM panel failure: the next DM shows its basics while the first fetch is in flight");
    response(200, kLoaded);
    check(g_app.dm_profile_wake_ms && application_dm_profile_pending(9000) &&
              application_dm_profile_submit(704, 9000) && read_user == 124,
          "DM panel failure: the first answer wakes the shown user's fetch without a frame");
    application_dm_profile_release();
    chat_state_store_select_channel(&g_app.account_ui->chat_state, 9998);
}

void set_tag(u64 user, bool enabled, bool system) noexcept {
    UserAppearance a{};
    a.user_id = user;
    a.fields = appearance_guild_tag | appearance_nameplate | appearance_system;
    a.primary_enabled = enabled;
    a.primary_guild_id = enabled ? 777 : 0;
    a.primary_tag.bytes = enabled ? 4 : 0;
    copy_bytes(a.primary_tag.data, "WUBE", 4);
    a.primary_badge.bytes = enabled ? 6 : 0;
    copy_bytes(a.primary_badge.data, "abc123", 6);
    a.nameplate_sku = enabled ? 555 : 0;
    a.system = system;
    user_appearance_upsert(&g_app.account_ui->user_store.appearance, a);
}

// A one-to-one row's name, its server tag or OFFICIAL chip and the close button never overlap, at any width.
void row_cases() noexcept {
    auto& ui = *g_app.account_ui;
    const UiPrivateRowSource source{&ui.chat_state, &ui.user_store, g_relations, nullptr, &ui.text_measure_cache};
    set_tag(123, true, false);
    bool apart = true, inside = true, centred = true, closed = true;
    for (float width = 60; width <= 320; width += 20) {
        const UiRect row{0, 0, 60 + width + 40, 42}, content{60, 0, 60 + width, 42};
        UiPrivateRowChip chip{};
        const bool made = ui_private_row_chip(source, 9998, {}, content, content.right, false, &chip);
        const auto& b = chip.badge.rectangle;
        const auto close = ui_private_row_close_rectangle(row);
        apart = apart && made && chip.kind == UiPrivateRowChipKind::server_tag && chip.name.left == content.left &&
                chip.name.right <= b.left && b.left - chip.name.right <= kUiPrivateRowChipGap + .01f;
        inside = inside && b.left >= content.left && b.right <= content.right + .01f && chip.name.right <= content.right;
        centred = centred && near((chip.name.top + chip.name.bottom) * .5f, (b.top + b.bottom) * .5f);
        closed = closed && b.right <= close.left && chip.name.right <= close.left;
    }
    check(apart && inside && centred && closed,
          "DM row: the server tag follows the name 8 DIPs after it, centred on its line and clear of the close button at every width");
    UiPrivateRowChip chip{};
    const UiRect content{60, 0, 300, 42};
    // "Mai" measures 24 DIPs; the line keeps 2 more so the name is never ellipsized by rounding.
    check(ui_private_row_chip(source, 9998, {}, content, 300, false, &chip) && near(chip.name.right, 60 + 26) &&
              near(chip.badge.rectangle.left, 60 + 26 + kUiPrivateRowChipGap) &&
              near(ui_rect_width(chip.badge.rectangle), 66),
          "DM row: a short name leaves the chip at its natural width right after it");
    check(ui_private_row_chip(source, 9998, {}, content, 300, true, &chip) &&
              near(chip.name.bottom - chip.name.top, 20) && near(chip.name.top, 21 - 19),
          "DM row: a subtitle lifts the name line (and its chip) above the activity line");
    const auto* record = user_appearance_find(ui.user_store.appearance, 123);
    ProfileMedia plate{}, still{}, badge{};
    const UiRect row_rectangle{0, 0, 240, 42};
    check(record && user_nameplate_media(*record, false, row_rectangle, row_rectangle, &still) &&
              user_nameplate_media(*record, true, row_rectangle, row_rectangle, &plate) &&
              utf8_equal({still.url.data, still.url.bytes},
                         utf8_literal("https://cdn.discordapp.com/media/v1/collectibles-shop/555/static")) &&
              utf8_equal({plate.url.data, plate.url.bytes},
                         utf8_literal("https://cdn.discordapp.com/media/v1/collectibles-shop/555/animated")) &&
              !(still.key.flags & media_asset_flag_animated) && (plate.key.flags & media_asset_flag_animated) &&
              user_guild_badge_media(*record, row_rectangle, row_rectangle, &badge) &&
              utf8_equal({badge.url.data, badge.url.bytes},
                         utf8_literal("https://cdn.discordapp.com/clan-badges/777/abc123.png?size=32")),
          "DM row: the nameplate and the tag's badge come from the CDN, the nameplate animated only when asked");
    // The nameplate shows behind a hovered or selected row only, and moves only under the pointer.
    const auto rest = ui_private_row_plate(false, false, true), selected = ui_private_row_plate(true, false, true),
               hovered = ui_private_row_plate(false, true, true), still_hover = ui_private_row_plate(false, true, false);
    check(!rest.shown && !rest.animated && selected.shown && !selected.animated && hovered.shown && hovered.animated &&
              still_hover.shown && !still_hover.animated,
          "DM row: the nameplate shows when hovered or selected, never at rest, and animates only under the pointer");
    set_tag(123, false, false);
    check(!ui_private_row_chip(source, 9998, {}, content, 300, false, &chip) && chip.kind == UiPrivateRowChipKind::none &&
              near(chip.name.right, 300),
          "DM row: without a tag the name keeps the whole line");
    // A conversation with its context menu open keeps its nameplate, though neither hovered nor selected.
    static PopupScene popup;
    popup.active = true;
    popup.kind = PopupKind::context_menu;
    popup.owner_id = 9998;
    UiPrivateRowSource with_menu = source;
    with_menu.menu = &popup;
    const auto menu_plate = ui_private_row_plate(false, false, true, ui_private_row_menu_channel(with_menu) == 9998);
    const auto other_plate = ui_private_row_plate(false, false, true, ui_private_row_menu_channel(with_menu) == 9996);
    popup.kind = PopupKind::popover;
    check(menu_plate.shown && !menu_plate.animated && !other_plate.shown &&
              ui_private_row_menu_channel(with_menu) == 0 && ui_private_row_menu_channel(source) == 0,
          "DM row: the nameplate stays while the row's context menu is open (still), and only for that row");
    popup.active = false;
    // Discord's system account: OFFICIAL with a check, and "Official Discord Message" under the name.
    set_tag(124, false, true);
    char storage[64];
    check(ui_private_row_chip(source, 9996, {}, content, 300, true, &chip) && chip.kind == UiPrivateRowChipKind::official &&
              chip.badge.accent && chip.badge.compact && chip.badge.leading &&
              utf8_equal(chip.badge.label, utf8_literal("OFFICIAL")) &&
              utf8_equal(ui_private_row_subtitle(source, 9996, {storage, sizeof(storage)}),
                         utf8_literal("Official Discord Message")),
          "DM row: the system account shows OFFICIAL and its official-message line");
    set_tag(124, false, false);
}

// The header's identity (avatar and name), its click and the names popout.
void header_identity_cases() noexcept {
    auto& ui = *g_app.account_ui;
    const UiPrivateRowSource source{&ui.chat_state, &ui.user_store, g_relations, nullptr, &ui.text_measure_cache};
    UiPrivateHeaderIdentity identity{};
    const bool laid_out = layout_shell();
    const auto* label = ui_scene_node(g_app.shell_scene, UiSemantic::title_label);
    const bool found = laid_out && ui_private_header_identity(g_app.shell_scene, source, 9998, &identity);
    check(found && label && identity.user == 123 && ui_rect_equal(identity.avatar, ui_private_header_avatar(g_app.shell_scene)) &&
              near(identity.name.left, label->rectangle.left + kUiPrivateHeaderAvatar + 8) &&
              near(ui_rect_width(identity.name), 24),
          "DM header: the identity is the avatar and the name after it");
    UiPrivateHeaderIdentity group{};
    check(!ui_private_header_identity(g_app.shell_scene, source, 9997, &group),
          "DM header: a group has no identity to open");
    const auto event = [&](EventType type, float x, float y) noexcept {
        WindowEvent e{};
        e.type = type;
        e.button = static_cast<u8>(PointerButton::left);
        e.pointer.x = x;
        e.pointer.y = y;
        return e;
    };
    const float ay = (identity.avatar.top + identity.avatar.bottom) * .5f;
    const float ax = (identity.avatar.left + identity.avatar.right) * .5f;
    const float nx = (identity.name.left + identity.name.right) * .5f;
    const u32 before = view_full_opens;
    check(application_private_header_window_event(event(EventType::cursor_query, ax, ay)) &&
              view_full_opens == before,
          "DM header: the pointer over the avatar is a button");
    check(application_private_header_window_event(event(EventType::pointer_down, ax, ay)) &&
              view_full_opens == before + 1 && view_full_user == 123 && ui_rect_equal(view_full_anchor, identity.avatar),
          "DM header: clicking the avatar opens the user's full profile");
    check(application_private_header_window_event(event(EventType::pointer_down, nx, ay)) &&
              view_full_opens == before + 2 && view_full_user == 123,
          "DM header: clicking the name opens it too");
    check(!application_private_header_window_event(event(EventType::pointer_down, identity.name.right + 120, ay)) &&
              !application_private_header_window_event(event(EventType::pointer_down, ax, identity.avatar.bottom + 60)) &&
              view_full_opens == before + 2,
          "DM header: clicks beside the identity leave the header alone");
    // Without another nickname there is no popout to show on hover.
    check(!application_private_header_window_event(event(EventType::pointer_move, ax, ay)) &&
              !g_app.renderer.names_card_scene,
          "DM header: hovering a user with no other nicknames shows no popout");
    // The same nickname in two servers is one row with both servers' pictures; the pointer on the popout keeps it.
    static GuildMemberRecord member_records[8];
    static u32 member_order[8];
    auto& state = ui.chat_state;
    const bool shared =
        guild_member_store_initialize(&ui.guild_member_store, {member_records, 8}, {member_order, 8}) &&
        chat_state_store_upsert_guild(&state, {10, utf8_literal("Til Server"), GuildMutationKind::create, 3}).ok() &&
        chat_state_store_upsert_guild(&state, {11, utf8_literal("Wube"), GuildMutationKind::create, 3}).ok() &&
        guild_member_store_upsert(&ui.guild_member_store, state, 10, 123, utf8_literal("TIL"), {}, false,
                                  GuildMemberStatus::offline).ok() &&
        guild_member_store_upsert(&ui.guild_member_store, state, 11, 123, utf8_literal("TIL"), {}, false,
                                  GuildMemberStatus::offline).ok();
    application_private_header_window_event(event(EventType::pointer_move, identity.name.right + 200, ay));
    application_private_header_window_event(event(EventType::pointer_move, ax, ay));
    const auto* shown = g_app.renderer.names_card_scene;
    check(shared && shown && shown->active && shown->row_count == 1 && shown->icon_count[0] == 2 &&
              utf8_equal({shown->username_text, shown->username_bytes}, utf8_literal("mai")) &&
              utf8_equal({shown->nickname_text[0], shown->nickname_bytes[0]}, utf8_literal("TIL")) &&
              utf8_equal({shown->initial_text[0], shown->initial_bytes[0]}, utf8_literal("T")) &&
              near(shown->panel.top, identity.avatar.bottom + kUiNamesCardGap),
          "DM header: hovering the avatar shows the username with AKA and each other nickname with its servers");
    const float cx = shown ? shown->panel.left + 30 : 0, cy = shown ? shown->panel.top + 30 : 0;
    check(shown && application_private_header_window_event(event(EventType::pointer_move, cx, cy)) &&
              application_private_header_window_event(event(EventType::pointer_down, cx, cy)) &&
              view_full_opens == before + 2 && g_app.renderer.names_card_scene,
          "DM header: the popout stays under the pointer and swallows clicks on it");
    WindowEvent left{};
    left.type = EventType::pointer_leave;
    application_private_header_window_event(left);
    check(g_app.renderer.names_card_scene && application_private_header_next_wake() != 0,
          "DM header: leaving schedules the popout's close instead of dropping it at once");
    chat_state_store_select_channel(&state, 9996);
    application_private_header_pump();
    chat_state_store_select_channel(&state, 9998);
    check(!g_app.renderer.names_card_scene && application_private_header_next_wake() == 0,
          "DM header: switching conversations closes the popout");
}

// The identity is a focusable header button: Enter and Space open the profile, focus shows the popout, and the
// screen-reader node names it and offers Invoke and Expand.
void header_keyboard_cases() noexcept {
    auto& ui = *g_app.account_ui;
    const bool laid_out = layout_shell();
    const auto identity = g_app.shell_scene.semantics[static_cast<u32>(UiSemantic::private_identity)];
    const auto* node = ui_scene_node(g_app.shell_scene, UiSemantic::private_identity);
    const auto* label = ui_scene_node(g_app.shell_scene, UiSemantic::title_label);
    check(laid_out && identity.generation && node && label && near(node->rectangle.left, label->rectangle.left) &&
              near(ui_rect_width(node->rectangle), kUiPrivateHeaderAvatar + 8 + 24),
          "DM header: the identity is one button over the avatar and the name");
    const bool focused = ui_widget_manager_focus(&g_app.shell_scene.widgets, identity);
    g_app.context_focus = &g_app.shell_scene.widgets;
    const u32 before = view_full_opens;
    check(focused && application_private_header_focused() && application_private_header_activate() &&
              view_full_opens == before + 1 && view_full_user == 123,
          "DM header: the focused identity opens the full profile");
    // Focus shows the popout (the nicknames of header_identity_cases are still known) and releases it with a grace.
    chat_state_store_select_channel(&ui.chat_state, 9998);
    application_private_header_pump();
    check(g_app.renderer.names_card_scene && g_app.renderer.names_card_scene->active,
          "DM header: focusing the identity shows the names popout");
    g_app.shell_scene.widgets.focused = ui_widget_invalid_handle();
    application_private_header_pump();
    check(g_app.renderer.names_card_scene && application_private_header_next_wake() != 0,
          "DM header: moving focus on releases the popout after the grace");
    application_private_header_popout(false);
    UiAccessWidgetInfo info{};
    application_private_header_access(&info);
    check(!g_app.renderer.names_card_scene && info.role == UiAccessRole::button && info.item == kPrivateHeaderAccessItem &&
              (info.actions & (ui_access_action_invoke | ui_access_action_expand)) ==
                  (ui_access_action_invoke | ui_access_action_expand) &&
              (info.state & ui_access_collapsed) && utf8_equal(info.name, utf8_literal("Mai, view profile")),
          "DM header: the screen-reader node is a collapsed button named after the user");
    application_private_header_popout(true);
    UiAccessWidgetInfo open{};
    application_private_header_access(&open);
    check(g_app.renderer.names_card_scene && (open.state & ui_access_expanded),
          "DM header: Expand shows the popout for a screen reader");
    application_private_header_popout(false);
    g_app.context_focus = nullptr;
}

void names_card_cases() noexcept {
    static UiNamesCardScene card;
    UiNamesCardContent content{};
    content.username = utf8_literal("biglojze");
    content.row_count = 2;
    content.rows[0].nickname = utf8_literal("TIL");
    content.rows[0].server_count = 1;
    content.rows[0].icons[0] = {1001, 1, MediaAssetKind::guild_icon, 0};
    content.rows[1].nickname = utf8_literal("Wube Fan");
    content.rows[1].server_count = 5;
    for (u32 i = 0; i < 3; ++i) {
        content.rows[1].icons[i] = i ? MediaAssetKey{} : MediaAssetKey{1002, 1, MediaAssetKind::guild_icon, 0};
        content.rows[1].initials[i] = utf8_literal("W");
    }
    const UiRect anchor{30, 8, 54, 32}, viewport{0, 0, 800, 600};
    auto& ui = *g_app.account_ui;
    check(ui_names_card_show(&card, 123, content, anchor, viewport, 0, true, &ui.text_measure_cache) && card.active &&
              near(card.panel.top, anchor.bottom + kUiNamesCardGap) && near(card.panel.left, 18) &&
              ui_rect_width(card.panel) >= kUiNamesCardMinimum + 32 && ui_rect_width(card.panel) <= kUiNamesCardMaximum + 32,
          "names popout: under the avatar, 12 DIPs left of it, 212 to 312 DIPs wide");
    const float inner_left = card.panel.left + kUiNamesCardPadding, inner_right = card.panel.right - kUiNamesCardPadding;
    check(near(card.username.left, inner_left) && card.label.left >= card.username.right + kUiNamesCardGap - .01f &&
              near(card.divider.left, inner_left) && near(card.divider.right, inner_right) &&
              near(card.divider.bottom - card.divider.top, 1) &&
              near(card.divider.top, card.username.bottom + kUiNamesCardGap),
          "names popout: the username and AKA, then a divider across the content");
    bool clear = card.icon_count[0] == 1 && card.icon_count[1] == 3 && card.more_bytes[0] == 0 && card.more_bytes[1] == 2 &&
                 utf8_equal({card.more_text[1], card.more_bytes[1]}, utf8_literal("+2"));
    for (u32 row = 0; row < 2; ++row) {
        const u32 first = row * kUiNamesCardIcons;
        clear = clear && card.nicknames[row].right + kUiNamesCardNicknameGap <= card.icons[first].left + .01f &&
                card.icons[first + card.icon_count[row] - 1].right <= inner_right + .01f &&
                card.nicknames[row].top >= card.divider.bottom;
        for (u32 i = 1; i < card.icon_count[row]; ++i)
            clear = clear && near(card.icons[first + i].left, card.icons[first + i - 1].right + kUiNamesCardIconGap);
    }
    clear = clear && card.more[1].left >= card.icons[kUiNamesCardIcons + 2].right && card.more[1].right <= inner_right + .01f &&
            card.nicknames[1].top >= card.nicknames[0].bottom + kUiNamesCardRowGap - .01f;
    check(clear, "names popout: nicknames, up to three pictures and +N stay apart and inside the card");
    UiNamesCardMedia media{};
    check(ui_names_card_media(card, 0, &media) && media.key.id == 1001 &&
              !ui_names_card_media(card, kUiNamesCardIcons + 1, &media) &&
              ui_names_card_media(card, kUiNamesCardIcons, &media) && media.key.id == 1002,
          "names popout: a server picture is drawn where it has one; a server without is a disc with its initial");
    const UiRect low{30, 560, 54, 584};
    check(ui_names_card_show(&card, 123, content, low, viewport, 0, true, &ui.text_measure_cache) &&
              card.panel.bottom <= low.top && ui_names_card_contains(card, card.panel.left + 20, card.panel.top + 20),
          "names popout: opens above the avatar when the window ends below it");
    ui_names_card_close(&card);
    check(!card.active && !ui_names_card_contains(card, 40, 40), "names popout: closing hides it");
}

void run() noexcept {
    side_panel_cases();
    layout_cases();
    fetch_policy_cases();
    caption_cases();
    typing_cases();
    if (!fixture_setup()) {
        check(false, "DM fixture: stores");
        return;
    }
    header_cases();
    row_cases();
    header_identity_cases();
    header_keyboard_cases();
    names_card_cases();
    panel_cases();
    bot_cases();
    geometry_cases();
    activity_art_cases();
    progress_cases();
    header_order_cases();
    failure_cases();
}
} // namespace maicord::dm_profile_test

namespace maicord::app_detail {
using namespace dm_profile_test;
UiRect application_viewport_dips() noexcept {
    return {0, 0, 1280, 800};
}
bool refresh_account_text(bool) noexcept {
    return true;
}
bool application_friends_user_relation(u64, u8* relation) noexcept {
    *relation = 1;
    return true;
}
bool application_friends_user_action(u64, ApplicationFriendsUserAction) noexcept {
    return true;
}
bool application_friends_active() noexcept {
    return false;
}
const GameStore* application_games() noexcept {
    return nullptr;
}
void game_lookup_want(u64) noexcept {}
u32 game_lookup_revision() noexcept {
    return 0;
}
bool application_search_active() noexcept {
    return false;
}
bool application_profile_open(u64, const UiRect&, ProfileAnchor) noexcept {
    ++full_opens;
    return true;
}
bool application_profile_expand(u64, ProfileTab tab) noexcept {
    expanded_tab = tab;
    return true;
}
bool application_profile_view_full(u64 user, const UiRect& anchor) noexcept {
    ++view_full_opens;
    view_full_user = user;
    view_full_anchor = anchor;
    return true;
}
bool application_settings_active() noexcept {
    return false;
}
const UiPrivateRowSource* application_private_channel_rows() noexcept {
    static UiPrivateRowSource source{};
    source = {&g_app.account_ui->chat_state, &g_app.account_ui->user_store, g_relations, nullptr,
             &g_app.account_ui->text_measure_cache};
    return &source;
}
// The bot's loaded root commands: its third popular one is NSFW, its fourth
// has subcommands, and the index lists them out of popularity order.
u32 application_commands_for_application(u64 channel, u64 application, Slice<ApplicationRootCommand> output,
                                         bool* loading) noexcept {
    asked_channel = channel;
    *loading = commands_loading;
    if (commands_loading)
        return 0;
    static const ApplicationRootCommand roots[]{
        {utf8_literal("json"), utf8_literal("Blueprint as JSON"), 902, false, false},
        {utf8_literal("mods"), {}, 904, false, true},
        {utf8_literal("info"), utf8_literal("About the bot"), 901, false, false},
        {utf8_literal("lewd"), utf8_literal("Not for here"), 903, true, false}};
    if (application != 55)
        return 0;
    const u32 count = minimum<u32>(4, static_cast<u32>(output.count));
    for (u32 i = 0; i < count; ++i)
        output[i] = roots[i];
    return count;
}
u32 application_commands_catalog_revision() noexcept {
    return command_revision;
}
bool application_commands_insert(u64 application, u64 command, Utf8View name) noexcept {
    inserted_application = application;
    inserted_command = command;
    inserted_name_bytes = minimum<u32>(name.bytes, sizeof(inserted_name));
    copy_bytes(inserted_name, name.data, inserted_name_bytes);
    return true;
}
bool application_guild_card_open(u64, UiRect) noexcept {
    return true;
}
bool application_open_content_url(const Utf8View&) noexcept {
    return true;
}
bool application_link_open(Utf8View, bool) noexcept {
    return true;
}
const GatewayClientVoiceSnapshot* application_voice_call() noexcept {
    return nullptr;
}
bool application_voice_join(u64, u64) noexcept {
    return true;
}
bool select_content_guild(u64) noexcept {
    return true;
}
bool application_user_menu_open(u64, UiRect, u64, UserMenuSource, ConstSlice<PopupItem>,
                                const UiPopupFeedback*) noexcept {
    ++menus;
    return true;
}
} // namespace maicord::app_detail
namespace maicord {
// The shell text rebuild projects what the renderer would: the bound, active
// panel's text runs (render/d3d11/renderer_text_overlay.cpp project_shell_member_text).
bool renderer_rebuild_content_text(D3D11Renderer* renderer, const Theme& theme) noexcept {
    using namespace dm_profile_test;
    ++text_builds;
    text_runs = 0;
    const auto* panel = renderer->dm_profile_panel;
    static UiTextCommand commands[256];
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    if (panel && panel->active && ui_text_command_stream_initialize(&stream, {commands, 256}) &&
        profile_scene_text_output(*panel, theme, &output, nullptr))
        text_runs = stream.count;
    return true;
}
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_dm_profile_tests_entry() noexcept {
    maicord::dm_profile_test::run();
    maicord::test_exit(maicord::dm_profile_test::failures, "DM view chrome");
}
