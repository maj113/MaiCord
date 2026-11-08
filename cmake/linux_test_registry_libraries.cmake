# The tests of the portable libraries (src/protocol, src/model, src/text, src/media) that the Linux build runs: the
# Linux-buildable subset of cmake/test_registry.cmake, under the same test names, targets and entries. Sources are the
# shared tests/ files, and a platform difference inside one is an #if in the test. A test is registered here when
# every source it builds is one of the libraries' (or tests/support's) and every library it links is built for Linux
# (cmake/linux_libraries.cmake); the tests of the Windows decoders, the GDI font locator and the file, network and
# window layers are not. The shared tests call a few Win32 file and console functions, which
# tests/support/win32_test_compat.hpp supplies on Linux.

maicord_test_bundle(MaiCordCoreUnitTests LIBS maicord_compression maicord_protocol maicord_media_core
    maicord_media_codec)
maicord_test_bundle(MaiCordProtocolTests LIBS maicord_core maicord_protocol maicord_media_core)
maicord_test_bundle(MaiCordCodecUnitTests LIBS maicord_core maicord_media_codec maicord_compression
    INCLUDES src/platform)
maicord_test_bundle(MaiCordModelTests LIBS maicord_core maicord_protocol maicord_model INCLUDES src/app)
maicord_test_bundle(MaiCordTextUnitTests LIBS maicord_core maicord_unicode maicord_font)

maicord_test(MaiCordNotificationRetentionTests ENTRY maicord_notification_retention_tests_entry SOURCES
        tests/model/notifications/retention.cpp
    LIBS maicord_model
    BUNDLE MaiCordCoreUnitTests
    TEST maicord_notification_retention)
maicord_test(MaiCordNotificationPcmTests ENTRY maicord_notification_pcm_tests_entry SOURCES
        tests/media/audio/notification_pcm.cpp
    LIBS maicord_notification_pcm
    BUNDLE MaiCordCoreUnitTests
    TEST maicord_notification_pcm)
maicord_test(MaiCordPermissionComputeTests ENTRY maicord_permission_compute_entry SOURCES
        tests/model/guild/permission_compute.cpp
    BUNDLE MaiCordModelTests TIMEOUT 10
    TEST maicord_permission_compute)
maicord_test(MaiCordCookieJarTests ENTRY maicord_cookie_jar_entry SOURCES tests/protocol/http/cookie_jar.cpp
    BUNDLE MaiCordModelTests TIMEOUT 10
    TEST maicord_cookie_jar)
maicord_test(MaiCordUserSettingsTests ENTRY maicord_user_settings_tests_entry SOURCES
        tests/protocol/user/settings_proto.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 10
    TEST maicord_user_settings)
maicord_test(MaiCordGuildSettingsTests ENTRY maicord_guild_settings_entry SOURCES
        tests/model/guild/settings_model.cpp
    BUNDLE MaiCordModelTests TIMEOUT 10
    TEST maicord_guild_settings)
maicord_test(MaiCordThreadBrowserProtocolTests ENTRY maicord_thread_browser_protocol_test_entry SOURCES
        tests/protocol/messages/thread_browser_protocol.cpp
    LIBS maicord_model
    BUNDLE MaiCordProtocolTests TIMEOUT 15
    TEST maicord_thread_browser_protocol)
maicord_test(MaiCordThreadCreationProtocolTests ENTRY maicord_thread_creation_protocol_test_entry SOURCES
        tests/protocol/messages/thread_creation_protocol.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 15
    TEST maicord_thread_creation_protocol)
maicord_test(MaiCordForumPostProtocolTests ENTRY maicord_forum_post_protocol_test_entry SOURCES
        tests/protocol/messages/forum_post_protocol.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 15
    TEST maicord_forum_post_protocol)
maicord_test(MaiCordMp4Tests ENTRY maicord_mp4_tests_entry SOURCES tests/media/containers/mp4_demux_test.cpp
    BUNDLE MaiCordCodecUnitTests
    TEST maicord_mp4_demux)
