// Manual benchmark: built only with -DMAICORD_BUILD_BENCHMARKS=ON (cmake/benchmarks.cmake) and
// never part of the CTest suite.
#include "channel_name_preflight_observer.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/trace_record.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::channel_name_preflight_bench {
namespace {
constexpr u32 kGuilds = 256, kChannels = 4096, kRoles = 8192, kNameBank = 32768;
constexpr u32 kModes = 4, kIterations = 512, kWarmups = 16, kSamples = 7;
const char* const kModesNames[kModes] = {"empty", "identical", "append_fit", "compaction_required"};
struct Fixture {
    ChatStateStore state;
    GuildRecord guilds[kGuilds];
    ChannelRecord channels[kChannels];
    RoleRecord roles[kRoles];
    PermissionOverwriteRecord overwrites[1];
    u16 guild_order[kGuilds], channel_order[kChannels];
    u8 names[kNameBank * 2];
};
static_assert(sizeof(Fixture) < 1024 * 1024);
Fixture source{}, oracle{};
volatile u64 published_checksum = 0;
u32 live_name_bytes = 0;

void emit(const TraceRecord& row) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), row.text, row.used, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
}
bool initialize(u32 guilds, u32 channels, u32 roles) noexcept {
    source = {};
    auto& s = source.state;
    if (!guilds || !channels || guilds > kGuilds || channels > kChannels || roles > kRoles ||
        !chat_state_store_initialize(&s, {source.guilds, kGuilds}, {source.guild_order, kGuilds},
            {source.channels, kChannels}, {source.channel_order, kChannels}, {source.names, sizeof(source.names)}) ||
        !chat_state_store_bind_permissions(&s, {source.roles, kRoles}, {source.overwrites, 1})) return false;
    for (u32 i = 0; i < guilds; ++i)
        if (!chat_state_store_upsert_guild(&s, {100ull + i, utf8_literal("G"),
            GuildMutationKind::create, 999}).ok()) return false;
    for (u32 i = 0; i < channels; ++i) {
        ChannelInput input{};
        input.id = 5000ull + i; input.guild_id = 100ull + i % guilds;
        input.name = i == 0 ? utf8_literal("Target") : utf8_literal("C"); input.name_known = true;
        if (!chat_state_store_upsert_channel(&s, input).ok()) return false;
    }
    // Ascending guild/role order avoids measuring fixture insertion shifts.
    for (u32 guild = 0; guild < guilds; ++guild)
        for (u32 i = guild; i < roles; i += guilds)
            if (!chat_state_store_upsert_role(&s, {800000ull + i, 100ull + guild, 0, utf8_literal("R")})) return false;
    live_name_bytes = guilds + channels + 5 + roles;
    return s.guild_count == guilds && s.channel_count == channels && s.role_count == roles &&
        s.name_used == live_name_bytes && s.name_wasted == 0 && s.active_name_bank == 0 && s.compactions == 0;
}
ChannelInput input_for(u32 mode) noexcept {
    ChannelInput input{};
    input.id = 5000; input.guild_id = 100; input.name_known = true;
    input.name = mode == 0 ? Utf8View{} : mode == 1 ? utf8_literal("Target") : utf8_literal("Renamed!");
    return input;
}
void clone_oracle() noexcept {
    copy_bytes(&oracle, &source, sizeof(source));
    auto& s = oracle.state;
    s.guilds = oracle.guilds; s.channels = oracle.channels; s.roles = oracle.roles;
    s.overwrites = oracle.overwrites; s.guild_order = oracle.guild_order;
    s.channel_order = oracle.channel_order; s.names = oracle.names;
}
u64 signature() noexcept {
    // Full const-store and both-bank immutability check outside timing. Reading
    // representation bytes here observes unchanged storage, not struct equality
    // across independently constructed objects.
    const auto* bytes = reinterpret_cast<const u8*>(&source);
    u64 digest = kObservationSeed;
    for (usize i = 0; i < sizeof(source); ++i) digest = (digest ^ bytes[i]) * 1099511628211ull;
    return digest;
}
bool validate(u32 mode, u64* immutable_signature) noexcept {
    auto& s = source.state;
    s.name_used = mode == 3 ? kNameBank - 4 : live_name_bytes;
    s.name_wasted = s.name_used - live_name_bytes;
    const auto input = input_for(mode);
    *immutable_signature = signature();
    if (chat_state_store_preflight_channel(s, input) != ChatStateStoreError::none ||
        signature() != *immutable_signature) return false;
    clone_oracle();
    const u32 old_mutations = oracle.state.channel_mutations;
    const auto applied = chat_state_store_upsert_channel(&oracle.state, input);
    const auto* target = chat_state_store_find_channel(oracle.state, input.id);
    if (!applied.ok() || applied.inserted || !target || target->guild_id != 100 ||
        oracle.state.channel_count != s.channel_count || oracle.state.role_count != s.role_count ||
        oracle.state.channel_mutations != old_mutations + 1 ||
        !utf8_equal(chat_state_store_channel_name(oracle.state, *target), mode < 2 ? utf8_literal("Target") : utf8_literal("Renamed!")))
        return false;
    const u32 expected_offset = mode < 2 ? s.guild_count : mode == 2 ? live_name_bytes : kNameBank + live_name_bytes;
    const u32 expected_used = mode < 2 ? live_name_bytes : live_name_bytes + 8;
    if (target->name_offset != expected_offset || target->name_bytes != (mode < 2 ? u16{6} : u16{8}) ||
        oracle.state.name_used != expected_used || oracle.state.name_wasted != (mode < 2 ? 0u : 6u) ||
        oracle.state.active_name_bank != (mode == 3 ? 1 : 0) || oracle.state.compactions != (mode == 3 ? 1u : 0u)) return false;
    // Namespaces survive actual allocation/compaction, independently of the
    // preflight result. Check every unrelated live label and role identity.
    for (u32 i = 0; i < s.guild_count; ++i)
        if (!utf8_equal(chat_state_store_guild_name(oracle.state, oracle.guilds[i]), utf8_literal("G"))) return false;
    for (u32 i = 1; i < s.channel_count; ++i)
        if (oracle.channels[i].id != 5000ull + i ||
            !utf8_equal(chat_state_store_channel_name(oracle.state, oracle.channels[i]), utf8_literal("C"))) return false;
    for (u32 i = 0; i < s.role_count; ++i)
        if (oracle.roles[i].id != source.roles[i].id || oracle.roles[i].guild_id != source.roles[i].guild_id ||
            !utf8_equal(chat_state_store_role_name(oracle.state, oracle.roles[i]), utf8_literal("R"))) return false;
    return signature() == *immutable_signature;
}
u64 expected_observation(const ChannelInput& input, u32 ordinal) noexcept {
    return (kObservationSeed ^ ordinal ^ input.id ^ (static_cast<u64>(source.state.channel_count) << 32) ^
        source.state.role_count ^ input.name.bytes ^ source.state.name_used) * 1099511628211ull;
}
bool iteration(u32 mode, u32 ordinal, u64* digest) noexcept {
    const auto input = input_for(mode);
    *digest = observe_preflight(source.state, input, ordinal);
    return *digest == expected_observation(input, ordinal);
}
bool deadline_ok(i64 start, u64 frequency) noexcept {
    LARGE_INTEGER now{};
    return QueryPerformanceCounter(&now) && now.QuadPart >= start &&
        static_cast<u64>(now.QuadPart - start) / frequency < 45;
}
bool measure(u32 mode, u64* ticks) noexcept {
    LARGE_INTEGER start{}, finish{};
    if (!QueryPerformanceCounter(&start)) return false;
    u64 checksum = kObservationSeed;
    for (u32 i = 0; i < kIterations; ++i) {
        u64 value = 0; if (!iteration(mode, i, &value)) return false;
        checksum = (checksum ^ value) * 1099511628211ull;
    }
    if (!QueryPerformanceCounter(&finish) || finish.QuadPart <= start.QuadPart) return false;
    *ticks = static_cast<u64>(finish.QuadPart - start.QuadPart);
    published_checksum = published_checksum ^ checksum ^ *ticks;
    return true;
}
u64 nanoseconds(u64 ticks, u64 frequency) noexcept {
    return ((ticks / frequency) * 1000000000ull + (ticks % frequency) * 1000000000ull / frequency) / kIterations;
}
bool case_run(u32 guilds, u32 channels, u32 roles, u64 frequency, i64 started) noexcept {
    if (!initialize(guilds, channels, roles)) return false;
    TraceRecord occupancy("channel_preflight_case");
    occupancy.field("guilds", guilds); occupancy.field("channels", channels); occupancy.field("roles", roles);
    occupancy.field("unrelated_channels", channels - 1); occupancy.field("live_name_bytes", live_name_bytes);
    occupancy.field("target_always_resident", 1); occupancy.field("actual_allocation_oracle", 1); emit(occupancy);
    for (u32 mode = 0; mode < kModes; ++mode) {
        u64 unchanged = 0;
        if (!validate(mode, &unchanged)) return false;
        for (u32 i = 0; i < kWarmups; ++i) { u64 ignored = 0; if (!iteration(mode, i, &ignored)) return false; }
    }
    u64 samples[kModes][kSamples]{};
    for (u32 sample = 0; sample < kSamples; ++sample)
        for (u32 slot = 0; slot < kModes; ++slot) {
            const u32 mode = sample % 2 ? (sample + kModes - slot) % kModes : (sample + slot) % kModes;
            u64 unchanged = 0;
            if (!deadline_ok(started, frequency) || !validate(mode, &unchanged) ||
                !measure(mode, &samples[mode][sample]) || signature() != unchanged) return false;
            TraceRecord row("channel_preflight_sample");
            row.field("channels", channels); row.field("roles", roles); row.field("sample", sample);
            row.field("mode", kModesNames[mode]); row.field("batch_ticks", samples[mode][sample]);
            row.field("ns_per_call", nanoseconds(samples[mode][sample], frequency)); emit(row);
        }
    for (u32 mode = 0; mode < kModes; ++mode) {
        for (u32 i = 1; i < kSamples; ++i) {
            const u64 value = samples[mode][i]; u32 at = i;
            while (at && samples[mode][at - 1] > value) { samples[mode][at] = samples[mode][at - 1]; --at; }
            samples[mode][at] = value;
        }
        TraceRecord row("channel_preflight_summary");
        row.field("channels", channels); row.field("roles", roles); row.field("mode", kModesNames[mode]);
        row.field("min_ns", nanoseconds(samples[mode][0], frequency));
        row.field("median_ns", nanoseconds(samples[mode][kSamples / 2], frequency));
        row.field("max_ns", nanoseconds(samples[mode][kSamples - 1], frequency));
        row.field("median_batch_ticks", samples[mode][kSamples / 2]); emit(row);
    }
    return deadline_ok(started, frequency);
}
bool run() noexcept {
    LARGE_INTEGER frequency{}, started{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        static_cast<u64>(frequency.QuadPart) > ~u64{0} / 1000000000ull || !QueryPerformanceCounter(&started)) return false;
    TraceRecord metadata("channel_preflight_metadata");
    metadata.field("msc_full_ver", _MSC_FULL_VER); metadata.field("msc_ver", _MSC_VER);
    metadata.field("qpc_frequency", static_cast<u64>(frequency.QuadPart)); metadata.field("iterations", kIterations);
    metadata.field("warmups", kWarmups); metadata.field("samples", kSamples);
    metadata.field("fixture_bytes_each", sizeof(Fixture)); metadata.field("fixture_count", 2);
    metadata.field("observer_separate_GL_disabled_required", 1); metadata.field("timed_work", "production_API_plus_opaque_boundary");
    metadata.field("app_frame_latency_measured", static_cast<u64>(0)); metadata.field("batch", "retained_hot_input"); emit(metadata);
    const u64 qpc = static_cast<u64>(frequency.QuadPart);
    const bool valid = case_run(1, 1, 0, qpc, started.QuadPart) && case_run(16, 256, 512, qpc, started.QuadPart) &&
        case_run(256, 4096, 8192, qpc, started.QuadPart);
    TraceRecord result("channel_preflight_result"); result.field("valid", valid ? 1u : 0u);
    result.field("published_checksum", published_checksum); emit(result);
    return valid;
}
}
}

extern "C" __declspec(noreturn) void WINAPI maicord_channel_name_preflight_benchmark_entry() noexcept {
    const bool valid = maicord::channel_name_preflight_bench::run();
    constexpr char pass[] = "PASS channel name preflight benchmark\n";
    constexpr char fail[] = "FAIL channel name preflight benchmark\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), valid ? pass : fail, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(valid ? 0 : 1);
}
