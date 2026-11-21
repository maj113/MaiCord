#include "maicord/text/unicode/grapheme.hpp"

namespace maicord {
namespace {
using G=UnicodeGraphemeBreak;
using I=UnicodeIndicConjunctBreak;
constexpr u8 ri_odd=1,pictograph_extend=2,pictograph_zwj=4;
// A zero state must act as the start of text. GB1 and GB4 both break, and a
// Control leaves no flags or Indic state behind, so a zero state, whose previous
// break class is Control, decides every rule as the start of text would.
static_assert(static_cast<u8>(G::cn)==0);
constexpr bool control(G value) noexcept {return value==G::cn || value==G::cr || value==G::lf;}
constexpr u32 classes=static_cast<u32>(G::zwj)+1;
// GB3 to GB9b read only the two break classes. Per previous class, the bits of the classes that they join to it
// and of those that GB4 and GB5 cut from it; the other pairs go to the rules that read the state.
struct PairRules {u16 joined[classes],cut[classes];};
constexpr PairRules pair_rules=[]() constexpr noexcept {
    PairRules rules{};
    for(u32 a=0;a<classes;++a)for(u32 b=0;b<classes;++b){
        const G previous=static_cast<G>(a),current=static_cast<G>(b);const u16 bit=static_cast<u16>(1u<<b);
        if(previous==G::cr && current==G::lf)rules.joined[a]|=bit; // GB3
        else if(control(previous) || control(current))rules.cut[a]|=bit; // GB4/GB5
        else if((previous==G::l && (current==G::l || current==G::v || current==G::lv || current==G::lvt)) || // GB6
                ((previous==G::lv || previous==G::v) && (current==G::v || current==G::t)) || // GB7
                ((previous==G::lvt || previous==G::t) && current==G::t) || // GB8
                current==G::ex || current==G::zwj || current==G::sm || previous==G::pp)rules.joined[a]|=bit; // GB9/9a/9b
    }
    return rules;
}();
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
    const u32 bit=1u<<static_cast<u32>(current);
    bool split=!(pair_rules.joined[static_cast<u32>(previous)]&bit);
    if(split && !(pair_rules.cut[static_cast<u32>(previous)]&bit))
        split=!((indic==I::consonant && state.indic==2) || // GB9c
                (pictograph && (state.flags&pictograph_zwj)) || // GB11
                (previous==G::ri && current==G::ri && (state.flags&ri_odd))); // GB12/13
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
