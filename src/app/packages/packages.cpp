#include "packages.hpp"
#include "../account/application_account_logout.hpp"
#include "../chat/composer/attachments.hpp"
#include "../notifications/desktop.hpp"
#include "../plugins/catalog_service.hpp"
#include "../settings/custom_theme.hpp"
#include "../shell/state.hpp"
#include "maicord/app/net/file_persistence.hpp"
#include "maicord/app/packages/install.hpp"
#include "maicord/app/update/https.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/sys/shell.hpp"
#include "maicord/ui/theme/background_color_math.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {

constexpr u64 kLinkWaitMs = 120'000;
constexpr u32 kProtocolAsked = 1;
constexpr u32 kPathUnits = 1024;

// One candidate: the file, its parse and its theme document. Allocated while a session is open.
struct Candidate {
    McfxWorkspace workspace;
    ClientThemeDocument theme;
    u8 file[kMcfxMaximumBytes];
};

struct ThemeScratch {
    ClientThemeDocument defaults;
    u32 structurals[kClientThemeImportMaximumBytes];
};

struct Session {
    PackagePhase phase;
    PackageError error;
    u64 revision;
    PackageCard card;
    PackageLink link;
    Candidate* candidate;
    u32 size;
    bool effect_trial, theme_offered, theme_previewing, open_when_done;
    // The fetch worker.
    HANDLE thread;
    UpdateHttpsTransport* volatile https;
    volatile LONG finished, cancelled;
    UpdateBufferSink sink;
    UpdateFetchResult fetched;
};

struct Pending {
    bool active;
    PackageLink link;
    u64 queued_ms;
};

struct State {
    Session session;
    Pending pending;
    PackageDeepLink deep;
    bool deep_ready;
    volatile LONG changed;
    HANDLE notify;
};
State g;

void bump() noexcept {
    ++g.session.revision;
    application_request_frame();
}

void set_failed(PackageError error) noexcept {
    g.session.phase = PackagePhase::failed;
    g.session.error = error;
    const bool open = g.session.open_when_done;
    g.session.open_when_done = false;
    bump();
    if (open)
        application_store_open(PackageLinkKind::effect);
}

bool text_is(const char* a, const char* b) noexcept {
    const u32 n = c_string_length(a);
    return n == c_string_length(b) && bytes_equal(a, b, n);
}

// ---- Files -----------------------------------------------------------------------------------------------
// %LOCALAPPDATA%\MaiCord, created.
bool data_root(wchar_t (&root)[kPackageRootUnits]) noexcept {
    wchar_t probe[512];
    return data_path(probe, 504, L"effects", true).ok() && data_path(root, kPackageRootUnits, L"").ok();
}

// ---- Analysis -------------------------------------------------------------------------------------------
bool parse_theme(const u8* bytes, u32 size, ClientThemeDocument* output) noexcept {
    ScratchPages<ThemeScratch> scratch;
    if (!scratch.data || !size || size > kClientThemeImportMaximumBytes)
        return false;
    scratch.data->defaults = client_theme_document_defaults();
    return client_theme_document_parse({reinterpret_cast<const char*>(bytes), size}, scratch.data->defaults,
                                       {scratch.data->structurals, kClientThemeImportMaximumBytes}, output)
        .ok();
}

// "Night Owl" -> "night-owl-1a2b3c4d": the file name of an unreviewed theme.
void theme_slug(const ClientThemeDocument& document, const u8* bytes, u32 size, char (&slug)[kPackageSlugMaximumBytes + 1]) noexcept {
    u32 n = 0;
    for (u32 i = 0; i < document.name_bytes && n < 24; ++i) {
        char c = document.name[i];
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            slug[n++] = c;
        else if (n && slug[n - 1] != '-')
            slug[n++] = '-';
    }
    while (n && slug[n - 1] == '-')
        --n;
    if (!n)
        slug[n++] = 't';
    slug[n++] = '-';
    const u32 hash = static_cast<u32>(hash64_bytes(kHash64Seed, bytes, size));
    for (u32 i = 0; i < 8; ++i)
        slug[n++] = ascii_hex_digit(hash >> (28 - i * 4));
    slug[n] = 0;
}

