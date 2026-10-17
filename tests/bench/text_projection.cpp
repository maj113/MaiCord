// Manual benchmark: built only with -DMAICORD_BUILD_BENCHMARKS=ON (cmake/benchmarks.cmake) and
// never part of the CTest suite.
#include "maicord/ui/friends_scene.hpp"
#include "maicord/ui/mention_badge.hpp"
#include "maicord/ui/shell_text_presentation.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/presence.hpp"
#include "text_projection_observer.hpp"

#include <Windows.h>

namespace maicord {
namespace text_projection_bench {
#include "../text_projection_fixture.inc"

static_assert(sizeof(Fixture) < 1024 * 1024, "benchmark fixture remains bounded");
constexpr u32 kIterations = 128, kSamples = 7, kWarmups = 16, kModes = 4;
const char* const kNames[kModes] = {
    "stored_preflight", "emitter_preflight", "stored_cold_observe", "emitter_cold_observe"};
u64 expected_signature = 0, expected_observer = 0;
u32 expected_count = 0;
volatile u64 published_checksum = 0;

bool viewport(float height) noexcept {
    auto& f = fixture;
    if (!ui_scene_layout_with_metrics(&f.shell, UiShellView::friends, kFriendsShellLayoutMetrics, 1280, height))
        return false;
    const auto* guild = ui_scene_resolve(f.shell, ui_scene_find(f.shell, UiSemantic::guild_navigation));
    const auto* channels = ui_scene_resolve(f.shell, ui_scene_find(f.shell, UiSemantic::channel_navigation));
    const auto* content = ui_scene_resolve(f.shell, ui_scene_find(f.shell, UiSemantic::content));
    return guild && channels && content &&
        guild_rail_scene_update(&f.rail, &f.view, guild->rectangle, 0).ok() &&
        shell_navigation_scene_update(&f.navigation, &f.view, channels->rectangle, 0).ok() &&
        ui_friends_scene_update(&f.friends, &f.source, UiFriendsTab::pending,
            UiFriendsPhase::ready, {}, content->rectangle);
}
void write_number(const char* key, u64 value, const char* unit) noexcept {
    char digits[24]{};
    u32 count = 0;
    do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    DWORD written = 0;
    u32 bytes = 0; while (key[bytes]) ++bytes;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), key, bytes, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "=", 1, &written, nullptr);
    while (count) WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), &digits[--count], 1, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), " ", 1, &written, nullptr);
    bytes = 0; while (unit[bytes]) ++bytes;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), unit, bytes, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
}
bool validate(u32 height) noexcept {
    auto& f = fixture;
    ui_text_command_stream_reset(&f.stream);
    UiTextOutput stored{&f.stream, nullptr};
    if (!tall_projection(&stored) || !f.stream.count || f.stream.count > 1500 ||
        (height == 8192 && f.stream.count <= 1156)) return false;
    expected_count = f.stream.count;
    expected_signature = f.stream.signature;
    Observation exact{}; exact.expected = &f.stream;
    UiTextEmitter emitter{};
    UiTextOutput emitted{nullptr, &emitter};
    if (!ui_text_emitter_initialize(&emitter, consume, &exact) || !tall_projection(&emitted) ||
        exact.count != expected_count || emitter.count != expected_count ||
        emitter.signature != expected_signature || emitter.rejected) return false;
    Observation stored_observer{};
    if (!observe_stored(f.stream, &stored_observer) || stored_observer.hash != exact.hash ||
        stored_observer.count != expected_count) return false;
    expected_observer = exact.hash;
    return true;
}
bool iteration(u32 mode, u64* checksum) noexcept {
    auto& f = fixture;
    UiTextEmitter preflight{}, emitted{};
    Observation observer{};
    UiTextOutput stored{&f.stream, nullptr}, checked{nullptr, &preflight}, delivered{nullptr, &emitted};
    if (mode == 0 || mode == 2) {
        ui_text_command_stream_reset(&f.stream);
        if (!tall_projection(&stored) || f.stream.count != expected_count ||
            f.stream.signature != expected_signature || f.stream.rejected) return false;
        if (mode == 0) {
            *checksum = observe_preflight(&f.stream, nullptr);
            return *checksum == (expected_signature ^ expected_count);
        }
        if (!observe_stored(f.stream, &observer)) return false;
    } else {
        if (!ui_text_emitter_initialize(&preflight) || !tall_projection(&checked) ||
            preflight.count != expected_count || preflight.signature != expected_signature ||
            preflight.rejected) return false;
        if (mode == 1) {
            *checksum = observe_preflight(nullptr, &preflight);
            return *checksum == (expected_signature ^ expected_count);
        }
        if (!ui_text_emitter_initialize(&emitted, consume, &observer) || !tall_projection(&delivered) ||
            emitted.count != expected_count || emitted.signature != expected_signature ||
            emitted.rejected) return false;
    }
    *checksum = observer.hash;
    return observer.count == expected_count && observer.hash == expected_observer;
}
bool measure(u32 mode, u64* ticks) noexcept {
    LARGE_INTEGER start{}, finish{};
    if (!QueryPerformanceCounter(&start)) return false;
    u64 checksum = 0;
    for (u32 i = 0; i < kIterations; ++i) {
        u64 observed = 0;
        if (!iteration(mode, &observed)) return false;
        checksum ^= observed + i;
    }
    if (!QueryPerformanceCounter(&finish) || finish.QuadPart <= start.QuadPart) return false;
    *ticks = static_cast<u64>(finish.QuadPart - start.QuadPart);
    published_checksum = published_checksum ^ checksum ^ *ticks;
    return true;
}
u64 ns_per_iteration(u64 ticks, u64 frequency) noexcept {
    return ticks * 1000000000ull / (frequency * kIterations);
}
bool case_run(u32 height, u64 frequency) noexcept {
    if (!viewport(static_cast<float>(height)) || !validate(height)) return false;
    const u32 resolutions = fixture.resolutions;
    write_number("case_height", height, "DIP");
    write_number("visible_commands", expected_count, "commands");
    write_number("production_ordered_signature", expected_signature, "u64");
    write_number("observer_ordered_digest", expected_observer, "u64");
    write_number("stored_owned_numeric_bytes", fixture.stream.owned_bytes, "bytes");
    u64 samples[kModes][kSamples]{};
    for (u32 warmup = 0; warmup < kWarmups; ++warmup)
        for (u32 mode = 0; mode < kModes; ++mode) {
            u64 ignored = 0; if (!iteration(mode, &ignored)) return false;
        }
    for (u32 sample = 0; sample < kSamples; ++sample) {
        write_number("sample_index", sample, "index");
        for (u32 slot = 0; slot < kModes; ++slot) {
            const u32 mode = sample % 2 ? (sample + 4 - slot) % kModes : (sample + slot) % kModes;
            if (!measure(mode, &samples[mode][sample])) return false;
            write_number(kNames[mode], ns_per_iteration(samples[mode][sample], frequency), "ns_per_iteration");
            write_number("batch_ticks", samples[mode][sample], "QPC_ticks");
        }
    }
    for (u32 mode = 0; mode < kModes; ++mode) {
        for (u32 i = 1; i < kSamples; ++i) {
            const u64 value = samples[mode][i];
            u32 at = i;
            while (at && samples[mode][at - 1] > value) {
                samples[mode][at] = samples[mode][at - 1]; --at;
            }
            samples[mode][at] = value;
        }
        write_number("summary_mode", mode, "index");
        write_number("min", ns_per_iteration(samples[mode][0], frequency), "ns_per_iteration");
        write_number("median", ns_per_iteration(samples[mode][kSamples / 2], frequency), "ns_per_iteration");
        write_number("max", ns_per_iteration(samples[mode][kSamples - 1], frequency), "ns_per_iteration");
    }
    return fixture.resolutions == resolutions;
}
bool run() noexcept {
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 || !initialize()) return false;
    write_number("projection_only", 1, "bool");
    write_number("shaping_raster_frame_latency_measured", 0, "bool");
    write_number("compiler_msc_full_ver", _MSC_FULL_VER, "version");
    write_number("compiler_msc_ver", _MSC_VER, "version");
