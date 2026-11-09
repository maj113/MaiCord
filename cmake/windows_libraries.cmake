# The static libraries only the Windows client builds (included by cmake/libraries.cmake, which lists the libraries
# both clients share): the Win32 platform layer, the Direct3D 11 backend and the application libraries the Linux build
# takes from src/app as one library (cmake/linux_libraries.cmake). Each source list is the archive's member order.

# The TLS stream's imports (Winsock, Schannel through sspicli, bcrypt, crypt32).
set(maicord_tls_libs ws2_32 maicord_sspicli bcrypt crypt32)

# Checks MaiCord's SDK-free headers against the Windows SDK and compiler
# headers on every client build (a MaiCord dependency): one generated unit
# (tools/generate_sys_conformance.py), so the SDK headers are parsed once. It
# defines nothing, so no image links it.
set(maicord_sys_headers
    include/maicord/core/win32_types.hpp include/maicord/core/win32_error.hpp
    include/maicord/core/win32_kernel.hpp include/maicord/core/win32_exception.hpp
    include/maicord/core/win32_image.hpp include/maicord/core/win32_process_launch.hpp
    include/maicord/core/win32_threadpool.hpp include/maicord/core/win32_system_time.hpp
    include/maicord/core/win32_file_info.hpp include/maicord/core/win32_user.hpp
    include/maicord/sys/crt.hpp include/maicord/sys/crt_extra.hpp include/maicord/sys/intrin.hpp
    include/maicord/sys/intrin_mem.hpp
    include/maicord/sys/com.hpp include/maicord/sys/ole.hpp include/maicord/sys/registry.hpp
    include/maicord/sys/gdi.hpp include/maicord/sys/winsock.hpp include/maicord/sys/crypto.hpp
    include/maicord/sys/sspi.hpp include/maicord/sys/shell.hpp include/maicord/sys/winrt.hpp
    include/maicord/sys/dxgi.hpp include/maicord/sys/d3d11.hpp include/maicord/sys/dcomp.hpp
    include/maicord/sys/dxva.hpp include/maicord/sys/d2d1.hpp include/maicord/sys/dwrite.hpp
    include/maicord/sys/wasapi.hpp include/maicord/sys/media_foundation.hpp include/maicord/sys/tsf.hpp
    include/maicord/sys/uia.hpp include/maicord/sys/speech.hpp include/maicord/sys/spellcheck.hpp
    include/maicord/sys/webauthn.hpp include/maicord/sys/process_info.hpp include/maicord/sys/desktop_capture.hpp
    include/maicord/sys/libc/stddef.h include/maicord/sys/libc/stdint.h include/maicord/sys/libc/limits.h
    include/maicord/sys/libc/stdarg.h include/maicord/sys/libc/crtdbg.h)
set(maicord_sys_conformance_unit "${MAICORD_GENERATED_DIRECTORY}/sys_conformance.cpp")
add_custom_command(OUTPUT "${maicord_sys_conformance_unit}"
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_sys_conformance.py"
        --root "${CMAKE_CURRENT_SOURCE_DIR}" --headers ${maicord_sys_headers} --output "${maicord_sys_conformance_unit}"
    DEPENDS ${maicord_sys_headers} tools/generate_sys_conformance.py
    VERBATIM)
add_library(maicord_sys_conformance OBJECT src/core/win32_sdk_conformance.cpp src/platform/window/win32/key_conformance.cpp
    "${maicord_sys_conformance_unit}")
target_link_libraries(maicord_sys_conformance PRIVATE maicord_compile_contract)
target_include_directories(maicord_sys_conformance PRIVATE src/sys)
# The checks name every vtable slot, including members the SDK deprecates. The
# bytecode constant interpreter evaluates their thousands of static_asserts in
# a tenth of the time.
target_compile_options(maicord_sys_conformance PRIVATE
    -Wno-deprecated-declarations /clang:-fexperimental-new-constant-interpreter)
