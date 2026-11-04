#pragma once

// Shared by the parts of the core test executable (test_main.cpp and tests/**/core_*.cpp): the
// expectation sink, helpers and buffers used by more than one part, and the declarations of the
// ordered cases in core_test_cases.inc. Each buffer is defined in the part that owns it.

#include "maicord/model/chat/message_rich_store.hpp"

#include "maicord/sys/crt.hpp"

namespace maicord {

#define CORE_LOCAL_TEST(name) void name() noexcept;
#define CORE_EXTERNAL_TEST(name) void name(void (*check)(bool, const char*) noexcept) noexcept;
#include "../core_test_cases.inc"
#undef CORE_EXTERNAL_TEST
#undef CORE_LOCAL_TEST

struct JpegWorkspace;
struct LossyWebpWorkspace;
struct Vp8lWorkspace;

// Records a failed expectation under name (defined in test_main.cpp).
void expect(bool condition, const char* name) noexcept;

template <usize Count> constexpr ConstSlice<u8> byte_literal(const char (&text)[Count]) noexcept {
    static_assert(Count > 0);
    return ConstSlice<u8>{reinterpret_cast<const u8*>(text), static_cast<u32>(Count - 1)};
}

// Sentinel bytes proving an output buffer was left untouched on failure.
inline void guard_fill(u8* data, usize bytes, u8 value = 0xA5) noexcept {
    for (usize index = 0; index < bytes; ++index)
        data[index] = value;
}

inline bool guard_intact(const u8* data, usize bytes, u8 value = 0xA5) noexcept {
    for (usize index = 0; index < bytes; ++index)
        if (data[index] != value)
            return false;
    return true;
}

inline bool bytes_equal(ConstSlice<u8> left, ConstSlice<u8> right) noexcept {
    return left.count == right.count && maicord::bytes_equal(left.data, right.data, left.count);
}

extern MessageRichRecord g_message_scene_rich_records[32];
extern u16 g_message_scene_rich_order[32];
extern u8 g_message_scene_rich_text[16 * 1024];
extern u8 g_png_encoded[17'000];
extern u8 g_png_zlib[17'000];
alignas(u32) extern u8 g_png_scanlines[17'000];
extern Vp8lWorkspace g_vp8l_workspace;
extern LossyWebpWorkspace g_lossy_webp_workspace;
extern JpegWorkspace g_jpeg_workspace;

// Offset of the occurrence-th chunk of the given type, or 0xFFFFFFFF (defined in core_codecs.cpp).
u32 test_find_png_chunk(ConstSlice<u8> encoded, u32 type, u32 occurrence) noexcept;
// Recomputes the CRC of the chunk at chunk_offset (defined in core_codecs.cpp).
bool test_rewrite_png_chunk_crc(Slice<u8> encoded, u32 chunk_offset) noexcept;

} // namespace maicord
