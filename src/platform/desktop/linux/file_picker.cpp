// platform/desktop/file_picker.hpp on Linux. The dialog is the FileChooser portal (platform/dbus/file_chooser.hpp): the
// caller starts it on the UI thread, the session bus processing settles it (the portal's answer arrives inside the
// window loop) and `notification` is signaled then. Dropped files and a pasted attachment are checked on a worker
// thread, as on Windows, and published the same way; the clipboard's files and image are copied out of the capture
// when it is taken, because the caller releases the capture at once.
//
// Paths are UTF-16 as everywhere in shared code (D16), absolute and rooted at '/'; a file name that holds a backslash
// is refused (the file layer reads a backslash as a separator).
#include "maicord/platform/desktop/file_picker.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/heap.hpp"
#include "maicord/core/interlocked.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/os/file.hpp"
#include "maicord/os/text.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/dbus/file_chooser.hpp"
#include "maicord/platform/window/wayland.hpp"

namespace maicord {
namespace {

// A pasted image: its PNG bytes, shared by the upload that reads them.
struct MemorySource {
    volatile LONG references;
    u64 bytes;
    u8* data;
};

void source_retain(void* opaque) noexcept {
    InterlockedIncrement(&static_cast<MemorySource*>(opaque)->references);
}

void source_release(void* opaque) noexcept {
    auto* source = static_cast<MemorySource*>(opaque);
    if (InterlockedDecrement(&source->references) == 0) {
        heap_free(source->data);
        heap_free(source);
    }
}

ConstSlice<u8> source_read(void* opaque, u64 offset, Slice<u8> scratch) noexcept {
    const auto& source = *static_cast<const MemorySource*>(opaque);
    if (offset >= source.bytes || !scratch.count)
        return {};
    // The bytes are immutable and owned here: an exact range is a view, a read into scratch is the same view.
    if (!scratch.data)
        return scratch.count > source.bytes - offset ? ConstSlice<u8>{} : ConstSlice<u8>{source.data + offset, scratch.count};
    return {source.data + offset, static_cast<u32>(minimum<u64>(source.bytes - offset, scratch.count))};
}

const UploadSourceOps kSourceOperations{source_retain, source_release, source_read};

struct Runtime {
    Arena paths;
    ConstSlice<wchar_t> selected[kNativeFilePickerMaximumFiles];
    FileSnapshot snapshots[kNativeFilePickerMaximumFiles];
    UploadSource sources[kNativeFilePickerMaximumFiles];
    ConstSlice<wchar_t> filenames[kNativeFilePickerMaximumFiles];
    // What the worker checks: dropped or pasted files (into `selected`) and the pasted image's source.
    ConstSlice<wchar_t> pending[kNativeFilePickerMaximumFiles];
    u32 pending_count;
    bool pending_too_many;
    UploadSource image;
    u64 maximum_bytes;
    ClipboardAttachmentOversize oversize;
    NativeFilePickerResult result;
    volatile LONG cancelled;
    u32 maximum_files;
    bool save, destination_exists;
    dbus::FileChooser* chooser;
    DroppedFiles* drop;
    Thread* thread;
};

bool cancelled(const Runtime* runtime) noexcept {
    return read_acquire(&runtime->cancelled) != 0;
}

void note_oversize(Runtime* runtime, ConstSlice<wchar_t> name, u64 bytes) noexcept {
    auto& oversize = runtime->oversize;
    const u32 units = minimum<u32>(static_cast<u32>(name.count), 259);
    copy_bytes(oversize.name, name.data, units * sizeof(wchar_t));
    oversize.name[units] = 0;
    oversize.name_units = units;
    oversize.bytes = bytes;
}

bool snapshot_path(ConstSlice<wchar_t> path, FileSnapshot* snapshot, NativeFilePickerResult* outcome) noexcept {
    if (!native_file_picker_valid_path(path)) {
        outcome->error = NativeFilePickerError::path;
        return false;
    }
    const FileHandle file = file_open(path.data, {FileAccess::attributes, FileCreate::existing,
                                                  kFileShareRead | kFileShareWrite | kFileShareDelete, 0});
    if (file == INVALID_HANDLE_VALUE) {
        outcome->error = NativeFilePickerError::path;
        outcome->native_error = thread_last_error();
        return false;
    }
    const bool ok = file_snapshot_read(file, snapshot, &outcome->native_error);
    file_close(file);
    if (!ok)
        outcome->error = NativeFilePickerError::path;
    return ok;
}

// A copy of `path` in the runtime's arena, terminated; empty when the arena is full.
ConstSlice<wchar_t> keep(Runtime* runtime, ConstSlice<wchar_t> path) noexcept {
    auto* copy = arena_allocate<wchar_t>(&runtime->paths, path.count + 1);
    if (!copy)
        return {};
    copy_bytes(copy, path.data, path.count * sizeof(wchar_t));
    copy[path.count] = 0;
    return {copy, path.count};
}

void publish(NativeFilePicker* picker, NativeFilePickerResult outcome) noexcept {
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    if (cancelled(runtime)) {
        outcome.count = 0;
        outcome.error = NativeFilePickerError::cancelled;
    }
    if (outcome.error != NativeFilePickerError::none) {
        outcome.count = 0;
        arena_reset(&runtime->paths, ArenaRewindMode::erase);
        SecureZeroMemory(runtime->selected, sizeof(runtime->selected));
        SecureZeroMemory(runtime->snapshots, sizeof(runtime->snapshots));
    }
    outcome.phase = NativeFilePickerPhase::complete;
    runtime->result = outcome;
    InterlockedExchange(&picker->phase, static_cast<LONG>(NativeFilePickerPhase::complete));
    event_set(picker->notification);
}

// The portal's answer, as paths checked on this thread (a few stat calls).
void collect_dialog(NativeFilePicker* picker) noexcept {
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    NativeFilePickerResult outcome{};
    switch (dbus::file_chooser_state(runtime->chooser)) {
    case dbus::FileChooserState::done: {
        const u32 count = dbus::file_chooser_count(runtime->chooser);
        if (!count || count > runtime->maximum_files) {
            outcome.error = NativeFilePickerError::capacity;
            break;
        }
        for (u32 i = 0; i < count; ++i) {
            const char* text = dbus::file_chooser_path(runtime->chooser, i);
            u32 bytes = 0;
            while (text[bytes])
                ++bytes;
            auto* wide = arena_allocate<wchar_t>(&runtime->paths, bytes + 1);
            const u32 units = wide ? utf8_to_utf16(text, bytes, wide, bytes + 1, true) : 0;
            if (!wide) {
                outcome.error = NativeFilePickerError::memory;
            } else if (!units || units > kNativeFilePickerMaximumPathUnits) {
                outcome.error = NativeFilePickerError::path;
            } else {
                wide[units] = 0;
                if (runtime->save) {
                    // The portal's own overwrite prompt has been answered; a destination that exists is recorded.
                    FileSnapshot current{};
                    NativeFilePickerResult probe{};
                    if (snapshot_path({wide, units}, &current, &probe)) {
                        runtime->snapshots[0] = current;
                        runtime->destination_exists = true;
                    } else if (probe.native_error != ERROR_FILE_NOT_FOUND || !native_file_picker_valid_path({wide, units})) {
                        outcome.error = NativeFilePickerError::path;
                        outcome.native_error = probe.native_error;
                    }
                } else {
                    snapshot_path({wide, units}, &runtime->snapshots[i], &outcome);
                }
                if (outcome.error == NativeFilePickerError::none)
                    runtime->selected[i] = {wide, units};
            }
            if (outcome.error != NativeFilePickerError::none)
                break;
        }
        if (outcome.error == NativeFilePickerError::none)
            outcome.count = count;
        break;
    }
    case dbus::FileChooserState::cancelled:
        outcome.error = NativeFilePickerError::cancelled;
        break;
    case dbus::FileChooserState::capacity:
        outcome.error = NativeFilePickerError::capacity;
        break;
    default:
        outcome.error = NativeFilePickerError::dialog;
        break;
    }
    publish(picker, outcome);
}

void dialog_settled(void* context) noexcept {
    collect_dialog(static_cast<NativeFilePicker*>(context));
}

// A worker over paths in runtime->pending (the files of a drop or the clipboard).
void check_pending(NativeFilePicker* picker, NativeFilePickerResult* outcome, bool sized) noexcept {
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    const u32 count = runtime->pending_count;
    if (runtime->pending_too_many || count > runtime->maximum_files) {
        outcome->error = NativeFilePickerError::capacity;
        return;
    }
    if (!count) {
        outcome->error = NativeFilePickerError::path;
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        if (cancelled(runtime)) {
            outcome->error = NativeFilePickerError::cancelled;
            return;
        }
        const ConstSlice<wchar_t> path = runtime->pending[i];
        if (!snapshot_path(path, &runtime->snapshots[i], outcome))
            return;
        if (sized && runtime->snapshots[i].bytes > runtime->maximum_bytes) {
            outcome->error = NativeFilePickerError::path;
            outcome->native_error = ERROR_FILE_TOO_LARGE;
            u32 first = path.count;
            while (first && path[first - 1] != L'/')
                --first;
            note_oversize(runtime, {path.data + first, path.count - first}, runtime->snapshots[i].bytes);
            outcome->oversize_bytes = runtime->snapshots[i].bytes;
            return;
        }
        runtime->selected[i] = path;
    }
    outcome->count = count;
}

u32 drop_worker(void* context) noexcept {
    auto* picker = static_cast<NativeFilePicker*>(context);
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    NativeFilePickerResult outcome{};
    const u32 count = cancelled(runtime) ? 0 : dropped_files_count(runtime->drop);
    if (count > runtime->maximum_files) {
        runtime->pending_too_many = true;
    } else {
        for (u32 i = 0; i < count && outcome.error == NativeFilePickerError::none; ++i) {
            const u32 units = dropped_files_path_length(runtime->drop, i);
            if (!units || units > kNativeFilePickerMaximumPathUnits) {
                outcome.error = NativeFilePickerError::path;
                break;
            }
            auto* path = arena_allocate<wchar_t>(&runtime->paths, units + 1);
            if (!path) {
                outcome.error = NativeFilePickerError::memory;
            } else if (dropped_files_path(runtime->drop, i, {path, units + 1}) != units) {
                outcome.error = NativeFilePickerError::path;
            } else {
                path[units] = 0;
                runtime->pending[runtime->pending_count++] = {path, units};
            }
        }
    }
    dropped_files_release(runtime->drop);
    runtime->drop = nullptr;
    if (outcome.error == NativeFilePickerError::none)
        check_pending(picker, &outcome, false);
    publish(picker, outcome);
    return 0;
}

u32 clipboard_worker(void* context) noexcept {
    auto* picker = static_cast<NativeFilePicker*>(context);
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    NativeFilePickerResult outcome{};
    if (runtime->image.context) {
        // The pasted image: one virtual file, refused when it exceeds the allowance.
        static const wchar_t kName[] = L"image.png";
        if (runtime->image.bytes > runtime->maximum_bytes) {
            note_oversize(runtime, {kName, 9}, runtime->image.bytes);
            outcome.error = NativeFilePickerError::path;
            outcome.native_error = ERROR_FILE_TOO_LARGE;
            outcome.oversize_bytes = runtime->image.bytes;
        } else {
            const ConstSlice<wchar_t> name = keep(runtime, {kName, 9});
            if (!name.data) {
                outcome.error = NativeFilePickerError::memory;
            } else {
                runtime->filenames[0] = name;
                runtime->snapshots[0].bytes = runtime->image.bytes;
                runtime->sources[0] = runtime->image;
                runtime->image = {};
                outcome.count = 1;
            }
        }
    } else {
        check_pending(picker, &outcome, true);
    }
    publish(picker, outcome);
    return 0;
}

void destroy(NativeFilePicker* picker) noexcept {
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    if (runtime) {
        if (runtime->chooser)
            dbus::file_chooser_release(runtime->chooser);
        if (runtime->drop)
            dropped_files_release(runtime->drop);
        upload_source_release(runtime->image);
        for (const auto source : runtime->sources)
            upload_source_release(source);
        if (runtime->thread)
            thread_close(runtime->thread);
        arena_reset(&runtime->paths, ArenaRewindMode::erase);
        arena_release(&runtime->paths);
        SecureZeroMemory(runtime, sizeof(*runtime));
        pages_release(runtime);
    }
    if (picker->notification)
        waitable_close(picker->notification);
    reset_value(picker);
}

NativeFilePickerError initialize(NativeFilePicker* picker, u32 maximum_files) noexcept {
    if (!picker || picker->runtime || picker->thread || picker->notification || !maximum_files ||
        maximum_files > kNativeFilePickerMaximumFiles)
        return NativeFilePickerError::invalid_input;
    reset_value(picker);
    auto* runtime = commit_pages<Runtime>();
    if (!runtime)
        return NativeFilePickerError::memory;
    picker->runtime = runtime;
    runtime->maximum_files = maximum_files;
    if (!arena_initialize(&runtime->paths,
                          static_cast<usize>(maximum_files) * (kNativeFilePickerMaximumPathUnits + 1) * sizeof(wchar_t), 0,
                          4096, ArenaTag::platform)) {
        destroy(picker);
        return NativeFilePickerError::memory;
    }
    picker->notification = event_create(true, false);
    if (!picker->notification) {
        destroy(picker);
        return NativeFilePickerError::memory;
    }
    picker->phase = static_cast<LONG>(NativeFilePickerPhase::starting);
    return NativeFilePickerError::none;
}

// Runs `routine` on a thread of its own. The picker's `thread` is that thread's waitable (what callers wait on
// before they release the picker); the Thread itself is the runtime's.
bool launch(NativeFilePicker* picker, ThreadProc routine) noexcept {
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    runtime->thread = thread_start(routine, picker, 256 * 1024, nullptr);
    if (!runtime->thread)
        return false;
    picker->thread = thread_waitable(runtime->thread);
    return true;
}

// An empty title, or one without NULs that fits the dialog.
bool title_plain(Utf8View title) noexcept {
    if (title.bytes > 1024 || (!title.data && title.bytes))
        return false;
    for (u32 i = 0; i < title.bytes; ++i)
        if (!title.data[i])
            return false;
    return utf8_validate(title).valid;
}

NativeFilePickerError open_dialog(NativeFilePicker* picker, Utf8View title, Utf8View file_name, bool save) noexcept {
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    runtime->save = save;
    char title_text[1025];
    char name_text[1025];
    copy_bytes(title_text, title.data, title.bytes);
    title_text[title.bytes] = 0;
    copy_bytes(name_text, file_name.data, file_name.bytes);
    name_text[file_name.bytes] = 0;
    dbus::FileChooserOptions options{};
    options.title = title.bytes ? title_text : nullptr;
    options.multiple = runtime->maximum_files > 1;
    options.current_name = save ? name_text : nullptr;
    options.settled = dialog_settled;
    options.settled_context = picker;
    runtime->chooser = save ? dbus::file_chooser_save(options) : dbus::file_chooser_open(options);
    if (!runtime->chooser)
        return NativeFilePickerError::dialog;
    InterlockedExchange(&picker->phase, static_cast<LONG>(NativeFilePickerPhase::showing));
    return NativeFilePickerError::none;
}

} // namespace

bool native_file_picker_valid_path(ConstSlice<wchar_t> path) noexcept {
    if (!path.data || path.count < 2 || path.count > kNativeFilePickerMaximumPathUnits || path[0] != L'/' ||
        path[path.count - 1] == L'/')
        return false;
    for (u32 i = 0; i < path.count; ++i) {
        const u16 c = path[i];
        if (c < 0x20 || c == L'\\')
            return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i >= path.count || path[i] < 0xdc00 || path[i] > 0xdfff)
                return false;
        } else if (c >= 0xdc00 && c <= 0xdfff) {
            return false;
        }
    }
    return true;
}

