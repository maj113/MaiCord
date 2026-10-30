#include "maicord/text/grapheme.hpp"

namespace maicord {
namespace {
using G=UnicodeGraphemeBreak;
using I=UnicodeIndicConjunctBreak;
constexpr u8 ri_odd=1,pictograph_extend=2,pictograph_zwj=4;
// A zero state must act as the start of text. GB1 and GB4 both break, and a
// Control leaves no flags or Indic state behind, so a zero state, whose previous
// break class is Control, decides every rule as the start of text would.
static_assert(static_cast<u8>(G::cn)==0);
bool control(G value) noexcept {return value==G::cn || value==G::cr || value==G::lf;}
}
bool unicode_grapheme_boundary(UnicodeGraphemeState& state,u32 scalar) noexcept {
    const G previous=state.previous;
    // Printable ASCII has Other/None/non-pictographic properties. GB9b is
    // still relevant after Prepend; every other incoming state breaks here.
    // This transition also clears RI, emoji-ZWJ and Indic lookbehind state.
    if(scalar>=32&&scalar<=126){state.previous=G::xx;state.flags=0;state.indic=0;return previous!=G::pp;}
    const auto& properties=*unicode_properties(scalar);
    const G current=properties.grapheme_break();
    const I indic=properties.indic_conjunct_break();
    const bool pictograph=properties.has(UnicodeFlag::extended_pictographic);
    bool split=true;
    if(previous==G::cr && current==G::lf)split=false; // GB3
    else if(control(previous) || control(current))split=true; // GB4/GB5
    else if(previous==G::l && (current==G::l || current==G::v || current==G::lv || current==G::lvt))split=false; // GB6
    else if((previous==G::lv || previous==G::v) && (current==G::v || current==G::t))split=false; // GB7
    else if((previous==G::lvt || previous==G::t) && current==G::t)split=false; // GB8
    else if(current==G::ex || current==G::zwj || current==G::sm || previous==G::pp)split=false; // GB9/9a/9b
    else if(indic==I::consonant && state.indic==2)split=false; // GB9c
    else if(pictograph && (state.flags&pictograph_zwj))split=false; // GB11
    else if(previous==G::ri && current==G::ri && (state.flags&ri_odd))split=false; // GB12/13
    u8 flags=0;
    if(current==G::ri && !(state.flags&ri_odd))flags|=ri_odd;
    if(pictograph || (current==G::ex && (state.flags&pictograph_extend)))flags|=pictograph_extend;
    if(current==G::zwj && (state.flags&pictograph_extend))flags|=pictograph_zwj;
    if(indic==I::consonant)state.indic=1;
    else if(indic==I::linker && state.indic)state.indic=2;
    else if(indic!=I::extend && indic!=I::linker)state.indic=0;
    state.previous=current;state.flags=flags;return split;
}
} // namespace maicord
