# The static libraries both clients link (MaiCord.exe and the Linux MaiCord). Each source list is the archive's member
# order (and so the link order); keep it stable when adding files. An entry prefixed windows: or linux: belongs to that
# system's build only, in place (maicord_library, cmake/build_contract.cmake); a library one system alone builds is in
# cmake/windows_libraries.cmake or cmake/linux_libraries.cmake, included at the end. Headers are found through the
# include paths and are not listed.

maicord_library(maicord_core
    src/core/arena.cpp src/core/byte_queue.cpp src/core/byte_order.cpp windows:src/core/data_path.cpp
    linux:src/core/linux/data_path.cpp src/core/json_reader.cpp src/core/json_scan.cpp src/core/reserved_bytes.cpp
    src/core/stable_storage.cpp src/core/utf.cpp src/core/byte_range.cpp windows:src/core/delay_load.cpp
    # Linux: the system side of what Windows takes from src/core (log, paths, shared libraries, resources), and the
    # bodies of the neutral os layer (include/maicord/os): address space, SRW locks, waits, threads, files and
    # directories, ordinal compares, the local time zone and date formatting (the TZif reader is pure).
    linux:src/core/linux/log.cpp linux:src/core/linux/path.cpp linux:src/core/linux/shared_library.cpp linux:src/core/linux/crc32.cpp
    linux:src/core/linux/resource.cpp linux:src/os/linux/pages.cpp linux:src/os/linux/srwlock.cpp
    linux:src/os/linux/wait.cpp linux:src/os/linux/thread.cpp linux:src/os/linux/file.cpp
    linux:src/os/linux/directory.cpp linux:src/os/linux/ordinal.cpp linux:src/os/linux/local_time.cpp
    linux:src/os/linux/date_format.cpp linux:src/os/linux/process.cpp linux:src/os/linux/text.cpp
    linux:src/platform/time/tzif.cpp)

maicord_library(maicord_compression src/media/compression/deflate.cpp src/media/compression/zstd.cpp
    src/media/compression/inflated_block.cpp LIBS maicord_core)

maicord_library(maicord_media_core
    src/media/core/animation_timeline.cpp src/media/core/encoded_cache.cpp src/media/core/presentation_policy.cpp
    src/media/image/regional_flag_art.cpp src/media/image/dominant_color.cpp src/ui/text/regional_flag.cpp
    LIBS maicord_core)

# maiopus (third_party/opus): the Opus decoder, and the voice encoder, as C under the same freestanding contract: like
# every MaiCord unit it sees only MaiCord's C library and intrinsic headers and follows the unwind-table and no-outline
# policies (see cmake/build_contract.cmake), with whole-program optimization.
set(MAIOPUS_DECODER_TARGET maicord_opus_decoder)
set(MAIOPUS_ENCODER_TARGET maicord_opus_encoder)
set(MAIOPUS_LINK_CONTRACT maicord_link_contract)
set(MAIOPUS_LTCG ON)
set(MAIOPUS_LTCG_OPTION ${MAICORD_WHOLE_PROGRAM_OPTION})
set(MAIOPUS_SSE4_1 ON)
if(MAICORD_LINUX)
    set(MAIOPUS_ARCH -march=sandybridge -mtune=generic)
    set(maicord_opus_options -nostdinc -fPIE -fvisibility=hidden -fno-stack-protector -fcf-protection=none
        -ffp-contract=off ${MAICORD_NO_OUTLINE_OPTION} ${MAICORD_UNWIND_OPTIONS})
else()
    set(MAIOPUS_ARCH /arch:AVX)
    set(maicord_opus_options /clang:-nostdinc ${MAICORD_UNWIND_OPTIONS} ${MAICORD_NO_OUTLINE_OPTION})
