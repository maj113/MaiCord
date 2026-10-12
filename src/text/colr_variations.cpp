#include "maicord/text/colr_variations.hpp"
#include "variation_delta.hpp"

namespace maicord {
namespace {
#pragma float_control(precise,on,push)
double floor_value(double value) noexcept;
SfntStatus adjustment(const ColrVariationInstance& instance,VariationDeltaBlock& block,u32 base,u32 field,double* value) noexcept {
    *value=0;
    if(!instance.present)return SfntStatus::ok;
    if(instance.mode==ColrVariationMode::standard) {
        if(base==0xffffffff)return SfntStatus::ok;
        if(base>0xffffffffu-field)return SfntStatus::malformed;
    }
    // The current Windows paint reader forms per-field indices with uint32
    // wrapping before map lookup, including a base of 0xffffffff. Keep that
    // Windows behavior isolated from the standards-oriented policy.
    u32 index=0;const auto status=variation_index_map_get(instance.map,base+field,&index);
    if(status!=SfntStatus::ok)return status;
    const auto computed=item_variation_delta_cached(instance.instance,index,value,&block,
        instance.mode==ColrVariationMode::paint_reader?VariationArithmetic::float_32:VariationArithmetic::wide);
    if(computed==SfntStatus::ok && instance.mode==ColrVariationMode::paint_reader)*value=floor_value(*value+.5);
    return computed;
}
double clamp_alpha(double value) noexcept {return value<0?0:value>1?1:value;}
double floor_value(double value) noexcept {const i64 n=static_cast<i64>(value);return static_cast<double>(n)-(static_cast<double>(n)>value?1:0);}
double ceil_value(double value) noexcept {const i64 n=static_cast<i64>(value);return static_cast<double>(n)+(static_cast<double>(n)<value?1:0);}
bool later(const ColrResolvedColorStop& a,const ColrResolvedColorStop& b) noexcept {
    return a.position>b.position || (a.position==b.position && a.original_index>b.original_index);
}
void sift(Slice<ColrResolvedColorStop> stops,u32 root,u32 count) noexcept {
    while(root<count/2) {
        u32 child=root*2+1;
        if(child+1<count && later(stops[child+1],stops[child]))++child;
        if(!later(stops[child],stops[root]))return;
        const auto value=stops[root];stops[root]=stops[child];stops[child]=value;root=child;
    }
}
}

SfntStatus colr_variation_data_open(const ColrFont& font,ColrVariationData* output,u32 work) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!font.table.data)return SfntStatus::invalid_input;
    if(!font.variation_store)return SfntStatus::ok;
    ColrVariationData result{};
    auto status=item_variation_store_open(font.table,font.variation_store,&result.store,work);
    if(status!=SfntStatus::ok)return status;
    if(font.variation_map){status=variation_index_map_open(font.table,font.variation_map,&result.map,work);if(status!=SfntStatus::ok)return status;}
    result.present=true;*output=result;return SfntStatus::ok;
}
SfntStatus colr_variation_instance_prepare(const ColrVariationData& data,ConstSlice<double> coordinates,
                                          Slice<double> storage,ColrVariationInstance* output,u32 work,ColrVariationMode mode) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!data.present){output->mode=mode;return SfntStatus::ok;}
    ColrVariationInstance result{};
    const auto status=variation_instance_prepare(data.store,coordinates,storage,&result.instance,work,
        mode==ColrVariationMode::paint_reader?VariationCoordinatePrecision::fixed_16:VariationCoordinatePrecision::full);
    if(status!=SfntStatus::ok)return status;
    result.map=data.map;result.present=true;result.mode=mode;*output=result;return SfntStatus::ok;
}
SfntStatus colr_resolve_paint_values(const ColrVariationInstance& instance,const ColrPaint& paint,ColrPaintValues* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!paint.format || paint.format>32 || paint.value_count>6)return SfntStatus::invalid_input;
    ColrPaintValues result{};result.count=paint.value_count;
    VariationDeltaBlock block;
    const u32 base=paint.variable?paint.format-1:paint.format;
    u32 fractional=0;
    if(base==2)fractional=1;
    else if(base==8)fractional=12;
    else if(base>=16 && base<=30) {
        constexpr u8 masks[]{3,3,1,1,1,1,3,3};fractional=masks[(base-16)/2];
    }
    for(u32 i=0;i<result.count;++i) {
        double delta=0;
        if(paint.variable){const auto status=adjustment(instance,block,paint.variation_index,i,&delta);if(status!=SfntStatus::ok)return status;}
        const double divisor=base==12?65536.0:(fractional&(1u<<i))?16384.0:1.0;
        result.values[i]=paint.values[i]+delta/divisor;
    }
    if(base==2 && instance.mode==ColrVariationMode::standard)result.values[0]=clamp_alpha(result.values[0]);
    *output=result;return SfntStatus::ok;
}
SfntStatus colr_resolve_clip(const ColrVariationInstance& instance,const ColrClip& clip,ColrClip* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!clip.present){output->variation_index=0xffffffff;return SfntStatus::ok;}
    double values[]{clip.minimum_x,clip.minimum_y,clip.maximum_x,clip.maximum_y};
    VariationDeltaBlock block;
    for(u32 i=0;i<4;++i) {
        double delta=0;if(clip.variable){const auto status=adjustment(instance,block,clip.variation_index,i,&delta);if(status!=SfntStatus::ok)return status;}
        values[i]+=delta;
        if(!(values[i]>=-9.e18 && values[i]<=9.e18))return SfntStatus::malformed;
        values[i]=i<2?floor_value(values[i]):ceil_value(values[i]);
    }
    if(instance.mode==ColrVariationMode::paint_reader && (values[0]>=values[2] || values[1]>=values[3])) {
        output->variation_index=0xffffffff;return SfntStatus::ok;
    }
    *output={values[0],values[1],values[2],values[3],0xffffffff,true,false};return SfntStatus::ok;
}
SfntStatus colr_resolve_color_stops(const ColrFont& font,const ColrVariationInstance& instance,
                                   const ColrColorLine& line,Slice<ColrResolvedColorStop> output) noexcept {
    if(output.count<line.count || (line.count && !output.data))return SfntStatus::output_too_small;
    VariationDeltaBlock block;bool ordered=true;
    for(u32 i=0;i<line.count;++i) {
        ColrColorStop stop{};auto status=colr_color_stop(font,line,i,&stop);if(status!=SfntStatus::ok)return status;
        double position=0,alpha=0;
        if(stop.variable) {
            status=adjustment(instance,block,stop.variation_index,0,&position);if(status!=SfntStatus::ok)return status;
            status=adjustment(instance,block,stop.variation_index,1,&alpha);if(status!=SfntStatus::ok)return status;
        }
        const double opacity=stop.alpha+alpha/16384.0;
        output[i]={stop.position+position/16384.0,instance.mode==ColrVariationMode::standard?clamp_alpha(opacity):opacity,i,stop.palette};
        if(i && output[i-1].position>output[i].position)ordered=false;
    }
    // Equal offsets already retain file order. Test the resolved positions,
    // because variation can change their order even in a sorted input line.
    if(ordered)return SfntStatus::ok;
    // Heap sort with original-index tie breaking gives stable semantic order
    // in O(n log n) time and O(1) additional space, including adversarial stops.
    for(u32 i=line.count/2;i>0;--i)sift(output,i-1,line.count);
    for(u32 count=line.count;count>1;--count){const auto value=output[0];output[0]=output[count-1];output[count-1]=value;sift(output,0,count-1);}
    return SfntStatus::ok;
}
#pragma float_control(pop)
} // namespace maicord
