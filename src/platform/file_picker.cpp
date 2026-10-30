#include "maicord/core/win32_process.hpp"
#include "maicord/platform/file_picker.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/module.hpp"
#include "maicord/sys/shell.hpp"
#include "maicord/core/win32_file_info.hpp"

namespace maicord {
namespace {
constexpr wchar_t kClassName[] = L"MaiCord.FilePicker.Cancel";
SRWLOCK g_class_lock{};
u32 g_class_users;
struct Runtime {
    Arena paths;
    ConstSlice<wchar_t> selected[kNativeFilePickerMaximumFiles];
    FileSnapshot snapshots[kNativeFilePickerMaximumFiles];
    IFileDialog* dialog;
    wchar_t save_name[256];
    bool save, destination_exists, overwrite_authorized, overwrite_observed;
    HWND owner;
    HDROP drop;
    ClipboardAttachmentData clipboard;
    UploadSource sources[kNativeFilePickerMaximumFiles];
    ConstSlice<wchar_t> filenames[kNativeFilePickerMaximumFiles];
    u64 maximum_bytes;
    NativeFilePickerResult result;
    volatile LONG cancelled;
    u32 maximum_files;
    wchar_t title[256];
};
bool cancelled(Runtime* runtime) noexcept { return InterlockedCompareExchange(&runtime->cancelled, 0, 0) != 0; }
LRESULT CALLBACK cancel_window(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* runtime = reinterpret_cast<Runtime*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_TIMER && runtime && runtime->dialog && cancelled(runtime)) {
        // This callback runs in the dialog's apartment while Show pumps events.
        runtime->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
HINSTANCE class_acquire() noexcept {
    const HINSTANCE module = module_instance();
    AcquireSRWLockExclusive(&g_class_lock);
    bool ok = true;
    if (!g_class_users) {
        WNDCLASSEXW type{};
        type.cbSize = sizeof(type); type.hInstance = module;
        type.lpfnWndProc = cancel_window; type.lpszClassName = kClassName;
        ok = RegisterClassExW(&type) != 0;
    }
    if (ok) ++g_class_users;
    ReleaseSRWLockExclusive(&g_class_lock);
    return ok ? module : nullptr;
}
void class_release(HINSTANCE module) noexcept {
    AcquireSRWLockExclusive(&g_class_lock);
    if (g_class_users && --g_class_users == 0) UnregisterClassW(kClassName, module);
    ReleaseSRWLockExclusive(&g_class_lock);
}
bool snapshot_path(ConstSlice<wchar_t> path, FileSnapshot* snapshot, NativeFilePickerResult* outcome) noexcept {
    if (!native_file_picker_valid_path(path)) { outcome->error = NativeFilePickerError::path; return false; }
    const HANDLE file = CreateFileW(path.data, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { outcome->error = NativeFilePickerError::path; outcome->native_error = thread_last_error(); return false; }
    const bool ok = file_snapshot_read(file, snapshot, &outcome->native_error);
    CloseHandle(file);
    if (!ok) outcome->error = NativeFilePickerError::path;
    return ok;
}
NativeFilePickerResult collect(Runtime* runtime) noexcept {
    IShellItemArray* items = nullptr;
    // An open dialog is created as IFileOpenDialog, so its IFileDialog pointer is one.
    HRESULT result = static_cast<IFileOpenDialog*>(runtime->dialog)->GetResults(&items);
    if (FAILED(result) || !items) return {0, static_cast<u32>(result), NativeFilePickerError::dialog};
    DWORD count = 0;
    result = items->GetCount(&count);
    if (FAILED(result) || !count || count > runtime->maximum_files) {
        items->Release();
        return {0, static_cast<u32>(result), count > runtime->maximum_files ? NativeFilePickerError::capacity : NativeFilePickerError::dialog};
    }
    NativeFilePickerResult outcome{};
    for (u32 i = 0; i < count; ++i) {
        if (cancelled(runtime)) { outcome.error = NativeFilePickerError::cancelled; break; }
        IShellItem* item = nullptr;
        PWSTR path = nullptr;
        result = items->GetItemAt(i, &item);
        if (SUCCEEDED(result) && item) result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        com_release(item);
        if (FAILED(result) || !path) {
            if (path) CoTaskMemFree(path);
            outcome.error = NativeFilePickerError::path; outcome.native_error = static_cast<u32>(result); break;
        }
        const u32 units = wide_length(path, kNativeFilePickerMaximumPathUnits + 1);
        if (snapshot_path({path, units}, &runtime->snapshots[i], &outcome)) {
            auto* copy = arena_allocate<wchar_t>(&runtime->paths, units + 1);
            if (!copy) outcome.error = NativeFilePickerError::memory;
            else {
                copy_bytes(copy, path, (units + 1) * sizeof(wchar_t));
                runtime->selected[i] = {copy, units};
            }
        }
        CoTaskMemFree(path);
        if (outcome.error != NativeFilePickerError::none) break;
    }
    items->Release();
    if (outcome.error == NativeFilePickerError::none) outcome.count = count;
    else arena_reset(&runtime->paths, ArenaRewindMode::erase);
    return outcome;
}
bool save_snapshot(const wchar_t* path, FileSnapshot* snapshot, u32* native_error) noexcept {
    const HANDLE file = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) { *native_error = thread_last_error(); return false; }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    bool ok = GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes)) != FALSE &&
        !(attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    if (!ok) *native_error = ERROR_ACCESS_DENIED;
    else ok = file_snapshot_read(file, snapshot, native_error);
    CloseHandle(file); return ok;
}
struct SaveEvents final : IFileDialogEvents {
    Runtime* runtime; LONG refs = 1;
    explicit SaveEvents(Runtime* owner) noexcept : runtime(owner) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER; *out = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IFileDialogEvents)) return E_NOINTERFACE;
        *out = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return static_cast<ULONG>(InterlockedDecrement(&refs)); }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) noexcept override {
        if (response) *response = FDESVR_DEFAULT; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem* item, FDE_OVERWRITE_RESPONSE* response) noexcept override {
        if (!response || !item) return E_POINTER;
        *response = FDEOR_DEFAULT;
        PWSTR path = nullptr; const auto result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        u32 error = 0; FileSnapshot snapshot{};
        const bool ok = SUCCEEDED(result) && path && save_snapshot(path, &snapshot, &error);
        if (path) CoTaskMemFree(path);
        if (!ok) { *response = FDEOR_REFUSE; return HRESULT_FROM_WIN32(error ? error : ERROR_ACCESS_DENIED); }
        runtime->snapshots[0] = snapshot; runtime->overwrite_observed = true;
        return S_OK; // The native overwrite confirmation remains responsible for consent.
    }
};
NativeFilePickerResult collect_save(Runtime* runtime) noexcept {
    IShellItem* item = nullptr; PWSTR path = nullptr;
    HRESULT result = runtime->dialog->GetResult(&item);
    if (SUCCEEDED(result) && item) result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    com_release(item);
    NativeFilePickerResult outcome{};
    if (FAILED(result) || !path) { outcome.error = NativeFilePickerError::path; outcome.native_error = static_cast<u32>(result); }
    else {
        const u32 units = wide_length(path, kNativeFilePickerMaximumPathUnits + 1);
        if (!native_file_picker_valid_path({path, units})) outcome.error = NativeFilePickerError::path;
        else {
            FileSnapshot current{};
            if (save_snapshot(path, &current, &outcome.native_error)) {
                if (!runtime->overwrite_observed || !file_identity_equal(current.identity, runtime->snapshots[0].identity) ||
                    current.bytes != runtime->snapshots[0].bytes) outcome.error = NativeFilePickerError::path;
                else runtime->destination_exists = runtime->overwrite_authorized = true;
            } else if (outcome.native_error != ERROR_FILE_NOT_FOUND || runtime->overwrite_observed)
                outcome.error = NativeFilePickerError::path;
            else outcome.native_error = 0;
            if (outcome.error == NativeFilePickerError::none) {
                auto* owned = arena_allocate<wchar_t>(&runtime->paths, units + 1);
                if (!owned) outcome.error = NativeFilePickerError::memory;
                else { copy_bytes(owned, path, (units + 1) * sizeof(wchar_t)); runtime->selected[0] = {owned, units}; outcome.count = 1; }
            }
        }
    }
    if (path) CoTaskMemFree(path);
    return outcome;
}
void publish(NativeFilePicker* picker, NativeFilePickerResult outcome) noexcept {
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    if (cancelled(runtime)) { outcome.count = 0; outcome.error = NativeFilePickerError::cancelled; }
    if (outcome.error != NativeFilePickerError::none) {
        outcome.count = 0; arena_reset(&runtime->paths, ArenaRewindMode::erase);
        SecureZeroMemory(runtime->selected, sizeof(runtime->selected));
        SecureZeroMemory(runtime->snapshots, sizeof(runtime->snapshots));
    }
    outcome.phase = NativeFilePickerPhase::complete;
    runtime->result = outcome;
    InterlockedExchange(&picker->phase, static_cast<LONG>(NativeFilePickerPhase::complete));
    SetEvent(picker->notification);
}
DWORD WINAPI drop_worker(void* context) noexcept {
    auto* picker = static_cast<NativeFilePicker*>(context);
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    NativeFilePickerResult outcome{};
    const u32 count = cancelled(runtime) ? 0 : DragQueryFileW(runtime->drop, 0xffffffffu, nullptr, 0);
    if (!count) outcome.error = NativeFilePickerError::path;
    else if (count > runtime->maximum_files) outcome.error = NativeFilePickerError::capacity;
    else for (u32 i = 0; i < count; ++i) {
        if (cancelled(runtime)) { outcome.error = NativeFilePickerError::cancelled; break; }
        const u32 units = DragQueryFileW(runtime->drop, i, nullptr, 0);
        if (!units || units > kNativeFilePickerMaximumPathUnits) { outcome.error = NativeFilePickerError::path; break; }
        auto* path = arena_allocate<wchar_t>(&runtime->paths, units + 1);
        if (!path) { outcome.error = NativeFilePickerError::memory; break; }
        if (DragQueryFileW(runtime->drop, i, path, units + 1) != units || path[units] != 0) {
            outcome.error = NativeFilePickerError::path; break;
        }
        if (!snapshot_path({path, units}, &runtime->snapshots[i], &outcome)) break;
        runtime->selected[i] = {path, units};
    }
    if (outcome.error == NativeFilePickerError::none) outcome.count = count;
    DragFinish(runtime->drop); runtime->drop = nullptr;
    publish(picker, outcome);
    return 0;
}
DWORD WINAPI clipboard_worker(void* context) noexcept {
    auto* picker = static_cast<NativeFilePicker*>(context);
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    NativeFilePickerResult outcome{};
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct Visit { Runtime* runtime; NativeFilePickerResult* outcome; } visit{runtime, &outcome};
    if (FAILED(initialized)) outcome.error = NativeFilePickerError::com;
    else {
        const DWORD error = clipboard_attachment_visit(&runtime->clipboard, runtime->maximum_files, runtime->maximum_bytes,
            +[](void* opaque, ConstSlice<wchar_t> path, ConstSlice<wchar_t> name, UploadSource source) noexcept {
                auto& state = *static_cast<Visit*>(opaque); auto* r = state.runtime;
                const u32 i = state.outcome->count;
                if (cancelled(r) || i >= r->maximum_files) return false;
                const auto text = path.count ? path : name;
                auto* copied = arena_allocate<wchar_t>(&r->paths, text.count + 1);
                if (!copied) return false;
                copy_bytes(copied, text.data, text.count * sizeof(wchar_t)); copied[text.count] = 0;
                if (path.count) {
                    if (!snapshot_path({copied, text.count}, &r->snapshots[i], state.outcome)) return false;
                    if (r->snapshots[i].bytes > r->maximum_bytes) { state.outcome->native_error = ERROR_FILE_TOO_LARGE; return false; }
                    r->selected[i] = {copied, text.count};
                } else { r->filenames[i] = {copied, text.count}; r->snapshots[i].bytes = source.bytes; r->sources[i] = source; }
                return true;
            }, &visit, &outcome.count);
        if (error) {
            outcome.error = error == ERROR_TOO_MANY_NAMES ? NativeFilePickerError::capacity : NativeFilePickerError::path;
            if (!outcome.native_error) outcome.native_error = error;
        }
        CoUninitialize();
    }
    publish(picker, outcome); return 0;
}
DWORD WINAPI worker(void* context) noexcept {
    auto* picker = static_cast<NativeFilePicker*>(context);
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    NativeFilePickerResult outcome{};
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HINSTANCE module = nullptr; HWND cancel = nullptr;
    SaveEvents save_events(runtime); DWORD event_cookie = 0; bool advised = false;
    if (FAILED(initialized)) { outcome.error = NativeFilePickerError::com; outcome.native_error = static_cast<u32>(initialized); }
    else {
        HRESULT result = CoCreateInstance(runtime->save ? __uuidof(FileSaveDialog) : __uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
            runtime->save ? __uuidof(IFileDialog) : __uuidof(IFileOpenDialog), reinterpret_cast<void**>(&runtime->dialog));
        if (SUCCEEDED(result)) {
            FILEOPENDIALOGOPTIONS options{};
            result = runtime->dialog->GetOptions(&options);
            if (SUCCEEDED(result)) result = runtime->dialog->SetOptions(options | FOS_FORCEFILESYSTEM |
                FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | (runtime->save ? FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN | FOS_NOTESTFILECREATE | FOS_NODEREFERENCELINKS : FOS_FILEMUSTEXIST | (runtime->maximum_files > 1 ? FOS_ALLOWMULTISELECT : 0)));
            if (SUCCEEDED(result) && runtime->save) {
                result = runtime->dialog->SetFileName(runtime->save_name);
                if (SUCCEEDED(result)) { result = runtime->dialog->Advise(&save_events, &event_cookie); advised = SUCCEEDED(result); }
            }
            if (SUCCEEDED(result) && runtime->title[0]) result = runtime->dialog->SetTitle(runtime->title);
        }
        if (SUCCEEDED(result)) {
            module = class_acquire();
            if (module) cancel = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module, runtime);
            if (!cancel || !SetTimer(cancel, 1, 100, nullptr)) result = HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
        }
        if (SUCCEEDED(result)) {
            if (cancelled(runtime) || !IsWindow(runtime->owner)) result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            else {
                InterlockedExchange(&picker->phase, static_cast<LONG>(NativeFilePickerPhase::showing));
                result = runtime->dialog->Show(runtime->owner);
            }
        }
        if (cancel) { KillTimer(cancel, 1); DestroyWindow(cancel); }
        if (module) class_release(module);
        if (cancelled(runtime) || result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) outcome.error = NativeFilePickerError::cancelled;
        else if (FAILED(result)) { outcome.error = NativeFilePickerError::dialog; outcome.native_error = static_cast<u32>(result); }
        else outcome = runtime->save ? collect_save(runtime) : collect(runtime);
        if (runtime->dialog) { if (advised) runtime->dialog->Unadvise(event_cookie); runtime->dialog->Release(); runtime->dialog = nullptr; }
        CoUninitialize();
    }
    publish(picker, outcome);
    return 0;
}
void destroy(NativeFilePicker* picker) noexcept {
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    if (runtime) {
        clipboard_attachment_release(&runtime->clipboard);
        for (auto source : runtime->sources) upload_source_release(source);
        arena_reset(&runtime->paths, ArenaRewindMode::erase);
        arena_release(&runtime->paths);
        SecureZeroMemory(runtime, sizeof(*runtime)); VirtualFree(runtime, 0, MEM_RELEASE);
    }
    if (picker->thread) CloseHandle(picker->thread);
    if (picker->notification) CloseHandle(picker->notification);
    reset_value(picker);
}
bool launch(NativeFilePicker* picker, LPTHREAD_START_ROUTINE routine) noexcept {
    picker->thread = CreateThread(nullptr, 256 * 1024, routine, picker, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    return picker->thread != nullptr;
}
// An empty title, or one without NULs that fits the dialog.
bool title_plain(Utf8View title) noexcept {
    if (title.bytes > 1024 || (!title.data && title.bytes)) return false;
    for (u32 i = 0; i < title.bytes; ++i) if (!title.data[i]) return false;
    return true;
}
bool title_widen(Runtime* runtime, Utf8View title) noexcept {
    return !title.bytes || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, title.data, static_cast<int>(title.bytes), runtime->title, 255) != 0;
}
NativeFilePickerError initialize(NativeFilePicker* picker, u32 maximum_files) noexcept {
    if (!picker || picker->runtime || picker->thread || picker->notification || !maximum_files ||
        maximum_files > kNativeFilePickerMaximumFiles) return NativeFilePickerError::invalid_input;
    reset_value(picker);
    auto* runtime = static_cast<Runtime*>(VirtualAlloc(nullptr, sizeof(Runtime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!runtime) return NativeFilePickerError::memory;
    picker->runtime = runtime; runtime->maximum_files = maximum_files;
    if (!arena_initialize(&runtime->paths, static_cast<usize>(maximum_files) * (kNativeFilePickerMaximumPathUnits + 1) * sizeof(wchar_t), 0, 4096)) {
        destroy(picker); return NativeFilePickerError::memory;
    }
    picker->notification = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!picker->notification) { destroy(picker); return NativeFilePickerError::memory; }
    picker->phase = static_cast<LONG>(NativeFilePickerPhase::starting);
    return NativeFilePickerError::none;
}
}
bool native_file_picker_valid_path(ConstSlice<wchar_t> path) noexcept {
    if (!path.data || path.count < 3 || path.count > kNativeFilePickerMaximumPathUnits) return false;
    u32 first = 0;
    bool unc = path.count > 4 && path[0] == L'\\' && path[1] == L'\\';
    if (unc && (path[2] == L'.' || path[2] == L'?')) {
        if (path[2] != L'?' || path[3] != L'\\' || path.count < 8) return false;
        first = 4;
        unc = path.count > 8 && (path[4] == L'U' || path[4] == L'u') &&
            (path[5] == L'N' || path[5] == L'n') && (path[6] == L'C' || path[6] == L'c') && path[7] == L'\\';
    }
    if (unc) {
        u32 cursor = first ? 8 : 2;
        for (u32 component = 0; component < 2; ++component) {
            const u32 start = cursor;
            while (cursor < path.count && path[cursor] != L'\\') ++cursor;
            if (cursor == start || cursor == path.count) return false;
            ++cursor;
        }
        if (cursor == path.count) return false;
    } else {
        const bool drive = ((path[first] >= L'A' && path[first] <= L'Z') || (path[first] >= L'a' && path[first] <= L'z')) &&
            path[first + 1] == L':' && path[first + 2] == L'\\';
        if (!drive) return false;
    }
    if (path[path.count - 1] == L'\\') return false;
    for (u32 i = 0; i < path.count; ++i) {
        const u16 c = path[i];
        if (c < 0x20) return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i >= path.count || path[i] < 0xdc00 || path[i] > 0xdfff) return false;
        } else if (c >= 0xdc00 && c <= 0xdfff) return false;
    }
    return true;
}
NativeFilePickerError native_file_picker_start(NativeFilePicker* picker, HWND owner, u32 maximum_files, Utf8View title) noexcept {
    if (!owner || !IsWindow(owner) || !title_plain(title)) return NativeFilePickerError::invalid_input;
    const auto initialized = initialize(picker, maximum_files);
    if (initialized != NativeFilePickerError::none) return initialized;
    auto* runtime = static_cast<Runtime*>(picker->runtime); runtime->owner = owner;
    if (!title_widen(runtime, title)) { destroy(picker); return NativeFilePickerError::invalid_input; }
    if (!launch(picker, worker)) { destroy(picker); return NativeFilePickerError::thread; }
    return NativeFilePickerError::none;
}
NativeFilePickerError native_file_picker_save_start(NativeFilePicker* picker, HWND owner, Utf8View filename, Utf8View title) noexcept {
    if (!owner || !IsWindow(owner) || !filename.data || !filename.bytes || filename.bytes > 1020 || !title_plain(title) ||
        !utf8_validate(filename).valid || !utf8_validate(title).valid) return NativeFilePickerError::invalid_input;
    for (u32 i = 0; i < filename.bytes; ++i) if (static_cast<u8>(filename.data[i]) < 32 || filename.data[i] == '/' || filename.data[i] == '\\' || filename.data[i] == ':' || filename.data[i] == 0)
        return NativeFilePickerError::invalid_input;
    const auto result = initialize(picker, 1); if (result != NativeFilePickerError::none) return result;
    auto* runtime = static_cast<Runtime*>(picker->runtime); runtime->owner = owner; runtime->save = true;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filename.data, static_cast<int>(filename.bytes), runtime->save_name, 255) ||
        !title_widen(runtime, title)) {
        destroy(picker); return NativeFilePickerError::invalid_input;
    }
    if (!launch(picker, worker)) { destroy(picker); return NativeFilePickerError::thread; }
    return NativeFilePickerError::none;
}
bool native_file_picker_save_selection(const NativeFilePicker* picker, NativeFileSaveSelection* output) noexcept {
    if (output) reset_value(output);
    const auto status = native_file_picker_status(picker);
    if (!output || !picker || !picker->runtime || !status.ok())
        return false;
    const auto* runtime = static_cast<const Runtime*>(picker->runtime);
    if (!runtime->save || !runtime->selected[0].count) return false;
    *output = {runtime->selected[0], runtime->snapshots[0], runtime->destination_exists, runtime->overwrite_authorized}; return true;
}
NativeFilePickerError native_file_picker_take_drop(NativeFilePicker* picker, HANDLE drop, u32 maximum_files) noexcept {
    if (!drop) return NativeFilePickerError::invalid_input;
    const auto initialized = initialize(picker, maximum_files);
    if (initialized != NativeFilePickerError::none) return initialized;
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    runtime->drop = static_cast<HDROP>(drop);
    if (!launch(picker, drop_worker)) { runtime->drop = nullptr; destroy(picker); return NativeFilePickerError::thread; }
    return NativeFilePickerError::none;
}
NativeFilePickerError native_file_picker_take_clipboard(NativeFilePicker* picker, ClipboardAttachmentData* input, u32 maximum_files, u64 maximum_bytes) noexcept {
    if (!input || !input->object || !maximum_bytes) return NativeFilePickerError::invalid_input;
    const auto initialized = initialize(picker, maximum_files);
    if (initialized != NativeFilePickerError::none) return initialized;
    auto* runtime = static_cast<Runtime*>(picker->runtime); runtime->clipboard = *input;
    runtime->maximum_bytes = maximum_bytes;
    if (!launch(picker, clipboard_worker)) { reset_value(&runtime->clipboard); destroy(picker); return NativeFilePickerError::thread; }
    reset_value(input); return NativeFilePickerError::none;
}
ConstSlice<wchar_t> native_file_picker_filename(const NativeFilePicker* picker, u32 index) noexcept {
    const auto status = native_file_picker_status(picker);
    return status.ok() && index < status.count ? static_cast<const Runtime*>(picker->runtime)->filenames[index] : ConstSlice<wchar_t>{};
}
UploadSource native_file_picker_source(const NativeFilePicker* picker, u32 index) noexcept {
    const auto status = native_file_picker_status(picker);
    return status.ok() && index < status.count ? static_cast<const Runtime*>(picker->runtime)->sources[index] : UploadSource{};
}
NativeFilePickerResult native_file_picker_status(const NativeFilePicker* picker) noexcept {
    if (!picker || !picker->runtime) return {};
    const auto phase = static_cast<NativeFilePickerPhase>(InterlockedCompareExchange(const_cast<volatile LONG*>(&picker->phase), 0, 0));
    return phase == NativeFilePickerPhase::complete ? static_cast<const Runtime*>(picker->runtime)->result : NativeFilePickerResult{0, 0, NativeFilePickerError::none, phase};
}
ConstSlice<wchar_t> native_file_picker_path(const NativeFilePicker* picker, u32 index) noexcept {
    const auto status = native_file_picker_status(picker);
    if (!status.ok() || index >= status.count) return {};
    return static_cast<const Runtime*>(picker->runtime)->selected[index];
}
bool native_file_picker_snapshot(const NativeFilePicker* picker, u32 index, FileSnapshot* output) noexcept {
    const auto status = native_file_picker_status(picker);
    if (!output || !status.ok() || index >= status.count) return false;
    *output = static_cast<const Runtime*>(picker->runtime)->snapshots[index];
    return true;
}
void native_file_picker_cancel(NativeFilePicker* picker) noexcept {
    if (picker && picker->runtime) InterlockedExchange(&static_cast<Runtime*>(picker->runtime)->cancelled, 1);
}
bool native_file_picker_release(NativeFilePicker* picker, u32 timeout_ms) noexcept {
    if (!picker) return false;
    if (!picker->runtime) return true;
    native_file_picker_cancel(picker);
    if (picker->thread && WaitForSingleObject(picker->thread, timeout_ms) != WAIT_OBJECT_0) return false;
    destroy(picker); return true;
}
u32 native_file_picker_committed_bytes(const NativeFilePicker* picker) noexcept {
    // Arena counters are worker-owned until terminal publication.
    if (!picker || !picker->runtime) return 0;
    const auto status = native_file_picker_status(picker);
    if (status.phase != NativeFilePickerPhase::complete) return 0;
    const auto* runtime = static_cast<const Runtime*>(picker->runtime);
    return static_cast<u32>((sizeof(Runtime) + 4095) & ~4095ull) + static_cast<u32>(runtime->paths.committed);
}

}