endif()
add_subdirectory(third_party/opus)
foreach(maiopus_target IN ITEMS maicord_opus_decoder maicord_opus_encoder)
    target_include_directories(${maiopus_target} PRIVATE include include/maicord/sys/libc)
    target_compile_options(${maiopus_target} PRIVATE ${maicord_opus_options})
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
    src/media/video/rtp_nal.cpp src/media/video/hevc_stream.cpp src/media/video/rtp_video_packetize.cpp
    src/media/video/rtp_av1.cpp src/media/video/av1.cpp src/media/containers/mp4_fragment.cpp src/media/containers/mp4_fragment_track.cpp
    src/media/containers/mp4_fragment_source.cpp
    src/media/containers/mp4_fragment_audio.cpp
    src/media/containers/mp4_fragment_segment.cpp
    src/media/containers/mp4_segment_store.cpp
    src/media/containers/mpeg_ts.cpp
    src/media/containers/mpeg_ts_codec.cpp
    src/media/containers/mpeg_ts_segment_store.cpp
    src/media/audio/mp3_decoder.c
    LIBS maicord_core maicord_compression maicord_opus_decoder windows:ntdll)
# The MPEG audio decoder is C: the contract's C++ code generation and warnings,
# for C (LTO comes from the contract), and its hint-only inlining (/Ob1): on Linux
# 0.9 KB smaller and its decode 0.9-1.8% slower, which the size cap takes.
if(MAICORD_LINUX)
    target_compile_options(maicord_media_codec PRIVATE
        "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-march=sandybridge;-mtune=generic;-finline-hint-functions;-Wno-missing-braces>")
else()
    target_compile_options(maicord_media_codec PRIVATE
        "$<$<COMPILE_LANGUAGE:C>:/W4;/WX;/Zl;/GS-;/O2;/Ob1;/arch:AVX;/fp:precise;/Gy;/Gw;/GF;/Brepro;-Wno-missing-braces>")
endif()

# The Go Live encoder (include/maicord/platform/encode/gpu_stream_encoder.hpp): hardware Media Foundation transforms
# on Windows, VA-API on the decoders' display on Linux; the rewriting of the encoders' units into the form the stream
# sender takes is shared.
maicord_library(maicord_video_encode
    src/platform/encode/access_unit.cpp src/platform/encode/av1_headers.cpp
    linux:src/platform/encode/linux/gpu_stream_encoder.cpp
    windows:src/platform/encode/gpu_stream_encoder.cpp
    LIBS maicord_core maicord_media_codec windows:maicord_gpu_d3d11 windows:d3d11 windows:ole32 linux:maicord_video_vaapi
        linux:maicord_gpu_gles)

# Go Live capture (include/maicord/platform/capture/screen_capture.hpp, platform/audio/stream_audio.hpp): screens and
# windows through Windows.Graphics.Capture and DXGI Desktop Duplication and stream audio through WASAPI process loopback
# on Windows; the ScreenCast portal over the D-Bus client and PipeWire (dlopen()ed: dma-bufs, and the other
# applications' playback for stream audio) on Linux. See src/platform/capture/README.md.
maicord_library(maicord_capture
    windows:src/platform/capture/windows/screen_capture.cpp windows:src/platform/capture/windows/stream_audio.cpp
    linux:src/platform/capture/linux/pipewire.cpp linux:src/platform/capture/linux/screen_capture.cpp
    linux:src/platform/capture/linux/stream_audio.cpp
    LIBS maicord_core windows:d3d11 windows:dxgi windows:user32 windows:gdi32 windows:ole32 windows:maicord_combase
    linux:maicord_dbus)

# Voice send encoder; linked only through the account module.
maicord_library(maicord_voice_encoder src/media/audio/opus_encoder.cpp LIBS maicord_core maicord_opus_encoder)

# The headless window backend (src/platform/window/null) behind the same service as the system's, for tests that drive
# handlers without a window: list it before the system backend (maicord_platform on Windows, maicord_window_wayland on
# Linux) in LIBS so its definitions win, as an archive member only provides them when still undefined.
maicord_library(maicord_window_null src/platform/window/null/window.cpp
    LIBS maicord_core)

maicord_library(maicord_notification_pcm src/media/audio/notification_pcm.cpp
    LIBS maicord_core maicord_compression maicord_media_codec)

