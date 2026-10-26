#include "maicord/text/nominal_participation.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/nominal_sources.hpp"
#include "maicord/text/layout_tables.hpp"
#include "maicord/text/base.hpp"
namespace maicord {namespace {
u16 r16(const u8* b,u32 at) noexcept {return byte_load_be16(b+at);}
u16 r16(const ConstSlice<u8>& b,u32 at) noexcept {return byte_load_be16(b.data+at);}
bool span(const ConstSlice<u8>& b,u32 at,u32 n) noexcept {return b.data && at<=b.count && n<=b.count-at;}
bool spend(u64* work,u64 n=1) noexcept {if(n>*work)return false;*work-=n;return true;}
u32 allowance(u64 work) noexcept {return static_cast<u32>(minimum<u64>(work,0xffffffffu));}
bool standard(SfntLayoutKind kind,u32 tag) noexcept {
    if(kind==SfntLayoutKind::substitution){switch(tag){
        case sfnt_tag('c','c','m','p'):case sfnt_tag('l','o','c','l'):case sfnt_tag('r','l','i','g'):case sfnt_tag('r','v','r','n'):
        case sfnt_tag('l','i','g','a'):case sfnt_tag('c','l','i','g'):case sfnt_tag('c','a','l','t'):return true;default:return false;}}
    switch(tag){case sfnt_tag('k','e','r','n'):case sfnt_tag('m','a','r','k'):case sfnt_tag('m','k','m','k'):case sfnt_tag('d','i','s','t'):return true;default:return false;}
}
void set(Slice<u64> bits,u16 index) noexcept {bits.data[index/64]|=1ull<<(index%64);}
bool has(Slice<u64> bits,u16 index) noexcept {return (bits.data[index/64]&(1ull<<(index%64)))!=0;}
struct Collector {
    const SfntLayout& layout;u32 limit;Slice<u64> bitmap,features,lookups;SfntNominalParticipationResult& result;u64 work;ConstSlice<u64> sources;SfntNominalSourceCache* lazy;u64 source_work;const SfntFeatureSubstitutions& variations;
    SfntStatus prepare_sources() noexcept {
        if(!lazy)return SfntStatus::ok;
        if(!lazy->ready){const auto status=sfnt_nominal_sources(*lazy->cmap,lazy->bitmap,nullptr,source_work);
            if(status!=SfntStatus::ok)return status;lazy->ready=true;}
        sources={lazy->bitmap.data,lazy->bitmap.count};return SfntStatus::ok;
    }
    SfntStatus range(u16 first,u16 last) noexcept {
        if(first>last || last>=limit)return SfntStatus::malformed;
        const u32 a=first/64,b=last/64;if(!spend(&work,b-a+1))return SfntStatus::resource_limit;
        const u64 left=~0ull<<(first%64),right=~0ull>>(63-last%64);
        if(a==b)bitmap.data[a]|=left&right;
        else {bitmap.data[a]|=left;for(u32 i=a+1;i<b;++i)bitmap.data[i]=~0ull;bitmap.data[b]|=right;}
        return SfntStatus::ok;
    }
    SfntStatus open_coverage(ConstSlice<u8> b,u32 offset,SfntCoverage* c) noexcept {
        if(!offset)return SfntStatus::malformed;if(!span(b,offset,4))return SfntStatus::truncated;
        if(!spend(&work,static_cast<u32>(r16(b,offset+2))+1))return SfntStatus::resource_limit;
        return sfnt_coverage_open({b.data+offset,b.count-offset},limit,c);
    }
    SfntStatus available(ConstSlice<u8> b,u32 offset,bool* found) noexcept {
        *found=false;SfntCoverage c{};auto s=open_coverage(b,offset,&c);if(s!=SfntStatus::ok)return s;
        for(u32 i=0;i<c.record_count;++i){const u32 first=r16(c.records,i*(c.format==1?2:6)),last=c.format==1?first:r16(c.records,i*6+2);
            for(u32 word=first/64;word<=last/64;++word){if(!spend(&work))return SfntStatus::resource_limit;u64 bits=sources.data[word];if(word==first/64)bits&=~0ull<<(first%64);if(word==last/64)bits&=~0ull>>(63-last%64);if(bits){*found=true;return SfntStatus::ok;}}
        }return SfntStatus::ok;
    }
    SfntStatus coverage(ConstSlice<u8> b,u32 offset) noexcept {
        SfntCoverage c{};auto s=open_coverage(b,offset,&c);if(s!=SfntStatus::ok)return s;
        for(u32 i=0;i<c.record_count;++i){const u16 first=r16(c.records,i*(c.format==1?2:6));const u16 last=c.format==1?first:r16(c.records,i*6+2);s=range(first,last);if(s!=SfntStatus::ok)return s;}
        return SfntStatus::ok;
    }
    SfntStatus nominal_ligatures(ConstSlice<u8> b) noexcept {
        if(!span(b,0,6))return SfntStatus::truncated;if(r16(b,0)!=1)return SfntStatus::unsupported;const u32 sets=r16(b,4);if(!span(b,6,sets*2))return SfntStatus::truncated;
        SfntCoverage roots{};auto s=open_coverage(b,r16(b,2),&roots);if(s!=SfntStatus::ok)return s;if(roots.covered_count!=sets)return SfntStatus::malformed;
        for(u32 i=0;i<sets;++i){if(!spend(&work,17))return SfntStatus::resource_limit;u16 first=0;s=sfnt_coverage_glyph(roots,i,&first);if(s!=SfntStatus::ok)return s;
            if(!(sources.data[first/64]&(1ull<<(first%64))))continue;
            const u32 set_at=r16(b,6+i*2);if(!set_at || !span(b,set_at,2))return SfntStatus::truncated;const u32 count=r16(b,set_at);if(!span(b,set_at+2,count*2))return SfntStatus::truncated;
            for(u32 j=0;j<count;++j){if(!spend(&work))return SfntStatus::resource_limit;const u32 at=set_at+r16(b,set_at+2+j*2);if(!span(b,at,4))return SfntStatus::truncated;const u32 components=r16(b,at+2);if(!components)return SfntStatus::malformed;if(!span(b,at+4,(components-1)*2))return SfntStatus::truncated;
                bool possible=true;for(u32 k=1;k<components;++k){if(!spend(&work))return SfntStatus::resource_limit;const u16 glyph=r16(b,at+2+k*2);if(glyph>=limit)return SfntStatus::malformed;if(!(sources.data[glyph/64]&(1ull<<(glyph%64)))){possible=false;break;}}
                if(!possible)continue;s=range(first,first);if(s!=SfntStatus::ok)return s;
                for(u32 k=1;k<components;++k){const u16 glyph=r16(b,at+2+k*2);s=range(glyph,glyph);if(s!=SfntStatus::ok)return s;}
            }
        }return SfntStatus::ok;
    }
    SfntStatus subtable(ConstSlice<u8> b,u16 type) noexcept {
        const bool gsub=layout.kind==SfntLayoutKind::substitution;
        if(gsub && type==8)return SfntStatus::ok;
        if(!span(b,0,4))return SfntStatus::truncated;const u16 format=r16(b,0);
        const bool context=gsub?(type==5 || type==6):(type==7 || type==8);
        if((gsub&&type==4)||(context&&format==3)){const auto s=prepare_sources();if(s!=SfntStatus::ok)return s;}
        if(gsub&&type==4&&sources.count)return nominal_ligatures(b);
        if(context && format==3){const bool chain=type==(gsub?6:8);u32 counts[3]{},offsets[3]{};
            if(chain){u32 at=2;for(u32 part=0;part<3;++part){if(!span(b,at,2))return SfntStatus::truncated;counts[part]=r16(b,at);offsets[part]=at+2;at+=2+counts[part]*2;if(!span(b,offsets[part],counts[part]*2))return SfntStatus::truncated;}}
            else {if(!span(b,0,6))return SfntStatus::truncated;counts[1]=r16(b,2);offsets[1]=6;if(!span(b,6,counts[1]*2))return SfntStatus::truncated;}
            if(!counts[1])return SfntStatus::malformed;
            if(sources.count)for(u32 part=0;part<3;++part)for(u32 i=0;i<counts[part];++i){if(!spend(&work))return SfntStatus::resource_limit;bool found=false;const auto s=available(b,r16(b,offsets[part]+i*2),&found);if(s!=SfntStatus::ok)return s;if(!found)return SfntStatus::ok;}
            for(u32 i=0;i<counts[1];++i){if(!spend(&work))return SfntStatus::resource_limit;const auto s=coverage(b,r16(b,offsets[1]+i*2));if(s!=SfntStatus::ok)return s;}
            return SfntStatus::ok;
        }
        if(context && format!=1 && format!=2)return SfntStatus::unsupported;
        auto s=coverage(b,r16(b,2));if(s!=SfntStatus::ok || !gsub || type!=4)return s;
        if(format!=1)return SfntStatus::unsupported;if(!span(b,0,6))return SfntStatus::truncated;const u16 sets=r16(b,4);if(!span(b,6,static_cast<u32>(sets)*2))return SfntStatus::truncated;
        for(u32 i=0;i<sets;++i){if(!spend(&work))return SfntStatus::resource_limit;const u32 set_at=r16(b,6+i*2);if(!span(b,set_at,2) || !set_at)return SfntStatus::truncated;const u16 count=r16(b,set_at);if(!span(b,set_at+2,static_cast<u32>(count)*2))return SfntStatus::truncated;
            for(u32 j=0;j<count;++j){if(!spend(&work))return SfntStatus::resource_limit;const u32 at=set_at+r16(b,set_at+2+j*2);if(!span(b,at,4))return SfntStatus::truncated;const u16 components=r16(b,at+2);if(!components)return SfntStatus::malformed;if(!span(b,at+4,static_cast<u32>(components-1)*2))return SfntStatus::truncated;
                for(u32 k=1;k<components;++k){const u16 glyph=r16(b,at+2+k*2);s=range(glyph,glyph);if(s!=SfntStatus::ok)return s;}
            }
        }return SfntStatus::ok;
    }
    SfntStatus run() noexcept {
        if(!spend(&work,static_cast<u64>(features.count)+lookups.count))return SfntStatus::resource_limit;
        for(u32 i=0;i<features.count;++i)features.data[i]=0;for(u32 i=0;i<lookups.count;++i)lookups.data[i]=0;
        for(u32 i=0;i<layout.script_count;++i){u32 tag=0,used=0;SfntScript script{};auto s=sfnt_layout_script(layout,static_cast<u16>(i),&tag,&script,allowance(work),&used);work-=used;if(s!=SfntStatus::ok)return s;
            for(u32 j=0;j<=script.language_count;++j){if(!spend(&work))return SfntStatus::resource_limit;SfntLanguage lang{};s=sfnt_script_language(script,layout.feature_count,j?j-1:sfnt_default_language,&tag,&lang,allowance(work),&used);work-=used;if(s==SfntStatus::missing_table)continue;if(s!=SfntStatus::ok)return s;if(lang.required_feature!=0xffff)set(features,lang.required_feature);}
        }
        for(u32 i=0;i<layout.feature_count;++i){if(!spend(&work))return SfntStatus::resource_limit;const u32 tag=byte_load_be32(layout.features.data+2+i*6);if(!standard(layout.kind,tag) && !has(features,static_cast<u16>(i)))continue;
            SfntFeature f{};u32 actual_tag=0,used=0;auto s=sfnt_layout_feature(layout,static_cast<u16>(i),variations,&actual_tag,&f,allowance(work),&used);work-=used;if(s!=SfntStatus::ok)return s;
            if(!spend(&work,f.lookup_indices.count/2))return SfntStatus::resource_limit;for(u32 j=0;j<f.lookup_indices.count;j+=2)set(lookups,r16(f.lookup_indices,j));
        }
        for(u32 i=0;i<layout.lookup_count;++i){if(!spend(&work))return SfntStatus::resource_limit;if(!has(lookups,static_cast<u16>(i)))continue;
            SfntLookup lookup{};u32 used=0;auto s=sfnt_layout_lookup(layout,static_cast<u16>(i),&lookup,allowance(work),&used);work-=used;if(s!=SfntStatus::ok)return s;++result.lookups;
            for(u32 j=0;j<lookup.subtable_count;++j){if(!spend(&work))return SfntStatus::resource_limit;ConstSlice<u8> b{};s=sfnt_lookup_subtable(lookup,static_cast<u16>(j),&b);if(s!=SfntStatus::ok)return s;s=subtable(b,lookup.type);if(s!=SfntStatus::ok)return s;}
        }
        return SfntStatus::ok;
    }
};
}
SfntStatus sfnt_nominal_participation_add(const SfntLayout& layout,u32 limit,Slice<u64> bitmap,Slice<u64> scratch,SfntNominalParticipationResult* result,u64 work,ConstSlice<u64> sources,SfntNominalSourceCache* lazy,const SfntFeatureSubstitutions& variations) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);if(!layout.table.data || static_cast<u8>(layout.kind)>1 || !limit || limit>65536 || (bitmap.count && !bitmap.data) || (scratch.count && !scratch.data) || (sources.count && !sources.data))return SfntStatus::invalid_input;
    if(lazy&&(sources.count||!lazy->cmap||!lazy->cmap->base.data||lazy->cmap->glyph_count!=limit||!lazy->bitmap.data))return SfntStatus::invalid_input;
    if(lazy&&lazy->bitmap.count<(limit+63)/64)return SfntStatus::output_too_small;
    const u32 f=(static_cast<u32>(layout.feature_count)+63)/64,l=(static_cast<u32>(layout.lookup_count)+63)/64;
    if(bitmap.count<(limit+63)/64 || scratch.count<f+l || (sources.count && sources.count<(limit+63)/64))return SfntStatus::output_too_small;
    Collector collector{layout,limit,bitmap,{scratch.data,f},{scratch.data?scratch.data+f:nullptr,l},*result,work,sources,lazy,work,variations};const auto status=collector.run();result->operations=work-collector.work;return status;
}
}
