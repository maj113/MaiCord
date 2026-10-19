#include "maicord/render/colr_source.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
u8 bytes[1024];
void p16(u8* p,u16 v) noexcept {p[0]=static_cast<u8>(v>>8);p[1]=static_cast<u8>(v);}
void p24(u8* p,u32 v) noexcept {p[0]=static_cast<u8>(v>>16);p[1]=static_cast<u8>(v>>8);p[2]=static_cast<u8>(v);}
void p32(u8* p,u32 v) noexcept {byte_store_be32(p,v);}
void fixture() noexcept {
    clear_bytes(bytes,sizeof(bytes));p32(bytes,0x10000);p16(bytes+4,3);
    const u32 tags[]{sfnt_tag('C','O','L','R'),sfnt_tag('C','P','A','L'),sfnt_tag('m','a','x','p')},at[]{256,128,192},lengths[]{512,22,6};
    for(u32 i=0;i<3;++i){p32(bytes+12+i*16,tags[i]);p32(bytes+20+i*16,at[i]);p32(bytes+24+i*16,lengths[i]);}
    p32(bytes+192,0x5000);p16(bytes+196,8);
    auto* p=bytes+128;p16(p+2,2);p16(p+4,1);p16(p+6,2);p32(p+8,14);
    p[14]=153;p[15]=102;p[16]=51;p[17]=255;p[18]=255;p[19]=0;p[20]=0;p[21]=128;
    p=bytes+256;p16(p,1);p16(p+2,1);p32(p+4,360);p32(p+8,366);p16(p+12,1);
    p32(p+14,34);p32(p+18,80);p32(p+22,100);p32(p+34,5);
    const u32 roots[]{140,220,270,330,340};for(u32 i=0;i<5;++i){p16(p+38+i*6,static_cast<u16>(i));p32(p+40+i*6,roots[i]-34);}
    p32(p+80,2);p32(p+84,150-80);p32(p+88,170-80);
    p[100]=1;p32(p+101,1);p16(p+105,0);p16(p+107,4);p24(p+109,12);
    p[112]=1;p16(p+117,1000);p16(p+119,1000);
    p[140]=1;p[141]=2;p32(p+142,0);
    p[150]=10;p24(p+151,6);p16(p+154,0);p[156]=2;p16(p+157,0);p16(p+159,16384);
    p[170]=4;p24(p+171,20);p16(p+178,1000);p16(p+184,1000);
    p[190]=2;p16(p+191,3);
    p16(p+193,16384);p16(p+195,1);p16(p+197,8192);
    p16(p+199,0);p16(p+201,0xffff);p16(p+203,16384);
    p16(p+205,0);p16(p+207,0);p16(p+209,4096);
    p[220]=12;p24(p+221,7);p24(p+224,16);p[227]=2;p16(p+228,0);p16(p+230,16384);
    p32(p+240,65536);p32(p+244,65536);
    p[270]=6;p24(p+271,32);p16(p+278,40000);p16(p+284,300);
    p[302]=0;p16(p+303,1);p16(p+307,0);p16(p+309,16384);
    p[330]=11;p16(p+331,3);p[340]=11;p16(p+341,0);
    p16(p+360,6);p16(p+362,0);p16(p+364,1);p16(p+366,0);p16(p+368,0);
}
bool no_outline(void*,u16,GlyphOutline*) noexcept {return false;}
bool cases() noexcept {
    fixture();SfntFace face{};ColrFont font{};
    if(sfnt_open({bytes,sizeof(bytes)},0,&face)!=SfntStatus::ok || colr_open(face,&font)!=SfntStatus::ok)return false;
    ColrVariationInstance instance{};instance.mode=ColrVariationMode::quantized;
    ColrSourceFrame frames[65]{};ColrResolvedColorStop scratch[3]{};ColrNativeSource cursor{};
    cursor.font=&font;cursor.variation=&instance;cursor.frames={frames,65};cursor.stop_storage={scratch,3};cursor.outline_provider=no_outline;cursor.foreground={1,1,1,1};cursor.inverse_em=.001f;
    ColrPaintElement root{},child{};ColrRect clip{};ColrPaintSource source{};u32 bits=0;
    if(colr_source_begin(&cursor,0,1000,&root,&clip,&source)!=SfntStatus::ok || root.kind!=ColrPaintKind::layers || root.paint.layers.childCount!=2)return false;
    copy_bytes(&bits,&clip.bottom,4);if(bits!=0x80000000)return false;
    if(!source.first_child(source.context,&child) || child.kind!=ColrPaintKind::solid_glyph)return false;
    // Non-gradient leaves must reject stop reads. Restart is required after
    // the failed callback before another traversal.
    ColrGradientStop unused{};
    if(source.stops(source.context,{&unused,1}) || cursor.status!=SfntStatus::invalid_input)return false;
    if(colr_source_begin(&cursor,0,1000,&root,&clip,&source)!=SfntStatus::ok ||
       !source.first_child(source.context,&child))return false;
    copy_bytes(&bits,&child.paint.solidGlyph.color.value.r,4);if(bits!=0x3e4cccce)return false;
    if(!source.next_sibling(source.context,&child) || child.kind!=ColrPaintKind::linear_gradient)return false;
    ColrGradientStop stops[3]{};
    if(!source.stops(source.context,{stops,3}) || stops[0].position!=0 || stops[0].color.r!=1 || stops[1].position!=0 || stops[1].color.a!=.25f || stops[2].position!=1)return false;
    if(!source.parent(source.context) || cursor.depth!=0)return false;
    if(colr_source_begin(&cursor,1,1000,&root,&clip,&source)!=SfntStatus::ok || root.kind!=ColrPaintKind::transform)return false;
    copy_bytes(&bits,&root.paint.transform.m22,4);if(bits!=0x80000000)return false;
    if(colr_source_begin(&cursor,2,1000,&root,&clip,&source)!=SfntStatus::ok || root.paint.radialGradient.radius0>=0)return false;
    instance.mode=ColrVariationMode::standard;
    if(colr_source_begin(&cursor,2,1000,&root,&clip,&source)!=SfntStatus::ok || root.paint.radialGradient.radius0!=40.f)return false;
    instance.mode=ColrVariationMode::quantized;
    if(colr_source_begin(&cursor,3,1000,&root,&clip,&source)!=SfntStatus::ok || source.first_child(source.context,&child) || cursor.status!=SfntStatus::malformed)return false;
    if(colr_source_begin(&cursor,6,1000,&root,&clip,&source)!=SfntStatus::ok || root.kind!=ColrPaintKind::layers || root.paint.layers.childCount!=1 ||
       !source.first_child(source.context,&child) || child.kind!=ColrPaintKind::solid_glyph)return false;
    if(colr_source_begin(&cursor,0,0,&root,&clip,&source)!=SfntStatus::resource_limit)return false;
    cursor.frames.count=1;
    if(colr_source_begin(&cursor,0,1000,&root,&clip,&source)!=SfntStatus::ok || source.first_child(source.context,&child) || cursor.status!=SfntStatus::resource_limit)return false;
    cursor.frames.count=65;cursor.stop_storage.count=2;
    return colr_source_begin(&cursor,0,1000,&root,&clip,&source)==SfntStatus::ok && source.first_child(source.context,&child) && source.next_sibling(source.context,&child) &&
        !source.stops(source.context,{stops,3}) && cursor.status==SfntStatus::output_too_small;
}
bool legacy_payload_cases() noexcept {
    for(u32 mode=0;mode<4;++mode){
        fixture();
        if(mode==0)p16(bytes+622,8);
        if(mode==1)p16(bytes+624,2);
        if(mode==2)p16(bytes+618,2);
        if(mode==3)p16(bytes+256,0);
        SfntFace face{};ColrFont font{};ColrVariationInstance instance{};ColrSourceFrame frames[65]{};
        ColrNativeSource cursor{};cursor.font=&font;cursor.variation=&instance;cursor.frames={frames,65};
        cursor.outline_provider=no_outline;cursor.inverse_em=.001f;cursor.foreground={1,1,1,1};
        ColrPaintElement root{},child{};ColrRect clip{};ColrPaintSource source{};
        if(sfnt_open({bytes,sizeof(bytes)},0,&face)!=SfntStatus::ok||colr_open(face,&font)!=SfntStatus::ok)return false;
        if(mode<3&&(colr_source_begin(&cursor,0,1000,&root,&clip,&source)!=SfntStatus::ok||
                    !source.first_child(source.context,&child)))return false;
        const auto status=colr_source_begin(&cursor,6,1000,&root,&clip,&source);
        if(mode==2){if(status!=SfntStatus::malformed||source.first_child)return false;}
        else {
            if(status!=SfntStatus::ok||source.first_child(source.context,&child)!=(mode==3)||
               cursor.status!=(mode==3?SfntStatus::ok:SfntStatus::malformed))return false;
        }
    }
    return true;
}
bool numeric_cases() noexcept {
    fixture();auto* p=bytes+256;p[220]=24;p16(p+224,4109);
    SfntFace face{};ColrFont font{};ColrVariationInstance instance{};instance.mode=ColrVariationMode::quantized;
    ColrSourceFrame frames[65]{};ColrNativeSource cursor{};cursor.font=&font;cursor.variation=&instance;cursor.frames={frames,65};cursor.outline_provider=no_outline;cursor.foreground={1,1,1,1};cursor.inverse_em=.001f;
    ColrPaintElement root{};ColrRect clip{};ColrPaintSource source{};u32 bits=0;
    if(sfnt_open({bytes,sizeof(bytes)},0,&face)!=SfntStatus::ok || colr_open(face,&font)!=SfntStatus::ok || colr_source_begin(&cursor,1,1000,&root,&clip,&source)!=SfntStatus::ok)return false;
    copy_bytes(&bits,&root.paint.transform.m11,4);if(bits!=0x3f34914a)return false;
    p16(p+224,4154);
    if(colr_open(face,&font)!=SfntStatus::ok || colr_source_begin(&cursor,1,1000,&root,&clip,&source)!=SfntStatus::ok)return false;
    copy_bytes(&bits,&root.paint.transform.m11,4);if(bits!=1060306617)return false;
    // An actual one-axis COLR store with a large LONG_WORDS angle delta.
    fixture();p=bytes+256;p32(p+30,440);p[220]=25;p24(p+221,100);p16(p+224,4096);p32(p+226,0);
    p[320]=2;p16(p+321,0);p16(p+323,16384);
    p16(p+440,1);p32(p+442,12);p16(p+446,1);p32(p+448,22);
    p16(p+452,1);p16(p+454,1);p16(p+456,0);p16(p+458,16384);p16(p+460,16384);
    p16(p+462,1);p16(p+464,0x8001);p16(p+466,1);p16(p+468,0);p32(p+470,2000000000);
    ColrVariationData data{};double coordinate=.125,scalar=0;
    if(colr_open(face,&font)!=SfntStatus::ok || colr_variation_data_open(font,&data)!=SfntStatus::ok ||
       colr_variation_instance_prepare(data,{&coordinate,1},{&scalar,1},&instance,1000,ColrVariationMode::quantized)!=SfntStatus::ok ||
       colr_source_begin(&cursor,1,1000,&root,&clip,&source)!=SfntStatus::ok)return false;
    copy_bytes(&bits,&root.paint.transform.m11,4);if(bits!=0xbf7dff0a)return false;
    // Windows applies both center deltas to X for nonuniform VarScaleAroundCenter.
    p[220]=19;p16(p+224,8192);p16(p+226,12288);p16(p+228,100);p16(p+230,200);p32(p+232,0);
    p16(p+462,4);p16(p+464,1);p16(p+470,0);p16(p+472,0);p16(p+474,0);p16(p+476,10);coordinate=1;
    if(colr_open(face,&font)!=SfntStatus::ok || colr_variation_data_open(font,&data)!=SfntStatus::ok ||
       colr_variation_instance_prepare(data,{&coordinate,1},{&scalar,1},&instance,1000,ColrVariationMode::quantized)!=SfntStatus::ok ||
       colr_source_begin(&cursor,1,1000,&root,&clip,&source)!=SfntStatus::ok)return false;
    if(!(root.paint.transform.dx>.054f && root.paint.transform.dx<.056f && root.paint.transform.dy>-.051f))return false;
    instance.mode=ColrVariationMode::standard;
    if(colr_source_begin(&cursor,1,1000,&root,&clip,&source)!=SfntStatus::ok || !(root.paint.transform.dx<.051f && root.paint.transform.dy<-.052f))return false;
    // Per-field index wrapping is confined to the compatibility policy.
    p[220]=15;p16(p+224,1);p16(p+226,2);p32(p+228,0xffffffff);p16(p+470,100);
    if(colr_open(face,&font)!=SfntStatus::ok || colr_variation_data_open(font,&data)!=SfntStatus::ok ||
       colr_variation_instance_prepare(data,{&coordinate,1},{&scalar,1},&instance,1000,ColrVariationMode::quantized)!=SfntStatus::ok ||
       colr_source_begin(&cursor,1,1000,&root,&clip,&source)!=SfntStatus::ok || !(root.paint.transform.dy<-.101f))return false;
    instance.mode=ColrVariationMode::standard;
    return colr_source_begin(&cursor,1,1000,&root,&clip,&source)==SfntStatus::ok && root.paint.transform.dy>-.003f;
}
bool walk(const ColrPaintSource& source,const ColrPaintElement& element,u32* work) noexcept {
    if(!*work)return false;--*work;u32 count=0;
    switch(element.kind) {
    case ColrPaintKind::layers:count=element.paint.layers.childCount;break;
    case ColrPaintKind::glyph:case ColrPaintKind::color_glyph:case ColrPaintKind::transform:count=1;break;
    case ColrPaintKind::composite:count=2;break;
    case ColrPaintKind::linear_gradient:case ColrPaintKind::radial_gradient:case ColrPaintKind::sweep_gradient:{
        const u32 stops=element.kind==ColrPaintKind::linear_gradient?element.paint.linearGradient.gradientStopCount:
            element.kind==ColrPaintKind::radial_gradient?element.paint.radialGradient.gradientStopCount:element.paint.sweepGradient.gradientStopCount;
        ColrGradientStop buffer[3]{};return stops<=3 && source.stops(source.context,{buffer,stops});}
    default:return true;
    }
    if(!count)return true;ColrPaintElement child{};if(!source.first_child(source.context,&child))return false;
    for(u32 i=0;i<count;++i){if(!walk(source,child,work))return false;if(i+1<count && !source.next_sibling(source.context,&child))return false;}
    return source.parent(source.context);
}
bool guarded() noexcept {
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!memory)return false;
    DWORD old=0;if(!VirtualProtect(memory+4096,4096,PAGE_NOACCESS,&old)){VirtualFree(memory,0,MEM_RELEASE);return false;}
    fixture();u32 random=0x89ad1123;
    for(u32 i=0;i<20000;++i) {
        random^=random<<13;random^=random>>17;random^=random<<5;const u32 size=i%4?768u:random%769;
        auto* input=memory+4096-size;copy_bytes(input,bytes,size);if(size)input[(random>>8)%size]^=static_cast<u8>(random|1);
        SfntFace face{};ColrFont font{};if(sfnt_open({input,size},0,&face)!=SfntStatus::ok || colr_open(face,&font)!=SfntStatus::ok)continue;
        ColrVariationInstance instance{};ColrSourceFrame frames[65]{};ColrResolvedColorStop stops[3]{};ColrNativeSource cursor{};
        cursor.font=&font;cursor.variation=&instance;cursor.frames={frames,65};cursor.stop_storage={stops,3};cursor.outline_provider=no_outline;cursor.inverse_em=.001f;
        ColrPaintElement root{};ColrRect clip{};ColrPaintSource source{};
        if(colr_source_begin(&cursor,static_cast<u16>(i%7),200,&root,&clip,&source)==SfntStatus::ok){u32 work=200;walk(source,root,&work);}
    }
    VirtualFree(memory,0,MEM_RELEASE);return true;
}
bool glyph_child_cases() noexcept {
    fixture();SfntFace face{};ColrFont font{};
    if(sfnt_open({bytes,sizeof(bytes)},0,&face)!=SfntStatus::ok || colr_open(face,&font)!=SfntStatus::ok)return false;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!memory)return false;
    struct Release {u8* p;~Release() noexcept {VirtualFree(p,0,MEM_RELEASE);}} release{memory};
    DWORD old=0;if(!VirtualProtect(memory+4096,4096,PAGE_NOACCESS,&old))return false;
    auto* table=memory+4096-font.table.count;copy_bytes(table,font.table.data,font.table.count);font.table.data=table;
    ColrVariationInstance instance{};instance.mode=ColrVariationMode::quantized;
    ColrSourceFrame frames[65]{};ColrResolvedColorStop scratch[3]{};ColrNativeSource cursor{};
    cursor.font=&font;cursor.variation=&instance;cursor.frames={frames,65};cursor.stop_storage={scratch,3};
    cursor.outline_provider=no_outline;cursor.inverse_em=.001f;cursor.foreground={1,1,1,1};
    ColrPaintElement root{},child{};ColrRect clip{};ColrPaintSource source{};
    table[140]=10;p16(table+144,0);
    // Both solid formats retain fusion, including alpha and foreground tracking.
    for(u8 format=2;format<=3;++format){
        p24(table+141,16);table[156]=format;p16(table+157,0xffff);p16(table+159,8192);p32(table+161,0);
        if(colr_source_begin(&cursor,0,100,&root,&clip,&source)!=SfntStatus::ok ||
           root.kind!=ColrPaintKind::solid_glyph || root.paint.solidGlyph.color.value.a!=.5f || !cursor.uses_foreground)return false;
    }
    // Static fusion and the variable reader must agree at signed F2DOT14
    // extremes, including the standards policy's alpha clamping.
    const i16 encoded_alpha[]{-32768,-16384,-1,0,1,8192,16384,32767};
    const float expected_alpha[]{-2.f,-1.f,-1.f/16384,0.f,1.f/16384,.5f,1.f,32767.f/16384};
    for(u32 policy=0;policy<2;++policy)for(u8 format=2;format<=3;++format)for(u32 i=0;i<8;++i){
        instance.mode=policy?ColrVariationMode::standard:ColrVariationMode::quantized;
        table[156]=format;p16(table+159,static_cast<u16>(encoded_alpha[i]));
        const float expected=policy?(i<3?0.f:i==7?1.f:expected_alpha[i]):expected_alpha[i];
        if(colr_source_begin(&cursor,0,100,&root,&clip,&source)!=SfntStatus::ok ||
           root.kind!=ColrPaintKind::solid_glyph || root.paint.solidGlyph.color.value.a!=expected)return false;
    }
    instance.mode=ColrVariationMode::quantized;
    for(u8 format=2;format<=3;++format){
        table[156]=format;p16(table+157,2);
        if(colr_source_begin(&cursor,0,100,&root,&clip,&source)!=SfntStatus::malformed)return false;
    }
    p16(table+157,0xffff);
    // The gradient remains a child and its stop validation cannot be bypassed.
    p24(table+141,30);
    if(colr_source_begin(&cursor,0,100,&root,&clip,&source)!=SfntStatus::ok || root.kind!=ColrPaintKind::glyph ||
       !source.first_child(source.context,&child) || child.kind!=ColrPaintKind::linear_gradient)return false;
    ColrGradientStop stops[3]{};
    if(!source.stops(source.context,{stops,3}) || !source.parent(source.context) || cursor.depth!=0)return false;
    p16(table+195,2);
    if(colr_source_begin(&cursor,0,100,&root,&clip,&source)!=SfntStatus::ok || !source.first_child(source.context,&child) ||
       source.stops(source.context,{stops,3}) || cursor.status!=SfntStatus::malformed)return false;
    p16(table+195,1);
    if(colr_source_begin(&cursor,0,1,&root,&clip,&source)!=SfntStatus::ok ||
       source.first_child(source.context,&child) || cursor.status!=SfntStatus::resource_limit)return false;
    // Every child format ends at the guard page with only its format byte
    // available. Failure may occur at parent entry or child traversal; neither
    // path may consume a malformed child or permit reuse without restart.
    for(u8 format=0;format<=33;++format){
        p24(table+141,511-140);table[511]=format;
        auto status=colr_source_begin(&cursor,0,100,&root,&clip,&source);
        if(status==SfntStatus::ok){if(source.first_child(source.context,&child))return false;status=cursor.status;}
        if(status!=(format==0 || format==33?SfntStatus::unsupported:SfntStatus::truncated))return false;
        if(source.first_child && (source.first_child(source.context,&child) || source.next_sibling(source.context,&child) || source.parent(source.context)))return false;
        p24(table+141,30);
        if(colr_source_begin(&cursor,0,100,&root,&clip,&source)!=SfntStatus::ok || !source.first_child(source.context,&child))return false;
    }
    // A one-past-end child has no readable format byte at all.
    p24(table+141,512-140);
    return colr_source_begin(&cursor,0,100,&root,&clip,&source)==SfntStatus::truncated;
}
}
bool colr_scratch_cases() noexcept {
    fixture();SfntFace face{};ColrFont font{};ColrVariationInstance instance{};
    ColrSourceFrame frames[65]{};ColrNativeSource cursor{};Arena arena{};
    if(!arena_initialize(&arena,4096,0,4096))return false;
    struct Release {Arena& a;~Release() noexcept {arena_release(&a);}} release{arena};
    cursor.font=&font;cursor.variation=&instance;cursor.frames={frames,65};cursor.stop_arena=&arena;
    cursor.outline_provider=no_outline;cursor.inverse_em=.001f;cursor.foreground={1,1,1,1};
    ColrPaintElement root{},child{};ColrRect clip{};ColrPaintSource source{};ColrGradientStop stops[3]{};
    if(sfnt_open({bytes,sizeof(bytes)},0,&face)!=SfntStatus::ok || colr_open(face,&font)!=SfntStatus::ok ||
       colr_source_begin(&cursor,0,1000,&root,&clip,&source)!=SfntStatus::ok || cursor.uses_foreground ||
       !source.first_child(source.context,&child) || cursor.uses_foreground ||
       !source.next_sibling(source.context,&child) || !source.stops(source.context,{stops,3}) ||
       !cursor.uses_foreground || cursor.stop_storage.count!=3 || arena.committed!=4096)return false;
    auto* retained=cursor.stop_storage.data;
    if(!source.stops(source.context,{stops,3}) || cursor.stop_storage.data!=retained ||
       colr_source_begin(&cursor,1,1000,&root,&clip,&source)!=SfntStatus::ok || cursor.uses_foreground)return false;
    return true;
}
bool static_value_cases() noexcept {
    constexpr u8 formats[]{2,4,6,8,12,14,16,18,20,22,24,26,28,30};
    constexpr i16 fields[]{-32768,-16384,-1,0,1,8192,16384,32767};
    for(u32 policy=0;policy<2;++policy)for(u8 format:formats)for(i16 field:fields) {
        fixture();auto* p=bytes+256;clear_bytes(p+220,64);
        p[220]=format;
        if(format==2){p16(p+221,0xffff);p16(p+223,static_cast<u16>(field));}
        else if(format<=8) {
            p24(p+221,82);
            for(u32 i=0;i<(format==8?4u:6u);++i)p16(p+224+i*2,static_cast<u16>(field));
        } else {
            p24(p+221,100);p[320]=2;p16(p+321,0);p16(p+323,16384);
            if(format==12){p24(p+224,16);for(u32 i=0;i<6;++i)p32(p+236+i*4,static_cast<u32>(static_cast<i32>(field)*65536));}
            else {constexpr u8 counts[]{2,2,4,1,3,1,3,2,4};for(u32 i=0;i<counts[(format-14)/2];++i)p16(p+224+i*2,static_cast<u16>(field));}
        }
        SfntFace face{};ColrFont font{};ColrVariationInstance instance{};
        instance.mode=policy?ColrVariationMode::standard:ColrVariationMode::quantized;
        ColrSourceFrame frames[65]{};ColrNativeSource cursor{};
        cursor.font=&font;cursor.variation=&instance;cursor.frames={frames,65};
        cursor.outline_provider=no_outline;cursor.inverse_em=.001f;cursor.foreground={1,1,1,1};
        ColrPaintElement plain{},variable{};ColrRect clip{};ColrPaintSource source{};
        if(sfnt_open({bytes,sizeof(bytes)},0,&face)!=SfntStatus::ok || colr_open(face,&font)!=SfntStatus::ok ||
           colr_source_begin(&cursor,1,1000,&plain,&clip,&source)!=SfntStatus::ok)return false;
        p[220]=format+1;
        if(colr_source_begin(&cursor,1,1000,&variable,&clip,&source)!=SfntStatus::ok ||
           !bytes_equal(&plain,&variable,sizeof(plain)))return false;
        if(format==2) {
            const float alpha=static_cast<float>(field)/16384.f;
            const float expected=policy?maximum(0.f,minimum(1.f,alpha)):alpha;
            if(plain.paint.solid.value.a!=expected)return false;
        }
    }
    return true;
}
bool colr_source_tests() noexcept {return cases() && legacy_payload_cases() && numeric_cases() && guarded() && glyph_child_cases() && colr_scratch_cases() && static_value_cases();}
} // namespace maicord
