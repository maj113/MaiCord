#include "maicord/app/account_stage.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "rest_submit_hook.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

// Capture-the-submission: every stage request reaches the transport with the
// official client's exact target, method, route bucket and JSON body.
namespace maicord {
namespace stage_account_test {
u32 failures;
struct Capture {
    RestRouteKey route;
    u64 id;
    u32 submissions, cancellations, target_bytes, body_bytes, timeout_ms;
    u8 target[kRestClientMaximumTargetBytes], body[kStageMaximumBodyBytes];
    bool json, identity;
    RestClientSubmitError result;
} capture;

void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
bool same(const u8* data, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(data), bytes}, expected);
}
StageRequest request(StageOperation operation, u64 user = 0) noexcept {
    StageRequest r{};
    r.operation = operation;
    r.guild_id = 10;
    r.channel_id = 20;
    r.user_id = user;
    r.unix_ms = 1'791'331'200'123ull; // 2026-10-07T00:00:00Z
    return r;
}
// Submits and compares everything the transport received.
bool submitted(const StageRequest& r, RestMethod method, RestMajorParameter major, u64 major_key,
               Utf8View target, Utf8View body) noexcept {
    RestClient* rest = reinterpret_cast<RestClient*>(&capture);
    const u32 before = capture.submissions;
    if (!account_stage_submit(rest, &r, 77).ok() || capture.submissions != before + 1)
        return false;
    return capture.id == 77 && capture.route.method == method && capture.route.major == major &&
           capture.route.major_key == major_key && (capture.route.route_id >> 24) == (kStageRouteBase >> 24) &&
           capture.timeout_ms == kRestClientDefaultTimeoutMs && capture.json == (body.bytes != 0) &&
           !capture.identity && same(capture.target, capture.target_bytes, target) &&
           same(capture.body, capture.body_bytes, body);
}

void test_voice_states() noexcept {
    using M = RestMethod;
    using P = RestMajorParameter;
    check(submitted(request(StageOperation::request_to_speak), M::patch, P::guild, 10,
                    utf8_literal("/api/v9/guilds/10/voice-states/@me"),
                    utf8_literal(R"({"request_to_speak_timestamp":"2026-10-07T00:00:00+00:00","channel_id":"20"})")),
          "stage account: request to speak sends a timestamp for @me");
    check(submitted(request(StageOperation::lower_hand), M::patch, P::guild, 10,
                    utf8_literal("/api/v9/guilds/10/voice-states/@me"),
                    utf8_literal(R"({"request_to_speak_timestamp":null,"channel_id":"20"})")),
          "stage account: lowering a hand clears the timestamp");
    check(submitted(request(StageOperation::become_speaker), M::patch, P::guild, 10,
                    utf8_literal("/api/v9/guilds/10/voice-states/@me"),
                    utf8_literal(R"({"suppress":false,"request_to_speak_timestamp":null,"channel_id":"20"})")),
          "stage account: accepting an invite unsuppresses and clears the hand");
    auto silent = request(StageOperation::become_speaker);
    silent.silent = true;
    check(submitted(silent, M::patch, P::guild, 10, utf8_literal("/api/v9/guilds/10/voice-states/@me"),
                    utf8_literal(R"({"suppress":false,"request_to_speak_timestamp":null,"channel_id":"20","silent":true})")),
          "stage account: a moderator who started the stage joins the speakers silently");
    check(submitted(request(StageOperation::decline_invite), M::patch, P::guild, 10,
                    utf8_literal("/api/v9/guilds/10/voice-states/@me"),
                    utf8_literal(R"({"suppress":true,"request_to_speak_timestamp":null,"channel_id":"20"})")),
          "stage account: declining an invite stays suppressed");
    check(submitted(request(StageOperation::move_self_to_audience), M::patch, P::guild, 10,
                    utf8_literal("/api/v9/guilds/10/voice-states/@me"),
                    utf8_literal(R"({"suppress":true,"channel_id":"20","self_video":false,"self_stream":false})")),
          "stage account: moving yourself to the audience also stops video and streams");
    check(submitted(request(StageOperation::invite_to_speak, 30), M::patch, P::guild, 10,
                    utf8_literal("/api/v9/guilds/10/voice-states/30"),
                    utf8_literal(
                        R"({"suppress":false,"request_to_speak_timestamp":"2026-10-07T00:00:00+00:00","channel_id":"20"})")),
          "stage account: a moderator invites a requester by user ID");
    check(submitted(request(StageOperation::move_to_audience, 30), M::patch, P::guild, 10,
                    utf8_literal("/api/v9/guilds/10/voice-states/30"),
                    utf8_literal(R"({"suppress":true,"channel_id":"20"})")),
          "stage account: a moderator moves a speaker to the audience");
}

