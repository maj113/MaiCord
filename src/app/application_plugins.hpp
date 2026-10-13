#pragma once
#include "maicord/app/plugin_host.hpp"
#include "maicord/app/plugin_sdk.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/friends_scene.hpp"
#include "maicord/app/plugin_bundle_install.hpp"
#include "maicord/app/plugin_catalog.hpp"
#include "maicord/app/plugin_preferences.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/core/win32_types.hpp"
// Declared as <Windows.h> declares it, without parsing it.
extern "C" __declspec(dllimport) int __stdcall WideCharToMultiByte(UINT code_page, DWORD flags,
    const wchar_t* wide, int wide_units, char* bytes, int byte_capacity, const char* default_char,
    BOOL* used_default_char);
namespace maicord { struct GatewayClientEvent; }
namespace maicord { struct MessageTextHit; }
namespace maicord::app_detail {
inline constexpr u32 kApplicationPluginPopupActionFirst = 0xF200;
// Plugin rows one native menu shows, across every plugin and target.
inline constexpr u32 kApplicationPluginMenuRows = 8;
inline constexpr u32 kApplicationPluginMenuTextBytes = 512;
// The native menu plugin rows are appended to. It fixes the popup owner the
// binder expects: member, message, channel/private channel, guild, or the
// Friends row menu (user rows owned by the Friends popup).
enum class ApplicationPluginMenuSurface : u8 { none, user, message, channel, guild, friends };
// What a message menu was opened on: MC_MENU_LINK, MC_MENU_ATTACHMENT or
// MC_MENU_EMOJI rows come before the message rows. Views are copied.
struct ApplicationPluginMenuExtra {
    u32 target;
    u32 attachment_index;
    u64 emoji_id;
    bool animated;
    Utf8View url, text;
};
// Labels are host copies: popups borrow them, never SDK binding storage.
struct ApplicationPluginMenuRow {
    u64 owner_id, item_id;
    u32 target, flags, label_bytes;
    char label[kPluginSdkMenuLabelBytes];
};
// One captured target. Activation tokens are (owner, item, menu_revision).
struct ApplicationPluginMenu {
    ApplicationPluginMenuRow rows[kApplicationPluginMenuRows];
    u64 user_id, channel_id, message_id, guild_id, emoji_id, recipient_id;
    u64 popup_owner_id, menu_revision, account_scope, account_serial;
    u32 attachment_index, flags, popup_opens, target_revision;
    u16 popup_generation, target_generation, url_bytes, text_bytes;
    ApplicationPluginMenuSurface surface;
    u8 popup_owner; // PopupOwnerKind
    u8 count;
    bool bound;
    char url[kPluginSdkUrlBytes];
    char text[kApplicationPluginMenuTextBytes];
};
// Projection copies rows and target identity; no plugin code runs. id is the
// user, message, channel or guild; guild_id is a user menu's server. Dispatch
// dismisses the borrowing popup and revalidates the copy before plugin entry.
u32 application_plugins_menu_items(ApplicationPluginMenuSurface, u64 id, Slice<PopupItem>, u64 guild_id = 0,
                                   const ApplicationPluginMenuExtra* extra = nullptr) noexcept;
// The current bank (tests inspect it); cleared with every borrowing popup.
const ApplicationPluginMenu& application_plugins_menu_state() noexcept;
bool application_plugins_menu_bind() noexcept;
void application_plugins_menu_dismissed() noexcept;
// Called before account teardown/start and accepted fresh READY. Never allocates.
void application_plugins_account_scope_invalidated() noexcept;
// SDK event feed: one popped Gateway event, before the stores apply it (so a
// deleted message can still be read). Cheap no-op without a subscriber.
void application_plugins_gateway(const GatewayClientEvent&, ConstSlice<u8> payload) noexcept;
// Maps one event to SDK events for the kinds in interest; returns how many
// emit accepted. Views passed to emit borrow payload or stores until return.
using PluginGatewayEmit = bool (*)(void* context, const NativePluginSdkEvent&) noexcept;
u32 application_plugins_gateway_translate(const GatewayClientEvent&, ConstSlice<u8> payload, u64 interest,
                                          void* context, PluginGatewayEmit emit) noexcept;
void application_plugins_detach_owner(u64 owner_id) noexcept;
bool application_plugins_menu_action(u32 action) noexcept;
bool application_plugins_menu_action_id(u32 action) noexcept;
bool application_plugins_choose() noexcept;
bool application_plugins_install() noexcept;
// One call when the authenticated shell replaces the restoration view. Fresh
// manifests and the pinned declared loader revalidate remembered user choices.
bool application_plugins_restore() noexcept;
bool application_plugins_retry_save() noexcept;
bool application_plugins_save_pending() noexcept;
inline constexpr u32 kApplicationPluginCatalogWindow = 8;
inline constexpr u64 kApplicationPluginDiscoverAction = 2;
inline constexpr u64 kApplicationPluginPreviousAction = 3;
inline constexpr u64 kApplicationPluginNextAction = 4;
inline constexpr u64 kApplicationPluginForgetAction = 5;
inline constexpr u64 kApplicationPluginInstallAction = 6;
inline constexpr u64 kApplicationPluginCatalogActionFirst = 200;
struct ApplicationPluginCatalogText { char text[160]; u32 bytes; };
// Presentation identity captured when this exact module generation was admitted.
// A null identity means active code was not admitted through a declared bundle.
struct ApplicationPluginCatalogLoaded {
    u64 owner_id;
    const NativePluginPreference* identity;
};
// Pure bounded projection shared with native GPU probes. Only visible text is
// composed; names and full metadata stay borrowed from caller-owned inventory.
// preferences, when given, marks entries that stay off after a crash or a time
// budget overrun; schema-2 entries list their permissions.
u32 application_plugins_catalog_project(const NativePluginCatalog*, u32 first,
    bool discovered, bool busy, ConstSlice<ApplicationPluginCatalogLoaded> loaded,
    Slice<UiSettingsControl>, Slice<ApplicationPluginCatalogText>, u32 remembered = 0,
    const NativePluginPreferences* preferences = nullptr) noexcept;
u32 application_plugins_settings_controls(Slice<UiSettingsControl>, Slice<ApplicationPluginCatalogText>, NativePluginError) noexcept;
bool application_plugins_discover() noexcept;
bool application_plugins_catalog_page(bool previous, u64 expected_revision) noexcept;
bool application_plugins_catalog_enable(u32 index, u64 expected_revision) noexcept;
bool application_plugins_catalog_forget(u64 expected_revision) noexcept;
// Catalog operations (application_plugin_browser.cpp). Starts the installer
// handshake for a staged manifest, or an uninstall of uninstall_owner, from
// Settings > Plugins.
bool application_plugins_install_prepared(ConstSlice<wchar_t> manifest, NativePluginBundleInstallOptions options, u64 uninstall_owner,
    ConstSlice<wchar_t> directory) noexcept;
// Re-reads plugins.bin after the plugin manager changed it.
bool application_plugins_reload_trust() noexcept;
// Enables the inventory row of owner (moving the window to it), prompting
// only when its permissions were not approved before.
bool application_plugins_enable_owner(u64 owner) noexcept;
u64 application_plugins_catalog_revision() noexcept;
const NativePluginCatalog* application_plugins_catalog_current() noexcept;
u32 application_plugins_catalog_owner_bytes() noexcept;
// Drop native Settings/choice borrowing before inventory/text is overwritten or freed.
void application_settings_plugin_catalog_unbind() noexcept;
bool application_settings_plugin_catalog_reopen() noexcept;
HANDLE application_plugins_notification() noexcept;
void application_plugins_poll() noexcept;
// ABI-5 integration points. The composer hook runs local commands and the
// pre-send transforms; *handled means the composer text must not be sent.
bool application_plugins_compose(bool* handled) noexcept;
bool application_plugins_command_pending() noexcept;
u32 application_plugins_commands(Utf8View prefix, Slice<NativePluginSdkCommandView>) noexcept;
Utf8View application_plugins_composer_notice() noexcept;
// MC_TONE_* (neutral..critical) of the plugin composer notice.
u8 application_plugins_composer_notice_tone() noexcept;
// Plugin keybinds (application_plugins_actions.cpp). Key-downs after the custom
// keybinds; never while a dialog, menu or other overlay is open or the window
// is in the background. True when a plugin handled the chord.
bool application_plugins_keybind_input(UiInputKind, u32 code, bool repeated) noexcept;
// Re-runs plugin chord conflict checks when custom keybinds or push to talk changed.
void application_plugins_keybinds_synchronize() noexcept;
u32 application_plugins_wait_timeout(u64 now_ms) noexcept;
// Opens the text form for an ABI-5 text, colour or keybind control.
bool application_plugins_settings_edit(UiPageKey page, u64 control_id) noexcept;
u64 application_plugins_slot_revision() noexcept;
// Message text embeds plugin badges; a slot change makes it stale.
bool application_plugins_slots_stale() noexcept;
void application_plugins_slots_rendered() noexcept;
bool application_plugins_slot_action(u64 owner_id, NativePluginSdkSlotTarget, u64 action_id) noexcept;
// Shows the popover the last slot or accessory action produced, anchored to
// anchor (application_plugins_surfaces.cpp). False when there is none.
bool application_plugins_popover_open(UiRect anchor) noexcept;
// Message render decorators (application_plugins_render.cpp). prepare runs
// the bounded decorator pass before message text is rebuilt; stale/rendered
// track the published decoration revision; poll refreshes text after plugin
// invalidations; click routes the Show link and accessory semantics.
void application_plugins_render_prepare() noexcept;
bool application_plugins_render_stale() noexcept;
void application_plugins_render_rendered() noexcept;
void application_plugins_render_poll() noexcept;
bool application_plugins_render_click(const MessageTextHit& hit, UiRect anchor) noexcept;
// ---- Slot surfaces (application_plugins_surfaces.cpp) ----
// Runs a slot button, then shows the popover it produced at anchor. False when
// the action did not run (busy, gone, not a button).
bool application_plugins_slot_activate(u64 owner_id, NativePluginSdkSlotTarget, u64 action_id, UiRect anchor) noexcept;
// Popover rows use their own action range under PopupOwnerKind::plugin_popover.
inline constexpr u32 kApplicationPluginPopoverActionFirst = 0x504F0000u;
bool application_plugins_popover_action(u32 action) noexcept;
bool application_plugins_popover_activate(u32 action) noexcept;
// Closes a popover whose owner unloaded, whose rows changed or whose account
// scope ended (called from the plugin poll).
void application_plugins_popover_poll() noexcept;
// Slot revision changed: re-layout surfaces that cache plugin items.
void application_plugins_surfaces_changed() noexcept;
// ---- Background effects (application_plugins_fx.cpp) ----
// Before each frame: hands the saved plugin effect (when registered) to the
// renderer, or falls back to the built-in; applies GPU guard verdicts.
void application_plugins_fx_synchronize() noexcept;
// Background effect picker: appends registered plugin effects (by name) after
// the built-ins and selects the saved one. False leaves the control as is.
bool application_plugins_fx_project(UiSettingsControl*) noexcept;
// A plugin effect choice is saved here (true, *result = saved); a built-in
// choice clears the plugin choice and returns false for the caller to write.
bool application_plugins_fx_activate(u32 choice, bool* result) noexcept;
// Channel/DM row badges, server rail badges and account panel buttons.
struct ApplicationPluginSurfaceHit {
    NativePluginSdkSlotTarget target;
    u64 owner_id, action_id;
    UiRect anchor;
    Utf8View label;
    const UiWidgetManager* manager;
    UiWidgetHandle owner;
    u32 index;
};
bool application_plugins_surface_hit(float x, float y, ApplicationPluginSurfaceHit*) noexcept;
bool application_plugins_surface_click(float x, float y) noexcept;
// Account panel hover fill; true while over a plugin button.
bool application_plugins_surface_hover(float x, float y) noexcept;
// Message hover bar plugin button index (0..kUiPluginSlotHoverButtons) for message.
bool application_plugins_hover_activate(u32 index, u64 message_id, UiRect anchor) noexcept;
Utf8View application_plugins_hover_label(u32 index) noexcept;
// Popover rows as popup items: buttons and links enabled, labels inert,
// separators fold into the next row. Labels borrow nodes.
inline u32 application_plugins_popover_items(const NativePluginSdkPopoverNode* nodes, u32 count,
                                             Slice<PopupItem> output) noexcept {
    u32 used = 0;
    bool separator = false;
    for (u32 i = 0; nodes && i < count && used < output.count; ++i) {
        const auto& node = nodes[i];
        if (node.kind == MC_NODE_SEPARATOR) {
            separator = used != 0;
            continue;
        }
        const Utf8View label{node.text, minimum(node.text_bytes, static_cast<u32>(sizeof(node.text)))};
        if (!label.bytes) continue;
        const bool enabled = (node.kind == MC_NODE_BUTTON && node.action_id) || (node.kind == MC_NODE_LINK && node.value_bytes);
        u16 flags = enabled ? popup_item_enabled : popup_item_none;
        if (node.tone == MC_TONE_CRITICAL) flags = static_cast<u16>(flags | popup_item_danger);
        if (separator) flags = static_cast<u16>(flags | popup_item_separator_before);
        separator = false;
        PopupItem item{};
        item.label = label;
        item.action = kApplicationPluginPopoverActionFirst + i;
        item.flags = flags;
        if (node.icon < static_cast<u16>(UiIcon::count))
            item.presentation = ui_menu_row_presentation(static_cast<UiIcon>(node.icon));
        else if (node.kind == MC_NODE_LINK)
            item.presentation = ui_menu_row_presentation(UiIcon::link);
        output[used++] = item;
    }
    return used;
}
bool application_plugins_unload(u32 slot) noexcept;
bool application_plugins_release() noexcept;
bool application_plugins_busy() noexcept;
bool application_plugins_present(u32 slot) noexcept;
Utf8View application_plugins_label(u32 slot) noexcept;
Utf8View application_plugins_status() noexcept;
namespace application_plugin_catalog_detail {
inline bool identity_matches(const NativePluginPreference& identity, const NativePluginManifest& manifest) noexcept {
    if (identity.owner_id != manifest.owner_id || identity.abi_version != manifest.abi_version ||
        identity.dll_bytes != manifest.dll_bytes || identity.dll_bytes > kNativePluginDllNameCapacity) return false;
    for (u32 i = 0; i < 32; ++i) if (identity.sha256[i] != manifest.sha256[i]) return false;
    for (u32 i = 0; i < identity.dll_bytes; ++i) {
        const char c = manifest.dll[i];
        if (identity.dll[i] != (c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c)) return false;
    }
    return true;
}
inline void append_text(ApplicationPluginCatalogText& output, Utf8View value) noexcept {
    if (output.bytes >= sizeof(output.text) - 3) return;
    for (u32 i = 0; i < value.bytes;) {
        const auto decoded = utf8_decode_one(value.data + i, value.bytes - i);
        if (!decoded.valid) return;
        const u32 width = decoded.width;
        if (width > value.bytes - i || width > sizeof(output.text) - 3 - output.bytes) {
            if (output.bytes <= sizeof(output.text) - 3) {
                output.text[output.bytes++] = '.'; output.text[output.bytes++] = '.'; output.text[output.bytes++] = '.';
            }
            return;
        }
        for (u32 j = 0; j < width; ++j) output.text[output.bytes++] = value.data[i++];
    }
}
// CP_UTF8 and WC_ERR_INVALID_CHARS (checked in application_plugins.cpp).
inline constexpr UINT kApplicationPluginCodePageUtf8 = 65001;
inline constexpr DWORD kApplicationPluginInvalidCharsError = 0x80;
inline void filename_text(ApplicationPluginCatalogText& output, const NativePluginCatalogEntry& entry) noexcept {
    output = {};
    for (u32 i = 0; i < entry.filename_characters && i < kNativePluginCatalogFilenameCapacity;) {
        const u32 width = entry.filename[i] >= 0xD800 && entry.filename[i] <= 0xDBFF &&
            i + 1 < entry.filename_characters && i + 1 < kNativePluginCatalogFilenameCapacity ? 2 : 1;
        char encoded[4];
        const int bytes = WideCharToMultiByte(kApplicationPluginCodePageUtf8, kApplicationPluginInvalidCharsError,
            entry.filename + i, static_cast<int>(width), encoded, static_cast<int>(sizeof(encoded)), nullptr,
            nullptr);
        const u32 before = output.bytes;
        append_text(output, bytes > 0 ? Utf8View{encoded, static_cast<u32>(bytes)} : utf8_literal("?"));
        if (output.bytes == sizeof(output.text) || output.bytes == before) break;
        i += width;
        if (output.bytes >= sizeof(output.text) - 3) break;
    }
    if (!output.bytes) append_text(output, utf8_literal("Unreadable plugin"));
}
inline void permissions_text(ApplicationPluginCatalogText& output, u64 permissions) noexcept {
    if (!permissions) return;
    append_text(output, utf8_literal("Permissions: "));
    bool first = true;
    for (u32 bit = 0; bit < kNativePluginPermissionCount; ++bit)
        if (permissions & (1ull << bit)) {
            if (!first) append_text(output, utf8_literal(", "));
            first = false;
            append_text(output, native_plugin_permission_name(bit));
        }
    append_text(output, utf8_literal(". "));
}
inline Utf8View entry_error(NativePluginCatalogError error) noexcept {
    if (error == NativePluginCatalogError::duplicate_owner || error == NativePluginCatalogError::duplicate_dll)
        return utf8_literal("Another plugin uses the same ID or file.");
    return utf8_literal("Couldn't read this plugin's manifest.");
}
}
inline u32 application_plugins_catalog_project(const NativePluginCatalog* catalog, u32 first,
    bool discovered, bool busy, ConstSlice<ApplicationPluginCatalogLoaded> loaded,
    Slice<UiSettingsControl> controls, Slice<ApplicationPluginCatalogText> text, u32 remembered,
    const NativePluginPreferences* preferences) noexcept {
    using namespace application_plugin_catalog_detail;
    if (!controls.data || controls.count < 11 || !text.data || text.count < 8 ||
        loaded.count > 8 || (loaded.count && !loaded.data) || (catalog &&
         (catalog->count > 32 || catalog->count > catalog->entries.count || (catalog->count && !catalog->entries.data)))) return 0;
    const u32 total = catalog ? catalog->count : 0;
    if ((first % 8) || (total ? first >= total : first != 0)) return 0;
    controls[0] = {kApplicationPluginDiscoverAction, utf8_literal("Local plugins"),
        utf8_literal("Installed plugins"),
        UiSettingsControlKind::action, !busy, false, {}, 0, discovered ? utf8_literal("Refresh") : utf8_literal("Discover")};
    controls[1] = {kApplicationPluginPreviousAction, utf8_literal("Previous plugins"), {},
        UiSettingsControlKind::action, !busy && first != 0, false, {}, 0, utf8_literal("Previous")};
    if (!first && remembered) controls[1] = {kApplicationPluginForgetAction, utf8_literal("Saved plugin choices"),
        utf8_literal("Stop loading saved plugins at startup."),
        UiSettingsControlKind::action, !busy, false, {}, 0, utf8_literal("Forget all")};
    controls[2] = {kApplicationPluginNextAction, utf8_literal("More plugins"), {},
        UiSettingsControlKind::action, !busy && total - first > 8, false, {}, 0, utf8_literal("Next")};
    u32 count = 3;
    for (u32 i = 0; i < 8 && i < total - first; ++i) {
        const auto& entry = catalog->entries[first + i];
        bool active = false, exact = false, declared = false;
        for (u32 j = 0; j < loaded.count; ++j) if (loaded[j].owner_id && loaded[j].owner_id == entry.manifest.owner_id) {
            active = true;
            declared = loaded[j].identity != nullptr;
            exact = declared && identity_matches(*loaded[j].identity, entry.manifest);
            break;
        }
        auto& label = text[i]; label = {};
        const bool valid = entry.status == NativePluginCatalogError::none && native_plugin_manifest_valid(entry.manifest);
        Utf8View name{}, description{};
        if (valid) {
            name = {entry.manifest.name, entry.manifest.name_bytes};
            append_text(label, utf8_literal("Version "));
            append_text(label, {entry.manifest.version, entry.manifest.version_bytes});
            append_text(label, !active ? utf8_literal(" | Ready. ") : exact ? utf8_literal(" | Loaded. ") :
                declared ? utf8_literal(" · Another version is running. Unload it first.") :
                utf8_literal(" · A manually loaded plugin is running. Unload it first."));
            const auto* remembered_entry = preferences && !active ?
                native_plugin_preferences_find(*preferences, entry.manifest) : nullptr;
            if (remembered_entry && (remembered_entry->flags & native_plugin_preference_crashed))
                append_text(label, utf8_literal("Turned off because MaiCord closed while it was running. "));
            else if (remembered_entry && (remembered_entry->flags & native_plugin_preference_overran))
                append_text(label, utf8_literal("Turned off because it kept MaiCord waiting. "));
            if (!active || exact) {
                permissions_text(label, entry.manifest.permissions);
                append_text(label, {entry.manifest.description, entry.manifest.description_bytes});
            }
            description = {label.text, label.bytes};
        } else {
            filename_text(label, entry); name = {label.text, label.bytes}; description = entry_error(entry.status);
        }
        controls[count++] = {kApplicationPluginCatalogActionFirst + first + i, name, description,
            UiSettingsControlKind::action, valid && !busy && !active, false, {}, 0,
            !valid ? utf8_literal("Unavailable") : !active ? utf8_literal("Enable") : exact ? utf8_literal("Loaded") : utf8_literal("In use")};
    }
    return count;
}
}
