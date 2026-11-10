#pragma once
#include "maicord/app/plugins/surfaces.hpp"
#include "maicord/app/plugins/ui.hpp"
#include "maicord/ui/account/friends_scene.hpp"
#include "maicord/ui/core/input_router.hpp"
#include "maicord/ui/shell/popup_scene.hpp"
// What the application calls for the plugin runtime (src/app/plugins). The runtime's own entry points
// (enable, disable, hooks) are in include/maicord/app/plugins; this file is the application's side.
namespace maicord::app_detail {
inline constexpr u32 kApplicationPluginPopupActionFirst = 0xF200;
// Plugin rows one native menu shows, across every plugin and target.
inline constexpr u32 kApplicationPluginMenuRows = 8;
inline constexpr u32 kApplicationPluginMenuTextBytes = 512;
inline constexpr u32 kApplicationPluginMenuUrlBytes = 2048;
inline constexpr u32 kApplicationPluginMenuContentTypeBytes = 32;
// The native menu plugin rows are appended to. It fixes the popup owner the
// binder expects: member, message, channel/private channel, guild, or the
// Friends row menu (user rows owned by the Friends popup).
enum class ApplicationPluginMenuSurface : u8 { none, user, message, channel, guild, friends };
// What a message menu was opened on: link, image or emoji rows come before the message rows. Views are copied.
struct ApplicationPluginMenuExtra {
    PluginMenuKind target; // link, image or emoji
    u32 attachment_index;
    u64 emoji_id;
    bool animated;
    Utf8View url, text;
    // image only: the attachment id, content type and size.
    u64 attachment_id = 0;
    Utf8View content_type = {};
    u32 width = 0, height = 0;
};
// Labels are host copies: popups borrow them, never plugin storage. item_id is the item's index, target a
// PluginMenuKind and flags kPluginMenuRow*.
struct ApplicationPluginMenuRow {
    u64 owner_id, item_id;
    u32 target, flags, label_bytes;
    char label[kPluginMenuLabelBytes];
};
// One captured target. Activation tokens are (owner, item, revision).
struct ApplicationPluginMenu {
    ApplicationPluginMenuRow rows[kApplicationPluginMenuRows];
    u64 user_id, channel_id, message_id, guild_id, emoji_id, recipient_id, attachment_id;
    u64 popup_owner_id, account_scope, account_serial;
    u32 attachment_index, flags, popup_opens, target_revision, revision, width, height;
    u16 popup_generation, target_generation, url_bytes, text_bytes, content_type_bytes;
    ApplicationPluginMenuSurface surface;
    u8 popup_owner; // PopupOwnerKind
    u8 count;
    bool bound;
    char url[kApplicationPluginMenuUrlBytes];
    char text[kApplicationPluginMenuTextBytes];
    char content_type[kApplicationPluginMenuContentTypeBytes];
};
// Projection copies rows and target identity. id is the user, message, channel or guild; guild_id is a user
// menu's server. Dispatch dismisses the borrowing popup and revalidates the copy before plugin entry.
u32 application_plugins_menu_items(ApplicationPluginMenuSurface, u64 id, Slice<PopupItem>, u64 guild_id = 0,
                                   const ApplicationPluginMenuExtra* extra = nullptr) noexcept;
bool application_plugins_menu_bind() noexcept;
void application_plugins_menu_dismissed() noexcept;
// Called before account teardown/start and accepted fresh READY. Never allocates.
void application_plugins_account_scope_invalidated() noexcept;
bool application_plugins_menu_action(u32 action) noexcept;
bool application_plugins_menu_action_id(u32 action) noexcept;
// The composer's hooks: pre-send and pre-edit rewrite, cancel or question the text before the ordinary
// send or save; *handled means the composer text must not be sent.
bool application_plugins_compose(bool* handled) noexcept;
// The notice a plugin that stopped a send or an edit leaves under the composer. The wait is the milliseconds
// until it expires (INFINITE: none); the poll clears it when its time is up.
Utf8View application_plugins_composer_notice() noexcept;
u32 application_plugins_composer_notice_wait(u64 now_ms) noexcept;
void application_plugins_composer_notice_poll(u64 now_ms) noexcept;
// Keys bound in plugin settings. Key-downs after the custom keybinds; never while a dialog, menu or other
// overlay is open or the window is in the background. True when a plugin handled the chord.
bool application_plugins_keybind_input(UiInputKind, u32 code, bool repeated) noexcept;

// ---- Slot surfaces (surfaces.cpp) ----
// Slot revision changed: re-layout surfaces that cache plugin items.
void application_plugins_surfaces_changed() noexcept;
// Channel/DM row badges, server rail badges and account panel buttons.
struct ApplicationPluginSurfaceHit {
    PluginSurfaceTarget target;
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
bool application_plugins_hover_activate(u32 index, u64 message_id) noexcept;
Utf8View application_plugins_hover_label(u32 index) noexcept;
// What the plugin host files (surface_host.cpp, pipeline_host.cpp, ui_host.cpp) read of the account alike: an
// account that is signed in with nothing required of the user, a channel's server (0 in a DM) and whether a user is
// a bot.
struct AccountUiRuntime;
bool plugin_account_ready() noexcept;
u64 plugin_guild_of_channel(const AccountUiRuntime& ui, u64 channel_id) noexcept;
bool plugin_user_is_bot(const AccountUiRuntime& ui, u64 user_id) noexcept;
}
