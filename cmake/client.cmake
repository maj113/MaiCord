# The shipping client: MaiCord.exe with the account module linked in, its
# runtime assets and the cold-code size policy.

option(MAICORD_STARTUP_PROBE "Measure isolated production-path startup" OFF)

# The account module, linked into MaiCord.exe (and through the account host
# into every test that drives it).
maicord_library(maicord_account_module
    src/app/account_message_poll.cpp src/app/account_guild_invite.cpp src/app/account_settings_rest.cpp
    src/app/account_module.cpp src/app/account_message_search.cpp
    src/app/account_media_catalog.cpp src/app/account_guild_assets.cpp src/app/guild_image.cpp
    src/app/profile_image.cpp src/app/account_profile_edit.cpp
    LIBS maicord_core maicord_platform maicord_protocol
    maicord_app_core maicord_model maicord_account_media maicord_media_core
    maicord_media_codec maicord_cdn_media
    kernel32 ${maicord_tls_libs} maicord_voice_encoder)

maicord_program(MaiCord WIN32 ENTRY maicord_entry
    SOURCES src/app/application_poll.cpp config/maicord.manifest config/maicord.rc
    "${MAICORD_NOTIFICATION_SOUND_RESOURCE}" src/app/application_state.cpp src/app/application_settings.cpp
    src/app/settings_catalog.cpp src/app/application_user_settings.cpp src/app/application_account_settings.cpp
    src/app/application_theme_editor.cpp src/app/application_custom_theme.cpp
    src/app/application_theme_publication.cpp src/app/application_friends.cpp
    src/app/application_friends_search.cpp src/app/application_friends_storage.cpp
    src/app/application_plugins.cpp src/app/application_plugins_sdk.cpp src/app/application_plugins_read.cpp
    src/app/application_plugins_actions.cpp src/app/application_plugins_surfaces.cpp
    src/app/application_plugins_render.cpp src/app/application_plugins_fx.cpp
    src/app/application_plugins_compose.cpp src/app/application_plugin_menus.cpp
    src/app/application_window.cpp src/app/application_window_runtime.cpp src/app/application_input_routing.cpp
    src/app/application_caption.cpp src/app/app_icon.cpp
    src/app/application_keyboard_focus.cpp src/app/application_account_module.cpp
    src/app/application_account_logout.cpp
    src/app/application_account_pump.cpp src/app/application_account_presentation.cpp
    src/app/application_account_reconnect.cpp
    src/app/application_thread_starter.cpp src/app/application_channel_session.cpp
    src/app/application_channel_snapshots.cpp src/app/application_commands.cpp src/app/application_builtin_commands.cpp
    src/app/application_composer.cpp
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
    src/app/application_emoji_tooltip.cpp src/app/application_expression_card.cpp
    src/app/application_interaction_card.cpp
    src/app/application_toast.cpp
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
    src/app/application_profile.cpp src/app/application_dm_profile.cpp src/app/application_member_moderation.cpp
    src/app/application_message_reaction.cpp src/app/application_message_send.cpp
    src/app/application_mention_confirmation.cpp src/app/application_message_history.cpp
    src/app/application_read_state.cpp src/app/application_scoped_read.cpp
    src/app/application_scoped_read_menu.cpp src/app/application_notifications.cpp
    src/app/application_notification_controls.cpp src/app/application_custom_status.cpp
    src/app/application_activity.cpp src/app/game_detection.cpp src/app/application_inbox.cpp
    src/app/application_message_interaction.cpp src/app/application_user_actions.cpp
    src/app/application_report.cpp src/app/application_scheduler.cpp src/app/application_shutdown_metrics.cpp
    src/app/application_scrolling.cpp src/app/application_scroll_host.cpp src/app/application_shell_input.cpp
    src/app/application_pane_resize.cpp src/app/main.cpp $<TARGET_OBJECTS:maicord_runtime>
    src/app/application_accessibility.cpp src/app/application_updates.cpp src/app/application_update_entry.cpp
    src/app/application_plugin_browser.cpp src/app/application_plugin_browser_worker.cpp
    src/app/application_plugin_browser_entry.cpp src/app/application_plugin_browser_projection.cpp
    # Voice calls: soundboard, voice messages, preferences, voice card, mute/deafen,
    # the voice user menu, the call view, stage channels and Go Live.
    src/app/application_soundboard.cpp src/app/application_voice_message.cpp src/app/application_voice.cpp
    src/app/application_voice_settings.cpp src/app/voice_settings_binding.cpp src/app/application_call.cpp
    src/app/application_stage.cpp src/app/application_voice_channels.cpp src/app/application_message_components.cpp
    src/app/application_channel_memory.cpp src/app/application_startup_history.cpp
    src/app/application_startup_shell.cpp
    src/app/application_invite_cards.cpp src/app/application_stream.cpp
    LIBS maicord_compile_contract
    maicord_core maicord_text maicord_ui maicord_model maicord_client_preferences
    maicord_notification_sound_assets maicord_plugin_host maicord_plugin_modules maicord_plugin_manager maicord_ui_input
    maicord_message_runtime maicord_message_media maicord_account_paths maicord_account_host
    maicord_render_core maicord_atlas_gpu maicord_renderer maicord_media_core maicord_webauthn
    kernel32 user32 shell32 d3d11 dxgi dcomp maicord_uia maicord_app_core ws2_32 maicord_sspicli bcrypt
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}"
    PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
