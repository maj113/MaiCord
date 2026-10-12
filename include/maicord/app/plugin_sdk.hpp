#pragma once
#include "maicord/app/plugin_host.hpp"
#include "maicord/app/plugin_call_ledger.hpp"
#include "maicord/sdk/maicord_plugin.h"

namespace maicord {

// Host limits for ABI 5 (the SDK documents these as minimums).
inline constexpr u32 kPluginSdkPages = 4;
inline constexpr u32 kPluginSdkControls = 32;
inline constexpr u32 kPluginSdkChoices = 16;
inline constexpr u32 kPluginSdkSliderSteps = 101;
inline constexpr u32 kPluginSdkMenus = 16;
inline constexpr u32 kPluginSdkMenuLabelBytes = 64;
inline constexpr u32 kPluginSdkKeybinds = 8;
inline constexpr u32 kPluginSdkCommands = 8;
inline constexpr u32 kPluginSdkCommandOptions = 4;
inline constexpr u32 kPluginSdkSlotEntries = 256;
inline constexpr u32 kPluginSdkSlotNodes = 4;
inline constexpr u32 kPluginSdkPopoverNodes = 8;
inline constexpr u32 kPluginSdkStoreKeys = 32;
inline constexpr u32 kPluginSdkStoreValueBytes = 2048;
inline constexpr u32 kPluginSdkStoreBytes = 16 * 1024;
inline constexpr u32 kPluginSdkTimers = 4;
inline constexpr u32 kPluginSdkTimerMinimumMs = 250;
inline constexpr u32 kPluginSdkOutputBytes = 4000;
inline constexpr u32 kPluginSdkInputBytes = 16 * 1024;
inline constexpr u32 kPluginSdkStateBytes = 64 * 1024;
inline constexpr u32 kPluginSdkBindings = 16;
// Events: queued across plugins, delivered per plugin in bounded batches.
inline constexpr u32 kPluginSdkEventQueue = 512;
inline constexpr u32 kPluginSdkEventBatch = 32;
inline constexpr u32 kPluginSdkEventCallsPerPass = 4;
inline constexpr u32 kPluginSdkEventTextBytes = 256 * 1024;
inline constexpr u32 kPluginSdkEventContentBytes = 4000;
// Decorations: merged per message revision in a host cache.
inline constexpr u32 kPluginSdkRenderEntries = 512;
inline constexpr u32 kPluginSdkRenderArenaBytes = 192 * 1024;
inline constexpr u32 kPluginSdkRenderContentBytes = 2000;
inline constexpr u32 kPluginSdkRenderBadges = 2;      // per plugin
inline constexpr u32 kPluginSdkRenderAccessories = 4; // per plugin
inline constexpr u32 kPluginSdkRenderMergedBadges = 4;
inline constexpr u32 kPluginSdkRenderMergedAccessories = 8;
inline constexpr u32 kPluginSdkRenderPerPass = 64;
// Background effects.
inline constexpr u32 kPluginSdkEffects = 2;
inline constexpr u32 kPluginSdkEffectBytecodeBytes = 64 * 1024;
inline constexpr u32 kPluginSdkEffectNameBytes = 48;
// Actions.
inline constexpr u32 kPluginSdkMessageBytes = 2000;
inline constexpr u32 kPluginSdkClipboardBytes = 16 * 1024;
inline constexpr u32 kPluginSdkUrlBytes = 2048;
inline constexpr u32 kPluginSdkNoticeBytes = 200;
inline constexpr u32 kPluginSdkModalNodes = 8;
// Adapter header in front of the plugin state inside the host slot's state block.
inline constexpr u32 kPluginSdkStateHeaderBytes = 64;
inline constexpr u32 kPluginSdkHostAbi = 5;
// The oldest plugin query ABI (maicord_plugin_query_v<abi>) this host loads;
// catalog versions outside [oldest, host] are not compatible.
inline constexpr u32 kPluginSdkOldestPluginAbi = 5;

struct NativePluginSdkBinding;

// ---- Host sources (the application fills these; tests install fakes) --------

// Read-only client state for mc.read. Each callback fills the complete struct
// (the SDK layer copies the caller's prefix) and returns false when the data
// isn't held. Called on the UI thread; never fetches.
struct NativePluginSdkReadSource {
    void* context;
    bool (*context_info)(void*, mc_context_info*) noexcept;
    bool (*guild)(void*, u64 guild, mc_guild_info*) noexcept;
    bool (*channel)(void*, u64 channel, mc_channel_info*) noexcept;
    bool (*role)(void*, u64 guild, u64 role, mc_role_info*) noexcept;
    bool (*member)(void*, u64 guild, u64 user, mc_member_info*) noexcept;
    bool (*user)(void*, u64 user, mc_user_info*) noexcept;
    bool (*relationship)(void*, u64 user, mc_relationship_info*) noexcept;
    bool (*profile)(void*, u64 user, mc_profile_info*) noexcept;
    bool (*presence)(void*, u64 user, u64 guild, mc_presence_info*) noexcept;
    bool (*activity)(void*, u64 user, u32 index, mc_activity_info*) noexcept;
    bool (*voice)(void*, u64 guild, u64 user, mc_voice_info*) noexcept;
    // Fills everything except content; *content borrows the text until return.
    bool (*message)(void*, u64 channel, u64 message, mc_message_info*, Utf8View* content) noexcept;
    bool (*attachment)(void*, u64 channel, u64 message, u32 index, mc_attachment_info*) noexcept;
    bool (*embed)(void*, u64 channel, u64 message, u32 index, mc_embed_info*) noexcept;
    bool (*reaction)(void*, u64 channel, u64 message, u32 index, mc_reaction_info*) noexcept;
    // Copies ids from cursor; returns false for an unknown scope. *next is 0 at the end.
    bool (*list)(void*, u32 list, u64 scope, u32 cursor, Slice<u64> ids, u32* count, u32* next) noexcept;
    void (*revisions)(void*, mc_read_revisions*) noexcept;
};

// Validated copies of action requests. Text borrows SDK storage until return.
struct NativePluginSdkSend {
    u64 channel_id, reply_to;
    u32 flags;
    Utf8View text;
};
struct NativePluginSdkActivity {
    u64 application_id, start_ms, end_ms;
    u32 type;
    Utf8View name, details, state;
};
struct NativePluginSdkModalNode {
    u32 kind, tone;
    Utf8View text, value;
};
struct NativePluginSdkModal {
    u64 owner_id, modal_id;
    Utf8View title;
    u32 node_count, input_max_bytes;
    NativePluginSdkModalNode nodes[kPluginSdkModalNodes];
};
// Host-executed actions. Each returns an MC_* status; MC_OK means queued.
struct NativePluginSdkActionSink {
    void* context;
    mc_status (*send_message)(void*, u64 owner, const NativePluginSdkSend&) noexcept;
    mc_status (*edit_message)(void*, u64 owner, u64 channel, u64 message, Utf8View text) noexcept;
    mc_status (*react)(void*, u64 owner, u64 channel, u64 message, u64 emoji_id, Utf8View name) noexcept;
    mc_status (*mark_read)(void*, u64 owner, u64 channel) noexcept;
    mc_status (*custom_status)(void*, u64 owner, Utf8View text, Utf8View emoji, u64 expires_ms) noexcept;
    // Null clears the owner's activity.
    mc_status (*activity)(void*, u64 owner, const NativePluginSdkActivity*) noexcept;
    mc_status (*open_channel)(void*, u64 owner, u64 channel, u64 message) noexcept;
    mc_status (*open_profile)(void*, u64 owner, u64 user, u64 guild) noexcept;
    mc_status (*open_page)(void*, UiPageKey page) noexcept;
    mc_status (*copy_text)(void*, u64 owner, Utf8View text) noexcept;
    mc_status (*open_url)(void*, u64 owner, Utf8View url) noexcept;
    mc_status (*notice)(void*, u64 owner, Utf8View text, u32 tone) noexcept;
    mc_status (*open_modal)(void*, const NativePluginSdkModal&) noexcept;
};

// Keybinds: modifier bits match the shortcut registry (ctrl 1, alt 2, shift 4).
struct NativePluginSdkChord {
    u8 key, modifiers;
};
// Application conflict check for a plugin chord. Returns false and a short
// label ("Toggle mute", "a system shortcut") when the chord can't be used.
using NativePluginSdkKeybindCheck = bool (*)(void* context, NativePluginSdkChord, Utf8View* conflict) noexcept;

// One per application. UI-thread only, zero-initialized, stable address.
// Bindings link themselves here; buffers back the copy-only text exchange.
struct NativePluginSdkEventRecord {
    u64 sequence, guild_id, channel_id, message_id, user_id, value, time_ms;
    u32 kind, flags;
    u32 text_offset, text_bytes, name_offset, name_bytes;
};
struct NativePluginSdkRenderArenaNode {
    u64 owner_id, action_id;
    u32 text_offset, tooltip_offset, value_offset;
    u16 text_bytes, tooltip_bytes, value_bytes, icon;
    u8 kind, tone, reserved[2];
};
struct NativePluginSdkRenderEntry {
    u64 message_id, channel_id, stamp;
    u32 revision, epoch;
    u32 prefix_offset, suffix_offset, label_offset, content_offset;
    u16 prefix_bytes, suffix_bytes, label_bytes, content_bytes;
    u8 visibility, media, name_tone, badge_count, accessory_count;
    bool used, replace_content, stale;
    NativePluginSdkRenderArenaNode badges[kPluginSdkRenderMergedBadges];
    NativePluginSdkRenderArenaNode accessories[kPluginSdkRenderMergedAccessories];
};
struct NativePluginSdkPopoverNode {
    u64 action_id;
    u16 icon;
    u8 kind, tone;
    u32 text_bytes, value_bytes;
    char text[256];
    char value[512];
};
struct NativePluginSdkRuntime {
    PluginCallLedger* ledger;
    NativePluginSdkReadSource read;
    NativePluginSdkActionSink actions;
    NativePluginSdkKeybindCheck keybind_check;
    void* keybind_context;
    u64 (*clock_ms)() noexcept;      // monotonic; null uses GetTickCount64
    u64 (*unix_ms)() noexcept;       // event timestamps; null leaves zero
    // Root for per-plugin data folders; empty keeps storage in memory only.
    wchar_t data_directory[512];
    u32 data_directory_characters;
    NativePluginSdkBinding* pending;
    u64 slot_revision, slot_stamp;
    u64 menu_revision, keybind_revision, effect_revision;
    u32 gesture_depth;
    // Events.
    u64 event_head, event_tail; // sequences [tail, head) are queued
    u32 event_text_head;
    NativePluginSdkEventRecord events[kPluginSdkEventQueue];
    char event_text[kPluginSdkEventTextBytes];
    // Decorations.
    u64 render_revision, render_stamp;
    u32 render_epoch, render_arena_used;
    NativePluginSdkRenderEntry render[kPluginSdkRenderEntries];
    char render_arena[kPluginSdkRenderArenaBytes];
    // The popover the last slot action produced (application shows it).
    u64 popover_owner, popover_revision;
    mc_slot_event popover_anchor;
    bool popover_from_render; // rows run the accessory handler instead of the slot handler
    u32 popover_count;
    NativePluginSdkPopoverNode popover[kPluginSdkPopoverNodes];
    char input[kPluginSdkInputBytes];
    char output[2][kPluginSdkOutputBytes];
};

// ---- Loader ---------------------------------------------------------------

// One fixed-size host allocation per plugin. approved is the user-approved
// permission set; the definition may only request a subset of it.
NativePluginSdkBinding* native_plugin_sdk_create(NativePluginSdkRuntime*, u64 approved) noexcept;
bool native_plugin_sdk_destroy(NativePluginSdkBinding*) noexcept;
enum class NativePluginSdkQueryError : u8 { none, query_failed, abi, owner, permissions, invalid, budget };
// Runs the plugin's query and builds the slot definition the plugin host enables.
NativePluginSdkQueryError native_plugin_sdk_query(NativePluginSdkBinding*, mc_query_fn, u64 expected_owner,
                                                NativePluginDefinition* adapter) noexcept;
NativePluginResult native_plugin_sdk_enable(NativePluginSdkBinding*, NativePluginHost*,
                                           const NativePluginDefinition& adapter, Slice<u8> state) noexcept;
bool native_plugin_sdk_adapter(const NativePluginDefinition&) noexcept;
u64 native_plugin_sdk_owner(const NativePluginSdkBinding*) noexcept;
u64 native_plugin_sdk_permissions(const NativePluginSdkBinding*) noexcept;
// Enabled binding for owner, or null.
const NativePluginSdkBinding* native_plugin_sdk_find(const NativePluginSdkRuntime&, u64 owner_id) noexcept;

// ---- Settings (host page format STG4) ----------------------------------------

struct NativePluginSdkEdit {
    u32 kind; // MC_SETTING_TEXT, MC_SETTING_COLOUR or MC_SETTING_KEYBIND
    u32 max_bytes;
    Utf8View label, value;
};
// Text, colour and keybind controls are edited outside the settings scene.
bool native_plugin_sdk_settings_edit(const NativePluginSdkRuntime&, UiPageKey page, u64 control_id,
                                    NativePluginSdkEdit* edit) noexcept;
// Validates and normalizes the text, then calls the plugin's setting handler.
NativePluginError native_plugin_sdk_settings_submit(NativePluginSdkRuntime*, UiPageKey page, u64 control_id,
                                                   Utf8View text) noexcept;
// "Ctrl+Shift+K" normal form; false for an invalid chord.
bool native_plugin_sdk_keybind_normalize(Utf8View text, char (&output)[64], u32* bytes) noexcept;
// Normal form to a virtual key and modifiers; false for Win chords and invalid text.
bool native_plugin_sdk_keybind_chord(Utf8View normal, NativePluginSdkChord* chord) noexcept;
bool native_plugin_sdk_colour_parse(Utf8View text, u32* colour) noexcept;

// ---- Keybinds ------------------------------------------------------------------

// Fires the plugin keybind registered for chord. text_focus: a text field or
// the composer has focus (chords without Ctrl/Alt then never fire). True when
// a plugin handled it.
bool native_plugin_sdk_keybind_press(NativePluginSdkRuntime*, NativePluginSdkChord, bool text_focus) noexcept;
struct NativePluginSdkKeybindView {
    u64 owner_id, page_id, control_id;
    NativePluginSdkChord chord;
    Utf8View label;
};
u32 native_plugin_sdk_keybinds(const NativePluginSdkRuntime&, Slice<NativePluginSdkKeybindView>) noexcept;
// Re-runs conflict checks (after custom keybinds changed).
void native_plugin_sdk_keybinds_recheck(NativePluginSdkRuntime*) noexcept;

// ---- Composer ---------------------------------------------------------------

enum class NativePluginSdkComposeAction : u8 { none, send, replace, notice, done, cancel };
// text borrows a runtime buffer until the next compose call.
struct NativePluginSdkComposeResult {
    NativePluginSdkComposeAction action;
    Utf8View text;
    u64 owner_id;
};
struct NativePluginSdkDraft {
    u64 channel_id, guild_id, author_id;
    Utf8View text;
};
struct NativePluginSdkCommandView {
    u64 owner_id, command_id;
    Utf8View name, description;
};
// Local commands whose name starts with prefix (no leading '/'), in plugin order.
u32 native_plugin_sdk_commands(const NativePluginSdkRuntime&, Utf8View prefix,
                              Slice<NativePluginSdkCommandView> output) noexcept;
// True when text is "/name" or "/name ..." for an enabled local command.
bool native_plugin_sdk_command_matches(const NativePluginSdkRuntime&, Utf8View text) noexcept;
// none when no local command matches; notice carries argument errors.
NativePluginSdkComposeResult native_plugin_sdk_run_command(NativePluginSdkRuntime*, const NativePluginSdkDraft&) noexcept;
// Chains every pre-send hook. none means unchanged.
NativePluginSdkComposeResult native_plugin_sdk_pre_send(NativePluginSdkRuntime*, const NativePluginSdkDraft&) noexcept;

// ---- Menus -----------------------------------------------------------------------

struct NativePluginSdkMenuItem {
    u64 owner_id, item_id;
    Utf8View label;
    u32 flags;
};
// Items registered for target, in plugin order. Views borrow binding storage
// until menu_revision changes.
u32 native_plugin_sdk_menu_items(const NativePluginSdkRuntime&, u32 target, Slice<NativePluginSdkMenuItem>) noexcept;
struct NativePluginSdkMenuContext {
    u32 target, attachment_index, flags;
    u64 user_id, channel_id, message_id, guild_id, emoji_id;
    Utf8View url, text;
};
// Runs the item's handler inside a gesture. False when the item is gone.
bool native_plugin_sdk_menu_activate(NativePluginSdkRuntime*, u64 owner_id, u64 item_id,
                                     const NativePluginSdkMenuContext&) noexcept;

// ---- Events ----------------------------------------------------------------------

struct NativePluginSdkEvent {
    u32 kind, flags;
    u64 guild_id, channel_id, message_id, user_id, value;
    Utf8View text, name;
};
// Union of every enabled plugin's subscription (MC_EVENT_MASK bits).
u64 native_plugin_sdk_event_interest(const NativePluginSdkRuntime&) noexcept;
// Copies the event into the queue when any plugin subscribed to its kind.
bool native_plugin_sdk_event_push(NativePluginSdkRuntime*, const NativePluginSdkEvent&) noexcept;
// Delivers queued events within the per-pass bounds. True when more remain.
bool native_plugin_sdk_event_deliver(NativePluginSdkRuntime*) noexcept;

// ---- Read (also used by tests) ---------------------------------------------------

// ---- Messages render decorators -----------------------------------------------------

struct NativePluginSdkRenderMessage {
    u64 channel_id, message_id, author_id, guild_id, timestamp_ms, edited_ms;
    u32 flags, revision, relationship, attachment_count, embed_count;
    Utf8View content, author_name;
};
// Views borrow the runtime cache until render_revision changes.
struct NativePluginSdkSlotNode {
    u64 owner_id, action_id;
    Utf8View text, tooltip, value;
    u16 icon; // UiIcon value, or kPluginSdkNoIcon
    u8 kind, tone;
};
struct NativePluginSdkDecoration {
    u8 visibility, media, name_tone;
    bool replace_content;
    Utf8View collapsed_label, name_prefix, name_suffix, content;
    u32 badge_count, accessory_count;
    NativePluginSdkSlotNode badges[kPluginSdkRenderMergedBadges];
    NativePluginSdkSlotNode accessories[kPluginSdkRenderMergedAccessories];
};
bool native_plugin_sdk_has_decorators(const NativePluginSdkRuntime&) noexcept;
// Current decoration for the message revision. With evaluate the decorators
// run when the cache is stale; without it a stale entry reports false.
// False also means "no decoration" (nothing to change).
bool native_plugin_sdk_decorate(NativePluginSdkRuntime*, const NativePluginSdkRenderMessage&, bool evaluate,
                                NativePluginSdkDecoration* output) noexcept;
// True when the message revision has a current cache entry (decorated or not).
bool native_plugin_sdk_decoration_current(const NativePluginSdkRuntime&, u64 message_id, u32 revision) noexcept;
// Accessory button on a decorated message.
bool native_plugin_sdk_render_action(NativePluginSdkRuntime*, u64 owner_id, u64 channel_id, u64 message_id,
                                     u64 action_id) noexcept;

// ---- Slots, popovers, timers ----------------------------------------------------------

struct NativePluginSdkSlotTarget {
    u32 surface;
    u64 channel_id, message_id, user_id, guild_id;
};
inline constexpr u16 kPluginSdkNoIcon = 0xffff;
u32 native_plugin_sdk_slots(const NativePluginSdkRuntime&, NativePluginSdkSlotTarget,
                           Slice<NativePluginSdkSlotNode> output) noexcept;
// For global surfaces target carries the click context (MESSAGE_HOVER: the
// message). A popover the handler produced is left in runtime->popover.
bool native_plugin_sdk_slot_action(NativePluginSdkRuntime*, u64 owner_id, NativePluginSdkSlotTarget,
                                  u64 action_id) noexcept;
// Activates popover row index of the current popover (runs the handler again).
bool native_plugin_sdk_popover_action(NativePluginSdkRuntime*, u64 popover_revision, u32 index) noexcept;
void native_plugin_sdk_popover_clear(NativePluginSdkRuntime*) noexcept;
// Icon pack name ("lucide/star") to UiIcon, or kPluginSdkNoIcon.
u16 native_plugin_sdk_icon(Utf8View name) noexcept;

// ---- Modals ------------------------------------------------------------------------------

// Delivers a modal result (a gesture). False when the plugin is gone.
bool native_plugin_sdk_modal_result(NativePluginSdkRuntime*, u64 owner_id, u64 modal_id, bool accepted,
                                    Utf8View text) noexcept;

// ---- Effects --------------------------------------------------------------------------------

struct NativePluginSdkEffectView {
    u64 owner_id, effect_id;
    Utf8View name;
    ConstSlice<u8> bytecode;
    const float* parameters; // kPluginSdk... MC_FX_PARAMETERS values
    u32 revision;            // changes when bytecode or parameters change
};
u32 native_plugin_sdk_effects(const NativePluginSdkRuntime&, Slice<NativePluginSdkEffectView>) noexcept;
bool native_plugin_sdk_effect(const NativePluginSdkRuntime&, u64 owner_id, u64 effect_id,
                              NativePluginSdkEffectView*) noexcept;
// GPU guard: unregisters the effect after a fault; the owner stays enabled.
bool native_plugin_sdk_effect_fault(NativePluginSdkRuntime*, u64 owner_id, u64 effect_id) noexcept;
// DXBC container holding a ps_5_0 program.
bool native_plugin_sdk_effect_bytecode_valid(ConstSlice<u8>) noexcept;

// ---- Poll ------------------------------------------------------------------------------------

struct NativePluginSdkPoll {
    u64 next_timer_ms; // clock time of the next timer, or zero
    bool events_pending;
};
// Fires due timers, delivers events and flushes dirty stores.
NativePluginSdkPoll native_plugin_sdk_poll(NativePluginSdkRuntime*) noexcept;
// Writes every dirty store now (shutdown/disable).
void native_plugin_sdk_flush(NativePluginSdkRuntime*) noexcept;

} // namespace maicord