set_target_properties(maicord_sys_conformance PROPERTIES MAICORD_SDK_INCLUDES ON)
# Ninja starts ready edges by the length of the chain behind them. A
# prerequisite of the library everything else links, the 5 s conformance unit
# (the longest compile of the client) starts first instead of last. The
# client's link waits for it through maicord_core; the test twins don't.
add_dependencies(maicord_core maicord_sys_conformance)

maicord_library(maicord_cdn_media src/app/media/cdn_media_pipeline.cpp
    LIBS maicord_core maicord_media_core maicord_media_codec maicord_protocol)

# maicord_import_library(<target> <def>): an import library built from a .def
# under src/platform, so MaiCord.exe imports those functions by name from the
# DLL that implements them instead of a forwarding or API-set DLL.
# combase: the toast path's Windows Runtime activation and string calls, rather
# than runtimeobject.lib's api-ms-win-core-winrt API sets. sspicli: the TLS
# stream's Schannel calls, which secur32.dll only forwards to SspiCli.dll.
function(maicord_import_library target def)
    set(library "${MAICORD_GENERATED_DIRECTORY}/${target}.lib")
    add_custom_command(OUTPUT "${library}"
        COMMAND "${CMAKE_AR}" /NOLOGO /MACHINE:X64 "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/platform/${def}"
            "/OUT:${library}"
        DEPENDS "src/platform/${def}"
        VERBATIM)
    add_custom_target(${target}_import_library DEPENDS "${library}")
    add_library(${target} INTERFACE)
    target_link_libraries(${target} INTERFACE "${library}")
    add_dependencies(${target} ${target}_import_library)
    set(${target}_import_library "${library}" PARENT_SCOPE)
endfunction()
maicord_import_library(maicord_combase desktop/combase.def)
maicord_import_library(maicord_sspicli net/sspicli.def)

# window_post (include/maicord/platform/window/window.hpp) for Windows: the audio workers wake the UI thread through it.
maicord_library(maicord_window_post src/platform/window/win32/post.cpp
    LIBS maicord_core user32)

# The Windows window backend (include/maicord/platform/window): key state and the window service. The null backend
# for headless tests is maicord_window_null.
maicord_library(maicord_window_win32 src/platform/window/win32/keys.cpp src/platform/window/win32/window.cpp
    src/platform/window/win32/translate.cpp src/platform/window/win32/window_proc.cpp
    src/platform/window/win32/icon.cpp src/platform/window/win32/create_icon.cpp src/platform/window/win32/settings.cpp
    src/platform/window/win32/drop.cpp src/platform/window/win32/main_window.cpp src/platform/window/win32/pump.cpp
    src/platform/window/win32/caption_frame.cpp
    LIBS maicord_core maicord_window_post user32 advapi32 shell32)

# What the test hooks (include/maicord/core/test_hook.hpp) of a client built with tools/build.ps1 -TestHooks add; a
# shipping build compiles none of it.
set(maicord_test_hook_net_sources)
if(MAICORD_CLIENT_TEST_HOOKS)
    set(maicord_test_hook_net_sources src/platform/net/test_server.cpp)
endif()

maicord_library(maicord_platform
    src/platform/desktop/staged_file.cpp src/platform/desktop/browser_sign_in_launcher.cpp src/platform/decode/aac_decoder.cpp
    src/platform/audio/media_audio.cpp src/platform/audio/media_audio_ring.cpp src/platform/crypto/aead.cpp
    src/platform/crypto/aes_gcm.cpp src/platform/crypto/aes_gcm64.cpp
    src/platform/crypto/hkdf_sha256.cpp src/platform/crypto/p256.cpp src/platform/crypto/xchacha20_poly1305.cpp
    src/platform/net/network.cpp src/platform/net/datagram.cpp src/platform/net/file.cpp src/platform/net/socket.cpp
    src/platform/desktop/file_picker.cpp src/platform/desktop/clipboard_attachment.cpp src/platform/desktop/clipboard_image.cpp
    src/platform/desktop/file_identity.cpp src/platform/net/worker.cpp src/platform/crypto/random.cpp
    src/platform/desktop/timezone.cpp src/platform/crypto/remote_auth_crypto.cpp src/platform/crypto/secure_buffer.cpp
    src/platform/net/tls_client.cpp src/platform/net/tls_stream.cpp src/platform/desktop/notifications.cpp
    src/platform/desktop/notification_identity.cpp src/platform/desktop/taskbar_attention.cpp
    src/platform/desktop/screen_color.cpp
    src/platform/desktop/integration.cpp src/platform/desktop/system_tray.cpp src/platform/desktop/spell_checker.cpp
    src/platform/audio/speech.cpp src/platform/audio/voice_audio_output.cpp src/platform/audio/voice_audio_capture.cpp
    # Used by the application core and client preferences, which both link this
    # library: one compile for every image.
    src/app/net/file_persistence.cpp
    ${maicord_test_hook_net_sources}
    LIBS maicord_core maicord_window_win32 ${maicord_tls_libs} ole32 user32 gdi32 shell32 mfuuid ntdll maicord_combase advapi32)

