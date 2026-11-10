// Stream audio on Linux (include/maicord/platform/audio/stream_audio.hpp): what the other applications play, without
// MaiCord's own output (the call), through PipeWire (libpipewire-0.3, dlopen()ed by pipewire.cpp).
//
// One capture stream (48 kHz stereo PCM16, not autoconnected) is linked by MaiCord itself, through the link factory, to
// the output ports of every application playback stream (media.class Stream/Output/Audio) whose
// application.process.id is not MaiCord's: left channels to its left input, right to right, mono and centre to both.
// The process is in the node's full properties (pipewire-pulse puts the PulseAudio client's there, MaiCord's own audio
// included; pw_stream its application's), which the registry's global lacks, so each playback node is bound and its
// info read before it is linked. The PipeWire registry keeps the set current: a stream that appears is linked, one that
// goes takes its links with it.
// PipeWire mixes everything linked to an input port, after each stream's own volume, as the session manager would. The
// capture follows the graph's driver (the sinks those streams play to); while nothing plays, nothing arrives. Without
// the library or a running daemon there is no stream audio: never a sink monitor, which would carry the call.
#include "maicord/platform/audio/stream_audio.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"
#include "pipewire_stream.hpp"
#include "stream_audio_routes.hpp"

// The registry bookkeeping and the stream's set-up run as playback streams come and go: compiled for size. on_process,
// which runs per block, is outside the regions.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {
using namespace pipewire;

constexpr u32 kFrameBytes = sizeof(i16) * kStreamAudioChannels;
constexpr u32 kNodes = 128, kPorts = 512, kLinks = 512;

struct Runtime;
// A playback node (media.class Stream/Output/Audio), bound for its info. Slots stay where they are while used: the
// node's listener points at its slot.
struct NodeEntry {
    Runtime* runtime;
    Proxy* proxy;
    Hook hook;
    u32 id;
    u32 process; // application.process.id from the info, 0 when it names none
    bool used, known;
};
struct PortEntry {
    u32 id, node;
    bool output;
    u8 inputs; // output ports: the inputs it feeds (bit 0 left, bit 1 right); input ports: the input it is
};
struct LinkEntry {
    u32 output_port, input_port;
    Proxy* proxy;
};

struct Runtime {
    const Api* api;
    ThreadLoop* loop;
    Context* context;
    Core* core;
    Registry* registry;
    Hook registry_hook;
    RegistryEvents registry_events;
    Stream* stream;
    Hook stream_hook;
    StreamEvents stream_events;
    StreamAudioSink sink;
    StreamAudioOptions options;
    u32 own_process;
    u32 port_count, link_count;
    NodeEntry nodes[kNodes];
    PortEntry ports[kPorts];
    LinkEntry links[kLinks];
};

const char* property(const Dict* props, const char* key) noexcept {
    for (u32 i = 0; props && i < props->n_items; ++i)
        if (stream_audio_text_equal(props->items[i].key, key))
            return props->items[i].value;
    return nullptr;
}

// Whether a node's output is recorded: its info has come and names a process that is not MaiCord's.
bool recorded(const Runtime& r, const NodeEntry& node) noexcept {
    return node.known && stream_audio_node_recorded(true, node.process, r.own_process, r.options);
}

const NodeEntry* find_node(const Runtime& r, u32 id) noexcept {
    for (const NodeEntry& node : r.nodes)
        if (node.used && node.id == id)
            return &node;
    return nullptr;
}

bool linked(const Runtime& r, u32 output, u32 input) noexcept {
    for (u32 i = 0; i < r.link_count; ++i)
        if (r.links[i].output_port == output && r.links[i].input_port == input)
            return true;
    return false;
}

void create_link(Runtime& r, const PortEntry& output, const PortEntry& input) noexcept {
    if (r.link_count == kLinks)
        return;
    char text[4][12];
    const u32 values[4]{output.id, input.id, output.node, input.node};
    for (u32 i = 0; i < 4; ++i)
        text[i][decimal_write(text[i], values[i])] = 0;
    const DictItem items[]{{"link.output.port", text[0]}, {"link.input.port", text[1]},  {"link.output.node", text[2]},
                           {"link.input.node", text[3]},  {"object.linger", "false"}};
    const Dict props{0, sizeof(items) / sizeof(items[0]), items};
    auto* core = reinterpret_cast<Interface*>(r.core);
    auto* proxy = static_cast<Proxy*>(static_cast<const CoreMethods*>(core->cb.funcs)
                                          ->create_object(core->cb.data, "link-factory", kTypeLink, kVersionLink, &props, 0));
    if (proxy)
        r.links[r.link_count++] = {output.id, input.id, proxy};
}

