// Manual extension: MAICORD_NOTIFICATION_RETENTION_PROFILE=1 prints a retention timing profile.
#include "maicord/model/notification_retention.hpp"

namespace maicord {
namespace {
using Error=NotificationRetentionError;
struct Fixture {
    NotificationRetention owner{};
    NotificationRetentionRow rows[8]{};
    u32 index[16]{};
    bool initialize(u32 capacity=8) noexcept {return notification_retention_initialize(&owner,{rows,capacity},{index,16});}
    bool reinitialize() noexcept {owner={};return initialize();}
};
NotificationRetentionRow large_rows[8192]{};
u32 large_index[16384]{};
volatile u64 observed_profile_checksum{};
bool unchanged(const Fixture& f,const Fixture& before) noexcept {return bytes_equal(&f,&before,sizeof(f));}
bool ticket(const NotificationRetention& n,u64 channel,u32 ordinal,u64 expected) noexcept {
    u64 value=~expected;return notification_retention_ticket(&n,channel,ordinal,&value)&&value==expected;
}
void fifo(void(*check)(bool,const char*)noexcept) noexcept {
    Fixture f{};check(f.initialize(),"retention: caller-owned sparse storage initializes");
    bool ordered=true;
    for(u32 i=0;i<20;++i){const auto added=notification_retention_append(&f.owner,7,i);ordered=ordered&&added.ok()&&!added.evicted;}
    for(u32 i=0;i<20;++i)ordered=ordered&&ticket(f.owner,7,i,i);
    check(ordered&&f.owner.count==1&&notification_retention_room(&f.owner,7).tickets==20,
        "retention: twenty opaque successes including zero stay in exact FIFO order");
    auto added=notification_retention_append(&f.owner,7,20);
    check(added.ok()&&added.evicted&&added.evicted_ticket==0&&ticket(f.owner,7,0,1)&&ticket(f.owner,7,19,20),
        "retention: twenty-first append explicitly evicts opaque zero, not an empty sentinel");
    ordered=true;
    for(u32 i=21;i<65;++i){added=notification_retention_append(&f.owner,7,i);ordered=ordered&&added.ok()&&added.evicted&&added.evicted_ticket==i-20;}
    for(u32 i=0;i<20;++i)ordered=ordered&&ticket(f.owner,7,i,45+i);
    check(ordered,"retention: repeated FIFO ring wrap returns every oldest success exactly once");
    check(notification_retention_discard_channel(&f.owner,7)==Error::none&&f.owner.count==0&&
        !notification_retention_room(&f.owner,7).exists,"retention: channel discard deletes metadata");
    for(u32 i=0;i<21;++i)added=notification_retention_append(&f.owner,9,123);
    check(added.ok()&&added.evicted&&added.evicted_ticket==123&&notification_retention_room(&f.owner,9).tickets==20,
        "retention: duplicate tickets occupy independent successful records and are not deduplicated");
    check(notification_retention_discard_channel(&f.owner,9)==Error::none&&f.owner.count==0&&
        notification_retention_discard_channel(&f.owner,99)==Error::none,"retention: discard and absent clear retire no unrelated records");
}
void validation(void(*check)(bool,const char*)noexcept) noexcept {
    Fixture f{},before{};
    check(!notification_retention_initialize(&f.owner,{f.rows,8},{f.index,8})&&!f.owner.rows.data,
        "retention: index must provide at least twice caller row capacity");
    check(!notification_retention_initialize(&f.owner,{f.rows,8},{f.index,15})&&f.initialize(),
        "retention: non-power-of-two index rejected before valid initialization");
    notification_retention_append(&f.owner,1,11);notification_retention_append(&f.owner,2,22);
    copy_bytes(&before,&f,sizeof(f));
    u64 output[20]{};output[0]=71;
    check(notification_retention_append(&f.owner,0,8).error==Error::invalid_input&&
        notification_retention_discard_channel(&f.owner,0)==Error::invalid_input&&unchanged(f,before),
        "retention: invalid operations are atomic and preserve existing successes");
    check(!notification_retention_ticket(&f.owner,1,1,output)&&output[0]==71&&
        !notification_retention_ticket(&f.owner,1,0,&f.rows[0].tickets[0])&&
        !notification_retention_ticket(&f.owner,1,0,reinterpret_cast<u64*>(&f.owner))&&unchanged(f,before),
        "retention: ticket outputs cannot alias owner or any row storage");
    check(notification_retention_rebind(&f.owner,{f.rows+1,4},{f.index,16})==Error::invalid_input&&
        notification_retention_rebind(&f.owner,{f.rows,1},{f.index,16})==Error::invalid_input&&unchanged(f,before),
        "retention: partial storage overlap and shrinking below active count are atomic");
    Fixture destination{},saved_destination{};copy_bytes(&saved_destination,&destination,sizeof(destination));
    check(notification_retention_rebind(&f.owner,{destination.rows,8},{reinterpret_cast<u32*>(f.rows),16})==Error::invalid_input&&
        unchanged(f,before)&&unchanged(destination,saved_destination),
        "retention: cross-bank rebind overlap leaves both source and destination untouched");
    u32 occupied{};while(occupied<16&&!f.index[occupied])++occupied;
    if(occupied==16){check(false,"retention: malformed-state fixture requires an occupied index slot");return;}
    const u32 old=f.index[occupied];const u64 channel=f.rows[old-1].channel;f.index[occupied]=99;
    copy_bytes(&before,&f,sizeof(f));
    check(notification_retention_append(&f.owner,channel,44).error==Error::invalid_input&&
        notification_retention_rebind(&f.owner,{destination.rows,8},{destination.index,16})==Error::invalid_input&&
        unchanged(f,before)&&unchanged(destination,saved_destination),
        "retention: malformed probed index and inconsistent rebind reject without mutation");
    f.index[occupied]=old;
    NotificationRetention empty{};
    check(notification_retention_initialize(&empty,{},{})&&notification_retention_room(&empty,1).error==Error::full_channels&&
        notification_retention_append(&empty,1,2).error==Error::full_channels,
        "retention: zero initial storage is valid without hidden allocations");
    Fixture alias{};
    check(!notification_retention_initialize(&alias.owner,{reinterpret_cast<NotificationRetentionRow*>(&alias.owner),1},{alias.index,16})&&
        !notification_retention_initialize(&alias.owner,{alias.rows,1},{reinterpret_cast<u32*>(alias.rows),16}),
        "retention: initialization rejects owner and row/index storage aliases");
}
void mapping(void(*check)(bool,const char*)noexcept) noexcept {
    Fixture f{};check(f.initialize(),"retention: collision fixture initializes");
    // Discover colliders through the actual initialized index, without copying
    // the hash formula into an oracle. Last bucket forces wrap through zero.
    u64 colliders[5]{};u32 found{};
    for(u64 candidate=1;candidate<=4096&&found<5;++candidate){
        notification_retention_append(&f.owner,candidate,candidate);
        if(f.index[15])colliders[found++]=candidate;
        f.reinitialize();
    }
    check(found==5,"retention: bounded corpus finds a wraparound collision cluster");
    if(found!=5)return;
    for(u32 i=0;i<5;++i)notification_retention_append(&f.owner,colliders[i],100+i);
    bool correct=f.index[15]&&f.index[0]&&f.index[1]&&f.index[2]&&f.index[3];
    correct=correct&&notification_retention_discard_channel(&f.owner,colliders[0])==Error::none;
    correct=correct&&ticket(f.owner,colliders[4],0,104)&&ticket(f.owner,colliders[1],0,101);
    correct=correct&&notification_retention_discard_channel(&f.owner,colliders[2])==Error::none;
    correct=correct&&ticket(f.owner,colliders[1],0,101)&&ticket(f.owner,colliders[3],0,103)&&ticket(f.owner,colliders[4],0,104);
    correct=correct&&notification_retention_discard_channel(&f.owner,colliders[4])==Error::none;
    correct=correct&&ticket(f.owner,colliders[1],0,101)&&ticket(f.owner,colliders[3],0,103)&&f.owner.count==2;
    check(correct,"retention: wrapped cluster head/middle/tail deletion repairs probes and dense swap-last indexes");
    f.reinitialize();
    correct=true;
    for(u32 i=0;i<8;++i)correct=correct&&notification_retention_append(&f.owner,u64(i+1)<<40,i+30).ok();
    Fixture before{};copy_bytes(&before,&f,sizeof(f));
    check(correct&&notification_retention_room(&f.owner,99).error==Error::full_channels&&
        notification_retention_append(&f.owner,99,999).error==Error::full_channels&&unchanged(f,before),
        "retention: full caller channel storage never evicts an unrelated channel");
    check(notification_retention_append(&f.owner,u64{1}<<40,777).ok()&&ticket(f.owner,u64{1}<<40,1,777),
        "retention: existing channel can append at full channel capacity");
    f.reinitialize();
    correct=true;
    for(u32 i=0;i<10000;++i){const u64 id=(u64(i+1)<<22)|5;
        correct=correct&&notification_retention_append(&f.owner,id,i).ok()&&ticket(f.owner,id,0,i)&&
            notification_retention_discard_channel(&f.owner,id)==Error::none&&f.owner.count==0;
    }
    check(correct,"retention: over8192 same-low22-bit historical channels churn without tombstone exhaustion");
    notification_retention_append(&f.owner,~u64{0},812);notification_retention_append(&f.owner,u64{1}<<63,813);
    check(ticket(f.owner,~u64{0},0,812)&&ticket(f.owner,u64{1}<<63,0,813),
        "retention: full-width channel IDs retain high-bit and maximum values independently");
}
void growth(void(*check)(bool,const char*)noexcept) noexcept {
    Fixture f{},destination{};check(f.initialize(2),"retention: small growth fixture initializes");
    for(u32 i=0;i<25;++i)notification_retention_append(&f.owner,41,i);
    notification_retention_append(&f.owner,42,77);
    check(notification_retention_rebind(&f.owner,{destination.rows,8},{destination.index,16})==Error::none&&
        f.owner.count==2&&ticket(f.owner,41,0,5)&&ticket(f.owner,41,19,24)&&ticket(f.owner,42,0,77),
        "retention: disjoint replacement banks preserve live wrapped FIFO rows and rebuild indexes");
    check(notification_retention_rebind(&f.owner,{destination.rows,2},{destination.index,4})==Error::none&&
        notification_retention_rebind(&f.owner,{destination.rows,8},{destination.index,16})==Error::none&&ticket(f.owner,41,19,24),
        "retention: same-base shrink and geometric growth preserve metadata");
    NotificationRetention large{};check(notification_retention_initialize(&large,{large_rows,4096},{large_index,8192}),
        "retention: large caller storage initializes without a model source cap");
    bool correct=true;
    for(u32 i=0;i<4096;++i)correct=correct&&notification_retention_append(&large,(u64(i+1)<<22)|17,i).ok();
    check(correct&&large.count==4096&&notification_retention_room(&large,(u64{4097}<<22)|17).error==Error::full_channels,
        "retention: same-low22-bit4096-channel dataset fills only caller capacity");
    check(notification_retention_rebind(&large,{large_rows,8192},{large_index,16384})==Error::none&&
        notification_retention_append(&large,(u64{4097}<<22)|17,4096).ok()&&large.count==4097&&
        ticket(large,(u64{1}<<22)|17,0,0)&&ticket(large,(u64{4097}<<22)|17,0,4096),
        "retention: caller growth beyond4096 historical channels preserves first and last success");
}
struct Line {
    char text[256]{};u32 count{};
    void literal(const char* s) noexcept {for(u32 i=0;s[i]&&count<sizeof(text);++i)text[count++]=s[i];}
    void number(u64 n) noexcept {char digits[20];u32 used{};do{digits[used++]=static_cast<char>('0'+n%10);n/=10;}while(n);while(used&&count<sizeof(text))text[count++]=digits[--used];}
    void write() noexcept {literal("\r\n");DWORD bytes{};WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,count,&bytes,nullptr);}
};
u64 linear_lookup(const NotificationRetention& n,u64 channel,u32 ordinal) noexcept {
    for(u32 i=0;i<n.count;++i)if(n.rows[i].channel==channel){const auto& r=n.rows[i];
        const u32 position=(u32(r.head)+ordinal)%20;return u64(r.count)+r.tickets[position];}
    return 0;
}
void profile(void(*check)(bool,const char*)noexcept) noexcept {
    wchar_t setting[2]{};
    if(GetEnvironmentVariableW(L"MAICORD_NOTIFICATION_RETENTION_PROFILE",setting,2)!=1||setting[0]!=L'1')return;
    NotificationRetention n{};bool ready=notification_retention_initialize(&n,{large_rows,4096},{large_index,8192});
    for(u32 i=0;i<4096;++i){const u64 channel=(u64(i+1)<<22)|17;
        ready=ready&&notification_retention_append(&n,channel,i).ok()&&notification_retention_append(&n,channel,i+65536).ok();}
    check(ready,"retention profile: deterministic4096-channel same-low22-bit dataset ready");
    if(!ready)return;
    LARGE_INTEGER frequency{};const bool clock=QueryPerformanceFrequency(&frequency)!=0;
    check(clock&&frequency.QuadPart>0,"retention profile: QPC frequency is available");if(!clock||frequency.QuadPart<=0)return;
    Line header{};header.literal("retention_profile channels=4096 operations=10000 samples=7 qpc_frequency=");header.number(static_cast<u64>(frequency.QuadPart));header.write();
    for(u32 sample=0;sample<7;++sample){u64 checksums[2]{};
        // Alternate the first method to reduce fixed order/thermal bias.
        for(u32 pass=0;pass<2;++pass){const u32 method=(pass+sample)&1;
            LARGE_INTEGER begin{},end{};ULONG64 cycles_begin{},cycles_end{};
            const bool cycles_a=QueryThreadCycleTime(GetCurrentThread(),&cycles_begin)!=0;
            QueryPerformanceCounter(&begin);u64 sum{};
            for(u32 i=0;i<10000;++i){const u32 row=(i*4051+sample*17)&4095,ordinal=(i+sample)&1;
                const u64 channel=(u64(row+1)<<22)|17;
                if(method){sum+=linear_lookup(n,channel,ordinal);}
                else {const auto room=notification_retention_room(&n,channel);u64 value{};
                    if(room.ok()&&room.exists&&notification_retention_ticket(&n,channel,ordinal,&value))sum+=room.tickets+value;}
            }
            QueryPerformanceCounter(&end);
            const bool cycles_b=QueryThreadCycleTime(GetCurrentThread(),&cycles_end)!=0;
            observed_profile_checksum=sum;checksums[method]=sum;
            Line line{};line.literal("retention_profile sample=");line.number(sample);line.literal(method?" method=linear":" method=indexed");
            line.literal(" operations=10000 qpc_ticks=");line.number(static_cast<u64>(end.QuadPart-begin.QuadPart));
            line.literal(" cycles_valid=");line.number(cycles_a&&cycles_b);line.literal(" thread_cycles=");line.number(cycles_a&&cycles_b?cycles_end-cycles_begin:0);
            line.literal(" checksum=");line.number(sum);line.write();
        }
        check(checksums[0]==checksums[1],"retention profile: indexed and linear lookup checksums match");
    }
}
}
void test_notification_retention(void(*check)(bool,const char*)noexcept) noexcept {
    check(sizeof(NotificationRetentionRow)==176,"retention: twenty-ticket sparse row has exact176-byte layout");
    fifo(check);validation(check);mapping(check);growth(check);profile(check);
}
} // namespace maicord

namespace maicord {
namespace notification_retention_entry {
u32 failures;
void check(bool passed,const char* message) noexcept {
    if(passed)return;++failures;u32 count{};while(message[count])++count;DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),message,count,&written,nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),"\r\n",2,&written,nullptr);
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_notification_retention_tests_entry() noexcept {
    maicord::test_notification_retention(maicord::notification_retention_entry::check);ExitProcess(maicord::notification_retention_entry::failures?1:0);
}
