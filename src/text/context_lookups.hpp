#pragma once
#include "maicord/text/context.hpp"
namespace maicord {
using SfntLookupVisitor=SfntStatus(*)(void*,u16) noexcept;
// Structural/action traversal only; selected payloads still require context_open.
SfntStatus sfnt_context_visit_lookups(ConstSlice<u8>,SfntContextKind,u16 lookup_limit,
    SfntLookupVisitor,void*,u32 work,u32* used) noexcept;
}
