#include "maicord/protocol/user_social.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/rest.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {
constexpr u32 depth_limit = 32;

struct Member {
    Utf8View name;
    JsonValue value;
    bool found;
};
bool find_member(void* p, JsonString name, JsonValue value) noexcept {
    auto& m = *static_cast<Member*>(p);
    if (!m.found && json_string_equals_utf8(name, m.name)) {
        m.value = value;
        m.found = true;
    }
    return true;
}
JsonValue field(const JsonIndexedDocument& doc, const JsonValue& object, const char* name) noexcept {
    u32 bytes = 0;
    while (name[bytes])
        ++bytes;
    Member member{{name, bytes}, {}, false};
    if (object.source.bytes && object.kind == JsonValueKind::object)
        json_visit_indexed_object(doc, object, depth_limit, find_member, &member);
    return member.value;
}
} // namespace

Utf8View report_menu_name(ReportMenuName name) noexcept {
    switch (name) {
    case ReportMenuName::message:
        return utf8_literal("message");
    case ReportMenuName::user:
        return utf8_literal("user");
    default:
        return {};
    }
}

bool user_note_valid(Utf8View note) noexcept {
    if ((!note.data && note.bytes) || note.bytes > kUserNoteMaximumBytes)
        return false;
    const auto validation = utf8_validate(note);
    return validation.valid && validation.codepoints <= kUserNoteMaximumCodepoints;
}

bool user_social_request_valid(const UserSocialRequest& r) noexcept {
    switch (r.operation) {
    case UserSocialOperation::note_get:
        return r.user_id != 0;
    case UserSocialOperation::note_put:
        return r.user_id != 0 && user_note_valid(r.text);
    case UserSocialOperation::report_menu:
        return r.report < ReportMenuName::count;
    case UserSocialOperation::report_submit:
        return r.report < ReportMenuName::count && r.text.data && r.text.bytes >= 2 &&
               r.text.bytes <= kUserSocialMaximumBodyBytes && r.text.data[0] == '{' &&
               r.text.data[r.text.bytes - 1] == '}' && utf8_validate(r.text).valid;
    case UserSocialOperation::message_flags:
        return r.channel_id && r.message_id;
    default:
        return false;
    }
}

RestMethod user_social_method(UserSocialOperation operation) noexcept {
    switch (operation) {
    case UserSocialOperation::note_put:
        return RestMethod::put;
    case UserSocialOperation::report_submit:
        return RestMethod::post;
    case UserSocialOperation::message_flags:
        return RestMethod::patch;
    default:
        return RestMethod::get;
    }
}

bool user_social_has_body(UserSocialOperation operation) noexcept {
    return operation == UserSocialOperation::note_put || operation == UserSocialOperation::report_submit ||
           operation == UserSocialOperation::message_flags;
}

RestRouteKey user_social_route(const UserSocialRequest& r) noexcept {
    switch (r.operation) {
    case UserSocialOperation::note_get:
    case UserSocialOperation::note_put:
        return {kUserSocialRouteBase + 1, 0, user_social_method(r.operation), RestMajorParameter::none};
    case UserSocialOperation::report_menu:
        return {kUserSocialRouteBase + 2, 0, RestMethod::get, RestMajorParameter::none};
    case UserSocialOperation::report_submit:
        return {kUserSocialRouteBase + 3, 0, RestMethod::post, RestMajorParameter::none};
    default:
        // Message edits share the channel's message bucket.
        return {kUserSocialRouteBase + 4, r.channel_id, RestMethod::patch, RestMajorParameter::channel};
    }
}

