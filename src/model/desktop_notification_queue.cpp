#include "maicord/model/desktop_notification_queue.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
bool aliases(const DesktopNotificationQueue& q,const void* p,usize bytes) noexcept {
    return byte_ranges_conflict(p,bytes,&q,sizeof(q)) || byte_ranges_conflict(p,bytes,q.rows.data,usize(q.rows.count)*sizeof(*q.rows.data)) ||
        byte_ranges_conflict(p,bytes,q.bytes.data,q.bytes.count) || byte_ranges_conflict(p,bytes,q.scratch.data,q.scratch.count);
}
bool valid_text(Utf8View text) noexcept {
    if((text.bytes && !text.data) || !utf8_validate(text).valid) return false;
    for(u32 i=0;i<text.bytes;++i) if(!text.data[i]) return false;
    return true;
}
u32 size(const DesktopNotificationQueueRow& row) noexcept {return row.title_bytes+row.body_bytes+row.avatar_bytes;}
void increment(u64& n) noexcept {if(n!=~u64{0})++n;}
bool initialized(const DesktopNotificationQueue* q) noexcept {return q && q->rows.data && q->bytes.data && q->scratch.data;}
void clear_inflight(DesktopNotificationQueue& q) noexcept {
    clear_bytes(q.scratch.data,size(q.inflight));reset_value(&q.inflight);q.has_inflight=q.dispatching=q.inflight_canceled=false;q.attempt=0;
}
void erase_head(DesktopNotificationQueue& q) noexcept {
    const u32 n=size(q.rows[0]);
    copy_bytes(q.bytes.data,q.bytes.data+n,q.used-n);clear_bytes(q.bytes.data+q.used-n,n);
    for(u32 i=1;i<q.count;++i){q.rows[i-1]=q.rows[i];q.rows[i-1].offset-=n;}
    reset_value(&q.rows[--q.count]);q.used-=n;
}
enum class Purge : u8 {all,channel,message};
bool matches(const DesktopNotificationQueueRow& r,Purge kind,u64 p,u64 g,u64 c,u64 m) noexcept {
    return kind==Purge::all || (r.profile_id==p && r.generation==g && r.channel_id==c &&
        (kind==Purge::channel || r.message_id==m));
}
u32 purge(DesktopNotificationQueue* q,Purge kind,u64 p=0,u64 g=0,u64 c=0,u64 m=0) noexcept {
    if(!initialized(q))return 0;
    u32 kept=0,used=0,removed=0;
    for(u32 i=0;i<q->count;++i){auto row=q->rows[i];
        if(matches(row,kind,p,g,c,m)){++removed;increment(q->counters.canceled);continue;}
        const u32 n=size(row);copy_bytes(q->bytes.data+used,q->bytes.data+row.offset,n);row.offset=used;used+=n;q->rows[kept++]=row;
    }
    clear_bytes(q->bytes.data+used,q->used-used);
    for(u32 i=kept;i<q->count;++i)reset_value(&q->rows[i]);q->count=kept;q->used=used;
    if(q->has_inflight && !q->inflight_canceled && matches(q->inflight,kind,p,g,c,m)){
        ++removed;increment(q->counters.canceled);q->inflight_canceled=true;
        // Preserve borrowed scratch while the native call is in progress.
        if(!q->dispatching)clear_inflight(*q);
    }
    return removed;
}
}
bool desktop_notification_queue_initialize(DesktopNotificationQueue* q,Slice<DesktopNotificationQueueRow> rows,
    Slice<char> bytes,Slice<char> scratch) noexcept {
    if(!q || !rows.data || !rows.count || rows.count>kDesktopNotificationQueueRows || !bytes.data || !bytes.count ||
        bytes.count>kDesktopNotificationQueueBytes || !scratch.data || !scratch.count || scratch.count>kDesktopNotificationQueueItemBytes ||
        byte_ranges_conflict(q,sizeof(*q),rows.data,usize(rows.count)*sizeof(*rows.data)) || byte_ranges_conflict(q,sizeof(*q),bytes.data,bytes.count) ||
        byte_ranges_conflict(q,sizeof(*q),scratch.data,scratch.count) || byte_ranges_conflict(rows.data,usize(rows.count)*sizeof(*rows.data),bytes.data,bytes.count) ||
        byte_ranges_conflict(rows.data,usize(rows.count)*sizeof(*rows.data),scratch.data,scratch.count) || byte_ranges_conflict(bytes.data,bytes.count,scratch.data,scratch.count))return false;
    reset_value(q);q->rows=rows;q->bytes=bytes;q->scratch=scratch;
    clear_bytes(rows.data,rows.count*sizeof(*rows.data));clear_bytes(bytes.data,bytes.count);clear_bytes(scratch.data,scratch.count);return true;
}
void desktop_notification_queue_reset(DesktopNotificationQueue* q,u64 p,u64 g) noexcept {
    if(!initialized(q))return;purge(q,Purge::all);q->profile_id=p;q->generation=g;
}
bool desktop_notification_queue_can_admit(const DesktopNotificationQueue* q,u32 bytes) noexcept {
    return initialized(q) && q->profile_id && q->generation && q->next_token && bytes<=q->scratch.count &&
        q->count+u32(q->has_inflight)<q->rows.count && bytes<=q->bytes.count-q->used;
}
DesktopNotificationQueueResult desktop_notification_queue_enqueue(DesktopNotificationQueue* q,const DesktopNotificationQueueInput& in) noexcept {
    if(!initialized(q))return {DesktopNotificationQueueError::invalid_input};
    // Snapshot scalar/views first, so the input object itself can be temporary.
    const auto value=in;
    const u64 total=u64(value.title.bytes)+value.body.bytes+value.avatar_file_uri.bytes;
    if(total>q->scratch.count)return {DesktopNotificationQueueError::invalid_input};
    if(!value.profile_id || !value.generation || !value.channel_id || !value.message_id ||
        aliases(*q,value.title.data,value.title.bytes) || aliases(*q,value.body.data,value.body.bytes) ||
        aliases(*q,value.avatar_file_uri.data,value.avatar_file_uri.bytes) ||
        !valid_text(value.title) || !valid_text(value.body) || !valid_text(value.avatar_file_uri))return {DesktopNotificationQueueError::invalid_input};
    if(value.profile_id!=q->profile_id || value.generation!=q->generation)return {DesktopNotificationQueueError::stale_generation};
    const auto duplicate=[&](const DesktopNotificationQueueRow& r) noexcept {return r.profile_id==value.profile_id &&
        r.generation==value.generation && r.channel_id==value.channel_id && r.message_id==value.message_id;};
    if(q->has_inflight && duplicate(q->inflight))return {DesktopNotificationQueueError::duplicate,q->inflight.token};
    for(u32 i=0;i<q->count;++i)if(duplicate(q->rows[i]))return {DesktopNotificationQueueError::duplicate,q->rows[i].token};
    if(!q->next_token)return {DesktopNotificationQueueError::token_exhausted};
    if(q->count+u32(q->has_inflight)==q->rows.count){increment(q->counters.capacity_failures);return {DesktopNotificationQueueError::full_rows};}
    if(total>q->bytes.count-q->used){increment(q->counters.capacity_failures);return {DesktopNotificationQueueError::full_bytes};}
    const bool wake=q->count==0 && !q->has_inflight;
    DesktopNotificationQueueRow row{q->next_token,value.profile_id,value.generation,value.channel_id,value.message_id,
        value.source_sequence,q->used,value.title.bytes,value.body.bytes,value.avatar_file_uri.bytes,value.silent,value.suppress_popup,
        value.policy_channel_id,value.sound_epoch,value.sound_id};
    u32 at=q->used;copy_bytes(q->bytes.data+at,value.title.data,value.title.bytes);at+=value.title.bytes;
    copy_bytes(q->bytes.data+at,value.body.data,value.body.bytes);at+=value.body.bytes;
    copy_bytes(q->bytes.data+at,value.avatar_file_uri.data,value.avatar_file_uri.bytes);at+=value.avatar_file_uri.bytes;
    q->rows[q->count++]=row;q->used=at;++q->next_token;increment(q->counters.admitted);
    q->counters.high_rows=maximum(q->counters.high_rows,q->count+u32(q->has_inflight));q->counters.high_bytes=maximum(q->counters.high_bytes,q->used);
    return {DesktopNotificationQueueError::none,row.token,wake};
}
bool desktop_notification_queue_front(const DesktopNotificationQueue* q,DesktopNotificationQueueRow* row) noexcept {
    if(!initialized(q) || !row || aliases(*q,row,sizeof(*row)))return false;
    if(q->has_inflight){*row=q->inflight;return !q->inflight_canceled;}
    if(!q->count)return false;*row=q->rows[0];return true;
}
bool desktop_notification_queue_pending(const DesktopNotificationQueue* q) noexcept {return initialized(q) && (q->count || q->has_inflight);}
DesktopNotificationQueueMetrics desktop_notification_queue_metrics(const DesktopNotificationQueue* q) noexcept {
    if(!initialized(q))return {};auto out=q->counters;out.waiting=q->count;out.bytes=q->used;out.outstanding=q->count+u32(q->has_inflight);return out;
}
DesktopNotificationQueueResult desktop_notification_queue_begin(DesktopNotificationQueue* q,DesktopNotificationQueueDispatch* out) noexcept {
    if(!initialized(q) || !out || aliases(*q,out,sizeof(*out)))return {DesktopNotificationQueueError::invalid_input};
    if(q->dispatching)return {DesktopNotificationQueueError::busy};
    if(!q->has_inflight){
        if(!q->count)return {DesktopNotificationQueueError::empty};
        q->inflight=q->rows[0];q->inflight.offset=0;copy_bytes(q->scratch.data,q->bytes.data,size(q->inflight));
        erase_head(*q);q->has_inflight=true;q->attempt=0;
    }
    if(q->attempt==~u32{0})return {DesktopNotificationQueueError::token_exhausted,q->inflight.token};
    ++q->attempt;q->dispatching=true;
    *out={q->inflight,{q->scratch.data,q->inflight.title_bytes},
        {q->scratch.data+q->inflight.title_bytes,q->inflight.body_bytes},
        {q->scratch.data+q->inflight.title_bytes+q->inflight.body_bytes,q->inflight.avatar_bytes},q->attempt};
    return {DesktopNotificationQueueError::none,q->inflight.token};
}
DesktopNotificationQueueCompletion desktop_notification_queue_complete(DesktopNotificationQueue* q,u64 token,u32 attempt,DesktopNotificationQueueOutcome outcome) noexcept {
    if(!initialized(q) || outcome>DesktopNotificationQueueOutcome::retry)return {DesktopNotificationQueueError::invalid_input};
    if(!q->has_inflight || !q->dispatching || !token || !attempt || q->inflight.token!=token || q->attempt!=attempt)
        return {DesktopNotificationQueueError::stale_token};
    DesktopNotificationQueueCompletion result{DesktopNotificationQueueError::none,q->inflight,false,q->inflight_canceled,
        q->inflight_canceled && outcome==DesktopNotificationQueueOutcome::submitted};
    q->dispatching=false;
    if(outcome==DesktopNotificationQueueOutcome::retry && !q->inflight_canceled){increment(q->counters.retries);result.retry_pending=true;return result;}
    if(outcome==DesktopNotificationQueueOutcome::submitted)increment(q->counters.submitted);
    else if(outcome==DesktopNotificationQueueOutcome::failed)increment(q->counters.failed);
    clear_inflight(*q);return result;
}
u32 desktop_notification_queue_purge_channel(DesktopNotificationQueue* q,u64 p,u64 g,u64 c) noexcept {return p&&g&&c?purge(q,Purge::channel,p,g,c):0;}
u32 desktop_notification_queue_purge_message(DesktopNotificationQueue* q,u64 p,u64 g,u64 c,u64 m) noexcept {return p&&g&&c&&m?purge(q,Purge::message,p,g,c,m):0;}
u32 desktop_notification_queue_purge_all(DesktopNotificationQueue* q) noexcept {return purge(q,Purge::all);}
} // namespace maicord
