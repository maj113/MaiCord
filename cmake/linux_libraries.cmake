# The static libraries of the Linux build (cmake/libraries.cmake lists the Windows ones). Each source list is the
# archive's member order; the Linux-only platform files live in src/core/linux.

include(cmake/linux_generated_sources.cmake)

# The core: memory, byte, UTF-8 and JSON primitives, and the Linux side of the platform files the Windows build
# takes from src/core (data_path, log) and src/core's Win32 sources (src/core/delay_load.cpp, runtime_x64.asm and
# win32_sdk_conformance.cpp have no Linux counterpart).
maicord_library(maicord_core
    src/core/arena.cpp src/core/byte_queue.cpp src/core/byte_order.cpp src/core/linux/data_path.cpp
    src/core/json_reader.cpp src/core/json_scan.cpp src/core/reserved_bytes.cpp src/core/stable_storage.cpp
    src/core/utf.cpp src/core/byte_range.cpp src/core/linux/log.cpp src/core/linux/path.cpp
    src/core/linux/shared_library.cpp src/core/linux/resource.cpp
    # The Linux bodies of the neutral os layer (include/maicord/os): address space, SRW locks, waits, threads, files
    # and directories, ordinal compares, the local time zone and date formatting (the TZif reader is pure).
    src/os/linux/pages.cpp src/os/linux/srwlock.cpp src/os/linux/wait.cpp src/os/linux/thread.cpp
    src/os/linux/file.cpp src/os/linux/directory.cpp src/os/linux/ordinal.cpp src/os/linux/local_time.cpp
    src/os/linux/date_format.cpp src/os/linux/process.cpp src/os/linux/text.cpp src/platform/time/tzif.cpp)
# resource.cpp #embeds the executable's icon (the generated ICO of cmake/linux_generated_sources.cmake) and the sounds.
set_source_files_properties(src/core/linux/resource.cpp PROPERTIES
    OBJECT_DEPENDS "${MAICORD_APP_ICON}"
    COMPILE_OPTIONS "-I${MAICORD_GENERATED_DIRECTORY};-Wno-c23-extensions")

# What the desktop shell asks the system for: the time zone identifier, the spell checker (Hunspell or Enchant
# through dlopen; spell_checker.cpp holds the language choice the two systems share), opening a link or a file and the
# launch's arguments as the command line the launch parser reads.
maicord_library(maicord_desktop_system
    src/platform/desktop/linux/timezone.cpp src/platform/desktop/spell_checker.cpp
    src/platform/desktop/linux/spell_checker.cpp src/platform/desktop/linux/shell_open.cpp
    src/platform/desktop/linux/launch_line.cpp
    LIBS maicord_core maicord_dbus)

maicord_library(maicord_compression
    src/media/compression/deflate.cpp src/media/compression/zstd.cpp src/media/compression/inflated_block.cpp
    LIBS maicord_core)

maicord_library(maicord_media_core
    src/media/core/animation_timeline.cpp src/media/core/encoded_cache.cpp src/media/core/presentation_policy.cpp
    src/media/image/regional_flag_art.cpp src/ui/text/regional_flag.cpp
    LIBS maicord_core)

# maiopus (third_party/opus): the Opus decoder, and the voice encoder, as C under the same freestanding contract:
# only MaiCord's C library and intrinsic headers, no unwind tables, whole-program optimization.
set(MAIOPUS_DECODER_TARGET maicord_opus_decoder)
set(MAIOPUS_ENCODER_TARGET maicord_opus_encoder)
set(MAIOPUS_LINK_CONTRACT maicord_link_contract)
set(MAIOPUS_LTCG ON)
set(MAIOPUS_LTCG_OPTION ${MAICORD_WHOLE_PROGRAM_OPTION})
set(MAIOPUS_ARCH -mavx)
set(MAIOPUS_SSE4_1 ON)
add_subdirectory(third_party/opus)
foreach(maiopus_target IN ITEMS maicord_opus_decoder maicord_opus_encoder)
    target_include_directories(${maiopus_target} PRIVATE include include/maicord/sys/libc)
    target_compile_options(${maiopus_target} PRIVATE -nostdinc -fPIE -fvisibility=hidden -fno-stack-protector
        -fcf-protection=none -ffp-contract=off ${MAICORD_NO_OUTLINE_OPTION})
    if(NOT MAICORD_UNWIND_TABLES)
        target_compile_options(${maiopus_target} PRIVATE -fno-asynchronous-unwind-tables -fno-unwind-tables)
    endif()
