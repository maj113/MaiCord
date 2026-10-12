#include "maicord/core/base.hpp"
#include "test_output.hpp"

namespace maicord {
void test_aes_gcm(void (*check)(bool, const char*) noexcept) noexcept;
void test_aes_gcm64(void (*check)(bool, const char*) noexcept) noexcept;
void test_xchacha20_poly1305(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_packet_crypto(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_protocol_selection(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_udp_media(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_participants(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_media_dispatch(void (*check)(bool, const char*) noexcept) noexcept;
void test_dave_replay(void (*check)(bool, const char*) noexcept) noexcept;
void test_dave_frame(void (*check)(bool, const char*) noexcept) noexcept;
void test_dave_frame_crypto(void (*check)(bool, const char*) noexcept) noexcept;
void test_dave_key_derivation(void (*check)(bool, const char*) noexcept) noexcept;
void test_mls_exporter(void (*check)(bool, const char*) noexcept) noexcept;
void test_mls_key_schedule(void (*check)(bool, const char*) noexcept) noexcept;
void test_p256(void (*check)(bool, const char*) noexcept) noexcept;
void test_mls_signature(void (*check)(bool, const char*) noexcept) noexcept;
void test_hpke_derivation(void (*check)(bool, const char*) noexcept) noexcept;
void test_hpke_context(void (*check)(bool, const char*) noexcept) noexcept;
void test_mls_hpke(void (*check)(bool, const char*) noexcept) noexcept;
void test_mls_objects(void (*check)(bool, const char*) noexcept) noexcept;
void test_mls_join_messages(void (*check)(bool, const char*) noexcept) noexcept;
void test_dave_key_package(void (*check)(bool, const char*) noexcept) noexcept;
void test_dave_welcome(void (*check)(bool, const char*) noexcept) noexcept;
void test_dave_package_policy(void (*check)(bool, const char*) noexcept) noexcept;
void test_dave_receive_epoch(void (*check)(bool, const char*) noexcept) noexcept;
void test_dave_receive_sender(void (*check)(bool, const char*) noexcept) noexcept;
void test_dave_session(void (*check)(bool, const char*) noexcept) noexcept;
void test_gateway_voice(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_connection_setup(void (*check)(bool, const char*) noexcept) noexcept;
void test_gateway_voice_control(void (*check)(bool, const char*) noexcept) noexcept;
void test_account_session_identity(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_gateway(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_heartbeat(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_session(void (*check)(bool, const char*) noexcept) noexcept;
void test_tls_stream_lifecycle(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_socket(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_connection(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_media_events(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_udp(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_udp_handoff(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_state_store(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_gateway_policy(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_jitter_buffer(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_playback(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_receive(void (*check)(bool, const char*) noexcept) noexcept;
void test_video_rtp_receive(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_video_signaling(void (*check)(bool, const char*) noexcept) noexcept;
void test_dave_video_frames(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_video_sink(void (*check)(bool, const char*) noexcept) noexcept;
void test_gateway_stream(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_send(void (*check)(bool, const char*) noexcept) noexcept;
void test_voice_preferences(void (*check)(bool, const char*) noexcept) noexcept;
void test_call_view(void (*check)(bool, const char*) noexcept) noexcept;
void test_stream_watch(void (*check)(bool, const char*) noexcept) noexcept;
void test_stage_channel(void (*check)(bool, const char*) noexcept) noexcept;
namespace {
u32 failures;
void check(bool value, const char* message) noexcept {
    test_check(&failures, value, message);
}
} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_voice_tests_entry() noexcept {
    maicord::test_aes_gcm(maicord::check);
    maicord::test_aes_gcm64(maicord::check);
    maicord::test_xchacha20_poly1305(maicord::check);
    maicord::test_voice_packet_crypto(maicord::check);
    maicord::test_voice_protocol_selection(maicord::check);
    maicord::test_voice_udp_media(maicord::check);
    maicord::test_voice_participants(maicord::check);
    maicord::test_voice_media_dispatch(maicord::check);
    maicord::test_dave_replay(maicord::check);
    maicord::test_dave_frame(maicord::check);
    maicord::test_dave_frame_crypto(maicord::check);
    maicord::test_dave_key_derivation(maicord::check);
    maicord::test_mls_exporter(maicord::check);
    maicord::test_mls_key_schedule(maicord::check);
    maicord::test_p256(maicord::check);
    maicord::test_mls_signature(maicord::check);
    maicord::test_hpke_derivation(maicord::check);
    maicord::test_hpke_context(maicord::check);
    maicord::test_mls_hpke(maicord::check);
    maicord::test_mls_objects(maicord::check);
    maicord::test_mls_join_messages(maicord::check);
    maicord::test_dave_key_package(maicord::check);
    maicord::test_dave_welcome(maicord::check);
    maicord::test_dave_package_policy(maicord::check);
    maicord::test_dave_receive_epoch(maicord::check);
    maicord::test_dave_receive_sender(maicord::check);
    maicord::test_dave_session(maicord::check);
    maicord::test_gateway_voice(maicord::check);
    maicord::test_voice_connection_setup(maicord::check);
    maicord::test_gateway_voice_control(maicord::check);
    maicord::test_account_session_identity(maicord::check);
    maicord::test_voice_gateway(maicord::check);
    maicord::test_voice_heartbeat(maicord::check);
    maicord::test_voice_session(maicord::check);
    maicord::test_tls_stream_lifecycle(maicord::check);
    maicord::test_voice_socket(maicord::check);
    maicord::test_voice_connection(maicord::check);
    maicord::test_voice_media_events(maicord::check);
    maicord::test_voice_udp(maicord::check);
    maicord::test_voice_udp_handoff(maicord::check);
    maicord::test_voice_state_store(maicord::check);
    maicord::test_voice_gateway_policy(maicord::check);
    maicord::test_voice_jitter_buffer(maicord::check);
    maicord::test_voice_playback(maicord::check);
    maicord::test_voice_receive(maicord::check);
    maicord::test_video_rtp_receive(maicord::check);
    maicord::test_voice_video_signaling(maicord::check);
    maicord::test_dave_video_frames(maicord::check);
    maicord::test_voice_video_sink(maicord::check);
    maicord::test_gateway_stream(maicord::check);
    maicord::test_voice_send(maicord::check);
    maicord::test_voice_preferences(maicord::check);
    maicord::test_call_view(maicord::check);
    maicord::test_stream_watch(maicord::check);
    maicord::test_stage_channel(maicord::check);
    ExitProcess(maicord::failures ? 1 : 0);
}
