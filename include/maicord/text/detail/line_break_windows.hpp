#pragma once
// Engine-internal interface shared with adapters and tests; not a stable API.
#include "maicord/text/line_break.hpp"
namespace maicord {
// Experimental en-US Windows-compatible policy, meant for resolved Latin
// itemization only. It is not a complete dictionary segmenter or a general
// fallback policy.
// Input/output and failure contracts match unicode_line_breaks. End-of-text
// keeps Windows' allowed/mandatory distinction.
bool line_windows_candidate(ConstSlice<u32>,Slice<u8>) noexcept;
}