maicord_library(maicord_notification_audio src/platform/audio/notification_audio.cpp
    src/platform/audio/notification_audio_mixer.cpp
    LIBS maicord_core maicord_window_post kernel32 ole32 user32)
maicord_library(maicord_notification_sound_assets src/app/notifications/sound_assets.cpp
    LIBS maicord_notification_pcm maicord_notification_audio kernel32)

maicord_library(maicord_account_paths
    src/app/account/account_paths.cpp src/app/account/account_inventory.cpp src/app/account/account_switch.cpp
    src/app/account/account_reconnect.cpp
    LIBS maicord_core kernel32)
maicord_library(maicord_account_host src/app/account/account_logout_stop.cpp src/app/account/account_host.cpp src/app/account/account_host_load.cpp
    LIBS kernel32 maicord_account_module)

maicord_library(maicord_client_preferences
    src/app/settings/client_theme_document.cpp src/app/settings/client_theme_storage.cpp src/app/settings/client_preferences.cpp
    src/app/settings/client_preferences_writer.cpp src/app/voice/preferences.cpp
    LIBS maicord_core maicord_ui maicord_platform kernel32)

# Theme and effect packages: the .mcfx container, the link grammar and the install files (no UI, no catalog).
maicord_library(maicord_packages
    src/app/packages/mcfx.cpp src/app/packages/link.cpp src/app/packages/install.cpp
    LIBS maicord_core maicord_app_core kernel32)

maicord_library(maicord_webauthn src/platform/desktop/webauthn.cpp LIBS maicord_core maicord_platform maicord_protocol)

