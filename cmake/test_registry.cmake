# Test registry: every temporary test executable or fixture and its CTest
# cases. maicord_test(), maicord_add_tests() and the registration helpers
# below are in cmake/test_support.cmake; register each new test here, next to
# the area it covers.

# The application stack most src/app fixtures link.
set(maicord_app_test_libs maicord_core maicord_platform maicord_protocol maicord_model maicord_app_core maicord_ui)

maicord_test(MaiCordRemoteAuthCancellationTests ENTRY maicord_remote_auth_cancellation_entry
    SOURCES tests/remote_auth_ticket_exchange_cancellation.cpp src/app/remote_auth_ticket_exchange.cpp
        src/app/remote_auth_handshake.cpp src/app/captcha_challenge.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32 ws2_32 secur32 bcrypt crypt32
    TIMEOUT 15
    TEST maicord_remote_auth_cancellation)
maicord_test(MaiCordAccountLoginHandoffTests ENTRY maicord_account_login_handoff_entry
    SOURCES tests/account_login_handoff.cpp tests/account_login_remote_fixture.cpp
        src/app/account_profile_promotion.cpp src/app/remote_auth_ticket_exchange.cpp
        src/app/captcha_challenge.cpp src/app/remote_auth_handshake.cpp src/app/captcha_bridge.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32 ws2_32 secur32 bcrypt crypt32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_account_login_handoff)
maicord_test(MaiCordNotificationRetentionTests ENTRY maicord_notification_retention_tests_entry
    SOURCES tests/notification_retention.cpp
    LIBS maicord_model maicord_core kernel32
    TEST maicord_notification_retention)
maicord_test(MaiCordNotificationSoundAssetTests ENTRY maicord_notification_sound_assets_tests_entry
    SOURCES tests/notification_sound_assets.cpp
        "${MAICORD_NOTIFICATION_SOUND_RESOURCE}"
    LIBS maicord_notification_sound_assets kernel32 bcrypt
    TEST maicord_notification_sound_assets)
maicord_test(MaiCordNotificationAudioTests ENTRY maicord_notification_audio_tests_entry
    SOURCES tests/native_notification_audio.cpp
    LIBS maicord_notification_audio kernel32 ole32 user32
    TEST maicord_notification_audio)
# Compile an isolated copy with deterministic COM transport hooks. The
# shipping library excludes the seam completely; fixtures never open an
# actual endpoint even if no hook has been installed.
maicord_test(MaiCordNotificationAudioFaultTests ENTRY maicord_notification_audio_faults_tests_entry
    SOURCES src/platform/notification_audio.cpp tests/native_notification_audio_faults.cpp
    LIBS maicord_core kernel32 ole32 user32
    DEFINES MAICORD_NOTIFICATION_AUDIO_FIXTURE=1
    TEST maicord_notification_audio_faults)
maicord_test(MaiCordNotificationPcmTests ENTRY maicord_notification_pcm_tests_entry
    SOURCES tests/notification_pcm.cpp
    LIBS maicord_notification_pcm kernel32
    TEST maicord_notification_pcm)
maicord_test(MaiCordPluginFixture SHARED ENTRY maicord_plugin_fixture_entry
    SOURCES tests/fixtures/native_plugin.cpp
    LIBS kernel32)
# ABI-4 fixtures and the in-tree SDK sample (samples/plugins/showcase).
foreach(maicord_sdk_fixture IN ITEMS Full Slow)
    string(TOLOWER "${maicord_sdk_fixture}" maicord_sdk_name)
    maicord_c_plugin(MaiCordPluginSdk${maicord_sdk_fixture}Fixture maicord_plugin_sdk_${maicord_sdk_name}_entry
        tests/fixtures/plugin_sdk_${maicord_sdk_name}.c)
endforeach()
# MaiCordPluginSdkTests loads the sample; its manifest is stamped with the DLL
# digest after each link.
set(maicord_showcase_generated "${MAICORD_GENERATED_DIRECTORY}/showcase")
set(maicord_showcase_shader "${maicord_showcase_generated}/showcase_gradient_ps.h")
file(MAKE_DIRECTORY "${maicord_showcase_generated}")
add_custom_command(
    OUTPUT "${maicord_showcase_shader}"
    COMMAND "${MAICORD_FXC_EXECUTABLE}"
        /nologo /T ps_5_0 /E main /O3 /Ges /WX
        /Qstrip_debug /Qstrip_reflect /Qstrip_priv
        /Fh "${maicord_showcase_shader}" /Vn g_showcase_gradient
        "${CMAKE_CURRENT_SOURCE_DIR}/samples/plugins/showcase/gradient.hlsl"
    DEPENDS samples/plugins/showcase/gradient.hlsl
    VERBATIM)
maicord_c_plugin(MaiCordShowcasePlugin maicord_showcase_entry samples/plugins/showcase/showcase.c
    "${maicord_showcase_shader}")
target_include_directories(MaiCordShowcasePlugin PRIVATE "${maicord_showcase_generated}")
set_target_properties(MaiCordShowcasePlugin PROPERTIES OUTPUT_NAME showcase)
add_custom_command(TARGET MaiCordShowcasePlugin POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/samples/plugins/showcase/showcase.maicord-plugin.json.in"
        "-DDLL=$<TARGET_FILE:MaiCordShowcasePlugin>"
        "-DOUTPUT=$<TARGET_FILE_DIR:MaiCordShowcasePlugin>/showcase.maicord-plugin.json"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/stamp_plugin_manifest.cmake"
    VERBATIM)
maicord_test(MaiCordPluginSdkTests ENTRY maicord_plugin_sdk_tests_entry
    SOURCES tests/plugin_sdk.cpp
    LIBS maicord_core maicord_ui maicord_plugin_host maicord_plugin_modules maicord_client_preferences kernel32
        bcrypt
    INCLUDES src/app
    DEPENDS MaiCordPluginSdkFullFixture MaiCordPluginSdkSlowFixture MaiCordShowcasePlugin
    TIMEOUT 15
    TEST maicord_plugin_sdk)
maicord_test(MaiCordNativeExtensionTests ENTRY maicord_native_extensions_entry
    SOURCES tests/gateway_message_poll.cpp tests/message_poll_store.cpp tests/message_poll_actions.cpp
        tests/poll_controller.cpp tests/poll_controls.cpp tests/message_poll_presentation.cpp
        src/app/account_message_poll.cpp tests/native_extensions.cpp tests/channel_overview.cpp
        tests/channel_content_policy.cpp tests/channel_overview_controller.cpp
        tests/pinned_message_controller.cpp tests/quick_switcher_controller.cpp
        tests/message_forward_controller.cpp tests/bounded_edit.cpp tests/gif_favorites.cpp
        tests/emoji_preferences.cpp tests/thread_starter.cpp tests/update_manifest.cpp tests/update_artifact.cpp
        tests/page_registry.cpp tests/plugin_host.cpp tests/update_acceptance.cpp tests/update_stage.cpp
        tests/update_prepare.cpp tests/update_package.cpp tests/update_generation.cpp
        tests/update_generation_authentication.cpp tests/update_generation_failures.cpp tests/update_generation_legacy.cpp
        tests/update_install.cpp
        tests/update_signed_handoff.cpp tests/reaction_sections.cpp tests/message_reaction_controller.cpp
        tests/thread_authority_model.cpp tests/thread_header.cpp tests/application_thread_header_activation.cpp
        src/app/application_thread_header_activation.cpp tests/regional_flag_media.cpp tests/flag_surfaces.cpp
        tests/composer_source_spans.cpp tests/application_login_geometry.cpp tests/tsf_end_edit.cpp
        tests/member_list_sparse_model.cpp tests/member_viewport.cpp tests/member_paging_pump.cpp
        tests/thread_members.cpp tests/thread_storage.cpp tests/thread_archive.cpp
        tests/thread_member_viewport.cpp tests/thread_member_pump.cpp tests/thread_subscriptions.cpp
        tests/role_lookup_performance.cpp tests/role_order.cpp tests/guild_role_replacement.cpp
        tests/permission_prefix.cpp
        tests/guild_channel_validation.cpp tests/channel_name_preflight.cpp tests/chat_state_backing.cpp
        tests/read_state_model.cpp
        tests/read_state_wire.cpp tests/message_ack.cpp tests/read_ack_controller.cpp tests/bulk_message_ack.cpp
        tests/read_scope.cpp tests/bulk_read_ack_controller.cpp tests/read_state_pump.cpp
        tests/channel_context_policy.cpp tests/guild_context_policy.cpp
        tests/user_social_menus.cpp tests/unread_context.cpp tests/mention_badge_text.cpp
        tests/gateway_ready_projection.cpp tests/text_emitter.cpp tests/member_user_perf.cpp
        src/render/text_edit_layout.cpp tests/message_context_controller.cpp tests/settings_scene.cpp
        tests/settings_choice.cpp tests/password_recovery.cpp tests/password_reset_ui.cpp
        tests/profile_editor_layout.cpp tests/phone_recovery.cpp tests/password_phone_ui.cpp
        tests/friends_scene.cpp tests/friends_media.cpp tests/text_form_confirmation.cpp
        tests/relationship_sync.cpp tests/friend_search.cpp tests/client_preferences.cpp
        tests/scroll_host_preferences.cpp tests/menu_rail_smooth_scroll.cpp tests/visible_author_hydration.cpp
        tests/background_theme.cpp tests/background_effect_policy.cpp tests/background_color_math.cpp
        src/render/content_text_retention.cpp tests/theme_editor.cpp tests/theme_styles.cpp
        tests/client_theme_storage.cpp tests/client_theme_document.cpp tests/theme_style_projection.cpp
        tests/theme_tokens.cpp tests/surface_elevation.cpp tests/login_continuations.cpp
        tests/client_preferences_writer.cpp tests/plugin_module_loader.cpp tests/plugin_catalog.cpp
        tests/plugin_preferences.cpp tests/plugin_bundle_install.cpp
        tests/application_plugin_settings_projection.cpp tests/video_controls.cpp tests/video_scene.cpp
        tests/audio_controls.cpp tests/audio_presentation.cpp tests/ui_cursor.cpp tests/ogg_opus.cpp
        tests/opus_decoder.cpp tests/wave_pcm.cpp tests/mp3_stream.cpp tests/account_video_session.cpp
        tests/account_video_commands.cpp tests/account_media_video_dispatch.cpp tests/local_draft_preview.cpp
        tests/hevc.cpp tests/hevc_references.cpp tests/video_frame_bounds.cpp tests/spinner.cpp
        tests/transition.cpp tests/tooltip.cpp tests/message_rich_store.cpp tests/account_menu.cpp
        tests/message_search.cpp tests/message_pins.cpp tests/forwarded_message_snapshots.cpp
        tests/forwarded_message_pages.cpp tests/message_forward_source_flags.cpp tests/search_scene.cpp
        tests/pinned_messages_scene.cpp tests/inbox_scene.cpp tests/thread_browser_scene.cpp
        tests/forum_post_scene.cpp tests/quick_switcher_scene.cpp tests/message_forward_scene.cpp
        tests/download_scene.cpp tests/http_response_stream.cpp tests/attachment_download.cpp
        tests/https_file_download.cpp tests/search_query.cpp tests/search_filter_context.cpp
        tests/search_recent.cpp tests/link_trust.cpp tests/media_picker.cpp tests/media_catalog.cpp
        tests/message_create.cpp tests/message_composer_controller.cpp tests/application_video_request.cpp
        tests/widget_builder.cpp tests/staged_file.cpp tests/menu_row.cpp tests/icon_pack.cpp
        tests/aac_decoder.cpp tests/media_audio.cpp tests/browser_sign_in.cpp tests/browser_sign_in_receiver.cpp
        tests/browser_login_ui.cpp tests/gateway_self_status.cpp tests/application_update_startup.cpp
        tests/desktop_integration.cpp src/app/plugin_bundle_install.cpp
    LIBS maicord_core maicord_platform maicord_app_core maicord_ui maicord_plugin_host
        maicord_client_preferences maicord_account_host maicord_account_paths maicord_account_media
        maicord_plugin_modules maicord_message_media maicord_ui_input maicord_text maicord_render_core
        maicord_model kernel32 user32 bcrypt crypt32 ws2_32 secur32 ole32 ntdll
    INCLUDES src/app third_party/opus/include
    DEFINES MAICORD_PLUGIN_BUNDLE_INSTALL_FIXTURE=1
        "MAICORD_UPDATE_TEST_EXECUTABLE=L\"$<TARGET_FILE:MaiCord>\""
        "MAICORD_UPDATE_HANDOFF_TEST_PEER=L\"$<TARGET_FILE:MaiCordUpdateHandoffPeer>\""
        "MAICORD_THEME_EXAMPLE_PATH=L\"${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/example.maicord-theme.json\""
    DEPENDS MaiCordPluginFixture MaiCordUpdateHandoffPeer
    USES_CLIENT MaiCord)
# The same 173 fixtures run once across four private process/profile owners.
# Shard 0 keeps all 13 durable update fixtures serial and runs 40 other fixtures;
# the remaining shards each run 40. Every shard checks its exact function count.
foreach(maicord_extension_shard RANGE 0 3)
    set(maicord_extension_test maicord_native_extensions)
    set(maicord_extension_fixture_count 40)
    if(maicord_extension_shard)
        string(APPEND maicord_extension_test "_shard_${maicord_extension_shard}")
    else()
        set(maicord_extension_fixture_count 53)
    endif()
    maicord_add_tests(MaiCordNativeExtensionTests
        PASS_REGULAR_EXPRESSION "PASS native extensions shard=${maicord_extension_shard}/4 fixtures=${maicord_extension_fixture_count}/173"
        TEST ${maicord_extension_test} --fixture-shard ${maicord_extension_shard} 4)
endforeach()
maicord_test(MaiCordUpdateHandoffPeer ENTRY maicord_update_handoff_peer_entry
    SOURCES tests/update_handoff_peer.cpp
    LIBS maicord_core maicord_platform maicord_app_core maicord_account_host kernel32 bcrypt crypt32)
