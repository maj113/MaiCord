#pragma once

#include "maicord/core/base.hpp"

// The libpulse interface MaiCord uses, declared without libpulse's headers (Linux). The library is dlopen()ed
// ("libpulse.so.0": PulseAudio, pipewire-pulse and WSLg all speak it), so a system without a sound server client
// library reports audio as unavailable instead of failing to load. Types and calls keep libpulse's binary layout;
// tools/check_pulse_abi.py compares this file with the installed libpulse-dev headers (constants marked with the
// macro they copy, the function table, the struct offsets). Only functions that exist in libpulse 15.99 (Ubuntu 22.04,
// the oldest of the supported distributions; pipewire-pulse 0.3.48 serves it there) are used.

namespace maicord::pulse {

struct ThreadedMainloop;
struct MainloopApi;
struct Context;
struct Stream;
struct Operation;
struct Proplist;

// pa_sample_spec
struct SampleSpec {
    i32 format;
    u32 rate;
    u8 channels;
};
// pa_buffer_attr: byte counts; every field (u32)-1 asks the server for its default.
struct BufferAttr {
    u32 maxlength;
    u32 tlength;
    u32 prebuf;
    u32 minreq;
    u32 fragsize;
};
inline constexpr u32 kChannelsMaximum = 32; // PA_CHANNELS_MAX
struct ChannelMap {
    u8 channels;
    i32 map[kChannelsMaximum];
};
struct CVolume {
    u8 channels;
    u32 values[kChannelsMaximum];
};
// The leading fields of pa_sink_info and pa_source_info, the ones the device list reads.
struct SinkInfo {
    const char* name;
    u32 index;
    const char* description;
    SampleSpec sample_spec;
    ChannelMap channel_map;
    u32 owner_module;
    CVolume volume;
    i32 mute;
    u32 monitor_source;
    const char* monitor_source_name;
};
struct SourceInfo {
    const char* name;
    u32 index;
    const char* description;
    SampleSpec sample_spec;
    ChannelMap channel_map;
    u32 owner_module;
    CVolume volume;
    i32 mute;
    u32 monitor_of_sink;
    const char* monitor_of_sink_name;
};
// The leading fields of pa_server_info.
struct ServerInfo {
    const char* user_name;
    const char* host_name;
    const char* server_version;
    const char* server_name;
    SampleSpec sample_spec;
    const char* default_sink_name;
    const char* default_source_name;
};

// The layouts above are libpulse's; tools/check_pulse_abi.py compares these numbers with the installed headers.
static_assert(sizeof(SampleSpec) == 12 && sizeof(BufferAttr) == 20 && sizeof(ChannelMap) == 132 &&
              sizeof(CVolume) == 132);
static_assert(__builtin_offsetof(SinkInfo, description) == 16 && __builtin_offsetof(SinkInfo, sample_spec) == 24 &&
              __builtin_offsetof(SinkInfo, channel_map) == 36 && __builtin_offsetof(SinkInfo, volume) == 172 &&
              __builtin_offsetof(SinkInfo, monitor_source) == 308 &&
              __builtin_offsetof(SinkInfo, monitor_source_name) == 312);
static_assert(__builtin_offsetof(SourceInfo, description) == 16 && __builtin_offsetof(SourceInfo, volume) == 172 &&
              __builtin_offsetof(SourceInfo, monitor_of_sink) == 308 &&
              __builtin_offsetof(SourceInfo, monitor_of_sink_name) == 312);
static_assert(__builtin_offsetof(ServerInfo, server_name) == 24 && __builtin_offsetof(ServerInfo, sample_spec) == 32 &&
              __builtin_offsetof(ServerInfo, default_sink_name) == 48 &&
              __builtin_offsetof(ServerInfo, default_source_name) == 56);

inline constexpr u32 kInvalid = 0xFFFFFFFFu; // PA_INVALID_INDEX, and the "server default" of a buffer attribute

inline constexpr i32 kSampleS16LE = 3;      // PA_SAMPLE_S16LE
inline constexpr i32 kSampleFloat32LE = 5;  // PA_SAMPLE_FLOAT32LE

inline constexpr i32 kContextNoAutospawn = 1; // PA_CONTEXT_NOAUTOSPAWN
inline constexpr i32 kContextReady = 4;       // PA_CONTEXT_READY
inline constexpr i32 kContextFailed = 5;      // PA_CONTEXT_FAILED
inline constexpr i32 kContextTerminated = 6;  // PA_CONTEXT_TERMINATED

inline constexpr i32 kStreamCreating = 1;   // PA_STREAM_CREATING
inline constexpr i32 kStreamReady = 2;      // PA_STREAM_READY
inline constexpr i32 kStreamFailed = 3;     // PA_STREAM_FAILED
inline constexpr i32 kStreamTerminated = 4; // PA_STREAM_TERMINATED

inline constexpr u32 kStreamStartCorked = 0x1u;       // PA_STREAM_START_CORKED
inline constexpr u32 kStreamInterpolateTiming = 0x2u; // PA_STREAM_INTERPOLATE_TIMING
inline constexpr u32 kStreamAutoTimingUpdate = 0x8u;  // PA_STREAM_AUTO_TIMING_UPDATE
inline constexpr u32 kStreamAdjustLatency = 0x2000u;  // PA_STREAM_ADJUST_LATENCY

inline constexpr i32 kSeekRelative = 0; // PA_SEEK_RELATIVE

inline constexpr u32 kSubscribeSink = 0x1u;    // PA_SUBSCRIPTION_MASK_SINK
inline constexpr u32 kSubscribeSource = 0x2u;  // PA_SUBSCRIPTION_MASK_SOURCE
inline constexpr u32 kSubscribeServer = 0x80u; // PA_SUBSCRIPTION_MASK_SERVER
inline constexpr u32 kEventFacilityMask = 0xFu; // PA_SUBSCRIPTION_EVENT_FACILITY_MASK
inline constexpr u32 kEventTypeMask = 0x30u;    // PA_SUBSCRIPTION_EVENT_TYPE_MASK
inline constexpr u32 kEventSink = 0u;           // PA_SUBSCRIPTION_EVENT_SINK
inline constexpr u32 kEventSource = 1u;         // PA_SUBSCRIPTION_EVENT_SOURCE
inline constexpr u32 kEventServer = 7u;         // PA_SUBSCRIPTION_EVENT_SERVER
inline constexpr u32 kEventNew = 0x0u;          // PA_SUBSCRIPTION_EVENT_NEW
inline constexpr u32 kEventRemove = 0x20u;      // PA_SUBSCRIPTION_EVENT_REMOVE

// Callbacks run on libpulse's mainloop thread.
using ContextNotifyCb = void (*)(Context*, void*) noexcept;
using ContextSuccessCb = void (*)(Context*, i32 success, void*) noexcept;
using SubscribeCb = void (*)(Context*, u32 event, u32 index, void*) noexcept;
using SinkInfoCb = void (*)(Context*, const SinkInfo*, i32 end_of_list, void*) noexcept;
using SourceInfoCb = void (*)(Context*, const SourceInfo*, i32 end_of_list, void*) noexcept;
using ServerInfoCb = void (*)(Context*, const ServerInfo*, void*) noexcept;
using StreamNotifyCb = void (*)(Stream*, void*) noexcept;
using StreamRequestCb = void (*)(Stream*, usize bytes, void*) noexcept;
using StreamSuccessCb = void (*)(Stream*, i32 success, void*) noexcept;
using FreeCb = void (*)(void*) noexcept;

// The entry points of libpulse used by the backend: F(return type, name, (argument types)). Filled by api().
#define MAICORD_PULSE_FUNCTIONS(F)                                                                                     \
    F(ThreadedMainloop*, pa_threaded_mainloop_new, (void))                                                             \
    F(void, pa_threaded_mainloop_free, (ThreadedMainloop*))                                                            \
    F(i32, pa_threaded_mainloop_start, (ThreadedMainloop*))                                                            \
    F(void, pa_threaded_mainloop_stop, (ThreadedMainloop*))                                                            \
    F(void, pa_threaded_mainloop_lock, (ThreadedMainloop*))                                                            \
    F(void, pa_threaded_mainloop_unlock, (ThreadedMainloop*))                                                          \
    F(MainloopApi*, pa_threaded_mainloop_get_api, (ThreadedMainloop*))                                                 \
    F(Proplist*, pa_proplist_new, (void))                                                                              \
    F(i32, pa_proplist_sets, (Proplist*, const char*, const char*))                                                    \
    F(void, pa_proplist_free, (Proplist*))                                                                             \
    F(Context*, pa_context_new_with_proplist, (MainloopApi*, const char*, const Proplist*))                            \
    F(i32, pa_context_connect, (Context*, const char*, i32, const void*))                                              \
    F(void, pa_context_disconnect, (Context*))                                                                         \
    F(void, pa_context_unref, (Context*))                                                                              \
    F(i32, pa_context_get_state, (const Context*))                                                                     \
    F(i32, pa_context_errno, (const Context*))                                                                         \
    F(void, pa_context_set_state_callback, (Context*, ContextNotifyCb, void*))                                         \
    F(void, pa_context_set_subscribe_callback, (Context*, SubscribeCb, void*))                                         \
    F(Operation*, pa_context_subscribe, (Context*, u32, ContextSuccessCb, void*))                                      \
    F(Operation*, pa_context_get_sink_info_list, (Context*, SinkInfoCb, void*))                                        \
    F(Operation*, pa_context_get_source_info_list, (Context*, SourceInfoCb, void*))                                    \
    F(Operation*, pa_context_get_server_info, (Context*, ServerInfoCb, void*))                                         \
    F(Operation*, pa_context_set_sink_input_volume, (Context*, u32, const CVolume*, ContextSuccessCb, void*))          \
    F(void, pa_operation_cancel, (Operation*))                                                                         \
    F(void, pa_operation_unref, (Operation*))                                                                          \
    F(Stream*, pa_stream_new_with_proplist, (Context*, const char*, const SampleSpec*, const ChannelMap*,             \
                                              Proplist*))                                                              \
    F(void, pa_stream_unref, (Stream*))                                                                                \
    F(i32, pa_stream_connect_playback, (Stream*, const char*, const BufferAttr*, u32, const CVolume*, Stream*))        \
    F(i32, pa_stream_connect_record, (Stream*, const char*, const BufferAttr*, u32))                                   \
    F(i32, pa_stream_disconnect, (Stream*))                                                                            \
    F(i32, pa_stream_get_state, (const Stream*))                                                                       \
    F(void, pa_stream_set_state_callback, (Stream*, StreamNotifyCb, void*))                                            \
    F(void, pa_stream_set_write_callback, (Stream*, StreamRequestCb, void*))                                           \
    F(void, pa_stream_set_read_callback, (Stream*, StreamRequestCb, void*))                                            \
    F(usize, pa_stream_writable_size, (const Stream*))                                                                 \
    F(i32, pa_stream_write, (Stream*, const void*, usize, FreeCb, i64, i32))                                           \
    F(i32, pa_stream_peek, (Stream*, const void**, usize*))                                                            \
    F(i32, pa_stream_drop, (Stream*))                                                                                  \
    F(Operation*, pa_stream_cork, (Stream*, i32, StreamSuccessCb, void*))                                              \
    F(Operation*, pa_stream_drain, (Stream*, StreamSuccessCb, void*))                                                  \
    F(i32, pa_stream_get_time, (Stream*, u64*))                                                                        \
    F(u32, pa_stream_get_index, (const Stream*))                                                                       \
    F(const BufferAttr*, pa_stream_get_buffer_attr, (Stream*))                                                         \
    F(u32, pa_sw_volume_from_linear, (double))                                                                         \
    F(CVolume*, pa_cvolume_set, (CVolume*, u32, u32))

struct Api {
#define MAICORD_PULSE_MEMBER(ret, name, args) ret(*name) args;
    MAICORD_PULSE_FUNCTIONS(MAICORD_PULSE_MEMBER)
#undef MAICORD_PULSE_MEMBER
};

} // namespace maicord::pulse
