#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/text/outline.hpp"
#include "maicord/ui/text_style.hpp"
#include "maicord/ui/theme_styles.hpp"
#include "maicord/text/font_run_handle.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {
struct NativeFontSession;

enum class TextShapeError : u8 {
    none,
    invalid_input,
    invalid_utf8,
    output_too_small,
    unsupported,
};

enum class TextWrapping : u8 {
    wrap,
    no_wrap,
};

enum class TextOverflow : u8 {
    clip,
    ellipsis,
};

struct TextShapeResult {
    HRESULT native_error;
    u32 utf16_units;
    u32 glyph_count;
    u32 run_count;
    TextShapeError error;

    constexpr bool ok() const noexcept {
        return error == TextShapeError::none;
    }
};

struct TextMeasureResult {
    HRESULT native_error;
    float width;
    float width_including_trailing_whitespace;
    float height;
    u32 utf16_units;
    u32 line_count;
    TextShapeError error;

    constexpr bool ok() const noexcept {
        return error == TextShapeError::none;
    }
};

struct TextGlyph {
    float advance;
    float advance_offset;
    float ascender_offset;
    u16 index;
    u16 reserved;
};

// Every run holds one reference to its native font run (released by
// shaped_text_reset). native_font is false only for runs without a font
// reference, such as a test reference shaper's output.
struct TextRun {
    NativeFontRunHandle native_face;
    float baseline_x;
    float baseline_y;
    float em_size;
    u32 first_glyph;
    u32 glyph_count;
    u32 text_position;
    u32 text_length;
    u32 style_flags;
    u8 bidi_level;
    bool sideways;
    bool native_font;
};

struct TextHitSpan {
    u32 byte_offset;
    u32 byte_count;
    u16 semantic_index;
    u16 reserved;
};

struct TextHitRegion {
    float left;
    float top;
    float right;
    float bottom;
    u16 semantic_index;
    u16 reserved;
};

// Validated UTF-16 range, independent of the layout provider. Input hit spans
// remain UTF-8 byte ranges and may overlap or arrive in any order.
struct TextHitRange {u32 first_unit,unit_count;u16 semantic_index,reserved;};

inline constexpr u32 kTextHitSpanMaximum = 128;

struct ShapedText {
    TextGlyph* glyphs;
    TextRun* runs;
    u16* clusters;
    TextDecoration* decorations;
    TextHitRegion* hit_regions;
    u32 glyph_capacity;
    u32 run_capacity;
    u32 cluster_capacity;
    u32 decoration_capacity;
    u32 hit_region_capacity;
    u32 glyph_count;
    u32 run_count;
    u32 cluster_count;
    u32 decoration_count;
    u32 hit_region_count;
    float width;
    float width_including_trailing_whitespace;
    float height;
    u32 line_count;
    // Packed into the bool plus trailing padding. NativeFontSession is page-aligned;
    // bit zero stores the inline-object flag, other bits hold one session reference.
    // The producer/reset retains and releases it, not these accessors.
    u64 owner_flags;
    bool unsupported_inline_object() const noexcept {return (owner_flags&1)!=0;}
    void set_unsupported_inline_object(bool value) noexcept {owner_flags=(owner_flags&~u64{1})|static_cast<u64>(value);}
    NativeFontSession* native_fonts() const noexcept {return reinterpret_cast<NativeFontSession*>(owner_flags&~u64{1});}
    void set_native_fonts(NativeFontSession* value) noexcept {owner_flags=reinterpret_cast<u64>(value)|(owner_flags&1);}
};

struct NativeTextContext;
// Borrowed policy owned by a prepared renderer typography snapshot. Native
// contexts hold the code-font declaration (native_text_context_set_code_format).
struct TextSystemTypography {
    UiThemeTypography value{};
    bool code_fixed_weight{};
};
struct TextSystem {
    NativeFontSession* native_fonts;
    // Owned by the initialized system. Render-command copies borrow this owner.
    NativeTextContext* native_text;
    float em_size;
    float tracking;
    bool fixed_weight;
    bool independent_letters;
    bool message_hit_policy; // Native geometry matching the existing raw message layout.
    // Format owned by native_text; required by every shaping call.
    u32 native_format;
    const TextSystemTypography* typography;
};
static_assert(sizeof(TextSystem) == 40);

inline float text_system_font_size(const TextSystem& system, u32 flags) noexcept {
    return system.typography ? ui_theme_text_size(system.typography->value, system.em_size, flags)
                             : text_style_font_size(system.em_size, flags);
}
// Spans with an explicit size become their folded shaping flags (size zero).
// Returns the spans to shape: `styles` itself when none is sized, else the
// folded copy in `scratch`; an empty view with `valid` false rejects an
// out-of-range size.
struct TextFoldedStyles {
    ConstSlice<TextStyleSpan> styles;
    bool valid;
};
TextFoldedStyles text_fold_style_sizes(ConstSlice<TextStyleSpan> styles,
                                       Slice<TextStyleSpan> scratch) noexcept;
inline bool text_system_fixed_weight(const TextSystem& system, u32 flags) noexcept {
    return (flags & text_style_monospace) && system.typography ? system.typography->code_fixed_weight
                                                               : system.fixed_weight;
}

