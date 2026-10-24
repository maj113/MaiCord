#include "maicord/app/segmented_video_fetch.hpp"
#include "maicord/protocol/embed_player.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/media/mpeg_ts_segment_store.hpp"

namespace maicord {
namespace {
bool live_location(bool youtube,Utf8View url,bool playlist,bool transport=false) noexcept {
    return youtube ? embed_stream_location(playlist?CdnHost::youtube_playlist:CdnHost::youtube_live,url)
        : twitch_live_location(url,playlist,nullptr,transport);
}
u32 write_target(SegmentedVideoFetch& s, Slice<u8> path, bool initialization, u32 number) noexcept {
    if (s.folder_bytes > path.count) return 0;
    copy_bytes(path.data, s.folder, s.folder_bytes);
    if (s.playlist.input.bytes) {
        if (s.manifest.bytes) {
            if (s.sequence_base > ~u64{0} - number || s.sequence_base + number < s.playlist.sequence ||
                s.sequence_base + number - s.playlist.sequence >= s.playlist.segments) return 0;
            number = static_cast<u32>(s.sequence_base + number - s.playlist.sequence);
        }
        Utf8View part = s.playlist.initialization;
        if (!initialization || s.playlist.transport) {
            if (!s.current.uri.bytes || number != s.current.index) {
                const bool found = s.current.uri.bytes && number == s.current.index + 1
                    ? hls_media_follow(s.playlist, s.current, &s.current) : hls_media_segment(s.playlist, number, &s.current);
                if (!found) return 0;
            }
            part = s.current.uri;
        }
        if (s.vimeo || s.soundcloud) {
            const u32 bytes = hls_resolve_uri({s.folder, s.folder_bytes}, part,
                                            {reinterpret_cast<char*>(path.data), path.count});
            const Utf8View url{reinterpret_cast<const char*>(path.data), bytes};
            return bytes && (s.soundcloud ? soundcloud_location(url, false) : vimeo_location(url, false)) ? bytes : 0;
        }
        if (part.bytes > path.count - s.folder_bytes) return 0;
        copy_bytes(path.data + s.folder_bytes, part.data, part.bytes);
        const u32 bytes = s.folder_bytes + part.bytes;
        const Utf8View url{reinterpret_cast<const char*>(path.data), bytes};
        return (s.manifest.bytes ? live_location(s.youtube,url,false,s.playlist.transport) : twitch_vod_location(url, false)) ? bytes : 0;
    }
    const u32 size = dash_segment_path(s.track, number, initialization,
        {path.data + s.folder_bytes, path.count - s.folder_bytes});
    const u32 bytes = s.folder_bytes + size;
    return size && steam_media_location({reinterpret_cast<const char*>(path.data), bytes}) ? bytes : 0;
}
bool target(ProgressiveVideoFetch& f, bool initialization, u32 number) noexcept {
    f.segmented->refreshing = false;
    if (f.segmented->manifest.bytes) {
        f.host = f.segmented->youtube?CdnHost::youtube_live:CdnHost::twitch_live;
        f.image_type = f.segmented->playlist.transport ? CdnImageType::mpeg_ts :
            f.kind == ProgressiveVideoKind::fragmented_audio ? CdnImageType::m4a : CdnImageType::mp4;
    }
    f.target_bytes = write_target(*f.segmented, {f.target, sizeof(f.target)}, initialization, number);
    return f.target_bytes != 0;
}
ProgressiveVideoNext failed(ProgressiveVideoFetch& f) noexcept {
    f.state = ProgressiveVideoFetchState::failed; return {{}, ProgressiveVideoStep::failed};
}
bool locate(void* context, u64 time, u32* number) noexcept {
    auto& s = *static_cast<SegmentedVideoFetch*>(context);
    HlsMediaSegment found{};
    // Container and playlist endpoints can differ by one frame. The last file
    // is still checked against its actual interval by the source seek path.
    if (time >= s.playlist.duration_us) { *number = s.playlist.segments - 1; return true; }
    if (!hls_media_find(s.playlist, time, &found)) return false;
    *number = found.index; return true;
}
bool live_locations(const HlsMediaPlaylist& playlist,bool youtube) noexcept {
    if (!playlist.transport && (youtube || !twitch_live_location(playlist.initialization, false))) return false;
    HlsMediaSegment segment{};
    for (u32 i = 0; i < playlist.segments; ++i)
        if (!(i ? hls_media_follow(playlist, segment, &segment) : hls_media_segment(playlist, 0, &segment)) ||
            !live_location(youtube,segment.uri,false,playlist.transport)) return false;
    return true;
}
bool live_counts(const SegmentedVideoFetch& s, const HlsMediaPlaylist& p, u32* first, u32* count) noexcept {
    const u64 last = p.sequence + p.segments - 1;
    if (last < s.sequence_base || last - s.sequence_base >= ~u32{0}) return false;
    *first = p.sequence > s.sequence_base ? static_cast<u32>(p.sequence - s.sequence_base) : 0;
    *count = static_cast<u32>(last - s.sequence_base + 1); return true;
}
}
bool progressive_video_fetch_begin_hls(ProgressiveVideoFetch* f, Utf8View manifest, Utf8View text, bool audio, bool live, u64 now) noexcept {
    HlsMediaPlaylist parsed{};
    const bool youtube=live && embed_stream_location(CdnHost::youtube_playlist,manifest);
    const bool vimeo = !live && vimeo_location(manifest, true);
    const bool soundcloud = !live && audio && soundcloud_location(manifest, true);
    if (!f || !(live ? now && live_location(youtube,manifest,true) : vimeo || soundcloud || twitch_vod_location(manifest, true)) ||
        hls_media_playlist(text, &parsed, live ? HlsPlaylistMode::sliding : vimeo || soundcloud ? HlsPlaylistMode::complete_uri : HlsPlaylistMode::complete) != HlsPlaylistError::none ||
        (live && !live_locations(parsed,youtube))) return false;
    u32 leaf = manifest.bytes; while (leaf && manifest.data[leaf - 1] != '/') --leaf;
    if (!leaf || (!live && (vimeo || soundcloud ? manifest.bytes : leaf) > 1024)) return false;
    auto* s = static_cast<SegmentedVideoFetch*>(VirtualAlloc(nullptr,
        sizeof(SegmentedVideoFetch) + (live ? manifest.bytes + 65536 : text.bytes), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!s) return false;
    s->youtube=youtube;
    s->vimeo=vimeo;
    s->soundcloud=soundcloud;
    auto* owned = reinterpret_cast<char*>(s + 1);
    if (live) {
        copy_bytes(owned, manifest.data, manifest.bytes); s->manifest = {owned, manifest.bytes}; owned += manifest.bytes;
        // Start with at most three complete files of latency; ordinals remain
        // stable as the advertised playlist drops older entries.
        s->sequence_base = parsed.sequence + (parsed.segments > 3 ? parsed.segments - 3 : 0);
        s->refresh_at = deadline_after(now, parsed.target_seconds);
    }
    copy_bytes(owned, text.data, text.bytes);
    s->playlist = parsed;
    s->playlist.input.data = owned;
    if (parsed.initialization.bytes) s->playlist.initialization.data = s->playlist.input.data + (parsed.initialization.data - text.data);
    if (!live) { s->folder_bytes = vimeo || soundcloud ? manifest.bytes : leaf; copy_bytes(s->folder, manifest.data, s->folder_bytes); }
    u8 probe[kCdnMaximumTargetBytes];
    const u32 last = live ? minimum(3u, parsed.segments) - 1 : parsed.segments - 1;
    if (!write_target(*s, {probe, sizeof(probe)}, true, 0) || !write_target(*s, {probe, sizeof(probe)}, false, last)) {
        VirtualFree(s, 0, MEM_RELEASE); return false;
    }
    progressive_video_fetch_release(f);
    f->segmented = s; f->host = live ? (youtube?CdnHost::youtube_live:CdnHost::twitch_live) : soundcloud ? CdnHost::soundcloud_audio : vimeo ? CdnHost::vimeo_video : CdnHost::twitch_vod;
    f->kind = audio ? ProgressiveVideoKind::fragmented_audio : ProgressiveVideoKind::fragmented_video;
    f->image_type = audio ? CdnImageType::m4a : CdnImageType::mp4;
    f->state = ProgressiveVideoFetchState::probing; f->expires_at_seconds = ~u64{0};
    return true;
}
bool progressive_video_fetch_begin_segments(ProgressiveVideoFetch* f, Utf8View manifest, const DashTrack& track, bool audio) noexcept {
    if (!f || !steam_manifest_location(manifest) || !track.segment_count || track.segment_count > 8192 ||
        !track.timescale || !track.segment_duration) return false;
    u32 leaf = manifest.bytes;
    while (leaf && manifest.data[leaf - 1] != '/') --leaf;
    if (!leaf || leaf > 1024) return false;
    auto* s = static_cast<SegmentedVideoFetch*>(VirtualAlloc(nullptr, sizeof(SegmentedVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!s) return false;
    s->track = track; s->folder_bytes = leaf; copy_bytes(s->folder, manifest.data, leaf);
    Utf8View* views[]{&s->track.id, &s->track.initialization, &s->track.media};
    u32 used = 0;
    for (auto* view : views) {
        if (!view->data || !view->bytes || view->bytes > sizeof(s->text) - used) { VirtualFree(s, 0, MEM_RELEASE); return false; }
        copy_bytes(s->text + used, view->data, view->bytes); view->data = s->text + used; used += view->bytes;
    }
    u8 probe[kCdnMaximumTargetBytes];
    if (!write_target(*s, {probe, sizeof(probe)}, true, 0) || !write_target(*s, {probe, sizeof(probe)}, false, track.segment_count - 1)) {
        VirtualFree(s, 0, MEM_RELEASE); return false;
    }
    progressive_video_fetch_release(f);
    f->segmented = s; f->host = CdnHost::steam_video;
    f->kind = audio ? ProgressiveVideoKind::fragmented_audio : ProgressiveVideoKind::fragmented_video;
    f->image_type = audio ? CdnImageType::m4a : CdnImageType::mp4;
    f->state = ProgressiveVideoFetchState::probing; f->expires_at_seconds = ~u64{0};
    return true;
}
ProgressiveVideoNext segmented_video_next(ProgressiveVideoFetch* f, bool ahead, u64 now) noexcept {
    auto& s = *f->segmented;
    if (f->cancelling) return {};
    if (f->inflight_id) {
        if (!s.refreshing && !s.initialization && (!s.store.pending || s.serial != s.store.request_serial)) return {{}, ProgressiveVideoStep::cancel};
        return {};
    }
    if (!s.store.initialization.base) {
        s.initialization = true;
        const u32 limit=s.playlist.transport && f->kind!=ProgressiveVideoKind::fragmented_audio ? 8*1024*1024 : 1024*1024;
        return target(*f, true, 0) ? ProgressiveVideoNext{{0, limit}, ProgressiveVideoStep::submit} : failed(*f);
    }
    if (f->state == ProgressiveVideoFetchState::probing) {
        const auto ready = mp4_fragment_source_initialize_segments(&s.store, &f->fragment);
        if (ready == Mp4DemuxError::none) {
            if (f->kind == ProgressiveVideoKind::fragmented_audio && mp4_fragment_audio_initialize(&f->fragment, &f->audio) != Mp4DemuxError::none)
                return failed(*f);
            f->state = ProgressiveVideoFetchState::ready; return {{}, ProgressiveVideoStep::ready};
        }
        if (ready != Mp4DemuxError::data_pending) return failed(*f);
    }
    if (s.manifest.bytes && ahead && !s.store.ended && now >= s.refresh_at) {
        copy_bytes(f->target, s.manifest.data, s.manifest.bytes); f->target_bytes = s.manifest.bytes;
        f->host = s.youtube?CdnHost::youtube_playlist:CdnHost::twitch_playlist; f->image_type = CdnImageType::hls_playlist;
        s.refreshing = true; s.requested_at = now;
        return {{}, ProgressiveVideoStep::submit};
    }
    if (ahead && !s.store.pending && f->fragment.loaded && !f->fragment.preparing && !f->fragment.ended &&
               f->fragment.segment.index + 1 < s.store.count) {
        mp4_segment_store_request(&s.store, f->fragment.segment.index + 1);
    }
    if (!s.store.pending) return {};
    s.initialization = false; s.number = s.store.demand; s.serial = s.store.request_serial;
    const u32 bytes = f->kind == ProgressiveVideoKind::fragmented_audio ? 1024 * 1024 : 8 * 1024 * 1024;
    return target(*f, false, s.number) ? ProgressiveVideoNext{{0, bytes}, ProgressiveVideoStep::submit} : failed(*f);
}
ProgressiveVideoAccept segmented_video_accept(ProgressiveVideoFetch* f, const CdnResponseResult& policy, ConstSlice<u8> body,u64 initial_clock) noexcept {
    auto& s = *f->segmented;
    if (s.refreshing) {
        HlsMediaPlaylist next{}; HlsPlaylistAdvance update{}; u32 first{}, count{};
        if (policy.disposition != CdnResponseDisposition::accept_body || policy.status_code != 200 || policy.body_bytes != body.count ||
            hls_media_playlist({reinterpret_cast<const char*>(body.data), body.count}, &next, HlsPlaylistMode::sliding) != HlsPlaylistError::none ||
            !live_locations(next,s.youtube)) { failed(*f); return ProgressiveVideoAccept::failed; }
        const auto changed = hls_media_update(s.playlist, next, &update);
        const u32 interval = update.changed ? next.target_seconds : maximum(1u, next.target_seconds / 2);
        s.refresh_at = deadline_after(s.requested_at, interval); s.refreshing = false;
        if (changed == HlsPlaylistError::stale && ++f->transport_failures < kProgressiveVideoMaximumTransportFailures)
            return ProgressiveVideoAccept::ignored;
        if (changed != HlsPlaylistError::none || !live_counts(s, next, &first, &count) ||
            mp4_segment_store_extend(&s.store, first, count, next.ended) != Mp4DemuxError::none) {
            failed(*f); return ProgressiveVideoAccept::failed;
        }
        auto* owned = const_cast<char*>(s.playlist.input.data);
        copy_bytes(owned, body.data, body.count);
        if (next.initialization.bytes) next.initialization.data = owned + (next.initialization.data - next.input.data);
        next.input.data = owned;
        s.playlist = next; s.current = {}; f->transport_failures = 0;
        return ProgressiveVideoAccept::progress;
    }
    if (!s.initialization && (!s.store.pending || s.serial != s.store.request_serial || s.number != s.store.demand))
        return ProgressiveVideoAccept::ignored;
    const u32 maximum = (s.initialization && !s.playlist.transport) || f->kind == ProgressiveVideoKind::fragmented_audio ? 1024 * 1024 : 8 * 1024 * 1024;
    const bool whole = policy.disposition == CdnResponseDisposition::accept_body ||
        (policy.disposition == CdnResponseDisposition::accept_partial && !policy.range_first && policy.entity_bytes == body.count);
    if (!whole || !body.count || body.count > maximum || policy.body_bytes != body.count) {
        failed(*f); return ProgressiveVideoAccept::failed;
    }
    const auto error = s.playlist.transport
        ? (s.initialization
            ? mpeg_ts_segment_store_initialize(&s.store,body,f->kind==ProgressiveVideoKind::fragmented_audio,
                minimum(3u,s.playlist.segments),s.current.duration_us,initial_clock)
            : mpeg_ts_segment_store_accept(&s.store,s.number,s.serial,body,s.current.duration_us))
        : s.initialization
        ? mp4_segment_store_initialize(&s.store, body, f->kind == ProgressiveVideoKind::fragmented_audio,
                                      s.manifest.bytes ? minimum(3u, s.playlist.segments) : s.playlist.input.bytes ? s.playlist.segments : s.track.segment_count,
                                      s.playlist.input.bytes ? s.playlist.target_seconds : s.track.segment_duration,
                                      s.playlist.input.bytes ? 1 : s.track.timescale, s.manifest.bytes != 0)
        : mp4_segment_store_accept(&s.store, s.number, s.serial, body);
    if (error != Mp4DemuxError::none) { failed(*f); return ProgressiveVideoAccept::failed; }
    if (s.initialization && s.manifest.bytes) s.store.ended = s.playlist.ended;
    if (s.initialization && s.playlist.input.bytes && !s.manifest.bytes) { s.store.seek_context = &s; s.store.locate = locate; }
    f->transport_failures = 0;
    return ProgressiveVideoAccept::progress;
}
u32 segmented_video_refresh_delay(const ProgressiveVideoFetch* f, u64 now) noexcept {
    if (!f || !f->segmented || !f->segmented->manifest.bytes || f->segmented->store.ended || f->inflight_id ||
        f->state != ProgressiveVideoFetchState::ready) return kInfiniteWaitMilliseconds;
    const u64 due = f->segmented->refresh_at;
    return due <= now ? 0 : static_cast<u32>(minimum<u64>(due - now, (kInfiniteWaitMilliseconds - 1) / 1000) * 1000);
}
void segmented_video_retire(ProgressiveVideoFetch* f) noexcept {
    auto& s = *f->segmented;
    f->audio = {};
    if (mp4_fragment_source_valid(f->fragment)) mp4_fragment_source_select(&f->fragment, 0);
    mp4_segment_store_cancel(&s.store); s.store.active = 2;
    if (!++s.store.generation) ++s.store.generation;
    for (auto& slot : s.store.slots) { slot.bytes = 0; slot.media = {}; reserved_bytes_decommit(&slot.storage); }
}
void segmented_video_release(ProgressiveVideoFetch* f) noexcept {
    if (!f || !f->segmented) return;
    mp4_fragment_source_release(&f->fragment); mp4_segment_store_release(&f->segmented->store);
    VirtualFree(f->segmented, 0, MEM_RELEASE); f->segmented = nullptr;
}
Mp4DemuxError segmented_video_synchronize(ProgressiveVideoFetch* video, ProgressiveVideoFetch* audio) noexcept {
    using E = Mp4DemuxError;
    if (!video || !video->segmented || !video->segmented->playlist.input.bytes) return E::none;
    if (video->segmented->synchronized) return E::none;
    if (!audio || !audio->segmented || !audio->segmented->playlist.input.bytes ||
        video->state != ProgressiveVideoFetchState::ready || audio->state != ProgressiveVideoFetchState::ready ||
        !mp4_fragment_source_valid(video->fragment) || !mp4_fragment_source_valid(audio->fragment)) return E::invalid_input;
    auto& v = video->fragment; auto& a = audio->fragment;
    if (!v.track.timescale || !a.track.timescale ||
        v.track.audio || !a.track.audio || v.segment.index || a.segment.index || v.fragment.index || a.fragment.index) return E::unsupported;
    const bool live = video->segmented->manifest.bytes != 0;
    if (live != (audio->segmented->manifest.bytes != 0)) return E::unsupported;
    if (v.track.edited || a.track.edited) {
        // A VOD edit list already maps the track into its movie timeline.
        // Rebasing its first packet would erase video composition offsets or
        // AAC priming. Live snapshots still require an unedited shared origin.
        if (live || !v.track.movie_timescale || !a.track.movie_timescale) return E::unsupported;
        video->segmented->synchronized = audio->segmented->synchronized = true;
        return E::none;
    }
    const u64 vt = v.index.first_time, at = a.index.first_time;
    const u64 vs = v.track.timescale, as = a.track.timescale;
    const bool video_first = vt / vs != at / as ? vt / vs < at / as : (vt % vs) * as <= (at % as) * vs;
    const bool choose_video = live ? !video_first : video_first;
    const u64 origin = choose_video ? vt : at, scale = choose_video ? vs : as;
    // One shared rational origin. Round only when converting into the other
    // track's ticks; independently zeroing both tracks would erase their offset.
    const auto converted = [&](u64 destination, i64* output) noexcept {
        constexpr u64 limit = 0x7fffffffffffffffull;
        const u64 seconds = origin / scale;
        if (seconds > limit / destination) return false;
        const u64 whole = seconds * destination, fraction = ((origin % scale) * destination + scale / 2) / scale;
        if (fraction > limit - whole) return false;
        *output = static_cast<i64>(whole + fraction); return true;
    };
    i64 video_origin{}, audio_origin{};
    if (!converted(vs, &video_origin) || !converted(as, &audio_origin)) return E::unsupported;
    if (live) {
        Mp4FragmentSource* sources[]{&v, &a}; const i64 starts[]{video_origin, audio_origin};
        for (u32 i = 0; i < 2; ++i) {
            auto& store = *sources[i]->segments;
            if (store.available_end <= static_cast<u64>(starts[i])) {
                if (store.received_count >= store.count) return E::unsupported;
                return mp4_segment_store_request(&store, store.received_count);
            }
            if (!mp4_fragment_source_refresh(sources[i])) return E::unsupported;
        }
    }
    const i64 old_v = v.track.media_start, old_a = a.track.media_start;
    v.track.media_start = video_origin; a.track.media_start = audio_origin;
    Mp4FragmentAudio packets{};
    if (mp4_fragment_audio_initialize(&a, &packets) != Mp4DemuxError::none) {
        v.track.media_start = old_v; a.track.media_start = old_a; return E::unsupported;
    }
    audio->audio = packets;
    video->segmented->synchronized = audio->segmented->synchronized = true;
    return E::none;
}
u32 segmented_video_buffered(ProgressiveVideoFetch* f, u64 duration, u16* begin, u16* end, u32 capacity) noexcept {
    if (!duration || !begin || !end || !capacity || !mp4_fragment_source_valid(f->fragment)) return 0;
    const auto& slots = f->segmented->store.slots;
    const u32 first = slots[0].number > slots[1].number ? 1 : 0;
    u32 count = 0;
    for (u32 i = 0; i < 2; ++i) {
        const auto& slot = slots[(first + i) % 2];
        if (!slot.bytes) continue;
        const auto& range = slot.media.range;
        const u64 endpoint = range.index + 1 == f->fragment.index.count && f->fragment.presentation_end
            ? f->fragment.presentation_end : range.time + range.duration;
        i64 start{}, finish{};
        if (!mp4_fragment_source_time(f->fragment, static_cast<i64>(range.time), &start) ||
            !mp4_fragment_source_time(f->fragment, static_cast<i64>(endpoint), &finish)) continue;
        const u16 b = static_cast<u16>(minimum<u64>(start < 0 ? 0 : static_cast<u64>(start), duration) * 10000 / duration);
        const u16 e = static_cast<u16>(minimum<u64>(finish < 0 ? 0 : static_cast<u64>(finish), duration) * 10000 / duration);
        if (e <= b) continue;
        if (count && b <= end[count - 1] + 1u) end[count - 1] = maximum(end[count - 1], e);
        else if (count < capacity) { begin[count] = b; end[count++] = e; }
    }
    return count;
}
} // namespace maicord
