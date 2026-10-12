#pragma once
#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"
namespace maicord {
struct FileIdentity { u64 index; u64 last_write; u32 volume; u32 reserved; };
struct FileSnapshot { FileIdentity identity; u64 bytes; };
// Metadata only. Caller schedules this on a worker for potentially slow files.
bool file_snapshot_read(HANDLE file, FileSnapshot* output, u32* native_error = nullptr) noexcept;
bool file_identity_equal(const FileIdentity& a, const FileIdentity& b) noexcept;
}
