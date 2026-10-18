// Shipping host with fake account admission and renderer bindings. No live
// account, network, GPU or shipping diagnostic hooks are involved.
#include "application_profile_editor.hpp"
#include "application_shell_input.hpp"
#include "../src/app/application_channel_overview.cpp"
#include "maicord/ui/clipboard.hpp"
#include "test_output.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/sys/winsock.hpp"

namespace maicord::channel_overview_runtime_test {
using namespace app_detail;
u32 failures, submissions, discard_prompts;
ApplicationTextFormRequest discard_request;
u64 form_marker;
RestClientSubmitError admission{};
u64 submitted_id;
ChannelOverviewOperation submitted_operation;
ChannelOverviewPatch submitted_patch;
char submitted_name[kChannelOverviewNameBytes], submitted_topic[kChannelOverviewTopicBytes];
ChannelRecord channel;
u16 order[1];
RoleRecord role;
PermissionOverwriteRecord overwrite;
constexpr auto initial = utf8_literal(
    R"({"id":"7","guild_id":"1","type":0,"name":"general","topic":"Initial topic","rate_limit_per_user":0,"nsfw":false})");
constexpr auto remote = utf8_literal(
    R"({"id":"7","guild_id":"1","type":0,"name":"remote","topic":"Initial topic","rate_limit_per_user":0,"nsfw":false})");
constexpr auto compatible_remote = utf8_literal(
    R"({"id":"7","guild_id":"1","type":0,"name":"x\u2028\u2029y","topic":"a\t\r\u0085b","rate_limit_per_user":0,"nsfw":false})");
void check(bool value, const char* label) noexcept {
    test_check(&failures, value, label);
}
RestClientSubmitResult WINAPI submit(void* context, ChannelOverviewOperation operation, u64 id,
                                     const ChannelOverviewPatch* patch, u64 request_id) noexcept {
    check(context == &channel && id == 7 && request_id && owner &&
              owner->controller.scope.account_scope == 7 && owner->controller.scope.profile_id == 4 &&
              owner->controller.scope.self_user_id == 50 &&
              ((operation == ChannelOverviewOperation::fetch && !patch) ||
               (operation == ChannelOverviewOperation::update && patch &&
                channel_overview_patch_valid(*patch))),
          "Overview runtime: account adapter admits only exact scoped identity and operation projection");
    ++submissions;
    submitted_id = request_id;
    submitted_operation = operation;
    submitted_patch = {};
    if (patch) {
        submitted_patch = *patch;
        copy_bytes(submitted_name, patch->name.data, patch->name.bytes);
        copy_bytes(submitted_topic, patch->topic.data, patch->topic.bytes);
        submitted_patch.name = {submitted_name, patch->name.bytes};
        submitted_patch.topic = {submitted_topic, patch->topic.bytes};
    }
    return {admission};
}
bool response(u64 id, Utf8View body = initial, u16 status = 200) noexcept {
    RestClientResponse value{};
    value.request_id = id;
    value.status_code = status;
    value.body_bytes = body.bytes;
    const bool consumed = application_channel_overview_response(value, body);
    // Production details pump clears routing only for its matching receipt.
    if (consumed && g_app.details_request_id == id) {
        g_app.details_request_owner = MessageHistoryRequestOwner::none;
        g_app.details_request_id = 0;
    }
    return consumed;
}
bool send() noexcept {
    return application_channel_overview_submit(
        maximum(clock_monotonic_milliseconds(), owner ? owner->controller.retry_at_ms : 0));
}
void focus(UiSettingsTarget target) noexcept {
    settings_layer_focus(&owner->layer, target);
}
void activate(UiSettingsTarget target) noexcept {
    if (owner)
        settings_layer_activate_target(&owner->layer, target);
}
bool edit_name(Utf8View value) noexcept {
    focus({UiSettingsTargetKind::control, page_key, 1});
    const auto target = application_channel_overview_edit_target();
    u32 length = 0;
    TextInputChange change{};
    u16 converted[200]{};
    u32 count = 0;
    for (u32 offset = 0; offset < value.bytes;) {
        const auto scalar = utf8_decode_one(value.data + offset, value.bytes - offset);
        if (!scalar.valid)
            return false;
        if (scalar.codepoint > 0xffff) {
            const u32 c = scalar.codepoint - 0x10000;
            converted[count++] = static_cast<u16>(0xd800 + (c >> 10));
            converted[count++] = static_cast<u16>(0xdc00 + (c & 1023));
        } else
            converted[count++] = static_cast<u16>(scalar.codepoint);
        offset += scalar.width;
    }
    return text_input_length(target, &length) == TextInputResult::unchanged &&
           text_input_replace(target, 0, length, {converted, count}, &change) == TextInputResult::changed &&
           application_channel_overview_edit_changed(true);
}
void permit(bool value = true) noexcept {
    channel.permissions_resolved = true;
    channel.effective_permissions =
        kPermissionViewChannel | (value ? kPermissionManageChannels : 0);
    channel.actions = channel_action_view | (value ? channel_action_manage_channel : 0);
}
void run() noexcept {
    BYTE saved[256]{}, clean[256]{};
    const bool keyboard_saved = GetKeyboardState(saved) != FALSE;
    check(keyboard_saved && SetKeyboardState(clean),
          "Overview runtime: controlled native keyboard modifiers");
    g_app.window = CreateWindowExW(0, L"STATIC", L"Overview isolated hidden fixture", WS_POPUP, 0, 0, 1200,
                                   900, nullptr, nullptr, nullptr, nullptr);
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.window && g_app.account_ui,
          "Overview runtime: isolated native host and account owner initialize");
    if (!g_app.window || !g_app.account_ui)
        return;
    channel.id = 7;
    channel.guild_id = 1;
    channel.type = 0;
    channel.generation = 3;
    channel.revision = 1;
    channel.occupied = true;
    permit();
    auto& state = g_app.account_ui->chat_state;
    state.channels = &channel;
    state.channel_order = order;
    state.channel_count = state.channel_capacity = 1;
    state.roles = &role;
    state.overwrites = &overwrite;
    state.self_user_id = 50;
    g_app.plugin_account_scope_serial = 7;
    g_app.account.status.profile_id = 4;
    g_app.account.status.running = true;
    g_app.account.instance = &channel;
    g_app.account.api.channel_overview = submit;
    g_app.renderer_ready = true;
    g_app.renderer.width = 1200;
    g_app.renderer.height = 900;
    g_app.renderer.dpi = 96;
    g_app.keyboard_focus_visible = true;
    permit(false);
    check(!application_channel_overview_open(7) && !owner && !submissions,
          "Overview runtime: cached permission denial cannot allocate or submit management owner");
    permit();
    check(application_channel_overview_open(7) && owner && application_channel_overview_active() &&
              owner->controller.phase == ChannelOverviewPhase::loading &&
              !application_channel_overview_edit_target(),
          "Overview runtime: authorized open remains loading and unavailable to text input until fetch");
    if (!owner)
        return;
    const u64 first_lifetime = owner->lifetime;
    admission = RestClientSubmitError::busy;
    check(send() && owner->controller.queued && !owner->controller.request_id &&
              owner->controller.retry_at_ms &&
              g_app.details_request_owner == MessageHistoryRequestOwner::none,
          "Overview runtime: busy admission retains queued fetch without claiming details ticket");
    admission = RestClientSubmitError::none;
    check(send() && owner->controller.request_id == submitted_id &&
              g_app.details_request_owner == MessageHistoryRequestOwner::channel_overview &&
              !response(submitted_id + 1) && response(submitted_id) && owner->controller.authoritative &&
              owner->models[0].text == owner->controller.draft.name &&
              owner->models[1].text == owner->controller.draft.topic,
          "Overview runtime: exact fetch receipt publishes authoritative borrowed editor banks");
    focus({UiSettingsTargetKind::control, page_key, 1});
    check(text_input_select(application_channel_overview_edit_target(), {7, 7}) ==
                  TextInputResult::changed &&
              application_channel_overview_input(UiInputKind::character, 'x', false) &&
              application_channel_overview_input(UiInputKind::character, 0xd83d, false) &&
              application_channel_overview_input(UiInputKind::character, 0xde80, false) &&
              application_channel_overview_input(UiInputKind::character, 'q', false) &&
              utf8_equal(channel_overview_name_view(owner->controller.draft),
                         utf8_literal("generalx\xF0\x9F\x9A\x80"
                                      "q")),
          "Overview runtime: actual character and surrogate callbacks translate byte selection to native "
          "ACP after supplementary input");
    check(edit_name(utf8_literal("renamed")) && owner->controller.dirty_fields == channel_overview_name &&
              utf8_equal(channel_overview_name_view(owner->controller.baseline), utf8_literal("general")) &&
              utf8_equal(channel_overview_name_view(owner->controller.draft), utf8_literal("renamed")),
          "Overview runtime: actual bounded text callback marks alias-edited draft dirty against baseline");
    const auto name_target = application_channel_overview_edit_target();
    focus({UiSettingsTargetKind::control, page_key, 2});
    check(!text_input_ready(name_target) && text_input_ready(application_channel_overview_edit_target()) &&
              application_channel_overview_edit_model() == &owner->models[1],
          "Overview runtime: switching inline field retires old native text ownership without changing its "
          "bytes");
    focus({UiSettingsTargetKind::control, page_key, 1});
    auto old_target = application_channel_overview_edit_target();
    activate({UiSettingsTargetKind::save, page_key, 0});
    check(owner->controller.queued && !application_channel_overview_edit_target() && send() &&
              submitted_operation == ChannelOverviewOperation::update &&
              submitted_patch.fields == channel_overview_name &&
              utf8_equal(submitted_patch.name, utf8_literal("renamed")),
          "Overview runtime: explicit Save locks controls and submits only changed authoritative fields");
    check(response(submitted_id, {}, 400) && owner->controller.phase == ChannelOverviewPhase::failed &&
              owner->controller.dirty_fields == channel_overview_name &&
              utf8_equal(channel_overview_name_view(owner->controller.draft), utf8_literal("renamed")) &&
              !text_input_ready(old_target),
          "Overview runtime: rejected server save retains draft and retires old native document target");
    activate({UiSettingsTargetKind::reset, page_key, 0});
    check(!owner->controller.dirty_fields && owner->models[0].bytes == 7 &&
              utf8_equal(message_composer_text(owner->models[0]), utf8_literal("general")),
          "Overview runtime: Reset adopts baseline into borrowed model without clearing fetched bytes");
    check(edit_name(utf8_literal("local")),
          "Overview runtime: independent local draft prepares conflict fixture");
    GatewayClientEvent update{};
    update.kind = GatewayClientEventKind::dispatch;
    update.dispatch_kind = GatewayClientDispatchKind::channel_update;
    update.state.id = 7;
    update.state.guild_id = 1;
    application_channel_overview_gateway(update, {});
    check(!owner->controller.authoritative && send() && response(submitted_id, remote) &&
              owner->controller.phase == ChannelOverviewPhase::conflict && !owner->footer.can_save &&
              utf8_equal(channel_overview_name_view(owner->controller.draft), utf8_literal("local")),
          "Overview runtime: gateway invalidation refetches then exposes conflict while preserving local "
          "field");
    activate({UiSettingsTargetKind::control, page_key, 5});
    check(owner->controller.phase == ChannelOverviewPhase::ready && owner->controller.dirty_fields &&
              edit_name(utf8_literal("local-reviewed")),
          "Overview runtime: explicit draft review rebases without implicit mutation request");
    activate({UiSettingsTargetKind::save, page_key, 0});
    admission = RestClientSubmitError::invalid_input;
    check(send() && !owner->controller.queued &&
              owner->controller.failure != ChannelOverviewFailure::none &&
              !application_channel_overview_next_wake() && owner->controller.dirty_fields,
          "Overview runtime: permanent synchronous admission failure preserves draft without immediate "
          "retry trap");
    admission = RestClientSubmitError::none;
    activate({UiSettingsTargetKind::save, page_key, 0});
    check(send() && response(submitted_id, {}, 403) && owner->controller.permission_denied &&
              application_channel_overview_synchronize() && !owner->controller.can_manage &&
              !application_channel_overview_edit_target() && !application_channel_overview_next_wake(),
          "Overview runtime: server 403 overrides unchanged cached permission proof and stops edit "
          "deadlines");
    GatewayClientEvent permission_event{};
    permission_event.kind = GatewayClientEventKind::dispatch;
    permission_event.dispatch_kind = GatewayClientDispatchKind::guild_role_update;
    permission_event.state.guild_id = 2;
    permission_event.state.fields = gateway_client_state_field_permissions;
    application_channel_overview_gateway(permission_event, {});
    check(application_channel_overview_synchronize() && !owner->controller.can_manage,
          "Overview runtime: unrelated guild permission event cannot clear known server denial");
    permission_event.state.guild_id = 1;
    permission_event.state.fields = gateway_client_state_field_name;
    application_channel_overview_gateway(permission_event, {});
    check(
        application_channel_overview_synchronize() && !owner->controller.can_manage &&
            owner->controller.permission_denied && !application_channel_overview_next_wake(),
        "Overview runtime: name-only role receipt cannot clear server denial using stale permission cache");
    permission_event.state.fields = gateway_client_state_field_permissions;
    application_channel_overview_gateway(permission_event, {});
    check(application_channel_overview_synchronize() && owner->controller.can_manage && send() &&
              response(submitted_id, remote),
          "Overview runtime: explicit fresh permission observation can reauthorize through authoritative "
          "refetch");
    activate({UiSettingsTargetKind::reset, page_key, 0});
    UiSettingsControlGeometry nsfw{};
    const UiSettingsTarget nsfw_target{UiSettingsTargetKind::control, page_key, 4};
    check(ui_settings_scene_control_geometry(owner->scene, nsfw_target, &nsfw),
          "Overview runtime: age-restricted field has genuine native clipped value hit geometry");
    const LPARAM point = MAKELPARAM(static_cast<i16>((nsfw.field.left + nsfw.field.right) * .5f),
                                    static_cast<i16>((nsfw.field.top + nsfw.field.bottom) * .5f));
    const bool before = owner->controller.draft.nsfw;
    application_channel_overview_window_message(WM_LBUTTONDOWN, 0, point);
    application_channel_overview_window_message(WM_LBUTTONUP, 0, point);
    check(
        owner->controller.draft.nsfw != before && (owner->controller.dirty_fields & channel_overview_nsfw),
        "Overview runtime: actual mouse capture survives focus repaint and activates toggle exactly once");
    request_close();
    check(!discard_prompts && !g_app.text_form && application_channel_overview_active() &&
              owner->layer.shake_started && owner->footer.visible,
          "Overview runtime: closing with unsaved changes shakes the save bar and keeps the editor open");
    application_channel_overview_tick(owner->layer.shake_started + 30);
    check(owner->scene.footer_shift_q != 0 && owner->scene.footer_alert_q > 0,
          "Overview runtime: the shake moves and tints the unsaved-changes bar");
    application_channel_overview_tick(owner->layer.shake_started + 2'000);
    check(!owner->layer.shake_started && owner->scene.footer_shift_q == 0 && owner->scene.footer_alert_q == 0,
          "Overview runtime: the bar settles after the shake");
    // As a dialog, a click on the scrim closes through the same path as the
    // close button: unsaved changes shake the bar, a clean editor closes.
    ui_settings_scene_set_presentation(&owner->scene, UiSettingsPresentation::dialog);
    const auto modal = owner->scene.layout.modal;
    const bool dialog = application_channel_overview_synchronize() && !owner->scene.layout.compact &&
                        owner->scene.layout.modal.left > owner->scene.viewport.left + 8 &&
                        (modal.left != owner->scene.layout.modal.left || modal.right != owner->scene.layout.modal.right);
    const LPARAM scrim = MAKELPARAM(static_cast<i16>(owner->scene.viewport.left + 4),
                                    static_cast<i16>((owner->scene.layout.modal.top + owner->scene.layout.modal.bottom) * .5f));
    const LPARAM inside = MAKELPARAM(static_cast<i16>((owner->scene.layout.modal.left + owner->scene.layout.modal.right) * .5f),
                                     static_cast<i16>(owner->scene.layout.modal.top + 8));
    application_channel_overview_window_message(WM_LBUTTONDOWN, 0, scrim);
    application_channel_overview_window_message(WM_LBUTTONUP, 0, inside);
    const bool dragged_open = application_channel_overview_active() && !owner->layer.shake_started;
    application_channel_overview_window_message(WM_LBUTTONDOWN, 0, scrim);
    application_channel_overview_window_message(WM_LBUTTONUP, 0, scrim);
    check(dialog && dragged_open && application_channel_overview_active() && owner->layer.shake_started,
          "Overview runtime: a scrim click on a dirty dialog shakes the bar and keeps it open");
    application_channel_overview_tick(owner->layer.shake_started + 2'000);
    activate({UiSettingsTargetKind::reset, page_key, 0});
    application_channel_overview_window_message(WM_LBUTTONDOWN, 0, scrim);
    application_channel_overview_window_message(WM_LBUTTONUP, 0, scrim);
    check(!application_channel_overview_active() && g_app.context_focus == &g_app.shell_scene.widgets,
          "Overview runtime: a clean editor closes from the scrim and restores channel pane focus");
    ui_settings_scene_set_presentation(&owner->scene, UiSettingsPresentation::layer);
    check(application_channel_overview_open(7) && send() && response(submitted_id, remote),
          "Overview runtime: reopened owner fetches independently");
    const u64 opening = owner->lifetime;
    activate({UiSettingsTargetKind::control, page_key, 5});
    check(send(), "Overview runtime: outstanding fetch establishes cancellation lifetime fixture");
    const u64 cancelled = submitted_id;
    application_channel_overview_close(false);
    check(
        !application_channel_overview_active() && !application_channel_overview_open(7) &&
            !response(cancelled + 1) && response(cancelled) && application_channel_overview_open(7) &&
            owner->lifetime > opening && owner->lifetime > first_lifetime,
        "Overview runtime: close fences outstanding exact receipt before a new owner lifetime can reopen");
    check(send(), "Overview runtime: reopened owner issues independent authoritative fetch");
    const u64 changed_scope_receipt = submitted_id;
    ++g_app.plugin_account_scope_serial;
    check(application_channel_overview_synchronize() && !application_channel_overview_active() &&
              response(changed_scope_receipt) && !owner->controller.active,
          "Overview runtime: account scope change retires UI while exact old receipt remains drainable");
    --g_app.plugin_account_scope_serial;
    check(application_channel_overview_open(7) && send() && response(submitted_id, compatible_remote) &&
              !owner->controller.dirty_fields &&
              utf8_equal(message_composer_text(owner->models[0]), utf8_literal("x\xE2\x80\xA8\xE2\x80\xA9"
                                                                               "y")) &&
              utf8_equal(message_composer_text(owner->models[1]), utf8_literal("a\t\r\xC2\x85"
                                                                               "b")),
          "Overview runtime: independent scope can reopen after retired receipt drains");
    focus({UiSettingsTargetKind::control, page_key, 2});
    TextInputChange compatibility_change{};
    check(text_input_select(application_channel_overview_edit_target(), {1, 4}) ==
                  TextInputResult::changed &&
              text_input_replace(application_channel_overview_edit_target(), 1, 4, {},
                                 &compatibility_change) == TextInputResult::changed &&
              application_channel_overview_edit_changed(true) &&
              owner->controller.dirty_fields == channel_overview_topic &&
              utf8_equal(channel_overview_topic_view(owner->controller.draft), utf8_literal("ab")),
          "Overview runtime: server TAB CR and C1 topic imports unchanged then native field removes them "
          "explicitly");
    focus({UiSettingsTargetKind::control, page_key, 1});
    check(text_input_select(application_channel_overview_edit_target(), {1, 3}) ==
                  TextInputResult::changed &&
              text_input_replace(application_channel_overview_edit_target(), 1, 3, {},
                                 &compatibility_change) == TextInputResult::changed &&
              application_channel_overview_edit_changed(true) &&
              owner->controller.dirty_fields == (channel_overview_name | channel_overview_topic) &&
              utf8_equal(channel_overview_name_view(owner->controller.draft), utf8_literal("xy")),
          "Overview runtime: authoritative name line separators remain editable until explicit native "
          "removal");
    {
        // A wheel notch over a long dialog page glides through the shared
        // scroll host (intermediate offsets across frames), and jumps when
        // smooth scrolling is off, through the real window-message path.
        const u32 full_height = g_app.renderer.height;
        g_app.renderer.height = 320;
        g_app.clock = clock_initialize();
        g_app.process_start = clock_now() - g_app.clock.frequency;
        g_app.scroll_host = {};
        bool ok = application_channel_overview_synchronize();
        const auto bar = ui_settings_scene_scrollbar(owner->scene, false);
        const auto* body = ui_widget_manager_layout_node(owner->scene.widgets, owner->scene.body);
        ok = ok && body && bar.maximum_scroll > 0;
        if (ok)
            ui_settings_scene_scroll(&owner->scene, false, -owner->scene.content_offset);
        const LPARAM over_body =
            body ? MAKELPARAM(static_cast<int>((body->rectangle.left + body->rectangle.right) * .5f),
                              static_cast<int>((body->rectangle.top + body->rectangle.bottom) * .5f))
                 : 0;
        const WPARAM notch = MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(-120)));
        const bool handled = application_channel_overview_window_message(WM_MOUSEWHEEL, notch, over_body);
        ok = ok && handled && owner->scene.content_offset == 0 && ui_scroll_host_active(g_app.scroll_host);
        const u64 start = clock_microseconds(g_app.clock, g_app.process_start, clock_now());
        float previous = 0;
        u32 intermediate = 0;
        bool changed = false;
        for (u32 frame = 1; ok && frame <= 30 && ui_scroll_host_active(g_app.scroll_host); ++frame) {
            ok = ui_scroll_host_tick(&g_app.scroll_host, start + frame * 16'667ull, &changed);
            const float offset = owner->scene.content_offset;
            ok = ok && offset >= previous;
            intermediate += offset > previous && offset > 0 && ui_scroll_host_active(g_app.scroll_host);
            previous = offset;
        }
        // The landing offset follows the system wheel-lines setting.
        const float target = owner->scene.content_offset;
        check(ok && intermediate >= 3 && target > 0 && !ui_scroll_host_active(g_app.scroll_host),
              "Overview runtime: a wheel notch over the page glides across frames to its target");
        g_app.scroll_host.immediate = true;
        ui_settings_scene_scroll(&owner->scene, false, -owner->scene.content_offset);
        check(application_channel_overview_window_message(WM_MOUSEWHEEL, notch, over_body) &&
                  owner->scene.content_offset == target && !ui_scroll_host_active(g_app.scroll_host),
              "Overview runtime: with smooth scrolling off the same notch jumps at once");
        g_app.scroll_host = {};
        ui_settings_scene_scroll(&owner->scene, false, -owner->scene.content_offset);
        g_app.renderer.height = full_height;
        application_channel_overview_synchronize();
    }
    channel.occupied = false;
    state.channel_count = 0;
    check(application_channel_overview_synchronize() && !application_channel_overview_active() &&
              !application_channel_overview_open(7),
          "Overview runtime: channel deletion retires owner and cached form cannot revive it");
    application_channel_overview_release();
    check(!owner && !g_app.renderer.settings_scene && !application_channel_overview_owner_bytes() &&
              !application_channel_overview_next_wake() && !application_channel_overview_caret_wake(),
          "Overview runtime: release removes owner scene binding and all deadlines");
    DestroyWindow(g_app.window);
    g_app.window = nullptr;
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
    if (keyboard_saved)
        check(SetKeyboardState(saved) != FALSE, "Overview runtime: native keyboard state restores");
}
} // namespace maicord::channel_overview_runtime_test

