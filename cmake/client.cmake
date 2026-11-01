# The shipping client: MaiCord.exe with the account module linked in, its
# runtime assets and the cold-code size policy.

option(MAICORD_STARTUP_PROBE "Measure isolated production-path startup" OFF)

# The account module, linked into MaiCord.exe (and through the account host
# into every test that drives it).
maicord_library(maicord_account_module
    src/app/chat/polls/account_message_poll.cpp src/app/guild/account_guild_invite.cpp src/app/settings/account_settings_rest.cpp
    src/app/account/account_module.cpp src/app/account/account_module_login.cpp src/app/account/account_module_requests.cpp
    src/app/chat/search/account_message_search.cpp
    src/app/media/account_media_catalog.cpp src/app/guild/account_guild_assets.cpp src/app/media/guild_image.cpp
    src/app/media/profile_image.cpp src/app/account/account_profile_edit.cpp
    LIBS maicord_core maicord_platform maicord_protocol
    maicord_app_core maicord_model maicord_account_media maicord_media_core
    maicord_media_codec maicord_cdn_media
    kernel32 ${maicord_tls_libs} maicord_voice_encoder)

# The icon and notification sounds resource objects (linked into MaiCord below,
# and into the perf probe).
add_library(maicord_client_resources OBJECT config/maicord.rc "${MAICORD_NOTIFICATION_SOUND_RESOURCE}")
target_link_libraries(maicord_client_resources PRIVATE maicord_compile_contract)
target_include_directories(maicord_client_resources PRIVATE "${MAICORD_GENERATED_DIRECTORY}")

