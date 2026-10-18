#pragma once
#include "maicord/render/colr_source.hpp"
#include "maicord/text/truetype_outline.hpp"
#include "maicord/text/cff_outline.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_types.hpp"

// QA owner only. The engine receives immutable views and caller-owned scratch;
// font mapping and these maximum-capacity buffers are not a shipping cache.
struct NativePaintContext {
    HANDLE file = INVALID_HANDLE_VALUE, mapping = nullptr;
    const void* bytes = nullptr;
    maicord::SfntFace face{};
    maicord::SfntTrueType tt{};
    maicord::CffFont cff{};
    maicord::ColrFont color{};
    maicord::ColrVariationData variation_data{};
    maicord::ColrVariationInstance instance{};
    maicord::ColrNativeSource cursor{};
    maicord::SfntOutlinePoint points[65536]{};
    maicord::u32 ends[32768]{};
    maicord::u16 active[64]{};
    double scalars[32767]{};
    maicord::ColrResolvedColorStop stops[65535]{};
    maicord::ColrSourceFrame frames[65]{};
    bool is_tt = false;
    bool open(const wchar_t* path) noexcept;
    bool build(maicord::ColrPaintProgram* program,maicord::u16 glyph,float em) noexcept;
    void close() noexcept;
};
