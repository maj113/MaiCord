#include "maicord/protocol/reaction_users.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/json_scan.hpp"
#include "maicord/protocol/rest_route.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "json_field_lists.hpp"

namespace maicord {
namespace {

struct UserObject {
    u64 id;
    JsonString username;
    JsonString global_name;
    JsonString avatar;
    ReactionUsersDecodeError error;
    bool id_seen;
    bool username_seen;
    bool global_name_seen;
    bool global_name_null;
    bool avatar_seen;
    bool avatar_null;
    bool bot;
    bool bot_seen;
};

struct PageContext {
    Slice<u32> object_scratch;
    Slice<ReactionUser> users;
    Slice<char> text;
    u32 maximum_depth;
    u32 count;
    u32 text_used;
    u32 error_offset;
    ReactionUsersDecodeError error;
    JsonScanError scan_error;
    JsonReadError read_error;
};

ReactionUsersWriteResult write_error(
    ReactionUsersWriteError error) noexcept {
    return ReactionUsersWriteResult{0, error};
}

ReactionUsersDecodeResult decode_error(
    const PageContext* context, ReactionUsersDecodeError error,
    u32 offset = 0, JsonScanError scan_error = JsonScanError::none,
    JsonReadError read_error = JsonReadError::none) noexcept {
    return ReactionUsersDecodeResult{
        0,
        0,
        context && context->error_offset != 0 ? context->error_offset : offset,
        context && context->error != ReactionUsersDecodeError::none
            ? context->error
            : error,
        context && context->scan_error != JsonScanError::none
            ? context->scan_error
            : scan_error,
        context && context->read_error != JsonReadError::none
            ? context->read_error
            : read_error};
}

bool visit_user_member(void* opaque, JsonString name,
                       JsonValue value) noexcept {
    auto* user = static_cast<UserObject*>(opaque);
    bool valid = true;
    switch (protocol_detail::member_index<"id", "username", "global_name", "avatar", "bot">(name)) {
    case 0:
        valid = !user->id_seen && json_value_to_snowflake(value, &user->id);
        user->id_seen = true;
        break;
    case 1:
        valid = !user->username_seen && json_value_as_string(value, &user->username);
        user->username_seen = true;
        break;
    case 2:
        valid = !user->global_name_seen;
        user->global_name_seen = true;
        user->global_name_null = value.kind == JsonValueKind::null_value;
        valid = valid && (user->global_name_null || json_value_as_string(value, &user->global_name));
        break;
    case 3:
        valid = !user->avatar_seen;
        user->avatar_seen = true;
        user->avatar_null = value.kind == JsonValueKind::null_value;
        valid = valid && (user->avatar_null || json_value_as_string(value, &user->avatar));
        break;
    case 4:
        valid = !user->bot_seen && json_value_to_bool(value, &user->bot);
        user->bot_seen = true;
        break;
    default: break;
    }
    if (!valid)
        user->error = ReactionUsersDecodeError::invalid_user;
    return valid;
}

enum class DecodeStringResult : u8 {
    ok,
    invalid,
    capacity,
};

DecodeStringResult decode_string(JsonString source, u32 maximum_bytes,
                                 Slice<char> output, u32* used,
                                 Utf8View* result, bool required) noexcept {
    const JsonStringDecodeResult measured =
        json_string_decode_utf8(source, Slice<char>{nullptr, 0});
    if (!(measured.ok() ||
          measured.error == JsonStringDecodeError::output_too_small) ||
        measured.bytes > maximum_bytes || (required && measured.bytes == 0)) {
        return DecodeStringResult::invalid;
    }
    if (measured.bytes > output.count - *used) {
        return DecodeStringResult::capacity;
    }
    if (measured.bytes == 0) {
        reset_value<Utf8View>(result);
        return DecodeStringResult::ok;
    }
    const JsonStringDecodeResult decoded = json_string_decode_utf8(
        source, Slice<char>{output.data + *used, output.count - *used});
    if (!decoded.ok()) {
        return DecodeStringResult::invalid;
    }
    *result = Utf8View{output.data + *used, decoded.bytes};
    *used += decoded.bytes;
    return DecodeStringResult::ok;
}

bool visit_user(void* opaque, u32, JsonValue value) noexcept {
    auto* page = static_cast<PageContext*>(opaque);
    if (value.kind != JsonValueKind::object) {
        page->error = ReactionUsersDecodeError::invalid_user;
        page->error_offset = value.source_offset;
        return false;
    }
    if (page->count >= page->users.count) {
        page->error = ReactionUsersDecodeError::user_capacity;
        page->error_offset = value.source_offset;
        return false;
    }
    const JsonScanResult scan =
        json_scan_structurals_avx1(value.source, page->object_scratch);
    if (!scan.ok()) {
        page->error = ReactionUsersDecodeError::scan;
        page->scan_error = scan.error;
        page->error_offset = value.source_offset + scan.error_offset;
        return false;
    }
    UserObject user{};
    const JsonReadResult read = json_visit_value_object(
        value,
        ConstSlice<u32>{page->object_scratch.data, scan.count},
        page->maximum_depth, visit_user_member, &user);
    if (!read.ok()) {
        page->error = user.error != ReactionUsersDecodeError::none
                          ? user.error
                          : ReactionUsersDecodeError::json;
        page->read_error = read.error;
        page->error_offset = value.source_offset + read.error_offset;
        return false;
    }
    if (!user.id_seen || !user.username_seen) {
        page->error = ReactionUsersDecodeError::invalid_user;
        page->error_offset = value.source_offset;
        return false;
    }

    ReactionUser decoded{};
    decoded.id = user.id;
    decoded.bot = user.bot_seen && user.bot;
    const auto decode = [&](JsonString source, u32 maximum, Utf8View* result, bool required) noexcept {
        const DecodeStringResult status =
            decode_string(source, maximum, page->text, &page->text_used, result, required);
        if (status != DecodeStringResult::ok) {
            page->error = status == DecodeStringResult::capacity ? ReactionUsersDecodeError::text_capacity
                                                                 : ReactionUsersDecodeError::invalid_user;
            page->error_offset = value.source_offset;
        }
        return status == DecodeStringResult::ok;
    };
    if (!decode(user.username, kReactionUserMaximumNameBytes, &decoded.username, true))
        return false;
    if (user.global_name_seen && !user.global_name_null) {
        if (!decode(user.global_name, kReactionUserMaximumNameBytes, &decoded.display_name, true))
            return false;
    } else {
        decoded.display_name = decoded.username;
    }
    if (user.avatar_seen && !user.avatar_null &&
        !decode(user.avatar, kReactionUserMaximumAvatarHashBytes, &decoded.avatar_hash, false))
        return false;
    page->users[page->count++] = decoded;
    return true;
}

} // namespace

ReactionUsersWriteResult write_reaction_users_target(
    u64 channel_id, u64 message_id, u64 emoji_id, Utf8View emoji_name,
    MessageReactionType type, u64 after_user_id, u32 limit,
    Slice<u8> output) noexcept {
    const u8 type_value = static_cast<u8>(type);
    if (channel_id == 0 || message_id == 0 || !emoji_name.data ||
        emoji_name.bytes == 0 ||
        emoji_name.bytes > kMessageReactionMaximumEmojiNameBytes ||
        type_value > static_cast<u8>(MessageReactionType::burst) ||
        limit == 0 || limit > kReactionUsersMaximumPage) {
        return write_error(ReactionUsersWriteError::invalid_input);
    }
    if (!utf8_validate(emoji_name).valid) {
        return write_error(ReactionUsersWriteError::invalid_utf8);
    }
    RouteWriter writer{};
    if (!route_writer_initialize(&writer, output) ||
        !route_append(&writer, utf8_literal("/api/v10/channels/")) ||
        !route_append_u64(&writer, channel_id) ||
        !route_append(&writer, utf8_literal("/messages/")) ||
        !route_append_u64(&writer, message_id) ||
        !route_append(&writer, utf8_literal("/reactions/")) ||
        !route_append_path_component(&writer, emoji_name) ||
        (emoji_id != 0 &&
         (!route_append_byte(&writer, ':') ||
          !route_append_u64(&writer, emoji_id))) ||
        !route_append(&writer, utf8_literal("?limit=")) ||
        !route_append_u64(&writer, limit) ||
        (after_user_id != 0 &&
         (!route_append(&writer, utf8_literal("&after=")) ||
          !route_append_u64(&writer, after_user_id))) ||
        !route_append(&writer, utf8_literal("&type=")) ||
        !route_append_u64(&writer, type_value)) {
        return write_error(ReactionUsersWriteError::output_too_small);
    }
    return ReactionUsersWriteResult{
        writer.bytes, ReactionUsersWriteError::none};
}

ReactionUsersDecodeResult decode_reaction_users(
    Utf8View input, Slice<u32> page_structural_scratch,
    Slice<u32> object_structural_scratch, u32 maximum_depth,
    Slice<ReactionUser> users, Slice<char> text_output) noexcept {
    if (!input.data || input.bytes == 0 || !page_structural_scratch.data ||
        page_structural_scratch.count == 0 ||
        !object_structural_scratch.data ||
        object_structural_scratch.count == 0 || maximum_depth == 0 ||
        !users.data || users.count == 0 || !text_output.data ||
        text_output.count == 0) {
        return decode_error(nullptr,
                            ReactionUsersDecodeError::invalid_input);
    }
    const JsonScanResult scan =
        json_scan_structurals_avx1(input, page_structural_scratch);
    if (!scan.ok()) {
        return decode_error(nullptr, ReactionUsersDecodeError::scan,
                            scan.error_offset, scan.error);
    }
    PageContext page{object_structural_scratch, users, text_output,
                     maximum_depth, 0, 0, 0,
                     ReactionUsersDecodeError::none,
                     JsonScanError::none, JsonReadError::none};
    const JsonReadResult read = json_visit_root_array(
        input, ConstSlice<u32>{page_structural_scratch.data, scan.count},
        maximum_depth, visit_user, &page);
    if (!read.ok()) {
        return decode_error(&page, ReactionUsersDecodeError::json,
                            read.error_offset, JsonScanError::none,
                            read.error);
    }
    return ReactionUsersDecodeResult{
        page.count, page.text_used, 0,
        ReactionUsersDecodeError::none, JsonScanError::none,
        JsonReadError::none};
}

} // namespace maicord
