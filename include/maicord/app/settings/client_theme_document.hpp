#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/ui/theme/theme.hpp"
#include "maicord/ui/theme/styles.hpp"

namespace maicord {
inline constexpr u32 kClientThemeDocumentMaximumBytes = 64u * 1024u;
inline constexpr u32 kClientThemeNameMaximumBytes = 128;
inline constexpr u32 kClientThemeDocumentMaximumDepth = 8;
enum class ClientThemeBase : u8 {
    dark,
    light,
    amoled
};
enum class ClientThemeDocumentError : u8 {
    none,
    invalid_input,
    capacity,
    json,
    duplicate_field,
    missing_version,
    unsupported_version,
    type,
    range,
    text,
    reference,
    cycle
};
// The optional package effect a theme asks for: "background": {"effect": "<slug>", "knobs": {...}, "universal": {...}}
// next to the built-in effect names. Applied when the effect is installed and no built-in effect is active.
inline constexpr u32 kClientThemeEffectSlugBytes = 40, kClientThemeEffectKnobMaximum = 28,
                     kClientThemeEffectKnobIdBytes = 24;
struct ClientThemeEffectKnob {
    char id[kClientThemeEffectKnobIdBytes + 1];
    bool color; // rgb is the value; otherwise number (a toggle is 0 or 1, a choice its index)
    float number;
    u32 rgb;
};
enum ClientThemeEffectUniversal : u8 {
    theme_effect_speed = 1,
    theme_effect_resolution = 2,
    theme_effect_frame_rate = 4,
    theme_effect_opacity = 8,
    theme_effect_blend = 16,
};
struct ClientThemeEffect {
    char slug[kClientThemeEffectSlugBytes + 1];
    u8 knob_count;
    u8 universal;                // ClientThemeEffectUniversal bits that are set
    u8 speed, opacity;           // twentieths of a second per second (0..60); percent
    u8 resolution, frame_rate;   // step + 1
    u8 blend;                    // EffectBlend
    ClientThemeEffectKnob knobs[kClientThemeEffectKnobMaximum];
};
struct ClientThemeDocument {
    Theme theme{};
    UiThemeTypography typography{};
    UiThemeMetrics metrics{};
    char name[kClientThemeNameMaximumBytes + 1]{};
    u32 name_bytes{};
    ClientThemeBase base{ClientThemeBase::dark};
    // Component tier: zero colours keep their semantic-derived defaults.
    UiThemeIcons icons{};
    UiThemeSettings settings{};
    ClientThemeEffect effect{};
};
struct ClientThemeDocumentResult {
    ClientThemeDocumentError error{};
    u32 error_offset{}, unsupported_fields{};
    bool ok() const noexcept {
        return error == ClientThemeDocumentError::none;
    }
};
ClientThemeDocument client_theme_document_defaults(ClientThemeBase = ClientThemeBase::dark) noexcept;
bool client_theme_document_valid(const ClientThemeDocument&) noexcept;
bool client_theme_document_equal(const ClientThemeDocument&, const ClientThemeDocument&) noexcept;
// Partial fields inherit defaults for the same base; a changed base starts its
// built-in defaults before any overrides, independent of member order.
// The owned candidate is published only
// after complete indexed JSON/schema validation. Future token names are counted
// without applying them. Scratch belongs to the caller; no allocation or IO.
ClientThemeDocumentResult client_theme_document_parse(Utf8View, const ClientThemeDocument& defaults,
                                                      Slice<u32> structural_scratch,
                                                      ClientThemeDocument* output) noexcept;
// Canonical version 2.0 document contains every resolved typed value. Invalid
// input or insufficient capacity preserves output and bytes; null/zero output
// is a supported size query, updating bytes only on success.
ClientThemeDocumentResult client_theme_document_write(const ClientThemeDocument&, Slice<char> output,
                                                      u32* bytes) noexcept;
} // namespace maicord
