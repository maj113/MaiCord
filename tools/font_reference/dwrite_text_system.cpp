// Test/tool-only DirectWrite reference text system; see dwrite_text_system.hpp.
// Never linked into MaiCord.exe.
#include "dwrite_text_system.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/text/font_session.hpp"
#include "maicord/ui/emoji_presentation.hpp"
#include "maicord/ui/ansi_text.hpp"
#include "maicord/ui/native_text_candidate.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/sys/com.hpp"
#include "maicord/sys/d2d1.hpp"
#include "maicord/sys/dwrite.hpp"
// Placement new without <new>: its STL CRT record would clash with the /MT
// probe executables that link this reference next to /MD test libraries.
#include "maicord/text/paragraph_hits.hpp"

namespace maicord {
namespace {
// One-em inline cells for custom-emoji regional flags: caller-owned COM metrics.
// A layout borrows these cells until its final Release; Release never deletes
// caller storage. Two alternating identities for each of eight typography sizes
// keep adjacent equal-em flags from coalescing into one DirectWrite property
// range, independent of emoji count.
class RegionalFlagInlineCell final : public IDWriteInlineObject {
public:
    float em = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) noexcept override;
    ULONG STDMETHODCALLTYPE AddRef() noexcept override;
    ULONG STDMETHODCALLTYPE Release() noexcept override;
    HRESULT STDMETHODCALLTYPE Draw(void*, IDWriteTextRenderer*, FLOAT, FLOAT, BOOL, BOOL, IUnknown*) noexcept override;
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS*) noexcept override;
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS*) noexcept override;
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION*, DWRITE_BREAK_CONDITION*) noexcept override;
private:
    volatile LONG references = 1;
};
struct RegionalFlagLayoutCells { RegionalFlagInlineCell cells[16]; u32 count = 0; u32 identity = 0; };
// Placement-construct after every layout borrowing the previous bank is released.
void regional_flag_layout_initialize(RegionalFlagLayoutCells*) noexcept;
bool regional_flag_layout_required(Utf8View, ConstSlice<TextStyleSpan>) noexcept;
// Actual UTF16 remains authoritative. Width/height one em, baseline 0.8 em;
// artwork is centered in this box by the existing media presentation pass.
HRESULT regional_flag_layout_apply(IDWriteTextLayout*, Utf8View,
    ConstSlice<TextStyleSpan>, RegionalFlagLayoutCells*) noexcept;

namespace flag_detail {
bool admitted(ConstSlice<TextStyleSpan> styles, RegionalFlagRange range, u32* index) noexcept {
    while (*index < styles.count && styles[*index].byte_offset + styles[*index].byte_count <= range.first_byte) ++*index;
    if (*index >= styles.count) return false;
    const auto& style = styles[*index];
    return style.byte_offset <= range.first_byte && style.byte_count >= range.end_byte - style.byte_offset &&
        (style.flags & text_style_custom_emoji) && !(style.flags & text_style_monospace);
}
bool valid(Utf8View text, ConstSlice<TextStyleSpan> styles) noexcept {
    if ((styles.count && !styles.data) || (text.bytes && !text.data) || styles.count > kTextStyleSpanMaximum) return false;
    u32 end = 0;
    for (u32 i = 0; i < styles.count; ++i) {
        const auto& style = styles[i];
        if (style.byte_offset < end || style.byte_offset > text.bytes || style.byte_count > text.bytes - style.byte_offset)
            return false;
        end = style.byte_offset + style.byte_count;
    }
    return true;
}
} // namespace flag_detail
using namespace flag_detail;
HRESULT STDMETHODCALLTYPE RegionalFlagInlineCell::QueryInterface(REFIID id, void** output) noexcept {
    if (!output) return E_POINTER; *output = nullptr;
    if (id != __uuidof(IUnknown) && id != __uuidof(IDWriteInlineObject)) return E_NOINTERFACE;
    *output = static_cast<IDWriteInlineObject*>(this); AddRef(); return S_OK;
}
ULONG STDMETHODCALLTYPE RegionalFlagInlineCell::AddRef() noexcept { return static_cast<ULONG>(InterlockedIncrement(&references)); }
ULONG STDMETHODCALLTYPE RegionalFlagInlineCell::Release() noexcept { return static_cast<ULONG>(InterlockedDecrement(&references)); }
HRESULT STDMETHODCALLTYPE RegionalFlagInlineCell::Draw(void*, IDWriteTextRenderer*, FLOAT, FLOAT, BOOL, BOOL, IUnknown*) noexcept { return S_OK; }
HRESULT STDMETHODCALLTYPE RegionalFlagInlineCell::GetMetrics(DWRITE_INLINE_OBJECT_METRICS* output) noexcept {
    if (!output) return E_POINTER; *output = {em, em, em * .8f, FALSE}; return S_OK;
}
HRESULT STDMETHODCALLTYPE RegionalFlagInlineCell::GetOverhangMetrics(DWRITE_OVERHANG_METRICS* output) noexcept {
    if (!output) return E_POINTER; *output = {}; return S_OK;
}
HRESULT STDMETHODCALLTYPE RegionalFlagInlineCell::GetBreakConditions(DWRITE_BREAK_CONDITION* before, DWRITE_BREAK_CONDITION* after) noexcept {
    if (!before || !after) return E_POINTER; *before = *after = DWRITE_BREAK_CONDITION_NEUTRAL; return S_OK;
}
void regional_flag_layout_initialize(RegionalFlagLayoutCells* cells) noexcept {
    if (cells) new (cells) RegionalFlagLayoutCells{};
}
bool regional_flag_layout_required(Utf8View text, ConstSlice<TextStyleSpan> styles) noexcept {
    if (!valid(text, styles)) return false;
    bool suppressed = false;
    for (u32 i = 0; i < styles.count; ++i) suppressed |= (styles[i].flags & text_style_custom_emoji) != 0;
    if (!suppressed) return false;
    u32 cursor = 0, style = 0; RegionalFlagRange range{};
    while (regional_flag_next(text, &cursor, &range)) if (admitted(styles, range, &style)) return true;
    return false;
}
HRESULT regional_flag_layout_apply(IDWriteTextLayout* layout, Utf8View text,
    ConstSlice<TextStyleSpan> styles, RegionalFlagLayoutCells* cells) noexcept {
    if (!layout || !valid(text, styles)) return E_INVALIDARG;
    bool suppressed = false;
    for (u32 i = 0; i < styles.count; ++i) suppressed |= (styles[i].flags & text_style_custom_emoji) != 0;
    if (!suppressed) return S_OK;
    u32 cursor = 0, style = 0, byte = 0, unit = 0; RegionalFlagRange range{};
    while (regional_flag_next(text, &cursor, &range)) {
        if (!admitted(styles, range, &style)) continue;
        if (!cells || cells->count > 8 || cells->identity > 1) return E_INVALIDARG;
        while (byte < range.first_byte) {
            const auto cp = utf8_decode_one(text.data + byte, text.bytes - byte);
            if (!cp.valid || cp.width > range.first_byte - byte) return E_INVALIDARG;
            byte += cp.width; unit += cp.codepoint > 0xffff ? 2u : 1u;
        }
        FLOAT em = 0; HRESULT result = layout->GetFontSize(unit, &em);
        if (FAILED(result) || !(em > 0) || em > 65536) return E_INVALIDARG;
        u32 cell = 0;
        while (cell < cells->count && cells->cells[cell * 2].em != em) ++cell;
        if (cell == cells->count) {
            if (cell == 8) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            cells->cells[cell * 2].em = cells->cells[cell * 2 + 1].em = em; ++cells->count;
        }
        const u32 units = (range.end_byte - range.first_byte) / 2;
        result = layout->SetInlineObject(&cells->cells[cell * 2 + cells->identity], {unit, units});
        if (FAILED(result)) return result;
        cells->identity ^= 1u;
        unit += units; byte = range.end_byte;
    }
    return S_OK;
}


// Reference run faces stay referenced for the process; runs keep a 1-based table index.
constexpr u32 kReferenceFaceCapacity = 1u << 14;
IDWriteFontFace* g_reference_faces[kReferenceFaceCapacity];
u32 g_reference_face_slots[kReferenceFaceCapacity * 2];
u32 g_reference_face_count;
NativeFontRunHandle reference_face_handle(IDWriteFontFace* face) noexcept {
    if (!face)
        return {};
    u32 slot = static_cast<u32>((reinterpret_cast<usize>(face) >> 4) * 2654435761u) & (kReferenceFaceCapacity * 2 - 1);
    for (;;) {
        const u32 entry = g_reference_face_slots[slot];
        if (!entry)
            break;
        if (g_reference_faces[entry - 1] == face)
            return {entry, 0};
        slot = (slot + 1) & (kReferenceFaceCapacity * 2 - 1);
    }
    if (g_reference_face_count == kReferenceFaceCapacity)
        return {};
    face->AddRef();
    g_reference_faces[g_reference_face_count++] = face;
    g_reference_face_slots[slot] = g_reference_face_count;
    return {g_reference_face_count, 0};
}

