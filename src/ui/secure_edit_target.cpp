#include "maicord/ui/secure_edit.hpp"
namespace maicord {
namespace {
SecureEdit* edit(const TextInputTarget& t) noexcept { return static_cast<SecureEdit*>(t.context); }
bool ready(const TextInputTarget& t) noexcept { const auto* e = edit(t); return e && e->storage.data && e->storage.locked && !e->password; }
u32 revision(const TextInputTarget& t) noexcept { return edit(t)->revision; }
TextInputResult length(const TextInputTarget& t, u32* n) noexcept { return secure_edit_utf16_length(*edit(t), n); }
TextInputResult selection(const TextInputTarget& t, TextInputSelection* s) noexcept { return secure_edit_utf16_selection(*edit(t), s); }
TextInputResult select(const TextInputTarget& t, TextInputSelection s) noexcept { return secure_edit_set_utf16_selection(edit(t), s); }
TextInputResult copy(const TextInputTarget& t, u32 a, u32 b, Slice<u16> o, u32* n, u32* next) noexcept { return secure_edit_copy_utf16(*edit(t), a, b, o, n, next); }
TextInputResult replace(const TextInputTarget& t, u32 a, u32 b, ConstSlice<u16> v, TextInputChange* c) noexcept { return secure_edit_replace_utf16(edit(t), a, b, v, c); }
void cancel(const TextInputTarget& t) noexcept { secure_edit_cancel_composition(edit(t)); }
bool begin(const TextInputTarget& t) noexcept { return secure_edit_begin_history_group(edit(t)); }
void end(const TextInputTarget& t) noexcept { secure_edit_end_history_group(edit(t)); }
const TextInputOperations operations{ready, revision, length, selection, select, copy, replace, cancel, begin, end};
}
TextInputTarget secure_edit_input_target(SecureEdit* e) noexcept { return {e, &operations, 0}; }
}
