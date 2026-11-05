// The text store where there is no TSF: fields edit through the window's key and character events, and an input
// method (Wayland text-input-v3, window_wayland_text_input_*) talks through ime_* events. One store at a time holds the
// window's text input: the one that is bound to a field, attached and focused. It tells the input method what is around
// the caret and where the caret is, and applies the events that come back to the bound target the way the TSF store
// applies a composition: the preedit text sits in the model between `start` and `end` (so layout, caret and selection
// see it), a commit replaces it, and the whole composition is one history group.
#include "maicord/ui/text/secure_text_store.hpp"
#include "maicord/core/heap.hpp"
#include "maicord/os/text.hpp"
#include "maicord/platform/window/wayland.hpp"

namespace maicord {
namespace {

// UTF-16 units of text sent around the caret on each side; at most three bytes each, under the protocol's 4000.
constexpr u32 kSurroundingUnits = 500;

struct Implementation {
    SecureTextStoreConfig config;
    TextInputTarget target;
    SecureTextInputScope scope;
    bool attached;
    bool bound;
    bool focused;
    bool enabled;  // the input method has been told about this field
    bool composing;
    bool group;  // the composition owns an open history group
    u32 start, end;  // the composition in the target, UTF-16 units
    SecureTextComposition composition;
    // What the input method was last told.
    u32 sent_revision;
    TextInputSelection sent_selection;
    SecureTextRect sent_rectangle;
};

Implementation* g_active;  // the store holding the window's text input

Implementation* implementation(SecureTextStore* store) noexcept {
    return store ? static_cast<Implementation*>(store->implementation) : nullptr;
}

const Implementation* implementation(const SecureTextStore& store) noexcept {
    return static_cast<const Implementation*>(store.implementation);
}

u32 next_revision(u32 revision) noexcept {
    return revision == ~0u ? 1 : revision + 1;
}

// The composition as the widgets read it; the method gives no per-span attributes, so the preedit is one `input` span
// with the segment the method highlights (when it names one) as the converted target.
void publish(Implementation* value, u32 caret_begin, u32 caret_end) noexcept {
    SecureTextComposition next{};
    next.revision = next_revision(value->composition.revision);
    next.active = value->composing;
    next.complete = true;
    if (value->composing) {
        next.start = value->start;
        next.end = value->end;
        const u32 length = value->end - value->start;
        const u32 begin = caret_begin < caret_end ? caret_begin : caret_end;
        const u32 last = caret_begin < caret_end ? caret_end : caret_begin;
        auto add = [&](u32 from, u32 to, TextCompositionAttribute attribute) noexcept {
            if (from >= to || next.span_count >= kTextCompositionSpanCapacity)
                return;
            TextCompositionSpan& span = next.spans[next.span_count++];
            span.start = value->start + from;
            span.end = value->start + to;
            span.line_style = attribute == TextCompositionAttribute::input ? TextCompositionLineStyle::dot
                                                                           : TextCompositionLineStyle::solid;
            span.attribute = attribute;
        };
        if (begin < last && last <= length) {
            add(0, begin, TextCompositionAttribute::input);
            add(begin, last, TextCompositionAttribute::target_converted);
            add(last, length, TextCompositionAttribute::input);
        } else {
            add(0, length, TextCompositionAttribute::input);
        }
    }
    value->composition = next;
}

void notify(Implementation* value) noexcept {
    if (value->config.changed)
        value->config.changed(value->config.context);
}

// Removes the preedit text from the model and ends the composition (and its history group).
void clear_composition(Implementation* value) noexcept {
    if (!value->composing)
        return;
    TextInputChange change{};
    text_input_replace(value->target, value->start, value->end, {}, &change);
    text_input_select(value->target, {value->start, value->start});
    value->composing = false;
    if (value->group) {
        value->group = false;
        text_input_end_group(value->target);
    }
    publish(value, 0, 0);
}

// Where the caret is in the client area, for the input method's candidate window.
void caret_rectangle(const Implementation* value, u32 caret, SecureTextRect* rectangle) noexcept {
    bool clipped = false;
    *rectangle = {};
    if (value->config.range_geometry &&
        value->config.range_geometry(value->config.context, caret, caret, rectangle, &clipped))
        return;
    SecureTextStoreGeometry geometry{};
    if (value->config.geometry && value->config.geometry(value->config.context, &geometry))
        *rectangle = geometry.text;
}

void disable(Implementation* value) noexcept {
    if (g_active == value)
        g_active = nullptr;
    if (!value->enabled)
        return;
    value->enabled = false;
    clear_composition(value);
    window_wayland_text_input_disable(value->config.window);
}

// Tells the input method about the field when it is the one that holds the window's text input, and when what it
// was told changed (the text outside a composition, the selection, the caret rectangle).
void refresh(Implementation* value) noexcept {
    // A password edit refuses text operations (its target is never ready): no input method for it, as with TSF.
    const bool active = value->bound && value->attached && value->focused && text_input_ready(value->target);
    if (!active || !window_wayland_text_input_available(value->config.window)) {
        disable(value);
        return;
    }
    if (g_active && g_active != value)
        disable(g_active);
    g_active = value;

    TextInputSelection selection{};
    u32 length = 0;
    if (text_input_length(value->target, &length) == TextInputResult::invalid_input ||
        text_input_selection(value->target, &selection) == TextInputResult::invalid_input)
        return;
    const u32 revision = text_input_revision(value->target);
    SecureTextRect rectangle{};
    caret_rectangle(value, selection.caret, &rectangle);
    if (value->enabled && revision == value->sent_revision && selection.anchor == value->sent_selection.anchor &&
        selection.caret == value->sent_selection.caret && rectangle.left == value->sent_rectangle.left &&
        rectangle.top == value->sent_rectangle.top && rectangle.right == value->sent_rectangle.right &&
        rectangle.bottom == value->sent_rectangle.bottom)
        return;

    TextInputRequest request{};
    request.purpose = value->scope == SecureTextInputScope::digits ? TextInputPurpose::digits : TextInputPurpose::normal;
    request.x = rectangle.left;
    request.y = rectangle.top;
    request.width = rectangle.right > rectangle.left ? rectangle.right - rectangle.left : 1;
    request.height = rectangle.bottom > rectangle.top ? rectangle.bottom - rectangle.top : 1;

    // The text around the caret without the composition.
    u16 units[2 * kSurroundingUnits + 8];
    char bytes[3 * (2 * kSurroundingUnits + 8)];
    {
        // The surrounding text is the text outside the composition: [low, before) then [after, high).
        const u32 before = value->composing ? value->start : selection.caret;
        const u32 after = value->composing ? value->end : selection.caret;
        const u32 low = before > kSurroundingUnits ? before - kSurroundingUnits : 0;
        const u32 high = length - after > kSurroundingUnits ? after + kSurroundingUnits : length;
        u32 first = 0, second = 0, next = 0;
        const bool copied_first = text_input_copy(value->target, low, before, {units, kSurroundingUnits + 4}, &first,
                                                  &next) != TextInputResult::invalid_input;
        const bool copied_second = text_input_copy(value->target, after, high, {units + first, kSurroundingUnits + 4},
                                                   &second, &next) != TextInputResult::invalid_input;
        if (copied_first && copied_second) {
            // The caret and the selection anchor, as byte offsets into the converted text.
            const u32 total = first + second;
            auto point = [&](u32 position) noexcept {
                return value->composing || position <= low ? (value->composing ? first : 0)
                       : position - low > total ? total : position - low;
            };
            const u32 caret_units = point(selection.caret);
            const u32 anchor_units = point(selection.anchor);
            const wchar_t* text = reinterpret_cast<const wchar_t*>(units);
            const u32 low_units = caret_units < anchor_units ? caret_units : anchor_units;
            const u32 high_units = caret_units < anchor_units ? anchor_units : caret_units;
            const u32 head = utf16_to_utf8(text, low_units, bytes, sizeof(bytes), false);
            const u32 middle = utf16_to_utf8(text + low_units, high_units - low_units, bytes + head,
                                             sizeof(bytes) - head, false);
            const u32 tail = utf16_to_utf8(text + high_units, total - high_units, bytes + head + middle,
                                           sizeof(bytes) - head - middle, false);
            request.surrounding = {bytes, head + middle + tail};
            request.cursor_byte = caret_units <= anchor_units ? head : head + middle;
            request.anchor_byte = caret_units <= anchor_units ? head + middle : head;
        }
    }
    if (window_wayland_text_input_enable(value->config.window, request)) {
        value->enabled = true;
        value->sent_revision = revision;
        value->sent_selection = selection;
        value->sent_rectangle = rectangle;
    }
}

// The input method's preedit replaces the composition (starting one at the selection when there is none).
bool apply_preedit(Implementation* value, const WindowEvent& event) noexcept {
    const u32 count = event.ime.count;
    if (count == 0) {
        clear_composition(value);
        return true;
    }
    u32 start = value->start, end = value->end;
    if (!value->composing) {
        TextInputSelection selection{};
        if (text_input_selection(value->target, &selection) == TextInputResult::invalid_input)
            return true;
        start = selection.anchor < selection.caret ? selection.anchor : selection.caret;
        end = selection.anchor < selection.caret ? selection.caret : selection.anchor;
        value->group = text_input_begin_group(value->target);
    }
    TextInputChange change{};
    const TextInputResult result = text_input_replace(value->target, start, end, {event.ime.text, count}, &change);
    if (result != TextInputResult::changed && result != TextInputResult::unchanged) {
        // Refused (the field is full, a stale target): nothing of the preedit is shown.
        if (!value->composing && value->group) {
            value->group = false;
            text_input_end_group(value->target);
        }
        return true;
    }
    value->composing = true;
    value->start = start;
    value->end = result == TextInputResult::changed ? change.new_end : start + count;
    const u32 length = value->end - value->start;
    auto position = [&](i32 offset) noexcept { return offset < 0 || static_cast<u32>(offset) > length ? length : static_cast<u32>(offset); };
    const u32 caret = value->start + position(event.ime.cursor_end);
    text_input_select(value->target, {caret, caret});
    publish(value, event.ime.cursor_begin < 0 ? length : position(event.ime.cursor_begin), position(event.ime.cursor_end));
    return true;
}

// The commit replaces the composition, or the selection when there is none.
bool apply_commit(Implementation* value, const WindowEvent& event) noexcept {
    u32 start = value->start, end = value->end;
    if (!value->composing) {
        TextInputSelection selection{};
        if (text_input_selection(value->target, &selection) == TextInputResult::invalid_input)
            return true;
        start = selection.anchor < selection.caret ? selection.anchor : selection.caret;
        end = selection.anchor < selection.caret ? selection.caret : selection.anchor;
    }
    TextInputChange change{};
    const TextInputResult result = text_input_replace(value->target, start, end, {event.ime.text, event.ime.count}, &change);
    const u32 caret = result == TextInputResult::changed ? change.new_end : start + event.ime.count;
    value->composing = false;
    if (value->group) {
        value->group = false;
        text_input_end_group(value->target);
    }
    if (result == TextInputResult::changed || result == TextInputResult::unchanged)
        text_input_select(value->target, {caret, caret});
    publish(value, 0, 0);
    return true;
}

// Text around the caret the method asked to remove, in units before and after it.
bool apply_delete(Implementation* value, const WindowEvent& event) noexcept {
    clear_composition(value);
    TextInputSelection selection{};
    u32 length = 0;
    if (text_input_selection(value->target, &selection) == TextInputResult::invalid_input ||
        text_input_length(value->target, &length) == TextInputResult::invalid_input)
        return true;
    const u32 caret = selection.caret;
    const u32 from = caret > event.ime.delete_before ? caret - event.ime.delete_before : 0;
    const u32 to = length - caret > event.ime.delete_after ? caret + event.ime.delete_after : length;
    TextInputChange change{};
    if (text_input_replace(value->target, from, to, {}, &change) == TextInputResult::changed)
        text_input_select(value->target, {from, from});
    return true;
}

} // namespace

bool secure_text_store_initialize(SecureTextStore* store, SecureTextStoreConfig config) noexcept {
    if (!store || store->implementation || !config.window)
        return false;
    auto* value = static_cast<Implementation*>(heap_allocate(sizeof(Implementation), true));
    if (!value)
        return false;
    value->config = config;
    value->focused = true;
    store->implementation = value;
    return true;
}

void secure_text_store_release(SecureTextStore* store) noexcept {
    if (auto* value = implementation(store)) {
        store->implementation = nullptr;
        disable(value);
        heap_free(value);
    }
}

bool secure_text_store_process_deferred() noexcept {
    return true;
}

bool secure_text_store_attach(SecureTextStore* store) noexcept {
    auto* value = implementation(store);
    if (!value || !value->bound)
        return false;
    value->attached = true;
    refresh(value);
    return true;
}

void secure_text_store_detach(SecureTextStore* store) noexcept {
    if (auto* value = implementation(store)) {
        value->attached = false;
        refresh(value);
    }
}

bool secure_text_store_attached(const SecureTextStore& store) noexcept {
    const auto* value = implementation(store);
    return value && value->attached;
}

// Keys reach this store as plain events; only a text service that sees them first (TSF) takes any.
bool secure_text_store_filter_key(SecureTextStore*, const WindowEvent&) noexcept {
    return false;
}

bool secure_text_store_ime_event(const WindowEvent& event) noexcept {
    Implementation* value = g_active;
    if (!value || !text_input_ready(value->target))
        return false;
    bool taken = false;
    switch (event.type) {
    case EventType::ime_preedit:
        taken = apply_preedit(value, event);
        break;
    case EventType::ime_commit:
        taken = apply_commit(value, event);
        break;
    case EventType::ime_delete_surrounding:
        taken = apply_delete(value, event);
        break;
    default:
        return false;
    }
    notify(value);
    refresh(value);
    return taken;
}

bool secure_text_store_bind(SecureTextStore* store, SecureEdit* edit, SecureTextInputScope scope) noexcept {
    auto* value = implementation(store);
    if (!value || !edit)
        return false;
    disable(value);
    value->target = secure_edit_input_target(edit);
    value->scope = scope;
    value->bound = true;
    refresh(value);
    return true;
}

bool secure_text_store_bind_target(SecureTextStore* store, TextInputTarget target, SecureTextInputScope scope) noexcept {
    auto* value = implementation(store);
    if (!value || !target)
        return false;
    disable(value);
    value->target = target;
    value->scope = scope;
    value->bound = true;
    refresh(value);
    return true;
}

void secure_text_store_unbind(SecureTextStore* store) noexcept {
    if (auto* value = implementation(store)) {
        disable(value);
        value->bound = false;
        value->target = {};
    }
}

void secure_text_store_set_window_focus(SecureTextStore* store, bool focused) noexcept {
    if (auto* value = implementation(store)) {
        value->focused = focused;
        refresh(value);
    }
}

void secure_text_store_synchronize(SecureTextStore* store) noexcept {
    if (auto* value = implementation(store))
        refresh(value);
}

void secure_text_store_notify_layout(SecureTextStore* store) noexcept {
    if (auto* value = implementation(store))
        refresh(value);
}

bool secure_text_store_composition(const SecureTextStore& store, SecureTextComposition* composition) noexcept {
    const auto* value = implementation(store);
    if (!value)
        return false;
    *composition = value->composition;
    return true;
}

bool secure_text_store_composing(const SecureTextStore& store) noexcept {
    const auto* value = implementation(store);
    return value && value->composing;
}

} // namespace maicord
