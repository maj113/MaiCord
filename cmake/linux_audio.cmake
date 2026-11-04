# The audio backend of the Linux build (src/platform/audio/pulse), behind include/maicord/platform/audio. It never
# enters the Windows build: that one compiles the WASAPI sources next to them (src/platform/audio/*.cpp).
#
# libpulse is dlopen()ed ("libpulse.so.0": PulseAudio, pipewire-pulse and WSLg), so the image gains no DT_NEEDED and a
# machine without a sound server library or without a running server reports audio as unavailable. media_audio_ring.cpp
# and notification_audio_mixer.cpp are the transport-free halves of media and notification audio the Windows build
# compiles too; text-to-speech has no Linux engine yet (speech_unavailable.cpp: speech_speak reports false).
maicord_library(maicord_audio_pulse
    src/platform/audio/pulse/pulse.cpp src/platform/audio/pulse/voice_audio_output.cpp
    src/platform/audio/pulse/voice_audio_capture.cpp src/platform/audio/media_audio_ring.cpp
    src/platform/audio/pulse/media_audio.cpp src/platform/audio/notification_audio_mixer.cpp
    src/platform/audio/pulse/notification_audio.cpp src/platform/audio/speech_unavailable.cpp
    LIBS maicord_core)

# The AAC decoder interface of include/maicord/platform/decode/aac_decoder.hpp on Linux (Windows decodes AAC through
# Media Foundation, src/platform/decode/aac_decoder.cpp): an in-house AAC-LC core in src/media/audio/aac_lc.cpp with
# the Huffman codebooks and band tables of aac_tables.inc (tools/generate_aac_tables.py), and the adapter that
# answers submit/read/drain/flush with it.
maicord_library(maicord_aac_decoder
    src/media/audio/aac_lc.cpp src/platform/decode/linux/aac_decoder.cpp
    LIBS maicord_core)
