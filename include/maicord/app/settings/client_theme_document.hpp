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
// The optional package effect a theme asks for: "background": {"effect": "<slug>", "knobs": {...}, "universal": {...},
// "follow": {...}} next to the built-in effect names. Applied when the effect is installed and no built-in effect is
// active.
inline constexpr u32 kClientThemeEffectSlugBytes = 40, kClientThemeEffectKnobMaximum = 28,
                     kClientThemeEffectKnobIdBytes = 24, kClientThemeFollowPhaseMaximum = 16;
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
    // "follow": {"month": "<knob>", "phase": "<knob>", "phases": ["auto" | "night" | "dawn" | "day" | "dusk", ...]}:
    // knobs of the effect that hold a theme with variants on a month or a phase (client_theme_follow). Empty: none.
    char month_knob[kClientThemeEffectKnobIdBytes + 1], phase_knob[kClientThemeEffectKnobIdBytes + 1];
    u8 phases[kClientThemeFollowPhaseMaximum]; // per option of phase_knob: 0 the schedule, else ClientThemePhase + 1
    u8 zero[2]; // always 0: documents compare and hash bytewise, so the record has no padding
};
// A theme that changes with the time of day: "variants": {"night"|"dawn"|"day"|"dusk": {"base": ..., "colors": {...},
// "months": [12 objects of colours]}}. A phase starts from its base's built-in colours (the root's base when it
// names none), then its own core colours, then the current month's; the semantic tier derives from them. A phase
// the document leaves out is the root look. Older clients count the member as unsupported and show the root look.
inline constexpr u32 kClientThemePhaseCount = 4, kClientThemeColorSets = 13;
// Catalog interface: 1 a plain theme, 2 one with variants.
inline constexpr u32 kClientThemeInterfaceMaximum = 2;
enum ClientThemePhase : u8 { theme_phase_night, theme_phase_dawn, theme_phase_day, theme_phase_dusk };
struct ClientThemeVariants {
    // [phase][0] the phase's core colours, [phase][month] that month's; packed RGBA, 0 inherits.
    u32 colors[kClientThemePhaseCount][kClientThemeColorSets][static_cast<u32>(UiThemeColor::count)];
    u8 bases[kClientThemePhaseCount]; // ClientThemeBase + 1; 0: the phase is absent
};
// Silk Waves' day: each phase is held around its keyframe (night 23-01, dawn 05-07, day 11-13, dusk 17-19) and the
// next one takes over on a smoothstep across the four hours between. `amount` is the share of `to`.
struct ClientThemePhaseMix {
    u32 from, to;
    float amount;
};
constexpr ClientThemePhaseMix client_theme_phase_at(u32 minute_of_day) noexcept {
    const i32 minute = static_cast<i32>(minute_of_day % 1440u), shifted = minute >= 23 * 60 ? minute - 1440 : minute;
    const u32 phase = static_cast<u32>(shifted + 60) / 360u;
    float x = static_cast<float>(shifted - static_cast<i32>(phase * 360u + 60u)) / 240.f;
    x = x < 0 ? 0.f : x > 1 ? 1.f : x;
    return {phase, (phase + 1u) & 3u, x * x * (3 - 2 * x)};
}
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
    ClientThemeVariants variants{};
};
inline bool client_theme_document_scheduled(const ClientThemeDocument& document) noexcept {
    return (document.variants.bases[0] | document.variants.bases[1] | document.variants.bases[2] |
            document.variants.bases[3]) != 0;
}
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
ClientThemeDocumentResult client_theme_document_write(const ClientThemeDocument&, const Slice<char>& output,
                                                      u32* bytes) noexcept;
// The look on a day (1..31) of a month (1..12) at a minute of the day (0..1439): the two phases around the minute
// mixed (client_theme_mix), or across a change of base the nearer one whole; from the 5th to the 30th each phase
// turns toward the next month's colours on a smoothstep, as Silk Waves' sky does. Only colours vary; everything else
// is the root's.
Theme client_theme_document_at(const ClientThemeDocument&, u32 minute_of_day, u32 month, u32 day) noexcept;
// client_theme_document_at's clock with the theme's effect's knobs applied (ClientThemeEffect::month_knob, phase_knob):
// `month` and `phase` are those knobs' values, negative while the effect isn't the window's background. A month 1..12
// holds that month's colours (no turn toward the next); a phase option that names a phase holds that phase whole, base
// included. Anything else keeps the clock.
struct ClientThemeClock {
    u32 minute, month, day;
};
ClientThemeClock client_theme_follow(const ClientThemeEffect&, ClientThemeClock, float month, float phase) noexcept;
// Every colour of *to (core and semantic) moved from `from` by 1 - amount: amount 0 is `from`, 1 is *to.
void client_theme_mix(Theme* to, const Theme& from, float amount) noexcept;
} // namespace maicord
