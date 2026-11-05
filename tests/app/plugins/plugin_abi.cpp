// ABI 6 SDK: the C++ layer's helpers and wrappers, the sample plugins in
// samples/plugins and the plain C fixture DLL: their PE shape (no entry point,
// no TLS, no imports, one export), their descriptors, and their entry points
// called directly against a recording host table.
#include "maicord/sdk/plugin.hpp"

#include "maicord/core/byte_order.hpp"
#if defined(_WIN32)
#include "maicord/core/win32_kernel.hpp"
#else
#include "maicord/core/shared_library.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "plugin_fixture.hpp"
#endif
#include "../../support/test_output.hpp"

// Frozen layouts (the C fixture asserts every field; these guard the C++ view).
static_assert(sizeof(mc_ctx) == 32 && sizeof(mc_setting_def) == 128 && sizeof(mc_hook) == 24);
static_assert(sizeof(mc_draft) == 80 && offsetof(mc_draft, verdict) == 68 && offsetof(mc_draft, reply_author_id) == 72);
static_assert(sizeof(mc_descriptor) == 232 && offsetof(mc_descriptor, hooks) == 160 &&
              offsetof(mc_descriptor, start) == 216);
static_assert(sizeof(mc_api) == 368 && offsetof(mc_api, toast) == 48 && offsetof(mc_api, http) == 160 &&
              offsetof(mc_api, clock) == 320);
static_assert(sizeof(mc_message) == 112 && sizeof(mc_decoration) == 1208 && sizeof(mc_completion) == 64);

namespace maicord::plugin_abi_tests {
namespace {

u32 failures;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}

mc::Str view(const char* text) noexcept {
    u32 bytes = 0;
    while (text[bytes])
        ++bytes;
    return mc::Str(text, bytes);
}
bool same(mc::Str text, const char* expected) noexcept {
    return text == view(expected);
}

// ---- url::strip_params, url::encode, json ---------------------------------------

void strip(const char* input, const char* expected, const char* label,
           const mc::StrList& extra = {}) noexcept {
    mc::Text<256> out;
    const bool changed = mc::url::strip_params(view(input), mc::url::kTrackers, extra, out);
    check(expected ? changed && same(out, expected) : !changed && out.empty(), label);
}

void url_cases() noexcept {
    strip("https://a.com/?utm_source=x", "https://a.com/", "strip: only parameter");
    strip("x https://a.com/p?id=1&utm_medium=m&b=2#frag y", "x https://a.com/p?id=1&b=2#frag y",
          "strip: middle parameter, fragment and surrounding text kept");
    strip("<https://a.com/?fbclid=1>", "<https://a.com/>", "strip: suppressed embed");
    strip("[t](https://a.com/?gclid=1)", "[t](https://a.com/)", "strip: masked link keeps its parenthesis");
    strip("(https://en.wikipedia.org/wiki/A_(b)?utm_x=1)", "(https://en.wikipedia.org/wiki/A_(b))",
          "strip: balanced parentheses stay in the link");
    strip("HTTPS://A.COM/?UTM_SOURCE=1. ok", "HTTPS://A.COM/. ok", "strip: ASCII case and trailing period");
    strip("||https://a.com/?utm_source=1||", "||https://a.com/||", "strip: spoiler markers");
    strip("https://a.com/?a=1&&utm_source=2&", "https://a.com/?a=1", "strip: empty segments dropped");
    strip("https://a.com/?utm_a=1 and http://b.org/x?fbclid=2&k=v", "https://a.com/ and http://b.org/x?k=v",
          "strip: two links");
    strip("https://a.com/?id=1", nullptr, "strip: nothing to remove");
    strip("no links utm_source=1", nullptr, "strip: parameters outside links");
    strip("https://a.com/#utm_source=1", nullptr, "strip: fragment is not a query");
    strip("https://youtu.be/x?si=abc&t=1", nullptr, "strip: si is not a default tracker");
    const mc_str extra_items[] = {MC_S(" si "), MC_S("*"), MC_S("")};
    mc::StrList extra{};
    extra.items = extra_items;
    extra.count = 3;
    strip("https://youtu.be/x?si=abc&t=1", "https://youtu.be/x?t=1",
          "strip: extra pattern trimmed; '*' and '' match nothing", extra);
    mc::Text<8> small;
    check(!mc::url::strip_params(view("https://a.com/?utm_source=x"), mc::url::kTrackers, small) &&
              small.empty(),
          "strip: result that doesn't fit is no change");
    check(mc::url::key_matches(view("utm_campaign"), view("utm_*")) &&
              !mc::url::key_matches(view("utm"), view("utm_*")),
          "key_matches: prefix pattern");

    mc::Text<4> text;
    check(text.append(view("abc")) && !text.append(view("de")) && text.overflowed() && same(text, "abc") &&
              text.push('d') && same(text, "abcd") && !text.push('e'),
          "Text: bounded appends");

    mc::Text<64> encoded;
    encoded.append("q=");
    check(mc::url::encode(view("a b&c/é~"), encoded) && same(encoded, "q=a%20b%26c%2F%C3%A9~"),
          "url::encode: unreserved bytes stay, others become %XX");
    mc::Text<4> tight;
    check(!mc::url::encode(view("a b"), tight) && tight.empty(), "url::encode: no change when it doesn't fit");

    mc::Text<32> found;
    check(mc::json::find(view(R"({"a":{"text":1,"x":"text"},"text" : "H\u00e9\n\"\ud83d\ude00"})"),
                         view("text"), found) &&
              same(found, "H\xC3\xA9\n\"\xF0\x9F\x98\x80"),
          "json::find: the first string value of the key, unescaped; strings never match keys");
    check(!mc::json::find(view(R"({"text":5})"), view("text"), found) &&
              !mc::json::find(view(R"({"text":"unterminated)"), view("text"), found),
          "json::find: numbers and broken strings are not found");
    u32 at = 0;
    check(mc::json::skip(view(R"([1,"]",{"a":[2]}],3)"), at) && at == 17, "json::skip: one nested value");
}

// ---- A recording host table ------------------------------------------------------

struct Host {
    u32 logs, toasts, sets, gets, removes, setting_writes, https, inserts, opens, copies, buttons, invalidates;
    u32 level, tone, setting_index, setting_bytes, button_index, button_flags, invalidate_what;
    u8 setting_value[8];
    char key[16];
    u32 key_bytes;
    u8 value[16];
    u32 value_bytes;
    bool stored;
    u64 http_tag, invalidate_id;
    u32 http_method, http_headers;
    mc::Text<256> text; // the last toast, inserted or opened text
    mc::Text<256> url, body, header_name, header_value, button_tooltip;
} g_host;

