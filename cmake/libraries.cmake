# Shipping static libraries linked into MaiCord.exe and MaiCordAccount.dll.
# Each source list is the archive's member order (and so the link order);
# keep it stable when adding files. Headers are found through the include
# paths and are not listed.

maicord_library(maicord_core
    src/core/arena.cpp src/core/byte_queue.cpp src/core/byte_order.cpp src/core/data_path.cpp
    src/core/json_reader.cpp src/core/json_scan.cpp src/core/reserved_bytes.cpp src/core/stable_storage.cpp
    src/core/utf.cpp src/core/byte_range.cpp)
# Checks MaiCord's SDK-free headers against the Windows SDK and compiler
# headers on every client build (a MaiCord dependency): one generated unit per
# header (tools/generate_sys_conformance.py). It defines nothing, so no image
# links it.
set(maicord_sys_headers
    include/maicord/core/win32_types.hpp include/maicord/core/win32_error.hpp
    include/maicord/core/win32_kernel.hpp include/maicord/core/win32_user.hpp
    include/maicord/sys/crt.hpp include/maicord/sys/intrin.hpp include/maicord/sys/simd.hpp
    include/maicord/sys/com.hpp include/maicord/sys/ole.hpp include/maicord/sys/registry.hpp
    include/maicord/sys/gdi.hpp include/maicord/sys/winsock.hpp include/maicord/sys/crypto.hpp
    include/maicord/sys/sspi.hpp include/maicord/sys/shell.hpp include/maicord/sys/winrt.hpp
    include/maicord/sys/dxgi.hpp include/maicord/sys/d3d11.hpp include/maicord/sys/dcomp.hpp
    include/maicord/sys/dxva.hpp include/maicord/sys/d2d1.hpp include/maicord/sys/dwrite.hpp
    include/maicord/sys/wasapi.hpp include/maicord/sys/media_foundation.hpp include/maicord/sys/tsf.hpp
    include/maicord/sys/uia.hpp include/maicord/sys/speech.hpp include/maicord/sys/spellcheck.hpp
    include/maicord/sys/webauthn.hpp include/maicord/sys/process_info.hpp include/maicord/sys/ioctl.hpp
    include/maicord/sys/libc/stddef.h include/maicord/sys/libc/stdint.h include/maicord/sys/libc/limits.h)
set(maicord_sys_conformance_sources src/core/win32_sdk_conformance.cpp)
foreach(header IN LISTS maicord_sys_headers)
    get_filename_component(name "${header}" NAME_WE)
    set(unit "${MAICORD_GENERATED_DIRECTORY}/sys_conformance/${name}.cpp")
    add_custom_command(OUTPUT "${unit}"
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_sys_conformance.py"
            --root "${CMAKE_CURRENT_SOURCE_DIR}" --header "${header}" --output "${unit}"
        DEPENDS "${header}" tools/generate_sys_conformance.py
        VERBATIM)
    list(APPEND maicord_sys_conformance_sources "${unit}")
endforeach()
add_library(maicord_sys_conformance OBJECT ${maicord_sys_conformance_sources})
target_link_libraries(maicord_sys_conformance PRIVATE maicord_compile_contract)
target_include_directories(maicord_sys_conformance PRIVATE src/sys)
set_target_properties(maicord_sys_conformance PROPERTIES MAICORD_SDK_INCLUDES ON)

maicord_library(maicord_compression src/media/deflate.cpp LIBS maicord_core)

maicord_library(maicord_media_core
    src/media/animation_timeline.cpp src/media/encoded_cache.cpp src/media/presentation_policy.cpp
    src/media/regional_flag_svg.cpp src/ui/regional_flag.cpp
    LIBS maicord_core)

set(MAIOPUS_DECODER_TARGET maicord_opus_decoder)
set(MAIOPUS_ENCODER_TARGET maicord_opus_encoder)
set(MAIOPUS_LINK_CONTRACT maicord_link_contract)
set(MAIOPUS_LTCG ON)
set(MAIOPUS_ARCH /arch:AVX)
set(MAIOPUS_SSE4_1 ON)
add_subdirectory(third_party/opus)

maicord_library(maicord_media_codec
    src/media/opus_decoder.cpp src/media/ogg_opus.cpp src/media/ogg_opus_writer.cpp src/media/wave_pcm.cpp
    src/media/mp3_stream.cpp src/media/apng.cpp src/media/gif.cpp src/media/h264_parameters.cpp
    src/media/h264_configuration.cpp src/media/h264_slice.cpp src/media/h264_access_unit.cpp
    src/media/h264_output_queue.cpp src/media/h264_picture_order.cpp src/media/h264_references.cpp
    src/media/h264_stream.cpp src/media/hevc_parameters.cpp src/media/hevc_picture.cpp
    src/media/hevc_mp4_demux.cpp src/media/hevc_references.cpp src/media/jpeg.cpp src/media/lossy_webp.cpp
    src/media/mp4_demux.cpp src/media/mp4_audio_demux.cpp src/media/progressive_source.cpp src/media/png.cpp
    src/media/png_stream.cpp src/media/png_encode.cpp src/media/png_common.cpp src/media/vp8.cpp
    src/media/webp.cpp src/media/webp_thumbnail.cpp src/media/webp_alpha.cpp src/media/animated_webp.cpp
    src/media/yuv_convert.cpp src/media/yuv_convert_avx2.cpp
    src/media/rtp_h264.cpp src/media/rtp_h265.cpp src/media/hevc_stream.cpp
    src/media/rtp_av1.cpp src/media/av1.cpp
    LIBS maicord_core maicord_compression maicord_opus_decoder ntdll
    INCLUDES third_party/minimp3)