UserSocialWriteResult user_social_write_target(const UserSocialRequest& r, Slice<u8> output) noexcept {
    if (!user_social_request_valid(r) || !output.data)
        return {0, UserSocialWriteError::input};
    u8 staged[kUserSocialMaximumTargetBytes];
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    bool ok = protocol_writer_append_literal(&w, "/api/v9");
    switch (r.operation) {
    case UserSocialOperation::note_get:
    case UserSocialOperation::note_put:
        ok = ok && protocol_writer_append_literal(&w, "/users/@me/notes/") && protocol_writer_append_u64(&w, r.user_id);
        break;
    case UserSocialOperation::report_menu:
    case UserSocialOperation::report_submit: {
        const auto name = report_menu_name(r.report);
        ok = ok &&
             (r.operation == UserSocialOperation::report_menu ? protocol_writer_append_literal(&w, "/reporting/menu/")
                                                              : protocol_writer_append_literal(&w, "/reporting/")) &&
             protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(name.data), name.bytes);
        break;
    }
    default:
        ok = ok && protocol_writer_append_literal(&w, "/channels/") && protocol_writer_append_u64(&w, r.channel_id) &&
             protocol_writer_append_literal(&w, "/messages/") && protocol_writer_append_u64(&w, r.message_id);
        break;
    }
    return protocol_writer_finish<UserSocialWriteResult>(w, ok, output, staged);
}

UserSocialWriteResult user_social_write_body(const UserSocialRequest& r, Slice<u8> output) noexcept {
    if (!user_social_request_valid(r) || !output.data || !user_social_has_body(r.operation))
        return {0, UserSocialWriteError::input};
    if (r.operation == UserSocialOperation::report_submit) {
        if (r.text.bytes > output.count)
            return {0, UserSocialWriteError::capacity};
        copy_bytes(output.data, r.text.data, r.text.bytes);
        return {r.text.bytes, UserSocialWriteError::none};
    }
    u8 staged[kUserNoteMaximumBytes * 6 + 64];
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    bool ok = false;
    if (r.operation == UserSocialOperation::note_put)
        ok = protocol_writer_append_literal(&w, "{\"note\":") && protocol_writer_append_json_string(&w, r.text) &&
             protocol_writer_append_byte(&w, '}');
    else
        ok = protocol_writer_append_literal(&w, "{\"flags\":") && protocol_writer_append_u64(&w, r.flags) &&
             protocol_writer_append_byte(&w, '}');
    return protocol_writer_finish<UserSocialWriteResult>(w, ok, output, staged);
}

bool user_note_decode(Utf8View body, Slice<u32> scratch, bool gateway, UserNoteRecord* output) noexcept {
    if (!body.data || !body.bytes)
        return false;
    const auto scan = json_scan_structurals_avx1(body, scratch);
    if (!scan.ok())
        return false;
    const JsonIndexedDocument doc{body, {scratch.data, scan.count}};
    const JsonValue root{body, 0, JsonValueKind::object};
    if (body.data[0] != '{')
        return false;
    UserNoteRecord record{};
    if (!json_value_to_u64_or_string(field(doc, root, gateway ? "id" : "note_user_id"), &record.user_id) ||
        !record.user_id)
        return false;
    const auto note = field(doc, root, "note");
    JsonString string{};
    if (json_value_as_string(note, &string)) {
        const auto decoded = json_string_decode_utf8(string, {record.text, sizeof(record.text)});
        if (!decoded.ok() || !user_note_valid({record.text, decoded.bytes}))
            return false;
        record.bytes = decoded.bytes;
    } else if (note.source.bytes && note.kind != JsonValueKind::null_value)
        return false;
    copy_bytes(output, &record, static_cast<u32>(sizeof(record)));
    return true;
}

