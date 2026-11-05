#pragma once

// The C library functions MaiCord imports, declared as glibc and musl declare them (the two agree on x86-64; types
// without the headers). These are the only imports of a Linux image besides what it dlopens: libc.so.6, libdl.so.2
// and libpthread.so.0 are linked as stub libraries (tools/generate_libc_stubs.py) that carry the oldest symbol
// versions, so the image never requires a newer glibc than 2.17; musl's loader answers those names itself and ignores
// the versions (tools/retarget_libc.py makes the musl image). Add a function only together with its stub entry there,
// and only if musl defines it too (or import it weakly, as malloc_trim). tools/check_libc_abi.py compares the types
// and constants below with the installed C library's headers.

#include "maicord/sys/linux/abi.hpp"

extern "C" {

// <dlfcn.h> (libdl.so.2, GLIBC_2.2.5)
void* dlopen(const char* file, int mode);
void* dlsym(void* handle, const char* name);
int dlclose(void* handle);
char* dlerror(void);

// <pthread.h> (libpthread.so.0, GLIBC_2.2.5). The attribute is null or a PthreadAttribute.
int pthread_create(unsigned long* thread, const void* attributes, void* (*start)(void*), void* argument);
int pthread_join(unsigned long thread, void** result);
int pthread_detach(unsigned long thread);
int pthread_attr_init(void* attribute);
int pthread_attr_setstacksize(void* attribute, unsigned long bytes);
int pthread_attr_destroy(void* attribute);

// <stdlib.h> and <malloc.h> (libc.so.6, GLIBC_2.2.5): the process heap, behind maicord/core/heap.hpp. malloc_trim is
// glibc's alone: it is a weak import, null where the C library has none (musl), and heap_trim tests it.
void* malloc(unsigned long bytes);
void* calloc(unsigned long count, unsigned long bytes);
void free(void* block);
[[gnu::weak]] int malloc_trim(unsigned long pad);

// <netdb.h> (libc.so.6, GLIBC_2.2.5): name resolution. It blocks, so the network layer calls it on a pool thread.
int getaddrinfo(const char* node, const char* service, const maicord::AddrInfo* hints, maicord::AddrInfo** result);
void freeaddrinfo(maicord::AddrInfo* result);

// <time.h> (libc.so.6, GLIBC_2.17): the vDSO fast path.
int clock_gettime(int clock, maicord::TimeSpec* time);
}

namespace maicord {

// <dlfcn.h> modes (the same values in glibc and musl).
inline constexpr int kDlLazy = 0x1;
inline constexpr int kDlNow = 0x2;
inline constexpr int kDlLocal = 0x0;
inline constexpr int kDlGlobal = 0x100;

// <pthread.h>: the opaque pthread_attr_t of x86-64 glibc and musl (56 bytes, 8-byte aligned).
struct alignas(8) PthreadAttribute {
    unsigned char bytes[56];
};

} // namespace maicord
