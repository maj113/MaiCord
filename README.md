<div align="center">

<img src=".github/assets/banner.svg" alt="MaiCord — a native Discord client for Windows" width="100%">

**A lightweight, native Discord client for Windows, written in C++ with Direct3D 11.**<br>
No Electron, no browser engine, nothing to install. About 7 MB.

[![CI](https://github.com/maj113/MaiCord/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/maj113/MaiCord/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/maj113/MaiCord?sort=semver&display_name=tag)](https://github.com/maj113/MaiCord/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/maj113/MaiCord/total)](https://github.com/maj113/MaiCord/releases)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011%20x64-0078D6)](#requirements)
[![License](https://img.shields.io/badge/license-MPL--2.0-brightgreen)](LICENSE)

[Website](https://maicord.com/) ·
[Download](https://github.com/maj113/MaiCord/releases/latest) ·
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
scratch in freestanding C++23 (no STL, CRT, exceptions or RTTI) and draws its
own interface on the GPU.

| | MaiCord |
| --- | --- |
| Download | ~5.5 MB zip, portable |
| On disk | one EXE + one DLL, ~7 MB together |
| Dependencies | only DLLs that ship with Windows |
| First frame | ~60 ms after launch¹ |
| Idle | near-zero CPU: nothing redraws while nothing changes |

¹ Measured on the developer's machine with the in-tree perf harness
([`tools/perf`](tools/perf/README.md)); your numbers will differ.

## Features

**Chat**
- Discord markdown, including syntax-highlighted code blocks and
  [ANSI colored text](https://gist.github.com/kkrypt0nn/a02506f3712ff2d1c8ca7c9e0aed7c06)
- Rich embeds with image galleries, link previews, stickers and custom emoji
- Replies, forwards, pins, reactions, threads, forum posts and polls (create and vote)
- Record and send voice messages
- Emoji, GIF and sticker picker that syncs your Discord favorites
- Paste screenshots straight from the clipboard; drag and drop files
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

**Accounts**
- Password sign-in with two-factor authentication (security keys and Windows
  Hello, authenticator apps, backup codes, SMS), QR login and CAPTCHA support
- Up to five saved accounts with quick switching
- Full profile editor: avatar cropping, banner, decorations, effects and
  profile colors

**Windows integration**
- Native notifications, tray icon and taskbar badge
- Spellcheck and IME input through the Windows text services
- Screen reader support through UI Automation
- Signed automatic updates from GitHub releases

**Make it yours**
- Themes control colors, typography, spacing, components and animated
  backgrounds
- Native plugins with a stable C API: read data, react to events, decorate
  messages, add buttons and menus, register shortcuts, ship background shaders

## Install

1. Download `MaiCord-<version>-windows-x64.zip` from the
   [latest release](https://github.com/maj113/MaiCord/releases/latest).
2. Extract it anywhere, for example `%LOCALAPPDATA%\Programs\MaiCord`.
3. Run `MaiCord.exe`. No installer or admin rights are needed.

MaiCord keeps its settings and saved sign-ins in `%LOCALAPPDATA%\MaiCord` and
updates itself. Every update is checked against a signature built into the
client before it is installed.

### Requirements

- Windows 10 or 11, 64-bit
- A CPU with AVX
- A GPU with Direct3D 11 support

## Plugins and themes

**Themes** are JSON files (up to 64 KiB) imported under
*Settings → Appearance*. A theme starts from the Dark, Light or AMOLED base and
can override any design token. See
[`assets/themes/example.maicord-theme.json`](assets/themes/example.maicord-theme.json).

**Plugins** are native DLLs in `%LOCALAPPDATA%\MaiCord\plugins`, each with a
`*.maicord-plugin.json` manifest. MaiCord checks each DLL's SHA-256 before it
can be enabled and asks you to approve the permissions it requests. Plugins
are native code, so only install ones you trust. The SDK is a single C header,
[`include/maicord/sdk/maicord_plugin.h`](include/maicord/sdk/maicord_plugin.h),
and [`samples/plugins/showcase`](samples/plugins/showcase) uses every part of it.

## Building from source

You need:

- **Visual Studio 2026** (version 18) or its Build Tools, with the
  *Desktop development with C++* workload (MSVC 14.5x, Windows SDK, CMake and
  Ninja)
- **Python 3.10+** on `PATH`
- **Windows PowerShell 5.1** (included with Windows)

```powershell
git clone https://github.com/maj113/MaiCord.git
cd MaiCord
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1
```

The script finds Visual Studio, builds `MaiCord.exe` and `MaiCordAccount.dll`
into the sibling directory `..\MaiCord-build\Release`, and audits both
binaries. Close MaiCord before rebuilding.

Run the test suite with `tools\build.ps1 -Test` (add `-TestNames <name>` for a
single test). It finishes in under 20 seconds and removes its output
afterwards.

<details>
<summary>Project layout</summary>

```
include/maicord/   public headers, one folder per module (sdk/ holds the plugin API)
src/app/           application shell, account runtime, updater, plugin host
src/core/          freestanding runtime: memory, strings, JSON, containers
src/model/         chat state: guilds, channels, messages, members
src/protocol/      Gateway, REST, HTTP/1.1, CDN, voice and encryption
src/ui/            scenes, layout, input and widgets
src/render/        Direct3D 11 renderer, glyph and media atlases, shaders
src/text/          text shaping, bidi, line breaking, fonts, color emoji
src/media/         image, video and audio decoders
src/platform/      Win32: windows, TLS, CNG crypto, audio, notifications
tests/             unit, integration and rendering tests (CTest)
benchmarks/        opt-in freestanding benchmarks
samples/plugins/   example plugins built against the SDK
cmake/             build modules included by CMakeLists.txt
config/            application manifest and resource scripts
tools/             build, PE audit, perf harness and release scripts
third_party/       Opus, Lucide icons, minimp3
assets/            fonts, sounds, themes and license notices
```

</details>

## Roadmap

- Sending camera video and screen share
- An installer
- More plugin APIs: composer hooks and network access
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
include their notices:

| Component | License | Notice |
| --- | --- | --- |
| [Opus](https://opus-codec.org/) 1.6.1 decoder and encoder subsets | BSD-3-Clause | [`third_party/opus/COPYING`](third_party/opus/COPYING) |
| [Lucide](https://lucide.dev/) icons | ISC | [`third_party/lucide/LICENSE`](third_party/lucide/LICENSE) |
| [minimp3](https://github.com/lieff/minimp3) | CC0 1.0 | [`assets/licenses/minimp3.txt`](assets/licenses/minimp3.txt) |
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
