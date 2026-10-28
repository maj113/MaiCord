#include "maicord/protocol/application_commands.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr u32 depth_limit = 64;
struct Member {
    Utf8View name;
    JsonValue value;
};
bool find_member(void* p, JsonString name, JsonValue value) noexcept {
    auto& m = *static_cast<Member*>(p);
    if (json_string_equals_utf8(name, m.name))
        m.value = value;
    return true;
}
JsonValue field(const JsonIndexedDocument& doc, JsonValue obj, const char* name) noexcept {
    return command_member(doc, obj, {name, c_string_length(name)});
}
Utf8View text(CommandIndex& index, JsonValue value) noexcept {
    JsonString string{};
    if (!json_value_as_string(value, &string))
        return {};
    if (!string.has_escapes)
        return string.encoded;
    const auto size = json_string_decode_utf8(string, {});
    if (size.error != JsonStringDecodeError::output_too_small && size.error != JsonStringDecodeError::none)
        return {};
    char* decoded = arena_allocate<char>(&index.storage, maximum(1u, size.bytes));
    if (!decoded)
        return {};
    const auto result = json_string_decode_utf8(string, {decoded, size.bytes});
    return result.error == JsonStringDecodeError::none ? Utf8View{decoded, result.bytes} : Utf8View{};
}
Utf8View named(CommandIndex& index, JsonValue value, const char* key) noexcept {
    return text(index, field(index.document, value, key));
}
Utf8View command_name(CommandIndex& index, JsonValue value) noexcept {
    const auto canonical = named(index, value, "name_default");
    return canonical.bytes ? canonical : named(index, value, "name");
}
bool is_type(const JsonIndexedDocument& doc, JsonValue value, u64 expected) noexcept {
    u64 kind = 0;
    return json_value_to_u64_or_string(field(doc, value, "type"), &kind) && kind == expected;
}
struct Walk {
    CommandIndex* index;
    ApplicationCommand root;
    Utf8View group;
    u32 count, context_count;
    bool write;
};
bool emit(Walk& w, JsonValue value, bool sub) noexcept {
    const u32 slot = w.count++;
    if (!w.write)
        return true;
    auto entry = w.root;
    entry.group_name = w.group;
    entry.sub_name = sub ? command_name(*w.index, value) : Utf8View{};
    entry.options = field(w.index->document, value, "options");
    entry.description = named(*w.index, value, "description");
    if (!entry.description.bytes)
        entry.description = named(*w.index, value, "description_default");
    const u32 length = entry.root_name.bytes + (entry.group_name.bytes ? entry.group_name.bytes + 1 : 0) +
                       (entry.sub_name.bytes ? entry.sub_name.bytes + 1 : 0);
    char* name = arena_allocate<char>(&w.index->storage, length + 1);
    if (!name || !entry.root_name.bytes || (sub && !entry.sub_name.bytes))
        return false;
    u32 used = 0;
    const Utf8View parts[] = {entry.root_name, entry.group_name, entry.sub_name};
    for (const auto part : parts) {
        if (!part.bytes)
            continue;
        if (used)
            name[used++] = ' ';
        copy_bytes(name + used, part.data, part.bytes);
        used += part.bytes;
    }
    entry.name = {name, used};
    w.index->commands[slot] = entry;
    return true;
}
bool visit_sub(void* context, u32, JsonValue value) noexcept {
    auto& w = *static_cast<Walk*>(context);
    return !is_type(w.index->document, value, 1) || emit(w, value, true);
}
bool visit_option(void* context, u32, JsonValue value) noexcept {
    auto& w = *static_cast<Walk*>(context);
    if (is_type(w.index->document, value, 1))
        return emit(w, value, true);
    if (!is_type(w.index->document, value, 2))
        return true;
    w.group = w.write ? command_name(*w.index, value) : Utf8View{};
    const auto options = field(w.index->document, value, "options");
    const bool ok = options.kind == JsonValueKind::array && options.source.bytes &&
                    json_visit_indexed_array(w.index->document, options, depth_limit, visit_sub, &w).ok();
    reset_value(&w.group);
    return ok;
}
bool read_root(Walk& w, JsonValue value) noexcept {
    const auto& doc = w.index->document;
    if (!json_value_to_u64_or_string(field(doc, value, "id"), &w.root.id) || !w.root.id ||
        !json_value_to_u64_or_string(field(doc, value, "application_id"), &w.root.application_id) ||
        !w.root.application_id ||
        !json_value_to_u64_or_string(field(doc, value, "version"), &w.root.version))
        return false;
    json_value_to_u64_or_string(field(doc, value, "guild_id"), &w.root.guild_id);
    w.root.dm_permission = true;
    json_value_to_bool(field(doc, value, "dm_permission"), &w.root.dm_permission);
    w.root.root_name = command_name(*w.index, value);
    w.root.original = value;
    return true;
}
bool visit_command(void* context, u32, JsonValue value) noexcept {
    auto& w = *static_cast<Walk*>(context);
    u64 type = 0;
    if (!json_value_to_u64_or_string(field(w.index->document, value, "type"), &type))
        return true;
    if (type == kCommandTypeUser || type == kCommandTypeMessage) {
        // Context menu commands have no options or subcommands.
        const u32 slot = w.context_count++;
        if (!w.write)
            return true;
        reset_value(&w.root);
        if (!read_root(w, value) || !w.root.root_name.bytes)
            return false;
        w.root.name = w.root.root_name;
        w.root.description = named(*w.index, value, "description");
        w.index->context_commands[slot] = w.root;
        w.index->context_types[slot] = static_cast<u8>(type);
        return true;
    }
    if (type != 1)
        return true;
    reset_value(&w.root);
    if (w.write && !read_root(w, value))
        return false;
    const u32 before = w.count;
    const auto options = field(w.index->document, value, "options");
    if (options.source.bytes && options.kind == JsonValueKind::array &&
        !json_visit_indexed_array(w.index->document, options, depth_limit, visit_option, &w).ok())
        return false;
    return w.count != before || emit(w, value, false);
}
struct Apps {
    CommandIndex* index;
    u32 count;
    bool write;
};
bool visit_application(void* context, u32, JsonValue value) noexcept {
    auto& a = *static_cast<Apps*>(context);
    const u32 slot = a.count++;
    if (!a.write)
        return true;
    auto& app = a.index->applications[slot];
    const auto& doc = a.index->document;
    if (!json_value_to_u64_or_string(field(doc, value, "id"), &app.id) || !app.id)
        return false;
    app.name = named(*a.index, value, "name");
    app.icon = named(*a.index, value, "icon");
    const auto bot = field(doc, value, "bot");
    json_value_to_u64_or_string(field(doc, bot, "id"), &app.bot_id);
    if (!app.bot_id)
        json_value_to_u64_or_string(field(doc, value, "bot_id"), &app.bot_id);
    app.avatar = named(*a.index, bot, "avatar");
    return true;
}
} // namespace
JsonValue command_member(const JsonIndexedDocument& doc, JsonValue object, Utf8View name) noexcept {
    Member member{name, {}};
    if (object.source.bytes && object.kind == JsonValueKind::object)
        json_visit_indexed_object(doc, object, depth_limit, find_member, &member);
    return member.value;
}
bool command_json_text(JsonValue value, Slice<char> output, Utf8View* result) noexcept {
    JsonString string{};
    if (!result || !json_value_as_string(value, &string))
        return false;
    const auto decoded = json_string_decode_utf8(string, output);
    if (decoded.error != JsonStringDecodeError::none)
        return false;
    *result = {output.data, decoded.bytes};
    return true;
}
void command_index_release(CommandIndex* index) noexcept {
    if (!index)
        return;
    arena_release(&index->storage);
    reset_value(index);
}
bool command_index_decode(Utf8View body, Slice<u32> scratch, CommandIndex* output) noexcept {
    if (!output || !body.bytes || !body.data)
        return false;
    const auto scan = json_scan_structurals_avx1(body, scratch);
    if (!scan.ok())
        return false;
    CommandIndex index{};
    const usize reserve = static_cast<usize>(body.bytes) * 8 + 65536;
    if (!arena_initialize(&index.storage, reserve, 0, 4096))
        return false;
    char* retained = arena_allocate<char>(&index.storage, body.bytes);
    u32* positions = arena_allocate<u32>(&index.storage, scan.count);
    if (!retained || !positions) {
        command_index_release(&index);
        return false;
    }
    copy_bytes(retained, body.data, body.bytes);
    copy_bytes(positions, scratch.data, scan.count * sizeof(u32));
    index.document = {{retained, body.bytes}, {positions, scan.count}};
    const JsonValue root{index.document.input, 0, JsonValueKind::object};
    const auto commands = field(index.document, root, "application_commands");
    const auto applications = field(index.document, root, "applications");
    Walk walk{&index};
    Apps apps{&index};
    bool ok =
        commands.source.bytes && applications.source.bytes &&
        json_visit_indexed_array(index.document, commands, depth_limit, visit_command, &walk).ok() &&
        json_visit_indexed_array(index.document, applications, depth_limit, visit_application, &apps).ok();
    if (ok) {
        index.command_count = walk.count;
        index.context_command_count = walk.context_count;
        index.application_count = apps.count;
        index.commands =
            arena_allocate_zero<ApplicationCommand>(&index.storage, maximum(1u, walk.count));
        index.context_commands =
            arena_allocate_zero<ApplicationCommand>(&index.storage, maximum(1u, walk.context_count));
        index.context_types = arena_allocate_zero<u8>(&index.storage, maximum(1u, walk.context_count));
        index.applications =
            arena_allocate_zero<CommandApplication>(&index.storage, maximum(1u, apps.count));
        walk.count = 0;
        walk.context_count = 0;
        walk.write = true;
        apps.count = 0;
        apps.write = true;
        ok = index.commands && index.context_commands && index.context_types && index.applications &&
             json_visit_indexed_array(index.document, applications, depth_limit, visit_application, &apps)
                 .ok() &&
             json_visit_indexed_array(index.document, commands, depth_limit, visit_command, &walk).ok();
    }
    if (!ok) {
        command_index_release(&index);
        return false;
    }
    command_index_release(output);
    *output = index;
    return true;
}
bool command_data(const ApplicationCommand& command, Utf8View options,
                  ProtocolWriter* w) noexcept {
    if (!w || !command.id || !command.version || !command.root_name.bytes || !options.bytes)
        return false;
    if (!protocol_writer_append_literal(w, "{\"version\":\"") ||
        !protocol_writer_append_u64(w, command.version) ||
        !protocol_writer_append_literal(w, "\",\"id\":\"") || !protocol_writer_append_u64(w, command.id) ||
        !protocol_writer_append_literal(w, "\",\"name\":") ||
        !protocol_writer_append_json_string(w, command.root_name) ||
        !protocol_writer_append_literal(w, " ,\"type\":1,\"options\":"))
        return false;
    const auto wrapper = [&](Utf8View name, u32 type) noexcept {
        return protocol_writer_append_literal(w, "[{\"type\":") && protocol_writer_append_u64(w, type) &&
               protocol_writer_append_literal(w, ",\"name\":") &&
               protocol_writer_append_json_string(w, name) &&
               protocol_writer_append_literal(w, ",\"options\":");
    };
    if (command.group_name.bytes && !wrapper(command.group_name, 2))
        return false;
    if (command.sub_name.bytes && !wrapper(command.sub_name, 1))
        return false;
    if (!protocol_writer_append_bytes(w, {reinterpret_cast<const u8*>(options.data), options.bytes}))
        return false;
    if (command.sub_name.bytes && !protocol_writer_append_literal(w, "}]"))
        return false;
    if (command.group_name.bytes && !protocol_writer_append_literal(w, "}]"))
        return false;
    if (command.guild_id &&
        (!protocol_writer_append_literal(w, ",\"guild_id\":\"") ||
         !protocol_writer_append_u64(w, command.guild_id) || !protocol_writer_append_byte(w, '"')))
        return false;
    return protocol_writer_append_literal(w, ",\"application_command\":") &&
           protocol_writer_append_bytes(w, {reinterpret_cast<const u8*>(command.original.source.data),
                                            command.original.source.bytes}) &&
           protocol_writer_append_byte(w, '}');
}

