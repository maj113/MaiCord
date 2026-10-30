#pragma once
#include "maicord/text/font_geometry.hpp"
#include "maicord/text/base.hpp"
namespace maicord {
struct NativeFontHandle {u32 slot,generation;};
struct NativeFontFace {FontGeometry geometry;SfntCmap cmap;};
struct NativeFontFileSlot {
    HANDLE file;
    ConstSlice<u8> bytes;
    Win32FileIdInfo identity;
    u32 faces;
};
struct NativeFontFaceSlot {
    NativeFontFace face;
    u32 file_slot,references,generation;
};
struct NativeFontStore {
    Slice<NativeFontFileSlot> files;
    Slice<NativeFontFaceSlot> faces;
};
// Optional owning-thread growth. Append zero-initialized slots to the requested
// stable prefixes; preserve all existing slots and their addresses. Called at
// most once per acquire, after duplicate lookup, while its read handle is open.
using NativeFontStoreGrow=bool (*)(void* context,bool files,bool faces) noexcept;
enum class NativeFontError : u8 {none,invalid_input,io,capacity,font};
struct NativeFontResult {
    NativeFontError error;
    SfntStatus font_status;
    u32 native_error;
    constexpr bool ok() const noexcept {return error==NativeFontError::none;}
};
struct NativeFontStoreStats {u64 mapped_bytes;u32 files,faces,references;};
// Caller supplies zero-initialized slots at stable addresses. Handles hold a face
// and its read-only mapping, no COM object. Slots and borrowed views must not be
// copied or modified while in use. Serialize mutations on the owning thread; readers
// can share read-only views while an owner holds the handle and synchronizes its
// final release. Acquire performs file I/O; retain/get/release take handles without
// path lookup. The last release unmaps the file once no other face of it is retained.
// No shutdown/reset operation invalidates outstanding references implicitly.
NativeFontResult native_font_acquire(NativeFontStore* store,const wchar_t* path,
    u32 face_index,NativeFontHandle* output,NativeFontStoreGrow grow=nullptr,void* context=nullptr) noexcept;
bool native_font_retain(NativeFontStore* store,NativeFontHandle handle) noexcept;
bool native_font_release(NativeFontStore* store,NativeFontHandle handle) noexcept;
const NativeFontFace* native_font_get(const NativeFontStore& store,NativeFontHandle handle) noexcept;
NativeFontStoreStats native_font_stats(const NativeFontStore& store) noexcept;
static_assert(sizeof(NativeFontHandle)==8);
} // namespace maicord
