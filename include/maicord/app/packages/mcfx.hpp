#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/render/effects/background_effect.hpp"

// The .mcfx background-effect package (plugin design 16.1). Little-endian:
//   header (16): "MCFX", u16 format = 1, u16 section count (1..9), u32 file bytes, u32 0
//   table (count x 16): u32 type, u32 offset, u32 bytes, u32 0
//   sections packed back to back after the table, ascending by type, each at most once; the last ends at file bytes.
// Types: 1 MANIFEST, 2 DXBC, 3 TEXTURE0, 4 TEXTURE1, 5 SOURCE, 6 PREVIEW, and under interface 3 7 TEXTURE2,
// 8 TEXTURE3, 9 SHADERS. MANIFEST (compact JSON, unknown members refused) and DXBC (ps_5_0) are required. The
// manifest's "interface" is 1; 2 when "uses" names clock or appearance (mc_ambient); 3 for a pass graph ("targets",
// "passes"), texture2/texture3 or a float data texture. A client before an interface refuses its packages.
// SHADERS holds a graph's shaders 1.. as whole DXBC containers back to back (shader 0 is DXBC). A texture section
// is an 8-bit RGB/RGBA PNG, or (interface 3) "MCFT", u16 width, u16 height, then width x height RGBA32F texels.
// Nothing in a package runs on the CPU: parsing checks bounds, grammar and the bytecode header; D3D11 checks the
// rest when it is selected.
namespace maicord {

inline constexpr u32 kMcfxInterfaceMaximum = 3; // the newest manifest "interface" this client runs
inline constexpr u32 kMcfxMaximumBytes = 1536u * 1024u;
inline constexpr u32 kMcfxManifestMaximumBytes = 16u * 1024u;
inline constexpr u32 kMcfxBytecodeMaximumBytes = 64u * 1024u; // each shader
inline constexpr u32 kMcfxShadersMaximumBytes = 512u * 1024u;
inline constexpr u32 kMcfxTextureMaximumBytes = 512u * 1024u;
inline constexpr u32 kMcfxSourceMaximumBytes = 64u * 1024u;
inline constexpr u32 kMcfxPreviewMaximumBytes = 256u * 1024u;
inline constexpr u32 kMcfxKnobMaximum = 28;      // 16 numeric or toggle, 4 colour, 8 choice
inline constexpr u32 kMcfxChoiceKnobMaximum = 8; // choice knobs per package
inline constexpr u32 kMcfxChoiceMaximum = 16;    // entries per choice knob
inline constexpr u32 kMcfxIdBytes = 24, kMcfxLabelBytes = 48, kMcfxHintBytes = 96, kMcfxChoiceBytes = 24;
inline constexpr u32 kMcfxSlugBytes = 40, kMcfxNameBytes = 48, kMcfxVersionBytes = 23, kMcfxLicenseBytes = 32;
inline constexpr u32 kMcfxAuthorsBytes = 200, kMcfxCreditUrlBytes = 256, kMcfxCreditNameBytes = 64;

enum class McfxError : u8 {
    none,
    invalid_input,
    truncated,     // shorter than the header, the table or a section it names
    header,        // magic, format, count, size, reserved words
    section,       // type, order, duplicate, gap, overlap, a required section missing
    oversize,      // the file or a section over its cap
    bytecode,      // not DXBC, not shader model 5.0, the wrong stage for its use, or an inconsistent container
    manifest,      // JSON, grammar, limits, knob layout or pass graph
    texture,       // not a PNG or a float data texture, or over 512 x 512
    preview,
};

enum class McfxKnobType : u8 { number, integer, toggle, color, choice };

// A knob in declaration order. `slot` is its float index in mc_knobs (row * 4 + lane); a colour owns slot..slot + 2.
struct McfxKnob {
    McfxKnobType type;
    u8 slot, choice_index, choice_count; // choice_index: into McfxPackage::choices
    bool percent;                        // a float knob shown as a percentage (0.75 reads "75 %"); the manifest's "percent"
    float minimum, maximum, fallback;    // toggle 0..1; choice 0..count-1; colour unused
    u32 rgb;                             // the colour's default, 0xRRGGBB (sRGB)
    char id[kMcfxIdBytes + 1], label[kMcfxLabelBytes + 1], hint[kMcfxHintBytes + 1];
};

struct McfxCredit {
    char url[kMcfxCreditUrlBytes + 1], author[kMcfxCreditNameBytes + 1], license[kMcfxLicenseBytes + 1];
};

// A parsed package. Strings are NUL-terminated copies; the byte views borrow the parsed file.
struct McfxPackage {
    char slug[kMcfxSlugBytes + 1], name[kMcfxNameBytes + 1], version[kMcfxVersionBytes + 1],
        license[kMcfxLicenseBytes + 1], authors[kMcfxAuthorsBytes + 1]; // authors: "A, B"
    bool has_credit;
    McfxCredit derived_from;
    u32 uses; // EffectUse mask
    u8 interface_version;
    EffectBlend blend;
    u8 requested_resolution, requested_frame_rate; // step + 1, 0 = no request
    u8 knob_count, rows;                           // rows of mc_knobs the layout fills
    McfxKnob knobs[kMcfxKnobMaximum];
    char choices[kMcfxChoiceKnobMaximum][kMcfxChoiceMaximum][kMcfxChoiceBytes + 1];
    ConstSlice<u8> texture[kEffectTextureCount], source, preview;
    // shaders[0] is the DXBC section; with a graph, shaders 1.. come from SHADERS (graph.shader_count in all).
    ConstSlice<u8> shaders[kEffectGraphShaders];
    EffectGraph graph; // pass_count 0: a single-pass effect
    u32 file_bytes;
};

// Scratch for the manifest's JSON index (one u32 per manifest byte).
struct McfxWorkspace {
    McfxPackage package;
    u32 structurals[kMcfxManifestMaximumBytes];
};

struct McfxResult {
    McfxError error;
    u32 offset; // where the error was found (file offset, or manifest offset for `manifest`)
    constexpr bool ok() const noexcept { return error == McfxError::none; }
};

// Validates a whole file and fills workspace->package. No allocation or I/O; `file` must stay alive.
McfxResult mcfx_parse(ConstSlice<u8> file, McfxWorkspace* workspace) noexcept;

// The mc_knobs rows of a package: every knob at its default, or at `values` (natural units; a colour reads
// `rgb`). Colours are written as the sRGB the author typed.
struct McfxKnobValue {
    float number;
    u32 rgb;
};
void mcfx_knob_defaults(const McfxPackage& package, McfxKnobValue (&values)[kMcfxKnobMaximum]) noexcept;
bool mcfx_knob_value_valid(const McfxKnob& knob, const McfxKnobValue& value) noexcept;
void mcfx_knob_pack(const McfxPackage& package, const McfxKnobValue (&values)[kMcfxKnobMaximum],
                    float (&rows)[kEffectKnobRows][4]) noexcept;
// The host glides mc_knobs toward a change (effects.cpp): a float or colour knob's lanes in `shown` move a share
// dt / (kMcfxKnobGlideMs + dt) of the way to `target` each frame and settle on it within a thousandth of the knob's
// range (1/1024 of a colour channel). A step shows 95 % of the way in about 0.2 s and settles within about 0.45 s; a
// drag trails the pointer by about kMcfxKnobGlideMs of its motion, never more. Whole numbers, toggles and choices
// switch at once; an effect that wants them gradual eases them itself. True while a lane still moves.
inline constexpr u32 kMcfxKnobGlideMs = 60;
bool mcfx_knob_glide(const McfxPackage& package, const float (&target)[kEffectKnobRows][4],
                     float (&shown)[kEffectKnobRows][4], u32 elapsed_ms) noexcept;

// PNG textures: the container only checks the signature and the IHDR extent (<= 512 x 512, 8-bit RGBA/RGB).
bool mcfx_png_extent(ConstSlice<u8> png, u32* width, u32* height) noexcept;
// A float data texture ("MCFT"): true with its extent; the texels follow 8 bytes in, RGBA32F, rows contiguous.
bool mcfx_float_texture(ConstSlice<u8> texture, u32* width, u32* height) noexcept;

} // namespace maicord
