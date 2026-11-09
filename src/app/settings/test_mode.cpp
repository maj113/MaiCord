#include "test_mode.hpp"
#include "settings.hpp"
#include "account_broker.hpp"
#include "../chat/composer/text_form.hpp"
#include "../account/account_ui_runtime.hpp"
#include "../chat/messages/history.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/core/utf.hpp"

namespace maicord::app_detail {
namespace {
// The official client's check, then its application fetch; both on the details lane (profile_get operations 7 and 5).
enum class Phase : u8 { idle, check_queued, checking, fetch_queued, fetching };
constexpr u64 kEmbeddedFlag = 1ull << 17; // APPLICATION_FLAG EMBEDDED: an Activity
constexpr u64 kRetryMs = 1'000;
struct TestMode {
    Phase phase;
    u64 candidate, request_id, retry_at, named;
    u64 form_scope, revision;
    char status[160];
    u32 status_bytes;
    char name[128];
    u32 name_bytes;
    // The dialog started this fetch; a fetch for a restored application only names it.
    bool dialog;
};
TestMode g_mode{};

void status(Utf8View text) noexcept {
    g_mode.status_bytes = utf8_copy_truncated(g_mode.status, sizeof(g_mode.status), text);
    ++g_mode.revision;
}
bool form_valid(TextFormOwnerKey key) noexcept {
    return key.scope == g_mode.form_scope && g_app.preferences.developer_mode;
}
bool form_check(TextFormOwnerKey, Utf8View text, u32*) noexcept {
    u64 id = 0;
    return snowflake_parse(ascii_trim(text), &id) && id;
}
bool form_save(TextFormOwnerKey, Utf8View text) noexcept {
    u64 id = 0;
    if (!snowflake_parse(ascii_trim(text), &id) || !id || g_mode.phase != Phase::idle)
        return false;
    g_mode.candidate = id;
    g_mode.dialog = true;
    g_mode.phase = Phase::check_queued;
    status(utf8_literal("Checking your access to this application…"));
    pump_message_history();
    return false; // the dialog stays until the check answers (form_progress)
}
ApplicationTextFormProgress form_progress(TextFormOwnerKey) noexcept {
    ApplicationTextFormProgress progress{};
    const bool busy = g_mode.dialog && g_mode.phase != Phase::idle;
    progress.status = {g_mode.status, g_mode.status_bytes};
    progress.revision = g_mode.revision;
    progress.editable = !busy;
    progress.can_accept = !busy;
    progress.accept = utf8_literal("Activate");
    progress.complete = g_mode.dialog && g_mode.phase == Phase::idle && g_app.preferences.test_mode_application &&
                        g_app.preferences.test_mode_application == g_mode.candidate;
    return progress;
}
void form_closed(TextFormOwnerKey) noexcept {
    g_mode.dialog = false;
    g_mode.status_bytes = 0;
    if (g_mode.phase == Phase::check_queued || g_mode.phase == Phase::fetch_queued)
        g_mode.phase = Phase::idle;
}

void fail(Utf8View text) noexcept {
    g_mode.phase = Phase::idle;
    g_mode.request_id = 0;
    if (g_mode.dialog)
        status(text);
    application_request_frame();
}

struct PublicApplication {
    u64 id, flags;
    JsonString name;
    bool named;
};
bool application_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& application = *static_cast<PublicApplication*>(context);
    JsonString text{};
    if (json_name_is(name, utf8_literal("id")) && json_value_as_string(value, &text))
        json_string_to_u64(text, &application.id);
    else if (json_name_is(name, utf8_literal("flags")))
        json_value_to_u64(value, &application.flags);
    else if (json_name_is(name, utf8_literal("name")) && json_value_as_string(value, &application.name))
        application.named = true;
    return true;
}
struct Applications {
    const JsonIndexedDocument* document;
    PublicApplication found;
    u64 wanted;
};
bool application_item(void* context, u32, JsonValue value) noexcept {
    auto& list = *static_cast<Applications*>(context);
    PublicApplication application{};
    if (value.kind != JsonValueKind::object ||
        !json_visit_indexed_object(*list.document, value, 8, application_member, &application).ok())
        return true;
    if (application.id == list.wanted)
        list.found = application;
    return true;
}
} // namespace

bool application_test_mode_open() noexcept {
    if (!g_app.preferences.developer_mode || g_app.text_form || !account_online())
        return false;
    g_mode.form_scope = g_app.account.status.profile_id ^ ++g_mode.revision;
    g_mode.status_bytes = 0;
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Application Test Mode");
    request.context = utf8_literal(
        "Enter your Discord application ID to enable test mode for that application. An application in test mode "
        "will allow you to view related unpublished SKUs and bypass related purchases to make development easier.");
    request.label = utf8_literal("Application ID");
    request.accept = utf8_literal("Activate");
    request.owner = {g_mode.form_scope, 0x544d4f44, 1};
    request.valid = form_valid;
    request.save = form_save;
    request.maximum_codepoints = 20;
    request.single_line = true;
    request.check = form_check;
    request.rejected = utf8_literal("Enter a valid application ID.");
    request.progress = form_progress;
    request.closed = form_closed;
    return application_text_form_open(request);
}

