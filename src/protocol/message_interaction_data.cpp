#include "maicord/protocol/message_interaction_data.hpp"
#include "maicord/protocol/application_commands.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr u32 kDepthLimit = 16;
constexpr u32 kOptionNameBytes = 96;
constexpr u32 kValueBytes = 512;
// Application command option types (userdoccers: Application Command Option Type).
constexpr u32 kSubCommand = 1, kSubCommandGroup = 2, kUser = 6, kChannel = 7, kRole = 8, kMentionable = 9,
              kAttachment = 11;

struct Out {
    char* data;
    u32 capacity, used;
    bool overflow;
    void put(Utf8View text) noexcept {
        if (overflow)
            return;
        if (text.bytes > capacity - used) {
            overflow = true;
            return;
        }
        copy_bytes(data + used, text.data, text.bytes);
        used += text.bytes;
    }
    // Line breaks read as spaces on the one-line card.
    void put_line(Utf8View text) noexcept {
        const u32 begin = used;
        put(text);
        for (u32 i = begin; i < used; ++i)
            if (data[i] == '\n' || data[i] == '\r' || data[i] == '\t')
                data[i] = ' ';
    }
};

struct State {
    const JsonIndexedDocument* document;
    const InteractionDataNames* names;
    Out out;
};

struct Option {
    char name[kOptionNameBytes];
    u32 name_bytes;
    u32 type;
    JsonValue value, options;
};

bool visit_option_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& option = *static_cast<Option*>(opaque);
    if (json_string_equals_utf8(name, utf8_literal("name"))) {
        Utf8View decoded{};
        if (command_json_text(value, {option.name, sizeof(option.name)}, &decoded))
            option.name_bytes = decoded.bytes;
    } else if (json_string_equals_utf8(name, utf8_literal("type"))) {
        u64 type = 0;
        option.type = json_value_to_u64(value, &type) && type < 256 ? static_cast<u32>(type) : 0u;
    } else if (json_string_equals_utf8(name, utf8_literal("value"))) {
        option.value = value;
    } else if (json_string_equals_utf8(name, utf8_literal("options"))) {
        option.options = value;
    }
    return true;
}

// A snowflake option value: a JSON string of digits (or a number).
bool entity_id(const JsonValue& value, u64* id) noexcept {
    JsonString string{};
    if (json_value_as_string(value, &string))
        return json_string_to_u64(string, id) && *id != 0;
    return json_value_to_u64(value, id) && *id != 0;
}

// A value printed raw: a string's text, else its JSON text.
void put_raw(State& state, const JsonValue& value) noexcept {
    if (value.kind != JsonValueKind::string) {
        state.out.put_line(value.source);
        return;
    }
    char text[kValueBytes];
    Utf8View decoded{};
    if (command_json_text(value, {text, sizeof(text)}, &decoded))
        state.out.put_line(decoded);
    else
        state.out.put(utf8_literal("\xE2\x80\xA6"));
}

void put_entity(State& state, const JsonValue& value, InteractionDataEntity entity, const char* prefix) noexcept {
    u64 id = 0;
    const Utf8View name = entity_id(value, &id) && state.names && state.names->resolve
                              ? state.names->resolve(state.names->context, entity, id)
                              : Utf8View{};
    if (!name.bytes)
        return put_raw(state, value); // an unknown entity shows its raw value
    state.out.put({prefix, 1});
    state.out.put_line(name);
}

void put_value(State& state, u32 type, const JsonValue& value) noexcept {
    switch (type) {
    case kUser:
        return put_entity(state, value, InteractionDataEntity::user, "@");
    case kChannel:
        return put_entity(state, value, InteractionDataEntity::channel, "#");
    case kRole:
        return put_entity(state, value, InteractionDataEntity::role, "@");
    case kMentionable: {
        u64 id = 0;
        if (entity_id(value, &id) && state.names && state.names->resolve &&
            state.names->resolve(state.names->context, InteractionDataEntity::role, id).bytes)
            return put_entity(state, value, InteractionDataEntity::role, "@");
        return put_entity(state, value, InteractionDataEntity::user, "@");
    }
    case kAttachment:
        return state.out.put(utf8_literal("[attachment]"));
    default:
        return put_raw(state, value);
    }
}

