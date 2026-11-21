#include "maicord/app/settings/window_placement_file.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "../../support/test_output.hpp"

// The main window's placement: the restored rectangle at another DPI, the move onto a monitor's work area when it is
// off the screen, and window.bin (round trip, damaged files, a profile from a client before the file existed).
namespace maicord {
namespace {

u32 g_failures;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

constexpr bool rect_is(const WindowRect& rect, i32 left, i32 top, i32 right, i32 bottom) noexcept {
    return rect.left == left && rect.top == top && rect.right == right && rect.bottom == bottom;
}

bool same(const WindowPlacement& a, const WindowPlacement& b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom && a.dpi == b.dpi &&
           a.maximized == b.maximized;
}

// The validity rules and the DPI conversion are constant expressions: the build checks them too.
static_assert(window_placement_valid({0, 0, 1280, 800, 96, false}) && !window_placement_valid({0, 0, 1280, 800, 0, false}) &&
              !window_placement_valid({0, 0, 15, 800, 96, false}) &&
              !window_placement_valid({-40000, 0, 1280, 800, 96, false}) &&
              !window_placement_valid({0, 0, 1280, 800, 961, false}));

void dpi_conversion() noexcept {
    // Saved at 150% on a 2560x1440 monitor, restored on a 100% one: the same logical size, the same corner.
    const WindowPlacement at_150{2660, 90, 2660 + 1920, 90 + 1200, 144, false};
    check(rect_is(window_placement_at_dpi(at_150, 96), 2660, 90, 2660 + 1280, 90 + 800),
          "dpi: 1920x1200 at 150% is 1280x800 at 100%");
    check(rect_is(window_placement_at_dpi(at_150, 144), 2660, 90, 2660 + 1920, 90 + 1200),
          "dpi: the same DPI keeps the rectangle");
    check(rect_is(window_placement_at_dpi({0, 0, 1280, 800, 96, false}, 192), 0, 0, 2560, 1600),
          "dpi: 100% to 200% doubles the size");
    // Odd sizes round to the nearest pixel.
    check(rect_is(window_placement_at_dpi({-1000, -50, -1000 + 1281, -50 + 801, 144, false}, 96), -1000, -50,
                  -1000 + 854, -50 + 534),
          "dpi: odd sizes round to the nearest pixel, on a monitor left of the primary one");
    check(rect_is(window_placement_at_dpi({0, 0, 1000, 700, 120, true}, 168), 0, 0, 1400, 980),
          "dpi: 125% to 175%");
}

void clamping() noexcept {
    const WindowRect work{0, 0, 1920, 1040};
    // On the screen (it overlaps the work area inset by 32 pixels): it stays, also against an edge or partly off it.
    check(rect_is(window_rect_fit({100, 100, 900, 700}, work), 100, 100, 900, 700), "fit: a window on the screen stays");
    check(rect_is(window_rect_fit({-7, 0, 793, 600}, work), -7, 0, 793, 600),
          "fit: a window against the left edge keeps its invisible border off the screen");
    check(rect_is(window_rect_fit({1800, 900, 2600, 1500}, work), 1800, 900, 2600, 1500),
          "fit: a window mostly off the bottom right stays while 120 pixels show");
    // Not on the screen: the overlap is inside the 32-pixel inset, or there is none.
    check(rect_is(window_rect_fit({1900, 100, 2700, 700}, work), 1120, 100, 1920, 700),
          "fit: a window with 20 pixels on the screen moves in at the right edge");
    check(rect_is(window_rect_fit({100, -620, 900, 20}, work), 100, 0, 900, 640),
          "fit: a window above the work area moves down to its top");
    // The monitor of the saved rectangle is gone: the nearest monitor's work area takes it.
    check(rect_is(window_rect_fit({2020, 100, 3020, 800}, work), 920, 100, 1920, 800),
          "fit: a window on a removed monitor to the right moves onto the nearest one");
    check(rect_is(window_rect_fit({-3000, -2000, -2000, -1300}, work), 0, 0, 1000, 700),
          "fit: a window far above and left goes to the top-left corner");
    // A rectangle larger than the work area of a smaller monitor shrinks to it.
    check(rect_is(window_rect_fit({4000, 0, 6560, 1440}, {0, 40, 1280, 1024}), 0, 40, 1280, 1024),
          "fit: a window larger than the work area takes the whole work area");
    // A work area that does not start at zero: a taskbar at the top or left, a second monitor.
    check(rect_is(window_rect_fit({0, 0, 500, 400}, {-1920, 48, 0, 1080}), -500, 48, 0, 448),
          "fit: a work area left of the primary monitor below a top taskbar");
}

void codec() noexcept {
    u8 record[kWindowPlacementRecordBytes];
    const WindowPlacement cases[]{
        {100, 80, 1380, 880, 96, false},
        {-1820, 200, -100, 1100, 144, true},
        {2600, -300, 4400, 900, 120, false},
        {-32767, -32767, 32767, 32767, 960, true},
        {0, 0, 16, 16, 48, false},
    };
    bool round_trip = true;
    for (const WindowPlacement& placement : cases) {
        WindowPlacement decoded{};
        round_trip = round_trip && window_placement_encode(placement, {record, sizeof(record)}) &&
                     window_placement_decode({record, sizeof(record)}, &decoded) && same(decoded, placement);
    }
    check(round_trip, "codec: every placement decodes as it was encoded");

    check(!window_placement_encode({0, 0, 1280, 800, 0, false}, {record, sizeof(record)}) &&
              !window_placement_encode({0, 0, 1280, 800, 96, false}, {record, sizeof(record) - 1}),
          "codec: an invalid placement or a short record does not encode");

    // Every byte changed in turn: the checksum or a field check refuses the record, and the output stays.
    const WindowPlacement original{-1820, 200, -100, 1100, 144, true};
    window_placement_encode(original, {record, sizeof(record)});
    const WindowPlacement sentinel{1, 2, 300, 400, 96, false};
    bool refused = true;
    for (u32 i = 0; i < kWindowPlacementRecordBytes; ++i) {
        u8 damaged[kWindowPlacementRecordBytes];
        copy_bytes(damaged, record, sizeof(damaged));
        damaged[i] ^= 0x40;
        WindowPlacement output = sentinel;
        refused = refused && !window_placement_decode({damaged, sizeof(damaged)}, &output) && same(output, sentinel);
    }
    check(refused, "codec: a change to any byte refuses the record and keeps the output");

    // Records with a valid checksum and a wrong field: a newer version, an unknown flag, a reserved byte, an invalid
    // rectangle.
    const auto resealed = [&](u32 offset, u32 value, bool word) noexcept {
        u8 copy[kWindowPlacementRecordBytes];
        copy_bytes(copy, record, sizeof(copy));
        if (word)
            byte_store_le32(copy + offset, value);
        else
            copy[offset] = static_cast<u8>(value);
        byte_store_le64(copy + 40, hash64_bytes(kHash64Seed, copy, 40));
        WindowPlacement output = sentinel;
        return !window_placement_decode({copy, sizeof(copy)}, &output) && same(output, sentinel);
    };
    check(resealed(8, 2, true), "codec: version 2 is refused");
    check(resealed(12, 47, true), "codec: a wrong size field is refused");
    check(resealed(36, 3, false), "codec: an unknown flag is refused");
    check(resealed(38, 1, false), "codec: a reserved byte that is not zero is refused");
    check(resealed(24, static_cast<u32>(-1820), true), "codec: an empty rectangle is refused");
    check(resealed(32, 20, true), "codec: a DPI out of range is refused");
    WindowPlacement output = sentinel;
    check(!window_placement_decode({record, sizeof(record) - 1}, &output) && same(output, sentinel),
          "codec: a short record is refused");
}

bool write_file(const wchar_t* path, const void* bytes, u32 count) noexcept {
    // The atomic save leaves the file hidden: CREATE_ALWAYS must repeat the attribute to replace it.
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes, count, &written, nullptr) && written == count;
    CloseHandle(file);
    return ok;
}

void files() noexcept {
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, directory);
    const bool fixture = length && length < MAX_PATH && GetTempFileNameW(directory, L"MCW", 0, path);
    check(fixture, "files: a temporary file name");
    if (!fixture)
        return;
    const WindowPlacement sentinel{1, 2, 300, 400, 96, false};

