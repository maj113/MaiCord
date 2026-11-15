#include "maicord/text/unicode/unicode.hpp"
namespace maicord {namespace {
#include "generated/canonical_data.inc"
constexpr u32 bmp_count=sizeof(canonical_bmp_code_points)/sizeof(canonical_bmp_code_points[0]);
constexpr u32 supplementary_count=sizeof(canonical_supplementary_decompositions)/sizeof(canonical_supplementary_decompositions[0]);
// The 16-code-point blocks of the BMP that hold a decomposition, and those that hold the second character of a
// primary composition: most characters skip the searches.
struct BlockBits {u64 words[64];};
constexpr BlockBits decomposable=[]() constexpr noexcept {BlockBits bits{};
    for(const u32 cp:canonical_bmp_code_points)bits.words[cp>>10]|=1ull<<((cp>>4)&63);
    return bits;}();
constexpr BlockBits composable=[]() constexpr noexcept {BlockBits bits{};
    for(const u16 index:canonical_bmp_compositions){const u32 cp=canonical_bmp_pairs[index]&0xffff;bits.words[cp>>10]|=1ull<<((cp>>4)&63);}
    for(const u8 index:canonical_supplementary_compositions){const u32 cp=static_cast<u32>(canonical_supplementary_decompositions[index])&0x1fffff;if(cp<0x10000)bits.words[cp>>10]|=1ull<<((cp>>4)&63);}
    return bits;}();
}
u32 unicode_canonical_decompose(u32 cp,u32* second) noexcept {
    if(cp-0xac00u<11172){const u32 index=cp-0xac00u,trailing=index%28;
        if(trailing){*second=0x11a7+trailing;return cp-trailing;}*second=0x1161+index%588/28;return 0x1100+index/588;}
    if(cp<0x10000){if(!((decomposable.words[cp>>10]>>((cp>>4)&63))&1))return 0;u32 lo=0,hi=bmp_count;
        while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(canonical_bmp_code_points[mid]<cp)lo=mid+1;else hi=mid;}
        if(lo==bmp_count||canonical_bmp_code_points[lo]!=cp)return 0;
        *second=canonical_bmp_pairs[lo]&0xffff;return canonical_bmp_pairs[lo]>>16;}
    u32 lo=0,hi=supplementary_count;
    while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(static_cast<u32>(canonical_supplementary_decompositions[mid]>>42)<cp)lo=mid+1;else hi=mid;}
    if(lo==supplementary_count||static_cast<u32>(canonical_supplementary_decompositions[lo]>>42)!=cp)return 0;
    *second=static_cast<u32>(canonical_supplementary_decompositions[lo])&0x1fffff;return static_cast<u32>(canonical_supplementary_decompositions[lo]>>21)&0x1fffff;
}
u32 unicode_canonical_compose(u32 first,u32 second) noexcept {
    if(first-0x1100u<19&&second-0x1161u<21)return 0xac00+((first-0x1100)*21+(second-0x1161))*28;
    if(first-0xac00u<11172&&!((first-0xac00)%28)&&second-0x11a8u<27)return first+second-0x11a7;
    if(second<0x10000&&!((composable.words[second>>10]>>((second>>4)&63))&1))return 0;
    // The first of a BMP composite is in the BMP, its second too.
    if(first<0x10000){
        if(second>=0x10000)return 0;
        constexpr u32 count=sizeof(canonical_bmp_compositions)/sizeof(canonical_bmp_compositions[0]);
        const u32 pair=first<<16|second;u32 lo=0,hi=count;
        while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(canonical_bmp_pairs[canonical_bmp_compositions[mid]]<pair)lo=mid+1;else hi=mid;}
        return lo<count&&canonical_bmp_pairs[canonical_bmp_compositions[lo]]==pair?canonical_bmp_code_points[canonical_bmp_compositions[lo]]:0;}
    constexpr u32 count=sizeof(canonical_supplementary_compositions)/sizeof(canonical_supplementary_compositions[0]);constexpr u64 pair=(1ull<<42)-1;
    const u64 wanted=static_cast<u64>(first)<<21|second;u32 lo=0,hi=count;
    while(lo<hi){const u32 mid=lo+(hi-lo)/2;if((canonical_supplementary_decompositions[canonical_supplementary_compositions[mid]]&pair)<wanted)lo=mid+1;else hi=mid;}
    return lo<count&&(canonical_supplementary_decompositions[canonical_supplementary_compositions[lo]]&pair)==wanted?static_cast<u32>(canonical_supplementary_decompositions[canonical_supplementary_compositions[lo]]>>42):0;
}
}
