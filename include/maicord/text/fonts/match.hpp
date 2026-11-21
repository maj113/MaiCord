#pragma once
#include "maicord/text/base.hpp"
namespace maicord {
// Compact WWS properties of an unsimulated physical face or named instance.
// Family membership, stable catalog ordering and named axes are caller-owned.
// Style: 0=normal, 1=oblique, 2=italic; width uses OS/2 classes 1..9.
struct NativeFontProperties {u16 weight;u8 width,style;};
constexpr bool operator==(NativeFontProperties a,NativeFontProperties b) noexcept {return a.weight==b.weight&&a.width==b.width&&a.style==b.style;}
struct NativeFontMatch {u32 index;NativeFontProperties properties;u32 simulations;};
constexpr bool native_font_properties_valid(NativeFontProperties p) noexcept {return p.weight&&p.weight<=1000&&p.width&&p.width<=9&&p.style<=2;}
// Select one face within an already resolved WWS family. The result retains the
// source index and reports the required simulations (bold=1, oblique=2) with the
// simulated properties. Equal candidates keep input order. Candidates must be
// nonempty and all properties valid; the caller validates them once.
NativeFontMatch native_font_match(ConstSlice<NativeFontProperties> candidates,NativeFontProperties requested) noexcept;
// native_font_match over the one candidate `face`, without the ranking that only orders several candidates: index 0,
// the simulations the request needs and the simulated properties.
constexpr NativeFontMatch native_font_match_one(NativeFontProperties face,NativeFontProperties requested) noexcept {
    u32 simulations=0;
    if(requested.style&&!face.style){face.style=1;simulations|=2;}
    if(requested.weight>=600&&face.weight<=550&&requested.weight>static_cast<u32>(face.weight)+100){face.weight=700;simulations|=1;}
    return {0,face,simulations};
}
// Fallback preference order over one resolved family: writes the candidate
// indices (candidates.count of them) best first. Ranks geometric distance after
// the required simulations, then breaks ties by simulation flags and ascending
// original weight; equal candidates keep source order. Candidates must be valid.
void native_font_fallback_order(ConstSlice<NativeFontProperties> candidates,NativeFontProperties requested,u32* order) noexcept;
static_assert(sizeof(NativeFontProperties)==4);
static_assert(sizeof(NativeFontMatch)==12);
}
