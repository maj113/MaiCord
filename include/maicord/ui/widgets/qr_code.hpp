#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/ui/core/paint.hpp"

namespace maicord {

// QR Model 2 symbols of versions 1 to 10 at error-correction level M, in byte mode. A symbol's storage caps its
// version: the sign-in QR (QrCode) holds version 5, kQrCodeSize modules a side; an authenticator app's otpauth://
// link (QrCodeLarge) takes up to version 10.
inline constexpr u32 kQrCodeSize = 37;
inline constexpr u32 kQrCodeMaximumVersion = 10;
inline constexpr u32 kQrCodeMaximumSize = 17 + 4 * kQrCodeMaximumVersion;
// Version 5-M's and version 10-M's byte-mode capacities.
inline constexpr u32 kQrCodeMaximumPayloadBytes = 84;
inline constexpr u32 kQrCodeLargeMaximumPayloadBytes = 213;

template <u32 MaximumSize> struct QrCodeOf {
    static_assert(MaximumSize >= 21 && MaximumSize <= kQrCodeMaximumSize && (MaximumSize - 17) % 4 == 0);
    u8 modules[(MaximumSize * MaximumSize + 7) / 8];
    u8 size;
    u8 mask;
};
// The sign-in QR (version 5) and an authenticator link's (up to version 10).
struct QrCode : QrCodeOf<kQrCodeSize> {};
struct QrCodeLarge : QrCodeOf<kQrCodeMaximumSize> {};

// A symbol's modules as the functions below read them: bit y * size + x of `modules`, size 0 when there is none.
struct QrCodeModules {
    const u8* modules;
    u32 size;
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

// Encodes payload in the smallest version from minimum_version that holds it, up to the largest version
// `maximum_size` modules allow; writes the modules into `modules` (room for maximum_size squared bits) and the side
// and mask. Transactional: false, with nothing written, when no allowed version holds it.
bool qr_code_encode_m(ConstSlice<u8> payload, u32 minimum_version, u32 maximum_size, u8* modules, u8* size,
                      u8* mask) noexcept;
template <u32 MaximumSize>
bool qr_code_encode_m(ConstSlice<u8> payload, u32 minimum_version, QrCodeOf<MaximumSize>* output) noexcept {
    return output && qr_code_encode_m(payload, minimum_version, MaximumSize, output->modules, &output->size,
                                      &output->mask);
}
template <u32 MaximumSize> QrCodeModules qr_code_modules(const QrCodeOf<MaximumSize>& code) noexcept {
    return {code.modules, code.size <= MaximumSize ? code.size : 0u};
}

bool qr_code_module(QrCodeModules code, u32 x, u32 y) noexcept;
template <u32 MaximumSize> bool qr_code_module(const QrCodeOf<MaximumSize>& code, u32 x, u32 y) noexcept {
    return qr_code_module(qr_code_modules(code), x, y);
}
// Coalesces adjacent rows with identical horizontal black runs. A null/zero
// output is a size query and the destination remains untouched when too small.
QrCodeRectResult qr_code_compact_rects(QrCodeModules code, Slice<QrCodeRect> output) noexcept;
template <u32 MaximumSize>
QrCodeRectResult qr_code_compact_rects(const QrCodeOf<MaximumSize>& code, Slice<QrCodeRect> output) noexcept {
    return qr_code_compact_rects(qr_code_modules(code), output);
}
// Paints the symbol's modules in `dark` over the light square `area` (DIPs) the caller filled: its `quiet_dip` padding
// is the quiet zone and the symbol fills the rest in whole device pixels per module (dpi_scale device pixels to a
// DIP), centred.
bool qr_code_paint(PaintStream* stream, const UiRect& area, QrCodeModules code, float dpi_scale, float quiet_dip,
                   Color dark) noexcept;

} // namespace maicord
