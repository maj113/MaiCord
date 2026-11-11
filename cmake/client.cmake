# The shipping client of both systems, MaiCord (MaiCord.exe on Windows, with the account module linked in), with its
# updater identity, its runtime assets, the image audit and the cold-code size policy.

# Self-updater publisher and plugin catalog keys, the installed version (cmake/update_config.cmake).
include(cmake/update_config.cmake)

# The shipped image has no symbol or string table on either system (/DEBUG:NONE, --strip-all): the release and every
# package take MaiCord as the build leaves it. MAICORD_SIZE_REPORT keeps what size analysis reads: the linker map
# (MaiCord.map) on Windows, the symbol table (nm, bloaty, profilers) on Linux.
option(MAICORD_SIZE_REPORT "Keep the client's linker map (Windows) or symbol table (Linux) for size analysis" OFF)

if(MAICORD_LINUX)
    # The application's shell over the libraries (cmake/linux_libraries.cmake), with the Wayland window backend (the
    # null backend, maicord_window_null, is the tests'). The shell is every unit of src/app/shell (the Windows branch
    # lists them one by one); the entry and the window glue are src/app/linux.
    file(GLOB MAICORD_SHELL_SOURCES CONFIGURE_DEPENDS RELATIVE "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/app/shell/*.cpp")
    maicord_program(MaiCord ENTRY maicord_entry
        SOURCES ${MAICORD_SHELL_SOURCES} src/app/linux/entry.cpp src/app/linux/platform_window.cpp
            src/app/linux/shutdown_signal.cpp src/app/linux/desktop_fonts.cpp
        LIBS maicord_compile_contract
            maicord_app_client maicord_ui_secure_text maicord_ui maicord_text_system maicord_text_catalog
            maicord_font_system maicord_font maicord_font_color maicord_theme maicord_unicode
            maicord_renderer maicord_atlas_gpu maicord_render_colr_paint maicord_paint_program maicord_render_core
            maicord_effect_host maicord_gpu_gles_video maicord_gpu_gles_shaders maicord_gpu_gles
            maicord_window_wayland maicord_audio_pulse maicord_aac_decoder maicord_video_decode
            maicord_desktop_system maicord_dbus maicord_atspi maicord_model maicord_protocol maicord_voice_encoder
            maicord_notification_pcm maicord_media_core maicord_media_codec maicord_compression
            maicord_crypto maicord_net maicord_core
        INCLUDES src/app "${MAICORD_GENERATED_DIRECTORY}")
    target_compile_options(MaiCord PRIVATE -Wno-c23-extensions)
    if(NOT MAICORD_SIZE_REPORT)
        target_link_options(MaiCord PRIVATE LINKER:--strip-all)
    endif()
