#include "file_drop_fixture.hpp"

#include "maicord/sys/shell.hpp"
namespace maicord {
void test_file_drop(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace file_drop_fixture;
    wchar_t filename[MAX_PATH]{}; u32 units = 0; FileSnapshot expected{};
    if (!seed(filename, &units, &expected)) { check(false, "file drop seeds temporary disk fixture"); if(filename[0]) DeleteFileW(filename); return; }
    ConstSlice<wchar_t> paths[2]{{filename, units}, {filename, units}};
    NativeFilePicker picker{};
    HDROP drop = make({paths, 2});
    const auto started = native_file_picker_take_drop(&picker, drop, 2);
    if (started != NativeFilePickerError::none) { if (drop) DragFinish(drop); check(false, "file drop starts worker"); DeleteFileW(filename); return; }
    check(await(&picker), "file drop publishes worker completion");
    FileSnapshot actual{};
    const auto selected = native_file_picker_path(&picker, 0);
    check(native_file_picker_status(&picker).ok() && native_file_picker_status(&picker).count == 2 &&
        selected.count == units && bytes_equal(selected.data, filename, units * sizeof(wchar_t)) &&
        native_file_picker_snapshot(&picker, 0, &actual) && actual.bytes == 8 && file_identity_equal(actual.identity, expected.identity),
        "file drop retains ordered paths and original disk identity");
    check(GlobalFlags(reinterpret_cast<HGLOBAL>(drop)) == GMEM_INVALID_HANDLE,
        "file drop releases transferred Shell handle before terminal publication");
    drop = make({paths, 1});
    check(native_file_picker_take_drop(&picker, drop, 1) == NativeFilePickerError::invalid_input &&
        DragQueryFileW(drop, 0xffffffffu, nullptr, 0) == 1,
        "busy selection owner rejects another drop without taking caller handle");
    DragFinish(drop);
    check(reap(&picker), "file drop nonblocking reap clears completed worker");

    drop = make({paths, 2});
    const auto limited = native_file_picker_take_drop(&picker, drop, 1);
    if (limited != NativeFilePickerError::none) DragFinish(drop);
    check(limited == NativeFilePickerError::none && await(&picker) &&
        native_file_picker_status(&picker).error == NativeFilePickerError::capacity &&
        !native_file_picker_status(&picker).count && !native_file_picker_path(&picker, 0).count,
        "file drop rejects over-capacity batch without exposing partial paths");
    reap(&picker);

    constexpr wchar_t invalid[] = L"relative.txt";
    paths[1] = {invalid, sizeof(invalid) / sizeof(wchar_t) - 1};
    drop = make({paths, 2});
    const auto partial = native_file_picker_take_drop(&picker, drop, 2);
    if (partial != NativeFilePickerError::none) DragFinish(drop);
    check(partial == NativeFilePickerError::none && await(&picker) &&
        native_file_picker_status(&picker).error == NativeFilePickerError::path &&
        !native_file_picker_path(&picker, 0).count && !native_file_picker_snapshot(&picker, 0, &actual),
        "invalid second dropped file does not expose first file metadata");
    reap(&picker);

    drop = make({paths, 1});
    const auto cancelled = native_file_picker_take_drop(&picker, drop, 1);
    if (cancelled != NativeFilePickerError::none) DragFinish(drop);
    native_file_picker_cancel(&picker);
    check(cancelled == NativeFilePickerError::none && await(&picker), "file drop cancellation reaches terminal result");
    const auto terminal = native_file_picker_status(&picker);
    check(terminal.ok() || (terminal.error == NativeFilePickerError::cancelled && !terminal.count),
        "drop cancellation either wins before publication or observes already complete result");
    check(reap(&picker) && !picker.runtime && !picker.thread && !picker.notification,
        "cancelled file drop leaves no retained worker handles");
    drop = make({paths, 1});
    check(native_file_picker_take_drop(nullptr, drop, 1) == NativeFilePickerError::invalid_input &&
        DragQueryFileW(drop, 0xffffffffu, nullptr, 0) == 1,
        "invalid file drop owner preserves caller ownership");
    DragFinish(drop); DeleteFileW(filename);
}
}
