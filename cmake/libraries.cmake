# Shipping static libraries linked into MaiCord.exe.
# Each source list is the archive's member order (and so the link order);
# keep it stable when adding files. Headers are found through the include
# paths and are not listed.

# The TLS stream's imports (Winsock, Schannel through sspicli, bcrypt, crypt32).
set(maicord_tls_libs ws2_32 maicord_sspicli bcrypt crypt32)

maicord_library(maicord_core
    src/core/arena.cpp src/core/byte_queue.cpp src/core/byte_order.cpp src/core/data_path.cpp
    src/core/json_reader.cpp src/core/json_scan.cpp src/core/reserved_bytes.cpp src/core/stable_storage.cpp
    src/core/utf.cpp src/core/byte_range.cpp src/core/delay_load.cpp)
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
    include/maicord/sys/webauthn.hpp include/maicord/sys/process_info.hpp
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

maicord_library(maicord_compression src/media/compression/deflate.cpp src/media/compression/zstd.cpp
    src/media/compression/inflated_block.cpp LIBS maicord_core)

maicord_library(maicord_media_core
    src/media/core/animation_timeline.cpp src/media/core/encoded_cache.cpp src/media/core/presentation_policy.cpp
    src/media/image/regional_flag_svg.cpp src/ui/text/regional_flag.cpp
    LIBS maicord_core)

set(MAIOPUS_DECODER_TARGET maicord_opus_decoder)
set(MAIOPUS_ENCODER_TARGET maicord_opus_encoder)
set(MAIOPUS_LINK_CONTRACT maicord_link_contract)
set(MAIOPUS_LTCG ON)
set(MAIOPUS_LTCG_OPTION ${MAICORD_WHOLE_PROGRAM_OPTION})
set(MAIOPUS_ARCH /arch:AVX)
set(MAIOPUS_SSE4_1 ON)
add_subdirectory(third_party/opus)
# maiopus, like every MaiCord unit, sees only MaiCord's C library and intrinsic
# headers and follows the unwind-table and no-outline policies (see
# cmake/build_contract.cmake).
foreach(maiopus_target IN ITEMS maicord_opus_decoder maicord_opus_encoder)
    target_include_directories(${maiopus_target} PRIVATE include include/maicord/sys/libc)
    target_compile_options(${maiopus_target} PRIVATE /clang:-nostdinc ${MAICORD_UNWIND_OPTIONS}
        ${MAICORD_NO_OUTLINE_OPTION})
endforeach()

maicord_library(maicord_media_codec
    src/media/audio/opus_decoder.cpp src/media/containers/ogg_opus.cpp src/media/containers/ogg_opus_writer.cpp src/media/audio/wave_pcm.cpp
    src/media/audio/mp3_stream.cpp src/media/image/apng.cpp src/media/image/gif.cpp src/media/video/h264_parameters.cpp
    src/media/video/h264_configuration.cpp src/media/video/h264_slice.cpp src/media/video/h264_access_unit.cpp
    src/media/video/h264_output_queue.cpp src/media/video/h264_picture_order.cpp src/media/video/h264_references.cpp
    src/media/video/h264_stream.cpp src/media/video/hevc_parameters.cpp src/media/video/hevc_picture.cpp
    src/media/containers/hevc_mp4_demux.cpp src/media/video/hevc_references.cpp src/media/image/jpeg.cpp
    src/media/image/jpeg_icc.cpp src/media/image/jpeg_entropy.cpp src/media/image/jpeg_reconstruct.cpp
    src/media/image/lossy_webp.cpp
    src/media/containers/mp4_demux.cpp src/media/containers/mp4_audio_demux.cpp src/media/core/progressive_source.cpp src/media/image/png.cpp
    src/media/image/png_stream.cpp src/media/image/png_encode.cpp src/media/image/png_common.cpp src/media/image/vp8.cpp
    src/media/image/webp.cpp src/media/image/webp_thumbnail.cpp src/media/image/webp_alpha.cpp src/media/image/animated_webp.cpp
    src/media/video/rtp_h264.cpp src/media/video/rtp_h265.cpp src/media/video/hevc_stream.cpp
    src/media/video/rtp_av1.cpp src/media/video/av1.cpp src/media/containers/mp4_fragment.cpp src/media/containers/mp4_fragment_track.cpp
    src/media/containers/mp4_fragment_source.cpp
    src/media/containers/mp4_fragment_audio.cpp
    src/media/containers/mp4_fragment_segment.cpp
    src/media/containers/mp4_segment_store.cpp
    src/media/containers/mpeg_ts.cpp
    src/media/containers/mpeg_ts_codec.cpp
    src/media/containers/mpeg_ts_segment_store.cpp
    src/media/audio/mp3_decoder.c
    LIBS maicord_core maicord_compression maicord_opus_decoder ntdll)
