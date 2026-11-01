#include "ui_internal.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/protocol/encoding/writer.hpp"

// Slash commands: the composer's command index built from the enabled plugins' declared
// commands, and running one with the options the composer collected.
namespace maicord {
using namespace plugin_ui_detail;
namespace {

static_assert(MC_ARG_TEXT == 1 && MC_ARG_INT == 2 && MC_ARG_BOOL == 3 && MC_ARG_USER == 4 && MC_ARG_CHANNEL == 5);

// A command's id: its owner's id with the low byte replaced by its 1-based position. Ids are unique across plugins
// as long as no two owners agree in their upper 56 bits.
constexpr u64 command_id(u64 owner_id, u32 item) noexcept {
    return (owner_id & ~0xFFull) | (item + 1);
}
constexpr u64 application_id(u64 owner_id) noexcept {
    return owner_id | kPluginCommandApplicationFlag;
}

// Discord's application command option types.
constexpr u32 option_type(u32 type) noexcept {
    constexpr u32 types[] = {0, 3, 4, 5, 6, 7};
    return types[type];
}

Utf8View view(mc_str text) noexcept {
    return {text.data, text.bytes};
}

bool write_argument(ProtocolWriter* w, const mc_arg_def& arg) noexcept {
    const Utf8View name = view(arg.name), description = view(arg.description);
    if (!protocol_write(w, "{\"name\":", json_text(name), ",\"description\":", json_text(description), ",\"type\":",
                        option_type(arg.type)))
        return false;
    if ((arg.flags & MC_ARG_REQUIRED) && !protocol_write(w, ",\"required\":true")) return false;
    if (arg.type == MC_ARG_TEXT) {
        if (arg.minimum > 0 && !protocol_write(w, ",\"min_length\":", static_cast<u64>(arg.minimum))) return false;
        if (arg.maximum > 0 && !protocol_write(w, ",\"max_length\":", static_cast<u64>(arg.maximum))) return false;
    } else if ((arg.type == MC_ARG_INT) && (arg.minimum || arg.maximum)) {
        // Discord's JSON carries signed bounds; negative values are written by hand.
        const auto bound = [&](const char* key, i64 value) noexcept {
            if (!protocol_write(w, ",\"", key, "\":")) return false;
            if (value < 0 && !protocol_write(w, '-')) return false;
            const u64 magnitude = value < 0 ? ~static_cast<u64>(value) + 1 : static_cast<u64>(value);
            return protocol_write(w, magnitude);
        };
        if (!bound("min_value", arg.minimum) || !bound("max_value", arg.maximum)) return false;
    }
    if (arg.choice_count) {
        if (!protocol_write(w, ",\"choices\":[")) return false;
        for (u32 i = 0; i < arg.choice_count; ++i) {
            const Utf8View choice = view(arg.choices[i]);
            if (!protocol_write(w, i ? "," : "", "{\"name\":", json_text(choice), ",\"value\":", json_text(choice), '}'))
                return false;
        }
        if (!protocol_write(w, ']')) return false;
    }
    return protocol_write(w, '}');
}

bool write_command(ProtocolWriter* w, const PluginBinding& binding, u32 item) noexcept {
    const mc_command_def& def = descriptor_of(binding).commands[item];
    if (!protocol_write(w, "{\"id\":", snowflake_text(command_id(binding.owner_id, item)), ",\"application_id\":",
                        snowflake_text(application_id(binding.owner_id)), ",\"version\":\"1\",\"type\":1,\"name\":",
                        json_text(view(def.name)), ",\"description\":", json_text(view(def.description)),
                        (def.flags & MC_COMMAND_GUILD_ONLY) ? ",\"dm_permission\":false" : "", ",\"options\":["))
        return false;
    for (u32 i = 0; i < def.arg_count; ++i)
        if ((i && !protocol_write(w, ',')) || !write_argument(w, def.args[i])) return false;
    return protocol_write(w, "]}");
}

// ---- Running a command ---------------------------------------------------------------

// Text values decode into this; one command's arguments share it.
constexpr u32 kTextBytes = 16384;
char g_text[kTextBytes];
char g_reply[MC_REPLY_TEXT_MAX];

struct Arguments {
    const JsonIndexedDocument* document;
    const mc_command_def* def;
    mc_arg values[MC_COMMAND_ARGS_MAX];
    u32 text_used;
    bool failed;
};

bool name_is(const JsonString& name, const char* expected) noexcept {
    return json_string_equals_ascii_bytes(name.encoded.data, name.encoded.bytes, expected, c_string_length(expected));
}

// One option object: {"name","type","value"}.
struct Option {
    JsonString name;
    JsonValue value;
    bool has_name, has_value;
};

bool visit_member(void* context, JsonString name, JsonValue value) noexcept {
    Option& option = *static_cast<Option*>(context);
    if (name_is(name, "name") && json_value_as_string(value, &option.name)) option.has_name = true;
    else if (name_is(name, "value")) {
        option.value = value;
        option.has_value = true;
    }
    return true;
}

bool parse_integer(Utf8View text, i64* output) noexcept {
    u32 at = 0;
    const bool negative = text.bytes && text.data[0] == '-';
    if (negative) ++at;
    if (at == text.bytes || text.bytes - at > 15) return false;
    i64 value = 0;
    for (; at < text.bytes; ++at) {
        if (text.data[at] < '0' || text.data[at] > '9') return false;
        value = value * 10 + (text.data[at] - '0');
    }
    *output = negative ? -value : value;
    return true;
}

bool fill(Arguments& arguments, u32 index, const Option& option) noexcept {
    const mc_arg_def& def = arguments.def->args[index];
    mc_arg& value = arguments.values[index];
    switch (def.type) {
    case MC_ARG_TEXT: {
        JsonString text;
        if (!json_value_as_string(option.value, &text)) return false;
        const JsonStringDecodeResult decoded = json_string_decode_utf8(text, {g_text + arguments.text_used, kTextBytes - arguments.text_used});
        if (!decoded.ok()) return false;
        value.text = {g_text + arguments.text_used, decoded.bytes, 0};
        arguments.text_used += decoded.bytes;
        break;
    }
    case MC_ARG_INT: {
        i64 number = 0;
        if (option.value.kind != JsonValueKind::number || !parse_integer(option.value.source, &number)) return false;
        value.integer = number;
        break;
    }
    case MC_ARG_BOOL: {
        bool flag = false;
        if (!json_value_to_bool(option.value, &flag)) return false;
        value.integer = flag ? 1 : 0;
        break;
    }
    default: {
        u64 id = 0;
        JsonString text;
        const bool parsed = option.value.kind == JsonValueKind::string
                                ? json_value_as_string(option.value, &text) && json_string_to_u64(text, &id)
                                : json_value_to_u64(option.value, &id);
        if (!parsed || !id) return false;
        value.id = id;
        break;
    }
    }
    value.present = 1;
    return true;
}

bool visit_option(void* context, u32, JsonValue item) noexcept {
    Arguments& arguments = *static_cast<Arguments*>(context);
    Option option{};
    if (!json_visit_indexed_object(*arguments.document, item, 4, visit_member, &option).ok() || !option.has_name ||
        !option.has_value) {
        arguments.failed = true;
        return false;
    }
    for (u32 i = 0; i < arguments.def->arg_count; ++i) {
        const mc_str expected = arguments.def->args[i].name;
        if (!json_string_equals_ascii_bytes(option.name.encoded.data, option.name.encoded.bytes, expected.data,
                                            expected.bytes))
            continue;
        if (!fill(arguments, i, option)) arguments.failed = true;
        return !arguments.failed;
    }
    // An option the command does not declare was not typed by the composer.
    arguments.failed = true;
    return false;
}

struct Run {
    const mc_command_def* def;
    mc_command_call* call;
};

mc_status invoke_run(mc_ctx* context, void* argument) noexcept {
    Run& run = *static_cast<Run*>(argument);
    return run.def->run(context, run.call);
}

} // namespace

u32 plugin_command_count() noexcept {
    u32 count = 0;
    for (u32 i = 0; i < g_slot_count; ++i)
        if (callable(*g_slots[i].binding)) count += descriptor_of(*g_slots[i].binding).command_count;
    return count;
}

u32 plugin_command_index_json(Slice<char> output) noexcept {
    if (!plugin_command_count()) return 0;
    ProtocolWriter w{{reinterpret_cast<u8*>(output.data), output.count}, 0};
    if (!protocol_write(&w, "{\"applications\":[")) return 0;
    bool comma = false;
    for (u32 slot = 0; slot < g_slot_count; ++slot) {
        const PluginBinding& binding = *g_slots[slot].binding;
        const mc_descriptor& descriptor = descriptor_of(binding);
        if (!callable(binding) || !descriptor.command_count) continue;
        if (!protocol_write(&w, comma ? "," : "", "{\"id\":", snowflake_text(application_id(binding.owner_id)),
                            ",\"name\":", json_text(view(descriptor.name)), '}'))
            return 0;
        comma = true;
    }
    if (!protocol_write(&w, "],\"application_commands\":[")) return 0;
    comma = false;
    for (u32 slot = 0; slot < g_slot_count; ++slot) {
        const PluginBinding& binding = *g_slots[slot].binding;
        if (!callable(binding)) continue;
        for (u32 item = 0; item < descriptor_of(binding).command_count; ++item) {
            if ((comma && !protocol_write(&w, ',')) || !write_command(&w, binding, item)) return 0;
            comma = true;
        }
    }
    return protocol_write(&w, "]}") ? w.used : 0;
}

PluginCommandResult plugin_command_run(u64 application, u64 command, u64 channel_id, u64 guild_id,
                                       Utf8View options_json, Slice<u32> scratch) noexcept {
    PluginCommandResult result{PluginCommandOutcome::gone, 0, 0, {}};
    PluginBinding* binding = nullptr;
    for (u32 i = 0; i < g_slot_count && !binding; ++i)
        if (application_id(g_slots[i].binding->owner_id) == application) binding = g_slots[i].binding;
    if (!binding || !callable(*binding)) return result;
    const mc_descriptor& descriptor = descriptor_of(*binding);
    const u32 item = static_cast<u32>(command & 0xFF) - 1;
    if (!(command & 0xFF) || item >= descriptor.command_count || command_id(binding->owner_id, item) != command)
        return result;
    result.owner_id = binding->owner_id;
    result.outcome = PluginCommandOutcome::failed;
    const mc_command_def& def = descriptor.commands[item];

    Arguments arguments{};
    arguments.def = &def;
    for (u32 i = 0; i < def.arg_count; ++i) arguments.values[i].type = def.args[i].type;
    if (options_json.bytes) {
        const JsonScanResult scan = json_scan_structurals_avx1(options_json, scratch);
        if (!scan.ok()) return result;
        const JsonIndexedDocument document{options_json, {scratch.data, scan.count}};
        arguments.document = &document;
        const JsonValue root{options_json, 0, JsonValueKind::array};
        if (!json_visit_indexed_array(document, root, 8, visit_option, &arguments).ok() || arguments.failed) return result;
    }
    // Required arguments the composer should have enforced; the plugin relies on them.
    for (u32 i = 0; i < def.arg_count; ++i)
        if ((def.args[i].flags & MC_ARG_REQUIRED) && !arguments.values[i].present) return result;

    mc_command_call call{};
    call.channel_id = channel_id;
    call.guild_id = guild_id;
    call.args = arguments.values;
    call.arg_count = def.arg_count;
    call.reply = g_reply;
    call.reply_capacity = sizeof(g_reply);
    Run run{&def, &call};
    const PluginCallResult ran = plugin_binding_call(binding, PluginCallKind::command, PluginCallClass::gesture,
                                                     MC_CTX_GESTURE, invoke_run, &run);
    if (ran.overran || ran.status != MC_OK) return result;
    const Utf8View text{g_reply, call.reply_bytes};
    const bool has_text = call.reply_bytes && call.reply_bytes <= sizeof(g_reply) && utf8_validate(text).valid;
    switch (call.reply_kind) {
    case MC_REPLY_NONE:
        result.outcome = PluginCommandOutcome::none;
        return result;
    case MC_REPLY_SEND:
        if (!has_text || !(binding->permissions & MC_PERM_MESSAGE_SEND) || call.reply_bytes > MC_DRAFT_TEXT_MAX)
            return result;
        result.outcome = PluginCommandOutcome::send;
        break;
    case MC_REPLY_INSERT: result.outcome = PluginCommandOutcome::insert; break;
    case MC_REPLY_LOCAL: result.outcome = PluginCommandOutcome::local; break;
    case MC_REPLY_TOAST: result.outcome = PluginCommandOutcome::toast; break;
    default: return result;
    }
    if (!has_text) {
        result.outcome = PluginCommandOutcome::failed;
        return result;
    }
    result.text = text;
    result.tone = call.reply_tone <= MC_TONE_CRITICAL ? call.reply_tone : MC_TONE_NEUTRAL;
    return result;
}

} // namespace maicord
