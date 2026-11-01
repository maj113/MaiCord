#include "provider_internal.hpp"

namespace maicord {
namespace provider_detail {

// Patterns
HRESULT STDMETHODCALLTYPE invoke_invoke(void* self) noexcept {
    return request(element_from<kInvoke>(self), UiAccessAction::invoke);
}

HRESULT STDMETHODCALLTYPE toggle_toggle(void* self) noexcept {
    return request(element_from<kToggle>(self), UiAccessAction::toggle);
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

} // namespace provider_detail
} // namespace maicord