bool interface_equal(REFIID left, REFIID right) noexcept;
class EmojiFontPolicy final : public IDWriteFontFallback {
  public:
    LONG references = 1;
    IDWriteFontFallback* other_text = nullptr;
    IDWriteFont* emoji = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) noexcept override {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (!interface_equal(id, __uuidof(IUnknown)) && !interface_equal(id, __uuidof(IDWriteFontFallback)))
            return E_NOINTERFACE;
        *object = static_cast<IDWriteFontFallback*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return InterlockedIncrement(&references);
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const auto count = InterlockedDecrement(&references);
        if (!count) {
            com_release(other_text);
            com_release(emoji);
            this->~EmojiFontPolicy();
            HeapFree(process_heap(), 0, this);
        }
        return count;
    }
    HRESULT STDMETHODCALLTYPE MapCharacters(IDWriteTextAnalysisSource* source, UINT32 position,
                                            UINT32 length, IDWriteFontCollection* collection,
                                            const wchar_t* family, DWRITE_FONT_WEIGHT weight,
                                            DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch,
                                            UINT32* mapped_length, IDWriteFont** mapped_font,
                                            FLOAT* scale) noexcept override {
        constexpr wchar_t selected[] = L"Segoe UI Emoji";
        u32 i = 0;
        if (family)
            while (selected[i] && family[i] == selected[i])
                ++i;
        if (family && !selected[i] && !family[i]) {
            // An unsupported emoji stays in the requested font; no monochrome
            // Symbol glyph or other set replaces it.
            if (!mapped_length || !mapped_font || !scale)
                return E_POINTER;
            *mapped_length = length;
            *mapped_font = emoji;
            *scale = 1;
            emoji->AddRef();
            return S_OK;
        }
        return other_text->MapCharacters(source, position, length, collection, family, weight, style,
                                         stretch, mapped_length, mapped_font, scale);
    }
};

HRESULT create_emoji_policy(IDWriteFactory* factory, IDWriteFontFallback** output) noexcept {
    auto* memory = HeapAlloc(process_heap(), 0, sizeof(EmojiFontPolicy));
    if (!memory)
        return E_OUTOFMEMORY;
    auto* policy = new (memory) EmojiFontPolicy{};
    IDWriteFactory2* factory2 = nullptr;
    IDWriteFontCollection* fonts = nullptr;
    IDWriteFontFamily* family = nullptr;
    UINT32 index = 0;
    BOOL exists = FALSE;
    HRESULT result =
        factory->QueryInterface(__uuidof(IDWriteFactory2), reinterpret_cast<void**>(&factory2));
    if (SUCCEEDED(result))
        result = factory2->GetSystemFontFallback(&policy->other_text);
    if (SUCCEEDED(result))
        result = factory->GetSystemFontCollection(&fonts);
    if (SUCCEEDED(result))
        result = fonts->FindFamilyName(L"Segoe UI Emoji", &index, &exists);
    if (SUCCEEDED(result) && !exists)
        result = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (SUCCEEDED(result))
        result = fonts->GetFontFamily(index, &family);
    if (SUCCEEDED(result))
        result = family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              DWRITE_FONT_STYLE_NORMAL, &policy->emoji);
    com_release(family);
    com_release(fonts);
    com_release(factory2);
    if (SUCCEEDED(result))
        *output = policy;
    else
        policy->Release();
    return result;
}

TextShapeResult shape_result(TextShapeError error, HRESULT native_error = S_OK, u32 utf16_units = 0,
                             const ShapedText* output = nullptr) noexcept {
    return TextShapeResult{native_error, utf16_units, output ? output->glyph_count : 0,
                           output ? output->run_count : 0, error};
}

bool layout_extent_valid(float value) noexcept {
    return value == value && value > 0.0f && value <= 1'048'576.0f;
}

bool interface_equal(REFIID left, REFIID right) noexcept {
    const auto* left_bytes = reinterpret_cast<const volatile u8*>(&left);
    const auto* right_bytes = reinterpret_cast<const volatile u8*>(&right);
    bool equal = true;
    for (u32 index = 0; index < sizeof(IID); ++index) {
        equal = equal && left_bytes[index] == right_bytes[index];
    }
    return equal;
}

void convert_utf16(Utf8View input, wchar_t* output) noexcept {
    u32 written = 0;
    u32 offset = 0;
    while (offset < input.bytes) {
        const Utf8DecodeResult decoded = utf8_decode_one(input.data + offset, input.bytes - offset);
        if (decoded.codepoint <= 0xFFFF) {
            output[written++] = static_cast<wchar_t>(decoded.codepoint);
        } else {
            const u32 value = decoded.codepoint - 0x10000;
            output[written++] = static_cast<wchar_t>(0xD800 + (value >> 10));
            output[written++] = static_cast<wchar_t>(0xDC00 + (value & 0x3FF));
        }
        offset += decoded.width;
    }
}

struct ResolvedTextStyle {
    DWRITE_TEXT_RANGE range;
    u32 flags;
    u16 ansi_background;
};

class TextDrawingEffect final : public IUnknown {
  public:
    TextDrawingEffect() noexcept : references_(1), flags_(0) {
    }

    void set_flags(u32 flags) noexcept {
        flags_ = flags;
    }

    u32 flags() const noexcept {
        return flags_;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interface_id, void** object) noexcept override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interface_equal(interface_id, __uuidof(IUnknown))) {
            *object = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        return static_cast<ULONG>(InterlockedDecrement(&references_));
    }

  private:
    volatile LONG references_;
    u32 flags_;
};

// Per-request drawing effects. Only the styled prefix is constructed (in
// apply_text_styles), not all kTextStyleSpanMaximum entries.
struct TextDrawingEffects {
    alignas(TextDrawingEffect) unsigned char storage[sizeof(TextDrawingEffect) * kTextStyleSpanMaximum];
    TextDrawingEffect* data() noexcept { return reinterpret_cast<TextDrawingEffect*>(storage); }
};

u32 drawing_effect_flags(IUnknown* effect) noexcept {
    return effect ? static_cast<TextDrawingEffect*>(effect)->flags() : 0;
}

class CaptureRenderer final : public IDWriteTextRenderer {
  public:
    explicit CaptureRenderer(ShapedText* output) noexcept
        : references_(1), output_(output), overflowed_(false), drawing_inline_(false) {
    }

    bool overflowed() const noexcept {
        return overflowed_;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interface_id, void** object) noexcept override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interface_equal(interface_id, __uuidof(IUnknown)) ||
            interface_equal(interface_id, __uuidof(IDWritePixelSnapping)) ||
            interface_equal(interface_id, __uuidof(IDWriteTextRenderer))) {
            *object = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        return static_cast<ULONG>(InterlockedDecrement(&references_));
    }

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) noexcept override {
        if (!disabled) {
            return E_POINTER;
        }
        *disabled = TRUE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) noexcept override {
        if (!transform) {
            return E_POINTER;
        }
        *transform = DWRITE_MATRIX{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels_per_dip) noexcept override {
        if (!pixels_per_dip) {
            return E_POINTER;
        }
        *pixels_per_dip = 1.0f;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT baseline_x, FLOAT baseline_y,
                                           DWRITE_MEASURING_MODE measuring_mode,
                                           const DWRITE_GLYPH_RUN* glyph_run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION* description,
                                           IUnknown* drawing_effect) noexcept override {
        if (!output_ || !glyph_run || !glyph_run->fontFace ||
            (glyph_run->glyphCount && (!glyph_run->glyphIndices ||
             (description && description->stringLength != 0 && !description->clusterMap)))) {
            return E_INVALIDARG;
        }
        if (description &&
            (description->textPosition > output_->cluster_count ||
             description->stringLength > output_->cluster_count - description->textPosition)) {
            return reject_overflow();
        }
        if (glyph_run->glyphCount > output_->glyph_capacity - output_->glyph_count) {
            return reject_overflow();
        }
        if (output_->run_count >= output_->run_capacity) {
            return reject_overflow();
        }
        if (glyph_run->bidiLevel > 0xFF) {
            return reject_overflow();
        }

        const u32 first_glyph = output_->glyph_count;
        for (u32 index = 0; index < glyph_run->glyphCount; ++index) {
            const DWRITE_GLYPH_OFFSET offset =
                glyph_run->glyphOffsets ? glyph_run->glyphOffsets[index] : DWRITE_GLYPH_OFFSET{0.0f, 0.0f};
            output_->glyphs[first_glyph + index] = TextGlyph{
                glyph_run->glyphAdvances ? glyph_run->glyphAdvances[index] : 0.0f,
                offset.advanceOffset,
                offset.ascenderOffset,
                glyph_run->glyphIndices[index],
                0,
            };
        }
        // Runs containing only controls can have no glyphs while their source
        // cluster map still counts multiple clusters (CRLF and emoji tags).
        // Keep their source entries at the nondrawing sentinel, without trying
        // to index a nonexistent glyph array.
        if (description && glyph_run->glyphCount) {
            for (u32 index = 0; index < description->stringLength; ++index) {
                const u32 glyph = static_cast<u32>(description->clusterMap[index]);
                // DirectWrite maps formatting controls such as a hard line break to
                // the one-past-the-end glyph. They have no drawable
                // glyph, so retain the initialized 0xFFFF cluster sentinel.
                if (glyph == glyph_run->glyphCount) {
                    continue;
                }
                if (glyph > glyph_run->glyphCount) {
                    return reject_overflow();
                }
                if (first_glyph > 0xFFFFu - glyph) {
                    return reject_overflow();
                }
                output_->clusters[description->textPosition + index] =
                    static_cast<u16>(first_glyph + glyph);
            }
        }

        (void)measuring_mode;
        const NativeFontRunHandle face = reference_face_handle(glyph_run->fontFace);
        if (!face.slot) {
            return reject_overflow();
        }
        TextRun& run = output_->runs[output_->run_count++];
        run = TextRun{face,
                      baseline_x,
                      baseline_y,
                      glyph_run->fontEmSize,
                      first_glyph,
                      glyph_run->glyphCount,
                      description ? description->textPosition : 0,
                      description ? description->stringLength : 0,
                      drawing_effect_flags(drawing_effect),
                      static_cast<u8>(glyph_run->bidiLevel),
                      glyph_run->isSideways != FALSE,
                      false};
        output_->glyph_count += glyph_run->glyphCount;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT baseline_x, FLOAT baseline_y,
                                            const DWRITE_UNDERLINE* underline,
                                            IUnknown* drawing_effect) noexcept override {
        if (!underline || !(underline->width >= 0.0f) || !(underline->thickness > 0.0f)) {
            return E_INVALIDARG;
        }
        // The renderer skips zero-width quads; do not retain an inert underline
        // or consume a decoration slot for it.
        if (underline->width == 0.0f) return S_OK;
        return append_decoration(
            TextDecoration{baseline_x, baseline_y + underline->offset, baseline_x + underline->width,
                           baseline_y + underline->offset + underline->thickness,
                           drawing_effect_flags(drawing_effect), TextDecorationKind::underline, 0});
    }

    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT baseline_x, FLOAT baseline_y,
                                                const DWRITE_STRIKETHROUGH* strikethrough,
                                                IUnknown* drawing_effect) noexcept override {
        if (!strikethrough || !(strikethrough->width >= 0.0f) || !(strikethrough->thickness > 0.0f)) {
            return E_INVALIDARG;
        }
        if (strikethrough->width == 0.0f) return S_OK;
        return append_decoration(TextDecoration{
            baseline_x, baseline_y + strikethrough->offset, baseline_x + strikethrough->width,
            baseline_y + strikethrough->offset + strikethrough->thickness,
            drawing_effect_flags(drawing_effect), TextDecorationKind::strikethrough, 0});
    }

    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* context, FLOAT origin_x, FLOAT origin_y,
                                               IDWriteInlineObject* object, BOOL sideways,
                                               BOOL right_to_left,
                                               IUnknown* drawing_effect) noexcept override {
        if (!output_ || !object || drawing_inline_) {
            return object ? E_FAIL : E_INVALIDARG;
        }
        drawing_inline_ = true;
        const HRESULT result =
            object->Draw(context, this, origin_x, origin_y, sideways, right_to_left, drawing_effect);
        drawing_inline_ = false;
        return result;
    }

  private:
    HRESULT append_decoration(const TextDecoration& decoration) noexcept {
        if (!output_ || !output_->decorations ||
            output_->decoration_count >= output_->decoration_capacity) {
            return reject_overflow();
        }
        output_->decorations[output_->decoration_count++] = decoration;
        return S_OK;
    }

    HRESULT reject_overflow() noexcept {
        overflowed_ = true;
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    volatile LONG references_;
    ShapedText* output_;
    bool overflowed_;
    bool drawing_inline_;
};

