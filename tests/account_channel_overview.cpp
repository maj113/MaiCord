#include "maicord/app/account_channel_overview.hpp"
#include "maicord/core/utf.hpp"
#include "rest_submit_hook.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace channel_overview_account_test {
RestCapture submission;
u32 failures;
void check(bool success, const char* message) noexcept {
    test_check(&failures, success, message);
}
bool captured(RestClient* client, u64 channel, u64 request, RestMethod method, Utf8View target,
              Utf8View body, bool json) noexcept {
    return submission.safe && submission.client == client && submission.request_id == request &&
           submission.route.route_id == kChannelOverviewRouteId && submission.route.major_key == channel &&
           submission.route.major == RestMajorParameter::channel && submission.route.method == method &&
           submission.json == json &&
           utf8_equal({reinterpret_cast<const char*>(submission.target), submission.target_bytes},
                      target) &&
           utf8_equal({reinterpret_cast<const char*>(submission.body), submission.body_bytes}, body);
}
void test_transport() noexcept {
    RestClient client{};
    ChannelOverviewPatch patch{utf8_literal("general"), {}, 0, channel_overview_name, false};
    const u32 untouched = submission.submissions;
    check(account_channel_overview_submit(nullptr, ChannelOverviewOperation::fetch, 20, nullptr, 1).error ==
                  RestClientSubmitError::invalid_input &&
              account_channel_overview_submit(&client, ChannelOverviewOperation::fetch, 0, nullptr, 1)
                      .error == RestClientSubmitError::invalid_input &&
              account_channel_overview_submit(&client, ChannelOverviewOperation::fetch, 20, nullptr, 0)
                      .error == RestClientSubmitError::invalid_input &&
              account_channel_overview_submit(&client, static_cast<ChannelOverviewOperation>(255), 20,
                                              nullptr, 1)
                      .error == RestClientSubmitError::invalid_input &&
              account_channel_overview_submit(&client, ChannelOverviewOperation::fetch, 20, &patch, 1)
                      .error == RestClientSubmitError::invalid_input &&
              account_channel_overview_submit(&client, ChannelOverviewOperation::update, 20, nullptr, 1)
                      .error == RestClientSubmitError::invalid_input &&
              submission.submissions == untouched,
          "channel overview account: invalid owner identity operation and argument combinations never "
          "publish transport work");
    check(
        account_channel_overview_submit(&client, ChannelOverviewOperation::fetch, 20, nullptr, 101).ok() &&
            captured(&client, 20, 101, RestMethod::get, utf8_literal("/api/v10/channels/20"), {}, false),
        "channel overview account: GET uses exact channel-major route bodyless JSON admission and caller "
        "request identity");
    char topic[] = "quote \"\\\n";
    patch = {{}, {topic, sizeof(topic) - 1}, 0, channel_overview_topic, false};
    check(account_channel_overview_submit(&client, ChannelOverviewOperation::update, 20, &patch, 102).ok(),
          "channel overview account: typed topic PATCH is admitted once");
    for (u32 i = 0; i < sizeof(topic) - 1; ++i)
        topic[i] = 'x';
    check(captured(&client, 20, 102, RestMethod::patch, utf8_literal("/api/v10/channels/20"),
                   utf8_literal("{\"topic\":\"quote \\\"\\\\\\n\"}"), true),
          "channel overview account: dirty-only escaped PATCH is copied before adapter stack and caller "
          "strings are reused");
    patch = {{}, {}, 21600, static_cast<u8>(channel_overview_slowmode | channel_overview_nsfw), true};
    check(account_channel_overview_submit(&client, ChannelOverviewOperation::update, ~u64{0}, &patch, 103)
                  .ok() &&
              captured(&client, ~u64{0}, 103, RestMethod::patch,
                       utf8_literal("/api/v10/channels/18446744073709551615"),
                       utf8_literal("{\"rate_limit_per_user\":21600,\"nsfw\":true}"), true),
          "channel overview account: full snowflake and exact boolean slowmode JSON retain channel-major "
          "rate ownership");
    const ChannelOverviewPatch invalid[]{{{}, {}, 0, 0, false},
                                         {{}, {}, 0, 16, false},
                                         {{}, {}, 0, channel_overview_name, false},
                                         {utf8_literal("bad\nname"), {}, 0, channel_overview_name, false},
                                         {{}, {nullptr, 1}, 0, channel_overview_topic, false},
                                         {{}, utf8_literal("\xC0\xAF"), 0, channel_overview_topic, false},
                                         {{}, {}, 21601, channel_overview_slowmode, false}};
    const u32 before_invalid = submission.submissions;
    bool rejected = true;
    for (const auto& input : invalid)
        rejected =
            account_channel_overview_submit(&client, ChannelOverviewOperation::update, 20, &input, 104)
                    .error == RestClientSubmitError::invalid_input &&
            rejected;
    check(rejected && submission.submissions == before_invalid,
          "channel overview account: invalid masks missing names line breaks UTF8 pointer ranges and "
          "slowmode never reach REST");
    char name[100]{}, longest_topic[1024]{};
    for (auto& byte : name)
        byte = 'n';
    for (auto& byte : longest_topic)
        byte = '\1';
    patch = {
        {name, sizeof(name)}, {longest_topic, sizeof(longest_topic)}, 21600, channel_overview_all, true};
    check(
        account_channel_overview_submit(&client, ChannelOverviewOperation::update, 20, &patch, 105).ok() &&
            submission.safe && submission.body_bytes > sizeof(longest_topic) * 6 &&
            submission.body_bytes <= kChannelOverviewMaximumBodyBytes &&
            submission.submissions == before_invalid + 1,
        "channel overview account: maximum fields with six-byte JSON escaping fit ordinary bounded "
        "transport without truncation");
    const RestClientSubmitError errors[]{RestClientSubmitError::busy, RestClientSubmitError::signal,
                                         RestClientSubmitError::account_restricted,
                                         RestClientSubmitError::memory};
    bool propagated = true;
    for (const auto error : errors) {
        submission.error = error;
        const u32 calls = submission.submissions;
        propagated =
            account_channel_overview_submit(&client, ChannelOverviewOperation::fetch, 20, nullptr, 106)
                    .error == error &&
            submission.submissions == calls + 1 && propagated;
    }
    check(propagated, "channel overview account: busy signal restriction and allocation refusal propagate "
                      "exactly without hidden retry");
    submission.error = RestClientSubmitError::none;
}
} // namespace channel_overview_account_test
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_channel_overview_account_entry() noexcept {
    maicord::rest_install_capture(maicord::channel_overview_account_test::submission);
    using namespace maicord::channel_overview_account_test;
    test_transport();
    maicord::test_exit(failures, "Channel Overview account transport");
}
