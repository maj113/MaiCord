# Tests of the application and the UI on Linux: the fixtures of cmake/test_registry.cmake whose sources need no Win32
# window or message loop, over the Linux application library (linux_app_client.cmake), the null window backend and
# the shared tests/support Win32 compat shims (win32_test_compat.hpp, linux_test_app.hpp). Same names as the Windows
# registry. The bundles join their members into one process per bundle, as on Windows.
# The portable units of src/app/shell the fixtures link (the shell itself belongs to the window layer).
maicord_library(maicord_test_shell_units
    src/app/shell/shortcut_registry.cpp src/app/shell/navigation_shortcuts.cpp src/app/shell/quick_switcher.cpp
    src/app/shell/quick_switcher_controller.cpp src/app/shell/input_layers.cpp
    LIBS maicord_core INCLUDES src/app)

set(maicord_linux_app_test_libs maicord_app_client maicord_test_shell_units maicord_ui maicord_ui_secure_text
    maicord_text_system maicord_text_catalog maicord_font_system maicord_font maicord_font_color maicord_theme
    maicord_unicode maicord_render_core maicord_window_null maicord_desktop_system maicord_dbus maicord_atspi maicord_model
    maicord_protocol maicord_media_core maicord_media_codec maicord_compression maicord_crypto maicord_net
    maicord_core)
maicord_test_bundle(MaiCordAppUnitTests LIBS ${maicord_linux_app_test_libs} INCLUDES src/app)
maicord_test_bundle(MaiCordAppWaveTests LIBS ${maicord_linux_app_test_libs} INCLUDES src/app)
maicord_test_bundle(MaiCordUiUnitTests LIBS ${maicord_linux_app_test_libs} maicord_edit_history INCLUDES src/app)
maicord_test_bundle(MaiCordAccountRequestTests LIBS ${maicord_linux_app_test_libs} INCLUDES src/app)

# Not registered here: the Windows fixtures that drive a real Win32 window and message loop, call the Windows API
# directly (BCrypt, Winsock, GDI, UI Automation, the registry), depend on the Windows clipboard
# (ChannelOverviewRuntime, SearchRuntime: the clipboard seam is the Wayland backend's) or on the size of a Windows
# committed region (MessageReactionRuntime). The plugin runtime tests, which load plugins, are cmake/linux_plugin_tests.cmake.

# The seams this port added, on both systems (tests/os) and the Linux services behind the desktop layer.
maicord_test(MaiCordOsTextTests ENTRY maicord_os_text_entry SOURCES tests/os/text.cpp
    BUNDLE MaiCordCoreUnitTests TIMEOUT 10 TEST maicord_os_text)
maicord_test(MaiCordOsProcessTests ENTRY maicord_os_process_entry SOURCES tests/os/process.cpp
    BUNDLE MaiCordCoreUnitTests TIMEOUT 10 TEST maicord_os_process)
maicord_test(MaiCordLinuxResourceTests ENTRY maicord_linux_resource_entry SOURCES tests/core/linux_resource.cpp
    BUNDLE MaiCordCoreUnitTests TIMEOUT 10 TEST maicord_linux_resource)
# The files seam's path policy (include/maicord/os/path.hpp) and the shared code that asks it, on a Linux profile.
maicord_test(MaiCordPathPolicyTests ENTRY maicord_path_policy_entry SOURCES tests/os/path_policy.cpp
    LIBS ${maicord_linux_app_test_libs} TIMEOUT 20 TEST maicord_path_policy)
# The settings catalog's Linux wording (no row names Windows) and the rows this system leaves out.
maicord_test(MaiCordLinuxCatalogTextTests ENTRY maicord_linux_catalog_text_entry
    SOURCES tests/app/settings/linux_catalog_text.cpp
    LIBS ${maicord_linux_app_test_libs} INCLUDES src/app
    TIMEOUT 10 TEST maicord_linux_catalog_text)
# The sign-in screen's wording for a security key on Linux, where none can be used (src/ui/account/login_text_presentation.cpp).
maicord_test(MaiCordLinuxWebAuthnTextTests ENTRY maicord_linux_webauthn_text_entry
    SOURCES tests/ui/account/linux_webauthn_text.cpp
    LIBS ${maicord_linux_app_test_libs} INCLUDES src/app
    TIMEOUT 10 TEST maicord_linux_webauthn_text)
