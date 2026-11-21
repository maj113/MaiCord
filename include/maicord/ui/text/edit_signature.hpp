#pragma once

#include "maicord/core/hash.hpp"
#include "maicord/core/slice.hpp"

namespace maicord {

// The text signature of an edit layout (TextEditLayoutInput::text_signature). A scene makes it for each of its edit
// fields, and the application compares a field's layout with the field's text by it, so every side calls this one
// function. Only compared in memory.
inline u64 text_edit_signature(Utf8View text) noexcept {
    return hash64_words(kHash64Seed, text.data, text.bytes);
}

} // namespace maicord
