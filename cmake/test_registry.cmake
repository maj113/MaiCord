# Test registry of both systems: every temporary test executable or fixture and its CTest cases, under the same names
# and labels on Windows and Linux. A test of code that only one system builds says ONLY WINDOWS or ONLY LINUX, and
# WINDOWS and LINUX mark the arguments of one system (maicord_test_select). maicord_test(), maicord_add_tests() and the
# registration helpers below are in cmake/test_support.cmake; register each new test here, next to the area it covers.
# Sources are the shared tests/ files: a platform difference inside one is an #if in the test, and the few Win32 file
# and console calls the shared tests make come from tests/support/win32_test_compat.hpp on Linux.

# The account module of the tests that link application code without it (the shipped client calls it by name): the
# account fake answers as a module without an owner unless a test sets the hook of an entry in g_account_fake.
set(maicord_account_fake tests/support/account_module_fake.cpp)
# Library sets most src/app fixtures link on Windows.
set(maicord_app_test_libs maicord_core maicord_platform maicord_protocol maicord_model maicord_app_core maicord_ui)
set(maicord_app_ui_test_libs ${maicord_app_test_libs} maicord_ui_input maicord_message_runtime maicord_text)
# The app libraries for tests that drive handlers against the headless window (src/platform/window/null): it is listed
# before maicord_platform so its window service wins over the Windows backend.
set(maicord_headless_test_libs maicord_core maicord_window_null maicord_platform maicord_protocol maicord_model maicord_app_core maicord_ui)
set(maicord_headless_ui_test_libs ${maicord_headless_test_libs} maicord_ui_input maicord_message_runtime maicord_text)
if(MAICORD_LINUX)
    # The Linux application library (maicord_app_client) over the null window backend and the Linux services, with the
    # portable units of src/app/shell the fixtures link (the shell itself belongs to the window layer).
    maicord_library(maicord_test_shell_units
        src/app/shell/shortcut_registry.cpp src/app/shell/navigation_shortcuts.cpp src/app/shell/quick_switcher.cpp
        src/app/shell/quick_switcher_controller.cpp src/app/shell/input_layers.cpp
        LIBS maicord_core INCLUDES src/app)
    set(maicord_linux_app_test_libs maicord_app_client maicord_test_shell_units maicord_ui maicord_ui_secure_text
        maicord_text_system maicord_text_catalog maicord_font_system maicord_font maicord_font_color maicord_theme
        maicord_unicode maicord_render_core maicord_window_null maicord_desktop_system maicord_dbus maicord_atspi
        maicord_model maicord_protocol maicord_media_core maicord_media_codec maicord_compression maicord_crypto
        maicord_net maicord_core)
    # The shared renderer over the OpenGL ES backend, for the tests that draw into an offscreen GpuSurface (an EGL
    # driver: llvmpipe anywhere, GALLIUM_DRIVER=d3d12 on WSL).
    set(maicord_render_test_libs
        maicord_renderer maicord_atlas_gpu maicord_paint_program maicord_render_colr_paint maicord_effect_host
        maicord_ui maicord_text_system maicord_text_catalog maicord_font_system maicord_font_color maicord_font
        maicord_unicode maicord_model maicord_app_update maicord_media_core maicord_media_codec maicord_compression
        maicord_render_core maicord_theme maicord_gpu_gles_video maicord_gpu_gles_shaders maicord_gpu_gles maicord_core)
endif()
# Link inputs of each shared test executable: all its members' libraries and
# include directories, in link order (see maicord_test_bundle).
maicord_test_bundle(MaiCordAppUnitTests INCLUDES src/app
    WINDOWS LIBS maicord_core maicord_platform maicord_app_core kernel32 bcrypt maicord_ui
        user32 maicord_protocol maicord_model maicord_text maicord_message_runtime ws2_32
        maicord_sspicli crypt32 maicord_ui_input maicord_account_paths
    LINUX LIBS ${maicord_linux_app_test_libs})
maicord_test_bundle(MaiCordUiUnitTests
    WINDOWS LIBS maicord_core maicord_platform maicord_ui kernel32 user32 maicord_model maicord_ui_input
    LINUX LIBS ${maicord_linux_app_test_libs} INCLUDES src/app
    BOTH LIBS maicord_edit_history)
maicord_test_bundle(MaiCordAppWaveTests
    WINDOWS LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_app_core maicord_ui
        maicord_message_runtime kernel32 user32 ole32
    LINUX LIBS ${maicord_linux_app_test_libs} INCLUDES src/app)
maicord_test_bundle(MaiCordModelTests INCLUDES src/app LIBS maicord_core maicord_protocol maicord_model kernel32
    WINDOWS LIBS maicord_platform maicord_text maicord_ui user32 maicord_ui_input maicord_message_runtime)
maicord_test_bundle(MaiCordAccountRequestTests
    WINDOWS LIBS maicord_core maicord_protocol maicord_model maicord_account_host kernel32 maicord_platform
        maicord_media_codec maicord_compression user32
    LINUX LIBS ${maicord_linux_app_test_libs} INCLUDES src/app)
maicord_test_bundle(MaiCordCoreUnitTests
    LIBS maicord_core kernel32 WINDOWS maicord_account_paths BOTH maicord_compression maicord_protocol
        maicord_media_core maicord_media_codec crypt32 bcrypt)
maicord_test_bundle(MaiCordProtocolTests
    LIBS maicord_core maicord_protocol kernel32 WINDOWS maicord_platform BOTH maicord_media_core)
maicord_test_bundle(MaiCordCodecUnitTests LIBS maicord_core maicord_media_codec kernel32 INCLUDES src/platform
    LINUX LIBS maicord_compression)
maicord_test_bundle(MaiCordGpuUnitTests ONLY WINDOWS
    LIBS maicord_atlas_gpu maicord_core d3d11 kernel32 maicord_gpu_d3d11 maicord_compression maicord_media_codec
        maicord_renderer maicord_render_core maicord_ui
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}")
maicord_test_bundle(MaiCordTextUnitTests
    LIBS maicord_core maicord_unicode kernel32 maicord_font advapi32 WINDOWS maicord_ui maicord_paint_program)
maicord_test_bundle(MaiCordOAuthTests ONLY WINDOWS
    LIBS maicord_app_core maicord_protocol maicord_platform maicord_core kernel32 ${maicord_tls_libs})

maicord_test(MaiCordRemoteAuthCancellationTests ONLY WINDOWS ENTRY maicord_remote_auth_cancellation_entry
    SOURCES tests/app/account/login/remote_auth_ticket_exchange_cancellation.cpp src/app/account/login/remote_auth_ticket_exchange.cpp
        src/app/account/login/remote_auth_handshake.cpp src/app/account/login/captcha_challenge.cpp src/app/net/signed_out_session.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32 ${maicord_tls_libs}
    TIMEOUT 15 TEST maicord_remote_auth_cancellation)
maicord_test(MaiCordAccountLoginHandoffTests ONLY WINDOWS ENTRY maicord_account_login_handoff_entry
    SOURCES tests/app/account/login/account_login_handoff.cpp tests/app/account/login/account_login_remote_fixture.cpp src/app/account/account_profile_promotion.cpp
        src/app/account/login/remote_auth_ticket_exchange.cpp src/app/account/login/captcha_challenge.cpp src/app/account/login/remote_auth_handshake.cpp
        src/app/account/login/captcha_bridge.cpp src/app/account/login/loopback_server.cpp src/app/net/signed_out_session.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32 ${maicord_tls_libs}
    INCLUDES src/app
    TIMEOUT 15 TEST maicord_account_login_handoff)
maicord_test(MaiCordNotificationRetentionTests ENTRY maicord_notification_retention_tests_entry
    SOURCES tests/model/notifications/retention.cpp
    LIBS maicord_model
    BUNDLE MaiCordCoreUnitTests
    TEST maicord_notification_retention)
maicord_test(MaiCordNotificationSoundAssetTests ONLY WINDOWS ENTRY maicord_notification_sound_assets_tests_entry
    SOURCES tests/app/notifications/sound_assets.cpp "${MAICORD_NOTIFICATION_SOUND_RESOURCE}"
    LIBS maicord_notification_sound_assets kernel32 bcrypt
    TEST maicord_notification_sound_assets)
maicord_test(MaiCordNotificationAudioTests ENTRY maicord_notification_audio_tests_entry
    SOURCES tests/platform/audio/native_notification_audio.cpp
    WINDOWS LIBS maicord_notification_audio ole32 user32
        BUNDLE MaiCordCoreUnitTests
    LINUX SOURCES tests/platform/audio/window_post_stub.cpp
        LIBS maicord_audio_pulse maicord_core
    TEST maicord_notification_audio)
# Compile an isolated copy with deterministic COM transport hooks. The
# shipping library excludes the seam completely; fixtures never open an
# actual endpoint even if no hook has been installed.
maicord_test(MaiCordNotificationAudioFaultTests ONLY WINDOWS ENTRY maicord_notification_audio_faults_tests_entry
    SOURCES src/platform/audio/notification_audio.cpp src/platform/audio/notification_audio_mixer.cpp
        tests/platform/audio/native_notification_audio_faults.cpp
    LIBS maicord_core kernel32 ole32 user32
    DEFINES MAICORD_NOTIFICATION_AUDIO_FIXTURE=1
    TEST maicord_notification_audio_faults)
if(MAICORD_LINUX)
    # The tests of the Linux audio backend (src/platform/audio).
    #
    #   maicord_audio_pure                 the transport-free media ring and clock conversions shared with the Windows
    #                                      suite, and the arithmetic of the PulseAudio transport (buffer attributes
    #                                      against the Windows engine timing, the playback timeline, capture pacing and
    #                                      the reopen schedule) with an injected clock
    #   maicord_notification_audio         the notification mixer, request preparation, lifecycle and bounded worker
    #                                      cases the Windows suite runs (tests/platform/audio/native_notification_audio.cpp)
    #   maicord_audio_no_server            audio with libpulse but no server: a state, not a crash
    #   maicord_audio_no_library           audio without libpulse: the same
    #
    # The integration test needs a PulseAudio server, so it is opt-in: configure with MAICORD_PULSE_INTEGRATION=1 in the
    # environment (tools/test_pulse_audio.sh does, and picks the server: WSLg's, or a local pulseaudio) and it runs;
    # otherwise it is registered disabled. It plays into a null sink of its own and records the sink's monitor.
    #
    #   maicord_pulse_loopback             voice enumeration, voice output, notification sounds at two rates, media with
    #                                      per-stream volume, the capture of all of it, endpoint change events
    #
    # No test links a window backend: window_post_stub.cpp counts the wakes the audio workers post.
    maicord_test(MaiCordAudioPureTests ENTRY maicord_audio_pure_entry
        SOURCES tests/platform/audio/audio_pure_linux.cpp tests/platform/audio/media_audio.cpp
            tests/platform/audio/pulse_timing.cpp tests/platform/audio/window_post_stub.cpp
        LIBS maicord_audio_pulse maicord_core
        TIMEOUT 10 TEST maicord_audio_pure)
    maicord_test(MaiCordAudioUnavailableTests ENTRY maicord_audio_unavailable_entry
        SOURCES tests/platform/audio/pulse_unavailable.cpp tests/platform/audio/window_post_stub.cpp
        LIBS maicord_audio_pulse maicord_core
        TIMEOUT 20 ENV PULSE_SERVER=unix:/maicord-no-such-pulse-server
        TEST maicord_audio_no_server --no-server)
    maicord_add_tests(MaiCordAudioUnavailableTests TIMEOUT 20 ENV MAICORD_PULSE_LIBRARY=libmaicord-no-such-pulse.so.0
        TEST maicord_audio_no_library --no-library)
    # Text-to-speech (src/platform/audio/speechd), the logic against fake libspeechd entry points and the real library's
    # unavailable states. The no-daemon case has no daemon to reach and none it could start (an address in a directory that
    # does not exist, no PATH for the spawn).
    #
    #   maicord_speechd                    what is opened and said, the text handed over, the failure and release rules
    #   maicord_speechd_no_library         without libspeechd: speech reports unavailable
    #   maicord_speechd_no_daemon          libspeechd but no speech-dispatcher: the same (skipped without libspeechd)
    #
    # The integration test needs speech-dispatcher with its dummy output module, so it is opt-in: configure with
    # MAICORD_SPEECHD_INTEGRATION=1 in the environment (tools/test_speechd.sh does, with a private daemon whose log it reads
    # afterwards).
    #
    #   maicord_speechd_live               the real libspeechd says, interrupts and closes against that daemon
    maicord_test(MaiCordSpeechdTests ENTRY maicord_speechd_entry
        SOURCES tests/platform/audio/speechd.cpp
        LIBS maicord_audio_pulse maicord_core
        TIMEOUT 20 TEST maicord_speechd)
    maicord_add_tests(MaiCordSpeechdTests TIMEOUT 20 ENV MAICORD_SPEECHD_LIBRARY=libmaicord-no-such-speechd.so.2
        TEST maicord_speechd_no_library --no-library)
    maicord_add_tests(MaiCordSpeechdTests TIMEOUT 60
        ENV SPEECHD_ADDRESS=unix_socket:/maicord-no-such-directory/speech.sock PATH=/maicord-no-such-path
        TEST maicord_speechd_no_daemon --no-daemon)
    maicord_add_tests(MaiCordSpeechdTests TIMEOUT 60 LABELS integration RESOURCE_LOCK speech_dispatcher
        TEST maicord_speechd_live --live)
    set_tests_properties(maicord_speechd_no_daemon maicord_speechd_live PROPERTIES SKIP_RETURN_CODE 77)
    if(NOT "$ENV{MAICORD_SPEECHD_INTEGRATION}")
        set_tests_properties(maicord_speechd_live PROPERTIES DISABLED TRUE)
    endif()
    maicord_test(MaiCordPulseLoopbackTests ENTRY maicord_pulse_loopback_entry
        SOURCES tests/platform/audio/pulse_loopback.cpp tests/platform/audio/window_post_stub.cpp
        LIBS maicord_audio_pulse maicord_core
        TIMEOUT 120 LABELS integration TEST maicord_pulse_loopback)
    # Status 77: no libpulse, no server, or a server that refuses to load a module. One server at a time.
    set_tests_properties(maicord_pulse_loopback PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK pulse_server)
    if(NOT "$ENV{MAICORD_PULSE_INTEGRATION}")
        set_tests_properties(maicord_pulse_loopback PROPERTIES DISABLED TRUE)
    endif()
    # The AAC-LC decoder (src/media/audio/aac_lc.cpp) behind the decoder interface of the Linux build, against fixtures
    # from libavcodec's encoder and decoder (tools/generate_aac_fixtures.py): conformance to the reference decoder within
    # half a PCM16 step, the MP4 and ADTS paths, pulse data against the standard's IMDCT, the interface's lifecycle rules,
    # and corrupted frames.
    maicord_test(MaiCordAacLcTests ENTRY maicord_aac_lc_entry
        SOURCES tests/platform/decode/aac_lc.cpp
        LIBS maicord_aac_decoder maicord_media_codec maicord_core
        TIMEOUT 30 TEST maicord_aac_lc)
endif()
maicord_test(MaiCordNotificationPcmTests ENTRY maicord_notification_pcm_tests_entry SOURCES tests/media/audio/notification_pcm.cpp
    LIBS maicord_notification_pcm
    BUNDLE MaiCordCoreUnitTests
    TEST maicord_notification_pcm)
# ABI 6 (include/maicord/sdk/maicord_plugin.h, plugin.hpp): the samples in
# samples/plugins and a plain C plugin, built the way the SDK documents (DLLs on
# Windows; on Linux shared objects in plugins/ of the build directory, where the
# tests copy them from); MaiCordPluginAbiTests checks their image shape and
# descriptors and calls their entry points directly. The compile-failure cases
# compile tests/app/plugins/plugin_compile_failures.cpp once each: VALID must
# build cleanly with the SDK's warnings as errors, the others must fail with the
# given diagnostic.
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
# Plain C plugins take memcpy and the other compiler helpers from MaiCord's runtime objects on Windows and from the
# SDK's own runtime (tests/fixtures/plugin_runtime_support.cpp) on Linux.
if(MAICORD_LINUX)
    set(MAICORD_PLUGIN_C_RUNTIME tests/fixtures/plugin_runtime_support.cpp)
else()
    set(MAICORD_PLUGIN_C_RUNTIME $<TARGET_OBJECTS:maicord_runtime>)
endif()
maicord_plugin(MaiCordPluginAbiCFixture tests/app/plugins/plugin_abi_c.c ${MAICORD_PLUGIN_C_RUNTIME})
maicord_test(MaiCordPluginAbiTests ENTRY maicord_plugin_abi_tests_entry
    SOURCES tests/app/plugins/plugin_abi.cpp
    DEPENDS ${maicord_plugin_samples} MaiCordPluginAbiCFixture
    WINDOWS LIBS maicord_core kernel32
    LINUX LIBS ${maicord_linux_app_test_libs}
        INCLUDES src/app tests
    TEST maicord_plugin_abi)
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
# The plugin runtime (src/app/plugins/loader.cpp, binding.cpp, dispatch.cpp): one ABI-6 fixture per scenario of
# tests/fixtures/plugin_runtime_fixture.c, the clear-urls sample and the C fixture above, loaded from the test's
# directory through a pinned plugins directory. A shared object has scenarios of its own: an initializer, an import, a
# needed library, an executable stack and the previous ABI.
set(maicord_runtime_fixtures)
set(maicord_runtime_scenarios Full FailStart Entry Tls Extra Bad Level Slow Crash Net Pipeline PipelineAuto PipelineBare)
if(MAICORD_LINUX)
    list(APPEND maicord_runtime_scenarios Abi Needed Import Init ExecStack)
endif()
foreach(maicord_runtime_fixture IN LISTS maicord_runtime_scenarios)
    string(TOUPPER "${maicord_runtime_fixture}" maicord_runtime_define)
    maicord_plugin(MaiCordPluginRuntime${maicord_runtime_fixture}Fixture tests/fixtures/plugin_runtime_fixture.c
        ${MAICORD_PLUGIN_C_RUNTIME})
    target_compile_definitions(MaiCordPluginRuntime${maicord_runtime_fixture}Fixture PRIVATE FIXTURE_${maicord_runtime_define})
    list(APPEND maicord_runtime_fixtures MaiCordPluginRuntime${maicord_runtime_fixture}Fixture)
endforeach()
if(MAICORD_LINUX)
    # The thread-local variant needs its unused .tdata kept to make a PT_TLS segment.
    target_link_options(MaiCordPluginRuntimeTlsFixture PRIVATE LINKER:--no-gc-sections)
    # The entry-point variant is the one object whose ELF header names an entry point.
    target_link_options(MaiCordPluginRuntimeEntryFixture PRIVATE LINKER:-e,fixture_entry)
    # A needed library: another fixture, linked in even though nothing is used from it.
    add_dependencies(MaiCordPluginRuntimeNeededFixture MaiCordPluginRuntimeFullFixture)
    target_link_options(MaiCordPluginRuntimeNeededFixture PRIVATE LINKER:--no-as-needed
        "$<TARGET_FILE:MaiCordPluginRuntimeFullFixture>")
    target_link_options(MaiCordPluginRuntimeExecStackFixture PRIVATE LINKER:-z,execstack)
else()
    # The entry-point variant is the one DLL with an entry point.
    get_target_property(maicord_entry_options MaiCordPluginRuntimeEntryFixture LINK_OPTIONS)
    list(REMOVE_ITEM maicord_entry_options /NOENTRY)
    set_property(TARGET MaiCordPluginRuntimeEntryFixture PROPERTY LINK_OPTIONS ${maicord_entry_options} /ENTRY:fixture_entry)
    target_link_libraries(MaiCordPluginRuntimeSlowFixture PRIVATE kernel32)
endif()
# The application's side of the plugin runtime (the plugin_host_* functions of include/maicord/app/plugins/{ui,pipeline,
# surfaces}.hpp) is called by name. A test that links runtime objects links one .cpp per side in place of
# src/app/plugins/{ui_host,pipeline_host,surface_host}.cpp: recording fakes of the UI and of the actions plugins take,
# and a reader over an empty cache (a test that reads defines the reader itself).
set(maicord_plugin_fake_ui tests/app/plugins/fake_ui_host.cpp)
set(maicord_plugin_fake_actions tests/app/plugins/fake_actions.cpp)
set(maicord_plugin_fake_reader tests/app/plugins/fake_reader.cpp)
set(maicord_plugin_fakes ${maicord_plugin_fake_ui} ${maicord_plugin_fake_actions} ${maicord_plugin_fake_reader})
# The loader on its own: the policy of the pinned directory, the file and the ELF checks.
maicord_test(MaiCordPluginLoaderElfTests ONLY LINUX ENTRY maicord_plugin_loader_elf_entry
    SOURCES tests/app/plugins/loader_elf.cpp
    LIBS ${maicord_linux_app_test_libs}
    INCLUDES src/app tests
    DEPENDS ${maicord_runtime_fixtures} ${maicord_plugin_samples} MaiCordPluginAbiCFixture
    TIMEOUT 20 TEST maicord_plugin_loader_elf)
maicord_test(MaiCordPluginBindingTests ENTRY maicord_plugin_binding_tests_entry
    SOURCES tests/app/plugins/plugin_binding.cpp ${maicord_plugin_fakes}
    INCLUDES src/app
    DEPENDS ${maicord_runtime_fixtures} MaiCordClearUrlsPlugin MaiCordPluginAbiCFixture
    WINDOWS SOURCES src/core/log.cpp
        LIBS maicord_core maicord_plugin_runtime kernel32 bcrypt
    LINUX LIBS ${maicord_linux_app_test_libs}
        INCLUDES tests
    TIMEOUT 15 TEST maicord_plugin_binding)
# The HTTP broker, completions and gestures, DPAPI-sealed secrets and the typed settings against the
# network fixture and a scripted transport.
maicord_test(MaiCordPluginNetworkTests ENTRY maicord_plugin_network_tests_entry
    SOURCES tests/app/plugins/plugin_network.cpp ${maicord_plugin_fakes}
    INCLUDES src/app
    DEPENDS MaiCordPluginRuntimeNetFixture MaiCordTranslatePlugin
    WINDOWS SOURCES src/core/log.cpp
        LIBS maicord_core maicord_plugin_runtime maicord_app_core maicord_platform kernel32 bcrypt ws2_32
            maicord_sspicli crypt32
    LINUX LIBS ${maicord_linux_app_test_libs}
        INCLUDES tests
    TIMEOUT 15 TEST maicord_plugin_network)
# The message pipeline: click, deleted, edited and typing hooks, the batched event feed, timers, kept
# messages and the permission, gesture and rate-limit gates of the actions, against the pipeline fixtures.
maicord_test(MaiCordPluginPipelineTests ENTRY maicord_plugin_pipeline_tests_entry
    SOURCES tests/app/plugins/plugin_pipeline.cpp ${maicord_plugin_fakes}
    INCLUDES src/app
    DEPENDS MaiCordPluginRuntimeFullFixture MaiCordPluginRuntimePipelineFixture MaiCordPluginRuntimePipelineAutoFixture
        MaiCordPluginRuntimePipelineBareFixture MaiCordClearUrlsPlugin MaiCordSilentTogglePlugin MaiCordTranslatePlugin
    WINDOWS SOURCES src/core/log.cpp
        LIBS maicord_core maicord_plugin_runtime maicord_app_core maicord_platform kernel32 bcrypt ws2_32
            maicord_sspicli crypt32
    LINUX LIBS ${maicord_linux_app_test_libs}
        INCLUDES tests
    TIMEOUT 15 TEST maicord_plugin_pipeline)
# The network layer against sockets of this machine: streams, datagrams, lookups, signals, files and the worker.
maicord_test(MaiCordNetworkLoopTests ENTRY maicord_network_loop_entry
    SOURCES tests/platform/net/network_loop.cpp
    WINDOWS LIBS maicord_core maicord_platform kernel32 ws2_32 bcrypt maicord_sspicli crypt32
    LINUX LIBS maicord_net maicord_crypto maicord_core
    TIMEOUT 20 TEST maicord_network_loop)
# The loop has no other backend than io_uring: a seccomp profile that refuses io_uring_setup (EPERM as Docker's and
# Podman's defaults answer, ENOSYS as a runtime that does not know the call answers) leaves no loop and a reason.
maicord_add_tests(MaiCordNetworkLoopTests ONLY LINUX TIMEOUT 10
    TEST maicord_network_loop_ring_refused --ring-refused
    TEST maicord_network_loop_ring_hidden --ring-hidden)