# Push to talk through the GlobalShortcuts portal while a call is open (src/app/voice/push_to_talk_binding.cpp).
maicord_test(MaiCordPushToTalkBindingTests ENTRY maicord_push_to_talk_binding_entry
    SOURCES tests/app/voice/push_to_talk_binding.cpp
    LIBS ${maicord_linux_app_test_libs} INCLUDES src/app
    TIMEOUT 10 TEST maicord_push_to_talk_binding)
# The termination signals as a loop event (src/app/linux/shutdown_signal.cpp), and the minimum window size that follows
# the layout (src/app/shell/window.cpp: sign-in, main layout, panes).
maicord_test(MaiCordLinuxQuitSignalTests ENTRY maicord_linux_quit_signal_entry
    SOURCES tests/app/shell/linux_quit_signal.cpp src/app/linux/shutdown_signal.cpp
    LIBS maicord_core INCLUDES src/app
    TIMEOUT 10 TEST maicord_linux_quit_signal)
maicord_test(MaiCordLinuxWindowMinimumTests ENTRY maicord_linux_window_minimum_entry
    SOURCES tests/app/shell/linux_window_minimum.cpp src/app/shell/state.cpp src/app/shell/window.cpp
    LIBS maicord_core maicord_window_null maicord_protocol maicord_model maicord_app_client maicord_ui
        ${maicord_linux_app_test_libs}
    INCLUDES src/app
    TIMEOUT 10 TEST maicord_linux_window_minimum)
# Startup and protocol registration write under XDG_CONFIG_HOME and XDG_DATA_HOME: scratch directories.
maicord_test(MaiCordLinuxIntegrationTests ENTRY maicord_linux_integration_entry
    SOURCES tests/platform/desktop/linux_integration.cpp
    LIBS maicord_app_client maicord_desktop_system maicord_core
    TIMEOUT 10
    ENV XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-profile/linux-integration/config
        XDG_DATA_HOME=${CMAKE_BINARY_DIR}/test-profile/linux-integration/data
        APPDIR=${CMAKE_BINARY_DIR}
        APPIMAGE=${CMAKE_BINARY_DIR}/test-profile/linux-integration/Test.AppImage
    TEST maicord_linux_integration)
# The launch's arguments as the command line the launch parser reads (the Linux entry's input to the shared startup).
maicord_test(MaiCordLinuxLaunchLineTests ENTRY maicord_linux_launch_line_entry
    SOURCES tests/platform/desktop/linux_launch_line.cpp
    LIBS maicord_app_client maicord_desktop_system maicord_core
    TIMEOUT 10 TEST maicord_linux_launch_line)
# The account's file locations and the start-up logout probe on a Linux profile.
maicord_test(MaiCordLinuxAccountPathsTests ENTRY maicord_linux_account_paths_entry
    SOURCES tests/app/account/linux_account_paths.cpp
    LIBS maicord_app_client maicord_core
    TIMEOUT 10 TEST maicord_linux_account_paths)
# The toast layer without a session bus: no daemon is ever reached. The _mock case runs it against the mock notification
# daemon of tools/test_dbus.sh (skipped, 77, outside that script).
maicord_test(MaiCordLinuxNotificationsTests ENTRY maicord_linux_notifications_entry
    SOURCES tests/platform/desktop/linux_notifications.cpp
    LIBS maicord_app_client maicord_desktop_system maicord_dbus maicord_window_null maicord_core
    TIMEOUT 10
    ENV DBUS_SESSION_BUS_ADDRESS=unix:path=/maicord-no-session-bus
    TEST maicord_linux_notifications)
maicord_add_tests(MaiCordLinuxNotificationsTests TIMEOUT 20 TEST maicord_linux_notifications_mock --mock)
set_tests_properties(maicord_linux_notifications_mock PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK dbus_private_bus)
# The security-key assertion (src/platform/desktop/linux/fido2.cpp, webauthn_ceremony.cpp) against a fake libfido2: what the
# library is asked and what comes back, the PIN, no key, no touch, a cancel; and, with no libfido2, an unavailable state.
maicord_test(MaiCordLinuxFido2Tests ENTRY maicord_linux_fido2_entry
    SOURCES tests/platform/desktop/linux_fido2.cpp
    LIBS maicord_app_client maicord_desktop_system maicord_dbus maicord_window_null maicord_core
    TIMEOUT 30 TEST maicord_linux_fido2)
