# Opt-in network probes against the live service endpoints (TLS, REST, CDN) and the
# Opus decoder conformance run. tools/probe_network.ps1 measures the probes and
# tools/opus_conformance.ps1 runs the conformance vectors. They never run in the
# test suite.
option(MAICORD_BUILD_INTEGRATION_TESTS "Build opt-in network probes and the Opus conformance run" OFF)
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
maicord_program(MaiCordTlsResumeProbe ENTRY maicord_tls_resume_probe_entry
    SOURCES tests/integration/tls_resume_probe.cpp
    LIBS maicord_core maicord_platform kernel32 ${maicord_tls_libs})
maicord_program(MaiCordCdnClientProbe ENTRY maicord_cdn_client_probe_entry
    SOURCES tests/integration/cdn_client_probe.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_media_codec kernel32 ${maicord_tls_libs})

# The official RFC 6716/8251 test vectors through the shipping decoder. It compiles
# against the SDK (MAICORD_SDK_INCLUDES) for bcrypt's SHA-256.
maicord_program(MaiCordOpusConformance ENTRY maicord_opus_conformance_entry
    SOURCES tests/integration/opus_conformance.cpp
    LIBS maicord_core maicord_media_codec maicord_opus_decoder kernel32 bcrypt
    INCLUDES tests third_party/opus/include)
set_target_properties(MaiCordOpusConformance PROPERTIES MAICORD_SDK_INCLUDES ON)