maicord_library(maicord_protocol
    src/protocol/messages/poll.cpp src/protocol/gateway/message_poll.cpp src/protocol/auth/browser_sign_in.cpp
    src/protocol/auth/captcha_bridge_page.cpp src/protocol/encoding/base64.cpp src/protocol/encoding/base64_codec.cpp
    src/protocol/encoding/base64url.cpp src/protocol/media/cdn_url.cpp src/protocol/media/attachment_refresh.cpp
    src/protocol/media/attachment_upload.cpp src/protocol/media/upload_allowance.cpp src/protocol/http/https_upload.cpp
    src/protocol/media/cdn_http.cpp src/protocol/http/rest.cpp src/protocol/encoding/json_ids.cpp
    src/protocol/http/rest_route.cpp src/protocol/gateway/commands.cpp src/protocol/gateway/endpoint.cpp
    src/protocol/gateway/events.cpp src/protocol/auth/account_requirement.cpp src/protocol/gateway/voice.cpp
    src/protocol/gateway/stream.cpp src/protocol/guild/stage_channel.cpp src/protocol/user/social.cpp
    src/protocol/guild/community.cpp src/protocol/guild/community_admin.cpp src/protocol/voice/gateway.cpp
    src/protocol/voice/discovery.cpp
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
    src/protocol/user/profile_connection.cpp
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
    src/protocol/user/frecency_wire.cpp
    src/protocol/user/gif_favorites.cpp src/protocol/guild/channel_overview.cpp src/protocol/guild/admin.cpp
    src/protocol/guild/admin_decode.cpp src/protocol/guild/game_profile.cpp
    src/protocol/guild/assets.cpp src/protocol/user/profile_edit.cpp src/protocol/voice/video_signaling.cpp
    src/protocol/voice/rtcp_feedback.cpp src/protocol/guild/soundboard.cpp src/protocol/user/notification_settings.cpp
    src/protocol/voice/rtcp_sender.cpp src/protocol/voice/dave_codec.cpp
    src/protocol/user/guild_settings.cpp
    # Icon hash check kept apart from cdn_url.cpp: the client keeps it (Gateway
    # user records) while it forwards every other CDN URL function to the module.
    src/protocol/media/cdn_icon_hash.cpp src/protocol/guild/moderation.cpp src/protocol/voice/channel_info.cpp
    src/protocol/guild/automation.cpp
    src/protocol/media/embed_provider.cpp
    src/protocol/media/embed_player.cpp
    src/protocol/media/embed_player_video.cpp
    src/protocol/media/embed_player_audio.cpp
    src/protocol/media/dash_manifest.cpp
    src/protocol/media/steam_player.cpp
    src/protocol/media/hls_playlist.cpp
    src/protocol/media/twitch_playback.cpp
    src/protocol/http/cookie_jar.cpp
    src/protocol/gateway/qos_token.cpp
    src/protocol/gateway/etf.cpp
    src/protocol/messages/interaction_data.cpp
    LIBS maicord_core maicord_compression maicord_media_core)

maicord_library(maicord_unicode
    src/text/scripts/arabic_joining.cpp src/text/unicode/line_break.cpp
    src/text/unicode/thai_word_break.cpp src/text/unicode/thai_lexical.cpp src/text/unicode/thai_syllable.cpp
    src/text/unicode/bidi_tailored.cpp src/text/unicode/unicode.cpp src/text/unicode/canonical.cpp src/text/unicode/grapheme.cpp src/text/unicode/bidi.cpp
    # unicode.cpp and thai_lexical.cpp inflate their tables.
    LIBS maicord_core maicord_compression)
if(MAICORD_LINUX)
    # The tables are #embedded (a C23 extension to clang's C++).
    target_compile_options(maicord_unicode PRIVATE -Wno-c23-extensions)
endif()

