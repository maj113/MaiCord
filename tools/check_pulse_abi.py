#!/usr/bin/env python3
"""Checks include/maicord/platform/audio/pulse/abi.hpp against the installed libpulse headers (libpulse-dev): the
analogue of tools/check_openssl_abi.py for the audio backend.

The audio backend never links libpulse: it declares the functions it resolves with dlsym, and the constants and
struct layouts it uses, itself. Nothing at build time proves those declarations match the library, so this script
does:

  * every constant of abi.hpp whose comment names a libpulse macro or enumerator (`// PA_STREAM_READY`) is compiled
    against the real headers in a _Static_assert, so a value that differs fails;
  * the sizeof and offsetof numbers pinned by the static_asserts of abi.hpp are compiled against pa_sample_spec,
    pa_buffer_attr, pa_channel_map, pa_cvolume, pa_sink_info, pa_source_info and pa_server_info;
  * every function of the MAICORD_PULSE_FUNCTIONS table is found in the preprocessed headers, and its argument count
    and the kind (pointer, integer of a size, double, void) of its return type and every argument must be the
    declared one. The opaque structs and the callback typedefs are pointers in both.

    check_pulse_abi.py [--include DIR]... [--compiler CC]

With no --include the system headers (/usr/include) are used; CI installs libpulse-dev and runs this beside the build.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_openssl_abi as shared  # noqa: E402  (its declaration parsing is not OpenSSL specific)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "maicord", "platform", "audio", "pulse", "abi.hpp")
INCLUDES = ("pulse/pulseaudio.h",)

# Scalar names either side uses, beyond the ones check_openssl_abi knows.
shared.SCALARS.update({
    "i32": 4, "i64": 8, "u8": 1, "int64_t": 8, "uint8_t": 1, "double": "double",
    "pa_volume_t": 4, "pa_usec_t": 8,
})

STRUCTS = {
    "SampleSpec": "pa_sample_spec", "BufferAttr": "pa_buffer_attr", "ChannelMap": "pa_channel_map",
    "CVolume": "pa_cvolume", "SinkInfo": "pa_sink_info", "SourceInfo": "pa_source_info",
    "ServerInfo": "pa_server_info",
}


def kind(type_text):
    text = shared.plain(type_text)
    # An opaque struct (pa_context*), a callback typedef (pa_context_notify_cb_t, ContextNotifyCb) or a pointer.
    if "*" in text or re.fullmatch(r"\w+_cb_t|\w+Cb", text):
        return "pointer"
    # The enumerations (pa_context_state_t, pa_stream_flags_t, pa_seek_mode_t, ...) are int-sized.
    if re.fullmatch(r"pa_\w+_t", text) and text not in shared.SCALARS:
        return 4
    return shared.kind(text)


if __name__ == "__main__":
    sys.exit(shared.check_library(__doc__, HEADER, "MAICORD_PULSE_FUNCTIONS", "PA_", INCLUDES, kind, structs=STRUCTS))
