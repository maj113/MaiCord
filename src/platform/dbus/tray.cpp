#include "maicord/core/heap.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/dbus/bus.hpp"
#include "maicord/platform/dbus/session.hpp"
#include "maicord/platform/dbus/tray.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord::dbus {
namespace {

constexpr const char* kWatcherName = "org.kde.StatusNotifierWatcher";
constexpr const char* kWatcherPath = "/StatusNotifierWatcher";
constexpr const char* kItemPath = "/StatusNotifierItem";
constexpr const char* kMenuPath = "/MenuBar";
constexpr const char* kItemInterface = "org.kde.StatusNotifierItem";
constexpr const char* kMenuInterface = "com.canonical.dbusmenu";
constexpr u32 kMaxPixmaps = 4;
constexpr u32 kMaxPixmapPixels = 64 * 64;
constexpr u32 kMaxItems = 32;  // menu entries and the separators between them
constexpr u32 kLabelBytes = 96;

struct Item {
    u32 command;  // 0 for a separator
    bool checkable, checked, separator;
    char label[kLabelBytes];
};

struct Tray {
    bool started;
    bool registered;
    bool hook_added;
    TrayHandler handler;
    void* context;
    u32 watch;
    char id[64];
    char service[96];
    char title[64];
    char tooltip[192];
    bool attention;
    u32 icon_count;
    u32 icon_width[kMaxPixmaps];
    u32 icon_height[kMaxPixmaps];
    u32 pixels[kMaxPixmaps][kMaxPixmapPixels];
    Item items[kMaxItems];
    u32 item_count;
    u32 revision;
};

Tray g;

// The interfaces only: object_handle_standard (bus.cpp) adds the node and the standard interfaces.
const char kItemIntrospection[] =
    "<interface name=\"org.kde.StatusNotifierItem\">"
    "<property name=\"Category\" type=\"s\" access=\"read\"/>"
    "<property name=\"Id\" type=\"s\" access=\"read\"/>"
    "<property name=\"Title\" type=\"s\" access=\"read\"/>"
    "<property name=\"Status\" type=\"s\" access=\"read\"/>"
    "<property name=\"WindowId\" type=\"u\" access=\"read\"/>"
    "<property name=\"IconThemePath\" type=\"s\" access=\"read\"/>"
    "<property name=\"Menu\" type=\"o\" access=\"read\"/>"
    "<property name=\"ItemIsMenu\" type=\"b\" access=\"read\"/>"
    "<property name=\"IconName\" type=\"s\" access=\"read\"/>"
    "<property name=\"IconPixmap\" type=\"a(iiay)\" access=\"read\"/>"
    "<property name=\"OverlayIconName\" type=\"s\" access=\"read\"/>"
    "<property name=\"OverlayIconPixmap\" type=\"a(iiay)\" access=\"read\"/>"
    "<property name=\"AttentionIconName\" type=\"s\" access=\"read\"/>"
    "<property name=\"AttentionIconPixmap\" type=\"a(iiay)\" access=\"read\"/>"
    "<property name=\"AttentionMovieName\" type=\"s\" access=\"read\"/>"
    "<property name=\"ToolTip\" type=\"(sa(iiay)ss)\" access=\"read\"/>"
    "<method name=\"ContextMenu\"><arg type=\"i\" name=\"x\" direction=\"in\"/><arg type=\"i\" name=\"y\" direction=\"in\"/></method>"
    "<method name=\"Activate\"><arg type=\"i\" name=\"x\" direction=\"in\"/><arg type=\"i\" name=\"y\" direction=\"in\"/></method>"
    "<method name=\"SecondaryActivate\"><arg type=\"i\" name=\"x\" direction=\"in\"/><arg type=\"i\" name=\"y\" direction=\"in\"/></method>"
    "<method name=\"Scroll\"><arg type=\"i\" name=\"delta\" direction=\"in\"/><arg type=\"s\" name=\"orientation\" direction=\"in\"/></method>"
    "<signal name=\"NewTitle\"/>"
    "<signal name=\"NewIcon\"/>"
    "<signal name=\"NewAttentionIcon\"/>"
    "<signal name=\"NewOverlayIcon\"/>"
    "<signal name=\"NewToolTip\"/>"
    "<signal name=\"NewStatus\"><arg type=\"s\" name=\"status\"/></signal>"
    "</interface>";

const char kMenuIntrospection[] =
    "<interface name=\"com.canonical.dbusmenu\">"
    "<property name=\"Version\" type=\"u\" access=\"read\"/>"
    "<property name=\"TextDirection\" type=\"s\" access=\"read\"/>"
    "<property name=\"Status\" type=\"s\" access=\"read\"/>"
    "<property name=\"IconThemePath\" type=\"as\" access=\"read\"/>"
    "<method name=\"GetLayout\"><arg type=\"i\" name=\"parentId\" direction=\"in\"/><arg type=\"i\" name=\"recursionDepth\" direction=\"in\"/>"
    "<arg type=\"as\" name=\"propertyNames\" direction=\"in\"/><arg type=\"u\" name=\"revision\" direction=\"out\"/>"
    "<arg type=\"(ia{sv}av)\" name=\"layout\" direction=\"out\"/></method>"
    "<method name=\"GetGroupProperties\"><arg type=\"ai\" name=\"ids\" direction=\"in\"/><arg type=\"as\" name=\"propertyNames\" direction=\"in\"/>"
    "<arg type=\"a(ia{sv})\" name=\"properties\" direction=\"out\"/></method>"
    "<method name=\"GetProperty\"><arg type=\"i\" name=\"id\" direction=\"in\"/><arg type=\"s\" name=\"name\" direction=\"in\"/>"
    "<arg type=\"v\" name=\"value\" direction=\"out\"/></method>"
    "<method name=\"Event\"><arg type=\"i\" name=\"id\" direction=\"in\"/><arg type=\"s\" name=\"eventId\" direction=\"in\"/>"
    "<arg type=\"v\" name=\"data\" direction=\"in\"/><arg type=\"u\" name=\"timestamp\" direction=\"in\"/></method>"
    "<method name=\"EventGroup\"><arg type=\"a(isvu)\" name=\"events\" direction=\"in\"/><arg type=\"ai\" name=\"idErrors\" direction=\"out\"/></method>"
    "<method name=\"AboutToShow\"><arg type=\"i\" name=\"id\" direction=\"in\"/><arg type=\"b\" name=\"needUpdate\" direction=\"out\"/></method>"
    "<method name=\"AboutToShowGroup\"><arg type=\"ai\" name=\"ids\" direction=\"in\"/><arg type=\"ai\" name=\"updatesNeeded\" direction=\"out\"/>"
    "<arg type=\"ai\" name=\"idErrors\" direction=\"out\"/></method>"
    "<signal name=\"ItemsPropertiesUpdated\"><arg type=\"a(ia{sv})\" name=\"updatedProps\"/><arg type=\"a(ias)\" name=\"removedProps\"/></signal>"
    "<signal name=\"LayoutUpdated\"><arg type=\"u\" name=\"revision\"/><arg type=\"i\" name=\"parent\"/></signal>"
    "<signal name=\"ItemActivationRequested\"><arg type=\"i\" name=\"id\"/><arg type=\"u\" name=\"timestamp\"/></signal>"
    "</interface>";

u32 text_length(const char* text) noexcept {
    u32 length = 0;
    while (text[length])
        ++length;
    return length;
}

bool text_equal(const char* a, const char* b) noexcept {
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

void copy_text(char* out, u32 capacity, const char* text) noexcept {
    u32 length = text ? text_length(text) : 0;
    if (length >= capacity)
        length = capacity - 1;
    if (length)
        copy_bytes(out, text, length);
    out[length] = 0;
}

void emit_event(TrayEventKind kind, i32 x, i32 y, u32 command) noexcept {
    if (!g.handler)
        return;
    TrayEvent event{};
    event.kind = kind;
    event.x = x;
    event.y = y;
    event.command = command;
    g.handler(g.context, event);
}

// StatusNotifierItem properties ---------------------------------------------------------------------------------------

void variant_text(Builder* b, const char* signature, const char* text) noexcept {
    builder_open_variant(b, signature);
    builder_string(b, text);
    builder_close_variant(b);
}

// a(iiay) of the state's icons (empty when there are none), as the value of the open variant.
void write_pixmaps(Builder* b) noexcept {
    builder_open_variant(b, "a(iiay)");
    builder_open_array(b, "(iiay)");
    for (u32 i = 0; i < g.icon_count; ++i) {
        const u32 count = g.icon_width[i] * g.icon_height[i];
        u8* bytes = static_cast<u8*>(heap_allocate(count * 4 + 1));
        if (!bytes) {
            b->failed = true;
            break;
        }
        tray_pixmap_bytes(g.pixels[i], count, bytes);
        builder_open_struct(b, "(iiay)");
        builder_i32(b, static_cast<i32>(g.icon_width[i]));
        builder_i32(b, static_cast<i32>(g.icon_height[i]));
        builder_byte_array(b, bytes, count * 4);
        builder_close_struct(b);
        heap_free(bytes);
    }
    builder_close_array(b);
    builder_close_variant(b);
}

void write_empty_pixmaps(Builder* b) noexcept {
    builder_open_variant(b, "a(iiay)");
    builder_open_array(b, "(iiay)");
    builder_close_array(b);
    builder_close_variant(b);
}

const ExportedProperty kItemProperties[] = {
    {kItemInterface, "Category"},          {kItemInterface, "Id"},
    {kItemInterface, "Title"},             {kItemInterface, "Status"},
    {kItemInterface, "WindowId"},          {kItemInterface, "IconThemePath"},
    {kItemInterface, "Menu"},              {kItemInterface, "ItemIsMenu"},
    {kItemInterface, "IconName"},          {kItemInterface, "IconPixmap"},
    {kItemInterface, "OverlayIconName"},   {kItemInterface, "OverlayIconPixmap"},
    {kItemInterface, "AttentionIconName"}, {kItemInterface, "AttentionIconPixmap"},
    {kItemInterface, "AttentionMovieName"}, {kItemInterface, "ToolTip"},
};

bool write_item_property(void*, const char*, const char* name, Builder* b) noexcept {
    if (text_equal(name, "Category")) {
        variant_text(b, "s", "ApplicationStatus");
    } else if (text_equal(name, "Id")) {
        variant_text(b, "s", g.id);
    } else if (text_equal(name, "Title")) {
        variant_text(b, "s", g.title);
    } else if (text_equal(name, "Status")) {
        variant_text(b, "s", g.attention ? "NeedsAttention" : "Active");
    } else if (text_equal(name, "WindowId")) {
        builder_open_variant(b, "u");
        builder_u32(b, 0);
        builder_close_variant(b);
    } else if (text_equal(name, "IconThemePath") || text_equal(name, "IconName") || text_equal(name, "OverlayIconName") ||
               text_equal(name, "AttentionIconName") || text_equal(name, "AttentionMovieName")) {
        variant_text(b, "s", "");
    } else if (text_equal(name, "Menu")) {
        builder_open_variant(b, "o");
        builder_object_path(b, kMenuPath);
        builder_close_variant(b);
    } else if (text_equal(name, "ItemIsMenu")) {
        builder_open_variant(b, "b");
        builder_bool(b, false);
        builder_close_variant(b);
    } else if (text_equal(name, "IconPixmap") || text_equal(name, "AttentionIconPixmap")) {
        write_pixmaps(b);
    } else if (text_equal(name, "OverlayIconPixmap")) {
        write_empty_pixmaps(b);
    } else if (text_equal(name, "ToolTip")) {
        // (icon name, icon pixmaps, title, description)
        builder_open_variant(b, "(sa(iiay)ss)");
        builder_open_struct(b, "(sa(iiay)ss)");
        builder_string(b, "");
        builder_open_array(b, "(iiay)");
        builder_close_array(b);
        builder_string(b, g.title);
        builder_string(b, g.tooltip);
        builder_close_struct(b);
        builder_close_variant(b);
    } else {
        return false;
    }
    return true;
}

ExportedObject item_object() noexcept {
    return ExportedObject{kItemProperties, static_cast<u32>(sizeof(kItemProperties) / sizeof(kItemProperties[0])),
                          write_item_property, nullptr, kItemIntrospection};
}

bool item_call(void*, Connection* c, const Message& call) noexcept {
    if (object_handle_standard(c, call, item_object()))
        return true;
    if (!call.interface || !text_equal(call.interface, kItemInterface))
        return false;
    Reader r;
    message_reader(call, &r);
    if (text_equal(call.member, "Activate") || text_equal(call.member, "SecondaryActivate") ||
        text_equal(call.member, "ContextMenu")) {
        i32 x = 0;
        i32 y = 0;
        if (!reader_i32(&r, &x) || !reader_i32(&r, &y)) {
            connection_reply_error(c, call, "org.freedesktop.DBus.Error.InvalidArgs", "(ii)");
            return true;
        }
        connection_reply(c, call, nullptr);
        emit_event(text_equal(call.member, "Activate")            ? TrayEventKind::activate
                   : text_equal(call.member, "SecondaryActivate") ? TrayEventKind::secondary_activate
                                                                  : TrayEventKind::context_menu,
                   x, y, 0);
        return true;
    }
    if (text_equal(call.member, "Scroll")) {
        connection_reply(c, call, nullptr);
        return true;
    }
    return false;
}

// dbusmenu --------------------------------------------------------------------------------------------------------------

const ExportedProperty kMenuProperties[] = {
    {kMenuInterface, "Version"},
    {kMenuInterface, "TextDirection"},
    {kMenuInterface, "Status"},
    {kMenuInterface, "IconThemePath"},
};

bool write_menu_property(void*, const char*, const char* name, Builder* b) noexcept {
    if (text_equal(name, "Version")) {
        builder_open_variant(b, "u");
        builder_u32(b, 3);
        builder_close_variant(b);
    } else if (text_equal(name, "TextDirection")) {
        variant_text(b, "s", "ltr");
    } else if (text_equal(name, "Status")) {
        variant_text(b, "s", "normal");
    } else if (text_equal(name, "IconThemePath")) {
        builder_open_variant(b, "as");
        builder_open_array(b, "s");
        builder_close_array(b);
        builder_close_variant(b);
    } else {
        return false;
    }
    return true;
}

ExportedObject menu_object() noexcept {
    return ExportedObject{kMenuProperties, static_cast<u32>(sizeof(kMenuProperties) / sizeof(kMenuProperties[0])),
                          write_menu_property, nullptr, kMenuIntrospection};
}

const Item* item_by_id(i32 id) noexcept {
    return id >= 1 && static_cast<u32>(id) <= g.item_count ? &g.items[id - 1] : nullptr;
}

void property_entry(Builder* b, const char* name, const char* signature) noexcept {
    builder_open_entry(b);
    builder_string(b, name);
    builder_open_variant(b, signature);
}

void end_entry(Builder* b) noexcept {
    builder_close_variant(b);
    builder_close_entry(b);
}

// a{sv} of one item (id 0 is the root menu).
void write_item_properties(Builder* b, i32 id) noexcept {
    builder_open_array(b, "{sv}");
    if (id == 0) {
        property_entry(b, "children-display", "s");
        builder_string(b, "submenu");
        end_entry(b);
    } else if (const Item* item = item_by_id(id)) {
        if (item->separator) {
            property_entry(b, "type", "s");
            builder_string(b, "separator");
            end_entry(b);
        } else {
            property_entry(b, "label", "s");
            builder_string(b, item->label);
            end_entry(b);
            property_entry(b, "enabled", "b");
            builder_bool(b, true);
            end_entry(b);
            property_entry(b, "visible", "b");
            builder_bool(b, true);
            end_entry(b);
            if (item->checkable) {
                property_entry(b, "toggle-type", "s");
                builder_string(b, "checkmark");
                end_entry(b);
                property_entry(b, "toggle-state", "i");
                builder_i32(b, item->checked ? 1 : 0);
                end_entry(b);
            }
        }
    }
    builder_close_array(b);
}

// (ia{sv}av): the item and, within the depth, its children (only the root has any).
void write_layout(Builder* b, i32 id, i32 depth) noexcept {
    builder_open_struct(b, "(ia{sv}av)");
    builder_i32(b, id);
    write_item_properties(b, id);
    builder_open_array(b, "v");
    if (id == 0 && depth != 0) {
        for (u32 i = 0; i < g.item_count; ++i) {
            builder_open_variant(b, "(ia{sv}av)");
            write_layout(b, static_cast<i32>(i + 1), depth - 1);
            builder_close_variant(b);
        }
    }
    builder_close_array(b);
    builder_close_struct(b);
}

void menu_clicked(i32 id) noexcept {
    const Item* item = item_by_id(id);
    if (item && !item->separator && item->command)
        emit_event(TrayEventKind::command, 0, 0, item->command);
}

bool menu_call(void*, Connection* c, const Message& call) noexcept {
    if (object_handle_standard(c, call, menu_object()))
        return true;
    if (!call.interface || !text_equal(call.interface, kMenuInterface))
        return false;
    Reader r;
    message_reader(call, &r);
    Builder body;
    builder_init(&body);
    bool ok = true;
    if (text_equal(call.member, "GetLayout")) {
        i32 parent = 0;
        i32 depth = 0;
        ok = reader_i32(&r, &parent) && reader_i32(&r, &depth);
        if (!ok || (parent != 0 && !item_by_id(parent))) {
            builder_release(&body);
            connection_reply_error(c, call, "org.freedesktop.DBus.Error.InvalidArgs", "no such menu item");
            return true;
        }
        builder_u32(&body, g.revision);
        write_layout(&body, parent, depth);
    } else if (text_equal(call.member, "GetGroupProperties")) {
        bool all = true;
        ok = reader_enter_array(&r);
        u32 ids[kMaxItems + 1];
        u32 count = 0;
        while (ok && reader_more(r)) {
            i32 id = 0;
            ok = reader_i32(&r, &id);
            if (ok && count < kMaxItems + 1)
                ids[count++] = static_cast<u32>(id);
            all = false;
        }
        ok = ok && reader_leave(&r);
        if (all) {
            for (u32 i = 0; i < g.item_count; ++i)
                ids[count++] = i + 1;
        }
        builder_open_array(&body, "(ia{sv})");
        for (u32 i = 0; ok && i < count; ++i) {
            if (ids[i] != 0 && !item_by_id(static_cast<i32>(ids[i])))
                continue;
            builder_open_struct(&body, "(ia{sv})");
            builder_i32(&body, static_cast<i32>(ids[i]));
            write_item_properties(&body, static_cast<i32>(ids[i]));
            builder_close_struct(&body);
        }
        builder_close_array(&body);
    } else if (text_equal(call.member, "GetProperty")) {
        // The one property asked for, taken out of the item's full set.
        i32 id = 0;
        const char* name = nullptr;
        ok = reader_i32(&r, &id) && reader_string(&r, &name, nullptr);
        Builder all;
        builder_init(&all);
        write_item_properties(&all, id);
        Reader a;
        reader_init(&a, all.data, all.size, "a{sv}", nullptr, 0);
        bool found = false;
        if (ok && reader_enter_array(&a)) {
            while (!found && reader_more(a)) {
                const char* key = nullptr;
                ok = reader_enter_struct(&a) && reader_string(&a, &key, nullptr);
                if (ok && text_equal(key, name)) {
                    // Copy the variant by re-reading: only strings, booleans and integers occur.
                    const char* signature = nullptr;
                    ok = reader_enter_variant(&a, &signature);
                    if (ok) {
                        builder_open_variant(&body, signature);
                        if (signature[0] == 's') {
                            const char* text = nullptr;
                            ok = reader_string(&a, &text, nullptr);
                            builder_string(&body, text ? text : "");
                        } else if (signature[0] == 'b') {
                            bool value = false;
                            ok = reader_bool(&a, &value);
                            builder_bool(&body, value);
                        } else {
                            i32 value = 0;
                            ok = reader_i32(&a, &value);
                            builder_i32(&body, value);
                        }
                        builder_close_variant(&body);
                        found = ok;
                    }
                    break;
                }
                ok = ok && reader_leave(&a);
            }
        }
        builder_release(&all);
        if (!found) {
            builder_release(&body);
            connection_reply_error(c, call, "org.freedesktop.DBus.Error.InvalidArgs", "no such property");
            return true;
        }
    } else if (text_equal(call.member, "Event")) {
        i32 id = 0;
        const char* event = nullptr;
        ok = reader_i32(&r, &id) && reader_string(&r, &event, nullptr);
        if (ok && text_equal(event, "clicked"))
            menu_clicked(id);
    } else if (text_equal(call.member, "EventGroup")) {
        ok = reader_enter_array(&r);
        while (ok && reader_more(r)) {
            i32 id = 0;
            const char* event = nullptr;
            ok = reader_enter_struct(&r) && reader_i32(&r, &id) && reader_string(&r, &event, nullptr) && reader_leave(&r);
            if (ok && text_equal(event, "clicked"))
                menu_clicked(id);
        }
        builder_open_array(&body, "i");
        builder_close_array(&body);
    } else if (text_equal(call.member, "AboutToShow")) {
        builder_bool(&body, false);
    } else if (text_equal(call.member, "AboutToShowGroup")) {
        builder_open_array(&body, "i");
        builder_close_array(&body);
        builder_open_array(&body, "i");
        builder_close_array(&body);
    } else {
        builder_release(&body);
        return false;
    }
    if (!ok || !builder_ok(body)) {
        builder_release(&body);
        connection_reply_error(c, call, "org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        return true;
    }
    connection_reply(c, call, &body);
    builder_release(&body);
    return true;
}

// Registration ----------------------------------------------------------------------------------------------------------

void on_register_reply(void*, Status status, const Message* reply) noexcept {
    g.registered = status == Status::ok;
    if (!g.registered)
        log_write(LogLevel::trace, "tray",
                  status == Status::error_reply && reply && reply->error_name ? reply->error_name : "registration failed");
}

void register_item(Connection* c) noexcept {
    Builder body;
    builder_init(&body);
    builder_string(&body, g.service);
    const CallTarget target{kWatcherName, kWatcherPath, kWatcherName, "RegisterStatusNotifierItem"};
    connection_call(c, target, &body, 10000, on_register_reply, nullptr);
    builder_release(&body);
}

void on_watcher(void*, const char*, bool owned) noexcept {
    Connection* c = session_bus();
    if (!c || !g.started)
        return;
    if (owned)
        register_item(c);
    else
        g.registered = false;
}

void on_session(Connection* c) noexcept {
    if (!g.started)
        return;
    g.registered = false;
    if (!connection_request_name(c, g.service, 3000)) {
        log_write(LogLevel::trace, "tray", "the item's bus name could not be taken");
        return;
    }
    connection_export_object(c, kItemPath, item_call, nullptr);
    connection_export_object(c, kMenuPath, menu_call, nullptr);
    g.watch = connection_watch_name(c, kWatcherName, on_watcher, nullptr);
}

// ARGB32 in network order: alpha, red, green, blue.
bool same_icons(const TrayState& state) noexcept {
    if (state.icon_count != g.icon_count)
        return false;
    for (u32 i = 0; i < state.icon_count; ++i) {
        if (state.icons[i].width != g.icon_width[i] || state.icons[i].height != g.icon_height[i] ||
            !bytes_equal(state.icons[i].pixels, g.pixels[i], state.icons[i].width * state.icons[i].height * 4))
            return false;
    }
    return true;
}

bool same_menu(const TrayState& state) noexcept {
    u32 expected = 0;
    for (u32 i = 0; i < state.menu_count; ++i)
        expected += state.menu[i].separator_before ? 2 : 1;
    if (expected != g.item_count)
        return false;
    u32 at = 0;
    for (u32 i = 0; i < state.menu_count; ++i) {
        if (state.menu[i].separator_before && !g.items[at++].separator)
            return false;
        const Item& item = g.items[at++];
        if (item.command != state.menu[i].command || item.checkable != state.menu[i].checkable ||
            item.checked != state.menu[i].checked || !text_equal(item.label, state.menu[i].label))
            return false;
    }
    return true;
}

} // namespace

void tray_pixmap_bytes(const u32* pixels, u32 count, u8* bytes) noexcept {
    for (u32 i = 0; i < count; ++i) {
        const u32 p = pixels[i];
        bytes[i * 4] = static_cast<u8>(p >> 24);
        bytes[i * 4 + 1] = static_cast<u8>(p >> 16);
        bytes[i * 4 + 2] = static_cast<u8>(p >> 8);
        bytes[i * 4 + 3] = static_cast<u8>(p);
    }
}

bool tray_start(const char* id, TrayHandler handler, void* context) noexcept {
    if (g.started)
        return true;
    g.started = true;
    g.handler = handler;
    g.context = context;
    copy_text(g.id, sizeof(g.id), id);
    // org.kde.StatusNotifierItem-<pid>-<n>: the name hosts expect (the watcher is told it).
    const char prefix[] = "org.kde.StatusNotifierItem-";
    copy_bytes(g.service, prefix, sizeof(prefix) - 1);
    u32 length = sizeof(prefix) - 1;
    char digits[12];
    u32 count = 0;
    for (u32 n = process_id(); n || count == 0; n /= 10)
        digits[count++] = static_cast<char>('0' + n % 10);
    while (count)
        g.service[length++] = digits[--count];
    g.service[length++] = '-';
    g.service[length++] = '1';
    g.service[length] = 0;
    if (!g.hook_added) {
        g.hook_added = true;
        session_bus_add_hook(on_session);  // runs at once when the bus is already open
    }
    return session_bus_open() && session_bus() != nullptr;
}

bool tray_available() noexcept {
    return g.started && g.registered;
}

void tray_update(const TrayState& state) noexcept {
    const bool title_changed = !text_equal(g.title, state.title ? state.title : "");
    const bool tooltip_changed = !text_equal(g.tooltip, state.tooltip ? state.tooltip : "");
    const bool icons_changed = !same_icons(state);
    const bool menu_changed = !same_menu(state);
    const bool attention_changed = g.attention != state.attention;
    if (!title_changed && !tooltip_changed && !icons_changed && !menu_changed && !attention_changed)
        return;
    copy_text(g.title, sizeof(g.title), state.title);
    copy_text(g.tooltip, sizeof(g.tooltip), state.tooltip);
    g.attention = state.attention;
    if (icons_changed) {
        g.icon_count = 0;
        for (u32 i = 0; i < state.icon_count && g.icon_count < kMaxPixmaps; ++i) {
            const TrayPixmap& icon = state.icons[i];
            if (!icon.pixels || icon.width == 0 || icon.height == 0 || icon.width * icon.height > kMaxPixmapPixels)
                continue;
            g.icon_width[g.icon_count] = icon.width;
            g.icon_height[g.icon_count] = icon.height;
            copy_bytes(g.pixels[g.icon_count], icon.pixels, icon.width * icon.height * 4);
            ++g.icon_count;
        }
    }
    if (menu_changed) {
        g.item_count = 0;
        for (u32 i = 0; i < state.menu_count && g.item_count + 2 <= kMaxItems; ++i) {
            if (state.menu[i].separator_before) {
                Item& separator = g.items[g.item_count++];
                separator = Item{};
                separator.separator = true;
            }
            Item& item = g.items[g.item_count++];
            item = Item{};
            item.command = state.menu[i].command;
            item.checkable = state.menu[i].checkable;
            item.checked = state.menu[i].checked;
            copy_text(item.label, sizeof(item.label), state.menu[i].label);
        }
        ++g.revision;
    }
    Connection* c = session_bus();
    if (!c || !g.started)
        return;
    if (title_changed)
        connection_emit(c, kItemPath, kItemInterface, "NewTitle", nullptr);
    if (icons_changed) {
        connection_emit(c, kItemPath, kItemInterface, "NewIcon", nullptr);
        connection_emit(c, kItemPath, kItemInterface, "NewAttentionIcon", nullptr);
    }
    if (tooltip_changed || title_changed)
        connection_emit(c, kItemPath, kItemInterface, "NewToolTip", nullptr);
    if (attention_changed) {
        Builder body;
        builder_init(&body);
        builder_string(&body, g.attention ? "NeedsAttention" : "Active");
        connection_emit(c, kItemPath, kItemInterface, "NewStatus", &body);
        builder_release(&body);
    }
    if (menu_changed) {
        Builder body;
        builder_init(&body);
        builder_u32(&body, g.revision);
        builder_i32(&body, 0);
        connection_emit(c, kMenuPath, kMenuInterface, "LayoutUpdated", &body);
        builder_release(&body);
    }
}

void tray_stop() noexcept {
    if (!g.started)
        return;
    if (Connection* c = session_bus()) {
        connection_unwatch_name(c, g.watch);
        connection_unexport_object(c, kItemPath);
        connection_unexport_object(c, kMenuPath);
        connection_release_name(c, g.service, 1000);
    }
    g.started = false;
    g.registered = false;
    g.watch = 0;
}

} // namespace maicord::dbus
