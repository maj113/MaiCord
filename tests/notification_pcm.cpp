#include "maicord/media/notification_pcm.hpp"
#include "fixtures/notification_pcm_fixture.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
void test_notification_pcm(void(*check)(bool,const char*)noexcept) noexcept {
    using namespace notification_pcm_fixture;
    u8 pcm[sizeof(expected)+16];DeflateWorkspace workspace{};
    const ConstSlice<u8> cases[]{
        {packed_0_0,sizeof(packed_0_0)},{packed_0_1,sizeof(packed_0_1)},{packed_0_2,sizeof(packed_0_2)},
        {packed_1_0,sizeof(packed_1_0)},{packed_1_1,sizeof(packed_1_1)},{packed_1_2,sizeof(packed_1_2)},
        {packed_2_0,sizeof(packed_2_0)},{packed_2_1,sizeof(packed_2_1)},{packed_2_2,sizeof(packed_2_2)}};
    bool exact=true,guard=true;
    for(u32 i=0;i<9;++i) {
        for(auto& value:pcm)value=0x5a;
        const auto result=notification_pcm_unpack(cases[i],{frames,static_cast<u8>(i/3),static_cast<u8>(i%3)},
            {pcm,sizeof(expected)},&workspace);
        exact=exact && result.ok() && bytes_equal(pcm,expected,sizeof(expected));
        for(u32 at=sizeof(expected);at<sizeof(pcm);++at)guard=guard && pcm[at]==0x5a;
    }
    check(exact && guard,"PCM: independent frozen9 predictor vectors restore exact stereo/extremes/wrap/cross-block state without tail writes");
    const auto packed=cases[8];
    const auto spec=NotificationPcmSpec{frames,2,2};
    for(auto& value:pcm)value=0x5a;
    const auto invalid=notification_pcm_unpack(packed,{frames,3,2},{pcm,sizeof(expected)},&workspace);
    bool untouched=true;for(auto value:pcm)untouched=untouched && value==0x5a;
    check(invalid.error==NotificationPcmError::invalid_input && untouched,
        "PCM: unsupported predictor rejects atomically before touching private bank");
    check(notification_pcm_unpack(packed,{0,2,2},{pcm,sizeof(expected)},&workspace).error==NotificationPcmError::invalid_input &&
        notification_pcm_unpack(packed,{~u32{0},2,2},{pcm,sizeof(expected)},&workspace).error==NotificationPcmError::invalid_input &&
        notification_pcm_unpack(packed,spec,{pcm,sizeof(expected)-1},&workspace).error==NotificationPcmError::invalid_input &&
        notification_pcm_unpack(packed,spec,{pcm,sizeof(expected)},nullptr).error==NotificationPcmError::invalid_input,
        "PCM: frame/size/owner bounds reject before inflate");
    check(notification_pcm_unpack({pcm,12},spec,{pcm,sizeof(expected)},&workspace).error==NotificationPcmError::invalid_input &&
        notification_pcm_unpack(packed,spec,{reinterpret_cast<u8*>(&workspace),sizeof(expected)},&workspace).error==NotificationPcmError::invalid_input,
        "PCM: source/output/workspace aliases reject before writes");
    u8 damaged[sizeof(packed_2_2)+1];copy_bytes(damaged,packed.data,packed.count);damaged[packed.count-1]^=1;
    check(notification_pcm_unpack({damaged,packed.count},spec,{pcm,sizeof(expected)},&workspace).error==NotificationPcmError::inflate,
        "PCM: corrupted filtered stream is not published as canonical samples");
    check(notification_pcm_unpack({packed.data,packed.count-1},spec,{pcm,sizeof(expected)},&workspace).error==NotificationPcmError::inflate,
        "PCM: truncated filtered stream fails closed");
    copy_bytes(damaged,packed.data,packed.count);damaged[packed.count]=0;
    const auto trailing=notification_pcm_unpack({damaged,packed.count+1},spec,{pcm,sizeof(expected)},&workspace);
    check(trailing.error==NotificationPcmError::inflate && trailing.inflate_error==ZlibInflateError::trailing_data,
        "PCM: appended data is rejected by exact sealed stream admission");
}
}

namespace maicord {
namespace notification_pcm_entry {u32 failures;
void check(bool passed,const char* message) noexcept {
    if(passed)return;++failures;u32 n=0;while(message[n])++n;DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),message,n,&written,nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),"\r\n",2,&written,nullptr);
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_notification_pcm_tests_entry() noexcept {
    maicord::test_notification_pcm(maicord::notification_pcm_entry::check);ExitProcess(maicord::notification_pcm_entry::failures?1:0);
}
