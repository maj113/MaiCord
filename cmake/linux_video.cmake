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
    src/platform/decode/vaapi/va_av1.cpp src/platform/decode/vaapi/va_stream.cpp src/platform/decode/vaapi/va_file.cpp
    src/platform/decode/av1_dxva_parameters.cpp src/media/video/h264_slice_detail.cpp
    src/media/video/hevc_slice_detail.cpp
    LIBS maicord_gpu_gles maicord_media_codec maicord_core INCLUDES src)

# The live stream decoder (the Linux gpu_stream_decoder.hpp) over both tiers, and the movie decoder of attachments
# (mp4_decoder.hpp: the Windows build's mp4_decoder.cpp over h264_file_decoder.cpp and hevc_file_decoder.cpp, the Linux
# h264_gpu_decoder.hpp and hevc_gpu_decoder.hpp). h264_gpu_configuration.cpp is the Windows decoder's container-neutral
# first-picture check and fragment seek.
maicord_library(maicord_video_decode
    src/platform/decode/linux/gpu_stream_decoder.cpp src/platform/decode/linux/software_stream.cpp
    src/platform/decode/mp4_decoder.cpp src/platform/decode/linux/h264_file_decoder.cpp
    src/platform/decode/linux/hevc_file_decoder.cpp
    src/platform/decode/h264_gpu_configuration.cpp
    LIBS maicord_video_vaapi maicord_video_software maicord_gpu_gles maicord_media_codec maicord_core INCLUDES src)
maicord_library(maicord_gpu_gles_video src/gpu/gles/video_frame.cpp
    LIBS maicord_gpu_gles_shaders maicord_gpu_gles maicord_core INCLUDES src)
set_target_properties(maicord_gpu_gles_video PROPERTIES EXCLUDE_FROM_ALL TRUE)
