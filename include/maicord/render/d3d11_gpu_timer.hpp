#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Query;

namespace maicord {

inline constexpr u32 kD3D11GpuTimerFrameCount = 2;

enum class D3D11GpuTimerError : u8 {
    none,
    invalid_input,
    disjoint_query,
    begin_query,
    end_query,
};

struct D3D11GpuTimerResult {
    HRESULT native_error;
    D3D11GpuTimerError error;

    constexpr bool ok() const noexcept {
        return error == D3D11GpuTimerError::none;
    }
};

struct D3D11GpuTimerFrame {
    ID3D11Query* disjoint;
    ID3D11Query* begin;
    ID3D11Query* end;
    bool pending;
};

// Owns the bounded asynchronous timestamp ring and its diagnostic projection.
// Collection always uses DONOTFLUSH, so enabling metrics cannot force the GPU
// to complete work on the UI thread.
struct D3D11GpuTimerOwner {
    D3D11GpuTimerFrame frames[kD3D11GpuTimerFrameCount];
    u64 total_microseconds;
    u64 maximum_microseconds;
    u64 last_microseconds;
    u32 cursor;
    u32 samples;
    u32 skipped;
    u32 disjoint;
    u32 errors;
    bool enabled;
};

static_assert(sizeof(D3D11GpuTimerResult) == 8);
static_assert(sizeof(D3D11GpuTimerFrame) == 32);
static_assert(sizeof(D3D11GpuTimerOwner) == 112);

D3D11GpuTimerResult d3d11_gpu_timer_initialize(D3D11GpuTimerOwner* owner, ID3D11Device* device,
                                               bool enabled) noexcept;
D3D11GpuTimerFrame* d3d11_gpu_timer_begin(D3D11GpuTimerOwner* owner, ID3D11DeviceContext* context) noexcept;
void d3d11_gpu_timer_end(D3D11GpuTimerOwner* owner, ID3D11DeviceContext* context,
                         D3D11GpuTimerFrame* frame) noexcept;
void d3d11_gpu_timer_collect(D3D11GpuTimerOwner* owner, ID3D11DeviceContext* context) noexcept;
void d3d11_gpu_timer_release(D3D11GpuTimerOwner* owner, ID3D11DeviceContext* context) noexcept;

} // namespace maicord