maicord_add_tests(MaiCordLinuxFido2Tests TIMEOUT 10 ENV MAICORD_FIDO2_LIBRARY=libmaicord-no-such-fido2.so.1
    TEST maicord_linux_fido2_no_library --no-library)
# The sign-in's side of the ceremony (src/app/account/login/auth.cpp) over a fake ceremony: the screens, the PIN field, each
# failure, a cancel, no libfido2. The fake ceremony replaces webauthn_ceremony.cpp, which stays out of the link.
maicord_test(MaiCordLinuxWebAuthnFlowTests ENTRY maicord_linux_webauthn_flow_entry
    SOURCES tests/app/account/login/linux_webauthn_flow.cpp src/app/account/login/browser_auth.cpp
        src/app/account/login/captcha_auth.cpp src/app/account/login/auth.cpp src/app/account/login/auth_recovery.cpp
        src/app/shell/state.cpp src/app/account/account_host.cpp
    LIBS ${maicord_linux_app_test_libs}
    INCLUDES src/app
    TIMEOUT 20 TEST maicord_linux_webauthn_flow)


maicord_test(MaiCordUpdateChangelogTests ENTRY maicord_update_changelog_tests_entry
    SOURCES tests/app/update/changelog.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAppUnitTests
    TIMEOUT 10
    TEST maicord_update_changelog)

maicord_test(MaiCordLayoutConsistencyTests ENTRY maicord_layout_consistency_tests_entry
    SOURCES tests/ui/shell/layout_consistency.cpp
    LIBS ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAppUnitTests
    TIMEOUT 10
    TEST maicord_layout_consistency)

maicord_test(MaiCordNavigationGapsTests ENTRY maicord_navigation_gaps_entry
    SOURCES tests/ui/shell/navigation_gaps.cpp
    LIBS ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAppWaveTests
    TIMEOUT 10
    TEST maicord_navigation_gaps)

maicord_test(MaiCordNavigationWaveTests ENTRY maicord_navigation_wave_entry
    SOURCES tests/app/gateway/navigation_wave.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAppWaveTests
    TIMEOUT 10
    TEST maicord_navigation_wave)

maicord_test(MaiCordGuildBannerTests ENTRY maicord_guild_banner_entry
    SOURCES tests/ui/guild/banner.cpp
    LIBS ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAppWaveTests
    TIMEOUT 10
    TEST maicord_guild_banner)

maicord_test(MaiCordPresenceWaveTests ENTRY maicord_presence_wave_entry
    SOURCES tests/ui/widgets/presence_wave.cpp src/app/account/game_detection.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAppWaveTests
    TIMEOUT 10
    TEST maicord_presence_wave)

maicord_test(MaiCordProfileActivityTests ENTRY maicord_profile_activity_entry
    SOURCES tests/ui/account/profile_activity.cpp
    LIBS ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAppWaveTests
    TIMEOUT 10
    TEST maicord_profile_activity)

maicord_test(MaiCordRegionalFlagNativeGeometryTests ENTRY maicord_flag_native_geometry_entry
    SOURCES tests/ui/text/flag_native_geometry.cpp src/ui/text/native_text_candidate.cpp src/ui/text/native_text_catalog.cpp src/ui/media/emoji_presentation.cpp
    LIBS maicord_core maicord_font maicord_media_core maicord_ui ${maicord_linux_app_test_libs}
    TIMEOUT 15
    ENVIRONMENT LOCALAPPDATA=
    TEST maicord_regional_flag_native_geometry)

maicord_test(MaiCordFriendsAccountTests ENTRY maicord_friends_account_entry
    SOURCES tests/app/account/account_relationships.cpp tests/protocol/http/rest_submit_hook.cpp src/app/account/account_relationships.cpp src/app/chat/messages/account_message_ack.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAccountRequestTests
    TEST maicord_friends_account)

maicord_test(MaiCordFriendsRuntimeTests ENTRY maicord_friends_runtime_entry
    SOURCES tests/app/account/friends_runtime.cpp src/app/shell/state.cpp src/app/account/friends_search.cpp src/app/account/friends_storage.cpp src/app/account/account_relationships.cpp
    LIBS maicord_core maicord_window_null maicord_protocol maicord_model maicord_app_client maicord_ui ${maicord_linux_app_test_libs}
    INCLUDES src/app
    TEST maicord_friends_runtime)

