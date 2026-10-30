#pragma once
#include "maicord/platform/hkdf_sha256.hpp"

namespace maicord {
// MLS SHA256 ExpandWithLabel (RFC 9420), streamed without concatenating the
// label/context. Both lengths use canonical MLS variable integers, not TLS 1.3
// one-byte lengths. Output.count is the requested size, at most 8160.
KdfResult mls_expand_label(KdfSha256Workspace& workspace, ConstSlice<u8> secret, ConstSlice<u8> label,
                           ConstSlice<u8> context, Slice<u8> output) noexcept;
// Stateless atomic ratchet step. DAVE uses only the key, so the unused MLS
// nonce is not derived. Seed is initially 16 bytes and subsequently 32.
// Generation is encoded as a big-endian uint32 in the MLS context, independent
// of the frame's little-endian truncated nonce convention.
// Writes 16 key bytes and 32 next-secret bytes only after both derive.
// The destinations are caller storage of exactly those sizes; they may replace
// seed storage but must not overlap each other, the workspace or the
// workspace's storage. The HKDF layer rejects a seed that overlaps the
// workspace. The caller commits its generation and secret only after
// success, prevents generation wrap, expires old keys, and binds the
// initial exported seed to its sender/MLS epoch.
KdfResult dave_ratchet_step(KdfSha256Workspace& workspace, ConstSlice<u8> seed, u32 generation, u8* key,
                            u8* next_secret) noexcept;
} // namespace maicord
