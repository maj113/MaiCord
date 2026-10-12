#pragma once
#include "maicord/app/dave_key_derivation.hpp"

namespace maicord {
// Candidate SHA-256 epoch material, not proof of an authenticated MLS state.
// Keep these values in caller-owned locked storage and erase them when done.
struct MlsEpochSecrets {
    u8 joiner_secret[32], epoch_secret[32], sender_data_secret[32], encryption_secret[32];
    u8 exporter_secret[32], epoch_authenticator[32], external_secret[32], confirmation_key[32];
    u8 membership_key[32], resumption_psk[32], init_secret[32], welcome_secret[32], confirmation_tag[32];
};
void mls_epoch_clear(MlsEpochSecrets* epoch) noexcept;
// All secrets/PSK inputs are exactly 32 bytes; no-PSK means 32 zero bytes.
// Context is the already serialized GroupContext, validated by the caller.
// Initial creation uses zero commit/PSK secrets and an EMPTY confirmed hash.
// From-joiner permits empty or 32-byte confirmed hash; next requires 32 bytes.
// Every output is computed first and written atomically. Output may replace input storage
// (including previous init_secret); ALL failures preserve the previous output.
// Inputs/output must not alias the workspace. No allocation after workspace init.
KdfResult mls_epoch_initial(KdfSha256Workspace& workspace, ConstSlice<u8> init_secret,
                            ConstSlice<u8> context, MlsEpochSecrets* output) noexcept;
KdfResult mls_epoch_next(KdfSha256Workspace& workspace, ConstSlice<u8> init_secret,
                         ConstSlice<u8> commit_secret, ConstSlice<u8> psk_secret,
                         ConstSlice<u8> confirmed_hash, ConstSlice<u8> context,
                         MlsEpochSecrets* output) noexcept;
KdfResult mls_epoch_from_joiner(KdfSha256Workspace& workspace, ConstSlice<u8> joiner_secret,
                                ConstSlice<u8> psk_secret, ConstSlice<u8> confirmed_hash,
                                ConstSlice<u8> context, MlsEpochSecrets* output) noexcept;
// Transcript primitives accept caller-validated serialized hash input. These
// do not parse AuthenticatedContent or verify signatures/membership. Output is
// exactly 32 bytes; excess capacity preserved, input alias follows SHA contract.
KdfResult mls_transcript_confirm(KdfSha256Workspace& workspace, ConstSlice<u8> previous_interim,
                                 ConstSlice<u8> confirmed_input, Slice<u8> output) noexcept;
KdfResult mls_transcript_interim(KdfSha256Workspace& workspace, ConstSlice<u8> confirmed_hash,
                                 ConstSlice<u8> confirmation_tag, Slice<u8> output) noexcept;
// Fixed-length, full-byte comparison; never a prefix comparison. Call only on
// a successfully derived candidate's tag, then complete all other MLS checks.
bool mls_confirmation_matches(ConstSlice<u8> expected, ConstSlice<u8> received) noexcept;
} // namespace maicord