# The MPEG audio decoder is C: the contract's C++ code generation and warnings,
# for C (LTO comes from the contract).
target_compile_options(maicord_media_codec PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:/W4;/WX;/Zl;/GS-;/O2;/Ob1;/arch:AVX;/fp:precise;/utf-8;/Gy;/Gw;/GF;/Brepro;-Wno-missing-braces>")

# Voice send encoder; linked only through the account module.
maicord_library(maicord_voice_encoder src/media/audio/opus_encoder.cpp LIBS maicord_core maicord_opus_encoder)

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
    LIBS maicord_core maicord_window_post user32 advapi32 shell32)
# The headless backend (src/platform/window/null) behind the same service, for tests: list it before maicord_platform in
# LIBS so its definitions win over maicord_window_win32's, which an archive member only provides when still undefined.
maicord_library(maicord_window_null src/platform/window/null/window.cpp
    LIBS maicord_core)

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
    src/platform/desktop/integration.cpp src/platform/desktop/system_tray.cpp src/platform/desktop/spell_checker.cpp
    src/platform/audio/speech.cpp src/platform/audio/voice_audio_output.cpp src/platform/audio/voice_audio_capture.cpp
    src/platform/desktop/window_frame.cpp
    # Used by the application core and client preferences, which both link this
    # library: one compile for every image.
    src/app/net/file_persistence.cpp
    LIBS maicord_core maicord_window_win32 ${maicord_tls_libs} ole32 user32 shell32 mfuuid ntdll maicord_combase advapi32)

maicord_library(maicord_notification_pcm src/media/audio/notification_pcm.cpp
    LIBS maicord_core maicord_compression maicord_media_codec)
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