maicord_test(MaiCordH264Tests ENTRY maicord_h264_tests_entry SOURCES tests/media/video/h264_parameters_test.cpp
        tests/media/video/h264_state_test.cpp tests/media/video/h264_stream_test.cpp
        tests/media/video/hevc_stream_test.cpp
    BUNDLE MaiCordCodecUnitTests
    TEST maicord_h264_parameters)
maicord_test(MaiCordUnicodeTests ENTRY maicord_unicode_tests_entry SOURCES tests/text/unicode/unicode_test.cpp
        tests/text/unicode/grapheme_test.cpp
    LIBS maicord_core maicord_unicode
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_unicode)
maicord_test(MaiCordArabicJoiningTests ENTRY maicord_arabic_joining_tests_entry SOURCES
        tests/text/scripts/arabic_joining_test.cpp
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_arabic_joining)
maicord_test(MaiCordArabicMaskTests ENTRY maicord_arabic_mask_tests_entry SOURCES
        tests/text/scripts/arabic_mask_test.cpp
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_arabic_mask)
maicord_font_test(MaiCordArabicMarksTests maicord_arabic_marks_tests_entry text/scripts/arabic_marks_test
    TEST maicord_native_arabic_marks)
maicord_test(MaiCordBidiTests ENTRY maicord_bidi_tests_entry SOURCES tests/text/unicode/bidi_test.cpp
    LIBS maicord_core maicord_unicode
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_bidi)
maicord_test(MaiCordLineBreakTests ENTRY maicord_line_break_entry SOURCES
        tests/text/unicode/line_break_test.cpp
    LIBS maicord_core maicord_unicode
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_line_break)
maicord_font_test(MaiCordLayoutTableTests maicord_layout_tables_entry text/opentype/layout_tables_test
    TEST maicord_native_layout_tables)
maicord_font_test(MaiCordGdefTests maicord_gdef_tests_entry text/opentype/gdef_test
    TEST maicord_native_gdef)
maicord_font_test(MaiCordLayoutDirectoryTests maicord_layout_directory_entry text/opentype/layout_directory_test
    TEST maicord_native_layout_directory)
maicord_font_test(MaiCordGsubTests maicord_gsub_tests_entry text/opentype/gsub_test
    TEST maicord_native_gsub)
maicord_font_test(MaiCordContextTests maicord_context_tests_entry text/opentype/context_test
    TEST maicord_native_context)
maicord_font_test(MaiCordGlyphBufferTests maicord_glyph_buffer_entry text/opentype/glyph_buffer_test)
maicord_font_test(MaiCordGsubExecuteTests maicord_gsub_execute_entry text/opentype/gsub_execute_test)
maicord_font_test(MaiCordClustersTests maicord_clusters_entry text/shaping/clusters_test)
maicord_font_test(MaiCordComponentShapingTests maicord_component_shaping_entry text/opentype/component_shaping_test
        UNICODE
    TEST maicord_native_component_shaping)
maicord_font_test(MaiCordSinhalaInputTests maicord_sinhala_input_entry text/scripts/sinhala_input_test UNICODE)
maicord_font_test(MaiCordTibetanInputTests maicord_tibetan_input_entry text/scripts/tibetan_input_test UNICODE)
maicord_add_tests(MaiCordTibetanInputTests TIMEOUT 2 LABELS native-text-tibetan-input
    TEST maicord_native_tibetan_input)
maicord_add_tests(MaiCordSinhalaInputTests TIMEOUT 2 LABELS native-text-sinhala-input
    TEST maicord_native_sinhala_input)
maicord_font_test(MaiCordKhmerInputTests maicord_khmer_input_entry text/scripts/khmer_input_test UNICODE TIMEOUT 2
        LABELS native-text-khmer-input
    TEST maicord_native_khmer_input)
maicord_font_test(MaiCordUsePlanTests maicord_use_plan_entry text/scripts/use_plan_test TIMEOUT 2 LABELS
        native-text-use-plan
    TEST maicord_native_use_plan)
