#pragma once
#include "maicord/core/win32_types.hpp"

struct IDataObject;
namespace maicord {
// The system clipboard's data object (a reference the caller releases) and its sequence number; null when it cannot be
// read. It and clipboard_has_attachment sit in a file of their own so that a test pasting its own data object links
// its own pair.
IDataObject* clipboard_source_open(DWORD* sequence) noexcept;
} // namespace maicord
