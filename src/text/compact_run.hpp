#pragma once
#include "maicord/text/ascii_run.hpp"
namespace maicord {
// Internal publication of an already-shaped run. Only exact-prefix aliases of
// corresponding rich buffers are supported; other overlaps violate the contract.
SfntStatus compact_shape_result(const NativeAsciiStorage&,const SfntShapeRunResult&,NativeAsciiResult*) noexcept;
// Decodes text into the component storage, which script frontends borrow for
// scalars until shaping opens the component forest.
SfntStatus decode_run_scalars(Utf8View text,const SfntShapeStorage& storage,ConstSlice<u32>& scalars) noexcept;
}
