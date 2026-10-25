#include "application_message_mutation_notice.hpp"
#include "maicord/core/memory.hpp"
#include "account_ui_runtime.hpp"
#include "application_state.hpp"
#include "application_message_navigation.hpp"

namespace maicord::app_detail {
namespace {
constexpr u32 index(ApplicationMessageMutationKind kind) noexcept {
    return static_cast<u32>(kind);
}
bool equal(const ApplicationMessageMutationScope& a, const ApplicationMessageMutationScope& b) noexcept {
    return a.account_serial == b.account_serial && a.profile_id == b.profile_id &&
           a.self_user_id == b.self_user_id && a.channel_id == b.channel_id &&
           a.channel_generation == b.channel_generation && a.message_id == b.message_id;
}
ApplicationMessageMutationScope scope(u64 channel_id, u64 message_id, bool any_channel = false) noexcept {
    if (!g_app.account_ui || !g_app.account.instance || g_app.account.stop_failed || g_app.login.visible ||
        !g_app.plugin_account_scope_serial || !g_app.account.status.profile_id || !channel_id ||
        !message_id || (!any_channel && g_app.account_ui->active_channel_id != channel_id) ||
        !g_app.account_ui->chat_state.self_user_id)
        return {};
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id);
    return channel && channel->occupied
               ? ApplicationMessageMutationScope{g_app.plugin_account_scope_serial,
                                                 g_app.account.status.profile_id,
                                                 g_app.account_ui->chat_state.self_user_id,
                                                 channel_id,
                                                 channel->generation,
                                                 message_id}
               : ApplicationMessageMutationScope{};
}
bool current(const ApplicationMessageMutationScope& value, bool any_channel = false) noexcept {
    return value.account_serial && equal(value, scope(value.channel_id, value.message_id, any_channel));
}
ApplicationMessageMutationScope* request(ApplicationMessageMutationKind kind, u64 channel_id,
                                         u64 message_id) noexcept {
    if (index(kind) >= kApplicationMessageMutationKinds)
        return nullptr;
    auto& value = g_app.message_mutation_notice.requests[index(kind)];
    if (value.channel_id != channel_id || value.message_id != message_id ||
        !current(value, kind == ApplicationMessageMutationKind::mark_unread ||
                            kind == ApplicationMessageMutationKind::navigate_message))
        return nullptr;
    return &value;
}
void clear_shown() noexcept {
    auto& value = g_app.message_mutation_notice;
    reset_value(&value.shown);
    value.phase = ApplicationMessageMutationNoticePhase::empty;
    reset_value(&value.rejection);
    value.status_code = 0;
    value.transport_failed = false;
    value.navigation_stage = ApplicationMessageNavigationStage::idle;
}
Utf8View navigation_text(ApplicationMessageNavigationStage stage, bool failed, u16 status) noexcept {
    using Stage = ApplicationMessageNavigationStage;
    if (failed) {
        switch (stage) {
        case Stage::failure_parent:
            return utf8_literal("Couldn't load channel. Select link to retry.");
        case Stage::failure_membership:
            return utf8_literal("Couldn't check access. Select link to retry.");
        case Stage::permission_denied:
            return utf8_literal("You cannot read this message.");
        case Stage::source_changed:
            return utf8_literal("The message changed. Select the link again.");
        case Stage::message_missing:
            return utf8_literal("This message is unavailable.");
        default:
            return status == 403   ? utf8_literal("You cannot read this message.")
                   : status == 404 ? utf8_literal("This message is unavailable.")
                                   : utf8_literal("Couldn't open message. Select link to retry.");
        }
    }
    switch (stage) {
    case Stage::waiting_channel:
    case Stage::loading_channel:
    case Stage::loading_parent:
        return utf8_literal("Loading channel… Esc to cancel");
    case Stage::waiting_membership:
        return utf8_literal("Checking access… Esc to cancel");
    case Stage::rate_limited:
        return utf8_literal("Retrying… Esc to cancel");
    case Stage::busy:
        return utf8_literal("Another message is opening.");
    case Stage::cancelled:
        return utf8_literal("Opening cancelled.");
    default:
        return utf8_literal("Opening message… Esc to cancel");
    }
}
} // namespace
void application_message_mutation_notice_begin(ApplicationMessageMutationKind kind, u64 channel_id,
                                               u64 message_id) noexcept {
    if (index(kind) >= kApplicationMessageMutationKinds)
        return;
    auto& value = g_app.message_mutation_notice;
    value.requests[index(kind)] = scope(channel_id, message_id,
                                        kind == ApplicationMessageMutationKind::mark_unread ||
                                            kind == ApplicationMessageMutationKind::navigate_message);
    // A new action dismisses the previous transient result.
    clear_shown();
}
void application_message_mutation_notice_pending(ApplicationMessageMutationKind kind, u64 channel_id,
                                                 u64 message_id) noexcept {
    application_message_mutation_notice_pending(kind, channel_id, message_id, 0);
}
void application_message_mutation_notice_pending(ApplicationMessageMutationKind kind, u64 channel_id,
                                                 u64 message_id, u16 status_code) noexcept {
    const auto* owner = request(kind, channel_id, message_id);
    if (!owner)
        return;
    auto& value = g_app.message_mutation_notice;
    value.shown = *owner;
    value.kind = kind;
    value.phase = ApplicationMessageMutationNoticePhase::pending;
    value.status_code = status_code;
    value.navigation_stage = kind == ApplicationMessageMutationKind::navigate_message
                                 ? (status_code == 429 ? ApplicationMessageNavigationStage::rate_limited
                                                       : ApplicationMessageNavigationStage::opening_history)
                                 : ApplicationMessageNavigationStage::idle;
}
void application_message_mutation_notice_navigation_pending(u64 channel_id, u64 message_id,
                                                            ApplicationMessageNavigationStage stage,
                                                            u16 status_code) noexcept {
    if (!request(ApplicationMessageMutationKind::navigate_message, channel_id, message_id) ||
        stage > ApplicationMessageNavigationStage::message_missing)
        return;
    application_message_mutation_notice_pending(ApplicationMessageMutationKind::navigate_message,
                                                channel_id, message_id, status_code);
    g_app.message_mutation_notice.navigation_stage = stage;
}
void application_message_mutation_notice_navigation_failed(
    u64 channel_id, u64 message_id, ApplicationMessageNavigationStage stage,
    const RestClientResponse& response) noexcept {
    if (!request(ApplicationMessageMutationKind::navigate_message, channel_id, message_id) ||
        stage > ApplicationMessageNavigationStage::message_missing)
        return;
    application_message_mutation_notice_failed(ApplicationMessageMutationKind::navigate_message, channel_id,
                                               message_id, response);
    g_app.message_mutation_notice.navigation_stage = stage;
}
void application_message_mutation_notice_failed(ApplicationMessageMutationKind kind, u64 channel_id,
                                                u64 message_id,
                                                const RestClientResponse& response) noexcept {
    auto* owner = request(kind, channel_id, message_id);
    if (!owner)
        return;
    auto& value = g_app.message_mutation_notice;
    value.shown = *owner;
    value.kind = kind;
    value.phase = ApplicationMessageMutationNoticePhase::failed;
    value.rejection = response.rejection;
    value.status_code = response.status_code;
    value.transport_failed = !response.transport_ok();
    value.navigation_stage = kind == ApplicationMessageMutationKind::navigate_message
                                 ? ApplicationMessageNavigationStage::error
                                 : ApplicationMessageNavigationStage::idle;
    reset_value(owner);
}
void application_message_mutation_notice_succeeded(ApplicationMessageMutationKind kind, u64 channel_id,
                                                   u64 message_id) noexcept {
    if (auto* owner = request(kind, channel_id, message_id)) {
        auto& value = g_app.message_mutation_notice;
        if (value.kind == kind && equal(value.shown, *owner))
            clear_shown();
        reset_value(owner);
    }
}
Utf8View application_message_mutation_notice_text() noexcept {
    auto& value = g_app.message_mutation_notice;
    if (value.phase == ApplicationMessageMutationNoticePhase::empty)
        return {};
    const bool unread = value.kind == ApplicationMessageMutationKind::mark_unread;
    const bool routed = value.kind == ApplicationMessageMutationKind::navigate_message;
    if (!current(value.shown, unread || routed)) {
        clear_shown();
        return {};
    }
    // A channel-menu action may finish while another channel is selected.
    // Keep its result, and show it only in the channel that started it.
    if ((unread || routed) && g_app.account_ui->active_channel_id != value.shown.channel_id)
        return {};
    if (value.phase == ApplicationMessageMutationNoticePhase::pending)
        return value.kind == ApplicationMessageMutationKind::navigate_message
                   ? navigation_text(value.navigation_stage, false, value.status_code)
               : value.kind == ApplicationMessageMutationKind::attachment_open
                   ? utf8_literal("Opening attachment…")
                   : Utf8View{};
    const auto required = api_rejection_notice(value.rejection);
    if (required.bytes)
        return required;
    if (unread)
        return utf8_literal("Couldn't mark as unread. Try again.");
    if (value.kind == ApplicationMessageMutationKind::navigate_message)
        return navigation_text(value.navigation_stage, true, value.status_code);
    if (value.kind == ApplicationMessageMutationKind::attachment_open)
        return utf8_literal("Couldn't open the attachment. Try again.");
    if (value.status_code == 403)
        return value.kind == ApplicationMessageMutationKind::edit
                   ? utf8_literal("You can't edit this message.")
               : value.kind == ApplicationMessageMutationKind::erase
                   ? utf8_literal("You cannot delete this message.")
                   : utf8_literal("You cannot change reactions on this message.");
    if (value.status_code == 404)
        return value.kind == ApplicationMessageMutationKind::edit
                   ? utf8_literal("This message was deleted.")
                   : utf8_literal("This message is no longer available.");
    if (value.kind == ApplicationMessageMutationKind::edit)
        return utf8_literal("Couldn't save your edit. Try again.");
    if (value.kind == ApplicationMessageMutationKind::erase)
        return value.transport_failed || value.status_code >= 500
                   ? utf8_literal("Couldn't confirm the message was deleted.")
                   : utf8_literal("Couldn't delete the message. Try again.");
    return utf8_literal("Couldn't update the reaction. Try again.");
}
bool application_message_mutation_notice_is_error() noexcept {
    return application_message_mutation_notice_text().bytes &&
           g_app.message_mutation_notice.phase == ApplicationMessageMutationNoticePhase::failed;
}
void application_message_mutation_notice_clear(ApplicationMessageMutationKind kind, u64 channel_id,
                                               u64 message_id) noexcept {
    if (index(kind) >= kApplicationMessageMutationKinds)
        return;
    auto& value = g_app.message_mutation_notice;
    const auto matches = [&](const ApplicationMessageMutationScope& owner) noexcept {
        return (!channel_id || owner.channel_id == channel_id) &&
               (!message_id || owner.message_id == message_id);
    };
    if (matches(value.requests[index(kind)]))
        reset_value(&value.requests[index(kind)]);
    if (value.kind == kind && matches(value.shown))
        clear_shown();
}
} // namespace maicord::app_detail
