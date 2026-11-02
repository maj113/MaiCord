#pragma once

#include "maicord/app/plugins/binding.hpp"
#include "maicord/app/plugins/surfaces.hpp"

namespace maicord {
struct UiPluginSlotNode;
// An icon pack name ("lucide/star") as a UiIcon value, or plugin_surfaces_detail::kNoIcon (icons.cpp).
u16 plugin_icon(Utf8View name) noexcept;
} // namespace maicord
// Shared by decorations.cpp (the per-plugin verdict caches, hooks and UI queries) and buttons.cpp (descriptor
// buttons). UI thread only.
namespace maicord::plugin_surfaces_detail {

// The decorate hook kinds, in the order of MC_HOOK_DECORATE_MESSAGE .. MC_HOOK_DECORATE_HEADER.
inline constexpr u32 kKindMessage = 0, kKindUser = 1, kKindChannel = 2, kKindGuild = 3, kKindHeader = 4;
inline constexpr u32 kKinds = 5;

// One cached verdict. key and scope name the entity: message (id, 0); user (id, MC_USER_* surface, or 0 for a
// push that covers every surface); channel and guild (id, 0); header (MC_HEADER_* or 0 for a push, 0).
struct Entry {
    u64 key, scope;
    u32 revision, generation; // pulled verdicts: the entity revision and the plugin's generation they answer
    u32 offset, lap;          // the record in the ring arena; bytes == 0: the plugin decorates nothing
    u16 bytes;
    u8 state; // 0 empty, 1 pulled, 2 pushed
    u8 reserved;
};

// Entries plus a ring of records: a new record goes at head, wrapping to the start (and counting a lap) when it
// would not fit; an entry whose record the head has passed is gone.
struct Cache {
    Entry* entries;
    u8* arena;
    u32 mask, arena_bytes, head, lap;
};

inline constexpr u32 kPopoverMemory = 8;
struct ButtonState {
    u32 flags, badge;
    u16 icon, declared; // the icon shown (the state's override or the declared one) and the declared one
    u8 tooltip_bytes;
    char tooltip[64];
};
inline constexpr u16 kNoIcon = 0xffff;

// What the message hover bar's visible predicates answered for one message revision.
struct VisibleMemory {
    u64 message_id;
    u32 revision, bits; // bit i: descriptor button i is shown
};

// One enabled plugin that decorates or has buttons. Stable address until removed.
struct Plugin {
    PluginBinding* binding;
    u8* memory;                          // the caches; committed on the first verdict
    Cache caches[kKinds];
    const struct mc_hook* hooks[kKinds]; // the plugin's decorate hooks
    const struct mc_hook* action;        // its NODE_ACTION hook
    u32 generation[kKinds];              // bumped by invalidate(kind, 0): every pulled verdict is stale
    u32 pushed;                          // bit per kind: some entity has a pushed verdict
    u32 button_count;
    ButtonState buttons[16];
    u32 shown;  // bit i: button i passed its visible predicate (message-independent buttons)
    u32 visible_next;
    VisibleMemory visible[kPopoverMemory];
};
inline constexpr u32 kMaximumPlugins = kPluginMaximumBindings;

extern Plugin g_plugins[kMaximumPlugins];
extern u32 g_plugin_count;

Plugin* find_plugin(const PluginBinding*) noexcept;
// The plugin's record, created (and the UI queries installed) on first use. Null when full.
Plugin* ensure_plugin(PluginBinding*) noexcept;
// Recomputes wants, the slot-surface mask and installs or removes the host table and the UI queries.
void republish() noexcept;
// A cached verdict or a button's state changed: the UI rebuilds. message: it was a message verdict.
void changed(bool message) noexcept;

// buttons.cpp
void buttons_add(Plugin&) noexcept;
u32 button_slots(u32 surface, u64 message_id, Slice<UiPluginSlotNode> output) noexcept;
bool buttons_popover(PluginDecoratePass*, const PluginMessageInput&) noexcept;
bool buttons_activate(u64 owner_id, const PluginSurfaceTarget&, u64 action_id, const PluginMessageInput* message) noexcept;
u32 buttons_overflow(u64 message_id, u32 bar, Slice<PluginButtonRef> output) noexcept;
// Re-runs the visible predicates that don't depend on a message; the channel and guild are remembered for
// predicates that run later (invalidate(MC_INVALIDATE_BUTTONS)).
void buttons_refresh(u64 channel_id, u64 guild_id) noexcept;
void buttons_refresh_remembered() noexcept;
// mc_api.button_state
mc_status buttons_set_state(PluginBinding*, u32 button, const mc_button_state&) noexcept;
// Bit (1 << UiPluginSlot*) of the surfaces some enabled plugin declares a button for.
u32 button_surfaces() noexcept;
// Called from api.cpp (decorations.cpp): mc_api.decorate_push and mc_api.invalidate.
mc_status plugin_surfaces_push(PluginBinding*, u32 hook_kind, u64 id, u64 scope, const mc_decoration*) noexcept;
mc_status plugin_surfaces_invalidate(PluginBinding*, u32 what, u64 id) noexcept;
// decorations.cpp helpers the buttons use.
bool text_valid(mc_str text, u32 maximum) noexcept;
void fill_message(const PluginMessageInput&, mc_message*) noexcept;
// reads.cpp: mc_api.read_*, from g_plugin_reader.
mc_status read_user(u64 user_id, u64 guild_id, mc_user* out) noexcept;
mc_status read_channel(u64 channel_id, mc_channel* out) noexcept;
mc_status read_guild(u64 guild_id, mc_guild* out) noexcept;
mc_status read_permissions(u64 guild_id, u64 channel_id, u64 user_id, u64* bits) noexcept;
mc_status read_voice_channel(u64 user_id, u64 guild_id, u64* channel) noexcept;
mc_status read_shared_group_dms(u64 user_id, u64* out, u32 capacity, u32* count) noexcept;
// The id a descriptor button's slot node carries as action_id: bit 63 set, the button index below.
inline constexpr u64 kButtonAction = 1ull << 63;

// Runs one timed call and returns its microseconds. Shared by both files.
u64 elapsed_us(i64 begin) noexcept;

} // namespace maicord::plugin_surfaces_detail