enum class Sniff : u8 { unknown, effect, theme, program };
Sniff sniff(const u8* bytes, u32 size) noexcept {
    if (size >= 4 && bytes_equal(bytes, "MCFX", 4))
        return Sniff::effect;
    if (size >= 2 && bytes[0] == 'M' && bytes[1] == 'Z')
        return Sniff::program;
    u32 at = size >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf ? 3 : 0;
    while (at < size && (bytes[at] == ' ' || bytes[at] == '\t' || bytes[at] == '\r' || bytes[at] == '\n'))
        ++at;
    return at < size && bytes[at] == '{' ? Sniff::theme : Sniff::unknown;
}

PackageError analyze_kind(Sniff kind, const u8* bytes, u32 size, Candidate* candidate, PackageInfo* info) noexcept {
    clear_bytes(info, sizeof(*info));
    info->bytes = size;
    if (kind == Sniff::effect) {
        if (size > kMcfxMaximumBytes)
            return PackageError::too_large;
        const auto parsed = mcfx_parse({bytes, size}, &candidate->workspace);
        if (!parsed.ok())
            return parsed.error == McfxError::oversize ? PackageError::too_large
                   : parsed.error == McfxError::bytecode ? PackageError::refused_shader
                                                          : PackageError::invalid;
        info->kind = PackageLinkKind::effect;
        copy_bytes(info->slug, candidate->workspace.package.slug, sizeof(info->slug));
        copy_bytes(info->name, candidate->workspace.package.name, kMcfxNameBytes + 1);
        return PackageError::none;
    }
    if (kind == Sniff::theme) {
        if (size > kClientThemeImportMaximumBytes)
            return PackageError::too_large;
        if (!parse_theme(bytes, size, &candidate->theme))
            return PackageError::invalid;
        info->kind = PackageLinkKind::theme;
        copy_bytes(info->name, candidate->theme.name, candidate->theme.name_bytes + 1);
        theme_slug(candidate->theme, bytes, size, info->slug);
        return PackageError::none;
    }
    return kind == Sniff::program ? PackageError::unsupported : PackageError::invalid;
}

bool effect_installed(const char* slug) noexcept {
    for (u32 i = 0, count = application_effects_count(); i < count; ++i) {
        const EffectInfo* info = application_effects_info(i);
        if (info && text_is(info->slug, slug))
            return true;
    }
    return false;
}

void fill_card(const PackageInfo& info) noexcept {
    auto& s = g.session;
    auto& card = s.card;
    clear_bytes(&card, sizeof(card));
    card.kind = info.kind;
    card.bytes = info.bytes;
    copy_bytes(card.slug, info.slug, sizeof(card.slug));
    copy_bytes(card.name, info.name, sizeof(card.name));
    // The host of a link (a file's session has no link: empty).
    const char* url = s.link.url + 8;
    for (u32 n = 0; url[n] && url[n] != '/' && url[n] != '?' && n < sizeof(card.host) - 1; ++n)
        card.host[n] = url[n];
    const auto state = application_catalog_item_state(static_cast<CatalogKind>(info.kind),
                                                      {info.slug, c_string_length(info.slug)});
    card.replaces = state.installed && state.unreviewed;
    auto* candidate = s.candidate;
    if (info.kind == PackageLinkKind::effect) {
        card.effect = &candidate->workspace.package;
        return;
    }
    const auto& theme = candidate->theme;
    card.swatches[0] = background_color_pack(theme.theme.base_low);
    card.swatches[1] = background_color_pack(theme.theme.surface);
    card.swatches[2] = background_color_pack(theme.theme.text_primary);
    card.swatches[3] = background_color_pack(theme.theme.accent);
    copy_bytes(card.effect_slug, theme.effect.slug, sizeof(card.effect_slug));
    card.effect_installed = theme.effect.slug[0] && effect_installed(theme.effect.slug);
}

