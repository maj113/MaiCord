#include "application_guild_community.hpp"
#include "application_guild_invite.hpp"
#include "application_link_trust.hpp"
#include "application_action_captcha.hpp"
#include "application_content_input.hpp"
#include "application_message_history.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/guild_invite.hpp"
#include "maicord/ui/message_markdown.hpp"


namespace maicord::app_detail {
namespace {
enum class InviteState : u8 {
    idle,
    resolve_queued,
    resolving,
    resolved,
    accept_queued,
    accepting,
    awaiting_guild, // accepted; waiting for GUILD_CREATE before switching
    unknown,
    captcha,
    banned,
    failed,
};
struct InviteOwner {
    u64 scope;
    u64 request_id;
    u64 revision;
    u64 retry_at_ms;
    GuildInvite invite;
    char code[kGuildInviteCodeMaximumBytes];
    char status[320];
    u16 status_bytes;
    u8 code_bytes;
    InviteState state;
    bool dialog;
    bool captcha_retry;
    ApplicationActionCaptchaPhase captcha_phase;
};
InviteOwner g_invite{};
u32 g_scratch[8192];

// Nonzero key of the invite a CAPTCHA belongs to.
u64 invite_subject() noexcept {
    u64 hash = 1469598103934665603ull;
    for (u32 i = 0; i < g_invite.code_bytes; ++i)
        hash = (hash ^ static_cast<u8>(g_invite.code[i])) * 1099511628211ull;
    return hash | 1;
}
ApplicationActionCaptchaPhase invite_captcha_phase() noexcept {
    return application_action_captcha_phase(ApplicationActionCaptchaKind::guild_invite, invite_subject());
}

bool member_of(u64 guild_id) noexcept {
    return guild_id && g_app.account_ui && chat_state_store_find_guild(g_app.account_ui->chat_state, guild_id);
}
bool scope_current() noexcept {
    return g_app.account_ui && g_app.account.instance && g_invite.scope == g_app.plugin_account_scope_serial;
}
void append(u32* used, Utf8View text) noexcept {
    const u32 bytes = minimum<u32>(text.bytes, sizeof(g_invite.status) - *used);
    copy_bytes(g_invite.status + *used, text.data, bytes);
    *used += bytes;
}
void append_count(u32* used, u32 value) noexcept {
    char digits[16];
    append(used, {digits, decimal_write_grouped(digits, value)});
}
// Status copy mirrors the official client's invite card: name, then online/member counts.
void publish(InviteState state) noexcept {
    g_invite.state = state;
    u32 used = 0;
    const auto& invite = g_invite.invite;
    switch (state) {
    case InviteState::resolve_queued:
    case InviteState::resolving:
        append(&used, utf8_literal("Resolving invite\xE2\x80\xA6"));
        break;
    case InviteState::resolved:
    case InviteState::accept_queued:
    case InviteState::accepting:
    case InviteState::awaiting_guild:
        append(&used, {invite.name, invite.name_bytes});
        if (invite.counts_known) {
            append(&used, utf8_literal("\n"));
            append_count(&used, invite.presence_count);
            append(&used, utf8_literal(" online \xC2\xB7 "));
            append_count(&used, invite.member_count);
            append(&used, invite.member_count == 1 ? utf8_literal(" member") : utf8_literal(" members"));
        }
        if (state == InviteState::accepting || state == InviteState::accept_queued)
            append(&used, utf8_literal("\nJoining\xE2\x80\xA6"));
        else if (state == InviteState::awaiting_guild)
            append(&used, utf8_literal("\nJoined. Opening the server\xE2\x80\xA6"));
        break;
    case InviteState::unknown:
        append(&used, utf8_literal("Invalid invite. It may have expired. Ask for a new invite."));
        break;
    case InviteState::captcha: {
        const auto phase = invite_captcha_phase();
        g_invite.captcha_phase = phase;
        append(&used, phase != ApplicationActionCaptchaPhase::none
                          ? application_action_captcha_status(phase)
                          : utf8_literal("Discord wants a CAPTCHA before you can join. Open the invite in your browser."));
        break;
    }
    case InviteState::banned:
        append(&used, utf8_literal("You're banned from this server."));
        break;
    case InviteState::failed:
        append(&used, utf8_literal("Couldn't load this invite. Try again."));
        break;
    case InviteState::idle:
        break;
    }
    g_invite.status_bytes = static_cast<u16>(used);
    ++g_invite.revision;
    application_request_frame();
}

bool form_valid(TextFormOwnerKey key) noexcept {
    return g_invite.dialog && key.scope == g_invite.scope && scope_current();
}
// discord.gg is a trusted host, so the link flow opens it without a warning.
void open_in_browser() noexcept {
    char url[64] = "https://discord.gg/";
    u32 at = 19;
    for (u32 i = 0; i < g_invite.code_bytes && at < sizeof(url); ++i)
        url[at++] = g_invite.code[i];
    application_link_open({url, at}, true);
}
bool form_save(TextFormOwnerKey key, Utf8View) noexcept {
    if (!form_valid(key))
        return true;
    switch (g_invite.state) {
    case InviteState::resolved:
        if (member_of(g_invite.invite.guild_id))
            return select_content_guild(g_invite.invite.guild_id) || true;
        publish(InviteState::accept_queued);
        pump_message_history();
        return false;
    case InviteState::captcha:
        if (invite_captcha_phase() == ApplicationActionCaptchaPhase::none) {
            open_in_browser();
            return true;
        }
        application_action_captcha_verify(ApplicationActionCaptchaKind::guild_invite, invite_subject());
        publish(InviteState::captcha);
        pump_message_history();
        return false;
    case InviteState::failed:
        publish(InviteState::resolve_queued);
        pump_message_history();
        return false;
    default:
        return false;
    }
}
ApplicationTextFormProgress form_progress(TextFormOwnerKey) noexcept {
    ApplicationTextFormProgress progress{};
    progress.status = {g_invite.status, g_invite.status_bytes};
    progress.revision = g_invite.revision;
    progress.editable = false;
    const auto state = g_invite.state;
    const bool joined = member_of(g_invite.invite.guild_id);
    const bool verifying = state == InviteState::captcha && g_invite.captcha_phase != ApplicationActionCaptchaPhase::none;
    progress.accept = verifying                      ? utf8_literal("Verify in browser")
                      : state == InviteState::captcha ? utf8_literal("Open in browser")
                      : state == InviteState::failed ? utf8_literal("Try again")
                      : joined                       ? utf8_literal("Joined")
                                                     : utf8_literal("Join");
    progress.can_accept = state == InviteState::resolved || state == InviteState::failed ||
                          (state == InviteState::captcha && g_invite.captcha_phase != ApplicationActionCaptchaPhase::waiting &&
                           g_invite.captcha_phase != ApplicationActionCaptchaPhase::solved);
    progress.complete = !scope_current();
    return progress;
}
void form_closed(TextFormOwnerKey) noexcept {
    g_invite.dialog = false;
    // An accepted invite still switches to the guild once it arrives.
    if (g_invite.state != InviteState::awaiting_guild && g_invite.state != InviteState::accepting) {
        if (g_invite.state == InviteState::captcha)
            application_action_captcha_cancel();
        g_invite.state = InviteState::idle;
    }
}
} // namespace

bool application_guild_invite_open_url(Utf8View url) noexcept {
    InviteLink link{};
    if (!message_markdown_invite_link(url, &link))
        return false;
    // An event link opens the event in a server the account is in; others
    // fall back to the browser.
    if (invite_link_event(link))
        return member_of(link.event_guild_id) &&
               (application_guild_community_open_event(link.event_guild_id, link.event_id) || true);
    application_guild_invite_open({link.code, link.code_bytes});
    return true;
}

bool application_guild_invite_open(Utf8View code) noexcept {
    if (!g_app.account_ui || !g_app.account.instance || !g_app.account.api.guild_invite ||
        !guild_invite_code_valid(code.data, code.bytes) || g_app.text_form)
        return false;
    g_invite = {};
    g_invite.scope = g_app.plugin_account_scope_serial;
    g_invite.code_bytes = static_cast<u8>(code.bytes);
    copy_bytes(g_invite.code, code.data, code.bytes);
    publish(InviteState::resolve_queued);
    ApplicationTextFormRequest request{utf8_literal("You've been invited to join a server"),
                                       {},
                                       {},
                                       utf8_literal("Join"),
                                       {},
                                       {g_invite.scope, 0x494e56, 1},
                                       form_valid,
                                       form_save,
                                       0,
                                       true};
    request.progress = form_progress;
    request.closed = form_closed;
    g_invite.dialog = true;
    if (!application_text_form_open(request)) {
        g_invite = {};
        return false;
    }
    pump_message_history();
    return true;
}

bool application_guild_invite_pending(u64 now_ms) noexcept {
    if (scope_current() && g_invite.state == InviteState::captcha) {
        const auto phase = invite_captcha_phase();
        if (phase == ApplicationActionCaptchaPhase::solved) {
            // The one armed retry of the same accept.
            g_invite.captcha_retry = true;
            publish(InviteState::accept_queued);
        } else if (phase != g_invite.captcha_phase) {
            publish(InviteState::captcha);
        }
    }
    return scope_current() && now_ms >= g_invite.retry_at_ms &&
           (g_invite.state == InviteState::resolve_queued || g_invite.state == InviteState::accept_queued);
}

bool application_guild_invite_submit(u64 request_id, u64 now_ms) noexcept {
    if (!request_id || !application_guild_invite_pending(now_ms))
        return false;
    GuildInviteRequest request{};
    copy_bytes(request.code, g_invite.code, g_invite.code_bytes);
    request.code_bytes = g_invite.code_bytes;
    const bool accept = g_invite.state == InviteState::accept_queued;
    request.operation = accept ? GuildInviteOperation::accept : GuildInviteOperation::resolve;
    if (accept) {
        request.location_guild_id = g_invite.invite.guild_id;
        request.location_channel_id = g_invite.invite.channel_id;
        request.location_channel_type = g_invite.invite.channel_type;
    }
    const bool retry = accept && g_invite.captcha_retry;
    g_invite.captcha_retry = false;
    if (retry && !application_action_captcha_arm(ApplicationActionCaptchaKind::guild_invite, invite_subject(),
                                                 request_id, true)) {
        publish(InviteState::failed);
        return false;
    }
    const auto submitted = g_app.account.api.guild_invite(g_app.account.instance, &request, request_id);
    if (!submitted.ok()) {
        if (submitted.error == RestClientSubmitError::busy)
            g_invite.retry_at_ms = now_ms + 50;
        else
            publish(InviteState::failed);
        return false;
    }
    g_invite.request_id = request_id;
    g_invite.state = accept ? InviteState::accepting : InviteState::resolving;
    return true;
}

bool application_guild_invite_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!scope_current() || response.request_id != g_invite.request_id ||
        (g_invite.state != InviteState::resolving && g_invite.state != InviteState::accepting))
        return true;
    const bool accept = g_invite.state == InviteState::accepting;
    g_invite.request_id = 0;
    GuildInvite decoded{};
    auto outcome = response.transport_ok() ? guild_invite_decode_response(response.status_code, body,
                                                                          {g_scratch, 8192}, &decoded)
                                           : GuildInviteOutcome::unavailable;
    if (response.transport_ok() && response.rejection.captcha)
        outcome = GuildInviteOutcome::captcha;
    switch (outcome) {
    case GuildInviteOutcome::resolved:
        if (!accept || decoded.guild_id == g_invite.invite.guild_id || !g_invite.invite.guild_id)
            g_invite.invite = decoded;
        publish(accept ? InviteState::awaiting_guild : InviteState::resolved);
        break;
    case GuildInviteOutcome::unknown:
        publish(InviteState::unknown);
        break;
    case GuildInviteOutcome::captcha:
        if (accept)
            application_action_captcha_capture(ApplicationActionCaptchaKind::guild_invite, invite_subject(),
                                               response, body);
        publish(InviteState::captcha);
        break;
    case GuildInviteOutcome::banned:
        publish(InviteState::banned);
        break;
    case GuildInviteOutcome::rate_limited:
        if (response.rate_limited && response.retry_at_ms &&
            !api_rejection_requires_action(response.rejection)) {
            g_invite.retry_at_ms = response.retry_at_ms;
            publish(accept ? InviteState::accept_queued : InviteState::resolve_queued);
        } else {
            publish(InviteState::failed);
        }
        break;
    default:
        log_u64(LogLevel::warning, "invite", "response_status", response.status_code);
        // An accept that may have succeeded server-side waits for GUILD_CREATE.
        publish(accept && response.transport_ok() && response.status_code < 300 ? InviteState::awaiting_guild
                                                                               : InviteState::failed);
        break;
    }
    return true;
}

void application_guild_invite_synchronize() noexcept {
    if (g_invite.state != InviteState::awaiting_guild || !scope_current() || !member_of(g_invite.invite.guild_id))
        return;
    const u64 guild = g_invite.invite.guild_id;
    g_invite.state = InviteState::idle;
    ++g_invite.revision;
    if (g_invite.dialog)
        application_text_form_close(false);
    select_content_guild(guild);
    // Rules or onboarding follow a join, as in the official client.
    application_guild_community_joined(guild);
}

void application_guild_invite_release() noexcept {
    g_invite = {};
}

} // namespace maicord::app_detail