maicord_test(MaiCordChannelOverviewAccountTests ENTRY maicord_channel_overview_account_entry
    SOURCES tests/app/guild/account_channel_overview.cpp tests/protocol/http/rest_submit_hook.cpp src/app/guild/account_channel_overview.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_channel_overview_account)

maicord_test(MaiCordGuildAdminAccountTests ENTRY maicord_account_guild_admin_entry
    SOURCES tests/app/guild/account_guild_admin.cpp tests/protocol/http/rest_submit_hook.cpp src/app/guild/account_guild_admin.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_guild_admin_account)

maicord_test(MaiCordStageAccountTests ENTRY maicord_account_stage_entry
    SOURCES tests/app/voice/account_stage.cpp tests/protocol/http/rest_submit_hook.cpp src/app/voice/account_stage.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 10
    TEST maicord_stage_account)

maicord_test(MaiCordUserSocialAccountTests ENTRY maicord_account_user_social_entry
    SOURCES tests/app/account/account_user_social.cpp tests/protocol/http/rest_submit_hook.cpp src/app/account/account_user_social.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 10
    TEST maicord_user_social_account)

maicord_test(MaiCordProfileImageTests ENTRY maicord_profile_image_entry
    SOURCES tests/app/media/profile_image.cpp tests/support/media_fixture.cpp tests/support/media_fixture_encoders.cpp src/app/media/profile_image.cpp src/app/media/guild_image.cpp src/app/media/local_preview_thumbnail.cpp
    LIBS maicord_app_client maicord_core maicord_window_null maicord_desktop_system maicord_dbus maicord_protocol maicord_model maicord_media_codec maicord_compression ${maicord_linux_app_test_libs}
    TIMEOUT 10
    TEST maicord_profile_image)

maicord_test(MaiCordStorePreviewTests ENTRY maicord_store_preview_entry
    SOURCES tests/app/store/store_preview.cpp tests/support/media_fixture.cpp tests/support/media_fixture_encoders.cpp src/app/store/store_preview_decode.cpp src/app/media/local_preview_thumbnail.cpp
    LIBS maicord_app_client maicord_core maicord_window_null maicord_desktop_system maicord_dbus maicord_protocol maicord_model maicord_media_codec maicord_compression ${maicord_linux_app_test_libs}
    TIMEOUT 10
    TEST maicord_store_preview)

maicord_test(MaiCordProfileMediaFitTests ENTRY maicord_profile_media_fit_entry
    SOURCES tests/app/media/profile_media_fit.cpp tests/support/media_fixture.cpp tests/support/media_fixture_encoders.cpp
        tests/platform/audio/window_post_stub.cpp
    LIBS maicord_video_decode maicord_aac_decoder maicord_audio_pulse maicord_gpu_gles_video maicord_gpu_gles_shaders
        maicord_gpu_gles maicord_video_software maicord_paint_program ${maicord_linux_app_test_libs}
    TIMEOUT 10
    TEST maicord_profile_media_fit)

maicord_test(MaiCordDmProfileTests ENTRY maicord_dm_profile_tests_entry
    SOURCES tests/ui/account/dm_profile_panel.cpp src/app/account/dm_profile.cpp src/app/shell/state.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAppUnitTests
    TIMEOUT 10
    TEST maicord_dm_profile)

maicord_test(MaiCordAccountMessageSearchTests ENTRY maicord_account_message_search_test_entry
    SOURCES tests/app/chat/search/account_message_search.cpp tests/protocol/http/rest_submit_hook.cpp src/app/chat/search/account_message_search.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 10
    TEST maicord_account_message_search)

maicord_test(MaiCordAccountPinnedMessagesTests ENTRY maicord_account_pinned_messages_test_entry
    SOURCES tests/app/chat/messages/account_pinned_messages.cpp tests/protocol/http/rest_submit_hook.cpp src/app/chat/messages/account_pinned_messages.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 10
    TEST maicord_account_pinned_messages)

maicord_test(MaiCordAccountThreadActionsTests ENTRY maicord_account_thread_actions_test_entry
    SOURCES tests/app/chat/threads/account_thread_actions.cpp tests/protocol/http/rest_submit_hook.cpp src/app/chat/threads/account_thread_actions.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_account_thread_actions)

maicord_test(MaiCordAccountThreadBrowserTests ENTRY maicord_account_thread_browser_test_entry
    SOURCES tests/app/chat/threads/account_thread_browser.cpp tests/protocol/http/rest_submit_hook.cpp src/app/chat/threads/account_thread_browser.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_account_thread_browser)

