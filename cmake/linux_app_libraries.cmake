# The application libraries the Linux build compiles: the HTTP/REST and CDN transport, the Gateway client and the
# updater, over the neutral network, file and TLS layers (cmake/libraries.cmake lists the Windows ones).

maicord_library(maicord_app_net
    src/app/net/file_persistence.cpp src/app/net/rest_client.cpp src/app/net/rest_client_transaction.cpp
    src/app/net/rest_client_worker.cpp src/app/net/cdn_client.cpp src/app/net/https_file_upload.cpp
    src/app/net/https_file_download.cpp
    src/app/gateway/client.cpp src/app/gateway/client_storage.cpp src/app/gateway/client_transport.cpp
    src/app/gateway/client_outputs.cpp src/app/gateway/client_session.cpp src/platform/desktop/staged_file.cpp
    src/app/account/account_logout.cpp src/app/account/client_session_metadata.cpp
    src/app/account/user_client_configuration.cpp src/app/account/linux/client_identity.cpp
    LIBS maicord_core maicord_protocol maicord_net maicord_crypto maicord_desktop_system maicord_compression
        maicord_media_codec INCLUDES src/app)

maicord_library(maicord_app_update
    src/app/update/manifest.cpp src/app/update/artifact.cpp src/app/update/acceptance.cpp
    src/app/update/prepare.cpp src/app/update/package.cpp src/app/update/fetch.cpp src/app/update/https.cpp
    src/app/update/service.cpp src/app/update/changelog.cpp src/app/account/login/acceptance_record.cpp
    src/app/update/linux/portable.cpp
    LIBS maicord_app_net maicord_core maicord_protocol maicord_net INCLUDES src/app)

# Effect and theme packages (src/app/packages): the link parser, the container reader and the installer; the session
# owner packages.cpp talks to the shell and stays Windows-only.
maicord_library(maicord_app_packages
    src/app/packages/link.cpp src/app/packages/mcfx.cpp src/app/packages/install.cpp
    LIBS maicord_app_net maicord_crypto maicord_core INCLUDES src/app)
