# The tests of the Linux live video decoders (cmake/linux_video.cmake), registered from linux_test_registry.cmake.
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
# asks of the VA-API backend) and how a VA-API picture is submitted (a vaapi::Session over a recording libva).
maicord_test(MaiCordVideoHardwareTierTests ENTRY maicord_video_hardware_tier_entry
    SOURCES tests/platform/decode/video_hardware_tier.cpp
    LIBS maicord_video_decode maicord_video_vaapi maicord_video_software maicord_gpu_gles maicord_media_codec maicord_core
    INCLUDES src
    TIMEOUT 20 ENV MAICORD_VAAPI=off TEST maicord_video_hardware_tier)
