#include "maicord/ui/accessibility.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/hash.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {
namespace {

enum AccessInternal : u8 {
    internal_explicit_name = 1u << 0,
    internal_synthesized = 1u << 1,
    internal_remove = 1u << 2,
    internal_text_attributed = 1u << 3,
    internal_ignore_text = 1u << 4,
};

constexpr u32 kNameFromContentRoles =
    ui_access_role_bit(UiAccessRole::button) | ui_access_role_bit(UiAccessRole::toggle_button) |
    ui_access_role_bit(UiAccessRole::check_box) | ui_access_role_bit(UiAccessRole::radio_button) |
    ui_access_role_bit(UiAccessRole::list_item) | ui_access_role_bit(UiAccessRole::tree_item) |
    ui_access_role_bit(UiAccessRole::tab) | ui_access_role_bit(UiAccessRole::menu_item) |
    ui_access_role_bit(UiAccessRole::link) | ui_access_role_bit(UiAccessRole::header) |
    ui_access_role_bit(UiAccessRole::status) | ui_access_role_bit(UiAccessRole::text);
// Roles whose descendants are presentational: their text forms the name.
constexpr u32 kCompositeRoles = kNameFromContentRoles & ~ui_access_role_bit(UiAccessRole::text);
// Controls labelled by drawn text without merging children.
constexpr u32 kLabelledRoles = ui_access_role_bit(UiAccessRole::edit) |
                               ui_access_role_bit(UiAccessRole::combo_box) |
                               ui_access_role_bit(UiAccessRole::slider);
// Text climbs through these toward a composite ancestor.
constexpr u32 kPassThroughRoles = ui_access_role_bit(UiAccessRole::none) |
                                  ui_access_role_bit(UiAccessRole::group) |
                                  ui_access_role_bit(UiAccessRole::text) |
                                  ui_access_role_bit(UiAccessRole::image);
constexpr u32 kContainerRoles =
    ui_access_role_bit(UiAccessRole::window) | ui_access_role_bit(UiAccessRole::pane) |
    ui_access_role_bit(UiAccessRole::dialog) | ui_access_role_bit(UiAccessRole::list) |
    ui_access_role_bit(UiAccessRole::tree) | ui_access_role_bit(UiAccessRole::menu) |
    ui_access_role_bit(UiAccessRole::tab_list) | ui_access_role_bit(UiAccessRole::document);
// Nodes that get a humanized debug name when nothing better is known.
constexpr u32 kFallbackNameRoles =
    kCompositeRoles | kLabelledRoles | kContainerRoles | ui_access_role_bit(UiAccessRole::edit) |
    ui_access_role_bit(UiAccessRole::scroll_bar);

constexpr bool has_role(u32 mask, UiAccessRole role) noexcept {
    return (mask & ui_access_role_bit(role)) != 0;
}

u64 bucket_hash(u64 key) noexcept {
    key ^= key >> 33;
    key *= 0xFF51AFD7ED558CCDull;
    key ^= key >> 33;
    key *= 0xC4CEB9FE1A85EC53ull;
    return key ^ (key >> 33);
}

bool utf8_continuation(char byte) noexcept {
    return (static_cast<u8>(byte) & 0xC0u) == 0x80u;
}

// Appends normalized text: whitespace runs collapse to one space, U+FFFC and
// other C0 controls are dropped, and the total stays within limit on a scalar
// boundary. Returns bytes appended.
u32 append_normalized(UiAccessTree* tree, Utf8View text, u32 used, u32 limit, bool leading_space) noexcept {
    u32 written = 0;
    bool pending_space = leading_space && used != 0;
    const u32 capacity = tree->text.count;
    for (u32 index = 0; index < text.bytes;) {
        const char byte = text.data[index];
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') {
            pending_space = used + written != 0;
            ++index;
            continue;
        }
        if (static_cast<u8>(byte) < 0x20u) {
            ++index;
            continue;
        }
        u32 width = 1;
        if ((static_cast<u8>(byte) & 0xE0u) == 0xC0u)
            width = 2;
        else if ((static_cast<u8>(byte) & 0xF0u) == 0xE0u)
            width = 3;
        else if ((static_cast<u8>(byte) & 0xF8u) == 0xF0u)
            width = 4;
        if (index + width > text.bytes)
            break;
        // U+FFFC object replacement marks inline objects; it has no speech.
        if (width == 3 && static_cast<u8>(text.data[index]) == 0xEFu &&
            static_cast<u8>(text.data[index + 1]) == 0xBFu && static_cast<u8>(text.data[index + 2]) == 0xBCu) {
            index += 3;
            continue;
        }
        const u32 needed = width + (pending_space ? 1u : 0u);
        if (used + written + needed > limit)
            break;
        if (tree->text_used + written + needed > capacity) {
            tree->exhausted |= ui_access_exhausted_text;
            break;
        }
        char* out = tree->text.data + tree->text_used + written;
        if (pending_space) {
            *out++ = ' ';
            ++written;
            pending_space = false;
        }
        for (u32 i = 0; i < width; ++i)
            out[i] = text.data[index + i];
        written += width;
        index += width;
    }
    return written;
}

bool write_field(UiAccessTree* tree, Utf8View text, u32 limit, u32* offset, u32* bytes) noexcept {
    *offset = tree->text_used;
    const u32 written = append_normalized(tree, text, 0, limit, false);
    tree->text_used += written;
    *bytes = written;
    return written == text.bytes || written != 0 || text.bytes == 0;
}

// Values keep their line structure (edit content); only the limit applies.
bool write_raw(UiAccessTree* tree, Utf8View text, u32 limit, u32* offset, u32* bytes) noexcept {
    u32 length = minimum(text.bytes, minimum(limit, tree->text.count - tree->text_used));
    if (length < minimum(text.bytes, limit))
        tree->exhausted |= ui_access_exhausted_text;
    while (length != 0 && length < text.bytes && utf8_continuation(text.data[length]))
        --length;
    *offset = tree->text_used;
    copy_bytes(tree->text.data + tree->text_used, text.data, length);
    tree->text_used += length;
    *bytes = length;
    return length == text.bytes;
}

