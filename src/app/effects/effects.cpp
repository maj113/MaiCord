#include "effects.hpp"
#include "../settings/custom_theme.hpp"
#include "../settings/theme_publication.hpp"
#include "../settings/settings.hpp"
#include "../shell/background.hpp"
#include "../shell/state.hpp"
#include "../shell/toast.hpp"
#include "../shell/window_state.hpp"
#include "maicord/app/net/file_persistence.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/interlocked.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/media/image/png.hpp"
#include "maicord/ui/settings/scene.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_system_time.hpp"
#include "maicord/sys/registry.hpp"

namespace maicord::app_detail {
namespace {

constexpr u64 kIdleMilliseconds = 5ull * 60 * 1000;
constexpr u32 kOverrides = 16, kRefusals = 16, kPathUnits = 400;
constexpr wchar_t kStateFile[] = L"effects\\state.bin";
constexpr wchar_t kMarkerFile[] = L"effects\\effect-frame.bin";
// StateFile::flags: the global light-theme row is Off (bit 1 belongs to packages.cpp, the maicord:// answer).
constexpr u32 kLightOff = 2;

struct Entry {
    EffectInfo info;
    wchar_t path[kPathUnits];
    EffectBlend blend;
    u8 requested_resolution, requested_frame_rate;
    u64 modified;
};

// An effect the renderer turned off, or refused, on one GPU.
struct Refusal {
    u64 identity, adapter;
};

// The persisted state: the choice, the global rows, each package's own settings and the refusals.
struct StateFile {
    u8 magic[4];
    u32 version;
    u64 selected;
    EffectUserSettings user;
    u32 override_count, refusal_count, flags;
    EffectShaderSettings overrides[kOverrides];
    Refusal refusals[kRefusals];
};
static_assert(sizeof(StateFile) == 32 + 16 * kOverrides + 16 * kRefusals);
constexpr u8 kMagic[4]{'M', 'C', 'E', 'S'};

// A package read into memory: its file (the byte views of the package point into it), the parsed manifest, and
// the knob values in force.
struct Loaded {
    u8* file;
    u32 bytes;
    u64 identity, theme_hash;
    McfxPackage package;
    McfxKnobValue values[kMcfxKnobMaximum];
};

struct Trial {
    bool active;
    EffectTrialState state;
    u64 identity;
    const u8* file;
    u32 bytes;
    const McfxPackage* package;
};

// The library entry a knob page previews (see application_effects_preview).
struct Preview {
    u64 identity; // 0: none
    bool running;
    bool swapped; // it draws in place of the window's own effect
    EffectTrialState state;
};

// A simulated clock or appearance from the development folder (see read_simulation); used in Developer Mode only.
struct Simulation {
    bool time, date, appearance, dark;
    u16 minutes;     // the simulated time of day, in minutes from midnight
    u16 month, day;
    u32 day_seconds; // 0: the simulated time stands; else a whole day passes in this many seconds
    u64 anchor_ms;   // when the file was read
};

// mc_ambient's sources and easing (effect_ambient_sample), and mc_knobs as they glide (mcfx_knob_glide).
struct Inputs {
    EffectAmbient ambient;
    u64 identity;      // the package the easing belongs to
    u64 next_frame_ms; // the next eased frame while a value eases, else 0
    EffectLocalTime local;
    u64 clock_next_ms; // the local clock is read again at the next whole minute
    bool system_read, system_dark, knobs_moving;
    Simulation simulation;
    float knobs[kEffectKnobRows][4]; // as last sent to the renderer
    u64 knobs_ms;                    // when
};

struct State {
    StateFile file;
    bool state_loaded, library_loaded, marker_opened;
    Entry entries[kEffectLibraryCapacity];
    u32 count;
    u64 last_input_ms;
    EffectSettings settings; // as last sent to the renderer
    bool selected;
    Utf8View choices[kBackgroundEffectCount + kEffectLibraryCapacity];
    Loaded resident, opened;
    bool opened_is_resident, opened_valid;
    Trial trial;
    Preview preview;
    Inputs inputs;
    char off[4][kMcfxSlugBytes + 1]; // a theme's effect the user's store turned off this session
    u32 off_next;
    volatile LONG dirty;
};
State g;

// ---- Files -----------------------------------------------------------------------------------------------
void load_state() noexcept {
    if (g.state_loaded)
        return;
    g.state_loaded = true;
    g.file.user = kEffectUserDefaults;
    wchar_t path[512];
    if (!data_path(path, 504, kStateFile, false).ok())
        return;
    StateFile read{};
    const auto result = file_persistence_read_file(path, {reinterpret_cast<u8*>(&read), sizeof(read)}, sizeof(read));
    if (!result.ok() || result.bytes != sizeof(read) || !bytes_equal(read.magic, kMagic, 4) || read.version != 1 ||
        read.override_count > kOverrides || read.refusal_count > kRefusals || !effect_user_settings_valid(read.user))
        return;
    for (u32 i = 0; i < read.override_count; ++i)
        if (!effect_shader_settings_valid(read.overrides[i]) || !read.overrides[i].identity)
            return;
    g.file = read;
}

bool save_state() noexcept {
    wchar_t path[512];
    const DataPathResult built = data_path(path, 504, kStateFile, true);
    if (!built.ok())
        return false;
    copy_bytes(g.file.magic, kMagic, 4);
    g.file.version = 1;
    const auto written = file_persistence_write_atomic(path, built.length, {reinterpret_cast<const u8*>(&g.file), sizeof(g.file)});
    if (!written.ok())
        log_u64(LogLevel::warning, "effects", "state_save_error", written.native_error);
    return written.ok();
}

// Reads a whole file into `buffer`; the byte count, or 0.
u32 read_file(const wchar_t* path, const Slice<u8>& buffer) noexcept {
    const auto result = file_persistence_read_file(path, buffer);
    return result.ok() ? result.bytes : 0;
}

bool folder_path(wchar_t (&folder)[512], bool developer) noexcept {
    if (developer) {
        const u32 length = GetEnvironmentVariableW(L"MAICORD_EFFECT_DEV", folder, 480);
        if (length && length < 480)
            return true;
        return data_path(folder, 480, L"effects\\dev").ok();
    }
    return data_path(folder, 480, L"effects").ok();
}

// effects\<slug>.knobs: "MCKB", a count, then {hash of the knob id, number, rgb} per saved knob.
DataPathResult knobs_path(wchar_t (&path)[512], const char* slug, bool create) noexcept {
    wchar_t relative[96] = L"effects\\";
    u32 at = 8;
    for (u32 i = 0; slug[i] && at < 80; ++i)
        relative[at++] = static_cast<wchar_t>(slug[i]);
    for (const wchar_t c : L".knobs")
        relative[at++] = c;
    return data_path(path, 504, relative, create);
}

void delete_knob_file(const char* slug) noexcept {
    wchar_t path[512];
    if (knobs_path(path, slug, false).ok())
        DeleteFileW(path);
}

u32 id_hash(const char* id) noexcept { return hash32_bytes(reinterpret_cast<const u8*>(id), c_string_length(id)); }

struct KnobRecord {
    u32 id, number_bits, rgb;
};

void load_knob_file(Loaded& l) noexcept {
    wchar_t path[512];
    u8 bytes[12 + sizeof(KnobRecord) * kMcfxKnobMaximum];
    if (!knobs_path(path, l.package.slug, false).ok())
        return;
    const u32 got = read_file(path, {bytes, sizeof(bytes)});
    u32 count = 0;
    if (got < 12 || !bytes_equal(bytes, "MCKB", 4))
        return;
    copy_bytes(&count, bytes + 8, 4);
    if (count > kMcfxKnobMaximum || got != 12 + count * sizeof(KnobRecord))
        return;
    for (u32 i = 0; i < count; ++i) {
        KnobRecord record;
        copy_bytes(&record, bytes + 12 + i * sizeof(record), sizeof(record));
        for (u32 k = 0; k < l.package.knob_count; ++k) {
            if (id_hash(l.package.knobs[k].id) != record.id)
                continue;
            McfxKnobValue value{};
            copy_bytes(&value.number, &record.number_bits, 4);
            value.rgb = record.rgb;
            if (mcfx_knob_value_valid(l.package.knobs[k], value))
                l.values[k] = value;
        }
    }
}

bool save_knob_file(const Loaded& l) noexcept {
    wchar_t path[512];
    u8 bytes[12 + sizeof(KnobRecord) * kMcfxKnobMaximum];
    const DataPathResult built = knobs_path(path, l.package.slug, true);
    if (!built.ok())
        return false;
    copy_bytes(bytes, "MCKB", 4);
    const u32 version = 1, count = l.package.knob_count;
    copy_bytes(bytes + 4, &version, 4);
    copy_bytes(bytes + 8, &count, 4);
    for (u32 i = 0; i < count; ++i) {
        KnobRecord record{id_hash(l.package.knobs[i].id), 0, l.values[i].rgb};
        copy_bytes(&record.number_bits, &l.values[i].number, 4);
        copy_bytes(bytes + 12 + i * sizeof(record), &record, sizeof(record));
    }
    return file_persistence_write_atomic(path, built.length, {bytes, 12 + count * static_cast<u32>(sizeof(KnobRecord))}).ok();
}

// ---- Loaded packages ---------------------------------------------------------------------------------------
void release(Loaded& l) noexcept {
    if (l.file)
        VirtualFree(l.file, 0, MEM_RELEASE);
    reset_value(&l);
}

const ClientThemeEffect* theme_effect() noexcept {
    const ClientThemeDocument* document = application_custom_theme_document();
    return document && document->effect.slug[0] && document->theme.background.effect == BackgroundEffect::none
               ? &document->effect
               : nullptr;
}

u64 theme_hash(const ClientThemeEffect* effect) noexcept {
    return effect ? hash64_bytes(kHash64Seed, effect, sizeof(*effect)) | 1 : 0;
}

bool text_is(const char* a, const char* b) noexcept {
    const u32 n = c_string_length(a);
    return n == c_string_length(b) && bytes_equal(a, b, n);
}

// The theme's effect when it is the one named `slug`.
const ClientThemeEffect* theme_for(const char* slug) noexcept {
    const ClientThemeEffect* theme = theme_effect();
    return theme && text_is(theme->slug, slug) ? theme : nullptr;
}

// Defaults, then the theme's values, then what the user saved.
void init_values(Loaded& l, const ClientThemeEffect* theme) noexcept {
    mcfx_knob_defaults(l.package, l.values);
    if (theme)
        for (u32 i = 0; i < theme->knob_count; ++i)
            for (u32 k = 0; k < l.package.knob_count; ++k) {
                const auto& knob = l.package.knobs[k];
                if (!text_is(knob.id, theme->knobs[i].id) || theme->knobs[i].color != (knob.type == McfxKnobType::color))
                    continue;
                McfxKnobValue value{theme->knobs[i].number, theme->knobs[i].rgb};
                if (mcfx_knob_value_valid(knob, value))
                    l.values[k] = value;
            }
    load_knob_file(l);
    l.theme_hash = theme_hash(theme);
}

bool load(Loaded& l, const Entry& entry, const ClientThemeEffect* theme) noexcept {
    release(l);
    l.file = static_cast<u8*>(commit_pages(entry.info.bytes));
    if (!l.file)
        return false;
    l.bytes = entry.info.bytes;
    ScratchPages<McfxWorkspace> workspace;
    if (!workspace.data || read_file(entry.path, {l.file, l.bytes}) != l.bytes ||
        (hash64_bytes(kHash64Seed, l.file, l.bytes) | 1) != entry.info.identity ||
        !mcfx_parse({l.file, l.bytes}, workspace.data).ok()) {
        release(l);
        return false;
    }
    l.package = workspace.data->package;
    l.identity = entry.info.identity;
    init_values(l, theme);
    return true;
}

void close_opened() noexcept {
    if (g.opened_valid && !g.opened_is_resident)
        release(g.opened);
    g.opened_valid = g.opened_is_resident = false;
}

// ---- Text ------------------------------------------------------------------------------------------------
bool name_before(const Entry& a, const Entry& b) noexcept {
    const u32 an = c_string_length(a.info.name), bn = c_string_length(b.info.name), shared = an < bn ? an : bn;
    for (u32 i = 0; i < shared; ++i) {
        char x = a.info.name[i], y = b.info.name[i];
        x = x >= 'A' && x <= 'Z' ? static_cast<char>(x + 32) : x;
        y = y >= 'A' && y <= 'Z' ? static_cast<char>(y + 32) : y;
        if (x != y)
            return x < y;
    }
    return an != bn ? an < bn : a.info.identity < b.info.identity;
}

// "<slug>-<16 hex>.mcfx".
bool installed_name(const wchar_t* name, const char* slug) noexcept {
    const u32 length = wide_length(name, 200), slug_bytes = c_string_length(slug);
    if (length != slug_bytes + 1 + 16 + 5)
        return false;
    for (u32 i = 0; i < slug_bytes; ++i)
        if (name[i] != static_cast<wchar_t>(slug[i]))
            return false;
    if (name[slug_bytes] != L'-' || wide_length(name + slug_bytes + 17, 8) != 5 ||
        !bytes_equal(name + slug_bytes + 17, L".mcfx", 6 * sizeof(wchar_t)))
        return false;
    for (u32 i = 0; i < 16; ++i) {
        const wchar_t c = name[slug_bytes + 1 + i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')))
            return false;
    }
    return true;
}

void scan_folder(bool developer, u8* buffer, McfxWorkspace* workspace) noexcept {
    wchar_t folder[512];
    if (!folder_path(folder, developer))
        return;
    wchar_t pattern[512];
    u32 pattern_length = 0;
    pattern[0] = 0;
    if (!wide_append(pattern, 512, &pattern_length, folder) || !wide_append(pattern, 512, &pattern_length, L"\\*.mcfx"))
        return;
    WIN32_FIND_DATAW found;
    const HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE)
        return;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        Entry entry{};
        u32 path_length = 0;
        if (!wide_append(entry.path, kPathUnits, &path_length, folder) ||
            !wide_append(entry.path, kPathUnits, &path_length, L"\\") ||
            !wide_append(entry.path, kPathUnits, &path_length, found.cFileName))
            continue;
        const u32 bytes = read_file(entry.path, {buffer, kMcfxMaximumBytes});
        if (!bytes || !mcfx_parse({buffer, bytes}, workspace).ok()) {
            log_write(LogLevel::warning, "effects", "package_rejected");
            continue;
        }
        const McfxPackage& package = workspace->package;
        auto& info = entry.info;
        if (!developer && !installed_name(found.cFileName, package.slug)) {
            log_write(LogLevel::warning, "effects", "package_name_mismatch");
            continue;
        }
        info.identity = hash64_bytes(kHash64Seed, buffer, bytes) | 1;
        copy_bytes(info.slug, package.slug, sizeof(info.slug));
        copy_bytes(info.name, package.name, sizeof(info.name));
        copy_bytes(info.version, package.version, sizeof(info.version));
        copy_bytes(info.license, package.license, sizeof(info.license));
        copy_bytes(info.authors, package.authors, sizeof(info.authors));
        info.bytes = bytes;
        info.knob_count = package.knob_count;
        info.developer = developer;
        entry.blend = package.blend;
        entry.requested_resolution = package.requested_resolution;
        entry.requested_frame_rate = package.requested_frame_rate;
        entry.modified = static_cast<u64>(found.ftLastWriteTime.dwHighDateTime) << 32 | found.ftLastWriteTime.dwLowDateTime;
        // One installed file per slug: the newer one wins (a catalog update removes the old one after writing).
        bool replaced = false;
        for (u32 i = 0; i < g.count && !developer; ++i)
            if (!g.entries[i].info.developer && text_is(g.entries[i].info.slug, info.slug)) {
                if (g.entries[i].modified <= entry.modified)
                    g.entries[i] = entry;
                replaced = true;
                break;
            }
        if (replaced || g.count == kEffectLibraryCapacity)
            continue;
        g.entries[g.count++] = entry;
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

Entry* find(u64 identity) noexcept {
    for (u32 i = 0; identity && i < g.count; ++i)
        if (g.entries[i].info.identity == identity)
            return &g.entries[i];
    return nullptr;
}

Entry* find_slug(const char* slug) noexcept {
    for (u32 i = 0; i < g.count; ++i)
        if (text_is(g.entries[i].info.slug, slug))
            return &g.entries[i];
    return nullptr;
}

// ---- Simulated inputs ---------------------------------------------------------------------------------------
bool word_is(const char* text, u32 bytes, const char* word) noexcept {
    return bytes == c_string_length(word) && bytes_equal(text, word, bytes);
}
// 1..6 decimal digits.
bool digits(const char* text, u32 bytes, u32* value) noexcept {
    if (!bytes || bytes > 6)
        return false;
    u32 v = 0;
    for (u32 i = 0; i < bytes; ++i) {
        if (text[i] < '0' || text[i] > '9')
            return false;
        v = v * 10 + static_cast<u32>(text[i] - '0');
    }
    *value = v;
    return true;
}
void simulation_line(Simulation& s, const char* line, u32 bytes) noexcept {
    u32 equals = 0;
    while (equals < bytes && line[equals] != '=')
        ++equals;
    if (equals == bytes)
        return;
    const char* value = line + equals + 1;
    const u32 value_bytes = bytes - equals - 1;
    u32 a = 0, b = 0;
    const bool pair = value_bytes == 5 && digits(value, 2, &a) && digits(value + 3, 2, &b);
    if (word_is(line, equals, "time") && pair && value[2] == ':' && a < 24 && b < 60) {
        s.time = true;
        s.minutes = static_cast<u16>(a * 60 + b);
    } else if (word_is(line, equals, "date") && pair && value[2] == '-' && b >= 1 && b <= effect_month_days(2000, a)) {
        s.date = true;
        s.month = static_cast<u16>(a);
        s.day = static_cast<u16>(b);
    } else if (word_is(line, equals, "appearance") &&
               (word_is(value, value_bytes, "dark") || word_is(value, value_bytes, "light"))) {
        s.appearance = true;
        s.dark = value_bytes == 4;
    } else if (word_is(line, equals, "day_seconds") && digits(value, value_bytes, &a) && a >= 10 && a <= 86400) {
        s.day_seconds = a;
    }
}
// The development folder's simulate-inputs.txt, written by `maicord-plugin effect preview --time/--date/
// --appearance`, read with every library scan (the folder's watcher rescans when it changes). One key=value per
// line: time=HH:MM, date=MM-DD, appearance=dark|light, day_seconds=N (a whole simulated day passes in N seconds,
// from `time`). A key left out, or no file, keeps the real source.
void read_simulation() noexcept {
    Simulation& s = g.inputs.simulation;
    s = {};
    wchar_t path[512];
    if (!folder_path(path, true))
        return;
    u32 length = wide_length(path, 480);
    if (!wide_append(path, 512, &length, L"\\simulate-inputs.txt"))
        return;
    char text[512];
    const u32 bytes = read_file(path, {reinterpret_cast<u8*>(text), sizeof(text)});
    for (u32 at = 0; at < bytes;) {
        u32 end = at;
        while (end < bytes && text[end] != '\n')
            ++end;
        u32 stop = end;
        while (stop > at && (text[stop - 1] == '\r' || text[stop - 1] == ' '))
            --stop;
        simulation_line(s, text + at, stop - at);
        at = end + 1;
    }
    s.anchor_ms = clock_monotonic_milliseconds();
    if (s.time || s.date || s.appearance)
        log_write(LogLevel::info, "effects", "simulated_inputs");
}

void load_library() noexcept {
    if (g.library_loaded)
        return;
    g.library_loaded = true;
    InterlockedExchange(&g.dirty, 0);
    reset_value(&g.entries);
    g.count = 0;
    auto* buffer = static_cast<u8*>(commit_pages(kMcfxMaximumBytes));
    ScratchPages<McfxWorkspace> workspace;
    if (buffer && workspace.data) {
        scan_folder(false, buffer, workspace.data);
        scan_folder(true, buffer, workspace.data);
    }
    if (buffer)
        VirtualFree(buffer, 0, MEM_RELEASE);
    // Sorted by name.
    for (u32 i = 1; i < g.count; ++i)
        for (u32 j = i; j && name_before(g.entries[j], g.entries[j - 1]); --j) {
            Entry swap = g.entries[j];
            g.entries[j] = g.entries[j - 1];
            g.entries[j - 1] = swap;
        }
    read_simulation();
}

// ---- Settings records ----------------------------------------------------------------------------------
i32 override_index(u64 identity) noexcept {
    for (u32 i = 0; i < g.file.override_count; ++i)
        if (g.file.overrides[i].identity == identity)
            return static_cast<i32>(i);
    return -1;
}

// The global light-theme row: Auto unless the user turned it off.
bool light_auto() noexcept {
    return !(g.file.flags & kLightOff);
}

i32 refusal_index(u64 identity, u64 adapter) noexcept {
    for (u32 i = 0; i < g.file.refusal_count; ++i)
        if (g.file.refusals[i].identity == identity && g.file.refusals[i].adapter == adapter)
            return static_cast<i32>(i);
    return -1;
}

void forget_refusal(u64 identity, u64 adapter) noexcept {
    const i32 at = refusal_index(identity, adapter);
    if (at < 0)
        return;
    g.file.refusals[at] = g.file.refusals[--g.file.refusal_count];
    reset_value(&g.file.refusals[g.file.refusal_count]);
}

// A record per package, per adapter; the oldest makes room.
void note_refusal(u64 identity, u64 adapter) noexcept {
    if (refusal_index(identity, adapter) >= 0)
        return;
    if (g.file.refusal_count == kRefusals) {
        for (u32 i = 1; i < kRefusals; ++i)
            g.file.refusals[i - 1] = g.file.refusals[i];
        --g.file.refusal_count;
    }
    g.file.refusals[g.file.refusal_count++] = {identity, adapter};
}

// The notice for an effect that went off: its name, then why.
void notify(u64 identity, Utf8View reason) noexcept {
    char text[kMcfxNameBytes + 96];
    u32 bytes = 0;
    const Entry* entry = find(identity);
    const Utf8View name = entry ? Utf8View{entry->info.name, c_string_length(entry->info.name)}
                                : utf8_literal("A background effect");
    for (u32 i = 0; i < name.bytes && bytes < kMcfxNameBytes; ++i)
        text[bytes++] = name.data[i];
    for (u32 i = 0; i < reason.bytes && bytes < sizeof(text); ++i)
        text[bytes++] = reason.data[i];
    application_toast_show(UiToastKind::failure, {text, bytes});
}

void clear_effect() noexcept {
    renderer_clear_effect(&g_app.renderer);
    g.selected = false;
    g.inputs.identity = g.inputs.next_frame_ms = 0;
}

// The effect leaves the window and the pages that show it follow.
void drop_effect() noexcept {
    clear_effect();
    application_settings_refresh_pages();
    application_request_frame();
}

void forget_selection(u64 identity) noexcept {
    if (g.file.selected == identity) {
        g.file.selected = 0;
        save_state();
    }
    drop_effect();
}

// The renderer turned the effect off while drawing it, or a previous run died inside one of its frames.
void apply_verdict() noexcept {
    auto& effect = g_app.renderer.effect;
    if (effect.verdict == EffectVerdict::none)
        return;
    const EffectVerdict verdict = effect.verdict;
    const u64 identity = effect.verdict_identity;
    effect.verdict = EffectVerdict::none;
    const bool trial = g.trial.active && identity == g.trial.identity;
    if (trial || (g.preview.swapped && identity == g.preview.identity)) {
        // A candidate, or another entry previewed in the window's place, is not remembered: its card or page
        // reports it.
        (trial ? g.trial.state : g.preview.state) = EffectTrialState::refused;
        clear_effect();
        application_request_frame();
        return;
    }
    note_refusal(identity, g_app.renderer.adapter_key);
    log_u64(LogLevel::warning, "effects", "effect_turned_off_verdict", static_cast<u64>(verdict));
    notify(identity, verdict == EffectVerdict::refused
                         ? utf8_literal(" is too heavy for this graphics card")
                     : verdict == EffectVerdict::overloaded
                         ? utf8_literal(" was turned off because it slowed down drawing")
                         : utf8_literal(" was turned off because the graphics device was reset while it drew"));
    forget_selection(identity);
}

// The marker mapping (see D3D11EffectOwner::marker): a word written before a frame is submitted and cleared once
// its timing resolves. A word left set means a previous process died with that frame in flight.
void open_marker() noexcept {
    if (g.marker_opened)
        return;
    g.marker_opened = true;
    wchar_t path[512];
    if (!data_path(path, 512, kMarkerFile, true).ok())
        return;
    const HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    // The mapping and the view live until the process exits: the renderer writes to them to its last frame.
    const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 8, nullptr);
    CloseHandle(file);
    if (!mapping)
        return;
    auto* word = static_cast<volatile u64*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 8));
    if (!word)
        return;
    const u64 leftover = *word;
    *word = 0;
    d3d11_effect_set_marker(&g_app.renderer.effect, word);
    if (leftover) {
        note_refusal(leftover, g_app.renderer.adapter_key);
        log_u64(LogLevel::warning, "effects", "effect_frame_left_in_flight", leftover);
        notify(leftover, utf8_literal(" was turned off because MaiCord stopped while it was drawing"));
        forget_selection(leftover);
    }
}