if(MAICORD_LINUX)
    # The platform crypto over the system OpenSSL: the vectors shared with the Windows suite (AES-GCM, AES-GCM with a
    # 64-bit tag, XChaCha20-Poly1305, P-256) and the ones of the parts the Linux backend adds (HKDF/HMAC/SHA-256, the
    # random bytes, the incremental hash, the remote-auth RSA key), in one process.
    maicord_test(MaiCordCryptoTests ENTRY maicord_crypto_tests_entry
        SOURCES tests/platform/crypto/crypto_tests_linux.cpp tests/platform/crypto/aes_gcm.cpp
            tests/platform/crypto/aes_gcm64.cpp tests/platform/crypto/xchacha20_poly1305.cpp
            tests/platform/crypto/p256.cpp tests/platform/crypto/kdf_sha256.cpp tests/platform/crypto/random.cpp
            tests/platform/crypto/hash_stream.cpp tests/platform/crypto/remote_auth_crypto_linux.cpp
        LIBS maicord_crypto maicord_core
        TIMEOUT 20 TEST maicord_crypto)
    # The secure buffer and the user-bound blobs under the key file of the private data directory; then the same calls
    # with a data directory that cannot hold the key (a path under /proc, and no home at all).
    maicord_test(MaiCordSecureBufferTests ENTRY maicord_secure_buffer_entry
        SOURCES tests/platform/crypto/secure_buffer_linux.cpp
        LIBS maicord_crypto maicord_core
        TIMEOUT 10 TEST maicord_secure_buffer)
    maicord_add_tests(MaiCordSecureBufferTests TIMEOUT 10
        ENV XDG_DATA_HOME=/proc
        TEST maicord_secure_buffer_unwritable --unavailable)
    maicord_add_tests(MaiCordSecureBufferTests TIMEOUT 10
        ENV --unset=XDG_DATA_HOME --unset=HOME
        TEST maicord_secure_buffer_no_home --unavailable)
    # The TLS client over the system OpenSSL: its cases that need no network, and the loopback sessions against local
    # `openssl s_server` processes that tests/platform/net/tls_smoke.sh starts (skipped, status 77, without the command).
    maicord_test(MaiCordTlsClientTests ENTRY maicord_tls_client_entry
        SOURCES tests/platform/net/tls_client_linux.cpp
        LIBS maicord_crypto maicord_core
        TIMEOUT 10 TEST maicord_tls_client)
    add_test(NAME maicord_tls_smoke
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/platform/net/tls_smoke.sh" "$<TARGET_FILE:MaiCordTlsClientTests>"
            "${CMAKE_BINARY_DIR}/tls-smoke")
    set_tests_properties(maicord_tls_smoke PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77)
    maicord_test_target(maicord_tls_smoke MaiCordTlsClientTests)
    # tls_stream over the whole stack (lookup, io_uring loop, sockets, OpenSSL) against the same kind of local s_server
    # processes, started by tests/platform/net/tls_stream_smoke.sh (skipped, status 77, without the command).
    maicord_test(MaiCordTlsStreamTests ENTRY maicord_tls_stream_entry SOURCES tests/platform/net/tls_stream_linux.cpp
        LIBS maicord_net maicord_crypto maicord_core)
    add_test(NAME maicord_tls_stream_smoke
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/platform/net/tls_stream_smoke.sh" "$<TARGET_FILE:MaiCordTlsStreamTests>"
            "${CMAKE_BINARY_DIR}/tls-stream-smoke")
    set_tests_properties(maicord_tls_stream_smoke PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    maicord_test_target(maicord_tls_stream_smoke MaiCordTlsStreamTests)
endif()
# The test hooks that point a Windows client at tools/fake_discord_server.py (platform/net/test_server.hpp): MAICORD_TEST_SERVER
# parsed and applied to the name lookup, MAICORD_TEST_CA_FILE the only trust (a certificate that does not chain to it is
# rejected). The sources with hooks compile into this executable with MAICORD_TEST_HOOKS; the libraries have none.
maicord_test(MaiCordTestServerTests ONLY WINDOWS ENTRY maicord_test_server_entry
    SOURCES tests/platform/net/test_server.cpp src/platform/net/test_server.cpp src/platform/net/network.cpp
    LIBS maicord_core maicord_platform kernel32 ws2_32 bcrypt maicord_sspicli crypt32
    DEFINES MAICORD_TEST_HOOKS=1
    TIMEOUT 15 TEST maicord_test_server)
# Go Live end to end (tests/app/voice/go_live_loopback.cpp): MaiCord's sender session streams H.264, H.265 and AV1
# with stream audio and DAVE to MaiCord's receive path through tools/fake_discord_server.py's stream server, which
# tools/test_go_live_loopback.py starts (the test hooks point the connections at it); the H.264 frames are decoded on
# the GPU and checked. The three codecs run side by side (each has its own server and client, paced by the clock), and
# the case holds no GPU chain (it renders no window). Skipped (77) without openssl or a hardware video device.
maicord_test(MaiCordGoLiveLoopbackTests ONLY WINDOWS ENTRY maicord_go_live_loopback_entry
    SOURCES tests/app/voice/go_live_loopback.cpp tests/support/voice_log_sink.cpp src/platform/net/test_server.cpp
        src/platform/net/network.cpp src/platform/net/tls_client.cpp
    LIBS maicord_core maicord_protocol maicord_platform maicord_app_core maicord_account_media maicord_model
        maicord_media_codec maicord_media_core maicord_voice_encoder kernel32 crypt32 ws2_32 maicord_sspicli bcrypt
        d3d11 ole32 user32
    DEFINES MAICORD_TEST_HOOKS=1
    INCLUDES src/app)
if(NOT MAICORD_LINUX)
    add_test(NAME maicord_go_live_loopback
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_go_live_loopback.py"
            "$<TARGET_FILE:MaiCordGoLiveLoopbackTests>" "${CMAKE_BINARY_DIR}/go-live-loopback")
    set_tests_properties(maicord_go_live_loopback PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS gpu)
    maicord_test_target(maicord_go_live_loopback MaiCordGoLiveLoopbackTests)
endif()
# Context menus (targets, a submenu, checkbox, visibility), typed slash commands against the composer's own
# option parsing, banners, modals and the navigation actions, against the UI fixture and a recording host.
maicord_plugin(MaiCordPluginUiFixture tests/fixtures/plugin_ui_fixture.c ${MAICORD_PLUGIN_C_RUNTIME})
maicord_test(MaiCordPluginUiTests ENTRY maicord_plugin_ui_tests_entry
    SOURCES tests/app/plugins/plugin_ui.cpp ${maicord_plugin_fakes}
    INCLUDES src/app
    DEPENDS MaiCordPluginUiFixture MaiCordReverseImageSearchPlugin MaiCordSilentTogglePlugin
    WINDOWS SOURCES src/core/log.cpp
        LIBS maicord_core maicord_protocol maicord_plugin_runtime maicord_app_core maicord_platform kernel32 bcrypt
            ws2_32 maicord_sspicli crypt32
    LINUX LIBS ${maicord_linux_app_test_libs}
        INCLUDES tests
    TIMEOUT 15 TEST maicord_plugin_ui)
# Decorations and buttons: the surfaces fixture decorates every surface and declares a button for each; the
# second build of the same file decorates messages after it, to show how verdicts merge.
maicord_plugin(MaiCordPluginSurfacesFixture tests/fixtures/plugin_surfaces_fixture.c ${MAICORD_PLUGIN_C_RUNTIME})
maicord_plugin(MaiCordPluginSurfacesSecondFixture tests/fixtures/plugin_surfaces_fixture.c ${MAICORD_PLUGIN_C_RUNTIME})
target_compile_definitions(MaiCordPluginSurfacesSecondFixture PRIVATE SURFACES_SECOND)
if(NOT MAICORD_LINUX)
    target_link_libraries(MaiCordPluginSurfacesFixture PRIVATE kernel32)
endif()
# Theme and effect packages: the .mcfx parser (truncation, caps, bad bytecode, manifest, pass graphs, mutation
# vectors), the link grammar and the install files in a private folder. The spark-ribbon sample, packed by
# tools/maicord-plugin as an author would, is parsed too.
if(NOT MAICORD_LINUX)
    set(maicord_graph_package_folder "${MAICORD_GENERATED_DIRECTORY}/spark-ribbon-package")
    set(maicord_graph_package "${maicord_graph_package_folder}/build/spark-ribbon.mcfx")
    add_custom_command(OUTPUT "${maicord_graph_package}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${maicord_graph_package_folder}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/samples/effects/spark-ribbon/spark-ribbon.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/samples/effects/spark-ribbon/spark-ribbon.effect" "${maicord_graph_package_folder}"
        COMMAND "${CMAKE_COMMAND}" -E env "MAICORD_FXC_EXECUTABLE=${MAICORD_FXC_EXECUTABLE}" "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/maicord-plugin/maicord_plugin.py" effect build
            --dir "${maicord_graph_package_folder}" --compiler fxc
        DEPENDS samples/effects/spark-ribbon/spark-ribbon.hlsl samples/effects/spark-ribbon/spark-ribbon.effect
            include/maicord/sdk/background.hlsli tools/maicord-plugin/mcfx_tool.py VERBATIM)
endif()
maicord_test(MaiCordPackagesTests ONLY WINDOWS ENTRY maicord_packages_tests_entry
    SOURCES tests/app/packages/packages.cpp "${maicord_graph_package}"
    LIBS maicord_packages maicord_app_core maicord_platform maicord_core kernel32 bcrypt ws2_32 maicord_sspicli crypt32
    DEFINES "MAICORD_GRAPH_PACKAGE=L\"${maicord_graph_package}\""
    TIMEOUT 15 TEST maicord_packages)
maicord_test(MaiCordPluginSurfaceTests ENTRY maicord_plugin_surfaces_tests_entry
    SOURCES tests/app/plugins/plugin_surfaces.cpp tests/ui/plugins/surface_scenes.cpp ${maicord_plugin_fake_ui}
        ${maicord_plugin_fake_actions}
    INCLUDES src/app
    DEPENDS MaiCordPluginSurfacesFixture MaiCordPluginSurfacesSecondFixture MaiCordShowMeYourNamePlugin
        MaiCordTranslatePlugin
    WINDOWS SOURCES src/core/log.cpp
        LIBS maicord_core maicord_ui maicord_plugin_runtime maicord_app_core maicord_platform kernel32 bcrypt ws2_32
            maicord_sspicli crypt32
    LINUX LIBS ${maicord_linux_app_test_libs}
        INCLUDES tests
    TIMEOUT 15 TEST maicord_plugin_surfaces)
maicord_test(MaiCordNativeExtensionTests ONLY WINDOWS ENTRY maicord_native_extensions_entry
    SOURCES tests/protocol/gateway/message_poll.cpp tests/model/chat/message_poll_store.cpp tests/protocol/messages/poll_actions.cpp
        tests/app/chat/polls/controller.cpp tests/ui/chat/polls/controls.cpp tests/protocol/messages/poll_presentation.cpp
        src/app/chat/polls/account_message_poll.cpp tests/native_extensions.cpp tests/protocol/guild/channel_overview.cpp
        tests/app/gateway/channel_content_policy.cpp tests/app/guild/channel_overview_controller.cpp tests/app/chat/messages/pinned_message_controller.cpp
        tests/app/shell/quick_switcher_controller.cpp tests/app/chat/messages/forward_controller.cpp tests/model/chat/bounded_edit.cpp
        tests/protocol/user/gif_favorites.cpp tests/protocol/user/emoji_preferences.cpp tests/protocol/gateway/thread_starter.cpp tests/app/update/manifest.cpp
        tests/app/update/artifact.cpp tests/ui/shell/page_registry.cpp tests/app/update/acceptance.cpp
        tests/app/update/prepare.cpp tests/app/update/package.cpp tests/model/chat/reaction_sections.cpp
        tests/app/chat/messages/reaction_controller.cpp tests/model/chat/thread_authority_model.cpp tests/ui/chat/threads/header.cpp
        tests/app/chat/threads/header_activation.cpp src/app/chat/threads/header_activation.cpp
        tests/ui/text/regional_flag_media.cpp tests/ui/text/flag_surfaces.cpp tests/ui/chat/messages/composer_source_spans.cpp
        tests/app/account/login/geometry.cpp tests/ui/text/tsf_end_edit.cpp tests/model/guild/member_list_sparse_model.cpp
        tests/app/guild/member_viewport.cpp tests/app/gateway/member_paging_pump.cpp tests/app/gateway/thread_members.cpp tests/model/guild/thread_storage.cpp
        tests/app/gateway/thread_archive.cpp tests/model/guild/thread_member_viewport.cpp tests/app/gateway/thread_member_pump.cpp
        tests/app/gateway/thread_subscriptions.cpp tests/model/chat/role_lookup_performance.cpp tests/model/guild/role_order.cpp
        tests/app/gateway/guild_role_replacement.cpp tests/app/gateway/permission_prefix.cpp tests/app/gateway/guild_channel_validation.cpp
        tests/model/chat/channel_name_preflight.cpp tests/model/chat/chat_state_backing.cpp tests/model/chat/read_state_model.cpp
        tests/model/chat/read_state_wire.cpp tests/protocol/messages/ack.cpp tests/app/chat/read_ack_controller.cpp tests/protocol/messages/bulk_message_ack.cpp
        tests/app/chat/read_scope.cpp tests/app/chat/bulk_read_ack_controller.cpp tests/model/chat/read_state_pump.cpp
        tests/ui/guild/channel_context_policy.cpp tests/ui/guild/context_policy.cpp tests/ui/guild/folder_open.cpp tests/ui/guild/hidden_channel_menu.cpp tests/protocol/user/social_menus.cpp
        tests/ui/chat/unread_context.cpp tests/ui/chat/messages/mention_badge_text.cpp tests/app/gateway/ready_projection.cpp tests/ui/text/emitter.cpp
        tests/model/user/member_user_perf.cpp src/render/text/edit_layout.cpp tests/ui/chat/messages/context_controller.cpp
        tests/ui/settings/scene.cpp tests/ui/widgets/kit.cpp tests/ui/widgets/settings_choice.cpp tests/ui/account/password_recovery.cpp tests/ui/account/password_reset_ui.cpp
        tests/model/user/profile_editor_layout.cpp tests/protocol/auth/phone_recovery.cpp tests/ui/account/password_phone_ui.cpp tests/ui/account/friends_scene.cpp
        tests/ui/account/friends_media.cpp tests/ui/text/form_confirmation.cpp tests/ui/widgets/color_picker.cpp
        tests/app/gateway/relationship_sync.cpp tests/model/user/friend_search.cpp
        tests/app/settings/client_preferences.cpp tests/ui/layout/scroll_host_preferences.cpp tests/ui/guild/menu_rail_smooth_scroll.cpp
        tests/app/chat/visible_author_hydration.cpp tests/ui/theme/background_theme.cpp tests/ui/theme/background_effect_policy.cpp
        tests/ui/theme/background_color_math.cpp src/render/text/content_text_retention.cpp tests/ui/settings/theme_editor.cpp
        tests/ui/theme/styles.cpp tests/app/settings/client_theme_storage.cpp tests/app/settings/client_theme_document.cpp
        tests/ui/theme/style_projection.cpp tests/ui/theme/tokens.cpp tests/ui/theme/surface_elevation.cpp
        tests/app/account/login/continuations.cpp tests/app/settings/client_preferences_writer.cpp
        tests/ui/voice/video_controls.cpp tests/ui/voice/video_scene.cpp
        tests/ui/voice/audio_controls.cpp tests/ui/voice/audio_presentation.cpp tests/ui/core/ui_cursor.cpp tests/media/containers/ogg_opus.cpp
        tests/media/audio/opus_decoder.cpp tests/media/audio/wave_pcm.cpp tests/media/audio/mp3_stream.cpp tests/media/audio/mp3_decoder.cpp
        tests/app/media/account_video_session.cpp
        tests/app/media/account_video_commands.cpp tests/app/media/account_media_video_dispatch.cpp tests/app/media/local_draft_preview.cpp
        tests/media/video/hevc.cpp tests/media/video/hevc_references.cpp tests/media/video/video_frame_bounds.cpp tests/ui/widgets/spinner.cpp tests/ui/core/transition.cpp
        tests/app/store/store_page.cpp tests/ui/store/store_scene.cpp src/app/store/store_projection.cpp src/app/store/store_page.cpp
        src/app/store/store_detail.cpp src/app/store/store_dialogs.cpp src/app/store/store_act.cpp
        tests/ui/widgets/tooltip.cpp tests/model/chat/message_rich_store.cpp tests/ui/account/menu.cpp tests/model/chat/message_search.cpp
        tests/protocol/messages/pins.cpp tests/app/gateway/forwarded_message_snapshots.cpp tests/protocol/messages/forwarded_message_pages.cpp
        tests/app/gateway/message_forward_source_flags.cpp tests/ui/chat/search/scene.cpp tests/ui/chat/pinned_messages_scene.cpp
        tests/ui/chat/inbox_scene.cpp tests/ui/chat/threads/browser_scene.cpp tests/ui/chat/threads/forum_post_scene.cpp tests/ui/shell/quick_switcher_scene.cpp
        tests/ui/chat/messages/forward_scene.cpp tests/ui/media/download_scene.cpp tests/protocol/http/http_response_stream.cpp
        tests/protocol/media/attachment_download.cpp tests/app/net/https_file_download.cpp tests/ui/chat/search/query.cpp
        tests/ui/chat/search/filter_context.cpp tests/app/chat/search/recent.cpp tests/app/chat/link_trust.cpp tests/ui/media/picker.cpp
        tests/protocol/media/media_catalog.cpp tests/protocol/messages/create.cpp tests/app/chat/composer/message_composer_controller.cpp
        tests/app/media/video_request.cpp tests/ui/core/widget_builder.cpp tests/platform/desktop/staged_file.cpp tests/ui/widgets/menu_row.cpp
        tests/ui/widgets/icon_pack.cpp tests/platform/decode/aac_decoder.cpp tests/platform/audio/media_audio.cpp tests/protocol/auth/browser_sign_in.cpp
        tests/app/account/login/browser_sign_in_receiver.cpp tests/app/account/login/browser_login_ui.cpp tests/protocol/gateway/self_status.cpp
        tests/platform/desktop/integration.cpp
    LIBS maicord_core maicord_platform maicord_app_core maicord_ui maicord_client_preferences
        maicord_account_host maicord_account_paths maicord_account_media maicord_message_media
        maicord_ui_input maicord_text maicord_render_core maicord_model kernel32 user32 bcrypt crypt32 ws2_32
        maicord_sspicli ole32 ntdll
    INCLUDES src/app third_party/opus/include "${MAICORD_GENERATED_DIRECTORY}"
    DEFINES "MAICORD_THEME_EXAMPLE_PATH=L\"${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/example.maicord-theme.json\""
    USES_CLIENT MaiCord)
# The same 164 fixtures run once across four private process/profile owners.
# Shard 0 keeps all 5 durable update fixtures serial and runs 40 other fixtures;
# shards 1 and 2 run 40 and shard 3 runs 39. Every shard checks its exact function count.
set(maicord_extension_fixture_counts 45 40 40 39)
foreach(maicord_extension_shard RANGE 0 3)
    set(maicord_extension_test maicord_native_extensions)
    list(GET maicord_extension_fixture_counts ${maicord_extension_shard} maicord_extension_fixture_count)
    if(maicord_extension_shard)
        string(APPEND maicord_extension_test "_shard_${maicord_extension_shard}")
    endif()
    maicord_add_tests(MaiCordNativeExtensionTests
        PASS_REGULAR_EXPRESSION
            "PASS native extensions shard=${maicord_extension_shard}/4 fixtures=${maicord_extension_fixture_count}/164"
        TEST ${maicord_extension_test} --fixture-shard ${maicord_extension_shard} 4)
endforeach()
if(NOT MAICORD_LINUX)
    add_test(NAME maicord_build_output_guard COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_output_guard.ps1")
    set_tests_properties(maicord_build_output_guard PROPERTIES TIMEOUT 10)
endif()
maicord_test(MaiCordAccountVideoSessionProbe ONLY WINDOWS ENTRY maicord_account_video_session_probe_entry
    SOURCES tests/integration/account_video_session_probe.cpp src/app/media/account_video_session.cpp
        src/app/media/account_video_session_audio.cpp
        src/platform/decode/hevc_gpu_decoder.cpp src/gpu/d3d11/video_frame.cpp ${MAICORD_VIDEO_FRAME_HEADER}
    LIBS maicord_account_media maicord_platform maicord_model maicord_gpu_d3d11 d3d11 dxgi ole32 kernel32 user32
    INCLUDES ${MAICORD_GENERATED_DIRECTORY})
if(MAICORD_TEST_AUDIO_PLAYBACK)
    maicord_test(MaiCordNotificationAudioPlayback ONLY WINDOWS ENTRY maicord_notification_audio_playback_entry
        SOURCES tests/platform/audio/notification_audio_playback.cpp ${MAICORD_NOTIFICATION_SOUND_RESOURCE}
        LIBS maicord_notification_audio maicord_notification_sound_assets kernel32 ole32 user32
        TIMEOUT 5 RESOURCE_LOCK native_audio TEST maicord_notification_audio_playback)
    maicord_add_tests(MaiCordAccountVideoSessionProbe TIMEOUT 5 COST 51 RESOURCE_LOCK native_audio
        TEST maicord_account_audio_playback --audio)
endif()
maicord_test(MaiCordUpdateClientTests ONLY WINDOWS ENTRY maicord_update_client_tests_entry
    SOURCES tests/app/update/client.cpp src/app/update/updates.cpp src/app/settings/catalog.cpp
    LIBS maicord_core maicord_platform maicord_app_core maicord_ui maicord_client_preferences kernel32 bcrypt ws2_32
        maicord_sspicli
    INCLUDES src/app
    DEFINES "MAICORD_UPDATE_TEST_EXECUTABLE=L\"$<TARGET_FILE:MaiCord>\""
    USES_CLIENT MaiCord
    TIMEOUT 15 TEST maicord_update_client)
maicord_test(MaiCordUpdateChangelogTests ENTRY maicord_update_changelog_tests_entry SOURCES tests/app/update/changelog.cpp
    BUNDLE MaiCordAppUnitTests TIMEOUT 10 TEST maicord_update_changelog)
# The startup record a session's log begins with: its lines and fields, and no user name, path or token.
maicord_test(MaiCordStartupRecordTests ENTRY maicord_startup_record_entry
    SOURCES tests/app/shell/startup_record.cpp src/app/shell/startup_record.cpp
    INCLUDES src/app
    WINDOWS SOURCES src/core/log.cpp
        LIBS maicord_core maicord_window_null kernel32
    LINUX LIBS maicord_app_net maicord_net maicord_crypto maicord_desktop_system maicord_protocol maicord_compression
            maicord_media_codec maicord_window_null maicord_core
        INCLUDES tests
    TIMEOUT 10 TEST maicord_startup_record)
if(MAICORD_LINUX)
    # Tests of the application libraries on Linux: the updater's signed-manifest,
    # artifact, acceptance, preparation and package pipeline, which the Windows suite runs inside its native-extension
    # tests, and its Linux tarball install replayed over a fake network (tests/app/update/linux_package.cpp).
    maicord_test(MaiCordUpdatePipelineTests ENTRY maicord_update_pipeline_entry
        SOURCES tests/app/update/linux_main.cpp tests/app/update/manifest.cpp tests/app/update/artifact.cpp
            tests/app/update/acceptance.cpp tests/app/update/prepare.cpp tests/app/update/linux_package.cpp
            tests/app/update/portable.cpp
        LIBS maicord_app_update maicord_compression maicord_crypto maicord_protocol maicord_core INCLUDES src/app tests
        TIMEOUT 20 TEST maicord_update_pipeline)
    maicord_test(MaiCordHttpsFileDownloadTests ENTRY maicord_https_file_download_entry
        SOURCES tests/app/net/linux_main.cpp tests/app/net/https_file_download.cpp
        LIBS maicord_app_net maicord_net maicord_crypto maicord_protocol maicord_core INCLUDES src/app tests
        TIMEOUT 20 TEST maicord_https_file_download)
    maicord_test(MaiCordTransportTests ENTRY maicord_transport_entry
        SOURCES tests/protocol/http/linux_main.cpp tests/protocol/http/core_transport.cpp
        LIBS maicord_app_net maicord_net maicord_crypto maicord_desktop_system maicord_protocol maicord_compression
            maicord_media_codec maicord_core
        INCLUDES src/app tests
        TIMEOUT 60 TEST maicord_transport)
    maicord_test(MaiCordClientIdentityTests ENTRY maicord_client_identity_entry
        SOURCES tests/app/account/linux_main.cpp tests/app/account/client_request_metadata.cpp
            tests/app/account/client_identity.cpp
        LIBS maicord_app_net maicord_net maicord_crypto maicord_desktop_system maicord_protocol maicord_compression
            maicord_media_codec maicord_core
        INCLUDES src/app tests
        TIMEOUT 20 TEST maicord_client_identity)
endif()
# The Linux client's own checks: the text it shows names no Windows feature (tools/check_linux_ui_text.py reads the
# image; the settings catalog, which is stored deflated, is checked by maicord_linux_catalog_text), and `MaiCord
# --version` prints the build's number and nothing else (src/app/linux/entry.cpp). The Windows client's import sets
# are checked by the image audit after every link (cmake/client.cmake).
if(MAICORD_LINUX)
    add_test(NAME maicord_linux_client_text
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_linux_ui_text.py" "$<TARGET_FILE:MaiCord>")
    set_tests_properties(maicord_linux_client_text PROPERTIES TIMEOUT 30)
    add_test(NAME maicord_linux_client_version COMMAND MaiCord --version)
    set_tests_properties(maicord_linux_client_version PROPERTIES
        PASS_REGULAR_EXPRESSION "^MaiCord [0-9]+\\.[0-9]+\\.[0-9]+[^\n]*\n$" TIMEOUT 10)
    # Server Settings > Server Tag against the fake Discord (tools/test_server_tag_wayland.sh on
    # tools/test_server_settings_wayland.sh): the page loads the profile, saves the tag fields with their exact PATCH body
    # and adopts the tag; a server without GUILD_TAGS shows the locked card and writes nothing. Needs user and network
    # namespaces and headless sway, so it is opt-in: MAICORD_FAKE_DISCORD_INTEGRATION=1 at configure time (status 77
    # without the tools).
    add_test(NAME maicord_server_tag_ui_probe
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_server_tag_wayland.sh" --binary "$<TARGET_FILE:MaiCord>"
            --out "${CMAKE_BINARY_DIR}/server-tag-qa")
    set_tests_properties(maicord_server_tag_ui_probe PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
        RESOURCE_LOCK wayland_compositor)
    if(NOT "$ENV{MAICORD_FAKE_DISCORD_INTEGRATION}")
        set_tests_properties(maicord_server_tag_ui_probe PROPERTIES DISABLED TRUE)
    endif()
    # Home's Friends page against the fake Discord (tools/test_friends_wayland.sh): Add Friend sends the typed username
    # and reports the refusals under its field, and a friend's nickname saves with its PATCH body. Opt-in like the
    # Server Tag run.
    add_test(NAME maicord_friends_ui_probe
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_friends_wayland.sh" --binary "$<TARGET_FILE:MaiCord>"
            --out "${CMAKE_BINARY_DIR}/friends-qa")
    set_tests_properties(maicord_friends_ui_probe PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
        RESOURCE_LOCK wayland_compositor)
    if(NOT "$ENV{MAICORD_FAKE_DISCORD_INTEGRATION}")
        set_tests_properties(maicord_friends_ui_probe PROPERTIES DISABLED TRUE)
    endif()
    # Go Live against the fake Discord and the desktop portal mocks (tools/test_go_live_wayland.sh): the voice card's
    # screen share button opens the dialog, which picks a window through the ScreenCast portal (or explains why this
    # computer cannot stream), and Escape closes it. Opt-in like the Server Tag run.
    add_test(NAME maicord_go_live_ui_probe
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_go_live_wayland.sh" --binary "$<TARGET_FILE:MaiCord>"
            --out "${CMAKE_BINARY_DIR}/go-live-qa")
    set_tests_properties(maicord_go_live_ui_probe PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
        RESOURCE_LOCK wayland_compositor)
    if(NOT "$ENV{MAICORD_FAKE_DISCORD_INTEGRATION}")
        set_tests_properties(maicord_go_live_ui_probe PROPERTIES DISABLED TRUE)
    endif()
    # Server Settings > Server Profile against the fake Discord (tools/test_server_profile_wayland.sh): the page reads
    # the profile, the server's top games and their game objects, then saves a banner preset, a trait and a suggested
    # game in one PATCH, checked by its body. Opt-in like the Server Tag run.
    add_test(NAME maicord_server_profile_ui_probe
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_server_profile_wayland.sh" --binary "$<TARGET_FILE:MaiCord>"
            --out "${CMAKE_BINARY_DIR}/server-profile-qa")
    set_tests_properties(maicord_server_profile_ui_probe PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
        RESOURCE_LOCK wayland_compositor)
    if(NOT "$ENV{MAICORD_FAKE_DISCORD_INTEGRATION}")
        set_tests_properties(maicord_server_profile_ui_probe PROPERTIES DISABLED TRUE)
    endif()
    # Settings > Profiles against the fake Discord (tools/test_profile_editor_wayland.sh): the name font and effect
    # dropdowns picked by mouse and by keyboard, the colour picker (drag, typed hex, apply) and the saved PATCH body with
    # Nitro; without it the same picks stay in Try It Out and write nothing; an owned nameplate shows on the preview's
    # member row and saves; the same picker sets a role's custom colour in Server Settings > Roles. Opt-in like the
    # Server Tag run.
    add_test(NAME maicord_profile_editor_ui_probe
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_profile_editor_wayland.sh" --binary "$<TARGET_FILE:MaiCord>"
            --out "${CMAKE_BINARY_DIR}/profile-editor-qa")
    set_tests_properties(maicord_profile_editor_ui_probe PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
        RESOURCE_LOCK wayland_compositor)
    if(NOT "$ENV{MAICORD_FAKE_DISCORD_INTEGRATION}")
        set_tests_properties(maicord_profile_editor_ui_probe PROPERTIES DISABLED TRUE)
    endif()
    # Server Settings > Integrations and AutoMod against the fake Discord (tools/test_server_apps_wayland.sh): webhook
    # rename and create, unfollow, an app's and a command's permissions (with a member override), Gateway events from
    # another moderator, AutoMod rule disable and set up, and AutoMod inside a community server's Safety Setup, each
    # checked by its exact request body. Opt-in like the Server Tag run.
    add_test(NAME maicord_server_apps_ui_probe
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_server_apps_wayland.sh" --binary "$<TARGET_FILE:MaiCord>"
            --out "${CMAKE_BINARY_DIR}/server-apps-qa")
    set_tests_properties(maicord_server_apps_ui_probe PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77
        RESOURCE_LOCK wayland_compositor)
    if(NOT "$ENV{MAICORD_FAKE_DISCORD_INTEGRATION}")
        set_tests_properties(maicord_server_apps_ui_probe PROPERTIES DISABLED TRUE)
    endif()
    # Server Settings > Server Template, Access and Enable Community against the fake Discord
    # (tools/test_server_access_wayland.sh): template create, sync and delete; rules and questions typed in their
    # dialogs and saved with their exact member-verification bodies; the waiting applications read and rejected on
    # leaving Apply to Join; listing on Discovery (guild and metadata PATCHes, a tag, a subcategory); and the Community
    # setup's role and guild PATCHes. Opt-in like the Server Tag run.
    add_test(NAME maicord_server_access_ui_probe
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_server_access_wayland.sh" --binary "$<TARGET_FILE:MaiCord>"
            --out "${CMAKE_BINARY_DIR}/server-access-qa")
    set_tests_properties(maicord_server_access_ui_probe PROPERTIES TIMEOUT 420 SKIP_RETURN_CODE 77
        RESOURCE_LOCK wayland_compositor)
    if(NOT "$ENV{MAICORD_FAKE_DISCORD_INTEGRATION}")
        set_tests_properties(maicord_server_access_ui_probe PROPERTIES DISABLED TRUE)
    endif()
    # Game mentions against the fake Discord (tools/test_game_mentions_wayland.sh): mention chips and their batch
    # lookup, the tooltip, the game profile (its game object request and the friend who plays) and the composer's
    # @game search and pick. Opt-in like the Server Tag run.
    add_test(NAME maicord_game_mentions_ui_probe
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_game_mentions_wayland.sh" --binary "$<TARGET_FILE:MaiCord>"
            --out "${CMAKE_BINARY_DIR}/game-mentions-qa")
    set_tests_properties(maicord_game_mentions_ui_probe PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
        RESOURCE_LOCK wayland_compositor)
    if(NOT "$ENV{MAICORD_FAKE_DISCORD_INTEGRATION}")
        set_tests_properties(maicord_game_mentions_ui_probe PROPERTIES DISABLED TRUE)
    endif()
    # The app-behaviour User Settings pages against the fake Discord (tools/test_app_settings_wayland.sh): UI Density,
    # Show image descriptions, /tts, sticker autocomplete and Developer Mode, each checked by the field its decoded
    # settings-proto PATCH carries, with the Accessibility preview and the Application Test Mode dialog in the
    # screenshots. Opt-in like the Server Tag run.
    add_test(NAME maicord_app_settings_ui_probe
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_app_settings_wayland.sh" --binary "$<TARGET_FILE:MaiCord>"
            --out "${CMAKE_BINARY_DIR}/app-settings-qa")
    set_tests_properties(maicord_app_settings_ui_probe PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77
        RESOURCE_LOCK wayland_compositor)
    if(NOT "$ENV{MAICORD_FAKE_DISCORD_INTEGRATION}")
        set_tests_properties(maicord_app_settings_ui_probe PROPERTIES DISABLED TRUE)
    endif()
    # My Account, Data & Privacy and Connected Apps against the fake Discord
    # (tools/test_account_settings_wayland.sh on tools/test_user_settings_wayland.sh): the username change with its
    # password, a data request and a connection's visibility, each checked by its exact body. Opt-in like the Server
    # Tag run.
    add_test(NAME maicord_account_settings_ui_probe
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_account_settings_wayland.sh" --binary "$<TARGET_FILE:MaiCord>"
            --out "${CMAKE_BINARY_DIR}/account-settings-qa")
    set_tests_properties(maicord_account_settings_ui_probe PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77
        RESOURCE_LOCK wayland_compositor)
    if(NOT "$ENV{MAICORD_FAKE_DISCORD_INTEGRATION}")
        set_tests_properties(maicord_account_settings_ui_probe PROPERTIES DISABLED TRUE)
    endif()
endif()
# The delay-load helper (src/core/delay_load.cpp): imports by ordinal and by
# name, a forwarded API-set export, and racing first calls.
maicord_test(MaiCordDelayLoadHelperTests ONLY WINDOWS ENTRY maicord_delay_load_helper_entry
    SOURCES tests/core/delay_load_helper.cpp
    LIBS maicord_core kernel32 ws2_32 ole32
    LINK_OPTIONS /DELAYLOAD:ws2_32.dll /DELAYLOAD:ole32.dll
    TIMEOUT 5 TEST maicord_delay_load_helper)
# The official catalog (format 2): the index verifier against the vectors shared with
# tools/release/test_plugin_index_tools.py (tests/fixtures/plugin_index/format2), the install records
# the tool derives, and the catalog-state.bin record. MAICORD_CATALOG_E2E_DIR (with
# MAICORD_CATALOG_E2E_KEY) also verifies a CI-built index and every file it names.
maicord_test(MaiCordCatalogIndexTests ONLY WINDOWS ENTRY maicord_catalog_index_tests_entry
    SOURCES tests/app/plugins/catalog_index.cpp ${maicord_plugin_fakes}
    LIBS maicord_core maicord_platform maicord_app_core maicord_plugin_catalog maicord_plugin_runtime
        maicord_protocol kernel32 bcrypt
    DEFINES "MAICORD_CATALOG_FIXTURES=L\"${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/plugin_index/format2\""
    TIMEOUT 10 TEST maicord_catalog_index)
# The catalog service against a fake server: install, update, rollback, remove and revocation of
# plugins (the clear-urls sample, which then strips links through the pre-send hook), themes and
# effects; startup enabling from catalog-state.bin; replayed, unsigned and mismatched downloads.
maicord_test(MaiCordCatalogServiceTests ONLY WINDOWS ENTRY maicord_catalog_service_tests_entry
    SOURCES tests/app/plugins/catalog_service.cpp src/app/plugins/catalog_service.cpp src/app/plugins/catalog_worker.cpp
        src/app/plugins/dev_console.cpp src/core/log.cpp ${maicord_plugin_fakes}
    LIBS maicord_core maicord_platform maicord_app_core maicord_plugin_catalog maicord_plugin_runtime
        maicord_packages maicord_protocol kernel32 bcrypt ws2_32 maicord_sspicli crypt32
    INCLUDES src/app
    DEPENDS MaiCordClearUrlsPlugin MaiCordTranslatePlugin
    TIMEOUT 15 TEST maicord_catalog_service)
if(MAICORD_LINUX)
    # The official catalog's effects as the Linux client reads them: a signed index-4.json (interface 4 packages with the
    # GLSL section) verified, listed, downloaded, parsed and installed, and the DXBC package index-3.json lists refused for
    # lack of a payload. A catalog of three packages of tests/gpu/effects signed with the vectors' test key always; with
    # MAICORD_CATALOG_E2E_DIR and MAICORD_CATALOG_E2E_KEY (a site MaiCord-plugins' scripts built and signed; ctest passes the
    # environment on) the same over that site's effects.
    maicord_test(MaiCordCatalogEffectsTests ENTRY maicord_catalog_effects_entry
        SOURCES tests/app/plugins/catalog_effects.cpp src/app/update/fetch.cpp
        LIBS maicord_app_catalog_index maicord_app_packages maicord_app_net maicord_net maicord_crypto maicord_protocol
            maicord_core
        INCLUDES src/app
        TIMEOUT 20 TEST maicord_catalog_effects)
endif()
# Developer Mode and the maicord-plugin tool (plugin design 9.2 and 13). Four builds of one plugin stand for a
# developer's rebuilds (tests/fixtures/plugin_reload_fixture.cpp: same layout, changed layout, one more
# permission). What the tool itself makes is made by the tool here, as a user runs it: `new hello`, `build` and
# `dev` for a plugin, `effect new` and `effect preview` for an effect, into the generated folder; the test loads
# the plugin from that dev folder and parses the effect with the client's parser.
if(NOT MAICORD_LINUX)
    foreach(maicord_reload_build IN ITEMS a:1 b:2 c:3 d:4)
        string(REPLACE ":" ";" maicord_reload_build "${maicord_reload_build}")
        list(GET maicord_reload_build 0 maicord_reload_letter)
        list(GET maicord_reload_build 1 maicord_reload_variant)
        string(TOUPPER ${maicord_reload_letter} maicord_reload_upper)
        maicord_plugin(MaiCordPluginReload${maicord_reload_upper}Fixture tests/fixtures/plugin_reload_fixture.cpp)
        target_compile_definitions(MaiCordPluginReload${maicord_reload_upper}Fixture PRIVATE
            RELOAD_VARIANT=${maicord_reload_variant})
        set_target_properties(MaiCordPluginReload${maicord_reload_upper}Fixture PROPERTIES
            OUTPUT_NAME reload_${maicord_reload_letter})
    endforeach()
    set(maicord_dx "${MAICORD_GENERATED_DIRECTORY}/dx")
    get_filename_component(maicord_dx_llvm "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(maicord_dx_tool "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/maicord-plugin/maicord_plugin.py")
    add_custom_command(OUTPUT "${maicord_dx}/stamp"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${maicord_dx}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${maicord_dx}"
        COMMAND ${maicord_dx_tool} new hello --dir "${maicord_dx}/hello"
        COMMAND ${maicord_dx_tool} build --dir "${maicord_dx}/hello" --llvm-bin "${maicord_dx_llvm}" --no-fetch
        COMMAND ${maicord_dx_tool} dev --dir "${maicord_dx}/hello" --dev-folder "${maicord_dx}/plugin-dev"
            --llvm-bin "${maicord_dx_llvm}" --no-fetch
        COMMAND ${maicord_dx_tool} effect new "Soft Rain" --dir "${maicord_dx}/soft-rain"
        COMMAND ${maicord_dx_tool} effect preview --dir "${maicord_dx}/soft-rain" --dev-folder "${maicord_dx}/effect-dev"
        COMMAND "${CMAKE_COMMAND}" -E touch "${maicord_dx}/stamp"
        DEPENDS tools/maicord-plugin/maicord_plugin.py tools/maicord-plugin/plugin_tool.py tools/maicord-plugin/mcfx_tool.py
            include/maicord/sdk/plugin.hpp include/maicord/sdk/maicord_plugin.h include/maicord/sdk/background.hlsli
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "maicord-plugin: new, build and dev a plugin, new and preview an effect"
        VERBATIM)
    add_custom_target(MaiCordDevToolFixtures DEPENDS "${maicord_dx}/stamp")
endif()
maicord_test(MaiCordPluginDevTests ONLY WINDOWS ENTRY maicord_plugin_dev_tests_entry
    SOURCES tests/app/plugins/plugin_dev.cpp src/app/plugins/catalog_service.cpp src/app/plugins/catalog_worker.cpp
        src/app/plugins/dev_console.cpp src/core/log.cpp ${maicord_plugin_fakes}
    LIBS maicord_core maicord_platform maicord_app_core maicord_plugin_catalog maicord_plugin_runtime
        maicord_protocol maicord_packages kernel32 bcrypt ws2_32 maicord_sspicli crypt32
    INCLUDES src/app
    DEFINES "MAICORD_DX_FOLDER=L\"${maicord_dx}\""
    DEPENDS MaiCordPluginReloadAFixture MaiCordPluginReloadBFixture MaiCordPluginReloadCFixture MaiCordPluginReloadDFixture
        MaiCordDevToolFixtures
    TEST maicord_plugin_dev)
# Nested rounded rectangles, clipped highlights and per-component padding
# across representative synthetic scenes.
maicord_test(MaiCordLayoutConsistencyTests ENTRY maicord_layout_consistency_tests_entry
    SOURCES tests/ui/shell/layout_consistency.cpp
    BUNDLE MaiCordAppUnitTests TIMEOUT 10 TEST maicord_layout_consistency)
# Window caption frame insets, hit testing across DPIs and button states.
maicord_test(MaiCordWindowCaptionTests ONLY WINDOWS ENTRY maicord_window_caption_tests_entry
    SOURCES tests/platform/desktop/window_caption.cpp
    BUNDLE MaiCordUiUnitTests TIMEOUT 10 TEST maicord_window_caption)
maicord_test(MaiCordSettingsRuntimeTests ONLY WINDOWS ENTRY maicord_settings_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/settings/settings_runtime.cpp tests/app/settings/settings_runtime_stubs.cpp
        tests/app/settings/custom_theme.cpp tests/app/settings/catalog_pack.cpp
        tests/app/plugins/navigation_runtime.cpp
        tests/app/settings/settings_choice_runtime.cpp tests/app/settings/user_settings_runtime.cpp
        tests/app/settings/account_settings_runtime.cpp tests/app/settings/keybinds_settings_runtime.cpp
        src/app/settings/user_settings.cpp src/app/settings/account_settings.cpp src/app/shell/state.cpp
        src/app/settings/test_mode.cpp
        src/app/settings/settings.cpp src/app/settings/settings_projection.cpp src/app/settings/settings_activation.cpp
        src/app/settings/settings_input.cpp src/app/settings/catalog.cpp src/app/settings/theme_editor.cpp
        src/app/notifications/desktop_settings.cpp src/app/effects/effects.cpp src/app/plugins/surfaces.cpp
        src/app/plugins/surface_host.cpp src/app/shell/input.cpp
        src/render/text/content_text_retention.cpp src/core/log.cpp src/app/update/updates.cpp
        src/app/voice/settings.cpp src/app/voice/settings_binding.cpp
        src/app/settings/keybinds_settings.cpp src/app/account/activity.cpp src/app/account/game_detection.cpp
        src/app/account/game_detection_system.cpp src/app/account/detectable_list.cpp
        tests/app/store/store_routing_runtime.cpp tests/app/store/store_runtime_stubs.cpp src/app/store/store_host.cpp
        src/app/store/store_source.cpp src/app/store/store_projection.cpp src/app/store/store_page.cpp
        src/app/store/store_detail.cpp src/app/store/store_dialogs.cpp src/app/store/store_act.cpp
        src/app/store/store_preview_pump.cpp src/app/store/store_preview_decode.cpp src/app/media/local_preview_thumbnail.cpp
        ${maicord_plugin_fake_ui} ${maicord_plugin_fake_actions}
    LIBS bcrypt ws2_32 maicord_sspicli maicord_compile_contract maicord_core maicord_window_null maicord_platform maicord_app_core
        maicord_ui maicord_client_preferences maicord_plugin_runtime
        maicord_message_media maicord_ui_input maicord_packages maicord_media_core maicord_webauthn kernel32 user32 shell32
        ole32 ntdll
    INCLUDES src/app
    DEPENDS MaiCordPluginUiFixture MaiCordPluginSurfacesFixture
    TEST maicord_settings_runtime)
maicord_test(MaiCordNavigationGapsTests ENTRY maicord_navigation_gaps_entry SOURCES tests/ui/shell/navigation_gaps.cpp
    BUNDLE MaiCordAppWaveTests TIMEOUT 10 TEST maicord_navigation_gaps)
# Server lifecycle, guild folders, channel reordering, channel list order,
# drag state machines, text in voice and the server menu policy.
maicord_test(MaiCordNavigationWaveTests ENTRY maicord_navigation_wave_entry SOURCES tests/app/gateway/navigation_wave.cpp
    BUNDLE MaiCordAppWaveTests TIMEOUT 10 TEST maicord_navigation_wave)
# Membership screening, onboarding and scheduled events.
maicord_test(MaiCordGuildCommunityTests ONLY WINDOWS ENTRY maicord_guild_community_entry
    SOURCES tests/protocol/guild/community.cpp
    BUNDLE MaiCordAppWaveTests TIMEOUT 10 TEST maicord_guild_community)
# Server banners: Gateway decode, CDN URLs, channel header geometry and
# animation, the list inset, header paint/text and the server tag popout.
maicord_test(MaiCordGuildBannerTests ENTRY maicord_guild_banner_entry SOURCES tests/ui/guild/banner.cpp
    BUNDLE MaiCordAppWaveTests TIMEOUT 10 TEST maicord_guild_banner)
# Rich presence, automatic idle, opcode 3, activity sharing, message requests
# and DM safety warnings.
maicord_test(MaiCordPresenceWaveTests ENTRY maicord_presence_wave_entry
    SOURCES tests/ui/widgets/presence_wave.cpp
    BUNDLE MaiCordAppWaveTests TIMEOUT 10 TEST maicord_presence_wave)
# Game detection over fake process snapshots: Discord's detectable entries, path, file-name and argument matching,
# launchers, exclusions, windowless processes, several games, per-game detection and the saved record. The presence
# sequence (shown, held, cleared, replaced) is in tests/app/account/activity_retry.cpp.
maicord_test(MaiCordGameDetectionTests ENTRY maicord_game_detection_entry
    SOURCES tests/app/account/game_detection.cpp src/app/account/game_detection.cpp src/app/account/detectable_list.cpp
    BUNDLE MaiCordAppWaveTests TIMEOUT 10 TEST maicord_game_detection)
# Profile rich presence cards: order, text, timers, progress, asset URLs and
# artwork fallback, popout and full profile layout, ticks, and the application
# icon and button metadata requests.
maicord_test(MaiCordProfileActivityTests ENTRY maicord_profile_activity_entry SOURCES tests/ui/account/profile_activity.cpp
    BUNDLE MaiCordAppWaveTests TIMEOUT 10 TEST maicord_profile_activity)
# Every profile shape (server tag, pronouns, theme colours, failed fetch, webhook) projects text
# commands the renderer accepts, in the popout and the full profile, on dark and light themes.
maicord_test(MaiCordProfileTextProjectionTests ENTRY maicord_profile_text_projection_entry
    SOURCES tests/ui/account/profile_text_projection.cpp
    BUNDLE MaiCordAppWaveTests TIMEOUT 10 TEST maicord_profile_text_projection)
# A large-account READY through the production projection and the app's
# message event pump; skipped-event isolation and diagnostics.
maicord_test(MaiCordReadyScalePumpTests ONLY WINDOWS ENTRY maicord_ready_scale_pump_entry
    SOURCES tests/model/user/ready_scale_pump.cpp
    LIBS ole32
    BUNDLE MaiCordAppUnitTests
    TIMEOUT 10 TEST maicord_ready_scale_pump)
maicord_test(MaiCordAccessibilityTests ONLY WINDOWS ENTRY maicord_accessibility_entry
    SOURCES tests/ui/accessibility/accessibility.cpp tests/ui/accessibility/accessibility_overlays.cpp
    LIBS maicord_uia ole32 oleaut32
    BUNDLE MaiCordAppUnitTests
    TIMEOUT 10 TEST maicord_accessibility)
# Queries a provider window through the system UI Automation client.
# Exclude the "uia" (or "display") label where UI Automation is unavailable.
maicord_add_tests(MaiCordAccessibilityTests TIMEOUT 10 LABELS uia display
    TEST maicord_accessibility_uia_probe --uia-probe)
maicord_test(MaiCordAccountPermissionStorageTests ONLY WINDOWS ENTRY maicord_account_permission_storage_entry
    SOURCES tests/app/guild/account_permission_storage.cpp src/app/guild/account_permission_storage.cpp
    BUNDLE MaiCordModelTests TEST maicord_account_permission_storage)
maicord_test(MaiCordHistoryDecodeStorageTests ONLY WINDOWS ENTRY maicord_history_decode_storage_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/chat/messages/history_decode_storage.cpp src/app/chat/messages/send.cpp src/app/chat/threads/actions.cpp
        src/app/chat/messages/edit.cpp src/app/shell/state.cpp src/core/log.cpp tests/support/pipeline_host_stubs.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_message_runtime maicord_ui
        maicord_model maicord_render_core kernel32 user32
    INCLUDES src/app
    TEST maicord_history_decode_storage)
maicord_test(MaiCordReactionHistoryRuntimeTests ONLY WINDOWS ENTRY maicord_application_reaction_history_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/shell/reaction_history_runtime.cpp src/app/shell/state.cpp src/core/log.cpp
        src/app/chat/threads/actions.cpp src/app/shell/startup_history.cpp
        src/app/shell/application_startup_shell.cpp tests/support/pipeline_host_stubs.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_message_runtime maicord_message_media
        maicord_ui maicord_model maicord_render_core kernel32 user32
    INCLUDES src/app
    TIMEOUT 15 TEST maicord_reaction_history_runtime)
maicord_test(MaiCordMessageReactionRuntimeTests ONLY WINDOWS ENTRY mainCRTStartup
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/chat/messages/reaction_runtime.cpp src/app/shell/state.cpp
        src/app/chat/messages/mutation_notice.cpp src/core/log.cpp
    LIBS ${maicord_app_ui_test_libs} kernel32 user32
    INCLUDES src/app
    TIMEOUT 15 TEST maicord_message_reaction_runtime)
maicord_test(MaiCordReactionPreferencesStorageTests ONLY WINDOWS ENTRY maicord_reaction_preference_storage_entry
    SOURCES tests/model/chat/reaction_preferences_storage.cpp
    BUNDLE MaiCordModelTests TEST maicord_reaction_preference_storage)
maicord_test(MaiCordRegionalFlagTests ONLY WINDOWS ENTRY maicord_regional_flag_test_entry
    SOURCES tests/ui/text/regional_flag_test.cpp
    LIBS maicord_cdn_media maicord_media_core
    BUNDLE MaiCordUiUnitTests
    TEST maicord_regional_flag_identity)
# Compile the alternate native geometry path without changing the shipping
# text-provider configuration or creating a second build tree.
maicord_test(MaiCordRegionalFlagNativeGeometryTests ENTRY maicord_flag_native_geometry_entry
    SOURCES tests/ui/text/flag_native_geometry.cpp src/ui/text/native_text_candidate.cpp
        src/ui/text/native_text_catalog.cpp src/ui/media/emoji_presentation.cpp
    LIBS maicord_core maicord_font maicord_media_core maicord_ui kernel32 advapi32 gdi32
    WINDOWS SOURCES src/core/log.cpp
    LINUX LIBS ${maicord_linux_app_test_libs}
    TIMEOUT 15 ENVIRONMENT LOCALAPPDATA= TEST maicord_regional_flag_native_geometry)
maicord_test(MaiCordRegionalFlagArtTests ONLY WINDOWS ENTRY maicord_regional_flag_art_test_entry
    SOURCES tests/media/image/regional_flag_art_test.cpp
    LIBS maicord_render_core maicord_paint_program
    BUNDLE MaiCordUiUnitTests
    TIMEOUT 15 TEST maicord_regional_flag_art)
maicord_test(MaiCordTextFormConfirmationTests ONLY WINDOWS ENTRY maicord_text_form_confirmation_entry
    SOURCES tests/app/chat/composer/text_form_confirmation_runtime.cpp tests/app/chat/application_link_trust_runtime.cpp
        src/app/shell/state.cpp src/app/chat/composer/text_form.cpp src/render/text/form_surface.cpp
        src/core/log.cpp
    LIBS maicord_core maicord_platform maicord_ui maicord_model maicord_app_core kernel32 user32
    INCLUDES src/app
    TEST maicord_text_form_confirmation)
maicord_test(MaiCordFriendsAccountTests ENTRY maicord_friends_account_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/account/account_relationships.cpp tests/protocol/http/rest_submit_hook.cpp src/app/account/account_relationships.cpp
        src/app/chat/messages/account_message_ack.cpp src/app/account/account_host.cpp
    BUNDLE MaiCordAccountRequestTests TEST maicord_friends_account)
maicord_test(MaiCordFriendsStorageTests ONLY WINDOWS ENTRY maicord_friends_storage_entry
    SOURCES tests/app/account/friends_storage.cpp src/app/account/friends_storage.cpp
    BUNDLE MaiCordModelTests TEST maicord_friends_storage)
maicord_test(MaiCordFriendsRuntimeTests ENTRY maicord_friends_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/account/friends_runtime.cpp src/app/shell/state.cpp src/app/account/friends_search.cpp
        src/app/account/friends_storage.cpp src/app/account/account_relationships.cpp
    INCLUDES src/app
    WINDOWS SOURCES src/core/log.cpp
        LIBS maicord_core maicord_window_null maicord_protocol maicord_model maicord_app_core maicord_ui kernel32 user32
    LINUX LIBS maicord_core maicord_window_null maicord_protocol maicord_model maicord_app_client maicord_ui
            ${maicord_linux_app_test_libs}
    TEST maicord_friends_runtime)
maicord_test(MaiCordVoiceTests ONLY WINDOWS ENTRY maicord_voice_tests_entry
    SOURCES tests/platform/crypto/aes_gcm.cpp tests/platform/crypto/aes_gcm64.cpp tests/platform/crypto/xchacha20_poly1305.cpp tests/app/voice/packet_crypto.cpp
        tests/app/voice/protocol_selection.cpp tests/app/voice/udp_media.cpp tests/app/voice/participants.cpp
        tests/app/voice/media_dispatch.cpp tests/protocol/voice/dave_replay.cpp tests/protocol/voice/dave_frame.cpp tests/app/voice/dave/frame_crypto.cpp
        tests/app/voice/dave/key_derivation.cpp tests/app/voice/mls/exporter.cpp tests/app/voice/mls/key_schedule.cpp tests/app/voice/mls/signature.cpp
        tests/app/voice/mls/hpke_derivation.cpp tests/app/voice/mls/hpke.cpp tests/protocol/voice/mls_objects.cpp
        tests/protocol/voice/mls_join_messages.cpp tests/app/voice/dave/key_package.cpp tests/app/voice/dave/welcome.cpp tests/app/voice/mls/dave_package_policy.cpp
        tests/platform/crypto/p256.cpp tests/platform/crypto/kdf_sha256.cpp tests/platform/crypto/random.cpp
        tests/platform/crypto/hash_stream.cpp tests/app/voice/dave/receive_epoch.cpp tests/app/voice/dave/receive_sender.cpp tests/voice_setup_entry.cpp
        tests/app/voice/connection_setup.cpp tests/app/gateway/voice_control.cpp tests/app/account/account_session_identity.cpp
        tests/protocol/voice/gateway.cpp tests/protocol/voice/heartbeat.cpp tests/protocol/voice/session.cpp tests/platform/net/tls_stream_lifecycle.cpp
        tests/app/voice/socket.cpp tests/app/voice/connection.cpp tests/protocol/voice/media_events.cpp tests/app/voice/udp.cpp
        tests/app/voice/udp_handoff.cpp tests/protocol/gateway/voice.cpp tests/model/voice/state_store.cpp tests/ui/voice/channel_rows.cpp
        tests/protocol/voice/channel_info.cpp tests/protocol/voice/gateway_policy.cpp tests/app/voice/dave/session.cpp tests/app/voice/video_rtp_receive.cpp
        tests/app/voice/jitter_buffer.cpp tests/app/voice/playback.cpp tests/app/voice/receive.cpp tests/app/voice/call_view.cpp
        tests/app/voice/stream_watch.cpp tests/protocol/guild/stage_channel.cpp tests/protocol/voice/video_signaling.cpp tests/app/voice/dave/video_frames.cpp
        tests/app/voice/video_sink.cpp tests/protocol/gateway/stream.cpp tests/app/voice/send.cpp tests/app/voice/preferences.cpp
        tests/ui/voice/go_live_scene.cpp tests/app/voice/go_live_send.cpp tests/app/voice/go_live_thumbnails.cpp
        src/app/voice/settings_binding.cpp src/core/log.cpp tests/support/voice_log_sink.cpp
    LIBS maicord_core maicord_protocol maicord_platform maicord_app_core maicord_model kernel32 crypt32 ws2_32
        maicord_sspicli bcrypt maicord_ui maicord_media_codec ole32 maicord_client_preferences user32
    INCLUDES src/app
    TEST maicord_voice_setup)
# The linked account module's entries without an owner: they refuse before creating an account, login, worker or
# transport. The tests that fake the module link the account fake instead. The module comes before maicord_app_core so
# that its Go Live producers, not the library's inactive ones, are the ones the Gateway client links.
maicord_test(MaiCordAccountModuleTests ONLY WINDOWS ENTRY maicord_account_module_entry
    SOURCES tests/app/account/account_module_contract.cpp tests/app/account/account_module_dormant.cpp src/core/log.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_account_module maicord_account_host maicord_app_core maicord_model
        maicord_message_runtime maicord_message_media maicord_render_core maicord_media_core maicord_media_codec
        maicord_cdn_media maicord_account_media bcrypt kernel32 user32 ${maicord_tls_libs}
    INCLUDES src/app
    TIMEOUT 15 TEST maicord_account_module)
maicord_test(MaiCordTests ENTRY maicord_tests_entry
    WINDOWS SOURCES tests/test_main.cpp tests/model/chat/core_stores.cpp tests/ui/core/core_scene.cpp
            tests/ui/chat/messages/core_messages.cpp tests/app/chat/composer/core_composer.cpp
            tests/app/account/login/core_login.cpp tests/app/chat/messages/core_timeline.cpp
            tests/app/chat/messages/retained_messages.cpp tests/media/image/core_codecs.cpp
            tests/app/media/core_media.cpp tests/protocol/gateway/core_gateway.cpp
            tests/protocol/http/core_transport.cpp tests/core/core_runtime.cpp tests/app/account/core_account.cpp
            tests/app/account/login/account_credential_forget.cpp tests/app/account/account_logout_stop.cpp
            tests/app/account/account_inventory.cpp ${maicord_account_fake}
            tests/app/media/parity.cpp tests/app/media/catalog_media.cpp tests/ui/chat/messages/preparation.cpp
            tests/render/media/atlas_pressure.cpp tests/ui/shell/composer_geometry.cpp
            tests/ui/chat/composer/typing_strip.cpp
            tests/ui/chat/messages/reply_grouping.cpp tests/ui/chat/messages/markdown_cache.cpp
            tests/app/chat/messages/interaction_rules.cpp tests/ui/chat/timeline_overlay_regressions.cpp
            tests/ui/chat/messages/chat_display.cpp tests/app/chat/messages/live_updates.cpp
            tests/ui/chat/messages/embed_colors.cpp tests/ui/text/ansi_code_blocks.cpp
            tests/ui/chat/messages/components.cpp tests/ui/chat/messages/content_wave.cpp
            tests/ui/text/embed_convergence.cpp tests/ui/chat/messages/embed_parity.cpp
            tests/ui/chat/messages/embed_parity_embeds.cpp tests/ui/chat/messages/link_chips.cpp
            tests/ui/chat/messages/plugin_dispatch.cpp tests/app/chat/messages/history_presence_recovery.cpp
            tests/ui/account/profile_appearance.cpp tests/protocol/user/profile_decode_scratch.cpp
            tests/protocol/gateway/message_rich_index.cpp tests/app/net/cdn_response_storage.cpp
            tests/protocol/http/rest_response_storage.cpp tests/model/chat/engineering_regressions.cpp
            tests/media/compression/deflate_matches.cpp tests/media/compression/deflate_differential.cpp
            tests/media/compression/deflate_reference.cpp tests/render/text/shaped_text_cache.cpp
            src/render/text/shaped_text_cache.cpp tests/app/media/animation_modal_pause.cpp
            tests/core/mailbox_release_race.cpp tests/core/mailbox_acquire_release.cpp
            tests/app/gateway/responsiveness.cpp tests/app/gateway/ready_users.cpp tests/core/json_string_decode.cpp
            tests/core/json_reader_visit.cpp tests/core/json_scan.cpp tests/core/clock.cpp
            tests/app/shell/owner_pages.cpp tests/platform/net/resolver_publication.cpp
            tests/app/media/worker_publication.cpp tests/media/image/png_crc.cpp tests/app/shell/frame_rate.cpp
            tests/ui/core/layout_anchors.cpp tests/ui/account/widget_input.cpp tests/ui/text/form.cpp
            tests/app/chat/threads/forum_form.cpp tests/ui/text/secure_edit_history.cpp
            tests/ui/text/composition_history.cpp tests/ui/text/input_runs.cpp tests/ui/layout/widget_scroll.cpp
            tests/ui/core/input_router.cpp tests/app/shell/navigation_shortcuts.cpp
            tests/app/shell/shortcut_registry.cpp tests/ui/layout/list_paging.cpp tests/ui/widgets/button_keyboard.cpp
            tests/protocol/media/attachment_upload.cpp tests/app/chat/composer/composer_attachments.cpp
            tests/ui/chat/composer/attachment_strip.cpp tests/media/image/composer_thumbnails.cpp
            tests/app/chat/composer/preview_owner.cpp tests/app/chat/composer/history.cpp
            tests/app/chat/composer/commands.cpp tests/app/chat/composer/command_autocomplete.cpp
            tests/model/guild/composer_mentions.cpp tests/protocol/messages/command_argument_editing.cpp
            tests/ui/account/auth_transition.cpp tests/protocol/auth/account_requirement.cpp
            tests/media/compression/http_compression_workers.cpp tests/app/account/client_request_metadata.cpp
            tests/app/account/client_identity.cpp tests/protocol/messages/composer_edit_menu.cpp
            tests/ui/chat/composer/spellcheck.cpp src/app/chat/composer/edit_menu.cpp
            tests/app/chat/messages/send_response.cpp tests/platform/net/file.cpp tests/platform/net/worker_async.cpp
            tests/platform/desktop/file_picker.cpp tests/platform/desktop/clipboard_attachment.cpp
            tests/media/image/image_decode_corpus.cpp tests/ui/media/image_viewer.cpp
            tests/app/shell/history_scrollback.cpp tests/app/chat/messages/timeline_window.cpp
            tests/app/chat/messages/snapshot_cache.cpp src/app/shell/scrolling.cpp src/app/chat/read_state.cpp
            src/app/chat/scoped_read.cpp src/app/chat/messages/mutation_notice.cpp tests/app/shell/pane_resize.cpp
            src/app/shell/pane_resize.cpp tests/platform/desktop/file_drop.cpp
            tests/app/chat/composer/attachment_drafts.cpp tests/app/chat/composer/application_attachments.cpp
            src/app/media/upload_lane.cpp src/core/log.cpp src/app/chat/composer/attachments.cpp
            src/app/chat/composer/preview.cpp src/app/media/attachment_probe.cpp
            src/app/media/attachment_media_probe.cpp src/app/media/attachment_allowance.cpp src/app/shell/state.cpp
            tests/app/gateway/private_channel_state.cpp tests/app/gateway/private_membership.cpp
            tests/app/gateway/private_snapshot.cpp tests/app/gateway/state_recovery.cpp
            tests/app/chat/messages/navigation_restore.cpp tests/app/chat/channel_memory.cpp
            tests/app/shell/startup_shell.cpp tests/app/gateway/private_order.cpp tests/app/shell/private_rail_pins.cpp
            tests/protocol/user/private_conversation.cpp tests/app/chat/private_channel_actions.cpp
            tests/support/media_fixture.cpp tests/support/media_fixture_encoders.cpp ${maicord_plugin_fakes}
        LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_webauthn maicord_account_paths
            maicord_account_host maicord_text maicord_ui maicord_ui_input maicord_model maicord_message_runtime
            maicord_message_media maicord_render_core maicord_media_core maicord_media_codec maicord_cdn_media
            maicord_account_media maicord_plugin_runtime bcrypt kernel32 user32 ${maicord_tls_libs}
        INCLUDES src/app
    LINUX SOURCES tests/support/test_main_linux.cpp tests/core/core_runtime.cpp tests/core/json_scan.cpp
            tests/core/clock.cpp tests/core/json_reader_visit.cpp tests/core/json_string_decode.cpp
            tests/core/mailbox_release_race.cpp tests/core/mailbox_acquire_release.cpp
            tests/media/compression/deflate_matches.cpp tests/media/image/png_crc.cpp
            tests/media/image/image_decode_corpus.cpp tests/protocol/gateway/message_rich_index.cpp
            tests/protocol/messages/command_argument_editing.cpp tests/protocol/media/attachment_upload.cpp
            tests/protocol/user/profile_decode_scratch.cpp tests/media/compression/deflate_differential.cpp
            tests/media/compression/deflate_reference.cpp tests/media/image/core_codecs.cpp
            tests/support/media_fixture.cpp tests/support/media_fixture_encoders.cpp tests/app/shell/owner_pages.cpp
        LIBS maicord_core maicord_compression maicord_protocol maicord_media_core maicord_media_codec maicord_model
    TEST maicord_core)
maicord_test(MaiCordCommandUiTests ENTRY maicord_command_ui_tests_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/chat/composer/commands_ui.cpp tests/app/chat/composer/commands_ui_cases.cpp
        tests/app/chat/composer/commands_ui_plugin.cpp src/app/chat/composer/commands.cpp
        src/app/chat/composer/commands_frecency.cpp src/app/chat/composer/commands_picker.cpp
        src/app/chat/composer/commands_draft.cpp src/app/chat/composer/builtin_commands.cpp
        src/app/chat/composer/plugin_commands.cpp src/app/shell/state.cpp src/app/settings/modal.cpp
        src/app/chat/messages/feedback.cpp tests/app/account/activity_retry.cpp src/app/account/game_detection.cpp
        src/app/account/detectable_list.cpp
        tests/app/account/custom_status_retry.cpp tests/app/notifications/retry.cpp tests/app/guild/community_retry.cpp
        tests/app/guild/invite_retry.cpp ${maicord_plugin_fakes}
    INCLUDES src/app
    DEPENDS MaiCordPluginUiFixture
    WINDOWS SOURCES src/core/log.cpp
        LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_text maicord_ui maicord_ui_input
            maicord_model maicord_message_runtime maicord_plugin_runtime bcrypt kernel32 user32 ${maicord_tls_libs}
    LINUX LIBS ${maicord_linux_app_test_libs}
    TEST maicord_command_ui)
maicord_test(MaiCordMediaVisibilityTests ONLY WINDOWS ENTRY maicord_media_visibility_tests_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/media/media_visibility.cpp tests/app/media/media_visibility_search.cpp
        tests/app/guild/stream_preview_media_stub.cpp src/app/media/media.cpp
        src/app/media/animation.cpp src/app/media/requests.cpp src/app/media/requests_message.cpp src/app/shell/state.cpp
        src/core/log.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_text maicord_ui maicord_ui_input
        maicord_model maicord_message_runtime maicord_message_media maicord_render_core maicord_media_core
        maicord_account_media maicord_atlas_gpu maicord_renderer kernel32 user32 ${maicord_tls_libs} d3d11 dxgi dcomp
    INCLUDES src/app
    TEST maicord_media_visibility)
if(NOT MAICORD_LINUX)
    add_executable(MaiCordAccountPumpTests tests/app/account/account_pump.cpp tests/app/account/account_pump_stubs.cpp
        src/app/chat/threads/header_activation.cpp
        tests/app/account/application_account_text_retry_runtime.hpp
        src/app/account/account_pump.cpp src/app/shell/scheduler.cpp src/app/chat/application_channel_memory.cpp
        src/app/account/account_host.cpp src/app/shell/state.cpp src/core/log.cpp
        src/app/chat/messages/mutation_notice.cpp ${maicord_account_fake}
        $<TARGET_OBJECTS:maicord_runtime>)
    target_link_libraries(MaiCordAccountPumpTests PRIVATE
        maicord_compile_contract maicord_core maicord_platform maicord_protocol maicord_app_core
        maicord_text maicord_ui maicord_ui_input maicord_model maicord_message_runtime maicord_media_core
        kernel32 user32 ws2_32 maicord_sspicli bcrypt crypt32)
    target_link_options(MaiCordAccountPumpTests PRIVATE
        /ENTRY:maicord_account_pump_tests_entry /SUBSYSTEM:CONSOLE,6.02)
    maicord_add_tests(MaiCordAccountPumpTests TEST maicord_account_pump)
endif()
maicord_test(MaiCordSurfaceMotionTests ONLY WINDOWS ENTRY maicord_surface_motion_entry
    SOURCES tests/app/shell/surface_motion.cpp src/app/shell/surface_motion.cpp
    LIBS maicord_core maicord_ui kernel32
    INCLUDES src/app
    DEFINES MAICORD_SURFACE_MOTION_CONTROLLER_TEST=1)
maicord_test(MaiCordChannelOverviewRuntimeTests ONLY WINDOWS ENTRY maicord_application_channel_overview_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/guild/channel_overview_runtime.cpp src/app/settings/layer.cpp
        src/app/settings/modal.cpp
        src/app/shell/state.cpp src/core/log.cpp
    LIBS ${maicord_headless_ui_test_libs} kernel32 user32 shell32
    INCLUDES src/app
    TIMEOUT 15 TEST maicord_channel_overview_runtime)
maicord_test(MaiCordChannelOverviewAccountTests ENTRY maicord_channel_overview_account_entry
    SOURCES tests/app/guild/account_channel_overview.cpp tests/protocol/http/rest_submit_hook.cpp src/app/guild/account_channel_overview.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 15 TEST maicord_channel_overview_account)
maicord_test(MaiCordPermissionComputeTests ENTRY maicord_permission_compute_entry SOURCES tests/model/guild/permission_compute.cpp
    BUNDLE MaiCordModelTests TIMEOUT 10 TEST maicord_permission_compute)
maicord_test(MaiCordCookieJarTests ENTRY maicord_cookie_jar_entry SOURCES tests/protocol/http/cookie_jar.cpp
    BUNDLE MaiCordModelTests TIMEOUT 10 TEST maicord_cookie_jar)
maicord_test(MaiCordUserSettingsTests ENTRY maicord_user_settings_tests_entry SOURCES tests/protocol/user/settings_proto.cpp
    BUNDLE MaiCordProtocolTests
    TIMEOUT 10 TEST maicord_user_settings)
# Registering a security key: the creation challenge, its client data and the credential JSON the request carries.
maicord_test(MaiCordWebAuthnRegistrationTests ENTRY maicord_webauthn_registration_entry
    SOURCES tests/protocol/auth/webauthn_registration.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 10 TEST maicord_webauthn_registration)
maicord_test(MaiCordGuildSettingsTests ENTRY maicord_guild_settings_entry SOURCES tests/model/guild/settings_model.cpp
    BUNDLE MaiCordModelTests TIMEOUT 10 TEST maicord_guild_settings)
# Access, Enable Community / Community Overview and Server Template: the member-verification, template and discovery
# requests and replies, the guild PATCH's community fields, and the Access form and community drafts.
maicord_test(MaiCordCommunityAdminTests ENTRY maicord_community_admin_entry SOURCES tests/protocol/guild/community_admin.cpp
    BUNDLE MaiCordModelTests TIMEOUT 10 TEST maicord_community_admin)
# Server Tag: the tag fields of the server profile (decode, draft, badge catalogue, validation) and the exact tag
# PATCH and PUT /users/@me/clan requests.
maicord_test(MaiCordServerTagTests ENTRY maicord_server_tag_entry SOURCES tests/model/guild/server_tag.cpp
    BUNDLE MaiCordModelTests TIMEOUT 10 TEST maicord_server_tag)
# Game mentions (<@$id>): the syntax, the game cache and search, the composer's @game token, the markdown chip, the
# GET /games and /games/autocomplete targets and the game profile's details.
maicord_test(MaiCordGameMentionsTests ENTRY maicord_game_mentions_tests_entry SOURCES tests/model/chat/game_mentions.cpp
    BUNDLE MaiCordAppUnitTests TIMEOUT 10 TEST maicord_game_mentions)
# Role manager: tri-state permissions and switch, role reorder and
# hierarchy, View Server as Role and the role editor's locks and drafts.
maicord_test(MaiCordRoleManagerTests ONLY WINDOWS ENTRY maicord_role_manager_entry
    SOURCES tests/model/guild/role_manager.cpp
    BUNDLE MaiCordModelTests TIMEOUT 10 TEST maicord_role_manager)
# Members search, bulk ban and ban search: exact bodies, targets, replies,
# paging and the transport.
maicord_test(MaiCordGuildModerationTests ONLY WINDOWS ENTRY maicord_guild_moderation_entry
    SOURCES tests/protocol/guild/moderation.cpp tests/protocol/http/rest_submit_hook.cpp src/app/guild/account_guild_moderation.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 10 TEST maicord_guild_moderation)
# Integrations, webhooks, command permissions and AutoMod rules: every route,
# method and body, the limits, the keyword boxes, the replies and the transport.
maicord_test(MaiCordGuildAutomationTests ONLY WINDOWS ENTRY maicord_guild_automation_entry
    SOURCES tests/protocol/guild/automation.cpp tests/protocol/http/rest_submit_hook.cpp src/app/guild/account_guild_automation.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 10 TEST maicord_guild_automation)
maicord_test(MaiCordGuildAdminAccountTests ENTRY maicord_account_guild_admin_entry
    SOURCES tests/app/guild/account_guild_admin.cpp tests/protocol/http/rest_submit_hook.cpp src/app/guild/account_guild_admin.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 15 TEST maicord_guild_admin_account)
# Stage requests: exact targets, methods, buckets and bodies at the transport.
maicord_test(MaiCordStageAccountTests ENTRY maicord_account_stage_entry
    SOURCES tests/app/voice/account_stage.cpp tests/protocol/http/rest_submit_hook.cpp src/app/voice/account_stage.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 10 TEST maicord_stage_account)
# Soundboard: effect/sound decoding, request bodies, gating, upload checks,
# the clip cache and mixer, and the module's fetch-decode-play path (stubs).
maicord_test(MaiCordSoundboardTests ONLY WINDOWS ENTRY maicord_soundboard_entry
    SOURCES tests/protocol/guild/soundboard.cpp src/app/voice/account_soundboard.cpp src/app/voice/soundboard_audio.cpp src/app/voice/mixer.cpp
    LIBS maicord_core maicord_protocol maicord_media_codec kernel32
    TIMEOUT 10 TEST maicord_soundboard)
# Notes, report menus/submissions and message flags: exact targets and bodies.
maicord_test(MaiCordUserSocialAccountTests ENTRY maicord_account_user_social_entry
    SOURCES tests/app/account/account_user_social.cpp tests/protocol/http/rest_submit_hook.cpp src/app/account/account_user_social.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 10 TEST maicord_user_social_account)
maicord_test(MaiCordGuildAssetsTests ONLY WINDOWS ENTRY maicord_guild_assets_entry
    SOURCES tests/protocol/guild/assets.cpp tests/protocol/http/rest_submit_hook.cpp src/app/guild/account_guild_assets.cpp src/app/media/guild_image.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 15 TEST maicord_guild_assets)
# Avatar/banner crop: cropper geometry, PNG/zlib encoder, still and GIF crops.
maicord_test(MaiCordProfileImageTests ENTRY maicord_profile_image_entry
    SOURCES tests/app/media/profile_image.cpp tests/support/media_fixture.cpp tests/support/media_fixture_encoders.cpp
        src/app/media/profile_image.cpp src/app/media/guild_image.cpp src/app/media/local_preview_thumbnail.cpp
    WINDOWS LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_media_codec maicord_compression
            kernel32 user32
    LINUX LIBS maicord_app_client maicord_core maicord_window_null maicord_desktop_system maicord_dbus maicord_protocol
            maicord_model maicord_media_codec maicord_compression ${maicord_linux_app_test_libs}
    TIMEOUT 10 TEST maicord_profile_image)
# Store pictures: the tile's pixel width, the atlas identity and the PNG reduction.
maicord_test(MaiCordStorePreviewTests ENTRY maicord_store_preview_entry
    SOURCES tests/app/store/store_preview.cpp tests/support/media_fixture.cpp tests/support/media_fixture_encoders.cpp
        src/app/store/store_preview_decode.cpp src/app/media/local_preview_thumbnail.cpp
    WINDOWS LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_media_codec maicord_compression
            kernel32 user32
    LINUX LIBS maicord_app_client maicord_core maicord_window_null maicord_desktop_system maicord_dbus maicord_protocol
            maicord_model maicord_media_codec maicord_compression ${maicord_linux_app_test_libs}
    TIMEOUT 10 TEST maicord_store_preview)
# Profile editor: PATCH bodies at the transport, loads, gates, drafts, errors.
maicord_test(MaiCordProfileEditorTests ONLY WINDOWS ENTRY maicord_profile_editor_entry
    SOURCES tests/model/user/profile_editor.cpp tests/protocol/http/rest_submit_hook.cpp src/app/account/account_profile_edit.cpp
        src/app/media/profile_image.cpp src/app/media/guild_image.cpp src/app/media/local_preview_thumbnail.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 10 TEST maicord_profile_editor)
maicord_test(MaiCordMediaWorkerPosterLeaseTests ONLY WINDOWS ENTRY maicord_media_worker_poster_lease_entry
    SOURCES tests/app/media/worker_poster_lease.cpp
    LIBS maicord_account_media ole32
    BUNDLE MaiCordCodecUnitTests
    TIMEOUT 10 TEST maicord_media_worker_poster_lease)
# Media decode lanes: threads start on first use; lane stacks keep a margin
# over the deepest decode (every image codec and the largest bundled flag).
maicord_test(MaiCordMediaWorkerLanesTests ONLY WINDOWS ENTRY maicord_media_worker_lanes_entry
    SOURCES tests/app/media/worker_lanes.cpp tests/support/media_fixture.cpp tests/support/media_fixture_encoders.cpp
    LIBS maicord_account_media
    BUNDLE MaiCordCodecUnitTests
    TIMEOUT 10 TEST maicord_media_worker_lanes)
# Profile effect layers the CDN cannot size: the decode's area-average fit to the effect slot, still and animated.
maicord_test(MaiCordProfileMediaFitTests ENTRY maicord_profile_media_fit_entry
    SOURCES tests/app/media/profile_media_fit.cpp tests/support/media_fixture.cpp
        tests/support/media_fixture_encoders.cpp
    WINDOWS LIBS maicord_account_media
        BUNDLE MaiCordCodecUnitTests
    LINUX SOURCES tests/platform/audio/window_post_stub.cpp
        LIBS maicord_video_decode maicord_aac_decoder maicord_audio_pulse maicord_gpu_gles_video
            maicord_gpu_gles_shaders maicord_gpu_gles maicord_video_software maicord_paint_program
            ${maicord_linux_app_test_libs}
    TIMEOUT 10 TEST maicord_profile_media_fit)
if(MAICORD_LINUX)
    # The GPU VP8 gate: a device with the 128 invocations per group the specification guarantees has no GPU VP8 path, and
    # lossy WebP stills and animation frames decode on the CPU into RGBA8 (src/gpu/gles/README.md).
    maicord_test(MaiCordVp8CpuRouteTests ENTRY maicord_vp8_cpu_route_entry
        SOURCES tests/app/media/vp8_cpu_route.cpp tests/support/media_fixture.cpp tests/support/media_fixture_encoders.cpp
        LIBS maicord_gpu_gles_shaders maicord_gpu_gles ${maicord_linux_app_test_libs}
        TIMEOUT 20 ENV MAICORD_GLES_MAX_COMPUTE_INVOCATIONS=128 TEST maicord_vp8_cpu_route)
endif()
maicord_test(MaiCordGuildAdminGatewayTests ONLY WINDOWS ENTRY maicord_guild_admin_gateway_entry
    SOURCES tests/protocol/guild/admin_gateway.cpp
    BUNDLE MaiCordAppUnitTests TIMEOUT 15 TEST maicord_guild_admin_gateway)
maicord_add_tests(MaiCordSurfaceMotionTests TIMEOUT 10 TEST maicord_surface_motion)
maicord_test(MaiCordSurfaceMotionHostTests ONLY WINDOWS ENTRY maicord_surface_motion_host_entry
    SOURCES tests/app/shell/surface_motion_host.cpp src/app/shell/surface_motion.cpp
        src/app/shell/state.cpp src/render/d3d11/layer_presentation.cpp
    LIBS maicord_core maicord_ui maicord_render_core kernel32 user32
    INCLUDES src/app
    TIMEOUT 10 TEST maicord_surface_motion_host)
# Theme glides (src/app/settings/theme_publication.cpp): one publication per display frame, an exact settle and no
# frames after it; reduced motion and a window that can't show it take a change at once.
maicord_test(MaiCordThemePublicationTests ONLY WINDOWS ENTRY maicord_theme_publication_entry
    SOURCES tests/app/settings/theme_publication.cpp src/app/shell/state.cpp
    LIBS maicord_core maicord_ui maicord_client_preferences kernel32 user32
    INCLUDES src/app
    TIMEOUT 10 TEST maicord_theme_publication)
# Draw submission: pipeline, shared-state and scissor binds, merged draw runs.
maicord_test(MaiCordDrawSubmissionTests ONLY WINDOWS ENTRY maicord_draw_submission_entry
    SOURCES tests/render/d3d11/draw_submission.cpp
    BUNDLE MaiCordCoreUnitTests TIMEOUT 5 TEST maicord_draw_submission)
maicord_test(MaiCordTooltipRuntimeTests ONLY WINDOWS ENTRY maicord_tooltip_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/shell/tooltip_runtime.cpp src/app/shell/tooltip.cpp src/app/shell/state.cpp
        tests/app/chat/messages/reaction_tooltip_runtime.cpp tests/app/media/emoji_tooltip_runtime.cpp
        tests/app/chat/messages/edited_tooltip_runtime.cpp src/core/log.cpp
    LIBS ${maicord_headless_test_libs} maicord_message_runtime kernel32 user32
    INCLUDES src/app
    TIMEOUT 10 TEST maicord_tooltip_runtime)
maicord_test(MaiCordAccountMenuRuntimeTests ONLY WINDOWS ENTRY maicord_account_menu_runtime_entry
    SOURCES tests/app/account/account_menu_runtime.cpp src/app/account/account_menu.cpp
        src/app/shell/state.cpp src/core/log.cpp
    LIBS ${maicord_headless_test_libs} maicord_message_runtime kernel32 user32
    INCLUDES src/app
    TIMEOUT 10 TEST maicord_account_menu_runtime)
maicord_test(MaiCordDmProfileTests ENTRY maicord_dm_profile_tests_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/ui/account/dm_profile_panel.cpp src/app/account/dm_profile.cpp src/app/chat/private_header.cpp
        src/app/shell/state.cpp
    BUNDLE MaiCordAppUnitTests
    WINDOWS SOURCES src/core/log.cpp
    TIMEOUT 10 TEST maicord_dm_profile)
maicord_test(MaiCordProfileRuntimeTests ONLY WINDOWS ENTRY maicord_profile_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/account/profile_runtime.cpp src/app/account/profile.cpp src/app/shell/state.cpp
        src/core/log.cpp
    LIBS ${maicord_app_test_libs} maicord_message_runtime kernel32 user32 bcrypt
    INCLUDES src/app
    TIMEOUT 10 TEST maicord_profile_runtime)
maicord_test(MaiCordAccountMessageSearchTests ENTRY maicord_account_message_search_test_entry
    SOURCES tests/app/chat/search/account_message_search.cpp tests/protocol/http/rest_submit_hook.cpp src/app/chat/search/account_message_search.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 10 TEST maicord_account_message_search)
maicord_test(MaiCordAccountPinnedMessagesTests ENTRY maicord_account_pinned_messages_test_entry
    SOURCES tests/app/chat/messages/account_pinned_messages.cpp tests/protocol/http/rest_submit_hook.cpp src/app/chat/messages/account_pinned_messages.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 10 TEST maicord_account_pinned_messages)
maicord_test(MaiCordAccountThreadActionsTests ENTRY maicord_account_thread_actions_test_entry
    SOURCES tests/app/chat/threads/account_thread_actions.cpp tests/protocol/http/rest_submit_hook.cpp src/app/chat/threads/account_thread_actions.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 15 TEST maicord_account_thread_actions)
maicord_test(MaiCordThreadBrowserProtocolTests ENTRY maicord_thread_browser_protocol_test_entry
    SOURCES tests/protocol/messages/thread_browser_protocol.cpp
    LIBS maicord_model
    BUNDLE MaiCordProtocolTests
    TIMEOUT 15 TEST maicord_thread_browser_protocol)
maicord_test(MaiCordAccountThreadBrowserTests ENTRY maicord_account_thread_browser_test_entry
    SOURCES tests/app/chat/threads/account_thread_browser.cpp tests/protocol/http/rest_submit_hook.cpp src/app/chat/threads/account_thread_browser.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 15 TEST maicord_account_thread_browser)
maicord_test(MaiCordApplicationThreadBrowserTests ENTRY maicord_application_thread_browser_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/chat/threads/browser.cpp
    INCLUDES src/app
    WINDOWS LIBS ${maicord_headless_ui_test_libs} kernel32 user32
    LINUX LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client
            maicord_ui maicord_ui_secure_text maicord_text_system ${maicord_linux_app_test_libs}
    TIMEOUT 15 TEST maicord_application_thread_browser)
maicord_test(MaiCordApplicationPollTests ONLY WINDOWS ENTRY maicord_application_poll_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/chat/polls/poll.cpp
    LIBS ${maicord_app_ui_test_libs} kernel32 user32 shell32
    INCLUDES src/app
    TIMEOUT 15 TEST maicord_application_poll)
maicord_test(MaiCordThreadCreationProtocolTests ENTRY maicord_thread_creation_protocol_test_entry
    SOURCES tests/protocol/messages/thread_creation_protocol.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 15 TEST maicord_thread_creation_protocol)
maicord_test(MaiCordAccountThreadCreationTests ENTRY maicord_account_thread_creation_test_entry
    SOURCES tests/app/chat/threads/account_thread_creation.cpp tests/protocol/http/rest_submit_hook.cpp src/app/chat/threads/account_thread_creation.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 15 TEST maicord_account_thread_creation)
maicord_test(MaiCordAccountPollCreateTests ENTRY maicord_account_poll_create_test_entry
    SOURCES tests/app/chat/polls/account_poll_create.cpp tests/protocol/http/rest_submit_hook.cpp src/app/chat/polls/account_poll_create.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 15 TEST maicord_account_poll_create)
maicord_test(MaiCordAccountNotificationSettingsTests ENTRY maicord_account_notification_settings_test_entry
    SOURCES tests/app/notifications/account_notification_settings.cpp tests/protocol/http/rest_submit_hook.cpp src/app/notifications/account_notification_settings.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 15 TEST maicord_account_notification_settings)
maicord_test(MaiCordAccountPrivateChannelTests ENTRY maicord_account_private_channel_test_entry
    SOURCES tests/app/chat/account_private_channel.cpp tests/protocol/http/rest_submit_hook.cpp src/app/chat/account_private_channel.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 15 TEST maicord_account_private_channel)
maicord_test(MaiCordApplicationThreadCreationTests ENTRY maicord_application_thread_creation_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/chat/threads/creation.cpp
    INCLUDES src/app
    WINDOWS LIBS ${maicord_app_ui_test_libs} kernel32 user32
    LINUX LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client
            maicord_ui maicord_ui_secure_text maicord_text_system ${maicord_linux_app_test_libs}
    TIMEOUT 15 TEST maicord_application_thread_creation)
maicord_test(MaiCordApplicationThreadActionsRuntimeTests ONLY WINDOWS
    SOURCES ${maicord_account_fake}
    ENTRY maicord_application_thread_actions_runtime_entry
    SOURCES tests/app/chat/threads/actions_runtime.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_app_core kernel32
    INCLUDES src/app)
maicord_test(MaiCordForumPostProtocolTests ENTRY maicord_forum_post_protocol_test_entry
    SOURCES tests/protocol/messages/forum_post_protocol.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 15 TEST maicord_forum_post_protocol)
maicord_test(MaiCordAccountForumPostTests ENTRY maicord_account_forum_post_test_entry
    SOURCES tests/app/chat/threads/account_forum_post.cpp tests/protocol/http/rest_submit_hook.cpp src/app/chat/threads/account_forum_post.cpp
    BUNDLE MaiCordAccountRequestTests TIMEOUT 15 TEST maicord_account_forum_post)
maicord_test(MaiCordApplicationForumAttachmentsTests ONLY WINDOWS ENTRY maicord_application_forum_attachments_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/chat/threads/forum_attachments.cpp
    LIBS ${maicord_headless_ui_test_libs} kernel32 user32 shell32
    INCLUDES src/app
    TIMEOUT 20
    TEST maicord_application_forum_post --forum-post
    TEST maicord_application_forum_attachments)
maicord_test(MaiCordApplicationForumPreviewTests ENTRY maicord_application_forum_preview_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/chat/threads/forum_preview.cpp
    INCLUDES src/app
    WINDOWS LIBS ${maicord_app_ui_test_libs} maicord_render_core kernel32 user32
    LINUX LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client
            maicord_ui maicord_ui_secure_text maicord_text_system maicord_render_core ${maicord_linux_app_test_libs}
    TIMEOUT 15 TEST maicord_application_forum_preview)
maicord_test(MaiCordApplicationContentNavigationTests ONLY WINDOWS ENTRY maicord_application_content_navigation_entry
    SOURCES tests/app/shell/content_navigation.cpp src/core/log.cpp
    LIBS ${maicord_app_ui_test_libs} maicord_render_core kernel32 user32 shell32
    INCLUDES src/app
    TIMEOUT 15 TEST maicord_application_content_navigation)
maicord_add_tests(MaiCordApplicationThreadActionsRuntimeTests TIMEOUT 15
    TEST maicord_application_thread_actions_runtime)
maicord_test(MaiCordMessagePinnedStateTests ENTRY maicord_message_pinned_state_entry
    SOURCES tests/app/gateway/message_pinned_state.cpp
    BUNDLE MaiCordAppUnitTests TIMEOUT 15 TEST maicord_message_pinned_state)
maicord_test(MaiCordForwardedTextContractTests ONLY WINDOWS ENTRY maicord_forwarded_text_contract_entry
    SOURCES tests/ui/chat/messages/forwarded_text_contract.cpp
    BUNDLE MaiCordModelTests TIMEOUT 15 TEST maicord_forwarded_text_contract)
maicord_test(MaiCordEditedLabelTests ENTRY maicord_edited_label_entry
    SOURCES tests/ui/chat/messages/edited_label.cpp
    WINDOWS BUNDLE MaiCordModelTests
    LINUX BUNDLE MaiCordUiUnitTests
    TIMEOUT 15 TEST maicord_edited_label)
maicord_test(MaiCordReceivedAttachmentOpenTests ONLY WINDOWS ENTRY maicord_received_attachment_open_test_entry
    SOURCES tests/app/media/received_attachment_open.cpp src/app/media/received_attachment_open.cpp src/app/media/url_resolver.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 10 TEST maicord_received_attachment_open)
maicord_test(MaiCordMessageMutationNoticeTests ENTRY maicord_message_mutation_notice_tests_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/chat/messages/mutation_notice.cpp src/app/chat/messages/mutation_notice.cpp
        src/app/chat/threads/actions.cpp src/app/chat/messages/delete.cpp src/app/chat/messages/reaction.cpp
        src/app/shell/state.cpp
    INCLUDES src/app
    WINDOWS SOURCES src/core/log.cpp
        LIBS ${maicord_app_ui_test_libs} kernel32 user32
    LINUX LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client
            maicord_ui maicord_ui_secure_text maicord_text_system ${maicord_linux_app_test_libs}
    TIMEOUT 15 TEST maicord_message_mutation_notice)
maicord_test(MaiCordGatewaySelfStatusRuntimeTests ENTRY maicord_gateway_self_status_runtime_entry
    SOURCES tests/app/gateway/self_status_runtime.cpp src/app/gateway/client.cpp
    INCLUDES src/app
    DEFINES MAICORD_GATEWAY_SELF_STATUS_TEST=1
    WINDOWS SOURCES src/core/log.cpp
        LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_model kernel32 user32
            ${maicord_tls_libs}
    LINUX LIBS maicord_app_net maicord_net maicord_crypto maicord_desktop_system maicord_protocol maicord_compression
            maicord_media_codec maicord_model maicord_core
        INCLUDES tests
    TIMEOUT 10 TEST maicord_gateway_self_status_runtime)
maicord_test(MaiCordSelfStatusRuntimeTests ENTRY maicord_self_status_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/account/self_status_runtime.cpp src/app/account/self_status.cpp src/app/shell/state.cpp
    BUNDLE MaiCordAppUnitTests
    WINDOWS SOURCES src/core/log.cpp
    TIMEOUT 10 TEST maicord_self_status_frontend_runtime)
maicord_test(MaiCordSearchRuntimeTests ONLY WINDOWS ENTRY maicord_application_search_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/chat/search/search_runtime.cpp tests/app/chat/search/search_runtime_filters.cpp
        tests/app/chat/search/search_runtime_stubs.cpp src/app/shell/state.cpp src/core/log.cpp
    LIBS ${maicord_headless_ui_test_libs} kernel32 user32
    INCLUDES src/app
    TIMEOUT 15 TEST maicord_search_runtime)
maicord_test(MaiCordSearchContentRuntimeTests ENTRY maicord_application_search_content_runtime_entry
    SOURCES tests/app/chat/search/content_runtime.cpp src/app/shell/state.cpp
    INCLUDES src/app
    WINDOWS SOURCES src/core/log.cpp
        LIBS ${maicord_app_ui_test_libs} kernel32 user32 shell32
    LINUX LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client
            maicord_ui maicord_ui_secure_text maicord_text_system ${maicord_linux_app_test_libs}
    TIMEOUT 15 TEST maicord_search_content_runtime)
maicord_test(MaiCordSearchNavigationRuntimeTests ENTRY maicord_application_search_navigation_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/chat/search/navigation_runtime.cpp src/app/chat/messages/mutation_notice.cpp
    INCLUDES src/app
    WINDOWS SOURCES src/core/log.cpp
        LIBS ${maicord_app_ui_test_libs} kernel32 user32
    LINUX LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client
            maicord_ui maicord_ui_secure_text maicord_text_system ${maicord_linux_app_test_libs}
    TIMEOUT 15 TEST maicord_search_navigation_runtime)
maicord_test(MaiCordImageViewerRuntimeTests ONLY WINDOWS ENTRY maicord_application_image_viewer_runtime_entry
    SOURCES tests/app/media/image_viewer_runtime.cpp src/app/media/image_zoom.cpp src/app/shell/state.cpp
        src/core/log.cpp
    LIBS ${maicord_headless_ui_test_libs} maicord_render_core kernel32 user32
    INCLUDES src/app
    TIMEOUT 15 TEST maicord_image_viewer_runtime)
maicord_test(MaiCordQuickSwitcherRuntimeTests ENTRY maicord_application_quick_switcher_runtime_entry
    SOURCES tests/app/shell/quick_switcher_runtime.cpp src/app/shell/quick_switcher.cpp src/app/shell/input_routing.cpp
        src/app/shell/state.cpp
    INCLUDES src/app
    WINDOWS SOURCES src/core/log.cpp
        LIBS ${maicord_headless_ui_test_libs} kernel32 user32
    LINUX LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client
            maicord_ui maicord_ui_secure_text maicord_text_system ${maicord_linux_app_test_libs}
    TIMEOUT 15 TEST maicord_quick_switcher_runtime)
maicord_test(MaiCordMessageForwardRuntimeTests ENTRY maicord_application_message_forward_runtime_entry
    SOURCES tests/app/chat/messages/forward_runtime.cpp src/app/shell/input_routing.cpp src/app/shell/state.cpp
    INCLUDES src/app
    WINDOWS SOURCES src/core/log.cpp
        LIBS ${maicord_headless_ui_test_libs} kernel32 user32
    LINUX LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client
            maicord_ui maicord_ui_secure_text maicord_text_system ${maicord_linux_app_test_libs}
    TIMEOUT 15 TEST maicord_message_forward_runtime)
maicord_test(MaiCordDownloadsRuntimeTests ONLY WINDOWS ENTRY maicord_application_downloads_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/media/downloads_runtime.cpp src/app/shell/state.cpp src/core/log.cpp
    LIBS ${maicord_headless_ui_test_libs} kernel32 user32 shell32 ole32
    INCLUDES src/app
    TIMEOUT 15 TEST maicord_downloads_runtime)
# Account restart schedule: backoff with jitter, attempt cap, user stops, banner states and wakes.
maicord_test(MaiCordAccountReconnectTests ONLY WINDOWS ENTRY maicord_account_reconnect_tests_entry
    SOURCES tests/app/account/account_reconnect.cpp
    BUNDLE MaiCordCoreUnitTests TIMEOUT 5 TEST maicord_account_reconnect)
# Toast queue: order, merge, overflow, timing by update and wake counts, placement and paint.
maicord_test(MaiCordToastTests ENTRY maicord_toast_tests_entry SOURCES tests/ui/widgets/toast.cpp
    BUNDLE MaiCordUiUnitTests TIMEOUT 5 TEST maicord_toast)
# A multi-line settings editor with a selected empty line (Ctrl+A in the profile Bio) lays out, paints and outputs text.
maicord_test(MaiCordSettingsEditSelectionTests ENTRY maicord_settings_edit_selection_entry
    SOURCES tests/ui/settings/edit_selection.cpp
    BUNDLE MaiCordUiUnitTests TIMEOUT 5 TEST maicord_settings_edit_selection)
# Server Settings invite rows: Copy Link through the clipboard sink, its toast and Revoke.
maicord_test(MaiCordInviteCopyRuntimeTests ONLY WINDOWS ENTRY maicord_invite_copy_runtime_entry
    SOURCES tests/app/guild/settings/invite_copy_runtime.cpp src/app/shell/state.cpp src/core/log.cpp
    LIBS ${maicord_app_ui_test_libs} maicord_render_core kernel32 user32
    INCLUDES src/app
    TIMEOUT 10 TEST maicord_invite_copy_runtime)
maicord_test(MaiCordTlsStreamTailTests ONLY WINDOWS ENTRY maicord_tls_stream_tail_entry
    SOURCES tests/platform/net/tls_stream_tail_entry.cpp src/platform/net/tls_stream.cpp
    LIBS maicord_core maicord_platform kernel32 ${maicord_tls_libs}
    TIMEOUT 15 TEST maicord_tls_stream_tail)
maicord_test(MaiCordPinnedMessagesRuntimeTests ENTRY maicord_application_pinned_messages_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/chat/messages/pinned_messages_runtime.cpp src/app/shell/state.cpp
    INCLUDES src/app
    WINDOWS SOURCES src/core/log.cpp
        LIBS ${maicord_headless_ui_test_libs} kernel32 user32
    LINUX LIBS maicord_core maicord_window_null maicord_desktop_system maicord_protocol maicord_model maicord_app_client
            maicord_ui maicord_ui_secure_text maicord_text_system ${maicord_linux_app_test_libs}
    TIMEOUT 15 TEST maicord_pinned_messages_runtime)
maicord_test(MaiCordMediaPickerRuntimeTests ONLY WINDOWS ENTRY maicord_application_media_picker_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/media/picker_runtime.cpp src/app/shell/state.cpp
        src/core/log.cpp
    LIBS ${maicord_headless_test_libs} maicord_ui_input maicord_message_runtime kernel32 user32
    INCLUDES src/app
    TIMEOUT 15 TEST maicord_media_picker_runtime)
maicord_test(MaiCordGifFavoritesRuntimeTests ONLY WINDOWS ENTRY maicord_application_gif_favorites_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/media/gif_favorites_runtime.cpp src/app/shell/state.cpp
    LIBS ${maicord_app_test_libs} kernel32 user32
    INCLUDES src/app
    TIMEOUT 15 TEST maicord_gif_favorites_runtime)
# Includes collection navigation, canonical embed queue recovery, joined restarts and failure recovery.
maicord_test(MaiCordVideoFocusRuntimeTests ONLY WINDOWS ENTRY maicord_video_focus_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/media/video_focus_runtime.cpp
        src/app/media/video.cpp src/app/media/video_sources.cpp
        src/app/media/video_input.cpp src/app/shell/state.cpp
        src/core/log.cpp
    LIBS ${maicord_headless_test_libs} maicord_message_runtime kernel32 user32
    INCLUDES src/app
    TIMEOUT 10 TEST maicord_video_focus_runtime)
maicord_test(MaiCordLoginCancelRuntimeTests ONLY WINDOWS ENTRY maicord_login_cancel_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/account/login/cancel_runtime.cpp src/app/account/login/browser_auth.cpp
        src/app/account/login/captcha_auth.cpp src/app/account/login/auth.cpp src/app/account/login/auth_recovery.cpp
        src/app/shell/state.cpp
        src/app/account/account_host.cpp src/core/log.cpp
    BUNDLE MaiCordAppUnitTests)
maicord_test(MaiCordNotificationTests ONLY WINDOWS ENTRY maicord_notifications_tests_entry
    SOURCES tests/model/notifications/policy.cpp tests/protocol/user/notification_settings_wire.cpp tests/model/notifications/desktop_notification_queue.cpp
        tests/model/notifications/sound_policy.cpp tests/model/notifications/sound_admission.cpp tests/protocol/user/notification_controls.cpp
        tests/model/notifications/rules.cpp
    BUNDLE MaiCordAppUnitTests
    TEST maicord_notifications)
# tests/platform/desktop/toast_abi.cpp checks the client's WinRT toast ABI against the SDK's
# headers, so it compiles against them; runtimeobject: the SDK string calls it
# compares the client's against.
if(NOT MAICORD_LINUX)
    add_library(maicord_toast_abi_conformance OBJECT tests/platform/desktop/toast_abi.cpp)
    target_link_libraries(maicord_toast_abi_conformance PRIVATE maicord_compile_contract)
    set_target_properties(maicord_toast_abi_conformance PROPERTIES CXX_EXTENSIONS OFF MAICORD_SDK_INCLUDES ON)
endif()
maicord_test(MaiCordNativeNotificationTests ONLY WINDOWS ENTRY maicord_native_notifications_tests_entry
    SOURCES tests/platform/desktop/native_notifications.cpp $<TARGET_OBJECTS:maicord_toast_abi_conformance>
        tests/integration/desktop_notifications_probe.cpp
    LIBS maicord_core maicord_platform kernel32 runtimeobject
    TEST maicord_native_notifications)
# Compile the explicit enrollment branch without invoking it: this fixture
# substitutes a copied notification sink and never creates desktop identity.
maicord_test(MaiCordApplicationNotificationTests ONLY WINDOWS ENTRY maicord_application_notifications_tests_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/notifications/notifications.cpp src/app/notifications/notifications.cpp src/app/shell/state.cpp
        src/core/log.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_model maicord_client_preferences
        maicord_notification_sound_assets maicord_ui maicord_text maicord_message_runtime kernel32 user32 shell32
    INCLUDES src/app
    DEFINES MAICORD_NOTIFICATION_FIXTURE=1
    TEST maicord_application_notifications)
# Go Live capture (src/platform/capture), never capturing the user's screen or sound:
#
#   maicord_capture            the shared picker, thumbnail, frame-cap and name rules; Windows: the WinRT and process
#                              loopback ABI against the SDK (tests/platform/capture/capture_abi.cpp, SDK includes), the
#                              methods per build, the source list's order, the GPU thumbnail of a synthetic texture;
#                              Linux: the PipeWire negotiation PODs against libspa's bytes, the format reader, fourccs
#   maicord_capture_portal     Linux: the ScreenCast portal flow against the mock (tools/test_dbus.sh; 77 without it)
#   maicord_capture_pipewire   Linux: the consumer against a private PipeWire daemon and a synthetic dma-buf source
#                              (tools/test_capture_pipewire.sh; 77 without it, libpipewire or /dev/dri/card0)
#   maicord_stream_audio_pipewire  Linux: stream audio against the same daemon, with a synthetic "MaiCord" player that
#                              must never be heard and another application's player that must
#   maicord_capture_probe      opt-in manual probe of the real screen and sound (MAICORD_CAPTURE_PROBE=1; 77 without)
if(NOT MAICORD_LINUX)
    add_library(maicord_capture_abi_conformance OBJECT tests/platform/capture/capture_abi.cpp)
    target_link_libraries(maicord_capture_abi_conformance PRIVATE maicord_compile_contract)
    set_target_properties(maicord_capture_abi_conformance PROPERTIES CXX_EXTENSIONS OFF MAICORD_SDK_INCLUDES ON)
endif()
maicord_test(MaiCordCaptureTests ENTRY maicord_capture_entry
    SOURCES tests/platform/capture/capture.cpp
    WINDOWS SOURCES $<TARGET_OBJECTS:maicord_capture_abi_conformance> src/core/log.cpp
        LIBS maicord_capture maicord_gpu_d3d11 maicord_core d3d11 dxgi ole32 user32 gdi32 kernel32
    LINUX LIBS maicord_capture maicord_dbus maicord_core
    BOTH TIMEOUT 20 TEST maicord_capture)
maicord_test(MaiCordCaptureProbe ENTRY maicord_capture_probe_entry
    SOURCES tests/platform/capture/capture_probe.cpp
    WINDOWS SOURCES src/core/log.cpp
        LIBS maicord_capture maicord_gpu_d3d11 maicord_core d3d11 dxgi ole32 user32 gdi32 kernel32
    LINUX LIBS maicord_capture maicord_dbus maicord_core
    BOTH TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS integration TEST maicord_capture_probe)
# The whole producer chain on the real desktop: the primary screen into the real hardware encoder, counts per second
# (opt-in like the probe above: MAICORD_CAPTURE_PROBE=1; 77 without).
maicord_test(MaiCordGoLivePipelineProbe ONLY WINDOWS ENTRY maicord_go_live_pipeline_probe_entry
    SOURCES tests/platform/encode/go_live_pipeline_probe.cpp src/core/log.cpp
    LIBS maicord_capture maicord_video_encode maicord_gpu_d3d11 maicord_core d3d11 dxgi ole32 user32 gdi32 kernel32
    TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS integration TEST maicord_go_live_pipeline_probe)
maicord_test(MaiCordCapturePortalTests ONLY LINUX ENTRY maicord_capture_portal_entry
    SOURCES tests/platform/capture/capture_portal.cpp
    LIBS maicord_capture maicord_dbus maicord_core
    TIMEOUT 40 SKIP_RETURN_CODE 77 TEST maicord_capture_portal)
maicord_add_tests(MaiCordCapturePortalTests ONLY LINUX TIMEOUT 20 SKIP_RETURN_CODE 77 TEST maicord_capture_portal_bare
    --bare)
maicord_test(MaiCordCapturePipewireTests ONLY LINUX ENTRY maicord_capture_pipewire_entry
    SOURCES tests/platform/capture/pipewire_dmabuf.cpp
    LIBS maicord_capture maicord_dbus maicord_core
    TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS integration TEST maicord_capture_pipewire)
maicord_test(MaiCordStreamAudioPipewireTests ONLY LINUX ENTRY maicord_stream_audio_pipewire_entry
    SOURCES tests/platform/capture/pipewire_audio.cpp
    LIBS maicord_capture maicord_dbus maicord_core
    TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS integration TEST maicord_stream_audio_pipewire)
if(MAICORD_LINUX)
    # The tests of the D-Bus client (src/platform/dbus).
    #
    #   maicord_dbus_wire       marshalling, header parsing, hostile input and addresses; needs no bus.
    #   maicord_atspi_map       the AT-SPI server's objects (roles, states, interfaces, actions, text, the answers to calls on a
    #                           synthetic window); needs no bus.
    #
    # The integration tests talk to private buses that tools/test_dbus.sh starts (dbus-daemon and the python mock services);
    # without MAICORD_DBUS_TEST_BUS in the environment they skip with status 77, so they sit in the normal suite and never
    # touch the bus of the user's session. (cmake/tests.cmake gives every test MAICORD_SECRET_SERVICE=off, so that no
    # test reaches the user's keyring; the secure buffer tests below ask for `auto`, against the mock service.)
    #
    #   maicord_dbus_bus              the connection: authentication, calls, timeouts, typed bodies decoded by libdbus,
    #                                 descriptors, signals, an exported object, a fake bus that sends garbage
    #   maicord_dbus_services         the settings portal, notifications through the daemon, OpenURI, the Secret Service and
    #                                 the portal probes, against the mock services
    #   maicord_dbus_services_portal  notifications through the portal (the script sets MAICORD_NOTIFICATION_BACKEND=portal)
    #   maicord_dbus_services_plain   notifications through a daemon that does not read markup in the body
    #   maicord_dbus_services_bare    every client on a bus without any service
    #   maicord_dbus_desktop          the tray (StatusNotifierItem and dbusmenu), GlobalShortcuts, the file chooser, the
    #                                 eyedropper (Screenshot.PickColor), the launcher badge and single instance on the bus,
    #                                 against the mock; _bare: all of it without services
    #   maicord_dbus_instance_socket  single instance over the abstract socket (no bus needed: always runs)
    #   maicord_dbus_secure_buffer    the user-bound secure buffer with its master key in the Secret Service (creation,
    #                                 lookup, the fallback to the key file, moving the file in), and on a bus without one
    #   maicord_dbus_real             the settings portal, the probes and the Secret Service against xdg-desktop-portal and
    #                                 gnome-keyring on a private bus (tools/test_dbus_real.sh; skips without MAICORD_DBUS_REAL=1)
    #   maicord_desktop_entry         the generated .desktop file (tools/generate_desktop_entry.py --validate), and
    #                                 desktop-file-validate when it is installed
    maicord_test(MaiCordDbusWireTests ENTRY maicord_dbus_wire_entry
        SOURCES tests/platform/dbus/dbus_wire.cpp
        LIBS maicord_dbus maicord_core
        DEPENDS maicord_dbus
        TIMEOUT 20 TEST maicord_dbus_wire)
    # The AT-SPI server's objects (src/platform/atspi) without a bus: the role, state, interface and action mapping, the paths,
    # the text offsets, and the answers to the calls a screen reader makes on a synthetic window. tools/test_atspi.sh drives the
    # real client through the real registry and a client library.
    maicord_test(MaiCordAtspiMapTests ENTRY maicord_atspi_map_entry
        SOURCES tests/platform/atspi/atspi_map.cpp
        LIBS maicord_atspi maicord_ui maicord_dbus maicord_core
        DEPENDS maicord_atspi
        TIMEOUT 20 TEST maicord_atspi_map)
    # The server's life on a bare private bus (tools/test_dbus.sh), with this program as org.a11y.Bus, the registry and the
    # screen reader: nothing connects while accessibility is off, IsEnabled or ScreenReaderEnabled brings the connection and the
    # registration, a client's call, the events of a snapshot difference, off again, and a registry that returns.
    maicord_test(MaiCordAtspiBusTests ENTRY maicord_atspi_bus_entry
        SOURCES tests/platform/atspi/atspi_bus.cpp
        LIBS maicord_atspi maicord_ui maicord_dbus maicord_core
        DEPENDS maicord_atspi
        TIMEOUT 30 TEST maicord_atspi_bus)
    maicord_test(MaiCordDbusBusTests ENTRY maicord_dbus_bus_entry
        SOURCES tests/platform/dbus/dbus_bus.cpp
        LIBS maicord_dbus maicord_core
        DEPENDS maicord_dbus
        TIMEOUT 20 TEST maicord_dbus_bus)
    maicord_test(MaiCordDbusServicesTests ENTRY maicord_dbus_services_entry
        SOURCES tests/platform/dbus/dbus_services.cpp
        LIBS maicord_dbus maicord_core
        DEPENDS maicord_dbus
        TIMEOUT 20 TEST maicord_dbus_services)
    maicord_add_tests(MaiCordDbusServicesTests TIMEOUT 20 TEST maicord_dbus_services_portal --portal)
    maicord_add_tests(MaiCordDbusServicesTests TIMEOUT 20 TEST maicord_dbus_services_plain --plain)
    maicord_add_tests(MaiCordDbusServicesTests TIMEOUT 20 TEST maicord_dbus_services_bare --bare)
    maicord_test(MaiCordDbusDesktopTests ENTRY maicord_dbus_desktop_entry
        SOURCES tests/platform/dbus/dbus_desktop.cpp src/platform/desktop/screen_color.cpp
        LIBS maicord_dbus maicord_core
        DEPENDS maicord_dbus
        TIMEOUT 20 TEST maicord_dbus_desktop)
    maicord_add_tests(MaiCordDbusDesktopTests TIMEOUT 20 TEST maicord_dbus_desktop_bare --bare)
    maicord_add_tests(MaiCordDbusDesktopTests TIMEOUT 20 ENV MAICORD_INSTANCE_BACKEND=socket
        TEST maicord_dbus_instance_socket --socket)
    maicord_test(MaiCordSecureBufferServiceTests ENTRY maicord_secure_buffer_service_entry
        SOURCES tests/platform/dbus/secure_buffer_service.cpp
        LIBS maicord_crypto maicord_dbus maicord_core
        DEPENDS maicord_dbus
        TIMEOUT 20 TEST maicord_dbus_secure_buffer)
    maicord_add_tests(MaiCordSecureBufferServiceTests TIMEOUT 20 TEST maicord_dbus_secure_buffer_bare --bare)
    maicord_test(MaiCordDbusRealTests ENTRY maicord_dbus_real_entry
        SOURCES tests/platform/dbus/dbus_real.cpp
        LIBS maicord_crypto maicord_dbus maicord_core
        DEPENDS maicord_dbus
        TIMEOUT 20 TEST maicord_dbus_real)
    foreach(maicord_dbus_case IN ITEMS maicord_dbus_bus maicord_dbus_services maicord_dbus_services_portal
            maicord_dbus_services_plain maicord_dbus_services_bare maicord_dbus_desktop maicord_dbus_desktop_bare maicord_dbus_secure_buffer
            maicord_dbus_secure_buffer_bare maicord_dbus_real maicord_atspi_bus)
        # Status 77: no private bus to talk to. The mock services serve one test at a time.
        set_tests_properties(${maicord_dbus_case} PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK dbus_private_bus)
    endforeach()
    foreach(maicord_dbus_case IN ITEMS maicord_dbus_secure_buffer maicord_dbus_secure_buffer_bare)
        set_property(TEST ${maicord_dbus_case} APPEND PROPERTY ENVIRONMENT "MAICORD_SECRET_SERVICE=auto")
    endforeach()
    set_tests_properties(maicord_dbus_instance_socket PROPERTIES SKIP_RETURN_CODE 77)
    # The packaging artifact: the generated .desktop file is valid (our checks, then desktop-file-validate where installed).
    add_test(NAME maicord_desktop_entry
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_desktop_entry.py"
            --validate "${MAICORD_DESKTOP_ENTRY}")
    add_test(NAME maicord_desktop_entry_validate
        COMMAND sh -c "command -v desktop-file-validate >/dev/null || exit 77; desktop-file-validate \"$1\"" validate
            "${MAICORD_DESKTOP_ENTRY}")
    set_tests_properties(maicord_desktop_entry_validate PROPERTIES SKIP_RETURN_CODE 77)
    maicord_test_target(maicord_desktop_entry maicord_desktop_entry)
    maicord_test_target(maicord_desktop_entry_validate maicord_desktop_entry)
endif()
maicord_add_tests(MaiCordLoginCancelRuntimeTests TEST maicord_login_cancel_runtime)
maicord_test(MaiCordPasswordRecoveryCoordinatorTests ENTRY maicord_password_recovery_coordinator_entry
    SOURCES tests/app/account/login/password_recovery_coordinator.cpp
    WINDOWS LIBS maicord_core maicord_platform maicord_protocol maicord_app_core kernel32 ${maicord_tls_libs}
    LINUX LIBS maicord_core maicord_window_null maicord_desktop_system maicord_dbus maicord_protocol maicord_app_client
            maicord_crypto maicord_net ${maicord_linux_app_test_libs}
    TEST maicord_password_recovery_coordinator)
maicord_test(MaiCordCaptchaBridgeTests ONLY WINDOWS ENTRY maicord_captcha_bridge_tests_entry
    SOURCES src/core/log.cpp
    SOURCES tests/app/account/login/captcha_bridge_login.cpp tests/app/account/login/captcha_bridge_runtime.cpp tests/protocol/auth/captcha_bridge_protocol.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_ui kernel32 user32 ${maicord_tls_libs}
        shell32
    INCLUDES src/app
    TIMEOUT 15 TEST maicord_captcha_bridge)
maicord_test(MaiCordBrowserHandoffExchangeTests ONLY WINDOWS ENTRY maicord_browser_handoff_exchange_coordinator_entry
    SOURCES tests/app/account/login/browser_handoff_exchange_coordinator.cpp tests/protocol/auth/browser_handoff_exchange_protocol.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core kernel32 ${maicord_tls_libs}
    TIMEOUT 15 TEST maicord_browser_handoff_exchange)
maicord_test(MaiCordPhoneRecoveryCoordinatorTests ENTRY maicord_phone_recovery_coordinator_entry
    SOURCES tests/app/account/login/phone_recovery_coordinator.cpp
    WINDOWS LIBS maicord_core maicord_platform maicord_protocol maicord_app_core kernel32 ${maicord_tls_libs}
    LINUX LIBS maicord_core maicord_window_null maicord_desktop_system maicord_dbus maicord_protocol maicord_app_client
            maicord_crypto maicord_net ${maicord_linux_app_test_libs}
    TIMEOUT 15 TEST maicord_phone_recovery_coordinator)
maicord_test(MaiCordPasswordResetProtocolTests ONLY WINDOWS ENTRY maicord_password_reset_protocol_entry
    SOURCES tests/protocol/auth/password_reset_protocol.cpp src/app/account/login/continuations.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32 bcrypt crypt32
    TEST maicord_password_reset_protocol)
maicord_test(MaiCordPasswordResetCoordinatorTests ENTRY maicord_password_reset_coordinator_entry
    SOURCES tests/app/account/login/password_reset_coordinator.cpp src/app/account/account_profile.cpp
        src/app/account/login/mfa_policy.cpp src/app/net/file_persistence.cpp
        src/app/account/login/captcha_challenge.cpp
    WINDOWS LIBS maicord_core maicord_platform maicord_protocol kernel32 ${maicord_tls_libs}
    LINUX LIBS maicord_app_client maicord_core maicord_window_null maicord_desktop_system maicord_dbus maicord_protocol
            maicord_crypto maicord_net ${maicord_linux_app_test_libs}
    TEST maicord_password_reset_coordinator)
# Offline fixtures over the shipping REST owner share one translation unit, so
# the owner implementation they include compiles once; each case runs one.
maicord_test(MaiCordRestClientFixtureTests ONLY WINDOWS ENTRY maicord_rest_client_fixtures_entry
    SOURCES ${maicord_account_fake} src/app/net/rest_client_trace.cpp
    SOURCES tests/app/net/rest_client_fixtures.cpp src/app/chat/messages/account_bulk_message_ack.cpp src/app/account/account_logout.cpp
        src/app/account/account_host.cpp src/media/image/png_common.cpp src/app/account/user_client_configuration.cpp
        src/app/account/client_session_metadata.cpp src/app/net/signed_out_session.cpp src/core/log.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_compression kernel32 ${maicord_tls_libs} ntdll
    TIMEOUT 15
    TEST maicord_reset_rest_storage reset-storage
    TEST maicord_user_settings_rest_storage user-settings-storage
    TEST maicord_account_logout_transport logout-transport
    TEST maicord_account_bulk_read_transport bulk-read-transport
    TEST maicord_account_action_captcha_transport action-captcha-transport
    TEST maicord_rest_credential_swap credential-swap
    TEST maicord_signed_out_session signed-out-session)
maicord_test(MaiCordAccountLogoutRuntimeTests ONLY WINDOWS ENTRY maicord_application_account_logout_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/account/application_account_logout_runtime.cpp src/app/account/account_logout_stop.cpp
        src/app/account/login/account_credential_forget.cpp src/app/account/account_paths.cpp src/app/net/file_persistence.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_ui maicord_account_host kernel32 user32
    INCLUDES src/app
    TIMEOUT 20 TEST maicord_account_logout_runtime)
if(NOT MAICORD_LINUX)
    add_library(MaiCordSecureBufferLockBudgetPeer MODULE tests/platform/crypto/secure_buffer_lock_budget.cpp
        $<TARGET_OBJECTS:maicord_runtime>)
    target_compile_definitions(MaiCordSecureBufferLockBudgetPeer PRIVATE MAICORD_LOCK_BUDGET_PEER_BUILD=1)
    target_link_libraries(MaiCordSecureBufferLockBudgetPeer PRIVATE maicord_compile_contract kernel32 crypt32)
    target_link_options(MaiCordSecureBufferLockBudgetPeer PRIVATE /NOENTRY /SUBSYSTEM:WINDOWS,6.02)
    set_target_properties(MaiCordSecureBufferLockBudgetPeer PROPERTIES CXX_EXTENSIONS OFF)
endif()
maicord_test(MaiCordSecureBufferLockBudgetTests ONLY WINDOWS ENTRY maicord_secure_buffer_lock_budget_entry
    SOURCES tests/platform/crypto/secure_buffer_lock_budget.cpp
    LIBS kernel32 crypt32 psapi
    DEPENDS MaiCordSecureBufferLockBudgetPeer
    TIMEOUT 20 TEST maicord_secure_buffer_lock_budget)
maicord_test(MaiCordGuildChannelUniquenessTests ENTRY maicord_guild_channel_uniqueness_entry
    SOURCES tests/app/gateway/guild_channel_uniqueness.cpp
    BUNDLE MaiCordAppUnitTests
    TIMEOUT 20 TEST maicord_guild_channel_uniqueness)
# The plugin loader's cleanup failures (a failed handle close or hash destruction during pinning, hashing and
# unloading), through imported-call wrappers around the production implementation.
maicord_test(MaiCordPluginLoaderFailureTests ONLY WINDOWS ENTRY maicord_plugin_loader_failures_entry
    SOURCES tests/app/plugins/loader_failures.cpp
    LIBS maicord_core kernel32 bcrypt
    TIMEOUT 15 TEST maicord_plugin_loader_failures)
maicord_test(MaiCordScopedReadRuntimeTests ONLY WINDOWS ENTRY maicord_scoped_read_runtime_entry
    SOURCES ${maicord_account_fake}
    SOURCES tests/app/chat/scoped_read_runtime.cpp src/app/chat/read_state.cpp
        src/app/chat/scoped_read.cpp src/app/chat/messages/mutation_notice.cpp
        src/app/shell/state.cpp src/core/log.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_ui maicord_model
        maicord_message_runtime kernel32 user32
    INCLUDES src/app
    TIMEOUT 20
    TEST maicord_read_state_runtime --read-state
    TEST maicord_scoped_read_runtime)
maicord_test(MaiCordMp4Tests ENTRY maicord_mp4_tests_entry SOURCES tests/media/containers/mp4_demux_test.cpp
    BUNDLE MaiCordCodecUnitTests TEST maicord_mp4_demux)
maicord_test(MaiCordProgressiveVideoTests ONLY WINDOWS ENTRY maicord_progressive_video_tests_entry
    SOURCES tests/app/media/progressive_video.cpp
    LIBS maicord_app_core maicord_protocol maicord_media_core
    BUNDLE MaiCordCodecUnitTests
    TIMEOUT 10 TEST maicord_progressive_video)
# AV1 header parsing and DXVA parameters on the synthetic SVT-AV1 fixture, with fuzzing.
maicord_test(MaiCordAv1Tests ONLY WINDOWS ENTRY maicord_av1_tests_entry
    SOURCES tests/media/video/av1.cpp src/platform/decode/av1_dxva_parameters.cpp
    BUNDLE MaiCordCodecUnitTests TIMEOUT 10 TEST maicord_av1)
maicord_test(MaiCordH264Tests ENTRY maicord_h264_tests_entry
    SOURCES tests/media/video/h264_parameters_test.cpp tests/media/video/h264_state_test.cpp tests/media/video/h264_stream_test.cpp
        tests/media/video/hevc_stream_test.cpp
    BUNDLE MaiCordCodecUnitTests
    TEST maicord_h264_parameters)
# Live H.264 and H.265 stream decoders on real DXVA hardware with synthetic
# PCM/skip streams (1080p/1440p/4K included), the live AV1 decoder on a lossless
# SVT-AV1 fixture, then the attachment decoder on an in-memory MP4 through
# video_frame_gpu; skipped when no video device exists.
maicord_test(MaiCordH264StreamGpuTests ONLY WINDOWS ENTRY maicord_h264_stream_gpu_tests_entry
    SOURCES tests/media/video/h264_stream_gpu_test.cpp tests/media/video/hevc_stream_gpu_test.cpp src/platform/decode/gpu_stream_decoder.cpp
        src/platform/decode/h264_gpu_stream_decoder.cpp src/platform/decode/hevc_gpu_stream_decoder.cpp src/app/voice/log.cpp
        src/app/voice/log_record.cpp src/core/log.cpp
        tests/media/video/av1_stream_gpu_test.cpp src/platform/decode/av1_gpu_stream.cpp src/platform/decode/av1_dxva_parameters.cpp
        src/app/voice/video_stream_decoders.cpp tests/platform/decode/h264_attachment_gpu_test.cpp src/platform/decode/mp4_decoder.cpp
        src/platform/decode/h264_gpu_decoder.cpp src/platform/decode/hevc_gpu_decoder.cpp src/platform/decode/h264_gpu_configuration.cpp
        src/platform/decode/gpu_decoder_surfaces.cpp src/gpu/d3d11/video_frame.cpp ${MAICORD_VIDEO_FRAME_HEADER}
    LIBS maicord_core maicord_media_codec maicord_gpu_d3d11 kernel32 d3d11 dxguid
    INCLUDES ${MAICORD_GENERATED_DIRECTORY}
    TIMEOUT 10 LABELS gpu RESOURCE_LOCK native_gpu_c TEST maicord_h264_stream_gpu)
# The Go Live encoder on real hardware: each codec the GPU encodes, through MaiCord's own stream decoder and video
# conversion, at 40 dB PSNR or better; keyframe requests, the preview and a reconfiguration, then a continuous stream.
# One case per codec (MAICORD_STREAM_ENCODER_CODEC), so the three hardware sessions run side by side: a case for a
# codec the GPU lacks passes with nothing to do, and the first codec the GPU encodes also runs the checks done once.
# Skipped without a hardware encoder.
maicord_test(MaiCordStreamEncoderGpuTests ONLY WINDOWS ENTRY maicord_stream_encoder_gpu_tests_entry
    SOURCES tests/platform/encode/stream_encoder_gpu_test.cpp src/platform/decode/gpu_stream_decoder.cpp
        src/platform/decode/h264_gpu_stream_decoder.cpp src/platform/decode/hevc_gpu_stream_decoder.cpp
        src/platform/decode/av1_gpu_stream.cpp src/platform/decode/av1_dxva_parameters.cpp
        src/platform/decode/gpu_decoder_surfaces.cpp src/gpu/d3d11/video_frame.cpp ${MAICORD_VIDEO_FRAME_HEADER}
    LIBS maicord_video_encode maicord_core maicord_media_codec maicord_gpu_d3d11 kernel32 d3d11 dxguid ole32
    INCLUDES ${MAICORD_GENERATED_DIRECTORY})
foreach(maicord_encoder_codec IN ITEMS h264 h265 av1)
    maicord_add_tests(MaiCordStreamEncoderGpuTests ONLY WINDOWS TIMEOUT 20 LABELS gpu
        ENV MAICORD_STREAM_ENCODER_CODEC=${maicord_encoder_codec} TEST maicord_stream_encoder_gpu_${maicord_encoder_codec})
endforeach()
# The encoder's pure parts on both systems: Discord's quality presets and Nitro limits, extents and bitrates, the codec
# tiers, and the rewriting of the hardware encoders' output into the sender's access units.
maicord_test(MaiCordStreamEncoderUnitTests ENTRY maicord_stream_encoder_units_entry
    SOURCES tests/platform/encode/stream_encoder_units.cpp
    LIBS maicord_video_encode maicord_media_codec maicord_core kernel32
    TIMEOUT 10 TEST maicord_stream_encoder_units)
if(MAICORD_LINUX)
    # The VA-API encoder on real hardware, through MaiCord's own decoder at 40 dB PSNR or better. Status 77 without an
    # EGL device, a VA-API encoder (WSL has none) or /dev/udmabuf.
    maicord_test(MaiCordStreamEncoderVaapiTests ENTRY maicord_stream_encoder_vaapi_entry
        SOURCES tests/platform/encode/stream_encoder_vaapi_test.cpp
        LIBS maicord_video_encode maicord_video_decode maicord_gpu_gles_video maicord_gpu_gles_shaders maicord_gpu_gles
            maicord_video_vaapi maicord_video_software maicord_compression maicord_media_codec maicord_core
        INCLUDES src
        TIMEOUT 20 LABELS gpu TEST maicord_stream_encoder_vaapi)
    set_tests_properties(maicord_stream_encoder_vaapi PROPERTIES SKIP_RETURN_CODE 77)
endif()
if(MAICORD_LINUX)
    # The tests of the Linux live video decoders (src/platform/decode).
    #
    #   maicord_video_software             libopenh264 and libdav1d against libavcodec's pictures from the same streams
    #                                      (fixtures: tools/generate_video_fixtures.py); skipped (77) without a library
    #   maicord_video_software_no_library  both libraries absent: no tier, no decoder
    maicord_test(MaiCordVideoSoftwareTests ENTRY maicord_video_software_entry
        SOURCES tests/platform/decode/video_software.cpp
        LIBS maicord_video_software maicord_media_codec maicord_core
        TIMEOUT 20 TEST maicord_video_software)
    set_tests_properties(maicord_video_software PROPERTIES SKIP_RETURN_CODE 77)
    maicord_add_tests(MaiCordVideoSoftwareTests TIMEOUT 10
        ENV MAICORD_DAV1D_LIBRARY=libmaicord-no-such-dav1d.so.7 MAICORD_OPENH264_LIBRARY=libmaicord-no-such-openh264.so.7
        TEST maicord_video_software_no_library --no-library)
    # The live stream decoder over the software tier, end to end on the GPU API: access units in, the planes it publishes
    # converted by the video_frame shader and read back, against the reference pictures converted on the CPU. Runs on
    # llvmpipe anywhere; MAICORD_VAAPI=off keeps a developer GPU's VA-API out of it.
    maicord_test(MaiCordVideoStreamSoftwareTests ENTRY maicord_video_stream_software_entry
        SOURCES tests/platform/decode/video_stream_software.cpp
        LIBS maicord_video_decode maicord_gpu_gles_video maicord_gpu_gles_shaders maicord_gpu_gles maicord_video_software
            maicord_compression maicord_media_codec maicord_core
        INCLUDES src
        TIMEOUT 20 ENV MAICORD_VAAPI=off TEST maicord_video_stream_software)
    set_tests_properties(maicord_video_stream_software PROPERTIES SKIP_RETURN_CODE 77)
    # The VA-API parameter and slice builders without a GPU: every picture of the encoder fixtures through the stream state
    # machines, the VA picture parameters against the DXVA builders' output, the slice parameters against values derived
    # from the parsers. Hardware validation is pending (WSL has no /dev/dri).
    maicord_test(MaiCordVaapiParametersTests ENTRY maicord_vaapi_parameters_entry
        SOURCES tests/platform/decode/vaapi_parameters.cpp
        LIBS maicord_video_vaapi maicord_media_codec maicord_gpu_gles maicord_core
        INCLUDES src
        TIMEOUT 20 TEST maicord_vaapi_parameters)
    # What the client advertises and the pure parts of the hardware tier: the capability policy with mocked probe results,
    # the Select Protocol offers built from them, the dma-buf layout of an exported surface, the H.264 profile choice and the
    # plane pool under concurrent retains. The four environment cases leave the hardware tier without a library, without
    # MAICORD_VAAPI, on a node no driver serves and (where the machine has none) without a render node.
    maicord_test(MaiCordVideoCapabilityTests ENTRY maicord_video_capability_entry
        SOURCES tests/platform/decode/video_capability.cpp
        LIBS maicord_video_vaapi maicord_protocol maicord_media_codec maicord_gpu_gles maicord_core
        INCLUDES src
        TIMEOUT 20 TEST maicord_video_capability)
    maicord_add_tests(MaiCordVideoCapabilityTests TIMEOUT 10 ENV MAICORD_VAAPI=off TEST maicord_vaapi_off --off)
    maicord_add_tests(MaiCordVideoCapabilityTests TIMEOUT 10 ENV MAICORD_VAAPI_LIBRARY=libmaicord-no-such-va.so.2
        TEST maicord_vaapi_no_library --no-library)
    maicord_add_tests(MaiCordVideoCapabilityTests TIMEOUT 10 ENV MAICORD_VAAPI_DEVICE=/dev/null
        TEST maicord_vaapi_bad_node --bad-node)
    maicord_add_tests(MaiCordVideoCapabilityTests TIMEOUT 10 TEST maicord_vaapi_no_node --no-node)
    set_tests_properties(maicord_vaapi_no_node PROPERTIES SKIP_RETURN_CODE 77)
    # How the stream decoder chooses and gives up a tier (a hardware backend double: it defines the two functions the decoder
    # asks of the VA-API backend), how a VA-API picture is submitted and waited for (a vaapi::Session over a recording libva)
    # and, where the machine has a DRM device (a VM's vgem: dumb buffers as dma-bufs), the renderer's import and the exports a
    # session keeps.
    maicord_test(MaiCordVideoHardwareTierTests ENTRY maicord_video_hardware_tier_entry
        SOURCES tests/platform/decode/video_hardware_tier.cpp
        LIBS maicord_video_decode maicord_video_vaapi maicord_video_software maicord_gpu_gles maicord_media_codec maicord_core
        INCLUDES src
        TIMEOUT 20 ENV MAICORD_VAAPI=off TEST maicord_video_hardware_tier)
    # Pictures that arrive as dma-bufs (NV12 and P010 in dumb buffers of a DRM device), imported, converted by the video_frame
    # shader and compared with the CPU conversion; skipped (77) without a DRM device this user may use or an EGL that imports.
    maicord_test(MaiCordVideoDmabufTests ENTRY maicord_video_dmabuf_entry
        SOURCES tests/platform/decode/video_dmabuf.cpp
        LIBS maicord_gpu_gles_video maicord_gpu_gles_shaders maicord_gpu_gles maicord_core
        INCLUDES src
        TIMEOUT 20 ENV MAICORD_VAAPI=off TEST maicord_video_dmabuf)
    set_tests_properties(maicord_video_dmabuf PROPERTIES SKIP_RETURN_CODE 77)
    # The movie decoder of attachments (mp4_decoder.hpp) with the software tier forced: movies from
    # tools/generate_mp4_fixtures.py against libavformat's and libavcodec's pictures and PCM. The AAC decoder is the
    # Linux one (maicord_aac_decoder).
    maicord_test(MaiCordMp4DecoderLinuxTests ENTRY maicord_mp4_decoder_linux_entry
        SOURCES tests/platform/decode/mp4_decoder_linux.cpp
        LIBS maicord_video_decode maicord_aac_decoder maicord_gpu_gles_video maicord_gpu_gles_shaders maicord_gpu_gles
            maicord_video_software maicord_compression maicord_media_codec maicord_core
        INCLUDES src
        TIMEOUT 30 ENV MAICORD_VAAPI=off TEST maicord_mp4_decoder_linux)
    set_tests_properties(maicord_mp4_decoder_linux PROPERTIES SKIP_RETURN_CODE 77)
    # The media player's session (src/app/media/account_video_session*.cpp, the Windows code) over that decoder: a movie
    # played by the clock, seeked and replayed, every picture converted and compared. --audio (the opt-in
    # maicord_video_playback_audio, against a sound server: tools/test_pulse_audio.sh) plays the AAC track into a server.
    maicord_test(MaiCordAccountVideoPlaybackTests ENTRY maicord_account_video_playback_entry
        SOURCES tests/app/media/account_video_playback_linux.cpp src/app/media/account_video_session.cpp
            src/app/media/account_video_session_audio.cpp tests/platform/audio/window_post_stub.cpp
        LIBS maicord_video_decode maicord_aac_decoder maicord_audio_pulse maicord_gpu_gles_video maicord_gpu_gles_shaders
            maicord_gpu_gles maicord_video_software maicord_media_core maicord_media_codec maicord_compression maicord_core
        INCLUDES src src/app
        TIMEOUT 60 ENV MAICORD_VAAPI=off TEST maicord_account_video_playback)
    set_tests_properties(maicord_account_video_playback PROPERTIES SKIP_RETURN_CODE 77)
    # The same binary with --audio: the AAC track of a movie through the session's PCM transport into a sound server.
    # Opt-in like maicord_pulse_loopback (tools/test_pulse_audio.sh configures MAICORD_PULSE_INTEGRATION and picks the
    # server and loads the null sink maicord_test_playback that the audio goes to).
    maicord_add_tests(MaiCordAccountVideoPlaybackTests TIMEOUT 60 SKIP_RETURN_CODE 77 LABELS integration
        RESOURCE_LOCK pulse_server ENVIRONMENT MAICORD_VAAPI=off PULSE_SINK=maicord_test_playback
        TEST maicord_video_playback_audio --audio)
    if(NOT "$ENV{MAICORD_PULSE_INTEGRATION}")
        set_tests_properties(maicord_video_playback_audio PROPERTIES DISABLED TRUE)
    endif()
    # The composer's local video attachment preview (account_media_decode_local_video_preview, the worker lane's code) on the
    # movie decoder: a file streamed through the exact-read callback into the lane's sparse source, its first picture
    # published as a GPU frame and converted. Linked as the client is, over the null window backend.
    maicord_test(MaiCordLocalVideoPreviewTests ENTRY maicord_local_video_preview_entry
        SOURCES tests/app/media/local_video_preview_linux.cpp tests/platform/audio/window_post_stub.cpp
        LIBS maicord_video_decode maicord_aac_decoder maicord_audio_pulse maicord_gpu_gles_video maicord_gpu_gles_shaders
            maicord_gpu_gles maicord_video_software ${maicord_linux_app_test_libs}
        INCLUDES src src/app
        TIMEOUT 60 ENV MAICORD_VAAPI=off TEST maicord_local_video_preview)
    set_tests_properties(maicord_local_video_preview PROPERTIES SKIP_RETURN_CODE 77)
endif()
maicord_test(MaiCordGlyphEffectTests ONLY WINDOWS ENTRY maicord_glyph_effect_entry
    SOURCES tests/render/text/glyph_effect_ordinal_test.cpp
    BUNDLE MaiCordCoreUnitTests TIMEOUT 2 LABELS native-text-renderer TEST maicord_glyph_effect_ordinals)
if(NOT MAICORD_LINUX)
    maicord_shader(MAICORD_GLYPH_DISTANCE_REFERENCE_HEADER maicord_glyph_distance_reference_cs.hpp cs_5_0
            glyph_coverage g_maicord_glyph_distance_reference_shader tests/shaders/glyph_distance_reference.hlsl)
endif()
maicord_test(MaiCordGlyphDistanceProbe ONLY WINDOWS ENTRY maicord_glyph_distance_entry
    SOURCES tests/integration/glyph_distance_probe.cpp "${MAICORD_GLYPH_DISTANCE_REFERENCE_HEADER}"
    BUNDLE MaiCordGpuUnitTests TIMEOUT 5 LABELS native-text-renderer gpu TEST maicord_glyph_distance)
maicord_test(MaiCordGlyphCapacityProbe ONLY WINDOWS ENTRY maicord_glyph_capacity_entry
    SOURCES tests/integration/glyph_capacity_probe.cpp
    BUNDLE MaiCordGpuUnitTests TIMEOUT 5 LABELS native-text-renderer gpu TEST maicord_glyph_capacity)
maicord_test(MaiCordShaderBytecodeProbe ONLY WINDOWS ENTRY maicord_shader_bytecode_probe_entry
    SOURCES tests/integration/shader_bytecode_probe.cpp
    BUNDLE MaiCordGpuUnitTests TIMEOUT 5 LABELS native-text-renderer gpu TEST maicord_shader_bytecode)
# The ui quads scene through the draw pipeline's Direct3D 11 shaders against tests/gpu/golden/ui_quads.png, which the
# OpenGL ES test of the Linux build (tests/gpu/ui_quads.cpp) also reproduces; tools/update_gpu_goldens.py renews it.
maicord_test(MaiCordGpuUiQuadsProbe ONLY WINDOWS ENTRY maicord_gpu_ui_quads_windows_entry
    SOURCES tests/gpu/ui_quads_windows.cpp
    BUNDLE MaiCordGpuUnitTests TIMEOUT 10 LABELS gpu RESOURCE_LOCK native_gpu TEST maicord_gpu_ui_quads)
# The glyph scene (compute rasterization into an R8 atlas, then the glyph shaders' draw) against
# tests/gpu/golden/glyph_atlas.png and glyph_draw.png, which tests/gpu/glyphs.cpp reproduces on Linux.
maicord_test(MaiCordGpuGlyphsProbe ONLY WINDOWS ENTRY maicord_gpu_glyphs_windows_entry
    SOURCES tests/gpu/glyphs_windows.cpp
    BUNDLE MaiCordGpuUnitTests TIMEOUT 10 LABELS gpu RESOURCE_LOCK native_gpu TEST maicord_gpu_glyphs)
# The media scene (PNG, VP8 and JPEG compute into an RGBA8 atlas with over-blends, then the media shaders' draw)
# against tests/gpu/golden/media_atlas.png and media_draw.png, which tests/gpu/media.cpp reproduces on Linux.
maicord_test(MaiCordGpuMediaProbe ONLY WINDOWS ENTRY maicord_gpu_media_windows_entry
    SOURCES tests/gpu/media_windows.cpp
    BUNDLE MaiCordGpuUnitTests TIMEOUT 10 LABELS gpu RESOURCE_LOCK native_gpu TEST maicord_gpu_media)
# The Direct3D 11 paged texture (src/gpu/d3d11/paged_texture.cpp) on WARP: tiled creation, tile band mappings from the
# pool, unmapped rows reading zero, decommits returning pool memory and recommitted bands starting transparent.
maicord_test(MaiCordGpuPagedTextureProbe ONLY WINDOWS ENTRY maicord_gpu_paged_texture_entry
    SOURCES tests/gpu/paged_texture_windows.cpp
    BUNDLE MaiCordGpuUnitTests TIMEOUT 10 LABELS gpu TEST maicord_gpu_paged_texture)
# The device floor (src/gpu/d3d11/device.cpp): a driver whose ID3D11Device5::CreateFence fails (below WDDM 2.0) is
# refused as unsupported, one that makes fences is accepted.
maicord_test(MaiCordGpuDeviceFloorProbe ONLY WINDOWS ENTRY maicord_gpu_device_floor_entry
    SOURCES tests/gpu/device_floor_windows.cpp
    BUNDLE MaiCordGpuUnitTests TIMEOUT 10 LABELS gpu RESOURCE_LOCK native_gpu TEST maicord_gpu_device_floor)
# Every sample effect and Silk Waves (tests/gpu/effects/*.mcfx, interface 4) through the real pass executor on Direct3D 11:
# the same source runs on Linux against what this captures (tools/update_gpu_goldens.py --scene effects).
maicord_test(MaiCordGpuEffectsProbe ONLY WINDOWS ENTRY maicord_gpu_effects_entry
    SOURCES tests/gpu/effects.cpp
    LIBS maicord_packages maicord_app_core maicord_platform
    BUNDLE MaiCordGpuUnitTests TIMEOUT 60 LABELS gpu RESOURCE_LOCK native_gpu TEST maicord_gpu_effects)
if(MAICORD_LINUX)
    # The GPU tests of the Linux build. They need an EGL driver: Mesa's llvmpipe is the CI default, and a developer GPU
    # is selected the way the driver stack does it (on WSL: GALLIUM_DRIVER=d3d12 tools/build_linux.sh --test ...).
    # The device made by the prestart worker, views that keep their texture, retains, timers and fences on a real driver.
    maicord_test(MaiCordGpuSyncTests ENTRY maicord_gpu_sync_entry
        SOURCES tests/gpu/sync.cpp
        LIBS maicord_gpu_gles maicord_core
        TIMEOUT 20 TEST maicord_gpu_sync)
    maicord_add_tests(MaiCordGpuSyncTests TIMEOUT 20 ENV MAICORD_TEST_PRESTART=0 TEST maicord_gpu_sync_no_prestart)
    # A driver without timer queries: the create fails the way it does on a device without them.
    maicord_add_tests(MaiCordGpuSyncTests TIMEOUT 20 ENV MAICORD_GLES_NO_TIMER_QUERY=1 TEST maicord_gpu_sync_no_timers)
    maicord_test(MaiCordGpuClearReadbackTests ENTRY maicord_gpu_clear_readback_entry
        SOURCES tests/gpu/clear_readback.cpp
        LIBS maicord_gpu_gles maicord_core
        TIMEOUT 10 TEST maicord_gpu_clear_readback)
    # What draws or dispatches needs the shader pack too (the pinned toolchain builds it on first use).
    maicord_test(MaiCordGpuShaderPackTests ENTRY maicord_gpu_shader_pack_entry
        SOURCES tests/gpu/shader_pack.cpp
        LIBS maicord_gpu_gles_shaders maicord_gpu_gles maicord_compression maicord_core INCLUDES src
        TIMEOUT 20 TEST maicord_gpu_shader_pack)
    # A device that offers the 128 invocations per group the specification guarantees: colr_layers links as its narrow variant.
    maicord_add_tests(MaiCordGpuShaderPackTests TIMEOUT 20
        ENV MAICORD_GLES_MAX_COMPUTE_INVOCATIONS=128
        TEST maicord_gpu_shader_pack_small_groups)
    # The ui quads scene against the PNG that tests/gpu/ui_quads_windows.cpp captured from Direct3D 11.
    maicord_test(MaiCordGpuUiQuadsTests ENTRY maicord_gpu_ui_quads_entry
        SOURCES tests/gpu/ui_quads.cpp
        LIBS maicord_gpu_gles_shaders maicord_gpu_gles maicord_compression maicord_core
        TIMEOUT 20 LABELS gpu TEST maicord_gpu_ui_quads)
    # The glyph scene (compute rasterization into the atlas, then the glyph shaders' draw) against the PNGs that
    # tests/gpu/glyphs_windows.cpp captured from Direct3D 11.
    maicord_test(MaiCordGpuGlyphsTests ENTRY maicord_gpu_glyphs_entry
        SOURCES tests/gpu/glyphs.cpp
        LIBS maicord_gpu_gles_shaders maicord_gpu_gles maicord_compression maicord_core
        TIMEOUT 20 LABELS gpu TEST maicord_gpu_glyphs)
    # The media scene (PNG, VP8 and JPEG compute into an RGBA8 atlas, then the media shaders' draw) against the PNGs that
    # tests/gpu/media_windows.cpp captured from Direct3D 11.
    maicord_test(MaiCordGpuMediaTests ENTRY maicord_gpu_media_entry
        SOURCES tests/gpu/media.cpp
        LIBS maicord_gpu_gles_shaders maicord_gpu_gles maicord_compression maicord_core
        TIMEOUT 20 LABELS gpu TEST maicord_gpu_media)
    # Every sample effect and Silk Waves, as interface 4 packages, through the real pass executor against the pictures
    # Direct3D 11 made (the same source runs on Windows and captures them). The cost probe is off so a slow software
    # renderer can neither refuse an effect nor lower its resolution.
    maicord_test(MaiCordGpuEffectsTests ENTRY maicord_gpu_effects_entry
        SOURCES tests/gpu/effects.cpp
        LIBS maicord_effect_host maicord_gpu_gles_shaders maicord_gpu_gles maicord_compression maicord_media_codec
            maicord_core
        INCLUDES src
        TIMEOUT 120 LABELS gpu ENV MAICORD_GLES_NO_TIMER_QUERY=1 TEST maicord_gpu_effects)
    # A window on a compositor showing the same scene through the EGL window surface, the damage blit and the frame
    # callbacks. Opt-in like the Wayland window tests (a window opens): MAICORD_WAYLAND_INTEGRATION=1 at configure time;
    # tools/test_gpu_surface.sh runs it on headless sway with a screenshot check. Status 77: no compositor.
    maicord_test(MaiCordGpuSurfaceWaylandTests ENTRY maicord_gpu_surface_wayland_entry
        SOURCES tests/gpu/surface_wayland.cpp
        LIBS maicord_gpu_gles_shaders maicord_gpu_gles maicord_window_wayland maicord_compression maicord_core
        TIMEOUT 20 LABELS integration TEST maicord_gpu_surface_wayland)
    set_tests_properties(maicord_gpu_surface_wayland PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK wayland_compositor)
    if(NOT "$ENV{MAICORD_WAYLAND_INTEGRATION}")
        set_tests_properties(maicord_gpu_surface_wayland PROPERTIES DISABLED TRUE)
    endif()
    # A colour bitmap glyph (the CBDT fixture) uploaded into the colour atlas and read back from the GPU.
    maicord_test(MaiCordGpuBitmapGlyphTests ENTRY maicord_gpu_bitmap_glyph_entry
        SOURCES tests/gpu/bitmap_glyph.cpp
        LIBS maicord_render_colr_paint maicord_font_color maicord_font maicord_gpu_gles_shaders maicord_gpu_gles
             maicord_compression maicord_core INCLUDES src
        TIMEOUT 20 TEST maicord_gpu_bitmap_glyph)
endif()
if(NOT MAICORD_LINUX)
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
endif()
# One executable, four reference comparisons selected by MAICORD_GLYPH_PROBE.
# Stage and color controls 1 and 2 rerun them with recolored, re-sampled instances.
maicord_test(MaiCordGlyphTopologyProbe ONLY WINDOWS ENTRY maicord_glyph_topology_entry
    SOURCES tests/integration/glyph_topology_probe.cpp "${MAICORD_GLYPH_LIST_VERTEX_HEADER}"
        "${MAICORD_GLYPH_EASING_VERTEX_HEADER}" "${MAICORD_GLYPH_EASING_PIXEL_HEADER}"
        "${MAICORD_GLYPH_STAGE_VERTEX_HEADER}" "${MAICORD_GLYPH_STAGE_PIXEL_HEADER}"
        "${MAICORD_GLYPH_COLOR_PIXEL_HEADER}" "${MAICORD_GLYPH_VERTEX_HEADER}"
    BUNDLE MaiCordGpuUnitTests TIMEOUT 5 LABELS native-text-renderer gpu TEST maicord_glyph_topology)
maicord_add_tests(MaiCordGlyphTopologyProbe TIMEOUT 5 LABELS native-text-renderer gpu
    ENVIRONMENT MAICORD_GLYPH_PROBE=easing TEST maicord_glyph_easing)
foreach(maicord_glyph_probe IN ITEMS stage color)
    maicord_add_tests(MaiCordGlyphTopologyProbe TIMEOUT 5 LABELS native-text-renderer gpu
        ENVIRONMENT MAICORD_GLYPH_PROBE=${maicord_glyph_probe} TEST maicord_glyph_${maicord_glyph_probe})
    foreach(maicord_glyph_control IN ITEMS 1 2)
        maicord_add_tests(MaiCordGlyphTopologyProbe TIMEOUT 5 LABELS native-text-renderer gpu
            ENVIRONMENT MAICORD_GLYPH_PROBE=${maicord_glyph_probe} MAICORD_GLYPH_STAGE_CONTROL=${maicord_glyph_control}
            TEST maicord_glyph_${maicord_glyph_probe}_control_${maicord_glyph_control})
    endforeach()
endforeach()
maicord_test(MaiCordUnicodeTests ENTRY maicord_unicode_tests_entry
    SOURCES tests/text/unicode/unicode_test.cpp tests/text/unicode/grapheme_test.cpp
    LIBS maicord_core maicord_unicode kernel32
    BUNDLE MaiCordTextUnitTests STANDALONE TEST maicord_native_unicode)
maicord_test(MaiCordArabicJoiningTests ENTRY maicord_arabic_joining_tests_entry SOURCES tests/text/scripts/arabic_joining_test.cpp
    BUNDLE MaiCordTextUnitTests TEST maicord_native_arabic_joining)
maicord_test(MaiCordArabicMaskTests ENTRY maicord_arabic_mask_tests_entry SOURCES tests/text/scripts/arabic_mask_test.cpp
    BUNDLE MaiCordTextUnitTests TEST maicord_native_arabic_mask)
maicord_test(MaiCordArabicPlanTests ENTRY maicord_arabic_plan_tests_entry SOURCES tests/text/scripts/arabic_plan_test.cpp
    LIBS maicord_core maicord_unicode maicord_font kernel32
    BUNDLE MaiCordTextUnitTests TEST maicord_native_arabic_plan)
maicord_add_tests(MaiCordArabicPlanTests TIMEOUT 5 LABELS native-text-arabic-run TEST maicord_native_arabic_run --run)
maicord_font_test(MaiCordArabicMarksTests maicord_arabic_marks_tests_entry text/scripts/arabic_marks_test
    TEST maicord_native_arabic_marks)
maicord_test(MaiCordBidiTests ENTRY maicord_bidi_tests_entry SOURCES tests/text/unicode/bidi_test.cpp
    LIBS maicord_core maicord_unicode kernel32
    BUNDLE MaiCordTextUnitTests STANDALONE TEST maicord_native_bidi)
# The TZif reader and POSIX rules (pure; the same test runs in the Linux suite) against zic's output and Python's zoneinfo.
maicord_test(MaiCordTzifTests ENTRY maicord_tzif_entry
    SOURCES tests/platform/time/tzif_test.cpp
    LIBS maicord_core kernel32
    WINDOWS SOURCES src/platform/time/tzif.cpp
        BUNDLE MaiCordCoreUnitTests
    LINUX INCLUDES src
    TEST maicord_tzif)
if(MAICORD_LINUX)
    set(maicord_zoneinfo ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/zoneinfo)
    maicord_test(MaiCordLocalTimeTests ENTRY maicord_local_time_entry
        SOURCES tests/os/local_time_test.cpp
        LIBS maicord_desktop_system maicord_core
        TIMEOUT 10 ENV TZDIR=${maicord_zoneinfo} TZ=Europe/Berlin
        TEST maicord_local_time_berlin berlin)
    maicord_add_tests(MaiCordLocalTimeTests TIMEOUT 10 ENV TZDIR=${maicord_zoneinfo} TZ=:America/New_York
        TEST maicord_local_time_new_york new_york)
    maicord_add_tests(MaiCordLocalTimeTests TIMEOUT 10 ENV TZ=${maicord_zoneinfo}/Asia/Kolkata
        TEST maicord_local_time_path kolkata)
    maicord_add_tests(MaiCordLocalTimeTests TIMEOUT 10 ENV TZDIR=${maicord_zoneinfo} TZ=Australia/Lord_Howe
        TEST maicord_local_time_lord_howe lord_howe)
    maicord_add_tests(MaiCordLocalTimeTests TIMEOUT 10 ENV TZ=EST5EDT,M3.2.0,M11.1.0
        TEST maicord_local_time_rule rule)
    maicord_add_tests(MaiCordLocalTimeTests TIMEOUT 10 ENV TZ=
        TEST maicord_local_time_empty utc)
    maicord_add_tests(MaiCordLocalTimeTests TIMEOUT 10 ENV TZDIR=${maicord_zoneinfo} TZ=Nowhere/Land
        TEST maicord_local_time_missing missing)
    # Whatever zone the host has ($TZ unset: /etc/localtime).
    maicord_add_tests(MaiCordLocalTimeTests TIMEOUT 10 TEST maicord_local_time_system system)
    maicord_add_tests(MaiCordLocalTimeTests TIMEOUT 10 ENV TZDIR=${maicord_zoneinfo} TZ=America/New_York
        TEST maicord_local_time_log log)
    # Date and time formatting (English names; the hour cycle comes from LC_ALL, LC_TIME or LANG, in that order).
    maicord_test(MaiCordDateFormatTests ENTRY maicord_date_format_entry
        SOURCES tests/os/date_format_test.cpp
        LIBS maicord_core
        TIMEOUT 10 ENV LC_ALL=en_US.UTF-8
        TEST maicord_date_format_en_us 12)
    maicord_add_tests(MaiCordDateFormatTests TIMEOUT 10 ENV LC_ALL=de_DE.UTF-8 TEST maicord_date_format_de_de 24)
    maicord_add_tests(MaiCordDateFormatTests TIMEOUT 10 ENV LC_ALL=C TEST maicord_date_format_c 12)
    maicord_add_tests(MaiCordDateFormatTests TIMEOUT 10 ENV LC_ALL=en_GB.UTF-8 TEST maicord_date_format_en_gb 24)
    # LC_TIME beats LANG, and LC_ALL beats both.
    maicord_add_tests(MaiCordDateFormatTests TIMEOUT 10 ENV LC_ALL= LC_TIME=fr_FR.UTF-8 LANG=en_US.UTF-8
        TEST maicord_date_format_lc_time 24)
    maicord_add_tests(MaiCordDateFormatTests TIMEOUT 10 ENV LC_ALL=en_US.UTF-8 LC_TIME=de_DE.UTF-8 LANG=de_DE.UTF-8
        TEST maicord_date_format_lc_all 12)
    maicord_add_tests(MaiCordDateFormatTests TIMEOUT 10 ENV LC_ALL= LC_TIME= LANG=ja_JP.UTF-8
        TEST maicord_date_format_lang 24)
endif()
# The system spell checker, read-only: ISpellChecker on Windows, each backend on Linux (hunspell, enchant). CI may
# pass -ExcludeTestLabels system_spellcheck.
maicord_test(MaiCordSystemSpellcheckTests ENTRY maicord_system_spellcheck_entry
    SOURCES tests/platform/desktop/system_spellcheck.cpp
    WINDOWS LIBS maicord_platform ole32
        BUNDLE MaiCordCoreUnitTests
    LINUX LIBS maicord_desktop_system maicord_core
    TIMEOUT 10 LABELS system_spellcheck ENV LANG=en_US.UTF-8 LC_ALL= LC_MESSAGES=
    TEST maicord_system_spellcheck)
maicord_add_tests(MaiCordSystemSpellcheckTests ONLY LINUX TIMEOUT 10 LABELS system_spellcheck
    ENV MAICORD_SPELLCHECK_BACKEND=enchant LANG=en_US.UTF-8 LC_ALL= LC_MESSAGES=
    TEST maicord_system_spellcheck_enchant)
if(MAICORD_LINUX)
    # The spell checker tests of the Linux build. The "fixture" cases use the dictionary of tests/fixtures/hunspell through
    # $DICPATH; the language cases need the Ubuntu packages hunspell-en-us and hunspell-de-de (MAICORD_TEST_SPELL_SET=ubuntu,
    # which the registry sets) and are skipped without them elsewhere; the system test runs on whatever the host offers with
    # each backend.
    set(maicord_spell_set_environment MAICORD_TEST_SPELL_SET=ubuntu)
    set(maicord_hunspell_fixtures ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hunspell)
    maicord_test(MaiCordSpellcheckLinuxTests ENTRY maicord_spellcheck_linux_entry
        SOURCES tests/platform/desktop/spellcheck_linux_test.cpp
        LIBS maicord_desktop_system maicord_core
        TIMEOUT 10 ENV DICPATH=${maicord_hunspell_fixtures} LANG=xx_XX.UTF-8 LC_ALL= LC_MESSAGES=
        TEST maicord_spellcheck_fixture fixture)
    maicord_add_tests(MaiCordSpellcheckLinuxTests TIMEOUT 10
        ENV DICPATH=${maicord_hunspell_fixtures} LANG=en_US.UTF-8 LC_ALL=xx_XX.UTF-8 LC_MESSAGES=
        TEST maicord_spellcheck_lc_all fixture)
    maicord_add_tests(MaiCordSpellcheckLinuxTests TIMEOUT 10
        ENV DICPATH=${maicord_hunspell_fixtures} LANG=en_US.UTF-8 LC_ALL= LC_MESSAGES=xx_XX
        TEST maicord_spellcheck_lc_messages fixture)
    maicord_add_tests(MaiCordSpellcheckLinuxTests TIMEOUT 10
        ENV MAICORD_HUNSPELL_LIBRARY=libmaicord-no-hunspell.so.0 MAICORD_ENCHANT_LIBRARY=libmaicord-no-enchant.so.2
        TEST maicord_spellcheck_missing missing)
    maicord_add_tests(MaiCordSpellcheckLinuxTests TIMEOUT 10 ENV ${maicord_spell_set_environment} LANG=de_DE.UTF-8
        LC_ALL= LC_MESSAGES=
        TEST maicord_spellcheck_de_de language de-DE)
    # Austrian German has no dictionary of its own here: the base language's (de_DE) is used.
    maicord_add_tests(MaiCordSpellcheckLinuxTests TIMEOUT 10 ENV ${maicord_spell_set_environment} LANG=de_AT.UTF-8
        LC_ALL= LC_MESSAGES=
        TEST maicord_spellcheck_de_at language de)
    maicord_add_tests(MaiCordSpellcheckLinuxTests TIMEOUT 10 ENV ${maicord_spell_set_environment} LANG=C LC_ALL=
        LC_MESSAGES=
        TEST maicord_spellcheck_c language en-US)
    maicord_add_tests(MaiCordSpellcheckLinuxTests TIMEOUT 10 ENV ${maicord_spell_set_environment} LANG=tlh_XX.UTF-8
        LC_ALL= LC_MESSAGES=
        TEST maicord_spellcheck_unknown_language language en-US)
    maicord_add_tests(MaiCordSpellcheckLinuxTests TIMEOUT 10 ENV ${maicord_spell_set_environment} LANG=de_DE.UTF-8
        LC_ALL=en_US.UTF-8 LC_MESSAGES=
        TEST maicord_spellcheck_en_us language en-US)
endif()
# Multi-click pointer selection: click counting, word/line units, drags.
maicord_test(MaiCordTextPointerSelectionTests ENTRY maicord_text_pointer_selection_entry
    SOURCES tests/ui/text/pointer_selection.cpp
    BUNDLE MaiCordUiUnitTests TIMEOUT 10 TEST maicord_text_pointer_selection)
maicord_test(MaiCordLineBreakTests ENTRY maicord_line_break_entry SOURCES tests/text/unicode/line_break_test.cpp
    LIBS maicord_core maicord_unicode kernel32
    BUNDLE MaiCordTextUnitTests STANDALONE TEST maicord_native_line_break)
maicord_font_test(MaiCordLayoutTableTests maicord_layout_tables_entry text/opentype/layout_tables_test STANDALONE
    TEST maicord_native_layout_tables)
maicord_font_test(MaiCordGdefTests maicord_gdef_tests_entry text/opentype/gdef_test STANDALONE TEST maicord_native_gdef)
maicord_font_test(MaiCordLayoutDirectoryTests maicord_layout_directory_entry text/opentype/layout_directory_test STANDALONE
    TEST maicord_native_layout_directory)
maicord_font_test(MaiCordGsubTests maicord_gsub_tests_entry text/opentype/gsub_test STANDALONE TEST maicord_native_gsub)
maicord_font_test(MaiCordContextTests maicord_context_tests_entry text/opentype/context_test STANDALONE TEST maicord_native_context)
maicord_font_test(MaiCordGlyphBufferTests maicord_glyph_buffer_entry text/opentype/glyph_buffer_test STANDALONE)
maicord_font_test(MaiCordGsubExecuteTests maicord_gsub_execute_entry text/opentype/gsub_execute_test STANDALONE)
maicord_font_test(MaiCordClustersTests maicord_clusters_entry text/shaping/clusters_test STANDALONE)
maicord_font_test(MaiCordComponentShapingTests maicord_component_shaping_entry text/opentype/component_shaping_test UNICODE
    STANDALONE TEST maicord_native_component_shaping)
maicord_font_test(MaiCordSyllabicShapeTests maicord_syllabic_shape_entry text/scripts/syllabic_shape_test UNICODE TIMEOUT 5
    LABELS native-text-syllabic TEST maicord_native_syllabic_shape)
maicord_add_tests(MaiCordSyllabicShapeTests ONLY WINDOWS TIMEOUT 5 LABELS native-text-syllabic
    ENVIRONMENT MAICORD_SYLLABIC_OWNED=khmer TEST maicord_native_khmer_owned)
maicord_add_tests(MaiCordSyllabicShapeTests ONLY WINDOWS TIMEOUT 5 LABELS native-text-syllabic
    ENVIRONMENT MAICORD_SYLLABIC_OWNED=myanmar TEST maicord_native_myanmar_owned)
maicord_add_tests(MaiCordSyllabicShapeTests ONLY WINDOWS TIMEOUT 5 LABELS native-text-syllabic
    ENVIRONMENT MAICORD_SYLLABIC_OWNED=sinhala TEST maicord_native_sinhala_owned)
maicord_add_tests(MaiCordSyllabicShapeTests ONLY WINDOWS TIMEOUT 5 LABELS native-text-syllabic
    ENVIRONMENT MAICORD_SYLLABIC_OWNED=tibetan TEST maicord_native_tibetan_owned)
maicord_font_test(MaiCordShapeStageTests maicord_shape_stage_entry text/shaping/shape_stage_test TIMEOUT 5
    LABELS native-text-stages TEST maicord_native_shape_stages)
# Keep large QA corpora out of the C++ parser. Reconfigure and rebuild the
# resource if any binary record changes, including changes of equal size.
if(NOT MAICORD_LINUX)
    set(MAICORD_INDIC_INPUT_IDENTITIES "")
    foreach(script IN ITEMS bengali gujarati gurmukhi odia telugu kannada malayalam devanagari tamil)
        set(fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/generated/${script}_input_fixture.bin")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fixture}")
        file(SHA256 "${fixture}" fixture_sha)
        string(APPEND MAICORD_INDIC_INPUT_IDENTITIES "${script}:${fixture_sha} ")
    endforeach()
    configure_file(tests/text/scripts/indic_input_fixtures.rc.in "${CMAKE_CURRENT_BINARY_DIR}/indic_input_fixtures.rc"
        @ONLY)
endif()
maicord_test(MaiCordDevanagariPlanTests ONLY WINDOWS ENTRY maicord_devanagari_plan_entry
    SOURCES tests/text/scripts/devanagari_plan_test.cpp "${CMAKE_CURRENT_BINARY_DIR}/indic_input_fixtures.rc"
    LIBS maicord_core maicord_font kernel32
    TIMEOUT 5 LABELS native-text-devanagari-plan
    TEST maicord_native_devanagari_plan)
foreach(script IN ITEMS devanagari tamil bengali gujarati gurmukhi odia telugu kannada malayalam)
    maicord_indic_plan_tests(${script} input)
endforeach()
maicord_test(MaiCordFontCatalogTests ONLY WINDOWS ENTRY maicord_font_catalog_entry
    SOURCES tests/text/fonts/catalog_test.cpp
    LIBS maicord_core maicord_font kernel32 advapi32
    TIMEOUT 2 LABELS native-text-font-catalog TEST maicord_native_font_catalog)
maicord_test(MaiCordFontCatalogCoverageTests ONLY WINDOWS ENTRY maicord_font_catalog_coverage_entry
    SOURCES tests/text/fonts/catalog_coverage_test.cpp
    BUNDLE MaiCordTextUnitTests
    TIMEOUT 2 LABELS native-text-font-coverage TEST maicord_native_font_catalog_coverage)
maicord_font_test(MaiCordFontFallbackTests maicord_font_fallback_entry text/fonts/fallback_test
    TEST maicord_native_font_fallback_cascade)
maicord_test(MaiCordFontFallbackFamilyTests ENTRY maicord_font_fallback_family_entry
    SOURCES tests/text/fonts/fallback_family_test.cpp
    BUNDLE MaiCordTextUnitTests TEST maicord_native_font_fallback_families)
maicord_font_test(MaiCordFontFallbackRangeTests maicord_font_fallback_range_entry text/fonts/fallback_range_test
    TEST maicord_native_font_fallback_ranges)
maicord_test(MaiCordFontFamilyTests ENTRY maicord_font_candidates_entry SOURCES tests/text/fonts/candidates_test.cpp
    LIBS maicord_core maicord_font kernel32
    DEFINES MAICORD_FONT_FAMILY=1
    TIMEOUT 10 LABELS native-text-font-family TEST maicord_native_font_family)
maicord_font_test(MaiCordFontMatchTests maicord_font_match_entry text/fonts/match_test TEST maicord_native_font_match)
maicord_font_test(MaiCordFontNamesTests maicord_font_names_entry text/fonts/names_test TIMEOUT 2
    LABELS native-text-font-names TEST maicord_native_font_names)
maicord_font_test(MaiCordRunPlanTests maicord_run_plan_entry text/shaping/run_plan_test TEST maicord_native_run_plans)
# <font> <weight> <command> runs one font through a single check.
maicord_add_tests(MaiCordRunPlanTests TIMEOUT 5
    TEST maicord_native_run_plan_lazy_units "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf" 400 lazy-units
    TEST maicord_native_run_plan_profiles "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf" 400 profiles)
maicord_font_test(MaiCordSimulationPlanTests maicord_simulation_plan_entry text/shaping/simulation_plan_test
    TEST maicord_native_simulation_plans "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Orbitron.ttf")
maicord_font_test(MaiCordLatinRunTests maicord_latin_run_entry text/scripts/latin_run_test TEST maicord_native_latin_guards)
maicord_font_test(MaiCordEmojiRunTests maicord_emoji_run_entry text/scripts/emoji_run_test TIMEOUT 5
    LABELS native-text-emoji-run TEST maicord_native_emoji_runs)
maicord_test(MaiCordEmojiPresentationTests ONLY WINDOWS ENTRY maicord_emoji_presentation_entry
    SOURCES tests/ui/media/emoji_presentation_test.cpp
    BUNDLE MaiCordTextUnitTests TEST maicord_native_emoji_presentation)
maicord_font_test(MaiCordEmojiLineTests maicord_emoji_line_entry text/scripts/emoji_line_test TEST maicord_native_emoji_lines)
maicord_font_test(MaiCordEmojiParagraphTests maicord_emoji_paragraph_entry text/scripts/emoji_paragraph_test
    TEST maicord_native_emoji_paragraphs)
maicord_font_test(MaiCordScriptTests maicord_script_tests_entry text/unicode/script_test UNICODE STANDALONE
    TEST maicord_native_scripts)
maicord_test(MaiCordZstdStreamTests ENTRY maicord_zstd_stream_entry SOURCES tests/media/compression/zstd_stream.cpp
    BUNDLE MaiCordCoreUnitTests TEST maicord_zstd_stream)
maicord_test(MaiCordEtfTranscodeTests ENTRY maicord_etf_transcode_entry
    SOURCES tests/protocol/gateway/etf_transcode.cpp src/protocol/gateway/etf.cpp
    BUNDLE MaiCordCoreUnitTests TEST maicord_etf_transcode)
maicord_test(MaiCordGatewayEtfSessionTests ENTRY maicord_gateway_etf_session_entry
    SOURCES tests/protocol/gateway/etf_session.cpp
    BUNDLE MaiCordCoreUnitTests TEST maicord_gateway_etf_session)
maicord_test(MaiCordByteOrderTests ENTRY maicord_byte_order_entry SOURCES tests/core/byte_order_test.cpp
    BUNDLE MaiCordCoreUnitTests TEST maicord_byte_order)
# The window vocabulary (include/maicord/platform/window): key numbering, event layout, headers compile.
maicord_test(MaiCordWindowVocabularyTests ENTRY maicord_window_vocabulary_entry
    SOURCES tests/platform/window/window_vocabulary.cpp BUNDLE MaiCordCoreUnitTests TEST maicord_window_vocabulary)
# The Windows key-state service against the thread's synthetic keyboard state.
maicord_test(MaiCordWindowWin32KeysTests ONLY WINDOWS ENTRY maicord_window_win32_keys_entry
    SOURCES tests/platform/window/win32_keys.cpp
    LIBS maicord_core maicord_window_win32 kernel32 user32 TIMEOUT 10 TEST maicord_window_win32_keys)
# Windows behaviours the window seam relies on (capture notification timing, context-menu keys), captured in a hidden
# harness window.
maicord_test(MaiCordWindowWin32BehaviourTests ONLY WINDOWS ENTRY maicord_window_win32_behaviour_entry
    SOURCES tests/platform/window/win32_behaviour.cpp
    LIBS maicord_core kernel32 user32 TIMEOUT 10 TEST maicord_window_win32_behaviour)
# The Windows backend's event translation and window procedure (hooks, sink answers, post-actions).
maicord_test(MaiCordWindowWin32EventsTests ONLY WINDOWS ENTRY maicord_window_win32_events_entry
    SOURCES tests/platform/window/win32_events.cpp
    LIBS maicord_core maicord_window_win32 kernel32 user32 TIMEOUT 10 TEST maicord_window_win32_events)
# The headless backend: capture timing as Windows reports it, key and pointer state, probes, injected events.
maicord_test(MaiCordWindowNullTests ONLY WINDOWS ENTRY maicord_window_null_entry
    SOURCES tests/platform/window/null_window.cpp
    LIBS maicord_core maicord_window_null kernel32 TIMEOUT 10 TEST maicord_window_null)
if(MAICORD_LINUX)
    # The tests of the Wayland window backend (src/platform/window/wayland).
    #
    #   maicord_wayland_input     the pure parts: the key table, characters, double click, key repeat with an injected
    #                             clock, the generated protocol tables against libwayland-client's and the key table over
    #                             real xkb layouts (the last two report SKIP when the libraries are not installed).
    #
    # The integration tests need a compositor, so they are opt-in: configure with MAICORD_WAYLAND_INTEGRATION=1 in the
    # environment (tools/test_wayland_window.sh does, after starting sway) and they run; otherwise they are registered
    # disabled. They assert the WindowEvents of a real window.
    #
    #   maicord_wayland_window_lifecycle   the window opens, is configured and acked, answers frame callbacks, takes a wl_shm
    #                                      buffer, the integration protocols (xdg-foreign handle, system bell, icon) and
    #                                      closes; any compositor (sway, Weston, WSLg)
    #   maicord_wayland_window_input       pointer, keyboard, client-side decorations, clipboard, IME,
    #                                      window_post, attention, idle inhibit and the system idle time, driven through a
    #                                      virtual pointer and keyboard and two helper clients (sway and other wlroots
    #                                      compositors)
    maicord_test(MaiCordWaylandInputTests ENTRY maicord_wayland_input_entry
        SOURCES tests/platform/window/wayland_input.cpp
        LIBS maicord_window_wayland maicord_core
        INCLUDES "${MAICORD_WAYLAND_GENERATED_DIRECTORY}"
        DEPENDS maicord_window_wayland
        TIMEOUT 10 TEST maicord_wayland_input)
    maicord_test(MaiCordWaylandWindowTests ENTRY maicord_wayland_window_entry
        SOURCES tests/platform/window/wayland_window.cpp
        LIBS maicord_window_wayland maicord_core
        INCLUDES "${MAICORD_WAYLAND_GENERATED_DIRECTORY}"
        DEPENDS maicord_window_wayland
        TIMEOUT 20 LABELS integration TEST maicord_wayland_window_lifecycle --lifecycle)
    maicord_add_tests(MaiCordWaylandWindowTests TIMEOUT 20 LABELS integration
        TEST maicord_wayland_window_input --input)
    foreach(maicord_wayland_case IN ITEMS maicord_wayland_window_lifecycle maicord_wayland_window_input)
        # Status 77: no compositor to talk to. One window at a time on the compositor.
        set_tests_properties(${maicord_wayland_case} PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK wayland_compositor)
        if(NOT "$ENV{MAICORD_WAYLAND_INTEGRATION}")
            set_tests_properties(${maicord_wayland_case} PROPERTIES DISABLED TRUE)
        endif()
        # The script's directory for the files it shares with a test: one subdirectory a case.
        if(DEFINED ENV{MAICORD_WAYLAND_SYNC_DIR})
            string(REPLACE "maicord_wayland_window_" "" maicord_wayland_name "${maicord_wayland_case}")
            set_tests_properties(${maicord_wayland_case} PROPERTIES
                ENVIRONMENT "MAICORD_WAYLAND_SYNC_DIR=$ENV{MAICORD_WAYLAND_SYNC_DIR}/${maicord_wayland_name}")
        endif()
    endforeach()
    # The input-method text store (text-input-v3 events applied to the bound edit) against the headless window's request
    # log; the Windows store takes composition from TSF, so this test is Linux only.
    maicord_test(MaiCordImeStoreTests ENTRY maicord_ime_store_entry
        SOURCES tests/ui/text/ime_store.cpp
        LIBS maicord_core maicord_window_null maicord_ui maicord_ui_secure_text maicord_text_system
        TIMEOUT 10
        TEST maicord_ime_store)
endif()
# The neutral os layer (include/maicord/os): events, timers and waits, threads, and SRW lock contention.
maicord_test(MaiCordOsWaitTests ENTRY maicord_os_wait_entry SOURCES tests/os/wait.cpp
    BUNDLE MaiCordCoreUnitTests TIMEOUT 10 TEST maicord_os_wait)
maicord_test(MaiCordOsThreadTests ENTRY maicord_os_thread_entry SOURCES tests/os/thread.cpp
    BUNDLE MaiCordCoreUnitTests TIMEOUT 10 TEST maicord_os_thread)
# UTF-16/UTF-8 conversion (include/maicord/os/text.hpp) and starting another program (include/maicord/os/process.hpp).
maicord_test(MaiCordOsTextTests ENTRY maicord_os_text_entry SOURCES tests/os/text.cpp
    BUNDLE MaiCordCoreUnitTests TIMEOUT 10 TEST maicord_os_text)
maicord_test(MaiCordOsProcessTests ENTRY maicord_os_process_entry SOURCES tests/os/process.cpp
    BUNDLE MaiCordCoreUnitTests TIMEOUT 10 TEST maicord_os_process)
# Files, directories and mappings (include/maicord/os/file.hpp); file_snapshot_read comes from maicord_platform.
maicord_test(MaiCordOsFileTests ENTRY maicord_os_file_entry
    SOURCES tests/os/file.cpp
    LIBS maicord_core kernel32
    WINDOWS LIBS maicord_platform
    TIMEOUT 10 TEST maicord_os_file)
# The files seam's path policy (include/maicord/os/path.hpp) and the shared code that asks it (acceptance record,
# attachment name, theme storage and import); one source for both systems.
maicord_test(MaiCordPathPolicyTests ENTRY maicord_path_policy_entry
    SOURCES tests/os/path_policy.cpp
    WINDOWS LIBS maicord_message_runtime maicord_client_preferences maicord_app_core maicord_ui maicord_platform
            maicord_core maicord_text maicord_render_core maicord_model kernel32 user32 bcrypt crypt32 ws2_32
            maicord_sspicli ole32 ntdll
    LINUX LIBS ${maicord_linux_app_test_libs}
    TIMEOUT 20 TEST maicord_path_policy)
if(MAICORD_LINUX)
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
        SOURCES ${maicord_account_fake}
        SOURCES tests/app/account/login/linux_webauthn_flow.cpp src/app/account/login/browser_auth.cpp
            src/app/account/login/captcha_auth.cpp src/app/account/login/auth.cpp src/app/account/login/auth_recovery.cpp
            src/app/shell/state.cpp src/app/account/account_host.cpp
        LIBS ${maicord_linux_app_test_libs}
        INCLUDES src/app
        TIMEOUT 20 TEST maicord_linux_webauthn_flow)
endif()
maicord_test(MaiCordOsSrwLockTests ENTRY maicord_os_srwlock_entry SOURCES tests/os/srwlock.cpp
    BUNDLE MaiCordCoreUnitTests TIMEOUT 15 TEST maicord_os_srwlock)
if(MAICORD_LINUX)
    # The runtime: process entry state, the pinned libc imports, the library search and the XDG data path.
    maicord_test(MaiCordLinuxRuntimeTests ENTRY maicord_linux_runtime_entry SOURCES tests/core/linux_runtime.cpp
        BUNDLE MaiCordCoreUnitTests TIMEOUT 5 TEST maicord_linux_runtime)
    maicord_add_tests(MaiCordLinuxRuntimeTests TIMEOUT 5
        TEST maicord_linux_data_path_create --data-path-create)
    maicord_add_tests(MaiCordLinuxRuntimeTests TIMEOUT 5
        ENV MAICORD_LIBRARY_PATH=/maicord-no-such-directory:/maicord-another
        TEST maicord_linux_library_path --library-path)
    maicord_add_tests(MaiCordLinuxRuntimeTests TIMEOUT 5
        ENV XDG_DATA_HOME=/maicord-xdg-test/data HOME=/maicord-home-test
        TEST maicord_linux_data_path_xdg --data-path-xdg)
    maicord_add_tests(MaiCordLinuxRuntimeTests TIMEOUT 5
        ENV --unset=XDG_DATA_HOME HOME=/maicord-home-test
        TEST maicord_linux_data_path_home --data-path-home)
    maicord_add_tests(MaiCordLinuxRuntimeTests TIMEOUT 5
        ENV XDG_DATA_HOME=relative/data HOME=/maicord-home-test
        TEST maicord_linux_data_path_relative_xdg --data-path-relative-xdg)
    maicord_add_tests(MaiCordLinuxRuntimeTests TIMEOUT 5
        ENV XDG_CACHE_HOME=/maicord-xdg-test/cache XDG_STATE_HOME=/maicord-xdg-test/state HOME=/maicord-home-test
        TEST maicord_linux_cache_state_xdg --cache-state-xdg)
    maicord_add_tests(MaiCordLinuxRuntimeTests TIMEOUT 5
        ENV XDG_CACHE_HOME=relative/cache --unset=XDG_STATE_HOME HOME=/maicord-home-test
        TEST maicord_linux_cache_state_home --cache-state-home)
    # The program resources linked into the image (src/core/linux/resource.cpp), read by their resource ids.
    maicord_test(MaiCordLinuxResourceTests ENTRY maicord_linux_resource_entry SOURCES tests/core/linux_resource.cpp
        BUNDLE MaiCordCoreUnitTests TIMEOUT 10 TEST maicord_linux_resource)
endif()
# The inline TEB/PEB reads in core/win32_process.hpp against the public APIs, on two threads.
maicord_test(MaiCordProcessAccessorTests ONLY WINDOWS ENTRY maicord_process_accessors_entry
    SOURCES tests/core/process_accessors.cpp
    BUNDLE MaiCordCoreUnitTests TIMEOUT 5 TEST maicord_process_accessors)
maicord_font_test(MaiCordFontSessionTests maicord_font_session_entry text/fonts/session_test TIMEOUT 5
    TEST maicord_native_font_session)
maicord_test(MaiCordNativeFormatTests ONLY WINDOWS ENTRY maicord_native_format_entry
    SOURCES tests/ui/text/native_format_test.cpp src/ui/text/native_text_candidate.cpp src/ui/text/native_text_catalog.cpp
        src/ui/media/emoji_presentation.cpp src/core/log.cpp
    LIBS maicord_core maicord_font maicord_media_core maicord_ui kernel32 advapi32 gdi32
    TEST maicord_native_format_ownership "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Compagnon.otf")
maicord_add_tests(MaiCordNativeFormatTests TIMEOUT 2
    TEST maicord_native_recovery_pressure --recovery-pressure "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Compagnon.otf")
# Inline the shared test driver's hot comparisons and query helpers (/Ob2). Its layouts are checked against
# the committed references of tests/ui/text/app_line_reference.hpp (tools/font_reference/text_reference.py
# records them); status 77 means this machine's fonts were never recorded.
if(NOT MAICORD_LINUX)
    add_library(maicord_native_app_line_driver OBJECT tests/ui/text/native_app_line_test.cpp
        tests/ui/text/native_app_line_scripts.cpp tests/ui/text/native_app_line_editing.cpp
        tests/ui/text/app_line_reference.cpp)
    target_link_libraries(maicord_native_app_line_driver PRIVATE maicord_compile_contract)
    set_target_properties(maicord_native_app_line_driver PROPERTIES CXX_EXTENSIONS OFF MAICORD_INLINE_ANY ON)
endif()
maicord_test(MaiCordNativeAppLineTests ONLY WINDOWS ENTRY maicord_native_app_line_entry
    SOURCES $<TARGET_OBJECTS:maicord_native_app_line_driver> src/render/text/edit_layout.cpp
    LIBS maicord_core maicord_ui maicord_text kernel32
    TEST maicord_native_app_lines)
# Cases that can draw with the repository's fonts do (--bundled-fonts: Segoe UI, Georgia and Consolas become MuseoModerno,
# Zilla Slab and BioRhyme), so a machine with other system fonts still checks them; the script, emoji and RTL
# corpora need the system's fonts and skip there (tests/ui/text/app_line_reference.hpp).
set(maicord_bundled_fonts --bundled-fonts "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 2 TEST maicord_native_tsf_points --tsf-points)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 TEST maicord_native_historic --historic)
if(NOT MAICORD_LINUX)
    add_executable(MaiCordNativeTextStoreTests tests/ui/text/native_text_store_test.cpp src/render/text/edit_layout.cpp
        $<TARGET_OBJECTS:maicord_runtime>)
    target_link_libraries(MaiCordNativeTextStoreTests PRIVATE maicord_compile_contract maicord_core maicord_ui
        maicord_text maicord_ui_input kernel32)
    target_link_options(MaiCordNativeTextStoreTests PRIVATE /ENTRY:maicord_native_text_store_entry /SUBSYSTEM:CONSOLE,6.02)
    maicord_add_tests(MaiCordNativeTextStoreTests TIMEOUT 5 PASS_REGULAR_EXPRESSION "PASS 44 "
        TEST maicord_Native_text_store)
endif()
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 2 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    LABELS native-text-app-lines TEST maicord_native_request_recovery --request-recovery)
maicord_add_tests(MaiCordNativeAppLineTests
    TEST maicord_native_fallback_bindings --fallback-bindings
    TEST maicord_native_system_fallback ${maicord_bundled_fonts} --system-fallback-profile --family "Segoe UI" 400
    TEST maicord_native_hebrew_lines --hebrew-profile --family "Segoe UI" 400
    TEST maicord_native_hangul_lines --hangul-profile --family "Segoe UI" 400)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 15 LABELS native-text-thai-shaping
    TEST maicord_native_thai_shaping --thai-shape-profile --family "Segoe UI" 400
    TEST maicord_native_thai_shaping_private --thai-shape-profile
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf")
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 15 LABELS native-text-thai-wrapping
    TEST maicord_native_thai_wrapping --thai-profile --family "Segoe UI" 400
    TEST maicord_native_thai_wrapping_private --thai-profile "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf")
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 15 LABELS native-text-thai-wrapping native-text-thai-context
    TEST maicord_native_thai_context --thai-context-check)
maicord_add_tests(MaiCordNativeAppLineTests TEST maicord_native_font_range_replay --font-replay-check)
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
maicord_native_profile_tests(5 tamil-direction "Nirmala UI" PRIVATE)
maicord_native_profile_tests(5 tamil-app "Nirmala UI" PRIVATE)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-telugu-cpu
    TEST maicord_native_telugu_cpu --telugu-styled)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-kannada-cpu
    TEST maicord_native_kannada_cpu --kannada-styled)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-arabic-cpu
    TEST maicord_native_arabic_cpu --arabic-styled)