maicord_font_test(MaiCordMyanmarInputTests maicord_myanmar_input_entry text/scripts/myanmar_input_test TIMEOUT 2
        LABELS native-text-myanmar-input
    TEST maicord_native_myanmar_input)
maicord_add_tests(MaiCordUsePlanTests TIMEOUT 2 LABELS native-text-tibetan-plan
    TEST maicord_native_tibetan_plan --tibetan)
maicord_add_tests(MaiCordUsePlanTests TIMEOUT 2 LABELS native-text-myanmar-plan
    TEST maicord_native_myanmar_plan --myanmar)
maicord_font_test(MaiCordKhmerPlanTests maicord_khmer_plan_entry text/scripts/khmer_plan_test TIMEOUT 2 LABELS
        native-text-khmer-plan
    TEST maicord_native_khmer_plan)
maicord_font_test(MaiCordShapeStageTests maicord_shape_stage_entry text/shaping/shape_stage_test TIMEOUT 5 LABELS
        native-text-stages
    TEST maicord_native_shape_stages)
maicord_font_test(MaiCordFontFallbackTests maicord_font_fallback_entry text/fonts/fallback_test
    TEST maicord_native_font_fallback_cascade)
maicord_test(MaiCordFontFallbackFamilyTests ENTRY maicord_font_fallback_family_entry SOURCES
        tests/text/fonts/fallback_family_test.cpp
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_font_fallback_families)
maicord_font_test(MaiCordFontFallbackRangeTests maicord_font_fallback_range_entry text/fonts/fallback_range_test
    TEST maicord_native_font_fallback_ranges)
maicord_test(MaiCordFontFamilyTests ENTRY maicord_font_candidates_entry SOURCES tests/text/fonts/candidates_test.cpp
    LIBS maicord_core maicord_font
    DEFINES MAICORD_FONT_FAMILY=1 TIMEOUT 10 LABELS native-text-font-family
    TEST maicord_native_font_family)
maicord_font_test(MaiCordFontMatchTests maicord_font_match_entry text/fonts/match_test
    TEST maicord_native_font_match)
maicord_font_test(MaiCordFontNamesTests maicord_font_names_entry text/fonts/names_test TIMEOUT 2 LABELS
        native-text-font-names
    TEST maicord_native_font_names)
maicord_font_test(MaiCordScriptTests maicord_script_tests_entry text/unicode/script_test UNICODE
    TEST maicord_native_scripts)
maicord_test(MaiCordZstdStreamTests ENTRY maicord_zstd_stream_entry SOURCES tests/media/compression/zstd_stream.cpp
    BUNDLE MaiCordCoreUnitTests
    TEST maicord_zstd_stream)
maicord_test(MaiCordEtfTranscodeTests ENTRY maicord_etf_transcode_entry SOURCES
        tests/protocol/gateway/etf_transcode.cpp src/protocol/gateway/etf.cpp
    BUNDLE MaiCordCoreUnitTests
    TEST maicord_etf_transcode)
maicord_test(MaiCordGatewayEtfSessionTests ENTRY maicord_gateway_etf_session_entry SOURCES
        tests/protocol/gateway/etf_session.cpp
    BUNDLE MaiCordCoreUnitTests
    TEST maicord_gateway_etf_session)
maicord_font_test(MaiCordCursiveDeferredTests maicord_cursive_deferred_entry text/opentype/cursive_deferred_test
    TEST maicord_native_cursive_deferred)
maicord_font_test(MaiCordLookupPlanTests maicord_lookup_plan_entry text/opentype/lookup_plan_test
    TEST maicord_native_lookup_plan)
maicord_font_test(MaiCordNominalParticipationTests maicord_nominal_participation_entry
        text/opentype/nominal_participation_test
    TEST maicord_native_nominal_participation)
maicord_font_test(MaiCordNominalSourcesTests maicord_nominal_sources_entry text/opentype/nominal_sources_test
    TEST maicord_native_nominal_sources)