maicord_library(maicord_protocol
    src/protocol/messages/poll.cpp src/protocol/gateway/message_poll.cpp src/protocol/auth/browser_sign_in.cpp
    src/protocol/auth/captcha_bridge_page.cpp src/protocol/encoding/base64.cpp src/protocol/encoding/base64_codec.cpp
    src/protocol/encoding/base64url.cpp src/protocol/media/cdn_url.cpp src/protocol/media/attachment_refresh.cpp
    src/protocol/media/attachment_upload.cpp src/protocol/media/upload_allowance.cpp src/protocol/http/https_upload.cpp
    src/protocol/media/cdn_http.cpp src/protocol/http/rest.cpp src/protocol/encoding/json_ids.cpp
    src/protocol/http/rest_route.cpp src/protocol/gateway/commands.cpp src/protocol/gateway/endpoint.cpp
    src/protocol/gateway/events.cpp src/protocol/auth/account_requirement.cpp src/protocol/gateway/voice.cpp
    src/protocol/gateway/stream.cpp src/protocol/guild/stage_channel.cpp src/protocol/user/social.cpp
    src/protocol/guild/community.cpp src/protocol/voice/gateway.cpp src/protocol/voice/discovery.cpp
    src/protocol/voice/media_packet.cpp src/protocol/voice/media_events.cpp src/protocol/voice/participants.cpp
    src/protocol/voice/dave_replay.cpp src/protocol/voice/dave_frame.cpp src/protocol/auth/p256_signature.cpp
    src/protocol/voice/mls_objects.cpp src/protocol/voice/mls_objects_write.cpp src/protocol/voice/heartbeat.cpp
    src/protocol/voice/session.cpp src/protocol/http/websocket_message.cpp src/protocol/gateway/lifecycle.cpp
    src/protocol/gateway/message_events.cpp src/protocol/gateway/message_validation.cpp
    src/protocol/gateway/reaction_events.cpp src/protocol/gateway/typing_events.cpp
    src/protocol/gateway/message_rich.cpp src/protocol/gateway/message_rich_reference.cpp
    src/protocol/gateway/message_rich_attachments.cpp src/protocol/gateway/message_rich_embeds.cpp
    src/protocol/messages/interaction.cpp src/protocol/gateway/payload.cpp
    src/protocol/gateway/runtime.cpp src/protocol/gateway/session.cpp src/protocol/http/http1.cpp
    src/protocol/http/http_response_stream.cpp src/protocol/media/attachment_download.cpp src/protocol/encoding/json_write.cpp
    src/protocol/messages/history.cpp src/protocol/messages/ack.cpp src/protocol/messages/bulk_message_ack.cpp
    src/protocol/messages/create.cpp src/protocol/messages/delete.cpp src/protocol/user/private_conversation.cpp
    src/protocol/messages/edit.cpp src/protocol/messages/reaction.cpp src/protocol/user/profile_decode.cpp
    src/protocol/gateway/activity_decode.cpp src/protocol/messages/reaction_users.cpp src/protocol/messages/typing.cpp
    src/protocol/auth/remote_auth.cpp src/protocol/auth/user_auth.cpp src/protocol/auth/webauthn_mfa.cpp
    src/protocol/http/websocket_frame.cpp src/protocol/http/websocket_handshake.cpp src/protocol/messages/application_commands.cpp
    src/protocol/encoding/writer.cpp src/protocol/auth/login_continuations.cpp src/protocol/user/relationships.cpp
    src/protocol/messages/search.cpp src/protocol/messages/pins.cpp src/protocol/messages/thread_actions.cpp
    src/protocol/messages/thread_browser.cpp src/protocol/messages/thread_creation.cpp src/protocol/messages/forum_post.cpp
    src/protocol/media/media_catalog.cpp src/protocol/gateway/presence.cpp src/protocol/gateway/client_status.cpp
    src/protocol/user/self_status_settings.cpp src/protocol/guild/folders.cpp src/protocol/guild/invite.cpp
    src/protocol/user/settings_proto.cpp src/protocol/user/account_settings.cpp src/protocol/user/emoji_preferences.cpp
    src/protocol/user/gif_favorites.cpp src/protocol/guild/channel_overview.cpp src/protocol/guild/admin.cpp
    src/protocol/guild/admin_decode.cpp
    src/protocol/guild/assets.cpp src/protocol/user/profile_edit.cpp src/protocol/voice/video_signaling.cpp
    src/protocol/voice/rtcp_feedback.cpp src/protocol/guild/soundboard.cpp src/protocol/user/notification_settings.cpp
    src/protocol/user/guild_settings.cpp
    # Icon hash check kept apart from cdn_url.cpp: the client keeps it (Gateway
    # user records) while it forwards every other CDN URL function to the module.
    src/protocol/media/cdn_icon_hash.cpp src/protocol/guild/moderation.cpp src/protocol/voice/channel_info.cpp
    src/protocol/media/embed_provider.cpp
    src/protocol/media/embed_player.cpp
    src/protocol/media/embed_player_video.cpp
    src/protocol/media/embed_player_audio.cpp
    src/protocol/media/dash_manifest.cpp
    src/protocol/media/steam_player.cpp
    src/protocol/auth/oauth.cpp
    src/protocol/media/hls_playlist.cpp
    src/protocol/media/twitch_playback.cpp
    src/protocol/http/cookie_jar.cpp
    src/protocol/gateway/qos_token.cpp
    src/protocol/gateway/etf.cpp
    src/protocol/messages/interaction_data.cpp
    LIBS maicord_core maicord_compression maicord_media_core)

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
    src/app/net/rest_client_transaction.cpp src/app/net/rest_client_worker.cpp
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
    # acceptance_record.cpp: durable floor records shared by update_acceptance and the plugin catalog.
    src/app/account/login/acceptance_record.cpp src/app/gateway/guild_details.cpp src/app/guild/account_guild_moderation.cpp
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

maicord_library(maicord_unicode
    src/text/scripts/arabic_joining.cpp src/text/unicode/line_break_tailored.cpp
    src/text/unicode/thai_word_break.cpp src/text/unicode/thai_lexical.cpp src/text/unicode/thai_syllable.cpp
    src/text/unicode/bidi_tailored.cpp src/text/unicode/unicode.cpp src/text/unicode/grapheme.cpp src/text/unicode/bidi.cpp
    # unicode.cpp and thai_lexical.cpp inflate their tables; deflate.cpp checks gzip members with png_crc32.
    LIBS maicord_core maicord_compression maicord_media_codec)

