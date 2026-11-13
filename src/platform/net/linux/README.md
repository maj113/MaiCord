# Linux network layer

`src/platform/net/linux` implements `include/maicord/platform/net/network.hpp` and `socket.hpp` on Linux: the Windows
completion-port model (an operation is started on a caller-owned object, finishes as exactly one completion from
`network_loop_poll`) on io_uring.

## Backend

- One io_uring per `NetworkLoop`: 128 entries, `SINGLE_ISSUER | COOP_TASKRUN | SUBMIT_ALL`, raw syscalls and mmap
  (`include/maicord/sys/linux/io_uring.hpp`), no liburing. Linux 6.1 and later; there is no other backend and no
  kernel-version probe.
- The ring belongs to the loop's thread: `network_loop_initialize`, every operation, `network_loop_poll` and
  `network_loop_release` run on it. Other threads (the name-lookup pool, `network_signal_post`) call `post()`, which
  pushes on a lock-free stack and writes the loop's eventfd; a ring READ on the eventfd wakes the loop.
- Starting an operation only queues a submission entry (`start`), except a connect, which is submitted at once as a
  Windows one starts (a peer may accept it before the loop is polled). `network_loop_poll` submits what is queued and
  waits in one `io_uring_enter` (`EXT_ARG` timeout, so even a zero-timeout poll runs the ring's pending work). A request
  that can finish at once finishes inside that call.
- Files are ring READ/WRITE requests: the kernel serves them inline from the page cache and from its own workers
  otherwise. The old pread/pwrite thread pool is gone.
- Name lookups still run `getaddrinfo` on a two-thread pool (`resolver.cpp`) and post to the loop. `socket.cpp`'s
  `SocketWatch` (the waitable of the blocking loopback helpers) is an epoll descriptor; it is not the loop.

## Rules

- One completion per operation, however it ends. Closing a connection or datagram bumps its generation (its completions
  read as stale), submits the queue, cancels every request on the descriptor (`ASYNC_CANCEL` by fd) and then closes it;
  each cancelled request still completes once. A closed file keeps its descriptor until its completion is polled
  (`close_when_done`). Buffers and the owner stay alive until the `*_pending` flag clears, as on Windows.
- A send or file operation that moves only part of its buffer asks for the rest in `finish` (network.cpp); it completes
  with everything or an error.
- Datagram reads: only the oldest posted slot's read is in the kernel (`receive_queue`); several reads waiting on one
  socket are woken in no promised order, and the contract is oldest-first.
- A connect reads its address from `NetworkConnection::connect_address` when the loop submits it.
- No fallback. If io_uring cannot be made (`kernel.io_uring_disabled`, a seccomp profile, a kernel older than 6.1)
  `network_loop_initialize` fails with `completion_port_create` and `network_unavailable_reason()` names the cause;
  the client entry (`src/app/linux/entry.cpp`) prints it and exits. Docker's default profile answers EPERM and
  Podman's ENOSYS, so the distro test containers run with `--security-opt seccomp=unconfined`
  (`tools/test_linux_distros.sh`); Flatpak's seccomp blocklist does not list io_uring.
- Tests: `maicord_network_loop` (the model, large sends, more requests than ring entries),
  `maicord_network_loop_ring_refused` / `_ring_hidden` (a seccomp filter returns EPERM / ENOSYS for
  `io_uring_setup`), `maicord_tls_stream_smoke` (`tls_stream` over the loop against a local `openssl s_server`).

## Measured (WSL2 Ubuntu 24.04, kernel 6.18, Ryzen 7 5800X, median of 5 interleaved runs, loopback; epoll emulation -> ring)

| load | syscalls of the loop's thread | time |
| --- | --- | --- |
| gateway socket, 100 messages + 20 heartbeats | 332 -> 141 | wake latency p50 70 -> 74 us |
| REST, 1600 request/response on 8 connections | 5003 -> 430-650 | 43.2 -> 40.7 ms |
| download, 64 MB (256 MB timed) | 1150 -> 1056 | 69.8 -> 63.0 ms, CPU 41 -> 38 ms |
| voice, 250 packets echoed at 50/s | 1252 -> 503 | p50 55 -> 55 us |
| video burst, 20000 packets, 8 slots | 44670 -> 20004 | 472 -> 461 ms, wakeups 5791 -> 6260 |
| file reads, 1024 x 64 KB | 3076 + 6144 in the pool -> 1028 | 62.2 -> 4.4 ms, 3058 -> 0 context switches |
| file writes, 512 x 128 KB | 1558 + 3074 in the pool -> 516 | 44.9 -> 40.8 ms, CPU 52 -> 36 ms |

Sockets are a tie in time and CPU with about half the syscalls or fewer; file operations are the large win (no helper
threads, no wakeups). `getrusage` CPU has tick granularity, so small CPU differences are noise.

## Tried and not used

- `DEFER_TASKRUN`: slower than `COOP_TASKRUN` here (download 72.8 vs 62.3 ms, voice p50 64 vs 52 us, 6624 vs 5843 wakeups
  in the burst).
- `SEND_ZC`: TLS records are at most about 16 KB; on loopback 16 KB sends cost 3.7x the CPU of `SEND` and 64 KB 2x
  (it also holds the buffer until a second completion).
- Fixed files: no measurable difference in 200000 small sends. Not tried: registered buffers (buffered file reads and
  socket receives copy anyway), multishot receive with provided buffers (the API posts one buffer per receive) and
  `SQPOLL` (a spinning kernel thread for a chat client).