void keep(mc::Text<256>& out, mc_str text) noexcept {
    out.clear();
    out.append(mc::Str(text));
}
bool host_key_is(mc_str key) noexcept {
    return g_host.stored && mc::Str(key) == mc::Str(g_host.key, g_host.key_bytes);
}
void MC_CALL host_log(mc_ctx*, uint32_t level, mc_str) {
    ++g_host.logs;
    g_host.level = level;
}
mc_status MC_CALL host_set_setting(mc_ctx*, uint32_t index, const void* value, uint32_t bytes) {
    ++g_host.setting_writes;
    g_host.setting_index = index;
    g_host.setting_bytes = bytes;
    if (bytes <= sizeof(g_host.setting_value))
        copy_bytes(g_host.setting_value, value, bytes);
    return MC_OK;
}
mc_status MC_CALL host_store_get(mc_ctx*, mc_str key, void* out, uint32_t capacity, uint32_t* bytes) {
    ++g_host.gets;
    if (!host_key_is(key))
        return MC_E_NOT_FOUND;
    *bytes = g_host.value_bytes;
    if (g_host.value_bytes > capacity)
        return MC_E_TRUNCATED;
    copy_bytes(out, g_host.value, g_host.value_bytes);
    return MC_OK;
}
mc_status MC_CALL host_store_set(mc_ctx*, mc_str key, const void* value, uint32_t bytes) {
    ++g_host.sets;
    if (key.bytes > sizeof(g_host.key) || bytes > sizeof(g_host.value))
        return MC_E_CAPACITY;
    copy_bytes(g_host.key, key.data, key.bytes);
    g_host.key_bytes = key.bytes;
    copy_bytes(g_host.value, value, bytes);
    g_host.value_bytes = bytes;
    g_host.stored = true;
    return MC_OK;
}
mc_status MC_CALL host_store_remove(mc_ctx*, mc_str key) {
    ++g_host.removes;
    if (!host_key_is(key))
        return MC_E_NOT_FOUND;
    g_host.stored = false;
    return MC_OK;
}
mc_status MC_CALL host_toast(mc_ctx*, mc_str text, uint32_t tone, uint32_t flags) {
    ++g_host.toasts;
    g_host.tone = tone;
    keep(g_host.text, text);
    return flags == 0 ? MC_OK : MC_E_INVALID;
}
mc_status MC_CALL host_invalidate(mc_ctx*, uint32_t what, uint64_t id) {
    ++g_host.invalidates;
    g_host.invalidate_what = what;
    g_host.invalidate_id = id;
    return MC_OK;
}
mc_status MC_CALL host_button_state(mc_ctx*, uint32_t button, const mc_button_state* state) {
    ++g_host.buttons;
    g_host.button_index = button;
    g_host.button_flags = state->flags;
    keep(g_host.button_tooltip, state->tooltip);
    return MC_OK;
}
mc_status MC_CALL host_open_url(mc_ctx*, mc_str url) {
    ++g_host.opens;
    keep(g_host.text, url);
    return MC_OK;
}
mc_status MC_CALL host_copy_text(mc_ctx*, mc_str text) {
    ++g_host.copies;
    keep(g_host.text, text);
    return MC_OK;
}
mc_status MC_CALL host_http(mc_ctx*, const mc_http_request* request, uint64_t tag) {
    ++g_host.https;
    g_host.http_tag = tag;
    g_host.http_method = request->method;
    g_host.http_headers = request->header_count;
    keep(g_host.url, request->url);
    g_host.body.clear();
    g_host.body.append(mc::Str(static_cast<const char*>(request->body), request->body_bytes));
    g_host.header_name.clear();
    g_host.header_value.clear();
    if (request->header_count) {
        const mc_http_header& last = request->headers[request->header_count - 1];
        keep(g_host.header_name, last.name);
        keep(g_host.header_value, last.value);
    }
    return MC_OK;
}
mc_status MC_CALL host_composer_insert(mc_ctx*, mc_str text) {
    ++g_host.inserts;
    keep(g_host.text, text);
    return MC_OK;
}
mc_status MC_CALL host_clock(mc_ctx*, uint64_t* unix_ms) {
    *unix_ms = 1'700'000'000'123ull;
    return MC_OK;
}
mc_status MC_CALL host_local_offset(mc_ctx*, int64_t unix_seconds, int32_t* seconds_east) {
    *seconds_east = unix_seconds > 0 ? 3600 : 0;
    return unix_seconds >= 0 ? MC_OK : MC_E_INVALID;
}
mc_status MC_CALL host_read_voice_channel(mc_ctx*, uint64_t user, uint64_t, uint64_t* channel) {
    *channel = user == 5 ? 66 : 0;
    return *channel ? MC_OK : MC_E_NOT_FOUND;
}
mc_status MC_CALL host_read_open_channel(mc_ctx*, mc_open_channel* out) {
    *out = {9, 4, 31, 0};
    return MC_OK;
}
constexpr mc_api kApi{
    .size = sizeof(mc_api),
    .level = MC_API_LEVEL,
    .log = host_log,
    .set_setting = host_set_setting,
    .store_get = host_store_get,
    .store_set = host_store_set,
    .store_remove = host_store_remove,
    .toast = host_toast,
    .invalidate = host_invalidate,
    .button_state = host_button_state,
    .open_url = host_open_url,
    .copy_text = host_copy_text,
    .http = host_http,
    .composer_insert = host_composer_insert,
    .clock = host_clock,
    .local_offset = host_local_offset,
    .read_voice_channel = host_read_voice_channel,
    .read_open_channel = host_read_open_channel,
};