maicord_library(maicord_font
    src/text/scripts/arabic_shape_run.cpp src/text/scripts/arabic_marks.cpp src/text/scripts/arabic_plan.cpp
    src/text/scripts/devanagari_plan.cpp src/text/scripts/use_plan.cpp src/text/scripts/sinhala_input.cpp src/text/scripts/tibetan_input.cpp
    src/text/shaping/cluster_run.cpp src/text/scripts/tibetan_shape_cluster.cpp src/text/scripts/khmer_input.cpp
    src/text/scripts/myanmar_shape_cluster.cpp src/text/scripts/myanmar_input.cpp src/text/scripts/khmer_shape_cluster.cpp
    src/text/scripts/sinhala_shape_cluster.cpp src/text/scripts/devanagari_forms.cpp
    src/text/scripts/devanagari_clusters.cpp src/text/scripts/indic_input.cpp src/text/scripts/tamil_shape_run.cpp
    src/text/scripts/tamil_run.cpp src/text/shaping/script_paragraphs.cpp
    src/text/scripts/indic_run_plan.cpp src/text/scripts/devanagari_order.cpp
    src/text/scripts/devanagari_shape_cluster.cpp src/text/scripts/indic_run.cpp src/text/shaping/compact_run.cpp src/text/scripts/devanagari_run.cpp
    src/text/scripts/devanagari_run_plan.cpp src/text/opentype/tt_orientation.cpp
    src/text/opentype/tt_bold_points.cpp src/text/fonts/session.cpp src/text/fonts/prepare.cpp
    src/text/opentype/gpos_cursive_execute.cpp src/text/opentype/position_anchor.cpp src/text/opentype/gpos_mark_execute.cpp
    src/text/opentype/gpos_ligature_execute.cpp src/text/opentype/gpos_execute.cpp src/text/opentype/gpos_program.cpp
    src/text/opentype/lookup_selection.cpp src/text/opentype/position_flags.cpp
    src/text/opentype/gpos_attachment.cpp src/text/opentype/gpos.cpp src/text/shaping/clusters.cpp src/text/opentype/components.cpp
    src/text/shaping/shape_run.cpp src/text/shaping/run_plan.cpp src/text/fonts/names.cpp src/text/fonts/match.cpp
    src/text/fonts/candidates.cpp src/text/fonts/family.cpp src/text/fonts/catalog.cpp
    src/text/fonts/handle_stat.cpp src/text/fonts/catalog_coverage.cpp src/text/fonts/fallback_range.cpp
    src/text/fonts/fallback_route.cpp src/text/fonts/fallback_map.cpp
    src/text/fonts/run.cpp src/text/scripts/arabic_run_plan.cpp src/text/scripts/arabic_run.cpp
    src/text/scripts/arabic_paragraph.cpp src/text/opentype/gsub_execute.cpp src/text/opentype/glyph_buffer.cpp src/text/opentype/context.cpp
    src/text/opentype/gsub.cpp src/text/opentype/layout_directory.cpp src/text/opentype/lookup_plan.cpp src/text/opentype/nominal_participation.cpp
    src/text/opentype/nominal_sources.cpp src/text/scripts/ascii_run.cpp src/text/scripts/latin_run.cpp
    src/text/scripts/hebrew_shape_run.cpp src/text/shaping/run_profile.cpp src/text/scripts/emoji_run.cpp src/text/scripts/emoji_line.cpp
    src/text/scripts/emoji_paragraph.cpp src/text/layout/visibility.cpp src/text/unicode/script_class.cpp
    src/text/layout/unicode_line_geometry.cpp src/text/scripts/latin_line.cpp src/text/scripts/latin_paragraph.cpp
    src/text/scripts/ascii_line.cpp src/text/scripts/ascii_paragraph.cpp src/text/layout/paragraph_hits.cpp
    src/text/layout/paragraph_ranges.cpp src/text/fonts/metrics.cpp src/text/opentype/layout_variations.cpp src/text/opentype/gdef.cpp
    src/text/opentype/layout_tables.cpp src/text/opentype/sfnt.cpp src/text/opentype/sfnt_cmap.cpp src/text/opentype/truetype_outline.cpp
    src/text/opentype/cff_font.cpp src/text/opentype/cff_charset.cpp src/text/opentype/cff_outline.cpp src/text/opentype/cpal.cpp
    src/text/opentype/colr_font.cpp src/text/opentype/colr_paint_reader.cpp
    src/text/opentype/variation_store.cpp src/text/opentype/metric_variations.cpp src/text/opentype/gvar.cpp src/text/fonts/geometry.cpp
    src/text/fonts/store.cpp src/text/fonts/instance.cpp src/text/opentype/tt_interpreter.cpp src/text/opentype/tt_outline.cpp
    src/text/opentype/colr_variations.cpp src/text/fonts/axes.cpp
    LIBS maicord_core maicord_unicode ntdll)
