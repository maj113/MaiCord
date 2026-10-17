#pragma once
#include "maicord/text/font_catalog.hpp"
#include "maicord/text/font_fallback.hpp"
namespace maicord {
// Resolves a WWS family and publishes catalog candidate indices in fallback order.
// Scratch, output, name and count are disjoint. The catalog is immutable; a retained
// order is valid only for that catalog and requested properties. A missing family
// returns unsupported_sequence. Failure clears count; scratch/output may change.
// Work counts visited faces, gathered candidates and preference ordering.
SfntStatus native_font_catalog_fallback_order(const NativeFontCatalog&,ConstSlice<u16> name,NativeFontProperties,
    Slice<NativeFontProperties> properties_scratch,Slice<u32> indices_scratch,Slice<u32> order,u32* count,
    u32 maximum_faces=65536,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
}