# Theme ratchets: presentation literals in src/ui and the generated example theme.
add_test(NAME maicord_theme_literals COMMAND "${Python3_EXECUTABLE}" -B tools/check_theme_literals.py
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME maicord_theme_example COMMAND "${Python3_EXECUTABLE}" -B tools/generate_theme_example.py --check
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(maicord_theme_literals maicord_theme_example PROPERTIES TIMEOUT 10)
add_test(NAME maicord_build_output_guard COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_output_guard.ps1")
set_tests_properties(maicord_build_output_guard PROPERTIES TIMEOUT 10)
maicord_test(MaiCordAccountVideoSessionProbe ENTRY maicord_account_video_session_probe_entry
    SOURCES tests/integration/account_video_session_probe.cpp src/app/account_video_session.cpp
        src/platform/hevc_gpu_decoder.cpp src/render/video_frame_gpu.cpp ${MAICORD_VIDEO_FRAME_HEADER}
    LIBS maicord_account_media maicord_platform maicord_model maicord_shader_bytecode d3d11 dxgi ole32 kernel32
        user32
    INCLUDES ${MAICORD_GENERATED_DIRECTORY}
    DEFINES MAICORD_ACCOUNT_VIDEO_SESSION_PROBE=1)
if(MAICORD_TEST_AUDIO_PLAYBACK)
    maicord_test(MaiCordNotificationAudioPlayback ENTRY maicord_notification_audio_playback_entry
        SOURCES tests/notification_audio_playback.cpp ${MAICORD_NOTIFICATION_SOUND_RESOURCE}
        LIBS maicord_notification_audio maicord_notification_sound_assets kernel32 ole32 user32
        TIMEOUT 5 RESOURCE_LOCK native_audio
        TEST maicord_notification_audio_playback)
    maicord_add_tests(MaiCordAccountVideoSessionProbe TIMEOUT 5 COST 51 RESOURCE_LOCK native_audio
        TEST maicord_account_audio_playback --audio)
endif()
maicord_test(MaiCordUpdateHandoffTests ENTRY maicord_update_handoff_tests_entry
    SOURCES tests/update_handoff.cpp
    LIBS maicord_core maicord_platform maicord_app_core kernel32
    DEFINES "MAICORD_UPDATE_HANDOFF_TEST_PEER=L\"$<TARGET_FILE:MaiCordUpdateHandoffPeer>\""
    DEPENDS MaiCordUpdateHandoffPeer
    TIMEOUT 20
    TEST maicord_update_handoff)
maicord_test(MaiCordUpdateClientTests ENTRY maicord_update_client_tests_entry
    SOURCES tests/update_client.cpp src/app/application_updates.cpp src/app/settings_catalog.cpp
    LIBS maicord_core maicord_platform maicord_app_core maicord_ui maicord_client_preferences kernel32 bcrypt
        ws2_32 secur32
    INCLUDES src/app
    DEFINES "MAICORD_UPDATE_TEST_EXECUTABLE=L\"$<TARGET_FILE:MaiCord>\""
    USES_CLIENT MaiCord
    TIMEOUT 15
    TEST maicord_update_client)
maicord_test(MaiCordUpdateChangelogTests ENTRY maicord_update_changelog_tests_entry
    SOURCES tests/update_changelog.cpp
    LIBS maicord_core maicord_platform maicord_app_core kernel32 bcrypt
    BUNDLE MaiCordAppUnitTests
    TIMEOUT 10
    TEST maicord_update_changelog)
# Startup: the DLLs MaiCord.exe maps before its entry point (static imports)
# and those it delay-loads (MAICORD_CLIENT_DELAY_LOADS in cmake/client.cmake).
list(JOIN MAICORD_CLIENT_DELAY_LOADS "," maicord_client_delay_load_list)
maicord_test(MaiCordClientStartupImportsTests ENTRY maicord_client_startup_imports_entry
    SOURCES tests/client_startup_imports.cpp
    LIBS maicord_core kernel32
    DEFINES "MAICORD_CLIENT_IMPORTS_EXECUTABLE=L\"$<TARGET_FILE:MaiCord>\""
        "MAICORD_CLIENT_DELAY_LOADS=\"${maicord_client_delay_load_list}\""
    USES_CLIENT MaiCord
    TIMEOUT 5
    TEST maicord_client_imports)
# Plugin catalog: the index verifier against the vectors shared with
# tools/release/test_plugin_index_tools.py (tests/fixtures/plugin_index), the
# catalog policy, the floor, providers, state and journal records, provider
# addresses and the add probe, and the check service against a fake server.
maicord_test(MaiCordPluginIndexTests ENTRY maicord_plugin_index_tests_entry
    SOURCES tests/plugin_index.cpp tests/plugin_index_policy.cpp
        tests/plugin_manager_records.cpp tests/plugin_providers.cpp tests/plugin_index_client.cpp
    LIBS maicord_core maicord_platform maicord_app_core maicord_plugin_manager maicord_plugin_modules kernel32 bcrypt
    DEFINES "MAICORD_PLUGIN_INDEX_FIXTURES=L\"${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/plugin_index\""
    TIMEOUT 10
    TEST maicord_plugin_index)
# Plugin manager: catalog install, update, rollback and uninstall through the
# bundle installer (compiled with its fault fixture), and a simulated crash at
# each journal phase followed by recovery. A fake transport serves a signed
# index and the MaiCordPluginFixture DLL, which is never loaded.
maicord_test(MaiCordPluginManagerTests ENTRY maicord_plugin_manager_tests_entry
    SOURCES tests/plugin_manager_install.cpp src/app/plugin_bundle_install.cpp
    LIBS maicord_core maicord_platform maicord_app_core maicord_plugin_manager maicord_plugin_modules kernel32 bcrypt
    DEFINES MAICORD_PLUGIN_BUNDLE_INSTALL_FIXTURE=1
    DEPENDS MaiCordPluginFixture
    TIMEOUT 10
    TEST maicord_plugin_manager)
# Plugin catalog at runtime: startup recovery, the roll-forward of a staged
# automatic update and the startup gate; applying indexes to installed plugins
# (adoption, revocation, the blocklist, pruning, offers); the browser
# snapshot; and the catalog worker driven through a fake server.
maicord_test(MaiCordPluginRuntimeTests ENTRY maicord_plugin_runtime_tests_entry
    SOURCES tests/plugin_manager_runtime.cpp src/app/application_plugin_browser_worker.cpp
    LIBS maicord_core maicord_platform maicord_app_core maicord_plugin_manager maicord_plugin_modules kernel32 bcrypt
        ws2_32 secur32 crypt32
    INCLUDES src/app
    DEFINES "MAICORD_PLUGIN_INDEX_FIXTURES=L\"${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/plugin_index\""
    DEPENDS MaiCordPluginFixture
    TIMEOUT 10
    TEST maicord_plugin_runtime)
# Settings > Plugins with the catalog: tab and filter control counts, chips,
# paging, the detail view and the dialog texts (pure projections).
maicord_test(MaiCordPluginBrowserTests ENTRY maicord_plugin_browser_tests_entry
    SOURCES tests/application_plugin_browser_projection.cpp src/app/application_plugin_browser_projection.cpp
    LIBS maicord_core maicord_platform maicord_app_core maicord_ui maicord_plugin_manager maicord_plugin_modules kernel32
        user32 bcrypt
    INCLUDES src/app
    BUNDLE MaiCordAppUnitTests
    TIMEOUT 10
    TEST maicord_plugin_browser)
# Nested rounded rectangles, clipped highlights and per-component padding
# across representative synthetic scenes.
maicord_test(MaiCordLayoutConsistencyTests ENTRY maicord_layout_consistency_tests_entry
    SOURCES tests/layout_consistency.cpp src/app/application_plugin_browser_projection.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_app_core maicord_ui maicord_text
        maicord_message_runtime maicord_plugin_manager maicord_plugin_modules kernel32 user32 bcrypt
    INCLUDES src/app
    BUNDLE MaiCordAppUnitTests
    TIMEOUT 10
    TEST maicord_layout_consistency)
# Window caption frame insets, hit testing across DPIs and button states.
maicord_test(MaiCordWindowCaptionTests ENTRY maicord_window_caption_tests_entry
    SOURCES tests/window_caption.cpp
    LIBS maicord_core maicord_platform maicord_ui kernel32 user32
    BUNDLE MaiCordUiUnitTests
    TIMEOUT 10
    TEST maicord_window_caption)
maicord_test(MaiCordSettingsRuntimeTests ENTRY maicord_settings_runtime_entry
    SOURCES tests/application_settings_runtime.cpp tests/application_custom_theme.cpp
        tests/application_plugin_catalog_runtime.cpp tests/application_plugin_navigation_runtime.cpp
        tests/application_settings_choice_runtime.cpp tests/application_user_settings_runtime.cpp
        tests/application_account_settings_runtime.cpp
        tests/application_keybinds_settings_runtime.cpp tests/application_plugins_read_runtime.cpp
        tests/application_plugins_actions_runtime.cpp src/app/application_user_settings.cpp
        src/app/application_account_settings.cpp src/app/application_state.cpp src/app/application_settings.cpp
        src/app/settings_catalog.cpp src/app/application_theme_editor.cpp src/app/application_plugins.cpp
        src/app/application_desktop_settings.cpp src/app/application_plugins_sdk.cpp
        src/app/application_plugins_read.cpp src/app/application_plugins_fx.cpp
        src/app/application_plugins_surfaces.cpp src/app/application_plugins_render.cpp
        src/app/application_plugin_menus.cpp src/app/application_shell_input.cpp
        src/render/content_text_retention.cpp src/core/log.cpp src/app/application_updates.cpp
        src/app/application_voice_settings.cpp src/app/voice_settings_binding.cpp
        src/app/application_keybinds_settings.cpp src/app/application_activity.cpp src/app/game_detection.cpp
        src/app/application_plugin_browser.cpp src/app/application_plugin_browser_worker.cpp
        src/app/application_plugin_browser_entry.cpp src/app/application_plugin_browser_projection.cpp
    LIBS bcrypt ws2_32 secur32 maicord_compile_contract maicord_core maicord_platform maicord_app_core
        maicord_ui maicord_client_preferences maicord_plugin_host maicord_plugin_modules maicord_plugin_manager
        maicord_message_media maicord_ui_input kernel32 user32 shell32 ole32
    INCLUDES src/app
    DEPENDS MaiCordPluginFixture
    TEST maicord_settings_runtime)
maicord_test(MaiCordNavigationGapsTests ENTRY maicord_navigation_gaps_entry
    SOURCES tests/navigation_gaps.cpp
    LIBS ${maicord_app_test_libs} maicord_message_runtime kernel32 user32 ole32
    BUNDLE MaiCordAppWaveTests
    TIMEOUT 10
    TEST maicord_navigation_gaps)
# Server lifecycle, guild folders, channel reordering, channel list order,
# drag state machines, text in voice and the server menu policy.
maicord_test(MaiCordNavigationWaveTests ENTRY maicord_navigation_wave_entry
    SOURCES tests/navigation_wave.cpp
    LIBS ${maicord_app_test_libs} maicord_message_runtime kernel32 user32 ole32
    BUNDLE MaiCordAppWaveTests
    TIMEOUT 10
    TEST maicord_navigation_wave)
# Membership screening, onboarding and scheduled events.
maicord_test(MaiCordGuildCommunityTests ENTRY maicord_guild_community_entry
    SOURCES tests/guild_community.cpp
    LIBS ${maicord_app_test_libs} maicord_message_runtime kernel32 user32 ole32
    BUNDLE MaiCordAppWaveTests
    TIMEOUT 10
    TEST maicord_guild_community)
# Server banners: Gateway decode, CDN URLs, channel header geometry and
# animation, the list inset, header paint/text and the server tag popout.
maicord_test(MaiCordGuildBannerTests ENTRY maicord_guild_banner_entry
    SOURCES tests/guild_banner.cpp
    LIBS ${maicord_app_test_libs} maicord_message_runtime kernel32 user32 ole32
    BUNDLE MaiCordAppWaveTests
    TIMEOUT 10
    TEST maicord_guild_banner)
# Rich presence, automatic idle, opcode 3, activity sharing, game
# detection, message requests and DM safety warnings.
maicord_test(MaiCordPresenceWaveTests ENTRY maicord_presence_wave_entry
    SOURCES tests/presence_wave.cpp src/app/game_detection.cpp
    LIBS ${maicord_app_test_libs} maicord_message_runtime kernel32 user32 ole32
    BUNDLE MaiCordAppWaveTests
    TIMEOUT 10
    TEST maicord_presence_wave)
# Profile rich presence cards: order, text, timers, progress, asset URLs and
# artwork fallback, popout and full profile layout, ticks, and the application
# icon and button metadata requests.
maicord_test(MaiCordProfileActivityTests ENTRY maicord_profile_activity_entry
    SOURCES tests/profile_activity.cpp
    LIBS ${maicord_app_test_libs} maicord_message_runtime kernel32 user32 ole32
    BUNDLE MaiCordAppWaveTests
    TIMEOUT 10
    TEST maicord_profile_activity)
# A large-account READY through the production projection and the app's
# message event pump; skipped-event isolation and diagnostics.
maicord_test(MaiCordReadyScalePumpTests ENTRY maicord_ready_scale_pump_entry
    SOURCES tests/ready_scale_pump.cpp
    LIBS ${maicord_app_test_libs} maicord_message_runtime kernel32 user32 ole32
    TIMEOUT 10
    TEST maicord_ready_scale_pump)
maicord_test(MaiCordAccessibilityTests ENTRY maicord_accessibility_entry
    SOURCES tests/accessibility.cpp
    LIBS maicord_core maicord_ui maicord_message_runtime maicord_uia kernel32 user32 ole32 oleaut32
    TIMEOUT 10
    TEST maicord_accessibility)
# Queries a provider window through the system UI Automation client.
# Exclude the "uia" (or "display") label where UI Automation is unavailable.
maicord_add_tests(MaiCordAccessibilityTests TIMEOUT 10 LABELS uia display
    TEST maicord_accessibility_uia_probe --uia-probe)
maicord_test(MaiCordAccountPermissionStorageTests ENTRY maicord_account_permission_storage_entry
    SOURCES tests/account_permission_storage.cpp src/app/account_permission_storage.cpp
    LIBS maicord_core maicord_protocol maicord_model kernel32
    INCLUDES src/app
    BUNDLE MaiCordModelTests
    TEST maicord_account_permission_storage)
maicord_test(MaiCordHistoryDecodeStorageTests ENTRY maicord_history_decode_storage_entry
    SOURCES tests/history_decode_storage.cpp src/app/application_message_send.cpp
        src/app/application_thread_actions.cpp src/app/application_message_edit.cpp
        src/app/application_state.cpp src/core/log.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_message_runtime maicord_ui
        maicord_model maicord_render_core kernel32 user32
    INCLUDES src/app
    TEST maicord_history_decode_storage)
maicord_test(MaiCordReactionHistoryRuntimeTests ENTRY maicord_application_reaction_history_runtime_entry
    SOURCES tests/application_reaction_history_runtime.cpp src/app/application_state.cpp src/core/log.cpp
        src/app/application_thread_actions.cpp src/app/application_startup_history.cpp
        src/app/application_startup_shell.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_message_runtime
        maicord_message_media maicord_ui maicord_model maicord_render_core kernel32 user32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_reaction_history_runtime)
maicord_test(MaiCordMessageReactionRuntimeTests ENTRY mainCRTStartup
    SOURCES tests/application_message_reaction_runtime.cpp src/app/application_state.cpp
        src/app/application_message_mutation_notice.cpp src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text kernel32 user32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_message_reaction_runtime)
maicord_test(MaiCordReactionPreferencesStorageTests ENTRY maicord_reaction_preference_storage_entry
    SOURCES tests/reaction_preferences_storage.cpp
    LIBS maicord_core maicord_platform maicord_model kernel32
    INCLUDES src/app
    BUNDLE MaiCordModelTests
    TEST maicord_reaction_preference_storage)
maicord_test(MaiCordRegionalFlagTests ENTRY maicord_regional_flag_test_entry
    SOURCES tests/regional_flag_test.cpp
    LIBS maicord_core maicord_platform maicord_ui maicord_cdn_media maicord_media_core kernel32
    TEST maicord_regional_flag_identity)
# Compile the alternate native geometry path without changing the shipping
# text-provider configuration or creating a second build tree.
maicord_test(MaiCordRegionalFlagNativeGeometryTests ENTRY maicord_flag_native_geometry_entry
    SOURCES tests/flag_native_geometry.cpp src/ui/native_text_candidate.cpp src/ui/native_text_catalog.cpp
        src/ui/emoji_presentation.cpp src/core/log.cpp
    LIBS maicord_core maicord_font maicord_media_core maicord_ui kernel32 advapi32 gdi32
    TIMEOUT 15 ENVIRONMENT LOCALAPPDATA=
    TEST maicord_regional_flag_native_geometry)
maicord_test(MaiCordRegionalFlagSvgTests ENTRY maicord_regional_flag_svg_test_entry
    SOURCES tests/regional_flag_svg_test.cpp
    LIBS maicord_core maicord_platform maicord_render_core maicord_paint_program kernel32
    TIMEOUT 15
    TEST maicord_regional_flag_svg --self-test)
set(MAICORD_REGIONAL_FLAG_REFERENCE_DIRECTORY "" CACHE PATH
    "External pinned licensed flag SVG corpus for the optional full reference check")
if(MAICORD_REGIONAL_FLAG_REFERENCE_DIRECTORY)
    if(NOT IS_DIRECTORY "${MAICORD_REGIONAL_FLAG_REFERENCE_DIRECTORY}")
        message(FATAL_ERROR "MAICORD_REGIONAL_FLAG_REFERENCE_DIRECTORY does not exist")
    endif()
    maicord_add_tests(MaiCordRegionalFlagSvgTests TIMEOUT 15 WORKING_DIRECTORY
            ${MAICORD_REGIONAL_FLAG_REFERENCE_DIRECTORY}
        TEST maicord_regional_flag_svg_reference)
endif()
maicord_test(MaiCordTextFormConfirmationTests ENTRY maicord_text_form_confirmation_entry
    SOURCES tests/application_text_form_confirmation_runtime.cpp tests/application_link_trust_runtime.cpp
        src/app/application_state.cpp src/app/application_text_form.cpp src/render/text_form_surface.cpp
        src/core/log.cpp
    LIBS maicord_core maicord_platform maicord_ui maicord_model maicord_app_core kernel32 user32
    INCLUDES src/app
    TEST maicord_text_form_confirmation)
maicord_test(MaiCordFriendsAccountTests ENTRY maicord_friends_account_entry
    SOURCES tests/account_relationships.cpp tests/rest_submit_hook.cpp src/app/account_relationships.cpp
        src/app/account_message_ack.cpp
    LIBS maicord_core maicord_protocol maicord_model maicord_account_host kernel32
    BUNDLE MaiCordAccountRequestTests
    TEST maicord_friends_account)
maicord_test(MaiCordFriendsStorageTests ENTRY maicord_friends_storage_entry
    SOURCES tests/application_friends_storage.cpp src/app/application_friends_storage.cpp
    LIBS maicord_core maicord_protocol maicord_model kernel32
    INCLUDES src/app
    BUNDLE MaiCordModelTests
    TEST maicord_friends_storage)
maicord_test(MaiCordFriendsRuntimeTests ENTRY maicord_friends_runtime_entry
    SOURCES tests/application_friends_runtime.cpp src/app/application_state.cpp src/app/application_friends.cpp
        src/app/application_friends_search.cpp src/app/application_friends_storage.cpp
        src/app/account_relationships.cpp src/core/log.cpp
    LIBS maicord_core maicord_protocol maicord_model maicord_app_core maicord_ui kernel32 user32
    INCLUDES src/app
    TEST maicord_friends_runtime)
maicord_test(MaiCordVoiceTests ENTRY maicord_voice_tests_entry
    SOURCES tests/aes_gcm.cpp tests/aes_gcm64.cpp tests/xchacha20_poly1305.cpp tests/voice_packet_crypto.cpp
        tests/voice_protocol_selection.cpp tests/voice_udp_media.cpp tests/voice_participants.cpp
        tests/voice_media_dispatch.cpp tests/dave_replay.cpp tests/dave_frame.cpp tests/dave_frame_crypto.cpp
        tests/dave_key_derivation.cpp tests/mls_exporter.cpp tests/mls_key_schedule.cpp tests/mls_signature.cpp
        tests/hpke_derivation.cpp tests/hpke_context.cpp tests/mls_hpke.cpp tests/mls_objects.cpp
        tests/mls_join_messages.cpp tests/dave_key_package.cpp tests/dave_welcome.cpp
        tests/dave_package_policy.cpp tests/p256.cpp tests/dave_receive_epoch.cpp tests/dave_receive_sender.cpp
        tests/voice_setup_entry.cpp tests/voice_connection_setup.cpp tests/gateway_voice_control.cpp
        tests/account_session_identity.cpp tests/voice_gateway.cpp tests/voice_heartbeat.cpp
        tests/voice_session.cpp tests/tls_stream_lifecycle.cpp tests/voice_socket.cpp tests/voice_connection.cpp
        tests/voice_media_events.cpp tests/voice_udp.cpp tests/voice_udp_handoff.cpp tests/gateway_voice.cpp
        tests/voice_state_store.cpp tests/voice_elapsed_timer.cpp tests/voice_channel_info.cpp
        tests/voice_gateway_policy.cpp
        tests/dave_session.cpp
        tests/video_rtp_receive.cpp tests/voice_jitter_buffer.cpp tests/voice_playback.cpp
        tests/voice_receive.cpp tests/call_view.cpp tests/stream_watch.cpp tests/stage_channel.cpp
        tests/voice_video_signaling.cpp tests/dave_video_frames.cpp tests/voice_video_sink.cpp
        tests/gateway_stream.cpp tests/voice_send.cpp tests/voice_preferences.cpp
        src/app/voice_settings_binding.cpp
    LIBS maicord_core maicord_protocol maicord_platform maicord_app_core maicord_model kernel32 crypt32 ws2_32
        secur32 bcrypt maicord_ui maicord_media_codec ole32 maicord_client_preferences user32
    INCLUDES src/app
    TEST maicord_voice_setup)
maicord_test(MaiCordTests ENTRY maicord_tests_entry
    SOURCES tests/test_main.cpp tools/font_reference/dwrite_text_system.cpp tests/account_credential_forget.cpp
        tests/account_logout_stop.cpp tests/account_module_dormant.cpp tests/account_inventory.cpp tests/media_parity.cpp
        tests/catalog_media.cpp tests/message_preparation.cpp tests/atlas_pressure.cpp
        tests/composer_geometry.cpp tests/reply_grouping.cpp tests/message_markdown_cache.cpp
        tests/message_interaction_rules.cpp tests/timeline_overlay_regressions.cpp tests/chat_display.cpp
        tests/message_live_updates.cpp tests/embed_colors.cpp tests/ansi_code_blocks.cpp
        tests/message_components.cpp tests/message_content_wave.cpp tests/embed_convergence.cpp
        tests/embed_parity.cpp tests/history_presence_recovery.cpp tests/profile_appearance.cpp
        tests/profile_decode_scratch.cpp tests/message_rich_index.cpp tests/cdn_response_storage.cpp
        tests/rest_response_storage.cpp tests/engineering_regressions.cpp tests/deflate_matches.cpp
        tests/deflate_differential.cpp tests/deflate_reference.cpp tests/shaped_text_cache.cpp
        src/render/shaped_text_cache.cpp tests/animation_modal_pause.cpp tests/mailbox_release_race.cpp
        tests/mailbox_acquire_release.cpp tests/responsiveness.cpp tests/gateway_ready_users.cpp
        tests/json_string_decode.cpp tests/resolver_publication.cpp tests/media_worker_publication.cpp
        tests/png_crc.cpp tests/frame_rate.cpp tests/layout_anchors.cpp
        tests/widget_input.cpp tests/text_form.cpp tests/application_forum_form.cpp
        tests/secure_edit_history.cpp tests/text_composition_history.cpp tests/text_input_runs.cpp
        tests/widget_scroll.cpp tests/input_router.cpp tests/navigation_shortcuts.cpp
        tests/shortcut_registry.cpp tests/list_paging.cpp tests/button_keyboard.cpp tests/attachment_upload.cpp
        tests/composer_attachments.cpp tests/composer_attachment_strip.cpp tests/composer_thumbnails.cpp
        tests/composer_preview_owner.cpp tests/composer_history.cpp tests/application_commands.cpp
        tests/command_autocomplete.cpp tests/composer_mentions.cpp tests/command_argument_editing.cpp
        tests/account_auth_transition.cpp tests/account_requirement.cpp tests/http_compression_workers.cpp
        tests/client_request_metadata.cpp tests/composer_edit_menu.cpp tests/composer_spellcheck.cpp
        src/app/application_composer_edit_menu.cpp tests/message_send_response.cpp tests/network_file.cpp
        tests/network_worker_async.cpp tests/file_picker.cpp tests/clipboard_attachment.cpp
        tests/image_decode_corpus.cpp tests/image_viewer.cpp tests/history_scrollback.cpp
        tests/timeline_window.cpp tests/message_snapshot_cache.cpp src/app/application_scrolling.cpp
        src/app/application_read_state.cpp src/app/application_scoped_read.cpp
        src/app/application_message_mutation_notice.cpp tests/pane_resize.cpp
        src/app/application_pane_resize.cpp tests/file_drop.cpp tests/attachment_drafts.cpp
        tests/application_attachments.cpp src/app/application_upload_lane.cpp src/core/log.cpp
        src/app/application_attachments.cpp src/app/application_composer_preview.cpp
        src/app/application_attachment_probe.cpp src/app/attachment_media_probe.cpp
        src/app/application_attachment_allowance.cpp src/app/application_state.cpp
        tests/private_channel_state.cpp tests/private_membership.cpp tests/private_snapshot.cpp
        tests/gateway_state_recovery.cpp tests/navigation_restore.cpp tests/channel_memory.cpp tests/startup_shell.cpp
        tests/private_order.cpp
        tests/private_rail_pins.cpp
        tests/private_conversation.cpp tests/private_channel_actions.cpp tests/media_fixture.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_webauthn maicord_account_paths
        maicord_account_host maicord_text maicord_ui maicord_ui_input maicord_model maicord_message_runtime
        maicord_message_media maicord_render_core maicord_media_core maicord_media_codec maicord_cdn_media
        maicord_account_media kernel32 user32 ws2_32 secur32 bcrypt crypt32 dwrite
    INCLUDES src/app
    TEST maicord_core)
maicord_test(MaiCordCommandUiTests ENTRY maicord_command_ui_tests_entry
    SOURCES tests/application_commands_ui.cpp src/app/application_commands.cpp src/app/application_state.cpp
        src/app/application_message_feedback.cpp
        tests/application_activity_retry.cpp tests/application_custom_status_retry.cpp
        tests/application_notification_retry.cpp tests/application_community_retry.cpp
        tests/application_invite_retry.cpp
        src/core/log.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_text maicord_ui
        maicord_ui_input maicord_model maicord_message_runtime kernel32 user32 ws2_32 secur32 bcrypt crypt32
    INCLUDES src/app
    TEST maicord_command_ui)
maicord_test(MaiCordMediaVisibilityTests ENTRY maicord_media_visibility_tests_entry
    SOURCES tests/application_media_visibility.cpp tests/stream_preview_media_stub.cpp
        src/app/application_media.cpp src/app/application_media_animation.cpp
        src/app/application_media_requests.cpp src/app/application_state.cpp src/core/log.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_text maicord_ui
        maicord_ui_input maicord_model maicord_message_runtime maicord_message_media maicord_render_core
        maicord_media_core maicord_account_media maicord_atlas_gpu maicord_renderer kernel32 user32 ws2_32
        secur32 bcrypt crypt32 d3d11 dxgi dcomp
    INCLUDES src/app
    TEST maicord_media_visibility)
add_executable(MaiCordAccountPumpTests tests/application_account_pump.cpp
    src/app/application_thread_header_activation.cpp
    tests/application_account_text_retry_runtime.hpp
    src/app/application_account_pump.cpp src/app/application_scheduler.cpp src/app/application_channel_memory.cpp
    src/app/account_host.cpp src/app/application_state.cpp src/core/log.cpp
    src/app/application_message_mutation_notice.cpp
    $<TARGET_OBJECTS:maicord_runtime>)
target_link_libraries(MaiCordAccountPumpTests PRIVATE
    maicord_compile_contract maicord_core maicord_platform maicord_protocol maicord_app_core
    maicord_text maicord_ui maicord_ui_input maicord_model maicord_message_runtime maicord_media_core
    kernel32 user32 ws2_32 secur32 bcrypt crypt32)
target_link_options(MaiCordAccountPumpTests PRIVATE
    /ENTRY:maicord_account_pump_tests_entry /SUBSYSTEM:CONSOLE,6.02)
maicord_add_tests(MaiCordAccountPumpTests TEST maicord_account_pump)
maicord_test(MaiCordSurfaceMotionTests ENTRY maicord_surface_motion_entry
    SOURCES tests/application_surface_motion.cpp src/app/application_surface_motion.cpp
    LIBS maicord_core maicord_ui kernel32
    INCLUDES src/app
    DEFINES MAICORD_SURFACE_MOTION_CONTROLLER_TEST=1)
maicord_test(MaiCordChannelOverviewRuntimeTests ENTRY maicord_application_channel_overview_runtime_entry
    SOURCES tests/application_channel_overview_runtime.cpp src/app/application_settings_layer.cpp
        src/app/application_state.cpp src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text kernel32 user32 shell32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_channel_overview_runtime)
maicord_test(MaiCordChannelOverviewAccountTests ENTRY maicord_channel_overview_account_entry
    SOURCES tests/account_channel_overview.cpp tests/rest_submit_hook.cpp src/app/account_channel_overview.cpp
    LIBS maicord_core maicord_protocol maicord_model maicord_account_host kernel32
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_channel_overview_account)
maicord_test(MaiCordPermissionComputeTests ENTRY maicord_permission_compute_entry
    SOURCES tests/permission_compute.cpp
    LIBS maicord_core maicord_protocol maicord_model kernel32
    BUNDLE MaiCordModelTests
    TIMEOUT 10
    TEST maicord_permission_compute)
maicord_test(MaiCordCookieJarTests ENTRY maicord_cookie_jar_entry
    SOURCES tests/cookie_jar.cpp
    LIBS maicord_core maicord_protocol kernel32
    BUNDLE MaiCordModelTests
    TIMEOUT 10
    TEST maicord_cookie_jar)
maicord_test(MaiCordUserSettingsTests ENTRY maicord_user_settings_tests_entry
    SOURCES tests/user_settings_proto.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32
    TIMEOUT 10
    TEST maicord_user_settings)
maicord_test(MaiCordGuildSettingsTests ENTRY maicord_guild_settings_entry
    SOURCES tests/guild_settings_model.cpp
    LIBS maicord_core maicord_protocol maicord_model kernel32
    BUNDLE MaiCordModelTests
    TIMEOUT 10
    TEST maicord_guild_settings)
# Role manager: tri-state permissions and switch, role reorder and
# hierarchy, View Server as Role and the role editor's locks and drafts.
maicord_test(MaiCordRoleManagerTests ENTRY maicord_role_manager_entry
    SOURCES tests/role_manager.cpp
    LIBS maicord_core maicord_text maicord_ui maicord_protocol maicord_model kernel32 user32
    BUNDLE MaiCordModelTests
    TIMEOUT 10
    TEST maicord_role_manager)
# Members search, bulk ban and ban search: exact bodies, targets, replies,
# paging and the transport.
maicord_test(MaiCordGuildModerationTests ENTRY maicord_guild_moderation_entry
    SOURCES tests/guild_moderation.cpp tests/rest_submit_hook.cpp src/app/account_guild_moderation.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model kernel32
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 10
    TEST maicord_guild_moderation)
maicord_test(MaiCordGuildAdminAccountTests ENTRY maicord_account_guild_admin_entry
    SOURCES tests/account_guild_admin.cpp tests/rest_submit_hook.cpp src/app/account_guild_admin.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_account_host kernel32
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_guild_admin_account)
# Stage requests: exact targets, methods, buckets and bodies at the transport.
maicord_test(MaiCordStageAccountTests ENTRY maicord_account_stage_entry
    SOURCES tests/account_stage.cpp tests/rest_submit_hook.cpp src/app/account_stage.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 10
    TEST maicord_stage_account)
# Soundboard: effect/sound decoding, request bodies, gating, upload checks,
# the clip cache and mixer, and the module's fetch-decode-play path (stubs).
maicord_test(MaiCordSoundboardTests ENTRY maicord_soundboard_entry
    SOURCES tests/soundboard.cpp src/app/account_soundboard.cpp src/app/soundboard_audio.cpp
        src/app/voice_mixer.cpp
    LIBS maicord_core maicord_protocol maicord_media_codec kernel32
    TIMEOUT 10
    TEST maicord_soundboard)
# Notes, report menus/submissions and message flags: exact targets and bodies.
maicord_test(MaiCordUserSocialAccountTests ENTRY maicord_account_user_social_entry
    SOURCES tests/account_user_social.cpp tests/rest_submit_hook.cpp src/app/account_user_social.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 10
    TEST maicord_user_social_account)
maicord_test(MaiCordGuildAssetsTests ENTRY maicord_guild_assets_entry
    SOURCES tests/guild_assets.cpp tests/rest_submit_hook.cpp src/app/account_guild_assets.cpp
        src/app/guild_image.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_media_codec maicord_compression kernel32 user32
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_guild_assets)
# Avatar/banner crop: cropper geometry, PNG/zlib encoder, still and GIF crops.
maicord_test(MaiCordProfileImageTests ENTRY maicord_profile_image_entry
    SOURCES tests/profile_image.cpp tests/media_fixture.cpp src/app/profile_image.cpp src/app/guild_image.cpp
        src/app/local_preview_thumbnail.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_media_codec maicord_compression
        kernel32 user32
    TIMEOUT 10
    TEST maicord_profile_image)
# Profile editor: PATCH bodies at the transport, loads, gates, drafts, errors.
maicord_test(MaiCordProfileEditorTests ENTRY maicord_profile_editor_entry
    SOURCES tests/profile_editor.cpp tests/rest_submit_hook.cpp src/app/account_profile_edit.cpp
        src/app/profile_image.cpp src/app/guild_image.cpp src/app/local_preview_thumbnail.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_media_codec maicord_compression
        kernel32 user32
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 10
    TEST maicord_profile_editor)
maicord_test(MaiCordMediaWorkerPosterLeaseTests ENTRY maicord_media_worker_poster_lease_entry
    SOURCES tests/media_worker_poster_lease.cpp
    LIBS maicord_account_media kernel32 ole32
    TIMEOUT 10
    TEST maicord_media_worker_poster_lease)
# Media decode lanes: threads start on first use; lane stacks keep a margin
# over the deepest decode (every image codec and the largest bundled flag).
maicord_test(MaiCordMediaWorkerLanesTests ENTRY maicord_media_worker_lanes_entry
    SOURCES tests/media_worker_lanes.cpp tests/media_fixture.cpp
    LIBS maicord_account_media maicord_media_codec kernel32
    TIMEOUT 10
    TEST maicord_media_worker_lanes)
maicord_test(MaiCordGuildAdminGatewayTests ENTRY maicord_guild_admin_gateway_entry
    SOURCES tests/guild_admin_gateway.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_app_core maicord_message_runtime
        kernel32
    BUNDLE MaiCordAppUnitTests
    TIMEOUT 15
    TEST maicord_guild_admin_gateway)
maicord_add_tests(MaiCordSurfaceMotionTests TIMEOUT 10 TEST maicord_surface_motion)
maicord_test(MaiCordSurfaceMotionHostTests ENTRY maicord_surface_motion_host_entry
    SOURCES tests/application_surface_motion_host.cpp src/app/application_surface_motion.cpp
        src/app/application_state.cpp src/render/layer_presentation.cpp
    LIBS maicord_core maicord_ui maicord_render_core kernel32 user32
    INCLUDES src/app
    TIMEOUT 10
    TEST maicord_surface_motion_host)
# Draw submission: pipeline, shared-state and scissor binds, merged draw runs.
maicord_test(MaiCordDrawSubmissionTests ENTRY maicord_draw_submission_entry
    SOURCES tests/draw_submission.cpp
    LIBS maicord_core kernel32
    BUNDLE MaiCordCoreUnitTests
    TIMEOUT 5
    TEST maicord_draw_submission)
maicord_test(MaiCordTooltipRuntimeTests ENTRY maicord_tooltip_runtime_entry
    SOURCES tests/application_tooltip_runtime.cpp src/app/application_tooltip.cpp src/app/application_state.cpp
        tests/application_reaction_tooltip_runtime.cpp
        src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_message_runtime kernel32 user32
    INCLUDES src/app
    TIMEOUT 10
    TEST maicord_tooltip_runtime)
maicord_test(MaiCordAccountMenuRuntimeTests ENTRY maicord_account_menu_runtime_entry
    SOURCES tests/application_account_menu_runtime.cpp src/app/application_account_menu.cpp
        src/app/application_state.cpp src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_message_runtime kernel32 user32
    INCLUDES src/app
    TIMEOUT 10
    TEST maicord_account_menu_runtime)
maicord_test(MaiCordDmProfileTests ENTRY maicord_dm_profile_tests_entry
    SOURCES tests/dm_profile_panel.cpp src/app/application_dm_profile.cpp src/app/application_state.cpp
        src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_message_runtime kernel32 user32
    INCLUDES src/app
    BUNDLE MaiCordAppUnitTests
    TIMEOUT 10
    TEST maicord_dm_profile)
maicord_test(MaiCordProfileRuntimeTests ENTRY maicord_profile_runtime_entry
    SOURCES tests/application_profile_runtime.cpp src/app/application_profile.cpp
        src/app/application_state.cpp src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_message_runtime kernel32 user32 bcrypt
    INCLUDES src/app
    TIMEOUT 10
    TEST maicord_profile_runtime)
maicord_test(MaiCordAccountMessageSearchTests ENTRY maicord_account_message_search_test_entry
    SOURCES tests/account_message_search.cpp tests/rest_submit_hook.cpp src/app/account_message_search.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 10
    TEST maicord_account_message_search)
maicord_test(MaiCordAccountPinnedMessagesTests ENTRY maicord_account_pinned_messages_test_entry
    SOURCES tests/account_pinned_messages.cpp tests/rest_submit_hook.cpp src/app/account_pinned_messages.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 10
    TEST maicord_account_pinned_messages)
maicord_test(MaiCordAccountThreadActionsTests ENTRY maicord_account_thread_actions_test_entry
    SOURCES tests/account_thread_actions.cpp tests/rest_submit_hook.cpp src/app/account_thread_actions.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_account_thread_actions)
maicord_test(MaiCordThreadBrowserProtocolTests ENTRY maicord_thread_browser_protocol_test_entry
    SOURCES tests/thread_browser_protocol.cpp
    LIBS maicord_core maicord_protocol maicord_model kernel32
    TIMEOUT 15
    TEST maicord_thread_browser_protocol)
maicord_test(MaiCordAccountThreadBrowserTests ENTRY maicord_account_thread_browser_test_entry
    SOURCES tests/account_thread_browser.cpp tests/rest_submit_hook.cpp src/app/account_thread_browser.cpp
    LIBS maicord_core maicord_protocol maicord_model maicord_account_host kernel32
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_account_thread_browser)
maicord_test(MaiCordApplicationThreadBrowserTests ENTRY maicord_application_thread_browser_entry
    SOURCES tests/application_thread_browser.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text kernel32 user32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_application_thread_browser)
maicord_test(MaiCordApplicationPollTests ENTRY maicord_application_poll_entry
    SOURCES tests/application_poll.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text kernel32 user32 shell32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_application_poll)
maicord_test(MaiCordThreadCreationProtocolTests ENTRY maicord_thread_creation_protocol_test_entry
    SOURCES tests/thread_creation_protocol.cpp
    LIBS maicord_core maicord_protocol kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 15
    TEST maicord_thread_creation_protocol)
maicord_test(MaiCordAccountThreadCreationTests ENTRY maicord_account_thread_creation_test_entry
    SOURCES tests/account_thread_creation.cpp tests/rest_submit_hook.cpp src/app/account_thread_creation.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_account_host kernel32
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_account_thread_creation)
maicord_test(MaiCordAccountPollCreateTests ENTRY maicord_account_poll_create_test_entry
    SOURCES tests/account_poll_create.cpp tests/rest_submit_hook.cpp src/app/account_poll_create.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_account_host kernel32
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_account_poll_create)
maicord_test(MaiCordAccountNotificationSettingsTests ENTRY maicord_account_notification_settings_test_entry
    SOURCES tests/account_notification_settings.cpp tests/rest_submit_hook.cpp src/app/account_notification_settings.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_account_host kernel32
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_account_notification_settings)
maicord_test(MaiCordAccountPrivateChannelTests ENTRY maicord_account_private_channel_test_entry
    SOURCES tests/account_private_channel.cpp tests/rest_submit_hook.cpp src/app/account_private_channel.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_account_host kernel32
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_account_private_channel)
maicord_test(MaiCordApplicationThreadCreationTests ENTRY maicord_application_thread_creation_entry
    SOURCES tests/application_thread_creation.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text kernel32 user32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_application_thread_creation)
maicord_test(MaiCordApplicationThreadActionsRuntimeTests ENTRY maicord_application_thread_actions_runtime_entry
    SOURCES tests/application_thread_actions_runtime.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_app_core kernel32
    INCLUDES src/app)
maicord_test(MaiCordForumPostProtocolTests ENTRY maicord_forum_post_protocol_test_entry
    SOURCES tests/forum_post_protocol.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 15
    TEST maicord_forum_post_protocol)
maicord_test(MaiCordAccountForumPostTests ENTRY maicord_account_forum_post_test_entry
    SOURCES tests/account_forum_post.cpp tests/rest_submit_hook.cpp src/app/account_forum_post.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_account_host kernel32
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_account_forum_post)
maicord_test(MaiCordApplicationForumAttachmentsTests ENTRY maicord_application_forum_attachments_entry
    SOURCES tests/application_forum_attachments.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text kernel32 user32 shell32
    INCLUDES src/app
    TIMEOUT 20
    TEST maicord_application_forum_post --forum-post
    TEST maicord_application_forum_attachments)
maicord_test(MaiCordApplicationForumPreviewTests ENTRY maicord_application_forum_preview_entry
    SOURCES tests/application_forum_preview.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text maicord_render_core kernel32
        user32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_application_forum_preview)
maicord_test(MaiCordApplicationContentNavigationTests ENTRY maicord_application_content_navigation_entry
    SOURCES tests/application_content_navigation.cpp src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text maicord_render_core kernel32
        user32 shell32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_application_content_navigation)
maicord_add_tests(MaiCordApplicationThreadActionsRuntimeTests TIMEOUT 15
    TEST maicord_application_thread_actions_runtime)
maicord_test(MaiCordMessagePinnedStateTests ENTRY maicord_message_pinned_state_entry
    SOURCES tests/message_pinned_state.cpp
    LIBS maicord_core maicord_protocol maicord_model maicord_ui maicord_message_runtime maicord_app_core
        maicord_platform kernel32 ws2_32 secur32 bcrypt crypt32
    BUNDLE MaiCordAppUnitTests
    TIMEOUT 15
    TEST maicord_message_pinned_state)
maicord_test(MaiCordForwardedTextContractTests ENTRY maicord_forwarded_text_contract_entry
    SOURCES tests/forwarded_text_contract.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_ui maicord_ui_input
        maicord_message_runtime maicord_text kernel32 user32
    BUNDLE MaiCordModelTests
    TIMEOUT 15
    TEST maicord_forwarded_text_contract)
maicord_test(MaiCordReceivedAttachmentOpenTests ENTRY maicord_received_attachment_open_test_entry
    SOURCES tests/received_attachment_open.cpp src/app/received_attachment_open.cpp
        src/app/media_url_resolver.cpp
    LIBS maicord_core maicord_protocol kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 10
    TEST maicord_received_attachment_open)
maicord_test(MaiCordMessageMutationNoticeTests ENTRY maicord_message_mutation_notice_tests_entry
    SOURCES tests/application_message_mutation_notice.cpp src/app/application_message_mutation_notice.cpp
        src/app/application_thread_actions.cpp src/app/application_message_delete.cpp
        src/app/application_message_reaction.cpp src/app/application_state.cpp src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text kernel32 user32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_message_mutation_notice)
maicord_test(MaiCordGatewaySelfStatusRuntimeTests ENTRY maicord_gateway_self_status_runtime_entry
    SOURCES tests/gateway_self_status_runtime.cpp src/app/gateway_client.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_model kernel32 user32 ws2_32
        secur32 bcrypt crypt32
    INCLUDES src/app
    DEFINES MAICORD_GATEWAY_SELF_STATUS_TEST=1
    TIMEOUT 10
    TEST maicord_gateway_self_status_runtime)
maicord_test(MaiCordSelfStatusRuntimeTests ENTRY maicord_self_status_runtime_entry
    SOURCES tests/application_self_status_runtime.cpp src/app/application_self_status.cpp
        src/app/application_state.cpp src/core/log.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_model maicord_ui kernel32
        user32
    INCLUDES src/app
    BUNDLE MaiCordAppUnitTests
    TIMEOUT 10
    TEST maicord_self_status_frontend_runtime)
maicord_test(MaiCordSearchRuntimeTests ENTRY maicord_application_search_runtime_entry
    SOURCES tests/application_search_runtime.cpp src/app/application_state.cpp src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text kernel32 user32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_search_runtime)
maicord_test(MaiCordSearchContentRuntimeTests ENTRY maicord_application_search_content_runtime_entry
    SOURCES tests/application_search_content_runtime.cpp src/app/application_state.cpp src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text kernel32 user32 shell32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_search_content_runtime)
maicord_test(MaiCordSearchNavigationRuntimeTests ENTRY maicord_application_search_navigation_runtime_entry
    SOURCES tests/application_search_navigation_runtime.cpp src/app/application_message_mutation_notice.cpp
        src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text kernel32 user32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_search_navigation_runtime)
maicord_test(MaiCordImageViewerRuntimeTests ENTRY maicord_application_image_viewer_runtime_entry
    SOURCES tests/application_image_viewer_runtime.cpp src/app/application_state.cpp src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text maicord_render_core
        kernel32 user32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_image_viewer_runtime)
maicord_test(MaiCordQuickSwitcherRuntimeTests ENTRY maicord_application_quick_switcher_runtime_entry
    SOURCES tests/application_quick_switcher_runtime.cpp src/app/application_quick_switcher.cpp
        src/app/application_input_routing.cpp src/app/application_state.cpp src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text kernel32 user32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_quick_switcher_runtime)
maicord_test(MaiCordMessageForwardRuntimeTests ENTRY maicord_application_message_forward_runtime_entry
    SOURCES tests/application_message_forward_runtime.cpp src/app/application_input_routing.cpp
        src/app/application_state.cpp src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text kernel32 user32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_message_forward_runtime)
maicord_test(MaiCordDownloadsRuntimeTests ENTRY maicord_application_downloads_runtime_entry
    SOURCES tests/application_downloads_runtime.cpp src/app/application_state.cpp src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text kernel32 user32 shell32 ole32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_downloads_runtime)
# Account restart schedule: backoff with jitter, attempt cap, user stops, banner states and wakes.
maicord_test(MaiCordAccountReconnectTests ENTRY maicord_account_reconnect_tests_entry
    SOURCES tests/account_reconnect.cpp
    LIBS maicord_account_paths maicord_core kernel32
    BUNDLE MaiCordCoreUnitTests
    TIMEOUT 5
    TEST maicord_account_reconnect)
# Toast queue: order, merge, overflow, timing by update and wake counts, placement and paint.
maicord_test(MaiCordToastTests ENTRY maicord_toast_tests_entry
    SOURCES tests/toast.cpp
    LIBS maicord_core maicord_ui kernel32 user32
    BUNDLE MaiCordUiUnitTests
    TIMEOUT 5
    TEST maicord_toast)
# Server Settings invite rows: Copy Link through the clipboard sink, its toast and Revoke.
maicord_test(MaiCordInviteCopyRuntimeTests ENTRY maicord_invite_copy_runtime_entry
    SOURCES tests/application_invite_copy_runtime.cpp src/app/application_state.cpp src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text maicord_render_core
        kernel32 user32
    INCLUDES src/app
    TIMEOUT 10
    TEST maicord_invite_copy_runtime)
maicord_test(MaiCordTlsStreamTailTests ENTRY maicord_tls_stream_tail_entry
    SOURCES tests/tls_stream_tail_entry.cpp src/platform/tls_stream.cpp
    LIBS maicord_core maicord_platform kernel32 ws2_32 secur32 bcrypt crypt32
    TIMEOUT 15
    TEST maicord_tls_stream_tail)
maicord_test(MaiCordPinnedMessagesRuntimeTests ENTRY maicord_application_pinned_messages_runtime_entry
    SOURCES tests/application_pinned_messages_runtime.cpp src/app/application_state.cpp src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text kernel32 user32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_pinned_messages_runtime)
maicord_test(MaiCordMediaPickerRuntimeTests ENTRY maicord_application_media_picker_runtime_entry
    SOURCES tests/application_media_picker_runtime.cpp src/app/application_state.cpp src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime kernel32 user32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_media_picker_runtime)
maicord_test(MaiCordGifFavoritesRuntimeTests ENTRY maicord_application_gif_favorites_runtime_entry
    SOURCES tests/application_gif_favorites_runtime.cpp src/app/application_state.cpp
    LIBS ${maicord_app_test_libs} kernel32 user32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_gif_favorites_runtime)
# Includes collection navigation, canonical embed queue recovery, joined restarts and failure recovery.
maicord_test(MaiCordVideoFocusRuntimeTests ENTRY maicord_video_focus_runtime_entry
    SOURCES tests/application_video_focus_runtime.cpp src/app/application_video.cpp
        src/app/application_state.cpp src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_message_runtime kernel32 user32
    INCLUDES src/app
    TIMEOUT 10
    TEST maicord_video_focus_runtime)
maicord_test(MaiCordLoginCancelRuntimeTests ENTRY maicord_login_cancel_runtime_entry
    SOURCES tests/application_login_cancel_runtime.cpp src/app/application_browser_auth.cpp
        src/app/application_captcha_auth.cpp src/app/application_login_auth.cpp src/app/application_state.cpp
        src/app/account_host.cpp src/core/log.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_text maicord_ui
        maicord_ui_input maicord_model maicord_account_paths kernel32 user32 ws2_32 secur32 bcrypt crypt32
    INCLUDES src/app
    BUNDLE MaiCordAppUnitTests)
maicord_test(MaiCordNotificationTests ENTRY maicord_notifications_tests_entry
    SOURCES tests/notification_policy.cpp tests/notification_settings_wire.cpp
        tests/desktop_notification_queue.cpp tests/notification_sound_policy.cpp
        tests/notification_sound_admission.cpp tests/notification_controls.cpp tests/notification_rules.cpp
    LIBS maicord_core maicord_model maicord_protocol kernel32
    TEST maicord_notifications)
# tests/toast_abi.cpp checks the client's WinRT toast ABI against the SDK's
# headers, so it compiles against them; runtimeobject: the SDK string calls it
# compares the client's against.
add_library(maicord_toast_abi_conformance OBJECT tests/toast_abi.cpp)
target_link_libraries(maicord_toast_abi_conformance PRIVATE maicord_compile_contract)
set_target_properties(maicord_toast_abi_conformance PROPERTIES CXX_EXTENSIONS OFF MAICORD_SDK_INCLUDES ON)
maicord_test(MaiCordNativeNotificationTests ENTRY maicord_native_notifications_tests_entry
    SOURCES tests/native_notifications.cpp $<TARGET_OBJECTS:maicord_toast_abi_conformance>
        tests/integration/desktop_notifications_probe.cpp
    LIBS maicord_core maicord_platform kernel32 runtimeobject
    TEST maicord_native_notifications)
# Compile the explicit enrollment branch without invoking it: this fixture
# substitutes a copied notification sink and never creates desktop identity.
maicord_test(MaiCordApplicationNotificationTests ENTRY maicord_application_notifications_tests_entry
    SOURCES tests/application_notifications.cpp
        src/app/application_notifications.cpp src/app/application_state.cpp src/core/log.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_model
        maicord_client_preferences maicord_notification_sound_assets maicord_ui maicord_text
        maicord_message_runtime kernel32 user32 shell32
    INCLUDES src/app
    DEFINES MAICORD_NOTIFICATION_FIXTURE=1
    TEST maicord_application_notifications)
maicord_add_tests(MaiCordLoginCancelRuntimeTests TEST maicord_login_cancel_runtime)
maicord_test(MaiCordPasswordRecoveryCoordinatorTests ENTRY maicord_password_recovery_coordinator_entry
    SOURCES tests/password_recovery_coordinator.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core kernel32 ws2_32 secur32 bcrypt crypt32
    TEST maicord_password_recovery_coordinator)
maicord_test(MaiCordCaptchaBridgeTests ENTRY maicord_captcha_bridge_tests_entry
    SOURCES tests/captcha_bridge_login.cpp tests/captcha_bridge_runtime.cpp tests/captcha_bridge_protocol.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_ui kernel32 user32 ws2_32
        secur32 bcrypt crypt32 shell32
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_captcha_bridge)
maicord_test(MaiCordBrowserHandoffExchangeTests ENTRY maicord_browser_handoff_exchange_coordinator_entry
    SOURCES tests/browser_handoff_exchange_coordinator.cpp tests/browser_handoff_exchange_protocol.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core kernel32 ws2_32 secur32 bcrypt crypt32
    TIMEOUT 15
    TEST maicord_browser_handoff_exchange)
maicord_test(MaiCordPhoneRecoveryCoordinatorTests ENTRY maicord_phone_recovery_coordinator_entry
    SOURCES tests/phone_recovery_coordinator.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core kernel32 ws2_32 secur32 bcrypt crypt32
    TIMEOUT 15
    TEST maicord_phone_recovery_coordinator)
maicord_test(MaiCordPasswordResetProtocolTests ENTRY maicord_password_reset_protocol_entry
    SOURCES tests/password_reset_protocol.cpp
        src/app/login_continuations.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32 bcrypt crypt32
    TEST maicord_password_reset_protocol)
maicord_test(MaiCordPasswordResetCoordinatorTests ENTRY maicord_password_reset_coordinator_entry
    SOURCES tests/password_reset_coordinator.cpp src/app/account_profile.cpp src/app/login_mfa_policy.cpp
        src/app/file_persistence.cpp src/app/captcha_challenge.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32 ws2_32 secur32 bcrypt crypt32
    TEST maicord_password_reset_coordinator)
# Offline fixtures over the shipping REST owner share one translation unit, so
# the owner implementation they include compiles once; each case runs one.
maicord_test(MaiCordRestClientFixtureTests ENTRY maicord_rest_client_fixtures_entry
    SOURCES tests/rest_client_fixtures.cpp src/app/account_bulk_message_ack.cpp src/app/account_logout.cpp
        src/app/account_host.cpp src/media/png_common.cpp src/app/user_client_configuration.cpp
        src/app/client_session_metadata.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_compression kernel32 ws2_32 secur32 bcrypt
        crypt32 ntdll
    TIMEOUT 15
    TEST maicord_reset_rest_storage reset-storage
    TEST maicord_user_settings_rest_storage user-settings-storage
    TEST maicord_account_logout_transport logout-transport
    TEST maicord_account_bulk_read_transport bulk-read-transport
    TEST maicord_account_action_captcha_transport action-captcha-transport)
maicord_test(MaiCordAccountLogoutRuntimeTests ENTRY maicord_application_account_logout_runtime_entry
    SOURCES tests/application_account_logout_runtime.cpp src/app/account_logout_stop.cpp
        src/app/account_credential_forget.cpp src/app/account_paths.cpp src/app/file_persistence.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_ui maicord_account_host kernel32
        user32
    INCLUDES src/app
    TIMEOUT 20
    TEST maicord_account_logout_runtime)
add_library(MaiCordSecureBufferLockBudgetPeer MODULE tests/secure_buffer_lock_budget.cpp
    $<TARGET_OBJECTS:maicord_runtime>)
target_compile_definitions(MaiCordSecureBufferLockBudgetPeer PRIVATE MAICORD_LOCK_BUDGET_PEER_BUILD=1)
target_link_libraries(MaiCordSecureBufferLockBudgetPeer PRIVATE maicord_compile_contract kernel32 crypt32)
target_link_options(MaiCordSecureBufferLockBudgetPeer PRIVATE /NOENTRY /SUBSYSTEM:WINDOWS,6.02)
set_target_properties(MaiCordSecureBufferLockBudgetPeer PROPERTIES CXX_EXTENSIONS OFF)
maicord_test(MaiCordSecureBufferLockBudgetTests ENTRY maicord_secure_buffer_lock_budget_entry
    SOURCES tests/secure_buffer_lock_budget.cpp
    LIBS kernel32 crypt32 psapi
    DEPENDS MaiCordSecureBufferLockBudgetPeer
    TIMEOUT 20
    TEST maicord_secure_buffer_lock_budget)
maicord_test(MaiCordGuildChannelUniquenessTests ENTRY maicord_guild_channel_uniqueness_entry
    SOURCES tests/guild_channel_uniqueness.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_app_core maicord_message_runtime
        kernel32
    TIMEOUT 20
    TEST maicord_guild_channel_uniqueness)
maicord_test(MaiCordPluginCatalogFailureTests ENTRY maicord_plugin_catalog_failures_entry
    SOURCES tests/plugin_catalog_failures.cpp
    LIBS maicord_core maicord_plugin_host maicord_plugin_modules maicord_ui kernel32 bcrypt
    TIMEOUT 15
    TEST maicord_plugin_catalog_failures)
maicord_test(MaiCordScopedReadRuntimeTests ENTRY maicord_scoped_read_runtime_entry
    SOURCES tests/application_scoped_read_runtime.cpp src/app/application_read_state.cpp
        src/app/application_scoped_read.cpp src/app/application_message_mutation_notice.cpp
        src/app/application_state.cpp src/core/log.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_ui maicord_model
        maicord_message_runtime kernel32 user32
    INCLUDES src/app
    TIMEOUT 20
    TEST maicord_read_state_runtime --read-state
    TEST maicord_scoped_read_runtime)
maicord_test(MaiCordMp4Tests ENTRY maicord_mp4_tests_entry
    SOURCES tests/mp4_demux_test.cpp
    LIBS maicord_core maicord_media_codec kernel32
    BUNDLE MaiCordCodecUnitTests
    TEST maicord_mp4_demux)
maicord_test(MaiCordProgressiveVideoTests ENTRY maicord_progressive_video_tests_entry
    SOURCES tests/progressive_video.cpp
    LIBS maicord_core maicord_app_core maicord_protocol maicord_media_codec maicord_media_core kernel32
    TIMEOUT 10
    TEST maicord_progressive_video)
# AV1 header parsing and DXVA parameters on the synthetic SVT-AV1 fixture, with fuzzing.
maicord_test(MaiCordAv1Tests ENTRY maicord_av1_tests_entry
    SOURCES tests/av1.cpp src/platform/av1_dxva_parameters.cpp
    LIBS maicord_core maicord_media_codec kernel32
    INCLUDES src/platform
    BUNDLE MaiCordCodecUnitTests
    TIMEOUT 10
    TEST maicord_av1)
maicord_test(MaiCordH264Tests ENTRY maicord_h264_tests_entry
    SOURCES tests/h264_parameters_test.cpp tests/h264_state_test.cpp tests/h264_stream_test.cpp
        tests/hevc_stream_test.cpp
    LIBS maicord_core maicord_media_codec kernel32
    TEST maicord_h264_parameters)
# Live H.264 and H.265 stream decoders on real DXVA hardware with synthetic
# PCM/skip streams (1080p/1440p/4K included), the live AV1 decoder on a lossless
# SVT-AV1 fixture, then the attachment decoder on an in-memory MP4 through
# video_frame_gpu; skipped when no video device exists.
maicord_test(MaiCordH264StreamGpuTests ENTRY maicord_h264_stream_gpu_tests_entry
    SOURCES tests/h264_stream_gpu_test.cpp tests/hevc_stream_gpu_test.cpp src/platform/gpu_stream_decoder.cpp
        src/platform/h264_gpu_stream_decoder.cpp src/platform/hevc_gpu_stream_decoder.cpp src/app/voice_log.cpp
        tests/av1_stream_gpu_test.cpp src/platform/av1_gpu_stream.cpp src/platform/av1_dxva_parameters.cpp
        src/app/video_stream_decoders.cpp tests/h264_attachment_gpu_test.cpp src/platform/mp4_decoder.cpp
        src/platform/h264_gpu_decoder.cpp src/platform/hevc_gpu_decoder.cpp src/platform/h264_gpu_configuration.cpp
        src/platform/gpu_decoder_surfaces.cpp src/render/video_frame_gpu.cpp ${MAICORD_VIDEO_FRAME_HEADER}
    LIBS maicord_core maicord_media_codec maicord_shader_bytecode kernel32 d3d11 dxguid
    INCLUDES ${MAICORD_GENERATED_DIRECTORY}
    TIMEOUT 10 LABELS gpu RESOURCE_LOCK native_gpu
    TEST maicord_h264_stream_gpu)
maicord_test(MaiCordGlyphEffectTests ENTRY maicord_glyph_effect_entry
    SOURCES tests/glyph_effect_ordinal_test.cpp
    LIBS maicord_core kernel32
    BUNDLE MaiCordCoreUnitTests
    TIMEOUT 2 LABELS native-text-renderer
    TEST maicord_glyph_effect_ordinals)
maicord_shader(MAICORD_GLYPH_DISTANCE_REFERENCE_HEADER maicord_glyph_distance_reference_cs.hpp cs_5_0
        glyph_coverage g_maicord_glyph_distance_reference_shader tests/shaders/glyph_distance_reference.hlsl)
maicord_test(MaiCordGlyphDistanceProbe ENTRY maicord_glyph_distance_entry
    SOURCES tests/integration/glyph_distance_probe.cpp "${MAICORD_GLYPH_DISTANCE_REFERENCE_HEADER}"
    LIBS maicord_atlas_gpu maicord_core d3d11 kernel32
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}"
    BUNDLE MaiCordGpuUnitTests
    TIMEOUT 5 LABELS native-text-renderer gpu
    TEST maicord_glyph_distance)
maicord_test(MaiCordGlyphCapacityProbe ENTRY maicord_glyph_capacity_entry
    SOURCES tests/integration/glyph_capacity_probe.cpp
    LIBS maicord_atlas_gpu maicord_core d3d11 kernel32
    BUNDLE MaiCordGpuUnitTests
    TIMEOUT 5 LABELS native-text-renderer gpu
    TEST maicord_glyph_capacity)
maicord_test(MaiCordShaderBytecodeProbe ENTRY maicord_shader_bytecode_probe_entry
    SOURCES tests/integration/shader_bytecode_probe.cpp
    LIBS maicord_shader_bytecode maicord_compression maicord_media_codec maicord_core d3d11 kernel32
    BUNDLE MaiCordGpuUnitTests
    TIMEOUT 5 LABELS native-text-renderer gpu
    TEST maicord_shader_bytecode)
maicord_shader(MAICORD_GLYPH_LIST_VERTEX_HEADER maicord_glyph_list_vs.hpp vs_4_0 glyph_vertex
        g_maicord_glyph_list_vertex_shader src/render/shaders/glyph_draw.hlsl
        MAICORD_GLYPH_TRIANGLE_LIST_REFERENCE=1)
maicord_shader(MAICORD_GLYPH_EASING_VERTEX_HEADER maicord_glyph_easing_reference_vs.hpp vs_4_0 glyph_vertex
        g_maicord_glyph_easing_reference_vertex_shader tests/shaders/glyph_easing_reference.hlsl)
maicord_shader(MAICORD_GLYPH_EASING_PIXEL_HEADER maicord_glyph_easing_reference_ps.hpp ps_4_0 glyph_pixel
        g_maicord_glyph_easing_reference_pixel_shader tests/shaders/glyph_easing_reference.hlsl)
maicord_shader(MAICORD_GLYPH_STAGE_VERTEX_HEADER maicord_glyph_stage_reference_vs.hpp vs_4_0 glyph_vertex
        g_maicord_glyph_stage_reference_vertex_shader tests/shaders/glyph_stage_reference.hlsl)
maicord_shader(MAICORD_GLYPH_STAGE_PIXEL_HEADER maicord_glyph_stage_reference_ps.hpp ps_4_0 glyph_pixel
        g_maicord_glyph_stage_reference_pixel_shader tests/shaders/glyph_stage_reference.hlsl)
maicord_shader(MAICORD_GLYPH_COLOR_PIXEL_HEADER maicord_glyph_color_reference_ps.hpp ps_4_0 glyph_pixel
        g_maicord_glyph_color_reference_pixel_shader tests/shaders/glyph_color_reference.hlsl)
# One executable, four reference comparisons selected by MAICORD_GLYPH_PROBE.
# Stage and color controls 1 and 2 rerun them with recolored, re-sampled instances.
maicord_test(MaiCordGlyphTopologyProbe ENTRY maicord_glyph_topology_entry
    SOURCES tests/integration/glyph_topology_probe.cpp "${MAICORD_GLYPH_LIST_VERTEX_HEADER}"
        "${MAICORD_GLYPH_EASING_VERTEX_HEADER}" "${MAICORD_GLYPH_EASING_PIXEL_HEADER}"
        "${MAICORD_GLYPH_STAGE_VERTEX_HEADER}" "${MAICORD_GLYPH_STAGE_PIXEL_HEADER}"
        "${MAICORD_GLYPH_COLOR_PIXEL_HEADER}" "${MAICORD_GLYPH_VERTEX_HEADER}"
    LIBS maicord_renderer maicord_core d3d11 kernel32
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}"
    BUNDLE MaiCordGpuUnitTests
    TIMEOUT 5 LABELS native-text-renderer gpu
    TEST maicord_glyph_topology)
