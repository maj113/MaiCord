#include "effects.hpp"
#include "../settings/custom_theme.hpp"
#include "../settings/settings.hpp"
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
#include "maicord/media/image/png.hpp"
#include "maicord/ui/settings/scene.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {

constexpr u64 kIdleMilliseconds = 5ull * 60 * 1000;
constexpr u32 kOverrides = 16, kRefusals = 16, kPathUnits = 400;
constexpr wchar_t kStateFile[] = L"effects\\state.bin";
constexpr wchar_t kMarkerFile[] = L"effects\\effect-frame.bin";

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
    char off[4][kMcfxSlugBytes + 1]; // a theme's effect the user's store turned off this session
    u32 off_next;
    volatile LONG dirty;
};
State g;

// ---- Files -----------------------------------------------------------------------------------------------
bool state_path(wchar_t (&path)[512], u32* length, bool create) noexcept {
    const DataPathResult built = data_path(path, 504, kStateFile, create);
    *length = built.length;
    return built.ok();
}

void load_state() noexcept {
    if (g.state_loaded)
        return;
    g.state_loaded = true;
    g.file.user = kEffectUserDefaults;
    wchar_t path[512];
    u32 length = 0;
    if (!state_path(path, &length, false))
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
    u32 length = 0;
    if (!state_path(path, &length, true))
        return false;
    copy_bytes(g.file.magic, kMagic, 4);
    g.file.version = 1;
    const auto written = file_persistence_write_atomic(path, length, {reinterpret_cast<const u8*>(&g.file), sizeof(g.file)});
    if (!written.ok())
        log_u64(LogLevel::warning, "effects", "state_save_error", written.native_error);
    return written.ok();
}

