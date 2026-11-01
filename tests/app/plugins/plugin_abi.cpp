// ABI 6 SDK: the C++ layer's helpers and wrappers, and the clear-urls sample
// and plain C fixture DLLs: their PE shape (no entry point, no TLS, no
// imports, one export), their descriptors, and their hooks called directly.
#include "maicord/sdk/plugin.hpp"

#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "../../support/test_output.hpp"

// Frozen layouts (the C fixture asserts every field; these guard the C++ view).
static_assert(sizeof(mc_ctx) == 32 && sizeof(mc_setting_def) == 128 && sizeof(mc_hook) == 24);
static_assert(sizeof(mc_draft) == 72 && offsetof(mc_draft, verdict) == 68);
static_assert(sizeof(mc_descriptor) == 248 && offsetof(mc_descriptor, hooks) == 160 &&
              offsetof(mc_descriptor, start) == 232);
static_assert(sizeof(mc_api) == 56 && offsetof(mc_api, toast) == 48);

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

// ---- url::strip_params ---------------------------------------------------------

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
}

// ---- Ctx wrappers over a fake host table ----------------------------------------

struct FakeHost {
    u32 logs, toasts, sets, gets, removes, setting_writes;
    u32 level, tone, setting_index, setting_bytes;
    u8 setting_value[8];
    char key[16];
    u32 key_bytes;
    u8 value[16];
    u32 value_bytes;
    bool stored;
} g_host;

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
mc_status MC_CALL host_toast(mc_ctx*, mc_str, uint32_t tone, uint32_t flags) {
    ++g_host.toasts;
    g_host.tone = tone;
    return flags == 0 ? MC_OK : MC_E_INVALID;
}
constexpr mc_api kApi{
    sizeof(mc_api),    1,         host_log, host_set_setting, host_store_get, host_store_set,
    host_store_remove, host_toast};

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
}

// ---- Plugin DLLs ---------------------------------------------------------------

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

char g_buffer[MC_DRAFT_TEXT_MAX];
struct Run {
    mc_status status;
    u32 verdict;
    mc::Str text;
};
Run run(const mc_hook& hook, mc_ctx* ctx, const char* text, u32 capacity = MC_DRAFT_TEXT_MAX) noexcept {
    mc_draft draft{};
    draft.channel_id = 10;
    draft.text = view(text);
    draft.buffer = g_buffer;
    draft.buffer_capacity = capacity;
    const mc_status status = hook.fn.draft(ctx, &draft);
    return {status, draft.verdict,
            mc::Str(g_buffer, draft.verdict == MC_DRAFT_REPLACE ? draft.replaced_bytes : 0)};
}

// The block clear-urls' descriptor describes: on_edit (TOGGLE at 0), extra (TEXT_LIST at 8).
struct ClearUrlsSettings {
    u8 on_edit;
    u8 padding[7];
    mc_list extra;
};

void clear_urls() noexcept {
    g_host = {};
    const HMODULE module = load_sibling(L"clear-urls.dll");
    check(module != nullptr, "clear-urls: loads");
    if (!module)
        return;
    check_pe(module, "clear-urls: no entry point, imports or TLS; one export, the descriptor");
    const auto* d = reinterpret_cast<const mc_descriptor*>(GetProcAddress(module, MC_DESCRIPTOR_EXPORT));
    check(d && d->size == sizeof(mc_descriptor) && d->abi == MC_ABI && d->api_level == 1 &&
              d->owner_id == 0x6D63'636C'7572'6C73ull && same(d->name, "Clear URLs") &&
              same(d->version, "1.0.0") &&
              same(d->summary, "Removes tracking parameters from links you send.") && d->author.bytes == 0,
          "clear-urls: descriptor metadata");
    if (!d)
        return;
    check(d->permissions == MC_PERM_MESSAGE_SEND && d->state_bytes == 0 && d->state_align == 0 &&
              d->network_count == 0 && d->command_count == 0 && !d->start && !d->stop && d->flags == 0,
          "clear-urls: permissions implied by its hooks, no state or lifecycle");
    const mc_setting_def* s = d->settings;
    check(d->setting_count == 2 && d->settings_bytes == sizeof(ClearUrlsSettings) &&
              same(s[0].key, "on_edit") && s[0].type == MC_SET_TOGGLE && s[0].offset == 0 &&
              s[0].default_int == 1 && same(s[0].label, "Also clean edited messages") &&
              same(s[1].key, "extra") && s[1].type == MC_SET_TEXT_LIST &&
              s[1].offset == offsetof(ClearUrlsSettings, extra) &&
              same(s[1].hint, "One per line, e.g. si") && !s[1].items && s[1].item_count == 0,
          "clear-urls: settings schema");
    check(d->hook_count == 2 && d->hooks[0].kind == MC_HOOK_PRE_SEND &&
              d->hooks[1].kind == MC_HOOK_PRE_EDIT && d->hooks[0].fn.draft && d->hooks[1].fn.draft &&
              d->hooks[0].flags == 0 && d->hooks[0].filter == 0,
          "clear-urls: hooks from parameter types");

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
    check(g_host.logs + g_host.toasts + g_host.sets + g_host.gets + g_host.removes +
                  g_host.setting_writes ==
              0,
          "clear-urls: no host calls");
    FreeLibrary(module);
}

void c_fixture() noexcept {
    const HMODULE module = load_sibling(L"MaiCordPluginAbiCFixture.dll");
    check(module != nullptr, "C fixture: loads");
    if (!module)
        return;
    check_pe(module, "C fixture: no entry point, imports or TLS; one export, the descriptor");
    const auto* d = reinterpret_cast<const mc_descriptor*>(GetProcAddress(module, MC_DESCRIPTOR_EXPORT));
    check(d && d->abi == MC_ABI && same(d->name, "C fixture") && d->setting_count == 2 &&
              d->settings_bytes == 8 && d->settings[1].type == MC_SET_CHOICE &&
              d->settings[1].item_count == 2 && same(d->settings[1].items[1], "Long") &&
              d->hook_count == 1 && d->state_bytes == 8 && d->start && d->stop,
          "C fixture: descriptor");
    if (!d || !d->start || !d->stop || d->hook_count != 1)
        return;
    u32 state[2] = {};
    const u8 settings[8] = {1, 0, 0, 0, 1, 0, 0, 0};
    mc_ctx ctx{&kApi, state, settings, MC_CTX_FIRST_START, 1};
    check(d->start(&ctx) == MC_OK && state[0] == 1, "C fixture: start");
    Run result = run(d->hooks[0], &ctx, "hi");
    check(result.verdict == MC_DRAFT_REPLACE && same(result.text, "[c] hi"),
          "C fixture: pre_send replaces");
    result = run(d->hooks[0], &ctx, "cancel");
    check(result.verdict == MC_DRAFT_CANCEL, "C fixture: pre_send cancels");
    d->stop(&ctx);
    check(state[1] == 1, "C fixture: stop");
    FreeLibrary(module);
}

} // namespace
} // namespace maicord::plugin_abi_tests

extern "C" __declspec(noreturn) void WINAPI maicord_plugin_abi_tests_entry() noexcept {
    using namespace maicord::plugin_abi_tests;
    url_cases();
    ctx_wrappers();
    clear_urls();
    c_fixture();
    maicord::test_exit(failures, "plugin_abi");
}
