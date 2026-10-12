#pragma once
#include "maicord/text/font_catalog.hpp"
#include "maicord/text/font_prepare.hpp"
namespace maicord {
// Prepare a catalog-selected candidate in a growing native session: one file
// acquisition, signature validation and instance preparation with the
// candidate's axes. The catalog is only borrowed for this call. The returned
// instance has its own reference to the face mapping; the caller keeps the
// session alive. On any failure, outputs clear and the temporary face reference
// is released; session pools can have grown, and existing live handles remain
// valid. Scratch appends as in native_font_prepare and must not overlap catalog
// storage or outputs. Simulation rules match native_font_prepare.
NativeFontResult native_font_catalog_prepare(NativeFontSession*,const NativeFontCatalog&,u32 candidate,
    u32 simulations,bool outline_instructions,Arena*,NativeFontInstanceHandle*,bool* oblique=nullptr,bool* bold=nullptr) noexcept;
}