set_source_files_properties(src/media/yuv_convert_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")

# Voice send encoder; linked only into MaiCordAccount, never the client EXE.
maicord_library(maicord_voice_encoder src/media/opus_encoder.cpp LIBS maicord_core maicord_opus_encoder)

maicord_library(maicord_cdn_media src/app/cdn_media_pipeline.cpp
    LIBS maicord_core maicord_media_core maicord_media_codec maicord_protocol)

# The combase.dll exports the toast path calls, imported by name from combase
# itself rather than through runtimeobject.lib's api-ms-win-core-winrt API sets.
set(maicord_combase_import_library "${MAICORD_GENERATED_DIRECTORY}/maicord_combase.lib")
add_custom_command(OUTPUT "${maicord_combase_import_library}"
    COMMAND "${CMAKE_AR}" /NOLOGO /MACHINE:X64 "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/platform/combase.def"
        "/OUT:${maicord_combase_import_library}"
    DEPENDS src/platform/combase.def
    VERBATIM)
add_custom_target(maicord_combase_import_library DEPENDS "${maicord_combase_import_library}")
add_library(maicord_combase INTERFACE)
target_link_libraries(maicord_combase INTERFACE "${maicord_combase_import_library}")
add_dependencies(maicord_combase maicord_combase_import_library)

maicord_library(maicord_platform
    src/platform/staged_file.cpp src/platform/browser_sign_in_launcher.cpp src/platform/aac_decoder.cpp
    src/platform/media_audio.cpp src/platform/aead.cpp src/platform/aes_gcm.cpp src/platform/aes_gcm64.cpp
    src/platform/hkdf_sha256.cpp src/platform/p256.cpp src/platform/xchacha20_poly1305.cpp
    src/platform/network.cpp src/platform/network_datagram.cpp src/platform/network_file.cpp
    src/platform/file_picker.cpp src/platform/clipboard_attachment.cpp src/platform/clipboard_image.cpp
    src/platform/file_identity.cpp src/platform/network_worker.cpp src/platform/random.cpp
    src/platform/timezone.cpp src/platform/remote_auth_crypto.cpp src/platform/secure_buffer.cpp
    src/platform/tls_client.cpp src/platform/tls_stream.cpp src/platform/desktop_notifications.cpp
    src/platform/notification_identity.cpp src/platform/taskbar_attention.cpp
    src/platform/desktop_integration.cpp src/platform/system_tray.cpp src/platform/spell_checker.cpp
    src/platform/speech.cpp src/platform/voice_audio_output.cpp src/platform/voice_audio_capture.cpp
    src/platform/window_frame.cpp
    # Used by the application core and client preferences, which both link this
    # library: one compile for every image.
    src/app/file_persistence.cpp
    LIBS maicord_core ws2_32 secur32 bcrypt crypt32 ole32 user32 shell32 mfuuid ntdll maicord_combase advapi32)

maicord_library(maicord_notification_pcm src/media/notification_pcm.cpp
    LIBS maicord_core maicord_compression maicord_media_codec)
maicord_library(maicord_notification_audio src/platform/notification_audio.cpp
    LIBS maicord_core kernel32 ole32 user32)
maicord_library(maicord_notification_sound_assets src/app/notification_sound_assets.cpp
    LIBS maicord_notification_pcm maicord_notification_audio kernel32)

maicord_library(maicord_account_paths
    src/app/account_paths.cpp src/app/account_inventory.cpp src/app/account_switch.cpp
    src/app/account_reconnect.cpp
    LIBS maicord_core kernel32)
maicord_library(maicord_account_host src/app/account_logout_stop.cpp src/app/account_host.cpp LIBS kernel32)

maicord_library(maicord_client_preferences
    src/app/client_theme_document.cpp src/app/client_theme_storage.cpp src/app/client_preferences.cpp
    src/app/client_preferences_writer.cpp src/app/voice_preferences.cpp
    LIBS maicord_core maicord_ui maicord_platform kernel32)

maicord_library(maicord_protocol
    src/protocol/message_poll.cpp src/protocol/gateway_message_poll.cpp src/protocol/browser_sign_in.cpp
    src/protocol/captcha_bridge_page.cpp src/protocol/base64.cpp src/protocol/base64_codec.cpp
    src/protocol/base64url.cpp src/protocol/cdn_url.cpp src/protocol/attachment_refresh.cpp
    src/protocol/attachment_upload.cpp src/protocol/upload_allowance.cpp src/protocol/https_upload.cpp
    src/protocol/cdn_http.cpp src/protocol/rest.cpp src/protocol/json_ids.cpp
    src/protocol/rest_route.cpp src/protocol/gateway_commands.cpp src/protocol/gateway_endpoint.cpp
    src/protocol/gateway_events.cpp src/protocol/account_requirement.cpp src/protocol/gateway_voice.cpp
    src/protocol/gateway_stream.cpp src/protocol/stage_channel.cpp src/protocol/user_social.cpp
    src/protocol/guild_community.cpp src/protocol/voice_gateway.cpp src/protocol/voice_discovery.cpp
    src/protocol/media_packet.cpp src/protocol/voice_media_events.cpp src/protocol/voice_participants.cpp
    src/protocol/dave_replay.cpp src/protocol/dave_frame.cpp src/protocol/p256_signature.cpp
    src/protocol/mls_objects.cpp src/protocol/mls_objects_write.cpp src/protocol/voice_heartbeat.cpp
    src/protocol/voice_session.cpp src/protocol/websocket_message.cpp src/protocol/gateway_lifecycle.cpp
    src/protocol/gateway_message_events.cpp src/protocol/gateway_message_validation.cpp
    src/protocol/gateway_reaction_events.cpp src/protocol/gateway_typing_events.cpp
    src/protocol/gateway_message_rich.cpp src/protocol/message_interaction.cpp src/protocol/gateway_payload.cpp
    src/protocol/gateway_runtime.cpp src/protocol/gateway_session.cpp src/protocol/http1.cpp
    src/protocol/http_response_stream.cpp src/protocol/attachment_download.cpp src/protocol/json_write.cpp
    src/protocol/message_history.cpp src/protocol/message_ack.cpp src/protocol/bulk_message_ack.cpp
    src/protocol/message_create.cpp src/protocol/message_delete.cpp src/protocol/private_conversation.cpp
    src/protocol/message_edit.cpp src/protocol/message_reaction.cpp src/protocol/profile_decode.cpp
    src/protocol/activity_decode.cpp src/protocol/reaction_users.cpp src/protocol/message_typing.cpp
    src/protocol/remote_auth.cpp src/protocol/user_auth.cpp src/protocol/webauthn_mfa.cpp
    src/protocol/websocket_frame.cpp src/protocol/websocket_handshake.cpp src/protocol/application_commands.cpp
    src/protocol/writer.cpp src/protocol/login_continuations.cpp src/protocol/relationships.cpp
    src/protocol/message_search.cpp src/protocol/message_pins.cpp src/protocol/thread_actions.cpp
    src/protocol/thread_browser.cpp src/protocol/thread_creation.cpp src/protocol/forum_post.cpp
    src/protocol/media_catalog.cpp src/protocol/gateway_presence.cpp src/protocol/client_status.cpp
    src/protocol/self_status_settings.cpp src/protocol/guild_folders.cpp src/protocol/guild_invite.cpp
    src/protocol/user_settings_proto.cpp src/protocol/account_settings.cpp src/protocol/emoji_preferences.cpp
    src/protocol/gif_favorites.cpp src/protocol/channel_overview.cpp src/protocol/guild_admin.cpp
    src/protocol/guild_assets.cpp src/protocol/profile_edit.cpp src/protocol/voice_video_signaling.cpp
    src/protocol/rtcp_feedback.cpp src/protocol/soundboard.cpp src/protocol/notification_settings.cpp
    src/protocol/user_guild_settings.cpp
    # Icon hash check kept apart from cdn_url.cpp: the client keeps it (Gateway
    # user records) while it forwards every other CDN URL function to the module.
    src/protocol/cdn_icon_hash.cpp src/protocol/guild_moderation.cpp src/protocol/voice_channel_info.cpp
    LIBS maicord_core maicord_compression maicord_media_core)

maicord_library(maicord_webauthn src/platform/webauthn.cpp LIBS maicord_core maicord_platform maicord_protocol)

maicord_library(maicord_app_core
    src/app/poll_controller.cpp src/app/poll_draft.cpp src/app/gateway_state_dispatch.cpp
    src/app/gateway_payload_writer.cpp src/app/gateway_user_payload.cpp src/app/browser_login.cpp
    src/app/browser_sign_in_receiver.cpp src/app/captcha_bridge.cpp src/app/application_update_startup.cpp
    src/app/login_continuations.cpp src/app/input_layers.cpp src/app/navigation_shortcuts.cpp
    src/app/shortcut_registry.cpp src/app/account_profile.cpp src/app/account_credential_forget.cpp
    src/app/account_logout.cpp src/app/account_profile_promotion.cpp src/app/account_session_store.cpp
    src/app/captcha_challenge.cpp src/app/cdn_client.cpp src/app/progressive_video_fetch.cpp
    src/app/https_file_upload.cpp src/app/https_file_download.cpp src/app/rest_client.cpp
    src/app/gateway_client.cpp src/app/voice_connection_setup.cpp
    src/app/voice_connection.cpp src/app/voice_udp_discovery.cpp src/app/voice_media_setup.cpp
    src/app/voice_packet_crypto.cpp src/app/dave_frame_crypto.cpp src/app/dave_key_derivation.cpp
    src/app/mls_exporter.cpp src/app/mls_key_schedule.cpp src/app/mls_signature.cpp src/app/hpke_derivation.cpp
    src/app/hpke_context.cpp src/app/mls_hpke.cpp src/app/mls_key_package.cpp src/app/dave_key_package.cpp
    src/app/dave_welcome.cpp src/app/dave_receive_epoch.cpp src/app/dave_receive_sender.cpp
    src/app/voice_udp_media.cpp src/app/voice_participants.cpp src/app/voice_socket.cpp
    src/app/gateway_voice_control.cpp src/app/gateway_voice_dispatch.cpp src/app/gateway_stage_dispatch.cpp
    src/app/gateway_scheduled_event_dispatch.cpp src/app/gateway_stream_control.cpp src/app/gateway_dispatch.cpp
    src/app/gateway_notification_records.cpp src/app/gateway_dispatch_payload.cpp
    src/app/gateway_member_list_dispatch.cpp src/app/gateway_guild_admin_dispatch.cpp
    src/app/gateway_thread_member_dispatch.cpp src/app/gateway_presence_dispatch.cpp
    src/app/gateway_private_dispatch.cpp src/app/gateway_relationship_dispatch.cpp
    src/app/gateway_ready_projection.cpp src/app/gateway_guild_folders.cpp src/app/gateway_user_settings.cpp
    src/app/gateway_read_state_dispatch.cpp src/app/account_relationships.cpp src/app/account_message_ack.cpp
    src/app/account_bulk_message_ack.cpp src/app/account_gif_favorites.cpp src/app/account_action_captcha.cpp
    src/app/channel_overview_controller.cpp src/app/pinned_message_controller.cpp
    src/app/quick_switcher_controller.cpp src/app/message_forward_controller.cpp
    src/app/message_reaction_controller.cpp src/app/account_channel_overview.cpp src/app/account_guild_admin.cpp
    src/app/account_stage.cpp src/app/account_user_social.cpp src/app/account_guild_community.cpp
    src/app/account_pinned_messages.cpp src/app/account_thread_actions.cpp src/app/account_thread_browser.cpp
    src/app/account_thread_creation.cpp src/app/account_poll_create.cpp
    src/app/account_notification_settings.cpp src/app/account_private_channel.cpp src/app/account_forum_post.cpp
    src/app/received_attachment_open.cpp src/app/gateway_state_payload.cpp src/app/login_mfa_policy.cpp
    src/app/member_list_channel_binding.cpp src/app/remote_auth_client.cpp src/app/remote_auth_handshake.cpp
    src/app/remote_auth_ticket_exchange.cpp src/app/user_client_configuration.cpp
    src/app/user_account_coordinator.cpp src/app/user_auth_coordinator.cpp src/app/update_manifest.cpp
    src/app/update_artifact.cpp src/app/update_acceptance.cpp src/app/update_stage.cpp
    src/app/update_prepare.cpp src/app/update_pair_package.cpp src/app/update_generation.cpp
    src/app/update_generation_authentication.cpp src/app/update_handoff.cpp src/app/update_install.cpp
    src/app/update_fetch.cpp src/app/update_https.cpp src/app/update_service.cpp src/app/search_recent.cpp
    src/app/channel_memory.cpp
    src/app/link_trust.cpp src/app/voice_dave_route.cpp src/app/voice_jitter_buffer.cpp src/app/voice_mixer.cpp
    src/app/voice_playback.cpp src/app/voice_receive.cpp src/app/gateway_stream_session.cpp
    src/app/video_rtp_receive.cpp src/app/voice_video_sink.cpp
    # Voice send: capture, gating, encoding and RTP. Executables pull the encoder
    # only when they reference the send path (the account module and voice tests).
    src/app/voice_transmit.cpp src/app/voice_send.cpp src/app/soundboard_audio.cpp src/app/account_soundboard.cpp
    src/app/voice_message_flow.cpp src/app/voice_message_recorder.cpp src/app/account_voice_message.cpp
    src/app/call_video_presenter.cpp src/app/stream_preview_cache.cpp src/app/stream_watch_view.cpp
    # acceptance_record.cpp: durable floor records shared by update_acceptance and the plugin catalog.
    src/app/acceptance_record.cpp src/app/gateway_guild_details.cpp src/app/account_guild_moderation.cpp
    src/app/update_changelog.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_media_codec maicord_media_core
        maicord_voice_encoder)

maicord_library(maicord_account_media
    src/app/local_preview_thumbnail.cpp src/app/local_video_poster.cpp src/app/account_media.cpp
    src/app/regional_flag_assets.cpp src/app/account_media_worker.cpp src/app/account_media_video.cpp
    src/app/account_video_session.cpp src/platform/mp4_decoder.cpp src/platform/h264_gpu_decoder.cpp
    src/platform/hevc_gpu_decoder.cpp src/platform/h264_gpu_configuration.cpp
    src/platform/h264_gpu_stream_decoder.cpp src/app/video_stream_decoders.cpp
    src/platform/gpu_decoder_surfaces.cpp src/platform/gpu_stream_decoder.cpp src/platform/hevc_gpu_stream_decoder.cpp
    src/platform/av1_gpu_stream.cpp src/platform/av1_dxva_parameters.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_media_core maicord_media_codec
        maicord_cdn_media maicord_paint_program d3d11 dxguid maicord_compression
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}"
    DEPENDS maicord_regional_flag_data)

