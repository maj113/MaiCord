#include "maicord/text/color_font.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
u8 data[3072];
void put16(u8* p,u16 x) noexcept {p[0]=static_cast<u8>(x>>8);p[1]=static_cast<u8>(x);}
void put24(u8* p,u32 x) noexcept {p[0]=static_cast<u8>(x>>16);put16(p+1,static_cast<u16>(x));}
void put32(u8* p,u32 x) noexcept {byte_store_be32(p,x);}
void record(u32 i,u32 tag,u32 at,u32 length) noexcept {auto* p=data+12+i*16;put32(p,tag);put32(p+8,at);put32(p+12,length);}
void fixture() noexcept {
    clear_bytes(data,sizeof(data));put32(data,0x10000);put16(data+4,3);
    record(0,sfnt_tag('C','O','L','R'),512,2048);record(1,sfnt_tag('C','P','A','L'),128,80);record(2,sfnt_tag('m','a','x','p'),256,6);
    put32(data+256,0x5000);put16(data+260,4);
    auto* p=data+128;put16(p,1);put16(p+2,3);put16(p+4,2);put16(p+6,4);put32(p+8,32);put16(p+14,1);
    put32(p+16,48);put32(p+20,56);put32(p+24,60);
    for(u32 i=0;i<4;++i){p[32+i*4]=static_cast<u8>(i+1);p[33+i*4]=static_cast<u8>(i+11);p[34+i*4]=static_cast<u8>(i+21);p[35+i*4]=static_cast<u8>(i+101);}
    put32(p+48,1);put32(p+52,2);put16(p+56,256);put16(p+58,0xffff);
    put16(p+60,260);put16(p+62,0xffff);put16(p+64,262);
    p=data+512;put16(p,1);put16(p+2,1);put32(p+4,200);put32(p+8,208);put16(p+12,1);
    put32(p+14,64);put32(p+18,96);put32(p+22,112);
    put32(p+64,2);put16(p+68,0);put32(p+70,96);put16(p+74,1);put32(p+76,128);
    put32(p+96,2);put32(p+100,160);put32(p+104,160);
    p[112]=1;put32(p+113,1);put16(p+117,0);put16(p+119,1);put24(p+121,208);
    p[160]=1;p[161]=2;put32(p+162,0);
    p[192]=10;put24(p+193,64);put16(p+196,2);
    put16(p+200,0);put16(p+202,0);put16(p+204,1);put16(p+208,2);put16(p+210,0xffff);
    p[256]=2;put16(p+257,1);put16(p+259,0x4000);
    p[320]=1;put16(p+321,static_cast<u16>(-10));put16(p+323,static_cast<u16>(-20));put16(p+325,40);put16(p+327,50);
}
SfntStatus open(ConstSlice<u8> input,ColrFont* font) noexcept {
    SfntFace face{};auto status=sfnt_open(input,0,&face);return status==SfntStatus::ok?colr_open(face,font):status;
}
bool palettes_and_graphs() noexcept {
    fixture();ColrFont font{};if(open({data,sizeof(data)},&font)!=SfntStatus::ok)return false;
    CpalColor color{};
    if(cpal_color(font.palette,1,0,&color)!=SfntStatus::ok || color.red!=22 || color.green!=12 || color.blue!=2 || color.alpha!=102 ||
       cpal_color(font.palette,0,1,&color)!=SfntStatus::ok || color.red!=22 ||
       cpal_color(font.palette,0,0xffff,&color)!=SfntStatus::unsupported_sequence ||
       cpal_color(font.palette,2,0,&color)!=SfntStatus::invalid_input)return false;
    ColrLayerRange layers{};ColrLegacyLayer layer{};
    if(colr_legacy_layers(font,0,&layers)!=SfntStatus::ok || layers.count!=1 ||
       colr_legacy_layer(font,0,&layer)!=SfntStatus::ok || layer.glyph!=2 || layer.palette!=0xffff)return false;
    ColrClip clip{};
    if(colr_clip(font,0,&clip)!=SfntStatus::ok || !clip.present || clip.variable || clip.minimum_x!=-10 || clip.minimum_y!=-20 || clip.maximum_x!=40 ||
       colr_clip(font,2,&clip)!=SfntStatus::ok || clip.present)return false;
    fixture();put16(data+128,0);
    if(open({data,sizeof(data)},&font)!=SfntStatus::ok)return false;
    return true;
}