// Reads a whole file into `buffer`; the byte count, or 0.
u32 read_file(const wchar_t* path, Slice<u8> buffer) noexcept {
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
bool knobs_path(wchar_t (&path)[512], u32* length, const char* slug, bool create) noexcept {
    wchar_t relative[96] = L"effects\\";
    u32 at = 8;
    for (u32 i = 0; slug[i] && at < 80; ++i)
        relative[at++] = static_cast<wchar_t>(slug[i]);
    for (const wchar_t c : L".knobs")
        relative[at++] = c;
    const DataPathResult built = data_path(path, 504, relative, create);
    *length = built.length;
    return built.ok();
}

u32 id_hash(const char* id) noexcept { return hash32_bytes(reinterpret_cast<const u8*>(id), c_string_length(id)); }

struct KnobRecord {
    u32 id, number_bits, rgb;
};

void load_knob_file(Loaded& l) noexcept {
    wchar_t path[512];
    u32 length = 0;
    u8 bytes[12 + sizeof(KnobRecord) * kMcfxKnobMaximum];
    if (!knobs_path(path, &length, l.package.slug, false))
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
    u32 length = 0;
    u8 bytes[12 + sizeof(KnobRecord) * kMcfxKnobMaximum]{};
    if (!knobs_path(path, &length, l.package.slug, true))
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
    return file_persistence_write_atomic(path, length, {bytes, 12 + count * static_cast<u32>(sizeof(KnobRecord))}).ok();
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

// Defaults, then the theme's values, then what the user saved.
void init_values(Loaded& l, const ClientThemeEffect* theme) noexcept {
    mcfx_knob_defaults(l.package, l.values);
    if (theme)
        for (u32 i = 0; i < theme->knob_count; ++i)
            for (u32 k = 0; k < l.package.knob_count; ++k) {
                const auto& knob = l.package.knobs[k];
                if (c_string_length(knob.id) != c_string_length(theme->knobs[i].id) ||
                    !bytes_equal(knob.id, theme->knobs[i].id, c_string_length(knob.id)) ||
                    (theme->knobs[i].color != (knob.type == McfxKnobType::color)))
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
    l.file = static_cast<u8*>(VirtualAlloc(nullptr, entry.info.bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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

bool text_is(const char* a, const char* b) noexcept {
    const u32 n = c_string_length(a);
    return n == c_string_length(b) && bytes_equal(a, b, n);
}

// "<slug>-<16 hex>.mcfx": the 16 digits, or false.
bool installed_name(const wchar_t* name, const char* slug, char (&sha16)[17]) noexcept {
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
        sha16[i] = static_cast<char>(c);
    }
    sha16[16] = 0;
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
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE)
        return;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        Entry entry{};
        u32 path_length = 0;
        entry.path[0] = 0;
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
        if (!developer && !installed_name(found.cFileName, package.slug, info.sha16)) {
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
        info.uses = package.uses;
        info.knob_count = package.knob_count;
        info.developer = developer;
        info.has_source = package.source.count != 0;
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

void load_library() noexcept {
    if (g.library_loaded)
        return;
    g.library_loaded = true;
    InterlockedExchange(&g.dirty, 0);
    reset_value(&g.entries);
    g.count = 0;
    auto* buffer = static_cast<u8*>(VirtualAlloc(nullptr, kMcfxMaximumBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
}

// ---- Settings records ----------------------------------------------------------------------------------
i32 override_index(u64 identity) noexcept {
    for (u32 i = 0; i < g.file.override_count; ++i)
        if (g.file.overrides[i].identity == identity)
            return static_cast<i32>(i);
    return -1;
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

void forget_selection(u64 identity) noexcept {
    if (g.file.selected == identity) {
        g.file.selected = 0;
        save_state();
    }
    renderer_clear_effect(&g_app.renderer);
    g.selected = false;
    application_settings_refresh_pages();
    application_request_frame();
}

// The renderer turned the effect off while drawing it, or a previous run died inside one of its frames.
void apply_verdict() noexcept {
    auto& effect = g_app.renderer.effect;
    if (effect.verdict == EffectVerdict::none)
        return;
    const EffectVerdict verdict = effect.verdict;
    const u64 identity = effect.verdict_identity;
    effect.verdict = EffectVerdict::none;
    if (g.trial.active && identity == g.trial.identity) {
        // A candidate that cannot be drawn is not remembered: the install card reports it.
        g.trial.state = EffectTrialState::refused;
        renderer_clear_effect(&g_app.renderer);
        g.selected = false;
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
        PngImage image;
        if (!png_parse(package.texture[i], &image).ok() || image.width > kEffectTextureMaximumExtent ||
            image.height > kEffectTextureMaximumExtent || image.bit_depth != 8 ||
            (image.color_type != 2 && image.color_type != 6) || (!workspace && !(workspace = deflate_workspace_allocate()))) {
            ok = false;
            break;
        }
        const u32 pixels = image.width * image.height;
        const u32 scan_bytes = png_decode_buffer_bytes(image);
        auto* scan = static_cast<u8*>(VirtualAlloc(nullptr, scan_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        textures->memory[i] = static_cast<u8*>(VirtualAlloc(nullptr, pixels * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
        spec->textures[i] = {reinterpret_cast<const u32*>(textures->memory[i]), static_cast<u16>(image.width),
                             static_cast<u16>(image.height)};
    }
    deflate_workspace_free(workspace);
    return ok;
}

// Draws `entry`'s package, or the trial's. False: nothing is selected now.
bool select_package(u64 identity, const McfxPackage& package, const McfxKnobValue (&values)[kMcfxKnobMaximum],
                    EffectBlend blend, EffectSettings settings, bool trial) noexcept {
    BackgroundEffectSpec spec{};
    spec.bytecode = package.bytecode;
    spec.identity = identity;
    spec.uses = package.uses;
    spec.blend = blend;
    spec.requested_resolution = package.requested_resolution;
    spec.requested_frame_rate = package.requested_frame_rate;
    mcfx_knob_pack(package, values, spec.knobs);
    g.settings = settings;
    g.selected = true;
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
    // A refusal by the cost probe arrives as a verdict.
    apply_verdict();
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
            application_effects_close();
        application_settings_refresh_pages();
    }
    if (g.trial.active) {
        if (g.trial.state != EffectTrialState::drawing) {
            if (g.selected)
                renderer_clear_effect(&g_app.renderer);
            g.selected = false;
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
        if (!select_package(g.trial.identity, *g.trial.package, values, g.trial.package->blend, settings, true)) {
            if (g.trial.state == EffectTrialState::drawing)
                g.trial.state = EffectTrialState::rejected;
            renderer_clear_effect(&g_app.renderer);
            g.selected = false;
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
    if (!entry || refusal_index(entry->info.identity, adapter) >= 0) {
        // Not installed (any more), or off on this GPU: the built-in effect applies and the choice stays saved.
        if (g.selected)
            renderer_clear_effect(&g_app.renderer);
        g.selected = false;
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
    select_package(entry->info.identity, g.resident.package, g.resident.values, entry->blend, settings, false);
}

bool application_effects_project(UiSettingsControl* control) noexcept {
    if (!control || control->choices.count != kBackgroundEffectCount)
        return false;
    load_state();
    load_library();
    if (!g.count)
        return false;
    for (u32 i = 0; i < kBackgroundEffectCount; ++i)
        g.choices[i] = control->choices[i];
    for (u32 i = 0; i < g.count; ++i) {
        g.choices[kBackgroundEffectCount + i] = {g.entries[i].info.name, c_string_length(g.entries[i].info.name)};
        if (g.entries[i].info.identity == g.file.selected)
            control->selected_choice = kBackgroundEffectCount + i;
    }
    control->choices = {g.choices, kBackgroundEffectCount + g.count};
    return true;
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
    renderer_clear_effect(&g_app.renderer);
    g.selected = false;
    application_settings_refresh_pages();
    application_request_frame();
    return saved;
}

bool application_effects_activate(u32 choice, bool* result) noexcept {
    if (result)
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
    const bool saved = application_effects_select(choice - kBackgroundEffectCount);
    if (result)
        *result = saved;
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
    application_effects_close();
    if (index >= g.count)
        return nullptr;
    const Entry& entry = g.entries[index];
    const ClientThemeEffect* theme = theme_effect();
    if (g.resident.file && g.resident.identity == entry.info.identity) {
        g.opened_is_resident = true;
    } else {
        if (!load(g.opened, entry, theme && text_is(theme->slug, entry.info.slug) ? theme : nullptr))
            return nullptr;
        g.opened_is_resident = false;
    }
    g.opened_valid = true;
    return g.opened_is_resident ? &g.resident.package : &g.opened.package;
}

void application_effects_close() noexcept {
    if (g.opened_valid && !g.opened_is_resident)
        release(g.opened);
    g.opened_valid = g.opened_is_resident = false;
}

bool application_effects_knob_get(u32 knob, McfxKnobValue* value) noexcept {
    Loaded& l = g.opened_is_resident ? g.resident : g.opened;
    if (!g.opened_valid || !value || knob >= l.package.knob_count)
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
    wchar_t path[512];
    u32 length = 0;
    if (knobs_path(path, &length, l.package.slug, false))
        DeleteFileW(path);
    const ClientThemeEffect* theme = theme_effect();
    init_values(l, theme && text_is(theme->slug, l.package.slug) ? theme : nullptr);
    application_request_frame();
    return true;
}

EffectCost application_effects_cost(u64 identity) noexcept {
    EffectCost cost{};
    if (!identity)
        return cost;
    const auto level = [](float ms) noexcept {
        return ms <= kEffectBudgetMs[0] ? EffectCostClass::light : ms <= kEffectBudgetMs[1] ? EffectCostClass::medium
                                                                                            : EffectCostClass::heavy;
    };
    if ((g.trial.active && g.trial.identity == identity && g.trial.state == EffectTrialState::refused) ||
        refusal_index(identity, g_app.renderer.adapter_key) >= 0) {
        cost = {true, true, kEffectRefuseMs, EffectCostClass::heavy};
        return cost;
    }
    const auto& owner = g_app.renderer.effect;
    if (owner.selected && owner.identity == identity && owner.probed) {
        const float ms = effect_probe_estimate(owner.probe, effect_step_pixels(owner.window_width, owner.window_height, owner.step));
        cost = {true, false, ms, level(ms)};
    }
    return cost;
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
    if (const ClientThemeEffect* theme = theme_effect(); theme && text_is(theme->slug, text)) {
        copy_bytes(g.off[g.off_next], text, sizeof(text));
        g.off_next = (g.off_next + 1) % 4;
    }
    renderer_clear_effect(&g_app.renderer);
    g.selected = false;
    application_settings_refresh_pages();
    application_request_frame();
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
    application_effects_close();
    application_effects_deactivate(slug, slug_bytes);
    const bool removed = DeleteFileW(path) != 0;
    wchar_t knobs[512];
    u32 length = 0;
    if (knobs_path(knobs, &length, text, false))
        DeleteFileW(knobs);
    g.library_loaded = false;
    load_library();
    application_settings_refresh_pages();
    return removed;
}

bool application_effects_trial_begin(ConstSlice<u8> file, const McfxPackage& package) noexcept {
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
    renderer_clear_effect(&g_app.renderer);
    g.selected = false;
    application_request_frame();
}

EffectTrialState application_effects_trial_state() noexcept {
    return g.trial.active ? g.trial.state : EffectTrialState::none;
}

u64 application_effects_trial_identity() noexcept {
    return g.trial.active ? g.trial.identity : 0;
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