maicord_add_tests(MaiCordGlyphTopologyProbe TIMEOUT 5 LABELS native-text-renderer gpu
    ENVIRONMENT MAICORD_GLYPH_PROBE=easing TEST maicord_glyph_easing)
foreach(maicord_glyph_probe IN ITEMS stage color)
    maicord_add_tests(MaiCordGlyphTopologyProbe TIMEOUT 5 LABELS native-text-renderer gpu
        ENVIRONMENT MAICORD_GLYPH_PROBE=${maicord_glyph_probe} TEST maicord_glyph_${maicord_glyph_probe})
    foreach(maicord_glyph_control IN ITEMS 1 2)
        maicord_add_tests(MaiCordGlyphTopologyProbe TIMEOUT 5 LABELS native-text-renderer gpu ENVIRONMENT
                MAICORD_GLYPH_PROBE=${maicord_glyph_probe} MAICORD_GLYPH_STAGE_CONTROL=${maicord_glyph_control}
            TEST maicord_glyph_${maicord_glyph_probe}_control_${maicord_glyph_control})
    endforeach()
endforeach()
maicord_test(MaiCordUnicodeTests ENTRY maicord_unicode_tests_entry
    SOURCES tests/unicode_test.cpp tests/grapheme_test.cpp
    LIBS maicord_core maicord_unicode kernel32
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_unicode)
maicord_test(MaiCordArabicJoiningTests ENTRY maicord_arabic_joining_tests_entry
    SOURCES tests/arabic_joining_test.cpp
    LIBS maicord_core maicord_unicode kernel32
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_arabic_joining)
maicord_test(MaiCordArabicMaskTests ENTRY maicord_arabic_mask_tests_entry
    SOURCES tests/arabic_mask_test.cpp
    LIBS maicord_core maicord_unicode maicord_font kernel32
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_arabic_mask)
maicord_test(MaiCordArabicPlanTests ENTRY maicord_arabic_plan_tests_entry
    SOURCES tests/arabic_plan_test.cpp
    LIBS maicord_core maicord_unicode maicord_font kernel32
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_arabic_plan)
maicord_add_tests(MaiCordArabicPlanTests TIMEOUT 5 LABELS native-text-arabic-run
    TEST maicord_native_arabic_run --run)