maicord_library(maicord_unicode
    src/text/arabic_joining.cpp src/text/line_break.cpp src/text/line_break_tailored.cpp
    src/text/thai_word_break.cpp src/text/thai_lexical.cpp src/text/thai_syllable.cpp
    src/text/bidi_tailored.cpp src/text/unicode.cpp src/text/grapheme.cpp src/text/bidi.cpp
    LIBS maicord_core)

maicord_library(maicord_font
    src/text/arabic_shape_run.cpp src/text/arabic_marks.cpp src/text/arabic_plan.cpp
    src/text/devanagari_plan.cpp src/text/use_plan.cpp src/text/sinhala_input.cpp src/text/tibetan_input.cpp
    src/text/tibetan_run.cpp src/text/tibetan_shape_cluster.cpp src/text/khmer_input.cpp
    src/text/myanmar_shape_cluster.cpp src/text/myanmar_input.cpp src/text/khmer_shape_cluster.cpp
    src/text/khmer_run.cpp src/text/sinhala_shape_cluster.cpp src/text/devanagari_forms.cpp
    src/text/devanagari_clusters.cpp src/text/indic_input.cpp src/text/tamil_shape_run.cpp
    src/text/tamil_run.cpp src/text/myanmar_run.cpp src/text/sinhala_run.cpp src/text/script_paragraphs.cpp
    src/text/indic_run_plan.cpp src/text/devanagari_order.cpp
    src/text/devanagari_shape_cluster.cpp src/text/indic_run.cpp src/text/compact_run.cpp src/text/devanagari_run.cpp
    src/text/devanagari_run_plan.cpp src/text/tt_orientation.cpp
    src/text/tt_bold_points.cpp src/text/font_session.cpp src/text/font_prepare.cpp
    src/text/gpos_cursive_execute.cpp src/text/position_anchor.cpp src/text/gpos_mark_execute.cpp
    src/text/gpos_ligature_execute.cpp src/text/gpos_execute.cpp src/text/gpos_program.cpp
    src/text/lookup_selection.cpp src/text/position_flags.cpp
    src/text/gpos_attachment.cpp src/text/gpos.cpp src/text/clusters.cpp src/text/components.cpp
    src/text/shape_run.cpp src/text/run_plan.cpp src/text/font_names.cpp src/text/font_match.cpp
    src/text/font_candidates.cpp src/text/font_family.cpp src/text/font_catalog.cpp
    src/text/font_handle_stat.cpp src/text/font_fallback.cpp src/text/font_fallback_order.cpp
    src/text/font_fallback_catalog.cpp src/text/font_catalog_coverage.cpp src/text/font_fallback_range.cpp
    src/text/font_fallback_route.cpp src/text/font_fallback_map.cpp
    src/text/font_run.cpp src/text/arabic_run_plan.cpp src/text/arabic_run.cpp
    src/text/arabic_paragraph.cpp src/text/gsub_execute.cpp src/text/glyph_buffer.cpp src/text/context.cpp
    src/text/gsub.cpp src/text/layout_directory.cpp src/text/lookup_plan.cpp src/text/nominal_participation.cpp
    src/text/nominal_sources.cpp src/text/ascii_run.cpp src/text/latin_run.cpp
    src/text/hebrew_shape_run.cpp src/text/run_profile.cpp src/text/emoji_run.cpp src/text/emoji_line.cpp
    src/text/emoji_paragraph.cpp src/text/visibility.cpp src/text/script_class.cpp
    src/text/unicode_line_geometry.cpp src/text/latin_line.cpp src/text/latin_paragraph.cpp
    src/text/ascii_line.cpp src/text/ascii_paragraph.cpp src/text/paragraph_hits.cpp
    src/text/paragraph_ranges.cpp src/text/font_metrics.cpp src/text/layout_variations.cpp src/text/gdef.cpp
    src/text/layout_tables.cpp src/text/sfnt.cpp src/text/sfnt_cmap.cpp src/text/truetype_outline.cpp
    src/text/cff_font.cpp src/text/cff_charset.cpp src/text/cff_outline.cpp src/text/cpal.cpp
    src/text/colr_font.cpp src/text/colr_paint_reader.cpp src/text/colr_validate.cpp
    src/text/variation_store.cpp src/text/metric_variations.cpp src/text/gvar.cpp src/text/font_geometry.cpp
    src/text/font_store.cpp src/text/font_instance.cpp src/text/tt_interpreter.cpp src/text/tt_outline.cpp
    src/text/colr_variations.cpp src/text/font_axes.cpp
    LIBS maicord_core maicord_unicode ntdll)
