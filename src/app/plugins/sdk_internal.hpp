#pragma once
// Private to the plugin SDK host (plugin_sdk*.cpp): the binding layout and the
// helpers every interface shares. Nothing outside these files includes it.
#include "maicord/app/plugins/plugin_sdk.hpp"
#include "maicord/app/plugins/host.hpp"
#include <stddef.h>

namespace maicord::plugin_sdk {

constexpr u64 kBindingMagic = 0x3556'4e49'4c50'434dull;
constexpr u64 kHeaderMagic = 0x3556'5244'4850'434dull;
constexpr u32 kPageTextBytes = 12 * 1024;
constexpr u32 kPageViews = 1024;
constexpr u32 kLabelBytes = 128, kDescriptionBytes = 256, kActionBytes = 32, kChoiceBytes = 64;
constexpr u32 kTextMaximum = 1024;
constexpr u32 kCommandNameBytes = 32, kCommandDescriptionBytes = 100;
constexpr u32 kSlotTextBytes = 32, kSlotTooltipBytes = 64, kSlotValueBytes = 256;
constexpr u32 kNodeLabelBytes = 256, kNodeValueBytes = 512;
constexpr u32 kStoreKeyBytes = 64;
constexpr u32 kConflictBytes = 64;

enum class Phase : u8 { empty, queried, starting, enabled, stopping };

struct ControlExtra {
    u32 sdk_kind, max_bytes;
    i32 minimum, step;
    Utf8View value; // raw text/keybind or "#RRGGBB"
};
// Host page format STG4. The prefix through format is what
// native_plugin_settings_snapshot reads; everything else is adapter-private.
struct Page {
    Utf8View title, description;
    ConstSlice<NativePluginSettingsControl> controls;
    u32 revision, format;
    u64 page_id;
    bool registered;
    char label[kLabelBytes];
    u32 label_bytes, text_used, views_used;
    NativePluginSettingsControl bank[kPluginSdkControls];
    ControlExtra extra[kPluginSdkControls];
    Utf8View views[kPageViews];
    char text[kPageTextBytes];
};
static_assert(offsetof(Page, revision) == 48 && offsetof(Page, format) == 52);
struct Menu {
    u64 id;
    u32 target, flags, label_bytes;
    bool registered;
    char label[kPluginSdkMenuLabelBytes];
};
struct Keybind {
    u64 page_id, control_id;
    NativePluginSdkChord chord;
    bool active; // registered (no conflict)
    u32 label_bytes;
    char label[kLabelBytes];
};
struct CommandOption {
    u32 type, name_bytes, description_bytes;
    bool required;
    char name[kCommandNameBytes];
    char description[kCommandDescriptionBytes];
};
struct Command {
    u64 id;
    u32 name_bytes, description_bytes, option_count;
    char name[kCommandNameBytes];
    char description[kCommandDescriptionBytes];
    CommandOption options[kPluginSdkCommandOptions];
};
struct SlotNode {
    u64 action_id;
    u16 icon;
    u8 kind, tone, text_bytes, tooltip_bytes;
    u16 value_bytes;
    char text[kSlotTextBytes];
    char tooltip[kSlotTooltipBytes];
    char value[kSlotValueBytes];
};
struct SlotEntry {
    u64 channel_id, message_id, user_id, guild_id, stamp;
    u32 surface, node_count;
    SlotNode nodes[kPluginSdkSlotNodes];
};
struct StoreEntry {
    u16 offset, value_bytes;
    u8 key_bytes;
};
struct Store {
    bool loaded, dirty;
    u32 count, used;
    StoreEntry entries[kPluginSdkStoreKeys];
    u8 data[kPluginSdkStoreBytes];
};
struct Timer {
    u64 id, due, interval;
    bool repeat, active;
};
// Token buckets: capacity tokens refill over window_ms.
struct Bucket {
    u64 updated_ms;
    u32 tokens_milli;
};
enum BucketKind : u32 { bucket_messages, bucket_reactions, bucket_read, bucket_presence, bucket_notice, bucket_count };
struct StateHeader {
    u64 magic;
    NativePluginSdkBinding* binding;
};
} // namespace maicord::plugin_sdk