// Fills d from text that dies with this frame, as a decorate hook's locals do.
void decorate_from_local(mc::Decoration& d) noexcept {
    mc::Text<16> text, tip;
    text.append("local");
    tip.append("tip");
    d.badge(mc::badge(text, mc::Tone::accent, {}, tip));
    d.prefix(text);
    d.card(mc::card().title(text));
    // What the host would read if the decoration still pointed here.
    for (u32 i = 0; i < text.size(); ++i)
        text.data()[i] = '#';
    for (u32 i = 0; i < tip.size(); ++i)
        tip.data()[i] = '#';
}
void ctx_wrappers() noexcept {
    g_host = {};
    u64 state = 0;
    mc_ctx raw{&kApi, &state, nullptr, MC_CTX_FIRST_START, 1};
    const mc::Ctx ctx(&raw);
    check(ctx.first_start() && !ctx.gesture() && !ctx.reloaded() && ctx.raw() == &raw, "Ctx: flags");

    check(ctx.store.put("count", u32{7}) == MC_OK && g_host.value_bytes == 4 &&
              same(mc::Str(g_host.key, g_host.key_bytes), "count"),
          "Store: put copies a value of its size");
    u32 count = 0;
    u64 wide = 0;
    check(ctx.store.get("count", count) && count == 7 && !ctx.store.get("count", wide),
          "Store: get needs the exact size");
    check(ctx.store.put("note", "hello") == MC_OK && g_host.value_bytes == 5,
          "Store: a literal is stored as text");
    mc::Text<4> small;
    mc::Text<8> note;
    check(!ctx.store.get("note", small) && small.empty() && ctx.store.get("note", note) &&
              same(note, "hello"),
          "Store: text get respects capacity");
    check(ctx.store.remove("note") == MC_OK && ctx.store.remove("note") == MC_E_NOT_FOUND, "Store: remove");

    check(ctx.toast("Hi", mc::Tone::warning) == MC_OK && g_host.toasts == 1 &&
              g_host.tone == MC_TONE_WARNING,
          "Ctx: toast");
    ctx.log("x");
    ctx.log(mc::Level::error, "y");
    check(g_host.logs == 2 && g_host.level == MC_LOG_ERROR, "Ctx: log levels");
    check(ctx.set_setting(2u, true) == MC_OK && g_host.setting_index == 2 && g_host.setting_bytes == 1 &&
              g_host.setting_value[0] == 1,
          "Ctx: toggle setting is one byte");
    check(ctx.set_setting(0u, mc::i32{-5}) == MC_OK && g_host.setting_bytes == 4 &&
              static_cast<i32>(byte_load_le32(g_host.setting_value)) == -5,
          "Ctx: int setting is four bytes");
    check(ctx.set_setting(1u, u64{9}) == MC_OK && g_host.setting_bytes == 8 && g_host.setting_value[0] == 9,
          "Ctx: channel setting is eight bytes");
    check(ctx.invalidate_message(5) == MC_OK && g_host.invalidate_what == MC_INVALIDATE_MESSAGE &&
              g_host.invalidate_id == 5 && ctx.invalidate_buttons() == MC_OK &&
              g_host.invalidate_what == MC_INVALIDATE_BUTTONS,
          "Ctx: invalidate");
    check(ctx.open_url("https://a.com") == MC_OK && same(g_host.text, "https://a.com"), "Ctx: open_url");
    check(ctx.now_ms() == 1'700'000'000'123ull && ctx.local_offset(10) == 3600 && ctx.local_offset(-1) == 0,
          "Ctx: the clock and the local offset, 0 on failure");
    const auto open = ctx.read.open_channel();
    check(ctx.read.voice_channel(5) == 66 && ctx.read.voice_channel(6) == 0 && open && open->reply_to == 31,
          "Reader: voice channel and open channel");

    mc::Decoration d{};
    d.collapse("Hidden");
    d.suffix("x", mc::Tone::role);
    d.replace_content("**y**");
    bool badges = true;
    for (u32 i = 0; i < MC_DECORATION_BADGES; ++i)
        badges = badges && d.badge(mc::badge("b"));
    check(d.visibility == MC_VIS_COLLAPSED && same(d.collapsed_label, "Hidden") && same(d.name_suffix, "x") &&
              d.name_tone == MC_TONE_ROLE && (d.flags & MC_DECO_REPLACE_CONTENT) && badges &&
              !d.badge(mc::badge("one too many")) && d.badge_count == MC_DECORATION_BADGES,
          "Decoration: builders fill the fields and stop at the limits");
    const mc::Card c = mc::card().title("T").button("A", 1).button("B", 2).button("C", 3);
    check(same(c.mc_card::title, "T") && c.button_count == MC_CARD_BUTTONS && c.buttons[1].tag == 2,
          "Card: at most two buttons");

    // A hook's locals are gone when the host reads its decoration: the text is copied.
    mc::Decoration local{};
    decorate_from_local(local);
    const char* const kept_begin = mc::detail::g_kept;
    const char* const kept_end = kept_begin + sizeof(mc::detail::g_kept);
    const char* const badge_text = local.badges[0].text.data;
    check(same(local.badges[0].text, "local") && same(local.badges[0].tooltip, "tip") &&
              same(local.name_prefix, "local") && same(local.mc_decoration::card.title, "local") &&
              badge_text >= kept_begin && badge_text < kept_end,
          "Decoration: text from a hook's locals is copied");
    const mc::Ctx next(&raw);
    check(mc::detail::g_kept_used == 0, "Ctx: every entry point starts the decoration text over");
}

// ---- Plugin DLLs ---------------------------------------------------------------

#if defined(_WIN32)
HMODULE load_sibling(const wchar_t* leaf) noexcept {
    wchar_t path[1024];
    const DWORD count = GetModuleFileNameW(nullptr, path, 1024);
    if (!count || count >= 1024)
        return nullptr;
    u32 at = count;
    while (at && path[at - 1] != L'\\')
        --at;
    u32 length = 0;
    while (leaf[length])
        ++length;
    if (at + length >= 1024)
        return nullptr;
    for (u32 i = 0; i <= length; ++i)
        path[at + i] = leaf[i];
    return LoadLibraryExW(path, nullptr, 0);
}
#else
// The samples are built beside the test executable, in plugins/, as "<name>.so"; the tests name them "<name>.dll".
bool sibling_path(const wchar_t* leaf, wchar_t (&path)[MAX_PATH]) noexcept {
    wchar_t directory[MAX_PATH], shared_object[MAX_PATH];
    const u32 length = plugin_fixture::length(leaf);
    if (length < 5 || length >= MAX_PATH || leaf[length - 4] != L'.' || !plugin_fixture::test_directory(directory))
        return false;
    for (u32 i = 0; i + 3 < length; ++i)
        shared_object[i] = leaf[i];
    shared_object[length - 3] = L's';
    shared_object[length - 2] = L'o';
    shared_object[length - 1] = 0;
    plugin_fixture::join(path, directory, shared_object);
    return true;
}
HMODULE load_sibling(const wchar_t* leaf) noexcept {
    wchar_t path[MAX_PATH];
    char text[1024];
    SharedLibrary library{};
    return sibling_path(leaf, path) && path_to_utf8(path, text, sizeof(text)) != 0 &&
                   shared_library_open(&library, text)
               ? library.handle
               : nullptr;
}
#endif

