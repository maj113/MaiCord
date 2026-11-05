# Tests of the application libraries on Linux (cmake/linux_app_libraries.cmake): the updater's signed-manifest,
# artifact, acceptance, preparation and package pipeline, which the Windows suite runs inside its native-extension
# tests.

maicord_test(MaiCordUpdatePipelineTests ENTRY maicord_update_pipeline_entry
    SOURCES tests/app/update/linux_main.cpp tests/app/update/manifest.cpp tests/app/update/artifact.cpp
        tests/app/update/acceptance.cpp tests/app/update/prepare.cpp tests/app/update/package.cpp
        tests/app/update/portable.cpp
    LIBS maicord_app_update maicord_crypto maicord_protocol maicord_core INCLUDES src/app tests
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

# The Gateway client's self-status and typed-settings fixture: client.cpp built into the test with its test hook, over
# the rest of the client from maicord_app_net (the executable's own client.cpp wins over the library's).
maicord_test(MaiCordGatewaySelfStatusRuntimeTests ENTRY maicord_gateway_self_status_runtime_entry
    SOURCES tests/app/gateway/self_status_runtime.cpp src/app/gateway/client.cpp
    LIBS maicord_app_net maicord_net maicord_crypto maicord_desktop_system maicord_protocol maicord_compression
        maicord_media_codec maicord_model maicord_core
    INCLUDES src/app tests
    DEFINES MAICORD_GATEWAY_SELF_STATUS_TEST=1
    TIMEOUT 10 TEST maicord_gateway_self_status_runtime)