class GlyphOutlineSink final : public ID2D1SimplifiedGeometrySink {
  public:
    explicit GlyphOutlineSink(GlyphOutline* output) noexcept
        : references_(1), output_(output), figure_open_(false), overflowed_(false),
          invalid_sequence_(false) {
    }

    bool overflowed() const noexcept {
        return overflowed_;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interface_id, void** object) noexcept override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interface_equal(interface_id, __uuidof(IUnknown)) ||
            interface_equal(interface_id, __uuidof(ID2D1SimplifiedGeometrySink))) {
            *object = static_cast<ID2D1SimplifiedGeometrySink*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        return static_cast<ULONG>(InterlockedDecrement(&references_));
    }

    void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE fill_mode) noexcept override {
        if (!output_) {
            invalid_sequence_ = true;
            return;
        }
        output_->fill_rule =
            fill_mode == D2D1_FILL_MODE_WINDING ? GlyphFillRule::winding : GlyphFillRule::alternate;
    }

    void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) noexcept override {
    }

    void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F point, D2D1_FIGURE_BEGIN begin) noexcept override {
        if (figure_open_) {
            invalid_sequence_ = true;
            return;
        }
        figure_open_ = true;
        const GlyphPathCommandKind kind = begin == D2D1_FIGURE_BEGIN_HOLLOW
                                              ? GlyphPathCommandKind::begin_hollow
                                              : GlyphPathCommandKind::begin_filled;
        append(GlyphPathCommand{point.x, point.y, 0.0f, 0.0f, 0.0f, 0.0f, kind, {0, 0, 0}}, 1);
    }

    void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* points, UINT32 count) noexcept override {
        if (!figure_open_ || (!points && count != 0)) {
            invalid_sequence_ = true;
            return;
        }
        for (u32 index = 0; index < count; ++index) {
            append(GlyphPathCommand{points[index].x,
                                    points[index].y,
                                    0.0f,
                                    0.0f,
                                    0.0f,
                                    0.0f,
                                    GlyphPathCommandKind::line,
                                    {0, 0, 0}},
                   1);
        }
    }

    void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* beziers, UINT32 count) noexcept override {
        if (!figure_open_ || (!beziers && count != 0)) {
            invalid_sequence_ = true;
            return;
        }
        for (u32 index = 0; index < count; ++index) {
            const D2D1_BEZIER_SEGMENT& curve = beziers[index];
            append(GlyphPathCommand{curve.point1.x,
                                    curve.point1.y,
                                    curve.point2.x,
                                    curve.point2.y,
                                    curve.point3.x,
                                    curve.point3.y,
                                    GlyphPathCommandKind::cubic,
                                    {0, 0, 0}},
                   3);
        }
    }

    void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END end) noexcept override {
        if (!figure_open_) {
            invalid_sequence_ = true;
            return;
        }
        const GlyphPathCommandKind kind = end == D2D1_FIGURE_END_CLOSED ? GlyphPathCommandKind::end_closed
                                                                        : GlyphPathCommandKind::end_open;
        append(GlyphPathCommand{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, kind, {0, 0, 0}}, 0);
        figure_open_ = false;
    }

    HRESULT STDMETHODCALLTYPE Close() noexcept override {
        if (overflowed_) {
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }
        return figure_open_ || invalid_sequence_ ? E_UNEXPECTED : S_OK;
    }

  private:
    void include_point(float x, float y) noexcept {
        if (!output_->has_bounds) {
            output_->minimum_x = x;
            output_->maximum_x = x;
            output_->minimum_y = y;
            output_->maximum_y = y;
            output_->has_bounds = true;
            return;
        }
        output_->minimum_x = minimum(output_->minimum_x, x);
        output_->minimum_y = minimum(output_->minimum_y, y);
        output_->maximum_x = maximum(output_->maximum_x, x);
        output_->maximum_y = maximum(output_->maximum_y, y);
    }

    void append(const GlyphPathCommand& command, u32 point_count) noexcept {
        if (!output_ || overflowed_) {
            return;
        }
        if (output_->count >= output_->capacity) {
            overflowed_ = true;
            ++output_->rejected;
            return;
        }
        output_->commands[output_->count++] = command;
        if (point_count >= 1) {
            include_point(command.x1, command.y1);
        }
        if (point_count >= 2) {
            include_point(command.x2, command.y2);
        }
        if (point_count >= 3) {
            include_point(command.x3, command.y3);
        }
    }

    volatile LONG references_;
    GlyphOutline* output_;
    bool figure_open_;
    bool overflowed_;
    bool invalid_sequence_;
};

bool resolve_text_styles(Utf8View input, ConstSlice<TextStyleSpan> styles,
                         ResolvedTextStyle* resolved) noexcept {
    if (styles.count > kTextStyleSpanMaximum || (!styles.data && styles.count != 0) ||
        (!resolved && styles.count != 0)) {
        return false;
    }
    u32 byte_offset = 0;
    u32 utf16_offset = 0;
    u32 previous_end = 0;
    for (u32 index = 0; index < styles.count; ++index) {
        const TextStyleSpan& style = styles[index];
        // Shapers take folded spans (text_fold_style_sizes): sizes are flags here.
        if (style.flags == 0 || style.size != 0 || style.byte_count == 0 || style.byte_offset < previous_end ||
            style.byte_offset > input.bytes || style.byte_count > input.bytes - style.byte_offset) {
            return false;
        }
        while (byte_offset < style.byte_offset) {
            const Utf8DecodeResult decoded =
                utf8_decode_one(input.data + byte_offset, input.bytes - byte_offset);
            if (!decoded.valid || decoded.width == 0 || decoded.width > style.byte_offset - byte_offset) {
                return false;
            }
            byte_offset += decoded.width;
            utf16_offset += decoded.codepoint <= 0xFFFF ? 1u : 2u;
        }
        if (byte_offset != style.byte_offset) {
            return false;
        }
        const u32 range_begin = utf16_offset;
        const u32 byte_end = style.byte_offset + style.byte_count;
        while (byte_offset < byte_end) {
            const Utf8DecodeResult decoded =
                utf8_decode_one(input.data + byte_offset, input.bytes - byte_offset);
            if (!decoded.valid || decoded.width == 0 || decoded.width > byte_end - byte_offset) {
                return false;
            }
            byte_offset += decoded.width;
            utf16_offset += decoded.codepoint <= 0xFFFF ? 1u : 2u;
        }
        if (byte_offset != byte_end || utf16_offset == range_begin) {
            return false;
        }
        resolved[index] =
            ResolvedTextStyle{DWRITE_TEXT_RANGE{range_begin, utf16_offset - range_begin}, style.flags,
                              ansi_span_background(style)};
        previous_end = byte_end;
    }
    return true;
}

