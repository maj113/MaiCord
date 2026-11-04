// GPU timers (GL_EXT_disjoint_timer_query), fences (GL sync objects) and shared references of the OpenGL ES backend.
#include "gpu/gles/internal.hpp"
#include "gpu/gles/video_planes_internal.hpp"

namespace maicord {

void gpu_release(GpuContext*& handle) noexcept {
    handle = nullptr; // the context goes with gpu_device_destroy; the signalling interface is the context itself
}

// ---- Shared references ---------------------------------------------------------------------------------------

void gpu_retain(GpuTexture* handle) noexcept {
    if (gles::video_plane_retain(handle))
        return;
    ++handle->refs;
}

void gpu_retain(GpuSrv* handle) noexcept {
    if (gles::video_plane_retain(handle))
        return;
    ++handle->refs;
}

void gpu_retain(GpuDevice* handle) noexcept {
    ++handle->refs;
}

void gpu_release(GpuDevice*& handle) noexcept {
    GpuDevice* device = handle;
    handle = nullptr;
    if (device && device->refs)
        --device->refs; // the device itself is destroyed by gpu_device_destroy
}

// ---- Timers --------------------------------------------------------------------------------------------------

void gpu_release(GpuQuery*& handle) noexcept {
    GpuQuery* query = handle;
    handle = nullptr;
    if (!query)
        return;
    GpuDevice& d = gles::device();
    const u32 names[2] = {query->begin, query->end};
    d.gl.glDeleteQueriesEXT(2, names);
    d.queries.release(query);
}

GpuStatus gpu_timer_create(GpuDevice* device, GpuTimer* timer) {
    timer->query = nullptr;
    if (!device->caps.timer_query)
        return kGpuUnsupported;
    GpuQuery* query = device->queries.allocate();
    if (!query)
        return kGpuOutOfMemory;
    u32 names[2] = {};
    device->gl.glGenQueriesEXT(2, names);
    query->begin = names[0];
    query->end = names[1];
    timer->query = query;
    return kGpuOk;
}

bool gpu_timer_valid(const GpuTimer& timer) {
    return timer.query != nullptr;
}

void gpu_timer_release(GpuTimer* timer) {
    gpu_release(timer->query);
}

void gpu_timer_begin(GpuContext*, GpuTimer* timer) {
    GpuDevice& d = gles::device();
    gl::Int disjoint = 0;
    d.gl.glGetIntegerv(gl::kGpuDisjointExt, &disjoint); // clears the flag: a frame is judged by what happens in it
    d.gl.glQueryCounterEXT(timer->query->begin, gl::kTimestampExt);
}

void gpu_timer_end(GpuContext*, GpuTimer* timer) {
    gles::device().gl.glQueryCounterEXT(timer->query->end, gl::kTimestampExt);
    timer->query->issued = true;
}

GpuPoll gpu_timer_poll(GpuContext*, GpuTimer* timer, bool flush, double* milliseconds) {
    GpuDevice& d = gles::device();
    GpuQuery* query = timer->query;
    if (!query || !query->issued)
        return GpuPoll::pending;
    gl::Uint available = 0;
    d.gl.glGetQueryObjectuivEXT(query->end, gl::kQueryResultAvailableExt, &available);
    if (!available) {
        if (flush)
            d.gl.glFlush();
        return GpuPoll::pending;
    }
    query->issued = false;
    gl::Int disjoint = 0;
    d.gl.glGetIntegerv(gl::kGpuDisjointExt, &disjoint);
    u64 begin = 0, end = 0;
    d.gl.glGetQueryObjectui64vEXT(query->begin, gl::kQueryResultExt, &begin);
    d.gl.glGetQueryObjectui64vEXT(query->end, gl::kQueryResultExt, &end);
    if (disjoint || end < begin)
        return GpuPoll::invalid;
    *milliseconds = static_cast<double>(end - begin) / 1.0e6;
    return GpuPoll::ready;
}

// ---- Fences --------------------------------------------------------------------------------------------------

namespace gles {

void poll_fences() noexcept {
    GpuDevice& d = device();
    if (!d.created)
        return;
    for (GpuFence& fence : d.fences.items) {
        if (!fence.in_use)
            continue;
        // Signals complete in order: take the leading ones the GPU has passed.
        u32 passed = 0;
        while (passed < fence.signal_count) {
            const gl::Enum result = d.gl.glClientWaitSync(fence.syncs[passed], 0, 0);
            if (result != gl::kAlreadySignaled && result != gl::kConditionSatisfied)
                break;
            d.gl.glDeleteSync(fence.syncs[passed]);
            fence.completed = fence.values[passed];
            ++passed;
        }
        if (passed) {
            for (u32 index = passed; index < fence.signal_count; ++index) {
                fence.syncs[index - passed] = fence.syncs[index];
                fence.values[index - passed] = fence.values[index];
            }
            fence.signal_count -= passed;
        }
        u32 kept = 0;
        for (u32 index = 0; index < fence.wait_count; ++index) {
            if (fence.wait_values[index] <= fence.completed) {
                event_set(fence.wait_events[index]);
            } else {
                fence.wait_values[kept] = fence.wait_values[index];
                fence.wait_events[kept] = fence.wait_events[index];
                ++kept;
            }
        }
        fence.wait_count = kept;
    }
}

} // namespace gles

void gpu_release(GpuFence*& handle) noexcept {
    GpuFence* fence = handle;
    handle = nullptr;
    if (!fence)
        return;
    GpuDevice& d = gles::device();
    for (u32 index = 0; index < fence->signal_count; ++index)
        d.gl.glDeleteSync(fence->syncs[index]);
    d.fences.release(fence);
}

GpuStatus gpu_fence_create(GpuDevice* device, GpuContext4** signaller, GpuFence** fence) {
    *signaller = nullptr;
    *fence = nullptr;
    GpuFence* result = device->fences.allocate();
    if (!result)
        return kGpuOutOfMemory;
    result->in_use = true;
    *signaller = &gles::context();
    *fence = result;
    return kGpuOk;
}

GpuStatus gpu_fence_signal(GpuContext4*, GpuFence* fence, u64 value) {
    GpuDevice& d = gles::device();
    if (fence->signal_count == kGlesFenceSignals) {
        // The oldest signal has to be settled to make room: wait for it (flushing), then take what is done.
        d.gl.glClientWaitSync(fence->syncs[0], gl::kSyncFlushCommandsBit, 1000000000ull);
        gles::poll_fences();
        if (fence->signal_count == kGlesFenceSignals)
            return kGpuFail;
    }
    const gl::Sync sync = d.gl.glFenceSync(gl::kSyncGpuCommandsComplete, 0);
    if (!sync)
        return kGpuFail;
    fence->syncs[fence->signal_count] = sync;
    fence->values[fence->signal_count] = value;
    ++fence->signal_count;
    d.gl.glFlush(); // the signal must reach the GPU even if nothing else is queued
    return kGpuOk;
}

GpuStatus gpu_fence_notify(GpuFence* fence, u64 value, Waitable event) {
    if (fence->completed >= value) {
        event_set(event);
        return kGpuOk;
    }
    if (fence->wait_count == kGlesFenceWaits)
        return kGpuFail;
    fence->wait_values[fence->wait_count] = value;
    fence->wait_events[fence->wait_count] = event;
    ++fence->wait_count;
    gles::poll_fences(); // an already finished signal wakes the event now
    return kGpuOk;
}

} // namespace maicord
