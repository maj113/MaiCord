#include "maicord/model/notification_retention.hpp"
#include "maicord/core/hash.hpp"

namespace maicord {
namespace {
using Error = NotificationRetentionError;
bool range(const void* p,usize bytes,usize alignment) noexcept {
    const usize address=reinterpret_cast<usize>(p);
    return !bytes || (p && !(address&(alignment-1)) && address<=~usize{0}-bytes);
}
bool overlaps(const void* a,usize an,const void* b,usize bn) noexcept {
    if(!an || !bn)return false;
    const usize x=reinterpret_cast<usize>(a),y=reinterpret_cast<usize>(b);
    return x<y+bn && y<x+an;
}
usize row_bytes(Slice<NotificationRetentionRow> rows) noexcept {return usize(rows.count)*sizeof(NotificationRetentionRow);}
usize index_bytes(Slice<u32> index) noexcept {return usize(index.count)*sizeof(u32);}
bool owner_range(const NotificationRetention* n) noexcept {return n && range(n,sizeof(*n),alignof(NotificationRetention));}
bool storage(const NotificationRetention* n,Slice<NotificationRetentionRow> rows,Slice<u32> index,u32 count) noexcept {
    if(!owner_range(n) || count>rows.count)return false;
    if(!rows.count || !index.count)return !rows.count && !index.count && !rows.data && !index.data && !count;
    if((index.count&(index.count-1)) || u64(index.count)<2*u64(rows.count) ||
        !range(rows.data,row_bytes(rows),alignof(NotificationRetentionRow)) ||
        !range(index.data,index_bytes(index),alignof(u32)))return false;
    return !overlaps(rows.data,row_bytes(rows),index.data,index_bytes(index)) &&
        !overlaps(n,sizeof(*n),rows.data,row_bytes(rows)) && !overlaps(n,sizeof(*n),index.data,index_bytes(index));
}
bool layout(const NotificationRetention* n) noexcept {return owner_range(n) && storage(n,n->rows,n->index,n->count);}
bool row_valid(const NotificationRetentionRow& row) noexcept {
    return row.channel && row.count && row.count<=kNotificationRetentionTickets && row.head<kNotificationRetentionTickets;
}
u32 bucket(u64 channel,u32 mask) noexcept {
    // Snowflakes often share their low worker/sequence bits. Hash all eight
    // bytes, rather than masking a one-step mix of the unshifted integer.
    return static_cast<u32>(hash64_bytes(kHash64Seed,&channel,sizeof(channel)))&mask;
}
struct Found {Error error{};u32 slot{},row{};bool exists{};};
Found find(const NotificationRetention& n,u64 channel) noexcept {
    if(!n.index.count)return {};
    const u32 mask=n.index.count-1;
    u32 slot=bucket(channel,mask);
    for(u32 visited=0;visited<n.index.count;++visited,slot=(slot+1)&mask){
        const u32 value=n.index[slot];
        if(!value)return {Error::none,slot};
        if(value>n.count || !row_valid(n.rows[value-1]))return {Error::invalid_input};
        if(n.rows[value-1].channel==channel)return {Error::none,slot,value-1,true};
    }
    return {Error::invalid_input};
}
// Full consistency is checked for rare reset/rebind operations. Hot lookups
// validate layout and every probed slot; callers must mutate metadata only
// through this API, not externally forge an otherwise well-formed table.
bool consistent(const NotificationRetention& n) noexcept {
    if(!layout(&n))return false;
    u32 occupied{};
    for(u32 i=0;i<n.index.count;++i){if(n.index[i]){if(n.index[i]>n.count)return false;++occupied;}}
    if(occupied!=n.count)return false;
    for(u32 i=0;i<n.count;++i){
        if(!row_valid(n.rows[i]))return false;
        const auto located=find(n,n.rows[i].channel);
        if(located.error!=Error::none || !located.exists || located.row!=i)return false;
    }
    return true;
}
void zero_index(Slice<u32> index) noexcept {for(u32 i=0;i<index.count;++i)index[i]=0;}
bool output_valid(const NotificationRetention& n,const void* p,usize bytes) noexcept {
    return range(p,bytes,alignof(u64)) && !overlaps(p,bytes,&n,sizeof(n)) &&
        !overlaps(p,bytes,n.rows.data,row_bytes(n.rows)) && !overlaps(p,bytes,n.index.data,index_bytes(n.index));
}
struct Erase {Error error{};u32 moved_slot{};};
Erase prepare_erase(const NotificationRetention& n,const Found& found) noexcept {
    Erase result{};
    if(found.row!=n.count-1){
        if(!row_valid(n.rows[n.count-1]))return {Error::invalid_input};
        const auto moved=find(n,n.rows[n.count-1].channel);
        if(moved.error!=Error::none || !moved.exists || moved.row!=n.count-1)return {Error::invalid_input};
        result.moved_slot=moved.slot;
    }
    const u32 mask=n.index.count-1;
    u32 slot=(found.slot+1)&mask;
    for(u32 visited=0;visited<n.index.count;++visited,slot=(slot+1)&mask){
        const u32 value=n.index[slot];
        if(!value)return result;
        if(value>n.count || !row_valid(n.rows[value-1]))return {Error::invalid_input};
    }
    return {Error::invalid_input};
}
void erase(NotificationRetention& n,const Found& found,Erase prepared) noexcept {
    const u32 last=n.count-1,mask=n.index.count-1;
    u32 hole=found.slot,slot=(hole+1)&mask;
    while(n.index[slot]){
        const u32 value=n.index[slot],home=bucket(n.rows[value-1].channel,mask);
        if(((slot-home)&mask)>=((slot-hole)&mask)){
            n.index[hole]=value;
            if(value==last+1)prepared.moved_slot=hole;
            hole=slot;
        }
        slot=(slot+1)&mask;
    }
    n.index[hole]=0;
    if(found.row!=last){n.rows[found.row]=n.rows[last];n.index[prepared.moved_slot]=found.row+1;}
    n.rows[last]={};--n.count;
}
u32 ticket_index(const NotificationRetentionRow& row,u32 ordinal) noexcept {
    const u32 index=u32(row.head)+ordinal;
    return index>=kNotificationRetentionTickets?index-kNotificationRetentionTickets:index;
}
}
bool notification_retention_initialize(NotificationRetention* n,Slice<NotificationRetentionRow> rows,Slice<u32> index) noexcept {
    if(!owner_range(n) || n->rows.data || n->rows.count || n->index.data || n->index.count || n->count ||
        !storage(n,rows,index,0))return false;
    zero_index(index);*n={rows,index,0};return true;
}
NotificationRetentionError notification_retention_rebind(NotificationRetention* n,
    Slice<NotificationRetentionRow> rows,Slice<u32> index) noexcept {
    if(!layout(n) || !storage(n,rows,index,n->count) || !consistent(*n))return Error::invalid_input;
    // Same-base prefix resizing is safe; any other overlap could destroy rows
    // needed to reconstruct the index, so reject before writing either bank.
    if((rows.data!=n->rows.data && overlaps(rows.data,row_bytes(rows),n->rows.data,row_bytes(n->rows))) ||
        (index.data!=n->index.data && overlaps(index.data,index_bytes(index),n->index.data,index_bytes(n->index))) ||
        overlaps(rows.data,row_bytes(rows),n->index.data,index_bytes(n->index)) ||
        overlaps(index.data,index_bytes(index),n->rows.data,row_bytes(n->rows)))return Error::invalid_input;
    if(rows.data!=n->rows.data)for(u32 i=0;i<n->count;++i)rows[i]=n->rows[i];
    zero_index(index);
    for(u32 i=0;i<n->count;++i){
        const u32 mask=index.count-1;
        u32 slot=bucket(rows[i].channel,mask);
        while(index[slot])slot=(slot+1)&mask;
        index[slot]=i+1;
    }
    n->rows=rows;n->index=index;return Error::none;
}
NotificationRetentionRoom notification_retention_room(const NotificationRetention* n,u64 channel) noexcept {
    if(!channel || !layout(n))return {Error::invalid_input};
    const auto located=find(*n,channel);
    if(located.error!=Error::none)return {located.error};
    if(located.exists)return {Error::none,n->rows[located.row].count,true};
    return {n->count<n->rows.count?Error::none:Error::full_channels};
}
NotificationRetentionAppend notification_retention_append(NotificationRetention* n,u64 channel,u64 ticket) noexcept {
    if(!channel || !layout(n))return {Error::invalid_input};
    const auto located=find(*n,channel);
    if(located.error!=Error::none)return {located.error};
    if(!located.exists){
        if(n->count==n->rows.count)return {Error::full_channels};
        auto& row=n->rows[n->count];row={};row.channel=channel;row.tickets[0]=ticket;row.count=1;
        n->index[located.slot]=++n->count;return {};
    }
    auto& row=n->rows[located.row];
    if(row.count<kNotificationRetentionTickets){row.tickets[ticket_index(row,row.count)]=ticket;++row.count;return {};}
    const u64 evicted=row.tickets[row.head];row.tickets[row.head]=ticket;
    if(++row.head==kNotificationRetentionTickets)row.head=0;
    return {Error::none,evicted,true};
}
bool notification_retention_ticket(const NotificationRetention* n,u64 channel,u32 ordinal,u64* ticket) noexcept {
    if(!channel || !layout(n) || !output_valid(*n,ticket,sizeof(*ticket)))return false;
    const auto located=find(*n,channel);
    if(located.error!=Error::none || !located.exists || ordinal>=n->rows[located.row].count)return false;
    *ticket=n->rows[located.row].tickets[ticket_index(n->rows[located.row],ordinal)];return true;
}
NotificationRetentionTake notification_retention_remove_channel(NotificationRetention* n,u64 channel,Slice<u64> output) noexcept {
    if(!channel || !layout(n) || !output_valid(*n,output.data,usize(output.count)*sizeof(u64)))return {Error::invalid_input};
    const auto located=find(*n,channel);
    if(located.error!=Error::none)return {located.error};
    if(!located.exists)return {};
    const auto& row=n->rows[located.row];
    if(output.count<row.count)return {Error::output_too_small};
    const auto prepared=prepare_erase(*n,located);
    if(prepared.error!=Error::none)return {prepared.error};
    const u32 count=row.count;
    for(u32 i=0;i<count;++i)output[i]=row.tickets[ticket_index(row,i)];
    erase(*n,located,prepared);return {Error::none,count};
}
NotificationRetentionError notification_retention_discard_channel(NotificationRetention* n,u64 channel) noexcept {
    if(!channel || !layout(n))return Error::invalid_input;
    const auto located=find(*n,channel);
    if(located.error!=Error::none || !located.exists)return located.error;
    const auto prepared=prepare_erase(*n,located);
    if(prepared.error!=Error::none)return prepared.error;
    erase(*n,located,prepared);return Error::none;
}
NotificationRetentionError notification_retention_reset(NotificationRetention* n) noexcept {
    if(!layout(n) || !consistent(*n))return Error::invalid_input;
    for(u32 i=0;i<n->count;++i)n->rows[i]={};
    zero_index(n->index);n->count=0;return Error::none;
}
} // namespace maicord
