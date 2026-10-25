#include "maicord/text/variation_store.hpp"
#include "maicord/core/memory.hpp"
#include "variation_delta.hpp"
#include "sfnt_read.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord {
using namespace sfnt_read;

SfntStatus variation_index_map_open(ConstSlice<u8> table,u32 at,VariationIndexMap* output,u32 work,u32* work_used) noexcept {
    WorkReport report{work,work,work_used};
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    if(!span(table,at,4))return SfntStatus::truncated;
    const u8 format=table[at],entry=table[at+1];
    if(format>1)return SfntStatus::unsupported;
    if(entry&0xc0)return SfntStatus::malformed;
    const u32 header=format?6u:4u;
    if(!span(table,at,header))return SfntStatus::truncated;
    const u32 count=format?r32(table,at+2):r16(table,at+2);
    const u8 width=static_cast<u8>(((entry>>4)&3)+1),bits=static_cast<u8>((entry&15)+1);
    if(!count)return SfntStatus::ok;
    if(count>(table.count-at-header)/width)return SfntStatus::truncated;
    if(count>work)return SfntStatus::resource_limit;work-=count;
    const auto entries=view(table,at+header,count*width);
    // Both logical index components are 16-bit even when the packed outer
    // portion has more than 16 available bits.
    for(u32 i=0;i<count;++i)if((packed(entries.data+i*width,width)>>bits)>0xffffu)return SfntStatus::malformed;
    *output={entries,count,width,bits};return SfntStatus::ok;
}
SfntStatus variation_index_map_get(const VariationIndexMap& map,u32 index,u32* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output=0xffffffff;
    if(!map.entries.data && !map.count){*output=index;return SfntStatus::ok;}
    if(!map.count || !map.entries.data || !map.entry_size || map.entry_size>4 || !map.inner_bits || map.inner_bits>16)return SfntStatus::invalid_input;
    const u32 at=minimum(index,map.count-1)*map.entry_size;
    const u32 value=packed(map.entries.data+at,map.entry_size);
    *output=((value>>map.inner_bits)<<16)|(value&((1u<<map.inner_bits)-1));return SfntStatus::ok;
}
SfntStatus item_variation_data(const ItemVariationStore& store,u16 outer,ItemVariationData* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    if(!store.table.data)return SfntStatus::invalid_input;
    if(outer>=store.offsets.count/4)return SfntStatus::malformed;
    const u32 offset=r32(store.offsets,outer*4u);
    if(!offset){output->absent=true;return SfntStatus::ok;}
    if(store.origin>store.table.count || offset>store.table.count-store.origin)return SfntStatus::truncated;
    const u32 at=store.origin+offset;
    if(!span(store.table,at,6))return SfntStatus::truncated;
    ItemVariationData result{};
    result.item_count=r16(store.table,at);result.word_count=r16(store.table,at+2)&0x7fff;
    result.long_words=(r16(store.table,at+2)&0x8000)!=0;result.region_count=r16(store.table,at+4);
    if(result.word_count>result.region_count)return SfntStatus::malformed;
    const u32 indices=result.region_count*2u;
    if(!span(store.table,at+6,indices))return SfntStatus::truncated;
    result.region_indices=view(store.table,at+6,indices);
    result.stride=(result.region_count+result.word_count)*(result.long_words?2u:1u);
    const u32 start=at+6+indices;
    if(result.stride && result.item_count>(store.table.count-start)/result.stride)return SfntStatus::truncated;
    result.deltas=view(store.table,start,result.item_count*result.stride);
    *output=result;return SfntStatus::ok;
}
SfntStatus item_variation_store_open(ConstSlice<u8> table,u32 at,ItemVariationStore* output,u32 work,u32* work_used) noexcept {
    WorkReport report{work,work,work_used};
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    if(!span(table,at,8))return SfntStatus::truncated;
    if(r16(table,at)!=1)return SfntStatus::unsupported;
    const u32 region_offset=r32(table,at+2),count=r16(table,at+6);
    if(!span(table,at+8,count*4))return SfntStatus::truncated;
    if(!region_offset)return SfntStatus::malformed;
    if(region_offset>table.count-at || !span(table,at+region_offset,4))return SfntStatus::truncated;
    const u32 region_at=at+region_offset;
    ItemVariationStore result{table,view(table,at+8,count*4),{},at,r16(table,region_at),r16(table,region_at+2)};
    if(result.region_count>=32768)return SfntStatus::malformed;
    const u32 cells=static_cast<u32>(result.region_count)*result.axis_count;
    if(cells>(table.count-region_at-4)/6)return SfntStatus::truncated;
    result.regions=view(table,region_at+4,cells*6);
    for(u32 i=0;i<cells;++i) {
        if(!work)return SfntStatus::resource_limit;--work;
        for(u32 k=0;k<3;++k){const i16 value=static_cast<i16>(r16(result.regions,i*6+k*2));if(value < -16384 || value > 16384)return SfntStatus::malformed;}
    }
    for(u32 i=0;i<count;++i) {
        if(!work)return SfntStatus::resource_limit;--work;
        ItemVariationData data{};const auto status=item_variation_data(result,static_cast<u16>(i),&data);
        if(status!=SfntStatus::ok)return status;
        for(u32 j=0;j<data.region_count;++j) {
            if(!work)return SfntStatus::resource_limit;--work;
            if(r16(data.region_indices,j*2)>=result.region_count)return SfntStatus::malformed;
        }
    }
    *output=result;return SfntStatus::ok;
}