maicord_font_test(MaiCordArabicMarksTests maicord_arabic_marks_tests_entry arabic_marks_test
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_arabic_marks)
maicord_test(MaiCordBidiTests ENTRY maicord_bidi_tests_entry
    SOURCES tests/bidi_test.cpp
    LIBS maicord_core maicord_unicode kernel32
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_bidi)
# Real Windows spell checker (read-only); CI may pass -ExcludeTestLabels system_spellcheck.
maicord_test(MaiCordSystemSpellcheckTests ENTRY maicord_system_spellcheck_entry
    SOURCES tests/system_spellcheck.cpp
    LIBS maicord_core maicord_platform kernel32 ole32
    TIMEOUT 10 LABELS system_spellcheck
    TEST maicord_system_spellcheck)
# Multi-click pointer selection: click counting, word/line units, drags.
maicord_test(MaiCordTextPointerSelectionTests ENTRY maicord_text_pointer_selection_entry
    SOURCES tests/text_pointer_selection.cpp
    LIBS maicord_core maicord_platform maicord_model maicord_ui_input maicord_edit_history kernel32 user32
    BUNDLE MaiCordUiUnitTests
    TIMEOUT 10
    TEST maicord_text_pointer_selection)
maicord_test(MaiCordLineBreakTests ENTRY maicord_line_break_tests_entry
    SOURCES tests/line_break_test.cpp
    LIBS maicord_core maicord_unicode kernel32
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_line_breaks)
maicord_test(MaiCordLineTailoredTests ENTRY maicord_line_tailored_entry
    SOURCES tests/line_tailored_test.cpp
    LIBS maicord_core maicord_unicode kernel32
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_line_compat)
maicord_font_test(MaiCordLayoutTableTests maicord_layout_tables_entry layout_tables_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_layout_tables)
maicord_font_test(MaiCordGdefTests maicord_gdef_tests_entry gdef_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_gdef)
maicord_font_test(MaiCordLayoutDirectoryTests maicord_layout_directory_entry layout_directory_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_layout_directory)
maicord_font_test(MaiCordGsubTests maicord_gsub_tests_entry gsub_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_gsub)
maicord_font_test(MaiCordContextTests maicord_context_tests_entry context_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_context)
maicord_font_test(MaiCordGlyphBufferTests maicord_glyph_buffer_entry glyph_buffer_test
    BUNDLE MaiCordTextUnitTests STANDALONE)