# Compact the shared Indic ordering routine; keep /O2, /Ob1 and LTCG.
set_property(SOURCE src/text/devanagari_order.cpp APPEND PROPERTY COMPILE_OPTIONS /Os)

maicord_library(maicord_text
    src/ui/text.cpp src/ui/native_text_candidate.cpp src/ui/native_text_catalog.cpp src/core/log.cpp
    LIBS maicord_core maicord_font maicord_ui advapi32 gdi32)
# Keep the native layout adapter compact while retaining optimization and LTCG,
# and compact only the explicitly scoped cold catalog discovery/publication.
set_property(SOURCE src/ui/native_text_candidate.cpp APPEND PROPERTY COMPILE_OPTIONS /Os)
set_property(SOURCE src/ui/native_text_candidate.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS MAICORD_COMPACT_TEXT_ADAPTER=1)
set_property(SOURCE src/ui/native_text_catalog.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS MAICORD_COMPACT_CATALOG_SETUP=1)

maicord_library(maicord_render_core
    src/render/animation_frame_cache.cpp src/render/glyph_atlas.cpp src/render/glyph_quad_bounds.cpp
    src/render/glyph_raster.cpp src/render/media_atlas.cpp src/render/presentation_damage.cpp
    LIBS maicord_core)

maicord_library(maicord_paint_program
    src/render/colr_program.cpp src/render/colr_source.cpp src/render/regional_flag_program.cpp
    src/render/lottie_program.cpp
    LIBS d2d1 maicord_compile_contract maicord_core maicord_render_core maicord_font maicord_media_core)

