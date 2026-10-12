#include "maicord/app/progressive_video_fetch.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/hevc_mp4_demux.hpp"

#include <Windows.h>

namespace maicord {
namespace {
ProgressiveVideoAccept fail(ProgressiveVideoFetch* f) noexcept {
    f->state = ProgressiveVideoFetchState::failed;
    return ProgressiveVideoAccept::failed;
}
} // namespace

bool progressive_video_fetch_begin(ProgressiveVideoFetch* f, ConstSlice<u8> target, CdnHost host,
                                   u64 expires_at_seconds) noexcept {
    if (!f || !target.data || !target.count || target.count > sizeof(f->target))
        return false;
    progressive_video_fetch_release(f);
    copy_bytes(f->target, target.data, target.count);
    f->target_bytes = target.count;
    f->host = host;
    f->expires_at_seconds = expires_at_seconds;
    f->state = ProgressiveVideoFetchState::probing;
    return true;
}

void progressive_video_fetch_release(ProgressiveVideoFetch* f) noexcept {
    if (!f)
        return;
    progressive_source_release(&f->source);
    SecureZeroMemory(f, sizeof(*f));
}

ProgressiveVideoNext progressive_video_fetch_next(ProgressiveVideoFetch* f, bool read_ahead,
                                                  u64 now_seconds) noexcept {
    ProgressiveVideoNext next{};
    if (!f || f->state == ProgressiveVideoFetchState::empty || f->state == ProgressiveVideoFetchState::failed) {
        next.step = ProgressiveVideoStep::failed;
        return next;
    }
    if (f->state == ProgressiveVideoFetchState::refresh_required) {
        next.step = ProgressiveVideoStep::refresh_required;
        return next;
    }
    if (!f->source.base) {
        // The head probe also learns the entity length from Content-Range.
        if (f->inflight.bytes)
            return next;
        next.range = {0, kProgressiveHeadProbeBytes};
        next.step = ProgressiveVideoStep::submit;
    } else if (f->state == ProgressiveVideoFetchState::probing) {
        if (f->inflight.bytes)
            return next;
        const auto probe = progressive_mp4_probe(&f->source);
        if (probe.state == ProgressiveProbeState::ready) {
            f->state = ProgressiveVideoFetchState::ready;
            next.step = ProgressiveVideoStep::ready;
            return next;
        }
        if (probe.state != ProgressiveProbeState::need_data) {
            f->state = ProgressiveVideoFetchState::failed;
            next.step = ProgressiveVideoStep::failed;
            return next;
        }
        next.range = {probe.need.first, probe.need.bytes};
        next.step = ProgressiveVideoStep::submit;
    } else {
        if (f->cancelling)
            return next;
        const auto plan = progressive_schedule(f->source, {f->inflight.first, f->inflight.bytes}, read_ahead);
        if (plan.cancel_inflight) {
            next.step = ProgressiveVideoStep::cancel;
            return next;
        }
        if (!plan.submit)
            return next;
        next.range = {plan.request.first, plan.request.bytes};
        next.step = ProgressiveVideoStep::submit;
    }
    if (f->expires_at_seconds <= now_seconds) {
        const bool exhausted = ++f->refreshes > kProgressiveVideoMaximumRefreshes;
        f->state = exhausted ? ProgressiveVideoFetchState::failed : ProgressiveVideoFetchState::refresh_required;
        next = {{}, exhausted ? ProgressiveVideoStep::failed : ProgressiveVideoStep::refresh_required};
    }
    return next;
}

void progressive_video_fetch_submitted(ProgressiveVideoFetch* f, u64 request_id,
                                       CdnByteRange range) noexcept {
    if (!f)
        return;
    f->inflight_id = request_id;
    f->inflight = range;
    f->cancelling = false;
}

void progressive_video_fetch_cancelling(ProgressiveVideoFetch* f) noexcept {
    if (f && f->inflight.bytes)
        f->cancelling = true;
}

ProgressiveVideoAccept progressive_video_fetch_accept(ProgressiveVideoFetch* f, u64 request_id,
                                                      bool transport_ok,
                                                      const CdnResponseResult& policy,
                                                      ConstSlice<u8> body) noexcept {
    if (!f || !request_id || request_id != f->inflight_id)
        return ProgressiveVideoAccept::ignored;
    const bool cancelled = f->cancelling;
    f->inflight_id = 0;
    f->inflight = {};
    f->cancelling = false;
    if (!transport_ok) {
        // A cancelled stale range, or a transient transport failure, is
        // simply rescheduled; repeated failures end the source.
        if (cancelled)
            return ProgressiveVideoAccept::ignored;
        return ++f->transport_failures < kProgressiveVideoMaximumTransportFailures ? ProgressiveVideoAccept::ignored
                                                                                   : fail(f);
    }
    if (!policy.ok())
        return fail(f);
    if (policy.disposition == CdnResponseDisposition::source_expired) {
        f->state = ++f->refreshes > kProgressiveVideoMaximumRefreshes ? ProgressiveVideoFetchState::failed
                                                                      : ProgressiveVideoFetchState::refresh_required;
        return f->state == ProgressiveVideoFetchState::failed ? ProgressiveVideoAccept::failed
                                                              : ProgressiveVideoAccept::refresh_required;
    }
    u64 first = 0, length = 0;
    if (policy.disposition == CdnResponseDisposition::accept_partial) {
        first = policy.range_first;
        length = policy.entity_bytes;
    } else if (policy.disposition == CdnResponseDisposition::accept_body) {
        if (!f->source.base)
            return ProgressiveVideoAccept::whole_entity;
        length = body.count;
    } else {
        return fail(f);
    }
    if (!body.count || policy.body_bytes != body.count || !length || first > length ||
        body.count > length - first)
        return fail(f);
    if (!f->source.base && !progressive_source_initialize(&f->source, static_cast<u32>(length)))
        return fail(f);
    // A signed URL refresh or CDN replacement must still name the same entity.
    if (length != f->source.total_bytes || !progressive_source_write(&f->source, first, body))
        return fail(f);
    f->transport_failures = 0;
    f->cache_expires_at_seconds = policy.expires_at_seconds;
    f->cache_store_allowed = policy.store_allowed;
    f->cache_must_revalidate = policy.must_revalidate;
    return ProgressiveVideoAccept::progress;
}

bool progressive_video_fetch_refresh(ProgressiveVideoFetch* f, ConstSlice<u8> target, CdnHost host,
                                     u64 expires_at_seconds) noexcept {
    if (!f || f->state != ProgressiveVideoFetchState::refresh_required || !target.data || !target.count ||
        target.count > sizeof(f->target))
        return false;
    copy_bytes(f->target, target.data, target.count);
    f->target_bytes = target.count;
    f->host = host;
    f->expires_at_seconds = expires_at_seconds;
    f->state = f->source.base && progressive_mp4_probe(&f->source).state == ProgressiveProbeState::ready
                   ? ProgressiveVideoFetchState::ready
                   : ProgressiveVideoFetchState::probing;
    return true;
}

u32 progressive_video_fetch_buffered(ProgressiveVideoFetch* f, u64 duration_us, u16* begin, u16* end,
                                     u32 capacity) noexcept {
    if (!f || !f->source.base || f->state == ProgressiveVideoFetchState::probing)
        return 0;
    if (!f->index_ready) {
        const auto bytes = progressive_source_bytes(f->source);
        const auto reader = progressive_source_reader(&f->source, ProgressiveTrack::video);
        const auto error = hevc_container_present(bytes) ? hevc_demux_initialize(&f->index, bytes, reader)
                                                         : mp4_demux_initialize(&f->index, bytes, reader);
        if (error != Mp4DemuxError::none)
            return 0;
        f->index_ready = true;
    }
    return progressive_buffered_ranges(f->source, f->index, duration_us, begin, end, capacity);
}

} // namespace maicord
