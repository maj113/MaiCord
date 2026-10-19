#include "application_profile.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_composer_input.hpp"
#include "application_composer_text_input.hpp"
#include "application_content_input.hpp"
#include "application_friends.hpp"
#include "application_guild_card.hpp"
#include "application_message_interaction.hpp"
#include "application_plugins.hpp"
#include "application_private_conversation.hpp"
#include "application_profile_editor.hpp"
#include "application_scrolling.hpp"
#include "application_user_actions.hpp"
#include "maicord/app/account_guild_admin.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/clipboard.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::profile_runtime_test {
using namespace app_detail;
u32 failures, mutations, reads;
u8 read_operation;
u64 read_user, read_guild, request_id;
RestClient rest;
RestClientSubmitError admission;
RestRouteKey route;
u8 target[128], body[2048];
u32 target_bytes, body_bytes;
char read_value[64];
u32 read_value_bytes;
char opened_url[128], content_url[128];
u32 opened_url_bytes, opened, content_url_bytes, content_opened;
bool opened_trusted;
GuildRecord guild;
u16 guild_order;
RoleRecord roles[21];
u8 names[21 * 8];
PopupItem popup_items[100];
PopupRowWidget popup_rows[100];
UiWidget popup_widgets[210];
UiLayoutNode popup_nodes[210];

void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
bool same(ConstSlice<u8> bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(bytes.data), bytes.count}, expected);
}
RestClientSubmitResult WINAPI submit_roles(void*, const GuildAdminRequest* request, u64 id) noexcept {
    return account_guild_admin_submit(&rest, request, id);
}
RestClientSubmitResult WINAPI read_profile(void*, u64 user, u64 server, u64 id, u8 operation,
                                           Utf8View value) noexcept {
    ++reads;
    read_user = user;
    read_guild = server;
    read_operation = operation;
    request_id = id;
    read_value_bytes = value.bytes <= sizeof(read_value) ? value.bytes : 0;
    copy_bytes(read_value, value.data, read_value_bytes);
    // Activity reads (5 and 6) carry IDs or a session; the others nothing.
    check(operation >= 5 || !value.bytes, "profile runtime: profile and effect reads carry no mutation payload");
    // The real account bridge accepts profile operations 0..6 only.
    return {operation <= 6 ? RestClientSubmitError::none : RestClientSubmitError::invalid_input};
}
bool click(ProfileAction action, u64 id = 0) noexcept {
    auto& scene = g_app.profile->scene;
    for (u32 i = 0; i < scene.part_count; ++i) {
        const auto& part = scene.parts[i];
        if (part.action != action || (id && part.target != id))
            continue;
        const auto* node = ui_widget_manager_layout_node(scene.widgets, part.widget);
        if (!node)
            return false;
        const UiPoint center{(node->rectangle.left + node->rectangle.right) * .5f,
                             (node->rectangle.top + node->rectangle.bottom) * .5f};
        const auto hit = profile_scene_hit_test(scene, center.x, center.y);
        if (hit.slot != part.widget.slot || hit.generation != part.widget.generation)
            return false;
        return application_profile_pointer(UiButtonPointerEvent::press, center) &&
               application_profile_pointer(UiButtonPointerEvent::release, center);
    }
    return false;
}
bool click_badge(u16 source) noexcept {
    auto& scene = g_app.profile->scene;
    for (u32 i = 0; i < scene.part_count; ++i) {
        const auto& part = scene.parts[i];
        if (part.media != ProfileMediaKind::badge || part.source != source)
            continue;
        const auto* node = ui_widget_manager_layout_node(scene.widgets, part.widget);
        if (!node)
            return false;
        const UiPoint center{(node->rectangle.left + node->rectangle.right) * .5f,
                             (node->rectangle.top + node->rectangle.bottom) * .5f};
        return profile_scene_action(scene, profile_scene_hit_test(scene, center.x, center.y)) ==
                   ProfileAction::badge &&
               application_profile_pointer(UiButtonPointerEvent::press, center) &&
               application_profile_pointer(UiButtonPointerEvent::release, center);
    }
    return false;
}
bool choose_add() noexcept {
    if (!click(ProfileAction::choose_roles) || !g_app.popup_scene.active)
        return false;
    auto& p = *g_app.profile;
    for (u32 i = 0; i < p.menu_role_count; ++i)
        if (p.menu_roles[i] == 200)
            return application_profile_menu_activate(p.menu_items[i].action);
    return false;
}
bool retry() noexcept {
    auto& scene = g_app.profile->scene;
    for (u32 i = 0; i < scene.part_count; ++i) {
        const auto& part = scene.parts[i];
        if (part.action == ProfileAction::retry)
            return ui_widget_manager_focus(&scene.widgets, part.widget) &&
                   application_profile_key(VK_RETURN, true, false) &&
                   application_profile_key(VK_RETURN, false, false);
    }
    return false;
}
void response(u16 status, Utf8View json = {}) noexcept {
    RestClientResponse reply{};
    reply.request_id = request_id;
    reply.status_code = status;
    // The DETAILS dispatcher releases the lane before the controller reply.
    g_app.details_request_owner = MessageHistoryRequestOwner::none;
    g_app.details_request_id = 0;
    check(application_profile_response(reply, json),
          "profile runtime: actual controller handles its response");
}
constexpr auto kBefore = utf8_literal(
    R"({"user":{"id":"42","username":"fixture"},"guild_member":{"roles":["100","101","102","103","104","105","106","107","108","109","110","111","112","113","114","115","116","117"]}})");