float styled_font_size(const TextSystem& system, u32 flags) noexcept {
    return text_system_font_size(system, flags);
}

HRESULT apply_font_styles(const DWriteTextSystem* system, IDWriteTextLayout* layout,
                          ConstSlice<ResolvedTextStyle> styles) noexcept {
    HRESULT result = S_OK;
    for (u32 index = 0; index < styles.count && SUCCEEDED(result); ++index) {
        const ResolvedTextStyle& style = styles[index];
        if (SUCCEEDED(result) && (style.flags & text_style_bold) != 0 &&
            !text_system_fixed_weight(*system, style.flags)) {
            result = layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, style.range);
        }
        if (SUCCEEDED(result) && (style.flags & text_style_italic) != 0) {
            result = layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, style.range);
        }
        if (SUCCEEDED(result) && (style.flags & text_style_underline) != 0) {
            result = layout->SetUnderline(TRUE, style.range);
        }
        if (SUCCEEDED(result) && (style.flags & text_style_strikethrough) != 0) {
            result = layout->SetStrikethrough(TRUE, style.range);
        }
        if (SUCCEEDED(result) && (style.flags & text_style_monospace) != 0) {
            result = layout->SetFontFamilyName(system->code_family ? system->code_family : L"Consolas", style.range);
            if (SUCCEEDED(result) && system->code_collection)
                result = layout->SetFontCollection(system->code_collection, style.range);
        }
        const float font_size = styled_font_size(*system, style.flags);
        if (SUCCEEDED(result) && font_size != system->em_size) {
            result = layout->SetFontSize(font_size, style.range);
        }
    }
    return result;
}

// Explicit range selection precedes shaping, measurement and editor hit testing.
// Keep selectors, skin tones, tags and ZWJ components in the same font range.
HRESULT apply_emoji_font(const DWriteTextSystem& system, IDWriteTextLayout* layout, Utf8View text) noexcept {
    IDWriteFontCollection* collection = nullptr;
    IDWriteTypography* typography = nullptr;
    IDWriteTextLayout2* emoji_layout = nullptr;
    HRESULT result = S_OK;
    TextEmojiCursor cursor{};
    while (SUCCEEDED(result)) {
        TextEmojiRange selected{};
        const auto next = text_emoji_next(text, &cursor, &selected);
        if (next == TextEmojiNext::end) break;
        if (next == TextEmojiNext::invalid_input) { result = E_INVALIDARG; break; }
        {
            if (!collection) {
                result = layout->QueryInterface(__uuidof(IDWriteTextLayout2),
                                                reinterpret_cast<void**>(&emoji_layout));
                if (SUCCEEDED(result))
                    result = emoji_layout->SetFontFallback(system.emoji_font_policy);
                if (SUCCEEDED(result))
                    result = system.factory->GetSystemFontCollection(&collection);
            }
            const DWRITE_TEXT_RANGE range{selected.first_unit, selected.end_unit - selected.first_unit};
            if (SUCCEEDED(result))
                result = layout->SetFontCollection(collection, range);
            if (SUCCEEDED(result))
                result = layout->SetFontFamilyName(L"Segoe UI Emoji", range);
            if (SUCCEEDED(result))
                result = layout->SetFontWeight(DWRITE_FONT_WEIGHT_NORMAL, range);
            if (SUCCEEDED(result))
                result = layout->SetFontStyle(DWRITE_FONT_STYLE_NORMAL, range);
            if (SUCCEEDED(result) && system.independent_letters) {
                if (!typography) result = system.factory->CreateTypography(&typography);
                if (SUCCEEDED(result)) result = layout->SetTypography(typography, range);
            }
            if (SUCCEEDED(result) && system.tracking)
                result = emoji_layout->SetCharacterSpacing(0, 0, 0, range);
        }
    }
    com_release(collection);
    com_release(typography);
    com_release(emoji_layout);
    return result;
}

HRESULT apply_text_styles(DWriteTextSystem* system, IDWriteTextLayout* layout,
                          ConstSlice<ResolvedTextStyle> styles, TextDrawingEffect* effects) noexcept {
    HRESULT result = apply_font_styles(system, layout, styles);
    for (u32 i = 0; i < styles.count && SUCCEEDED(result); ++i) {
        new (&effects[i]) TextDrawingEffect{};
        effects[i].set_flags(styles[i].flags);
        result = layout->SetDrawingEffect(&effects[i], styles[i].range);
    }
    return result;
}

bool append_layout_decoration(ShapedText* output, const TextDecoration& decoration) noexcept {
    if (!output->decorations || output->decoration_count >= output->decoration_capacity) {
        return false;
    }
    output->decorations[output->decoration_count++] = decoration;
    return true;
}

HRESULT capture_code_block_decorations(IDWriteTextLayout* layout, ConstSlice<ResolvedTextStyle> styles,
                                       ShapedText* output) noexcept {
    if (!layout || !output) {
        return E_INVALIDARG;
    }
    DWRITE_HIT_TEST_METRICS metrics[256]{};
    for (u32 index = 0; index < styles.count;) {
        const ResolvedTextStyle& first = styles[index];
        if ((first.flags & text_style_code_block) == 0) {
            ++index;
            continue;
        }
        u32 block_end = first.range.startPosition + first.range.length;
        u32 next = index + 1;
        while (next < styles.count && (styles[next].flags & text_style_code_block) != 0 &&
               styles[next].range.startPosition == block_end) {
            block_end += styles[next].range.length;
            ++next;
        }

        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        bool bounded = false;
        bool multiline = false;
        float first_line_top = 0.0f;
        UINT32 actual = 0;
        const HRESULT tested = layout->HitTestTextRange(
            first.range.startPosition, block_end - first.range.startPosition, 0.0f, 0.0f, metrics,
            static_cast<UINT32>(sizeof(metrics) / sizeof(metrics[0])), &actual);
        if (SUCCEEDED(tested)) {
            for (u32 item = 0; item < actual; ++item) {
                const DWRITE_HIT_TEST_METRICS& metric = metrics[item];
                if (!(metric.height > 0.0f)) {
                    continue;
                }
                const float metric_right = metric.left + metric.width;
                const float metric_bottom = metric.top + metric.height;
                if (!bounded) {
                    left = metric.left;
                    top = metric.top;
                    right = metric_right;
                    bottom = metric_bottom;
                    first_line_top = metric.top;
                    bounded = true;
                } else {
                    multiline = multiline || metric.top != first_line_top;
                    left = minimum(left, metric.left);
                    top = minimum(top, metric.top);
                    right = maximum(right, metric_right);
                    bottom = maximum(bottom, metric_bottom);
                }
            }
            if (bounded && multiline) {
                // A fenced-code block is block-level: wrapped or explicit
                // subsequent lines share one full-width surface.
                left = 0.0f;
                right = layout->GetMaxWidth();
            }
        } else if (tested != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
            return tested;
        }

        // Very long fenced blocks can have more line fragments than the bounded
        // scratch buffer. The first/last-position fallback keeps shaping
        // allocation-free and never rejects the retained frame.
        if (!bounded) {
            FLOAT first_x = 0.0f;
            FLOAT first_y = 0.0f;
            FLOAT last_x = 0.0f;
            FLOAT last_y = 0.0f;
            DWRITE_HIT_TEST_METRICS first_metric{};
            DWRITE_HIT_TEST_METRICS last_metric{};
            HRESULT position_result = layout->HitTestTextPosition(first.range.startPosition, FALSE,
                                                                  &first_x, &first_y, &first_metric);
            if (SUCCEEDED(position_result)) {
                position_result =
                    layout->HitTestTextPosition(block_end - 1, TRUE, &last_x, &last_y, &last_metric);
            }
            if (FAILED(position_result)) {
                return position_result;
            }
            left = minimum(first_metric.left, last_metric.left);
            top = minimum(first_metric.top, last_metric.top);
            right = maximum(first_metric.left + first_metric.width, last_metric.left + last_metric.width);
            bottom = maximum(first_metric.top + first_metric.height, last_metric.top + last_metric.height);
            if (last_metric.top > first_metric.top) {
                left = 0.0f;
                right = layout->GetMaxWidth();
            }
            bounded = bottom > top;
        }
        if (bounded &&
            !append_layout_decoration(output, TextDecoration{maximum(0.0f, left - 4.0f), top - 2.0f,
                                                             minimum(layout->GetMaxWidth(), right + 4.0f),
                                                             bottom + 2.0f,
                                                             text_style_monospace | text_style_code_block,
                                                             TextDecorationKind::background, 0})) {
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }
        index = next;
    }
    return S_OK;
}

// Quote divider: one 4-DIP spine spanning the whole quote,
// in the 16-DIP indent (spine + 12-DIP padding) left of the quoted text.
inline constexpr float kQuoteSpineLeft = -16.0f;
inline constexpr float kQuoteSpineWidth = 4.0f;