maicord_font_test(MaiCordGsubExecuteTests maicord_gsub_execute_entry gsub_execute_test
    BUNDLE MaiCordTextUnitTests STANDALONE)
maicord_font_test(MaiCordClustersTests maicord_clusters_entry clusters_test
    BUNDLE MaiCordTextUnitTests STANDALONE)
maicord_font_test(MaiCordComponentShapingTests maicord_component_shaping_entry component_shaping_test UNICODE
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_component_shaping)
maicord_font_test(MaiCordSinhalaShapeTests maicord_sinhala_shape_entry sinhala_shape_test UNICODE
    BUNDLE MaiCordTextUnitTests STANDALONE
    TIMEOUT 2 LABELS native-text-sinhala-shape
    TEST maicord_native_sinhala_shape)
maicord_font_test(MaiCordKhmerShapeTests maicord_khmer_shape_entry khmer_shape_test UNICODE
    BUNDLE MaiCordTextUnitTests STANDALONE
    TIMEOUT 2 LABELS native-text-khmer-shape
    TEST maicord_native_khmer_shape)
maicord_add_tests(MaiCordKhmerShapeTests TIMEOUT 2 LABELS native-text-khmer-shape ENVIRONMENT
        MAICORD_KHMER_OWNED_TEST=1
    TEST maicord_native_khmer_owned)
maicord_add_tests(MaiCordSinhalaShapeTests TIMEOUT 2 LABELS native-text-sinhala-shape ENVIRONMENT
        MAICORD_SINHALA_OWNED_TEST=1
    TEST maicord_native_sinhala_owned)
maicord_font_test(MaiCordSinhalaInputTests maicord_sinhala_input_entry sinhala_input_test UNICODE
    BUNDLE MaiCordTextUnitTests STANDALONE)
maicord_font_test(MaiCordTibetanInputTests maicord_tibetan_input_entry tibetan_input_test UNICODE
    BUNDLE MaiCordTextUnitTests STANDALONE)
maicord_font_test(MaiCordTibetanShapeTests maicord_tibetan_shape_entry tibetan_shape_test UNICODE
    BUNDLE MaiCordTextUnitTests STANDALONE
    TIMEOUT 2 LABELS native-text-tibetan-shape
    TEST maicord_native_tibetan_shape)
maicord_add_tests(MaiCordTibetanShapeTests TIMEOUT 2 LABELS native-text-tibetan-shape ENVIRONMENT
        MAICORD_TIBETAN_OWNED_TEST=1
    TEST maicord_native_tibetan_owned)
maicord_add_tests(MaiCordTibetanInputTests TIMEOUT 2 LABELS native-text-tibetan-input
    TEST maicord_native_tibetan_input)
maicord_add_tests(MaiCordSinhalaInputTests TIMEOUT 2 LABELS native-text-sinhala-input
    TEST maicord_native_sinhala_input)
maicord_font_test(MaiCordKhmerInputTests maicord_khmer_input_entry khmer_input_test UNICODE
    BUNDLE MaiCordTextUnitTests STANDALONE
    TIMEOUT 2 LABELS native-text-khmer-input
    TEST maicord_native_khmer_input)
maicord_font_test(MaiCordUsePlanTests maicord_use_plan_entry use_plan_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TIMEOUT 2 LABELS native-text-use-plan
    TEST maicord_native_use_plan)
maicord_font_test(MaiCordMyanmarShapeTests maicord_myanmar_shape_entry myanmar_shape_test UNICODE
    BUNDLE MaiCordTextUnitTests STANDALONE
    TIMEOUT 2 LABELS native-text-myanmar-shape
    TEST maicord_native_myanmar_shape)
maicord_add_tests(MaiCordMyanmarShapeTests TIMEOUT 2 LABELS native-text-myanmar-shape ENVIRONMENT
        MAICORD_MYANMAR_OWNED_TEST=1
    TEST maicord_native_myanmar_owned)
maicord_font_test(MaiCordMyanmarInputTests maicord_myanmar_input_entry myanmar_input_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TIMEOUT 2 LABELS native-text-myanmar-input
    TEST maicord_native_myanmar_input)
maicord_add_tests(MaiCordUsePlanTests TIMEOUT 2 LABELS native-text-tibetan-plan
    TEST maicord_native_tibetan_plan --tibetan)
maicord_add_tests(MaiCordUsePlanTests TIMEOUT 2 LABELS native-text-myanmar-plan
    TEST maicord_native_myanmar_plan --myanmar)
maicord_font_test(MaiCordKhmerPlanTests maicord_khmer_plan_entry khmer_plan_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TIMEOUT 2 LABELS native-text-khmer-plan
    TEST maicord_native_khmer_plan)
maicord_font_test(MaiCordShapeStageTests maicord_shape_stage_entry shape_stage_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TIMEOUT 5 LABELS native-text-stages
    TEST maicord_native_shape_stages)
# Keep large QA corpora out of the C++ parser. Reconfigure and rebuild the
# resource if any binary record changes, including changes of equal size.
set(MAICORD_INDIC_INPUT_IDENTITIES "")
foreach(script IN ITEMS bengali gujarati gurmukhi odia telugu kannada malayalam)
    set(fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/generated/${script}_input_fixture.bin")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fixture}")
    file(SHA256 "${fixture}" fixture_sha)
    string(APPEND MAICORD_INDIC_INPUT_IDENTITIES "${script}:${fixture_sha} ")
endforeach()
configure_file(tests/indic_input_fixtures.rc.in "${CMAKE_CURRENT_BINARY_DIR}/indic_input_fixtures.rc" @ONLY)
maicord_test(MaiCordDevanagariPlanTests ENTRY maicord_devanagari_plan_entry
    SOURCES tests/devanagari_plan_test.cpp "${CMAKE_CURRENT_BINARY_DIR}/indic_input_fixtures.rc"
    LIBS maicord_core maicord_font kernel32
    TIMEOUT 5 LABELS native-text-devanagari-plan
    TEST maicord_native_devanagari_plan
    TEST maicord_native_devanagari_profile --profile)
maicord_indic_plan_tests(gurmukhi plan)
maicord_indic_plan_tests(kannada shape)
maicord_indic_plan_tests(malayalam plan profile shape input)
maicord_indic_plan_tests(kannada plan profile input)
maicord_indic_plan_tests(telugu plan profile shape input)
maicord_indic_plan_tests(odia plan profile input shape)
maicord_indic_plan_tests(gurmukhi profile input shape)
maicord_indic_plan_tests(gujarati plan profile input shape)
maicord_indic_plan_tests(bengali plan profile input shape)
maicord_indic_plan_tests(tamil plan profile input owned_profile)
maicord_test(MaiCordFontCatalogTests ENTRY maicord_font_catalog_entry
    SOURCES tests/font_catalog_test.cpp
    LIBS maicord_core maicord_font kernel32 advapi32
    TIMEOUT 2 LABELS native-text-font-catalog
    TEST maicord_native_font_catalog)
