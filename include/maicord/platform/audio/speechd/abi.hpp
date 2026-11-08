#pragma once

#include "maicord/core/base.hpp"

// The libspeechd interface MaiCord uses, declared without speech-dispatcher's headers (Linux). The library is dlopen()ed
// ("libspeechd.so.2"), so a system without speech-dispatcher reports text-to-speech as unavailable instead of failing to
// load. tools/check_speechd_abi.py compares this file with the installed libspeechd-dev headers (constants marked with
// the macro they copy, the function table). Only functions that exist in libspeechd 0.11.1 (Ubuntu 22.04, the oldest of
// the supported distributions; Alpine has speech-dispatcher from 3.20) are used.

namespace maicord::speechd {

struct Connection; // SPDConnection
struct Address;    // SPDConnectionAddress: null asks for the default one ($SPEECHD_ADDRESS, else the user's socket)

inline constexpr i32 kModeSingle = 0; // SPD_MODE_SINGLE: no thread for the server's events, which are not used
inline constexpr i32 kPriorityText = 3; // SPD_TEXT: text being read; the next message of the client does not wait for it

// libspeechd's functions. spd_open2 starts the daemon when it is not running (autospawn) and, on failure, returns null
// with a malloc()ed message in *error_result. spd_say returns the message id, -1 on error; spd_cancel stops the client's
// current message and drops its queue, 0 on success.
#define MAICORD_SPEECHD_FUNCTIONS(F)                                                                                   \
    F(Connection*, spd_open2,                                                                                          \
      (const char* client_name, const char* connection_name, const char* user_name, i32 mode, const Address* address,   \
       i32 autospawn, char** error_result))                                                                            \
    F(void, spd_close, (Connection * connection))                                                                      \
    F(i32, spd_say, (Connection * connection, i32 priority, const char* text))                                         \
    F(i32, spd_cancel, (Connection * connection))

struct Api {
#define MAICORD_SPEECHD_MEMBER(result, name, arguments) result(*name) arguments;
    MAICORD_SPEECHD_FUNCTIONS(MAICORD_SPEECHD_MEMBER)
#undef MAICORD_SPEECHD_MEMBER
};

} // namespace maicord::speechd