if(MAICORD_STARTUP_PROBE)
    set_property(TARGET MaiCord PROPERTY MAICORD_STARTUP_PROBE ON)
    # Only these sources use the probe. Restoring a normal client after the
    # measurement must not recompile unrelated application sources.
    set_property(SOURCE src/app/main.cpp src/app/application_window.cpp
        src/app/application_frame_presentation.cpp src/app/application_shutdown_metrics.cpp
        APPEND PROPERTY COMPILE_DEFINITIONS
        "$<$<BOOL:$<TARGET_PROPERTY:MAICORD_STARTUP_PROBE>>:MAICORD_STARTUP_PROBE=1>")
endif()
option(MAICORD_SIZE_REPORT "Report shipping linker-map code extents" OFF)
if(MAICORD_SIZE_REPORT)
    target_link_options(MaiCord PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/MaiCord.map")
endif()
# Startup: DLLs the device prestart does not need map on first use instead of
# before the entry point, which the prestart waits for. The renderer maps
# dcomp, shell32, ole32 and advapi32 (which GPU drivers import) on the main
# thread while the worker creates the device; the rest load when first used.
# maicord_core's __delayLoadHelper2 (src/core/delay_load.cpp) resolves them
# through kernel32's ResolveDelayLoadedAPI and fails fast when one is missing.
# tools/audit_pe.ps1 (-DelayLoad) and maicord_client_imports check the list.
set(MAICORD_CLIENT_DELAY_LOADS dcomp.dll shell32.dll ws2_32.dll sspicli.dll ole32.dll advapi32.dll
    combase.dll d2d1.dll bcrypt.dll crypt32.dll)
list(TRANSFORM MAICORD_CLIENT_DELAY_LOADS PREPEND /DELAYLOAD: OUTPUT_VARIABLE MAICORD_CLIENT_DELAY_LOAD_OPTIONS)
target_link_options(MaiCord PRIVATE ${MAICORD_CLIENT_DELAY_LOAD_OPTIONS})
# LTO code generation in parallel partitions: the fork's splitter gives each
# partition foldable copies of the constants it reads. Three partitions link in
# 39 s instead of 57 s at the 1-partition size; other counts land 512 B (one
# file-alignment unit) larger on this tree. Re-check the count when it drifts.
target_link_options(MaiCord PRIVATE /opt:lldltopartitions=3)
add_dependencies(MaiCord maicord_sys_conformance)

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
# The installed version. CI and release builds pass it (MAICORD_UPDATE_VERSION
# and MAICORD_UPDATE_COMMIT). Left empty, it is this checkout's build identity
# (tools/release/maicord_release.py identity), numbered exactly as CI numbers
# master builds: X.Y.Z-dev.N for N commits after the last stable tag, X.Y.Z on
# the tag. A local build is then never older than the published build of its
# commit, so the updater never offers it an older one. tools/build.ps1
# configures on every run, so each build is numbered for the current HEAD.
set(MAICORD_UPDATE_VERSION "$ENV{MAICORD_UPDATE_VERSION}" CACHE STRING
    "Installed version X.Y.Z or X.Y.Z-dev.N (N 1-998); empty for this checkout's git build identity")
set(MAICORD_UPDATE_COMMIT "$ENV{MAICORD_UPDATE_COMMIT}" CACHE STRING
    "Source commit (40 lowercase hex digits) shown with the version; empty for HEAD with a git build identity")
set(maicord_update_version "${MAICORD_UPDATE_VERSION}")
set(maicord_update_commit "${MAICORD_UPDATE_COMMIT}")
set(maicord_update_repository "${MAICORD_UPDATE_REPOSITORY}")
set(maicord_update_public_key "${MAICORD_UPDATE_PUBLIC_KEY}")
if(NOT maicord_update_version)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=GITHUB_OUTPUT
            "${Python3_EXECUTABLE}" -B tools/release/maicord_release.py identity --repository .
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE maicord_identity_result
        OUTPUT_VARIABLE maicord_identity
        ERROR_VARIABLE maicord_identity_error
        ERROR_STRIP_TRAILING_WHITESPACE)
    string(REGEX MATCH "version=([^\r\n]+)" maicord_identity_version "${maicord_identity}")
    set(maicord_identity_version "${CMAKE_MATCH_1}")
    string(REGEX MATCH "commit=([0-9a-f]+)" maicord_identity_commit "${maicord_identity}")
    set(maicord_identity_commit "${CMAKE_MATCH_1}")
    if(maicord_identity_result EQUAL 0 AND maicord_identity_version)
        set(maicord_update_version "${maicord_identity_version}")
        if(NOT maicord_update_commit)
            set(maicord_update_commit "${maicord_identity_commit}")
        endif()
        message(STATUS "MaiCord build identity: ${maicord_update_version} (${maicord_update_commit})")
    else()
        # Without its number a build can't tell whether a release is newer.
        message(WARNING "No git build identity (${maicord_identity_error}). MaiCord builds as the project version "
            "${PROJECT_VERSION} with the updater disabled; set MAICORD_UPDATE_VERSION to number it.")
        set(maicord_update_repository "")
        set(maicord_update_public_key "")
    endif()
