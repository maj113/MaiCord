#include "maicord/protocol/message_interaction.hpp"

#include "maicord/core/json_scan.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "json_helpers.hpp"

namespace maicord {
namespace {

using namespace protocol_detail;

constexpr u32 kDepth = 16;

bool append_snowflake_member(ProtocolWriter* output, const char* name, u64 value) noexcept {
    return protocol_writer_append_c_string(output, name) && protocol_writer_append_snowflake(output, value);
}

struct Pool {
    Slice<char> bytes;
    u32 used;
};

bool decode_string(JsonValue value, Pool* pool, Utf8View* output, u32 maximum = 0xFFFFu) noexcept {
    JsonString text{};
    if (value.kind == JsonValueKind::null_value) {
        *output = {};
        return true;
    }
    if (!json_value_as_string(value, &text))
        return false;
    const auto size = json_string_decode_utf8(text, {nullptr, 0});
    const u32 bytes = size.bytes;
    if ((size.error != JsonStringDecodeError::output_too_small && !size.ok()) || bytes > maximum ||
        bytes > pool->bytes.count - pool->used)
        return false;
    if (!bytes) {
        *output = {};
        return true;
    }
    const auto decoded = json_string_decode_utf8(text, {pool->bytes.data + pool->used, bytes});
    if (!decoded.ok())
        return false;
    *output = {pool->bytes.data + pool->used, decoded.bytes};
    pool->used += decoded.bytes;
    return true;
}

struct EventContext {
    MessageInteractionEvent event;
    bool id_seen;
};

bool visit_event(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<EventContext*>(opaque);
    if (name_is(name, "id"))
        return (context->id_seen = json_value_to_u64_or_string(value, &context->event.id));
    if (name_is(name, "nonce")) {
        // Other clients may use opaque nonces; only numeric ones can match ours.
        if (!json_value_to_u64_or_string(value, &context->event.nonce))
            context->event.nonce = 0;
        return true;
    }
    return true;
}

struct ModalContext {
    JsonIndexedDocument document;
    Pool pool;
    MessageModal* modal;
    JsonValue components;
    JsonValue application;
    bool components_seen;
    bool application_seen;
};

struct InputContext {
    ModalContext* modal;
    MessageModalField field;
    u64 type;
    JsonValue inner;
    bool inner_seen;
    Utf8View content;
    JsonValue children;
    bool children_seen;
};

bool visit_input(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<InputContext*>(opaque);
    Pool* pool = &context->modal->pool;
    MessageModalField& field = context->field;
    u64 number = 0;
    if (name_is(name, "type"))
        return json_value_to_u64(value, &context->type);
    if (name_is(name, "id")) {
        if (json_value_to_u64(value, &number) && number <= 0xFFFFFFFFu)
            field.id = static_cast<u32>(number);
        return true;
    }
    if (name_is(name, "custom_id"))
        return decode_string(value, pool, &field.custom_id, kMessageInteractionCustomIdMaximumBytes);
    if (name_is(name, "label"))
        return decode_string(value, pool, &field.label, 512);
    if (name_is(name, "description"))
        return decode_string(value, pool, &field.description, 512);
    if (name_is(name, "placeholder"))
        return decode_string(value, pool, &field.placeholder, 512);
    if (name_is(name, "value"))
        return decode_string(value, pool, &field.value, kMessageModalValueMaximumBytes);
    if (name_is(name, "content"))
        return decode_string(value, pool, &context->content, 4096);
    if (name_is(name, "style")) {
        if (!json_value_to_u64(value, &number))
            return false;
        field.style = number == 2 ? MessageModalInputStyle::paragraph : MessageModalInputStyle::short_text;
        return true;
    }
    if (name_is(name, "min_length") || name_is(name, "max_length")) {
        if (value.kind == JsonValueKind::null_value)
            return true;
        if (!json_value_to_u64(value, &number) || number > 4000)
            return false;
        (name_is(name, "min_length") ? field.min_length : field.max_length) = static_cast<u16>(number);
        return true;
    }
    if (name_is(name, "required"))
        return value.kind == JsonValueKind::null_value || json_value_to_bool(value, &field.required);
    if (name_is(name, "component") && value.kind == JsonValueKind::object) {
        context->inner = value;
        context->inner_seen = true;
        return true;
    }
    if (name_is(name, "components") && value.kind == JsonValueKind::array) {
        context->children = value;
        context->children_seen = true;
    }
    return true;
}

bool add_field(ModalContext* context, const MessageModalField& field) noexcept {
    MessageModal& modal = *context->modal;
    if (modal.field_count >= kMessageModalMaximumFields || !field.custom_id.bytes)
        return false;
    modal.fields[modal.field_count++] = field;
    return true;
}

bool parse_input(ModalContext* context, JsonValue value, InputContext* output) noexcept {
    *output = InputContext{};
    output->modal = context;
    output->field.required = true; // The API's default for text inputs
    output->field.max_length = 4000;
    return json_visit_indexed_object(context->document, value, kDepth, visit_input, output).ok();
}

bool visit_row_child(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<ModalContext*>(opaque);
    InputContext input{};
    if (!parse_input(context, value, &input))
        return false;
    if (input.type != 4) {
        context->modal->unsupported = true;
        return true;
    }
    return add_field(context, input.field);
}

bool visit_root_component(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<ModalContext*>(opaque);
    InputContext outer{};
    if (!parse_input(context, value, &outer))
        return false;
    MessageModal& modal = *context->modal;
    switch (outer.type) {
    case 1: // action row of inputs
        return !outer.children_seen ||
               json_visit_indexed_array(context->document, outer.children, kDepth, visit_row_child, context).ok();
    case 10: // text display
        if (outer.content.bytes && modal.paragraph_count < kMessageModalMaximumParagraphs)
            modal.paragraphs[modal.paragraph_count++] = outer.content;
        return true;
    case 18: { // label wrapping one input
        if (!outer.inner_seen)
            return false;
        InputContext inner{};
        if (!parse_input(context, outer.inner, &inner))
            return false;
        if (inner.type != 4) {
            modal.unsupported = true;
            return true;
        }
        inner.field.label = outer.field.label;
        inner.field.description = outer.field.description;
        inner.field.labelled = true;
        return add_field(context, inner.field);
    }
    default:
        modal.unsupported = true;
        return true;
    }
}

struct ApplicationContext {
    ModalContext* modal;
};

bool visit_application(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ApplicationContext*>(opaque);
    if (name_is(name, "id"))
        return json_value_to_u64_or_string(value, &context->modal->modal->application_id);
    if (name_is(name, "name"))
        return decode_string(value, &context->modal->pool, &context->modal->modal->application_name, 256);
    return true;
}

bool visit_modal(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ModalContext*>(opaque);
    MessageModal& modal = *context->modal;
    if (name_is(name, "id"))
        return json_value_to_u64_or_string(value, &modal.interaction_id);
    if (name_is(name, "nonce")) {
        if (!json_value_to_u64_or_string(value, &modal.nonce))
            modal.nonce = 0;
        return true;
    }
    if (name_is(name, "channel_id"))
        return json_value_to_u64_or_string(value, &modal.channel_id);
    if (name_is(name, "custom_id"))
        return decode_string(value, &context->pool, &modal.custom_id, kMessageInteractionCustomIdMaximumBytes);
    if (name_is(name, "title"))
        return decode_string(value, &context->pool, &modal.title, 256);
    if (name_is(name, "components")) {
        if (value.kind != JsonValueKind::array)
            return false;
        context->components = value;
        context->components_seen = true;
    } else if (name_is(name, "application") && value.kind == JsonValueKind::object) {
        context->application = value;
        context->application_seen = true;
    }
    return true;
}

} // namespace

bool message_interaction_request_valid(const MessageInteractionRequest& request) noexcept {
    if (!request.application_id || !request.channel_id || !request.nonce || !request.data_bytes ||
        request.data_bytes > kMessageInteractionDataMaximumBytes || request.data[0] != '{' ||
        request.data[request.data_bytes - 1u] != '}' || !utf8_validate({request.data, request.data_bytes}).valid)
        return false;
    for (u8 byte : request.reserved)
        if (byte)
            return false;
    if (request.type == MessageInteractionType::message_component)
        return request.message_id != 0;
    return request.type == MessageInteractionType::modal_submit;
}

bool message_interaction_write_body(const MessageInteractionRequest& request, Utf8View session_id,
                                    ProtocolWriter* output) noexcept {
    if (!output || !message_interaction_request_valid(request) || !session_id.bytes)
        return false;
    const bool component = request.type == MessageInteractionType::message_component;
    bool ok = protocol_writer_append_literal(output, "{\"type\":") &&
              protocol_writer_append_u64(output, static_cast<u64>(request.type)) &&
              append_snowflake_member(output, ",\"nonce\":", request.nonce) &&
              (!request.guild_id || append_snowflake_member(output, ",\"guild_id\":", request.guild_id)) &&
              append_snowflake_member(output, ",\"channel_id\":", request.channel_id);
    if (component)
        ok = ok && protocol_writer_append_literal(output, ",\"message_flags\":") &&
             protocol_writer_append_u64(output, request.message_flags) &&
             append_snowflake_member(output, ",\"message_id\":", request.message_id);
    return ok && append_snowflake_member(output, ",\"application_id\":", request.application_id) &&
           protocol_writer_append_literal(output, ",\"session_id\":") && protocol_writer_append_json_string(output, session_id) &&
           protocol_writer_append_literal(output, ",\"data\":") &&
           protocol_writer_append_bytes(output, reinterpret_cast<const u8*>(request.data), request.data_bytes) &&
           protocol_writer_append_byte(output, '}');
}

bool message_interaction_write_component_data(u16 component_type, Utf8View custom_id,
                                              ConstSlice<Utf8View> values, ProtocolWriter* output) noexcept {
    if (!output || !component_type || !custom_id.bytes || custom_id.bytes > kMessageInteractionCustomIdMaximumBytes ||
        values.count > kMessageInteractionMaximumValues || (!values.data && values.count))
        return false;
    const bool select = component_type == 3 || (component_type >= 5 && component_type <= 8);
    bool ok = protocol_writer_append_literal(output, "{\"component_type\":") &&
              protocol_writer_append_u64(output, component_type) &&
              protocol_writer_append_literal(output, ",\"custom_id\":") && protocol_writer_append_json_string(output, custom_id);
    if (select) {
        ok = ok && protocol_writer_append_literal(output, ",\"type\":") &&
             protocol_writer_append_u64(output, component_type) &&
             protocol_writer_append_literal(output, ",\"values\":[");
        for (u32 i = 0; ok && i < values.count; ++i)
            ok = (i == 0 || protocol_writer_append_byte(output, ',')) && protocol_writer_append_json_string(output, values[i]);
        ok = ok && protocol_writer_append_byte(output, ']');
    } else if (values.count) {
        return false;
    }
    return ok && protocol_writer_append_byte(output, '}');
}

bool message_interaction_event_decode(Utf8View json, Slice<u32> structurals,
                                      MessageInteractionEvent* output) noexcept {
    if (!output || !json.data || !json.bytes || !structurals.data)
        return false;
    const auto scan = json_scan_structurals_avx1(json, structurals);
    if (!scan.ok())
        return false;
    EventContext context{};
    if (!json_visit_root_object(json, {structurals.data, scan.count}, kDepth, visit_event, &context).ok() ||
        !context.id_seen)
        return false;
    *output = context.event;
    return true;
}

bool message_modal_decode(Utf8View json, Slice<u32> structurals, Slice<char> pool,
                          MessageModal* output) noexcept {
    if (!output || !json.data || !json.bytes || !structurals.data || !pool.data)
        return false;
    const auto scan = json_scan_structurals_avx1(json, structurals);
    if (!scan.ok())
        return false;
    MessageModal modal{};
    ModalContext context{{json, {structurals.data, scan.count}}, {pool, 0}, &modal, {}, {}, false, false};
    if (!json_visit_root_object(json, context.document.structurals, kDepth, visit_modal, &context).ok() ||
        !context.components_seen || !modal.interaction_id || !modal.custom_id.bytes)
        return false;
    ApplicationContext application{&context};
    if (context.application_seen &&
        !json_visit_indexed_object(context.document, context.application, kDepth, visit_application, &application)
             .ok())
        return false;
    if (!json_visit_indexed_array(context.document, context.components, kDepth, visit_root_component, &context)
             .ok() ||
        (!modal.field_count && !modal.unsupported))
        return false;
    *output = modal;
    return true;
}

bool message_modal_value_valid(const MessageModalField& field, Utf8View value, Utf8View* reason) noexcept {
    const auto validated = utf8_validate(value);
    if (!validated.valid) {
        if (reason)
            *reason = utf8_literal("Text contains invalid characters");
        return false;
    }
    // Lengths count characters (UTF-16 code units server-side); scalars are a
    // close, conservative proxy for the limits shown to the user.
    u32 characters = 0;
    for (u32 i = 0; i < value.bytes; ++i)
        characters += (static_cast<u8>(value.data[i]) & 0xC0u) != 0x80u ? 1u : 0u;
    if (!characters) {
        if (field.required) {
            if (reason)
                *reason = utf8_literal("This field is required");
            return false;
        }
        return true;
    }
    if (field.min_length && characters < field.min_length) {
        if (reason)
            *reason = utf8_literal("Too short");
        return false;
    }
    if (field.max_length && characters > field.max_length) {
        if (reason)
            *reason = utf8_literal("Too long");
        return false;
    }
    return true;
}

bool message_modal_write_submit_data(const MessageModal& modal, ConstSlice<Utf8View> values,
                                     ProtocolWriter* output) noexcept {
    if (!output || modal.unsupported || !modal.interaction_id || !modal.custom_id.bytes ||
        values.count != modal.field_count || (!values.data && values.count))
        return false;
    bool ok = append_snowflake_member(output, "{\"id\":", modal.interaction_id) &&
              protocol_writer_append_literal(output, ",\"custom_id\":") && protocol_writer_append_json_string(output, modal.custom_id) &&
              protocol_writer_append_literal(output, ",\"components\":[");
    for (u32 i = 0; ok && i < modal.field_count; ++i) {
        const MessageModalField& field = modal.fields[i];
        if (!message_modal_value_valid(field, values[i], nullptr))
            return false;
        ok = (i == 0 || protocol_writer_append_byte(output, ',')) &&
             protocol_writer_append_c_string(output, field.labelled ? "{\"type\":18,\"component\":"
                                                                    : "{\"type\":1,\"components\":[") &&
             protocol_writer_append_literal(output, "{\"type\":4,") &&
             (!field.id || (protocol_writer_append_literal(output, "\"id\":") &&
                            protocol_writer_append_u64(output, field.id) && protocol_writer_append_byte(output, ','))) &&
             protocol_writer_append_literal(output, "\"custom_id\":") && protocol_writer_append_json_string(output, field.custom_id) &&
             protocol_writer_append_literal(output, ",\"value\":") && protocol_writer_append_json_string(output, values[i]) &&
             protocol_writer_append_byte(output, '}') &&
             protocol_writer_append_c_string(output, field.labelled ? "}" : "]}");
    }
    return ok && protocol_writer_append_literal(output, "]}");
}

} // namespace maicord
