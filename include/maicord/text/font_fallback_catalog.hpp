#pragma once
#include "maicord/text/font_catalog.hpp"
#include "maicord/text/font_fallback.hpp"
namespace maicord {
// Resolve a WWS family and publish catalog candidate indices in fallback order.
// No mapping, instance preparation or allocation. Scratch/output/name/count are
// disjoint. The catalog is immutable; a retained order is valid only for that
// catalog and requested properties. Missing family returns unsupported_sequence.
// Failure clears count; scratch/output may change. Work includes visited faces,
// gathered candidates and preference ordering (not OS ordinal name comparison).
SfntStatus native_font_catalog_fallback_order(const NativeFontCatalog&,ConstSlice<u16> name,NativeFontProperties,
    Slice<NativeFontProperties> properties_scratch,Slice<u32> indices_scratch,Slice<u32> order,u32* count,
    u32 maximum_faces=65536,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
// Order must be a successful unchanged result above. Callback tokens are catalog
// candidate indices. It can borrow cached native cmaps or lazily map a face and
// retain the selected handle in its own context. This operation retains nothing.
// Normal miss is ok with candidate=UINT_MAX; errors clear output to the same
// sentinel. Properties/simulations publish only after successful coverage.
// Work counts candidate attempts; callback-internal work is caller-controlled.
// Other-family/locale policy and text-range boundaries remain caller obligations.
SfntStatus native_font_catalog_fallback_select(const NativeFontCatalog&,ConstSlice<u32> order,NativeFontProperties,
    u32 scalar,NativeFallbackCoverage,void* context,NativeFontCatalogMatch*,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
}