maicord_native_profile_tests(5 telugu-app "Nirmala UI")
maicord_native_profile_tests(5 kannada-app "Nirmala UI")
maicord_native_profile_tests(5 sinhala-app "Nirmala UI")
maicord_native_profile_tests(5 khmer-app "Leelawadee UI")
maicord_native_profile_tests(5 myanmar-app "Myanmar Text")
maicord_native_profile_tests(5 tibetan-app "Microsoft Himalaya")
maicord_native_profile_tests(5 malayalam-app "Nirmala UI")
maicord_native_profile_tests(5 odia-app "Nirmala UI")
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-odia-cpu
    TEST maicord_native_odia_cpu --odia-styled)
maicord_native_profile_tests(5 gurmukhi-app "Nirmala UI")
maicord_native_profile_tests(5 gujarati-app "Nirmala UI")
maicord_native_profile_tests(5 bengali-app "Nirmala UI")
maicord_native_profile_tests(10 devanagari-app "Nirmala UI" PRIVATE)
maicord_native_profile_tests(5 devanagari-direction "Nirmala UI" PRIVATE)
maicord_native_profile_tests(5 direction-tracking "Nirmala UI" PRIVATE)
maicord_native_profile_tests(5 arabic-context "Nirmala UI" PRIVATE)
maicord_native_profile_tests(10 devanagari-tracking "Nirmala UI" PRIVATE)
# Every app profile keeps a _default variant (Segoe UI, the script's glyphs through system fallback); the
# private-font variant (_private, BioRhyme) runs for the Tamil and Devanagari profiles, the direction and Arabic
# context profiles, and the Thai, standard and extended script profiles. Tracking under a fallback font runs only in
# the devanagari, direction, Arabic context and Tamil direction profiles; the other scripts' tracking profiles use
# their own family.
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
maicord_add_tests(MaiCordNativeAppLineTests
    TEST maicord_native_bidi_emoji --bidi-emoji-profile --family "Segoe UI" 400)
