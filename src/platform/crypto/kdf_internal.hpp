#pragma once

#include "maicord/platform/crypto/hkdf_sha256.hpp"

namespace maicord {
namespace kdf_internal {
namespace {

// Internal linkage and no inline hint: the functions are what the Windows file's own anonymous namespace held, so a
// compiler sees them exactly as before.
KdfResult result(KdfError error, u32 bytes = 0, i32 status = 0) noexcept {
    return {bytes, status, error};
}

} // namespace
} // namespace kdf_internal
} // namespace maicord
