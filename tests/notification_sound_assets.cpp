#include "maicord/app/notification_sound_assets.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/crypto.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
bool pcm_hash(NotificationAudioClip clip,const u8 (&expected)[32]) noexcept {
    BCRYPT_HASH_HANDLE hash{};u8 digest[32]{};
    NTSTATUS status=BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE,&hash,nullptr,0,nullptr,0,0);
    if(status>=0)status=BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<i16*>(clip.samples.data)),clip.samples.count*2,0);
    if(status>=0)status=BCryptFinishHash(hash,digest,32,0);
    if(hash)BCryptDestroyHash(hash);
    return status>=0&&bytes_equal(digest,expected,32);
}
bool pcm_hash_hex(NotificationAudioClip clip,const char* hex) noexcept {
    u8 expected[32]{};
    for(u32 i=0;i<32;++i){
        const char a=hex[i*2],b=hex[i*2+1];
        const u32 high=a>='0'&&a<='9'?a-'0':a>='a'&&a<='f'?a-'a'+10:16;
        const u32 low=b>='0'&&b<='9'?b-'0':b>='a'&&b<='f'?b-'a'+10:16;
        if(high>15||low>15)return false;expected[i]=static_cast<u8>(high*16+low);
    }
    return !hex[64]&&pcm_hash(clip,expected);
}
bool readonly_pcm(const void* address,u32 bytes) noexcept {
    MEMORY_BASIC_INFORMATION region{};
    return address&&VirtualQuery(address,&region,sizeof(region))==sizeof(region)&&
        region.State==MEM_COMMIT&&region.Type==MEM_PRIVATE&&region.Protect==PAGE_READONLY&&
        region.BaseAddress==address&&region.RegionSize>=bytes;
}
bool freed(const void* address) noexcept {
    MEMORY_BASIC_INFORMATION region{};
    return address&&VirtualQuery(address,&region,sizeof(region))==sizeof(region)&&region.State==MEM_FREE;
}
void measurement(const char* name,u64 value) noexcept {
    test_write_metric(name, value);
}

}
void test_notification_sound_assets(void(*check)(bool,const char*) noexcept) noexcept {
    // Only metadata, resource decoding and VM lifetime are exercised. No audio
    // owner is initialized, no worker exists and no endpoint can be opened.
    NotificationSoundAssetBank bank{};bank.module=GetModuleHandleW(nullptr);
    check(bank.module&&!bank.pcm[0]&&!bank.pcm[1]&&!bank.failed_mask&&bank.output_percent==100,
        "sound assets: metadata initialization allocates no PCM bank");
    const u32 frame_counts[]{11040,7200};
    const u32 rates[]{48000,48000};
    bool metadata=true;
    for(u32 i=0;i<kNotificationSoundAssetCount;++i){
        HRSRC resource=FindResourceW(bank.module,MAKEINTRESOURCEW(kNotificationMessageAlertResource+i),RT_RCDATA);
        const u32 bytes=resource?SizeofResource(bank.module,resource):0;
        HGLOBAL loaded=resource?LoadResource(bank.module,resource):nullptr;
        const auto* packed=loaded?static_cast<const u8*>(LockResource(loaded)):nullptr;
        metadata=metadata&&packed&&bytes>16&&bytes<=2*1024*1024;
        if(packed&&bytes>16)metadata=metadata&&bytes_equal(packed,"MCSP",4)&&packed[4]==1&&
            packed[5]<=2&&packed[6]<=2&&packed[7]==2&&byte_load_le32(packed+8)==frame_counts[i]&&
            byte_load_le32(packed+12)==rates[i];
    }
    check(metadata,"sound assets: both original resource headers preserve stereo rate and exact frames");
    i16 sentinel_sample{};NotificationAudioClip untouched{{&sentinel_sample,1}};
    check(!notification_sound_asset_resolve(nullptr,1,&untouched)&&
        !notification_sound_asset_resolve(&bank,0,&untouched)&&
        !notification_sound_asset_resolve(&bank,kNotificationSoundAssetCount+1,&untouched)&&
        !notification_sound_asset_resolve(&bank,~0u,&untouched)&&
        !notification_sound_asset_resolve(&bank,1,nullptr)&&
        untouched.samples.data==&sentinel_sample&&untouched.samples.count==1&&!bank.failed_mask&&!bank.pcm[0]&&!bank.pcm[1],
        "sound assets: invalid callback inputs preserve descriptor and lazy bank");
    NotificationAudioClip first{},third{},again{};
    const bool first_ok=notification_sound_asset_resolve(&bank,1,&first);
    check(first_ok&&bank.frames[0]==11040&&first.samples.count==22080&&
        first.sample_rate==48000&&readonly_pcm(first.samples.data,44160)&&pcm_hash_hex(first,"3162e7907b056612cd095fe89643f946c93ba8414165e927b57b5bf0ba9f51fc")&&!bank.pcm[1],
        "sound assets: original ordinary alert resolves exact canonical 44160-byte PCM into private readonly VM");
    const bool third_ok=notification_sound_asset_resolve(&bank,2,&third);
    check(third_ok&&bank.frames[1]==7200&&third.samples.count==14400&&
        third.sample_rate==48000&&readonly_pcm(third.samples.data,28800)&&pcm_hash_hex(third,"41431438a81d1b593e2f2242c9d0e7dee6e764f1692a1a0683b08c61c731dfd1"),
        "sound assets: original focused alert resolves exact canonical 28800-byte PCM into private readonly VM");
    check(first_ok&&third_ok&&first.samples.data!=third.samples.data&&
        notification_sound_asset_resolve(&bank,1,&again)&&again.samples.data==first.samples.data&&again.samples.count==first.samples.count&&
        notification_sound_asset_resolve(&bank,2,&again)&&again.samples.data==third.samples.data&&again.samples.count==third.samples.count,
        "sound assets: repeated resolves share each immutable clip without reallocating either bank");
    u64 committed_bytes{},reserved_bytes{},logical_bytes{};bool regions=true;
    for(u32 i=0;i<kNotificationSoundAssetCount;++i){
        MEMORY_BASIC_INFORMATION live{},tail{};
        const bool found=VirtualQuery(bank.pcm[i],&live,sizeof(live))==sizeof(live)&&live.AllocationBase==bank.pcm[i]&&live.State==MEM_COMMIT;
        regions=regions&&found;if(!found)continue;
        committed_bytes+=live.RegionSize;reserved_bytes+=live.RegionSize;logical_bytes+=bank.frames[i]*4;
        const auto* end=static_cast<const u8*>(bank.pcm[i])+live.RegionSize;
        if(VirtualQuery(end,&tail,sizeof(tail))==sizeof(tail)&&tail.AllocationBase==bank.pcm[i]){
            regions=regions&&tail.State==MEM_RESERVE;reserved_bytes+=tail.RegionSize;
        }
    }
    check(regions&&logical_bytes==72960&&committed_bytes>=logical_bytes&&reserved_bytes>=committed_bytes&&reserved_bytes<=128*1024,
        "sound assets: both decoded original clips stay within the bounded shared VM bank budget");
    measurement("sound_asset_bank_metadata_bytes=",sizeof(bank));
    measurement("sound_asset_original_pair_pcm_logical_bytes=",logical_bytes);
    measurement("sound_asset_original_pair_vm_committed_bytes=",committed_bytes);
    measurement("sound_asset_original_pair_vm_reserved_bytes=",reserved_bytes);
    bool volumes=true;
    for(u32 percent=0;percent<=200;++percent){
        notification_sound_assets_volume(&bank,percent);float gain=-1,clamped=-1;
        const float expected=static_cast<float>(percent)/200.0f;
        const float expected_clamp=percent>100?1.0f:static_cast<float>(percent)/100.0f;
        volumes=volumes&&bank.output_percent==static_cast<LONG>(percent)&&
            notification_sound_asset_gain(&bank,.5f,&gain)&&gain>=expected-.000001f&&gain<=expected+.000001f&&
            notification_sound_asset_gain(&bank,1.f,&clamped)&&clamped==expected_clamp;
    }
    check(volumes,"sound assets: every exact output percent samples gain and clamps boosted nominal gain to one");
    notification_sound_assets_volume(&bank,73);notification_sound_assets_volume(&bank,201);
    notification_sound_assets_volume(&bank,~0u);notification_sound_assets_volume(nullptr,100);
    float gain=9;u32 nan_bits=0x7FC00000;float nan{};copy_bytes(&nan,&nan_bits,sizeof(nan));
    check(bank.output_percent==73&&!notification_sound_asset_gain(nullptr,.5f,&gain)&&
        !notification_sound_asset_gain(&bank,-.1f,&gain)&&!notification_sound_asset_gain(&bank,1.1f,&gain)&&
        !notification_sound_asset_gain(&bank,nan,&gain)&&!notification_sound_asset_gain(&bank,.5f,nullptr)&&gain==9,
        "sound assets: invalid percentages and nominal gains cannot change volume or output");
    InterlockedExchange(&bank.output_percent,-1);
    bool invalid_percent=!notification_sound_asset_gain(&bank,.5f,&gain)&&gain==9;
    InterlockedExchange(&bank.output_percent,201);
    invalid_percent=invalid_percent&&!notification_sound_asset_gain(&bank,.5f,&gain)&&gain==9;
    check(invalid_percent,"sound assets: corrupt sampled volume fails without publishing gain");
    notification_sound_assets_volume(&bank,100);
    const void* addresses[kNotificationSoundAssetCount]{};
    for(u32 i=0;i<kNotificationSoundAssetCount;++i)addresses[i]=bank.pcm[i];
    notification_sound_assets_release(&bank);
    bool all_freed=true;
    for(u32 i=0;i<kNotificationSoundAssetCount;++i)all_freed=all_freed&&!bank.pcm[i]&&!bank.frames[i]&&!bank.rates[i]&&freed(addresses[i]);
    check(first_ok&&third_ok&&all_freed&&!bank.failed_mask,
        "sound assets: isolated owner release frees both original PCM allocations after no worker lifetime exists");
    notification_sound_assets_release(&bank);notification_sound_assets_release(nullptr);
    check(!bank.pcm[0]&&!bank.pcm[1],"sound assets: empty and null release are idempotent");
}
}

namespace maicord {
namespace notification_sound_assets_entry {u32 failures;
void check(bool passed,const char* message) noexcept {
    if(passed)return;++failures;u32 n=0;while(message[n])++n;DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),message,n,&written,nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),"\r\n",2,&written,nullptr);
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_notification_sound_assets_tests_entry() noexcept {
    maicord::test_notification_sound_assets(maicord::notification_sound_assets_entry::check);ExitProcess(maicord::notification_sound_assets_entry::failures?1:0);
}