maicord_library(maicord_app_core
    src/app/chat/polls/controller.cpp src/app/chat/polls/draft.cpp src/app/gateway/state_dispatch.cpp
    src/app/gateway/state_dispatch_objects.cpp src/app/gateway/state_dispatch_records.cpp
    src/app/gateway/payload_writer.cpp src/app/gateway/user_payload.cpp src/app/account/login/browser_login.cpp
    src/app/account/login/browser_sign_in_receiver.cpp src/app/account/login/captcha_bridge.cpp src/app/account/login/loopback_server.cpp
    src/app/account/login/continuations.cpp src/app/shell/input_layers.cpp src/app/shell/navigation_shortcuts.cpp
    src/app/shell/shortcut_registry.cpp src/app/account/account_profile.cpp src/app/account/login/account_credential_forget.cpp
    src/app/account/account_logout.cpp src/app/account/account_profile_promotion.cpp src/app/account/account_session_store.cpp
    src/app/account/login/captcha_challenge.cpp src/app/net/cdn_client.cpp src/app/media/progressive_video_fetch.cpp
    src/app/net/https_file_upload.cpp src/app/net/https_file_download.cpp src/app/net/rest_client.cpp
    src/app/net/rest_client_transaction.cpp src/app/net/rest_client_worker.cpp src/app/net/signed_out_session.cpp
    src/app/gateway/client.cpp src/app/gateway/client_storage.cpp src/app/gateway/client_transport.cpp
    src/app/gateway/client_outputs.cpp src/app/gateway/client_session.cpp src/app/voice/connection_setup.cpp
    src/app/voice/connection.cpp src/app/voice/udp_discovery.cpp src/app/voice/media_setup.cpp
    src/app/voice/packet_crypto.cpp src/app/voice/dave/frame_crypto.cpp src/app/voice/dave/key_derivation.cpp
    src/app/voice/mls/exporter.cpp src/app/voice/mls/key_schedule.cpp src/app/voice/mls/signature.cpp src/app/voice/mls/hpke_derivation.cpp
    src/app/voice/mls/hpke_context.cpp src/app/voice/mls/hpke.cpp src/app/voice/mls/key_package.cpp src/app/voice/dave/key_package.cpp
    src/app/voice/dave/welcome.cpp src/app/voice/dave/receive_epoch.cpp src/app/voice/dave/receive_sender.cpp
    src/app/voice/udp_media.cpp src/app/voice/participants.cpp src/app/voice/socket.cpp
    src/app/gateway/voice_control.cpp src/app/gateway/voice_dispatch.cpp src/app/gateway/stage_dispatch.cpp
    src/app/gateway/scheduled_event_dispatch.cpp src/app/gateway/stream_control.cpp src/app/gateway/dispatch.cpp
    src/app/gateway/dispatch_notifications.cpp
    src/app/notifications/gateway_notification_records.cpp src/app/gateway/dispatch_payload.cpp
    src/app/gateway/member_list_dispatch.cpp src/app/gateway/guild_admin_dispatch.cpp
    src/app/gateway/thread_member_dispatch.cpp src/app/gateway/presence_dispatch.cpp
    src/app/gateway/private_dispatch.cpp src/app/gateway/relationship_dispatch.cpp
    src/app/gateway/ready_projection.cpp src/app/gateway/guild_folders.cpp src/app/gateway/user_settings.cpp
    src/app/gateway/read_state_dispatch.cpp src/app/account/account_relationships.cpp src/app/chat/messages/account_message_ack.cpp
    src/app/chat/messages/account_bulk_message_ack.cpp src/app/media/account_gif_favorites.cpp src/app/account/login/account_action_captcha.cpp
    src/app/guild/channel_overview_controller.cpp src/app/chat/messages/pinned_message_controller.cpp
    src/app/shell/quick_switcher_controller.cpp src/app/chat/messages/forward_controller.cpp
    src/app/chat/messages/reaction_controller.cpp src/app/guild/account_channel_overview.cpp src/app/guild/account_guild_admin.cpp
    src/app/voice/account_stage.cpp src/app/account/account_user_social.cpp src/app/guild/account_guild_community.cpp
    src/app/chat/messages/account_pinned_messages.cpp src/app/chat/threads/account_thread_actions.cpp src/app/chat/threads/account_thread_browser.cpp
    src/app/chat/threads/account_thread_creation.cpp src/app/chat/polls/account_poll_create.cpp
    src/app/notifications/account_notification_settings.cpp src/app/chat/account_private_channel.cpp src/app/chat/threads/account_forum_post.cpp
    src/app/media/received_attachment_open.cpp src/app/gateway/state_payload.cpp src/app/account/login/mfa_policy.cpp
    src/app/guild/member_list_channel_binding.cpp src/app/account/login/remote_auth_client.cpp src/app/account/login/remote_auth_handshake.cpp
    src/app/account/login/remote_auth_ticket_exchange.cpp src/app/account/user_client_configuration.cpp
    src/app/account/user_account_coordinator.cpp src/app/account/login/user_auth_coordinator.cpp
    src/app/account/login/user_auth_coordinator_responses.cpp src/app/update/manifest.cpp
    src/app/update/artifact.cpp src/app/update/acceptance.cpp
    src/app/update/prepare.cpp src/app/update/package.cpp
    src/app/update/fetch.cpp src/app/update/https.cpp src/app/update/service.cpp src/app/chat/search/recent.cpp
    src/app/chat/channel_memory.cpp
    src/app/chat/link_trust.cpp src/app/voice/dave/route.cpp src/app/voice/jitter_buffer.cpp src/app/voice/mixer.cpp
    src/app/voice/playback.cpp src/app/voice/receive.cpp src/app/gateway/stream_session.cpp
    src/app/voice/video_rtp_receive.cpp src/app/voice/video_sink.cpp
    # Voice send: capture, gating, encoding and RTP. Executables pull the encoder
    # only when they reference the send path (the account module and voice tests).
    src/app/voice/transmit.cpp src/app/voice/send.cpp src/app/voice/soundboard_audio.cpp src/app/voice/account_soundboard.cpp
    src/app/voice/message_flow.cpp src/app/voice/message_recorder.cpp src/app/voice/account_voice_message.cpp
    src/app/voice/call_video_presenter.cpp src/app/voice/stream_preview_cache.cpp src/app/voice/stream_watch_view.cpp
    # Go Live sending: the session, RTP/RTCP sender and bandwidth estimation.
    src/app/gateway/go_live_stream.cpp src/app/voice/go_live_send.cpp
    # acceptance_record.cpp: durable floor records shared by update_acceptance and the plugin catalog.
    src/app/account/login/acceptance_record.cpp src/app/gateway/guild_details.cpp src/app/guild/account_guild_moderation.cpp
    src/app/guild/account_guild_automation.cpp
    src/app/update/changelog.cpp
    src/app/media/embed_resolver.cpp
    src/app/media/segmented_video_fetch.cpp
    src/app/account/login/oauth_authorization.cpp
    src/app/account/login/oauth_sign_in.cpp
    src/app/account/login/oauth_account.cpp
    src/app/account/client_session_metadata.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_media_codec maicord_media_core
        maicord_voice_encoder)

