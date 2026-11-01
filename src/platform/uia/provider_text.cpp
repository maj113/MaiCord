#include "provider_internal.hpp"

namespace maicord {
namespace provider_detail {

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
    if (*iid != kIidUnknown && *iid != kIidTextRange)
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
    if (SUCCEEDED(runtime->reserved_not_supported(&reserved)) && reserved) {
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
    // MultiByteToWideChar returns 0, never a negative count, on failure or empty input.
    int units = MultiByteToWideChar(CP_UTF8, 0, part.data, static_cast<int>(part.bytes), wide,
                                    static_cast<int>(kUiAccessValueMaximumBytes));
    if (maximum_length >= 0 && units > maximum_length) {
        units = maximum_length;
        // Never split a surrogate pair.
        if (units > 0 && wide[units - 1] >= 0xD800 && wide[units - 1] <= 0xDBFF)
            --units;
    }
    *out = runtime->alloc_string_len(wide, static_cast<UINT>(units));
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
} // namespace provider_detail
} // namespace maicord
