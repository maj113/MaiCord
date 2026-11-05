# Working on MaiCord

Guidance for coding agents and contributors. [CONTRIBUTING.md](CONTRIBUTING.md)
has the same rules in more detail.

## Build and test

- Build only with `tools/build.ps1`, from Windows PowerShell 5.1:
  `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1`
  builds the Release client; add `-Test` for the whole suite and
  `-TestNames a,b` for single tests.
- The only toolchain is clang-cl with lld-link and llvm-lib from the
  maj113/llvm-pe fork: `-LlvmBin`, `MAICORD_LLVM_BIN`, else the release pinned
  in `tools/llvm_toolchain.json` (fetched once into
  `%LOCALAPPDATA%\MaiCord-toolchain`); Visual Studio supplies the SDK, MASM,
  rc and mt.
- The only build directory is the sibling `..\MaiCord-build\Release`. Don't
  create other build trees, staging copies, QA directories or archives, and
  don't move unwanted output elsewhere: delete it.
- If `MaiCord.exe` is running from the build directory, add `-SkipClient` to a
  `-Test` run; the script won't overwrite a running client. Never close
  someone's running client for them.
- The suite must finish within 20 seconds. Keep coverage when trimming test
  work. Tests assert outputs and counts (operations, allocations, bytes),
  never wall-clock times.
- The script removes test executables, objects, temporary profiles and logs
  after each run; leave the build directory holding only the client binaries
  and their runtime assets. For iterative work, `-KeepIntermediates` keeps
  objects, ninja state and `compile_commands.json`, so later builds are
  incremental and `python tools/check_sources.py` can parse changed files and
  every unit including a changed header without building. Finish with a
  normal run to restore the clean directory.
