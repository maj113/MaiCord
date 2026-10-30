# Opt-in network probes against the live service endpoints (TLS, REST, Gateway,
# CDN, voice UDP). tools/probe_network.ps1 measures the TLS, REST and CDN ones;
# the others run by hand. They never run in the test suite.
option(MAICORD_BUILD_INTEGRATION_TESTS "Build opt-in network integration probes" OFF)
if(NOT MAICORD_BUILD_INTEGRATION_TESTS)
    return()
endif()

maicord_program(MaiCordTlsProbe ENTRY maicord_tls_probe_entry
    SOURCES tests/integration/tls_probe.cpp tests/integration/probe_transport.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32 ${maicord_tls_libs})
maicord_program(MaiCordRestProbe ENTRY maicord_rest_probe_entry
    SOURCES tests/integration/rest_probe.cpp tests/integration/probe_transport.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32 ${maicord_tls_libs})
maicord_program(MaiCordRestClientProbe ENTRY maicord_rest_client_probe_entry
    SOURCES tests/integration/rest_client_probe.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core kernel32 ${maicord_tls_libs})
maicord_program(MaiCordTlsStreamProbe ENTRY maicord_tls_stream_probe_entry
    SOURCES tests/integration/tls_stream_probe.cpp
    LIBS maicord_core maicord_platform maicord_protocol kernel32 ${maicord_tls_libs})
maicord_program(MaiCordVoiceSocketProbe ENTRY maicord_voice_socket_probe_entry
    SOURCES tests/integration/voice_socket_probe.cpp
    LIBS maicord_app_core maicord_core maicord_platform maicord_protocol kernel32 ${maicord_tls_libs})
maicord_program(MaiCordCdnClientProbe ENTRY maicord_cdn_client_probe_entry
    SOURCES tests/integration/cdn_client_probe.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_media_codec kernel32 ${maicord_tls_libs})
maicord_program(MaiCordLottieCdnProbe ENTRY maicord_lottie_cdn_probe_entry
    SOURCES tests/integration/lottie_cdn_probe.cpp
    LIBS maicord_core maicord_platform maicord_app_core maicord_media_core kernel32 ${maicord_tls_libs})
# The Windows resource probe stays an optimized native object outside LTCG.
set_target_properties(MaiCordLottieCdnProbe PROPERTIES MAICORD_NO_WHOLE_PROGRAM ON)
