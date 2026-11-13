#include "clipboard_source.hpp"
#include "maicord/platform/window/clipboard.hpp"
#include "maicord/sys/shell.hpp"

namespace maicord {
IDataObject* clipboard_source_open(DWORD* sequence) noexcept {
    IDataObject* object = nullptr;
    if (FAILED(OleGetClipboard(&object)))
        return nullptr;
    *sequence = GetClipboardSequenceNumber();
    return object;
}
bool clipboard_has_attachment() noexcept {
    const UINT descriptors = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW),
               png = RegisterClipboardFormatW(L"PNG");
    return IsClipboardFormatAvailable(CF_HDROP) ||
           (descriptors && IsClipboardFormatAvailable(descriptors)) ||
           (png && IsClipboardFormatAvailable(png)) || IsClipboardFormatAvailable(CF_DIBV5) ||
           IsClipboardFormatAvailable(CF_DIB);
}
} // namespace maicord
