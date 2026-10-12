#include "maicord/text/nominal_sources.hpp"
#include "maicord/core/byte_order.hpp"
#include <intrin.h>
namespace maicord {namespace {
#include "generated/nominal_scalar_data.inc"
u64 scalar_word(u32 word) noexcept {return nominal_page_words[static_cast<u32>(nominal_page_index[word/4])*4+word%4];}
bool spend(u64* work,u64 n=1) noexcept {if(n>*work)return false;*work-=n;return true;}
enum class Mapping:u8 {constant,sequential,delta,bytes,words};
struct Builder {
    const SfntCmap& cmap;Slice<u64> bitmap;u64 work;
    SfntStatus map(u32 first,u32 last,Mapping mode,u32 base,const u8* array=nullptr,u16 delta=0) noexcept {
        if(first>0xffff)return SfntStatus::ok;last=minimum(last,0xffffu);
        for(u32 word=first/64;word<=last/64;++word){if(!spend(&work))return SfntStatus::resource_limit;u64 bits=scalar_word(word);if(word==first/64)bits&=~0ull<<(first%64);if(word==last/64)bits&=~0ull>>(63-last%64);
            if(bits&&(mode==Mapping::sequential||mode==Mapping::delta)){
                const u32 target=mode==Mapping::delta?(word*64+base)&65535u:base+word*64-first;
                if(static_cast<u64>(target)+64<=cmap.glyph_count){const u32 count=static_cast<u32>(__popcnt64(bits));
                    if(work>=count){work-=count;const u32 index=target/64,shift=target%64;const u64 low=bits<<shift;
                        bitmap.data[index]|=index?low:low&~1ull;
                        if(shift)bitmap.data[index+1]|=bits>>(64-shift);
                        continue;
                    }
                }
            }
            while(bits){if(!spend(&work))return SfntStatus::resource_limit;unsigned long bit=0;_BitScanForward64(&bit,bits);const u32 scalar=word*64+bit,offset=scalar-first;u32 glyph=base;
                switch(mode){case Mapping::constant:break;case Mapping::sequential:glyph+=offset;break;case Mapping::delta:glyph=(scalar+base)&65535u;break;
                    case Mapping::bytes:glyph=array[offset];break;case Mapping::words:glyph=byte_load_be16(array+offset*2);if(glyph)glyph=(glyph+delta)&65535u;break;}
                if(glyph>=cmap.glyph_count)return SfntStatus::glyph_out_of_range;if(glyph)bitmap.data[glyph/64]|=1ull<<(glyph%64);
                if(mode==Mapping::constant)return SfntStatus::ok;bits&=bits-1;
            }
        }return SfntStatus::ok;
    }
    SfntStatus run() noexcept {
        const u32 words=(cmap.glyph_count+63u)/64;if(bitmap.count<words)return SfntStatus::output_too_small;if(!spend(&work,words))return SfntStatus::resource_limit;
        for(u32 i=0;i<words;++i)bitmap.data[i]=0;const u8* p=cmap.base.data;
        if(cmap.format==0)return map(0,255,Mapping::bytes,0,p+6);
        if(cmap.format==6 || cmap.format==10){const bool short_map=cmap.format==6;const u32 first=short_map?byte_load_be16(p+6):byte_load_be32(p+12),count=short_map?byte_load_be16(p+8):byte_load_be32(p+16);
            return count?map(first,first+count-1,Mapping::words,0,p+(short_map?10:20)):SfntStatus::ok;}
        if(cmap.format==4){const u32 count=byte_load_be16(p+6)/2;
            for(u32 i=0;i<count;++i){if(!spend(&work))return SfntStatus::resource_limit;const u32 first=byte_load_be16(p+16+count*2+i*2),last=byte_load_be16(p+14+i*2),delta=byte_load_be16(p+16+count*4+i*2),word=16+count*6+i*2,offset=byte_load_be16(p+word);
                const auto s=map(first,last,offset?Mapping::words:Mapping::delta,delta,offset?p+word+offset:nullptr,static_cast<u16>(delta));if(s!=SfntStatus::ok)return s;}
            return SfntStatus::ok;
        }
        if(cmap.format==12 || cmap.format==13){const u32 count=byte_load_be32(p+12);
            for(u32 i=0;i<count;++i){if(!spend(&work))return SfntStatus::resource_limit;const u8* group=p+16+i*12;const u32 first=byte_load_be32(group),last=byte_load_be32(group+4),glyph=byte_load_be32(group+8);if(first>65535)break;
                if(first==last){if((scalar_word(first/64)>>(first%64))&1){
                        if(glyph>=cmap.glyph_count)return SfntStatus::glyph_out_of_range;if(glyph)bitmap.data[glyph/64]|=1ull<<(glyph%64);}
                    continue;}
                const auto s=map(first,last,cmap.format==12?Mapping::sequential:Mapping::constant,glyph);if(s!=SfntStatus::ok)return s;}
            return SfntStatus::ok;
        }
        return SfntStatus::unsupported;
    }
};
}
bool nominal_scalar_is_simple(u32 scalar) noexcept {return scalar<65536 && ((scalar_word(scalar/64)>>(scalar%64))&1)!=0;}
SfntStatus sfnt_nominal_sources(const SfntCmap& cmap,Slice<u64> bitmap,u64* used,u64 work) noexcept {
    u64 ignored=0;if(!used)used=&ignored;*used=0;if(!cmap.base.data || !cmap.glyph_count || (bitmap.count && !bitmap.data))return SfntStatus::invalid_input;
    Builder builder{cmap,bitmap,work};const auto s=builder.run();*used=work-builder.work;return s;
}
}
