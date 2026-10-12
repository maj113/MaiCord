#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// One fixed address range for a rare large buffer. Initialization reserves
// address space only; pages are committed when a caller needs them and
// decommitted when it is done, so an idle owner pays no committed memory.
struct ReservedBytes {
    u8* base;
    u32 reserved;
    u32 committed;
    // Largest commit since initialization (measurement and tests).
    u32 peak;
};

bool reserved_bytes_initialize(ReservedBytes* bytes, u32 reserve_bytes) noexcept;
// Commits the prefix covering `required` bytes (rounded up to 64 KiB, bounded
// by the reservation). Existing bytes keep their address and contents.
bool reserved_bytes_commit(ReservedBytes* bytes, u32 required) noexcept;
// Returns every committed page to the system; the reservation stays.
void reserved_bytes_decommit(ReservedBytes* bytes) noexcept;
void reserved_bytes_release(ReservedBytes* bytes) noexcept;

} // namespace maicord
