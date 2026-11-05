# Native plugins on Linux: the shared objects the tests load and the tests of the loader and the plugin runtime
# (cmake/test_registry.cmake registers the Windows ones under the same names, over DLLs). Every plugin is a freestanding
# shared object built by maicord_plugin (cmake/test_support.cmake: no libc, hidden symbols, one export) into plugins/
# of the build directory, where the tests copy it from. Included after cmake/linux_app_ui_tests.cmake, whose library
# list the runtime tests link.

# The samples of samples/plugins, built as an author would (samples/plugins/*/README.md).
set(maicord_plugin_samples)
foreach(maicord_sample IN ITEMS ClearUrls|clear-urls Translate|translate ShowMeYourName|show-me-your-name
        ReverseImageSearch|reverse-image-search SilentToggle|silent-toggle)
    string(REPLACE "|" ";" maicord_sample "${maicord_sample}")
    list(GET maicord_sample 0 maicord_sample_target)
    list(GET maicord_sample 1 maicord_sample_folder)
    maicord_plugin(MaiCord${maicord_sample_target}Plugin samples/plugins/${maicord_sample_folder}/plugin.cpp)
    set_target_properties(MaiCord${maicord_sample_target}Plugin PROPERTIES OUTPUT_NAME ${maicord_sample_folder})
    list(APPEND maicord_plugin_samples MaiCord${maicord_sample_target}Plugin)
endforeach()

# Plain C plugins take the compiler helpers from the SDK's own runtime (tests/fixtures/plugin_runtime_support.cpp).
set(MAICORD_PLUGIN_C_RUNTIME tests/fixtures/plugin_runtime_support.cpp)
maicord_plugin(MaiCordPluginAbiCFixture tests/app/plugins/plugin_abi_c.c ${MAICORD_PLUGIN_C_RUNTIME})

# One ABI-6 fixture per scenario of tests/fixtures/plugin_runtime_fixture.c: the Windows scenarios, and the ones a
# shared object has of its own (an initializer, an import, a needed library, an executable stack, the previous ABI).
set(maicord_runtime_fixtures)
foreach(maicord_runtime_fixture IN ITEMS Full FailStart Entry Tls Extra Bad Level Slow Crash Net Pipeline PipelineAuto
        PipelineBare Abi Needed Import Init ExecStack)
    string(TOUPPER "${maicord_runtime_fixture}" maicord_runtime_define)
    maicord_plugin(MaiCordPluginRuntime${maicord_runtime_fixture}Fixture tests/fixtures/plugin_runtime_fixture.c
        ${MAICORD_PLUGIN_C_RUNTIME})
    target_compile_definitions(MaiCordPluginRuntime${maicord_runtime_fixture}Fixture PRIVATE
        FIXTURE_${maicord_runtime_define})
    list(APPEND maicord_runtime_fixtures MaiCordPluginRuntime${maicord_runtime_fixture}Fixture)
endforeach()
# The thread-local variant needs its unused .tdata kept to make a PT_TLS segment.
target_link_options(MaiCordPluginRuntimeTlsFixture PRIVATE LINKER:--no-gc-sections)
# The entry-point variant is the one object whose ELF header names an entry point.
target_link_options(MaiCordPluginRuntimeEntryFixture PRIVATE LINKER:-e,fixture_entry)
# A needed library: another fixture, linked in even though nothing is used from it.
add_dependencies(MaiCordPluginRuntimeNeededFixture MaiCordPluginRuntimeFullFixture)
target_link_options(MaiCordPluginRuntimeNeededFixture PRIVATE LINKER:--no-as-needed
    "$<TARGET_FILE:MaiCordPluginRuntimeFullFixture>")
target_link_options(MaiCordPluginRuntimeExecStackFixture PRIVATE LINKER:-z,execstack)

