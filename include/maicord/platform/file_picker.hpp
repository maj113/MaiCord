#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/platform/file_identity.hpp"
#include "maicord/platform/clipboard_attachment.hpp"
#include "maicord/core/win32_types.hpp"
namespace maicord {
inline constexpr u32 kNativeFilePickerMaximumFiles = 64;
inline constexpr u32 kNativeFilePickerMaximumPathUnits = 32767;
enum class NativeFilePickerPhase : u8 { empty, starting, showing, complete };
enum class NativeFilePickerError : u8 { none, invalid_input, memory, thread, com, dialog, cancelled, capacity, path };
struct NativeFilePickerResult {
    u32 count;
    u32 native_error;
    NativeFilePickerError error;
    NativeFilePickerPhase phase;
    bool ok() const noexcept { return phase == NativeFilePickerPhase::complete && error == NativeFilePickerError::none; }
};
struct NativeFilePicker {
    HANDLE thread;
    HANDLE notification;
    void* runtime;
    volatile LONG phase;
};
struct NativeFileSaveSelection {
    ConstSlice<wchar_t> path;
    FileSnapshot destination;
    bool destination_exists;
    bool overwrite_authorized;
};
// Reuses the dedicated dialog STA. An existing destination has already passed
// the native overwrite prompt; the worker snapshots the chosen file.
NativeFilePickerError native_file_picker_save_start(NativeFilePicker*, HWND owner,
    Utf8View default_filename, Utf8View title = {}) noexcept;
bool native_file_picker_save_selection(const NativeFilePicker*, NativeFileSaveSelection*) noexcept;
// A dedicated STA thread runs the Windows dialog and holds its COM objects. The
// calling thread starts/polls/cancels/releases this object; no filesystem/Shell work runs there.
// Every selected absolute UTF-16 path is kept until release. Selection is
// all-or-nothing: exceeding maximum_files returns capacity, never a partial list.
NativeFilePickerError native_file_picker_start(NativeFilePicker* picker, HWND owner,
    u32 maximum_files, Utf8View title = {}) noexcept;
// Accepts a WM_DROPFILES HDROP without enumerating paths or opening files on
// the caller thread. Success transfers the handle to the worker, which calls
// DragFinish once. On failure the caller still owns the drop handle.
// Uses the same batch result/cancel/release contract as the dialog.
NativeFilePickerError native_file_picker_take_drop(NativeFilePicker* picker, HANDLE drop,
    u32 maximum_files) noexcept;
NativeFilePickerError native_file_picker_take_clipboard(NativeFilePicker* picker, ClipboardAttachmentData* input,
    u32 maximum_files, u64 maximum_bytes) noexcept;
// Virtual clipboard files expose a filename and borrowed source instead of a path.
ConstSlice<wchar_t> native_file_picker_filename(const NativeFilePicker* picker, u32 index) noexcept;
UploadSource native_file_picker_source(const NativeFilePicker* picker, u32 index) noexcept;
NativeFilePickerResult native_file_picker_status(const NativeFilePicker* picker) noexcept;
ConstSlice<wchar_t> native_file_picker_path(const NativeFilePicker* picker, u32 index) noexcept;
bool native_file_picker_snapshot(const NativeFilePicker* picker, u32 index, FileSnapshot* output) noexcept;
void native_file_picker_cancel(NativeFilePicker* picker) noexcept;
// Nonblocking with timeout=0. False keeps all worker-owned storage alive.
bool native_file_picker_release(NativeFilePicker* picker, u32 timeout_ms = 0) noexcept;
// Shared absolute-path validation for dialog, drop and clipboard files; no disk I/O.
bool native_file_picker_valid_path(ConstSlice<wchar_t> path) noexcept;
}

