#include "maicord/app/account_forum_post.hpp"
#include "maicord/app/account_host.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include <stddef.h>
#include "test_output.hpp"

namespace maicord {
static_assert(sizeof(AccountUploadResult) == 48);
static_assert(offsetof(AccountUploadResult, owns_transfer) == 42);
static_assert(sizeof(ForumPostRequest) == 144);
static_assert(offsetof(ForumPostRequest, attachments) == 96);
static_assert(offsetof(ForumPostRequest, upload_filename) == 112);
static_assert(offsetof(ForumPostRequest, upload_bytes) == 128);
static_assert(offsetof(ForumPostRequest, upload_id) == 136);
static_assert(offsetof(ForumPostRequest, upload_reserved) == 140);
namespace forum_account_test {
u32 failures;
struct Capture {
    RestClient* client;
    RestRouteKey route;
    u64 id, active_id;
    u32 submissions, cancellations, target_bytes, body_bytes;
    u8 target[kRestClientMaximumTargetBytes], body[kForumPostMaximumBodyBytes];
    bool json, safe, published;
    RestClientSubmitError error;
} capture;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
bool equal(const u8* data, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(data), bytes}, expected);
}
void transport() noexcept {
    RestClient details{};
    ForumPostRequest request{};
    request.parent_id = 20;
    request.guild_id = 10;
    request.operation = ForumPostOperation::parent_metadata;
    check(account_forum_post_submit(&details, &request, 101).ok() && capture.safe &&
              capture.client == &details && capture.id == 101 &&
              capture.route.route_id == kForumPostRouteId && capture.route.major_key == 20 &&
              capture.route.major == RestMajorParameter::channel &&
              capture.route.method == RestMethod::get && !capture.json && !capture.body_bytes &&
              equal(capture.target, capture.target_bytes, utf8_literal("/api/v10/channels/20")),
          "forum account: canonical parent metadata GET is bodyless on supplied DETAILS owner");
    char title[] = "A \"post\"", content[] = "Body\nwith \\ escape";
    request.title = {title, sizeof(title) - 1};
    request.content = {content, sizeof(content) - 1};
    request.auto_archive_duration = 4320;
    request.operation = ForumPostOperation::create;
    request.tag_count = 2;
    request.applied_tag_ids[0] = 31;
    request.applied_tag_ids[1] = 32;
    check(account_forum_post_submit(&details, &request, 102).ok() && capture.safe && capture.json &&
              capture.route.method == RestMethod::post && capture.route.major_key == 20 &&
              equal(capture.target, capture.target_bytes,
                    utf8_literal("/api/v10/channels/20/threads?use_nested_fields=true")),
          "forum account: atomic nested-message POST preserves parent major and exact target");
    u8 expected[16384];
    const auto written = forum_post_write_body(request, {expected, sizeof(expected)});
    check(written.ok() && equal(capture.body, capture.body_bytes,
                                {reinterpret_cast<const char*>(expected), written.bytes}),
          "forum account: typed protocol JSON reaches authenticated transport intact");
    for (auto& byte : title)
        byte = 'x';
    for (auto& byte : content)
        byte = 'y';
    request.applied_tag_ids[0] = 99;
    check(equal(capture.body, capture.body_bytes, {reinterpret_cast<const char*>(expected), written.bytes}),
          "forum account: synchronous submit copies borrowed title content and tags before caller reuse");
    char titleMax[400], contentMax[8000];
    const auto fill = [](char* data, u32 bytes) noexcept {
        constexpr u8 scalar[]{0xF0, 0x9F, 0x98, 0x80};
        for (u32 i = 0; i < bytes; i += 4) {
            copy_bytes(data + i, scalar, sizeof(scalar));
        }
    };
    fill(titleMax, sizeof(titleMax));
    fill(contentMax, sizeof(contentMax));
    request.title = {titleMax, sizeof(titleMax)};
    request.content = {contentMax, sizeof(contentMax)};
    request.tag_count = 5;
    for (u32 i = 0; i < 5; ++i)
        request.applied_tag_ids[i] = ~u64{0} - i;
    check(account_forum_post_submit(&details, &request, 103).ok() && capture.body_bytes > 8000 &&
              capture.body_bytes <= kForumPostMaximumBodyBytes &&
              capture.body_bytes <= kRestClientMaximumRequestBodyBytes,
          "forum account: maximum Unicode scalar inputs and five full-width tags fit unchanged REST body "
          "budget");
    UploadedAttachment attachments[10]{};
    char filename[] = "A\".png", uploaded[] = "upload/A\".png", description[] = "Photo";
    attachments[0] = {{filename, sizeof(filename) - 1},
                      {uploaded, sizeof(uploaded) - 1},
                      {description, sizeof(description) - 1},
                      0,
                      true};
    attachments[1] = {utf8_literal("file.pdf"), utf8_literal("upload/file.pdf"), {}, 1, false};
    auto file_request = request;
    file_request.content = {};
    file_request.attachments = {attachments, 2};
    const auto file_body = forum_post_write_body(file_request, {expected, sizeof(expected)});
    check(file_body.ok() && account_forum_post_submit(&details, &file_request, 106).ok() && capture.safe &&
              capture.route.route_id == kForumPostRouteId && capture.route.major_key == 20 &&
              capture.json && capture.route.method == RestMethod::post &&
              capture.body_bytes == file_body.bytes && bytes_equal(capture.body, expected, file_body.bytes),
          "forum account: file-only atomic POST uses typed references on exact parent DETAILS route");
    clear_bytes(filename, sizeof(filename));
    clear_bytes(uploaded, sizeof(uploaded));
    clear_bytes(description, sizeof(description));
    clear_bytes(attachments, sizeof(attachments));
    check(bytes_equal(capture.body, expected, file_body.bytes),
          "forum account: demand-backed submit copies file references before caller reuse and workspace "
          "release");
    ForumPostRequest upload{};
    upload.parent_id = 20;
    upload.guild_id = 10;
    upload.operation = ForumPostOperation::attachment_url;
    upload.upload_filename = utf8_literal("A\".png");
    upload.upload_bytes = ~u64{0};
    upload.upload_id = ~u32{0};
    u8 legacy_target[96], legacy_body[2304];
    const auto legacy = attachment_upload_write(upload.parent_id, upload.upload_id, upload.upload_filename,
                                                upload.upload_bytes, {legacy_target, sizeof(legacy_target)},
                                                {legacy_body, sizeof(legacy_body)});
    const auto route = attachment_upload_route(20);
    check(
        legacy.ok && account_forum_post_submit(&details, &upload, 107).ok() && capture.safe &&
            capture.json && capture.route.route_id == route.route_id &&
            capture.route.major_key == route.major_key && capture.route.major == route.major &&
            capture.route.method == route.method && capture.target_bytes == legacy.target_bytes &&
            bytes_equal(capture.target, legacy_target, legacy.target_bytes) &&
            capture.body_bytes == legacy.body_bytes &&
            bytes_equal(capture.body, legacy_body, legacy.body_bytes),
        "forum account: attachment URL operation preserves established upload endpoint and route identity");
    const u32 before_upload_invalid = capture.submissions;
    auto invalid_upload = upload;
    invalid_upload.upload_reserved = 1;
    check(account_forum_post_submit(&details, &invalid_upload, 108).error ==
                  RestClientSubmitError::invalid_input &&
              capture.submissions == before_upload_invalid,
          "forum account: upload reserved input rejects before transport");
    char quotes[4096];
    for (auto& c : quotes)
        c = '"';
    for (u32 at = 0; at < 10; ++at)
        attachments[at] = {{quotes, 1024}, {quotes, 4096}, {quotes, 1024}, at, false};
    file_request.attachments = {attachments, 10};
    const auto oversized = forum_post_body_bytes(file_request);
    check(forum_post_request_valid(file_request) &&
              oversized.error == ForumPostError::capacity &&
              oversized.bytes > kRestClientMaximumRequestBodyBytes &&
              account_forum_post_submit(&details, &file_request, 109).error ==
                  RestClientSubmitError::body_capacity &&
              capture.submissions == before_upload_invalid,
          "forum account: exact oversized metadata preflight refuses before allocation or transport "
          "without truncation");
    const u32 before = capture.submissions;
    check(!account_forum_post_submit(nullptr, &request, 104).ok() &&
              !account_forum_post_submit(&details, nullptr, 104).ok() &&
              !account_forum_post_submit(&details, &request, 0).ok(),
          "forum account: absent owner request or ID rejects before transport");
    auto invalid = request;
    invalid.parent_id = 0;
    check(!account_forum_post_submit(&details, &invalid, 104).ok(),
          "forum account: invalid parent rejects");
    invalid = request;
    invalid.tag_count = 6;
    check(!account_forum_post_submit(&details, &invalid, 104).ok(), "forum account: tag capacity rejects");
    invalid = request;
    invalid.applied_tag_ids[1] = invalid.applied_tag_ids[0];
    check(!account_forum_post_submit(&details, &invalid, 104).ok(), "forum account: duplicate tag rejects");
    invalid = request;
    invalid.operation = ForumPostOperation::parent_metadata;
    check(!account_forum_post_submit(&details, &invalid, 104).ok(),
          "forum account: metadata operation rejects noncanonical content bank");
    for (u32 i = 0; i < sizeof(request.reserved); ++i) {
        invalid = request;
        invalid.reserved[i] = 1;
        check(!account_forum_post_submit(&details, &invalid, 104).ok(),
              "forum account: reserved byte rejects");
    }
    check(capture.submissions == before, "forum account: rejected inputs never publish a request");
    const RestClientSubmitError errors[]{
        RestClientSubmitError::busy, RestClientSubmitError::signal,
        RestClientSubmitError::memory, RestClientSubmitError::account_restricted};
    for (auto error : errors) {
        capture.error = error;
        const u32 count = capture.submissions;
        check(account_forum_post_submit(&details, &request, 105).error == error &&
                  capture.submissions == count + 1,
              "forum account: admission error propagates exactly once");
    }
    capture.error = RestClientSubmitError::none;
    capture.active_id = 200;
    const u32 cancels = capture.cancellations;
    check(!account_forum_post_cancel(nullptr, 200).ok() && !account_forum_post_cancel(&details, 0).ok() &&
              capture.cancellations == cancels,
          "forum account: invalid cancellation does not touch worker");
    check(account_forum_post_cancel(&details, 199).error == RestClientSubmitError::busy &&
              capture.active_id == 200 && account_forum_post_cancel(&details, 200).ok() &&
              !capture.active_id,
          "forum account: cancellation cannot retire another DETAILS request");
    capture.active_id = 201;
    capture.published = true;
    check(account_forum_post_cancel(&details, 201).error == RestClientSubmitError::busy &&
              capture.active_id == 201,
          "forum account: published receipt remains available for exact drain");
}
void dormant_abi() noexcept {
    static_assert(sizeof(AccountModuleApi) == 1176 && sizeof(AccountHost) == 1424);
    static_assert(offsetof(AccountModuleApi, thread_creation) == 592 &&
                  offsetof(AccountModuleApi, forum_post) == 600);
    AccountHost host{};
    check(account_host_load(&host).ok(), "forum account: cold ABI94 DLL loads without credentials");
    if (!host.module)
        return;
    ForumPostRequest request{};
    request.parent_id = 20;
    request.guild_id = 10;
    request.operation = ForumPostOperation::parent_metadata;
    check(host.api.version == 128 && host.api.bytes == 1176 && host.api.forum_post &&
              host.api.forum_post(nullptr, &request, 1).error == RestClientSubmitError::invalid_input &&
              host.api.forum_post(nullptr, nullptr, 1).error == RestClientSubmitError::invalid_input &&
              !host.instance && !host.login_instance,
          "forum account: dormant callback allocates no account or worker");
    auto missing = host.api;
    missing.media_local_preview = nullptr;
    check(!account_host_api_valid(missing), "forum account: local preview callback is mandatory");
    check(host.api.media_local_preview &&
              host.api.media_local_preview(nullptr, AccountLocalPreviewOperation::request, nullptr, 1, 1)
                      .error == AccountMediaError::invalid_input,
          "forum account: dormant local preview allocates no worker");
    AccountMediaLocalPreviewRequest poster_receipt{};
    poster_receipt.lease_serial = 17;
    check(host.api.media_local_preview &&
              host.api.media_local_preview(nullptr, AccountLocalPreviewOperation::uploaded, &poster_receipt, 0, 0)
                      .error == AccountMediaError::invalid_input &&
              host.api.media_local_preview(nullptr, AccountLocalPreviewOperation::presented, nullptr, 0, 0)
                      .error == AccountMediaError::invalid_input && !host.instance && !host.login_instance,
          "forum account: poster upload and presentation require an account owner");
    missing = host.api;
    missing.forum_post = nullptr;
    check(!account_host_api_valid(missing), "forum account: mandatory callback absence rejects");
    const auto get_api = reinterpret_cast<AccountModuleGetApiFunction>(
        GetProcAddress(host.module, MAKEINTRESOURCEA(kAccountModuleApiOrdinal)));
    AccountModuleApi previous{};
    previous.bytes = 0x12345678;
    check(get_api && !get_api(88, &previous) && previous.bytes == 0x12345678,
          "forum account: previous ABI rejects before writing caller");
    previous.version = 116;
    u8 previous_bytes[sizeof(previous)];
    copy_bytes(previous_bytes, &previous, sizeof(previous));
    check(get_api && !get_api(116, &previous) && bytes_equal(&previous, previous_bytes, sizeof(previous)),
          "forum account: ABI 116 rejects before any caller output changes");
    account_host_release(&host);
}
} // namespace forum_account_test
RestClientSubmitResult rest_client_submit(RestClient* rest,
                                          const RestClientRequest& request) noexcept {
    auto& c = forum_account_test::capture;
    ++c.submissions;
    c.client = rest;
    c.route = request.route;
    c.id = request.request_id;
    c.target_bytes = request.target.count;
    c.body_bytes = request.body.count;
    c.json = request.json_body;
    c.safe = request.target.data && c.target_bytes <= sizeof(c.target) && c.body_bytes <= sizeof(c.body) &&
             (!c.body_bytes || request.body.data) && !request.identity_response &&
             request.timeout_ms == kRestClientDefaultTimeoutMs &&
             (request.json_body || (!request.body.count && !request.body.data));
    if (!c.safe)
        return {RestClientSubmitError::invalid_input};
    copy_bytes(c.target, request.target.data, c.target_bytes);
    if (c.body_bytes)
        copy_bytes(c.body, request.body.data, c.body_bytes);
    return {c.error};
}
bool rest_client_cancel(RestClient* rest, u64 id) noexcept {
    auto& c = forum_account_test::capture;
    ++c.cancellations;
    c.client = rest;
    if (id != c.active_id || c.published)
        return false;
    c.active_id = 0;
    return true;
}
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_account_forum_post_test_entry() noexcept {
    maicord::forum_account_test::transport();
    maicord::forum_account_test::dormant_abi();
    ExitProcess(maicord::forum_account_test::failures ? 1 : 0);
}