maicord_add_tests(MaiCordNativeAppLineTests
    TEST maicord_native_symbols ${maicord_bundled_fonts} --symbols-profile --family "Segoe UI" 400
    TEST maicord_native_bidi_style_ranges --bidi-range-check
    TEST maicord_native_simulated_styles --simulated-styles)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-emoji-app
    TEST maicord_native_emoji_dispatch --emoji-dispatch)
maicord_add_tests(MaiCordNativeAppLineTests TEST maicord_native_mixed_emoji --mixed-emoji)
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 5 LABELS native-text-owned-formats
    TEST maicord_native_owned_formats ${maicord_bundled_fonts} --native-formats)
maicord_add_tests(MaiCordNativeAppLineTests TEST maicord_native_tracking ${maicord_bundled_fonts} --tracking-profile --family "Segoe UI" 400)
# The remaining modes (the 1-2 s Arabic, CJK and alphabetic corpora, exhaustive variants of the matrix
# profiles) are manual; tests/ui/text/native_app_line_test.cpp lists them.
maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 10
    TEST maicord_native_font_selection --font-selection-check
    TEST maicord_native_font_context_layouts --font-context-layouts
    TEST maicord_native_han_paragraph --han-paragraph
    TEST maicord_native_paragraphs ${maicord_bundled_fonts} --paragraphs
    TEST maicord_native_paragraph_fuzz ${maicord_bundled_fonts} --paragraph-fuzz
    TEST maicord_native_latin_paragraph_fuzz ${maicord_bundled_fonts} --latin-paragraph-fuzz
    TEST maicord_native_hit_profile ${maicord_bundled_fonts} --hit-profile --family "Segoe UI" 400
    TEST maicord_native_latin_paragraph ${maicord_bundled_fonts} --latin-paragraph --family "Segoe UI" 400
    TEST maicord_native_latin_hits --latin-hits --family "Segoe UI" 400
    TEST maicord_native_paragraph_profile ${maicord_bundled_fonts} --paragraph-profile --family "Segoe UI" 400
    TEST maicord_native_common_punctuation ${maicord_bundled_fonts} --common-punctuation-profile --family "Segoe UI" 400
    TEST maicord_native_directional_large --directional-large-profile --family "Segoe UI" 400
    TEST maicord_native_simulated_profile ${maicord_bundled_fonts} --simulated-profile --family "Segoe UI" 400)
