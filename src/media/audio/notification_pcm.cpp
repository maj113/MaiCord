#include "maicord/media/audio/notification_pcm.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {namespace {
constexpr u32 kNotificationPcmBlockFrames=256;
u16 restore(u16 encoded,u8 order,u16 previous,u16 before) noexcept {
    const i32 residual=(encoded&1)?-static_cast<i32>(encoded/2)-1:static_cast<i32>(encoded/2);
    const u32 prediction=order==0?0:order==1?previous:2u*previous-before;
    return static_cast<u16>(static_cast<u32>(residual)+prediction);
}
}
NotificationPcmError notification_pcm_unpack(ConstSlice<u8> packed,NotificationPcmSpec spec,
    Slice<u8> pcm,DeflateWorkspace* workspace) noexcept {
    const u32 bytes=spec.frames*4;
    // Every range is nonempty here, so a conflict also covers a null or wrapping range.
    if(!spec.frames || spec.frames>kNotificationPcmMaximumFrames || spec.left_order>2 || spec.side_order>2 ||
        pcm.count!=bytes || !packed.count || packed.count>2*1024*1024 ||
        byte_ranges_conflict(packed.data,packed.count,pcm.data,pcm.count) ||
        byte_ranges_conflict(packed.data,packed.count,workspace,sizeof(*workspace)) ||
        byte_ranges_conflict(pcm.data,pcm.count,workspace,sizeof(*workspace)))
        return NotificationPcmError::invalid_input;
    const DeflateInputSpan input{packed.data,packed.count};
    const auto inflated=zlib_inflate({&input,1},pcm,bytes,workspace);
    if(!inflated.ok()) return NotificationPcmError::inflate;
    u8 block[kNotificationPcmBlockFrames*4];
    u16 left=0,before_left=0,side=0,before_side=0;
    for(u32 first=0;first<spec.frames;first+=kNotificationPcmBlockFrames) {
        const u32 n=minimum(kNotificationPcmBlockFrames,spec.frames-first);
        copy_bytes(block,pcm.data+first*4,n*4);
        for(u32 i=0;i<n;++i) {
            const u16 left_residual=static_cast<u16>(u16(block[i])|(u16(block[n+i])<<8));
            const u16 side_residual=static_cast<u16>(u16(block[2*n+i])|(u16(block[3*n+i])<<8));
            const u16 next_left=restore(left_residual,spec.left_order,left,before_left);
            const u16 next_side=restore(side_residual,spec.side_order,side,before_side);
            before_left=left;left=next_left;before_side=side;side=next_side;
            const u16 right=static_cast<u16>(u32(left)+side);
            byte_store_le32(pcm.data+(first+i)*4,u32(left)|(u32(right)<<16));
        }
    }
    return NotificationPcmError::none;
}
}