maicord_library(maicord_font
    src/text/scripts/arabic_shape_run.cpp src/text/scripts/arabic_marks.cpp src/text/scripts/arabic_plan.cpp
    src/text/scripts/devanagari_plan.cpp src/text/scripts/syllabic_plan.cpp src/text/scripts/syllabic_shape.cpp
    src/text/shaping/cluster_run.cpp src/text/scripts/devanagari_forms.cpp
    src/text/shaping/script_paragraphs.cpp src/text/scripts/indic_run_plan.cpp
    src/text/scripts/indic_syllables.cpp src/text/scripts/indic_shape.cpp src/text/shaping/compact_run.cpp
    src/text/opentype/tt_orientation.cpp
    src/text/opentype/tt_bold_points.cpp src/text/fonts/session.cpp src/text/fonts/prepare.cpp
    src/text/opentype/gpos_cursive_execute.cpp src/text/opentype/position_anchor.cpp src/text/opentype/gpos_mark_execute.cpp
    src/text/opentype/gpos_ligature_execute.cpp src/text/opentype/gpos_execute.cpp src/text/opentype/gpos_program.cpp
    src/text/opentype/lookup_selection.cpp src/text/opentype/position_flags.cpp
    src/text/opentype/gpos_attachment.cpp src/text/opentype/gpos.cpp src/text/shaping/clusters.cpp src/text/opentype/components.cpp
    src/text/shaping/shape_run.cpp src/text/shaping/run_plan.cpp src/text/fonts/names.cpp src/text/fonts/match.cpp
    src/text/fonts/candidates.cpp src/text/fonts/family.cpp windows:src/text/fonts/catalog.cpp
    windows:src/text/fonts/handle_stat.cpp src/text/fonts/catalog_coverage.cpp src/text/fonts/fallback_range.cpp
    src/text/fonts/fallback_route.cpp src/text/fonts/fallback_map.cpp
    src/text/fonts/run.cpp src/text/scripts/arabic_run_plan.cpp src/text/scripts/arabic_run.cpp
    src/text/scripts/arabic_paragraph.cpp src/text/opentype/gsub_execute.cpp src/text/opentype/glyph_buffer.cpp src/text/opentype/context.cpp
    src/text/opentype/gsub.cpp src/text/opentype/layout_directory.cpp src/text/opentype/lookup_plan.cpp src/text/opentype/nominal_participation.cpp
    src/text/opentype/nominal_sources.cpp src/text/scripts/ascii_run.cpp src/text/scripts/latin_run.cpp
    src/text/shaping/run_profile.cpp src/text/scripts/emoji_run.cpp src/text/scripts/emoji_line.cpp
    src/text/scripts/emoji_paragraph.cpp src/text/layout/visibility.cpp src/text/unicode/script_class.cpp
    src/text/layout/unicode_line_geometry.cpp src/text/scripts/latin_line.cpp src/text/scripts/latin_paragraph.cpp
    src/text/scripts/ascii_line.cpp src/text/scripts/ascii_paragraph.cpp src/text/layout/paragraph_hits.cpp
    src/text/layout/paragraph_ranges.cpp src/text/fonts/metrics.cpp src/text/opentype/layout_variations.cpp src/text/opentype/gdef.cpp
    src/text/opentype/layout_tables.cpp src/text/opentype/sfnt.cpp src/text/opentype/sfnt_cmap.cpp src/text/opentype/truetype_outline.cpp
    src/text/opentype/cff_font.cpp src/text/opentype/cff_charset.cpp src/text/opentype/cff_outline.cpp src/text/opentype/cpal.cpp
    src/text/opentype/colr_font.cpp src/text/opentype/colr_paint_reader.cpp
    src/text/opentype/variation_store.cpp src/text/opentype/metric_variations.cpp src/text/opentype/gvar.cpp src/text/fonts/geometry.cpp
    windows:src/text/fonts/store.cpp src/text/fonts/instance.cpp src/text/opentype/tt_interpreter.cpp
    src/text/opentype/tt_outline.cpp src/text/opentype/colr_variations.cpp src/text/fonts/axes.cpp
    # The font store, the catalog check and the handle stamp read the file system through the files seam
    # (include/maicord/os/file.hpp); handle_stat.cpp has a Linux twin.
    linux:src/text/fonts/store.cpp linux:src/text/fonts/catalog.cpp linux:src/text/fonts/linux/handle_stat.cpp
    LIBS maicord_core maicord_unicode windows:ntdll)
