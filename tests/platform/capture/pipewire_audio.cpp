// Linux stream audio (src/platform/capture/linux/stream_audio.cpp) against a real PipeWire daemon
// (tools/test_capture_pipewire.sh: a private daemon with a dummy driver and no session manager). Two synthetic players
// in this process play constant tones: one stands for another application (application.process.id 1, level 1000), the
// other for MaiCord (this process's id, level 20000). They start after the capture, so the registry has to bring them
// in. Asserted: the capture links itself to the other application's output only and hears its level on both channels,
// never MaiCord's (alone 20000, mixed 21000); blocks carry timestamps on the client clock; stopping returns.
//
// Skips (77) without MAICORD_PIPEWIRE_TEST (the script's daemon) or libpipewire-0.3.so.0.
#include "../../../src/platform/capture/linux/pipewire_stream.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/platform/audio/stream_audio.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord {
namespace {
using namespace pipewire;

u32 g_failures = 0;
void check(bool condition, const char* label) noexcept {
    test_check(&g_failures, condition, label);
}

// A player: a playback stream of its own on its own loop, writing one constant level.
struct Player {
    const Api* api;
    ThreadLoop* loop;
    Context* context;
    Core* core;
    Stream* stream;
    Hook hook;
    StreamEvents events;
    i16 level;
    alignas(8) u8 format[256];
};
Player g_other, g_maicord;

void player_process(void* data) noexcept {
    auto* p = static_cast<Player*>(data);
    Buffer* buffer = p->api->pw_stream_dequeue_buffer(p->stream);
    if (!buffer)
        return;
    Data& plane = buffer->buffer->datas[0];
    if (plane.data) {
        u32 frames = plane.maxsize / (2 * sizeof(i16));
        frames = frames > 1024 ? 1024 : frames;
        auto* samples = static_cast<i16*>(plane.data);
        for (u32 i = 0; i < frames * 2; ++i)
            samples[i] = p->level;
        plane.chunk->offset = 0;
        plane.chunk->size = frames * 2 * sizeof(i16);
        plane.chunk->stride = 2 * sizeof(i16);
    }
    p->api->pw_stream_queue_buffer(p->stream, buffer);
}

// `client_process` is the client's application.process.id (a native application's, as the other player); a nonzero
// `node_process` also goes on the node, as pipewire-pulse puts the PulseAudio client's process on the nodes it makes
// (MaiCord's own audio reaches PipeWire that way, so the "MaiCord" player carries it there).
bool player_start(Player* p, const Api* api, const char* name, u32 client_process, u32 node_process,
                  i16 level) noexcept {
    p->api = api;
    p->level = level;
    p->events.version = 0;
    p->events.process = player_process;
    p->loop = api->pw_thread_loop_new(name, nullptr);
    if (!p->loop || api->pw_thread_loop_start(p->loop) < 0)
        return false;
    char client[64];
    static constexpr char kClient[] = "application.process.id=";
    copy_bytes(client, kClient, sizeof(kClient) - 1);
    client[sizeof(kClient) - 1 + decimal_write(client + sizeof(kClient) - 1, client_process)] = 0;
    char properties[256];
    // Output ports per channel without a session manager (one would configure them, as it does every stream's).
    static constexpr char kPrefix[] = "media.type=Audio media.category=Playback node.want-driver=true "
                                      "adapter.auto-port-config={ mode=dsp position=preserve }";
    copy_bytes(properties, kPrefix, sizeof(kPrefix) - 1);
    u32 used = sizeof(kPrefix) - 1;
    if (node_process) {
        static constexpr char kNode[] = " application.process.id=";
        copy_bytes(properties + used, kNode, sizeof(kNode) - 1);
        used += sizeof(kNode) - 1;
        used += decimal_write(properties + used, node_process);
    }
    properties[used] = 0;
    api->pw_thread_loop_lock(p->loop);
    p->context = api->pw_context_new(api->pw_thread_loop_get_loop(p->loop), api->pw_properties_new_string(client), 0);
    p->core = p->context ? api->pw_context_connect(p->context, nullptr, 0) : nullptr;
    p->stream = p->core ? api->pw_stream_new(p->core, name, api->pw_properties_new_string(properties)) : nullptr;
    const Pod* format = nullptr;
    bool ok = p->stream && build_audio_format(p->format, sizeof(p->format), &format);
    if (ok) {
        api->pw_stream_add_listener(p->stream, &p->hook, &p->events, p);
        ok = api->pw_stream_connect(p->stream, kDirectionOutput, kAnyId, kStreamMapBuffers, &format, 1) >= 0;
    }
    api->pw_thread_loop_unlock(p->loop);
    return ok;
}

void player_stop(Player* p) noexcept {
    if (!p->loop)
        return;
    p->api->pw_thread_loop_lock(p->loop);
    if (p->stream)
        p->api->pw_stream_destroy(p->stream);
    if (p->core)
        p->api->pw_core_disconnect(p->core);
    p->api->pw_thread_loop_unlock(p->loop);
    p->api->pw_thread_loop_stop(p->loop);
    if (p->context)
        p->api->pw_context_destroy(p->context);
    p->api->pw_thread_loop_destroy(p->loop);
    p->loop = nullptr;
}

struct Heard {
    volatile LONG blocks;
    u64 samples, other, maicord, mixed, silent, odd, stale;
};
Heard g_heard;

void on_block(void* context, ConstSlice<i16> stereo, u64 timestamp_us) noexcept {
    auto* h = static_cast<Heard*>(context);
    const u64 now = clock_microseconds(clock_initialize(), 0, clock_now());
    if (!timestamp_us || timestamp_us > now || timestamp_us + 1'000'000 < now)
        ++h->stale;
    for (u32 i = 0; i < stereo.count; ++i) {
        const i32 sample = stereo[i];
        const auto near = [sample](i32 level) noexcept { return sample >= level - 8 && sample <= level + 8; };
        ++h->samples;
        if (near(1000))
            ++h->other;
        else if (near(20000))
            ++h->maicord;
        else if (near(21000))
            ++h->mixed;
        else if (sample == 0)
            ++h->silent;
        else
            ++h->odd;
    }
    InterlockedIncrement(&h->blocks);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_stream_audio_pipewire_entry) {
    using namespace maicord;
    if (!environment_get("MAICORD_PIPEWIRE_TEST")) {
        test_write_line("SKIP stream_audio_pipewire (no private PipeWire daemon: tools/test_capture_pipewire.sh)");
        test_process_exit(77);
    }
    const Api* api = pipewire::api();
    if (!api) {
        test_write_line("SKIP stream_audio_pipewire (no libpipewire-0.3.so.0)");
        test_process_exit(77);
    }
    check(stream_audio_method() == StreamAudioMethod::application_streams,
          "stream audio: the daemon's socket makes the PipeWire method available");
    StreamAudioCapture capture{};
    check(stream_audio_start(&capture, {StreamAudioScope::everything, 0}, {&g_heard, on_block}),
          "stream audio: the capture connects");
    thread_sleep(200);
    check(player_start(&g_maicord, api, "maicord-test-voice", 2, static_cast<u32>(sys_getpid()), 20000) &&
              player_start(&g_other, api, "maicord-test-other-app", 1, 0, 1000),
          "stream audio: the two players connect after the capture");
    const u64 deadline = clock_monotonic_milliseconds() + 8000;
    while (g_heard.other < 2 * kStreamAudioRate * kStreamAudioChannels && clock_monotonic_milliseconds() < deadline)
        thread_sleep(50);
    stream_audio_stop(&capture);
    player_stop(&g_other);
    player_stop(&g_maicord);
    test_write_metric("samples: ", g_heard.samples);
    test_write_metric("other application: ", g_heard.other);
    test_write_metric("MaiCord alone: ", g_heard.maicord);
    test_write_metric("mixed with MaiCord: ", g_heard.mixed);
    test_write_metric("silent: ", g_heard.silent);
    test_write_metric("other values: ", g_heard.odd);
    check(g_heard.other >= 2 * kStreamAudioRate * kStreamAudioChannels,
          "stream audio: two seconds of the other application on both channels");
    check(!g_heard.maicord && !g_heard.mixed, "stream audio: MaiCord's own output is never heard");
    check(g_heard.odd * 100 <= g_heard.samples, "stream audio: what is heard is the other application's level");
    check(!g_heard.stale, "stream audio: blocks are stamped on the client clock");
    test_exit(g_failures, "stream_audio_pipewire");
}
