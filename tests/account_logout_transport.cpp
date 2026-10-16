// Offline fixture over the shipping REST owner. IOCP and its mailbox are real;
// plaintext responses are supplied at the existing transport callback boundary.
// No credential, worker thread, socket, DNS lookup or live HTTP is created.
#include "../src/app/rest_client.cpp"
#include "maicord/app/account_host.hpp"
#include <stddef.h>
#include "rest_client_probe.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool pass, const char* label) noexcept {
    test_check(&failures, pass, label);
}

void admission() noexcept {
    RestClientProbe p;
    check(p.initialize(), "logout: native owner initializes without network work");
    if (!p.runtime || !p.client.worker.thread)
        return;
    p.client.authentication_rejected = 1;
    rest_client_set_required_action(&p.client, AccountRequirement::captcha);
    constexpr char ordinary_target[] = "/api/v10/users/@me";
    const RestClientRequest ordinary{
        {reinterpret_cast<const u8*>(ordinary_target), sizeof(ordinary_target) - 1},
        {},
        {99, 0, RestMethod::post, RestMajorParameter::none},
        99,
        10'000,
        false,
        false};
    check(rest_client_submit(&p.client, ordinary).error == RestClientSubmitError::invalid_input &&
              p.client.request_state == request_state_empty,
          "logout: authentication rejection still blocks ordinary mutations");
    p.client.authentication_rejected = 0;
    check(rest_client_submit(&p.client, ordinary).error == RestClientSubmitError::account_restricted &&
              p.client.request_state == request_state_empty,
          "logout: action restriction still blocks ordinary mutations");
    p.client.authentication_rejected = 1;
    check(account_logout(&p.client, AccountLogoutOperation::begin, 101, nullptr).submit_error ==
                  RestClientSubmitError::none &&
              account_logout_request_valid(p.retained()) && p.runtime->command.timeout_ms == 10'000 &&
              !p.runtime->extended_body,
          "logout: fixed no-push operation is admitted despite authentication and action gates");
    const auto original = p.retained();
    check(utf8_equal({reinterpret_cast<const char*>(original.target.data), original.target.count},
                     utf8_literal("/api/v10/auth/logout")) &&
              utf8_equal({reinterpret_cast<const char*>(original.body.data), original.body.count},
                         utf8_literal("{\"provider\":null,\"voip_provider\":null}")),
          "logout: canonical endpoint and JSON copied into existing request owner");
    p.runtime->request_body[0] ^= 1;
    check(!account_logout_request_valid(original), "logout: equal-length JSON content is fenced");
    p.runtime->request_body[0] ^= 1;
    check(!reject_restricted_request(&p.client, p.runtime),
          "logout: worker recheck preserves the same narrow admission exception");
    auto wrong = original;
    wrong.route.route_id++;
    check(!account_logout_request_valid(wrong), "logout: route identity fence");
    wrong = original;
    wrong.route.method = RestMethod::get;
    check(!account_logout_request_valid(wrong), "logout: method fence");
    wrong = original;
    wrong.route.major_key = 1;
    check(!account_logout_request_valid(wrong), "logout: major key fence");
    wrong = original;
    wrong.route.major = RestMajorParameter::channel;
    check(!account_logout_request_valid(wrong), "logout: major kind fence");
    wrong = original;
    wrong.target.count--;
    check(!account_logout_request_valid(wrong), "logout: exact endpoint fence");
    wrong = original;
    wrong.body.count--;
    check(!account_logout_request_valid(wrong), "logout: exact JSON fence");
    wrong = original;
    wrong.json_body = false;
    check(!account_logout_request_valid(wrong), "logout: JSON framing fence");
    wrong = original;
    wrong.identity_response = true;
    check(!account_logout_request_valid(wrong), "logout: canonical response mode fence");
    wrong = original;
    wrong.request_id = 0;
    check(!account_logout_request_valid(wrong), "logout: nonzero request ticket fence");
    wrong = original;
    wrong.route.route_id++;
    check(rest_client_submit(&p.client, wrong).error == RestClientSubmitError::busy &&
              p.runtime->command.request_id == 101,
          "logout: reservation prevents a mutation from replacing its pending owner");
    check(!account_logout(&p.client, AccountLogoutOperation::cancel, 102, nullptr).cancelled &&
              p.client.request_state == request_state_pending &&
              account_logout(&p.client, AccountLogoutOperation::cancel, 101, nullptr).cancelled &&
              p.client.current_request_id == 101,
          "logout: exact cancellation retains terminal drain ticket");
    check(p.signal(), "logout: cancellation consumes only existing request notification");
    publish_request_error(&p.client, p.runtime, RestClientError::cancelled, 0);
    AccountLogoutResponse response{999};
    check(!account_logout(&p.client, AccountLogoutOperation::take, 102, &response).consumed &&
              response.request_id == 999 && p.client.request_state == request_state_ready,
          "logout: wrong response ID cannot consume or overwrite caller receipt");
    check(account_logout(&p.client, AccountLogoutOperation::take, 101, &response).consumed &&
              response.request_id == 101 && response.transport_error == RestClientError::cancelled &&
              !p.client.current_request_id && p.client.request_state == request_state_empty,
          "logout: cancelled receipt drains exactly once without a body or new allocation");
    check(!account_logout(&p.client, AccountLogoutOperation::take, 101, &response).consumed,
          "logout: terminal receipt cannot be taken twice");

    p.client.authentication_rejected = 0;
    check(account_logout(&p.client, AccountLogoutOperation::begin, 103, nullptr).submit_error ==
                  RestClientSubmitError::none &&
              p.signal(),
          "logout: second explicit fixture operation admitted");
    p.runtime->request_target[1] ^= 1;
    p.client.request_state = request_state_active;
    check(reject_restricted_request(&p.client, p.runtime) &&
              account_logout(&p.client, AccountLogoutOperation::take, 103, &response).consumed &&
              response.transport_error == RestClientError::account_restricted,
          "logout: worker independently rejects a changed endpoint under action restriction");
}

