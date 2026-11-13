# Build and QA tools

The build entry points (`build.ps1`, `build_linux.sh`) and their options are in
[CONTRIBUTING.md](../CONTRIBUTING.md#building-and-testing). Every script documents its options, environment and needs in its
header (the shell scripts print it with `--help`); this file is the index, plus what has no script of its own.
[`perf/`](perf/README.md), [`release/`](release/README.md), [`font_reference/`](font_reference/README.md) and
[`unicode/`](unicode/README.md) have their own READMEs.

## The LLVM toolchain

Local builds and CI (`.github/actions/setup-toolchain`, used by every workflow that builds, on Windows and Linux) fetch the
[maj113/llvm-pe](https://github.com/maj113/llvm-pe/releases) release pinned in `llvm_toolchain.json` through
`resolve_llvm_toolchain.ps1` / `.sh`: downloaded once, checked against the pinned SHA-256 and kept in
`%LOCALAPPDATA%\MaiCord-toolchain\<tag>` / `~/.cache/maicord-toolchain/<tag>` (`MAICORD_TOOLCHAIN_CACHE`, else
`$XDG_CACHE_HOME/maicord-toolchain`). CI puts its `bin` on `PATH` and in `MAICORD_LLVM_BIN` and caches the extracted toolchain
per system with `actions/cache`, keyed by the hash of `llvm_toolchain.json`, so a new pin fetches anew. Where the Build Tools
are missing, the action installs exactly the components in `.vsconfig`; CMake and Ninja come from the runner's `PATH`.

Each toolchain release of `maj113/llvm-pe` must provide:

- a tag `toolchain-YYYY.MM.DD`;
- the asset `llvm-pe-x86_64-windows.zip`, whose root is the toolchain directory: `bin\clang-cl.exe`, `bin\lld-link.exe` and
  `bin\llvm-lib.exe`, and clang's core, x86 and Windows resource headers in `lib\clang\<major>\include`, which clang-cl finds
  relative to `bin`. The executables link the static MSVC runtime, so no redistributable is needed;
- the asset `llvm-pe-x86_64-linux.tar.xz`, whose root is the toolchain directory as well: `bin/clang`, `bin/clang++`,
  `bin/ld.lld`, `bin/llvm-ar` and `bin/llvm-ranlib` (they need glibc 2.38), and clang's core resource headers in
  `lib/clang/<major>/include` (the ABI checks compile the system's headers with them);
- each asset's `.sha256`, the archive's hex SHA-256.

To move to a new release, set `tag` in `llvm_toolchain.json` to the tag, and `sha256` and `linux.sha256` to the two `.sha256`
values.

The fork's code generation options (the internal call contract, IPRA on register masks, 1-byte function alignment, lld-link's
`/OPT:LLDBRANCHRELAX`) are added one by one, each only when the configure step finds that the compiler and linker take it
(`maicord_llvm_options` in `cmake/build_contract.cmake`, which records what each saves). A stock LLVM therefore builds the
same sources into a bigger client instead of failing; Linux takes plain IPRA from any LLVM. On Windows another toolchain
directory reconfigures the build directory from scratch.

Windows has no developer shell: `enter_build_environment.ps1` (which `build.ps1` dot-sources) finds the `.vsconfig` components
where VsDevCmd would. It puts the toolchain first on `PATH`, then the MSVC toolset's `bin\Hostx64\x64` (MASM, `ml64`) and the
newest Windows SDK's `bin\<version>\x64` (`rc`, `mt`, `fxc`), and sets `LIB` to the SDK's import libraries and `INCLUDE` to its
headers and the toolset's C and C++ library headers, for the units compiled against the SDK (`MAICORD_SDK_INCLUDES`). MASM is
`ml64`, never `llvm-ml`: it encodes about 40 register-to-register instructions the other valid way, so the client would not be
byte-identical (`CMakeLists.txt` refuses any other assembler).

## Images, size and memory

| Tool | Use |
| --- | --- |
| `audit_image.py [--libc musl] [--no-test-hooks]` | The PE and ELF audit every client link runs; `--no-test-hooks` rejects an image with test hooks |
| `compare_image.py OLD NEW` | Compares two builds, PE or ELF, section by section (work on one system must not change the other's client) |
| `report_binary_size.py` | What `-SizeReport` / `--size-report` print: the client's sections and largest code. On Linux that run keeps the symbol table for `llvm-nm -S --size-sort` |
| `retarget_libc.py --libc musl IN OUT` | The musl form of a Linux image (`src/core/linux/README.md`) |
| `linux_memory_report.py PID` | Splits a running Linux client's resident memory by mapping (image, heap, GPU driver) |
| `audit_linux_reachability.sh` | Links the Linux client without LTO and lists the code no path from `_start` reaches, by source file: finds a feature that a stub or an `#if` cut off (`src/app/linux/README.md` lists what is unreachable on purpose) |
| `check_sources.py` | With `-KeepIntermediates`: parses changed files and the units that include a changed header, without building |
| `check_{libc,openssl,pulse,speechd,fido2,video,pipewire}_abi.py` | Compare the `dlopen` tables and the libc pins with the installed `-dev` headers |

## Linux QA scripts

All run on headless sway (or nested sway; WSLg's Weston has no screencopy) with private buses, profiles and servers, and report
instead of touching the user's session. `install_linux_toolchain.sh --integration` installs what they need on Debian and
Ubuntu. The fake Discord runs need user, network and mount namespaces; CTest registers them as opt-in
(`MAICORD_FAKE_DISCORD_INTEGRATION=1` at configure time; status 77 where the namespaces or sway are missing).

| Script | Runs |
| --- | --- |
| `test_linux_integration.sh [CASE...]` | The CI set, each case reported: `wayland`, `gpu-surface`, `client`, `atspi`, `dbus`, `audio`, `speech` (the next seven rows). Failing only with `--blocking` or the repository variable `MAICORD_LINUX_INTEGRATION_BLOCKING=true` |
| `test_wayland_window.sh [--scale S] [--integer]` | The Wayland window tests (`MAICORD_WAYLAND_INTEGRATION=1`) |
| `test_gpu_surface.sh` | The EGL window surface, damage blit and frame callbacks |
| `test_client_wayland.sh` | The client on its signed-out screen: screenshot, idle CPU, wakeups, memory; `--terminate TERM\|INT\|HUP` |
| `test_atspi.sh` | The client behind the real at-spi-bus-launcher and registry |
| `test_dbus.sh` | The D-Bus mocks (`tests/platform/dbus/mock_services.py`) on private buses |
| `test_pulse_audio.sh` | A private PulseAudio server (`MAICORD_PULSE_INTEGRATION=1`) |
| `test_speechd.sh` | A private speech-dispatcher with its dummy module (`MAICORD_SPEECHD_INTEGRATION=1`) |
| `test_capture_pipewire.sh` | The PipeWire capture and stream audio tests against a private PipeWire daemon |
| `test_dbus_real.sh`, `test_dbus_tray.sh` | The real xdg-desktop-portal and gnome-keyring; swaybar's tray. They need a graphical session and stay manual |
| `test_linux_distros.sh [--build DIR] [DISTRO...]` | The portable tests (`linux_portable_tests.txt`) in distribution containers; its header lists each distribution's packages |
| `client_flow.py STEP...` | Drives the client through clicks, typing (`wtype`), scroll, output scale and window commands with screenshots (`wayland_pointer.py` is its virtual pointer) |
| `test_discord_wayland.sh --binary CLIENT [--site SITE] [--root] [--dbus] [--bar] [--audio] STEP...` | The client signed in against the fake Discord, in a private user, network and mount namespace. `--site` serves a catalog site on the catalog host too (`--root` makes the namespace with sudo so the plugin loader's ownership check sees real root); `--dbus` starts the desktop mocks and a swaybar hosting the tray and prints the notifications and portal calls (`desktop-calls.txt`) |
| `test_signin_wayland.sh` | The sign-in regression run, without and with `--bar` (a swaybar that reserves the top 23 pixels) |
| `test_voice_wayland.sh` | Joins the fake guild's stage with a private PulseAudio server (a voice channel needs DAVE, which the fake's voice server does not serve) |
| `test_go_live_wayland.sh`, `test_go_live_loopback.py` | Go Live from the client through the portal mocks; `maicord_go_live_loopback` streams from MaiCord's sender to its receive path through the fake's stream server for each codec and checks the frames |
| `test_catalog_wayland.sh --binary CLIENT --site SITE --expect index-4.json ...` | The client against a catalog site built with the catalog scripts and a throwaway key (`MAICORD_PLUGIN_CATALOG_PUBLIC_KEY=<hex> tools/build_linux.sh` in a fresh build directory makes the matching client) |
| `test_server_settings_wayland.sh --page "<sidebar label>" [--expect REGEX]... STEP...` | The Server Settings probe: a test build opens Server Settings on the fake's QA guild at that page (the `MAICORD_TEST_GUILD_SETTINGS` hook), runs the steps and checks the fake's request log, which also holds the bodies of the writes |
| `test_server_tag_wayland.sh`, `test_server_apps_wayland.sh`, `test_server_access_wayland.sh`, `test_server_profile_wayland.sh` | Its runs for Server Tag; Integrations and AutoMod; Server Template, Access and Enable Community; Server Profile |
| `test_user_settings_wayland.sh --page <catalog key> [--expect REGEX]... STEP...` | The User Settings probe: a test build opens Settings at that page (the `MAICORD_TEST_USER_SETTINGS` hook) once the account is ready; the fake's account routes log each write's body |
| `test_account_settings_wayland.sh`, `test_app_settings_wayland.sh`, `test_profile_editor_wayland.sh` | Its runs for My Account, Data & Privacy and Connected Apps; the app-behaviour pages (each switch checked by the field its decoded settings-proto PATCH carries); Profiles |
| `test_friends_wayland.sh`, `test_game_mentions_wayland.sh` | Home's Friends and Message Requests pages; game mention chips, tooltip, the game profile and the composer's @game search |

- Never judge a screenshot from a bare `grim`: a signed-in window that headless sway scans out directly stays unrepainted in
  grim until some input repaints the output, so `client_flow.py`'s `shot:` first re-applies the output scale.
- `client_flow.py`'s `keys:A,B,...` sends several keys from one virtual keyboard: each new keyboard moves keyboard focus, which
  closes an open menu.
- A client run that opens Settings and closes (`python3 tools/client_flow.py wait:4 click:1139,16 wait:1.5 close`) must exit 0.

## The fake Discord server

`fake_discord_server.py` serves REST and the user gateway (ETF, one zstd frame per message) over TLS with no account involved:
sign in with any e-mail and password (the account routes take the password `hunter2`, the authenticator code `123456` and the
verification code `424242`). It serves one account, a guild with text channels, a voice channel and a stage, seeded messages, a
voice server and a Go Live stream server (`us-qa-stream.discord.media`) that runs DAVE as Discord's does and relays the media
between the sender and its viewers; its docstring is the reference. It answers only what the client reads (an unknown request
is 404 and logged), and its READY must stay faithful to what the client's decoders require (the user's settings proto and the
guild's `user_guild_settings` entry decide whether a mention raises a notification).

| Variable | Effect |
| --- | --- |
| `MAICORD_FAKE_WORLD=DIR` | Replaces the QA guild with a fuller made-up world (`fake_discord_world.py` describes `world.json` and `cdn/`); `MAICORD_FAKE_WORLD_NOW=MS` pins its clock |
| `MAICORD_FAKE_MENTION_AFTER=N` | Adds a mention of the account (`test_discord_wayland.sh` passes it as `--mention-after`) |
| `MAICORD_FAKE_EXTRA_MESSAGES=FILE` | More seeded messages (format in `test_discord_wayland.sh`'s header) |
| `MAICORD_FAKE_GUILD_FEATURES=A,B` | The guild's features (`GUILD_TAGS` by default) |
| `MAICORD_FAKE_AUTOMATION_EXTERNAL=1` | Changes the webhook, integration, command permission and AutoMod lists as another moderator would |
| `MAICORD_FAKE_PROFILE_TRAITS=JSON` | The guild profile's traits |
| `MAICORD_FAKE_GAME_FRIEND=1`, `MAICORD_FAKE_MESSAGE_REQUEST=1` | The other user is a friend playing a game; a stranger's DM waits as a message request |
| `MAICORD_FAKE_NITRO=1`, `MAICORD_FAKE_ROLE=1` | The account has Nitro; the guild has the "QA Role" |
| `MAICORD_FAKE_STREAM_CODEC`, `MAICORD_FAKE_STREAM_VIEWER_AFTER` | The Go Live codec; when the other user starts watching |

On Linux `test_discord_wayland.sh` points the client at it with a private hosts file and `SSL_CERT_FILE`. On Windows a client
built with `build.ps1 -TestHooks` takes two switches instead (`include/maicord/platform/net/test_server.hpp`):
`MAICORD_TEST_SERVER=127.0.0.1:PORT` answers every name lookup (API, gateway, CDN, catalog) with that address and port, and
`MAICORD_TEST_CA_FILE=cert.pem` replaces Schannel's validation with a chain check against only the certificates in that file (a
private chain engine over an in-memory store: no change to the system or user store). The certificate must cover the names
(`discord.com`, `gateway.discord.gg`, `cdn.discordapp.com`, `media.discordapp.net`, ...). A hooks client given only one of the
two switches, a value that does not parse or a CA file it cannot use exits at startup (status 3, reason in the log, component
`test_hooks`) instead of reaching the real network.

`run_fake_server_client.ps1 [-Executable PATH] [-World DIR]` does it all: it refuses a client without hooks, copies it with its
assets, makes the certificate, starts the server, launches the client with a private `LOCALAPPDATA` and returns once the server
has logged the client's first request; `-Stop` ends both. Run it with its output redirected to a file, not a pipe: the processes
it starts keep the pipe open.
