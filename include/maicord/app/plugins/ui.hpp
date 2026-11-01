#pragma once

#include "maicord/core/slice.hpp"

// What the application calls for the plugin surfaces that ask the user something or show
// them something: context menu items, slash commands, banners, modals and the navigation
// actions behind them. Free of the plugin ABI header so ordinary application code can
// include it; the registry behind it is built when a plugin is enabled (binding.hpp).
// UI thread only.
namespace maicord {

// ---- Context menus ------------------------------------------------------------------

enum class PluginMenuKind : u8 { user, message, channel, guild, link, image, emoji };
inline constexpr u32 kPluginMenuTargetOwn = 1;      // your message or yourself
inline constexpr u32 kPluginMenuTargetPrivate = 2;  // in a DM or group DM
inline constexpr u32 kPluginMenuTargetAnimated = 4; // an animated emoji

// What a menu was opened on. Ids that don't apply are zero; views are read during the call.
struct PluginMenuTarget {
    PluginMenuKind kind;
    u32 flags;
    u64 user_id, channel_id, guild_id, message_id;
    // emoji: the emoji id; image: the attachment id
    u64 item_id;
    // link, image: the URL; emoji: its image. text: link text, file name or emoji name.
    Utf8View url, text, content_type;
    u32 attachment_index, width, height;
};

inline constexpr u32 kPluginMenuLabelBytes = 64;
inline constexpr u32 kPluginMenuRowDanger = 1;
inline constexpr u32 kPluginMenuRowDisabled = 2;
inline constexpr u32 kPluginMenuRowChecked = 4;
inline constexpr u32 kPluginMenuRowSubmenu = 8;
// A row is a copy: the label stays valid however the plugin changes.
struct PluginMenuRow {
    u64 owner_id;
    u32 item; // index into the plugin's menus
    u32 flags;
    u32 label_bytes;
    char label[kPluginMenuLabelBytes];
};

// Whether an enabled plugin has an item for this kind of target; no plugin code runs.
bool plugin_menu_present(PluginMenuKind) noexcept;
// The visible top-level items for the target, plugin by plugin in enable order and in
// descriptor order within a plugin. Runs each item's visible function.
u32 plugin_menu_collect(const PluginMenuTarget&, Slice<PluginMenuRow> output) noexcept;
// The visible children of a submenu row.
u32 plugin_menu_children(const PluginMenuTarget&, u64 owner_id, u32 parent_item, Slice<PluginMenuRow> output) noexcept;
// gone: the plugin was disabled or the item is not one for this target (nothing ran).
enum class PluginMenuRun : u8 { gone, ran, failed };
// Runs a plugin's select function as a user gesture.
PluginMenuRun plugin_menu_activate(const PluginMenuTarget&, u64 owner_id, u32 item) noexcept;
// Changes whenever a plugin is enabled or disabled: captured rows are stale after it.
u32 plugin_ui_revision() noexcept;

// ---- Slash commands -------------------------------------------------------------------

// Plugin commands live in the composer's command index as applications whose ids have this
// bit set; Discord ids never do.
inline constexpr u64 kPluginCommandApplicationFlag = 1ull << 63;
constexpr bool plugin_command_application(u64 application_id) noexcept {
    return (application_id & kPluginCommandApplicationFlag) != 0;
}
// Enabled plugins' commands as an application command index (the JSON the composer decodes):
// one application per plugin, one command per declared command. Returns the bytes written, 0
// when there are none or output is too small.
u32 plugin_command_index_json(Slice<char> output) noexcept;
// Declared commands across enabled plugins.
u32 plugin_command_count() noexcept;

enum class PluginCommandOutcome : u8 {
    none,   // ran; nothing to do
    send,   // text is sent as an ordinary message
    insert, // text goes into the composer
    local,  // text is a local message
    toast,  // text is a toast
    failed, // the plugin failed or overran
    gone,   // no such command
};
struct PluginCommandResult {
    PluginCommandOutcome outcome;
    u32 tone; // MC_TONE_*
    u64 owner_id;
    // Valid until the next plugin_command_run.
    Utf8View text;
};
// Runs a command the composer collected options for: options_json is the array command_arguments
// wrote ([{"name","type","value"}...]); scratch is structural-index storage for parsing it.
PluginCommandResult plugin_command_run(u64 application_id, u64 command_id, u64 channel_id, u64 guild_id,
                                       Utf8View options_json, Slice<u32> scratch) noexcept;

// ---- Banners -------------------------------------------------------------------------

// What the banner card shows. Views are valid until the banner changes.
struct PluginBannerView {
    u64 owner_id, tag;
    u32 serial; // changes with every new banner
    bool warning;
    Utf8View title, text, action; // action empty: no button
};
// The banner to show: the earliest enabled plugin's. False when there is none.
bool plugin_banner_current(PluginBannerView*) noexcept;
// Changes when a banner appears or goes.
u32 plugin_banner_revision() noexcept;
// The user pressed the action (true) or dismissed the banner: tells the plugin and removes it.
bool plugin_banner_resolve(bool action) noexcept;

// ---- Modals --------------------------------------------------------------------------

enum class PluginModalKind : u8 {
    confirm,      // text and two buttons
    toggle,       // a confirmation with one checkbox row
    input,        // a text field
    input_select, // a text field and a dropdown
};
inline constexpr u32 kPluginModalSelectItems = 16;
// A copy of what the plugin asked for; views stay valid until the modal is resolved.
struct PluginModalRequest {
    PluginModalKind kind;
    bool danger, multiline;
    u64 owner_id;
    Utf8View title, context, accept, cancel;
    Utf8View input_label, input_initial, input_hint;
    u32 input_max_chars;
    Utf8View toggle_label;
    bool toggle_initial;
    Utf8View select_label;
    Utf8View select_items[kPluginModalSelectItems];
    u32 select_count, select_initial;
};
// The modal waiting for an answer, if any.
const PluginModalRequest* plugin_modal_current() noexcept;
// Answers it: tells the plugin (accepted, or cancelled) and frees the slot. input and select are
// ignored for kinds that don't have them.
bool plugin_modal_resolve(bool accepted, Utf8View input, bool toggle, u32 select) noexcept;

// ---- Host ------------------------------------------------------------------------------

enum class PluginUiStatus : u8 { ok, busy, not_found, denied };
// What the application provides to the plugin API; the application installs it once. A null
// function makes its API call fail with MC_E_UNSUPPORTED.
struct PluginUiHost {
    // Shows the modal. False: not now (another dialog is open); the plugin sees MC_E_BUSY.
    bool (*open_modal)(const PluginModalRequest&) noexcept;
    // A banner appeared, changed or went.
    void (*banner_changed)() noexcept;
    // The modal went away without an answer (its plugin was disabled).
    void (*close_modal)() noexcept;
    // Through the link trust rules and the leaving confirmation.
    PluginUiStatus (*open_url)(Utf8View url) noexcept;
    PluginUiStatus (*copy_text)(Utf8View text) noexcept;
    PluginUiStatus (*open_channel)(u64 channel_id) noexcept;
    PluginUiStatus (*open_profile)(u64 user_id, u64 guild_id) noexcept;
    // A local message only this user sees in channel_id (0: the open channel); false unless that is the open channel.
    bool (*local_notice)(u64 channel_id, Utf8View text, u64* message_id) noexcept;
    // A toast; tone is MC_TONE_*.
    void (*toast)(u64 owner_id, Utf8View text, u32 tone) noexcept;
};
void plugin_ui_host_install(const PluginUiHost*) noexcept;
// The installed host, or null.
const PluginUiHost* plugin_ui_host() noexcept;

} // namespace maicord
