#pragma once

#include "maicord/core/base.hpp"

// The libdav1d and libopenh264 interfaces of the software decode tier (Linux), declared without their headers. Both
// libraries are dlopen()ed ("libdav1d.so.7" or ".6"; "libopenh264.so.7", ".6" or ".8"), so a machine without them
// simply has no software tier. Types keep the libraries' binary layout: only the leading fields MaiCord reads are
// declared, the rest of each struct is storage sized above the largest version (tools/check_video_abi.py compares
// every constant, offset, size bound and function signature with the installed libdav1d-dev and libopenh264-dev
// headers).

namespace maicord::software {

// ---- libdav1d (AV1) ----------------------------------------------------------------------------------------------

struct Dav1dContext;

// Dav1dSettings: the leading fields (stable since 1.0); the library's own dav1d_default_settings fills all of it.
struct Dav1dSettings {
    i32 n_threads;
    i32 max_frame_delay;
    i32 apply_grain;
    i32 operating_point;
    i32 all_layers;
    u32 frame_size_limit;
    void* allocator[3]; // Dav1dPicAllocator: cookie and the two callbacks
    void* logger_cookie;
    // Dav1dLogger.callback(cookie, format, va_list): a va_list is a pointer here, so a function that ignores it
    // silences the library's stderr messages.
    void (*logger_callback)(void*, const char*, void*);
    u8 rest[160];
};
// Dav1dData: data and size, then the reference and the properties the library manages.
struct Dav1dData {
    const u8* data;
    usize size;
    u8 rest[96];
};
// Dav1dSequenceHeader: the fields that decide the colour conversion.
struct Dav1dSequenceHeader {
    u8 profile;
    i32 max_width, max_height;
    i32 layout;
    i32 primaries, transfer, matrix, chroma_position;
    u8 hbd;
    u8 color_range;
};
// Dav1dPicture: owned by the library (dav1d_picture_unref releases it); only the leading fields are read.
struct Dav1dPicture {
    const Dav1dSequenceHeader* sequence;
    const void* frame_header;
    void* data[3];
    i64 stride[2];
    i32 width, height;
    i32 layout;
    i32 bits_per_component;
    u8 rest[352];
};

inline constexpr i32 kDav1dLayoutI420 = 1; // DAV1D_PIXEL_LAYOUT_I420
inline constexpr i32 kDav1dEagain = 11; // EAGAIN, returned negated

#define MAICORD_DAV1D_FUNCTIONS(F)                                                                                     \
    F(void, dav1d_default_settings, (Dav1dSettings*))                                                                  \
    F(i32, dav1d_open, (Dav1dContext**, const Dav1dSettings*))                                                         \
    F(void, dav1d_close, (Dav1dContext**))                                                                             \
    F(void, dav1d_flush, (Dav1dContext*))                                                                              \
    F(u8*, dav1d_data_create, (Dav1dData*, usize))                                                                     \
    F(void, dav1d_data_unref, (Dav1dData*))                                                                            \
    F(i32, dav1d_send_data, (Dav1dContext*, Dav1dData*))                                                               \
    F(i32, dav1d_get_picture, (Dav1dContext*, Dav1dPicture*))                                                          \
    F(void, dav1d_picture_unref, (Dav1dPicture*))

struct Dav1dApi {
#define MAICORD_DAV1D_MEMBER(ret, name, args) ret(*name) args;
    MAICORD_DAV1D_FUNCTIONS(MAICORD_DAV1D_MEMBER)
#undef MAICORD_DAV1D_MEMBER
};

// ---- libopenh264 (H.264) ----------------------------------------------------------------------------------------

// ISVCDecoder of the C API is a pointer to a table of functions (the layout of the C++ class's virtual table); a decoder
// handle is a pointer to that pointer.
struct IsvcDecoderTable;
using IsvcDecoder = const IsvcDecoderTable*;
struct SBufferInfo {
    i32 buffer_status; // iBufferStatus: 1 when a picture is ready
    u64 in_timestamp;
    u64 out_timestamp;
    i32 width, height, format;
    i32 stride[2];
    u8* planes[3]; // pDst
};
struct SDecodingParam {
    char* file_name_restructed;
    u32 cpu_load;
    u8 target_dq_layer;
    i32 error_concealment; // eEcActiveIdc
    bool parse_only;
    u32 video_property_size; // sVideoProperty.size
    i32 bitstream_type;      // sVideoProperty.eVideoBsType
};
struct IsvcDecoderTable {
    long (*Initialize)(IsvcDecoder*, const SDecodingParam*);
    long (*Uninitialize)(IsvcDecoder*);
    i32 (*DecodeFrame)(IsvcDecoder*, const u8*, i32, u8**, i32*, i32*, i32*);
    i32 (*DecodeFrameNoDelay)(IsvcDecoder*, const u8*, i32, u8**, SBufferInfo*);
    i32 (*DecodeFrame2)(IsvcDecoder*, const u8*, i32, u8**, SBufferInfo*);
    i32 (*FlushFrame)(IsvcDecoder*, u8**, SBufferInfo*);
    void* DecodeParser;
    void* DecodeFrameEx;
    long (*SetOption)(IsvcDecoder*, i32, void*);
    long (*GetOption)(IsvcDecoder*, i32, void*);
};

// DECODING_STATE bits
inline constexpr i32 kDsRefLost = 0x02;          // dsRefLost
inline constexpr i32 kDsBitstreamError = 0x04;   // dsBitstreamError
inline constexpr i32 kDsDepLayerLost = 0x08;     // dsDepLayerLost
inline constexpr i32 kDsNoParamSets = 0x10;      // dsNoParamSets
inline constexpr i32 kDsDataErrorConcealed = 0x20; // dsDataErrorConcealed
inline constexpr i32 kDsInvalidArgument = 0x1000; // dsInvalidArgument
inline constexpr i32 kDsOutOfMemory = 0x4000;    // dsOutOfMemory
inline constexpr i32 kErrorConcealmentDisable = 0; // ERROR_CON_DISABLE
inline constexpr i32 kBitstreamAvc = 0;            // VIDEO_BITSTREAM_AVC
inline constexpr i32 kVideoFormatI420 = 23;        // videoFormatI420

struct OpenH264Api {
    long (*WelsCreateDecoder)(IsvcDecoder**);
    void (*WelsDestroyDecoder)(IsvcDecoder*);
};

static_assert(sizeof(Dav1dSettings) >= 128 && sizeof(Dav1dPicture) <= 512);
static_assert(__builtin_offsetof(Dav1dSettings, logger_callback) == 56);
static_assert(__builtin_offsetof(Dav1dPicture, data) == 16);
static_assert(__builtin_offsetof(Dav1dPicture, stride) == 40);
static_assert(__builtin_offsetof(Dav1dPicture, width) == 56);
static_assert(__builtin_offsetof(Dav1dSequenceHeader, hbd) == 32);
static_assert(__builtin_offsetof(Dav1dSequenceHeader, color_range) == 33);
static_assert(__builtin_offsetof(Dav1dSequenceHeader, matrix) == 24);
static_assert(__builtin_offsetof(Dav1dData, size) == 8);
static_assert(__builtin_offsetof(SBufferInfo, width) == 24);
static_assert(__builtin_offsetof(SBufferInfo, stride) == 36);
static_assert(__builtin_offsetof(SBufferInfo, planes) == 48);
static_assert(sizeof(SBufferInfo) == 72);
static_assert(__builtin_offsetof(SDecodingParam, error_concealment) == 16);
static_assert(__builtin_offsetof(SDecodingParam, video_property_size) == 24);
static_assert(sizeof(SDecodingParam) == 32);

} // namespace maicord::software
