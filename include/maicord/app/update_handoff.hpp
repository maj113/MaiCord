#pragma once

#include "maicord/app/update_generation_authentication.hpp"

namespace maicord {

inline constexpr u32 kUpdateHandoffProtocol = 1;
inline constexpr u32 kUpdateHandoffCommandCharacters = 1024;
inline constexpr u32 kUpdateHandoffMaximumWaitMs = 60000;
inline constexpr u32 kUpdateHandoffAcknowledgementBytes = 128;

struct UpdateHandoffIdentity {
    UpdateGenerationIdentity installation;
    UpdateGenerationIdentity transaction;
    u8 document_sha256[32];
};
enum class UpdateHandoffError : u8 {
    none, invalid_input, enrollment, generation, authenticity, target, parent,
    channel, attributes, launch, wait, acknowledgement, release,
};
enum class UpdateHandoffOutcome : u8 { none, acknowledged, process_exited, deadline, cancelled };
struct UpdateHandoffResult {
    UpdateHandoffError error;
    DWORD native_error;
    UpdateHandoffOutcome outcome;
    constexpr bool ok() const noexcept { return error == UpdateHandoffError::none; }
};

// Zero initialized, noncopyable by contract. No heap, permanent worker or named
// IPC. Failed close/unmap leaves the handle for a release retry. A launch
// does not acknowledge startup. Caller keeps generation gate and member pins live
// through launch, then coordinates journal acknowledgement/recovery separately.
struct UpdateHandoff {
    HANDLE process, thread;
    HANDLE parent; // real SYNCHRONIZE handle, never a PID/pseudo handle
    HANDLE mapping, event;
    HANDLE child_mapping, child_event; // reduced, inheritable capabilities
    HANDLE validation; // transient fixed-path identity handle; preserve on failure
    void* view;
    UpdateHandoffIdentity expected;
    bool launched, wait_completed;
};

// Gate is supplied only by the trusted installation enrollment caller. This API
// reauthenticates the active descriptor under the concrete enrolled public-key
// inventory and binds every borrowed member pin to its generation file identity.
// No executable/source/destination path is accepted from an offer or command line.
// Pending may launch its authenticated active package; that never advances journal.
// The permission comes only from a live coordinator activation, never a parsed
// pending journal. Stable dispatcher launches use nullptr and preserve the
// stable journal's zero transaction identity; their startup acknowledgement cannot
// acknowledge a pending installation transaction.
UpdateHandoffResult update_handoff_launch(UpdateGenerationTransaction*,
    UpdateGenerationAuthenticationContext*, const UpdatePackage&, bool trusted_caller_enrolled,
    const UpdateGenerationLaunchAdmission* live_pending_admission, UpdateHandoff*) noexcept;
// One native wait. Cancel is borrowed and optional; cancellation and
// process death take precedence when already signalled. A malformed/stale ack
// consumes the wait and fails closed. No periodic wakeups or retry loop.
UpdateHandoffResult update_handoff_wait(UpdateHandoff*, u32 timeout_ms, HANDLE cancel = nullptr) noexcept;
UpdateHandoffResult update_handoff_release(UpdateHandoff*) noexcept;

// Child-side handles are only those inherited through the explicit list.
// The early dispatcher first validates its own trusted enrollment/generation,
// then compares these fixed protocol IDs. No serialized HANDLE grants trust.
struct UpdateHandoffChild {
    HANDLE parent, mapping, event;
    void* view;
    UpdateHandoffIdentity identity;
    bool acknowledged;
};
UpdateHandoffResult update_handoff_child_open(ConstSlice<wchar_t> command_line,
    const UpdateHandoffIdentity& independently_validated_identity,
    bool trusted_caller_enrolled, UpdateHandoffChild*) noexcept;
// Caller asserts local initialization AND account_host API acceptance;
// network authentication/READY is neither needed nor accepted as a substitute.
UpdateHandoffResult update_handoff_child_acknowledge(UpdateHandoffChild*, bool local_startup_admitted) noexcept;
UpdateHandoffResult update_handoff_child_release(UpdateHandoffChild*) noexcept;

} // namespace maicord