maicord_plugin(MaiCordPluginUiFixture tests/fixtures/plugin_ui_fixture.c ${MAICORD_PLUGIN_C_RUNTIME})
maicord_plugin(MaiCordPluginSurfacesFixture tests/fixtures/plugin_surfaces_fixture.c ${MAICORD_PLUGIN_C_RUNTIME})
maicord_plugin(MaiCordPluginSurfacesSecondFixture tests/fixtures/plugin_surfaces_fixture.c ${MAICORD_PLUGIN_C_RUNTIME})
target_compile_definitions(MaiCordPluginSurfacesSecondFixture PRIVATE SURFACES_SECOND)

set(maicord_plugin_test_libs ${maicord_linux_app_test_libs})

# The loader on its own: the policy of the pinned directory, the file and the ELF checks (tests/app/plugins/loader_elf.cpp).
maicord_test(MaiCordPluginLoaderElfTests ENTRY maicord_plugin_loader_elf_entry
    SOURCES tests/app/plugins/loader_elf.cpp
    LIBS ${maicord_plugin_test_libs}
    INCLUDES src/app tests
    DEPENDS ${maicord_runtime_fixtures} ${maicord_plugin_samples} MaiCordPluginAbiCFixture
    TIMEOUT 20 TEST maicord_plugin_loader_elf)

# ABI 6 (maicord_plugin.h, plugin.hpp): the C++ helpers, then each sample and the plain C fixture as shared objects: the
# shape of the image (no entry point, TLS, needed library or undefined symbol, one export), the descriptor, and the entry
# points called directly against a recording host (tests/app/plugins/plugin_abi.cpp; the PE half is the Windows one).
maicord_test(MaiCordPluginAbiTests ENTRY maicord_plugin_abi_tests_entry
    SOURCES tests/app/plugins/plugin_abi.cpp
    LIBS ${maicord_plugin_test_libs}
    INCLUDES src/app tests
    DEPENDS ${maicord_plugin_samples} MaiCordPluginAbiCFixture
    TIMEOUT 15 TEST maicord_plugin_abi)

# The plugin runtime (loader, bindings, hook dispatch) against the fixtures, the samples and the C fixture.
maicord_test(MaiCordPluginBindingTests ENTRY maicord_plugin_binding_tests_entry
    SOURCES tests/app/plugins/plugin_binding.cpp
    LIBS ${maicord_plugin_test_libs}
    INCLUDES src/app tests
    DEPENDS ${maicord_runtime_fixtures} MaiCordClearUrlsPlugin MaiCordPluginAbiCFixture
    TIMEOUT 15 TEST maicord_plugin_binding)
maicord_test(MaiCordPluginNetworkTests ENTRY maicord_plugin_network_tests_entry
    SOURCES tests/app/plugins/plugin_network.cpp
    LIBS ${maicord_plugin_test_libs}
    INCLUDES src/app tests
    DEPENDS MaiCordPluginRuntimeNetFixture MaiCordTranslatePlugin
    TIMEOUT 15 TEST maicord_plugin_network)
maicord_test(MaiCordPluginPipelineTests ENTRY maicord_plugin_pipeline_tests_entry
    SOURCES tests/app/plugins/plugin_pipeline.cpp
    LIBS ${maicord_plugin_test_libs}
    INCLUDES src/app tests
    DEPENDS MaiCordPluginRuntimeFullFixture MaiCordPluginRuntimePipelineFixture MaiCordPluginRuntimePipelineAutoFixture
        MaiCordPluginRuntimePipelineBareFixture MaiCordClearUrlsPlugin MaiCordSilentTogglePlugin MaiCordTranslatePlugin
    TIMEOUT 15 TEST maicord_plugin_pipeline)
# Context menus, slash commands, banners, modals and the navigation actions, against the UI fixture and a recording host.
maicord_test(MaiCordPluginUiTests ENTRY maicord_plugin_ui_tests_entry
    SOURCES tests/app/plugins/plugin_ui.cpp
    LIBS ${maicord_plugin_test_libs}
    INCLUDES src/app tests
    DEPENDS MaiCordPluginUiFixture MaiCordReverseImageSearchPlugin MaiCordSilentTogglePlugin
    TIMEOUT 15 TEST maicord_plugin_ui)