bool append_name(UiAccessTree* tree, u32 node_index, Utf8View text) noexcept {
    auto& node = tree->nodes[node_index];
    if (node.name_offset + node.name_bytes != tree->text_used || node.name_bytes == 0) {
        // Move the current name to the tail so it can grow in place.
        const u32 bytes = node.name_bytes;
        if (tree->text_used + bytes > tree->text.count) {
            tree->exhausted |= ui_access_exhausted_text;
            return false;
        }
        if (bytes)
            copy_bytes(tree->text.data + tree->text_used, tree->text.data + node.name_offset, bytes);
        node.name_offset = tree->text_used;
        tree->text_used += bytes;
    }
    const u32 written = append_normalized(tree, text, node.name_bytes, kUiAccessNameMaximumBytes, true);
    tree->text_used += written;
    node.name_bytes = static_cast<u16>(node.name_bytes + written);
    return true;
}

UiAccessRole default_role(UiWidgetKind kind) noexcept {
    switch (kind) {
    case UiWidgetKind::text:
        return UiAccessRole::text;
    case UiWidgetKind::button:
        return UiAccessRole::button;
    case UiWidgetKind::text_field:
        return UiAccessRole::edit;
    case UiWidgetKind::image:
        return UiAccessRole::image;
    case UiWidgetKind::scroll_view:
        return UiAccessRole::pane;
    case UiWidgetKind::container:
    case UiWidgetKind::custom:
    default:
        return UiAccessRole::group;
    }
}

bool layout_visible(const UiLayoutNode& node) noexcept {
    return node.visible && (node.spec.style.flags & ui_layout_node_visible) != 0;
}

struct WalkContext {
    UiAccessTree* tree;
    const UiWidgetManager* manager;
    const UiAccessScope* scope;
    UiAccessOwner* owner;
    u32 groups[kUiAccessGroupMaximum];
    bool group_bounds[kUiAccessGroupMaximum]; // bounds follow the members
    bool failed;
};

