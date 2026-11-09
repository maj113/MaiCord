// The PipeWire consumer of Linux screen capture (pipewire_stream.hpp).
//
// Negotiation: the consumer offers packed 32-bit RGB (BGRx, BGRA, RGBx, RGBA) as dma-bufs only, with the linear
// modifier and the implicit one (DRM_FORMAT_MOD_INVALID) as a mandatory choice the producer fixes, the PipeWire
// dma-buf protocol: a producer that leaves the choice open gets the format again with the modifier it picked fixed.
// Linear and implicit buffers are what every importer reads (VA-API's DRM PRIME surfaces, EGL) without asking the
// renderer for its modifier list. A compositor that can only give shared memory gets no format: the stream fails, and
// capture ends as unsupported rather than copying pictures through the CPU.
// Buffers carry the header meta, whose pts (CLOCK_MONOTONIC nanoseconds on mutter, KWin and wlroots) is the frame's
// timestamp, the clock clock_now() reads; a buffer without it is stamped when it arrives.
#include "pipewire_stream.hpp"
#include "maicord/platform/audio/stream_audio.hpp"
#include "../capture_policy.hpp"
#include "maicord/core/interlocked.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/wait.hpp"

namespace maicord::pipewire {
namespace {

constexpr u64 kModifierLinear = 0;
constexpr u32 kAcceptedFormats[]{kVideoFormatBgrx, kVideoFormatBgra, kVideoFormatRgbx, kVideoFormatRgba};

// A POD writer over caller storage: begin/end nest containers (objects, choices) and patch their sizes.
struct Writer {
    u8* data;
    u32 size, capacity;
    bool failed;
    u32 open[4];
    u32 depth;