namespace maicord {
struct NativePluginSdkBinding {
    u64 magic;
    NativePluginSdkRuntime* runtime;
    mc_plugin_definition definition;
    u64 owner_id, approved, generation;
    NativePluginServices services;
    u8* plugin_state;
    u32 plugin_state_bytes;
    plugin_sdk::Phase phase;
    bool start_attempted;
    mc_setting_fn setting_handler;
    mc_keybind_fn keybind_handler;
    mc_menu_fn menu_handler;
    mc_pre_send_fn pre_send;
    mc_command_fn command_handler;
    mc_slot_action_fn slot_handler;
    mc_slot_action_fn render_action_handler;
    mc_render_fn decorator;
    mc_timer_fn timer_handler;
    mc_event_fn event_handler;
    mc_modal_fn modal_handler;
    u64 event_kinds, event_next;
    u64 modal_id; // open modal, or zero
    plugin_sdk::Page pages[kPluginSdkPages];
    plugin_sdk::Menu menus[kPluginSdkMenus];
    plugin_sdk::Keybind keybinds[kPluginSdkKeybinds];
    plugin_sdk::Command commands[kPluginSdkCommands];
    u32 command_count, slot_high_water;
    plugin_sdk::Timer timers[kPluginSdkTimers];
    plugin_sdk::Bucket buckets[plugin_sdk::bucket_count];
    plugin_sdk::Store store;
    plugin_sdk::SlotEntry slots[kPluginSdkSlotEntries];
};
} // namespace maicord