maicord_test(MaiCordApplicationThreadBrowserTests ENTRY maicord_application_thread_browser_entry
    SOURCES tests/app/chat/threads/browser.cpp
    LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client maicord_ui maicord_ui_secure_text maicord_text_system ${maicord_linux_app_test_libs}
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_application_thread_browser)

maicord_test(MaiCordAccountThreadCreationTests ENTRY maicord_account_thread_creation_test_entry
    SOURCES tests/app/chat/threads/account_thread_creation.cpp tests/protocol/http/rest_submit_hook.cpp src/app/chat/threads/account_thread_creation.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_account_thread_creation)

maicord_test(MaiCordAccountPollCreateTests ENTRY maicord_account_poll_create_test_entry
    SOURCES tests/app/chat/polls/account_poll_create.cpp tests/protocol/http/rest_submit_hook.cpp src/app/chat/polls/account_poll_create.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_account_poll_create)

maicord_test(MaiCordAccountNotificationSettingsTests ENTRY maicord_account_notification_settings_test_entry
    SOURCES tests/app/notifications/account_notification_settings.cpp tests/protocol/http/rest_submit_hook.cpp src/app/notifications/account_notification_settings.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_account_notification_settings)

maicord_test(MaiCordAccountPrivateChannelTests ENTRY maicord_account_private_channel_test_entry
    SOURCES tests/app/chat/account_private_channel.cpp tests/protocol/http/rest_submit_hook.cpp src/app/chat/account_private_channel.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_account_private_channel)

maicord_test(MaiCordApplicationThreadCreationTests ENTRY maicord_application_thread_creation_entry
    SOURCES tests/app/chat/threads/creation.cpp
    LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client maicord_ui maicord_ui_secure_text maicord_text_system ${maicord_linux_app_test_libs}
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_application_thread_creation)

maicord_test(MaiCordAccountForumPostTests ENTRY maicord_account_forum_post_test_entry
    SOURCES tests/app/chat/threads/account_forum_post.cpp tests/protocol/http/rest_submit_hook.cpp src/app/chat/threads/account_forum_post.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAccountRequestTests
    TIMEOUT 15
    TEST maicord_account_forum_post)

maicord_test(MaiCordApplicationForumPreviewTests ENTRY maicord_application_forum_preview_entry
    SOURCES tests/app/chat/threads/forum_preview.cpp
    LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client maicord_ui maicord_ui_secure_text maicord_text_system maicord_render_core ${maicord_linux_app_test_libs}
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_application_forum_preview)

maicord_test(MaiCordMessagePinnedStateTests ENTRY maicord_message_pinned_state_entry
    SOURCES tests/app/gateway/message_pinned_state.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAppUnitTests
    TIMEOUT 15
    TEST maicord_message_pinned_state)

maicord_test(MaiCordMessageMutationNoticeTests ENTRY maicord_message_mutation_notice_tests_entry
    SOURCES tests/app/chat/messages/mutation_notice.cpp src/app/chat/messages/mutation_notice.cpp src/app/chat/threads/actions.cpp src/app/chat/messages/delete.cpp src/app/chat/messages/reaction.cpp src/app/shell/state.cpp
    LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client maicord_ui maicord_ui_secure_text maicord_text_system ${maicord_linux_app_test_libs}
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_message_mutation_notice)

maicord_test(MaiCordSelfStatusRuntimeTests ENTRY maicord_self_status_runtime_entry
    SOURCES tests/app/account/self_status_runtime.cpp src/app/account/self_status.cpp src/app/shell/state.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAppUnitTests
    TIMEOUT 10
    TEST maicord_self_status_frontend_runtime)

maicord_test(MaiCordSearchContentRuntimeTests ENTRY maicord_application_search_content_runtime_entry
    SOURCES tests/app/chat/search/content_runtime.cpp src/app/shell/state.cpp
    LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client maicord_ui maicord_ui_secure_text maicord_text_system ${maicord_linux_app_test_libs}
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_search_content_runtime)

maicord_test(MaiCordSearchNavigationRuntimeTests ENTRY maicord_application_search_navigation_runtime_entry
    SOURCES tests/app/chat/search/navigation_runtime.cpp src/app/chat/messages/mutation_notice.cpp
    LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client maicord_ui maicord_ui_secure_text maicord_text_system ${maicord_linux_app_test_libs}
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_search_navigation_runtime)

