#include "maicord/platform/uia_provider.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/ui/accessibility.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/sys/ole.hpp"

#include <stddef.h>

namespace maicord {
namespace {

// UI Automation ABI constants (UIAutomationClient.h / UIAutomationCoreApi.h).
// The SDK headers are not included: this translation unit declares its own
// vtables and loads every entry point at run time.
constexpr long kUiaRootObjectId = -25;
constexpr int kUiaAppendRuntimeId = 3;
constexpr HRESULT kElementNotAvailable = static_cast<HRESULT>(0x80040201L);
constexpr HRESULT kElementNotEnabled = static_cast<HRESULT>(0x80040200L);
constexpr HRESULT kInvalidOperation = static_cast<HRESULT>(0x80131509L);

enum : int {
    pattern_invoke = 10000,
    pattern_selection = 10001,
    pattern_value = 10002,
    pattern_range_value = 10003,
    pattern_scroll = 10004,
    pattern_expand_collapse = 10005,
    pattern_grid = 10006,
    pattern_grid_item = 10007,
    pattern_selection_item = 10010,
    pattern_text = 10014,
    pattern_toggle = 10015,
    pattern_scroll_item = 10017,
};
enum : int {
    event_focus_changed = 20005,
    event_element_selected = 20012,
    event_text_selection_changed = 20014,
    event_text_changed = 20015,
    event_live_region_changed = 20024,
};
enum : int {
    attribute_is_hidden = 40013,
    attribute_is_read_only = 40015,
};
enum : int {
    property_control_type = 30003,
    property_name = 30005,
    property_has_keyboard_focus = 30008,
    property_is_keyboard_focusable = 30009,
    property_is_enabled = 30010,
    property_automation_id = 30011,
    property_class_name = 30012,
    property_help_text = 30013,
    property_is_control_element = 30016,
    property_is_content_element = 30017,
    property_is_password = 30019,
    property_is_offscreen = 30022,
    property_framework_id = 30024,
    property_value_value = 30045,
    property_range_value_value = 30047,
    property_expand_collapse_state = 30070,
    property_selection_item_is_selected = 30079,
    property_toggle_state = 30086,
    property_provider_description = 30107,
    property_live_setting = 30135,
    property_position_in_set = 30152,
    property_size_of_set = 30153,
    property_is_dialog = 30174,
};
enum : int {
    control_button = 50000,
    control_check_box = 50002,
    control_combo_box = 50003,
    control_edit = 50004,
    control_hyperlink = 50005,
    control_image = 50006,
    control_list_item = 50007,
    control_list = 50008,
    control_menu = 50009,
    control_menu_item = 50011,
    control_radio_button = 50013,
    control_scroll_bar = 50014,
    control_slider = 50015,
    control_status_bar = 50017,
    control_tab = 50018,
    control_tab_item = 50019,
    control_text = 50020,
    control_tree = 50023,
    control_tree_item = 50024,
    control_custom = 50025,
    control_group = 50026,
    control_document = 50030,
    control_window = 50032,
    control_pane = 50033,
    control_header = 50034,
};
constexpr int kProviderOptionsServerSide = 2;
constexpr int kStructureChildrenInvalidated = 2;
constexpr double kScrollNoScroll = -1.0;

struct UiaBounds {
    double left, top, width, height;
};

constexpr GUID kIidUnknown{0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidSimple{0xd6dd68d1, 0x86fd, 0x4332, {0x86, 0x66, 0x9a, 0xbe, 0xde, 0xa2, 0xd2, 0x4c}};
constexpr GUID kIidFragment{0xf7063da8, 0x8359, 0x439c, {0x92, 0x97, 0xbb, 0xc5, 0x29, 0x9a, 0x7d, 0x87}};
constexpr GUID kIidFragmentRoot{0x620ce2a5, 0xab8f, 0x40a9, {0x86, 0xcb, 0xde, 0x3c, 0x75, 0x59, 0x9b, 0x58}};
constexpr GUID kIidInvoke{0x54fcb24b, 0xe18e, 0x47a2, {0xb4, 0xd3, 0xec, 0xcb, 0xe7, 0x75, 0x99, 0xa2}};
constexpr GUID kIidToggle{0x56d00bd0, 0xc4f4, 0x433c, {0xa8, 0x36, 0x1a, 0x52, 0xa5, 0x7e, 0x08, 0x92}};
constexpr GUID kIidSelection{0xfb8b03af, 0x3bdf, 0x48d4, {0xbd, 0x36, 0x1a, 0x65, 0x79, 0x3b, 0xe1, 0x68}};
constexpr GUID kIidSelectionItem{0x2acad808, 0xb2d4, 0x452d, {0xa4, 0x07, 0x91, 0xff, 0x1a, 0xd1, 0x67, 0xb2}};
constexpr GUID kIidValue{0xc7935180, 0x6fb3, 0x4201, {0xb1, 0x74, 0x7d, 0xf7, 0x3a, 0xdb, 0xf6, 0x4a}};
constexpr GUID kIidRangeValue{0x36dc7aef, 0x33e6, 0x4691, {0xaf, 0xe1, 0x2b, 0xe7, 0x27, 0x4b, 0x3d, 0x33}};
constexpr GUID kIidScroll{0xb38b8077, 0x1fc3, 0x42a5, {0x8c, 0xae, 0xd4, 0x0c, 0x22, 0x15, 0x05, 0x5a}};
constexpr GUID kIidExpandCollapse{0xd847d3a5, 0xcab0, 0x4a98, {0x8c, 0x32, 0xec, 0xb4, 0x5c, 0x59, 0xad, 0x24}};
constexpr GUID kIidGrid{0xb17d6187, 0x0907, 0x464b, {0xa1, 0x68, 0x0e, 0xf1, 0x7a, 0x15, 0x72, 0xb1}};
constexpr GUID kIidGridItem{0xd02541f1, 0xfb81, 0x4d64, {0xae, 0x32, 0xf5, 0x20, 0xf8, 0xa6, 0xdb, 0xd1}};
constexpr GUID kIidScrollItem{0x2360c714, 0x4bf1, 0x4b26, {0xba, 0x65, 0x9b, 0x21, 0x31, 0x61, 0x27, 0xeb}};
constexpr GUID kIidText{0x3589c92c, 0x63f3, 0x4367, {0x99, 0xbb, 0xad, 0xa6, 0x53, 0xb7, 0x7c, 0xf2}};
constexpr GUID kIidTextRange{0x5347ad7b, 0xc355, 0x46f8, {0xaf, 0xf5, 0x90, 0x90, 0x33, 0x58, 0x2f, 0x63}};

bool guid_equal(const GUID& a, const GUID& b) noexcept {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           bytes_equal(a.Data4, b.Data4, static_cast<u32>(sizeof(a.Data4)));
}

// Run-time entry points.
using ReturnRawElementProvider = LRESULT(WINAPI*)(HWND, WPARAM, LPARAM, void*);
using HostProviderFromHwnd = HRESULT(WINAPI*)(HWND, void**);
using RaiseAutomationEvent = HRESULT(WINAPI*)(void*, int);
using RaisePropertyChangedEvent = HRESULT(WINAPI*)(void*, int, VARIANT, VARIANT);
using RaiseStructureChangedEvent = HRESULT(WINAPI*)(void*, int, int*, int);
using RaiseNotificationEvent = HRESULT(WINAPI*)(void*, int, int, BSTR, BSTR);
using ClientsAreListening = BOOL(WINAPI*)();
using DisconnectProvider = HRESULT(WINAPI*)(void*);
using AllocStringLen = BSTR(WINAPI*)(const OLECHAR*, UINT);
using FreeString = void(WINAPI*)(BSTR);
using CreateVector = SAFEARRAY*(WINAPI*)(VARTYPE, LONG, ULONG);
using PutElement = HRESULT(WINAPI*)(SAFEARRAY*, LONG*, void*);
using DestroyArray = HRESULT(WINAPI*)(SAFEARRAY*);
using ReservedValue = HRESULT(WINAPI*)(void**);

} // namespace

struct UiaRuntime {
    HMODULE core, automation;
    ReturnRawElementProvider return_raw_element_provider;
    HostProviderFromHwnd host_provider_from_hwnd;
    RaiseAutomationEvent raise_automation_event;
    RaisePropertyChangedEvent raise_property_changed_event;
    RaiseStructureChangedEvent raise_structure_changed_event;
    RaiseNotificationEvent raise_notification_event;
    ClientsAreListening clients_are_listening;
    DisconnectProvider disconnect_provider;
    AllocStringLen alloc_string_len;
    FreeString free_string;
    CreateVector create_vector;
    PutElement put_element;
    DestroyArray destroy_array;
    ReservedValue reserved_not_supported;
};

namespace {

UiaRuntime g_runtime;
bool g_runtime_loaded;
bool g_runtime_failed;

template <typename T> bool resolve(HMODULE module, const char* name, T* target) noexcept {
    *target = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    return *target != nullptr;
}

const UiaRuntime* load_runtime() noexcept {
    if (g_runtime_loaded)
        return &g_runtime;
    if (g_runtime_failed)
        return nullptr;
    UiaRuntime runtime{};
    runtime.core = LoadLibraryExW(L"OleAut32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    runtime.automation = LoadLibraryExW(L"UIAutomationCore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    bool ok = runtime.core && runtime.automation &&
              resolve(runtime.automation, "UiaReturnRawElementProvider", &runtime.return_raw_element_provider) &&
              resolve(runtime.automation, "UiaHostProviderFromHwnd", &runtime.host_provider_from_hwnd) &&
              resolve(runtime.automation, "UiaRaiseAutomationEvent", &runtime.raise_automation_event) &&
              resolve(runtime.automation, "UiaRaiseAutomationPropertyChangedEvent",
                      &runtime.raise_property_changed_event) &&
              resolve(runtime.automation, "UiaRaiseStructureChangedEvent", &runtime.raise_structure_changed_event) &&
              resolve(runtime.automation, "UiaClientsAreListening", &runtime.clients_are_listening) &&
              resolve(runtime.core, "SysAllocStringLen", &runtime.alloc_string_len) &&
              resolve(runtime.core, "SysFreeString", &runtime.free_string) &&
              resolve(runtime.core, "SafeArrayCreateVector", &runtime.create_vector) &&
              resolve(runtime.core, "SafeArrayPutElement", &runtime.put_element) &&
              resolve(runtime.core, "SafeArrayDestroy", &runtime.destroy_array);
    if (ok) {
        // Optional (Windows 10 1709+ / Windows 8+).
        resolve(runtime.automation, "UiaRaiseNotificationEvent", &runtime.raise_notification_event);
        resolve(runtime.automation, "UiaDisconnectProvider", &runtime.disconnect_provider);
        resolve(runtime.automation, "UiaGetReservedNotSupportedValue", &runtime.reserved_not_supported);
    }
    if (!ok) {
        // Libraries stay loaded on success for the process lifetime: UI
        // Automation keeps references to our objects until it disconnects.
        if (runtime.automation)
            FreeLibrary(runtime.automation);
        if (runtime.core)
            FreeLibrary(runtime.core);
        g_runtime_failed = true;
        return nullptr;
    }
    g_runtime = runtime;
    g_runtime_loaded = true;
    return &g_runtime;
}

// ---- vtables ---------------------------------------------------------------

#define UIA_METHOD(name, ...) HRESULT(STDMETHODCALLTYPE* name)(void* self __VA_OPT__(, ) __VA_ARGS__)

struct UnknownVtbl {
    HRESULT(STDMETHODCALLTYPE* query_interface)(void* self, const GUID* iid, void** out);
    ULONG(STDMETHODCALLTYPE* add_ref)(void* self);
    ULONG(STDMETHODCALLTYPE* release)(void* self);
};
struct SimpleVtbl {
    UnknownVtbl unknown;
    UIA_METHOD(get_provider_options, int*);
    UIA_METHOD(get_pattern_provider, int, void**);
    UIA_METHOD(get_property_value, int, VARIANT*);
    UIA_METHOD(get_host_raw_element_provider, void**);
};
struct FragmentVtbl {
    UnknownVtbl unknown;
    UIA_METHOD(navigate, int, void**);
    UIA_METHOD(get_runtime_id, SAFEARRAY**);
    UIA_METHOD(get_bounding_rectangle, UiaBounds*);
    UIA_METHOD(get_embedded_fragment_roots, SAFEARRAY**);
    UIA_METHOD(set_focus);
    UIA_METHOD(get_fragment_root, void**);
};
struct FragmentRootVtbl {
    UnknownVtbl unknown;
    UIA_METHOD(element_provider_from_point, double, double, void**);
    UIA_METHOD(get_focus, void**);
};
struct InvokeVtbl {
    UnknownVtbl unknown;
    UIA_METHOD(invoke);
};
struct ToggleVtbl {
    UnknownVtbl unknown;
    UIA_METHOD(toggle);
    UIA_METHOD(get_toggle_state, int*);
};
struct SelectionVtbl {
    UnknownVtbl unknown;
    UIA_METHOD(get_selection, SAFEARRAY**);
    UIA_METHOD(get_can_select_multiple, BOOL*);
    UIA_METHOD(get_is_selection_required, BOOL*);
};
struct SelectionItemVtbl {
    UnknownVtbl unknown;
    UIA_METHOD(select);
    UIA_METHOD(add_to_selection);
    UIA_METHOD(remove_from_selection);
    UIA_METHOD(get_is_selected, BOOL*);
    UIA_METHOD(get_selection_container, void**);
};
struct ValueVtbl {
    UnknownVtbl unknown;
    UIA_METHOD(set_value, const wchar_t*);
    UIA_METHOD(get_value, BSTR*);
    UIA_METHOD(get_is_read_only, BOOL*);
};
struct RangeValueVtbl {
    UnknownVtbl unknown;
    UIA_METHOD(set_value, double);
    UIA_METHOD(get_value, double*);
    UIA_METHOD(get_is_read_only, BOOL*);
    UIA_METHOD(get_maximum, double*);
    UIA_METHOD(get_minimum, double*);
    UIA_METHOD(get_large_change, double*);
    UIA_METHOD(get_small_change, double*);
};
struct ScrollVtbl {
    UnknownVtbl unknown;
    UIA_METHOD(scroll, int, int);
    UIA_METHOD(set_scroll_percent, double, double);
    UIA_METHOD(get_horizontal_scroll_percent, double*);
    UIA_METHOD(get_vertical_scroll_percent, double*);
    UIA_METHOD(get_horizontal_view_size, double*);
    UIA_METHOD(get_vertical_view_size, double*);
    UIA_METHOD(get_horizontally_scrollable, BOOL*);
    UIA_METHOD(get_vertically_scrollable, BOOL*);
};
struct ExpandCollapseVtbl {
    UnknownVtbl unknown;
    UIA_METHOD(expand);
    UIA_METHOD(collapse);
    UIA_METHOD(get_expand_collapse_state, int*);
};
struct ScrollItemVtbl {
    UnknownVtbl unknown;
    UIA_METHOD(scroll_into_view);
};
struct GridVtbl {
    UnknownVtbl unknown;
    UIA_METHOD(get_item, int, int, void**);
    UIA_METHOD(get_row_count, int*);
    UIA_METHOD(get_column_count, int*);
};
struct GridItemVtbl {
    UnknownVtbl unknown;
    UIA_METHOD(get_row, int*);
    UIA_METHOD(get_column, int*);
    UIA_METHOD(get_row_span, int*);
    UIA_METHOD(get_column_span, int*);
    UIA_METHOD(get_containing_grid, void**);
};
// UiaPoint travels by value (by hidden reference on x64, as in the SDK).
struct UiaPointValue {
    double x, y;
};
struct TextVtbl {
    UnknownVtbl unknown;
    UIA_METHOD(get_selection, SAFEARRAY**);
    UIA_METHOD(get_visible_ranges, SAFEARRAY**);
    UIA_METHOD(range_from_child, void*, void**);
    UIA_METHOD(range_from_point, UiaPointValue, void**);
    UIA_METHOD(get_document_range, void**);
    UIA_METHOD(get_supported_text_selection, int*);
};
struct TextRangeVtbl {
    UnknownVtbl unknown;
    UIA_METHOD(clone, void**);
    UIA_METHOD(compare, void*, BOOL*);
    UIA_METHOD(compare_endpoints, int, void*, int, int*);
    UIA_METHOD(expand_to_enclosing_unit, int);
    UIA_METHOD(find_attribute, int, VARIANT, BOOL, void**);
    UIA_METHOD(find_text, BSTR, BOOL, BOOL, void**);
    UIA_METHOD(get_attribute_value, int, VARIANT*);
    UIA_METHOD(get_bounding_rectangles, SAFEARRAY**);
    UIA_METHOD(get_enclosing_element, void**);
    UIA_METHOD(get_text, int, BSTR*);
    UIA_METHOD(move, int, int, int*);
    UIA_METHOD(move_endpoint_by_unit, int, int, int, int*);
    UIA_METHOD(move_endpoint_by_range, int, void*, int);
    UIA_METHOD(select);
    UIA_METHOD(add_to_selection);
    UIA_METHOD(remove_from_selection);
    UIA_METHOD(scroll_into_view, BOOL);
    UIA_METHOD(get_children, SAFEARRAY**);
};
#undef UIA_METHOD

struct Element {
    const SimpleVtbl* simple;
    const FragmentVtbl* fragment;
    const FragmentRootVtbl* fragment_root;
    const InvokeVtbl* invoke;
    const ToggleVtbl* toggle;
    const SelectionVtbl* selection;
    const SelectionItemVtbl* selection_item;
    const ValueVtbl* value;
    const RangeValueVtbl* range_value;
    const ScrollVtbl* scroll;
    const ExpandCollapseVtbl* expand_collapse;
    const ScrollItemVtbl* scroll_item;
    const GridVtbl* grid;
    const GridItemVtbl* grid_item;
    const TextVtbl* text;
    volatile LONG references;
    UiaProviderHost* host;
    u64 key;
    bool root;
};

template <usize Offset> Element* element_from(void* self) noexcept {
    return reinterpret_cast<Element*>(static_cast<char*>(self) - Offset);
}

void* element_create(UiaProviderHost* host, u64 key, bool root) noexcept;

constexpr HRESULT kTimeout = static_cast<HRESULT>(0x80131505L);
constexpr WPARAM kCallSignature = 0x4D434155u;
constexpr UINT kCallTimeoutMs = 3000;

using WindowCallFunction = HRESULT (*)(const void* context) noexcept;
struct WindowCall {
    WindowCallFunction run;
    const void* context;
    HRESULT result;
    volatile LONG done;
};

// Runs run(context) on the window thread. A call that times out before the
// window thread takes it is withdrawn; one already taken is waited for, so the
// context never outlives the caller's frame. One shared body serves every
// marshaled COM method; each method only adds its typed thunk.
__declspec(noinline) HRESULT run_on_window_thread(UiaProviderHost* host, WindowCallFunction run,
                                                  const void* context) noexcept {
    if (!host->window || !host->call_message || GetCurrentThreadId() == host->thread)
        return run(context);
    WindowCall call{run, context, kTimeout, 0};
    AcquireSRWLockExclusive(host->call_lock.native());
    InterlockedExchangePointer(const_cast<void**>(&host->pending_call), &call);
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(host->window, host->call_message, kCallSignature, 0, SMTO_NORMAL, kCallTimeoutMs, &ignored);
    if (InterlockedCompareExchangePointer(const_cast<void**>(&host->pending_call), nullptr, &call) != &call) {
        // The window thread took the call; wait for it to finish.
        while (!InterlockedCompareExchange(&call.done, 0, 0))
            SwitchToThread();
    } else {
        call.result = kTimeout;
    }
    ReleaseSRWLockExclusive(host->call_lock.native());
    InterlockedIncrement(&host->marshaled);
    return call.result;
}

template <typename F> HRESULT on_window_thread(UiaProviderHost* host, const F& f) noexcept {
    return run_on_window_thread(
        host, [](const void* context) noexcept -> HRESULT { return (*static_cast<const F*>(context))(); }, &f);
}

// One shared body per distinct argument list: a thunk only forwards its
// method and the distance from its interface pointer to the owning host.
template <typename... Args>
__declspec(noinline) HRESULT marshal_call(void* self, Args... args,
                                          HRESULT(STDMETHODCALLTYPE* method)(void*, Args...) noexcept,
                                          usize host_distance) noexcept {
    auto* host = *reinterpret_cast<UiaProviderHost**>(static_cast<char*>(self) + host_distance);
    return on_window_thread(host, [&]() noexcept -> HRESULT { return method(self, args...); });
}

template <usize HostDistance, auto Method> struct Marshal;
template <usize HostDistance, typename... Args, HRESULT(STDMETHODCALLTYPE* Method)(void*, Args...) noexcept>
struct Marshal<HostDistance, Method> {
    static HRESULT STDMETHODCALLTYPE call(void* self, Args... args) noexcept {
        return marshal_call<Args...>(self, args..., Method, HostDistance);
    }
};

ULONG element_add_ref(Element* element) noexcept {
    return static_cast<ULONG>(InterlockedIncrement(&element->references));
}

ULONG element_release(Element* element) noexcept {
    const LONG remaining = InterlockedDecrement(&element->references);
    if (remaining == 0) {
        InterlockedDecrement(&element->host->elements);
        HeapFree(GetProcessHeap(), 0, element);
    }
    return static_cast<ULONG>(remaining);
}

struct Resolved {
    const UiAccessTree* tree;
    const UiAccessNode* node;
    u32 index;
};

bool resolve_element(const Element* element, Resolved* out) noexcept {
    auto* host = element->host;
    if (!host->connected)
        return false;
    const UiAccessTree* tree = host->callbacks.tree(host->callbacks.context);
    if (!tree || !tree->finished || !tree->count)
        return false;
    const u32 index = element->root ? 0 : ui_access_find(*tree, element->key);
    if (index == kUiAccessNone)
        return false;
    *out = {tree, &tree->nodes[index], index};
    return true;
}

bool supports(const Resolved& r, int pattern) noexcept {
    const auto& node = *r.node;
    switch (pattern) {
    case pattern_invoke:
        return (node.actions & ui_access_action_invoke) != 0;
    case pattern_toggle:
        return (node.actions & ui_access_action_toggle) != 0;
    case pattern_selection:
        // Radio groups are groups that select among their children.
        return node.role == UiAccessRole::list || node.role == UiAccessRole::tree ||
               node.role == UiAccessRole::tab_list ||
               (node.role == UiAccessRole::group && (node.actions & ui_access_action_select));
    case pattern_selection_item:
        return ((node.actions & ui_access_action_select) != 0 || (node.state & ui_access_selectable) != 0) &&
               node.role != UiAccessRole::group && node.role != UiAccessRole::list &&
               node.role != UiAccessRole::tab_list && node.role != UiAccessRole::tree;
    case pattern_value:
        return (node.actions & ui_access_action_value) != 0;
    case pattern_range_value:
        return (node.actions & ui_access_action_range) != 0;
    case pattern_scroll:
        return (node.actions & ui_access_action_scroll) != 0;
    case pattern_expand_collapse:
        return (node.actions & ui_access_action_expand) != 0 ||
               (node.state & (ui_access_expanded | ui_access_collapsed)) != 0;
    case pattern_scroll_item:
        return (node.actions & ui_access_action_scroll_item) != 0;
    case pattern_grid:
        return (node.actions & ui_access_action_grid) != 0;
    case pattern_grid_item:
        return (node.actions & ui_access_action_grid_item) != 0;
    case pattern_text:
        // Never over a password: the range text would disclose it.
        return (node.actions & ui_access_action_text) != 0 && !(node.state & ui_access_password);
    default:
        return false;
    }
}

// UiAccessRole order.
constexpr u16 kControlTypes[]{
    control_custom,      control_window,     control_pane,      control_group,        control_window,
    control_button,      control_button,     control_check_box, control_radio_button, control_combo_box,
    control_list,        control_list_item,  control_tree,      control_tree_item,    control_tab,
    control_tab_item,    control_menu,       control_menu_item, control_text,         control_header,
    control_edit,        control_slider,     control_image,     control_hyperlink,    control_document,
    control_status_bar,  control_scroll_bar};
static_assert(sizeof(kControlTypes) / sizeof(kControlTypes[0]) == static_cast<usize>(UiAccessRole::count));

int control_type(UiAccessRole role) noexcept {
    const u32 index = static_cast<u32>(role);
    return index < static_cast<u32>(UiAccessRole::count) ? kControlTypes[index] : control_custom;
}

BSTR make_bstr(const UiaRuntime* runtime, Utf8View text) noexcept {
    if (!runtime)
        return nullptr;
    if (!text.bytes)
        return runtime->alloc_string_len(L"", 0);
    const int units = MultiByteToWideChar(CP_UTF8, 0, text.data, static_cast<int>(text.bytes), nullptr, 0);
    if (units <= 0)
        return nullptr;
    BSTR result = runtime->alloc_string_len(nullptr, static_cast<UINT>(units));
    if (result)
        MultiByteToWideChar(CP_UTF8, 0, text.data, static_cast<int>(text.bytes), result, units);
    return result;
}

bool variant_string(const UiaRuntime* runtime, VARIANT* out, Utf8View text) noexcept {
    if (!text.bytes)
        return false;
    BSTR value = make_bstr(runtime, text);
    if (!value)
        return false;
    V_VT(out) = VT_BSTR;
    V_BSTR(out) = value;
    return true;
}

bool variant_text(const UiaRuntime* runtime, VARIANT* out, Utf8View text) noexcept {
    if (!variant_string(runtime, out, text)) {
        V_VT(out) = VT_BSTR;
        V_BSTR(out) = make_bstr(runtime, {});
    }
    return V_BSTR(out) != nullptr;
}

void variant_bool(VARIANT* out, bool value) noexcept {
    V_VT(out) = VT_BOOL;
    V_BOOL(out) = value ? VARIANT_TRUE : VARIANT_FALSE;
}

void variant_int(VARIANT* out, int value) noexcept {
    V_VT(out) = VT_I4;
    V_I4(out) = value;
}

void variant_double(VARIANT* out, double value) noexcept {
    V_VT(out) = VT_R8;
    V_R8(out) = value;
}

int toggle_state(u16 state) noexcept {
    return (state & ui_access_mixed) ? 2 : (state & ui_access_checked) ? 1 : 0;
}

int expand_state(const UiAccessNode& node) noexcept {
    return (node.state & ui_access_expanded) ? 1 : (node.state & ui_access_collapsed) ? 0 : 3;
}

HRESULT request(Element* element, UiAccessAction action, double number, const Utf8View& text) noexcept {
    Resolved r{};
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    if ((r.node->state & ui_access_disabled) && action != UiAccessAction::focus &&
        action != UiAccessAction::scroll_into_view)
        return kElementNotEnabled;
    auto* host = element->host;
    if (!host->callbacks.act ||
        !host->callbacks.act(host->callbacks.context, r.node->key, action, number, text))
        return kInvalidOperation;
    return S_OK;
}

__declspec(noinline) HRESULT request(Element* element, UiAccessAction action) noexcept {
    return request(element, action, 0, Utf8View{});
}

void* element_for_index(Element* from, const Resolved& r, u32 index) noexcept {
    if (index == kUiAccessNone)
        return nullptr;
    return element_create(from->host, index == 0 ? 0 : r.tree->nodes[index].key, index == 0);
}

// IUnknown, per interface offset.
template <usize Offset> HRESULT STDMETHODCALLTYPE query_interface(void* self, const GUID* iid, void** out) noexcept;
template <usize Offset> ULONG STDMETHODCALLTYPE add_ref(void* self) noexcept {
    return element_add_ref(element_from<Offset>(self));
}
template <usize Offset> ULONG STDMETHODCALLTYPE release(void* self) noexcept {
    return element_release(element_from<Offset>(self));
}
template <usize Offset> constexpr UnknownVtbl unknown_vtbl() noexcept {
    return {&query_interface<Offset>, &add_ref<Offset>, &release<Offset>};
}

constexpr usize kSimple = offsetof(Element, simple);
constexpr usize kFragment = offsetof(Element, fragment);
constexpr usize kFragmentRoot = offsetof(Element, fragment_root);
constexpr usize kInvoke = offsetof(Element, invoke);
constexpr usize kToggle = offsetof(Element, toggle);
constexpr usize kSelection = offsetof(Element, selection);
constexpr usize kSelectionItem = offsetof(Element, selection_item);
constexpr usize kValue = offsetof(Element, value);
constexpr usize kRangeValue = offsetof(Element, range_value);
constexpr usize kScroll = offsetof(Element, scroll);
constexpr usize kExpandCollapse = offsetof(Element, expand_collapse);
constexpr usize kScrollItem = offsetof(Element, scroll_item);
constexpr usize kGrid = offsetof(Element, grid);
constexpr usize kGridItem = offsetof(Element, grid_item);
constexpr usize kText = offsetof(Element, text);

struct Pattern {
    const GUID* iid;
    int id;
    usize offset;
};
constexpr Pattern kPatterns[]{
    {&kIidInvoke, pattern_invoke, kInvoke},
    {&kIidToggle, pattern_toggle, kToggle},
    {&kIidSelection, pattern_selection, kSelection},
    {&kIidSelectionItem, pattern_selection_item, kSelectionItem},
    {&kIidValue, pattern_value, kValue},
    {&kIidRangeValue, pattern_range_value, kRangeValue},
    {&kIidScroll, pattern_scroll, kScroll},
    {&kIidExpandCollapse, pattern_expand_collapse, kExpandCollapse},
    {&kIidScrollItem, pattern_scroll_item, kScrollItem},
    {&kIidGrid, pattern_grid, kGrid},
    {&kIidGridItem, pattern_grid_item, kGridItem},
    {&kIidText, pattern_text, kText},
};

void* pattern_interface(Element* element, const GUID& iid) noexcept {
    for (const auto& pattern : kPatterns) {
        if (!guid_equal(iid, *pattern.iid))
            continue;
        Resolved r{};
        if (!resolve_element(element, &r) || !supports(r, pattern.id))
            return nullptr;
        return reinterpret_cast<char*>(element) + pattern.offset;
    }
    return nullptr;
}

// The identity interfaces depend only on fields fixed at creation, so the
// calling thread answers them; pattern support reads the snapshot on the
// window thread.
__declspec(noinline) HRESULT element_query(Element* element, const GUID* iid, void** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (!iid)
        return E_INVALIDARG;
    void* result = nullptr;
    if (guid_equal(*iid, kIidUnknown) || guid_equal(*iid, kIidSimple)) {
        result = &element->simple;
    } else if (guid_equal(*iid, kIidFragment)) {
        result = &element->fragment;
    } else if (guid_equal(*iid, kIidFragmentRoot)) {
        result = element->root ? &element->fragment_root : nullptr;
    } else {
        on_window_thread(element->host, [&]() noexcept -> HRESULT {
            result = pattern_interface(element, *iid);
            return S_OK;
        });
    }
    if (!result)
        return E_NOINTERFACE;
    element_add_ref(element);
    *out = result;
    return S_OK;
}
template <usize Offset> HRESULT STDMETHODCALLTYPE query_interface(void* self, const GUID* iid, void** out) noexcept {
    return element_query(element_from<Offset>(self), iid, out);
}

// IRawElementProviderSimple
HRESULT STDMETHODCALLTYPE simple_provider_options(void*, int* out) noexcept {
    if (!out)
        return E_POINTER;
    *out = kProviderOptionsServerSide;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE simple_pattern_provider(void* self, int pattern, void** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* element = element_from<kSimple>(self);
    Resolved r{};
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    if (!supports(r, pattern))
        return S_OK;
    for (const auto& entry : kPatterns) {
        if (entry.id != pattern)
            continue;
        element_add_ref(element);
        *out = reinterpret_cast<char*>(element) + entry.offset;
        break;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE simple_property_value(void* self, int property, VARIANT* out) noexcept {
    if (!out)
        return E_POINTER;
    V_VT(out) = VT_EMPTY;
    auto* element = element_from<kSimple>(self);
    Resolved r{};
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    const auto& node = *r.node;
    const auto* runtime = element->host->runtime;
    switch (property) {
    case property_control_type:
        if (!element->root)
            variant_int(out, control_type(node.role));
        break;
    case property_name:
        variant_string(runtime, out, ui_access_name(*r.tree, r.index));
        break;
    case property_automation_id:
        variant_string(runtime, out, ui_access_automation_id(*r.tree, r.index));
        break;
    case property_help_text:
        variant_string(runtime, out, ui_access_help(*r.tree, r.index));
        break;
    case property_class_name:
        if (!element->root)
            variant_string(runtime, out, utf8_literal("MaiCordElement"));
        break;
    case property_has_keyboard_focus:
        if (!element->root)
            variant_bool(out, (node.state & ui_access_focused) != 0 &&
                                  (!element->host->window || GetFocus() == element->host->window));
        break;
    case property_is_keyboard_focusable:
        if (!element->root)
            variant_bool(out, (node.state & (ui_access_focusable | ui_access_focused)) != 0);
        break;
    case property_is_enabled:
        variant_bool(out, (node.state & ui_access_disabled) == 0);
        break;
    case property_is_offscreen:
        if (!element->root)
            variant_bool(out, (node.state & ui_access_offscreen) != 0);
        break;
    case property_is_password:
        variant_bool(out, (node.state & ui_access_password) != 0);
        break;
    case property_is_control_element:
    case property_is_content_element:
        variant_bool(out, true);
        break;
    case property_framework_id:
        variant_string(runtime, out, utf8_literal("MaiCord"));
        break;
    case property_provider_description:
        variant_string(runtime, out, utf8_literal("MaiCord UI Automation provider"));
        break;
    case property_live_setting:
        if (node.state & ui_access_live)
            variant_int(out, 1);
        break;
    case property_is_dialog:
        if (node.role == UiAccessRole::dialog)
            variant_bool(out, true);
        break;
    case property_position_in_set:
        if (!element->root && node.placement.position)
            variant_int(out, static_cast<int>(node.placement.position));
        break;
    case property_size_of_set:
        if (!element->root && node.placement.set_size)
            variant_int(out, static_cast<int>(node.placement.set_size));
        break;
    default:
        break;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE simple_host_provider(void* self, void** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* element = element_from<kSimple>(self);
    auto* host = element->host;
    if (!element->root || !host->connected || !host->runtime)
        return S_OK;
    return host->runtime->host_provider_from_hwnd(host->window, out);
}

// IRawElementProviderFragment
HRESULT STDMETHODCALLTYPE fragment_navigate(void* self, int direction, void** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* element = element_from<kFragment>(self);
    Resolved r{};
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    u32 target = kUiAccessNone;
    switch (direction) {
    case 0: target = element->root ? kUiAccessNone : r.node->parent; break;
    case 1: target = element->root ? kUiAccessNone : r.node->next_sibling; break;
    case 2: target = element->root ? kUiAccessNone : r.node->previous_sibling; break;
    case 3: target = r.node->first_child; break;
    case 4: target = r.node->last_child; break;
    default: return E_INVALIDARG;
    }
    void* found = element_for_index(element, r, target);
    if (found)
        *out = static_cast<char*>(found) + (kFragment - kSimple);
    return S_OK;
}

SAFEARRAY* runtime_id_array(const UiaRuntime* runtime, u64 key) noexcept {
    SAFEARRAY* array = runtime->create_vector(VT_I4, 0, 3);
    if (!array)
        return nullptr;
    int parts[3]{kUiaAppendRuntimeId, static_cast<int>(key & 0x7FFF'FFFFu), static_cast<int>((key >> 31) & 0x7FFF'FFFFu)};
    for (LONG index = 0; index < 3; ++index)
        runtime->put_element(array, &index, &parts[index]);
    return array;
}

HRESULT STDMETHODCALLTYPE fragment_runtime_id(void* self, SAFEARRAY** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* element = element_from<kFragment>(self);
    Resolved r{};
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    if (element->root)
        return S_OK;
    *out = runtime_id_array(element->host->runtime, r.node->key);
    return *out ? S_OK : E_OUTOFMEMORY;
}

float host_scale(const UiaProviderHost* host) noexcept {
    return host->callbacks.scale ? host->callbacks.scale(host->callbacks.context) : 1.0f;
}

UiaBounds screen_bounds(const UiaProviderHost* host, const UiRect& rectangle) noexcept {
    const float scale = host_scale(host);
    POINT origin{0, host->content_offset_y};
    ClientToScreen(host->window, &origin);
    UiaBounds bounds{};
    bounds.left = static_cast<double>(origin.x) + static_cast<double>(rectangle.left * scale);
    bounds.top = static_cast<double>(origin.y) + static_cast<double>(rectangle.top * scale);
    bounds.width = static_cast<double>(maximum(0.0f, (rectangle.right - rectangle.left) * scale));
    bounds.height = static_cast<double>(maximum(0.0f, (rectangle.bottom - rectangle.top) * scale));
    return bounds;
}

HRESULT STDMETHODCALLTYPE fragment_bounds(void* self, UiaBounds* out) noexcept {
    if (!out)
        return E_POINTER;
    reset_value(out);
    auto* element = element_from<kFragment>(self);
    Resolved r{};
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    if (element->root || (r.node->state & ui_access_offscreen))
        return S_OK;
    *out = screen_bounds(element->host, r.node->visible);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fragment_embedded_roots(void*, SAFEARRAY** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fragment_set_focus(void* self) noexcept {
    auto* element = element_from<kFragment>(self);
    return element->root ? S_OK : request(element, UiAccessAction::focus);
}

HRESULT STDMETHODCALLTYPE fragment_root(void* self, void** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* element = element_from<kFragment>(self);
    if (!element->host->connected)
        return kElementNotAvailable;
    void* root = element_create(element->host, 0, true);
    if (root)
        *out = static_cast<char*>(root) + (kFragmentRoot - kSimple);
    return root ? S_OK : E_OUTOFMEMORY;
}

// IRawElementProviderFragmentRoot
HRESULT STDMETHODCALLTYPE root_from_point(void* self, double x, double y, void** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* element = element_from<kFragmentRoot>(self);
    Resolved r{};
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    POINT point{static_cast<LONG>(x), static_cast<LONG>(y)};
    ScreenToClient(element->host->window, &point);
    point.y -= element->host->content_offset_y;
    const float scale = host_scale(element->host);
    const float divisor = scale > 0 ? scale : 1.0f;
    const float dip_x = static_cast<float>(point.x) / divisor;
    const float dip_y = static_cast<float>(point.y) / divisor;
    const u32 hit = ui_access_hit_test(*r.tree, dip_x, dip_y);
    void* found = element_for_index(element, r, hit == kUiAccessNone ? 0 : hit);
    if (found)
        *out = static_cast<char*>(found) + (kFragment - kSimple);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE root_focus(void* self, void** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* element = element_from<kFragmentRoot>(self);
    Resolved r{};
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    if (r.tree->focus == kUiAccessNone || r.tree->focus == 0)
        return S_OK;
    void* found = element_for_index(element, r, r.tree->focus);
    if (found)
        *out = static_cast<char*>(found) + (kFragment - kSimple);
    return S_OK;
}

// Patterns
HRESULT STDMETHODCALLTYPE invoke_invoke(void* self) noexcept {
    return request(element_from<kInvoke>(self), UiAccessAction::invoke);
}

HRESULT STDMETHODCALLTYPE toggle_toggle(void* self) noexcept {
    return request(element_from<kToggle>(self), UiAccessAction::toggle);
}

// The getters that read one value of the resolved node share this body per value type.
template <typename T>
__declspec(noinline) HRESULT read_node(Element* element, T* out, T (*read)(const UiAccessNode&) noexcept) noexcept {
    if (!out)
        return E_POINTER;
    Resolved r{};
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    *out = read(*r.node);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE toggle_state_get(void* self, int* out) noexcept {
    return read_node<int>(element_from<kToggle>(self), out,
                          [](const UiAccessNode& node) noexcept -> int { return toggle_state(node.state); });
}

HRESULT STDMETHODCALLTYPE selection_get(void* self, SAFEARRAY** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* element = element_from<kSelection>(self);
    Resolved r{};
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    const auto* runtime = element->host->runtime;
    u32 selected = 0;
    for (u32 child = r.node->first_child; child != kUiAccessNone; child = r.tree->nodes[child].next_sibling)
        selected += (r.tree->nodes[child].state & ui_access_selected) ? 1u : 0u;
    SAFEARRAY* array = runtime->create_vector(VT_UNKNOWN, 0, selected);
    if (!array)
        return E_OUTOFMEMORY;
    LONG position = 0;
    for (u32 child = r.node->first_child; child != kUiAccessNone; child = r.tree->nodes[child].next_sibling) {
        if (!(r.tree->nodes[child].state & ui_access_selected))
            continue;
        void* item = element_for_index(element, r, child);
        if (item) {
            runtime->put_element(array, &position, item); // SafeArrayPutElement AddRefs
            element_release(element_from<kSimple>(item));
        }
        ++position;
    }
    *out = array;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE out_false(void*, BOOL* out) noexcept {
    if (!out)
        return E_POINTER;
    *out = FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE selection_item_select(void* self) noexcept {
    return request(element_from<kSelectionItem>(self), UiAccessAction::select);
}

HRESULT STDMETHODCALLTYPE selection_item_add(void* self) noexcept {
    Resolved r{};
    auto* element = element_from<kSelectionItem>(self);
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    return (r.node->state & ui_access_selected) ? S_OK : request(element, UiAccessAction::select);
}

HRESULT STDMETHODCALLTYPE selection_item_remove(void* self) noexcept {
    Resolved r{};
    if (!resolve_element(element_from<kSelectionItem>(self), &r))
        return kElementNotAvailable;
    return (r.node->state & ui_access_selected) ? kInvalidOperation : S_OK;
}

HRESULT STDMETHODCALLTYPE selection_item_selected(void* self, BOOL* out) noexcept {
    return read_node<int>(element_from<kSelectionItem>(self), out, [](const UiAccessNode& node) noexcept -> int {
        return (node.state & ui_access_selected) ? TRUE : FALSE;
    });
}

HRESULT STDMETHODCALLTYPE selection_item_container(void* self, void** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* element = element_from<kSelectionItem>(self);
    Resolved r{};
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    const u32 container = ui_access_ancestor(*r.tree, r.index,
                                             ui_access_role_bit(UiAccessRole::list) |
                                                 ui_access_role_bit(UiAccessRole::tree) |
                                                 ui_access_role_bit(UiAccessRole::tab_list) |
                                                 ui_access_role_bit(UiAccessRole::menu) |
                                                 ui_access_role_bit(UiAccessRole::group));
    *out = element_for_index(element, r, container);
    return S_OK;
}

// UTF-8 bytes of a NUL-terminated UTF-16 argument (capped at the value limit); 0 when empty, <= 0 on failure.
__declspec(noinline) int wide_argument_utf8(const wchar_t* text, char* output, int capacity) noexcept {
    int units = 0;
    while (units < static_cast<int>(kUiAccessValueMaximumBytes) && text[units])
        ++units;
    return units ? WideCharToMultiByte(CP_UTF8, 0, text, units, output, capacity, nullptr, nullptr) : 0;
}

HRESULT STDMETHODCALLTYPE value_set(void* self, const wchar_t* text) noexcept {
    auto* element = element_from<kValue>(self);
    Resolved r{};
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    if (r.node->state & ui_access_read_only)
        return kInvalidOperation;
    if (!text)
        return E_INVALIDARG;
    ScratchPages<char[kUiAccessValueMaximumBytes * 3]> scratch;
    if (!scratch.data)
        return E_OUTOFMEMORY;
    char* utf8 = *scratch.data;
    const int bytes = wide_argument_utf8(text, utf8, static_cast<int>(sizeof(*scratch.data)));
    if (text[0] && bytes <= 0)
        return E_INVALIDARG;
    return request(element, UiAccessAction::set_value, 0, Utf8View{utf8, static_cast<u32>(bytes)});
}

HRESULT STDMETHODCALLTYPE value_get(void* self, BSTR* out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* element = element_from<kValue>(self);
    Resolved r{};
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    if (r.node->state & ui_access_password)
        return kInvalidOperation;
    *out = make_bstr(element->host->runtime, ui_access_value(*r.tree, r.index));
    return *out ? S_OK : E_OUTOFMEMORY;
}

template <usize Offset> HRESULT STDMETHODCALLTYPE read_only_get(void* self, BOOL* out) noexcept {
    return read_node<int>(element_from<Offset>(self), out, [](const UiAccessNode& node) noexcept -> int {
        return (node.state & ui_access_read_only) ? TRUE : FALSE;
    });
}

HRESULT STDMETHODCALLTYPE range_set(void* self, double value) noexcept {
    auto* element = element_from<kRangeValue>(self);
    Resolved r{};
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    if (r.node->state & ui_access_read_only)
        return kInvalidOperation;
    if (value < r.node->range.minimum || value > r.node->range.maximum)
        return E_INVALIDARG;
    return request(element, UiAccessAction::set_range, value, Utf8View{});
}

HRESULT STDMETHODCALLTYPE range_value_get(void* self, double* out) noexcept {
    return read_node<double>(element_from<kRangeValue>(self), out, [](const UiAccessNode& node) noexcept -> double {
        return node.range.value;
    });
}
HRESULT STDMETHODCALLTYPE range_minimum(void* self, double* out) noexcept {
    return read_node<double>(element_from<kRangeValue>(self), out, [](const UiAccessNode& node) noexcept -> double {
        return node.range.minimum;
    });
}
HRESULT STDMETHODCALLTYPE range_maximum(void* self, double* out) noexcept {
    return read_node<double>(element_from<kRangeValue>(self), out, [](const UiAccessNode& node) noexcept -> double {
        return node.range.maximum;
    });
}
HRESULT STDMETHODCALLTYPE range_step(void* self, double* out) noexcept {
    return read_node<double>(element_from<kRangeValue>(self), out, [](const UiAccessNode& node) noexcept -> double {
        return node.range.step;
    });
}
HRESULT STDMETHODCALLTYPE range_large_step(void* self, double* out) noexcept {
    return read_node<double>(element_from<kRangeValue>(self), out, [](const UiAccessNode& node) noexcept -> double {
        return node.range.large_step > 0 ? node.range.large_step : node.range.step;
    });
}

HRESULT STDMETHODCALLTYPE scroll_scroll(void* self, int horizontal, int vertical) noexcept {
    // ScrollAmount: 0 large decrement, 1 small decrement, 2 none, 3 large increment, 4 small increment.
    if (horizontal != 2)
        return kInvalidOperation;
    UiAccessScroll amount = UiAccessScroll::none;
    switch (vertical) {
    case 0: amount = UiAccessScroll::large_decrement; break;
    case 1: amount = UiAccessScroll::small_decrement; break;
    case 3: amount = UiAccessScroll::large_increment; break;
    case 4: amount = UiAccessScroll::small_increment; break;
    default: return S_OK;
    }
    return request(element_from<kScroll>(self), UiAccessAction::scroll, static_cast<double>(amount), Utf8View{});
}

HRESULT STDMETHODCALLTYPE scroll_set_percent(void*, double horizontal, double vertical) noexcept {
    return horizontal == kScrollNoScroll && vertical == kScrollNoScroll ? S_OK : kInvalidOperation;
}

HRESULT STDMETHODCALLTYPE scroll_no_scroll(void*, double* out) noexcept {
    if (!out)
        return E_POINTER;
    *out = kScrollNoScroll;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE scroll_full_view(void*, double* out) noexcept {
    if (!out)
        return E_POINTER;
    *out = 100.0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE scroll_vertical_percent(void* self, double* out) noexcept {
    return read_node<double>(element_from<kScroll>(self), out, [](const UiAccessNode& node) noexcept -> double {
        return node.scroll.vertical_percent < 0 ? kScrollNoScroll : node.scroll.vertical_percent;
    });
}

HRESULT STDMETHODCALLTYPE scroll_vertical_view(void* self, double* out) noexcept {
    return read_node<double>(element_from<kScroll>(self), out, [](const UiAccessNode& node) noexcept -> double {
        return node.scroll.vertical_view_percent > 0 ? node.scroll.vertical_view_percent : 100.0;
    });
}

HRESULT STDMETHODCALLTYPE scroll_vertically(void* self, BOOL* out) noexcept {
    return read_node<int>(element_from<kScroll>(self), out, [](const UiAccessNode& node) noexcept -> int {
        return node.scroll.vertical_percent >= 0 ? TRUE : FALSE;
    });
}

HRESULT STDMETHODCALLTYPE expand_expand(void* self) noexcept {
    return request(element_from<kExpandCollapse>(self), UiAccessAction::expand);
}

HRESULT STDMETHODCALLTYPE expand_collapse_action(void* self) noexcept {
    return request(element_from<kExpandCollapse>(self), UiAccessAction::collapse);
}

HRESULT STDMETHODCALLTYPE expand_state_get(void* self, int* out) noexcept {
    return read_node<int>(element_from<kExpandCollapse>(self), out,
                          [](const UiAccessNode& node) noexcept -> int { return expand_state(node); });
}

// ScrollItem
HRESULT STDMETHODCALLTYPE scroll_item_into_view(void* self) noexcept {
    return request(element_from<kScrollItem>(self), UiAccessAction::scroll_into_view);
}

// Grid / GridItem
u32 grid_cell(const UiAccessTree& tree, u32 grid, int row, int column) noexcept {
    // Cells are children of the grid or of its direct children (sections).
    for (u32 child = tree.nodes[grid].first_child; child != kUiAccessNone; child = tree.nodes[child].next_sibling) {
        const auto& node = tree.nodes[child];
        if ((node.actions & ui_access_action_grid_item) && node.placement.grid_row == row &&
            node.placement.grid_column == column)
            return child;
        for (u32 cell = node.first_child; cell != kUiAccessNone; cell = tree.nodes[cell].next_sibling) {
            const auto& inner = tree.nodes[cell];
            if ((inner.actions & ui_access_action_grid_item) && inner.placement.grid_row == row &&
                inner.placement.grid_column == column)
                return cell;
        }
    }
    return kUiAccessNone;
}

HRESULT STDMETHODCALLTYPE grid_item_at(void* self, int row, int column, void** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* element = element_from<kGrid>(self);
    Resolved r{};
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    if (row < 0 || column < 0 || row >= r.node->placement.grid_row || column >= r.node->placement.grid_column)
        return E_INVALIDARG;
    const u32 cell = grid_cell(*r.tree, r.index, row, column);
    if (cell == kUiAccessNone)
        return E_INVALIDARG;
    *out = element_for_index(element, r, cell);
    return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE grid_rows(void* self, int* out) noexcept {
    return read_node<int>(element_from<kGrid>(self), out,
                          [](const UiAccessNode& node) noexcept -> int { return node.placement.grid_row; });
}
HRESULT STDMETHODCALLTYPE grid_columns(void* self, int* out) noexcept {
    return read_node<int>(element_from<kGrid>(self), out,
                          [](const UiAccessNode& node) noexcept -> int { return node.placement.grid_column; });
}

HRESULT STDMETHODCALLTYPE grid_item_row(void* self, int* out) noexcept {
    return read_node<int>(element_from<kGridItem>(self), out,
                          [](const UiAccessNode& node) noexcept -> int { return node.placement.grid_row; });
}
HRESULT STDMETHODCALLTYPE grid_item_column(void* self, int* out) noexcept {
    return read_node<int>(element_from<kGridItem>(self), out,
                          [](const UiAccessNode& node) noexcept -> int { return node.placement.grid_column; });
}
HRESULT STDMETHODCALLTYPE grid_item_span(void*, int* out) noexcept {
    if (!out)
        return E_POINTER;
    *out = 1;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE grid_item_container(void* self, void** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* element = element_from<kGridItem>(self);
    Resolved r{};
    if (!resolve_element(element, &r))
        return kElementNotAvailable;
    for (u32 current = r.node->parent; current != kUiAccessNone; current = r.tree->nodes[current].parent)
        if (r.tree->nodes[current].actions & ui_access_action_grid) {
            *out = element_for_index(element, r, current);
            break;
        }
    return S_OK;
}

// Text: ranges are byte offsets into the node value, revalidated (clamped to
// the current text and its cluster boundaries) on every call.
struct TextRange {
    const TextRangeVtbl* vtbl;
    volatile LONG references;
    UiaProviderHost* host;
    u64 key;
    u32 start, end;
};

void* range_create(UiaProviderHost* host, u64 key, u32 start, u32 end) noexcept;
bool is_range(const void* unknown) noexcept;
ULONG STDMETHODCALLTYPE range_release(void* self) noexcept;

struct RangeText {
    const UiAccessTree* tree;
    const UiAccessNode* node;
    u32 index;
    Utf8View text;
};

bool text_node(UiaProviderHost* host, u64 key, RangeText* out) noexcept {
    if (!host->connected)
        return false;
    const UiAccessTree* tree = host->callbacks.tree(host->callbacks.context);
    if (!tree || !tree->finished)
        return false;
    const u32 index = ui_access_find(*tree, key);
    if (index == kUiAccessNone)
        return false;
    const auto& node = tree->nodes[index];
    if (!(node.actions & ui_access_action_text) || (node.state & ui_access_password))
        return false;
    *out = {tree, &node, index, ui_access_value(*tree, index)};
    return true;
}

bool range_resolve(TextRange* range, RangeText* out) noexcept {
    if (!text_node(range->host, range->key, out))
        return false;
    const u32 length = out->text.bytes;
    range->start = ui_access_text_align(out->text, range->start < length ? range->start : length);
    range->end = ui_access_text_align(out->text, range->end < length ? range->end : length);
    if (range->end < range->start)
        range->end = range->start;
    return true;
}

void ordered_selection(const RangeText& text, u32* start, u32* end) noexcept {
    const u32 length = text.text.bytes;
    u32 anchor = text.node->placement.selection_anchor, caret = text.node->placement.selection_caret;
    anchor = ui_access_text_align(text.text, anchor < length ? anchor : length);
    caret = ui_access_text_align(text.text, caret < length ? caret : length);
    *start = anchor < caret ? anchor : caret;
    *end = anchor < caret ? caret : anchor;
}

HRESULT range_array(UiaProviderHost* host, u64 key, u32 start, u32 end, SAFEARRAY** out) noexcept {
    void* range = range_create(host, key, start, end);
    if (!range)
        return E_OUTOFMEMORY;
    SAFEARRAY* array = host->runtime->create_vector(VT_UNKNOWN, 0, 1);
    if (array) {
        LONG zero = 0;
        host->runtime->put_element(array, &zero, range); // AddRefs
    }
    range_release(range);
    *out = array;
    return array ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE text_selection(void* self, SAFEARRAY** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* element = element_from<kText>(self);
    RangeText text{};
    if (!text_node(element->host, element->key, &text))
        return kElementNotAvailable;
    u32 start = 0, end = 0;
    ordered_selection(text, &start, &end);
    return range_array(element->host, text.node->key, start, end, out);
}

HRESULT STDMETHODCALLTYPE text_visible_ranges(void* self, SAFEARRAY** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* element = element_from<kText>(self);
    RangeText text{};
    if (!text_node(element->host, element->key, &text))
        return kElementNotAvailable;
    return range_array(element->host, text.node->key, 0, text.text.bytes, out);
}

HRESULT text_range(void* self, void** out, bool caret) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* element = element_from<kText>(self);
    RangeText text{};
    if (!text_node(element->host, element->key, &text))
        return kElementNotAvailable;
    u32 start = 0, end = text.text.bytes;
    if (caret) {
        const u32 length = text.text.bytes;
        const u32 at = text.node->placement.selection_caret;
        start = end = ui_access_text_align(text.text, at < length ? at : length);
    }
    *out = range_create(element->host, text.node->key, start, end);
    return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE text_range_from_child(void* self, void* child, void** out) noexcept {
    if (!child)
        return E_INVALIDARG;
    // Text nodes have no embedded children: the whole document encloses any.
    return text_range(self, out, false);
}

HRESULT STDMETHODCALLTYPE text_range_from_point(void* self, UiaPointValue, void** out) noexcept {
    return text_range(self, out, true);
}

HRESULT STDMETHODCALLTYPE text_document_range(void* self, void** out) noexcept {
    return text_range(self, out, false);
}

HRESULT STDMETHODCALLTYPE text_supported_selection(void*, int* out) noexcept {
    if (!out)
        return E_POINTER;
    *out = 1; // SupportedTextSelection_Single
    return S_OK;
}

HRESULT STDMETHODCALLTYPE range_query_interface(void* self, const GUID* iid, void** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (!iid)
        return E_INVALIDARG;
    if (!guid_equal(*iid, kIidUnknown) && !guid_equal(*iid, kIidTextRange))
        return E_NOINTERFACE;
    InterlockedIncrement(&static_cast<TextRange*>(self)->references);
    *out = self;
    return S_OK;
}
ULONG STDMETHODCALLTYPE range_add_ref(void* self) noexcept {
    return static_cast<ULONG>(InterlockedIncrement(&static_cast<TextRange*>(self)->references));
}
ULONG STDMETHODCALLTYPE range_release(void* self) noexcept {
    auto* range = static_cast<TextRange*>(self);
    const LONG remaining = InterlockedDecrement(&range->references);
    if (remaining == 0) {
        InterlockedDecrement(&range->host->ranges);
        HeapFree(GetProcessHeap(), 0, range);
    }
    return static_cast<ULONG>(remaining);
}

UiAccessTextUnit text_unit(int unit) noexcept {
    return unit <= 0 ? UiAccessTextUnit::character
           : unit >= 6 ? UiAccessTextUnit::document
                       : static_cast<UiAccessTextUnit>(unit);
}

HRESULT STDMETHODCALLTYPE range_clone(void* self, void** out) noexcept {
    if (!out)
        return E_POINTER;
    auto* range = static_cast<TextRange*>(self);
    *out = range_create(range->host, range->key, range->start, range->end);
    return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE range_compare(void* self, void* other, BOOL* out) noexcept {
    if (!out)
        return E_POINTER;
    *out = FALSE;
    if (!is_range(other))
        return E_INVALIDARG;
    auto* range = static_cast<TextRange*>(self);
    auto* target = static_cast<TextRange*>(other);
    RangeText text{}, target_text{};
    if (!range_resolve(range, &text) || !range_resolve(target, &target_text))
        return kElementNotAvailable;
    *out = range->key == target->key && range->start == target->start && range->end == target->end ? TRUE : FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE range_compare_endpoints(void* self, int endpoint, void* other, int other_endpoint,
                                                  int* out) noexcept {
    if (!out)
        return E_POINTER;
    *out = 0;
    if (!is_range(other))
        return E_INVALIDARG;
    auto* range = static_cast<TextRange*>(self);
    auto* target = static_cast<TextRange*>(other);
    RangeText text{}, target_text{};
    if (!range_resolve(range, &text) || !range_resolve(target, &target_text))
        return kElementNotAvailable;
    if (range->key != target->key)
        return E_INVALIDARG;
    const u32 a = endpoint == 0 ? range->start : range->end;
    const u32 b = other_endpoint == 0 ? target->start : target->end;
    *out = static_cast<int>(a) - static_cast<int>(b);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE range_expand(void* self, int unit) noexcept {
    auto* range = static_cast<TextRange*>(self);
    RangeText text{};
    if (!range_resolve(range, &text))
        return kElementNotAvailable;
    ui_access_text_expand(text.text, text_unit(unit), &range->start, &range->end);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE range_find_attribute(void* self, int, VARIANT, BOOL, void** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    RangeText text{};
    return range_resolve(static_cast<TextRange*>(self), &text) ? S_OK : kElementNotAvailable;
}

HRESULT STDMETHODCALLTYPE range_find_text(void* self, BSTR needle, BOOL backward, BOOL ignore_case,
                                          void** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (!needle)
        return E_INVALIDARG;
    auto* range = static_cast<TextRange*>(self);
    RangeText text{};
    if (!range_resolve(range, &text))
        return kElementNotAvailable;
    ScratchPages<char[kUiAccessValueMaximumBytes * 3]> scratch;
    if (!scratch.data)
        return E_OUTOFMEMORY;
    char* pattern = *scratch.data;
    const int bytes = wide_argument_utf8(needle, pattern, static_cast<int>(sizeof(*scratch.data)));
    if (bytes <= 0)
        return E_INVALIDARG;
    const u32 length = static_cast<u32>(bytes);
    if (range->end - range->start < length)
        return S_OK;
    const u32 last = range->end - length;
    for (u32 step = 0; step <= last - range->start; ++step) {
        const u32 at = backward ? last - step : range->start + step;
        bool match = true;
        for (u32 index = 0; index < length && match; ++index) {
            const char a = text.text.data[at + index], b = pattern[index];
            match = ignore_case ? ascii_lower(a) == ascii_lower(b) : a == b;
        }
        if (match) {
            *out = range_create(range->host, range->key, at, at + length);
            return *out ? S_OK : E_OUTOFMEMORY;
        }
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE range_attribute(void* self, int attribute, VARIANT* out) noexcept {
    if (!out)
        return E_POINTER;
    V_VT(out) = VT_EMPTY;
    auto* range = static_cast<TextRange*>(self);
    RangeText text{};
    if (!range_resolve(range, &text))
        return kElementNotAvailable;
    switch (attribute) {
    case attribute_is_read_only:
        variant_bool(out, (text.node->state & ui_access_read_only) != 0);
        return S_OK;
    case attribute_is_hidden:
        variant_bool(out, false);
        return S_OK;
    default:
        break;
    }
    // Everything else is plain text without formatting: "not supported".
    void* reserved = nullptr;
    const auto* runtime = range->host->runtime;
    if (runtime && runtime->reserved_not_supported && SUCCEEDED(runtime->reserved_not_supported(&reserved)) &&
        reserved) {
        V_VT(out) = VT_UNKNOWN;
        V_UNKNOWN(out) = static_cast<IUnknown*>(reserved);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE range_bounds(void* self, SAFEARRAY** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* range = static_cast<TextRange*>(self);
    RangeText text{};
    if (!range_resolve(range, &text))
        return kElementNotAvailable;
    auto* host = range->host;
    UiRect rectangles[16]{};
    const u32 count = host->callbacks.text_bounds
                          ? host->callbacks.text_bounds(host->callbacks.context, range->key, range->start, range->end,
                                                        rectangles, 16)
                          : 0;
    SAFEARRAY* array = host->runtime->create_vector(VT_R8, 0, (count < 16 ? count : 16) * 4);
    if (!array)
        return E_OUTOFMEMORY;
    LONG position = 0;
    for (u32 index = 0; index < count && index < 16; ++index) {
        const UiaBounds bounds = screen_bounds(host, rectangles[index]);
        double values[4]{bounds.left, bounds.top, bounds.width, bounds.height};
        for (double& value : values) {
            host->runtime->put_element(array, &position, &value);
            ++position;
        }
    }
    *out = array;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE range_enclosing(void* self, void** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* range = static_cast<TextRange*>(self);
    RangeText text{};
    if (!range_resolve(range, &text))
        return kElementNotAvailable;
    *out = element_create(range->host, range->key, false);
    return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE range_text(void* self, int maximum_length, BSTR* out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* range = static_cast<TextRange*>(self);
    RangeText text{};
    if (!range_resolve(range, &text))
        return kElementNotAvailable;
    const auto* runtime = range->host->runtime;
    const Utf8View part{text.text.data + range->start, range->end - range->start};
    ScratchPages<wchar_t[kUiAccessValueMaximumBytes + 1]> scratch;
    if (!scratch.data)
        return E_OUTOFMEMORY;
    wchar_t* wide = *scratch.data;
    int units = part.bytes ? MultiByteToWideChar(CP_UTF8, 0, part.data, static_cast<int>(part.bytes), wide,
                                                 static_cast<int>(kUiAccessValueMaximumBytes))
                           : 0;
    if (units < 0)
        units = 0;
    if (maximum_length >= 0 && units > maximum_length) {
        units = maximum_length;
        // Never split a surrogate pair.
        if (units > 0 && wide[units - 1] >= 0xD800 && wide[units - 1] <= 0xDBFF)
            --units;
    }
    *out = runtime->alloc_string_len(units ? wide : L"", static_cast<UINT>(units));
    return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE range_move(void* self, int unit, int count, int* out) noexcept {
    if (!out)
        return E_POINTER;
    *out = 0;
    auto* range = static_cast<TextRange*>(self);
    RangeText text{};
    if (!range_resolve(range, &text))
        return kElementNotAvailable;
    *out = ui_access_text_move(text.text, text_unit(unit), count, &range->start, &range->end);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE range_move_endpoint(void* self, int endpoint, int unit, int count, int* out) noexcept {
    if (!out)
        return E_POINTER;
    *out = 0;
    auto* range = static_cast<TextRange*>(self);
    RangeText text{};
    if (!range_resolve(range, &text))
        return kElementNotAvailable;
    u32* moving = endpoint == 0 ? &range->start : &range->end;
    *out = ui_access_text_move_endpoint(text.text, text_unit(unit), count, moving);
    if (range->start > range->end) {
        if (endpoint == 0)
            range->end = range->start;
        else
            range->start = range->end;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE range_move_endpoint_by_range(void* self, int endpoint, void* other,
                                                       int other_endpoint) noexcept {
    if (!is_range(other))
        return E_INVALIDARG;
    auto* range = static_cast<TextRange*>(self);
    auto* target = static_cast<TextRange*>(other);
    RangeText text{}, target_text{};
    if (!range_resolve(range, &text) || !range_resolve(target, &target_text))
        return kElementNotAvailable;
    if (range->key != target->key)
        return E_INVALIDARG;
    const u32 value = other_endpoint == 0 ? target->start : target->end;
    if (endpoint == 0) {
        range->start = value;
        if (range->end < value)
            range->end = value;
    } else {
        range->end = value;
        if (range->start > value)
            range->start = value;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE range_select(void* self) noexcept {
    auto* range = static_cast<TextRange*>(self);
    RangeText text{};
    if (!range_resolve(range, &text))
        return kElementNotAvailable;
    if (text.node->state & ui_access_disabled)
        return kElementNotEnabled;
    auto* host = range->host;
    if (!host->callbacks.act ||
        !host->callbacks.act(host->callbacks.context, range->key, UiAccessAction::select_text,
                             ui_access_text_selection_pack(range->start, range->end), {}))
        return kInvalidOperation;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE range_selection_change(void*) noexcept {
    return kInvalidOperation; // single selection only
}

HRESULT STDMETHODCALLTYPE range_scroll(void* self, BOOL) noexcept {
    RangeText text{};
    return range_resolve(static_cast<TextRange*>(self), &text) ? S_OK : kElementNotAvailable;
}

HRESULT STDMETHODCALLTYPE range_children(void* self, SAFEARRAY** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    auto* range = static_cast<TextRange*>(self);
    RangeText text{};
    if (!range_resolve(range, &text))
        return kElementNotAvailable;
    *out = range->host->runtime->create_vector(VT_UNKNOWN, 0, 0);
    return *out ? S_OK : E_OUTOFMEMORY;
}

#define UIA_RANGE(method) &Marshal<offsetof(TextRange, host), &method>::call
constexpr TextRangeVtbl kTextRangeVtbl{
    {&range_query_interface, &range_add_ref, &range_release},
    UIA_RANGE(range_clone),
    UIA_RANGE(range_compare),
    UIA_RANGE(range_compare_endpoints),
    UIA_RANGE(range_expand),
    UIA_RANGE(range_find_attribute),
    UIA_RANGE(range_find_text),
    UIA_RANGE(range_attribute),
    UIA_RANGE(range_bounds),
    UIA_RANGE(range_enclosing),
    UIA_RANGE(range_text),
    UIA_RANGE(range_move),
    UIA_RANGE(range_move_endpoint),
    UIA_RANGE(range_move_endpoint_by_range),
    UIA_RANGE(range_select),
    &range_selection_change,
    &range_selection_change,
    UIA_RANGE(range_scroll),
    UIA_RANGE(range_children),
};
#undef UIA_RANGE

bool is_range(const void* unknown) noexcept {
    return unknown && *static_cast<const TextRangeVtbl* const*>(unknown) == &kTextRangeVtbl;
}

void* range_create(UiaProviderHost* host, u64 key, u32 start, u32 end) noexcept {
    if (!host || !host->connected)
        return nullptr;
    auto* range = static_cast<TextRange*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TextRange)));
    if (!range)
        return nullptr;
    range->vtbl = &kTextRangeVtbl;
    range->references = 1;
    range->host = host;
    range->key = key;
    range->start = start;
    range->end = end < start ? start : end;
    InterlockedIncrement(&host->ranges);
    return range;
}

#define UIA_MARSHAL(offset, method) &Marshal<offsetof(Element, host) - (offset), &method>::call
constexpr SimpleVtbl kSimpleVtbl{unknown_vtbl<kSimple>(), &simple_provider_options,
                                 UIA_MARSHAL(kSimple, simple_pattern_provider),
                                 UIA_MARSHAL(kSimple, simple_property_value),
                                 UIA_MARSHAL(kSimple, simple_host_provider)};
constexpr FragmentVtbl kFragmentVtbl{unknown_vtbl<kFragment>(), UIA_MARSHAL(kFragment, fragment_navigate),
                                     UIA_MARSHAL(kFragment, fragment_runtime_id),
                                     UIA_MARSHAL(kFragment, fragment_bounds), &fragment_embedded_roots,
                                     UIA_MARSHAL(kFragment, fragment_set_focus),
                                     UIA_MARSHAL(kFragment, fragment_root)};
constexpr FragmentRootVtbl kFragmentRootVtbl{unknown_vtbl<kFragmentRoot>(),
                                             UIA_MARSHAL(kFragmentRoot, root_from_point),
                                             UIA_MARSHAL(kFragmentRoot, root_focus)};
constexpr InvokeVtbl kInvokeVtbl{unknown_vtbl<kInvoke>(), UIA_MARSHAL(kInvoke, invoke_invoke)};
constexpr ToggleVtbl kToggleVtbl{unknown_vtbl<kToggle>(), UIA_MARSHAL(kToggle, toggle_toggle),
                                 UIA_MARSHAL(kToggle, toggle_state_get)};
constexpr SelectionVtbl kSelectionVtbl{unknown_vtbl<kSelection>(), UIA_MARSHAL(kSelection, selection_get),
                                       &out_false, &out_false};
constexpr SelectionItemVtbl kSelectionItemVtbl{
    unknown_vtbl<kSelectionItem>(), UIA_MARSHAL(kSelectionItem, selection_item_select),
    UIA_MARSHAL(kSelectionItem, selection_item_add), UIA_MARSHAL(kSelectionItem, selection_item_remove),
    UIA_MARSHAL(kSelectionItem, selection_item_selected), UIA_MARSHAL(kSelectionItem, selection_item_container)};
constexpr ValueVtbl kValueVtbl{unknown_vtbl<kValue>(), UIA_MARSHAL(kValue, value_set),
                               UIA_MARSHAL(kValue, value_get), UIA_MARSHAL(kValue, read_only_get<kValue>)};
constexpr RangeValueVtbl kRangeValueVtbl{
    unknown_vtbl<kRangeValue>(), UIA_MARSHAL(kRangeValue, range_set), UIA_MARSHAL(kRangeValue, range_value_get),
    UIA_MARSHAL(kRangeValue, read_only_get<kRangeValue>), UIA_MARSHAL(kRangeValue, range_maximum),
    UIA_MARSHAL(kRangeValue, range_minimum), UIA_MARSHAL(kRangeValue, range_large_step),
    UIA_MARSHAL(kRangeValue, range_step)};
constexpr ScrollVtbl kScrollVtbl{unknown_vtbl<kScroll>(), UIA_MARSHAL(kScroll, scroll_scroll),
                                 &scroll_set_percent, &scroll_no_scroll,
                                 UIA_MARSHAL(kScroll, scroll_vertical_percent), &scroll_full_view,
                                 UIA_MARSHAL(kScroll, scroll_vertical_view), &out_false,
                                 UIA_MARSHAL(kScroll, scroll_vertically)};
constexpr ExpandCollapseVtbl kExpandCollapseVtbl{unknown_vtbl<kExpandCollapse>(),
                                                 UIA_MARSHAL(kExpandCollapse, expand_expand),
                                                 UIA_MARSHAL(kExpandCollapse, expand_collapse_action),
                                                 UIA_MARSHAL(kExpandCollapse, expand_state_get)};
constexpr ScrollItemVtbl kScrollItemVtbl{unknown_vtbl<kScrollItem>(), UIA_MARSHAL(kScrollItem, scroll_item_into_view)};
constexpr GridVtbl kGridVtbl{unknown_vtbl<kGrid>(), UIA_MARSHAL(kGrid, grid_item_at), UIA_MARSHAL(kGrid, grid_rows),
                             UIA_MARSHAL(kGrid, grid_columns)};
constexpr GridItemVtbl kGridItemVtbl{unknown_vtbl<kGridItem>(), UIA_MARSHAL(kGridItem, grid_item_row),
                                     UIA_MARSHAL(kGridItem, grid_item_column), &grid_item_span,
                                     &grid_item_span, UIA_MARSHAL(kGridItem, grid_item_container)};
constexpr TextVtbl kTextVtbl{unknown_vtbl<kText>(),
                             UIA_MARSHAL(kText, text_selection),
                             UIA_MARSHAL(kText, text_visible_ranges),
                             UIA_MARSHAL(kText, text_range_from_child),
                             UIA_MARSHAL(kText, text_range_from_point),
                             UIA_MARSHAL(kText, text_document_range),
                             &text_supported_selection};
#undef UIA_MARSHAL

void* element_create(UiaProviderHost* host, u64 key, bool root) noexcept {
    if (!host || !host->connected)
        return nullptr;
    if (root && host->root) {
        auto* cached = static_cast<Element*>(host->root);
        element_add_ref(cached);
        return cached;
    }
    auto* element = static_cast<Element*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Element)));
    if (!element)
        return nullptr;
    element->simple = &kSimpleVtbl;
    element->fragment = &kFragmentVtbl;
    element->fragment_root = &kFragmentRootVtbl;
    element->invoke = &kInvokeVtbl;
    element->toggle = &kToggleVtbl;
    element->selection = &kSelectionVtbl;
    element->selection_item = &kSelectionItemVtbl;
    element->value = &kValueVtbl;
    element->range_value = &kRangeValueVtbl;
    element->scroll = &kScrollVtbl;
    element->expand_collapse = &kExpandCollapseVtbl;
    element->scroll_item = &kScrollItemVtbl;
    element->grid = &kGridVtbl;
    element->grid_item = &kGridItemVtbl;
    element->text = &kTextVtbl;
    element->references = 1;
    element->host = host;
    element->key = key;
    element->root = root;
    InterlockedIncrement(&host->elements);
    if (root) {
        // The host keeps one reference to the root for WM_GETOBJECT answers.
        element_add_ref(element);
        host->root = element;
    }
    return element;
}

bool property_values(const UiaProviderHost* host, const Resolved& r, const UiAccessPropertyChange& change, int* id,
                     VARIANT* old_value, VARIANT* new_value) noexcept {
    V_VT(old_value) = VT_EMPTY;
    V_VT(new_value) = VT_EMPTY;
    const auto& node = *r.node;
    switch (change.property) {
    case UiAccessProperty::toggle:
        *id = property_toggle_state;
        variant_int(old_value, toggle_state(change.old_state));
        variant_int(new_value, toggle_state(node.state));
        return true;
    case UiAccessProperty::selected:
        *id = property_selection_item_is_selected;
        variant_bool(old_value, (change.old_state & ui_access_selected) != 0);
        variant_bool(new_value, (node.state & ui_access_selected) != 0);
        return true;
    case UiAccessProperty::expanded: {
        *id = property_expand_collapse_state;
        UiAccessNode old_node = node;
        old_node.state = change.old_state;
        variant_int(old_value, expand_state(old_node));
        variant_int(new_value, expand_state(node));
        return true;
    }
    case UiAccessProperty::range:
        *id = property_range_value_value;
        variant_double(old_value, static_cast<double>(change.old_range));
        variant_double(new_value, static_cast<double>(node.range.value));
        return true;
    case UiAccessProperty::value:
        *id = property_value_value;
        if (node.state & ui_access_password)
            return false;
        return variant_text(host->runtime, new_value, ui_access_value(*r.tree, r.index));
    case UiAccessProperty::name:
        *id = property_name;
        return variant_text(host->runtime, new_value, ui_access_name(*r.tree, r.index));
    default:
        return false;
    }
}

void variant_free(const UiaRuntime* runtime, VARIANT* value) noexcept {
    if (V_VT(value) == VT_BSTR && V_BSTR(value))
        runtime->free_string(V_BSTR(value));
    V_VT(value) = VT_EMPTY;
}

bool raise_event(UiaProviderHost* host, u64 key, int event) noexcept {
    if (!host || !host->runtime || !host->connected)
        return false;
    void* element = element_create(host, key, key == 0);
    if (!element)
        return false;
    const HRESULT hr = host->runtime->raise_automation_event(element, event);
    element_release(static_cast<Element*>(element));
    return SUCCEEDED(hr);
}

} // namespace

bool uia_host_initialize(UiaProviderHost* host, HWND window, const UiaHostCallbacks& callbacks) noexcept {
    if (!host || !callbacks.tree)
        return false;
    reset_value<UiaProviderHost>(host);
    host->window = window;
    host->callbacks = callbacks;
    host->thread = GetCurrentThreadId();
    host->call_message = window ? RegisterWindowMessageW(L"MaiCord.UiaProviderCall") : 0;
    host->connected = true;
    return true;
}

bool uia_host_load(UiaProviderHost* host) noexcept {
    if (!host || !host->connected)
        return false;
    if (host->runtime)
        return true;
    if (host->load_failed)
        return false;
    host->runtime = load_runtime();
    host->load_failed = host->runtime == nullptr;
    return host->runtime != nullptr;
}

bool uia_host_window_message(UiaProviderHost* host, UINT message, WPARAM wparam, LPARAM lparam,
                             LRESULT* result) noexcept {
    if (host && result && host->call_message && message == host->call_message) {
        // Only a call queued by run_on_window_thread runs; the message carries no data.
        if (wparam == kCallSignature && GetCurrentThreadId() == host->thread) {
            auto* call = static_cast<WindowCall*>(
                InterlockedExchangePointer(const_cast<void**>(&host->pending_call), nullptr));
            if (call) {
                call->result = call->run(call->context);
                InterlockedExchange(&call->done, 1);
            }
        }
        *result = 0;
        return true;
    }
    if (message != WM_GETOBJECT || !host || !result || !host->connected)
        return false;
    if (static_cast<long>(lparam) != kUiaRootObjectId)
        return false;
    if (!uia_host_load(host))
        return false;
    void* root = element_create(host, 0, true);
    if (!root)
        return false;
    *result = host->runtime->return_raw_element_provider(host->window, wparam, lparam, root);
    element_release(static_cast<Element*>(root));
    ++host->requests;
    return true;
}

bool uia_host_listening(const UiaProviderHost& host) noexcept {
    return host.runtime && host.connected && host.runtime->clients_are_listening() != FALSE;
}

void* uia_host_element(UiaProviderHost* host, u64 key) noexcept {
    if (!host || !host->connected)
        return nullptr;
    return element_create(host, key, key == 0);
}

bool uia_host_raise_focus(UiaProviderHost* host, u64 key) noexcept {
    return raise_event(host, key, event_focus_changed);
}

bool uia_host_raise_live_region(UiaProviderHost* host, u64 key) noexcept {
    return raise_event(host, key, event_live_region_changed);
}

bool uia_host_raise_structure(UiaProviderHost* host, u64 key) noexcept {
    if (!host || !host->runtime || !host->connected)
        return false;
    auto* element = static_cast<Element*>(element_create(host, key, key == 0));
    if (!element)
        return false;
    int runtime_id[3]{kUiaAppendRuntimeId, static_cast<int>(key & 0x7FFF'FFFFu),
                      static_cast<int>((key >> 31) & 0x7FFF'FFFFu)};
    const HRESULT hr = host->runtime->raise_structure_changed_event(
        element, kStructureChildrenInvalidated, element->root ? nullptr : runtime_id, element->root ? 0 : 3);
    element_release(element);
    return SUCCEEDED(hr);
}

bool uia_host_raise_property(UiaProviderHost* host, u64 key, const UiAccessPropertyChange& change) noexcept {
    if (!host || !host->runtime || !host->connected)
        return false;
    auto* element = static_cast<Element*>(element_create(host, key, key == 0));
    if (!element)
        return false;
    Resolved r{};
    bool raised = false;
    int id = 0;
    VARIANT old_value, new_value;
    const bool resolved = resolve_element(element, &r);
    const bool text = resolved && supports(r, pattern_text);
    if (change.property == UiAccessProperty::text_selection) {
        raised = text && SUCCEEDED(host->runtime->raise_automation_event(element, event_text_selection_changed));
    } else if (resolved && property_values(host, r, change, &id, &old_value, &new_value)) {
        raised = SUCCEEDED(host->runtime->raise_property_changed_event(element, id, old_value, new_value));
        variant_free(host->runtime, &old_value);
        variant_free(host->runtime, &new_value);
        if (text && change.property == UiAccessProperty::value)
            host->runtime->raise_automation_event(element, event_text_changed);
        // Lists whose focus stays elsewhere (combo boxes, pickers) announce
        // the newly selected item through ElementSelected.
        if (change.property == UiAccessProperty::selected && (r.node->state & ui_access_selected) &&
            !(r.node->state & ui_access_focused))
            host->runtime->raise_automation_event(element, event_element_selected);
    }
    element_release(element);
    return raised;
}

bool uia_host_raise_notification(UiaProviderHost* host, u64 key, UiaNotificationKind kind,
                                 UiaNotificationProcessing processing, Utf8View text, Utf8View activity) noexcept {
    if (!host || !host->runtime || !host->connected || !host->runtime->raise_notification_event || !text.bytes)
        return false;
    auto* element = static_cast<Element*>(element_create(host, key, key == 0));
    if (!element)
        return false;
    BSTR display = make_bstr(host->runtime, text);
    BSTR identity = make_bstr(host->runtime, activity.bytes ? activity : utf8_literal("MaiCord"));
    bool raised = false;
    if (display && identity)
        raised = SUCCEEDED(host->runtime->raise_notification_event(element, static_cast<int>(kind),
                                                                   static_cast<int>(processing), display, identity));
    if (display)
        host->runtime->free_string(display);
    if (identity)
        host->runtime->free_string(identity);
    element_release(element);
    return raised;
}

void uia_host_shutdown(UiaProviderHost* host) noexcept {
    if (!host || !host->connected)
        return;
    if (host->runtime) {
        if (host->window)
            host->runtime->return_raw_element_provider(host->window, 0, 0, nullptr);
        if (host->root && host->runtime->disconnect_provider)
            host->runtime->disconnect_provider(host->root);
    }
    host->connected = false;
    if (host->root) {
        auto* root = static_cast<Element*>(host->root);
        host->root = nullptr;
        element_release(root);
    }
}

} // namespace maicord