maicord_library(maicord_ui
    src/ui/poll_controls.cpp src/ui/poll_voters.cpp src/ui/poll_presentation.cpp src/ui/audio_controls.cpp
    src/ui/audio_presentation.cpp src/ui/video_controls.cpp src/ui/video_scene.cpp src/ui/spinner.cpp
    src/ui/transition.cpp src/ui/tooltip.cpp src/ui/emoji_presentation.cpp src/ui/ansi_text.cpp
    src/ui/code_highlight.cpp src/ui/composer_autocomplete.cpp src/ui/message_markdown.cpp
    src/ui/timestamp_format.cpp src/ui/navigation_view.cpp src/ui/emoji_catalog.cpp src/ui/geometry.cpp
    src/ui/guild_rail_scene.cpp src/ui/navigation_drag.cpp src/ui/unread_banner_scene.cpp
    src/app/unread_banner_projection.cpp src/ui/layout_engine.cpp src/ui/login_scene.cpp
    src/ui/login_presentation.cpp src/ui/login_text_presentation.cpp src/ui/login_view.cpp
    src/ui/member_list_scene.cpp src/ui/member_list_navigation.cpp src/ui/message_action_bar.cpp
    src/ui/message_action_bar_presentation.cpp src/ui/message_context_controller.cpp
    src/ui/message_context_menu.cpp src/ui/user_context_menu.cpp src/ui/unread_context.cpp
    src/ui/message_rich_layout.cpp src/ui/message_component_layout.cpp src/ui/message_component_media.cpp
    src/ui/message_media_policy.cpp src/ui/message_system_text.cpp src/ui/message_rich_widgets.cpp
    src/ui/message_scene.cpp src/ui/message_scene_navigation.cpp src/ui/message_scene_measurements.cpp
    src/ui/scrollbar.cpp src/ui/message_composer_scene.cpp src/ui/composer_attachment_strip.cpp
    src/ui/composer_voice_bar.cpp src/ui/message_presentation.cpp src/ui/image_viewer.cpp
    src/ui/message_reaction_presentation.cpp src/ui/message_typing_presentation.cpp src/ui/paint.cpp
    src/ui/icon.cpp src/ui/button.cpp src/ui/message_text_coordinator.cpp src/ui/message_text_presentation.cpp
    src/ui/popup_scene.cpp src/ui/channel_context_policy.cpp src/ui/guild_context_policy.cpp
    src/ui/member_context_policy.cpp src/ui/attachment_menu.cpp src/ui/received_attachment_menu.cpp
    src/ui/popup_presentation.cpp src/ui/text_edit_menu.cpp src/ui/spell_tokenize.cpp
    src/ui/composer_spellcheck.cpp src/ui/text_form.cpp src/ui/text_composition.cpp src/ui/qr_code.cpp
    src/ui/reaction_picker.cpp src/ui/edit_field.cpp src/ui/badge.cpp src/ui/profile_scene.cpp
    src/ui/profile_media.cpp src/ui/profile_dialog_scene.cpp src/ui/reaction_viewer.cpp
    src/ui/reaction_viewer_presentation.cpp src/ui/scene.cpp src/ui/shell_navigation_scene.cpp
    src/ui/shell_presentation.cpp src/ui/shell_text_presentation.cpp src/ui/private_channel_title.cpp
    src/ui/private_channel_rows.cpp src/ui/smooth_scroll.cpp src/ui/scroll_host.cpp src/ui/text_flow_layout.cpp
    src/ui/text_measure_cache.cpp src/ui/text_presentation.cpp src/ui/virtual_list.cpp src/ui/widget_manager.cpp
    src/ui/widget_builder.cpp src/ui/menu_row.cpp src/ui/button_keyboard.cpp src/ui/input_router.cpp
    src/ui/list_navigation.cpp src/ui/list_paging.cpp src/ui/widget_scroll.cpp src/ui/widget_storage.cpp
    src/ui/page_registry.cpp src/ui/settings_scene.cpp src/ui/theme_editor.cpp src/ui/theme_styles.cpp
    src/ui/theme_style_projection.cpp src/ui/settings_choice.cpp src/ui/friends_scene.cpp
    src/ui/friends_media.cpp src/ui/controls/settings_tokens.cpp src/ui/controls/switch.cpp
    src/ui/controls/radio.cpp src/ui/controls/slider.cpp src/ui/controls/input.cpp src/ui/controls/swatch.cpp
    src/ui/controls/navigation.cpp src/ui/controls/text_metrics.cpp src/ui/settings_schema.cpp
    src/ui/controls/permission_grid.cpp src/ui/controls/admin_list.cpp src/ui/accessibility.cpp
    src/ui/accessibility_scenes.cpp src/ui/icon_pack.cpp src/ui/account_menu.cpp src/ui/account_switcher.cpp
    src/ui/search_scene.cpp src/ui/pinned_messages_scene.cpp src/ui/inbox_scene.cpp src/ui/thread_header.cpp
    src/ui/thread_browser_scene.cpp src/ui/forum_channel_scene.cpp src/ui/forum_post_scene.cpp
    src/ui/attachment_draft_list.cpp src/ui/quick_switcher_scene.cpp src/ui/message_forward_scene.cpp
    src/ui/download_scene.cpp src/ui/shortcuts_scene.cpp src/ui/media_picker.cpp
    src/ui/media_picker_presentation.cpp src/ui/search_query.cpp src/ui/search_filter_context.cpp
    src/ui/voice_channel_rows.cpp src/ui/voice_panel.cpp
    src/ui/call_scene.cpp src/ui/call_stage.cpp src/ui/stream_card_scene.cpp src/ui/guild_card_scene.cpp
    src/ui/guild_banner.cpp src/ui/profile_activity.cpp src/ui/caption_bar.cpp src/ui/activity_status.cpp
    src/ui/typing_indicator.cpp src/ui/dm_profile_panel.cpp
    src/ui/toast.cpp src/ui/theme.cpp
    # The spell tokenizer classifies letters with the Unicode property tables;
    # deflate.cpp checks gzip members with png_crc32 from maicord_media_codec.
    LIBS maicord_core maicord_model maicord_media_core maicord_unicode maicord_compression maicord_media_codec
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}"
    DEPENDS maicord_emoji_catalog_data maicord_icon_pack_data)

