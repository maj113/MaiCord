// The catalog service (src/app/plugins/catalog_service.cpp and catalog_worker.cpp) against a fake catalog
// server: a signed format-2 index naming the clear-urls sample plugin (and a patched 1.1.0 build of it), a
// theme and an effect. Covers install with consent, the plugin then stripping links through the pre-send
// hook, digest failures, automatic update, rollback, startup enabling from catalog-state.bin, replayed and
// unsigned indexes, revocation, removal, a declined consent, a refresh while the consent dialog is open, the
// Developer Mode gate on sideloads and a sideloaded plugin's host switches surviving a restart (the translate sample).
#include "plugins/catalog_service.hpp"
#include "plugins/catalog_worker.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/app/packages/mcfx.hpp"
#include "maicord/app/settings/client_theme_document.hpp"
#include "maicord/app/plugins/binding.hpp"
#include "maicord/app/plugins/consent.hpp"
#include "maicord/app/plugins/dispatch.hpp"
#include "maicord/app/plugins/pipeline.hpp"
#include "maicord/core/memory.hpp"
#include "catalog_fixture.hpp"
#include "fake_transport.hpp"
#include "fake_actions.hpp"

namespace maicord::catalog_service_tests {
using namespace app_detail;
using namespace catalog_fixture;

u32 failures;
void check(bool value, const char* label) noexcept {
    if (value) return;
    ++failures;
    test_write_text("FAIL catalog_service: ");
    test_write_line(label);
}

constexpr char kOrigin[] = "maj113.github.io";
constexpr Utf8View kSlug = utf8_literal("clear-urls");
constexpr u64 kDay = 86400;

// The catalog: two builds of the plugin, a theme, an effect and a details page.
struct Blob {
    u8* data;
    u32 bytes;
    u8 sha256[32];
    char target[128];
};
struct Catalog {
    plugin_fake::Server server;
    Blob plugin[2]; // 1.0.0, 1.1.0
    Blob theme, effect, details, corrupt;
    u64 permissions;
    Text document;
    u8 signature[64];
    char index_target[64], signature_target[64];
};
Catalog* g;
SigningKey g_key;
u64 g_now = 1760400000;
u64 clock() noexcept { return g_now; }

// Host stubs.
bool g_developer;
u32 g_toasts, g_library_changes, g_removed, g_deactivated;
bool developer_mode() noexcept { return g_developer; }
Utf8View g_last_toast; // the service's toasts are literals
void toast(Utf8View text, bool) noexcept {
    ++g_toasts;
    g_last_toast = text;
}
bool validate_package(CatalogKind kind, ConstSlice<u8> bytes, Utf8View) noexcept {
    return kind != CatalogKind::plugin && bytes.count && bytes.data[0] == '{';
}
void library_changed() noexcept { ++g_library_changes; }
void package_removed(CatalogKind, Utf8View, bool keep) noexcept { ++(keep ? g_deactivated : g_removed); }

void blob(Blob& b, const char* folder, const char* extension) noexcept {
    g_key.digest({b.data, b.bytes}, b.sha256);
    Text t{};
    t.add("/MaiCord-plugins/").add(folder).digest(b.sha256).add(extension);
    copy_bytes(b.target, t.data, t.bytes);
    b.target[t.bytes] = 0;
}
bool make_blob(Blob& b, const char* text, const char* folder, const char* extension) noexcept {
    b.bytes = c_string_length(text);
    b.data = static_cast<u8*>(VirtualAlloc(nullptr, b.bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!b.data) return false;
    copy_bytes(b.data, text, b.bytes);
    blob(b, folder, extension);
    return true;
}

// The plugin's effective permissions, read the way the client reads them.
bool read_permissions(const wchar_t* directory) noexcept {
    PluginDirectory pinned{};
    PluginImage image{};
    bool ok = plugin_directory_open(&pinned, {directory, static_cast<u32>(wide_length(directory, MAX_PATH))}).ok() &&
              plugin_image_load(&image, pinned, {L"clear-urls.dll", 14}, ~0ull).ok();
    if (ok) {
        g->permissions = image.permissions;
        ok = utf8_equal({image.info.version, image.info.version_bytes}, utf8_literal("1.0.0"));
        plugin_image_unload(&image);
    }
    plugin_directory_close(&pinned);
    return ok;
}

bool setup(const wchar_t* directory) noexcept {
    wchar_t path[MAX_PATH];
    wide_copy(path, wide_copy(path, 0, directory), L"\\clear-urls.dll");
    Blob& first = g->plugin[0];
    first.data = read_file(path, &first.bytes);
    Blob& second = g->plugin[1];
    second.data = read_file(path, &second.bytes);
    if (!first.data || !second.data || !read_permissions(directory)) return false;
    // 1.1.0: the same plugin with its version text patched, so the descriptor and the listing agree.
    u32 found = 0, at = 0;
    for (u32 i = 0; i + 5 <= second.bytes; ++i)
        if (bytes_equal(second.data + i, "1.0.0", 5)) ++found, at = i;
    if (found != 1) return false;
    second.data[at + 2] = '1';
    blob(first, "files/", ".dll");
    blob(second, "files/", ".dll");
    return make_blob(g->theme, "{\"version\":\"2.0\",\"name\":\"Fjord\"}", "files/", ".json") &&
           make_blob(g->effect, "{effect}", "files/", ".mcfx") && make_blob(g->details, "# Clear URLs\n", "details/", ".md") &&
           make_blob(g->corrupt, "{\"version\":\"2.0\",\"name\":\"Fjorx\"}", "files/", ".json");
}

struct Publish {
    u64 sequence;
    bool offer_update;    // list 1.1.0 above 1.0.0
    bool corrupt_theme;   // serve other bytes for the theme
    bool bad_signature;
    const u8* revoked;    // a malicious revocation
    const u8* broken;     // a broken one
    u32 effect_interface; // the effect's .mcfx interface, 0 to leave it out
    u32 theme_interface;  // the theme's document interface, 0 to leave it out
};
void file(Text& t, const Blob& b) noexcept {
    t.add("\"file\":{\"sha256\":\"").digest(b.sha256).add("\",\"bytes\":").number(b.bytes).add("}");
}
void common(Text& t, const char* kind, const char* slug, const char* name) noexcept {
    t.add("{\"kind\":\"").add(kind).add("\",\"slug\":\"").add(slug).add("\",\"name\":\"").add(name)
        .add("\",\"summary\":\"A test item.\",\"authors\":[{\"name\":\"MaiCord\"}],\"license\":\"MPL-2.0\","
             "\"source\":\"https://github.com/maj113/MaiCord-plugins\",\"categories\":[\"privacy\"],\"details\":{\"sha256\":\"")
        .digest(g->details.sha256).add("\",\"bytes\":").number(g->details.bytes).add("},\"previews\":[");
}
void plugin_version(Text& t, const char* version, u64 released, const Blob& b) noexcept {
    t.add("{\"version\":\"").add(version).add("\",\"released\":").number(released).add(",\"api_level\":").number(MC_API_LEVEL)
        .add(",\"permissions\":[");
    bool first = true;
    for (u32 bit = 0; bit < 11; ++bit)
        if (g->permissions & (1ull << bit)) t.add(first ? "\"" : ",\"").add(catalog_permission_name(bit)).add("\""), first = false;
    t.add("],\"network\":[],");
    file(t, b);
    t.add("}");
}
void revocation(Text& t, const u8* sha, const char* severity) noexcept {
    u8 digest[32];
    copy_bytes(digest, sha, 32);
    t.add("{\"sha256\":\"").digest(digest).add("\",\"severity\":\"").add(severity).add("\",\"reason\":\"Test.\",\"since\":")
        .number(g_now - 10).add("}");
}
void publish(const Publish& p) noexcept {
    Text& t = g->document;
    t.bytes = 0;
    t.add("{\"format\":2,\"sequence\":").number(p.sequence).add(",\"issued\":").number(g_now - 60).add(",\"expires\":")
        .number(g_now + 30 * kDay).add(",\"items\":[");
    common(t, "plugin", "clear-urls", "Clear URLs");
    t.add("],\"versions\":[");
    if (p.offer_update) plugin_version(t, "1.1.0", g_now - 100, g->plugin[1]), t.add(",");
    plugin_version(t, "1.0.0", g_now - 1000, g->plugin[0]);
    t.add("]},");
    common(t, "theme", "fjord", "Fjord");
    t.add("{\"sha256\":\"").digest(g->details.sha256).add("\",\"bytes\":1,\"width\":1440,\"height\":900,\"alt\":\"Fjord\"}],"
                                                         "\"version\":\"1.0.0\",\"released\":")
        .number(g_now - 1000).add(",");
    file(t, g->theme);
    if (p.theme_interface) t.add(",\"interface\":").number(p.theme_interface);
    t.add(",\"base\":\"dark\"},");
    common(t, "effect", "rain", "Rain");
    t.add("{\"sha256\":\"").digest(g->details.sha256).add("\",\"bytes\":1,\"width\":480,\"height\":270,\"alt\":\"Rain\"}],"
                                                         "\"version\":\"1.0.0\",\"released\":")
        .number(g_now - 1000).add(",");
    file(t, g->effect);
    if (p.effect_interface) t.add(",\"interface\":").number(p.effect_interface);
    t.add(",\"cost\":\"light\",\"blend\":\"normal\",\"uses\":[],\"knobs\":[]}],\"revoked\":[");
    if (p.revoked) revocation(t, p.revoked, "malicious");
    if (p.broken) t.add(p.revoked ? "," : ""), revocation(t, p.broken, "broken");
    t.add("],\"blocklist\":[]}");
    g_key.sign(t.view(), g->signature);
    if (p.bad_signature) g->signature[3] ^= 1;
    auto& s = g->server;
    plugin_fake::reset(s, kOrigin);
    // The index this system's client reads (index-3.json on Windows, index-4.json on Linux).
    // The fake server keeps the pointers, so the strings live in the catalog.
    char(&index_target)[64] = g->index_target;
    char(&signature_target)[64] = g->signature_target;
    u32 at = 0;
    for (const char* c = "/MaiCord-plugins/"; *c; ++c) index_target[at++] = *c;
    for (u32 i = 0; i < kCatalogIndexFile.bytes; ++i) index_target[at++] = kCatalogIndexFile.data[i];
    index_target[at] = 0;
    for (u32 i = 0; i <= at; ++i) signature_target[i] = index_target[i];
    for (u32 i = 0; i < 4; ++i) signature_target[at + i] = ".sig"[i];
    signature_target[at + 4] = 0;
    plugin_fake::serve(s, UpdateHost::pages, index_target, 200, t.data, t.bytes);
    plugin_fake::serve(s, UpdateHost::pages, signature_target, 200, g->signature, 64);
    for (const Blob& b : g->plugin) plugin_fake::serve(s, UpdateHost::pages, b.target, 200, b.data, b.bytes);
    const Blob& theme = p.corrupt_theme ? g->corrupt : g->theme;
    plugin_fake::serve(s, UpdateHost::pages, g->theme.target, 200, theme.data, theme.bytes);
    plugin_fake::serve(s, UpdateHost::pages, g->effect.target, 200, g->effect.data, g->effect.bytes);
    plugin_fake::serve(s, UpdateHost::pages, g->details.target, 200, g->details.data, g->details.bytes);
}

// Polls until done says so (or about ten seconds pass).
template <typename F> bool settle(F&& done) noexcept {
    for (u32 i = 0; i < 400; ++i) {
        application_catalog_poll();
        if (done()) return true;
        const HANDLE notify = application_catalog_notification();
        if (notify) WaitForSingleObject(notify, 25);
    }
    return false;
}
bool check_now() noexcept {
    const u64 before = application_catalog_view().revision;
    return application_catalog_check_now() &&
           settle([&] { return !application_catalog_view().fetching && application_catalog_view().revision != before; });
}
CatalogPhase phase() noexcept { return application_catalog_view().operation.phase; }
bool finished() noexcept { return phase() == CatalogPhase::done || phase() == CatalogPhase::failed; }
bool consent_or_finished() noexcept { return phase() == CatalogPhase::consent || finished(); }

// The pre-send hook as the composer runs it.
bool strips() noexcept {
    const Utf8View sent = utf8_literal("see https://example.com/page?utm_source=news&id=7");
    const PluginDraftResult r = plugin_dispatch_draft(PluginHook::pre_send, {1, 0, 0, 0, sent});
    return r.verdict == PluginDraftVerdict::replace && utf8_equal(r.text, utf8_literal("see https://example.com/page?id=7"));
}
const PluginBinding* running() noexcept {
    PluginRuntime* r = application_catalog_runtime();
    return r && r->binding_count == 1 ? r->bindings[0] : nullptr;
}
bool running_version(const char* version) noexcept {
    const PluginBinding* b = running();
    return b && utf8_equal({b->image.info.version, b->image.info.version_bytes}, {version, c_string_length(version)});
}

// The client's side of the plugin actions: what a plugin showed as a local message (fake_actions.cpp, which answers
// the new message's id with 4242).
constexpr Utf8View kDraft = utf8_literal("\xC5\xBEivjo **test**");
mc_status show_draft(mc_ctx* ctx, void*) noexcept {
    u64 id = 0;
    const mc_status status = ctx->api->local_message(ctx, 0, {kDraft.data, kDraft.bytes, 0}, &id);
    return status == MC_OK && id != 4242 ? MC_E_INVALID : status;
}
// A click on a plugin's button: the gesture reaches the client's actions though the plugin has no event,
// timer or kept-message hook (clear-urls has only pre_send).
bool acts() noexcept {
    PluginRuntime* r = application_catalog_runtime();
    if (!r || r->binding_count != 1) return false;
    plugin_fake_actions::recorded.local_bytes = 0;
    const PluginCallResult call = plugin_binding_call(r->bindings[0], PluginCallKind::slot_action, PluginCallClass::gesture,
                                                      MC_CTX_GESTURE, show_draft, nullptr);
    const auto& actions = plugin_fake_actions::recorded;
    return call.status == MC_OK && !call.overran && utf8_equal({actions.local, actions.local_bytes}, kDraft);
}
bool plugin_file(const wchar_t* base, const u8 (&sha)[32]) noexcept {
    wchar_t leaf[69], path[MAX_PATH];
    catalog_plugin_leaf(sha, leaf);
    wide_copy(path, wide_copy(path, wide_copy(path, 0, base), L"\\plugins\\"), leaf);
    return exists(path);
}

bool initialize(const wchar_t* base) noexcept {
    u8 point[65];
    g_key.point(point);
    const u8* keys[]{point};
    CatalogServiceOptions options{};
    options.base = {base, static_cast<u32>(wide_length(base, MAX_PATH))};
    options.repository = utf8_literal("maj113/MaiCord-plugins");
    options.keys = {keys, 1};
    options.transport = plugin_fake::transport(g->server);
    options.clock_unix = clock;
    options.host = {developer_mode, toast, validate_package, library_changed, package_removed};
    return application_catalog_initialize(options);
}

PluginBinding* only_binding() noexcept {
    PluginRuntime* r = application_catalog_runtime();
    return r && r->binding_count == 1 ? r->bindings[0] : nullptr;
}
// The approved hosts catalog-state.bin holds for one plugin, read straight from the file.
bool saved_hosts(const wchar_t* base, u64 owner, u32* hosts) noexcept {
    wchar_t path[MAX_PATH];
    wide_copy(path, wide_copy(path, 0, base), L"\\catalog-state.bin");
    auto* state = static_cast<CatalogState*>(VirtualAlloc(nullptr, sizeof(CatalogState), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!state) return false;
    const CatalogStateEntry* entry = catalog_state_load(path, state).ok() ? catalog_state_find_owner(*state, owner) : nullptr;
    if (entry) *hosts = entry->host_bits;
    release(state);
    return entry != nullptr;
}

// Hosts the user switched on a plugin's settings page are what the plugin runs with after a restart, kept in
// catalog-state.bin: written by the next poll, and by release for a switch no poll saw.
void run_hosts(const wchar_t* base) noexcept {
    wchar_t directory[MAX_PATH], translate[MAX_PATH];
    if (!test_directory(directory)) {
        check(false, "hosts: the test directory");
        return;
    }
    wide_copy(translate, wide_copy(translate, 0, directory), L"\\translate.dll");
    g_developer = true;
    check(application_catalog_sideload({translate, static_cast<u32>(wide_length(translate, MAX_PATH))}) && settle(consent_or_finished) &&
              phase() == CatalogPhase::consent && application_catalog_consent()->request->hosts.count == 3,
          "hosts: a plugin with three hosts asks for consent");
    check(application_catalog_consent_answer(true, true) && only_binding() && only_binding()->approved_hosts == 7,
          "hosts: accepting approves every host");
    PluginBinding* binding = only_binding();
    if (!binding) return;
    const u64 owner = binding->owner_id;
    u32 saved = 0;
    check(saved_hosts(base, owner, &saved) && saved == 7, "hosts: the install is saved with all three");

    // A switch the poll saw is on disk while the plugin still runs.
    check(plugin_binding_set_host(binding, 1, false), "hosts: the second host is switched off");
    application_catalog_poll();
    check(saved_hosts(base, owner, &saved) && saved == 5, "hosts: the poll saves a switch");
    application_catalog_release();
    check(initialize(base), "hosts: the service starts again");
    application_catalog_restore();
    check(only_binding() && only_binding()->owner_id == owner && only_binding()->approved_hosts == 5,
          "hosts: after a restart the plugin has the hosts the user left on");

    // A switch made just before exit, with no poll in between, is saved by release.
    binding = only_binding();
    if (!binding) return;
    check(plugin_binding_set_host(binding, 1, true) && plugin_binding_set_host(binding, 2, false), "hosts: two more switches");
    application_catalog_release();
    check(saved_hosts(base, owner, &saved) && saved == 3, "hosts: release saves a switch no poll saw");
    check(initialize(base), "hosts: the service starts a third time");
    application_catalog_restore();
    check(only_binding() && only_binding()->approved_hosts == 3, "hosts: and the plugin runs with them");
    g_developer = false;
}

void run_flow(const wchar_t* base) noexcept {
    check(initialize(base), "the service starts");
    application_catalog_restore();
    check(!application_catalog_runtime() && !application_catalog_notification() && g->server.requests == 0,
          "nothing installed: no runtime, no worker and no traffic");

    // Browse fetches and verifies the signed index.
    publish({100});
    application_catalog_browse();
    check(settle([] { return application_catalog_view().status == CatalogStatus::ok; }) && application_catalog_view().index &&
              application_catalog_view().index->item_count == 3 && application_catalog_view().installable,
          "browsing fetches the signed index");

    // Install: download, verify, consent, enable.
    check(application_catalog_install(CatalogKind::plugin, kSlug) && settle(consent_or_finished) && phase() == CatalogPhase::consent,
          "install asks for consent after the download verifies");
    const CatalogConsent* consent = application_catalog_consent();
    check(consent && consent->request->permissions == g->permissions && !consent->sideload && !consent->update &&
              utf8_equal(consent->request->name, utf8_literal("Clear URLs")),
          "the consent lists what the plugin needs");
    check(application_catalog_consent_answer(true, true) && phase() == CatalogPhase::done && running_version("1.0.0"),
          "accepting enables the plugin");
    check(strips(), "the installed plugin strips tracking parameters from a sent link");
    check(acts(), "the plugin's gesture shows a local message without event, timer or kept-message hooks");
    auto state = application_catalog_item_state(CatalogKind::plugin, kSlug);
    check(state.installed && state.enabled && state.running && state.auto_update && !state.can_roll_back &&
              state.update == kCatalogNone && state.installed->approved_permissions == g->permissions,
          "the install is recorded with its approval");

    // The store's model holds the index it lists: a refresh that replaces it keeps it readable until the holder
    // takes the next one or lets go (the page memory is released then, so a later read would fault).
    const CatalogIndex* held = application_catalog_hold_index();
    publish({101, false, true, false, nullptr, nullptr, kMcfxInterfaceMaximum + 1});
    check(check_now(), "a newer index is accepted");
    const CatalogIndex* current = application_catalog_view().index;
    check(held && current && held != current && held->sequence < current->sequence && held->item_count == 3 &&
              utf8_equal(catalog_text(*held, held->items[0].slug), utf8_literal("clear-urls")) &&
              application_catalog_hold_index() == current,
          "a held index stays readable after a refresh replaces it, until the holder takes the next one");
    application_catalog_release_index();
    // A theme whose download doesn't match the index is refused; the right bytes install. An effect whose package
    // needs a newer .mcfx interface is listed but can't be installed.
    check(application_catalog_install(CatalogKind::theme, utf8_literal("fjord")) && settle(finished) && phase() == CatalogPhase::failed &&
              application_catalog_view().operation.error == CatalogError::digest &&
              !application_catalog_item_state(CatalogKind::theme, utf8_literal("fjord")).installed,
          "a download that doesn't match its digest installs nothing");
    check(application_catalog_item_state(CatalogKind::effect, utf8_literal("rain")).incompatible &&
              !application_catalog_install(CatalogKind::effect, utf8_literal("rain")) &&
              phase() == CatalogPhase::failed && application_catalog_view().operation.error == CatalogError::incompatible &&
              !application_catalog_item_state(CatalogKind::effect, utf8_literal("rain")).installed,
          "an effect needing a newer interface is listed as needing a newer MaiCord and isn't downloaded");
    publish({102, false, false, false, nullptr, nullptr, kMcfxInterfaceMaximum, kClientThemeInterfaceMaximum});
    check(check_now(), "the fixed index is accepted");
    const u32 changes = g_library_changes;
    check(application_catalog_install(CatalogKind::theme, utf8_literal("fjord")) && settle(finished) && phase() == CatalogPhase::done &&
              application_catalog_install(CatalogKind::effect, utf8_literal("rain")) && settle(finished) && phase() == CatalogPhase::done,
          "a theme and an effect install");
    wchar_t path[MAX_PATH];
    wide_copy(path, wide_copy(path, 0, base), L"\\themes\\fjord.maicord-theme.json");
    check(exists(path) && g_library_changes == changes + 2 && application_catalog_item_state(CatalogKind::effect, utf8_literal("rain")).installed,
          "their files are written and the library told");

    // Details are fetched once, verified and cached.
    const CatalogItem& listed = application_catalog_view().index->items[0];
    CatalogAsset asset{};
    check(application_catalog_asset_request(CatalogAssetKind::details, listed.details) &&
              settle([&] { return application_catalog_asset_take(listed.details.sha256, &asset); }) &&
              asset.count == g->details.bytes && bytes_equal(asset.bytes, g->details.data, asset.count),
          "a details page arrives verified");
    application_catalog_asset_release(&asset);

    // 1.1.0 needs nothing new: it updates on its own and keeps 1.0.0 for rollback.
    publish({103, true});
    check(check_now() && settle([] { return running_version("1.1.0") && finished(); }) && strips(),
          "an update within the approval applies automatically");
    state = application_catalog_item_state(CatalogKind::plugin, kSlug);
    check(state.can_roll_back && state.update == kCatalogNone && plugin_file(base, g->plugin[0].sha256) &&
              plugin_file(base, g->plugin[1].sha256),
          "the previous build is kept for rollback");

    // Startup: the next process enables the approved plugin from catalog-state.bin without any traffic.
    application_catalog_release();
    check(!plugin_hook_subscribed(PluginHook::pre_send), "release disables the plugin");
    const u32 requests = g->server.requests;
    check(initialize(base), "the service starts again");
    application_catalog_restore();
    check(running_version("1.1.0") && strips() && g->server.requests == requests, "startup enables the approved plugin");
    check(settle([] { return application_catalog_view().index != nullptr; }) && application_catalog_view().installable,
          "the cached index is read back");

    // A plugin file whose bytes changed on disk is not loaded.
    application_catalog_release();
    wchar_t leaf[69], dll[MAX_PATH];
    catalog_plugin_leaf(g->plugin[1].sha256, leaf);
    wide_copy(dll, wide_copy(dll, wide_copy(dll, 0, base), L"\\plugins\\"), leaf);
    check(write_file(dll, g->plugin[0].data, g->plugin[0].bytes) && initialize(base), "the file is replaced");
    application_catalog_restore();
    check(!running() && !plugin_hook_subscribed(PluginHook::pre_send), "a plugin file that changed on disk is refused");
    application_catalog_release();
    check(write_file(dll, g->plugin[1].data, g->plugin[1].bytes) && initialize(base), "the file is put back");
    application_catalog_restore();
    check(running_version("1.1.0"), "the right bytes load again");
    check(settle([] { return application_catalog_view().index != nullptr; }), "the cache is read again");

    // Rollback, then a replayed and an unsigned index change nothing.
    check(application_catalog_rollback(CatalogKind::plugin, kSlug) && phase() == CatalogPhase::done && running_version("1.0.0") && strips(),
          "rollback runs the previous build");
    publish({50, true});
    check(check_now() && application_catalog_view().status == CatalogStatus::invalid &&
              application_catalog_view().error == CatalogError::replay && application_catalog_view().index->sequence == 103,
          "an older index is refused as a replay");
    publish({104, true, false, true});
    check(check_now() && application_catalog_view().error == CatalogError::signature && application_catalog_view().index->sequence == 103,
          "an index with a bad signature is refused");

    // Revocation: the running build is malicious, the effect broken.
    const u32 toasts = g_toasts;
    publish({105, true, false, false, g->plugin[0].sha256, g->effect.sha256});
    check(check_now() && !running() && !plugin_hook_subscribed(PluginHook::pre_send) && g_deactivated == 1 && g_toasts >= toasts + 2,
          "revocation turns the plugin and the effect off at once");
    state = application_catalog_item_state(CatalogKind::plugin, kSlug);
    check(state.revoked == CatalogRevocationSeverity::malicious && (state.installed->flags & kCatalogEntryRevoked) && !state.enabled,
          "the plugin is marked revoked");
    check(!application_catalog_set_enabled(CatalogKind::plugin, kSlug, true) &&
              application_catalog_view().operation.error == CatalogError::revoked && !running(),
          "a revoked plugin can't be turned back on");
    check(state.update != kCatalogNone && !state.needs_review, "an update past the revoked build is offered");

    // Remove everything.
    check(application_catalog_remove(CatalogKind::plugin, kSlug, true) && !application_catalog_item_state(CatalogKind::plugin, kSlug).installed &&
              !plugin_file(base, g->plugin[0].sha256) && !plugin_file(base, g->plugin[1].sha256),
          "remove deletes the plugin's files");
    check(application_catalog_remove(CatalogKind::theme, utf8_literal("fjord"), false) && g_removed == 1 &&
              !application_catalog_item_state(CatalogKind::theme, utf8_literal("fjord")).installed,
          "remove hands themes to the package library");

    // A theme needing a newer document interface is listed but can't be installed (one with variants installs above).
    publish({106, true, false, false, g->plugin[0].sha256, g->effect.sha256, 0, kClientThemeInterfaceMaximum + 1});
    check(check_now() && application_catalog_item_state(CatalogKind::theme, utf8_literal("fjord")).incompatible &&
              !application_catalog_install(CatalogKind::theme, utf8_literal("fjord")) &&
              application_catalog_view().operation.error == CatalogError::incompatible,
          "a theme needing a newer interface is listed as needing a newer MaiCord and isn't downloaded");

    // A declined consent leaves nothing behind.
    check(application_catalog_install(CatalogKind::plugin, kSlug) && settle(consent_or_finished) && phase() == CatalogPhase::consent &&
              !application_catalog_consent_answer(false, false) && application_catalog_view().operation.error == CatalogError::declined &&
              !application_catalog_item_state(CatalogKind::plugin, kSlug).installed && !plugin_file(base, g->plugin[1].sha256),
          "declining installs nothing");

    // A refresh while the consent dialog is open: allowing installs only the build the dialog showed, listed as it
    // was then. One the refresh removed or listed differently installs nothing and the dialog asks again about what
    // the catalog offers now; one it still lists unchanged installs, recorded as the version shown.
    const auto shown = [](const char* version) noexcept {
        const CatalogConsent* c = application_catalog_consent();
        return phase() == CatalogPhase::consent && c && utf8_equal(c->request->version, {version, c_string_length(version)});
    };
    const auto installed_as = [](const char* version) noexcept {
        const auto s = application_catalog_item_state(CatalogKind::plugin, kSlug);
        return s.installed && utf8_equal(catalog_entry_version(*s.installed), {version, c_string_length(version)});
    };
    check(application_catalog_install(CatalogKind::plugin, kSlug) && settle(consent_or_finished) && shown("1.1.0"),
          "the dialog shows the newest build");
    publish({107});
    check(check_now() && shown("1.1.0"), "a refresh lands while the dialog is open");
    const u32 refused_toasts = g_toasts;
    check(!application_catalog_consent_answer(true, true) && g_toasts == refused_toasts + 1 &&
              utf8_equal(g_last_toast, application_catalog_error_text(CatalogError::review_again)) &&
              !application_catalog_item_state(CatalogKind::plugin, kSlug).installed && !plugin_file(base, g->plugin[1].sha256),
          "allowing a build a refresh took out of the catalog installs nothing and says to review it again");
    check(settle(consent_or_finished) && shown("1.0.0"), "the dialog asks again about the version the catalog offers now");
    const u64 permissions = g->permissions;
    g->permissions |= permissions & MC_PERM_CLIPBOARD ? MC_PERM_ACT_PRESENCE : MC_PERM_CLIPBOARD;
    publish({108});
    g->permissions = permissions;
    check(check_now() && shown("1.0.0") && !application_catalog_consent_answer(true, true) &&
              !application_catalog_item_state(CatalogKind::plugin, kSlug).installed && settle(finished) &&
              application_catalog_view().operation.error == CatalogError::plugin_mismatch && !running(),
          "allowing a build whose listing a refresh changed installs nothing (the new listing doesn't match the build)");
    publish({109});
    check(check_now() && application_catalog_install(CatalogKind::plugin, kSlug) && settle(consent_or_finished) && shown("1.0.0"),
          "the dialog shows the build again");
    publish({110, true});
    check(check_now() && shown("1.0.0") && application_catalog_consent_answer(true, true) && running_version("1.0.0") &&
              installed_as("1.0.0"),
          "allowing a build a refresh still lists unchanged installs it, recorded as the version shown");
    check(settle([] { return running_version("1.1.0") && finished(); }) && installed_as("1.1.0") &&
              application_catalog_remove(CatalogKind::plugin, kSlug, true) &&
              !application_catalog_item_state(CatalogKind::plugin, kSlug).installed,
          "the newer listed build then updates it within the approval, and it is removed");

    // Sideloads need Developer Mode.
    check(!application_catalog_sideload({L"C:\\x.dll", 8}) &&
              application_catalog_view().operation.error == CatalogError::developer_mode,
          "sideloading needs Developer Mode");

    // Link installs are recorded as unreviewed and never take a catalog slug.
    u8 sha[32]{1};
    check(application_catalog_record_unreviewed(CatalogKind::theme, utf8_literal("dusk-1a2b3c4d"), sha, {}) &&
              application_catalog_item_state(CatalogKind::theme, utf8_literal("dusk-1a2b3c4d")).unreviewed &&
              !application_catalog_record_unreviewed(CatalogKind::effect, utf8_literal("rain"), sha, utf8_literal("1.0.0")) &&
              application_catalog_forget(CatalogKind::theme, utf8_literal("dusk-1a2b3c4d")),
          "unreviewed items are recorded apart from catalog items");
    run_hosts(base);
    application_catalog_release();
}

void run() noexcept {
    g = static_cast<Catalog*>(VirtualAlloc(nullptr, sizeof(Catalog), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    wchar_t directory[MAX_PATH], base[MAX_PATH];
    const DWORD temp = GetTempPathW(MAX_PATH - 40, base);
    check(g && g_key.open() && test_directory(directory) && temp && setup(directory), "setup");
    if (!g || !g_key.ready || !temp) return;
    u32 at = wide_copy(base, temp, L"maicord-catalog-service-");
    u32 pid = GetCurrentProcessId();
    do base[at++] = static_cast<wchar_t>(L'0' + pid % 10), pid /= 10;
    while (pid);
    base[at] = 0;
    remove_tree(base);
    CreateDirectoryW(base, nullptr);
    if (!failures) run_flow(base);
    remove_tree(base);
    g_key.close();
}

} // namespace maicord::catalog_service_tests

extern "C" __declspec(noreturn) void WINAPI maicord_catalog_service_tests_entry() noexcept {
    maicord::catalog_service_tests::run();
    ExitProcess(maicord::catalog_service_tests::failures ? 1 : 0);
}
