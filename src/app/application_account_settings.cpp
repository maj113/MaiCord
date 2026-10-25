#include "application_account_settings.hpp"
#include "application_link_trust.hpp"
#include "application_settings.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "application_user_settings.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/protocol/account_requirement.hpp"
#include "maicord/ui/timestamp_format.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/protocol/account_settings.hpp"
#include "account_ui_runtime.hpp"

namespace maicord::app_detail {
namespace {
enum Load : u8 { load_self = 1, load_profile = 2, load_sessions = 4, load_tokens = 8 };
enum class Mutation : u8 { none, session_logout, token_revoke, display_name, pronouns, bio };
constexpr u32 kRows = kAccountSessionMaximum + kAccountAuthorizedAppMaximum;
constexpr u64 kTwoFactorRequired = 60003;
struct Row {
    char label[112], description[176];
    u8 label_bytes, description_bytes;
};
struct Bank {
    AccountSelfUser self;
    AccountProfileText profile;
    AccountSession sessions[kAccountSessionMaximum];
    AccountAuthorizedApp apps[kAccountAuthorizedAppMaximum];
    Row rows[kRows];
    char email_mask[128], phone_mask[32];
    u8 email_mask_bytes, phone_mask_bytes;
    char text[kAccountSettingsBioBytes];
    u16 text_bytes;
    char hash[kAccountSettingsHashBytes];
    u8 hash_bytes;
    u32 structurals[8192];
};
struct Owner {
    Bank* bank;
    u64 profile_id, self_user_id, request_id, lifetime, target_id;
    u32 session_count, app_count, revision;
    u8 loaded, failed, wanted, inflight_load;
    Mutation queued, inflight;
    bool reveal_email, reveal_phone, status_error;
    Utf8View status;
    SettingsPanel status_panel;
};
Owner g_owner{};
// Last connection the settings host projected; survives owner scope resets.
u8 g_connection = 0xFF;

bool online() noexcept {
    return application_account_settings_connection() == AccountSettingsConnection::ready;
}
bool mutation_pending() noexcept {
    return g_owner.queued != Mutation::none || g_owner.inflight != Mutation::none;
}
void changed() noexcept {
    ++g_owner.revision;
    application_settings_refresh_pages();
}
void release_bank() noexcept {
    if (!g_owner.bank)
        return;
    secure_clear_bytes(g_owner.bank, sizeof(Bank));
    VirtualFree(g_owner.bank, 0, MEM_RELEASE);
    g_owner.bank = nullptr;
}
Bank* bank() noexcept {
    if (!g_owner.bank)
        g_owner.bank = static_cast<Bank*>(VirtualAlloc(nullptr, sizeof(Bank), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return g_owner.bank;
}
void scope() noexcept {
    const u64 profile = g_app.account.status.profile_id;
    const u64 self = g_app.account_ui ? g_app.account_ui->chat_state.self_user_id : 0;
    if (g_owner.profile_id == profile && g_owner.self_user_id == self)
        return;
    if (g_owner.request_id && g_app.account.api.account_settings && g_app.account.instance)
        g_app.account.api.account_settings(g_app.account.instance, nullptr, g_owner.request_id);
    release_bank();
    const u64 lifetime = g_owner.lifetime + 1;
    const u32 revision = g_owner.revision + 1;
    // The open page stays wanted: a page opened before READY loads once it lands.
    const u8 wanted = g_owner.wanted;
    reset_value(&g_owner);
    g_owner.lifetime = lifetime;
    g_owner.revision = revision;
    g_owner.wanted = wanted;
    g_owner.profile_id = profile;
    g_owner.self_user_id = self;
}
void set_status(SettingsPanel panel, Utf8View text, bool error) noexcept {
    g_owner.status = text;
    g_owner.status_error = error;
    g_owner.status_panel = panel;
}
Utf8View view(const char* data, u32 bytes) noexcept {
    return {data, bytes};
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
// The service's own pages are trusted hosts; they still leave through the link flow.
bool open_browser(Utf8View url) noexcept {
    return application_link_open(url, true);
}
Utf8View failure_text(Mutation mutation) noexcept {
    switch (mutation) {
    case Mutation::session_logout:
        return utf8_literal("Couldn't log out that device. Try again.");
    case Mutation::token_revoke:
        return utf8_literal("Couldn't deauthorize that app. Try again.");
    default:
        return utf8_literal("Couldn't save your profile. Try again.");
    }
}
SettingsPanel mutation_panel(Mutation mutation) noexcept {
    return mutation == Mutation::session_logout ? SettingsPanel::devices
           : mutation == Mutation::token_revoke ? SettingsPanel::authorized_apps
           : mutation == Mutation::display_name ? SettingsPanel::account
                                                : SettingsPanel::profiles;
}

// Confirmation and edit dialogs. The key binds the dialog to this owner.
TextFormOwnerKey form_key(u64 collection, u64 item) noexcept {
    return {g_owner.lifetime, collection, item};
}
bool form_valid(TextFormOwnerKey key) noexcept {
    return key.scope == g_owner.lifetime && g_owner.bank && online();
}
bool form_save(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!form_valid(key) || mutation_pending())
        return false;
    auto& b = *g_owner.bank;
    const auto mutation = static_cast<Mutation>(key.collection);
    switch (mutation) {
    case Mutation::session_logout: {
        if (key.item >= g_owner.session_count)
            return false;
        const auto& session = b.sessions[key.item];
        copy_bytes(b.hash, session.id_hash, session.id_hash_bytes);
        b.hash_bytes = session.id_hash_bytes;
        break;
    }
    case Mutation::token_revoke:
        if (!key.item)
            return false;
        g_owner.target_id = key.item;
        break;
    case Mutation::display_name:
    case Mutation::pronouns:
    case Mutation::bio: {
        const u32 limit = mutation == Mutation::display_name ? kAccountSettingsDisplayNameBytes
                          : mutation == Mutation::pronouns   ? kAccountSettingsPronounsBytes
                                                             : kAccountSettingsBioBytes;
        if (text.bytes > limit)
            return false;
        copy_bytes(b.text, text.data, text.bytes);
        b.text_bytes = static_cast<u16>(text.bytes);
        break;
    }
    default:
        return false;
    }
    application_text_form_close(false, false);
    g_owner.queued = mutation;
    set_status(mutation_panel(mutation), {}, false);
    changed();
    return true;
}
void open_confirmation(Mutation mutation, u64 item, Utf8View title, Utf8View context, Utf8View accept) noexcept {
    ApplicationTextFormRequest request{};
    request.title = title;
    request.context = context;
    request.accept = accept;
    request.owner = form_key(static_cast<u64>(mutation), item);
    request.valid = form_valid;
    request.save = form_save;
    request.confirmation_only = true;
    application_text_form_open(request);
}
void open_editor(Mutation mutation, Utf8View title, Utf8View label, Utf8View initial, u32 codepoints, u32 bytes,
                 bool single_line) noexcept {
    ApplicationTextFormRequest request{};
    request.title = title;
    request.label = label;
    request.accept = utf8_literal("Save");
    request.initial = initial;
    request.owner = form_key(static_cast<u64>(mutation), 0);
    request.valid = form_valid;
    request.save = form_save;
    request.maximum_codepoints = codepoints;
    request.maximum_bytes = bytes;
    request.single_line = single_line;
    application_text_form_open(request);
}

void append(Row& row, bool description, Utf8View text) noexcept {
    char* data = description ? row.description : row.label;
    u8& used = description ? row.description_bytes : row.label_bytes;
    const u32 capacity = description ? sizeof(row.description) : sizeof(row.label);
    used = static_cast<u8>(used + utf8_copy_truncated(data + used, capacity - used, text));
}
bool current_session(const AccountSession& session) noexcept {
    const Utf8View hash = application_user_settings_session_hash();
    return hash.bytes && hash.bytes == session.id_hash_bytes && bytes_equal(hash.data, session.id_hash, hash.bytes);
}
void build_session_row(Row& row, const AccountSession& session, bool current) noexcept {
    reset_value(&row);
    append(row, false, session.os_bytes ? view(session.os, session.os_bytes) : utf8_literal("Unknown device"));
    if (session.platform_bytes) {
        append(row, false, utf8_literal(" · "));
        append(row, false, view(session.platform, session.platform_bytes));
    }
    if (current)
        append(row, true, utf8_literal("This device"));
    if (session.location_bytes) {
        if (row.description_bytes)
            append(row, true, utf8_literal(" · "));
        append(row, true, view(session.location, session.location_bytes));
    }
    if (!current && session.last_used_ms) {
        char relative[64];
        const auto formatted = format_timestamp_local(session.last_used_ms / 1000, 'R',
                                                              clock_unix_milliseconds() / 1000, {relative, 64});
        if (formatted.ok) {
            if (row.description_bytes)
                append(row, true, utf8_literal(" · "));
            append(row, true, utf8_literal("Active "));
            append(row, true, view(relative, formatted.bytes));
        }
    }
}
// The notice row for one page: loading, failure or the last mutation result.
bool project_notice(SettingsPanel panel, u8 loads, UiSettingsControl* control,
                    UiSettingsControlPresentation* presentation) noexcept {
    reset_value(&control->action_label);
    if (mutation_pending()) {
        const Mutation pending = g_owner.inflight != Mutation::none ? g_owner.inflight : g_owner.queued;
        if (mutation_panel(pending) != panel)
            return false;
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
void update_masks() noexcept {
    auto& b = *g_owner.bank;
    b.email_mask_bytes = static_cast<u8>(account_settings_mask_email(view(b.self.email, b.self.email_bytes),
                                                                     {b.email_mask, sizeof(b.email_mask)}));
    b.phone_mask_bytes = static_cast<u8>(account_settings_mask_phone(view(b.self.phone, b.self.phone_bytes),
                                                                     {b.phone_mask, sizeof(b.phone_mask)}));
}
} // namespace

void application_account_settings_visible(SettingsPanel panel) noexcept {
    scope();
    u8 wanted = 0;
    switch (panel) {
    case SettingsPanel::account:
        wanted = load_self;
        break;
    case SettingsPanel::profiles:
        wanted = load_self | load_profile;
        break;
    case SettingsPanel::devices:
        wanted = load_sessions;
        break;
    case SettingsPanel::authorized_apps:
        wanted = load_tokens;
        break;
    default:
        g_owner.reveal_email = g_owner.reveal_phone = false;
        return;
    }
    // Device and app lists are refreshed every time their page opens.
    const u8 fresh = wanted & (load_sessions | load_tokens);
    g_owner.loaded &= ~fresh;
    g_owner.failed &= ~wanted;
    g_owner.wanted |= wanted;
    if (g_owner.status_panel != panel)
        reset_value(&g_owner.status);
    // Called while the settings host projects; it re-projects by itself.
    ++g_owner.revision;
}

bool application_account_settings_project(SettingBinding binding, UiSettingsControl* control,
                                          UiSettingsControlPresentation* presentation) noexcept {
    scope();
    const auto* b = g_owner.bank;
    const bool self = b && (g_owner.loaded & load_self);
    const bool profile = b && (g_owner.loaded & load_profile);
    const bool idle = online() && !mutation_pending();
    switch (binding) {
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
        return project_notice(SettingsPanel::authorized_apps, load_tokens, control, presentation);
    case SettingBinding::account_display_name:
    case SettingBinding::profile_display_name: {
        const Utf8View name = self ? view(b->self.global_name, b->self.global_name_bytes) : Utf8View{};
        control->description = name.bytes ? name : self ? utf8_literal("Not set") : self_name(false);
        control->enabled = self && idle;
        return true;
    }
    case SettingBinding::account_username:
        control->description = self_name(true);
        control->action_label = utf8_literal("Edit in browser");
        return true;
    case SettingBinding::account_email:
    case SettingBinding::account_phone: {
        const bool email = binding == SettingBinding::account_email;
        const bool revealed = email ? g_owner.reveal_email : g_owner.reveal_phone;
        if (!self) {
            control->description = utf8_literal("Loading…");
            control->enabled = false;
            return true;
        }
        const Utf8View value = email ? view(b->self.email, b->self.email_bytes) : view(b->self.phone, b->self.phone_bytes);
        const Utf8View masked =
            email ? view(b->email_mask, b->email_mask_bytes) : view(b->phone_mask, b->phone_mask_bytes);
        if (!value.bytes) {
            control->description = utf8_literal("Not added");
            control->action_label = utf8_literal("Add in browser");
            return true;
        }
        control->description = revealed ? value : masked;
        control->action_label = revealed ? utf8_literal("Hide") : utf8_literal("Reveal");
        return true;
    }
    case SettingBinding::account_password:
        return true;
    case SettingBinding::account_mfa:
        if (!self)
            return false;
        control->label = b->self.mfa_enabled ? utf8_literal("Multi-Factor Authentication is on")
                                             : utf8_literal("Multi-Factor Authentication is off");
        control->description = b->self.mfa_enabled
                                   ? utf8_literal("Your account asks for a code when you sign in.")
                                   : utf8_literal("Turn it on in Discord to protect your account.");
        presentation->variant = b->self.mfa_enabled ? UiSettingsVariant::positive : UiSettingsVariant::standard;
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
        return true;
    default:
        return false;
    }
}

u32 application_account_settings_list(SettingBinding binding, Slice<UiSettingsControl> controls,
                                      Slice<UiSettingsControlPresentation> presentation) noexcept {
    auto* b = g_owner.bank;
    const bool devices = binding == SettingBinding::devices_list;
    const u8 load = devices ? load_sessions : load_tokens;
    if (!b || !(g_owner.loaded & load) || !controls.data || !presentation.data)
        return 0;
    const bool idle = online() && !mutation_pending();
    u32 count = 0;
    const u32 total = devices ? g_owner.session_count : g_owner.app_count;
    const u32 row_base = devices ? 0 : kAccountSessionMaximum;
    // The current device leads the list, as in the official client.
    for (u32 pass = 0; pass < (devices ? 2u : 1u); ++pass)
        for (u32 i = 0; i < total && count < controls.count && count < presentation.count; ++i) {
            Row& row = b->rows[row_base + i];
            UiSettingsControl& c = controls[count];
            UiSettingsControlPresentation& p = presentation[count];
            p = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
            if (devices) {
                const bool current = current_session(b->sessions[i]);
                if ((pass == 0) != current)
                    continue;
                build_session_row(row, b->sessions[i], current);
                c = {settings_list_control_id(binding, i), view(row.label, row.label_bytes),
                     view(row.description, row.description_bytes), UiSettingsControlKind::action, idle || current,
                     false, {}, 0, current ? Utf8View{} : utf8_literal("Log out"), nullptr};
                if (current)
                    p.flags = ui_settings_presentation_accent;
                else
                    p.variant = UiSettingsVariant::critical;
            } else {
                const auto& app = b->apps[i];
                reset_value(&row);
                append(row, false, app.name_bytes ? view(app.name, app.name_bytes) : utf8_literal("Unknown app"));
                append(row, true, view(app.scopes, app.scopes_bytes));
                c = {settings_list_control_id(binding, i), view(row.label, row.label_bytes),
                     view(row.description, row.description_bytes), UiSettingsControlKind::action, idle, false, {}, 0,
                     utf8_literal("Deauthorize"), nullptr};
                p.variant = UiSettingsVariant::critical;
            }
            ++count;
        }
    if (!count && controls.count && presentation.count) {
        controls[0] = {settings_list_control_id(binding, 0xFF), devices ? utf8_literal("No devices") : utf8_literal("No authorized apps"),
                       {}, UiSettingsControlKind::notice, true, false, {}, 0, {}, nullptr};
        presentation[0] = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
        count = 1;
    }
    return count;
}

bool application_account_settings_activate(SettingBinding binding) noexcept {
    scope();
    auto* b = g_owner.bank;
    const bool self = b && (g_owner.loaded & load_self);
    switch (binding) {
    case SettingBinding::account_card:
        return application_settings_open_key(utf8_literal("profiles"));
    case SettingBinding::account_notice:
    case SettingBinding::profile_notice:
    case SettingBinding::devices_notice:
    case SettingBinding::apps_notice:
        g_owner.failed = 0;
        changed();
        return true;
    case SettingBinding::account_display_name:
    case SettingBinding::profile_display_name:
        if (!self)
            return false;
        open_editor(Mutation::display_name, utf8_literal("Change your display name"),
                    utf8_literal("Display name"), view(b->self.global_name, b->self.global_name_bytes), 32,
                    kAccountSettingsDisplayNameBytes, true);
        return true;
    case SettingBinding::profile_pronouns:
    case SettingBinding::profile_bio: {
        if (!b || !(g_owner.loaded & load_profile))
            return false;
        const bool pronouns = binding == SettingBinding::profile_pronouns;
        open_editor(pronouns ? Mutation::pronouns : Mutation::bio,
                    pronouns ? utf8_literal("Change your pronouns") : utf8_literal("Edit about me"),
                    pronouns ? utf8_literal("Pronouns") : utf8_literal("About me"),
                    pronouns ? view(b->profile.pronouns, b->profile.pronouns_bytes) : view(b->profile.bio, b->profile.bio_bytes),
                    pronouns ? 40u : 190u, pronouns ? kAccountSettingsPronounsBytes : kAccountSettingsBioBytes, pronouns);
        return true;
    }
    case SettingBinding::account_email:
    case SettingBinding::account_phone: {
        const bool email = binding == SettingBinding::account_email;
        if (!self)
            return false;
        if (!(email ? b->self.email_bytes : b->self.phone_bytes))
            return open_browser(utf8_literal("https://discord.com/settings/account"));
        bool& revealed = email ? g_owner.reveal_email : g_owner.reveal_phone;
        revealed = !revealed;
        changed();
        return true;
    }
    case SettingBinding::account_username:
    case SettingBinding::account_password:
        return open_browser(utf8_literal("https://discord.com/settings/account"));
    default:
        return false;
    }
}

bool application_account_settings_activate_row(u64 id) noexcept {
    scope();
    const auto* b = g_owner.bank;
    if (!b || id < kSettingsListFirst || !online())
        return false;
    const auto binding = static_cast<SettingBinding>((id - kSettingsListFirst) >> 8);
    const u32 index = static_cast<u32>((id - kSettingsListFirst) & 0xFF);
    static char title[160];
    if (binding == SettingBinding::devices_list && index < g_owner.session_count &&
        !current_session(b->sessions[index])) {
        open_confirmation(Mutation::session_logout, index, utf8_literal("Log out this device?"),
                          utf8_literal("It will need to sign in again to use your account."),
                          utf8_literal("Log out"));
        return true;
    }
    if (binding == SettingBinding::apps_list && index < g_owner.app_count) {
        const auto& app = b->apps[index];
        u32 used = 0;
        const auto add = [&](Utf8View text) noexcept {
            const u32 bytes = minimum(text.bytes, static_cast<u32>(sizeof(title)) - used);
            copy_bytes(title + used, text.data, bytes);
            used += bytes;
        };
        add(utf8_literal("Deauthorize "));
        add(app.name_bytes ? view(app.name, app.name_bytes) : utf8_literal("this app"));
        add(utf8_literal("?"));
        open_confirmation(Mutation::token_revoke, app.token_id, {title, used},
                          utf8_literal("It loses access to your account until you authorize it again."),
                          utf8_literal("Deauthorize"));
        return true;
    }
    return false;
}

bool application_account_settings_pending(u64) noexcept {
    scope();
    return online() && !g_owner.request_id &&
           (g_owner.queued != Mutation::none || (g_owner.wanted & ~g_owner.loaded & ~g_owner.failed));
}

u64 application_account_settings_next_wake() noexcept {
    return application_account_settings_pending(0) ? 1 : 0;
}

bool application_account_settings_submit(u64 request_id, u64 now_ms) noexcept {
    if (!request_id || !application_account_settings_pending(now_ms) || !bank())
        return false;
    auto& b = *g_owner.bank;
    AccountSettingsRequest request{};
    u8 load = 0;
    switch (g_owner.queued) {
    case Mutation::session_logout:
        request.operation = AccountSettingsOperation::session_logout;
        request.payload = {reinterpret_cast<const u8*>(b.hash), b.hash_bytes};
        break;
    case Mutation::token_revoke:
        request.operation = AccountSettingsOperation::token_revoke;
        request.id = g_owner.target_id;
        break;
    case Mutation::display_name:
    case Mutation::pronouns:
    case Mutation::bio:
        request.operation = g_owner.queued == Mutation::display_name ? AccountSettingsOperation::display_name
                                                                     : AccountSettingsOperation::profile_patch;
        request.field = g_owner.queued == Mutation::bio ? AccountProfileField::bio : AccountProfileField::pronouns;
        request.payload = {reinterpret_cast<const u8*>(b.text), b.text_bytes};
        break;
    case Mutation::none: {
        const u8 missing = g_owner.wanted & ~g_owner.loaded & ~g_owner.failed;
        load = missing & load_self       ? load_self
               : missing & load_profile  ? load_profile
               : missing & load_sessions ? load_sessions
                                         : load_tokens;
        request.operation = load == load_self       ? AccountSettingsOperation::self_user
                            : load == load_profile  ? AccountSettingsOperation::profile
                            : load == load_sessions ? AccountSettingsOperation::sessions
                                                    : AccountSettingsOperation::tokens;
        if (load == load_profile)
            request.id = g_owner.self_user_id;
        break;
    }
    }
    const auto admitted = g_app.account.api.account_settings(g_app.account.instance, &request, request_id);
    if (!admitted.ok()) {
        if (admitted.error != RestClientSubmitError::busy) {
            if (g_owner.queued != Mutation::none)
                set_status(mutation_panel(g_owner.queued), failure_text(g_owner.queued), true);
            else
                g_owner.failed |= load;
            g_owner.queued = Mutation::none;
            changed();
        }
        return false;
    }
    g_owner.request_id = request_id;
    g_owner.inflight = g_owner.queued;
    g_owner.inflight_load = load;
    g_owner.queued = Mutation::none;
    changed();
    return true;
}

bool application_account_settings_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!g_owner.request_id || response.request_id != g_owner.request_id)
        return false;
    g_owner.request_id = 0;
    const Mutation mutation = g_owner.inflight;
    const u8 load = g_owner.inflight_load;
    g_owner.inflight = Mutation::none;
    g_owner.inflight_load = 0;
    auto* b = g_owner.bank;
    if (!b)
        return true;
    const bool ok = response.transport_ok() && response.status_code >= 200 && response.status_code < 300;
    const Slice<u32> structurals{b->structurals, sizeof(b->structurals) / sizeof(u32)};
    if (mutation != Mutation::none) {
        if (ok) {
            switch (mutation) {
            case Mutation::session_logout:
                g_owner.loaded &= ~load_sessions;
                set_status(SettingsPanel::devices, utf8_literal("The device was logged out."), false);
                break;
            case Mutation::token_revoke:
                g_owner.loaded &= ~load_tokens;
                set_status(SettingsPanel::authorized_apps, utf8_literal("The app was deauthorized."), false);
                break;
            default:
                g_owner.loaded &= ~(mutation == Mutation::display_name ? load_self : load_profile);
                set_status(mutation_panel(mutation), utf8_literal("Your changes were saved."), false);
                break;
            }
        } else {
            const u64 code = response.transport_ok() ? account_settings_error_code(body, structurals) : 0;
            set_status(mutation_panel(mutation),
                       code == kTwoFactorRequired
                           ? utf8_literal("Discord needs your two-factor code for this. Use Discord in your browser.")
                       : response.status_code == 400 ? utf8_literal("Discord didn't accept that change.")
                                                     : failure_text(mutation),
                       true);
        }
        b->text_bytes = 0;
        b->hash_bytes = 0;
        secure_clear_bytes(b->text, sizeof(b->text));
        changed();
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
        default:
            break;
        }
    }
    if (decoded)
        g_owner.loaded |= load;
    else {
        g_owner.failed |= load;
        log_u64(LogLevel::warning, "account_settings", "load_failed", response.status_code);
    }
    changed();
    return true;
}

void application_account_settings_release() noexcept {
    release_bank();
    const u64 lifetime = g_owner.lifetime + 1;
    reset_value(&g_owner);
    g_owner.lifetime = lifetime;
}

AccountSettingsConnection application_account_settings_connection() noexcept {
    const auto& host = g_app.account;
    const auto& status = host.status;
    if (!host.instance || host.stop_failed || !g_app.account_ui || !host.api.account_settings || status.failed)
        return AccountSettingsConnection::offline;
    if (status.required_action != AccountRequirement::none)
        return AccountSettingsConnection::action_required;
    // Running is the coordinator phase; READY's self user lands in the drain after it.
    if (!status.running || !status.profile_id || !g_app.account_ui->chat_state.self_user_id)
        return AccountSettingsConnection::connecting;
    return AccountSettingsConnection::ready;
}

bool application_account_settings_connection_changed() noexcept {
    const u8 connection = static_cast<u8>(application_account_settings_connection());
    if (connection == g_connection)
        return false;
    g_connection = connection;
    ++g_owner.revision;
    return true;
}
} // namespace maicord::app_detail
