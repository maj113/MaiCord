#include "maicord/text/tt_orientation.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {namespace {
#include "generated/orientation_fixture.inc"
// Points sit at marked[2..n+2) between sentinels; ends are absolute indices there.
SfntStatus classify(SfntOutlinePoint (&marked)[132],const SfntOutlinePoint* original,u32 n,const u32* ends,u32 nc,u32 budget,u32* used) noexcept {
    marked[0]={123};marked[1]={234};marked[n+2]={345};
    copy_bytes(marked+2,original,n*sizeof(SfntOutlinePoint));
    return tt_mark_contour_directions({marked,n+2},{ends,nc},2,budget,used);
}
bool corpus() noexcept {
    const u8* row=orientation_fixture+4;const u32 cases=byte_load_le32(orientation_fixture);
    for(u32 c=0;c<cases;++c){const u32 n=byte_load_le32(row),nc=byte_load_le32(row+4);row+=8;
        if(n>128||nc>16)return false;u32 ends[16]{};u8 expected[16]{};SfntOutlinePoint original[128]{},marked[132]{};
        for(u32 i=0;i<nc;++i){ends[i]=byte_load_le32(row)+2;expected[i]=static_cast<u8>(byte_load_le32(row+4));row+=8;}
        for(u32 i=0;i<n;++i){original[i]={static_cast<float>(static_cast<i16>(byte_load_le16(row))),static_cast<float>(static_cast<i16>(byte_load_le16(row+2))),static_cast<u8>(byte_load_le32(row+4)),{7,8,9}};row+=8;}
        u32 full=0;
        if(classify(marked,original,n,ends,nc,1000000,&full)!=SfntStatus::ok||!full)return false;
        const u32 budgets[]{0,1,full/2,full-1,full};
        for(u32 budget:budgets){u32 used=0;const auto status=classify(marked,original,n,ends,nc,budget,&used);
            if(status!=(budget<full?SfntStatus::resource_limit:SfntStatus::ok)||used>budget)return false;
        }
        // The last run was the exact budget: each contour's first point carries its direction.
        if(marked[0].x!=123||marked[1].x!=234||marked[n+2].x!=345)return false;
        u32 contour=0,start=0;for(u32 i=0;i<n;++i){auto expected_point=original[i];if(i==start){expected_point.reserved[0]=expected[contour];start=ends[contour++]-2+1;}
            if(!bytes_equal(&expected_point,&marked[i+2],sizeof(SfntOutlinePoint)))return false;}
    }return row==orientation_fixture+sizeof(orientation_fixture);
}
bool invalid() noexcept {
    SfntOutlinePoint points[4]{{0,0},{0,10},{10,10},{10,0}};u32 end=3;u32 used=0;
    if(tt_mark_contour_directions({},{},0,0,&used)!=SfntStatus::ok||used)return false;
    const u32 bad[]{0x7f800000,0xff800000,0x7fc00000,0x3f000000,0x47000000};
    for(u32 bits:bad){copy_bytes(&points[0].x,&bits,4);if(tt_mark_contour_directions({points,4},{&end,1},0,100)!=SfntStatus::unsupported)return false;}
    return true;
}
int run() noexcept {return corpus()?(invalid()?0:2):1;}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_tt_orientation_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
