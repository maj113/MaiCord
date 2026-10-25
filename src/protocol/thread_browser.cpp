#include "maicord/protocol/thread_browser.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/thread_actions.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {
namespace {
bool whitespace(u32 scalar) noexcept {
    return scalar <= 32 || scalar == 0x85 || scalar == 0xa0 || scalar == 0x1680 ||
        (scalar >= 0x2000 && scalar <= 0x200a) || scalar == 0x2028 || scalar == 0x2029 ||
        scalar == 0x202f || scalar == 0x205f || scalar == 0x3000;
}
bool valid_name(Utf8View name, bool nonblank) noexcept {
    if (!name.bytes || name.bytes > kThreadBrowserNameMaximumBytes ||
        !byte_range_valid({reinterpret_cast<const u8*>(name.data), name.bytes})) return false;
    const auto valid = utf8_validate(name);
    if (!valid.valid || !valid.codepoints || valid.codepoints > kThreadBrowserNameMaximumScalars)
        return false;
    bool content = false;
    for (u32 at = 0; at < name.bytes;) {
        const auto scalar = utf8_decode_one(name.data + at, name.bytes - at);
        if (!scalar.valid || scalar.codepoint < 32 ||
            (scalar.codepoint >= 127 && scalar.codepoint <= 159)) return false;
        content |= !whitespace(scalar.codepoint);
        at += scalar.width;
    }
    return !nonblank || content;
}
struct PageParse {
    JsonIndexedDocument document;
    ThreadBrowserRequest request;
    Slice<ThreadBrowserRow> rows;
    Slice<char> names;
    // Optional post metadata; null data is ignored.
    Slice<ThreadBrowserPost> posts;
    Slice<char> text;
    u64 self;
    u32 count{}, name_bytes{}, member_count{}, root_fields{}, text_bytes{};
    u64 member_ids[kThreadBrowserSearchMaximumRows]{};
    JsonValue threads{}, members{}, first_messages{};
    bool has_more{};
    ThreadBrowserError error{};
};
bool reject(PageParse& page, ThreadBrowserError error) noexcept {
    page.error = error;
    return false;
}
bool read_ok(PageParse& page, JsonReadResult read) noexcept {
    if (read.ok()) return true;
    if (page.error == ThreadBrowserError::none) page.error = ThreadBrowserError::json;
    return false;
}
bool root_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& page = *static_cast<PageParse*>(opaque);
    u32 bit{};
    if (json_string_equals_ascii(name, utf8_literal("threads"))) bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("members"))) bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("has_more"))) bit = 4;
    else if (json_string_equals_ascii(name, utf8_literal("first_messages"))) bit = 8;
    else if (json_string_equals_ascii(name, utf8_literal("most_recent_messages"))) bit = 16;
    else return true;
    if (page.root_fields & bit) return reject(page, ThreadBrowserError::duplicate);
    page.root_fields |= bit;
    if (bit == 4) return json_value_to_bool(value, &page.has_more) || reject(page, ThreadBrowserError::field);
    if ((bit == 2 || bit == 8 || bit == 16) && value.kind == JsonValueKind::null_value) return true;
    if (value.kind != JsonValueKind::array) return reject(page, ThreadBrowserError::field);
    if (bit == 1) page.threads = value;
    else if (bit == 2) page.members = value;
    else if (bit == 8) page.first_messages = value;
    // Preview arrays are syntactically validated by the indexed reader and
    // not retained in this metadata-only protocol.
    return true;
}
struct ThreadParse {
    PageParse* page;
    ThreadBrowserRow row{};
    ThreadBrowserPost post{};
    JsonString name{};
    JsonValue metadata{}, member{};
    u32 seen{};
};
bool applied_tag(void* opaque, u32 index, JsonValue value) noexcept {
    auto& thread = *static_cast<ThreadParse*>(opaque);
    u64 id{};
    if (index >= kThreadBrowserPostTagMaximum) return reject(*thread.page, ThreadBrowserError::capacity);
    if (!json_value_to_snowflake(value, &id)) return reject(*thread.page, ThreadBrowserError::field);
    for (u32 before = 0; before < index; ++before)
        if (thread.post.tags[before] == id) return reject(*thread.page, ThreadBrowserError::duplicate);
    thread.post.tags[index] = id;
    thread.post.tag_count = static_cast<u8>(index + 1);
    return true;
}
bool thread_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& thread = *static_cast<ThreadParse*>(opaque);
    auto& page = *thread.page;
    u32 bit{};
    if (json_string_equals_ascii(name, utf8_literal("id"))) bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("parent_id"))) bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("guild_id"))) bit = 4;
    else if (json_string_equals_ascii(name, utf8_literal("type"))) bit = 8;
    else if (json_string_equals_ascii(name, utf8_literal("name"))) bit = 16;
    else if (json_string_equals_ascii(name, utf8_literal("thread_metadata"))) bit = 32;
    else if (json_string_equals_ascii(name, utf8_literal("last_message_id"))) bit = 64;
    else if (json_string_equals_ascii(name, utf8_literal("member"))) bit = 128;
    else if (!page.posts.data) return true;
    else if (json_string_equals_ascii(name, utf8_literal("applied_tags"))) bit = 256;
    else if (json_string_equals_ascii(name, utf8_literal("message_count"))) bit = 512;
    else if (json_string_equals_ascii(name, utf8_literal("total_message_sent"))) bit = 1024;
    else if (json_string_equals_ascii(name, utf8_literal("owner_id"))) bit = 2048;
    else if (json_string_equals_ascii(name, utf8_literal("flags"))) bit = 4096;
    else return true;
    if (thread.seen & bit) return reject(page, ThreadBrowserError::duplicate);
    thread.seen |= bit;
    bool valid = false;
    if (bit == 1) valid = json_value_to_snowflake(value, &thread.row.id);
    else if (bit == 2) valid = json_value_to_snowflake(value, &thread.row.parent_id);
    else if (bit == 4) valid = json_value_to_snowflake(value, &thread.row.guild_id);
    else if (bit == 8) {
        u64 type{};
        valid = json_value_to_u64(value, &type) && (type == 10 || type == 11 || type == 12);
        thread.row.type = static_cast<u8>(type);
    } else if (bit == 16) valid = json_value_as_string(value, &thread.name);
    else if (bit == 256) {
        valid = value.kind == JsonValueKind::array &&
            read_ok(page, json_visit_indexed_array(page.document, value, 32, applied_tag, &thread));
        thread.post.fields |= thread_browser_post_tags;
        if (!valid) return page.error != ThreadBrowserError::none ? false
            : reject(page, ThreadBrowserError::field);
    } else if (bit == 512 || bit == 1024 || bit == 4096) {
        u64 number{};
        valid = json_value_to_u64(value, &number) && number <= 0xffffffffull;
        if (bit == 512) thread.post.message_count = static_cast<u32>(number);
        else if (bit == 1024) thread.post.total_message_sent = static_cast<u32>(number);
        else thread.post.flags = static_cast<u32>(number);
        thread.post.fields |= bit == 512 ? thread_browser_post_message_count
            : bit == 1024 ? thread_browser_post_total_sent : thread_browser_post_flags;
    } else if (bit == 2048) {
        valid = json_value_to_snowflake(value, &thread.post.owner_id);
        thread.post.fields |= thread_browser_post_owner;
    }
    else if (bit == 32 || bit == 128) {
        valid = value.kind == JsonValueKind::object;
        if (bit == 32) thread.metadata = value;
        else thread.member = value;
    } else {
        valid = value.kind == JsonValueKind::null_value ||
            json_value_to_snowflake(value, &thread.row.last_message_id);
        thread.row.fields |= thread_browser_last_message_known;
    }
    return valid || reject(page, ThreadBrowserError::field);
}
struct MetadataParse { ThreadParse* thread; u8 seen{}; };
bool metadata_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& metadata = *static_cast<MetadataParse*>(opaque);
    auto& thread = *metadata.thread;
    u8 bit{};
    if (json_string_equals_ascii(name, utf8_literal("archived"))) bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("locked"))) bit = 2;
    else if (thread.page->posts.data && json_string_equals_ascii(name, utf8_literal("create_timestamp"))) bit = 4;
    else return true;
    if (metadata.seen & bit) return reject(*thread.page, ThreadBrowserError::duplicate);
    metadata.seen |= bit;
    if (bit == 4) {
        // Threads created before 2022-01-09 report null; the snowflake is then the creation time.
        if (value.kind == JsonValueKind::null_value) return true;
        JsonString timestamp{};
        if (!json_value_as_string(value, &timestamp) ||
            !parse_iso_timestamp(timestamp, &thread.post.create_timestamp_ms))
            return reject(*thread.page, ThreadBrowserError::field);
        thread.post.fields |= thread_browser_post_created;
        return true;
    }
    bool state{};
    if (!json_value_to_bool(value, &state)) return reject(*thread.page, ThreadBrowserError::field);
    thread.row.fields |= bit == 1 ? thread_browser_archived_known : thread_browser_locked_known;
    if (state) thread.row.fields |= bit == 1 ? thread_browser_archived : thread_browser_locked;
    return true;
}
bool project_member(PageParse& page, ThreadBrowserRow& row,
                    const ThreadMemberSnapshot& member) noexcept {
    if ((row.fields & thread_browser_self_joined) &&
        (row.self_join_timestamp_ms != member.join_timestamp_ms || row.self_flags != member.flags))
        return reject(page, ThreadBrowserError::field);
    row.fields |= thread_browser_self_joined;
    row.self_join_timestamp_ms = member.join_timestamp_ms;
    row.self_flags = member.flags;
    return true;
}
bool visit_thread(void* opaque, u32 index, JsonValue value) noexcept {
    auto& page = *static_cast<PageParse*>(opaque);
    const u32 maximum = page.request.mode == ThreadBrowserMode::search
        ? kThreadBrowserSearchMaximumRows : kThreadBrowserArchivedLimit;
    if (index >= maximum || index >= page.rows.count) return reject(page, ThreadBrowserError::capacity);
    ThreadParse thread{};
    thread.page = &page;
    if (!read_ok(page, json_visit_indexed_object(page.document, value, 32, thread_field, &thread))) return false;
    if ((thread.seen & 63) != 63) return reject(page, ThreadBrowserError::field);
    if (thread.row.parent_id != page.request.parent_id || thread.row.guild_id != page.request.guild_id ||
        thread.row.id == thread.row.parent_id || thread.row.id == thread.row.guild_id)
        return reject(page, ThreadBrowserError::identity);
    for (u32 before = 0; before < index; ++before)
        if (page.rows[before].id == thread.row.id) return reject(page, ThreadBrowserError::duplicate);
    MetadataParse metadata{&thread};
    if (!read_ok(page, json_visit_indexed_object(page.document, thread.metadata, 32, metadata_field, &metadata))) return false;
    if ((metadata.seen & 3) != 3 || (page.request.mode == ThreadBrowserMode::archived &&
        !(thread.row.fields & thread_browser_archived))) return reject(page, ThreadBrowserError::field);
    char decoded_name[kThreadBrowserNameMaximumBytes];
    const auto decoded = json_string_decode_utf8(thread.name, {decoded_name, sizeof(decoded_name)});
    if (!decoded.ok() || !valid_name({decoded_name, decoded.bytes}, false))
        return reject(page, ThreadBrowserError::field);
    if (decoded.bytes > page.names.count - page.name_bytes) return reject(page, ThreadBrowserError::capacity);
    if (thread.seen & 128) {
        ThreadMemberSnapshot member{};
        if (!thread_member_decode(thread.member.source, thread.row.id, page.self, &member).ok())
            return reject(page, ThreadBrowserError::field);
        if (!project_member(page, thread.row, member)) return false;
    }
    thread.row.name_offset = page.name_bytes;
    thread.row.name_bytes = decoded.bytes;
    copy_bytes(page.names.data + page.name_bytes, decoded_name, decoded.bytes);
    page.name_bytes += decoded.bytes;
    page.rows[index] = thread.row;
    if (page.posts.data) {
        if (index >= page.posts.count) return reject(page, ThreadBrowserError::capacity);
        thread.post.thread_id = thread.row.id;
        thread.post.text_offset = page.text_bytes;
        page.posts[index] = thread.post;
    }
    ++page.count;
    return true;
}
// first_messages: one starter per page thread.
struct AuthorParse {
    PageParse* page;
    u64 id{};
    JsonString username{}, global_name{}, avatar{};
    u8 seen{};
    bool global_known{}, avatar_known{};
};
bool author_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& author = *static_cast<AuthorParse*>(opaque);
    u8 bit{};
    if (json_string_equals_ascii(name, utf8_literal("id"))) bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("username"))) bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("global_name"))) bit = 4;
    else if (json_string_equals_ascii(name, utf8_literal("avatar"))) bit = 8;
    else return true;
    if (author.seen & bit) return reject(*author.page, ThreadBrowserError::duplicate);
    author.seen |= bit;
    bool valid{};
    if (bit == 1) valid = json_value_to_snowflake(value, &author.id);
    else if (bit == 2) valid = json_value_as_string(value, &author.username);
    else if (value.kind == JsonValueKind::null_value) valid = true;
    else if (bit == 4) valid = author.global_known = json_value_as_string(value, &author.global_name);
    else valid = author.avatar_known = json_value_as_string(value, &author.avatar);
    return valid || reject(*author.page, ThreadBrowserError::field);
}
struct AttachmentParse {
    PageParse* page;
    u64 id{}, width{}, height{};
    JsonString content_type{}, url{};
    u8 seen{};
};
bool attachment_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& attachment = *static_cast<AttachmentParse*>(opaque);
    u8 bit{};
    if (json_string_equals_ascii(name, utf8_literal("id"))) bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("url"))) bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("content_type"))) bit = 4;
    else if (json_string_equals_ascii(name, utf8_literal("width"))) bit = 8;
    else if (json_string_equals_ascii(name, utf8_literal("height"))) bit = 16;
    else return true;
    if (attachment.seen & bit) return reject(*attachment.page, ThreadBrowserError::duplicate);
    attachment.seen |= bit;
    bool valid{};
    if (bit == 1) valid = json_value_to_snowflake(value, &attachment.id);
    else if (bit == 2) valid = json_value_as_string(value, &attachment.url);
    else if (bit == 4) valid = json_value_as_string(value, &attachment.content_type);
    else valid = value.kind == JsonValueKind::null_value ||
        (json_value_to_u64(value, bit == 8 ? &attachment.width : &attachment.height) &&
         (bit == 8 ? attachment.width : attachment.height) <= 0xffffffffull);
    return valid || reject(*attachment.page, ThreadBrowserError::field);
}
struct StarterParse {
    PageParse* page;
    ThreadBrowserPost* post{};
    u64 id{}, channel{};
    JsonString content{};
    JsonValue author{}, attachments{};
    u32 media_url_bytes{};
    u8 seen{};
    bool media{};
    char media_url[kThreadBrowserPostMediaUrlMaximumBytes];
};
ThreadBrowserMediaType media_type(JsonString content_type) noexcept {
    if (json_string_equals_ascii(content_type, utf8_literal("image/png"))) return ThreadBrowserMediaType::png;
    if (json_string_equals_ascii(content_type, utf8_literal("image/jpeg"))) return ThreadBrowserMediaType::jpeg;
    if (json_string_equals_ascii(content_type, utf8_literal("image/webp"))) return ThreadBrowserMediaType::webp;
    if (json_string_equals_ascii(content_type, utf8_literal("image/gif"))) return ThreadBrowserMediaType::gif;
    return ThreadBrowserMediaType::none;
}
bool visit_attachment(void* opaque, u32, JsonValue value) noexcept {
    auto& starter = *static_cast<StarterParse*>(opaque);
    AttachmentParse attachment{starter.page};
    if (!read_ok(*starter.page, json_visit_indexed_object(starter.page->document, value, 32, attachment_field, &attachment)))
        return false;
    if (!(attachment.seen & 1) || !(attachment.seen & 2)) return reject(*starter.page, ThreadBrowserError::field);
    const auto type = (attachment.seen & 4) ? media_type(attachment.content_type) : ThreadBrowserMediaType::none;
    if (starter.media || type == ThreadBrowserMediaType::none || !attachment.width || !attachment.height)
        return true;
    // An over-long signed URL can't be fetched from the CDN; the post shows no thumbnail.
    const auto decoded = json_string_decode_utf8(attachment.url, {starter.media_url, sizeof(starter.media_url)});
    if (!decoded.ok()) return decoded.error == JsonStringDecodeError::output_too_small ||
        reject(*starter.page, ThreadBrowserError::field);
    if (!decoded.bytes) return reject(*starter.page, ThreadBrowserError::field);
    starter.media_url_bytes = decoded.bytes;
    starter.media = true;
    starter.post->media_id = attachment.id;
    starter.post->media_width = static_cast<u32>(attachment.width);
    starter.post->media_height = static_cast<u32>(attachment.height);
    starter.post->media_type = type;
    return true;
}
bool starter_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& starter = *static_cast<StarterParse*>(opaque);
    u8 bit{};
    if (json_string_equals_ascii(name, utf8_literal("id"))) bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("channel_id"))) bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("content"))) bit = 4;
    else if (json_string_equals_ascii(name, utf8_literal("author"))) bit = 8;
    else if (json_string_equals_ascii(name, utf8_literal("attachments"))) bit = 16;
    else return true;
    if (starter.seen & bit) return reject(*starter.page, ThreadBrowserError::duplicate);
    starter.seen |= bit;
    bool valid{};
    if (bit == 1) valid = json_value_to_snowflake(value, &starter.id);
    else if (bit == 2) valid = json_value_to_snowflake(value, &starter.channel);
    else if (bit == 4) valid = json_value_as_string(value, &starter.content);
    else if (bit == 8) { valid = value.kind == JsonValueKind::object; starter.author = value; }
    else { valid = value.kind == JsonValueKind::array; starter.attachments = value; }
    return valid || reject(*starter.page, ThreadBrowserError::field);
}
// Collapses whitespace/control runs to one space, trims, and stops on a scalar boundary.
u32 write_preview(Utf8View content, char* output) noexcept {
    u32 bytes = 0;
    bool space = false;
    for (u32 at = 0; at < content.bytes;) {
        const auto scalar = utf8_decode_one(content.data + at, content.bytes - at);
        if (!scalar.valid) return 0;
        if (whitespace(scalar.codepoint) || (scalar.codepoint >= 127 && scalar.codepoint <= 159)) space = bytes != 0;
        else {
            const u32 need = scalar.width + (space ? 1u : 0u);
            if (bytes + need > kThreadBrowserPostPreviewMaximumBytes) break;
            if (space) output[bytes++] = ' ';
            copy_bytes(output + bytes, content.data + at, scalar.width);
            bytes += scalar.width;
            space = false;
        }
        at += scalar.width;
    }
    return bytes;
}
bool avatar_hash_valid(Utf8View hash) noexcept {
    if (!hash.bytes || hash.bytes > kThreadBrowserPostAvatarHashMaximumBytes) return false;
    for (u32 at = 0; at < hash.bytes; ++at) {
        const char c = hash.data[at];
        if (!(ascii_is_lower_hex_digit(c) || c == '_')) return false;
    }
    return true;
}
void append_text(PageParse& page, Utf8View value) noexcept {
    copy_bytes(page.text.data + page.text_bytes, value.data, value.bytes);
    page.text_bytes += value.bytes;
}
__declspec(noinline) bool visit_starter(void* opaque, u32 index, JsonValue value) noexcept {
    auto& page = *static_cast<PageParse*>(opaque);
    if (index >= page.count) return reject(page, ThreadBrowserError::capacity);
    StarterParse starter{&page};
    if (!read_ok(page, json_visit_indexed_object(page.document, value, 32, starter_field, &starter))) return false;
    if ((starter.seen & 7) != 7) return reject(page, ThreadBrowserError::field);
    ThreadBrowserPost* post = nullptr;
    for (u32 at = 0; at < page.count; ++at) if (page.rows[at].id == starter.channel) post = page.posts.data + at;
    if (!post) return reject(page, ThreadBrowserError::identity);
    if (post->fields & thread_browser_post_starter) return reject(page, ThreadBrowserError::duplicate);
    auto candidate = *post;
    candidate.fields |= thread_browser_post_starter;
    candidate.starter_message_id = starter.id;
    starter.post = &candidate;
    char content[kThreadBrowserPostContentMaximumBytes];
    const auto decoded = json_string_decode_utf8(starter.content, {content, sizeof(content)});
    if (!decoded.ok()) return reject(page, ThreadBrowserError::field);
    char preview[kThreadBrowserPostPreviewMaximumBytes];
    const u32 preview_bytes = write_preview({content, decoded.bytes}, preview);
    char name[kThreadBrowserPostAuthorNameMaximumBytes];
    char avatar[kThreadBrowserPostAvatarHashMaximumBytes];
    u32 name_bytes = 0, avatar_bytes = 0;
    if (starter.seen & 8) {
        AuthorParse author{&page};
        if (!read_ok(page, json_visit_indexed_object(page.document, starter.author, 32, author_field, &author))) return false;
        if (!(author.seen & 1) || !(author.seen & 2)) return reject(page, ThreadBrowserError::field);
        const auto shown = json_string_decode_utf8(author.global_known ? author.global_name : author.username, {name, sizeof(name)});
        if (!shown.ok() || !shown.bytes) return reject(page, ThreadBrowserError::field);
        name_bytes = shown.bytes;
        if (author.avatar_known) {
            const auto hash = json_string_decode_utf8(author.avatar, {avatar, sizeof(avatar)});
            if (!hash.ok() || !avatar_hash_valid({avatar, hash.bytes})) return reject(page, ThreadBrowserError::field);
            avatar_bytes = hash.bytes;
        }
        candidate.author_id = author.id;
        candidate.fields |= thread_browser_post_author;
    }
    if ((starter.seen & 16) &&
        !read_ok(page, json_visit_indexed_array(page.document, starter.attachments, 32, visit_attachment, &starter)))
        return false;
    if (starter.media) candidate.fields |= thread_browser_post_media;
    if (u64{preview_bytes} + name_bytes + avatar_bytes + starter.media_url_bytes > page.text.count - page.text_bytes)
        return reject(page, ThreadBrowserError::capacity);
    candidate.text_offset = page.text_bytes;
    candidate.preview_bytes = static_cast<u16>(preview_bytes);
    candidate.author_name_bytes = static_cast<u16>(name_bytes);
    candidate.avatar_hash_bytes = static_cast<u16>(avatar_bytes);
    candidate.media_url_bytes = static_cast<u16>(starter.media_url_bytes);
    append_text(page, {preview, preview_bytes});
    append_text(page, {name, name_bytes});
    append_text(page, {avatar, avatar_bytes});
    append_text(page, {starter.media_url, starter.media_url_bytes});
    *post = candidate;
    return true;
}
struct MemberIdentity { PageParse* page; u64 id{}, user{}; u8 seen{}; };
bool member_identity(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& identity = *static_cast<MemberIdentity*>(opaque);
    u8 bit{};
    if (json_string_equals_ascii(name, utf8_literal("id"))) bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("user_id"))) bit = 2;
    else return true;
    if (identity.seen & bit) return reject(*identity.page, ThreadBrowserError::duplicate);
    identity.seen |= bit;
    return json_value_to_snowflake(value, bit == 1 ? &identity.id : &identity.user) ||
        reject(*identity.page, ThreadBrowserError::identity);
}
bool visit_member(void* opaque, u32 index, JsonValue value) noexcept {
    auto& page = *static_cast<PageParse*>(opaque);
    if (index >= page.count) return reject(page, ThreadBrowserError::capacity);
    MemberIdentity identity{&page};
    if (!read_ok(page, json_visit_indexed_object(page.document, value, 32, member_identity, &identity))) return false;
    if (identity.seen != 3 || identity.user != page.self) return reject(page, ThreadBrowserError::identity);
    for (u32 before = 0; before < index; ++before)
        if (page.member_ids[before] == identity.id) return reject(page, ThreadBrowserError::duplicate);
    ThreadBrowserRow* row = nullptr;
    for (u32 at = 0; at < page.count; ++at) if (page.rows[at].id == identity.id) row = page.rows.data + at;
    if (!row) return reject(page, ThreadBrowserError::identity);
    ThreadMemberSnapshot member{};
    if (!thread_member_decode(value.source, identity.id, page.self, &member).ok())
        return reject(page, ThreadBrowserError::field);
    if (!project_member(page, *row, member)) return false;
    page.member_ids[index] = identity.id;
    ++page.member_count;
    return true;
}
template <typename T> ConstSlice<u8> bytes(Slice<T> span) noexcept {
    return {reinterpret_cast<const u8*>(span.data), static_cast<u32>(u64{span.count} * sizeof(T))};
}
} // namespace