// ---- Selection ------------------------------------------------------------------------------------------
bool turned_off(const char* slug) noexcept {
    for (const auto& off : g.off)
        if (off[0] && text_is(off, slug))
            return true;
    return false;
}

// ---- Ambient inputs (mc_ambient) ----------------------------------------------------------------------------
// The local date and time: the clock is read at each whole minute (the input's granularity), then a simulation
// from the development folder may stand in for the time of day or the date.
EffectLocalTime local_time(u64 now_ms) noexcept {
    Inputs& in = g.inputs;
    if (!in.clock_next_ms || now_ms >= in.clock_next_ms) {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        in.local = {time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute};
        in.clock_next_ms = now_ms + (60u - minimum<u32>(time.wSecond, 59u)) * 1000u - minimum<u32>(time.wMilliseconds, 999u);
    }
    EffectLocalTime local = in.local;
    const Simulation& s = in.simulation;
    if (!g_app.preferences.developer_mode)
        return local;
    if (s.time) {
        u64 minutes = s.minutes;
        if (s.day_seconds && now_ms > s.anchor_ms)
            minutes += (now_ms - s.anchor_ms) * 1440u / (u64{s.day_seconds} * 1000u);
        minutes %= 1440u;
        local.hour = static_cast<u16>(minutes / 60u);
        local.minute = static_cast<u16>(minutes % 60u);
    }
    if (s.date) {
        local.month = s.month;
        local.day = static_cast<u16>(minimum<u32>(s.day, effect_month_days(local.year, s.month)));
    }
    return local;
}

