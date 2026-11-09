// Linux capture's PipeWire consumer (src/platform/capture/linux/pipewire.cpp) against a real PipeWire daemon with a
// synthetic source in place of the compositor (tools/test_capture_pipewire.sh starts a private daemon and links the two
// nodes, as the session manager does for the portal). The producer here offers packed RGB as dma-bufs with the modifier
// left to the consumer, the way mutter and KWin do; its buffers are dumb buffers of /dev/dri/card0 (vgem in WSL or a
// VM) that it fills with a pattern per frame and stamps with CLOCK_MONOTONIC. Asserted: the consumer fixes the linear
// modifier, and each frame it hands over is that dma-buf (the same file), as XR24 with the producer's size, stride
// and timestamp, holding the pattern of its frame. When the source goes, frames stop and the consumer closes cleanly
// (the end of a real capture comes from the portal's Session.Closed or the session manager unlinking the stream).
//
// Skips (77) without MAICORD_PIPEWIRE_TEST (the script's daemon), libpipewire-0.3.so.0 or a dumb-buffer device.
#include "../../../src/platform/capture/linux/pipewire_stream.hpp"
#include "../../support/dumb_dmabuf.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord {
namespace {
using namespace pipewire;

constexpr u32 kWidth = 64, kHeight = 32, kBuffers = 4;
constexpr u32 kDirectionOutput = 1;                  // PW_DIRECTION_OUTPUT
constexpr u32 kFlagDriver = 8, kFlagAllocBuffers = 256; // PW_STREAM_FLAG_DRIVER, PW_STREAM_FLAG_ALLOC_BUFFERS
constexpr u32 kDataReadable = 1;                     // SPA_DATA_FLAG_READABLE

u32 g_failures = 0;
void check(bool condition, const char* label) noexcept {
    test_check(&g_failures, condition, label);
}

u64 inode_of(int fd) noexcept {
    Statx status{};
    return sys_statx(fd, "", kAtEmptyPath, 0x100 /* STATX_INO */, &status) == 0 ? status.inode : 0;
}

// The producer: a driver stream of its own on its own loop, standing in for the compositor's.
struct Producer {
    const Api* api;
    int (*trigger)(Stream*);
    ThreadLoop* loop;
    Context* context;
    Core* core;
    Stream* stream;
    Hook hook;
    StreamEvents events;
    i32 node;
    DumbBuffer buffers[kBuffers];
    u64 inodes[kBuffers];
    u32 buffer_count;
    volatile LONG negotiated_modifier_fixed;
    volatile LONG frames_sent;
    alignas(8) u8 pods[2048];
};
Producer g_producer;

u32 pattern(u32 frame) noexcept {
    return 0xFF000000u | (frame * 0x010203u & 0x00FFFFFFu);
}

void producer_param(void* data, u32 id, const Pod* param) noexcept {
    auto* p = static_cast<Producer*>(data);
    VideoFormat format{};
    if (id != kParamFormat || !param || !parse_video_format(param, &format))
        return;
    if (format.modifier_choice)
        return; // the consumer fixes it
    InterlockedExchange(&p->negotiated_modifier_fixed, format.modifier == 0 ? 1 : 2);
    const Pod* pods[2];
    const u32 count = build_buffer_params(p->pods, sizeof(p->pods), pods);
    p->api->pw_stream_update_params(p->stream, pods, count);
}

void producer_add_buffer(void* data, Buffer* buffer) noexcept {
    auto* p = static_cast<Producer*>(data);
    if (p->buffer_count >= kBuffers || !buffer->buffer || !buffer->buffer->n_datas)
        return;
    const u32 index = p->buffer_count++;
    DumbBuffer& dumb = p->buffers[index];
    Data& plane = buffer->buffer->datas[0];
    const i32 node = static_cast<i32>(p->node);
    if (!dumb.pixels && !dumb_create(node, kWidth, kHeight, 32, &dumb))
        return;
    const i32 fd = dumb_export(node, dumb);
    p->inodes[index] = inode_of(fd);
    plane.type = kDataDmaBuf;
    plane.flags = kDataReadable;
    plane.fd = fd;
    plane.mapoffset = 0;
    plane.maxsize = static_cast<u32>(dumb.size);
    plane.data = nullptr;
    buffer->user_data = &dumb;
}

void producer_process(void* data) noexcept {
    auto* p = static_cast<Producer*>(data);
    Buffer* buffer = p->api->pw_stream_dequeue_buffer(p->stream);
    if (!buffer)
        return;
    auto* dumb = static_cast<DumbBuffer*>(buffer->user_data);
    SpaBuffer* spa = buffer->buffer;
    const u32 frame = static_cast<u32>(InterlockedIncrement(&p->frames_sent));
    if (dumb && spa->n_datas) {
        for (u32 y = 0; y < kHeight; ++y)
            for (u32 x = 0; x < kWidth; ++x)
                reinterpret_cast<u32*>(dumb->pixels + y * dumb->pitch)[x] = pattern(frame);
        Chunk* chunk = spa->datas[0].chunk;
        chunk->offset = 0;
        chunk->size = dumb->pitch * kHeight;
        chunk->stride = static_cast<i32>(dumb->pitch);
        chunk->flags = 0;
    }
    for (u32 i = 0; i < spa->n_metas; ++i)
        if (spa->metas[i].type == kMetaHeader && spa->metas[i].size >= sizeof(MetaHeader)) {
            auto* header = static_cast<MetaHeader*>(spa->metas[i].data);
            header->flags = 0;
            header->pts = clock_now(); // CLOCK_MONOTONIC nanoseconds, as compositors stamp
            header->seq = frame;
        }
    p->api->pw_stream_queue_buffer(p->stream, buffer);
}

bool producer_start(Producer* p, i32 drm) noexcept {
    p->node = drm;
    p->events.version = 0;
    p->events.param_changed = producer_param;
    p->events.add_buffer = producer_add_buffer;
    p->events.process = producer_process;
    p->loop = p->api->pw_thread_loop_new("maicord-test-producer", nullptr);
    if (!p->loop || p->api->pw_thread_loop_start(p->loop) < 0)
        return false;
    p->api->pw_thread_loop_lock(p->loop);
    p->context = p->api->pw_context_new(p->api->pw_thread_loop_get_loop(p->loop), nullptr, 0);
    p->core = p->context ? p->api->pw_context_connect(p->context, nullptr, 0) : nullptr;
    p->stream = p->core ? p->api->pw_stream_new(p->core, "maicord-test-producer",
                                                p->api->pw_properties_new_string(
                                                    "media.class=Video/Source node.name=maicord-test-producer"))
                        : nullptr;
    bool ok = p->stream != nullptr;
    if (ok) {
        p->api->pw_stream_add_listener(p->stream, &p->hook, &p->events, p);
        const Pod* pods[4];
        const u32 count = build_enum_formats(p->pods, sizeof(p->pods), pods, 60, false, 0, 0);
        // A compositor offers its output's size alone: the range default, minimum and maximum become 64x32.
        auto* words = reinterpret_cast<u32*>(p->pods);
        for (u32 i = 0; i + 5 < sizeof(p->pods) / 4; ++i)
            if (words[i] == 1920 && words[i + 1] == 1080 && words[i + 4] == 16384 && words[i + 5] == 16384) {
                const u32 fixed[6]{kWidth, kHeight, kWidth, kHeight, kWidth, kHeight};
                copy_bytes(words + i, fixed, sizeof(fixed));
            }
        ok = p->api->pw_stream_connect(p->stream, kDirectionOutput, kAnyId, kFlagDriver | kFlagAllocBuffers, pods,
                                       count) >= 0;
    }
    p->api->pw_thread_loop_unlock(p->loop);
    return ok;
}

void producer_trigger(Producer* p) noexcept {
    p->api->pw_thread_loop_lock(p->loop);
    p->trigger(p->stream);
    p->api->pw_thread_loop_unlock(p->loop);
}

void producer_stop(Producer* p) noexcept {
    if (!p->loop)
        return;
    p->api->pw_thread_loop_lock(p->loop);
    if (p->stream)
        p->api->pw_stream_destroy(p->stream);
    p->stream = nullptr;
    if (p->core)
        p->api->pw_core_disconnect(p->core);
    p->core = nullptr;
    p->api->pw_thread_loop_unlock(p->loop);
    p->api->pw_thread_loop_stop(p->loop);
    if (p->context)
        p->api->pw_context_destroy(p->context);
    p->api->pw_thread_loop_destroy(p->loop);
    p->loop = nullptr;
}

// What the consumer handed over, checked on PipeWire's thread inside the frame callback.
struct Received {
    volatile LONG frames, wrong_file, wrong_layout, wrong_time, wrong_pixels;
    u64 last_timestamp;
};
Received g_received;

void on_frame(void* context, const CaptureFrame& frame) noexcept {
    auto* r = static_cast<Received*>(context);
    const u64 inode = inode_of(frame.plane.fd);
    bool known = false;
    u32 index = 0;
    for (u32 i = 0; i < g_producer.buffer_count; ++i)
        if (inode && inode == g_producer.inodes[i]) {
            known = true;
            index = i;
        }
    const DumbBuffer& dumb = g_producer.buffers[index];
    if (!known)
        InterlockedIncrement(&r->wrong_file);
    if (frame.plane.fourcc != 0x34325258 || frame.width != kWidth || frame.height != kHeight ||
        frame.plane.width != kWidth || frame.plane.height != kHeight || frame.plane.pitch != dumb.pitch ||
        frame.plane.offset != 0 || frame.plane.modifier != 0 || !frame.cursor)
        InterlockedIncrement(&r->wrong_layout);
    const u64 now = clock_microseconds(clock_initialize(), 0, clock_now());
    if (!frame.timestamp_us || frame.timestamp_us > now || frame.timestamp_us + 1'000'000 < now ||
        frame.timestamp_us < r->last_timestamp)
        InterlockedIncrement(&r->wrong_time);
    r->last_timestamp = frame.timestamp_us;
    // The dma-buf itself, mapped from the descriptor the consumer got: every pixel holds one frame's pattern.
    const long mapped = sys_mmap_file(frame.plane.fd, static_cast<usize>(dumb.size), 1 /* PROT_READ */, kMapShared);
    bool uniform = !syscall_failed(mapped);
    if (uniform) {
        const auto* pixels = reinterpret_cast<const u32*>(mapped);
        const u32 first = pixels[0];
        uniform = (first >> 24) == 0xFF;
        for (u32 y = 0; uniform && y < kHeight; ++y)
            for (u32 x = 0; x < kWidth; ++x)
                uniform = uniform && reinterpret_cast<const u32*>(reinterpret_cast<const u8*>(pixels) + y * dumb.pitch)[x] == first;
        sys_munmap(reinterpret_cast<void*>(mapped), static_cast<usize>(dumb.size));
    }
    if (!uniform)
        InterlockedIncrement(&r->wrong_pixels);
    InterlockedIncrement(&r->frames);
}
void on_ended(void*, CaptureEnd) noexcept {}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_capture_pipewire_entry) {
    using namespace maicord;
    if (!environment_get("MAICORD_PIPEWIRE_TEST")) {
        test_write_line("SKIP capture_pipewire (no private PipeWire daemon: tools/test_capture_pipewire.sh)");
        test_process_exit(77);
    }
    const Api* api = pipewire::api();
    const i32 drm = dumb_open();
    if (!api || drm < 0) {
        test_write_line(api ? "SKIP capture_pipewire (no /dev/dri/card0 for dumb buffers)"
                            : "SKIP capture_pipewire (no libpipewire-0.3.so.0)");
        test_process_exit(77);
    }
    SharedLibrary library{};
    shared_library_open(&library, "libpipewire-0.3.so.0");
    g_producer.api = api;
    g_producer.trigger = reinterpret_cast<int (*)(Stream*)>(shared_library_symbol(library, "pw_stream_trigger_process"));
    check(g_producer.trigger != nullptr, "pipewire: the driver's trigger");
    check(producer_start(&g_producer, drm), "pipewire: the synthetic source connects to the daemon");

    const Waitable ended = event_create(true, false);
    Consumer* consumer = consumer_open(-1, kAnyId, true, 0, {&g_received, on_frame, on_ended, nullptr}, ended);
    check(consumer != nullptr, "pipewire: the consumer connects to the daemon");
    // The script links the nodes once both exist; then each trigger is one frame.
    const u64 deadline = clock_monotonic_milliseconds() + 8000;
    while (consumer && g_producer.trigger && ReadAcquire(&g_received.frames) < 20 &&
           clock_monotonic_milliseconds() < deadline) {
        producer_trigger(&g_producer);
        thread_sleep(15);
    }
    const LONG frames = ReadAcquire(&g_received.frames);
    check(consumer && consumer_linked(consumer), "pipewire: the stream reached the source and settled a format");
    check(ReadAcquire(&g_producer.negotiated_modifier_fixed) == 1, "pipewire: the consumer fixed the linear modifier");
    check(frames >= 20, "pipewire: frames arrive as the source sends them");
    check(!g_received.wrong_file, "pipewire: each frame is one of the source's dma-bufs, never a copy");
    check(!g_received.wrong_layout, "pipewire: XR24 at the source's size, stride and modifier, with the cursor flag");
    check(!g_received.wrong_time, "pipewire: timestamps are the source's, on the client clock, never going back");
    check(!g_received.wrong_pixels, "pipewire: the dma-buf holds the frame the source wrote");

    // The source goes: frames stop, nothing fails, and the consumer closes.
    producer_stop(&g_producer);
    wait_one(ended, 500);
    const LONG after_stop = ReadAcquire(&g_received.frames);
    thread_sleep(200);
    const StreamEnd end = consumer ? consumer_end(consumer) : StreamEnd::failed;
    check(ReadAcquire(&g_received.frames) == after_stop && (end == StreamEnd::none || end == StreamEnd::closed),
          "pipewire: no frame and no failure once the source goes");
    consumer_close(consumer);
    if (frames < 20)
        test_write_metric("frames received: ", static_cast<u64>(frames));
    for (DumbBuffer& buffer : g_producer.buffers)
        dumb_destroy(drm, &buffer);
    test_exit(g_failures, "capture_pipewire");
}
