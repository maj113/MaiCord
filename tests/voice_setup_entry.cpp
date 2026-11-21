#include "maicord/core/base.hpp"
#include "support/test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

// One list serves the prototypes and the calls, in execution order.
#define MAICORD_VOICE_TESTS(X) \
    X(test_aes_gcm) \
    X(test_aes_gcm64) \
    X(test_xchacha20_poly1305) \
    X(test_kdf_sha256) \
    X(test_secure_random) \
    X(test_hash_stream) \
    X(test_voice_packet_crypto) \
    X(test_voice_protocol_selection) \
    X(test_voice_udp_media) \
    X(test_voice_participants) \
    X(test_voice_media_dispatch) \
    X(test_dave_replay) \
    X(test_dave_frame) \
    X(test_dave_frame_crypto) \
    X(test_dave_key_derivation) \
    X(test_mls_exporter) \
    X(test_mls_key_schedule) \
    X(test_p256) \
    X(test_mls_signature) \
    X(test_hpke_derivation) \
    X(test_mls_hpke) \
    X(test_mls_objects) \
    X(test_mls_join_messages) \
    X(test_dave_key_package) \
    X(test_dave_welcome) \
    X(test_dave_package_policy) \
    X(test_dave_receive_epoch) \
    X(test_dave_receive_sender) \
    X(test_dave_session) \
    X(test_gateway_voice) \
    X(test_voice_connection_setup) \
    X(test_gateway_voice_control) \
    X(test_account_session_identity) \
    X(test_voice_gateway) \
    X(test_voice_heartbeat) \
    X(test_voice_session) \
    X(test_tls_stream_lifecycle) \
    X(test_voice_socket) \
    X(test_voice_connection) \
    X(test_voice_media_events) \
    X(test_voice_udp) \
    X(test_voice_udp_handoff) \
    X(test_voice_state_store) \
    X(test_voice_channel_rows) \
    X(test_voice_channel_info) \
    X(test_voice_gateway_policy) \
    X(test_voice_jitter_buffer) \
    X(test_voice_playback) \
    X(test_voice_receive) \
    X(test_video_rtp_receive) \
    X(test_voice_video_signaling) \
    X(test_dave_video_frames) \
    X(test_voice_video_sink) \
    X(test_gateway_stream) \
    X(test_voice_send) \
    X(test_voice_preferences) \
    X(test_call_view) \
    X(test_stream_watch) \
    X(test_stage_channel) \
    X(test_go_live_scene) \
    X(test_go_live_send) \
    X(test_go_live_thumbnails)

namespace maicord {
#define MAICORD_DECLARE_VOICE_TEST(name) void name(void (*check)(bool, const char*) noexcept) noexcept;
MAICORD_VOICE_TESTS(MAICORD_DECLARE_VOICE_TEST)
#undef MAICORD_DECLARE_VOICE_TEST
namespace {
u32 failures;
void check(bool value, const char* message) noexcept {
    test_check(&failures, value, message);
}
} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_voice_tests_entry() noexcept {
#define MAICORD_RUN_VOICE_TEST(name) maicord::name(maicord::check);
    MAICORD_VOICE_TESTS(MAICORD_RUN_VOICE_TEST)
#undef MAICORD_RUN_VOICE_TEST
    ExitProcess(maicord::failures ? 1 : 0);
}
