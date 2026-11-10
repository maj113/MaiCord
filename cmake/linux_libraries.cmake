# The static libraries only the Linux client builds (included by cmake/libraries.cmake, which lists the libraries both
# clients share): the Linux platform layers (src/core/linux, src/os/linux and the Linux backends under src/platform,
# src/gpu/gles) and the application, which the Windows build splits into several libraries (cmake/windows_libraries.cmake).
# Each source list is the archive's member order.

# What the desktop shell asks the system for: the time zone identifier, the spell checker (Hunspell or Enchant
# through dlopen; spell_checker.cpp holds the language choice the two systems share), opening a link or a file and the
# launch's arguments as the command line the launch parser reads.
maicord_library(maicord_desktop_system
    src/platform/desktop/linux/timezone.cpp src/platform/desktop/spell_checker.cpp
    src/platform/desktop/linux/spell_checker.cpp src/platform/desktop/linux/shell_open.cpp
    src/platform/desktop/linux/launch_line.cpp
    LIBS maicord_core maicord_dbus)

# The system font locator (include/maicord/platform/font): fontconfig through dlopen with a directory scan as the
# fallback, and the platform's default families.
maicord_library(maicord_font_system
    src/platform/font/linux/system_font_locator.cpp src/platform/font/linux/font_config.cpp
    src/platform/font/linux/font_scan.cpp src/platform/font/linux/default_fonts.cpp
    LIBS maicord_core maicord_font)

# The catalog of system and private faces (src/ui/text): family orders, selection and the fallback routes over the
# system font locator.
maicord_library(maicord_text_catalog
    src/ui/text/native_text_catalog.cpp
    LIBS maicord_core maicord_font maicord_font_system)

# The text system entry points (src/ui/text): the native layout adapter over the catalog, with the emoji presentation
# and theme typography it reads.
maicord_library(maicord_text_system
    src/ui/text/text.cpp src/ui/text/native_text_candidate.cpp src/ui/media/emoji_presentation.cpp
    src/ui/theme/styles.cpp
    LIBS maicord_core maicord_font maicord_unicode maicord_media_core maicord_text_catalog)

# Colour bitmap glyphs (CBLC/CBDT, the emoji format of Noto Color Emoji on Debian, Ubuntu and nixpkgs) over the in-house
# PNG decoder. Linux only: Segoe UI Emoji is COLR, which maicord_font reads on both systems.
maicord_library(maicord_font_color
    src/text/opentype/cbdt_font.cpp src/text/opentype/bitmap_color_raster.cpp
    LIBS maicord_core maicord_font maicord_media_codec maicord_compression)

# The colour atlas side of the renderer: paint programs on the GPU, and the upload of colour bitmap glyphs.
maicord_library(maicord_render_colr_paint
    src/render/effects/colr_paint.cpp
    LIBS maicord_gpu_gles maicord_core)

# The platform crypto and the TLS client over the system OpenSSL (dlopen: no DT_NEEDED), with the random bytes from
# getrandom and the secure buffer on mmap/mlock. aead.cpp, aes_gcm64.cpp and xchacha20_poly1305.cpp are the portable
# files of src/platform/crypto that the Windows build also compiles (their CNG parts are under _WIN32).
maicord_library(maicord_crypto
    src/platform/crypto/aead.cpp src/platform/crypto/aes_gcm64.cpp src/platform/crypto/xchacha20_poly1305.cpp
    src/platform/crypto/linux/random.cpp src/platform/crypto/linux/secure_buffer.cpp
    src/platform/crypto/openssl/openssl.cpp src/platform/crypto/openssl/aead_openssl.cpp
    src/platform/crypto/openssl/aes_gcm.cpp src/platform/crypto/openssl/kdf_sha256.cpp
    src/platform/crypto/openssl/hash_stream.cpp src/platform/crypto/openssl/p256.cpp
    src/platform/crypto/openssl/remote_auth_crypto.cpp
    src/platform/net/linux/tls_client.cpp
    LIBS maicord_core maicord_dbus)

