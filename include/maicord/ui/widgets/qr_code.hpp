#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

inline constexpr u32 kQrCodeSize = 37;
inline constexpr u32 kQrCodeMaximumPayloadBytes = 84;
inline constexpr u32 kQrCodeStorageBytes =
    (kQrCodeSize * kQrCodeSize + 7) / 8;

struct QrCode {
    u8 modules[kQrCodeStorageBytes];
    u8 size;
    u8 mask;
};

struct QrCodeRect {
    u8 left;
    u8 top;
    u8 right;
    u8 bottom;
};

struct QrCodeRectResult {
    u32 count;
    bool output_too_small;

    constexpr bool ok() const noexcept {
        return !output_too_small;
    }
};

// Encodes one QR Model 2 Version 5, error-correction level M symbol in byte
// mode. The fixed profile fits the remote-auth login URL without tables or heap
// state for other versions. Output is transactional.
bool qr_code_encode_v5_m(ConstSlice<u8> payload, QrCode* output) noexcept;
bool qr_code_module(const QrCode& code, u32 x, u32 y) noexcept;
// Coalesces adjacent rows with identical horizontal black runs. A null/zero
// output is a size query and the destination remains untouched when too small.
QrCodeRectResult qr_code_compact_rects(const QrCode& code,
                                       Slice<QrCodeRect> output) noexcept;

} // namespace maicord