# UI Automation server over accessibility snapshots. UIAutomationCore and
# OleAut32 load on the first WM_GETOBJECT, so neither is a static import.
maicord_library(maicord_uia src/platform/uia_provider.cpp LIBS maicord_core maicord_ui)

# Native extension ownership stays in independent archives. The shell links
# settings pages and manual session loading; the account DLL owns no plugin UI.
maicord_library(maicord_plugin_host
    src/app/plugin_host.cpp src/app/plugin_settings_projection.cpp src/app/plugin_call_ledger.cpp
    src/app/plugin_sdk.cpp src/app/plugin_sdk_surfaces.cpp src/app/plugin_sdk_data.cpp
    LIBS maicord_core maicord_ui)
maicord_library(maicord_plugin_modules
    src/app/plugin_module_loader.cpp src/app/plugin_manifest.cpp src/app/plugin_catalog.cpp
    src/app/plugin_bundle_install.cpp src/app/plugin_preferences.cpp
    LIBS maicord_core maicord_plugin_host kernel32 bcrypt)
maicord_library(maicord_plugin_manager
    src/app/plugin_index.cpp src/app/plugin_index_policy.cpp src/app/plugin_index_acceptance.cpp
    src/app/plugin_providers.cpp src/app/plugin_manager_state.cpp src/app/plugin_index_service.cpp
    src/app/plugin_manager_install.cpp src/app/plugin_manager_runtime.cpp
    LIBS maicord_core maicord_platform maicord_app_core maicord_plugin_modules kernel32 bcrypt)

