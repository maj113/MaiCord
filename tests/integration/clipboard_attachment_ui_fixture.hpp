#pragma once
#include <objidl.h>
namespace maicord {
// UiProbe owns this borrowed provider. Capture still marshals it for the native
// worker; no fixture operation reads or replaces the live OS clipboard.
IDataObject* clipboard_attachment_ui_fixture_provider() noexcept;
} // namespace maicord
