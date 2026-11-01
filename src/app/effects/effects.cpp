#include "effects.hpp"
#include "../settings/settings.hpp"
#include "../shell/state.hpp"
#include "../shell/toast.hpp"
#include "../shell/window_state.hpp"
#include "maicord/app/net/file_persistence.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/settings/scene.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {

constexpr u32 kBytecodeMaximum = 64 * 1024, kSidecarMaximum = 4096, kNameBytes = 48;
constexpr u64 kIdleMilliseconds = 5ull * 60 * 1000;
constexpr u32 kOverrides = 16, kRefusals = 16;
constexpr wchar_t kStateFile[] = L"effects\\state.bin";
constexpr wchar_t kMarkerFile[] = L"effects\\effect-frame.bin";

struct Entry {
    u64 identity;
    u8* code;
    u32 code_bytes, name_bytes;
    char name[kNameBytes];
    u32 uses;
    EffectBlend blend;
    u8 requested_resolution, requested_frame_rate;
    float knobs[kEffectKnobRows][4];
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
    u32 override_count, refusal_count, reserved;
    EffectShaderSettings overrides[kOverrides];
    Refusal refusals[kRefusals];
};
static_assert(sizeof(StateFile) == 32 + 16 * kOverrides + 16 * kRefusals);
constexpr u8 kMagic[4]{'M', 'C', 'E', 'S'};

struct State {
    StateFile file;
    bool state_loaded, library_loaded, marker_opened;
    Entry entries[kEffectLibraryCapacity];
    u32 count;
    u64 last_input_ms;
    EffectSettings settings; // as last sent to the renderer
    bool selected;
    Utf8View choices[kBackgroundEffectCount + kEffectLibraryCapacity];
};
State g;

// ---- Text ------------------------------------------------------------------------------------
bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r'; }

bool word_is(const char* word, u32 bytes, const char* ascii) noexcept {
    u32 i = 0;
    for (; i < bytes && ascii[i]; ++i)
        if (word[i] != ascii[i])
            return false;
    return i == bytes && !ascii[i];
}

// Next space-separated word of [*at, end); false at the end.
bool next_word(const char** at, const char* end, const char** word, u32* bytes) noexcept {
    const char* p = *at;
    while (p < end && is_space(*p))
        ++p;
    if (p == end)
        return false;
    *word = p;
    while (p < end && !is_space(*p))
        ++p;
    *bytes = static_cast<u32>(p - *word);
    *at = p;
    return true;
}

bool parse_number(const char* word, u32 bytes, i32* value) noexcept {
    u32 i = 0;
    const bool negative = bytes && word[0] == '-';
    i += negative;
    if (i == bytes || bytes - i > 9)
        return false;
    i32 result = 0;
    for (; i < bytes; ++i) {
        if (word[i] < '0' || word[i] > '9')
            return false;
        result = result * 10 + (word[i] - '0');
    }
    *value = negative ? -result : result;
    return true;
}

// A percent from the resolution table, or a frame rate from the frame-rate table, as step + 1; 0 when it is
// not a step.
u8 step_of(const u8* table, u32 count, i32 value) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (table[i] == value)
            return static_cast<u8>(i + 1);
    return 0;
}

