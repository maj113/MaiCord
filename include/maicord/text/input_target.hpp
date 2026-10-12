#pragma once
#include "maicord/core/utf.hpp"
namespace maicord {
enum class TextInputResult : u8 { unchanged, changed, pending_surrogate, capacity, invalid_input, invalid_utf8, invalid_utf16 };
struct TextInputSelection { u32 anchor, caret; };
struct TextInputChange { u32 start, old_end, new_end; };
struct TextInputTarget;
struct TextInputOperations {
    bool (*ready)(const TextInputTarget&) noexcept;
    u32 (*revision)(const TextInputTarget&) noexcept;
    TextInputResult (*length)(const TextInputTarget&, u32*) noexcept;
    TextInputResult (*selection)(const TextInputTarget&, TextInputSelection*) noexcept;
    TextInputResult (*select)(const TextInputTarget&, TextInputSelection) noexcept;
    TextInputResult (*copy)(const TextInputTarget&, u32, u32, Slice<u16>, u32*, u32*) noexcept;
    TextInputResult (*replace)(const TextInputTarget&, u32, u32, ConstSlice<u16>, TextInputChange*) noexcept;
    void (*cancel)(const TextInputTarget&) noexcept;
    bool (*begin_group)(const TextInputTarget&) noexcept;
    void (*end_group)(const TextInputTarget&) noexcept;
};
// Borrowed model and static operations table. Both outlive binding. Identity
// captures the document; operations must reject stale identities. Unbind before
// destroying/replacing a model. The native store never owns its text buffer.
struct TextInputTarget {
    void* context;
    const TextInputOperations* operations;
    u64 identity;
    explicit operator bool() const noexcept { return context && operations; }
    bool operator==(const TextInputTarget&) const = default;
};
inline bool text_input_ready(const TextInputTarget& t) noexcept {
    if (!t) return false;
    const auto& o = *t.operations;
    return o.ready && o.revision && o.length && o.selection && o.select && o.copy && o.replace &&
        o.cancel && o.begin_group && o.end_group && o.ready(t);
}
inline u32 text_input_revision(const TextInputTarget& t) noexcept { return text_input_ready(t) ? t.operations->revision(t) : 0; }
inline TextInputResult text_input_length(const TextInputTarget& t, u32* n) noexcept { return text_input_ready(t) ? t.operations->length(t, n) : TextInputResult::invalid_input; }
inline TextInputResult text_input_selection(const TextInputTarget& t, TextInputSelection* s) noexcept { return text_input_ready(t) ? t.operations->selection(t, s) : TextInputResult::invalid_input; }
inline TextInputResult text_input_select(const TextInputTarget& t, TextInputSelection s) noexcept { return text_input_ready(t) ? t.operations->select(t, s) : TextInputResult::invalid_input; }
inline TextInputResult text_input_copy(const TextInputTarget& t, u32 a, u32 b, Slice<u16> out, u32* n, u32* next) noexcept { return text_input_ready(t) ? t.operations->copy(t, a, b, out, n, next) : TextInputResult::invalid_input; }
inline TextInputResult text_input_replace(const TextInputTarget& t, u32 a, u32 b, ConstSlice<u16> in, TextInputChange* c) noexcept { return text_input_ready(t) ? t.operations->replace(t, a, b, in, c) : TextInputResult::invalid_input; }
inline void text_input_cancel(const TextInputTarget& t) noexcept { if (text_input_ready(t)) t.operations->cancel(t); }
inline bool text_input_begin_group(const TextInputTarget& t) noexcept { return text_input_ready(t) && t.operations->begin_group(t); }
inline void text_input_end_group(const TextInputTarget& t) noexcept { if (text_input_ready(t)) t.operations->end_group(t); }
}
