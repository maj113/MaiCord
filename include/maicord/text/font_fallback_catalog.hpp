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
// Order must be a successful unchanged result from the function above. Callback
// tokens are catalog candidate indices; the callback can borrow cached native cmaps
// or lazily map a face and keep the selected handle in its own context (this call
// retains nothing). A normal miss is ok with candidate=UINT_MAX; errors clear output
// to the same sentinel. Properties/simulations are published only after successful
// coverage. Work counts candidate attempts, not callback internals. Other-family and
// locale policy and text-range boundaries remain the caller's.
SfntStatus native_font_catalog_fallback_select(const NativeFontCatalog&,ConstSlice<u32> order,NativeFontProperties,
    u32 scalar,NativeFallbackCoverage,void* context,NativeFontCatalogMatch*,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
}