# The network layer (include/maicord/platform/net): the completion-port model on io_uring with its name lookup pool
# (src/platform/net/linux), and the shared TLS stream and worker on top of it.
maicord_library(maicord_net
    src/platform/net/linux/address.cpp src/platform/net/linux/network.cpp src/platform/net/linux/datagram.cpp
    src/platform/net/linux/resolver.cpp src/platform/net/linux/file.cpp src/platform/net/linux/socket.cpp
    src/platform/net/tls_stream.cpp src/platform/net/worker.cpp
    LIBS maicord_core maicord_crypto)

# The OpenGL ES GPU backend of the Linux build (src/gpu/gles, behind include/maicord/gpu/api.hpp). It never enters the
# Windows build: that one compiles src/render/d3d11 behind the same handles.
#
# maicord_gpu_gles          EGL device (libEGL.so.1 and libGLESv2.so.2 by dlopen), caps probe, resources, state tracker
# maicord_gpu_gles_shaders  the GLSL shader pack (tools/build_gles_shader_pack.py) and the creation of the built-ins
#
# Tests that only clear, copy and read back link the first; anything that draws or dispatches needs both. The pack is
# made from the GLSL in src/render/shaders/gles by python alone: no shader compiler runs in the build (the
# toolchain of tools/linux_shader_toolchain.json, DXC and SPIRV-Cross, is for plugin effects only). The second library
# stays out of the default build: the tests that link it build it.

maicord_library(maicord_gpu_gles
    src/gpu/gles/loader.cpp src/gpu/gles/device.cpp src/gpu/gles/resources.cpp src/gpu/gles/draw.cpp
    src/gpu/gles/surface.cpp src/gpu/gles/sync.cpp src/gpu/gles/video_planes.cpp
    LIBS maicord_core INCLUDES src)

maicord_library(maicord_gpu_gles_shaders src/gpu/gles/shader_pack.cpp
    LIBS maicord_gpu_gles maicord_compression maicord_core INCLUDES src "${MAICORD_GENERATED_DIRECTORY}")
set_source_files_properties(src/gpu/gles/shader_pack.cpp PROPERTIES OBJECT_DEPENDS "${MAICORD_GLES_SHADER_PACK}")
set_target_properties(maicord_gpu_gles_shaders PROPERTIES EXCLUDE_FROM_ALL TRUE)
# The pack is #embed'ed (a C23 extension to clang's C++).
target_compile_options(maicord_gpu_gles_shaders PRIVATE -Wno-c23-extensions)

# The effect host: the pass-graph executor and inputs of .mcfx background effects (the code Windows runs, over the GPU
# API) and the package parser. The default theme the executor takes its palette from is maicord_theme's (below).
maicord_library(maicord_effect_host
    src/render/d3d11/effect_renderer.cpp src/render/d3d11/effect_inputs.cpp src/app/packages/mcfx.cpp
    src/app/packages/link.cpp
    LIBS maicord_gpu_gles maicord_media_codec maicord_theme maicord_core INCLUDES src)
target_compile_options(maicord_effect_host PRIVATE -Wno-c23-extensions)

# The default theme (src/ui/theme/theme.cpp), which the effect host takes its palette from; the Windows build compiles it
# into maicord_ui.
maicord_library(maicord_theme src/ui/theme/theme.cpp LIBS maicord_core)

# The Wayland window backend of the Linux build (src/platform/window/wayland), behind include/maicord/platform/window.
# It never enters the Windows build: that one compiles src/platform/window/win32.
#
# The protocol bindings are generated at build time (cmake/generated_sources.cmake); libwayland-client,
# libwayland-egl, libwayland-cursor and libxkbcommon are dlopen()ed, so the image gains no DT_NEEDED. maicord_dbus:
# the desktop settings (the settings portal) and the session bus the loop waits on.
maicord_library(maicord_window_wayland
    src/platform/window/wayland/client.cpp src/platform/window/wayland/xkb.cpp
    src/platform/window/wayland/keymap.cpp src/platform/window/wayland/window.cpp
    src/platform/window/wayland/pointer.cpp src/platform/window/wayland/keyboard.cpp
    src/platform/window/wayland/pump.cpp src/platform/window/wayland/global_keys.cpp
    src/platform/window/wayland/frame.cpp src/platform/window/wayland/extras.cpp
    src/platform/window/wayland/data.cpp src/platform/window/wayland/uri.cpp
    src/platform/window/wayland/text_input.cpp
    # Copy Image encodes its PNG with the clipboard's own encoder, which the Windows backend shares.
    src/platform/desktop/clipboard_image.cpp
    "${MAICORD_WAYLAND_PROTOCOL_HEADER}" "${MAICORD_WAYLAND_PROTOCOL_SOURCE}"
    LIBS maicord_core maicord_media_codec maicord_dbus INCLUDES "${MAICORD_WAYLAND_GENERATED_DIRECTORY}")

