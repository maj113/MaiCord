# Tools

Every script documents its options in its header or `--help`. [CONTRIBUTING.md](../CONTRIBUTING.md#building-and-testing)
describes the build and test runs. [`perf/`](perf/README.md) and [`release/`](release/README.md) have their own READMEs.

| Tools | Use |
| --- | --- |
| `build.ps1`, `build_linux.sh` | The build entry points (options in [AGENTS.md](../AGENTS.md#build-and-test)) |
| `enter_build_environment.ps1`, `build_lock_name.ps1`, `maintain_build_outputs.ps1`, `list_checkout_files.ps1` | Helpers of the build entry points |
| `llvm_toolchain.json`, `resolve_llvm_toolchain.ps1`, `.sh`, `install_linux_toolchain.sh` | The pinned toolchain ([below](#the-llvm-toolchain)); the Linux packages on Debian and Ubuntu |
| The scripts that `cmake/*.cmake` runs | Generated inputs: packed data (`pack_*.py`, `optimal_deflate.py`, `deflate_cache.txt`, `embed_fixture.py`), the shader packs, the icon pack, regional flags, the emoji catalog, Wayland bindings and the SDK conformance unit |
| `audit_image.py [--libc musl] [--no-test-hooks] [--processor-entry-map MAP]` | The PE and ELF audit that every client link runs |
| `check_processor_entry.py IMAGE MAP` | The audit's check that the client runs only first x86-64 instructions until its processor check decides |
| `check_source_rules.py` | The source rules that every client link runs: shader twins, constant buffers, the example theme |
| `check_{libc,openssl,pulse,speechd,fido2,video,pipewire}_abi.py` | Compare the `dlopen` tables and the libc pins with the installed `-dev` headers (CI) |
| `check_linux_ui_text.py` | Fails when the Linux client's text names Windows (a registered test) |
| `retarget_libc.py --libc musl IN OUT` | The musl form of a Linux image (`src/core/linux/README.md`) |
| `compare_image.py OLD NEW` | Compares two builds, PE or ELF, section by section: work on one system must not change the other's client |
| `report_binary_size.py` | The client's sections and largest code (`-SizeReport`, `--size-report`) |
| `check_sources.py` | After a `-KeepIntermediates` build: parses changed files and the units that include a changed header |
| `update_gpu_goldens.py` | Renews `tests/gpu/golden` from Direct3D 11 after a shader or scene change |
| `linux_memory_report.py PID` | Splits a running Linux client's resident memory by mapping |
| `linux_portable_tests.py`, `linux_portable_tests.txt` | The portable test set that CI runs in distribution containers |
| `maicord-plugin/` | The plugin and effect tool for plugin authors ([SDK](../include/maicord/sdk/README.md)) |
| `glsl_translate.py`, `fetch_linux_shader_toolchain.py`, `linux_shader_toolchain.json`, `shadertool/` | HLSL to GLSL for the Linux section of a plugin effect (`maicord-plugin effect build --glsl`) |
| `fake_discord_server.py`, `fake_discord_world.py`, `run_fake_server_client.ps1`, `test_go_live_loopback.py` | A fake Discord ([below](#the-fake-discord-server)), its Windows runner and the Go Live loopback test's driver |
| `test_*.sh`, `client_flow.py`, `sway_test.sh`, `dbus_test_bus.sh`, `wayland_pointer.py`, `qa_voice_signal.py` | The Linux integration runs ([below](#linux-integration-runs)) |
| `analysis/` | The CodeQL gate of `.github/workflows/analysis.yml` |

## The LLVM toolchain

Local builds and CI (`.github/actions/setup-toolchain`) fetch the [maj113/llvm-pe](https://github.com/maj113/llvm-pe/releases)
release pinned in `llvm_toolchain.json` through `resolve_llvm_toolchain.ps1` / `.sh`. The scripts check the pinned SHA-256
and keep the toolchain in `%LOCALAPPDATA%\MaiCord-toolchain\<tag>` / `~/.cache/maicord-toolchain/<tag>`
(`MAICORD_TOOLCHAIN_CACHE`, else `$XDG_CACHE_HOME/maicord-toolchain`). CI caches the extracted toolchain per system, keyed by
the hash of `llvm_toolchain.json`. Where the Build Tools are missing, the action installs the components in `.vsconfig`.

Each toolchain release of `maj113/llvm-pe` must provide:

- a tag `toolchain-YYYY.MM.DD`;
- the asset `llvm-pe-x86_64-windows.zip`, whose root is the toolchain directory: `bin\clang-cl.exe`, `bin\lld-link.exe` and
  `bin\llvm-lib.exe`, and clang's core, x86 and Windows resource headers in `lib\clang\<major>\include`. The executables link
  the static MSVC runtime, so no redistributable is needed;
- the asset `llvm-pe-x86_64-linux.tar.xz`, whose root is the toolchain directory too: `bin/clang`, `bin/clang++`,
  `bin/ld.lld`, `bin/llvm-ar` and `bin/llvm-ranlib` (they need glibc 2.38), and clang's core resource headers in
  `lib/clang/<major>/include` (the ABI checks compile the system's headers with them);
- the `.sha256` of each asset, the archive's hex SHA-256.

To move to a new release, set `tag`, `sha256` and `linux.sha256` in `llvm_toolchain.json`.

The configure step adds the fork's code generation options one by one, each only when the compiler and linker take it
(`maicord_llvm_options` in `cmake/build_contract.cmake`, which records what each saves). A stock LLVM therefore builds a
bigger client instead of failing. On Windows another toolchain directory reconfigures the build directory from scratch.

Windows has no developer shell: `enter_build_environment.ps1` finds the `.vsconfig` components where VsDevCmd would. It puts
the toolchain first on `PATH`, then the MSVC toolset's `bin\Hostx64\x64` (`ml64`) and the newest Windows SDK's
`bin\<version>\x64` (`rc`, `mt`, `fxc`). It sets `LIB` to the SDK's import libraries and `INCLUDE` to its headers and the
toolset's C and C++ headers, for the units compiled against the SDK (`MAICORD_SDK_INCLUDES`). MASM is `ml64`, never
`llvm-ml`: `llvm-ml` encodes about 40 register-to-register instructions the other valid way, so the client would not be
byte-identical (`CMakeLists.txt` refuses any other assembler).

## Linux integration runs

The scripts run on headless sway with private buses, profiles and servers, and never touch the user's session.
`install_linux_toolchain.sh --integration` installs what they need on Debian and Ubuntu. `test_linux_integration.sh` is the
CI set: `test_wayland_window.sh`, `test_gpu_surface.sh`, `test_client_wayland.sh`, `test_atspi.sh`, `test_dbus.sh`,
`test_pulse_audio.sh` and `test_speechd.sh`. `test_linux_distros.sh` runs the portable tests in distribution containers (CI).

The other scripts drive opt-in tests or run by hand:

- `test_capture_pipewire.sh`, `test_pulse_stall.sh` and `test_dbus_real.sh` run against a private PipeWire, a PulseAudio
  that stalls, and the real portal and keyring.
- `test_discord_wayland.sh` runs the client signed in against the fake Discord, in a private user, network and mount
  namespace. The scripts built on it are opt-in CTest cases (`MAICORD_FAKE_DISCORD_INTEGRATION=1` at configure time).
- `client_flow.py` drives the client with clicks, typing (`wtype`), scroll and screenshots. Its `shot:` step re-applies the
  output scale first: headless sway scans a full-screen window out directly, and a bare `grim` then shows a stale frame.
  `keys:A,B,...` sends several keys from one virtual keyboard, because each new keyboard moves the focus and closes a menu.

## The fake Discord server

`fake_discord_server.py` serves REST and the user gateway over TLS with no account; its docstring is the reference. Sign in
with any e-mail and password. The account routes take the password `hunter2`, the authenticator code `123456` and the
verification code `424242`. The server answers only what the client reads: an unknown request is 404 and logged.

| Variable | Effect |
| --- | --- |
| `MAICORD_FAKE_WORLD=DIR` | A fuller made-up world in place of the QA guild (`fake_discord_world.py`); `MAICORD_FAKE_WORLD_NOW=MS` pins its clock |
| `MAICORD_FAKE_MENTION_AFTER=N` | A mention of the account (`test_discord_wayland.sh --mention-after`) |
| `MAICORD_FAKE_EXTRA_MESSAGES=FILE` | More seeded messages (format in `test_discord_wayland.sh`'s header) |
| `MAICORD_FAKE_GUILD_FEATURES=A,B` | The guild's features (`GUILD_TAGS` by default) |
| `MAICORD_FAKE_AUTOMATION_EXTERNAL=1` | Another moderator changes the webhook, integration, command permission and AutoMod lists |
| `MAICORD_FAKE_PROFILE_TRAITS=JSON` | The guild profile's traits |
| `MAICORD_FAKE_GAME_FRIEND=1`, `MAICORD_FAKE_MESSAGE_REQUEST=1` | The other user is a friend playing a game; a stranger's DM waits as a message request |
| `MAICORD_FAKE_SERVER_HUB=1` or `empty` | A published Server Hub and a Boost Goal (header of `fake_discord_server.py`); `empty` answers the hub with 404 |
| `MAICORD_FAKE_NITRO=1`, `MAICORD_FAKE_ROLE=1` | The account has Nitro; the guild has the "QA Role" |
| `MAICORD_FAKE_STREAM_CODEC`, `MAICORD_FAKE_STREAM_VIEWER_AFTER` | The Go Live codec; when the other user starts watching |

On Linux `test_discord_wayland.sh` points the client at the server with a private hosts file and `SSL_CERT_FILE`. On Windows
a client built with `build.ps1 -TestHooks` takes two switches instead (`include/maicord/platform/net/test_server.hpp`):

- `MAICORD_TEST_SERVER=127.0.0.1:PORT` answers every name lookup with that address and port.
- `MAICORD_TEST_CA_FILE=cert.pem` trusts only the certificates in that file, through a private chain engine. The system and
  user stores do not change. The certificate must cover `discord.com`, `gateway.discord.gg`, `cdn.discordapp.com` and
  `media.discordapp.net`.

A hooks client with only one of the switches, a value that does not parse or a CA file it cannot use exits at startup (status
3, reason in the log, component `test_hooks`), before any network access. `run_fake_server_client.ps1` does all of this on
Windows (CONTRIBUTING.md).
