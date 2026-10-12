// Manual benchmark: built only with -DMAICORD_BUILD_BENCHMARKS=ON (cmake/benchmarks.cmake) and
// never part of the CTest suite.
#include "guild_channel_validation_observer.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/trace_record.hpp"
#include <intrin.h>

#include <Windows.h>

namespace maicord::guild_channel_validation_bench {
namespace {
constexpr u32 kChannels = 1000, kIterations = 32, kWarmups = 16, kSamples = 7, kModes = 4;
constexpr u64 kGuild = 77, kSelf = 42, kOwner = 999;
constexpr u64 kPermissions = kPermissionViewChannel | kPermissionReadMessageHistory |
    kPermissionSendMessages;
constexpr const char* kModesText[kModes] = {"ascending", "decreasing", "permuted", "late_duplicate"};
static_assert(sizeof(GatewayClientStateMutation) == 168);
struct Model {
    ChatStateStore state;
    GuildRecord guilds[1];
    ChannelRecord channels[kChannels];
    RoleRecord roles[1];
    PermissionOverwriteRecord overwrites[kChannels];
    u16 guild_order[1], channel_order[kChannels];
    u8 names[32768];
};
struct Workload {
    GatewayClientEvent event;
    u8 payload[128 * 1024];
    u32 count, mode, used;
    bool named;
    u64 expected_digest;
};
Model actual{}, expected{};
Workload workload{};
volatile u64 checksum = 0;

bool reset(Model* model) noexcept {
    clear_bytes(model, sizeof(*model));
    if (!chat_state_store_initialize(&model->state, {model->guilds, 1}, {model->guild_order, 1},
        {model->channels, kChannels}, {model->channel_order, kChannels}, {model->names, sizeof(model->names)}) ||
        !chat_state_store_bind_permissions(&model->state, {model->roles, 1}, {model->overwrites, kChannels})) return false;
    model->state.self_user_id = kSelf;
    return true;
}
u32 ordinal(u32 index, u32 mode, u32 count) noexcept {
    if (mode == 1) return count - 1 - index;
    // Both tested sizes are coprime with 37: a complete bounded permutation.
    if (mode == 2) return (index * 37 + 19) % count;
    if (mode == 3 && index + 1 == count) return count / 3;
    return index;
}
GatewayClientStateMutation channel_at(u32 index) noexcept {
    GatewayClientStateMutation channel{};
    channel.id = 10000 + ordinal(index, workload.mode, workload.count);
    channel.guild_id = kGuild;
    channel.type = 0;
    channel.fields = gateway_client_state_field_id | gateway_client_state_field_guild_id |
        gateway_client_state_field_type | gateway_client_state_field_name |
        gateway_client_state_field_permission_overwrites | gateway_client_state_field_last_message_id;
    channel.position = index;
    channel.last_message_id = 200000 + index;
    channel.name_bytes = workload.named ? 8 : 0;
    channel.overwrite_count = workload.named ? 1 : 0;
    channel.member_list_id_known = workload.named;
    channel.member_list_id = workload.named ? 300000 + index : 0;
    return channel;
}
void name_at(u32 index, char* output) noexcept {
    for (u32 byte = 0; byte < 8; ++byte) output[byte] = static_cast<char>('a' + (index + byte * 3) % 26);
}
bool append(const void* data, u32 bytes) noexcept {
    if (bytes > sizeof(workload.payload) - workload.used) return false;
    copy_bytes(workload.payload + workload.used, data, bytes); workload.used += bytes;
    return true;
}
bool build(u32 count, u32 mode, bool named) noexcept {
    clear_bytes(&workload, sizeof(workload));
    workload.count = count; workload.mode = mode; workload.named = named;
    auto& event = workload.event;
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::guild_create;
    event.state.id = kGuild; event.state.owner_id = kOwner;
    event.state.fields = gateway_client_state_field_id | gateway_client_state_field_name |
        gateway_client_state_field_owner_id;
    event.state.name_bytes = 5; event.state.role_count = 1;
    event.state.record_count = count; event.state.overwrite_count = named ? count : 0;
    GatewayClientRoleMutation role{}; role.id = kGuild; role.permissions = kPermissions;
    if (!append("Bench", 5) || !append(&role, sizeof(role))) return false;
    for (u32 i = 0; i < count; ++i) {
        const auto channel = channel_at(i);
        char name[8]; name_at(i, name);
        GatewayClientPermissionOverwriteMutation overwrite{};
        overwrite.id = kGuild; overwrite.allow = kPermissionAddReactions;
        if (!append(&channel, sizeof(channel)) || !append(name, channel.name_bytes) ||
            (named && !append(&overwrite, sizeof(overwrite)))) return false;
    }
    event.dispatch_payload_bytes = workload.used;
    return workload.used <= sizeof(workload.payload);
}
ChatStateApplyResult apply() noexcept {
    return chat_state_apply_gateway_event(&actual.state, workload.event,
        {workload.payload, workload.used}, {});
}
bool build_expected() noexcept {
    if (!reset(&expected)) return false;
    auto& state = expected.state;
    if (!chat_state_store_upsert_guild(&state, {kGuild, utf8_literal("Bench"),
            GuildMutationKind::create, kOwner}).ok() ||
        !chat_state_store_upsert_role(&state, {kGuild, kGuild, kPermissions, {}})) return false;
    for (u32 i = 0; i < workload.count; ++i) {
        const auto wire = channel_at(i); char name[8]; name_at(i, name);
        const ChannelInput input{wire.id, kGuild, 0, {name, wire.name_bytes}, wire.position, 0, 0,
            wire.last_message_id, true, wire.member_list_id, wire.member_list_id_known, true};
        if (!chat_state_store_upsert_channel(&state, input).ok() ||
            (workload.named && !chat_state_store_append_channel_overwrite(&state, wire.id,
                {kGuild, kPermissionAddReactions, 0, 0}))) return false;
    }
    return chat_state_store_set_self_roles(&state, kGuild, kOwner, kSelf, {}) &&
        (chat_state_store_recompute_guild_permissions(&state, kGuild), true);
}
bool same_model() noexcept {
    // All storage starts byte-zeroed and both constructions have identical
    // packet-order allocations. Compare complete records and both name banks,
    // not a sampled digest or only the number of rows.
    const auto& a = actual.state; const auto& b = expected.state;
    return a.guild_count == b.guild_count && a.channel_count == b.channel_count &&
        a.role_count == b.role_count && a.overwrite_count == b.overwrite_count &&
        a.name_used == b.name_used && a.name_wasted == b.name_wasted &&
        a.selected_guild_id == b.selected_guild_id && a.selected_channel_id == b.selected_channel_id &&
        a.selection_revision == b.selection_revision && a.home_selected == b.home_selected &&
        bytes_equal(actual.guilds, expected.guilds, sizeof(actual.guilds)) &&
        bytes_equal(actual.channels, expected.channels, sizeof(actual.channels)) &&
        bytes_equal(actual.roles, expected.roles, sizeof(actual.roles)) &&
        bytes_equal(actual.overwrites, expected.overwrites, sizeof(actual.overwrites)) &&
        bytes_equal(actual.guild_order, expected.guild_order, sizeof(actual.guild_order)) &&
        bytes_equal(actual.channel_order, expected.channel_order, sizeof(actual.channel_order)) &&
        bytes_equal(actual.names, expected.names, sizeof(actual.names));
}
bool validate() noexcept {
    if (!reset(&actual)) return false;
    if (workload.mode == 3) {
        // Exact full-state snapshot includes untouched capacity and metadata.
        copy_bytes(&expected, &actual, sizeof(actual));
        const auto result = apply();
        if (result.error != ChatStateApplyError::invalid_payload || result.store_error != ChatStateStoreError::none ||
            result.guilds || result.channels || result.roles || result.overwrites || result.emojis || result.selection_changed ||
            !bytes_equal(&actual, &expected, sizeof(actual))) return false;
        workload.expected_digest = observe(actual.state, result);
        return true;
    }
    if (!build_expected()) return false;
    const auto result = apply();
    if (!result.ok() || result.store_error != ChatStateStoreError::none || result.guilds != 1 ||
        result.channels != workload.count || result.roles != 1 || result.overwrites != (workload.named ? workload.count : 0) ||
        result.emojis || result.selection_changed != (actual.state.selection_revision != 0) || !same_model()) return false;
    workload.expected_digest = observe(actual.state, result);
    return true;
}
bool deadline_ok(i64 start, u64 frequency) noexcept {
    LARGE_INTEGER now{};
    return QueryPerformanceCounter(&now) && now.QuadPart >= start &&
        static_cast<u64>(now.QuadPart - start) / frequency < 45;
}
bool iteration(u64* ticks, i64 started, u64 frequency) noexcept {
    // Initialization and all exact expected-output checking are outside QPC.
    if (!deadline_ok(started, frequency) || !reset(&actual)) return false;
    LARGE_INTEGER start{}, finish{};
    if (!QueryPerformanceCounter(&start)) return false;
    const auto result = apply();
    const u64 digest = observe(actual.state, result);
    if (!QueryPerformanceCounter(&finish) || finish.QuadPart <= start.QuadPart || digest != workload.expected_digest) return false;
    *ticks = static_cast<u64>(finish.QuadPart - start.QuadPart);
    checksum = (checksum ^ digest ^ *ticks) * 1099511628211ull;
    return result.error == (workload.mode == 3 ? ChatStateApplyError::invalid_payload : ChatStateApplyError::none);
}
u64 ns(u64 ticks, u64 frequency) noexcept {
    return ((ticks / frequency) * 1000000000ull + (ticks % frequency) * 1000000000ull / frequency) / kIterations;
}
void emit(const TraceRecord& line) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.text, line.used, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
}
bool case_run(u32 count, bool named, i64 started, u64 frequency) noexcept {
    u64 samples[kModes][kSamples]{};
    for (u32 mode = 0; mode < kModes; ++mode) {
        if (!build(count, mode, named) || !validate()) return false;
        TraceRecord line("guild_channel_case"); line.field("channels", count); line.field("mode", kModesText[mode]);
        line.field("named_overwrite_rows", named ? 1u : 0u); line.field("payload_bytes", workload.used);
        line.field("exact_complete_untimed_validation", 1); emit(line);
        for (u32 warmup = 0; warmup < kWarmups; ++warmup) {
            u64 ignored = 0; if (!iteration(&ignored, started, frequency)) return false;
        }
    }
    for (u32 sample = 0; sample < kSamples; ++sample) {
        for (u32 slot = 0; slot < kModes; ++slot) {
            const u32 mode = sample % 2 ? (sample + kModes - slot) % kModes : (sample + slot) % kModes;
            if (!build(count, mode, named) || !validate()) return false;
            u64 ticks = 0;
            for (u32 rep = 0; rep < kIterations; ++rep) {
                u64 elapsed = 0; if (!iteration(&elapsed, started, frequency)) return false; ticks += elapsed;
            }
            samples[mode][sample] = ticks;
            TraceRecord line("guild_channel_sample"); line.field("channels", count); line.field("mode", kModesText[mode]);
            line.field("sample", sample); line.field("summed_call_ticks", ticks); line.field("ns_per_call", ns(ticks, frequency)); emit(line);
        }
    }
    for (u32 mode = 0; mode < kModes; ++mode) {
        for (u32 i = 1; i < kSamples; ++i) {
            const u64 value = samples[mode][i]; u32 at = i;
            while (at && samples[mode][at - 1] > value) { samples[mode][at] = samples[mode][at - 1]; --at; }
            samples[mode][at] = value;
        }
        TraceRecord line("guild_channel_summary"); line.field("channels", count); line.field("mode", kModesText[mode]);
        line.field("min_ns", ns(samples[mode][0], frequency)); line.field("median_ns", ns(samples[mode][kSamples / 2], frequency));
        line.field("max_ns", ns(samples[mode][kSamples - 1], frequency)); emit(line);
    }
    return deadline_ok(started, frequency);
}
bool run() noexcept {
    LARGE_INTEGER frequency{}, started{}, finished{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        static_cast<u64>(frequency.QuadPart) > ~u64{0} / 1000000000ull || !QueryPerformanceCounter(&started)) return false;
    TraceRecord metadata("guild_channel_metadata"); metadata.field("msc_full_ver", _MSC_FULL_VER);
    metadata.field("qpc_frequency", frequency.QuadPart); metadata.field("repetitions", kIterations);
    metadata.field("samples", kSamples); metadata.field("warmups", kWarmups);
    metadata.field("fixture_static_bytes", sizeof(actual) + sizeof(expected) + sizeof(workload));
    metadata.field("scope", "state_apply_plus_opaque_observer_individual_QPC_intervals");
    metadata.field("setup_and_exact_validation_timed", static_cast<u64>(0));
    metadata.field("frame_latency_measured", static_cast<u64>(0)); metadata.field("observer_GL_disabled_required", 1); emit(metadata);
    const bool valid = case_run(64, true, started.QuadPart, frequency.QuadPart) &&
        case_run(kChannels, false, started.QuadPart, frequency.QuadPart);
    TraceRecord result("guild_channel_result"); result.field("valid", valid ? 1u : 0u); result.field("checksum", checksum);
    if (QueryPerformanceCounter(&finished) && finished.QuadPart >= started.QuadPart)
        result.field("elapsed_ticks", static_cast<u64>(finished.QuadPart - started.QuadPart));
    emit(result); return valid;
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_guild_channel_validation_benchmark_entry() noexcept {
    const bool valid = maicord::guild_channel_validation_bench::run();
    ExitProcess(valid ? 0 : 1);
}
