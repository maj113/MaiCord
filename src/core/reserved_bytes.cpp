#include "maicord/core/reserved_bytes.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/virtual_memory.hpp"

namespace maicord {
namespace {
constexpr u32 kCommitGranularity = 64 * 1024;
} // namespace

bool reserved_bytes_initialize(ReservedBytes* bytes, u32 reserve_bytes) noexcept {
    if (!bytes || bytes->base || !reserve_bytes || reserve_bytes > 0x7FFF0000u)
        return false;
    const u32 reserved = (reserve_bytes + kCommitGranularity - 1) & ~(kCommitGranularity - 1);
    auto* base = static_cast<u8*>(pages_reserve(reserved));
    if (!base)
        return false;
    *bytes = ReservedBytes{base, reserved, 0, 0};
    return true;
}

bool reserved_bytes_commit(ReservedBytes* bytes, u32 required) noexcept {
    if (!bytes || !bytes->base || !required || required > bytes->reserved)
        return false;
    if (required <= bytes->committed)
        return true;
    u32 target = (required + kCommitGranularity - 1) & ~(kCommitGranularity - 1);
    if (target > bytes->reserved)
        target = bytes->reserved;
    u8* address = bytes->base + bytes->committed;
    if (!pages_commit(address, target - bytes->committed))
        return false;
    bytes->committed = target;
    if (target > bytes->peak)
        bytes->peak = target;
    return true;
}

void reserved_bytes_decommit(ReservedBytes* bytes) noexcept {
    if (!bytes || !bytes->base || !bytes->committed)
        return;
    // Decommitted pages are discarded; a later commit returns zero pages.
    if (pages_decommit(bytes->base, bytes->committed))
        bytes->committed = 0;
}

void reserved_bytes_release(ReservedBytes* bytes) noexcept {
    if (!bytes)
        return;
    if (bytes->base)
        pages_release(bytes->base, bytes->reserved);
    reset_value<ReservedBytes>(bytes);
}

} // namespace maicord