// An input may glide: the background shows and could move, and the window has focus or the effect keeps moving
// unfocused. Being idle does not stop it; otherwise (hidden, covered, reduced motion, unfocused) it settles at once.
bool transition_allowed() noexcept {
    return application_background_motion_possible(true) &&
           (application_window_focused() || g.file.user.unfocused == EffectUnfocused::keep);
}

// The ambient inputs and the knobs glide on the performance counter, in milliseconds: GetTickCount64 moves in 16 ms
// steps, so frames at 30 or 60 a second would see a glide stand still on one and leap on the next.
u64 ease_now() noexcept {
    return application_now_us() / 1000;
}

// mc_knobs as this frame shows them: a float or colour knob glides to a change (mcfx_knob_glide) where the window can
// show it moving; a new package, or a window that can't, takes them whole. The step is at most one effect frame long,
// so a change after a quiet spell starts like any other frame.
void glide_knobs(u64 identity, const McfxPackage& package, float (&knobs)[kEffectKnobRows][4]) noexcept {
    Inputs& in = g.inputs;
    const u64 now = ease_now();
    const u32 step = static_cast<u32>(minimum<u64>(now - in.knobs_ms, application_effects_interval_ms()));
    in.knobs_moving = in.identity == identity && transition_allowed() && mcfx_knob_glide(package, knobs, in.knobs, step);
    if (!in.knobs_moving)
        copy_bytes(in.knobs, knobs, sizeof(in.knobs));
    in.knobs_ms = now;
    copy_bytes(knobs, in.knobs, sizeof(in.knobs));
}

