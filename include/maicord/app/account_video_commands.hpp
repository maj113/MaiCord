#pragma once
#include "maicord/app/account_video_playback.hpp"

namespace maicord {
// Caller serializes access and validates command identity/serial before push.
struct AccountVideoCommandQueue {
    AccountVideoControl commands[8]{};
    u8 head{},count{};
};

inline bool account_video_commands_push(AccountVideoCommandQueue& queue,const AccountVideoControl& command) noexcept {
    if(command.command==AccountVideoCommand::stop){
        queue.commands[0]=command;
        queue.head=0;
        queue.count=1;
        return true;
    }
    if(queue.count){
        const u32 tail=(queue.head+queue.count-1)%8;
        if(queue.commands[tail].command==command.command&&
            (command.command==AccountVideoCommand::seek||command.command==AccountVideoCommand::volume||
                command.command==AccountVideoCommand::mute)){
            queue.commands[tail]=command;
            return true;
        }
    }
    if(queue.count==8)return false;
    queue.commands[(queue.head+queue.count)%8]=command;
    ++queue.count;
    return true;
}

inline bool account_video_commands_peek(const AccountVideoCommandQueue& queue,AccountVideoControl& command) noexcept {
    if(!queue.count)return false;
    command=queue.commands[queue.head];
    return true;
}

inline bool account_video_commands_pop(AccountVideoCommandQueue& queue) noexcept {
    if(!queue.count)return false;
    queue.head=static_cast<u8>((queue.head+1)%8);
    --queue.count;
    return true;
}
}