constexpr auto kAfter = utf8_literal(
    R"({"user":{"id":"42","username":"fixture"},"guild_member":{"roles":["100","101","102","103","104","105","106","107","108","109","110","111","112","113","114","115","116","117","200"]}})");
bool role_request(RestMethod method) noexcept {
    return same({target, target_bytes}, utf8_literal("/api/v10/guilds/7/members/42/roles/200")) &&
           !body_bytes && route.method == method && route.major == RestMajorParameter::guild &&
           route.major_key == 7;
}
bool setup() noexcept {
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    g_app.profile = static_cast<ApplicationProfile*>(
        VirtualAlloc(nullptr, sizeof(ApplicationProfile), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.account_ui || !g_app.profile)
        return false;
    auto& state = g_app.account_ui->chat_state;
    state.guilds = &guild;
    state.guild_order = &guild_order;
    state.guild_count = 1;
    state.roles = roles;
    state.role_count = 21;
    state.names = names;
    state.name_bank_bytes = sizeof(names);
    state.self_user_id = 99;
    state.selected_guild_id = 7;
    guild.id = 7;
    guild.owner_id = 1;
    guild.permissions_resolved = guild.member_roles_known = true;
    guild.base_permissions = kPermissionManageRoles;
    for (u32 i = 0; i < 21; ++i) {
        roles[i].id = i == 0 ? 7 : i < 19 ? 99 + i : i == 19 ? 200 : 300;
        roles[i].guild_id = 7;
        roles[i].position = i;
        roles[i].name_offset = i * 8;
        roles[i].name_bytes = 4;
        copy_bytes(names + i * 8, "Role", 4);
    }
    roles[20].self_assigned = true;
    g_app.account.api.guild_admin = submit_roles;
    g_app.account.api.profile_get = read_profile;
    auto& p = *g_app.profile;
    if (!g_app.account_ui->profile_decode.decode_into(kBefore, 42, 7, p.data) ||
        !popup_scene_initialize(&g_app.popup_scene, {popup_items, 100}, {popup_rows, 100},
                                {popup_widgets, 210}, {popup_nodes, 210}))
        return false;
    p.scene.phase = ProfilePhase::ready;
    p.scene.reduced_motion = true;
    return profile_scene_open(&p.scene, &p.data, &state, &g_app.account_ui->user_store, 99,
                              {700, 100, 720, 120}, application_viewport_dips());
}
void run() noexcept {
    const bool initialized = setup();
    check(initialized, "profile runtime: real profile scene opens with an eighteen-role member snapshot");
    if (!initialized)
        return;
    auto& p = *g_app.profile;
    check(choose_add() && p.pending && p.scene.roles_busy && p.mutation == ProfileRoleChange::add,
          "profile runtime: pointer picker activation queues the selected role");
    admission = RestClientSubmitError::busy;
    check(application_profile_submit(100) && mutations == 1 && reads == 0 && p.pending &&
              p.retry_at == 200 && g_app.details_request_owner == MessageHistoryRequestOwner::none &&
              !application_profile_submit(199),
          "profile runtime: a busy role request keeps its intent and waits for admission retry");
    admission = RestClientSubmitError::none;
    check(application_profile_submit(200) && mutations == 2 && reads == 0 && !p.pending &&
              role_request(RestMethod::put) &&
              g_app.details_request_owner == MessageHistoryRequestOwner::user_profile,
          "profile runtime: picker adds only the selected role without replacing eighteen held roles");
    response(204);
    check(p.pending && !p.scene.roles_busy && p.mutation == ProfileRoleChange::none &&
              application_profile_submit(201) && reads == 1 && read_operation == 0 && read_user == 42 &&
              read_guild == 7,
          "profile runtime: accepted roles trigger the normal profile GET on the same member and guild");
    response(200, kAfter);
    check(p.scene.phase == ProfilePhase::ready && p.data.role_count == 19 && p.data.roles[18] == 200 &&
              click(ProfileAction::remove_role, 200) && application_profile_submit(202) && mutations == 3 &&
              role_request(RestMethod::delete_method),
          "profile runtime: refreshed profile exposes a working remove hit and preserves every other role");
    response(403);
    check(p.scene.phase == ProfilePhase::failed && p.data.role_count == 19 && !p.scene.roles_busy &&
              p.mutation == ProfileRoleChange::none && !p.mutation_role && retry() &&
              application_profile_submit(203) && mutations == 3 && read_operation == 0,
          "profile runtime: rejected mutation keeps the snapshot and retry refreshes authoritative roles");
    response(200, kAfter);
    check(click(ProfileAction::remove_role, 200) && application_profile_submit(204) && mutations == 4 &&
              role_request(RestMethod::delete_method),
          "profile runtime: removal remains usable after a failed request and refresh");
    response(204);
    check(application_profile_submit(205) && read_operation == 0,
          "profile runtime: successful removal refetches the authoritative profile");
    response(200, kBefore);
    check(p.scene.phase == ProfilePhase::ready && p.data.role_count == 18 && choose_add(),
          "profile runtime: reconciled removal makes the role assignable again");
    admission = RestClientSubmitError::invalid_input;
    check(application_profile_submit(206) && !p.pending && !p.scene.roles_busy &&
              p.scene.phase == ProfilePhase::failed && p.mutation == ProfileRoleChange::none &&
              !p.mutation_role,
          "profile runtime: local submission refusal clears busy and exposes recovery");
    admission = RestClientSubmitError::none;
    check(retry() && application_profile_submit(207) && read_operation == 0,
          "profile runtime: local refusal can refresh the profile without reopening");
    response(200, kBefore);
    check(choose_add(), "profile runtime: picker remains usable after local rejection");
    const u32 sent = mutations;
    guild.base_permissions = 0;
    check(application_profile_submit(208) && mutations == sent && !p.pending && !p.scene.roles_busy,
          "profile runtime: permission revoked after clicking blocks the role request before transport");
    guild.base_permissions = kPermissionManageRoles;
    p.scene.phase = ProfilePhase::ready;
    p.data.role_count = 128;
    profile_scene_layout(&p.scene, p.scene.viewport, true);
    check(
        choose_add() && application_profile_submit(209) && mutations == sent + 1 &&
            role_request(RestMethod::put),
        "profile runtime: a capped role snapshot changes only the selected role, preserving unknown roles");
    response(204);
    check(application_profile_submit(210), "profile runtime: capped snapshot mutation refreshes normally");
    response(200, utf8_literal(R"({"user":{"id":"42","username":"fixture"}})"));
    check(p.data.role_count == 0 && choose_add() && application_profile_submit(211) &&
              mutations == sent + 2 && role_request(RestMethod::put),
          "profile runtime: omitted member roles never cause a whole-list replacement");
    response(204);
    check(application_profile_submit(212), "profile runtime: omitted-role mutation refetches normally");
    response(200, kBefore);
    p.effect_pending = true;
    p.data.effect_sku_id = 555;
    check(application_profile_submit(213) && read_operation == 4 && read_user == 555,
          "profile runtime: effect metadata continues to use its existing profile operation");
    response(404);

    // A card without artwork asks once for its application's icon; a custom
    // button fetches its URL from the activity's metadata.
    constexpr auto presence = utf8_literal(R"({"activities":[{"type":0,"name":"Mossrun","application_id":"55",
        "session_id":"abc","buttons":["Open"]}]})");
    u32 structurals[512];
    UserActivitySet set{};
    bool has = false;
    const auto scan = json_scan_structurals_avx1(presence, {structurals, 512});
    auto& users = g_app.account_ui->user_store;
    const bool stored = scan.ok() &&
                        user_activities_decode({presence, {structurals, scan.count}},
                                               {presence, 0, JsonValueKind::object}, 42, &set, &has) &&
                        user_activity_upsert(&users.activities, set);
    ++users.revision;
    profile_scene_layout(&p.scene, p.scene.viewport, true);
    check(stored && p.scene.missing_icon_count == 1 && application_profile_submit(214) && read_operation == 5 &&
              utf8_equal({read_value, read_value_bytes}, utf8_literal("55")),
          "profile runtime: a card without art fetches its public application");
    response(200, utf8_literal(R"([{"id":"55","name":"Mossrun","icon":"beef"}])"));
    check(p.application_count == 1 && p.applications[0].id == 55 &&
              utf8_equal(p.applications[0].icon.view(), utf8_literal("beef")) && !p.scene.missing_icon_count &&
              !application_profile_submit(215),
          "profile runtime: the icon is remembered and not asked for again");
    check(click(ProfileAction::activity_button) && p.metadata_pending && application_profile_submit(216) &&
              read_operation == 6 && read_user == 42 && read_guild == 55 &&
              utf8_equal({read_value, read_value_bytes}, utf8_literal("abc")),
          "profile runtime: a custom button asks for the activity's metadata");
    response(200, utf8_literal(R"({"button_urls":["https://example.invalid/open"]})"));
    check(!p.metadata_pending && opened == 1 && !opened_trusted &&
              utf8_equal({opened_url, opened_url_bytes}, utf8_literal("https://example.invalid/open")),
          "profile runtime: the button opens its URL through the link warning");
    // A badge with a link opens it through the content link rules; one
    // without does nothing, and the card stays open.
    constexpr auto badges = utf8_literal(
        R"({"user":{"id":"42","username":"fixture"},"badges":[)"
        R"({"id":"premium","description":"Subscriber since Jan 1, 2020","icon":"aa","link":"https://discord.com/settings/premium"},)"
        R"({"id":"legacy_username","description":"Originally known as fixture#0001","icon":"bb"}]})");
    const bool laid = g_app.account_ui->profile_decode.decode_into(badges, 42, 7, p.data) &&
                      profile_scene_layout(&p.scene, p.scene.viewport, true);
    const u32 links = opened;
    check(laid && click_badge(0) && content_opened == 1 && opened == links &&
              utf8_equal({content_url, content_url_bytes}, utf8_literal("https://discord.com/settings/premium")) &&
              p.scene.active,
          "profile runtime: a badge click opens its link through the content link rules");
    check(laid && click_badge(1) && content_opened == 1 && p.scene.active,
          "profile runtime: a badge without a link opens nothing");
    application_profile_release();
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
}
} // namespace maicord::profile_runtime_test

namespace maicord {
ClipboardTextResult clipboard_read_utf8(HWND, Slice<char>) noexcept {
    profile_runtime_test::check(false, "profile runtime: role actions do not read the clipboard");
    return {0, ClipboardTextError::unavailable};
}
bool clipboard_write_utf8(HWND, Utf8View) noexcept {
    profile_runtime_test::check(false, "profile runtime: role actions do not write the clipboard");
    return false;
}
RestClientSubmitResult rest_client_submit(RestClient*, const RestClientRequest& request) noexcept {
    using namespace profile_runtime_test;
    ++mutations;
    route = request.route;
    request_id = request.request_id;
    target_bytes = request.target.count;
    body_bytes = request.body.count;
    if (target_bytes > sizeof(target) || body_bytes > sizeof(body))
        return {RestClientSubmitError::invalid_input};
    copy_bytes(target, request.target.data, target_bytes);
    copy_bytes(body, request.body.data, body_bytes);
    return {admission};
}
RestClientSubmitResult rest_client_submit_large(RestClient* client,
                                                const RestClientRequest& request) noexcept {
    return rest_client_submit(client, request);
}
RestClientSubmitResult rest_client_submit_large_response(RestClient* client,
                                                         const RestClientRequest& request, u32) noexcept {
    return rest_client_submit(client, request);
}
bool rest_client_cancel(RestClient*, u64) noexcept {
    return true;
}
bool text_edit_layout_hit_test(const TextEditLayoutOwner&, float, float, u32*) noexcept {
    return false;
}
namespace app_detail {
UiRect application_viewport_dips() noexcept {
    return {0, 0, 1280, 900};
}
bool refresh_account_text(bool) noexcept {
    return true;
}
bool refresh_popup_text() noexcept {
    return true;
}
void set_popup_owner(PopupOwnerKind kind) noexcept {
    g_app.popup_owner_kind = kind;
}
bool dismiss_popup() noexcept {
    popup_scene_close(&g_app.popup_scene);
    set_popup_owner(PopupOwnerKind::none);
    return true;
}
void application_composer_text_input_suspend() noexcept {
}
bool application_composer_text_input_composing() noexcept {
    return false;
}
void end_message_composer_pointer_selection() noexcept {
}
void end_message_text_selection() noexcept {
}
void application_surface_motion_retire(RendererLayerSurface) noexcept {
}
bool application_friends_user_relation(u64, u8*) noexcept {
    return false;
}
bool application_friends_user_action(u64, ApplicationFriendsUserAction) noexcept {
    return false;
}
const RelationshipStore* application_friends_relationships() noexcept {
    return nullptr;
}
bool application_profile_editor_open(ProfileEditorTab, u64) noexcept {
    return false;
}
bool application_user_menu_open(u64, UiRect, u64, UserMenuSource, ConstSlice<PopupItem>,
                                const UiPopupFeedback*) noexcept {
    return false;
}
Utf8View application_user_note(u64, bool* loading) noexcept {
    if (loading)
        *loading = false;
    return {};
}
bool application_user_note_edit(u64, Utf8View) noexcept {
    return true;
}
void application_user_note_flush(u64) noexcept {
}
bool application_guild_card_open(u64, UiRect) noexcept {
    return false;
}
bool application_plugins_slot_activate(u64, NativePluginSdkSlotTarget, u64, UiRect) noexcept {
    return false;
}
bool application_open_content_url(Utf8View url) noexcept {
    using namespace profile_runtime_test;
    ++content_opened;
    content_url_bytes = url.bytes <= sizeof(content_url) ? url.bytes : 0;
    copy_bytes(content_url, url.data, content_url_bytes);
    return true;
}
bool application_link_open(Utf8View url, bool trusted) noexcept {
    using namespace profile_runtime_test;
    ++opened;
    opened_trusted = trusted;
    opened_url_bytes = url.bytes <= sizeof(opened_url) ? url.bytes : 0;
    copy_bytes(opened_url, url.data, opened_url_bytes);
    return true;
}
bool select_content_guild(u64) noexcept {
    return false;
}
bool queue_private_conversation(PrivateConversationOperation, u64) noexcept {
    return false;
}
bool queue_profile_scroll(bool, i32) noexcept {
    return false;
}
void update_scrollbar_hover(float, float) noexcept {
}
} // namespace app_detail
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_profile_runtime_entry() noexcept {
    maicord::profile_runtime_test::run();
    ExitProcess(maicord::profile_runtime_test::failures ? 1 : 0);
}