maicord_program(MaiCord WIN32 ENTRY maicord_entry
    SOURCES src/app/chat/polls/poll.cpp config/maicord.manifest
    src/app/shell/state.cpp src/app/settings/settings.cpp src/app/settings/settings_projection.cpp
    src/app/settings/settings_activation.cpp src/app/settings/settings_input.cpp
    src/app/settings/catalog.cpp src/app/settings/user_settings.cpp src/app/settings/account_settings.cpp
    src/app/settings/theme_editor.cpp src/app/settings/custom_theme.cpp
    src/app/settings/theme_publication.cpp src/app/account/friends.cpp
    src/app/account/friends_search.cpp src/app/account/friends_storage.cpp
    src/app/plugins/plugins.cpp src/app/plugins/plugins_sdk.cpp src/app/plugins/read.cpp
    src/app/plugins/actions.cpp src/app/plugins/surfaces.cpp
    src/app/plugins/render.cpp src/app/plugins/fx.cpp
    src/app/plugins/compose.cpp src/app/plugins/menus.cpp
    src/app/shell/window.cpp src/app/shell/window_runtime.cpp src/app/shell/input_routing.cpp
    src/app/shell/caption.cpp src/app/shell/app_icon.cpp
    src/app/shell/keyboard_focus.cpp src/app/account/application_account_module.cpp
    src/app/account/application_account_logout.cpp
    src/app/account/account_pump.cpp src/app/account/account_presentation.cpp
    src/app/account/application_account_reconnect.cpp
    src/app/chat/threads/starter.cpp src/app/chat/channel_session.cpp
    src/app/chat/channel_snapshots.cpp src/app/chat/composer/commands.cpp src/app/chat/composer/commands_frecency.cpp
    src/app/chat/composer/commands_picker.cpp src/app/chat/composer/commands_draft.cpp src/app/chat/composer/builtin_commands.cpp
    src/app/chat/composer/composer.cpp
    src/app/chat/messages/feedback.cpp src/app/chat/composer/reply_draft.cpp
    src/app/chat/composer/input.cpp src/app/shell/button_input.cpp
    src/app/chat/composer/edit_menu.cpp src/app/chat/composer/spellcheck.cpp
    src/app/chat/composer/attachments.cpp src/app/media/attachment_allowance.cpp
    src/app/media/upload_lane.cpp src/app/shell/content_input.cpp src/app/chat/application_link_trust.cpp
    src/app/media/image_viewer.cpp src/app/shell/frame_presentation.cpp
    src/app/chat/unread_banner.cpp src/app/chat/messages/intro.cpp
    src/app/guild/folders.cpp src/app/shell/navigation_drag.cpp
    src/app/guild/lifecycle.cpp src/app/account/profile_editor.cpp src/app/account/profile_editor_preview.cpp
    src/app/account/profile_editor_images.cpp
    src/app/guild/menu.cpp src/app/guild/community.cpp src/app/guild/community_projection.cpp
    src/app/guild/card.cpp src/app/guild/banner.cpp src/app/guild/invite.cpp
    src/app/notifications/taskbar_attention.cpp src/app/notifications/desktop.cpp
    src/app/notifications/desktop_settings.cpp src/app/shell/surface_motion.cpp
    src/app/chat/composer/text_form.cpp src/app/chat/composer/text_input.cpp
    src/app/account/login/auth.cpp src/app/account/login/auth_recovery.cpp src/app/account/login/input.cpp src/app/account/login/ui.cpp
    src/app/shell/ui_runtime.cpp src/app/guild/account_permission_storage.cpp src/app/media/media.cpp
    src/app/media/video.cpp src/app/media/video_sources.cpp src/app/media/video_input.cpp
    src/app/shell/tooltip.cpp src/app/chat/messages/reaction_tooltip.cpp
    src/app/media/emoji_tooltip.cpp src/app/media/expression_card.cpp
    src/app/chat/composer/interaction_card.cpp
    src/app/shell/toast.cpp
    src/app/account/account_menu.cpp
    src/app/account/application_account_switch.cpp src/app/chat/search/search.cpp src/app/chat/search/search_results.cpp
    src/app/chat/search/search_filters.cpp src/app/chat/search/search_actions.cpp src/app/chat/search/search_input.cpp
    src/app/chat/search/content.cpp
    src/app/chat/search/navigation.cpp src/app/chat/threads/actions.cpp
    src/app/chat/threads/browser.cpp src/app/chat/threads/creation.cpp
    src/app/chat/threads/forum_post.cpp src/app/chat/threads/forum_form.cpp
    src/app/chat/threads/forum_attachments.cpp src/app/chat/threads/forum_preview.cpp
    src/app/chat/composer/preview.cpp src/app/media/local_preview.cpp
    src/app/media/attachment_probe.cpp src/app/media/attachment_media_probe.cpp
    src/app/chat/threads/header_activation.cpp src/app/chat/threads/header.cpp
    src/app/shell/quick_switcher.cpp src/app/chat/messages/forward.cpp
    src/app/media/downloads.cpp src/app/media/downloads_input.cpp src/app/shell/shortcuts.cpp
    src/app/settings/keybinds_settings.cpp src/app/chat/messages/pinned_messages.cpp
    src/app/media/picker.cpp src/app/media/picker_projection.cpp src/app/media/picker_stickers.cpp
    src/app/media/picker_input.cpp src/app/media/gif_favorites.cpp
    src/app/media/gif_favorite_star.cpp
    src/app/settings/emoji_preferences.cpp src/app/account/login/action_captcha.cpp
    src/app/guild/channel_overview.cpp src/app/guild/channel_overview_projection.cpp src/app/settings/layer.cpp
    src/app/settings/modal.cpp
    src/app/guild/settings/settings.cpp src/app/guild/settings/settings_requests.cpp src/app/guild/settings/profile.cpp
    src/app/guild/settings/engagement.cpp src/app/guild/settings/expressions.cpp
    src/app/guild/settings/members.cpp src/app/guild/settings/roles.cpp
    src/app/guild/settings/invites.cpp src/app/guild/settings/safety.cpp
    src/app/guild/settings/audit_log.cpp src/app/guild/settings/bans.cpp
    src/app/guild/settings/boost.cpp
    src/app/chat/polls/create.cpp
    src/app/account/self_status.cpp src/app/account/login/browser_auth.cpp
    src/app/account/login/captcha_auth.cpp src/app/media/animation.cpp
    src/app/media/requests.cpp src/app/media/requests_message.cpp src/app/chat/messages/delete.cpp
    src/app/chat/application_private_conversation.cpp src/app/chat/private_channel.cpp
    src/app/chat/messages/edit.cpp src/app/chat/messages/mutation_notice.cpp
    src/app/account/profile.cpp src/app/account/dm_profile.cpp src/app/guild/member_moderation.cpp
    src/app/chat/messages/reaction.cpp src/app/chat/messages/send.cpp
    src/app/chat/composer/mention_confirmation.cpp src/app/chat/messages/history.cpp
    src/app/chat/read_state.cpp src/app/chat/scoped_read.cpp
    src/app/chat/scoped_read_menu.cpp src/app/notifications/notifications.cpp
    src/app/notifications/controls.cpp src/app/account/custom_status.cpp
    src/app/account/activity.cpp src/app/account/game_detection.cpp src/app/chat/inbox.cpp
    src/app/chat/messages/interaction.cpp src/app/chat/messages/interaction_popups.cpp src/app/account/user_actions.cpp
    src/app/chat/report.cpp src/app/shell/scheduler.cpp src/app/shell/shutdown_metrics.cpp
    src/app/shell/scrolling.cpp src/app/shell/scroll_host.cpp src/app/shell/input.cpp
    src/app/shell/pane_resize.cpp src/app/shell/main.cpp $<TARGET_OBJECTS:maicord_runtime>
    src/app/accessibility/accessibility.cpp src/app/accessibility/accessibility_describe.cpp
    src/app/accessibility/accessibility_actions.cpp src/app/update/updates.cpp src/app/update/entry.cpp
    src/app/plugins/browser.cpp src/app/plugins/browser_worker.cpp
    src/app/plugins/browser_entry.cpp src/app/plugins/browser_projection.cpp
    # Voice calls: soundboard, voice messages, preferences, voice card, mute/deafen,
    # the voice user menu, the call view, stage channels and Go Live.
    src/app/voice/soundboard.cpp src/app/voice/application_voice_message.cpp src/app/voice/voice.cpp
    src/app/voice/settings.cpp src/app/voice/settings_binding.cpp src/app/voice/call.cpp
    src/app/voice/stage.cpp src/app/voice/channels.cpp src/app/chat/messages/components.cpp
    src/app/chat/application_channel_memory.cpp src/app/shell/startup_history.cpp
    src/app/shell/application_startup_shell.cpp
    src/app/guild/invite_cards.cpp src/app/voice/stream.cpp
    LIBS maicord_compile_contract
    maicord_core maicord_text maicord_ui maicord_model maicord_client_preferences
    maicord_notification_sound_assets maicord_plugin_host maicord_plugin_modules maicord_plugin_manager
    maicord_plugin_runtime maicord_ui_input
    maicord_message_runtime maicord_message_media maicord_account_paths maicord_account_host
    maicord_render_core maicord_atlas_gpu maicord_renderer maicord_media_core maicord_webauthn
    kernel32 user32 shell32 d3d11 dxgi dcomp maicord_uia maicord_app_core ws2_32 maicord_sspicli bcrypt
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}"
    PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
