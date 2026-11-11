#include "account_settings.hpp"
#include "../account/account_access.hpp"
#include "../shell/owner.hpp"
#include "settings.hpp"
#include "../shell/state.hpp"
#include "../chat/composer/text_form.hpp"
#include "../chat/composer/text_form_state.hpp"
#include "user_settings.hpp"
#include "streamer_mode.hpp"
#include "../account/friends.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/platform/crypto/random.hpp"
#include "maicord/protocol/auth/account_requirement.hpp"
#include "maicord/ui/text/timestamp_format.hpp"

#include "maicord/protocol/user/account_settings.hpp"
#include "../account/account_ui_runtime.hpp"
#include "maicord/platform/desktop/webauthn.hpp"
#include "maicord/platform/window/event.hpp"
#if defined(MAICORD_TEST_HOOKS)
#include "maicord/core/test_hook.hpp"
#endif

namespace maicord::app_detail {
namespace {
using Op = AccountSettingsOperation;
enum Load : u16 {
    load_self = 1,
    load_profile = 2,
    load_sessions = 4,
    load_tokens = 8,
    load_keys = 16,
    load_connections = 32,
    load_consents = 64,
    load_harvest = 128,
};
constexpr u64 kMfaRequired = 60003;
// A data request may be made once every 30 days (the official client's cooldown).
constexpr u64 kHarvestCooldownMs = 30ull * 24 * 60 * 60 * 1000;
// Messaging Permissions lists up to this many blocked and as many ignored accounts (then says how many more).
constexpr u32 kRelationRows = 72;
constexpr u32 kRelationFirstRow = kAccountSessionMaximum + kAccountAuthorizedAppMaximum + kAccountSecurityKeyMaximum;
constexpr u32 kRows = kRelationFirstRow + 2 * (kRelationRows + 1);
// Both lists and their more lines fit the bank with the page's own rows.
static_assert(2 * (kRelationRows + 1) + 40 <= kAccountSettingsBankRows);
// A password or code field's longest text (Discord passwords are at most 72 characters).
constexpr u32 kSecretCodepoints = 72;
static_assert(kSecretCodepoints <= kApplicationTextFormSecretCodepoints);

// The dialogs of the account flows, in the order a flow opens them. Each saves into a field and either opens the
// next one or sends its request; the request's answer opens the one after it.
enum class Form : u8 {
    none,
    display_name,
    pronouns,
    bio,
    username,
    username_password,
    password_current,
    password_new,
    password_confirm,
    email_send,
    email_code,
    email_new,
    email_password,
    phone_number,
    phone_code,
    phone_password,
    phone_remove,
    totp_setup,
    totp_remove,
    new_codes,
    sms_enable,
    sms_disable,
    backup_password,
    backup_key,
    backup_list,
    key_rename,
    key_delete,
    account_disable,
    account_delete,
    session_logout,
    logout_all,
    token_revoke,
    connection_remove,
    consent_off,
    key_register,
    key_pin,
    key_name,
    mfa_password,
    mfa_code,
};
// The texts a flow keeps between its dialogs. Secrets are wiped when the flow ends.
enum Field : u8 { field_text, field_password, field_extra, field_token, field_token2, field_mfa, field_count };
// A security key's credential JSON is the longest text a flow keeps.
constexpr u32 kFieldBytes = kAccountSettingsTextBytes;

struct Row {
    char label[112], description[176];
    u8 label_bytes, description_bytes;
};
struct Bank {
    AccountSelfUser self;
    AccountProfileText profile;
    AccountSession sessions[kAccountSessionMaximum];
    AccountAuthorizedApp apps[kAccountAuthorizedAppMaximum];
    AccountSecurityKey keys[kAccountSecurityKeyMaximum];
    AccountConnection connections[kAccountConnectionMaximum];
    AccountBackupCode codes[kAccountBackupCodeMaximum];
    AccountConsents consents;
    AccountHarvest harvest;
    AccountMfaChallenge mfa;
    // The authenticator setup's link as a QR code (it holds the key: cleared with the flow).
    QrCodeLarge totp_qr;
    Row rows[kRows];
    // The accounts the blocked and ignored rows stand for, as last listed.
    u64 relation_ids[2][kRelationRows];
    char email_mask[128], phone_mask[32];
    u8 email_mask_bytes, phone_mask_bytes;
    char fields[field_count][kFieldBytes];
    u16 field_bytes[field_count];
    // The open dialog's composed texts: its context and its read-only detail.
    char context[768], detail[512];
    u16 context_bytes, detail_bytes;
    // The last answer's error message, or a flow's own status line.
    char message[512];
    u16 message_bytes;
    char date[64];
    char webauthn[4096];
    u32 webauthn_bytes;
    // Connected Apps' rows (connections and their display options, then authorized apps).
    UiSettingsControl controls[kAccountSettingsBankRows];
    UiSettingsControlPresentation presentation[kAccountSettingsBankRows];
    u32 structurals[8192];
};
struct Owner {
    Bank* bank;
    u64 profile_id, self_user_id, request_id, lifetime, form_serial, form_revision;
    u32 session_count, app_count, key_count, connection_count, code_count;
    u16 loaded, failed, wanted, inflight_load;
    // The flow's request: queued, then in flight; the one MFA verification interrupted is retried with the token.
    Op op, retry, inflight;
    bool queued, action_inflight, verified;
    // A security-key ceremony of Linux in flight (1 a verification, 2 a registration) and the token its wakes carry (its
    // top bit tells them from the sign-in's); and what the next dialog opens saying.
    u8 key_ceremony;
    u32 key_serial;
    u64 key_token;
    Utf8View carry;
    u64 op_id;
    u8 op_option;
    // The connection, security key or session a flow acts on.
    u32 target;
    Form form;
    SettingsPanel flow_panel;
    // The open dialog: whether closing it opens `form`, whether its request runs, and its status line.
    bool form_shown, advance, busy;
    Utf8View form_status;
    // The verification's method as /mfa/finish names it ("totp", "password", …).
    Utf8View mfa_type;
    u8 harvest_types;
    bool reveal_email, reveal_phone, status_error;
    // Messaging Permissions is open: its blocked and ignored lists follow the relationship store (its revision + 1).
    bool social;
    u32 relationship_revision;
    Utf8View status;
    SettingsPanel status_panel;
};
Owner g_owner{};
// Last connection the settings host projected; survives owner scope resets.
u8 g_connection = 0xFF;

bool online() noexcept {
    return application_account_settings_connection() == AccountSettingsConnection::ready;
}
bool action_pending() noexcept {
    return g_owner.queued || g_owner.action_inflight;
}
void release_bank() noexcept {
    owner_release(g_owner.bank);
}
Bank* bank() noexcept {
    return owner_ensure(g_owner.bank);
}
// A security-key ceremony of Linux (a worker thread) ends with the flow that started it: cancelled and joined.
void end_ceremony() noexcept {
#if !defined(_WIN32)
    if (g_owner.key_ceremony) {
        webauthn_ceremony_release();
        g_owner.key_ceremony = 0;
    }
#endif
}
void scope() noexcept {
    const u64 profile = g_app.account.status.profile_id;
    const u64 self = g_app.account_ui ? g_app.account_ui->chat_state.self_user_id : 0;
    if (g_owner.profile_id == profile && g_owner.self_user_id == self)
        return;
    application_cancel_request(account_module_account_settings, g_owner.request_id);
    release_bank();
    const u64 lifetime = g_owner.lifetime + 1;
    // The open page stays wanted: a page opened before READY loads once it lands.
    const u16 wanted = g_owner.wanted;
    end_ceremony();
    reset_value(&g_owner);
    g_owner.lifetime = lifetime;
    g_owner.wanted = wanted;
    g_owner.profile_id = profile;
    g_owner.self_user_id = self;
}
void set_status(SettingsPanel panel, const Utf8View& text, bool error) noexcept {
    g_owner.status = text;
    g_owner.status_error = error;
    g_owner.status_panel = panel;
}
Utf8View view(const char* data, u32 bytes) noexcept {
    return {data, bytes};
}
Utf8View field(Field f) noexcept {
    return g_owner.bank ? view(g_owner.bank->fields[f], g_owner.bank->field_bytes[f]) : Utf8View{};
}
bool set_field(Field f, Utf8View text) noexcept {
    auto& b = *g_owner.bank;
    if (text.bytes > kFieldBytes)
        return false;
    secure_clear_bytes(b.fields[f], b.field_bytes[f]);
    copy_bytes(b.fields[f], text.data, text.bytes);
    b.field_bytes[f] = static_cast<u16>(text.bytes);
    return true;
}
// Appends to one of the bank's composed texts; the result is the whole text so far.
template <u32 Capacity, typename Size> Utf8View append(char (&buffer)[Capacity], Size& used, Utf8View text) noexcept {
    used = static_cast<Size>(used + utf8_copy_truncated(buffer + used, Capacity - used, text));
    return {buffer, used};
}
Utf8View message(Utf8View text) noexcept {
    auto& b = *g_owner.bank;
    b.message_bytes = 0;
    return append(b.message, b.message_bytes, text);
}
// Cached self user from READY until GET /users/@me answers.
Utf8View self_name(bool username) noexcept {
    const auto* b = g_owner.bank;
    if (b && (g_owner.loaded & load_self)) {
        if (username)
            return view(b->self.username, b->self.username_bytes);
        if (b->self.global_name_bytes)
            return view(b->self.global_name, b->self.global_name_bytes);
    }
    if (!g_app.account_ui)
        return {};
    const auto& users = g_app.account_ui->user_store;
    const auto* user = user_store_find(users, g_app.account_ui->chat_state.self_user_id);
    return user ? user_store_name(users, *user) : Utf8View{};
}
bool self_loaded() noexcept {
    return g_owner.bank && (g_owner.loaded & load_self);
}
bool totp_enabled() noexcept {
    return self_loaded() && g_owner.bank->self.mfa_enabled &&
           (g_owner.bank->self.authenticators & kAccountAuthenticatorTotp);
}
bool sms_enabled() noexcept {
    return self_loaded() && (g_owner.bank->self.authenticators & kAccountAuthenticatorSms);
}
Utf8View failure_text(Op op) noexcept {
    switch (op) {
    case Op::session_logout:
        return utf8_literal("Couldn't log out of that device. Try again.");
    case Op::token_revoke:
        return utf8_literal("Couldn't deauthorize that app. Try again.");
    case Op::connection_patch:
    case Op::connection_delete:
        return utf8_literal("Couldn't change that connection. Try again.");
    case Op::consent_patch:
    case Op::harvest_create:
        return utf8_literal("Oops, something went wrong... Please try again.");
    default:
        return utf8_literal("Something went wrong. Try again.");
    }
}

// ---- Dialogs ----------------------------------------------------------------------------------------------------

void clear_flow() noexcept {
    if (auto* b = g_owner.bank) {
        secure_clear_bytes(b->fields, sizeof(b->fields));
        reset_value(&b->field_bytes);
        secure_clear_bytes(&b->mfa, sizeof(b->mfa));
        secure_clear_bytes(b->codes, sizeof(b->codes));
        secure_clear_bytes(&b->totp_qr, sizeof(b->totp_qr));
        secure_clear_bytes(b->webauthn, b->webauthn_bytes);
        b->webauthn_bytes = 0;
        b->detail_bytes = b->context_bytes = 0;
    }
    g_owner.code_count = 0;
    g_owner.verified = false;
    end_ceremony();
    g_owner.form = Form::none;
}
__forceinline TextFormOwnerKey form_key() noexcept {
    return {g_owner.lifetime, static_cast<u64>(g_owner.form), g_owner.form_serial};
}
bool form_valid(TextFormOwnerKey key) noexcept {
    return key.scope == g_owner.lifetime && key.collection == static_cast<u64>(g_owner.form) &&
           key.item == g_owner.form_serial && g_owner.bank && online();
}
ApplicationTextFormProgress form_progress(TextFormOwnerKey) noexcept {
    ApplicationTextFormProgress progress{};
    progress.status = g_owner.form_status;
    progress.revision = g_owner.form_revision;
    progress.editable = progress.can_accept = !g_owner.busy;
    // A security key's ceremony on Linux waits on the dialog: closing it cancels the ceremony.
    progress.dismissible = !g_owner.busy || g_owner.key_ceremony != 0;
    return progress;
}
void form_status(Utf8View text) noexcept {
    g_owner.form_status = text;
    ++g_owner.form_revision;
}
void open_form(Form form) noexcept;
// Closing the dialog the user dismissed ends its flow (a request in flight still settles on the page); closing it to
// advance opens the next one.
void form_closed(TextFormOwnerKey) noexcept {
    g_owner.form_shown = false;
    if (g_owner.advance) {
        g_owner.advance = false;
        if (g_owner.form != Form::none) {
            open_form(g_owner.form);
            return;
        }
    }
    if (!action_pending())
        clear_flow();
}
// Opens `form` once the dialog showing closes, or now when none shows.
void transition(Form form) noexcept {
    g_owner.form = form;
    g_owner.busy = false;
    if (g_owner.form_shown) {
        g_owner.advance = true;
        application_text_form_close(false, false);
    } else if (form != Form::none)
        open_form(form);
}
// The end of a flow: its result on the page.
void finish(Utf8View text, bool error) noexcept {
    set_status(g_owner.flow_panel, text, error);
    transition(Form::none);
    clear_flow();
    application_settings_refresh_pages();
}
void queue(Op op, u64 id = 0, u8 option = 0) noexcept {
    g_owner.op = op;
    g_owner.op_id = id;
    g_owner.op_option = option;
    g_owner.queued = true;
    g_owner.busy = true;
    form_status({});
    set_status(g_owner.flow_panel, {}, false);
    application_settings_refresh_pages();
}
void begin(Form form, SettingsPanel panel) noexcept {
    clear_flow();
    g_owner.flow_panel = panel;
    set_status(panel, {}, false);
    transition(form);
}

// Appends the key to `detail` grouped in fours, lowercase, as the official client shows a TOTP key.
__forceinline Utf8View grouped_secret() noexcept {
    auto& b = *g_owner.bank;
    const Utf8View secret = field(field_extra);
    u32 at = 0;
    char grouped[48];
    for (u32 i = 0; i < secret.bytes && at + 2 < sizeof(grouped); ++i) {
        if (i && !(i % 4))
            grouped[at++] = ' ';
        const char c = secret.data[i];
        grouped[at++] = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return append(b.detail, b.detail_bytes, {grouped, at});
}
// 20 random bytes as the 32 base32 characters /users/@me/mfa/totp/enable takes.
bool generate_secret() noexcept {
    u8 random[20];
    if (!secure_random_fill({random, sizeof(random)}).ok())
        return false;
    constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    char secret[32];
    for (u32 i = 0; i < 32; ++i) {
        const u32 bit = i * 5, byte = bit / 8, shift = bit % 8;
        const u32 window = (static_cast<u32>(random[byte]) << 8) | (byte + 1 < 20 ? random[byte + 1] : 0);
        secret[i] = kAlphabet[(window >> (11 - shift)) & 31];
    }
    const bool stored = set_field(field_extra, {secret, 32});
    secure_clear_bytes(random, sizeof(random));
    secure_clear_bytes(secret, sizeof(secret));
    return stored;
}
// The official client's authenticator link: otpauth://totp/Discord:<email>?secret=<key>&issuer=Discord, the email
// escaped as encodeURI escapes it. 0 when it does not fit `output`.
__forceinline u32 totp_uri(Utf8View email, Utf8View key, Slice<char> output) noexcept {
    u32 at = 0;
    bool fits = true;
    const auto put = [&](Utf8View text) noexcept {
        fits = fits && at + text.bytes <= output.count;
        if (fits) {
            copy_bytes(output.data + at, text.data, text.bytes);
            at += text.bytes;
        }
    };
    put(utf8_literal("otpauth://totp/Discord:"));
    constexpr char kHex[] = "0123456789ABCDEF";
    for (u32 i = 0; i < email.bytes; ++i) {
        const u8 c = static_cast<u8>(email.data[i]);
        const bool kept = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                          (c < 0x80 && c != '%' && c != '"' && c != '<' && c != '>' && c != '\\' && c != '^' &&
                           c != '`' && c != '{' && c != '|' && c != '}' && c > ' ' && c != 0x7f);
        const char escaped[3]{'%', kHex[c >> 4], kHex[c & 15]};
        put(kept ? Utf8View{email.data + i, 1} : Utf8View{escaped, 3});
    }
    put(utf8_literal("?secret="));
    put(key);
    put(utf8_literal("&issuer=Discord"));
    return fits ? at : 0;
}
// The backup codes as the official client lists them: "abcd-efgh", used ones marked.
__forceinline Utf8View codes_detail() noexcept {
    auto& b = *g_owner.bank;
    b.detail_bytes = 0;
    for (u32 i = 0; i < g_owner.code_count; ++i) {
        const auto& code = b.codes[i];
        if (i)
            append(b.detail, b.detail_bytes, utf8_literal("\n"));
        const u32 half = code.code_bytes / 2;
        append(b.detail, b.detail_bytes, view(code.code, half));
        append(b.detail, b.detail_bytes, utf8_literal("-"));
        append(b.detail, b.detail_bytes, view(code.code + half, code.code_bytes - half));
        if (code.consumed)
            append(b.detail, b.detail_bytes, utf8_literal(" (used)"));
    }
    return view(b.detail, b.detail_bytes);
}

// The MFA methods a verification offers, as its radio group shows them.
constexpr Utf8View kMfaChoiceLabels[]{utf8_literal("Authenticator app"), utf8_literal("SMS code"),
                                      utf8_literal("Backup code"), utf8_literal("Security key")};
constexpr u8 kMfaChoiceMethods[]{kAccountAuthenticatorTotp, kAccountAuthenticatorSms, kAccountMfaMethodBackup,
                                 kAccountAuthenticatorWebauthn};
constexpr const char* kMfaChoiceTypes[]{"totp", "sms", "backup", "webauthn"};
Utf8View g_mfa_choices[4];
u8 g_mfa_choice_methods[4];
__forceinline u32 mfa_choices() noexcept {
    u32 count = 0;
    const u8 methods = g_owner.bank->mfa.methods;
    for (u32 i = 0; i < 4; ++i) {
        // A security key needs libfido2 on Linux: without it the choice is not offered.
        if (kMfaChoiceMethods[i] == kAccountAuthenticatorWebauthn && !webauthn_platform_available())
            continue;
        if (methods & kMfaChoiceMethods[i]) {
            g_mfa_choices[count] = kMfaChoiceLabels[i];
            g_mfa_choice_methods[count++] = static_cast<u8>(i);
        }
    }
    return count;
}

bool form_save(TextFormOwnerKey key, Utf8View text) noexcept;
bool form_save_option(TextFormOwnerKey key, bool checked) noexcept;
bool form_save_choices(TextFormOwnerKey key, Utf8View text, bool typed, u32 choice, u32 select) noexcept;

// Each dialog's fixed texts, in Form order: its title, context, field label and accept button, four names each in one
// NameList (an empty name is absent; an empty accept is "Done"; a password field without a label asks for the current
// password), and its shape. A text field holds up to `codepoints`; a confirmation has none. The texts a dialog
// composes are filled in by open_form.
enum FormShape : u8 { form_text, form_secret = 1, form_confirm = 2, form_danger = 4 };
struct FormSpec {
    u8 shape, codepoints;
};
constexpr char kFormTexts[] =
    // none
    "\0" "\0" "\0" "\0"
    // display_name, pronouns, bio
    "Change your display name\0" "\0" "Display name\0" "Save\0"
    "Change your pronouns\0" "\0" "Pronouns\0" "Save\0"
    "Edit about me\0" "\0" "About me\0" "Save\0"
    // username, username_password
    "Change your username\0" "Enter a new username and your existing password.\0" "Username\0" "Next\0"
    "Change your username\0" "Enter a new username and your existing password.\0" "\0" "\0"
    // password_current, password_new, password_confirm
    "Change your password\0" "Enter your current password and a new password.\0" "\0" "Next\0"
    "Change your password\0" "Enter your current password and a new password.\0" "New Password\0" "Next\0"
    "Change your password\0" "Enter your current password and a new password.\0" "Confirm New Password\0" "\0"
    // email_send, email_code, email_new, email_password
    "Verify email address\0" "\0" "\0" "Send Verification Code\0"
    "Enter code\0" "Check your email: we sent you a verification code.\0" "Verification code\0" "Next\0"
    "Enter an email address\0" "Enter a new email address and your existing password.\0" "Email\0" "Next\0"
    "Enter an email address\0" "Enter a new email address and your existing password.\0" "\0" "\0"
    // phone_number, phone_code, phone_password, phone_remove
    "Enter a phone number\0" "You will receive a text message with a verification code.\0" "Phone Number\0" "Send\0"
    "Verify your phone number\0" "\0" "Verification code\0" "Next\0"
    "Verify your phone number\0" "Enter your password to finish.\0" "\0" "\0"
    "Remove phone number\0" "Enter your password to remove the phone number from your account.\0" "\0" "Remove\0"
    // totp_setup: as the official client, the steps, the QR of the otpauth:// link (Discord:<email>, the key, issuer
    // Discord), the key for manual entry under it, then the code field.
    "Enable Authenticator App\0"
    "Make your account safer in 3 easy steps:\n1. Download an authenticator app.\n2. Scan the QR code with it, or "
    "enter the key under it.\n3. Enter the 6-digit code it shows.\0"
    "6-digit authentication code\0"
    "Activate\0"
    // totp_remove, new_codes
    "Remove Authenticator App\0" "Are you sure you want to remove your Authenticator App?\0" "\0" "Remove\0"
    "Backup Codes\0"
    "These codes will allow you to enter your account if you lose your authenticator app. Each code can only be used "
    "once.\0"
    "Your backup codes\0"
    "\0"
    // sms_enable, sms_disable
    "Enable SMS Authentication\0"
    "Your phone number becomes a backup way to sign in: Discord can text it a code.\0"
    "\0"
    "Enable\0"
    "Remove SMS Authentication\0" "Enter your password to stop using your phone for authentication.\0" "\0" "Remove\0"
    // backup_password, backup_key; backup_list shows new_codes' texts
    "Show Backup Codes\0"
    "Enter your password. Discord emails you a verification key to view your backup codes.\0"
    "\0"
    "Send Key\0"
    "Show Backup Codes\0"
    "Check your email: enter the verification key Discord sent you.\0"
    "Verification key\0"
    "Show Codes\0"
    "\0" "\0" "\0" "\0"
    // key_rename, key_delete
    "Rename security key\0" "\0" "Name\0" "Save\0"
    "Remove security key\0" "You won't be able to use this key to sign in anymore.\0" "\0" "Remove\0"
    // account_disable, account_delete
    "Disable Account\0"
    "Are you sure that you want to disable your account? This will immediately log you out and make your account "
    "inaccessible to anyone.\0"
    "Password\0"
    "Disable Account\0"
    "Delete Account\0"
    "Are you sure that you want to delete your account? This will immediately log you out of your account and you "
    "will not be able to log in again.\0"
    "Password\0"
    "Delete Account\0"
    // session_logout, logout_all, token_revoke, connection_remove
    "Log out this device?\0" "It will need to sign in again to use your account.\0" "\0" "Log Out\0"
    "Log out of all known devices?\0"
    "You'll have to log back in on all logged out devices.\0"
    "\0"
    "Log Out All Known Devices\0"
    "\0" "It loses access to your account until you authorize it again.\0" "\0" "Deauthorize\0"
    "\0"
    "Disconnecting removes this account from your profile and leaves any server you joined through it.\0"
    "\0"
    "Disconnect\0"
    // consent_off, key_register, key_pin, key_name, mfa_password, mfa_code
    "Turn off use data to improve Discord?\0"
    "If you turn this off, we'll stop processing new data for analytics. You will lose access to new experimental "
    "features and usage-based improvements to features like voice chat.\0"
    "\0"
    "Yes, I'm sure\0"
    "Register a Security Key\0"
    "When you are ready to authenticate, press the button below.\0"
    "\0"
    "Let\xE2\x80\x99s go\0"
    "Security Key PIN\0" "Enter the PIN of your security key.\0" "PIN\0" "Confirm\0"
    "Name your security key\0" "Choose a name for your new key.\0" "Name\0" "Save\0"
    "Confirm Password\0"
    "For your security, confirm your password to continue.\0"
    "Password\0"
    "Confirm\0"
    "Multi-Factor Authentication\0"
    "Confirm it's you: choose a method and enter its code.\0"
    "\0"
    "Confirm\0";
constexpr FormSpec kForms[]{
    {form_text, 0},                                  // none
    {form_text, 32},                                 // display_name
    {form_text, 40},                                 // pronouns
    {form_text, 190},                                // bio
    {form_text, 32},                                 // username
    {form_secret, kSecretCodepoints},                // username_password
    {form_secret, kSecretCodepoints},                // password_current
    {form_secret, kSecretCodepoints},                // password_new
    {form_secret, kSecretCodepoints},                // password_confirm
    {form_confirm, 0},                               // email_send
    {form_text, 16},                                 // email_code
    {form_text, 128},                                // email_new
    {form_secret, kSecretCodepoints},                // email_password
    {form_text, 24},                                 // phone_number
    {form_text, 16},                                 // phone_code
    {form_secret, kSecretCodepoints},                // phone_password
    {form_secret | form_danger, kSecretCodepoints},  // phone_remove
    {form_text, 10},                                 // totp_setup
    {form_confirm | form_danger, 0},                 // totp_remove
    {form_confirm, 0},                               // new_codes
    {form_confirm, 0},                               // sms_enable
    {form_secret | form_danger, kSecretCodepoints},  // sms_disable
    {form_secret, kSecretCodepoints},                // backup_password
    {form_text, 32},                                 // backup_key
    {form_confirm, 0},                               // backup_list
    {form_text, 32},                                 // key_rename
    {form_confirm | form_danger, 0},                 // key_delete
    {form_secret | form_danger, kSecretCodepoints},  // account_disable
    {form_secret | form_danger, kSecretCodepoints},  // account_delete
    {form_confirm | form_danger, 0},                 // session_logout
    {form_confirm | form_danger, 0},                 // logout_all
    {form_confirm | form_danger, 0},                 // token_revoke
    {form_confirm | form_danger, 0},                 // connection_remove
    {form_confirm | form_danger, 0},                 // consent_off
    {form_confirm, 0},                               // key_register
    {form_secret, kSecretCodepoints},                // key_pin
    {form_text, 32},                                 // key_name
    {form_secret, kSecretCodepoints},                // mfa_password
    {form_text, 16},                                 // mfa_code
};
static_assert(sizeof(kForms) / sizeof(kForms[0]) == static_cast<u32>(Form::mfa_code) + 1);
// Four names per dialog (empty ones included, so the list is walked rather than counted): their terminators and the end.
static_assert([]() constexpr {
    u32 terminators = 0;
    for (const char c : kFormTexts)
        terminators += c == '\0';
    return terminators;
}() == 4 * sizeof(kForms) / sizeof(kForms[0]) + 1);

void open_form(Form form) noexcept {
    auto& b = *g_owner.bank;
    g_owner.form = form;
    ++g_owner.form_serial;
    g_owner.busy = false;
    // What an earlier step left to say (a refused PIN, a failed ceremony) opens with the dialog.
    form_status(g_owner.carry);
    g_owner.carry = {};
    if (form == Form::none)
        return;
    const FormSpec& spec = kForms[static_cast<u32>(form)];
    const bool danger = (spec.shape & form_danger) != 0;
    ApplicationTextFormRequest r{};
    r.owner = form_key();
    r.valid = form_valid;
    r.save = form_save;
    r.progress = form_progress;
    r.closed = form_closed;
    r.single_line = true;
    NameList texts{kFormTexts};
    for (u32 skip = 4 * static_cast<u32>(form == Form::backup_list ? Form::new_codes : form); skip; --skip)
        texts.next();
    r.title = texts.name();
    texts.next();
    r.context = texts.name();
    texts.next();
    r.label = texts.name();
    texts.next();
    r.accept = texts.bytes ? texts.name() : utf8_literal("Done");
    r.danger = danger;
    if (spec.shape & form_confirm) {
        r.confirmation_only = true;
        r.accept_focused = !danger;
    } else {
        r.maximum_codepoints = spec.codepoints;
        r.maximum_bytes = 4 * spec.codepoints;
        r.secret = (spec.shape & form_secret) != 0;
        if (r.secret && !r.label.bytes)
            r.label = utf8_literal("Current Password");
    }
    b.context_bytes = 0;
    switch (form) {
    case Form::display_name:
        r.initial = view(b.self.global_name, b.self.global_name_bytes);
        r.maximum_bytes = kAccountSettingsDisplayNameBytes;
        break;
    case Form::pronouns:
        r.initial = view(b.profile.pronouns, b.profile.pronouns_bytes);
        r.maximum_bytes = kAccountSettingsPronounsBytes;
        break;
    case Form::bio:
        r.initial = view(b.profile.bio, b.profile.bio_bytes);
        r.maximum_bytes = kAccountSettingsBioBytes;
        r.single_line = false;
        break;
    case Form::username:
        r.initial = view(b.self.username, b.self.username_bytes);
        break;
    case Form::email_send:
        append(b.context, b.context_bytes, utf8_literal("We'll need to verify your old email address, "));
        append(b.context, b.context_bytes, view(b.email_mask, b.email_mask_bytes));
        r.context = append(b.context, b.context_bytes, utf8_literal(", in order to change it."));
        break;
    case Form::phone_number:
        r.placeholder = utf8_literal("+1 555 123 4567");
        break;
    case Form::phone_code:
        append(b.context, b.context_bytes, utf8_literal("We sent a code to "));
        append(b.context, b.context_bytes, field(field_extra));
        r.context = append(b.context, b.context_bytes, utf8_literal(". Enter it to verify your phone number."));
        break;
    case Form::totp_setup: {
        b.detail_bytes = 0;
        append(b.detail, b.detail_bytes, utf8_literal("2FA Key (Manual entry)\n"));
        r.qr_caption = grouped_secret();
        char uri[kQrCodeLargeMaximumPayloadBytes + 1];
        const u32 uri_bytes = totp_uri(view(b.self.email, b.self.email_bytes), field(field_extra), {uri, sizeof(uri)});
        if (uri_bytes && qr_code_encode_m(ConstSlice<u8>{reinterpret_cast<const u8*>(uri), uri_bytes}, 1, &b.totp_qr))
            r.qr = qr_code_modules(b.totp_qr);
        secure_clear_bytes(uri, sizeof(uri));
        break;
    }
    case Form::new_codes:
    case Form::backup_list:
        r.detail = codes_detail();
        r.cancel = utf8_literal("Close");
        if (form == Form::backup_list && field(field_token2).bytes) {
            r.option = utf8_literal("Generate New Backup Codes");
            r.save_option = form_save_option;
        }
        break;
    case Form::key_rename:
        r.initial = view(b.keys[g_owner.target].name, b.keys[g_owner.target].name_bytes);
        break;
    case Form::key_name:
        r.initial = utf8_literal("My Key");
        break;
    case Form::token_revoke: {
        const auto& app = b.apps[g_owner.target];
        append(b.context, b.context_bytes, utf8_literal("Deauthorize "));
        append(b.context, b.context_bytes, app.name_bytes ? view(app.name, app.name_bytes) : utf8_literal("this app"));
        r.title = append(b.context, b.context_bytes, utf8_literal("?"));
        break;
    }
    case Form::connection_remove: {
        const auto& connection = b.connections[g_owner.target];
        append(b.context, b.context_bytes, utf8_literal("Disconnect "));
        append(b.context, b.context_bytes, view(connection.name, connection.name_bytes));
        r.title = append(b.context, b.context_bytes, utf8_literal("?"));
        break;
    }
    case Form::consent_off:
        r.cancel = utf8_literal("No, take me back!");
        break;
    case Form::mfa_code:
        r.choices_label = utf8_literal("Verify with");
        r.choices = {g_mfa_choices, mfa_choices()};
        r.save = nullptr;
        r.save_choices = form_save_choices;
        r.placeholder = utf8_literal("Code");
        break;
    default:
        break;
    }
    g_owner.form_shown = application_text_form_open(r);
    if (!g_owner.form_shown) {
        log_u64(LogLevel::warning, "account_settings", "form_open_failed", static_cast<u64>(form));
        set_status(g_owner.flow_panel, utf8_literal("Couldn't open that dialog. Try again."), true);
        clear_flow();
        application_settings_refresh_pages();
    }
}

// Digits, letters and nothing else: a code typed with spaces or dashes.
Utf8View compact_code(Utf8View text, char (&output)[64]) noexcept {
    u32 at = 0;
    for (u32 i = 0; i < text.bytes && at < sizeof(output); ++i) {
        const char c = text.data[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            output[at++] = c;
    }
    return {output, at};
}
// An E.164 number: '+' and 8 to 15 digits, typed with any spacing.
bool phone_number(Utf8View text, char (&output)[24], u32* bytes) noexcept {
    u32 at = 0;
    bool plus = false;
    for (u32 i = 0; i < text.bytes; ++i) {
        const char c = text.data[i];
        if (c == '+' && !at && !plus) {
            plus = true;
            output[at++] = c;
        } else if (c >= '0' && c <= '9') {
            if (at + 1 >= sizeof(output))
                return false;
            output[at++] = c;
        } else if (c != ' ' && c != '-' && c != '(' && c != ')' && c != '.')
            return false;
    }
    *bytes = at;
    return plus && at >= 9 && at <= 16;
}
bool reject(Utf8View text) noexcept {
    form_status(text);
    return false;
}

// What went wrong with a security key, as the dialog says it.
enum class KeyFailure : u8 {
    generic,
    unavailable,
    cancelled,
    timed_out,
    excluded,
    unsupported,
    no_device,
    cannot_open,
    pin_blocked,
    pin_not_set,
    key_full,
};
Utf8View key_failure_text(KeyFailure failure, bool registering) noexcept {
    switch (failure) {
    case KeyFailure::unavailable:
        return utf8_literal("Security keys need libfido2, which isn't installed.");
    case KeyFailure::cancelled:
        return registering ? utf8_literal("Registration was cancelled.") : utf8_literal("Verification was cancelled.");
    case KeyFailure::timed_out:
        return utf8_literal("Your security key wasn't touched in time. Try again.");
    case KeyFailure::excluded:
        return utf8_literal("This security key is already registered to your account.");
    case KeyFailure::unsupported:
        return utf8_literal("This security key isn't supported.");
    case KeyFailure::no_device:
        return utf8_literal("No security key was found. Plug it in and try again.");
    case KeyFailure::cannot_open:
        return utf8_literal("Couldn't reach your security key. Check that you may access it.");
    case KeyFailure::pin_blocked:
        return utf8_literal("Your security key's PIN is blocked.");
    case KeyFailure::pin_not_set:
        return utf8_literal("Set a PIN on your security key first.");
    case KeyFailure::key_full:
        return utf8_literal("Your security key has no room for another credential.");
    default:
        return registering ? utf8_literal("Couldn't register your security key. Try again.")
                           : utf8_literal("Your security key wasn't verified. Try again or choose another method.");
    }
}

#if defined(_WIN32)
// Registers a key through Windows (the OS prompt the loop waits in): the credential JSON for the second request goes
// into field_token2.
__forceinline bool register_key_windows(KeyFailure* failure) noexcept {
    auto& b = *g_owner.bank;
    *failure = KeyFailure::generic;
    auto* creation = static_cast<WebAuthnMfaCreationOptions*>(pages_allocate(sizeof(WebAuthnMfaCreationOptions)));
    if (!creation)
        return false;
    SecureBuffer credential{};
    bool ok = webauthn_mfa_decode_creation(view(b.webauthn, b.webauthn_bytes), {b.structurals, 8192}, 16, creation).ok() &&
              secure_buffer_initialize(&credential, kAccountSettingsTextBytes).ok();
    if (ok) {
        const WindowsWebAuthnResult made = windows_webauthn_register(g_app.window, *creation, &credential);
        ok = made.ok() && set_field(field_token2, {reinterpret_cast<const char*>(credential.data), credential.bytes});
        switch (made.error) {
        case WindowsWebAuthnError::cancelled:
            *failure = KeyFailure::cancelled;
            break;
        case WindowsWebAuthnError::timed_out:
            *failure = KeyFailure::timed_out;
            break;
        case WindowsWebAuthnError::excluded:
            *failure = KeyFailure::excluded;
            break;
        case WindowsWebAuthnError::unsupported:
            *failure = KeyFailure::unsupported;
            break;
        default:
            break;
        }
    }
    secure_buffer_release(&credential);
    secure_clear_bytes(creation, sizeof(*creation));
    pages_release(creation);
    return ok;
}
#else
// Starts the Linux ceremony (platform/desktop/webauthn.hpp) for the challenge the flow holds: a registration (true) or
// a verification (false). Its wakes carry a token with the top bit set, which application_account_settings_webauthn_progress
// answers.
bool start_ceremony(bool registering) noexcept {
    auto& b = *g_owner.bank;
    auto* options = static_cast<WebAuthnMfaCreationOptions*>(pages_allocate(sizeof(WebAuthnMfaCreationOptions)));
    if (!options)
        return false;
    const Waker waker{window_surface_native(g_app.window), wake_begin_webauthn};
    const u64 token = (u64{1} << 63) | ++g_owner.key_serial;
    const Utf8View challenge = view(b.webauthn, b.webauthn_bytes);
    bool started = false;
    if (registering) {
        started = webauthn_mfa_decode_creation(challenge, {b.structurals, 8192}, 16, options).ok() &&
                  webauthn_ceremony_start_creation(*options, waker, token);
    } else {
        // A verification's options are the request's: the storage holds them as the creation's first member.
        WebAuthnMfaRequestOptions* request = &options->request;
        started = webauthn_mfa_decode_request(challenge, {b.structurals, 8192}, 16, request).ok() &&
                  webauthn_ceremony_start(*request, waker, token);
    }
    secure_clear_bytes(options, sizeof(*options));
    pages_release(options);
    if (started) {
        g_owner.key_ceremony = registering ? 2 : 1;
        g_owner.key_token = token;
    }
    return started;
}

KeyFailure failure_of(WebAuthnCeremonyError error) noexcept {
    switch (error) {
    case WebAuthnCeremonyError::unavailable:
        return KeyFailure::unavailable;
    case WebAuthnCeremonyError::cancelled:
        return KeyFailure::cancelled;
    case WebAuthnCeremonyError::timed_out:
        return KeyFailure::timed_out;
    case WebAuthnCeremonyError::excluded:
        return KeyFailure::excluded;
    case WebAuthnCeremonyError::unsupported:
        return KeyFailure::unsupported;
    case WebAuthnCeremonyError::no_device:
        return KeyFailure::no_device;
    case WebAuthnCeremonyError::cannot_open:
        return KeyFailure::cannot_open;
    case WebAuthnCeremonyError::pin_blocked:
        return KeyFailure::pin_blocked;
    case WebAuthnCeremonyError::pin_not_set:
        return KeyFailure::pin_not_set;
    case WebAuthnCeremonyError::key_full:
        return KeyFailure::key_full;
    default:
        return KeyFailure::generic;
    }
}

// The key wants its PIN: the PIN dialog, saying so when the last one was refused.
void ceremony_pin() noexcept {
    auto& b = *g_owner.bank;
    const WebAuthnPinPrompt prompt = webauthn_ceremony_pin_prompt();
    Utf8View status{};
    if (prompt.wrong) {
        message(utf8_literal("Wrong PIN."));
        if (prompt.retries >= 0) {
            char count[16];
            append(b.message, b.message_bytes, utf8_literal(" "));
            append(b.message, b.message_bytes, {count, decimal_write(count, static_cast<u64>(prompt.retries))});
            append(b.message, b.message_bytes, prompt.retries == 1 ? utf8_literal(" attempt left.")
                                                                   : utf8_literal(" attempts left."));
        }
        status = view(b.message, b.message_bytes);
    }
    if (g_owner.form == Form::key_pin && g_owner.form_shown) {
        g_owner.busy = false;
        form_status(status);
        return;
    }
    g_owner.carry = status;
    transition(Form::key_pin);
}

// The ceremony ended: its credential goes on with the flow (the name to give a new key, or the verification's
// request); a failure is told on the dialog the flow came from.
void ceremony_done() noexcept {
    const bool registering = g_owner.key_ceremony == 2;
    SecureBuffer credential{};
    WebAuthnCeremonyResult ended{WebAuthnCeremonyError::failed, 0};
    bool stored = false;
    if (secure_buffer_initialize(&credential, kAccountSettingsTextBytes).ok()) {
        ended = webauthn_ceremony_finish(&credential);
        stored = ended.ok() &&
                 set_field(registering ? field_token2 : field_mfa,
                           {reinterpret_cast<const char*>(credential.data), credential.bytes});
    } else {
        webauthn_ceremony_release();
    }
    secure_buffer_release(&credential);
    g_owner.key_ceremony = 0;
    if (stored) {
        if (registering)
            transition(Form::key_name);
        else
            queue(Op::mfa_finish);
        return;
    }
    const Utf8View text = key_failure_text(ended.ok() ? KeyFailure::generic : failure_of(ended.error), registering);
    const Form back = registering ? Form::key_register : Form::mfa_code;
    if (g_owner.form == back && g_owner.form_shown) {
        g_owner.busy = false;
        form_status(text);
        return;
    }
    g_owner.carry = text;
    transition(back);
}
#endif

bool form_save(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!form_valid(key) || g_owner.busy || action_pending())
        return false;
    auto& b = *g_owner.bank;
    char code[64];
    switch (g_owner.form) {
    case Form::display_name:
    case Form::pronouns:
    case Form::bio:
        if (!set_field(field_text, text))
            return false;
        queue(g_owner.form == Form::display_name ? Op::display_name : Op::profile_patch, 0,
              g_owner.form == Form::bio ? 1 : 0);
        return false;
    case Form::username:
        if (text.bytes < 2)
            return reject(utf8_literal("Must be between 2 and 32 in length."));
        set_field(field_text, text);
        g_owner.form = Form::username_password;
        g_owner.advance = true;
        return true;
    case Form::username_password:
        set_field(field_password, text);
        queue(Op::username);
        return false;
    case Form::password_current:
        set_field(field_password, text);
        g_owner.form = Form::password_new;
        g_owner.advance = true;
        return true;
    case Form::password_new:
        if (utf8_validate(text).codepoints < 8)
            return reject(utf8_literal("Must be between 8 and 72 in length."));
        set_field(field_extra, text);
        g_owner.form = Form::password_confirm;
        g_owner.advance = true;
        return true;
    case Form::password_confirm:
        if (!utf8_equal(text, field(field_extra)))
            return reject(utf8_literal("Passwords do not match!"));
        queue(Op::password);
        return false;
    case Form::email_code:
    case Form::phone_code:
    case Form::backup_key: {
        const Utf8View compact = compact_code(text, code);
        if (!compact.bytes)
            return reject(utf8_literal("Enter the code."));
        set_field(field_text, compact);
        secure_clear_bytes(code, sizeof(code));
        queue(g_owner.form == Form::email_code   ? Op::email_verify_code
              : g_owner.form == Form::phone_code ? Op::phone_verify
                                                 : Op::backup_codes);
        return false;
    }
    case Form::email_new: {
        u32 at = 0;
        for (u32 i = 0; i < text.bytes; ++i)
            at += text.data[i] == '@';
        if (at != 1 || text.bytes < 3)
            return reject(utf8_literal("Not a well formed email address."));
        set_field(field_extra, text);
        g_owner.form = Form::email_password;
        g_owner.advance = true;
        return true;
    }
    case Form::email_password:
        set_field(field_password, text);
        queue(Op::email);
        return false;
    case Form::phone_number: {
        char number[24];
        u32 bytes = 0;
        if (!phone_number(text, number, &bytes))
            return reject(utf8_literal("Enter the number with its country code, starting with +."));
        set_field(field_extra, {number, bytes});
        queue(Op::phone_code);
        return false;
    }
    case Form::phone_password:
    case Form::phone_remove:
    case Form::sms_disable:
    case Form::backup_password:
    case Form::account_disable:
    case Form::account_delete:
        if (!text.bytes)
            return reject(utf8_literal("Enter your password."));
        set_field(field_password, text);
        queue(g_owner.form == Form::phone_password    ? Op::phone_add
              : g_owner.form == Form::phone_remove    ? Op::phone_remove
              : g_owner.form == Form::sms_disable     ? Op::sms_disable
              : g_owner.form == Form::backup_password ? Op::backup_challenge
              : g_owner.form == Form::account_disable ? Op::account_disable
                                                      : Op::account_delete);
        return false;
    case Form::totp_setup: {
        const Utf8View compact = compact_code(text, code);
        if (compact.bytes != 6)
            return reject(utf8_literal("Invalid two-factor code"));
        set_field(field_text, compact);
        secure_clear_bytes(code, sizeof(code));
        queue(Op::totp_enable);
        return false;
    }
    case Form::totp_remove:
        queue(Op::totp_disable);
        return false;
    case Form::sms_enable:
        queue(Op::sms_enable);
        return false;
    case Form::new_codes:
    case Form::backup_list:
        g_owner.form = Form::none;
        return true;
    case Form::key_rename:
        if (!text.bytes)
            return reject(utf8_literal("Enter a name."));
        set_field(field_text, text);
        queue(Op::security_key_rename, b.keys[g_owner.target].id);
        return false;
    case Form::key_register: {
        // Starts the key's ceremony: one OS prompt the loop waits in on Windows, a worker the dialog waits on elsewhere.
#if defined(_WIN32)
        KeyFailure failure{};
        if (!register_key_windows(&failure))
            return reject(key_failure_text(failure, true));
        g_owner.form = Form::key_name;
        g_owner.advance = true;
        return true;
#else
        if (!start_ceremony(true))
            return reject(key_failure_text(webauthn_platform_available() ? KeyFailure::generic : KeyFailure::unavailable,
                                           true));
        g_owner.busy = true;
        form_status(utf8_literal("Interact with your security key. You may need to tap your device or confirm a prompt."));
        return false;
#endif
    }
    case Form::key_pin:
#if !defined(_WIN32)
        if (!webauthn_ceremony_submit_pin({reinterpret_cast<const u8*>(text.data), text.bytes}))
            return reject(utf8_literal("Enter your security key's PIN: 4 to 63 characters."));
        g_owner.busy = true;
        form_status(utf8_literal("Interact with your security key."));
#endif
        return false;
    case Form::key_name:
        if (!text.bytes)
            return reject(utf8_literal("Enter a name."));
        set_field(field_text, text);
        queue(Op::security_key_create);
        return false;
    case Form::key_delete:
        queue(Op::security_key_delete, b.keys[g_owner.target].id);
        return false;
    case Form::session_logout:
    case Form::logout_all:
        queue(Op::session_logout);
        return false;
    case Form::token_revoke:
        queue(Op::token_revoke, b.apps[g_owner.target].token_id);
        return false;
    case Form::connection_remove:
        queue(Op::connection_delete);
        return false;
    case Form::consent_off:
        queue(Op::consent_patch, 0, static_cast<u8>(AccountConsent::usage_statistics));
        return false;
    case Form::mfa_password:
        if (!text.bytes)
            return reject(utf8_literal("Enter your password."));
        set_field(field_mfa, text);
        g_owner.mfa_type = utf8_literal("password");
        queue(Op::mfa_finish);
        return false;
    default:
        return false;
    }
}
// The backup codes dialog's "Generate New Backup Codes": the regenerate nonce with the emailed key.
bool form_save_option(TextFormOwnerKey key, bool checked) noexcept {
    if (!form_valid(key) || g_owner.busy || action_pending())
        return false;
    if (!checked) {
        g_owner.form = Form::none;
        return true;
    }
    queue(Op::backup_codes, 0, 1);
    return false;
}
#if defined(_WIN32)
// Runs the security-key ceremony for the verification's WebAuthn challenge (the OS prompt the loop waits in) and keeps
// the credential JSON for /mfa/finish.
bool security_key_assertion() noexcept {
    auto& b = *g_owner.bank;
    auto* request = static_cast<WebAuthnMfaRequestOptions*>(pages_allocate(sizeof(WebAuthnMfaRequestOptions)));
    if (!request)
        return false;
    bool ok = webauthn_mfa_decode_request(view(b.webauthn, b.webauthn_bytes), {b.structurals, 8192}, 16, request).ok();
    SecureBuffer credential{};
    if (ok) {
        ok = secure_buffer_initialize(&credential, kAccountSettingsTextBytes).ok() &&
             windows_webauthn_authenticate(g_app.window, *request, &credential).ok() &&
             set_field(field_mfa, {reinterpret_cast<const char*>(credential.data), credential.bytes});
    }
    secure_buffer_release(&credential);
    secure_clear_bytes(request, sizeof(*request));
    pages_release(request);
    return ok;
}
#endif
bool form_save_choices(TextFormOwnerKey key, Utf8View text, bool, u32 choice, u32) noexcept {
    if (!form_valid(key) || g_owner.busy || action_pending() || g_owner.form != Form::mfa_code)
        return false;
    if (choice >= 4 || !g_mfa_choices[choice].bytes)
        return reject(utf8_literal("Choose how to verify."));
    const u32 method = g_mfa_choice_methods[choice];
    g_owner.mfa_type = view(kMfaChoiceTypes[method], c_string_length(kMfaChoiceTypes[method]));
    if (kMfaChoiceMethods[method] == kAccountAuthenticatorWebauthn) {
#if defined(_WIN32)
        if (!security_key_assertion())
            return reject(utf8_literal("Your security key wasn't verified. Try again or choose another method."));
        queue(Op::mfa_finish);
#else
        if (!start_ceremony(false))
            return reject(key_failure_text(KeyFailure::generic, false));
        g_owner.busy = true;
        form_status(utf8_literal("Interact with your security key. You may need to tap your device or confirm a prompt."));
#endif
        return false;
    }
    char code[64];
    const Utf8View compact = compact_code(text, code);
    if (kMfaChoiceMethods[method] == kAccountAuthenticatorSms && !compact.bytes) {
        queue(Op::mfa_sms_send);
        return false;
    }
    if (!compact.bytes)
        return reject(utf8_literal("Enter the code."));
    set_field(field_mfa, compact);
    secure_clear_bytes(code, sizeof(code));
    queue(Op::mfa_finish);
    return false;
}

// ---- Rows ---------------------------------------------------------------------------------------------------------

// A row's texts are read only up to their lengths: emptying it needs no wipe of the buffers.
void clear_row(Row& row) noexcept {
    row.label_bytes = row.description_bytes = 0;
}
void append_row(Row& row, bool description, const Utf8View& text) noexcept {
    if (description)
        append(row.description, row.description_bytes, text);
    else
        append(row.label, row.label_bytes, text);
}
// One row of a list: its control, with the plain presentation.
UiSettingsControl& list_row(const Slice<UiSettingsControl>& controls,
                            const Slice<UiSettingsControlPresentation>& presentation, u32 at, u64 id,
                            const Utf8View& label, const Utf8View& description, UiSettingsControlKind kind, bool enabled,
                            const Utf8View& action) noexcept {
    presentation[at] = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
    return controls[at] = {id, label, description, kind, enabled, false, {}, 0, action, nullptr};
}
bool current_session(const AccountSession& session) noexcept {
    const Utf8View hash = application_user_settings_session_hash();
    return hash.bytes && hash.bytes == session.id_hash_bytes && bytes_equal(hash.data, session.id_hash, hash.bytes);
}
__forceinline Utf8View relative_time(u64 unix_ms, char (&output)[64]) noexcept {
    const auto formatted = format_timestamp_local(unix_ms / 1000, 'R', clock_unix_milliseconds() / 1000, {output, 64});
    return formatted.ok ? Utf8View{output, formatted.bytes} : Utf8View{};
}
void build_session_row(Row& row, const AccountSession& session, bool current) noexcept {
    clear_row(row);
    append_row(row, false, session.os_bytes ? view(session.os, session.os_bytes) : utf8_literal("Unknown device"));
    if (session.platform_bytes) {
        append_row(row, false, utf8_literal(" · "));
        append_row(row, false, view(session.platform, session.platform_bytes));
    }
    if (current)
        append_row(row, true, utf8_literal("This device"));
    if (session.location_bytes) {
        if (row.description_bytes)
            append_row(row, true, utf8_literal(" · "));
        append_row(row, true, view(session.location, session.location_bytes));
    }
    char relative[64];
    if (const Utf8View when = !current && session.last_used_ms ? relative_time(session.last_used_ms, relative) : Utf8View{};
        when.bytes) {
        if (row.description_bytes)
            append_row(row, true, utf8_literal(" · "));
        append_row(row, true, utf8_literal("Active "));
        append_row(row, true, when);
    }
}
// The notice row for one page: working, the last result, loading, failure.
bool project_notice(SettingsPanel panel, u16 loads, UiSettingsControl* control,
                    UiSettingsControlPresentation* presentation) noexcept {
    reset_value(&control->action_label);
    if (action_pending() && g_owner.flow_panel == panel && !g_owner.form_shown) {
        control->label = utf8_literal("Working…");
        reset_value(&control->description);
        return true;
    }
    if (g_owner.status.bytes && g_owner.status_panel == panel) {
        control->label = g_owner.status_error ? utf8_literal("Something went wrong") : utf8_literal("Done");
        control->description = g_owner.status;
        presentation->variant = g_owner.status_error ? UiSettingsVariant::critical : UiSettingsVariant::positive;
        return true;
    }
    switch (application_account_settings_connection()) {
    case AccountSettingsConnection::offline:
        control->label = utf8_literal("Offline");
        control->description = utf8_literal("Connect to Discord to see these details.");
        return true;
    case AccountSettingsConnection::connecting:
        control->label = utf8_literal("Connecting…");
        control->description = utf8_literal("These details load once MaiCord is connected.");
        return true;
    case AccountSettingsConnection::action_required:
        control->label = utf8_literal("Discord needs something from you");
        control->description = account_requirement_notice(g_app.account.status.required_action);
        presentation->variant = UiSettingsVariant::critical;
        return true;
    case AccountSettingsConnection::ready:
        break;
    }
    if (g_owner.failed & loads) {
        control->label = utf8_literal("Couldn't load these details");
        reset_value(&control->description);
        control->action_label = utf8_literal("Try again");
        presentation->variant = UiSettingsVariant::critical;
        return true;
    }
    if ((g_owner.loaded & loads) != loads) {
        control->label = utf8_literal("Loading…");
        reset_value(&control->description);
        return true;
    }
    return false;
}
__forceinline void update_masks() noexcept {
    auto& b = *g_owner.bank;
    b.email_mask_bytes = static_cast<u8>(account_settings_mask_email(view(b.self.email, b.self.email_bytes),
                                                                     {b.email_mask, sizeof(b.email_mask)}));
    b.phone_mask_bytes = static_cast<u8>(account_settings_mask_phone(view(b.self.phone, b.self.phone_bytes),
                                                                     {b.phone_mask, sizeof(b.phone_mask)}));
}

// Connected accounts: each platform's type and name (one NameList of pairs) and which display options it has
// (Userdoccers' connection types; the official client offers the status option for platforms that report activity and
// the details option for platforms with profile metadata).
constexpr char kPlatforms[] =
    "amazon-music\0" "Amazon Music\0" "battlenet\0" "Battle.net\0" "bluesky\0" "Bluesky\0" "bungie\0" "Bungie.net\0"
    "crunchyroll\0" "Crunchyroll\0" "domain\0" "Domain\0" "ebay\0" "eBay\0" "epicgames\0" "Epic Games\0"
    "facebook\0" "Facebook\0" "github\0" "GitHub\0" "instagram\0" "Instagram\0" "leagueoflegends\0" "League of Legends\0"
    "mastodon\0" "Mastodon\0" "paypal\0" "PayPal\0" "playstation\0" "PlayStation Network\0" "reddit\0" "Reddit\0"
    "riotgames\0" "Riot Games\0" "roblox\0" "Roblox\0" "soundcloud\0" "SoundCloud\0" "spotify\0" "Spotify\0"
    "steam\0" "Steam\0" "tiktok\0" "TikTok\0" "twitch\0" "Twitch\0" "twitter\0" "X\0"
    "xbox\0" "Xbox\0" "youtube\0" "YouTube\0";
enum PlatformOption : u8 { platform_activity = 1, platform_metadata = 2 };
constexpr u8 kPlatformOptions[]{platform_activity, platform_activity, 0, 0, platform_activity, 0, platform_metadata,
                                platform_activity, 0, 0, 0, platform_activity, 0, platform_metadata,
                                platform_activity, platform_metadata, platform_activity, platform_activity, 0,
                                platform_activity, platform_activity | platform_metadata, platform_metadata,
                                platform_activity, platform_metadata, platform_activity, platform_activity};
static_assert(name_list_count(kPlatforms) == 2 * sizeof(kPlatformOptions));
struct Platform {
    Utf8View name;
    u8 options;
};
bool& connection_option(AccountConnection& connection, AccountConnectionField field) noexcept {
    return field == AccountConnectionField::visibility            ? connection.visible
           : field == AccountConnectionField::metadata_visibility ? connection.metadata_visible
                                                                  : connection.show_activity;
}
// The connection's platform; unknown ones have no name.
Platform platform(const AccountConnection& connection) noexcept {
    u32 index = 0;
    for (NameList list{kPlatforms}; list; ++index) {
        const bool match = utf8_equal(view(connection.type, connection.type_bytes), list.name());
        list.next();
        if (match)
            return {list.name(), kPlatformOptions[index]};
        list.next();
    }
    return {};
}
// A connection's rows: the account (Disconnect), then its display options.
enum ConnectionPart : u32 { part_account, part_visible, part_metadata, part_activity, kConnectionParts };
Utf8View g_activity_labels[kAccountConnectionMaximum];
char g_activity_text[kAccountConnectionMaximum][48];

u32 connection_rows(SettingBinding binding, Slice<UiSettingsControl> controls,
                    Slice<UiSettingsControlPresentation> presentation) noexcept {
    auto& b = *g_owner.bank;
    const bool idle = online() && !action_pending();
    u32 count = 0;
    for (u32 i = 0; i < g_owner.connection_count && count + kConnectionParts <= controls.count; ++i) {
        const auto& connection = b.connections[i];
        const Platform p = platform(connection);
        const Utf8View name = p.name.bytes ? p.name : view(connection.type, connection.type_bytes);
        const auto row = [&](u32 part, Utf8View label, Utf8View description, UiSettingsControlKind kind, bool checked,
                             Utf8View action) noexcept {
            list_row(controls, presentation, count, settings_list_control_id(binding, i * kConnectionParts + part), label,
                     description, kind, idle, action)
                .checked = checked;
            if (part == part_account && action.bytes)
                presentation[count].variant = UiSettingsVariant::critical;
            ++count;
        };
        // Streamer Mode keeps connected accounts' names off screen.
        row(part_account, application_streamer_hide_personal() ? name : view(connection.name, connection.name_bytes),
            connection.revoked ? utf8_literal("This connection was revoked. Disconnect it and connect again.") : name,
            UiSettingsControlKind::action, false, utf8_literal("Disconnect"));
        if (connection.revoked)
            continue;
        row(part_visible, utf8_literal("Display on profile"), {}, UiSettingsControlKind::toggle, connection.visible, {});
        if (p.options & platform_metadata)
            row(part_metadata, utf8_literal("Display details on profile"), {}, UiSettingsControlKind::toggle,
                connection.metadata_visible, {});
        if (p.options & platform_activity) {
            u8 used = 0;
            append(g_activity_text[i], used, utf8_literal("Display "));
            append(g_activity_text[i], used, name);
            g_activity_labels[i] = append(g_activity_text[i], used, utf8_literal(" as your status"));
            row(part_activity, g_activity_labels[i], {}, UiSettingsControlKind::toggle, connection.show_activity, {});
        }
    }
    return count;
}

// Whether a data request may be made, and the page's line about it.
bool harvest_allowed(Utf8View* reason) noexcept {
    auto& b = *g_owner.bank;
    if (!b.self.verified) {
        *reason = utf8_literal("You must verify your email to request your data. Verify it in your account settings.");
        return false;
    }
    const u64 next = b.harvest.created_ms ? b.harvest.created_ms + kHarvestCooldownMs : 0;
    if (next > clock_unix_milliseconds()) {
        const auto formatted = format_timestamp_local(next / 1000, 'D', clock_unix_milliseconds() / 1000,
                                                      {b.date, sizeof(b.date)});
        b.message_bytes = 0;
        append(b.message, b.message_bytes, utf8_literal("You recently requested a copy of your data. You can request again on "));
        append(b.message, b.message_bytes, formatted.ok ? view(b.date, formatted.bytes) : utf8_literal("a later date"));
        *reason = append(b.message, b.message_bytes, utf8_literal("."));
        return false;
    }
    *reason = utf8_literal("If you need a copy of your personal data from your account's entire history, you can get it "
                           "here. Choose what to include below.");
    return true;
}
constexpr SettingBinding first_harvest = SettingBinding::harvest_account;
u32 harvest_bit(SettingBinding binding) noexcept {
    return static_cast<u32>(binding) - static_cast<u32>(first_harvest);
}
bool harvest_binding(SettingBinding binding) noexcept {
    return harvest_bit(binding) < 7;
}

// Messaging Permissions' blocked (list 0) or ignored (list 1) accounts, each with its Unblock or Unignore button; then
// how many more there are, or a line saying there are none.
__forceinline u32 relation_rows(SettingBinding binding, Slice<UiSettingsControl> controls,
                  Slice<UiSettingsControlPresentation> presentation) noexcept {
    auto& b = *g_owner.bank;
    const u32 list = binding == SettingBinding::ignored_list ? 1 : 0;
    const auto* store = g_app.account_ui ? application_friends_relationships() : nullptr;
    const bool idle = online();
    Row* rows = b.rows + kRelationFirstRow + list * (kRelationRows + 1);
    u32 count = 0, more = 0;
    for (u32 i = 0; store && i < store->count; ++i) {
        const auto& record = store->records[i];
        if (list ? !record.ignored : record.type != RelationshipType::blocked)
            continue;
        if (count == kRelationRows || count == controls.count) {
            ++more;
            continue;
        }
        Row& row = rows[count];
        clear_row(row);
        const Utf8View name = relationship_display_name(record, g_app.account_ui->user_store);
        append_row(row, false, name.bytes ? name : utf8_literal("Unknown user"));
        b.relation_ids[list][count] = record.user_id;
        list_row(controls, presentation, count, settings_list_control_id(binding, count), view(row.label, row.label_bytes),
                 {}, UiSettingsControlKind::action, idle, list ? utf8_literal("Unignore") : utf8_literal("Unblock"));
        ++count;
    }
    if (count >= controls.count || (count && !more))
        return count;
    Row& note = rows[kRelationRows];
    clear_row(note);
    if (more) {
        char text[32];
        copy_bytes(text, "And ", 4);
        u32 at = 4 + decimal_write(text + 4, more);
        copy_bytes(text + at, " more", 5);
        append_row(note, false, {text, at + 5});
    } else
        append_row(note, false, list ? utf8_literal("You haven't ignored anyone.") : utf8_literal("You haven't blocked anyone."));
    list_row(controls, presentation, count, settings_list_control_id(binding, 0xFF), view(note.label, note.label_bytes),
             {}, UiSettingsControlKind::notice, true, {});
    return count + 1;
}
} // namespace

void application_account_settings_visible(SettingsPanel panel) noexcept {
    scope();
    u16 wanted = 0;
    g_owner.social = panel == SettingsPanel::content_social;
    switch (panel) {
    case SettingsPanel::account:
        wanted = load_self | load_keys;
        break;
    case SettingsPanel::profiles:
        wanted = load_self | load_profile;
        break;
    case SettingsPanel::devices:
        wanted = load_sessions;
        break;
    case SettingsPanel::authorized_apps:
        wanted = load_tokens | load_connections;
        break;
    case SettingsPanel::data_privacy:
        wanted = load_self | load_consents | load_harvest;
        break;
    default:
        g_owner.reveal_email = g_owner.reveal_phone = false;
        g_owner.wanted = 0;
        return;
    }
    // Lists are refreshed every time their page opens.
    const u16 fresh = wanted & (load_sessions | load_tokens | load_keys | load_connections | load_harvest);
    g_owner.loaded &= ~fresh;
    g_owner.failed &= ~wanted;
    // Only the open page loads: a list another page wanted is not fetched after it closes.
    g_owner.wanted = wanted;
    if (g_owner.status_panel != panel)
        reset_value(&g_owner.status);
    // No refresh: this runs while the settings host projects, and it re-projects by itself.
}

bool application_account_settings_project(SettingBinding binding, UiSettingsControl* control,
                                          UiSettingsControlPresentation* presentation) noexcept {
    scope();
    const auto* b = g_owner.bank;
    const bool self = self_loaded();
    const bool profile = b && (g_owner.loaded & load_profile);
    const bool idle = online() && !action_pending();
    static constexpr Utf8View kEdit[]{utf8_literal("Edit"), utf8_literal("Reveal")};
    static constexpr Utf8View kEditHide[]{utf8_literal("Edit"), utf8_literal("Hide")};
    static constexpr Utf8View kPhone[]{utf8_literal("Edit"), utf8_literal("Remove"), utf8_literal("Reveal")};
    static constexpr Utf8View kPhoneHide[]{utf8_literal("Edit"), utf8_literal("Remove"), utf8_literal("Hide")};
    static constexpr Utf8View kAdd[]{utf8_literal("Add")};
    switch (binding) {
    case SettingBinding::account_devices:
        return true;
    case SettingBinding::account_card:
        control->label = self_name(false);
        control->description = self_name(true);
        presentation->flags = static_cast<u8>(presentation->flags | ui_settings_presentation_avatar);
        return control->label.bytes != 0;
    case SettingBinding::account_notice:
        return project_notice(SettingsPanel::account, load_self, control, presentation);
    case SettingBinding::profile_notice:
        return project_notice(SettingsPanel::profiles, load_self | load_profile, control, presentation);
    case SettingBinding::devices_notice:
        return project_notice(SettingsPanel::devices, load_sessions, control, presentation);
    case SettingBinding::apps_notice:
        return project_notice(SettingsPanel::authorized_apps, load_tokens | load_connections, control, presentation);
    case SettingBinding::data_notice:
        return project_notice(SettingsPanel::data_privacy, load_self | load_consents | load_harvest, control,
                              presentation);
    case SettingBinding::account_display_name:
    case SettingBinding::profile_display_name: {
        const Utf8View name = self ? view(b->self.global_name, b->self.global_name_bytes) : Utf8View{};
        control->description = name.bytes ? name : self ? utf8_literal("Not set") : self_name(false);
        control->enabled = self && idle;
        return true;
    }
    case SettingBinding::account_username:
        control->description = self_name(true);
        control->enabled = self && idle;
        return true;
    case SettingBinding::account_email:
    case SettingBinding::account_phone: {
        const bool email = binding == SettingBinding::account_email;
        if (!self) {
            // A buttons row needs its buttons: the first one shows, disabled, until the account loads.
            control->description = utf8_literal("Loading…");
            control->choices = {email ? kEdit : kPhone, 1};
            control->enabled = false;
            return true;
        }
        const Utf8View value = email ? view(b->self.email, b->self.email_bytes) : view(b->self.phone, b->self.phone_bytes);
        // Streamer Mode keeps the address masked and offers no Reveal.
        const bool hidden = application_streamer_hide_personal();
        const bool revealed = !hidden && (email ? g_owner.reveal_email : g_owner.reveal_phone);
        control->enabled = idle;
        if (!value.bytes) {
            control->description = email ? utf8_literal("You haven't added an email address yet.")
                                         : utf8_literal("You haven't added a phone number yet.");
            control->choices = {kAdd, 1};
            return true;
        }
        control->description = revealed ? value
                               : email  ? view(b->email_mask, b->email_mask_bytes)
                                        : view(b->phone_mask, b->phone_mask_bytes);
        if (email)
            control->choices = {revealed ? kEditHide : kEdit, hidden ? 1u : 2u};
        else
            control->choices = {revealed ? kPhoneHide : kPhone, hidden ? 2u : 3u};
        return true;
    }
    case SettingBinding::account_password:
        control->enabled = self && idle;
        return true;
    case SettingBinding::account_mfa:
        if (!self)
            return false;
        control->label = b->self.mfa_enabled ? utf8_literal("Multi-Factor Authentication is on")
                                             : utf8_literal("Multi-Factor Authentication is off");
        control->description = b->self.mfa_enabled
                                   ? utf8_literal("Your account asks for a second step when you sign in.")
                                   : utf8_literal("Protect your account with an authenticator app or a security key.");
        presentation->variant = b->self.mfa_enabled ? UiSettingsVariant::positive : UiSettingsVariant::standard;
        return true;
    case SettingBinding::account_totp:
        if (!self)
            return false;
        control->enabled = idle;
        if (totp_enabled()) {
            control->description = utf8_literal("Enabled");
            control->action_label = utf8_literal("Remove");
            presentation->variant = UiSettingsVariant::critical;
        } else {
            control->action_label = utf8_literal("Enable Authenticator App");
            presentation->variant = UiSettingsVariant::primary;
        }
        return true;
    case SettingBinding::account_sms:
        // SMS is a backup method: offered once an authenticator app is on (as the official client).
        if (!totp_enabled())
            return false;
        control->enabled = idle;
        if (sms_enabled()) {
            static char ending[48];
            u8 used = 0;
            append(ending, used, utf8_literal("Phone number ending in ****"));
            const Utf8View phone = view(b->self.phone, b->self.phone_bytes);
            control->description =
                append(ending, used, phone.bytes >= 4 ? view(phone.data + phone.bytes - 4, 4) : phone);
            control->action_label = utf8_literal("Remove");
            presentation->variant = UiSettingsVariant::critical;
        } else {
            control->description = b->self.phone_bytes
                                       ? utf8_literal("Use your phone as a backup way to sign in.")
                                       : utf8_literal("Add a phone number to your account to use SMS authentication.");
            control->enabled = idle && b->self.phone_bytes;
            control->action_label = utf8_literal("Enable SMS Authentication");
        }
        return true;
    case SettingBinding::account_key_add:
        if (!self)
            return false;
        control->enabled = idle && webauthn_platform_available();
        if (!webauthn_platform_available())
            control->description = utf8_literal("Security keys need libfido2, which isn't installed.");
        return true;
    case SettingBinding::account_backup_codes:
        if (!self || !b->self.mfa_enabled)
            return false;
        control->enabled = idle;
        return true;
    case SettingBinding::account_disable:
    case SettingBinding::account_delete:
        control->enabled = self && idle;
        return true;
    case SettingBinding::devices_logout_all: {
        if (!b || !(g_owner.loaded & load_sessions))
            return false;
        bool others = false;
        for (u32 i = 0; i < g_owner.session_count; ++i)
            others = others || !current_session(b->sessions[i]);
        control->enabled = idle;
        return others;
    }
    case SettingBinding::data_usage_statistics:
        if (!b || !(g_owner.loaded & load_consents))
            return false;
        control->checked = b->consents.usage_statistics;
        control->enabled = idle;
        return true;
    case SettingBinding::data_request: {
        if (!self || !(g_owner.loaded & load_harvest))
            return false;
        Utf8View reason{};
        const bool allowed = harvest_allowed(&reason);
        control->description = reason;
        control->enabled = idle && allowed && g_owner.harvest_types;
        presentation->variant = UiSettingsVariant::primary;
        return true;
    }
    case SettingBinding::data_activity_link:
        return true;
    case SettingBinding::profile_pronouns:
    case SettingBinding::profile_bio: {
        const bool pronouns = binding == SettingBinding::profile_pronouns;
        const Utf8View value = !profile ? Utf8View{}
                               : pronouns ? view(b->profile.pronouns, b->profile.pronouns_bytes)
                                          : view(b->profile.bio, b->profile.bio_bytes);
        control->description = !profile ? utf8_literal("Loading…") : value.bytes ? value : utf8_literal("Not set");
        control->enabled = profile && idle;
        return true;
    }
    case SettingBinding::devices_list:
    case SettingBinding::apps_list:
    case SettingBinding::connections_list:
    case SettingBinding::security_keys_list:
    case SettingBinding::blocked_list:
    case SettingBinding::ignored_list:
        return true;
    default:
        if (harvest_binding(binding)) {
            Utf8View reason{};
            if (!self || !(g_owner.loaded & load_harvest) || !harvest_allowed(&reason))
                return false;
            control->checked = (g_owner.harvest_types >> harvest_bit(binding)) & 1;
            control->enabled = idle;
            return true;
        }
        return false;
    }
}

u32 application_account_settings_list(SettingBinding binding, Slice<UiSettingsControl> controls,
                                      Slice<UiSettingsControlPresentation> presentation) noexcept {
    auto* b = g_owner.bank;
    if (binding == SettingBinding::blocked_list || binding == SettingBinding::ignored_list)
        return b ? relation_rows(binding, controls, presentation) : 0;
    const u16 load = binding == SettingBinding::devices_list         ? load_sessions
                    : binding == SettingBinding::apps_list          ? load_tokens
                    : binding == SettingBinding::connections_list   ? load_connections
                    : binding == SettingBinding::security_keys_list ? load_keys
                                                                    : 0;
    if (!b || !load || !(g_owner.loaded & load))
        return 0;
    if (binding == SettingBinding::connections_list) {
        const u32 count = connection_rows(binding, controls, presentation);
        if (count || !controls.count)
            return count;
        list_row(controls, presentation, 0, settings_list_control_id(binding, 0xFF), utf8_literal("No connections"),
                 utf8_literal("Accounts you connect to Discord show here."), UiSettingsControlKind::notice, true, {});
        return 1;
    }
    const bool idle = online() && !action_pending();
    u32 count = 0;
    if (binding == SettingBinding::security_keys_list) {
        static constexpr Utf8View kKeyActions[]{utf8_literal("Rename"), utf8_literal("Remove")};
        for (u32 i = 0; i < g_owner.key_count && count < controls.count; ++i) {
            const auto& key = b->keys[i];
            list_row(controls, presentation, count, settings_list_control_id(binding, i), view(key.name, key.name_bytes),
                     utf8_literal("Security key"), UiSettingsControlKind::buttons, idle, {})
                .choices = {kKeyActions, 2};
            ++count;
        }
        return count;
    }
    const bool devices = binding == SettingBinding::devices_list;
    const u32 total = devices ? g_owner.session_count : g_owner.app_count;
    const u32 row_base = devices ? 0 : kAccountSessionMaximum;
    // The current device leads the list, as in the official client.
    for (u32 pass = 0; pass < (devices ? 2u : 1u); ++pass)
        for (u32 i = 0; i < total && count < controls.count; ++i) {
            Row& row = b->rows[row_base + i];
            bool current = false;
            if (devices) {
                current = current_session(b->sessions[i]);
                if ((pass == 0) != current)
                    continue;
                build_session_row(row, b->sessions[i], current);
            } else {
                const auto& app = b->apps[i];
                clear_row(row);
                append_row(row, false, app.name_bytes ? view(app.name, app.name_bytes) : utf8_literal("Unknown app"));
                append_row(row, true, view(app.scopes, app.scopes_bytes));
            }
            list_row(controls, presentation, count, settings_list_control_id(binding, i),
                     view(row.label, row.label_bytes), view(row.description, row.description_bytes),
                     UiSettingsControlKind::action, idle || current,
                     current ? Utf8View{} : devices ? utf8_literal("Log out") : utf8_literal("Deauthorize"));
            if (current)
                presentation[count].flags = ui_settings_presentation_accent;
            else
                presentation[count].variant = UiSettingsVariant::critical;
            ++count;
        }
    if (!count && controls.count) {
        list_row(controls, presentation, 0, settings_list_control_id(binding, 0xFF),
                 devices ? utf8_literal("No devices") : utf8_literal("No authorized apps"), {},
                 UiSettingsControlKind::notice, true, {});
        count = 1;
    }
    return count;
}

bool application_account_settings_bank(UiSettingsControl** controls,
                                       UiSettingsControlPresentation** presentation) noexcept {
    scope();
    if (!bank())
        return false;
    *controls = g_owner.bank->controls;
    if (presentation)
        *presentation = g_owner.bank->presentation;
    return true;
}

bool application_account_settings_activate(SettingBinding binding, bool checked, u32 choice) noexcept {
    scope();
    auto* b = g_owner.bank;
    const bool self = self_loaded();
    if (binding == SettingBinding::account_notice || binding == SettingBinding::profile_notice ||
        binding == SettingBinding::devices_notice || binding == SettingBinding::apps_notice ||
        binding == SettingBinding::data_notice) {
        g_owner.failed = 0;
        reset_value(&g_owner.status);
        application_settings_refresh_pages();
        return true;
    }
    if (binding == SettingBinding::account_card)
        return application_settings_open_key(utf8_literal("profiles"));
    if (binding == SettingBinding::data_activity_link)
        return application_settings_open_key(utf8_literal("activity_privacy"));
    if (binding == SettingBinding::account_devices)
        return application_settings_open_key(utf8_literal("devices"));
    if (!b || !online() || action_pending() || g_owner.form_shown)
        return false;
    if (harvest_binding(binding)) {
        g_owner.harvest_types = static_cast<u8>(checked ? g_owner.harvest_types | (1u << harvest_bit(binding))
                                                        : g_owner.harvest_types & ~(1u << harvest_bit(binding)));
        application_settings_refresh_pages();
        return true;
    }
    switch (binding) {
    case SettingBinding::account_display_name:
    case SettingBinding::profile_display_name:
        if (!self)
            return false;
        begin(Form::display_name, binding == SettingBinding::account_display_name ? SettingsPanel::account
                                                                                 : SettingsPanel::profiles);
        return true;
    case SettingBinding::profile_pronouns:
    case SettingBinding::profile_bio:
        if (!(g_owner.loaded & load_profile))
            return false;
        begin(binding == SettingBinding::profile_pronouns ? Form::pronouns : Form::bio, SettingsPanel::profiles);
        return true;
    case SettingBinding::account_username:
        if (!self)
            return false;
        begin(Form::username, SettingsPanel::account);
        return true;
    case SettingBinding::account_password:
        if (!self)
            return false;
        begin(Form::password_current, SettingsPanel::account);
        return true;
    case SettingBinding::account_email:
    case SettingBinding::account_phone: {
        if (!self)
            return false;
        const bool email = binding == SettingBinding::account_email;
        const bool present = email ? b->self.email_bytes : b->self.phone_bytes;
        const u32 reveal = email ? 1 : 2;
        if (present && choice == reveal) {
            bool& revealed = email ? g_owner.reveal_email : g_owner.reveal_phone;
            revealed = !revealed;
            application_settings_refresh_pages();
            return true;
        }
        if (!email && present && choice == 1) {
            begin(Form::phone_remove, SettingsPanel::account);
            return true;
        }
        // A verified address proves itself with a code first (Userdoccers: email-verification).
        begin(email ? (present && b->self.verified ? Form::email_send : Form::email_new) : Form::phone_number,
              SettingsPanel::account);
        return true;
    }
    case SettingBinding::account_totp:
        if (!self)
            return false;
        if (totp_enabled()) {
            begin(Form::totp_remove, SettingsPanel::account);
            return true;
        }
        clear_flow();
        if (!generate_secret()) {
            set_status(SettingsPanel::account, utf8_literal("Couldn't start the setup. Try again."), true);
            application_settings_refresh_pages();
            return false;
        }
        g_owner.flow_panel = SettingsPanel::account;
        set_status(SettingsPanel::account, {}, false);
        transition(Form::totp_setup);
        return true;
    case SettingBinding::account_sms:
        if (!totp_enabled())
            return false;
        begin(sms_enabled() ? Form::sms_disable : Form::sms_enable, SettingsPanel::account);
        return true;
    case SettingBinding::account_key_add:
        if (!self || !webauthn_platform_available())
            return false;
        clear_flow();
        g_owner.flow_panel = SettingsPanel::account;
        queue(Op::security_key_create);
        return true;
    case SettingBinding::account_backup_codes:
        begin(Form::backup_password, SettingsPanel::account);
        return true;
    case SettingBinding::account_disable:
    case SettingBinding::account_delete:
        begin(binding == SettingBinding::account_disable ? Form::account_disable : Form::account_delete,
              SettingsPanel::account);
        return true;
    case SettingBinding::devices_logout_all:
        begin(Form::logout_all, SettingsPanel::devices);
        return true;
    case SettingBinding::data_usage_statistics:
        if (!(g_owner.loaded & load_consents))
            return false;
        if (!checked) {
            // Turning it off asks first: the switch stays on until the answer.
            begin(Form::consent_off, SettingsPanel::data_privacy);
            return false;
        }
        clear_flow();
        g_owner.flow_panel = SettingsPanel::data_privacy;
        g_owner.bank->consents.usage_statistics = true;
        queue(Op::consent_patch, 1, static_cast<u8>(AccountConsent::usage_statistics));
        return true;
    case SettingBinding::data_request: {
        Utf8View reason{};
        if (!harvest_allowed(&reason) || !g_owner.harvest_types)
            return false;
        clear_flow();
        g_owner.flow_panel = SettingsPanel::data_privacy;
        queue(Op::harvest_create, 0, g_owner.harvest_types);
        return true;
    }
    default:
        return false;
    }
}

bool application_account_settings_activate_row(u64 id, bool checked, u32 choice) noexcept {
    scope();
    const auto* b = g_owner.bank;
    if (!b || id < kSettingsListFirst || !online() || action_pending() || g_owner.form_shown)
        return false;
    const auto binding = settings_list_binding(id);
    const u32 index = settings_list_index(id);
    if (binding == SettingBinding::devices_list && index < g_owner.session_count &&
        !current_session(b->sessions[index])) {
        g_owner.target = index;
        begin(Form::session_logout, SettingsPanel::devices);
        return true;
    }
    if (binding == SettingBinding::apps_list && index < g_owner.app_count) {
        g_owner.target = index;
        begin(Form::token_revoke, SettingsPanel::authorized_apps);
        return true;
    }
    if ((binding == SettingBinding::blocked_list || binding == SettingBinding::ignored_list) && index < kRelationRows) {
        // Unblocking and unignoring run at once, as in the official client; the list follows the store.
        const bool ignored = binding == SettingBinding::ignored_list;
        return application_friends_user_action(b->relation_ids[ignored ? 1 : 0][index],
                                               ignored ? ApplicationFriendsUserAction::unignore
                                                       : ApplicationFriendsUserAction::unblock);
    }
    if (binding == SettingBinding::security_keys_list && index < g_owner.key_count && choice <= 1) {
        g_owner.target = index;
        begin(choice ? Form::key_delete : Form::key_rename, SettingsPanel::account);
        return true;
    }
    if (binding == SettingBinding::connections_list && index / kConnectionParts < g_owner.connection_count) {
        g_owner.target = index / kConnectionParts;
        const u32 part = index % kConnectionParts;
        if (part == part_account) {
            begin(Form::connection_remove, SettingsPanel::authorized_apps);
            return true;
        }
        clear_flow();
        g_owner.flow_panel = SettingsPanel::authorized_apps;
        const auto field = part == part_visible    ? AccountConnectionField::visibility
                           : part == part_metadata ? AccountConnectionField::metadata_visibility
                                                   : AccountConnectionField::show_activity;
        // The switch shows the change at once; a refused one switches back.
        connection_option(g_owner.bank->connections[g_owner.target], field) = checked;
        queue(Op::connection_patch, checked ? 1 : 0, static_cast<u8>(field));
        return true;
    }
    return false;
}

bool application_account_settings_pending(u64) noexcept {
    scope();
    return online() && !g_owner.request_id &&
           (g_owner.queued || (g_owner.wanted & ~g_owner.loaded & ~g_owner.failed));
}

u64 application_account_settings_next_wake() noexcept {
    return application_account_settings_pending(0) ? 1 : 0;
}

bool application_account_settings_submit(u64 request_id, u64 now_ms) noexcept {
    if (!request_id || !application_account_settings_pending(now_ms) || !bank())
        return false;
    auto& b = *g_owner.bank;
    AccountSettingsRequest request{};
    Utf8View texts[3]{};
    u32 text_count = 0;
    const auto text = [&](Utf8View value) noexcept { texts[text_count++] = value; };
    char hashes[kAccountSessionMaximum * (kAccountSettingsHashBytes + 1)];
    u16 load = 0;
    if (g_owner.queued) {
        request.operation = g_owner.op;
        request.id = g_owner.op_id;
        request.option = g_owner.op_option;
        if (g_owner.verified)
            request.mfa = field(field_mfa);
        switch (g_owner.op) {
        case Op::session_logout: {
            // One device, or every device but this one.
            u32 at = 0;
            for (u32 i = 0; i < g_owner.session_count; ++i) {
                const auto& session = b.sessions[i];
                if (current_session(session) || (g_owner.form == Form::session_logout && i != g_owner.target))
                    continue;
                if (at)
                    hashes[at++] = ',';
                copy_bytes(hashes + at, session.id_hash, session.id_hash_bytes);
                at += session.id_hash_bytes;
            }
            request.payload = {reinterpret_cast<const u8*>(hashes), at};
            break;
        }
        case Op::display_name:
        case Op::profile_patch:
            request.payload = {reinterpret_cast<const u8*>(b.fields[field_text]), b.field_bytes[field_text]};
            request.field = g_owner.op_option ? AccountProfileField::bio : AccountProfileField::pronouns;
            request.option = 0;
            break;
        case Op::username:
            text(field(field_text));
            text(field(field_password));
            break;
        case Op::password:
            text(field(field_password));
            text(field(field_extra));
            break;
        case Op::email_verify_code:
            text(field(field_text));
            break;
        case Op::email:
            text(field(field_extra));
            text(field(field_token));
            text(field(field_password));
            break;
        case Op::phone_code:
            text(field(field_extra));
            break;
        case Op::phone_verify:
            text(field(field_extra));
            text(field(field_text));
            break;
        case Op::phone_add:
            text(field(field_token));
            text(field(field_password));
            break;
        case Op::totp_enable:
            text(field(field_text));
            text(field(field_extra));
            break;
        case Op::phone_remove:
        case Op::sms_disable:
        case Op::backup_challenge:
        case Op::account_disable:
        case Op::account_delete:
            text(field(field_password));
            break;
        case Op::backup_codes:
            text(field(field_text));
            text(field(g_owner.op_option ? field_token2 : field_token));
            break;
        case Op::security_key_create:
            // The first request asks for a ticket and a challenge (no texts); the second names the key and sends the
            // credential the key made.
            if (field(field_token2).bytes) {
                text(field(field_text));
                text(field(field_token));
                text(field(field_token2));
            }
            break;
        case Op::security_key_rename:
            text(field(field_text));
            break;
        case Op::mfa_finish:
            text(view(b.mfa.ticket, b.mfa.ticket_bytes));
            text(g_owner.mfa_type);
            text(field(field_mfa));
            break;
        case Op::mfa_sms_send:
            text(view(b.mfa.ticket, b.mfa.ticket_bytes));
            break;
        case Op::connection_patch:
        case Op::connection_delete: {
            const auto& connection = b.connections[g_owner.target];
            text(view(connection.type, connection.type_bytes));
            text(view(connection.id, connection.id_bytes));
            break;
        }
        default:
            break;
        }
        request.texts = {texts, text_count};
    } else {
        const u16 missing = g_owner.wanted & ~g_owner.loaded & ~g_owner.failed;
        load = static_cast<u16>(missing & -missing);
        // Each load bit's GET, in bit order.
        constexpr Op kLoads[]{Op::self_user,   Op::profile,  Op::sessions,      Op::tokens,
                              Op::security_keys, Op::connections, Op::consents, Op::harvest};
        u32 bit = 0;
        while (bit + 1 < sizeof(kLoads) / sizeof(kLoads[0]) && !(load >> bit & 1))
            ++bit;
        request.operation = kLoads[bit];
        if (load == load_profile)
            request.id = g_owner.self_user_id;
    }
    const auto admitted = account_module_account_settings(g_app.account.instance, &request, request_id);
    secure_clear_bytes(hashes, sizeof(hashes));
    if (!admitted.ok()) {
        if (admitted.error != RestClientSubmitError::busy) {
            log_u64(LogLevel::warning, "account_settings", "submit_refused", static_cast<u64>(admitted.error));
            if (g_owner.queued) {
                g_owner.queued = false;
                if (g_owner.form_shown) {
                    g_owner.busy = false;
                    form_status(failure_text(g_owner.op));
                } else
                    finish(failure_text(g_owner.op), true);
            } else
                g_owner.failed |= load;
            application_settings_refresh_pages();
        }
        return false;
    }
    g_owner.request_id = request_id;
    g_owner.action_inflight = g_owner.queued;
    g_owner.inflight = g_owner.queued ? g_owner.op : request.operation;
    g_owner.inflight_load = load;
    g_owner.queued = false;
    application_settings_refresh_pages();
    return true;
}

namespace {
// A token the answer carries replaces the session's: the old one stops working (password and MFA changes).
void adopt_token(Utf8View body, Slice<u32> structurals) noexcept {
    char token[256];
    const u32 bytes = account_settings_decode_member(body, structurals, utf8_literal("token"), {token, sizeof(token)});
    if (bytes != ~0u && bytes &&
        !account_module_credential_replace(g_app.account.instance, reinterpret_cast<const u8*>(token), bytes))
        log_write(LogLevel::warning, "account_settings", "credential_replace_failed");
    secure_clear_bytes(token, sizeof(token));
}
bool keep_member(Utf8View body, Slice<u32> structurals, Utf8View name, Field f) noexcept {
    auto& b = *g_owner.bank;
    secure_clear_bytes(b.fields[f], b.field_bytes[f]);
    const u32 bytes = account_settings_decode_member(body, structurals, name, {b.fields[f], kFieldBytes});
    b.field_bytes[f] = static_cast<u16>(bytes == ~0u ? 0 : bytes);
    return bytes != ~0u && bytes;
}
// A successful answer to the flow's request: the next dialog, or the flow's result.
__forceinline void action_succeeded(Op op, Utf8View body, Slice<u32> structurals) noexcept {
    auto& b = *g_owner.bank;
    switch (op) {
    case Op::mfa_finish:
        if (!keep_member(body, structurals, utf8_literal("token"), field_mfa)) {
            finish(utf8_literal("Discord didn't confirm it's you. Try again."), true);
            return;
        }
        g_owner.verified = true;
        queue(g_owner.retry);
        return;
    case Op::mfa_sms_send: {
        char phone[32];
        const u32 bytes = account_settings_decode_member(body, structurals, utf8_literal("phone"), {phone, sizeof(phone)});
        g_owner.busy = false;
        message(utf8_literal("We've sent a message to "));
        append(b.message, b.message_bytes, bytes != ~0u ? view(phone, bytes) : utf8_literal("your phone"));
        form_status(append(b.message, b.message_bytes, utf8_literal(". Please enter the code you received.")));
        return;
    }
    case Op::email_challenge:
        transition(Form::email_code);
        return;
    case Op::email_verify_code:
        if (!keep_member(body, structurals, utf8_literal("token"), field_token)) {
            finish(failure_text(op), true);
            return;
        }
        transition(Form::email_new);
        return;
    case Op::phone_code:
        transition(Form::phone_code);
        return;
    case Op::phone_verify:
        if (!keep_member(body, structurals, utf8_literal("token"), field_token)) {
            finish(failure_text(op), true);
            return;
        }
        transition(Form::phone_password);
        return;
    case Op::backup_challenge:
        if (!keep_member(body, structurals, utf8_literal("nonce"), field_token)) {
            finish(failure_text(op), true);
            return;
        }
        keep_member(body, structurals, utf8_literal("regenerate_nonce"), field_token2);
        transition(Form::backup_key);
        return;
    case Op::backup_codes: {
        const u32 count = account_settings_decode_backup_codes(body, structurals, {b.codes, kAccountBackupCodeMaximum});
        g_owner.code_count = count == ~0u ? 0 : count;
        // A nonce works once: regenerating spends the second.
        if (g_owner.op_option) {
            secure_clear_bytes(b.fields[field_token2], b.field_bytes[field_token2]);
            b.field_bytes[field_token2] = 0;
        }
        transition(Form::backup_list);
        return;
    }
    case Op::totp_enable: {
        adopt_token(body, structurals);
        const u32 count = account_settings_decode_backup_codes(body, structurals, {b.codes, kAccountBackupCodeMaximum});
        g_owner.code_count = count == ~0u ? 0 : count;
        g_owner.loaded &= ~load_self;
        set_status(SettingsPanel::account, utf8_literal("Your authenticator app is on. Save your backup codes."), false);
        transition(g_owner.code_count ? Form::new_codes : Form::none);
        if (!g_owner.code_count)
            clear_flow();
        return;
    }
    case Op::password:
    case Op::totp_disable:
        adopt_token(body, structurals);
        g_owner.loaded &= ~load_self;
        finish(op == Op::password ? utf8_literal("Your password was changed.")
                                  : utf8_literal("Your authenticator app was removed."),
               false);
        return;
    case Op::username:
    case Op::email:
    case Op::phone_add:
    case Op::phone_remove:
    case Op::sms_enable:
    case Op::sms_disable:
    case Op::display_name:
        g_owner.loaded &= ~load_self;
        finish(op == Op::username      ? utf8_literal("Your username was changed.")
               : op == Op::email       ? utf8_literal("Check your inbox to verify your new email address.")
               : op == Op::phone_add   ? utf8_literal("Your phone number was updated.")
               : op == Op::phone_remove ? utf8_literal("Your phone number was removed.")
               : op == Op::sms_enable  ? utf8_literal("SMS authentication is on.")
               : op == Op::sms_disable ? utf8_literal("SMS authentication was removed.")
                                       : utf8_literal("Your changes were saved."),
               false);
        return;
    case Op::profile_patch:
        g_owner.loaded &= ~load_profile;
        finish(utf8_literal("Your changes were saved."), false);
        return;
    case Op::security_key_create:
        if (!field(field_token2).bytes) {
            // The first answer: the ticket and the challenge to answer with the key.
            const u32 challenge =
                account_settings_decode_member(body, structurals, utf8_literal("challenge"), {b.webauthn, sizeof(b.webauthn)});
            b.webauthn_bytes = challenge == ~0u ? 0 : challenge;
            if (!keep_member(body, structurals, utf8_literal("ticket"), field_token) || !b.webauthn_bytes) {
                finish(utf8_literal("Couldn't start the registration. Try again."), true);
                return;
            }
            transition(Form::key_register);
            return;
        }
        // The second: the new key, and the backup codes when it is the account's first way to verify.
        adopt_token(body, structurals);
        {
            const u32 count = account_settings_decode_backup_codes(body, structurals, {b.codes, kAccountBackupCodeMaximum});
            g_owner.code_count = count == ~0u ? 0 : count;
        }
        g_owner.loaded &= ~(load_keys | load_self);
        set_status(SettingsPanel::account,
                   g_owner.code_count ? utf8_literal("Your security key was added. Save your backup codes.")
                                      : utf8_literal("Your security key was added."),
                   false);
        transition(g_owner.code_count ? Form::new_codes : Form::none);
        if (!g_owner.code_count)
            clear_flow();
        return;
    case Op::security_key_rename:
    case Op::security_key_delete:
        g_owner.loaded &= ~(load_keys | load_self);
        finish(op == Op::security_key_rename ? utf8_literal("The security key was renamed.")
                                             : utf8_literal("The security key was removed."),
               false);
        return;
    case Op::account_disable:
    case Op::account_delete:
        // Discord ends every session of the account: this one signs out when its connection closes.
        finish(op == Op::account_disable ? utf8_literal("Your account was disabled.")
                                         : utf8_literal("Your account is scheduled for deletion."),
               false);
        return;
    case Op::session_logout:
        g_owner.loaded &= ~load_sessions;
        finish(g_owner.form == Form::logout_all ? utf8_literal("All known devices were logged out.")
                                                : utf8_literal("The device was logged out."),
               false);
        return;
    case Op::token_revoke:
        g_owner.loaded &= ~load_tokens;
        finish(utf8_literal("The app was deauthorized."), false);
        return;
    case Op::connection_delete:
        g_owner.loaded &= ~load_connections;
        finish(utf8_literal("The account was disconnected."), false);
        return;
    case Op::connection_patch:
        // The switch already shows the new state.
        finish({}, false);
        return;
    case Op::consent_patch:
        // The answer leaves usage_statistics out (Userdoccers: user-settings#consents-object).
        b.consents.usage_statistics = g_owner.op_id != 0;
        finish({}, false);
        return;
    case Op::harvest_create:
        if (!account_settings_decode_harvest(body, structurals, &b.harvest))
            b.harvest.created_ms = clock_unix_milliseconds();
        g_owner.harvest_types = 0;
        finish(utf8_literal("Request received. Our privacy farmers have begun harvesting your data. This can take up to "
                            "30 days, but we'll email you when it's done."),
               false);
        return;
    default:
        finish({}, false);
        return;
    }
}
} // namespace

bool application_account_settings_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!g_owner.request_id || response.request_id != g_owner.request_id)
        return false;
    g_owner.request_id = 0;
    const bool action = g_owner.action_inflight;
    const Op op = g_owner.inflight;
    const u16 load = g_owner.inflight_load;
    g_owner.action_inflight = false;
    g_owner.inflight_load = 0;
    auto* b = g_owner.bank;
    if (!b)
        return true;
    const bool ok = response.succeeded();
    const Slice<u32> structurals{b->structurals, sizeof(b->structurals) / sizeof(u32)};
    if (action) {
        if (ok)
            action_succeeded(op, body, structurals);
        else {
            const u64 code = response.transport_ok() ? account_settings_error_code(body, structurals) : 0;
            // Discord asks for MFA (or the password) first: verify, then send the same request with the token.
            if (code == kMfaRequired && op != Op::mfa_finish && !g_owner.verified &&
                account_settings_decode_mfa_challenge(body, structurals, &b->mfa, {b->webauthn, sizeof(b->webauthn)},
                                                      &b->webauthn_bytes)) {
                g_owner.retry = op;
                transition(b->mfa.methods == kAccountMfaMethodPassword ? Form::mfa_password : Form::mfa_code);
            } else {
                b->message_bytes = 0;
                const u32 bytes = response.transport_ok()
                                      ? account_settings_error_message(body, structurals, {b->message, sizeof(b->message)})
                                      : 0;
                b->message_bytes = static_cast<u16>(bytes);
                const Utf8View reason = bytes ? view(b->message, bytes) : failure_text(op);
                if (op == Op::connection_patch && g_owner.target < g_owner.connection_count)
                    connection_option(b->connections[g_owner.target],
                                      static_cast<AccountConnectionField>(g_owner.op_option)) = !g_owner.op_id;
                if (op == Op::consent_patch)
                    b->consents.usage_statistics = !g_owner.op_id;
                // A failure after verifying cannot reuse the spent ticket: the flow ends with it.
                if (g_owner.form_shown && !g_owner.verified) {
                    g_owner.busy = false;
                    form_status(reason);
                } else
                    finish(reason, true);
            }
        }
        application_settings_refresh_pages();
        return true;
    }
    bool decoded = false;
    if (ok) {
        switch (load) {
        case load_self:
            decoded = account_settings_decode_self(body, structurals, &b->self);
            if (decoded)
                update_masks();
            break;
        case load_profile:
            decoded = account_settings_decode_profile(body, structurals, &b->profile);
            break;
        case load_sessions: {
            const u32 count = account_settings_decode_sessions(body, structurals, {b->sessions, kAccountSessionMaximum});
            decoded = count != ~0u;
            g_owner.session_count = decoded ? count : 0;
            break;
        }
        case load_tokens: {
            const u32 count = account_settings_decode_tokens(body, structurals, {b->apps, kAccountAuthorizedAppMaximum});
            decoded = count != ~0u;
            g_owner.app_count = decoded ? count : 0;
            break;
        }
        case load_keys: {
            const u32 count = account_settings_decode_security_keys(body, structurals, {b->keys, kAccountSecurityKeyMaximum});
            decoded = count != ~0u;
            g_owner.key_count = decoded ? count : 0;
            break;
        }
        case load_connections: {
            const u32 count = account_settings_decode_connections(body, structurals, {b->connections, kAccountConnectionMaximum});
            decoded = count != ~0u;
            g_owner.connection_count = decoded ? count : 0;
            break;
        }
        case load_consents:
            decoded = account_settings_decode_consents(body, structurals, &b->consents);
            break;
        case load_harvest:
            // 204: no request was ever made.
            reset_value(&b->harvest);
            decoded = !body.bytes || account_settings_decode_harvest(body, structurals, &b->harvest);
            break;
        default:
            break;
        }
    }
    if (decoded)
        g_owner.loaded |= load;
    else {
        // An account without security keys may refuse to list them; the page shows none.
        if (load == load_keys) {
            g_owner.key_count = 0;
            g_owner.loaded |= load;
        } else
            g_owner.failed |= load;
        log_u64(LogLevel::warning, "account_settings", "load_failed", response.status_code);
    }
    application_settings_refresh_pages();
    return true;
}