namespace maicord {
bool renderer_rebuild_content_text(D3D11Renderer* renderer, const Theme&) noexcept {
    renderer->interaction_text_settings_revision =
        renderer->settings_scene ? renderer->settings_scene->revision : 0;
    return true;
}
void renderer_bind_settings_scene(D3D11Renderer* renderer, const UiSettingsScene* scene) noexcept {
    renderer->settings_scene = scene;
}
bool renderer_set_settings_modal_progress(D3D11Renderer* renderer, float progress) noexcept {
    renderer->settings_modal_progress = progress;
    return true;
}
bool text_edit_layout_hit_test(const TextEditLayoutOwner&, float, float, u32* position) noexcept {
    if (!position)
        return false;
    *position = 0;
    return true;
}
bool clipboard_write_utf8(HWND, Utf8View) noexcept {
    return true;
}
ClipboardTextResult clipboard_read_utf8(HWND, Slice<char>) noexcept {
    return {0, ClipboardTextError::unavailable};
}
namespace app_detail {
// Server Settings is never open in this fixture.
bool application_guild_settings_active() noexcept {
    return false;
}
void application_guild_settings_close(bool) noexcept {
}
void application_guild_settings_release() noexcept {
}
bool application_guild_settings_synchronize() noexcept {
    return true;
}
bool application_guild_settings_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_guild_settings_window_message(UINT, WPARAM, LPARAM) noexcept {
    return false;
}
bool application_guild_settings_submit(u64) noexcept {
    return false;
}
bool application_guild_settings_owns_response(u64) noexcept {
    return false;
}
bool application_guild_settings_response(const RestClientResponse&, Utf8View) noexcept {
    return false;
}
void application_guild_settings_gateway(const GatewayClientEvent&, Utf8View) noexcept {
}
u64 application_guild_settings_next_wake() noexcept {
    return 0;
}
u64 application_guild_settings_caret_wake() noexcept {
    return 0;
}
void application_guild_settings_tick(u64) noexcept {
}
bool application_guild_settings_edit_focused() noexcept {
    return false;
}
MessageComposer* application_guild_settings_edit_model() noexcept {
    return nullptr;
}
TextInputTarget application_guild_settings_edit_target() noexcept {
    return {};
}
void application_guild_settings_edit_cancel_native_group(TextInputTarget) noexcept {
}
UiRect application_guild_settings_edit_bounds() noexcept {
    return {};
}
bool application_guild_settings_edit_changed(bool) noexcept {
    return false;
}
u32 application_guild_settings_owner_bytes() noexcept {
    return 0;
}
// Create poll is never open in this fixture.
bool application_poll_create_active() noexcept {
    return false;
}
void application_poll_create_close(bool) noexcept {
}
void application_poll_create_release() noexcept {
}
bool application_poll_create_synchronize() noexcept {
    return true;
}
bool application_poll_create_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_poll_create_window_message(UINT, WPARAM, LPARAM) noexcept {
    return false;
}
bool application_poll_create_submit(u64) noexcept {
    return false;
}
bool application_poll_create_owns_response(u64) noexcept {
    return false;
}
bool application_poll_create_response(const RestClientResponse&, Utf8View) noexcept {
    return false;
}
u64 application_poll_create_next_wake() noexcept {
    return 0;
}
u64 application_poll_create_caret_wake() noexcept {
    return 0;
}
void application_poll_create_tick(u64) noexcept {
}
bool application_poll_create_edit_focused() noexcept {
    return false;
}
MessageComposer* application_poll_create_edit_model() noexcept {
    return nullptr;
}
TextInputTarget application_poll_create_edit_target() noexcept {
    return {};
}
void application_poll_create_edit_cancel_native_group(TextInputTarget) noexcept {
}
UiRect application_poll_create_edit_bounds() noexcept {
    return {};
}
bool application_poll_create_edit_changed(bool) noexcept {
    return false;
}
u32 application_poll_create_owner_bytes() noexcept {
    return 0;
}
// The server lifecycle modal is never open in this fixture.
bool application_guild_lifecycle_active() noexcept {
    return false;
}
void application_guild_lifecycle_close(bool) noexcept {
}
void application_guild_lifecycle_release() noexcept {
}
bool application_guild_lifecycle_synchronize() noexcept {
    return true;
}
bool application_guild_lifecycle_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_guild_lifecycle_window_message(UINT, WPARAM, LPARAM) noexcept {
    return false;
}
bool application_guild_lifecycle_submit(u64) noexcept {
    return false;
}
bool application_guild_lifecycle_owns_response(u64) noexcept {
    return false;
}
bool application_guild_lifecycle_response(const RestClientResponse&, Utf8View) noexcept {
    return false;
}
u64 application_guild_lifecycle_next_wake() noexcept {
    return 0;
}
u64 application_guild_lifecycle_caret_wake() noexcept {
    return 0;
}
void application_guild_lifecycle_tick(u64) noexcept {
}
bool application_guild_lifecycle_edit_focused() noexcept {
    return false;
}
MessageComposer* application_guild_lifecycle_edit_model() noexcept {
    return nullptr;
}
TextInputTarget application_guild_lifecycle_edit_target() noexcept {
    return {};
}
void application_guild_lifecycle_edit_cancel_native_group(TextInputTarget) noexcept {
}
UiRect application_guild_lifecycle_edit_bounds() noexcept {
    return {};
}
bool application_guild_lifecycle_edit_changed(bool) noexcept {
    return false;
}
// The profile editor is never open here either.
bool application_profile_editor_open(ProfileEditorTab, u64) noexcept { return false; }
bool application_profile_editor_active() noexcept { return false; }
void application_profile_editor_close(bool) noexcept {}
void application_profile_editor_release() noexcept {}
bool application_profile_editor_synchronize() noexcept { return true; }
bool application_profile_editor_input(UiInputKind, u32, bool) noexcept { return false; }
SettingsLayer* application_profile_editor_settings_layer() noexcept { return nullptr; }
bool application_profile_editor_window_message(UINT, WPARAM, LPARAM) noexcept { return false; }
bool application_profile_editor_submit(u64) noexcept { return false; }
bool application_profile_editor_owns_response(u64) noexcept { return false; }
bool application_profile_editor_response(const RestClientResponse&, Utf8View) noexcept { return false; }
u64 application_profile_editor_next_wake() noexcept { return 0; }
u64 application_profile_editor_caret_wake() noexcept { return 0; }
void application_profile_editor_tick(u64) noexcept {}
bool application_profile_editor_edit_focused() noexcept { return false; }
MessageComposer* application_profile_editor_edit_model() noexcept { return nullptr; }
TextInputTarget application_profile_editor_edit_target() noexcept { return {}; }
void application_profile_editor_edit_cancel_native_group(TextInputTarget) noexcept {}
UiRect application_profile_editor_edit_bounds() noexcept { return {}; }
bool application_profile_editor_edit_changed(bool) noexcept { return false; }
u32 application_profile_editor_owner_bytes() noexcept { return 0; }
// Community modals (rules, onboarding, events) are never open here either.
bool application_guild_community_active() noexcept { return false; }
void application_guild_community_close(bool) noexcept {}
void application_guild_community_release() noexcept {}
bool application_guild_community_synchronize() noexcept { return true; }
bool application_guild_community_input(UiInputKind, u32, bool) noexcept { return false; }
bool application_guild_community_window_message(UINT, WPARAM, LPARAM) noexcept { return false; }
bool application_guild_community_submit(u64) noexcept { return false; }
bool application_guild_community_owns_response(u64) noexcept { return false; }
bool application_guild_community_response(const RestClientResponse&, Utf8View) noexcept { return false; }
u64 application_guild_community_next_wake() noexcept { return 0; }
u64 application_guild_community_caret_wake() noexcept { return 0; }
void application_guild_community_tick(u64) noexcept {}
bool application_guild_community_edit_focused() noexcept { return false; }
MessageComposer* application_guild_community_edit_model() noexcept { return nullptr; }
TextInputTarget application_guild_community_edit_target() noexcept { return {}; }
void application_guild_community_edit_cancel_native_group(TextInputTarget) noexcept {}
UiRect application_guild_community_edit_bounds() noexcept { return {}; }
bool application_guild_community_edit_changed(bool) noexcept { return false; }
u32 application_guild_community_owner_bytes() noexcept { return 0; }
// Reports and user actions share the host; this fixture never opens them.
bool application_report_active() noexcept { return false; }
void application_report_close(bool) noexcept {}
void application_report_release() noexcept {}
bool application_report_synchronize() noexcept { return true; }
bool application_report_input(UiInputKind, u32, bool) noexcept { return false; }
bool application_report_window_message(UINT, WPARAM, LPARAM) noexcept { return false; }
SettingsLayer* application_report_settings_layer() noexcept { return nullptr; }
u64 application_report_next_wake() noexcept { return 0; }
u64 application_report_caret_wake() noexcept { return 0; }
void application_report_tick(u64) noexcept {}
bool application_report_submit(u64) noexcept { return false; }
bool application_report_owns_response(u64) noexcept { return false; }
bool application_report_response(const RestClientResponse&, Utf8View) noexcept { return true; }
u32 application_report_owner_bytes() noexcept { return 0; }
u64 application_user_actions_next_wake() noexcept { return 0; }
bool application_user_actions_submit(u64) noexcept { return false; }
bool application_user_actions_owns_response(u64) noexcept { return false; }
bool application_user_actions_response(const RestClientResponse&, Utf8View) noexcept { return true; }
void application_user_actions_release() noexcept {}
u32 application_guild_lifecycle_owner_bytes() noexcept {
    return 0;
}
// The custom status dialog is never open in this fixture.
bool application_custom_status_active() noexcept {
    return false;
}
void application_custom_status_close(bool) noexcept {
}
void application_custom_status_release() noexcept {
}
bool application_custom_status_synchronize() noexcept {
    return true;
}
bool application_custom_status_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_custom_status_window_message(UINT, WPARAM, LPARAM) noexcept {
    return false;
}
u64 application_custom_status_caret_wake() noexcept {
    return 0;
}
void application_custom_status_dialog_tick(u64) noexcept {
}
bool application_custom_status_edit_focused() noexcept {
    return false;
}
MessageComposer* application_custom_status_edit_model() noexcept {
    return nullptr;
}
TextInputTarget application_custom_status_edit_target() noexcept {
    return {};
}
void application_custom_status_edit_cancel_native_group(TextInputTarget) noexcept {
}
UiRect application_custom_status_edit_bounds() noexcept {
    return {};
}
bool application_custom_status_edit_changed(bool) noexcept {
    return false;
}
u32 application_custom_status_owner_bytes() noexcept {
    return 0;
}
UiSettingsAvatarMedia application_user_avatar_media(u64) noexcept {
    return {};
}
void application_settings_close(bool) noexcept {
}
void application_media_picker_close() noexcept {
}
bool application_profile_close() noexcept {
    return true;
}
void application_composer_text_input_suspend() noexcept {
}
void application_sync_attachment_drop_target() noexcept {
}
bool synchronize_member_subscription(u64) noexcept {
    return true;
}
bool refresh_account_text(bool) noexcept {
    return true;
}
UiRect application_viewport_dips() noexcept {
    return {0, 0, static_cast<float>(g_app.renderer.width), static_cast<float>(g_app.renderer.height)};
}
bool dismiss_popup() noexcept {
    popup_scene_close(&g_app.popup_scene);
    return true;
}
bool application_text_form_open(const ApplicationTextFormRequest& request) noexcept {
    ++channel_overview_runtime_test::discard_prompts;
    channel_overview_runtime_test::discard_request = request;
    g_app.text_form = reinterpret_cast<ApplicationTextForm*>(&channel_overview_runtime_test::form_marker);
    return true;
}
void application_text_form_close(bool restore, bool) noexcept {
    g_app.text_form = nullptr;
    if (restore)
        g_app.context_focus = nullptr;
}
void application_text_form_cancel_owner(TextFormOwnerValid validator) noexcept {
    if (g_app.text_form && channel_overview_runtime_test::discard_request.valid == validator)
        application_text_form_close(false, false);
}
float client_dip_x(LPARAM value) noexcept {
    return static_cast<float>(static_cast<i16>(LOWORD(value)));
}
float client_dip_y(LPARAM value) noexcept {
    return static_cast<float>(static_cast<i16>(HIWORD(value)));
}
} // namespace app_detail
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_application_channel_overview_runtime_entry() noexcept {
    maicord::channel_overview_runtime_test::run();
    const bool failed = maicord::channel_overview_runtime_test::failures != 0;
    constexpr char pass[] = "PASS application channel Overview runtime\n";
    constexpr char fail[] = "FAIL application channel Overview runtime\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}
