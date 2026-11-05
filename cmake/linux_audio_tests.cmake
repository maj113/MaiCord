# The tests of the Linux audio backend (cmake/linux_audio.cmake), registered from linux_test_registry.cmake.
#
#   maicord_audio_pure                 the transport-free media ring and clock conversions shared with the Windows
#                                      suite, and the arithmetic of the PulseAudio transport (buffer attributes
#                                      against the Windows engine timing, the playback timeline, capture pacing and
#                                      the reopen schedule) with an injected clock
#   maicord_notification_audio         the notification mixer, request preparation, lifecycle and bounded worker
#                                      cases the Windows suite runs (tests/platform/audio/native_notification_audio.cpp)
#   maicord_audio_no_server            audio with libpulse but no server: a state, not a crash
#   maicord_audio_no_library           audio without libpulse: the same
#
# The integration test needs a PulseAudio server, so it is opt-in: configure with MAICORD_PULSE_INTEGRATION=1 in the
# environment (tools/test_pulse_audio.sh does, and picks the server: WSLg's, or a local pulseaudio) and it runs;
# otherwise it is registered disabled. It plays into a null sink of its own and records the sink's monitor.
#
#   maicord_pulse_loopback             voice enumeration, voice output, notification sounds at two rates, media with
#                                      per-stream volume, the capture of all of it, endpoint change events
#
# No test links a window backend: window_post_stub.cpp counts the wakes the audio workers post.
maicord_test(MaiCordAudioPureTests ENTRY maicord_audio_pure_entry
    SOURCES tests/platform/audio/audio_pure_linux.cpp tests/platform/audio/media_audio.cpp
        tests/platform/audio/pulse_timing.cpp tests/platform/audio/window_post_stub.cpp
    LIBS maicord_audio_pulse maicord_core
    TIMEOUT 10 TEST maicord_audio_pure)

maicord_test(MaiCordNotificationAudioTests ENTRY maicord_notification_audio_tests_entry
    SOURCES tests/platform/audio/native_notification_audio.cpp tests/platform/audio/window_post_stub.cpp
    LIBS maicord_audio_pulse maicord_core
    TIMEOUT 10 TEST maicord_notification_audio)

maicord_test(MaiCordAudioUnavailableTests ENTRY maicord_audio_unavailable_entry
    SOURCES tests/platform/audio/pulse_unavailable.cpp tests/platform/audio/window_post_stub.cpp
    LIBS maicord_audio_pulse maicord_core
    TIMEOUT 20 ENV PULSE_SERVER=unix:/maicord-no-such-pulse-server
    TEST maicord_audio_no_server --no-server)
maicord_add_tests(MaiCordAudioUnavailableTests TIMEOUT 20 ENV MAICORD_PULSE_LIBRARY=libmaicord-no-such-pulse.so.0
    TEST maicord_audio_no_library --no-library)

# Text-to-speech (src/platform/audio/speechd), the logic against fake libspeechd entry points and the real library's
# unavailable states. The no-daemon case has no daemon to reach and none it could start (an address in a directory that
# does not exist, no PATH for the spawn).
#
#   maicord_speechd                    what is opened and said, the text handed over, the failure and release rules
#   maicord_speechd_no_library         without libspeechd: speech reports unavailable
#   maicord_speechd_no_daemon          libspeechd but no speech-dispatcher: the same (skipped without libspeechd)
#
# The integration test needs speech-dispatcher with its dummy output module, so it is opt-in: configure with
# MAICORD_SPEECHD_INTEGRATION=1 in the environment (tools/test_speechd.sh does, with a private daemon whose log it reads
# afterwards).
#
#   maicord_speechd_live               the real libspeechd says, interrupts and closes against that daemon
maicord_test(MaiCordSpeechdTests ENTRY maicord_speechd_entry
    SOURCES tests/platform/audio/speechd.cpp
    LIBS maicord_audio_pulse maicord_core
    TIMEOUT 20 TEST maicord_speechd)
maicord_add_tests(MaiCordSpeechdTests TIMEOUT 20 ENV MAICORD_SPEECHD_LIBRARY=libmaicord-no-such-speechd.so.2
    TEST maicord_speechd_no_library --no-library)
maicord_add_tests(MaiCordSpeechdTests TIMEOUT 60
    ENV SPEECHD_ADDRESS=unix_socket:/maicord-no-such-directory/speech.sock PATH=/maicord-no-such-path
    TEST maicord_speechd_no_daemon --no-daemon)
maicord_add_tests(MaiCordSpeechdTests TIMEOUT 60 LABELS integration RESOURCE_LOCK speech_dispatcher
    TEST maicord_speechd_live --live)
set_tests_properties(maicord_speechd_no_daemon maicord_speechd_live PROPERTIES SKIP_RETURN_CODE 77)
if(NOT "$ENV{MAICORD_SPEECHD_INTEGRATION}")
    set_tests_properties(maicord_speechd_live PROPERTIES DISABLED TRUE)
endif()

maicord_test(MaiCordPulseLoopbackTests ENTRY maicord_pulse_loopback_entry
    SOURCES tests/platform/audio/pulse_loopback.cpp tests/platform/audio/window_post_stub.cpp
    LIBS maicord_audio_pulse maicord_core
    TIMEOUT 120 LABELS integration TEST maicord_pulse_loopback)
# Status 77: no libpulse, no server, or a server that refuses to load a module. One server at a time.
set_tests_properties(maicord_pulse_loopback PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK pulse_server)
if(NOT "$ENV{MAICORD_PULSE_INTEGRATION}")
    set_tests_properties(maicord_pulse_loopback PROPERTIES DISABLED TRUE)
endif()

# The AAC-LC decoder (src/media/audio/aac_lc.cpp) behind the decoder interface of the Linux build, against fixtures
# from libavcodec's encoder and decoder (tools/generate_aac_fixtures.py): conformance to the reference decoder within
# half a PCM16 step, the MP4 and ADTS paths, pulse data against the standard's IMDCT, the interface's lifecycle rules,
# and corrupted frames.
maicord_test(MaiCordAacLcTests ENTRY maicord_aac_lc_entry
    SOURCES tests/platform/decode/aac_lc.cpp
    LIBS maicord_aac_decoder maicord_media_codec maicord_core
    TIMEOUT 30 TEST maicord_aac_lc)
