# The size policy of both systems' shipping client: which sources are compiled for size (MAICORD_SIZE_OPTION) and which
# of those also fold their helpers (MAICORD_FOLD_OPTION: /Ob2 on Windows, -finline-functions on Linux, against the
# contract's /Ob1 and -finline-hint-functions). cmake/client.cmake (MaiCord.exe) and cmake/linux_client.cmake
# (MaiCord) include it after their own targets exist, so the lists below name Windows files and Linux files side by
# side: a path no target of the build holds is ignored, and a target that does not exist (the Windows libraries on
# Linux, the Linux ones on Windows) is skipped. The Windows build's code generation is the one it had before the
# lists moved here. The Linux client has a third tier at the end of this file: -Os (MAICORD_BALANCED_OPTION) for the
# sources that are still at -O2 and are not byte or sample loops.

# Cold code is compiled for size: source-level -Oz lands after the contract's
# /O2 /Ob1 -flto. Parsing, crypto, codecs, compression and text shaping stay
# /O2; each exclusion below is a hot byte or frame loop.
function(maicord_compact_sources)
    cmake_parse_arguments(ARG "" "" "TARGETS;SOURCES;EXCLUDE" ${ARGN})
    set(files ${ARG_SOURCES})
    foreach(target IN LISTS ARG_TARGETS)
        if(TARGET ${target})
            get_target_property(target_sources ${target} SOURCES)
            list(APPEND files ${target_sources})
        endif()
    endforeach()
    list(FILTER files INCLUDE REGEX "\\.cpp$")
    foreach(pattern IN LISTS ARG_EXCLUDE)
        list(FILTER files EXCLUDE REGEX "${pattern}")
    endforeach()
    list(REMOVE_DUPLICATES files)
    # A source listed twice keeps one size option.
    foreach(file IN LISTS files)
        get_source_file_property(options "${file}" COMPILE_OPTIONS)
        if(NOT options MATCHES "${MAICORD_SIZE_OPTION}")
            set_property(SOURCE "${file}" APPEND PROPERTY COMPILE_OPTIONS ${MAICORD_SIZE_OPTION})
        endif()
    endforeach()