// Samples and eases the selected package's ambient inputs and its light adaptation (`adapt`: the user's Auto for it)
// and hands them to the renderer.
void sample_inputs(u64 identity, u32 uses, bool adapt) noexcept {
    Inputs& in = g.inputs;
    if (in.identity != identity) {
        in.ambient = {};
        in.identity = identity;
    }
    in.next_frame_ms = 0;
    const u64 now = clock_monotonic_milliseconds();
    EffectAmbientSources sources{};
    if (uses & effect_use_clock)
        sources.local = local_time(now);
    if ((uses & effect_use_appearance) || adapt) {
        if (!in.system_read) {
            in.system_dark = application_effects_read_system_dark();
            in.system_read = true;
        }
        sources.theme_dark = application_theme().appearance != ThemeAppearance::light;
        sources.system_dark = in.system_dark;
        sources.theme_scheduled = theme_publication.scheduled;
        if (g_app.preferences.developer_mode && in.simulation.appearance)
            sources.theme_dark = sources.system_dark = in.simulation.dark;
    }
    float values[4];
    const u64 eased = ease_now();
    const float light = effect_ambient_sample(&in.ambient, uses, sources, eased, transition_allowed(), adapt, values);
    renderer_set_effect_ambient(&g_app.renderer, values);
    renderer_set_effect_light(&g_app.renderer, light);
    // The next frame is due on the loop's clock.
    if (effect_ambient_easing(in.ambient, eased) || in.knobs_moving)
        in.next_frame_ms = now + application_effects_interval_ms();
}

