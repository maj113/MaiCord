// The backend's lifetime and synchronization surface on a real driver: a device made by the prestart worker (the
// continuation runs on the worker, the main thread adopts the device and takes the context over), views that keep
// their texture alive after its handle is released, shared references (gpu_retain), GPU timers
// (GL_EXT_disjoint_timer_query, where the driver has it; else the create must fail the way a device without timers
// does) and fences (GL sync objects waking an event). MAICORD_TEST_PRESTART=0 skips the prestart half so the second ctest
// run covers the plain creation of the same checks.
#include "maicord/core/log.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../support/test_output.hpp"

namespace maicord {
namespace {

u32 g_failures = 0;
GpuTexture* g_worker_texture;
GpuRtv* g_worker_view;
u32 g_worker_ran;

void check(bool condition, const char* label) noexcept {
    test_check(&g_failures, condition, label);
}

// Runs on the prestart worker: the context is current there. Whatever it makes the main thread reads later.
void worker_continuation(GpuDevice* device, GpuContext* context) noexcept {
    const float colour[4]{0.0f, 1.0f, 0.0f, 1.0f};
    if (gpu_ok(gpu_texture_create(device, {8, 8, GpuFormat::rgba8, GpuUsage::default_, gpu_bind_srv | gpu_bind_rtv}, nullptr,
                                  &g_worker_texture)) &&
        gpu_ok(gpu_rtv_create(device, g_worker_texture, &g_worker_view))) {
        gpu_clear_rtv(context, g_worker_view, colour);
        gpu_flush(context);
        g_worker_ran = 1;
    }
}

void test_views_keep_textures(GpuDevice* device, GpuContext* context) noexcept {
    GpuTexture* texture = nullptr;
    GpuRtv* view = nullptr;
    GpuSrv* sampled = nullptr;
    check(gpu_ok(gpu_texture_create(device, {4, 4, GpuFormat::rgba8, GpuUsage::default_, gpu_bind_srv | gpu_bind_rtv}, nullptr,
                                    &texture)) &&
              gpu_ok(gpu_rtv_create(device, texture, &view)) &&
              gpu_ok(gpu_srv_create(device, texture, {GpuViewKind::texture, GpuFormat::rgba8}, &sampled)),
          "a texture with its views");
    if (!texture || !view || !sampled)
        return;
    // The pass executors release a texture right after making its views; the views keep it, and a retain is one more
    // owner. The last release of any owner destroys it.
    GpuTexture* kept = texture;
    gpu_retain(kept);
    gpu_release(texture); // the creator's
    const float red[4]{1.0f, 0.0f, 0.0f, 1.0f};
    gpu_clear_rtv(context, view, red);
    u8 pixels[4 * 4 * 4]{};
    check(gpu_ok(gpu_readback(device, context, kept, {0, 0, 4, 4}, pixels, 16)) && pixels[0] == 255 && pixels[1] == 0 &&
              pixels[2] == 0 && pixels[3] == 255,
          "the texture lives on through its views and a retain");
    gpu_release(view);
    gpu_release(sampled);
    const float blue[4]{0.0f, 0.0f, 1.0f, 1.0f};
    GpuRtv* again = nullptr;
    check(gpu_ok(gpu_rtv_create(device, kept, &again)), "a view of the retained texture");
    gpu_clear_rtv(context, again, blue);
    check(gpu_ok(gpu_readback(device, context, kept, {0, 0, 4, 4}, pixels, 16)) && pixels[2] == 255,
          "the retained texture is still drawn into");
    gpu_release(again);
    gpu_release(kept); // the last owner
    // A buffer the same way: a raw view outlives its handle.
    GpuBuffer* buffer = nullptr;
    GpuSrv* raw = nullptr;
    check(gpu_ok(gpu_buffer_create(device, {64, GpuUsage::default_, gpu_bind_srv, true}, nullptr, &buffer)) &&
              gpu_ok(gpu_srv_create(device, buffer, {GpuViewKind::buffer_raw, GpuFormat::r32_uint}, &raw)),
          "a buffer with a view");
    gpu_release(buffer);
    gpu_release(raw);
}

void test_timer(GpuDevice* device, GpuContext* context) noexcept {
    GpuTimer timer{};
    const GpuStatus status = gpu_timer_create(device, &timer);
    if (!gpu_caps(device).timer_query) {
        check(!gpu_ok(status) && !gpu_timer_valid(timer), "without timer queries the create fails");
        test_write_line("timer queries: not offered by this driver");
        return;
    }
    check(gpu_ok(status) && gpu_timer_valid(timer), "a timer is created");
    GpuTexture* texture = nullptr;
    GpuRtv* view = nullptr;
    if (!gpu_ok(gpu_texture_create(device, {64, 64, GpuFormat::rgba8, GpuUsage::default_, gpu_bind_rtv}, nullptr, &texture)) ||
        !gpu_ok(gpu_rtv_create(device, texture, &view))) {
        check(false, "the timed target");
        return;
    }
    const float colour[4]{0.2f, 0.4f, 0.6f, 1.0f};
    gpu_timer_begin(context, &timer);
    gpu_clear_rtv(context, view, colour);
    gpu_timer_end(context, &timer);
    double milliseconds = -1.0;
    GpuPoll polled = GpuPoll::pending;
    for (u32 attempt = 0; attempt < 200 && polled == GpuPoll::pending; ++attempt) {
        polled = gpu_timer_poll(context, &timer, true, &milliseconds);
        if (polled == GpuPoll::pending)
            thread_sleep(5);
    }
    // A disjoint GPU (a clock that jumped) is a legal answer; a timer that never answers is not.
    check(polled == GpuPoll::ready || polled == GpuPoll::invalid, "the timer answers");
    check(polled != GpuPoll::ready || (milliseconds >= 0.0 && milliseconds < 1000.0), "the time is plausible");
    gpu_timer_release(&timer);
    check(!gpu_timer_valid(timer), "the timer is released");
    gpu_release(view);
    gpu_release(texture);
}

void test_fence(GpuDevice* device, GpuContext* context) noexcept {
    GpuContext4* signaller = nullptr;
    GpuFence* fence = nullptr;
    check(gpu_ok(gpu_fence_create(device, &signaller, &fence)) && signaller && fence, "a fence is created");
    if (!fence)
        return;
    Waitable first = event_create(false, false);
    Waitable second = event_create(false, false);
    check(first && second, "events");
    // Armed before the signal: the event fires once the GPU passes the value, noticed at a flush.
    check(gpu_ok(gpu_fence_notify(fence, 1, first)), "a wait is armed");
    check(gpu_ok(gpu_fence_signal(signaller, fence, 1)), "the fence is signalled");
    bool woke = false;
    for (u32 attempt = 0; attempt < 400 && !woke; ++attempt) {
        gpu_flush(context);
        woke = wait_one(first, 5) == WAIT_OBJECT_0;
    }
    check(woke, "the event wakes when the GPU passes the value");
    // A value already passed wakes at once.
    check(gpu_ok(gpu_fence_notify(fence, 1, second)) && wait_one(second, 1000) == WAIT_OBJECT_0,
          "a passed value wakes its event at once");
    // Values in order.
    check(gpu_ok(gpu_fence_signal(signaller, fence, 2)) && gpu_ok(gpu_fence_signal(signaller, fence, 3)) &&
              gpu_ok(gpu_fence_notify(fence, 3, second)),
          "more values are signalled and waited for");
    woke = false;
    for (u32 attempt = 0; attempt < 400 && !woke; ++attempt) {
        gpu_flush(context);
        woke = wait_one(second, 5) == WAIT_OBJECT_0;
    }
    check(woke, "the last value wakes its event");
    waitable_close(first);
    waitable_close(second);
    gpu_release(fence);
    gpu_release(signaller);
}

// A prestart nobody adopts (the window could not be made, so the renderer never started): the worker's objects are
// released before the device that finish destroys, which used to call a GL function of a device already gone.
void test_prestart_cancel() noexcept {
    g_worker_ran = 0;
    g_worker_texture = nullptr;
    g_worker_view = nullptr;
    gpu_device_prestart(worker_continuation);
    gpu_device_prestart_wait();
    check(g_worker_ran == 1 && g_worker_texture && g_worker_view, "the worker's objects exist after the wait");
    gpu_release(g_worker_view);
    gpu_release(g_worker_texture);
    gpu_device_prestart_finish();
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    check(gpu_ok(gpu_device_create(&device, &context)) && !gpu_device_prestarted(device),
          "a device is made again after a prestart nobody adopted");
    if (device)
        gpu_device_destroy(&device, &context);
}

void run() noexcept {
    const char* prestart_setting = environment_get("MAICORD_TEST_PRESTART");
    const bool prestart = !(prestart_setting && prestart_setting[0] == '0');
    if (prestart)
        gpu_device_prestart(worker_continuation);
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    if (!gpu_ok(gpu_device_create(&device, &context))) {
        check(false, "device creation");
        return;
    }
    test_write_text("device: ");
    test_write_line(gpu_device_description(device));
    if (prestart) {
        check(gpu_device_prestarted(device), "the device came from the prestart worker");
        gpu_device_prestart_finish();
        check(g_worker_ran == 1, "the continuation ran on the worker");
        // The worker drew into a texture; the main thread owns the context now and reads it.
        u8 pixels[8 * 8 * 4]{};
        check(g_worker_texture && gpu_ok(gpu_readback(device, context, g_worker_texture, {0, 0, 8, 8}, pixels, 32)) &&
                  pixels[0] == 0 && pixels[1] == 255 && pixels[2] == 0 && pixels[3] == 255,
              "what the worker drew is read on the main thread");
        gpu_release(g_worker_view);
        gpu_release(g_worker_texture);
    } else {
        check(!gpu_device_prestarted(device), "a device made here is not a prestarted one");
    }
    test_views_keep_textures(device, context);
    test_timer(device, context);
    test_fence(device, context);
    gpu_device_destroy(&device, &context);
    if (prestart)
        test_prestart_cancel();
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_gpu_sync_entry) {
    maicord::log_open();
    maicord::run();
    maicord::test_exit(maicord::g_failures, "maicord_gpu_sync");
}