namespace {
struct Decoder {
    ReportMenu* menu;
    bool counting;
    u32 nodes, children, elements, options;
    bool failed;
};
Utf8View text(ReportMenu& menu, const JsonValue& value) noexcept {
    JsonString string{};
    if (!json_value_as_string(value, &string))
        return {};
    if (!string.has_escapes)
        return string.encoded;
    const auto size = json_string_decode_utf8(string, {});
    if (size.error != JsonStringDecodeError::output_too_small && size.error != JsonStringDecodeError::none)
        return {};
    char* decoded = arena_allocate<char>(&menu.storage, maximum(1u, size.bytes));
    if (!decoded)
        return {};
    const auto result = json_string_decode_utf8(string, {decoded, size.bytes});
    return result.ok() ? Utf8View{decoded, result.bytes} : Utf8View{};
}
Utf8View named(ReportMenu& menu, const JsonValue& object, const char* key) noexcept {
    return text(menu, field(menu.document, object, key));
}
// A JSON array of [a, b, c?] tuples; positions beyond two are optional.
struct Tuple {
    JsonValue values[3];
    u32 count;
};
bool tuple_item(void* p, u32 index, JsonValue value) noexcept {
    auto& t = *static_cast<Tuple*>(p);
    if (index < 3) {
        t.values[index] = value;
        t.count = index + 1;
    }
    return true;
}
Tuple tuple(ReportMenu& menu, JsonValue value) noexcept {
    Tuple t{};
    if (value.kind == JsonValueKind::array)
        json_visit_indexed_array(menu.document, value, depth_limit, tuple_item, &t);
    return t;
}
struct ListContext {
    Decoder* decoder;
    ReportNode* node;
    ReportElement* element;
};
bool child_item(void* p, u32, JsonValue value) noexcept {
    auto& c = *static_cast<ListContext*>(p);
    auto& d = *c.decoder;
    const Tuple t = tuple(*d.menu, value);
    u64 target = 0;
    if (t.count < 2 || !json_value_to_u64_or_string(t.values[1], &target) || target > 0xFFFFFFFFull)
        return true; // unknown shapes are skipped, never fatal
    if (d.children >= kReportMaximumChildren) {
        d.failed = true;
        return false;
    }
    if (!d.counting) {
        d.menu->children[d.children] = {text(*d.menu, t.values[0]), static_cast<u32>(target)};
        ++c.node->child_count;
    }
    ++d.children;
    return true;
}
bool option_item(void* p, u32, JsonValue value) noexcept {
    auto& c = *static_cast<ListContext*>(p);
    auto& d = *c.decoder;
    const Tuple t = tuple(*d.menu, value);
    if (t.count < 2 || (c.element && c.element->option_count >= 64))
        return true;
    if (d.options >= kReportMaximumOptions) {
        d.failed = true;
        return false;
    }
    if (!d.counting) {
        auto& option = d.menu->options[d.options];
        option.value = text(*d.menu, t.values[0]);
        option.label = text(*d.menu, t.values[1]);
        option.description = t.count > 2 ? text(*d.menu, t.values[2]) : Utf8View{};
        if (!option.value.bytes)
            return true;
        ++c.element->option_count;
    }
    ++d.options;
    return true;
}
ReportElementKind element_kind(JsonValue value) noexcept {
    JsonString s{};
    if (!json_value_as_string(value, &s))
        return ReportElementKind::unknown;
    struct Name {
        const char* text;
        ReportElementKind kind;
    };
    static constexpr Name names[]{{"checkbox", ReportElementKind::checkbox},
                           {"text", ReportElementKind::text},
                           {"free_text", ReportElementKind::free_text},
                           {"external_link", ReportElementKind::external_link},
                           {"breadcrumbs", ReportElementKind::breadcrumbs},
                           {"message_preview", ReportElementKind::message_preview},
                           {"user_preview", ReportElementKind::user_preview},
                           {"success", ReportElementKind::success},
                           {"fail", ReportElementKind::fail}};
    for (const auto& name : names) {
        u32 bytes = 0;
        while (name.text[bytes])
            ++bytes;
        if (json_string_equals_ascii(s, {name.text, bytes}))
            return name.kind;
    }
    return ReportElementKind::unknown;
}
bool element_item(void* p, u32, JsonValue value) noexcept {
    auto& c = *static_cast<ListContext*>(p);
    auto& d = *c.decoder;
    if (value.kind != JsonValueKind::object)
        return true;
    if (d.elements >= kReportMaximumElements) {
        d.failed = true;
        return false;
    }
    auto& menu = *d.menu;
    const auto kind = element_kind(field(menu.document, value, "type"));
    const auto data = field(menu.document, value, "data");
    ReportElement scratch{};
    ReportElement* element = d.counting ? &scratch : &menu.elements[d.elements];
    ListContext options{&d, c.node, element};
    if (!d.counting) {
        reset_value(element);
        element->kind = kind;
        element->name = named(menu, value, "name");
        bool submit = false;
        json_value_to_bool(field(menu.document, value, "should_submit_data"), &submit);
        element->submit = submit;
        element->option_first = d.options;
        if (data.kind == JsonValueKind::string)
            element->body = text(menu, data);
        else if (data.kind == JsonValueKind::object) {
            element->header = named(menu, data, "header");
            element->body = named(menu, data, "body");
            if (kind == ReportElementKind::external_link) {
                element->url = named(menu, data, "url");
                if (!element->header.bytes)
                    element->header = named(menu, data, "link_text");
                if (!element->body.bytes)
                    element->body = named(menu, data, "link_description");
            }
        }
        ++c.node->element_count;
    }
    if (kind == ReportElementKind::checkbox && data.kind == JsonValueKind::array &&
        !json_visit_indexed_array(menu.document, data, depth_limit, option_item, &options).ok())
        return false;
    ++d.elements;
    return !d.failed;
}
ReportButtonKind button_kind(ReportMenu& menu, JsonValue button) noexcept {
    JsonString s{};
    if (!json_value_as_string(field(menu.document, button, "type"), &s))
        return ReportButtonKind::none;
    if (json_string_equals_ascii(s, utf8_literal("next")))
        return ReportButtonKind::next;
    if (json_string_equals_ascii(s, utf8_literal("submit")))
        return ReportButtonKind::submit;
    if (json_string_equals_ascii(s, utf8_literal("done")))
        return ReportButtonKind::done;
    if (json_string_equals_ascii(s, utf8_literal("cancel")))
        return ReportButtonKind::cancel;
    return ReportButtonKind::none;
}
bool node_member(void* p, JsonString, JsonValue value) noexcept {
    auto& d = *static_cast<Decoder*>(p);
    if (value.kind != JsonValueKind::object)
        return true;
    if (d.nodes >= kReportMaximumNodes) {
        d.failed = true;
        return false;
    }
    auto& menu = *d.menu;
    u64 id = 0;
    if (!json_value_to_u64_or_string(field(menu.document, value, "id"), &id) || id > 0xFFFFFFFFull)
        return true;
    ReportNode scratch{};
    ReportNode* node = d.counting ? &scratch : &menu.nodes[d.nodes];
    if (!d.counting) {
        reset_value(node);
        node->id = static_cast<u32>(id);
        node->key = named(menu, value, "key");
        node->header = named(menu, value, "header");
        node->subheader = named(menu, value, "subheader");
        node->info = named(menu, value, "info");
        const auto button = field(menu.document, value, "button");
        node->button = button_kind(menu, button);
        u64 target = 0;
        if (json_value_to_u64_or_string(field(menu.document, button, "target"), &target) &&
            target <= 0xFFFFFFFFull)
            node->button_target = static_cast<u32>(target);
        bool flag = false;
        if (json_value_to_bool(field(menu.document, value, "is_multi_select_required"), &flag))
            node->multi_select_required = flag;
        flag = false;
        if (json_value_to_bool(field(menu.document, value, "is_auto_submit"), &flag))
            node->auto_submit = flag;
        node->child_first = d.children;
        node->element_first = d.elements;
    }
    ListContext list{&d, node, nullptr};
    const auto children = field(menu.document, value, "children");
    if (children.kind == JsonValueKind::array &&
        !json_visit_indexed_array(menu.document, children, depth_limit, child_item, &list).ok())
        return false;
    const auto elements = field(menu.document, value, "elements");
    if (elements.kind == JsonValueKind::array &&
        !json_visit_indexed_array(menu.document, elements, depth_limit, element_item, &list).ok())
        return false;
    ++d.nodes;
    return !d.failed;
}
bool walk_nodes(Decoder& d, JsonValue nodes) noexcept {
    d.nodes = d.children = d.elements = d.options = 0;
    d.failed = false;
    return json_visit_indexed_object(d.menu->document, nodes, depth_limit, node_member, &d).ok() && !d.failed;
}
} // namespace

