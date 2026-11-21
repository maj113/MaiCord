# Audio on Linux

The audio interfaces in `include/maicord/platform/audio` name no Windows type: borrowed events are `Waitable`, the cross-thread
wake of media and notification audio is a `Waker` posted with `window_post`, and the WASAPI test seam of the notification worker is
`include/maicord/platform/audio/wasapi`. The transport-free halves are shared files (`media_audio_ring.cpp`,
`notification_audio_mixer.cpp`, helpers in `audio_support.hpp`). `media_audio.cpp`, `notification_audio.cpp`, `voice_audio_*.cpp`
and `speech.cpp` in this directory stay the WASAPI and SAPI code; their Linux counterparts are in `pulse/` and `speechd/`. Change
bits of endpoint watching are in `endpoint_watch.hpp`; each backend fills them its own way (COM observers, a
`pa_context_subscribe` subscription).

## PulseAudio (`pulse/`)

- libpulse (`libpulse.so.0`: PulseAudio, pipewire-pulse, WSLg) is opened with `dlopen`; the declarations are
  `include/maicord/platform/audio/pulse/abi.hpp`, checked against libpulse-dev by `python tools/check_pulse_abi.py` (functions,
  constants, struct layouts).
- With no libpulse or no server audio is an unavailable state (empty device lists, a `waiting` capture, a device error in the
  notification status, a device or server error in the media status), never a crash.
- The media worker keeps a frame in its ring until the server has read it, not until it is heard: WSLg's RDP sink reports
  0.4 to 1 s of latency, more than the video session's ring of 8192 frames. Its stream asks for the server's prebuffer, so
  a stream that runs dry waits and drops nothing.
- When the server does not answer in 3 s or refuses the connection (WSLg's server stalls while its RDP sink waits for
  Weston), the media worker tries two more times, 1 s and 2 s later, and shows buffering. A pause, a seek or a new play
  cancels the retries. After the last one the error is `MediaAudioError::server`, which the player shows as "Sound server
  not answering". `tools/test_pulse_stall.sh` runs `maicord_pulse_stall_*` against a private pulseaudio that the test
  stops with SIGSTOP.
- Tests: `maicord_audio_pure` (the transport arithmetic with an injected clock), `maicord_notification_audio` (logic shared with
  Windows), `maicord_audio_no_server` and `maicord_audio_no_library` (`MAICORD_PULSE_LIBRARY` names a library that does not exist;
  a test hook). `tools/test_pulse_audio.sh` runs the opt-in `maicord_pulse_loopback` and `maicord_video_playback_audio` against
  WSLg's PulseServer, `$PULSE_SERVER` or a private `pulseaudio` (`--private`): it loads a null sink, plays voice, notification and
  media audio into it and records the sink's monitor with the voice capture.

## Text to speech (`speechd/`)

"Speak message" and Settings > Accessibility's Text-to-Speech rate Preview are the callers of `speech_speak`; the message "tts" flag is always false and the client reads no notification
aloud, as on Windows. It is speech-dispatcher through `libspeechd.so.2` (`dlopen`; `include/maicord/platform/audio/speechd/abi.hpp`,
checked by `python tools/check_speechd_abi.py`).

- The client name is `maicord` on `spd_open2` (single threaded, the default address, so `$SPEECHD_ADDRESS`, with the daemon's
  autospawn); `spd_cancel` before each `spd_say` at priority text (SAPI's `SPF_PURGEBEFORESPEAK`); `spd_cancel` and `spd_close` at
  release.
- The voice and language are the user's: the daemon's own configuration and its per-client section for `maicord`. So is the rate
  until Settings > Accessibility's Text-to-Speech rate moves it: `spd_set_voice_rate` (its -10..10 step x 10) on the connection when
  it differs from the one set last; a new connection starts from the configured rate again.
- A text goes in as its valid UTF-8 prefix, cut at 4096 bytes and at the first NUL. With no library, no daemon (not retried: each
  try can wait on the spawn) or a refused say the call returns false and the caller logs `speech_unavailable`; a failed say drops the
  connection so the next call opens another.
- Tests: `maicord_speechd` (fake libspeechd entry points), `maicord_speechd_no_library` / `_no_daemon`, and `tools/test_speechd.sh`
  (`maicord_speechd_live`: a private `speech-dispatcher` with its dummy module; its log shows both texts in order, the second after a
  cancel, the invalid one never, the client removed at release).

## Opus from WebM (`src/media/containers/webm_opus.cpp`), on both systems

YouTube's Opus formats (itags 249 to 251) are WebM files. `webm_opus.cpp` reads one Opus track in place as a grid of packets
of one length, from a sparse source, and `mp4_fragment_audio.cpp` gives it to the session through the calls the AAC formats use.
The session's audio pump decodes the packets with MaiCord's Opus decoder into float (`opus_decode` with `wide`), so a peak above
full scale reaches the transport. The fetch (`progressive_video_fetch.cpp`) starts again with the AAC format when the reader
refuses a file. `tests/fixtures/webm` holds a file from ffmpeg with libopus and ffprobe's packet list.
`maicord_webm_opus` reads the fixture, a copy without Cues, a sparse copy and damaged copies. `maicord_progressive_video`
(Windows) covers the fetch: seek, URL refresh and the fallback. `maicord_video_playback_audio` (opt-in, see above) plays the
fixture through the session into a sound server.

## AAC (`src/media/audio`, `src/platform/decode/linux/aac_decoder.cpp`)

AAC is behind `include/maicord/platform/decode/aac_decoder.hpp`: Media Foundation on Windows (unchanged), on Linux the in-house
AAC-LC decoder `src/media/audio/aac_lc.cpp` (no libm, no allocation: the caller gives it memory) with the adapter in
`platform/decode/linux/aac_decoder.cpp`. Mono and stereo, section and scalefactor data, Huffman codebooks, pulses, M/S, intensity
stereo, PNS, TNS, sine and KBD windows, long and short blocks. HE-AAC (SBR, PS) is refused at `aac_decoder_initialize`: every
streaming source the client resolves (DASH, HLS, embeds) is `mp4a.40.2`, and only a user-uploaded MP4 can carry HE-AAC.
`src/media/audio/aac_tables.inc` is generated by `tools/generate_aac_tables.py` from the system libavcodec (not run by the
build), and `tests/fixtures/aac` holds libavcodec's encodes and decodes. `maicord_aac_lc` compares the decoder with
libavcodec within half a PCM16 step, feeds the MP4 and ADTS paths, builds a pulse frame by hand and corrupts frames.