NativeFilePickerError native_file_picker_start(NativeFilePicker* picker, Window window, u32 maximum_files,
                                               Utf8View title) noexcept {
    if (!window || !title_plain(title))
        return NativeFilePickerError::invalid_input;
    const auto initialized = initialize(picker, maximum_files);
    if (initialized != NativeFilePickerError::none)
        return initialized;
    const auto opened = open_dialog(picker, title, {}, false);
    if (opened != NativeFilePickerError::none)
        destroy(picker);
    return opened;
}

NativeFilePickerError native_file_picker_save_start(NativeFilePicker* picker, Window window, Utf8View filename,
                                                    Utf8View title) noexcept {
    if (!window || !filename.data || !filename.bytes || filename.bytes > 1020 || !title_plain(title) ||
        !utf8_validate(filename).valid)
        return NativeFilePickerError::invalid_input;
    for (u32 i = 0; i < filename.bytes; ++i)
        if (static_cast<u8>(filename.data[i]) < 32 || filename.data[i] == '/' || filename.data[i] == '\\' ||
            filename.data[i] == ':')
            return NativeFilePickerError::invalid_input;
    const auto initialized = initialize(picker, 1);
    if (initialized != NativeFilePickerError::none)
        return initialized;
    const auto opened = open_dialog(picker, title, filename, true);
    if (opened != NativeFilePickerError::none)
        destroy(picker);
    return opened;
}