maicord_test(MaiCordNativeSpacingTests ONLY WINDOWS ENTRY maicord_native_spacing_entry
    SOURCES tests/text/opentype/native_spacing_test.cpp
    LIBS maicord_core maicord_font kernel32
    TIMEOUT 5 LABELS native-text-spacing TEST maicord_native_spacing)
maicord_add_tests(MaiCordNativeAppLineTests
    TEST maicord_native_paragraph_options ${maicord_bundled_fonts} --paragraph-options
    TEST maicord_native_paragraph_hits ${maicord_bundled_fonts} --paragraph-hits
    TEST maicord_native_edit_hits ${maicord_bundled_fonts} --edit-hits
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
    TEST maicord_native_height_overflow ${maicord_bundled_fonts} --height-overflow-profile --family "Segoe UI" 400
    TEST maicord_native_ellipsis_sign ${maicord_bundled_fonts} --ellipsis-sign-profile --family "Segoe UI" 400
    TEST maicord_native_ellipsis_cut ${maicord_bundled_fonts} --ellipsis-cut-profile --family "Segoe UI" 400
    TEST maicord_native_ellipsis_line ${maicord_bundled_fonts} --ellipsis-line-profile --family "Segoe UI" 400
    TEST maicord_native_ellipsis_styles --ellipsis-style-profile --family "Segoe UI" 400)
