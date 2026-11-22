#include "../../../src/app/guild/community.cpp"
#include "../../../src/app/guild/community_projection.cpp"
#include "../../../src/app/guild/channels_roles.cpp"
#include "../../support/model_store_fixtures.hpp"
#include "../../support/account_module_fake.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "../../support/test_output.hpp"

// Controlled responses, form projection and serialization; no account or
// network is opened and renderer/editor hooks remain inert.
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
// Dialog exits are covered by the surface motion host test; these closes run at once.
float application_motion_tick(UiMotionTransition*, const UiMotionScope&, float target, u64,
                              const UiMotionStyle&) noexcept {
    return target;
}
// My Profile's avatar: the settings projection that resolves it would bring in the composer, which this test fakes.
UiSettingsAvatarMedia application_listed_user_avatar_media(u64, const Utf8View&, u16) noexcept {
    return {};
}
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
bool settings_layer_editor(SettingsLayer*, u64*, MessageComposer**, BoundedEdit**) noexcept {
    return false;
}
UiSettingsEditor settings_layer_editor_view(SettingsLayer*, u64, MessageComposer&, const Utf8View&,
                                            bool) noexcept {
    return {};
}
void settings_layer_focus(SettingsLayer*, UiSettingsTarget) noexcept {
}
void settings_layer_open_choice(SettingsLayer*, u64, const ConstSlice<PopupItem>&, const UiRect&, float) noexcept {
}
void settings_layer_close_choice(SettingsLayer*) noexcept {
}
void settings_layer_begin_entrance(SettingsLayer*) noexcept {
}
void settings_layer_cancel_pointer(SettingsLayer*) noexcept {
}
bool settings_layer_choice_lapsed(SettingsLayer&) noexcept {
    return false;
}
void settings_layer_tick(SettingsLayer*, u64) noexcept {
}
u64 settings_layer_next_wake(const SettingsLayer&) noexcept {
    return 0;
}
bool settings_layer_input(SettingsLayer*, UiInputKind, u32, bool) noexcept {
    return false;
}
bool settings_layer_window_event(SettingsLayer*, const WindowEvent&) noexcept {
    return false;
}
UiRect settings_layer_edit_bounds(SettingsLayer*) noexcept {
    return {};
}
bool settings_layer_edit_changed(SettingsLayer*, bool) noexcept {
    return false;
}
void settings_layer_cancel_native_group(SettingsLayer*, const TextInputTarget&) noexcept {
}

namespace {
u32 retry_card_responses;
bool retry_card_join;
u16 retry_card_status;
Background retry_background;
Owner retry_owner;
// The last request handed to the account module, with a copy of its body.
GuildCommunityRequest retry_sent;
char retry_sent_body[kGuildCommunityMaximumBodyBytes];
RestClientSubmitResult WINAPI community_submit(void*, const GuildCommunityRequest* request, u64) noexcept {
    retry_sent = *request;
    copy_bytes(retry_sent_body, request->body, request->body_bytes);
    retry_sent.body = reinterpret_cast<const u8*>(retry_sent_body);
    return {};
}
} // namespace
void application_guild_card_response(u64, bool join, const RestClientResponse& response,
                                     Utf8View) noexcept {
    ++retry_card_responses;
    retry_card_join = join;
    retry_card_status = response.status_code;
}
// The Server Hub's lane hooks; this fixture never opens the hub.
Utf8View application_guild_hub_query() noexcept {
    return {};
}
void application_guild_hub_response(GuildCommunityOperation, u64, bool, u16, Utf8View) noexcept {}
void application_guild_hub_profile(u64, bool, Utf8View) noexcept {}
void application_guild_hub_forget(u64) noexcept {}
// The content page host: the fixture records the page shown and builds its tree itself.
const ContentPageOwner* retry_page;
bool application_content_page_open(const ContentPageOwner* shown) noexcept {
    retry_page = shown;
    return true;
}
void application_content_page_close() noexcept {
    retry_page = nullptr;
}
const ContentPageOwner* application_content_page() noexcept {
    return retry_page;
}
void application_content_page_invalidate() noexcept {}
// Browse Channels' Create and View: the dialogs, the voice call and the forum stay with their own tests.
bool application_guild_settings_create_channel(u64, u64, u8) noexcept {
    return false;
}
bool application_voice_join(u64, u64) noexcept {
    return false;
}
bool application_thread_browser_open(u64) noexcept {
    return false;
}