- The Linux port (`src/core` first) builds only with `tools/build_linux.sh`, run in WSL by
  `tools\build_linux.ps1`: stock clang and ld.lld 23, `-nostdinc` with the kernel and glibc interface in
  `include/maicord/sys/linux`, ELF images audited by `tools/audit_elf.py`. Its build directory is `~/mc-build`
  on ext4 (never under `/mnt`), emptied after each run unless `--keep`. The Windows build, its toolchain and every
  rule above stay as they are: code the Windows build compiles must not change for the port, and
  `python tools/compare_pe.py OLD.exe NEW.exe` proves it by comparing two builds section by section.
  The Linux crypto and TLS backend (`src/platform/crypto/openssl`, `src/platform/net/linux/tls_client.cpp`) uses the
  system OpenSSL 3 or 4 through `dlopen` only, so images gain no `DT_NEEDED`: `python tools/check_openssl_abi.py`
  checks its function and constant tables against the installed libssl-dev headers, and the TLS tests run
  `openssl s_server` on the loopback (`tests/platform/net/tls_smoke.sh`).
  The Linux window backend (`src/platform/window/wayland`) loads libwayland-client, libwayland-egl, libwayland-cursor
  and libxkbcommon through `dlopen` too; its protocol bindings are generated at build time by
  `tools/generate_wayland_protocols.py` from the XML in `third_party/wayland` (no `wayland-scanner`). The pure part
  of it (`maicord_wayland_input`: key table, characters, double click, key repeat, the generated tables against
  libwayland-client's) runs in the normal Linux suite; `tools/test_wayland_window.sh` starts headless or nested sway
  and runs the opt-in integration tests (`maicord_wayland_window_lifecycle`; `maicord_wayland_window_input`, which
  drives a virtual pointer and keyboard and two helper clients to check input, client-side decorations, clipboard, IME
  and more; `maicord_wayland_window_egl`, a real EGL surface checked in a `grim` screenshot) on headless sway, with
  `--scale` and `--integer` for the scale paths. `MAICORD_WAYLAND_NO_FRACTIONAL=1` makes a window ignore
  fractional-scale and viewporter.
  The window service (`include/maicord/platform/window/window.hpp`, `event.hpp`, `key.hpp`) is what shared code uses:
  `WindowEvent`s into one sink, `window_*` calls out, key state through `window_key_down`/`window_modifiers`
  (a null `Window` reads the global state), the global push-to-talk keys through `global_key_down`. Windows implements it
  in `src/platform/window/win32` (`maicord_window_win32`: `keys.cpp`, `window.cpp`, `translate.cpp`,
  `window_proc.cpp`, `pump.cpp` for `window_wait`/`window_pump`, `caption_frame.cpp` for the non-client glue,
  `clipboard.cpp`, `drop.cpp`, `settings.cpp`, `icon.cpp`; an `HWND` is the `Window` itself, `win32_hwnd`/`win32_window`
  in `win32.hpp` convert for the Windows-only code), Linux in `src/platform/window/wayland`, and the tests in
  `src/platform/window/null`
  (`maicord_window_null`, listed before `maicord_platform` so its definitions win; `tests/support/window_fixture.hpp`
  builds events and opens it). Handlers under test take events, never messages; see CONTRIBUTING.md. The shell
  (`src/app/shell`) holds no window Win32: the Windows-only application glue (the window's hooks and caption forwarders,
  `maicord_entry`) is `src/app/windows`, and the caption controller (`shell/caption.cpp`) works on events and the
  portable frame geometry (`platform/window/frame.hpp`, `window_frame_hit`) that both backends use. Text input goes
  through `SecureTextStore` (`ui/text/secure_text_store.hpp`): on Windows TSF (`src/ui/text/secure_text_store.cpp`,
  keys filtered in `WindowPumpHooks::key_prefilter`), on Linux `src/ui/text/linux/secure_text_store.cpp`, which tells
  the Wayland text-input-v3 backend about the focused field (`window_wayland_text_input_enable`: text around the caret
  without the preedit, caret rectangle) and applies `ime_preedit`/`ime_commit`/`ime_delete_surrounding` events, routed
  by the dispatcher through `secure_text_store_ime_event`, to the bound `TextInputTarget` as a composition the widgets
  read through `secure_text_store_composition`. `maicord_ime_store` tests it on Linux against the headless window's
  text input log (`null_window_set_text_input_available`, `NullWindowProbe::text_*`).
  The desktop services (`src/platform/dbus`, `include/maicord/platform/dbus`) are an in-house D-Bus client, no libdbus or
  sd-bus: the wire format (`wire.hpp`, little endian, the whole type system, `UNIX_FD` over SCM_RIGHTS), the bus
  connection (`connection.hpp`: EXTERNAL authentication, calls with timeouts, `AddMatch` signals, exported methods; one
  thread per connection, a waitable for the loop), the session bus the window loop waits on (`session.hpp`) and the clients
  over it: the settings portal (`desktop_settings.hpp`: colour scheme, accent, contrast, motion, fonts, cursor, GNOME
  button layout; the window backend maps it to `PlatformSettings` and `settings_changed`), notifications (the daemon
  directly, the portal when sandboxed; `MAICORD_NOTIFICATION_BACKEND=portal|direct`), OpenURI (the portal, then
  `xdg-open` through `posix_spawnp` looked up with `dlsym`), the Secret Service (`secret_service.hpp`, private connections,
  any thread) and a portal probe. The user-bound `secure_buffer` keeps its master key in the Secret Service
  (`application=maicord`, `purpose=master-key`; an old key file moves in and is deleted) and falls back to the 0600 key
  file, logged, when there is no usable keyring; `MAICORD_SECRET_SERVICE=off` forces the file, and every test runs with it
  off unless it asks for `auto`. `maicord_dbus_wire` needs no bus; `tools/test_dbus.sh` starts private `dbus-daemon`s with
  `tests/platform/dbus/mock_services.py` (python3-dbus, python3-gi: libdbus decodes every request, the tests spell out
  what it saw) and runs the connection, service, secure-buffer and no-service tests, which skip (77) without it;
  `tools/test_dbus_real.sh` runs the clients against xdg-desktop-portal with its GTK backend and gnome-keyring on a private
  bus. `tools/test_wayland_window.sh` also starts the mock bus for the lifecycle test's settings stage.
  The same library carries the desktop integration the Windows `integration.hpp`, `system_tray.hpp`, `file_picker.hpp` and
  `taskbar_attention.hpp` give: `tray.hpp` (a StatusNotifierItem with its `com.canonical.dbusmenu`, registered with the
  watcher when one exists, silent without; stock GNOME has none), `global_shortcuts.hpp` (the GlobalShortcuts portal; the
  window backend's `window_wayland_global_shortcut` makes a bound key read as down in `global_key_down`, and focused-window
  only where the portal is missing), `instance.hpp` (single instance and `discord://` links over `org.freedesktop.Application`
  on the application id `io.github.maj113.MaiCord`, or an abstract Unix socket when there is no bus; the application id is
  also the window's app_id and the .desktop file's name), `file_chooser.hpp` (the FileChooser portal with the shape of
  `native_file_picker`) and `launcher_entry.hpp` (the Unity badge signal). `tools/generate_desktop_entry.py` writes the
  `.desktop` file (`MimeType=x-scheme-handler/discord`) and the hicolor icons into `<build>/packaging` (target
  `maicord_desktop_entry`). `tools/test_dbus_tray.sh` checks the tray against swaybar's real watcher and host (a screenshot of
  its bar, `busctl` on the item and its menu) and single instance with `gdbus call`.
  The Linux GPU backend (`src/gpu/gles`) is OpenGL ES 3.1 over EGL; EGL, GLES and libwayland-egl are opened with `dlopen`
  (`libEGL.so.1`, `libGLESv2.so.2`, `libwayland-egl.so.1`); the shaders are the HLSL of `src/render/shaders` translated by
  `tools/build_gles_shader_pack.py` (DXC and SPIRV-Cross pinned in `tools/linux_shader_toolchain.json`, built into
  `~/.cache/maicord-toolchain`). The normal Linux suite renders the ui quads, glyph (compute rasterization, atlas and effect
  materials) and media (PNG, VP8 and JPEG decode compute with read-modify-write blends, media shaders) scenes through the GPU
  API and compares them with PNGs captured from Direct3D 11 (`tests/gpu/golden`, renewed by
  `tools/update_gpu_goldens.py`); it runs on llvmpipe anywhere and on a GPU with `GALLIUM_DRIVER=d3d12` on WSL.
  `MAICORD_GLES_NO_COPY_IMAGE=1` takes the framebuffer-copy path of drivers without copy-image and
  `MAICORD_GLES_MAX_COMPUTE_INVOCATIONS=<n>` reports a smaller compute group (the narrow COLR variant, the decode
  gates). `tools/test_gpu_surface.sh` starts headless (or nested) sway, runs `tests/gpu/surface_wayland.cpp` (an EGL
  window surface fed by an offscreen frame texture, damage blits with buffer age, frame callbacks as the frame clock)
  and checks a `grim` screenshot, with `--scale` for the scale paths.
  The background effects run through the same pass executor as on Windows (`src/render/d3d11/effect_renderer.cpp`, built
  into `maicord_effect_host`). A package of `.mcfx` interface 4 carries a GLSL section (type 10, one entry per DXBC shader:
  `include/maicord/gpu/glsl_shader.hpp`) next to the DXBC, which Windows ignores and Linux runs through
  `gpu_*_shader_create_blob`; `tools/maicord-plugin/mcfx_tool.py effect build --glsl auto|yes|no [--dxbc-from PACKAGE]`
  makes it with `tools/glsl_translate.py` (the shader pack's DXC and SPIRV-Cross chain; `MAICORD_GLSL_TOOLCHAIN=<dir>`
  names a prebuilt one). `tests/gpu/effects.cpp` (one source for both systems) renders every `samples/effects` effect and
  Silk Waves (`tests/gpu/effects/*.mcfx`) for 24 frames through the executor, including persistent and multi-pass targets,
  and compares named frames with PNGs captured from Direct3D 11 by PSNR (`tools/update_gpu_goldens.py --scene effects`);
  `MAICORD_GPU_EFFECT_DUMP=<prefix>` writes the raw pictures. Without GL_EXT_blend_func_extended
  (`MAICORD_GLES_NO_DUAL_SOURCE=1`) the composite pixel shader is drawn as a weight pass and a colour pass; without
  GL_EXT_disjoint_timer_query (`MAICORD_GLES_NO_TIMER_QUERY=1`, which the effects tests set so the cost probe cannot move
  a resolution) `caps.timer_query` is false; `MAICORD_GLES_CHECK=1` reports GL errors after each draw and clear.
  `tests/gpu/sync.cpp` covers the prestart worker, view and `gpu_retain` ownership, timers and fences. Float render targets
  (`rgba32f`) need ES 3.2 or `GL_EXT_color_buffer_float` and are sampled with a point twin where the driver cannot filter them.
  The shared renderer (`src/render`: the `D3D11Renderer` logic, text, layout, media, caption, effects, presentation and
  damage) builds on Linux over the same `gpu_*` calls (`cmake/linux_render.cmake`: `maicord_render_core`,
  `maicord_atlas_gpu`, `maicord_renderer`, `maicord_paint_program`, with the scene library `maicord_ui` it paints and the
  theme in `maicord_theme`). `renderer_initialize` takes a `GpuSurfaceDesc`: the application passes
  `renderer_window_surface(window)`; a null window is an offscreen surface on the GLES backend (the frame texture alone, no
  swap; `gpu_surface_target_texture` reads it back), which is how the Linux tests run. `tests/render/scenes.cpp` is one
  source for both systems: the bootstrap title at three DPIs through the standalone draw path and the window caption
  (band, glyphs, hover and pressed fills, update badge, theme colours checked on any font) on llvmpipe and
  `GALLIUM_DRIVER=d3d12`, and on Windows over a hidden window. Text differs between the systems by design (system fonts),
  so the scenes check theme colours, counts and repeat-draw identity rather than a Windows capture of text; the
  probes under `tests/integration` remain Windows tests (they read the swap chain with Direct3D 11 and own an HWND), so
  a probe's scene moves to `tests/render` as a portable test, not by building the probe. Not on Linux yet: Lottie
  stickers (`src/render/effects/lottie_program.cpp` flattens paths with Direct2D geometry) and the colour-glyph paint
  programs' tests that use it; the video frame upload is `maicord_gpu_gles_video`'s.
  The Linux audio backend (`src/platform/audio/pulse`) loads libpulse (`libpulse.so.0`: PulseAudio, pipewire-pulse and
  WSLg) through `dlopen`; with no libpulse or no server, audio is an unavailable state (empty device lists, a
  `waiting` capture, a device error in the media and notification status), never a crash (`MAICORD_PULSE_LIBRARY`
  names another soname). `python tools/check_pulse_abi.py` checks its function, constant and struct-layout tables
  against the installed libpulse-dev headers. The normal Linux suite runs the arithmetic of the transport with an
  injected clock (`maicord_audio_pure`), the media and notification logic shared with Windows
  (`maicord_notification_audio`) and the no-server and no-library cases; `tools/test_pulse_audio.sh` runs the opt-in
  `maicord_pulse_loopback` against WSLg's PulseServer, `$PULSE_SERVER` or a private `pulseaudio`: it loads a null sink,
  plays voice, notification and media audio into it and records the sink's monitor with the voice capture. Text-to-speech
  has no Linux engine (`speech_speak` returns false); speech-dispatcher is the later backend.
  AAC on Linux is an in-house AAC-LC decoder (`src/media/audio/aac_lc.cpp`, interface in
  `src/platform/decode/linux/aac_decoder.cpp`; Windows keeps Media Foundation): mono and stereo, section and
  scalefactor data, Huffman codebooks, pulses, M/S, intensity stereo, PNS, TNS, sine and KBD windows, long and short
  blocks. HE-AAC (SBR, PS) is refused at `aac_decoder_initialize`: every streaming source the client resolves (DASH,
  HLS, embeds) is `mp4a.40.2`, and only a user-uploaded MP4 can carry HE-AAC. `src/media/audio/aac_tables.inc` is
  generated by `tools/generate_aac_tables.py` and `tests/fixtures/aac` by `tools/generate_aac_fixtures.py`, both from
  the system libavcodec in WSL (not run by the build); `maicord_aac_lc` compares the decoder with libavcodec's own
  within half a PCM16 step, feeds the MP4 and ADTS paths, builds a pulse frame by hand and corrupts frames.
  Video on Linux is a separate decoder stack behind the unchanged `gpu_stream_decoder.hpp`
  (`src/platform/decode/linux`; the DXVA files stay Windows-only). A stream starts on the hardware tier, VA-API
  (`src/platform/decode/vaapi`; `libva.so.2` and `libva-drm.so.2` through `dlopen`, the render node from the EGL device or
  the first `/dev/dri/renderD*`; H.264, HEVC and AV1 parameter and slice buffers built from the in-house parsers, the
  surfaces exported with `vaExportSurfaceHandle` and imported as EGLImages, one per plane, by
  `src/gpu/gles/video_planes.cpp`) and falls to the software tier (`src/platform/decode/software`, `libdav1d.so.7`/`.6`
  for AV1 and `libopenh264.so.7`/`.6`/`.8` for H.264, 8-bit 4:2:0, CPU planes uploaded as textures) when the probe lists
  no hardware codec or the hardware gives up on a stream (the profile, an AV1 stream with film grain, three failed
  pictures in a row). There is no software HEVC, and software H.264 refuses a stream that has B slices (OpenH264 returns
  pictures in decode order). `video_capability.hpp` turns the probes into what the client advertises to Discord: on
  Linux only codecs that can be decoded, hardware ones first, H.264 offered only when some tier decodes it; Windows is
  unchanged. Environment: `MAICORD_VAAPI=off`, `MAICORD_VAAPI_LIBRARY`, `MAICORD_VAAPI_DRM_LIBRARY`,
  `MAICORD_VAAPI_DEVICE` (a render node), `MAICORD_DAV1D_LIBRARY`, `MAICORD_OPENH264_LIBRARY`. `python
  tools/check_video_abi.py` checks the three ABI declarations (`vaapi/abi.hpp`, `software/abi.hpp`) against libva-dev,
  libdav1d-dev and libopenh264-dev; `tools/generate_video_fixtures.py` (libavcodec, libx264, libx265 and libaom in WSL, not
  run by the build) writes `tests/fixtures/video` and the slice-header oracle. The tests: `maicord_video_software`
  (exact against libavcodec), `maicord_video_stream_software` (decode, plane import and the GLES conversion, read back
  against a CPU reference), `maicord_vaapi_parameters` (every VA parameter against the DXVA builders' output and the
  libavcodec oracle), `maicord_video_hardware_tier` (tier choice, the fallback and a VA session against a recording
  libva) and `maicord_video_capability`. No test decodes through a real VA-API driver (WSL has no `/dev/dri`): decode on
  Intel, AMD and NVIDIA, the dma-buf export and import and the sync between VA and GL stay to be validated on hardware.
  Files and sockets are neutral seams too: `include/maicord/os/file.hpp` (open, read, write, atomic replace, identity,
  enumeration, directory watch, mmap; errors in `thread_last_error()`, Windows inline forwarders, Linux bodies in
  `src/os/linux`) and `include/maicord/platform/net` (`NetworkAddress` and `network_last_error()` instead of `sockaddr`
  and `WSAGetLastError()`; `socket.hpp` for the blocking and loopback helpers). The Linux net layer
  (`src/platform/net/linux`) emulates IOCP with epoll, an eventfd and a ready list, resolves names with `getaddrinfo` on
  a small thread pool (the only libc import beyond the audited list) and runs files on a pread/pwrite pool.
  Caches and logs go to `cache_path`/`state_path` (`$XDG_CACHE_HOME`, `$XDG_STATE_HOME`); each Linux test gets private
  ones. `maicord_network_loop` tests the layer on local sockets and `maicord_tls_stream_smoke` runs `tls_stream` over
  it against a local `openssl s_server` (skipped without the `openssl` command).
  Lottie stickers need geometry (flattening, area, boolean combination for mattes, masks and merge paths, stroke
  widening): Windows asks Direct2D, which the Lottie compiler (`src/render/effects/lottie_program.cpp`) uses as a CPU
  geometry engine, and Linux runs the in-house one in `src/render/effects/vector_geometry.cpp`, `vector_overlay.cpp`
  and `vector_stroke.cpp` behind the same method names (`include/maicord/os/linux/d2d1.hpp` gives the compiler the
  Direct2D vocabulary; the Windows code is untouched). It keeps cubics and lines, and boolean results keep the original
  curves between crossings; arenas hold everything. `maicord_lottie_geometry` compares it with answers Direct2D gave
  for a fixed list of shapes (areas, bounds and coverage; `tests/render/effects/golden/lottie_geometry.bin`, captured
  with `MAICORD_LOTTIE_GEOMETRY_CAPTURE=<file>` on Windows, which also runs both live), and `maicord_lottie_program`
  compiles the generated corpus `tests/render/effects/corpus` (`tools/generate_lottie_corpus.py`) and compares the
  masks' coverage with `golden/lottie_program.bin` (`MAICORD_LOTTIE_PROGRAM_CAPTURE=<file>`). Both write nothing and
  need no GPU; a list of other files (`MAICORD_LOTTIE_CORPUS_LIST`, see the test's header) is compared the same way.
  The application transport and updater units build on Linux too (`cmake/linux_app_libraries.cmake`: `maicord_app_net`,
  `maicord_app_update`, `maicord_app_packages`), with `maicord_update_pipeline` (manifest, artifact, acceptance,
  preparation, package), `maicord_https_file_download`, `maicord_transport` (WebSocket, HTTP/1, REST request heads and
  transactions, TLS stream contract, resolver, worker, loopback), `maicord_client_identity` and
  `maicord_gateway_self_status_runtime` in the normal suite. Shared code reaches files, events, threads and time only
  through `os/` and `core/time.hpp`; where the two systems differ (rooted paths against drive paths, advisory sharing
  against sharing modes) the fixture keeps the Windows-only expectations under `#if defined(_WIN32)`.
  The client identity is the official desktop client of the host platform, because the account is the same account:
  `client_user_agent.hpp` selects the profile (user agent, `client_version`, Electron and Chrome versions,
  `native_build_number`, `os`, `sec-ch-ua-platform`) at compile time, and `user_client_configuration.cpp` fills the
  host-derived properties through `RtlGetVersion` and the registry on Windows or through
  `src/app/account/linux/client_identity.cpp` on Linux: `os_version` is the kernel release (`uname`), `system_locale`
  and Accept-Language come from `$LANGUAGE`, `$LC_ALL`, `$LC_MESSAGES`, `$LANG` through the same shipped-locale
  resolution as Windows, and `window_manager` (`$XDG_CURRENT_DESKTOP,$GDMSESSION`), `distro` (lsb-release's
  description, else os-release's `PRETTY_NAME`), `runtime_environment` and `display_server` (`$XDG_SESSION_TYPE`)
  follow discord_desktop_core. `GatewayUserProperties::linux_desktop` selects the Linux object layout
  (`os_version_is_release` after `os_version`, no `os_sdk_version`, the host fields after `browser_version`); the
  Windows layout is unchanged. The Linux reader takes a `LinuxClientEnvironment` value, so
  `tests/app/account/client_identity.cpp` pins the exact JSON for fixture hosts; when Discord ships a new desktop
  build, bump the Linux constants and those expected strings together (the Windows profile is separate).
  System fonts on Linux (`src/platform/font/linux`, interface `include/maicord/platform/font`) replace the GDI
  `SystemFontLocator`: `libfontconfig.so.1` is opened with `dlopen` for a family, weight and slant (`fontconfig.hpp`
  lists the functions used) and closed again after each catalog query; with no libfontconfig an in-house scan reads
  the directories of `fonts.conf` (`<dir>`/`<include>`, `FONTCONFIG_FILE`/`FONTCONFIG_PATH`, the XDG and NixOS roots).
  Defaults come from `default_fonts.hpp` (GTK `settings.ini` and KDE `kdeglobals`, else fontconfig's `sans-serif`;
  Noto Color Emoji for emoji). System fallback asks `FcFontSort` for the route's sample scalar first, then the
  generated range table `src/ui/text/generated/linux_fallback_data.inc` (`python tools/font_reference/linux_fallback.py
  --ucd <UCD-17.0.0.zip>`, CJK variants per language). Colour bitmap emoji (CBLC/CBDT, PNG images) are read by
  `maicord_font_color` on Linux only (`src/text/opentype`; Windows' Segoe UI Emoji is COLR) and reach the colour atlas
  through `colr_paint_upload_pixels`; `tools/font_reference/make_cbdt_fixture.py` regenerates the test fixture. The
  font tests (`cmake/linux_font_tests.cmake`) hold on any host and are strict about the installed families when
  `MAICORD_TEST_FONT_SET=ubuntu` (the registry sets it; the Ubuntu packages are `fonts-dejavu-core`, `fonts-noto-core`,
  `fonts-noto-cjk`, `fonts-noto-color-emoji`, `fonts-liberation`). `maicord_font_locator` also runs on NixOS, where
  libfontconfig is found only through `NIX_LD_LIBRARY_PATH` or the scan answers.
  Local time on Linux (`include/maicord/os/local_time.hpp`, `src/os/linux/local_time.cpp`) is the TZif database read by
  the pure reader `src/platform/time/tzif.cpp`: `$TZ` (a name, `:name`, a path or a POSIX rule), else `/etc/localtime`,
  else `/etc/timezone`, in `$TZDIR` or the usual zoneinfo directories (NixOS: `/etc/zoneinfo`); a zone that changed is
  read again within fifteen seconds, an unreadable one is UTC. The Linux log's stamps and `host_iana_timezone` use it.
  `python3 tools/tzif_fixtures.py` (zic and Python's zoneinfo, in WSL) regenerates `tests/generated/tzif_*.bin` and
  `tests/fixtures/zoneinfo`; `maicord_tzif` also runs in the Windows suite. Date and time text goes through
  `include/maicord/os/date_format.hpp` (English on Linux, hour cycle from `LC_ALL`/`LC_TIME`/`LANG`).
  The spell checker on Linux (`src/platform/desktop/linux/spell_checker.cpp`) opens Hunspell (`libhunspell-1.7.so.0`),
  else Enchant 2 (`libenchant-2.so.2`), with `dlopen`, for the locale of `LC_ALL`/`LC_MESSAGES`/`LANG` (else `en-US`);
  Hunspell dictionaries come from `$DICPATH`, the XDG data directories, `/usr/share/hunspell`, `/usr/share/myspell` and
  the NixOS profiles (UTF-8 only); `MAICORD_SPELLCHECK_BACKEND` (hunspell | enchant) and `MAICORD_HUNSPELL_LIBRARY` /
  `MAICORD_ENCHANT_LIBRARY` choose or replace a backend. The tests use `tests/fixtures/hunspell` and, with
  `MAICORD_TEST_SPELL_SET=ubuntu` (set by the registry), the packages `hunspell-en-us`, `hunspell-de-de` and
  `libenchant-2-2`.
  Linux CI is the `linux` and `distros` jobs of `.github/workflows/ci.yml` (ubuntu-24.04; nothing is published). The
  toolchain is installed by `tools/install_linux_toolchain.sh` (clang, lld and llvm 23 from apt.llvm.org, whose archive
  key is checked against its fingerprint; the major is the pin because the archive keeps only the newest 23.x build;
  `--tests` and `--integration` add the runtime libraries, fonts, dictionaries and tools, named in its header).
  `tools/build_linux.sh` builds the checkout where it is when it sits on a native Linux file system or `$CI` is set, and
  rsyncs it to ext4 only from a Windows or network mount (`--sync`, `--in-place`); it puts `/usr/lib/llvm-23/bin` first,
  refuses another clang major and takes `--timeout-scale N` for slow runners. The shader toolchain cache
  (`~/.cache/maicord-toolchain`) is keyed by `tools/linux_shader_toolchain.json`, `tools/shadertool/mc_shadertool.cpp`,
  `tools/fetch_linux_shader_toolchain.py` and the compiler that builds the tool. The sway, GPU surface, D-Bus and
  PulseAudio scripts run after the suite and report without failing until the repository variable
  `MAICORD_LINUX_INTEGRATION_BLOCKING` is `true`. `tools/linux_portable_tests.txt` names the cases that
  `tools/test_linux_distros.sh` runs in containers of Ubuntu 22.04, Debian 12, Fedora, Arch Linux and NixOS (the same
  executables; `tools/linux_portable_tests.py` packs them): locally `python3 tools/linux_portable_tests.py` needs a
  build, and the runner needs docker or podman (`sudo mount --make-rshared /` once for rootless podman on WSL; the
  containers use the host network, which also means no `/dev/net/tun` is needed). Each distribution installs only
  the packages of the `dlopen`s, listed in the script's header; NixOS is `nixos/nix` with the glibc loader linked at
  `/lib64` and `LD_LIBRARY_PATH` set, which is what `programs.nix-ld` does. The matrix found two things worth knowing:
  OpenSSL 3.0.2 (Ubuntu 22.04) returns a compressed point for the "pub" parameter of a key made from a private
  scalar, so `p256.cpp` reads `qx` and `qy`; and OpenSSL before 3.2 reports a key identifier mismatch for a trusted
  certificate with the same subject as the self-signed one under test, so `tls_smoke.sh` gives them different subjects.
  Linux packaging is in `packaging/` (read its README): `build_tarball.py` makes the release tarball, around which the AUR
  `maicord-bin`, the `.deb`/`.rpm` (nfpm), the Flatpak and the Nix flake (`packaging/nix`, a source build with
  `llvmPackages_23` and a `-bin` variant) are built; `packaging/check.sh` is the CI check that needs no client (stand-in
  program `--stub`). The application id is `io.github.maj113.MaiCord`. Only the tarball install carries `.maicord-portable`
  beside the executable and so may self-update (the updater's Linux mode must check it next to `/proc/self/exe` in
  `application_update_build_config()`); every package-manager install leaves it out. The `linux` job of `release.yml` is
  off until `MAICORD_LINUX_RELEASES` is `true`. Flathub needs a hand-written source-build manifest (its policy refuses
  AI-generated manifests and AI-opened submissions): do not write or submit one.
- The application (`src/app`, `src/ui`) builds for both systems. Not in `maicord_app_client` on Linux: `src/app/shell`
  (the client's own sources), `src/app/windows` (the Win32 entry and window procedure), `src/app/linux` (the Linux entry
  and window glue) and the UI Automation server (`accessibility.cpp`,
  `accessibility_actions.cpp`; `src/app/accessibility/linux` stands in, the snapshots are shared).
  `cmake/linux_app_client.cmake` globs `src/app` into `maicord_app_client`, so a new unit builds on Linux by default and
  must use the seams: `os/process.hpp` (`process_launch_detached`), `os/text.hpp` (`utf16_to_utf8`, `utf8_to_utf16`),
  `os/thread.hpp` (`thread_waitable` for a wait set that holds a thread, `thread_com_enter`), `os/local_time.hpp` (local
  dates), `core/data_path.hpp` (`environment_variable`, `local_data_root`, `executable_path`), `core/module.hpp`
  (`module_image`), `core/resource.hpp` (`program_resource`: RCDATA on Windows, `#embed` data on Linux),
  `core/virtual_memory.hpp` (`system_memory_low`), `platform/desktop/shell_open.hpp` (`desktop_open_target`,
  `desktop_reveal_file`, `desktop_alert_beep`) and a `Window` wherever a window is passed (`desktop_window_reveal`,
  `browser_sign_in_launch`, `taskbar_attention_apply`, `native_file_picker_start`), never an `HWND`. A Windows-only part
  keeps `#if defined(_WIN32)` in its file with the Linux alternative beside it: `notifications/desktop.cpp` (the tray
  over the StatusNotifierItem and the single instance over `org.freedesktop.Application`),
  `platform/desktop/taskbar_attention.cpp` (the launcher entry), `platform/desktop/linux/notifications.cpp` (toasts over
  D-Bus), `platform/desktop/linux/integration.cpp` (the startup entry is an XDG autostart file, the scheme handler a
  desktop file plus the default in `mimeapps.list`), `platform/desktop/linux/webauthn.cpp` (unavailable), the plugin
  loader (`PluginLoadError::unsupported`) and game detection (`/proc/*/comm`). The application and UI fixtures that need
  no Win32 window run on Linux from `cmake/linux_app_ui_tests.cmake` over `maicord_window_null`, with the Win32 calls
  they make shimmed for tests only in `tests/support/linux_test_app.hpp`; a fixture that calls other Windows API
  directly stays Windows-only and is listed there.
  The client is the `MaiCord` target (`cmake/linux_client.cmake`): the shell (`src/app/shell`) compiled on Linux with
  `src/app/linux/entry.cpp` (`argv` as the one command line `desktop_launch_parse` reads, via
  `platform/desktop/launch_line.hpp`: `--startup`, `--url -- <link>`, a bare `discord://` argument and single-instance
  forwarding are the Windows startup's) and `src/app/linux/platform_window.cpp` (the window through the Wayland backend;
  `application_platform_window_prepare` connects the compositor before the renderer's device start, which EGL needs). Only
  the Wayland backend is linked (`maicord_window_null` is the tests'); the resources Windows takes from `.rc` are
  embedded (`core/linux/resource.cpp`: the sounds and the icon, which `window_app_icon_png` reads as an ICO). Shared
  paths join with `kPathSeparator` (`core/data_path.hpp`), not a literal backslash, wherever they are compared or
  split, and `directory_ensure` makes missing ancestors on Linux. The updater is disabled on Linux until the release
  channels carry the tarball (`application_update_build_config()`, `kLinuxUpdatesPublished`); then only the install with the
  `.maicord-portable` marker beside its real executable (`update_portable_install`) enables it. `build_linux.sh` leaves
  `MaiCord`, `fonts/` and `licenses/` in `~/mc-build`. `tools/test_client_wayland.sh` (headless or nested sway, the running
  compositor; screenshot, idle CPU, wakeups, RSS, binary size) and `tools/client_flow.py` (clicks over
  `tools/wayland_pointer.py`, typing, scale and window commands, screenshots) run it with a private empty profile;
  `GALLIUM_DRIVER=d3d12` selects WSL's GPU, and WSLg's Weston has no screencopy (nested sway is the screenshot path).

## Code rules

- For Discord API, authentication and Gateway work, consult
  [Discord Userdoccers](https://docs.discord.food/) for new features and to verify
  existing behavior, alongside Discord's official documentation. Cross-check
  uncertain details against the supplied client research; keep regression tests
  for request metadata, session lifetimes, rate limits and challenge handling.
  Its [MCP endpoint](https://docs.discord.food/mcp) provides `search_docs` and
  `fetch_page`; use the sitemap/navigation to enumerate all pages and sections.
- Freestanding C++23: no STL, CRT, exceptions or RTTI. Use
  `include/maicord/core`. Every build runs the PE audits in
  `tools/audit_pe.ps1` (imports, debug data, initialized-data budget).
- Code compiles without unwind tables: no SEH, unhandled-exception filters or
  stack walks through MaiCord frames (see CONTRIBUTING.md).
- Targets compile with `-nostdinc`: Windows SDK, C library and compiler
  declarations (intrinsics included) come from `include/maicord/sys` and
  `include/maicord/core/win32_*.hpp`, declared exactly as the SDK and compiler
  declare them. Add what you use there; the generated conformance unit
  (`tools/generate_sys_conformance.py`) checks it against the SDK on every
  build.
- UI colors, radii and spacing come from theme tokens and scales
  (`include/maicord/ui/theme/tokens.inc`); `tools/check_theme_literals.py`
  fails on new literals.
- Never ship Discord-owned assets (icons, sounds, illustrations, fonts) or
  copied client code. Research material stays in the git-ignored `Analysis/`
  and `docs/` directories.
- Name things by what they do, not after Discord or reverse-engineering
  artifacts.
- Performance and size work needs before/after numbers: `tools/perf/run_perf.ps1`
  for CPU, wakeups, allocations and memory, and the EXE/DLL sizes from the build.
  Keep only changes that pay for themselves.
- Never log tokens, passwords, signed URLs or message content.

## Commits

- Imperative subject lines, one topic per commit.
- Update tests with every behavior change and register them in
  `cmake/test_registry.cmake`.
