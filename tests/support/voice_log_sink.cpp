#include "voice_log_sink.hpp"

namespace maicord {

void voice_log_fields(VoiceLogLevel level, const char* event, ConstSlice<VoiceLogField> fields) noexcept {
    const VoiceLogSink sink = g_voice_log_sink;
    if (sink && event)
        sink(level, event, fields);
}

} // namespace maicord
