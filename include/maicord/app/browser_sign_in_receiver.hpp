#pragma once

#include "maicord/platform/secure_buffer.hpp"
#include "maicord/protocol/browser_sign_in.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {

enum class BrowserSignInReceiverPhase : u8 {
    empty, starting, waiting, candidate_ready, exchange_pending, complete, expired, cancelled, failed, stopping,
};
enum class BrowserSignInReceiverError : u8 {
    none, invalid_input, memory, random, event, thread, network, ports_busy, state, output, stop,
};
struct BrowserSignInReceiverStatus {
    u64 generation;
    u64 candidate_id;
    u64 deadline_ms;
    u32 native_error;
    u16 port;
    BrowserSignInReceiverPhase phase;
    BrowserSignInReceiverError error;
    constexpr bool ok() const noexcept { return error == BrowserSignInReceiverError::none; }
};
struct BrowserSignInReceiver { void* runtime; };

// Public APIs are serialized on the UI thread; only the private socket worker
// runs concurrently. Keep receiver/module alive after a failed stop.
// Start only on explicit user action. This starts one short-lived loopback
// worker; it does not launch a browser, stop another sign-in, or authenticate
// a user. The caller must stop its previous login before start. Generation
// must be nonzero and unique to this attempt. Timeout: 1 s through 5 minutes.
BrowserSignInReceiverStatus browser_sign_in_receiver_start(BrowserSignInReceiver* receiver,
    u64 generation, u32 timeout_ms) noexcept;
BrowserSignInReceiverStatus browser_sign_in_receiver_status(const BrowserSignInReceiver* receiver) noexcept;
HANDLE browser_sign_in_receiver_notification_handle(const BrowserSignInReceiver* receiver) noexcept;
// Output must be initialized, locked, disjoint and caller-owned. UTF-8
// URL contains the random binding key; do not log it or offer arbitrary URLs.
SecureBufferResult browser_sign_in_receiver_copy_url(const BrowserSignInReceiver* receiver,
    u64 expected_generation, SecureBuffer* output) noexcept;
// A single atomic locked JSON snapshot {key,handoff_token}. Success moves
// candidate_ready to exchange_pending. Submit through the ordinary protected
// auth transport, then erase output immediately after synchronous copying.
SecureBufferResult browser_sign_in_receiver_take_exchange(BrowserSignInReceiver* receiver,
    u64 expected_generation, u64 expected_candidate_id, SecureBuffer* output) noexcept;
// accepted=true is permitted only after the server validated the exchange with
// this attempt's key. false erases the candidate and resumes the same bounded
// wait: invalid/unbound local traffic must not destroy the waiting attempt.
// false may also discard candidate_ready after a snapshot capacity failure.
BrowserSignInReceiverStatus browser_sign_in_receiver_finish_exchange(BrowserSignInReceiver* receiver,
    u64 expected_generation, u64 expected_candidate_id, bool accepted) noexcept;
// Rejects new work and erases the key/token before signaling stop.
// A failed join keeps the live runtime for retry; never release its module.
// Join timeout is 0 through 5000 milliseconds.
BrowserSignInReceiverStatus browser_sign_in_receiver_stop(BrowserSignInReceiver* receiver,
    u32 timeout_ms) noexcept;

} // namespace maicord