# Compact the shared Indic ordering routine; keep /O2, /Ob1 and LTO.
set_property(SOURCE src/text/scripts/devanagari_order.cpp APPEND PROPERTY COMPILE_OPTIONS ${MAICORD_SIZE_OPTION})
# Font table opening, outlines and variations (once per font or glyph), the
# non-Latin script shapers and the Unicode line geometry keep their short loops
# rolled: unrolled they only bloat (text benchmark: cycles_ascii -9%, Latin,
# Arabic and Devanagari within 1%). The lookup executors, the Latin, ASCII,
# Arabic and Devanagari shapers and the paragraph layout keep unrolling: without
# it message_hit_rebuild was 3-9% slower and cycles_arabic 2.4%.
get_target_property(maicord_rolled_sources maicord_font SOURCES)
list(FILTER maicord_rolled_sources INCLUDE REGEX
    "/(opentype/(sfnt|cff_|tt_|truetype_outline|colr_|cpal|gvar|variation_store|metric_variations|layout_directory|layout_tables|gdef|layout_variations|lookup_selection|nominal_)|scripts/(hebrew|sinhala|tibetan|khmer|myanmar|tamil)|layout/unicode_line_geometry)")
set_property(SOURCE ${maicord_rolled_sources} APPEND PROPERTY COMPILE_OPTIONS /clang:-fno-unroll-loops)

maicord_library(maicord_text
    src/ui/text/text.cpp src/ui/text/native_text_candidate.cpp src/ui/text/native_text_catalog.cpp src/core/log.cpp
    LIBS maicord_core maicord_font maicord_ui advapi32 gdi32)
# The native layout adapter is compiled for size by a minsize attribute region
# in its source, which leaves retained-layout publication and the single-line
# entry optimized for speed.

maicord_library(maicord_render_core
    src/render/media/animation_frame_cache.cpp src/render/text/glyph_atlas.cpp src/render/text/glyph_quad_bounds.cpp
    src/render/text/glyph_raster.cpp src/render/media/atlas.cpp src/render/d3d11/presentation_damage.cpp
    LIBS maicord_core)

maicord_library(maicord_paint_program
    src/render/effects/colr_program.cpp src/render/effects/colr_source.cpp src/render/effects/regional_flag_program.cpp
    src/render/effects/lottie_program.cpp
    LIBS d2d1 maicord_compile_contract maicord_core maicord_render_core maicord_font maicord_media_core)

# The geometry engine the Lottie compiler runs on without Direct2D (Linux). Nothing shipping on Windows links it; its
# tests compare it with Direct2D.
maicord_library(maicord_vector_geometry
    src/render/effects/vector_geometry.cpp src/render/effects/vector_overlay.cpp src/render/effects/vector_stroke.cpp
    LIBS maicord_compile_contract maicord_core)

