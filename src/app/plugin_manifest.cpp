#include "maicord/app/plugin_manifest.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/name_list.hpp"
#include "../protocol/json_field_lists.hpp"

namespace maicord {
namespace {
bool text_valid(Utf8View text, u32 capacity, bool required) noexcept {
    if (!text.data || text.bytes >= capacity || (required && !text.bytes)) return false;
    u32 at = 0;
    while (at < text.bytes) {
        const auto scalar = utf8_decode_one(text.data + at, text.bytes - at);
        if (!scalar.valid || scalar.codepoint < 32 ||
            (scalar.codepoint >= 127 && scalar.codepoint <= 159)) return false;
        at += scalar.width;
    }
    return true;
}
constexpr u32 kManifestSchema = 3;
constexpr u32 kManifestAbi = 5;
enum Field : u32 { schema, owner, abi, name, version, description, dll, digest, permissions, min_host, publisher,
    field_count };
// min_host_abi and publisher are optional; everything before them is required.
constexpr u32 kRequiredFields = (1u << min_host) - 1;
constexpr char kPermissionNames[] =
    "settings\0menus\0message_send\0commands\0ui_slots\0store\0timer\0read_context\0read_guilds\0read_members\0"
    "read_presence\0read_voice\0read_messages\0message_render\0act_messages\0act_presence\0navigate\0clipboard\0"
    "open_links\0notify\0keybinds\0effects\0";
constexpr char kPermissionLabels[] =
    "Add settings pages\0Add items to context menus\0Change your messages before they are sent\0"
    "Add slash commands\0Show badges and buttons\0Store its own data\0Run on a timer\0"
    "See the open channel and your username\0See your servers, channels and roles\0"
    "See users, server members, friends and open profiles\0See online status, devices and activities\0"
    "See who is in voice channels\0Read messages you receive, including edits and deletions\0"
    "Change how messages are shown (hide, label, blur)\0"
    "Send, edit and react to messages and mark them read as you\0"
    "Change your custom status and shown activity\0"
    "Open channels, profiles and its own pages when you use it\0"
    "Copy text to the clipboard when you use it\0Open links (MaiCord asks first)\0"
    "Show notices and dialogs\0Add keyboard shortcuts\0Draw a background effect\0";
static_assert(name_list_count(kPermissionNames) == kNativePluginPermissionCount &&
              name_list_count(kPermissionLabels) == kNativePluginPermissionCount);
struct Parse { NativePluginManifest candidate; u32 seen; NativePluginManifestError error; JsonIndexedDocument document; };
bool permission(void* context, u32 index, JsonValue value) noexcept {
    auto& parse = *static_cast<Parse*>(context);
    JsonString string{};
    if (index >= kNativePluginPermissionCount || !json_value_as_string(value, &string)) {
        parse.error = index >= kNativePluginPermissionCount ? NativePluginManifestError::capacity : NativePluginManifestError::type;
        return false;
    }
    u32 bit = 0;
    for (NameList names{kPermissionNames}; names; names.next(), ++bit) {
        if (!json_string_equals_ascii(string, names.name())) continue;
        if (parse.candidate.permissions & (1ull << bit)) { parse.error = NativePluginManifestError::duplicate_field; return false; }
        parse.candidate.permissions |= 1ull << bit;
        return true;
    }
    parse.error = NativePluginManifestError::unsupported;
    return false;
}
bool decode(JsonValue value, char* target, u32 capacity, u32* count, bool required, Parse& parse) noexcept {
    JsonString string{};
    if (!json_value_as_string(value, &string)) { parse.error = NativePluginManifestError::type; return false; }
    const auto decoded = json_string_decode_utf8(string, {target, capacity - 1});
    if (!decoded.ok() || !text_valid({target, decoded.bytes}, capacity, required)) {
        parse.error = NativePluginManifestError::text; return false;
    }
    *count = decoded.bytes;
    target[*count] = 0;
    return true;
}
bool visit(void* context, JsonString key, JsonValue value) noexcept {
    auto& parse = *static_cast<Parse*>(context);
    const u32 field = protocol_detail::member_index<"schema", "owner_id", "abi", "name", "version", "description", "dll",
                                                    "sha256", "permissions", "min_host_abi", "publisher">(key);
    if (field == field_count) { parse.error = NativePluginManifestError::unknown_field; return false; }
    if (parse.seen & (1u << field)) { parse.error = NativePluginManifestError::duplicate_field; return false; }
    parse.seen |= 1u << field;
    auto& output = parse.candidate;
    u64 number = 0;
    if (field <= abi || field == min_host) {
        if (!json_value_to_u64(value, &number)) { parse.error = NativePluginManifestError::type; return false; }
        if ((field == schema && number != kManifestSchema) || (field == abi && number != kManifestAbi) ||
            (field == owner && !number) || (field == min_host && (number < kManifestAbi || number > 0xFFFF))) {
            parse.error = NativePluginManifestError::unsupported; return false;
        }
        if (field == owner) output.owner_id = number;
        if (field == abi) output.abi_version = static_cast<u32>(number);
        if (field == min_host) output.min_host_abi = static_cast<u32>(number);
        return true;
    }
    switch (field) {
    case name: return decode(value, output.name, sizeof(output.name), &output.name_bytes, true, parse);
    case version: return decode(value, output.version, sizeof(output.version), &output.version_bytes, true, parse);
    case description: return decode(value, output.description, sizeof(output.description), &output.description_bytes, false, parse);
    case publisher: return decode(value, output.publisher, sizeof(output.publisher), &output.publisher_bytes, false, parse);
    case permissions: {
        if (value.kind != JsonValueKind::array) { parse.error = NativePluginManifestError::type; return false; }
        const auto read = json_visit_indexed_array(parse.document, value, 1, permission, &parse);
        if (!read.ok() && parse.error == NativePluginManifestError::none) parse.error = NativePluginManifestError::json;
        return read.ok();
    }
    case dll:
        if (!decode(value, output.dll, sizeof(output.dll), &output.dll_bytes, true, parse)) return false;
        if (!native_plugin_dll_name_valid({output.dll, output.dll_bytes})) { parse.error = NativePluginManifestError::dll_name; return false; }
        return true;
    case digest: {
        JsonString string{};
        if (!json_value_as_string(value, &string)) { parse.error = NativePluginManifestError::type; return false; }
        if (string.has_escapes || string.encoded.bytes != 64) { parse.error = NativePluginManifestError::digest; return false; }
        for (u32 i = 0; i < 64; ++i) {
            const char c = string.encoded.data[i];
            const i32 nibble = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (nibble < 0) { parse.error = NativePluginManifestError::digest; return false; }
            output.sha256[i / 2] = static_cast<u8>((output.sha256[i / 2] << 4) | nibble);
        }
        return true;
    }
    default: return false;
    }
}
} // namespace
bool native_plugin_manifest_same_dll(const NativePluginManifest& a, const NativePluginManifest& b) noexcept {
    if (a.dll_bytes != b.dll_bytes) return false;
    for (u32 i = 0; i < a.dll_bytes; ++i) if (ascii_lower(a.dll[i]) != ascii_lower(b.dll[i])) return false;
    return true;
}
bool native_plugin_dll_name_valid(Utf8View name) noexcept {
    if (name.bytes < 5 || name.bytes >= kNativePluginDllNameCapacity || name.data[0] == '.' ||
        name.data[name.bytes - 4] != '.' || ascii_lower(name.data[name.bytes - 3]) != 'd' ||
        ascii_lower(name.data[name.bytes - 2]) != 'l' || ascii_lower(name.data[name.bytes - 1]) != 'l') return false;
    u32 stem = 0;
    for (u32 i = 0; i < name.bytes; ++i) {
        const char c = ascii_lower(name.data[i]);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
        if (c == '.' && i && name.data[i - 1] == '.') return false;
        if (!stem && c == '.') stem = i;
    }
    const Utf8View base{name.data, stem};
    static constexpr Utf8View reserved[]{utf8_literal("con"), utf8_literal("prn"), utf8_literal("aux"), utf8_literal("nul")};
    for (auto word : reserved) {
        if (base.bytes != word.bytes) continue;
        bool match = true;
        for (u32 i = 0; i < base.bytes; ++i) if (ascii_lower(base.data[i]) != word.data[i]) match = false;
        if (match) return false;
    }
    if (base.bytes == 4 && base.data[3] >= '1' && base.data[3] <= '9' &&
        ((ascii_lower(base.data[0]) == 'c' && ascii_lower(base.data[1]) == 'o' && ascii_lower(base.data[2]) == 'm') ||
         (ascii_lower(base.data[0]) == 'l' && ascii_lower(base.data[1]) == 'p' && ascii_lower(base.data[2]) == 't'))) return false;
    return true;
}
bool native_plugin_path_component_valid(ConstSlice<wchar_t> part) noexcept {
    if (!part.count || part[0] == L'.' || part[part.count - 1] == L'.' || part[part.count - 1] == L' ') return false;
    u32 stem = 0;
    while (stem < part.count && part[stem] != L'.') ++stem;
    const auto stem_is = [&](const wchar_t* name) noexcept {
        return ascii_lower(part[0]) == name[0] && ascii_lower(part[1]) == name[1] && ascii_lower(part[2]) == name[2];
    };
    if (stem == 3 && (stem_is(L"con") || stem_is(L"prn") || stem_is(L"aux") || stem_is(L"nul"))) return false;
    if (stem == 4 && part[3] >= L'1' && part[3] <= L'9' && (stem_is(L"com") || stem_is(L"lpt"))) return false;
    for (u32 i = 0; i < part.count; ++i) {
        const wchar_t c = part[i];
        if (c < 32 || c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' || c == L'"' ||
            c == L'<' || c == L'>' || c == L'|' || c == L'~' || (c >= 127 && c <= 159) ||
            (c >= 0xd800 && c <= 0xdfff)) return false;
    }
    return true;
}
Utf8View native_plugin_permission_name(u32 index) noexcept {
    return index < kNativePluginPermissionCount ? name_list_at(kPermissionNames, index) : Utf8View{};
}
u32 native_plugin_permission_bit(Utf8View name) noexcept {
    u32 bit = 0;
    for (NameList names{kPermissionNames}; names; names.next(), ++bit)
        if (utf8_equal(name, names.name())) return bit;
    return kNativePluginPermissionCount;
}
Utf8View native_plugin_permission_label(u32 index) noexcept {
    return index < kNativePluginPermissionCount ? name_list_at(kPermissionLabels, index) : Utf8View{};
}
bool native_plugin_manifest_valid(const NativePluginManifest& manifest) noexcept {
    if ((manifest.permissions & ~kNativePluginPermissionsAll) ||
        manifest.publisher_bytes >= sizeof(manifest.publisher) ||
        !text_valid({manifest.publisher, manifest.publisher_bytes}, sizeof(manifest.publisher), false)) return false;
    return manifest.owner_id && manifest.abi_version == kManifestAbi &&
        text_valid({manifest.name, manifest.name_bytes}, sizeof(manifest.name), true) &&
        text_valid({manifest.version, manifest.version_bytes}, sizeof(manifest.version), true) &&
        text_valid({manifest.description, manifest.description_bytes}, sizeof(manifest.description), false) &&
        manifest.dll_bytes < sizeof(manifest.dll) && native_plugin_dll_name_valid({manifest.dll, manifest.dll_bytes});
}
NativePluginManifestResult native_plugin_manifest_parse(Utf8View document, NativePluginManifest* output) noexcept {
    const ConstSlice<u8> input{reinterpret_cast<const u8*>(document.data), document.bytes};
    const ConstSlice<u8> target{reinterpret_cast<const u8*>(output), sizeof(*output)};
    if (!output || !document.bytes || document.bytes > kNativePluginManifestMaximumBytes ||
        !byte_range_valid(input) || !byte_range_valid(target) || byte_ranges_overlap(input, target))
        return {NativePluginManifestError::invalid_input};
    // Eight scalar fields need fewer than 64 JSON structural offsets; excess
    // structure cannot be a valid schema and is refused rather than allocated.
    u32 indexes[128];
    const auto scan = json_scan_structurals_avx1(document, {indexes, 128});
    if (!scan.ok()) return {NativePluginManifestError::json};
    Parse parse{};
    parse.document = {document, {indexes, scan.count}};
    const auto read = json_visit_root_object(document, {indexes, scan.count}, 2, visit, &parse);
    if (!read.ok()) return {parse.error == NativePluginManifestError::none ? NativePluginManifestError::json : parse.error};
    if ((parse.seen & kRequiredFields) != kRequiredFields) return {NativePluginManifestError::missing_field};
    copy_value(output, parse.candidate);
    return {NativePluginManifestError::none};
}
NativePluginManifestResult native_plugin_manifest_write(const NativePluginManifest& manifest,
    Slice<char> output,u32* bytes) noexcept {
    if(!bytes || !native_plugin_manifest_valid(manifest) || output.count>kNativePluginManifestMaximumBytes)
        return {NativePluginManifestError::invalid_input};
    const ConstSlice<u8> ranges[]{{reinterpret_cast<const u8*>(&manifest),sizeof(manifest)},
        {reinterpret_cast<const u8*>(output.data),output.count},{reinterpret_cast<const u8*>(bytes),sizeof(*bytes)}};
    for(u32 i=0;i<3;++i){if(!byte_range_valid(ranges[i]))return {NativePluginManifestError::invalid_input};
        for(u32 j=0;j<i;++j)if(byte_ranges_overlap(ranges[i],ranges[j]))return {NativePluginManifestError::invalid_input};}
    struct Writer {
        char* data{};u32 used{};
        void character(char c) noexcept {if(data)data[used]=c;++used;}
        void text(Utf8View v) noexcept {for(u32 i=0;i<v.bytes;++i)character(v.data[i]);}
        void quoted(Utf8View v) noexcept {character('"');for(u32 i=0;i<v.bytes;++i){
            if(v.data[i]=='"'||v.data[i]=='\\')character('\\');character(v.data[i]);}character('"');}
        void number(u64 value) noexcept {char digits[20];u32 count{};do{digits[count++]=static_cast<char>('0'+value%10);value/=10;}while(value);
            while(count)character(digits[--count]);}
    };
    const auto write=[&manifest](Writer& w) noexcept {
        w.text(utf8_literal("{\"schema\":3,\"owner_id\":"));
        w.number(manifest.owner_id);
        w.text(utf8_literal(",\"abi\":"));w.number(manifest.abi_version);
        if(manifest.min_host_abi){w.text(utf8_literal(",\"min_host_abi\":"));w.number(manifest.min_host_abi);}
        if(manifest.publisher_bytes){w.text(utf8_literal(",\"publisher\":"));w.quoted({manifest.publisher,manifest.publisher_bytes});}
        w.text(utf8_literal(",\"permissions\":["));bool first=true;
        for(u32 bit=0;bit<kNativePluginPermissionCount;++bit)if(manifest.permissions&(1ull<<bit)){
            if(!first)w.character(',');first=false;w.quoted(name_list_at(kPermissionNames,bit));}
        w.character(']');
        w.text(utf8_literal(",\"name\":"));w.quoted({manifest.name,manifest.name_bytes});
        w.text(utf8_literal(",\"version\":"));w.quoted({manifest.version,manifest.version_bytes});
        w.text(utf8_literal(",\"description\":"));w.quoted({manifest.description,manifest.description_bytes});
        w.text(utf8_literal(",\"dll\":"));w.quoted({manifest.dll,manifest.dll_bytes});
        w.text(utf8_literal(",\"sha256\":\""));constexpr char hex[]="0123456789abcdef";
        for(u32 i=0;i<32;++i){w.character(hex[manifest.sha256[i]>>4]);w.character(hex[manifest.sha256[i]&15]);}
        w.text(utf8_literal("\"}"));
    };
    Writer size{};write(size);
    if(size.used>output.count)return {NativePluginManifestError::capacity};
    Writer actual{output.data};write(actual);*bytes=actual.used;return {NativePluginManifestError::none};
}
} // namespace maicord