# Font table opening, outlines and variations (once per font or glyph), the
# non-Latin script shapers and the Unicode line geometry keep their short loops
# rolled: unrolled they only bloat (text benchmark: cycles_ascii -9%, Latin,
# Arabic and Devanagari within 1%). The lookup executors, the Latin, ASCII,
# Arabic and Devanagari shapers and the paragraph layout keep unrolling: without
# it message_hit_rebuild was 3-9% slower and cycles_arabic 2.4%.
get_target_property(maicord_rolled_sources maicord_font SOURCES)
list(FILTER maicord_rolled_sources INCLUDE REGEX
    "/(opentype/(sfnt|cff_|tt_|truetype_outline|colr_|cpal|gvar|variation_store|metric_variations|layout_directory|layout_tables|gdef|layout_variations|lookup_selection|nominal_)|scripts/hebrew|layout/unicode_line_geometry)")
set_property(SOURCE ${maicord_rolled_sources} APPEND PROPERTY COMPILE_OPTIONS ${maicord_clang}-fno-unroll-loops)

maicord_library(maicord_render_core
    src/render/media/animation_frame_cache.cpp src/render/text/glyph_atlas.cpp src/render/text/glyph_quad_bounds.cpp
    src/render/text/glyph_raster.cpp src/render/media/atlas.cpp src/render/d3d11/presentation_damage.cpp
    LIBS maicord_core)

# The vector geometry engine of the Lottie compiler: paths, transforms, boolean combinations, areas and strokes.
maicord_library(maicord_vector_geometry
    src/render/effects/vector_geometry.cpp src/render/effects/vector_overlay.cpp src/render/effects/vector_stroke.cpp
    LIBS maicord_core)

maicord_library(maicord_paint_program
    src/render/effects/colr_program.cpp src/render/effects/colr_source.cpp src/render/effects/regional_flag_program.cpp
    src/render/effects/lottie_program.cpp
    LIBS maicord_vector_geometry maicord_core maicord_render_core maicord_font maicord_media_core)

maicord_library(maicord_ui
    src/ui/chat/polls/controls.cpp src/ui/chat/polls/voters.cpp src/ui/chat/polls/presentation.cpp src/ui/voice/audio_controls.cpp
    src/ui/voice/audio_presentation.cpp src/ui/voice/video_controls.cpp src/ui/voice/video_scene.cpp src/ui/widgets/spinner.cpp
    src/ui/core/transition.cpp src/ui/widgets/tooltip.cpp windows:src/ui/media/emoji_presentation.cpp src/ui/text/ansi_text.cpp
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
    src/ui/account/profile_scene_activity.cpp src/ui/account/activity_card.cpp
    src/ui/account/profile_media.cpp src/ui/account/profile_dialog_scene.cpp src/ui/chat/messages/reaction_viewer.cpp
    src/ui/chat/messages/reaction_viewer_presentation.cpp src/ui/core/scene.cpp src/ui/kit/scene_kit.cpp src/ui/shell/navigation_scene.cpp
    src/ui/shell/presentation.cpp src/ui/shell/text_presentation.cpp src/ui/shell/notice_band.cpp src/ui/chat/private_channel_title.cpp
    src/ui/chat/private_channel_rows.cpp src/ui/chat/names_card.cpp src/ui/layout/smooth_scroll.cpp src/ui/layout/scroll_host.cpp src/ui/text/flow_layout.cpp
    src/ui/text/measure_cache.cpp src/ui/text/presentation.cpp src/ui/layout/virtual_list.cpp src/ui/core/widget_manager.cpp
    src/ui/core/widget_builder.cpp src/ui/widgets/menu_row.cpp src/ui/widgets/button_keyboard.cpp src/ui/core/input_router.cpp
    src/ui/layout/list_navigation.cpp src/ui/layout/list_paging.cpp src/ui/layout/widget_scroll.cpp src/ui/core/widget_storage.cpp
    src/ui/shell/page_registry.cpp src/ui/settings/scene.cpp src/ui/settings/scene_layout.cpp
    src/ui/settings/scene_paint.cpp src/ui/settings/theme_editor.cpp windows:src/ui/theme/styles.cpp
    src/ui/theme/style_projection.cpp src/ui/widgets/settings_choice.cpp src/ui/account/friends_scene.cpp
    src/ui/widgets/kit_fields.cpp src/ui/widgets/kit_collections.cpp src/ui/widgets/color_picker.cpp
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
    src/ui/voice/channel_rows.cpp src/ui/voice/panel.cpp src/ui/voice/go_live_scene.cpp
    src/ui/voice/call_scene.cpp src/ui/voice/call_stage.cpp src/ui/widgets/hover_card.cpp src/ui/voice/stream_card_scene.cpp
    src/ui/guild/card_scene.cpp src/ui/guild/folder_open.cpp src/ui/guild/hidden_channel_menu.cpp
    src/ui/guild/banner.cpp src/ui/chat/game_media.cpp src/ui/account/profile_activity.cpp src/ui/shell/caption_bar.cpp src/ui/shell/tab_strip.cpp src/ui/widgets/activity_status.cpp
    src/ui/chat/messages/typing_indicator.cpp src/ui/account/dm_profile_panel.cpp
    src/ui/widgets/toast.cpp windows:src/ui/theme/theme.cpp src/ui/theme/background_color_math.cpp src/ui/chat/search/message_result_card.cpp
    src/ui/text/pointer_selection.cpp src/ui/chat/messages/author.cpp src/ui/chat/messages/interaction.cpp
    src/ui/media/expression_card_scene.cpp
    # The spell tokenizer classifies letters with the Unicode property tables.
    # On Linux the text system entry points, the emoji presentation and the theme
    # typography are maicord_text_system's, the theme maicord_theme's.
    LIBS maicord_core maicord_model maicord_media_core maicord_unicode maicord_compression
        linux:maicord_theme
    INCLUDES linux:src "${MAICORD_GENERATED_DIRECTORY}"
    DEPENDS maicord_emoji_catalog_data maicord_icon_pack_data)
