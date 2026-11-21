#include "maicord/text/opentype/layout_tables.hpp"
#include "maicord/core/byte_order.hpp"
#include "../../support/text_test_support.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "../../support/test_output.hpp"

namespace maicord {namespace {
void put(u8* bytes,u32 at,u16 value) noexcept {bytes[at]=static_cast<u8>(value>>8);bytes[at+1]=static_cast<u8>(value);}
bool units() noexcept {
    u8 data[40]{};SfntCoverage coverage{};SfntClassDef classes{};SfntDevice device{};u16 glyph=0;i16 delta=0;u32 index=0;
    put(data,0,1);put(data,2,3);put(data,4,0);put(data,6,3);put(data,8,7);
    if(sfnt_coverage_open({data,10},8,&coverage)!=SfntStatus::ok || coverage.covered_count!=3 || sfnt_coverage_index(coverage,3)!=1 || sfnt_coverage_index(coverage,2)!=sfnt_not_covered ||
       sfnt_coverage_glyph(coverage,2,&glyph)!=SfntStatus::ok || glyph!=7 || sfnt_coverage_glyph(coverage,3,&glyph)!=SfntStatus::glyph_out_of_range || glyph)return false;
    put(data,8,3);if(sfnt_coverage_open({data,10},8,&coverage)!=SfntStatus::malformed || coverage.format)return false;
    put(data,0,2);put(data,2,2);put(data,4,1);put(data,6,3);put(data,8,0);put(data,10,6);put(data,12,7);put(data,14,3);
    if(sfnt_coverage_open({data,16},8,&coverage)!=SfntStatus::ok || coverage.covered_count!=5 || sfnt_coverage_index(coverage,7)!=4 ||
       sfnt_coverage_glyph(coverage,3,&glyph)!=SfntStatus::ok || glyph!=6)return false;
    put(data,14,4);if(sfnt_coverage_open({data,16},8,&coverage)!=SfntStatus::malformed)return false;
    put(data,14,3);put(data,10,3);if(sfnt_coverage_open({data,16},8,&coverage)!=SfntStatus::malformed)return false;
    put(data,2,1);put(data,4,0);put(data,6,65535);put(data,8,0);
    if(sfnt_coverage_open({data,10},65536,&coverage)!=SfntStatus::ok || coverage.covered_count!=65536 || sfnt_coverage_index(coverage,65535)!=65535 ||
       sfnt_coverage_glyph(coverage,65535,&glyph)!=SfntStatus::ok || glyph!=65535 || sfnt_coverage_open({data,10},65535,&coverage)!=SfntStatus::malformed)return false;
    put(data,0,1);put(data,2,5);put(data,4,3);put(data,6,2);put(data,8,0);put(data,10,65535);
    if(sfnt_class_def_open({data,12},8,&classes)!=SfntStatus::ok || classes.max_class!=65535 || sfnt_class_value(classes,4)!=0 || sfnt_class_value(classes,5)!=2 ||
       sfnt_class_value(classes,6)!=0 || sfnt_class_value(classes,7)!=65535 || sfnt_class_value(classes,65535)!=0)return false;
    if(sfnt_class_def_open({data,12},7,&classes)!=SfntStatus::malformed || classes.format)return false;
    put(data,0,2);put(data,2,2);put(data,4,1);put(data,6,3);put(data,8,4);put(data,10,6);put(data,12,7);put(data,14,2);
    if(sfnt_class_def_open({data,16},8,&classes)!=SfntStatus::ok || classes.max_class!=4 || sfnt_class_value(classes,2)!=4 || sfnt_class_value(classes,5)!=0 || sfnt_class_value(classes,7)!=2)return false;
    put(data,10,3);if(sfnt_class_def_open({data,16},8,&classes)!=SfntStatus::malformed)return false;
    put(data,0,10);put(data,2,18);put(data,4,1);put(data,6,0x6c6c);put(data,8,0xbfff);
    const i16 samples[]{1,-2,-1,0,1,-2,-1,0,-2};
    if(sfnt_device_open({data,10},&device)!=SfntStatus::ok)return false;
    for(u32 ppem=0;ppem<65536;++ppem)if(sfnt_device_pixels(device,static_cast<u16>(ppem),&delta)!=SfntStatus::ok || delta!=(ppem>=10 && ppem<=18?samples[ppem-10]:0))return false;
    put(data,0,0);put(data,2,4);put(data,4,2);put(data,6,0x178f);put(data,8,0xa123);
    const i16 four[]{1,7,-8,-1,-6};
    if(sfnt_device_open({data,10},&device)!=SfntStatus::ok)return false;
    for(u32 i=0;i<5;++i)if(sfnt_device_pixels(device,static_cast<u16>(i),&delta)!=SfntStatus::ok || delta!=four[i])return false;
    put(data,0,65533);put(data,2,65535);put(data,4,3);put(data,6,0x7f80);put(data,8,0xffa5);
    const i16 eight[]{127,-128,-1};
    if(sfnt_device_open({data,10},&device)!=SfntStatus::ok)return false;
    for(u32 i=0;i<3;++i)if(sfnt_device_pixels(device,static_cast<u16>(65533+i),&delta)!=SfntStatus::ok || delta!=eight[i])return false;
    put(data,0,65535);put(data,2,2);put(data,4,0x8000);
    if(sfnt_device_open({data,6},&device)!=SfntStatus::ok || !sfnt_device_variation_index(device,&index) || index!=0xffff0002 ||
       sfnt_device_pixels(device,16,&delta)!=SfntStatus::unsupported || delta)return false;
    put(data,4,4);if(sfnt_device_open({data,10},&device)!=SfntStatus::unsupported || device.format)return false;
    put(data,0,1);put(data,2,0);
    if(sfnt_coverage_open({data,4},8,&coverage)!=SfntStatus::ok || coverage.covered_count || sfnt_coverage_index(coverage,0)!=sfnt_not_covered)return false;
    put(data,0,2);
    if(sfnt_class_def_open({data,4},8,&classes)!=SfntStatus::ok || classes.max_class || sfnt_class_value(classes,7))return false;
    return sfnt_coverage_open({},8,&coverage)==SfntStatus::invalid_input && sfnt_class_def_open({data,4},0,&classes)==SfntStatus::invalid_input &&
           sfnt_device_open({},&device)==SfntStatus::invalid_input && sfnt_class_value({},123)==0;
}
bool guards() noexcept {
    Win32SystemInfo info{};GetSystemInfo(info.native());const u32 page=info.dwPageSize;
    auto* memory=static_cast<u8*>(pages_allocate(page*2));if(!memory)return false;
    if(!pages_protect(memory+page, page, PageAccess::none)){pages_release(memory);return false;}
    u8 base[32]{};put(base,0,2);put(base,2,2);put(base,4,1);put(base,6,3);put(base,8,0);put(base,10,6);put(base,12,7);put(base,14,3);
    bool ok=true;u32 random=0x07c0a117;
    for(u32 iteration=0;iteration<20000 && ok;++iteration) {
        random^=random<<13;random^=random>>17;random^=random<<5;const u32 size=iteration<16?iteration:random%33;
        u8* bytes=memory+page-size;for(u32 i=0;i<size;++i)bytes[i]=base[i];
        if(iteration>=16 && size){const u32 at=(random>>8)%size;bytes[at]^=static_cast<u8>(random>>16);}
        SfntCoverage coverage{};SfntClassDef classes{};SfntDevice device{};const ConstSlice<u8> view{bytes,size};
        if(sfnt_coverage_open(view,128,&coverage)==SfntStatus::ok) {
            for(u32 glyph=0;glyph<128;++glyph) {const u32 index=sfnt_coverage_index(coverage,static_cast<u16>(glyph));if(index!=sfnt_not_covered){u16 found=0;if(index>=coverage.covered_count || sfnt_coverage_glyph(coverage,index,&found)!=SfntStatus::ok || found!=glyph)ok=false;}}
        }
        if(sfnt_class_def_open(view,128,&classes)==SfntStatus::ok)for(u32 glyph=0;glyph<128;++glyph)if(sfnt_class_value(classes,static_cast<u16>(glyph))>classes.max_class)ok=false;
        if(sfnt_device_open(view,&device)==SfntStatus::ok){i16 delta=0;const auto status=sfnt_device_pixels(device,device.start_or_outer,&delta);if(status!=SfntStatus::ok && status!=SfntStatus::unsupported)ok=false;}
    }
    pages_release(memory);return ok;
}
int run() noexcept {
    if(!units()){print(utf8_literal("FAIL layout units\n"));return 1;}if(!guards()){print(utf8_literal("FAIL layout guards\n"));return 2;}
    print(utf8_literal("PASS native layout common tables\n"));return 0;
}
}}
MAICORD_TEST_ENTRY(maicord_layout_tables_entry) {::maicord::test_process_exit(static_cast<UINT>(maicord::run()));}
