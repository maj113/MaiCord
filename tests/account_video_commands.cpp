#include "maicord/app/account_video_commands.hpp"

namespace maicord {
namespace {
AccountVideoControl command(AccountVideoCommand kind,u64 serial) noexcept {
    AccountVideoControl result{};
    result.identity={11,22,33,44,55,66,0};
    result.command_serial=serial;
    result.command=kind;
    result.seek_time_100ns=static_cast<i64>(serial*10000000);
    result.volume=static_cast<float>(serial%10)/10;
    result.muted=(serial%2)!=0;
    return result;
}
bool equal(const AccountVideoControl& a,const AccountVideoControl& b) noexcept {
    return a.identity.account_serial==b.identity.account_serial&&a.identity.message_id==b.identity.message_id&&
        a.identity.content_identity==b.identity.content_identity&&a.identity.session_epoch==b.identity.session_epoch&&
        a.identity.message_revision==b.identity.message_revision&&a.identity.cache_generation==b.identity.cache_generation&&
        a.identity.reserved==b.identity.reserved&&a.command_serial==b.command_serial&&
        a.seek_time_100ns==b.seek_time_100ns&&a.volume==b.volume&&a.command==b.command&&a.muted==b.muted;
}
bool equal(const AccountVideoCommandQueue& a,const AccountVideoCommandQueue& b) noexcept {
    if(a.head!=b.head||a.count!=b.count)return false;
    for(u32 i=0;i<8;++i)if(!equal(a.commands[i],b.commands[i]))return false;
    return true;
}
bool take(AccountVideoCommandQueue& queue,const AccountVideoControl& expected) noexcept {
    AccountVideoControl front{};
    return account_video_commands_peek(queue,front)&&equal(front,expected)&&account_video_commands_pop(queue);
}
}

void test_account_video_commands(void(*check)(bool,const char*) noexcept) noexcept {
    AccountVideoCommandQueue queue{};
    AccountVideoControl output=command(AccountVideoCommand::pause,99);
    const auto sentinel=output;
    check(!account_video_commands_peek(queue,output)&&equal(output,sentinel)&&
        !account_video_commands_pop(queue)&&queue.head==0&&queue.count==0,
        "empty video command queue preserves output and metadata");

    const auto play=command(AccountVideoCommand::play,1);
    const auto volume=command(AccountVideoCommand::volume,2);
    const auto mute=command(AccountVideoCommand::mute,3);
    check(account_video_commands_push(queue,play)&&account_video_commands_push(queue,volume)&&
        account_video_commands_push(queue,mute)&&queue.count==3,
        "video Play then Volume and Mute admits all three intents");
    check(account_video_commands_peek(queue,output)&&equal(output,play)&&queue.count==3&&queue.head==0,
        "video command peek copies the complete Play without consuming it");
    check(take(queue,play)&&take(queue,volume)&&take(queue,mute)&&queue.count==0,
        "video audio controls do not erase the preceding Play");

    queue={};
    const auto seek1=command(AccountVideoCommand::seek,4);
    const auto seek2=command(AccountVideoCommand::seek,5);
    const auto pause=command(AccountVideoCommand::pause,6);
    const auto seek3=command(AccountVideoCommand::seek,7);
    const auto seek4=command(AccountVideoCommand::seek,8);
    const auto play2=command(AccountVideoCommand::play,9);
    const auto seek5=command(AccountVideoCommand::seek,10);
    check(account_video_commands_push(queue,seek1)&&account_video_commands_push(queue,seek2)&&queue.count==1&&
        account_video_commands_peek(queue,output)&&output.command_serial==5&&output.seek_time_100ns==50000000,
        "video consecutive Seek replaces pending target and serial with latest drag intent");
    check(account_video_commands_push(queue,pause)&&account_video_commands_push(queue,seek3)&&
        account_video_commands_push(queue,seek4)&&account_video_commands_push(queue,play2)&&
        account_video_commands_push(queue,seek5)&&queue.count==5&&
        take(queue,seek2)&&take(queue,pause)&&take(queue,seek4)&&take(queue,play2)&&take(queue,seek5),
        "video Seek coalescing preserves separate intents across Pause and Play");

    queue={};
    const auto volume2=command(AccountVideoCommand::volume,7);
    const auto mute2=command(AccountVideoCommand::mute,8);
    check(account_video_commands_push(queue,volume)&&account_video_commands_push(queue,volume2)&&queue.count==1&&
        account_video_commands_push(queue,play)&&account_video_commands_push(queue,volume)&&queue.count==3&&
        account_video_commands_push(queue,mute)&&account_video_commands_push(queue,mute2)&&queue.count==4&&
        take(queue,volume2)&&take(queue,play)&&take(queue,volume)&&take(queue,mute2),
        "video consecutive tail Volume or Mute coalesces, preserving intervening Play");

    queue={};
    bool admitted=true;
    for(u64 serial=1;serial<=8;++serial)
        admitted=account_video_commands_push(queue,command(AccountVideoCommand::pause,serial))&&admitted;
    const auto full=queue;
    bool rejected=true;
    for(u64 serial=9;serial<=72;++serial)
        rejected=!account_video_commands_push(queue,command(AccountVideoCommand::play,serial))&&equal(queue,full)&&rejected;
    check(admitted&&queue.count==8&&rejected,
        "bounded video command flood refuses every overflow with all slots and metadata unchanged");
    const auto stop=command(AccountVideoCommand::stop,73);
    check(account_video_commands_push(queue,stop)&&queue.head==0&&queue.count==1&&take(queue,stop)&&!queue.count,
        "video Stop supersedes a full queue and is the only remaining intent");

    queue={};
    for(u64 serial=1;serial<=7;++serial)account_video_commands_push(queue,command(AccountVideoCommand::pause,serial));
    account_video_commands_push(queue,volume);
    check(account_video_commands_push(queue,volume2)&&queue.count==8&&queue.commands[7].command_serial==7,
        "video full queue still accepts a consecutive Volume replacement");
    bool ordered=true;
    for(u64 serial=1;serial<=7;++serial)ordered=take(queue,command(AccountVideoCommand::pause,serial))&&ordered;
    check(ordered&&take(queue,volume2)&&!queue.count,
        "video full queue tail coalescing preserves all earlier commands");

    queue={};
    for(u64 serial=1;serial<=7;++serial)account_video_commands_push(queue,command(AccountVideoCommand::pause,serial));
    account_video_commands_push(queue,command(AccountVideoCommand::seek,8));
    const auto latest_seek=command(AccountVideoCommand::seek,100);
    check(account_video_commands_push(queue,latest_seek)&&queue.count==8&&equal(queue.commands[7],latest_seek),
        "video full queue accepts final Seek target and serial by replacing consecutive tail Seek");
    const auto full_seek=queue;
    check(!account_video_commands_push(queue,command(AccountVideoCommand::play,101))&&equal(queue,full_seek),
        "video full Seek queue rejects another command kind without altering latest target or earlier intents");
    ordered=true;
    for(u64 serial=1;serial<=7;++serial)ordered=take(queue,command(AccountVideoCommand::pause,serial))&&ordered;
    check(ordered&&take(queue,latest_seek)&&!queue.count,
        "video full queue Seek replacement preserves earlier FIFO and dispatches latest mouseup target");

    queue={};
    for(u64 serial=1;serial<=8;++serial)account_video_commands_push(queue,command(AccountVideoCommand::pause,serial));
    ordered=true;
    for(u64 serial=1;serial<=5;++serial)ordered=take(queue,command(AccountVideoCommand::pause,serial))&&ordered;
    for(u64 serial=9;serial<=13;++serial)
        ordered=account_video_commands_push(queue,command(AccountVideoCommand::play,serial))&&ordered;
    check(ordered&&queue.head==5&&queue.count==8,"video FIFO reuses wrapped slots at its fixed capacity");
    for(u64 serial=6;serial<=13;++serial)
        ordered=take(queue,command(serial<=8?AccountVideoCommand::pause:AccountVideoCommand::play,serial))&&ordered;
    check(ordered&&!queue.count&&!account_video_commands_peek(queue,output)&&!account_video_commands_pop(queue),
        "video wrapped FIFO drains in admission order without duplicating retired commands");
}
}
