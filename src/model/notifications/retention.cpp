#include "maicord/model/notifications/retention.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/hash.hpp"

namespace maicord {
namespace {
using Error = NotificationRetentionError;

u32 bucket(u64 channel,u32 mask) noexcept {
    // Snowflakes often share their low worker/sequence bits. Hash all eight
    // bytes, rather than masking a one-step mix of the unshifted integer.
    return static_cast<u32>(hash64_bytes(kHash64Seed,&channel,sizeof(channel)))&mask;
}
struct Found {u32 slot,row;bool exists;};
Found find(const NotificationRetention& n,u64 channel) noexcept {
    if(!n.index.count)return {};
    const u32 mask=n.index.count-1;
    u32 slot=bucket(channel,mask);
    // The index keeps at least half its slots empty, so every probe ends.
    for(;n.index[slot];slot=(slot+1)&mask)
        if(n.rows[n.index[slot]-1].channel==channel)return {slot,n.index[slot]-1,true};
    return {slot};
}
void zero_index(Slice<u32> index) noexcept {clear_bytes(index.data,index.count*static_cast<u32>(sizeof(u32)));}
void erase(NotificationRetention& n,const Found& found) noexcept {
    const u32 last=n.count-1,mask=n.index.count-1;
    u32 moved_slot=found.row!=last?find(n,n.rows[last].channel).slot:0;
    u32 hole=found.slot,slot=(hole+1)&mask;
    while(n.index[slot]){
        const u32 value=n.index[slot],home=bucket(n.rows[value-1].channel,mask);
        if(((slot-home)&mask)>=((slot-hole)&mask)){
            n.index[hole]=value;
            if(value==last+1)moved_slot=hole;
            hole=slot;
        }
        slot=(slot+1)&mask;
    }
    n.index[hole]=0;
    if(found.row!=last){n.rows[found.row]=n.rows[last];n.index[moved_slot]=found.row+1;}
    reset_value(&n.rows[last]);--n.count;
}
u32 ticket_index(const NotificationRetentionRow& row,u32 ordinal) noexcept {
    const u32 index=u32(row.head)+ordinal;
    return index>=kNotificationRetentionTickets?index-kNotificationRetentionTickets:index;
}
}
void notification_retention_rebind(NotificationRetention* n,Slice<NotificationRetentionRow> rows,Slice<u32> index) noexcept {
    for(u32 i=0;i<n->count;++i)rows[i]=n->rows[i];
    zero_index(index);
    for(u32 i=0;i<n->count;++i){
        const u32 mask=index.count-1;
        u32 slot=bucket(rows[i].channel,mask);
        while(index[slot])slot=(slot+1)&mask;
        index[slot]=i+1;
    }
    n->rows=rows;n->index=index;
}
NotificationRetentionRoom notification_retention_room(const NotificationRetention* n,u64 channel) noexcept {
    if(!channel)return {Error::invalid_input};
    const auto located=find(*n,channel);
    if(located.exists)return {Error::none,n->rows[located.row].count,true};
    return {n->count<n->rows.count?Error::none:Error::full_channels};
}
NotificationRetentionAppend notification_retention_append(NotificationRetention* n,u64 channel,u64 ticket) noexcept {
    if(!channel)return {Error::invalid_input};
    const auto located=find(*n,channel);
    if(!located.exists){
        if(n->count==n->rows.count)return {Error::full_channels};
        auto& row=n->rows[n->count];reset_value(&row);row.channel=channel;row.tickets[0]=ticket;row.count=1;
        n->index[located.slot]=++n->count;return {};
    }
    auto& row=n->rows[located.row];
    if(row.count<kNotificationRetentionTickets){row.tickets[ticket_index(row,row.count)]=ticket;++row.count;return {};}
    const u64 evicted=row.tickets[row.head];row.tickets[row.head]=ticket;
    if(++row.head==kNotificationRetentionTickets)row.head=0;
    return {Error::none,evicted,true};
}
bool notification_retention_ticket(const NotificationRetention* n,u64 channel,u32 ordinal,u64* ticket) noexcept {
    if(!channel || !ticket)return false;
    const auto located=find(*n,channel);
    if(!located.exists || ordinal>=n->rows[located.row].count)return false;
    *ticket=n->rows[located.row].tickets[ticket_index(n->rows[located.row],ordinal)];return true;
}
NotificationRetentionError notification_retention_discard_channel(NotificationRetention* n,u64 channel) noexcept {
    if(!channel)return Error::invalid_input;
    const auto located=find(*n,channel);
    if(located.exists)erase(*n,located);
    return Error::none;
}
} // namespace maicord
