#include "maicord/protocol/forum_post.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "test_output.hpp"

namespace maicord::forum_post_test {
u32 failures;
u32 scratch[16384];
char changed[262145], generated[262145];
u32 generated_bytes;
void check(bool ok, const char* label) noexcept {
    test_check(&failures, ok, label);
}
Utf8View replace(Utf8View input, Utf8View needle, Utf8View replacement) noexcept {
    for (u32 at = 0; at + needle.bytes <= input.bytes; ++at) {
        if (!bytes_equal(input.data + at, needle.data, needle.bytes))
            continue;
        const u32 bytes = input.bytes - needle.bytes + replacement.bytes;
        if (bytes > sizeof(changed))
            return {};
        copy_bytes(changed, input.data, at);
        copy_bytes(changed + at, replacement.data, replacement.bytes);
        copy_bytes(changed + at + replacement.bytes, input.data + at + needle.bytes,
                   input.bytes - at - needle.bytes);
        return {changed, bytes};
    }
    check(false, "forum post: fixture replacement anchor must exist");
    return {};
}
void append(const char* text) noexcept {
    for (u32 at = 0; text[at]; ++at)
        generated[generated_bytes++] = text[at];
}
void number(u64 value) noexcept {
    char digits[20];
    u32 count{};
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        generated[generated_bytes++] = digits[--count];
}
Utf8View inventory(u32 count) noexcept {
    generated_bytes = 0;
    append("{\"id\":\"10\",\"guild_id\":\"40\",\"type\":15,\"name\":\"Forum\",\"flags\":16,\"available_"
           "tags\":[");
    for (u32 at = 0; at < count; ++at) {
        if (at)
            append(",");
        append("{\"id\":\"");
        number(100 + at);
        append("\",\"name\":\"Tag\",\"moderated\":false}");
    }
    append("]}");
    return {generated, generated_bytes};
}
constexpr auto parent_json = utf8_literal(
    R"({"id":"10","guild_id":"40","type":15,"name":"Forum \u65e5","flags":16,"default_auto_archive_duration":1440,"available_tags":[{"id":"100","name":"Help","moderated":false,"emoji_id":null,"emoji_name":"\ud83d\ude80"},{"id":"101","name":"Staff","moderated":true,"emoji_id":"102","emoji_name":null}],"future":{"valid":[1,true,null]}})");
constexpr auto post_json = utf8_literal(
    R"({"id":"20","guild_id":"40","parent_id":"10","owner_id":"30","type":11,"name":"Server title","applied_tags":["100"],"thread_metadata":{"archived":false,"locked":false,"auto_archive_duration":1440,"archive_timestamp":"2026-10-05T12:00:00Z"},"message":{"id":"21","channel_id":"20","guild_id":"40","author":{"id":"30","username":"Self","bot":false},"content":"Server\ntext \u65e5","timestamp":"2026-10-05T12:00:00Z","flags":0,"type":0,"embeds":[]},"future":{"valid":[1,true,null]}})");
constexpr auto media_json = utf8_literal(
    R"({"id":"20","guild_id":"40","parent_id":"10","owner_id":"30","type":11,"name":"Server title","applied_tags":["100"],"thread_metadata":{"archived":false,"locked":false,"auto_archive_duration":1440,"archive_timestamp":"2026-10-05T12:00:00Z"},"message":{"id":"21","channel_id":"20","guild_id":"40","author":{"id":"30","bot":false},"content":"","timestamp":"2026-10-05T12:00:00Z","flags":0,"type":0,"attachments":[{"id":"51","filename":"image.png","title":"Image","description":"Photo \u65e5","content_type":"image/png","size":13,"url":"https://cdn.discordapp.com/attachments/20/51/image.png?ex=1","proxy_url":"https://media.discordapp.net/attachments/20/51/image.png?ex=1","width":32,"height":16,"flags":8,"ephemeral":false},{"id":"52","filename":"file.pdf","size":14,"url":"https://cdn.discordapp.com/attachments/20/52/file.pdf","width":null,"height":null}],"embeds":[]}})");
Utf8View attachment_inventory(u32 count, bool large_strings = false,
                              bool escaped_descriptions = false) noexcept {
    generated_bytes = 0;
    append(
        R"({"id":"20","guild_id":"40","parent_id":"10","owner_id":"30","type":11,"name":"Server title","applied_tags":["100"],"thread_metadata":{"archived":false,"locked":false,"auto_archive_duration":1440,"archive_timestamp":"2026-10-05T12:00:00Z"},"message":{"id":"21","channel_id":"20","author":{"id":"30","bot":false},"content":"","timestamp":"2026-10-05T12:00:00Z","flags":0,"type":0,"attachments":[)");
    for (u32 at = 0; at < count; ++at) {
        if (at)
            append(",");
        append("{\"id\":\"");
        number(50 + at);
        append("\",\"filename\":\"file.png\",\"size\":1,\"url\":\"https://cdn.discordapp.com/");
        if (large_strings)
            for (u32 n = 27; n < 4096; ++n)
                generated[generated_bytes++] = 'a';
        append("\"");
        if (large_strings || escaped_descriptions) {
            append(",\"description\":\"");
            for (u32 n = 0; n < 1024; ++n)
                if (escaped_descriptions)
                    append("\\ud83d\\ude00");
                else
                    append("\xF0\x9F\x98\x80");
            append("\"");
        }
        append("}");
    }
    append("]}}");
    return {generated, generated_bytes};
}
void run_media() noexcept {
    UploadedAttachment files[10]{};
    files[0] = {utf8_literal("image.png"), utf8_literal("upload/image.png"),
                utf8_literal("Photo \xE6\x97\xA5"), 0, true};
    files[1] = {utf8_literal("file.pdf"), utf8_literal("upload/file.pdf"), {}, 1, false};
    for (u32 at = 2; at < 10; ++at)
        files[at] = {files[0].filename, files[0].uploaded_filename, {}, at, false};
    ForumPostRequest create{10,   40, utf8_literal("Files"),      {}, {100},
                            1440, 1,  ForumPostOperation::create, {}};
    create.attachments = {files, 2};
    auto* body = static_cast<u8*>(VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* banks = static_cast<ForumPostMediaSnapshot*>(VirtualAlloc(
        nullptr, sizeof(ForumPostMediaSnapshot) * 3, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(body && banks, "forum media: demand-backed fixture banks available");
    if (!body || !banks) {
        if (body)
            VirtualFree(body, 0, MEM_RELEASE);
        if (banks)
            VirtualFree(banks, 0, MEM_RELEASE);
        return;
    }
    auto& temporary = banks[0];
    auto& output = banks[1];
    auto& saved = banks[2];
    const auto measured = forum_post_body_bytes(create);
    const auto written = forum_post_write_body(create, {body, 65536});
    check(
        measured.ok() && written.ok() && measured.bytes == written.bytes &&
            utf8_equal(
                {reinterpret_cast<const char*>(body), written.bytes},
                utf8_literal(
                    R"({"name":"Files","auto_archive_duration":1440,"applied_tags":["100"],"message":{"content":"","attachments":[{"id":"0","filename":"image.png","uploaded_filename":"upload/image.png","description":"Photo 日","is_spoiler":true},{"id":"1","filename":"file.pdf","uploaded_filename":"upload/file.pdf"}]}})")),
        "forum media: file-only nested references are exact including description and spoiler");
    body[0] = 0xa5;
    check(forum_post_write_body(create, {body, measured.bytes - 1}).error == ForumPostError::capacity &&
              body[0] == 0xa5,
          "forum media: exact one-byte-short capacity preserves complete output");
    auto invalid = create;
    invalid.attachments.count = 11;
    check(!forum_post_request_valid(invalid), "forum media: eleventh uploaded reference rejected");
    invalid = create;
    files[1].id = 0;
    check(!forum_post_request_valid(invalid), "forum media: duplicate placeholder IDs rejected");
    files[1].id = 1;
    invalid = create;
    invalid.attachments = {};
    check(!forum_post_request_valid(invalid),
          "forum media: empty contribution still rejected without files");
    invalid = create;
    invalid.upload_bytes = 1;
    check(!forum_post_request_valid(invalid),
          "forum media: create rejects foreign upload operation fields");
    invalid = create;
    invalid.content = {reinterpret_cast<const char*>(body), 1};
    body[0] = 'x';
    check(forum_post_write_body(invalid, {body, 65536}).error == ForumPostError::input && body[0] == 'x',
          "forum media: input/output alias refuses before writing");
    ForumPostRequest upload{10, 40, {}, {}, {}, 0, 0, ForumPostOperation::attachment_url, {}};
    upload.upload_filename = utf8_literal("A\".png");
    upload.upload_bytes = ~u64{0};
    upload.upload_id = ~u32{0};
    u8 target[96], legacy_target[96], legacy_body[2304];
    const auto legacy =
        attachment_upload_write(10, upload.upload_id, upload.upload_filename, upload.upload_bytes,
                                {legacy_target, sizeof(legacy_target)}, {legacy_body, sizeof(legacy_body)});
    const auto upload_target = forum_post_write_target(upload, {target, sizeof(target)});
    const auto upload_body = forum_post_write_body(upload, {body, 65536});
    check(legacy.ok && upload_target.ok() && upload_body.ok() && upload_body.bytes == legacy.body_bytes &&
              upload_target.bytes == legacy.target_bytes &&
              bytes_equal(target, legacy_target, upload_target.bytes) &&
              bytes_equal(body, legacy_body, upload_body.bytes) &&
              forum_post_body_bytes(upload).bytes == upload_body.bytes,
          "forum media: typed attachment URL delegates exact existing v9 metadata route/body");
    invalid = upload;
    invalid.upload_reserved = 1;
    check(!forum_post_request_valid(invalid), "forum media: upload reserved field rejected");
    invalid = upload;
    invalid.attachments = {files, 1};
    check(!forum_post_request_valid(invalid), "forum media: upload rejects create references");
    invalid = upload;
    invalid.content = utf8_literal("wrong");
    check(!forum_post_request_valid(invalid), "forum media: upload rejects draft payload");
    invalid = upload;
    invalid.upload_bytes = 0;
    check(!forum_post_request_valid(invalid), "forum media: empty upload rejected");
    invalid = upload;
    invalid.operation = ForumPostOperation::parent_metadata;
    check(!forum_post_request_valid(invalid), "forum media: GET rejects upload operation payload");
    char quotes[4096];
    for (auto& c : quotes)
        c = '"';
    UploadedAttachment maximum[10];
    for (u32 at = 0; at < 10; ++at)
        maximum[at] = {{quotes, 1024}, {quotes, 4096}, {quotes, 1024}, at, false};
    invalid = create;
    invalid.attachments = {maximum, 10};
    const auto oversized = forum_post_body_bytes(invalid);
    body[0] = 0xa5;
    check(forum_post_request_valid(invalid) && oversized.error == ForumPostError::capacity &&
              oversized.bytes > 65536 &&
              forum_post_write_body(invalid, {body, 65536}).error == ForumPostError::capacity &&
              body[0] == 0xa5,
          "forum media: valid individual maxima exceeding body budget produce visible atomic capacity "
          "failure");
    for (u32 at = 0; at < 10; ++at)
        maximum[at] = {
            utf8_literal("a.png"), at < 7 ? Utf8View{quotes, 4096} : utf8_literal("u"), {}, at, false};
    maximum[7].uploaded_filename = {quotes, 1};
    invalid = create;
    invalid.attachments = {maximum, 10};
    auto base = forum_post_body_bytes(invalid);
    if ((65536 - base.bytes) & 1u) {
        invalid.content = utf8_literal("x");
        base = forum_post_body_bytes(invalid);
    }
    const u32 exact_length = 1 + (65536 - base.bytes) / 2;
    check(base.ok() && base.bytes < 65536 && exact_length <= 4096,
          "forum media: exact body-capacity fixture is bounded");
    maximum[7].uploaded_filename = {quotes, exact_length};
    const auto exact = forum_post_body_bytes(invalid);
    check(exact.ok() && exact.bytes == 65536 && forum_post_write_body(invalid, {body, 65536}).ok(),
          "forum media: exact 64KiB encoded body is admitted without spare padding");
    body[0] = 0xa5;
    check(forum_post_write_body(invalid, {body, 65535}).error == ForumPostError::capacity &&
              body[0] == 0xa5,
          "forum media: one byte below exact 64KiB body rejects atomically");
    auto decode = [&](Utf8View text) noexcept {
        return forum_post_media_decode(text, create, 30, {scratch, 16384}, &temporary, &output);
    };
    check(decode(media_json).ok() && output.post.message_id == 21 && !output.post.content_bytes &&
              output.attachment_count == 2,
          "forum media: atomic file-only thread and starter receipt with complete attachments");
    const auto image = forum_post_attachment_view(output, 0);
    const auto file = forum_post_attachment_view(output, 1);
    check(
        image.id == 51 && image.size == 13 && image.width == 32 && image.height == 16 && image.flags == 8 &&
            image.has_width && image.has_height && utf8_equal(image.filename, utf8_literal("image.png")) &&
            utf8_equal(image.description, utf8_literal("Photo \xE6\x97\xA5")) &&
            utf8_equal(image.content_type, utf8_literal("image/png")) && file.id == 52 && !file.has_width &&
            !file.has_height && file.url.bytes && !forum_post_attachment_view(output, 2).id,
        "forum media: final attachment IDs are distinct from placeholders and owning image/file fields "
        "retained");
    char name_json[1026];
    name_json[0] = name_json[1025] = '"';
    for (u32 at = 1; at <= 1024; ++at)
        name_json[at] = 'n';
    check(decode(replace(media_json, utf8_literal("\"image.png\""), {name_json, sizeof(name_json)})).ok() &&
              forum_post_attachment_view(output, 0).filename.bytes == 1024,
          "forum media: valid uploaded filename beyond generic 512-byte rich limit is retained fully");
    copy_bytes(&saved, &output, sizeof(saved));
    struct Change {
        Utf8View needle, value;
    };
    constexpr Change bad[]{
        {utf8_literal("\"id\":\"52\""), utf8_literal("\"id\":\"51\"")},
        {utf8_literal("\"size\":13"), utf8_literal("\"size\":0")},
        {utf8_literal("\"size\":13"), utf8_literal("\"size\":13,\"s\\u0069ze\":13")},
        {utf8_literal("\"filename\":\"image.png\""), utf8_literal("\"filename\":\"\"")},
        {utf8_literal("\"filename\":\"image.png\""), utf8_literal("\"filename\":\"\\ud800\"")},
        {utf8_literal("https://cdn.discordapp.com/attachments/20/51/image.png?ex=1"),
         utf8_literal("javascript:bad")},
        {utf8_literal("\"width\":32"), utf8_literal("\"width\":4294967296")},
        {utf8_literal("\"ephemeral\":false"), utf8_literal("\"ephemeral\":0")},
        {utf8_literal("\"attachments\":["), utf8_literal("\"attachments\":null,\"ignored\":[")},
        {utf8_literal("\"attachments\":["), utf8_literal("\"attachments\":[],\"attachments\":[")},
    };
    ForumPostSnapshot core{};
    core.message_id = 777;
    for (const auto& change : bad) {
        const auto text = replace(media_json, change.needle, change.value);
        check(!decode(text).ok() && bytes_equal(&output, &saved, sizeof(saved)) &&
                  !forum_post_decode(text, create, 30, {scratch, 16384}, &core).ok() &&
                  core.message_id == 777,
              "forum media: malformed/duplicate/foreign attachment contract preserves core and rich "
              "receipts");
    }
    check(!decode(post_json).ok() && bytes_equal(&output, &saved, sizeof(saved)),
          "forum media: file request requires actual response attachment evidence");
    auto one = create;
    one.attachments.count = 1;
    check(!forum_post_media_decode(media_json, one, 30, {scratch, 16384}, &temporary, &output).ok() &&
              bytes_equal(&output, &saved, sizeof(saved)),
          "forum media: returned attachment count must equal captured request");
    create.attachments = {files, 10};
    check(decode(attachment_inventory(10)).ok() && output.attachment_count == 10,
          "forum media: complete ten-attachment receipt admitted without truncation");
    const auto wide_envelope = attachment_inventory(10, false, true);
    check(wide_envelope.bytes > kThreadCreationMaximumResponseBytes &&
              wide_envelope.bytes <= kForumPostMaximumResponseBytes && decode(wide_envelope).ok() &&
              output.attachment_count == 10 &&
              output.attachment_text_bytes < kForumPostAttachmentTextBytes &&
              forum_post_attachment_view(output, 9).description.bytes == 4096,
          "forum media: valid escaped rich receipt above64KiB fits forum envelope and owning packed bank");
    ThreadCreationRequest channel_intent{
        10, 40, 20, utf8_literal("Files"), 1440, 11, ThreadCreationOperation::reconcile, {}};
    ThreadCreationSnapshot channel_receipt{};
    check(thread_creation_decode(wide_envelope, channel_intent, 30, {scratch, 16384}, &channel_receipt)
                      .error == ThreadCreationError::input &&
              thread_creation_decode(wide_envelope, channel_intent, 30, {scratch, 16384}, &channel_receipt,
                                     ThreadCreationEnvelope::forum_post)
                  .ok() &&
              channel_receipt.id == 20 &&
              thread_creation_decode(wide_envelope, channel_intent, 30, {scratch, 16384}, &channel_receipt,
                                     static_cast<ThreadCreationEnvelope>(255))
                      .error == ThreadCreationError::input,
          "forum media: finite wider envelope preserves legacy channel cap and rejects unknown scope");
    copy_bytes(&saved, &output, sizeof(saved));
    check(!decode(attachment_inventory(11)).ok() && bytes_equal(&output, &saved, sizeof(saved)),
          "forum media: eleventh response attachment rejects whole receipt");
    create.attachments.count = 2;
    check(decode(attachment_inventory(2, true)).ok(),
          "forum media: maximum URL and supplementary description independently fit strict fields");
    copy_bytes(&saved, &output, sizeof(saved));
    create.attachments.count = 10;
    const auto crowded = attachment_inventory(10, true);
    check(decode(crowded).error == ForumPostError::capacity &&
              bytes_equal(&output, &saved, sizeof(saved)) &&
              forum_post_decode(crowded, create, 30, {scratch, 16384}, &core).error ==
                  ForumPostError::capacity &&
              core.message_id == 777,
          "forum media: packed string capacity fails atomically in both decoder contracts");
    create.attachments.count = 2;
    check(forum_post_media_decode(media_json, create, 30, {scratch, 16384}, &output, &output).error ==
                  ForumPostError::input &&
              forum_post_media_decode(media_json, create, 30, {reinterpret_cast<u32*>(&temporary), 1},
                                      &temporary, &output)
                      .error == ForumPostError::input &&
              forum_post_media_decode(media_json, create, 30, {scratch, ~u32{0}}, &temporary, &output)
                      .error == ForumPostError::input &&
              bytes_equal(&output, &saved, sizeof(saved)),
          "forum media: scratch/output aliases and structural overflow reject before mutation");
    copy_bytes(generated, media_json.data, media_json.bytes);
    check(decode({generated, media_json.bytes}).ok(),
          "forum media: independent owning receipt fixture decoded");
    clear_bytes(generated, media_json.bytes);
    clear_bytes(&temporary, sizeof(temporary));
    check(utf8_equal(forum_post_attachment_view(output, 0).filename, utf8_literal("image.png")) &&
              forum_post_attachment_view(output, 1).url.bytes,
          "forum media: receipt survives erased response and candidate banks");
    ForumParentSnapshot parent{};
    ForumPostRequest get{10, 40, {}, {}, {}, 0, 0, ForumPostOperation::parent_metadata, {}};
    const auto media_parent =
        replace(parent_json, utf8_literal("\"type\":15"), utf8_literal("\"type\":16"));
    const u64 tag[]{100};
    check(forum_parent_decode(media_parent, get, {scratch, 16384}, &parent).ok() && parent.type == 16 &&
              forum_post_selection_valid(parent, {tag, 1}, false),
          "forum media: type16 fresh metadata uses existing tag authority");
    SecureZeroMemory(body, 65536);
    VirtualFree(body, 0, MEM_RELEASE);
    SecureZeroMemory(banks, sizeof(ForumPostMediaSnapshot) * 3);
    VirtualFree(banks, 0, MEM_RELEASE);
}
void run() noexcept {
    run_media();
    ForumPostRequest get{10, 40, {}, {}, {}, 0, 0, ForumPostOperation::parent_metadata, {}};
    ForumPostRequest create{10,
                            40,
                            utf8_literal("A\"\\\xC3\xA9"),
                            utf8_literal("Line 1\nLine 2\t\xE6\x97\xA5"),
                            {100},
                            1440,
                            1,
                            ForumPostOperation::create,
                            {}};
    u8 target[96], body[16384];
    auto written = forum_post_write_target(get, {target, sizeof(target)});
    body[0] = 0xa5;
    check(written.ok() &&
              utf8_equal({reinterpret_cast<const char*>(target), written.bytes},
                         utf8_literal("/api/v10/channels/10")) &&
              forum_post_write_body(get, {body, sizeof(body)}).ok() && body[0] == 0xa5,
          "forum post: canonical metadata GET has exact target and no body");
    written = forum_post_write_target(create, {target, sizeof(target)});
    check(written.ok() && utf8_equal({reinterpret_cast<const char*>(target), written.bytes},
                                     utf8_literal("/api/v10/channels/10/threads?use_nested_fields=true")),
          "forum post: exact observed atomic POST target");
    written = forum_post_write_body(create, {body, sizeof(body)});
    check(written.ok() &&
              utf8_equal(
                  {reinterpret_cast<const char*>(body), written.bytes},
                  utf8_literal(
                      "{\"name\":\"A\\\"\\\\\xC3\xA9\",\"auto_archive_duration\":1440,\"applied_tags\":["
                      "\"100\"],\"message\":{\"content\":\"Line 1\\nLine 2\\t\xE6\x97\xA5\"}}")),
          "forum post: exact escaped title nested content and tags; no fictitious nonce");
    auto full_ids = create;
    full_ids.parent_id = ~u64{0};
    full_ids.guild_id = ~u64{0} - 1;
    for (u32 at = 0; at < 5; ++at)
        full_ids.applied_tag_ids[at] = ~u64{0} - 2 - at;
    full_ids.tag_count = 5;
    written = forum_post_write_target(full_ids, {target, sizeof(target)});
    check(written.ok() &&
              utf8_equal(
                  {reinterpret_cast<const char*>(target), written.bytes},
                  utf8_literal("/api/v10/channels/18446744073709551615/threads?use_nested_fields=true")) &&
              forum_post_write_body(full_ids, {body, sizeof(body)}).ok(),
          "forum post: full-width parent/tag IDs serialize exactly");
    target[0] = body[0] = 0xa5;
    check(forum_post_write_target(create, {target, 1}).error == ForumPostError::capacity &&
              forum_post_write_body(create, {body, 1}).error == ForumPostError::capacity &&
              target[0] == 0xa5 && body[0] == 0xa5,
          "forum post: writer capacity failure preserves destinations");
    char supplementary[8004];
    for (u32 at = 0; at < sizeof(supplementary); at += 4) {
        supplementary[at] = '\xf0';
        supplementary[at + 1] = '\x9f';
        supplementary[at + 2] = '\x98';
        supplementary[at + 3] = '\x80';
    }
    auto boundary = full_ids;
    boundary.title = {supplementary, 400};
    boundary.content = {supplementary, 8000};
    written = forum_post_write_body(boundary, {body, sizeof(body)});
    check(forum_post_request_valid(boundary) && written.ok() && written.bytes > 8400 &&
              written.bytes <= 16384,
          "forum post: exact 100/2000 supplementary scalar and 400/8000 byte boundaries fit bounded body "
          "with five tags");
    boundary.title = {supplementary, 404};
    check(!forum_post_request_valid(boundary), "forum post: 101 title scalars rejected");
    boundary.title = create.title;
    boundary.content = {supplementary, 8004};
    check(!forum_post_request_valid(boundary), "forum post: 2001 content scalars rejected");
    char ascii[2001];
    for (auto& byte : ascii)
        byte = 'x';
    boundary.content = {ascii, sizeof(ascii)};
    check(!forum_post_request_valid(boundary),
          "forum post: scalar bounds apply independently of byte bounds");
    constexpr Utf8View invalid_text[]{{},
                                      utf8_literal(" \t\n"),
                                      utf8_literal("\xC2\xA0"),
                                      utf8_literal("a\0b"),
                                      utf8_literal("\xFF"),
                                      utf8_literal("\xC0\xAF"),
                                      utf8_literal("\xED\xA0\x80"),
                                      utf8_literal("\xF4\x90\x80\x80"),
                                      utf8_literal("a\xC2\x85")};
    for (auto text : invalid_text) {
        auto invalid = create;
        invalid.content = text;
        body[0] = 0xa5;
        check(!forum_post_request_valid(invalid) &&
                  !forum_post_write_body(invalid, {body, sizeof(body)}).ok() && body[0] == 0xa5,
              "forum post: blank/control/invalid UTF8 request refuses without mutation");
    }
    bool invalid_requests = true;
    auto invalid = create;
    invalid.title = utf8_literal("a\nb");
    invalid_requests &= !forum_post_request_valid(invalid);
    invalid.title = utf8_literal("a\xE2\x80\xA8");
    invalid_requests &= !forum_post_request_valid(invalid);
    invalid = create;
    invalid.tag_count = 6;
    invalid_requests &= !forum_post_request_valid(invalid);
    invalid = create;
    invalid.tag_count = 2;
    invalid.applied_tag_ids[1] = 100;
    invalid_requests &= !forum_post_request_valid(invalid);
    invalid = create;
    invalid.applied_tag_ids[0] = 0;
    invalid_requests &= !forum_post_request_valid(invalid);
    invalid = create;
    invalid.applied_tag_ids[4] = 105;
    invalid_requests &= !forum_post_request_valid(invalid);
    invalid = create;
    invalid.auto_archive_duration = 61;
    invalid_requests &= !forum_post_request_valid(invalid);
    invalid = create;
    invalid.operation = static_cast<ForumPostOperation>(255);
    invalid_requests &= !forum_post_request_valid(invalid);
    invalid = create;
    invalid.parent_id = invalid.guild_id;
    invalid_requests &= !forum_post_request_valid(invalid);
    for (u32 at = 0; at < 4; ++at) {
        invalid = create;
        invalid.reserved[at] = 1;
        invalid_requests &= !forum_post_request_valid(invalid);
    }
    invalid = get;
    invalid.title = {"", 0};
    invalid_requests &= !forum_post_request_valid(invalid);
    invalid = get;
    invalid.auto_archive_duration = 4320;
    invalid_requests &= !forum_post_request_valid(invalid);
    check(invalid_requests, "forum post: operation/reserved/zero/distinct/duplicate/unused/tag/duration "
                            "and GET canonical fields enforced");
    constexpr u16 durations[]{60, 1440, 4320, 10080};
    for (u16 duration : durations) {
        auto valid = create;
        valid.auto_archive_duration = duration;
        check(forum_post_request_valid(valid), "forum post: every documented archive duration admitted");
    }

    ForumParentSnapshot parent{};
    auto decode_parent = [&](Utf8View text) noexcept {
        return forum_parent_decode(text, get, {scratch, 16384}, &parent);
    };
    check(decode_parent(parent_json).ok() && parent.id == 10 && parent.guild_id == 40 &&
              parent.type == 15 && parent.flags == 16 && parent.fields == 7 &&
              parent.default_auto_archive_duration == 1440 && parent.tag_count == 2 &&
              parent.tags[0].id == 100 && !parent.tags[0].moderated && parent.tags[1].moderated &&
              utf8_equal(Utf8View{parent.name, parent.name_bytes}, utf8_literal("Forum \xE6\x97\xA5")) &&
              utf8_equal(forum_tag_name_view(parent.tags[0]), utf8_literal("Help")),
          "forum post: exact owning fresh forum metadata and moderated inventory");
    const auto owned_parent = parent;
    const u64 public_tag[]{100}, moderated_tag[]{101}, duplicate_tags[]{100, 100}, foreign_tags[]{102};
    check(!forum_post_selection_valid(parent, {}, false) &&
              forum_post_selection_valid(parent, {public_tag, 1}, false) &&
              !forum_post_selection_valid(parent, {moderated_tag, 1}, false) &&
              forum_post_selection_valid(parent, {moderated_tag, 1}, true) &&
              !forum_post_selection_valid(parent, {duplicate_tags, 2}, true) &&
              !forum_post_selection_valid(parent, {foreign_tags, 1}, true),
          "forum post: REQUIRE_TAG/moderation/unique/inventory authority selection is explicit");
    parent.flags = 0;
    check(forum_post_selection_valid(parent, {}, false),
          "forum post: optional-tag forum permits no selection");
    parent.flags = 1u << 17;
    check(!forum_post_selection_valid(parent, {public_tag, 1}, true),
          "forum post: obfuscated parent metadata cannot authorize selection");
    check(decode_parent(utf8_literal(R"({"id":"10","guild_id":"40","type":15,"name":"Forum"})")).ok() &&
              parent.fields == 0 && parent.default_auto_archive_duration == 4320 &&
              !forum_post_selection_valid(parent, {}, true),
          "forum post: absent flags/inventory stays unknown; missing archive default uses explicit "
          "observed fallback");
    check(decode_parent(
              utf8_literal(
                  R"({"id":"10","guild_id":"40","type":15,"name":"Forum","flags":0,"available_tags":[]})"))
                  .ok() &&
              parent.fields == 3 && !parent.tag_count && forum_post_selection_valid(parent, {}, false),
          "forum post: explicit empty inventory is complete");
    check(decode_parent(inventory(20)).ok() && parent.tag_count == 20 && parent.tags[19].id == 119,
          "forum post: complete twenty-tag page admitted without truncation");
    parent = owned_parent;
    check(!decode_parent(inventory(21)).ok() && bytes_equal(&parent, &owned_parent, sizeof(parent)),
          "forum post: twenty-first tag rejects whole metadata without partial publication");
    struct Replacement {
        Utf8View needle, replacement;
    };
    constexpr Replacement invalid_parents[]{
        {utf8_literal("\"id\":\"10\""), utf8_literal("\"id\":\"11\"")},
        {utf8_literal("\"guild_id\":\"40\""), utf8_literal("\"guild_id\":\"41\"")},
        {utf8_literal("\"type\":15"), utf8_literal("\"type\":0")},
        {utf8_literal("\"flags\":16"), utf8_literal("\"flags\":4294967296")},
        {utf8_literal("\"flags\":16"), utf8_literal("\"flags\":16,\"flags\":0")},
        {utf8_literal("\"default_auto_archive_duration\":1440"),
         utf8_literal("\"default_auto_archive_duration\":61")},
        {utf8_literal("\"id\":\"101\""), utf8_literal("\"id\":\"100\"")},
        {utf8_literal("\"id\":\"101\""), utf8_literal("\"id\":\"10\"")},
        {utf8_literal("\"moderated\":true"), utf8_literal("\"moderated\":0")},
        {utf8_literal("\"moderated\":true"), utf8_literal("\"moderated\":true,\"moderated\":false")},
        {utf8_literal("\"emoji_name\":null"), utf8_literal("\"emoji_name\":\"x\"")},
        {utf8_literal("\"name\":\"Help\""), utf8_literal("\"name\":\"123456789012345678901\"")},
        {utf8_literal("\"name\":\"Help\""), utf8_literal("\"name\":\"\\ud800\"")},
        {utf8_literal("\"valid\":[1,true,null]"), utf8_literal("\"valid\":[1,tru,null]")},
    };
    for (const auto& edit : invalid_parents) {
        check(!decode_parent(replace(parent_json, edit.needle, edit.replacement)).ok() &&
                  bytes_equal(&parent, &owned_parent, sizeof(parent)),
              "forum post: foreign/malformed/duplicate metadata, tag, emoji and ignored subtree rejects "
              "atomically");
    }
    check(decode_parent(
              replace(parent_json, utf8_literal("\"name\":\"Help\""), utf8_literal("\"name\":\"\"")))
                  .ok() &&
              !parent.tags[0].name_bytes,
          "forum post: documented zero-length tag label retained without invented text");
    parent = owned_parent;
    check(forum_parent_decode(parent_json, get, {scratch, 1}, &parent).error == ForumPostError::capacity &&
              forum_parent_decode(parent_json, get, {reinterpret_cast<u32*>(&parent), sizeof(parent) / 4},
                                  &parent)
                      .error == ForumPostError::input &&
              bytes_equal(&parent, &owned_parent, sizeof(parent)),
          "forum post: structural capacity and alias failure preserve metadata");
    check(!decode_parent(replace(parent_json, utf8_literal("\"guild_id\":\"40\","), {})).ok() &&
              !decode_parent(replace(parent_json, utf8_literal("\"name\":\"Forum \\u65e5\""),
                                     utf8_literal("\"name\":\"Forum\",\"n\\u0061me\":\"Forum\"")))
                   .ok() &&
              bytes_equal(&parent, &owned_parent, sizeof(parent)),
          "forum post: actual required guild and escaped duplicate field names are enforced");

    ForumPostSnapshot post{};
    auto decode_post = [&](Utf8View text, const ForumPostRequest* query = nullptr) noexcept {
        return forum_post_decode(text, query ? *query : create, 30, {scratch, 16384}, &post);
    };
    check(decode_post(post_json).ok() && post.thread.id == 20 && post.thread.parent_id == 10 &&
              post.thread.guild_id == 40 && post.owner_id == 30 && post.message_id == 21 &&
              post.message_channel_id == 20 && post.message_author_id == 30 && post.message_timestamp_ms &&
              !post.message_flags && post.applied_tag_count == 1 && post.applied_tag_ids[0] == 100 &&
              !(post.thread.fields & thread_creation_self_joined) &&
              utf8_equal(forum_post_content_view(post), utf8_literal("Server\ntext \xE6\x97\xA5")),
          "forum post: atomic typed channel and independent nested message ID/content; no invented self "
          "membership");
    const auto owned_post = post;
    constexpr Replacement invalid_posts[]{
        {utf8_literal("\"id\":\"20\""), utf8_literal("\"id\":\"10\"")},
        {utf8_literal("\"owner_id\":\"30\""), utf8_literal("\"owner_id\":\"31\"")},
        {utf8_literal("\"owner_id\":\"30\""), utf8_literal("\"owner_id\":\"30\",\"owner_id\":\"30\"")},
        {utf8_literal("\"parent_id\":\"10\""), utf8_literal("\"parent_id\":\"11\"")},
        {utf8_literal("\"type\":11"), utf8_literal("\"type\":12")},
        {utf8_literal("\"applied_tags\":[\"100\"]"), utf8_literal("\"applied_tags\":[\"101\"]")},
        {utf8_literal("\"applied_tags\":[\"100\"]"), utf8_literal("\"applied_tags\":[\"100\",\"100\"]")},
        {utf8_literal("\"channel_id\":\"20\""), utf8_literal("\"channel_id\":\"10\"")},
        {utf8_literal("\"author\":{\"id\":\"30\""), utf8_literal("\"author\":{\"id\":\"31\"")},
        {utf8_literal("\"bot\":false"), utf8_literal("\"bot\":true")},
        {utf8_literal("\"timestamp\":\"2026-10-05T12:00:00Z\""), utf8_literal("\"timestamp\":\"invalid\"")},
        {utf8_literal("\"flags\":0,\"type\":0"), utf8_literal("\"flags\":0,\"type\":6")},
        {utf8_literal("\"flags\":0,\"type\":0"), utf8_literal("\"flags\":0")},
        {utf8_literal("\"content\":\"Server\\ntext \\u65e5\""), utf8_literal("\"content\":null")},
        {utf8_literal("\"content\":\"Server\\ntext \\u65e5\""),
         utf8_literal("\"content\":\"ok\",\"content\":\"ok\"")},
        {utf8_literal("\"locked\":false"), utf8_literal("\"locked\":null")},
        {utf8_literal("\"valid\":[1,true,null]"), utf8_literal("\"valid\":[1,tru,null]")},
    };
    for (const auto& edit : invalid_posts) {
        check(!decode_post(replace(post_json, edit.needle, edit.replacement)).ok() &&
                  bytes_equal(&post, &owned_post, sizeof(post)),
              "forum post: foreign/duplicate/malformed channel or nested message leaves whole receipt "
              "unchanged");
    }
    check(!decode_post(replace(post_json, utf8_literal("\"channel_id\":\"20\",\"guild_id\":\"40\""),
                               utf8_literal("\"channel_id\":\"20\",\"guild_id\":\"41\"")))
                  .ok() &&
              bytes_equal(&post, &owned_post, sizeof(post)),
          "forum post: supplied nested message guild cannot differ from captured parent guild");
    check(
        !decode_post(replace(post_json, utf8_literal("\"channel_id\":\"20\",\"guild_id\":\"40\""),
                             utf8_literal("\"channel_id\":\"20\",\"guild_id\":\"40\",\"guild_id\":\"40\"")))
                .ok() &&
            bytes_equal(&post, &owned_post, sizeof(post)),
        "forum post: supplied nested guild duplicate is rejected despite production message field "
        "omission");
    check(decode_post(
              replace(post_json, utf8_literal("\"embeds\":[]"),
                      utf8_literal("\"embeds\":[{\"type\":\"rich\",\"title\":\"Server URL preview\"}]")))
              .ok(),
          "forum post: syntactically valid server preview is ignored without borrowed media");
    check(decode_post(replace(post_json, utf8_literal("\"content\":\"Server\\ntext \\u65e5\""),
                              utf8_literal("\"content\":\"\"")))
                  .ok() &&
              !post.content_bytes,
          "forum post: server-normalized empty content is a valid atomic receipt, not permission to send "
          "an empty draft");
    check(
        decode_post(replace(post_json, utf8_literal("\"future\":{"),
                            utf8_literal("\"member\":{\"id\":\"20\",\"user_id\":\"30\",\"join_timestamp\":"
                                         "\"2026-10-05T12:00:00Z\",\"flags\":0},\"future\":{")))
                .ok() &&
            (post.thread.fields & thread_creation_self_joined),
        "forum post: only actual embedded current-self member adds positive membership proof");
    post = owned_post;
    check(
        !decode_post(replace(post_json, utf8_literal("\"future\":{"),
                             utf8_literal("\"member\":{\"id\":\"20\",\"user_id\":\"31\",\"join_timestamp\":"
                                          "\"2026-10-05T12:00:00Z\",\"flags\":0},\"future\":{")))
                .ok() &&
            bytes_equal(&post, &owned_post, sizeof(post)),
        "forum post: foreign embedded member rejects receipt without losing prior owned output");
    check(forum_post_decode(post_json, create, 0, {scratch, 16384}, &post).error == ForumPostError::input &&
              forum_post_decode(post_json, create, 30, {scratch, 1}, &post).error ==
                  ForumPostError::capacity &&
              forum_post_decode(post_json, create, 30, {reinterpret_cast<u32*>(&post), sizeof(post) / 4},
                                &post)
                      .error == ForumPostError::input &&
              bytes_equal(&post, &owned_post, sizeof(post)),
          "forum post: captured self, capacity and overlapping output/scratch reject transactionally");
    check(forum_post_decode({reinterpret_cast<const char*>(&post), sizeof(post)}, create, 30,
                            {scratch, 16384}, &post)
                      .error == ForumPostError::input &&
              forum_post_decode(post_json, create, 30, {scratch, 0xffffffffu}, &post).error ==
                  ForumPostError::input &&
              bytes_equal(&post, &owned_post, sizeof(post)),
          "forum post: input/output overlap and structural byte overflow refuse before scanning");
    copy_bytes(scratch, post_json.data, post_json.bytes);
    check(forum_post_decode({reinterpret_cast<const char*>(scratch), post_json.bytes}, create, 30,
                            {scratch, 16384}, &post)
                      .error == ForumPostError::input &&
              bytes_equal(&post, &owned_post, sizeof(post)),
          "forum post: input and structural scratch may not overlap");
    check(
        !decode_post(replace(post_json, utf8_literal("\"message\":{"), utf8_literal("\"other\":{"))).ok() &&
            !decode_post(replace(post_json, utf8_literal("\"owner_id\":\"30\","), {})).ok() &&
            bytes_equal(&post, &owned_post, sizeof(post)),
        "forum post: atomic success requires actual owner and nested initial message");
    auto no_tags = create;
    no_tags.tag_count = 0;
    no_tags.applied_tag_ids[0] = 0;
    check(decode_post(replace(post_json, utf8_literal("\"applied_tags\":[\"100\"],"), {}), &no_tags).ok() &&
              !post.applied_tag_count,
          "forum post: omitted applied tags admitted only for immutable empty selection");
    post = owned_post;
    check(!decode_post(replace(post_json, utf8_literal("\"applied_tags\":[\"100\"],"), {})).ok() &&
              bytes_equal(&post, &owned_post, sizeof(post)),
          "forum post: selected tags require actual receipt evidence");
    auto two_tags = create;
    two_tags.tag_count = 2;
    two_tags.applied_tag_ids[1] = 101;
    check(decode_post(replace(post_json, utf8_literal("\"applied_tags\":[\"100\"]"),
                              utf8_literal("\"applied_tags\":[\"101\",\"100\"]")),
                      &two_tags)
              .ok(),
          "forum post: returned tag order may differ while exact selected set must match");
    copy_bytes(generated, post_json.data, post_json.bytes);
    check(decode_post({generated, post_json.bytes}).ok(), "forum post: owned receipt storage fixture");
    clear_bytes(generated, post_json.bytes);
    check(utf8_equal(forum_post_content_view(post), utf8_literal("Server\ntext \xE6\x97\xA5")) &&
              utf8_equal(thread_creation_name_view(post.thread), utf8_literal("Server title")),
          "forum post: title and starter content survive erased response bytes");
    for (auto& byte : generated)
        byte = ' ';
    check(!decode_post({generated, sizeof(generated)}).ok(),
          "forum post: response exceeds immutable 256KiB bound before parse");
}
} // namespace maicord::forum_post_test
extern "C" __declspec(noreturn) void WINAPI maicord_forum_post_protocol_test_entry() noexcept {
    maicord::forum_post_test::run();
    ExitProcess(maicord::forum_post_test::failures ? 1 : 0);
}