endfunction()
# The client's own Application sources (all of MaiCord.exe's but the entry and
# the icon) and the updater are cold.
file(GLOB MAICORD_COLD_UPDATE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/app/update/*.cpp")
maicord_compact_sources(
    SOURCES ${MAICORD_COLD_UPDATE_SOURCES} src/app/account/login/acceptance_record.cpp
        src/app/media/account_media.cpp src/app/media/account_media_fetch.cpp src/app/media/account_media_requests.cpp
        src/app/media/account_media_local_preview.cpp src/app/media/account_media_worker.cpp
        src/app/media/account_media_worker_lanes.cpp src/app/media/regional_flag_assets.cpp
        src/app/account/game_detection.cpp src/app/net/file_persistence.cpp src/core/log.cpp src/platform/window/win32/clipboard.cpp
        src/platform/desktop/staged_file.cpp src/platform/desktop/browser_sign_in_launcher.cpp
        src/platform/net/file.cpp src/platform/desktop/file_picker.cpp src/platform/desktop/clipboard_attachment.cpp
        src/platform/desktop/file_identity.cpp src/platform/desktop/timezone.cpp src/platform/crypto/secure_buffer.cpp
    TARGETS MaiCord maicord_protocol maicord_model maicord_plugin_catalog
        maicord_plugin_runtime maicord_packages
        maicord_account_host maicord_client_preferences maicord_account_paths maicord_webauthn
    # The user store resolves every member name and presence update: member-list
    # model time is 6% lower with it at /O2, for 2 KiB. The guild member store
    # and member list compile for size through minsize attribute regions in
    # their sources, which leave their member-list update and rebuild paths /O2.
    EXCLUDE
        "/app/shell/(main|app_icon)\\.cpp$"
        "/app/(shell/frame_presentation|shell/input_layers|shell/scheduler|media/video(_sources|_input)?)\\.cpp$"
        "/model/(chat/message_(rich_)?store(_pack)?|user/store|guild/member_store|guild/member_list)\\.cpp$"
        "/protocol/(encoding/json_ids|gateway/etf|gateway/message_events|gateway/message_rich(_reference|_attachments|_embeds)?|gateway/payload|gateway/reaction_events|gateway/typing_events|http/http1|http/http_response_stream|http/websocket_frame|http/websocket_message|messages/history|voice/dave_frame|voice/dave_replay|voice/media_packet)\\.cpp$")
# UI scenes, presentation, renderer and the message runtime: the offline perf
# probe measured equal or lower main-thread CPU in every phase at /Os (smaller
# code and no chained unwind records). The parsers and tokenizers they run per
# message and glyph outlines stay /O2, as do the atlas allocator's lookups. The
# atlas GPU upload and animation files record device commands once per image and
# compile for size through minsize attribute regions in their sources.
# Elsewhere: cold controllers, sign-in/captcha/remote auth, DAVE/MLS group
# setup (not frame crypto), voice setup, notifications and one-shot account
# requests. Gateway dispatch, media decode and text shaping stay /O2.
maicord_compact_sources(
    TARGETS maicord_ui maicord_renderer maicord_message_runtime maicord_message_media
    EXCLUDE "/ui/(chat/messages/markdown(_links)?|text/ansi_text|text/code_highlight|text/measure_cache|text/spell_tokenize)\\.cpp$"
        "/render/(text/native_glyph_outlines|effects/colr_paint)\\.cpp$"
    SOURCES
        src/app/chat/channel_memory.cpp
        src/app/shell/quick_switcher_controller.cpp src/app/chat/messages/forward_controller.cpp
        src/app/chat/messages/pinned_message_controller.cpp src/app/chat/polls/controller.cpp src/app/chat/polls/draft.cpp
        src/app/guild/channel_overview_controller.cpp src/app/chat/search/recent.cpp src/app/chat/link_trust.cpp
        src/app/media/received_attachment_open.cpp src/app/account/login/account_credential_forget.cpp
        src/app/account/login/mfa_policy.cpp src/app/shell/navigation_shortcuts.cpp src/app/account/login/continuations.cpp
        src/app/settings/catalog.cpp src/app/voice/settings_binding.cpp
        src/platform/desktop/notifications.cpp src/platform/desktop/notification_identity.cpp
        src/platform/desktop/taskbar_attention.cpp src/platform/desktop/clipboard_image.cpp
        src/app/account/login/user_auth_coordinator.cpp src/app/account/login/user_auth_coordinator_responses.cpp
        src/app/account/user_account_coordinator.cpp
        src/app/account/user_client_configuration.cpp src/app/account/login/remote_auth_client.cpp
        src/app/account/login/remote_auth_handshake.cpp src/app/account/login/remote_auth_ticket_exchange.cpp
        src/platform/crypto/remote_auth_crypto.cpp src/app/account/login/captcha_bridge.cpp src/app/account/login/captcha_challenge.cpp
        src/app/account/login/account_action_captcha.cpp src/app/account/login/browser_sign_in_receiver.cpp src/app/account/login/loopback_server.cpp src/app/account/login/browser_login.cpp
        src/app/account/account_session_store.cpp src/app/account/account_profile.cpp
        src/app/account/account_profile_promotion.cpp src/app/account/account_logout.cpp
        src/app/guild/account_guild_admin.cpp src/app/chat/threads/account_forum_post.cpp src/app/media/account_gif_favorites.cpp
        src/app/notifications/account_notification_settings.cpp src/app/chat/threads/account_thread_creation.cpp
        src/app/chat/threads/account_thread_actions.cpp src/app/chat/threads/account_thread_browser.cpp
        src/app/account/account_relationships.cpp src/app/chat/polls/account_poll_create.cpp
        src/app/chat/messages/account_pinned_messages.cpp src/app/guild/account_channel_overview.cpp
        src/app/chat/account_private_channel.cpp src/app/chat/search/account_message_search.cpp
        src/app/guild/account_guild_invite.cpp src/app/settings/account_settings_rest.cpp
        src/app/media/account_media_catalog.cpp src/app/chat/polls/account_message_poll.cpp
        src/app/voice/mls/hpke_derivation.cpp src/app/voice/mls/hpke_context.cpp src/app/voice/mls/hpke.cpp
        src/app/voice/mls/key_schedule.cpp src/app/voice/mls/key_package.cpp src/app/voice/mls/signature.cpp
        src/app/voice/mls/exporter.cpp src/app/voice/dave/key_package.cpp src/app/voice/dave/welcome.cpp
        src/app/voice/dave/receive_epoch.cpp src/app/voice/connection_setup.cpp src/app/voice/media_setup.cpp
        src/app/voice/udp_discovery.cpp
        # Rebuilt on change, not per frame: the UI Automation provider,
        # sign-in secure text input, keybind registry, notification sounds,
        # stage/stream/voice signaling and guild assets.
        src/platform/uia/provider.cpp src/platform/uia/provider_patterns.cpp src/platform/uia/provider_text.cpp
        src/ui/text/secure_text_store.cpp src/ui/text/secure_edit.cpp src/ui/text/secure_edit_history.cpp
        src/ui/text/secure_edit_target.cpp src/app/shell/shortcut_registry.cpp
        src/platform/audio/notification_audio.cpp src/platform/audio/notification_audio_mixer.cpp
        src/app/voice/stream_preview_cache.cpp src/app/voice/stream_watch_view.cpp
        src/app/voice/account_stage.cpp src/app/gateway/stage_dispatch.cpp src/app/gateway/stream_control.cpp
        src/app/gateway/stream_session.cpp src/app/gateway/voice_control.cpp
        src/app/guild/account_guild_assets.cpp src/media/image/regional_flag_svg.cpp
        # Opened on a click: the user's own reaction requests; shell/tray registration;
        # the account module's API wrappers (its pump only forwards), the REST
        # request queue, the TLS handshake, soundboard and voice-message
        # requests and network-bound file transfers; H.264/H.265 parameter
        # sets and avcC, parsed once per stream or keyframe (slices and pixel
        # paths stay /O2).
        src/app/chat/messages/reaction_controller.cpp
        src/platform/desktop/integration.cpp src/platform/desktop/system_tray.cpp src/platform/net/tls_client.cpp
        src/app/account/account_module.cpp src/app/account/account_module_login.cpp src/app/account/account_module_requests.cpp
        src/app/net/rest_client.cpp src/app/net/rest_client_transaction.cpp
        src/app/net/rest_client_worker.cpp
        src/app/voice/account_soundboard.cpp src/app/voice/account_voice_message.cpp
        src/app/net/https_file_upload.cpp src/app/net/https_file_download.cpp
        src/media/video/h264_parameters.cpp src/media/video/hevc_parameters.cpp src/media/video/h264_configuration.cpp
        # Once per embed, stream segment, CDN request, voice connection or READY:
        # embed stream resolution, segment and CDN request control, voice
        # connection setup and READY projection (their byte paths stay /O2).
        src/app/media/embed_resolver.cpp src/app/media/segmented_video_fetch.cpp src/app/net/cdn_client.cpp
        src/app/voice/connection.cpp src/app/gateway/ready_projection.cpp
        # The account module's attachment playback sessions and CDN admission, once per packet, command or
        # response around the decoders and demuxers, which stay /O2 (the Linux client compiles the same files
        # -Oz). The offline perf probe replaces the account module, so no probe phase runs them.
        src/app/media/account_media_video.cpp src/app/media/account_video_session.cpp
        src/app/media/account_video_session_audio.cpp src/app/media/cdn_media_pipeline.cpp
        # Run once per font file or face: name/family/property resolution,
        # catalog records, seac charsets and face preparation.
        src/text/fonts/names.cpp src/text/fonts/family.cpp src/text/fonts/candidates.cpp
        src/text/fonts/catalog.cpp src/text/fonts/prepare.cpp src/text/opentype/cff_charset.cpp
        src/ui/text/native_text_catalog.cpp
        # Run once per font instance, script and feature set (the plans are
        # cached): lookup selection and planning, and nominal participation.
        src/text/opentype/lookup_plan.cpp src/text/scripts/use_plan.cpp src/text/scripts/devanagari_plan.cpp src/text/scripts/arabic_plan.cpp
        src/text/scripts/indic_run_plan.cpp src/text/scripts/arabic_run_plan.cpp src/text/opentype/lookup_selection.cpp
        src/text/opentype/layout_variations.cpp src/text/opentype/nominal_participation.cpp src/text/opentype/nominal_sources.cpp
        # Signaling, setup and per-surface control around byte paths that stay
        # /O2: OAuth, member-list channel binding, guild details, voice socket
        # and participants, progressive video requests, spell checking, P-256
        # and HKDF, socket setup, profile and guild image preparation, posters,
        # Lottie and COLR program building, the encoded-media cache and
        # upload-only PNG encoding (perf probe phases equal or lower). Gateway
        # dispatch (3-9% slower) and the video presenter, session and decoder
        # control (call phases ~3% slower) measured worse at /Os and stay /O2.
        src/app/account/login/oauth_sign_in.cpp src/app/guild/member_list_channel_binding.cpp src/app/gateway/guild_details.cpp
        src/app/voice/socket.cpp src/app/voice/participants.cpp src/app/media/progressive_video_fetch.cpp
        src/media/image/png_encode.cpp
        src/platform/desktop/spell_checker.cpp src/platform/crypto/p256.cpp src/platform/crypto/hkdf_sha256.cpp
        src/platform/net/network.cpp src/platform/net/worker.cpp
        # Audio endpoint and decoder surface setup, device enumeration and the
        # worker loops that wait on them (the sample paths live elsewhere).
        src/platform/audio/voice_audio_capture.cpp src/platform/audio/voice_audio_output.cpp src/platform/audio/media_audio.cpp
        src/platform/audio/media_audio_ring.cpp
        src/platform/decode/gpu_decoder_surfaces.cpp src/platform/decode/h264_gpu_configuration.cpp
        src/app/media/guild_image.cpp src/app/media/profile_image.cpp src/app/account/account_profile_edit.cpp
        src/app/media/local_video_poster.cpp
        # One-shot REST request builders, OAuth and session metadata, an MP4 duration
        # probe, permission storage setup and rare Gateway account events.
        src/app/guild/account_guild_community.cpp src/app/guild/account_guild_moderation.cpp src/app/account/account_user_social.cpp
        src/app/chat/messages/account_bulk_message_ack.cpp src/app/chat/messages/account_message_ack.cpp src/app/guild/account_permission_storage.cpp
        src/app/account/login/oauth_authorization.cpp src/app/account/login/oauth_account.cpp src/app/account/client_session_metadata.cpp
        src/app/media/attachment_media_probe.cpp src/app/gateway/guild_folders.cpp src/app/gateway/user_settings.cpp
        src/app/gateway/scheduled_event_dispatch.cpp
        src/render/effects/lottie_program.cpp src/render/effects/colr_source.cpp src/media/core/encoded_cache.cpp)
# Gateway client connection, transport, outputs, storage and session loop, and chat-state
# application: one-shot, per request or per guild event. Their sources are not what the Gateway
# pipeline benchmark measures (it includes client*.cpp into its own unit), and the guild-create apply
# benchmark stays within 1.6% of 2.5-16 us. The READY decoders (state_dispatch*, user/state
# payload, read state, relationships, thread members), the message and member-list paths and
# the voice audio path (mixer, transmit, playback, jitter buffer, receive, UDP media: +5% to
# +75% at -Oz, with per-sample loops) stay /O2. So do the voice, guild-admin and notification
# dispatchers, voice recording and send, call video, CDN admission and the entry loop: with them at
# -Oz the pipeline benchmark's READY and supplemental decode ran 2-3% longer under three code
# placements (interleaved A/B, 16-20 rounds; A/A under 1%).
maicord_compact_sources(
    SOURCES
        src/app/gateway/client.cpp src/app/gateway/client_outputs.cpp src/app/gateway/client_storage.cpp
        src/app/gateway/client_transport.cpp src/app/gateway/chat_state_dispatch.cpp
        src/app/gateway/client_session.cpp)
# Core and platform code with no byte loop: path building, per-frame DXVA submission and MP4
# decoder plumbing, speech, datagram and TLS stream control around the OS calls, the random
# source and the window backend (message translation and procedure, services, the non-client frame, settings and
# icons: the per-message work is a few compares and stores, and -Oz also drops the jump tables of the message
# switches); Gateway connection events, voice session descriptions, request
# bodies (JSON strings, the writer) and the P-256 signature framing, all once per session or
# per user action (the decoders that run per event stay /O2).
maicord_compact_sources(
    SOURCES
        src/core/data_path.cpp src/platform/crypto/random.cpp src/platform/window/win32/caption_frame.cpp
        src/platform/window/win32/main_window.cpp src/platform/window/win32/post.cpp src/gpu/d3d11/device.cpp
        src/gpu/d3d11/surface.cpp src/gpu/d3d11/paged_texture.cpp src/platform/net/socket.cpp
        src/platform/window/win32/translate.cpp src/platform/window/win32/window_proc.cpp src/platform/window/win32/window.cpp
        src/platform/window/win32/pump.cpp src/platform/window/win32/settings.cpp src/platform/window/win32/icon.cpp
        src/platform/window/win32/create_icon.cpp src/platform/window/win32/drop.cpp src/platform/window/win32/keys.cpp
        src/platform/audio/speech.cpp src/platform/net/datagram.cpp src/platform/net/tls_stream.cpp
        src/platform/decode/aac_decoder.cpp src/platform/decode/av1_dxva_parameters.cpp
        src/platform/decode/av1_gpu_stream.cpp src/platform/decode/gpu_stream_decoder.cpp
        src/platform/decode/h264_gpu_decoder.cpp src/platform/decode/h264_gpu_stream_decoder.cpp
        src/platform/decode/hevc_gpu_decoder.cpp src/platform/decode/hevc_gpu_stream_decoder.cpp
        src/platform/decode/mp4_decoder.cpp)
# Ogg/Opus page parsing, the window icon decode, local-file thumbnails and COLR paint program building (a
# regional flag's program too, once per flag, with the SVG path outlining it inlines).
# The rare-script shapers (Sinhala, Tamil, Myanmar, Khmer, Tibetan, Hebrew) stay /O2: -Oz cost them up to 6%
# more cycles per shaped run in the text benchmark.
maicord_compact_sources(
    SOURCES
        src/media/containers/ogg_opus.cpp src/media/containers/ogg_opus_writer.cpp
        src/app/shell/app_icon.cpp src/app/media/local_preview_thumbnail.cpp
        src/render/effects/colr_program.cpp src/render/effects/colr_paint.cpp
        src/render/effects/regional_flag_program.cpp)
# The Linux client. Its application sources are one library (maicord_app_client; the Windows build spreads them over
# MaiCord, the account module and maicord_app_core, and compacts the first two and the listed units of the third), so
# the application is cold except what the Windows build leaves at -O2: Gateway dispatch and payload decode, the media
# decode pumps and video paths, the voice audio, crypto and RTP paths and the frame loop.
if(MAICORD_LINUX)
    maicord_compact_sources(
        TARGETS maicord_app_client maicord_app_net maicord_app_update maicord_app_packages maicord_app_catalog_index
        EXCLUDE
            "/app/gateway/(dispatch(_notifications|_payload)?|guild_admin_dispatch|member_list_dispatch|payload_writer|presence_dispatch|private_dispatch|read_state_dispatch|relationship_dispatch|state_dispatch(_objects|_records)?|state_payload|thread_member_dispatch|user_payload|voice_dispatch)\\.cpp$"
            "/app/media/(account_media_video|account_video_session(_audio)?|cdn_media_pipeline|video(_input|_sources)?)\\.cpp$"
            "/app/notifications/(gateway_notification_records|sound_assets)\\.cpp$"
            "/app/voice/(call_video_presenter|dave/(frame_crypto|key_derivation|receive_sender|route)|jitter_buffer|message_flow|message_recorder|mixer|packet_crypto|playback|receive|send|soundboard_audio|transmit|udp_media|video_rtp_receive|video_sink|video_stream_decoders)\\.cpp$")
    # The platform layers the Windows build compacts under other names (win32 window, d3d11 device, IOCP, DXVA,
    # WASAPI, CNG, the Win32 file and shell services): once per window, device, connection, stream or user action, or
    # glue around the kernel's own work. Left at -O2 as on Windows: the sample, byte and frame loops (os/linux/text,
    # thread, wait and srwlock, the GLES draw, resource and video upload wrappers, the AEAD and hash wrappers, the
    # in-house AAC decoder, the Lottie geometry, the bitmap colour glyphs).
    maicord_compact_sources(
        TARGETS maicord_window_wayland maicord_dbus maicord_atspi maicord_audio_pulse maicord_net maicord_desktop_system
            maicord_font_system maicord_video_vaapi maicord_video_software maicord_video_decode
            maicord_ui_secure_text maicord_effect_host
        SOURCES
            src/core/linux/data_path.cpp src/core/linux/log.cpp src/core/linux/path.cpp src/core/linux/resource.cpp
            src/core/linux/shared_library.cpp
            src/os/linux/date_format.cpp src/os/linux/directory.cpp src/os/linux/file.cpp src/os/linux/local_time.cpp
            src/os/linux/ordinal.cpp src/os/linux/pages.cpp src/os/linux/process.cpp src/platform/time/tzif.cpp
            src/gpu/gles/device.cpp src/gpu/gles/loader.cpp src/gpu/gles/shader_pack.cpp src/gpu/gles/surface.cpp
            src/gpu/gles/sync.cpp
            src/platform/audio/speechd/speech.cpp
            src/platform/crypto/linux/random.cpp src/platform/crypto/linux/secure_buffer.cpp
            src/platform/crypto/openssl/openssl.cpp src/platform/crypto/openssl/kdf_sha256.cpp
            src/platform/crypto/openssl/p256.cpp src/platform/crypto/openssl/remote_auth_crypto.cpp
            src/platform/net/linux/tls_client.cpp
            src/platform/decode/linux/aac_decoder.cpp src/platform/decode/linux/gpu_stream_decoder.cpp
            src/platform/decode/linux/mp4_decoder.cpp src/platform/decode/linux/software_stream.cpp
            src/text/fonts/linux/handle_stat.cpp
            # The Windows build holds these in maicord_ui (and so compacts them); Linux's maicord_theme and
            # maicord_text_system hold them next to sources that stay -O2.
            src/ui/media/emoji_presentation.cpp src/ui/theme/styles.cpp src/ui/theme/theme.cpp)
endif()

# The contract's /Ob1 emits every function not declared inline as noinline, so
# the helpers of a -Oz source stay calls even where inlining them is smaller.
# /Ob2 lets the size-driven inliner (-Oz keeps its minimum threshold) fold
# them: 17 KiB less code in the client, and no slower in the offline perf
# probe. Sources that grew in measured size with /Ob2 keep /Ob1, as do
# theme_styles (the text shaping path inlined ui_theme_text_size into slower
# code: text benchmark cycles_latin +1.6%, cycles_arabic +3%) and
# virtual_list (retention benchmark -8%, text measure cache hit -9%).
get_target_property(maicord_ui_sources maicord_ui SOURCES)
if(MAICORD_LINUX)
    list(APPEND maicord_ui_sources src/ui/media/emoji_presentation.cpp src/ui/theme/styles.cpp src/ui/theme/theme.cpp)
endif()
foreach(source IN LISTS maicord_ui_sources)
    get_source_file_property(options "${source}" COMPILE_OPTIONS)
    if(options MATCHES "${MAICORD_SIZE_OPTION}" AND NOT source MATCHES
        "/ui/(account/friends_scene|account/menu|account/profile_activity|account/profile_media|chat/composer/message_composer_scene|chat/messages/action_bar|chat/messages/context_menu|chat/messages/rich_widgets|chat/messages/scene(_rows)?|chat/pinned_messages_scene|chat/polls/voters|chat/private_channel_rows|chat/threads/browser_scene|chat/threads/forum_channel_scene|controls/slider|layout/virtual_list|settings/scene(_layout|_paint)?|shell/navigation_view|shell/popup_presentation|shell/quick_switcher_scene|theme/style_projection|theme/styles|voice/call_stage|voice/panel|widgets/toast)\\.cpp$")
        set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS ${MAICORD_FOLD_OPTION})
    endif()
endforeach()
# The same /Ob2 for the other size-compiled sources that fold well: the protocol, model, render and text libraries,
# the Windows window, GPU and socket backends, the desktop services, the crypto setup and the image encoders. Sources
# that grew with /Ob2 keep /Ob1: the message stores (model/chat), the Gateway and HTTP encoders (protocol/encoding,
# protocol/http), the glyph and media renderers (render/text, render/media) and the OpenType tables; model/voice
# gained nothing.
function(maicord_fold_helpers)
    cmake_parse_arguments(ARG "" "" "INCLUDE;EXCLUDE" ${ARGN})
    get_property(targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(type STREQUAL "INTERFACE_LIBRARY")
            continue()
        endif()
        get_target_property(files ${target} SOURCES)
        foreach(source IN LISTS files)
            if(NOT source MATCHES "\\.cpp$" OR source MATCHES "^\\$<")
                continue()
            endif()
            set(selected FALSE)
            foreach(pattern IN LISTS ARG_INCLUDE)
                if(source MATCHES "${pattern}")
                    set(selected TRUE)
                endif()
            endforeach()
            foreach(pattern IN LISTS ARG_EXCLUDE)
                if(source MATCHES "${pattern}")
                    set(selected FALSE)
                endif()
            endforeach()
            get_source_file_property(options "${source}" COMPILE_OPTIONS)
            if(selected AND options MATCHES "${MAICORD_SIZE_OPTION}" AND NOT options MATCHES "${MAICORD_FOLD_OPTION}")
                set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS ${MAICORD_FOLD_OPTION})
            endif()
        endforeach()
    endforeach()
endfunction()
maicord_fold_helpers(
    INCLUDE "(^|/)src/(protocol|model|render|text)/" "(^|/)src/platform/(window/(win32|wayland)|desktop|crypto|dbus)/"
        "(^|/)src/gpu/(d3d11|gles)/" "(^|/)src/platform/net/(linux/)?socket\\.cpp$" "(^|/)src/media/image/"
    EXCLUDE "(^|/)src/model/(chat|voice)/" "(^|/)src/protocol/(encoding|http)/" "(^|/)src/render/(text|media)/"
        "(^|/)src/text/opentype/")
# The Linux client's third tier. It carries what the Windows one gets from the operating system (the D-Bus and AT-SPI
# services, the TLS and crypto wrappers over OpenSSL, the Wayland and EGL backends, the in-house AAC decoder, the Lottie
# geometry, the ELF plugin loader), and with the lists above it stayed over the Windows client's size cap
# (6,500,000 bytes). So every source that is still at the contract's -O2 compiles -Os (MAICORD_BALANCED_OPTION) instead,
# except the hot code below. Every exception was measured with the repository's former benchmarks built for Linux (with a
# <Windows.h> shim and drivers for the Gateway replay, the glyph atlas, the UI units and AAC) at -O2 and with the policy, `perf stat`
# instructions and best-of-five wall time per metric. One binary against itself moves a metric by up to 2%, and unrelated
# code by up to 5% on a metric of a few nanoseconds, so a path counts when several of its metrics move together.
# At -Os, text shaping cycles were 5-13% longer, the stores 3-9%, atlas allocation 9-17%, text entry (with the unicode
# tables) 3-12%, Markdown and code highlighting 3-4%, the history page decode 4-6%, JPEG thumbnails 12-31%, Adam7 PNG 6%,
# lossless WebP 4-6%, the AAC decoder 20% more instructions, the CELT encode 18%; those stay -O2. What is left at -Os
# measured within that noise: the Gateway replay (READY, supplemental, 3200 events: zstd, ETF, scan, dispatch), message
# and event decoding, the event pump, inflate and zstd, the fonts, GIF, and the render, GPU and container code. The text
# shapers also drop loop unrolling and vectorization (neutral, 5 KB); the same on the stores, text entry or the OpenType
# tables made them 3-43% slower, and outlining the -Os group made the stores and text entry 3-20% slower, so neither is
# done there. Code that runs once per user action or per network request (the media sessions and CDN pipeline, the
# Gateway handlers of the rarer events, HTTP/1, the MP4 and MPEG-TS containers) compiles -Oz: with it the Gateway
# replay, the event pump and the channel switch moved 0.0-0.8% in instructions (the wall times inside the noise), while
# -Oz on the whole of src/app/gateway made the READY producer 10% slower, so only the handlers named below.
# third_party/opus has its own tiers (third_party/opus/CMakeLists.txt). The Windows build never defines or calls any of
# this.
if(MAICORD_LINUX)
    # maicord_source_options(OPTIONS <option>... [INCLUDE <regex>...] [EXCLUDE <regex>...]): appends the options to
    # every src/*.cpp that no list above compiled for size or balance, when it matches INCLUDE (any, without it) and not EXCLUDE.
    function(maicord_source_options)
        cmake_parse_arguments(ARG "" "" "OPTIONS;INCLUDE;EXCLUDE" ${ARGN})
        get_property(targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
        foreach(target IN LISTS targets)
            get_target_property(type ${target} TYPE)
            if(type STREQUAL "INTERFACE_LIBRARY")
                continue()
            endif()
            get_target_property(files ${target} SOURCES)
            foreach(source IN LISTS files)
                if(NOT source MATCHES "^src/.*\\.cpp$")
                    continue()
                endif()
                get_source_file_property(options "${source}" COMPILE_OPTIONS)
                if(options MATCHES "${MAICORD_SIZE_OPTION}" OR options MATCHES "${MAICORD_BALANCED_OPTION}")
                    continue()
                endif()
                set(selected TRUE)
                if(ARG_INCLUDE)
                    set(selected FALSE)
                    foreach(pattern IN LISTS ARG_INCLUDE)
                        if(source MATCHES "${pattern}")
                            set(selected TRUE)
                        endif()
                    endforeach()
                endif()
                foreach(pattern IN LISTS ARG_EXCLUDE)
                    if(source MATCHES "${pattern}")
                        set(selected FALSE)
                    endif()
                endforeach()
                if(selected)
                    set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS ${ARG_OPTIONS})
                endif()
            endforeach()
        endforeach()
    endfunction()
    maicord_source_options(OPTIONS ${MAICORD_SIZE_OPTION}
        INCLUDE "^src/app/media/(account_|cdn_)"
            "^src/app/gateway/(guild_admin|thread_member|voice|dispatch_notifications|relationship|read_state|private)_dispatch"
            "^src/protocol/http/" "^src/protocol/gateway/reaction_events" "^src/media/containers/")
    maicord_source_options(OPTIONS ${MAICORD_BALANCED_OPTION}
        EXCLUDE "^src/core/(json_reader|json_scan|utf)\\.cpp$" "^src/core/linux/(runtime|start)\\.cpp$"
            "^src/os/linux/(thread|wait|srwlock)\\.cpp$" "^src/platform/crypto/"
            "^src/media/image/(jpeg|webp|lossy_webp|animated_webp|vp8|apng|png\\.cpp|png_common|png_stream)"
            "^src/media/audio/aac_lc\\.cpp$"
            "^src/protocol/(encoding|gateway/etf|messages/history|gateway/message_)"
            "^src/app/voice/(mixer|transmit|playback|receive|jitter_buffer|packet_crypto|udp_media|send)"
            "^src/text/(shaping|scripts|layout|opentype|unicode)/" "^src/ui/text/" "^src/ui/chat/messages/markdown"
            "^src/model/" "^src/render/text/glyph_atlas\\.cpp$")
    maicord_source_options(OPTIONS -fno-unroll-loops -fno-vectorize -fno-slp-vectorize
        INCLUDE "^src/text/(shaping|scripts|layout)/")
endif()