namespace maicord::plugin_sdk {

extern NativePluginSdkBinding* g_live[kPluginSdkBindings];

NativePluginSdkBinding* live(const void* candidate) noexcept;
// Binding for an interface call, or null when stale/disabled (registration: start only).
NativePluginSdkBinding* active(void* candidate, u64 generation, bool registration = false) noexcept;
inline bool permitted(const NativePluginSdkBinding& binding, u64 permission) noexcept {
    return (binding.definition.permissions & permission) != 0;
}
inline bool permitted_all(const NativePluginSdkBinding& binding, u64 permissions) noexcept {
    return (binding.definition.permissions & permissions) == permissions;
}
// Interface-call prologue: the binding for handle when it is current and holds every one of permissions, else
// null with the status to return in *status.
NativePluginSdkBinding* gate(void* handle, u64 generation, u64 permissions, mc_status* status,
                             bool registration = false) noexcept;
// Moves a binding between phases; runtime->enabled counts the enabled ones.
inline void set_phase(NativePluginSdkBinding& binding, Phase phase) noexcept {
    binding.runtime->enabled += (phase == Phase::enabled) - (binding.phase == Phase::enabled);
    binding.phase = phase;
}
// Enabled, not waiting for a budget disable, and the handler is set.
bool callable(const NativePluginSdkBinding& binding, const void* handler) noexcept;
NativePluginSdkBinding* binding_for(const NativePluginSdkRuntime& runtime, u64 owner) noexcept;
// Iteration helper: enabled bindings of this runtime that aren't waiting for a disable.
bool usable(const NativePluginSdkBinding* binding, const NativePluginSdkRuntime& runtime) noexcept;
// The enabled binding of owner holding permission whose handler Field is set, or null.
template <auto Field> inline NativePluginSdkBinding* handler_binding(const NativePluginSdkRuntime* runtime, u64 owner,
                                                                     u64 permission) noexcept {
    auto* binding = runtime ? binding_for(*runtime, owner) : nullptr;
    return binding && permitted_all(*binding, permission) && callable(*binding, reinterpret_cast<const void*>(binding->*Field))
               ? binding : nullptr;
}

// Where a handler setter stores, packed into one register so each interface function is a tail call: the
// permissions it needs, the handler's byte offset in the binding, and the byte offset of a runtime u64 stepped
// when it changes (kNoBump: none).
constexpr u32 kNoBump = 0xFFFFF;
struct HandlerSlot {
    u64 permissions : 22, field : 20, bump : 20;
};
static_assert(sizeof(HandlerSlot) == sizeof(u64) && MC_PERM_ALL < (1ull << 22));
mc_status set_handler_at(void* handle, u64 generation, const void* handler, HandlerSlot slot) noexcept;
// Interface functions that store one handler (`Field` of the binding), gated by Permission; Bump names a runtime
// revision stepped when the handler changes.
template <typename C, typename T> T handler_type(T C::*) noexcept;
template <auto Field, u64 Permission, u64 NativePluginSdkRuntime::*Bump = nullptr>
mc_status MC_CALL set_handler(void* handle, uint64_t generation, decltype(handler_type(Field)) handler) {
    static_assert(sizeof(Field) == sizeof(u32) && sizeof(Bump) == sizeof(u32), "data member pointers are offsets");
    const u32 field = __builtin_bit_cast(u32, Field);
    const u32 bump = Bump != nullptr ? __builtin_bit_cast(u32, Bump) : kNoBump; // offsets stay far below 1 MiB
    return set_handler_at(handle, generation, reinterpret_cast<const void*>(handler), {Permission, field, bump});
}

struct Call {
    PluginCallLedger* ledger;
    PluginCallTicket ticket;
    Call(const NativePluginSdkBinding& binding, PluginCallKind kind) noexcept
        : ledger(binding.runtime->ledger), ticket(plugin_call_begin(binding.runtime->ledger, binding.owner_id, kind)) {}
    ~Call() noexcept { plugin_call_end(ledger, ticket); }
};
// A user-initiated callback: gesture-only actions are allowed while it runs.
struct Gesture {
    NativePluginSdkRuntime* runtime;
    explicit Gesture(NativePluginSdkRuntime* value) noexcept : runtime(value) { ++runtime->gesture_depth; }
    ~Gesture() noexcept { --runtime->gesture_depth; }
};
// Runs a plugin handler under the call ledger, inside a gesture when kGesture.
template <bool kGesture, typename Handler, typename... Args>
inline decltype(auto) invoke(NativePluginSdkBinding& binding, PluginCallKind kind, Handler handler, Args... args) {
    Call call(binding, kind);
    if constexpr (kGesture) {
        Gesture gesture(binding.runtime);
        return handler(binding.plugin_state, binding.plugin_state_bytes, args...);
    } else {
        return handler(binding.plugin_state, binding.plugin_state_bytes, args...);
    }
}

u64 now_ms(const NativePluginSdkRuntime& runtime) noexcept;
inline Utf8View view(mc_str text) noexcept { return {text.data, text.bytes}; }
// UTF-8 without C0 controls (except, when lines is set, LF).
bool text_ok(mc_str text, u32 maximum, bool required, bool lines = false) noexcept;
u32 copy_text(char* output, u32 capacity, Utf8View text) noexcept;
// Untyped bodies of read_prefix/write_prefix (plugin_sdk.cpp), shared by every
// size-prefixed struct: the leading u32 is the struct's size field.
bool read_prefix_bytes(const void* source, void* output, u32 output_bytes, u32 minimum_bytes, u32 stride) noexcept;
mc_status write_prefix_bytes(void* output, const void* value, u32 value_bytes) noexcept;
template <typename T> constexpr bool size_prefixed() noexcept {
    return offsetof(T, size) == 0 && sizeof(T::size) == sizeof(u32);
}
// Copies a size-prefixed struct written by the plugin into a zeroed local.
template <typename T> bool read_prefix(const T* source, T* output, u32 minimum_bytes, u32 stride = ~0u) noexcept {
    static_assert(size_prefixed<T>());
    return read_prefix_bytes(source, output, sizeof(T), minimum_bytes, stride);
}
// Writes value into the caller's struct up to the size the caller declared.
template <typename T> mc_status write_prefix(T* output, const T& value) noexcept {
    static_assert(size_prefixed<T>());
    return write_prefix_bytes(output, &value, sizeof(T));
}
mc_status status_of(NativePluginResult result) noexcept;

// Validated copy of one declarative node. limits: text/value maxima for the context.
struct NodeLimits {
    u32 text, value;
    u32 allowed_kinds; // 1 << MC_NODE_*
};
bool read_node(const mc_slot_node* nodes, u32 stride, u32 index, const NodeLimits& limits, mc_slot_node* node) noexcept;
bool url_ok(Utf8View url) noexcept;

// Interface tables (defined in their translation units).
extern const mc_menus_v2 kMenus;
extern const mc_slots_v2 kSlots;
extern const mc_render_v1 kRender;
extern const mc_read_v2 kRead;
extern const mc_events_v1 kEvents;
extern const mc_actions_v1 kActions;
extern const mc_store_v1 kStore;
// Writes a dirty store (plugin_sdk_store.cpp); true when nothing is left to write.
bool store_save(NativePluginSdkBinding& binding) noexcept;

// Per-subsystem resets when a binding stops (plugin_sdk_surfaces/data.cpp).
void surfaces_reset(NativePluginSdkBinding&) noexcept;
void data_reset(NativePluginSdkBinding&) noexcept;
} // namespace maicord::plugin_sdk
