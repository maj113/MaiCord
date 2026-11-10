#pragma once

#include "maicord/app/voice/log.hpp"

namespace maicord {

// The voice log of the tests that read it: they link voice_log_sink.cpp in place of src/app/voice/log_record.cpp, and
// every record goes to the sink a test sets here (null: dropped).
using VoiceLogSink = void (*)(VoiceLogLevel level, const char* event, ConstSlice<VoiceLogField> fields) noexcept;
inline VoiceLogSink volatile g_voice_log_sink;

} // namespace maicord