maicord_library(maicord_ui
    src/ui/chat/polls/controls.cpp src/ui/chat/polls/voters.cpp src/ui/chat/polls/presentation.cpp src/ui/voice/audio_controls.cpp
    src/ui/voice/audio_presentation.cpp src/ui/voice/video_controls.cpp src/ui/voice/video_scene.cpp src/ui/widgets/spinner.cpp
    src/ui/core/transition.cpp src/ui/widgets/tooltip.cpp src/ui/media/emoji_presentation.cpp src/ui/text/ansi_text.cpp
    src/ui/text/code_highlight.cpp src/ui/chat/composer/autocomplete.cpp src/ui/chat/messages/markdown.cpp
    src/ui/chat/messages/markdown_links.cpp
    src/ui/text/timestamp_format.cpp src/ui/shell/navigation_view.cpp src/ui/text/emoji_catalog.cpp src/ui/core/geometry.cpp
    src/ui/guild/rail_scene.cpp src/ui/shell/navigation_drag.cpp src/ui/chat/unread_banner_scene.cpp
    src/app/chat/unread_banner_projection.cpp src/ui/layout/layout_engine.cpp src/ui/account/login_scene.cpp
    src/ui/account/login_presentation.cpp src/ui/account/login_text_presentation.cpp src/ui/account/login_view.cpp
    src/ui/guild/member_list_scene.cpp src/ui/guild/member_list_navigation.cpp src/ui/chat/messages/action_bar.cpp
    src/ui/chat/messages/action_bar_presentation.cpp src/ui/chat/messages/context_controller.cpp
    src/ui/chat/messages/context_menu.cpp src/ui/account/user_context_menu.cpp src/ui/chat/unread_context.cpp
    src/ui/chat/messages/rich_layout.cpp src/ui/chat/messages/component_layout.cpp src/ui/chat/messages/component_media.cpp
    src/ui/chat/messages/media_policy.cpp src/ui/chat/messages/system_text.cpp src/ui/chat/messages/rich_widgets.cpp
    src/ui/chat/messages/scene.cpp src/ui/chat/messages/scene_rows.cpp src/ui/chat/messages/scene_navigation.cpp
    src/ui/chat/messages/scene_measurements.cpp
    src/ui/layout/scrollbar.cpp src/ui/chat/composer/message_composer_scene.cpp src/ui/chat/composer/attachment_strip.cpp
    src/ui/chat/composer/voice_bar.cpp src/ui/chat/messages/presentation.cpp src/ui/media/image_viewer.cpp
    src/ui/media/zoom_lens.cpp
    src/ui/chat/messages/reaction_presentation.cpp src/ui/chat/messages/typing_presentation.cpp src/ui/core/paint.cpp
    src/ui/widgets/icon.cpp src/ui/widgets/button.cpp src/ui/chat/messages/text_coordinator.cpp src/ui/chat/messages/text_presentation.cpp
    src/ui/chat/messages/text_presentation_reply.cpp src/ui/chat/messages/text_presentation_content.cpp
    src/ui/chat/messages/text_presentation_cards.cpp src/ui/chat/messages/text_presentation_row.cpp
    src/ui/shell/popup_scene.cpp src/ui/guild/channel_context_policy.cpp src/ui/guild/context_policy.cpp
    src/ui/guild/member_context_policy.cpp src/ui/chat/composer/attachment_menu.cpp src/ui/chat/messages/received_attachment_menu.cpp
    src/ui/shell/popup_presentation.cpp src/ui/text/edit_menu.cpp src/ui/text/spell_tokenize.cpp
    src/ui/chat/composer/spellcheck.cpp src/ui/text/form.cpp src/ui/text/composition.cpp src/ui/widgets/qr_code.cpp
    src/ui/chat/messages/reaction_picker.cpp src/ui/text/edit_field.cpp src/ui/widgets/badge.cpp src/ui/account/profile_scene.cpp
    src/ui/account/profile_scene_sections.cpp src/ui/account/profile_scene_layout.cpp
    src/ui/account/profile_media.cpp src/ui/account/profile_dialog_scene.cpp src/ui/chat/messages/reaction_viewer.cpp
    src/ui/chat/messages/reaction_viewer_presentation.cpp src/ui/core/scene.cpp src/ui/kit/scene_kit.cpp src/ui/shell/navigation_scene.cpp
    src/ui/shell/presentation.cpp src/ui/shell/text_presentation.cpp src/ui/chat/private_channel_title.cpp
    src/ui/chat/private_channel_rows.cpp src/ui/layout/smooth_scroll.cpp src/ui/layout/scroll_host.cpp src/ui/text/flow_layout.cpp
    src/ui/text/measure_cache.cpp src/ui/text/presentation.cpp src/ui/layout/virtual_list.cpp src/ui/core/widget_manager.cpp
    src/ui/core/widget_builder.cpp src/ui/widgets/menu_row.cpp src/ui/widgets/button_keyboard.cpp src/ui/core/input_router.cpp
    src/ui/layout/list_navigation.cpp src/ui/layout/list_paging.cpp src/ui/layout/widget_scroll.cpp src/ui/core/widget_storage.cpp
    src/ui/shell/page_registry.cpp src/ui/settings/scene.cpp src/ui/settings/scene_layout.cpp
    src/ui/settings/scene_paint.cpp src/ui/settings/theme_editor.cpp src/ui/theme/styles.cpp
    src/ui/theme/style_projection.cpp src/ui/widgets/settings_choice.cpp src/ui/account/friends_scene.cpp
    src/ui/widgets/kit_fields.cpp src/ui/widgets/kit_collections.cpp
    src/ui/widgets/kit_markdown.cpp
    src/ui/store/store_layout.cpp src/ui/store/store_input.cpp src/ui/store/store_paint.cpp src/ui/store/store_access.cpp
    src/ui/account/friends_media.cpp src/ui/controls/settings_tokens.cpp src/ui/controls/switch.cpp
    src/ui/controls/radio.cpp src/ui/controls/slider.cpp src/ui/controls/input.cpp src/ui/controls/swatch.cpp
    src/ui/controls/navigation.cpp src/ui/controls/text_metrics.cpp src/ui/settings/schema.cpp
    src/ui/controls/permission_grid.cpp src/ui/controls/admin_list.cpp src/ui/accessibility/accessibility.cpp
    src/ui/accessibility/scenes.cpp src/ui/widgets/icon_pack.cpp src/ui/account/menu.cpp src/ui/account/switcher.cpp
    src/ui/chat/search/scene.cpp src/ui/chat/search/scene_items.cpp src/ui/chat/search/scene_paint.cpp
    src/ui/chat/pinned_messages_scene.cpp src/ui/chat/inbox_scene.cpp src/ui/chat/threads/header.cpp
    src/ui/chat/threads/browser_scene.cpp src/ui/chat/threads/forum_channel_scene.cpp src/ui/chat/threads/forum_post_scene.cpp
    src/ui/chat/composer/attachment_draft_list.cpp src/ui/shell/quick_switcher_scene.cpp src/ui/chat/messages/forward_scene.cpp
    src/ui/media/download_scene.cpp src/ui/shell/shortcuts_scene.cpp src/ui/media/picker.cpp
    src/ui/media/picker_layout.cpp
    src/ui/media/picker_presentation.cpp src/ui/chat/search/query.cpp src/ui/chat/search/filter_context.cpp
    src/ui/voice/channel_rows.cpp src/ui/voice/panel.cpp
    src/ui/voice/call_scene.cpp src/ui/voice/call_stage.cpp src/ui/widgets/hover_card.cpp src/ui/voice/stream_card_scene.cpp
    src/ui/guild/card_scene.cpp src/ui/guild/folder_open.cpp src/ui/guild/hidden_channel_menu.cpp
    src/ui/guild/banner.cpp src/ui/account/profile_activity.cpp src/ui/shell/caption_bar.cpp src/ui/widgets/activity_status.cpp
    src/ui/chat/messages/typing_indicator.cpp src/ui/account/dm_profile_panel.cpp
    src/ui/widgets/toast.cpp src/ui/theme/theme.cpp src/ui/theme/background_color_math.cpp src/ui/chat/search/message_result_card.cpp
    src/ui/text/pointer_selection.cpp src/ui/chat/messages/author.cpp src/ui/chat/messages/interaction.cpp
    src/ui/media/expression_card_scene.cpp
    # The spell tokenizer classifies letters with the Unicode property tables;
    # deflate.cpp checks gzip members with png_crc32 from maicord_media_codec.
    LIBS maicord_core maicord_model maicord_media_core maicord_unicode maicord_compression maicord_media_codec
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}"
    DEPENDS maicord_emoji_catalog_data maicord_icon_pack_data)

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

