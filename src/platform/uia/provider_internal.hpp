#pragma once

#include "maicord/platform/uia/provider.hpp"
#include "maicord/ui/accessibility/accessibility.hpp"

#include "maicord/sys/ole.hpp"

#include <stddef.h>

namespace maicord {
namespace provider_detail {

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
    control_slider = 50015,
    control_status_bar = 50017,
    control_tab = 50018,
    control_tab_item = 50019,
    control_text = 50020,
    control_tree = 50023,
    control_custom = 50025,
    control_group = 50026,
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

inline constexpr GUID kIidUnknown{0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
inline constexpr GUID kIidSimple{0xd6dd68d1, 0x86fd, 0x4332, {0x86, 0x66, 0x9a, 0xbe, 0xde, 0xa2, 0xd2, 0x4c}};
inline constexpr GUID kIidFragment{0xf7063da8, 0x8359, 0x439c, {0x92, 0x97, 0xbb, 0xc5, 0x29, 0x9a, 0x7d, 0x87}};
inline constexpr GUID kIidFragmentRoot{0x620ce2a5, 0xab8f, 0x40a9, {0x86, 0xcb, 0xde, 0x3c, 0x75, 0x59, 0x9b, 0x58}};
inline constexpr GUID kIidInvoke{0x54fcb24b, 0xe18e, 0x47a2, {0xb4, 0xd3, 0xec, 0xcb, 0xe7, 0x75, 0x99, 0xa2}};
inline constexpr GUID kIidToggle{0x56d00bd0, 0xc4f4, 0x433c, {0xa8, 0x36, 0x1a, 0x52, 0xa5, 0x7e, 0x08, 0x92}};
inline constexpr GUID kIidSelection{0xfb8b03af, 0x3bdf, 0x48d4, {0xbd, 0x36, 0x1a, 0x65, 0x79, 0x3b, 0xe1, 0x68}};
inline constexpr GUID kIidSelectionItem{0x2acad808, 0xb2d4, 0x452d, {0xa4, 0x07, 0x91, 0xff, 0x1a, 0xd1, 0x67, 0xb2}};
inline constexpr GUID kIidValue{0xc7935180, 0x6fb3, 0x4201, {0xb1, 0x74, 0x7d, 0xf7, 0x3a, 0xdb, 0xf6, 0x4a}};
inline constexpr GUID kIidRangeValue{0x36dc7aef, 0x33e6, 0x4691, {0xaf, 0xe1, 0x2b, 0xe7, 0x27, 0x4b, 0x3d, 0x33}};
inline constexpr GUID kIidScroll{0xb38b8077, 0x1fc3, 0x42a5, {0x8c, 0xae, 0xd4, 0x0c, 0x22, 0x15, 0x05, 0x5a}};
inline constexpr GUID kIidExpandCollapse{0xd847d3a5, 0xcab0, 0x4a98, {0x8c, 0x32, 0xec, 0xb4, 0x5c, 0x59, 0xad, 0x24}};
inline constexpr GUID kIidGrid{0xb17d6187, 0x0907, 0x464b, {0xa1, 0x68, 0x0e, 0xf1, 0x7a, 0x15, 0x72, 0xb1}};
inline constexpr GUID kIidGridItem{0xd02541f1, 0xfb81, 0x4d64, {0xae, 0x32, 0xf5, 0x20, 0xf8, 0xa6, 0xdb, 0xd1}};
inline constexpr GUID kIidScrollItem{0x2360c714, 0x4bf1, 0x4b26, {0xba, 0x65, 0x9b, 0x21, 0x31, 0x61, 0x27, 0xeb}};
inline constexpr GUID kIidText{0x3589c92c, 0x63f3, 0x4367, {0x99, 0xbb, 0xad, 0xa6, 0x53, 0xb7, 0x7c, 0xf2}};
inline constexpr GUID kIidTextRange{0x5347ad7b, 0xc355, 0x46f8, {0xaf, 0xf5, 0x90, 0x90, 0x33, 0x58, 0x2f, 0x63}};

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

} // namespace provider_detail
using namespace provider_detail;

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

namespace provider_detail {

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

__declspec(noinline) HRESULT run_on_window_thread(UiaProviderHost* host, WindowCallFunction run,
                                                  const void* context) noexcept;

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

struct Resolved {
    const UiAccessTree* tree;
    const UiAccessNode* node;
    u32 index;
};

ULONG element_release(Element* element) noexcept;
bool resolve_element(const Element* element, Resolved* out) noexcept;
BSTR make_bstr(const UiaRuntime* runtime, Utf8View text) noexcept;
void variant_bool(VARIANT* out, bool value) noexcept;
int toggle_state(u16 state) noexcept;
int expand_state(const UiAccessNode& node) noexcept;
UiaBounds screen_bounds(const UiaProviderHost* host, const UiRect& rectangle) noexcept;
HRESULT request(Element* element, UiAccessAction action, double number, const Utf8View& text) noexcept;
__declspec(noinline) HRESULT request(Element* element, UiAccessAction action) noexcept;
void* element_for_index(Element* from, const Resolved& r, u32 index) noexcept;

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
template <usize Offset> HRESULT STDMETHODCALLTYPE read_only_get(void* self, BOOL* out) noexcept {
    return read_node<int>(element_from<Offset>(self), out, [](const UiAccessNode& node) noexcept -> int {
        return (node.state & ui_access_read_only) ? TRUE : FALSE;
    });
}

// Pattern entry points (provider_patterns.cpp).
__declspec(noinline) int wide_argument_utf8(const wchar_t* text, char* output, int capacity) noexcept;
HRESULT STDMETHODCALLTYPE invoke_invoke(void* self) noexcept;
HRESULT STDMETHODCALLTYPE toggle_toggle(void* self) noexcept;
HRESULT STDMETHODCALLTYPE toggle_state_get(void* self, int* out) noexcept;
HRESULT STDMETHODCALLTYPE selection_get(void* self, SAFEARRAY** out) noexcept;
HRESULT STDMETHODCALLTYPE out_false(void*, BOOL* out) noexcept;
HRESULT STDMETHODCALLTYPE selection_item_select(void* self) noexcept;
HRESULT STDMETHODCALLTYPE selection_item_add(void* self) noexcept;
HRESULT STDMETHODCALLTYPE selection_item_remove(void* self) noexcept;
HRESULT STDMETHODCALLTYPE selection_item_selected(void* self, BOOL* out) noexcept;
HRESULT STDMETHODCALLTYPE selection_item_container(void* self, void** out) noexcept;
HRESULT STDMETHODCALLTYPE value_set(void* self, const wchar_t* text) noexcept;
HRESULT STDMETHODCALLTYPE value_get(void* self, BSTR* out) noexcept;
HRESULT STDMETHODCALLTYPE range_set(void* self, double value) noexcept;
HRESULT STDMETHODCALLTYPE range_value_get(void* self, double* out) noexcept;
HRESULT STDMETHODCALLTYPE range_minimum(void* self, double* out) noexcept;
HRESULT STDMETHODCALLTYPE range_maximum(void* self, double* out) noexcept;
HRESULT STDMETHODCALLTYPE range_step(void* self, double* out) noexcept;
HRESULT STDMETHODCALLTYPE range_large_step(void* self, double* out) noexcept;
HRESULT STDMETHODCALLTYPE scroll_scroll(void* self, int horizontal, int vertical) noexcept;
HRESULT STDMETHODCALLTYPE scroll_set_percent(void*, double horizontal, double vertical) noexcept;
HRESULT STDMETHODCALLTYPE scroll_no_scroll(void*, double* out) noexcept;
HRESULT STDMETHODCALLTYPE scroll_full_view(void*, double* out) noexcept;
HRESULT STDMETHODCALLTYPE scroll_vertical_percent(void* self, double* out) noexcept;
HRESULT STDMETHODCALLTYPE scroll_vertical_view(void* self, double* out) noexcept;
HRESULT STDMETHODCALLTYPE scroll_vertically(void* self, BOOL* out) noexcept;
HRESULT STDMETHODCALLTYPE expand_expand(void* self) noexcept;
HRESULT STDMETHODCALLTYPE expand_collapse_action(void* self) noexcept;
HRESULT STDMETHODCALLTYPE expand_state_get(void* self, int* out) noexcept;
HRESULT STDMETHODCALLTYPE scroll_item_into_view(void* self) noexcept;
HRESULT STDMETHODCALLTYPE grid_item_at(void* self, int row, int column, void** out) noexcept;
HRESULT STDMETHODCALLTYPE grid_rows(void* self, int* out) noexcept;
HRESULT STDMETHODCALLTYPE grid_columns(void* self, int* out) noexcept;
HRESULT STDMETHODCALLTYPE grid_item_row(void* self, int* out) noexcept;
HRESULT STDMETHODCALLTYPE grid_item_column(void* self, int* out) noexcept;
HRESULT STDMETHODCALLTYPE grid_item_span(void*, int* out) noexcept;
HRESULT STDMETHODCALLTYPE grid_item_container(void* self, void** out) noexcept;

// Text pattern entry points (provider_text.cpp).
HRESULT STDMETHODCALLTYPE text_selection(void* self, SAFEARRAY** out) noexcept;
HRESULT STDMETHODCALLTYPE text_visible_ranges(void* self, SAFEARRAY** out) noexcept;
HRESULT STDMETHODCALLTYPE text_range_from_child(void* self, void* child, void** out) noexcept;
HRESULT STDMETHODCALLTYPE text_range_from_point(void* self, UiaPointValue, void** out) noexcept;
HRESULT STDMETHODCALLTYPE text_document_range(void* self, void** out) noexcept;
HRESULT STDMETHODCALLTYPE text_supported_selection(void*, int* out) noexcept;

} // namespace provider_detail
} // namespace maicord
