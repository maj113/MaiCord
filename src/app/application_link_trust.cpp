#include "application_link_trust.hpp"
#include "application_friends.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/core/utf.hpp"
#include "account_ui_runtime.hpp"

#include "maicord/sys/shell.hpp"

namespace maicord::app_detail {
namespace {
// The warning's link, captured when it opened; accepting it launches exactly
// this URL for the same account.
struct PendingLink {
    char url[2048];
    u32 bytes;
    LinkTarget target;
    u64 serial;
};
PendingLink pending;
LinkTrustStore trust;
u64 trust_profile;
u64 pending_serial;

LinkTrustStore* current_store() noexcept {
    const u64 profile = g_app.account.status.profile_id;
    if (!profile)
        return nullptr;
    if (trust_profile != profile) {
        wchar_t path[512]{};
        reset_value(&trust);
        if (link_trust_path(profile, path, 512) && !link_trust_load_file(path, &trust))
            log_write(LogLevel::warning, "links", "trusted_links_unreadable");
        trust_profile = profile;
    }
    return &trust;
}

bool launch(Utf8View url) noexcept {
    wchar_t wide[2048]{};
    const int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data, static_cast<int>(url.bytes),
                                          wide, static_cast<int>(sizeof(wide) / sizeof(wide[0]) - 1));
    if (units <= 0)
        return true;
    wide[units] = 0;
    // The official client shows nothing when the browser refuses a link; neither do we.
    const HINSTANCE opened = ShellExecuteW(g_app.window, L"open", wide, nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32)
        log_write(LogLevel::warning, "links", "link_launch_failed");
    return true;
}

bool pending_valid(TextFormOwnerKey key) noexcept {
    return pending.bytes && g_app.account_ui && key.scope == g_app.plugin_account_scope_serial &&
           key.collection == g_app.account.status.profile_id && key.item == pending.serial;
}

bool pending_accept(TextFormOwnerKey key, bool trust_link) noexcept {
    if (!pending_valid(key))
        return true;
    const Utf8View url{pending.url, pending.bytes};
    if (trust_link) {
        auto* store = current_store();
        const bool added = store && (pending.target.web
                                         ? link_trust_add_host(store, link_target_host(pending.target))
                                         : link_trust_add_protocol(store, link_target_protocol(pending.target)));
        wchar_t path[512]{};
        if (!added || !link_trust_path(g_app.account.status.profile_id, path, 512) ||
            !link_trust_save_file(path, *store))
            log_write(LogLevel::warning, "links", "trusted_link_not_saved");
    }
    pending.bytes = 0;
    return launch(url);
}
} // namespace

const LinkTrustStore* application_link_trust_store() noexcept {
    return current_store();
}

bool application_link_channel_trusted(u64 channel_id) noexcept {
    if (!g_app.account_ui)
        return false;
    const auto& state = g_app.account_ui->chat_state;
    const ChannelRecord* channel = chat_state_store_find_channel(state, channel_id);
    if (!channel || channel->type != 1)
        return true;
    const auto* record = state.private_channels ? private_channel_store_find(*state.private_channels, channel_id)
                                                : nullptr;
    const auto recipients =
        record ? private_channel_store_recipients(*state.private_channels, *record) : ConstSlice<u64>{};
    const auto* relationships = application_friends_relationships();
    const auto* relation =
        recipients.count == 1 && relationships ? relationship_store_find(*relationships, recipients[0]) : nullptr;
    return relation && relation->type == RelationshipType::friend_;
}

bool application_link_open(Utf8View url, bool trusted_text) noexcept {
    const auto decision = link_open_decision(current_store(), url, trusted_text);
    if (decision == LinkOpenDecision::reject)
        return true;
    if (decision == LinkOpenDecision::open)
        return launch(url);
    if (!g_app.account_ui || url.bytes > sizeof(pending.url))
        return true;
    LinkTarget target{};
    if (!link_target_parse(url, &target))
        return true;
    const bool website = decision == LinkOpenDecision::warn_website;
    // The warning shows the URL without credentials; very long URLs end in
    // an ellipsis there (the accepted link is always the whole URL).
    char shown[2048]{};
    const u32 full_bytes = link_display_url(url, {shown, sizeof(shown)});
    constexpr u32 kShownMaximum = 1536;
    const u32 shown_bytes = utf8_truncated_length({shown, full_bytes}, kShownMaximum);
    char context_text[1700]{};
    TextBank context{context_text, sizeof(context_text), 0};
    context.add(website ? utf8_literal("This link is taking you to the following website\n\n")
                        : utf8_literal("This link is trying to open an application on your device\n\n"));
    context.add({shown, shown_bytes});
    if (shown_bytes < full_bytes)
        context.add("\xE2\x80\xA6");
    char option_text[64 + kLinkTrustHostBytes]{};
    TextBank option{option_text, sizeof(option_text), 0};
    option.add("Trust ");
    option.add(website ? link_target_host(target) : link_target_protocol(target));
    option.add(" links from now on");

    copy_bytes(pending.url, url.data, url.bytes);
    pending.bytes = url.bytes;
    pending.target = target;
    pending.serial = ++pending_serial;
    ApplicationTextFormRequest request{utf8_literal("Leaving Discord"),
                                       context.view(),
                                       {},
                                       website ? utf8_literal("Visit Site") : utf8_literal("Open Link"),
                                       {},
                                       {g_app.plugin_account_scope_serial, g_app.account.status.profile_id,
                                        pending.serial},
                                       pending_valid,
                                       nullptr,
                                       0,
                                       true};
    request.accept_focused = true;
    request.cancel = utf8_literal("Go Back");
    request.option = option.view();
    request.save_option = pending_accept;
    if (!application_text_form_open(request)) {
        pending.bytes = 0;
        return false;
    }
    return true;
}

} // namespace maicord::app_detail
