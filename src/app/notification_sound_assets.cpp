#include "maicord/app/notification_sound_assets.hpp"
#include "maicord/media/notification_pcm.hpp"
#include "maicord/core/byte_order.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord {
bool notification_sound_asset_resolve(void* context,u32 id,NotificationAudioClip* clip) noexcept {
    auto* bank=static_cast<NotificationSoundAssetBank*>(context);
    if(!bank || !clip || id<1 || id>kNotificationSoundAssetCount) return false;
    const u32 index=id-1;
    if(bank->failed_mask&(1u<<index)) return false;
    if(!bank->pcm[index]) {
        const HMODULE module=bank->module?bank->module:GetModuleHandleW(nullptr);
        HRSRC resource=FindResourceW(module,MAKEINTRESOURCEW(kNotificationMessageAlertResource+index),RT_RCDATA);
        const u32 count=resource?SizeofResource(module,resource):0;
        HGLOBAL loaded=resource?LoadResource(module,resource):nullptr;
        const auto* data=loaded?static_cast<const u8*>(LockResource(loaded)):nullptr;
        const u32 rate=data&&count>=16?byte_load_le32(data+12):0;
        if(!data || count<=16 || count>2*1024*1024 || !bytes_equal(data,"MCSP",4) || data[4]!=1 ||
            data[5]>2 || data[6]>2 || data[7]!=2 ||
            (rate!=kNotificationAudioRate&&rate!=kNotificationAudioAlternateRate)) {
            bank->failed_mask|=1u<<index; return false;
        }
        const u32 frames=byte_load_le32(data+8);
        if(!frames || frames>10*rate || frames>kNotificationPcmMaximumFrames) {bank->failed_mask|=1u<<index;return false;}
        const u32 bytes=frames*4;
        void* pcm=VirtualAlloc(nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        if(!pcm) return false;
        DeflateWorkspace* workspace=deflate_workspace_allocate();
        if(!workspace) {VirtualFree(pcm,0,MEM_RELEASE);return false;}
        const auto result=notification_pcm_unpack({data+16,count-16},{frames,data[5],data[6]},
            {static_cast<u8*>(pcm),bytes},workspace);
        deflate_workspace_free(workspace);
        if(!result.ok()) {VirtualFree(pcm,0,MEM_RELEASE);bank->failed_mask|=1u<<index;return false;}
        DWORD old{};
        if(!VirtualProtect(pcm,bytes,PAGE_READONLY,&old)) {VirtualFree(pcm,0,MEM_RELEASE);return false;}
        bank->pcm[index]=pcm;bank->frames[index]=frames;bank->rates[index]=rate;
    }
    // Give Bluetooth endpoints 100 ms to start and drain short alerts.
    // The mixer generates these zeros; the asset and decoded bank stay compact.
    *clip={{static_cast<const i16*>(bank->pcm[index]),bank->frames[index]*2},bank->rates[index],bank->rates[index]/10};
    return true;
}
bool notification_sound_asset_gain(void* context,float nominal,float* gain) noexcept {
    auto* bank=static_cast<NotificationSoundAssetBank*>(context);
    if(!bank || !gain || !notification_audio_gain_valid(nominal)) return false;
    const LONG percent=ReadAcquire(&bank->output_percent);
    if(percent<0 || percent>200) return false;
    const float actual=(static_cast<float>(percent)/100.0f)*nominal;
    *gain=actual>1.0f?1.0f:actual;return true;
}
void notification_sound_assets_volume(NotificationSoundAssetBank* bank,u32 percent) noexcept {
    if(bank && percent<=200) InterlockedExchange(&bank->output_percent,static_cast<LONG>(percent));
}
void notification_sound_assets_release(NotificationSoundAssetBank* bank) noexcept {
    if(!bank) return;
    for(auto& pcm:bank->pcm) {if(pcm)VirtualFree(pcm,0,MEM_RELEASE);pcm=nullptr;}
    for(u32 i=0;i<kNotificationSoundAssetCount;++i){bank->frames[i]=0;bank->rates[i]=0;}
    bank->failed_mask=0;
}
}