endforeach()

maicord_library(maicord_media_codec
    src/media/audio/opus_decoder.cpp src/media/containers/ogg_opus.cpp src/media/containers/ogg_opus_writer.cpp
    src/media/audio/wave_pcm.cpp src/media/audio/mp3_stream.cpp src/media/image/apng.cpp src/media/image/gif.cpp
    src/media/video/h264_parameters.cpp src/media/video/h264_configuration.cpp src/media/video/h264_slice.cpp
    src/media/video/h264_access_unit.cpp src/media/video/h264_output_queue.cpp src/media/video/h264_picture_order.cpp
    src/media/video/h264_references.cpp src/media/video/h264_stream.cpp src/media/video/hevc_parameters.cpp
    src/media/video/hevc_picture.cpp src/media/containers/hevc_mp4_demux.cpp src/media/video/hevc_references.cpp
    src/media/image/jpeg.cpp src/media/image/jpeg_icc.cpp src/media/image/jpeg_entropy.cpp
    src/media/image/jpeg_reconstruct.cpp src/media/image/lossy_webp.cpp src/media/containers/mp4_demux.cpp
    src/media/containers/mp4_audio_demux.cpp src/media/core/progressive_source.cpp src/media/image/png.cpp
    src/media/image/png_stream.cpp src/media/image/png_encode.cpp src/media/image/png_common.cpp
    src/media/image/vp8.cpp src/media/image/webp.cpp src/media/image/webp_thumbnail.cpp
    src/media/image/webp_alpha.cpp src/media/image/animated_webp.cpp src/media/video/rtp_h264.cpp
    src/media/video/rtp_h265.cpp src/media/video/hevc_stream.cpp src/media/video/rtp_av1.cpp src/media/video/av1.cpp
    src/media/containers/mp4_fragment.cpp src/media/containers/mp4_fragment_track.cpp
    src/media/containers/mp4_fragment_source.cpp src/media/containers/mp4_fragment_audio.cpp
    src/media/containers/mp4_fragment_segment.cpp src/media/containers/mp4_segment_store.cpp
    src/media/containers/mpeg_ts.cpp src/media/containers/mpeg_ts_codec.cpp
    src/media/containers/mpeg_ts_segment_store.cpp src/media/audio/mp3_decoder.c
    LIBS maicord_core maicord_compression maicord_opus_decoder)