endif()
set(maicord_update_dev 0)
if(NOT maicord_update_version)
    set(maicord_update_major ${PROJECT_VERSION_MAJOR})
    set(maicord_update_minor ${PROJECT_VERSION_MINOR})
    set(maicord_update_patch ${PROJECT_VERSION_PATCH})
elseif(maicord_update_version MATCHES "^(0|[1-9][0-9]?[0-9]?)\\.(0|[1-9][0-9]?[0-9]?)\\.(0|[1-9][0-9]?[0-9]?)(-dev\\.([1-9][0-9]?[0-9]?))?$"
        AND NOT CMAKE_MATCH_5 STREQUAL "999")
    set(maicord_update_major ${CMAKE_MATCH_1})
    set(maicord_update_minor ${CMAKE_MATCH_2})
    set(maicord_update_patch ${CMAKE_MATCH_3})
    if(CMAKE_MATCH_5)
        set(maicord_update_dev ${CMAKE_MATCH_5})
    endif()
else()
    message(FATAL_ERROR "MAICORD_UPDATE_VERSION must be empty, X.Y.Z or X.Y.Z-dev.N (components 0-999, N 1-998).")
endif()
string(LENGTH "${maicord_update_commit}" maicord_update_commit_length)
if(maicord_update_commit AND (NOT maicord_update_commit_length EQUAL 40 OR NOT maicord_update_commit MATCHES "^[0-9a-f]+$"))
    message(FATAL_ERROR "MAICORD_UPDATE_COMMIT must be empty or 40 lowercase hex digits.")