#if defined(_WIN32)

// What the host's PE pre-check looks at, read from the mapped image.
struct PeShape {
    bool amd64;
    u32 entry;
    u32 import_bytes;
    u32 tls_bytes;
    u32 functions;
    u32 names;
    bool descriptor_export;
};
PeShape pe_shape(HMODULE module) noexcept {
    const auto* base = reinterpret_cast<const u8*>(module);
    const u32 nt = byte_load_le32(base + 0x3C);
    const u8* optional = base + nt + 24;
    const u8* directories = optional + 112; // IMAGE_OPTIONAL_HEADER64.DataDirectory
    PeShape shape{};
    shape.amd64 = byte_load_le32(base + nt) == 0x00004550 && byte_load_le16(base + nt + 4) == 0x8664 &&
                  byte_load_le16(optional) == 0x20B;
    shape.entry = byte_load_le32(optional + 16);
    shape.import_bytes = byte_load_le32(directories + 1 * 8 + 4);
    shape.tls_bytes = byte_load_le32(directories + 9 * 8 + 4);
    const u32 exports = byte_load_le32(directories);
    if (exports) {
        const u8* table = base + exports; // IMAGE_EXPORT_DIRECTORY
        shape.functions = byte_load_le32(table + 20);
        shape.names = byte_load_le32(table + 24);
        const auto* name =
            reinterpret_cast<const char*>(base + byte_load_le32(base + byte_load_le32(table + 32)));
        shape.descriptor_export = shape.names == 1 && same(view(name), MC_DESCRIPTOR_EXPORT);
    }
    return shape;
}
void check_pe(HMODULE module, const char* label) noexcept {
    const PeShape shape = pe_shape(module);
    const bool valid = shape.amd64 && shape.entry == 0 && shape.import_bytes == 0 && shape.tls_bytes == 0 &&
                       shape.functions == 1 && shape.descriptor_export;
    check(valid, label);
}
void check_shape(HMODULE module, const wchar_t*, const char* label) noexcept {
    check_pe(module, label);
}
void* module_symbol(HMODULE module, const char* name) noexcept {
    return reinterpret_cast<void*>(GetProcAddress(module, name));
}
void close_module(HMODULE module) noexcept {
    FreeLibrary(module);
}
#else
// The shared object's shape, read from its section headers and program headers: what the host's ELF check rejects
// (no entry point, no PT_TLS, no DT_NEEDED, no undefined symbol) and exactly one exported symbol, the descriptor.
// The loader reads the dynamic section; this reads the sections the linker also wrote, so the two disagree on a
// malformed file.
struct ElfShape {
    bool x86_64;
    bool entry;
    u32 needed;
    u32 tls;
    u32 undefined;
    u32 exports;
    bool descriptor_export;
};
constexpr u32 kShtDynamic = 6, kShtDynsym = 11, kPtTls = 7;
ElfShape elf_shape(const u8* file, u32 size) noexcept {
    ElfShape shape{};
    if (size < 64 || file[0] != 0x7F || file[1] != 'E' || file[2] != 'L' || file[3] != 'F')
        return shape;
    shape.x86_64 = file[4] == 2 && file[5] == 1 && byte_load_le16(file + 16) == 3 && byte_load_le16(file + 18) == 62;
    shape.entry = byte_load_le64(file + 24) != 0;
    const u64 program = byte_load_le64(file + 32), sections = byte_load_le64(file + 40);
    const u32 program_size = byte_load_le16(file + 54), program_count = byte_load_le16(file + 56);
    const u32 section_size = byte_load_le16(file + 58), section_count = byte_load_le16(file + 60);
    if (program_size < 56 || section_size < 64 || program > size || sections > size ||
        program_count > (size - program) / program_size || section_count > (size - sections) / section_size)
        return ElfShape{};
    for (u32 i = 0; i < program_count; ++i)
        shape.tls += byte_load_le32(file + program + u64{i} * program_size) == kPtTls;
    for (u32 i = 0; i < section_count; ++i) {
        const u8* header = file + sections + u64{i} * section_size;
        const u32 type = byte_load_le32(header + 4), link = byte_load_le32(header + 40);
        const u64 offset = byte_load_le64(header + 24), bytes = byte_load_le64(header + 32);
        if ((type != kShtDynamic && type != kShtDynsym) || offset > size || bytes > size - offset)
            continue;
        if (type == kShtDynamic) {
            for (u64 at = 0; at + 16 <= bytes; at += 16)
                shape.needed += byte_load_le64(file + offset + at) == 1; // DT_NEEDED
            continue;
        }
        if (link >= section_count)
            return ElfShape{};
        const u8* names = file + sections + u64{link} * section_size;
        const u64 names_at = byte_load_le64(names + 24), names_bytes = byte_load_le64(names + 32);
        if (names_at > size || names_bytes > size - names_at)
            return ElfShape{};
        for (u64 at = 24; at + 24 <= bytes; at += 24) { // symbol 0 is the null symbol
            const u8* symbol = file + offset + at;
            if (byte_load_le16(symbol + 6) == 0) {
                ++shape.undefined;
                continue;
            }
            ++shape.exports;
            const u32 name = byte_load_le32(symbol);
            const char* text = reinterpret_cast<const char*>(file + names_at + name);
            shape.descriptor_export = name < names_bytes && names_bytes - name >= sizeof(MC_DESCRIPTOR_EXPORT) &&
                                      shape.exports == 1 && same(view(text), MC_DESCRIPTOR_EXPORT);
        }
    }
    return shape;
}
u8 g_image[64 * 1024];
void check_shape(HMODULE, const wchar_t* leaf, const char* label) noexcept {
    wchar_t path[MAX_PATH];
    char text[1024];
    u32 size = 0;
    bool read = sibling_path(leaf, path) && path_to_utf8(path, text, sizeof(text)) != 0;
    const long file = read ? sys_open(text, kOpenReadOnly) : -1;
    if (!syscall_failed(file)) {
        for (long got = 1; got > 0 && size < sizeof(g_image);) {
            got = sys_read(static_cast<int>(file), g_image + size, sizeof(g_image) - size);
            if (got > 0)
                size += static_cast<u32>(got);
        }
        sys_close(static_cast<int>(file));
    }
    const ElfShape shape = elf_shape(g_image, size);
    check(size && size < sizeof(g_image) && shape.x86_64 && !shape.entry && shape.needed == 0 && shape.undefined == 0 &&
              shape.tls == 0 && shape.exports == 1 && shape.descriptor_export,
          label);
}
void* module_symbol(HMODULE module, const char* name) noexcept {
    return shared_library_symbol(SharedLibrary{module}, name);
}
void close_module(HMODULE module) noexcept {
    SharedLibrary library{module};
    shared_library_close(&library);
}
#endif

