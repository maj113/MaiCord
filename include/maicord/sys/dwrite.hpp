#pragma once

// DirectWrite, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/d2d1.hpp"
#include "maicord/sys/dxgi.hpp"
#include "maicord/sys/gdi.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <dwrite_3.h>
#endif

// <DWrite_2.h>
#define DWRITE_NO_PALETTE_INDEX 0xFFFF

MAICORD_SYS_TYPES_BEGIN
struct DWRITE_CLUSTER_METRICS;
struct DWRITE_COLOR_GLYPH_RUN1;
struct DWRITE_COLOR_GLYPH_RUN;
struct DWRITE_FONT_AXIS_RANGE;
struct DWRITE_FONT_AXIS_VALUE;
struct DWRITE_FONT_FEATURE;
struct DWRITE_FONT_METRICS1;
struct DWRITE_FONT_METRICS;
struct DWRITE_FONT_PROPERTY;
struct DWRITE_GLYPH_METRICS;
struct DWRITE_GLYPH_OFFSET;
struct DWRITE_GLYPH_RUN;
struct DWRITE_GLYPH_RUN_DESCRIPTION;
struct DWRITE_HIT_TEST_METRICS;
struct DWRITE_INLINE_OBJECT_METRICS;
struct DWRITE_LINE_BREAKPOINT;
struct DWRITE_LINE_METRICS;
struct DWRITE_LINE_SPACING;
struct DWRITE_MATRIX;
struct DWRITE_OVERHANG_METRICS;
struct DWRITE_PAINT_COLOR;
struct DWRITE_PAINT_ELEMENT;
struct DWRITE_SCRIPT_ANALYSIS;
struct DWRITE_SHAPING_GLYPH_PROPERTIES;
struct DWRITE_STRIKETHROUGH;
struct DWRITE_TEXT_METRICS1;
struct DWRITE_TEXT_METRICS;
struct DWRITE_TEXT_RANGE;
struct DWRITE_TRIMMING;
struct DWRITE_UNDERLINE;
struct IDWriteColorGlyphRunEnumerator1;
struct IDWriteColorGlyphRunEnumerator;
struct IDWriteFactory1;
struct IDWriteFactory2;
struct IDWriteFactory3;
struct IDWriteFactory4;
struct IDWriteFactory5;
struct IDWriteFactory6;
struct IDWriteFactory;
struct IDWriteFont1;
struct IDWriteFont2;
struct IDWriteFont3;
struct IDWriteFont;
struct IDWriteFontCollection1;
struct IDWriteFontCollection2;
struct IDWriteFontCollection;
struct IDWriteFontFace1;
struct IDWriteFontFace2;
struct IDWriteFontFace3;
struct IDWriteFontFace4;
struct IDWriteFontFace5;
struct IDWriteFontFace6;
struct IDWriteFontFace7;
struct IDWriteFontFace;
struct IDWriteFontFaceReference1;
struct IDWriteFontFaceReference;
struct IDWriteFontFallback;
struct IDWriteFontFamily1;
struct IDWriteFontFamily2;
struct IDWriteFontFamily;
struct IDWriteFontFile;
struct IDWriteFontFileLoader;
struct IDWriteFontList;
struct IDWriteFontResource;
struct IDWriteFontSet1;
struct IDWriteFontSet;
struct IDWriteFontSetBuilder1;
struct IDWriteFontSetBuilder2;
struct IDWriteFontSetBuilder;
struct IDWriteGlyphRunAnalysis;
struct IDWriteInlineObject;
struct IDWriteLocalFontFileLoader;
struct IDWriteLocalizedStrings;
struct IDWriteNumberSubstitution;
struct IDWritePaintReader;
struct IDWritePixelSnapping;
struct IDWriteRenderingParams1;
struct IDWriteRenderingParams2;
struct IDWriteRenderingParams3;
struct IDWriteRenderingParams;
struct IDWriteStringList;
struct IDWriteTextAnalysisSink;
struct IDWriteTextAnalysisSource;
struct IDWriteTextAnalyzer1;
struct IDWriteTextAnalyzer;
struct IDWriteTextFormat1;
struct IDWriteTextFormat2;
struct IDWriteTextFormat3;
struct IDWriteTextFormat;
struct IDWriteTextLayout1;
struct IDWriteTextLayout2;
struct IDWriteTextLayout;
struct IDWriteTextRenderer;
struct IDWriteTypography;

