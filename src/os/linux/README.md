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
  block). `thread_main` signals the record's `done` event just before it publishes the state, so `thread_exit_code` waits
  out that window when the event is already set.
- A wait is one `ppoll` over eventfds, timerfds and borrowed descriptors (`wait.cpp`); there is no futex wait on several
  words, which is why `futex_waitv` has no use here. Every futex user (`srwlock.cpp`, which the page table and the log
  lock with, the thread table, the OpenSSL/TLS locks) waits on one word.
- The network loop's ring and its file operations are in `src/platform/net/linux`, not here.

## Evaluated and not adopted

- `openat2` (`RESOLVE_BENEATH`, `NO_SYMLINKS`) for the plugin directory pin (`src/app/plugins/loader.cpp`): the pin
  checks the owner and mode of every ancestor and holds a descriptor for each, and compares `/proc/self/fd` with the
  pinned path to catch a rename or a bind mount; per-component `O_NOFOLLOW` already refuses links, and `RESOLVE_*` replaces
  none of those checks.
- `pidfd`: children (`process_launch_detached`, `xdg-open`) are fire and forget under `SIGCHLD = SIG_IGN`; nothing waits
  for one.
- `timerfd` versus ring timeouts: timer waitables sit in `ppoll` sets with other descriptors (Wayland, D-Bus), which a ring
  timeout cannot join; the network loop's own waits use the ring's timeout.
- Landlock self-sandboxing: not sensible for the client. A Landlock domain is inherited by every child, so a browser
  started through `xdg-open` would run restricted; plugins are in-process shared objects, so a domain cannot separate them
  from the client; and the allowlist the client needs (XDG directories, any file the user picks, GPU and Wayland and D-Bus
  sockets, `/proc`, `/sys`) leaves little to deny. Flatpak and the portals are the sandbox that fits. (Landlock ABI 4, TCP
  port rules, is Linux 6.7; 6.1 has ABI 2.)
- `rseq`: nothing here is a per-CPU hot path.
