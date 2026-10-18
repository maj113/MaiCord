#include "maicord/protocol/thread_creation.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::thread_creation_test {
u32 failures;
void check(bool ok, const char* label) noexcept {
    test_check(&failures, ok, label);
}
constexpr auto receipt = utf8_literal(R"({"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"A\u00e9","last_message_id":"22","thread_metadata":{"archived":false,"locked":false,"auto_archive_duration":4320,"archive_timestamp":"2026-10-05T12:34:56.123456+00:00","create_timestamp":null},"member":{"id":"20","user_id":"30","join_timestamp":"2026-10-05T12:34:56Z","flags":4},"future":{"ignored":[1,true,null]}})");
char changed[4096];
Utf8View replace(Utf8View source, Utf8View needle, Utf8View replacement) noexcept {
    for (u32 at = 0; at + needle.bytes <= source.bytes; ++at) {
        if (!bytes_equal(source.data + at, needle.data, needle.bytes)) continue;
        const u32 count = source.bytes - needle.bytes + replacement.bytes;
        if (count > sizeof(changed)) return {};
        copy_bytes(changed, source.data, at);
        copy_bytes(changed + at, replacement.data, replacement.bytes);
        copy_bytes(changed + at + replacement.bytes, source.data + at + needle.bytes, source.bytes - at - needle.bytes);
        return {changed, count};
    }
    return {};
}
void run() noexcept {
    ThreadCreationRequest request{10, 40, 20, utf8_literal("a\"\\\xC3\xA9"),
        kThreadCreationDefaultArchiveDuration, 11, ThreadCreationOperation::from_message, {}};
    u8 target[kThreadCreationMaximumTargetBytes], body[kThreadCreationMaximumBodyBytes];
    auto written = thread_creation_write_target(request, {target, sizeof(target)});
    check(written.ok() && utf8_equal({reinterpret_cast<const char*>(target), written.bytes},
        utf8_literal("/api/v10/channels/10/messages/20/threads")), "thread creation: exact message-origin POST target");
    written = thread_creation_write_body(request, {body, sizeof(body)});
    check(written.ok() && utf8_equal({reinterpret_cast<const char*>(body), written.bytes},
        utf8_literal("{\"name\":\"a\\\"\\\\\xC3\xA9\",\"type\":11,\"auto_archive_duration\":4320}")),
        "thread creation: escaped UTF8 name explicit public type and observed archive fallback only");
    auto reconcile = request; reconcile.operation = ThreadCreationOperation::reconcile;
    written = thread_creation_write_target(reconcile, {target, sizeof(target)});
    body[0] = 0xa5;
    const auto empty = thread_creation_write_body(reconcile, {body, sizeof(body)});
    check(written.ok() && utf8_equal({reinterpret_cast<const char*>(target), written.bytes},
        utf8_literal("/api/v10/channels/20")) && empty.ok() && !empty.bytes && body[0] == 0xa5,
        "thread creation: reconcile GET uses immutable source ID and sends no body");
    auto announcement = request; announcement.thread_type = 10;
    written = thread_creation_write_body(announcement, {body, sizeof(body)});
    check(written.ok() && utf8_equal({reinterpret_cast<const char*>(body), written.bytes},
        utf8_literal("{\"name\":\"a\\\"\\\\\xC3\xA9\",\"type\":10,\"auto_archive_duration\":4320}")),
        "thread creation: announcement parent derives explicit type10");
    auto maximum_ids = request; maximum_ids.parent_id = ~u64{0}; maximum_ids.guild_id = ~u64{0} - 1;
    maximum_ids.source_message_id = ~u64{0} - 2;
    written = thread_creation_write_target(maximum_ids, {target, sizeof(target)});
    check(written.ok() && utf8_equal({reinterpret_cast<const char*>(target), written.bytes}, utf8_literal(
        "/api/v10/channels/18446744073709551615/messages/18446744073709551613/threads")),
        "thread creation: full-width parent and source IDs are exact");
    target[0] = body[0] = 0xa5;
    check(thread_creation_write_target(request, {target, 1}).error == ThreadCreationError::capacity &&
        thread_creation_write_body(request, {body, 1}).error == ThreadCreationError::capacity &&
        target[0] == 0xa5 && body[0] == 0xa5, "thread creation: insufficient writer capacity preserves destination");
    char unicode[400];
    for (u32 at = 0; at < sizeof(unicode); at += 4) {
        unicode[at] = static_cast<char>(0xf0); unicode[at + 1] = static_cast<char>(0x9f);
        unicode[at + 2] = static_cast<char>(0x98); unicode[at + 3] = static_cast<char>(0x80);
    }
    auto boundary = request; boundary.name = {unicode, sizeof(unicode)};
    check(thread_creation_request_valid(boundary) &&
        thread_creation_write_body(boundary, {body, sizeof(body)}).ok(),
        "thread creation: 100 four-byte scalars fit owned name and bounded request body");
    char overlong[401]; for (auto& byte : overlong) byte = 'x';
    constexpr Utf8View invalid_names[]{ {}, utf8_literal("   "), utf8_literal("\xC2\xA0\xE3\x80\x80"),
        utf8_literal("a\0b"), utf8_literal("a\nb"), utf8_literal("\xC2\x85"), utf8_literal("\xFF"),
        utf8_literal("\xC0\xAF"), utf8_literal("\xED\xA0\x80"), utf8_literal("\xF4\x90\x80\x80") };
    for (auto name : invalid_names) {
        auto invalid = request; invalid.name = name;
        target[0] = body[0] = 0xa5;
        check(!thread_creation_request_valid(invalid) &&
            !thread_creation_write_target(invalid, {target, sizeof(target)}).ok() &&
            !thread_creation_write_body(invalid, {body, sizeof(body)}).ok() &&
            target[0] == 0xa5 && body[0] == 0xa5,
            "thread creation: empty blank controls and malformed Unicode refuse atomically");
    }
    bool invalid_requests = true;
    auto invalid = request; invalid.name = {overlong, 101}; invalid_requests &= !thread_creation_request_valid(invalid);
    invalid.name = {overlong, 401}; invalid_requests &= !thread_creation_request_valid(invalid);
    for (u32 at = 0; at < 4; ++at) { invalid = request; invalid.reserved[at] = 1; invalid_requests &= !thread_creation_request_valid(invalid); }
    invalid = request; invalid.parent_id = 0; invalid_requests &= !thread_creation_request_valid(invalid);
    invalid = request; invalid.guild_id = 0; invalid_requests &= !thread_creation_request_valid(invalid);
    invalid = request; invalid.source_message_id = 0; invalid_requests &= !thread_creation_request_valid(invalid);
    invalid = request; invalid.source_message_id = request.parent_id; invalid_requests &= !thread_creation_request_valid(invalid);
    invalid = request; invalid.thread_type = 12; invalid_requests &= !thread_creation_request_valid(invalid);
    invalid = request; invalid.operation = static_cast<ThreadCreationOperation>(255); invalid_requests &= !thread_creation_request_valid(invalid);
    invalid = request; invalid.auto_archive_duration = 0; invalid_requests &= !thread_creation_request_valid(invalid);
    invalid.auto_archive_duration = 61; invalid_requests &= !thread_creation_request_valid(invalid);
    check(invalid_requests, "thread creation: noncanonical IDs type operation duration reserved and name bounds refuse");
    constexpr u16 durations[]{60, 1440, 4320, 10080};
    for (u16 duration : durations) { auto valid = request; valid.auto_archive_duration = duration;
        check(thread_creation_request_valid(valid), "thread creation: every documented explicit duration admitted"); }

    u32 scratch[4096];
    ThreadCreationSnapshot snapshot{};
    auto decode = [&](Utf8View input, const ThreadCreationRequest* query = nullptr) noexcept {
        return thread_creation_decode(input, query ? *query : request, 30, {scratch, 4096}, &snapshot);
    };
    check(decode(receipt).ok() && snapshot.id == 20 && snapshot.parent_id == 10 && snapshot.guild_id == 40 &&
        snapshot.type == 11 && snapshot.last_message_id == 22 && snapshot.fields == 9 &&
        snapshot.auto_archive_duration == 4320 && snapshot.archive_timestamp_ms && snapshot.self_join_timestamp_ms &&
        snapshot.self_flags == 4 && utf8_equal(thread_creation_name_view(snapshot), utf8_literal("A\xC3\xA9")),
        "thread creation: receipt requires exact source identity complete thread metadata and positive self proof");
    auto owned = snapshot;
    check(decode(receipt, &reconcile).ok() && bytes_equal(&owned, &snapshot, sizeof(snapshot)),
        "thread creation: POST and reconciliation share the same strict owned channel receipt");
    const auto member_null = replace(receipt, utf8_literal(R"("member":{"id":"20","user_id":"30","join_timestamp":"2026-10-05T12:34:56Z","flags":4})"),
        utf8_literal(R"("member":null)"));
    check(decode(member_null).ok() && !(snapshot.fields & thread_creation_self_joined) &&
        !snapshot.self_flags && !snapshot.self_join_timestamp_ms,
        "thread creation: null optional member is unknown and never a negative membership observation");
    const auto member_absent = replace(receipt,
        utf8_literal(R"(,"member":{"id":"20","user_id":"30","join_timestamp":"2026-10-05T12:34:56Z","flags":4})"), {});
    check(decode(member_absent).ok() && !(snapshot.fields & thread_creation_self_joined),
        "thread creation: absent optional member publishes no membership removal");
    const auto last_null = replace(receipt, utf8_literal(R"("last_message_id":"22")"), utf8_literal(R"("last_message_id":null)"));
    check(decode(last_null).ok() && !snapshot.last_message_id && (snapshot.fields & thread_creation_last_message_known),
        "thread creation: null last message is explicit known absence");
    const auto member_implicit = replace(receipt, utf8_literal(R"("id":"20","user_id":"30","join_timestamp")"), utf8_literal(R"("join_timestamp")"));
    check(decode(member_implicit).ok() && (snapshot.fields & thread_creation_self_joined),
        "thread creation: omitted embedded self IDs bind only to the captured actor and thread");
    const auto archive_reconcile = replace(receipt, utf8_literal(R"("archived":false,"locked":false)"), utf8_literal(R"("archived":true,"locked":true)"));
    check(decode(archive_reconcile, &reconcile).ok() && snapshot.fields == 15,
        "thread creation: reconciliation can return a subsequently archived locked thread without inventing active authority");
    const auto announcement_receipt = replace(receipt, utf8_literal(R"("type":11)"), utf8_literal(R"("type":10)"));
    check(decode(announcement_receipt, &announcement).ok() && snapshot.type == 10,
        "thread creation: strict announcement receipt matches captured derived type");
    struct Change { Utf8View needle, replacement; };
    constexpr Change invalid_receipts[] {
        {utf8_literal(R"("id":"20")"), utf8_literal(R"("id":"21")")},
        {utf8_literal(R"("parent_id":"10")"), utf8_literal(R"("parent_id":"11")")},
        {utf8_literal(R"("guild_id":"40")"), utf8_literal(R"("guild_id":"41")")},
        {utf8_literal(R"("type":11)"), utf8_literal(R"("type":10)")},
        {utf8_literal(R"("type":11)"), utf8_literal(R"("type":12)")},
        {utf8_literal(R"("id":"20",)"), {}},
        {utf8_literal(R"("guild_id":"40",)"), {}},
        {utf8_literal(R"("parent_id":"10",)"), {}},
        {utf8_literal(R"("type":11,)"), {}},
        {utf8_literal(R"("name":"A\u00e9")"), utf8_literal(R"("name":"\u0000")")},
        {utf8_literal(R"("name":"A\u00e9")"), utf8_literal(R"("name":"   ")")},
        {utf8_literal(R"("name":"A\u00e9")"), utf8_literal(R"("name":"\ud800")")},
        {utf8_literal(R"("id":"20")"), utf8_literal(R"("id":"20","\u0069d":"20")")},
        {utf8_literal(R"("archived":false)"), utf8_literal(R"("archived":false,"archived":false)")},
        {utf8_literal(R"("locked":false,)"), {}},
        {utf8_literal(R"("archived":false)"), utf8_literal(R"("archived":0)")},
        {utf8_literal(R"("auto_archive_duration":4320)"), utf8_literal(R"("auto_archive_duration":61)")},
        {utf8_literal(R"("auto_archive_duration":4320,)"), {}},
        {utf8_literal(R"("archive_timestamp":"2026-10-05T12:34:56.123456+00:00")"), utf8_literal(R"("archive_timestamp":null)")},
        {utf8_literal(R"("archive_timestamp":"2026-10-05T12:34:56.123456+00:00")"), utf8_literal(R"("archive_timestamp":"bad")")},
        {utf8_literal(R"("user_id":"30")"), utf8_literal(R"("user_id":"31")")},
        {utf8_literal(R"("flags":4)"), utf8_literal(R"("flags":4294967296)")},
        {utf8_literal(R"("join_timestamp":"2026-10-05T12:34:56Z",)"), {}},
        {utf8_literal(R"("flags":4)"), utf8_literal(R"("flags":4,"flags":4)")},
        {utf8_literal(R"("ignored":[1,true,null])"), utf8_literal(R"("ignored":[1,true,])")},
    };
    for (const auto& change : invalid_receipts) {
        snapshot = owned;
        const auto input = replace(receipt, change.needle, change.replacement);
        check(input.bytes && !decode(input).ok() && bytes_equal(&owned, &snapshot, sizeof(snapshot)),
            "thread creation: foreign missing duplicate malformed and ignored-invalid receipts preserve whole output");
    }
    snapshot = owned;
    check(thread_creation_decode(receipt, request, 0, {scratch, 4096}, &snapshot).error == ThreadCreationError::input &&
        thread_creation_decode(receipt, request, 30, {scratch, 1}, &snapshot).error == ThreadCreationError::capacity &&
        bytes_equal(&owned, &snapshot, sizeof(snapshot)), "thread creation: actor and structural capacity failure are transactional");
    check(thread_creation_decode({reinterpret_cast<const char*>(&snapshot), sizeof(snapshot)}, request, 30,
        {scratch, 4096}, &snapshot).error == ThreadCreationError::input &&
        thread_creation_decode(receipt, request, 30,
            {reinterpret_cast<u32*>(&snapshot), sizeof(snapshot) / sizeof(u32)}, &snapshot).error == ThreadCreationError::input &&
        bytes_equal(&owned, &snapshot, sizeof(snapshot)), "thread creation: aliased input scratch and output cannot publish");
    char mutable_receipt[4096];
    copy_bytes(mutable_receipt, receipt.data, receipt.bytes);
    check(decode({mutable_receipt, receipt.bytes}).ok(), "thread creation: owned receipt fixture decodes");
    clear_bytes(mutable_receipt, receipt.bytes);
    check(utf8_equal(thread_creation_name_view(snapshot), utf8_literal("A\xC3\xA9")),
        "thread creation: decoded name remains owned after response storage is erased");
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_thread_creation_protocol_test_entry() noexcept {
    maicord::thread_creation_test::run();
    ExitProcess(maicord::thread_creation_test::failures ? 1 : 0);
}