# Decorations and buttons on every surface, merged verdicts, and the samples' own.
maicord_test(MaiCordPluginSurfaceTests ENTRY maicord_plugin_surfaces_tests_entry
    SOURCES tests/app/plugins/plugin_surfaces.cpp tests/ui/plugins/surface_scenes.cpp
    LIBS ${maicord_plugin_test_libs}
    INCLUDES src/app tests
    DEPENDS MaiCordPluginSurfacesFixture MaiCordPluginSurfacesSecondFixture MaiCordShowMeYourNamePlugin
        MaiCordTranslatePlugin
    TIMEOUT 15 TEST maicord_plugin_surfaces)
# Slash commands of a plugin in the composer: the picker, option chips, Enter and where a reply lands, against the UI
# fixture (the application's units are listed as in the Windows registry; the library holds the rest).
maicord_test(MaiCordCommandUiTests ENTRY maicord_command_ui_tests_entry
    SOURCES tests/app/chat/composer/commands_ui.cpp tests/app/chat/composer/commands_ui_cases.cpp
        tests/app/chat/composer/commands_ui_plugin.cpp
        src/app/chat/composer/commands.cpp
        src/app/chat/composer/commands_frecency.cpp src/app/chat/composer/commands_picker.cpp
        src/app/chat/composer/commands_draft.cpp src/app/chat/composer/builtin_commands.cpp
        src/app/chat/composer/plugin_commands.cpp
        src/app/shell/state.cpp src/app/settings/modal.cpp
        src/app/chat/messages/feedback.cpp tests/app/account/activity_retry.cpp
        tests/app/account/custom_status_retry.cpp tests/app/notifications/retry.cpp
        tests/app/guild/community_retry.cpp tests/app/guild/invite_retry.cpp
    LIBS ${maicord_plugin_test_libs}
    INCLUDES src/app
    DEPENDS MaiCordPluginUiFixture
    TIMEOUT 15 TEST maicord_command_ui)

# The SDK's compile-time contract (plugin.hpp's diagnostics), over the Linux header branches.
foreach(maicord_sdk_case IN ITEMS
        "VALID|" "DUPLICATE_HOOK|MC_HOOKS: two hooks of the same kind"
        "UNKNOWN_HOOK|MC_HOOKS: no hook kind takes these parameters"
        "DUPLICATE_EVENT|MC_HOOKS: two handlers for the same event"
        "FILTER_KIND|mc::filter: this hook kind takes no filter"
        "WRONG_STATE|T must be the type named by MC_STATE"
        "STATE_NOT_TRIVIAL|the state type must be trivially copyable"
        "SETTING_KEY|error_setting_key_must_be_1_to_32_bytes"
        "CHOICE_DEFAULT|error_setting_default_out_of_range"
        "NO_OWNER|error_plugin_owner_must_be_nonzero"
        "MISSING_PERMISSION|ctx.copy needs .permissions = MC_PERM_CLIPBOARD"
        "NO_NETWORK|ctx.http needs .network")
    string(REPLACE "|" ";" maicord_sdk_case "${maicord_sdk_case}")
    list(GET maicord_sdk_case 0 maicord_sdk_case_name)
    list(GET maicord_sdk_case 1 maicord_sdk_case_expect)
    string(TOLOWER "${maicord_sdk_case_name}" maicord_sdk_case_test)
    add_test(NAME maicord_plugin_compile_${maicord_sdk_case_test} COMMAND "${CMAKE_COMMAND}"
        "-DCOMPILER=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DCASE=${maicord_sdk_case_name}"
        "-DEXPECT=${maicord_sdk_case_expect}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/app/plugins/plugin_compile_failures.cmake")
    set_tests_properties(maicord_plugin_compile_${maicord_sdk_case_test} PROPERTIES TIMEOUT 15)
endforeach()
