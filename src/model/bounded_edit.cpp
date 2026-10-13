#include "maicord/model/bounded_edit.hpp"

namespace maicord {
namespace {
bool allowed(u32 scalar, bool multiline) noexcept {
    if (scalar == '\n') return multiline;
    if (scalar < 32 || (scalar >= 127 && scalar <= 159)) return false;
    return multiline || (scalar != 0x2028 && scalar != 0x2029);
}
bool valid_document(const BoundedEdit& edit) noexcept {
    if (!edit.composer || !edit.identity || !edit.binding_identity || !edit.maximum_codepoints ||
        edit.document_identity != edit.composer->document_identity ||
        !text_input_ready(message_composer_input_target(edit.composer))) return false;
    const auto text = message_composer_text(*edit.composer);
    u32 count = 0;
    for (u32 offset = 0; offset < text.bytes;) {
        const auto decoded = utf8_decode_one(text.data + offset, text.bytes - offset);
        // An existing document can contain characters this field does not admit as
        // new input; they stay selectable, copyable and removable.
        if (!decoded.valid || !decoded.codepoint ||
            ++count > edit.maximum_codepoints) return false;
        offset += decoded.width;
    }
    return true;
}
TextInputTarget delegate(const TextInputTarget& target) noexcept {
    const auto* edit = static_cast<const BoundedEdit*>(target.context);
    return edit ? TextInputTarget{edit->composer, message_composer_input_target(edit->composer).operations,
                                 edit->document_identity} : TextInputTarget{};
}
bool ready(const TextInputTarget& target) noexcept {
    const auto* edit = static_cast<const BoundedEdit*>(target.context);
    if (!edit || target.identity != edit->binding_identity || !valid_document(*edit)) return false;
    // Host readiness may reenter and bind another document. Carry the original
    // scope into the callback, then fence that complete binding again.
    const BoundedEdit captured = *edit;
    if (captured.ready && !captured.ready(captured.context, captured.identity)) return false;
    return target.identity == edit->binding_identity && edit->identity == captured.identity &&
        edit->composer == captured.composer &&
        edit->document_identity == captured.document_identity &&
        edit->maximum_codepoints == captured.maximum_codepoints && edit->multiline == captured.multiline &&
        edit->ready == captured.ready && edit->context == captured.context && valid_document(*edit);
}
u32 revision(const TextInputTarget& target) noexcept {
    return ready(target) ? text_input_revision(delegate(target)) : 0;
}
TextInputResult length(const TextInputTarget& target, u32* count) noexcept {
    return ready(target) ? text_input_length(delegate(target), count) : TextInputResult::invalid_input;
}
TextInputResult selection(const TextInputTarget& target, TextInputSelection* value) noexcept {
    return ready(target) ? text_input_selection(delegate(target), value) : TextInputResult::invalid_input;
}
TextInputResult select(const TextInputTarget& target, TextInputSelection value) noexcept {
    return ready(target) ? text_input_select(delegate(target), value) : TextInputResult::invalid_input;
}
TextInputResult copy(const TextInputTarget& target, u32 first, u32 last, Slice<u16> output,
                     u32* count, u32* next) noexcept {
    return ready(target) ? text_input_copy(delegate(target), first, last, output, count, next)
                         : TextInputResult::invalid_input;
}
TextInputResult replace(const TextInputTarget& target, u32 first, u32 last, ConstSlice<u16> input,
                        TextInputChange* change) noexcept {
    if (!change || (!input.data && input.count) || first > last || !ready(target))
        return TextInputResult::invalid_input;
    const auto& edit = *static_cast<const BoundedEdit*>(target.context);
    const auto text = message_composer_text(*edit.composer);
    u32 first_byte = 0, last_byte = 0;
    if (!utf8_byte_offset_for_utf16_position(text, first, &first_byte) ||
        !utf8_byte_offset_for_utf16_position(text, last, &last_byte)) return TextInputResult::invalid_utf16;
    const auto removed = utf8_validate({text.data + first_byte, last_byte - first_byte});
    const u32 retained = message_composer_codepoints(*edit.composer) - removed.codepoints;
    u32 inserted = 0;
    for (u32 i = 0; i < input.count; ++i) {
        u32 scalar = input[i];
        if (scalar >= 0xD800 && scalar <= 0xDBFF) {
            if (i + 1 == input.count || input[i + 1] < 0xDC00 || input[i + 1] > 0xDFFF)
                return TextInputResult::invalid_utf16;
            scalar = 0x10000 + ((scalar - 0xD800) << 10) + (input[++i] - 0xDC00);
        } else if (scalar >= 0xDC00 && scalar <= 0xDFFF) return TextInputResult::invalid_utf16;
        if (!allowed(scalar, edit.multiline)) return TextInputResult::invalid_input;
        if (++inserted > edit.maximum_codepoints - retained) return TextInputResult::capacity;
    }
    return text_input_replace(delegate(target), first, last, input, change);
}
void cancel(const TextInputTarget& target) noexcept {
    if (ready(target)) text_input_cancel(delegate(target));
}
bool begin(const TextInputTarget& target) noexcept {
    if (!ready(target)) return false;
    auto* model = static_cast<BoundedEdit*>(target.context)->composer;
    constexpr u32 maximum_history = 16 * 1024;
    const u32 budget = minimum(maximum_history, maximum(2048u, 4 * model->capacity));
    if (model->history && message_composer_history_costs(*model).payload_bytes > maximum_history)
        return false;
    return (model->history || message_composer_enable_history(model, budget)) &&
        text_input_begin_group(delegate(target));
}
void end(const TextInputTarget& target) noexcept {
    if (ready(target)) text_input_end_group(delegate(target));
}
constexpr TextInputOperations operations{ready, revision, length, selection, select, copy, replace,
                                         cancel, begin, end};
} // namespace
bool bounded_edit_bind(BoundedEdit* edit, MessageComposer* model, u32 maximum_codepoints,
                       bool multiline, u64 identity, BoundedEditReady ready_callback,
                       void* context) noexcept {
    if (!edit || !model || edit->binding_identity == ~u64{0}) return false;
    const BoundedEdit candidate{model, identity, model->document_identity, edit->binding_identity + 1,
                                maximum_codepoints, multiline,
                                ready_callback, context};
    if (!valid_document(candidate)) return false;
    *edit = candidate;
    return true;
}
TextInputTarget bounded_edit_input_target(BoundedEdit* edit) noexcept {
    return {edit, &operations, edit ? edit->binding_identity : 0};
}
} // namespace maicord
