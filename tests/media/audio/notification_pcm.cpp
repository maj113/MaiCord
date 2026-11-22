#include "maicord/media/audio/notification_pcm.hpp"
#include "../../fixtures/notification_pcm_fixture.hpp"
#include "../../support/test_output.hpp"

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
        exact=exact && notification_pcm_unpack(cases[i],{frames,static_cast<u8>(i/3),static_cast<u8>(i%3)},
            {pcm,sizeof(expected)},&workspace) && bytes_equal(pcm,expected,sizeof(expected));
        for(u32 at=sizeof(expected);at<sizeof(pcm);++at)guard=guard && pcm[at]==0x5a;
    }
    check(exact && guard,"PCM: independent frozen9 predictor vectors restore exact stereo/extremes/wrap/cross-block state without tail writes");
    const auto packed=cases[8];
    const auto spec=NotificationPcmSpec{frames,2,2};
    u8 damaged[sizeof(packed_2_2)+1];copy_bytes(damaged,packed.data,packed.count);damaged[packed.count-1]^=1;
    check(!notification_pcm_unpack({damaged,packed.count},spec,{pcm,sizeof(expected)},&workspace),
        "PCM: corrupted filtered stream is not published as canonical samples");
    check(!notification_pcm_unpack({packed.data,packed.count-1},spec,{pcm,sizeof(expected)},&workspace),
        "PCM: truncated filtered stream fails closed");
    copy_bytes(damaged,packed.data,packed.count);damaged[packed.count]=0;
    check(!notification_pcm_unpack({damaged,packed.count+1},spec,{pcm,sizeof(expected)},&workspace),
        "PCM: appended data is rejected by exact sealed stream admission");
}
}

namespace maicord {
namespace notification_pcm_entry {u32 failures;
void check(bool passed,const char* message) noexcept { test_check(&failures,passed,message); }
}}
MAICORD_TEST_ENTRY(maicord_notification_pcm_tests_entry) {
    maicord::test_notification_pcm(maicord::notification_pcm_entry::check);::maicord::test_process_exit(maicord::notification_pcm_entry::failures?1:0);
}