// Texture pixels for one build of the shader; the renderer copies them inside the select call.
struct Textures {
    u8* memory[kEffectTextureCount];
};
void free_textures(Textures& t) noexcept {
    for (auto*& memory : t.memory)
        if (memory) {
            VirtualFree(memory, 0, MEM_RELEASE);
            memory = nullptr;
        }
}
bool decode_textures(const McfxPackage& package, BackgroundEffectSpec* spec, Textures* textures) noexcept {
    DeflateWorkspace* workspace = nullptr;
    bool ok = true;
    for (u32 i = 0; i < kEffectTextureCount && ok; ++i) {
        if (!package.texture[i].data)
            continue;
        // A float data texture is used as the package holds it.
        u32 width{}, height{};
        if (mcfx_float_texture(package.texture[i], &width, &height)) {
            spec->textures[i] = {package.texture[i].data + 8, static_cast<u16>(width), static_cast<u16>(height), true};
            continue;
        }
        PngImage image;
        if (!png_parse(package.texture[i], &image).ok() || image.width > kEffectTextureMaximumExtent ||
            image.height > kEffectTextureMaximumExtent || image.bit_depth != 8 ||
            (image.color_type != 2 && image.color_type != 6) || (!workspace && !(workspace = deflate_workspace_allocate()))) {
            ok = false;
            break;
        }
        const u32 pixels = image.width * image.height;
        const u32 scan_bytes = png_decode_buffer_bytes(image);
        auto* scan = static_cast<u8*>(commit_pages(scan_bytes));
        textures->memory[i] = static_cast<u8*>(commit_pages(pixels * 4));
        ok = scan && textures->memory[i] && png_decode_parsed_scanlines(&image, workspace, {scan, scan_bytes}).ok();
        for (u32 y = 0; ok && y < image.height; ++y)
            for (u32 x = 0; x < image.width; ++x) {
                const u8* from = scan + y * image.row_stride + 1 + x * image.bytes_per_pixel;
                u8* to = textures->memory[i] + (y * image.width + x) * 4;
                to[0] = from[0];
                to[1] = from[1];
                to[2] = from[2];
                to[3] = image.color_type == 6 ? from[3] : 255;
            }
        if (scan)
            VirtualFree(scan, 0, MEM_RELEASE);
        spec->textures[i] = {textures->memory[i], static_cast<u16>(image.width), static_cast<u16>(image.height), false};
    }
    deflate_workspace_free(workspace);
    return ok;
}

