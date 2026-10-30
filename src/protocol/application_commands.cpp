#include "maicord/protocol/application_commands.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/protocol/user_settings_proto.hpp"
#include "json_helpers.hpp"
#include "proto_wire.hpp"

namespace maicord {
namespace {
constexpr u32 depth_limit = 64;

JsonValue field(const JsonIndexedDocument& doc, JsonValue obj, const char* name) noexcept {
    return command_member(doc, obj, {name, c_string_length(name)});
}
Utf8View text(CommandIndex& index, JsonValue value) noexcept {
    return protocol_detail::arena_string(&index.storage, value);
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
    json_value_to_bool(field(doc, value, "nsfw"), &w.root.nsfw);
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
    app.bot_name = named(*a.index, bot, "global_name");
    if (!app.bot_name.bytes)
        app.bot_name = named(*a.index, bot, "username");
    return true;
}
} // namespace
JsonValue command_member(const JsonIndexedDocument& doc, JsonValue object, Utf8View name) noexcept {
    return protocol_detail::object_member(doc, object, name, depth_limit);
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
bool command_data(const ApplicationCommand& command, Utf8View options, ProtocolWriter* w,
                  Utf8View attachments) noexcept {
    if (!w || !command.id || !command.version || !command.root_name.bytes || !options.bytes)
        return false;
    // The key order: version, id, guild_id, name, type, options,
    // application_command, then the staged attachments.
    if (!protocol_writer_append_literal(w, "{\"version\":\"") ||
        !protocol_writer_append_u64(w, command.version) ||
        !protocol_writer_append_literal(w, "\",\"id\":\"") || !protocol_writer_append_u64(w, command.id) ||
        !protocol_writer_append_byte(w, '"'))
        return false;
    if (command.guild_id &&
        (!protocol_writer_append_literal(w, ",\"guild_id\":\"") ||
         !protocol_writer_append_u64(w, command.guild_id) || !protocol_writer_append_byte(w, '"')))
        return false;
    if (!protocol_writer_append_literal(w, ",\"name\":") ||
        !protocol_writer_append_json_string(w, command.root_name) ||
        !protocol_writer_append_literal(w, ",\"type\":1,\"options\":"))
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
    if (!protocol_writer_append_literal(w, ",\"application_command\":") ||
        !protocol_writer_append_bytes(w, {reinterpret_cast<const u8*>(command.original.source.data),
                                          command.original.source.bytes}))
        return false;
    if (attachments.bytes &&
        (!protocol_writer_append_literal(w, ",\"attachments\":") ||
         !protocol_writer_append_bytes(w, {reinterpret_cast<const u8*>(attachments.data), attachments.bytes})))
        return false;
    return protocol_writer_append_byte(w, '}');
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
    bool required[25], autocomplete[25];
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
    bool flag = false;
    json_value_to_bool(field(doc, value, "required"), &flag);
    schema.required[n] = flag;
    flag = false;
    json_value_to_bool(field(doc, value, "autocomplete"), &flag);
    schema.autocomplete[n] = flag;
    return true;
}
bool load_schema(const CommandIndex& index, const ApplicationCommand& command, ArgumentSchema* schema) noexcept {
    schema->index = &index;
    schema->count = 0;
    return !command.options.source.bytes ||
           json_visit_indexed_array(index.document, command.options, depth_limit, argument_schema, schema).ok();
}
bool blank(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
// Option text is trimmed before use.
Utf8View trimmed(Utf8View input, u32 first, u32 last) noexcept {
    while (first < last && blank(input.data[first]))
        ++first;
    while (last > first && blank(input.data[last - 1]))
        --last;
    return {input.data + first, last - first};
}
bool option_end_at(Utf8View input, u32 at) noexcept {
    return at + kCommandOptionEndBytes <= input.bytes &&
           bytes_equal(input.data + at, kCommandOptionEnd, kCommandOptionEndBytes);
}
// The unused option whose key starts at `at` (start of the arguments, after a
// blank or right after an option's end), or schema.count.
u32 key_at(const ArgumentSchema& schema, Utf8View input, u32 at, const bool* used) noexcept {
    if (at >= input.bytes || blank(input.data[at]) ||
        (at && !blank(input.data[at - 1]) &&
         !(at >= kCommandOptionEndBytes && option_end_at(input, at - kCommandOptionEndBytes))))
        return schema.count;
    for (u32 n = 0; n < schema.count; ++n) {
        const auto& name = schema.names[n];
        if (!used[n] && name.bytes < input.bytes - at && input.data[at + name.bytes] == ':' &&
            bytes_equal(input.data + at, name.data, name.bytes))
            return n;
    }
    return schema.count;
}
u32 scan_spans(const ArgumentSchema& schema, Utf8View input, CommandArgumentSpan* spans, u32 capacity) noexcept {
    bool used[25]{};
    u32 count = 0, at = 0;
    while (at < input.bytes && count < capacity) {
        u32 slot = schema.count;
        while (at < input.bytes && (slot = key_at(schema, input, at, used)) == schema.count)
            ++at;
        if (slot == schema.count)
            break;
        used[slot] = true;
        const u32 key_first = at, value_first = at + schema.names[slot].bytes + 1;
        // A terminated value runs to its end; an unterminated one to the blank
        // before the next key, or the end.
        u32 value_last = value_first;
        while (value_last < input.bytes && !option_end_at(input, value_last))
            ++value_last;
        u32 end_last = value_last;
        if (value_last < input.bytes) {
            end_last = at = value_last + kCommandOptionEndBytes;
        } else {
            at = value_first;
            while (at < input.bytes && key_at(schema, input, at, used) == schema.count)
                ++at;
            value_last = end_last = at < input.bytes ? at - 1 : input.bytes;
        }
        spans[count++] = {schema.options[slot],    {input.data + key_first, schema.names[slot].bytes},
                          {},                      key_first,
                          value_first,             value_last,
                          end_last,                slot,
                          schema.types[slot],      schema.required[slot],
                          schema.autocomplete[slot]};
    }
    for (u32 n = 0; n < count; ++n)
        spans[n].value = trimmed(input, spans[n].value_first, spans[n].value_last);
    return count;
}
struct ChoiceMatch {
    const JsonIndexedDocument* document;
    Utf8View name;
    JsonValue value;
    bool found;
};
bool match_choice(void* opaque, u32, JsonValue choice) noexcept {
    auto& match = *static_cast<ChoiceMatch*>(opaque);
    char storage[512];
    Utf8View name{};
    if (!match.found && command_json_text(field(*match.document, choice, "name"), {storage, sizeof(storage)}, &name) &&
        utf8_equal(name, match.name)) {
        match.value = field(*match.document, choice, "value");
        match.found = true;
    }
    return true;
}
// The choice whose display name is exactly value, or none.
bool find_choice(const JsonIndexedDocument& doc, JsonValue option, Utf8View value, JsonValue* output) noexcept {
    const auto choices = field(doc, option, "choices");
    if (choices.kind != JsonValueKind::array || !value.bytes)
        return false;
    ChoiceMatch match{&doc, value, {}, false};
    json_visit_indexed_array(doc, choices, depth_limit, match_choice, &match);
    *output = match.value;
    return match.found;
}
const CommandAcceptedChoice* accepted_choice(ConstSlice<CommandAcceptedChoice> accepted, Utf8View option,
                                             Utf8View value) noexcept {
    for (u32 n = 0; n < accepted.count; ++n)
        if (utf8_equal(accepted[n].option, option) && utf8_equal(accepted[n].name, value))
            return accepted.data + n;
    return nullptr;
}
// A typed number as JavaScript's Number() reads it, with the en-US group
// separator ',' dropped. Decimal literals (sign, digits,
// fraction, exponent), unsigned 0x/0o/0b integers and a signed Infinity.
struct NumberText {
    double value;
    Utf8View whole, fraction, exponent;
    bool negative, exponent_negative, has_exponent;
    char text[64];
};
bool digit(char c) noexcept {
    return c >= '0' && c <= '9';
}
bool parse_number(Utf8View input, NumberText* out) noexcept {
    *out = {};
    u32 bytes = 0;
    for (u32 n = 0; n < input.bytes; ++n) {
        if (input.data[n] == ',')
            continue;
        if (bytes == sizeof(out->text))
            return false;
        out->text[bytes++] = input.data[n];
    }
    const Utf8View text{out->text, bytes};
    if (bytes > 2 && text.data[0] == '0') {
        const char prefix = ascii_lower(text.data[1]);
        const u32 base = prefix == 'x' ? 16 : prefix == 'o' ? 8 : prefix == 'b' ? 2 : 0;
        if (base) {
            double value = 0;
            for (u32 n = 2; n < bytes; ++n) {
                const i32 d = ascii_hex_value(text.data[n]);
                if (d < 0 || static_cast<u32>(d) >= base)
                    return false;
                value = value * base + d;
            }
            out->value = value;
            return true;
        }
    }
    u32 pos = 0;
    if (pos < text.bytes && (text.data[pos] == '+' || text.data[pos] == '-'))
        out->negative = text.data[pos++] == '-';
    // Infinity only has to compare beyond every safe number.
    if (utf8_equal({text.data + pos, text.bytes - pos}, utf8_literal("Infinity"))) {
        out->value = out->negative ? -1e300 : 1e300;
        return true;
    }
    const u32 whole = pos;
    while (pos < text.bytes && digit(text.data[pos]))
        ++pos;
    out->whole = {text.data + whole, pos - whole};
    if (pos < text.bytes && text.data[pos] == '.') {
        const u32 fraction = ++pos;
        while (pos < text.bytes && digit(text.data[pos]))
            ++pos;
        out->fraction = {text.data + fraction, pos - fraction};
    }
    if (!out->whole.bytes && !out->fraction.bytes)
        return false;
    if (pos < text.bytes && (text.data[pos] == 'e' || text.data[pos] == 'E')) {
        ++pos;
        if (pos < text.bytes && (text.data[pos] == '+' || text.data[pos] == '-'))
            out->exponent_negative = text.data[pos++] == '-';
        const u32 first = pos;
        while (pos < text.bytes && digit(text.data[pos]))
            ++pos;
        if (pos == first)
            return false;
        out->exponent = {text.data + first, pos - first};
        out->has_exponent = true;
    }
    if (pos != text.bytes)
        return false;
    // Accumulate only significant digits; a long mantissa with a negative
    // exponent need not overflow.
    double mantissa = 0;
    u32 significant = 0;
    i32 exponent = 0;
    for (u32 n = 0; n < out->whole.bytes; ++n) {
        if (significant < 18) {
            mantissa = mantissa * 10 + (out->whole.data[n] - '0');
            significant += mantissa != 0 ? 1u : 0u;
        } else
            ++exponent;
    }
    for (u32 n = 0; n < out->fraction.bytes && significant < 18; ++n) {
        mantissa = mantissa * 10 + (out->fraction.data[n] - '0');
        significant += mantissa != 0 ? 1u : 0u;
        --exponent;
    }
    i32 magnitude = 0;
    for (u32 n = 0; n < out->exponent.bytes; ++n)
        magnitude = minimum(100000, magnitude * 10 + (out->exponent.data[n] - '0'));
    exponent += out->exponent_negative ? -magnitude : magnitude;
    // Past 1e300 the value only has to compare beyond every safe number.
    while (exponent > 0 && mantissa != 0 && mantissa < 1e300) {
        mantissa *= 10;
        --exponent;
    }
    while (exponent < 0 && mantissa != 0) {
        mantissa /= 10;
        ++exponent;
    }
    out->value = out->negative ? -mantissa : mantissa;
    return true;
}
constexpr double max_safe_integer = 9007199254740991.0;
bool safe_integer(double value) noexcept {
    return value >= -max_safe_integer && value <= max_safe_integer &&
           static_cast<double>(static_cast<i64>(value)) == value;
}
Utf8View join(Slice<char> output, Utf8View a, Utf8View b = {}, Utf8View c = {}, Utf8View d = {},
              Utf8View e = {}) noexcept {
    u32 used = 0;
    const Utf8View parts[] = {a, b, c, d, e};
    for (const auto part : parts) {
        if (part.bytes > output.count - used)
            return {};
        copy_bytes(output.data + used, part.data, part.bytes);
        used += part.bytes;
    }
    return {output.data, used};
}
// The per-type message for a value that does not check out.
Utf8View invalid_text(const JsonIndexedDocument& doc, JsonValue option, u64 type) noexcept {
    if (field(doc, option, "choices").kind == JsonValueKind::array)
        return utf8_literal("Not a valid choice.");
    switch (type) {
    case 5:
        return utf8_literal("Pick either true or false.");
    case 7:
        return utf8_literal("Not a valid channel.");
    case 4:
        return utf8_literal("Input a valid integer.");
    case 10:
        return utf8_literal("Input a valid number.");
    case 8:
        return utf8_literal("Not a valid role.");
    case 6:
        return utf8_literal("Not a valid user.");
    case 9:
        return utf8_literal("Not a valid user or role.");
    default:
        return utf8_literal("Not a valid value.");
    }
}
// The id of a user, channel or role value: its digits, or a mention of the
// option's kind (<@id>, <@!id>, <#id>, <@&id>).
bool mention_id(Utf8View value, u64 type, Utf8View* id) noexcept {
    Utf8View digits = value;
    if (value.bytes > 3 && value.data[0] == '<' && value.data[value.bytes - 1] == '>') {
        const bool user = value.data[1] == '@' && value.data[2] != '&';
        const bool role = value.data[1] == '@' && value.data[2] == '&';
        const bool channel = value.data[1] == '#';
        const bool kind = type == 6 ? user : type == 7 ? channel : type == 8 ? role : type == 9 && (user || role);
        if (!kind)
            return false;
        u32 first = 2 + (role || (user && value.data[2] == '!') ? 1u : 0u);
        digits = {value.data + first, value.bytes - first - 1};
    }
    u64 parsed = 0;
    for (u32 n = 0; n < digits.bytes; ++n)
        if (!digit(digits.data[n]))
            return false;
    if (!json_string_to_u64({{digits.data, digits.bytes}, 0, false}, &parsed) || !parsed)
        return false;
    *id = digits;
    return true;
}
Utf8View number_range(const JsonIndexedDocument& doc, JsonValue option, double value, Slice<char> message) noexcept {
    const auto low = field(doc, option, "min_value"), high = field(doc, option, "max_value");
    NumberText bound{};
    const bool has_low = low.kind == JsonValueKind::number && parse_number(low.source, &bound);
    const double minimum_value = bound.value;
    const bool has_high = high.kind == JsonValueKind::number && parse_number(high.source, &bound);
    const double maximum_value = bound.value;
    if (!((has_low && value < minimum_value) || (has_high && value > maximum_value)))
        return {};
    if (has_low && has_high)
        return join(message, utf8_literal("Enter a number between "), low.source, utf8_literal(" and "), high.source,
                    utf8_literal("."));
    if (has_low)
        return join(message, utf8_literal("Enter a number greater than or equal to "), low.source, utf8_literal("."));
    return join(message, utf8_literal("Enter a number less than or equal to "), high.source, utf8_literal("."));
}
Utf8View length_range(const JsonIndexedDocument& doc, JsonValue option, Utf8View value, Slice<char> message) noexcept {
    u64 low = 0, high = 0;
    const bool has_low = json_value_to_u64(field(doc, option, "min_length"), &low);
    const bool has_high = json_value_to_u64(field(doc, option, "max_length"), &high);
    // The client counts JavaScript string length: UTF-16 units.
    const u64 length = utf8_utf16_units(value);
    if (!((has_low && length < low) || (has_high && length > high)))
        return {};
    char low_text[24], high_text[24];
    const Utf8View low_view{low_text, decimal_write(low_text, low)}, high_view{high_text, decimal_write(high_text, high)};
    if (has_low && has_high && low == high)
        return join(message, utf8_literal("Enter "), low_view, utf8_literal(" characters."));
    if (has_low && has_high)
        return join(message, utf8_literal("Enter between "), low_view, utf8_literal(" and "), high_view,
                    utf8_literal(" characters."));
    if (has_low)
        return join(message, utf8_literal("Enter "), low_view, utf8_literal(" or more characters."));
    return join(message, utf8_literal("Enter "), high_view, utf8_literal(" or fewer characters."));
}
} // namespace
u32 command_argument_spans(const CommandIndex& index, const ApplicationCommand& command, Utf8View input,
                           Slice<CommandArgumentSpan> output) noexcept {
    ArgumentSchema schema;
    if (!output.data || !utf8_validate(input).valid || !command.options.source.bytes ||
        !load_schema(index, command, &schema))
        return 0;
    return scan_spans(schema, input, output.data, minimum<u32>(output.count, 25));
}
Utf8View command_option_check(const CommandIndex& index, JsonValue option, bool present, Utf8View value,
                              bool allow_empty, Slice<char> message,
                              ConstSlice<CommandAcceptedChoice> accepted) noexcept {
    const auto& doc = index.document;
    u64 type = 0;
    bool required = false;
    json_value_to_u64_or_string(field(doc, option, "type"), &type);
    json_value_to_bool(field(doc, option, "required"), &required);
    constexpr auto required_text = utf8_literal("This option is required. Specify a value.");
    // ATTACHMENT options take the channel's staged uploads when the command is
    // sent (command_arguments asks for a file then).
    if (type == 11)
        return {};
    if (!present)
        return required ? required_text : Utf8View{};
    if (!value.bytes)
        return allow_empty ? Utf8View{} : required ? required_text : invalid_text(doc, option, type);
    bool autocomplete = false;
    json_value_to_bool(field(doc, option, "autocomplete"), &autocomplete);
    char name[128];
    Utf8View option_name{};
    command_json_text(field(doc, option, "name"), {name, sizeof(name)}, &option_name);
    const auto* taken = autocomplete ? accepted_choice(accepted, option_name, value) : nullptr;
    JsonValue choice{};
    const bool has_choices = field(doc, option, "choices").kind == JsonValueKind::array;
    switch (type) {
    case 3:
        if (has_choices)
            return find_choice(doc, option, value, &choice) && choice.kind == JsonValueKind::string
                       ? Utf8View{}
                       : invalid_text(doc, option, type);
        if (taken && taken->kind == JsonValueKind::string)
            return {};
        return length_range(doc, option, value, message);
    case 4:
    case 10: {
        if (has_choices)
            return find_choice(doc, option, value, &choice) && choice.kind == JsonValueKind::number
                       ? Utf8View{}
                       : invalid_text(doc, option, type);
        if (taken && taken->kind == JsonValueKind::number)
            return {};
        NumberText number{};
        if (!parse_number(value, &number) ||
            (type == 4 ? !safe_integer(number.value)
                       : number.value > max_safe_integer || number.value < -max_safe_integer))
            return invalid_text(doc, option, type);
        return number_range(doc, option, number.value, message);
    }
    case 5:
        return ascii_equal_ignore_case(value, utf8_literal("true")) ||
                       ascii_equal_ignore_case(value, utf8_literal("false"))
                   ? Utf8View{}
                   : invalid_text(doc, option, type);
    case 6:
    case 7:
    case 8:
    case 9: {
        Utf8View id{};
        return mention_id(value, type, &id) ? Utf8View{} : invalid_text(doc, option, type);
    }
    default:
        return invalid_text(doc, option, type);
    }
}
u32 command_choice_score(Utf8View name, Utf8View query) noexcept {
    if (query.bytes <= name.bytes && ascii_equal_ignore_case({name.data, query.bytes}, query))
        return name.bytes == query.bytes ? 10 : 7;
    if (ascii_contains_ignore_case(name, query))
        return 5;
    // Every comma- or space-separated word of the query.
    bool words = query.bytes != 0;
    for (u32 at = 0; at < query.bytes && words;) {
        while (at < query.bytes && (query.data[at] == ' ' || query.data[at] == ','))
            ++at;
        u32 end = at;
        while (end < query.bytes && query.data[end] != ' ' && query.data[end] != ',')
            ++end;
        words = ascii_contains_ignore_case(name, {query.data + at, end - at});
        at = end;
    }
    if (words)
        return 3;
    // fuzzysearch: the query's characters in order.
    u32 at = 0;
    for (u32 n = 0; n < query.bytes; ++n) {
        while (at < name.bytes && ascii_lower(name.data[at]) != ascii_lower(query.data[n]))
            ++at;
        if (at++ >= name.bytes)
            return 0;
    }
    return 1;
}
bool command_arguments(const CommandIndex& index, const ApplicationCommand& command,
                       Utf8View input, ProtocolWriter* writer, Utf8View* error,
                       Utf8View focused_name, Utf8View focused_query, ConstSlice<Utf8View> files,
                       CommandAttachmentPlan* plan, ConstSlice<CommandAcceptedChoice> accepted) noexcept {
    if (!writer || !error || !utf8_validate(input).valid || !utf8_validate(focused_name).valid ||
        !utf8_validate(focused_query).valid || files.count > 10)
        return false;
    reset_value(error);
    if (plan)
        plan->count = 0;
    ArgumentSchema schema;
    if (!load_schema(index, command, &schema)) {
        *error = utf8_literal("Unable to read command options");
        return false;
    }
    CommandArgumentSpan spans[25];
    const u32 count = scan_spans(schema, input, spans, 25);
    Utf8View values[25];
    bool supplied[25]{};
    for (u32 n = 0; n < count; ++n) {
        values[spans[n].slot] = spans[n].value;
        supplied[spans[n].slot] = true;
    }
    u32 focused = schema.count;
    if (focused_name.bytes) {
        focused = 0;
        while (focused < schema.count && !utf8_equal(schema.names[focused], focused_name))
            ++focused;
        if (focused == schema.count ||
            (schema.types[focused] != 3 && schema.types[focused] != 4 && schema.types[focused] != 10) ||
            !schema.autocomplete[focused]) {
            *error = utf8_literal("Option does not support autocomplete");
            return false;
        }
        supplied[focused] = true;
        values[focused] = focused_query;
    }
    const u32 mark = writer->used;
    const auto fail = [&](Utf8View text) noexcept {
        *error = text;
        writer->used = mark;
        return false;
    };
    if (!protocol_writer_append_byte(writer, '['))
        return false;
    bool comma = false;
    for (u32 n = 0; n < schema.count; ++n) {
        const u32 type = schema.types[n];
        if (focused_name.bytes && type == 11)
            continue;
        if (type == 11 && (supplied[n] || schema.required[n])) {
            // The upload whose name the value gives, else the next unused one.
            const Utf8View wanted = supplied[n] ? values[n] : Utf8View{};
            u32 chosen = files.count;
            for (u32 file = 0; plan && file < files.count && plan->count < 10; ++file) {
                bool used = false;
                for (u32 k = 0; k < plan->count && !used; ++k)
                    used = plan->file[k] == file;
                if (used || (wanted.bytes && !utf8_equal(files[file], wanted)))
                    continue;
                chosen = file;
                break;
            }
            if (chosen == files.count)
                return fail(utf8_literal("Please attach a file"));
            if ((comma && !protocol_writer_append_byte(writer, ',')) ||
                !protocol_writer_append_literal(writer, "{\"name\":") ||
                !protocol_writer_append_json_string(writer, schema.names[n]) ||
                !protocol_writer_append_literal(writer, ",\"type\":11,\"value\":") ||
                !protocol_writer_append_u64(writer, plan->count) || !protocol_writer_append_byte(writer, '}')) {
                writer->used = mark;
                return false;
            }
            comma = true;
            plan->file[plan->count++] = static_cast<u8>(chosen);
            continue;
        }
        if (n != focused) {
            char message[160];
            const Utf8View failure = command_option_check(index, schema.options[n], supplied[n], values[n], false,
                                                          {message, sizeof(message)}, accepted);
            if (failure.bytes) {
                if (focused_name.bytes)
                    continue;
                // A formatted range message lives in local storage; the caller
                // checks options itself first, so this keeps the type's text.
                return fail(failure.data == message ? invalid_text(index.document, schema.options[n], type)
                                                    : failure);
            }
        }
        if (!supplied[n])
            continue;
        Utf8View value = values[n];
        const auto* taken = n != focused ? accepted_choice(accepted, schema.names[n], value) : nullptr;
        JsonValue choice{};
        const bool chosen =
            n != focused && !taken && find_choice(index.document, schema.options[n], value, &choice);
        if ((comma && !protocol_writer_append_byte(writer, ',')) || !protocol_writer_append_byte(writer, '{') ||
            !protocol_writer_append_literal(writer, "\"name\":") ||
            !protocol_writer_append_json_string(writer, schema.names[n]) ||
            !protocol_writer_append_literal(writer, ",\"type\":") || !protocol_writer_append_u64(writer, type) ||
            !protocol_writer_append_literal(writer, ",\"value\":"))
            return fail(utf8_literal("Command is too large to send"));
        comma = true;
        bool ok = true;
        char decoded[512];
        char number_text[48];
        if (n == focused) {
            ok = protocol_writer_append_json_string(writer, value);
        } else if (taken || chosen) {
            // A choice sends its value: a string for STRING, a number otherwise.
            const JsonValueKind kind = taken ? taken->kind : choice.kind;
            Utf8View mapped = taken ? taken->value : choice.source;
            if (!taken && kind == JsonValueKind::string && !command_json_text(choice, {decoded, sizeof(decoded)}, &mapped))
                return fail(invalid_text(index.document, schema.options[n], type));
            ok = kind == JsonValueKind::string
                     ? protocol_writer_append_json_string(writer, mapped)
                     : protocol_writer_append_bytes(writer, {reinterpret_cast<const u8*>(mapped.data), mapped.bytes});
        } else if (type == 3) {
            ok = protocol_writer_append_json_string(writer, value);
        } else if (type == 4 || type == 10) {
            NumberText number{};
            parse_number(value, &number);
            u32 used = 0;
            // Numbers are sent as JSON numbers: whole values as integers.
            if (safe_integer(number.value)) {
                used = decimal_write_signed(number_text, static_cast<i64>(number.value));
            } else {
                // A fraction in JSON form: no '+', no leading zeros, a whole
                // part before the point and no trailing zeros after it.
                Utf8View whole = number.whole, fraction = number.fraction;
                while (whole.bytes > 1 && whole.data[0] == '0')
                    whole = {whole.data + 1, whole.bytes - 1};
                while (fraction.bytes && fraction.data[fraction.bytes - 1] == '0')
                    --fraction.bytes;
                const Utf8View parts[] = {number.negative ? utf8_literal("-") : Utf8View{},
                                          whole.bytes ? whole : utf8_literal("0"),
                                          fraction.bytes ? utf8_literal(".") : Utf8View{}, fraction,
                                          number.has_exponent ? (number.exponent_negative ? utf8_literal("e-")
                                                                                          : utf8_literal("e"))
                                                              : Utf8View{},
                                          number.exponent};
                for (const auto part : parts) {
                    if (part.bytes > sizeof(number_text) - used)
                        return fail(invalid_text(index.document, schema.options[n], type));
                    copy_bytes(number_text + used, part.data, part.bytes);
                    used += part.bytes;
                }
            }
            ok = protocol_writer_append_bytes(writer, {reinterpret_cast<const u8*>(number_text), used});
        } else if (type == 5) {
            ok = ascii_equal_ignore_case(value, utf8_literal("true")) ? protocol_writer_append_literal(writer, "true")
                                                                       : protocol_writer_append_literal(writer, "false");
        } else {
            Utf8View id{};
            ok = mention_id(value, type, &id) && protocol_writer_append_json_string(writer, id);
        }
        if (!ok || (n == focused && !protocol_writer_append_literal(writer, ",\"focused\":true")) ||
            !protocol_writer_append_byte(writer, '}'))
            return fail(utf8_literal("Command is too large to send"));
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
    NumberText number{};
    if ((choice.kind == JsonValueKind::number &&
         (!parse_number(text, &number) || number.value > max_safe_integer + 1 || number.value < -max_safe_integer - 1)) ||
        !label.data ||
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
namespace {
bool first_element(void* opaque, u32, JsonValue value) noexcept {
    *static_cast<JsonValue*>(opaque) = value;
    return false;
}
void first_form_error(const JsonIndexedDocument& doc, JsonValue value, u32 depth, JsonValue* entry) noexcept;
struct FormErrorWalk {
    const JsonIndexedDocument* document;
    JsonValue* entry;
    u32 depth;
    bool descended;
};
bool form_error_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& walk = *static_cast<FormErrorWalk*>(opaque);
    if (walk.descended || json_string_equals_utf8(name, utf8_literal("_errors")) ||
        (value.kind != JsonValueKind::object && value.kind != JsonValueKind::array))
        return true;
    // Follow only the first nested container.
    walk.descended = true;
    first_form_error(*walk.document, value, walk.depth + 1, walk.entry);
    return true;
}
void first_form_error(const JsonIndexedDocument& doc, JsonValue value, u32 depth, JsonValue* entry) noexcept {
    if (depth > 16 || !value.source.bytes)
        return;
    if (value.kind == JsonValueKind::array) {
        JsonValue element{};
        json_visit_indexed_array(doc, value, depth_limit, first_element, &element);
        first_form_error(doc, element, depth + 1, entry);
        return;
    }
    if (value.kind != JsonValueKind::object)
        return;
    const auto errors = field(doc, value, "_errors");
    if (errors.kind == JsonValueKind::array) {
        json_visit_indexed_array(doc, errors, depth_limit, first_element, entry);
        return;
    }
    FormErrorWalk walk{&doc, entry, depth, false};
    json_visit_indexed_object(doc, value, depth_limit, form_error_member, &walk);
}
} // namespace
bool command_interaction_failure(Utf8View body, Slice<u32> scratch, Slice<char> message,
                                 CommandInteractionFailure* output) noexcept {
    if (!output)
        return false;
    reset_value(output);
    if (!body.data || !body.bytes || body.data[0] != '{')
        return false;
    const auto scan = json_scan_structurals_avx1(body, scratch);
    if (!scan.ok())
        return false;
    const JsonIndexedDocument doc{body, {scratch.data, scan.count}};
    const JsonValue root{body, 0, JsonValueKind::object};
    json_value_to_u64(field(doc, root, "code"), &output->code);
    JsonValue text = field(doc, root, "message");
    const auto errors = field(doc, root, "errors");
    constexpr u64 invalid_form_body = 50035, unknown_integration = 10005;
    if (output->code == invalid_form_body && errors.kind == JsonValueKind::object) {
        JsonValue entry{};
        first_form_error(doc, errors, 0, &entry);
        char storage[64];
        Utf8View code{};
        output->stale_index =
            command_json_text(field(doc, entry, "code"), {storage, sizeof(storage)}, &code) &&
            (utf8_equal(code, utf8_literal("INTERACTION_APPLICATION_COMMAND_INVALID_VERSION")) ||
             utf8_equal(code, utf8_literal("INTERACTION_APPLICATION_COMMAND_INVALID")));
        text = field(doc, entry, "message");
    } else
        output->stale_index = output->code == unknown_integration;
    if (!command_json_text(text, message, &output->message))
        reset_value(&output->message);
    return true;
}

namespace {
using namespace protocol_detail;
constexpr u32 kFrecencyField = 7, kVersionsField = 1, kDataVersionField = 3;
// One FrecencyItem message in [p, stop) of an extracted group.
bool frecency_item(const ProtoSource& w, u32 p, u32 stop, CommandFrecencyEntry* entry) noexcept {
    while (p < stop) {
        u32 field = 0, type = 0;
        u64 n = 0;
        if (!proto_tag(w, p, stop, field, type))
            return false;
        if ((field == 1 || field == 3 || field == 4) && type == 0) {
            if (!proto_varint(w, p, stop, n))
                return false;
            if (field == 1)
                entry->total_uses = static_cast<u32>(n);
            else if (field == 3)
                entry->frecency = static_cast<i32>(n);
            else
                entry->score = static_cast<i32>(n) * 100;
        } else if (field == 2 && (type == 0 || type == 2)) {
            u32 end = p;
            if (type == 2 && !proto_length(w, p, stop, end))
                return false;
            do {
                if (!proto_varint(w, p, type == 2 ? end : stop, n))
                    return false;
                if (n) {
                    // Keep the latest ten, ascending.
                    if (entry->recent_count == kCommandFrecencyRecentUses) {
                        if (n <= entry->recent_uses[0])
                            continue;
                        copy_bytes(entry->recent_uses, entry->recent_uses + 1,
                                   (kCommandFrecencyRecentUses - 1) * sizeof(u64));
                        --entry->recent_count;
                    }
                    u32 at = entry->recent_count++;
                    while (at && entry->recent_uses[at - 1] > n) {
                        entry->recent_uses[at] = entry->recent_uses[at - 1];
                        --at;
                    }
                    entry->recent_uses[at] = n;
                }
            } while (type == 2 && p < end);
        } else if (!proto_skip(w, p, stop, type, field)) {
            return false;
        }
    }
    return true;
}
i64 find_frecency(ConstSlice<CommandFrecencyEntry> entries, u32 count, Utf8View key) noexcept {
    for (u32 n = 0; n < count; ++n)
        if (entries[n].key_bytes == key.bytes && bytes_equal(entries[n].key, key.data, key.bytes))
            return n;
    return -1;
}
u64 last_use(const CommandFrecencyEntry& entry) noexcept {
    return entry.recent_count ? entry.recent_uses[entry.recent_count - 1] : 0;
}
} // namespace

bool command_frecency_decode_response(Utf8View body, Slice<u8> scratch, Slice<CommandFrecencyEntry> entries,
                                      u32* count, u32* data_version) noexcept {
    if (!count || !data_version || !entries.data || !scratch.data)
        return false;
    *count = 0;
    *data_version = 0;
    const auto response =
        user_settings_proto_decode_response(body, (1u << kVersionsField) | (1u << kFrecencyField), {}, scratch);
    if (!response.extracted.ok())
        return false;
    const ConstSlice<u8> extracted{scratch.data, response.extracted.bytes};
    const ConstSlice<u8> versions = user_settings_proto_group(extracted, kVersionsField);
    const ProtoSource v = proto_source(versions);
    for (u32 p = 0; p < v.size;) {
        u32 field = 0, type = 0;
        u64 n = 0;
        if (!proto_tag(v, p, v.size, field, type))
            return false;
        if (field == kDataVersionField && type == 0) {
            if (!proto_varint(v, p, v.size, n))
                return false;
            *data_version = static_cast<u32>(n);
        } else if (!proto_skip(v, p, v.size, type, field)) {
            return false;
        }
    }
    const ConstSlice<u8> group = user_settings_proto_group(extracted, kFrecencyField);
    const ProtoSource w = proto_source(group);
    for (u32 p = 0; p < w.size;) {
        u32 field = 0, type = 0, stop = 0;
        if (!proto_tag(w, p, w.size, field, type))
            return false;
        if (field != 1 || type != 2) {
            if (!proto_skip(w, p, w.size, type, field))
                return false;
            continue;
        }
        if (!proto_length(w, p, w.size, stop))
            return false;
        // Map entry: key = 1, value = 2.
        CommandFrecencyEntry entry{};
        bool keyed = false, long_key = false;
        while (p < stop) {
            u32 entry_field = 0, entry_type = 0, end = 0;
            if (!proto_tag(w, p, stop, entry_field, entry_type))
                return false;
            if (entry_type != 2) {
                if (!proto_skip(w, p, stop, entry_type, entry_field))
                    return false;
                continue;
            }
            if (!proto_length(w, p, stop, end))
                return false;
            if (entry_field == 1) {
                long_key = end - p > kCommandFrecencyKeyBytes;
                if (!long_key) {
                    copy_bytes(entry.key, group.data + p, end - p);
                    entry.key_bytes = static_cast<u8>(end - p);
                    keyed = true;
                }
            } else if (entry_field == 2) {
                reset_value(&entry.recent_uses);
                entry.recent_count = 0;
                if (!frecency_item(w, p, end, &entry))
                    return false;
            }
            p = end;
        }
        if (!keyed || long_key)
            continue;
        const i64 existing = find_frecency({entries.data, entries.count}, *count, {entry.key, entry.key_bytes});
        if (existing >= 0)
            entries[static_cast<u32>(existing)] = entry;
        else if (*count < entries.count)
            entries[(*count)++] = entry;
    }
    return true;
}

u32 command_frecency_compute(Slice<CommandFrecencyEntry> entries, u32 count, u64 now_unix_ms) noexcept {
    constexpr u64 day_ms = 24ull * 60 * 60 * 1000;
    u32 kept = 0;
    for (u32 n = 0; n < count; ++n) {
        auto entry = entries[n];
        u32 score = 0;
        for (u32 use = 0; use < entry.recent_count; ++use) {
            const u64 used = entry.recent_uses[use];
            const u64 days = now_unix_ms > used ? (now_unix_ms - used) / day_ms : 0;
            score += days <= 3 ? 100 : days <= 15 ? 70 : days <= 30 ? 50 : days <= 45 ? 30 : days <= 80 ? 10 : 1;
        }
        if (!score)
            continue;
        entry.score = static_cast<i32>(score);
        // ceil(total * (score / 100) / uses)
        const u64 denominator = 100ull * entry.recent_count;
        entry.frecency = static_cast<i32>(
            minimum<u64>(0x7FFFFFFF, (static_cast<u64>(entry.total_uses) * score + denominator - 1) / denominator));
        entries[kept++] = entry;
    }
    return kept;
}

void command_frecency_track(Slice<CommandFrecencyEntry> entries, u32* count, Utf8View key,
                            u64 now_unix_ms) noexcept {
    if (!count || !key.bytes || key.bytes > kCommandFrecencyKeyBytes || !entries.count)
        return;
    i64 found = find_frecency({entries.data, entries.count}, *count, key);
    if (found < 0) {
        u32 slot = *count;
        if (slot == entries.count) {
            // The least recently used entry goes first.
            slot = 0;
            for (u32 n = 1; n < *count; ++n)
                if (last_use(entries[n]) < last_use(entries[slot]))
                    slot = n;
        } else {
            ++*count;
        }
        auto& entry = entries[slot];
        reset_value(&entry);
        copy_bytes(entry.key, key.data, key.bytes);
        entry.key_bytes = static_cast<u8>(key.bytes);
        found = slot;
    }
    auto& entry = entries[static_cast<u32>(found)];
    ++entry.total_uses;
    if (entry.recent_count == kCommandFrecencyRecentUses) {
        copy_bytes(entry.recent_uses, entry.recent_uses + 1, (kCommandFrecencyRecentUses - 1) * sizeof(u64));
        --entry.recent_count;
    }
    entry.recent_uses[entry.recent_count++] = now_unix_ms;
}

u32 command_frecency_write_patch(ConstSlice<CommandFrecencyEntry> entries, u32 data_version, Slice<u8> scratch,
                                 Slice<u8> json) noexcept {
    const auto item_bytes = [](const CommandFrecencyEntry& entry) noexcept {
        u32 packed = 0;
        for (u32 use = 0; use < entry.recent_count; ++use)
            packed += proto_varint_size(entry.recent_uses[use]);
        const u64 score = (static_cast<u64>(entry.score) + 50) / 100;
        return (entry.total_uses ? 1 + proto_varint_size(entry.total_uses) : 0) +
               (packed ? 1 + proto_varint_size(packed) + packed : 0) +
               (entry.frecency ? 1 + proto_varint_size(static_cast<u64>(static_cast<i64>(entry.frecency))) : 0) +
               (score ? 1 + proto_varint_size(score) : 0);
    };
    u64 map_bytes = 0;
    for (u32 n = 0; n < entries.count; ++n) {
        const auto& entry = entries[n];
        const u32 value = item_bytes(entry);
        const u32 inner = 1 + proto_varint_size(entry.key_bytes) + entry.key_bytes + 1 + proto_varint_size(value) + value;
        map_bytes += 1 + proto_varint_size(inner) + inner;
    }
    if (map_bytes > scratch.count)
        return 0;
    ProtoWriter out{scratch, 0, true};
    out.tag(kFrecencyField, 2);
    out.varint(map_bytes);
    for (u32 n = 0; n < entries.count; ++n) {
        const auto& entry = entries[n];
        const u32 value = item_bytes(entry);
        out.tag(1, 2);
        out.varint(1 + proto_varint_size(entry.key_bytes) + entry.key_bytes + 1 + proto_varint_size(value) + value);
        out.tag(1, 2);
        out.varint(entry.key_bytes);
        out.bytes(entry.key, entry.key_bytes);
        out.tag(2, 2);
        out.varint(value);
        if (entry.total_uses) {
            out.tag(1, 0);
            out.varint(entry.total_uses);
        }
        u32 packed = 0;
        for (u32 use = 0; use < entry.recent_count; ++use)
            packed += proto_varint_size(entry.recent_uses[use]);
        if (packed) {
            out.tag(2, 2);
            out.varint(packed);
            for (u32 use = 0; use < entry.recent_count; ++use)
                out.varint(entry.recent_uses[use]);
        }
        if (entry.frecency) {
            out.tag(3, 0);
            out.varint(static_cast<u64>(static_cast<i64>(entry.frecency)));
        }
        const u64 score = (static_cast<u64>(entry.score) + 50) / 100;
        if (score) {
            out.tag(4, 0);
            out.varint(score);
        }
    }
    if (!out.ok)
        return 0;
    const u32 bytes = proto_settings_patch_bytes(out.at, data_version, true);
    if (!bytes || bytes > json.count || !proto_write_settings_patch({scratch.data, out.at}, data_version, json, true))
        return 0;
    return bytes;
}

bool command_frecency_patch_valid(Utf8View json) noexcept {
    u32 indexes[64];
    const auto scanned = json_scan_structurals_avx1(json, {indexes, 64});
    if (!scanned.ok())
        return false;
    struct Envelope {
        JsonString settings;
        bool seen, other;
    } envelope{};
    if (!json_visit_root_object(json, {indexes, scanned.count}, 4,
                                [](void* opaque, JsonString name, JsonValue value) noexcept {
                                    auto& e = *static_cast<Envelope*>(opaque);
                                    if (json_string_equals_ascii(name, utf8_literal("settings")))
                                        e.seen = json_value_as_string(value, &e.settings) && !e.settings.has_escapes;
                                    else if (!json_string_equals_ascii(name, utf8_literal("required_data_version")))
                                        e.other = true;
                                    return true;
                                },
                                &envelope)
             .ok() ||
        !envelope.seen || envelope.other)
        return false;
    const auto measured = base64_decode(envelope.settings.encoded, {});
    if ((!measured.ok() && measured.error != Base64Error::output_too_small) || !measured.bytes)
        return false;
    const ProtoSource w{{}, envelope.settings.encoded, measured.bytes};
    for (u32 p = 0; p < w.size;) {
        u32 field = 0, type = 0, stop = 0;
        if (!proto_tag(w, p, w.size, field, type) || field != kFrecencyField || type != 2 ||
            !proto_length(w, p, w.size, stop))
            return false;
        p = stop;
    }
    return true;
}
namespace {
constexpr u8 kPendingMagic[] = {'M', 'C', 'C', 'F'};
constexpr u8 kPendingVersion = 1;
}
u32 command_frecency_pending_encode(ConstSlice<CommandFrecencyUse> uses, Slice<u8> output) noexcept {
    if (uses.count > kCommandFrecencyPendingUses || !output.data || output.count < 10)
        return 0;
    u32 used = 0;
    const auto put = [&](const void* data, u32 bytes) noexcept {
        if (bytes > output.count - used)
            return false;
        copy_bytes(output.data + used, data, bytes);
        used += bytes;
        return true;
    };
    const u8 header[] = {kPendingMagic[0], kPendingMagic[1], kPendingMagic[2], kPendingMagic[3], kPendingVersion,
                         static_cast<u8>(uses.count)};
    if (!put(header, sizeof(header)))
        return 0;
    for (u32 n = 0; n < uses.count; ++n) {
        const auto& use = uses[n];
        u8 at[8];
        for (u32 b = 0; b < 8; ++b)
            at[b] = static_cast<u8>(use.at >> (8 * b));
        if (!use.key_bytes || use.key_bytes > kCommandFrecencyKeyBytes || !put(at, 8) || !put(&use.key_bytes, 1) ||
            !put(use.key, use.key_bytes))
            return 0;
    }
    const u32 hash = hash32_bytes(output.data, used);
    const u8 tail[] = {static_cast<u8>(hash), static_cast<u8>(hash >> 8), static_cast<u8>(hash >> 16),
                       static_cast<u8>(hash >> 24)};
    return put(tail, 4) ? used : 0;
}
bool command_frecency_pending_decode(ConstSlice<u8> record, Slice<CommandFrecencyUse> uses, u32* count) noexcept {
    if (!count || !uses.data || !record.data || record.count < 10 || record.count > kCommandFrecencyPendingFileBytes)
        return false;
    const u32 body = record.count - 4;
    const u32 stored = record[body] | (u32{record[body + 1]} << 8) | (u32{record[body + 2]} << 16) |
                       (u32{record[body + 3]} << 24);
    const u32 listed = record[5];
    if (!bytes_equal(record.data, kPendingMagic, 4) || record[4] != kPendingVersion ||
        listed > kCommandFrecencyPendingUses || listed > uses.count || stored != hash32_bytes(record.data, body))
        return false;
    u32 at = 6;
    for (u32 n = 0; n < listed; ++n) {
        if (body - at < 9)
            return false;
        u64 time = 0;
        for (u32 b = 0; b < 8; ++b)
            time |= u64{record[at + b]} << (8 * b);
        const u32 bytes = record[at + 8];
        at += 9;
        if (!bytes || bytes > kCommandFrecencyKeyBytes || bytes > body - at)
            return false;
        uses[n].at = time;
        uses[n].key_bytes = static_cast<u8>(bytes);
        copy_bytes(uses[n].key, record.data + at, bytes);
        at += bytes;
    }
    if (at != body)
        return false;
    *count = listed;
    return true;
}
} // namespace maicord