#if defined(_DEBUG)
    write_number("debug_build", 1, "bool");
#else
    write_number("debug_build", 0, "bool");
#endif
#if defined(_M_X64)
    write_number("target_x64", 1, "bool");
#else
    write_number("target_x64", 0, "bool");
#endif
    write_number("observer_requires_separate_GL_disabled_TU", 1, "contract");
    write_number("qpc_frequency", static_cast<u64>(frequency.QuadPart), "ticks_per_second");
    write_number("iterations_per_sample", kIterations, "iterations");
    write_number("samples_per_mode", kSamples, "samples");
    write_number("warmups_per_mode", kWarmups, "iterations");
    write_number("case_width", 1280, "DIP");
    write_number("fixture_input_rows", 192, "rows_per_source");
    write_number("fixture_static_bytes", sizeof(Fixture), "bytes");
    write_number("stored_command_capacity", 1500, "commands");
    write_number("stored_command_bytes", sizeof(fixture.commands), "bytes");
    write_number("stored_owned_text_capacity", sizeof(fixture.text), "bytes");
    write_number("emitter_maximum_commands", kUiTextEmitterMaximumCommands, "commands");
    write_number("stored_cold_projection_passes", 1, "passes_per_iteration");
    write_number("emitter_cold_projection_passes", 2, "passes_per_iteration");
    const bool valid = case_run(900, static_cast<u64>(frequency.QuadPart)) &&
        case_run(8192, static_cast<u64>(frequency.QuadPart));
    write_number("published_checksum", published_checksum, "u64");
    return valid;
}
} // namespace text_projection_bench
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_text_projection_benchmark_entry() noexcept {
    using namespace maicord::text_projection_bench;
    const bool valid = run();
    ui_friends_scene_release(&fixture.friends);
    ui_scene_release(&fixture.shell);
    constexpr char pass[] = "PASS projection-only text emitter benchmark\n";
    constexpr char fail[] = "FAIL projection-only text emitter benchmark\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), valid ? pass : fail, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(valid ? 0 : 1);
}

