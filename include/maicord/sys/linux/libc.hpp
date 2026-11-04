#pragma once

// The C library functions MaiCord imports, declared as glibc declares them (types without the headers). These are
// the only imports of a Linux image besides what it dlopens: libc.so.6, libdl.so.2 and libpthread.so.0 are linked
// as stub libraries (tools/generate_libc_stubs.py) that carry the oldest symbol versions, so the image never
// requires a newer glibc than 2.17. Add a function only together with its stub entry there.

#include "maicord/sys/linux/abi.hpp"

extern "C" {

// <dlfcn.h> (libdl.so.2, GLIBC_2.2.5)
void* dlopen(const char* file, int mode);
void* dlsym(void* handle, const char* name);
int dlclose(void* handle);
char* dlerror(void);

// <pthread.h> (libpthread.so.0, GLIBC_2.2.5). The attribute is passed as null.
int pthread_create(unsigned long* thread, const void* attributes, void* (*start)(void*), void* argument);
int pthread_join(unsigned long thread, void** result);
int pthread_detach(unsigned long thread);

// <time.h> (libc.so.6, GLIBC_2.17): the vDSO fast path.
int clock_gettime(int clock, maicord::TimeSpec* time);
}

namespace maicord {

// <dlfcn.h> modes.
inline constexpr int kDlLazy = 0x1;
inline constexpr int kDlNow = 0x2;
inline constexpr int kDlLocal = 0x0;
inline constexpr int kDlGlobal = 0x100;

} // namespace maicord
