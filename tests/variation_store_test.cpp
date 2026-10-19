#include "maicord/text/colr_variations.hpp"
#include "../src/text/variation_delta.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
u8 bytes[512],font_bytes[1024];double scalars[16];
void put16(u8* p,u16 value) noexcept {p[0]=static_cast<u8>(value>>8);p[1]=static_cast<u8>(value);}
void put32(u8* p,u32 value) noexcept {byte_store_be32(p,value);}
void support(u32 region,u32 axis,i16 start,i16 peak,i16 end) noexcept {
    auto* p=bytes+52+(region*2+axis)*6;put16(p,static_cast<u16>(start));put16(p+2,static_cast<u16>(peak));put16(p+4,static_cast<u16>(end));
}
void fixture() noexcept {
    clear_bytes(bytes,sizeof(bytes));put16(bytes+16,1);put32(bytes+18,32);put16(bytes+22,3);
    put32(bytes+24,112);put32(bytes+28,176);put32(bytes+32,0);
    put16(bytes+48,2);put16(bytes+50,4);
    support(0,0,0,16384,16384);support(1,0,-16384,-16384,0);
    support(2,0,0,8192,16384);support(2,1,0,16384,16384);
    support(3,0,12288,8192,16384); // Invalid ordering: ignore this axis.
    put16(bytes+128,2);put16(bytes+130,1);put16(bytes+132,3);
    put16(bytes+134,0);put16(bytes+136,1);put16(bytes+138,2);
    put16(bytes+140,100);bytes[142]=static_cast<u8>(-20);bytes[143]=30;
    put16(bytes+144,static_cast<u16>(-200));bytes[146]=127;bytes[147]=128;
    put16(bytes+192,1);put16(bytes+194,0x8001);put16(bytes+196,2);
    put16(bytes+198,0);put16(bytes+200,3);put32(bytes+202,100000);put16(bytes+206,static_cast<u16>(-300));
}
bool stores() noexcept {
    fixture();ItemVariationStore store{};VariationInstance instance{};const double coordinates[]{.5,.5};double delta=0;
    if(item_variation_store_open({bytes,sizeof(bytes)},16,&store)!=SfntStatus::ok || store.axis_count!=2 || store.region_count!=4 ||
       variation_instance_prepare(store,{coordinates,2},{scalars,16},&instance)!=SfntStatus::ok)return false;
    if(scalars[0]!=.5 || scalars[1]!=0 || scalars[2]!=.5 || scalars[3]!=1)return false;
    if(item_variation_delta(instance,0,&delta)!=SfntStatus::ok || delta!=65 ||
       item_variation_delta(instance,1,&delta)!=SfntStatus::ok || delta!=-164 ||
       item_variation_delta(instance,0x10000,&delta)!=SfntStatus::ok || delta!=49700 ||
       item_variation_delta(instance,0x2ffff,&delta)!=SfntStatus::ok || delta!=0 ||
       item_variation_delta(instance,0xffffffff,&delta)!=SfntStatus::ok || delta!=0 ||
       item_variation_delta(instance,2,&delta)!=SfntStatus::malformed || delta!=0)return false;
    const double negative[]{-.5,.5};
    if(variation_instance_prepare(store,{negative,2},{scalars,16},&instance)!=SfntStatus::ok ||
       item_variation_delta(instance,0,&delta)!=SfntStatus::ok || delta!=-10)return false;
    const double peak[]{1,1};
    if(variation_instance_prepare(store,{peak,2},{scalars,16},&instance)!=SfntStatus::ok || scalars[0]!=1 || scalars[2]!=0)return false;
    if(variation_instance_prepare(store,{coordinates,2},{scalars,3},&instance)!=SfntStatus::output_too_small || instance.store.table.data)return false;
    if(variation_instance_prepare(store,{coordinates,2},{scalars,16},&instance,1)!=SfntStatus::resource_limit || instance.store.table.data)return false;
    const double outside[]{1.1,0};
    if(variation_instance_prepare(store,{outside,2},{scalars,16},&instance)!=SfntStatus::invalid_input)return false;
    if(item_variation_store_open({bytes,sizeof(bytes)},16,&store,1)!=SfntStatus::resource_limit || store.table.data)return false;
    fixture();put16(bytes+130,4);
    if(item_variation_store_open({bytes,sizeof(bytes)},16,&store)!=SfntStatus::malformed)return false;
    fixture();put16(bytes+134,4);
    if(item_variation_store_open({bytes,sizeof(bytes)},16,&store)!=SfntStatus::malformed)return false;
    fixture();put32(bytes+24,0xfffffff0);
    return item_variation_store_open({bytes,sizeof(bytes)},16,&store)==SfntStatus::truncated;
}
bool block_reuse() noexcept {
    fixture();ItemVariationStore store{};VariationInstance instance{};
    if(item_variation_store_open({bytes,sizeof(bytes)},16,&store)!=SfntStatus::ok)return false;
    const double coordinates[]{.5,.5};
    if(variation_instance_prepare(store,{coordinates,2},{scalars,16},&instance)!=SfntStatus::ok)return false;
    const u32 indices[]{0,1,0,2,0,0x10000,0x10001,1,0x2ffff,0xffffffff,0x30000,1,0x10000,0};
    for(u32 mode=0;mode<2;++mode) {
        VariationDeltaBlock block{};const auto arithmetic=mode?VariationArithmetic::float_32:VariationArithmetic::wide;
        for(u32 index:indices) {
            double expected=99,actual=99;u64 a=0,b=0;
            const auto status=item_variation_delta(instance,index,&expected,arithmetic);
            if(item_variation_delta_cached(instance,index,&actual,&block,arithmetic)!=status)return false;
            copy_bytes(&a,&expected,8);copy_bytes(&b,&actual,8);if(a!=b)return false;
        }
    }
    // All-zero weights still validate the requested row; they cannot hide a
    // malformed inner/outer index through a no-variation fast path.
    const double zero[]{0,0};
    if(variation_instance_prepare(store,{zero,2},{scalars,16},&instance)!=SfntStatus::ok)return false;
    VariationDeltaBlock block{};double value=99;
    return item_variation_delta_cached(instance,0,&value,&block,VariationArithmetic::wide)==SfntStatus::ok &&
           item_variation_delta_cached(instance,2,&value,&block,VariationArithmetic::wide)==SfntStatus::malformed && value==0 &&
           item_variation_delta_cached(instance,0,&value,nullptr,VariationArithmetic::wide)==SfntStatus::invalid_input && value==0;
}
bool maps() noexcept {
    for(u32 width=1;width<=4;++width)for(u32 format=0;format<=1;++format) {
        clear_bytes(bytes,sizeof(bytes));const u32 bits=width==4?16:width==3?8:width==2?4:2;
        const u32 outer=width==4?65535:width==3?1000:width==2?10:1,inner=(1u<<bits)-1;
        bytes[0]=static_cast<u8>(format);bytes[1]=static_cast<u8>(((width-1)<<4)|(bits-1));
        if(format)put32(bytes+2,1);else put16(bytes+2,1);
        const u32 header=format?6u:4u,packed=(outer<<bits)|inner;
        for(u32 i=0;i<width;++i)bytes[header+i]=static_cast<u8>(packed>>(8*(width-1-i)));
        VariationIndexMap map{};u32 index=0;
        if(variation_index_map_open({bytes,header+width},0,&map)!=SfntStatus::ok ||
           variation_index_map_get(map,0,&index)!=SfntStatus::ok || index!=((outer<<16)|inner) ||
           variation_index_map_get(map,100000,&index)!=SfntStatus::ok || index!=((outer<<16)|inner))return false;
        if(variation_index_map_open({bytes,header+width-1},0,&map)!=SfntStatus::truncated)return false;
    }
    VariationIndexMap map{};u32 index=0;
    if(variation_index_map_get(map,0x30004,&index)!=SfntStatus::ok || index!=0x30004)return false;
    const u8 empty[]{0,0,0,0};
    if(variation_index_map_open({empty,4},0,&map)!=SfntStatus::ok ||
       variation_index_map_get(map,0x30004,&index)!=SfntStatus::ok || index!=0x30004)return false;
    const u8 overflow[]{0,0x30,0,1,0xff,0xff,0xff,0xff};
    return variation_index_map_open({overflow,8},0,&map)==SfntStatus::malformed;
}
bool precision_modes() noexcept {
    fixture();put32(bytes+202,static_cast<u32>(-400000));put16(bytes+206,3);put16(bytes+200,2);
    ItemVariationStore store{};VariationInstance instance{};
    const double coordinates[]{.125,43690.0/65536.0};double wide=0,narrow=0;
    if(item_variation_store_open({bytes,sizeof(bytes)},16,&store)!=SfntStatus::ok ||
       variation_instance_prepare(store,{coordinates,2},{scalars,16},&instance)!=SfntStatus::ok ||
       item_variation_delta(instance,0x10000,&wide)!=SfntStatus::ok ||
       item_variation_delta(instance,0x10000,&narrow,VariationArithmetic::float_32)!=SfntStatus::ok ||
       !(wide < -49999.5) || narrow!=-49999.5)return false;
    const double thirds[]{1.0/3.0,.5};
    if(variation_instance_prepare(store,{thirds,2},{scalars,16},&instance,1000,VariationCoordinatePrecision::fixed_16)!=SfntStatus::ok || scalars[0]!=21845.0/65536.0)return false;
    const double negative[]{-1.0/3.0,.5};
    return variation_instance_prepare(store,{negative,2},{scalars,16},&instance,1000,VariationCoordinatePrecision::fixed_16)==SfntStatus::ok && scalars[1]==21845.0/65536.0;
}
void font_fixture(bool tied) noexcept {
    fixture();clear_bytes(font_bytes,sizeof(font_bytes));put32(font_bytes,0x10000);put16(font_bytes+4,3);
    const u32 tags[]{sfnt_tag('C','O','L','R'),sfnt_tag('C','P','A','L'),sfnt_tag('m','a','x','p')},offsets[]{512,128,256},lengths[]{512,18,6};
    for(u32 i=0;i<3;++i){auto* p=font_bytes+12+i*16;put32(p,tags[i]);put32(p+8,offsets[i]);put32(p+12,lengths[i]);}
    put32(font_bytes+256,0x5000);put16(font_bytes+260,1);
    auto* p=font_bytes+128;put16(p+2,1);put16(p+4,1);put16(p+6,1);put32(p+8,14);p[17]=255;
    p=font_bytes+512;put16(p,1);put32(p+14,64);put32(p+26,384);put32(p+30,128);
    put32(p+64,1);put32(p+70,32);p[96]=5;p[97]=0;p[98]=1;p[99]=48; // ColorLine at 400.
    copy_bytes(p+128,bytes+16,208-16);
    p[384]=0;p[385]=0;put16(p+386,6);
    if(!tied)p[390]=p[391]=1;
    p[400]=0;put16(p+401,3);
    for(u32 i=0;i<3;++i){auto* s=p+403+i*10;put16(s,i?0x2000:0x4000);put16(s+4,0x2000);put32(s+6,i?2*(i-1):0xffffffff);}
}
bool resolver() noexcept {
    for(u32 tied=0;tied<2;++tied) {
        font_fixture(tied!=0);SfntFace face{};ColrFont font{};ColrVariationData data{};ColrVariationInstance instance{};
        const double coordinates[]{.5,.5};
        if(sfnt_open({font_bytes,sizeof(font_bytes)},0,&face)!=SfntStatus::ok || colr_open(face,&font)!=SfntStatus::ok ||
           colr_variation_data_open(font,&data)!=SfntStatus::ok ||
           colr_variation_instance_prepare(data,{coordinates,2},{scalars,16},&instance)!=SfntStatus::ok)return false;
        ColrPaint paint{};ColrPaintValues values{};
        if(colr_paint_read(font,96,&paint)!=SfntStatus::ok || colr_resolve_paint_values(instance,paint,&values)!=SfntStatus::ok || values.values[0]!=65)return false;
        ColrColorLine line{};ColrResolvedColorStop stops[3];
        if(colr_color_line(font,paint,&line)!=SfntStatus::ok || colr_resolve_color_stops(font,instance,line,{stops,3})!=SfntStatus::ok ||
           stops[0].original_index!=(tied?1u:2u) || stops[1].original_index!=(tied?2u:1u) || stops[2].original_index!=0)return false;
        if(colr_resolve_color_stops(font,instance,line,{stops,2})!=SfntStatus::output_too_small)return false;
        if(tied) {
            ColrClip clip{-10.5,20.25,30.5,40.75,100,true,true},resolved{};
            if(colr_resolve_clip(instance,clip,&resolved)!=SfntStatus::ok || resolved.minimum_x!=54 || resolved.minimum_y!=85 ||
               resolved.maximum_x!=96 || resolved.maximum_y!=106 || resolved.variable)return false;
            ColrVariationInstance compatible{};const double fractional[]{.125,.5};ColrPaintValues adjusted{};
            if(colr_variation_instance_prepare(data,{fractional,2},{scalars,16},&compatible,1000,ColrVariationMode::quantized)!=SfntStatus::ok ||
               colr_resolve_paint_values(compatible,paint,&adjusted)!=SfntStatus::ok || adjusted.values[0]!=16)return false;
            ColrClip empty{10,10,0,0,0xffffffff,true,true};
            if(colr_resolve_clip(compatible,empty,&resolved)!=SfntStatus::ok || resolved.present || resolved.maximum_x)return false;
            ColrPaint opacity{};opacity.format=3;opacity.variable=true;opacity.value_count=1;opacity.values[0]=1;
            if(colr_resolve_paint_values(compatible,opacity,&adjusted)!=SfntStatus::ok || adjusted.values[0]<=1)return false;
            // Restore the standard instance after reusing its scalar storage.
            if(colr_variation_instance_prepare(data,{coordinates,2},{scalars,16},&instance)!=SfntStatus::ok ||
               colr_resolve_paint_values(instance,opacity,&adjusted)!=SfntStatus::ok || adjusted.values[0]!=1)return false;
        }
        paint.variation_index=0xfffffffe;
        if(colr_resolve_paint_values(instance,paint,&values)!=SfntStatus::malformed || values.count)return false;
        paint.variation_index=0xffffffff;
        if(colr_resolve_paint_values(instance,paint,&values)!=SfntStatus::ok || values.values[0]!=0)return false;
        paint.variation_index=0xfffffffe;ColrVariationInstance absent{};
        if(colr_resolve_paint_values(absent,paint,&values)!=SfntStatus::ok || values.values[0]!=0)return false;
    }
    return true;
}
bool stop_order_cases() noexcept {
    u8 encoded[512*6]{};ColrResolvedColorStop output[512];i16 positions[512];
    ColrFont font{};font.table={encoded,sizeof(encoded)};font.palette.entry_count=1;
    ColrVariationInstance instance{};
    const u32 counts[]{0,1,2,3,16,511,512};
    for(u32 count:counts)for(u32 pattern=0;pattern<6;++pattern) {
        for(u32 i=0;i<count;++i) {
            const u32 rank=pattern==0?i:pattern==1?count-i:pattern==2?0:
                pattern==3?i/4:pattern==4?(i*197)%count:(i+2==count?count:i);
            positions[i]=static_cast<i16>(static_cast<i32>(rank)-256);
            put16(encoded+i*6,static_cast<u16>(positions[i]));put16(encoded+i*6+2,0);
            put16(encoded+i*6+4,static_cast<u16>((i%8)*2048));
        }
        const ColrColorLine line{{encoded,count*6},static_cast<u16>(count),0,false};
        if(colr_resolve_color_stops(font,instance,line,{output,512})!=SfntStatus::ok)return false;
        // Independent rank oracle checks both numeric order and file-order
        // ties, while tracking each record's distinct opacity payload.
        for(u32 original=0;original<count;++original) {
            u32 rank=0;for(u32 other=0;other<count;++other)
                if(positions[other]<positions[original] || (positions[other]==positions[original]&&other<original))++rank;
            if(output[rank].original_index!=original || output[rank].position!=positions[original]/16384.0 ||
               output[rank].alpha!=(original%8)/8.0 || output[rank].palette)return false;
        }
    }
    return true;
}
bool guards() noexcept {
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,8192,MEM_RESERVE,PAGE_NOACCESS));if(!memory)return false;
    if(!VirtualAlloc(memory,4096,MEM_COMMIT,PAGE_READWRITE)){VirtualFree(memory,0,MEM_RELEASE);return false;}
    u32 random=0x1a71a719;bool ok=true;
    for(u32 i=0;i<20000 && ok;++i) {
        fixture();random^=random<<13;random^=random>>17;random^=random<<5;
        const u32 length=i<512?i:512;
        if(i>=512)bytes[random%512]^=static_cast<u8>((random>>16)|1);
        auto* input=memory+4096-length;copy_bytes(input,bytes,length);ItemVariationStore store{};
        if(item_variation_store_open({input,length},16,&store,1024)==SfntStatus::ok) {
            VariationInstance instance{};const double coordinates[]{.5,.5};
            if(variation_instance_prepare(store,{coordinates,2},{scalars,16},&instance,1024)==SfntStatus::ok) {
                VariationDeltaBlock block;const u32 indices[]{0,1,0x10000,random&0x3ffff,0};
                for(u32 index:indices) {
                    double expected=0,actual=0;u64 a=0,b=0;
                    const auto status=item_variation_delta(instance,index,&expected);
                    if(item_variation_delta_cached(instance,index,&actual,&block,VariationArithmetic::wide)!=status){ok=false;break;}
                    copy_bytes(&a,&expected,8);copy_bytes(&b,&actual,8);if(a!=b){ok=false;break;}
                }
            }
        }
        VariationIndexMap map{};if(variation_index_map_open({input,length},0,&map,1024)==SfntStatus::ok){u32 index=0;variation_index_map_get(map,random,&index);}
    }
    VirtualFree(memory,0,MEM_RELEASE);return ok;
}
}
bool variation_store_tests() noexcept {return stores() && block_reuse() && maps() && precision_modes() && resolver() && stop_order_cases() && guards();}
} // namespace maicord
