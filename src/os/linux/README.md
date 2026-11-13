# Linux OS layer

`src/os/linux` holds the Linux bodies of the neutral seams in `include/maicord/os` (files, directories, directory watch,
mmap and pages, threads, waits, processes, local time, text and date formatting). Win32 names stay the shared vocabulary
(`FileHandle`, `Waitable`, `thread_last_error()`); the layer maps errno onto them (`include/maicord/os/linux/error.hpp`).

## Kernel floor

Linux 6.1 is the floor and every call is used unconditionally: no version probes, no `uname` checks, no alternate
implementation for an older kernel (`statx`, `renameat2`, `close_range` with `CLOSE_RANGE_CLOEXEC` in `process_replace`,
`memfd_create`, `getdents64`, `inotify`, `timerfd`, `eventfd2`, `getrandom`, private futexes). The two places that retry
a call are file-system gaps, not kernel ones: `file_rename_noreplace` falls back to `link` + `unlink` when the file system
has no `RENAME_NOREPLACE`, and `sync_parent` accepts a file system that cannot sync a directory.

## Rules

- A `Thread` is a glibc pthread over a fixed table of 64 records (Mesa, libEGL and libwayland need a real thread control
  block). `thread_main` signals the record's `done` event (`thread_waitable`) just before it publishes the state.
- A wait is one `ppoll` over eventfds, timerfds and borrowed descriptors (`wait.cpp`); there is no futex wait on several
  words, which is why `futex_waitv` has no use here. Every futex user (`srwlock.cpp`, which the page table and the log
  lock with, the thread table, the OpenSSL/TLS locks) waits on one word.
- The network loop's ring and its file operations are in `src/platform/net/linux`, not here.
- Fewer system calls where the kernel does the work: `file_copy_new` copies with `sendfile` (any output file since
  2.6.33; one call per 2 GiB instead of a read and a write per 32 KiB), and `data_path`/`state_path` with
  `create_parents` ask one `statx` whether the file's directory exists before they create anything (it was a `mkdir`,
  an `open` and a `close` for every ancestor from `/home` down).
- Detached children (`process_launch_detached`, `xdg-open`) start through one helper, `process_spawn_detached`
  (`maicord/sys/linux/process.hpp`): libc's `posix_spawn`/`posix_spawnp` found with `dlsym(RTLD_DEFAULT)` (not
  imported, and no `dlopen` of `libc.so.6`), which glibc and musl run as a `CLONE_VM | CLONE_VFORK` child.

## Evaluated and not adopted

- `openat2` (`RESOLVE_BENEATH`, `NO_SYMLINKS`) for the plugin directory pin (`src/app/plugins/loader.cpp`): the pin
  checks the owner and mode of every ancestor and holds a descriptor for each, and compares `/proc/self/fd` with the
  pinned path to catch a rename or a bind mount; per-component `O_NOFOLLOW` already refuses links, and `RESOLVE_*` replaces
  none of those checks.
- `pidfd`: children (`process_launch_detached`, `xdg-open`) are fire and forget under `SIGCHLD = SIG_IGN`; nothing waits
  for one.
- `timerfd` versus ring timeouts: timer waitables sit in `ppoll` sets with other descriptors (Wayland, D-Bus), which a ring
  timeout cannot join; the network loop's own waits use the ring's timeout.
- `copy_file_range` for `file_copy_new`: since 5.19 it answers `EXDEV` between file systems, so it needs `sendfile`
  beside it anyway; `sendfile` alone covers both (the only caller is the updater's last-resort restore).
- `O_TMPFILE` + `linkat` for the master key file (`src/platform/crypto/linux/secure_buffer.cpp`): it saves one `unlink`
  of a file written once per profile, and needs the named temporary file as well where the file system has no
  `O_TMPFILE`.
- An own `clone3(CLONE_VFORK)` spawn: `posix_spawn` is that already, and an own one needs a `PATH` search (`xdg-open`)
  and a child stack in assembly.
- A vDSO lookup for `clock_gettime` instead of the libc import: more code than the import saves, and a system call
  fallback where the vDSO is off.
- `MADV_POPULATE_READ`/`WRITE` (5.14): nothing touches pages to fault them in (the ring maps with `MAP_POPULATE`).
- Landlock self-sandboxing: not sensible for the client. A Landlock domain is inherited by every child, so a browser
  started through `xdg-open` would run restricted; plugins are in-process shared objects, so a domain cannot separate them
  from the client; and the allowlist the client needs (XDG directories, any file the user picks, GPU and Wayland and D-Bus
  sockets, `/proc`, `/sys`) leaves little to deny. Flatpak and the portals are the sandbox that fits. (Landlock ABI 4, TCP
  port rules, is Linux 6.7; 6.1 has ABI 2.)
- `rseq`: nothing here is a per-CPU hot path.
