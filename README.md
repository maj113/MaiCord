<div align="center">

<img src=".github/assets/banner.svg" alt="MaiCord — a native Discord client for Windows and Linux" width="100%">

**A lightweight, native Discord client for Windows and Linux, written in C++ with Direct3D 11 and OpenGL ES.**<br>
No Electron, no browser engine. One ~6.5 MB executable on each system.

[![CI](https://github.com/maj113/MaiCord/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/maj113/MaiCord/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/maj113/MaiCord?sort=semver&display_name=tag)](https://github.com/maj113/MaiCord/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/maj113/MaiCord/total)](https://github.com/maj113/MaiCord/releases)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20x86--64-0078D6)](#requirements)
[![License](https://img.shields.io/badge/license-MPL--2.0-brightgreen)](LICENSE)

[Website](https://maicord.com/) ·
[Download](https://github.com/maj113/MaiCord/releases/latest) ·
[Install](#install) ·
[Features](#features) ·
[Build from source](#building-from-source) ·
[Plugins and themes](#plugins-and-themes) ·
[Contributing](CONTRIBUTING.md)

</div>

> [!WARNING]
> MaiCord is an **unofficial** third-party client. It is not affiliated with,
> endorsed by or supported by Discord Inc. Using third-party clients may
> violate [Discord's Terms of Service](https://discord.com/terms) and could put
> your account at risk. Use it at your own discretion.

## Why MaiCord

The official desktop app is a web page in Electron. MaiCord is written from
scratch in freestanding C++23 (no STL, CRT, exceptions or RTTI), shapes and
rasterizes its own text, and draws its own interface on the GPU.

| | Windows | Linux |
| --- | --- | --- |
| Download | 5.3 MB zip, portable | 5.3 MB tarball or 5.9 MB AppImage |
| Executable | one 6.49 MB EXE | one 6.5 MB program |
| On disk | the EXE plus its profile fonts and license notices, ~9 MB together | the same, ~9 MB together |
| Dependencies | only DLLs that ship with Windows | the C library, and what a desktop has: OpenSSL, Wayland, EGL and GLES |
| First frame | ~0.1 s | ~0.7 s (Mesa d3d12) to ~1.0 s (llvmpipe, software) |
| Idle | no CPU time measured over 20 s: nothing redraws while nothing changes | the same: 0.00% CPU, under one wakeup a second |
| Memory at the login screen | 54 MB working set | ~165 MB resident, mostly the GPU driver's libraries; the program's own image and heap are ~13 MB |

Measured on one machine (Ryzen 7 5800X, Radeon RX 9070 XT): a Release build of the current tree on Windows, and
the Linux build in WSL2 under headless sway, each on a fresh profile at the signed-out screen, with the idle figures over
20 seconds. Sizes are the 0.6.6 Windows zip and the Linux 0.6.7 files. Other machines and drivers differ, Linux
memory most of all.

## Features

**Chat**
- Discord markdown, including syntax-highlighted code blocks and
  [ANSI colored text](https://gist.github.com/kkrypt0nn/a02506f3712ff2d1c8ca7c9e0aed7c06)
- Rich embeds with image galleries, link previews, stickers and custom emoji
- Replies (with timestamps on the preview), forwards, pins, reactions, threads, forum posts and polls (create and vote)
- Role colors on mentions, in voice participant lists and on typing names
- Slash commands, including Discord's built-in ones, and app buttons, select
  menus, modals and Components V2 layouts
- Record and send voice messages
- Emoji, GIF and sticker picker that syncs your Discord favorites
- Paste screenshots straight from the clipboard; drag and drop files
- Image zoom: hold <kbd>Ctrl</kbd> over an image in chat, or hold the mouse button
  on an image in the viewer, to magnify it; scroll for the zoom, <kbd>Shift</kbd>+scroll for the lens size
- Search with `from:`, `in:`, `has:`, `mentions:` and `pinned:` filters,
  an inbox for mentions, and a Quick Switcher on <kbd>Ctrl</kbd>+<kbd>K</kbd>

**Voice and video**
- Voice channels with Discord's end-to-end encryption (DAVE)
- Push-to-talk, per-user volume, soundboard and stage channels
- Watch Go Live streams and cameras, decoded on the GPU

**Servers**
- Server settings: roles and permissions, members, bans, audit log, emoji,
  stickers, soundboard and invites
- Scheduled events, community onboarding, per-server profiles and privacy
- Server folders: opening one closes the others
- Hidden channels are listed with a lock and a summary of who can see them;
  nothing is fetched from them

**Accounts**
- Password sign-in with two-factor authentication (security keys, Windows Hello
  on Windows, authenticator apps, backup codes, SMS), QR login and CAPTCHA support
- Up to five saved accounts with quick switching
- Full profile editor: avatar cropping, banner, decorations, effects and
  profile colors

**System integration**
- Native notifications, a tray icon and an unread badge: Windows toasts, the taskbar and the Win32 tray; on Linux the
  desktop portal or D-Bus, a StatusNotifier tray icon and the launcher badge
- Spellcheck and IME input through the Windows text services; Hunspell or Enchant and the Wayland text-input protocol on Linux
- Screen reader support through UI Automation on Windows and AT-SPI (Orca) on Linux
- Signed automatic updates from GitHub releases (see [Install](#install) for what updates itself on Linux)

**Make it yours**
- Themes control colors, typography, spacing, components and animated
  backgrounds; browse and install them, and animated background effects, from
  Settings > Themes and Settings > Effects, from a `maicord://` link or from a
  file. Settings > Themes lists every installed theme, and Appearance shows
  them next to the built-in ones ("Your themes")
- Native plugins with a stable C ABI and a header-only C++ SDK: hooks and
  events, message decorations, buttons and menus, slash commands, settings and
  shortcuts. Settings > Plugins installs them from a signed catalog of reviewed
  plugins (27 at v0.6.0), and they install, update, roll back and toggle
  without a restart
- Developer tooling: [`tools/maicord-plugin`](tools/maicord-plugin) creates,
  builds, checks and packs plugins and effects, and Developer Mode hot-reloads
  them as you rebuild

## Install

### Windows

1. Download `MaiCord-<version>-windows-x64.zip` from the
   [latest release](https://github.com/maj113/MaiCord/releases/latest).
2. Extract it anywhere you can write to, for example `%LOCALAPPDATA%\Programs\MaiCord`.
3. Run `MaiCord.exe`. No installer or admin rights are needed.

MaiCord keeps its settings and saved sign-ins in `%LOCALAPPDATA%\MaiCord` and
updates itself. Every update is checked against a signature built into the
client before it is installed, and it replaces `MaiCord.exe` in place when you
choose *Restart to update*, so the folder must stay writable.

### Linux

| You | Do |
| --- | --- |
| Any distribution, one command | `curl -fsSL https://github.com/maj113/MaiCord/releases/latest/download/install.sh \| sh` installs under `~/.local` without root, checks a signature and a SHA-256 before it unpacks anything, and updates itself |
| Any distribution, one file | Download `MaiCord-<version>-x86_64.AppImage` from the [latest release](https://github.com/maj113/MaiCord/releases/latest), `chmod +x` it and run it. `./MaiCord-<version>-x86_64.AppImage --integrate` adds the menu entry and the `discord://` handler |
| Debian, Ubuntu, Mint | `sudo curl -fsSLo /etc/apt/sources.list.d/maicord.sources https://maj113.github.io/MaiCord/repo/maicord.sources && sudo apt update && sudo apt install maicord` |
| Fedora, openSUSE, RHEL-likes | `sudo curl -fsSLo /etc/yum.repos.d/maicord.repo https://maj113.github.io/MaiCord/repo/maicord.repo && sudo dnf install maicord` (openSUSE: `zypper addrepo` the same file) |
| Arch, Manjaro, EndeavourOS | the signed pacman repository below, then `sudo pacman -Syu maicord` |
| NixOS, Nix | `nix run 'github:maj113/MaiCord?dir=packaging/nix'` |
| Alpine, Void (musl) | the `-musl` tarball or AppImage of the release |

```sh
curl -fsSL https://maj113.github.io/MaiCord/repo/maicord.asc | sudo pacman-key --add -
sudo pacman-key --lsign-key 27EFAD9C252F3E39887CA28EACB877AA84964FC8
printf '\n[maicord]\nSigLevel = Required\nServer = https://maj113.github.io/MaiCord/repo/arch/$arch\n' | sudo tee -a /etc/pacman.conf >/dev/null
```

The release also has the `.deb`, `.rpm`, Arch package and tarballs
(`maicord-<version>-linux-x86_64.tar.gz`, and `-musl`) with their checksums.
The one-command installer and the tarball install update themselves from
*Settings > Updates*, from the same signed manifests as Windows, and so does the
glibc AppImage; a package from a repository is updated by your package manager, and
the musl AppImage by AppImageUpdate. MaiCord keeps its data in
`$XDG_DATA_HOME/maicord` (`~/.local/share/maicord`), the same place for every
install. [`packaging/README.md`](packaging/README.md) has every format and its details.

### Requirements

**Windows**
- Windows 10 or 11, 64-bit
- A CPU with AVX and AES-NI
- A GPU with Direct3D 11 (feature level 11_0) and Tiled Resources Tier 1 (Direct3D 11.2); MaiCord refuses to start
  on a GPU or driver without it and says so

**Linux**
- x86-64 with AVX and AES-NI, and Linux 6.1 or newer: the network loop runs on io_uring, so a system
  that disables it (`kernel.io_uring_disabled`, Docker's and Podman's default seccomp profile) is refused at
  startup with the cause named. Stock Ubuntu 22.04 and RHEL 9 kernels are older than 6.1
- glibc 2.34 or newer, or musl (Alpine, Void, Chimera). The release is checked in containers of Ubuntu 22.04,
  Debian 12, AlmaLinux 9, Fedora, Arch, NixOS, Alpine, Void and Chimera
- A Wayland compositor: there is no X11 backend. MaiCord draws its own title bar, so it needs no server-side
  decorations. It has been run on sway and Weston
- A GPU driver with OpenGL ES 3.1 over EGL, `GL_EXT_blend_func_extended` and image copies (Mesa, or NVIDIA)
- OpenSSL 3 or 4, libwayland, libxkbcommon and EGL/GLES; used when present: PulseAudio or PipeWire's Pulse
  server (voice), fontconfig, Hunspell or Enchant (spellcheck), libfido2 (security keys), VA-API (video),
  speech-dispatcher, and the desktop portals and a Secret Service (notifications, file dialogs, saved sign-ins)

## Plugins and themes

**Themes** are JSON files (up to 64 KiB). A theme starts from the Dark, Light or AMOLED base and can override any
design token; an optional `background` member can name an installed effect and its knob values. See
[`assets/themes/example.maicord-theme.json`](assets/themes/example.maicord-theme.json). A theme can change with the
time of day: `variants` holds up to four phases (`night`, `dawn`, `day`, `dusk`), each with a `base`, core `colors`
and an optional `months` list of twelve colour sets (each turning toward the next month's from the 5th to the 30th,
as the Silk Waves sky does). MaiCord holds each phase around its hours (night 23–01, dawn 05–07, day 11–13, dusk
17–19, the Silk Waves schedule), mixes the colours across the hours between, shows the nearer phase whole where the
base changes (gliding there, at once with reduced motion), and lets the theme alone decide an
effect's day and night. A phase starts from its base's built-in colours, so it is light or dark on its own (the
root's `semantic` overrides don't apply to it; its colours may name the root's `palette`); the root's typography,
metrics, scale, motion, components and background apply to every phase. MaiCord before 0.6.4 skips `variants` as an
unknown member and shows the root look. Such a theme can follow knobs of its effect (`background.follow`: a month
knob and a phase knob with the phase each option holds), so a month or a day or night picked on the effect's knob
page holds the theme there too while that effect is the background; see
[`samples/effects/README.md`](samples/effects/README.md#time-of-day-and-appearance).

**Background effects** are `.mcfx` packages: a manifest, `ps_5_0` bytecode (or, for a pass graph, up to six passes of
pixel and vertex shaders over host-made geometry and small float targets), up to four textures, the source and a
preview. Linux runs the GLSL ES that a package of interface 4 carries next to the bytecode, so one file serves both
systems. They are data, not code: MaiCord checks the container and asks Direct3D (Windows) or the OpenGL ES driver
(Linux) to build the shaders, and caps the GPU time an effect may use. Themes and effects come from the catalog,
from a link (`https://…`), from a file or from a drop onto the install dialog. Anything that is not from the catalog
is shown as *not reviewed* with a live preview before you install it, and never updates on its own.

`maicord://install/<theme|effect|plugin>/<slug>` links open the catalog entry in MaiCord. Registering MaiCord for
these links is optional: it is asked once, when you first open the catalog, and only writes
`HKEY_CURRENT_USER\Software\Classes\maicord` on Windows, and a desktop entry and the default for the scheme in
`mimeapps.list` on Linux.

**Plugins** are native code from the catalog, built by its CI from reviewed source and listed in a signed index: a DLL
on Windows and a freestanding shared object (`.so`) on Linux, built from the same source. MaiCord checks each
download's size and SHA-256 against the index and its declared permissions and network hosts against its listing,
asks you to approve them, and loads it without running any of its code until you agree. Updates that need nothing
new apply on their own; the previous version is kept for one-click rollback, and a plugin the catalog revokes is
turned off at once. Plugins are native code, so only install ones you trust. Loading your own DLL or `.so` needs
Developer Mode.

**Catalog.** At v0.6.7 it lists 27 plugins, 9 themes and 8 effects, browsable in Settings and on
[maicord.com/plugins](https://maicord.com/plugins/), [/themes](https://maicord.com/themes/) and
[/effects](https://maicord.com/effects/). Entries come from the
[MaiCord-plugins](https://github.com/maj113/MaiCord-plugins) repository. A plugin version published before
Linux support has no shared object and is listed for Windows only until its plugin is next released.

**Writing your own.** The SDK is [`include/maicord/sdk/plugin.hpp`](include/maicord/sdk/plugin.hpp) over the C ABI in
[`maicord_plugin.h`](include/maicord/sdk/maicord_plugin.h); see
[`include/maicord/sdk/README.md`](include/maicord/sdk/README.md). Samples:
[`samples/plugins`](samples/plugins) and [`samples/effects`](samples/effects). [`tools/maicord-plugin`](tools/maicord-plugin)
creates, builds, checks and packs plugins (Python 3.9+; on Windows it needs no Visual Studio, on Linux clang and
ld.lld) and builds effects (on Windows; see the effects guide for Linux), and with Developer Mode
on, MaiCord reloads them each time you rebuild. Guides:
[maicord.com/plugins/develop](https://maicord.com/plugins/develop/) and
[maicord.com/effects/develop](https://maicord.com/effects/develop/).

## Building from source

### Windows

You need:

- **The MaiCord LLVM toolchain**: clang-cl, lld-link and llvm-lib from a
  [maj113/llvm-pe](https://github.com/maj113/llvm-pe/releases) release. The
  build fetches the release pinned in `tools/llvm_toolchain.json` on first use
  (into `%LOCALAPPDATA%\MaiCord-toolchain`); pass another `bin` directory as
  `-LlvmBin` (or set `MAICORD_LLVM_BIN`) to override it
- **Visual Studio 2026** (version 18) or its Build Tools, with the
  *Desktop development with C++* workload (Windows SDK, MASM, CMake and Ninja)
- **Python 3.10+** on `PATH`
- **Windows PowerShell 5.1** (included with Windows)

```powershell
git clone https://github.com/maj113/MaiCord.git
cd MaiCord
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1
```

The script finds Visual Studio and the LLVM toolchain, builds `MaiCord.exe`
into the sibling directory `..\MaiCord-build\Release`, and audits it. Close
MaiCord before rebuilding.

Run the test suite with `tools\build.ps1 -Test` (add `-TestNames <name>` for a
single test). It finishes in under 20 seconds and removes its output
afterwards.

### Linux

You need Ubuntu 22.04+, Debian 12+ or a similar system (WSL2 works), with clang, lld and llvm 23, CMake 3.28 or newer,
Ninja, Python 3, git and rsync. `tools/install_linux_toolchain.sh` installs them from apt.llvm.org on Debian and
Ubuntu; `tools/build_linux.sh` refuses any other clang major. The shaders are hand-written GLSL that the build packs with
Python, so the build fetches no shader compiler (only a plugin effect's GLSL section needs DXC and SPIRV-Cross, which
`maicord-plugin effect build --glsl` fetches and builds into `~/.cache/maicord-toolchain` on first use).

```sh
git clone https://github.com/maj113/MaiCord.git
cd MaiCord
tools/build_linux.sh
```

The script builds in `~/mc-build` on a Linux file system (a checkout on a Windows or network mount is copied there
first), audits the images with `tools/audit_elf.py` and leaves `~/mc-build/MaiCord` with its `fonts/` and `licenses/`.
From Windows, `tools\build_linux.ps1` runs it inside WSL. `tools/build_linux.sh --test` also builds and runs the Linux
test suite (add test names to run some of them); a build with `--test` is not a shipping build.

<details>
<summary>Project layout</summary>

```
include/maicord/   public headers; each module's folders mirror its src/ folders
                   (sdk/ holds the plugin API, sys/ the Win32 and Linux system
                   declarations, os/ and gpu/ the per-system seams)
src/app/           the client: shell (window, input, startup), chat (messages,
                   composer, threads, polls, search), voice (calls, streams,
                   DAVE/MLS), account (login), guild, media, settings,
                   notifications, gateway, net, plugins, store (catalog
                   pages), packages (theme/effect/link install), effects,
                   update, accessibility
src/core/          freestanding runtime: memory, strings, JSON, containers
src/model/         chat state: chat, guild, user, voice, notifications
src/protocol/      Discord wire formats: gateway, http, auth, encoding,
                   messages, guild, user, media, voice
src/ui/            framework (core, layout, kit, widgets, controls, theme,
                   text) and scenes by feature (chat, voice, account, store, ...)
src/render/        renderer: d3d11, text, media atlases, effects, shaders
src/gpu/           GPU backends: d3d11 (Windows), gles (Linux, OpenGL ES over EGL)
src/os/            Linux system layer behind include/maicord/os
src/text/          text engine: unicode, opentype, fonts, shaping, scripts,
                   layout
src/media/         decoders: image, video, audio, containers, compression
src/platform/      per-system services: desktop, net (TLS), crypto, audio,
                   decode (GPU), window (Win32, Wayland), uia and atspi
                   (accessibility), dbus, font, time
tests/             unit tests mirroring src/ (tests/<module>/<folder>/), shared
                   support/, fixtures/, generated/ data and integration/ probes
benchmarks/        opt-in benchmarks
samples/           example plugins (plugins/) and background effects (effects/)
cmake/             build modules included by CMakeLists.txt
config/            application manifest and resource scripts
tools/             build (Windows and Linux), PE and ELF audits, perf harness,
                   release scripts and the maicord-plugin tool
packaging/         Linux packages, AppImage, installer and repositories
third_party/       Opus, Lucide icons, Wayland protocol XML
assets/            fonts, icons, flags, sounds, the example theme and license
                   notices
```

</details>

## Roadmap

- Sending camera video and screen share
- An installer
- More plugin APIs: on-demand Discord fetches and the voice panel
- Remaining Discord features, tracked in
  [issues](https://github.com/maj113/MaiCord/issues?q=is%3Aopen+label%3Aparity)

## Contributing

Bug reports, ideas and pull requests are welcome; please read
[CONTRIBUTING.md](CONTRIBUTING.md) first. Report security problems privately
through [GitHub security advisories](https://github.com/maj113/MaiCord/security/advisories/new).

## License

MaiCord is licensed under the [Mozilla Public License 2.0](LICENSE). Changes
to MaiCord's own files must be shared under the MPL when you distribute them;
plugins and themes that only use the public SDK and theme format can use any
license.

Bundled third-party components keep their own licenses, and release builds
include their notices in the `licenses` and `fonts` folders:

| Component | License | Notice |
| --- | --- | --- |
| [Opus](https://opus-codec.org/) 1.6.1 decoder and encoder subsets | BSD-3-Clause | [`third_party/opus/COPYING`](third_party/opus/COPYING) |
| [Lucide](https://lucide.dev/) icons | ISC | [`third_party/lucide/LICENSE`](third_party/lucide/LICENSE) |
| [Twemoji](https://github.com/jdecked/twemoji) flag artwork | CC BY 4.0 | [`assets/licenses/Twemoji-graphics.txt`](assets/licenses/Twemoji-graphics.txt) |
| Unicode data | Unicode License v3 | [`assets/licenses/Unicode-3.0.txt`](assets/licenses/Unicode-3.0.txt) |
| [gemoji](https://github.com/github/gemoji) emoji aliases | MIT | [`assets/licenses/gemoji-MIT.txt`](assets/licenses/gemoji-MIT.txt) |
| [Emojibase](https://emojibase.dev/) shortcodes (incl. JoyPixels and emoji-data presets) | MIT | [`assets/licenses/emojibase-MIT.txt`](assets/licenses/emojibase-MIT.txt) |
| [ICU](https://icu.unicode.org/) 78.3 Thai word list | Unicode License v3 and ICU 1.8.1–57.1 license | [`assets/licenses/ICU-Thai-dictionary.txt`](assets/licenses/ICU-Thai-dictionary.txt) |
| [PyThaiNLP](https://github.com/PyThaiNLP/pythainlp) 5.3.8 Thai word list | CC0 1.0 | [`assets/licenses/PyThaiNLP-corpus.txt`](assets/licenses/PyThaiNLP-corpus.txt) |
| Profile display fonts | SIL Open Font License 1.1 | [`assets/fonts/`](assets/fonts/README.md) |
| MaiCord notification sounds (original) | CC0 1.0 | [`assets/licenses/MaiCord-sounds.txt`](assets/licenses/MaiCord-sounds.txt) |

## Disclaimer

MaiCord is an independent project and is not affiliated with, authorized by or
endorsed by Discord Inc. "Discord" and related marks are trademarks of
Discord Inc. MaiCord is provided "as is", without warranty of any kind.
