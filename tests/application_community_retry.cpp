#include "../src/app/application_guild_community.cpp"

// Only the response lane is exercised. No modal is active and the fixture
// does not navigate, render or open an editor.
namespace maicord {
bool renderer_rebuild_content_text(D3D11Renderer*, const Theme&) noexcept {
    return true;
}
void renderer_bind_settings_scene(D3D11Renderer*, const UiSettingsScene*) noexcept {
}
bool renderer_set_settings_modal_progress(D3D11Renderer*, float) noexcept {
    return true;
}
} // namespace maicord

namespace maicord::app_detail {
bool dismiss_popup() noexcept {
    popup_scene_close(&g_app.popup_scene);
    g_app.popup_owner_kind = PopupOwnerKind::none;
    return true;
}
bool refresh_popup_text() noexcept {
    return true;
}
bool pump_message_history() noexcept {
    return true;
}
bool synchronize_navigation_view() noexcept {
    return true;
}
bool application_channel_overview_active() noexcept {
    return false;
}
void application_composer_text_input_suspend() noexcept {
}
void application_settings_close(bool) noexcept {
}
void application_media_picker_close() noexcept {
}
bool application_profile_close() noexcept {
    return true;
}
void application_sync_attachment_drop_target() noexcept {
}
bool application_navigate_known_message(u64, u64) noexcept {
    return false;
}
bool application_action_captcha_arm(ApplicationActionCaptchaKind, u64, u64, bool) noexcept {
    return false;
}
bool application_guild_card_join_take_captcha(u64) noexcept {
    return false;
}
void application_guild_card_join_poll(u64) noexcept {
}
void settings_layer_reset(SettingsLayer*) noexcept {
}
bool settings_layer_editor(SettingsLayer*, u64*, MessageComposer**, BoundedEdit**, bool*) noexcept {
    return false;
}
UiSettingsEditor settings_layer_editor_view(SettingsLayer*, u64, MessageComposer&, Utf8View,
                                            bool) noexcept {
    return {};
}
bool settings_layer_focus(SettingsLayer*, UiSettingsTarget) noexcept {
    return false;
}
bool settings_layer_open_choice(SettingsLayer*, u64, ConstSlice<PopupItem>, UiRect, float) noexcept {
    return false;
}
void settings_layer_close_choice(SettingsLayer*) noexcept {
}
void settings_layer_begin_entrance(SettingsLayer*, u64, bool) noexcept {
}
void settings_layer_cancel_pointer(SettingsLayer*) noexcept {
}
bool settings_layer_choice_current(const SettingsLayer&) noexcept {
    return false;
}
u64 settings_layer_tick(SettingsLayer*, u64, bool) noexcept {
    return 0;
}
u64 settings_layer_next_wake(const SettingsLayer&) noexcept {
    return 0;
}
bool settings_layer_input(SettingsLayer*, UiInputKind, u32, bool) noexcept {
    return false;
}
bool settings_layer_window_message(SettingsLayer*, UINT, WPARAM, LPARAM) noexcept {
    return false;
}
UiRect settings_layer_edit_bounds(SettingsLayer*) noexcept {
    return {};
}
bool settings_layer_edit_changed(SettingsLayer*, bool) noexcept {
    return false;
}
void settings_layer_cancel_native_group(SettingsLayer*, TextInputTarget) noexcept {
}

namespace {
u32 retry_card_responses;
bool retry_card_join;
u16 retry_card_status;
Background retry_background;
} // namespace
void application_guild_card_response(u64, bool join, const RestClientResponse& response,
                                     Utf8View) noexcept {
    ++retry_card_responses;
    retry_card_join = join;
    retry_card_status = response.status_code;
}

void test_community_retry(void (*check)(bool, const char*) noexcept) noexcept {
    auto* old_background = background;
    auto* old_owner = owner;
    background = &retry_background;
    owner = nullptr;
    retry_card_responses = 0;
    for (u32 kind = 0; kind < 4; ++kind) {
        retry_background = {};
        retry_background.request_id = 74;
        retry_background.flight = {80, 0, GuildCommunityOperation::guild_join};
        RestClientResponse response{};
        response.request_id = 74;
        response.status_code = 429;
        response.rate_limited = kind != 1;
        response.retry_at_ms = kind == 2 ? 0 : 4567890;
        response.rejection.captcha = kind == 0;
        const u32 before = retry_card_responses;
        check(application_guild_community_response(response, {}), "community exact completion consumed");
        if (kind == 3)
            check(retry_background.queue_count == 1 && !retry_background.request_id &&
                      retry_background.retry_at == 4567890 &&
                      retry_background.queue[0].op == GuildCommunityOperation::guild_join &&
                      retry_background.queue[0].guild_id == 80 && retry_card_responses == before,
                  "community genuine 429 requeues the same join at the exact server deadline");
        else
            check(!retry_background.queue_count && !retry_background.request_id &&
                      !retry_background.retry_at && retry_card_responses == before + 1 && retry_card_join &&
                      retry_card_status == 429,
                  "community challenge or unknown cooldown reaches join error presentation without replay");
        check(!application_guild_community_response(response, {}),
              "community drained receipt cannot requeue twice");
    }
    check(retry_card_responses == 3, "community terminal joins each reach the card once");
    retry_background = {};
    background = old_background;
    owner = old_owner;
}
} // namespace maicord::app_detail