maicord_library(maicord_edit_history src/model/edit_history.cpp LIBS maicord_core maicord_platform)

maicord_library(maicord_ui_input
    src/ui/clipboard.cpp src/ui/secure_edit.cpp src/ui/secure_edit_target.cpp src/ui/secure_edit_history.cpp
    src/ui/secure_text_store.cpp
    LIBS maicord_core maicord_platform maicord_edit_history user32)

maicord_library(maicord_model
    src/model/message_poll_store.cpp src/model/bounded_edit.cpp src/model/channel_permissions.cpp
    src/model/permission_compute.cpp src/model/guild_ban_store.cpp src/model/member_moderation.cpp
    src/model/guild_member_store.cpp src/model/member_list.cpp src/model/thread_member_store.cpp
    src/model/thread_member_list.cpp src/model/chat_state.cpp src/model/read_state.cpp
    src/model/guild_display_order.cpp src/model/guild_folder_edit.cpp src/model/channel_reorder.cpp
    src/model/private_channel_store.cpp src/model/custom_emoji.cpp src/model/user_profile.cpp
    src/model/user_activity.cpp src/model/user_store.cpp src/model/relationship_store.cpp
    src/model/user_note_store.cpp src/model/active_now.cpp src/model/friend_search.cpp
    src/model/typing_store.cpp src/model/message_rich_store.cpp src/model/invite_card_cache.cpp
    src/model/scheduled_event_store.cpp src/model/message_composer.cpp
    src/model/message_composer_input_target.cpp src/model/message_draft_store.cpp
    src/model/reaction_preferences.cpp src/model/message_store.cpp src/model/message_search.cpp
    src/model/message_notifications.cpp src/model/voice_state_store.cpp src/model/stage_instance_store.cpp
    src/model/stage_policy.cpp src/model/image_crop.cpp src/model/profile_editor.cpp
    src/app/mls_tree_math.cpp src/app/mls_ratchet_tree.cpp src/app/mls_tree_validate.cpp src/app/mls_tree_edit.cpp
    src/app/mls_treekem.cpp src/app/mls_group_context.cpp src/app/mls_proposal.cpp src/app/mls_commit.cpp
    src/app/mls_welcome.cpp src/app/mls_group.cpp src/app/dave_session.cpp src/app/voice_connection_dave.cpp
    src/app/voice_log.cpp
    src/model/desktop_notification_queue.cpp src/model/notification_sound_policy.cpp
    src/model/notification_sound_admission.cpp src/model/notification_retention.cpp
    src/model/permission_grid.cpp src/model/guild_settings_model.cpp src/model/channel_settings_model.cpp
    src/model/guild_detail_store.cpp src/model/guild_member_search.cpp src/model/voice_channel_info_store.cpp
    LIBS maicord_core maicord_edit_history maicord_protocol)

