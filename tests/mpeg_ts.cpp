#include "maicord/media/mpeg_ts.hpp"
#include "maicord/media/mpeg_ts_codec.hpp"
#include "maicord/media/mpeg_ts_segment_store.hpp"
#include "maicord/media/mp4_fragment_audio.hpp"
#include "maicord/app/account_video_session.hpp"
#include "maicord/app/account_media.hpp"
#include "maicord/app/segmented_video_fetch.hpp"
#include "maicord/media/h264_parameters.hpp"
#include "maicord/media/h264_slice.hpp"
#include "maicord/platform/aac_decoder.hpp"
#include "maicord/sys/com.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "test_output.hpp"
#include "h264_stream_fixture.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool ok, const char* name) noexcept { test_check(&failures, ok, name); }
using E = MpegTsError;
struct Bytes {
    u8 data[4096]{}; u32 size{};
    void byte(u8 value) noexcept { data[size++] = value; }
    void add(ConstSlice<u8> b) noexcept { copy_bytes(data + size, b.data, b.count); size += b.count; }
    ConstSlice<u8> view() const noexcept { return {data, size}; }
};
void checksum(Bytes& b) noexcept {
    const u32 length = b.size + 4 - 3;
    b.data[1] = static_cast<u8>(176 | (length >> 8)); b.data[2] = static_cast<u8>(length);
    u32 value = 0xffffffff;
    for (u32 i = 0; i < b.size; ++i) {
        value ^= u32{b.data[i]} << 24;
        for (u32 j = 0; j < 8; ++j) value = (value << 1) ^ ((value & 0x80000000) ? 0x04c11db7u : 0);
    }
    for (i32 shift = 24; shift >= 0; shift -= 8) b.byte(static_cast<u8>(value >> shift));
}
Bytes pat(bool two = false) noexcept {
    Bytes b{}; const u8 h[]{0,176,0,0,1,193,0,0,0,1,240,0}; b.add({h,sizeof(h)});
    if (two) { const u8 p[]{0,2,240,1}; b.add({p,4}); }
    checksum(b); return b;
}
Bytes pmt(bool encrypted = false) noexcept {
    Bytes b{}; const u8 h[]{2,176,0,0,1,193,0,0,225,1,240,0,27,225,1,240,0,15,225,0,240,0}; b.add({h,sizeof(h)});
    if (encrypted) { b.data[21] = 2; b.byte(9); b.byte(0); }
    checksum(b); return b;
}
void packet(Bytes& b, u16 pid, u8 cc, bool start, ConstSlice<u8> payload, u8 flags = 0) noexcept {
    const u32 at = b.size; for (u32 i = 0; i < 188; ++i) b.byte(255);
    auto* p = b.data + at; p[0] = 71; p[1] = static_cast<u8>((pid >> 8) | (start ? 64 : 0)); p[2] = static_cast<u8>(pid);
    p[3] = static_cast<u8>(cc | (!payload.count ? 32 : payload.count == 184 ? 16 : 48));
    const u32 offset = 188 - payload.count;
    if (offset > 4) { p[4] = static_cast<u8>(offset - 5); if (offset > 5) p[5] = flags; }
    if (flags & 16) { for (u32 i = 6; i < 12; ++i) p[i] = 0; p[10] = 126; }
    copy_bytes(p + offset, payload.data, payload.count);
}
void psi(Bytes& b, u16 pid, const Bytes& table, bool split = false) noexcept {
    u8 payload[184]{}; payload[0] = 0;
    const u32 first = split ? 7 : table.size;
    copy_bytes(payload + 1, table.data, first); packet(b, pid, 0, true, {payload, first + 1});
    if (split) packet(b, pid, 1, false, {table.data + first, table.size - first});
}
void stamp(Bytes& b, u64 value, u8 prefix) noexcept {
    b.byte(static_cast<u8>(prefix | ((value >> 29) & 14) | 1)); b.byte(static_cast<u8>(value >> 22));
    b.byte(static_cast<u8>((value >> 14) | 1)); b.byte(static_cast<u8>(value >> 7)); b.byte(static_cast<u8>((value << 1) | 1));
}
Bytes pes(u64 pts, u64 dts, u32 payload = 250, bool audio = false) noexcept {
    Bytes b{}; const u8 h[]{0,0,1,224,0,0,128,192,10}; b.add({h,sizeof(h)});
    if (audio) b.data[3] = 192;
    stamp(b, pts, 48); stamp(b, dts, 16);
    for (u32 i = 0; i < payload; ++i) b.byte(static_cast<u8>(i));
    if (audio) { b.data[4] = static_cast<u8>((b.size - 6) >> 8); b.data[5] = static_cast<u8>(b.size - 6); }
    return b;
}
Bytes fixture(bool split = false) noexcept {
    Bytes b{}; psi(b,0,pat(),split); psi(b,4096,pmt(),split);
    const auto a = pes(93000,90000), z = pes(96000,93000,12);
    packet(b,257,15,true,{a.data,100},80);
    packet(b,257,0,false,{a.data+100,a.size-100});
    packet(b,257,1,true,z.view()); return b;
}
void run() noexcept {
    check(sizeof(MpegTsReader) <= 32 && sizeof(MpegTsProgram) <= 12, "transport cursor has fixed small storage");
    auto bytes = fixture(); MpegTsProgram program{};
    check(mpeg_ts_program(bytes.view(), &program) == E::none && program.number == 1 && program.pmt_pid == 4096 &&
        program.pcr_pid == 257 && program.video_pid == 257 && program.audio_pid == 256, "clear single-program AVC/AAC selection");
    const auto split = fixture(true);
    check(mpeg_ts_program(split.view(), &program) == E::none, "split PAT and PMT assemble with validated CRC and continuity");
    Bytes audio_first{}; audio_first.add({bytes.data,2*188}); const auto early = pes(90000,90000,10,true);
    packet(audio_first,256,0,true,early.view()); audio_first.add({bytes.data+2*188,3*188});
    check(mpeg_ts_program(audio_first.view(),&program) == E::none,
        "Twitch audio may precede the video PID's first PCR within a complete segment");
    MpegTsReader reader{bytes.view(),0,~u32{0},257}; MpegTsPes result{}; u8 storage[1024]{};
    check(mpeg_ts_next(&reader,{storage,sizeof(storage)},&result) == E::none && result.pts == 93000 && result.dts == 90000 &&
        result.random_access && result.payload.count == 250 && result.payload.data[249] == 249 && reader.offset == 4 * 188,
        "PES spans transport packets and preserves independent DTS/PTS plus exact payload bytes");
    check(mpeg_ts_next(&reader,{storage,sizeof(storage)},&result) == E::none && result.pts == 96000 && result.payload.count == 12,
        "following PES starts at the retained lookahead packet");
    const auto saved = reader; const auto output = result;
    check(mpeg_ts_next(&reader,{storage,sizeof(storage)},&result) == E::end_of_stream &&
        bytes_equal(&saved,&reader,sizeof(reader)) && bytes_equal(&output,&result,sizeof(result)), "EOF preserves cursor and result");
    const auto reject = [&](Bytes bad, E expected, const char* name) noexcept {
        const auto prior = program;
        check(mpeg_ts_program(bad.view(),&program) == expected && bytes_equal(&program,&prior,sizeof(program)), name);
    };
    auto bad = bytes; bad.data[0] = 0; reject(bad,E::malformed,"bad transport sync is rejected transactionally");
    bad = bytes; --bad.size; reject(bad,E::invalid_input,"partial transport packet cannot enter parser");
    bad = bytes; bad.data[1] |= 128; reject(bad,E::malformed,"transport error indicator is rejected");
    bad = bytes; bad.data[3] |= 128; reject(bad,E::unsupported,"scrambled transport is unsupported");
    bad = bytes; bad.data[4] = 184; reject(bad,E::malformed,"adaptation cannot exceed packet bounds");
    bad = bytes; bad.data[187] ^= 1; reject(bad,E::malformed,"PAT CRC corruption is rejected");
    bad = bytes; bad.data[2*188+5] |= 128; reject(bad,E::unsupported,"transport discontinuity requires caller resynchronization");
    Bytes multiple{}; psi(multiple,0,pat(true)); reject(multiple,E::unsupported,"multiple programs are unsupported");
    Bytes protected_stream{}; psi(protected_stream,0,pat()); psi(protected_stream,4096,pmt(true));
    reject(protected_stream,E::unsupported,"conditional-access descriptor is rejected");
    auto no_clock_table=pmt();no_clock_table.size-=4;no_clock_table.data[8]=no_clock_table.data[9]=255;checksum(no_clock_table);
    Bytes no_clock{};psi(no_clock,0,pat());psi(no_clock,4096,no_clock_table);
    const auto clocked_payload=pes(93000,90000,32);packet(no_clock,257,0,true,clocked_payload.view());
    MpegTsProgram no_clock_program{};
    check(mpeg_ts_program(no_clock.view(),&no_clock_program)==E::none && no_clock_program.pcr_pid==kMpegTsNoPid,
        "a track may explicitly omit PCR while preserving its PES timing");
    const u32 stuffing_at=no_clock.size;packet(no_clock,257,1,false,{});no_clock.data[stuffing_at+3]|=16;
    MpegTsReader padded{no_clock.view(),0,~u32{0},257};MpegTsPes padded_pes{};
    check(mpeg_ts_program(no_clock.view(),&no_clock_program)==E::none &&
        mpeg_ts_next(&padded,{storage,sizeof(storage)},&padded_pes)==E::none && padded_pes.payload.count==32 &&
        padded_pes.pts==93000 && padded_pes.dts==90000,
        "empty payload-marked trailing stuffing advances continuity without extending PES bytes");
    no_clock.data[stuffing_at+1]|=64;
    check(mpeg_ts_program(no_clock.view(),&no_clock_program)==E::malformed,
        "an empty stuffing packet cannot claim to start a PES");
    Bytes missing_clock{};psi(missing_clock,0,pat());psi(missing_clock,4096,pmt());packet(missing_clock,257,0,true,clocked_payload.view());
    reject(missing_clock,E::malformed,"a declared PCR still must be supplied");
    const auto reject_pes = [&](Bytes bad, u32 capacity, E expected, const char* name) noexcept {
        MpegTsReader r{bad.view(),0,~u32{0},257}; const auto old = r; MpegTsPes p{}; const auto before = p;
        check(mpeg_ts_next(&r,{storage,capacity},&p) == expected && bytes_equal(&r,&old,sizeof(r)) &&
            bytes_equal(&p,&before,sizeof(p)), name);
    };
    reject_pes(bytes,32,E::capacity,"insufficient output space preserves the PES cursor");
    bad = bytes; bad.data[3*188+3] = 50; reject_pes(bad,sizeof(storage),E::malformed,"missing continuation packet is rejected");
    bad = bytes; bad.data[2*188+88+9] &= 254; reject_pes(bad,sizeof(storage),E::malformed,"PTS marker bit is required");
    bad = bytes; bad.data[2*188+88+7] = 64; reject_pes(bad,sizeof(storage),E::malformed,"DTS without PTS is invalid");
    bad = bytes; bad.data[2*188+88+8] = 255; reject_pes(bad,sizeof(storage),E::malformed,"truncated PES optional header is rejected");
    Bytes duplicate{}; duplicate.add({bytes.data,3*188}); duplicate.add({bytes.data+2*188,188}); duplicate.add({bytes.data+3*188,2*188});
    MpegTsReader dup{duplicate.view(),0,~u32{0},257};
    check(mpeg_ts_next(&dup,{storage,sizeof(storage)},&result) == E::none && result.payload.count == 250,
        "one exact duplicate start packet does not duplicate elementary payload");
    duplicate.data[3*188+6] = 1; // Legal refreshed PCR inside a duplicate.
    dup = {duplicate.view(),0,~u32{0},257};
    check(mpeg_ts_next(&dup,{storage,sizeof(storage)},&result) == E::none && result.payload.count == 250,
        "duplicate transport packet may carry a refreshed PCR without changing its payload");
    Bytes triple{}; triple.add({duplicate.data,4*188}); triple.add({duplicate.data+3*188,188}); triple.add({duplicate.data+4*188,2*188});
    reject_pes(triple,sizeof(storage),E::malformed,"a third copy of a continuity counter is rejected");
    Bytes adaptation{}; adaptation.add({bytes.data,3*188}); packet(adaptation,257,15,false,{});
    adaptation.add({bytes.data+3*188,2*188}); MpegTsReader adapted{adaptation.view(),0,~u32{0},257};
    check(mpeg_ts_next(&adapted,{storage,sizeof(storage)},&result) == E::none && result.payload.count == 250,
        "adaptation-only packet keeps the previous payload continuity counter");
    adaptation.data[3*188+3] = 32;
    reject_pes(adaptation,sizeof(storage),E::malformed,"adaptation-only packet cannot advance continuity");
    Bytes split_pes{}; split_pes.add({bytes.data,2*188}); const auto short_header = pes(93000,90000,40);
    packet(split_pes,257,0,true,{short_header.data,7},80);
    packet(split_pes,257,1,false,{short_header.data+7,short_header.size-7});
    MpegTsReader header_reader{split_pes.view(),0,~u32{0},257};
    check(mpeg_ts_next(&header_reader,{storage,sizeof(storage)},&result) == E::none && result.pts == 93000 &&
        result.dts == 90000 && result.payload.count == 40, "PES timestamps may span transport packet boundaries");
    reader = {bytes.view(),0,~u32{0},257};
    check(mpeg_ts_next(&reader,{bytes.data,bytes.size},&result) == E::invalid_input && !reader.offset,
        "PES output cannot overwrite its immutable transport source");
    constexpr u64 wrap = u64{1} << 33; u64 timestamp = 123;
    check(mpeg_ts_unwrap(5,wrap-10,&timestamp) && timestamp == wrap+5 &&
        mpeg_ts_unwrap(wrap-5,wrap+10,&timestamp) && timestamp == wrap-5,
        "33-bit timestamp rollover maps to the nearest accepted epoch in either direction");
    timestamp = 123;
    check(!mpeg_ts_unwrap(wrap,0,&timestamp) && !mpeg_ts_unwrap(wrap/2,0,&timestamp) &&
        !mpeg_ts_unwrap(wrap-1,0,&timestamp) && !mpeg_ts_unwrap(1,~u64{0},&timestamp) && timestamp == 123,
        "invalid ambiguous negative and overflowing timestamp mappings preserve output");
}
void codecs() noexcept {
    const u8 encoded[]{255,241,76,128,1,95,252,11,22,33,255,241,76,128,1,31,252,44};
    MpegTsAacFrame frame{};
    check(mpeg_ts_aac_frame({encoded,sizeof(encoded)},&frame)==E::none && frame.bytes==10 &&
        frame.payload.count==3 && frame.payload.data==encoded+7 && frame.sample_rate==48000 && frame.channels==2 &&
        frame.configuration[0]==17 && frame.configuration[1]==144,
        "ADTS frame borrows raw AAC and derives native stereo48k ASC");
    check(mpeg_ts_aac_frame({encoded+frame.bytes,sizeof(encoded)-frame.bytes},&frame)==E::none && frame.bytes==8 &&
        frame.payload.count==1 && frame.payload.data[0]==44, "multiple ADTS frames in one PES are split at their declared lengths");
    const auto saved=frame; u8 invalid[sizeof(encoded)];
    const auto reject=[&](u32 index,u8 value,E error,const char* label) noexcept {
        copy_bytes(invalid,encoded,sizeof(encoded)); invalid[index]=value;
        check(mpeg_ts_aac_frame({invalid,sizeof(invalid)},&frame)==error && bytes_equal(&frame,&saved,sizeof(frame)),label);
    };
    reject(0,0,E::malformed,"ADTS sync required"); reject(1,243,E::malformed,"ADTS layer must be zero");
    reject(1,240,E::unsupported,"CRC-bearing ADTS is explicit unsupported");
    reject(2,12,E::unsupported,"AAC Main cannot be mislabeled as AAC-LC");
    reject(2,116,E::unsupported,"reserved ADTS frequency rejected");
    reject(3,0,E::unsupported,"ADTS PCE channel layout unsupported");
    reject(6,253,E::unsupported,"multiple raw blocks require a different clock path");
    reject(4,255,E::malformed,"ADTS length cannot exceed PES payload");
    check(mpeg_ts_aac_frame({encoded,6},&frame)==E::malformed && bytes_equal(&frame,&saved,sizeof(frame)),
        "truncated ADTS header preserves output");
    u8 raw[2048]{}, bits_data[2048]{}, packed[2048]{};
    h264_fixture::Output generated{raw,sizeof(raw),0,true}; h264_fixture::Bits bits{bits_data,sizeof(bits_data),0,true};
    h264_fixture::Sps sps{0,66,1,1,0,1,true,0,0,31}; h264_fixture::Pps pps{};
    h264_fixture::write_sps(&generated,&bits,sps); h264_fixture::write_pps(&generated,&bits,pps);
    const u32 headers=generated.count;
    h264_fixture::write_idr(&generated,&bits,sps,pps,1,0,1,false);
    MpegTsAvcConfiguration configuration{}; u32 count=0; H264Sps parsed_sps{}; H264Pps parsed_pps{};
    check(generated.ok && mpeg_ts_avc_configuration({raw,generated.count},&configuration)==E::none &&
        h264_select_parameters({configuration.bytes,configuration.count},0,&parsed_sps,&parsed_pps)==H264Error::none &&
        configuration.width==16 && configuration.height==16,
        "Annex B SPS/PPS become validated native AVC configuration without parameter tables");
    check(mpeg_ts_avc_sample({raw,generated.count},{packed,sizeof(packed)},&count)==E::none && count,
        "Annex B access unit converts to existing four-byte AVC lengths");
    H264NalCursor cursor{{packed,count},0,4}; u32 nals=0; bool idr=false;
    for (;;) { const auto nal=h264_next_nal(&cursor); if(nal.error==H264Error::end) break;
        if(nal.error!=H264Error::none) {check(false,"converted NAL boundary");break;} ++nals; idr=idr||nal.type==5; }
    check(nals==4 && idr,"all original SPS PPS AUD and IDR NALs survive repacking");
    u8 compacted[4096]{}; copy_bytes(compacted+512,raw,generated.count); u32 compacted_count{};
    check(mpeg_ts_avc_sample({compacted+512,generated.count},{compacted,sizeof(compacted)},&compacted_count)==E::none &&
        compacted_count==count && bytes_equal(compacted,packed,count),"bounded in-place AVC compaction preserves every converted byte");
    const u32 prior=count;
    check(mpeg_ts_avc_sample({raw,generated.count},{packed,4},&count)==E::capacity && count==prior &&
        mpeg_ts_avc_sample({raw,headers},{packed,sizeof(packed)},&count)==E::malformed && count==prior &&
        mpeg_ts_avc_sample({raw,generated.count},{raw,sizeof(raw)},&count)==E::invalid_input,
        "AVC repacking rejects capacity header-only and aliasing without publishing a new byte count");
    h264_fixture::write_aud(&generated,&bits,true);
    check(mpeg_ts_avc_sample({raw,generated.count},{packed,sizeof(packed)},&count)==E::unsupported && count==prior,
        "a PES containing a second access unit is not silently assigned one timestamp");
    const auto old_configuration=configuration;
    ++sps.width_mbs; h264_fixture::write_sps(&generated,&bits,sps);
    check(mpeg_ts_avc_configuration({raw,generated.count},&configuration)==E::unsupported &&
        bytes_equal(&configuration,&old_configuration,sizeof(configuration)),
        "conflicting parameter sets preserve the retained decoder configuration");
}
Bytes packed_audio(u64 timestamp,u8 version=3) noexcept {
    Bytes b{};const u8 header[]{'I','D','3',version,0,0,0,0,0,0,'P','R','I','V',0,0,0,0,0,0};b.add({header,sizeof(header)});
    constexpr char owner[]="com.apple.streaming.transportStreamTimestamp";
    b.add({reinterpret_cast<const u8*>(owner),sizeof(owner)});
    for(i32 shift=56;shift>=0;shift-=8) b.byte(static_cast<u8>(timestamp>>shift));
    b.data[9]=static_cast<u8>(b.size-10);b.data[17]=static_cast<u8>(b.size-20);
    const u8 aac[]{255,241,76,128,1,95,252,11,22,33,255,241,76,128,1,31,252,44};b.add({aac,sizeof(aac)});return b;
}
void packed_audio_tests() noexcept {
    constexpr u64 wrap=u64{1}<<33;
    auto bytes=packed_audio(wrap-1000);PackedAudioSegment segment{};
    check(packed_aac_segment(bytes.view(),&segment)==E::none && segment.timestamp==wrap-1000 && segment.payload.count==18,
        "HLS packed AAC obtains its real 33-bit timestamp and borrows ADTS after ID3");
    auto version4=packed_audio(90000,4);
    check(packed_aac_segment(version4.view(),&segment)==E::none && segment.timestamp==90000,
        "ID3v2.4 synchsafe frame lengths retain the same packed audio contract");
    const auto saved=segment;
    const auto reject=[&](Bytes bad,E error,const char* label) noexcept {
        check(packed_aac_segment(bad.view(),&segment)==error && bytes_equal(&segment,&saved,sizeof(segment)),label);
    };
    auto bad=bytes;bad.data[6]=128;reject(bad,E::malformed,"invalid ID3 synchsafe sizes preserve output");
    bad=bytes;bad.data[5]=128;reject(bad,E::unsupported,"unsynchronized ID3 cannot silently alter audio or timestamp bytes");
    bad=bytes;bad.data[18]=1;reject(bad,E::unsupported,"ID3 frame transformations are explicit unsupported");
    bad=bytes;bad.data[20]='x';reject(bad,E::malformed,"packed audio requires the standard timestamp owner");
    bad=packed_audio(wrap);reject(bad,E::malformed,"packed timestamp upper31 bits must be zero");
    bad=bytes;const u32 tag_end=10+bad.data[9],frame_bytes=tag_end-10;
    copy_bytes(bad.data+tag_end+frame_bytes,bad.data+tag_end,bad.size-tag_end);
    copy_bytes(bad.data+tag_end,bad.data+10,frame_bytes);bad.size+=frame_bytes;
    bad.data[9]=static_cast<u8>(bad.data[9]+frame_bytes);
    reject(bad,E::malformed,"duplicate packed timestamps cannot create an ambiguous audio clock");
    for(u32 i=1;i<bytes.size-18+7;++i) {
        bad=bytes;bad.size=i;reject(bad,E::malformed,"truncated ID3 timestamp or first ADTS frame is never published");
    }
    using D=Mp4DemuxError;Mp4SegmentStore store{};Mp4FragmentSource source{};Mp4FragmentPacket packet{};
    check(mpeg_ts_segment_store_initialize(&store,bytes.view(),true,2,42667)==D::none && store.track.sample_rate==48000 &&
        store.track.channels==2 && store.received_count==1 && store.initialization.committed==65536,
        "packed audio uses the existing bounded sample store and one configuration allocation");
    check(mp4_fragment_source_initialize_segments(&store,&source)==D::none && mp4_fragment_source_peek(&source,&packet)==D::none &&
        packet.bytes.count==3 && packet.bytes.data[0]==11 && packet.sample.duration==1024,
        "packed ADTS headers and ID3 metadata do not reach the AAC decoder payload");
    const auto first=source.index.first_time;
    auto following=packed_audio(2840);
    check(mp4_segment_store_request(&store,1)==D::data_pending &&
        mpeg_ts_segment_store_accept(&store,1,store.request_serial,following.view(),42667)==D::none &&
        store.available_end==first+4096 && store.received_count==2,
        "adjacent packed audio spans 33-bit rollover on the exact rational AAC sample grid");
    const auto* retained=mp4_segment_store_find(store,0);
    check(retained && retained->bytes && retained->media.samples==2,"receiving packed audio preserves the active file");
    const u64 end=store.available_end;const auto* active_payload=packet.bytes.data;
    check(mp4_segment_store_extend(&store,0,3,false)==D::none && mp4_segment_store_request(&store,2)==D::data_pending,
        "packed audio demand advances within the same live store");
    auto gap=packed_audio(2840+3840+90000);
    check(mpeg_ts_segment_store_accept(&store,2,store.request_serial,gap.view(),42667)==D::malformed &&
        store.available_end==end && mp4_segment_store_find(store,0) && active_payload[0]==11,
        "a real packed-audio timestamp gap cannot be rounded away or overwrite active audio");
    mp4_fragment_source_release(&source);mp4_segment_store_release(&store);
}
Bytes timed_video(u64 timestamp) noexcept {
    u8 raw[2048]{},bits_data[2048]{};
    h264_fixture::Output generated{raw,sizeof(raw),0,true};h264_fixture::Bits bits{bits_data,sizeof(bits_data),0,true};
    h264_fixture::Sps sps{0,66,1,1,0,1,true,0,0,31};h264_fixture::Pps pps{};
    h264_fixture::write_sps(&generated,&bits,sps);h264_fixture::write_pps(&generated,&bits,pps);
    h264_fixture::write_idr(&generated,&bits,sps,pps,1,0,1,false);
    Bytes value{};psi(value,0,pat());psi(value,4096,pmt());packet(value,257,15,false,{},16);
    auto unit=pes(timestamp,timestamp,0);unit.add({raw,generated.count});u8 counter=0;
    for(u32 at=0;at<unit.size;) {
        const u32 bytes=minimum(184u,unit.size-at);packet(value,257,counter,at==0,{unit.data+at,bytes});
        counter=static_cast<u8>((counter+1)&15);at+=bytes;
    }
    return value;
}
void paired_wrap_tests() noexcept {
    using D=Mp4DemuxError;constexpr u64 wrap=u64{1}<<33;
    constexpr auto manifest=utf8_literal("https://manifest.googlevideo.com/api/manifest/hls_playlist/id/example/file/index.m3u8");
    constexpr auto playlist=utf8_literal("#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:100\n#EXTINF:0.1,\nhttps://rr1.googlevideo.com/videoplayback/id/example/sq/100/file/seg.ts\n");
    for(u32 variant=0;variant<8;++variant) {
        auto* media=static_cast<AccountMedia*>(VirtualAlloc(nullptr,sizeof(AccountMedia),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        auto* v=static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr,sizeof(ProgressiveVideoFetch),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        auto* a=static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr,sizeof(ProgressiveVideoFetch),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        check(media && v && a,"paired wrap fixture owners");
        if(media && v && a) {
            media->progressive=v;media->progressive_audio=a;
            const bool video_later=(variant&1)!=0,audio_first=(variant&2)!=0;
            const auto video=timed_video(video_later?900:wrap-900);auto audio=packed_audio(video_later?wrap-900:900);
            if(variant&4) {const u32 at=10+audio.data[9];audio.data[at+2]=80;audio.data[at+12]=80;}
            check(progressive_video_fetch_begin_hls(v,manifest,playlist,false,true,1000) &&
                progressive_video_fetch_begin_hls(a,manifest,playlist,true,true,1000),"paired wrap live tracks begin");
            ProgressiveVideoFetch* tracks[]{audio_first?a:v,audio_first?v:a};
            for(u32 i=0;i<2;++i) {
                auto* track=tracks[i];const auto next=progressive_video_fetch_next(track,false,1000);
                const auto body=track==v?video.view():audio.view();
                progressive_video_fetch_submitted(track,i+1,next.range);
                CdnClientResponse response{};response.request_id=i+1;response.status_code=200;response.requested_range=next.range;
                response.body_bytes=body.count;response.entity_bytes=body.count;
                check(next.step==ProgressiveVideoStep::submit && account_media_accept_progressive(media,response,body)==ProgressiveVideoAccept::progress,
                    "paired wrap packets arrive through the shared native media lane");
                check(progressive_video_fetch_next(track,false,1000).step==ProgressiveVideoStep::ready,"paired wrap source prepares its actual sample clock");
            }
            const i64 delta=static_cast<i64>(v->fragment.index.first_time*a->fragment.track.timescale)-
                static_cast<i64>(a->fragment.index.first_time*90000);
            const i64 expected=static_cast<i64>(1800)*a->fragment.track.timescale*(video_later?1:-1);
            const i64 difference=delta-expected;
            check(difference>=-90000 && difference<=90000 && segmented_video_synchronize(v,a)==D::none &&
                v->segmented->synchronized && a->segmented->synchronized,
                "paired wrap preserves the20ms offset in either arrival order and both AAC sample rates");
        }
        if(v) {progressive_video_fetch_release(v);VirtualFree(v,0,MEM_RELEASE);}
        if(a) {progressive_video_fetch_release(a);VirtualFree(a,0,MEM_RELEASE);}
        if(media) VirtualFree(media,0,MEM_RELEASE);
    }
}
bool video_samples(ConstSlice<u8> input,u16 pid,Slice<u8> scratch,Slice<u8> packed) noexcept {
    MpegTsReader reader{input,0,~u32{0},pid}; MpegTsPes pes{}; MpegTsAvcConfiguration config{};
    H264Sps sps{}; H264Pps pps{}; u32 pictures=0; u64 previous=0; E error{};
    while((error=mpeg_ts_next(&reader,scratch,&pes))==E::none) {
        u32 size{};
        if(mpeg_ts_avc_sample(pes.payload,packed,&size)!=E::none) return false;
        if(!pictures) {
            if(mpeg_ts_avc_configuration(pes.payload,&config)!=E::none) return false;
            H264NalCursor nals{{packed.data,size},0,4}; u8 identity{}; bool found=false;
            for(;;) {const auto nal=h264_next_nal(&nals); if(nal.error!=H264Error::none) break;
                if(nal.type==1 || nal.type==5) {found=h264_slice_parameter_id(nal.bytes,&identity)==H264Error::none;break;}}
            if(!found || h264_select_parameters({config.bytes,config.count},identity,&sps,&pps)!=H264Error::none) return false;
        }
        H264Picture picture{}; u64 time{};
        if(h264_parse_picture({packed.data,size},4,sps,pps,&picture,{config.bytes,config.count})!=H264Error::none ||
            (!pictures && !picture.first.idr) || !mpeg_ts_unwrap(pes.dts,pictures?previous:pes.dts,&time) ||
            (pictures && time<=previous)) return false;
        previous=time; ++pictures;
    }
    test_write_metric("TS AVC pictures validated: ",pictures);
    test_write_metric("TS AVC width: ",config.width); test_write_metric("TS AVC height: ",config.height);
    return error==E::end_of_stream && pictures && config.width && config.height;
}
bool audio_decode(ConstSlice<u8> input, u16 pid, Slice<u8> scratch) noexcept {
    const auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(apartment)) return false;
    MpegTsReader reader{input,0,~u32{0},pid}; MpegTsPes pes{}; u32 at=0,packets=0,frames=0;
    u64 origin=0; AacDecoder decoder{}; u8 pcm[kAacDecoderMaximumOutputBytes]; bool valid=true,draining=false,ended=false;
    for(u32 calls=0;valid && !ended && calls<4096;++calls) {
        if(decoder.initialized) {
            const auto block=aac_decoder_read(&decoder,{pcm,sizeof(pcm)});
            if(block.ok()) {valid=block.timestamp_valid; frames+=block.frames;continue;}
            if(block.error==AacDecodeError::end_of_stream) {ended=draining;break;}
            if(block.error!=AacDecodeError::need_input || draining) {valid=false;break;}
        }
        if(at==pes.payload.count) {
            E next{};
            if(pid==kMpegTsNoPid) {
                if(reader.offset) next=E::end_of_stream;
                else {PackedAudioSegment packed{};next=packed_aac_segment(input,&packed);
                    if(next==E::none) {pes={packed.payload,packed.timestamp,packed.timestamp};reader.offset=input.count;}}
            } else next=mpeg_ts_next(&reader,scratch,&pes);
            if(next==E::end_of_stream) {valid=decoder.initialized && aac_decoder_drain(&decoder).ok();draining=true;continue;}
            if(next!=E::none || pes.pts!=pes.dts) {valid=false;break;}
            if(!packets) origin=pes.pts;
            else {
                u64 time{}; valid=mpeg_ts_unwrap(pes.pts,origin,&time) && time>=origin;
                const u64 actual=valid?(time-origin)*decoder.sample_rate:0, expected=u64{packets}*1024*90000;
                valid=valid && (actual>expected?actual-expected:expected-actual)<=decoder.sample_rate;
                if(!valid) break;
            }
            at=0;
        }
        MpegTsAacFrame frame{};
        valid=mpeg_ts_aac_frame({pes.payload.data+at,pes.payload.count-at},&frame)==E::none;
        if(!valid) break;
        if(!decoder.initialized) valid=aac_decoder_initialize(&decoder,{frame.configuration,2},frame.sample_rate,frame.channels).ok();
        else valid=frame.sample_rate==decoder.sample_rate && frame.channels==decoder.channels && bytes_equal(frame.configuration,decoder.asc,2);
        if(!valid) break;
        const u64 from=u64{packets}*1024*10000000/decoder.sample_rate,to=u64{packets+1}*1024*10000000/decoder.sample_rate;
        valid=valid && aac_decoder_submit(&decoder,frame.payload,static_cast<i64>(from),to-from).ok();
        if(valid) {++packets;at+=frame.bytes;}
    }
    test_write_metric("TS AAC packets decoded: ",packets); test_write_metric("TS AAC PCM frames: ",frames);
    valid=valid && ended && packets && frames==packets*1024;
    valid=aac_decoder_release(&decoder).ok() && valid; CoUninitialize(); return valid;
}
bool stored_session(ConstSlice<u8> input) noexcept {
    using D=Mp4DemuxError;
    const auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    const HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    Mp4SegmentStore stores[2]{}; Mp4FragmentSource sources[2]{}; Mp4FragmentAudio audio{};
    Mp4Decoder decoder{}; AccountVideoSession session{}; ID3D11Device* device{}; ID3D11DeviceContext* context{};
    bool valid=SUCCEEDED(apartment)&&event;
    for(u32 i=0;valid&&i<2;++i) valid=mpeg_ts_segment_store_initialize(stores+i,input,i!=0,1,2000000)==D::none &&
        stores[i].transport && mp4_segment_store_extend(stores+i,0,1,true)==D::none &&
        mp4_fragment_source_initialize_segments(stores+i,sources+i)==D::none;
    u32 stage=valid?1u:0u;
    if(valid) {
        const u64 vt=sources[0].index.first_time,at=sources[1].index.first_time;
        const u64 vs=sources[0].track.timescale,as=sources[1].track.timescale;
        const bool later=vt/vs!=at/as?vt/vs>at/as:(vt%vs)*as>(at%as)*vs;
        const u64 origin=later?vt:at,scale=later?vs:as;
        for(auto& source:sources) source.track.media_start=static_cast<i64>((origin/scale)*source.track.timescale+
            ((origin%scale)*source.track.timescale+scale/2)/scale);
        valid=mp4_fragment_audio_initialize(sources+1,&audio)==D::none;
    }
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0}; D3D_FEATURE_LEVEL level{};
    valid=valid&&SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,&device,&level,&context)) &&
        mp4_decoder_initialize_fragmented(&decoder,sources,device);
    if(valid) stage=2;
    const AccountVideoIdentity identity{1,2,3,4,0,1,0};
    valid=valid&&account_video_session_initialize(&session,&decoder,identity,event,{},&audio).ok() &&
        account_video_session_control(&session,identity,1,AccountVideoCommand::mute,0,1,true).ok() &&
        account_video_session_control(&session,identity,2,AccountVideoCommand::play).ok();
    if(valid) stage=3;
    const auto epoch=session.playback_epoch; u32 frames=0;
    const auto until=GetTickCount64()+4000;
    while(valid&&!session.status.ended&&GetTickCount64()<until) {
        const auto work=account_video_session_service(&session);
        valid=session.status.error==AccountVideoError::none&&session.playback_epoch==epoch;
        if(work.frame_ready) {++frames;valid=valid&&work.frame.gpu&&work.frame.width==852&&work.frame.height==480&&
            account_video_session_acknowledge(&session,identity,work.playback_epoch,work.frame_serial);}
        if(!work.needs_service) WaitForSingleObject(event,minimum(10u,work.next_service_delay_ms));
    }
    const auto progress=account_video_session_audio_progress(&session);
    valid=valid&&frames&&session.status.ended&&session.video_eof&&session.audio_eof&&
        progress.written_frames&&progress.consumed_frames==progress.written_frames;
    test_write_metric("TS stored session stage: ",stage); test_write_metric("TS stored session error: ",static_cast<u32>(session.status.error));
    test_write_metric("TS stored session native: ",session.status.native_error); test_write_metric("TS GPU frames: ",frames);
    test_write_metric("TS stored PCM written: ",progress.written_frames); test_write_metric("TS stored PCM consumed: ",progress.consumed_frames);
    u32 committed=0; for(const auto& store:stores) committed+=store.initialization.committed+store.slots[0].storage.committed+store.slots[1].storage.committed;
    test_write_metric("TS store committed bytes: ",committed);
    if(session.initialized&&!account_video_session_release(&session,1000).ok()) ExitProcess(1);
    mp4_decoder_release(&decoder); if(context) context->Release();if(device) device->Release();
    for(u32 i=0;i<2;++i) {mp4_fragment_source_release(sources+i);mp4_segment_store_release(stores+i);}
    if(event) CloseHandle(event);if(SUCCEEDED(apartment)) CoUninitialize(); return valid;
}
void store_lifetimes() noexcept {
    using D=Mp4DemuxError;
    const auto file=[](u32 index,bool broken=false) noexcept {
        Bytes value{}; psi(value,0,pat()); psi(value,4096,pmt()); packet(value,257,0,false,{},16);
        auto audio=pes(90000+index*1920,90000+index*1920,10,true);
        const u8 adts[]{255,241,76,128,1,95,252,11,22,33}; copy_bytes(audio.data+19,adts,10);
        if(broken) audio.data[19]=0;
        packet(value,256,static_cast<u8>(index&15),true,audio.view()); return value;
    };
    const auto first=file(0),second=file(1),third=file(2),broken=file(2,true);
    Mp4SegmentStore store{}; Mp4FragmentSource source{};
    check(mpeg_ts_segment_store_initialize(&store,first.view(),true,3,21333)==D::none && store.received_count==1 &&
        mp4_fragment_source_initialize_segments(&store,&source)==D::none,"transport bootstrap supplies its first file without a second download");
    Mp4FragmentPacket packet{};
    check(mp4_fragment_source_peek(&source,&packet)==D::none && packet.bytes.count==3 && packet.bytes.data[0]==11,
        "transport compaction exposes the exact borrowed AAC bytes through the existing sample cursor");
    const auto* borrowed=packet.bytes.data; const u64 initial=source.index.first_time;
    check(mp4_segment_store_request(&store,1)==D::data_pending &&
        mpeg_ts_segment_store_accept(&store,1,store.request_serial,second.view(),21333)==D::none &&
        store.available_end==initial+2048 && borrowed[0]==11,"next transport file preserves the pinned payload and exact audio sample grid");
    Mp4FragmentPosition position{};
    check(mp4_fragment_source_prepare(&source,1,0,&position)==D::none && mp4_segment_store_request(&store,2)==D::data_pending,
        "transport candidate can be borrowed as a normal seek proposal");
    const auto generation=store.generation;
    check(mpeg_ts_segment_store_accept(&store,2,store.request_serial,broken.view(),21333)==D::malformed &&
        store.generation!=generation && !mp4_fragment_source_adopt(&source,position) && borrowed[0]==11 &&
        !mp4_segment_store_find(store,1) && mp4_segment_store_find(store,0),
        "failed candidate replacement invalidates stale proposals without changing active bytes");
    const auto serial=store.request_serial;
    check(mpeg_ts_segment_store_accept(&store,2,serial-1,third.view(),21333)==D::invalid_input &&
        mpeg_ts_segment_store_accept(&store,2,serial,third.view(),21333)==D::none && borrowed[0]==11,
        "transport receipt fences stale serials and reuses its bounded candidate storage");
    check(store.initialization.committed==65536 && store.slots[0].storage.committed==65536 &&
        store.slots[1].storage.committed==65536,"small transport files retain only three bounded committed regions");
    mp4_fragment_source_release(&source); mp4_segment_store_release(&store);
}
void split_video_units() noexcept {
    u8 raw[1024]{}, bits_data[512]{}, packed[1024]{};
    h264_fixture::Output generated{raw,sizeof(raw),0,true};
    h264_fixture::Bits bits{bits_data,sizeof(bits_data),0,true};
    h264_fixture::Sps sps{0,66,1,1,0,1,true,0,0,31};h264_fixture::Pps pps{};
    h264_fixture::write_idr(&generated,&bits,sps,pps,1,0,1,true);
    u32 expected{};
    check(mpeg_ts_avc_sample({raw,generated.count},{packed,sizeof(packed)},&expected)==E::none,
          "split video fixture has a complete AVC access unit");
    constexpr u32 carries[]{1,7,15};
    for(u32 carry : carries) {
        Bytes transport{};psi(transport,0,pat());psi(transport,4096,pmt());packet(transport,257,15,false,{},16);
        u8 counter=0;
        for(u32 i=0;i<2;++i) {
            auto unit=pes(93000+i*3000,90000+i*3000,0);
            if(i) unit.add({raw+generated.count-carry,carry});
            unit.add({raw,generated.count-(i?0:carry)});
            for(u32 at=0;at<unit.size;) {
                const auto n=minimum(184u,unit.size-at);packet(transport,257,counter,at==0,{unit.data+at,n});
                counter=static_cast<u8>((counter+1)&15);at+=n;
            }
        }
        Mp4SegmentStore store{};
        const auto error=mpeg_ts_segment_store_initialize(&store,transport.view(),false,1,66667);
        const auto* slot=mp4_segment_store_find(store,0);
        bool valid=error==Mp4DemuxError::none && slot && slot->media.samples==2;
        if(valid) {
            auto cursor=slot->media.fragment;Mp4FragmentSample sample{};
            for(u32 i=0;i<2;++i)
                valid=valid && mp4_fragment_next(&cursor,&sample)==Mp4DemuxError::none && sample.bytes==expected &&
                    bytes_equal(slot->storage.base+sample.offset,packed,expected) && sample.duration==3000 &&
                    sample.presentation_time-static_cast<i64>(sample.decode_time)==3000;
        }
        check(valid,"AVC frames spanning PES packets preserve complete bytes and each access unit's DTS/PTS");
        mp4_segment_store_release(&store);
    }
}
void store_expansion() noexcept {
    // Three-byte start codes grow by one byte per NAL. Enough short filler
    // units make the converted samples larger than their transport container.
    ReservedBytes input{};
    if(!reserved_bytes_initialize(&input,1024*1024) || !reserved_bytes_commit(&input,1024*1024)) {
        check(false,"transport expansion fixture allocation"); reserved_bytes_release(&input); return;
    }
    u8 raw[4096]{},bits_data[2048]{},packed[4096]{};
    h264_fixture::Output generated{raw,sizeof(raw),0,true}; h264_fixture::Bits bits{bits_data,sizeof(bits_data),0,true};
    h264_fixture::Sps sps{0,66,1,1,0,1,true,0,0,31}; h264_fixture::Pps pps{};
    h264_fixture::write_sps(&generated,&bits,sps); h264_fixture::write_pps(&generated,&bits,pps);
    h264_fixture::write_idr(&generated,&bits,sps,pps,1,0,1,false);
    const u8 filler[]{0,0,1,12,128};
    for(u32 i=0;i<500;++i) {copy_bytes(raw+generated.count,filler,sizeof(filler));generated.count+=sizeof(filler);}
    u32 sample_bytes{};
    check(generated.ok && mpeg_ts_avc_sample({raw,generated.count},{packed,sizeof(packed)},&sample_bytes)==E::none,
        "transport expansion fixture converts within the bounded NAL count");
    Bytes tables{}; psi(tables,0,pat()); psi(tables,4096,pmt()); packet(tables,257,15,false,{},16);
    copy_bytes(input.base,tables.data,tables.size); u32 length=tables.size; u8 counter=0;
    constexpr u32 frames=256;
    for(u32 i=0;i<frames;++i) {
        auto unit=pes(90000+i*3000,90000+i*3000,0); unit.add({raw,generated.count});
        for(u32 at=0;at<unit.size;) {
            const u32 bytes=minimum(184u,unit.size-at); Bytes transport{};
            packet(transport,257,counter,at==0,{unit.data+at,bytes});counter=static_cast<u8>((counter+1)&15);
            copy_bytes(input.base+length,transport.data,transport.size);length+=transport.size;at+=bytes;
        }
    }
    const u32 initial_commit=(length+1024*16+512+65535)&~65535u;
    const u32 compacted=1024*16+frames*sample_bytes;
    check(compacted>initial_commit,"short NAL fixture exceeds the original transport-sized commitment");
    Mp4SegmentStore store{};
    const auto error=mpeg_ts_segment_store_initialize(&store,{input.base,length},false,1,8533333);
    const auto* slot=mp4_segment_store_find(store,0);
    bool valid=error==Mp4DemuxError::none && slot && slot->bytes==compacted && slot->media.samples==frames;
    if(valid) {
        auto cursor=slot->media.fragment; Mp4FragmentSample sample{};u32 count=0;
        while(mp4_fragment_next(&cursor,&sample)==Mp4DemuxError::none) {
            valid=valid && sample.bytes==sample_bytes && bytes_equal(slot->storage.base+sample.offset,packed,sample_bytes);
            ++count;
        }
        valid=valid && count==frames && slot->storage.committed>initial_commit &&
            slot->storage.committed<=compacted+65536+512 && !store.slots[1].storage.base;
    }
    check(valid,"transport store grows only its candidate pages and preserves every expanded sample");
    mp4_segment_store_release(&store);reserved_bytes_release(&input);
}
void capture(const char* name) noexcept {
    char path[1024]{}; wchar_t wide[1024]{};
    const u32 length = GetEnvironmentVariableA(name,path,sizeof(path));
    if (!length) return;
    if (length >= sizeof(path)) { check(false,"capture path bound"); return; }
    for (u32 i = 0; i < length; ++i) { if (static_cast<u8>(path[i]) >= 128) { check(false,"capture path encoding"); return; } wide[i] = path[i]; }
    const HANDLE file = CreateFileW(wide,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file == INVALID_HANDLE_VALUE) { check(false,"capture open"); return; }
    Win32LargeInteger size{}; DWORD read{};
    const bool sized = GetFileSizeEx(file,size.native()) && size.QuadPart > 0 && size.QuadPart <= kMpegTsMaximumBytes;
    auto* memory = sized ? static_cast<u8*>(VirtualAlloc(nullptr,static_cast<usize>(size.QuadPart)*3,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)) : nullptr;
    bool valid = memory && ReadFile(file,memory,static_cast<DWORD>(size.QuadPart),&read,nullptr) && read == size.QuadPart;
    CloseHandle(file); MpegTsProgram p{};
    if(valid && read>=3 && bytes_equal(memory,"ID3",3)) {
        Mp4SegmentStore store{};
        const auto error=mpeg_ts_segment_store_initialize(&store,{memory,read},true,1,5000000);
        test_write_metric("packed AAC store error: ",static_cast<u32>(error));
        test_write_metric("packed AAC sample rate: ",store.track.sample_rate);
        check(error==Mp4DemuxError::none && audio_decode({memory,read},kMpegTsNoPid,{}),
            "captured packed AAC preserves its transport clock and decodes every native audio frame");
        mp4_segment_store_release(&store);VirtualFree(memory,0,MEM_RELEASE);return;
    }
    const auto parsed = valid ? mpeg_ts_program({memory,read},&p) : E::invalid_input;
    test_write_metric("TS program error: ",static_cast<u32>(parsed));
    valid = valid && parsed == E::none;
    const u16 identities[]{p.video_pid,p.audio_pid};
    for (u16 id : identities) {
        if (!valid || id == kMpegTsNoPid) continue;
        MpegTsReader r{{memory,read},0,~u32{0},id}; u32 packets = 0, payload = 0; MpegTsPes sample{}; E error{};
        while ((error = mpeg_ts_next(&r,{memory+read,read},&sample)) == E::none) { ++packets; payload += sample.payload.count; }
        valid = error == E::end_of_stream && packets && payload;
        test_write_metric("TS PES result: ",static_cast<u32>(error));
        test_write_metric("TS elementary PID: ",id); test_write_metric("TS PES packets: ",packets); test_write_metric("TS payload bytes: ",payload);
    }
    check(valid,"captured clear Twitch segment has complete native AVC/AAC PES streams");
    if(valid && p.video_pid!=kMpegTsNoPid)
        check(video_samples({memory,read},p.video_pid,{memory+read,read},{memory+2*read,read}),
            "captured TS AVC uses existing native parameter and picture validation");
    if(valid && p.audio_pid!=kMpegTsNoPid)
        check(audio_decode({memory,read},p.audio_pid,{memory+read,read}),"captured TS AAC decodes through existing native codec with exact frame count");
    if(valid && p.video_pid!=kMpegTsNoPid && p.audio_pid!=kMpegTsNoPid)
        check(stored_session({memory,read}),"captured TS plays through the existing GPU and muted native audio session");
    if(valid && p.video_pid!=kMpegTsNoPid && p.audio_pid==kMpegTsNoPid) {
        Mp4SegmentStore store{};const auto error=mpeg_ts_segment_store_initialize(&store,{memory,read},false,1,5000000);
        test_write_metric("captured video store error: ",static_cast<u32>(error));
        check(error==Mp4DemuxError::none,"captured video-only TS fills the native sample store");
        mp4_segment_store_release(&store);
    }
    if (memory) VirtualFree(memory,0,MEM_RELEASE);
}
} // namespace
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_mpeg_ts_entry() noexcept {
    maicord::run(); maicord::codecs(); maicord::packed_audio_tests(); maicord::paired_wrap_tests(); maicord::store_lifetimes(); maicord::split_video_units(); maicord::store_expansion();
    const char* names[]{"MAICORD_TS_PROBE","MAICORD_TS_NEXT_PROBE","MAICORD_TS_AUDIO_PROBE","MAICORD_TS_AUDIO_NEXT_PROBE"};
    for (const auto* name : names) maicord::capture(name);
    ExitProcess(maicord::failures ? 1 : 0);
}
