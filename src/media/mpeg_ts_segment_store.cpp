#include "maicord/media/mpeg_ts_segment_store.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/mpeg_ts_codec.hpp"
#include "maicord/media/h264_parameters.hpp"
#include "maicord/media/h264_stream.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {
namespace {
using E=Mp4DemuxError;
constexpr u32 samples_limit=1024, headroom=512;
void advance(u64& value) noexcept { if(!++value) ++value; }
E converted(MpegTsError e) noexcept {
    return e==MpegTsError::none?E::none:e==MpegTsError::capacity||e==MpegTsError::unsupported?E::unsupported:E::malformed;
}
bool scale(u64 value,u32 from,u32 to,u64* result) noexcept {
    const u64 whole=value/from, part=(value%from)*to/from;
    constexpr u64 limit=0x7fffffffffffffffull;
    if(whole>limit/to || whole*to>limit-part) return false;
    *result=whole*to+part; return true;
}
bool close(u64 a,u64 b,u32 tolerance=1) noexcept {return a>b?a-b<=tolerance:b-a<=tolerance;}
struct VideoUnit { u64 pts, dts; u32 offset, bytes; };
// PES timestamps describe the first access unit starting in that packet; its
// leading bytes may finish the previous picture. Assemble the byte stream in
// the candidate slot, then use AUD boundaries before converting Annex B.
E video_units(ConstSlice<u8> body,u16 pid,ReservedBytes& storage,u32 table,
              VideoUnit* units,u32* count,u32* base) noexcept {
    MpegTsReader reader{body,0,~u32{0},pid}; MpegTsPes pes{};
    u32 packets=0,bytes=0;
    for(;;) {
        const auto error=mpeg_ts_next(&reader,{storage.base+table+headroom+bytes,
            storage.committed-table-headroom-bytes},&pes);
        if(error==MpegTsError::end_of_stream) break;
        if(error!=MpegTsError::none) return converted(error);
        if(packets==samples_limit) return E::unsupported;
        units[packets++]={pes.pts,pes.dts,bytes,pes.payload.count};
        copy_bytes(storage.base+table+headroom+bytes,pes.payload.data,pes.payload.count);
        bytes+=pes.payload.count;
    }
    if(!packets) return E::malformed;
    H264AnnexBCursor cursor{{storage.base+table+headroom,bytes},0};
    u32 packet=0,pictures=0,output_bytes=0,growth=0;
    auto current=units[0]; bool seen=false;
    for(;;) {
        const u32 begin=cursor.offset;
        const auto nal=h264_annexb_next(&cursor);
        if(nal.error==H264Error::end) break;
        if(nal.error!=H264Error::none) return E::malformed;
        if(output_bytes>storage.reserved-4 || nal.bytes.count>storage.reserved-output_bytes-4) return E::unsupported;
        output_bytes+=4+nal.bytes.count;
        if(output_bytes>cursor.offset) growth=maximum(growth,output_bytes-cursor.offset);
        if(nal.type!=9) continue;
        while(begin>=current.offset+current.bytes) {
            if(++packet==packets) return E::malformed;
            current=units[packet]; seen=false;
        }
        u32 boundary=begin;
        H264AnnexBCursor prefix{{cursor.bytes.data+current.offset,begin-current.offset},0};
        bool prelude=true;
        for(;;) {
            const auto preceding=h264_annexb_next(&prefix);
            if(preceding.error==H264Error::end) break;
            if(preceding.error!=H264Error::none || (preceding.type>=1 && preceding.type<=5)) {prelude=false;break;}
        }
        if(prelude) boundary=current.offset;
        // No timestamp is invented for multiple access units in one PES.
        if(seen || (!pictures && boundary)) return E::unsupported;
        seen=true;
        if(pictures) units[pictures-1].bytes=boundary-units[pictures-1].offset;
        units[pictures++]={current.pts,current.dts,boundary,0};
    }
    if(pictures) units[pictures-1].bytes=bytes-units[pictures-1].offset;
    else pictures=packets; // Aligned PES without AUDs retain their boundaries.
    const u32 gap=headroom+growth;
    if(gap>storage.reserved-table || bytes>storage.reserved-table-gap) return E::unsupported;
    if(!reserved_bytes_commit(&storage,table+gap+bytes)) return E::out_of_memory;
    if(growth) copy_bytes(storage.base+table+gap,storage.base+table+headroom,bytes);
    *count=pictures; *base=table+gap; return E::none;
}
E configuration(Mp4SegmentStore& s,const MpegTsProgram& program,const MpegTsPes& pes) noexcept {
    u8 audio[2]{}; MpegTsAvcConfiguration avc{}; ConstSlice<u8> config{};
    auto track=s.track; track.defaults.track_id=track.audio?program.audio_pid:program.video_pid;
    if(track.audio) {
        MpegTsAacFrame frame{}; const auto error=mpeg_ts_aac_frame(pes.payload,&frame);
        if(error!=MpegTsError::none) return converted(error);
        copy_bytes(audio,frame.configuration,2); config={audio,2};
        track.channels=frame.channels; track.sample_rate=track.timescale=frame.sample_rate;
        track.frames_per_packet=1024; track.defaults.duration=1024;
    } else {
        const auto error=mpeg_ts_avc_configuration(pes.payload,&avc);
        if(error!=MpegTsError::none) return converted(error);
        config={avc.bytes,avc.count}; track.timescale=90000; track.nal_length_bytes=4;
        track.width=avc.width; track.height=avc.height; track.sar_width=avc.sar_width; track.sar_height=avc.sar_height;
    }
    if(s.initialization.base) return track.defaults.track_id==s.track.defaults.track_id &&
        config.count==s.initialization_bytes && bytes_equal(config.data,s.initialization.base,config.count)?E::none:E::unsupported;
    if(!reserved_bytes_initialize(&s.initialization,config.count) || !reserved_bytes_commit(&s.initialization,config.count)) return E::out_of_memory;
    copy_bytes(s.initialization.base,config.data,config.count); s.initialization_bytes=config.count;
    track.configuration={s.initialization.base,config.count}; track.movie_timescale=track.timescale;
    s.track=track; return E::none;
}
E fill(Mp4SegmentStore& s,u32 number,ConstSlice<u8> body,u64 duration_us,u64 initial_reference=0) noexcept {
    if(!duration_us || duration_us>30000000 || !body.data || !body.count ||
        !byte_range_valid(body) || mp4_segment_store_overlaps(s,body.data,body.count)) return E::invalid_input;
    const u32 capacity=s.track.audio?1024*1024:8*1024*1024, stride=s.track.audio?4:16, table=samples_limit*stride;
    if(body.count>capacity-table-headroom) return E::unsupported;
    const bool packed=s.track.audio && body.count>=3 && bytes_equal(body.data,"ID3",3);
    PackedAudioSegment packed_audio{}; MpegTsProgram program{};
    auto parsed=packed?packed_aac_segment(body,&packed_audio):mpeg_ts_program(body,&program);
    if(packed) program.audio_pid=1;
    if(parsed!=MpegTsError::none) return converted(parsed);
    const u16 pid=s.track.audio?program.audio_pid:program.video_pid;
    if(pid==kMpegTsNoPid) return s.track.audio?E::no_audio:E::no_video;
    const u32 chosen=s.active==0?1:0; auto& slot=s.slots[chosen];
    if((!slot.storage.base&&!reserved_bytes_initialize(&slot.storage,capacity)) ||
        !reserved_bytes_commit(&slot.storage,body.count+table+headroom)) return E::out_of_memory;
    // The active slot stays pinned. Any proposal borrowing this candidate must
    // become stale before its bytes can be compacted, including on failure.
    slot.bytes=0; reset_value(&slot.media); advance(s.generation);
    u8* data=slot.storage.base; u32 used=table,count=0; u64 first=0,last=0,clock=0;
    VideoUnit units[samples_limit]; u32 unit_count=0,unit_base=0,unit_index=0;
    if(!s.track.audio) {
        const auto error=video_units(body,pid,slot.storage,table,units,&unit_count,&unit_base);
        if(error!=E::none) return error;
    }
    MpegTsReader reader{body,0,~u32{0},pid}; MpegTsPes pes{};
    for(;;) {
        if(s.track.audio?reader.offset==body.count:unit_index==unit_count) {parsed=MpegTsError::end_of_stream;break;}
        const u32 available=slot.storage.committed-used;
        if(!s.track.audio) {
            const auto& unit=units[unit_index++];
            pes={{data+unit_base+unit.offset,unit.bytes},unit.pts,unit.dts};parsed=MpegTsError::none;
        } else if(packed) {pes={packed_audio.payload,packed_audio.timestamp,packed_audio.timestamp};reader.offset=body.count;parsed=MpegTsError::none;}
        else parsed=available>headroom?mpeg_ts_next(&reader,{data+used+headroom,available-headroom},&pes):MpegTsError::capacity;
        if(parsed==MpegTsError::capacity) {
            if(slot.storage.committed==capacity) return E::unsupported;
            // Short Annex B start codes can expand beyond the original file.
            // A failed PES read preserves its cursor. The remaining transport
            // bytes bound that PES, so one page-rounded retry is sufficient.
            const u32 required=minimum(capacity,maximum(slot.storage.committed+1,used+headroom+body.count-reader.offset));
            if(!reserved_bytes_commit(&slot.storage,required)) return E::out_of_memory;
            continue;
        }
        if(parsed!=MpegTsError::none) break;
        if(!count) {
            const auto error=configuration(s,program,pes); if(error!=E::none) return error;
            u64 reference=initial_reference?initial_reference:pes.dts+(u64{1}<<33);
            if(s.available_end && !scale(s.available_end,s.track.timescale,90000,&reference)) return E::unsupported;
            if(!mpeg_ts_unwrap(pes.dts,reference,&clock)) return E::malformed;
            if(!scale(clock,90000,s.track.timescale,&first)) return E::unsupported;
        }
        u64 dts{},pts{};
        if(!mpeg_ts_unwrap(pes.dts,clock,&dts) || !mpeg_ts_unwrap(pes.pts,dts,&pts) ||
            dts>0x7fffffffffffffffull || pts>0x7fffffffffffffffull) return E::malformed;
        if(s.track.audio) {
            const u64 expected=clock+u64{count}*1024*90000/s.track.sample_rate;
            if(pes.pts!=pes.dts || !close(dts,expected)) return E::malformed;
            for(u32 at=0;at<pes.payload.count;) {
                if(count==samples_limit) return E::unsupported;
                MpegTsAacFrame frame{}; const auto error=mpeg_ts_aac_frame({pes.payload.data+at,pes.payload.count-at},&frame);
                if(error!=MpegTsError::none) return converted(error);
                if(!bytes_equal(frame.configuration,s.initialization.base,2)) return E::unsupported;
                byte_store_be32(data+count*stride,frame.payload.count);
                copy_bytes(data+used,frame.payload.data,frame.payload.count); used+=frame.payload.count;
                at+=frame.bytes; ++count;
            }
        } else {
            if(count==samples_limit || (count && (dts<=last || dts-last>~u32{0}))) return E::malformed;
            const i64 composition=static_cast<i64>(pts)-static_cast<i64>(dts);
            if(composition<(-2147483647ll-1) || composition>2147483647) return E::unsupported;
            u32 bytes{}; const auto error=mpeg_ts_avc_sample(pes.payload,{data+used,slot.storage.committed-used},&bytes);
            if(error!=MpegTsError::none) return converted(error);
            H264NalCursor nals{{data+used,bytes},0,4}; bool found=false,key=false;
            for(;;) { const auto nal=h264_next_nal(&nals); if(nal.error!=H264Error::none) break;
                if(nal.type==1||nal.type==5) {found=true;key=nal.type==5;break;} }
            if(!found || (!count&&!key)) return E::unsupported;
            if(count) byte_store_be32(data+(count-1)*stride,static_cast<u32>(dts-last));
            byte_store_be32(data+count*stride+4,bytes); byte_store_be32(data+count*stride+8,key?0:0x10000);
            byte_store_be32(data+count*stride+12,static_cast<u32>(composition));
            used+=bytes; last=dts; ++count;
        }
    }
    if(parsed!=MpegTsError::end_of_stream || !count) return parsed==MpegTsError::end_of_stream?E::malformed:converted(parsed);
    u64 duration{};
    if(s.track.audio) {
        duration=u64{count}*1024;
        // The preceding file may have occupied the candidate slot we just
        // replaced. Its accepted endpoint survives in the store's clock.
        if(number && number==s.received_count) {
            u64 mapped{};
            if(!scale(s.available_end,s.track.timescale,90000,&mapped) ||
                !close(mapped,clock,(90000+s.track.sample_rate-1)/s.track.sample_rate)) return E::malformed;
            first=s.available_end;
        }
        // Carry the exact sample grid across adjacent files. PES clocks are
        // quantized to 90kHz. Converting a sample tick back to that clock can
        // lose up to one native sample, including at the 33-bit wrap.
        for(const auto& prior:s.slots) if(prior.bytes) {
            u64 candidate=first;
            if(prior.number+1==number) candidate=prior.media.range.time+prior.media.range.duration;
            else if(number+1==prior.number) {if(prior.media.range.time<duration) return E::malformed;candidate=prior.media.range.time-duration;}
            else continue;
            u64 mapped{}; if(!scale(candidate,s.track.timescale,90000,&mapped)||
                !close(mapped,clock,(90000+s.track.sample_rate-1)/s.track.sample_rate)) return E::malformed;
            first=candidate;
        }
    } else {
        duration=(duration_us*90000+500000)/1000000;
        if(!duration || duration>0x7fffffffffffffffull-first || first+duration<=last || first+duration-last>~u32{0}) return E::malformed;
        byte_store_be32(data+(count-1)*stride,static_cast<u32>(first+duration-last));
    }
    if(duration>0x7fffffffffffffffull-first) return E::malformed;
    Mp4MediaSegment media{};
    media.range={0,first,duration,used,number,0,1,true}; media.samples=count; media.next_fragment=used;
    auto& fragment=media.fragment; fragment.metadata={data,table}; fragment.defaults=s.track.defaults;
    fragment.first_time=first; fragment.duration=duration; fragment.count=count; fragment.run_count=1;
    fragment.runs[0]={table,0,count,s.track.audio?0x200u:0x01000f00u,0}; mp4_fragment_reset(&fragment);
    auto cursor=fragment; Mp4FragmentSample sample{};
    while(mp4_fragment_next(&cursor,&sample)==E::none) {
        if(sample.offset>used || sample.bytes>used-sample.offset || sample.presentation_time<0 ||
            sample.presentation_time>0x7fffffffffffffffll-sample.duration) return E::malformed;
        media.presentation_end=maximum(media.presentation_end,static_cast<u64>(sample.presentation_time)+sample.duration);
    }
    if(cursor.index!=count || cursor.decode_time!=first+duration) return E::malformed;
    for(const auto& prior:s.slots) if(prior.bytes && prior.number!=number) {
        const auto& a=prior.number<number?prior.media.range:media.range;
        const auto& b=prior.number<number?media.range:prior.media.range;
        if(a.time+a.duration>b.time || ((prior.number+1==number||number+1==prior.number)&&a.time+a.duration!=b.time)) return E::malformed;
    }
    if(!s.received_count) s.track.media_start=static_cast<i64>(first);
    slot.media=media; slot.number=number; slot.bytes=used;
    s.available_end=maximum(s.available_end,first+duration); s.available_presentation_end=maximum(s.available_presentation_end,media.presentation_end);
    s.received_count=maximum(s.received_count,number+1); s.pending=false; return E::none;
}
}
Mp4DemuxError mpeg_ts_segment_store_initialize(Mp4SegmentStore* s,ConstSlice<u8> bytes,bool audio,u32 count,u64 duration,u64 reference) noexcept {
    if(!s||!byte_range_valid(s,sizeof(*s))||s->initialization.base||s->count||s->slots[0].storage.base||s->slots[1].storage.base||
        !count||count>8192||mp4_segment_store_overlaps(*s,bytes.data,bytes.count)) return E::invalid_input;
    Mp4SegmentStore next{}; next.track.audio=audio; next.count=count; next.nominal_duration=duration; next.nominal_timescale=1000000;
    next.live=next.transport=true; next.generation=s->generation; next.request_serial=s->request_serial;
    advance(next.generation); advance(next.request_serial);
    const auto error=fill(next,0,bytes,duration,reference);
    if(error!=E::none) {mp4_segment_store_release(&next);return error;}
    *s=next; return E::none;
}
Mp4DemuxError mpeg_ts_segment_store_accept(Mp4SegmentStore* s,u32 number,u64 serial,ConstSlice<u8> bytes,u64 duration) noexcept {
    if(!s||!s->transport||!s->initialization.base||!s->pending||s->demand!=number||serial!=s->request_serial||number>=s->count) return E::invalid_input;
    return fill(*s,number,bytes,duration);
}
} // namespace maicord
