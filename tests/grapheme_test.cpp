#include "maicord/text/grapheme.hpp"

namespace maicord {
namespace {
#include "generated/grapheme_break_test.inc"
bool vectors() noexcept {
    u32 at=0,cases=0;
    while(at<sizeof(grapheme_vectors)/sizeof(grapheme_vectors[0])) {
        const u32 count=grapheme_vectors[at++];UnicodeGraphemeState state{};
        if(count>sizeof(grapheme_vectors)/sizeof(grapheme_vectors[0])-at)return false;
        for(u32 i=0;i<count;++i) {
            const u32 value=grapheme_vectors[at++];
            if(unicode_grapheme_boundary(state,value&0x1fffff)!=((value&0x80000000)!=0))return false;
        }
        ++cases;
    }
    return cases==grapheme_vector_count;
}
bool long_contexts() noexcept {
    UnicodeGraphemeState state{};
    if(!unicode_grapheme_boundary(state,0x1f469))return false;
    for(u32 i=0;i<100000;++i)if(unicode_grapheme_boundary(state,0x301))return false;
    if(unicode_grapheme_boundary(state,0x200d)||unicode_grapheme_boundary(state,0x1f4bb))return false;
    state={};
    for(u32 i=0;i<100001;++i)if(unicode_grapheme_boundary(state,0x1f1e6)!=(i%2==0))return false;
    state={};
    if(!unicode_grapheme_boundary(state,0x915)||unicode_grapheme_boundary(state,0x94d))return false;
    for(u32 i=0;i<100000;++i)if(unicode_grapheme_boundary(state,0x93c))return false;
    return !unicode_grapheme_boundary(state,0x915);
}
bool ascii_equivalence() noexcept {
    // Substitute each printable ASCII scalar for every property-equivalent
    // scalar in the official vectors. Their independent boundary answers are
    // unchanged, including Prepend, controls and lookbehind-state transitions.
    for(u32 ascii=32;ascii<=126;++ascii){const auto* p=unicode_properties(ascii);
        if(p->grapheme_break()!=UnicodeGraphemeBreak::xx||p->indic_conjunct_break()!=UnicodeIndicConjunctBreak::none||p->has(UnicodeFlag::extended_pictographic))return false;
        u32 at=0;while(at<sizeof(grapheme_vectors)/sizeof(grapheme_vectors[0])){const u32 count=grapheme_vectors[at++];UnicodeGraphemeState state{};
            for(u32 i=0;i<count;++i){const u32 value=grapheme_vectors[at++];u32 scalar=value&0x1fffff;const auto* properties=unicode_properties(scalar);
                if(properties->grapheme_break()==UnicodeGraphemeBreak::xx&&properties->indic_conjunct_break()==UnicodeIndicConjunctBreak::none&&!properties->has(UnicodeFlag::extended_pictographic))scalar=ascii;
                if(unicode_grapheme_boundary(state,scalar)!=((value&0x80000000)!=0))return false;
            }
        }
    }return true;
}
}
bool unicode_grapheme_tests() noexcept {return vectors() && long_contexts() && ascii_equivalence();}
} // namespace maicord