// <DWrite.h>
enum DWRITE_FONT_FACE_TYPE {
    DWRITE_FONT_FACE_TYPE_CFF,
    DWRITE_FONT_FACE_TYPE_TRUETYPE,
    DWRITE_FONT_FACE_TYPE_OPENTYPE_COLLECTION,
    DWRITE_FONT_FACE_TYPE_TYPE1,
    DWRITE_FONT_FACE_TYPE_VECTOR,
    DWRITE_FONT_FACE_TYPE_BITMAP,
    DWRITE_FONT_FACE_TYPE_UNKNOWN,
    DWRITE_FONT_FACE_TYPE_RAW_CFF,
    DWRITE_FONT_FACE_TYPE_TRUETYPE_COLLECTION = DWRITE_FONT_FACE_TYPE_OPENTYPE_COLLECTION
};
enum DWRITE_FONT_SIMULATIONS {
    DWRITE_FONT_SIMULATIONS_NONE = 0x0000,
    DWRITE_FONT_SIMULATIONS_BOLD = 0x0001,
    DWRITE_FONT_SIMULATIONS_OBLIQUE = 0x0002
};
enum DWRITE_FONT_WEIGHT {
    DWRITE_FONT_WEIGHT_THIN = 100,
    DWRITE_FONT_WEIGHT_EXTRA_LIGHT = 200,
    DWRITE_FONT_WEIGHT_ULTRA_LIGHT = 200,
    DWRITE_FONT_WEIGHT_LIGHT = 300,
    DWRITE_FONT_WEIGHT_SEMI_LIGHT = 350,
    DWRITE_FONT_WEIGHT_NORMAL = 400,
    DWRITE_FONT_WEIGHT_REGULAR = 400,
    DWRITE_FONT_WEIGHT_MEDIUM = 500,
    DWRITE_FONT_WEIGHT_DEMI_BOLD = 600,
    DWRITE_FONT_WEIGHT_SEMI_BOLD = 600,
    DWRITE_FONT_WEIGHT_BOLD = 700,
    DWRITE_FONT_WEIGHT_EXTRA_BOLD = 800,
    DWRITE_FONT_WEIGHT_ULTRA_BOLD = 800,
    DWRITE_FONT_WEIGHT_BLACK = 900,
    DWRITE_FONT_WEIGHT_HEAVY = 900,
    DWRITE_FONT_WEIGHT_EXTRA_BLACK = 950,
    DWRITE_FONT_WEIGHT_ULTRA_BLACK = 950
};
enum DWRITE_FONT_STRETCH {
    DWRITE_FONT_STRETCH_UNDEFINED = 0,
    DWRITE_FONT_STRETCH_ULTRA_CONDENSED = 1,
    DWRITE_FONT_STRETCH_EXTRA_CONDENSED = 2,
    DWRITE_FONT_STRETCH_CONDENSED = 3,
    DWRITE_FONT_STRETCH_SEMI_CONDENSED = 4,
    DWRITE_FONT_STRETCH_NORMAL = 5,
    DWRITE_FONT_STRETCH_MEDIUM = 5,
    DWRITE_FONT_STRETCH_SEMI_EXPANDED = 6,
    DWRITE_FONT_STRETCH_EXPANDED = 7,
    DWRITE_FONT_STRETCH_EXTRA_EXPANDED = 8,
    DWRITE_FONT_STRETCH_ULTRA_EXPANDED = 9
};
enum DWRITE_FONT_STYLE {
    DWRITE_FONT_STYLE_NORMAL,
    DWRITE_FONT_STYLE_OBLIQUE,
    DWRITE_FONT_STYLE_ITALIC
};
struct DWRITE_FONT_METRICS {
    UINT16 designUnitsPerEm;
    UINT16 ascent;
    UINT16 descent;
    INT16 lineGap;
    UINT16 capHeight;
    UINT16 xHeight;
    INT16 underlinePosition;
    UINT16 underlineThickness;
    INT16 strikethroughPosition;
    UINT16 strikethroughThickness;
};
struct DWRITE_GLYPH_METRICS {
    INT32 leftSideBearing;
    UINT32 advanceWidth;
    INT32 rightSideBearing;
    INT32 topSideBearing;
    UINT32 advanceHeight;
    INT32 bottomSideBearing;
    INT32 verticalOriginY;
};
struct DWRITE_GLYPH_OFFSET {
    FLOAT advanceOffset;
    FLOAT ascenderOffset;
};
enum DWRITE_FACTORY_TYPE {
    DWRITE_FACTORY_TYPE_SHARED,
    DWRITE_FACTORY_TYPE_ISOLATED
};
struct __declspec(uuid("727cad4e-d6af-4c9e-8a08-d695b11caa49")) __declspec(novtable) IDWriteFontFileLoader : IUnknown {
    MAICORD_COM_UNUSED(CreateStreamFromKey);
};
struct __declspec(uuid("b2d9f3ec-c9fe-4a11-a2ec-d86208f7c0a2")) __declspec(novtable) IDWriteLocalFontFileLoader : IDWriteFontFileLoader {
    virtual __declspec(nothrow) HRESULT __stdcall GetFilePathLengthFromKey(void const* fontFileReferenceKey,
        UINT32 fontFileReferenceKeySize, UINT32* filePathLength) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFilePathFromKey(void const* fontFileReferenceKey,
        UINT32 fontFileReferenceKeySize, WCHAR* filePath, UINT32 filePathSize) = 0;
};
struct __declspec(uuid("739d886a-cef5-47dc-8769-1a8b41bebbb0")) __declspec(novtable) IDWriteFontFile : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall GetReferenceKey(void const** fontFileReferenceKey,
        UINT32* fontFileReferenceKeySize) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetLoader(IDWriteFontFileLoader** fontFileLoader) = 0;
};
enum DWRITE_PIXEL_GEOMETRY {
    DWRITE_PIXEL_GEOMETRY_FLAT,
    DWRITE_PIXEL_GEOMETRY_RGB,
    DWRITE_PIXEL_GEOMETRY_BGR
};
enum DWRITE_RENDERING_MODE {
    DWRITE_RENDERING_MODE_DEFAULT,
    DWRITE_RENDERING_MODE_ALIASED,
    DWRITE_RENDERING_MODE_GDI_CLASSIC,
    DWRITE_RENDERING_MODE_GDI_NATURAL,
    DWRITE_RENDERING_MODE_NATURAL,
    DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
    DWRITE_RENDERING_MODE_OUTLINE,
    DWRITE_RENDERING_MODE_CLEARTYPE_GDI_CLASSIC = DWRITE_RENDERING_MODE_GDI_CLASSIC,
    DWRITE_RENDERING_MODE_CLEARTYPE_GDI_NATURAL = DWRITE_RENDERING_MODE_GDI_NATURAL,
    DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL = DWRITE_RENDERING_MODE_NATURAL,
    DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC = DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC
};
struct DWRITE_MATRIX {
    FLOAT m11;
    FLOAT m12;
    FLOAT m21;
    FLOAT m22;
    FLOAT dx;
    FLOAT dy;
};
struct __declspec(uuid("2f0da53a-2add-47cd-82ee-d9ec34688e75")) __declspec(novtable) IDWriteRenderingParams : IUnknown {
    MAICORD_COM_UNUSED(GetGamma);
    MAICORD_COM_UNUSED(GetEnhancedContrast);
    MAICORD_COM_UNUSED(GetClearTypeLevel);
    MAICORD_COM_UNUSED(GetPixelGeometry);
    MAICORD_COM_UNUSED(GetRenderingMode);
};
typedef ID2D1SimplifiedGeometrySink IDWriteGeometrySink;
struct __declspec(uuid("5f49804d-7024-4d43-bfa9-d25984f53849")) __declspec(novtable) IDWriteFontFace : IUnknown {
    MAICORD_COM_UNUSED(GetType);
    virtual __declspec(nothrow) HRESULT __stdcall GetFiles(UINT32* numberOfFiles, IDWriteFontFile** fontFiles) = 0;
    virtual __declspec(nothrow) UINT32 __stdcall GetIndex() = 0;
    virtual __declspec(nothrow) DWRITE_FONT_SIMULATIONS __stdcall GetSimulations() = 0;
    MAICORD_COM_UNUSED(IsSymbolFont);
    virtual __declspec(nothrow) void __stdcall GetMetrics(DWRITE_FONT_METRICS* fontFaceMetrics) = 0;
    MAICORD_COM_UNUSED(GetGlyphCount);
    virtual __declspec(nothrow) HRESULT __stdcall GetDesignGlyphMetrics(UINT16 const* glyphIndices, UINT32 glyphCount,
        DWRITE_GLYPH_METRICS* glyphMetrics, BOOL isSideways = 0) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetGlyphIndicesW(UINT32 const* codePoints, UINT32 codePointCount,
        UINT16* glyphIndices) = 0;
    MAICORD_COM_UNUSED(TryGetFontTable);
    MAICORD_COM_UNUSED(ReleaseFontTable);
    virtual __declspec(nothrow) HRESULT __stdcall GetGlyphRunOutline(FLOAT emSize, UINT16 const* glyphIndices,
        FLOAT const* glyphAdvances, DWRITE_GLYPH_OFFSET const* glyphOffsets, UINT32 glyphCount, BOOL isSideways,
        BOOL isRightToLeft, IDWriteGeometrySink* geometrySink) = 0;
    MAICORD_COM_UNUSED(GetRecommendedRenderingMode);
    MAICORD_COM_UNUSED(GetGdiCompatibleMetrics);
    MAICORD_COM_UNUSED(GetGdiCompatibleGlyphMetrics);
};
struct __declspec(uuid("08256209-099a-4b34-b86d-c22b110e7771")) __declspec(novtable) IDWriteLocalizedStrings : IUnknown {
    virtual __declspec(nothrow) UINT32 __stdcall GetCount() = 0;
    MAICORD_COM_UNUSED(FindLocaleName);
    virtual __declspec(nothrow) HRESULT __stdcall GetLocaleNameLength(UINT32 index, UINT32* length) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetLocaleName(UINT32 index, WCHAR* localeName, UINT32 size) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetStringLength(UINT32 index, UINT32* length) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetString(UINT32 index, WCHAR* stringBuffer, UINT32 size) = 0;
};
struct __declspec(uuid("a84cee02-3eea-4eee-a827-87c1a02a0fcc")) __declspec(novtable) IDWriteFontCollection : IUnknown {
    virtual __declspec(nothrow) UINT32 __stdcall GetFontFamilyCount() = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFontFamily(UINT32 index, IDWriteFontFamily** fontFamily) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall FindFamilyName(WCHAR const* familyName, UINT32* index,
        BOOL* exists) = 0;
    MAICORD_COM_UNUSED(GetFontFromFontFace);
};
struct __declspec(uuid("1a0d8438-1d97-4ec1-aef9-a2fb86ed6acb")) __declspec(novtable) IDWriteFontList : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall GetFontCollection(IDWriteFontCollection** fontCollection) = 0;
    MAICORD_COM_UNUSED(GetFontCount);
    MAICORD_COM_UNUSED(GetFont);
};
struct __declspec(uuid("da20d8ef-812a-4c43-9802-62ec4abd7add")) __declspec(novtable) IDWriteFontFamily : IDWriteFontList {
    virtual __declspec(nothrow) HRESULT __stdcall GetFamilyNames(IDWriteLocalizedStrings** names) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFirstMatchingFont(DWRITE_FONT_WEIGHT weight,
        DWRITE_FONT_STRETCH stretch, DWRITE_FONT_STYLE style, IDWriteFont** matchingFont) = 0;
    MAICORD_COM_UNUSED(GetMatchingFonts);
};
struct __declspec(uuid("acd16696-8c14-4f5d-877e-fe3fc1d32737")) __declspec(novtable) IDWriteFont : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall GetFontFamily(IDWriteFontFamily** fontFamily) = 0;
    virtual __declspec(nothrow) DWRITE_FONT_WEIGHT __stdcall GetWeight() = 0;
    MAICORD_COM_UNUSED(GetStretch);
    virtual __declspec(nothrow) DWRITE_FONT_STYLE __stdcall GetStyle() = 0;
    MAICORD_COM_UNUSED(IsSymbolFont);
    MAICORD_COM_UNUSED(GetFaceNames);
    MAICORD_COM_UNUSED(GetInformationalStrings);
    virtual __declspec(nothrow) DWRITE_FONT_SIMULATIONS __stdcall GetSimulations() = 0;
    virtual __declspec(nothrow) void __stdcall GetMetrics(DWRITE_FONT_METRICS* fontMetrics) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall HasCharacter(UINT32 unicodeValue, BOOL* exists) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontFace(IDWriteFontFace** fontFace) = 0;
};
enum DWRITE_READING_DIRECTION {
    DWRITE_READING_DIRECTION_LEFT_TO_RIGHT = 0,
    DWRITE_READING_DIRECTION_RIGHT_TO_LEFT = 1,
    DWRITE_READING_DIRECTION_TOP_TO_BOTTOM = 2,
    DWRITE_READING_DIRECTION_BOTTOM_TO_TOP = 3
};
enum DWRITE_FLOW_DIRECTION {
    DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM = 0,
    DWRITE_FLOW_DIRECTION_BOTTOM_TO_TOP = 1,
    DWRITE_FLOW_DIRECTION_LEFT_TO_RIGHT = 2,
    DWRITE_FLOW_DIRECTION_RIGHT_TO_LEFT = 3
};
enum DWRITE_TEXT_ALIGNMENT {
    DWRITE_TEXT_ALIGNMENT_LEADING,
    DWRITE_TEXT_ALIGNMENT_TRAILING,
    DWRITE_TEXT_ALIGNMENT_CENTER,
    DWRITE_TEXT_ALIGNMENT_JUSTIFIED
};
enum DWRITE_PARAGRAPH_ALIGNMENT {
    DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
    DWRITE_PARAGRAPH_ALIGNMENT_FAR,
    DWRITE_PARAGRAPH_ALIGNMENT_CENTER
};
enum DWRITE_WORD_WRAPPING {
    DWRITE_WORD_WRAPPING_WRAP = 0,
    DWRITE_WORD_WRAPPING_NO_WRAP = 1,
    DWRITE_WORD_WRAPPING_EMERGENCY_BREAK = 2,
    DWRITE_WORD_WRAPPING_WHOLE_WORD = 3,
    DWRITE_WORD_WRAPPING_CHARACTER = 4
};
enum DWRITE_LINE_SPACING_METHOD {
    DWRITE_LINE_SPACING_METHOD_DEFAULT,
    DWRITE_LINE_SPACING_METHOD_UNIFORM,
    DWRITE_LINE_SPACING_METHOD_PROPORTIONAL
};
enum DWRITE_TRIMMING_GRANULARITY {
    DWRITE_TRIMMING_GRANULARITY_NONE,
    DWRITE_TRIMMING_GRANULARITY_CHARACTER,
    DWRITE_TRIMMING_GRANULARITY_WORD
};
enum DWRITE_FONT_FEATURE_TAG {
    DWRITE_FONT_FEATURE_TAG_ALTERNATIVE_FRACTIONS = ((static_cast < UINT32 > (static_cast < UINT8 > ('c')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('r')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('f')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('a'))),
    DWRITE_FONT_FEATURE_TAG_PETITE_CAPITALS_FROM_CAPITALS = ((static_cast < UINT32 > (static_cast < UINT8 > ('c')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('p')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('2')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('c'))),
    DWRITE_FONT_FEATURE_TAG_SMALL_CAPITALS_FROM_CAPITALS = ((static_cast < UINT32 > (static_cast < UINT8 > ('c')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('2')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('c'))),
    DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_ALTERNATES = ((static_cast < UINT32 > (static_cast < UINT8 > ('t')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('a')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('c'))),
    DWRITE_FONT_FEATURE_TAG_CASE_SENSITIVE_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('e')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('a')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('c'))),
    DWRITE_FONT_FEATURE_TAG_GLYPH_COMPOSITION_DECOMPOSITION = ((static_cast < UINT32 > (static_cast < UINT8 > ('p')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('m')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('c')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('c'))),
    DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_LIGATURES = ((static_cast < UINT32 > (static_cast < UINT8 > ('g')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('i')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('c'))),
    DWRITE_FONT_FEATURE_TAG_CAPITAL_SPACING = ((static_cast < UINT32 > (static_cast < UINT8 > ('p')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('p')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('c'))),
    DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_SWASH = ((static_cast < UINT32 > (static_cast < UINT8 > ('h')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('w')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('c'))),
    DWRITE_FONT_FEATURE_TAG_CURSIVE_POSITIONING = ((static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('r')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('u')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('c'))),
    DWRITE_FONT_FEATURE_TAG_DEFAULT = ((static_cast < UINT32 > (static_cast < UINT8 > ('t')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('f')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('d'))),
    DWRITE_FONT_FEATURE_TAG_DISCRETIONARY_LIGATURES = ((static_cast < UINT32 > (static_cast < UINT8 > ('g')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('i')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('d'))),
    DWRITE_FONT_FEATURE_TAG_EXPERT_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('t')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('p')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('x')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('e'))),
    DWRITE_FONT_FEATURE_TAG_FRACTIONS = ((static_cast < UINT32 > (static_cast < UINT8 > ('c')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('a')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('r')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('f'))),
    DWRITE_FONT_FEATURE_TAG_FULL_WIDTH = ((static_cast < UINT32 > (static_cast < UINT8 > ('d')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('i')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('w')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('f'))),
    DWRITE_FONT_FEATURE_TAG_HALF_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('f')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('a')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('h'))),
    DWRITE_FONT_FEATURE_TAG_HALANT_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('n')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('a')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('h'))),
    DWRITE_FONT_FEATURE_TAG_ALTERNATE_HALF_WIDTH = ((static_cast < UINT32 > (static_cast < UINT8 > ('t')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('a')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('h'))),
    DWRITE_FONT_FEATURE_TAG_HISTORICAL_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('t')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('i')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('h'))),
    DWRITE_FONT_FEATURE_TAG_HORIZONTAL_KANA_ALTERNATES = ((static_cast < UINT32 > (static_cast < UINT8 > ('a')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('n')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('k')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('h'))),
    DWRITE_FONT_FEATURE_TAG_HISTORICAL_LIGATURES = ((static_cast < UINT32 > (static_cast < UINT8 > ('g')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('i')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('h'))),
    DWRITE_FONT_FEATURE_TAG_HALF_WIDTH = ((static_cast < UINT32 > (static_cast < UINT8 > ('d')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('i')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('w')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('h'))),
    DWRITE_FONT_FEATURE_TAG_HOJO_KANJI_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('o')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('j')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('o')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('h'))),
    DWRITE_FONT_FEATURE_TAG_JIS04_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('4')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('0')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('p')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('j'))),
    DWRITE_FONT_FEATURE_TAG_JIS78_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('8')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('7')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('p')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('j'))),
    DWRITE_FONT_FEATURE_TAG_JIS83_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('3')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('8')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('p')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('j'))),
    DWRITE_FONT_FEATURE_TAG_JIS90_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('0')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('9')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('p')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('j'))),
    DWRITE_FONT_FEATURE_TAG_KERNING = ((static_cast < UINT32 > (static_cast < UINT8 > ('n')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('r')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('e')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('k'))),
    DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES = ((static_cast < UINT32 > (static_cast < UINT8 > ('a')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('g')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('i')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('l'))),
    DWRITE_FONT_FEATURE_TAG_LINING_FIGURES = ((static_cast < UINT32 > (static_cast < UINT8 > ('m')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('u')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('n')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('l'))),
    DWRITE_FONT_FEATURE_TAG_LOCALIZED_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('c')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('o')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('l'))),
    DWRITE_FONT_FEATURE_TAG_MARK_POSITIONING = ((static_cast < UINT32 > (static_cast < UINT8 > ('k')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('r')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('a')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('m'))),
    DWRITE_FONT_FEATURE_TAG_MATHEMATICAL_GREEK = ((static_cast < UINT32 > (static_cast < UINT8 > ('k')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('r')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('g')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('m'))),
    DWRITE_FONT_FEATURE_TAG_MARK_TO_MARK_POSITIONING = ((static_cast < UINT32 > (static_cast < UINT8 > ('k')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('m')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('k')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('m'))),
    DWRITE_FONT_FEATURE_TAG_ALTERNATE_ANNOTATION_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('t')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('a')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('n'))),
    DWRITE_FONT_FEATURE_TAG_NLC_KANJI_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('k')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('c')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('n'))),
    DWRITE_FONT_FEATURE_TAG_OLD_STYLE_FIGURES = ((static_cast < UINT32 > (static_cast < UINT8 > ('m')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('u')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('n')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('o'))),
    DWRITE_FONT_FEATURE_TAG_ORDINALS = ((static_cast < UINT32 > (static_cast < UINT8 > ('n')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('d')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('r')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('o'))),
    DWRITE_FONT_FEATURE_TAG_PROPORTIONAL_ALTERNATE_WIDTH = ((static_cast < UINT32 > (static_cast < UINT8 > ('t')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('a')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('p'))),
    DWRITE_FONT_FEATURE_TAG_PETITE_CAPITALS = ((static_cast < UINT32 > (static_cast < UINT8 > ('p')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('a')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('c')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('p'))),
    DWRITE_FONT_FEATURE_TAG_PROPORTIONAL_FIGURES = ((static_cast < UINT32 > (static_cast < UINT8 > ('m')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('u')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('n')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('p'))),
    DWRITE_FONT_FEATURE_TAG_PROPORTIONAL_WIDTHS = ((static_cast < UINT32 > (static_cast < UINT8 > ('d')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('i')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('w')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('p'))),
    DWRITE_FONT_FEATURE_TAG_QUARTER_WIDTHS = ((static_cast < UINT32 > (static_cast < UINT8 > ('d')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('i')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('w')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('q'))),
    DWRITE_FONT_FEATURE_TAG_REQUIRED_LIGATURES = ((static_cast < UINT32 > (static_cast < UINT8 > ('g')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('i')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('r'))),
    DWRITE_FONT_FEATURE_TAG_RUBY_NOTATION_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('y')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('b')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('u')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('r'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_ALTERNATES = ((static_cast < UINT32 > (static_cast < UINT8 > ('t')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('a')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_SCIENTIFIC_INFERIORS = ((static_cast < UINT32 > (static_cast < UINT8 > ('f')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('n')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('i')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_SMALL_CAPITALS = ((static_cast < UINT32 > (static_cast < UINT8 > ('p')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('c')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('m')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_SIMPLIFIED_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('p')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('m')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_1 = ((static_cast < UINT32 > (static_cast < UINT8 > ('1')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('0')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_2 = ((static_cast < UINT32 > (static_cast < UINT8 > ('2')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('0')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_3 = ((static_cast < UINT32 > (static_cast < UINT8 > ('3')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('0')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_4 = ((static_cast < UINT32 > (static_cast < UINT8 > ('4')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('0')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_5 = ((static_cast < UINT32 > (static_cast < UINT8 > ('5')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('0')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_6 = ((static_cast < UINT32 > (static_cast < UINT8 > ('6')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('0')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_7 = ((static_cast < UINT32 > (static_cast < UINT8 > ('7')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('0')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_8 = ((static_cast < UINT32 > (static_cast < UINT8 > ('8')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('0')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_9 = ((static_cast < UINT32 > (static_cast < UINT8 > ('9')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('0')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_10 = ((static_cast < UINT32 > (static_cast < UINT8 > ('0')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('1')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_11 = ((static_cast < UINT32 > (static_cast < UINT8 > ('1')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('1')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_12 = ((static_cast < UINT32 > (static_cast < UINT8 > ('2')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('1')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_13 = ((static_cast < UINT32 > (static_cast < UINT8 > ('3')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('1')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_14 = ((static_cast < UINT32 > (static_cast < UINT8 > ('4')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('1')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_15 = ((static_cast < UINT32 > (static_cast < UINT8 > ('5')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('1')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_16 = ((static_cast < UINT32 > (static_cast < UINT8 > ('6')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('1')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_17 = ((static_cast < UINT32 > (static_cast < UINT8 > ('7')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('1')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_18 = ((static_cast < UINT32 > (static_cast < UINT8 > ('8')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('1')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_19 = ((static_cast < UINT32 > (static_cast < UINT8 > ('9')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('1')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_STYLISTIC_SET_20 = ((static_cast < UINT32 > (static_cast < UINT8 > ('0')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('2')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_SUBSCRIPT = ((static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('b')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('u')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_SUPERSCRIPT = ((static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('p')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('u')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_SWASH = ((static_cast < UINT32 > (static_cast < UINT8 > ('h')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('w')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))),
    DWRITE_FONT_FEATURE_TAG_TITLING = ((static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('t')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('i')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('t'))),
    DWRITE_FONT_FEATURE_TAG_TRADITIONAL_NAME_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('m')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('a')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('n')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('t'))),
    DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES = ((static_cast < UINT32 > (static_cast < UINT8 > ('m')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('u')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('n')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('t'))),
    DWRITE_FONT_FEATURE_TAG_TRADITIONAL_FORMS = ((static_cast < UINT32 > (static_cast < UINT8 > ('d')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('a')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('r')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('t'))),
    DWRITE_FONT_FEATURE_TAG_THIRD_WIDTHS = ((static_cast < UINT32 > (static_cast < UINT8 > ('d')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('i')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('w')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('t'))),
    DWRITE_FONT_FEATURE_TAG_UNICASE = ((static_cast < UINT32 > (static_cast < UINT8 > ('c')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('i')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('n')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('u'))),
    DWRITE_FONT_FEATURE_TAG_VERTICAL_WRITING = ((static_cast < UINT32 > (static_cast < UINT8 > ('t')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('r')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('e')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('v'))),
    DWRITE_FONT_FEATURE_TAG_VERTICAL_ALTERNATES_AND_ROTATION = ((static_cast < UINT32 > (static_cast < UINT8 > ('2')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('t')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('r')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('v'))),
    DWRITE_FONT_FEATURE_TAG_SLASHED_ZERO = ((static_cast < UINT32 > (static_cast < UINT8 > ('o')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('r')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('e')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('z')))
};
struct DWRITE_TEXT_RANGE {
    UINT32 startPosition;
    UINT32 length;
};
struct DWRITE_FONT_FEATURE {
    DWRITE_FONT_FEATURE_TAG nameTag;
    UINT32 parameter;
};
struct DWRITE_TRIMMING {
    DWRITE_TRIMMING_GRANULARITY granularity;
    UINT32 delimiter;
    UINT32 delimiterCount;
};
struct __declspec(uuid("9c906818-31d7-4fd3-a151-7c5e225db55a")) __declspec(novtable) IDWriteTextFormat : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall SetTextAlignment(DWRITE_TEXT_ALIGNMENT textAlignment) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT paragraphAlignment) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetWordWrapping(DWRITE_WORD_WRAPPING wordWrapping) = 0;
    MAICORD_COM_UNUSED(SetReadingDirection);
    MAICORD_COM_UNUSED(SetFlowDirection);
    virtual __declspec(nothrow) HRESULT __stdcall SetIncrementalTabStop(FLOAT incrementalTabStop) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetTrimming(DWRITE_TRIMMING const* trimmingOptions,
        IDWriteInlineObject* trimmingSign) = 0;
    MAICORD_COM_UNUSED(SetLineSpacing);
    virtual __declspec(nothrow) DWRITE_TEXT_ALIGNMENT __stdcall GetTextAlignment() = 0;
    virtual __declspec(nothrow) DWRITE_PARAGRAPH_ALIGNMENT __stdcall GetParagraphAlignment() = 0;
    virtual __declspec(nothrow) DWRITE_WORD_WRAPPING __stdcall GetWordWrapping() = 0;
    virtual __declspec(nothrow) DWRITE_READING_DIRECTION __stdcall GetReadingDirection() = 0;
    virtual __declspec(nothrow) DWRITE_FLOW_DIRECTION __stdcall GetFlowDirection() = 0;
    virtual __declspec(nothrow) FLOAT __stdcall GetIncrementalTabStop() = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetTrimming(DWRITE_TRIMMING* trimmingOptions,
        IDWriteInlineObject** trimmingSign) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetLineSpacing(DWRITE_LINE_SPACING_METHOD* lineSpacingMethod,
        FLOAT* lineSpacing, FLOAT* baseline) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFontCollection(IDWriteFontCollection** fontCollection) = 0;
    virtual __declspec(nothrow) UINT32 __stdcall GetFontFamilyNameLength() = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFontFamilyName(WCHAR* fontFamilyName, UINT32 nameSize) = 0;
    virtual __declspec(nothrow) DWRITE_FONT_WEIGHT __stdcall GetFontWeight() = 0;
    virtual __declspec(nothrow) DWRITE_FONT_STYLE __stdcall GetFontStyle() = 0;
    virtual __declspec(nothrow) DWRITE_FONT_STRETCH __stdcall GetFontStretch() = 0;
    virtual __declspec(nothrow) FLOAT __stdcall GetFontSize() = 0;
    virtual __declspec(nothrow) UINT32 __stdcall GetLocaleNameLength() = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetLocaleName(WCHAR* localeName, UINT32 nameSize) = 0;
};
struct __declspec(uuid("55f1112b-1dc2-4b3c-9541-f46894ed85b6")) __declspec(novtable) IDWriteTypography : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall AddFontFeature(DWRITE_FONT_FEATURE fontFeature) = 0;
};
enum DWRITE_SCRIPT_SHAPES {
    DWRITE_SCRIPT_SHAPES_DEFAULT = 0,
    DWRITE_SCRIPT_SHAPES_NO_VISUAL = 1
};
struct DWRITE_SCRIPT_ANALYSIS {
    UINT16 script;
    DWRITE_SCRIPT_SHAPES shapes;
};
enum DWRITE_BREAK_CONDITION {
    DWRITE_BREAK_CONDITION_NEUTRAL,
    DWRITE_BREAK_CONDITION_CAN_BREAK,
    DWRITE_BREAK_CONDITION_MAY_NOT_BREAK,
    DWRITE_BREAK_CONDITION_MUST_BREAK
};
struct DWRITE_LINE_BREAKPOINT {
    UINT8 breakConditionBefore : 2;
    UINT8 breakConditionAfter : 2;
    UINT8 isWhitespace : 1;
    UINT8 isSoftHyphen : 1;
    UINT8 padding : 2;
};
struct __declspec(uuid("14885CC9-BAB0-4f90-B6ED-5C366A2CD03D")) __declspec(novtable) IDWriteNumberSubstitution : public IUnknown {
};
struct DWRITE_SHAPING_GLYPH_PROPERTIES {
    UINT16 justification : 4;
    UINT16 isClusterStart : 1;
    UINT16 isDiacritic : 1;
    UINT16 isZeroWidthSpace : 1;
    UINT16 reserved : 9;
};
struct __declspec(uuid("688e1a58-5094-47c8-adc8-fbcea60ae92b")) __declspec(novtable) IDWriteTextAnalysisSource : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall GetTextAtPosition(UINT32 textPosition, WCHAR const** textString,
        UINT32* textLength) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetTextBeforePosition(UINT32 textPosition, WCHAR const** textString,
        UINT32* textLength) = 0;
    virtual __declspec(nothrow) DWRITE_READING_DIRECTION __stdcall GetParagraphReadingDirection() = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetLocaleName(UINT32 textPosition, UINT32* textLength,
        WCHAR const** localeName) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetNumberSubstitution(UINT32 textPosition, UINT32* textLength,
        IDWriteNumberSubstitution** numberSubstitution) = 0;
};
struct __declspec(uuid("5810cd44-0ca0-4701-b3fa-bec5182ae4f6")) __declspec(novtable) IDWriteTextAnalysisSink : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall SetScriptAnalysis(UINT32 textPosition, UINT32 textLength,
        DWRITE_SCRIPT_ANALYSIS const* scriptAnalysis) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetLineBreakpoints(UINT32 textPosition, UINT32 textLength,
        DWRITE_LINE_BREAKPOINT const* lineBreakpoints) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetBidiLevel(UINT32 textPosition, UINT32 textLength,
        UINT8 explicitLevel, UINT8 resolvedLevel) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetNumberSubstitution(UINT32 textPosition, UINT32 textLength,
        IDWriteNumberSubstitution* numberSubstitution) = 0;
};
struct __declspec(uuid("b7e6163e-7f46-43b4-84b3-e4e6249c365d")) __declspec(novtable) IDWriteTextAnalyzer : IUnknown {
    MAICORD_COM_UNUSED(AnalyzeScript);
    MAICORD_COM_UNUSED(AnalyzeBidi);
    MAICORD_COM_UNUSED(AnalyzeNumberSubstitution);
    virtual __declspec(nothrow) HRESULT __stdcall AnalyzeLineBreakpoints(IDWriteTextAnalysisSource* analysisSource,
        UINT32 textPosition, UINT32 textLength, IDWriteTextAnalysisSink* analysisSink) = 0;
    MAICORD_COM_UNUSED(GetGlyphs);
    MAICORD_COM_UNUSED(GetGlyphPlacements);
    MAICORD_COM_UNUSED(GetGdiCompatibleGlyphPlacements);
};
struct DWRITE_GLYPH_RUN {
    IDWriteFontFace* fontFace;
    FLOAT fontEmSize;
    UINT32 glyphCount;
    UINT16 const* glyphIndices;
    FLOAT const* glyphAdvances;
    DWRITE_GLYPH_OFFSET const* glyphOffsets;
    BOOL isSideways;
    UINT32 bidiLevel;
};
struct DWRITE_GLYPH_RUN_DESCRIPTION {
    WCHAR const* localeName;
    WCHAR const* string;
    UINT32 stringLength;
    UINT16 const* clusterMap;
    UINT32 textPosition;
};
struct DWRITE_UNDERLINE {
    FLOAT width;
    FLOAT thickness;
    FLOAT offset;
    FLOAT runHeight;
    DWRITE_READING_DIRECTION readingDirection;
    DWRITE_FLOW_DIRECTION flowDirection;
    WCHAR const* localeName;
    DWRITE_MEASURING_MODE measuringMode;
};
struct DWRITE_STRIKETHROUGH {
    FLOAT width;
    FLOAT thickness;
    FLOAT offset;
    DWRITE_READING_DIRECTION readingDirection;
    DWRITE_FLOW_DIRECTION flowDirection;
    WCHAR const* localeName;
    DWRITE_MEASURING_MODE measuringMode;
};
struct DWRITE_LINE_METRICS {
    UINT32 length;
    UINT32 trailingWhitespaceLength;
    UINT32 newlineLength;
    FLOAT height;
    FLOAT baseline;
    BOOL isTrimmed;
};
struct DWRITE_CLUSTER_METRICS {
    FLOAT width;
    UINT16 length;
    UINT16 canWrapLineAfter : 1;
    UINT16 isWhitespace : 1;
    UINT16 isNewline : 1;
    UINT16 isSoftHyphen : 1;
    UINT16 isRightToLeft : 1;
    UINT16 padding : 11;
};
struct DWRITE_TEXT_METRICS {
    FLOAT left;
    FLOAT top;
    FLOAT width;
    FLOAT widthIncludingTrailingWhitespace;
    FLOAT height;
    FLOAT layoutWidth;
    FLOAT layoutHeight;
    UINT32 maxBidiReorderingDepth;
    UINT32 lineCount;
};
struct DWRITE_INLINE_OBJECT_METRICS {
    FLOAT width;
    FLOAT height;
    FLOAT baseline;
    BOOL supportsSideways;
};
struct DWRITE_OVERHANG_METRICS {
    FLOAT left;
    FLOAT top;
    FLOAT right;
    FLOAT bottom;
};
struct DWRITE_HIT_TEST_METRICS {
    UINT32 textPosition;
    UINT32 length;
    FLOAT left;
    FLOAT top;
    FLOAT width;
    FLOAT height;
    UINT32 bidiLevel;
    BOOL isText;
    BOOL isTrimmed;
};
struct __declspec(uuid("8339FDE3-106F-47ab-8373-1C6295EB10B3")) __declspec(novtable) IDWriteInlineObject : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall Draw(void* clientDrawingContext, IDWriteTextRenderer* renderer,
        FLOAT originX, FLOAT originY, BOOL isSideways, BOOL isRightToLeft, IUnknown* clientDrawingEffect) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetMetrics(DWRITE_INLINE_OBJECT_METRICS* metrics) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetOverhangMetrics(DWRITE_OVERHANG_METRICS* overhangs) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetBreakConditions(DWRITE_BREAK_CONDITION* breakConditionBefore,
        DWRITE_BREAK_CONDITION* breakConditionAfter) = 0;
};
struct __declspec(uuid("eaf3a2da-ecf4-4d24-b644-b34f6842024b")) __declspec(novtable) IDWritePixelSnapping : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall IsPixelSnappingDisabled(void* clientDrawingContext,
        BOOL* isDisabled) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetCurrentTransform(void* clientDrawingContext,
        DWRITE_MATRIX* transform) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetPixelsPerDip(void* clientDrawingContext, FLOAT* pixelsPerDip) = 0;
};
struct __declspec(uuid("ef8a8135-5cc6-45fe-8825-c5a0724eb819")) __declspec(novtable) IDWriteTextRenderer : IDWritePixelSnapping {
    virtual __declspec(nothrow) HRESULT __stdcall DrawGlyphRun(void* clientDrawingContext, FLOAT baselineOriginX,
        FLOAT baselineOriginY, DWRITE_MEASURING_MODE measuringMode, DWRITE_GLYPH_RUN const* glyphRun,
        DWRITE_GLYPH_RUN_DESCRIPTION const* glyphRunDescription, IUnknown* clientDrawingEffect) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall DrawUnderline(void* clientDrawingContext, FLOAT baselineOriginX,
        FLOAT baselineOriginY, DWRITE_UNDERLINE const* underline, IUnknown* clientDrawingEffect) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall DrawStrikethrough(void* clientDrawingContext, FLOAT baselineOriginX,
        FLOAT baselineOriginY, DWRITE_STRIKETHROUGH const* strikethrough, IUnknown* clientDrawingEffect) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall DrawInlineObject(void* clientDrawingContext, FLOAT originX,
        FLOAT originY, IDWriteInlineObject* inlineObject, BOOL isSideways, BOOL isRightToLeft,
        IUnknown* clientDrawingEffect) = 0;
};
struct __declspec(uuid("53737037-6d14-410b-9bfe-0b182bb70961")) __declspec(novtable) IDWriteTextLayout : IDWriteTextFormat {
    virtual __declspec(nothrow) HRESULT __stdcall SetMaxWidth(FLOAT maxWidth) = 0;
    MAICORD_COM_UNUSED(SetMaxHeight);
    virtual __declspec(nothrow) HRESULT __stdcall SetFontCollection(IDWriteFontCollection* fontCollection,
        DWRITE_TEXT_RANGE textRange) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetFontFamilyName(WCHAR const* fontFamilyName,
        DWRITE_TEXT_RANGE textRange) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetFontWeight(DWRITE_FONT_WEIGHT fontWeight,
        DWRITE_TEXT_RANGE textRange) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetFontStyle(DWRITE_FONT_STYLE fontStyle,
        DWRITE_TEXT_RANGE textRange) = 0;
    MAICORD_COM_UNUSED(SetFontStretch);
    virtual __declspec(nothrow) HRESULT __stdcall SetFontSize(FLOAT fontSize, DWRITE_TEXT_RANGE textRange) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetUnderline(BOOL hasUnderline, DWRITE_TEXT_RANGE textRange) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetStrikethrough(BOOL hasStrikethrough,
        DWRITE_TEXT_RANGE textRange) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetDrawingEffect(IUnknown* drawingEffect,
        DWRITE_TEXT_RANGE textRange) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetInlineObject(IDWriteInlineObject* inlineObject,
        DWRITE_TEXT_RANGE textRange) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetTypography(IDWriteTypography* typography,
        DWRITE_TEXT_RANGE textRange) = 0;
    MAICORD_COM_UNUSED(SetLocaleName);
    virtual __declspec(nothrow) FLOAT __stdcall GetMaxWidth() = 0;
    MAICORD_COM_UNUSED(GetMaxHeight);
    virtual __declspec(nothrow) HRESULT __stdcall GetFontCollection(UINT32 currentPosition,
        IDWriteFontCollection** fontCollection, DWRITE_TEXT_RANGE* textRange = 0) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFontFamilyNameLength(UINT32 currentPosition, UINT32* nameLength,
        DWRITE_TEXT_RANGE* textRange = 0) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFontFamilyName(UINT32 currentPosition, WCHAR* fontFamilyName,
        UINT32 nameSize, DWRITE_TEXT_RANGE* textRange = 0) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFontWeight(UINT32 currentPosition, DWRITE_FONT_WEIGHT* fontWeight,
        DWRITE_TEXT_RANGE* textRange = 0) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFontStyle(UINT32 currentPosition, DWRITE_FONT_STYLE* fontStyle,
        DWRITE_TEXT_RANGE* textRange = 0) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFontStretch(UINT32 currentPosition,
        DWRITE_FONT_STRETCH* fontStretch, DWRITE_TEXT_RANGE* textRange = 0) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFontSize(UINT32 currentPosition, FLOAT* fontSize,
        DWRITE_TEXT_RANGE* textRange = 0) = 0;
    MAICORD_COM_UNUSED(GetUnderline);
    MAICORD_COM_UNUSED(GetStrikethrough);
    MAICORD_COM_UNUSED(GetDrawingEffect);
    MAICORD_COM_UNUSED(GetInlineObject);
    MAICORD_COM_UNUSED(GetTypography);
    virtual __declspec(nothrow) HRESULT __stdcall GetLocaleNameLength(UINT32 currentPosition, UINT32* nameLength,
        DWRITE_TEXT_RANGE* textRange = 0) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetLocaleName(UINT32 currentPosition, WCHAR* localeName,
        UINT32 nameSize, DWRITE_TEXT_RANGE* textRange = 0) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall Draw(void* clientDrawingContext, IDWriteTextRenderer* renderer,
        FLOAT originX, FLOAT originY) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetLineMetrics(DWRITE_LINE_METRICS* lineMetrics, UINT32 maxLineCount,
        UINT32* actualLineCount) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetMetrics(DWRITE_TEXT_METRICS* textMetrics) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetOverhangMetrics(DWRITE_OVERHANG_METRICS* overhangs) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetClusterMetrics(DWRITE_CLUSTER_METRICS* clusterMetrics,
        UINT32 maxClusterCount, UINT32* actualClusterCount) = 0;
    MAICORD_COM_UNUSED(DetermineMinWidth);
    virtual __declspec(nothrow) HRESULT __stdcall HitTestPoint(FLOAT pointX, FLOAT pointY, BOOL* isTrailingHit,
        BOOL* isInside, DWRITE_HIT_TEST_METRICS* hitTestMetrics) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall HitTestTextPosition(UINT32 textPosition, BOOL isTrailingHit,
        FLOAT* pointX, FLOAT* pointY, DWRITE_HIT_TEST_METRICS* hitTestMetrics) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall HitTestTextRange(UINT32 textPosition, UINT32 textLength,
        FLOAT originX, FLOAT originY, DWRITE_HIT_TEST_METRICS* hitTestMetrics, UINT32 maxHitTestMetricsCount,
        UINT32* actualHitTestMetricsCount) = 0;
    using IDWriteTextFormat::GetFontCollection;
    using IDWriteTextFormat::GetFontFamilyNameLength;
    using IDWriteTextFormat::GetFontFamilyName;
    using IDWriteTextFormat::GetFontWeight;
    using IDWriteTextFormat::GetFontStyle;
    using IDWriteTextFormat::GetFontStretch;
    using IDWriteTextFormat::GetFontSize;
    using IDWriteTextFormat::GetLocaleNameLength;
    using IDWriteTextFormat::GetLocaleName;
};
struct __declspec(uuid("7d97dbf7-e085-42d4-81e3-6a883bded118")) __declspec(novtable) IDWriteGlyphRunAnalysis : IUnknown {
};
struct __declspec(uuid("b859ee5a-d838-4b5b-a2e8-1adc7d93db48")) __declspec(novtable) IDWriteFactory : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall GetSystemFontCollection(IDWriteFontCollection** fontCollection,
        BOOL checkForUpdates = 0) = 0;
    MAICORD_COM_UNUSED(CreateCustomFontCollection);
    MAICORD_COM_UNUSED(RegisterFontCollectionLoader);
    MAICORD_COM_UNUSED(UnregisterFontCollectionLoader);
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontFileReference(WCHAR const* filePath,
        FILETIME const* lastWriteTime, IDWriteFontFile** fontFile) = 0;
    MAICORD_COM_UNUSED(CreateCustomFontFileReference);
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontFace(DWRITE_FONT_FACE_TYPE fontFaceType,
        UINT32 numberOfFiles, IDWriteFontFile* const* fontFiles, UINT32 faceIndex,
        DWRITE_FONT_SIMULATIONS fontFaceSimulationFlags, IDWriteFontFace** fontFace) = 0;
    MAICORD_COM_UNUSED(CreateRenderingParams);
    MAICORD_COM_UNUSED(CreateMonitorRenderingParams);
    MAICORD_COM_UNUSED(CreateCustomRenderingParams);
    MAICORD_COM_UNUSED(RegisterFontFileLoader);
    MAICORD_COM_UNUSED(UnregisterFontFileLoader);
    virtual __declspec(nothrow) HRESULT __stdcall CreateTextFormat(WCHAR const* fontFamilyName,
        IDWriteFontCollection* fontCollection, DWRITE_FONT_WEIGHT fontWeight, DWRITE_FONT_STYLE fontStyle,
        DWRITE_FONT_STRETCH fontStretch, FLOAT fontSize, WCHAR const* localeName, IDWriteTextFormat** textFormat) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateTypography(IDWriteTypography** typography) = 0;
    MAICORD_COM_UNUSED(GetGdiInterop);
    virtual __declspec(nothrow) HRESULT __stdcall CreateTextLayout(WCHAR const* string, UINT32 stringLength,
        IDWriteTextFormat* textFormat, FLOAT maxWidth, FLOAT maxHeight, IDWriteTextLayout** textLayout) = 0;
    MAICORD_COM_UNUSED(CreateGdiCompatibleTextLayout);
    virtual __declspec(nothrow) HRESULT __stdcall CreateEllipsisTrimmingSign(IDWriteTextFormat* textFormat,
        IDWriteInlineObject** trimmingSign) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateTextAnalyzer(IDWriteTextAnalyzer** textAnalyzer) = 0;
    MAICORD_COM_UNUSED(CreateNumberSubstitution);
    MAICORD_COM_UNUSED(CreateGlyphRunAnalysis);
};
// <DWrite_1.h>
enum DWRITE_OUTLINE_THRESHOLD {
    DWRITE_OUTLINE_THRESHOLD_ANTIALIASED,
    DWRITE_OUTLINE_THRESHOLD_ALIASED
};
struct DWRITE_FONT_METRICS1 : public DWRITE_FONT_METRICS {
    INT16 glyphBoxLeft;
    INT16 glyphBoxTop;
    INT16 glyphBoxRight;
    INT16 glyphBoxBottom;
    INT16 subscriptPositionX;
    INT16 subscriptPositionY;
    INT16 subscriptSizeX;
    INT16 subscriptSizeY;
    INT16 superscriptPositionX;
    INT16 superscriptPositionY;
    INT16 superscriptSizeX;
    INT16 superscriptSizeY;
    BOOL hasTypographicMetrics;
};
struct __declspec(uuid("30572f99-dac6-41db-a16e-0486307e606a")) __declspec(novtable) IDWriteFactory1 : IDWriteFactory {
    MAICORD_COM_UNUSED(GetEudcFontCollection);
    virtual __declspec(nothrow) HRESULT __stdcall CreateCustomRenderingParams(FLOAT gamma, FLOAT enhancedContrast,
        FLOAT enhancedContrastGrayscale, FLOAT clearTypeLevel, DWRITE_PIXEL_GEOMETRY pixelGeometry,
        DWRITE_RENDERING_MODE renderingMode, IDWriteRenderingParams1** renderingParams) = 0;
};
struct __declspec(uuid("a71efdb4-9fdb-4838-ad90-cfc3be8c3daf")) __declspec(novtable) IDWriteFontFace1 : IDWriteFontFace {
    virtual __declspec(nothrow) void __stdcall GetMetrics(DWRITE_FONT_METRICS1* fontMetrics) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetGdiCompatibleMetrics(FLOAT emSize, FLOAT pixelsPerDip,
        DWRITE_MATRIX const* transform, DWRITE_FONT_METRICS1* fontMetrics) = 0;
    MAICORD_COM_UNUSED(GetCaretMetrics);
    MAICORD_COM_UNUSED(GetUnicodeRanges);
    MAICORD_COM_UNUSED(IsMonospacedFont);
    MAICORD_COM_UNUSED(GetDesignGlyphAdvances);
    MAICORD_COM_UNUSED(GetGdiCompatibleGlyphAdvances);
    MAICORD_COM_UNUSED(GetKerningPairAdjustments);
    MAICORD_COM_UNUSED(HasKerningPairs);
    virtual __declspec(nothrow) HRESULT __stdcall GetRecommendedRenderingMode(FLOAT fontEmSize, FLOAT dpiX, FLOAT dpiY,
        DWRITE_MATRIX const* transform, BOOL isSideways, DWRITE_OUTLINE_THRESHOLD outlineThreshold,
        DWRITE_MEASURING_MODE measuringMode, DWRITE_RENDERING_MODE* renderingMode) = 0;
    MAICORD_COM_UNUSED(GetVerticalGlyphVariants);
    MAICORD_COM_UNUSED(HasVerticalGlyphVariants);
    using IDWriteFontFace::GetMetrics;
};
struct __declspec(uuid("acd16696-8c14-4f5d-877e-fe3fc1d32738")) __declspec(novtable) IDWriteFont1 : IDWriteFont {
    virtual __declspec(nothrow) void __stdcall GetMetrics(DWRITE_FONT_METRICS1* fontMetrics) = 0;
    MAICORD_COM_UNUSED(GetPanose);
    MAICORD_COM_UNUSED(GetUnicodeRanges);
    MAICORD_COM_UNUSED(IsMonospacedFont);
    using IDWriteFont::GetMetrics;
};
struct __declspec(uuid("94413cf4-a6fc-4248-8b50-6674348fcad3")) __declspec(novtable) IDWriteRenderingParams1 : IDWriteRenderingParams {
    MAICORD_COM_UNUSED(GetGrayscaleEnhancedContrast);
};
struct __declspec(uuid("80DAD800-E21F-4E83-96CE-BFCCE500DB7C")) __declspec(novtable) IDWriteTextAnalyzer1 : IDWriteTextAnalyzer {
    virtual __declspec(nothrow) HRESULT __stdcall ApplyCharacterSpacing(FLOAT leadingSpacing, FLOAT trailingSpacing,
        FLOAT minimumAdvanceWidth, UINT32 textLength, UINT32 glyphCount, UINT16 const* clusterMap,
        FLOAT const* glyphAdvances, DWRITE_GLYPH_OFFSET const* glyphOffsets,
        DWRITE_SHAPING_GLYPH_PROPERTIES const* glyphProperties, FLOAT* modifiedGlyphAdvances,
        DWRITE_GLYPH_OFFSET* modifiedGlyphOffsets) = 0;
};
struct __declspec(uuid("9064D822-80A7-465C-A986-DF65F78B8FEB")) __declspec(novtable) IDWriteTextLayout1 : IDWriteTextLayout {
    MAICORD_COM_UNUSED(SetPairKerning);
    MAICORD_COM_UNUSED(GetPairKerning);
    virtual __declspec(nothrow) HRESULT __stdcall SetCharacterSpacing(FLOAT leadingSpacing, FLOAT trailingSpacing,
        FLOAT minimumAdvanceWidth, DWRITE_TEXT_RANGE textRange) = 0;
    MAICORD_COM_UNUSED(GetCharacterSpacing);
};
enum DWRITE_TEXT_ANTIALIAS_MODE {
    DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE,
    DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE
};
// <DWrite_2.h>
enum DWRITE_GRID_FIT_MODE {
    DWRITE_GRID_FIT_MODE_DEFAULT,
    DWRITE_GRID_FIT_MODE_DISABLED,
    DWRITE_GRID_FIT_MODE_ENABLED
};
struct DWRITE_TEXT_METRICS1 : DWRITE_TEXT_METRICS {
    FLOAT heightIncludingTrailingWhitespace;
};
struct __declspec(uuid("5F174B49-0D8B-4CFB-8BCA-F1CCE9D06C67")) __declspec(novtable) IDWriteTextFormat1 : IDWriteTextFormat {
    MAICORD_COM_UNUSED(SetVerticalGlyphOrientation);
    MAICORD_COM_UNUSED(GetVerticalGlyphOrientation);
    MAICORD_COM_UNUSED(SetLastLineWrapping);
    MAICORD_COM_UNUSED(GetLastLineWrapping);
    MAICORD_COM_UNUSED(SetOpticalAlignment);
    MAICORD_COM_UNUSED(GetOpticalAlignment);
    virtual __declspec(nothrow) HRESULT __stdcall SetFontFallback(IDWriteFontFallback* fontFallback) = 0;
    MAICORD_COM_UNUSED(GetFontFallback);
};
struct __declspec(uuid("1093C18F-8D5E-43F0-B064-0917311B525E")) __declspec(novtable) IDWriteTextLayout2 : IDWriteTextLayout1 {
    virtual __declspec(nothrow) HRESULT __stdcall GetMetrics(DWRITE_TEXT_METRICS1* textMetrics) = 0;
    MAICORD_COM_UNUSED(SetVerticalGlyphOrientation);
    MAICORD_COM_UNUSED(GetVerticalGlyphOrientation);
    MAICORD_COM_UNUSED(SetLastLineWrapping);
    MAICORD_COM_UNUSED(GetLastLineWrapping);
    MAICORD_COM_UNUSED(SetOpticalAlignment);
    MAICORD_COM_UNUSED(GetOpticalAlignment);
    virtual __declspec(nothrow) HRESULT __stdcall SetFontFallback(IDWriteFontFallback* fontFallback) = 0;
    using IDWriteTextLayout::GetMetrics;
};
struct __declspec(uuid("EFA008F9-F7A1-48BF-B05C-F224713CC0FF")) __declspec(novtable) IDWriteFontFallback : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall MapCharacters(IDWriteTextAnalysisSource* analysisSource,
        UINT32 textPosition, UINT32 textLength, IDWriteFontCollection* baseFontCollection,
        wchar_t const* baseFamilyName, DWRITE_FONT_WEIGHT baseWeight, DWRITE_FONT_STYLE baseStyle,
        DWRITE_FONT_STRETCH baseStretch, UINT32* mappedLength, IDWriteFont** mappedFont, FLOAT* scale) = 0;
};
typedef D3DCOLORVALUE DWRITE_COLOR_F;
struct __declspec(uuid("29748ed6-8c9c-4a6a-be0b-d912e8538944")) __declspec(novtable) IDWriteFont2 : IDWriteFont1 {
    MAICORD_COM_UNUSED(IsColorFont);
};
struct __declspec(uuid("d8b768ff-64bc-4e66-982b-ec8e87f693f7")) __declspec(novtable) IDWriteFontFace2 : IDWriteFontFace1 {
    MAICORD_COM_UNUSED(IsColorFont);
    MAICORD_COM_UNUSED(GetColorPaletteCount);
    MAICORD_COM_UNUSED(GetPaletteEntryCount);
    MAICORD_COM_UNUSED(GetPaletteEntries);
    virtual __declspec(nothrow) HRESULT __stdcall GetRecommendedRenderingMode(FLOAT fontEmSize, FLOAT dpiX, FLOAT dpiY,
        DWRITE_MATRIX const* transform, BOOL isSideways, DWRITE_OUTLINE_THRESHOLD outlineThreshold,
        DWRITE_MEASURING_MODE measuringMode, IDWriteRenderingParams* renderingParams,
        DWRITE_RENDERING_MODE* renderingMode, DWRITE_GRID_FIT_MODE* gridFitMode) = 0;
};
struct DWRITE_COLOR_GLYPH_RUN {
    DWRITE_GLYPH_RUN glyphRun;
    DWRITE_GLYPH_RUN_DESCRIPTION* glyphRunDescription;
    FLOAT baselineOriginX;
    FLOAT baselineOriginY;
    DWRITE_COLOR_F runColor;
    UINT16 paletteIndex;
};
struct __declspec(uuid("d31fbe17-f157-41a2-8d24-cb779e0560e8")) __declspec(novtable) IDWriteColorGlyphRunEnumerator : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall MoveNext(BOOL* hasRun) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetCurrentRun(DWRITE_COLOR_GLYPH_RUN const** colorGlyphRun) = 0;
};
struct __declspec(uuid("F9D711C3-9777-40AE-87E8-3E5AF9BF0948")) __declspec(novtable) IDWriteRenderingParams2 : IDWriteRenderingParams1 {
    MAICORD_COM_UNUSED(GetGridFitMode);
};
struct __declspec(uuid("0439fc60-ca44-4994-8dee-3a9af7b732ec")) __declspec(novtable) IDWriteFactory2 : IDWriteFactory1 {
    virtual __declspec(nothrow) HRESULT __stdcall GetSystemFontFallback(IDWriteFontFallback** fontFallback) = 0;
    MAICORD_COM_UNUSED(CreateFontFallbackBuilder);
    virtual __declspec(nothrow) HRESULT __stdcall TranslateColorGlyphRun(FLOAT baselineOriginX, FLOAT baselineOriginY,
        DWRITE_GLYPH_RUN const* glyphRun, DWRITE_GLYPH_RUN_DESCRIPTION const* glyphRunDescription,
        DWRITE_MEASURING_MODE measuringMode, DWRITE_MATRIX const* worldToDeviceTransform, UINT32 colorPaletteIndex,
        IDWriteColorGlyphRunEnumerator** colorLayers) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateCustomRenderingParams(FLOAT gamma, FLOAT enhancedContrast,
        FLOAT grayscaleEnhancedContrast, FLOAT clearTypeLevel, DWRITE_PIXEL_GEOMETRY pixelGeometry,
        DWRITE_RENDERING_MODE renderingMode, DWRITE_GRID_FIT_MODE gridFitMode,
        IDWriteRenderingParams2** renderingParams) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateGlyphRunAnalysis(DWRITE_GLYPH_RUN const* glyphRun,
        DWRITE_MATRIX const* transform, DWRITE_RENDERING_MODE renderingMode, DWRITE_MEASURING_MODE measuringMode,
        DWRITE_GRID_FIT_MODE gridFitMode, DWRITE_TEXT_ANTIALIAS_MODE antialiasMode, FLOAT baselineOriginX,
        FLOAT baselineOriginY, IDWriteGlyphRunAnalysis** glyphRunAnalysis) = 0;
};
// <dwrite_3.h>
enum DWRITE_FONT_PROPERTY_ID {
    DWRITE_FONT_PROPERTY_ID_NONE,
    DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME,
    DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME,
    DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FACE_NAME,
    DWRITE_FONT_PROPERTY_ID_FULL_NAME,
    DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME,
    DWRITE_FONT_PROPERTY_ID_POSTSCRIPT_NAME,
    DWRITE_FONT_PROPERTY_ID_DESIGN_SCRIPT_LANGUAGE_TAG,
    DWRITE_FONT_PROPERTY_ID_SUPPORTED_SCRIPT_LANGUAGE_TAG,
    DWRITE_FONT_PROPERTY_ID_SEMANTIC_TAG,
    DWRITE_FONT_PROPERTY_ID_WEIGHT,
    DWRITE_FONT_PROPERTY_ID_STRETCH,
    DWRITE_FONT_PROPERTY_ID_STYLE,
    DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FACE_NAME,
    DWRITE_FONT_PROPERTY_ID_TOTAL = DWRITE_FONT_PROPERTY_ID_STYLE + 1,
    DWRITE_FONT_PROPERTY_ID_TOTAL_RS3 = DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FACE_NAME + 1,
    DWRITE_FONT_PROPERTY_ID_PREFERRED_FAMILY_NAME = DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME,
    DWRITE_FONT_PROPERTY_ID_FAMILY_NAME = DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME,
    DWRITE_FONT_PROPERTY_ID_FACE_NAME = DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FACE_NAME
};
struct DWRITE_FONT_PROPERTY {
    DWRITE_FONT_PROPERTY_ID propertyId;
    WCHAR const* propertyValue;
    WCHAR const* localeName;
};
enum DWRITE_RENDERING_MODE1 {
    DWRITE_RENDERING_MODE1_DEFAULT = DWRITE_RENDERING_MODE_DEFAULT,
    DWRITE_RENDERING_MODE1_ALIASED = DWRITE_RENDERING_MODE_ALIASED,
    DWRITE_RENDERING_MODE1_GDI_CLASSIC = DWRITE_RENDERING_MODE_GDI_CLASSIC,
    DWRITE_RENDERING_MODE1_GDI_NATURAL = DWRITE_RENDERING_MODE_GDI_NATURAL,
    DWRITE_RENDERING_MODE1_NATURAL = DWRITE_RENDERING_MODE_NATURAL,
    DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC = DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
    DWRITE_RENDERING_MODE1_OUTLINE = DWRITE_RENDERING_MODE_OUTLINE,
    DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC_DOWNSAMPLED
};
struct __declspec(uuid("B7924BAA-391B-412A-8C5C-E44CC2D867DC")) __declspec(novtable) IDWriteRenderingParams3 : IDWriteRenderingParams2 {
};
struct __declspec(uuid("9A1B41C3-D3BB-466A-87FC-FE67556A3B65")) __declspec(novtable) IDWriteFactory3 : IDWriteFactory2 {
    virtual __declspec(nothrow) HRESULT __stdcall CreateGlyphRunAnalysis(DWRITE_GLYPH_RUN const* glyphRun,
        DWRITE_MATRIX const* transform, DWRITE_RENDERING_MODE1 renderingMode, DWRITE_MEASURING_MODE measuringMode,
        DWRITE_GRID_FIT_MODE gridFitMode, DWRITE_TEXT_ANTIALIAS_MODE antialiasMode, FLOAT baselineOriginX,
        FLOAT baselineOriginY, IDWriteGlyphRunAnalysis** glyphRunAnalysis) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateCustomRenderingParams(FLOAT gamma, FLOAT enhancedContrast,
        FLOAT grayscaleEnhancedContrast, FLOAT clearTypeLevel, DWRITE_PIXEL_GEOMETRY pixelGeometry,
        DWRITE_RENDERING_MODE1 renderingMode, DWRITE_GRID_FIT_MODE gridFitMode,
        IDWriteRenderingParams3** renderingParams) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontFaceReference(WCHAR const* filePath,
        FILETIME const* lastWriteTime, UINT32 faceIndex, DWRITE_FONT_SIMULATIONS fontSimulations,
        IDWriteFontFaceReference** fontFaceReference) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontFaceReference(IDWriteFontFile* fontFile, UINT32 faceIndex,
        DWRITE_FONT_SIMULATIONS fontSimulations, IDWriteFontFaceReference** fontFaceReference) = 0;
    MAICORD_COM_UNUSED(GetSystemFontSet);
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontSetBuilder(IDWriteFontSetBuilder** fontSetBuilder) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontCollectionFromFontSet(IDWriteFontSet* fontSet,
        IDWriteFontCollection1** fontCollection) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetSystemFontCollection(BOOL includeDownloadableFonts,
        IDWriteFontCollection1** fontCollection, BOOL checkForUpdates = 0) = 0;
    MAICORD_COM_UNUSED(GetFontDownloadQueue);
    using IDWriteFactory::GetSystemFontCollection;
};
struct __declspec(uuid("53585141-D9F8-4095-8321-D73CF6BD116B")) __declspec(novtable) IDWriteFontSet : IUnknown {
    MAICORD_COM_UNUSED(GetFontCount);
    MAICORD_COM_UNUSED(GetFontFaceReference);
    MAICORD_COM_UNUSED(FindFontFaceReference);
    MAICORD_COM_UNUSED(FindFontFace);
    virtual __declspec(nothrow) HRESULT __stdcall GetPropertyValues(UINT32 listIndex,
        DWRITE_FONT_PROPERTY_ID propertyId, BOOL* exists, IDWriteLocalizedStrings** values) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetPropertyValues(DWRITE_FONT_PROPERTY_ID propertyID,
        WCHAR const* preferredLocaleNames, IDWriteStringList** values) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetPropertyValues(DWRITE_FONT_PROPERTY_ID propertyID,
        IDWriteStringList** values) = 0;
    MAICORD_COM_UNUSED(GetPropertyOccurrenceCount);
    virtual __declspec(nothrow) HRESULT __stdcall GetMatchingFonts(DWRITE_FONT_PROPERTY const* properties,
        UINT32 propertyCount, IDWriteFontSet** filteredSet) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetMatchingFonts(WCHAR const* familyName,
        DWRITE_FONT_WEIGHT fontWeight, DWRITE_FONT_STRETCH fontStretch, DWRITE_FONT_STYLE fontStyle,
        IDWriteFontSet** filteredSet) = 0;
};
struct __declspec(uuid("2F642AFE-9C68-4F40-B8BE-457401AFCB3D")) __declspec(novtable) IDWriteFontSetBuilder : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall AddFontFaceReference(IDWriteFontFaceReference* fontFaceReference) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall AddFontFaceReference(IDWriteFontFaceReference* fontFaceReference,
        DWRITE_FONT_PROPERTY const* properties, UINT32 propertyCount) = 0;
    MAICORD_COM_UNUSED(AddFontSet);
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontSet(IDWriteFontSet** fontSet) = 0;
};
struct __declspec(uuid("53585141-D9F8-4095-8321-D73CF6BD116C")) __declspec(novtable) IDWriteFontCollection1 : IDWriteFontCollection {
    MAICORD_COM_UNUSED(GetFontSet);
    virtual __declspec(nothrow) HRESULT __stdcall GetFontFamily(UINT32 index, IDWriteFontFamily1** fontFamily) = 0;
    using IDWriteFontCollection::GetFontFamily;
};
struct __declspec(uuid("DA20D8EF-812A-4C43-9802-62EC4ABD7ADF")) __declspec(novtable) IDWriteFontFamily1 : IDWriteFontFamily {
    MAICORD_COM_UNUSED(GetFontLocality);
    virtual __declspec(nothrow) HRESULT __stdcall GetFont(UINT32 listIndex, IDWriteFont3** font) = 0;
    MAICORD_COM_UNUSED(GetFontFaceReference);
};
struct __declspec(uuid("5E7FA7CA-DDE3-424C-89F0-9FCD6FED58CD")) __declspec(novtable) IDWriteFontFaceReference : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontFace(IDWriteFontFace3** fontFace) = 0;
    MAICORD_COM_UNUSED(CreateFontFaceWithSimulations);
    virtual __declspec(nothrow) BOOL __stdcall Equals(IDWriteFontFaceReference* fontFaceReference) = 0;
    MAICORD_COM_UNUSED(GetFontFaceIndex);
    virtual __declspec(nothrow) DWRITE_FONT_SIMULATIONS __stdcall GetSimulations() = 0;
    MAICORD_COM_UNUSED(GetFontFile);
    MAICORD_COM_UNUSED(GetLocalFileSize);
    MAICORD_COM_UNUSED(GetFileSize);
    MAICORD_COM_UNUSED(GetFileTime);
    MAICORD_COM_UNUSED(GetLocality);
    MAICORD_COM_UNUSED(EnqueueFontDownloadRequest);
    MAICORD_COM_UNUSED(EnqueueCharacterDownloadRequest);
    MAICORD_COM_UNUSED(EnqueueGlyphDownloadRequest);
    MAICORD_COM_UNUSED(EnqueueFileFragmentDownloadRequest);
};
struct __declspec(uuid("29748ED6-8C9C-4A6A-BE0B-D912E8538944")) __declspec(novtable) IDWriteFont3 : IDWriteFont2 {
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontFace(IDWriteFontFace3** fontFace) = 0;
    virtual __declspec(nothrow) BOOL __stdcall Equals(IDWriteFont* font) = 0;
    MAICORD_COM_UNUSED(GetFontFaceReference);
    virtual __declspec(nothrow) BOOL __stdcall HasCharacter(UINT32 unicodeValue) = 0;
    using IDWriteFont::CreateFontFace;
    using IDWriteFont::HasCharacter;
};
struct __declspec(uuid("D37D7598-09BE-4222-A236-2081341CC1F2")) __declspec(novtable) IDWriteFontFace3 : IDWriteFontFace2 {
    MAICORD_COM_UNUSED(GetFontFaceReference);
    MAICORD_COM_UNUSED(GetPanose);
    virtual __declspec(nothrow) DWRITE_FONT_WEIGHT __stdcall GetWeight() = 0;
    MAICORD_COM_UNUSED(GetStretch);
    virtual __declspec(nothrow) DWRITE_FONT_STYLE __stdcall GetStyle() = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFamilyNames(IDWriteLocalizedStrings** names) = 0;
    MAICORD_COM_UNUSED(GetFaceNames);
    MAICORD_COM_UNUSED(GetInformationalStrings);
    virtual __declspec(nothrow) BOOL __stdcall HasCharacter(UINT32 unicodeValue) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetRecommendedRenderingMode(FLOAT fontEmSize, FLOAT dpiX, FLOAT dpiY,
        DWRITE_MATRIX const* transform, BOOL isSideways, DWRITE_OUTLINE_THRESHOLD outlineThreshold,
        DWRITE_MEASURING_MODE measuringMode, IDWriteRenderingParams* renderingParams,
        DWRITE_RENDERING_MODE1* renderingMode, DWRITE_GRID_FIT_MODE* gridFitMode) = 0;
    MAICORD_COM_UNUSED(IsCharacterLocal);
    MAICORD_COM_UNUSED(IsGlyphLocal);
    MAICORD_COM_UNUSED(AreCharactersLocal);
    MAICORD_COM_UNUSED(AreGlyphsLocal);
};
struct __declspec(uuid("CFEE3140-1157-47CA-8B85-31BFCF3F2D0E")) __declspec(novtable) IDWriteStringList : IUnknown {
    virtual __declspec(nothrow) UINT32 __stdcall GetCount() = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetLocaleNameLength(UINT32 listIndex, UINT32* length) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetLocaleName(UINT32 listIndex, WCHAR* localeName, UINT32 size) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetStringLength(UINT32 listIndex, UINT32* length) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetString(UINT32 listIndex, WCHAR* stringBuffer,
        UINT32 stringBufferSize) = 0;
};
enum DWRITE_FONT_LINE_GAP_USAGE {
    DWRITE_FONT_LINE_GAP_USAGE_DEFAULT,
    DWRITE_FONT_LINE_GAP_USAGE_DISABLED,
    DWRITE_FONT_LINE_GAP_USAGE_ENABLED
};
struct DWRITE_LINE_SPACING {
    DWRITE_LINE_SPACING_METHOD method;
    FLOAT height;
    FLOAT baseline;
    FLOAT leadingBefore;
    DWRITE_FONT_LINE_GAP_USAGE fontLineGapUsage;
};
struct __declspec(uuid("F67E0EDD-9E3D-4ECC-8C32-4183253DFE70")) __declspec(novtable) IDWriteTextFormat2 : IDWriteTextFormat1 {
    virtual __declspec(nothrow) HRESULT __stdcall SetLineSpacing(DWRITE_LINE_SPACING const* lineSpacingOptions) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetLineSpacing(DWRITE_LINE_SPACING* lineSpacingOptions) = 0;
    using IDWriteTextFormat1::GetLineSpacing;
};
struct DWRITE_COLOR_GLYPH_RUN1 : DWRITE_COLOR_GLYPH_RUN {
    DWRITE_GLYPH_IMAGE_FORMATS glyphImageFormat;
    DWRITE_MEASURING_MODE measuringMode;
};
struct __declspec(uuid("7C5F86DA-C7A1-4F05-B8E1-55A179FE5A35")) __declspec(novtable) IDWriteColorGlyphRunEnumerator1 : IDWriteColorGlyphRunEnumerator {
    virtual __declspec(nothrow) HRESULT __stdcall GetCurrentRun(DWRITE_COLOR_GLYPH_RUN1 const** colorGlyphRun) = 0;
    using IDWriteColorGlyphRunEnumerator::GetCurrentRun;
};
struct __declspec(uuid("27F2A904-4EB8-441D-9678-0563F53E3E2F")) __declspec(novtable) IDWriteFontFace4 : IDWriteFontFace3 {
    virtual __declspec(nothrow) DWRITE_GLYPH_IMAGE_FORMATS __stdcall GetGlyphImageFormats() = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetGlyphImageFormats(UINT16 glyphId, UINT32 pixelsPerEmFirst,
        UINT32 pixelsPerEmLast, DWRITE_GLYPH_IMAGE_FORMATS* glyphImageFormats) = 0;
    MAICORD_COM_UNUSED(GetGlyphImageData);
    MAICORD_COM_UNUSED(ReleaseGlyphImageData);
};
struct __declspec(uuid("4B0B5BD3-0797-4549-8AC5-FE915CC53856")) __declspec(novtable) IDWriteFactory4 : IDWriteFactory3 {
    virtual __declspec(nothrow) HRESULT __stdcall TranslateColorGlyphRun(D2D1_POINT_2F baselineOrigin,
        DWRITE_GLYPH_RUN const* glyphRun, DWRITE_GLYPH_RUN_DESCRIPTION const* glyphRunDescription,
        DWRITE_GLYPH_IMAGE_FORMATS desiredGlyphImageFormats, DWRITE_MEASURING_MODE measuringMode,
        DWRITE_MATRIX const* worldAndDpiTransform, UINT32 colorPaletteIndex,
        IDWriteColorGlyphRunEnumerator1** colorLayers) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall ComputeGlyphOrigins(DWRITE_GLYPH_RUN const* glyphRun,
        DWRITE_MEASURING_MODE measuringMode, D2D1_POINT_2F baselineOrigin, DWRITE_MATRIX const* worldAndDpiTransform,
        D2D1_POINT_2F* glyphOrigins) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall ComputeGlyphOrigins(DWRITE_GLYPH_RUN const* glyphRun,
        D2D1_POINT_2F baselineOrigin, D2D1_POINT_2F* glyphOrigins) = 0;
    using IDWriteFactory2::TranslateColorGlyphRun;
};
struct __declspec(uuid("3FF7715F-3CDC-4DC6-9B72-EC5621DCCAFD")) __declspec(novtable) IDWriteFontSetBuilder1 : IDWriteFontSetBuilder {
    MAICORD_COM_UNUSED(AddFontFile);
};
struct __declspec(uuid("958DB99A-BE2A-4F09-AF7D-65189803D1D3")) __declspec(novtable) IDWriteFactory5 : IDWriteFactory4 {
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontSetBuilder(IDWriteFontSetBuilder1** fontSetBuilder) = 0;
    MAICORD_COM_UNUSED(CreateInMemoryFontFileLoader);
    MAICORD_COM_UNUSED(CreateHttpFontFileLoader);
    MAICORD_COM_UNUSED(AnalyzeContainerType);
    MAICORD_COM_UNUSED(UnpackFontFile);
    using IDWriteFactory3::CreateFontSetBuilder;
};
enum DWRITE_FONT_AXIS_TAG : UINT32 {
    DWRITE_FONT_AXIS_TAG_WEIGHT = (static_cast < DWRITE_FONT_AXIS_TAG > (((static_cast < UINT32 > (static_cast < UINT8 > ('t')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('h')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('g')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('w'))))),
    DWRITE_FONT_AXIS_TAG_WIDTH = (static_cast < DWRITE_FONT_AXIS_TAG > (((static_cast < UINT32 > (static_cast < UINT8 > ('h')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('t')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('d')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('w'))))),
    DWRITE_FONT_AXIS_TAG_SLANT = (static_cast < DWRITE_FONT_AXIS_TAG > (((static_cast < UINT32 > (static_cast < UINT8 > ('t')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('n')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('s'))))),
    DWRITE_FONT_AXIS_TAG_OPTICAL_SIZE = (static_cast < DWRITE_FONT_AXIS_TAG > (((static_cast < UINT32 > (static_cast < UINT8 > ('z')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('s')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('p')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('o'))))),
    DWRITE_FONT_AXIS_TAG_ITALIC = (static_cast < DWRITE_FONT_AXIS_TAG > (((static_cast < UINT32 > (static_cast < UINT8 > ('l')) << 24) | (static_cast < UINT32 > (static_cast < UINT8 > ('a')) << 16) | (static_cast < UINT32 > (static_cast < UINT8 > ('t')) << 8) | static_cast < UINT32 > (static_cast < UINT8 > ('i')))))
};
struct DWRITE_FONT_AXIS_VALUE {
    DWRITE_FONT_AXIS_TAG axisTag;
    FLOAT value;
};
struct DWRITE_FONT_AXIS_RANGE {
    DWRITE_FONT_AXIS_TAG axisTag;
    FLOAT minValue;
    FLOAT maxValue;
};
enum DWRITE_FONT_FAMILY_MODEL {
    DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC,
    DWRITE_FONT_FAMILY_MODEL_WEIGHT_STRETCH_STYLE
};
struct __declspec(uuid("F3744D80-21F7-42EB-B35D-995BC72FC223")) __declspec(novtable) IDWriteFactory6 : IDWriteFactory5 {
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontFaceReference(IDWriteFontFile* fontFile, UINT32 faceIndex,
        DWRITE_FONT_SIMULATIONS fontSimulations, DWRITE_FONT_AXIS_VALUE const* fontAxisValues,
        UINT32 fontAxisValueCount, IDWriteFontFaceReference1** fontFaceReference) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontResource(IDWriteFontFile* fontFile, UINT32 faceIndex,
        IDWriteFontResource** fontResource) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetSystemFontSet(BOOL includeDownloadableFonts,
        IDWriteFontSet1** fontSet) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetSystemFontCollection(BOOL includeDownloadableFonts,
        DWRITE_FONT_FAMILY_MODEL fontFamilyModel, IDWriteFontCollection2** fontCollection) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontCollectionFromFontSet(IDWriteFontSet* fontSet,
        DWRITE_FONT_FAMILY_MODEL fontFamilyModel, IDWriteFontCollection2** fontCollection) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontSetBuilder(IDWriteFontSetBuilder2** fontSetBuilder) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateTextFormat(WCHAR const* fontFamilyName,
        IDWriteFontCollection* fontCollection, DWRITE_FONT_AXIS_VALUE const* fontAxisValues, UINT32 fontAxisValueCount,
        FLOAT fontSize, WCHAR const* localeName, IDWriteTextFormat3** textFormat) = 0;
    using IDWriteFactory5::CreateFontFaceReference;
    using IDWriteFactory3::GetSystemFontCollection;
    using IDWriteFactory5::CreateFontCollectionFromFontSet;
    using IDWriteFactory3::CreateFontSetBuilder;
    using IDWriteFactory5::CreateFontSetBuilder;
    using IDWriteFactory::CreateTextFormat;
};
struct __declspec(uuid("98EFF3A5-B667-479A-B145-E2FA5B9FDC29")) __declspec(novtable) IDWriteFontFace5 : IDWriteFontFace4 {
    virtual __declspec(nothrow) UINT32 __stdcall GetFontAxisValueCount() = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFontAxisValues(DWRITE_FONT_AXIS_VALUE* fontAxisValues,
        UINT32 fontAxisValueCount) = 0;
    MAICORD_COM_UNUSED(HasVariations);
    virtual __declspec(nothrow) HRESULT __stdcall GetFontResource(IDWriteFontResource** fontResource) = 0;
    virtual __declspec(nothrow) BOOL __stdcall Equals(IDWriteFontFace* fontFace) = 0;
};
struct __declspec(uuid("1F803A76-6871-48E8-987F-B975551C50F2")) __declspec(novtable) IDWriteFontResource : IUnknown {
    MAICORD_COM_UNUSED(GetFontFile);
    MAICORD_COM_UNUSED(GetFontFaceIndex);
    MAICORD_COM_UNUSED(GetFontAxisCount);
    MAICORD_COM_UNUSED(GetDefaultFontAxisValues);
    MAICORD_COM_UNUSED(GetFontAxisRanges);
    MAICORD_COM_UNUSED(GetFontAxisAttributes);
    MAICORD_COM_UNUSED(GetAxisNames);
    MAICORD_COM_UNUSED(GetAxisValueNameCount);
    MAICORD_COM_UNUSED(GetAxisValueNames);
    MAICORD_COM_UNUSED(HasVariations);
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontFace(DWRITE_FONT_SIMULATIONS fontSimulations,
        DWRITE_FONT_AXIS_VALUE const* fontAxisValues, UINT32 fontAxisValueCount, IDWriteFontFace5** fontFace) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontFaceReference(DWRITE_FONT_SIMULATIONS fontSimulations,
        DWRITE_FONT_AXIS_VALUE const* fontAxisValues, UINT32 fontAxisValueCount,
        IDWriteFontFaceReference1** fontFaceReference) = 0;
};
struct __declspec(uuid("C081FE77-2FD1-41AC-A5A3-34983C4BA61A")) __declspec(novtable) IDWriteFontFaceReference1 : IDWriteFontFaceReference {
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontFace(IDWriteFontFace5** fontFace) = 0;
    virtual __declspec(nothrow) UINT32 __stdcall GetFontAxisValueCount() = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFontAxisValues(DWRITE_FONT_AXIS_VALUE* fontAxisValues,
        UINT32 fontAxisValueCount) = 0;
    using IDWriteFontFaceReference::CreateFontFace;
};
struct __declspec(uuid("EE5BA612-B131-463C-8F4F-3189B9401E45")) __declspec(novtable) IDWriteFontSetBuilder2 : IDWriteFontSetBuilder1 {
};
struct __declspec(uuid("7E9FDA85-6C92-4053-BC47-7AE3530DB4D3")) __declspec(novtable) IDWriteFontSet1 : IDWriteFontSet {
    MAICORD_COM_UNUSED(GetMatchingFonts);
    MAICORD_COM_UNUSED(GetFirstFontResources);
    virtual __declspec(nothrow) HRESULT __stdcall GetFilteredFonts(DWRITE_FONT_PROPERTY const* properties,
        UINT32 propertyCount, BOOL selectAnyProperty, IDWriteFontSet1** filteredFontSet) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFilteredFonts(DWRITE_FONT_AXIS_RANGE const* fontAxisRanges,
        UINT32 fontAxisRangeCount, BOOL selectAnyRange, IDWriteFontSet1** filteredFontSet) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFilteredFonts(UINT32 const* indices, UINT32 indexCount,
        IDWriteFontSet1** filteredFontSet) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFilteredFontIndices(DWRITE_FONT_PROPERTY const* properties,
        UINT32 propertyCount, BOOL selectAnyProperty, UINT32* indices, UINT32 maxIndexCount,
        UINT32* actualIndexCount) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFilteredFontIndices(DWRITE_FONT_AXIS_RANGE const* fontAxisRanges,
        UINT32 fontAxisRangeCount, BOOL selectAnyRange, UINT32* indices, UINT32 maxIndexCount,
        UINT32* actualIndexCount) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFontAxisRanges(DWRITE_FONT_AXIS_RANGE* fontAxisRanges,
        UINT32 maxFontAxisRangeCount, UINT32* actualFontAxisRangeCount) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFontAxisRanges(UINT32 listIndex,
        DWRITE_FONT_AXIS_RANGE* fontAxisRanges, UINT32 maxFontAxisRangeCount, UINT32* actualFontAxisRangeCount) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFontFaceReference(UINT32 listIndex,
        IDWriteFontFaceReference1** fontFaceReference) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontResource(UINT32 listIndex,
        IDWriteFontResource** fontResource) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall CreateFontFace(UINT32 listIndex, IDWriteFontFace5** fontFace) = 0;
};
struct __declspec(uuid("3ED49E77-A398-4261-B9CF-C126C2131EF3")) __declspec(novtable) IDWriteFontFamily2 : IDWriteFontFamily1 {
};
struct __declspec(uuid("514039C6-4617-4064-BF8B-92EA83E506E0")) __declspec(novtable) IDWriteFontCollection2 : IDWriteFontCollection1 {
    virtual __declspec(nothrow) HRESULT __stdcall GetFontFamily(UINT32 index, IDWriteFontFamily2** fontFamily) = 0;
    using IDWriteFontCollection::GetFontFamily;
    using IDWriteFontCollection1::GetFontFamily;
};
struct __declspec(uuid("6D3B5641-E550-430D-A85B-B7BF48A93427")) __declspec(novtable) IDWriteTextFormat3 : IDWriteTextFormat2 {
    MAICORD_COM_UNUSED(SetFontAxisValues);
    virtual __declspec(nothrow) UINT32 __stdcall GetFontAxisValueCount() = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFontAxisValues(DWRITE_FONT_AXIS_VALUE* fontAxisValues,
        UINT32 fontAxisValueCount) = 0;
};
struct __declspec(uuid("C4B1FE1B-6E84-47D5-B54C-A597981B06AD")) __declspec(novtable) IDWriteFontFace6 : IDWriteFontFace5 {
    virtual __declspec(nothrow) HRESULT __stdcall GetFamilyNames(DWRITE_FONT_FAMILY_MODEL fontFamilyModel,
        IDWriteLocalizedStrings** names) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall GetFaceNames(DWRITE_FONT_FAMILY_MODEL fontFamilyModel,
        IDWriteLocalizedStrings** names) = 0;
    using IDWriteFontFace3::GetFamilyNames;
};
enum DWRITE_PAINT_FEATURE_LEVEL : INT32 {
    DWRITE_PAINT_FEATURE_LEVEL_NONE = 0,
    DWRITE_PAINT_FEATURE_LEVEL_COLR_V0 = 1,
    DWRITE_PAINT_FEATURE_LEVEL_COLR_V1 = 2
};
enum DWRITE_PAINT_ATTRIBUTES {
    DWRITE_PAINT_ATTRIBUTES_NONE = 0,
    DWRITE_PAINT_ATTRIBUTES_USES_PALETTE = 0x01,
    DWRITE_PAINT_ATTRIBUTES_USES_TEXT_COLOR = 0x02
};
struct DWRITE_PAINT_COLOR {
    DWRITE_COLOR_F value;
    UINT16 paletteEntryIndex;
    float alphaMultiplier;
    DWRITE_PAINT_ATTRIBUTES colorAttributes;
};
enum DWRITE_COLOR_COMPOSITE_MODE {
    DWRITE_COLOR_COMPOSITE_CLEAR,
    DWRITE_COLOR_COMPOSITE_SRC,
    DWRITE_COLOR_COMPOSITE_DEST,
    DWRITE_COLOR_COMPOSITE_SRC_OVER,
    DWRITE_COLOR_COMPOSITE_DEST_OVER,
    DWRITE_COLOR_COMPOSITE_SRC_IN,
    DWRITE_COLOR_COMPOSITE_DEST_IN,
    DWRITE_COLOR_COMPOSITE_SRC_OUT,
    DWRITE_COLOR_COMPOSITE_DEST_OUT,
    DWRITE_COLOR_COMPOSITE_SRC_ATOP,
    DWRITE_COLOR_COMPOSITE_DEST_ATOP,
    DWRITE_COLOR_COMPOSITE_XOR,
    DWRITE_COLOR_COMPOSITE_PLUS,
    DWRITE_COLOR_COMPOSITE_SCREEN,
    DWRITE_COLOR_COMPOSITE_OVERLAY,
    DWRITE_COLOR_COMPOSITE_DARKEN,
    DWRITE_COLOR_COMPOSITE_LIGHTEN,
    DWRITE_COLOR_COMPOSITE_COLOR_DODGE,
    DWRITE_COLOR_COMPOSITE_COLOR_BURN,
    DWRITE_COLOR_COMPOSITE_HARD_LIGHT,
    DWRITE_COLOR_COMPOSITE_SOFT_LIGHT,
    DWRITE_COLOR_COMPOSITE_DIFFERENCE,
    DWRITE_COLOR_COMPOSITE_EXCLUSION,
    DWRITE_COLOR_COMPOSITE_MULTIPLY,
    DWRITE_COLOR_COMPOSITE_HSL_HUE,
    DWRITE_COLOR_COMPOSITE_HSL_SATURATION,
    DWRITE_COLOR_COMPOSITE_HSL_COLOR,
    DWRITE_COLOR_COMPOSITE_HSL_LUMINOSITY
};
enum DWRITE_PAINT_TYPE {
    DWRITE_PAINT_TYPE_NONE,
    DWRITE_PAINT_TYPE_LAYERS,
    DWRITE_PAINT_TYPE_SOLID_GLYPH,
    DWRITE_PAINT_TYPE_SOLID,
    DWRITE_PAINT_TYPE_LINEAR_GRADIENT,
    DWRITE_PAINT_TYPE_RADIAL_GRADIENT,
    DWRITE_PAINT_TYPE_SWEEP_GRADIENT,
    DWRITE_PAINT_TYPE_GLYPH,
    DWRITE_PAINT_TYPE_COLOR_GLYPH,
    DWRITE_PAINT_TYPE_TRANSFORM,
    DWRITE_PAINT_TYPE_COMPOSITE
};
struct DWRITE_PAINT_ELEMENT {
    DWRITE_PAINT_TYPE paintType;
    union PAINT_UNION {
        struct PAINT_LAYERS {
            UINT32 childCount;
        } layers;
        struct PAINT_SOLID_GLYPH {
            UINT32 glyphIndex;
            DWRITE_PAINT_COLOR color;
        } solidGlyph;
        DWRITE_PAINT_COLOR solid;
        struct PAINT_LINEAR_GRADIENT {
            UINT32 extendMode;
            UINT32 gradientStopCount;
            float x0;
            float y0;
            float x1;
            float y1;
            float x2;
            float y2;
        } linearGradient;
        struct PAINT_RADIAL_GRADIENT {
            UINT32 extendMode;
            UINT32 gradientStopCount;
            float x0;
            float y0;
            float radius0;
            float x1;
            float y1;
            float radius1;
        } radialGradient;
        struct PAINT_SWEEP_GRADIENT {
            UINT32 extendMode;
            UINT32 gradientStopCount;
            float centerX;
            float centerY;
            float startAngle;
            float endAngle;
        } sweepGradient;
        struct PAINT_GLYPH {
            UINT32 glyphIndex;
        } glyph;
        struct PAINT_COLOR_GLYPH {
            UINT32 glyphIndex;
            D2D_RECT_F clipBox;
        } colorGlyph;
        DWRITE_MATRIX transform;
        struct PAINT_COMPOSITE {
            DWRITE_COLOR_COMPOSITE_MODE mode;
        } composite;
    } paint;
};
struct __declspec(uuid("8128E912-3B97-42A5-AB6C-24AAD3A86E54")) __declspec(novtable) IDWritePaintReader : IUnknown {
    virtual __declspec(nothrow) HRESULT __stdcall SetCurrentGlyph(UINT32 glyphIndex,
        DWRITE_PAINT_ELEMENT* paintElement, UINT32 structSize, D2D_RECT_F* clipBox,
        DWRITE_PAINT_ATTRIBUTES* glyphAttributes = nullptr) = 0;
    virtual __declspec(nothrow) HRESULT __stdcall SetTextColor(DWRITE_COLOR_F const& textColor) = 0;
    HRESULT SetCurrentGlyph(UINT32 glyphIndex, DWRITE_PAINT_ELEMENT* paintElement, D2D_RECT_F* clipBox, DWRITE_PAINT_ATTRIBUTES* glyphAttributes = nullptr) { return SetCurrentGlyph(glyphIndex, paintElement, sizeof(DWRITE_PAINT_ELEMENT), clipBox, glyphAttributes); }
};
struct __declspec(uuid("3945B85B-BC95-40F7-B72C-8B73BFC7E13B")) __declspec(novtable) IDWriteFontFace7 : IDWriteFontFace6 {
    MAICORD_COM_UNUSED(GetPaintFeatureLevel);
    virtual __declspec(nothrow) HRESULT __stdcall CreatePaintReader(DWRITE_GLYPH_IMAGE_FORMATS glyphImageFormat,
        DWRITE_PAINT_FEATURE_LEVEL paintFeatureLevel, IDWritePaintReader** paintReader) = 0;
};
MAICORD_SYS_TYPES_END

// <DWrite.h>
extern "C" HRESULT __declspec(dllimport) WINAPI DWriteCreateFactory(DWRITE_FACTORY_TYPE factoryType, const IID& iid,
    IUnknown** factory) noexcept;
