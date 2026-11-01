#pragma once
#include "maicord/app/plugins/host.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/app/plugins/plugin_sdk.hpp"
#include "maicord/ui/shell/popup_scene.hpp"
#include "maicord/ui/account/friends_scene.hpp"
#include "maicord/app/plugins/bundle_install.hpp"
#include "maicord/app/plugins/catalog.hpp"
#include "maicord/app/plugins/preferences.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/utf.hpp"
namespace maicord { struct UiSettingsControl; }
#include "maicord/ui/core/input_router.hpp"
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
    // MC_MENU_ATTACHMENT only: an image, its attachment id, content type and size. Other
    // attachments offer plugin rows of the message alone.
    bool image = false;
    u64 attachment_id = 0;
    Utf8View content_type = {};
    u32 width = 0, height = 0;
};
// Labels are host copies: popups borrow them, never SDK binding storage. native rows belong to
// ABI-6 plugins (maicord/app/plugins/ui.hpp): item_id is the item's index, target a PluginMenuKind
// and flags kPluginMenuRow*.
struct ApplicationPluginMenuRow {
    u64 owner_id, item_id;
    u32 target, flags, label_bytes;
    char label[kPluginSdkMenuLabelBytes];
    bool native;
};
// One captured target. Activation tokens are (owner, item, menu_revision).
struct ApplicationPluginMenu {
    ApplicationPluginMenuRow rows[kApplicationPluginMenuRows];
    u64 user_id, channel_id, message_id, guild_id, emoji_id, recipient_id, attachment_id;
    u64 popup_owner_id, menu_revision, account_scope, account_serial;
    u32 attachment_index, flags, popup_opens, target_revision, native_revision, width, height;
    u16 popup_generation, target_generation, url_bytes, text_bytes, content_type_bytes;
    ApplicationPluginMenuSurface surface;
    u8 popup_owner; // PopupOwnerKind
    u8 count;
    bool bound;
    char url[kPluginSdkUrlBytes];
    char text[kApplicationPluginMenuTextBytes];
    char content_type[32];
};
// Projection copies rows and target identity; no plugin code runs. id is the
// user, message, channel or guild; guild_id is a user menu's server. Dispatch
// dismisses the borrowing popup and revalidates the copy before plugin entry.
u32 application_plugins_menu_items(ApplicationPluginMenuSurface, u64 id, Slice<PopupItem>, u64 guild_id = 0,
                                   const ApplicationPluginMenuExtra* extra = nullptr) noexcept;
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
// Catalog operations (browser.cpp). Starts the installer
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
// Plugin keybinds (actions.cpp). Key-downs after the custom
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
// anchor (surfaces.cpp). False when there is none.
bool application_plugins_popover_open(UiRect anchor) noexcept;
// Message render decorators (render.cpp). prepare runs
// the bounded decorator pass before message text is rebuilt; stale/rendered
// track the published decoration revision; poll refreshes text after plugin
// invalidations; click routes the Show link and accessory semantics.
void application_plugins_render_prepare() noexcept;
bool application_plugins_render_stale() noexcept;
void application_plugins_render_rendered() noexcept;
void application_plugins_render_poll() noexcept;
bool application_plugins_render_click(const MessageTextHit& hit, UiRect anchor) noexcept;
// ---- Slot surfaces (surfaces.cpp) ----
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
bool application_plugins_unload(u32 slot) noexcept;
bool application_plugins_release() noexcept;
bool application_plugins_busy() noexcept;
bool application_plugins_present(u32 slot) noexcept;
Utf8View application_plugins_label(u32 slot) noexcept;
Utf8View application_plugins_status() noexcept;
}
