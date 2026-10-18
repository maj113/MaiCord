// Test/tool-only DirectWrite reference editor; see dwrite_text_system.hpp.
// Never linked into MaiCord.exe or MaiCordAccount.dll.
#include "dwrite_text_system.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/geometry.hpp"

#include <Windows.h>

namespace maicord {
bool text_edit_layout_measure(TextEditLayoutOwner* owner, const TextEditLayoutInput& input,
                              const DWriteTextSystem& text_system) noexcept {
    return text_edit_layout_measure(owner, input, native_view(text_system));
}

// DirectWrite layout behind owner geometry.
namespace {
constexpr u8 kEditKnownOptions = text_edit_layout_wrap | text_edit_layout_center_vertically |
                                 text_edit_layout_scroll_to_caret;
TextEditMetric edit_metric(const DWRITE_HIT_TEST_METRICS& m) noexcept {
    return {static_cast<u16>(m.textPosition), static_cast<u16>(m.length), m.left, m.top, m.width, m.height,
            m.bidiLevel};
}
u64 edit_style_signature(ConstSlice<TextStyleSpan> styles) noexcept {
    return hash64_counted_bytes(kHash64Seed, styles.data, styles.count * sizeof(TextStyleSpan));
}
HRESULT edit_range(const DWriteEditLayoutOwner& owner, u32 first, u32 length, float x, float y,
                   TextEditMetric* output, u32 capacity, u32* count) noexcept {
    if (!owner.layout || capacity > kTextEditSelectionCapacity)
        return E_FAIL;
    DWRITE_HIT_TEST_METRICS metrics[kTextEditSelectionCapacity]{};
    const HRESULT result = owner.layout->HitTestTextRange(first, length, x, y, metrics, capacity, count);
    for (u32 i = 0; i < minimum(capacity, *count); ++i)
        output[i] = edit_metric(metrics[i]);
    return result;
}
bool edit_large_selection(DWriteEditLayoutOwner* owner, const TextEditLayoutInput& input, float origin_y, u32 first,
                          u32 last) noexcept {
    struct Range { u32 first, last; } pending[16]{};
    u32 pending_count = 0;
    TextEditMetric batch[kTextEditSelectionCapacity]{};
    owner->selection_count = 0;
    for (;;) {
        u32 actual = 0;
        const HRESULT result =
            edit_range(*owner, first, last - first, owner->origin_x, origin_y, batch, kTextEditSelectionCapacity, &actual);
        if (result == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && actual > kTextEditSelectionCapacity) {
            u32 middle = first + (last - first) / 2;
            if (middle < last && owner->utf16[middle] >= 0xDC00 && owner->utf16[middle] <= 0xDFFF)
                --middle;
            if (middle <= first || middle >= last || pending_count == 16)
                return false;
            pending[pending_count++] = {middle, last};
            last = middle;
            continue;
        }
        if (FAILED(result) || actual > kTextEditSelectionCapacity)
            return false;
        for (u32 i = 0; i < actual; ++i) {
            auto metric = batch[i];
            const float right = minimum(metric.left + metric.width, input.origin_x + input.width);
            const float bottom = minimum(metric.top + metric.height, input.viewport_top + input.height);
            metric.left = maximum(metric.left, input.origin_x);
            metric.top = maximum(metric.top, input.viewport_top);
            metric.width = right - metric.left;
            metric.height = bottom - metric.top;
            if (metric.width <= 0 || metric.height <= 0)
                continue;
            for (u32 j = 0; j < owner->selection_count;) {
                const auto prior = owner->selections[j];
                if (metric.top == prior.top && metric.height == prior.height &&
                    metric.left <= prior.left + prior.width && prior.left <= metric.left + metric.width) {
                    const float merged_right = maximum(metric.left + metric.width, prior.left + prior.width);
                    metric.left = minimum(metric.left, prior.left);
                    metric.width = merged_right - metric.left;
                    const u32 end = maximum<u32>(metric.source_first + metric.source_count,
                                                 prior.source_first + prior.source_count);
                    metric.source_first = minimum(metric.source_first, prior.source_first);
                    metric.source_count = static_cast<u16>(end - metric.source_first);
                    owner->selections[j] = owner->selections[--owner->selection_count];
                    j = 0;
                } else
                    ++j;
            }
            if (owner->selection_count == owner->selection_capacity)
                return false;
            owner->selections[owner->selection_count++] = metric;
        }
        if (!pending_count)
            return true;
        const auto range = pending[--pending_count];
        first = range.first;
        last = range.last;
    }
}
void edit_commit(DWriteEditLayoutOwner* owner, const TextEditLayoutInput& input, const DWriteTextSystem& system,
                 float origin_y) noexcept {
    owner->format = reinterpret_cast<u64>(system.format);
    owner->text_signature = input.text_signature;
    owner->style_signature = edit_style_signature(input.styles);
    owner->text_bytes = input.text.bytes;
    owner->anchor = input.anchor_utf16;
    owner->caret_offset = input.caret_utf16;
    owner->viewport_left = input.origin_x;
    owner->origin_y = origin_y;
    owner->viewport_top = input.viewport_top;
    owner->width = input.width;
    owner->height = input.height;
    owner->maximum_layout_height = input.maximum_layout_height;
    owner->em_size = system.em_size;
    owner->options = input.options;
    owner->active = true;
    saturating_increment(owner->builds);
}
bool edit_reject(DWriteEditLayoutOwner* owner) noexcept {
    saturating_increment(owner->rejected);
    return false;
}
bool edit_same_text(const DWriteEditLayoutOwner& owner, const TextEditLayoutInput& input,
                    const DWriteTextSystem& system) noexcept {
    return owner.active && owner.format == reinterpret_cast<u64>(system.format) &&
           owner.text_signature == input.text_signature &&
           owner.style_signature == edit_style_signature(input.styles) && owner.text_bytes == input.text.bytes &&
           owner.width == input.width && owner.maximum_layout_height == input.maximum_layout_height &&
           owner.em_size == system.em_size;
}
bool edit_input_current(const DWriteEditLayoutOwner& owner, const TextEditLayoutInput& input,
                        const DWriteTextSystem& system) noexcept {
    return edit_same_text(owner, input, system) && owner.anchor == input.anchor_utf16 &&
           owner.caret_offset == input.caret_utf16 && owner.viewport_left == input.origin_x &&
           owner.viewport_top == input.viewport_top && owner.height == input.height &&
           owner.options == input.options;
}
bool dwrite_edit_measure(DWriteEditLayoutOwner* owner, const TextEditLayoutInput& input,
                         const DWriteTextSystem& system) noexcept {
    const u64 format = reinterpret_cast<u64>(system.format);
    if (!system.factory || !format || !owner->selections || owner->selection_capacity == 0 ||
        owner->selection_capacity > kTextEditSelectionCapacity || (!input.text.data && input.text.bytes != 0) ||
        input.text.bytes > kTextEditTextCapacity || input.text_signature == 0 || input.width <= 0.0f ||
        input.height <= 0.0f || input.maximum_layout_height <= 0.0f || (input.options & ~kEditKnownOptions) != 0 ||
        input.styles.count > kTextStyleSpanMaximum || (!input.styles.data && input.styles.count)) {
        text_edit_layout_invalidate(owner);
        return edit_reject(owner);
    }
    const bool same_text = edit_same_text(*owner, input, system);
    if (edit_input_current(*owner, input, system)) {
        saturating_increment(owner->hits);
        return true;
    }
    const float retained_scroll = owner->scroll_y, retained_scroll_x = owner->scroll_x;
    const bool manual_scroll = owner->manual_scroll && owner->text_signature == input.text_signature &&
                               owner->anchor == input.anchor_utf16 && owner->caret_offset == input.caret_utf16;
    const bool retain = same_text && owner->layout && ((owner->options ^ input.options) & text_edit_layout_wrap) == 0;
    IDWriteTextLayout* layout = retain ? owner->layout : nullptr;
    u32 units = retain ? owner->utf16_units : 0;
    if (retain)
        owner->layout = nullptr;
    text_edit_layout_invalidate(owner);
    if (input.text.bytes == 0) {
        if (input.anchor_utf16 != 0 || input.caret_utf16 != 0)
            return edit_reject(owner);
        const float caret_height = minimum(system.em_size * 1.25f, input.height);
        const float origin_y = (input.options & text_edit_layout_center_vertically) != 0
                                   ? input.viewport_top + (input.height - caret_height) * 0.5f
                                   : input.viewport_top;
        owner->origin_x = input.origin_x;
        owner->scroll_x = 0;
        owner->caret.left = input.origin_x;
        owner->caret.top = origin_y;
        owner->caret.width = 1.0f;
        owner->caret.height = caret_height;
        owner->layout_height = caret_height;
        edit_commit(owner, input, system, origin_y);
        return true;
    }
    HRESULT result = S_OK;
    if (!retain) {
        if (!owner->utf16)
            owner->utf16 = static_cast<wchar_t*>(HeapAlloc(process_heap(), 0, kTextEditTextCapacity * sizeof(wchar_t)));
        const int converted = owner->utf16 ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.text.data,
                                                                 static_cast<int>(input.text.bytes), owner->utf16,
                                                                 static_cast<int>(kTextEditTextCapacity))
                                           : 0;
        if (converted <= 0)
            return edit_reject(owner);
        units = static_cast<u32>(converted);
        result = system.factory->CreateTextLayout(owner->utf16, units, system.format, input.width,
                                                  input.maximum_layout_height, &layout);
        if (SUCCEEDED(result))
            result = layout->SetWordWrapping((input.options & text_edit_layout_wrap) != 0 ? DWRITE_WORD_WRAPPING_WRAP
                                                                                       : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (SUCCEEDED(result))
            result = layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        if (SUCCEEDED(result))
            result = text_apply_layout_styles(system, layout, input.text, input.styles);
        if (SUCCEEDED(result))
            result = regional_flag_layout_attach(layout, input.text, input.styles, &owner->flag_cells);
    }
    if (SUCCEEDED(result) && (input.anchor_utf16 > units || input.caret_utf16 > units))
        result = E_INVALIDARG;
    float caret_x = 0, caret_y = 0;
    DWRITE_HIT_TEST_METRICS caret_metrics{};
    DWRITE_TEXT_METRICS text_metrics{};
    if (SUCCEEDED(result))
        result = layout->HitTestTextPosition(input.caret_utf16, FALSE, &caret_x, &caret_y, &caret_metrics);
    if (SUCCEEDED(result))
        result = layout->GetMetrics(&text_metrics);
    if (FAILED(result)) {
        com_release(layout);
        return edit_reject(owner);
    }
    const float caret_height = caret_metrics.height > 0.0f ? caret_metrics.height : 20.0f;
    const float content_height = maximum(maximum(text_metrics.height, caret_y + caret_height), 1.0f);
    float scroll_y = 0.0f;
    if ((input.options & text_edit_layout_scroll_to_caret) != 0) {
        scroll_y = minimum(maximum(retained_scroll, 0.0f), maximum(0.0f, content_height - input.height));
        if (!manual_scroll && caret_y < scroll_y)
            scroll_y = caret_y;
        else if (!manual_scroll && caret_y + caret_height > scroll_y + input.height)
            scroll_y = caret_y + caret_height - input.height;
        scroll_y = minimum(maximum(scroll_y, 0.0f), maximum(0.0f, content_height - input.height));
    }
    const float origin_y = (input.options & text_edit_layout_center_vertically) != 0 && content_height <= input.height
                               ? input.viewport_top + (input.height - content_height) * 0.5f
                               : input.viewport_top - scroll_y;
    float scroll_x = 0;
    if ((input.options & text_edit_layout_scroll_to_caret) && !(input.options & text_edit_layout_wrap)) {
        const float visible_width = maximum(1.0f, input.width - 1.0f);
        scroll_x = minimum(maximum(0.0f, retained_scroll_x),
                           maximum(0.0f, text_metrics.widthIncludingTrailingWhitespace - visible_width));
        if (caret_x < scroll_x)
            scroll_x = caret_x;
        else if (caret_x > scroll_x + visible_width)
            scroll_x = caret_x - visible_width;
    }
    owner->layout = layout;
    owner->utf16_units = units;
    owner->scroll_x = scroll_x;
    owner->origin_x = input.origin_x - scroll_x;
    owner->scroll_y = scroll_y;
    owner->manual_scroll = manual_scroll;
    owner->layout_height = content_height;
    owner->caret = edit_metric(caret_metrics);
    owner->caret.left = owner->origin_x + caret_x;
    owner->caret.top = origin_y + caret_y;
    owner->caret.width = 1.0f;
    owner->caret.height = caret_height;
    const u32 selection_first = minimum(input.anchor_utf16, input.caret_utf16);
    const u32 selection_last = maximum(input.anchor_utf16, input.caret_utf16);
    if (selection_first != selection_last) {
        u32 actual = 0;
        result = edit_range(*owner, selection_first, selection_last - selection_first, owner->origin_x, origin_y,
                            owner->selections, owner->selection_capacity, &actual);
        const bool oversized = result == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && actual > owner->selection_capacity;
        if (oversized && edit_large_selection(owner, input, origin_y, selection_first, selection_last)) {
        } else if (FAILED(result) || actual > owner->selection_capacity) {
            text_edit_layout_invalidate(owner);
            return edit_reject(owner);
        } else
            owner->selection_count = actual;
    }
    edit_commit(owner, input, system, origin_y);
    return true;
}
} // namespace

bool text_edit_layout_measure(DWriteEditLayoutOwner* owner, const TextEditLayoutInput& input,
                              const DWriteTextSystem& system) noexcept {
    if (!owner)
        return false;
    if (system.native_text) {
        // Native admission is deterministic: an unchanged input that the
        // native editor declined keeps its retained DirectWrite fallback.
        if (owner->native_declined && owner->layout && !owner->geometry &&
            edit_input_current(*owner, input, system)) {
            saturating_increment(owner->hits);
            return true;
        }
        com_release(owner->layout);
        owner->native_declined = false;
        if (text_edit_layout_measure(static_cast<TextEditLayoutOwner*>(owner), input, system) || !system.factory ||
            !system.format)
            return owner->active;
        owner->native_declined = true;
    } else
        owner->native_declined = false;
    return dwrite_edit_measure(owner, input, system);
}
bool text_edit_layout_point_geometry(const DWriteEditLayoutOwner& owner, float x, float y, bool round_nearest,
                                     u32* position, bool* inside) noexcept {
    if (owner.geometry || !owner.text_bytes)
        return text_edit_layout_point_geometry(static_cast<const TextEditLayoutOwner&>(owner), x, y, round_nearest,
                                               position, inside);
    if (!position || !inside || !owner.active || !owner.layout)
        return false;
    BOOL trailing = FALSE, contains = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(owner.layout->HitTestPoint(x - owner.origin_x, y - owner.origin_y, &trailing, &contains, &metrics)))
        return false;
    *inside = contains != FALSE;
    *position = minimum(owner.utf16_units,
                        metrics.textPosition + ((round_nearest || !contains) && trailing ? metrics.length : 0u));
    return true;
}
bool text_edit_layout_hit_test(const DWriteEditLayoutOwner& owner, float x, float y, u32* position) noexcept {
    bool inside = false;
    return text_edit_layout_point_geometry(owner, x, y, true, position, &inside);
}
bool text_edit_layout_move_vertical(const DWriteEditLayoutOwner& owner, bool down, u32* position) noexcept {
    if (owner.geometry || !owner.text_bytes)
        return text_edit_layout_move_vertical(static_cast<const TextEditLayoutOwner&>(owner), down, position);
    if (!position || !owner.active || !owner.layout)
        return false;
    const float line_height = maximum(1.0f, owner.caret.height);
    const float x = owner.caret.left - owner.origin_x;
    const float y = owner.caret.top - owner.origin_y + (down ? line_height * 1.5f : -line_height * 0.5f);
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(owner.layout->HitTestPoint(x, y, &trailing, &inside, &metrics)))
        return false;
    *position = minimum(owner.utf16_units, metrics.textPosition + (trailing ? metrics.length : 0u));
    return true;
}
bool text_edit_layout_range_geometry(const DWriteEditLayoutOwner& owner, u32 first, u32 last,
                                     Slice<TextEditMetric> output, u32* count) noexcept {
    if (owner.geometry)
        return text_edit_layout_range_geometry(static_cast<const TextEditLayoutOwner&>(owner), first, last, output, count);
    if (!count || !owner.active || first > last || last > owner.utf16_units || !output.data || !output.count ||
        output.count > kTextEditSelectionCapacity)
        return false;
    if (first == last) {
        *count = 0;
        return true;
    }
    if (!owner.layout)
        return false;
    auto projection = owner;
    projection.selections = output.data;
    projection.selection_capacity = output.count;
    TextEditLayoutInput input{};
    input.origin_x = owner.viewport_left;
    input.viewport_top = owner.viewport_top;
    input.width = owner.width;
    input.height = owner.height;
    if (!edit_large_selection(&projection, input, owner.origin_y, first, last))
        return false;
    *count = projection.selection_count;
    return true;
}
bool text_edit_layout_position_geometry(const DWriteEditLayoutOwner& owner, u32 position, TextEditMetric* metric) noexcept {
    if (owner.geometry || !owner.text_bytes)
        return text_edit_layout_position_geometry(static_cast<const TextEditLayoutOwner&>(owner), position, metric);
    if (!metric || !owner.active || position > owner.utf16_units || !owner.layout)
        return false;
    float x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS m{};
    if (FAILED(owner.layout->HitTestTextPosition(position, FALSE, &x, &y, &m)))
        return false;
    *metric = edit_metric(m);
    metric->left = owner.origin_x + x;
    metric->top = owner.origin_y + y;
    metric->width = 1;
    return true;
}
bool text_edit_layout_raw_range_geometry(const DWriteEditLayoutOwner& owner, u32 first, u32 last,
                                         Slice<TextEditMetric> output, u32* count) noexcept {
    if (owner.geometry)
        return text_edit_layout_raw_range_geometry(static_cast<const TextEditLayoutOwner&>(owner), first, last, output,
                                                   count);
    if (!count || !owner.active || first > last || last > owner.utf16_units || !output.data || !output.count ||
        output.count > kTextEditSelectionCapacity)
        return false;
    return SUCCEEDED(edit_range(owner, first, last - first, owner.origin_x, owner.origin_y, output.data, output.count, count));
}
u32 text_edit_layout_line_count(const DWriteEditLayoutOwner& owner) noexcept {
    if (owner.geometry || !owner.text_bytes || !owner.active)
        return text_edit_layout_line_count(static_cast<const TextEditLayoutOwner&>(owner));
    DWRITE_TEXT_METRICS metrics{};
    return owner.layout && SUCCEEDED(owner.layout->GetMetrics(&metrics)) ? metrics.lineCount : 0;
}
bool text_edit_layout_scroll(DWriteEditLayoutOwner* owner, float delta) noexcept {
    if (!owner)
        return false;
    if (owner->geometry)
        return text_edit_layout_scroll(static_cast<TextEditLayoutOwner*>(owner), delta);
    if (!owner->active || !ui_coordinate_valid(delta))
        return false;
    const float next = minimum(maximum(0.0f, owner->scroll_y + delta), maximum(0.0f, owner->layout_height - owner->height));
    if (next == owner->scroll_y)
        return true;
    auto projection = *owner;
    const float offset = owner->scroll_y - next;
    projection.origin_y += offset;
    TextEditMetric metrics[kTextEditSelectionCapacity]{};
    u32 count = 0;
    if (!text_edit_layout_range_geometry(projection, minimum(owner->anchor, owner->caret_offset),
                                         maximum(owner->anchor, owner->caret_offset), {metrics, owner->selection_capacity},
                                         &count))
        return false;
    owner->scroll_y = next;
    owner->origin_y += offset;
    owner->caret.top += offset;
    owner->manual_scroll = true;
    for (u32 i = 0; i < count; ++i)
        owner->selections[i] = metrics[i];
    owner->selection_count = count;
    return true;
}
void text_edit_layout_invalidate(DWriteEditLayoutOwner* owner) noexcept {
    if (!owner)
        return;
    com_release(owner->layout);
    text_edit_layout_invalidate(static_cast<TextEditLayoutOwner*>(owner));
}
void text_edit_layout_release(DWriteEditLayoutOwner* owner) noexcept {
    if (!owner)
        return;
    com_release(owner->layout);
    if (owner->utf16)
        HeapFree(process_heap(), 0, owner->utf16);
    regional_flag_layout_free(owner->flag_cells);
    owner->utf16 = nullptr;
    owner->flag_cells = nullptr;
    text_edit_layout_release(static_cast<TextEditLayoutOwner*>(owner));
}

} // namespace maicord
