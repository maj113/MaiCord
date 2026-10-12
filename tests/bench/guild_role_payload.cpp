// Manual benchmark: built only with -DMAICORD_BUILD_BENCHMARKS=ON (cmake/benchmarks.cmake) and
// never part of the CTest suite.
#include "guild_role_payload_observer.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/trace_record.hpp"
#include "maicord/model/chat_state.hpp"
#include <intrin.h>

#include <Windows.h>

namespace maicord::guild_role_payload_bench {
namespace {
constexpr u32 kMaximumRoles = 256, kIterations = 32, kWarmups = 16, kSamples = 7, kModes = 3;
static_assert(kMaximumRoles == kGuildRoleMaximum);
const char* const kModeNames[kModes] = {"table_setup_observe", "ordinal_sweep_observe", "indexed_sweep_observe"};
const char* const kTickNames[kModes] = {"table_batch_ticks", "ordinal_batch_ticks", "indexed_batch_ticks"};
struct Fixture {
    GatewayClientEvent event;
    u8 payload[16384];
    u32 expected_offsets[kMaximumRoles];
    u32 offsets[kMaximumRoles];
    u32 prefix_bytes[2];
    u32 used;
    u64 expected_query_digest;
    u64 expected_offset_digest;
};
Fixture fixture{};
volatile u64 published_checksum = 0;

GatewayClientRoleMutation expected_role(u32 index, u16 name_bytes) noexcept {
    GatewayClientRoleMutation role{};
    role.id = 0x100000000ull + index * 17ull;
    role.permissions = 0x1020304050607080ull ^ index * 0x10101ull;
    role.position = index ^ 0xA5u;
    role.color = (0xABCDEFu + index) & 0xFFFFFFu;
    role.name_bytes = name_bytes;
    role.flags = static_cast<u8>(index & 3u);
    return role;
}
u32 make_name(u32 index, char* name) noexcept {
    if (index % 11 == 0) return 0;
    const u32 bytes = 4 + index % 23;
    u32 cursor = 0;
    if (index % 3 == 1) {
        name[cursor++] = static_cast<char>(0xE7);
        name[cursor++] = static_cast<char>(0x8C);
        name[cursor++] = static_cast<char>(0xAB);
    }
    while (cursor < bytes) {
        name[cursor] = static_cast<char>('a' + (index + cursor) % 26); ++cursor;
    }
    return bytes;
}
bool initialize() noexcept {
    auto& f = fixture;
    f = {};
    f.event.kind = GatewayClientEventKind::dispatch;
    f.event.dispatch_kind = GatewayClientDispatchKind::guild_update;
    f.event.state.id = 77; f.event.state.owner_id = 999;
    f.event.state.fields = gateway_client_state_field_id | gateway_client_state_field_name |
        gateway_client_state_field_owner_id | gateway_client_state_field_icon;
    f.event.state.name_bytes = 5; f.event.state.icon_bytes = 32;
    copy_bytes(f.payload, "Guild", 5); f.used = 5;
    for (u32 i = 0; i < 32; ++i) f.payload[f.used++] = 'a';
    for (u32 i = 0; i < kMaximumRoles; ++i) {
        char name[32]; const u32 name_bytes = make_name(i, name);
        const auto role = expected_role(i, static_cast<u16>(name_bytes));
        if (sizeof(role) + name_bytes > sizeof(f.payload) - f.used) return false;
        f.expected_offsets[i] = f.used;
        copy_bytes(f.payload + f.used, &role, sizeof(role)); f.used += sizeof(role);
        copy_bytes(f.payload + f.used, name, name_bytes); f.used += name_bytes;
        if (i == 63) f.prefix_bytes[0] = f.used;
    }
    f.prefix_bytes[1] = f.used;
    return f.used <= sizeof(f.payload);
}
ConstSlice<u8> payload() noexcept {
    return {fixture.payload, fixture.event.dispatch_payload_bytes};
}
bool role_equal(const GatewayClientRoleMutation& a, const GatewayClientRoleMutation& b) noexcept {
    return a.id == b.id && a.permissions == b.permissions && a.position == b.position && a.color == b.color &&
        a.name_bytes == b.name_bytes && a.flags == b.flags && bytes_equal(a.reserved, b.reserved, sizeof(a.reserved));
}
bool sweep(bool indexed, u64* digest) noexcept {
    auto& f = fixture;
    u64 hash = kObservationSeed;
    for (u32 query = 0; query < f.event.state.role_count; ++query) {
        const u32 index = (query * 73 + 17) & (f.event.state.role_count - 1);
        GatewayClientRoleMutation role{}; Utf8View name{};
        const bool decoded = indexed
            ? gateway_client_event_guild_role_at(f.event, payload(), f.offsets[index], &role, &name)
            : gateway_client_event_guild_role(f.event, payload(), index, &role, &name);
        if (!decoded) return false;
        hash = observe_role(hash, query, role, name);
    }
    *digest = hash;
    return true;
}
bool validate(u32 count) noexcept {
    auto& f = fixture;
    f.event.state.role_count = count;
    f.event.dispatch_payload_bytes = f.prefix_bytes[count == 64 ? 0 : 1];
    if (!gateway_client_event_guild_role_offsets(f.event, payload(), {f.offsets, count})) return false;
    u32 end = 37;
    for (u32 i = 0; i < count; ++i) {
        char generated_name[32]; const u32 bytes = make_name(i, generated_name);
        const auto expected = expected_role(i, static_cast<u16>(bytes));
        GatewayClientRoleMutation ordinal{}, indexed{}; Utf8View ordinal_name{}, indexed_name{};
        if (f.offsets[i] != f.expected_offsets[i] || f.offsets[i] != end ||
            !gateway_client_event_guild_role(f.event, payload(), i, &ordinal, &ordinal_name) ||
            !gateway_client_event_guild_role_at(f.event, payload(), f.offsets[i], &indexed, &indexed_name) ||
            !role_equal(expected, ordinal) || !role_equal(expected, indexed) ||
            ordinal_name.bytes != bytes || indexed_name.bytes != bytes ||
            !bytes_equal(ordinal_name.data, generated_name, bytes) ||
            !bytes_equal(indexed_name.data, generated_name, bytes)) return false;
        end += sizeof(GatewayClientRoleMutation) + bytes;
    }
    if (end != f.event.dispatch_payload_bytes) return false;
    u64 ordinal = 0, indexed = 0;
    if (!sweep(false, &ordinal) || !sweep(true, &indexed) || ordinal != indexed) return false;
    f.expected_query_digest = ordinal;
    f.expected_offset_digest = observe_offsets(f.event, payload(), {f.offsets, count});
    return true;
}
bool iteration(u32 mode, u64* digest) noexcept {
    auto& f = fixture;
    if (mode == 0) {
        if (!gateway_client_event_guild_role_offsets(f.event, payload(), {f.offsets, f.event.state.role_count})) return false;
        *digest = observe_offsets(f.event, payload(), {f.offsets, f.event.state.role_count});
        return *digest == f.expected_offset_digest;
    }
    return sweep(mode == 2, digest) && *digest == f.expected_query_digest;
}
bool measure(u32 mode, u64* ticks) noexcept {
    LARGE_INTEGER start{}, finish{};
    if (!QueryPerformanceCounter(&start)) return false;
    u64 checksum = kObservationSeed;
    for (u32 i = 0; i < kIterations; ++i) {
        u64 digest = 0;
        if (!iteration(mode, &digest)) return false;
        checksum = (checksum ^ digest ^ i) * 1099511628211ull;
    }
    if (!QueryPerformanceCounter(&finish) || finish.QuadPart <= start.QuadPart) return false;
    *ticks = static_cast<u64>(finish.QuadPart - start.QuadPart);
    published_checksum = published_checksum ^ checksum ^ *ticks;
    return true;
}
u64 ns_per_iteration(u64 ticks, u64 frequency) noexcept {
    return ((ticks / frequency) * 1000000000ull + (ticks % frequency) * 1000000000ull / frequency) / kIterations;
}
void emit(const TraceRecord& line) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.text, line.used, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
}
bool deadline_ok(i64 started, u64 frequency) noexcept {
    LARGE_INTEGER now{};
    return QueryPerformanceCounter(&now) && now.QuadPart >= started &&
        static_cast<u64>(now.QuadPart - started) / frequency < 45;
}
bool case_run(u32 count, u64 frequency, i64 started) noexcept {
    if (!validate(count)) return false;
    TraceRecord validation("role_payload_case");
    validation.field("roles", count); validation.field("payload_bytes", fixture.event.dispatch_payload_bytes);
    validation.field("exact_ordered_fields_names_offsets", 1);
    validation.field("query_digest", fixture.expected_query_digest); validation.field("table_digest", fixture.expected_offset_digest);
    emit(validation);
    for (u32 warmup = 0; warmup < kWarmups; ++warmup) {
        for (u32 mode = 0; mode < kModes; ++mode) {
            u64 ignored = 0; if (!iteration(mode, &ignored)) return false;
        }
    }
    u64 samples[kModes][kSamples]{};
    for (u32 sample = 0; sample < kSamples; ++sample) {
        TraceRecord row("role_payload_sample"); row.field("roles", count); row.field("sample", sample);
        for (u32 slot = 0; slot < kModes; ++slot) {
            const u32 mode = sample % 2 ? (sample + kModes - slot) % kModes : (sample + slot) % kModes;
            if (!deadline_ok(started, frequency) || !measure(mode, &samples[mode][sample])) return false;
            row.field(kModeNames[mode], ns_per_iteration(samples[mode][sample], frequency));
            row.field(kTickNames[mode], samples[mode][sample]);
        }
        row.field("unit", "ns_per_iteration"); emit(row);
    }
    for (u32 mode = 0; mode < kModes; ++mode) {
        for (u32 i = 1; i < kSamples; ++i) {
            const u64 value = samples[mode][i]; u32 at = i;
            while (at && samples[mode][at - 1] > value) { samples[mode][at] = samples[mode][at - 1]; --at; }
            samples[mode][at] = value;
        }
        TraceRecord row("role_payload_summary"); row.field("roles", count); row.field("mode", kModeNames[mode]);
        row.field("min_ns", ns_per_iteration(samples[mode][0], frequency));
        row.field("median_ns", ns_per_iteration(samples[mode][kSamples / 2], frequency));
        row.field("max_ns", ns_per_iteration(samples[mode][kSamples - 1], frequency));
        row.field("median_batch_ticks", samples[mode][kSamples / 2]);
        if (mode != 0) row.field("median_ns_per_query", ns_per_iteration(samples[mode][kSamples / 2], frequency) / count);
        emit(row);
    }
    return deadline_ok(started, frequency);
}
void metadata(u64 frequency) noexcept {
    SYSTEM_INFO system{}; GetNativeSystemInfo(&system);
    TraceRecord line("role_payload_metadata");
    line.field("msc_full_ver", _MSC_FULL_VER); line.field("msc_ver", _MSC_VER);
    line.field("x64", 1); line.field("pointer_bytes", sizeof(void*));
#if defined(_DEBUG)
    line.field("debug", 1);
#else
    line.field("debug", static_cast<u64>(0));
#endif
    line.field("qpc_frequency", frequency); line.field("iterations", kIterations);
    line.field("samples", kSamples); line.field("warmups", kWarmups);
    line.field("fixture_static_bytes", sizeof(Fixture)); line.field("table_capacity_bytes", sizeof(fixture.offsets));
    line.field("logical_processors", system.dwNumberOfProcessors);
    line.field("processor_architecture", system.wProcessorArchitecture);
    line.field("process_priority", GetPriorityClass(GetCurrentProcess()));
    line.field("observer_separate_GL_disabled_required", 1); emit(line);
    int cpu[4]{}; __cpuid(cpu, static_cast<int>(0x80000000u));
    char brand[49]{};
    if (static_cast<u32>(cpu[0]) >= 0x80000004u) {
        for (u32 leaf = 0; leaf < 3; ++leaf) {
            __cpuid(cpu, static_cast<int>(0x80000002u + leaf)); copy_bytes(brand + leaf * 16, cpu, 16);
        }
    }
    TraceRecord processor("role_payload_cpu"); processor.field("brand", brand[0] ? brand : "unavailable"); emit(processor);
    TraceRecord scope("role_payload_scope"); scope.field("work", "typed_decoders_plus_opaque_observer");
    scope.field("indexed_table_setup_in_sweep", static_cast<u64>(0));
    scope.field("client_frame_latency_measured", static_cast<u64>(0));
    scope.field("query_order", "affine_permutation_73i_plus17"); emit(scope);
}
bool run() noexcept {
    LARGE_INTEGER frequency{}, started{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        static_cast<u64>(frequency.QuadPart) > ~u64{0} / 1000000000ull ||
        !QueryPerformanceCounter(&started) || !initialize()) return false;
    metadata(static_cast<u64>(frequency.QuadPart));
    const bool valid = case_run(64, static_cast<u64>(frequency.QuadPart), started.QuadPart) &&
        case_run(256, static_cast<u64>(frequency.QuadPart), started.QuadPart);
    TraceRecord final("role_payload_result"); final.field("valid", valid ? 1u : 0u);
    final.field("published_checksum", published_checksum);
    LARGE_INTEGER finished{};
    if (QueryPerformanceCounter(&finished) && finished.QuadPart >= started.QuadPart)
        final.field("elapsed_qpc_ticks", static_cast<u64>(finished.QuadPart - started.QuadPart));
    emit(final);
    return valid;
}
} // namespace
} // namespace maicord::guild_role_payload_bench

extern "C" __declspec(noreturn) void WINAPI maicord_guild_role_payload_benchmark_entry() noexcept {
    const bool valid = maicord::guild_role_payload_bench::run();
    constexpr char pass[] = "PASS guild role payload decoder benchmark\n";
    constexpr char fail[] = "FAIL guild role payload decoder benchmark\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), valid ? pass : fail, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(valid ? 0 : 1);
}
