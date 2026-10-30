#include "maicord/text/tt_bold_points.hpp"
#include "maicord/core/byte_order.hpp"
namespace maicord {namespace {
#include "generated/bold_points_fixture.inc"
bool corpus() noexcept {
    const u8* row=bold_points_fixture+4;const u32 count=byte_load_le32(bold_points_fixture);
    for(u32 c=0;c<count;++c){const u32 units=byte_load_le32(row),n=byte_load_le32(row+4),nc=byte_load_le32(row+8),direction=byte_load_le32(row+12);row+=16;
        if(n>128||nc>16)return false;u32 ends[16]{};SfntOutlinePoint points[130]{};
        points[0].x=123;points[n+1].x=456;
        for(u32 i=0;i<nc;++i){ends[i]=byte_load_le32(row);row+=4;}
        const auto* expected=row;
        for(u32 i=0;i<n;++i){points[i+1]={static_cast<float>(static_cast<i32>(byte_load_le32(row)))/64,static_cast<float>(static_cast<i32>(byte_load_le32(row+4)))/64,static_cast<u8>(i),{7,8,9}};row+=16;}
        // The fixture gives one direction for every contour; markers sit on each contour's first point.
        for(u32 i=0,start=0;i<nc;start=ends[i++]+1)points[start+1].reserved[0]=static_cast<u8>(direction&1);
        u32 used=99;
        if(tt_bold_marked_points({points+1,n},{ends,nc},static_cast<u16>(units),(direction&256)!=0,n*3+nc-1,&used)!=SfntStatus::resource_limit||used)return false;
        if(tt_bold_marked_points({points+1,n},{ends,nc},static_cast<u16>(units),(direction&256)!=0,n*3+nc,&used)!=SfntStatus::ok||used!=n*3+nc)return false;
        if(points[0].x!=123||points[n+1].x!=456)return false;
        for(u32 i=0;i<n;++i){const auto& p=points[i+1];if(p.x!=static_cast<float>(static_cast<i32>(byte_load_le32(expected+i*16+8)))/64||p.y!=static_cast<float>(static_cast<i32>(byte_load_le32(expected+i*16+12)))/64||p.flags!=static_cast<u8>(i)||p.reserved[1]!=8||p.reserved[2]!=9)return false;}
    }return row==bold_points_fixture+sizeof(bold_points_fixture);
}
bool invalid() noexcept {
    SfntOutlinePoint p[4]{{0,0},{0,10},{10,10},{10,0}};u32 end=3;u32 used=99;
    const u32 bad[]{0x7f800000,0xff800000,0x7fc00000,0x3dcccccd,0x4c000000};
    for(u32 bits:bad){copy_bytes(&p[2].x,&bits,4);if(tt_bold_marked_points({p,4},{&end,1},2048,false,20,&used)!=SfntStatus::malformed||p[0].x!=0||p[1].y!=10)return false;}
    return true;
}
}
bool tt_bold_points_tests() noexcept {return corpus()&&invalid();}
}