    // A profile from a client before window.bin: no file. The window then takes today's default.
    DeleteFileW(path);
    WindowPlacement loaded = sentinel;
    check(window_placement_load_file(path, &loaded) == WindowPlacementFileResult::missing && same(loaded, sentinel),
          "files: no window.bin is missing and keeps the output");

    const WindowPlacement saved{-1820, 200, -100, 1100, 144, true};
    check(window_placement_save_file(path, saved) == WindowPlacementFileResult::ok &&
              window_placement_load_file(path, &loaded) == WindowPlacementFileResult::ok && same(loaded, saved),
          "files: a saved placement loads back");

    // Damaged files: empty, truncated, longer, and another file at that name.
    u8 record[kWindowPlacementRecordBytes + 1]{};
    window_placement_encode(saved, {record, kWindowPlacementRecordBytes});
    const u32 sizes[]{0, 16, kWindowPlacementRecordBytes - 1, kWindowPlacementRecordBytes + 1};
    bool damaged = true;
    for (const u32 size : sizes) {
        loaded = sentinel;
        damaged = damaged && write_file(path, record, size) &&
                  window_placement_load_file(path, &loaded) != WindowPlacementFileResult::ok && same(loaded, sentinel);
    }
    check(damaged, "files: an empty, short or long file is refused and keeps the output");
    const char text[] = "{\"WINDOW_BOUNDS\":{\"x\":10,\"y\":10,\"width\":1280,\"height\":720},\"IS_MAXIMIZED\":true}";
    loaded = sentinel;
    check(write_file(path, text, sizeof(text) - 1) &&
              window_placement_load_file(path, &loaded) == WindowPlacementFileResult::damaged && same(loaded, sentinel),
          "files: a file that is not a placement record is damaged");

    check(window_placement_save_file(path, {0, 0, 1280, 800, 0, false}) == WindowPlacementFileResult::unwritable,
          "files: an invalid placement is not written");
    DeleteFileW(path);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_window_placement_entry) {
    using namespace maicord;
    dpi_conversion();
    clamping();
    codec();
    files();
    test_exit(g_failures, "window placement");
}
