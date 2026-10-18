#pragma once
// Test/tool-only DirectWrite reference text system. The shipping text stack
// is native; this reference is compiled only into test and font-reference
// executables (which link dwrite.lib) and never into MaiCord.exe or
// MaiCordAccount.dll. Native output is compared against its DirectWrite layouts.
#include "maicord/ui/text.hpp"
#include "maicord/ui/native_text_candidate.hpp"
#include "maicord/render/text_edit_layout.hpp"
#include "maicord/text/font_prepare.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/sys/dwrite.hpp"

namespace maicord {

// A native text system plus the DirectWrite objects of the reference shaper.
// Copies borrow the owner's objects, like TextSystem command copies. Shaping
// overloads below dispatch on the members:
//   native_text set   -> the shipping native path; with factory and format also
//                        set, DirectWrite shapes what the native path rejects;
//   native_text null  -> DirectWrite only.
// While native_format is zero (or format was replaced after the last capture)
// it is derived from `format` on each call.
struct DWriteTextSystem : TextSystem {
    IDWriteFactory* factory{};
    IDWriteTextFormat* format{};
    IDWriteFontFallback* emoji_font_policy{};
    // Monospace spans; null selects Consolas from the system collection.
    const wchar_t* code_family{};
    IDWriteFontCollection* code_collection{};
    // The DirectWrite format native_format was captured from.
    IDWriteTextFormat* captured_format{};
};

// DirectWrite objects plus a native session/context; native_format is left
// for capture (native_text_format_capture).
TextShapeResult text_system_initialize(DWriteTextSystem* system, const wchar_t* family_name, float em_size,
                                       const wchar_t* locale_name) noexcept;
void text_system_release(DWriteTextSystem* system) noexcept;
// Exact DirectWrite object-creation phase of the initializer. Requires an
// empty owner; failure can leave partial objects for the caller to release.
HRESULT text_compatibility_objects_initialize(DWriteTextSystem*, const wchar_t*, float, const wchar_t*) noexcept;

TextShapeResult text_shape_utf8(DWriteTextSystem* system, Utf8View input, float maximum_width,
                                float maximum_height, Slice<wchar_t> utf16_scratch, ShapedText* output,
                                TextWrapping wrapping = TextWrapping::wrap,
                                TextOverflow overflow = TextOverflow::clip) noexcept;
TextShapeResult text_shape_utf8_styled(DWriteTextSystem* system, Utf8View input, ConstSlice<TextStyleSpan> styles,
                                       float maximum_width, float maximum_height,
                                       Slice<wchar_t> utf16_scratch, ShapedText* output,
                                       TextWrapping wrapping = TextWrapping::wrap,
                                       TextOverflow overflow = TextOverflow::clip) noexcept;
TextShapeResult text_shape_utf8_interactive(DWriteTextSystem* system, Utf8View input,
                                            ConstSlice<TextStyleSpan> styles, ConstSlice<TextHitSpan> hits,
                                            float maximum_width, float maximum_height,
                                            Slice<wchar_t> utf16_scratch, ShapedText* output,
                                            TextWrapping wrapping = TextWrapping::wrap,
                                            TextOverflow overflow = TextOverflow::clip) noexcept;
TextMeasureResult text_measure_utf8_styled(DWriteTextSystem* system, Utf8View input,
                                           ConstSlice<TextStyleSpan> styles, float maximum_width,
                                           float maximum_height, Slice<wchar_t> utf16_scratch,
                                           TextWrapping wrapping = TextWrapping::wrap) noexcept;

// The native system the shipping path sees for this reference system.
TextSystem native_view(const DWriteTextSystem& system) noexcept;
// Native entry points with the reference system's derived native format.
bool native_text_candidate_line(const DWriteTextSystem&, Utf8View, float maximum_width, float maximum_height,
                                ShapedText* output, TextMeasureResult* measure,
                                TextWrapping wrapping = TextWrapping::wrap, TextOverflow overflow = TextOverflow::clip) noexcept;
bool native_text_candidate_styled(const DWriteTextSystem&, Utf8View, ConstSlice<TextStyleSpan>, float width, float height,
                                  ShapedText*, TextMeasureResult*, TextWrapping, TextOverflow = TextOverflow::clip,
                                  ConstSlice<TextHitRange> hits = {}, NativeParagraphHitStorage* retained = nullptr) noexcept;
bool native_text_candidate_hits(const DWriteTextSystem&, Utf8View, float width, float height, TextWrapping,
                                NativeParagraphHitStorage, TextMeasureResult*) noexcept;
bool native_text_candidate_ellipsis(const DWriteTextSystem&, Utf8View, ConstSlice<TextStyleSpan>, float width, float height,
                                    ShapedText*, TextWrapping, ConstSlice<TextHitRange>) noexcept;
bool native_text_candidate_trimmed(const DWriteTextSystem&, Utf8View, ConstSlice<TextStyleSpan>, float width, float height,
                                   ShapedText*, TextWrapping, ConstSlice<TextHitRange> hits = {}) noexcept;
bool native_text_candidate_prefers_multiline(const DWriteTextSystem&, Utf8View, ConstSlice<TextStyleSpan>, TextWrapping) noexcept;
u64 native_text_format_identity(const DWriteTextSystem&) noexcept;
bool text_edit_layout_measure(TextEditLayoutOwner* owner, const TextEditLayoutInput& input,
                              const DWriteTextSystem& text_system) noexcept;

// A retained DirectWrite layout behind the same owner geometry. With a native
// reference system (native_text set) it measures natively first; queries on a
// native owner use the shipping geometry.
struct DWriteEditLayoutOwner : TextEditLayoutOwner {
    IDWriteTextLayout* layout{};
    wchar_t* utf16{};
    void* flag_cells{};
    // The retained layout is the fallback for an input the native editor declined.
    bool native_declined{};
};
bool text_edit_layout_measure(DWriteEditLayoutOwner* owner, const TextEditLayoutInput& input,
                              const DWriteTextSystem& text_system) noexcept;
bool text_edit_layout_hit_test(const DWriteEditLayoutOwner& owner, float x, float y, u32* utf16_position) noexcept;
bool text_edit_layout_point_geometry(const DWriteEditLayoutOwner&, float x, float y, bool round_nearest,
                                     u32* utf16_position, bool* inside) noexcept;
bool text_edit_layout_move_vertical(const DWriteEditLayoutOwner& owner, bool down, u32* utf16_position) noexcept;
bool text_edit_layout_range_geometry(const DWriteEditLayoutOwner& owner, u32 first, u32 last,
                                     Slice<TextEditMetric> output, u32* count) noexcept;
bool text_edit_layout_position_geometry(const DWriteEditLayoutOwner&, u32 position, TextEditMetric*) noexcept;
bool text_edit_layout_raw_range_geometry(const DWriteEditLayoutOwner&, u32 first, u32 last,
                                         Slice<TextEditMetric>, u32* count) noexcept;
u32 text_edit_layout_line_count(const DWriteEditLayoutOwner&) noexcept;
bool text_edit_layout_scroll(DWriteEditLayoutOwner* owner, float delta) noexcept;
void text_edit_layout_invalidate(DWriteEditLayoutOwner* owner) noexcept;
void text_edit_layout_release(DWriteEditLayoutOwner* owner) noexcept;

// Gives a layout one-em inline cells for custom-emoji regional flags; *storage
// (heap, released by regional_flag_layout_free) is reused across layouts.
HRESULT regional_flag_layout_attach(IDWriteTextLayout* layout, Utf8View text, ConstSlice<TextStyleSpan> styles,
                                    void** storage) noexcept;
void regional_flag_layout_free(void* storage) noexcept;

// Applies the shared font metrics/styles and emoji font ranges to a layout.
HRESULT text_apply_layout_styles(const DWriteTextSystem& system, IDWriteTextLayout* layout, Utf8View text,
                                 ConstSlice<TextStyleSpan> styles) noexcept;

// Derives native_format from `format` (family, collection scope, WWS, size,
// tab stop and wrapping); false for formats the native app policy rejects.
bool native_text_format_capture(DWriteTextSystem* system) noexcept;
// Declares that a private collection is exactly face zero at default axes of
// `path` and registers that path for native scopes. Repeating the same
// registration is harmless; another path for the same collection is rejected.
bool native_text_register_collection(NativeTextContext* context, IDWriteFontCollection* collection,
                                     const wchar_t* path) noexcept;

// Reference runs carry no native font; their face is kept here (borrowed,
// alive for the process). Null for native runs.
IDWriteFontFace* reference_run_face(const TextRun& run) noexcept;

enum class GlyphOutlineError : u8 {
    none,
    invalid_input,
    output_too_small,
    directwrite,
};
struct GlyphOutlineResult {
    HRESULT native_error;
    u32 command_count;
    GlyphOutlineError error;
    constexpr bool ok() const noexcept {
        return error == GlyphOutlineError::none;
    }
};
// Captures normalized (1 em) line/cubic contours from DirectWrite.
GlyphOutlineResult text_outline_glyph(IDWriteFontFace* font_face, u16 glyph_index, GlyphOutline* output) noexcept;

// Binds a DirectWrite-selected face to a native instance by its file path.
// Each optional style output permits its draw-time simulation. Optional source
// copies the actual fvar axes (up to two).
NativeFontResult native_font_bind_selected(NativeFontSession*, IDWriteFontFace*, bool outline_instructions,
                                           Arena* scratch, NativeFontInstanceHandle*, bool* oblique = nullptr,
                                           bool* bold = nullptr, NativeFontSelection* source = nullptr) noexcept;

} // namespace maicord