HRESULT capture_quote_decorations(IDWriteTextLayout* layout, ConstSlice<ResolvedTextStyle> styles,
                                  ShapedText* output) noexcept {
    DWRITE_LINE_METRICS lines[256];
    UINT32 line_count = 0;
    bool measured = false;
    for (u32 index = 0; index < styles.count;) {
        const ResolvedTextStyle& first = styles[index];
        if ((first.flags & text_style_quote) == 0) {
            ++index;
            continue;
        }
        u32 block_end = first.range.startPosition + first.range.length;
        u32 next = index + 1;
        while (next < styles.count && (styles[next].flags & text_style_quote) != 0 &&
               styles[next].range.startPosition == block_end) {
            block_end += styles[next].range.length;
            ++next;
        }
        if (!measured) {
            const HRESULT result =
                layout->GetLineMetrics(lines, static_cast<UINT32>(sizeof(lines) / sizeof(lines[0])), &line_count);
            if (FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER))
                return result;
            if (FAILED(result))
                line_count = 0; // Beyond the bounded scratch the spine spans the layout.
            measured = true;
        }
        // The spine covers the line boxes of the quoted text. Trimmed lines
        // report no height; text there belongs to the last visible line.
        float top = 0.0f, bottom = 0.0f, line_top = 0.0f, visible_top = 0.0f, visible_bottom = 0.0f;
        bool started = false, visible = false;
        u32 position = 0;
        for (u32 line = 0; line < line_count; ++line) {
            const u32 line_end = position + lines[line].length;
            const float line_bottom = line_top + lines[line].height;
            if (lines[line].height > 0.0f) {
                visible_top = line_top;
                visible_bottom = line_bottom;
                visible = true;
            }
            if (first.range.startPosition < line_end && visible) {
                if (!started)
                    top = visible_top;
                started = true;
                bottom = visible_bottom;
            }
            if (block_end <= line_end)
                break;
            position = line_end;
            line_top = line_bottom;
        }
        if (!line_count) {
            DWRITE_TEXT_METRICS metrics{};
            const HRESULT result = layout->GetMetrics(&metrics);
            if (FAILED(result))
                return result;
            bottom = metrics.top + metrics.height;
            top = metrics.top;
        }
        if (bottom > top &&
            !append_layout_decoration(output, TextDecoration{kQuoteSpineLeft, top, kQuoteSpineLeft + kQuoteSpineWidth,
                                                             bottom, first.flags, TextDecorationKind::quote_bar, 0}))
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        index = next;
    }
    return S_OK;
}

HRESULT capture_range_decorations(IDWriteTextLayout* layout, ConstSlice<ResolvedTextStyle> styles,
                                  ConstSlice<wchar_t> text, ShapedText* output) noexcept {
    HRESULT result = capture_code_block_decorations(layout, styles, output);
    if (SUCCEEDED(result))
        result = capture_quote_decorations(layout, styles, output);
    if (FAILED(result)) {
        return result;
    }
    DWRITE_HIT_TEST_METRICS metrics[256]{};
    for (u32 index = 0; index < styles.count; ++index) {
        const ResolvedTextStyle& style = styles[index];
        if (style.ansi_background != kAnsiColorNone) {
            // A ```ansi background covers each line fragment's line box.
            UINT32 fragments = 0;
            const HRESULT hit =
                layout->HitTestTextRange(style.range.startPosition, style.range.length, 0.0f, 0.0f, metrics,
                                         static_cast<UINT32>(sizeof(metrics) / sizeof(metrics[0])), &fragments);
            if (FAILED(hit) || fragments > sizeof(metrics) / sizeof(metrics[0])) {
                return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            }
            for (u32 item = 0; item < fragments; ++item) {
                const DWRITE_HIT_TEST_METRICS& metric = metrics[item];
                if (metric.width > 0.0f && metric.height > 0.0f &&
                    !append_layout_decoration(output, ansi_background_decoration(
                                                          metric.left, metric.top, metric.left + metric.width,
                                                          metric.top + metric.height, style.flags,
                                                          style.ansi_background))) {
                    return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
                }
            }
        }
        const u32 background_flags = text_style_monospace | text_style_spoiler | text_style_mention;
        if ((style.flags & (background_flags | text_style_list_bullet)) == 0) {
            continue;
        }
        // Code-block backgrounds are already captured with the first/last-position
        // fallback above; repeat the full range query only for a bullet.
        if ((style.flags & text_style_code_block) != 0 && (style.flags & text_style_list_bullet) == 0) {
            continue;
        }
        UINT32 actual = 0;
        const HRESULT hit =
            layout->HitTestTextRange(style.range.startPosition, style.range.length, 0.0f, 0.0f, metrics,
                                     static_cast<UINT32>(sizeof(metrics) / sizeof(metrics[0])), &actual);
        if (FAILED(hit) || actual > sizeof(metrics) / sizeof(metrics[0])) {
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }
        const bool begins_line =
            style.range.startPosition == 0 || (style.range.startPosition <= text.count && text.data &&
                                               text.data[style.range.startPosition - 1] == L'\n');
        bool list_bullet_emitted = false;
        for (u32 item = 0; item < actual; ++item) {
            const DWRITE_HIT_TEST_METRICS& metric = metrics[item];
            if (!(metric.height > 0.0f)) {
                continue;
            }
            if (begins_line && !list_bullet_emitted && metric.width > 0.0f &&
                (style.flags & text_style_list_bullet) != 0) {
                const u32 depth = (style.flags & kTextStyleListDepthMask) >> kTextStyleListDepthShift;
                const float center_x = metric.left + 5.0f + static_cast<float>(depth) * 8.0f;
                const float center_y = metric.top + metric.height * 0.5f;
                if (!append_layout_decoration(output,
                                              TextDecoration{center_x - 2.0f, center_y - 2.0f,
                                                             center_x + 2.0f, center_y + 2.0f, style.flags,
                                                             TextDecorationKind::list_bullet, 0})) {
                    return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
                }
                list_bullet_emitted = true;
            }
            if ((style.flags & text_style_code_block) == 0 && (style.flags & background_flags) != 0 &&
                metric.width > 0.0f) {
                constexpr float horizontal_padding = 2.0f;
                constexpr float vertical_padding = 1.0f;
                if (!append_layout_decoration(
                        output,
                        TextDecoration{metric.left - horizontal_padding, metric.top - vertical_padding,
                                       metric.left + metric.width + horizontal_padding,
                                       metric.top + metric.height + vertical_padding, style.flags,
                                       TextDecorationKind::background, 0})) {
                    return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
                }
            }
        }
    }
    return S_OK;
}

HRESULT capture_hit_regions(IDWriteTextLayout* layout, ConstSlice<TextHitRange> hits,
                            ShapedText* output) noexcept {
    if (hits.count != 0 && (!output->hit_regions || output->hit_region_capacity == 0)) {
        return E_INVALIDARG;
    }
    DWRITE_HIT_TEST_METRICS metrics[256]{};
    for (u32 index = 0; index < hits.count; ++index) {
        const TextHitRange& hit = hits[index];
        UINT32 actual = 0;
        const HRESULT tested =
            layout->HitTestTextRange(hit.first_unit, hit.unit_count, 0.0f, 0.0f, metrics,
                                     static_cast<UINT32>(sizeof(metrics) / sizeof(metrics[0])), &actual);
        if (FAILED(tested) || actual > sizeof(metrics) / sizeof(metrics[0])) {
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }
        for (u32 item = 0; item < actual; ++item) {
            const DWRITE_HIT_TEST_METRICS& metric = metrics[item];
            if (!(metric.width > 0.0f) || !(metric.height > 0.0f)) {
                continue;
            }
            if (output->hit_region_count >= output->hit_region_capacity) {
                return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            }
            output->hit_regions[output->hit_region_count++] = TextHitRegion{
                metric.left,        metric.top, metric.left + metric.width, metric.top + metric.height,
                hit.semantic_index, 0};
        }
    }
    return S_OK;
}


HRESULT configure_text_layout(DWriteTextSystem* system, IDWriteTextLayout* layout, TextWrapping wrapping,
                              TextOverflow overflow) noexcept {
    if (!system || !system->factory || !layout || wrapping > TextWrapping::no_wrap ||
        overflow > TextOverflow::ellipsis) {
        return E_INVALIDARG;
    }
    HRESULT result = S_OK;
    if (wrapping == TextWrapping::no_wrap) {
        result = layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (SUCCEEDED(result) && overflow == TextOverflow::ellipsis) {
        IDWriteInlineObject* trimming_sign = nullptr;
        result = system->factory->CreateEllipsisTrimmingSign(layout, &trimming_sign);
        if (SUCCEEDED(result)) {
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            result = layout->SetTrimming(&trimming, trimming_sign);
        }
        com_release(trimming_sign);
    }
    return result;
}

namespace {
HRESULT apply_tracking(const DWriteTextSystem* system, IDWriteTextLayout* layout, u32 units) noexcept {
    if (!units)
        return S_OK;
    if (system->independent_letters) {
        IDWriteTypography* typography = nullptr;
        HRESULT result = system->factory->CreateTypography(&typography);
        if (SUCCEEDED(result))
            result = typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES, 0});
        if (SUCCEEDED(result))
            result = typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_LIGATURES, 0});
        if (SUCCEEDED(result))
            result = typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_KERNING, 0});
        if (SUCCEEDED(result))
            result = layout->SetTypography(typography, {0, units});
        if (typography)
            typography->Release();
        if (FAILED(result))
            return result;
    }
    if (!system->tracking)
        return S_OK;
    IDWriteTextLayout1* spacing = nullptr;
    HRESULT result =
        layout->QueryInterface(__uuidof(IDWriteTextLayout1), reinterpret_cast<void**>(&spacing));
    float size = system->em_size;
    if (SUCCEEDED(result))
        result = layout->GetFontSize(0, &size);
    if (SUCCEEDED(result))
        result = spacing->SetCharacterSpacing(0, size * system->tracking, 0, {0, units});
    if (spacing)
        spacing->Release();
    return result;
}
} // namespace