// Loads a sample and checks its image shape; null (and a failed check) when it can't.
const mc_descriptor* open(const wchar_t* leaf, HMODULE* module, const char* label) noexcept {
    *module = load_sibling(leaf);
    check(*module != nullptr, label);
    if (!*module)
        return nullptr;
    check_shape(*module, leaf, label);
    const auto* d = reinterpret_cast<const mc_descriptor*>(module_symbol(*module, MC_DESCRIPTOR_EXPORT));
    check(d && d->size == sizeof(mc_descriptor) && d->abi == MC_ABI && d->api_level == MC_API_LEVEL && !d->flags,
          label);
    return d;
}
const mc_hook* hook_of(const mc_descriptor* d, u32 kind) noexcept {
    for (u32 i = 0; i < d->hook_count; ++i)
        if (d->hooks[i].kind == kind)
            return &d->hooks[i];
    return nullptr;
}
// A settings block filled at the descriptor's offsets.
struct Block {
    alignas(8) u8 bytes[MC_SETTINGS_BYTES_MAX];
    template <class T> void put(const mc_descriptor* d, u32 index, T value) noexcept {
        copy_bytes(bytes + d->settings[index].offset, &value, sizeof(T));
    }
};
alignas(64) u8 g_state[16384];

char g_buffer[MC_DRAFT_TEXT_MAX];
struct Run {
    mc_status status;
    u32 verdict;
    u32 flags;
    mc::Str text;
};
Run run(const mc_hook& hook, mc_ctx* ctx, const char* text, u32 capacity = MC_DRAFT_TEXT_MAX) noexcept {
    mc_draft draft{};
    draft.channel_id = 10;
    draft.text = view(text);
    draft.buffer = g_buffer;
    draft.buffer_capacity = capacity;
    const mc_status status = hook.fn.draft(ctx, &draft);
    return {status, draft.verdict, draft.flags,
            mc::Str(g_buffer, draft.verdict == MC_DRAFT_REPLACE || draft.verdict == MC_DRAFT_CONFIRM
                                  ? draft.replaced_bytes
                                  : 0)};
}
char g_reply[MC_REPLY_TEXT_MAX];
mc_command_call command_call(const mc_arg* args, u32 count) noexcept {
    mc_command_call call{};
    call.channel_id = 10;
    call.args = args;
    call.arg_count = count;
    call.reply = g_reply;
    call.reply_capacity = sizeof(g_reply);
    return call;
}

// The block clear-urls' descriptor describes: on_edit (TOGGLE at 0), extra (TEXT_LIST at 8).
struct ClearUrlsSettings {
    u8 on_edit;
    u8 padding[7];
    mc_list extra;
};

void clear_urls() noexcept {
    g_host = {};
    HMODULE module = nullptr;
    const mc_descriptor* d = open(L"clear-urls.dll", &module, "clear-urls: loads as a one-export image");
    if (!d)
        return;
    check(d->owner_id == 0x6D63'636C'7572'6C73ull && same(d->name, "Clear URLs") && same(d->version, "1.0.0") &&
              same(d->summary, "Removes tracking parameters from links you send.") && d->author.bytes == 0,
          "clear-urls: descriptor metadata");
    check(d->permissions == MC_PERM_MESSAGE_SEND && d->state_bytes == 0 && d->state_align == 0 &&
              d->network_count == 0 && d->command_count == 0 && d->menu_count == 0 && d->button_count == 0 &&
              !d->start && !d->stop,
          "clear-urls: permissions implied by its hooks, no state, lifecycle or UI");
    const mc_setting_def* s = d->settings;
    check(d->setting_count == 2 && d->settings_bytes == sizeof(ClearUrlsSettings) &&
              same(s[0].key, "on_edit") && s[0].type == MC_SET_TOGGLE && s[0].offset == 0 &&
              s[0].default_int == 1 && same(s[0].label, "Also clean edited messages") &&
              same(s[1].key, "extra") && s[1].type == MC_SET_TEXT_LIST &&
              s[1].offset == offsetof(ClearUrlsSettings, extra) &&
              same(s[1].hint, "One per line, e.g. si") && !s[1].items && s[1].item_count == 0,
          "clear-urls: settings schema");
    check(d->hook_count == 3 && d->hooks[0].kind == MC_HOOK_PRE_SEND && d->hooks[1].kind == MC_HOOK_PRE_EDIT &&
              d->hooks[2].kind == MC_HOOK_COMPLETION && d->hooks[0].fn.draft && d->hooks[1].fn.draft &&
              d->hooks[0].flags == 0 && d->hooks[0].filter == 0,
          "clear-urls: hooks from parameter types, plus the layer's completion hook");

    const mc_str extra_items[] = {MC_S("si")};
    ClearUrlsSettings settings{1, {}, {extra_items, 1, 0}};
    mc_ctx ctx{&kApi, nullptr, &settings, 0, 1};
    const mc_hook& pre_send = d->hooks[0];
    const mc_hook& pre_edit = d->hooks[1];
    Run result = run(pre_send, &ctx, "go https://ex.com/a?utm_source=news&id=7&fbclid=Z ok");
    check(result.status == MC_OK && result.verdict == MC_DRAFT_REPLACE &&
              same(result.text, "go https://ex.com/a?id=7 ok"),
          "clear-urls: pre_send strips tracking parameters");
    result = run(pre_send, &ctx, "https://youtu.be/x?si=abc");
    check(result.verdict == MC_DRAFT_REPLACE && same(result.text, "https://youtu.be/x"),
          "clear-urls: pre_send strips the extra parameters setting");
    result = run(pre_send, &ctx, "plain https://ex.com/?id=1");
    check(result.status == MC_OK && result.verdict == MC_DRAFT_KEEP, "clear-urls: clean text is kept");
    result = run(pre_send, &ctx, "https://ex.com/?utm_source=a", 4);
    check(result.verdict == MC_DRAFT_KEEP, "clear-urls: a replacement over the buffer capacity is dropped");
    result = run(pre_edit, &ctx, "https://ex.com/?gclid=1");
    check(result.verdict == MC_DRAFT_REPLACE && same(result.text, "https://ex.com/"),
          "clear-urls: pre_edit strips");
    settings.on_edit = 0;
    result = run(pre_edit, &ctx, "https://ex.com/?gclid=1");
    check(result.verdict == MC_DRAFT_KEEP, "clear-urls: pre_edit honours on_edit");
    mc_completion stray{};
    stray.kind = MC_COMPLETION_HTTP;
    stray.tag = 99;
    d->hooks[2].fn.completion(&ctx, &stray);
    check(g_host.logs + g_host.toasts + g_host.sets + g_host.gets + g_host.removes + g_host.setting_writes +
                  g_host.https ==
              0,
          "clear-urls: no host calls; a completion nobody asked for is ignored");
    close_module(module);
}

