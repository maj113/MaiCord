#include "maicord/app/voice/media_setup.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/protocol/voice/video_signaling.hpp"

namespace maicord {
void voice_media_clear(VoiceMediaSetup* setup) noexcept {
    voice_packet_release(&setup->packets);
    reset_value(setup);
}
VoiceMediaSetupError voice_media_ready(VoiceMediaSetup* setup, const VoiceReady& ready) noexcept {
    if (!ready.ssrc || !ready.port)
        return VoiceMediaSetupError::decode;
    char ip[64]{};
    const auto decoded = json_string_decode_utf8(ready.ip, {ip, sizeof(ip) - 1});
    if (!decoded.ok() || !decoded.bytes)
        return VoiceMediaSetupError::endpoint;
    for (u32 i = 0; i < decoded.bytes; ++i)
        if (!ip[i])
            return VoiceMediaSetupError::endpoint;
    NetworkAddress endpoint{};
    if (!network_address_parse(ip, ready.port, &endpoint))
        return VoiceMediaSetupError::endpoint;
    voice_media_clear(setup);
    setup->endpoint = endpoint;
    setup->ssrc = ready.ssrc;
    setup->video_ssrc = ready.video_ssrc;
    setup->video_rtx_ssrc = ready.video_rtx_ssrc;
    setup->offered_modes = ready.recognized_modes;
    setup->has_ready = true;
    return VoiceMediaSetupError::none;
}
bool voice_media_protocol_selected(VoiceMediaSetup* setup, u32 mode) noexcept {
    if (!setup->has_ready || setup->has_description || setup->selected_mode ||
        (mode != voice_transport_aes256_gcm_rtpsize &&
         mode != voice_transport_xchacha20_poly1305_rtpsize) ||
        !(setup->offered_modes & mode))
        return false;
    setup->selected_mode = mode;
    return true;
}
VoiceMediaSetupError voice_media_description(VoiceMediaSetup* setup, JsonValue data, Slice<u32> scratch,
                                             u32 maximum_depth, u16 advertised_dave_version,
                                             u64 ready_generation) noexcept {
    if (!setup->has_ready || !setup->selected_mode || setup->has_description ||
        setup->packets.generation || !ready_generation)
        return VoiceMediaSetupError::order;
    SecureBuffer key{};
    if (!secure_buffer_initialize(&key, 32).ok())
        return VoiceMediaSetupError::storage;
    VoiceSessionDescription description{};
    VoiceMediaSetupError error = VoiceMediaSetupError::none;
    if (!voice_decode_session_description(data, scratch, maximum_depth, &description, {key.data, 32}).ok())
        error = VoiceMediaSetupError::decode;
    else if (!description.udp || !description.recognized_mode ||
             description.recognized_mode != setup->selected_mode)
        error = VoiceMediaSetupError::mode;
    else if (description.dave_protocol_version > advertised_dave_version)
        error = VoiceMediaSetupError::dave_version;
    if (error == VoiceMediaSetupError::none) {
        const auto installed = voice_packet_initialize(&setup->packets, ready_generation,
                                                       setup->selected_mode, {key.data, 32});
        if (installed.ok()) {
            setup->dave_protocol_version = description.dave_protocol_version;
            setup->video_codec =
                description.has_video_codec ? voice_video_codec(description.video_codec) : VoiceVideoCodec::none;
            setup->keyframe_interval_ms = description.has_keyframe_interval ? description.keyframe_interval_ms : 0;
            setup->has_description = true;
        } else
            error = installed.error == AeadError::storage ? VoiceMediaSetupError::storage
                                                          : VoiceMediaSetupError::crypto;
    }
    secure_buffer_release(&key);
    return error;
}
bool voice_media_session_update(VoiceMediaSetup* setup, const VoiceSessionUpdate& update) noexcept {
    if (!setup->has_description)
        return false;
    if (update.has_video_codec)
        setup->video_codec = update.video;
    if (update.has_keyframe_interval)
        setup->keyframe_interval_ms = update.keyframe_interval_ms;
    return true;
}
} // namespace maicord