maicord_test(MaiCordQuickSwitcherRuntimeTests ENTRY maicord_application_quick_switcher_runtime_entry
    SOURCES tests/app/shell/quick_switcher_runtime.cpp src/app/shell/quick_switcher.cpp src/app/shell/input_routing.cpp src/app/shell/state.cpp
    LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client maicord_ui maicord_ui_secure_text maicord_text_system ${maicord_linux_app_test_libs}
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_quick_switcher_runtime)

maicord_test(MaiCordMessageForwardRuntimeTests ENTRY maicord_application_message_forward_runtime_entry
    SOURCES tests/app/chat/messages/forward_runtime.cpp src/app/shell/input_routing.cpp src/app/shell/state.cpp
    LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client maicord_ui maicord_ui_secure_text maicord_text_system ${maicord_linux_app_test_libs}
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_message_forward_runtime)

maicord_test(MaiCordToastTests ENTRY maicord_toast_tests_entry
    SOURCES tests/ui/widgets/toast.cpp
    LIBS ${maicord_linux_app_test_libs}
    BUNDLE MaiCordUiUnitTests
    TIMEOUT 5
    TEST maicord_toast)

maicord_test(MaiCordPinnedMessagesRuntimeTests ENTRY maicord_application_pinned_messages_runtime_entry
    SOURCES tests/app/chat/messages/pinned_messages_runtime.cpp src/app/shell/state.cpp
    LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client maicord_ui maicord_ui_secure_text maicord_text_system ${maicord_linux_app_test_libs}
    INCLUDES src/app
    TIMEOUT 15
    TEST maicord_pinned_messages_runtime)

maicord_test(MaiCordPasswordRecoveryCoordinatorTests ENTRY maicord_password_recovery_coordinator_entry
    SOURCES tests/app/account/login/password_recovery_coordinator.cpp
    LIBS maicord_core maicord_window_null maicord_desktop_system maicord_dbus maicord_protocol maicord_app_client maicord_crypto maicord_net ${maicord_linux_app_test_libs}
    TEST maicord_password_recovery_coordinator)

maicord_test(MaiCordPhoneRecoveryCoordinatorTests ENTRY maicord_phone_recovery_coordinator_entry
    SOURCES tests/app/account/login/phone_recovery_coordinator.cpp
    LIBS maicord_core maicord_window_null maicord_desktop_system maicord_dbus maicord_protocol maicord_app_client maicord_crypto maicord_net ${maicord_linux_app_test_libs}
    TIMEOUT 15
    TEST maicord_phone_recovery_coordinator)

maicord_test(MaiCordPasswordResetCoordinatorTests ENTRY maicord_password_reset_coordinator_entry
    SOURCES tests/app/account/login/password_reset_coordinator.cpp src/app/account/account_profile.cpp src/app/account/login/mfa_policy.cpp src/app/net/file_persistence.cpp src/app/account/login/captcha_challenge.cpp
    LIBS maicord_app_client maicord_core maicord_window_null maicord_desktop_system maicord_dbus maicord_protocol maicord_crypto maicord_net ${maicord_linux_app_test_libs}
    TEST maicord_password_reset_coordinator)

maicord_test(MaiCordGuildChannelUniquenessTests ENTRY maicord_guild_channel_uniqueness_entry
    SOURCES tests/app/gateway/guild_channel_uniqueness.cpp
    LIBS maicord_app_client ${maicord_linux_app_test_libs}
    BUNDLE MaiCordAppUnitTests
    TIMEOUT 20
    TEST maicord_guild_channel_uniqueness)

maicord_test(MaiCordTextPointerSelectionTests ENTRY maicord_text_pointer_selection_entry
    SOURCES tests/ui/text/pointer_selection.cpp
    LIBS ${maicord_linux_app_test_libs}
    BUNDLE MaiCordUiUnitTests
    TIMEOUT 10
    TEST maicord_text_pointer_selection)

maicord_test(MaiCordRectangleGeometryTests ENTRY rectangle_geometry_entry
    SOURCES tests/ui/core/rectangle_geometry.cpp
    LIBS ${maicord_linux_app_test_libs}
    BUNDLE MaiCordUiUnitTests
    TIMEOUT 5
    TEST maicord_rectangle_geometry)