void receipts() noexcept {
    const u16 statuses[]{204, 401, 403, 429};
    const Utf8View bodies[]{{},
                            utf8_literal("{\"code\":40002}"),
                            utf8_literal("{\"captcha_key\":[\"challenge required\"]}"),
                            utf8_literal("{\"retry_after\":0.25,\"global\":false}")};
    for (u32 i = 0; i < 4; ++i) {
        RestClientProbe p;
        if (!p.initialize()) {
            check(false, "logout: response owner initialization");
            continue;
        }
        const u64 id = 200 + i;
        check(account_logout(&p.client, AccountLogoutOperation::begin, id, nullptr).submit_error ==
                      RestClientSubmitError::none &&
                  p.receive(statuses[i], bodies[i], i == 3),
              "logout: controlled plaintext completion uses shipping parser and publication");
        if (p.client.request_state != request_state_ready)
            continue;
        check(!rest_client_cancel_logout(&p.client, id),
              "logout: late cancel retains already-published result");
        if (statuses[i] == 401)
            check(rest_client_status(&p.client).authentication_rejected,
                  "logout:401 closes ordinary authentication admission before receipt drain");
        const auto responses = p.client.responses;
        AccountLogoutResponse response{};
        check(account_logout(&p.client, AccountLogoutOperation::take, id, &response).consumed &&
                  response.request_id == id && response.http_status == statuses[i] &&
                  response.transport_error == RestClientError::none &&
                  zeros({p.runtime->response_body, bodies[i].bytes}) && !p.client.current_request_id,
              "logout: exact terminal metadata survives authentication rejection and erases body");
        check((statuses[i] != 401 || response.rejection.has_code) &&
                  (statuses[i] != 403 || response.rejection.captcha) &&
                  (statuses[i] != 429 || (response.rate_limited && response.retry_at_ms > 1)),
              "logout: denial challenge and cooldown are terminal bounded metadata");
        for (u32 n = 0; n < 64; ++n)
            rest_client_status(&p.client);
        check(p.client.responses == responses && p.client.request_state == request_state_empty &&
                  !p.client.current_request_id,
              "logout: terminal failure or cooldown never automatically submits another operation");
    }
    RestClientProbe p;
    if (!p.initialize())
        return;
    check(account_logout(&p.client, AccountLogoutOperation::begin, 301, nullptr).submit_error ==
                  RestClientSubmitError::none &&
              p.signal(),
          "logout: transport failure fixture request admitted");
    p.client.request_state = request_state_active;
    publish_request_error(&p.client, p.runtime, RestClientError::timeout, WAIT_TIMEOUT);
    AccountLogoutResponse response{};
    check(account_logout(&p.client, AccountLogoutOperation::take, 301, &response).consumed &&
              response.transport_error == RestClientError::timeout &&
              response.native_error == WAIT_TIMEOUT && !response.http_status,
          "logout: ambiguous transport timeout remains a failure and never claims revocation");
}