# The MPEG audio decoder is C: the contract's code generation and warnings, for C (LTO comes from the contract).
target_compile_options(maicord_media_codec PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-mavx;-Wno-missing-braces>")

# Voice send encoder; linked only through the account module.
maicord_library(maicord_voice_encoder src/media/audio/opus_encoder.cpp LIBS maicord_core maicord_opus_encoder)

maicord_library(maicord_notification_pcm src/media/audio/notification_pcm.cpp
    LIBS maicord_core maicord_compression maicord_media_codec)

maicord_library(maicord_unicode
    src/text/scripts/arabic_joining.cpp src/text/unicode/line_break.cpp src/text/unicode/thai_word_break.cpp
    src/text/unicode/thai_lexical.cpp src/text/unicode/thai_syllable.cpp src/text/unicode/bidi_tailored.cpp
    src/text/unicode/unicode.cpp src/text/unicode/canonical.cpp src/text/unicode/grapheme.cpp src/text/unicode/bidi.cpp
    LIBS maicord_core maicord_compression maicord_media_codec)
# unicode.cpp and thai_lexical.cpp #embed their tables.
target_compile_options(maicord_unicode PRIVATE -Wno-c23-extensions)

# The font store, the catalog check and the handle stamp read the file system through the files seam
# (include/maicord/os/file.hpp); handle_stat.cpp has a Linux twin under src/text/fonts/linux.
maicord_library(maicord_font
    src/text/scripts/arabic_shape_run.cpp src/text/scripts/arabic_marks.cpp src/text/scripts/arabic_plan.cpp
    src/text/scripts/devanagari_plan.cpp src/text/scripts/syllabic_plan.cpp src/text/scripts/syllabic_shape.cpp
    src/text/shaping/cluster_run.cpp
    src/text/scripts/devanagari_forms.cpp src/text/shaping/script_paragraphs.cpp src/text/scripts/indic_run_plan.cpp
    src/text/scripts/indic_syllables.cpp src/text/scripts/indic_shape.cpp src/text/shaping/compact_run.cpp
    src/text/opentype/tt_orientation.cpp
    src/text/opentype/tt_bold_points.cpp src/text/fonts/session.cpp src/text/fonts/prepare.cpp
    src/text/opentype/gpos_cursive_execute.cpp src/text/opentype/position_anchor.cpp
    src/text/opentype/gpos_mark_execute.cpp src/text/opentype/gpos_ligature_execute.cpp
    src/text/opentype/gpos_execute.cpp src/text/opentype/gpos_program.cpp src/text/opentype/lookup_selection.cpp
    src/text/opentype/position_flags.cpp src/text/opentype/gpos_attachment.cpp src/text/opentype/gpos.cpp
    src/text/shaping/clusters.cpp src/text/opentype/components.cpp src/text/shaping/shape_run.cpp
    src/text/shaping/run_plan.cpp src/text/fonts/names.cpp src/text/fonts/match.cpp src/text/fonts/candidates.cpp
    src/text/fonts/family.cpp
    src/text/fonts/catalog_coverage.cpp src/text/fonts/fallback_range.cpp src/text/fonts/fallback_route.cpp
    src/text/fonts/fallback_map.cpp src/text/fonts/run.cpp src/text/scripts/arabic_run_plan.cpp
    src/text/scripts/arabic_run.cpp src/text/scripts/arabic_paragraph.cpp src/text/opentype/gsub_execute.cpp
    src/text/opentype/glyph_buffer.cpp src/text/opentype/context.cpp src/text/opentype/gsub.cpp
    src/text/opentype/layout_directory.cpp src/text/opentype/lookup_plan.cpp
    src/text/opentype/nominal_participation.cpp src/text/opentype/nominal_sources.cpp src/text/scripts/ascii_run.cpp
    src/text/scripts/latin_run.cpp src/text/shaping/run_profile.cpp
    src/text/scripts/emoji_run.cpp src/text/scripts/emoji_line.cpp src/text/scripts/emoji_paragraph.cpp
    src/text/layout/visibility.cpp src/text/unicode/script_class.cpp src/text/layout/unicode_line_geometry.cpp
    src/text/scripts/latin_line.cpp src/text/scripts/latin_paragraph.cpp src/text/scripts/ascii_line.cpp
    src/text/scripts/ascii_paragraph.cpp src/text/layout/paragraph_hits.cpp src/text/layout/paragraph_ranges.cpp
    src/text/fonts/metrics.cpp src/text/opentype/layout_variations.cpp src/text/opentype/gdef.cpp
    src/text/opentype/layout_tables.cpp src/text/opentype/sfnt.cpp src/text/opentype/sfnt_cmap.cpp
    src/text/opentype/truetype_outline.cpp src/text/opentype/cff_font.cpp src/text/opentype/cff_charset.cpp
    src/text/opentype/cff_outline.cpp src/text/opentype/cpal.cpp src/text/opentype/colr_font.cpp
    src/text/opentype/colr_paint_reader.cpp src/text/opentype/variation_store.cpp
    src/text/opentype/metric_variations.cpp src/text/opentype/gvar.cpp src/text/fonts/geometry.cpp
    src/text/fonts/instance.cpp src/text/opentype/tt_interpreter.cpp
    src/text/opentype/tt_outline.cpp src/text/opentype/colr_variations.cpp src/text/fonts/axes.cpp
    src/text/fonts/store.cpp src/text/fonts/catalog.cpp src/text/fonts/linux/handle_stat.cpp
    LIBS maicord_core maicord_unicode)
# Compact the shared Indic ordering routine; keep -O2 and LTO.
set_property(SOURCE APPEND PROPERTY COMPILE_OPTIONS ${MAICORD_SIZE_OPTION})
# Font table opening, outlines and variations (once per font or glyph), the non-Latin script shapers and the Unicode
# line geometry keep their short loops rolled, as in the Windows build (cmake/libraries.cmake has the measurements).
get_target_property(maicord_rolled_sources maicord_font SOURCES)
list(FILTER maicord_rolled_sources INCLUDE REGEX
    "/(opentype/(sfnt|cff_|tt_|truetype_outline|colr_|cpal|gvar|variation_store|metric_variations|layout_directory|layout_tables|gdef|layout_variations|lookup_selection|nominal_)|scripts/hebrew|layout/unicode_line_geometry)")
set_property(SOURCE ${maicord_rolled_sources} APPEND PROPERTY COMPILE_OPTIONS -fno-unroll-loops)

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

maicord_library(maicord_protocol
    src/protocol/messages/poll.cpp src/protocol/gateway/message_poll.cpp src/protocol/auth/browser_sign_in.cpp
    src/protocol/auth/captcha_bridge_page.cpp src/protocol/encoding/base64.cpp src/protocol/encoding/base64_codec.cpp
    src/protocol/encoding/base64url.cpp src/protocol/media/cdn_url.cpp src/protocol/media/attachment_refresh.cpp
    src/protocol/media/attachment_upload.cpp src/protocol/media/upload_allowance.cpp
    src/protocol/http/https_upload.cpp src/protocol/media/cdn_http.cpp src/protocol/http/rest.cpp
    src/protocol/encoding/json_ids.cpp src/protocol/http/rest_route.cpp src/protocol/gateway/commands.cpp
    src/protocol/gateway/endpoint.cpp src/protocol/gateway/events.cpp src/protocol/auth/account_requirement.cpp
    src/protocol/gateway/voice.cpp src/protocol/gateway/stream.cpp src/protocol/guild/stage_channel.cpp
    src/protocol/user/social.cpp src/protocol/guild/community.cpp src/protocol/voice/gateway.cpp
    src/protocol/voice/discovery.cpp src/protocol/voice/media_packet.cpp src/protocol/voice/media_events.cpp
    src/protocol/voice/participants.cpp src/protocol/voice/dave_replay.cpp src/protocol/voice/dave_frame.cpp
    src/protocol/auth/p256_signature.cpp src/protocol/voice/mls_objects.cpp src/protocol/voice/mls_objects_write.cpp
    src/protocol/voice/heartbeat.cpp src/protocol/voice/session.cpp src/protocol/http/websocket_message.cpp
    src/protocol/gateway/lifecycle.cpp src/protocol/gateway/message_events.cpp
    src/protocol/gateway/message_validation.cpp src/protocol/gateway/reaction_events.cpp
    src/protocol/gateway/typing_events.cpp src/protocol/gateway/message_rich.cpp
    src/protocol/gateway/message_rich_reference.cpp src/protocol/gateway/message_rich_attachments.cpp
    src/protocol/gateway/message_rich_embeds.cpp src/protocol/messages/interaction.cpp
    src/protocol/gateway/payload.cpp src/protocol/gateway/runtime.cpp src/protocol/gateway/session.cpp
    src/protocol/http/http1.cpp src/protocol/http/http_response_stream.cpp src/protocol/media/attachment_download.cpp
    src/protocol/encoding/json_write.cpp src/protocol/messages/history.cpp src/protocol/messages/ack.cpp
    src/protocol/messages/bulk_message_ack.cpp src/protocol/messages/create.cpp src/protocol/messages/delete.cpp
    src/protocol/user/private_conversation.cpp src/protocol/messages/edit.cpp src/protocol/messages/reaction.cpp
    src/protocol/user/profile_decode.cpp src/protocol/gateway/activity_decode.cpp
    src/protocol/messages/reaction_users.cpp src/protocol/messages/typing.cpp src/protocol/auth/remote_auth.cpp
    src/protocol/auth/user_auth.cpp src/protocol/auth/webauthn_mfa.cpp src/protocol/http/websocket_frame.cpp
    src/protocol/http/websocket_handshake.cpp src/protocol/messages/application_commands.cpp
    src/protocol/encoding/writer.cpp src/protocol/auth/login_continuations.cpp src/protocol/user/relationships.cpp
    src/protocol/messages/search.cpp src/protocol/messages/pins.cpp src/protocol/messages/thread_actions.cpp
    src/protocol/messages/thread_browser.cpp src/protocol/messages/thread_creation.cpp
    src/protocol/messages/forum_post.cpp src/protocol/media/media_catalog.cpp src/protocol/gateway/presence.cpp
    src/protocol/gateway/client_status.cpp src/protocol/user/self_status_settings.cpp src/protocol/guild/folders.cpp
    src/protocol/guild/invite.cpp src/protocol/user/settings_proto.cpp src/protocol/user/account_settings.cpp
    src/protocol/user/emoji_preferences.cpp src/protocol/user/gif_favorites.cpp
    src/protocol/guild/channel_overview.cpp src/protocol/guild/admin.cpp src/protocol/guild/admin_decode.cpp
    src/protocol/guild/assets.cpp src/protocol/user/profile_edit.cpp src/protocol/voice/video_signaling.cpp
    src/protocol/voice/rtcp_feedback.cpp src/protocol/guild/soundboard.cpp
    src/protocol/user/notification_settings.cpp src/protocol/user/guild_settings.cpp
    src/protocol/media/cdn_icon_hash.cpp src/protocol/guild/moderation.cpp src/protocol/voice/channel_info.cpp
    src/protocol/media/embed_provider.cpp src/protocol/media/embed_player.cpp
    src/protocol/media/embed_player_video.cpp src/protocol/media/embed_player_audio.cpp
    src/protocol/media/dash_manifest.cpp src/protocol/media/steam_player.cpp src/protocol/auth/oauth.cpp
    src/protocol/media/hls_playlist.cpp src/protocol/media/twitch_playback.cpp src/protocol/http/cookie_jar.cpp
    src/protocol/gateway/qos_token.cpp src/protocol/gateway/etf.cpp src/protocol/messages/interaction_data.cpp
    LIBS maicord_core maicord_compression maicord_media_core)

maicord_library(maicord_edit_history
    src/model/chat/edit_history.cpp
    LIBS maicord_core maicord_crypto)

# Left out until the network seam lands: src/app/voice/dave/connection_dave.cpp (it names the voice socket and its TLS
# stream, so it includes platform/net/network.hpp, which is built on Winsock types).
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
    src/model/guild/invite_card_cache.cpp src/model/guild/scheduled_event_store.cpp
    src/model/chat/message_composer.cpp src/model/chat/message_composer_input_target.cpp
    src/model/chat/message_draft_store.cpp src/model/chat/reaction_preferences.cpp src/model/chat/message_store.cpp
    src/model/chat/message_search.cpp src/model/notifications/message_notifications.cpp
    src/model/voice/state_store.cpp src/model/voice/stage_instance_store.cpp src/model/voice/stage_policy.cpp
    src/model/user/image_crop.cpp src/model/user/profile_editor.cpp src/app/voice/mls/tree_math.cpp
    src/app/voice/mls/ratchet_tree.cpp src/app/voice/mls/tree_validate.cpp src/app/voice/mls/tree_edit.cpp
    src/app/voice/mls/treekem.cpp src/app/voice/mls/group_context.cpp src/app/voice/mls/proposal.cpp
    src/app/voice/mls/commit.cpp src/app/voice/mls/welcome.cpp src/app/voice/mls/group.cpp
    src/app/voice/dave/session.cpp src/app/voice/log.cpp
    src/model/notifications/desktop_notification_queue.cpp src/model/notifications/sound_policy.cpp
    src/model/notifications/sound_admission.cpp src/model/notifications/retention.cpp
    src/model/guild/permission_grid.cpp src/model/guild/settings_model.cpp src/model/guild/channel_settings_model.cpp
    src/model/guild/detail_store.cpp src/model/guild/member_search.cpp src/model/voice/channel_info_store.cpp
    LIBS maicord_core maicord_edit_history maicord_protocol maicord_crypto)

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

# The OpenGL ES GPU backend (src/gpu/gles).
include(cmake/linux_gpu.cmake)

# The shared renderer (src/render) over the GLES backend.
include(cmake/linux_render.cmake)

# The Wayland window backend (src/platform/window/wayland).
include(cmake/linux_window.cmake)

# The audio backend (src/platform/audio/pulse): libpulse behind the platform audio interfaces.
include(cmake/linux_audio.cmake)

# The in-house D-Bus client and the desktop services over it (src/platform/dbus).
include(cmake/linux_dbus.cmake)

# The .desktop file and icons a package installs (tools/generate_desktop_entry.py).
include(cmake/linux_packaging.cmake)

# The application transport and updater libraries (src/app/net, src/app/gateway, src/app/update).
include(cmake/linux_app_libraries.cmake)

# Live video decode (src/platform/decode): the software tier over libdav1d and libopenh264, VA-API, and the stream
# decoder that drives them.
include(cmake/linux_video.cmake)
# The rest of the application (src/app outside the shell) and the secure text field.
include(cmake/linux_app_client.cmake)