maicord_library(maicord_account_media
    src/app/media/local_preview_thumbnail.cpp src/app/media/local_video_poster.cpp src/app/media/account_media.cpp
    src/app/media/account_media_fetch.cpp src/app/media/account_media_requests.cpp src/app/media/account_media_local_preview.cpp
    src/app/media/regional_flag_assets.cpp src/app/media/account_media_worker.cpp src/app/media/account_media_worker_lanes.cpp
    src/app/media/account_media_video.cpp src/app/media/account_video_session.cpp src/app/media/account_video_session_audio.cpp
    src/platform/decode/mp4_decoder.cpp src/platform/decode/h264_gpu_decoder.cpp
    src/platform/decode/hevc_gpu_decoder.cpp src/platform/decode/h264_gpu_configuration.cpp
    src/platform/decode/h264_gpu_stream_decoder.cpp src/app/voice/video_stream_decoders.cpp
    src/platform/decode/gpu_decoder_surfaces.cpp src/platform/decode/gpu_stream_decoder.cpp src/platform/decode/hevc_gpu_stream_decoder.cpp
    src/platform/decode/av1_gpu_stream.cpp src/platform/decode/av1_dxva_parameters.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_media_core maicord_media_codec
        maicord_cdn_media maicord_paint_program d3d11 dxguid maicord_compression
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}"
    DEPENDS maicord_regional_flag_data)

maicord_library(maicord_text
    src/ui/text/text.cpp src/ui/text/native_text_candidate.cpp src/ui/text/native_text_catalog.cpp src/core/log.cpp
    LIBS maicord_core maicord_font maicord_ui advapi32 gdi32)
# The native layout adapter is compiled for size by a minsize attribute region
# in its source, which leaves retained-layout publication and the single-line
# entry optimized for speed.

# UI Automation server over accessibility snapshots. UIAutomationCore and
# OleAut32 load on the first WM_GETOBJECT, so neither is a static import.
maicord_library(maicord_uia src/platform/uia/provider.cpp src/platform/uia/provider_patterns.cpp
    src/platform/uia/provider_text.cpp LIBS maicord_core maicord_ui)