# The audio backend of the Linux build (src/platform/audio/pulse), behind include/maicord/platform/audio. It never
# enters the Windows build: that one compiles the WASAPI sources next to them (src/platform/audio/*.cpp).
#
# libpulse is dlopen()ed ("libpulse.so.0": PulseAudio, pipewire-pulse and WSLg), so the image gains no DT_NEEDED and a
# machine without a sound server library or without a running server reports audio as unavailable. media_audio_ring.cpp
# and notification_audio_mixer.cpp are the transport-free halves of media and notification audio the Windows build
# compiles too; text-to-speech is speech-dispatcher through libspeechd, also dlopen()ed (src/platform/audio/speechd).
maicord_library(maicord_audio_pulse
    src/platform/audio/pulse/pulse.cpp src/platform/audio/pulse/voice_audio_output.cpp
    src/platform/audio/pulse/voice_audio_capture.cpp src/platform/audio/media_audio_ring.cpp
    src/platform/audio/pulse/media_audio.cpp src/platform/audio/notification_audio_mixer.cpp
    src/platform/audio/pulse/notification_audio.cpp src/platform/audio/speechd/speech.cpp
    LIBS maicord_core)

# The AAC decoder interface of include/maicord/platform/decode/aac_decoder.hpp on Linux (Windows decodes AAC through
# Media Foundation, src/platform/decode/aac_decoder.cpp): an in-house AAC-LC core in src/media/audio/aac_lc.cpp with
# the Huffman codebooks and band tables of aac_tables.inc (tools/generate_aac_tables.py), and the adapter that
# answers submit/read/drain/flush with it.
maicord_library(maicord_aac_decoder
    src/media/audio/aac_lc.cpp src/platform/decode/linux/aac_decoder.cpp
    LIBS maicord_core)

# The D-Bus client of the Linux build (src/platform/dbus, include/maicord/platform/dbus): the wire format, the bus
# connection over the Unix socket, and the desktop services over it (settings portal, notifications, OpenURI, the Secret
# Service, portal probes, the tray, global shortcuts, single instance, the file chooser, the launcher badge). It never
# enters the Windows build; libc's posix_spawnp (xdg-open fallback) is looked up at run time, so the image gains no
# DT_NEEDED and no import.
maicord_library(maicord_dbus
    src/platform/dbus/wire.cpp src/platform/dbus/connection.cpp src/platform/dbus/bus.cpp
    src/platform/dbus/session.cpp
    src/platform/dbus/desktop_settings.cpp src/platform/dbus/notifications.cpp src/platform/dbus/open_uri.cpp
    src/platform/dbus/secret_service.cpp src/platform/dbus/portal.cpp
    src/platform/dbus/tray.cpp src/platform/dbus/global_shortcuts.cpp src/platform/dbus/instance.cpp
    src/platform/dbus/file_chooser.cpp src/platform/dbus/launcher_entry.cpp
    LIBS maicord_core)

# The AT-SPI server over that client (src/platform/atspi, include/maicord/platform/atspi): the application's objects on the
# accessibility bus for screen readers, answered from the accessibility snapshot (ui/accessibility). Linux only, like
# the UI Automation server it stands beside (maicord_uia, Windows only). Nothing connects until org.a11y.Status says an
# assistive technology is on.
maicord_library(maicord_atspi
    src/platform/atspi/map.cpp src/platform/atspi/accessible.cpp src/platform/atspi/server.cpp
    src/platform/atspi/events.cpp
    LIBS maicord_dbus maicord_ui maicord_core)