TextShapeResult shape_dwrite(
    DWriteTextSystem* system, Utf8View input, ConstSlice<TextStyleSpan> styles,
    ConstSlice<TextHitRange> resolved_hits, float maximum_width, float maximum_height,
    Slice<wchar_t> utf16_scratch, u32 utf16_units, ShapedText* output,
    TextWrapping wrapping, TextOverflow overflow) noexcept {
    ResolvedTextStyle resolved[kTextStyleSpanMaximum];
    if (!resolve_text_styles(input, styles, resolved)) {
        shaped_text_reset(output);
        return shape_result(TextShapeError::invalid_input, E_INVALIDARG, utf16_units);
    }

    IDWriteTextLayout* layout = nullptr;
    CaptureRenderer renderer(output);
    TextDrawingEffects effects;
    RegionalFlagLayoutCells flag_cells{};
    convert_utf16(input, utf16_scratch.data);
    HRESULT result = system->factory->CreateTextLayout(utf16_scratch.data, utf16_units, system->format,
                                                       maximum_width, maximum_height, &layout);
    if (SUCCEEDED(result)) {
        result = configure_text_layout(system, layout, wrapping, overflow);
    }
    if (SUCCEEDED(result) && styles.count != 0) {
        result = apply_text_styles(system, layout, ConstSlice<ResolvedTextStyle>{resolved, styles.count},
                                   effects.data());
    }
    if (SUCCEEDED(result))
        result = apply_tracking(system, layout, utf16_units);
    if (SUCCEEDED(result))
        result = apply_emoji_font(*system, layout, input);
    if (SUCCEEDED(result))
        result = regional_flag_layout_apply(layout, input, styles, &flag_cells);
    if (SUCCEEDED(result)) {
        result = layout->Draw(nullptr, &renderer, 0.0f, 0.0f);
    }
    if (renderer.overflowed()) {
        result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    if (SUCCEEDED(result)) {
        DWRITE_TEXT_METRICS metrics{};
        result = layout->GetMetrics(&metrics);
        if (SUCCEEDED(result)) {
            output->width = metrics.width;
            output->width_including_trailing_whitespace = metrics.widthIncludingTrailingWhitespace;
            output->height = metrics.height;
            output->line_count = metrics.lineCount;
        }
    }
    if (SUCCEEDED(result) && styles.count != 0) {
        result = capture_range_decorations(layout, ConstSlice<ResolvedTextStyle>{resolved, styles.count},
                                           ConstSlice<wchar_t>{utf16_scratch.data, utf16_units}, output);
    }
    if (SUCCEEDED(result) && resolved_hits.count != 0) {
        result =
            capture_hit_regions(layout, resolved_hits, output);
    }
    com_release(layout);
    if (FAILED(result)) {
        const TextShapeError error =
            renderer.overflowed() || result == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)
                ? TextShapeError::output_too_small
                : TextShapeError::unsupported;
        shaped_text_reset(output);
        return shape_result(error, result, utf16_units);
    }
    return shape_result(TextShapeError::none, S_OK, utf16_units, output);
}

TextMeasureResult measure_result(TextShapeError error, HRESULT native_error, u32 utf16_units = 0,
                                 const DWRITE_TEXT_METRICS* metrics = nullptr) noexcept {
    return TextMeasureResult{native_error,
                             metrics ? metrics->width : 0.0f,
                             metrics ? metrics->widthIncludingTrailingWhitespace : 0.0f,
                             metrics ? metrics->height : 0.0f,
                             utf16_units,
                             metrics ? metrics->lineCount : 0,
                             error};
}

TextMeasureResult measure_dwrite(
    DWriteTextSystem* system, Utf8View input, ConstSlice<TextStyleSpan> styles,
    float maximum_width, float maximum_height, Slice<wchar_t> utf16_scratch,
    u32 utf16_units, TextWrapping wrapping) noexcept {
    ResolvedTextStyle resolved[kTextStyleSpanMaximum];
    if (!resolve_text_styles(input, styles, resolved)) {
        return measure_result(TextShapeError::invalid_input, E_INVALIDARG, utf16_units);
    }
    IDWriteTextLayout* layout = nullptr;
    TextDrawingEffects effects;
    RegionalFlagLayoutCells flag_cells{};
    convert_utf16(input, utf16_scratch.data);
    HRESULT result = system->factory->CreateTextLayout(utf16_scratch.data, utf16_units, system->format,
                                                       maximum_width, maximum_height, &layout);
    if (SUCCEEDED(result) && wrapping == TextWrapping::no_wrap) {
        result = layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (SUCCEEDED(result) && styles.count != 0) {
        result = apply_text_styles(system, layout, ConstSlice<ResolvedTextStyle>{resolved, styles.count},
                                   effects.data());
    }
    if (SUCCEEDED(result))
        result = apply_tracking(system, layout, utf16_units);
    if (SUCCEEDED(result))
        result = apply_emoji_font(*system, layout, input);
    if (SUCCEEDED(result))
        result = regional_flag_layout_apply(layout, input, styles, &flag_cells);
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(result)) {
        result = layout->GetMetrics(&metrics);
    }
    com_release(layout);
    return SUCCEEDED(result) ? measure_result(TextShapeError::none, S_OK, utf16_units, &metrics)
                             : measure_result(TextShapeError::unsupported, result, utf16_units);
}