// ---- Translate -----------------------------------------------------------------

mc_message message(u64 id, const char* content) noexcept {
    mc_message m{};
    m.message_id = id;
    m.channel_id = 10;
    m.content = view(content);
    m.revision = 1;
    return m;
}
void complete_http(const mc_hook& hook, mc_ctx* ctx, u64 tag, u32 status, const char* body,
                   u32 error = MC_HTTP_OK) noexcept {
    mc_completion done{};
    done.kind = MC_COMPLETION_HTTP;
    done.tag = tag;
    done.error = error;
    done.status = status;
    done.body = body;
    done.body_bytes = view(body).bytes;
    hook.fn.completion(ctx, &done);
}

void translate() noexcept {
    g_host = {};
    clear_bytes(g_state, sizeof(g_state));
    HMODULE module = nullptr;
    const mc_descriptor* d = open(L"translate.dll", &module, "translate: loads as a one-export image");
    if (!d)
        return;
    const mc_setting_def* s = d->settings;
    check(d->setting_count == 4 && s[0].type == MC_SET_CHOICE && s[0].item_count == 5 &&
              same(s[0].items[3], "Japanese") && s[1].type == MC_SET_TOGGLE && s[2].type == MC_SET_CHOICE &&
              s[3].type == MC_SET_SECRET && same(s[3].key, "deepl_key") && s[3].visible_if == 3 &&
              s[3].visible_value == 1,
          "translate: settings, the DeepL key shown only for DeepL");
    check(d->network_count == 3 && same(d->network[0], "translate.googleapis.com") &&
              same(d->network[2], "api.deepl.com"),
          "translate: the declared hosts");
    check(d->permissions == (MC_PERM_COMPOSER | MC_PERM_MESSAGE_SEND | MC_PERM_READ_MESSAGES | MC_PERM_NETWORK),
          "translate: declared and implied permissions");
    const mc_hook* decorate = hook_of(d, MC_HOOK_DECORATE_MESSAGE);
    const mc_hook* hide = hook_of(d, MC_HOOK_NODE_ACTION);
    const mc_hook* pre_send = hook_of(d, MC_HOOK_PRE_SEND);
    const mc_hook* completion = hook_of(d, MC_HOOK_COMPLETION);
    check(d->hook_count == 4 && decorate && decorate->flags == (MC_NEED_CONTENT | MC_NEED_AUTHOR) && hide &&
              pre_send && completion,
          "translate: hooks");
    check(d->button_count == 1 && d->buttons[0].surface == MC_SURFACE_MESSAGE_POPOVER &&
              same(d->buttons[0].label, "Translate") && same(d->buttons[0].icon, "lucide/languages") &&
              d->buttons[0].visible && d->command_count == 1 && same(d->commands[0].name, "tr") &&
              d->commands[0].arg_count == 1 && same(d->commands[0].args[0].name, "text") &&
              d->commands[0].args[0].type == MC_ARG_TEXT && d->commands[0].args[0].flags == MC_ARG_REQUIRED &&
              same(d->commands[0].args[0].description, "The text to translate"),
          "translate: a popover button and a typed command");
    check(d->state_bytes <= sizeof(g_state), "translate: state fits the test's block");
    if (!decorate || !hide || !pre_send || !completion || !d->button_count || !d->command_count ||
        d->state_bytes > sizeof(g_state))
        return;

    Block settings{};
    settings.put(d, 0, u32{0}); // English
    mc_ctx ctx{&kApi, g_state, settings.bytes, MC_CTX_GESTURE, 1};
    const mc_message hello = message(77, "Hallo Welt");
    const mc_message empty = message(78, "");
    mc_button_event event{10, 0, &hello, 0, 0};
    mc_button_event blank{10, 0, &empty, 0, 0};
    check(d->buttons[0].visible(&ctx, &event) == 1 && d->buttons[0].visible(&ctx, &blank) == 0,
          "translate: the button shows on messages with text");

    check(d->buttons[0].click(&ctx, &event) == MC_OK && g_host.https == 1 && g_host.http_method == MC_HTTP_POST &&
              same(g_host.url,
                   "https://translate.googleapis.com/translate_a/single?client=gtx&sl=auto&dt=t&tl=en") &&
              same(g_host.body, "q=Hallo%20Welt") && g_host.http_headers == 1 && g_host.http_tag != 0,
          "translate: a click posts the text to Google");
    complete_http(*completion, &ctx, g_host.http_tag, 200,
                  R"([[["Hello ","Hallo ",null,null,10],["world","Welt",null,null,10]],null,"de"])");
    mc_decoration out{};
    check(g_host.invalidates == 1 && g_host.invalidate_id == 77 &&
              decorate->fn.decorate_message(&ctx, &hello, &out) == MC_OK && out.has_card &&
              same(out.card.text, "Hello world") && same(out.card.caption, "Translated") &&
              out.card.button_count == 1 && out.card.buttons[0].tag == 77,
          "translate: the answer becomes a card under the message");
    complete_http(*completion, &ctx, g_host.http_tag, 200, "[]");
    check(g_host.toasts == 0, "translate: a completion is delivered once");

    const mc_node_action press{77, 10, 77, 0, 0, MC_HOOK_DECORATE_MESSAGE, MC_NODE_CARD_BUTTON};
    out = {};
    check(hide->fn.node_action(&ctx, &press) == MC_OK && g_host.invalidates == 2 &&
              decorate->fn.decorate_message(&ctx, &hello, &out) == MC_OK && !out.has_card,
          "translate: Hide removes the card");

    d->buttons[0].click(&ctx, &event);
    complete_http(*completion, &ctx, g_host.http_tag, 0, "", MC_HTTP_E_NETWORK);
    check(g_host.toasts == 1 && g_host.tone == MC_TONE_CRITICAL && same(g_host.text, "Translation failed"),
          "translate: a failed request is a toast");

    settings.put(d, 2, u32{1}); // DeepL
    settings.put(d, 3, mc_str MC_S("key:fx"));
    settings.put(d, 0, u32{1}); // German
    d->buttons[0].click(&ctx, &event);
    check(same(g_host.url, "https://api-free.deepl.com/v2/translate") &&
              same(g_host.body, "target_lang=DE&text=Hallo%20Welt") && g_host.http_headers == 2 &&
              same(g_host.header_name, "Authorization") && same(g_host.header_value, "DeepL-Auth-Key key:fx"),
          "translate: DeepL gets the key and the free host for :fx keys");
    complete_http(*completion, &ctx, g_host.http_tag, 200,
                  R"({"translations":[{"detected_source_language":"DE","text":"Hallo Welt!"}]})");
    out = {};
    check(decorate->fn.decorate_message(&ctx, &hello, &out) == MC_OK && same(out.card.text, "Hallo Welt!"),
          "translate: DeepL's answer is read from its JSON");

    const mc_arg arg{MC_ARG_TEXT, 1, MC_S("Guten Tag"), 0, 0};
    mc_command_call call = command_call(&arg, 1);
    check(d->commands[0].run(&ctx, &call) == MC_OK && call.reply_kind == MC_REPLY_NONE &&
              same(g_host.body, "target_lang=DE&text=Guten%20Tag"),
          "translate: /tr starts a request");
    complete_http(*completion, &ctx, g_host.http_tag, 200, R"({"translations":[{"text":"Good day"}]})");
    check(g_host.inserts == 1 && same(g_host.text, "Good day"), "translate: /tr's result goes into the composer");

    settings.put(d, 1, u8{1});
    const Run asked = run(*pre_send, &ctx, "hi");
    check(asked.verdict == MC_DRAFT_CONFIRM && asked.text.size() != 0,
          "translate: outgoing messages ask before they're sent untranslated");
    close_module(module);
}