# The application's transport and updater libraries: the HTTP/REST and CDN transport, the Gateway client and the
# updater, over the neutral network, file and TLS layers (cmake/windows_libraries.cmake lists the Windows ones).
maicord_library(maicord_app_net
    src/app/net/file_persistence.cpp src/app/net/rest_client.cpp src/app/net/rest_client_transaction.cpp
    src/app/net/rest_client_trace.cpp src/app/net/rest_client_worker.cpp src/app/net/cdn_client.cpp
    src/app/net/https_file_upload.cpp
    src/app/net/https_file_download.cpp src/app/net/signed_out_session.cpp
    src/app/gateway/client.cpp src/app/gateway/client_storage.cpp src/app/gateway/client_transport.cpp
    src/app/gateway/client_outputs.cpp src/app/gateway/client_session.cpp src/platform/desktop/staged_file.cpp
    src/app/account/account_logout.cpp src/app/account/client_session_metadata.cpp
    src/app/account/user_client_configuration.cpp src/app/account/linux/client_identity.cpp
    LIBS maicord_core maicord_protocol maicord_net maicord_crypto maicord_desktop_system maicord_compression
        maicord_media_codec INCLUDES src/app)

maicord_library(maicord_app_update
    src/app/update/manifest.cpp src/app/update/artifact.cpp src/app/update/acceptance.cpp
    src/app/update/prepare.cpp src/app/update/fetch.cpp src/app/update/https.cpp
    src/app/update/service.cpp src/app/update/changelog.cpp src/app/account/login/acceptance_record.cpp
    src/app/update/linux/package.cpp src/app/update/linux/portable.cpp src/app/update/linux/stage.cpp
    LIBS maicord_app_net maicord_compression maicord_core maicord_protocol maicord_net INCLUDES src/app)

# Effect and theme packages (src/app/packages): the link parser, the container reader and the installer; the session
# owner packages.cpp talks to the shell and stays Windows-only.
maicord_library(maicord_app_packages
    src/app/packages/link.cpp src/app/packages/mcfx.cpp src/app/packages/install.cpp
    LIBS maicord_app_net maicord_crypto maicord_core INCLUDES src/app)

# The official catalog's index reader (src/app/plugins): the signed index's verifier and schema, for the test that takes
# a catalog's effects through list, download, parse and install (tests/app/plugins/catalog_effects.cpp). The client
# compiles the same file with the rest of src/app.
maicord_library(maicord_app_catalog_index
    src/app/plugins/catalog_index.cpp
    LIBS maicord_crypto maicord_core INCLUDES src/app)

# The live video decoders of the Linux build (src/platform/decode), behind include/maicord/platform/decode/
# gpu_stream_decoder.hpp. Windows decodes in hardware through DXVA (src/platform/decode/*_gpu_*.cpp) and never
# compiles these.
#
# maicord_video_software  libopenh264 (H.264) and libdav1d (AV1), dlopen()ed: access units to CPU pictures
maicord_library(maicord_video_software
    src/platform/decode/software/software_decoder.cpp
    LIBS maicord_media_codec maicord_core INCLUDES src)

# The hardware tier: VA-API through libva.so.2 and libva-drm.so.2 (dlopen), the VA parameter builders over the in-house
# parsers (and the slice-header parsers VA-API needs that DXVA's short slices do not), and the codec drivers.
# av1_dxva_parameters.cpp is the DXVA builder of the Windows build, compiled here for av1_warp_valid, which the VA
# builder shares, and so that the tests can compare both builders' output.
maicord_library(maicord_video_vaapi
    src/platform/decode/vaapi/va.cpp src/platform/decode/vaapi/va_h264.cpp src/platform/decode/vaapi/va_hevc.cpp
    src/platform/decode/vaapi/va_av1.cpp src/platform/decode/vaapi/va_stream.cpp src/platform/decode/vaapi/va_file.cpp
    src/platform/decode/av1_dxva_parameters.cpp src/media/video/h264_slice_detail.cpp
    src/media/video/hevc_slice_detail.cpp
    LIBS maicord_gpu_gles maicord_media_codec maicord_core INCLUDES src)

# The live stream decoder (the Linux gpu_stream_decoder.hpp) over both tiers, and the movie decoder of attachments
# (mp4_decoder.hpp: the Windows build's mp4_decoder.cpp over h264_file_decoder.cpp and hevc_file_decoder.cpp, the Linux
# h264_gpu_decoder.hpp and hevc_gpu_decoder.hpp). h264_gpu_configuration.cpp is the Windows decoder's container-neutral
# first-picture check and fragment seek.
maicord_library(maicord_video_decode
    src/platform/decode/linux/gpu_stream_decoder.cpp src/platform/decode/linux/software_stream.cpp
    src/platform/decode/mp4_decoder.cpp src/platform/decode/linux/h264_file_decoder.cpp
    src/platform/decode/linux/hevc_file_decoder.cpp
    src/platform/decode/h264_gpu_configuration.cpp
    LIBS maicord_video_vaapi maicord_video_software maicord_gpu_gles maicord_media_codec maicord_core INCLUDES src)
