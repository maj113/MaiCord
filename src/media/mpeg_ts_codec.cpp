#include "maicord/media/mpeg_ts_codec.hpp"
#include "maicord/media/aac_configuration.hpp"
#include "maicord/media/h264_stream.hpp"
#include "maicord/core/byte_order.hpp"

namespace maicord {
namespace {
using E = MpegTsError;
bool separate(ConstSlice<u8> input, const void* output, u32 size) noexcept {
    return input.data && input.count && input.count <= kMpegTsMaximumBytes && byte_range_valid(input) &&
        output && byte_range_valid(output,size) && !byte_ranges_overlap(input.data,input.count,output,size);
}
E parsed(H264Error error) noexcept { return error == H264Error::unsupported ? E::unsupported : E::malformed; }
}
MpegTsError mpeg_ts_aac_frame(ConstSlice<u8> input, MpegTsAacFrame* output) noexcept {
    if (!separate(input,output,sizeof(*output))) return E::invalid_input;
    const auto* p = input.data;
    if (input.count < 7 || p[0] != 255 || (p[1] & 246) != 240) return E::malformed;
    if (!(p[1] & 1) || (p[2] >> 6) != 1 || (p[6] & 3)) return E::unsupported;
    MpegTsAacFrame value{};
    value.bytes = ((p[3] & 3) << 11) | (p[4] << 3) | (p[5] >> 5);
    if (value.bytes <= 7 || value.bytes > input.count) return E::malformed;
    const u8 frequency = (p[2] >> 2) & 15;
    value.channels = static_cast<u8>(((p[2] & 1) << 2) | (p[3] >> 6));
    value.configuration[0] = static_cast<u8>(16 | (frequency >> 1));
    value.configuration[1] = static_cast<u8>((frequency << 7) | (value.channels << 3));
    AacConfiguration decoded{};
    if (frequency >= 13 || !aac_configuration_decode({value.configuration,2},&decoded)) return E::unsupported;
    value.sample_rate = decoded.sample_rate;
    value.payload = {p+7,value.bytes-7}; *output = value; return E::none;
}
MpegTsError packed_aac_segment(ConstSlice<u8> input, PackedAudioSegment* output) noexcept {
    if (!separate(input,output,sizeof(*output))) return E::invalid_input;
    if (input.count>1024*1024) return E::capacity;
    const auto* p=input.data;
    if (input.count<10 || !bytes_equal(p,"ID3",3)) return E::malformed;
    if ((p[3]!=3 && p[3]!=4) || p[4] || p[5]) return E::unsupported;
    const auto synchsafe=[](const u8* bytes,u32* value) noexcept {
        if ((bytes[0]|bytes[1]|bytes[2]|bytes[3])&128) return false;
        *value=(u32{bytes[0]}<<21)|(u32{bytes[1]}<<14)|(u32{bytes[2]}<<7)|bytes[3];return true;
    };
    u32 size{};
    if (!synchsafe(p+6,&size) || size>input.count-10) return E::malformed;
    if (size>65536) return E::capacity;
    const u32 end=10+size; bool found=false; u64 timestamp=0;
    constexpr char owner[]="com.apple.streaming.transportStreamTimestamp";
    for (u32 at=10,frames=0;at<end;) {
        if (!p[at]) {for(;at<end;++at) if(p[at]) return E::malformed;break;}
        if (end-at<10) return E::malformed;
        if (++frames>64) return E::capacity;
        for(u32 i=0;i<4;++i) if(!((p[at+i]>='A'&&p[at+i]<='Z')||(p[at+i]>='0'&&p[at+i]<='9'))) return E::malformed;
        u32 bytes=byte_load_be32(p+at+4);
        if (p[3]==4 && !synchsafe(p+at+4,&bytes)) return E::malformed;
        if (!bytes || bytes>end-at-10) return E::malformed;
        if (p[at+8] || p[at+9]) return E::unsupported;
        const auto* payload=p+at+10;
        if (bytes_equal(p+at,"PRIV",4) && bytes>=sizeof(owner) && bytes_equal(payload,owner,sizeof(owner))) {
            if (found || bytes!=sizeof(owner)+8) return E::malformed;
            timestamp=byte_load_be64(payload+sizeof(owner));
            if (timestamp>=(u64{1}<<33)) return E::malformed;
            found=true;
        }
        at+=10+bytes;
    }
    if (!found || end==input.count) return E::malformed;
    const ConstSlice<u8> payload{p+end,input.count-end}; MpegTsAacFrame first{};
    const auto error=mpeg_ts_aac_frame(payload,&first);
    if(error!=E::none) return error;
    *output={payload,timestamp};return E::none;
}
MpegTsError mpeg_ts_avc_configuration(ConstSlice<u8> input, MpegTsAvcConfiguration* output) noexcept {
    if (!separate(input,output,sizeof(*output))) return E::invalid_input;
    H264AnnexBCursor cursor{input,0}; ConstSlice<u8> sps{},pps{};
    for (u32 count = 0;; ++count) {
        const auto nal = h264_annexb_next(&cursor);
        if (nal.error == H264Error::end) break;
        if (nal.error != H264Error::none) return parsed(nal.error);
        if (count == 512) return E::unsupported;
        if (nal.type != 7 && nal.type != 8) continue;
        auto& previous = nal.type == 7 ? sps : pps;
        if (previous.count && (previous.count != nal.bytes.count || !bytes_equal(previous.data,nal.bytes.data,previous.count)))
            return E::unsupported;
        previous = nal.bytes;
    }
    if (!sps.count || !pps.count) return E::malformed;
    if (sps.count > 65535 || pps.count > 65535 || u64{sps.count}+pps.count+11 > sizeof(output->bytes)) return E::capacity;
    H264Sps sequence{}; H264Pps picture{};
    auto error = h264_parse_sps(sps,&sequence);
    if (error != H264Error::none) return parsed(error);
    error = h264_parse_pps(pps,sequence,&picture);
    if (error != H264Error::none) return parsed(error);
    MpegTsAvcConfiguration value{};
    value.width=sequence.width; value.height=sequence.height; value.sar_width=sequence.sar_width; value.sar_height=sequence.sar_height;
    value.bytes[0]=1; value.bytes[1]=sequence.profile; value.bytes[2]=sequence.constraints; value.bytes[3]=sequence.level;
    value.bytes[4]=255; value.bytes[5]=225;
    value.bytes[6]=static_cast<u8>(sps.count>>8); value.bytes[7]=static_cast<u8>(sps.count);
    copy_bytes(value.bytes+8,sps.data,sps.count);
    u32 at=8+sps.count; value.bytes[at++]=1;
    value.bytes[at++]=static_cast<u8>(pps.count>>8); value.bytes[at++]=static_cast<u8>(pps.count);
    copy_bytes(value.bytes+at,pps.data,pps.count);
    value.count=at+pps.count; *output=value; return E::none;
}
MpegTsError mpeg_ts_avc_sample(ConstSlice<u8> input, Slice<u8> scratch, u32* output) noexcept {
    if (!separate(input,output,sizeof(*output)) || !scratch.data || !scratch.count ||
        !byte_range_valid(scratch.data,scratch.count) || byte_ranges_overlap(scratch.data,scratch.count,output,sizeof(*output))) return E::invalid_input;
    if (byte_ranges_overlap(input.data,input.count,scratch.data,scratch.count) &&
        (reinterpret_cast<usize>(input.data)<reinterpret_cast<usize>(scratch.data) ||
         reinterpret_cast<usize>(input.data)-reinterpret_cast<usize>(scratch.data)<512)) return E::invalid_input;
    H264AnnexBCursor cursor{input,0}; u32 used=0; bool picture=false;
    for (u32 count=0;;++count) {
        const auto nal=h264_annexb_next(&cursor);
        if (nal.error==H264Error::end) break;
        if (nal.error!=H264Error::none) return parsed(nal.error);
        if (count==512 || (nal.type>=2 && nal.type<=4) || (picture && nal.type==9)) return E::unsupported;
        if (nal.type==1 || nal.type==5) picture=true;
        if (scratch.count-used<4 || nal.bytes.count>scratch.count-used-4) return E::capacity;
        byte_store_be32(scratch.data+used,nal.bytes.count); used+=4;
        copy_bytes(scratch.data+used,nal.bytes.data,nal.bytes.count); used+=nal.bytes.count;
    }
    if (!picture) return E::malformed;
    *output=used; return E::none;
}
} // namespace maicord
