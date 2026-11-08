#include "maicord/text/scripts/devanagari_forms.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/text/opentype/context.hpp"
namespace maicord {namespace {
// Validated subtable bytes; reads past the end yield zero.
struct Bytes {
    ConstSlice<u8> data;
    u16 at(u32 offset) const noexcept {return offset+2<=data.count?byte_load_be16(data.data+offset):0;}
    Bytes child(u32 offset) const noexcept {return offset&&offset<data.count?Bytes{{data.data+offset,data.count-offset}}:Bytes{};}
};
bool charge(u64* work) noexcept {if(!*work)return false;--*work;return true;}
// LigatureSubst format 1: a ligature whose components are exactly the glyphs.
SfntStatus ligature(const SfntGsubSubtable& subtable,ConstSlice<u16> glyphs,bool* hit,u64* work) noexcept {
    const u32 index=sfnt_coverage_index(subtable.coverage,glyphs[0]);const Bytes table{subtable.table};
    if(index==sfnt_not_covered||index>=table.at(4))return SfntStatus::ok;
    const Bytes set=table.child(table.at(6+index*2));
    for(u32 i=0,count=set.at(0);i<count;++i){if(!charge(work))return SfntStatus::resource_limit;
        const Bytes rule=set.child(set.at(2+i*2));if(rule.at(2)!=glyphs.count)continue;
        bool same=true;for(u32 j=1;j<glyphs.count&&same;++j)same=rule.at(4+(j-1)*2)==glyphs[j];
        if(same){*hit=true;return SfntStatus::ok;}}
    return SfntStatus::ok;
}
// One (chained) context rule of format 1 (glyphs) or 2 (input classes).
bool rule_matches(const SfntContext& context,Bytes rule,ConstSlice<u16> glyphs,bool zero_context) noexcept {
    const bool chained=context.kind==SfntContextKind::chained;u32 at=0;
    if(chained){const u16 back=rule.at(0);if(zero_context&&back)return false;at=2+static_cast<u32>(back)*2;}
    if(rule.at(at)!=glyphs.count)return false;at+=chained?2:4;
    for(u32 j=1;j<glyphs.count;++j){const u16 value=rule.at(at+(j-1)*2);
        if(context.format==1?glyphs[j]!=value:sfnt_class_value(context.classes[chained?1:0],glyphs[j])!=value)return false;}
    return !chained||!zero_context||!rule.at(at+(glyphs.count-1)*2);
}
SfntStatus context(const SfntContext& context,ConstSlice<u16> glyphs,bool zero_context,bool* hit,u64* work) noexcept {
    const bool chained=context.kind==SfntContextKind::chained;const Bytes table{context.table};
    if(context.format==3){
        const u32 back=chained?table.at(2):0,input=chained?table.at(4+back*2):table.at(2),look=chained?table.at(6+(back+input)*2):0;
        if(input!=glyphs.count||(zero_context&&(back||look)))return SfntStatus::ok;
        for(u32 i=0;i<input;++i){if(!charge(work))return SfntStatus::resource_limit;
            if(sfnt_coverage_index(context.coverages[back+i],glyphs[i])==sfnt_not_covered)return SfntStatus::ok;}
        *hit=true;return SfntStatus::ok;}
    u32 index=sfnt_coverage_index(context.coverages[0],glyphs[0]);if(index==sfnt_not_covered)return SfntStatus::ok;
    u32 sets_at=6;
    if(context.format==2){index=sfnt_class_value(context.classes[chained?1:0],glyphs[0]);sets_at=chained?12:8;}
    if(index>=table.at(sets_at-2))return SfntStatus::ok;
    const Bytes set=table.child(table.at(sets_at+index*2));
    for(u32 i=0,count=set.at(0);i<count;++i){if(!charge(work))return SfntStatus::resource_limit;
        if(rule_matches(context,set.child(set.at(2+i*2)),glyphs,zero_context)){*hit=true;return SfntStatus::ok;}}
    return SfntStatus::ok;
}
}
SfntStatus sfnt_gsub_would_substitute(const SfntGsubProgram& program,ConstSlice<SfntLookupStep> steps,ConstSlice<u16> glyphs,
    bool zero_context,bool* result,u64* work) noexcept {
    if(!result||!work||!glyphs.count||!glyphs.data||(steps.count&&!steps.data))return SfntStatus::invalid_input;*result=false;
    for(u32 s=0;s<steps.count&&!*result;++s){
        const SfntGsubExecutableLookup* lookup=nullptr;auto status=sfnt_gsub_lookup_prepare(program,steps[s].lookup,&lookup);if(status!=SfntStatus::ok)return status;
        for(u32 i=0;i<lookup->subtables.count&&!*result;++i){if(!charge(work))return SfntStatus::resource_limit;const auto& subtable=lookup->subtables[i];
            switch(lookup->type){
            case 1:case 2:case 3:*result=glyphs.count==1&&sfnt_coverage_index(subtable.basic.coverage,glyphs[0])!=sfnt_not_covered;break;
            case 4:status=ligature(subtable.basic,glyphs,result,work);break;
            case 5:case 6:status=context(subtable.context,glyphs,zero_context,result,work);break;
            case 8:*result=glyphs.count==1&&sfnt_coverage_index(subtable.reverse.coverages[0],glyphs[0])!=sfnt_not_covered;break;
            default:break;}
            if(status!=SfntStatus::ok)return status;}
    }
    return SfntStatus::ok;
}
}