// ---- Show Me Your Name, Reverse Image Search, Silent Toggle --------------------------

void show_me_your_name() noexcept {
    g_host = {};
    HMODULE module = nullptr;
    const mc_descriptor* d = open(L"show-me-your-name.dll", &module, "show-me-your-name: loads as a one-export image");
    if (!d)
        return;
    const mc_hook* decorate = hook_of(d, MC_HOOK_DECORATE_USER);
    check(decorate && decorate->filter == ((1u << MC_USER_MESSAGE_AUTHOR) | (1u << MC_USER_MEMBER_ROW)) &&
              d->permissions == 0 && d->setting_count == 3,
          "show-me-your-name: a user decoration for authors and member rows, no permissions");
    if (!decorate)
        return;
    Block settings{};
    settings.put(d, 0, u32{0});
    settings.put(d, 1, u8{1});
    settings.put(d, 2, u8{1});
    mc_ctx ctx{&kApi, nullptr, settings.bytes, 0, 1};
    mc_user user{};
    user.where = MC_USER_MESSAGE_AUTHOR;
    user.name = MC_S("Mai");
    user.username = MC_S("mai.c");
    mc_decoration out{};
    check(decorate->fn.decorate_user(&ctx, &user, &out) == MC_OK && same(out.name_suffix, "mai.c") &&
              !out.name_prefix.bytes,
          "show-me-your-name: the username follows the name");
    user.name = MC_S("MAI.C");
    out = {};
    decorate->fn.decorate_user(&ctx, &user, &out);
    check(!out.name_suffix.bytes, "show-me-your-name: matching names get nothing");
    settings.put(d, 0, u32{1});
    user.name = MC_S("Mai");
    out = {};
    decorate->fn.decorate_user(&ctx, &user, &out);
    check(same(out.name_prefix, "mai.c") && out.name_tone == MC_TONE_ROLE,
          "show-me-your-name: or comes first in the role colour");
    settings.put(d, 1, u8{0});
    user.where = MC_USER_MEMBER_ROW;
    out = {};
    decorate->fn.decorate_user(&ctx, &user, &out);
    check(!out.name_prefix.bytes && !out.name_suffix.bytes, "show-me-your-name: member rows follow their setting");
    close_module(module);
}

void reverse_image_search() noexcept {
    g_host = {};
    HMODULE module = nullptr;
    const mc_descriptor* d =
        open(L"reverse-image-search.dll", &module, "reverse-image-search: loads as a one-export image");
    if (!d)
        return;
    const mc_menu_def* m = d->menus;
    check(d->menu_count == 5 && m[0].flags == MC_MENU_SUBMENU && !m[0].select && m[0].visible &&
              m[0].targets == MC_MENU_IMAGE && same(m[0].label, "Search image") && m[1].parent == 1 &&
              m[4].parent == 1 && same(m[1].label, "Google Lens") && same(m[4].label, "TinEye") && m[4].visible &&
              !m[1].visible && m[1].select && d->permissions == 0,
          "reverse-image-search: a submenu with one item per engine");
    if (d->menu_count != 5)
        return;
    Block settings{};
    settings.put(d, 0, u8{0});
    mc_ctx ctx{&kApi, nullptr, settings.bytes, MC_CTX_GESTURE, 1};
    mc_menu_target target{};
    target.kind = MC_MENU_IMAGE;
    check(m[0].visible(&ctx, &target) == MC_MENU_HIDE, "reverse-image-search: hidden without a URL");
    target.url = MC_S("https://cdn.discordapp.com/a b.png");
    check(m[0].visible(&ctx, &target) == MC_MENU_SHOW && m[4].visible(&ctx, &target) == MC_MENU_HIDE,
          "reverse-image-search: shown for images; TinEye follows its setting");
    check(m[1].select(&ctx, &target) == MC_OK && g_host.opens == 1 &&
              same(g_host.text, "https://lens.google.com/uploadbyurl?url=https%3A%2F%2Fcdn.discordapp.com%2Fa%20b.png"),
          "reverse-image-search: opens the engine with the encoded image URL");
    close_module(module);
}