maicord_library(maicord_gpu_gles_video src/gpu/gles/video_frame.cpp
    LIBS maicord_gpu_gles_shaders maicord_gpu_gles maicord_core INCLUDES src)
set_target_properties(maicord_gpu_gles_video PROPERTIES EXCLUDE_FROM_ALL TRUE)

# The application proper on Linux: every unit of src/app that the transport, updater and package libraries above do
# not already hold, as one library (cmake/client.cmake lists them for Windows). The shell (src/app/shell) belongs to
# the window layer and is not here, nor are src/app/windows (the Win32 entry and window procedure) and src/app/linux
# (the Linux entry and window glue): those are the client's own (cmake/client.cmake). accessibility.cpp is the UI
# Automation server (the units that stand in for it are in
# src/app/accessibility/linux: the AT-SPI server's host, over platform/atspi); the snapshot (accessibility_describe.cpp),
# the actions it routes (accessibility_actions.cpp) and the announcements (accessibility_status.cpp) are shared.
# src/app/update/package.cpp, the zip and PE reader of the Windows updater, is not here either:
# src/app/update/linux/package.cpp (in maicord_app_update) reads the Linux tarball instead.
file(GLOB_RECURSE MAICORD_APP_CLIENT_SOURCES CONFIGURE_DEPENDS RELATIVE "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/app/*.cpp")
list(FILTER MAICORD_APP_CLIENT_SOURCES EXCLUDE REGEX
    "^src/app/shell/|^src/app/windows/|^src/app/linux/|^src/app/accessibility/accessibility\\.cpp$|^src/app/update/package\\.cpp$")
foreach(library maicord_app_net maicord_app_update maicord_app_packages maicord_app_catalog_index)
    get_target_property(held_sources ${library} SOURCES)
    list(REMOVE_ITEM MAICORD_APP_CLIENT_SOURCES ${held_sources})
endforeach()

maicord_library(maicord_app_client ${MAICORD_APP_CLIENT_SOURCES}
    # The desktop integration the application drives (platform/desktop): launch and link parsing, the Linux startup and
    # protocol registration, the taskbar badge, the notification-area icon's drawing and plans, the sign-in launcher.
    src/platform/desktop/integration.cpp src/platform/desktop/linux/integration.cpp
    src/platform/desktop/taskbar_attention.cpp src/platform/desktop/system_tray.cpp
    src/platform/desktop/browser_sign_in_launcher.cpp src/platform/desktop/linux/fido2.cpp
    src/platform/desktop/linux/webauthn_ceremony.cpp
    src/platform/desktop/linux/notifications.cpp src/platform/desktop/linux/notification_identity.cpp
    # The file dialog (the FileChooser portal) and the dropped and pasted files it checks.
    src/platform/desktop/linux/file_picker.cpp
    # The colour picker's eyedropper (the Screenshot portal's PickColor).
    src/platform/desktop/screen_color.cpp
    src/model/chat/message_reconcile.cpp
    LIBS maicord_app_net maicord_app_update maicord_app_packages maicord_app_catalog_index maicord_ui maicord_core maicord_model
        maicord_protocol maicord_media_core maicord_media_codec maicord_compression maicord_crypto
        maicord_desktop_system maicord_dbus maicord_atspi maicord_capture maicord_video_encode
    INCLUDES src/app "${MAICORD_GENERATED_DIRECTORY}")
target_compile_options(maicord_app_client PRIVATE -Wno-c23-extensions)

# The secure text field: the edit model, its history and target, and the Linux text store (the Windows one is the
# message-driven store in secure_text_store.cpp).
maicord_library(maicord_ui_secure_text
    src/ui/text/secure_edit.cpp src/ui/text/secure_edit_history.cpp src/ui/text/secure_edit_target.cpp
    src/ui/text/linux/secure_text_store.cpp
    LIBS maicord_ui maicord_core maicord_crypto INCLUDES src)
