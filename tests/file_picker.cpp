#include "maicord/platform/file_picker.hpp"


namespace maicord {
namespace {
template <usize N> ConstSlice<wchar_t> path(const wchar_t (&value)[N]) noexcept { return {value, static_cast<u32>(N - 1)}; }
void pump_picker_messages() noexcept {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
}
void test_file_picker(void (*check)(bool, const char*) noexcept) noexcept {
    check(native_file_picker_valid_path(path(L"C:\\folder\\a.txt")) &&
        native_file_picker_valid_path(path(L"\\\\server\\share\\a.txt")) &&
        native_file_picker_valid_path(path(L"\\\\?\\C:\\folder\\a.txt")) &&
        native_file_picker_valid_path(path(L"\\\\?\\UNC\\server\\share\\a.txt")) &&
        native_file_picker_valid_path(path(L"C:\\emoji-\xd83d\xde00.txt")),
        "native picker accepts absolute drive, UNC, extended and supplementary Unicode paths");
    check(!native_file_picker_valid_path(path(L"a.txt")) && !native_file_picker_valid_path(path(L"C:a.txt")) &&
        !native_file_picker_valid_path(path(L"C:\\")) && !native_file_picker_valid_path(path(L"\\\\server\\share")) &&
        !native_file_picker_valid_path(path(L"\\\\server\\\\a.txt")) &&
        !native_file_picker_valid_path(path(L"\\\\.\\pipe\\a")) &&
        !native_file_picker_valid_path(path(L"\\\\?\\GLOBALROOT\\Device\\a")) &&
        !native_file_picker_valid_path(path(L"C:\\a\xd83d.txt")) &&
        !native_file_picker_valid_path(path(L"C:\\a\xde00.txt")) &&
        !native_file_picker_valid_path(path(L"C:\\a\0.txt")),
        "native picker rejects relative, root-only, device and malformed Unicode paths");
    NativeFilePicker picker{};
    check(native_file_picker_status(&picker).phase == NativeFilePickerPhase::empty &&
        !native_file_picker_path(&picker, 0).count && native_file_picker_release(&picker) &&
        native_file_picker_start(&picker, nullptr, 1) == NativeFilePickerError::invalid_input,
        "empty picker has no result and rejects missing owner without starting work");
    const HWND owner = CreateWindowExW(0, L"STATIC", L"MaiCord picker cancellation fixture", WS_OVERLAPPED,
        0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) { check(false, "picker cancellation fixture creates hidden owner"); return; }
    const auto started = native_file_picker_start(&picker, owner, 10, utf8_literal("MaiCord cancellation fixture"));
    check(started == NativeFilePickerError::none, "native picker starts a dedicated dialog apartment");
    native_file_picker_cancel(&picker);
    const u64 deadline = GetTickCount64() + 10000;
    while (picker.runtime && native_file_picker_status(&picker).phase != NativeFilePickerPhase::complete && GetTickCount64() < deadline) {
        MsgWaitForMultipleObjectsEx(1, &picker.notification, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        pump_picker_messages();
    }
    const auto completed = native_file_picker_status(&picker);
    check(completed.phase == NativeFilePickerPhase::complete && completed.error == NativeFilePickerError::cancelled &&
        !completed.count && !native_file_picker_path(&picker, 0).count,
        "cancelled picker publishes no file paths and keeps apartment teardown off caller thread");
    bool released = native_file_picker_release(&picker);
    while (!released && GetTickCount64() < deadline) {
        pump_picker_messages(); Sleep(1); released = native_file_picker_release(&picker);
    }
    check(released && !picker.thread && !picker.notification && !picker.runtime,
        "picker nonblocking reap clears every caller-owned handle after thread exit");
    // Keep the owner alive while any Shell call could still send it messages.
    while (!released) { pump_picker_messages(); Sleep(10); released = native_file_picker_release(&picker); }
    NativeFileSaveSelection saved{};
    check(!native_file_picker_save_selection(&picker, &saved) &&
        native_file_picker_save_start(&picker, owner, utf8_literal("../bad.exe")) == NativeFilePickerError::invalid_input &&
        native_file_picker_save_start(&picker, owner, utf8_literal("bad:stream.txt")) == NativeFilePickerError::invalid_input &&
        !picker.runtime, "save picker rejects path and stream defaults without dialog work");
    const auto save_started = native_file_picker_save_start(&picker, owner, utf8_literal("original.bin"), utf8_literal("Save cancellation fixture"));
    check(save_started == NativeFilePickerError::none, "native Save starts its dedicated dialog STA");
    native_file_picker_cancel(&picker);
    const u64 save_deadline = GetTickCount64() + 10000;
    while (picker.runtime && native_file_picker_status(&picker).phase != NativeFilePickerPhase::complete && GetTickCount64() < save_deadline) {
        MsgWaitForMultipleObjectsEx(1, &picker.notification, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE); pump_picker_messages();
    }
    check(native_file_picker_status(&picker).error == NativeFilePickerError::cancelled &&
        !native_file_picker_save_selection(&picker, &saved) && !saved.path.count && !saved.overwrite_authorized,
        "cancelled Save publishes no destination or overwrite consent");
    released = native_file_picker_release(&picker);
    while (!released && GetTickCount64() < save_deadline) { pump_picker_messages(); Sleep(1); released = native_file_picker_release(&picker); }
    check(released && !picker.runtime && !picker.thread && !picker.notification, "Save picker reaps its owned STA before releasing borrowed paths");
    while (!released) { pump_picker_messages(); Sleep(10); released = native_file_picker_release(&picker); }
    DestroyWindow(owner);
}
}
