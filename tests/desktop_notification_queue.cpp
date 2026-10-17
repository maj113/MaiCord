#include "maicord/model/desktop_notification_queue.hpp"

namespace maicord {
namespace {
using Error=DesktopNotificationQueueError;
using Outcome=DesktopNotificationQueueOutcome;
struct Fixture {
    DesktopNotificationQueue q{};
    DesktopNotificationQueueRow rows[4]{};
    char bytes[128]{}, scratch[64]{};
    bool initialize(u32 row_count=4,u32 byte_count=128) noexcept {
        const bool ok=desktop_notification_queue_initialize(&q,{rows,row_count},{bytes,byte_count},{scratch,64});
        if(ok)desktop_notification_queue_reset(&q,11,1);return ok;
    }
};
DesktopNotificationQueueInput item(u64 message,u64 channel=22,Utf8View body=utf8_literal("body")) noexcept {
    return {11,1,channel,message,message,utf8_literal("author"),body,{},true,false};
}
bool zero(const char* data,u32 n) noexcept {for(u32 i=0;i<n;++i)if(data[i])return false;return true;}
}
void test_desktop_notification_queue(void(*check)(bool,const char*) noexcept) noexcept {
    Fixture f{};check(f.initialize(),"display queue initializes only caller-owned disjoint bounded storage");
    check(!desktop_notification_queue_initialize(&f.q,{f.rows,4},{f.scratch,32},{f.scratch,64}),
        "display queue rejects overlapping packed and dispatch buffers without destroying existing owner");
    check(f.q.profile_id==11 && f.q.generation==1 && desktop_notification_queue_can_admit(&f.q,64) &&
        !desktop_notification_queue_can_admit(&f.q,65),"display producer reservation includes independent dispatch capacity");
    char title[]="name", body[]="\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF", avatar[]="file:///D:/a.png";
    auto copied=item(1);copied.title={title,4};copied.body={body,8};copied.avatar_file_uri={avatar,16};copied.policy_channel_id=99;
    copied.sound_epoch=0x123456789ABCDEF0ull;copied.sound_id=2;
    const auto first=desktop_notification_queue_enqueue(&f.q,copied);
    copied.policy_channel_id=100;copied.source_sequence=0;copied.channel_id=23;copied.sound_epoch=7;copied.sound_id=1;
    clear_bytes(title,4);clear_bytes(body,8);clear_bytes(avatar,16);
    DesktopNotificationQueueDispatch dispatch{};
    check(first.ok() && first.wake_required && desktop_notification_queue_begin(&f.q,&dispatch).ok() &&
        utf8_equal(dispatch.title,utf8_literal("name")) &&
        utf8_equal(dispatch.body,utf8_literal("\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF")) &&
        utf8_equal(dispatch.avatar_file_uri,utf8_literal("file:///D:/a.png")) && dispatch.identity.policy_channel_id==99 &&
        dispatch.identity.channel_id==22 && dispatch.identity.source_sequence==1 &&
        dispatch.identity.sound_epoch==0x123456789ABCDEF0ull && dispatch.identity.sound_id==2,
        "display queue deep copies borrowed UTF8 and independent activation/policy/sound scalar identities");
    check(desktop_notification_queue_enqueue(&f.q,item(1)).error==Error::duplicate &&
        desktop_notification_queue_begin(&f.q,&dispatch).error==Error::busy,
        "display queue deduplicates in-flight IDs and prevents two simultaneous dispatches");
    const auto second=desktop_notification_queue_enqueue(&f.q,item(2));
    check(second.ok() && !second.wake_required && desktop_notification_queue_can_admit(&f.q,64),
        "display queue accepts waiting FIFO work without modifying stable dispatch scratch");
    const auto retry=desktop_notification_queue_complete(&f.q,first.token,dispatch.attempt,Outcome::retry);
    check(utf8_equal(dispatch.title,utf8_literal("name")) && retry.retry_pending &&
        retry.identity.sound_epoch==0x123456789ABCDEF0ull && retry.identity.sound_id==2 &&
        desktop_notification_queue_complete(&f.q,first.token,dispatch.attempt,Outcome::submitted).error==Error::stale_token,
        "display retry needs another begin and cannot complete the same attempt twice");
    DesktopNotificationQueueRow front{};
    check(desktop_notification_queue_front(&f.q,&front) && front.message_id==1 &&
        front.sound_epoch==0x123456789ABCDEF0ull && front.sound_id==2 &&
        desktop_notification_queue_begin(&f.q,&dispatch).ok() && dispatch.attempt==2 && dispatch.identity.token==first.token &&
        dispatch.identity.sound_epoch==0x123456789ABCDEF0ull && dispatch.identity.sound_id==2 &&
        utf8_equal(dispatch.title,utf8_literal("name")),"display retry retains copied preview and FIFO head across turns");
    check(desktop_notification_queue_complete(&f.q,first.token,1,Outcome::submitted).error==Error::stale_token &&
        desktop_notification_queue_complete(&f.q,first.token,0,Outcome::failed).error==Error::stale_token &&
        f.q.dispatching && dispatch.attempt==2 && dispatch.identity.policy_channel_id==99 &&
        utf8_equal(dispatch.title,utf8_literal("name")),
        "display delayed attempt1 completion cannot retire attempt2 or alter copied policy/scratch");
    const auto done=desktop_notification_queue_complete(&f.q,first.token,dispatch.attempt,Outcome::submitted);
    check(done.error==Error::none && !done.removal_required && zero(f.scratch,28) &&
        done.identity.sound_epoch==0x123456789ABCDEF0ull && done.identity.sound_id==2 &&
        desktop_notification_queue_begin(&f.q,&dispatch).ok() && dispatch.identity.message_id==2,
        "display success retires and zeros scratch then advances to the next FIFO intent");
    check(dispatch.identity.policy_channel_id==0 && !dispatch.identity.sound_epoch && !dispatch.identity.sound_id,
        "display trailing policy and sound scope preserve existing aggregate zero defaults after buffer reuse");
    desktop_notification_queue_complete(&f.q,second.token,dispatch.attempt,Outcome::failed);
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
    desktop_notification_queue_complete(&f.q,a.token,dispatch.attempt,Outcome::submitted);
    check(desktop_notification_queue_can_admit(&f.q,64),"display producer can resume only after a receipt frees its reservation");

    f.initialize(4,16);
    const auto bytes_first=desktop_notification_queue_enqueue(&f.q,item(20));
    check(bytes_first.ok() && desktop_notification_queue_enqueue(&f.q,item(21)).error==Error::full_bytes &&
        desktop_notification_queue_enqueue(&f.q,item(20)).error==Error::duplicate && f.q.count==1 && f.q.used==10,
        "display byte exhaustion and queued duplicate admission are atomic");
    constexpr char bad[]={char(-64),char(-81)}, nul[]={'a',0,'b'};
    auto invalid=item(21);invalid.body={bad,2};
    check(desktop_notification_queue_enqueue(&f.q,invalid).error==Error::invalid_input,
        "display queue rejects overlong UTF8 before publication");invalid.body={nul,3};
    check(desktop_notification_queue_enqueue(&f.q,invalid).error==Error::invalid_input,
        "display queue rejects embedded NUL without truncation");invalid.body={nullptr,1};
    check(desktop_notification_queue_enqueue(&f.q,invalid).error==Error::invalid_input,
        "display queue rejects nonempty null views");invalid=item(21);invalid.title={f.bytes,6};
    check(desktop_notification_queue_enqueue(&f.q,invalid).error==Error::invalid_input && f.q.count==1 && f.q.used==10,
        "display enqueue rejects queue-backed source aliases before any compaction or mutation");
    check(!desktop_notification_queue_front(&f.q,f.rows) &&
        desktop_notification_queue_begin(&f.q,reinterpret_cast<DesktopNotificationQueueDispatch*>(f.scratch)).error==Error::invalid_input,
        "display output descriptors cannot overwrite queue headers or borrowed dispatch storage");

    f.initialize();
    desktop_notification_queue_enqueue(&f.q,item(30,22,utf8_literal("one")));
    desktop_notification_queue_enqueue(&f.q,item(31,23,utf8_literal("middle")));
    auto cancel_sound=item(32,22,utf8_literal("three"));cancel_sound.sound_epoch=91;cancel_sound.sound_id=1;
    desktop_notification_queue_enqueue(&f.q,cancel_sound);cancel_sound.sound_epoch=0;cancel_sound.sound_id=0;
    check(desktop_notification_queue_purge_message(&f.q,11,2,23,31)==0 &&
        desktop_notification_queue_purge_message(&f.q,11,1,23,31)==1 && f.q.count==2 && f.q.used==20 &&
        f.rows[1].offset==9 && desktop_notification_queue_begin(&f.q,&dispatch).ok() && dispatch.identity.message_id==30,
        "display message deletion compacts only matching generation and preserves surviving FIFO text");
    desktop_notification_queue_complete(&f.q,dispatch.identity.token,dispatch.attempt,Outcome::submitted);
    desktop_notification_queue_begin(&f.q,&dispatch);
    check(dispatch.identity.message_id==32 && dispatch.identity.sound_epoch==91 && dispatch.identity.sound_id==1 &&
        utf8_equal(dispatch.body,utf8_literal("three")),
        "display packed compaction retains exact multientry byte offsets");
    const u64 active_token=dispatch.identity.token;
    check(desktop_notification_queue_purge_channel(&f.q,11,1,22)==1 &&
        desktop_notification_queue_purge_channel(&f.q,11,1,22)==0 && utf8_equal(dispatch.body,utf8_literal("three")),
        "display ACK during native dispatch marks cancellation once and preserves borrowed text until return");
    const auto canceled=desktop_notification_queue_complete(&f.q,active_token,dispatch.attempt,Outcome::submitted);
    check(canceled.canceled && canceled.removal_required && canceled.identity.message_id==32 &&
        canceled.identity.sound_epoch==91 && canceled.identity.sound_id==1 && zero(f.scratch,64),
        "display submitted after concurrent ACK explicitly requires deferred native removal");

    const auto generation_old=desktop_notification_queue_enqueue(&f.q,item(40));
    desktop_notification_queue_begin(&f.q,&dispatch);
    desktop_notification_queue_reset(&f.q,11,2);
    auto newer=item(41);newer.generation=2;
    check(desktop_notification_queue_enqueue(&f.q,item(42)).error==Error::stale_generation &&
        desktop_notification_queue_enqueue(&f.q,newer).ok() && utf8_equal(dispatch.body,utf8_literal("body")),
        "display same-profile READY invalidates generation while preserving the active OS call lifetime");
    const auto no_retry=desktop_notification_queue_complete(&f.q,generation_old.token,dispatch.attempt,Outcome::retry);
    check(no_retry.canceled && !no_retry.retry_pending && !no_retry.removal_required &&
        desktop_notification_queue_begin(&f.q,&dispatch).ok() && dispatch.identity.message_id==41,
        "display canceled old generation cannot retry or block fresh FIFO work");
    check(desktop_notification_queue_complete(&f.q,generation_old.token,1,Outcome::submitted).error==Error::stale_token &&
        f.q.dispatching && dispatch.identity.generation==2 && utf8_equal(dispatch.body,utf8_literal("body")),
        "display delayed old-generation completion leaves the fresh active receipt and preview unchanged");
    desktop_notification_queue_reset(&f.q,0,0);
    check(!desktop_notification_queue_can_admit(&f.q,0) &&
        desktop_notification_queue_enqueue(&f.q,newer).error==Error::stale_generation,
        "display stopped account admits no zero-byte or stale intents");

    f.initialize();f.q.next_token=~u64{0};
    const auto last=desktop_notification_queue_enqueue(&f.q,item(50));
    check(last.ok() && last.token==~u64{0} && !desktop_notification_queue_can_admit(&f.q,1) &&
        desktop_notification_queue_enqueue(&f.q,item(51)).error==Error::token_exhausted,
        "display receipt counter wrap fails closed without reusing old tokens");
    desktop_notification_queue_reset(&f.q,11,2);newer.message_id=52;
    check(desktop_notification_queue_enqueue(&f.q,newer).error==Error::token_exhausted &&
        desktop_notification_queue_complete(&f.q,last.token,1,Outcome::submitted).error==Error::stale_token,
        "display generation reset never rewinds tokens or accepts an old receipt");

    DesktopNotificationQueue large{};DesktopNotificationQueueRow rows[kDesktopNotificationQueueRows]{};
    char bytes[kDesktopNotificationQueueBytes]{},scratch[kDesktopNotificationQueueItemBytes]{};
    check(desktop_notification_queue_initialize(&large,{rows,32},{bytes,sizeof(bytes)},{scratch,sizeof(scratch)}),
        "display production storage budget initializes below24KiB with no heap allocation");
    desktop_notification_queue_reset(&large,11,1);
    bool fifo=true;for(u32 i=0;i<32;++i)fifo=fifo && desktop_notification_queue_enqueue(&large,item(100+i)).ok();
    fifo=fifo && !desktop_notification_queue_can_admit(&large,1280);
    for(u32 i=0;i<32;++i){const auto begin=desktop_notification_queue_begin(&large,&dispatch);
        fifo=fifo && begin.ok() && dispatch.identity.message_id==100+i && utf8_equal(dispatch.body,utf8_literal("body"));
        const auto end=desktop_notification_queue_complete(&large,begin.token,dispatch.attempt,Outcome::submitted);fifo=fifo && end.error==Error::none;
    }
    const auto metrics=desktop_notification_queue_metrics(&large);
    check(fifo && metrics.admitted==32 && metrics.submitted==32 && metrics.high_rows==32 && metrics.high_bytes==320 &&
        !metrics.outstanding && !metrics.bytes && zero(bytes,sizeof(bytes)) && zero(scratch,sizeof(scratch)),
        "display32-create burst retains all admitted intents and yields one explicit FIFO receipt per begin/complete");
    // Repeated draining crosses many old ring-index boundaries without retaining
    // stale offsets/tokens; this model uses packed compaction rather than a ring.
    u64 prior=32;fifo=true;
    for(u32 i=0;i<80;++i){const auto published=desktop_notification_queue_enqueue(&large,item(200+i));
        const auto begun=desktop_notification_queue_begin(&large,&dispatch);
        fifo=fifo && published.ok() && begun.ok() && begun.token>prior && dispatch.identity.message_id==200+i;
        prior=begun.token;desktop_notification_queue_complete(&large,begun.token,dispatch.attempt,Outcome::failed);
    }
    check(fifo && !desktop_notification_queue_pending(&large),"display repeated reuse preserves receipt order beyond32 slots without ring wrap aliases");
    char maximum_body[kDesktopNotificationQueueItemBytes];for(u32 i=0;i<sizeof(maximum_body);++i)maximum_body[i]='x';
    auto maximal=item(500);maximal.title={};maximal.body={maximum_body,sizeof(maximum_body)};
    check(desktop_notification_queue_can_admit(&large,4096) && desktop_notification_queue_enqueue(&large,maximal).ok(),
        "display queue accepts exact4KiB generic copied item with no truncation");
    maximal.message_id=501;maximal.title=utf8_literal("x");
    check(desktop_notification_queue_enqueue(&large,maximal).error==Error::invalid_input && large.count==1 && large.used==4096,
        "display queue rejects item byte sum over4KiB atomically before scanning or copying");
    desktop_notification_queue_purge_all(&large);
    check(zero(bytes,sizeof(bytes)),"display bulk purge erases copied maximum-size private preview");
}
} // namespace maicord
