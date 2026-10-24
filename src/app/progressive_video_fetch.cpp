#include "maicord/app/progressive_video_fetch.hpp"
#include "maicord/app/segmented_video_fetch.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/hevc_mp4_demux.hpp"


namespace maicord {
namespace {
ProgressiveVideoAccept fail(ProgressiveVideoFetch* f) noexcept {
    f->state = ProgressiveVideoFetchState::failed;
    return ProgressiveVideoAccept::failed;
}
} // namespace

bool progressive_video_fetch_begin(ProgressiveVideoFetch* f, ConstSlice<u8> target, CdnHost host,
                                   u64 expires_at_seconds, ProgressiveVideoKind kind) noexcept {
    if (!f || !target.data || !target.count || target.count > sizeof(f->target) ||
        static_cast<u8>(kind) > static_cast<u8>(ProgressiveVideoKind::fragmented_audio))
        return false;
    progressive_video_fetch_release(f);
    copy_bytes(f->target, target.data, target.count);
    f->target_bytes = target.count;
    f->host = host;
    f->kind = kind;
    f->image_type = kind == ProgressiveVideoKind::fragmented_audio ? CdnImageType::m4a : CdnImageType::mp4;
    f->expires_at_seconds = expires_at_seconds;
    f->refreshable = true;
    f->state = ProgressiveVideoFetchState::probing;
    return true;
}
bool progressive_video_fetch_restore(ProgressiveVideoFetch* f) noexcept {
    if (f && f->segmented) {
        if (!mp4_fragment_source_valid(f->fragment)) return false;
        return f->kind != ProgressiveVideoKind::fragmented_audio || f->audio.source ||
            mp4_fragment_audio_initialize(&f->fragment, &f->audio) == Mp4DemuxError::none;
    }
    if (!f || !f->source.base) return false;
    const bool audio = f->kind == ProgressiveVideoKind::fragmented_audio;
    if (f->kind == ProgressiveVideoKind::movie || (f->fragment.source && (!audio || f->audio.source))) return true;
    if (!f->fragment.source && mp4_fragment_source_initialize(&f->source, audio, &f->fragment) != Mp4DemuxError::none) return false;
    if (audio) {
        const auto error=mp4_fragment_audio_initialize(&f->fragment,&f->audio);
        if(error!=Mp4DemuxError::none) {
            // The first real fragment may be needed to resolve edit timing.
            // Keep its missing-byte demand until the existing fetcher fills it.
            if(error!=Mp4DemuxError::data_pending) mp4_fragment_source_release(&f->fragment);
            return false;
        }
    }
    u64 duration{};
    if (mp4_fragment_source_duration(f->fragment, &duration)) progressive_source_set_duration(&f->source, duration);
    return true;
}
void progressive_video_fetch_retire(ProgressiveVideoFetch* f) noexcept {
    if (!f) return;
    if (f->segmented) { segmented_video_retire(f); return; }
    f->audio = {}; mp4_fragment_source_release(&f->fragment);
    progressive_source_reset_readers(&f->source); progressive_source_trim(&f->source);
}

void progressive_video_fetch_release(ProgressiveVideoFetch* f) noexcept {
    if (!f)
        return;
    segmented_video_release(f);
    progressive_source_release(&f->source);
    secure_clear_bytes(f, sizeof(*f));
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
    if (f->segmented) return segmented_video_next(f, read_ahead, now_seconds);
    if (!f->source.base) {
        // The head probe also learns the entity length from Content-Range.
        if (f->inflight.bytes)
            return next;
        next.range = {0, f->kind == ProgressiveVideoKind::fragmented_audio ? kProgressiveBlockBytes : kProgressiveHeadProbeBytes};
        next.step = ProgressiveVideoStep::submit;
    } else if (f->state == ProgressiveVideoFetchState::probing) {
        if (f->inflight.bytes)
            return next;
        ProgressiveProbe probe{};
        if (f->kind == ProgressiveVideoKind::movie) probe = progressive_mp4_probe(&f->source);
        else {
            const bool ready = progressive_video_fetch_restore(f);
            if (ready) probe.state = ProgressiveProbeState::ready;
            else {
                const auto plan = progressive_schedule(f->source, {}, false);
                probe = {plan.request, plan.submit ? ProgressiveProbeState::need_data : ProgressiveProbeState::unsupported};
            }
        }
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
        const bool exhausted = !f->refreshable || ++f->refreshes > kProgressiveVideoMaximumRefreshes;
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
    if (f && f->inflight_id)
        f->cancelling = true;
}

ProgressiveVideoAccept progressive_video_fetch_accept(ProgressiveVideoFetch* f, u64 request_id,
                                                      bool transport_ok,
                                                      const CdnResponseResult& policy,
                                                      ConstSlice<u8> body,u64 initial_clock) noexcept {
    if (!f || !request_id || request_id != f->inflight_id)
        return ProgressiveVideoAccept::ignored;
    const bool cancelled = f->cancelling;
    f->inflight_id = 0;
    f->inflight = {};
    f->cancelling = false;
    if (cancelled && f->segmented) return ProgressiveVideoAccept::ignored;
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
        if (!f->refreshable)
            return fail(f);
        f->state = ++f->refreshes > kProgressiveVideoMaximumRefreshes ? ProgressiveVideoFetchState::failed
                                                                      : ProgressiveVideoFetchState::refresh_required;
        return f->state == ProgressiveVideoFetchState::failed ? ProgressiveVideoAccept::failed
                                                              : ProgressiveVideoAccept::refresh_required;
    }
    if (f->segmented) return segmented_video_accept(f, policy, body, initial_clock);
    u64 first = 0, length = 0;
    if (policy.disposition == CdnResponseDisposition::accept_partial) {
        first = policy.range_first;
        length = policy.entity_bytes;
    } else if (policy.disposition == CdnResponseDisposition::accept_body) {
        if (!f->source.base && f->kind == ProgressiveVideoKind::movie)
            return ProgressiveVideoAccept::whole_entity;
        if (f->kind != ProgressiveVideoKind::movie && body.count >
            (f->kind == ProgressiveVideoKind::fragmented_audio ? kProgressiveAudioResidentBytes : kProgressiveResidentBudgetBytes))
            return fail(f);
        length = body.count;
    } else {
        return fail(f);
    }
    if (!body.count || policy.body_bytes != body.count || !length || first > length ||
        body.count > length - first)
        return fail(f);
    if (!f->source.base && !progressive_source_initialize(&f->source, static_cast<u32>(length),
            f->kind == ProgressiveVideoKind::fragmented_audio))
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
    f->state = f->source.base && (f->kind == ProgressiveVideoKind::movie
        ? progressive_mp4_probe(&f->source).state == ProgressiveProbeState::ready : progressive_video_fetch_restore(f))
                   ? ProgressiveVideoFetchState::ready
                   : ProgressiveVideoFetchState::probing;
    return true;
}

u32 progressive_video_fetch_buffered(ProgressiveVideoFetch* f, u64 duration_us, u16* begin, u16* end,
                                     u32 capacity) noexcept {
    if (f && f->segmented && f->state == ProgressiveVideoFetchState::ready)
        return segmented_video_buffered(f, duration_us, begin, end, capacity);
    if (!f || !f->source.base || f->state == ProgressiveVideoFetchState::probing)
        return 0;
    if (f->kind != ProgressiveVideoKind::movie) {
        if (!f->fragment.source || !duration_us || !capacity || !begin || !end) return 0;
        Mp4FragmentRange range{}; u32 count = 0;
        if (!mp4_fragment_index_range(f->fragment.index, 0, &range)) return 0;
        for (u32 i = 0; i < f->fragment.index.count; ++i) {
            i64 first{}, last{};
            if (progressive_source_available(f->source, range.offset, range.bytes) &&
                mp4_fragment_source_time(f->fragment, static_cast<i64>(range.time), &first) &&
                mp4_fragment_source_time(f->fragment, static_cast<i64>(range.time + range.duration), &last)) {
                const u16 b = static_cast<u16>(minimum<u64>(first < 0 ? 0 : static_cast<u64>(first), duration_us) * 10000 / duration_us);
                const u16 e = static_cast<u16>(minimum<u64>(last < 0 ? 0 : static_cast<u64>(last), duration_us) * 10000 / duration_us);
                if (e > b) {
                    if (count && b <= end[count - 1] + 1u) end[count - 1] = maximum(end[count - 1], e);
                    else if (count < capacity) { begin[count] = b; end[count++] = e; }
                }
            }
            if (i + 1 < f->fragment.index.count && !mp4_fragment_index_follow(f->fragment.index, range, &range)) break;
        }
        return count;
    }
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

bool progressive_video_fetch_pending(const ProgressiveVideoFetch* f) noexcept {
    if (!f) return false;
    if (f->segmented) return f->segmented->store.pending;
    if (f->source.base) for (const auto& reader : f->source.readers)
        if (reader.active && reader.pending && !progressive_source_available(f->source, reader.missing, 1)) return true;
    return false;
}
u64 progressive_video_fetch_generation(const ProgressiveVideoFetch* f) noexcept {
    return !f ? 0 : f->segmented ? f->segmented->store.generation : f->source.generation;
}

} // namespace maicord