else()
    option(MAICORD_STARTUP_PROBE "Measure isolated production-path startup" OFF)

    # The account module, linked into MaiCord.exe (and through the account host
    # into every test that drives it).
    maicord_library(maicord_account_module
        src/app/chat/polls/account_message_poll.cpp src/app/guild/account_guild_invite.cpp src/app/settings/account_settings_rest.cpp
        src/app/account/account_module.cpp src/app/account/account_module_login.cpp src/app/account/account_module_requests.cpp
        src/app/chat/search/account_message_search.cpp
        src/app/media/account_media_catalog.cpp src/app/guild/account_guild_assets.cpp src/app/media/guild_image.cpp
        src/app/media/profile_image.cpp src/app/account/account_profile_edit.cpp
        # Go Live producers: capture, stream audio and the GPU encoder on the renderer's device.
        src/app/gateway/go_live_devices.cpp
        LIBS maicord_core maicord_platform maicord_protocol
        maicord_app_core maicord_model maicord_account_media maicord_media_core
        maicord_media_codec maicord_cdn_media maicord_capture maicord_video_encode
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
        src/app/settings/test_mode.cpp
        src/app/settings/theme_editor.cpp src/app/settings/custom_theme.cpp
        src/app/settings/theme_publication.cpp src/app/account/friends.cpp
        src/app/account/friends_search.cpp src/app/account/friends_storage.cpp
        src/app/plugins/surfaces.cpp src/app/plugins/keybinds.cpp
        src/app/plugins/settings_dialog.cpp
        src/app/effects/effects.cpp src/app/packages/packages.cpp
        src/app/plugins/compose.cpp src/app/plugins/menus.cpp src/app/plugins/ui_host.cpp
        src/app/plugins/pipeline_host.cpp src/app/plugins/surface_host.cpp
        src/app/shell/window.cpp src/app/windows/platform_window.cpp src/app/shell/window_dispatch.cpp
        src/app/shell/input_routing.cpp
        src/app/shell/caption.cpp src/app/shell/tabs.cpp src/app/shell/app_icon.cpp
        src/app/shell/keyboard_focus.cpp src/app/account/application_account_module.cpp
        src/app/account/application_account_logout.cpp
        src/app/account/account_pump.cpp src/app/account/account_presentation.cpp
        src/app/account/application_account_reconnect.cpp
        src/app/chat/threads/starter.cpp src/app/chat/channel_session.cpp
        src/app/chat/channel_snapshots.cpp src/app/chat/composer/commands.cpp src/app/chat/composer/commands_frecency.cpp
        src/app/chat/composer/commands_picker.cpp src/app/chat/composer/commands_draft.cpp src/app/chat/composer/builtin_commands.cpp
        src/app/chat/composer/plugin_commands.cpp
        src/app/chat/composer/composer.cpp
        src/app/chat/messages/feedback.cpp src/app/chat/composer/reply_draft.cpp
        src/app/chat/composer/input.cpp src/app/shell/button_input.cpp
        src/app/chat/composer/edit_menu.cpp src/app/chat/composer/spellcheck.cpp
        src/app/chat/composer/attachments.cpp src/app/media/attachment_allowance.cpp
        src/app/media/upload_lane.cpp src/app/shell/content_input.cpp src/app/chat/application_link_trust.cpp
        src/app/media/image_viewer.cpp src/app/media/image_zoom.cpp src/app/shell/frame_presentation.cpp src/app/shell/memory_summary.cpp
        src/app/shell/startup_record.cpp src/app/shell/startup_gpu.cpp
        src/app/chat/unread_banner.cpp src/app/chat/messages/intro.cpp
        src/app/guild/folders.cpp src/app/shell/navigation_drag.cpp
        src/app/guild/lifecycle.cpp src/app/account/profile_editor.cpp src/app/account/profile_editor_preview.cpp
        src/app/account/profile_editor_images.cpp
        src/app/guild/menu.cpp src/app/guild/community.cpp src/app/guild/community_projection.cpp
        src/app/guild/card.cpp src/app/guild/banner.cpp src/app/guild/invite.cpp src/app/guild/hidden_channel.cpp
        src/app/notifications/taskbar_attention.cpp src/app/notifications/desktop.cpp
        src/app/notifications/desktop_settings.cpp src/app/shell/surface_motion.cpp
        src/app/chat/composer/text_form.cpp src/app/chat/composer/text_input.cpp
        src/app/account/login/auth.cpp src/app/account/login/auth_recovery.cpp src/app/account/login/input.cpp src/app/account/login/ui.cpp
        src/app/shell/ui_runtime.cpp src/app/guild/account_permission_storage.cpp src/app/media/media.cpp
        src/app/media/video.cpp src/app/media/video_sources.cpp src/app/media/video_input.cpp
        src/app/shell/tooltip.cpp src/app/chat/messages/reaction_tooltip.cpp
        src/app/media/emoji_tooltip.cpp src/app/media/expression_card.cpp
        src/app/chat/composer/interaction_card.cpp
        src/app/shell/toast.cpp src/app/shell/notice.cpp
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
        src/app/guild/settings/boost.cpp src/app/guild/settings/server_tag.cpp
        src/app/guild/settings/integrations.cpp src/app/guild/settings/automod.cpp
        src/app/guild/settings/server_access.cpp src/app/guild/settings/access.cpp
        src/app/guild/settings/community_setup.cpp src/app/guild/settings/server_template.cpp
        src/app/guild/settings/page_common.cpp
        src/app/chat/polls/create.cpp
        src/app/account/self_status.cpp src/app/account/login/browser_auth.cpp
        src/app/account/login/captcha_auth.cpp src/app/media/animation.cpp
        src/app/media/requests.cpp src/app/media/requests_message.cpp src/app/chat/messages/delete.cpp
        src/app/chat/application_private_conversation.cpp src/app/chat/private_channel.cpp
        src/app/chat/private_header.cpp
        src/app/chat/messages/edit.cpp src/app/chat/messages/mutation_notice.cpp
        src/app/account/profile.cpp src/app/account/dm_profile.cpp src/app/guild/member_moderation.cpp
        src/app/chat/messages/reaction.cpp src/app/chat/messages/send.cpp
        src/app/chat/composer/mention_confirmation.cpp src/app/chat/messages/history.cpp
        src/app/chat/read_state.cpp src/app/chat/scoped_read.cpp
        src/app/chat/scoped_read_menu.cpp src/app/notifications/notifications.cpp
        src/app/notifications/controls.cpp src/app/account/custom_status.cpp
        src/app/account/activity.cpp src/app/account/game_detection.cpp src/app/account/game_detection_system.cpp
        src/app/account/detectable_list.cpp src/app/chat/inbox.cpp
        src/app/chat/game_lookup.cpp src/app/chat/game_profile.cpp src/app/chat/composer/game_search.cpp
        src/app/chat/messages/interaction.cpp src/app/chat/messages/interaction_popups.cpp src/app/account/user_actions.cpp
        src/app/chat/report.cpp src/app/shell/scheduler.cpp src/app/shell/shutdown_metrics.cpp
        src/app/shell/scrolling.cpp src/app/shell/scroll_host.cpp src/app/shell/input.cpp
        src/app/shell/pane_resize.cpp src/app/shell/main.cpp src/app/windows/entry.cpp $<TARGET_OBJECTS:maicord_runtime>
        src/app/accessibility/accessibility.cpp src/app/accessibility/accessibility_describe.cpp
        src/app/accessibility/accessibility_actions.cpp src/app/accessibility/accessibility_status.cpp
        src/app/update/updates.cpp src/app/update/entry.cpp
        src/app/plugins/catalog_worker.cpp src/app/plugins/catalog_service.cpp src/app/plugins/catalog_app.cpp
        src/app/plugins/dev_console.cpp
        src/app/store/store_projection.cpp src/app/store/store_page.cpp src/app/store/store_detail.cpp src/app/store/store_dialogs.cpp
        src/app/store/store_act.cpp src/app/store/store_source.cpp src/app/store/store_host.cpp
        src/app/store/store_preview_decode.cpp src/app/store/store_preview_pump.cpp
        # Voice calls: soundboard, voice messages, preferences, voice card, mute/deafen,
        # the voice user menu, the call view, stage channels and Go Live.
        src/app/voice/soundboard.cpp src/app/voice/application_voice_message.cpp src/app/voice/voice.cpp
        src/app/voice/settings.cpp src/app/voice/settings_binding.cpp src/app/voice/call.cpp
        src/app/voice/stage.cpp src/app/voice/channels.cpp src/app/chat/messages/components.cpp
        src/app/chat/application_channel_memory.cpp src/app/shell/startup_history.cpp
        src/app/shell/application_startup_shell.cpp
        src/app/guild/invite_cards.cpp src/app/voice/stream.cpp src/app/voice/go_live.cpp
        LIBS maicord_compile_contract
        maicord_core maicord_text maicord_ui maicord_model maicord_client_preferences maicord_packages
        maicord_notification_sound_assets maicord_plugin_catalog
        maicord_plugin_runtime maicord_ui_input
        maicord_message_runtime maicord_message_media maicord_account_paths maicord_account_host
        maicord_render_core maicord_atlas_gpu maicord_renderer maicord_media_core maicord_webauthn maicord_capture
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
        set_property(SOURCE src/app/shell/main.cpp src/app/shell/window.cpp src/app/windows/platform_window.cpp
            src/app/windows/entry.cpp src/app/shell/frame_presentation.cpp src/app/shell/shutdown_metrics.cpp
            APPEND PROPERTY COMPILE_DEFINITIONS
            "$<$<BOOL:$<TARGET_PROPERTY:MAICORD_STARTUP_PROBE>>:MAICORD_STARTUP_PROBE=1>")
    endif()
    if(MAICORD_SIZE_REPORT)
        target_link_options(MaiCord PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/MaiCord.map")
    endif()
    # Startup: DLLs the device prestart does not need map on first use instead of
    # before the entry point, which the prestart waits for. The renderer maps
    # dcomp, shell32, ole32 and advapi32 (which GPU drivers import) on the main
    # thread while the worker creates the device; the rest load when first used.
    # maicord_core's __delayLoadHelper2 (src/core/delay_load.cpp) resolves them
    # through kernel32's ResolveDelayLoadedAPI and fails fast when one is missing.
    # The audit below checks the list.
    set(MAICORD_CLIENT_DELAY_LOADS dcomp.dll shell32.dll ws2_32.dll sspicli.dll ole32.dll advapi32.dll
        combase.dll bcrypt.dll crypt32.dll)
    list(TRANSFORM MAICORD_CLIENT_DELAY_LOADS PREPEND /DELAYLOAD: OUTPUT_VARIABLE MAICORD_CLIENT_DELAY_LOAD_OPTIONS)
    target_link_options(MaiCord PRIVATE ${MAICORD_CLIENT_DELAY_LOAD_OPTIONS})
    # LTO code generation stays in one partition (lld's default). Once the machine
    # outliner is on (cmake/build_contract.cmake), one partition links 28 KB smaller
    # than three, in 39 s instead of 26 s; other counts are larger still.
    # Re-check the count when it drifts.
endif()

# Runtime assets next to the executable: licenses and the bundled fonts.
include(cmake/runtime_assets.cmake)

# The image audit (tools/audit_image.py) after every link of the client: no test hooks in a shipping client (and the
# hooks in a client built with them), no C or C++ runtime, no debug data, the mitigations and, on Windows, these exact
# import sets and the .data budget (the image keeps large zero-filled globals in .bss; 16 KiB of initialized .data
# leaves room for small tables but not for a serialized aggregate). A failed audit fails the link step, which Ninja
# then runs again on the next build.
if(maicord_test_hooks)
    set(maicord_client_audit_options --test-hooks)
else()
    set(maicord_client_audit_options --no-test-hooks)
endif()
if(NOT MAICORD_LINUX)
    set(MAICORD_CLIENT_STATIC_IMPORTS kernel32.dll user32.dll d3d11.dll gdi32.dll ntdll.dll)
    list(APPEND maicord_client_audit_options --maximum-initialized-data-bytes 16384
        --static-imports ${MAICORD_CLIENT_STATIC_IMPORTS} --delay-load ${MAICORD_CLIENT_DELAY_LOADS})
endif()
add_custom_command(TARGET MaiCord POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/audit_image.py" "$<TARGET_FILE:MaiCord>"
        ${maicord_client_audit_options}
    VERBATIM)

# The source rules that need no build (tools/check_source_rules.py: the GLSL twins of the HLSL shaders, the constant
# buffer layouts, theme literals, the example theme), checked before the client links. Ninja reruns them only when a
# file they read changes (their depfile lists them); the client's compiles don't wait for them.
set(maicord_source_rules_stamp "${CMAKE_CURRENT_BINARY_DIR}/source_rules.stamp")
add_custom_command(OUTPUT "${maicord_source_rules_stamp}"
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_source_rules.py"
        --stamp "${maicord_source_rules_stamp}"
    DEPFILE "${maicord_source_rules_stamp}.d"
    DEPENDS tools/check_source_rules.py
    VERBATIM)
add_custom_target(maicord_source_rules ALL DEPENDS "${maicord_source_rules_stamp}")
set_property(TARGET MaiCord APPEND PROPERTY LINK_DEPENDS "${maicord_source_rules_stamp}")

# The cold-code size policy (MAICORD_SIZE_OPTION sources and their folding), one list for both clients.
include(cmake/size_policy.cmake)

if(MAICORD_LINUX)
    return()
endif()

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
