// The DM view's chrome: the side slot plan (DM profile vs member list), the
// shell layout around it, the title bar centre per view, the DM header's
// activity line, the typing dots' clock, and the DM profile panel's runtime
// (fetch dedupe, caching, rate limits, toggles). Synthetic fixtures only.
#include "application_dm_profile.hpp"
#include "application_profile.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_content_input.hpp"
#include "application_friends.hpp"
#include "application_guild_card.hpp"
#include "application_link_trust.hpp"
#include "application_search.hpp"
#include "application_user_actions.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/model/relationship_store.hpp"
#include "maicord/ui/activity_status.hpp"
#include "maicord/ui/caption_bar.hpp"
#include "maicord/ui/dm_profile_panel.hpp"
#include "maicord/ui/private_channel_rows.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/ui/typing_indicator.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::dm_profile_test {
using namespace app_detail;
u32 failures, reads, full_opens, menus;
u64 read_user, read_guild, request_id;
u8 read_operation;
RestClientSubmitError admission;

void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
bool near(float a, float b) noexcept {
    return a - b < .01f && b - a < .01f;
}
// 8 DIPs a code point, 20 a line.
bool measure_text(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    const auto validation = utf8_validate(request.text);
    if (!validation.valid || request.constraint.width <= 0)
        return false;
    const float natural = static_cast<float>(validation.codepoints) * 8;
    const u32 columns = maximum(1u, static_cast<u32>(request.constraint.width / 8));
    const u32 lines = request.no_wrap ? 1u : maximum(1u, (validation.codepoints + columns - 1) / columns);
    *value = {{request.no_wrap ? natural : minimum(natural, request.constraint.width), static_cast<float>(lines) * 20},
              16,
              lines};
    return true;
}
RestClientSubmitResult WINAPI read_profile(void*, u64 user, u64 guild, u64 id, u8 operation, Utf8View) noexcept {
    if (admission == RestClientSubmitError::none) {
        ++reads;
        read_user = user;
        read_guild = guild;
        read_operation = operation;
        request_id = id;
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
    check(plan.panel == UiSidePanel::profile && plan.width == kUiDmProfilePanelWidth && plan.toggle &&
              !plan.unavailable,
          "side panel: a one-to-one DM shows the recipient's 340 DIP profile");
    check(utf8_equal(ui_side_panel_toggle_label(true, false, true), utf8_literal("Hide User Profile")) &&
              utf8_equal(ui_side_panel_toggle_label(true, false, false), utf8_literal("Show User Profile")) &&
              utf8_equal(ui_side_panel_toggle_label(true, true, false),
                         utf8_literal("Show User Profile (Unavailable)")) &&
              utf8_equal(ui_side_panel_toggle_label(false, false, true), utf8_literal("Hide member list")),
          "side panel: the header toggle names what it switches");
    dm.profile_visible = false;
    plan = ui_side_panel_plan(dm);
    check(plan.panel == UiSidePanel::none && plan.toggle && plan.width == kUiDmProfilePanelWidth,
          "side panel: the hidden profile keeps its toggle");
    dm.profile_visible = true;
    dm.members_visible = false;
    check(ui_side_panel_plan(dm).panel == UiSidePanel::profile,
          "side panel: the profile choice is independent of the member list's");
    dm.window_width = kUiDmProfilePanelMinimumWindowWidth - 1;
    plan = ui_side_panel_plan(dm);
    check(plan.panel == UiSidePanel::none && plan.unavailable && plan.toggle,
          "side panel: below 1132 DIPs the profile gives way and its toggle is unavailable");
    const UiSidePanelInput group{3, true, false, true, true, 0, 1280};
    const UiSidePanelInput server{0xFE, false, false, false, true, 260, 1280};
    const UiSidePanelInput friends{0, true, true, true, true, 0, 1280};
    const UiSidePanelInput empty_home{0, true, false, true, true, 0, 1280};
    check(ui_side_panel_plan(group).panel == UiSidePanel::members &&
              ui_side_panel_plan(server).panel == UiSidePanel::none && ui_side_panel_plan(server).width == 260 &&
              !ui_side_panel_plan(friends).toggle && !ui_side_panel_plan(empty_home).toggle,
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
        const auto* messages = ui_scene_resolve_semantic(scene, UiSemantic::message_view);
        const auto* members = ui_scene_resolve_semantic(scene, UiSemantic::member_sidebar);
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
          "side panel layout: the profile takes 340 DIPs at the right and the chat narrows by exactly that");
    check(ui_scene_resolve_semantic(scene, UiSemantic::member_toggle) != nullptr,
          "side panel layout: the hidden profile keeps its header toggle");
    dm.profile_visible = true;
    dm.window_width = 1131;
    const bool narrow = measure(dm, 1131, &chat, &slot);
    check(narrow && !ui_rect_has_area(slot) && ui_scene_resolve_semantic(scene, UiSemantic::member_toggle),
          "side panel layout: a narrower window drops the panel and keeps the toggle");
    // A wide channel list leaves less than a readable chat: the shell drops it too.
    auto metrics = shell_metrics({1, true, false, false, true, 0, 1200}, 1200);
    metrics.navigation_width = 600;
    const bool squeezed = ui_scene_layout_with_metrics(&scene, UiShellView::private_channels, metrics, 1200, 800);
    const auto* members = ui_scene_resolve_semantic(scene, UiSemantic::member_sidebar);
    const auto* messages = ui_scene_resolve_semantic(scene, UiSemantic::message_view);
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
    UiTypingDot dots[kUiTypingDotCount];
    ui_typing_dots_geometry({10, 0, 40, 20}, kUiTypingDotsRest, dots);
    check(near(dots[0].rectangle.left, 10) && near(ui_rect_width(dots[1].rectangle), 7) &&
              near(dots[2].rectangle.left - dots[1].rectangle.left, 8.75f) && near(dots[2].rectangle.top, 6.5f) &&
              near(kUiTypingLabelInset, 8 + 24.5f + 4),
          "typing dots: three 7 DIP dots 8.75 apart, the label 4 after them");
    PaintStream paint{};
    if (!paint_stream_initialize(&paint) || !paint_stream_reset(&paint, 100, 40)) {
        check(false, "typing dots: paint stream");
        return;
    }
    bool three = true;
    for (u16 phase = 0; phase < kUiTypingDotsPhases; ++phase) {
        const u32 before = paint.count;
        three = three && ui_typing_dots_paint(&paint, {0, 0, 40, 20}, kDefaultTheme.text_primary, phase) &&
                paint.count == before + 3;
    }
    check(three, "typing dots: every phase paints the same three quads (damage stays on them)");
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
    const u64 peer[] = {123};
    if (!chat_state_store_initialize(&state, {f.guilds, 2}, {f.guild_order, 2}, {f.channels, 8},
                                     {f.channel_order, 8}, {f.names, sizeof(f.names)}) ||
        !user_store_initialize(&ui.user_store, {f.users, 8}, {f.user_order, 8}, {f.user_names, sizeof(f.user_names)}) ||
        !private_channel_store_initialize(&private_store, {f.private_records, 4}, {f.private_ids, 8}) ||
        !private_channel_store_replace(&private_store, {9998, 0, {peer, 1}, {}, true, false, false}) ||
        !relationship_store_initialize(&relations, {f.relation_records, 4}) ||
        !ui_text_measure_cache_initialize(&ui.text_measure_cache, {f.measure_records, 128}, measure_text, nullptr))
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
        !chat_state_store_upsert_channel(&state, {9997, 0, 0, utf8_literal("Crew"), 0, 0, 3}).ok() ||
        !chat_state_store_select_home(&state).ok() || !chat_state_store_select_channel(&state, 9998).ok())
        return false;
    state.home_selected = true;
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
    g_app.account.api.profile_get = read_profile;
    return ui_scene_initialize_bootstrap(&g_app.shell_scene);
}

