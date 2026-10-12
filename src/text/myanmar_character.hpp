#pragma once
#include "maicord/text/unicode.hpp"
namespace maicord {
// Myanmar categories, including Extended A/B, verified with Windows probes.
// One byte per scalar; ordinal ranks are kept separate from classification.
// C/G: consonant/generic base; H/s: halant/asat; y/r/w/h: medials;
// e/a/b/v: pre/above/below/post vowels; n/d: anusvara/dot; p/t: tones;
// j/z/S/o: joiner/CGJ/selector/other.
inline char myanmar_category(u32 c) noexcept {
    static constexpr char classes[]{
        'C','C','C','C','C','C','C','C','C','C','C','C','C','C','C','C',
        'C','C','C','C','C','C','C','C','C','C','C','C','C','C','C','C',
        'C','C','C','C','C','C','C','C','C','C','C','v','v','a','a','b',
        'b','e','n','a','a','a','n','d','t','H','s','y','r','w','h','C',
        'G','G','G','G','G','G','G','G','G','G','o','o','o','o','C','o',
        'C','C','C','C','C','C','v','v','b','b','C','C','C','C','y','y',
        'h','C','v','p','p','C','C','v','v','p','p','p','p','p','C','C',
        'C','a','a','a','a','C','C','C','C','C','C','C','C','C','C','C',
        'C','C','w','v','e','a','a','t','t','t','t','t','t','t','C','t',
        'G','G','G','G','G','G','G','G','G','G','t','t','t','a','o','o',
        'C','C','C','C','C','C','C','C','C','C','C','C','C','C','C','C',
        'o','C','C','C','C','C','C','o','o','o','C','p','t','t','C','C',
        'C','C','C','C','C','a','o','C','C','C','C','C','C','C','C','C',
        'G','G','G','G','G','G','G','G','G','G','C','C','C','C','C','o',
    };
    if(c>=0x1000&&c<=0x109f)return classes[c-0x1000];
    if(c>=0xaa60&&c<=0xaa7f)return classes[160+c-0xaa60];
    if(c>=0xa9e0&&c<=0xa9ff)return classes[192+c-0xa9e0];
    if(c==0x200c||c==0x200d)return 'j';if(c==0x34f)return 'z';
    if((c>=0xfe00&&c<=0xfe0f)||(c>=0xe0100&&c<=0xe01ef))return 'S';
    if(c==0xa0||c==0xd7||c==0x2d||c==0x2022||c==0x25cc||
       (c>=0x2012&&c<=0x2015)||(c>=0x25fb&&c<=0x25fe))return 'G';
    return 'o';
}
}
