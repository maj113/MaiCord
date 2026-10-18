#include "maicord/app/call_video_presenter.hpp"

namespace maicord {
namespace {
const CallVideoTarget* target_for(ConstSlice<CallVideoTarget> targets, u64 user_id) noexcept {
    for (u32 i = 0; i < targets.count; ++i)
        if (targets[i].user_id == user_id && targets[i].ssrc)
            return &targets[i];
    return nullptr;
}
} // namespace

void call_video_presenter_reset(CallVideoPresenter* presenter) noexcept {
    if (!presenter)
        return;
    presenter->count = 0;
    for (auto& stream : presenter->streams)
        stream = {};
}

u32 call_video_reduction_extent(u32 width, u32 height, u32 tile_pixels) noexcept {
    const u32 longest = width > height ? width : height;
    if (!longest)
        return 0;
    u32 step = tile_pixels ? longest / tile_pixels : 1;
    const u32 cap = (longest + kCallVideoMaximumExtent - 1) / kCallVideoMaximumExtent;
    step = step < cap ? cap : step;
    step = step < 1 ? 1 : step > kGpuVideoReductionMaximum ? kGpuVideoReductionMaximum : step;
    return step == 1 ? 0 : (longest + step - 1) / step;
}

CallVideoPumpResult call_video_presenter_pump(CallVideoPresenter* presenter, const CallVideoHost& host,
                                              ConstSlice<CallVideoTarget> targets, bool poll) noexcept {
    CallVideoPumpResult result{};
    if (!presenter || (!targets.data && targets.count))
        return result;
    // Streams whose camera ended are forgotten; the voice server keeps the sink
    // want for the call, so a returning camera starts from an unknown want.
    for (u32 i = 0; i < presenter->count;) {
        const auto* target = target_for(targets, presenter->streams[i].user_id);
        if (target && target->ssrc == presenter->streams[i].ssrc) {
            ++i;
            continue;
        }
        if (presenter->streams[i].ready)
            result.changed = true;
        presenter->streams[i] = presenter->streams[--presenter->count];
        presenter->streams[presenter->count] = {};
    }
    for (u32 t = 0; t < targets.count; ++t) {
        const auto& target = targets[t];
        if (!target.user_id || !target.ssrc || target_for(targets, target.user_id) != &target)
            continue;
        CallVideoStream* stream = nullptr;
        for (u32 i = 0; i < presenter->count && !stream; ++i)
            if (presenter->streams[i].user_id == target.user_id)
                stream = &presenter->streams[i];
        if (!stream) {
            if (presenter->count == kCallVideoStreams)
                continue;
            stream = &presenter->streams[presenter->count++];
            *stream = {target.user_id, target.ssrc, 0, 0, 0, 0, CallVideoWant::unknown, false};
        }
        const auto want = target.visible ? CallVideoWant::wanted : CallVideoWant::unwanted;
        if (stream->want != want) {
            if (host.wanted && host.wanted(host.context, target.user_id, target.visible))
                stream->want = want;
            else
                result.want_pending = true;
        }
        if (!target.visible)
            continue;
        result.visible = true;
        if (!poll || !host.frame)
            continue;
        VideoStreamFrame frame{};
        if (!host.frame(host.context, target.ssrc, &frame))
            continue;
        if (frame.gpu.texture)
            ++presenter->acquired;
        const u32 width = frame.display_width ? frame.display_width : frame.gpu.width;
        const u32 height = frame.display_height ? frame.display_height : frame.gpu.height;
        const u32 extent = call_video_reduction_extent(frame.gpu.width, frame.gpu.height, target.tile_pixels);
        const bool resident = !host.resident || host.resident(host.context, target.user_id, target.ssrc);
        const bool fresh = !stream->ready || frame.sequence != stream->sequence || extent != stream->extent ||
                           !resident;
        if (fresh && frame.gpu.texture && host.present &&
            host.present(host.context, target.user_id, target.ssrc, frame, extent)) {
            result.presented = true;
            if (!stream->ready || stream->display_width != width || stream->display_height != height)
                result.changed = true;
            stream->ready = true;
            stream->sequence = frame.sequence;
            stream->extent = extent;
            stream->display_width = static_cast<u16>(width);
            stream->display_height = static_cast<u16>(height);
        }
        if (frame.gpu.texture) {
            if (host.release)
                host.release(host.context, &frame);
            ++presenter->released;
        }
    }
    return result;
}

const CallVideoStream* call_video_presenter_find(const CallVideoPresenter& presenter, u64 user_id) noexcept {
    for (u32 i = 0; user_id && i < presenter.count; ++i)
        if (presenter.streams[i].user_id == user_id)
            return &presenter.streams[i];
    return nullptr;
}

u32 call_video_unlisted_users(const VoiceStateStore& states, u64 channel_id, ConstSlice<CallVideoTarget> cameras,
                              Slice<u64> output) noexcept {
    if (!channel_id || voice_state_store_channel_count(states, channel_id))
        return 0;
    u32 count = 0;
    for (u32 i = 0; i < cameras.count && count < output.count; ++i) {
        const u64 user = cameras[i].user_id;
        bool listed = !user;
        for (u32 j = 0; j < count && !listed; ++j)
            listed = output[j] == user;
        if (!listed)
            output[count++] = user;
    }
    return count;
}

bool call_video_roster_changed(ConstSlice<CallVideoTarget> before, ConstSlice<CallVideoTarget> after) noexcept {
    if (before.count != after.count)
        return true;
    for (u32 i = 0; i < before.count; ++i)
        if (before[i].user_id != after[i].user_id || before[i].ssrc != after[i].ssrc)
            return true;
    return false;
}
} // namespace maicord
