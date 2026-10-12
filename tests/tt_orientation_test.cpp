#include "maicord/text/tt_orientation.hpp"
#include "maicord/core/byte_order.hpp"
namespace maicord {namespace {
#include "generated/orientation_fixture.inc"
bool corpus() noexcept {
    const u8* row=orientation_fixture+4;const u32 cases=byte_load_le32(orientation_fixture);
    for(u32 c=0;c<cases;++c){const u32 n=byte_load_le32(row),nc=byte_load_le32(row+4);row+=8;
        if(n>128||nc>16)return false;u32 ends[16]{};u8 expected[16]{},out[18]{};SfntOutlinePoint points[128]{};
        for(u32 i=0;i<nc;++i){ends[i]=byte_load_le32(row);expected[i]=static_cast<u8>(byte_load_le32(row+4));row+=8;}
        for(u32 i=0;i<n;++i){points[i]={static_cast<float>(static_cast<i16>(byte_load_le16(row))),static_cast<float>(static_cast<i16>(byte_load_le16(row+2))),static_cast<u8>(byte_load_le32(row+4)),{7,8,9}};row+=8;}
        SfntOutlinePoint original[128]{};copy_bytes(original,points,n*sizeof(SfntOutlinePoint));out[0]=123;out[nc+1]=234;u32 full=0;
        if(tt_contour_directions({points,n},{ends,nc},{out+1,nc},1000000,&full)!=SfntStatus::ok||!bytes_equal(out+1,expected,nc)||!full)return false;
        const u32 budgets[]{0,1,full/2,full-1,full};
        for(u32 budget:budgets){u32 used=0;const auto status=tt_contour_directions({points,n},{ends,nc},{out+1,nc},budget,&used);
            if(status!=(budget<full?SfntStatus::resource_limit:SfntStatus::ok)||used>budget||out[0]!=123||out[nc+1]!=234||!bytes_equal(original,points,n*sizeof(SfntOutlinePoint)))return false;
            if(status==SfntStatus::ok&&!bytes_equal(out+1,expected,nc))return false;
        }
        SfntOutlinePoint marked[132]{};copy_bytes(marked+2,original,n*sizeof(SfntOutlinePoint));marked[0].x=123;marked[1].x=234;marked[n+2].x=345;
        u32 absolute_ends[16]{};for(u32 i=0;i<nc;++i)absolute_ends[i]=ends[i]+2;u32 marked_work=0;
        if(tt_mark_contour_directions({marked,n+2},{absolute_ends,nc},2,full,&marked_work)!=SfntStatus::ok||marked_work!=full)return false;
        if(marked[0].x!=123||marked[1].x!=234||marked[n+2].x!=345)return false;
        u32 contour=0,start=0;for(u32 i=0;i<n;++i){auto expected_point=original[i];if(i==start){expected_point.reserved[0]=expected[contour];start=ends[contour++]+1;}
            if(!bytes_equal(&expected_point,&marked[i+2],sizeof(SfntOutlinePoint)))return false;}
    }return row==orientation_fixture+sizeof(orientation_fixture);
}
bool invalid() noexcept {
    SfntOutlinePoint points[4]{{0,0},{0,10},{10,10},{10,0}};u32 end=3;u8 out=7;u32 used=0;
    if(tt_contour_directions({}, {}, {},0,&used)!=SfntStatus::ok||used)return false;
    if(tt_contour_directions({nullptr,4},{&end,1},{&out,1},100)!=SfntStatus::invalid_input)return false;
    if(tt_contour_directions({points,4},{&end,1},{},100)!=SfntStatus::output_too_small)return false;
    end=4;if(tt_contour_directions({points,4},{&end,1},{&out,1},100)!=SfntStatus::invalid_input)return false;
    end=2;if(tt_contour_directions({points,4},{&end,1},{&out,1},100)!=SfntStatus::invalid_input)return false;end=3;
    const u32 bad[]{0x7f800000,0xff800000,0x7fc00000,0x3f000000,0x47000000};
    for(u32 bits:bad){copy_bytes(&points[0].x,&bits,4);if(tt_contour_directions({points,4},{&end,1},{&out,1},100)!=SfntStatus::unsupported)return false;}
    return true;
}
int run() noexcept {return corpus()?(invalid()?0:2):1;}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_tt_orientation_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
