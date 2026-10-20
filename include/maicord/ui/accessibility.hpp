#pragma once

#include "maicord/ui/widget_manager.hpp"

// Platform-neutral accessibility projection of retained widget trees.
// A snapshot is a flat tree of nodes built on the UI thread from the widget and
// layout trees plus the renderer's text commands. Every widget is accessible by
// default (role from its kind, state from its flags, bounds from layout, reading
// order from the tree, name from the text drawn inside it, else its debug name);
// surfaces refine nodes through a describe callback. The platform adapter only
// reads finished snapshots.

namespace maicord {
struct UiTextCommand;

inline constexpr u32 kUiAccessNone = 0xFFFF'FFFFu;
inline constexpr u32 kUiAccessOwnerMaximum = 48;
inline constexpr u32 kUiAccessNameMaximumBytes = 1024;
inline constexpr u32 kUiAccessValueMaximumBytes = 8192;
// Synthetic groups per ui_access_add_widgets call (UiAccessWidgetInfo::group).
inline constexpr u32 kUiAccessGroupMaximum = 4;

enum class UiAccessRole : u8 {
    none,
    window,
    pane,
    group,
    dialog,
    button,
    toggle_button,
    check_box,
    radio_button,
    combo_box,
    list,
    list_item,
    tree,
    tree_item,
    tab_list,
    tab,
    menu,
    menu_item,
    text,
    header,
    edit,
    slider,
    image,
    link,
    document,
    status,
    scroll_bar,
    count,
};

enum UiAccessState : u16 {
    ui_access_state_none = 0,
    ui_access_focusable = 1u << 0,
    ui_access_focused = 1u << 1,
    ui_access_selectable = 1u << 2,
    ui_access_selected = 1u << 3,
    ui_access_expanded = 1u << 4,
    ui_access_collapsed = 1u << 5,
    ui_access_disabled = 1u << 6,
    ui_access_checked = 1u << 7,
    ui_access_mixed = 1u << 8,
    ui_access_offscreen = 1u << 9,
    ui_access_password = 1u << 10,
    ui_access_read_only = 1u << 11,
    ui_access_modal = 1u << 12,
    // Live region: name changes are announced politely.
    ui_access_live = 1u << 13,
    ui_access_multiline = 1u << 14,
    ui_access_toggleable = 1u << 15,
};

// Patterns a node supports. Patterns imply the matching UiAccessAction.
enum UiAccessActions : u16 {
    ui_access_action_none = 0,
    ui_access_action_invoke = 1u << 0,
    ui_access_action_toggle = 1u << 1,
    ui_access_action_select = 1u << 2,
    ui_access_action_value = 1u << 3,
    ui_access_action_range = 1u << 4,
    ui_access_action_scroll = 1u << 5,
    ui_access_action_expand = 1u << 6,
    ui_access_action_focus = 1u << 7,
    // Text pattern over the value with selection_anchor/selection_caret.
    ui_access_action_text = 1u << 8,
    // ScrollItem: the host can bring the item into view.
    ui_access_action_scroll_item = 1u << 9,
    // Grid container (grid_row/grid_column hold the row and column counts).
    ui_access_action_grid = 1u << 10,
    // Grid cell (grid_row/grid_column hold its zero-based position).
    ui_access_action_grid_item = 1u << 11,
};

enum class UiAccessAction : u8 {
    none,
    invoke,
    toggle,
    select,
    set_value,
    set_range,
    scroll,
    expand,
    collapse,
    focus,
    // Bring an offscreen item into view (ScrollItem, Invoke on offscreen items).
    scroll_into_view,
    // Text selection: number carries ui_access_text_selection_pack(anchor, caret).
    select_text,
};

// Byte offsets into a node value, packed into an action number.
constexpr double ui_access_text_selection_pack(u32 anchor, u32 caret) noexcept {
    return static_cast<double>(anchor) * 4294967296.0 + static_cast<double>(caret);
}
constexpr void ui_access_text_selection_unpack(double number, u32* anchor, u32* caret) noexcept {
    const u64 packed = number > 0 ? static_cast<u64>(number) : 0;
    *anchor = static_cast<u32>(packed >> 32);
    *caret = static_cast<u32>(packed & 0xFFFF'FFFFull);
}

// Scroll actions carry their amount as the request number.
enum class UiAccessScroll : u8 {
    none,
    small_decrement,
    small_increment,
    large_decrement,
    large_increment,
};

struct UiAccessRange {
    float value, minimum, maximum, step;
    // The Page Up/Down step (UIA LargeChange); 0 reports step.
    float large_step;
};

// Scroll position for ui_access_action_scroll nodes, in percent (0..100);
// negative means "not scrollable".
struct UiAccessScrollState {
    float vertical_percent, vertical_view_percent;
};

// Set membership, grid placement and text selection shared by nodes, widget
// refinements and host-built descriptions. Zero position/set_size: unknown.
struct UiAccessPlacement {
    u32 position;  // one-based position in its set
    u32 set_size;  // items in the whole (possibly virtualized) set
    u32 selection_anchor, selection_caret; // byte offsets into the value (text nodes)
    u16 grid_row, grid_column; // cell position, or row/column counts of a grid
};

struct UiAccessNode {
    u64 key;
    u64 item; // host-defined identity for routing actions (message id, channel id...)
    UiRect rectangle; // full bounds, client DIPs
    UiRect visible;   // bounds after ancestor clipping
    UiWidgetReference source;
    UiAccessRange range;
    UiAccessScrollState scroll;
    UiAccessPlacement placement;
    u32 parent, first_child, last_child, next_sibling, previous_sibling, child_count;
    u32 name_offset, value_offset, help_offset, id_offset;
    u32 value_bytes;
    u16 name_bytes, help_bytes, id_bytes;
    u16 state;
    u16 actions;
    UiAccessRole role;
    u8 surface;
    u8 internal;
};

struct UiAccessOwner {
    const UiWidgetManager* manager;
    u32 slot_base;
    u32 slot_count;
    u16 generation;
};

// Regions a snapshot ran out of (set during a build, cleared by reset). A
// host with demand-committed storage grows the flagged regions and rebuilds.
enum UiAccessExhausted : u8 {
    ui_access_exhausted_nodes = 1,
    ui_access_exhausted_text = 2,
    ui_access_exhausted_slots = 4,
};

struct UiAccessTree {
    Slice<UiAccessNode> nodes;
    Slice<char> text;
    Slice<u32> slot_map;
    Slice<u32> key_index; // open addressing, power-of-two count; built by finish
    UiAccessOwner owners[kUiAccessOwnerMaximum];
    u32 owner_count;
    u32 count;
    u32 text_used;
    u32 slot_used;
    u32 key_mask;
    u32 focus;
    u32 rejected;
    u32 revision;
    bool finished;
    u8 exhausted;
};

// Refinement of one widget. Defaults are filled from the widget before the
// callback; leaving the info untouched keeps the default projection.
struct UiAccessWidgetInfo;
using UiAccessMount = void (*)(void* context, UiAccessTree* tree, u32 node) noexcept;
struct UiAccessWidgetInfo {
    u64 key; // zero derives a stable key from the surface and widget id
    u64 item;
    Utf8View name, value, help;
    UiAccessRange range;
    UiAccessScrollState scroll;
    UiAccessPlacement placement;
    UiAccessRole role;
    u16 state;
    u16 actions;
    bool skip;        // drop the widget, its subtree and its text
    bool transparent; // no node; children and text attach to the parent node
    bool leaf;        // keep the node; descendants contribute only text
    bool ignore_text; // drawn text owned by this node (or its leaf subtree) is dropped
    // 1..kUiAccessGroupMaximum: the node joins a synthetic container created
    // (through UiAccessScope::group) just before the first member, beside it.
    // Lets flat widget trees expose lists, tab lists and grids.
    u8 group;
    // Optional nested scene added beneath this node after its children.
    UiAccessMount mount;
    void* mount_context;
};

using UiAccessDescribe = void (*)(void* context, const UiWidgetManager& manager, UiWidgetHandle handle,
                                  const UiWidget& widget, const UiLayoutNode& layout,
                                  UiAccessWidgetInfo* info) noexcept;

struct UiAccessNodeDesc;
// Describes synthetic group node `group` (role defaults to list; key, name,
// state and actions as for any node). An empty rectangle becomes the union of
// the members' bounds. It runs while the first member's refinement is still
// pending, so it must not overwrite buffers that refinement's strings borrow.
using UiAccessDescribeGroup = void (*)(void* context, u8 group, UiAccessNodeDesc* description) noexcept;

struct UiAccessScope {
    UiAccessDescribe describe;
    void* context;
    u8 surface;
    // The manager's focused widget is the application keyboard focus.
    bool focus_owner;
    UiAccessDescribeGroup group;
};

struct UiAccessNodeDesc {
    u64 key;
    u64 item;
    UiRect rectangle;
    UiRect visible; // empty: same as rectangle
    UiWidgetReference source;
    Utf8View name, value, help;
    UiAccessRange range;
    UiAccessScrollState scroll;
    UiAccessPlacement placement;
    UiAccessRole role;
    u16 state;
    u16 actions;
    u8 surface;
    bool ignore_text;
};

bool ui_access_tree_initialize(UiAccessTree* tree, Slice<UiAccessNode> nodes, Slice<char> text,
                               Slice<u32> slot_map, Slice<u32> key_index) noexcept;
// Starts a snapshot with node 0 as the root window.
bool ui_access_tree_reset(UiAccessTree* tree, Utf8View window_name, UiRect bounds) noexcept;
u32 ui_access_add(UiAccessTree* tree, u32 parent, const UiAccessNodeDesc& description) noexcept;
bool ui_access_set_name(UiAccessTree* tree, u32 node, Utf8View name) noexcept;
bool ui_access_set_value(UiAccessTree* tree, u32 node, Utf8View value) noexcept;
bool ui_access_set_help(UiAccessTree* tree, u32 node, Utf8View help) noexcept;
// Adds the visible widget tree of one manager under parent, in tree order.
// Returns the node of the manager root (or parent when the root is transparent),
// kUiAccessNone on failure.
u32 ui_access_add_widgets(UiAccessTree* tree, u32 parent, const UiWidgetManager& manager,
                          const UiAccessScope& scope) noexcept;
// Node created for one widget, kUiAccessNone when absent (stale or pruned).
u32 ui_access_widget_node(const UiAccessTree& tree, UiWidgetReference widget) noexcept;
// Attributes drawn text to the node of its owner widget. Interactive owners
// (buttons, list items, menu items...) take it as their name; containers get
// a static text child. Text whose owner was never added is ignored.
bool ui_access_attribute_text(UiAccessTree* tree, UiWidgetReference owner, Utf8View text,
                              UiRect rectangle) noexcept;
// UiTextConsumeFunction adapter; context is the UiAccessTree.
bool ui_access_text_consumer(void* context, const UiTextCommand& command, u32 ordinal) noexcept;
// Labels, presentational children, flattening, compaction, links, focus, keys.
bool ui_access_tree_finish(UiAccessTree* tree) noexcept;

u32 ui_access_find(const UiAccessTree& tree, u64 key) noexcept;
// Offscreen, or clipped to less than half its height: activating it at its
// visible center would be ambiguous, so hosts scroll it into view first.
bool ui_access_needs_reveal(const UiAccessNode& node) noexcept;

u32 ui_access_hit_test(const UiAccessTree& tree, float x, float y) noexcept;
Utf8View ui_access_name(const UiAccessTree& tree, u32 node) noexcept;
Utf8View ui_access_value(const UiAccessTree& tree, u32 node) noexcept;
Utf8View ui_access_help(const UiAccessTree& tree, u32 node) noexcept;
Utf8View ui_access_automation_id(const UiAccessTree& tree, u32 node) noexcept;
// Nearest ancestor (exclusive) with one of the roles in the mask (1 << role).
u32 ui_access_ancestor(const UiAccessTree& tree, u32 node, u32 role_mask) noexcept;
constexpr u32 ui_access_role_bit(UiAccessRole role) noexcept {
    return 1u << static_cast<u32>(role);
}

// "CreateDirectMessage" -> "Create direct message". Returns bytes written.
u32 ui_access_humanize(Utf8View debug_name, Slice<char> output) noexcept;
u16 ui_access_default_actions(UiAccessRole role) noexcept;
u64 ui_access_key(u8 surface, u64 identity) noexcept;

// Event decisions between consecutive snapshots.
enum class UiAccessProperty : u8 {
    name,
    toggle,
    selected,
    expanded,
    value,
    range,
    // Text pattern nodes: the caret or selection moved (TextSelectionChanged).
    text_selection,
};

struct UiAccessObservation {
    u64 key;
    u64 value_hash;    // value text, or the child signature of containers
    u64 name_hash;
    u64 selection;     // anchor << 32 | caret of text nodes
    float range;
    u16 state;
    UiAccessRole role;
};

struct UiAccessEventState {
    Slice<UiAccessObservation> current, previous;
    u32 current_count, previous_count;
    u64 focus_key;
    bool primed;
};

inline constexpr u32 kUiAccessPlanStructureMaximum = 8;
inline constexpr u32 kUiAccessPlanPropertyMaximum = 32;

struct UiAccessPropertyChange {
    u32 node;
    UiAccessProperty property;
    u16 old_state;
    float old_range;
};

struct UiAccessEventPlan {
    u32 focus_node;
    u32 structure_count;
    u32 structures[kUiAccessPlanStructureMaximum];
    u32 property_count;
    UiAccessPropertyChange properties[kUiAccessPlanPropertyMaximum];
    bool focus_changed;
    bool structure_root; // too many containers changed: invalidate from the root
};

// The two observation buffers must be equally sized; nodes beyond their
// capacity are not observed (the plan then falls back to a root change).
bool ui_access_event_state_initialize(UiAccessEventState* state, Slice<UiAccessObservation> current,
                                      Slice<UiAccessObservation> previous) noexcept;
bool ui_access_plan_events(UiAccessEventState* state, const UiAccessTree& tree, UiAccessEventPlan* plan) noexcept;

// New-message announcement decision for the focused conversation. The first
// observation of a channel (or a channel switch) only primes the cursor.
struct UiAccessMessageCursor {
    u64 channel_id;
    u64 newest_message_id;
    bool primed;
};
bool ui_access_message_should_announce(UiAccessMessageCursor* cursor, u64 channel_id, u64 newest_message_id,
                                       bool authored_locally) noexcept;

// Text pattern units. Offsets are UTF-8 byte offsets into a node value and
// always land on scalar (and simple cluster) boundaries. Characters keep
// combining marks, variation selectors, skin tones, ZWJ sequences, flag pairs
// and CRLF together; words are a run of word or punctuation characters plus the
// spaces that follow; lines and paragraphs end after a line feed; format, page
// and document are the whole text.
enum class UiAccessTextUnit : u8 {
    character,
    format,
    word,
    line,
    paragraph,
    page,
    document,
};

// Clamps offset onto the nearest boundary at or before it.
u32 ui_access_text_align(Utf8View text, u32 offset) noexcept;
// ExpandToEnclosingUnit: the unit containing start (the last unit at the end).
void ui_access_text_expand(Utf8View text, UiAccessTextUnit unit, u32* start, u32* end) noexcept;
// MoveEndpointByUnit for one endpoint; returns the signed count actually moved.
i32 ui_access_text_move_endpoint(Utf8View text, UiAccessTextUnit unit, i32 count, u32* offset) noexcept;
// Move: a degenerate range moves its insertion point (it may reach the end);
// a non-degenerate range becomes the unit count units away. Returns moved.
i32 ui_access_text_move(Utf8View text, UiAccessTextUnit unit, i32 count, u32* start, u32* end) noexcept;

enum class UiAccessPoliteness : u8 {
    polite,    // queued after current speech; only the newest pending one survives
    assertive, // interrupts; reserved for failures that need attention
};

inline constexpr u32 kUiAccessAnnounceHistory = 8;
// Identical text inside this window is dropped.
inline constexpr u64 kUiAccessAnnounceRepeatMs = 4000;
// Polite announcements share this many slots per window; assertive ones bypass
// the budget but not the repeat filter.
inline constexpr u32 kUiAccessAnnouncePoliteBurst = 3;
inline constexpr u64 kUiAccessAnnounceWindowMs = 2000;

struct UiAccessAnnouncer {
    u64 hashes[kUiAccessAnnounceHistory];
    u64 times[kUiAccessAnnounceHistory];
    u64 polite_times[kUiAccessAnnouncePoliteBurst];
    u32 next, polite_next;
    u32 admitted, dropped;
};
// Rate limiting decision; records admitted announcements.
bool ui_access_announce_admit(UiAccessAnnouncer* announcer, Utf8View text, UiAccessPoliteness politeness,
                              u64 now_ms) noexcept;

// Snapshots follow presented frames. Urgent frames (input, focus, actions,
// structure) rebuild at once; others coalesce to one rebuild per interval and
// leave a deferred rebuild for the trailing frame of an animation.
inline constexpr u64 kUiAccessRebuildIntervalMs = 100;
struct UiAccessThrottle {
    u64 last_build_ms;
    u32 builds, coalesced;
    bool urgent;
    bool deferred;
};
enum class UiAccessThrottleDecision : u8 {
    idle,  // nothing to do (no listener)
    build, // rebuild now
    defer, // rebuild later (deferred stays set; call again when the delay passes)
};
void ui_access_throttle_mark_urgent(UiAccessThrottle* throttle) noexcept;
UiAccessThrottleDecision ui_access_throttle_frame(UiAccessThrottle* throttle, u64 now_ms, bool listening) noexcept;
// Milliseconds until a deferred rebuild is due (0 when due or none pending).
u64 ui_access_throttle_delay(const UiAccessThrottle& throttle, u64 now_ms) noexcept;

// Fixed buffer for describers composing names; ui_access_add copies at once.
struct UiAccessNameBuilder {
    char data[kUiAccessNameMaximumBytes];
    u32 bytes;

    UiAccessNameBuilder& clear() noexcept {
        bytes = 0;
        return *this;
    }
    __declspec(noinline) UiAccessNameBuilder& add(Utf8View text) noexcept {
        const u32 room = static_cast<u32>(sizeof(data)) - bytes;
        u32 length = text.bytes < room ? text.bytes : room;
        while (length && length < text.bytes && (static_cast<u8>(text.data[length]) & 0xC0u) == 0x80u)
            --length;
        for (u32 index = 0; index < length; ++index)
            data[bytes + index] = text.data[index];
        bytes += length;
        return *this;
    }
    // Adds separator before text when both the name so far and text are non-empty.
    __declspec(noinline) UiAccessNameBuilder& part(Utf8View text, Utf8View separator = {", ", 2}) noexcept {
        if (!text.bytes)
            return *this;
        if (bytes)
            add(separator);
        return add(text);
    }
    __declspec(noinline) UiAccessNameBuilder& number(u64 value) noexcept {
        char digits[20];
        u32 count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        while (count && bytes < sizeof(data))
            data[bytes++] = digits[--count];
        return *this;
    }
    Utf8View view() const noexcept {
        return {data, bytes};
    }
};

} // namespace maicord