void test_instances() noexcept {
    using M = RestMethod;
    using P = RestMajorParameter;
    auto start = request(StageOperation::start);
    start.topic = utf8_literal("Q&A \"live\"");
    check(submitted(start, M::post, P::none, 0, utf8_literal("/api/v9/stage-instances"),
                    utf8_literal(
                        R"({"channel_id":"20","topic":"Q&A \"live\"","privacy_level":2,"send_start_notification":false})")),
          "stage account: starting posts the channel, escaped topic and guild-only privacy");
    start.send_start_notification = true;
    start.privacy_level = 1; // Retired PUBLIC level passes through unchanged.
    check(submitted(start, M::post, P::none, 0, utf8_literal("/api/v9/stage-instances"),
                    utf8_literal(
                        R"({"channel_id":"20","topic":"Q&A \"live\"","privacy_level":1,"send_start_notification":true})")),
          "stage account: start notification and privacy pass through");
    auto topic = request(StageOperation::update_topic);
    topic.topic = utf8_literal("Round two");
    check(submitted(topic, M::patch, P::none, 0, utf8_literal("/api/v9/stage-instances/20"),
                    utf8_literal(R"({"topic":"Round two","privacy_level":2})")),
          "stage account: editing the topic patches the channel's instance");
    check(submitted(request(StageOperation::end), M::delete_method, P::none, 0,
                    utf8_literal("/api/v9/stage-instances/20"), {}),
          "stage account: ending deletes the instance without a body");
}

// A voice channel's status: PUT /channels/{id}/voice-status, channel-major.
void test_voice_status() noexcept {
    using M = RestMethod;
    using P = RestMajorParameter;
    auto status = request(StageOperation::voice_status);
    status.topic = utf8_literal("Late night \"jam\" \xF0\x9F\x8E\xB8");
    check(submitted(status, M::put, P::channel, 20, utf8_literal("/api/v9/channels/20/voice-status"),
                    utf8_literal("{\"status\":\"Late night \\\"jam\\\" \xF0\x9F\x8E\xB8\"}")),
          "voice status: setting it puts the status on the channel");
    check(submitted(request(StageOperation::voice_status), M::put, P::channel, 20,
                    utf8_literal("/api/v9/channels/20/voice-status"), utf8_literal(R"({"status":""})")),
          "voice status: an empty status clears it");
    char text[kStageVoiceStatusMaximumBytes + 4];
    for (auto& c : text)
        c = 'a';
    status.topic = {text, kStageVoiceStatusMaximumCodepoints};
    const bool longest = stage_request_valid(status);
    status.topic = {text, kStageVoiceStatusMaximumCodepoints + 1};
    const bool too_long = stage_request_valid(status);
    status.topic = utf8_literal("one\ntwo");
    const bool lines = stage_request_valid(status);
    // 500 four-byte code points fit; the byte limit follows the code points.
    for (u32 i = 0; i + 4 <= kStageVoiceStatusMaximumBytes; i += 4)
        copy_bytes(text + i, "\xF0\x9F\x8E\xB8", 4);
    status.topic = {text, kStageVoiceStatusMaximumBytes};
    const bool wide = stage_request_valid(status);
    check(longest && !too_long && !lines && wide,
          "voice status: one line of up to 500 code points");
}