# The resources are plain link inputs, not sources: a generated input of one
# source (the icon) orders every object of its target after its generator.
target_link_options(MaiCord PRIVATE "$<TARGET_OBJECTS:maicord_client_resources>")
set_property(TARGET MaiCord APPEND PROPERTY LINK_DEPENDS "$<TARGET_OBJECTS:maicord_client_resources>")
if(MAICORD_STARTUP_PROBE)
    set_property(TARGET MaiCord PROPERTY MAICORD_STARTUP_PROBE ON)
    # Only these sources use the probe. Restoring a normal client after the
    # measurement must not recompile unrelated application sources.
    set_property(SOURCE src/app/shell/main.cpp src/app/shell/window.cpp
        src/app/shell/frame_presentation.cpp src/app/shell/shutdown_metrics.cpp
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
# LTO code generation stays in one partition (lld's default). Once the machine
# outliner is on (cmake/build_contract.cmake), one partition links 28 KB smaller
# than three, in 39 s instead of 26 s; other counts are larger still.
# Re-check the count when it drifts.

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
set_source_files_properties(src/app/update/entry.cpp PROPERTIES COMPILE_DEFINITIONS
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
set_source_files_properties(src/app/plugins/browser_entry.cpp PROPERTIES COMPILE_DEFINITIONS
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
# The client's own Application sources (all of MaiCord.exe's but the entry and
# the icon) and the updater are cold.
file(GLOB MAICORD_COLD_UPDATE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/app/update/*.cpp")
maicord_compact_sources(
    SOURCES ${MAICORD_COLD_UPDATE_SOURCES} src/app/account/login/acceptance_record.cpp
        src/app/media/account_media.cpp src/app/media/account_media_fetch.cpp src/app/media/account_media_requests.cpp
        src/app/media/account_media_local_preview.cpp src/app/media/account_media_worker.cpp
        src/app/media/account_media_worker_lanes.cpp src/app/media/regional_flag_assets.cpp
        src/app/account/game_detection.cpp src/app/net/file_persistence.cpp src/core/log.cpp src/ui/text/clipboard.cpp
        src/platform/desktop/staged_file.cpp src/platform/desktop/browser_sign_in_launcher.cpp
        src/platform/net/file.cpp src/platform/desktop/file_picker.cpp src/platform/desktop/clipboard_attachment.cpp
        src/platform/desktop/file_identity.cpp src/platform/desktop/timezone.cpp src/platform/crypto/secure_buffer.cpp
    TARGETS MaiCord maicord_protocol maicord_model maicord_plugin_host maicord_plugin_modules maicord_plugin_manager
        maicord_plugin_runtime
        maicord_account_host maicord_client_preferences maicord_account_paths maicord_webauthn
    # The user store resolves every member name and presence update: member-list
    # model time is 6% lower with it at /O2, for 2 KiB. The guild member store
    # and member list compile for size through minsize attribute regions in
    # their sources, which leave their member-list update and rebuild paths /O2.
    EXCLUDE
        "/app/shell/(main|app_icon)\\.cpp$"
        "/app/(shell/frame_presentation|shell/scheduler|media/video(_sources|_input)?)\\.cpp$"
        "/model/(chat/message_(rich_)?store(_pack)?|user/store|guild/member_store|guild/member_list)\\.cpp$"
        "/protocol/(auth/p256_signature|encoding/json_ids|encoding/json_write|encoding/writer|gateway/etf|gateway/events|gateway/message_events|gateway/message_rich(_reference|_attachments|_embeds)?|gateway/payload|gateway/reaction_events|gateway/typing_events|http/http1|http/http_response_stream|http/websocket_frame|http/websocket_message|messages/history|voice/dave_frame|voice/dave_replay|voice/media_events|voice/media_packet)\\.cpp$")
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
        src/platform/audio/notification_audio.cpp src/app/voice/stream_preview_cache.cpp src/app/voice/stream_watch_view.cpp
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
# The contract's /Ob1 emits every function not declared inline as noinline, so
# the helpers of a -Oz source stay calls even where inlining them is smaller.
# /Ob2 lets the size-driven inliner (-Oz keeps its minimum threshold) fold
# them: 17 KiB less code in the client, and no slower in the offline perf
# probe. Sources that grew in measured size with /Ob2 keep /Ob1, as do
# theme_styles (the text shaping path inlined ui_theme_text_size into slower
# code: text benchmark cycles_latin +1.6%, cycles_arabic +3%) and
# virtual_list (retention benchmark -8%, text measure cache hit -9%).
get_target_property(maicord_ui_sources maicord_ui SOURCES)
foreach(source IN LISTS maicord_ui_sources)
    get_source_file_property(options "${source}" COMPILE_OPTIONS)
    if(options MATCHES "${MAICORD_SIZE_OPTION}" AND NOT source MATCHES
        "/ui/(account/friends_scene|account/menu|account/profile_activity|account/profile_media|chat/composer/message_composer_scene|chat/messages/action_bar|chat/messages/context_menu|chat/messages/rich_widgets|chat/messages/scene(_rows)?|chat/pinned_messages_scene|chat/polls/voters|chat/private_channel_rows|chat/threads/browser_scene|chat/threads/forum_channel_scene|controls/slider|layout/virtual_list|settings/scene(_layout|_paint)?|shell/navigation_view|shell/popup_presentation|shell/quick_switcher_scene|theme/style_projection|theme/styles|voice/call_stage|voice/panel|widgets/toast)\\.cpp$")
        set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS /Ob2)
    endif()
endforeach()

# maicord_quoted_include_paths(<variable> <spelling> <includer> <directory>):
# where a quoted include of <includer> can name an application header: beside
# <includer>, or under <directory> (src/app, an include directory of tests).
function(maicord_quoted_include_paths variable spelling includer directory)
    cmake_path(GET includer PARENT_PATH base)
    cmake_path(ABSOLUTE_PATH spelling BASE_DIRECTORY "${base}" NORMALIZE OUTPUT_VARIABLE beside)
    cmake_path(ABSOLUTE_PATH spelling BASE_DIRECTORY "${directory}" NORMALIZE OUTPUT_VARIABLE included)
    set(${variable} "${beside}" "${included}" PARENT_SCOPE)
endfunction()

# maicord_application_state_headers(<variable> <directory>): the paths of
# <directory>/shell/state.hpp and of the headers under <directory> that include
# it, directly or through another of them.
function(maicord_application_state_headers variable directory)
    file(GLOB_RECURSE headers "${directory}/*.hpp")
    set(app_headers "${directory}/shell/state.hpp")
    set(pending)
    set(index 0)
    foreach(path IN LISTS headers)
        math(EXPR index "${index} + 1")
        set(path_${index} "${path}")
        file(STRINGS "${path}" lines REGEX "^[ \t]*#[ \t]*include[ \t]*\"")
        set(includes_${index})
        foreach(line IN LISTS lines)
            if(line MATCHES "\"([^\"]+)\"")
                maicord_quoted_include_paths(included "${CMAKE_MATCH_1}" "${path}" "${directory}")
                list(APPEND includes_${index} ${included})
            endif()
        endforeach()
        list(APPEND pending ${index})
    endforeach()
    set(changed TRUE)
    while(changed)
        set(changed FALSE)
        foreach(index IN LISTS pending)
            foreach(included IN LISTS includes_${index})
                if(included IN_LIST app_headers)
                    list(APPEND app_headers "${path_${index}}")
                    list(REMOVE_ITEM pending ${index})
                    set(changed TRUE)
                    break()
                endif()
            endforeach()
        endforeach()
    endwhile()
    set(${variable} ${app_headers} PARENT_SCOPE)
endfunction()

# maicord_reaches_application_state(<variable> <source> <directory> <header>...):
# whether <source> includes one of the <header> paths before any #define,
# #undef or #pragma of its own.
function(maicord_reaches_application_state variable source directory)
    set(app_headers ${ARGN})
    set(app FALSE)
    file(STRINGS "${source}" lines REGEX "^[ \t]*#[ \t]*(include|define|undef|pragma)")
    foreach(line IN LISTS lines)
        if(line MATCHES "^[ \t]*#[ \t]*include[ \t]*\"([^\"]+)\"")
            maicord_quoted_include_paths(included "${CMAKE_MATCH_1}" "${source}" "${directory}")
            foreach(path IN LISTS included)
                if(path IN_LIST app_headers)
                    set(app TRUE)
                endif()
            endforeach()
            if(app)
                break()
            endif()
        elseif(NOT line MATCHES "^[ \t]*#[ \t]*(include|pragma[ \t]+once)")
            break()
        endif()
    endforeach()
    set(${variable} ${app} PARENT_SCOPE)
endfunction()

# Application sources share a precompiled header (src/app/shell/pch.hpp:
# shell/state.hpp), about half of each source's front-end time. A /Yu object
# also leaves the header's inline code to the PCH object, which shrinks the LTO
# link input. One PCH per optimization mode, since cold sources carry -Oz and
# clang rejects a PCH built with other options. A source qualifies when it
# reaches shell/state.hpp (directly or through another src/app header) before
# any #define, #undef or #pragma of its own and has no definitions of its own,
# so the PCH holds only declarations it already includes. The options apply
# only to targets with MAICORD_APPLICATION_PCH, the directory of their own
# PCHs, so every target compiling a source uses a PCH built with its settings,
# or none. A source with the property MAICORD_NO_APPLICATION_PCH set never
# uses one.
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
    set(header "${CMAKE_CURRENT_SOURCE_DIR}/src/app/shell/pch.hpp")
    maicord_application_state_headers(app_headers "${CMAKE_CURRENT_SOURCE_DIR}/src/app")
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
        maicord_reaches_application_state(app "${path}" "${CMAKE_CURRENT_SOURCE_DIR}/src/app" ${app_headers})
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
