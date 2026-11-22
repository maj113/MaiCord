#include "maicord/model/notifications/desktop_notification_queue.hpp"

namespace maicord {
namespace {
using Error=DesktopNotificationQueueError;
using Outcome=DesktopNotificationQueueOutcome;
struct Fixture {
    DesktopNotificationQueue q{};
    DesktopNotificationQueueRow rows[4]{};
    char bytes[128]{}, scratch[64]{};
    // The queue takes zeroed storage, as the client's fresh pages are.
    void initialize(u32 row_count=4,u32 byte_count=128) noexcept {
        *this=Fixture{};
        desktop_notification_queue_initialize(&q,{rows,row_count},{bytes,byte_count},{scratch,64});
        desktop_notification_queue_reset(&q,11,1);
    }
};
DesktopNotificationQueueInput item(u64 message,u64 channel=22,Utf8View body=utf8_literal("body")) noexcept {
    return {11,1,channel,message,utf8_literal("author"),body,{},true};
}
bool zero(const char* data,u32 n) noexcept {for(u32 i=0;i<n;++i)if(data[i])return false;return true;}
}
void test_desktop_notification_queue(void(*check)(bool,const char*) noexcept) noexcept {
    Fixture f{};f.initialize();
    check(f.q.profile_id==11 && f.q.generation==1 && desktop_notification_queue_can_admit(&f.q,64) &&
        !desktop_notification_queue_can_admit(&f.q,65),"display producer reservation includes independent dispatch capacity");
    char title[]="name", body[]="\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF", avatar[]="file:///D:/a.png";
    auto copied=item(1);copied.title={title,4};copied.body={body,8};copied.avatar_file_uri={avatar,16};copied.policy_channel_id=99;
    copied.sound_epoch=0x123456789ABCDEF0ull;copied.sound_id=2;
    const auto first=desktop_notification_queue_enqueue(&f.q,copied);
    copied.policy_channel_id=100;copied.channel_id=23;copied.sound_epoch=7;copied.sound_id=1;
    clear_bytes(title,4);clear_bytes(body,8);clear_bytes(avatar,16);
    DesktopNotificationQueueDispatch dispatch{};
    check(first.ok() && desktop_notification_queue_begin(&f.q,&dispatch).ok() &&
        utf8_equal(dispatch.title,utf8_literal("name")) &&
        utf8_equal(dispatch.body,utf8_literal("\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF")) &&
        utf8_equal(dispatch.avatar_file_uri,utf8_literal("file:///D:/a.png")) && dispatch.identity.policy_channel_id==99 &&
        dispatch.identity.channel_id==22 &&
        dispatch.identity.sound_epoch==0x123456789ABCDEF0ull && dispatch.identity.sound_id==2,
        "display queue deep copies borrowed UTF8 and independent activation/policy/sound scalar identities");
    check(desktop_notification_queue_enqueue(&f.q,item(1)).error==Error::duplicate &&
        desktop_notification_queue_begin(&f.q,&dispatch).error==Error::busy,
        "display queue deduplicates in-flight IDs and prevents two simultaneous dispatches");
    const auto second=desktop_notification_queue_enqueue(&f.q,item(2));
    check(second.ok() && desktop_notification_queue_can_admit(&f.q,64),
        "display queue accepts waiting FIFO work without modifying stable dispatch scratch");
    const auto retry=desktop_notification_queue_complete(&f.q,Outcome::retry);
    check(utf8_equal(dispatch.title,utf8_literal("name")) && !retry.canceled && !retry.removal_required &&
        desktop_notification_queue_pending(&f.q),"display retry keeps the copied intent for another begin");
    DesktopNotificationQueueRow front{};
    check(desktop_notification_queue_front(&f.q,&front) && front.message_id==1 &&
        front.sound_epoch==0x123456789ABCDEF0ull && front.sound_id==2 &&
        desktop_notification_queue_begin(&f.q,&dispatch).ok() && dispatch.attempt==2 &&
        dispatch.identity.sound_epoch==0x123456789ABCDEF0ull && dispatch.identity.sound_id==2 &&
        utf8_equal(dispatch.title,utf8_literal("name")),"display retry retains copied preview and FIFO head across turns");
    const auto done=desktop_notification_queue_complete(&f.q,Outcome::submitted);
    check(!done.canceled && !done.removal_required && zero(f.scratch,28) &&
        desktop_notification_queue_begin(&f.q,&dispatch).ok() && dispatch.identity.message_id==2,
        "display success retires and zeros scratch then advances to the next FIFO intent");
    check(dispatch.identity.policy_channel_id==0 && !dispatch.identity.sound_epoch && !dispatch.identity.sound_id,
        "display trailing policy and sound scope preserve existing aggregate zero defaults after buffer reuse");
    desktop_notification_queue_complete(&f.q,Outcome::failed);
    check(!desktop_notification_queue_pending(&f.q) && zero(f.bytes,128) && zero(f.scratch,64),
        "display terminal failure leaves no borrowed private text in either buffer");

    f.initialize(2,128);
    const auto a=desktop_notification_queue_enqueue(&f.q,item(10));
    const auto b=desktop_notification_queue_enqueue(&f.q,item(11));
    const auto full=desktop_notification_queue_enqueue(&f.q,item(12));
    check(a.ok() && b.ok() && full.error==Error::full_rows && !desktop_notification_queue_can_admit(&f.q,1) &&
        desktop_notification_queue_front(&f.q,&front) && front.message_id==10,
        "display row exhaustion is explicit and preserves FIFO instead of dropping or replacing");
    desktop_notification_queue_begin(&f.q,&dispatch);
    check(!desktop_notification_queue_can_admit(&f.q,1) &&
        desktop_notification_queue_enqueue(&f.q,item(12)).error==Error::full_rows,
        "display in-flight identity still consumes a header reservation");
    desktop_notification_queue_complete(&f.q,Outcome::submitted);
    check(desktop_notification_queue_can_admit(&f.q,64),"display producer can resume only after a receipt frees its reservation");

    f.initialize(4,16);
    const auto bytes_first=desktop_notification_queue_enqueue(&f.q,item(20));
    check(bytes_first.ok() && desktop_notification_queue_enqueue(&f.q,item(21)).error==Error::full_bytes &&
        desktop_notification_queue_enqueue(&f.q,item(20)).error==Error::duplicate && f.q.count==1,
        "display byte exhaustion and queued duplicate admission are atomic");
    constexpr char bad[]={char(-64),char(-81)}, nul[]={'a',0,'b'};
    auto invalid=item(21);invalid.body={bad,2};
    check(desktop_notification_queue_enqueue(&f.q,invalid).error==Error::invalid_input,
        "display queue rejects overlong UTF8 before publication");invalid.body={nul,3};
    check(desktop_notification_queue_enqueue(&f.q,invalid).error==Error::invalid_input,
        "display queue rejects embedded NUL without truncation");

    f.initialize();
    desktop_notification_queue_enqueue(&f.q,item(30,22,utf8_literal("one")));
    desktop_notification_queue_enqueue(&f.q,item(31,23,utf8_literal("middle")));
    auto cancel_sound=item(32,22,utf8_literal("three"));cancel_sound.sound_epoch=91;cancel_sound.sound_id=1;
    desktop_notification_queue_enqueue(&f.q,cancel_sound);
    desktop_notification_queue_purge_message(&f.q,11,2,23,31);
    const bool other_generation_kept=f.q.count==3;
    desktop_notification_queue_purge_message(&f.q,11,1,23,31);
    check(other_generation_kept && f.q.count==2 &&
        desktop_notification_queue_begin(&f.q,&dispatch).ok() && dispatch.identity.message_id==30,
        "display message deletion compacts only matching generation and preserves surviving FIFO text");
    desktop_notification_queue_complete(&f.q,Outcome::submitted);
    desktop_notification_queue_begin(&f.q,&dispatch);
    check(dispatch.identity.message_id==32 && dispatch.identity.sound_epoch==91 && dispatch.identity.sound_id==1 &&
        utf8_equal(dispatch.body,utf8_literal("three")),
        "display packed compaction retains exact multientry byte offsets");
    desktop_notification_queue_purge_channel(&f.q,11,1,22);
    check(utf8_equal(dispatch.body,utf8_literal("three")),
        "display ACK during native dispatch preserves borrowed text until return");
    const auto canceled=desktop_notification_queue_complete(&f.q,Outcome::submitted);
    check(canceled.canceled && canceled.removal_required && zero(f.scratch,64),
        "display submitted after concurrent ACK explicitly requires deferred native removal");

    desktop_notification_queue_enqueue(&f.q,item(40));
    desktop_notification_queue_begin(&f.q,&dispatch);
    desktop_notification_queue_reset(&f.q,11,2);
    auto newer=item(41);newer.generation=2;
    check(desktop_notification_queue_enqueue(&f.q,item(42)).error==Error::stale_generation &&
        desktop_notification_queue_enqueue(&f.q,newer).ok() && utf8_equal(dispatch.body,utf8_literal("body")),
        "display same-profile READY invalidates generation while preserving the active OS call lifetime");
    const auto no_retry=desktop_notification_queue_complete(&f.q,Outcome::retry);
    check(no_retry.canceled && !no_retry.removal_required &&
        desktop_notification_queue_begin(&f.q,&dispatch).ok() && dispatch.identity.message_id==41,
        "display canceled old generation cannot retry or block fresh FIFO work");
    desktop_notification_queue_reset(&f.q,0,0);
    check(!desktop_notification_queue_can_admit(&f.q,0) &&
        desktop_notification_queue_enqueue(&f.q,newer).error==Error::stale_generation,
        "display stopped account admits no zero-byte or stale intents");

    DesktopNotificationQueue large{};DesktopNotificationQueueRow rows[kDesktopNotificationQueueRows]{};
    char bytes[kDesktopNotificationQueueBytes]{},scratch[kDesktopNotificationQueueItemBytes]{};
    desktop_notification_queue_initialize(&large,{rows,32},{bytes,sizeof(bytes)},{scratch,sizeof(scratch)});
    desktop_notification_queue_reset(&large,11,1);
    bool fifo=true;for(u32 i=0;i<32;++i)fifo=fifo && desktop_notification_queue_enqueue(&large,item(100+i)).ok();
    fifo=fifo && !desktop_notification_queue_can_admit(&large,1280);
    for(u32 i=0;i<32;++i){const auto begin=desktop_notification_queue_begin(&large,&dispatch);
        fifo=fifo && begin.ok() && dispatch.identity.message_id==100+i && utf8_equal(dispatch.body,utf8_literal("body"));
        desktop_notification_queue_complete(&large,Outcome::submitted);
    }
    check(fifo && !desktop_notification_queue_pending(&large) && zero(bytes,sizeof(bytes)) && zero(scratch,sizeof(scratch)),
        "display32-create burst retains all admitted intents and yields one explicit FIFO receipt per begin/complete");
    // Repeated draining crosses many old ring-index boundaries without retaining
    // stale offsets; this model uses packed compaction rather than a ring.
    fifo=true;
    for(u32 i=0;i<80;++i){const auto published=desktop_notification_queue_enqueue(&large,item(200+i));
        const auto begun=desktop_notification_queue_begin(&large,&dispatch);
        fifo=fifo && published.ok() && begun.ok() && dispatch.identity.message_id==200+i;
        desktop_notification_queue_complete(&large,Outcome::failed);
    }
    check(fifo && !desktop_notification_queue_pending(&large),"display repeated reuse preserves receipt order beyond32 slots without ring wrap aliases");
    char maximum_body[kDesktopNotificationQueueItemBytes];for(u32 i=0;i<sizeof(maximum_body);++i)maximum_body[i]='x';
    auto maximal=item(500);maximal.title={};maximal.body={maximum_body,sizeof(maximum_body)};
    check(desktop_notification_queue_can_admit(&large,4096) && desktop_notification_queue_enqueue(&large,maximal).ok(),
        "display queue accepts exact4KiB generic copied item with no truncation");
    maximal.message_id=501;maximal.title=utf8_literal("x");
    check(desktop_notification_queue_enqueue(&large,maximal).error==Error::invalid_input && large.count==1,
        "display queue rejects item byte sum over4KiB atomically before scanning or copying");
    desktop_notification_queue_purge_all(&large);
    check(zero(bytes,sizeof(bytes)),"display bulk purge erases copied maximum-size private preview");
}
} // namespace maicord