// Links every recorded output port to the capture's matching inputs that are not linked yet.
void relink(Runtime& r) noexcept {
    const u32 own_node = r.api->pw_stream_get_node_id(r.stream);
    if (own_node == kAnyId)
        return;
    for (u32 i = 0; i < r.port_count; ++i) {
        const PortEntry& output = r.ports[i];
        const NodeEntry* node = output.output ? find_node(r, output.node) : nullptr;
        if (!node || !recorded(r, *node))
            continue;
        for (u32 k = 0; k < r.port_count; ++k) {
            const PortEntry& input = r.ports[k];
            if (!input.output && input.node == own_node && (output.inputs & input.inputs) &&
                !linked(r, output.id, input.id))
                create_link(r, output, input);
        }
    }
}

void on_node_info(void* data, const NodeInfo* info) noexcept {
    auto& node = *static_cast<NodeEntry*>(data);
    if (!info || !info->props || node.known)
        return;
    node.process = stream_audio_property_number(property(info->props, "application.process.id"));
    node.known = true;
    relink(*node.runtime);
}
const NodeEvents kNodeEvents{0, on_node_info, nullptr};

void forget_node(Runtime& r, NodeEntry& node) noexcept {
    if (node.proxy)
        r.api->pw_proxy_destroy(node.proxy);
    node = {};
}

void on_global(void* data, u32 id, u32, const char* type, u32, const Dict* props) noexcept {
    auto& r = *static_cast<Runtime*>(data);
    if (stream_audio_text_equal(type, kTypeNode)) {
        if (!stream_audio_text_equal(property(props, "media.class"), "Stream/Output/Audio"))
            return;
        NodeEntry* slot = nullptr;
        for (NodeEntry& node : r.nodes)
            if (!node.used) {
                slot = &node;
                break;
            }
        auto* registry = reinterpret_cast<Interface*>(r.registry);
        Proxy* proxy = slot ? static_cast<const RegistryMethods*>(registry->cb.funcs)
                                  ->bind(registry->cb.data, id, kTypeNode, kVersionNode, 0)
                            : nullptr;
        if (!proxy)
            return;
        *slot = {&r, proxy, {}, id, 0, true, false};
        auto* node = reinterpret_cast<Interface*>(proxy);
        static_cast<const NodeMethods*>(node->cb.funcs)->add_listener(node->cb.data, &slot->hook, &kNodeEvents, slot);
        return; // linked once its info names its process
    } else if (stream_audio_text_equal(type, kTypePort)) {
        if (r.port_count == kPorts || stream_audio_text_equal(property(props, "port.monitor"), "true"))
            return;
        const char* direction = property(props, "port.direction");
        const char* channel = property(props, "audio.channel");
        const bool output = stream_audio_text_equal(direction, "out");
        const u8 inputs = output ? stream_audio_channel_inputs(channel)
                                 : stream_audio_text_equal(channel, "FL")   ? 1
                                 : stream_audio_text_equal(channel, "FR") ? 2
                                                                           : 0;
        if (!inputs)
            return;
        r.ports[r.port_count++] = {id, stream_audio_property_number(property(props, "node.id")), output, inputs};
    } else {
        return;
    }
    relink(r);
}

void on_global_remove(void* data, u32 id) noexcept {
    auto& r = *static_cast<Runtime*>(data);
    for (NodeEntry& node : r.nodes)
        if (node.used && node.id == id)
            forget_node(r, node);
    for (u32 i = 0; i < r.port_count; ++i)
        if (r.ports[i].id == id)
            r.ports[i] = r.ports[--r.port_count];
    // A port that went took its links with it: forget their proxies.
    for (u32 i = 0; i < r.link_count;) {
        if (r.links[i].output_port == id || r.links[i].input_port == id) {
            r.api->pw_proxy_destroy(r.links[i].proxy);
            r.links[i] = r.links[--r.link_count];
        } else {
            ++i;
        }
    }
}

void on_state(void* data, i32, i32 state, const char* error) noexcept {
    auto& r = *static_cast<Runtime*>(data);
    if (state == kStreamStateError)
        log_write(LogLevel::warning, "stream_audio", error ? error : "pipewire_stream_error");
    else
        relink(r); // the capture's node now has its id
}

#pragma clang attribute pop

void on_process(void* data) noexcept {
    auto& r = *static_cast<Runtime*>(data);
    Buffer* buffer = r.api->pw_stream_dequeue_buffer(r.stream);
    if (!buffer)
        return;
    const SpaBuffer* spa = buffer->buffer;
    const Data* plane = spa && spa->n_datas ? &spa->datas[0] : nullptr;
    if (plane && plane->data && plane->chunk && plane->chunk->offset <= plane->maxsize &&
        plane->chunk->size <= plane->maxsize - plane->chunk->offset) {
        const u32 frames = plane->chunk->size / kFrameBytes;
        // The block ends now: its first frame was heard its duration ago.
        const u64 now = clock_microseconds(clock_initialize(), 0, clock_now());
        const u64 duration = u64{frames} * 1'000'000u / kStreamAudioRate;
        if (frames)
            r.sink.block(r.sink.context,
                         {reinterpret_cast<const i16*>(static_cast<const u8*>(plane->data) + plane->chunk->offset),
                          frames * kStreamAudioChannels},
                         now > duration ? now - duration : 0);
    }
    r.api->pw_stream_queue_buffer(r.stream, buffer);
}

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