// Draws `entry`'s package, or the trial's; `light` is the user's light-theme choice for it (effect_light_auto).
// False: nothing is selected now.
bool select_package(u64 identity, const McfxPackage& package, const McfxKnobValue (&values)[kMcfxKnobMaximum],
                    EffectBlend blend, EffectSettings settings, bool light, bool trial) noexcept {
    BackgroundEffectSpec spec{};
    spec.bytecode = package.shaders[0];
    if (package.graph.pass_count) {
        spec.graph = &package.graph;
        spec.shaders = package.shaders;
    }
    spec.identity = identity;
    spec.uses = package.uses;
    spec.blend = blend;
    spec.requested_resolution = package.requested_resolution;
    spec.requested_frame_rate = package.requested_frame_rate;
    mcfx_knob_pack(package, values, spec.knobs);
    g.settings = settings;
    g.selected = true;
    glide_knobs(identity, package, spec.knobs);
    if ((package.uses & effect_use_pointer) && settings.pointer_allowed && g_app.window) {
        POINT cursor{};
        if (GetCursorPos(&cursor) && ScreenToClient(g_app.window, &cursor))
            d3d11_effect_set_pointer(&g_app.renderer.effect, static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    }
    // Textures are decoded only when the renderer has to build the shader again.
    const auto& owner = g_app.renderer.effect;
    Textures textures{};
    if (!owner.selected || owner.identity != identity || !owner.shader)
        if (!decode_textures(package, &spec, &textures)) {
            free_textures(textures);
            return false;
        }
    const EffectSelectResult result = renderer_select_effect(&g_app.renderer, spec, settings);
    free_textures(textures);
    if (result.state == EffectSelect::rejected) {
        log_u64(LogLevel::warning, "effects", "effect_shader_error", static_cast<u32>(result.native_error));
        if (!trial) {
            notify(identity, utf8_literal(" was turned off because it could not be drawn"));
            forget_selection(identity);
        }
        return false;
    }
    sample_inputs(identity, package.uses, light);
    // A refusal by the cost probe arrives as a verdict.
    apply_verdict();
    return true;
}

// A knob page's preview of an entry other than `drawn` (the window's own effect, or none): it draws in its place
// with the values and settings the page edits, at the Low budget. True when it took the frame.
bool select_preview(const Entry* drawn, u64 adapter) noexcept {
    g.preview.swapped = false;
    const u64 identity = g.preview.identity;
    if (!identity || g.preview.state != EffectTrialState::drawing || (drawn && drawn->info.identity == identity))
        return false;
    const Entry* entry = find(identity);
    if (!entry) {
        g.preview = {}; // removed, or changed by an update: the page asks again
        return false;
    }
    if (refusal_index(identity, adapter) >= 0) {
        g.preview.state = EffectTrialState::refused;
        return false;
    }
    const Loaded& open = g.opened_is_resident ? g.resident : g.opened;
    const u32 index = static_cast<u32>(entry - g.entries);
    if ((!g.opened_valid || open.identity != identity) && !application_effects_open(index)) {
        g.preview.state = EffectTrialState::rejected;
        return false;
    }
    const Loaded& l = g.opened_is_resident ? g.resident : g.opened;
    const EffectShaderSettings shader = application_effects_shader_settings(identity);
    EffectSettings settings = effect_resolve_settings(g.file.user, shader, entry->requested_resolution,
                                                      entry->requested_frame_rate, entry->blend);
    settings.budget = EffectBudget::low;
    settings.pointer_allowed = false;
    g.preview.swapped = true;
    if (!select_package(identity, l.package, l.values, entry->blend, settings, effect_light_auto(shader.light, light_auto()),
                        true)) {
        g.preview.state = EffectTrialState::rejected;
        g.preview.swapped = false;
        clear_effect();
    }
    return true;
}

} // namespace

void application_effects_synchronize() noexcept {
    if (!g_app.renderer_ready)
        return;
    load_state();
    open_marker();
    apply_verdict();
    if (InterlockedExchange(&g.dirty, 0) && g.library_loaded) {
        // A file changed: rescan, and keep the choice across an update (the file's hash changed, the slug did not).
        const Entry* before = find(g.file.selected);
        char slug[kMcfxSlugBytes + 1]{};
        if (before)
            copy_bytes(slug, before->info.slug, sizeof(slug));
        g.library_loaded = false;
        load_library();
        if (before && !find(g.file.selected))
            if (const Entry* after = find_slug(slug)) {
                g.file.selected = after->info.identity;
                save_state();
            }
        if (g.opened_valid && !g.opened_is_resident)
            close_opened();
        application_settings_refresh_pages();
    }
    if (g.trial.active) {
        if (g.trial.state != EffectTrialState::drawing) {
            if (g.selected)
                clear_effect();
            return;
        }
        McfxKnobValue values[kMcfxKnobMaximum];
        mcfx_knob_defaults(*g.trial.package, values);
        auto settings = effect_resolve_settings(g.file.user, effect_shader_defaults(g.trial.identity),
                                                g.trial.package->requested_resolution, g.trial.package->requested_frame_rate,
                                                g.trial.package->blend);
        settings.budget = EffectBudget::low;
        settings.pointer_allowed = false;
        if (settings.resolution_step > kEffectDefaultResolutionStep)
            settings.resolution_step = kEffectDefaultResolutionStep;
        if (!select_package(g.trial.identity, *g.trial.package, values, g.trial.package->blend, settings, light_auto(),
                            true)) {
            if (g.trial.state == EffectTrialState::drawing)
                g.trial.state = EffectTrialState::rejected;
            clear_effect();
        }
        return;
    }
    const ClientThemeEffect* theme = nullptr;
    Entry* entry = nullptr;
    if (g.file.selected) {
        load_library();
        entry = find(g.file.selected);
    } else if ((theme = theme_effect())) {
        load_library();
        entry = turned_off(theme->slug) ? nullptr : find_slug(theme->slug);
    }
    const u64 adapter = g_app.renderer.adapter_key;
    if (entry && refusal_index(entry->info.identity, adapter) >= 0)
        entry = nullptr;
    if (select_preview(entry, adapter))
        return;
    if (!entry) {
        // Not installed (any more), or off on this GPU: the built-in effect applies and the choice stays saved.
        if (g.selected)
            clear_effect();
        return;
    }
    if (g.resident.identity != entry->info.identity || g.resident.theme_hash != theme_hash(theme)) {
        const bool same = g.resident.identity == entry->info.identity && g.resident.file;
        if (same) {
            init_values(g.resident, theme);
        } else if (!load(g.resident, *entry, theme)) {
            notify(entry->info.identity, utf8_literal(" could not be read"));
            forget_selection(entry->info.identity);
            return;
        }
    }
    const i32 own = override_index(entry->info.identity);
    EffectShaderSettings shader = own >= 0 ? g.file.overrides[own] : effect_shader_defaults(entry->info.identity);
    if (theme && own < 0) {
        // The theme's universal values stand in for the defaults until the user sets their own.
        if (theme->universal & theme_effect_speed) shader.speed = theme->speed;
        if (theme->universal & theme_effect_opacity) shader.opacity = theme->opacity;
        if (theme->universal & theme_effect_resolution) shader.resolution = theme->resolution;
        if (theme->universal & theme_effect_frame_rate) shader.frame_rate = theme->frame_rate;
        if (theme->universal & theme_effect_blend) shader.blend = static_cast<u8>(theme->blend + 1);
    }
    const EffectSettings settings = effect_resolve_settings(g.file.user, shader, entry->requested_resolution,
                                                            entry->requested_frame_rate, entry->blend);
    select_package(entry->info.identity, g.resident.package, g.resident.values, entry->blend, settings,
                   effect_light_auto(shader.light, light_auto()), false);
}