    void bytes(const void* source, u32 count) noexcept {
        if (failed || size + count > capacity) {
            failed = true;
            return;
        }
        copy_bytes(data + size, source, count);
        size += count;
    }
    void word(u32 value) noexcept {
        bytes(&value, 4);
    }
    void pad() noexcept {
        const u32 zero[2]{};
        bytes(zero, (8 - (size & 7)) & 7);
    }
    void begin(u32 type) noexcept {
        if (depth == 4) {
            failed = true;
            return;
        }
        open[depth++] = size;
        word(0);
        word(type);
    }
    void end() noexcept {
        const u32 start = open[--depth];
        if (!failed) {
            const u32 body = size - start - 8;
            copy_bytes(data + start, &body, 4);
        }
        pad();
    }
    void scalar(u32 type, const void* value, u32 value_size) noexcept {
        word(value_size);
        word(type);
        bytes(value, value_size);
        pad();
    }
    void id(u32 value) noexcept {
        scalar(kTypeId, &value, 4);
    }
    void integer(i32 value) noexcept {
        scalar(kTypeInt, &value, 4);
    }
    void property(u32 key, u32 flags) noexcept {
        word(key);
        word(flags);
    }
    // A choice of `count` values of `type`, each `value_size` bytes, packed after the child header.
    void choice(u32 kind, u32 type, u32 value_size, const void* values, u32 count) noexcept {
        begin(kTypeChoice);
        word(kind);
        word(0);
        word(value_size);
        word(type);
        bytes(values, value_size * count);
        end();
    }
    void object(u32 type, u32 id) noexcept {
        begin(kTypeObject);
        word(type);
        word(id);
    }
};

// A POD reader over one object's properties.
struct PropertyCursor {
    const u8* at;
    const u8* end;
};
bool property_next(PropertyCursor* cursor, u32* key, u32* flags, const Pod** value) noexcept {
    if (cursor->end - cursor->at < 16)
        return false;
    copy_bytes(key, cursor->at, 4);
    copy_bytes(flags, cursor->at + 4, 4);
    const auto* pod = reinterpret_cast<const Pod*>(cursor->at + 8);
    const u64 span = 8 + ((u64{pod->size} + 7) & ~u64{7});
    if (span > static_cast<u64>(cursor->end - (cursor->at + 8)))
        return false;
    *value = pod;
    cursor->at += 8 + span;
    return true;
}
// The value of a plain POD or the default (first value) of a choice, as `type` of `size` bytes.
bool pod_value(const Pod* pod, u32 type, u32 size, void* out, bool* choice) noexcept {
    const u8* body = reinterpret_cast<const u8*>(pod + 1);
    *choice = false;
    if (pod->type == kTypeChoice) {
        if (pod->size < 16 + size)
            return false;
        u32 child_size = 0, child_type = 0;
        copy_bytes(&child_size, body + 8, 4);
        copy_bytes(&child_type, body + 12, 4);
        u32 kind = 0;
        copy_bytes(&kind, body, 4);
        if (child_type != type || child_size != size)
            return false;
        *choice = kind != kChoiceNone;
        copy_bytes(out, body + 16, size);
        return true;
    }
    if (pod->type != type || pod->size < size)
        return false;
    copy_bytes(out, body, size);
    return true;
}

struct Library {
    SharedLibrary library;
    Api api;
    bool tried, loaded;
};
Library g_library{};
SRWLOCK g_library_lock = SRWLOCK_INIT;

} // namespace

const Api* api() noexcept {
    AcquireSRWLockExclusive(&g_library_lock);
    if (!g_library.tried) {
        g_library.tried = true;
        if (!shared_library_open(&g_library.library, "libpipewire-0.3.so.0")) {
            log_write(LogLevel::info, "capture", "libpipewire_missing");
        } else {
            SharedLibraryResolver resolve{g_library.library, nullptr};
#define MAICORD_PIPEWIRE_RESOLVE(ret, name, args) resolve(&g_library.api.name, #name);
            MAICORD_PIPEWIRE_FUNCTIONS(MAICORD_PIPEWIRE_RESOLVE)
#undef MAICORD_PIPEWIRE_RESOLVE
            if (resolve.missing) {
                log_write(LogLevel::warning, "capture", resolve.missing);
                shared_library_close(&g_library.library);
            } else {
                g_library.api.pw_init(nullptr, nullptr);
                g_library.loaded = true;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_library_lock);
    return g_library.loaded ? &g_library.api : nullptr;
}

u32 drm_fourcc(u32 video_format) noexcept {
    switch (video_format) {
    case kVideoFormatBgrx:
        return 0x34325258; // XR24: XRGB8888, little-endian B G R X
    case kVideoFormatBgra:
        return 0x34325241; // AR24: ARGB8888
    case kVideoFormatRgbx:
        return 0x34324258; // XB24: XBGR8888
    case kVideoFormatRgba:
        return 0x34324241; // AB24: ABGR8888
    default:
        return 0;
    }
}

u32 build_enum_formats(u8* storage, u32 capacity, const Pod** pods, u32 max_fps, bool fixed, u32 video_format,
                       u64 modifier) noexcept {
    Writer w{storage, 0, capacity, false, {}, 0};
    u32 count = 0;
    u32 offsets[4];
    for (const u32 format : kAcceptedFormats) {
        if (fixed && format != video_format)
            continue;
        offsets[count++] = w.size;
        w.object(kObjectFormat, kParamEnumFormat);
        w.property(kFormatMediaType, 0);
        w.id(kMediaTypeVideo);
        w.property(kFormatMediaSubtype, 0);
        w.id(kMediaSubtypeRaw);
        w.property(kFormatVideoFormat, 0);
        w.id(format);
        if (fixed) {
            w.property(kFormatVideoModifier, kPropMandatory);
            w.scalar(kTypeLong, &modifier, 8);
        } else {
            const u64 modifiers[]{kModifierLinear, kModifierLinear, kDrmFormatModifierInvalid};
            w.property(kFormatVideoModifier, kPropMandatory | kPropDontFixate);
            w.choice(kChoiceEnum, kTypeLong, 8, modifiers, 3);
        }
        const u32 sizes[]{1920, 1080, 1, 1, 16384, 16384};
        w.property(kFormatVideoSize, 0);
        w.choice(kChoiceRange, kTypeRectangle, 8, sizes, 3);
        const u32 rates[]{max_fps ? max_fps : 60, 1, 0, 1, 1000, 1};
        w.property(kFormatVideoFramerate, 0);
        w.choice(kChoiceRange, kTypeFraction, 8, rates, 3);
        w.end();
    }
    if (w.failed)
        return 0;
    for (u32 i = 0; i < count; ++i)
        pods[i] = reinterpret_cast<const Pod*>(storage + offsets[i]);
    return count;
}

bool build_audio_format(u8* storage, u32 capacity, const Pod** pod) noexcept {
    Writer w{storage, 0, capacity, false, {}, 0};
    w.object(kObjectFormat, kParamEnumFormat);
    w.property(kFormatMediaType, 0);
    w.id(kMediaTypeAudio);
    w.property(kFormatMediaSubtype, 0);
    w.id(kMediaSubtypeRaw);
    w.property(kFormatAudioFormat, 0);
    w.id(kAudioFormatS16);
    w.property(kFormatAudioRate, 0);
    w.integer(static_cast<i32>(kStreamAudioRate));
    w.property(kFormatAudioChannels, 0);
    w.integer(static_cast<i32>(kStreamAudioChannels));
    w.property(kFormatAudioPosition, 0);
    w.begin(kTypeArray);
    w.word(4);
    w.word(kTypeId);
    w.word(kChannelFl);
    w.word(kChannelFr);
    w.end();
    w.end();
    *pod = w.failed ? nullptr : reinterpret_cast<const Pod*>(storage);
    return !w.failed;
}

u32 build_buffer_params(u8* storage, u32 capacity, const Pod** pods) noexcept {
    Writer w{storage, 0, capacity, false, {}, 0};
    w.object(kObjectParamBuffers, kParamBuffers);
    const i32 counts[]{4, 2, 8};
    w.property(kBuffersBuffers, 0);
    w.choice(kChoiceRange, kTypeInt, 4, counts, 3);
    const i32 types[]{1 << kDataDmaBuf};
    w.property(kBuffersDataType, 0);
    w.choice(kChoiceFlags, kTypeInt, 4, types, 1);
    w.end();
    const u32 meta = w.size;
    w.object(kObjectParamMeta, kParamMeta);
    w.property(kMetaType, 0);
    w.id(kMetaHeader);
    w.property(kMetaSize, 0);
    w.integer(static_cast<i32>(sizeof(MetaHeader)));
    w.end();
    if (w.failed)
        return 0;
    pods[0] = reinterpret_cast<const Pod*>(storage);
    pods[1] = reinterpret_cast<const Pod*>(storage + meta);
    return 2;
}

bool parse_video_format(const Pod* pod, VideoFormat* format) noexcept {
    if (!pod || pod->type != kTypeObject || pod->size < 8)
        return false;
    const u8* body = reinterpret_cast<const u8*>(pod + 1);
    u32 object_type = 0;
    copy_bytes(&object_type, body, 4);
    if (object_type != kObjectFormat)
        return false;
    reset_value(format);
    format->modifier = kDrmFormatModifierInvalid;
    u32 media_type = 0, media_subtype = 0;
    PropertyCursor cursor{body + 8, body + pod->size};
    u32 key = 0, flags = 0;
    const Pod* value = nullptr;
    while (property_next(&cursor, &key, &flags, &value)) {
        bool choice = false;
        u32 size[2]{};
        if (key == kFormatMediaType)
            pod_value(value, kTypeId, 4, &media_type, &choice);
        else if (key == kFormatMediaSubtype)
            pod_value(value, kTypeId, 4, &media_subtype, &choice);
        else if (key == kFormatVideoFormat)
            pod_value(value, kTypeId, 4, &format->video_format, &choice);
        else if (key == kFormatVideoModifier) {
            pod_value(value, kTypeLong, 8, &format->modifier, &choice);
            format->modifier_choice = choice || (flags & kPropDontFixate);
        } else if (key == kFormatVideoSize && pod_value(value, kTypeRectangle, 8, size, &choice)) {
            format->width = size[0];
            format->height = size[1];
        }
    }
    return media_type == kMediaTypeVideo && media_subtype == kMediaSubtypeRaw && drm_fourcc(format->video_format) &&
           format->width && format->height;
}

// ---- The consumer ---------------------------------------------------------------------------------------------------

struct Consumer {
    const Api* api;
    ThreadLoop* loop;
    Context* context;
    Core* core;
    Stream* stream;
    Hook hook;
    StreamEvents events;
    CaptureSink sink;
    Waitable ended;
    VideoFormat format;
    CapturePacer pacer;
    u32 max_fps;
    bool cursor, connected, streamed;
    volatile bool linked; // a format settled with the node; read by the portal thread
    volatile LONG end;
    u8 pods[2048];
};

namespace {

void finish(Consumer* c, StreamEnd end) noexcept {
    if (InterlockedCompareExchange(&c->end, static_cast<LONG>(end), 0) == 0)
        event_set(c->ended);
}

void on_state(void* data, i32, i32 state, const char* error) noexcept {
    auto* c = static_cast<Consumer*>(data);
    if (state == kStreamStateError) {
        log_write(LogLevel::warning, "capture", error ? error : "pipewire_stream_error");
        // Connected but no format ever settled: the producer offers none of the formats asked for (it has no dma-bufs
        // of packed RGB). Before connecting (the remote refused us) or after the format: a failure.
        finish(c, c->connected && !c->linked ? StreamEnd::unsupported : StreamEnd::failed);
    } else if (state == kStreamStatePaused) {
        c->connected = true;
    } else if (state == kStreamStateStreaming) {
        c->streamed = true;
    } else if (state == kStreamStateUnconnected) {
        // The remote went away (the session ended) or never answered.
        finish(c, c->streamed ? StreamEnd::closed : StreamEnd::failed);
    }
}

void on_param(void* data, u32 id, const Pod* param) noexcept {
    auto* c = static_cast<Consumer*>(data);
    if (id != kParamFormat || !param)
        return;
    VideoFormat format{};
    if (!parse_video_format(param, &format))
        return;
    const Pod* pods[4];
    u32 count = 0;
    if (format.modifier_choice) {
        // The producer leaves the modifier to us: fix the first one it offers (linear, ours first) and negotiate again.
        count = build_enum_formats(c->pods, sizeof(c->pods), pods, c->max_fps, true, format.video_format,
                                   format.modifier);
    } else {
        c->format = format;
        c->linked = true;
        count = build_buffer_params(c->pods, sizeof(c->pods), pods);
    }
    if (count)
        c->api->pw_stream_update_params(c->stream, pods, count);
}

void on_process(void* data) noexcept {
    auto* c = static_cast<Consumer*>(data);
    // The newest picture only: older ones that queued up go straight back.
    Buffer* newest = nullptr;
    while (Buffer* buffer = c->api->pw_stream_dequeue_buffer(c->stream)) {
        if (newest)
            c->api->pw_stream_queue_buffer(c->stream, newest);
        newest = buffer;
    }
    if (!newest)
        return;
    const SpaBuffer* buffer = newest->buffer;
    const Data* plane = buffer && buffer->n_datas ? &buffer->datas[0] : nullptr;
    i64 pts = 0;
    bool corrupted = !plane || plane->type != kDataDmaBuf || plane->fd < 0 || !plane->chunk ||
                     (plane->chunk->flags & kChunkCorrupted) || plane->chunk->stride <= 0;
    for (u32 i = 0; buffer && i < buffer->n_metas; ++i) {
        const Meta& meta = buffer->metas[i];
        if (meta.type == kMetaHeader && meta.size >= sizeof(MetaHeader) && meta.data) {
            const auto* header = static_cast<const MetaHeader*>(meta.data);
            pts = header->pts;
            corrupted = corrupted || (header->flags & kHeaderCorrupted);
        }
    }
    const u64 now = clock_microseconds(clock_initialize(), 0, clock_now());
    const u64 stamped = pts > 0 ? static_cast<u64>(pts) / 1000 : 0;
    // A pts on another clock (far from now) is not ours to pass on.
    const u64 timestamp = stamped && stamped <= now + 1'000'000 && stamped + 1'000'000 >= now ? stamped : now;
    if (!corrupted && capture_pacer_admit(&c->pacer, timestamp, c->max_fps)) {
        CaptureFrame frame{};
        frame.plane = {static_cast<i32>(plane->fd),     drm_fourcc(c->format.video_format),
                       c->format.width,                 c->format.height,
                       plane->chunk->offset,            static_cast<u32>(plane->chunk->stride),
                       c->format.modifier,              -1,
                       0,                               0};
        frame.width = c->format.width;
        frame.height = c->format.height;
        frame.timestamp_us = timestamp;
        frame.cursor = c->cursor;
        c->sink.frame(c->sink.context, frame);
    }
    c->api->pw_stream_queue_buffer(c->stream, newest);
}

} // namespace

Consumer* consumer_open(int fd, u32 node, bool cursor, u32 max_fps, const CaptureSink& sink, Waitable ended) noexcept {
    const Api* a = api();
    auto* c = a ? commit_pages<Consumer>() : nullptr;
    if (!c) {
        if (fd >= 0)
            sys_close(fd);
        return nullptr;
    }
    c->api = a;
    c->sink = sink;
    c->ended = ended;
    c->cursor = cursor;
    c->max_fps = max_fps;
    c->events.version = 0;
    c->events.state_changed = on_state;
    c->events.param_changed = on_param;
    c->events.process = on_process;
    c->loop = a->pw_thread_loop_new("maicord-capture", nullptr);
    if (!c->loop || a->pw_thread_loop_start(c->loop) < 0) {
        if (fd >= 0)
            sys_close(fd);
        consumer_close(c);
        return nullptr;
    }
    a->pw_thread_loop_lock(c->loop);
    c->context = a->pw_context_new(a->pw_thread_loop_get_loop(c->loop), nullptr, 0);
    if (c->context)
        c->core = fd >= 0 ? a->pw_context_connect_fd(c->context, fd, nullptr, 0)
                          : a->pw_context_connect(c->context, nullptr, 0);
    else if (fd >= 0)
        sys_close(fd);
    if (c->core)
        c->stream = a->pw_stream_new(
            c->core, "MaiCord screen capture",
            a->pw_properties_new_string(
                "media.type=Video media.category=Capture media.role=Screen node.name=maicord-screen-capture"));
    const Pod* pods[4];
    const u32 count = build_enum_formats(c->pods, sizeof(c->pods), pods, max_fps, false, 0, 0);
    bool ok = c->stream != nullptr;
    if (ok) {
        a->pw_stream_add_listener(c->stream, &c->hook, &c->events, c);
        ok = a->pw_stream_connect(c->stream, kDirectionInput, node, kStreamAutoconnect, pods, count) >= 0;
    }
    a->pw_thread_loop_unlock(c->loop);
    if (!ok) {
        log_write(LogLevel::warning, "capture", "pipewire_connect_failed");
        consumer_close(c);
        return nullptr;
    }
    return c;
}

StreamEnd consumer_end(const Consumer* consumer) noexcept {
    return static_cast<StreamEnd>(ReadAcquire(&consumer->end));
}

bool consumer_linked(const Consumer* consumer) noexcept {
    return consumer->linked;
}

void consumer_close(Consumer* c) noexcept {
    if (!c)
        return;
    const Api* a = c->api;
    if (c->loop) {
        a->pw_thread_loop_lock(c->loop);
        if (c->stream)
            a->pw_stream_destroy(c->stream);
        if (c->core)
            a->pw_core_disconnect(c->core);
        a->pw_thread_loop_unlock(c->loop);
        a->pw_thread_loop_stop(c->loop);
    }
    if (c->context)
        a->pw_context_destroy(c->context);
    if (c->loop)
        a->pw_thread_loop_destroy(c->loop);
    pages_release(c, sizeof(Consumer));
}

} // namespace maicord::pipewire
