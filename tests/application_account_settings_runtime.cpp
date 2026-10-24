#include "application_account_settings.hpp"
#include "application_settings.hpp"
#include "application_state.hpp"
#include "maicord/protocol/account_requirement.hpp"
#include "maicord/protocol/account_settings.hpp"

// The account pages follow the connection: Offline only without a worker,
// Connecting until READY's self user lands, a specific card for a pending
// requirement, and loading by itself once ready.
namespace maicord::app_detail {
namespace {
u32 connection_requests;
RestClientSubmitResult WINAPI count_request(void*, const AccountSettingsRequest* request, u64) noexcept {
    if (request)
        ++connection_requests;
    return {RestClientSubmitError::none};
}
const UiSettingsControl* labelled(const UiSettingsPageContent* content, Utf8View label) noexcept {
    for (u32 i = 0; content && i < content->controls.count; ++i)
        if (utf8_equal(content->controls[i].label, label))
            return &content->controls[i];
    return nullptr;
}
} // namespace

void test_application_account_settings_connection_runtime(void (*check)(bool, const char*) noexcept) noexcept {
    auto* pages = application_settings_pages();
    auto* ui = g_app.account_ui;
    const UiPageKey account_page{1, static_cast<u64>(SettingsPanel::account) + 1};
    const auto* entry = pages ? ui_page_registry_find(*pages, account_page) : nullptr;
    const auto* content = entry ? static_cast<const UiSettingsPageContent*>(entry->context) : nullptr;
    if (!ui || !content) {
        check(false, "account settings connection: open settings and account runtime fixture");
        return;
    }
    const auto saved_account = g_app.account;
    const u64 saved_self = ui->chat_state.self_user_id;
    const auto shows = [&](const char* label) noexcept {
        u32 bytes = 0;
        while (label[bytes])
            ++bytes;
        return labelled(content, {label, bytes}) != nullptr;
    };
    const auto card = [](UiSettingsControl* c, UiSettingsControlPresentation* p) noexcept {
        *c = {};
        *p = {};
        return application_account_settings_project(SettingBinding::account_notice, c, p);
    };
    UiSettingsControl c{};
    UiSettingsControlPresentation p{};

    g_app.account = {};
    ui->chat_state.self_user_id = 0;
    check(ui_page_registry_select(pages, account_page).ok() && application_settings_synchronize() &&
              shows("Offline") && !application_account_settings_pending(0) &&
              application_account_settings_connection() == AccountSettingsConnection::offline,
          "account settings connection: without an account worker the page shows Offline and loads nothing");

    // Settings opened while the Gateway connects, before READY.
    g_app.account.instance = reinterpret_cast<void*>(1);
    g_app.account.api.account_settings = count_request;
    g_app.account.status.profile_id = 77;
    check(application_settings_synchronize() && shows("Connecting…") && !shows("Offline") &&
              !application_account_settings_pending(0) && card(&c, &p) &&
              p.variant == UiSettingsVariant::standard,
          "account settings connection: a starting worker shows Connecting, not Offline");
    g_app.account.status.running = true;
    check(application_settings_synchronize() && shows("Connecting…") && !application_account_settings_pending(0),
          "account settings connection: a running phase without READY's self user is still connecting");

    // READY lands with the page open: it re-projects and loads by itself.
    ui->chat_state.self_user_id = 42;
    const u32 revision = content->revision;
    check(application_settings_synchronize() && content->revision != revision && shows("Loading…") &&
              !shows("Connecting…") && application_account_settings_pending(0),
          "account settings connection: READY re-projects the open page from Connecting to Loading");
    check(application_account_settings_submit(9001, 0) && connection_requests == 1,
          "account settings connection: the page opened before READY requests its details once");
    const u32 settled = content->revision;
    check(application_settings_synchronize() && content->revision == settled && !application_account_settings_connection_changed(),
          "account settings connection: an unchanged connection does not re-project");

    g_app.account.status.required_action = AccountRequirement::verify_email;
    check(application_settings_synchronize() && shows("Discord needs something from you") && card(&c, &p) &&
              utf8_equal(c.description, account_requirement_notice(AccountRequirement::verify_email)) &&
              p.variant == UiSettingsVariant::critical && !application_account_settings_pending(0),
          "account settings connection: a pending requirement names what Discord needs");
    g_app.account.status.required_action = AccountRequirement::none;

    g_app.account.status.failed = true;
    check(application_settings_synchronize() && shows("Offline") && card(&c, &p) &&
              utf8_equal(c.description, utf8_literal("Connect to Discord to see these details.")),
          "account settings connection: a failed worker shows Offline");
    g_app.account.status.failed = false;
    check(application_settings_synchronize() && shows("Loading…"),
          "account settings connection: recovery leaves Offline without reopening the page");

    // A stop releases the host while the shell keeps its cached self user.
    g_app.account = {};
    check(application_settings_synchronize() && shows("Offline") && !application_account_settings_pending(0),
          "account settings connection: a stopped account with a live shell is Offline");

    application_account_settings_release();
    g_app.account = saved_account;
    ui->chat_state.self_user_id = saved_self;
    check(ui_page_registry_select(pages, {1, static_cast<u64>(SettingsPanel::appearance) + 1}).ok() &&
              application_settings_synchronize(),
          "account settings connection: Appearance restores after the connection checks");
}
} // namespace maicord::app_detail