maicord_library(maicord_edit_history src/model/chat/edit_history.cpp LIBS maicord_core maicord_platform)

maicord_library(maicord_ui_input
    src/platform/window/win32/clipboard.cpp src/ui/text/secure_edit.cpp src/ui/text/secure_edit_target.cpp src/ui/text/secure_edit_history.cpp
    src/ui/text/secure_text_store.cpp
    LIBS maicord_core maicord_platform maicord_edit_history user32)

maicord_library(maicord_model
    src/model/chat/message_poll_store.cpp src/model/chat/bounded_edit.cpp src/model/guild/channel_permissions.cpp
    src/model/guild/permission_compute.cpp src/model/guild/ban_store.cpp src/model/guild/member_moderation.cpp
    src/model/guild/member_store.cpp src/model/guild/member_list.cpp src/model/guild/thread_member_store.cpp
    src/model/guild/thread_member_list.cpp src/model/chat/chat_state.cpp src/model/chat/read_state.cpp
    src/model/guild/display_order.cpp src/model/guild/folder_edit.cpp src/model/guild/channel_reorder.cpp
    src/model/user/private_channel_store.cpp src/model/chat/custom_emoji.cpp src/model/user/profile.cpp
    src/model/user/activity.cpp src/model/user/store.cpp src/model/user/relationship_store.cpp
    src/model/user/note_store.cpp src/model/user/active_now.cpp src/model/user/friend_search.cpp
    src/model/chat/typing_store.cpp src/model/chat/message_rich_store.cpp src/model/chat/message_rich_store_pack.cpp
    src/model/guild/invite_card_cache.cpp
    src/model/guild/scheduled_event_store.cpp src/model/chat/message_composer.cpp
    src/model/chat/message_composer_input_target.cpp src/model/chat/message_draft_store.cpp
    src/model/chat/reaction_preferences.cpp src/model/chat/message_store.cpp src/model/chat/message_search.cpp
    src/model/notifications/message_notifications.cpp src/model/voice/state_store.cpp src/model/voice/stage_instance_store.cpp
    src/model/voice/stage_policy.cpp src/model/user/image_crop.cpp src/model/user/profile_editor.cpp
    src/app/voice/mls/tree_math.cpp src/app/voice/mls/ratchet_tree.cpp src/app/voice/mls/tree_validate.cpp src/app/voice/mls/tree_edit.cpp
    src/app/voice/mls/treekem.cpp src/app/voice/mls/group_context.cpp src/app/voice/mls/proposal.cpp src/app/voice/mls/commit.cpp
    src/app/voice/mls/welcome.cpp src/app/voice/mls/group.cpp src/app/voice/dave/session.cpp src/app/voice/dave/connection_dave.cpp
    src/app/voice/log.cpp
    src/model/notifications/desktop_notification_queue.cpp src/model/notifications/sound_policy.cpp
    src/model/notifications/sound_admission.cpp src/model/notifications/retention.cpp
    src/model/guild/permission_grid.cpp src/model/guild/settings_model.cpp src/model/guild/channel_settings_model.cpp
    src/model/guild/detail_store.cpp src/model/guild/member_search.cpp src/model/voice/channel_info_store.cpp
    LIBS maicord_core maicord_edit_history maicord_protocol)

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
    src/gpu/d3d11/shader_pack.cpp src/gpu/d3d11/device.cpp src/gpu/d3d11/surface.cpp "${MAICORD_SHADER_PACK_INCLUDE}"
    LIBS maicord_core maicord_compression maicord_media_codec d3d11
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}")