bool layout_shell() noexcept {
    const auto in = application_side_panel_input();
    g_app.shell_scene.side_panel_profile = in.home && in.channel_type == 1;
    return ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::private_channels, shell_metrics(in, 1280),
                                        1280, 800);
}

void header_cases() noexcept {
    auto& ui = *g_app.account_ui;
    const UiPrivateRowSource source{&ui.chat_state, &ui.user_store, g_relations, nullptr, &ui.text_measure_cache};
    char line[192];
    UiPrivateHeaderActivity activity{};
    const bool shown = layout_shell() &&
                       ui_private_header_activity(g_app.shell_scene, source, 9998, {line, sizeof(line)}, &activity);
    const auto* label = ui_scene_resolve_semantic(g_app.shell_scene, UiSemantic::title_label);
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
    return node && application_dm_profile_click({(node->rectangle.left + node->rectangle.right) * .5f,
                                                 (node->rectangle.top + node->rectangle.bottom) * .5f});
}

void panel_cases() noexcept {
    auto& state = g_app.account_ui->chat_state;
    check(layout_shell(), "DM panel: shell with the profile slot");
    application_dm_profile_synchronize();
    auto* s = application_dm_profile_scene();
    check(s && g_app.renderer.dm_profile_panel == s && s->presentation == ProfilePresentation::panel &&
              near(ui_rect_width(s->rectangle), kUiDmProfilePanelWidth) && s->phase == ProfilePhase::loading &&
              g_app.dm_profile_wake_ms == 1 && application_dm_profile_pending(10),
          "DM panel: showing a stale profile queues one fetch and wakes the scheduler");
    check(application_dm_profile_submit(500, 10) && reads == 1 && read_user == 123 && !read_guild &&
              !read_operation && !application_dm_profile_pending(20) && !g_app.dm_profile_wake_ms,
          "DM panel: one GET /users/123/profile, deduped while in flight");
    application_dm_profile_synchronize();
    check(!application_dm_profile_pending(30) && reads == 1, "DM panel: the next frame does not fetch again");
    response(200, utf8_literal(R"({"user":{"id":"123","username":"mai","global_name":"Mai","bio":"hello"},)"
                               R"("user_profile":{"bio":"hello","pronouns":"she/her"},)"
                               R"("mutual_guilds":[{"id":"7"},{"id":"8"}],"mutual_friends":[],)"
                               R"("mutual_friends_count":0})"));
    s = application_dm_profile_scene();
    check(s && s->phase == ProfilePhase::ready && s->profile->mutual_guild_count == 2 &&
              find_part(*s, ProfileAction::expand) && find_part(*s, ProfileAction::servers) &&
              !find_part(*s, ProfileAction::open_server),
          "DM panel: the loaded profile lays out with View Full Profile and collapsed mutual servers");
    g_app.dm_profile_clock_ms = 5;
    application_dm_profile_tick(4);
    check(g_app.dm_profile_clock_ms == 5, "DM panel: the loop tick waits for the activity clock");
    application_dm_profile_tick(5);
    check(!g_app.dm_profile_clock_ms,
          "DM panel: a due clock without a timed activity is consumed outside a frame");
    check(click(ProfileAction::servers) && find_part(*application_dm_profile_scene(), ProfileAction::open_server) &&
              (application_dm_profile_scene()->sections & 1),
          "DM panel: Mutual Servers expands in place");
    const u32 opens = full_opens;
    check(click(ProfileAction::expand) && full_opens == opens + 1, "DM panel: View Full Profile opens the full profile");
    // Leaving for a server and coming back within the minute reuses the cache.
    state.home_selected = false;
    layout_shell();
    application_dm_profile_synchronize();
    check(!application_dm_profile_scene() && !g_app.renderer.dm_profile_panel,
          "DM panel: a server channel hides it");
    state.home_selected = true;
    layout_shell();
    application_dm_profile_synchronize();
    check(application_dm_profile_scene() && !application_dm_profile_pending(40) && reads == 1 &&
              application_dm_profile_scene()->phase == ProfilePhase::ready,
          "DM panel: a fresh cached profile shows again without a fetch");
    // The preference hides it; the shell stops offering the slot.
    g_app.preferences.dm_profile_visible = false;
    layout_shell();
    application_dm_profile_synchronize();
    check(!application_dm_profile_scene(), "DM panel: the remembered choice hides it");
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
    panel_cases();
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
const RelationshipStore* application_friends_relationships() noexcept {
    return g_relations;
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
bool application_search_active() noexcept {
    return false;
}
bool application_profile_open(u64, UiRect, ProfileAnchor) noexcept {
    ++full_opens;
    return true;
}
bool application_profile_expand(u64) noexcept {
    return true;
}
bool application_guild_card_open(u64, UiRect) noexcept {
    return true;
}
bool application_open_content_url(Utf8View) noexcept {
    return true;
}
bool application_link_open(Utf8View, bool) noexcept {
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

extern "C" __declspec(noreturn) void WINAPI maicord_dm_profile_tests_entry() noexcept {
    maicord::dm_profile_test::run();
    const bool failed = maicord::dm_profile_test::failures != 0;
    constexpr char pass[] = "PASS DM view chrome\n", fail[] = "FAIL DM view chrome\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}