// Native format capture.
IUnknown* identity(IUnknown* object) noexcept {
    IUnknown* token = nullptr;
    if (object)
        object->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&token));
    return token;
}
struct RegisteredCollection {
    NativeTextContext* context;
    IUnknown* identity;
    wchar_t* path;
};
RegisteredCollection g_collections[256];
u32 g_collection_count;
bool same_path(const wchar_t* a, const wchar_t* b) noexcept {
    u32 i = 0;
    while (a[i] && a[i] == b[i])
        ++i;
    return a[i] == b[i];
}
bool collection_scope(const DWriteTextSystem& system, IDWriteFontCollection* collection, u32* scope) noexcept {
    IUnknown* token = identity(collection);
    if (!token)
        return false;
    struct Release {IUnknown*& token;~Release() noexcept {com_release(token);}} release{token};
    IDWriteFontCollection* system_fonts = nullptr;
    if (system.factory && SUCCEEDED(system.factory->GetSystemFontCollection(&system_fonts, FALSE))) {
        IUnknown* system_token = identity(system_fonts);
        const bool is_system = system_token == token;
        com_release(system_token);
        com_release(system_fonts);
        if (is_system) {
            *scope = 0;
            return true;
        }
    }
    for (u32 i = 0; i < g_collection_count; ++i)
        if (g_collections[i].context == system.native_text && g_collections[i].identity == token)
            return native_text_register_font_path(system.native_text, g_collections[i].path, scope);
    return false;
}
bool derive_format(const DWriteTextSystem& system, IDWriteTextFormat* format, u32* handle) noexcept {
    *handle = 0;
    if (!format || !system.native_text)
        return false;
    if (format->GetReadingDirection() != DWRITE_READING_DIRECTION_LEFT_TO_RIGHT ||
        format->GetFlowDirection() != DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM ||
        format->GetTextAlignment() != DWRITE_TEXT_ALIGNMENT_LEADING ||
        format->GetParagraphAlignment() != DWRITE_PARAGRAPH_ALIGNMENT_NEAR)
        return false;
    wchar_t locale[6]{};
    if (format->GetLocaleNameLength() != 5 || FAILED(format->GetLocaleName(locale, 6)))
        return false;
    constexpr wchar_t expected[] = L"en-us";
    for (u32 i = 0; i < 5; ++i)
        if ((locale[i] | 32) != expected[i])
            return false;
    DWRITE_LINE_SPACING_METHOD spacing{};
    float height = 0, baseline = 0;
    if (FAILED(format->GetLineSpacing(&spacing, &height, &baseline)) || spacing != DWRITE_LINE_SPACING_METHOD_DEFAULT)
        return false;
    DWRITE_TRIMMING trimming{};
    IDWriteInlineObject* sign = nullptr;
    const HRESULT trimmed = format->GetTrimming(&trimming, &sign);
    com_release(sign);
    if (FAILED(trimmed) || trimming.granularity != DWRITE_TRIMMING_GRANULARITY_NONE)
        return false;
    u16 family[256];
    const u32 length = format->GetFontFamilyNameLength();
    if (!length || length >= 256 || FAILED(format->GetFontFamilyName(reinterpret_cast<wchar_t*>(family), 256)))
        return false;
    IDWriteFontCollection* collection = nullptr;
    u32 scope = 0;
    const bool scoped = SUCCEEDED(format->GetFontCollection(&collection)) && collection_scope(system, collection, &scope);
    com_release(collection);
    if (!scoped)
        return false;
    return native_text_format_create(system.native_text,
                                     {{family, length},
                                      scope,
                                      {static_cast<u16>(format->GetFontWeight()), static_cast<u8>(format->GetFontStretch()),
                                       static_cast<u8>(format->GetFontStyle())},
                                      format->GetFontSize(),
                                      format->GetIncrementalTabStop(),
                                      format->GetWordWrapping() == DWRITE_WORD_WRAPPING_WRAP},
                                     handle);
}
bool reference_usable(const DWriteTextSystem* system) noexcept {
    return system && system->factory && system->format && system->emoji_font_policy;
}
TextShapeResult reference_shape(DWriteTextSystem* system, Utf8View source, ConstSlice<TextStyleSpan> sized_styles,
                                ConstSlice<TextHitSpan> hits, float maximum_width, float maximum_height,
                                Slice<wchar_t> utf16_scratch, ShapedText* output, TextWrapping wrapping,
                                TextOverflow overflow) noexcept {
    const TextShapingView shaping(source);
    const Utf8View input = shaping.text;
    TextStyleSpan folded[kTextStyleSpanMaximum];
    const auto folding = text_fold_style_sizes(sized_styles, {folded, kTextStyleSpanMaximum});
    if (!folding.valid)
        return shape_result(TextShapeError::invalid_input, E_INVALIDARG);
    const ConstSlice<TextStyleSpan> styles = folding.styles;
    if (!reference_usable(system) || !output || !output->glyphs || !output->runs || !output->clusters ||
        !layout_extent_valid(maximum_width) || !layout_extent_valid(maximum_height) ||
        (!styles.data && styles.count != 0) || styles.count > kTextStyleSpanMaximum ||
        (!hits.data && hits.count != 0) || hits.count > kTextHitSpanMaximum || wrapping > TextWrapping::no_wrap ||
        overflow > TextOverflow::ellipsis)
        return shape_result(TextShapeError::invalid_input, E_INVALIDARG);
    shaped_text_reset(output);
    const TextShapeResult validated = validate_text_input(input, utf16_scratch);
    if (!validated.ok())
        return validated;
    const u32 utf16_units = validated.utf16_units;
    if (utf16_units > output->cluster_capacity)
        return shape_result(TextShapeError::output_too_small, HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
                            utf16_units);
    output->cluster_count = utf16_units;
    for (u32 index = 0; index < utf16_units; ++index)
        output->clusters[index] = 0xFFFF;
    if (utf16_units == 0)
        return shape_result(TextShapeError::none);
    TextHitRange resolved_hits[kTextHitSpanMaximum];
    if (!resolve_text_hits(input, hits, resolved_hits)) {
        shaped_text_reset(output);
        return shape_result(TextShapeError::invalid_input, E_INVALIDARG, utf16_units);
    }
    return shape_dwrite(system, input, styles, {resolved_hits, hits.count}, maximum_width, maximum_height,
                        utf16_scratch, utf16_units, output, wrapping, overflow);
}
TextMeasureResult reference_measure(DWriteTextSystem* system, Utf8View source, ConstSlice<TextStyleSpan> sized_styles,
                                    float maximum_width, float maximum_height, Slice<wchar_t> utf16_scratch,
                                    TextWrapping wrapping) noexcept {
    const TextShapingView shaping(source);
    const Utf8View input = shaping.text;
    TextStyleSpan folded[kTextStyleSpanMaximum];
    const auto folding = text_fold_style_sizes(sized_styles, {folded, kTextStyleSpanMaximum});
    if (!folding.valid)
        return measure_result(TextShapeError::invalid_input, E_INVALIDARG);
    const ConstSlice<TextStyleSpan> styles = folding.styles;
    if (!reference_usable(system) || !layout_extent_valid(maximum_width) || !layout_extent_valid(maximum_height) ||
        (!styles.data && styles.count != 0) || styles.count > kTextStyleSpanMaximum || wrapping > TextWrapping::no_wrap)
        return measure_result(TextShapeError::invalid_input, E_INVALIDARG);
    const TextShapeResult validated = validate_text_input(input, utf16_scratch);
    if (!validated.ok())
        return measure_result(validated.error, validated.native_error, validated.utf16_units);
    if (validated.utf16_units == 0)
        return measure_result(TextShapeError::none, S_OK);
    return measure_dwrite(system, input, styles, maximum_width, maximum_height, utf16_scratch,
                          validated.utf16_units, wrapping);
}
} // namespace

HRESULT text_compatibility_objects_initialize(DWriteTextSystem* system,const wchar_t* family_name,float em_size,const wchar_t* locale_name) noexcept {
    HRESULT result = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(&system->factory));
    if (SUCCEEDED(result)) {
        result = system->factory->CreateTextFormat(family_name, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                   em_size, locale_name, &system->format);
    }
    if (SUCCEEDED(result)) {
        result = system->format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    if (SUCCEEDED(result))
        result = create_emoji_policy(system->factory, &system->emoji_font_policy);
    return result;
}

TextShapeResult text_system_initialize(DWriteTextSystem* system, const wchar_t* family_name, float em_size) noexcept {
    if (!system || !family_name || !family_name[0] || !layout_extent_valid(em_size))
        return shape_result(TextShapeError::invalid_input, E_INVALIDARG);
    *system = DWriteTextSystem{};
    HRESULT result = text_compatibility_objects_initialize(system, family_name, em_size, L"en-us");
    if (SUCCEEDED(result) && !native_font_session_create(&system->native_fonts))
        result = E_OUTOFMEMORY;
    if (SUCCEEDED(result) && !native_text_context_initialize(&system->native_text, system->native_fonts))
        result = E_OUTOFMEMORY;
    if (FAILED(result)) {
        text_system_release(system);
        return shape_result(TextShapeError::unsupported, result);
    }
    system->em_size = em_size;
    return shape_result(TextShapeError::none);
}

void text_system_release(DWriteTextSystem* system) noexcept {
    if (!system)
        return;
    com_release(system->format);
    com_release(system->emoji_font_policy);
    com_release(system->factory);
    system->captured_format = nullptr;
    text_system_release(static_cast<TextSystem*>(system));
}

TextShapeResult text_shape_utf8(DWriteTextSystem* system, Utf8View input, ConstSlice<TextStyleSpan> styles,
                                float maximum_width, float maximum_height, Slice<wchar_t> utf16_scratch,
                                ShapedText* output, TextWrapping wrapping, TextOverflow overflow,
                                ConstSlice<TextHitSpan> hits) noexcept {
    if (system && system->native_text) {
        TextSystem native = native_view(*system);
        const auto result = text_shape_utf8(&native, input, styles, maximum_width, maximum_height, utf16_scratch,
                                            output, wrapping, overflow, hits);
        if (result.ok() || !system->factory || !system->format)
            return result;
    }
    return reference_shape(system, input, styles, hits, maximum_width, maximum_height, utf16_scratch, output,
                           wrapping, overflow);
}
TextMeasureResult text_measure_utf8(DWriteTextSystem* system, Utf8View input, ConstSlice<TextStyleSpan> styles,
                                    float maximum_width, float maximum_height, Slice<wchar_t> utf16_scratch,
                                    TextWrapping wrapping) noexcept {
    if (system && system->native_text) {
        TextSystem native = native_view(*system);
        const auto result = text_measure_utf8(&native, input, styles, maximum_width, maximum_height,
                                              utf16_scratch, wrapping);
        if (result.ok() || !system->factory || !system->format)
            return result;
    }
    return reference_measure(system, input, styles, maximum_width, maximum_height, utf16_scratch, wrapping);
}

HRESULT text_apply_layout_styles(const DWriteTextSystem& system, IDWriteTextLayout* layout, Utf8View text,
                                 ConstSlice<TextStyleSpan> sized_styles) noexcept {
    ResolvedTextStyle resolved[kTextStyleSpanMaximum];
    TextStyleSpan folded[kTextStyleSpanMaximum];
    const auto folding = text_fold_style_sizes(sized_styles, {folded, kTextStyleSpanMaximum});
    const ConstSlice<TextStyleSpan> styles = folding.styles;
    if (!layout || !folding.valid || !resolve_text_styles(text, styles, resolved))
        return E_INVALIDARG;
    const HRESULT result = apply_font_styles(&system, layout, {resolved, styles.count});
    return SUCCEEDED(result) ? apply_emoji_font(system, layout, text) : result;
}

TextSystem native_view(const DWriteTextSystem& system) noexcept {
    TextSystem view = system;
    if (system.format && (!system.native_format || system.format != system.captured_format)) {
        u32 handle = 0;
        view.native_format = derive_format(system, system.format, &handle) ? handle : 0;
    }
    return view;
}
bool native_text_candidate_line(const DWriteTextSystem& system, Utf8View text, float maximum_width, float maximum_height,
                                ShapedText* output, TextMeasureResult* measure, TextWrapping wrapping,
                                TextOverflow overflow) noexcept {
    return native_text_candidate_line(native_view(system), text, maximum_width, maximum_height, output, measure,
                                      wrapping, overflow);
}
bool native_text_candidate_styled(const DWriteTextSystem& system, Utf8View text, ConstSlice<TextStyleSpan> styles,
                                  float width, float height, ShapedText* output, TextMeasureResult* measure,
                                  TextWrapping wrapping, TextOverflow overflow, ConstSlice<TextHitRange> hits,
                                  NativeParagraphHitStorage* retained) noexcept {
    return native_text_candidate_styled(native_view(system), text, styles, width, height, output, measure, wrapping,
                                        overflow, hits, retained);
}
bool native_text_candidate_hits(const DWriteTextSystem& system, Utf8View text, float width, float height,
                                TextWrapping wrapping, NativeParagraphHitStorage storage, TextMeasureResult* measure) noexcept {
    return native_text_candidate_hits(native_view(system), text, width, height, wrapping, storage, measure);
}
u64 native_text_format_identity(const DWriteTextSystem& system) noexcept {
    return native_text_format_identity(native_view(system));
}