void preemption() noexcept {
    constexpr char target[] = "/api/v10/users/@me";
    RestClientRequest ordinary{{reinterpret_cast<const u8*>(target), sizeof(target) - 1},
                               {},
                               {0x4C4F474F55545445ull, 0, RestMethod::get, RestMajorParameter::none},
                               501,
                               30'000,
                               false,
                               false};
    RestClientProbe p;
    if (!p.initialize()) {
        check(false, "logout: preemption owner initialization");
        return;
    }
    check(rest_client_submit(&p.client, ordinary).ok() &&
              account_logout(&p.client, AccountLogoutOperation::begin, 502, nullptr).submit_error ==
                  RestClientSubmitError::busy &&
              p.client.request_state == request_state_cancel_pending &&
              p.client.current_request_id == 501 && p.runtime->logout_preempt_id == 501 &&
              p.runtime->logout_reservation_id == 502 && !p.runtime->logout_request_id,
          "logout: busy PRIMARY reserves intent and cancels only the actual unrelated request");
    ordinary.request_id = 503;
    AccountLogoutResponse untouched{777};
    check(rest_client_submit(&p.client, ordinary).error == RestClientSubmitError::busy &&
              account_logout(&p.client, AccountLogoutOperation::begin, 502, nullptr).submit_error ==
                  RestClientSubmitError::busy &&
              !account_logout(&p.client, AccountLogoutOperation::take, 502, &untouched).consumed &&
              untouched.request_id == 777 && p.client.current_request_id == 501,
          "logout: reservation rejects ordinary publication and cannot steal an unrelated receipt");
    check(p.signal(), "logout: retained ordinary request notification consumed");
    publish_request_error(&p.client, p.runtime, RestClientError::cancelled, 0);
    check(account_logout(&p.client, AccountLogoutOperation::begin, 502, nullptr).submit_error ==
                  RestClientSubmitError::none &&
              p.client.current_request_id == 502 && p.runtime->logout_request_id == 502 &&
              !p.runtime->logout_preempt_id && account_logout_request_valid(p.retained()),
          "logout: late unrelated terminal result is discarded before one fixed POST admission");
    check(account_logout(&p.client, AccountLogoutOperation::begin, 502, nullptr).submit_error ==
                  RestClientSubmitError::busy &&
              p.client.request_state == request_state_pending && p.client.current_request_id == 502,
          "logout: repeated begin never cancels or repeats the admitted POST");
    check(p.receive(204, {}) &&
              account_logout(&p.client, AccountLogoutOperation::take, 502, &untouched).consumed &&
              !p.runtime->logout_reservation_id,
          "logout: exact owned terminal consumption releases reservation");

    RestClientProbe ready;
    if (!ready.initialize()) {
        check(false, "logout: ready preemption owner initialization");
        return;
    }
    ordinary.request_id = 601;
    check(rest_client_submit(&ready.client, ordinary).ok() && ready.signal(),
          "logout: unrelated ready receipt fixture admitted");
    ready.client.request_state = request_state_active;
    memset(ready.runtime->response_body, 'X', 128);
    ready.runtime->response.http.body_bytes = 128;
    RestClientResponse old{};
    old.status_code = 200;
    old.body_bytes = 128;
    publish_completed(&ready.client, ready.runtime, old);
    check(account_logout(&ready.client, AccountLogoutOperation::begin, 602, &untouched).submit_error ==
                  RestClientSubmitError::none &&
              untouched.request_id == 502 && ready.client.current_request_id == 602 &&
              account_logout_request_valid(ready.retained()) &&
              zeros({ready.runtime->request_body + ready.runtime->command.body_bytes,
                     128 - ready.runtime->command.body_bytes}),
          "logout: preexisting ready plaintext is erased without copying into logout output");

    RestClientProbe cancelled;
    if (!cancelled.initialize()) {
        check(false, "logout: pre-admission cancellation owner initialization");
        return;
    }
    ordinary.request_id = 701;
    check(rest_client_submit(&cancelled.client, ordinary).ok() &&
              account_logout(&cancelled.client, AccountLogoutOperation::begin, 702, nullptr).submit_error ==
                  RestClientSubmitError::busy &&
              account_logout(&cancelled.client, AccountLogoutOperation::cancel, 702, nullptr).cancelled &&
              account_logout(&cancelled.client, AccountLogoutOperation::begin, 702, nullptr).submit_error ==
                  RestClientSubmitError::invalid_input &&
              !cancelled.runtime->logout_request_id && cancelled.client.current_request_id == 701,
          "logout: cancellation during quiescence prevents later POST until worker join");
}

void abi() noexcept {
    static_assert(kAccountModuleApiVersion == 132 && kAccountModuleApiOrdinal == 132);
    static_assert(sizeof(AccountModuleApi) == 1176 && sizeof(AccountHost) == 1424);
    static_assert(offsetof(AccountModuleApi, logout) == 616);
    static_assert(offsetof(AccountLogoutResponse, http_status) == 28);
    AccountHost host{};
    check(account_host_load(&host).ok(), "logout: ABI94 dormant module loads without account startup");
    if (!host.module)
        return;
    auto missing = host.api;
    missing.logout = nullptr;
    check(!account_host_api_valid(missing), "logout: callback is mandatory at shared ABI validation");
    AccountLogoutResponse response{777};
    constexpr AccountLogoutOperation operations[]{
        AccountLogoutOperation::begin, AccountLogoutOperation::cancel, AccountLogoutOperation::take};
    for (const auto operation : operations)
        check(host.api.logout(nullptr, operation, 1, &response).submit_error ==
                      RestClientSubmitError::invalid_input &&
                  response.request_id == 777,
              "logout: dormant callback cannot allocate owner or overwrite response");
    const auto get_api = reinterpret_cast<AccountModuleGetApiFunction>(
        GetProcAddress(host.module, MAKEINTRESOURCEA(kAccountModuleApiOrdinal)));
    AccountModuleApi previous{};
    previous.bytes = 0x12345678;
    check(get_api && !get_api(89, &previous) && previous.bytes == 0x12345678,
          "logout: previous ABI rejects before touching caller bank");
    account_host_release(&host);
}
} // namespace
} // namespace maicord

extern "C" void WINAPI maicord_account_logout_transport_test_entry() noexcept {
    maicord::admission();
    maicord::receipts();
    maicord::preemption();
    maicord::abi();
    ExitProcess(maicord::failures ? 1u : 0u);
}
