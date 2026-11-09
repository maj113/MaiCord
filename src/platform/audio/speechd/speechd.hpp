#pragma once

#include "maicord/platform/audio/speech.hpp"
#include "maicord/platform/audio/speechd/abi.hpp"

// Text-to-speech through speech-dispatcher (src/platform/audio/speechd/speech.cpp implements speech.hpp with these).
// The logic takes the library's entry points as a table, so tests drive it with their own and no daemon.
namespace maicord::speechd {

// The longest text handed to the daemon, in bytes (cut at a character boundary): the callers' buffers are smaller.
inline constexpr u32 kTextMaximumBytes = 4096;

// Resolves libspeechd.so.2 (MAICORD_SPEECHD_LIBRARY names another one) once; null when it is not installed or lacks a
// function. The answer is logged once.
const Api* library() noexcept;

// Speaks `text`: opens the connection on the first call (the client "maicord"; speech-dispatcher starts when it is not
// running), cancels what the client was saying and queues the new text. False for no text, an unusable text (not UTF-8
// from its first byte, or empty up to a NUL), no library (`load` gives null) or no daemon. A failed open is not retried
// by later calls; a failed say drops the connection so that the next call opens another. `load` is called when there is
// something to say. `rate` (speech.hpp's -10..10) is set on the connection when it differs from the one applied last.
bool speak(Speech* speech, const Api* (*load)() noexcept, Utf8View text, i32 rate = 0) noexcept;

// Stops what the client says and closes the connection; the speech is closed (zeroed) afterwards.
void release(Speech* speech, const Api* api) noexcept;

} // namespace maicord::speechd
