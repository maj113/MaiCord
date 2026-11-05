# The live video decoders of the Linux build (src/platform/decode), behind include/maicord/platform/decode/
# gpu_stream_decoder.hpp. Windows decodes in hardware through DXVA (src/platform/decode/*_gpu_*.cpp) and never
# compiles these.
#
# maicord_video_software  libopenh264 (H.264) and libdav1d (AV1), dlopen()ed: access units to CPU pictures
maicord_library(maicord_video_software
    src/platform/decode/software/software_decoder.cpp
    LIBS maicord_media_codec maicord_core INCLUDES src)

# The hardware tier: VA-API through libva.so.2 and libva-drm.so.2 (dlopen), the VA parameter builders over the in-house
# parsers (and the slice-header parsers VA-API needs that DXVA's short slices do not), and the codec drivers.
# av1_dxva_parameters.cpp is the DXVA builder of the Windows build, compiled here for av1_warp_valid, which the VA
# builder shares, and so that the tests can compare both builders' output.
maicord_library(maicord_video_vaapi
    src/platform/decode/vaapi/va.cpp src/platform/decode/vaapi/va_h264.cpp src/platform/decode/vaapi/va_hevc.cpp
    src/platform/decode/vaapi/va_av1.cpp src/platform/decode/vaapi/va_stream.cpp
    src/platform/decode/av1_dxva_parameters.cpp src/media/video/h264_slice_detail.cpp
    src/media/video/hevc_slice_detail.cpp
    LIBS maicord_gpu_gles maicord_media_codec maicord_core INCLUDES src)

# The live stream decoder (the Linux gpu_stream_decoder.hpp) over the software tier, and the GLES conversion of the
# pictures it publishes (the planes of maicord/gpu/video_planes.hpp). mp4_decoder.cpp is the attachment decoder's
# interface answering "unavailable": no MP4 file decoder exists on Linux yet.
maicord_library(maicord_video_decode
    src/platform/decode/linux/gpu_stream_decoder.cpp src/platform/decode/linux/software_stream.cpp
    src/platform/decode/linux/mp4_decoder.cpp
    LIBS maicord_video_vaapi maicord_video_software maicord_gpu_gles maicord_media_codec maicord_core INCLUDES src)
maicord_library(maicord_gpu_gles_video src/gpu/gles/video_frame.cpp
    LIBS maicord_gpu_gles_shaders maicord_gpu_gles maicord_core INCLUDES src)
set_target_properties(maicord_gpu_gles_video PROPERTIES EXCLUDE_FROM_ALL TRUE)