void test_community_forms(void (*check)(bool, const char*) noexcept) noexcept {
    auto* saved_ui = g_app.account_ui;
    const auto saved_instance = g_app.account.instance;
    const auto saved_submit = g_account_fake.guild_community;
    const bool saved_ready = g_app.account.status.gateway_ready;
    const bool saved_running = g_app.account.status.running;
    const bool saved_login = g_app.login.visible;
    const bool saved_renderer = g_app.renderer_ready;
    auto* saved_text_form = g_app.text_form;
    auto* saved_focus = g_app.context_focus;
    const u64 saved_lifetime = lifetime_serial;
    auto* ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(ui != nullptr, "community form fixture allocates isolated state");
    if (!ui)
        return;
    g_app.account_ui = ui;
    g_app.account.instance = &retry_background;
    g_account_fake.guild_community = community_submit;
    g_app.account.status.gateway_ready = true;
    g_app.renderer_ready = false;
    retry_background = {};
    retry_background.account_scope = g_app.plugin_account_scope_serial;
    retry_owner = {};
    owner = &retry_owner;
    auto& p = retry_owner;
    bool editors_ready = true;
    for (u32 i = 0; i < kEditors; ++i)
        editors_ready =
            message_composer_initialize(&p.models[i], {p.editor_text[i], kEditorBytes}) && editors_ready;
    check(editors_ready, "community form fixture initializes bounded editors");
    p.guild_id = 80;
    p.scene.active = true;
    p.phase = Phase::ready;
    p.mode = Mode::rules;
    const auto project_form = [&]() noexcept {
        p.control_count = 0;
        Bank bank{p.banks[0], sizeof(p.banks[0]), 0};
        if (p.mode == Mode::rules)
            project_rules(bank);
        else
            project_onboarding(bank);
    };
    char unicode_rule[600];
    for (u32 i = 0; i < sizeof(unicode_rule); i += 2) {
        unicode_rule[i] = '\xC3';
        unicode_rule[i + 1] = '\xA9';
    }
    ProtocolWriter rules{{reinterpret_cast<u8*>(p.form_body), sizeof(p.form_body)}, 0};
    bool formed = protocol_writer_append_literal(
        &rules,
        R"({"version":"1","form_fields":[{"field_type":"TERMS","label":"Rules","required":true,"values":[)");
    for (u32 i = 0; formed && i < 16; ++i)
        formed = (!i || protocol_writer_append_byte(&rules, ',')) &&
                 protocol_writer_append_json_string(&rules, {unicode_rule, sizeof(unicode_rule)});
    formed = formed && protocol_writer_append_literal(&rules, "]}]}");
    p.form_body_bytes = rules.used;
    const bool large_rules =
        formed && guild_verification_decode({p.form_body, p.form_body_bytes},
                                            json_scratch(), &p.form);
    check(large_rules && !p.form.truncated && p.form.text_used > sizeof(p.banks[0]),
          "community complete Unicode rules fit the model but exceed the presentation bank");
    project_form();
    check(!rules_presentation_complete(p) && !p.footer.can_save && p.control_count == 1 &&
              p.controls[0].id == control_notice && p.footer.status.bytes,
          "community unseen rules replace consent with an unsupported notice");
    p.footer.can_save = true;
    submit_modal();
    SettingsLayerActivation consent{};
    consent.value.target = {UiSettingsTargetKind::control, kPage, control_agree};
    consent.value.checked = true;
    activate(nullptr, consent);
    check(p.phase == Phase::ready && !retry_background.queue_count && !p.answers[0].agreed,
          "community stale save and consent events cannot accept unseen rules");
    p.answers[0].agreed = true;
    p.phase = Phase::submitting;
    p.footer.can_save = false;
    check(!build_body({80, 0, GuildCommunityOperation::verification_submit}),
          "community queued rules write independently rejects presentation loss");

    const auto complete_rules = utf8_literal(
        R"({"version":"1","form_fields":[{"field_type":"TERMS","label":"Rules","required":true,"values":["Read this rule."]}]})");
    copy_bytes(p.form_body, complete_rules.data, complete_rules.bytes);
    p.form_body_bytes = complete_rules.bytes;
    check(guild_verification_decode(complete_rules, json_scratch(), &p.form),
          "community complete rules fixture decodes");
    p.phase = Phase::ready;
    p.answers[0].agreed = false;
    p.footer.can_save = true;
    submit_modal();
    check(p.phase == Phase::ready && !retry_background.queue_count,
          "community stale footer cannot queue unanswered complete rules");
    p.answers[0].agreed = true;
    project_form();
    submit_modal();
    check(p.phase == Phase::submitting && retry_background.queue_count == 1 &&
              retry_background.queue[0].op == GuildCommunityOperation::verification_submit,
          "community complete rules still queue normally");
    project_form();
    check(!p.footer.can_save && build_body(retry_background.queue[0]) != 0,
          "community submitting footer does not veto a complete queued rules body");
    retry_background.queue_count = kQueueCapacity;
    for (u32 i = 0; i < kQueueCapacity; ++i)
        retry_background.queue[i] = {100 + i, 0, GuildCommunityOperation::guild_join};
    p.phase = Phase::ready;
    p.footer.can_save = true;
    submit_modal();
    check(p.phase == Phase::ready && p.failed && p.status.bytes &&
              retry_background.queue_count == kQueueCapacity &&
              retry_background.queue[kQueueCapacity - 1].op == GuildCommunityOperation::guild_join,
          "community full queue leaves a complete form editable with a retryable error");
    retry_background.queue_count = 0;

    const auto make_onboarding = [&](u32 prompts, u32 options, bool long_titles) noexcept {
        ProtocolWriter writer{{reinterpret_cast<u8*>(p.form_body), sizeof(p.form_body)}, 0};
        bool ok = protocol_writer_append_literal(&writer, R"({"enabled":true,"prompts":[)");
        for (u32 i = 0; ok && i < prompts; ++i) {
            ok = (!i || protocol_writer_append_byte(&writer, ',')) &&
                 protocol_writer_append_literal(&writer, R"({"id":)") &&
                 protocol_writer_append_snowflake(&writer, i + 1) &&
                 protocol_writer_append_literal(&writer,
                                                R"(,"title":"Choose","in_onboarding":true,"options":[)");
            for (u32 o = 0; ok && o < options; ++o)
                ok = (!o || protocol_writer_append_byte(&writer, ',')) &&
                     protocol_writer_append_literal(&writer, R"({"id":)") &&
                     protocol_writer_append_snowflake(&writer, 1000 + i * options + o) &&
                     protocol_writer_append_literal(&writer, R"(,"title":)") &&
                     protocol_writer_append_json_string(&writer, long_titles ? Utf8View{unicode_rule, 200}
                                                                             : utf8_literal("Choice")) &&
                     protocol_writer_append_byte(&writer, '}');
            ok = ok && protocol_writer_append_literal(&writer, "]}");
        }
        ok = ok && protocol_writer_append_literal(&writer, "]}");
        return ok &&
               guild_onboarding_decode({p.form_body, writer.used}, json_scratch(),
                                       &p.onboarding) &&
               !p.onboarding.truncated;
    };
    p.mode = Mode::onboarding;
    p.phase = Phase::ready;
    p.status = {};
    p.failed = false;
    const bool decoded = make_onboarding(1, 45, true);
    p.flow_count = guild_onboarding_flow_prompts(p.onboarding, {p.flow, kGuildOnboardingPromptMaximum});
    p.step = p.flow_count ? p.flow_count - 1 : 0;
    check(decoded && !onboarding_presentation_complete(p),
          "community complete onboarding model can exceed text or control presentation limits");
    project_form();
    check(!p.footer.can_save && p.control_count == 1 && p.footer.status.bytes,
          "community incomplete onboarding presentation disables saving");
    p.footer.can_save = true;
    submit_modal();
    check(p.phase == Phase::ready && !retry_background.queue_count,
          "community stale footer cannot queue partial onboarding selections");
    p.phase = Phase::submitting;
    check(!build_body({80, 0, GuildCommunityOperation::onboarding_submit}),
          "community queued onboarding body rechecks its own presentation completeness");
    const auto complete_onboarding = utf8_literal(
        R"({"enabled":true,"responses":["101","202"],"prompts":[{"id":"1","title":"First","required":true,"in_onboarding":true,"options":[{"id":"101","title":"Choice"}]},{"id":"2","title":"Later","required":true,"in_onboarding":false,"options":[{"id":"202","title":"Saved"}]}]})");
    check(guild_onboarding_decode(complete_onboarding, json_scratch(), &p.onboarding) &&
              !p.onboarding.truncated,
          "community complete saved onboarding fixture decodes");
    // The Channels & Roles page saves the choices of every prompt, outside the onboarding flow too. The dialog submits
    // nothing meanwhile.
    p.phase = Phase::ready;
    static Page retry_roles_page;
    page = &retry_roles_page;
    page->guild_id = 80;
    page->phase = Phase::ready;
    const bool page_decoded = guild_onboarding_decode(complete_onboarding, json_scratch(), &page->onboarding);
    save();
    const u32 edit_bytes = build_body({80, 0, GuildCommunityOperation::onboarding_update});
    check(page_decoded && retry_background.queue_count == 1 && edit_bytes &&
              test_contains({retry_background.body, edit_bytes}, utf8_literal(R"("onboarding_responses":["101","202"])")),
          "community Channels & Roles saves every prompt's choices, outside the initial flow too");
    check(!build_body({80, 0, GuildCommunityOperation::verification_submit}) &&
              !build_body({80, 0, GuildCommunityOperation::onboarding_submit}) &&
              !build_body({81, 0, GuildCommunityOperation::onboarding_update}),
          "community queued writes cannot serialize an incompatible modal or another server's choices");
    // The official client keeps the only answer of a required prompt.
    activate(action_option + 1, {});
    check(page->onboarding.options[1].selected && !page->save_at,
          "community Channels & Roles keeps the only answer of a required prompt");
    retry_background.queue_count = 0;
    p.mode = Mode::onboarding;
    p.phase = Phase::ready;
    p.flow_count = guild_onboarding_flow_prompts(p.onboarding, {p.flow, kGuildOnboardingPromptMaximum});
    p.step = 0;
    p.onboarding.options[0].selected = false;
    p.footer.can_save = true;
    submit_modal();
    check(p.phase == Phase::ready && !retry_background.queue_count && !p.step,
          "community stale footer cannot finish an unanswered required flow prompt");
    p.phase = Phase::submitting;
    check(!build_body({80, 0, GuildCommunityOperation::onboarding_submit}),
          "community queued initial submission rechecks required flow answers");
    p.phase = Phase::ready;
    p.onboarding.options[0].selected = true;
    project_form();
    submit_modal();
    const u32 initial_bytes = build_body({80, 0, GuildCommunityOperation::onboarding_submit});
    check(p.phase == Phase::submitting && retry_background.queue_count == 1 && initial_bytes &&
              test_contains({retry_background.body, initial_bytes}, utf8_literal(R"("onboarding_responses":["101"])")),
          "community complete initial flow submits independently of nonflow required prompts");

    const GuildCommunityOperation requests[]{
        GuildCommunityOperation::verification_submit, GuildCommunityOperation::onboarding_submit,
        GuildCommunityOperation::event_create, GuildCommunityOperation::verification_get};
    const Mode modes[]{Mode::rules, Mode::onboarding, Mode::create, Mode::rules};
    for (u32 i = 0; i < 4; ++i) {
        for (u32 stale = 0; stale < 5; ++stale) {
            retry_background = {};
            retry_background.request_id = 74;
            retry_background.flight = {80, 0, requests[i]};
            retry_background.modal_lifetime = 40;
            p.lifetime = stale ? 41 : 40;
            p.mode = modes[i];
            p.phase = modal_submission(requests[i]) ? Phase::submitting : Phase::loading;
            const Phase current_phase = p.phase;
            p.scene.active = stale != 1;
            p.status = utf8_literal("New form");
            RestClientResponse response{};
            response.request_id = 74;
            response.status_code = stale == 4 ? 204 : stale == 3 ? 403 : 429;
            response.rate_limited = response.status_code == 429;
            response.retry_at_ms = 4567890;
            check(application_guild_community_response(response, {}),
                  "community modal request exact completion consumed");
            check(stale ? !retry_background.queue_count && !retry_background.retry_at &&
                              p.phase == current_phase && utf8_equal(p.status, utf8_literal("New form"))
                        : retry_background.queue_count == 1 && retry_background.retry_at == 4567890 &&
                              retry_background.queue[0].op == requests[i],
                  "community only the current modal document may retry or display request failures");
        }
    }
    // A Channels & Roles save belongs to no dialog: it retries while a dialog is closed.
    reset_value(&retry_background);
    retry_background.request_id = 74;
    retry_background.flight = {80, 0, GuildCommunityOperation::onboarding_update};
    p.scene.active = false;
    {
        RestClientResponse response{};
        response.request_id = 74;
        response.status_code = 429;
        response.rate_limited = true;
        response.retry_at_ms = 4567890;
        check(application_guild_community_response(response, {}) && retry_background.queue_count == 1 &&
                  retry_background.queue[0].op == GuildCommunityOperation::onboarding_update,
              "community a Channels & Roles save retries after its dialog closed");
    }
    p.scene.active = true;

    GuildRecord guilds[1]{};
    ChannelRecord channels[4]{};
    RoleRecord roles[3]{};
    PermissionOverwriteRecord overwrites[1]{};
    u16 guild_order[1]{}, channel_order[4]{};
    u8 names[256]{};
    const bool initialized =
        chat_state_store_initialize(&ui->chat_state, {guilds, 1}, {guild_order, 1}, {channels, 4},
                                    {channel_order, 4}, {names, sizeof(names)}) &&
        chat_state_store_bind_permissions(&ui->chat_state, {roles, 3}, {overwrites, 1}) &&
        chat_state_store_upsert_guild(&ui->chat_state,
                                      {80, utf8_literal("Community"), GuildMutationKind::create, 1})
            .ok();
    retry_background = {};
    p.scene.active = false;
    p.phase = Phase::loading;
    const bool opened = initialized && open_modal(Mode::rules, 80, utf8_literal("Rules"), 0);
    check(opened, "community native modal fixture opens");
    if (opened) {
        const u64 previous = p.lifetime;
        retry_background.request_id = 74;
        retry_background.flight = {80, 0, GuildCommunityOperation::verification_submit};
        retry_background.modal_lifetime = previous;
        retry_background.queue_count = 2;
        retry_background.queue[0] = {80, 0, GuildCommunityOperation::verification_submit};
        retry_background.queue[1] = {80, 0, GuildCommunityOperation::onboarding_get};
        check(open_modal(Mode::rules, 80, utf8_literal("Rules again"), 0) &&
                  retry_background.queue_count == 1 &&
                  retry_background.queue[0].op == GuildCommunityOperation::onboarding_get,
              "community active same-mode replacement advances its document and drops prior writes");
        p.phase = Phase::submitting;
        RestClientResponse response{};
        response.request_id = 74;
        response.status_code = 429;
        response.rate_limited = true;
        response.retry_at_ms = 4567890;
        check(application_guild_community_response(response, {}) && retry_background.queue_count == 1 &&
                  !retry_background.retry_at && p.phase == Phase::submitting,
              "community old response cannot replay after replacement itself submits");
        const bool permissions =
            chat_state_store_upsert_role(&ui->chat_state,
                                         {80, 80, kPermissionViewChannel | kPermissionManageEvents, {}}) &&
            chat_state_store_set_self_roles(&ui->chat_state, 80, 7, 7, {});
        chat_state_store_recompute_guild_permissions(&ui->chat_state, 80);
        g_app.account.status.running = true;
        g_app.login.visible = false;
        g_app.renderer_ready = true;
        g_app.text_form = nullptr;
        const bool create_opened = permissions && application_guild_community_open_create(80);
        check(create_opened, "community active Create fixture opens through the public action");
        if (create_opened) {
            const u64 create_lifetime = p.lifetime;
            retry_background.request_id = 74;
            retry_background.flight = {80, 0, GuildCommunityOperation::event_create};
            retry_background.modal_lifetime = create_lifetime;
            retry_background.queue_count = 2;
            retry_background.queue[0] = {80, 0, GuildCommunityOperation::event_create};
            retry_background.queue[1] = {80, 0, GuildCommunityOperation::onboarding_get};
            check(
                application_guild_community_open_create(80) && retry_background.queue_count == 1 &&
                    retry_background.queue[0].op == GuildCommunityOperation::onboarding_get,
                "community active Create replacement refreshes identity and drops its prior queued write");
            // A server without stage or voice channels: choosing that location type must still project a
            // presentable page (a choice row cannot be empty) and state why no channel can be picked.
            for (u32 where = 0; where < 2; ++where) {
                SettingsLayerActivation pick{};
                pick.value.target = {UiSettingsTargetKind::control, kPage, control_where};
                pick.value.selected_choice = where;
                p.phase = Phase::ready;
                activate(nullptr, pick);
                bool channel_notice = false;
                for (u32 i = 0; i < p.control_count; ++i)
                    if (p.controls[i].id == control_channel)
                        channel_notice = p.controls[i].kind == UiSettingsControlKind::notice &&
                                         p.controls[i].description.bytes != 0;
                const bool projected = project();
                check(p.where == where && !p.channel_count && channel_notice && projected &&
                          !p.footer.can_save,
                      where ? "community Create with no voice channels projects an empty-state notice"
                            : "community Create with no stage channels projects an empty-state notice");
            }
            p.phase = Phase::submitting;
            p.status = utf8_literal("Replacement event");
            check(application_guild_community_response(response, {}) && retry_background.queue_count == 1 &&
                      !retry_background.retry_at && p.phase == Phase::submitting &&
                      utf8_equal(p.status, utf8_literal("Replacement event")),
                  "community old Create 429 cannot retry after its replacement submits");
        }
        // Channels & Roles: the page's tree, a pick's summary, its save a second later, the role the save gave in
        // My Profile, and the saves on leaving Customize and when the page goes. The member holds Members; Artists is
        // the answer's role.
        const u64 artists = 7, members = 8;
        const bool roles_ready =
            permissions &&
            chat_state_store_upsert_role(&ui->chat_state, {artists, 80, 0, utf8_literal("Artists"), 1, 0x2ECC71}) &&
            chat_state_store_upsert_role(&ui->chat_state, {members, 80, 0, utf8_literal("Members"), 2, 0x3498DB}) &&
            chat_state_store_set_self_roles(&ui->chat_state, 80, 7, 7, {&members, 1});
        // The member saw both questions and every answer but Music: Pick roles shows under New Options.
        const auto page_body = utf8_literal(
            R"({"enabled":true,"responses":["601"],"prompts":[{"id":"1","title":"Pick roles","options":[)"
            R"({"id":"501","title":"Art","role_ids":["7"]},{"id":"502","title":"Music","emoji":{"id":"55","name":"note"}}]},)"
            R"({"id":"2","title":"Region","single_select":true,"required":true,"options":[{"id":"601","title":"EU"},)"
            R"({"id":"602","title":"NA"}]}],"onboarding_prompts_seen":{"1":1,"2":1},)"
            R"("onboarding_responses_seen":{"501":1,"601":1,"602":1}})");
        // A 1920 DIP window: My Profile shows beside the questions.
        const u32 saved_width = g_app.renderer.width, saved_dpi = g_app.renderer.dpi;
        g_app.renderer.width = 1920;
        g_app.renderer.dpi = 96;
        guilds[0].upload_features = guild_record_community | guild_record_onboarding_prompts;
        ui->chat_state.selected_guild_id = 80;
        ui->chat_state.home_selected = false;
        retry_page = nullptr;
        page->save_at = 0;
        reset_value(&retry_background);
        retry_background.account_scope = g_app.plugin_account_scope_serial;
        const bool page_opened = roles_ready && application_guild_channels_roles_open(80);
        check(page_opened && retry_page == &kRolesPage && !page->browse && page->phase == Phase::loading &&
                  retry_background.queue_count == 1 &&
                  retry_background.queue[0].op == GuildCommunityOperation::onboarding_get,
              "community Channels & Roles opens as a content page and asks for the questions");
        static UiStoreScene scene;
        const bool scene_ready = ui_store_scene_open(&scene);
        const auto rebuild = [&]() noexcept {
            ui_store_scene_begin(&scene);
            UiStoreBuilder builder(&scene);
            build(builder, 1000);
            return builder.ok();
        };
        // The index of the first node of `kind` with `text` (or with `action`), scene.count when none.
        const auto find = [&](UiStoreKind kind, Utf8View text, u32 action = 0) noexcept {
            u32 i = 0;
            for (; i < scene.count; ++i) {
                const auto& n = scene.nodes[i];
                if (n.kind == kind && (action ? n.action == action : utf8_equal(n.text, text)))
                    break;
            }
            return i;
        };
        if (page_opened && scene_ready) {
            retry_background.request_id = 75;
            retry_background.flight = {80, 0, GuildCommunityOperation::onboarding_get};
            retry_background.modal_lifetime = 0;
            RestClientResponse answer{};
            answer.request_id = 75;
            answer.status_code = 200;
            check(application_guild_community_response(answer, page_body) && page->phase == Phase::ready &&
                      page->onboarding.option_count == 4,
                  "community the onboarding answer no dialog waits for reaches the page");
            reset_value(&retry_background);
            retry_background.account_scope = g_app.plugin_account_scope_serial;
            const bool built = rebuild();
            const u32 browse_tab = find(UiStoreKind::tab, {}, action_browse);
            // The tile's card, its row, then the emoji's picture.
            const u32 emoji = find(UiStoreKind::card, {}, action_option + 1) + 2;
            const auto* emoji_media = emoji < scene.count ? media(scene.nodes[emoji].value) : nullptr;
            // A single-select prompt with nothing chosen on this visit has no summary line.
            check(built && (scene.nodes[find(UiStoreKind::tab, {}, action_customize)].flags & ui_store_selected) &&
                      browse_tab < scene.count && scene.nodes[browse_tab].text2.bytes &&
                      find(UiStoreKind::text, utf8_literal("New Option")) < scene.count &&
                      find(UiStoreKind::text, utf8_literal("Customization Question")) < scene.count &&
                      find(UiStoreKind::chip, utf8_literal("NEW")) < scene.count &&
                      scene.nodes[emoji].kind == UiStoreKind::preview && emoji_media &&
                      emoji_media->key.kind == MediaAssetKind::custom_emoji && emoji_media->key.id == 55 &&
                      find(UiStoreKind::text, utf8_literal("Choose all that apply.")) < scene.count &&
                      (scene.nodes[find(UiStoreKind::card, {}, action_option + 2)].flags & ui_store_selected) &&
                      scene.nodes[find(UiStoreKind::card, {}, action_option + 2)].variant ==
                          static_cast<u8>(UiStoreCard::answer) &&
                      find(UiStoreKind::text, utf8_literal("My Profile")) < scene.count &&
                      find(UiStoreKind::text, utf8_literal("Members")) < scene.count &&
                      find(UiStoreKind::text, utf8_literal("Artists")) == scene.count,
                  "community Channels & Roles builds tabs, new and other questions, answer tiles and My Profile");
            activate(action_option, {});
            const u64 due = page->save_at;
            rebuild();
            // The official client's words, the role's name in the strong ink; My Profile shows the role at once.
            const u32 named = find(UiStoreKind::text, utf8_literal("@Artists"));
            check(page->onboarding.options[0].selected && due && !retry_background.queue_count &&
                      find(UiStoreKind::text, utf8_literal("You received")) < named && named < scene.count &&
                      scene.nodes[named].ink == static_cast<u8>(UiStoreInk::strong) &&
                      utf8_equal(scene.nodes[named + 1].text, utf8_literal("role.")) &&
                      find(UiStoreKind::text, utf8_literal("Artists")) < scene.count,
                  "community a pick names what it gives, shows its role and waits a second to save");
            g_app.details_request_owner = MessageHistoryRequestOwner::none;
            retry_sent = {};
            const bool early = application_guild_community_submit(due - 1);
            check(!early && !retry_background.queue_count && page->save_at == due,
                  "community the save waits for its second");
            application_guild_community_submit(due);
            const Utf8View sent{retry_sent_body, retry_sent.body_bytes};
            check(retry_sent.operation == GuildCommunityOperation::onboarding_update && retry_sent.guild_id == 80 &&
                      !page->save_at && test_contains(sent, utf8_literal(
                                        R"({"onboarding_responses":["501","601"],"onboarding_prompts_seen":{"1":)")) &&
                      test_contains(sent, utf8_literal(R"(,"2":)")) &&
                      test_contains(sent, utf8_literal(R"("onboarding_responses_seen":{"501":)")) &&
                      test_contains(sent, utf8_literal(R"(,"602":)")),
                  "community the save is the official client's PUT with every prompt and option seen");
            rebuild();
            check(find(UiStoreKind::chip, utf8_literal("NEW")) == scene.count &&
                      find(UiStoreKind::text, utf8_literal("Customization Questions")) < scene.count,
                  "community a save sees every question and answer: no NEW badge stays");
            // Discord answers the save with GUILD_MEMBER_UPDATE: My Profile lists the new role, highest first.
            const u64 held[]{artists, members};
            chat_state_store_set_self_roles(&ui->chat_state, 80, 7, 7, {held, 2});
            chat_state_store_recompute_guild_permissions(&ui->chat_state, 80);
            // The fixture's server has no channel to keep selected: the store dropped the selection. Select it again.
            ui->chat_state.selected_guild_id = 80;
            ui->chat_state.home_selected = false;
            const bool changed = stale();
            rebuild();
            const u32 artists_pill = find(UiStoreKind::text, utf8_literal("Artists"));
            check(changed && artists_pill < scene.count &&
                      find(UiStoreKind::text, utf8_literal("Members")) < artists_pill &&
                      scene.nodes[artists_pill - 1].kind == UiStoreKind::swatch &&
                      scene.nodes[artists_pill - 1].value == 0x2ECC71,
                  "community My Profile follows the roles of a member update, highest first");
            reset_value(&retry_background);
            retry_background.account_scope = g_app.plugin_account_scope_serial;
            activate(action_browse, {});
            check(page->browse && retry_background.queue_count == 1 &&
                      retry_background.queue[0].op == GuildCommunityOperation::onboarding_update,
                  "community leaving Customize for Browse Channels saves the choices, as the official client does");
            activate(action_customize, {});
            reset_value(&retry_background);
            retry_background.account_scope = g_app.plugin_account_scope_serial;
            activate(action_option + 1, {});
            application_content_page_close();
            channels_roles_tick(1);
            const u32 closing = retry_background.queue_count == 1 ? build_body(retry_background.queue[0]) : 0;
            check(closing && retry_background.queue[0].op == GuildCommunityOperation::onboarding_update &&
                      !page->save_at &&
                      test_contains({retry_background.body, closing}, utf8_literal(R"("onboarding_responses":["501","502","601"])")),
                  "community a pending save goes at once when the page goes");
            // The official client saves whenever Customize goes with the page; never from Browse Channels.
            for (u32 tab = 0; tab < 2; ++tab) {
                reset_value(&retry_background);
                retry_background.account_scope = g_app.plugin_account_scope_serial;
                retry_page = &kRolesPage;
                page->shown = true;
                page->browse = tab != 0;
                application_content_page_close();
                channels_roles_tick(2);
                check(retry_background.queue_count == (tab ? 0u : 1u) && !page->shown,
                      tab ? "community leaving the page from Browse Channels sends nothing"
                          : "community leaving the page from Customize saves, changed or not");
            }
            // A narrow window at 200%: 720 pixels are 360 DIPs. The content view is 360 DIPs, then the smallest one
            // the window allows (320), less the page's padding of 32 on each side. The question's long title wraps
            // inside its card, which grows to fit, and every text lies inside its card's padding (else the column).
            const auto narrow_body = utf8_literal(
                R"({"enabled":true,"responses":["701"],"prompts":[{"id":"3","required":true,)"
                R"("title":"What do you want to do in this community?","options":[{"id":"701","title":"hello"},)"
                R"({"id":"702","title":"An answer whose title is far wider than its tile",)"
                R"("description":"And a description that is just as long as the title"}]}],)"
                R"("onboarding_prompts_seen":{"3":1},"onboarding_responses_seen":{"701":1,"702":1}})");
            const bool narrow_decoded = guild_onboarding_decode(narrow_body, json_scratch(), &page->onboarding);
            page->browse = false;
            page->phase = Phase::ready;
            g_app.renderer.width = 720;
            g_app.renderer.dpi = 192;
            static u16 parents[kUiStoreNodeCapacity];
            static constexpr float kViews[]{360, 320};
            for (const float view : kViews) {
                const float left = 32, right = view - 32;
                const bool laid = narrow_decoded && rebuild() &&
                                  ui_store_scene_commit(&scene, {0, 0, view, 700}, {left, 32, right, 700},
                                                        {0, 0, view, 700});
                for (u32 i = 0; i < scene.count; ++i)
                    parents[i] = kUiStoreNone;
                for (u32 i = 0; i < scene.count; ++i)
                    for (u16 c = scene.nodes[i].first; c != kUiStoreNone; c = scene.nodes[c].next)
                        parents[c] = static_cast<u16>(i);
                bool inside = laid;
                u32 texts = 0;
                for (u32 i = 0; i < scene.count; ++i) {
                    const auto& n = scene.nodes[i];
                    // A corner badge overhangs its card by design, as the official client's does.
                    if ((n.kind != UiStoreKind::text && n.kind != UiStoreKind::chip) || n.anchor)
                        continue;
                    u16 up = parents[i];
                    while (up != kUiStoreNone && scene.nodes[up].kind != UiStoreKind::card)
                        up = parents[up];
                    UiRect box{left, -1e9f, right, 1e9f};
                    if (up != kUiStoreNone) {
                        const auto& c = scene.nodes[up];
                        box = {c.x + c.pad_x, c.y + c.pad_y, c.x + c.w - c.pad_x, c.y + c.h - c.pad_y};
                    }
                    inside = inside && n.w > 0 && n.x >= box.left - .01f && n.x + n.w <= box.right + .01f &&
                             n.y >= box.top - .01f && n.y + n.h <= box.bottom + .01f;
                    ++texts;
                }
                const u32 title = find(UiStoreKind::text, utf8_literal("What do you want to do in this community?"));
                const u32 mark = find(UiStoreKind::text, utf8_literal("*"));
                check(inside && texts >= 6 && title < scene.count && scene.nodes[title].h >= 40 && mark < scene.count &&
                          scene.nodes[mark].x > scene.nodes[title].x + scene.nodes[title].w &&
                          find(UiStoreKind::text, utf8_literal("Choose all that apply.")) < scene.count,
                      view == 360 ? "community Channels & Roles at 360 DIPs and 200% wraps the question inside its card"
                                  : "community Channels & Roles at 320 DIPs keeps every text inside its card");
            }
            g_app.renderer.width = 1920;
            g_app.renderer.dpi = 96;
            // The Server Hub's Popular Games in pages from the narrowest (a 320 DIP view less 16 on each side) to the
            // widest (1072): stacked columns are the page's full width, and the covers fill one row of the card at
            // 96 DIPs or more each, inside its padding.
            static constexpr float kPages[]{288, 600, 639, 640, 1072};
            bool columns_full = true, covers_fit = true;
            for (const float page_width : kPages) {
                const auto columns = guild_hub_columns(page_width);
                const float sum = columns.main + columns.side + 16 - page_width;
                columns_full = columns_full && (page_width < 640 ? columns.main == page_width && columns.side == page_width
                                                                 : sum > -.01f && sum < .01f);
                const float cards[]{columns.main, columns.side};
                for (const float card_width : cards) {
                    const auto covers = guild_hub_covers(card_width);
                    ui_store_scene_begin(&scene);
                    UiStoreBuilder builder(&scene);
                    auto& plate = builder.open(UiStoreKind::card);
                    plate.pad_x = plate.pad_y = 16;
                    builder.open(UiStoreKind::row).gap = 12;
                    for (u32 i = 0; i < covers.count; ++i) {
                        builder.open(UiStoreKind::column).rw = covers.cell;
                        auto& cover = builder.open(UiStoreKind::preview);
                        cover.variant = static_cast<u8>(UiStoreAspect::fixed);
                        cover.rh = covers.cell * 4 / 3;
                        builder.close();
                        builder.close();
                    }
                    builder.close();
                    builder.close();
                    builder.close();
                    const bool laid = builder.ok() && ui_store_scene_commit(&scene, {0, 0, 1200, 700},
                                                                            {0, 0, card_width, 700}, {0, 0, 1200, 700});
                    const auto& card_node = scene.nodes[1];
                    for (u32 i = 2; laid && i < scene.count; ++i) {
                        const auto& n = scene.nodes[i];
                        if (n.kind == UiStoreKind::preview)
                            covers_fit = covers_fit && n.w == covers.cell && n.x >= card_node.x + 16 - .01f &&
                                         n.x + n.w <= card_node.x + card_node.w - 16 + .01f;
                    }
                    covers_fit = covers_fit && laid && covers.count >= 1 && covers.count <= kGuildSpaceGames &&
                                 covers.cell >= 96 && covers.count * covers.cell + 12 * (covers.count - 1) <= card_width - 32 + .01f;
                }
            }
            const auto narrowest = guild_hub_covers(288), widest = guild_hub_covers(guild_hub_columns(1072).main);
            check(columns_full && covers_fit && narrowest.count == 2 && widest.count == kGuildSpaceGames,
                  "community Server Hub stacks full-width columns and keeps Popular Games covers at 96 DIPs or more");
        }
        // The channel-list entry: Channels & Roles on a community server with questions, Browse Channels on one
        // without, nothing elsewhere.
        ui->chat_state.selected_guild_id = 80;
        ui->chat_state.home_selected = false;
        static NavigationView view;
        view = {};
        bool entry[3]{};
        bool browse_label[3]{};
        constexpr u32 features[]{guild_record_community | guild_record_onboarding_prompts, guild_record_community, 0};
        for (u32 i = 0; i < 3; ++i) {
            guilds[0].upload_features = features[i];
            application_guild_community_navigation(&view);
            entry[i] = view.channels_roles_row;
            browse_label[i] = view.browse_channels;
        }
        check(entry[0] && !browse_label[0] && entry[1] && browse_label[1] && !entry[2],
              "community the Channels & Roles entry follows the community and question features");
        // Browse Channels alone on a server without questions: uncategorized channels first, then each category's
        // name over its follow switch and its channels, in position order.
        const auto channel = [&](u64 id, u64 parent, u8 type, u32 position, Utf8View name) noexcept {
            ChannelInput input{id, 80, parent, name, position, 0, type};
            input.name_known = true;
            return chat_state_store_upsert_channel(&ui->chat_state, input).ok();
        };
        const bool channels_ready = channel(300, 0, 4, 0, utf8_literal("Talk")) &&
                                    channel(301, 300, 0, 1, utf8_literal("chat")) &&
                                    channel(302, 0, 0, 0, utf8_literal("rules"));
        chat_state_store_recompute_guild_permissions(&ui->chat_state, 80);
        reset_value(&retry_background);
        retry_background.account_scope = g_app.plugin_account_scope_serial;
        const bool browse_opened = channels_ready && application_guild_channels_roles_open(80) && rebuild();
        const u32 rules = find(UiStoreKind::text, utf8_literal("rules")),
                  talk = find(UiStoreKind::text, utf8_literal("Talk")),
                  follow = find(UiStoreKind::check, utf8_literal("Follow Category")),
                  chat = find(UiStoreKind::text, utf8_literal("chat")),
                  viewer = find(UiStoreKind::button, {}, action_view);
        const Utf8View subtitle = rules + 1 < scene.count ? scene.nodes[rules + 1].text : Utf8View{};
        check(browse_opened && page->browse_only && page->browse && page->phase == Phase::ready &&
                  !retry_background.queue_count && find(UiStoreKind::tab, {}, action_customize) == scene.count &&
                  find(UiStoreKind::text, utf8_literal("Choose Your Channels")) < rules &&
                  rules < talk && talk < follow && follow < chat && chat < scene.count && page->row_count == 3 &&
                  page->rows[0] == 302 && page->rows[1] == 300 && page->rows[2] == 301 &&
                  scene.nodes[find(UiStoreKind::card, {}, action_channel + 2)].variant ==
                      static_cast<u8>(UiStoreCard::row) &&
                  subtitle.bytes > 7 && bytes_equal(subtitle.data, "Active ", 7) && viewer < scene.count &&
                  scene.nodes[viewer].aux == action_channel,
              "community Browse Channels lists the introduction, categories with Follow Category and channel rows");
        activate(action_dismiss, {});
        check(rebuild() && find(UiStoreKind::text, utf8_literal("Choose Your Channels")) == scene.count,
              "community the Browse Channels introduction closes");
        g_app.renderer.width = saved_width;
        g_app.renderer.dpi = saved_dpi;
        ui_store_scene_release(&scene);
        application_content_page_close();
        page = nullptr;
        application_guild_community_close(false);
    }
    ui_settings_scene_release(&p.scene);
    for (u32 i = 0; i < kEditors; ++i)
        message_composer_release(&p.models[i]);
    owner = nullptr;
    page = nullptr;
    retry_owner = {};
    retry_background = {};
    g_app.account_ui = saved_ui;
    g_app.account.instance = saved_instance;
    g_account_fake.guild_community = saved_submit;
    g_app.account.status.gateway_ready = saved_ready;
    g_app.account.status.running = saved_running;
    g_app.login.visible = saved_login;
    g_app.renderer_ready = saved_renderer;
    g_app.text_form = saved_text_form;
    g_app.context_focus = saved_focus;
    lifetime_serial = saved_lifetime;
    VirtualFree(ui, 0, MEM_RELEASE);
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
    test_community_forms(check);
    retry_background = {};
    background = old_background;
    owner = old_owner;
}
} // namespace maicord::app_detail
