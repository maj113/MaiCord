#pragma once

#include "maicord/core/base.hpp"

// The libpipewire-0.3 interface screen capture and stream audio use, declared without PipeWire's or SPA's headers
// (Linux). The library is dlopen()ed ("libpipewire-0.3.so.0"), so a system without PipeWire reports capture as
// unavailable instead of failing to load. Types and calls keep the library's binary layout; tools/check_pipewire_abi.py
// compares this file with the installed libpipewire-0.3-dev and libspa-0.2-dev headers (constants marked with the macro
// or enumerator they copy, the function table, the struct sizes and offsets). Only functions of PipeWire 0.3.48
// (Ubuntu 22.04, the oldest of the supported distributions) are used.

namespace maicord::pipewire {

struct ThreadLoop;
struct Loop;
struct Context;
struct Core;
struct Stream;
struct Properties;
struct Registry;
struct Proxy;

// struct spa_hook: a listener's slot, zeroed storage the caller owns.
struct Hook {
    void* link[2];
    const void* functions;
    void* data;
    void (*removed)(Hook*);
    void* priv;
};
// struct spa_pod: the header of every POD; the body follows, padded to 8 bytes.
struct Pod {
    u32 size;
    u32 type;
};
// struct spa_chunk, spa_data, spa_meta, spa_buffer, pw_buffer and spa_meta_header.
struct Chunk {
    u32 offset;
    u32 size;
    i32 stride;
    i32 flags;
};
struct Data {
    u32 type;
    u32 flags;
    i64 fd;
    u32 mapoffset;
    u32 maxsize;
    void* data;
    Chunk* chunk;
};
struct Meta {
    u32 type;
    u32 size;
    void* data;
};
struct SpaBuffer {
    u32 n_metas;
    u32 n_datas;
    Meta* metas;
    Data* datas;
};
struct Buffer {
    SpaBuffer* buffer;
    void* user_data;
    u64 size;
};
struct MetaHeader {
    u32 flags;
    u32 offset;
    i64 pts;
    i64 dts_offset;
    u64 seq;
};
// struct pw_stream_events up to `drained` (version 0: the library reads no later member).
struct StreamEvents {
    u32 version;
    void (*destroy)(void* data) noexcept;
    void (*state_changed)(void* data, i32 old_state, i32 state, const char* error) noexcept;
    void (*control_info)(void* data, u32 id, const void* control) noexcept;
    void (*io_changed)(void* data, u32 id, void* area, u32 size) noexcept;
    void (*param_changed)(void* data, u32 id, const Pod* param) noexcept;
    void (*add_buffer)(void* data, Buffer* buffer) noexcept;
    void (*remove_buffer)(void* data, Buffer* buffer) noexcept;
    void (*process)(void* data) noexcept;
    void (*drained)(void* data) noexcept;
};
// struct spa_dict and its items: properties (key=value strings) of registry globals and of objects created.
struct DictItem {
    const char* key;
    const char* value;
};
struct Dict {
    u32 flags;
    u32 n_items;
    const DictItem* items;
};
// struct spa_interface: what pw_core and pw_registry are, so their methods (inline in PipeWire's headers) are called
// through the method table in cb.funcs with cb.data as the object.
struct Callbacks {
    const void* funcs;
    void* data;
};
struct Interface {
    const char* type;
    u32 version;
    Callbacks cb;
};
// struct pw_core_methods up to create_object, pw_registry_methods up to bind, pw_registry_events, pw_node_methods up to
// add_listener, pw_node_events and the leading fields of pw_node_info up to props (the node's full properties, which
// the registry's global does not carry).
struct RegistryEvents {
    u32 version;
    void (*global)(void* data, u32 id, u32 permissions, const char* type, u32 version, const Dict* props) noexcept;
    void (*global_remove)(void* data, u32 id) noexcept;
};
struct CoreMethods {
    u32 version;
    const void* add_listener;
    const void* hello;
    const void* sync;
    const void* pong;
    const void* error;
    Registry* (*get_registry)(void* object, u32 version, usize user_data_size);
    void* (*create_object)(void* object, const char* factory_name, const char* type, u32 version, const Dict* props,
                           usize user_data_size);
};
struct RegistryMethods {
    u32 version;
    int (*add_listener)(void* object, Hook* listener, const RegistryEvents* events, void* data);
    Proxy* (*bind)(void* object, u32 id, const char* type, u32 version, usize user_data_size);
};
struct NodeInfo {
    u32 id;
    u32 max_input_ports, max_output_ports;
    u64 change_mask;
    u32 n_input_ports, n_output_ports;
    i32 state;
    const char* error;
    const Dict* props;
};
struct NodeEvents {
    u32 version;
    void (*info)(void* data, const NodeInfo* info) noexcept;
    const void* param;
};
struct NodeMethods {
    u32 version;
    int (*add_listener)(void* object, Hook* listener, const NodeEvents* events, void* data);
};

// tools/check_pipewire_abi.py compares these numbers with the installed headers.
static_assert(sizeof(Hook) == 48 && sizeof(Pod) == 8 && sizeof(Chunk) == 16 && sizeof(Data) == 40 &&
              sizeof(Meta) == 16 && sizeof(SpaBuffer) == 24 && sizeof(MetaHeader) == 32);
static_assert(__builtin_offsetof(Data, fd) == 8 && __builtin_offsetof(Data, chunk) == 32 &&
              __builtin_offsetof(SpaBuffer, datas) == 16 && __builtin_offsetof(MetaHeader, pts) == 8 &&
              __builtin_offsetof(Buffer, size) == 16);
static_assert(__builtin_offsetof(StreamEvents, state_changed) == 16 &&
              __builtin_offsetof(StreamEvents, param_changed) == 40 && __builtin_offsetof(StreamEvents, process) == 64 &&
              __builtin_offsetof(StreamEvents, drained) == 72);
static_assert(sizeof(Dict) == 16 && sizeof(DictItem) == 16 && sizeof(Interface) == 32 && sizeof(RegistryEvents) == 24);
static_assert(__builtin_offsetof(Dict, items) == 8 && __builtin_offsetof(Interface, cb) == 16 &&
              __builtin_offsetof(RegistryEvents, global) == 8 && __builtin_offsetof(RegistryEvents, global_remove) == 16 &&
              __builtin_offsetof(CoreMethods, get_registry) == 48 && __builtin_offsetof(CoreMethods, create_object) == 56 &&
              __builtin_offsetof(RegistryMethods, add_listener) == 8 && __builtin_offsetof(RegistryMethods, bind) == 16);
static_assert(__builtin_offsetof(NodeInfo, props) == 48 && sizeof(NodeEvents) == 24 &&
              __builtin_offsetof(NodeEvents, info) == 8 && __builtin_offsetof(NodeMethods, add_listener) == 8);

inline constexpr u32 kTypeId = 3;          // SPA_TYPE_Id
inline constexpr u32 kTypeInt = 4;         // SPA_TYPE_Int
inline constexpr u32 kTypeLong = 5;        // SPA_TYPE_Long
inline constexpr u32 kTypeRectangle = 10;  // SPA_TYPE_Rectangle
inline constexpr u32 kTypeFraction = 11;   // SPA_TYPE_Fraction
inline constexpr u32 kTypeObject = 15;     // SPA_TYPE_Object
inline constexpr u32 kTypeChoice = 19;     // SPA_TYPE_Choice
inline constexpr u32 kTypeArray = 13;      // SPA_TYPE_Array
inline constexpr u32 kChoiceNone = 0;      // SPA_CHOICE_None
inline constexpr u32 kChoiceRange = 1;     // SPA_CHOICE_Range
inline constexpr u32 kChoiceEnum = 3;      // SPA_CHOICE_Enum
inline constexpr u32 kChoiceFlags = 4;     // SPA_CHOICE_Flags
inline constexpr u32 kPropMandatory = 8;   // SPA_POD_PROP_FLAG_MANDATORY
inline constexpr u32 kPropDontFixate = 16; // SPA_POD_PROP_FLAG_DONT_FIXATE

inline constexpr u32 kObjectFormat = 0x40003;        // SPA_TYPE_OBJECT_Format
inline constexpr u32 kObjectParamBuffers = 0x40004;  // SPA_TYPE_OBJECT_ParamBuffers
inline constexpr u32 kObjectParamMeta = 0x40005;     // SPA_TYPE_OBJECT_ParamMeta
inline constexpr u32 kParamEnumFormat = 3;           // SPA_PARAM_EnumFormat
inline constexpr u32 kParamFormat = 4;               // SPA_PARAM_Format
inline constexpr u32 kParamBuffers = 5;              // SPA_PARAM_Buffers
inline constexpr u32 kParamMeta = 6;                 // SPA_PARAM_Meta

inline constexpr u32 kFormatMediaType = 1;            // SPA_FORMAT_mediaType
inline constexpr u32 kFormatMediaSubtype = 2;         // SPA_FORMAT_mediaSubtype
inline constexpr u32 kFormatVideoFormat = 0x20001;    // SPA_FORMAT_VIDEO_format
inline constexpr u32 kFormatVideoModifier = 0x20002;  // SPA_FORMAT_VIDEO_modifier
inline constexpr u32 kFormatVideoSize = 0x20003;      // SPA_FORMAT_VIDEO_size
inline constexpr u32 kFormatVideoFramerate = 0x20004; // SPA_FORMAT_VIDEO_framerate
inline constexpr u32 kMediaTypeVideo = 2;             // SPA_MEDIA_TYPE_video
inline constexpr u32 kMediaSubtypeRaw = 1;            // SPA_MEDIA_SUBTYPE_raw
inline constexpr u32 kMediaTypeAudio = 1;             // SPA_MEDIA_TYPE_audio
inline constexpr u32 kFormatAudioFormat = 0x10001;    // SPA_FORMAT_AUDIO_format
inline constexpr u32 kFormatAudioRate = 0x10003;      // SPA_FORMAT_AUDIO_rate
inline constexpr u32 kFormatAudioChannels = 0x10004;  // SPA_FORMAT_AUDIO_channels
inline constexpr u32 kFormatAudioPosition = 0x10005;  // SPA_FORMAT_AUDIO_position
inline constexpr u32 kAudioFormatS16 = 259;           // SPA_AUDIO_FORMAT_S16_LE
inline constexpr u32 kChannelFl = 3;                  // SPA_AUDIO_CHANNEL_FL
inline constexpr u32 kChannelFr = 4;                  // SPA_AUDIO_CHANNEL_FR
inline constexpr u32 kVideoFormatRgbx = 7;            // SPA_VIDEO_FORMAT_RGBx
inline constexpr u32 kVideoFormatBgrx = 8;            // SPA_VIDEO_FORMAT_BGRx
inline constexpr u32 kVideoFormatRgba = 11;           // SPA_VIDEO_FORMAT_RGBA
inline constexpr u32 kVideoFormatBgra = 12;           // SPA_VIDEO_FORMAT_BGRA

inline constexpr u32 kBuffersBuffers = 1;   // SPA_PARAM_BUFFERS_buffers
inline constexpr u32 kBuffersDataType = 6;  // SPA_PARAM_BUFFERS_dataType
inline constexpr u32 kMetaType = 1;         // SPA_PARAM_META_type
inline constexpr u32 kMetaSize = 2;         // SPA_PARAM_META_size
inline constexpr u32 kMetaHeader = 1;       // SPA_META_Header
inline constexpr u32 kDataDmaBuf = 3;       // SPA_DATA_DmaBuf
inline constexpr i32 kChunkCorrupted = 1;   // SPA_CHUNK_FLAG_CORRUPTED
inline constexpr u32 kHeaderCorrupted = 2;  // SPA_META_HEADER_FLAG_CORRUPTED

inline constexpr i32 kStreamStateError = -1;      // PW_STREAM_STATE_ERROR
inline constexpr i32 kStreamStateUnconnected = 0; // PW_STREAM_STATE_UNCONNECTED
inline constexpr i32 kStreamStatePaused = 2;      // PW_STREAM_STATE_PAUSED
inline constexpr i32 kStreamStateStreaming = 3;   // PW_STREAM_STATE_STREAMING
inline constexpr u32 kDirectionInput = 0;         // PW_DIRECTION_INPUT
inline constexpr u32 kDirectionOutput = 1;        // PW_DIRECTION_OUTPUT
inline constexpr u32 kStreamAutoconnect = 1;      // PW_STREAM_FLAG_AUTOCONNECT
inline constexpr u32 kStreamMapBuffers = 4;       // PW_STREAM_FLAG_MAP_BUFFERS
inline constexpr u32 kAnyId = 0xFFFFFFFFu;        // PW_ID_ANY
inline constexpr u32 kVersionRegistry = 3;        // PW_VERSION_REGISTRY
inline constexpr u32 kVersionLink = 3;            // PW_VERSION_LINK
inline constexpr u32 kVersionNode = 3;            // PW_VERSION_NODE
inline constexpr char kTypeNode[] = "PipeWire:Interface:Node";
inline constexpr char kTypePort[] = "PipeWire:Interface:Port";
inline constexpr char kTypeLink[] = "PipeWire:Interface:Link";

// The entry points of libpipewire-0.3: F(return type, name, (argument types)). Filled by pipewire_api().
#define MAICORD_PIPEWIRE_FUNCTIONS(F)                                                                                  \
    F(void, pw_init, (int*, char***))                                                                                  \
    F(ThreadLoop*, pw_thread_loop_new, (const char*, const void*))                                                     \
    F(void, pw_thread_loop_destroy, (ThreadLoop*))                                                                     \
    F(int, pw_thread_loop_start, (ThreadLoop*))                                                                        \
    F(void, pw_thread_loop_stop, (ThreadLoop*))                                                                        \
    F(void, pw_thread_loop_lock, (ThreadLoop*))                                                                        \
    F(void, pw_thread_loop_unlock, (ThreadLoop*))                                                                      \
    F(Loop*, pw_thread_loop_get_loop, (ThreadLoop*))                                                                   \
    F(Context*, pw_context_new, (Loop*, Properties*, usize))                                                           \
    F(void, pw_context_destroy, (Context*))                                                                            \
    F(Core*, pw_context_connect, (Context*, Properties*, usize))                                                       \
    F(Core*, pw_context_connect_fd, (Context*, int, Properties*, usize))                                               \
    F(int, pw_core_disconnect, (Core*))                                                                                \
    F(Properties*, pw_properties_new_string, (const char*))                                                            \
    F(Stream*, pw_stream_new, (Core*, const char*, Properties*))                                                       \
    F(void, pw_stream_destroy, (Stream*))                                                                              \
    F(void, pw_stream_add_listener, (Stream*, Hook*, const StreamEvents*, void*))                                      \
    F(int, pw_stream_connect, (Stream*, u32, u32, u32, const Pod**, u32))                                              \
    F(u32, pw_stream_get_node_id, (Stream*))                                                                           \
    F(void, pw_proxy_destroy, (Proxy*))                                                                                \
    F(Buffer*, pw_stream_dequeue_buffer, (Stream*))                                                                    \
    F(int, pw_stream_queue_buffer, (Stream*, Buffer*))                                                                 \
    F(int, pw_stream_update_params, (Stream*, const Pod**, u32))

struct Api {
#define MAICORD_PIPEWIRE_MEMBER(ret, name, args) ret(*name) args;
    MAICORD_PIPEWIRE_FUNCTIONS(MAICORD_PIPEWIRE_MEMBER)
#undef MAICORD_PIPEWIRE_MEMBER
};

} // namespace maicord::pipewire
