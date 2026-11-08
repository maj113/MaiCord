#include "maicord/platform/atspi/accessible.hpp"

#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::atspi {
namespace {

using namespace dbus;

constexpr const char kProperties[] = "org.freedesktop.DBus.Properties";
constexpr const char kApplicationInterface[] = "org.a11y.atspi.Application";
constexpr const char kInvalidArguments[] = "org.freedesktop.DBus.Error.InvalidArgs";
constexpr const char kUnknownObject[] = "org.freedesktop.DBus.Error.UnknownObject";
constexpr const char kReadOnly[] = "org.freedesktop.DBus.Error.PropertyReadOnly";
constexpr const char kApplicationName[] = "MaiCord";

constexpr u32 kLayerWidget = 3;
constexpr u32 kLayerWindow = 7;

bool equal(const char* a, const char* b) noexcept {
    if (!a || !b)
        return false;
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

// One call being answered: who it is for, the arguments left to read and where the answer goes.
struct Request {
    Endpoint* endpoint;
    const Message* call;
    Reader args;
    Builder* out;
    Failure* failure;
    const UiAccessTree* tree;
    u32 index;                 // the node, kUiAccessNone for the application root
    const UiAccessNode* node;  // null for the application root
};

Outcome fail(Request& r, const char* name, const char* text) noexcept {
    *r.failure = {name, text};
    return Outcome::failed;
}

Outcome invalid(Request& r) noexcept {
    return fail(r, kInvalidArguments, "the arguments do not fit the method");
}

// The reply is complete: a builder that failed (a string with a NUL) or arguments left unread are errors.
Outcome done(Request& r) noexcept {
    if (r.args.failed || !builder_ok(*r.out))
        return invalid(r);
    return Outcome::replied;
}

void put_text(Builder* b, Utf8View text) noexcept {
    builder_string(b, text.bytes ? text.data : "", text.bytes);
}

void put_integer_variant(Builder* b, i32 value) noexcept {
    builder_open_variant(b, "i");
    builder_i32(b, value);
    builder_close_variant(b);
}

void put_double_variant(Builder* b, double value) noexcept {
    builder_open_variant(b, "d");
    builder_double(b, value);
    builder_close_variant(b);
}

void put_text_variant(Builder* b, Utf8View text) noexcept {
    builder_open_variant(b, "s");
    put_text(b, text);
    builder_close_variant(b);
}

void put_null_reference(Builder* b) noexcept {
    append_reference(b, "", kNullPath);
}

void put_node(Request& r, u32 index) noexcept {
    append_node_reference(r.out, *r.endpoint, r.tree->nodes[index].key);
}

void put_application(Request& r) noexcept {
    append_reference(r.out, r.endpoint->bus_name, kRootPath);
}

i32 round_to_integer(float value) noexcept {
    return static_cast<i32>(value >= 0 ? value + 0.5f : value - 0.5f);
}

struct Extent {
    i32 x, y, width, height;
};

// The node's bounds in window coordinates (the caption lies above the snapshot's origin); in parent coordinates when
// `relative` and the node has a parent.
Extent extent_of(Request& r, const UiAccessNode& node, bool relative) noexcept {
    const float top = r.endpoint->host.content_top ? r.endpoint->host.content_top(r.endpoint->host.context) : 0.0f;
    const UiRect& area = ui_rect_has_area(node.visible) ? node.visible : node.rectangle;
    Extent extent{round_to_integer(area.left), round_to_integer(area.top + top),
                  round_to_integer(ui_rect_width(area)), round_to_integer(ui_rect_height(area))};
    if (relative && node.parent != kUiAccessNone) {
        const UiAccessNode& parent = r.tree->nodes[node.parent];
        const UiRect& parent_area = ui_rect_has_area(parent.visible) ? parent.visible : parent.rectangle;
        extent.x -= round_to_integer(parent_area.left);
        extent.y -= round_to_integer(parent_area.top + top);
    }
    return extent;
}

// A point in the given coordinate type (1 window, 2 parent; 0 screen is the window's own) as a snapshot point.
UiPoint snapshot_point(Request& r, i32 x, i32 y, u32 coordinates) noexcept {
    const float top = r.endpoint->host.content_top ? r.endpoint->host.content_top(r.endpoint->host.context) : 0.0f;
    float px = static_cast<float>(x), py = static_cast<float>(y) - top;
    if (coordinates == 2 && r.node && r.node->parent != kUiAccessNone) {
        const UiAccessNode& parent = r.tree->nodes[r.node->parent];
        const UiRect& area = ui_rect_has_area(parent.visible) ? parent.visible : parent.rectangle;
        px += area.left;
        py += area.top;
    }
    return {px, py};
}

bool contains(const UiAccessNode& node, UiPoint point) noexcept {
    const UiRect& area = ui_rect_has_area(node.visible) ? node.visible : node.rectangle;
    return ui_rect_contains(area, point.x, point.y);
}

u32 child_at(Request& r, i32 position) noexcept {
    if (position < 0)
        return kUiAccessNone;
    if (!r.node)
        return position == 0 && r.tree->count ? 0u : kUiAccessNone;
    u32 child = r.node->first_child;
    for (i32 seen = 0; child != kUiAccessNone && seen < position; ++seen)
        child = r.tree->nodes[child].next_sibling;
    return child;
}

bool password_node(const UiAccessNode& node) noexcept {
    return (node.state & ui_access_password) != 0;
}

// The text a Text interface exposes: never a password's.
Utf8View text_of(Request& r) noexcept {
    if (!r.node || password_node(*r.node))
        return {};
    return ui_access_value(*r.tree, r.index);
}

Utf8View name_of(Request& r) noexcept {
    return r.node ? ui_access_name(*r.tree, r.index) : utf8_literal(kApplicationName);
}

bool editable(const UiAccessNode& node) noexcept {
    return (interfaces_for(node) & interface_editable_text) != 0;
}

// Property tables --------------------------------------------------------------------------------------------------

struct PropertyGroup {
    const char* interface;
    u32 bit;  // 0 for the Application interface of the root
    const char* const* names;
    u32 count;
};

constexpr const char* kAccessibleProperties[] = {"Name", "Description", "Parent", "ChildCount", "Locale",
                                                 "AccessibleId", "HelpText"};
constexpr const char* kApplicationProperties[] = {"ToolkitName", "Version", "AtspiVersion", "Id"};
constexpr const char* kActionProperties[] = {"NActions"};
constexpr const char* kTextProperties[] = {"CharacterCount", "CaretOffset"};
constexpr const char* kValueProperties[] = {"MinimumValue", "MaximumValue", "MinimumIncrement", "CurrentValue"};
constexpr const char* kSelectionProperties[] = {"NSelectedChildren"};

constexpr PropertyGroup kGroups[] = {
    {"org.a11y.atspi.Accessible", interface_accessible, kAccessibleProperties, 7},
    {kApplicationInterface, 0, kApplicationProperties, 4},
    {"org.a11y.atspi.Action", interface_action, kActionProperties, 1},
    {"org.a11y.atspi.Text", interface_text, kTextProperties, 2},
    {"org.a11y.atspi.Value", interface_value, kValueProperties, 4},
    {"org.a11y.atspi.Selection", interface_selection, kSelectionProperties, 1},
};

u32 interfaces_of(Request& r) noexcept {
    return r.node ? interfaces_for(*r.node) : interface_accessible;
}

const PropertyGroup* group_named(Request& r, const char* interface) noexcept {
    const u32 present = interfaces_of(r);
    for (const PropertyGroup& group : kGroups) {
        if (!equal(group.interface, interface))
            continue;
        if (group.bit ? (present & group.bit) == 0 : r.node != nullptr)
            return nullptr;
        return &group;
    }
    return nullptr;
}

u32 child_count(Request& r) noexcept {
    return r.node ? r.node->child_count : (r.tree->count ? 1u : 0u);
}

u32 selected_children(Request& r) noexcept {
    u32 selected = 0;
    if (r.node)
        for (u32 child = r.node->first_child; child != kUiAccessNone; child = r.tree->nodes[child].next_sibling)
            selected += (r.tree->nodes[child].state & ui_access_selected) ? 1u : 0u;
    return selected;
}

u32 caret_characters(Request& r) noexcept {
    return r.node ? text_character_offset(text_of(r), r.node->placement.selection_caret) : 0u;
}

// Writes the variant of one property; false when the object has no such property.
bool write_property(Request& r, const PropertyGroup& group, const char* name) noexcept {
    Builder* b = r.out;
    const UiAccessNode* node = r.node;
    if (group.bit == interface_accessible) {
        if (equal(name, "Name"))
            put_text_variant(b, name_of(r));
        else if (equal(name, "Description") || equal(name, "HelpText"))
            put_text_variant(b, node ? ui_access_help(*r.tree, r.index) : Utf8View{});
        else if (equal(name, "Parent")) {
            builder_open_variant(b, "(so)");
            if (!node)
                append_reference(b, r.endpoint->parent_name, r.endpoint->parent_name[0] ? r.endpoint->parent_path
                                                                                       : kNullPath);
            else if (node->parent == kUiAccessNone)
                put_application(r);
            else
                put_node(r, node->parent);
            builder_close_variant(b);
        } else if (equal(name, "ChildCount"))
            put_integer_variant(b, static_cast<i32>(child_count(r)));
        else if (equal(name, "Locale"))
            put_text_variant(b, utf8_literal("en_US"));
        else if (equal(name, "AccessibleId"))
            put_text_variant(b, node ? ui_access_automation_id(*r.tree, r.index) : Utf8View{});
        else
            return false;
        return true;
    }
    if (group.bit == 0) {
        if (equal(name, "ToolkitName"))
            put_text_variant(b, utf8_literal("MaiCord"));
        else if (equal(name, "Version"))
            put_text_variant(b, utf8_literal("1.0"));
        else if (equal(name, "AtspiVersion"))
            put_text_variant(b, utf8_literal("2.1"));
        else if (equal(name, "Id"))
            put_integer_variant(b, r.endpoint->id);
        else
            return false;
        return true;
    }
    if (group.bit == interface_action && equal(name, "NActions")) {
        ActionEntry entries[kActionMaximum];
        put_integer_variant(b, static_cast<i32>(actions_for(*node, entries)));
        return true;
    }
    if (group.bit == interface_text) {
        if (equal(name, "CharacterCount"))
            put_integer_variant(b, static_cast<i32>(text_character_count(text_of(r))));
        else if (equal(name, "CaretOffset"))
            put_integer_variant(b, static_cast<i32>(caret_characters(r)));
        else
            return false;
        return true;
    }
    if (group.bit == interface_value) {
        if (equal(name, "MinimumValue"))
            put_double_variant(b, node->range.minimum);
        else if (equal(name, "MaximumValue"))
            put_double_variant(b, node->range.maximum);
        else if (equal(name, "MinimumIncrement"))
            put_double_variant(b, node->range.step);
        else if (equal(name, "CurrentValue"))
            put_double_variant(b, node->range.value);
        else
            return false;
        return true;
    }
    if (group.bit == interface_selection && equal(name, "NSelectedChildren")) {
        put_integer_variant(b, static_cast<i32>(selected_children(r)));
        return true;
    }
    return false;
}

Outcome properties_call(Request& r, const char* member) noexcept {
    const char* interface = nullptr;
    const char* name = nullptr;
    if (equal(member, "Get")) {
        if (!reader_string(&r.args, &interface, nullptr) || !reader_string(&r.args, &name, nullptr))
            return invalid(r);
        const PropertyGroup* group = group_named(r, interface);
        if (!group || !write_property(r, *group, name))
            return invalid(r);
        return done(r);
    }
    if (equal(member, "GetAll")) {
        if (!reader_string(&r.args, &interface, nullptr))
            return invalid(r);
        const PropertyGroup* group = group_named(r, interface);
        builder_open_array(r.out, "{sv}");
        for (u32 i = 0; group && i < group->count; ++i) {
            builder_open_entry(r.out);
            builder_string(r.out, group->names[i]);
            write_property(r, *group, group->names[i]);
            builder_close_entry(r.out);
        }
        builder_close_array(r.out);
        return done(r);
    }
    if (equal(member, "Set")) {
        if (!reader_string(&r.args, &interface, nullptr) || !reader_string(&r.args, &name, nullptr))
            return invalid(r);
        const PropertyGroup* group = group_named(r, interface);
        if (!group)
            return invalid(r);
        const char* signature = nullptr;
        if (!reader_enter_variant(&r.args, &signature))
            return invalid(r);
        if (group->bit == 0 && equal(name, "Id") && signature && equal(signature, "i")) {
            i32 id = 0;
            if (!reader_i32(&r.args, &id))
                return invalid(r);
            r.endpoint->id = id;
            return done(r);
        }
        if (group->bit == interface_value && equal(name, "CurrentValue") && signature && equal(signature, "d")) {
            double value = 0;
            if (!reader_double(&r.args, &value))
                return invalid(r);
            if (!r.endpoint->host.act(r.endpoint->host.context, r.node->key, UiAccessAction::set_range, value, {}))
                return fail(r, "org.freedesktop.DBus.Error.Failed", "the value cannot be set");
            return done(r);
        }
        return fail(r, kReadOnly, "the property is read-only");
    }
    return Outcome::unhandled;
}

// Accessible -------------------------------------------------------------------------------------------------------

void put_attribute(Builder* b, const char* name, Utf8View value) noexcept {
    builder_open_entry(b);
    builder_string(b, name);
    put_text(b, value);
    builder_close_entry(b);
}

u32 index_in_parent(Request& r) noexcept {
    u32 index = 0;
    if (!r.node)
        return 0;
    for (u32 sibling = r.node->previous_sibling; sibling != kUiAccessNone; sibling = r.tree->nodes[sibling].previous_sibling)
        ++index;
    return index;
}

Outcome accessible_call(Request& r, const char* member) noexcept {
    Builder* b = r.out;
    const UiAccessNode* node = r.node;
    if (equal(member, "GetChildAtIndex")) {
        i32 position = 0;
        if (!reader_i32(&r.args, &position))
            return invalid(r);
        const u32 child = child_at(r, position);
        if (child == kUiAccessNone)
            put_null_reference(b);
        else
            put_node(r, child);
    } else if (equal(member, "GetChildren")) {
        builder_open_array(b, "(so)");
        if (!node && r.tree->count)
            put_node(r, 0);
        else if (node)
            for (u32 child = node->first_child; child != kUiAccessNone; child = r.tree->nodes[child].next_sibling)
                put_node(r, child);
        builder_close_array(b);
    } else if (equal(member, "GetIndexInParent")) {
        builder_i32(b, static_cast<i32>(index_in_parent(r)));
    } else if (equal(member, "GetRelationSet")) {
        builder_open_array(b, "(ua(so))");
        builder_close_array(b);
    } else if (equal(member, "GetRole")) {
        builder_u32(b, node ? role_for(*node) : static_cast<u32>(role_application));
    } else if (equal(member, "GetRoleName") || equal(member, "GetLocalizedRoleName")) {
        builder_string(b, role_name(node ? role_for(*node) : static_cast<u32>(role_application)));
    } else if (equal(member, "GetState")) {
        u32 words[2]{};
        if (node)
            states_for(*node, r.endpoint->host.window_active(r.endpoint->host.context), words);
        else
            words[0] = (1u << state_enabled) | (1u << state_sensitive) | (1u << state_showing) | (1u << state_visible);
        builder_open_array(b, "u");
        builder_u32(b, words[0]);
        builder_u32(b, words[1]);
        builder_close_array(b);
    } else if (equal(member, "GetAttributes")) {
        builder_open_array(b, "{ss}");
        put_attribute(b, "toolkit", utf8_literal("MaiCord"));
        if (node) {
            const Utf8View id = ui_access_automation_id(*r.tree, r.index);
            if (id.bytes)
                put_attribute(b, "id", id);
            char digits[12];
            if (node->placement.position)
                put_attribute(b, "posinset", {digits, decimal_write(digits, node->placement.position)});
            if (node->placement.set_size)
                put_attribute(b, "setsize", {digits, decimal_write(digits, node->placement.set_size)});
            if (node->state & ui_access_live)
                put_attribute(b, "live", utf8_literal("polite"));
        }
        builder_close_array(b);
    } else if (equal(member, "GetApplication")) {
        put_application(r);
    } else if (equal(member, "GetInterfaces")) {
        const u32 present = interfaces_of(r);
        builder_open_array(b, "s");
        for (u32 bit = 1; bit <= interface_selection; bit <<= 1)
            if (present & bit)
                builder_string(b, interface_name(bit));
        if (!node)
            builder_string(b, kApplicationInterface);
        builder_close_array(b);
    } else {
        return Outcome::unhandled;
    }
    return done(r);
}

Outcome application_call(Request& r, const char* member) noexcept {
    if (equal(member, "GetLocale")) {
        u32 category = 0;
        if (!reader_u32(&r.args, &category))
            return invalid(r);
        builder_string(r.out, "en_US");
    } else if (equal(member, "GetApplicationBusAddress")) {
        builder_string(r.out, "");
    } else {
        return Outcome::unhandled;
    }
    return done(r);
}

// Component ---------------------------------------------------------------------------------------------------------

Outcome component_call(Request& r, const char* member) noexcept {
    Builder* b = r.out;
    const UiAccessNode& node = *r.node;
    if (equal(member, "Contains") || equal(member, "GetAccessibleAtPoint")) {
        i32 x = 0, y = 0;
        u32 coordinates = 0;
        if (!reader_i32(&r.args, &x) || !reader_i32(&r.args, &y) || !reader_u32(&r.args, &coordinates))
            return invalid(r);
        const UiPoint point = snapshot_point(r, x, y, coordinates);
        if (equal(member, "Contains")) {
            builder_bool(b, contains(node, point));
        } else {
            u32 hit = ui_access_hit_test(*r.tree, point.x, point.y);
            // Only a descendant (or the node itself) counts.
            u32 walk = hit;
            while (walk != kUiAccessNone && walk != r.index)
                walk = r.tree->nodes[walk].parent;
            if (walk == kUiAccessNone)
                put_null_reference(b);
            else
                put_node(r, hit);
        }
    } else if (equal(member, "GetExtents")) {
        u32 coordinates = 0;
        if (!reader_u32(&r.args, &coordinates))
            return invalid(r);
        const Extent e = extent_of(r, node, coordinates == 2);
        builder_open_struct(b, "(iiii)");
        builder_i32(b, e.x);
        builder_i32(b, e.y);
        builder_i32(b, e.width);
        builder_i32(b, e.height);
        builder_close_struct(b);
    } else if (equal(member, "GetPosition")) {
        u32 coordinates = 0;
        if (!reader_u32(&r.args, &coordinates))
            return invalid(r);
        const Extent e = extent_of(r, node, coordinates == 2);
        builder_i32(b, e.x);
        builder_i32(b, e.y);
    } else if (equal(member, "GetSize")) {
        const Extent e = extent_of(r, node, false);
        builder_i32(b, e.width);
        builder_i32(b, e.height);
    } else if (equal(member, "GetLayer")) {
        builder_u32(b, node.role == UiAccessRole::window ? kLayerWindow : kLayerWidget);
    } else if (equal(member, "GetMDIZOrder")) {
        builder_i16(b, 0);
    } else if (equal(member, "GetAlpha")) {
        builder_double(b, 1.0);
    } else if (equal(member, "GrabFocus")) {
        const bool focusable = (node.actions & ui_access_action_focus) != 0;
        builder_bool(b, focusable && r.endpoint->host.act(r.endpoint->host.context, node.key, UiAccessAction::focus, 0, {}));
    } else if (equal(member, "ScrollTo")) {
        u32 type = 0;
        if (!reader_u32(&r.args, &type))
            return invalid(r);
        builder_bool(b, (node.actions & ui_access_action_scroll_item) &&
                            r.endpoint->host.act(r.endpoint->host.context, node.key, UiAccessAction::scroll_into_view, 0, {}));
    } else if (equal(member, "ScrollToPoint")) {
        u32 type = 0;
        i32 x = 0, y = 0;
        if (!reader_u32(&r.args, &type) || !reader_i32(&r.args, &x) || !reader_i32(&r.args, &y))
            return invalid(r);
        builder_bool(b, false);
    } else if (equal(member, "SetExtents") || equal(member, "SetPosition") || equal(member, "SetSize")) {
        // The window is the compositor's; nothing inside it is placed by a client of this interface.
        builder_bool(b, false);
    } else {
        return Outcome::unhandled;
    }
    return done(r);
}

// Action ------------------------------------------------------------------------------------------------------------

Outcome action_call(Request& r, const char* member) noexcept {
    Builder* b = r.out;
    ActionEntry entries[kActionMaximum];
    const u32 count = actions_for(*r.node, entries);
    if (equal(member, "GetActions")) {
        builder_open_array(b, "(sss)");
        for (u32 i = 0; i < count; ++i) {
            builder_open_struct(b, "(sss)");
            builder_string(b, entries[i].name);
            builder_string(b, entries[i].description);
            builder_string(b, "");
            builder_close_struct(b);
        }
        builder_close_array(b);
        return done(r);
    }
    i32 position = 0;
    if (!reader_i32(&r.args, &position))
        return Outcome::unhandled;
    const bool known = position >= 0 && static_cast<u32>(position) < count;
    if (equal(member, "DoAction")) {
        builder_bool(b, known && r.endpoint->host.act(r.endpoint->host.context, r.node->key,
                                                      entries[position].action, 0, {}));
    } else if (equal(member, "GetName") || equal(member, "GetLocalizedName")) {
        builder_string(b, known ? entries[position].name : "");
    } else if (equal(member, "GetDescription")) {
        builder_string(b, known ? entries[position].description : "");
    } else if (equal(member, "GetKeyBinding")) {
        builder_string(b, "");
    } else {
        return Outcome::unhandled;
    }
    return done(r);
}

// Text --------------------------------------------------------------------------------------------------------------

// Text's extents are four out arguments (Component's are one struct).
void put_extents(Builder* b, const Extent& e) noexcept {
    builder_i32(b, e.x);
    builder_i32(b, e.y);
    builder_i32(b, e.width);
    builder_i32(b, e.height);
}

// The bounds of bytes [start, end) of the node's text, in window coordinates; the node's own bounds when the host
// cannot tell.
Extent text_extent(Request& r, u32 start, u32 end) noexcept {
    const HostCallbacks& host = r.endpoint->host;
    UiRect lines[8];
    const u32 count = host.text_bounds ? host.text_bounds(host.context, r.node->key, start, end, lines, 8) : 0u;
    if (!count)
        return extent_of(r, *r.node, false);
    UiRect all = lines[0];
    for (u32 i = 1; i < count; ++i) {
        all.left = minimum(all.left, lines[i].left);
        all.top = minimum(all.top, lines[i].top);
        all.right = maximum(all.right, lines[i].right);
        all.bottom = maximum(all.bottom, lines[i].bottom);
    }
    const float top = host.content_top ? host.content_top(host.context) : 0.0f;
    return {round_to_integer(all.left), round_to_integer(all.top + top), round_to_integer(ui_rect_width(all)),
            round_to_integer(ui_rect_height(all))};
}

// Clamps a pair of character offsets (-1 is the end) to the text and orders them; byte offsets out.
void character_range(Utf8View text, i32 first, i32 last, u32* start, u32* end) noexcept {
    const u32 characters = text_character_count(text);
    u32 a = first < 0 ? characters : minimum(static_cast<u32>(first), characters);
    u32 z = last < 0 ? characters : minimum(static_cast<u32>(last), characters);
    if (z < a)
        a = z;
    *start = text_byte_offset(text, a);
    *end = text_byte_offset(text, z);
}

// The three out arguments of GetStringAtOffset and the older GetTextAtOffset calls: the text and its character range.
void put_span(Request& r, Utf8View text, u32 start, u32 end) noexcept {
    put_text(r.out, {text.data + start, end - start});
    builder_i32(r.out, static_cast<i32>(text_character_offset(text, start)));
    builder_i32(r.out, static_cast<i32>(text_character_offset(text, end)));
}

Outcome select_characters(Request& r, i32 first, i32 last) noexcept {
    const Utf8View text = text_of(r);
    u32 start = 0, end = 0;
    character_range(text, first, last, &start, &end);
    const bool selected = r.endpoint->host.act(r.endpoint->host.context, r.node->key, UiAccessAction::select_text,
                                               ui_access_text_selection_pack(start, end), {});
    builder_bool(r.out, selected);
    return done(r);
}

Outcome text_call(Request& r, const char* member) noexcept {
    Builder* b = r.out;
    const UiAccessNode& node = *r.node;
    const Utf8View text = text_of(r);
    if (equal(member, "GetText")) {
        i32 first = 0, last = 0;
        if (!reader_i32(&r.args, &first) || !reader_i32(&r.args, &last))
            return invalid(r);
        u32 start = 0, end = 0;
        character_range(text, first, last, &start, &end);
        put_text(b, {text.data + start, end - start});
    } else if (equal(member, "SetCaretOffset")) {
        i32 offset = 0;
        if (!reader_i32(&r.args, &offset))
            return invalid(r);
        return select_characters(r, offset, offset);
    } else if (equal(member, "GetCharacterAtOffset")) {
        i32 offset = 0;
        if (!reader_i32(&r.args, &offset))
            return invalid(r);
        builder_i32(b, offset < 0 ? -1 : static_cast<i32>(text_scalar_at(text, text_byte_offset(text, static_cast<u32>(offset)))));
    } else if (equal(member, "GetStringAtOffset")) {
        i32 offset = 0;
        u32 granularity = 0;
        if (!reader_i32(&r.args, &offset) || !reader_u32(&r.args, &granularity) || offset < 0 || granularity > 4)
            return invalid(r);
        u32 start = 0, end = 0;
        text_span(text, text_byte_offset(text, static_cast<u32>(offset)), static_cast<TextGranularity>(granularity), 0,
                  &start, &end);
        put_span(r, text, start, end);
    } else if (equal(member, "GetTextAtOffset") || equal(member, "GetTextBeforeOffset") ||
               equal(member, "GetTextAfterOffset")) {
        // The boundary types of the older calls: character, word start and end, sentence start and end, line start and end.
        i32 offset = 0;
        u32 boundary = 0;
        if (!reader_i32(&r.args, &offset) || !reader_u32(&r.args, &boundary) || offset < 0 || boundary > 6)
            return invalid(r);
        const TextGranularity granularity = boundary == 0   ? TextGranularity::character
                                            : boundary <= 2 ? TextGranularity::word
                                            : boundary <= 4 ? TextGranularity::sentence
                                                            : TextGranularity::line;
        const i32 direction = equal(member, "GetTextAtOffset") ? 0 : equal(member, "GetTextBeforeOffset") ? -1 : 1;
        u32 start = 0, end = 0;
        text_span(text, text_byte_offset(text, static_cast<u32>(offset)), granularity, direction, &start, &end);
        put_span(r, text, start, end);
    } else if (equal(member, "GetAttributeValue")) {
        i32 offset = 0;
        const char* name = nullptr;
        if (!reader_i32(&r.args, &offset) || !reader_string(&r.args, &name, nullptr))
            return invalid(r);
        builder_string(b, "");
    } else if (equal(member, "GetAttributes") || equal(member, "GetAttributeRun")) {
        i32 offset = 0;
        if (!reader_i32(&r.args, &offset))
            return invalid(r);
        if (equal(member, "GetAttributeRun")) {
            bool include_defaults = false;
            if (!reader_bool(&r.args, &include_defaults))
                return invalid(r);
        }
        // Three out arguments: the attributes, and the range they hold over.
        builder_open_array(b, "{ss}");
        builder_close_array(b);
        builder_i32(b, 0);
        builder_i32(b, static_cast<i32>(text_character_count(text)));
    } else if (equal(member, "GetDefaultAttributes") || equal(member, "GetDefaultAttributeSet")) {
        builder_open_array(b, "{ss}");
        builder_close_array(b);
    } else if (equal(member, "GetCharacterExtents")) {
        i32 offset = 0;
        u32 coordinates = 0;
        if (!reader_i32(&r.args, &offset) || !reader_u32(&r.args, &coordinates) || offset < 0)
            return invalid(r);
        const u32 start = text_byte_offset(text, static_cast<u32>(offset));
        u32 end = start;
        u32 unused = start;
        ui_access_text_expand(text, UiAccessTextUnit::character, &unused, &end);
        put_extents(b, text_extent(r, start, end));
    } else if (equal(member, "GetRangeExtents")) {
        i32 first = 0, last = 0;
        u32 coordinates = 0;
        if (!reader_i32(&r.args, &first) || !reader_i32(&r.args, &last) || !reader_u32(&r.args, &coordinates))
            return invalid(r);
        u32 start = 0, end = 0;
        character_range(text, first, last, &start, &end);
        put_extents(b, text_extent(r, start, end));
    } else if (equal(member, "GetOffsetAtPoint")) {
        i32 x = 0, y = 0;
        u32 coordinates = 0;
        if (!reader_i32(&r.args, &x) || !reader_i32(&r.args, &y) || !reader_u32(&r.args, &coordinates))
            return invalid(r);
        builder_i32(b, -1);
    } else if (equal(member, "GetBoundedRanges")) {
        i32 x = 0, y = 0, width = 0, height = 0;
        u32 coordinates = 0, x_clip = 0, y_clip = 0;
        if (!reader_i32(&r.args, &x) || !reader_i32(&r.args, &y) || !reader_i32(&r.args, &width) ||
            !reader_i32(&r.args, &height) || !reader_u32(&r.args, &coordinates) || !reader_u32(&r.args, &x_clip) ||
            !reader_u32(&r.args, &y_clip))
            return invalid(r);
        builder_open_array(b, "(iisv)");
        builder_close_array(b);
    } else if (equal(member, "GetNSelections")) {
        builder_i32(b, node.placement.selection_anchor != node.placement.selection_caret ? 1 : 0);
    } else if (equal(member, "GetSelection")) {
        i32 number = 0;
        if (!reader_i32(&r.args, &number))
            return invalid(r);
        if (number != 0 || node.placement.selection_anchor == node.placement.selection_caret)
            return fail(r, kInvalidArguments, "no such selection");
        const u32 low = minimum(node.placement.selection_anchor, node.placement.selection_caret);
        const u32 high = maximum(node.placement.selection_anchor, node.placement.selection_caret);
        builder_i32(b, static_cast<i32>(text_character_offset(text, low)));
        builder_i32(b, static_cast<i32>(text_character_offset(text, high)));
    } else if (equal(member, "AddSelection") || equal(member, "SetSelection")) {
        i32 number = 0, first = 0, last = 0;
        if (equal(member, "SetSelection") && !reader_i32(&r.args, &number))
            return invalid(r);
        if (!reader_i32(&r.args, &first) || !reader_i32(&r.args, &last))
            return invalid(r);
        if (number != 0) {
            builder_bool(b, false);
            return done(r);
        }
        return select_characters(r, first, last);
    } else if (equal(member, "RemoveSelection")) {
        i32 number = 0;
        if (!reader_i32(&r.args, &number))
            return invalid(r);
        if (number != 0)
            builder_bool(b, false);
        else
            return select_characters(r, static_cast<i32>(caret_characters(r)), static_cast<i32>(caret_characters(r)));
    } else if (equal(member, "ScrollSubstringTo") || equal(member, "ScrollSubstringToPoint")) {
        builder_bool(b, false);
        // the arguments are not used: nothing scrolls inside a text node
    } else {
        return Outcome::unhandled;
    }
    return done(r);
}

// EditableText ------------------------------------------------------------------------------------------------------

Outcome set_contents(Request& r, Utf8View text) noexcept {
    builder_bool(r.out, editable(*r.node) &&
                            r.endpoint->host.act(r.endpoint->host.context, r.node->key, UiAccessAction::set_value, 0, text));
    return done(r);
}

Outcome editable_text_call(Request& r, const char* member) noexcept {
    const Utf8View current = text_of(r);
    if (equal(member, "SetTextContents")) {
        const char* text = nullptr;
        u32 length = 0;
        if (!reader_string(&r.args, &text, &length))
            return invalid(r);
        return set_contents(r, {text, length});
    }
    char combined[kUiAccessValueMaximumBytes];
    if (equal(member, "InsertText")) {
        i32 position = 0, length = 0;
        const char* text = nullptr;
        u32 bytes = 0;
        if (!reader_i32(&r.args, &position) || !reader_string(&r.args, &text, &bytes) || !reader_i32(&r.args, &length) ||
            position < 0)
            return invalid(r);
        if (length >= 0 && static_cast<u32>(length) < bytes) {
            bytes = static_cast<u32>(length);
            while (bytes && (static_cast<u8>(text[bytes]) & 0xC0u) == 0x80u)
                --bytes;
        }
        const u32 at = text_byte_offset(current, static_cast<u32>(position));
        if (current.bytes + bytes > sizeof(combined)) {
            builder_bool(r.out, false);
            return done(r);
        }
        copy_bytes(combined, current.data, at);
        copy_bytes(combined + at, text, bytes);
        copy_bytes(combined + at + bytes, current.data + at, current.bytes - at);
        return set_contents(r, {combined, current.bytes + bytes});
    }
    if (equal(member, "DeleteText")) {
        i32 first = 0, last = 0;
        if (!reader_i32(&r.args, &first) || !reader_i32(&r.args, &last))
            return invalid(r);
        u32 start = 0, end = 0;
        character_range(current, first, last, &start, &end);
        copy_bytes(combined, current.data, start);
        copy_bytes(combined + start, current.data + end, current.bytes - end);
        return set_contents(r, {combined, current.bytes - (end - start)});
    }
    if (equal(member, "CopyText") || equal(member, "CutText") || equal(member, "PasteText")) {
        // The clipboard belongs to the window, not to a screen reader's request.
        builder_bool(r.out, false);
        return done(r);
    }
    return Outcome::unhandled;
}

// Value and Selection -----------------------------------------------------------------------------------------------

Outcome selection_call(Request& r, const char* member) noexcept {
    Builder* b = r.out;
    i32 position = 0;
    if (equal(member, "GetSelectedChild")) {
        if (!reader_i32(&r.args, &position))
            return invalid(r);
        i32 seen = 0;
        u32 found = kUiAccessNone;
        for (u32 child = r.node->first_child; child != kUiAccessNone && found == kUiAccessNone;
             child = r.tree->nodes[child].next_sibling)
            if ((r.tree->nodes[child].state & ui_access_selected) && seen++ == position)
                found = child;
        if (found == kUiAccessNone)
            put_null_reference(b);
        else
            put_node(r, found);
    } else if (equal(member, "SelectChild") || equal(member, "IsChildSelected") || equal(member, "DeselectChild")) {
        if (!reader_i32(&r.args, &position))
            return invalid(r);
        const u32 child = child_at(r, position);
        if (equal(member, "IsChildSelected"))
            builder_bool(b, child != kUiAccessNone && (r.tree->nodes[child].state & ui_access_selected));
        else if (equal(member, "SelectChild"))
            builder_bool(b, child != kUiAccessNone &&
                                r.endpoint->host.act(r.endpoint->host.context, r.tree->nodes[child].key,
                                                     UiAccessAction::select, 0, {}));
        else
            builder_bool(b, false);
    } else if (equal(member, "DeselectSelectedChild")) {
        if (!reader_i32(&r.args, &position))
            return invalid(r);
        builder_bool(b, false);
    } else if (equal(member, "SelectAll") || equal(member, "ClearSelection")) {
        builder_bool(b, false);
    } else {
        return Outcome::unhandled;
    }
    return done(r);
}

} // namespace

void append_reference(Builder* builder, const char* name, const char* path) noexcept {
    builder_open_struct(builder, "(so)");
    builder_string(builder, name);
    builder_object_path(builder, path);
    builder_close_struct(builder);
}

void append_node_reference(Builder* builder, const Endpoint& endpoint, u64 key) noexcept {
    char path[kPathBytes];
    path_for_key(key, path);
    append_reference(builder, endpoint.bus_name, path);
}

Outcome answer_call(Endpoint* endpoint, const Message& call, Builder* reply, Failure* failure) noexcept {
    if (!call.path || !call.interface || !call.member)
        return Outcome::unhandled;
    // Clients still ask for the object cache once: an empty one makes them query objects as they meet them.
    if (equal(call.path, "/org/a11y/atspi/cache") && equal(call.interface, "org.a11y.atspi.Cache") &&
        equal(call.member, "GetItems")) {
        builder_open_array(reply, "((so)(so)(so)iiassusau)");
        builder_close_array(reply);
        return builder_ok(*reply) ? Outcome::replied : Outcome::unhandled;
    }
    Request r{endpoint, &call, {}, reply, failure, nullptr, kUiAccessNone, nullptr};
    const bool root = is_root_path(call.path);
    u64 key = 0;
    if (!root && !key_from_path(call.path, &key))
        return Outcome::unhandled;
    r.tree = endpoint->host.tree(endpoint->host.context);
    if (!r.tree || !r.tree->finished || !r.tree->count)
        return fail(r, kUnknownObject, "the window has no accessible content");
    if (!root) {
        r.index = ui_access_find(*r.tree, key);
        if (r.index == kUiAccessNone)
            return fail(r, kUnknownObject, "the object is gone");
        r.node = &r.tree->nodes[r.index];
    }
    // The registry asks about the application as it embeds it; a real client is what makes events worth sending.
    if (!call.sender || !equal(call.sender, endpoint->registry_sender))
        endpoint->queried = true;
    message_reader(call, &r.args);
    const char* interface = call.interface;
    const char* member = call.member;
    if (equal(interface, kProperties))
        return properties_call(r, member);
    const u32 present = interfaces_of(r);
    if (equal(interface, "org.a11y.atspi.Accessible"))
        return accessible_call(r, member);
    if (equal(interface, kApplicationInterface))
        return !r.node ? application_call(r, member) : Outcome::unhandled;
    if (!r.node)
        return Outcome::unhandled;
    if (equal(interface, "org.a11y.atspi.Component"))
        return component_call(r, member);
    if (equal(interface, "org.a11y.atspi.Action"))
        return (present & interface_action) ? action_call(r, member) : Outcome::unhandled;
    if (equal(interface, "org.a11y.atspi.Text"))
        return (present & interface_text) ? text_call(r, member) : Outcome::unhandled;
    if (equal(interface, "org.a11y.atspi.EditableText"))
        return (present & interface_editable_text) ? editable_text_call(r, member) : Outcome::unhandled;
    if (equal(interface, "org.a11y.atspi.Selection"))
        return (present & interface_selection) ? selection_call(r, member) : Outcome::unhandled;
    return Outcome::unhandled;
}

} // namespace maicord::atspi