#if !defined(_WIN32)
bool application_account_settings_webauthn_progress(u64 token, u64 kind) noexcept {
    if (!g_owner.key_ceremony || token != g_owner.key_token || !g_owner.bank)
        return false;
    if (kind == kWebAuthnWakePin) {
        if (webauthn_ceremony_state() == WebAuthnCeremonyState::need_pin)
            ceremony_pin();
    } else if (kind == kWebAuthnWakeDone && webauthn_ceremony_state() == WebAuthnCeremonyState::done) {
        ceremony_done();
    }
    application_settings_refresh_pages();
    return true;
}
#endif

void application_account_settings_release() noexcept {
    if (g_owner.form_shown)
        application_text_form_cancel_owner(form_valid);
    application_settings_drop_borrowed();
    release_bank();
    const u64 lifetime = g_owner.lifetime + 1;
    reset_value(&g_owner);
    g_owner.lifetime = lifetime;
}

AccountSettingsConnection application_account_settings_connection() noexcept {
    const auto& host = g_app.account;
    const auto& status = host.status;
    if (!host.instance || host.stop_failed || !g_app.account_ui || status.failed)
        return AccountSettingsConnection::offline;
    if (status.required_action != AccountRequirement::none)
        return AccountSettingsConnection::action_required;
    // Running is the coordinator phase; READY's self user lands in the drain after it.
    if (!status.running || !status.profile_id || !g_app.account_ui->chat_state.self_user_id)
        return AccountSettingsConnection::connecting;
    return AccountSettingsConnection::ready;
}

bool application_account_settings_changed() noexcept {
    const u8 connection = static_cast<u8>(application_account_settings_connection());
    const auto* relationships = application_friends_relationships();
    const u32 revision = relationships ? relationships->revision + 1 : 0;
    const bool social = g_owner.social && revision != g_owner.relationship_revision;
    g_owner.relationship_revision = revision;
    if (connection == g_connection)
        return social;
    g_connection = connection;
    return true;
}

void application_account_settings_test_open() noexcept {
#if defined(MAICORD_TEST_HOOKS) && !defined(_WIN32)
    static bool opened;
    const char* key = test_hook("MAICORD_TEST_USER_SETTINGS");
    if (opened || !key || !g_app.renderer_ready || application_account_settings_connection() != AccountSettingsConnection::ready)
        return;
    opened = application_settings_open_key({key, c_string_length(key)});
#endif
}
} // namespace maicord::app_detail