void application_test_mode_clear() noexcept {
    g_app.preferences.test_mode_application = 0;
    g_mode.name_bytes = 0;
    g_mode.named = 0;
    application_settings_commit_preferences();
}

Utf8View application_test_mode_name() noexcept {
    const u64 active = g_app.preferences.test_mode_application;
    if (!active)
        return {};
    // A restored application is named once its public record arrives.
    if (g_mode.named != active && g_mode.phase == Phase::idle && !g_mode.dialog && account_online()) {
        g_mode.candidate = active;
        g_mode.phase = Phase::fetch_queued;
    }
    static char digits[24];
    return g_mode.named == active && g_mode.name_bytes ? Utf8View{g_mode.name, g_mode.name_bytes}
                                                       : Utf8View{digits, decimal_write(digits, active)};
}

Utf8View application_test_mode_status() noexcept {
    const Utf8View name = application_test_mode_name();
    if (!name.bytes)
        return utf8_literal("Enter your Discord application ID to enable test mode for that application.");
    static char text[320];
    TextWriter writer{{text, sizeof(text)}};
    writer.add(utf8_literal("Test mode is currently active for "));
    writer.add(name);
    writer.add(utf8_literal(". Unpublished SKUs will appear and you will not be charged for any related payments."));
    return {text, writer.used};
}

bool application_test_mode_pending(u64 now_ms) noexcept {
    return (g_mode.phase == Phase::check_queued || g_mode.phase == Phase::fetch_queued) && now_ms >= g_mode.retry_at &&
           account_online() && g_app.account.api.profile_get;
}

bool application_test_mode_submit(u64 request_id, u64 now_ms) noexcept {
    if (!application_test_mode_pending(now_ms))
        return false;
    auto& host = g_app.account;
    const bool check = g_mode.phase == Phase::check_queued;
    char digits[24];
    const Utf8View ids{digits, decimal_write(digits, g_mode.candidate)};
    const auto submitted = host.api.profile_get(host.instance, g_mode.candidate, 0, request_id, check ? 7 : 5,
                                                check ? Utf8View{} : ids);
    if (submitted.error == RestClientSubmitError::busy) {
        g_mode.retry_at = now_ms + kRetryMs;
        return false;
    }
    if (!submitted.ok()) {
        fail(utf8_literal("Couldn't reach Discord. Try again."));
        return false;
    }
    g_mode.request_id = request_id;
    g_mode.phase = check ? Phase::checking : Phase::fetching;
    return true;
}

bool application_test_mode_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!g_mode.request_id || response.request_id != g_mode.request_id)
        return true;
    g_mode.request_id = 0;
    const bool ok = response.transport_ok() && response.status_code >= 200 && response.status_code < 300;
    if (response.status_code == 429) {
        g_mode.retry_at = clock_monotonic_milliseconds() + (response.retry_at_ms ? response.retry_at_ms : kRetryMs);
        g_mode.phase = g_mode.phase == Phase::checking ? Phase::check_queued : Phase::fetch_queued;
        return true;
    }
    if (g_mode.phase == Phase::checking) {
        if (!ok) {
            fail(response.status_code == 403 || response.status_code == 404 || response.status_code == 401
                     ? utf8_literal("You don't have access to test this application.")
                     : utf8_literal("Couldn't check this application. Try again."));
            return true;
        }
        g_mode.phase = Phase::fetch_queued;
        status(utf8_literal("Loading the application…"));
        return true;
    }
    // The public application: its name for the row, and whether it is an Activity.
    PublicApplication found{};
    if (ok && body.bytes && body.data[0] == '[' && g_app.account_ui) {
        const Slice<u32> structurals{g_app.account_ui->message_history_page_structurals,
                                     kMessageHistoryPageStructuralCount};
        const auto scan = json_scan_structurals_avx1(body, structurals);
        if (scan.ok()) {
            const JsonIndexedDocument document{body, {structurals.data, scan.count}, 0};
            Applications list{&document, {}, g_mode.candidate};
            if (json_visit_indexed_array(document, {body, 0, JsonValueKind::array}, 8, application_item, &list).ok())
                found = list.found;
        }
    }
    if (!found.id) {
        fail(utf8_literal("Couldn't load this application. Try again."));
        return true;
    }
    if (g_mode.dialog && (found.flags & kEmbeddedFlag)) {
        fail(utf8_literal("This application is an Activity, which MaiCord can't run, so it can't enter test mode here."));
        return true;
    }
    g_mode.name_bytes = 0;
    if (found.named) {
        const auto decoded = json_string_decode_utf8(found.name, {g_mode.name, sizeof(g_mode.name)});
        g_mode.name_bytes = decoded.ok() ? decoded.bytes : 0;
    }
    g_mode.named = found.id;
    g_mode.phase = Phase::idle;
    if (g_mode.dialog) {
        g_app.preferences.test_mode_application = found.id;
        log_write(LogLevel::info, "settings", "test_mode_enabled");
    }
    application_settings_commit_preferences();
    return true;
}
} // namespace maicord::app_detail