void application_effects_project(UiSettingsControl* control) noexcept {
    if (control->choices.count != kBackgroundEffectCount)
        return;
    load_state();
    load_library();
    if (!g.count)
        return;
    for (u32 i = 0; i < kBackgroundEffectCount; ++i)
        g.choices[i] = control->choices[i];
    for (u32 i = 0; i < g.count; ++i) {
        g.choices[kBackgroundEffectCount + i] = {g.entries[i].info.name, c_string_length(g.entries[i].info.name)};
        if (g.entries[i].info.identity == g.file.selected)
            control->selected_choice = kBackgroundEffectCount + i;
    }
    control->choices = {g.choices, kBackgroundEffectCount + g.count};
}

bool application_effects_select(u32 index) noexcept {
    load_state();
    load_library();
    if (index >= g.count)
        return false;
    // Choosing an effect again is the user turning it back on after a refusal.
    forget_refusal(g.entries[index].info.identity, g_app.renderer.adapter_key);
    for (auto& off : g.off)
        if (text_is(off, g.entries[index].info.slug))
            off[0] = 0;
    g.file.selected = g.entries[index].info.identity;
    const bool saved = save_state();
    application_settings_refresh_pages();
    application_request_frame();
    return saved;
}

bool application_effects_select_none() noexcept {
    load_state();
    if (!g.file.selected)
        return true;
    g.file.selected = 0;
    const bool saved = save_state();
    drop_effect();
    return saved;
}

bool application_effects_activate(u32 choice, bool* result) noexcept {
    *result = false;
    load_state();
    if (choice < kBackgroundEffectCount) {
        // A built-in: its own preference is written by the caller.
        if (g.file.selected) {
            g.file.selected = 0;
            save_state();
            renderer_clear_effect(&g_app.renderer);
            application_request_frame();
        }
        return false;
    }
    load_library();
    if (choice - kBackgroundEffectCount >= g.count)
        return true;
    *result = application_effects_select(choice - kBackgroundEffectCount);
    return true;
}

void application_effects_note_input() noexcept {
    g.last_input_ms = GetTickCount64();
}

bool application_effects_selected() noexcept {
    return g.selected;
}

bool application_effects_motion_allowed() noexcept {
    if (!g.selected)
        return true;
    if (g.file.user.unfocused == EffectUnfocused::pause && !application_window_focused())
        return false;
    const u64 now = GetTickCount64();
    return !g.last_input_ms || now < g.last_input_ms || now - g.last_input_ms < kIdleMilliseconds;
}

u32 application_effects_interval_ms() noexcept {
    const bool slow = g.file.user.unfocused == EffectUnfocused::slow && !application_window_focused();
    return effect_interval_ms(g.selected ? g.settings.frame_rate_step : kEffectDefaultFrameRateStep, slow);
}

u64 application_effects_transition_wake() noexcept {
    return g.selected && g.inputs.next_frame_ms && transition_allowed() ? g.inputs.next_frame_ms : 0;
}

EffectLocalTime application_effects_local_time(u64 now_ms, u64* next_ms) noexcept {
    const EffectLocalTime local = local_time(now_ms);
    *next_ms = g.inputs.clock_next_ms;
    return local;
}

bool application_effects_read_system_dark() noexcept {
    DWORD light = 1, bytes = sizeof(light);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
    // Without the value Windows draws apps light.
    return status == ERROR_SUCCESS && light == 0;
}

void application_effects_set_system_dark(bool dark) noexcept {
    const bool changed = !g.inputs.system_read || g.inputs.system_dark != dark;
    g.inputs.system_read = true;
    g.inputs.system_dark = dark;
    if (changed)
        application_request_frame();
}

EffectUserSettings application_effects_user_settings() noexcept {
    load_state();
    return g.file.user;
}

bool application_effects_set_user_settings(const EffectUserSettings& value) noexcept {
    load_state();
    if (!effect_user_settings_valid(value))
        return false;
    g.file.user = value;
    application_request_frame();
    return save_state();
}

EffectShaderSettings application_effects_shader_settings(u64 identity) noexcept {
    load_state();
    const i32 at = override_index(identity);
    return at >= 0 ? g.file.overrides[at] : effect_shader_defaults(identity);
}

bool application_effects_set_shader_settings(const EffectShaderSettings& value) noexcept {
    load_state();
    if (!value.identity || !effect_shader_settings_valid(value))
        return false;
    i32 at = override_index(value.identity);
    if (at < 0) {
        if (g.file.override_count == kOverrides) {
            // The oldest record makes room.
            for (u32 i = 1; i < kOverrides; ++i)
                g.file.overrides[i - 1] = g.file.overrides[i];
            --g.file.override_count;
        }
        at = static_cast<i32>(g.file.override_count++);
    }
    g.file.overrides[at] = value;
    application_request_frame();
    return save_state();
}

bool application_effects_light_auto() noexcept {
    load_state();
    return light_auto();
}

bool application_effects_set_light_auto(bool on) noexcept {
    load_state();
    g.file.flags = on ? g.file.flags & ~kLightOff : g.file.flags | kLightOff;
    application_request_frame();
    return save_state();
}

u32 application_effects_count() noexcept {
    load_library();
    return g.count;
}

u64 application_effects_identity(u32 index) noexcept {
    load_library();
    return index < g.count ? g.entries[index].info.identity : 0;
}

const EffectInfo* application_effects_info(u32 index) noexcept {
    load_state();
    load_library();
    if (index >= g.count)
        return nullptr;
    g.entries[index].info.selected = g.entries[index].info.identity == g.file.selected;
    return &g.entries[index].info;
}

i32 application_effects_selected_index() noexcept {
    load_state();
    load_library();
    for (u32 i = 0; g.file.selected && i < g.count; ++i)
        if (g.entries[i].info.identity == g.file.selected)
            return static_cast<i32>(i);
    return -1;
}

void application_effects_reload() noexcept {
    g.library_loaded = false;
    load_library();
}

void application_effects_library_changed() noexcept {
    InterlockedExchange(&g.dirty, 1);
}