// A loaded candidate becomes a card, an effect starts its trial.
void finish_load(Sniff kind) noexcept {
    auto& s = g.session;
    PackageInfo info{};
    const PackageError error = analyze_kind(kind, s.candidate->file, s.size, s.candidate, &info);
    if (error != PackageError::none) {
        set_failed(error);
        return;
    }
    fill_card(info);
    if (info.kind == PackageLinkKind::effect) {
        if (!application_effects_trial_begin({s.candidate->file, s.size}, s.candidate->workspace.package)) {
            set_failed(PackageError::busy);
            return;
        }
        s.effect_trial = true;
    }
    s.phase = PackagePhase::ready;
    s.error = PackageError::none;
    if (s.open_when_done)
        application_store_open(info.kind);
    s.open_when_done = false;
    bump();
}

// ---- Session -------------------------------------------------------------------------------------------
void join_worker() noexcept {
    auto& s = g.session;
    if (!s.thread)
        return;
    InterlockedExchange(&s.cancelled, 1);
    if (auto* https = s.https)
        update_https_cancel(https);
    if (WaitForSingleObject(s.thread, 10'000) != WAIT_OBJECT_0) {
        // The worker still owns the buffer: leave it to the process exit.
        s.candidate = nullptr;
        log_write(LogLevel::warning, "packages", "fetch_join_timeout");
    }
    CloseHandle(s.thread);
    s.thread = nullptr;
}

void end_session() noexcept {
    auto& s = g.session;
    join_worker();
    if (s.effect_trial)
        application_effects_trial_end();
    if (s.theme_offered || s.theme_previewing)
        application_custom_theme_action(ApplicationCustomThemeAction::cancel);
    if (s.candidate)
        pages_release(s.candidate);
    const u64 revision = s.revision;
    reset_value(&s);
    s.revision = revision + 1;
    application_request_frame();
}

bool start_session() noexcept {
    auto& s = g.session;
    if (s.phase == PackagePhase::working)
        return false;
    if (s.phase != PackagePhase::idle)
        end_session();
    s.candidate = commit_pages<Candidate>();
    if (!s.candidate) {
        set_failed(PackageError::storage);
        return false;
    }
    return true;
}

DWORD WINAPI fetch_main(void*) noexcept {
    auto& s = g.session;
    auto* https = commit_pages<UpdateHttpsTransport>();
    if (https && update_https_initialize(https)) {
        s.https = https;
        UpdateLocation location{};
        if (InterlockedCompareExchange(&s.cancelled, 0, 0)) {
            update_https_cancel(https);
        } else if (package_link_location(s.link, &location)) {
            const UpdateTransport transport = update_https_transport(https);
            s.fetched = update_fetch(transport, location, kMcfxMaximumBytes, 0, update_buffer_sink, &s.sink,
                                     clock_unix_milliseconds() / 1000);
        }
        s.https = nullptr;
        update_https_release(https);
    }
    if (https)
        pages_release(https);
    InterlockedExchange(&s.finished, 1);
    SetEvent(g.notify);
    return 0;
}

PackageError map_fetch(const UpdateFetchResult& result) noexcept {
    switch (result.error) {
    case UpdateFetchError::redirect: return PackageError::redirect;
    case UpdateFetchError::oversize:
    case UpdateFetchError::size: return PackageError::too_large;
    default: return PackageError::unreachable;
    }
}

void finish_fetch() noexcept {
    auto& s = g.session;
    if (s.thread) {
        WaitForSingleObject(s.thread, 5'000);
        CloseHandle(s.thread);
        s.thread = nullptr;
    }
    s.finished = 0;
    if (!s.fetched.ok() || !s.sink.used) {
        set_failed(s.fetched.ok() ? PackageError::invalid : map_fetch(s.fetched));
        return;
    }
    s.size = s.sink.used;
    finish_load(sniff(s.candidate->file, s.size));
}

bool begin_url(const PackageLink& link, bool open_when_done) noexcept {
    UpdateLocation location{};
    if (link.type != PackageLinkType::url || !package_link_location(link, &location)) {
        log_write(LogLevel::info, "packages", "link_refused");
        return false;
    }
    if (!start_session())
        return false;
    auto& s = g.session;
    if (!g.notify)
        g.notify = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.link = link;
    s.sink = {s.candidate->file, kMcfxMaximumBytes, 0};
    s.fetched = {};
    s.finished = 0;
    s.cancelled = 0;
    s.open_when_done = open_when_done;
    s.phase = PackagePhase::working;
    s.card = {};
    s.thread = g.notify ? CreateThread(nullptr, 256 * 1024, fetch_main, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)
                        : nullptr;
    if (!s.thread) {
        set_failed(PackageError::unreachable);
        return false;
    }
    bump();
    return true;
}

// A catalog link: the store page opens on that item (taken once through application_packages_take_deep_link).
void open_catalog_item(const PackageLink& link) noexcept {
    g.deep = {link.kind, {}};
    copy_bytes(g.deep.slug, link.slug, link.slug_bytes + 1);
    g.deep_ready = true;
    application_store_open(link.kind);
}

} // namespace

// ---- Validation ----------------------------------------------------------------------------------------------
PackageError package_validate(PackageLinkKind kind, ConstSlice<u8> bytes, PackageInfo* info) noexcept {
    if (!info || !bytes.data || !bytes.count || (kind != PackageLinkKind::theme && kind != PackageLinkKind::effect))
        return PackageError::invalid;
    if (sniff(bytes.data, bytes.count) != (kind == PackageLinkKind::effect ? Sniff::effect : Sniff::theme))
        return PackageError::invalid;
    ScratchPages<Candidate> candidate;
    if (!candidate.data)
        return PackageError::storage;
    return analyze_kind(kind == PackageLinkKind::effect ? Sniff::effect : Sniff::theme, bytes.data, bytes.count,
                        candidate.data, info);
}

void application_packages_library_changed() noexcept {
    application_effects_library_changed();
    // The theme list scans on the UI thread's next poll; an event keeps this callable from any thread.
    InterlockedExchange(&g.changed, 1);
    if (g.notify)
        SetEvent(g.notify);
}

// ---- Sessions: link, file, drop -------------------------------------------------------------------------------
bool application_packages_begin_link(Utf8View text) noexcept {
    PackageLink link{};
    if (!package_link_parse(text, &link)) {
        log_write(LogLevel::info, "packages", "link_refused");
        return false;
    }
    if (link.type == PackageLinkType::catalog) {
        open_catalog_item(link);
        return true;
    }
    return begin_url(link, false);
}

bool application_packages_begin_file(ConstSlice<wchar_t> path) noexcept {
    if (!path.data || !path.count || path.count >= kPathUnits)
        return false;
    wchar_t text[kPathUnits];
    copy_bytes(text, path.data, path.count * sizeof(wchar_t));
    text[path.count] = 0;
    if (!start_session())
        return false;
    auto& s = g.session;
    s.link = {};
    const auto read = file_persistence_read_file(text, {s.candidate->file, kMcfxMaximumBytes});
    if (!read.ok()) {
        set_failed(read.error == FileReadError::size ? PackageError::too_large : PackageError::bad_file);
        return false;
    }
    s.size = read.bytes;
    s.phase = PackagePhase::working;
    finish_load(sniff(s.candidate->file, s.size));
    return s.phase == PackagePhase::ready;
}

PackageStatus application_packages_status() noexcept {
    const auto& s = g.session;
    return {s.phase, s.error, s.revision};
}

const PackageCard* application_packages_card() noexcept {
    return g.session.phase == PackagePhase::ready ? &g.session.card : nullptr;
}

bool application_packages_preview_start() noexcept {
    auto& s = g.session;
    if (s.phase != PackagePhase::ready)
        return false;
    if (s.card.kind == PackageLinkKind::effect)
        return s.effect_trial || (application_effects_trial_begin({s.candidate->file, s.size}, s.candidate->workspace.package) &&
                                  (s.effect_trial = true));
    if (s.theme_previewing)
        return true;
    if (!application_custom_theme_offer(s.candidate->theme))
        return false;
    s.theme_offered = true;
    s.theme_previewing = application_custom_theme_action(ApplicationCustomThemeAction::preview);
    bump();
    return s.theme_previewing;
}

bool application_packages_previewing() noexcept {
    return g.session.phase == PackagePhase::ready && (g.session.effect_trial || g.session.theme_previewing);
}

EffectCost application_packages_preview_cost() noexcept {
    return g.session.effect_trial ? application_effects_cost(application_effects_trial_identity()) : EffectCost{};
}

PackageError application_packages_commit() noexcept {
    auto& s = g.session;
    if (s.phase != PackagePhase::ready)
        return PackageError::busy;
    const bool effect = s.card.kind == PackageLinkKind::effect;
    if (effect && s.effect_trial) {
        const auto trial = application_effects_trial_state();
        if (trial == EffectTrialState::rejected)
            return PackageError::refused_shader;
        if (trial == EffectTrialState::refused)
            return PackageError::too_heavy;
    }
    // A reviewed catalog item owns its slug; an unreviewed file never replaces it.
    const auto state = application_catalog_item_state(static_cast<CatalogKind>(s.card.kind), {s.card.slug, c_string_length(s.card.slug)});
    if ((state.installed && !state.unreviewed) || state.item != kCatalogNone)
        return PackageError::conflict;
    if (effect && !s.card.replaces && application_effects_count() >= kEffectLibraryCapacity)
        return PackageError::capacity;
    wchar_t root[kPackageRootUnits], path[512];
    u8 hash[32];
    u32 units = 0;
    const Utf8View slug{s.card.slug, c_string_length(s.card.slug)};
    if (!data_root(root) || !package_sha256({s.candidate->file, s.size}, hash) ||
        !package_file_path(root, s.card.kind, slug, hash, {path, 512}, &units))
        return PackageError::storage;
    const bool existed = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    if (!package_install(root, s.card.kind, slug, {s.candidate->file, s.size}, hash))
        return PackageError::storage;
    const char* version = effect ? s.candidate->workspace.package.version : "";
    if (!application_catalog_record_unreviewed(static_cast<CatalogKind>(s.card.kind), slug, hash,
                                               {version, c_string_length(version)})) {
        if (!existed)
            DeleteFileW(path);
        return PackageError::storage;
    }
    if (effect)
        package_prune(root, slug, hash);
    end_session();
    application_packages_library_changed();
    return PackageError::none;
}

void application_packages_cancel() noexcept {
    if (g.session.phase != PackagePhase::idle)
        end_session();
}

// ---- maicord:// -------------------------------------------------------------------------------------------
bool application_packages_take_deep_link(PackageDeepLink* link) noexcept {
    if (!g.deep_ready || !link)
        return false;
    *link = g.deep;
    g.deep_ready = false;
    return true;
}

bool application_packages_open_link(ConstSlice<wchar_t> text) noexcept {
    PackageLink link{};
    if (!package_link_parse_wide(text, &link) || link.type == PackageLinkType::none) {
        log_write(LogLevel::info, "packages", "link_refused");
        return false;
    }
    g.pending = {true, link, clock_monotonic_milliseconds()};
    application_request_frame();
    return true;
}

void application_packages_tick(u64 now_ms) noexcept {
    if (!g.pending.active)
        return;
    const bool ready = g_app.account_ui && g_app.account.instance && !g_app.login.visible && !application_account_logout_blocked();
    if (!ready) {
        if (now_ms - g.pending.queued_ms > kLinkWaitMs)
            g.pending.active = false;
        return;
    }
    const PackageLink link = g.pending.link;
    g.pending.active = false;
    if (link.type == PackageLinkType::catalog) {
        open_catalog_item(link);
        return;
    }
    if (!begin_url(link, true))
        log_write(LogLevel::info, "packages", "link_not_started");
}

HANDLE application_packages_notification() noexcept {
    if (!g.notify)
        g.notify = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g.notify;
}

void application_packages_poll() noexcept {
    if (InterlockedExchange(&g.changed, 0)) {
        application_custom_theme_rescan();
        application_request_frame();
    }
    auto& s = g.session;
    if (s.phase == PackagePhase::working && s.thread) {
        if (InterlockedCompareExchange(&s.finished, 0, 0))
            finish_fetch();
    } else if (s.phase == PackagePhase::ready && s.effect_trial) {
        // The renderer's verdict on the candidate's shader arrives a frame or two after the trial starts.
        const auto trial = application_effects_trial_state();
        if (trial == EffectTrialState::rejected || trial == EffectTrialState::refused) {
            s.phase = PackagePhase::failed;
            s.error = trial == EffectTrialState::rejected ? PackageError::refused_shader : PackageError::too_heavy;
            bump();
        }
    }
}

void application_packages_release() noexcept {
    application_packages_cancel();
    if (g.notify) {
        CloseHandle(g.notify);
        g.notify = nullptr;
    }
}

bool application_packages_protocol_asked() noexcept {
    return application_effects_state_flag(kProtocolAsked);
}

bool application_packages_protocol_answer(bool enable) noexcept {
    bool saved = true;
    if (enable) {
        wchar_t buffer[1024];
        const ConstSlice<wchar_t> path = application_desktop_registration_executable(buffer, 1024);
        saved = path.count != 0 && desktop_protocol_set(desktop_registry_maicord_scope(), path, true);
        if (!saved)
            log_write(LogLevel::warning, "packages", "protocol_registration_failed");
    }
    return application_effects_set_state_flag(kProtocolAsked, true) && saved;
}

// ---- Drops ---------------------------------------------------------------------------------------------
void application_packages_drop_enable(bool wanted) noexcept {
    if (g_install_drop_wanted == wanted)
        return;
    g_install_drop_wanted = wanted;
    application_sync_attachment_drop_target();
}

bool application_packages_drop(HANDLE drop) noexcept {
    if (!g_install_drop_wanted || !drop)
        return false;
    wchar_t path[kPathUnits];
    const UINT units = DragQueryFileW(static_cast<HDROP>(drop), 0, path, kPathUnits);
    DragFinish(static_cast<HDROP>(drop));
    if (units && units < kPathUnits)
        application_packages_begin_file({path, units});
    return true;
}

// ---- Removal -----------------------------------------------------------------------------------------------
namespace {
struct ThemeFile {
    ClientThemeDocument parsed;
    u8 bytes[kClientThemeImportMaximumBytes];
};
// The applied custom theme came from this file when their names agree.
bool theme_is_applied(const char* slug) noexcept {
    const ClientThemeDocument* active = application_custom_theme_document();
    if (!active)
        return false;
    wchar_t root[kPackageRootUnits], path[512];
    u32 units = 0;
    ScratchPages<ThemeFile> file;
    if (!file.data || !data_root(root) ||
        !package_file_path(root, PackageLinkKind::theme, {slug, c_string_length(slug)}, nullptr, {path, 512}, &units))
        return false;
    const auto read = file_persistence_read_file(path, {file.data->bytes, kClientThemeImportMaximumBytes});
    return read.ok() && parse_theme(file.data->bytes, read.bytes, &file.data->parsed) &&
           file.data->parsed.name_bytes == active->name_bytes &&
           bytes_equal(file.data->parsed.name, active->name, active->name_bytes);
}
} // namespace

void application_packages_deactivate(PackageLinkKind kind, const Utf8View& slug) noexcept {
    if (!package_slug_valid(slug))
        return;
    char text[kPackageSlugMaximumBytes + 1]{};
    copy_bytes(text, slug.data, slug.bytes);
    if (kind == PackageLinkKind::effect)
        application_effects_deactivate(slug.data, slug.bytes);
    else if (kind == PackageLinkKind::theme && theme_is_applied(text))
        application_custom_theme_use_builtin();
}

bool application_packages_remove(PackageLinkKind kind, Utf8View slug) noexcept {
    if (!package_slug_valid(slug) || kind == PackageLinkKind::plugin)
        return false;
    char text[kPackageSlugMaximumBytes + 1]{};
    copy_bytes(text, slug.data, slug.bytes);
    const auto state = application_catalog_item_state(static_cast<CatalogKind>(kind), slug);
    bool removed = false;
    if (kind == PackageLinkKind::effect) {
        removed = application_effects_remove(text, slug.bytes);
    } else {
        // The applied theme falls back to the built-in one.
        application_packages_deactivate(kind, slug);
        wchar_t root[kPackageRootUnits];
        removed = data_root(root) && package_uninstall(root, kind, slug);
        application_custom_theme_rescan();
    }
    if (removed && state.installed && state.unreviewed)
        application_catalog_forget(static_cast<CatalogKind>(kind), slug);
    return removed;
}

} // namespace maicord::app_detail
