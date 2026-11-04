#include "audio_support.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_range.hpp"

// The part of notification audio that has no operating system in it: clip and gain validation, request
// preparation and the offline source-rate mixer. The Windows (notification_audio.cpp) and Linux
// (pulse/notification_audio.cpp) transports both build on it.

namespace maicord {namespace {
using audio_detail::fail;
float clipped(float v) noexcept {return v>32767.0f?32767.0f:v< -32768.0f? -32768.0f:v;}
bool rate_valid(u32 rate) noexcept {return rate==kNotificationAudioRate||rate==kNotificationAudioAlternateRate;}
u64& submitted(NotificationAudioMixer& m,u32 rate) noexcept {return rate==kNotificationAudioRate?m.submitted:m.submitted_48000;}
u64& consumed(NotificationAudioMixer& m,u32 rate) noexcept {return rate==kNotificationAudioRate?m.consumed:m.consumed_48000;}
u64 submitted(const NotificationAudioMixer& m,u32 rate) noexcept {return rate==kNotificationAudioRate?m.submitted:m.submitted_48000;}
}
bool notification_audio_clip_valid(NotificationAudioClip clip) noexcept {
    const auto s=clip.samples;
    return rate_valid(clip.sample_rate) && clip.silence_frames<=clip.sample_rate &&
        s.data && s.count && !(s.count&1u) && s.count<=20*clip.sample_rate &&
        !(reinterpret_cast<usize>(s.data)&1u) && reinterpret_cast<usize>(s.data)<=~usize{0}-usize(s.count)*sizeof(i16);
}
bool notification_audio_gain_valid(float gain) noexcept {return gain>=0.0f&&gain<=1.0f;}
bool notification_audio_record_stream_failure(NotificationAudioStatus* status,u64 closed_epoch,u32 native_error) noexcept {
    if(!status||!closed_epoch||status->desired_epoch!=closed_epoch)return false;
    status->error=NotificationAudioError::stream;status->native_error=native_error;return true;
}
NotificationAudioResult notification_audio_prepare_request(const NotificationAudioConfig& config,
    const NotificationAudioRequest& request,NotificationAudioClip* clip,float* gain) noexcept {
    if(!clip||!gain||!notification_audio_gain_valid(request.gain))return fail(NotificationAudioError::invalid_input);
    NotificationAudioClip resolved=request.clip;
    if(!resolved.samples.count&&(!config.resolve||!request.clip_id||!config.resolve(config.context,request.clip_id,&resolved)))
        return fail(NotificationAudioError::resolve);
    if(!notification_audio_clip_valid(resolved))return fail(NotificationAudioError::invalid_input);
    float effective=request.gain;
    if(config.gain&&!config.gain(config.context,request.gain,&effective))return fail(NotificationAudioError::invalid_input);
    if(!notification_audio_gain_valid(effective))return fail(NotificationAudioError::invalid_input);
    *clip=resolved;*gain=effective;return {};
}
void notification_audio_mixer_reset(NotificationAudioMixer* m,u64 epoch) noexcept {if(m){clear_bytes(m,sizeof(*m));m->epoch=epoch;}}
NotificationAudioResult notification_audio_mixer_add(NotificationAudioMixer* m,u64 epoch,NotificationAudioClip clip,float gain) noexcept {
    if(!m||!epoch||!notification_audio_clip_valid(clip)||!notification_audio_gain_valid(gain)||
        byte_ranges_conflict(m,sizeof(*m),clip.samples.data,usize(clip.samples.count)*sizeof(i16)))return fail(NotificationAudioError::invalid_input);
    if(epoch!=m->epoch)return fail(NotificationAudioError::stale_epoch);
    if(m->count==kNotificationAudioVoiceCapacity)return fail(NotificationAudioError::full_voices);
    const u64 position=submitted(*m,clip.sample_rate);
    if(position>~u64{0}-clip.samples.count/2-2*clip.silence_frames)return fail(NotificationAudioError::timeline_exhausted);
    m->voices[m->count++]={clip,position+clip.silence_frames,gain};return {};
}
u32 notification_audio_mixer_voices_rate(const NotificationAudioMixer& m,u32 rate) noexcept {
    if(!rate_valid(rate))return 0;u32 count{};
#pragma clang loop vectorize(disable) unroll(disable)
    for(u32 i=0;i<m.count;++i)if(m.voices[i].clip.sample_rate==rate)++count;
    return count;
}
u32 notification_audio_mixer_needed_rate(const NotificationAudioMixer& m,u32 rate) noexcept {
    if(!rate_valid(rate))return 0;const u64 position=submitted(m,rate);u64 end=position;
    for(u32 i=0;i<m.count;++i)if(m.voices[i].clip.sample_rate==rate)end=maximum(end,m.voices[i].start+m.voices[i].clip.samples.count/2+m.voices[i].clip.silence_frames);
    return static_cast<u32>(minimum<u64>(end-position,10*rate));
}
NotificationAudioMixResult notification_audio_mixer_render_rate(NotificationAudioMixer* m,Slice<i16> output,u32 rate) noexcept {
    if(!m||!m->epoch||!rate_valid(rate)||!output.data||!output.count||(output.count&1u)||output.count>2*kNotificationAudioMixChunkFrames||
        byte_ranges_conflict(m,sizeof(*m),output.data,usize(output.count)*sizeof(i16)))return {NotificationAudioError::invalid_input};
    for(u32 i=0;i<m->count;++i)if(byte_ranges_conflict(output.data,usize(output.count)*sizeof(i16),
        m->voices[i].clip.samples.data,usize(m->voices[i].clip.samples.count)*sizeof(i16)))return {NotificationAudioError::invalid_input};
    const u32 frames=minimum(output.count/2,notification_audio_mixer_needed_rate(*m,rate));
    bool nonzero=false;
    for(u32 i=0;i<frames;++i){float left=0,right=0;const u64 position=submitted(*m,rate)+i;
        for(u32 j=0;j<m->count;++j){const auto& v=m->voices[j];
            if(v.clip.sample_rate!=rate||position<v.start||position-v.start>=v.clip.samples.count/2)continue;
            const u32 sample=static_cast<u32>(position-v.start)*2;
            left+=float(v.clip.samples[sample])*v.gain;right+=float(v.clip.samples[sample+1])*v.gain;
        }
        output[2*i]=static_cast<i16>(clipped(left));output[2*i+1]=static_cast<i16>(clipped(right));
        nonzero=nonzero||output[2*i]!=0||output[2*i+1]!=0;
    }
    submitted(*m,rate)+=frames;return {NotificationAudioError::none,frames,nonzero};
}
u32 notification_audio_mixer_played_rate(NotificationAudioMixer* m,u32 padding,u32 rate) noexcept {
    if(!m||!rate_valid(rate)||padding>submitted(*m,rate)-consumed(*m,rate))return 0;
    consumed(*m,rate)=submitted(*m,rate)-padding;u32 kept=0;
    for(u32 i=0;i<m->count;++i)if(m->voices[i].clip.sample_rate!=rate||m->voices[i].start+m->voices[i].clip.samples.count/2+m->voices[i].clip.silence_frames>consumed(*m,rate))m->voices[kept++]=m->voices[i];
    const u32 retired=m->count-kept;for(u32 i=kept;i<m->count;++i)reset_value(&m->voices[i]);m->count=kept;return retired;
}
void notification_audio_mixer_rewind_rate(NotificationAudioMixer* m,u32 rate) noexcept {if(m&&rate_valid(rate))submitted(*m,rate)=consumed(*m,rate);}
} // namespace maicord