maicord_test(MaiCordFontCatalogCoverageTests ENTRY maicord_font_catalog_coverage_entry
    SOURCES tests/font_catalog_coverage_test.cpp
    LIBS maicord_core maicord_font kernel32 advapi32
    TIMEOUT 2 LABELS native-text-font-coverage
    TEST maicord_native_font_catalog_coverage)
maicord_font_test(MaiCordFontFallbackTests maicord_font_fallback_entry font_fallback_test
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_font_fallback_cascade)
maicord_test(MaiCordFontFallbackFamilyTests ENTRY maicord_font_fallback_family_entry
    SOURCES tests/font_fallback_family_test.cpp
    LIBS maicord_core maicord_font kernel32 advapi32
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_font_fallback_families)
maicord_font_test(MaiCordFontFallbackRangeTests maicord_font_fallback_range_entry font_fallback_range_test
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_font_fallback_ranges)
maicord_font_test(MaiCordFontFamilyTests maicord_font_candidates_entry font_candidates_test
    DEFINES MAICORD_FONT_FAMILY=1
    TIMEOUT 10 LABELS native-text-font-family
    TEST maicord_native_font_family)
# The family test also runs basic() and guards(). Keep the direct-candidates
# executable for tools/font_reference/font_candidates_compare.py's manual API
# comparison, but do not compile or run its identical no-argument CTest case.
maicord_font_test(MaiCordFontCandidatesTests maicord_font_candidates_entry font_candidates_test)
maicord_font_test(MaiCordFontMatchTests maicord_font_match_entry font_match_test BUNDLE MaiCordTextUnitTests
    TEST maicord_native_font_match)
maicord_font_test(MaiCordFontNamesTests maicord_font_names_entry font_names_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TIMEOUT 2 LABELS native-text-font-names
    TEST maicord_native_font_names)
maicord_font_test(MaiCordRunPlanTests maicord_run_plan_entry run_plan_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_run_plans)
# <font> <weight> <command> runs one font through a single check. The cost
# commands, lazy-failure (it needs a purpose-built font) and the
# q:/line:/ascii:/metrics/range audits are manual tools.
maicord_add_tests(MaiCordRunPlanTests TIMEOUT 5
    TEST maicord_native_run_plan_lazy_units "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf" 400 lazy-units
    TEST maicord_native_run_plan_profiles "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf" 400 profiles)
maicord_font_test(MaiCordSimulationPlanTests maicord_simulation_plan_entry simulation_plan_test
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_simulation_plans "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Orbitron.ttf")
maicord_font_test(MaiCordLatinRunTests maicord_latin_run_entry latin_run_test BUNDLE MaiCordTextUnitTests
    TEST maicord_native_latin_guards)
maicord_font_test(MaiCordEmojiRunTests maicord_emoji_run_entry emoji_run_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TIMEOUT 5 LABELS native-text-emoji-run
    TEST maicord_native_emoji_runs)
maicord_test(MaiCordEmojiPresentationTests ENTRY maicord_emoji_presentation_entry
    SOURCES tests/emoji_presentation_test.cpp
    LIBS maicord_core maicord_ui kernel32
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_emoji_presentation)
maicord_font_test(MaiCordEmojiLineTests maicord_emoji_line_entry emoji_line_test BUNDLE MaiCordTextUnitTests
    TEST maicord_native_emoji_lines)
maicord_font_test(MaiCordEmojiParagraphTests maicord_emoji_paragraph_entry emoji_paragraph_test
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_emoji_paragraphs)
maicord_font_test(MaiCordScriptTests maicord_script_tests_entry script_test UNICODE
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_scripts)
maicord_test(MaiCordZstdStreamTests ENTRY maicord_zstd_stream_entry
    SOURCES tests/zstd_stream.cpp
    LIBS maicord_core maicord_compression kernel32
    BUNDLE MaiCordCoreUnitTests
    TEST maicord_zstd_stream)
maicord_test(MaiCordEtfTranscodeTests ENTRY maicord_etf_transcode_entry
    SOURCES tests/etf_transcode.cpp src/protocol/etf.cpp
    LIBS maicord_core kernel32
    BUNDLE MaiCordCoreUnitTests
    TEST maicord_etf_transcode)
maicord_test(MaiCordGatewayEtfSessionTests ENTRY maicord_gateway_etf_session_entry
    SOURCES tests/gateway_etf_session.cpp
    LIBS maicord_core maicord_protocol maicord_compression maicord_media_core maicord_media_codec kernel32 crypt32
        bcrypt
    BUNDLE MaiCordCoreUnitTests
    TEST maicord_gateway_etf_session)
maicord_test(MaiCordByteOrderTests ENTRY maicord_byte_order_entry
    SOURCES tests/byte_order_test.cpp
    LIBS maicord_core kernel32
    BUNDLE MaiCordCoreUnitTests
    TEST maicord_byte_order)
maicord_font_test(MaiCordFontSessionTests maicord_font_session_entry font_session_test
    BUNDLE MaiCordTextUnitTests
    TIMEOUT 2
    TEST maicord_native_font_session)
maicord_test(MaiCordNativeFormatTests ENTRY maicord_native_format_entry
    SOURCES tests/native_format_test.cpp src/ui/native_text_candidate.cpp src/ui/native_text_catalog.cpp
        src/ui/emoji_presentation.cpp src/core/log.cpp
    LIBS maicord_core maicord_font maicord_media_core maicord_ui kernel32 advapi32 gdi32
    TEST maicord_native_format_ownership "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Compagnon.otf")
maicord_add_tests(MaiCordNativeFormatTests TIMEOUT 2
    TEST maicord_native_recovery_pressure --recovery-pressure
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Compagnon.otf")
# Inline the shared test driver's hot comparisons and query helpers (/Ob2).
add_library(maicord_native_app_line_driver OBJECT tests/native_app_line_test.cpp)
target_link_libraries(maicord_native_app_line_driver PRIVATE maicord_compile_contract)
set_target_properties(maicord_native_app_line_driver PROPERTIES CXX_EXTENSIONS OFF MAICORD_INLINE_ANY ON)
maicord_test(MaiCordNativeAppLineTests ENTRY maicord_native_app_line_entry
    SOURCES $<TARGET_OBJECTS:maicord_native_app_line_driver> src/render/text_edit_layout.cpp
        tools/font_reference/dwrite_text_system.cpp tools/font_reference/dwrite_edit_reference.cpp
    LIBS maicord_core maicord_ui maicord_text kernel32 dwrite
    TEST maicord_native_app_lines)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 2
    TEST maicord_native_tsf_points --tsf-points)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5
    TEST maicord_native_historic --historic)
add_executable(MaiCordNativeTextStoreTests tests/native_text_store_test.cpp src/render/text_edit_layout.cpp $<TARGET_OBJECTS:maicord_runtime>)
target_link_libraries(MaiCordNativeTextStoreTests PRIVATE maicord_compile_contract maicord_core maicord_ui maicord_text maicord_ui_input kernel32)
target_link_options(MaiCordNativeTextStoreTests PRIVATE /ENTRY:maicord_native_text_store_entry /SUBSYSTEM:CONSOLE,6.02)
maicord_add_tests(MaiCordNativeTextStoreTests TIMEOUT 5 PASS_REGULAR_EXPRESSION "PASS 44 "
    TEST maicord_Native_text_store)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 2 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} LABELS
        native-text-app-lines
    TEST maicord_native_request_recovery --request-recovery)
maicord_add_tests(MaiCordNativeAppLineTests
    TEST maicord_native_fallback_bindings --fallback-bindings
    TEST maicord_native_system_fallback --system-fallback-profile --family "Segoe UI" 400
    TEST maicord_native_hebrew_lines --hebrew-profile --family "Segoe UI" 400
    TEST maicord_native_hangul_lines --hangul-profile --family "Segoe UI" 400)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 15 LABELS native-text-thai-shaping
    TEST maicord_native_thai_shaping --thai-shape-profile --family "Segoe UI" 400
    TEST maicord_native_thai_shaping_private --thai-shape-profile
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf")
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 15 LABELS native-text-thai-wrapping
    TEST maicord_native_thai_wrapping --thai-profile --family "Segoe UI" 400
    TEST maicord_native_thai_wrapping_private --thai-profile
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf")
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 15 LABELS native-text-thai-wrapping native-text-thai-context
    TEST maicord_native_thai_context --thai-context-check)
maicord_add_tests(MaiCordNativeAppLineTests
    TEST maicord_native_font_range_replay --font-replay-check)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 2 LABELS native-text-script-storage
    TEST maicord_native_script_storage --script-storage-check)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 20 LABELS native-text-long
    TEST maicord_native_long_text --long-text-check
    TEST maicord_native_long_editor --long-editor-check)
foreach(sample RANGE 0 5)
    maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 2 LABELS native-text-editor-capacity
        TEST maicord_native_editor_capacity_${sample} --retained-editor-check ${sample})
endforeach()
maicord_add_tests(MaiCordNativeAppLineTests
    TEST maicord_native_standard_scripts --standard-script-profile --family "Segoe UI" 400)
maicord_native_profile_tests(5 tamil-direction "Nirmala UI")
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-tamil-app
    TEST maicord_native_tamil_app --tamil-profile --family "Nirmala UI" 400)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-telugu-cpu
    TEST maicord_native_telugu_cpu --smoke --telugu-bench)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-kannada-cpu
    TEST maicord_native_kannada_cpu --smoke --kannada-bench)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-arabic-cpu
    TEST maicord_native_arabic_cpu --smoke --arabic-bench)
maicord_native_profile_tests(5 telugu-app "Nirmala UI")
maicord_native_profile_tests(5 kannada-app "Nirmala UI")
maicord_native_profile_tests(5 sinhala-app "Nirmala UI")
maicord_native_profile_tests(5 khmer-app "Leelawadee UI")
maicord_native_profile_tests(5 myanmar-app "Myanmar Text")
maicord_native_profile_tests(5 tibetan-app "Microsoft Himalaya")
maicord_native_profile_tests(5 malayalam-app "Nirmala UI")
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-odia-app
    TEST maicord_native_odia_app --odia-profile --family "Nirmala UI" 400)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-odia-cpu
    TEST maicord_native_odia_cpu --smoke --odia-bench)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-odia-app
    TEST maicord_native_odia_app_default --odia-profile --family "Segoe UI" 400
    TEST maicord_native_odia_app_private --odia-profile "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf")
maicord_native_profile_tests(5 gurmukhi-app "Nirmala UI")
maicord_native_profile_tests(5 gujarati-app "Nirmala UI")
maicord_native_profile_tests(5 bengali-app "Nirmala UI")
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-tamil-app
    TEST maicord_native_tamil_app_default --tamil-profile --family "Segoe UI" 400
    TEST maicord_native_tamil_app_private --tamil-profile
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf")
maicord_native_profile_tests(10 devanagari-app "Nirmala UI")
maicord_native_profile_tests(5 devanagari-direction "Nirmala UI")
maicord_native_profile_tests(5 direction-tracking "Nirmala UI")
maicord_native_profile_tests(5 arabic-context "Nirmala UI")
maicord_native_profile_tests(10 devanagari-tracking "Nirmala UI")
# Every app profile above keeps its fallback variants (_default: Segoe UI,
# _private: a private font). Tracking under a fallback font runs only in the
# devanagari, direction, Arabic context and Tamil direction profiles; the other
# scripts' tracking profiles use their own family.
foreach(maicord_tracking_profile IN ITEMS tamil-direction-shape tamil-tracking-shape tamil-tracking telugu-tracking
        kannada-tracking sinhala-tracking malayalam-tracking odia-tracking gurmukhi-tracking gujarati-tracking
        bengali-tracking)
    string(REPLACE "-" "_" maicord_tracking_test "maicord_native_${maicord_tracking_profile}")
    maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-${maicord_tracking_profile}
        TEST ${maicord_tracking_test} --${maicord_tracking_profile}-profile --family "Nirmala UI" 400)
endforeach()
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-khmer-tracking
    TEST maicord_native_khmer_tracking --khmer-tracking-profile --family "Leelawadee UI" 400)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-myanmar-tracking
    TEST maicord_native_myanmar_tracking --myanmar-tracking-profile --family "Myanmar Text" 400)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-tibetan-tracking
    TEST maicord_native_tibetan_tracking --tibetan-tracking-profile --family "Microsoft Himalaya" 400)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 10 LABELS native-text-devanagari-memory
    TEST maicord_native_devanagari_memory --devanagari-memory-check)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-extended-scripts
    TEST maicord_native_extended_scripts --extended-standard-profile --family "Segoe UI" 400
    TEST maicord_native_extended_scripts_private --extended-standard-profile
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf"
    TEST maicord_native_extended_scripts_marker --extended-standard-profile
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/extended_scripts.ttf")
maicord_add_tests(MaiCordNativeAppLineTests
    TEST maicord_native_standard_scripts_private --standard-script-profile
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf"
    TEST maicord_native_hebrew_wrapping --hebrew-wrap-profile --family "Segoe UI" 400
    TEST maicord_native_mixed_bidi --mixed-bidi-wrap-profile --family "Segoe UI" 400
    TEST maicord_native_mixed_bidi_gates --mixed-bidi-gates
    TEST maicord_native_bidi_styles --bidi-styles-profile --family "Segoe UI" 400)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 30
    TEST maicord_native_bidi_emoji --bidi-emoji-profile --family "Segoe UI" 400)
maicord_add_tests(MaiCordNativeAppLineTests
    TEST maicord_native_symbols --symbols-profile --family "Segoe UI" 400
    TEST maicord_native_bidi_style_ranges --bidi-range-check
    TEST maicord_native_simulated_styles --simulated-styles)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-emoji-app
    TEST maicord_native_emoji_dispatch --emoji-dispatch)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 30
    TEST maicord_native_mixed_emoji --mixed-emoji)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-owned-formats
    TEST maicord_native_owned_formats --native-formats)
maicord_add_tests(MaiCordNativeAppLineTests
    TEST maicord_native_tracking --tracking-profile --family "Segoe UI" 400)
# DirectWrite cross-checks that were selectable but never ran: the ASCII break
# oracle, font selection and context layouts, editor policy, the paragraph, hit
# and Latin corpora, fuzzing and the simulated-style profile. The remaining
# modes (benchmarks, dumps, Thai probes, the 1-2 s Arabic, CJK and alphabetic
# corpora, exhaustive variants of the matrix profiles) are manual;
# tests/native_app_line_test.cpp lists them.
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 10
    TEST maicord_native_break_tests --break-tests
    TEST maicord_native_font_selection --font-selection-check
    TEST maicord_native_font_context_layouts --font-context-layouts
    TEST maicord_native_editor_policy --editor-policy-probe
    TEST maicord_native_han_paragraph --han-paragraph
    TEST maicord_native_paragraphs --paragraphs
    TEST maicord_native_paragraph_fuzz --paragraph-fuzz
    TEST maicord_native_latin_paragraph_fuzz --latin-paragraph-fuzz
    TEST maicord_native_hit_profile --hit-profile --family "Segoe UI" 400
    TEST maicord_native_latin_paragraph --latin-paragraph --family "Segoe UI" 400
    TEST maicord_native_latin_hits --latin-hits --family "Segoe UI" 400
    TEST maicord_native_paragraph_profile --paragraph-profile --family "Segoe UI" 400
    TEST maicord_native_common_punctuation --common-punctuation-profile --family "Segoe UI" 400
    TEST maicord_native_directional_large --directional-large-profile --family "Segoe UI" 400
    TEST maicord_native_simulated_profile --simulated-profile --family "Segoe UI" 400)
maicord_test(MaiCordNativeSpacingTests ENTRY maicord_native_spacing_entry
    SOURCES tests/native_spacing_test.cpp
    LIBS maicord_core maicord_font kernel32 dwrite
    TIMEOUT 5 LABELS native-text-spacing
    TEST maicord_native_spacing)
maicord_add_tests(MaiCordNativeAppLineTests
    TEST maicord_native_paragraph_options --paragraph-options
    TEST maicord_native_paragraph_hits --paragraph-hits
    TEST maicord_native_edit_hits --edit-hits
    TEST maicord_native_directional_edit --directional-edit
    TEST maicord_native_directional_edit_cuts --directional-edit-cuts
    TEST maicord_native_latin_lines --latin-profile --family "Segoe UI" 400
    TEST maicord_native_alphabetic --alphabetic-profile --family "Segoe UI" 400)
maicord_add_tests(MaiCordNativeAppLineTests ENVIRONMENT MAICORD_CJK_SAMPLE=14
    TEST maicord_native_cjk_supplementary --cjk-paragraph-profile --family "Segoe UI" 400)
foreach(mode paragraph editor)
    maicord_add_tests(MaiCordNativeAppLineTests ENVIRONMENT MAICORD_CJK_SAMPLE=16
        TEST maicord_native_light_fallback_${mode} --cjk-${mode}-profile --family "Microsoft JhengHei UI" 400)
endforeach()
maicord_add_tests(MaiCordNativeAppLineTests
    TEST maicord_native_height_overflow --height-overflow-profile --family "Segoe UI" 400
    TEST maicord_native_ellipsis_sign --ellipsis-sign-profile --family "Segoe UI" 400
    TEST maicord_native_ellipsis_cut --ellipsis-cut-profile --family "Segoe UI" 400
    TEST maicord_native_ellipsis_line --ellipsis-line-profile --family "Segoe UI" 400
    TEST maicord_native_ellipsis_styles --ellipsis-style-profile --family "Segoe UI" 400)
# Each process runs one complete profile. The five-profile union keeps
# all 139,944 matrix cases; --matrix-batch remains an offline full union.
set(maicord_matrix_profile_index 0)
foreach(maicord_matrix_profile IN ITEMS ellipsis_hits multiline_hits
        multiline_private_hits styled decorations)
    maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 20 COST 40 ENVIRONMENT LOCALAPPDATA=
        LABELS native-text-hits native-text-styled native-text-decorations native-text-quick
        TEST maicord_native_matrix_${maicord_matrix_profile} --matrix-profile ${maicord_matrix_profile_index}
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf")
    math(EXPR maicord_matrix_profile_index "${maicord_matrix_profile_index} + 1")