bool native_file_picker_save_selection(const NativeFilePicker* picker, NativeFileSaveSelection* output) noexcept {
    if (output)
        reset_value(output);
    const auto status = native_file_picker_status(picker);
    if (!output || !picker || !picker->runtime || !status.ok())
        return false;
    const auto* runtime = static_cast<const Runtime*>(picker->runtime);
    if (!runtime->save || !runtime->selected[0].count)
        return false;
    *output = {runtime->selected[0], runtime->snapshots[0], runtime->destination_exists, runtime->destination_exists};
    return true;
}

NativeFilePickerError native_file_picker_take_drop(NativeFilePicker* picker, DroppedFiles* drop,
                                                   u32 maximum_files) noexcept {
    if (!drop)
        return NativeFilePickerError::invalid_input;
    const auto initialized = initialize(picker, maximum_files);
    if (initialized != NativeFilePickerError::none)
        return initialized;
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    runtime->drop = drop;
    if (!launch(picker, drop_worker)) {
        runtime->drop = nullptr;
        destroy(picker);
        return NativeFilePickerError::thread;
    }
    return NativeFilePickerError::none;
}

NativeFilePickerError native_file_picker_take_clipboard(NativeFilePicker* picker, ClipboardCapture* input,
                                                        u32 maximum_files, u64 maximum_bytes) noexcept {
    if (!input || !maximum_bytes)
        return NativeFilePickerError::invalid_input;
    const auto initialized = initialize(picker, maximum_files);
    if (initialized != NativeFilePickerError::none)
        return initialized;
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    runtime->maximum_bytes = maximum_bytes;
    bool copied = true;
    if (clipboard_capture_kind(input) == ClipboardCaptureKind::image_png) {
        const ConstSlice<u8> png = clipboard_capture_png(input);
        auto* source = static_cast<MemorySource*>(heap_allocate(sizeof(MemorySource), true));
        auto* data = png.count ? static_cast<u8*>(heap_allocate(png.count)) : nullptr;
        if (!source || !data) {
            heap_free(source);
            heap_free(data);
            copied = false;
        } else {
            copy_bytes(data, png.data, png.count);
            source->references = 1;
            source->bytes = png.count;
            source->data = data;
            runtime->image = {source, &kSourceOperations, png.count};
        }
    } else {
        // The capture's files are paths the worker checks: copy them while the capture is still ours.
        const DroppedFiles* files = clipboard_capture_files(input);
        const u32 count = dropped_files_count(files);
        runtime->pending_too_many = count > runtime->maximum_files;
        for (u32 i = 0; copied && !runtime->pending_too_many && i < count; ++i) {
            const u32 units = dropped_files_path_length(files, i);
            auto* path = units && units <= kNativeFilePickerMaximumPathUnits
                             ? arena_allocate<wchar_t>(&runtime->paths, units + 1)
                             : nullptr;
            if (!path || dropped_files_path(files, i, {path, units + 1}) != units) {
                copied = false;
            } else {
                path[units] = 0;
                runtime->pending[runtime->pending_count++] = {path, units};
            }
        }
    }
    if (!copied) {
        destroy(picker);
        return NativeFilePickerError::memory;
    }
    if (!launch(picker, clipboard_worker)) {
        destroy(picker);
        return NativeFilePickerError::thread;
    }
    return NativeFilePickerError::none;
}