maicord_font_test(MaiCordNominalVariationsTests maicord_nominal_variations_entry
        text/opentype/nominal_variations_test
    TEST maicord_native_nominal_variations)
maicord_font_test(MaiCordMetricVariationsTests maicord_metric_variations_entry text/opentype/metric_variations_test
    TEST maicord_native_metric_variations)
maicord_font_test(MaiCordGvarOutlineTests maicord_gvar_outline_entry text/opentype/gvar_outline_test
    TEST maicord_native_gvar_outlines)
maicord_font_test(MaiCordTtInterpreterTests maicord_tt_interpreter_entry text/opentype/tt_interpreter_test)
maicord_font_test(MaiCordTtOrientationTests maicord_tt_orientation_entry text/opentype/tt_orientation_test
    TEST maicord_native_tt_orientation)
maicord_add_tests(MaiCordTtInterpreterTests
    TEST maicord_native_tt_interpreter)
maicord_font_test(MaiCordTtOutlineTests maicord_tt_outline_entry text/opentype/tt_outline_test
    TEST maicord_native_tt_bold bold
    TEST maicord_native_tt_bold_oblique bold-oblique
    TEST maicord_native_tt_outlines normal
    TEST maicord_native_tt_oblique oblique
    TEST maicord_native_tt_positioning position)
maicord_font_test(MaiCordGposTests maicord_gpos_tests_entry text/opentype/gpos_test)
maicord_font_test(MaiCordGposExecuteTests maicord_gpos_execute_tests_entry text/opentype/gpos_execute_test TIMEOUT 5
        LABELS native-text-gpos-marks
    TEST maicord_native_gpos_execute)
maicord_font_test(MaiCordGposAttachmentTests maicord_gpos_attachment_tests_entry text/opentype/gpos_attachment_test
    TEST maicord_native_gpos_attachments)
maicord_add_tests(MaiCordGposTests
    TEST maicord_native_gpos)
maicord_add_tests(MaiCordClustersTests
    TEST maicord_native_clusters)
maicord_add_tests(MaiCordGsubExecuteTests
    TEST maicord_native_gsub_execute)
maicord_add_tests(MaiCordGlyphBufferTests
    TEST maicord_native_glyph_buffer)
maicord_test(MaiCordFontTests ENTRY maicord_font_tests_entry SOURCES tests/text/opentype/sfnt_test.cpp
        tests/text/opentype/tt_bold_points_test.cpp tests/text/opentype/truetype_outline_test.cpp
        tests/text/opentype/cff_outline_test.cpp tests/text/opentype/color_font_test.cpp
        tests/text/opentype/variation_store_test.cpp tests/text/fonts/axes_test.cpp
    LIBS maicord_core maicord_font
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_font)
# The font store, catalog check and handle stamp over real files, through the files seam.
maicord_test(MaiCordFontStoreFilesTests ENTRY maicord_font_store_files_entry SOURCES
        tests/text/fonts/store_files_test.cpp
    LIBS maicord_core maicord_font TIMEOUT 5 TEST maicord_native_font_store_files)
maicord_test(MaiCordMediaDecodeReferenceTests ENTRY maicord_media_decode_reference_tests_entry SOURCES
        tests/media/image/media_decode_reference.cpp
    BUNDLE MaiCordCodecUnitTests TIMEOUT 10
    TEST maicord_media_decode_reference)
maicord_test(MaiCordOpusDecoderVectorTests ENTRY maicord_opus_decoder_vector_tests_entry SOURCES
        tests/media/audio/opus_decoder_vectors.cpp
    LIBS maicord_opus_decoder
    INCLUDES tests third_party/opus/include
    BUNDLE MaiCordCodecUnitTests TIMEOUT 10
    TEST maicord_opus_decoder_vectors)