# Each process runs one complete profile. The five-profile union keeps
# all 139,944 matrix cases; --matrix-batch remains an offline full union.
set(maicord_matrix_profile_index 0)
foreach(maicord_matrix_profile IN ITEMS ellipsis_hits multiline_hits
        multiline_private_hits styled decorations)
    # The ellipsis-hit profile counts its Segoe UI cases, so it keeps the system's fonts.
    set(maicord_matrix_fonts ${maicord_bundled_fonts})
    if(maicord_matrix_profile STREQUAL "ellipsis_hits")
        set(maicord_matrix_fonts)
    endif()
    maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT 20 COST 40 ENVIRONMENT LOCALAPPDATA=
        LABELS native-text-hits native-text-styled native-text-decorations native-text-quick
        TEST maicord_native_matrix_${maicord_matrix_profile} ${maicord_matrix_fonts} --matrix-profile ${maicord_matrix_profile_index}
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf")
    math(EXPR maicord_matrix_profile_index "${maicord_matrix_profile_index} + 1")
endforeach()
maicord_add_tests(MaiCordNativeAppLineTests
    TEST maicord_native_decoration_options ${maicord_bundled_fonts} --decoration-options
    TEST maicord_native_semantic_ranges ${maicord_bundled_fonts} --semantic-profile --family "Segoe UI" 400
    TEST maicord_native_directional_semantic_ranges --directional-semantic-profile --family "Segoe UI" 400
    TEST maicord_native_directional_editor_hits --directional-editor-profile --family "Segoe UI" 400
    TEST maicord_native_directional_editor_cuts --directional-editor-cuts-profile --family "Segoe UI" 400
    TEST maicord_native_semantic_options ${maicord_bundled_fonts} --semantic-options
    TEST maicord_native_styled_hits ${maicord_bundled_fonts} --styled-hit-profile --family "Segoe UI" 400
    TEST maicord_native_styled_editor ${maicord_bundled_fonts} --styled-edit)
