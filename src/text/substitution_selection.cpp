#include "substitution_selection.hpp"
#include "maicord/text/lookup_selection.hpp"
#include "maicord/text/base.hpp"
namespace maicord {namespace {
bool spend(u32& work,u32 count=1) noexcept {if(count>work)return false;work-=count;return true;}
bool has(ConstSlice<u64> bits,u32 index) noexcept {return (bits[index/64]&(1ull<<(index%64)))!=0;}
void set(Slice<u64> bits,u32 index) noexcept {bits[index/64]|=1ull<<(index%64);}
}
SfntStatus native_substitution_selection(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,ConstSlice<u64> required,Arena& scratch,ConstSlice<u64>* selected,u32 work) noexcept {
    *selected={};const u32 feature_words=(layout.feature_count+63u)/64,lookup_words=(layout.lookup_count+63u)/64;
    if(required.count<feature_words||(feature_words&&!required.data))return SfntStatus::invalid_input;
    if(!spend(work,lookup_words))return SfntStatus::resource_limit;
    Slice<u64> bits{arena_allocate_zero<u64>(&scratch,lookup_words),lookup_words};
    Slice<u16> queue{arena_allocate<u16>(&scratch,layout.lookup_count),layout.lookup_count};
    if((lookup_words&&!bits.data)||(layout.lookup_count&&!queue.data))return SfntStatus::output_too_small;
    u32 used=0;
    u32 count=0,excluded=0;
    for(u32 i=0;i<layout.feature_count;++i){if(!spend(work))return SfntStatus::resource_limit;const u32 tag=byte_load_be32(layout.features.data+2+i*6);
        if(!has(required,i)&&!alphabetic_substitution_feature(tag)&&!arabic_substitution_feature(tag)&&!indic_substitution_feature(tag)&&!use_substitution_feature(tag)){++excluded;continue;}
        SfntFeature feature{};u32 actual=0;const auto status=sfnt_layout_feature(layout,static_cast<u16>(i),variations,&actual,&feature,work,&used);work-=used;if(status!=SfntStatus::ok)return status;
        if(!spend(work,feature.lookup_indices.count/2))return SfntStatus::resource_limit;
        for(u32 j=0;j<feature.lookup_indices.count;j+=2){const u16 index=byte_load_be16(feature.lookup_indices.data+j);if(!has({bits.data,bits.count},index)){set(bits,index);queue[count++]=index;}}
    }
    if(!excluded||count==layout.lookup_count)return SfntStatus::ok;
    SfntLookupSelectionResult result{};const auto status=sfnt_lookup_selection(layout,{queue.data,count},bits,queue,&result,work);
    if(status==SfntStatus::ok&&result.lookups!=layout.lookup_count)*selected={bits.data,bits.count};return status;
}
}
