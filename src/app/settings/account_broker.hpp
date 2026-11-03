#pragma once
#include "maicord/app/media/account_gif_favorites.hpp"
#include "envelope.hpp"
#include "../account/account_ui_runtime.hpp"
#include "../shell/state.hpp"

namespace maicord::app_detail {

// Signed in, connected, with a profile and a self user and nothing required of the user: when an account setting may
// be asked for or written. User settings, emoji preferences and GIF favorites all wait for it.
__declspec(noinline) inline bool account_online() noexcept {
    return g_app.account.instance && g_app.account_ui && g_app.account.status.profile_id &&
           g_app.account_ui->chat_state.self_user_id && g_app.account.status.running &&
           !g_app.account.status.failed && g_app.account.status.required_action == AccountRequirement::none;
}

// One account's settings request as the brokers that fetch and patch an account setting (emoji preferences, GIF
// favorites) track it: the account it belongs to, the request in flight with its response buffer, and what the
// responses so far established.
struct AccountBroker {
    u8* response_body;
    u64 profile_id, self_user_id, request_id, gateway_revision;
    u32 generation, submitted_generation;
    AccountGifFavoritesOperation operation;
    u8 attempts;
    bool authoritative, fetch_wanted, was_running, receipt_consumed, admission_busy;
};

// The broker was bound to the open account.
__declspec(noinline) inline bool broker_scope(const AccountBroker& p) noexcept {
    return g_app.account.instance && g_app.account_ui && p.profile_id && p.self_user_id &&
           p.profile_id == g_app.account.status.profile_id &&
           p.self_user_id == g_app.account_ui->chat_state.self_user_id;
}
// Binds to the open account and starts following its gateway state.
__declspec(noinline) inline void broker_bind(AccountBroker& p) noexcept {
    p.profile_id = g_app.account.status.profile_id;
    p.self_user_id = g_app.account_ui->chat_state.self_user_id;
    p.gateway_revision = g_app.account.status.frecency_settings_revision;
    p.was_running = g_app.account.status.running;
}
// A request is in flight for this id.
inline bool broker_awaits(const AccountBroker* p, u64 request_id) noexcept {
    return p && p->request_id && p->request_id == request_id;
}
// The in-flight request's response arrived; its buffer is held until the response has been read.
inline void broker_received(AccountBroker& p) noexcept {
    p.request_id = 0;
    p.admission_busy = false;
    p.receipt_consumed = true;
}
// The REST client took the request.
inline void broker_admitted(AccountBroker& p, u64 request_id, AccountGifFavoritesOperation operation) noexcept {
    p.request_id = request_id;
    p.operation = operation;
    p.submitted_generation = p.generation;
    p.admission_busy = false;
}
// Wipes and frees the response buffer.
__declspec(noinline) inline void broker_drop_response(AccountBroker& p) noexcept {
    dispose(p.response_body, kRestClientMaximumResponseBytes);
    p.response_body = nullptr;
}
// Frees the response buffer once its response was read; whether it was.
inline bool broker_drain(AccountBroker* p) noexcept {
    if (!p || p->request_id || !p->receipt_consumed)
        return false;
    broker_drop_response(*p);
    p->receipt_consumed = false;
    return true;
}
inline Slice<u8> broker_response_buffer(const AccountBroker* p) noexcept {
    return p && p->response_body ? Slice<u8>{p->response_body, kRestClientMaximumResponseBytes} : Slice<u8>{};
}

} // namespace maicord::app_detail
