#include "audio_support.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/sys/simd.hpp"

// The part of media audio that has no operating system in it: the caller-owned ring, the gain rule and the clock
// conversions. The Windows (media_audio.cpp) and Linux (pulse/media_audio.cpp) transports both build on it.

namespace maicord {namespace {
using Error=MediaAudioError;
using audio_detail::disjoint;
using audio_detail::fail;
bool rate_valid(u32 rate) noexcept { return rate >= 8000 && rate <= 192000; }
bool ring_valid(const MediaAudioRing& r) noexcept {
    if(!r.samples.data||(reinterpret_cast<usize>(r.samples.data)&3)||!r.epoch||!rate_valid(r.sample_rate)||(r.channels!=1&&r.channels!=2)||
        r.samples.count%r.channels||r.samples.count/r.channels<kMediaAudioMinimumRingFrames||r.samples.count/r.channels>kMediaAudioMaximumRingFrames||
        !byte_range_valid({reinterpret_cast<const u8*>(r.samples.data),r.samples.count*4}))return false;
    return r.consumed<=r.submitted&&r.submitted<=r.written&&r.written-r.consumed<=r.samples.count/r.channels;
}
// No clip here. The system applies the user's volume to these samples and limits them at the device.
// A clip at this point cuts every decoded peak above full scale, also at a low volume.
void scale(float* out,const float* in,u32 count,float gain) noexcept {
    const __m256 factor=_mm256_set1_ps(gain);u32 i=0;
    for(;i+8<=count;i+=8)_mm256_storeu_ps(out+i,_mm256_mul_ps(_mm256_loadu_ps(in+i),factor));
#pragma clang loop vectorize(disable) unroll(disable)
    for(;i<count;++i)out[i]=in[i]*gain;
}
// A sample is finite unless all the bits of its exponent are set. Only then does the sum carry into the sign bit.
inline bool all_finite(const float* in,u32 count) noexcept {
    const __m128i exponent=_mm_set1_epi32(0x7f800000),one=_mm_set1_epi32(0x00800000);__m128i carry=_mm_setzero_si128();u32 i=0;
    for(;i+4<=count;i+=4)carry=_mm_or_si128(carry,_mm_add_epi32(_mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i_u*>(in+i)),exponent),one));
    bool finite=!(_mm_movemask_epi8(carry)&0x8888);
#pragma clang loop vectorize(disable) unroll(disable)
    for(;i<count;++i)finite=finite&&is_finite(in[i]);
    return finite;
}
}
bool media_audio_gain_valid(float gain) noexcept {return is_finite(gain)&&gain>=0&&gain<=1;}
bool media_audio_ring_initialize(MediaAudioRing* r,Slice<float> samples,u32 rate,u32 channels,u64 epoch) noexcept {
    if(!r||!disjoint(r,sizeof(*r),samples.data,samples.count>32768?0:samples.count*4)||samples.count>32768)return false;
    MediaAudioRing candidate{samples,epoch,0,0,0,channels,rate,false};if(!ring_valid(candidate))return false;*r=candidate;return true;
}
u32 media_audio_ring_free(const MediaAudioRing& r) noexcept {
    return ring_valid(r)?r.samples.count/r.channels-static_cast<u32>(r.written-r.consumed):0;
}
MediaAudioResult media_audio_ring_write(MediaAudioRing* r,u64 epoch,ConstSlice<float> pcm) noexcept {
    if(!r||!ring_valid(*r)||!pcm.count||pcm.count%r->channels||pcm.count/r->channels>kMediaAudioMaximumWriteFrames||
        (reinterpret_cast<usize>(pcm.data)&3)||!disjoint(r,sizeof(*r),pcm.data,pcm.count*4)||
        !disjoint(r->samples.data,r->samples.count*4,pcm.data,pcm.count*4))return fail(Error::invalid_input);
    if(epoch!=r->epoch)return fail(Error::stale_epoch);if(r->ended)return fail(Error::stopped);
    const u32 frames=pcm.count/r->channels;if(frames>media_audio_ring_free(*r))return fail(Error::capacity);
    if(r->written>~u64{0}-frames)return fail(Error::timeline);
    if(!all_finite(pcm.data,pcm.count))return fail(Error::invalid_input);
    // The ring wraps at its sample count, a whole number of frames: at most two contiguous copies.
    const u32 start=static_cast<u32>(r->written%(r->samples.count/r->channels))*r->channels,first=minimum(pcm.count,r->samples.count-start);
    copy_bytes(r->samples.data+start,pcm.data,first*4);copy_bytes(r->samples.data,pcm.data+first,(pcm.count-first)*4);
    r->written+=frames;return {};
}
u32 media_audio_ring_render(MediaAudioRing* r,Slice<float> output,float gain) noexcept {
    if(!r||!ring_valid(*r)||!media_audio_gain_valid(gain)||!output.count||output.count%r->channels||
        output.count/r->channels>kMediaAudioMaximumWriteFrames||(reinterpret_cast<usize>(output.data)&3)||
        !disjoint(r,sizeof(*r),output.data,output.count*4)||!disjoint(r->samples.data,r->samples.count*4,output.data,output.count*4))return 0;
    const u32 count=static_cast<u32>(minimum<u64>(output.count/r->channels,r->written-r->submitted)),capacity=r->samples.count/r->channels;
    const u32 total=count*r->channels,start=static_cast<u32>(r->submitted%capacity)*r->channels,first=minimum(total,r->samples.count-start);
    scale(output.data,r->samples.data+start,first,gain);scale(output.data+first,r->samples.data,total-first,gain);
    r->submitted+=count;return count;
}
bool media_audio_ring_native_queue_empty(const MediaAudioRing& r,u32 padding) noexcept {
    return ring_valid(r)&&!padding&&r.submitted==r.consumed;
}
void media_audio_ring_rewind(MediaAudioRing* r) noexcept {if(r&&ring_valid(*r))r->submitted=r->consumed;}
bool media_audio_clock_time(u64 position,u64 frequency,u64* time) noexcept {
    return media_audio_clock_frames(position,frequency,10000000,time);
}
bool media_audio_clock_frames(u64 position,u64 frequency,u32 rate,u64* frames) noexcept {
    if(!frames||!frequency||!rate)return false;
    const u64 whole=position/frequency,remainder=position%frequency;
    if(whole>~u64{0}/rate||remainder>~u64{0}/rate)return false;
    const u64 base=whole*rate,fraction=remainder*rate/frequency;
    if(base>~u64{0}-fraction)return false;*frames=base+fraction;return true;
}
} // namespace maicord
