#include "maicord/app/gateway/go_live_stream.hpp"

// The Go Live producers of the tests that link the Gateway client but stream nothing (go_live_stream.hpp): no device,
// no capture, no encoder.
namespace maicord {

u32 go_live_producer_encodable(void*, bool* known) noexcept {
    *known = false;
    return 0;
}
bool go_live_producer_capture(void*, GoLiveMedia*, const CaptureSource&, bool, u32) noexcept {
    return false;
}
void go_live_producer_capture_stop(void*) noexcept {}
bool go_live_producer_sound(void*, GoLiveMedia*, u32) noexcept {
    return false;
}
void go_live_producer_sound_stop(void*) noexcept {}
bool go_live_producer_encoder(void*, GoLiveMedia*, GpuStreamCodec, const GpuStreamEncoderSettings&) noexcept {
    return false;
}
void go_live_producer_configure(void*, const GpuStreamEncoderSettings&) noexcept {}
void go_live_producer_bitrate(void*, u32) noexcept {}
void go_live_producer_keyframe(void*) noexcept {}
void go_live_producer_encoder_stop(void*) noexcept {}
bool go_live_producer_preview(void*, GpuVideoFrame*, u32*) noexcept {
    return false;
}

} // namespace maicord