// Node of synthetic group `group`, created before its first member.
u32 group_node(WalkContext& context, u8 group, u32 parent) noexcept {
    u32& node = context.groups[group - 1];
    if (node != kUiAccessNone)
        return node;
    UiAccessNodeDesc description{};
    description.role = UiAccessRole::list;
    context.scope->group(context.scope->context, group, &description);
    description.key = ui_access_key(context.scope->surface,
                                    description.key ? description.key : (0x4752'4F55'5000'0000ull | group));
    description.surface = context.scope->surface;
    context.group_bounds[group - 1] = !ui_rect_has_area(description.rectangle);
    node = ui_access_add(context.tree, parent, description);
    if (node == kUiAccessNone)
        context.failed = true;
    else if (context.group_bounds[group - 1])
        context.tree->nodes[node].state |= ui_access_offscreen;
    return node;
}

void grow_group(WalkContext& context, u8 group, const UiAccessNode& member) noexcept {
    if (!context.group_bounds[group - 1])
        return;
    auto& node = context.tree->nodes[context.groups[group - 1]];
    const bool first = !ui_rect_has_area(node.rectangle);
    auto unite = [](UiRect a, UiRect b) noexcept {
        return UiRect{minimum(a.left, b.left), minimum(a.top, b.top), maximum(a.right, b.right),
                      maximum(a.bottom, b.bottom)};
    };
    node.rectangle = first ? member.rectangle : unite(node.rectangle, member.rectangle);
    if (!(member.state & ui_access_offscreen)) {
        node.visible = (node.state & ui_access_offscreen) ? member.visible : unite(node.visible, member.visible);
        node.state &= static_cast<u16>(~ui_access_offscreen);
    } else if (node.state & ui_access_offscreen) {
        node.visible = node.rectangle;
    }
}

void walk(WalkContext& context, u16 slot, u32 parent, bool leaf, u32 depth) noexcept {
    const auto& manager = *context.manager;
    if (context.failed || slot >= manager.count || depth > kMaximumUiLayoutDepth)
        return;
    const UiLayoutNode& layout = manager.layout.storage[slot];
    const UiWidget& widget = manager.storage[slot];
    if (!layout_visible(layout))
        return;
    auto* tree = context.tree;
    u32* map = tree->slot_map.data + context.owner->slot_base;
    if (leaf) {
        map[slot] = parent;
        for (u16 child = layout.first_child; child != kInvalidUiLayoutSlot && child < manager.count;
             child = manager.layout.storage[child].next_sibling)
            walk(context, child, parent, true, depth + 1);
        return;
    }
    const UiWidgetHandle handle{slot, manager.generation};
    UiAccessWidgetInfo info{};
    info.role = default_role(widget.kind);
    const bool interactive = widget.kind == UiWidgetKind::button || widget.kind == UiWidgetKind::text_field;
    if (widget.flags & ui_widget_focusable)
        info.state |= ui_access_focusable;
    if (interactive && !ui_widget_manager_input_enabled(manager, handle))
        info.state |= ui_access_disabled;
    if (context.scope->focus_owner && manager.focused.slot == slot && manager.focused.generation == manager.generation)
        info.state |= ui_access_focused;
    info.actions = ui_access_default_actions(info.role);
    if (context.scope->describe)
        context.scope->describe(context.scope->context, manager, handle, widget, layout, &info);
    if (info.skip)
        return;
    u32 node = parent;
    u32 attach = parent;
    const bool grouped = info.group && info.group <= kUiAccessGroupMaximum && context.scope->group &&
                         !info.transparent;
    if (grouped) {
        attach = group_node(context, info.group, parent);
        if (attach == kUiAccessNone)
            return;
    }
    if (!info.transparent) {
        UiAccessNodeDesc description{};
        description.key = ui_access_key(context.scope->surface,
                                        info.key ? info.key : (static_cast<u64>(widget.id) | (1ull << 63)));
        description.item = info.item;
        description.rectangle = layout.rectangle;
        description.visible = ui_rect_intersection(layout.rectangle, layout.clip_rectangle);
        description.source = {&manager, handle};
        description.name = info.name;
        description.value = info.value;
        description.help = info.help;
        description.range = info.range;
        description.scroll = info.scroll;
        description.placement = info.placement;
        description.role = info.role;
        description.state = info.state;
        description.actions = info.actions;
        description.surface = context.scope->surface;
        description.ignore_text = info.ignore_text;
        if (!ui_rect_has_area(description.visible))
            description.state |= ui_access_offscreen;
        node = ui_access_add(tree, attach, description);
        if (node == kUiAccessNone) {
            context.failed = true;
            return;
        }
        auto& created = tree->nodes[node];
        if (grouped)
            grow_group(context, info.group, created);
        const Utf8View debug_name = layout.spec.debug_name;
        if (debug_name.bytes) {
            u32 bytes = 0;
            write_field(tree, debug_name, 128, &created.id_offset, &bytes);
            created.id_bytes = static_cast<u16>(bytes);
        }
    }
    map[slot] = node;
    for (u16 child = layout.first_child; child != kInvalidUiLayoutSlot && child < manager.count;
         child = manager.layout.storage[child].next_sibling)
        walk(context, child, node, info.leaf && !info.transparent, depth + 1);
    if (info.mount && !info.transparent && !context.failed)
        info.mount(info.mount_context, tree, node);
}

const UiAccessOwner* find_owner(const UiAccessTree& tree, const UiWidgetManager* manager) noexcept {
    for (u32 index = tree.owner_count; index != 0; --index) {
        const auto& owner = tree.owners[index - 1];
        if (owner.manager == manager)
            return &owner;
    }
    return nullptr;
}

bool rect_before(const UiRect& a, const UiRect& b) noexcept {
    // Reading order: rows first (with a little tolerance), then left to right.
    if (a.bottom <= b.top + 1.0f)
        return true;
    if (b.bottom <= a.top + 1.0f)
        return false;
    return a.left < b.left;
}

} // namespace

u64 ui_access_key(u8 surface, u64 identity) noexcept {
    return hash64_mix(hash64_mix(kHash64Seed, static_cast<u64>(surface) + 1), identity);
}

u16 ui_access_default_actions(UiAccessRole role) noexcept {
    switch (role) {
    case UiAccessRole::button:
    case UiAccessRole::menu_item:
    case UiAccessRole::link:
        return ui_access_action_invoke;
    case UiAccessRole::toggle_button:
    case UiAccessRole::check_box:
        return ui_access_action_toggle;
    case UiAccessRole::radio_button:
    case UiAccessRole::tab:
        return ui_access_action_select;
    case UiAccessRole::edit:
        return ui_access_action_value | ui_access_action_focus;
    case UiAccessRole::slider:
        return ui_access_action_range;
    case UiAccessRole::combo_box:
        return ui_access_action_expand;
    default:
        return ui_access_action_none;
    }
}

bool ui_access_tree_initialize(UiAccessTree* tree, Slice<UiAccessNode> nodes, Slice<char> text,
                               Slice<u32> slot_map, Slice<u32> key_index) noexcept {
    if (!tree || !nodes.data || nodes.count < 2 || !text.data || !key_index.data ||
        key_index.count < nodes.count * 2 || (key_index.count & (key_index.count - 1)) != 0)
        return false;
    reset_value<UiAccessTree>(tree);
    tree->nodes = nodes;
    tree->text = text;
    tree->slot_map = slot_map;
    tree->key_index = key_index;
    tree->key_mask = key_index.count - 1;
    tree->focus = kUiAccessNone;
    return true;
}

bool ui_access_tree_reset(UiAccessTree* tree, Utf8View window_name, UiRect bounds) noexcept {
    if (!tree || !tree->nodes.data)
        return false;
    tree->count = 0;
    tree->text_used = 0;
    tree->slot_used = 0;
    tree->exhausted = 0;
    tree->owner_count = 0;
    tree->focus = kUiAccessNone;
    tree->finished = false;
    ++tree->revision;
    UiAccessNodeDesc root{};
    root.key = ui_access_key(0, 0);
    root.rectangle = bounds;
    root.role = UiAccessRole::window;
    root.name = window_name;
    return ui_access_add(tree, kUiAccessNone, root) == 0;
}

u32 ui_access_add(UiAccessTree* tree, u32 parent, const UiAccessNodeDesc& description) noexcept {
    if (!tree || tree->count >= tree->nodes.count || (tree->count != 0 && parent >= tree->count) ||
        (tree->count == 0 && parent != kUiAccessNone)) {
        if (tree) {
            ++tree->rejected;
            if (tree->count >= tree->nodes.count)
                tree->exhausted |= ui_access_exhausted_nodes;
        }
        return kUiAccessNone;
    }
    const u32 index = tree->count++;
    auto& node = tree->nodes[index];
    reset_value<UiAccessNode>(&node);
    node.key = description.key;
    node.item = description.item;
    node.rectangle = description.rectangle;
    node.visible = ui_rect_has_area(description.visible) ? description.visible : description.rectangle;
    node.source = description.source;
    node.range = description.range;
    node.scroll = description.scroll;
    node.placement = description.placement;
    node.parent = parent;
    node.first_child = node.last_child = node.next_sibling = node.previous_sibling = kUiAccessNone;
    node.state = description.state;
    node.role = description.role;
    node.actions = description.actions;
    node.surface = description.surface;
    if (description.ignore_text)
        node.internal |= internal_ignore_text;
    if (parent != kUiAccessNone) {
        // Provisional links; finish rebuilds them after pruning.
        auto& owner = tree->nodes[parent];
        node.previous_sibling = owner.last_child;
        if (owner.last_child != kUiAccessNone)
            tree->nodes[owner.last_child].next_sibling = index;
        else
            owner.first_child = index;
        owner.last_child = index;
        ++owner.child_count;
    }
    if (description.name.bytes)
        ui_access_set_name(tree, index, description.name);
    if (description.value.bytes)
        ui_access_set_value(tree, index, description.value);
    if (description.help.bytes)
        ui_access_set_help(tree, index, description.help);
    return index;
}

bool ui_access_set_name(UiAccessTree* tree, u32 node, Utf8View name) noexcept {
    if (!tree || node >= tree->count)
        return false;
    u32 offset = 0, bytes = 0;
    const bool complete = write_field(tree, name, kUiAccessNameMaximumBytes, &offset, &bytes);
    tree->nodes[node].name_offset = offset;
    tree->nodes[node].name_bytes = static_cast<u16>(bytes);
    tree->nodes[node].internal |= internal_explicit_name;
    return complete && bytes != 0;
}

bool ui_access_set_value(UiAccessTree* tree, u32 node, Utf8View value) noexcept {
    if (!tree || node >= tree->count)
        return false;
    u32 offset = 0, bytes = 0;
    const bool complete = write_raw(tree, value, kUiAccessValueMaximumBytes, &offset, &bytes);
    tree->nodes[node].value_offset = offset;
    tree->nodes[node].value_bytes = bytes;
    return complete;
}

bool ui_access_set_help(UiAccessTree* tree, u32 node, Utf8View help) noexcept {
    if (!tree || node >= tree->count)
        return false;
    u32 offset = 0, bytes = 0;
    const bool complete = write_field(tree, help, kUiAccessNameMaximumBytes, &offset, &bytes);
    tree->nodes[node].help_offset = offset;
    tree->nodes[node].help_bytes = static_cast<u16>(bytes);
    return complete;
}

u32 ui_access_add_widgets(UiAccessTree* tree, u32 parent, const UiWidgetManager& manager,
                          const UiAccessScope& scope) noexcept {
    if (!tree || parent >= tree->count || tree->finished || !manager.storage.data || manager.count == 0)
        return kUiAccessNone;
    if (tree->owner_count >= kUiAccessOwnerMaximum || tree->slot_used + manager.count > tree->slot_map.count) {
        ++tree->rejected;
        if (tree->slot_used + manager.count > tree->slot_map.count)
            tree->exhausted |= ui_access_exhausted_slots;
        return kUiAccessNone;
    }
    auto& owner = tree->owners[tree->owner_count++];
    owner = {&manager, tree->slot_used, manager.count, manager.generation};
    tree->slot_used += manager.count;
    for (u32 index = 0; index < manager.count; ++index)
        tree->slot_map[owner.slot_base + index] = kUiAccessNone;
    const u32 first = tree->count;
    WalkContext context{tree, &manager, &scope, &owner, {}, {}, false};
    for (u32& group : context.groups)
        group = kUiAccessNone;
    walk(context, 0, parent, false, 0);
    if (context.failed)
        return kUiAccessNone;
    return tree->count > first ? first : parent;
}

u32 ui_access_widget_node(const UiAccessTree& tree, UiWidgetReference widget) noexcept {
    if (!widget.manager || tree.finished)
        return kUiAccessNone;
    const auto* owner = find_owner(tree, widget.manager);
    if (!owner || widget.handle.slot >= owner->slot_count ||
        !ui_widget_manager_resolve(*widget.manager, widget.handle))
        return kUiAccessNone;
    return tree.slot_map[owner->slot_base + widget.handle.slot];
}

bool ui_access_attribute_text(UiAccessTree* tree, UiWidgetReference owner, Utf8View text,
                              UiRect rectangle) noexcept {
    if (!tree || !text.bytes)
        return false;
    const u32 node = ui_access_widget_node(*tree, owner);
    if (node == kUiAccessNone)
        return false;
    if (tree->nodes[node].internal & internal_ignore_text)
        return true;
    u32 target = node;
    while (target != kUiAccessNone && has_role(kPassThroughRoles, tree->nodes[target].role))
        target = tree->nodes[target].parent;
    if (target != kUiAccessNone &&
        (has_role(kCompositeRoles, tree->nodes[target].role) || has_role(kLabelledRoles, tree->nodes[target].role))) {
        auto& control = tree->nodes[target];
        if ((control.internal & internal_explicit_name) && !(control.internal & internal_text_attributed))
            return true;
        control.internal |= internal_text_attributed;
        return append_name(tree, target, text);
    }
    auto& owner_node = tree->nodes[node];
    if (owner_node.role == UiAccessRole::text) {
        if ((owner_node.internal & internal_explicit_name) && !(owner_node.internal & internal_text_attributed))
            return true;
        owner_node.internal |= internal_text_attributed;
        return append_name(tree, node, text);
    }
    UiAccessNodeDesc description{};
    description.key = hash64_mix(hash64_bytes(owner_node.key, text.data, text.bytes), 0x7458);
    description.rectangle = ui_rect_has_area(rectangle) ? rectangle : owner_node.rectangle;
    description.visible = ui_rect_intersection(description.rectangle, owner_node.visible);
    description.role = UiAccessRole::text;
    description.surface = owner_node.surface;
    description.state = static_cast<u16>(owner_node.state & ui_access_offscreen);
    if (!ui_rect_has_area(description.visible))
        description.state |= ui_access_offscreen;
    description.name = text;
    const u32 created = ui_access_add(tree, node, description);
    if (created == kUiAccessNone)
        return false;
    tree->nodes[created].internal |= internal_synthesized | internal_text_attributed;
    return true;
}

bool ui_access_text_consumer(void* context, const UiTextCommand& command, u32) noexcept {
    auto* tree = static_cast<UiAccessTree*>(context);
    if (tree && command.owner.manager)
        ui_access_attribute_text(tree, command.owner, command.text, command.rectangle);
    return true;
}

bool ui_access_tree_finish(UiAccessTree* tree) noexcept {
    if (!tree || tree->count == 0 || tree->finished)
        return false;
    const u32 count = tree->count;
    auto* nodes = tree->nodes.data;
    // 1. Labels: a labelled control takes the text of its preceding sibling.
    for (u32 index = 1; index < count; ++index) {
        auto& node = nodes[index];
        if (node.name_bytes || !(has_role(kLabelledRoles, node.role) || node.role == UiAccessRole::check_box ||
                                 node.role == UiAccessRole::radio_button))
            continue;
        for (u32 sibling = index - 1; sibling > node.parent && sibling != kUiAccessNone; --sibling) {
            if (nodes[sibling].parent != node.parent)
                continue;
            if (nodes[sibling].role == UiAccessRole::text && nodes[sibling].name_bytes) {
                const Utf8View label{tree->text.data + nodes[sibling].name_offset, nodes[sibling].name_bytes};
                char copy[kUiAccessNameMaximumBytes];
                copy_bytes(copy, label.data, label.bytes);
                ui_access_set_name(tree, index, {copy, label.bytes});
            }
            break;
        }
    }
    // 2. Fallback names from debug names.
    for (u32 index = 1; index < count; ++index) {
        auto& node = nodes[index];
        if (node.name_bytes || !node.id_bytes || !has_role(kFallbackNameRoles, node.role))
            continue;
        if (has_role(kContainerRoles, node.role) && node.role != UiAccessRole::dialog &&
            node.role != UiAccessRole::list && node.role != UiAccessRole::menu)
            continue;
        char humanized[160];
        const u32 bytes = ui_access_humanize({tree->text.data + node.id_offset, node.id_bytes},
                                             {humanized, sizeof(humanized)});
        if (bytes)
            ui_access_set_name(tree, index, {humanized, bytes});
    }
    // 3. Removal: unnamed text, presentational children, anonymous groups.
    u32* composite = tree->key_index.data;
    u32* keep_parent = tree->key_index.data + count;
    composite[0] = kUiAccessNone;
    for (u32 index = 1; index < count; ++index) {
        const auto& parent = nodes[nodes[index].parent];
        composite[index] = has_role(kCompositeRoles, parent.role) && !(parent.internal & internal_remove)
                               ? nodes[index].parent
                               : composite[nodes[index].parent];
        auto& node = nodes[index];
        const bool interactive = node.actions != 0 || (node.state & (ui_access_focusable | ui_access_focused));
        bool remove = false;
        if (node.role == UiAccessRole::text && !node.name_bytes)
            remove = true;
        else if (composite[index] != kUiAccessNone && !interactive && has_role(kPassThroughRoles, node.role))
            remove = true;
        else if ((node.role == UiAccessRole::group || node.role == UiAccessRole::none) && !interactive &&
                 !node.name_bytes && !(node.state & (ui_access_modal | ui_access_live)))
            remove = true;
        else if (node.role == UiAccessRole::pane && !node.name_bytes && !interactive)
            remove = true;
        if (remove)
            node.internal |= internal_remove;
    }
    // 4. Compaction. Parents always precede children, so new indices preserve that.
    u32* new_index = composite;
    keep_parent[0] = kUiAccessNone;
    u32 kept = 0;
    for (u32 index = 0; index < count; ++index) {
        if (index != 0) {
            const u32 parent = nodes[index].parent;
            keep_parent[index] = (nodes[parent].internal & internal_remove) ? keep_parent[parent] : parent;
        }
        new_index[index] = (nodes[index].internal & internal_remove) ? kUiAccessNone : kept++;
    }
    for (u32 index = 0; index < count; ++index) {
        if (new_index[index] == kUiAccessNone)
            continue;
        UiAccessNode node = nodes[index];
        node.parent = index == 0 ? kUiAccessNone : new_index[keep_parent[index]];
        node.first_child = node.last_child = node.next_sibling = node.previous_sibling = kUiAccessNone;
        node.child_count = 0;
        nodes[new_index[index]] = node;
    }
    tree->count = kept;
    // 5. Child links. Synthesized text joins its siblings in reading order.
    tree->focus = kUiAccessNone;
    for (u32 index = 1; index < kept; ++index) {
        auto& node = nodes[index];
        auto& parent = nodes[node.parent];
        u32 before = kUiAccessNone;
        if (node.internal & internal_synthesized) {
            for (u32 sibling = parent.first_child; sibling != kUiAccessNone; sibling = nodes[sibling].next_sibling)
                if (rect_before(node.visible, nodes[sibling].visible)) {
                    before = sibling;
                    break;
                }
        }
        if (before == kUiAccessNone) {
            node.previous_sibling = parent.last_child;
            if (parent.last_child != kUiAccessNone)
                nodes[parent.last_child].next_sibling = index;
            else
                parent.first_child = index;
            parent.last_child = index;
        } else {
            node.next_sibling = before;
            node.previous_sibling = nodes[before].previous_sibling;
            if (node.previous_sibling != kUiAccessNone)
                nodes[node.previous_sibling].next_sibling = index;
            else
                parent.first_child = index;
            nodes[before].previous_sibling = index;
        }
        ++parent.child_count;
    }
    for (u32 index = 0; index < kept; ++index)
        if (nodes[index].state & ui_access_focused)
            tree->focus = index;
    // 6. Key index; duplicates are re-keyed by position.
    for (u32 index = 0; index <= tree->key_mask; ++index)
        tree->key_index[index] = kUiAccessNone;
    for (u32 index = 0; index < kept; ++index) {
        for (u32 attempt = 0; attempt < 4; ++attempt) {
            u32 bucket = static_cast<u32>(bucket_hash(nodes[index].key)) & tree->key_mask;
            bool duplicate = false;
            while (tree->key_index[bucket] != kUiAccessNone) {
                if (nodes[tree->key_index[bucket]].key == nodes[index].key) {
                    duplicate = true;
                    break;
                }
                bucket = (bucket + 1) & tree->key_mask;
            }
            if (!duplicate) {
                tree->key_index[bucket] = index;
                break;
            }
            nodes[index].key = hash64_mix(nodes[index].key, 0x9E3779B97F4A7C15ull + index);
        }
    }
    tree->slot_used = 0;
    tree->owner_count = 0;
    tree->finished = true;
    return true;
}

u32 ui_access_find(const UiAccessTree& tree, u64 key) noexcept {
    if (!tree.finished)
        return kUiAccessNone;
    u32 bucket = static_cast<u32>(bucket_hash(key)) & tree.key_mask;
    for (u32 probe = 0; probe <= tree.key_mask; ++probe) {
        const u32 index = tree.key_index[bucket];
        if (index == kUiAccessNone)
            return kUiAccessNone;
        if (tree.nodes[index].key == key)
            return index;
        bucket = (bucket + 1) & tree.key_mask;
    }
    return kUiAccessNone;
}

bool ui_access_needs_reveal(const UiAccessNode& node) noexcept {
    if ((node.state & ui_access_offscreen) || !ui_rect_has_area(node.visible))
        return true;
    const float full = ui_rect_height(node.rectangle);
    return full > 0 && ui_rect_height(node.visible) < full * 0.5f;
}

u32 ui_access_hit_test(const UiAccessTree& tree, float x, float y) noexcept {

    if (!tree.count || !ui_rect_contains(tree.nodes[0].rectangle, x, y))
        return kUiAccessNone;
    u32 current = 0;
    for (u32 depth = 0; depth < 256; ++depth) {
        u32 found = kUiAccessNone;
        for (u32 child = tree.nodes[current].last_child; child != kUiAccessNone;
             child = tree.nodes[child].previous_sibling) {
            const auto& node = tree.nodes[child];
            if (!(node.state & ui_access_offscreen) && ui_rect_contains(node.visible, x, y)) {
                found = child;
                break;
            }
        }
        if (found == kUiAccessNone)
            return current;
        current = found;
    }
    return current;
}

Utf8View ui_access_name(const UiAccessTree& tree, u32 node) noexcept {
    return node < tree.count ? Utf8View{tree.text.data + tree.nodes[node].name_offset, tree.nodes[node].name_bytes}
                             : Utf8View{};
}

Utf8View ui_access_value(const UiAccessTree& tree, u32 node) noexcept {
    return node < tree.count ? Utf8View{tree.text.data + tree.nodes[node].value_offset, tree.nodes[node].value_bytes}
                             : Utf8View{};
}

Utf8View ui_access_help(const UiAccessTree& tree, u32 node) noexcept {
    return node < tree.count ? Utf8View{tree.text.data + tree.nodes[node].help_offset, tree.nodes[node].help_bytes}
                             : Utf8View{};
}

Utf8View ui_access_automation_id(const UiAccessTree& tree, u32 node) noexcept {
    return node < tree.count ? Utf8View{tree.text.data + tree.nodes[node].id_offset, tree.nodes[node].id_bytes}
                             : Utf8View{};
}

u32 ui_access_ancestor(const UiAccessTree& tree, u32 node, u32 role_mask) noexcept {
    if (node >= tree.count)
        return kUiAccessNone;
    for (u32 current = tree.nodes[node].parent; current != kUiAccessNone; current = tree.nodes[current].parent)
        if (role_mask & ui_access_role_bit(tree.nodes[current].role))
            return current;
    return kUiAccessNone;
}

u32 ui_access_humanize(Utf8View debug_name, Slice<char> output) noexcept {
    auto lower = [](char c) noexcept { return c >= 'a' && c <= 'z'; };
    auto upper = [](char c) noexcept { return c >= 'A' && c <= 'Z'; };
    auto digit = [](char c) noexcept { return c >= '0' && c <= '9'; };
    auto separator = [](char c) noexcept { return c == ' ' || c == '_' || c == '-'; };
    const char* in = debug_name.data;
    const u32 bytes = debug_name.bytes;
    u32 written = 0;
    bool first = true;
    for (u32 start = 0; start < bytes;) {
        if (separator(in[start])) {
            ++start;
            continue;
        }
        u32 end = start + 1;
        while (end < bytes && !separator(in[end])) {
            const char previous = in[end - 1], next = in[end];
            if (upper(next) && (lower(previous) || digit(previous)))
                break;
            // "HTTPServer": the last capital of a run starts the next word,
            // except for a plural acronym ("GIFs").
            if (upper(next) && upper(previous) && end + 1 < bytes && lower(in[end + 1]) &&
                !(in[end + 1] == 's' && (end + 2 >= bytes || !lower(in[end + 2]))))
                break;
            ++end;
        }
        u32 capitals = 0, letters = 0;
        for (u32 index = start; index < end; ++index) {
            capitals += upper(in[index]) ? 1u : 0u;
            letters += upper(in[index]) || lower(in[index]) ? 1u : 0u;
        }
        const bool plural = end - start >= 3 && in[end - 1] == 's' && capitals == letters - 1;
        const bool acronym = capitals >= 2 && (capitals == letters || plural);
        if (!first) {
            if (written >= output.count)
                break;
            output[written++] = ' ';
        }
        for (u32 index = start; index < end && written < output.count; ++index) {
            char c = in[index];
            if (index == start && !first && !acronym && upper(c))
                c = static_cast<char>(c - 'A' + 'a');
            output[written++] = c;
        }
        first = false;
        start = end;
    }
    return written;
}

bool ui_access_event_state_initialize(UiAccessEventState* state, Slice<UiAccessObservation> current,
                                      Slice<UiAccessObservation> previous) noexcept {
    if (!state || !current.data || !previous.data || current.count != previous.count || !current.count)
        return false;
    reset_value<UiAccessEventState>(state);
    state->current = current;
    state->previous = previous;
    return true;
}

bool ui_access_plan_events(UiAccessEventState* state, const UiAccessTree& tree, UiAccessEventPlan* plan) noexcept {
    if (!state || !plan || !tree.finished || !state->current.data)
        return false;
    reset_value<UiAccessEventPlan>(plan);
    plan->focus_node = kUiAccessNone;
    constexpr u16 observed_states = ui_access_checked | ui_access_mixed | ui_access_selected | ui_access_expanded |
                                    ui_access_collapsed | ui_access_focused | ui_access_live;
    constexpr u16 observed_actions = ui_access_action_toggle | ui_access_action_select | ui_access_action_value |
                                     ui_access_action_range | ui_access_action_expand | ui_access_action_text;
    u32 observed = 0;
    bool overflow = false;
    for (u32 index = 0; index < tree.count; ++index) {
        const auto& node = tree.nodes[index];
        const bool container = has_role(kContainerRoles, node.role);
        if (!container && !(node.state & observed_states) && !(node.actions & observed_actions))
            continue;
        if (observed >= state->current.count) {
            overflow = true;
            break;
        }
        UiAccessObservation entry{};
        entry.key = node.key;
        entry.state = node.state;
        entry.role = node.role;
        entry.range = node.range.value;
        if (node.actions & ui_access_action_text)
            entry.selection = (static_cast<u64>(node.placement.selection_anchor) << 32) |
                              node.placement.selection_caret;
        const Utf8View name = ui_access_name(tree, index);
        entry.name_hash = hash64_bytes(kHash64Seed, name.data, name.bytes);
        if (container) {
            u64 signature = kHash64Seed;
            for (u32 child = node.first_child; child != kUiAccessNone; child = tree.nodes[child].next_sibling)
                signature = hash64_mix(signature, tree.nodes[child].key);
            entry.value_hash = signature;
        } else {
            const Utf8View value = ui_access_value(tree, index);
            entry.value_hash = hash64_bytes(kHash64Seed, value.data, value.bytes);
        }
        state->current[observed++] = entry;
    }
    state->current_count = observed;
    heap_sort(state->current.data, observed,
              [](const UiAccessObservation& a, const UiAccessObservation& b) noexcept { return a.key < b.key; });
    const u64 focus_key = tree.focus != kUiAccessNone ? tree.nodes[tree.focus].key : 0;
    if (focus_key && focus_key != state->focus_key) {
        plan->focus_changed = true;
        plan->focus_node = tree.focus;
    }
    state->focus_key = focus_key;
    const bool primed = state->primed;
    state->primed = true;
    if (primed) {
        plan->structure_root = overflow;
        auto add_property = [&](u64 key, UiAccessProperty property, const UiAccessObservation& old) noexcept {
            if (plan->property_count >= kUiAccessPlanPropertyMaximum)
                return;
            const u32 node = ui_access_find(tree, key);
            if (node != kUiAccessNone)
                plan->properties[plan->property_count++] = {node, property, old.state, old.range};
        };
        u32 old_index = 0;
        for (u32 index = 0; index < observed; ++index) {
            const auto& entry = state->current[index];
            while (old_index < state->previous_count && state->previous[old_index].key < entry.key)
                ++old_index;
            if (old_index >= state->previous_count || state->previous[old_index].key != entry.key)
                continue;
            const auto& old = state->previous[old_index];
            if (has_role(kContainerRoles, entry.role)) {
                if (old.value_hash != entry.value_hash) {
                    if (plan->structure_count < kUiAccessPlanStructureMaximum) {
                        const u32 node = ui_access_find(tree, entry.key);
                        if (node != kUiAccessNone)
                            plan->structures[plan->structure_count++] = node;
                    } else {
                        plan->structure_root = true;
                    }
                }
            } else if (old.value_hash != entry.value_hash) {
                add_property(entry.key, UiAccessProperty::value, old);
            }
            if ((old.state ^ entry.state) & (ui_access_checked | ui_access_mixed))
                add_property(entry.key, UiAccessProperty::toggle, old);
            if ((old.state ^ entry.state) & ui_access_selected)
                add_property(entry.key, UiAccessProperty::selected, old);
            if ((old.state ^ entry.state) & (ui_access_expanded | ui_access_collapsed))
                add_property(entry.key, UiAccessProperty::expanded, old);
            if (old.range != entry.range)
                add_property(entry.key, UiAccessProperty::range, old);
            if (old.name_hash != entry.name_hash && (entry.state & (ui_access_focused | ui_access_live)))
                add_property(entry.key, UiAccessProperty::name, old);
            if (old.selection != entry.selection && (entry.state & ui_access_focused))
                add_property(entry.key, UiAccessProperty::text_selection, old);
        }
    }
    const auto spare = state->previous;
    state->previous = state->current;
    state->previous_count = state->current_count;
    state->current = spare;
    state->current_count = 0;
    return true;
}

bool ui_access_message_should_announce(UiAccessMessageCursor* cursor, u64 channel_id, u64 newest_message_id,
                                       bool authored_locally) noexcept {
    if (!cursor)
        return false;
    if (!channel_id || !newest_message_id) {
        if (cursor->channel_id != channel_id)
            *cursor = {channel_id, 0, false};
        return false;
    }
    if (!cursor->primed || cursor->channel_id != channel_id) {
        *cursor = {channel_id, newest_message_id, true};
        return false;
    }
    if (newest_message_id <= cursor->newest_message_id)
        return false;
    cursor->newest_message_id = newest_message_id;
    return !authored_locally;
}

// Text units

namespace {

enum TextClass : u8 {
    text_space,
    text_newline,
    text_word,
    text_punctuation,
};

u32 decode_scalar(Utf8View text, u32 index, u32* width) noexcept {
    const u8 lead = static_cast<u8>(text.data[index]);
    u32 count = lead < 0x80u                ? 1u
                : (lead & 0xE0u) == 0xC0u ? 2u
                : (lead & 0xF0u) == 0xE0u ? 3u
                : (lead & 0xF8u) == 0xF0u ? 4u
                                          : 1u;
    if (index + count > text.bytes)
        count = 1;
    u32 scalar = count == 1 ? lead : count == 2 ? (lead & 0x1Fu) : count == 3 ? (lead & 0x0Fu) : (lead & 0x07u);
    for (u32 i = 1; i < count; ++i) {
        const u8 next = static_cast<u8>(text.data[index + i]);
        if ((next & 0xC0u) != 0x80u) {
            *width = 1;
            return lead;
        }
        scalar = (scalar << 6) | (next & 0x3Fu);
    }
    *width = count;
    return scalar;
}

bool extends_cluster(u32 scalar) noexcept {
    return (scalar >= 0x0300u && scalar <= 0x036Fu) || (scalar >= 0x1AB0u && scalar <= 0x1AFFu) ||
           (scalar >= 0x1DC0u && scalar <= 0x1DFFu) || (scalar >= 0x20D0u && scalar <= 0x20FFu) ||
           (scalar >= 0xFE00u && scalar <= 0xFE0Fu) || (scalar >= 0xFE20u && scalar <= 0xFE2Fu) ||
           (scalar >= 0x1F3FBu && scalar <= 0x1F3FFu) || (scalar >= 0xE0020u && scalar <= 0xE007Fu) ||
           (scalar >= 0xE0100u && scalar <= 0xE01EFu);
}

bool regional_indicator(u32 scalar) noexcept {
    return scalar >= 0x1F1E6u && scalar <= 0x1F1FFu;
}

// End of the cluster starting at index; type receives its class.
u32 next_cluster(Utf8View text, u32 index, TextClass* type) noexcept {
    u32 width = 0;
    const u32 scalar = decode_scalar(text, index, &width);
    u32 next = index + width;
    if (scalar == '\r' || scalar == '\n') {
        if (scalar == '\r' && next < text.bytes && text.data[next] == '\n')
            ++next;
        *type = text_newline;
        return next;
    }
    if (scalar == ' ' || scalar == '\t' || scalar == 0x00A0u || scalar == 0x3000u ||
        (scalar >= 0x2000u && scalar <= 0x200Au))
        *type = text_space;
    else if (scalar < 0x80u)
        *type = (scalar >= '0' && scalar <= '9') || (scalar >= 'a' && scalar <= 'z') ||
                        (scalar >= 'A' && scalar <= 'Z') || scalar == '_' || scalar == 0x27u
                    ? text_word
                    : text_punctuation;
    else
        *type = text_word;
    if (regional_indicator(scalar) && next < text.bytes) {
        u32 pair = 0;
        if (regional_indicator(decode_scalar(text, next, &pair)))
            next += pair;
    }
    while (next < text.bytes) {
        u32 step = 0;
        const u32 following = decode_scalar(text, next, &step);
        if (extends_cluster(following)) {
            next += step;
            continue;
        }
        if (following == 0x200Du) {
            next += step;
            if (next < text.bytes) {
                decode_scalar(text, next, &step);
                next += step;
            }
            continue;
        }
        break;
    }
    return next;
}

bool unit_boundary(UiAccessTextUnit unit, TextClass previous, TextClass current) noexcept {
    switch (unit) {
    case UiAccessTextUnit::character:
        return true;
    case UiAccessTextUnit::word:
        if (previous == text_newline)
            return true;
        return (current == text_word || current == text_punctuation) &&
               (previous == text_space || previous != current);
    case UiAccessTextUnit::line:
    case UiAccessTextUnit::paragraph:
        return previous == text_newline;
    default:
        return false;
    }
}

constexpr u32 kNoBoundary = 0xFFFF'FFFFu;

// Yields 0, every unit boundary in increasing order, then the text end.
struct BoundaryScan {
    Utf8View text;
    UiAccessTextUnit unit;
    u32 position;
    TextClass previous;
    bool started, done;

    u32 next() noexcept {
        if (done)
            return kNoBoundary;
        if (!started) {
            started = true;
            if (!text.bytes)
                done = true;
            else
                position = next_cluster(text, 0, &previous);
            return 0;
        }
        while (position < text.bytes) {
            const u32 here = position;
            TextClass current{};
            position = next_cluster(text, here, &current);
            const bool boundary = unit_boundary(unit, previous, current);
            previous = current;
            if (boundary)
                return here;
        }
        done = true;
        return text.bytes;
    }
};

BoundaryScan boundary_scan(Utf8View text, UiAccessTextUnit unit) noexcept {
    return {text, unit, 0, text_space, false, false};
}

} // namespace

u32 ui_access_text_align(Utf8View text, u32 offset) noexcept {
    if (offset >= text.bytes)
        return text.bytes;
    auto boundaries = boundary_scan(text, UiAccessTextUnit::character);
    u32 aligned = 0;
    for (u32 boundary = boundaries.next(); boundary != kNoBoundary && boundary <= offset;
         boundary = boundaries.next())
        aligned = boundary;
    return aligned;
}

void ui_access_text_expand(Utf8View text, UiAccessTextUnit unit, u32* start, u32* end) noexcept {
    if (!start || !end)
        return;
    u32 position = ui_access_text_align(text, *start);
    auto boundaries = boundary_scan(text, unit);
    u32 first = 0, last = text.bytes;
    for (u32 boundary = boundaries.next(); boundary != kNoBoundary; boundary = boundaries.next()) {
        // The end of a non-empty text belongs to the last unit.
        if (boundary > position || (boundary == text.bytes && boundary != 0)) {
            last = boundary;
            break;
        }
        first = boundary;
    }
    *start = first;
    *end = last < first ? first : last;
}

i32 ui_access_text_move_endpoint(Utf8View text, UiAccessTextUnit unit, i32 count, u32* offset) noexcept {
    if (!offset)
        return 0;
    const u32 position = ui_access_text_align(text, *offset);
    *offset = position;
    if (count == 0)
        return 0;
    auto boundaries = boundary_scan(text, unit);
    if (count > 0) {
        i32 moved = 0;
        for (u32 boundary = boundaries.next(); boundary != kNoBoundary && moved < count; boundary = boundaries.next())
            if (boundary > position) {
                *offset = boundary;
                ++moved;
            }
        return moved;
    }
    u32 before = 0;
    for (u32 boundary = boundaries.next(); boundary != kNoBoundary && boundary < position;
         boundary = boundaries.next())
        ++before;
    const u64 wanted = static_cast<u64>(-static_cast<i64>(count));
    const u32 steps = wanted < before ? static_cast<u32>(wanted) : before;
    const u32 target = before - steps;
    auto again = boundary_scan(text, unit);
    u32 index = 0;
    for (u32 boundary = again.next(); boundary != kNoBoundary; boundary = again.next(), ++index)
        if (index == target) {
            *offset = boundary;
            break;
        }
    return -static_cast<i32>(steps);
}

i32 ui_access_text_move(Utf8View text, UiAccessTextUnit unit, i32 count, u32* start, u32* end) noexcept {
    if (!start || !end)
        return 0;
    if (*start == *end) {
        const i32 moved = ui_access_text_move_endpoint(text, unit, count, start);
        *end = *start;
        return moved;
    }
    u32 first = *start, last = *end;
    ui_access_text_expand(text, unit, &first, &last);
    i32 moved = 0;
    if (count > 0) {
        // Only unit starts qualify: the text end starts no unit.
        auto boundaries = boundary_scan(text, unit);
        const u32 origin = first;
        for (u32 boundary = boundaries.next(); boundary != kNoBoundary && moved < count; boundary = boundaries.next())
            if (boundary > origin && boundary < text.bytes) {
                first = boundary;
                ++moved;
            }
    } else if (count < 0) {
        moved = ui_access_text_move_endpoint(text, unit, count, &first);
    }
    last = first;
    ui_access_text_expand(text, unit, &first, &last);
    *start = first;
    *end = last;
    return moved;
}

// Announcements

bool ui_access_announce_admit(UiAccessAnnouncer* announcer, Utf8View text, UiAccessPoliteness politeness,
                              u64 now_ms) noexcept {
    if (!announcer || !text.bytes)
        return false;
    const u64 stamp = now_ms + 1; // zero marks an empty slot
    const u64 hash = hash64_bytes(kHash64Seed, text.data, text.bytes);
    for (u32 index = 0; index < kUiAccessAnnounceHistory; ++index)
        if (announcer->times[index] && announcer->hashes[index] == hash && stamp >= announcer->times[index] &&
            stamp - announcer->times[index] < kUiAccessAnnounceRepeatMs) {
            ++announcer->dropped;
            return false;
        }
    if (politeness == UiAccessPoliteness::polite) {
        u32 recent = 0;
        for (u32 index = 0; index < kUiAccessAnnouncePoliteBurst; ++index) {
            const u64 time = announcer->polite_times[index];
            recent += time && stamp >= time && stamp - time < kUiAccessAnnounceWindowMs ? 1u : 0u;
        }
        if (recent >= kUiAccessAnnouncePoliteBurst) {
            ++announcer->dropped;
            return false;
        }
        announcer->polite_times[announcer->polite_next] = stamp;
        announcer->polite_next = (announcer->polite_next + 1) % kUiAccessAnnouncePoliteBurst;
    }
    announcer->hashes[announcer->next] = hash;
    announcer->times[announcer->next] = stamp;
    announcer->next = (announcer->next + 1) % kUiAccessAnnounceHistory;
    ++announcer->admitted;
    return true;
}

// Throttling

void ui_access_throttle_mark_urgent(UiAccessThrottle* throttle) noexcept {
    if (throttle)
        throttle->urgent = true;
}

UiAccessThrottleDecision ui_access_throttle_frame(UiAccessThrottle* throttle, u64 now_ms, bool listening) noexcept {
    if (!throttle)
        return UiAccessThrottleDecision::idle;
    if (!listening) {
        throttle->deferred = false;
        return UiAccessThrottleDecision::idle;
    }
    if (throttle->urgent || !throttle->builds || now_ms < throttle->last_build_ms ||
        now_ms - throttle->last_build_ms >= kUiAccessRebuildIntervalMs) {
        throttle->urgent = false;
        throttle->deferred = false;
        throttle->last_build_ms = now_ms;
        ++throttle->builds;
        return UiAccessThrottleDecision::build;
    }
    throttle->deferred = true;
    ++throttle->coalesced;
    return UiAccessThrottleDecision::defer;
}

u64 ui_access_throttle_delay(const UiAccessThrottle& throttle, u64 now_ms) noexcept {
    if (!throttle.deferred || now_ms < throttle.last_build_ms)
        return 0;
    const u64 elapsed = now_ms - throttle.last_build_ms;
    return elapsed >= kUiAccessRebuildIntervalMs ? 0 : kUiAccessRebuildIntervalMs - elapsed;
}

} // namespace maicord