maicord_library(maicord_atlas_gpu
    src/gpu/d3d11/video_frame.cpp "${MAICORD_VIDEO_FRAME_HEADER}" src/render/text/glyph_atlas_gpu.cpp
    src/render/media/atlas_gpu.cpp src/render/media/atlas_gpu_animation.cpp
    src/render/media/atlas_gpu_dispatch.cpp src/render/media/atlas_gpu_operations.cpp
    src/render/media/atlas_gpu_validation.cpp src/render/media/atlas_gpu_vp8.cpp "${MAICORD_VP8_IDCT_HEADER}"
    "${MAICORD_COLR_WIDE_HEADER}" "${MAICORD_COLR_COMPUTE_HEADER}" "${MAICORD_COLR_LAYERS_HEADER}"
    "${MAICORD_GLYPH_COMPUTE_HEADER}" "${MAICORD_GLYPH_CULLED_COMPUTE_HEADER}" "${MAICORD_PNG_COMPUTE_HEADER}" "${MAICORD_VP8_RECONSTRUCT_HEADER}"
    "${MAICORD_VP8_FILTER_HEADER}" "${MAICORD_VP8_ATLAS_HEADER}" "${MAICORD_JPEG_IDCT_HEADER}"
    "${MAICORD_JPEG_ATLAS_HEADER}"
    LIBS maicord_core maicord_render_core maicord_text maicord_gpu_d3d11 d3d11
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}")

maicord_library(maicord_renderer
    src/render/media/animation_frame_cache_gpu.cpp src/render/text/content_text_retention.cpp
    src/render/text/shaped_text_cache.cpp src/render/d3d11/device.cpp src/render/d3d11/draw_pipeline.cpp
    src/render/effects/colr_paint.cpp src/render/d3d11/glyph_text.cpp src/render/media/icon_atlas.cpp
    src/render/text/native_glyph_outlines.cpp src/render/d3d11/background_fx.cpp
    src/render/d3d11/effect_renderer.cpp src/render/d3d11/effect_inputs.cpp src/render/d3d11/effect_glyphs.cpp
    src/render/d3d11/renderer.cpp src/render/d3d11/renderer_draw.cpp src/render/d3d11/renderer_edit.cpp
    src/render/d3d11/renderer_layout.cpp src/render/d3d11/renderer_layout_media.cpp src/render/d3d11/renderer_text.cpp
    src/render/d3d11/renderer_text_content.cpp src/render/d3d11/renderer_text_overlay.cpp
    src/render/d3d11/renderer_interaction.cpp src/render/d3d11/renderer_media.cpp
    src/render/d3d11/navigation.cpp src/render/text/edit_layout.cpp src/render/text/form_surface.cpp
    src/render/text/forum_post_surface.cpp src/render/text/message_hit_test.cpp src/render/d3d11/renderer_presentation.cpp
    src/render/d3d11/layer_presentation.cpp src/render/media/renderer_media_draw_storage.cpp
    src/render/d3d11/renderer_caption.cpp "${MAICORD_UI_VERTEX_HEADER}"
    "${MAICORD_UI_PIXEL_HEADER}" "${MAICORD_GLYPH_VERTEX_HEADER}" "${MAICORD_GLYPH_PIXEL_HEADER}"
    "${MAICORD_MEDIA_VERTEX_HEADER}" "${MAICORD_MEDIA_PIXEL_HEADER}"
    LIBS maicord_media_core maicord_font maicord_paint_program maicord_gpu_d3d11
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}"
    DEPENDS maicord_atlas_gpu)
