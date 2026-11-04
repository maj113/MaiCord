# The tests the Linux build registers: the Linux-buildable subset of cmake/test_registry.cmake, under the same test
# names (maicord_core, maicord_byte_order), plus the Linux runtime's own. Sources are the shared tests/ files; a
# platform difference inside one is an #if in the test. Not ported: tests/core/process_accessors.cpp,
# client_startup_imports.cpp and delay_load_helper.cpp, which test Windows interfaces (the TEB accessors, the PE
# import table and the delay-load helper).
maicord_test_bundle(MaiCordCoreUnitTests LIBS maicord_core)

# The core tests of tests/core_test_cases.inc that need nothing outside core and the portable libraries, run in one
# process in that order (tests/support/test_main_linux.cpp).
maicord_test(MaiCordTests ENTRY maicord_tests_entry
    SOURCES tests/support/test_main_linux.cpp tests/core/core_runtime.cpp tests/core/json_scan.cpp
        tests/core/clock.cpp tests/core/json_reader_visit.cpp tests/core/json_string_decode.cpp
        tests/core/mailbox_release_race.cpp tests/core/mailbox_acquire_release.cpp
        tests/media/compression/deflate_matches.cpp tests/media/image/png_crc.cpp
        tests/media/image/image_decode_corpus.cpp tests/protocol/gateway/message_rich_index.cpp
        tests/protocol/messages/command_argument_editing.cpp tests/protocol/media/attachment_upload.cpp
        tests/protocol/user/profile_decode_scratch.cpp tests/media/compression/deflate_differential.cpp
        tests/media/compression/deflate_reference.cpp tests/media/image/core_codecs.cpp tests/support/media_fixture.cpp
        tests/support/media_fixture_encoders.cpp
    LIBS maicord_core maicord_compression maicord_protocol maicord_media_core maicord_media_codec maicord_model
    TEST maicord_core)

maicord_test(MaiCordByteOrderTests ENTRY maicord_byte_order_entry SOURCES tests/core/byte_order_test.cpp
    BUNDLE MaiCordCoreUnitTests TEST maicord_byte_order)
maicord_test(MaiCordGpuTypesTests ENTRY maicord_gpu_types_entry SOURCES tests/gpu/gpu_types.cpp
    BUNDLE MaiCordCoreUnitTests TEST maicord_gpu_types)
maicord_test(MaiCordWindowVocabularyTests ENTRY maicord_window_vocabulary_entry
    SOURCES tests/platform/window/window_vocabulary.cpp BUNDLE MaiCordCoreUnitTests TEST maicord_window_vocabulary)
maicord_test(MaiCordOsWaitTests ENTRY maicord_os_wait_entry SOURCES tests/os/wait.cpp
    BUNDLE MaiCordCoreUnitTests TIMEOUT 10 TEST maicord_os_wait)
maicord_test(MaiCordOsThreadTests ENTRY maicord_os_thread_entry SOURCES tests/os/thread.cpp
    BUNDLE MaiCordCoreUnitTests TIMEOUT 10 TEST maicord_os_thread)
maicord_test(MaiCordOsFileTests ENTRY maicord_os_file_entry SOURCES tests/os/file.cpp
    LIBS maicord_core TIMEOUT 10 TEST maicord_os_file)
maicord_test(MaiCordOsSrwLockTests ENTRY maicord_os_srwlock_entry SOURCES tests/os/srwlock.cpp
    BUNDLE MaiCordCoreUnitTests TIMEOUT 15 TEST maicord_os_srwlock)

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

# The network layer against sockets of this machine (the same test as Windows): streams, datagrams, lookups,
# signals from other threads, file operations and the worker.
maicord_test(MaiCordNetworkLoopTests ENTRY maicord_network_loop_entry SOURCES tests/platform/net/network_loop.cpp
    LIBS maicord_net maicord_crypto maicord_core
    TIMEOUT 20 TEST maicord_network_loop)

# tls_stream over the whole stack (lookup, epoll loop, sockets, OpenSSL) against the same kind of local s_server
# processes, started by tests/platform/net/tls_stream_smoke.sh (skipped, status 77, without the command).
maicord_test(MaiCordTlsStreamTests ENTRY maicord_tls_stream_entry SOURCES tests/platform/net/tls_stream_linux.cpp
    LIBS maicord_net maicord_crypto maicord_core)
add_test(NAME maicord_tls_stream_smoke
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/platform/net/tls_stream_smoke.sh" "$<TARGET_FILE:MaiCordTlsStreamTests>"
        "${CMAKE_BINARY_DIR}/tls-stream-smoke")
set_tests_properties(maicord_tls_stream_smoke PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)

include(cmake/linux_test_registry_libraries.cmake)
include(cmake/linux_gpu_tests.cmake)
include(cmake/linux_render_tests.cmake)
include(cmake/linux_lottie_tests.cmake)
include(cmake/linux_window_tests.cmake)
include(cmake/linux_font_tests.cmake)
include(cmake/linux_audio_tests.cmake)
include(cmake/linux_dbus_tests.cmake)
include(cmake/linux_time_tests.cmake)
include(cmake/linux_spell_tests.cmake)
include(cmake/linux_app_tests.cmake)
include(cmake/linux_video_tests.cmake)
