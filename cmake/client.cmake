# The shipping pair: MaiCord.exe and the account module MaiCordAccount.dll,
# their runtime assets and the cold-code size policy.

add_library(MaiCordAccount SHARED
    src/app/account_message_poll.cpp src/app/account_guild_invite.cpp src/app/account_settings_rest.cpp
    src/app/account_self_status.cpp src/app/account_module.cpp src/app/account_message_search.cpp
    src/app/account_media_catalog.cpp src/app/account_guild_assets.cpp src/app/guild_image.cpp
    src/app/profile_image.cpp src/app/account_profile_edit.cpp
    $<TARGET_OBJECTS:maicord_runtime>)
target_link_libraries(MaiCordAccount PRIVATE
    maicord_compile_contract maicord_core maicord_platform maicord_protocol
    maicord_app_core maicord_model maicord_account_media maicord_media_core
    maicord_media_codec maicord_cdn_media
    kernel32 ws2_32 secur32 bcrypt crypt32 maicord_voice_encoder)
set_target_properties(MaiCordAccount PROPERTIES
    CXX_EXTENSIONS OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded")
# No manifest: LINK's default one holds only an asInvoker trustInfo, which a
# DLL ignores, and would cost the image a .rsrc section.
target_link_options(MaiCordAccount PRIVATE
    /ENTRY:maicord_account_module_entry
    /EXPORT:maicord_account_get_api,@126,NONAME
    /SUBSYSTEM:WINDOWS,6.02
    /MANIFEST:NO)
if(MAICORD_GENERATE_LINK_MAPS)
    target_link_options(MaiCordAccount PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/MaiCordAccount.map")
endif()

add_executable(MaiCord WIN32
    src/app/application_poll.cpp config/maicord.manifest config/maicord.rc
    "${MAICORD_NOTIFICATION_SOUND_RESOURCE}" src/app/application_state.cpp src/app/application_settings.cpp
    src/app/settings_catalog.cpp src/app/application_user_settings.cpp src/app/application_account_settings.cpp
    src/app/application_theme_editor.cpp src/app/application_custom_theme.cpp
    src/app/application_theme_publication.cpp src/app/application_friends.cpp
    src/app/application_friends_search.cpp src/app/application_friends_storage.cpp
    src/app/application_plugins.cpp src/app/application_plugins_sdk.cpp src/app/application_plugins_read.cpp
    src/app/application_plugins_actions.cpp src/app/application_plugins_surfaces.cpp
    src/app/application_plugins_render.cpp src/app/application_plugins_fx.cpp
    src/app/application_plugins_compose.cpp src/app/application_plugin_menus.cpp src/app/login_continuations.cpp
    src/app/application_window.cpp src/app/application_window_runtime.cpp src/app/application_input_routing.cpp
    src/app/application_keyboard_focus.cpp src/app/application_account_module.cpp
    src/app/account_module_decoders.cpp src/app/account_module_cdn_urls.cpp src/app/application_account_logout.cpp
    src/app/application_account_pump.cpp src/app/application_account_presentation.cpp
    src/app/application_thread_starter.cpp src/app/application_channel_session.cpp
    src/app/application_channel_snapshots.cpp src/app/application_commands.cpp src/app/application_composer.cpp
    src/app/application_message_feedback.cpp src/app/application_reply_draft.cpp
    src/app/application_composer_input.cpp src/app/application_button_input.cpp
    src/app/application_composer_edit_menu.cpp src/app/application_composer_spellcheck.cpp
    src/app/application_attachments.cpp src/app/application_attachment_allowance.cpp
    src/app/application_upload_lane.cpp src/app/application_content_input.cpp src/app/application_link_trust.cpp
    src/app/application_image_viewer.cpp src/app/application_frame_presentation.cpp
    src/app/application_unread_banner.cpp src/app/application_message_intro.cpp
    src/app/application_guild_folders.cpp src/app/application_navigation_drag.cpp
    src/app/application_guild_lifecycle.cpp src/app/application_profile_editor.cpp
    src/app/application_guild_menu.cpp src/app/application_guild_community.cpp
    src/app/application_guild_card.cpp src/app/application_guild_banner.cpp src/app/application_guild_invite.cpp
    src/app/application_taskbar_attention.cpp src/app/application_desktop.cpp
    src/app/application_desktop_settings.cpp src/app/application_surface_motion.cpp
    src/app/application_text_form.cpp src/app/application_composer_text_input.cpp
    src/app/application_login_auth.cpp src/app/application_login_input.cpp src/app/application_login_ui.cpp
    src/app/application_ui_runtime.cpp src/app/account_permission_storage.cpp src/app/application_media.cpp
    src/app/application_video.cpp src/app/application_tooltip.cpp src/app/application_reaction_tooltip.cpp
    src/app/application_account_menu.cpp
    src/app/application_account_switch.cpp src/app/application_search.cpp src/app/application_search_content.cpp
    src/app/application_search_navigation.cpp src/app/application_thread_actions.cpp
    src/app/application_thread_browser.cpp src/app/application_thread_creation.cpp
    src/app/application_forum_post.cpp src/app/application_forum_form.cpp
    src/app/application_forum_attachments.cpp src/app/application_forum_preview.cpp
    src/app/application_composer_preview.cpp src/app/application_local_preview.cpp
    src/app/application_attachment_probe.cpp src/app/attachment_media_probe.cpp
    src/app/application_thread_header_activation.cpp src/app/application_thread_header.cpp
    src/app/application_quick_switcher.cpp src/app/application_message_forward.cpp
    src/app/application_downloads.cpp src/app/application_shortcuts.cpp
    src/app/application_keybinds_settings.cpp src/app/application_pinned_messages.cpp
    src/app/application_media_picker.cpp src/app/application_gif_favorites.cpp
    src/app/application_gif_favorite_star.cpp
    src/app/application_emoji_preferences.cpp src/app/application_action_captcha.cpp
    src/app/application_channel_overview.cpp src/app/application_settings_layer.cpp
    src/app/application_guild_settings.cpp src/app/application_guild_settings_profile.cpp
    src/app/application_guild_settings_engagement.cpp src/app/application_guild_settings_expressions.cpp
    src/app/application_guild_settings_members.cpp src/app/application_guild_settings_roles.cpp
    src/app/application_guild_settings_invites.cpp src/app/application_guild_settings_safety.cpp
    src/app/application_guild_settings_audit_log.cpp src/app/application_guild_settings_bans.cpp
    src/app/application_guild_settings_boost.cpp
    src/app/application_poll_create.cpp
    src/app/application_self_status.cpp src/app/application_browser_auth.cpp
    src/app/application_captcha_auth.cpp src/app/application_media_animation.cpp
    src/app/application_media_requests.cpp src/app/application_message_delete.cpp
    src/app/application_private_conversation.cpp src/app/application_private_channel.cpp
    src/app/application_message_edit.cpp src/app/application_message_mutation_notice.cpp
    src/app/application_profile.cpp src/app/application_member_moderation.cpp
    src/app/application_message_reaction.cpp src/app/application_message_send.cpp
    src/app/application_mention_confirmation.cpp src/app/application_message_history.cpp
    src/app/application_read_state.cpp src/app/application_scoped_read.cpp
    src/app/application_scoped_read_menu.cpp src/app/application_notifications.cpp
    src/app/application_notification_controls.cpp src/app/application_custom_status.cpp
    src/app/application_activity.cpp src/app/game_detection.cpp src/app/application_inbox.cpp
    src/app/application_message_interaction.cpp src/app/application_user_actions.cpp
    src/app/application_report.cpp src/app/application_scheduler.cpp src/app/application_shutdown_metrics.cpp
    src/app/application_scrolling.cpp src/app/application_scroll_host.cpp src/app/application_shell_input.cpp
    src/app/application_pane_resize.cpp src/app/main.cpp src/core/log.cpp $<TARGET_OBJECTS:maicord_runtime>
    src/app/application_accessibility.cpp src/app/application_updates.cpp src/app/application_update_entry.cpp
    src/app/application_plugin_browser.cpp src/app/application_plugin_browser_worker.cpp
    src/app/application_plugin_browser_entry.cpp src/app/application_plugin_browser_projection.cpp
    # Voice calls: soundboard, voice messages, preferences, voice card, mute/deafen,
    # the voice user menu, the call view, stage channels and Go Live.
    src/app/application_soundboard.cpp src/app/application_voice_message.cpp src/app/application_voice.cpp
    src/app/application_voice_settings.cpp src/app/voice_settings_binding.cpp src/app/application_call.cpp
    src/app/application_stage.cpp src/app/application_message_components.cpp
    src/app/application_invite_cards.cpp src/app/application_stream.cpp)
target_link_libraries(MaiCord PRIVATE maicord_compile_contract
    maicord_core maicord_text maicord_ui maicord_model maicord_client_preferences
    maicord_notification_sound_assets maicord_plugin_host maicord_plugin_modules maicord_plugin_manager maicord_ui_input
    maicord_message_runtime maicord_message_media maicord_account_paths maicord_account_host
    maicord_render_core maicord_atlas_gpu maicord_renderer maicord_media_core maicord_webauthn
    kernel32 user32 shell32 d3d11 dxgi dcomp maicord_uia maicord_app_core ws2_32 secur32 bcrypt)
target_include_directories(MaiCord PRIVATE "${MAICORD_GENERATED_DIRECTORY}")
set_target_properties(MaiCord PROPERTIES
    CXX_EXTENSIONS OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_link_options(MaiCord PRIVATE
    /ENTRY:maicord_entry
    /SUBSYSTEM:WINDOWS,6.02)
if(MAICORD_GENERATE_LINK_MAPS)
    target_link_options(MaiCord PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/MaiCord.map")
endif()
# Startup: DLLs the device prestart does not need map on first use instead of
# before the entry point, which the prestart waits for. The renderer maps
# dcomp, shell32, ole32 and advapi32 (which GPU drivers import) on the main
# thread while the worker creates the device; the rest load when first used.
# dloadhelper resolves through kernel32's ResolveDelayLoadedAPI (no CRT).
# tools/audit_pe.ps1 (-DelayLoad) and maicord_client_imports check the list.
set(MAICORD_CLIENT_DELAY_LOADS dcomp.dll shell32.dll ws2_32.dll secur32.dll ole32.dll advapi32.dll
    combase.dll)
list(TRANSFORM MAICORD_CLIENT_DELAY_LOADS PREPEND /DELAYLOAD: OUTPUT_VARIABLE MAICORD_CLIENT_DELAY_LOAD_OPTIONS)
target_link_options(MaiCord PRIVATE ${MAICORD_CLIENT_DELAY_LOAD_OPTIONS})
target_link_libraries(MaiCord PRIVATE dloadhelper)
add_dependencies(MaiCord MaiCordAccount maicord_app_icon)

# Self-updater publisher. Environment variables override the official defaults,
# as the release workflow does; setting both empty compiles the updater disabled.
set(maicord_default_update_repository "maj113/MaiCord")
set(maicord_default_update_public_key
    "046d75f38397386d109c1b090e60fe720bf12e84ee1579e39fca81fd33bc65330129a46073d9555a6d9141ce090e114e7da3389d9c37b9fcedaf3eb9ef26301f44")
if(DEFINED ENV{MAICORD_UPDATE_REPOSITORY})
    set(maicord_default_update_repository "$ENV{MAICORD_UPDATE_REPOSITORY}")
endif()
if(DEFINED ENV{MAICORD_UPDATE_PUBLIC_KEY})
    set(maicord_default_update_public_key "$ENV{MAICORD_UPDATE_PUBLIC_KEY}")
endif()
set(MAICORD_UPDATE_REPOSITORY "${maicord_default_update_repository}" CACHE STRING
    "GitHub owner/repo whose releases publish signed MaiCord updates")
set(MAICORD_UPDATE_PUBLIC_KEY "${maicord_default_update_public_key}" CACHE STRING
    "Update publisher ECDSA P-256 public key: 130 hex digits (uncompressed SEC1 point)")
set(MAICORD_UPDATE_PREVIEW "$ENV{MAICORD_UPDATE_PREVIEW}" CACHE STRING
    "Preview number 1-998 for a vX.Y.Z-preview.N build; empty or 0 for stable")
if(NOT MAICORD_UPDATE_PREVIEW)
    set(maicord_update_preview 0)
elseif(MAICORD_UPDATE_PREVIEW MATCHES "^[1-9][0-9]?[0-9]?$" AND MAICORD_UPDATE_PREVIEW LESS 999)
    set(maicord_update_preview ${MAICORD_UPDATE_PREVIEW})
else()
    message(FATAL_ERROR "MAICORD_UPDATE_PREVIEW must be empty or 1-998.")
endif()
if(MAICORD_UPDATE_REPOSITORY OR MAICORD_UPDATE_PUBLIC_KEY)
    # The owner is a GitHub login: it also names the Pages host <owner>.github.io.
    if(NOT MAICORD_UPDATE_REPOSITORY MATCHES "^[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?/[A-Za-z0-9_.-]+$")
        message(FATAL_ERROR "MAICORD_UPDATE_REPOSITORY must be owner/repo.")
    endif()
    if(NOT MAICORD_UPDATE_PUBLIC_KEY MATCHES "^04[0-9A-Fa-f]+$")
        message(FATAL_ERROR "MAICORD_UPDATE_PUBLIC_KEY must be the 130-digit hex point printed by tools/release/generate_update_key.py.")
    endif()
    string(LENGTH "${MAICORD_UPDATE_PUBLIC_KEY}" maicord_update_key_length)
    if(NOT maicord_update_key_length EQUAL 130)
        message(FATAL_ERROR "MAICORD_UPDATE_PUBLIC_KEY must be 130 hex digits.")
    endif()
endif()
set_source_files_properties(src/app/application_update_entry.cpp PROPERTIES COMPILE_DEFINITIONS
    "MAICORD_UPDATE_REPOSITORY=\"${MAICORD_UPDATE_REPOSITORY}\";MAICORD_UPDATE_PUBLIC_KEY=\"${MAICORD_UPDATE_PUBLIC_KEY}\";MAICORD_UPDATE_PREVIEW=${maicord_update_preview};MAICORD_VERSION_MAJOR=${PROJECT_VERSION_MAJOR};MAICORD_VERSION_MINOR=${PROJECT_VERSION_MINOR};MAICORD_VERSION_PATCH=${PROJECT_VERSION_PATCH}")

# Official plugin catalog (maj113/MaiCord-plugins on GitHub Pages): the current
# and the next (offline) P-256 key. No key compiles the official catalog as
# "not configured"; custom providers still work. Environment variables
# override the defaults, as for the updater.
set(maicord_default_plugin_catalog_repository "maj113/MaiCord-plugins")
set(maicord_default_plugin_catalog_public_key "")
set(maicord_default_plugin_catalog_next_public_key "")
foreach(name REPOSITORY PUBLIC_KEY NEXT_PUBLIC_KEY)
    string(TOLOWER "${name}" lower)
    if(DEFINED ENV{MAICORD_PLUGIN_CATALOG_${name}})
        set(maicord_default_plugin_catalog_${lower} "$ENV{MAICORD_PLUGIN_CATALOG_${name}}")
    endif()
endforeach()
set(MAICORD_PLUGIN_CATALOG_REPOSITORY "${maicord_default_plugin_catalog_repository}" CACHE STRING
    "GitHub owner/repo whose Pages site publishes the signed official plugin catalog")
set(MAICORD_PLUGIN_CATALOG_PUBLIC_KEY "${maicord_default_plugin_catalog_public_key}" CACHE STRING
    "Plugin catalog ECDSA P-256 public key (130 hex digits); empty: the official catalog is not configured")
set(MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY "${maicord_default_plugin_catalog_next_public_key}" CACHE STRING
    "The next plugin catalog key, accepted alongside the current one for rotation (130 hex digits or empty)")
if(MAICORD_PLUGIN_CATALOG_PUBLIC_KEY OR MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY)
    if(NOT MAICORD_PLUGIN_CATALOG_REPOSITORY MATCHES "^[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?/[A-Za-z0-9_.-]+$")
        message(FATAL_ERROR "MAICORD_PLUGIN_CATALOG_REPOSITORY must be owner/repo.")
    endif()
    if(NOT MAICORD_PLUGIN_CATALOG_PUBLIC_KEY)
        message(FATAL_ERROR "MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY needs MAICORD_PLUGIN_CATALOG_PUBLIC_KEY.")
    endif()
    foreach(key MAICORD_PLUGIN_CATALOG_PUBLIC_KEY MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY)
        if(${key} AND NOT ${key} MATCHES "^04[0-9A-Fa-f]+$")
            message(FATAL_ERROR "${key} must be a 130-digit uncompressed point (04...).")
        endif()
        string(LENGTH "${${key}}" maicord_plugin_catalog_key_length)
        if(${key} AND NOT maicord_plugin_catalog_key_length EQUAL 130)
            message(FATAL_ERROR "${key} must be 130 hex digits.")
        endif()
    endforeach()
endif()
set_source_files_properties(src/app/application_plugin_browser_entry.cpp PROPERTIES COMPILE_DEFINITIONS
    "MAICORD_PLUGIN_CATALOG_REPOSITORY=\"${MAICORD_PLUGIN_CATALOG_REPOSITORY}\";MAICORD_PLUGIN_CATALOG_PUBLIC_KEY=\"${MAICORD_PLUGIN_CATALOG_PUBLIC_KEY}\";MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY=\"${MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY}\"")

# Runtime assets next to the executable: licenses and the bundled fonts.
add_custom_command(TARGET MaiCord POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:MaiCord>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus/COPYING"
        "$<TARGET_FILE_DIR:MaiCord>/licenses/Opus-BSD.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:MaiCord>/licenses/MaiCord-MPL-2.0.txt")
add_custom_command(TARGET MaiCord POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:MaiCord>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/Twemoji-graphics.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/Unicode-3.0.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/ICU-Thai-dictionary.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/PyThaiNLP-corpus.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/gemoji-MIT.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/emojibase-MIT.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/MaiCord-sounds.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/minimp3.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/Lucide-ISC.txt"
        "$<TARGET_FILE_DIR:MaiCord>/licenses")
file(GLOB maicord_runtime_font_files
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*.otf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/OFL-*.txt")
# maicord_copy_fonts(<target>...): targets whose run reads the bundled fonts
# next to their executable.
function(maicord_copy_fonts)
    foreach(target IN LISTS ARGN)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/fonts"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${maicord_runtime_font_files}
                "$<TARGET_FILE_DIR:${target}>/fonts"
            VERBATIM)
    endforeach()
endfunction()
maicord_copy_fonts(MaiCord)

# Cold code is compiled for size: source-level /Os lands after the contract's
# /O2 /Ob1 /GL. Parsing, crypto, codecs, text, render and per-frame UI/input
# paths stay /O2; each exclusion below is a hot byte or frame loop.
function(maicord_compact_sources)
    cmake_parse_arguments(ARG "" "" "TARGETS;SOURCES;EXCLUDE" ${ARGN})
    set(files ${ARG_SOURCES})
    foreach(target IN LISTS ARG_TARGETS)
        get_target_property(target_sources ${target} SOURCES)
        list(APPEND files ${target_sources})
    endforeach()
    list(FILTER files INCLUDE REGEX "\\.cpp$")
    foreach(pattern IN LISTS ARG_EXCLUDE)
        list(FILTER files EXCLUDE REGEX "${pattern}")
    endforeach()
    set_property(SOURCE ${files} APPEND PROPERTY COMPILE_OPTIONS "$<$<CONFIG:Release>:/Os>")
endfunction()
# Shutdown telemetry is cold and string-heavy (it is also matched by the
# application_*.cpp glob below, so it carries /Os twice).
set_property(SOURCE src/app/application_shutdown_metrics.cpp APPEND PROPERTY
    COMPILE_OPTIONS "$<$<CONFIG:Release>:/Os>")
file(GLOB MAICORD_COLD_APPLICATION_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/application_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/update_*.cpp")
maicord_compact_sources(
    SOURCES ${MAICORD_COLD_APPLICATION_SOURCES} src/app/acceptance_record.cpp
        src/app/account_media.cpp src/app/account_media_worker.cpp src/app/regional_flag_assets.cpp
        src/app/game_detection.cpp src/core/log.cpp src/ui/clipboard.cpp
        src/platform/staged_file.cpp src/platform/browser_sign_in_launcher.cpp
        src/platform/network_file.cpp src/platform/file_picker.cpp src/platform/clipboard_attachment.cpp
        src/platform/file_identity.cpp src/platform/timezone.cpp src/platform/secure_buffer.cpp
    TARGETS maicord_protocol maicord_model maicord_plugin_host maicord_plugin_modules maicord_plugin_manager
        maicord_account_host maicord_client_preferences maicord_account_paths maicord_webauthn
    EXCLUDE
        "/application_(frame_presentation|scrolling|scroll_host|surface_motion|ui_runtime|input_routing|scheduler|media_animation|account_pump|window_runtime|composer_input|composer_text_input|content_input|shell_input|keyboard_focus|video|image_viewer|tooltip|pane_resize)\\.cpp$"
        "/model/message_(rich_)?store\\.cpp$"
        "/protocol/(message_history|websocket_frame|websocket_message|http1|http_response_stream|json_write|writer|json_ids|gateway_payload|gateway_events|gateway_message_events|gateway_message_rich|gateway_reaction_events|gateway_typing_events|media_packet|dave_frame|dave_replay|voice_media_events|p256_signature)\\.cpp$")
# Cold pages, overlays and popups rebuilt on interaction (settings, server and
# channel settings, search, inbox, pins, threads, forum, quick switcher,
# forwarding, downloads, login, profile, poll creation/voters, context menus),
# their controllers, sign-in/captcha/remote auth, DAVE/MLS group setup (not
# frame crypto), voice setup, notifications and one-shot account requests.
# The message scene, composer, pickers, gateway dispatch, media and text stay /O2.
maicord_compact_sources(
    SOURCES
        src/ui/settings_scene.cpp src/ui/theme_editor.cpp src/ui/theme_style_projection.cpp
        src/ui/settings_choice.cpp src/ui/settings_schema.cpp src/ui/page_registry.cpp
        src/ui/friends_scene.cpp src/ui/friends_media.cpp
        src/ui/controls/settings_tokens.cpp src/ui/controls/switch.cpp src/ui/controls/radio.cpp
        src/ui/controls/slider.cpp src/ui/controls/input.cpp src/ui/controls/swatch.cpp
        src/ui/controls/navigation.cpp src/ui/controls/text_metrics.cpp
        src/ui/controls/permission_grid.cpp src/ui/controls/admin_list.cpp
        src/ui/account_menu.cpp src/ui/search_scene.cpp src/ui/search_query.cpp
        src/ui/search_filter_context.cpp src/ui/pinned_messages_scene.cpp src/ui/inbox_scene.cpp
        src/ui/thread_browser_scene.cpp src/ui/forum_channel_scene.cpp src/ui/forum_post_scene.cpp src/ui/quick_switcher_scene.cpp
        src/ui/message_forward_scene.cpp src/ui/download_scene.cpp src/ui/attachment_draft_list.cpp
        src/ui/shortcuts_scene.cpp src/app/application_keybinds_settings.cpp
        src/ui/login_scene.cpp src/ui/login_presentation.cpp src/ui/login_text_presentation.cpp
        src/ui/login_view.cpp src/ui/qr_code.cpp src/ui/profile_scene.cpp src/ui/profile_media.cpp
        src/ui/profile_dialog_scene.cpp
        src/ui/poll_controls.cpp src/ui/poll_voters.cpp
        src/ui/reaction_viewer.cpp src/ui/reaction_viewer_presentation.cpp
        src/ui/message_context_menu.cpp src/ui/attachment_menu.cpp src/ui/received_attachment_menu.cpp
        src/ui/text_edit_menu.cpp
        src/ui/channel_context_policy.cpp src/ui/guild_context_policy.cpp
        src/ui/member_context_policy.cpp src/ui/private_channel_title.cpp
        src/app/quick_switcher_controller.cpp src/app/message_forward_controller.cpp
        src/app/pinned_message_controller.cpp src/app/poll_controller.cpp src/app/poll_draft.cpp
        src/app/channel_overview_controller.cpp src/app/search_recent.cpp src/app/link_trust.cpp
        src/app/received_attachment_open.cpp src/app/account_credential_forget.cpp
        src/app/login_mfa_policy.cpp src/app/navigation_shortcuts.cpp src/app/login_continuations.cpp
        src/app/settings_catalog.cpp src/app/voice_settings_binding.cpp
        src/app/private_channel_actions.cpp src/app/private_conversation.cpp
        src/app/message_draft_persistence.cpp src/app/message_attachment_drafts.cpp
        src/app/bulk_read_ack_controller.cpp src/app/reaction_viewer_controller.cpp
        src/app/reaction_viewer_storage.cpp src/app/command_autocomplete.cpp
        src/platform/desktop_notifications.cpp src/platform/notification_identity.cpp
        src/platform/taskbar_attention.cpp src/platform/clipboard_image.cpp
        src/app/user_auth_coordinator.cpp src/app/user_account_coordinator.cpp
        src/app/user_client_configuration.cpp src/app/remote_auth_client.cpp
        src/app/remote_auth_handshake.cpp src/app/remote_auth_ticket_exchange.cpp
        src/platform/remote_auth_crypto.cpp src/app/captcha_bridge.cpp src/app/captcha_challenge.cpp
        src/app/account_action_captcha.cpp src/app/browser_sign_in_receiver.cpp src/app/browser_login.cpp
        src/app/account_session_store.cpp src/app/account_profile.cpp
        src/app/account_profile_promotion.cpp src/app/account_logout.cpp
        src/app/account_guild_admin.cpp src/app/account_forum_post.cpp src/app/account_gif_favorites.cpp
        src/app/account_notification_settings.cpp src/app/account_thread_creation.cpp
        src/app/account_thread_actions.cpp src/app/account_thread_browser.cpp
        src/app/account_relationships.cpp src/app/account_poll_create.cpp
        src/app/account_pinned_messages.cpp src/app/account_channel_overview.cpp
        src/app/account_private_channel.cpp src/app/account_message_search.cpp
        src/app/account_guild_invite.cpp src/app/account_settings_rest.cpp
        src/app/account_media_catalog.cpp src/app/account_message_poll.cpp src/app/account_self_status.cpp
        src/app/hpke_derivation.cpp src/app/hpke_context.cpp src/app/mls_hpke.cpp
        src/app/mls_key_schedule.cpp src/app/mls_key_package.cpp src/app/mls_signature.cpp
        src/app/mls_exporter.cpp src/app/dave_key_package.cpp src/app/dave_welcome.cpp
        src/app/dave_receive_epoch.cpp src/app/voice_connection_setup.cpp src/app/voice_media_setup.cpp
        src/app/voice_udp_discovery.cpp
        # Rebuilt on change, not per frame: UI Automation trees and provider,
        # call/stage/Go Live card layout (the video presenter stays /O2),
        # sign-in secure text input, theme import/apply, keybind registry,
        # notification sounds, stage/stream/voice signaling and guild assets.
        src/platform/uia_provider.cpp src/ui/accessibility.cpp src/ui/accessibility_scenes.cpp
        src/ui/call_scene.cpp src/ui/call_stage.cpp src/ui/stream_card_scene.cpp
        src/ui/secure_text_store.cpp src/ui/secure_edit.cpp src/ui/secure_edit_history.cpp
        src/ui/secure_edit_target.cpp src/ui/theme_styles.cpp src/app/shortcut_registry.cpp
        src/platform/notification_audio.cpp src/app/stream_preview_cache.cpp src/app/stream_watch_view.cpp
        src/app/account_stage.cpp src/app/gateway_stage_dispatch.cpp src/app/gateway_stream_control.cpp
        src/app/gateway_stream_session.cpp src/app/gateway_voice_control.cpp
        src/app/account_guild_assets.cpp src/ui/text_form.cpp src/media/regional_flag_svg.cpp
        # Opened on a click: context menus and popups, the user menu, the
        # account switcher and the server profile card; the staged-attachment
        # strip; the user's own reaction requests; shell/tray registration;
        # the account module's API wrappers (its pump only forwards), the REST
        # request queue, the TLS handshake, soundboard and voice-message
        # requests and network-bound file transfers; H.264/H.265 parameter
        # sets and avcC, parsed once per stream or keyframe (slices and pixel
        # paths stay /O2).
        src/ui/popup_scene.cpp src/ui/popup_presentation.cpp src/ui/user_context_menu.cpp
        src/ui/account_switcher.cpp src/ui/guild_card_scene.cpp src/ui/composer_attachment_strip.cpp
        src/app/message_reaction_controller.cpp
        src/platform/desktop_integration.cpp src/platform/system_tray.cpp src/platform/tls_client.cpp
        src/app/account_module.cpp src/app/rest_client.cpp
        src/app/account_soundboard.cpp src/app/account_voice_message.cpp
        src/app/https_file_upload.cpp src/app/https_file_download.cpp
        src/media/h264_parameters.cpp src/media/hevc_parameters.cpp src/media/h264_configuration.cpp)

# MaiCord's application sources share a precompiled header (src/app/
# application_pch.hpp: <Windows.h> and application_state.hpp), most of each
# source's front-end time. A /Yu object also leaves the header's inline code
# to the PCH object, which shrinks the LTCG link input. One PCH per
# optimization mode, since cold sources carry /Os and MSVC rejects a PCH built
# with other options. A source qualifies when it reaches application_state.hpp
# (directly or through another src/app header) before any #define, #undef or
# #pragma of its own and has no definitions of its own, so the PCH holds only
# declarations it already includes. The options apply only to targets with
# MAICORD_APPLICATION_PCH, never to test targets compiling the same sources.
function(maicord_precompile_application_sources target)
    set(header "${CMAKE_CURRENT_SOURCE_DIR}/src/app/application_pch.hpp")
    file(GLOB headers "${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.hpp")
    set(app_headers application_state.hpp)
    set(pending)
    foreach(path IN LISTS headers)
        cmake_path(GET path FILENAME name)
        file(STRINGS "${path}" lines REGEX "^[ \t]*#[ \t]*include[ \t]*\"")
        set(includes_${name})
        foreach(line IN LISTS lines)
            if(line MATCHES "\"([^\"]+)\"")
                cmake_path(GET CMAKE_MATCH_1 FILENAME included)
                list(APPEND includes_${name} ${included})
            endif()
        endforeach()
        list(APPEND pending ${name})
    endforeach()
    set(changed TRUE)
    while(changed)
        set(changed FALSE)
        foreach(name IN LISTS pending)
            foreach(included IN LISTS includes_${name})
                if(included IN_LIST app_headers)
                    list(APPEND app_headers ${name})
                    list(REMOVE_ITEM pending ${name})
                    set(changed TRUE)
                    break()
                endif()
            endforeach()
        endforeach()
    endwhile()
    set(directory "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target}.dir")
    set(enabled "$<BOOL:$<TARGET_PROPERTY:MAICORD_APPLICATION_PCH>>")
    set(modes)
    get_target_property(sources ${target} SOURCES)
    foreach(source IN LISTS sources)
        if(source MATCHES "^\$<" OR NOT source MATCHES "\.cpp$")
            continue()
        endif()
        set(path "${source}")
        cmake_path(ABSOLUTE_PATH path BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
        get_source_file_property(definitions "${path}" COMPILE_DEFINITIONS)
        if(definitions)
            continue()
        endif()
        set(app FALSE)
        file(STRINGS "${path}" lines REGEX "^[ \t]*#[ \t]*(include|define|undef|pragma)")
        foreach(line IN LISTS lines)
            if(line MATCHES "^[ \t]*#[ \t]*include[ \t]*\"([^\"]+)\"")
                cmake_path(GET CMAKE_MATCH_1 FILENAME included)
                if(included IN_LIST app_headers)
                    set(app TRUE)
                    break()
                endif()
            elseif(NOT line MATCHES "^[ \t]*#[ \t]*(include|pragma[ \t]+once)")
                break()
            endif()
        endforeach()
        if(NOT app)
            continue()
        endif()
        get_source_file_property(options "${path}" COMPILE_OPTIONS)
        set(mode O2)
        if(options MATCHES "/Os")
            set(mode Os)
        endif()
        list(APPEND modes ${mode})
        set(pch "${directory}/application_pch_${mode}.pch")
        set_property(SOURCE "${path}" APPEND PROPERTY COMPILE_OPTIONS
            "$<${enabled}:/FI${header}>" "$<${enabled}:/Yu${header}>" "$<${enabled}:/Fp${pch}>")
        # /showIncludes omits headers read from a PCH, so name the PCH itself
        # (OBJECT_DEPENDS takes no generator expressions; for other targets
        # compiling the source it is only an extra, already built, input).
        set_property(SOURCE "${path}" APPEND PROPERTY OBJECT_DEPENDS "${pch}")
    endforeach()
    list(REMOVE_DUPLICATES modes)
    foreach(mode IN LISTS modes)
        set(stub "${directory}/application_pch_${mode}.cpp")
        set(pch "${directory}/application_pch_${mode}.pch")
        file(CONFIGURE OUTPUT "${stub}" CONTENT "#include \"${header}\"\n")
        set(options "/Yc${header}" "/Fp${pch}")
        if(mode STREQUAL "Os")
            # Same position as the cold sources' own per-source option.
            list(PREPEND options "$<$<CONFIG:Release>:/Os>")
        endif()
        set_source_files_properties("${stub}" PROPERTIES COMPILE_OPTIONS "${options}" OBJECT_OUTPUTS "${pch}")
        # The PCH object holds the header's code; every image using the PCH links it.
        target_sources(${target} PRIVATE "${stub}")
    endforeach()
    set_target_properties(${target} PROPERTIES MAICORD_APPLICATION_PCH ON)
endfunction()
maicord_precompile_application_sources(MaiCord)
