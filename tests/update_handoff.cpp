#include "test_output.hpp"
// Actual shipping primitive compiled again with test-only renamed exports.
// The fixture launcher is internal in this TU; production has no path override.
#define update_handoff_launch fixture_handoff_launch
#define update_handoff_wait fixture_handoff_wait
#define update_handoff_release fixture_handoff_release
#define update_handoff_child_open fixture_handoff_child_open
#define update_handoff_child_acknowledge fixture_handoff_child_acknowledge
#define update_handoff_child_release fixture_handoff_child_release
#include "maicord/app/update_handoff.hpp"

#include <Windows.h>
namespace maicord::handoff_fault_test {
bool fail_close{}, fail_unmap{}, fail_launch{}, fail_signal{};
BOOL WINAPI close(HANDLE h) noexcept {
    if (fail_close) { fail_close = false; SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    return CloseHandle(h);
}
BOOL WINAPI unmap(const void* p) noexcept {
    if (fail_unmap) { fail_unmap = false; SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    return UnmapViewOfFile(p);
}
BOOL WINAPI signal(HANDLE h) noexcept {
    if (fail_signal) { fail_signal = false; SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    return SetEvent(h);
}
BOOL WINAPI launch(const wchar_t* app, wchar_t* command, SECURITY_ATTRIBUTES* process_security,
    SECURITY_ATTRIBUTES* thread_security, BOOL inherit, DWORD flags, void* environment,
    const wchar_t* directory, STARTUPINFOW* startup, PROCESS_INFORMATION* output) noexcept {
    if (fail_launch) { fail_launch = false; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return CreateProcessW(app, command, process_security, thread_security, inherit, flags, environment, directory, startup, output);
}
}
#define CloseHandle maicord::handoff_fault_test::close
#define UnmapViewOfFile maicord::handoff_fault_test::unmap
#define CreateProcessW maicord::handoff_fault_test::launch
#define SetEvent maicord::handoff_fault_test::signal
#include "../src/app/update_handoff.cpp"
#undef CloseHandle
#undef UnmapViewOfFile
#undef CreateProcessW
#undef SetEvent
#undef update_handoff_launch
#undef update_handoff_wait
#undef update_handoff_release
#undef update_handoff_child_open
#undef update_handoff_child_acknowledge
#undef update_handoff_child_release

namespace maicord {
namespace {
void measure(const char* label, u64 value) noexcept {
    test_write_metric(label, value);
}
UpdateHandoffIdentity fixture_identity(u8 scenario) noexcept {
    UpdateHandoffIdentity id{}; id.installation.bytes[0] = 0x65; id.transaction.bytes[0] = scenario; id.document_sha256[0] = 0x91; return id;
}
u32 fixture_length(const wchar_t* value) noexcept { u32 n = 0; while (value[n]) ++n; return n; }
void fixture_event_name(const UpdateHandoffIdentity& id, wchar_t* name) noexcept {
    constexpr wchar_t prefix[] = L"Local\\MaiCord-Handoff-Fixture-";
    constexpr wchar_t digits[] = L"0123456789abcdef"; u32 n = 0;
    for (u32 i = 0; i < sizeof(prefix) / sizeof(wchar_t) - 1; ++i) name[n++] = prefix[i];
    for (auto byte : id.installation.bytes) { name[n++] = digits[byte >> 4]; name[n++] = digits[byte & 15]; }
    name[n] = 0;
}
bool finish_peer(UpdateHandoff* owner, u8 scenario) noexcept {
    DWORD code = ~DWORD{0};
    const DWORD waited = owner->process ? WaitForSingleObject(owner->process, 3000) : WAIT_FAILED;
    const DWORD wait_error = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    const BOOL got_code = owner->process && GetExitCodeProcess(owner->process, &code);
    if (waited != WAIT_OBJECT_0 || !got_code || code) {
        measure("DIAG update_handoff_peer_scenario=", scenario);
        measure("DIAG update_handoff_peer_wait_result=", waited);
        measure("DIAG update_handoff_peer_wait_native=", wait_error);
        measure("DIAG update_handoff_peer_exit_code=", code);
    }
    return waited == WAIT_OBJECT_0 && got_code && !code;
}
bool native_initialization_control(const wchar_t* peer, UpdateHandoff* returned) noexcept {
    // Plain Win32 control: no handoff launcher, mapping, event, duplicated
    // parent, attribute list or inherited handle. Only the known fixture runs.
    wchar_t command[kUpdateHandoffCommandCharacters]{}; u32 at = 0;
    command[at++] = L'"';
    for (u32 i = 0; peer[i]; ++i) command[at++] = peer[i];
    command[at++] = L'"';
    constexpr wchar_t mode[] = L" --maicord-handoff-native-init-control-v1";
    for (u32 i = 0; i < sizeof(mode) / sizeof(wchar_t); ++i) command[at++] = mode[i];
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(peer, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process);
    returned->process = process.hProcess; returned->thread = process.hThread;
    const bool finished = started && finish_peer(returned, 0);
    // Caller can retry release if a close fails; no returned owner is discarded.
    return fixture_handoff_release(returned).ok() && finished;
}
}
void test_update_handoff(void (*check)(bool, const char*) noexcept) noexcept {
    DWORD handles_before = 0; const bool counted_before = GetProcessHandleCount(GetCurrentProcess(), &handles_before) != FALSE;
    measure("MEASURE update_handoff_owner_bytes=", sizeof(UpdateHandoff));
    measure("MEASURE update_handoff_child_bytes=", sizeof(UpdateHandoffChild));
    measure("MEASURE update_handoff_ack_mapping_logical_bytes=", kUpdateHandoffAcknowledgementBytes);
    UpdateHandoff empty_owner{}; UpdateGenerationTransaction transaction{}; UpdateGenerationAuthenticationContext authentication{}; UpdatePair pair{};
    check(fixture_handoff_launch(nullptr, &authentication, pair, false, nullptr, &empty_owner).error == UpdateHandoffError::invalid_input &&
        empty(empty_owner), "handoff: null production transaction cannot launch");
    pair.files[0] = reinterpret_cast<HANDLE>(4); pair.files[1] = reinterpret_cast<HANDLE>(8);
    check(fixture_handoff_launch(&transaction, &authentication, pair, false, nullptr, &empty_owner).error == UpdateHandoffError::enrollment &&
        empty(empty_owner), "handoff: trusted caller enrollment is required before process/channel creation");
    check(fixture_handoff_wait(&empty_owner, 1).error == UpdateHandoffError::invalid_input &&
        fixture_handoff_release(&empty_owner).ok(), "handoff: launch success is required before native wait; empty cleanup is safe");
    constexpr wchar_t bare[] = L"MaiCord.exe --maicord-update-child-v1";
    UpdateHandoffChild child{}; const auto identity = fixture_identity(1);
    check(fixture_handoff_child_open({bare, sizeof(bare) / sizeof(wchar_t) - 1}, identity, false, &child).error == UpdateHandoffError::enrollment &&
        !child.parent && !child.view, "handoff: command-line handles do not authorize unenrolled child");
    check(fixture_handoff_child_open({bare, sizeof(bare) / sizeof(wchar_t) - 1}, identity, true, &child).error == UpdateHandoffError::acknowledgement,
        "handoff: truncated fixed protocol fails closed before adopting owners");
#if defined(MAICORD_UPDATE_HANDOFF_TEST_PEER)
    constexpr wchar_t configured_peer[] = MAICORD_UPDATE_HANDOFF_TEST_PEER;
    static_assert(sizeof(configured_peer) / sizeof(wchar_t) <= kUpdateGenerationMaximumRootCharacters + 96);
    wchar_t peer[sizeof(configured_peer) / sizeof(wchar_t)]{};
    for (u32 i = 0; i < sizeof(configured_peer) / sizeof(wchar_t); ++i)
        peer[i] = configured_peer[i] == L'/' ? L'\\' : configured_peer[i];
    DWORD control_first = 0, control_second = 0; UpdateHandoff control_owner{};
    const bool first_control = native_initialization_control(peer, &control_owner);
    check(first_control, "handoff: plain no-inheritance native initialization control exits and closes both returned owners");
    if (!first_control) fixture_handoff_release(&control_owner);
    const bool first_count = GetProcessHandleCount(GetCurrentProcess(), &control_first) != FALSE;
    const bool second_control = native_initialization_control(peer, &control_owner);
    check(second_control, "handoff: repeated plain native initialization control exits and closes both returned owners");
    if (!second_control) fixture_handoff_release(&control_owner);
    const bool second_count = GetProcessHandleCount(GetCurrentProcess(), &control_second) != FALSE;
    check(first_control && second_control && first_count && second_count && control_first == control_second,
        "handoff: native control repeats restore strict initialized process handle baseline");
    measure("MEASURE update_handoff_native_control_cold_handles=", handles_before);
    measure("MEASURE update_handoff_native_control_first_handles=", control_first);
    measure("MEASURE update_handoff_native_control_second_handles=", control_second);
    if (counted_before && first_count && control_first >= handles_before)
        measure("MEASURE update_handoff_native_control_initial_handle_delta=", static_cast<u64>(control_first) - handles_before);
    // This baseline is established by the independent plain API control, never
    // by ignoring a resource increase in the actual handoff implementation.
    handles_before = control_second;
    // Real controlled peer processes. No MaiCord/account module, UI, credential,
    // shell, downloader or shipped helper is used for these native proofs.
    // Each round's unique named inheritable sentinel also releases its peer once
    // the parent has classified the handoff wait, so no fixed peer delay exists.
    const auto round_identity = [&](u8 scenario, u8 round, HANDLE* outsider) noexcept {
        auto id = fixture_identity(scenario);
        // Fixture namespace nonce only: PID is never used as a process wait or
        // trust capability. Root's SYNCHRONIZE owner remains the parent identity.
        const DWORD fixture_process = GetCurrentProcessId(); const u64 nonce = GetTickCount64();
        copy_bytes(id.installation.bytes, &fixture_process, sizeof(fixture_process));
        copy_bytes(id.installation.bytes + 4, &nonce, sizeof(nonce)); id.installation.bytes[12] = round;
        wchar_t event_name[80]{}; fixture_event_name(id, event_name);
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        *outsider = CreateEventW(&security, TRUE, FALSE, event_name);
        check(*outsider != nullptr && GetLastError() != ERROR_ALREADY_EXISTS,
            "handoff: unique test-only named inheritable sentinel created for object-identity exclusion proof");
        const u64 outsider_value = reinterpret_cast<u64>(*outsider);
        copy_bytes(id.document_sha256 + 24, &outsider_value, sizeof(outsider_value));
        return id;
    };
    for (u8 scenario = 1; scenario <= 7; ++scenario) {
        DWORD round_before = 0; GetProcessHandleCount(GetCurrentProcess(), &round_before);
        HANDLE outsider = nullptr; const auto id = round_identity(scenario, scenario, &outsider);
        UpdateHandoff owner{};
        const auto launched = launch_native(peer, fixture_length(peer), id, &owner);
        check(launched.ok() && owner.launched && owner.process && owner.thread && owner.parent && owner.view,
            "handoff: known fixture launches hidden with three explicit inherited capabilities");
        if (!launched.ok()) { fixture_handoff_release(&owner); if (outsider) CloseHandle(outsider); continue; }
        check(launched.outcome == UpdateHandoffOutcome::none && !owner.wait_completed,
            "handoff: CreateProcess success alone never acknowledges installation");
        MEMORY_BASIC_INFORMATION information{};
        const bool queried = VirtualQuery(owner.view, &information, sizeof(information)) == sizeof(information);
        check(queried && information.State == MEM_COMMIT && information.RegionSize <= 65536,
            "handoff: fixed acknowledgement mapping commitment is bounded");
        if (scenario == 1 && queried) measure("MEASURE update_handoff_ack_mapping_commit_bytes=", information.RegionSize);
        HANDLE cancel = nullptr;
        if (scenario == 3) cancel = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        const auto waited = fixture_handoff_wait(&owner, 1500, cancel);
        check((scenario == 1 && waited.ok() && waited.outcome == UpdateHandoffOutcome::acknowledged) ||
            (scenario == 2 && waited.error == UpdateHandoffError::acknowledgement) ||
            (scenario == 3 && waited.ok() && waited.outcome == UpdateHandoffOutcome::cancelled) ||
            (scenario == 4 && waited.ok() && waited.outcome == UpdateHandoffOutcome::process_exited) ||
            ((scenario == 5 || scenario == 6 || scenario == 7) && waited.error == UpdateHandoffError::acknowledgement),
            "handoff: exact ack, stale transaction/digest/installation, cancellation, exit and empty event classified");
        check(fixture_handoff_wait(&owner, 1).error == UpdateHandoffError::invalid_input,
            "handoff: one-shot wait never turns a late event into installation success");
        check(outsider && SetEvent(outsider) && finish_peer(&owner, scenario),
            "handoff: released controlled peer exits within bounded native process wait");
        if (outsider) check(CloseHandle(outsider) != FALSE, "handoff: excluded unrelated inheritable sentinel closes in parent");
        if (scenario == 4) {
            UpdateHandoffChild retained_process{};
            const bool retained = DuplicateHandle(GetCurrentProcess(), owner.process, GetCurrentProcess(),
                &retained_process.parent, SYNCHRONIZE, FALSE, 0) != FALSE;
            check(retained, "handoff: exited known peer has a separately retained real SYNCHRONIZE process owner");
            if (retained) {
                const bool closed_original = CloseHandle(owner.process) != FALSE;
                if (closed_original) owner.process = nullptr;
                check(closed_original, "handoff: original process handle can close without invalidating retained identity");
                check(WaitForSingleObject(retained_process.parent, 1) == WAIT_OBJECT_0,
                    "handoff: real retained process wait observes exact exit after original handle closed; no PID or sleep");
                check(fixture_handoff_child_release(&retained_process).ok(), "handoff: retained exited process owner closes explicitly");
            }
        }
        if (cancel) check(CloseHandle(cancel) != FALSE, "handoff: cancel owner released");
        if (scenario == 1) {
            void* mapped = owner.view; handoff_fault_test::fail_unmap = true;
            check(fixture_handoff_release(&owner).error == UpdateHandoffError::release && owner.view == mapped && owner.process,
                "handoff: failed unmap retains mapping and process owners for retry");
            HANDLE thread = owner.thread; handoff_fault_test::fail_close = true;
            check(fixture_handoff_release(&owner).error == UpdateHandoffError::release && !owner.view && owner.thread == thread && owner.process,
                "handoff: failed returned thread close retains all remaining cleanup owners");
        }
        check(fixture_handoff_release(&owner).ok() && empty(owner), "handoff: all explicit process/channel/parent owners released");
        DWORD round_after = 0;
        if (GetProcessHandleCount(GetCurrentProcess(), &round_after) && round_after != round_before) {
            measure("DIAG update_handoff_handle_scenario=", scenario);
            measure("DIAG update_handoff_handles_before_round=", round_before);
            measure("DIAG update_handoff_handles_after_round=", round_after);
        }
        check(round_after == round_before, "handoff: every real peer round restores its exact initialized handle baseline");
    }
    UpdateHandoff deadline{}; HANDLE deadline_release = nullptr;
    check(launch_native(peer, fixture_length(peer), round_identity(3, 8, &deadline_release), &deadline).ok(), "handoff: deadline peer launches");
    if (deadline.launched) {
        const auto result = fixture_handoff_wait(&deadline, 1);
        check(result.ok() && result.outcome == UpdateHandoffOutcome::deadline, "handoff: absent startup acknowledgement reaches bounded deadline");
        check(deadline_release && SetEvent(deadline_release) && finish_peer(&deadline, 3) && fixture_handoff_release(&deadline).ok(),
            "handoff: deadline never abandons launched process owners");
    } else fixture_handoff_release(&deadline);
    if (deadline_release) check(CloseHandle(deadline_release) != FALSE, "handoff: deadline release sentinel closes in parent");
    UpdateHandoff failed{}; handoff_fault_test::fail_launch = true;
    check(launch_native(peer, fixture_length(peer), identity, &failed).error == UpdateHandoffError::launch &&
        !failed.launched && failed.parent && failed.mapping && failed.view,
        "handoff: injected CreateProcess failure cannot acknowledge and preserves created capabilities");
    if (failed.view && failed.event) {
        UpdateHandoffChild signal_owner{}; signal_owner.identity = identity;
        const bool duplicate_event = DuplicateHandle(GetCurrentProcess(), failed.event, GetCurrentProcess(), &signal_owner.event,
            EVENT_MODIFY_STATE, FALSE, 0) != FALSE;
        signal_owner.view = MapViewOfFile(failed.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(AckSlot));
        check(duplicate_event && signal_owner.view, "handoff: local fault fixture owns independent acknowledgement capabilities");
        if (duplicate_event && signal_owner.view) {
            handoff_fault_test::fail_signal = true;
            check(fixture_handoff_child_acknowledge(&signal_owner, true).error == UpdateHandoffError::channel &&
                signal_owner.event && signal_owner.view && !signal_owner.acknowledged,
                "handoff: failed acknowledgement signal preserves owners and committed exact receipt");
            check(fixture_handoff_child_acknowledge(&signal_owner, true).ok() && signal_owner.acknowledged,
                "handoff: retry of failed exact acknowledgement signal succeeds without changing identity");
        }
        check(fixture_handoff_child_release(&signal_owner).ok(), "handoff: fault child capabilities close independently");
    }
    check(fixture_handoff_release(&failed).ok() && empty(failed), "handoff: failed launch cleanup releases channel and real parent");
#else
    check(false, "handoff: root must explicitly supply the known test-only peer absolute path");
#endif
    DWORD handles_after = 0;
    check(counted_before && GetProcessHandleCount(GetCurrentProcess(), &handles_after) && handles_before == handles_after,
        "handoff: real peers, bounded mappings, cancellation and injected failures restore process handle baseline");
    if (counted_before && handles_after >= handles_before)
        measure("MEASURE update_handoff_process_handle_delta=", static_cast<u64>(handles_after) - handles_before);
}
} // namespace maicord

namespace maicord {
namespace update_handoff_entry {
u32 failures;
void check(bool success, const char* name) noexcept {
    test_check(&failures, success, name);
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_update_handoff_tests_entry() noexcept {
    maicord::test_update_handoff(maicord::update_handoff_entry::check);
    ExitProcess(maicord::update_handoff_entry::failures ? 1 : 0);
}
