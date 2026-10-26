#pragma once
#include "maicord/text/ascii_run.hpp"
namespace maicord {
// Internal publication of an already-shaped run. Only exact-prefix aliases of
// corresponding rich buffers are supported; other overlaps violate the contract.
SfntStatus compact_shape_result(const NativeAsciiStorage&,const SfntShapeRunResult&,NativeAsciiResult*) noexcept;
// Decodes text into the component storage, which script frontends borrow for
// scalars until shaping opens the component forest.
SfntStatus decode_run_scalars(const Utf8View& text,const SfntShapeStorage& storage,ConstSlice<u32>& scalars) noexcept;
// Shared frontend of the script run wrappers: validates the text, component
// storage, spacing and text budget, then decodes the scalars unless the caller
// passes none (a path that shapes from the text itself).
SfntStatus script_run_begin(const Utf8View& text,const SfntShapeStorage& storage,const SfntShapeLimits& limits,float spacing,ConstSlice<u32>* scalars) noexcept;
// Shared tail: applies trailing spacing to a successfully shaped run and
// clears the counts when that fails.
SfntStatus script_run_finish(SfntStatus status,const SfntShapeStorage& storage,SfntShapeRunResult* result,const SfntShapeLimits& limits,float spacing,bool rtl) noexcept;
// Validates the borrowed compact output views and clears the result; every
// compact script wrapper calls this before shaping.
SfntStatus compact_run_begin(const NativeAsciiStorage&,NativeAsciiResult*) noexcept;
// Shapes through `run(storage.shaping,&shaped)` and publishes the compact result.
template<class Run> inline SfntStatus compact_run(const NativeAsciiStorage& storage,NativeAsciiResult* result,Run run) noexcept {
    const auto begin=compact_run_begin(storage,result);if(begin!=SfntStatus::ok)return begin;
    SfntShapeRunResult shaped{};const auto status=run(storage.shaping,&shaped);
    return status==SfntStatus::ok?compact_shape_result(storage,shaped,result):status;
}
}