endif()
set(maicord_owner_repository_pattern "^[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?/[A-Za-z0-9_.-]+$")
# The publisher keys are uncompressed P-256 points: 130 hex digits starting 04.
function(maicord_check_p256_point variable)
    string(LENGTH "${${variable}}" length)
    if(NOT length EQUAL 130 OR NOT ${variable} MATCHES "^04[0-9A-Fa-f]+$")
        message(FATAL_ERROR "${variable} must be 130 hex digits: an uncompressed point (04...).")
    endif()
endfunction()
if(MAICORD_UPDATE_REPOSITORY OR MAICORD_UPDATE_PUBLIC_KEY)
    # The owner is a GitHub login: it also names the Pages host <owner>.github.io.
    if(NOT MAICORD_UPDATE_REPOSITORY MATCHES "${maicord_owner_repository_pattern}")
        message(FATAL_ERROR "MAICORD_UPDATE_REPOSITORY must be owner/repo.")
    endif()
    maicord_check_p256_point(MAICORD_UPDATE_PUBLIC_KEY)
endif()
set_source_files_properties(src/app/application_update_entry.cpp PROPERTIES COMPILE_DEFINITIONS
    "MAICORD_UPDATE_REPOSITORY=\"${maicord_update_repository}\";MAICORD_UPDATE_PUBLIC_KEY=\"${maicord_update_public_key}\";MAICORD_UPDATE_DEV=${maicord_update_dev};MAICORD_UPDATE_COMMIT=\"${maicord_update_commit}\";MAICORD_VERSION_MAJOR=${maicord_update_major};MAICORD_VERSION_MINOR=${maicord_update_minor};MAICORD_VERSION_PATCH=${maicord_update_patch}")

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
    if(NOT MAICORD_PLUGIN_CATALOG_REPOSITORY MATCHES "${maicord_owner_repository_pattern}")
        message(FATAL_ERROR "MAICORD_PLUGIN_CATALOG_REPOSITORY must be owner/repo.")
    endif()
    if(NOT MAICORD_PLUGIN_CATALOG_PUBLIC_KEY)
        message(FATAL_ERROR "MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY needs MAICORD_PLUGIN_CATALOG_PUBLIC_KEY.")
    endif()
    foreach(key MAICORD_PLUGIN_CATALOG_PUBLIC_KEY MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY)
        if(${key})
            maicord_check_p256_point(${key})
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
        "$<TARGET_FILE_DIR:MaiCord>/licenses/MaiCord-MPL-2.0.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/Twemoji-graphics.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/Unicode-3.0.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/ICU-Thai-dictionary.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/PyThaiNLP-corpus.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/gemoji-MIT.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/emojibase-MIT.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/MaiCord-sounds.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/Lucide-ISC.txt"
        "$<TARGET_FILE_DIR:MaiCord>/licenses")
