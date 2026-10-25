#include "maicord/render/d3d11_gpu_timer.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/render/gpu_timing.hpp"

#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {

constexpr D3D11GpuTimerResult timer_success() noexcept {
    return D3D11GpuTimerResult{S_OK, D3D11GpuTimerError::none};
}

constexpr D3D11GpuTimerResult timer_failure(D3D11GpuTimerError error, HRESULT native_error) noexcept {
    return D3D11GpuTimerResult{native_error, error};
}

void release_queries(D3D11GpuTimerOwner* owner) noexcept {
    for (u32 index = 0; index < kD3D11GpuTimerFrameCount; ++index) {
        com_release(owner->frames[index].end);
        com_release(owner->frames[index].begin);
        com_release(owner->frames[index].disjoint);
        owner->frames[index].pending = false;
    }
}

bool owns_frame(const D3D11GpuTimerOwner& owner, const D3D11GpuTimerFrame* frame) noexcept {
    for (u32 index = 0; index < kD3D11GpuTimerFrameCount; ++index) {
        if (frame == &owner.frames[index]) {
            return true;
        }
    }
    return false;
}

} // namespace

D3D11GpuTimerResult d3d11_gpu_timer_initialize(D3D11GpuTimerOwner* owner, ID3D11Device* device,
                                               bool enabled) noexcept {
    if (!owner || (enabled && !device)) {
        return timer_failure(D3D11GpuTimerError::invalid_input, E_INVALIDARG);
    }

    D3D11GpuTimerOwner staged{};
    staged.enabled = enabled;
    if (enabled) {
        const D3D11_QUERY_DESC disjoint_description{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        const D3D11_QUERY_DESC timestamp_description{D3D11_QUERY_TIMESTAMP, 0};
        for (u32 index = 0; index < kD3D11GpuTimerFrameCount; ++index) {
            D3D11GpuTimerFrame& frame = staged.frames[index];
            HRESULT result = device->CreateQuery(&disjoint_description, &frame.disjoint);
            D3D11GpuTimerError error = D3D11GpuTimerError::disjoint_query;
            if (SUCCEEDED(result)) {
                result = device->CreateQuery(&timestamp_description, &frame.begin);
                error = D3D11GpuTimerError::begin_query;
            }
            if (SUCCEEDED(result)) {
                result = device->CreateQuery(&timestamp_description, &frame.end);
                error = D3D11GpuTimerError::end_query;
            }
            if (FAILED(result)) {
                release_queries(&staged);
                return timer_failure(error, result);
            }
        }
    }

    d3d11_gpu_timer_release(owner, nullptr);
    *owner = staged;
    return timer_success();
}

void d3d11_gpu_timer_collect(D3D11GpuTimerOwner* owner, ID3D11DeviceContext* context) noexcept {
    if (!owner || !owner->enabled || !context) {
        return;
    }
    for (u32 index = 0; index < kD3D11GpuTimerFrameCount; ++index) {
        D3D11GpuTimerFrame& frame = owner->frames[index];
        if (!frame.pending) {
            continue;
        }
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        u64 begin = 0;
        u64 end = 0;
        const HRESULT disjoint_result =
            context->GetData(frame.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (disjoint_result == S_FALSE) {
            continue;
        }
        if (FAILED(disjoint_result)) {
            frame.pending = false;
            saturating_increment(owner->errors);
            continue;
        }
        const HRESULT begin_result =
            context->GetData(frame.begin, &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const HRESULT end_result =
            context->GetData(frame.end, &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (begin_result == S_FALSE || end_result == S_FALSE) {
            continue;
        }
        frame.pending = false;
        if (FAILED(begin_result) || FAILED(end_result)) {
            saturating_increment(owner->errors);
            continue;
        }
        if (disjoint.Disjoint || disjoint.Frequency == 0 ||
            disjoint.Frequency > ~0ull / kGpuTimestampScale || end < begin) {
            saturating_increment(owner->disjoint);
            continue;
        }
        const u64 elapsed = gpu_timestamp_microseconds(end - begin, disjoint.Frequency);
        owner->last_microseconds = elapsed;
        saturating_accumulate(owner->total_microseconds, elapsed);
        owner->maximum_microseconds = maximum(owner->maximum_microseconds, elapsed);
        saturating_increment(owner->samples);
    }
}

D3D11GpuTimerFrame* d3d11_gpu_timer_begin(D3D11GpuTimerOwner* owner,
                                          ID3D11DeviceContext* context) noexcept {
    if (!owner || !owner->enabled || !context) {
        return nullptr;
    }
    d3d11_gpu_timer_collect(owner, context);
    for (u32 offset = 0; offset < kD3D11GpuTimerFrameCount; ++offset) {
        const u32 index = (owner->cursor + offset) % kD3D11GpuTimerFrameCount;
        D3D11GpuTimerFrame& frame = owner->frames[index];
        if (frame.pending) {
            continue;
        }
        context->Begin(frame.disjoint);
        context->End(frame.begin);
        owner->cursor = (index + 1) % kD3D11GpuTimerFrameCount;
        return &frame;
    }
    saturating_increment(owner->skipped);
    return nullptr;
}

void d3d11_gpu_timer_end(D3D11GpuTimerOwner* owner, ID3D11DeviceContext* context,
                         D3D11GpuTimerFrame* frame) noexcept {
    if (!owner || !owner->enabled || !context || !frame || !owns_frame(*owner, frame) || frame->pending) {
        return;
    }
    context->End(frame->end);
    context->End(frame->disjoint);
    frame->pending = true;
}

void d3d11_gpu_timer_release(D3D11GpuTimerOwner* owner, ID3D11DeviceContext* context) noexcept {
    if (!owner) {
        return;
    }
    d3d11_gpu_timer_collect(owner, context);
    release_queries(owner);
    reset_value<D3D11GpuTimerOwner>(owner);
}

} // namespace maicord