if(MAICORD_LINUX)
    target_compile_options(maicord_ui PRIVATE -Wno-c23-extensions)
endif()

maicord_library(maicord_edit_history src/model/chat/edit_history.cpp
    LIBS maicord_core windows:maicord_platform linux:maicord_crypto)

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
    src/model/guild/scheduled_event_store.cpp src/model/chat/message_composer.cpp src/model/chat/game_store.cpp
    src/model/chat/message_composer_input_target.cpp src/model/chat/message_draft_store.cpp
    src/model/chat/reaction_preferences.cpp src/model/chat/message_store.cpp src/model/chat/message_search.cpp
    src/model/notifications/message_notifications.cpp src/model/voice/state_store.cpp src/model/voice/stage_instance_store.cpp
    src/model/voice/stage_policy.cpp src/model/user/image_crop.cpp src/model/user/profile_editor.cpp
    src/app/voice/mls/tree_math.cpp src/app/voice/mls/ratchet_tree.cpp src/app/voice/mls/tree_validate.cpp src/app/voice/mls/tree_edit.cpp
    src/app/voice/mls/treekem.cpp src/app/voice/mls/group_context.cpp src/app/voice/mls/proposal.cpp src/app/voice/mls/commit.cpp
    src/app/voice/mls/welcome.cpp src/app/voice/mls/group.cpp src/app/voice/dave/session.cpp
    # The DAVE connection names the voice socket and its TLS stream (platform/net/network.hpp, Winsock types).
    windows:src/app/voice/dave/connection_dave.cpp
    src/app/voice/log.cpp src/app/voice/log_record.cpp
    src/model/notifications/desktop_notification_queue.cpp src/model/notifications/sound_policy.cpp
    src/model/notifications/sound_admission.cpp src/model/notifications/retention.cpp
    src/model/guild/permission_grid.cpp src/model/guild/settings_model.cpp src/model/guild/channel_settings_model.cpp
    src/model/guild/detail_store.cpp src/model/guild/member_search.cpp src/model/guild/server_tag.cpp
    src/model/guild/community_settings.cpp src/model/voice/channel_info_store.cpp
    LIBS maicord_core maicord_edit_history maicord_protocol linux:maicord_crypto)