bool thread_browser_request_valid(const ThreadBrowserRequest& request) noexcept {
    if (!request.parent_id || !request.guild_id || request.parent_id == request.guild_id ||
        static_cast<u8>(request.mode) > static_cast<u8>(ThreadBrowserMode::active)) return false;
    for (u8 byte : request.reserved) if (byte) return false;
    if (request.mode == ThreadBrowserMode::search) return !request.offset && valid_name(request.name, true);
    return !request.name.bytes && request.offset <= kThreadBrowserMaximumOffset &&
        request.offset % kThreadBrowserArchivedLimit == 0;
}
ThreadBrowserResult thread_browser_write_target(
    const ThreadBrowserRequest& request, Slice<u8> output) noexcept {
    if (!thread_browser_request_valid(request) || !byte_range_valid({output.data, output.count}))
        return {0, ThreadBrowserError::input};
    u8 temporary[kThreadBrowserMaximumTargetBytes];
    ProtocolWriter writer{{temporary, sizeof(temporary)}, 0};
    bool valid = protocol_writer_append_literal(&writer, "/api/v10/channels/") &&
        protocol_writer_append_u64(&writer, request.parent_id) &&
        protocol_writer_append_literal(&writer, "/threads/search?");
    if (request.mode == ThreadBrowserMode::archived)
        valid = valid && protocol_writer_append_literal(&writer,
            "archived=true&sort_by=last_message_time&sort_order=desc&limit=25&offset=") &&
            protocol_writer_append_u64(&writer, request.offset);
    else if (request.mode == ThreadBrowserMode::active)
        valid = valid && protocol_writer_append_literal(&writer,
            "archived=false&sort_by=last_message_time&sort_order=desc&limit=25&offset=") &&
            protocol_writer_append_u64(&writer, request.offset);
    else valid = valid && protocol_writer_append_literal(&writer, "name=") &&
                 protocol_writer_append_percent_encoded(&writer, request.name);
    if (!valid || writer.used > output.count) return {writer.used, ThreadBrowserError::capacity};
    copy_bytes(output.data, temporary, writer.used);
    return {writer.used};
}
namespace {
ThreadBrowserResult decode(
    Utf8View input, const ThreadBrowserRequest& request, u64 self_user_id,
    Slice<u32> structural_scratch, Slice<ThreadBrowserRow> row_scratch,
    Slice<char> name_scratch, Slice<ThreadBrowserPost> post_scratch, Slice<char> text_scratch,
    ThreadBrowserPage* output, ThreadBrowserPostPage* post_output) noexcept {
    if (!output || !self_user_id || !thread_browser_request_valid(request) || !input.bytes ||
        input.bytes > kThreadBrowserMaximumBodyBytes ||
        u64{structural_scratch.count} * sizeof(u32) > 0xffffffffull ||
        u64{row_scratch.count} * sizeof(ThreadBrowserRow) > 0xffffffffull ||
        u64{post_scratch.count} * sizeof(ThreadBrowserPost) > 0xffffffffull ||
        (post_output && (!post_scratch.data || (text_scratch.count && !text_scratch.data))))
        return {0, ThreadBrowserError::input};
    const u32 span_count = post_output ? 8 : 5;
    const ConstSlice<u8> spans[]{
        {reinterpret_cast<const u8*>(input.data), input.bytes}, bytes(structural_scratch), bytes(row_scratch),
        bytes(name_scratch), {reinterpret_cast<const u8*>(output), sizeof(*output)},
        bytes(post_scratch), bytes(text_scratch),
        {reinterpret_cast<const u8*>(post_output), post_output ? static_cast<u32>(sizeof(*post_output)) : 0u}};
    for (u32 at = 0; at < span_count; ++at) {
        if (!byte_range_valid(spans[at])) return {0, ThreadBrowserError::input};
        for (u32 before = 0; before < at; ++before)
            if (byte_ranges_overlap(spans[at], spans[before])) return {0, ThreadBrowserError::input};
    }
    if (!utf8_validate(input).valid) return {0, ThreadBrowserError::input};
    const auto query = request;
    const auto scanned = json_scan_structurals_avx1(input, structural_scratch);
    if (!scanned.ok()) return {0, scanned.error == JsonScanError::output_overflow
        ? ThreadBrowserError::capacity : ThreadBrowserError::json};
    PageParse page{};
    page.document = {input, {structural_scratch.data, scanned.count}};
    page.request = query; page.rows = row_scratch; page.names = name_scratch; page.self = self_user_id;
    if (post_output) { page.posts = post_scratch; page.text = text_scratch; }
    if (!read_ok(page, json_visit_root_object(input, page.document.structurals, 32, root_field, &page)))
        return {0, page.error};
    if (!(page.root_fields & 1) || (query.mode == ThreadBrowserMode::archived && !(page.root_fields & 4)))
        return {0, ThreadBrowserError::field};
    if (!read_ok(page, json_visit_indexed_array(page.document, page.threads, 32, visit_thread, &page)))
        return {0, page.error};
    if ((page.root_fields & 2) && page.members.kind == JsonValueKind::array &&
        !read_ok(page, json_visit_indexed_array(page.document, page.members, 32, visit_member, &page)))
        return {0, page.error};
    if (post_output && (page.root_fields & 8) && page.first_messages.kind == JsonValueKind::array &&
        !read_ok(page, json_visit_indexed_array(page.document, page.first_messages, 32, visit_starter, &page)))
        return {0, page.error};
    // Search does not expose pagination even if an unknown future server adds
    // has_more. The name-search action consumes one complete result.
    const bool paged = query.mode != ThreadBrowserMode::search;
    if (paged && page.has_more && !page.count)
        return {0, ThreadBrowserError::field};
    // The last reachable page still contains usable rows when the server has
    // more than 10,000 results. Stop local offset pagination without losing it.
    const bool continuation_reachable = paged &&
        query.offset <= kThreadBrowserMaximumOffset - kThreadBrowserArchivedLimit;
    *output = {row_scratch, name_scratch, page.count, page.name_bytes,
        continuation_reachable ? query.offset + kThreadBrowserArchivedLimit : 0,
        continuation_reachable && page.has_more, {}};
    if (post_output) *post_output = {post_scratch, text_scratch, page.count, page.text_bytes};
    return {input.bytes};
}
Utf8View post_text(const ThreadBrowserPostPage& page, u32 index, u32 part) noexcept {
    if (!page.posts.data || index >= page.post_count || page.post_count > page.posts.count ||
        page.text_bytes > page.text.count || (page.text_bytes && !page.text.data)) return {};
    const auto& post = page.posts[index];
    const u32 lengths[]{post.preview_bytes, post.author_name_bytes, post.avatar_hash_bytes, post.media_url_bytes};
    u64 offset = post.text_offset;
    for (u32 at = 0; at < part; ++at) offset += lengths[at];
    if (offset > page.text_bytes || lengths[part] > page.text_bytes - offset) return {};
    return {page.text.data + offset, lengths[part]};
}
} // namespace
ThreadBrowserResult thread_browser_decode(
    Utf8View input, const ThreadBrowserRequest& request, u64 self_user_id,
    Slice<u32> structural_scratch, Slice<ThreadBrowserRow> row_scratch,
    Slice<char> name_scratch, ThreadBrowserPage* output) noexcept {
    return decode(input, request, self_user_id, structural_scratch, row_scratch, name_scratch, {}, {}, output, nullptr);
}
ThreadBrowserResult thread_browser_decode_posts(
    Utf8View input, const ThreadBrowserRequest& request, u64 self_user_id,
    Slice<u32> structural_scratch, Slice<ThreadBrowserRow> row_scratch,
    Slice<char> name_scratch, Slice<ThreadBrowserPost> post_scratch, Slice<char> text_scratch,
    ThreadBrowserPage* output, ThreadBrowserPostPage* post_output) noexcept {
    if (!post_output) return {0, ThreadBrowserError::input};
    return decode(input, request, self_user_id, structural_scratch, row_scratch, name_scratch,
        post_scratch, text_scratch, output, post_output);
}
Utf8View thread_browser_post_preview(const ThreadBrowserPostPage& page, u32 index) noexcept {
    return post_text(page, index, 0);
}
Utf8View thread_browser_post_author_name(const ThreadBrowserPostPage& page, u32 index) noexcept {
    return post_text(page, index, 1);
}
Utf8View thread_browser_post_avatar_hash(const ThreadBrowserPostPage& page, u32 index) noexcept {
    return post_text(page, index, 2);
}
Utf8View thread_browser_post_media_url(const ThreadBrowserPostPage& page, u32 index) noexcept {
    return post_text(page, index, 3);
}
Utf8View thread_browser_row_name(const ThreadBrowserPage& page, u32 index) noexcept {
    if (!page.rows.data || !page.names.data || index >= page.row_count || page.row_count > page.rows.count ||
        page.name_bytes > page.names.count) return {};
    const auto& row = page.rows[index];
    if (row.name_offset > page.name_bytes || row.name_bytes > page.name_bytes - row.name_offset) return {};
    return {page.names.data + row.name_offset, row.name_bytes};
}
} // namespace maicord