void test_rejections() noexcept {
    RestClient* rest = reinterpret_cast<RestClient*>(&capture);
    const u32 before = capture.submissions;
    auto start = request(StageOperation::start);
    auto long_topic = request(StageOperation::update_topic);
    char text[200];
    for (auto& c : text)
        c = 'a';
    long_topic.topic = {text, 121};
    auto blank = request(StageOperation::start);
    blank.topic = utf8_literal("   ");
    auto lines = request(StageOperation::start);
    lines.topic = utf8_literal("one\ntwo");
    auto no_user = request(StageOperation::invite_to_speak);
    auto no_guild = request(StageOperation::request_to_speak);
    no_guild.guild_id = 0;
    StageRequest bad{};
    bad.operation = StageOperation::count;
    bad.channel_id = 1;
    check(account_stage_submit(rest, &start, 1).error == RestClientSubmitError::invalid_input &&
              account_stage_submit(rest, &long_topic, 1).error == RestClientSubmitError::invalid_input &&
              account_stage_submit(rest, &blank, 1).error == RestClientSubmitError::invalid_input &&
              account_stage_submit(rest, &lines, 1).error == RestClientSubmitError::invalid_input &&
              account_stage_submit(rest, &no_user, 1).error == RestClientSubmitError::invalid_input &&
              account_stage_submit(rest, &no_guild, 1).error == RestClientSubmitError::invalid_input &&
              account_stage_submit(rest, &bad, 1).error == RestClientSubmitError::invalid_input &&
              account_stage_submit(nullptr, &no_user, 1).error == RestClientSubmitError::invalid_input &&
              account_stage_submit(rest, nullptr, 1).error == RestClientSubmitError::invalid_input &&
              capture.submissions == before,
          "stage account: empty, blank, multi-line or 121-code-point topics and missing IDs never submit");
    long_topic.topic = {text, 120};
    check(stage_request_valid(long_topic), "stage account: a 120-code-point topic is accepted");
    capture.result = RestClientSubmitError::busy;
    const auto lower = request(StageOperation::lower_hand);
    check(account_stage_submit(rest, &lower, 5).error == RestClientSubmitError::busy,
          "stage account: transport backpressure propagates");
    capture.result = RestClientSubmitError::none;
    check(account_stage_cancel(rest, 5).ok() && capture.cancellations == 1 &&
              account_stage_cancel(rest, 0).error == RestClientSubmitError::invalid_input,
          "stage account: cancel names exactly one request");
    u8 small[8];
    check(!stage_write_body(lower, {small, sizeof(small)}).ok() && !stage_write_target(lower, {small, 4}).ok(),
          "stage account: writers refuse short output");
}
} // namespace stage_account_test

static RestClientSubmitResult submit_stub(RestClient*,
                                          const RestClientRequest& request) noexcept {
    auto& c = stage_account_test::capture;
    ++c.submissions;
    c.route = request.route;
    c.id = request.request_id;
    c.timeout_ms = request.timeout_ms;
    c.json = request.json_body;
    c.identity = request.identity_response;
    if (request.target.count > sizeof(c.target) || request.body.count > sizeof(c.body))
        return {RestClientSubmitError::invalid_input};
    c.target_bytes = request.target.count;
    c.body_bytes = request.body.count;
    copy_bytes(c.target, request.target.data, request.target.count);
    if (request.body.count)
        copy_bytes(c.body, request.body.data, request.body.count);
    return {c.result};
}

static bool cancel_stub(RestClient*, u64 request_id) noexcept {
    ++stage_account_test::capture.cancellations;
    return request_id == 5;
}
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_account_stage_entry() noexcept {
    maicord::rest_install_hooks({.submit = maicord::submit_stub, .cancel = maicord::cancel_stub});
    using namespace maicord::stage_account_test;
    test_voice_states();
    test_instances();
    test_voice_status();
    test_rejections();
    constexpr char pass[] = "PASS stage requests\n";
    constexpr char fail[] = "FAIL stage requests\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failures ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failures ? 1 : 0);
}