void silent_toggle() noexcept {
    g_host = {};
    HMODULE module = nullptr;
    const mc_descriptor* d = open(L"silent-toggle.dll", &module, "silent-toggle: loads as a one-export image");
    if (!d)
        return;
    const mc_hook* pre_send = hook_of(d, MC_HOOK_PRE_SEND);
    check(pre_send && d->button_count == 1 && d->buttons[0].flags == MC_BUTTON_TOGGLE &&
              d->buttons[0].surface == MC_SURFACE_CHAT_BAR && d->command_count == 1 &&
              same(d->commands[0].name, "silent") && d->commands[0].arg_count == 1 &&
              d->commands[0].args[0].type == MC_ARG_BOOL && d->commands[0].args[0].flags == 0 &&
              (d->settings[0].flags & MC_SET_HIDDEN) && d->start && d->permissions == MC_PERM_MESSAGE_SEND,
          "silent-toggle: a toggle button, an optional-argument command and pre_send");
    if (!pre_send || !d->command_count || !d->button_count || !d->start)
        return;
    Block settings{};
    mc_ctx ctx{&kApi, nullptr, settings.bytes, MC_CTX_GESTURE, 1};
    check(d->start(&ctx) == MC_OK && g_host.buttons == 1 && g_host.button_flags == 0 &&
              same(g_host.button_tooltip, "Silent messages: off"),
          "silent-toggle: start shows the saved state");
    const mc_arg omitted{MC_ARG_BOOL, 0, {}, 0, 0};
    mc_command_call call = command_call(&omitted, 1);
    check(d->commands[0].run(&ctx, &call) == MC_OK && call.reply_kind == MC_REPLY_TOAST &&
              same(mc::Str(call.reply, call.reply_bytes), "Messages are sent silently") &&
              g_host.setting_index == 0 && g_host.setting_value[0] == 1 && g_host.button_flags == MC_BUTTON_ACTIVE,
          "silent-toggle: /silent with no argument switches it on");
    const mc_arg off{MC_ARG_BOOL, 1, {}, 0, 0};
    call = command_call(&off, 1);
    check(d->commands[0].run(&ctx, &call) == MC_OK && g_host.setting_value[0] == 0 && g_host.button_flags == 0,
          "silent-toggle: /silent off");
    check(run(*pre_send, &ctx, "hi").flags == 0, "silent-toggle: off leaves messages alone");
    settings.put(d, 0, u8{1});
    check(run(*pre_send, &ctx, "hi").flags == MC_DRAFT_SILENT, "silent-toggle: on makes messages silent");
    mc_button_event press{10, 0, nullptr, 0, 0};
    const u32 writes = g_host.setting_writes;
    check(d->buttons[0].click(&ctx, &press) == MC_OK && g_host.setting_writes == writes + 1 &&
              g_host.setting_value[0] == 0,
          "silent-toggle: the button switches it");
    close_module(module);
}

// ---- The plain C fixture ----------------------------------------------------------

void c_fixture() noexcept {
    g_host = {};
    HMODULE module = nullptr;
    const mc_descriptor* d = open(L"MaiCordPluginAbiCFixture.dll", &module, "C fixture: loads as a one-export image");
    if (!d)
        return;
    check(same(d->name, "C fixture") && d->setting_count == 3 && d->settings_bytes == 24 &&
              d->settings[1].type == MC_SET_CHOICE && d->settings[1].item_count == 2 &&
              same(d->settings[1].items[1], "Long") && d->settings[2].visible_if == 2 &&
              d->settings[2].visible_value == 1 && d->hook_count == 2 && d->state_bytes == 16 && d->start &&
              d->stop && d->command_count == 1 && d->menu_count == 2 && d->button_count == 1,
          "C fixture: descriptor");
    if (d->hook_count != 2 || !d->command_count || d->menu_count != 2 || !d->button_count)
        return;
    u32 state[4] = {};
    const u8 settings[24] = {1, 0, 0, 0, 1};
    mc_ctx ctx{&kApi, state, settings, MC_CTX_FIRST_START, 1};
    check(d->start(&ctx) == MC_OK && state[0] == 1, "C fixture: start");
    Run result = run(d->hooks[0], &ctx, "hi");
    check(result.verdict == MC_DRAFT_REPLACE && same(result.text, "[c] hi"),
          "C fixture: pre_send replaces");
    result = run(d->hooks[0], &ctx, "cancel");
    check(result.verdict == MC_DRAFT_CANCEL, "C fixture: pre_send cancels");
    const mc_message m = message(5, "text");
    mc_decoration out{};
    check(d->hooks[1].fn.decorate_message(&ctx, &m, &out) == MC_OK && out.badge_count == 1 &&
              same(out.badges[0].text, "C"),
          "C fixture: decorate_message adds a badge");
    const mc_arg arg{MC_ARG_TEXT, 1, MC_S("hey"), 0, 0};
    mc_command_call call = command_call(&arg, 1);
    check(d->commands[0].run(&ctx, &call) == MC_OK && call.reply_kind == MC_REPLY_INSERT &&
              same(mc::Str(call.reply, call.reply_bytes), "HEY"),
          "C fixture: /shout");
    mc_menu_target link{};
    link.kind = MC_MENU_LINK;
    link.url = MC_S("https://a.b");
    check(d->menus[1].visible(&ctx, &link) == MC_MENU_SHOW && d->menus[1].select(&ctx, &link) == MC_OK &&
              state[2] == 11,
          "C fixture: the submenu item");
    mc_button_event press{};
    check(d->buttons[0].click(&ctx, &press) == MC_OK && state[3] == 1, "C fixture: the button");
    d->stop(&ctx);
    check(state[1] == 1, "C fixture: stop");
    close_module(module);
}

} // namespace
} // namespace maicord::plugin_abi_tests

extern "C" __declspec(noreturn) void WINAPI maicord_plugin_abi_tests_entry() noexcept {
    using namespace maicord::plugin_abi_tests;
    url_cases();
    ctx_wrappers();
    clear_urls();
    translate();
    show_me_your_name();
    reverse_image_search();
    silent_toggle();
    c_fixture();
    maicord::test_exit(failures, "plugin_abi");
}
