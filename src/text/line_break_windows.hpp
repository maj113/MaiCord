#pragma once
#include "maicord/text/line_break.hpp"
namespace maicord {
// Experimental en-US compatibility policy. QA must retain all differences;
// the app uses it only after resolved Latin itemization. It is not a complete
// dictionary segmenter or a general fallback policy.
// Input/output and failure contracts match unicode_line_breaks. End-of-text
// preserves the observed Windows allowed/mandatory distinction.
bool line_windows_candidate(ConstSlice<u32>,Slice<u8>) noexcept;
}