bool visit_option(void* opaque, u32, JsonValue value) noexcept;

void put_option(State& state, const JsonValue& object, u32 depth) noexcept {
    if (object.kind != JsonValueKind::object || depth > kDepthLimit)
        return;
    Option option{};
    json_visit_indexed_object(*state.document, object, kDepthLimit, visit_option_member, &option);
    if (!option.name_bytes)
        return;
    state.out.put(utf8_literal(" "));
    state.out.put({option.name, option.name_bytes});
    if (option.type == kSubCommand || option.type == kSubCommandGroup) {
        if (option.options.kind == JsonValueKind::array && option.options.source.bytes)
            json_visit_indexed_array(*state.document, option.options, kDepthLimit, visit_option, &state);
        return;
    }
    if (!option.value.source.bytes || option.value.kind == JsonValueKind::null_value)
        return;
    state.out.put(utf8_literal(": "));
    put_value(state, option.type, option.value);
}

bool visit_option(void* opaque, u32, JsonValue value) noexcept {
    put_option(*static_cast<State*>(opaque), value, 0);
    return true;
}
} // namespace

u32 interaction_data_write_target(u64 channel_id, u64 message_id, Slice<u8> output) noexcept {
    if (!channel_id || !message_id || !output.data)
        return 0;
    ProtocolWriter writer{output, 0};
    const bool ok = protocol_writer_append_literal(&writer, "/api/v10/channels/") &&
                    protocol_writer_append_u64(&writer, channel_id) &&
                    protocol_writer_append_literal(&writer, "/messages/") &&
                    protocol_writer_append_u64(&writer, message_id) &&
                    protocol_writer_append_literal(&writer, "/interaction-data");
    return ok ? writer.used : 0;
}

RestRouteKey interaction_data_route(u64 channel_id) noexcept {
    return {kInteractionDataRouteId, channel_id, RestMethod::get, RestMajorParameter::channel};
}

u32 interaction_data_text(Utf8View body, const InteractionDataNames& names, Slice<u32> scratch,
                          Slice<char> output) noexcept {
    // Room for the ellipsis a cut line ends with.
    constexpr u32 kEllipsisBytes = 3;
    if (!body.data || !body.bytes || !output.data || output.count <= kEllipsisBytes + 2 || !scratch.data)
        return 0;
    const auto scan = json_scan_structurals_avx1(body, scratch);
    if (!scan.ok())
        return 0;
    const JsonIndexedDocument document{body, {scratch.data, scan.count}};
    const JsonValue root{body, 0, JsonValueKind::object};
    // The root command's own name; "name" alone covers a body without the definition.
    char storage[kOptionNameBytes];
    const auto member_name = [&](const JsonValue& object, const char* key) noexcept {
        Utf8View text{};
        const JsonValue member = command_member(document, object, {key, c_string_length(key)});
        return member.kind == JsonValueKind::string && command_json_text(member, {storage, sizeof(storage)}, &text)
                   ? text
                   : Utf8View{};
    };
    const JsonValue definition = command_member(document, root, utf8_literal("application_command"));
    Utf8View name = member_name(definition, "name_localized");
    if (!name.bytes)
        name = member_name(definition, "name");
    if (!name.bytes)
        name = member_name(root, "name");
    if (!name.bytes)
        return 0;
    State state{&document, &names, {output.data, output.count - kEllipsisBytes, 0, false}};
    state.out.put(utf8_literal("/"));
    state.out.put_line(name);
    const JsonValue options = command_member(document, root, utf8_literal("options"));
    if (options.kind == JsonValueKind::array && options.source.bytes)
        json_visit_indexed_array(document, options, kDepthLimit, visit_option, &state);
    u32 used = state.out.used;
    // Every piece is whole UTF-8, so the cut after the last piece that fitted is a character boundary.
    if (state.out.overflow) {
        copy_bytes(output.data + used, "\xE2\x80\xA6", kEllipsisBytes);
        used += kEllipsisBytes;
    }
    return used;
}

} // namespace maicord
