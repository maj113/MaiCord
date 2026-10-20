#include "../src/app/application_guild_community.cpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_system_time.hpp"

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
Owner retry_owner;
RestClientSubmitResult WINAPI community_submit(void*, const GuildCommunityRequest*, u64) noexcept {
    return {};
}
bool community_contains(Utf8View text, Utf8View part) noexcept {
    for (u32 i = 0; part.bytes <= text.bytes && i <= text.bytes - part.bytes; ++i)
        if (bytes_equal(text.data + i, part.data, part.bytes))
            return true;
    return false;
}
} // namespace
void application_guild_card_response(u64, bool join, const RestClientResponse& response,
                                     Utf8View) noexcept {
    ++retry_card_responses;
    retry_card_join = join;
    retry_card_status = response.status_code;
}

void test_community_forms(void (*check)(bool, const char*) noexcept) noexcept {
    auto* saved_ui = g_app.account_ui;
    const auto saved_instance = g_app.account.instance;
    const auto saved_submit = g_app.account.api.guild_community;
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
    g_app.account.api.guild_community = community_submit;
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
                                            {retry_background.scratch, 16384}, &p.form);
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
    check(guild_verification_decode(complete_rules, {retry_background.scratch, 16384}, &p.form),
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
               guild_onboarding_decode({p.form_body, writer.used}, {retry_background.scratch, 16384},
                                       &p.onboarding) &&
               !p.onboarding.truncated;
    };
    p.mode = Mode::channels_roles;
    p.phase = Phase::ready;
    p.status = {};
    const bool exact_controls = make_onboarding(2, 47, false);
    project_form();
    check(exact_controls && onboarding_presentation_complete(p) && p.control_count == kControlCapacity &&
              p.footer.can_save,
          "community complete presentation at the control limit remains editable");
    for (u32 mode = 0; mode < 2; ++mode) {
        p.mode = mode ? Mode::channels_roles : Mode::onboarding;
        p.phase = Phase::ready;
        p.status = {};
        const bool decoded = make_onboarding(mode ? 2u : 1u, mode ? 48u : 45u, !mode);
        p.flow_count = guild_onboarding_flow_prompts(p.onboarding, {p.flow, kGuildOnboardingPromptMaximum});
        p.step = p.flow_count ? p.flow_count - 1 : 0;
        check(decoded && !onboarding_presentation_complete(p),
              "community complete onboarding model can exceed text or control presentation limits");
        project_form();
        check(!p.footer.can_save && p.control_count == 1 && p.footer.status.bytes,
              "community incomplete onboarding presentation disables saving in either mode");
        p.footer.can_save = true;
        submit_modal();
        check(p.phase == Phase::ready && !retry_background.queue_count,
              "community stale footer cannot queue partial onboarding selections");
        p.phase = Phase::submitting;
        check(!build_body({80, 0,
                           mode ? GuildCommunityOperation::onboarding_update
                                : GuildCommunityOperation::onboarding_submit}),
              "community queued onboarding body rechecks its own presentation completeness");
    }
    const auto complete_onboarding = utf8_literal(
        R"({"enabled":true,"responses":["101","202"],"prompts":[{"id":"1","title":"First","required":true,"in_onboarding":true,"options":[{"id":"101","title":"Choice"}]},{"id":"2","title":"Later","required":true,"in_onboarding":false,"options":[{"id":"202","title":"Saved"}]}]})");
    check(guild_onboarding_decode(complete_onboarding, {retry_background.scratch, 16384}, &p.onboarding) &&
              !p.onboarding.truncated,
          "community complete saved onboarding fixture decodes");
    p.mode = Mode::channels_roles;
    p.phase = Phase::ready;
    p.status = {};
    project_form();
    submit_modal();
    const u32 edit_bytes = build_body({80, 0, GuildCommunityOperation::onboarding_update});
    check(p.phase == Phase::submitting && retry_background.queue_count == 1 && edit_bytes &&
              community_contains({reinterpret_cast<const char*>(retry_background.body), edit_bytes},
                                 utf8_literal(R"("onboarding_responses":["101","202"])")),
          "community complete edit preserves saved selections outside the initial flow");
    check(!build_body({80, 0, GuildCommunityOperation::verification_submit}) &&
              !build_body({80, 0, GuildCommunityOperation::onboarding_submit}),
          "community queued writes cannot serialize an incompatible current modal");
    p.onboarding.options[1].selected = false;
    check(build_body({80, 0, GuildCommunityOperation::onboarding_update}) != 0,
          "community edit may clear a choice whose required flag gates only the initial flow");
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
              community_contains({reinterpret_cast<const char*>(retry_background.body), initial_bytes},
                                 utf8_literal(R"("onboarding_responses":["101"])")),
          "community complete initial flow submits independently of nonflow required prompts");

    const GuildCommunityOperation requests[]{
        GuildCommunityOperation::verification_submit, GuildCommunityOperation::onboarding_submit,
        GuildCommunityOperation::onboarding_update,   GuildCommunityOperation::event_create,
        GuildCommunityOperation::verification_get,    GuildCommunityOperation::onboarding_get};
    const Mode modes[]{Mode::rules,  Mode::onboarding, Mode::channels_roles,
                       Mode::create, Mode::rules,      Mode::onboarding};
    for (u32 i = 0; i < 6; ++i) {
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

    GuildRecord guilds[1]{};
    ChannelRecord channels[1]{};
    RoleRecord roles[1]{};
    PermissionOverwriteRecord overwrites[1]{};
    u16 guild_order[1]{}, channel_order[1]{};
    u8 names[64]{};
    const bool initialized =
        chat_state_store_initialize(&ui->chat_state, {guilds, 1}, {guild_order, 1}, {channels, 1},
                                    {channel_order, 1}, {names, sizeof(names)}) &&
        chat_state_store_bind_permissions(&ui->chat_state, {roles, 1}, {overwrites, 1}) &&
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
        check(open_modal(Mode::rules, 80, utf8_literal("Rules again"), 0) && p.lifetime != previous &&
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
            p.guild_generation = 0;
            check(
                application_guild_community_open_create(80) && p.lifetime != create_lifetime &&
                    p.guild_generation == guilds[0].generation && retry_background.queue_count == 1 &&
                    retry_background.queue[0].op == GuildCommunityOperation::onboarding_get,
                "community active Create replacement refreshes identity and drops its prior queued write");
            p.phase = Phase::submitting;
            p.status = utf8_literal("Replacement event");
            check(application_guild_community_response(response, {}) && retry_background.queue_count == 1 &&
                      !retry_background.retry_at && p.phase == Phase::submitting &&
                      utf8_equal(p.status, utf8_literal("Replacement event")),
                  "community old Create 429 cannot retry after its replacement submits");
        }
        application_guild_community_close(false);
    }
    ui_settings_scene_release(&p.scene);
    for (u32 i = 0; i < kEditors; ++i)
        message_composer_release(&p.models[i]);
    owner = nullptr;
    retry_owner = {};
    retry_background = {};
    g_app.account_ui = saved_ui;
    g_app.account.instance = saved_instance;
    g_app.account.api.guild_community = saved_submit;
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
    check(retry_card_responses == 3, "community terminal joins each reach the card once");
    test_community_forms(check);
    retry_background = {};
    background = old_background;
    owner = old_owner;
}
} // namespace maicord::app_detail
