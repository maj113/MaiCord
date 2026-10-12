#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/platform/secure_buffer.hpp"
#include "maicord/text/edit_history.hpp"
#include "maicord/text/input_target.hpp"

namespace maicord {

using SecureEditResult = TextInputResult;

enum class SecureEditMove : u8 {
    previous,
    next,
    home,
    end,
    line_home,
    line_end,
};

// Zero codepoints means the byte capacity alone bounds the field. Multiline
// fields preserve CR, LF and CRLF exactly, including their UTF-16 ACP offsets.
// Password fields must remain single-line. Policy is fixed at initialization.
struct SecureEditPolicy {
    u32 maximum_codepoints;
    bool multiline;
};

using SecureEditHistory = TextEditHistory;
struct SecureEdit {
    SecureBuffer storage;
    u32 anchor;
    u32 caret;
    u32 revision;
    u32 maximum_codepoints;
    u16 pending_high_surrogate;
    bool password;
    bool multiline;
    SecureEditHistory* history;
};

struct SecureEditRenderWindow {
    u32 first_codepoint;
    u32 codepoints;
    u32 anchor;
    u32 caret;
};

using SecureEditUtf16Selection = TextInputSelection;
using SecureEditUtf16Change = TextInputChange;
TextInputTarget secure_edit_input_target(SecureEdit* edit) noexcept;

bool secure_edit_initialize(SecureEdit* edit, u32 capacity, bool password,
                            SecureEditPolicy policy = {}) noexcept;
void secure_edit_release(SecureEdit* edit) noexcept;
// Optional locked, byte-bounded history for ordinary fields. Budget must hold
// a full-field replacement (twice field capacity). Keeps at most 64 edits.
// Assign/clear establish a new document and erase history; failed edits do not.
bool secure_edit_enable_history(SecureEdit* edit, u32 byte_budget) noexcept;
void secure_edit_reset_history(SecureEdit* edit) noexcept;
// Groups multiple model splices as one undo unit. No nesting. A disabled
// history is a successful no-op. End keeps current text; an unchanged group
// preserves redo. Assign/clear abort grouping. Undo/redo wait while grouped.
bool secure_edit_begin_history_group(SecureEdit* edit) noexcept;
void secure_edit_end_history_group(SecureEdit* edit) noexcept;
// Finish only an ordinary input run, never an explicit/native group.
void secure_edit_break_history_run(SecureEdit* edit) noexcept;
bool secure_edit_can_undo(const SecureEdit& edit) noexcept;
bool secure_edit_can_redo(const SecureEdit& edit) noexcept;
SecureEditResult secure_edit_undo(SecureEdit* edit) noexcept;
SecureEditResult secure_edit_redo(SecureEdit* edit) noexcept;
using SecureEditHistoryCosts = TextEditHistoryCosts;
SecureEditHistoryCosts secure_edit_history_costs(const SecureEdit& edit) noexcept;
SecureEditResult secure_edit_assign(SecureEdit* edit, Utf8View text) noexcept;
SecureEditResult secure_edit_clear(SecureEdit* edit) noexcept;
// Nonzero monotonic milliseconds opts into contiguous typing/deletion runs.
// A 1000ms gap, backwards clock, navigation or another edit kind ends the run.
// Zero keeps one undo step per splice. Newlines always form separate steps.
SecureEditResult secure_edit_insert_scalar(SecureEdit* edit, u32 codepoint, u64 input_time_ms = 0) noexcept;
SecureEditResult secure_edit_insert_utf8(SecureEdit* edit, Utf8View text) noexcept;
SecureEditResult secure_edit_insert_utf16(SecureEdit* edit, u16 code_unit, u64 input_time_ms = 0) noexcept;
SecureEditResult secure_edit_backspace(SecureEdit* edit, u64 input_time_ms = 0) noexcept;
SecureEditResult secure_edit_delete_forward(SecureEdit* edit, u64 input_time_ms = 0) noexcept;
SecureEditResult secure_edit_move(SecureEdit* edit, SecureEditMove movement,
                                  bool extend_selection) noexcept;
SecureEditResult secure_edit_set_codepoint(SecureEdit* edit, u32 codepoint, bool extend_selection) noexcept;
SecureEditResult secure_edit_select_all(SecureEdit* edit) noexcept;
SecureEditResult secure_edit_select_word(SecureEdit* edit, u32 byte_position) noexcept;
// Triple click: the line containing the position (all text in one-line fields).
SecureEditResult secure_edit_select_line(SecureEdit* edit, u32 byte_position) noexcept;
void secure_edit_cancel_composition(SecureEdit* edit) noexcept;
ConstSlice<u8> secure_edit_view(const SecureEdit& edit) noexcept;
bool secure_edit_has_selection(const SecureEdit& edit) noexcept;
SecureEditResult secure_edit_utf16_length(const SecureEdit& edit, u32* units) noexcept;
SecureEditResult secure_edit_utf16_selection(const SecureEdit& edit,
                                             SecureEditUtf16Selection* selection) noexcept;
SecureEditResult secure_edit_codepoint_from_utf16(const SecureEdit& edit, u32 position,
                                                  u32* codepoint) noexcept;
SecureEditResult secure_edit_set_utf16_selection(SecureEdit* edit,
                                                 SecureEditUtf16Selection selection) noexcept;
SecureEditResult secure_edit_copy_utf16(const SecureEdit& edit, u32 start, u32 end, Slice<u16> output,
                                        u32* written, u32* next) noexcept;
SecureEditResult secure_edit_replace_utf16(SecureEdit* edit, u32 start, u32 end, ConstSlice<u16> input,
                                           SecureEditUtf16Change* change) noexcept;

// Produces the only text view that should be handed to rendering. Password
// fields emit one UTF-8 bullet per Unicode scalar; ordinary fields are copied.
// Output is not NUL terminated and is caller-owned.
SecureEditResult secure_edit_render_text(const SecureEdit& edit, Slice<char> output,
                                         u32* output_bytes) noexcept;
// Renders at most maximum_codepoints while keeping the caret visible. Anchor
// and caret are returned as codepoint boundaries relative to the window.
SecureEditResult secure_edit_render_window(const SecureEdit& edit, u32 maximum_codepoints,
                                           Slice<char> output, u32* output_bytes,
                                           SecureEditRenderWindow* window) noexcept;

} // namespace maicord