file(GLOB maicord_runtime_font_files
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*.otf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/OFL-*.txt")
# One rule copies the bundled fonts next to every executable (they share the
# runtime output directory); per-target POST_BUILD copies raced each other in
# parallel builds.
set(maicord_runtime_font_outputs "")
foreach(font IN LISTS maicord_runtime_font_files)
    get_filename_component(name "${font}" NAME)
    list(APPEND maicord_runtime_font_outputs "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts/${name}")
endforeach()
add_custom_command(OUTPUT ${maicord_runtime_font_outputs}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${maicord_runtime_font_files}
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts"
    DEPENDS ${maicord_runtime_font_files}
    VERBATIM)
add_custom_target(maicord_runtime_fonts DEPENDS ${maicord_runtime_font_outputs})
# maicord_copy_fonts(<target>...): targets whose run reads the bundled fonts
# next to their executable.
function(maicord_copy_fonts)
    foreach(target IN LISTS ARGN)
        add_dependencies(${target} maicord_runtime_fonts)
    endforeach()
endfunction()
maicord_copy_fonts(MaiCord)

# Cold code is compiled for size: source-level -Oz lands after the contract's
# /O2 /Ob1 -flto. Parsing, crypto, codecs, compression and text shaping stay
# /O2; each exclusion below is a hot byte or frame loop.
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
    list(REMOVE_DUPLICATES files)
    # A source listed twice keeps one size option.
    foreach(file IN LISTS files)
        get_source_file_property(options "${file}" COMPILE_OPTIONS)
        if(NOT options MATCHES "${MAICORD_SIZE_OPTION}")
            set_property(SOURCE "${file}" APPEND PROPERTY COMPILE_OPTIONS ${MAICORD_SIZE_OPTION})
        endif()
    endforeach()
endfunction()
file(GLOB MAICORD_COLD_APPLICATION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/application_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/update_*.cpp")
maicord_compact_sources(
    SOURCES ${MAICORD_COLD_APPLICATION_SOURCES} src/app/acceptance_record.cpp
        src/app/account_media.cpp src/app/account_media_worker.cpp src/app/regional_flag_assets.cpp
        src/app/game_detection.cpp src/app/file_persistence.cpp src/core/log.cpp src/ui/clipboard.cpp
        src/platform/staged_file.cpp src/platform/browser_sign_in_launcher.cpp
        src/platform/network_file.cpp src/platform/file_picker.cpp src/platform/clipboard_attachment.cpp
        src/platform/file_identity.cpp src/platform/timezone.cpp src/platform/secure_buffer.cpp
    TARGETS maicord_protocol maicord_model maicord_plugin_host maicord_plugin_modules maicord_plugin_manager
        maicord_account_host maicord_client_preferences maicord_account_paths maicord_webauthn
    # The user store resolves every member name and presence update: member-list
    # model time is 6% lower with it at /O2, for 2 KiB. The guild member store
    # and member list compile for size through minsize attribute regions in
    # their sources, which leave their member-list update and rebuild paths /O2.
    EXCLUDE
        "/application_(frame_presentation|scheduler|video)\\.cpp$"
        "/model/(message_(rich_)?store|user_store|guild_member_store|member_list)\\.cpp$"
        "/protocol/(message_history|websocket_frame|websocket_message|http1|http_response_stream|json_write|writer|json_ids|gateway_payload|gateway_events|gateway_message_events|gateway_message_rich|gateway_reaction_events|gateway_typing_events|media_packet|dave_frame|dave_replay|voice_media_events|p256_signature|etf)\\.cpp$")
# UI scenes, presentation, renderer and the message runtime: the offline perf
# probe measured equal or lower main-thread CPU in every phase at /Os (smaller
# code and no chained unwind records). The parsers and tokenizers they run per
# message and glyph outlines stay /O2, as does the GPU atlas code.
# Elsewhere: cold controllers, sign-in/captcha/remote auth, DAVE/MLS group
# setup (not frame crypto), voice setup, notifications and one-shot account
# requests. Gateway dispatch, media decode and text shaping stay /O2.
maicord_compact_sources(
    TARGETS maicord_ui maicord_renderer maicord_message_runtime maicord_message_media
    EXCLUDE "/(message_markdown|code_highlight|spell_tokenize|ansi_text|text_measure_cache)\\.cpp$"
        "/(native_glyph_outlines|colr_paint)\\.cpp$"
    SOURCES
        src/app/channel_memory.cpp
        src/app/quick_switcher_controller.cpp src/app/message_forward_controller.cpp
        src/app/pinned_message_controller.cpp src/app/poll_controller.cpp src/app/poll_draft.cpp
        src/app/channel_overview_controller.cpp src/app/search_recent.cpp src/app/link_trust.cpp
        src/app/received_attachment_open.cpp src/app/account_credential_forget.cpp
        src/app/login_mfa_policy.cpp src/app/navigation_shortcuts.cpp src/app/login_continuations.cpp
        src/app/settings_catalog.cpp src/app/voice_settings_binding.cpp
        src/platform/desktop_notifications.cpp src/platform/notification_identity.cpp
        src/platform/taskbar_attention.cpp src/platform/clipboard_image.cpp
        src/app/user_auth_coordinator.cpp src/app/user_account_coordinator.cpp
        src/app/user_client_configuration.cpp src/app/remote_auth_client.cpp
        src/app/remote_auth_handshake.cpp src/app/remote_auth_ticket_exchange.cpp
        src/platform/remote_auth_crypto.cpp src/app/captcha_bridge.cpp src/app/captcha_challenge.cpp
        src/app/account_action_captcha.cpp src/app/browser_sign_in_receiver.cpp src/app/loopback_server.cpp src/app/browser_login.cpp
        src/app/account_session_store.cpp src/app/account_profile.cpp
        src/app/account_profile_promotion.cpp src/app/account_logout.cpp
        src/app/account_guild_admin.cpp src/app/account_forum_post.cpp src/app/account_gif_favorites.cpp
        src/app/account_notification_settings.cpp src/app/account_thread_creation.cpp
        src/app/account_thread_actions.cpp src/app/account_thread_browser.cpp
        src/app/account_relationships.cpp src/app/account_poll_create.cpp
        src/app/account_pinned_messages.cpp src/app/account_channel_overview.cpp
        src/app/account_private_channel.cpp src/app/account_message_search.cpp
        src/app/account_guild_invite.cpp src/app/account_settings_rest.cpp
        src/app/account_media_catalog.cpp src/app/account_message_poll.cpp
        src/app/hpke_derivation.cpp src/app/hpke_context.cpp src/app/mls_hpke.cpp
        src/app/mls_key_schedule.cpp src/app/mls_key_package.cpp src/app/mls_signature.cpp
        src/app/mls_exporter.cpp src/app/dave_key_package.cpp src/app/dave_welcome.cpp
        src/app/dave_receive_epoch.cpp src/app/voice_connection_setup.cpp src/app/voice_media_setup.cpp
        src/app/voice_udp_discovery.cpp
        # Rebuilt on change, not per frame: the UI Automation provider,
        # sign-in secure text input, keybind registry, notification sounds,
        # stage/stream/voice signaling and guild assets.
        src/platform/uia_provider.cpp
        src/ui/secure_text_store.cpp src/ui/secure_edit.cpp src/ui/secure_edit_history.cpp
        src/ui/secure_edit_target.cpp src/app/shortcut_registry.cpp
        src/platform/notification_audio.cpp src/app/stream_preview_cache.cpp src/app/stream_watch_view.cpp
        src/app/account_stage.cpp src/app/gateway_stage_dispatch.cpp src/app/gateway_stream_control.cpp
        src/app/gateway_stream_session.cpp src/app/gateway_voice_control.cpp
        src/app/account_guild_assets.cpp src/media/regional_flag_svg.cpp
        # Opened on a click: the user's own reaction requests; shell/tray registration;
        # the account module's API wrappers (its pump only forwards), the REST
        # request queue, the TLS handshake, soundboard and voice-message
        # requests and network-bound file transfers; H.264/H.265 parameter
        # sets and avcC, parsed once per stream or keyframe (slices and pixel
        # paths stay /O2).
        src/app/message_reaction_controller.cpp
        src/platform/desktop_integration.cpp src/platform/system_tray.cpp src/platform/tls_client.cpp
        src/app/account_module.cpp src/app/rest_client.cpp
        src/app/account_soundboard.cpp src/app/account_voice_message.cpp
        src/app/https_file_upload.cpp src/app/https_file_download.cpp
        src/media/h264_parameters.cpp src/media/hevc_parameters.cpp src/media/h264_configuration.cpp
        # Once per embed, stream segment, CDN request, voice connection or READY:
        # embed stream resolution, segment and CDN request control, voice
        # connection setup and READY projection (their byte paths stay /O2).
        src/app/embed_resolver.cpp src/app/segmented_video_fetch.cpp src/app/cdn_client.cpp
        src/app/voice_connection.cpp src/app/gateway_ready_projection.cpp
        # Run once per font file or face: name/family/property resolution,
        # catalog records, seac charsets and face preparation.
        src/text/font_names.cpp src/text/font_family.cpp src/text/font_candidates.cpp
        src/text/font_catalog.cpp src/text/font_prepare.cpp src/text/cff_charset.cpp
        src/ui/native_text_catalog.cpp
        # Signaling, setup and per-surface control around byte paths that stay
        # /O2: OAuth, member-list channel binding, guild details, voice socket
        # and participants, progressive video requests, spell checking, P-256
        # and HKDF, socket setup, profile and guild image preparation, posters,
        # Lottie and COLR program building, the encoded-media cache and
        # upload-only PNG encoding (perf probe phases equal or lower). Gateway
        # dispatch (3-9% slower) and the video presenter, session and decoder
        # control (call phases ~3% slower) measured worse at /Os and stay /O2.
        src/app/oauth_sign_in.cpp src/app/member_list_channel_binding.cpp src/app/gateway_guild_details.cpp
        src/app/voice_socket.cpp src/app/voice_participants.cpp src/app/progressive_video_fetch.cpp
        src/media/png_encode.cpp
        src/platform/spell_checker.cpp src/platform/p256.cpp src/platform/hkdf_sha256.cpp
        src/platform/network.cpp src/platform/network_worker.cpp
        src/app/guild_image.cpp src/app/profile_image.cpp src/app/account_profile_edit.cpp
        src/app/local_video_poster.cpp
        src/render/lottie_program.cpp src/render/colr_source.cpp src/media/encoded_cache.cpp)

# maicord_application_state_headers(<variable> <glob>...): the file names of
# application_state.hpp and of the globbed headers that include it, directly
# or through another of them.
function(maicord_application_state_headers variable)
    file(GLOB headers ${ARGN})
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
    set(${variable} ${app_headers} PARENT_SCOPE)
endfunction()

# maicord_reaches_application_state(<variable> <source> <header>...): whether
# <source> includes one of the <header> file names before any #define, #undef
# or #pragma of its own.
function(maicord_reaches_application_state variable source)
    set(app_headers ${ARGN})
    set(app FALSE)
    file(STRINGS "${source}" lines REGEX "^[ \t]*#[ \t]*(include|define|undef|pragma)")
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
    set(${variable} ${app} PARENT_SCOPE)
endfunction()

# Application sources share a precompiled header (src/app/
# application_pch.hpp: application_state.hpp), about half of each source's
# front-end time. A /Yu object also leaves the header's inline code
# to the PCH object, which shrinks the LTO link input. One PCH per
# optimization mode, since cold sources carry -Oz and clang rejects a PCH built
# with other options. A source qualifies when it reaches application_state.hpp
# (directly or through another src/app header) before any #define, #undef or
# #pragma of its own and has no definitions of its own, so the PCH holds only
# declarations it already includes. The options apply only to targets with
# MAICORD_APPLICATION_PCH, the directory of their own PCHs, so every target
# compiling a source uses a PCH built with its settings, or none. A source
# with the property MAICORD_NO_APPLICATION_PCH set never uses one.
# maicord_precompile_application_sources(<target> [STUBS <object library>]):
# without STUBS the PCHs are built in <target> and each qualifying source names
# them in OBJECT_DEPENDS, which has no per-target form, so a configuration that
# does not build the client (MAICORD_TEST_PREBUILT_CLIENT) does not set the
# client's PCH up at all, rather than have test objects wait on a PCH that
# nothing there uses. STUBS builds them in that object library instead (with
# <target>'s settings) and orders only <target>'s objects after them.
function(maicord_precompile_application_sources target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "STUBS" "")
    if(NOT ARG_STUBS)
        set(ARG_STUBS ${target})
    endif()
    set(header "${CMAKE_CURRENT_SOURCE_DIR}/src/app/application_pch.hpp")
    maicord_application_state_headers(app_headers "${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.hpp")
    set(directory "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${ARG_STUBS}.dir")
    set(enabled "$<BOOL:$<TARGET_PROPERTY:MAICORD_APPLICATION_PCH>>")
    set(modes)
    get_target_property(sources ${target} SOURCES)
    foreach(source IN LISTS sources)
        if(source MATCHES "^\\$<" OR NOT source MATCHES "\\.cpp$")
            continue()
        endif()
        set(path "${source}")
        cmake_path(ABSOLUTE_PATH path BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
        get_source_file_property(definitions "${path}" COMPILE_DEFINITIONS)
        if(definitions)
            continue()
        endif()
        get_source_file_property(no_pch "${path}" MAICORD_NO_APPLICATION_PCH)
        if(no_pch)
            continue()
        endif()
        maicord_reaches_application_state(app "${path}" ${app_headers})
        if(NOT app)
            continue()
        endif()
        get_source_file_property(options "${path}" COMPILE_OPTIONS)
        set(mode O2)
        if(options MATCHES "${MAICORD_SIZE_OPTION}")
            set(mode Os)
        endif()
        list(APPEND modes ${mode})
        # The options are the same for every target; the first one sets them.
        get_source_file_property(tagged "${path}" MAICORD_APPLICATION_PCH_MODE)
        if(NOT tagged)
            set(pch "$<TARGET_PROPERTY:MAICORD_APPLICATION_PCH>/application_pch_${mode}.pch")
            set_property(SOURCE "${path}" PROPERTY MAICORD_APPLICATION_PCH_MODE ${mode})
            set_property(SOURCE "${path}" APPEND PROPERTY COMPILE_OPTIONS
                "$<${enabled}:/FI${header}>" "$<${enabled}:/Yu${header}>" "$<${enabled}:/Fp${pch}>")
        endif()
        if(ARG_STUBS STREQUAL target)
            # /showIncludes omits headers read from a PCH, so name the PCH itself
            # (OBJECT_DEPENDS takes no generator expressions; for other targets
            # compiling the source it is only an extra, already built, input).
            set_property(SOURCE "${path}" APPEND PROPERTY OBJECT_DEPENDS "${directory}/application_pch_${mode}.pch")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES modes)
    set(pchs)
    foreach(mode IN LISTS modes)
        set(stub "${directory}/application_pch_${mode}.cpp")
        set(pch "${directory}/application_pch_${mode}.pch")
        file(CONFIGURE OUTPUT "${stub}" CONTENT "#include \"${header}\"\n")
        # The Os stub carries the size option first, the position of the cold sources' own option.
        set(options "/Yc${header}" "/Fp${pch}")
        if(mode STREQUAL "Os")
            list(PREPEND options ${MAICORD_SIZE_OPTION})
        endif()
        set_source_files_properties("${stub}" PROPERTIES COMPILE_OPTIONS "${options}" OBJECT_OUTPUTS "${pch}")
        # The PCH object holds the header's code; every image using the PCH links it.
        target_sources(${ARG_STUBS} PRIVATE "${stub}")
        list(APPEND pchs "${pch}")
    endforeach()
    if(NOT ARG_STUBS STREQUAL target)
        # A custom command output of the target orders all of its objects after
        # the PCHs, and only its own; it waits for nothing else the target needs.
        set(stamp "${directory}/application_pch.stamp")
        add_custom_command(OUTPUT "${stamp}" COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}" DEPENDS ${pchs}
            DEPENDS_EXPLICIT_ONLY VERBATIM)
        target_sources(${target} PRIVATE "$<TARGET_OBJECTS:${ARG_STUBS}>" "${stamp}")
    endif()
    set_target_properties(${target} PROPERTIES MAICORD_APPLICATION_PCH "${directory}")
endfunction()
if(NOT MAICORD_TEST_PREBUILT_CLIENT)
    maicord_precompile_application_sources(MaiCord)
endif()