bool parse_line(Entry* entry, const char* line, u32 bytes) noexcept {
    u32 equals = 0;
    while (equals < bytes && line[equals] != '=')
        ++equals;
    if (equals == bytes)
        return true; // blank or a comment
    const char* value = line + equals + 1;
    const char* end = line + bytes;
    const auto key = [&](const char* ascii) noexcept { return word_is(line, equals, ascii); };
    if (key("name")) {
        while (value < end && is_space(*value))
            ++value;
        u32 length = static_cast<u32>(end - value);
        while (length && is_space(value[length - 1]))
            --length;
        entry->name_bytes = length < kNameBytes ? length : kNameBytes - 1;
        copy_bytes(entry->name, value, entry->name_bytes);
        return true;
    }
    const char* word = nullptr;
    u32 word_bytes = 0;
    if (key("uses")) {
        while (next_word(&value, end, &word, &word_bytes)) {
            u32 use = 0;
            if (word_is(word, word_bytes, "noise")) use = effect_use_noise;
            else if (word_is(word, word_bytes, "glyphs")) use = effect_use_glyphs;
            else if (word_is(word, word_bytes, "blue_noise")) use = effect_use_blue_noise;
            else if (word_is(word, word_bytes, "pointer")) use = effect_use_pointer;
            else if (word_is(word, word_bytes, "texture0")) use = effect_use_texture0;
            else if (word_is(word, word_bytes, "texture1")) use = effect_use_texture1;
            else return false;
            entry->uses |= use;
        }
        return true;
    }
    if (key("blend")) {
        if (!next_word(&value, end, &word, &word_bytes))
            return false;
        if (word_is(word, word_bytes, "normal")) entry->blend = EffectBlend::normal;
        else if (word_is(word, word_bytes, "screen")) entry->blend = EffectBlend::screen;
        else if (word_is(word, word_bytes, "add")) entry->blend = EffectBlend::add;
        else return false;
        return true;
    }
    i32 number = 0;
    if (key("resolution") || key("fps")) {
        const bool resolution = key("resolution");
        if (!next_word(&value, end, &word, &word_bytes) || !parse_number(word, word_bytes, &number))
            return false;
        const u8 step = resolution ? step_of(kEffectResolutionPercent, kEffectResolutionSteps, number)
                                   : step_of(kEffectFramesPerSecond, kEffectFrameRateSteps, number);
        if (!step)
            return false;
        (resolution ? entry->requested_resolution : entry->requested_frame_rate) = step;
        return true;
    }
    if (key("knobs")) {
        u32 count = 0;
        while (next_word(&value, end, &word, &word_bytes)) {
            if (count == kEffectKnobRows * 4 || !parse_number(word, word_bytes, &number))
                return false;
            entry->knobs[count / 4][count % 4] = static_cast<float>(number) * .001f;
            ++count;
        }
        return true;
    }
    return false;
}

bool parse_sidecar(Entry* entry, const char* text, u32 bytes) noexcept {
    u32 begin = 0;
    for (u32 i = 0; i <= bytes; ++i) {
        if (i < bytes && text[i] != '\n')
            continue;
        if (!parse_line(entry, text + begin, i - begin))
            return false;
        begin = i + 1;
    }
    return entry->name_bytes != 0;
}

bool name_before(const Entry& a, const Entry& b) noexcept {
    const u32 shared = a.name_bytes < b.name_bytes ? a.name_bytes : b.name_bytes;
    for (u32 i = 0; i < shared; ++i) {
        char x = a.name[i], y = b.name[i];
        x = x >= 'A' && x <= 'Z' ? static_cast<char>(x + 32) : x;
        y = y >= 'A' && y <= 'Z' ? static_cast<char>(y + 32) : y;
        if (x != y)
            return x < y;
    }
    return a.name_bytes != b.name_bytes ? a.name_bytes < b.name_bytes : a.identity < b.identity;
}

// ---- Files -----------------------------------------------------------------------------------
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

void load_library() noexcept {
    if (g.library_loaded)
        return;
    g.library_loaded = true;
    for (auto& entry : g.entries)
        if (entry.code)
            VirtualFree(entry.code, 0, MEM_RELEASE);
    reset_value(&g.entries);
    g.count = 0;
    wchar_t folder[512];
    u32 length = GetEnvironmentVariableW(L"MAICORD_EFFECT_DEV", folder, 480);
    if (!length || length >= 480) {
        if (!data_path(folder, 480, L"effects\\dev").ok())
            return;
        length = wide_length(folder, 480);
    }
    wchar_t pattern[512];
    u32 pattern_length = 0;
    pattern[0] = 0;
    if (!wide_append(pattern, 512, &pattern_length, folder) || !wide_append(pattern, 512, &pattern_length, L"\\*.effect"))
        return;
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE)
        return;
    auto* scratch = static_cast<u8*>(VirtualAlloc(nullptr, kBytecodeMaximum, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !scratch || g.count == kEffectLibraryCapacity)
            continue;
        wchar_t effect_path[512];
        u32 path_length = 0;
        effect_path[0] = 0;
        const u32 name_length = wide_length(found.cFileName, 260);
        if (!wide_append(effect_path, 512, &path_length, folder) || !wide_append(effect_path, 512, &path_length, L"\\") ||
            !wide_append(effect_path, 512, &path_length, found.cFileName) || name_length < 8)
            continue;
        Entry entry{};
        entry.blend = EffectBlend::normal;
        const u32 text_bytes = read_file(effect_path, {scratch, kSidecarMaximum});
        if (!text_bytes || !parse_sidecar(&entry, reinterpret_cast<const char*>(scratch), text_bytes)) {
            log_write(LogLevel::warning, "effects", "sidecar_rejected");
            continue;
        }
        // <name>.cso beside the sidecar.
        effect_path[path_length - 6] = L'c';
        effect_path[path_length - 5] = L's';
        effect_path[path_length - 4] = L'o';
        effect_path[path_length - 3] = 0;
        const u32 code_bytes = read_file(effect_path, {scratch, kBytecodeMaximum});
        if (!code_bytes) {
            log_write(LogLevel::warning, "effects", "bytecode_missing");
            continue;
        }
        entry.code = static_cast<u8*>(VirtualAlloc(nullptr, code_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!entry.code)
            continue;
        copy_bytes(entry.code, scratch, code_bytes);
        entry.code_bytes = code_bytes;
        entry.identity = hash64_bytes(kHash64Seed, scratch, code_bytes) | 1;
        // Sorted by name as it goes in.
        u32 at = g.count++;
        while (at && name_before(entry, g.entries[at - 1])) {
            g.entries[at] = g.entries[at - 1];
            --at;
        }
        g.entries[at] = entry;
    } while (FindNextFileW(search, &found));
    FindClose(search);
    if (scratch)
        VirtualFree(scratch, 0, MEM_RELEASE);
}

