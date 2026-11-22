#include "maicord/app/settings/window_placement_file.hpp"
#include "maicord/app/account/account_profile.hpp"
#include "maicord/app/net/file_persistence.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr u8 kMagic[8]{'M', 'A', 'I', 'W', 'I', 'N', 'D', 'W'};
constexpr u32 kFlagsOffset = 36, kChecksumOffset = 40;
constexpr u8 kMaximized = 1;

bool user_path(wchar_t* path, bool create_directory) noexcept {
    return state_path(path, kAccountProfileMaximumPathCharacters, L"window.bin", create_directory).ok();
}
} // namespace

bool window_placement_encode(const WindowPlacement& placement, Slice<u8> record) noexcept {
    if (!window_placement_valid(placement))
        return false;
    u8* out = record.data;
    clear_bytes(out, kWindowPlacementRecordBytes);
    copy_bytes(out, kMagic, sizeof(kMagic));
    byte_store_le32(out + 8, kWindowPlacementFileVersion);
    byte_store_le32(out + 12, kWindowPlacementRecordBytes);
    byte_store_le32(out + 16, static_cast<u32>(placement.left));
    byte_store_le32(out + 20, static_cast<u32>(placement.top));
    byte_store_le32(out + 24, static_cast<u32>(placement.right));
    byte_store_le32(out + 28, static_cast<u32>(placement.bottom));
    byte_store_le32(out + 32, placement.dpi);
    out[kFlagsOffset] = placement.maximized ? kMaximized : 0;
    byte_store_le64(out + kChecksumOffset, hash64_bytes(kHash64Seed, out, kChecksumOffset));
    return true;
}

bool window_placement_decode(ConstSlice<u8> record, WindowPlacement* placement) noexcept {
    const u8* in = record.data;
    if (record.count != kWindowPlacementRecordBytes || !bytes_equal(in, kMagic, sizeof(kMagic)) ||
        byte_load_le32(in + 8) != kWindowPlacementFileVersion || byte_load_le32(in + 12) != kWindowPlacementRecordBytes ||
        (in[kFlagsOffset] & ~kMaximized) || in[37] || in[38] || in[39] ||
        byte_load_le64(in + kChecksumOffset) != hash64_bytes(kHash64Seed, in, kChecksumOffset))
        return false;
    const WindowPlacement decoded{static_cast<i32>(byte_load_le32(in + 16)), static_cast<i32>(byte_load_le32(in + 20)),
                                  static_cast<i32>(byte_load_le32(in + 24)), static_cast<i32>(byte_load_le32(in + 28)),
                                  byte_load_le32(in + 32), in[kFlagsOffset] == kMaximized};
    if (!window_placement_valid(decoded))
        return false;
    *placement = decoded;
    return true;
}

WindowPlacementFileResult window_placement_load_file(const wchar_t* path, WindowPlacement* placement) noexcept {
    // One byte more than a record: a longer file reads as the wrong size, not as a record.
    u8 record[kWindowPlacementRecordBytes + 1];
    const FileReadResult read = file_persistence_read_file(path, {record, sizeof(record)});
    if (read.error == FileReadError::missing)
        return WindowPlacementFileResult::missing;
    if (read.error == FileReadError::size)
        return WindowPlacementFileResult::damaged;
    if (!read.ok())
        return WindowPlacementFileResult::unreadable;
    return window_placement_decode({record, read.bytes}, placement) ? WindowPlacementFileResult::ok
                                                                    : WindowPlacementFileResult::damaged;
}

WindowPlacementFileResult window_placement_save_file(const wchar_t* path, const WindowPlacement& placement) noexcept {
    const u32 length = wide_length(path, kAccountProfileMaximumPathCharacters);
    u8 record[kWindowPlacementRecordBytes];
    if (!length || !window_placement_encode(placement, {record, sizeof(record)}))
        return WindowPlacementFileResult::unwritable;
    return file_persistence_write_atomic(path, length, {record, sizeof(record)}).ok()
               ? WindowPlacementFileResult::ok
               : WindowPlacementFileResult::unwritable;
}

WindowPlacementFileResult window_placement_load(WindowPlacement* placement) noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters];
    return user_path(path, false) ? window_placement_load_file(path, placement) : WindowPlacementFileResult::unreadable;
}

WindowPlacementFileResult window_placement_save(const WindowPlacement& placement) noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters];
    return user_path(path, true) ? window_placement_save_file(path, placement) : WindowPlacementFileResult::unwritable;
}

} // namespace maicord