void report_menu_release(ReportMenu* menu) noexcept {
    arena_release(&menu->storage);
    reset_value(menu);
}

bool report_menu_decode(Utf8View body, Slice<u32> scratch, ReportMenu* output) noexcept {
    if (!body.data || body.bytes < 2 || body.data[0] != '{')
        return false;
    const auto scan = json_scan_structurals_avx1(body, scratch);
    if (!scan.ok())
        return false;
    ReportMenu menu{};
    if (!arena_initialize(&menu.storage, static_cast<usize>(body.bytes) * 3 + 256 * 1024, 0, 4096))
        return false;
    char* retained = arena_allocate<char>(&menu.storage, body.bytes);
    u32* positions = arena_allocate<u32>(&menu.storage, maximum(1u, scan.count));
    if (!retained || !positions) {
        report_menu_release(&menu);
        return false;
    }
    copy_bytes(retained, body.data, body.bytes);
    copy_bytes(positions, scratch.data, scan.count * static_cast<u32>(sizeof(u32)));
    menu.document = {{retained, body.bytes}, {positions, scan.count}};
    const JsonValue root{menu.document.input, 0, JsonValueKind::object};
    u64 root_id = 0, success = 0, fail = 0;
    const auto nodes = field(menu.document, root, "nodes");
    if (!json_value_to_u64_or_string(field(menu.document, root, "root_node_id"), &root_id) ||
        root_id > 0xFFFFFFFFull || nodes.kind != JsonValueKind::object) {
        report_menu_release(&menu);
        return false;
    }
    json_value_to_u64_or_string(field(menu.document, root, "success_node_id"), &success);
    json_value_to_u64_or_string(field(menu.document, root, "fail_node_id"), &fail);
    menu.root_node_id = static_cast<u32>(root_id);
    menu.success_node_id = static_cast<u32>(success);
    menu.fail_node_id = static_cast<u32>(fail);
    menu.name = named(menu, root, "name");
    menu.language = named(menu, root, "language");
    const auto version = field(menu.document, root, "version");
    const auto variant = field(menu.document, root, "variant");
    if (version.kind == JsonValueKind::string || version.kind == JsonValueKind::number)
        menu.version_json = version.source;
    if (variant.kind == JsonValueKind::string || variant.kind == JsonValueKind::number)
        menu.variant_json = variant.source;
    Decoder decoder{&menu, true};
    if (!walk_nodes(decoder, nodes) || !decoder.nodes) {
        report_menu_release(&menu);
        return false;
    }
    menu.nodes = arena_allocate_zero<ReportNode>(&menu.storage, decoder.nodes);
    menu.children = arena_allocate_zero<ReportChild>(&menu.storage, maximum(1u, decoder.children));
    menu.elements = arena_allocate_zero<ReportElement>(&menu.storage, maximum(1u, decoder.elements));
    menu.options = arena_allocate_zero<ReportOption>(&menu.storage, maximum(1u, decoder.options));
    decoder.counting = false;
    if (!menu.nodes || !menu.children || !menu.elements || !menu.options || !walk_nodes(decoder, nodes)) {
        report_menu_release(&menu);
        return false;
    }
    menu.node_count = decoder.nodes;
    menu.child_count = decoder.children;
    menu.element_count = decoder.elements;
    menu.option_count = decoder.options;
    if (!report_menu_node(menu, menu.root_node_id)) {
        report_menu_release(&menu);
        return false;
    }
    report_menu_release(output);
    *output = menu;
    return true;
}