static_assert(sizeof(TextRun) == 44);
static_assert(sizeof(ShapedText) == 104);
static_assert(sizeof(TextHitSpan) == 12);
static_assert(sizeof(TextHitRegion) == 20);

// Checks shaping input and reports the UTF-16 units it needs in `output`.
TextShapeResult validate_text_input(Utf8View input, Slice<wchar_t> output) noexcept;
// Converts byte-addressed hit spans into UTF-16 ranges; false for an invalid span.
bool resolve_text_hits(Utf8View input, ConstSlice<TextHitSpan> hits, TextHitRange* resolved) noexcept;
bool shaped_text_initialize(ShapedText* text, Slice<TextGlyph> glyphs, Slice<TextRun> runs,
                            Slice<u16> clusters) noexcept;
bool shaped_text_bind_decorations(ShapedText* text, Slice<TextDecoration> decorations) noexcept;
bool shaped_text_bind_hit_regions(ShapedText* text, Slice<TextHitRegion> hit_regions) noexcept;
void shaped_text_reset(ShapedText* text) noexcept;

// Creates a native font session, text context and the family's format. Only
// en-US is supported; other locales are rejected.
TextShapeResult text_system_initialize(TextSystem* system, const wchar_t* family_name, float em_size,
                                       const wchar_t* locale_name) noexcept;
void text_system_release(TextSystem* system) noexcept;

bool glyph_outline_initialize(GlyphOutline* outline, Slice<GlyphPathCommand> commands) noexcept;
void glyph_outline_reset(GlyphOutline* outline) noexcept;

// The text every shaper and hit-tester sees: the source with browser-invisible
// default-ignorables replaced by same-length invisible scalars (see
// text_hide_default_ignorables). The source itself is never modified.
class TextShapingView {
public:
    explicit TextShapingView(Utf8View input) noexcept;
    ~TextShapingView() noexcept;
    TextShapingView(const TextShapingView&) = delete;
    TextShapingView& operator=(const TextShapingView&) = delete;
    Utf8View text;

private:
    char* heap_{};
    char local_[512];
};

// Under the client presentation policy (text_browser_invisibles) the text
// system shapes a same-length substitute instead of failing an item it cannot
// express. Each stage includes the earlier ones: 0 base scalars that fail on
// their own become the missing glyph; 1 the item is bisected (at spaces, then
// grapheme clusters) and only the clusters that still fail become one missing
// glyph each; 2 paired brackets become non-pairing lookalikes (< > or
// U+2039 U+203A); 3 the smallest failing range whose halves shape alone is
// substituted whole; 4 every scalar beyond printable ASCII becomes the missing
// glyph. Offsets are unchanged. Hit testing and editing use the same stages so
// they agree with drawing.
class TextNativeFallback {
public:
    static constexpr u32 stages = 5;
    TextNativeFallback() noexcept = default;
    ~TextNativeFallback() noexcept;
    TextNativeFallback(const TextNativeFallback&) = delete;
    TextNativeFallback& operator=(const TextNativeFallback&) = delete;
    // False when the stage changes nothing, or the policy is off.
    bool build(const TextSystem& system, Utf8View input, u32 stage) noexcept;
    Utf8View text;

private:
    bool narrow(const TextSystem& system, char* out, u32 begin, u32 end, bool known_failing, bool unresolved) noexcept;
    // Probed base scalars (scalar << 1 | shapes), kept across stages.
    u32 scalars_[256]{};
    u32 scalar_probes_{};
    u32 chunk_probes_{};
    char* heap_{};
    u32 heap_bytes_{};
    char local_[512];
};

// Layout/shaping is a cache-build operation, never a paint-path operation.
// All retained glyph/run/cluster output is caller-owned and bounded.
TextShapeResult text_shape_utf8(TextSystem* system, Utf8View input, float maximum_width,
                                float maximum_height, Slice<wchar_t> utf16_scratch, ShapedText* output,
                                TextWrapping wrapping = TextWrapping::wrap,
                                TextOverflow overflow = TextOverflow::clip) noexcept;
TextShapeResult text_shape_utf8_styled(TextSystem* system, Utf8View input, ConstSlice<TextStyleSpan> styles,
                                       float maximum_width, float maximum_height,
                                       Slice<wchar_t> utf16_scratch, ShapedText* output,
                                       TextWrapping wrapping = TextWrapping::wrap,
                                       TextOverflow overflow = TextOverflow::clip) noexcept;
TextShapeResult text_shape_utf8_interactive(TextSystem* system, Utf8View input,
                                            ConstSlice<TextStyleSpan> styles, ConstSlice<TextHitSpan> hits,
                                            float maximum_width, float maximum_height,
                                            Slice<wchar_t> utf16_scratch, ShapedText* output,
                                            TextWrapping wrapping = TextWrapping::wrap,
                                            TextOverflow overflow = TextOverflow::clip) noexcept;
TextMeasureResult text_measure_utf8_styled(TextSystem* system, Utf8View input,
                                           ConstSlice<TextStyleSpan> styles, float maximum_width,
                                           float maximum_height, Slice<wchar_t> utf16_scratch,
                                           TextWrapping wrapping = TextWrapping::wrap) noexcept;
} // namespace maicord
