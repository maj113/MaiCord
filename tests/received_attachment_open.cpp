#include "maicord/app/received_attachment_open.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/protocol/writer.hpp"
#include "test_output.hpp"

namespace maicord::received_attachment_test {
u32 failures{};
MediaUrlResolver resolver{};
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
void run() noexcept {
    const ReceivedAttachmentOpenIdentity identity{1, 7, 10, 70, 3};
    constexpr auto source = utf8_literal("https://cdn.discordapp.com/attachments/7/70/report.pdf");
    constexpr auto fresh = utf8_literal("https://cdn.discordapp.com/attachments/7/70/report.pdf?ex=ffff&is=1&hm="
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    ReceivedAttachmentOpen intent{};
    Utf8View ready{};
    check(received_attachment_open_begin(&intent, identity, fresh, &resolver, 1000, 100, &ready) ==
              ReceivedAttachmentOpenResult::ready && utf8_equal(ready, fresh) && !intent.pending &&
              resolver.entries[0].state == MediaUrlState::empty,
          "attachment open: current validated HTTPS receipt activates immediately without another resolver slot");
    check(received_attachment_open_begin(&intent, identity, source, &resolver, 1000, 100, &ready) ==
              ReceivedAttachmentOpenResult::pending && !ready.bytes && intent.pending &&
              resolver.entries[0].state == MediaUrlState::queued,
          "attachment open: unsigned source queues existing refresh and retains only one scoped intent");
    check(received_attachment_open_resume(&intent, identity, source, &resolver, 1001, 100, &ready) ==
              ReceivedAttachmentOpenResult::pending && !ready.bytes &&
              resolver.entries[1].state == MediaUrlState::empty,
          "attachment open: repeated frame observation cannot publish expired bytes or duplicate refresh owners");
    u8 body[512]{};
    ProtocolWriter writer{{body, sizeof(body)}, 0};
    const bool encoded = protocol_writer_append_literal(&writer, "{\"refreshed_urls\":[{\"original\":") &&
        protocol_writer_append_json_string(&writer, source) && protocol_writer_append_literal(&writer, ",\"refreshed\":") &&
        protocol_writer_append_json_string(&writer, fresh) && protocol_writer_append_literal(&writer, "}]}");
    resolver.pending_slot = 1;
    resolver.entries[0].state = MediaUrlState::pending;
    RestClientResponse response{};
    response.status_code = 200;
    media_url_complete(&resolver, response, {reinterpret_cast<const char*>(body), writer.used}, 1002, 100);
    check(encoded && received_attachment_open_resume(&intent, identity, source, &resolver, 1003, 100, &ready) ==
              ReceivedAttachmentOpenResult::ready && utf8_equal(ready, fresh) && !intent.pending &&
              received_attachment_open_resume(&intent, identity, source, &resolver, 1004, 100, &ready) ==
                  ReceivedAttachmentOpenResult::none && !ready.bytes,
          "attachment open: exact synthetic refresh receipt activates once and consumes the deferred intent");
    const Utf8View invalid[]{utf8_literal("file:///C:/report.pdf"), utf8_literal("javascript:alert(1)"),
        utf8_literal("https://example.com/attachments/7/70/report.pdf"),
        utf8_literal("https://cdn.discordapp.com/attachments/8/70/report.pdf"),
        utf8_literal("https://cdn.discordapp.com/attachments/7/71/report.pdf"),
        utf8_literal("https://cdn.discordapp.com@evil.test/attachments/7/70/report.pdf"),
        utf8_literal("https://cdn.discordapp.com/attachments/7/70/report.pdf#fragment"),
        utf8_literal("https://cdn.discordapp.com/attachments/7/70/report.pdf?ex=bad")};
    bool rejected = true;
    for (const auto url : invalid) {
        resolver = {};
        rejected = received_attachment_open_begin(&intent, identity, url, &resolver, 2000, 100, &ready) ==
                       ReceivedAttachmentOpenResult::invalid && !intent.pending && !ready.bytes &&
                   resolver.entries[0].state == MediaUrlState::empty && rejected;
    }
    check(rejected, "attachment open: executable schemes external hosts wrong channel or attachment identity fragments and malformed signature never reach refresh or activation");
    ReceivedAttachmentOpenIdentity stale[5]{};
    for (auto& id : stale) id = identity;
    ++stale[0].account_scope; ++stale[1].channel_id; ++stale[2].message_id;
    ++stale[3].attachment_id; ++stale[4].message_revision;
    bool fenced = true;
    for (const auto& id : stale) {
        resolver = {};
        received_attachment_open_begin(&intent, identity, source, &resolver, 3000, 100, &ready);
        fenced = received_attachment_open_resume(&intent, id, source, &resolver, 3001, 100, &ready) ==
                     ReceivedAttachmentOpenResult::stale && !intent.pending && !ready.bytes && fenced;
    }
    resolver = {};
    received_attachment_open_begin(&intent, identity, source, &resolver, 3000, 100, &ready);
    fenced = received_attachment_open_resume(&intent, identity,
        utf8_literal("https://cdn.discordapp.com/attachments/7/70/replaced.pdf"), &resolver, 3001, 100, &ready) ==
            ReceivedAttachmentOpenResult::stale && !ready.bytes && fenced;
    check(fenced, "attachment open: account switch channel navigation deletion revision and replaced source invalidate late user intent");
    resolver = {};
    received_attachment_open_begin(&intent, identity, source, &resolver, 4000, 100, &ready);
    resolver.pending_slot = 1;
    resolver.entries[0].state = MediaUrlState::pending;
    resolver.entries[0].attempts = 1;
    response.status_code = 429;
    response.retry_at_ms = 9000;
    media_url_complete(&resolver, response, {}, 4001, 100);
    check(received_attachment_open_resume(&intent, identity, source, &resolver, 4002, 100, &ready) ==
              ReceivedAttachmentOpenResult::pending && !ready.bytes && media_url_next_wake(resolver) == 9000 &&
              !media_url_next(&resolver, 8999),
          "attachment open: rate receipt retains exact existing retry deadline without a second clock or request");
    resolver.pending_slot = 1;
    resolver.entries[0].state = MediaUrlState::pending;
    response.status_code = 403;
    media_url_complete(&resolver, response, {}, 9001, 100);
    check(received_attachment_open_resume(&intent, identity, source, &resolver, 9002, 100, &ready) ==
              ReceivedAttachmentOpenResult::failed && !ready.bytes && !intent.pending &&
              received_attachment_open_resume(&intent, identity, source, &resolver, 9003, 100, &ready) ==
                  ReceivedAttachmentOpenResult::none,
          "attachment open: forbidden or deleted refresh fails once without opening stale bytes or spinning");
    resolver.entries[1].state = MediaUrlState::failed;
    resolver.entries[1].attempts = 3;
    resolver.entries[1].retry_at_ms = 50000;
    check(received_attachment_open_begin(&intent, identity, source, &resolver, 9004, 100, &ready) ==
              ReceivedAttachmentOpenResult::pending && !ready.bytes && intent.pending &&
              resolver.entries[0].state == MediaUrlState::queued && !resolver.entries[0].attempts &&
              !resolver.entries[0].retry_at_ms && resolver.entries[1].attempts == 3 &&
              resolver.entries[1].retry_at_ms == 50000,
          "attachment open: explicit user retry rearms only its exact failed receipt and preserves other source budgets");
    resolver = {};
    for (auto& entry : resolver.entries) entry.state = MediaUrlState::pending;
    check(received_attachment_open_begin(&intent, identity, source, &resolver, 10000, 100, &ready) ==
              ReceivedAttachmentOpenResult::failed && !intent.pending && !ready.bytes,
          "attachment open: saturated bounded resolver refuses intent instead of an indefinite pending state");
    resolver = {};
    received_attachment_open_begin(&intent, identity, source, &resolver, 11000, 100, &ready);
    const auto wrong = utf8_literal("https://cdn.discordapp.com/attachments/8/70/report.pdf?ex=ffff&is=1&hm="
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    copy_bytes(resolver.entries[0].refreshed, wrong.data, wrong.bytes);
    resolver.entries[0].refreshed_bytes = static_cast<u16>(wrong.bytes);
    resolver.entries[0].state = MediaUrlState::ready;
    check(received_attachment_open_resume(&intent, identity, source, &resolver, 11001, 100, &ready) ==
              ReceivedAttachmentOpenResult::invalid && !ready.bytes && !intent.pending,
          "attachment open: even a corrupt ready arena cannot activate another channel's attachment");
    resolver = {};
    auto forwarded = identity;
    forwarded.source_channel_id = 8;
    check(received_attachment_open_begin(&intent, forwarded, wrong, &resolver, 12000, 100, &ready) ==
              ReceivedAttachmentOpenResult::ready && utf8_equal(ready, wrong) &&
              forwarded.channel_id == identity.channel_id && forwarded.message_id == identity.message_id,
          "attachment open: proven forward origin permits exact original CDN channel while preserving enclosing action scope");
    constexpr auto forwarded_source = utf8_literal("https://cdn.discordapp.com/attachments/8/70/report.pdf");
    check(received_attachment_open_begin(&intent, forwarded, source, &resolver, 12001, 100, &ready) ==
              ReceivedAttachmentOpenResult::invalid && !ready.bytes,
          "attachment open: forwarded identity rejects the enclosing channel when the attachment belongs to another origin");
    received_attachment_open_begin(&intent, forwarded, forwarded_source, &resolver, 12002, 100, &ready);
    auto switched_origin = forwarded;
    switched_origin.source_channel_id = 9;
    check(received_attachment_open_resume(&intent, switched_origin, forwarded_source, &resolver,
              12003, 100, &ready) == ReceivedAttachmentOpenResult::stale && !intent.pending && !ready.bytes,
          "attachment open: replaced forward origin cannot complete a retained original attachment intent");

    constexpr auto distant = utf8_literal("https://cdn.discordapp.com/attachments/7/70/report.pdf?ex=ffffffff&is=1&hm="
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    constexpr auto renewed = utf8_literal("https://media.discordapp.net/attachments/7/70/report.pdf?ex=ffffffff&is=2&hm="
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    resolver = {};
    check(received_attachment_open_begin(&intent, identity, distant, &resolver, 13000, 100, &ready, true) ==
              ReceivedAttachmentOpenResult::pending && !ready.bytes && intent.pending && intent.force_refresh &&
              resolver.entries[0].state == MediaUrlState::queued && media_url_next(&resolver, 13000) == &resolver.entries[0],
          "attachment forced retry: valid far-future original queues an exact POST receipt instead of reopening the rejected URL");
    const auto post_bytes = attachment_refresh_body(distant, {body, sizeof(body)});
    const auto expected_post = utf8_literal("{\"attachment_urls\":[\"https://cdn.discordapp.com/attachments/7/70/report.pdf?ex=ffffffff&is=1&hm="
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"]}");
    check(post_bytes == expected_post.bytes && bytes_equal(body, expected_post.data, post_bytes),
          "attachment forced retry: refresh POST preserves the exact signed original source");
    resolver.pending_slot = 1; resolver.entries[0].state = MediaUrlState::pending;
    resolver.entries[0].attempts = 2; resolver.entries[0].retry_at_ms = 20000;
    bool pending_unchanged = true;
    for (u32 frame = 0; frame < 4; ++frame) {
        pending_unchanged = received_attachment_open_resume(&intent, identity, distant, &resolver, 13001 + frame, 100, &ready) ==
            ReceivedAttachmentOpenResult::pending && !ready.bytes && resolver.pending_slot == 1 &&
            resolver.entries[0].state == MediaUrlState::pending && resolver.entries[0].attempts == 2 &&
            resolver.entries[0].retry_at_ms == 20000 && resolver.entries[1].state == MediaUrlState::empty && pending_unchanged;
    }
    check(pending_unchanged && received_attachment_open_begin(&intent, identity, distant, &resolver, 13010, 100, &ready, true) ==
              ReceivedAttachmentOpenResult::pending && resolver.entries[0].attempts == 2 && resolver.entries[0].retry_at_ms == 20000,
          "attachment forced retry: frames and another explicit click do not reset an already pending source request");
    writer = {{body, sizeof(body)}, 0};
    const bool renewed_body = protocol_writer_append_literal(&writer, "{\"refreshed_urls\":[{\"original\":") &&
        protocol_writer_append_json_string(&writer, distant) && protocol_writer_append_literal(&writer, ",\"refreshed\":") &&
        protocol_writer_append_json_string(&writer, renewed) && protocol_writer_append_literal(&writer, "}]}");
    response = {}; response.status_code = 200;
    media_url_complete(&resolver, response, {reinterpret_cast<const char*>(body), writer.used}, 13011, 100);
    check(renewed_body && received_attachment_open_resume(&intent, identity, distant, &resolver, 13012, 100, &ready) ==
              ReceivedAttachmentOpenResult::ready && utf8_equal(ready, renewed) && !intent.pending && !intent.force_refresh &&
              received_attachment_open_resume(&intent, identity, distant, &resolver, 13013, 100, &ready) == ReceivedAttachmentOpenResult::none,
          "attachment forced retry: exact fresh response replaces rejected authority and consumes intent once");
    resolver.entries[0].attempts = 3; resolver.entries[0].retry_at_ms = 50000;
    resolver.entries[1].state = MediaUrlState::failed; resolver.entries[1].attempts = 3;
    resolver.entries[1].retry_at_ms = 60000; resolver.entries[1].original_bytes = 1; resolver.entries[1].original[0] = 'x';
    check(received_attachment_open_begin(&intent, identity, distant, &resolver, 13020, 100, &ready, true) ==
              ReceivedAttachmentOpenResult::pending && !ready.bytes && resolver.entries[0].state == MediaUrlState::queued &&
              !resolver.entries[0].attempts && !resolver.entries[0].retry_at_ms && !resolver.entries[0].refreshed_bytes &&
              resolver.entries[1].state == MediaUrlState::failed && resolver.entries[1].attempts == 3 && resolver.entries[1].retry_at_ms == 60000,
          "attachment forced retry: explicit retry rearms its exact ready cache once while preserving other source budgets");
    resolver.entries[0].attempts = 1; resolver.entries[0].retry_at_ms = 17000;
    check(received_attachment_open_resume(&intent, identity, distant, &resolver, 13021, 100, &ready) ==
              ReceivedAttachmentOpenResult::pending && resolver.entries[0].attempts == 1 && resolver.entries[0].retry_at_ms == 17000 &&
              !media_url_next(&resolver, 16999),
          "attachment forced retry: queued frame observation retains the real retry deadline");
    resolver.pending_slot = 1; resolver.entries[0].state = MediaUrlState::pending;
    response.status_code = 403;
    media_url_complete(&resolver, response, {}, 17001, 100);
    check(received_attachment_open_resume(&intent, identity, distant, &resolver, 17002, 100, &ready) ==
              ReceivedAttachmentOpenResult::failed && !ready.bytes && !intent.pending && !intent.force_refresh &&
              received_attachment_open_resume(&intent, identity, distant, &resolver, 17003, 100, &ready) ==
                  ReceivedAttachmentOpenResult::none && resolver.entries[0].state == MediaUrlState::failed,
          "attachment forced retry: terminal refresh failure stops frame resumption without reopening original authority");
    check(received_attachment_open_begin(&intent, identity, distant, &resolver, 17004, 100, &ready, true) ==
              ReceivedAttachmentOpenResult::pending && resolver.entries[0].state == MediaUrlState::queued &&
              !resolver.entries[0].attempts && !resolver.entries[0].retry_at_ms && resolver.entries[1].retry_at_ms == 60000,
          "attachment forced retry: a later explicit user retry can rearm the exact failed receipt");
    auto stale_forced = identity; ++stale_forced.account_scope;
    check(received_attachment_open_resume(&intent, stale_forced, distant, &resolver, 17005, 100, &ready) ==
              ReceivedAttachmentOpenResult::stale && !ready.bytes && !intent.pending && !intent.force_refresh &&
              resolver.entries[0].state == MediaUrlState::queued,
          "attachment forced retry: account identity change consumes user intent without rearming or opening its late receipt");
    resolver = {};
    bool forced_rejected = true;
    for (const auto unsafe : invalid)
        forced_rejected = received_attachment_open_begin(&intent, identity, unsafe, &resolver, 18000, 100, &ready, true) ==
            ReceivedAttachmentOpenResult::invalid && !intent.pending && !ready.bytes &&
            resolver.entries[0].state == MediaUrlState::empty && forced_rejected;
    const Utf8View forced_invalid[]{invalid[0], invalid[1], invalid[2], invalid[5], invalid[6], invalid[7]};
    for (const auto unsafe : forced_invalid)
        forced_rejected = !media_url_resolve(&resolver, unsafe, 18000, 100, true).bytes && forced_rejected;
    check(forced_rejected && resolver.entries[0].state == MediaUrlState::empty,
          "attachment forced retry: invalid authority or source identity cannot queue authenticated refresh work");
    check(received_attachment_open_begin(&intent, identity, distant, &resolver, 18001, 100, &ready) ==
              ReceivedAttachmentOpenResult::ready && utf8_equal(ready, distant) && resolver.entries[0].state == MediaUrlState::empty,
          "attachment forced retry: ordinary far-future attachment open retains immediate source behavior");
    received_attachment_open_begin(&intent, identity, distant, &resolver, 18002, 100, &ready, true);
    resolver.entries[0] = {};
    check(received_attachment_open_resume(&intent, identity, distant, &resolver, 18003, 100, &ready) ==
              ReceivedAttachmentOpenResult::failed && !ready.bytes && !intent.pending && resolver.entries[0].state == MediaUrlState::empty,
          "attachment forced retry: evicted exact receipt ends intent without a frame-triggered replacement request");
    received_attachment_open_begin(&intent, identity, distant, &resolver, 18004, 100, &ready, true);
    copy_bytes(resolver.entries[0].refreshed, fresh.data, fresh.bytes);
    resolver.entries[0].refreshed_bytes = static_cast<u16>(fresh.bytes); resolver.entries[0].state = MediaUrlState::ready;
    check(received_attachment_open_resume(&intent, identity, distant, &resolver, 18005, 65535, &ready) ==
              ReceivedAttachmentOpenResult::failed && !ready.bytes && !intent.pending && resolver.entries[0].state == MediaUrlState::ready,
          "attachment forced retry: expired ready receipt cannot silently rearm from frame observation");
}
} // namespace maicord::received_attachment_test
extern "C" __declspec(noreturn) void WINAPI maicord_received_attachment_open_test_entry() noexcept {
    maicord::received_attachment_test::run();
    const bool failed = maicord::received_attachment_test::failures != 0;
    constexpr char pass[] = "PASS received attachment open\n", fail[] = "FAIL received attachment open\n";
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}