Entry* find(u64 identity) noexcept {
    for (u32 i = 0; identity && i < g.count; ++i)
        if (g.entries[i].identity == identity)
            return &g.entries[i];
    return nullptr;
}

// ---- Settings records ----------------------------------------------------------------------
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
    char text[kNameBytes + 96];
    u32 bytes = 0;
    const Entry* entry = find(identity);
    const Utf8View name = entry ? Utf8View{entry->name, entry->name_bytes} : utf8_literal("A background effect");
    for (u32 i = 0; i < name.bytes && bytes < kNameBytes; ++i)
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

} // namespace

void application_effects_synchronize() noexcept {
    if (!g_app.renderer_ready)
        return;
    load_state();
    open_marker();
    apply_verdict();
    if (!g.file.selected) {
        if (g.selected)
            renderer_clear_effect(&g_app.renderer);
        g.selected = false;
        return;
    }
    load_library();
    const Entry* entry = find(g.file.selected);
    const u64 adapter = g_app.renderer.adapter_key;
    if (!entry || refusal_index(entry->identity, adapter) >= 0) {
        // Not installed (any more), or off on this GPU: the built-in effect applies and the choice stays saved.
        renderer_clear_effect(&g_app.renderer);
        g.selected = false;
        return;
    }
    const i32 own = override_index(entry->identity);
    const EffectShaderSettings shader = own >= 0 ? g.file.overrides[own] : effect_shader_defaults(entry->identity);
    BackgroundEffectSpec spec{};
    spec.bytecode = {entry->code, entry->code_bytes};
    spec.identity = entry->identity;
    spec.uses = entry->uses;
    spec.blend = entry->blend;
    spec.requested_resolution = entry->requested_resolution;
    spec.requested_frame_rate = entry->requested_frame_rate;
    copy_bytes(spec.knobs, entry->knobs, sizeof(spec.knobs));
    g.settings = effect_resolve_settings(g.file.user, shader, entry->requested_resolution, entry->requested_frame_rate,
                                         entry->blend);
    g.selected = true;
    if ((entry->uses & effect_use_pointer) && shader.pointer && g_app.window) {
        POINT cursor{};
        if (GetCursorPos(&cursor) && ScreenToClient(g_app.window, &cursor))
            d3d11_effect_set_pointer(&g_app.renderer.effect, static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    }
    const EffectSelectResult result = renderer_select_effect(&g_app.renderer, spec, g.settings);
    if (result.state == EffectSelect::rejected) {
        log_u64(LogLevel::warning, "effects", "effect_shader_error", static_cast<u32>(result.native_error));
        notify(entry->identity, utf8_literal(" was turned off because it could not be drawn"));
        forget_selection(entry->identity);
        return;
    }
    // A refusal by the cost probe arrives as a verdict.
    apply_verdict();
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
        g.choices[kBackgroundEffectCount + i] = {g.entries[i].name, g.entries[i].name_bytes};
        if (g.entries[i].identity == g.file.selected)
            control->selected_choice = kBackgroundEffectCount + i;
    }
    control->choices = {g.choices, kBackgroundEffectCount + g.count};
    return true;
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
    const u32 index = choice - kBackgroundEffectCount;
    if (index >= g.count)
        return true;
    // Choosing an effect again is the user turning it back on after a refusal.
    forget_refusal(g.entries[index].identity, g_app.renderer.adapter_key);
    g.file.selected = g.entries[index].identity;
    const bool saved = save_state();
    application_request_frame();
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
    return index < g.count ? g.entries[index].identity : 0;
}

void application_effects_reload() noexcept {
    g.library_loaded = false;
    load_library();
}

} // namespace maicord::app_detail