#pragma float_control(precise,on,push)
SfntStatus variation_instance_prepare(const ItemVariationStore& store,ConstSlice<double> coordinates,
                                     Slice<double> storage,VariationInstance* output,u32 work,VariationCoordinatePrecision precision) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    if(!store.table.data || coordinates.count!=store.axis_count || (coordinates.count && !coordinates.data))return SfntStatus::invalid_input;
    if(storage.count<store.region_count || (store.region_count && !storage.data))return SfntStatus::output_too_small;
    for(u32 i=0;i<coordinates.count;++i)if(!(coordinates[i]>=-1 && coordinates[i]<=1))return SfntStatus::invalid_input;
    for(u32 region=0;region<store.region_count;++region) {
        if(!work)return SfntStatus::resource_limit;--work;
        double scalar=1;
        for(u32 axis=0;axis<store.axis_count;++axis) {
            if(!work)return SfntStatus::resource_limit;--work;
            const u32 at=(region*store.axis_count+axis)*6;
            const double start=static_cast<i16>(r16(store.regions,at))/16384.0;
            const double peak=static_cast<i16>(r16(store.regions,at+2))/16384.0;
            const double end=static_cast<i16>(r16(store.regions,at+4))/16384.0;
            double value=coordinates[axis];
            if(precision==VariationCoordinatePrecision::fixed_16) {
                const double shifted=value*65536.0+.5;
                const i32 integer=static_cast<i32>(shifted);
                value=(integer-(static_cast<double>(integer)>shifted?1:0))/65536.0;
            }
            // The OpenType interpolation algorithm ignores invalid axis
            // ordering/cross-zero supports and zero-peak axes.
            if(start>peak || peak>end || (start<0 && end>0 && peak!=0) || peak==0)continue;
            if(value<start || value>end){scalar=0;break;}
            if(value==peak)continue;
            scalar*=value<peak?(value-start)/(peak-start):(end-value)/(end-peak);
        }
        storage[region]=scalar;
    }
    *output={store,{storage.data,store.region_count}};return SfntStatus::ok;
}
SfntStatus item_variation_delta_cached(const VariationInstance& instance,u32 index,double* output,VariationDeltaBlock* block,VariationArithmetic arithmetic) noexcept {
    if(!output)return SfntStatus::invalid_input;*output=0;
    if(!block)return SfntStatus::invalid_input;
    if(!instance.store.table.data || instance.scalars.count!=instance.store.region_count ||
       (instance.scalars.count && !instance.scalars.data))return SfntStatus::invalid_input;
    if(index==0xffffffff)return SfntStatus::ok;
    const u32 outer=index>>16;
    if(block->outer!=outer) {
        // Invalidate before decoding: a failed outer lookup must never leave
        // an old key paired with cleared/partially decoded block contents.
        block->outer=65536;
        const auto status=item_variation_data(instance.store,static_cast<u16>(outer),&block->data);
        if(status!=SfntStatus::ok)return status;
        block->all_zero=true;
        for(u32 i=0;i<block->data.region_count;++i)if(instance.scalars[r16(block->data.region_indices,i*2)]!=0) {
            block->all_zero=false;break;
        }
        block->outer=outer;
    }
    const auto& data=block->data;
    if(data.absent)return SfntStatus::ok;
    const u32 row=index&0xffff;
    if(row>=data.item_count)return SfntStatus::malformed;
    // Keep row validation even when this block has no active region. Its
    // contribution is then exactly +0 in both supported accumulation modes.
    if(block->all_zero)return SfntStatus::ok;
    u32 at=row*data.stride;double sum=0;__m128 narrow=_mm_setzero_ps();
    for(u32 i=0;i<data.region_count;++i) {
        const u32 width=i<data.word_count?(data.long_words?4u:2u):(data.long_words?2u:1u);
        const i32 delta=width==4?static_cast<i32>(r32(data.deltas,at)):
                        width==2?static_cast<i16>(r16(data.deltas,at)):static_cast<signed char>(data.deltas[at]);
        at+=width;
        const double scalar=instance.scalars[r16(data.region_indices,i*2)];
        if(arithmetic==VariationArithmetic::float_32)narrow=_mm_add_ss(narrow,_mm_mul_ss(_mm_set_ss(static_cast<float>(delta)),_mm_set_ss(static_cast<float>(scalar))));
        else sum+=delta*scalar;
    }
    *output=arithmetic==VariationArithmetic::float_32?_mm_cvtss_f32(narrow):sum;return SfntStatus::ok;
}
SfntStatus item_variation_delta(const VariationInstance& instance,u32 index,double* output,VariationArithmetic arithmetic) noexcept {
    VariationDeltaBlock block;
    return item_variation_delta_cached(instance,index,output,&block,arithmetic);
}
#pragma float_control(pop)
} // namespace maicord