ConstSlice<wchar_t> native_file_picker_filename(const NativeFilePicker* picker, u32 index) noexcept {
    const auto status = native_file_picker_status(picker);
    return status.ok() && index < status.count ? static_cast<const Runtime*>(picker->runtime)->filenames[index]
                                               : ConstSlice<wchar_t>{};
}

UploadSource native_file_picker_source(const NativeFilePicker* picker, u32 index) noexcept {
    const auto status = native_file_picker_status(picker);
    return status.ok() && index < status.count ? static_cast<const Runtime*>(picker->runtime)->sources[index]
                                               : UploadSource{};
}

ConstSlice<wchar_t> native_file_picker_oversize_name(const NativeFilePicker* picker) noexcept {
    if (!picker || !picker->runtime ||
        static_cast<NativeFilePickerPhase>(read_acquire(&picker->phase)) != NativeFilePickerPhase::complete)
        return {};
    const auto& oversize = static_cast<const Runtime*>(picker->runtime)->oversize;
    return {oversize.name, oversize.name_units};
}

NativeFilePickerResult native_file_picker_status(const NativeFilePicker* picker) noexcept {
    if (!picker || !picker->runtime)
        return {};
    const auto phase = static_cast<NativeFilePickerPhase>(read_acquire(&picker->phase));
    return phase == NativeFilePickerPhase::complete
               ? static_cast<const Runtime*>(picker->runtime)->result
               : NativeFilePickerResult{0, 0, NativeFilePickerError::none, phase};
}

