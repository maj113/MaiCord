#pragma once

// A dma-buf source for tests that have no GPU: the dumb buffers of a DRM device (the vgem node of a VM or WSL;
// runners without /dev/dri/card0 skip the tests that use it). A buffer is mapped for the CPU and exported as a PRIME
// descriptor as often as asked, each descriptor a new file for the same dma-buf, as a VA-API driver exports.

#include "maicord/core/base.hpp"
#include "maicord/sys/linux/abi.hpp"
#include "maicord/sys/linux/calls.hpp"

namespace maicord {

struct DumbBuffer {
    u32 handle, pitch;
    u64 size;
    u8* pixels; // the CPU mapping
};

namespace dumb_detail {
constexpr long kIoctl = 16;                        // x86-64 ioctl(2)
constexpr unsigned long kCreateDumb = 0xC02064B2;  // DRM_IOCTL_MODE_CREATE_DUMB
constexpr unsigned long kMapDumb = 0xC01064B3;     // DRM_IOCTL_MODE_MAP_DUMB
constexpr unsigned long kDestroyDumb = 0xC00464B4; // DRM_IOCTL_MODE_DESTROY_DUMB
constexpr unsigned long kPrimeToFd = 0xC00C642D;   // DRM_IOCTL_PRIME_HANDLE_TO_FD
struct Create {
    u32 height, width, bpp, flags, handle, pitch;
    u64 size;
};
struct Map {
    u32 handle, pad;
    u64 offset;
};
struct Prime {
    u32 handle, flags;
    i32 fd;
};
inline long call(i32 fd, unsigned long request, void* argument) noexcept {
    return syscall3(kIoctl, fd, static_cast<long>(request), reinterpret_cast<long>(argument));
}
} // namespace dumb_detail

// The DRM device to make buffers on; negative when this user has none.
inline i32 dumb_open() noexcept {
    return static_cast<i32>(sys_open("/dev/dri/card0", kOpenReadWrite));
}

// A buffer of `rows` rows of `width` pixels of `bpp` bits, zeroed by the kernel, mapped read/write.
inline bool dumb_create(i32 node, u32 width, u32 rows, u32 bpp, DumbBuffer* buffer) noexcept {
    using namespace dumb_detail;
    Create create{rows, width, bpp, 0, 0, 0, 0};
    if (call(node, kCreateDumb, &create) != 0)
        return false;
    Map map{create.handle, 0, 0};
    long mapped = -1;
    if (call(node, kMapDumb, &map) == 0)
        mapped = syscall6(kSysMmap, 0, static_cast<long>(create.size), 3 /* read | write */, kMapShared, node,
                          static_cast<long>(map.offset));
    if (syscall_failed(mapped)) {
        u32 handle = create.handle;
        call(node, kDestroyDumb, &handle);
        return false;
    }
    *buffer = {create.handle, create.pitch, create.size, reinterpret_cast<u8*>(mapped)};
    return true;
}

// A new descriptor of the buffer's dma-buf (close-on-exec); -1 on failure. The caller closes it.
inline i32 dumb_export(i32 node, const DumbBuffer& buffer) noexcept {
    using namespace dumb_detail;
    Prime prime{buffer.handle, 0x80002, -1}; // O_CLOEXEC | O_RDWR
    return call(node, kPrimeToFd, &prime) == 0 ? prime.fd : -1;
}

inline void dumb_destroy(i32 node, DumbBuffer* buffer) noexcept {
    using namespace dumb_detail;
    if (!buffer->pixels)
        return;
    sys_munmap(buffer->pixels, static_cast<usize>(buffer->size));
    u32 handle = buffer->handle;
    call(node, kDestroyDumb, &handle);
    *buffer = {};
}

} // namespace maicord
