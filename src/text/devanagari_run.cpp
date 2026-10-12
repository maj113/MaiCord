#include "maicord/text/devanagari_run.hpp"
#include "maicord/text/devanagari_clusters.hpp"
#include "maicord/text/devanagari_shape_cluster.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/text/unicode.hpp"

namespace maicord {
namespace {
// The caller already retains compact output through the call. Cache references
// to it instead of copying glyph/placement payloads into a second cache.
struct ClusterMemo {u32 hash,key,byte_first,unit_first,glyph_first,glyph_count;};
static_assert(sizeof(ClusterMemo)==24);
}
SfntStatus native_devanagari_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u8> font_forms,
    ConstSlice<SfntLookupStep> steps,Utf8View text,float em,NativeDevanagariRunStorage storage,
    NativeAsciiResult* result,SfntShapeLimits limits,float trailing_spacing,bool reuse_clusters,bool rtl) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    u32 spacing_bits=0;copy_bytes(&spacing_bits,&trailing_spacing,4);
    if((spacing_bits&0x7fffffffu)>=0x7f800000u||((spacing_bits&0x80000000u)&&(spacing_bits&0x7fffffffu)))return SfntStatus::invalid_input;
    if((text.bytes&&!text.data)||font_forms.count!=128||!font_forms.data||(steps.count&&!steps.data)||
       (storage.glyphs.count&&!storage.glyphs.data)||(storage.placements.count&&!storage.placements.data)||
       (storage.character_glyphs.count&&!storage.character_glyphs.data)||!plan.advance||!plan.units_per_em||
       !(em>0)||em>65536)return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    const bool shared_glyphs=reinterpret_cast<const void*>(storage.glyphs.data)==storage.shaping.glyphs.data;
    const bool shared_placements=storage.placements.data==storage.shaping.placements.data;
    const bool shared_characters=storage.character_glyphs.data==storage.shaping.character_glyphs.data;
    u32 window[32];u8 starts[32],forms[31];SfntClusterGlyph mapped[31];
    ClusterMemo memo[16]{};u32 victims=0;
    u32 read=0,pending=0,glyph_count=0,units=0,clusters=0,source_byte=0;
    const auto spend=[&](u64 cost) noexcept {if(cost>limits.lookup_operations-result->lookup_operations)return false;result->lookup_operations+=cost;return true;};
    while(read<text.bytes||pending){
        while(read<text.bytes&&pending<32){const auto scalar=utf8_decode_one(text.data+read,text.bytes-read);
            if(!scalar.valid)return SfntStatus::invalid_input;window[pending++]=scalar.codepoint;read+=scalar.width;}
        auto status=sfnt_devanagari_cluster_boundaries({window,pending},{starts,32},limits.stage_work);
        if(status!=SfntStatus::ok)return status;
        u32 first=0;
        while(first<pending){u32 end=first+1;while(end<pending&&!starts[end])++end;
            // Retain the unfinished final cluster until another scalar or EOF
            // decides its boundary. At 32 scalars the scanner always has a cut.
            if(end==pending&&read<text.bytes)break;
            const u32 count=end-first;if(count>31)return SfntStatus::invalid_input;
            const bool followed=end<pending||read<text.bytes;
            u32 local_units=0,local_bytes=0,hash=2166136261u;
            for(u32 i=0;i<count;++i){const u32 scalar=window[first+i];local_units+=scalar>0xffff?2u:1u;
                local_bytes+=scalar<0x80?1u:scalar<0x800?2u:scalar<0x10000?3u:4u;
                if(reuse_clusters)hash=(hash^scalar)*16777619u;}
            if(local_units>storage.character_glyphs.count-units)return SfntStatus::output_too_small;
            const u32 key=(local_bytes<<1)|static_cast<u32>(followed),set=(hash^(hash>>16))&7u;
            u32 hit=16;
            if(reuse_clusters){if(!spend(count+2))return SfntStatus::resource_limit;
                for(u32 way=0;way<2;++way){const u32 slot=set*2+way;const auto& entry=memo[slot];
                    if(entry.hash!=hash||entry.key!=key)continue;
                    if(!spend(local_bytes))return SfntStatus::resource_limit;
                    if(bytes_equal(text.data+entry.byte_first,text.data+source_byte,local_bytes)){hit=slot;break;}}}
            if(hit!=16){const auto& entry=memo[hit];
                if(entry.glyph_count>limits.stage_work-glyph_count)return SfntStatus::resource_limit;
                if(entry.glyph_count>storage.glyphs.count-glyph_count||entry.glyph_count>storage.placements.count-glyph_count)return SfntStatus::output_too_small;
                if(!spend(static_cast<u64>(entry.glyph_count)*2+local_units+1))return SfntStatus::resource_limit;
                for(u32 i=0;i<local_units;++i)storage.character_glyphs[units+i]=storage.character_glyphs[entry.unit_first+i]-entry.glyph_first+glyph_count;
                copy_bytes(storage.glyphs.data+glyph_count,storage.glyphs.data+entry.glyph_first,static_cast<usize>(entry.glyph_count)*sizeof(u16));
                copy_bytes(storage.placements.data+glyph_count,storage.placements.data+entry.glyph_first,static_cast<usize>(entry.glyph_count)*sizeof(SfntPlacement));
                victims=(victims&~(1u<<set))|(((hit&1u)^1u)<<set);
                glyph_count+=entry.glyph_count;units+=local_units;source_byte+=local_bytes;++clusters;first=end;continue;}
            u32 mapped_units=0;
            for(u32 i=0;i<count;++i){const u32 scalar=window[first+i];u16 glyph=0;
                status=sfnt_cmap_glyph(cmap,rtl?unicode_bidi_mirror(scalar):scalar,0,&glyph);if(status!=SfntStatus::ok)return status;
                mapped[i]={{glyph,sfnt_gdef_glyph_class(plan.gdef,glyph),0},mapped_units};
                forms[i]=scalar>=0x900&&scalar<0x980?font_forms[scalar-0x900]:0;
                mapped_units+=scalar>0xffff?2u:1u;}
            auto remaining=limits;remaining.lookup_operations-=result->lookup_operations;SfntShapeRunResult shaped{};
            auto scratch=storage.shaping;
            // Exact-prefix app storage retains earlier compact output while
            // each later cluster borrows only the still-unused backing suffix.
            if(shared_glyphs){if(glyph_count>=scratch.glyphs.count)return SfntStatus::output_too_small;
                scratch.glyphs={scratch.glyphs.data+glyph_count,scratch.glyphs.count-glyph_count};}
            if(shared_placements){if(glyph_count>=scratch.placements.count)return SfntStatus::output_too_small;
                scratch.placements={scratch.placements.data+glyph_count,scratch.placements.count-glyph_count};}
            if(shared_characters){if(units>=scratch.character_glyphs.count)return SfntStatus::output_too_small;
                scratch.character_glyphs={scratch.character_glyphs.data+units,scratch.character_glyphs.count-units};}
            status=sfnt_devanagari_shape_cluster(plan,cmap,{mapped,count},{window+first,count},{forms,count},
                local_units,em,scratch,&shaped,steps,remaining,followed,trailing_spacing,rtl);
            if(shaped.lookup_operations>remaining.lookup_operations)return SfntStatus::invalid_input;
            result->lookup_operations+=shaped.lookup_operations;if(status!=SfntStatus::ok)return status;
            if(shaped.glyph_count>limits.stage_work-glyph_count)return SfntStatus::resource_limit;
            if(shaped.glyph_count>storage.glyphs.count-glyph_count||shaped.glyph_count>storage.placements.count-glyph_count)
                return SfntStatus::output_too_small;
            if(reuse_clusters){if(!spend(6))return SfntStatus::resource_limit;const u32 way=(victims>>set)&1u;
                memo[set*2+way]={hash,key,source_byte,units,glyph_count,shaped.glyph_count};victims^=1u<<set;}
            for(u32 i=0;i<local_units;++i)storage.character_glyphs[units+i]=scratch.character_glyphs[i]+glyph_count;
            for(u32 i=0;i<shaped.glyph_count;++i){storage.glyphs[glyph_count+i]=scratch.glyphs[i].glyph.glyph;
                storage.placements[glyph_count+i]=scratch.placements[i];}
            glyph_count+=shaped.glyph_count;units+=local_units;source_byte+=local_bytes;++clusters;first=end;
        }
        if(!first)return SfntStatus::invalid_input;
        pending-=first;for(u32 i=0;i<pending;++i)window[i]=window[first+i];
    }
    *result={glyph_count,units,0,clusters,result->lookup_operations};return SfntStatus::ok;
}
}