const McfxPackage* application_effects_open(u32 index) noexcept {
    load_state();
    load_library();
    close_opened();
    if (index >= g.count)
        return nullptr;
    const Entry& entry = g.entries[index];
    if (g.resident.file && g.resident.identity == entry.info.identity) {
        g.opened_is_resident = true;
    } else {
        if (!load(g.opened, entry, theme_for(entry.info.slug)))
            return nullptr;
        g.opened_is_resident = false;
    }
    g.opened_valid = true;
    return g.opened_is_resident ? &g.resident.package : &g.opened.package;
}

bool application_effects_knob_get(u32 knob, McfxKnobValue* value) noexcept {
    Loaded& l = g.opened_is_resident ? g.resident : g.opened;
    if (!g.opened_valid || knob >= l.package.knob_count)
        return false;
    *value = l.values[knob];
    return true;
}

bool application_effects_knob_set(u32 knob, const McfxKnobValue& value, bool commit) noexcept {
    Loaded& l = g.opened_is_resident ? g.resident : g.opened;
    if (!g.opened_valid || knob >= l.package.knob_count || !mcfx_knob_value_valid(l.package.knobs[knob], value))
        return false;
    l.values[knob] = value;
    application_request_frame();
    return !commit || save_knob_file(l);
}

bool application_effects_knobs_reset() noexcept {
    Loaded& l = g.opened_is_resident ? g.resident : g.opened;
    if (!g.opened_valid)
        return false;
    delete_knob_file(l.package.slug);
    init_values(l, theme_for(l.package.slug));
    application_request_frame();
    return true;
}

float application_effects_drawn_knob(const char* slug, const char* id) noexcept {
    const Loaded& l = g.resident;
    if (g.selected && !g.trial.active && !g.preview.swapped && l.file && text_is(l.package.slug, slug))
        for (u32 k = 0; k < l.package.knob_count; ++k)
            if (text_is(l.package.knobs[k].id, id))
                return l.values[k].number;
    return -1;
}

EffectCost application_effects_cost(u64 identity) noexcept {
    if (!identity)
        return {};
    const auto level = [](float ms) noexcept {
        return ms <= kEffectBudgetMs[0] ? EffectCostClass::light : ms <= kEffectBudgetMs[1] ? EffectCostClass::medium
                                                                                            : EffectCostClass::heavy;
    };
    if ((g.trial.active && g.trial.identity == identity && g.trial.state == EffectTrialState::refused) ||
        (g.preview.identity == identity && g.preview.state == EffectTrialState::refused) ||
        refusal_index(identity, g_app.renderer.adapter_key) >= 0)
        return {true, true, kEffectRefuseMs, EffectCostClass::heavy};
    const auto& owner = g_app.renderer.effect;
    if (owner.selected && owner.identity == identity && owner.probed) {
        const float ms = effect_probe_estimate(owner.probe, effect_step_pixels(owner.window_width, owner.window_height, owner.step));
        return {true, false, ms, level(ms)};
    }
    return {};
}

void application_effects_deactivate(const char* slug, u32 slug_bytes) noexcept {
    if (!slug || !slug_bytes || slug_bytes > kMcfxSlugBytes)
        return;
    char text[kMcfxSlugBytes + 1]{};
    copy_bytes(text, slug, slug_bytes);
    load_state();
    load_library();
    const Entry* selected = find(g.file.selected);
    if (selected && text_is(selected->info.slug, text)) {
        g.file.selected = 0;
        save_state();
    }
    if (theme_for(text)) {
        copy_bytes(g.off[g.off_next], text, sizeof(text));
        g.off_next = (g.off_next + 1) % 4;
    }
    drop_effect();
}

bool application_effects_remove(const char* slug, u32 slug_bytes) noexcept {
    if (!slug || !slug_bytes || slug_bytes > kMcfxSlugBytes)
        return false;
    char text[kMcfxSlugBytes + 1]{};
    copy_bytes(text, slug, slug_bytes);
    load_state();
    load_library();
    const Entry* entry = find_slug(text);
    if (!entry || entry->info.developer)
        return false;
    wchar_t path[kPathUnits];
    copy_bytes(path, entry->path, sizeof(path));
    if (g.resident.identity == entry->info.identity)
        release(g.resident);
    close_opened();
    application_effects_deactivate(slug, slug_bytes);
    const bool removed = DeleteFileW(path) != 0;
    delete_knob_file(text);
    g.library_loaded = false;
    load_library();
    application_settings_refresh_pages();
    return removed;
}

bool application_effects_trial_begin(const ConstSlice<u8>& file, const McfxPackage& package) noexcept {
    if (g.trial.active)
        return false;
    load_state();
    g.trial = {true, EffectTrialState::drawing, hash64_bytes(kHash64Seed, file.data, file.count) | 1, file.data, file.count,
               &package};
    application_request_frame();
    return true;
}

void application_effects_trial_end() noexcept {
    if (!g.trial.active)
        return;
    reset_value(&g.trial);
    clear_effect();
    application_request_frame();
}

EffectTrialState application_effects_trial_state() noexcept {
    return g.trial.active ? g.trial.state : EffectTrialState::none;
}

u64 application_effects_trial_identity() noexcept {
    return g.trial.active ? g.trial.identity : 0;
}

void application_effects_preview(u32 index, bool running) noexcept {
    u64 identity = 0;
    if (index != ~0u) {
        load_library();
        identity = index < g.count ? g.entries[index].info.identity : 0;
    }
    running = running && identity;
    if (identity == g.preview.identity && running == g.preview.running)
        return;
    if (identity != g.preview.identity)
        g.preview = {identity, false, false, identity ? EffectTrialState::drawing : EffectTrialState::none};
    g.preview.running = running;
    application_request_frame();
}

EffectTrialState application_effects_preview_state(u64 identity) noexcept {
    if (!identity || identity != g.preview.identity)
        return EffectTrialState::none;
    if (g.preview.state != EffectTrialState::drawing)
        return g.preview.state;
    const auto& owner = g_app.renderer.effect;
    return !g.trial.active && g.selected && owner.selected && owner.identity == identity ? EffectTrialState::drawing
                                                                                         : EffectTrialState::none;
}

bool application_effects_preview_running() noexcept {
    return g.preview.running && application_effects_preview_state(g.preview.identity) == EffectTrialState::drawing &&
           application_window_focused();
}

bool application_effects_state_flag(u32 bit) noexcept {
    load_state();
    return (g.file.flags & bit) != 0;
}

bool application_effects_set_state_flag(u32 bit, bool on) noexcept {
    load_state();
    g.file.flags = on ? g.file.flags | bit : g.file.flags & ~bit;
    return save_state();
}

} // namespace maicord::app_detail