ConstSlice<wchar_t> native_file_picker_path(const NativeFilePicker* picker, u32 index) noexcept {
    const auto status = native_file_picker_status(picker);
    if (!status.ok() || index >= status.count)
        return {};
    return static_cast<const Runtime*>(picker->runtime)->selected[index];
}

bool native_file_picker_snapshot(const NativeFilePicker* picker, u32 index, FileSnapshot* output) noexcept {
    const auto status = native_file_picker_status(picker);
    if (!output || !status.ok() || index >= status.count)
        return false;
    *output = static_cast<const Runtime*>(picker->runtime)->snapshots[index];
    return true;
}

void native_file_picker_cancel(NativeFilePicker* picker) noexcept {
    if (!picker || !picker->runtime)
        return;
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    if (InterlockedExchange(&runtime->cancelled, 1))
        return;
    // A dialog still up is closed and ends cancelled; the workers see the flag at their next check.
    if (runtime->chooser && static_cast<NativeFilePickerPhase>(read_acquire(&picker->phase)) != NativeFilePickerPhase::complete) {
        dbus::file_chooser_cancel(runtime->chooser);
        publish(picker, {});
    }
}

bool native_file_picker_release(NativeFilePicker* picker, u32 timeout_ms) noexcept {
    if (!picker)
        return false;
    if (!picker->runtime)
        return true;
    native_file_picker_cancel(picker);
    auto* runtime = static_cast<Runtime*>(picker->runtime);
    if (runtime->thread && !thread_wait(runtime->thread, timeout_ms))
        return false;
    destroy(picker);
    return true;
}

} // namespace maicord
