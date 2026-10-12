#pragma once
#include "maicord/platform/aes_gcm.hpp"
#include "maicord/platform/xchacha20_poly1305.hpp"
#include "maicord/protocol/media_packet.hpp"
#include "maicord/protocol/voice_media_events.hpp"

namespace maicord {
enum class VoicePacketError : u8 {
    none,
    invalid_input,
    not_initialized,
    stale_generation,
    malformed_packet,
    output_too_small,
    nonce_exhausted,
    crypto
};
struct VoicePacketResult {
    u32 bytes, nonce;
    MediaPacketLayout layout;
    AeadResult crypto;
    VoicePacketError error;
    constexpr bool ok() const noexcept {
        return error == VoicePacketError::none;
    }
};
// Worker-owned, zero-initialize and never copy. All outbound RTP streams and
// RTCP for a key share this one counter. Resume with the same secret must retain
// this owner/counter; initialization is for a FRESH transport key, not Resume.
// Release destroys keys; it cannot prove a subsequently supplied key is fresh.
struct VoicePacketCrypto {
    AesGcmKey aes;
    XChaChaKey xchacha;
    u64 generation, next_nonce;
    u32 mode;
};
AeadResult voice_packet_initialize(VoicePacketCrypto* crypto, u64 generation, u32 mode,
                                   ConstSlice<u8> secret,
                                   XChaChaBackend backend = XChaChaBackend::automatic) noexcept;
void voice_packet_release(VoicePacketCrypto* crypto) noexcept;

// Input/output are full packets. Seal appends a 16-byte tag and 4-byte BE nonce.
// Exact in-place operation is allowed; partial overlaps and key aliases are not.
// Invalid/short/stale packets do not consume a nonce. Once encryption starts a
// nonce is consumed even if crypto or the subsequent UDP send fails. Never wrap.
// Rejected authentication/malformed decrypted framing clears the attempted
// plaintext packet. Returned layout/nonce are published only on success.
VoicePacketResult voice_packet_seal(VoicePacketCrypto& crypto, u64 generation, ConstSlice<u8> plaintext,
                                    Slice<u8> packet) noexcept;
VoicePacketResult voice_packet_open(VoicePacketCrypto& crypto, u64 generation, ConstSlice<u8> packet,
                                    Slice<u8> plaintext) noexcept;
// Open authenticates, but does not implement replay policy or SSRC membership.
// The receiver must apply those checks before dispatching media to DAVE/decoders.
} // namespace maicord