# The native plugin runtime: the pinned-directory loader with its PE checks, per-plugin bindings
# (state, settings, persisted store, call ledger), the mc_api host table and the hook dispatch lists.
maicord_library(maicord_plugin_runtime
    src/app/plugins/call_ledger.cpp src/app/plugins/loader.cpp src/app/plugins/binding.cpp src/app/plugins/api.cpp
    src/app/plugins/store.cpp
    src/app/plugins/dispatch.cpp src/app/plugins/call_watchdog.cpp src/app/plugins/http.cpp src/app/plugins/secrets.cpp
    src/app/plugins/consent.cpp src/app/plugins/settings_page.cpp
    src/app/plugins/ui_registry.cpp src/app/plugins/ui_menu.cpp src/app/plugins/ui_command.cpp
    src/app/plugins/ui_feedback.cpp src/app/plugins/keep.cpp src/app/plugins/pipeline.cpp
    src/app/plugins/pipeline_api.cpp
    src/app/plugins/decorations.cpp src/app/plugins/buttons.cpp src/app/plugins/reads.cpp src/app/plugins/icons.cpp
    LIBS maicord_core maicord_protocol maicord_platform maicord_app_core maicord_ui kernel32 bcrypt)
# The official catalog's pure parts: the format-2 index verifier and the catalog-state.bin record.
maicord_library(maicord_plugin_catalog
    src/app/plugins/catalog_index.cpp src/app/plugins/catalog_state.cpp
    LIBS maicord_core maicord_platform maicord_app_core maicord_plugin_runtime kernel32 bcrypt)

maicord_library(maicord_ui_input
    src/platform/window/win32/clipboard.cpp src/ui/text/secure_edit.cpp src/ui/text/secure_edit_target.cpp src/ui/text/secure_edit_history.cpp
    src/ui/text/secure_text_store.cpp
    LIBS maicord_core maicord_platform maicord_edit_history user32)

maicord_library(maicord_message_runtime
    src/app/chat/messages/edit_runtime.cpp src/app/chat/composer/message_composer_controller.cpp src/app/chat/composer/command_autocomplete.cpp
    src/app/chat/messages/send_response.cpp src/app/chat/composer/message_attachment_storage.cpp
    src/app/chat/composer/message_attachment_drafts.cpp src/app/chat/messages/reaction_viewer_controller.cpp
    src/app/chat/messages/reaction_viewer_storage.cpp src/app/chat/composer/message_draft_persistence.cpp src/app/chat/composer/message_draft_session.cpp
    src/app/gateway/chat_state_dispatch.cpp src/app/chat/private_conversation.cpp src/app/chat/private_channel_actions.cpp
    src/app/gateway/user_store_dispatch.cpp src/app/chat/messages/event_pump.cpp src/app/chat/messages/event_pump_observers.cpp
    src/app/chat/read_ack_controller.cpp
    src/app/chat/read_scope.cpp src/app/chat/bulk_read_ack_controller.cpp src/app/chat/messages/history_sync.cpp
    src/app/media/url_resolver.cpp src/app/chat/messages/send_tracker.cpp src/app/chat/messages/timeline.cpp
    src/app/chat/messages/view_runtime.cpp src/app/chat/messages/snapshot_cache.cpp src/model/chat/message_reconcile.cpp
    src/app/chat/messages/interaction_rules.cpp src/app/shell/startup_shell.cpp src/app/shell/startup_shell_state.cpp
    LIBS maicord_core maicord_model maicord_protocol maicord_app_core maicord_platform maicord_ui kernel32)

maicord_library(maicord_message_media
    src/app/media/animation_visual_runtime.cpp src/app/media/message_media_controller.cpp
    LIBS maicord_core maicord_model maicord_ui maicord_message_runtime maicord_media_core maicord_render_core)

# The Direct3D 11 backend of include/maicord/gpu/api.hpp. deflate.cpp checks gzip members with png_crc32 from
# maicord_media_codec.
maicord_library(maicord_gpu_d3d11
    src/gpu/d3d11/shader_pack.cpp src/gpu/d3d11/device.cpp src/gpu/d3d11/surface.cpp src/gpu/d3d11/paged_texture.cpp
    "${MAICORD_SHADER_PACK_INCLUDE}"
    LIBS maicord_core maicord_compression maicord_media_codec d3d11
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}")