maicord_test(MaiCordStoreLimitsTests ENTRY maicord_store_limits_entry SOURCES tests/model/guild/store_limits.cpp
    BUNDLE MaiCordModelTests TIMEOUT 10
    TEST maicord_store_limits)
maicord_test(MaiCordEmbedProviderTests ENTRY maicord_embed_provider_entry SOURCES
        tests/protocol/media/embed_provider.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5
    TEST maicord_embed_provider)
maicord_test(MaiCordYoutubePlayerTests ENTRY maicord_youtube_player_entry SOURCES
        tests/protocol/media/youtube_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5
    TEST maicord_youtube_player)
maicord_test(MaiCordTikTokPlayerTests ENTRY maicord_tiktok_player_entry SOURCES
        tests/protocol/media/tiktok_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5
    TEST maicord_tiktok_player)
maicord_test(MaiCordTwitchPlayerTests ENTRY maicord_twitch_player_entry SOURCES
        tests/protocol/media/twitch_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5
    TEST maicord_twitch_player)
maicord_test(MaiCordSpotifyPlayerTests ENTRY maicord_spotify_player_entry SOURCES
        tests/protocol/media/spotify_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5
    TEST maicord_spotify_player)
maicord_test(MaiCordStreamablePlayerTests ENTRY maicord_streamable_player_entry SOURCES
        tests/protocol/media/streamable_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5
    TEST maicord_streamable_player)
maicord_test(MaiCordPornhubPlayerTests ENTRY maicord_pornhub_player_entry SOURCES
        tests/protocol/media/pornhub_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5
    TEST maicord_pornhub_player)
maicord_test(MaiCordVimeoPlayerTests ENTRY maicord_vimeo_player_entry SOURCES tests/protocol/media/vimeo_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5
    TEST maicord_vimeo_player)
maicord_test(MaiCordAppleMusicPlayerTests ENTRY maicord_apple_music_player_entry SOURCES
        tests/protocol/media/apple_music_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5
    TEST maicord_apple_music_player)
maicord_test(MaiCordOAuthProtocolTests ENTRY maicord_oauth_protocol_entry SOURCES
        tests/protocol/auth/oauth_protocol.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5
    TEST maicord_oauth_protocol)
maicord_test(MaiCordDashManifestTests ENTRY maicord_dash_manifest_entry SOURCES
        tests/protocol/media/dash_manifest.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5
    TEST maicord_dash_manifest)
maicord_test(MaiCordSteamPlayerTests ENTRY maicord_steam_player_entry SOURCES tests/protocol/media/steam_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5
    TEST maicord_steam_player)
maicord_test(MaiCordHlsPlaylistTests ENTRY maicord_hls_playlist_entry SOURCES tests/protocol/media/hls_playlist.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5
    TEST maicord_hls_playlist)
maicord_test(MaiCordImgurPlayerTests ENTRY maicord_imgur_player_entry SOURCES tests/protocol/media/imgur_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5
    TEST maicord_imgur_player)
maicord_test(MaiCordTwitterPlayerTests ENTRY maicord_twitter_player_entry SOURCES
        tests/protocol/media/twitter_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5
    TEST maicord_twitter_player)
maicord_test(MaiCordAmazonMusicPlayerTests ENTRY maicord_amazon_music_player_entry SOURCES
        tests/protocol/media/amazon_music_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5
    TEST maicord_amazon_music_player)

# Tests of these libraries that need what the Linux build has not ported (the Windows memory queries, the file
# metadata of the Windows suite; cmake/linux_font_tests.cmake registers the font tests that read files):
# Not registered: MaiCordReactionPreferencesStorageTests (queries the Windows memory state (VirtualQuery) and maps a read-only file to refuse a commit)
# Not registered: MaiCordRoleManagerTests (links the settings UI scene)
# Not registered: MaiCordFontCatalogTests (the handle stamp units read Windows file information classes; tests/text/fonts/store_files_test.cpp covers the stamp on both systems)
# Not registered: MaiCordFontCatalogCoverageTests (sets a file's metadata change time, which Linux cannot set)
