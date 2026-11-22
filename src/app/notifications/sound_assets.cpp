#include "maicord/app/notifications/sound_assets.hpp"
#include "maicord/media/audio/notification_pcm.hpp"
#include "maicord/core/byte_order.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/resource.hpp"
#include "maicord/core/scratch_pages.hpp"

namespace maicord {
namespace {
// One note of a voice connection sound: a sine with a quieter third harmonic, for a soft and woody sound. A note
// rises over 4 ms, decays to about 1/64 of its level and fades out over its last 8 ms, so it ends without a click.
struct Note {u16 start_ms,length_ms,hz;};
constexpr Note kVoiceNotes[]{
    {0,160,659},{80,160,880}, // Join: E5 then A5, a rising fourth.
    {0,160,880},{80,160,659}, // Leave: the same fourth, falling.
    // Interrupted: three short pulses of two close tones that beat against each other.
    {0,70,415},{0,70,440},{110,70,415},{110,70,440},{220,70,415},{220,70,440},
};
// Clip 3 + i plays notes kVoiceNoteRange[i] up to, but not including, kVoiceNoteRange[i + 1].
constexpr u8 kVoiceNoteRange[]{0,2,4,10};
constexpr u32 kVoiceNoteMost=6,kVoiceRate=kNotificationAudioAlternateRate,kFramesPerMs=kVoiceRate/1000;
static_assert(sizeof(kVoiceNoteRange)==kNotificationSoundClipCount-kNotificationSoundAssetCount+1);
// Cosine and sine of an angle below 0.4 rad from their Taylor series. The error stays below 1e-7.
void rotation(float angle,float* c,float* s) noexcept {
    const float a2=angle*angle;
    *c=1.0f-a2/2.0f*(1.0f-a2/12.0f*(1.0f-a2/30.0f));
    *s=angle*(1.0f-a2/6.0f*(1.0f-a2/20.0f*(1.0f-a2/42.0f)));
}
bool voice_tone(NotificationSoundAssetBank* bank,u32 index) noexcept {
    const u32 first=kVoiceNoteRange[index-kNotificationSoundAssetCount];
    const u32 last=kVoiceNoteRange[index-kNotificationSoundAssetCount+1];
    u32 frames=0;
    for(u32 n=first;n<last;++n) {
        const u32 end=(kVoiceNotes[n].start_ms+kVoiceNotes[n].length_ms)*kFramesPerMs;
        frames=end>frames?end:frames;
    }
    auto* pcm=static_cast<i16*>(commit_pages(frames*4));
    if(!pcm) return false;
    // Each partial is a unit phasor that turns by one fixed rotation per frame. Its imaginary part is the sine.
    float re[2*kVoiceNoteMost],im[2*kVoiceNoteMost],c[2*kVoiceNoteMost],s[2*kVoiceNoteMost];
    float level[kVoiceNoteMost],decay[kVoiceNoteMost];
    for(u32 n=first;n<last;++n) {
        const u32 k=n-first;
        for(u32 p=0;p<2;++p) {
            const float hz=static_cast<float>(kVoiceNotes[n].hz*(1+2*p));
            rotation(6.2831853f*hz/static_cast<float>(kVoiceRate),&c[2*k+p],&s[2*k+p]);
            re[2*k+p]=1.0f;im[2*k+p]=0.0f;
        }
        // (1 - 4.2/N) to the power N is about e^-4.2, or 1/64.
        level[k]=.3f;decay[k]=1.0f-4.2f/static_cast<float>(kVoiceNotes[n].length_ms*kFramesPerMs);
    }
    for(u32 f=0;f<frames;++f) {
        float sum=0.0f;
        for(u32 n=first;n<last;++n) {
            const u32 k=n-first,start=kVoiceNotes[n].start_ms*kFramesPerMs,length=kVoiceNotes[n].length_ms*kFramesPerMs;
            if(f<start || f>=start+length) continue;
            const u32 t=f-start;
            float envelope=level[k];level[k]*=decay[k];
            if(t<4*kFramesPerMs) envelope*=static_cast<float>(t)/static_cast<float>(4*kFramesPerMs);
            if(length-t<8*kFramesPerMs) envelope*=static_cast<float>(length-t)/static_cast<float>(8*kFramesPerMs);
            for(u32 q=2*k;q<2*k+2;++q) {
                const float x=re[q]*c[q]-im[q]*s[q];
                im[q]=re[q]*s[q]+im[q]*c[q];re[q]=x;
            }
            sum+=envelope*(im[2*k]+.25f*im[2*k+1]);
        }
        pcm[2*f]=pcm[2*f+1]=static_cast<i16>(sum*32767.0f);
    }
    if(!pages_protect(pcm,frames*4,PageAccess::read)) {pages_release(pcm);return false;}
    bank->pcm[index]=pcm;bank->frames[index]=frames;bank->rates[index]=kVoiceRate;
    return true;
}
}
bool notification_sound_asset_resolve(void* context,u32 id,NotificationAudioClip* clip) noexcept {
    auto* bank=static_cast<NotificationSoundAssetBank*>(context);
    if(id<1 || id>kNotificationSoundClipCount) return false;
    const u32 index=id-1;
    if(bank->failed_mask&(1u<<index)) return false;
    if(!bank->pcm[index] && index>=kNotificationSoundAssetCount && !voice_tone(bank,index)) return false;
    if(!bank->pcm[index]) {
        const ConstSlice<u8> resource=program_resource(bank->module,kNotificationMessageAlertResource+index);
        const u8* data=resource.data;const u32 count=resource.count;
        const u32 rate=data&&count>=16?byte_load_le32(data+12):0;
        if(!data || count<=16 || count>2*1024*1024 || !bytes_equal(data,"MCSP",4) || data[4]!=1 ||
            data[5]>2 || data[6]>2 || data[7]!=2 ||
            (rate!=kNotificationAudioRate&&rate!=kNotificationAudioAlternateRate)) {
            bank->failed_mask|=1u<<index; return false;
        }
        const u32 frames=byte_load_le32(data+8);
        if(!frames || frames>10*rate || frames>kNotificationPcmMaximumFrames) {bank->failed_mask|=1u<<index;return false;}
        const u32 bytes=frames*4;
        void* pcm=commit_pages(bytes);
        if(!pcm) return false;
        DeflateWorkspace* workspace=deflate_workspace_allocate();
        if(!workspace) {pages_release(pcm);return false;}
        const bool unpacked=notification_pcm_unpack({data+16,count-16},{frames,data[5],data[6]},
            {static_cast<u8*>(pcm),bytes},workspace);
        deflate_workspace_free(workspace);
        if(!unpacked) {pages_release(pcm);bank->failed_mask|=1u<<index;return false;}
        if(!pages_protect(pcm,bytes,PageAccess::read)) {pages_release(pcm);return false;}
        bank->pcm[index]=pcm;bank->frames[index]=frames;bank->rates[index]=rate;
    }
    // Give Bluetooth endpoints 100 ms to start and drain short alerts.
    // The mixer generates these zeros; the asset and decoded bank stay compact.
    *clip={{static_cast<const i16*>(bank->pcm[index]),bank->frames[index]*2},bank->rates[index],bank->rates[index]/10};
    return true;
}
bool notification_sound_asset_gain(void* context,float nominal,float* gain) noexcept {
    auto* bank=static_cast<NotificationSoundAssetBank*>(context);
    if(!notification_audio_gain_valid(nominal)) return false;
    const LONG percent=ReadAcquire(&bank->output_percent);
    if(percent<0 || percent>200) return false;
    const float actual=(static_cast<float>(percent)/100.0f)*nominal;
    *gain=actual>1.0f?1.0f:actual;return true;
}
void notification_sound_assets_volume(NotificationSoundAssetBank* bank,u32 percent) noexcept {
    // The drain stores the volume on each turn of the main loop: write only a new value.
    if(percent<=200 && ReadAcquire(&bank->output_percent)!=static_cast<LONG>(percent))
        InterlockedExchange(&bank->output_percent,static_cast<LONG>(percent));
}
void notification_sound_assets_release(NotificationSoundAssetBank* bank) noexcept {
    for(auto& pcm:bank->pcm) {if(pcm)pages_release(pcm);pcm=nullptr;}
    for(u32 i=0;i<kNotificationSoundClipCount;++i){bank->frames[i]=0;bank->rates[i]=0;}
    bank->failed_mask=0;
}
}
