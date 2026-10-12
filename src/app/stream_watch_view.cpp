#include "maicord/app/stream_watch_view.hpp"

namespace maicord {
StreamWatchNotice stream_watch_notice(GatewayStreamEnd end, GatewayStreamDeleteReason reason) noexcept {
    switch (end) {
    case GatewayStreamEnd::deleted:
        switch (reason) {
        case GatewayStreamDeleteReason::stream_full:
            return StreamWatchNotice::full;
        case GatewayStreamDeleteReason::unauthorized:
            return StreamWatchNotice::not_allowed;
        case GatewayStreamDeleteReason::safety_guild_rate_limited:
            return StreamWatchNotice::limited;
        default:
            return StreamWatchNotice::ended;
        }
    case GatewayStreamEnd::timeout:
        return StreamWatchNotice::timed_out;
    case GatewayStreamEnd::failed:
    case GatewayStreamEnd::invalid_event:
    case GatewayStreamEnd::storage:
        return StreamWatchNotice::failed;
    case GatewayStreamEnd::session:
        return StreamWatchNotice::interrupted;
    default: // none, stopped, left_channel
        return StreamWatchNotice::none;
    }
}

StreamWatchView stream_watch_view(const GatewayClientStreamSnapshot& snapshot, const GatewayStreamKey& requested,
                                  u64 requested_ms, u64 now_ms) noexcept {
    StreamWatchView view{};
    view.volume_percent = snapshot.volume_percent ? snapshot.volume_percent : 100;
    if (!gateway_stream_key_valid(requested))
        return view;
    view.key = requested;
    view.state = StreamWatchState::connecting;
    if (!(snapshot.key == requested))
        return view; // The worker has not taken the request yet.
    view.viewers = snapshot.viewer_count;
    view.paused = snapshot.paused;
    const bool fresh = now_ms >= requested_ms && now_ms - requested_ms < kStreamWatchRequestGraceMs;
    switch (snapshot.phase) {
    case GatewayStreamPhase::active:
        view.state = StreamWatchState::live;
        view.video_ssrc = snapshot.video_ssrc;
        break;
    case GatewayStreamPhase::reconnecting:
        view.state = StreamWatchState::reconnecting;
        view.video_ssrc = snapshot.video_ssrc;
        break;
    case GatewayStreamPhase::ended:
        if (fresh)
            break; // The previous watch of this key; the new one is on its way.
        view.notice = stream_watch_notice(snapshot.end, snapshot.delete_reason);
        // A silent end (stopped elsewhere, left the channel) shows nothing.
        view.state = view.notice == StreamWatchNotice::none ? StreamWatchState::none : StreamWatchState::ended;
        view.viewers = 0;
        break;
    default: // idle, requested, awaiting_server, connecting
        break;
    }
    return view;
}
} // namespace maicord
