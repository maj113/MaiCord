# Tests of the application libraries on Linux (cmake/linux_app_libraries.cmake): the updater's signed-manifest,
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

# The Gateway client's self-status and typed-settings fixture: client.cpp built into the test with its test hook, over
# the rest of the client from maicord_app_net (the executable's own client.cpp wins over the library's).
maicord_test(MaiCordGatewaySelfStatusRuntimeTests ENTRY maicord_gateway_self_status_runtime_entry
    SOURCES tests/app/gateway/self_status_runtime.cpp src/app/gateway/client.cpp
    LIBS maicord_app_net maicord_net maicord_crypto maicord_desktop_system maicord_protocol maicord_compression
        maicord_media_codec maicord_model maicord_core
    INCLUDES src/app tests
    DEFINES MAICORD_GATEWAY_SELF_STATUS_TEST=1
    TIMEOUT 10 TEST maicord_gateway_self_status_runtime)

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