maicord_library(maicord_atlas_gpu
    windows:src/gpu/d3d11/video_frame.cpp "windows:${MAICORD_VIDEO_FRAME_HEADER}" src/render/text/glyph_atlas_gpu.cpp
    src/render/media/atlas_gpu.cpp src/render/media/atlas_gpu_animation.cpp
    src/render/media/atlas_gpu_dispatch.cpp src/render/media/atlas_gpu_operations.cpp
    src/render/media/atlas_gpu_validation.cpp src/render/media/atlas_gpu_vp8.cpp "windows:${MAICORD_VP8_IDCT_HEADER}"
    "windows:${MAICORD_COLR_WIDE_HEADER}" "windows:${MAICORD_COLR_COMPUTE_HEADER}" "windows:${MAICORD_COLR_LAYERS_HEADER}"
    "windows:${MAICORD_GLYPH_COMPUTE_HEADER}"
    "windows:${MAICORD_PNG_COMPUTE_HEADER}" "windows:${MAICORD_VP8_RECONSTRUCT_HEADER}"
    "windows:${MAICORD_VP8_FILTER_HEADER}" "windows:${MAICORD_VP8_ATLAS_HEADER}" "windows:${MAICORD_JPEG_IDCT_HEADER}"
    "windows:${MAICORD_JPEG_IDCT_ROWS_HEADER}" "windows:${MAICORD_JPEG_ATLAS_HEADER}"
    LIBS maicord_core maicord_render_core windows:maicord_text windows:maicord_gpu_d3d11 windows:d3d11
        linux:maicord_gpu_gles
    INCLUDES linux:src "${MAICORD_GENERATED_DIRECTORY}")

maicord_library(maicord_renderer
    src/render/media/animation_frame_cache_gpu.cpp src/render/text/content_text_retention.cpp
    src/render/text/shaped_text_cache.cpp src/render/d3d11/device.cpp src/render/d3d11/draw_pipeline.cpp
    windows:src/render/effects/colr_paint.cpp src/render/d3d11/glyph_text.cpp src/render/media/icon_atlas.cpp
    src/render/text/native_glyph_outlines.cpp src/render/d3d11/background_fx.cpp
    windows:src/render/d3d11/effect_renderer.cpp windows:src/render/d3d11/effect_inputs.cpp src/render/d3d11/effect_glyphs.cpp
    src/render/d3d11/renderer.cpp src/render/d3d11/renderer_draw.cpp src/render/d3d11/renderer_edit.cpp
    src/render/d3d11/renderer_layout.cpp src/render/d3d11/renderer_layout_media.cpp src/render/d3d11/renderer_text.cpp
    src/render/d3d11/renderer_text_content.cpp src/render/d3d11/renderer_text_overlay.cpp
    src/render/d3d11/renderer_interaction.cpp src/render/d3d11/renderer_media.cpp
    src/render/d3d11/navigation.cpp src/render/text/edit_layout.cpp src/render/text/form_surface.cpp
    src/render/text/forum_post_surface.cpp src/render/text/message_hit_test.cpp src/render/d3d11/renderer_presentation.cpp
    src/render/d3d11/layer_presentation.cpp src/render/media/renderer_media_draw_storage.cpp
    src/render/d3d11/renderer_caption.cpp "windows:${MAICORD_UI_VERTEX_HEADER}"
    "windows:${MAICORD_UI_PIXEL_HEADER}" "windows:${MAICORD_GLYPH_VERTEX_HEADER}" "windows:${MAICORD_GLYPH_PIXEL_HEADER}"
    "windows:${MAICORD_MEDIA_VERTEX_HEADER}" "windows:${MAICORD_MEDIA_PIXEL_HEADER}"
    # On Linux the colour paint programs are maicord_render_colr_paint's and the effect host maicord_effect_host's.
    LIBS maicord_media_core maicord_font windows:maicord_paint_program windows:maicord_gpu_d3d11 linux:maicord_gpu_gles
        linux:maicord_core linux:maicord_atlas_gpu linux:maicord_effect_host
    INCLUDES linux:src "${MAICORD_GENERATED_DIRECTORY}"
    DEPENDS windows:maicord_atlas_gpu)

if(MAICORD_LINUX)
    include(cmake/linux_libraries.cmake)
else()
    include(cmake/windows_libraries.cmake)
endif()