void destroy(Runtime* r) noexcept {
    const Api* a = r->api;
    if (r->loop) {
        a->pw_thread_loop_lock(r->loop);
        for (u32 i = 0; i < r->link_count; ++i)
            a->pw_proxy_destroy(r->links[i].proxy);
        r->link_count = 0;
        for (NodeEntry& node : r->nodes)
            if (node.used)
                forget_node(*r, node);
        if (r->stream)
            a->pw_stream_destroy(r->stream);
        if (r->registry)
            a->pw_proxy_destroy(reinterpret_cast<Proxy*>(r->registry));
        if (r->core)
            a->pw_core_disconnect(r->core);
        a->pw_thread_loop_unlock(r->loop);
        a->pw_thread_loop_stop(r->loop);
    }
    if (r->context)
        a->pw_context_destroy(r->context);
    if (r->loop)
        a->pw_thread_loop_destroy(r->loop);
    pages_release(r, sizeof(Runtime));
}

// Whether a PipeWire daemon listens for this user: PIPEWIRE_REMOTE names one, or $XDG_RUNTIME_DIR/pipewire-0 exists.
bool daemon_present() noexcept {
    if (environment_get("PIPEWIRE_REMOTE"))
        return true;
    const char* runtime = environment_get("XDG_RUNTIME_DIR");
    if (!runtime)
        return false;
    char path[512];
    const u32 length = c_string_length(runtime);
    static constexpr char kSocket[] = "/pipewire-0";
    if (length + sizeof(kSocket) > sizeof(path))
        return false;
    copy_bytes(path, runtime, length);
    copy_bytes(path + length, kSocket, sizeof(kSocket));
    return sys_access(path, 0) == 0;
}

} // namespace

StreamAudioMethod stream_audio_method() noexcept {
    return api() && daemon_present() ? StreamAudioMethod::application_streams
                                     : StreamAudioMethod::unavailable_no_pipewire;
}

bool stream_audio_start(StreamAudioCapture* capture, const StreamAudioOptions& options,
                        const StreamAudioSink& sink) noexcept {
    const Api* a = api();
    if (!capture || capture->runtime || !sink.block || !a)
        return false;
    auto* r = commit_pages<Runtime>();
    if (!r)
        return false;
    r->api = a;
    r->sink = sink;
    r->options = options;
    r->own_process = static_cast<u32>(sys_getpid());
    r->registry_events = {0, on_global, on_global_remove};
    r->stream_events.version = 0;
    r->stream_events.state_changed = on_state;
    r->stream_events.process = on_process;
    r->loop = a->pw_thread_loop_new("maicord-stream-audio", nullptr);
    if (!r->loop || a->pw_thread_loop_start(r->loop) < 0) {
        destroy(r);
        return false;
    }
    a->pw_thread_loop_lock(r->loop);
    r->context = a->pw_context_new(a->pw_thread_loop_get_loop(r->loop), nullptr, 0);
    r->core = r->context ? a->pw_context_connect(r->context, nullptr, 0) : nullptr;
    bool ok = r->core != nullptr;
    if (ok) {
        auto* core = reinterpret_cast<Interface*>(r->core);
        r->registry = static_cast<const CoreMethods*>(core->cb.funcs)->get_registry(core->cb.data, kVersionRegistry, 0);
        // Not autoconnected: the session manager would link it to a microphone. MaiCord links it itself (relink).
        r->stream = r->registry ? a->pw_stream_new(r->core, "MaiCord stream audio",
                                                   a->pw_properties_new_string(
                                                       "media.type=Audio media.category=Capture media.role=Screen "
                                                       "node.name=maicord-stream-audio node.autoconnect=false "
                                                       // One input port per channel (FL, FR) to link to, made by the
                                                       // adapter itself, as the session manager would.
                                                       "adapter.auto-port-config={ mode=dsp position=preserve }"))
                                : nullptr;
        const Pod* format = audio_format();
        ok = r->stream != nullptr;
        if (ok) {
            a->pw_stream_add_listener(r->stream, &r->stream_hook, &r->stream_events, r);
            ok = a->pw_stream_connect(r->stream, kDirectionInput, kAnyId, kStreamMapBuffers, &format, 1) >= 0;
        }
        if (ok) {
            auto* registry = reinterpret_cast<Interface*>(r->registry);
            ok = static_cast<const RegistryMethods*>(registry->cb.funcs)
                     ->add_listener(registry->cb.data, &r->registry_hook, &r->registry_events, r) >= 0;
        }
    }
    a->pw_thread_loop_unlock(r->loop);
    if (!ok) {
        log_write(LogLevel::info, "stream_audio", "pipewire_unavailable");
        destroy(r);
        return false;
    }
    capture->runtime = r;
    return true;
}

void stream_audio_stop(StreamAudioCapture* capture) noexcept {
    if (!capture || !capture->runtime)
        return;
    destroy(static_cast<Runtime*>(capture->runtime));
    capture->runtime = nullptr;
}

} // namespace maicord

#pragma clang attribute pop