maicord_library(maicord_message_runtime
    src/app/message_edit_runtime.cpp src/app/message_composer_controller.cpp src/app/command_autocomplete.cpp
    src/app/message_send_response.cpp src/app/message_attachment_storage.cpp
    src/app/message_attachment_drafts.cpp src/app/reaction_viewer_controller.cpp
    src/app/reaction_viewer_storage.cpp src/app/message_draft_persistence.cpp src/app/message_draft_session.cpp
    src/app/chat_state_dispatch.cpp src/app/private_conversation.cpp src/app/private_channel_actions.cpp
    src/app/user_store_dispatch.cpp src/app/message_event_pump.cpp src/app/read_ack_controller.cpp
    src/app/read_scope.cpp src/app/bulk_read_ack_controller.cpp src/app/message_history_sync.cpp
    src/app/media_url_resolver.cpp src/app/message_send_tracker.cpp src/app/message_timeline.cpp
    src/app/message_view_runtime.cpp src/app/message_snapshot_cache.cpp src/model/message_reconcile.cpp
    src/app/message_interaction_rules.cpp
    LIBS maicord_core maicord_model maicord_protocol maicord_app_core maicord_platform maicord_ui kernel32)

maicord_library(maicord_message_media
    src/app/animation_visual_runtime.cpp src/app/message_media_controller.cpp
    LIBS maicord_core maicord_model maicord_ui maicord_message_runtime maicord_media_core maicord_render_core)

# deflate.cpp checks gzip members with png_crc32 from maicord_media_codec.
maicord_library(maicord_shader_bytecode src/render/shader_bytecode.cpp "${MAICORD_SHADER_PACK_INCLUDE}"
    LIBS maicord_core maicord_compression maicord_media_codec d3d11
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}")

maicord_library(maicord_atlas_gpu
    src/render/video_frame_gpu.cpp "${MAICORD_VIDEO_FRAME_HEADER}" src/render/glyph_atlas_gpu.cpp
    src/render/media_atlas_gpu.cpp src/render/media_atlas_gpu_animation.cpp
    src/render/media_atlas_gpu_dispatch.cpp src/render/media_atlas_gpu_operations.cpp
    src/render/media_atlas_gpu_validation.cpp src/render/media_atlas_gpu_vp8.cpp "${MAICORD_VP8_IDCT_HEADER}"
    "${MAICORD_COLR_WIDE_HEADER}" "${MAICORD_COLR_COMPUTE_HEADER}" "${MAICORD_COLR_LAYERS_HEADER}"
    "${MAICORD_GLYPH_COMPUTE_HEADER}" "${MAICORD_PNG_COMPUTE_HEADER}" "${MAICORD_VP8_RECONSTRUCT_HEADER}"
    "${MAICORD_VP8_FILTER_HEADER}" "${MAICORD_VP8_ATLAS_HEADER}" "${MAICORD_JPEG_IDCT_HEADER}"
    "${MAICORD_JPEG_ATLAS_HEADER}"
    LIBS maicord_core maicord_render_core maicord_text maicord_shader_bytecode d3d11
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}")

maicord_library(maicord_renderer
    src/render/animation_frame_cache_gpu.cpp src/render/content_text_retention.cpp
    src/render/shaped_text_cache.cpp src/render/d3d11_device.cpp src/render/d3d11_draw_pipeline.cpp
    src/render/colr_paint.cpp src/render/d3d11_glyph_text.cpp src/render/icon_atlas.cpp
    src/render/native_glyph_outlines.cpp src/render/d3d11_background_fx.cpp src/render/d3d11_gpu_timer.cpp
    src/render/d3d11_renderer.cpp src/render/d3d11_renderer_draw.cpp src/render/d3d11_renderer_edit.cpp
    src/render/d3d11_renderer_layout.cpp src/render/d3d11_renderer_text.cpp
    src/render/d3d11_renderer_interaction.cpp src/render/d3d11_renderer_media.cpp
    src/render/d3d11_navigation.cpp src/render/text_edit_layout.cpp src/render/text_form_surface.cpp
    src/render/forum_post_surface.cpp src/render/message_hit_test.cpp src/render/renderer_presentation.cpp
    src/render/layer_presentation.cpp src/render/renderer_media_draw_storage.cpp
    src/render/d3d11_renderer_caption.cpp "${MAICORD_UI_VERTEX_HEADER}"
    "${MAICORD_UI_PIXEL_HEADER}" "${MAICORD_GLYPH_VERTEX_HEADER}" "${MAICORD_GLYPH_PIXEL_HEADER}"
    "${MAICORD_MEDIA_VERTEX_HEADER}" "${MAICORD_MEDIA_PIXEL_HEADER}"
    LIBS maicord_media_core maicord_font maicord_paint_program maicord_shader_bytecode
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}"
    DEPENDS maicord_atlas_gpu)