const ReportNode* report_menu_node(const ReportMenu& menu, u32 id) noexcept {
    for (u32 i = 0; i < menu.node_count; ++i)
        if (menu.nodes[i].id == id)
            return menu.nodes + i;
    return nullptr;
}

UserSocialWriteResult report_write_submit(const ReportMenu& menu, const ReportSubmission& s,
                                          Slice<u8> output) noexcept {
    const bool message = s.name == ReportMenuName::message;
    if (!output.data || s.name >= ReportMenuName::count || !s.breadcrumbs.count ||
        s.breadcrumbs.count > kReportMaximumBreadcrumbs || (!s.answers.data && s.answers.count) ||
        (message ? !s.channel_id || !s.message_id : !s.user_id))
        return {0, UserSocialWriteError::input};
    for (u32 i = 0; i < s.breadcrumbs.count; ++i)
        if (!report_menu_node(menu, s.breadcrumbs[i]))
            return {0, UserSocialWriteError::input};
    u8 staged[kUserSocialMaximumBodyBytes];
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    bool ok = protocol_writer_append_byte(&w, '{');
    bool comma = false;
    const auto key = [&](const char* name) noexcept {
        const bool written = (!comma || protocol_writer_append_byte(&w, ',')) &&
                             protocol_writer_append_byte(&w, '"') && protocol_writer_append_c_string(&w, name) &&
                             protocol_writer_append_literal(&w, "\":");
        comma = true;
        return written;
    };
    if (menu.version_json.bytes)
        ok = ok && key("version") &&
             protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(menu.version_json.data), menu.version_json.bytes);
    if (menu.variant_json.bytes)
        ok = ok && key("variant") &&
             protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(menu.variant_json.data), menu.variant_json.bytes);
    ok = ok && key("language") &&
         protocol_writer_append_json_string(&w, menu.language.bytes ? menu.language : utf8_literal("en"));
    ok = ok && key("breadcrumbs") && protocol_writer_append_byte(&w, '[');
    for (u32 i = 0; ok && i < s.breadcrumbs.count; ++i)
        ok = (!i || protocol_writer_append_byte(&w, ',')) && protocol_writer_append_u64(&w, s.breadcrumbs[i]);
    ok = ok && protocol_writer_append_byte(&w, ']') && key("elements") && protocol_writer_append_byte(&w, '{');
    bool first_element = true;
    for (u32 i = 0; ok && i < s.answers.count; ++i) {
        const auto& answer = s.answers[i];
        if (answer.element >= menu.element_count)
            return {0, UserSocialWriteError::input};
        const auto& element = menu.elements[answer.element];
        if (!element.name.bytes)
            continue;
        if (element.kind == ReportElementKind::checkbox) {
            if (!answer.selected)
                continue;
            ok = (first_element || protocol_writer_append_byte(&w, ',')) &&
                 protocol_writer_append_json_string(&w, element.name) && protocol_writer_append_literal(&w, ":[");
            bool first = true;
            for (u32 o = 0; ok && o < element.option_count && o < 64; ++o) {
                if (!(answer.selected & (1ull << o)))
                    continue;
                ok = (first || protocol_writer_append_byte(&w, ',')) &&
                     protocol_writer_append_json_string(&w, menu.options[element.option_first + o].value);
                first = false;
            }
            ok = ok && protocol_writer_append_byte(&w, ']');
            first_element = false;
        } else if (element.kind == ReportElementKind::free_text) {
            if (!answer.text.bytes)
                continue;
            if (answer.text.bytes > kReportFreeTextMaximumBytes || !utf8_validate(answer.text).valid)
                return {0, UserSocialWriteError::input};
            ok = (first_element || protocol_writer_append_byte(&w, ',')) &&
                 protocol_writer_append_json_string(&w, element.name) && protocol_writer_append_byte(&w, ':') &&
                 protocol_writer_append_json_string(&w, answer.text);
            first_element = false;
        }
    }
    ok = ok && protocol_writer_append_byte(&w, '}');
    if (message)
        ok = ok && key("channel_id") && protocol_writer_append_snowflake(&w, s.channel_id) &&
             key("message_id") && protocol_writer_append_snowflake(&w, s.message_id);
    else {
        if (s.guild_id)
            ok = ok && key("guild_id") && protocol_writer_append_snowflake(&w, s.guild_id);
        ok = ok && key("user_id") && protocol_writer_append_snowflake(&w, s.user_id);
    }
    ok = ok && key("name") && protocol_writer_append_json_string(&w, report_menu_name(s.name)) &&
         protocol_writer_append_byte(&w, '}');
    return protocol_writer_finish<UserSocialWriteResult>(w, ok, output, staged);
}
} // namespace maicord