bool command_context_data(const ApplicationCommand& command, u8 type, u64 target_id,
                          ProtocolWriter* w) noexcept {
    if (!w || !command.id || !command.version || !command.root_name.bytes || !target_id ||
        (type != kCommandTypeUser && type != kCommandTypeMessage) || !command.original.source.bytes)
        return false;
    bool ok = protocol_writer_append_literal(w, "{\"version\":\"") && protocol_writer_append_u64(w, command.version) &&
              protocol_writer_append_literal(w, "\",\"id\":\"") && protocol_writer_append_u64(w, command.id) &&
              protocol_writer_append_byte(w, '"');
    if (command.guild_id)
        ok = ok && protocol_writer_append_literal(w, ",\"guild_id\":\"") &&
             protocol_writer_append_u64(w, command.guild_id) && protocol_writer_append_byte(w, '"');
    return ok && protocol_writer_append_literal(w, ",\"name\":") &&
           protocol_writer_append_json_string(w, command.root_name) &&
           protocol_writer_append_literal(w, ",\"type\":") && protocol_writer_append_u64(w, type) &&
           protocol_writer_append_literal(w, ",\"options\":[],\"application_command\":") &&
           protocol_writer_append_bytes(w, {reinterpret_cast<const u8*>(command.original.source.data),
                                            command.original.source.bytes}) &&
           protocol_writer_append_literal(w, ",\"target_id\":\"") && protocol_writer_append_u64(w, target_id) &&
           protocol_writer_append_literal(w, "\"}");
}

