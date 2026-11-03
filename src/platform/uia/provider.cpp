#include "provider_internal.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord {
namespace provider_detail {

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
              resolve(runtime.core, "SafeArrayDestroy", &runtime.destroy_array) &&
              resolve(runtime.automation, "UiaDisconnectProvider", &runtime.disconnect_provider) &&
              resolve(runtime.automation, "UiaGetReservedNotSupportedValue", &runtime.reserved_not_supported);
    // Optional: Windows 10 1709+.
    if (ok)
        resolve(runtime.automation, "UiaRaiseNotificationEvent", &runtime.raise_notification_event);
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
    return call.result;
}

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
    control_list,        control_list_item,  control_tree,      control_tab,          control_tab_item,
    control_menu,        control_menu_item,  control_text,      control_header,       control_edit,
    control_slider,      control_image,      control_hyperlink, control_status_bar};
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

// Pattern support reads the snapshot, so it is answered on the window thread;
// an IID that names no pattern is refused without a cross-thread call.
const Pattern* pattern_for(const GUID& iid) noexcept {
    for (const auto& pattern : kPatterns)
        if (iid == *pattern.iid)
            return &pattern;
    return nullptr;
}

// The identity interfaces depend only on fields fixed at creation, so the
// calling thread answers them.
__declspec(noinline) HRESULT element_query(Element* element, const GUID* iid, void** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (!iid)
        return E_INVALIDARG;
    void* result = nullptr;
    if (*iid == kIidUnknown || *iid == kIidSimple) {
        result = &element->simple;
    } else if (*iid == kIidFragment) {
        result = &element->fragment;
    } else if (*iid == kIidFragmentRoot) {
        result = element->root ? &element->fragment_root : nullptr;
    } else if (const Pattern* pattern = pattern_for(*iid)) {
        on_window_thread(element->host, [&]() noexcept -> HRESULT {
            Resolved r{};
            if (resolve_element(element, &r) && supports(r, pattern->id))
                result = reinterpret_cast<char*>(element) + pattern->offset;
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
    if (V_VT(value) == VT_BSTR)
        runtime->free_string(V_BSTR(value)); // SysFreeString accepts null
    V_VT(value) = VT_EMPTY;
}

// The element a raised event is about; null while the runtime is unloaded or the host disconnected.
Element* event_element(UiaProviderHost* host, u64 key) noexcept {
    if (!host || !host->runtime || !host->connected)
        return nullptr;
    return static_cast<Element*>(element_create(host, key, key == 0));
}

bool raise_event(UiaProviderHost* host, u64 key, int event) noexcept {
    Element* element = event_element(host, key);
    if (!element)
        return false;
    const HRESULT hr = host->runtime->raise_automation_event(element, event);
    element_release(element);
    return SUCCEEDED(hr);
}

} // namespace provider_detail
using namespace provider_detail;

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
    return true;
}

bool uia_host_listening(const UiaProviderHost& host) noexcept {
    return host.runtime && host.connected && host.runtime->clients_are_listening() != FALSE;
}

bool uia_host_raise_focus(UiaProviderHost* host, u64 key) noexcept {
    return raise_event(host, key, event_focus_changed);
}

bool uia_host_raise_live_region(UiaProviderHost* host, u64 key) noexcept {
    return raise_event(host, key, event_live_region_changed);
}

bool uia_host_raise_structure(UiaProviderHost* host, u64 key) noexcept {
    auto* element = event_element(host, key);
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
    auto* element = event_element(host, key);
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
    if (!host || !host->runtime || !host->runtime->raise_notification_event || !text.bytes)
        return false;
    auto* element = event_element(host, key);
    if (!element)
        return false;
    BSTR display = make_bstr(host->runtime, text);
    BSTR identity = make_bstr(host->runtime, activity.bytes ? activity : utf8_literal("MaiCord"));
    bool raised = false;
    if (display && identity)
        raised = SUCCEEDED(host->runtime->raise_notification_event(element, static_cast<int>(kind),
                                                                   static_cast<int>(processing), display, identity));
    host->runtime->free_string(display); // SysFreeString accepts null
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
        if (host->root)
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