maicord_add_tests(MaiCordNativeAppLineTests
    TEST maicord_native_style_clusters ${maicord_bundled_fonts} --style-cluster-profile --family "Segoe UI" 400
    TEST maicord_native_style_continuity ${maicord_bundled_fonts} --continuity-profile --family "Segoe UI" 400
    TEST maicord_native_style_cluster_parity ${maicord_bundled_fonts} --style-clusters
    TEST maicord_native_style_continuity_pixelify --continuity-profile
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Pixelify.ttf" 400)
maicord_font_test(MaiCordCursiveDeferredTests maicord_cursive_deferred_entry text/opentype/cursive_deferred_test STANDALONE
    TEST maicord_native_cursive_deferred)
maicord_font_test(MaiCordLookupPlanTests maicord_lookup_plan_entry text/opentype/lookup_plan_test STANDALONE
    TEST maicord_native_lookup_plan)
maicord_font_test(MaiCordNominalParticipationTests maicord_nominal_participation_entry text/opentype/nominal_participation_test
    STANDALONE TEST maicord_native_nominal_participation)
maicord_font_test(MaiCordNominalSourcesTests maicord_nominal_sources_entry text/opentype/nominal_sources_test STANDALONE
    TEST maicord_native_nominal_sources)
maicord_font_test(MaiCordNominalVariationsTests maicord_nominal_variations_entry text/opentype/nominal_variations_test STANDALONE
    TEST maicord_native_nominal_variations)
maicord_font_test(MaiCordMetricVariationsTests maicord_metric_variations_entry text/opentype/metric_variations_test STANDALONE
    TEST maicord_native_metric_variations)
maicord_font_test(MaiCordGvarOutlineTests maicord_gvar_outline_entry text/opentype/gvar_outline_test STANDALONE
    TEST maicord_native_gvar_outlines)
maicord_font_test(MaiCordTtInterpreterTests maicord_tt_interpreter_entry text/opentype/tt_interpreter_test STANDALONE)
maicord_font_test(MaiCordTtOrientationTests maicord_tt_orientation_entry text/opentype/tt_orientation_test
    TEST maicord_native_tt_orientation)
maicord_add_tests(MaiCordTtInterpreterTests TEST maicord_native_tt_interpreter)
maicord_font_test(MaiCordTtOutlineTests maicord_tt_outline_entry text/opentype/tt_outline_test STANDALONE
    TEST maicord_native_tt_bold bold
    TEST maicord_native_tt_bold_oblique bold-oblique
    TEST maicord_native_tt_outlines normal
    TEST maicord_native_tt_oblique oblique
    TEST maicord_native_tt_positioning position)
maicord_font_test(MaiCordFontInstanceTests maicord_font_store_entry text/fonts/store_test STANDALONE TIMEOUT 5
    LABELS native-text-font-instance TEST maicord_native_font_instances)
# The font store, catalog check and handle stamp over real files, through the files seam.
maicord_test(MaiCordFontStoreFilesTests ENTRY maicord_font_store_files_entry SOURCES
        tests/text/fonts/store_files_test.cpp
    LIBS maicord_core maicord_font kernel32 TIMEOUT 5 TEST maicord_native_font_store_files)
if(MAICORD_LINUX)
    # The tests of the Linux text platform: the system font locator over fontconfig and over the directory scan, and what
    # builds on it. They read the host's installed fonts; the cases that
    # need the Ubuntu font packages (fonts-dejavu-core, fonts-noto-core, fonts-noto-cjk, fonts-noto-color-emoji) run when
    # MAICORD_TEST_FONT_SET=ubuntu says so, which the registry sets (tools/build_linux.sh's host notes name the packages);
    # any other system runs the same executable with the variable unset.
    set(maicord_font_set_environment MAICORD_TEST_FONT_SET=ubuntu)
    maicord_test(MaiCordFontLocatorTests ENTRY maicord_font_locator_entry
        SOURCES tests/platform/font/system_font_locator.cpp
        LIBS maicord_font_system maicord_font maicord_core
        TIMEOUT 10 ENV ${maicord_font_set_environment}
        TEST maicord_font_locator)
    maicord_add_tests(MaiCordFontLocatorTests TIMEOUT 10 ENV ${maicord_font_set_environment}
        TEST maicord_font_locator_scan --scan)
    maicord_add_tests(MaiCordFontLocatorTests TIMEOUT 10
        ENV FONTCONFIG_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fonts/fonts.conf
        TEST maicord_font_locator_fonts_conf --fonts-conf)
    # CBLC/CBDT colour bitmaps: the generated fixture and the installed Noto Color Emoji (strict with the Ubuntu set).
    maicord_test(MaiCordCbdtTests ENTRY maicord_cbdt_entry SOURCES tests/text/opentype/cbdt_test.cpp
        LIBS maicord_font_color maicord_font_system maicord_font maicord_media_codec maicord_compression maicord_core
        TIMEOUT 15 ENV ${maicord_font_set_environment}
        TEST maicord_native_cbdt)
    maicord_add_tests(MaiCordCbdtTests TIMEOUT 5 TEST maicord_native_cbdt_fixture --fixture)
    # The text system over the installed fonts: shaping smoke checks (Latin, CJK, Devanagari, Arabic, Thai, Hebrew, emoji)
    # and the language-specific CJK fallback order, and a bold chat paragraph whose emoji fall back to the bitmap colour
    # face (refused, not crashed on).
    maicord_test(MaiCordSystemTextTests ENTRY maicord_system_text_entry SOURCES tests/ui/text/system_text_linux_test.cpp
        LIBS maicord_text_system maicord_text_catalog maicord_font_system maicord_font maicord_unicode maicord_core
        TIMEOUT 15 ENV ${maicord_font_set_environment}
        TEST maicord_native_system_text)
    # The native text catalog: family lookup, the fallback routes and the generated Linux table.
    maicord_test(MaiCordTextCatalogTests ENTRY maicord_text_catalog_entry SOURCES tests/ui/text/native_catalog_linux_test.cpp
        LIBS maicord_text_catalog maicord_font_system maicord_font_color maicord_font maicord_media_codec maicord_compression
            maicord_unicode maicord_core
        TIMEOUT 15 ENV ${maicord_font_set_environment}
        TEST maicord_native_text_catalog)
    # The language picks the CJK order (fontconfig's sort for the language, the generated table's variants without it).
    foreach(language IN ITEMS "ja_JP.UTF-8|Noto Sans CJK JP" "ko_KR.UTF-8|Noto Sans CJK KR" "zh_CN.UTF-8|Noto Sans CJK SC"
            "zh_TW.UTF-8|Noto Sans CJK TC")
        string(REPLACE "|" ";" language "${language}")
        list(GET language 0 locale)
        list(GET language 1 family)
        string(REGEX REPLACE "_.*" "" short "${locale}")
        string(REGEX REPLACE "^[a-z]+_([A-Z]+).*" "\\1" territory "${locale}")
        string(TOLOWER "${short}_${territory}" name)
        maicord_add_tests(MaiCordSystemTextTests TIMEOUT 15 ENV ${maicord_font_set_environment} LC_ALL=${locale}
            TEST maicord_native_system_text_${name} --language "${family}")
    endforeach()
    maicord_add_tests(MaiCordTextCatalogTests TIMEOUT 15 ENV ${maicord_font_set_environment}
        TEST maicord_native_text_catalog_scan --scan)
endif()
maicord_font_test(MaiCordGposTests maicord_gpos_tests_entry text/opentype/gpos_test STANDALONE)
# Basic, mark, cursive and ligature placement corpora in one run.
maicord_font_test(MaiCordGposExecuteTests maicord_gpos_execute_tests_entry text/opentype/gpos_execute_test STANDALONE TIMEOUT 5
    LABELS native-text-gpos-marks TEST maicord_native_gpos_execute)
maicord_font_test(MaiCordGposAttachmentTests maicord_gpos_attachment_tests_entry text/opentype/gpos_attachment_test STANDALONE
    TEST maicord_native_gpos_attachments)
maicord_add_tests(MaiCordGposTests TEST maicord_native_gpos)
maicord_add_tests(MaiCordClustersTests TEST maicord_native_clusters)
maicord_add_tests(MaiCordGsubExecuteTests TEST maicord_native_gsub_execute)
maicord_add_tests(MaiCordGlyphBufferTests TEST maicord_native_glyph_buffer)
maicord_test(MaiCordFontTests ENTRY maicord_font_tests_entry
    SOURCES tests/text/opentype/sfnt_test.cpp tests/text/opentype/tt_bold_points_test.cpp
        tests/text/opentype/truetype_outline_test.cpp tests/text/opentype/cff_outline_test.cpp tests/text/opentype/color_font_test.cpp
        tests/text/opentype/variation_store_test.cpp tests/text/fonts/axes_test.cpp
    LIBS maicord_core maicord_font kernel32
    BUNDLE MaiCordTextUnitTests STANDALONE TEST maicord_native_font)
maicord_test(MaiCordColrProgramTests ONLY WINDOWS ENTRY maicord_colr_program_tests_entry
    SOURCES tests/render/effects/colr_program_test.cpp tests/render/effects/colr_source_test.cpp
    LIBS maicord_core maicord_font maicord_paint_program kernel32
    BUNDLE MaiCordTextUnitTests STANDALONE TEST maicord_colr_program)
# The Lottie geometry engine on a fixed list of shapes, combinations and strokes, against what Direct2D answered for
# them (tests/render/effects/golden/lottie_geometry.bin), and on cases with exact answers.
maicord_test(MaiCordLottieGeometryTests ENTRY maicord_lottie_geometry_entry
    SOURCES tests/render/effects/lottie_geometry_test.cpp
    LIBS maicord_vector_geometry maicord_core kernel32
    TIMEOUT 20 TEST maicord_lottie_geometry)
# The Lottie compiler end to end on the generated corpus, against the digests of its compiled masks that Direct2D's
# geometry produced (MAICORD_LOTTIE_PROGRAM_CAPTURE=<file> writes the digests of the current build).
maicord_test(MaiCordLottieProgramTests ENTRY maicord_lottie_program_entry
    SOURCES tests/render/effects/lottie_program_test.cpp
    WINDOWS LIBS maicord_core maicord_font maicord_paint_program kernel32
    LINUX LIBS maicord_paint_program maicord_vector_geometry maicord_render_core maicord_font maicord_media_core
            maicord_core
    TIMEOUT 20 TEST maicord_lottie_program)
if(NOT MAICORD_LINUX)
    maicord_shader(maicord_vp8_reference_reconstruct_header vp8_reference_reconstruct.hpp cs_5_0 vp8_reconstruct
            g_vp8_reference_reconstruct tests/fixtures/vp8_reconstruct_reference.hlsl)
    maicord_shader(maicord_vp8_reference_idct_header vp8_reference_idct.hpp cs_5_0 vp8_inverse_dct
            g_vp8_reference_idct tests/fixtures/vp8_reconstruct_reference.hlsl)
    maicord_shader(maicord_vp8_reference_filter_header vp8_reference_filter.hpp cs_5_0 vp8_filter
            g_vp8_reference_filter tests/fixtures/vp8_filter_reference.hlsl)
    maicord_shader(maicord_video_frame_reference_header video_frame_reference.hpp cs_5_0 cs_main
            g_video_frame_reference tests/fixtures/video_frame_reference.hlsl)
    # Background effect packages (include/maicord/sdk/background.hlsli) as byte arrays: the reference effect the probe
    # checks the pass against, the pass graph it checks the graph pass against, and the samples that ship
    # (samples/effects/<name>/<name>.hlsl). An item is name|source, or name|source|entry|profile for one shader of a graph.
    set(maicord_effect_headers)
    foreach(maicord_effect IN ITEMS reference|tests/shaders/background_effect_reference.hlsl
            ascii_rain|samples/effects/ascii-rain/ascii-rain.hlsl aurora|samples/effects/aurora/aurora.hlsl
            starfield|samples/effects/starfield/starfield.hlsl plasma|samples/effects/plasma/plasma.hlsl
            bokeh|samples/effects/bokeh/bokeh.hlsl wave_lines|samples/effects/wave-lines/wave-lines.hlsl
            film_grain|samples/effects/film-grain/film-grain.hlsl day_night|samples/effects/day-night/day-night.hlsl
            graph_count|tests/shaders/background_effect_graph.hlsl|count_ps|ps_5_0
            graph_half|tests/shaders/background_effect_graph.hlsl|half_ps|ps_5_0
            graph_fill|tests/shaders/background_effect_graph.hlsl|fill_ps|ps_5_0
            graph_grid|tests/shaders/background_effect_graph.hlsl|grid_vs|vs_5_0
            graph_solid|tests/shaders/background_effect_graph.hlsl|solid_ps|ps_5_0
            graph_quad|tests/shaders/background_effect_graph.hlsl|quad_vs|vs_5_0
            spark_ribbon_simulate|samples/effects/spark-ribbon/spark-ribbon.hlsl|simulate|ps_5_0
            spark_ribbon_ribbon_vs|samples/effects/spark-ribbon/spark-ribbon.hlsl|ribbon_vs|vs_5_0
            spark_ribbon_ribbon|samples/effects/spark-ribbon/spark-ribbon.hlsl|ribbon|ps_5_0
            spark_ribbon_compose|samples/effects/spark-ribbon/spark-ribbon.hlsl|compose|ps_5_0
            spark_ribbon_spark_vs|samples/effects/spark-ribbon/spark-ribbon.hlsl|spark_vs|vs_5_0
            spark_ribbon_spark|samples/effects/spark-ribbon/spark-ribbon.hlsl|spark|ps_5_0)
        string(REPLACE "|" ";" maicord_effect "${maicord_effect}")
        list(GET maicord_effect 0 maicord_effect_name)
        list(GET maicord_effect 1 maicord_effect_source)
        set(maicord_effect_entry main)
        set(maicord_effect_profile ps_5_0)
        list(LENGTH maicord_effect maicord_effect_fields)
        if(maicord_effect_fields EQUAL 4)
            list(GET maicord_effect 2 maicord_effect_entry)
            list(GET maicord_effect 3 maicord_effect_profile)
        endif()
        set(maicord_effect_header "${MAICORD_GENERATED_DIRECTORY}/maicord_effect_${maicord_effect_name}.hpp")
        add_custom_command(OUTPUT "${maicord_effect_header}"
            COMMAND "${MAICORD_FXC_EXECUTABLE}" /nologo /T ${maicord_effect_profile} /E ${maicord_effect_entry} /O3 /Ges /WX
                /Qstrip_debug /Qstrip_reflect /Qstrip_priv /I "${CMAKE_CURRENT_SOURCE_DIR}/include"
                /Fh "${maicord_effect_header}" /Vn g_maicord_effect_${maicord_effect_name}
                "${CMAKE_CURRENT_SOURCE_DIR}/${maicord_effect_source}"
            DEPENDS ${maicord_effect_source} include/maicord/sdk/background.hlsli VERBATIM)
        list(APPEND maicord_effect_headers "${maicord_effect_header}")
    endforeach()
endif()
maicord_test(MaiCordUiProbe ONLY WINDOWS WIN32 ENTRY maicord_ui_probe_entry
    SOURCES ${maicord_account_fake}
    SOURCES ${maicord_effect_headers} config/maicord.manifest tests/integration/ui_probe.cpp
        tests/integration/ui_probe_media.cpp tests/integration/ui_probe_scenes.cpp
        tests/integration/ui_probe_renderer.cpp
        tests/integration/poll_scene_ui_probe.cpp
        tests/integration/profile_editor_ui_probe.cpp tests/integration/navigation_gaps_ui_probe.cpp
        tests/integration/reaction_intent_ui_adapter.cpp tests/integration/thread_membership_ui_adapter.cpp
        tests/integration/reply_draft_ui_adapter.cpp tests/integration/thread_browser_ui_adapter.cpp
        src/app/chat/threads/browser.cpp tests/integration/thread_creation_ui_adapter.cpp
        src/app/chat/threads/creation.cpp tests/integration/forum_post_ui_adapter.cpp
        src/platform/desktop/clipboard_attachment.cpp src/app/chat/threads/forum_post.cpp src/app/chat/threads/forum_form.cpp
        src/app/chat/threads/forum_attachments.cpp src/app/media/upload_lane.cpp
        src/app/chat/threads/forum_preview.cpp src/app/chat/composer/preview.cpp
        src/app/media/local_preview.cpp src/app/media/attachment_allowance.cpp
        src/render/text/forum_post_surface.cpp src/app/chat/composer/reply_draft.cpp src/app/chat/messages/edit.cpp
        src/app/chat/messages/delete.cpp src/app/chat/threads/actions.cpp
        src/app/chat/threads/header_activation.cpp src/app/chat/threads/header.cpp
        src/app/chat/messages/reaction.cpp src/app/chat/messages/mutation_notice.cpp
        tests/integration/media_picker_ui_adapter.cpp tests/app/guild/stream_preview_media_stub.cpp
        tests/integration/channel_overview_ui_adapter.cpp tests/integration/regional_flag_probe.cpp
        tests/integration/regional_flag_surfaces_probe.cpp tests/integration/inline_flags_probe.cpp
        tests/integration/guild_rail_icon_probe.cpp tests/integration/background_theme_probe.cpp
        tests/integration/background_theme_pixels.cpp tests/integration/background_effect_pixels.cpp
        tests/integration/frame_damage_probe.cpp
        tests/integration/plugin_ui_probe.cpp tests/integration/plugin_navigation_ui_probe.cpp
        tests/integration/settings_choice_ui_probe.cpp tests/integration/user_settings_ui_probe.cpp
        tests/integration/resize_text_ui_probe.cpp
        tests/integration/store_ui_probe.cpp src/app/store/store_projection.cpp src/app/store/store_page.cpp src/app/store/store_detail.cpp
        src/app/store/store_dialogs.cpp src/app/store/store_act.cpp src/app/store/store_preview_decode.cpp
        src/app/settings/catalog.cpp src/app/update/updates.cpp
        tests/integration/notification_settings_ui_probe.cpp tests/integration/shortcuts_ui_probe.cpp
        tests/integration/call_scene_ui_probe.cpp tests/integration/inbox_ui_probe.cpp
        tests/integration/caption_ui_probe.cpp src/app/shell/app_icon.cpp config/maicord.rc
        tests/integration/showcase_probe.cpp tests/integration/showcase_probe_art.cpp
        tests/integration/login_recovery_ui_probe.cpp
        tests/ui/account/login_reset_ui_probe.cpp tests/integration/login_phone_ui_probe.cpp
        tests/integration/login_browser_ui_probe.cpp tests/integration/member_paging_probe.cpp
        tests/integration/thread_members_probe.cpp tests/integration/read_state_probe.cpp
        tests/integration/text_form_probe.cpp tests/integration/application_text_form_probe.cpp
        src/app/shell/button_input.cpp src/app/chat/composer/text_form.cpp
        src/app/chat/composer/text_input.cpp src/app/shell/state.cpp
        tests/integration/video_retention_probe.cpp tests/integration/video_scene_ui_probe.cpp
        tests/integration/audio_scene_ui_probe.cpp tests/integration/tooltip_ui_probe.cpp
        tests/integration/account_menu_ui_probe.cpp tests/integration/toast_ui_probe.cpp
        tests/integration/search_ui_probe.cpp tests/integration/application_media_animation_ui_probe.cpp
        tests/integration/profile_media_sequence_ui_probe.cpp
        src/app/media/media.cpp src/app/media/animation.cpp src/app/media/requests.cpp src/app/media/requests_message.cpp
        tests/integration/menu_row_ui_probe.cpp tests/integration/animation_preview_probe.cpp
        tests/integration/png_preview_probe.cpp tests/integration/gif_scroll_probe.cpp
        tests/integration/media_memory_probe.cpp tests/integration/vp8_reconstruct_probe.cpp
        "${maicord_vp8_reference_reconstruct_header}" "${maicord_vp8_reference_idct_header}"
        "${maicord_vp8_reference_filter_header}" tests/integration/video_frame_reference_probe.cpp
        "${maicord_video_frame_reference_header}" tests/support/media_fixture.cpp
        tests/support/media_fixture_encoders.cpp src/core/log.cpp
    LIBS maicord_core maicord_text maicord_ui maicord_ui_input maicord_model maicord_packages maicord_client_preferences
        maicord_message_runtime maicord_render_core maicord_media_codec maicord_atlas_gpu
        maicord_renderer maicord_platform maicord_protocol maicord_app_core maicord_message_media maicord_account_media
        kernel32 user32 ${maicord_tls_libs} d3d11 dxgi dcomp dwmapi
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}" src/app
    DEFINES MAICORD_CLIPBOARD_UI_FIXTURE=1 MAICORD_LOG_STDOUT=1
    PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
if(NOT MAICORD_LINUX)
    # The constinit probe state (renderer plus every scene owner) exceeds the
    # default constant-evaluation step budget.
    set_source_files_properties(tests/integration/ui_probe.cpp tests/integration/showcase_probe.cpp
        PROPERTIES COMPILE_OPTIONS /clang:-fconstexpr-steps=8388608)
    # These units are mostly their own code, so unoptimized without the
    # application PCH (which is /O2) compiles them several times faster.
    set_source_files_properties(tests/integration/ui_probe.cpp tests/integration/ui_probe_media.cpp
        tests/integration/ui_probe_scenes.cpp tests/integration/ui_probe_renderer.cpp
        tests/app/settings/settings_runtime.cpp tests/app/settings/settings_runtime_stubs.cpp
        PROPERTIES MAICORD_NO_APPLICATION_PCH ON)
endif()
# The single-scene WIN32 probes link the renderer stack.
set(maicord_scene_probe_libs maicord_core maicord_text maicord_ui maicord_ui_input maicord_model
    maicord_message_runtime maicord_render_core maicord_media_codec maicord_atlas_gpu maicord_renderer
    maicord_platform maicord_protocol maicord_app_core maicord_message_media maicord_account_media)
set(maicord_scene_probe_system_libs kernel32 user32 ws2_32 maicord_sspicli bcrypt crypt32 d3d11 dxgi dcomp shell32)
# The renderer scenes of tests/render/scenes.cpp, which the Linux build runs on an offscreen surface.
maicord_test(MaiCordRenderScenesTests ENTRY maicord_render_scenes_entry
    SOURCES tests/render/scenes.cpp
    WINDOWS SOURCES src/core/log.cpp
        LIBS ${maicord_scene_probe_libs} ${maicord_scene_probe_system_libs}
        INCLUDES "${MAICORD_GENERATED_DIRECTORY}" src src/app
    LINUX SOURCES src/app/chat/messages/timeline.cpp
        LIBS ${maicord_render_test_libs}
        INCLUDES src "${MAICORD_GENERATED_DIRECTORY}"
    TIMEOUT 20 LABELS gpu RESOURCE_LOCK native_gpu TEST maicord_render_scenes)
maicord_test(MaiCordMessageLinkUiProbe ONLY WINDOWS WIN32 ENTRY maicord_message_link_ui_probe_entry
    SOURCES ${maicord_account_fake}
    SOURCES config/maicord.manifest tests/integration/message_link_ui_probe.cpp src/app/shell/content_input.cpp
        src/app/chat/search/navigation.cpp src/app/chat/messages/mutation_notice.cpp
        src/app/chat/threads/actions.cpp src/core/log.cpp src/app/chat/application_channel_memory.cpp
    LIBS ${maicord_scene_probe_libs} ${maicord_scene_probe_system_libs}
    INCLUDES src/app
    RESOURCE_LOCK native_gpu
    TEST maicord_message_link_ui_probe)
maicord_test(MaiCordComposerUiProbe ONLY WINDOWS WIN32 ENTRY maicord_composer_ui_probe_entry
    SOURCES ${maicord_account_fake}
    SOURCES config/maicord.manifest tests/integration/composer_ui_probe.cpp src/app/chat/composer/composer.cpp
        src/app/chat/composer/input.cpp src/app/chat/composer/reply_draft.cpp
        src/app/chat/composer/edit_menu.cpp src/app/chat/messages/send.cpp src/core/log.cpp
        tests/support/pipeline_host_stubs.cpp
    LIBS ${maicord_scene_probe_libs} ${maicord_scene_probe_system_libs}
    INCLUDES src/app
    RESOURCE_LOCK native_gpu
    TEST maicord_composer_ui_probe)
maicord_test(MaiCordAccountLogoutUiProbe ONLY WINDOWS WIN32 ENTRY maicord_account_logout_ui_probe_entry
    SOURCES ${maicord_account_fake}
    SOURCES config/maicord.manifest tests/integration/account_logout_ui_probe.cpp src/app/chat/composer/text_form.cpp
        src/app/account/login/ui.cpp src/app/account/login/input.cpp src/app/account/account_menu.cpp
        src/app/shell/input.cpp src/core/log.cpp
    LIBS ${maicord_scene_probe_libs} maicord_account_host maicord_account_paths ${maicord_scene_probe_system_libs}
    INCLUDES src/app
    RESOURCE_LOCK native_gpu ENV MAICORD_UI_ACCOUNT_LOGOUT_PROBE=1
    TEST maicord_account_logout_ui_probe)
maicord_test(MaiCordScopedReadUiProbe ONLY WINDOWS WIN32 ENTRY maicord_scoped_read_ui_probe_entry
    SOURCES ${maicord_account_fake}
    SOURCES config/maicord.manifest tests/integration/scoped_read_ui_probe.cpp src/app/chat/scoped_read.cpp
        src/app/chat/read_state.cpp src/app/chat/scoped_read_menu.cpp
        src/app/chat/messages/mutation_notice.cpp src/app/guild/menu.cpp
        tests/integration/scoped_read_menu_scroll.cpp src/app/shell/scroll_host.cpp src/core/log.cpp
    LIBS ${maicord_scene_probe_libs} maicord_account_host maicord_account_paths ${maicord_scene_probe_system_libs}
    INCLUDES src/app
    OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/scoped_read_clock_fixture.hpp"
    RESOURCE_LOCK native_gpu ENV MAICORD_UI_SCOPED_READ_PROBE=1
    TEST maicord_scoped_read_ui_probe)
maicord_ui_probe_test(maicord_ui_probe UI_PROBE_ONCE LABELS gpu RESOURCE_LOCK native_gpu_c)
# Hosted CI renders in software; this probe needs a hardware GPU to finish in time.
# The GPU-locked probes run as three chains (native_gpu, native_gpu_b and native_gpu_c): this one, the longest, holds
# native_gpu_b alone; frame damage, forum media, the full UI probe and the hardware stream decoders share native_gpu_c;
# the many short probes share native_gpu. The suite's wall time is then the longest chain, not their sum. The pacing
# probe holds all three: nothing else draws while it measures frame pacing.
# Background canvases, Stars/Comets and package effects against CPU references; the samples rendered at 1440p.
maicord_ui_probe_test(maicord_background_theme_probe UI_BACKGROUND_THEME_PROBE BOOTSTRAP TIMEOUT 20 LABELS gpu display
    RESOURCE_LOCK native_gpu_b)
# Every present (scroll, hover, animated media, background effect ticks, theme
# and size changes) equals a full render of the same state, buffer age included.
maicord_ui_probe_test(maicord_frame_damage_probe UI_FRAME_DAMAGE_PROBE BOOTSTRAP TIMEOUT 20 LABELS gpu display
    RESOURCE_LOCK native_gpu_c)
maicord_ui_probe_test(maicord_inline_media_controls_ui_probe VIDEO_CONTROLS_UI_PROBE BOOTSTRAP TIMEOUT 20 COST 50
    RESOURCE_LOCK native_gpu)
# Rail server icons through layout and present: static, animated at rest and
# hovered, and still-loading icons must each build, queue the right resident
# rendition and present (a failed build_layout presents nothing).
maicord_ui_probe_test(maicord_guild_rail_icon_probe UI_GUILD_RAIL_ICON_PROBE BOOTSTRAP TIMEOUT 20
    LABELS native-ui display RESOURCE_LOCK native_gpu)
# Profiles settings page, preview card, cropper and decoration picker; Ctrl+A over a multi-line Bio.
maicord_ui_probe_test(maicord_profile_editor_ui_probe PROFILE_EDITOR_UI_PROBE BOOTSTRAP TIMEOUT 20
    LABELS native-ui display RESOURCE_LOCK native_gpu)
# Rich presence cards in the profile popout and full profile.
maicord_ui_probe_test(maicord_profile_activity_ui_probe PROFILE_ACTIVITY_UI_PROBE BOOTSTRAP TIMEOUT 20
    LABELS native-ui display RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_shortcuts_ui_probe UI_SHORTCUTS_PROBE BOOTSTRAP TIMEOUT 20 LABELS native-ui display
    RESOURCE_LOCK native_gpu)
# Growable media atlases, frame caches and a 1080p DXVA stream: pixels
# across growth/trim and DXGI memory (MAICORD_MEDIA_MEMORY_REPORT=<file>).
# DXGI usage is per process, so this probe needs no GPU resource lock.
maicord_ui_probe_test(maicord_media_memory_probe MEDIA_MEMORY_PROBE BOOTSTRAP TIMEOUT 20 LABELS gpu)
# Call view grid/focus/fullscreen with synthetic NV12 camera pictures.
maicord_ui_probe_test(maicord_call_ui_probe UI_CALL_PROBE BOOTSTRAP TIMEOUT 20 LABELS native-ui display
    RESOURCE_LOCK native_gpu)
# Exit 77: the display is asleep and DWM composes at ~4 Hz, which no paced
# frame can meet; CTest reports a skip instead of a pacing failure.
maicord_ui_probe_test(maicord_post_resize_pacing_probe UI_POST_RESIZE_PACING_PROBE BOOTSTRAP
    LABELS native-ui display RESOURCE_LOCK native_gpu native_gpu_b native_gpu_c SKIP_RETURN_CODE 77)
maicord_ui_probe_test(maicord_unread_boundary_probe UI_UNREAD_BOUNDARY_PROBE BOOTSTRAP
    LABELS native-ui read-state display RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_rich_search_probe UI_RICH_SEARCH_PROBE BOOTSTRAP LABELS native-ui search
    RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_reaction_intent_probe UI_REACTION_INTENT_PROBE BOOTSTRAP
    LABELS native-ui reactions RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_thread_membership_probe UI_THREAD_MEMBERSHIP_PROBE BOOTSTRAP
    LABELS native-ui threads RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_thread_browser_probe UI_THREAD_BROWSER_PROBE BOOTSTRAP
    LABELS native-ui threads RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_reply_draft_probe UI_REPLY_DRAFT_PROBE BOOTSTRAP LABELS native-ui reply-drafts
    RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_forum_media_probe UI_FORUM_MEDIA_PROBE BOOTSTRAP LABELS native-ui forum-media
    RESOURCE_LOCK native_gpu_c)
maicord_ui_probe_test(maicord_hangul_render_probe HANGUL_RENDER_PROBE TIMEOUT 15 LABELS native-text-render
    RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_standard_render_probe STANDARD_RENDER_PROBE TIMEOUT 15 LABELS native-text-render
    RESOURCE_LOCK native_gpu)
# Wrapped profile geometry for each script's samples. The same gates write
# frames when MAICORD_UI_PROBE_CAPTURE_DIR is set.
foreach(maicord_render_script IN ITEMS TIBETAN MYANMAR KHMER SINHALA MALAYALAM KANNADA TELUGU ODIA GURMUKHI
        GUJARATI BENGALI TAMIL THAI)
    string(TOLOWER "maicord_${maicord_render_script}_wrap_render_probe" maicord_render_test)
    maicord_ui_probe_test(${maicord_render_test} ${maicord_render_script}_WRAP_RENDER_PROBE TIMEOUT 15
        LABELS native-text-render)
endforeach()
foreach(maicord_render_mode IN ITEMS DEVANAGARI DEVANAGARI_TRACKING DEVANAGARI_DIRECTION DIRECTION_TRACKING
        EXTENDED_STANDARD)
    string(TOLOWER "maicord_${maicord_render_mode}_render_probe" maicord_render_test)
    maicord_ui_probe_test(${maicord_render_test} ${maicord_render_mode}_RENDER_PROBE TIMEOUT 15
        LABELS native-text-render)
endforeach()
# Isolated scene gates of the UI probe: each builds the shared fixture, drives
# one scene through the real layout, text and draw path, and exits. They need
# no GPU lock; every one is a hardware-renderer pixel and geometry check.
# Unified picker also checks cached centering and dense message batch capacity.
# MAICORD_<GATE>_PROBE runs as maicord_<gate>_probe, a leading UI_ moved to the end.
foreach(maicord_ui_gate IN ITEMS ACCOUNT_MENU_UI ANIMATION_HANDOFF_UI BOOTSTRAP_TITLE CATALOG_APNG_UI INBOX_UI
        LONG_EMBED_UI MEDIA_FAVORITES_UI MEDIA_PICKER_UI MENU_ROW_UI NOTIFICATION_SETTINGS_UI
        PROFILE_BRAILLE_UI SCOPED_SEARCH_UI SEARCH_UI STORE_UI TOOLTIP_UI TRANSPARENT_GUILD_UI USER_SETTINGS_UI UI_CHANNEL_OVERVIEW
        UI_GUILD_ROLES
        UI_CAPTION UI_COMPOSER_WORKFLOW UI_DOWNLOAD UI_EDITED_LABEL UI_FORUM_POST UI_FORWARDED_MESSAGES UI_HOVER_RETENTION
        UI_INLINE_FLAGS UI_RESIZE_TEXT
        UI_MEDIA_UNDERLAY UI_MESSAGE_COMPONENTS UI_MESSAGE_FORWARD UI_MESSAGE_HEADER_CLIP UI_PANE_GLYPH_STRESS
        UI_PINNED_MESSAGES UI_QUICK_SWITCHER
        UI_SEARCH_POLISH UI_SURFACE_MOTION UI_THREAD_CREATION UI_UNIFIED_PICKER TOAST_UI)
    string(TOLOWER "${maicord_ui_gate}" maicord_ui_gate_name)
    string(REGEX REPLACE "^ui_(.*)$" "\\1_ui" maicord_ui_gate_name "${maicord_ui_gate_name}")
    maicord_ui_probe_test(maicord_${maicord_ui_gate_name}_probe ${maicord_ui_gate}_PROBE BOOTSTRAP TIMEOUT 15
        LABELS native-ui display)
endforeach()
# The sequence that hung an AMD GPU when the tile pool shrank under queued writes (profile cards opened in the frame
# of a member-list upload burst): a GPU hang reproduction, so it runs on WARP with the debug layer only, never on the
# hardware adapter, and any debug-layer message but NULL_TILE_MAPPING_ACCESS_WARNING fails it.
maicord_ui_probe_test(maicord_profile_media_sequence_ui_probe UI_PROFILE_MEDIA_SEQUENCE_PROBE BOOTSTRAP WARP TIMEOUT 15
    LABELS native-ui display)
# The sign-in screen as the client starts it (first frame under the caption, window shown, the loop's first glyph
# maintenance, an idle trim): every resident glyph reads back with coverage through a staging copy, and every sign-in
# quad names a resident glyph and draws ink. On the hardware adapter and on WARP with the debug layer.
maicord_ui_probe_test(maicord_startup_glyph_atlas_probe STARTUP_GLYPH_ATLAS_PROBE BOOTSTRAP TIMEOUT 15
    LABELS native-ui display RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_startup_glyph_atlas_warp_probe STARTUP_GLYPH_ATLAS_PROBE BOOTSTRAP WARP TIMEOUT 15
    LABELS native-ui display)
# The full app shell (chat, member list) fills the glyph atlas, then the account signs out and the sign-in screen
# replaces it, followed by the loop's maintenance and an idle trim: the sign-in quads stay resident and drawn.
maicord_ui_probe_test(maicord_signout_glyph_atlas_probe SIGNOUT_GLYPH_ATLAS_PROBE BOOTSTRAP TIMEOUT 15
    LABELS native-ui display RESOURCE_LOCK native_gpu)
maicord_ui_probe_test(maicord_signout_glyph_atlas_warp_probe SIGNOUT_GLYPH_ATLAS_PROBE BOOTSTRAP WARP TIMEOUT 15
    LABELS native-ui display)
# Website screenshots (tests/integration/showcase_probe.cpp). Opt-in: build.ps1
# runs label showcase only when the test is named, e.g.
# -TestNames maicord_showcase_ui_probe -ProbeCaptureDirectory <dir>.
# With MAICORD_SHOWCASE_THEME (a theme with variants) and optionally
# MAICORD_SHOWCASE_EFFECT (its .mcfx) it takes only that theme through the day;
# MAICORD_SHOWCASE_TRANSITIONS adds frame strips of a theme glide and a knob change;
# MAICORD_SHOWCASE_FRIENDS takes only the Friends page in Dark, Light, AMOLED and
# Aurora, then by day and night in MAICORD_SHOWCASE_THEME's theme when one is named.
maicord_ui_probe_test(maicord_showcase_ui_probe UI_SHOWCASE_PROBE BOOTSTRAP LABELS showcase display
    RESOURCE_LOCK native_gpu)
# COLR compute bindings: layered, narrow and wide kernels over hand-built
# programs, upload growth, owner changes and failed-submission unbinding.
maicord_ui_probe_test(maicord_colr_binding_probe COLR_BINDING_PROBE TIMEOUT 15 LABELS gpu)
# Live-resize stress in a shown window: 64 WM_SIZE steps down and back, each
# painted once as a clipped update with no swap-chain reallocation.
maicord_ui_probe_test(maicord_resize_stress_probe UI_PROBE_RESIZE_STRESS TIMEOUT 15 LABELS gpu display)
# The bundled regional flags through popup emoji cells at three DPIs and two atlas shapes.
maicord_ui_probe_test(maicord_regional_flag_probe REGIONAL_FLAG_PROBE TIMEOUT 20 LABELS native-ui display)
# Fast CPU decode paths (PNG filters, GIF LZW, JPEG entropy) against bit-serial references.
maicord_test(MaiCordMediaDecodeReferenceTests ENTRY maicord_media_decode_reference_tests_entry
    SOURCES tests/media/image/media_decode_reference.cpp
    BUNDLE MaiCordCodecUnitTests
    TIMEOUT 10 TEST maicord_media_decode_reference)
# Production GPU image stages: all PNG sample layouts against integer references,
# JPEG coefficients against Pillow, and VP8 against the CPU thumbnail decoder.
maicord_test(MaiCordMediaGpuReferenceTests ONLY WINDOWS ENTRY maicord_media_gpu_reference_tests_entry
    SOURCES tests/media/image/media_gpu_reference.cpp
    BUNDLE MaiCordGpuUnitTests TIMEOUT 10 LABELS gpu RESOURCE_LOCK native_gpu TEST maicord_media_gpu_reference)
maicord_test(MaiCordOpusEncoderTests ONLY WINDOWS ENTRY maicord_opus_encoder_tests_entry
    SOURCES tests/media/audio/opus_encoder.cpp
    LIBS maicord_voice_encoder maicord_opus_encoder
    INCLUDES tests third_party/opus/include
    BUNDLE MaiCordCodecUnitTests
    TIMEOUT 10 TEST maicord_opus_encoder)
# Decoder coverage streams (tests/fixtures/opus_decoder_vectors.inc), range- and
# bit-exact; the official test vectors run opt-in (tools/opus_conformance.ps1).
maicord_test(MaiCordOpusDecoderVectorTests ENTRY maicord_opus_decoder_vector_tests_entry
    SOURCES tests/media/audio/opus_decoder_vectors.cpp
    LIBS maicord_opus_decoder
    INCLUDES tests third_party/opus/include
    BUNDLE MaiCordCodecUnitTests
    TIMEOUT 10 TEST maicord_opus_decoder_vectors)
# Voice messages: the Ogg Opus writer, recorder, waveform, upload/message
# bodies, gating, the composer flow and the module with a synthetic microphone.
maicord_test(MaiCordVoiceMessageTests ONLY WINDOWS ENTRY maicord_voice_message_tests_entry
    SOURCES tests/app/voice/voice_message.cpp
    LIBS maicord_app_core maicord_message_runtime maicord_model maicord_protocol maicord_platform
        maicord_voice_encoder maicord_opus_encoder maicord_opus_decoder ole32 user32
    INCLUDES tests
    BUNDLE MaiCordCodecUnitTests
    TIMEOUT 10 TEST maicord_voice_message)
# Discord-sized store limits: member role overflow, member roles in messages
# and owned voice-state storage.
maicord_test(MaiCordStoreLimitsTests ENTRY maicord_store_limits_entry SOURCES tests/model/guild/store_limits.cpp
    BUNDLE MaiCordModelTests TIMEOUT 10 TEST maicord_store_limits)

maicord_test(MaiCordIconAtlasCopyTests ONLY WINDOWS ENTRY maicord_icon_atlas_copy_entry
    SOURCES tests/render/media/icon_atlas_copy.cpp
    BUNDLE MaiCordGpuUnitTests TIMEOUT 5 TEST maicord_icon_atlas_copy)

maicord_test(MaiCordRectangleGeometryTests ENTRY rectangle_geometry_entry SOURCES tests/ui/core/rectangle_geometry.cpp
    BUNDLE MaiCordUiUnitTests TIMEOUT 5 TEST maicord_rectangle_geometry)

maicord_test(MaiCordEmbedProviderTests ENTRY maicord_embed_provider_entry SOURCES tests/protocol/media/embed_provider.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5 TEST maicord_embed_provider)

maicord_test(MaiCordYoutubePlayerTests ENTRY maicord_youtube_player_entry SOURCES tests/protocol/media/youtube_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5 TEST maicord_youtube_player)

maicord_test(MaiCordEmbedResolverTests ONLY WINDOWS ENTRY maicord_youtube_resolver_entry
    # Public SoundCloud queues.
    SOURCES tests/app/media/youtube_resolver.cpp tests/app/media/soundcloud_player.cpp
        tests/app/media/soundcloud_collection.cpp tests/app/media/youtube_collection.cpp tests/app/media/apple_music_album.cpp
        tests/app/media/apple_music_playlist.cpp src/app/update/https.cpp
    LIBS maicord_account_media maicord_app_core maicord_protocol maicord_platform maicord_core
        d3d11 dxgi ole32 user32 kernel32 ws2_32 maicord_sspicli bcrypt
    TIMEOUT 20 TEST maicord_youtube_resolver)

maicord_test(MaiCordTikTokPlayerTests ENTRY maicord_tiktok_player_entry SOURCES tests/protocol/media/tiktok_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5 TEST maicord_tiktok_player)

maicord_test(MaiCordTwitchPlayerTests ENTRY maicord_twitch_player_entry SOURCES tests/protocol/media/twitch_player.cpp
    BUNDLE MaiCordProtocolTests
    TIMEOUT 5 TEST maicord_twitch_player)

maicord_test(MaiCordSpotifyPlayerTests ENTRY maicord_spotify_player_entry SOURCES tests/protocol/media/spotify_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5 TEST maicord_spotify_player)

maicord_test(MaiCordStreamablePlayerTests ENTRY maicord_streamable_player_entry SOURCES tests/protocol/media/streamable_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5 TEST maicord_streamable_player)

maicord_test(MaiCordPornhubPlayerTests ENTRY maicord_pornhub_player_entry SOURCES tests/protocol/media/pornhub_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5 TEST maicord_pornhub_player)

# Vimeo canonical/share routes and native timestamp parsing.
maicord_test(MaiCordVimeoPlayerTests ENTRY maicord_vimeo_player_entry SOURCES tests/protocol/media/vimeo_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5 TEST maicord_vimeo_player)

maicord_test(MaiCordAppleMusicPlayerTests ENTRY maicord_apple_music_player_entry SOURCES tests/protocol/media/apple_music_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5 TEST maicord_apple_music_player)

maicord_test(MaiCordMp4FragmentTests ONLY WINDOWS ENTRY maicord_mp4_fragment_entry
    SOURCES tests/media/containers/mp4_fragment.cpp tests/media/containers/fragment_audio_playback_probe.cpp
    LIBS maicord_account_media maicord_platform ole32 user32 d3d11 dxgi
    BUNDLE MaiCordCodecUnitTests
    TIMEOUT 10 TEST maicord_mp4_fragment)

maicord_test(MaiCordOAuthProtocolTests ENTRY maicord_oauth_protocol_entry SOURCES tests/protocol/auth/oauth_protocol.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5 TEST maicord_oauth_protocol)

maicord_test(MaiCordSpotifyOAuthTests ONLY WINDOWS ENTRY maicord_spotify_oauth_entry
    SOURCES tests/app/account/login/spotify_oauth.cpp
    BUNDLE MaiCordOAuthTests TIMEOUT 5 TEST maicord_spotify_oauth)

maicord_test(MaiCordOAuthAuthorizationTests ONLY WINDOWS ENTRY maicord_oauth_authorization_entry
    SOURCES tests/app/account/login/oauth_authorization.cpp
    BUNDLE MaiCordOAuthTests TIMEOUT 5 TEST maicord_oauth_authorization)

maicord_test(MaiCordOAuthSignInTests ONLY WINDOWS ENTRY maicord_oauth_sign_in_entry
    SOURCES tests/app/account/login/oauth_sign_in.cpp
    BUNDLE MaiCordOAuthTests TIMEOUT 8 TEST maicord_oauth_sign_in)

maicord_test(MaiCordOAuthAccountTests ONLY WINDOWS ENTRY maicord_oauth_account_entry
    SOURCES tests/app/account/login/oauth_account.cpp
    BUNDLE MaiCordOAuthTests TIMEOUT 5 TEST maicord_oauth_account)

maicord_test(MaiCordDashManifestTests ENTRY maicord_dash_manifest_entry SOURCES tests/protocol/media/dash_manifest.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5 TEST maicord_dash_manifest)

maicord_test(MaiCordSteamPlayerTests ENTRY maicord_steam_player_entry SOURCES tests/protocol/media/steam_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5 TEST maicord_steam_player)

maicord_test(MaiCordHlsPlaylistTests ENTRY maicord_hls_playlist_entry SOURCES tests/protocol/media/hls_playlist.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5 TEST maicord_hls_playlist)

maicord_test(MaiCordMpegTsTests ONLY WINDOWS ENTRY maicord_mpeg_ts_entry SOURCES tests/media/containers/mpeg_ts.cpp
    LIBS maicord_account_media maicord_platform ole32 user32 d3d11 dxgi
    BUNDLE MaiCordCodecUnitTests
    TIMEOUT 10 TEST maicord_mpeg_ts)

maicord_test(MaiCordImgurPlayerTests ENTRY maicord_imgur_player_entry SOURCES tests/protocol/media/imgur_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5 TEST maicord_imgur_player)

maicord_test(MaiCordTwitterPlayerTests ENTRY maicord_twitter_player_entry SOURCES tests/protocol/media/twitter_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5 TEST maicord_twitter_player)

maicord_test(MaiCordAmazonMusicPlayerTests ENTRY maicord_amazon_music_player_entry
    SOURCES tests/protocol/media/amazon_music_player.cpp
    BUNDLE MaiCordProtocolTests TIMEOUT 5 TEST maicord_amazon_music_player)

# MaiCordNativeAppLineTests exits 77 (skip) on a machine whose system fonts its committed references never recorded.
get_property(maicord_registered_tests DIRECTORY PROPERTY TESTS)
foreach(maicord_registered_test IN LISTS maicord_registered_tests)
    get_property(maicord_registered_target GLOBAL PROPERTY MAICORD_TEST_TARGET_${maicord_registered_test})
    if(maicord_registered_target STREQUAL "MaiCordNativeAppLineTests")
        set_tests_properties(${maicord_registered_test} PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endforeach()