namespace {
struct ArgumentSchema {
    const CommandIndex* index;
    JsonValue options[25];
    Utf8View names[25];
    char decoded_names[25][128];
    u32 types[25];
    bool required[25];
    u32 count;
};
bool argument_schema(void* opaque, u32, JsonValue value) noexcept {
    auto& schema = *static_cast<ArgumentSchema*>(opaque);
    if (schema.count == 25)
        return false;
    const auto& doc = schema.index->document;
    JsonString name{};
    u64 type = 0;
    if (!json_value_as_string(field(doc, value, "name"), &name) ||
        !json_value_to_u64_or_string(field(doc, value, "type"), &type) || type < 3 || type > 11)
        return false;
    const u32 n = schema.count++;
    schema.options[n] = value;
    schema.names[n] = name.encoded;
    if (name.has_escapes) {
        const auto decoded = json_string_decode_utf8(name, {schema.decoded_names[n], 128});
        if (!decoded.ok())
            return false;
        schema.names[n] = {schema.decoded_names[n], decoded.bytes};
    }
    if (!schema.names[n].bytes)
        return false;
    schema.types[n] = static_cast<u32>(type);
    bool required = false;
    json_value_to_bool(field(doc, value, "required"), &required);
    schema.required[n] = required;
    return true;
}
bool blank(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

struct ScannedArgument {
    CommandArgumentFocus span;
    u32 slot;
};
bool scan_arguments(const ArgumentSchema& schema, Utf8View input, ScannedArgument* spans, u32* count,
                    bool partial, Utf8View* error) noexcept {
    *count = 0;
    bool seen[25]{};
    u32 at = 0;
    while (at < input.bytes) {
        while (at < input.bytes && blank(input.data[at]))
            ++at;
        if (at == input.bytes)
            break;
        const u32 start = at;
        while (at < input.bytes && input.data[at] != ':' && !blank(input.data[at]))
            ++at;
        if (at == input.bytes || input.data[at] != ':') {
            if (partial)
                break;
            *error = utf8_literal("Use option:value for command arguments");
            return false;
        }
        const Utf8View name{input.data + start, at - start};
        ++at;
        u32 slot = 0;
        while (slot < schema.count && !utf8_equal(schema.names[slot], name))
            ++slot;
        if (slot == schema.count || seen[slot]) {
            *error = utf8_literal("Unknown or repeated command option");
            return false;
        }
        seen[slot] = true;
        const u32 after_colon = at;
        while (at < input.bytes && blank(input.data[at]))
            ++at;
        bool next_option = false;
        if (at > after_colon) {
            u32 key_end = at;
            while (key_end < input.bytes && !blank(input.data[key_end]) && input.data[key_end] != ':')
                ++key_end;
            if (key_end < input.bytes && input.data[key_end] == ':') {
                const Utf8View key{input.data + at, key_end - at};
                for (u32 n = 0; n < schema.count; ++n)
                    if (utf8_equal(key, schema.names[n])) {
                        next_option = true;
                        break;
                    }
            }
        }
        if (next_option) {
            if (!partial) {
                *error = utf8_literal("Enter a value for the selected option");
                return false;
            }
            spans[(*count)++] = {{schema.options[slot],
                                  name,
                                  {input.data + after_colon, 0},
                                  after_colon,
                                  after_colon,
                                  false,
                                  true},
                                 slot};
            continue;
        }
        const u32 begin = at;
        const bool quoted = at < input.bytes && input.data[at] == '"';
        bool closed = !quoted;
        if (quoted) {
            ++at;
            bool escaped = false;
            while (at < input.bytes) {
                const char ch = input.data[at++];
                if (!escaped && ch == '"') {
                    closed = true;
                    break;
                }
                escaped = !escaped && ch == '\\';
            }
            if ((!closed && !partial) || (at < input.bytes && !blank(input.data[at]))) {
                *error = utf8_literal("Close the quoted option value");
                return false;
            }
        } else {
            while (at < input.bytes) {
                if (blank(input.data[at])) {
                    u32 next = at;
                    while (next < input.bytes && blank(input.data[next]))
                        ++next;
                    u32 end = next;
                    while (end < input.bytes && !blank(input.data[end]) && input.data[end] != ':')
                        ++end;
                    if (end < input.bytes && input.data[end] == ':')
                        break;
                }
                ++at;
            }
        }
        u32 end = at;
        if (!quoted)
            while (end > begin && blank(input.data[end - 1]))
                --end;
        if (end == begin && !partial) {
            *error = utf8_literal("Enter a value for the selected option");
            return false;
        }
        spans[(*count)++] = {{schema.options[slot],
                              name,
                              {input.data + begin, end - begin},
                              begin,
                              end,
                              quoted,
                              closed},
                             slot};
    }
    return true;
}
bool argument_lookup(const CommandIndex& index, const ApplicationCommand& command,
                     Utf8View input, u32 caret, Utf8View name,
                     CommandArgumentFocus* output) noexcept {
    if (!output || !utf8_validate(input).valid || caret > input.bytes ||
        (caret < input.bytes && (static_cast<u8>(input.data[caret]) & 0xc0) == 0x80))
        return false;
    reset_value(output);
    ArgumentSchema schema;
    schema.index = &index;
    schema.count = 0;
    if (command.options.source.bytes &&
        !json_visit_indexed_array(index.document, command.options, depth_limit, argument_schema, &schema)
             .ok())
        return false;
    ScannedArgument spans[25];
    u32 count = 0;
    Utf8View error{};
    if (!scan_arguments(schema, input, spans, &count, true, &error))
        return false;
    for (u32 n = 0; n < count; ++n) {
        const auto& span = spans[n].span;
        if (name.bytes ? utf8_equal(name, span.name)
                       : (caret >= span.value_begin && caret <= span.value_end)) {
            *output = span;
            return true;
        }
    }
    return false;
}
// JSON number grammar plus the API's finite numeric range. Accumulate only
// significant digits; a long mantissa with a negative exponent need not overflow.
bool command_number(Utf8View value, bool integer) noexcept {
    if (!value.data || !value.bytes)
        return false;
    u32 pos = value.data[0] == '-' ? 1u : 0u;
    if (pos == value.bytes || value.data[pos] < '0' || value.data[pos] > '9')
        return false;
    const u32 first = pos;
    double mantissa = 0;
    u32 significant = 0;
    i32 exponent = 0;
    while (pos < value.bytes && value.data[pos] >= '0' && value.data[pos] <= '9') {
        const u32 digit = static_cast<u32>(value.data[pos++] - '0');
        if (significant < 18) {
            mantissa = mantissa * 10 + digit;
            if (mantissa != 0)
                ++significant;
        } else
            ++exponent;
    }
    if (pos - first > 1 && value.data[first] == '0')
        return false;
    if (pos < value.bytes && value.data[pos] == '.' && !integer) {
        const u32 dot = pos++;
        while (pos < value.bytes && value.data[pos] >= '0' && value.data[pos] <= '9') {
            const u32 digit = static_cast<u32>(value.data[pos++] - '0');
            if (significant < 18) {
                mantissa = mantissa * 10 + digit;
                --exponent;
                if (mantissa != 0)
                    ++significant;
            }
        }
        if (pos == dot + 1)
            return false;
    }
    if (pos < value.bytes && (value.data[pos] == 'e' || value.data[pos] == 'E') && !integer) {
        ++pos;
        bool negative = false;
        if (pos < value.bytes && (value.data[pos] == '+' || value.data[pos] == '-'))
            negative = value.data[pos++] == '-';
        const u32 begin = pos;
        u32 magnitude = 0;
        while (pos < value.bytes && value.data[pos] >= '0' && value.data[pos] <= '9')
            magnitude = minimum(100000u, magnitude * 10 + static_cast<u32>(value.data[pos++] - '0'));
        if (pos == begin)
            return false;
        exponent += negative ? -static_cast<i32>(magnitude) : static_cast<i32>(magnitude);
    }
    if (pos != value.bytes)
        return false;
    const double maximum_value = integer ? 9007199254740991.0 : 9007199254740992.0;
    while (exponent > 0 && mantissa && mantissa <= maximum_value) {
        mantissa *= 10;
        --exponent;
    }
    while (exponent < 0 && mantissa) {
        mantissa *= 0.1;
        ++exponent;
    }
    return mantissa <= maximum_value;
}

} // namespace
bool command_argument_focus(const CommandIndex& index,
                            const ApplicationCommand& command, Utf8View input, u32 caret,
                            CommandArgumentFocus* output) noexcept {
    return argument_lookup(index, command, input, caret, {}, output);
}
bool command_argument_find(const CommandIndex& index,
                           const ApplicationCommand& command, Utf8View input, Utf8View name,
                           CommandArgumentFocus* output) noexcept {
    return name.bytes && argument_lookup(index, command, input, input.bytes, name, output);
}
bool command_argument_query(const CommandArgumentFocus& focus, Slice<char> scratch,
                            Utf8View* output) noexcept {
    if (!output)
        return false;
    if (!focus.quoted) {
        *output = focus.value;
        return true;
    }
    const u32 trim = focus.closed ? 2u : 1u;
    if (focus.value.bytes < trim)
        return false;
    const JsonString string{{focus.value.data + 1, focus.value.bytes - trim}, 0, true};
    const auto result = json_string_decode_utf8(string, scratch);
    if (!result.ok())
        return false;
    *output = {scratch.data, result.bytes};
    return true;
}
bool command_arguments(const CommandIndex& index, const ApplicationCommand& command,
                       Utf8View input, ProtocolWriter* writer, Utf8View* error,
                       Utf8View focused_name, Utf8View focused_query) noexcept {
    if (!writer || !error || !utf8_validate(input).valid || !utf8_validate(focused_name).valid ||
        !utf8_validate(focused_query).valid)
        return false;
    reset_value(error);
    ArgumentSchema schema;
    schema.index = &index;
    schema.count = 0;
    if (command.options.source.bytes &&
        !json_visit_indexed_array(index.document, command.options, depth_limit, argument_schema, &schema)
             .ok()) {
        *error = utf8_literal("Unable to read command options");
        return false;
    }
    Utf8View values[25];
    bool supplied[25]{};
    ScannedArgument spans[25];
    u32 count = 0;
    if (!scan_arguments(schema, input, spans, &count, focused_name.bytes != 0, error))
        return false;
    for (u32 n = 0; n < count; ++n) {
        const auto& argument = spans[n];
        if (!utf8_equal(argument.span.name, focused_name) &&
            (!argument.span.closed || !argument.span.value.bytes)) {
            if (focused_name.bytes)
                continue;
            *error = utf8_literal("Enter a complete option value");
            return false;
        }
        values[argument.slot] = argument.span.value;
        supplied[argument.slot] = true;
    }
    if (focused_name.bytes) {
        u32 slot = 0;
        while (slot < schema.count && !utf8_equal(schema.names[slot], focused_name))
            ++slot;
        bool enabled = false;
        if (slot == schema.count ||
            (schema.types[slot] != 3 && schema.types[slot] != 4 && schema.types[slot] != 10) ||
            !json_value_to_bool(field(index.document, schema.options[slot], "autocomplete"), &enabled) ||
            !enabled) {
            *error = utf8_literal("Option does not support autocomplete");
            return false;
        }
        supplied[slot] = true;
        values[slot] = focused_query;
    }
    const u32 mark = writer->used;
    if (!protocol_writer_append_byte(writer, '['))
        return false;
    bool comma = false;
    for (u32 n = 0; n < schema.count; ++n) {
        if (focused_name.bytes && schema.types[n] == 11)
            continue;
        if (!supplied[n]) {
            if (schema.required[n] && !focused_name.bytes) {
                *error = utf8_literal("Missing a required command option");
                writer->used = mark;
                return false;
            }
            continue;
        }
        Utf8View value = values[n];
        char decoded[16384];
        const bool focused = focused_name.bytes && utf8_equal(schema.names[n], focused_name);
        if (!focused && value.bytes && value.data[0] == '"') {
            if (!command_json_text({value, 0, JsonValueKind::string}, {decoded, sizeof(decoded)}, &value)) {
                *error = utf8_literal("Invalid quoted option value");
                writer->used = mark;
                return false;
            }
        }
        const auto fail = [&]() noexcept {
            *error = utf8_literal("Invalid command option value");
            writer->used = mark;
            return false;
        };
        if (comma && !protocol_writer_append_byte(writer, ','))
            return fail();
        comma = true;
        if (!protocol_writer_append_literal(writer, "{\"name\":") ||
            !protocol_writer_append_json_string(writer, schema.names[n]) ||
            !protocol_writer_append_literal(writer, ",\"type\":") ||
            !protocol_writer_append_u64(writer, schema.types[n]) ||
            !protocol_writer_append_literal(writer, ",\"value\":"))
            return fail();
        const u32 type = schema.types[n];
        if (type == 3 || focused) {
            if (!protocol_writer_append_json_string(writer, value))
                return fail();
        } else if (type == 4 || type == 10) {
            if (!command_number(value, type == 4))
                return fail();
            if (!protocol_writer_append_bytes(writer,
                                              {reinterpret_cast<const u8*>(value.data), value.bytes}))
                return fail();
        } else if (type == 5) {
            if (!utf8_equal(value, utf8_literal("true")) && !utf8_equal(value, utf8_literal("false")))
                return fail();
            if (!protocol_writer_append_bytes(writer,
                                              {reinterpret_cast<const u8*>(value.data), value.bytes}))
                return fail();
        } else {
            if (value.bytes > 3 && value.data[0] == '<' && value.data[value.bytes - 1] == '>') {
                u32 first = 1;
                while (first < value.bytes && (value.data[first] == '@' || value.data[first] == '#' ||
                                               value.data[first] == '&' || value.data[first] == '!'))
                    ++first;
                value = {value.data + first, value.bytes - first - 1};
            }
            JsonString id{{value.data, value.bytes}, 0, false};
            u64 parsed = 0;
            if (!json_string_to_u64(id, &parsed) || !parsed ||
                !protocol_writer_append_json_string(writer, value))
                return fail();
        }
        if (focused && !protocol_writer_append_literal(writer, ",\"focused\":true"))
            return fail();
        if (!protocol_writer_append_byte(writer, '}'))
            return fail();
    }
    return protocol_writer_append_byte(writer, ']');
}
namespace {
struct SuggestionDecode {
    const JsonIndexedDocument* document;
    CommandSuggestions* output;
};
Utf8View suggestion_text(CommandSuggestions& output, JsonValue value) noexcept {
    JsonString string{};
    if (!json_value_as_string(value, &string))
        return {};
    if (!string.has_escapes)
        return string.encoded;
    char* bytes = arena_allocate<char>(&output.storage, maximum(1u, string.encoded.bytes));
    Utf8View result{};
    return bytes && command_json_text(value, {bytes, string.encoded.bytes}, &result) ? result
                                                                                        : Utf8View{};
}
bool suggestion_choice(void* opaque, u32, JsonValue value) noexcept {
    auto& context = *static_cast<SuggestionDecode*>(opaque);
    auto& output = *context.output;
    if (output.count == 25 || value.kind != JsonValueKind::object)
        return false;
    const auto name = field(*context.document, value, "name");
    const auto choice = field(*context.document, value, "value");
    if (name.kind != JsonValueKind::string ||
        (choice.kind != JsonValueKind::string && choice.kind != JsonValueKind::number))
        return false;
    const auto label = suggestion_text(output, name);
    const auto text =
        choice.kind == JsonValueKind::string ? suggestion_text(output, choice) : choice.source;
    if ((choice.kind == JsonValueKind::number && !command_number(text, false)) || !label.data ||
        !label.bytes || !text.data || label.bytes > 400 || text.bytes > 400 ||
        utf8_validate(label).codepoints > 100 ||
        (choice.kind == JsonValueKind::string && utf8_validate(text).codepoints > 100))
        return false;
    output.choices[output.count++] = {label, text, choice.kind};
    return true;
}
} // namespace
void command_suggestions_release(CommandSuggestions* output) noexcept {
    if (!output)
        return;
    arena_release(&output->storage);
    reset_value(output);
}
bool command_suggestions_decode(Utf8View body, Slice<u32> scratch, u64 expected_nonce,
                                CommandSuggestions* output) noexcept {
    if (!output || !expected_nonce || !body.data || !body.bytes)
        return false;
    const auto scan = json_scan_structurals_avx1(body, scratch);
    if (!scan.ok())
        return false;
    JsonIndexedDocument document{body, {scratch.data, scan.count}};
    JsonValue root{body, 0, JsonValueKind::object};
    u64 nonce = 0;
    if (!json_value_to_u64_or_string(field(document, root, "nonce"), &nonce) || nonce != expected_nonce)
        return false;
    CommandSuggestions candidate{};
    candidate.nonce = nonce;
    if (!arena_initialize(&candidate.storage, static_cast<usize>(body.bytes) * 2 + 4096, 0, 4096))
        return false;
    char* bytes = arena_allocate<char>(&candidate.storage, body.bytes);
    if (!bytes) {
        command_suggestions_release(&candidate);
        return false;
    }
    copy_bytes(bytes, body.data, body.bytes);
    document.input = {bytes, body.bytes};
    root.source = document.input;
    const auto choices = field(document, root, "choices");
    SuggestionDecode context{&document, &candidate};
    if (choices.kind != JsonValueKind::array || !choices.source.bytes ||
        !json_visit_indexed_array(document, choices, 64, suggestion_choice, &context).ok()) {
        command_suggestions_release(&candidate);
        return false;
    }
    command_suggestions_release(output);
    copy_value(output, candidate);
    return true;
}
} // namespace maicord
