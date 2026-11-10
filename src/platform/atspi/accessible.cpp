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

// The methods and properties of each interface, as an enumeration and a pool of their names in one list: the
// enumerators are the positions in the pool, and a call is found by name once, then switched on.
#define ATSPI_ENUMERATOR(name) name,
#define ATSPI_NAME(name) #name "\0"

// The position of `name` among NUL-terminated names end to end (an empty one ends them), or ~0u.
u32 name_index(const char* names, const char* name) noexcept {
    for (u32 index = 0; *names; ++index, names += c_string_length(names) + 1)
        if (equal(names, name))
            return index;
    return ~0u;
}

const char* name_at(const char* names, u32 index) noexcept {
    for (; index; --index)
        names += c_string_length(names) + 1;
    return names;
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

// Reads the arguments `types` names ('i' an i32, 'u' a u32) into `words` in order; false when one is missing or of
// another type, as the typed readers say.
bool take(Request& r, const char* types, u32 (&words)[8]) noexcept {
    for (u32 index = 0; types[index]; ++index)
        if (!(types[index] == 'i' ? reader_i32(&r.args, reinterpret_cast<i32*>(&words[index]))
                                  : reader_u32(&r.args, &words[index])))
            return false;
    return true;
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

// The area a node covers: what is visible of it, else its rectangle.
const UiRect& area_of(const UiAccessNode& node) noexcept {
    return ui_rect_has_area(node.visible) ? node.visible : node.rectangle;
}

// How far the caption lies above the snapshot's origin.
float content_top(const Endpoint& endpoint) noexcept {
    return endpoint.host.content_top ? endpoint.host.content_top(endpoint.host.context) : 0.0f;
}

// The node's bounds in window coordinates (the caption lies above the snapshot's origin); in parent coordinates when
// `relative` and the node has a parent.
Extent extent_of(Request& r, const UiAccessNode& node, bool relative) noexcept {
    const float top = content_top(*r.endpoint);
    const UiRect& area = area_of(node);
    Extent extent{round_to_integer(area.left), round_to_integer(area.top + top),
                  round_to_integer(ui_rect_width(area)), round_to_integer(ui_rect_height(area))};
    if (relative && node.parent != kUiAccessNone) {
        const UiRect& parent_area = area_of(r.tree->nodes[node.parent]);
        extent.x -= round_to_integer(parent_area.left);
        extent.y -= round_to_integer(parent_area.top + top);
    }
    return extent;
}

// A point in the given coordinate type (1 window, 2 parent; 0 screen is the window's own) as a snapshot point.
UiPoint snapshot_point(Request& r, i32 x, i32 y, u32 coordinates) noexcept {
    float px = static_cast<float>(x), py = static_cast<float>(y) - content_top(*r.endpoint);
    if (coordinates == 2 && r.node && r.node->parent != kUiAccessNone) {
        const UiRect& area = area_of(r.tree->nodes[r.node->parent]);
        px += area.left;
        py += area.top;
    }
    return {px, py};
}

bool contains(const UiAccessNode& node, UiPoint point) noexcept {
    return ui_rect_contains(area_of(node), point.x, point.y);
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

#define ATSPI_PROPERTIES(M)                                                                                          \
    M(Name) M(Description) M(Parent) M(ChildCount) M(Locale) M(AccessibleId) M(HelpText)                              \
    M(ToolkitName) M(Version) M(AtspiVersion) M(Id)                                                                    \
    M(NActions)                                                                                                        \
    M(CharacterCount) M(CaretOffset)                                                                                   \
    M(MinimumValue) M(MaximumValue) M(MinimumIncrement) M(CurrentValue)                                                \
    M(NSelectedChildren)

enum class Property : u32 { ATSPI_PROPERTIES(ATSPI_ENUMERATOR) };
constexpr char kPropertyNames[] = ATSPI_PROPERTIES(ATSPI_NAME);

// The properties of an interface are the `count` from `first` in the list above.
struct PropertyGroup {
    u32 bit;  // 0 for the Application interface of the root
    u32 first;
    u32 count;
};

constexpr PropertyGroup kGroups[] = {
    {interface_accessible, static_cast<u32>(Property::Name), 7},
    {0, static_cast<u32>(Property::ToolkitName), 4},
    {interface_action, static_cast<u32>(Property::NActions), 1},
    {interface_text, static_cast<u32>(Property::CharacterCount), 2},
    {interface_value, static_cast<u32>(Property::MinimumValue), 4},
    {interface_selection, static_cast<u32>(Property::NSelectedChildren), 1},
};

u32 interfaces_of(Request& r) noexcept {
    return r.node ? interfaces_for(*r.node) : interface_accessible;
}

const PropertyGroup* group_named(Request& r, const char* interface) noexcept {
    const u32 present = interfaces_of(r);
    for (const PropertyGroup& group : kGroups) {
        if (!equal(group.bit ? interface_name(group.bit) : kApplicationInterface, interface))
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

// Writes the variant of one property of a group the object has.
void write_property(Request& r, Property property) noexcept {
    Builder* b = r.out;
    const UiAccessNode* node = r.node;
    using enum Property;
    switch (property) {
    case Name:
        put_text_variant(b, name_of(r));
        break;
    case Description:
    case HelpText:
        put_text_variant(b, node ? ui_access_help(*r.tree, r.index) : Utf8View{});
        break;
    case Parent:
        builder_open_variant(b, "(so)");
        if (!node)
            append_reference(b, r.endpoint->parent_name, r.endpoint->parent_name[0] ? r.endpoint->parent_path
                                                                                    : kNullPath);
        else if (node->parent == kUiAccessNone)
            put_application(r);
        else
            put_node(r, node->parent);
        builder_close_variant(b);
        break;
    case ChildCount:
        put_integer_variant(b, static_cast<i32>(child_count(r)));
        break;
    case Locale:
        put_text_variant(b, utf8_literal("en_US"));
        break;
    case AccessibleId:
        put_text_variant(b, node ? ui_access_automation_id(*r.tree, r.index) : Utf8View{});
        break;
    case ToolkitName:
        put_text_variant(b, utf8_literal("MaiCord"));
        break;
    case Version:
        put_text_variant(b, utf8_literal("1.0"));
        break;
    case AtspiVersion:
        put_text_variant(b, utf8_literal("2.1"));
        break;
    case Id:
        put_integer_variant(b, r.endpoint->id);
        break;
    case NActions: {
        ActionEntry entries[kActionMaximum];
        put_integer_variant(b, static_cast<i32>(actions_for(*node, entries)));
        break;
    }
    case CharacterCount:
        put_integer_variant(b, static_cast<i32>(text_character_count(text_of(r))));
        break;
    case CaretOffset:
        put_integer_variant(b, static_cast<i32>(caret_characters(r)));
        break;
    case MinimumValue:
        put_double_variant(b, node->range.minimum);
        break;
    case MaximumValue:
        put_double_variant(b, node->range.maximum);
        break;
    case MinimumIncrement:
        put_double_variant(b, node->range.step);
        break;
    case CurrentValue:
        put_double_variant(b, node->range.value);
        break;
    case NSelectedChildren:
        put_integer_variant(b, static_cast<i32>(selected_children(r)));
        break;
    }
}

Outcome properties_call(Request& r, const char* member) noexcept {
    const char* interface = nullptr;
    const char* name = nullptr;
    if (equal(member, "Get")) {
        if (!reader_string(&r.args, &interface, nullptr) || !reader_string(&r.args, &name, nullptr))
            return invalid(r);
        const PropertyGroup* group = group_named(r, interface);
        const u32 property = name_index(kPropertyNames, name);
        // Not found is ~0u: one unsigned compare also rejects a property of another interface.
        if (!group || property - group->first >= group->count)
            return invalid(r);
        write_property(r, static_cast<Property>(property));
        return done(r);
    }
    if (equal(member, "GetAll")) {
        if (!reader_string(&r.args, &interface, nullptr))
            return invalid(r);
        const PropertyGroup* group = group_named(r, interface);
        builder_open_array(r.out, "{sv}");
        for (u32 i = 0; group && i < group->count; ++i) {
            builder_open_entry(r.out);
            builder_string(r.out, name_at(kPropertyNames, group->first + i));
            write_property(r, static_cast<Property>(group->first + i));
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
        const u32 property = name_index(kPropertyNames, name);
        if (group->bit == 0 && property == static_cast<u32>(Property::Id) && signature && equal(signature, "i")) {
            i32 id = 0;
            if (!reader_i32(&r.args, &id))
                return invalid(r);
            r.endpoint->id = id;
            return done(r);
        }
        if (group->bit == interface_value && property == static_cast<u32>(Property::CurrentValue) && signature &&
            equal(signature, "d")) {
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

#define ATSPI_ACCESSIBLE_METHODS(M)                                                                                  \
    M(GetChildAtIndex) M(GetChildren) M(GetIndexInParent) M(GetRelationSet) M(GetRole) M(GetRoleName)                 \
    M(GetLocalizedRoleName) M(GetState) M(GetAttributes) M(GetApplication) M(GetInterfaces)
enum class AccessibleMethod : u32 { ATSPI_ACCESSIBLE_METHODS(ATSPI_ENUMERATOR) };
constexpr char kAccessibleMethods[] = ATSPI_ACCESSIBLE_METHODS(ATSPI_NAME);

Outcome accessible_call(Request& r, const char* member) noexcept {
    Builder* b = r.out;
    const UiAccessNode* node = r.node;
    u32 a[8];
    using enum AccessibleMethod;
    switch (static_cast<AccessibleMethod>(name_index(kAccessibleMethods, member))) {
    case GetChildAtIndex: {
        if (!take(r, "i", a))
            return invalid(r);
        const u32 child = child_at(r, static_cast<i32>(a[0]));
        if (child == kUiAccessNone)
            put_null_reference(b);
        else
            put_node(r, child);
        break;
    }
    case GetChildren:
        builder_open_array(b, "(so)");
        if (!node && r.tree->count)
            put_node(r, 0);
        else if (node)
            for (u32 child = node->first_child; child != kUiAccessNone; child = r.tree->nodes[child].next_sibling)
                put_node(r, child);
        builder_close_array(b);
        break;
    case GetIndexInParent:
        builder_i32(b, static_cast<i32>(index_in_parent(r)));
        break;
    case GetRelationSet:
        builder_open_array(b, "(ua(so))");
        builder_close_array(b);
        break;
    case GetRole:
        builder_u32(b, node ? role_for(*node) : static_cast<u32>(role_application));
        break;
    case GetRoleName:
    case GetLocalizedRoleName:
        builder_string(b, role_name(node ? role_for(*node) : static_cast<u32>(role_application)));
        break;
    case GetState: {
        u32 words[2]{};
        if (node)
            states_for(*node, r.endpoint->host.window_active(r.endpoint->host.context), words);
        else
            words[0] = (1u << state_enabled) | (1u << state_sensitive) | (1u << state_showing) | (1u << state_visible);
        builder_open_array(b, "u");
        builder_u32(b, words[0]);
        builder_u32(b, words[1]);
        builder_close_array(b);
        break;
    }
    case GetAttributes:
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
        break;
    case GetApplication:
        put_application(r);
        break;
    case GetInterfaces: {
        const u32 present = interfaces_of(r);
        builder_open_array(b, "s");
        for (u32 bit = 1; bit <= interface_selection; bit <<= 1)
            if (present & bit)
                builder_string(b, interface_name(bit));
        if (!node)
            builder_string(b, kApplicationInterface);
        builder_close_array(b);
        break;
    }
    default:
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

#define ATSPI_COMPONENT_METHODS(M)                                                                                   \
    M(Contains) M(GetAccessibleAtPoint) M(GetExtents) M(GetPosition) M(GetSize) M(GetLayer) M(GetMDIZOrder)          \
    M(GetAlpha) M(GrabFocus) M(ScrollTo) M(ScrollToPoint) M(SetExtents) M(SetPosition) M(SetSize)
enum class ComponentMethod : u32 { ATSPI_COMPONENT_METHODS(ATSPI_ENUMERATOR) };
constexpr char kComponentMethods[] = ATSPI_COMPONENT_METHODS(ATSPI_NAME);

Outcome component_call(Request& r, const char* member) noexcept {
    Builder* b = r.out;
    const UiAccessNode& node = *r.node;
    u32 a[8];
    using enum ComponentMethod;
    const auto method = static_cast<ComponentMethod>(name_index(kComponentMethods, member));
    switch (method) {
    case Contains:
    case GetAccessibleAtPoint: {
        if (!take(r, "iiu", a))
            return invalid(r);
        const UiPoint point = snapshot_point(r, static_cast<i32>(a[0]), static_cast<i32>(a[1]), a[2]);
        if (method == Contains) {
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
        break;
    }
    case GetExtents: {
        if (!take(r, "u", a))
            return invalid(r);
        const Extent e = extent_of(r, node, a[0] == 2);
        builder_open_struct(b, "(iiii)");
        builder_i32(b, e.x);
        builder_i32(b, e.y);
        builder_i32(b, e.width);
        builder_i32(b, e.height);
        builder_close_struct(b);
        break;
    }
    case GetPosition: {
        if (!take(r, "u", a))
            return invalid(r);
        const Extent e = extent_of(r, node, a[0] == 2);
        builder_i32(b, e.x);
        builder_i32(b, e.y);
        break;
    }
    case GetSize: {
        const Extent e = extent_of(r, node, false);
        builder_i32(b, e.width);
        builder_i32(b, e.height);
        break;
    }
    case GetLayer:
        builder_u32(b, node.role == UiAccessRole::window ? kLayerWindow : kLayerWidget);
        break;
    case GetMDIZOrder:
        builder_i16(b, 0);
        break;
    case GetAlpha:
        builder_double(b, 1.0);
        break;
    case GrabFocus: {
        const bool focusable = (node.actions & ui_access_action_focus) != 0;
        builder_bool(b, focusable && r.endpoint->host.act(r.endpoint->host.context, node.key, UiAccessAction::focus, 0, {}));
        break;
    }
    case ScrollTo:
        if (!take(r, "u", a))
            return invalid(r);
        builder_bool(b, (node.actions & ui_access_action_scroll_item) &&
                            r.endpoint->host.act(r.endpoint->host.context, node.key, UiAccessAction::scroll_into_view, 0, {}));
        break;
    case ScrollToPoint:
        if (!take(r, "uii", a))
            return invalid(r);
        builder_bool(b, false);
        break;
    case SetExtents:
    case SetPosition:
    case SetSize:
        // The window is the compositor's; nothing inside it is placed by a client of this interface.
        builder_bool(b, false);
        break;
    default:
        return Outcome::unhandled;
    }
    return done(r);
}

// Action ------------------------------------------------------------------------------------------------------------

#define ATSPI_ACTION_METHODS(M) M(GetActions) M(DoAction) M(GetName) M(GetLocalizedName) M(GetDescription) M(GetKeyBinding)
enum class ActionMethod : u32 { ATSPI_ACTION_METHODS(ATSPI_ENUMERATOR) };
constexpr char kActionMethods[] = ATSPI_ACTION_METHODS(ATSPI_NAME);

Outcome action_call(Request& r, const char* member) noexcept {
    Builder* b = r.out;
    ActionEntry entries[kActionMaximum];
    const u32 count = actions_for(*r.node, entries);
    u32 a[8];
    using enum ActionMethod;
    const auto method = static_cast<ActionMethod>(name_index(kActionMethods, member));
    switch (method) {
    case GetActions:
        builder_open_array(b, "(sss)");
        for (u32 i = 0; i < count; ++i) {
            builder_open_struct(b, "(sss)");
            builder_string(b, entries[i].name);
            builder_string(b, entries[i].description);
            builder_string(b, "");
            builder_close_struct(b);
        }
        builder_close_array(b);
        break;
    case DoAction:
    case GetName:
    case GetLocalizedName:
    case GetDescription:
    case GetKeyBinding: {
        if (!take(r, "i", a))
            return Outcome::unhandled;
        const i32 position = static_cast<i32>(a[0]);
        const bool known = position >= 0 && static_cast<u32>(position) < count;
        if (method == DoAction)
            builder_bool(b, known && r.endpoint->host.act(r.endpoint->host.context, r.node->key,
                                                          entries[position].action, 0, {}));
        else if (method == GetDescription)
            builder_string(b, known ? entries[position].description : "");
        else if (method == GetKeyBinding)
            builder_string(b, "");
        else
            builder_string(b, known ? entries[position].name : "");
        break;
    }
    default:
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
    const float top = content_top(*r.endpoint);
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

#define ATSPI_TEXT_METHODS(M)                                                                                        \
    M(GetText) M(SetCaretOffset) M(GetCharacterAtOffset) M(GetStringAtOffset) M(GetTextAtOffset)                     \
    M(GetTextBeforeOffset) M(GetTextAfterOffset) M(GetAttributeValue) M(GetAttributes) M(GetAttributeRun)            \
    M(GetDefaultAttributes) M(GetDefaultAttributeSet) M(GetCharacterExtents) M(GetRangeExtents)                      \
    M(GetOffsetAtPoint) M(GetBoundedRanges) M(GetNSelections) M(GetSelection) M(AddSelection) M(SetSelection)        \
    M(RemoveSelection) M(ScrollSubstringTo) M(ScrollSubstringToPoint)
enum class TextMethod : u32 { ATSPI_TEXT_METHODS(ATSPI_ENUMERATOR) };
constexpr char kTextMethods[] = ATSPI_TEXT_METHODS(ATSPI_NAME);

Outcome text_call(Request& r, const char* member) noexcept {
    Builder* b = r.out;
    const UiAccessNode& node = *r.node;
    const Utf8View text = text_of(r);
    u32 a[8];
    u32 start = 0, end = 0;
    using enum TextMethod;
    const auto method = static_cast<TextMethod>(name_index(kTextMethods, member));
    switch (method) {
    case GetText:
        if (!take(r, "ii", a))
            return invalid(r);
        character_range(text, static_cast<i32>(a[0]), static_cast<i32>(a[1]), &start, &end);
        put_text(b, {text.data + start, end - start});
        break;
    case SetCaretOffset:
        if (!take(r, "i", a))
            return invalid(r);
        return select_characters(r, static_cast<i32>(a[0]), static_cast<i32>(a[0]));
    case GetCharacterAtOffset:
        if (!take(r, "i", a))
            return invalid(r);
        builder_i32(b, static_cast<i32>(a[0]) < 0 ? -1 : static_cast<i32>(text_scalar_at(text, text_byte_offset(text, a[0]))));
        break;
    case GetStringAtOffset:
        if (!take(r, "iu", a) || static_cast<i32>(a[0]) < 0 || a[1] > 4)
            return invalid(r);
        text_span(text, text_byte_offset(text, a[0]), static_cast<TextGranularity>(a[1]), 0, &start, &end);
        put_span(r, text, start, end);
        break;
    case GetTextAtOffset:
    case GetTextBeforeOffset:
    case GetTextAfterOffset: {
        // The boundary types of the older calls: character, word start and end, sentence start and end, line start and end.
        if (!take(r, "iu", a) || static_cast<i32>(a[0]) < 0 || a[1] > 6)
            return invalid(r);
        const TextGranularity granularity = a[1] == 0   ? TextGranularity::character
                                            : a[1] <= 2 ? TextGranularity::word
                                            : a[1] <= 4 ? TextGranularity::sentence
                                                        : TextGranularity::line;
        const i32 direction = method == GetTextAtOffset ? 0 : method == GetTextBeforeOffset ? -1 : 1;
        text_span(text, text_byte_offset(text, a[0]), granularity, direction, &start, &end);
        put_span(r, text, start, end);
        break;
    }
    case GetAttributeValue: {
        const char* name = nullptr;
        if (!take(r, "i", a) || !reader_string(&r.args, &name, nullptr))
            return invalid(r);
        builder_string(b, "");
        break;
    }
    case GetAttributes:
    case GetAttributeRun: {
        bool include_defaults = false;
        if (!take(r, "i", a) || (method == GetAttributeRun && !reader_bool(&r.args, &include_defaults)))
            return invalid(r);
        // Three out arguments: the attributes, and the range they hold over.
        builder_open_array(b, "{ss}");
        builder_close_array(b);
        builder_i32(b, 0);
        builder_i32(b, static_cast<i32>(text_character_count(text)));
        break;
    }
    case GetDefaultAttributes:
    case GetDefaultAttributeSet:
        builder_open_array(b, "{ss}");
        builder_close_array(b);
        break;
    case GetCharacterExtents: {
        if (!take(r, "iu", a) || static_cast<i32>(a[0]) < 0)
            return invalid(r);
        start = text_byte_offset(text, a[0]);
        end = start;
        u32 unused = start;
        ui_access_text_expand(text, UiAccessTextUnit::character, &unused, &end);
        put_extents(b, text_extent(r, start, end));
        break;
    }
    case GetRangeExtents:
        if (!take(r, "iiu", a))
            return invalid(r);
        character_range(text, static_cast<i32>(a[0]), static_cast<i32>(a[1]), &start, &end);
        put_extents(b, text_extent(r, start, end));
        break;
    case GetOffsetAtPoint:
        if (!take(r, "iiu", a))
            return invalid(r);
        builder_i32(b, -1);
        break;
    case GetBoundedRanges:
        if (!take(r, "iiiiuuu", a))
            return invalid(r);
        builder_open_array(b, "(iisv)");
        builder_close_array(b);
        break;
    case GetNSelections:
        builder_i32(b, node.placement.selection_anchor != node.placement.selection_caret ? 1 : 0);
        break;
    case GetSelection: {
        if (!take(r, "i", a))
            return invalid(r);
        if (a[0] != 0 || node.placement.selection_anchor == node.placement.selection_caret)
            return fail(r, kInvalidArguments, "no such selection");
        const u32 low = minimum(node.placement.selection_anchor, node.placement.selection_caret);
        const u32 high = maximum(node.placement.selection_anchor, node.placement.selection_caret);
        builder_i32(b, static_cast<i32>(text_character_offset(text, low)));
        builder_i32(b, static_cast<i32>(text_character_offset(text, high)));
        break;
    }
    case AddSelection:
    case SetSelection: {
        // SetSelection names the selection (only the first exists) before the range.
        const u32 shift = method == SetSelection ? 1u : 0u;
        if (!take(r, shift ? "iii" : "ii", a))
            return invalid(r);
        if (shift && a[0] != 0) {
            builder_bool(b, false);
            break;
        }
        return select_characters(r, static_cast<i32>(a[shift]), static_cast<i32>(a[shift + 1]));
    }
    case RemoveSelection:
        if (!take(r, "i", a))
            return invalid(r);
        if (a[0] != 0) {
            builder_bool(b, false);
            break;
        }
        return select_characters(r, static_cast<i32>(caret_characters(r)), static_cast<i32>(caret_characters(r)));
    case ScrollSubstringTo:
    case ScrollSubstringToPoint:
        // The arguments are not used: nothing scrolls inside a text node.
        builder_bool(b, false);
        break;
    default:
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

#define ATSPI_SELECTION_METHODS(M)                                                                                   \
    M(GetSelectedChild) M(SelectChild) M(IsChildSelected) M(DeselectChild) M(DeselectSelectedChild) M(SelectAll)     \
    M(ClearSelection)
enum class SelectionMethod : u32 { ATSPI_SELECTION_METHODS(ATSPI_ENUMERATOR) };
constexpr char kSelectionMethods[] = ATSPI_SELECTION_METHODS(ATSPI_NAME);

Outcome selection_call(Request& r, const char* member) noexcept {
    Builder* b = r.out;
    u32 a[8];
    using enum SelectionMethod;
    const auto method = static_cast<SelectionMethod>(name_index(kSelectionMethods, member));
    switch (method) {
    case GetSelectedChild: {
        if (!take(r, "i", a))
            return invalid(r);
        i32 seen = 0;
        u32 found = kUiAccessNone;
        for (u32 child = r.node->first_child; child != kUiAccessNone && found == kUiAccessNone;
             child = r.tree->nodes[child].next_sibling)
            if ((r.tree->nodes[child].state & ui_access_selected) && seen++ == static_cast<i32>(a[0]))
                found = child;
        if (found == kUiAccessNone)
            put_null_reference(b);
        else
            put_node(r, found);
        break;
    }
    case SelectChild:
    case IsChildSelected:
    case DeselectChild: {
        if (!take(r, "i", a))
            return invalid(r);
        const u32 child = child_at(r, static_cast<i32>(a[0]));
        if (method == IsChildSelected)
            builder_bool(b, child != kUiAccessNone && (r.tree->nodes[child].state & ui_access_selected));
        else if (method == SelectChild)
            builder_bool(b, child != kUiAccessNone &&
                                r.endpoint->host.act(r.endpoint->host.context, r.tree->nodes[child].key,
                                                     UiAccessAction::select, 0, {}));
        else
            builder_bool(b, false);
        break;
    }
    case DeselectSelectedChild:
        if (!take(r, "i", a))
            return invalid(r);
        builder_bool(b, false);
        break;
    case SelectAll:
    case ClearSelection:
        builder_bool(b, false);
        break;
    default:
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
    if (equal(interface, kApplicationInterface))
        return !r.node ? application_call(r, member) : Outcome::unhandled;
    u32 bit = interface_accessible;
    while (bit <= interface_selection && !equal(interface, interface_name(bit)))
        bit <<= 1;
    if (bit == interface_accessible)
        return accessible_call(r, member);
    if (!r.node || bit > interface_selection)
        return Outcome::unhandled;
    // The other interfaces answer for the nodes that have them (every node has the Component one).
    if (bit != interface_component && !(interfaces_of(r) & bit))
        return Outcome::unhandled;
    switch (bit) {
    case interface_component:
        return component_call(r, member);
    case interface_action:
        return action_call(r, member);
    case interface_text:
        return text_call(r, member);
    case interface_editable_text:
        return editable_text_call(r, member);
    case interface_selection:
        return selection_call(r, member);
    default:  // Value has properties only
        return Outcome::unhandled;
    }
}

} // namespace maicord::atspi