bool empty_paint(const ColrPaint& paint) noexcept {
    for(double value:paint.values)if(value!=0)return false;
    return !paint.offset&&!paint.child[0]&&!paint.child[1]&&!paint.color_line&&!paint.first_layer&&!paint.variation_index&&
        !paint.glyph&&!paint.palette&&!paint.format&&!paint.mode&&!paint.layer_count&&!paint.value_count&&!paint.variable;
}
bool paint_formats() noexcept {
    for(u32 f=1;f<=32;++f) {
        fixture();auto* t=data+512;auto* p=t+384;p[0]=static_cast<u8>(f);
        const bool variable=f==3 || f==5 || f==7 || f==9 || (f>=13 && f<=31 && (f&1));
        if(f==1){p[1]=2;put32(p+2,0);}
        else if(f<=3){put16(p+1,0xffff);put16(p+3,0x2000);if(variable)put32(p+5,37);}
        else if(f<=9) {
            put24(p+1,1024-384);const u32 count=f<=7?6u:4u;
            for(u32 i=0;i<count;++i)put16(p+4+i*2,static_cast<u16>(0x2000+i));
            if(variable)put32(p+4+count*2,37);
        } else if(f==11)put16(p+1,1);
        else {
            put24(p+1,800-384);
            if(f==10)put16(p+4,2);
            else if(f==32){p[4]=27;put24(p+5,832-384);}
            else if(f<=13){put24(p+4,900-384);for(u32 i=0;i<6;++i)put32(t+900+i*4,0x00018000+i);if(variable)put32(t+924,37);}
            else {const u8 counts[]{2,2,4,1,3,1,3,2,4};const u32 count=counts[(f-14)/2];for(u32 i=0;i<count;++i)put16(p+4+i*2,static_cast<u16>(0x2000+i));if(variable)put32(p+4+count*2,37);}
        }
        t[800]=t[832]=2;put16(t+803,0x4000);put16(t+835,0x4000);
        t[1024]=2;put16(t+1025,3);
        const u32 stride=variable?10u:6u;
        for(u32 i=0;i<3;++i){auto* s=t+1027+i*stride;put16(s,i==0?0x4000:0);put16(s+2,i==2?0xffff:1);put16(s+4,0x2000);if(variable)put32(s+6,41+i*2);}
        put32(t+70,384-64);
        ColrFont font{};ColrPaint paint{};
        if(open({data,sizeof(data)},&font)!=SfntStatus::ok || colr_paint_read(font,384,&paint)!=SfntStatus::ok || paint.format!=f || paint.variable!=variable)return false;
        if(variable && paint.variation_index!=37)return false;
        if(f<=3 && f>=2 && (paint.values[0]!=.5 || paint.palette!=0xffff))return false;
        if(f>=4 && f<=9) {
            ColrColorLine line{};ColrColorStop stop{};
            if(colr_color_line(font,paint,&line)!=SfntStatus::ok || line.count!=3 || line.extend!=2 || line.variable!=variable ||
               colr_color_stop(font,line,0,&stop)!=SfntStatus::ok || stop.position!=1 || stop.alpha!=.5 ||
               colr_color_stop(font,line,2,&stop)!=SfntStatus::ok || stop.position!=0 || stop.palette!=0xffff ||
               (variable && stop.variation_index!=45))return false;
        }
        if(f>=12 && f<=13 && paint.values[0]!=1.5)return false;
        if(f>=14 && f<=31 && paint.values[0]!=(f<=15?8192:.5))return false;
        // Reuse a populated output across early and late failures. A direct
        // decode must not publish any partially written field on rejection.
        const auto valid=paint;
        for(u32 length=384;length<448;++length){
            auto truncated=font;truncated.table.count=length;paint=valid;
            if(colr_paint_read(truncated,384,&paint)!=SfntStatus::ok&&!empty_paint(paint))return false;
        }
        paint=valid;ColrFont absent{};
        if(colr_paint_read(absent,384,&paint)!=SfntStatus::invalid_input||!empty_paint(paint))return false;
    }
    return true;
}
bool malformed() noexcept {
    fixture();ColrFont font{};put16(data+128+14,2);
    if(open({data,sizeof(data)},&font)!=SfntStatus::malformed || font.table.data)return false;
    fixture();put16(data+512+74,0);
    if(open({data,sizeof(data)},&font)!=SfntStatus::malformed)return false;
    fixture();put16(data+512+119,4);
    if(open({data,sizeof(data)},&font)!=SfntStatus::malformed)return false;
    const u32 invalid_offsets[]{0u,0xffffffffu,1952u};
    for(u32 relative:invalid_offsets) {
        fixture();put32(data+512+100,relative);u32 at=99;
        if(open({data,sizeof(data)},&font)!=SfntStatus::ok)return false;
        if(colr_layer_paint(font,0,&at)!=(relative?SfntStatus::truncated:SfntStatus::malformed)||at)return false;
        if(colr_layer_paint(font,1,&at)!=SfntStatus::ok||at!=256)return false;
    }
    fixture();if(open({data,sizeof(data)},&font)!=SfntStatus::ok)return false;
    ColrPaint paint{};data[512+256]=33;
    if(open({data,sizeof(data)},&font)!=SfntStatus::ok)return false;
    if(colr_paint_read(font,256,&paint)!=SfntStatus::unsupported || !empty_paint(paint))return false;
    data[512+256]=2;put16(data+512+257,3);
    if(open({data,sizeof(data)},&font)!=SfntStatus::ok)return false;
    if(colr_paint_read(font,256,&paint)!=SfntStatus::malformed || !empty_paint(paint))return false;
    return true;
}
bool deferred_payloads() noexcept {
    const u16 values[]{0,1,2,3,4,65535};
    for(u16 glyph:values)for(u16 palette:values){
        fixture();put16(data+720,glyph);put16(data+722,palette);ColrFont font{};
        if(open({data,sizeof(data)},&font)!=SfntStatus::ok)return false;
        u32 paint=0;if(colr_base_paint(font,0,&paint)!=SfntStatus::ok||paint!=160)return false;
        ColrLegacyLayer layer{99,99};const bool valid=glyph<4&&(palette<3||palette==65535);
        if(colr_legacy_layer(font,0,&layer)!=(valid?SfntStatus::ok:SfntStatus::malformed))return false;
        if(valid?(layer.glyph!=glyph||layer.palette!=palette):(layer.glyph||layer.palette))return false;
    }
    for(u16 first:values)for(u16 count:values){
        fixture();put16(data+714,first);put16(data+716,count);ColrFont font{};
        if(open({data,sizeof(data)},&font)!=SfntStatus::ok)return false;
        ColrLayerRange layers{99,99};const bool valid=first<=1&&count<=1-first;
        if(colr_legacy_layers(font,0,&layers)!=(valid?SfntStatus::ok:SfntStatus::malformed))return false;
        if(valid?(layers.first!=first||layers.count!=count):(layers.first||layers.count))return false;
    }
    const u32 offsets[]{0,1984,0xffffffffu};
    for(u32 offset:offsets){
        fixture();put32(data+582,offset);ColrFont font{};u32 paint=99;
        if(open({data,sizeof(data)},&font)!=SfntStatus::ok||
           colr_base_paint(font,0,&paint)!=(offset?SfntStatus::truncated:SfntStatus::malformed)||paint||
           colr_base_paint(font,1,&paint)!=SfntStatus::ok||paint!=192)return false;
    }
    for(u32 format=0;format<4;++format)for(u32 length=1;length<=13;++length){
        fixture();const u32 at=2048-length;put24(data+633,at-112);data[512+at]=static_cast<u8>(format);
        ColrFont font{};ColrClip clip{};if(open({data,sizeof(data)},&font)!=SfntStatus::ok)return false;
        const auto expected=format!=1&&format!=2?SfntStatus::unsupported:
            length<(format==1?9u:13u)?SfntStatus::truncated:SfntStatus::ok;
        if(colr_clip(font,0,&clip)!=expected||(expected!=SfntStatus::ok&&clip.present))return false;
        if(colr_clip(font,2,&clip)!=SfntStatus::ok||clip.present)return false;
    }
    for(u32 offset:offsets){
        fixture();put24(data+633,offset);ColrFont font{};ColrClip clip{};
        if(open({data,sizeof(data)},&font)!=SfntStatus::ok||
           colr_clip(font,0,&clip)!=(offset?SfntStatus::truncated:SfntStatus::malformed)||clip.present)return false;
    }
    return true;
}
bool guards() noexcept {
    u8* memory=static_cast<u8*>(VirtualAlloc(nullptr,8192,MEM_RESERVE,PAGE_NOACCESS));if(!memory)return false;
    if(!VirtualAlloc(memory,4096,MEM_COMMIT,PAGE_READWRITE)){VirtualFree(memory,0,MEM_RELEASE);return false;}
    bool ok=true;u32 random=0xc010f019;
    for(u32 i=0;i<20000 && ok;++i) {
        fixture();random^=random<<13;random^=random>>17;random^=random<<5;
        const u32 size=i<2560?i:2560;
        if(i>=2560)data[(i&1)?512+random%1024:random%size]^=static_cast<u8>((random>>16)|1);
        if(size>=512)put32(data+24,size-512);
        auto* input=memory+4096-size;copy_bytes(input,data,size);ColrFont font{};
        if(open({input,size},&font)==SfntStatus::ok) {
            ColrPaint paint{};colr_paint_read(font,256,&paint);
            CpalColor color{};cpal_color(font.palette,0,0,&color);
            for(u16 glyph=0;glyph<4;++glyph){ColrLayerRange layers{};ColrClip clip{};
                colr_clip(font,glyph,&clip);
                if(colr_legacy_layers(font,glyph,&layers)==SfntStatus::ok)
                    for(u32 j=0;j<layers.count;++j){ColrLegacyLayer layer{};colr_legacy_layer(font,layers.first+j,&layer);}
            }
        }
    }
    VirtualFree(memory,0,MEM_RELEASE);return ok;
}
}
bool color_font_tests() noexcept {return palettes_and_graphs() && paint_formats() && malformed() && deferred_payloads() && guards();}
} // namespace maicord
