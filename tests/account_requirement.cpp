#include "maicord/protocol/account_requirement.hpp"
#include "maicord/app/user_account_coordinator.hpp"
#include "maicord/app/account_host.hpp"
#include "maicord/ui/message_composer_scene.hpp"

namespace maicord {
void test_account_requirement(void (*check)(bool, const char*) noexcept) noexcept {
    u32 scratch[512]{};
    AccountRequirement action = AccountRequirement::none;
    const auto update = [&](Utf8View source) noexcept {
        return account_requirement_update({source, 0, JsonValueKind::object}, {scratch, 512}, &action);
    };
    check(update(utf8_literal(R"({"required_action":"REQUIRE_VERIFIED_PHONE"})")) &&
              action == AccountRequirement::verify_phone,
          "required phone verification is retained without storing arbitrary server text");
    check(!update(utf8_literal(R"({"other":null})")) && action == AccountRequirement::verify_phone &&
              !update(utf8_literal(R"({"required_action":null,"required_action":null})")) &&
              action == AccountRequirement::verify_phone &&
              !update(utf8_literal(R"({"required_action":null)")) &&
              action == AccountRequirement::verify_phone,
          "missing duplicate or malformed updates never clear a known requirement");
    check(update(utf8_literal(R"({"required_action":"NEW_SERVER_ACTION"})")) &&
              action == AccountRequirement::other && account_requirement_notice(action).bytes != 0 &&
              update(utf8_literal(R"({"required_action":17})")) && action == AccountRequirement::other,
          "unknown required actions keep a generic verification notice");
    check(update(utf8_literal(R"({"required_action":"REQUIRE_VERIFIED_EMAI\u004c"})")) &&
              action == AccountRequirement::verify_email &&
              update(utf8_literal(R"({"required_action":null})")) && action == AccountRequirement::none,
          "escaped known actions decode and explicit null clears the restriction");
    const auto ready_body = utf8_literal(
        R"({"v":9,"user":{"id":"1"},"session_id":"session","resume_gateway_url":"wss://gateway.discord.gg","guilds":[],"required_action":"REQUIRE_SAFETY_FLOWS"})");
    GatewayReady ready{};
    check(gateway_decode_ready({ready_body, 0, JsonValueKind::object}, {scratch, 512}, 32, &ready).ok() &&
              ready.required_action == AccountRequirement::safety_flows,
          "READY carries the initial account requirement");
    GatewayEnvelope envelope{};
    envelope.opcode = 0;
    envelope.has_event_name = true;
    envelope.event_name = {utf8_literal("USER_REQUIRED_ACTION_UPDATE"), 0, false};
    envelope.data = {utf8_literal(R"({"required_action":"REQUIRE_CAPTCHA"})"), 0, JsonValueKind::object};
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    check(gateway_client_route_dispatch(envelope, {scratch, 512}, 32, {}, {}, &event, &payload).ok() &&
              event.dispatch_kind == GatewayClientDispatchKind::account_required_action &&
              event.required_action == AccountRequirement::captcha && !payload.count,
          "worker routing carries a requirement by value with no borrowed payload");

    UserAccountCoordinator account{};
    account.state.phase = UserAccountCoordinatorPhase::running;
    account.gateway.phase = static_cast<LONG>(GatewayClientPhase::ready);
    account.gateway.required_action = static_cast<LONG>(AccountRequirement::safety_flows);
    check(user_account_coordinator_pump(&account).ok() &&
              account.state.phase == UserAccountCoordinatorPhase::running &&
              account.rest.required_action == static_cast<LONG>(AccountRequirement::safety_flows),
          "verification keeps the session readable and propagates to the REST worker");
    constexpr RestMethod reads[] = {RestMethod::get, RestMethod::head};
    constexpr RestMethod writes[] = {RestMethod::post, RestMethod::put, RestMethod::patch,
                                     RestMethod::delete_method};
    for (const auto method : reads)
        check(account_request_allowed(AccountRequirement::safety_flows, method),
              "reads remain allowed during verification");
    for (const auto method : writes)
        check(!account_request_allowed(AccountRequirement::safety_flows, method),
              "writes are refused during verification");
    account.gateway.required_action = 0;
    check(user_account_coordinator_pump(&account).ok() && account.rest.required_action == 0,
          "server clearing the requirement restores write admission without a reconnect");
    AccountHost host{};
    AccountModuleStatus status{};
    status.required_action = AccountRequirement::verify_email;
    check((account_host_observe(&host, status) & account_host_observation_phase) &&
              !account_host_observe(&host, status),
          "requirement changes notify once without a polling loop");

    ApiRejection rejection{};
    check(api_rejection_decode(
              utf8_literal(R"({"code":40004,"message":"do not retain","errors":{"code":123}})"),
              {scratch, 512}, &rejection) &&
              rejection.has_code && rejection.code == 40004 && !rejection.captcha &&
              api_rejection_requires_action(rejection) && api_rejection_notice(rejection).bytes,
          "only root numeric API errors become restriction diagnostics");
    check(
        api_rejection_decode(
            utf8_literal(
                R"({"captcha_key":["required"],"captcha_rqtoken":"DO_NOT_STORE","captcha_sitekey":"DO_NOT_STORE"})"),
            {scratch, 512}, &rejection) &&
            !rejection.has_code && rejection.captcha && api_rejection_requires_action(rejection),
        "captcha detection retains presence only without needing an API code");
    check(api_rejection_decode(utf8_literal(R"({"code":0,"captcha_key":null,"errors":{"code":40002}})"),
                               {scratch, 512}, &rejection) &&
              rejection.has_code && !rejection.code && !rejection.captcha &&
              !api_rejection_requires_action(rejection),
          "general and nested errors do not imply an account restriction");
    constexpr Utf8View invalid[] = {
        utf8_literal(R"({"code":40002,"code":0})"), utf8_literal(R"({"code":-1})"),
        utf8_literal(R"({"code":4294967296})"),     utf8_literal(R"({"code":"40004"})"),
        utf8_literal("<html>error</html>"),         utf8_literal(R"({"code":40004} {})")};
    for (auto body : invalid)
        check(!api_rejection_decode(body, {scratch, 512}, &rejection) && !rejection.has_code &&
                  !rejection.captcha,
              "malformed error responses never leave stale diagnostics");

    char text[128]{};
    MessageComposer composer{};
    MessageComposerScene scene{};
    const bool initialized = message_composer_initialize(&composer, {text, sizeof(text)}) &&
                             message_composer_scene_initialize_owned(&scene);
    check(initialized, "read-only account composer fixture initializes");
    if (initialized) {
        message_composer_reset(&composer, 7, false);
        message_composer_insert_utf8(&composer, utf8_literal("keep this draft"));
        const auto notice = account_requirement_notice(AccountRequirement::safety_flows);
        check(message_composer_scene_update(&scene, &composer, {0, 0, 640, 110}, true, 9,
                                            utf8_literal("Reply"), nullptr, notice, {{}, false, true}, true)
                      .ok() &&
                  !message_composer_scene_can_send(scene) && !scene.can_attach &&
                  message_composer_scene_focus_edit(&scene) &&
                  message_composer_insert_utf8(&composer, utf8_literal("!")) ==
                      MessageComposerResult::changed &&
                  utf8_equal(message_composer_text(composer), utf8_literal("keep this draft!")) &&
                  scene.typing_label_bytes == notice.bytes,
              "restriction disables sending and uploads while draft editing and reply context remain "
              "available");
        check(message_composer_scene_update(&scene, &composer, {0, 0, 640, 110}, true).ok() &&
                  message_composer_scene_can_send(scene) &&
                  utf8_equal(message_composer_text(composer), utf8_literal("keep this draft!")),
              "clearing restriction preserves the draft and makes the send button available");
    }
    message_composer_scene_release(&scene);
}
} // namespace maicord
