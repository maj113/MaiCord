#pragma once
#include "maicord/core/slice.hpp"
namespace maicord {
struct UploadSourceOps {
    void (*retain)(void*) noexcept;
    void (*release)(void*) noexcept;
    // Worker/MTA only. The caller must hold a source reference.
    // Non-null scratch requests up to count bytes. The view expires at the next read.
    // Null scratch with nonzero count requests an optional, exact immutable range.
    // Only immutable storage owned by the source can satisfy this request.
    // That view remains valid across later/concurrent reads until the caller releases
    // its source reference. The caller must not modify, clear, or free the view.
    // Unsupported, empty, or out-of-range null-scratch requests return {}.
    // Null-scratch requests do not allocate, perform I/O, wait, change positions,
    // or change earlier views.
    ConstSlice<u8> (*read)(void*, u64 offset, Slice<u8> scratch) noexcept;
};
struct UploadSource { void* context; const UploadSourceOps* ops; u64 bytes; };
static_assert(sizeof(UploadSourceOps) == 24 && sizeof(UploadSource) == 24);
inline bool upload_source_valid(UploadSource source) noexcept {
    return source.context && source.ops && source.ops->retain && source.ops->release && source.ops->read && source.bytes;
}
inline void upload_source_retain(UploadSource source) noexcept { if (source.context) source.ops->retain(source.context); }
inline void upload_source_release(UploadSource source) noexcept { if (source.context) source.ops->release(source.context); }
}
