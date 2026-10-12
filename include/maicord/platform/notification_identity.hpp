#pragma once
#include "maicord/platform/desktop_notifications.hpp"
namespace maicord {
// stale: both resources carry this nonce but a value (path, name, icon) drifted;
// notification_identity_create repairs it in place.
enum class NotificationIdentityError : u8 { none, invalid_input, foreign_resource, registry, process, rollback, not_owned, absent, stale };
struct NotificationIdentityResult {
    NotificationIdentityError error{};
    u32 native_error{};
    // True when this call created a key, or when a failed rollback left one behind.
    bool registry_created{};
    bool ok() const noexcept { return error == NotificationIdentityError::none; }
};
// Unpackaged toast identity, entirely in the per-user registry (no Start Menu
// shortcut): <classes>\AppUserModelId\<app_id> holds DisplayName, IconUri and
// CustomActivator, and <classes>\CLSID\{activator}\LocalServer32 launches the
// executable for a cold toast click. Both keys carry MaiCordOwner=<nonce>.
struct NotificationIdentityConfig {
    ConstSlice<wchar_t> app_id;
    GUID activator_id{};
    ConstSlice<wchar_t> executable_path;
    ConstSlice<wchar_t> display_name;
    // Optional absolute .png/.ico shown as the toast's app icon; empty leaves
    // Windows' generic icon.
    ConstSlice<wchar_t> icon_path;
    // ASCII ownership nonce (16..64 alphanumerics) recorded in both keys.
    ConstSlice<wchar_t> ownership_nonce;
    // HKCU subkey standing in for Software\Classes; empty selects the real one.
    // Tests point it at a scratch key so they never register with Windows.
    ConstSlice<wchar_t> classes_key;
};
bool notification_identity_valid(const NotificationIdentityConfig&) noexcept;
// Read-only. absent: neither key exists. foreign_resource: a key exists without
// this nonce. stale: owned but incomplete or with drifted values.
NotificationIdentityResult notification_identity_check(const NotificationIdentityConfig&) noexcept;
// Sets process identity only; never enrolls or mutates the registry.
NotificationIdentityResult notification_identity_set_process(ConstSlice<wchar_t>) noexcept;
// Explicit enrollment: creates both keys, or rewrites the values of keys that
// already carry this nonce. Never touches a key owned by anything else, and on
// failure deletes only keys this call created. LocalServer32 is the quoted
// executable plus --toast-activated; ServerExecutable is the unquoted path.
NotificationIdentityResult notification_identity_create(const NotificationIdentityConfig&) noexcept;
// Deletes only keys carrying this nonce (non-recursively). Refuses foreign keys.
NotificationIdentityResult notification_identity_remove_owned(const NotificationIdentityConfig&) noexcept;
}