endforeach()
maicord_add_tests(MaiCordNativeAppLineTests
    TEST maicord_native_decoration_options --decoration-options
    TEST maicord_native_semantic_ranges --semantic-profile --family "Segoe UI" 400
    TEST maicord_native_directional_semantic_ranges --directional-semantic-profile --family "Segoe UI" 400
    TEST maicord_native_directional_editor_hits --directional-editor-profile --family "Segoe UI" 400
    TEST maicord_native_directional_editor_cuts --directional-editor-cuts-profile --family "Segoe UI" 400
    TEST maicord_native_semantic_options --semantic-options
    TEST maicord_native_styled_hits --styled-hit-profile --family "Segoe UI" 400
    TEST maicord_native_styled_editor --styled-edit)
maicord_add_tests(MaiCordNativeAppLineTests
    TEST maicord_native_style_clusters --style-cluster-profile --family "Segoe UI" 400
    TEST maicord_native_style_continuity --continuity-profile --family "Segoe UI" 400
    TEST maicord_native_style_cluster_parity --style-clusters
    TEST maicord_native_style_continuity_pixelify --continuity-profile
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Pixelify.ttf" 400)
maicord_font_test(MaiCordCursiveDeferredTests maicord_cursive_deferred_entry cursive_deferred_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_cursive_deferred)
maicord_font_test(MaiCordLookupPlanTests maicord_lookup_plan_entry lookup_plan_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_lookup_plan)
maicord_font_test(MaiCordNominalParticipationTests maicord_nominal_participation_entry nominal_participation_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_nominal_participation)
maicord_font_test(MaiCordNominalSourcesTests maicord_nominal_sources_entry nominal_sources_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_nominal_sources)
maicord_font_test(MaiCordNominalVariationsTests maicord_nominal_variations_entry nominal_variations_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_nominal_variations)
maicord_font_test(MaiCordMetricVariationsTests maicord_metric_variations_entry metric_variations_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_metric_variations)
maicord_font_test(MaiCordGvarOutlineTests maicord_gvar_outline_entry gvar_outline_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_gvar_outlines)
maicord_font_test(MaiCordTtInterpreterTests maicord_tt_interpreter_entry tt_interpreter_test
    BUNDLE MaiCordTextUnitTests STANDALONE)
maicord_font_test(MaiCordTtOrientationTests maicord_tt_orientation_entry tt_orientation_test
    BUNDLE MaiCordTextUnitTests
    TEST maicord_native_tt_orientation)
maicord_add_tests(MaiCordTtInterpreterTests TEST maicord_native_tt_interpreter)
maicord_font_test(MaiCordTtOutlineTests maicord_tt_outline_entry tt_outline_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_tt_bold bold
    TEST maicord_native_tt_bold_oblique bold-oblique
    TEST maicord_native_tt_outlines normal
    TEST maicord_native_tt_oblique oblique
    TEST maicord_native_tt_positioning position)
maicord_font_test(MaiCordFontInstanceTests maicord_font_store_entry font_store_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TIMEOUT 5 LABELS native-text-font-instance
    TEST maicord_native_font_instances)
maicord_font_test(MaiCordGposTests maicord_gpos_tests_entry gpos_test
    BUNDLE MaiCordTextUnitTests STANDALONE)
# Basic, mark, cursive and ligature placement corpora in one run.
maicord_font_test(MaiCordGposExecuteTests maicord_gpos_execute_tests_entry gpos_execute_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TIMEOUT 5 LABELS native-text-gpos-marks
    TEST maicord_native_gpos_execute)
maicord_font_test(MaiCordGposAttachmentTests maicord_gpos_attachment_tests_entry gpos_attachment_test
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_gpos_attachments)
maicord_add_tests(MaiCordGposTests TEST maicord_native_gpos)
maicord_add_tests(MaiCordClustersTests TEST maicord_native_clusters)
maicord_add_tests(MaiCordGsubExecuteTests TEST maicord_native_gsub_execute)
maicord_add_tests(MaiCordGlyphBufferTests TEST maicord_native_glyph_buffer)
maicord_test(MaiCordFontTests ENTRY maicord_font_tests_entry
    SOURCES tests/sfnt_test.cpp tests/font_simulation_test.cpp tests/tt_bold_points_test.cpp
        tests/truetype_outline_test.cpp tests/cff_outline_test.cpp tests/color_font_test.cpp
        tests/variation_store_test.cpp tests/font_axes_test.cpp
    LIBS maicord_core maicord_font kernel32
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_native_font)
maicord_test(MaiCordColrProgramTests ENTRY maicord_colr_program_tests_entry
    SOURCES tests/colr_program_test.cpp tests/colr_source_test.cpp
    LIBS maicord_core maicord_font maicord_paint_program kernel32
    BUNDLE MaiCordTextUnitTests STANDALONE
    TEST maicord_colr_program)
maicord_test(MaiCordYuvTests ENTRY maicord_yuv_tests_entry
    SOURCES tests/yuv_convert_test.cpp
    LIBS maicord_core maicord_media_codec kernel32
    BUNDLE MaiCordCodecUnitTests
    TEST maicord_yuv_conversion)
maicord_shader(maicord_vp8_reference_reconstruct_header vp8_reference_reconstruct.hpp cs_5_0 vp8_reconstruct
        g_vp8_reference_reconstruct tests/fixtures/vp8_reconstruct_reference.hlsl)
maicord_shader(maicord_vp8_reference_idct_header vp8_reference_idct.hpp cs_5_0 vp8_inverse_dct
        g_vp8_reference_idct tests/fixtures/vp8_reconstruct_reference.hlsl)
maicord_shader(maicord_vp8_reference_filter_header vp8_reference_filter.hpp cs_5_0 vp8_filter
        g_vp8_reference_filter tests/fixtures/vp8_filter_reference.hlsl)
maicord_shader(maicord_video_frame_reference_header video_frame_reference.hpp cs_5_0 cs_main
        g_video_frame_reference tests/fixtures/video_frame_reference.hlsl)
# A third-party style mc.fx@1 effect the background probe draws as a plugin.
set(MAICORD_FX_PLUGIN_REFERENCE_HEADER "${MAICORD_GENERATED_DIRECTORY}/maicord_fx_plugin_reference_ps.hpp")
add_custom_command(OUTPUT "${MAICORD_FX_PLUGIN_REFERENCE_HEADER}"
    COMMAND "${MAICORD_FXC_EXECUTABLE}" /nologo /T ps_5_0 /E main /O3 /Ges /WX
        /Qstrip_debug /Qstrip_reflect /Qstrip_priv
        /Fh "${MAICORD_FX_PLUGIN_REFERENCE_HEADER}" /Vn g_maicord_fx_plugin_reference_shader
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/background_fx_plugin_reference.hlsl"
    DEPENDS tests/shaders/background_fx_plugin_reference.hlsl VERBATIM)
maicord_test(MaiCordUiProbe WIN32 ENTRY maicord_ui_probe_entry
    SOURCES "${MAICORD_FX_PLUGIN_REFERENCE_HEADER}"
        config/maicord.manifest tests/integration/ui_probe.cpp tools/font_reference/dwrite_text_system.cpp
        tests/integration/poll_scene_ui_probe.cpp tests/integration/profile_editor_ui_probe.cpp
        tests/integration/navigation_gaps_ui_probe.cpp tests/integration/reaction_intent_ui_adapter.cpp
        tests/integration/thread_membership_ui_adapter.cpp tests/integration/reply_draft_ui_adapter.cpp
        tests/integration/thread_browser_ui_adapter.cpp src/app/application_thread_browser.cpp
        tests/integration/thread_creation_ui_adapter.cpp src/app/application_thread_creation.cpp
        tests/integration/forum_post_ui_adapter.cpp src/platform/clipboard_attachment.cpp
        src/app/application_forum_post.cpp src/app/application_forum_form.cpp
        src/app/application_forum_attachments.cpp src/app/application_upload_lane.cpp
        src/app/application_forum_preview.cpp src/app/application_composer_preview.cpp
        src/app/application_local_preview.cpp src/app/application_attachment_allowance.cpp
        src/render/forum_post_surface.cpp src/app/application_reply_draft.cpp
        src/app/application_message_edit.cpp src/app/application_message_delete.cpp
        src/app/application_thread_actions.cpp src/app/application_thread_header_activation.cpp
        src/app/application_thread_header.cpp src/app/application_message_reaction.cpp
        src/app/application_message_mutation_notice.cpp tests/integration/media_picker_ui_adapter.cpp
        tests/stream_preview_media_stub.cpp tests/integration/channel_overview_ui_adapter.cpp
        tests/integration/regional_flag_probe.cpp tests/integration/regional_flag_surfaces_probe.cpp
        tests/integration/inline_flags_probe.cpp tests/integration/guild_rail_icon_probe.cpp
        tests/integration/background_theme_probe.cpp
        tests/integration/plugin_ui_probe.cpp tests/integration/plugin_navigation_ui_probe.cpp
        tests/integration/plugin_catalog_ui_probe.cpp src/app/application_plugin_browser_projection.cpp
        tests/integration/settings_choice_ui_probe.cpp tests/integration/user_settings_ui_probe.cpp
        src/app/settings_catalog.cpp src/app/application_updates.cpp
        tests/integration/notification_settings_ui_probe.cpp tests/integration/shortcuts_ui_probe.cpp
        tests/integration/call_scene_ui_probe.cpp tests/integration/inbox_ui_probe.cpp
        tests/integration/caption_ui_probe.cpp src/app/app_icon.cpp config/maicord.rc
        tests/integration/showcase_probe.cpp
        tests/integration/login_recovery_ui_probe.cpp tests/login_reset_ui_probe.cpp
        tests/integration/login_phone_ui_probe.cpp tests/integration/login_browser_ui_probe.cpp
        tests/integration/member_paging_probe.cpp tests/integration/thread_members_probe.cpp
        tests/integration/read_state_probe.cpp tests/integration/text_form_probe.cpp
        tests/integration/application_text_form_probe.cpp src/app/application_button_input.cpp
        src/app/application_text_form.cpp src/app/application_composer_text_input.cpp
        src/app/application_state.cpp tests/integration/video_retention_probe.cpp
        tests/integration/video_scene_ui_probe.cpp tests/integration/audio_scene_ui_probe.cpp
        tests/integration/tooltip_ui_probe.cpp tests/integration/account_menu_ui_probe.cpp
        tests/integration/toast_ui_probe.cpp
        tests/integration/search_ui_probe.cpp tests/integration/application_media_animation_ui_probe.cpp
        src/app/application_media.cpp src/app/application_media_animation.cpp
        src/app/application_media_requests.cpp tests/integration/menu_row_ui_probe.cpp
        tests/integration/animation_preview_probe.cpp tests/integration/png_preview_probe.cpp
        tests/integration/gif_scroll_probe.cpp tests/integration/media_memory_probe.cpp
        tests/integration/vp8_tiled_probe.cpp
        "${maicord_vp8_reference_reconstruct_header}" "${maicord_vp8_reference_idct_header}"
        "${maicord_vp8_reference_filter_header}" tests/integration/video_frame_reference_probe.cpp
        "${maicord_video_frame_reference_header}"
        tests/media_fixture.cpp src/core/log.cpp
    LIBS maicord_core maicord_text maicord_ui maicord_ui_input maicord_model maicord_plugin_host
        maicord_plugin_modules maicord_plugin_manager maicord_message_runtime maicord_render_core maicord_media_codec
        maicord_atlas_gpu
        maicord_renderer maicord_platform maicord_protocol maicord_app_core maicord_message_media
        maicord_account_media kernel32 user32 ws2_32 secur32 bcrypt crypt32 d3d11 dxgi dcomp dwrite dwmapi
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}" src/app
    DEFINES MAICORD_CLIPBOARD_UI_FIXTURE=1 MAICORD_LOG_STDOUT=1
    PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
# The constinit probe state (renderer plus every scene owner) exceeds the
# default constant-evaluation step budget.
set_source_files_properties(tests/integration/ui_probe.cpp tests/integration/showcase_probe.cpp
    PROPERTIES COMPILE_OPTIONS "/constexpr:steps8388608")
# The caption probe decodes the client's own icon resource.
add_dependencies(MaiCordUiProbe maicord_app_icon)
# The single-scene WIN32 probes link the renderer stack and run with GPU metrics.
set(maicord_scene_probe_libs maicord_core maicord_text maicord_ui maicord_ui_input maicord_model
    maicord_message_runtime maicord_render_core maicord_media_codec maicord_atlas_gpu maicord_renderer
    maicord_platform maicord_protocol maicord_app_core maicord_message_media maicord_account_media)
set(maicord_scene_probe_system_libs kernel32 user32 ws2_32 secur32 bcrypt crypt32 d3d11 dxgi dcomp shell32)
maicord_test(MaiCordMessageLinkUiProbe WIN32 ENTRY maicord_message_link_ui_probe_entry
    SOURCES config/maicord.manifest tests/integration/message_link_ui_probe.cpp
        src/app/application_content_input.cpp src/app/application_search_navigation.cpp
        src/app/application_message_mutation_notice.cpp src/app/application_thread_actions.cpp src/core/log.cpp
        src/app/application_channel_memory.cpp
    LIBS ${maicord_scene_probe_libs} ${maicord_scene_probe_system_libs}
    INCLUDES src/app
    TIMEOUT 30 RESOURCE_LOCK native_gpu ENV MAICORD_UI_MESSAGE_LINK_PROBE=1 MAICORD_GPU_METRICS=1
    TEST maicord_message_link_ui_probe)
maicord_test(MaiCordComposerUiProbe WIN32 ENTRY maicord_composer_ui_probe_entry
    SOURCES config/maicord.manifest tests/integration/composer_ui_probe.cpp src/app/application_composer.cpp
        src/app/application_composer_input.cpp src/app/application_reply_draft.cpp
        src/app/application_composer_edit_menu.cpp src/app/application_message_send.cpp src/core/log.cpp
    LIBS ${maicord_scene_probe_libs} ${maicord_scene_probe_system_libs}
    INCLUDES src/app
    TIMEOUT 30 RESOURCE_LOCK native_gpu ENV MAICORD_UI_COMPOSER_PROBE=1 MAICORD_GPU_METRICS=1
    TEST maicord_composer_ui_probe)
maicord_test(MaiCordAccountLogoutUiProbe WIN32 ENTRY maicord_account_logout_ui_probe_entry
    SOURCES config/maicord.manifest tests/integration/account_logout_ui_probe.cpp
        src/app/application_text_form.cpp src/app/application_login_ui.cpp src/app/application_login_input.cpp
        src/app/application_account_menu.cpp src/app/application_shell_input.cpp src/core/log.cpp
    LIBS ${maicord_scene_probe_libs} maicord_account_host maicord_account_paths ${maicord_scene_probe_system_libs}
    INCLUDES src/app
    TIMEOUT 30 RESOURCE_LOCK native_gpu ENV MAICORD_UI_ACCOUNT_LOGOUT_PROBE=1 MAICORD_GPU_METRICS=1
    TEST maicord_account_logout_ui_probe)
maicord_test(MaiCordScopedReadUiProbe WIN32 ENTRY maicord_scoped_read_ui_probe_entry
    SOURCES config/maicord.manifest tests/integration/scoped_read_ui_probe.cpp
        src/app/application_scoped_read.cpp src/app/application_read_state.cpp
        src/app/application_scoped_read_menu.cpp src/app/application_message_mutation_notice.cpp
        src/app/application_guild_menu.cpp tests/integration/scoped_read_menu_scroll.cpp
        src/app/application_scroll_host.cpp src/core/log.cpp
    LIBS ${maicord_scene_probe_libs} maicord_account_host maicord_account_paths ${maicord_scene_probe_system_libs}
    INCLUDES src/app
    OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/scoped_read_clock_fixture.hpp"
    TIMEOUT 30 RESOURCE_LOCK native_gpu ENV MAICORD_UI_SCOPED_READ_PROBE=1 MAICORD_GPU_METRICS=1
    TEST maicord_scoped_read_ui_probe)
maicord_ui_probe_test(maicord_ui_probe UI_PROBE_ONCE LABELS gpu RESOURCE_LOCK native_gpu)
# Hosted CI renders in software; this probe needs a hardware GPU to finish in time.
# Background canvases, Stars/Comets and plugin effects (mc.fx) against CPU references.
maicord_ui_probe_test(maicord_background_theme_probe UI_BACKGROUND_THEME_PROBE BOOTSTRAP
    TIMEOUT 20 LABELS gpu display RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_inline_media_controls_ui_probe VIDEO_CONTROLS_UI_PROBE BOOTSTRAP
    TIMEOUT 20 COST 50 RESOURCE_LOCK native_gpu)
# Rail server icons through layout and present: static, animated at rest and
# hovered, and still-loading icons must each build, queue the right resident
# rendition and present (a failed build_layout presents nothing).
maicord_ui_probe_test(maicord_guild_rail_icon_probe UI_GUILD_RAIL_ICON_PROBE BOOTSTRAP
    TIMEOUT 20 LABELS native-ui display RESOURCE_LOCK native_gpu)
# Profiles settings page, preview card, cropper and decoration picker.
maicord_ui_probe_test(maicord_profile_editor_ui_probe PROFILE_EDITOR_UI_PROBE BOOTSTRAP
    TIMEOUT 20 LABELS native-ui display RESOURCE_LOCK native_gpu)
# Rich presence cards in the profile popout and full profile.
maicord_ui_probe_test(maicord_profile_activity_ui_probe PROFILE_ACTIVITY_UI_PROBE BOOTSTRAP
    TIMEOUT 20 LABELS native-ui display RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_shortcuts_ui_probe UI_SHORTCUTS_PROBE BOOTSTRAP
    TIMEOUT 20 LABELS native-ui display RESOURCE_LOCK native_gpu)
# Growable media atlases, frame caches and a 1080p DXVA stream: pixels
# across growth/trim and DXGI memory (MAICORD_MEDIA_MEMORY_REPORT=<file>).
# DXGI usage is per process, so this probe needs no GPU resource lock.
maicord_ui_probe_test(maicord_media_memory_probe MEDIA_MEMORY_PROBE BOOTSTRAP TIMEOUT 20 LABELS gpu)
# Call view grid/focus/fullscreen with synthetic NV12 camera pictures.
maicord_ui_probe_test(maicord_call_ui_probe UI_CALL_PROBE BOOTSTRAP
    TIMEOUT 20 LABELS native-ui display RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_post_resize_pacing_probe UI_POST_RESIZE_PACING_PROBE BOOTSTRAP
    TIMEOUT 30 LABELS native-ui display RESOURCE_LOCK native_gpu)
# Exit 77: the display is asleep and DWM composes at ~4 Hz, which no paced
# frame can meet; CTest reports a skip instead of a pacing failure.
set_tests_properties(maicord_post_resize_pacing_probe PROPERTIES SKIP_RETURN_CODE 77)
maicord_ui_probe_test(maicord_unread_boundary_probe UI_UNREAD_BOUNDARY_PROBE BOOTSTRAP
    TIMEOUT 30 LABELS native-ui read-state display RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_rich_search_probe UI_RICH_SEARCH_PROBE BOOTSTRAP
    TIMEOUT 30 LABELS native-ui search RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_reaction_intent_probe UI_REACTION_INTENT_PROBE BOOTSTRAP
    TIMEOUT 30 LABELS native-ui reactions RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_thread_membership_probe UI_THREAD_MEMBERSHIP_PROBE BOOTSTRAP
    TIMEOUT 30 LABELS native-ui threads RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_thread_browser_probe UI_THREAD_BROWSER_PROBE BOOTSTRAP
    TIMEOUT 30 LABELS native-ui threads RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_reply_draft_probe UI_REPLY_DRAFT_PROBE BOOTSTRAP
    TIMEOUT 30 LABELS native-ui reply-drafts RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_forum_media_probe UI_FORUM_MEDIA_PROBE BOOTSTRAP
    TIMEOUT 30 LABELS native-ui forum-media RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_hangul_render_probe HANGUL_RENDER_PROBE
    TIMEOUT 15 LABELS native-text-render RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_standard_render_probe STANDARD_RENDER_PROBE
    TIMEOUT 15 LABELS native-text-render RESOURCE_LOCK native_gpu)
# Wrapped profile geometry for each script's samples. The same gates write
# frames that tools/verify_hangul_render.py and tools/verify_tamil_render.py
# check pixel by pixel when MAICORD_UI_PROBE_CAPTURE_DIR is set.
foreach(maicord_render_script IN ITEMS TIBETAN MYANMAR KHMER SINHALA MALAYALAM KANNADA TELUGU ODIA GURMUKHI
        GUJARATI BENGALI TAMIL THAI)
    string(TOLOWER "maicord_${maicord_render_script}_wrap_render_probe" maicord_render_test)
    maicord_ui_probe_test(${maicord_render_test} ${maicord_render_script}_WRAP_RENDER_PROBE
        TIMEOUT 15 LABELS native-text-render)
endforeach()
foreach(maicord_render_mode IN ITEMS DEVANAGARI DEVANAGARI_TRACKING DEVANAGARI_DIRECTION DIRECTION_TRACKING
        EXTENDED_STANDARD)
    string(TOLOWER "maicord_${maicord_render_mode}_render_probe" maicord_render_test)
    maicord_ui_probe_test(${maicord_render_test} ${maicord_render_mode}_RENDER_PROBE TIMEOUT 15 LABELS native-text-render)
endforeach()
# Isolated scene gates of the UI probe: each builds the shared fixture, drives
# one scene through the real layout, text and draw path, and exits. They need
# no GPU lock; every one is a hardware-renderer pixel and geometry check.
# Unified picker also checks cached centering and dense message batch capacity.
# MAICORD_<GATE>_PROBE runs as maicord_<gate>_probe, a leading UI_ moved to the end.
foreach(maicord_ui_gate IN ITEMS ACCOUNT_MENU_UI ANIMATION_HANDOFF_UI BOOTSTRAP_TITLE CATALOG_APNG_UI INBOX_UI
        LONG_EMBED_UI MEDIA_FAVORITES_UI MEDIA_PICKER_UI MENU_ROW_UI NOTIFICATION_SETTINGS_UI PLUGIN_CATALOG_UI
        PROFILE_BRAILLE_UI SCOPED_SEARCH_UI SEARCH_UI TOOLTIP_UI TRANSPARENT_GUILD_UI USER_SETTINGS_UI UI_CHANNEL_OVERVIEW
        UI_GUILD_ROLES
        UI_CAPTION UI_COMPOSER_WORKFLOW UI_DOWNLOAD UI_FORUM_POST UI_FORWARDED_MESSAGES UI_HOVER_RETENTION
        UI_INLINE_FLAGS
        UI_MEDIA_UNDERLAY UI_MESSAGE_FORWARD UI_PANE_GLYPH_STRESS UI_PINNED_MESSAGES UI_QUICK_SWITCHER
        UI_SEARCH_POLISH UI_SURFACE_MOTION UI_THREAD_CREATION UI_UNIFIED_PICKER TOAST_UI)
    string(TOLOWER "${maicord_ui_gate}" maicord_ui_gate_name)
    string(REGEX REPLACE "^ui_(.*)$" "\\1_ui" maicord_ui_gate_name "${maicord_ui_gate_name}")
    maicord_ui_probe_test(maicord_${maicord_ui_gate_name}_probe ${maicord_ui_gate}_PROBE BOOTSTRAP
        TIMEOUT 15 LABELS native-ui display)
endforeach()
# Website screenshots (tests/integration/showcase_probe.cpp). Opt-in: build.ps1
# runs label showcase only when the test is named, e.g.
# -TestNames maicord_showcase_ui_probe -ProbeCaptureDirectory <dir>.
maicord_ui_probe_test(maicord_showcase_ui_probe UI_SHOWCASE_PROBE BOOTSTRAP
    TIMEOUT 120 LABELS showcase display RESOURCE_LOCK native_gpu)
# COLR compute bindings: layered, narrow and wide kernels over hand-built
# programs, upload growth, owner changes and failed-submission unbinding.
maicord_ui_probe_test(maicord_colr_binding_probe COLR_BINDING_PROBE TIMEOUT 15 LABELS gpu)
# Live-resize stress in a shown window: 64 WM_SIZE steps down and back, each
# painted once as a clipped update with no swap-chain reallocation.
maicord_ui_probe_test(maicord_resize_stress_probe UI_PROBE_RESIZE_STRESS TIMEOUT 15 LABELS gpu display)
# The two regional flag probes read the pinned external flag corpus, so they run
# only when MAICORD_REGIONAL_FLAG_REFERENCE_DIRECTORY names it.
if(MAICORD_REGIONAL_FLAG_REFERENCE_DIRECTORY)
    maicord_add_tests(MaiCordUiProbe TIMEOUT 20 LABELS native-ui display ENV
            MAICORD_REGIONAL_FLAG_REFERENCE_DIRECTORY=${MAICORD_REGIONAL_FLAG_REFERENCE_DIRECTORY}
            MAICORD_GPU_METRICS=1
        TEST maicord_regional_flag_reference_probe)
    maicord_add_tests(MaiCordUiProbe TIMEOUT 20 LABELS native-ui display ENV
            MAICORD_REGIONAL_FLAG_SURFACES_PROBE=1
            MAICORD_REGIONAL_FLAG_REFERENCE_DIRECTORY=${MAICORD_REGIONAL_FLAG_REFERENCE_DIRECTORY}
            MAICORD_GPU_METRICS=1
        TEST maicord_regional_flag_surfaces_probe)
endif()
if(MAICORD_TEST_AUDIO_PLAYBACK)
    maicord_add_tests(MaiCordVoiceTests TIMEOUT 15 RESOURCE_LOCK native_audio ENV MAICORD_VOICE_AUDIO_OUTPUT=1
        TEST maicord_voice_audio_output)
    maicord_add_tests(MaiCordVoiceTests TIMEOUT 15 RESOURCE_LOCK native_audio ENV MAICORD_VOICE_AUDIO_CAPTURE=1
        TEST maicord_voice_audio_capture)
endif()
# Fast CPU decode paths (PNG filters, GIF LZW, JPEG entropy) against bit-serial references.
maicord_test(MaiCordMediaDecodeReferenceTests ENTRY maicord_media_decode_reference_tests_entry
    SOURCES tests/media_decode_reference.cpp
    LIBS maicord_media_codec maicord_core kernel32
    TIMEOUT 10
    TEST maicord_media_decode_reference)
# Production GPU image stages: all PNG sample layouts against integer references,
# JPEG coefficients against Pillow, and VP8 against the CPU thumbnail decoder.
maicord_test(MaiCordMediaGpuReferenceTests ENTRY maicord_media_gpu_reference_tests_entry
    SOURCES tests/media_gpu_reference.cpp
    LIBS maicord_atlas_gpu maicord_render_core maicord_media_codec maicord_core d3d11 kernel32
    BUNDLE MaiCordGpuUnitTests
    TIMEOUT 10 LABELS gpu RESOURCE_LOCK native_gpu
    TEST maicord_media_gpu_reference)
maicord_test(MaiCordOpusEncoderTests ENTRY maicord_opus_encoder_tests_entry
    SOURCES tests/opus_encoder.cpp
    LIBS maicord_voice_encoder maicord_media_codec maicord_opus_encoder maicord_core kernel32
    INCLUDES tests third_party/opus/include
    TIMEOUT 10
    TEST maicord_opus_encoder)
# Decoder coverage streams (tests/fixtures/opus_decoder_vectors.inc), range- and
# bit-exact; the official test vectors run opt-in (tools/opus_conformance.ps1).
maicord_test(MaiCordOpusDecoderVectorTests ENTRY maicord_opus_decoder_vector_tests_entry
    SOURCES tests/opus_decoder_vectors.cpp
    LIBS maicord_opus_decoder maicord_core kernel32
    INCLUDES tests third_party/opus/include
    TIMEOUT 10
    TEST maicord_opus_decoder_vectors)
# Voice messages: the Ogg Opus writer, recorder, waveform, upload/message
# bodies, gating, the composer flow and the module with a synthetic microphone.
maicord_test(MaiCordVoiceMessageTests ENTRY maicord_voice_message_tests_entry
    SOURCES tests/voice_message.cpp
    LIBS maicord_app_core maicord_message_runtime maicord_model maicord_protocol maicord_platform
        maicord_media_codec maicord_voice_encoder maicord_opus_encoder maicord_opus_decoder maicord_core
        kernel32 ole32 user32
    INCLUDES tests
    TIMEOUT 10
    TEST maicord_voice_message)
# Discord-sized store limits: member role overflow, member roles in messages
# and owned voice-state storage.
maicord_test(MaiCordStoreLimitsTests ENTRY maicord_store_limits_entry
    SOURCES tests/store_limits.cpp
    LIBS maicord_core maicord_protocol maicord_model kernel32
    BUNDLE MaiCordModelTests
    TIMEOUT 10
    TEST maicord_store_limits)

maicord_test(MaiCordIconAtlasCopyTests ENTRY maicord_icon_atlas_copy_entry
    SOURCES tests/icon_atlas_copy.cpp
    LIBS maicord_renderer maicord_atlas_gpu maicord_ui maicord_core d3d11 kernel32
    BUNDLE MaiCordGpuUnitTests
    TIMEOUT 5 TEST maicord_icon_atlas_copy)

maicord_test(MaiCordRectangleGeometryTests ENTRY rectangle_geometry_entry
    SOURCES tests/rectangle_geometry.cpp
    LIBS maicord_ui maicord_core kernel32
    BUNDLE MaiCordUiUnitTests
    TIMEOUT 5 TEST maicord_rectangle_geometry)

maicord_test(MaiCordEmbedProviderTests ENTRY maicord_embed_provider_entry
    SOURCES tests/embed_provider.cpp
    LIBS maicord_protocol maicord_core kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 5 TEST maicord_embed_provider)

maicord_test(MaiCordYoutubePlayerTests ENTRY maicord_youtube_player_entry
    SOURCES tests/youtube_player.cpp
    LIBS maicord_protocol maicord_core kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 5 TEST maicord_youtube_player)

# MAICORD_SOUNDCLOUD_LIVE_PROBE=5 checks collection track switches and stale commands.
# MAICORD_YTM_LIVE_PROBE=1 exercises the reported music streams; adding
# MAICORD_AAC_SEEK_PROBE=1 compares sought PCM against continuous native decode.
maicord_test(MaiCordEmbedResolverTests ENTRY maicord_youtube_resolver_entry
    # Public SoundCloud queues; opt-in native YouTube HD and adaptive AAC seek comparison.
    SOURCES tests/youtube_resolver.cpp tests/twitch_live_fetch.cpp tests/soundcloud_player.cpp
        tests/soundcloud_collection.cpp tests/youtube_collection.cpp tests/apple_music_album.cpp
        tests/apple_music_playlist.cpp tests/native_player_perf.cpp src/app/update_https.cpp
    LIBS maicord_account_media maicord_app_core maicord_protocol maicord_platform maicord_core
        d3d11 dxgi ole32 user32 kernel32 ws2_32 secur32 bcrypt
    TIMEOUT 20 TEST maicord_youtube_resolver)

maicord_test(MaiCordTikTokPlayerTests ENTRY maicord_tiktok_player_entry
    SOURCES tests/tiktok_player.cpp
    LIBS maicord_protocol maicord_core kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 5 TEST maicord_tiktok_player)

maicord_test(MaiCordTwitchPlayerTests ENTRY maicord_twitch_player_entry
    SOURCES tests/twitch_player.cpp
    LIBS maicord_protocol maicord_core kernel32
    TIMEOUT 5 TEST maicord_twitch_player)

maicord_test(MaiCordSpotifyPlayerTests ENTRY maicord_spotify_player_entry
    SOURCES tests/spotify_player.cpp
    LIBS maicord_protocol maicord_media_core maicord_core kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 5 TEST maicord_spotify_player)

maicord_test(MaiCordStreamablePlayerTests ENTRY maicord_streamable_player_entry
    SOURCES tests/streamable_player.cpp
    LIBS maicord_protocol maicord_core kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 5 TEST maicord_streamable_player)

maicord_test(MaiCordPornhubPlayerTests ENTRY maicord_pornhub_player_entry
    SOURCES tests/pornhub_player.cpp
    LIBS maicord_protocol maicord_core kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 5 TEST maicord_pornhub_player)

# Vimeo canonical/share routes and native timestamp parsing.
maicord_test(MaiCordVimeoPlayerTests ENTRY maicord_vimeo_player_entry
    SOURCES tests/vimeo_player.cpp
    LIBS maicord_protocol maicord_core kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 5 TEST maicord_vimeo_player)

maicord_test(MaiCordAppleMusicPlayerTests ENTRY maicord_apple_music_player_entry
    SOURCES tests/apple_music_player.cpp
    LIBS maicord_protocol maicord_core kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 5 TEST maicord_apple_music_player)

maicord_test(MaiCordMp4FragmentTests ENTRY maicord_mp4_fragment_entry
    SOURCES tests/mp4_fragment.cpp tests/steam_playback_probe.cpp tests/twitch_fragment_probe.cpp tests/twitch_live_session.cpp
        tests/fragment_audio_playback_probe.cpp
    LIBS maicord_account_media maicord_media_codec maicord_platform maicord_core kernel32 ole32 user32 d3d11 dxgi
    TIMEOUT 10 TEST maicord_mp4_fragment)

maicord_test(MaiCordOAuthProtocolTests ENTRY maicord_oauth_protocol_entry
    SOURCES tests/oauth_protocol.cpp
    LIBS maicord_protocol maicord_core kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 5 TEST maicord_oauth_protocol)

maicord_test(MaiCordSpotifyOAuthTests ENTRY maicord_spotify_oauth_entry
    SOURCES tests/spotify_oauth.cpp
    LIBS maicord_app_core maicord_protocol maicord_platform maicord_core kernel32 ws2_32 secur32 bcrypt crypt32
    BUNDLE MaiCordOAuthTests
    TIMEOUT 5 TEST maicord_spotify_oauth)

maicord_test(MaiCordOAuthAuthorizationTests ENTRY maicord_oauth_authorization_entry
    SOURCES tests/oauth_authorization.cpp
    LIBS maicord_app_core maicord_protocol maicord_platform maicord_core kernel32 ws2_32 secur32 bcrypt crypt32
    BUNDLE MaiCordOAuthTests
    TIMEOUT 5 TEST maicord_oauth_authorization)

maicord_test(MaiCordOAuthSignInTests ENTRY maicord_oauth_sign_in_entry
    SOURCES tests/oauth_sign_in.cpp
    LIBS maicord_app_core maicord_protocol maicord_platform maicord_core kernel32 ws2_32 secur32 bcrypt crypt32
    BUNDLE MaiCordOAuthTests
    TIMEOUT 8 TEST maicord_oauth_sign_in)

maicord_test(MaiCordOAuthAccountTests ENTRY maicord_oauth_account_entry
    SOURCES tests/oauth_account.cpp
    LIBS maicord_app_core maicord_protocol maicord_platform maicord_core kernel32 ws2_32 secur32 bcrypt crypt32
    BUNDLE MaiCordOAuthTests
    TIMEOUT 5 TEST maicord_oauth_account)

maicord_test(MaiCordDashManifestTests ENTRY maicord_dash_manifest_entry
    SOURCES tests/dash_manifest.cpp
    LIBS maicord_protocol maicord_core kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 5 TEST maicord_dash_manifest)

maicord_test(MaiCordSteamPlayerTests ENTRY maicord_steam_player_entry
    SOURCES tests/steam_player.cpp
    LIBS maicord_protocol maicord_core kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 5 TEST maicord_steam_player)

maicord_test(MaiCordHlsPlaylistTests ENTRY maicord_hls_playlist_entry
    SOURCES tests/hls_playlist.cpp
    LIBS maicord_protocol maicord_core kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 5 TEST maicord_hls_playlist)

maicord_test(MaiCordMpegTsTests ENTRY maicord_mpeg_ts_entry
    SOURCES tests/mpeg_ts.cpp
    LIBS maicord_account_media maicord_media_codec maicord_platform maicord_core kernel32 ole32 user32 d3d11 dxgi
    TIMEOUT 10 TEST maicord_mpeg_ts)

maicord_test(MaiCordImgurPlayerTests ENTRY maicord_imgur_player_entry
    SOURCES tests/imgur_player.cpp
    LIBS maicord_protocol maicord_core kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 5 TEST maicord_imgur_player)

maicord_test(MaiCordTwitterPlayerTests ENTRY maicord_twitter_player_entry
    SOURCES tests/twitter_player.cpp
    LIBS maicord_protocol maicord_core kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 5 TEST maicord_twitter_player)

maicord_test(MaiCordAmazonMusicPlayerTests ENTRY maicord_amazon_music_player_entry
    SOURCES tests/amazon_music_player.cpp
    LIBS maicord_protocol maicord_core kernel32
    BUNDLE MaiCordProtocolTests
    TIMEOUT 5 TEST maicord_amazon_music_player)