bool native_text_format_capture(DWriteTextSystem* system) noexcept {
    if (!system)
        return false;
    system->native_format = 0;
    system->captured_format = nullptr;
    u32 handle = 0;
    if (!derive_format(*system, system->format, &handle))
        return false;
    system->native_format = handle;
    system->captured_format = system->format;
    return true;
}

bool native_text_register_collection(NativeTextContext* context, IDWriteFontCollection* collection,
                                     const wchar_t* path) noexcept {
    if (!context || !path)
        return false;
    u32 units = 0;
    while (units < 32768 && path[units])
        ++units;
    if (!units || units == 32768)
        return false;
    IUnknown* token = identity(collection);
    if (!token)
        return false;
    for (u32 i = 0; i < g_collection_count; ++i)
        if (g_collections[i].context == context && g_collections[i].identity == token) {
            token->Release();
            return same_path(g_collections[i].path, path);
        }
    u32 scope = 0;
    auto* saved = static_cast<wchar_t*>(HeapAlloc(process_heap(), 0, (units + 1) * sizeof(wchar_t)));
    if (g_collection_count == 256 || !saved || !native_text_register_font_path(context, path, &scope)) {
        if (saved)
            HeapFree(process_heap(), 0, saved);
        token->Release();
        return false;
    }
    copy_bytes(saved, path, (units + 1) * sizeof(wchar_t));
    g_collections[g_collection_count++] = {context, token, saved};
    return true;
}

IDWriteFontFace* reference_run_face(const TextRun& run) noexcept {
    return !run.native_font && run.native_face.slot && run.native_face.slot <= g_reference_face_count
               ? g_reference_faces[run.native_face.slot - 1]
               : nullptr;
}

GlyphOutlineResult text_outline_glyph(IDWriteFontFace* font_face, u16 glyph_index,
                                      GlyphOutline* output) noexcept {
    if (!font_face || !output || !output->commands || output->capacity == 0) {
        return GlyphOutlineResult{E_INVALIDARG, 0, GlyphOutlineError::invalid_input};
    }
    glyph_outline_reset(output);
    GlyphOutlineSink sink(output);
    const HRESULT outlined =
        font_face->GetGlyphRunOutline(1.0f, &glyph_index, nullptr, nullptr, 1, FALSE, FALSE, &sink);
    const HRESULT closed = sink.Close();
    const HRESULT result = FAILED(outlined) ? outlined : closed;
    if (FAILED(result)) {
        const GlyphOutlineError error =
            sink.overflowed() || result == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)
                ? GlyphOutlineError::output_too_small
                : GlyphOutlineError::directwrite;
        output->count = 0;
        output->has_bounds = false;
        return GlyphOutlineResult{result, 0, error};
    }
    return GlyphOutlineResult{S_OK, output->count, GlyphOutlineError::none};
}
namespace binding_detail {
template<class T> Slice<T> take(Arena& arena,u32 count) noexcept {return {count?arena_allocate<T>(&arena,count):nullptr,count};}
NativeFontResult failure(SfntStatus status) noexcept {return {NativeFontError::font,status,0};}
NativeFontResult windows_error(HRESULT hr) noexcept {return {NativeFontError::io,SfntStatus::ok,static_cast<u32>(hr)};}
} // namespace binding_detail
using namespace binding_detail;
NativeFontResult native_font_bind_selected(NativeFontSession* session,IDWriteFontFace* selected,bool outline_instructions,Arena* scratch,NativeFontInstanceHandle* output,bool* oblique,bool* bold,NativeFontSelection* source) noexcept {
    if(source)*source={};
    if(oblique)*oblique=false;if(bold)*bold=false;if(output)*output={};if(!session||!selected||!scratch||!scratch->base||!output)return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    const auto simulations=selected->GetSimulations();
    if(static_cast<u32>(simulations)>3||((simulations&DWRITE_FONT_SIMULATIONS_OBLIQUE)&&!oblique)||((simulations&DWRITE_FONT_SIMULATIONS_BOLD)&&!bold))return failure(SfntStatus::unsupported);
    u32 count=0;HRESULT hr=selected->GetFiles(&count,nullptr);if(FAILED(hr))return windows_error(hr);if(count!=1)return failure(SfntStatus::unsupported);
    IDWriteFontFile* file=nullptr;IDWriteFontFileLoader* loader=nullptr;IDWriteLocalFontFileLoader* local=nullptr;IDWriteFontFace5* variable=nullptr;
    struct Release {IDWriteFontFile*& f;IDWriteFontFileLoader*& l;IDWriteLocalFontFileLoader*& p;IDWriteFontFace5*& v;~Release() noexcept {com_release(v);com_release(p);com_release(l);com_release(f);}} release{file,loader,local,variable};
    hr=selected->GetFiles(&count,&file);const void* key=nullptr;u32 key_size=0,length=0;
    if(SUCCEEDED(hr))hr=file->GetReferenceKey(&key,&key_size);
    if(SUCCEEDED(hr))hr=file->GetLoader(&loader);
    if(SUCCEEDED(hr))hr=loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),reinterpret_cast<void**>(&local));
    if(SUCCEEDED(hr))hr=local->GetFilePathLengthFromKey(key,key_size,&length);
    if(FAILED(hr))return windows_error(hr);if(!length||length>32767)return failure(SfntStatus::unsupported);
    arena_reset(scratch);auto path=take<wchar_t>(*scratch,length+1);if(!path.data)return failure(SfntStatus::output_too_small);
    hr=local->GetFilePathFromKey(key,key_size,path.data,path.count);if(FAILED(hr))return windows_error(hr);
    NativeFontHandle face{};const auto acquired=native_font_session_face(session,path.data,selected->GetIndex(),&face);if(!acquired.ok())return acquired;
    auto& fonts=*native_font_session_fonts(session);
    struct ReleaseFace {NativeFontStore& s;NativeFontHandle h;~ReleaseFace() noexcept {native_font_release(&s,h);}} release_face{fonts,face};
    const auto& font=native_font_get(fonts,face)->geometry;ConstSlice<FontAxisSetting> settings{};
    if((simulations&DWRITE_FONT_SIMULATIONS_BOLD)&&(font.kind!=FontGeometryKind::truetype||font.gvar.table.data))return failure(SfntStatus::unsupported);
    if(font.axes.axis_count){
        hr=selected->QueryInterface(__uuidof(IDWriteFontFace5),reinterpret_cast<void**>(&variable));if(FAILED(hr))return windows_error(hr);
        const u32 axes=variable->GetFontAxisValueCount();if(axes>64)return failure(SfntStatus::unsupported);
        auto values=take<DWRITE_FONT_AXIS_VALUE>(*scratch,axes);auto native=take<FontAxisSetting>(*scratch,axes);
        if(!values.data||!native.data)return failure(SfntStatus::output_too_small);hr=variable->GetFontAxisValues(values.data,axes);if(FAILED(hr))return windows_error(hr);
        for(u32 i=0;i<axes;++i){const u32 t=values[i].axisTag;native[i]={((t&255)<<24)|((t&0xff00)<<8)|((t>>8)&0xff00)|(t>>24),values[i].value};}settings={native.data,axes};
    }
    const auto result=native_font_prepare(session,face,settings,static_cast<u32>(simulations),outline_instructions,scratch,output,oblique,bold);
    if(result.ok()&&source&&font.axes.axis_count<=2){NativeFontSelection saved{};saved.face=face;saved.count=font.axes.axis_count;
        for(u32 i=0;i<saved.count;++i){FontAxis axis{};font_axis(font.axes,i,&axis);saved.settings[i]={axis.tag,axis.normal/65536.0};
            for(u32 j=0;j<settings.count;++j)if(settings[j].tag==axis.tag)saved.settings[i]=settings[j];}*source=saved;}
    return result;
}

HRESULT regional_flag_layout_attach(IDWriteTextLayout* layout, Utf8View text, ConstSlice<TextStyleSpan> styles,
                                    void** storage) noexcept {
    if (!regional_flag_layout_required(text, styles))
        return S_OK;
    if (!*storage)
        *storage = HeapAlloc(process_heap(), 0, sizeof(RegionalFlagLayoutCells));
    auto* cells = static_cast<RegionalFlagLayoutCells*>(*storage);
    if (!cells)
        return E_OUTOFMEMORY;
    regional_flag_layout_initialize(cells);
    return regional_flag_layout_apply(layout, text, styles, cells);
}
void regional_flag_layout_free(void* storage) noexcept {
    if (storage)
        HeapFree(process_heap(), 0, storage);
}

} // namespace maicord
