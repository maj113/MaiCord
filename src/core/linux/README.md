# Linux runtime, C library and ELF contract

One freestanding x86-64 image per build: a dynamic PIE with its own `_start` (`start.cpp`), raw syscalls
(`include/maicord/sys/linux`), `memcpy` and friends (`runtime.cpp`), no CRT, no exceptions, no STL. Everything else
(Mesa, wayland, libpulse, OpenSSL, fontconfig, VA-API, ...) is `dlopen`ed by soname (`shared_library.cpp`). The libraries
the image loads are built for the system's C library, so the image always runs on the system's dynamic loader and never
bundles one.

## C library: glibc and musl, one image

- The build links the glibc form: interpreter `/lib64/ld-linux-x86-64.so.2`, `DT_NEEDED` `libc.so.6` alone (one generated
  stub library, `tools/generate_libc_stubs.py`, pinning each import at the oldest default version glibc 2.34's `libc.so.6`
  exports: `GLIBC_2.2.5` (`malloc` family, `getaddrinfo`, `pthread_attr_init`/`pthread_attr_destroy`), `GLIBC_2.17`
  (`clock_gettime`) and `GLIBC_2.34` (`dlopen` family, `pthread_create`/`join`/`detach`/`attr_setstacksize`), so glibc 2.34+
  runs it). glibc 2.34 moved `dlopen` and the pthread functions out of `libdl.so.2` and `libpthread.so.0` (empty since) into
  `libc.so.6`; every distribution the kernel floor (Linux 6.1) admits has at least that: Red Hat 9 and its rebuilds 2.34,
  Ubuntu 22.04 2.35, Debian 12 2.36, Fedora, Arch and NixOS newer. Its libc imports are the ones of
  `include/maicord/sys/linux/libc.hpp` (dlopen family, `pthread_create` family,
  `malloc`/`calloc`/`free`, `getaddrinfo`, `clock_gettime`); `malloc_trim` is glibc-only and a *weak* import.
- The musl image is the same bytes with another interpreter: `python3 tools/retarget_libc.py --libc musl MaiCord
  MaiCord-musl` rewrites PT_INTERP to `/lib/ld-musl-x86_64.so.1` and nothing else, and `tools/audit_elf.py --libc musl`
  audits it. It needs nothing more because musl's loader (`ldso/dynlink.c`, `load_library`) answers any needed `libc.*`
  (and `libdl.*`, `libpthread.*`, `libm.*`, `librt.*`, `libutil.*`, `libxnet.*`) by itself whatever the version suffix, takes the
  imports from itself and ignores symbol versions. The tests of a glibc build therefore are the tests of the musl image
  (`tools/test_linux_distros.sh` retargets its bundle for Alpine, Void musl and Chimera).
- `_start` tells the libc by `dlsym(RTLD_DEFAULT, "gnu_get_libc_version")` (glibc only). On musl it calls the libc's
  `__libc_start_main`, which is what crt1 does and its loader does not: `__progname`/`program_invocation_name` (Mesa
  `strrchr()`s it: a null one crashes `eglInitialize`), the stack canary, thread bookkeeping. glibc's loader has done all of
  it before `_start`, so glibc takes the direct path. `process_libc_is_musl()` exposes the result (the self-updater never
  replaces a musl install: the feeds carry the glibc tarball).
- Pinned and checked against the installed headers by `tools/check_libc_abi.py` (CI on glibc, in the Alpine container for
  musl): `struct addrinfo` layout, `pthread_attr_t` (56 bytes, 8-aligned) and `pthread_t`, the `RTLD_*` values and
  `RTLD_DEFAULT == 0`, `CLOCK_*`, every `E*` number the code uses (and `EWOULDBLOCK == EAGAIN`, `ENOTSUP == EOPNOTSUPP`), the
  `EAI_*` codes `src/platform/net/linux/resolver.cpp` maps.
- Adding a libc import: declare it in `libc.hpp`, add it to `tools/generate_libc_stubs.py` and the allow-list of
  `tools/audit_elf.py`, and make sure musl defines it (else import it weakly and test the pointer, as `heap_trim` does).
- Not done: a separate musl link (the extra full-LTO link would only differ in PT_INTERP), a static or self-loading
  image (a static-PIE launcher that `execve`s the system loader changes `/proc/self/exe`, the process name and the
  AppArmor/SELinux path to the loader's), our own resolver instead of `getaddrinfo` (NSS, systemd-resolved, mDNS).

## musl behaviour that differs

- musl's allocator (mallocng) serves blocks from a few KiB up with an `mmap`/`mprotect`/`munmap` each (no arena): the
  startup's allocation churn is several thousand mappings; on the same bytes with the same fonts the first frame comes at
  ~1.14 s against 0.83 s on glibc (`ui_ready` 365 ms against 252 ms). Steady-state idle cost is the same (0.1-0.2% CPU,
  ~183 MB RSS). A malloc replacement would need `LD_PRELOAD` or an in-image allocator.
- `dlclose` never unloads, so unloading a native plugin (`src/app/plugins/loader.cpp`: sealed memfd copy, `dlopen`,
  `dlclose`) keeps its mapping; the next copy loads fresh (musl tells modules apart by inode, not by name) and nothing of
  the old one runs. `maicord_plugin_loader_elf` and `maicord_plugin_binding` assert that the image is gone from
  `/proc/self/maps` after the unload, which only glibc satisfies (they are not in the portable subset). An own ELF mapper
  for the freestanding plugin images would make the unload libc-independent.
- `_start` finds its program headers through `__ehdr_start`, not `AT_PHDR`: started as `ld-musl-x86_64.so.1 MaiCord`
  (any form of the image) musl leaves the auxiliary vector as the kernel built it, so `AT_PHDR` names the loader's headers.
  glibc's `ld.so image` fixes the vector. Both loaders run the image this way, but `/proc/self/exe` and the process name
  are then the loader's (`ld-linux-x86-64`), which is why this is not the way the image starts.
- Thread stacks default to 128 KiB (every MaiCord thread passes its own size, at least 128 KiB: `src/os/linux/thread.cpp`).
- `getaddrinfo` reads `/etc/hosts` and `/etc/resolv.conf` only (no NSS).

## runtime.cpp: memory functions

Up to 128 bytes: overlapping 16-byte loads and stores. Above it, a forward copy and `memset` run a 4x32-byte AVX loop into a
32-byte aligned destination (head and tail stored last); from 1 MiB they use non-temporal stores plus `sfence`. A copy
whose destination overlaps the source from above runs downward in 16-byte steps. No `rep movsb`/`rep stosb`: the string
instructions are byte-wise into write-combined memory (`glMapBufferRange` mappings of d3d12 and real GPU drivers: 1 MiB
took 98 ms against 3 ms), and an AVX loop is faster up to the last-level cache. `tests/core/core_runtime.cpp` covers the
sweep, the overlaps, the guard-page bounds and the streaming sizes.

## ELF audit

`tools/audit_elf.py` (run by `tools/build_linux.sh` on every image): x86-64 ET_DYN, the interpreter of its libc, exactly one
needed library (`libc.so.6`), only the allowed imports under `GLIBC_` nodes of at most 2.34 (`malloc_trim` weak), no unwind or debug
sections, no TEXTREL/RPATH/RUNPATH, `DT_RELR` absent (relative relocations are packed under the Android tags and applied
by `start.cpp`: glibc's own `DT_RELR` needs 2.36 and a version node), full RELRO, a non-executable stack, no W+X segment.
