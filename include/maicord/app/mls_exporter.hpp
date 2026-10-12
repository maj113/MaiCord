#pragma once
#include "maicord/app/dave_key_derivation.hpp"

namespace maicord {
// RFC 9420 section 8.5 for SHA-256. exporter_secret must be 32 bytes from the
// verified MLS epoch key schedule. Output.count is the exact requested length,
// 0..8160. Label/context are raw bytes, not prehashed or null-terminated strings.
// Invalid input leaves output unchanged; native failure wipes attempted output.
// Output cannot overlap inputs, workspace owner or its allocation. Temporary
// derived secrets are erased; no per-call allocation or retained exporter cache.
// This primitive does not authenticate MLS group state or authorize membership.
KdfResult mls_export_secret(KdfSha256Workspace& workspace, ConstSlice<u8> exporter_secret,
                            ConstSlice<u8> label, ConstSlice<u8> context, Slice<u8> output) noexcept;
// DAVE v1's fixed media label and eight-byte little-endian snowflake context.
// Rejects user zero. Writes 16 bytes and preserves excess destination capacity.
KdfResult dave_export_media_seed(KdfSha256Workspace& workspace, ConstSlice<u8> exporter_secret, u64 user_id,
                                 Slice<u8> output) noexcept;
} // namespace maicord
