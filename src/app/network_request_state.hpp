#pragma once

#include "maicord/core/interlocked.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/platform/network.hpp"

namespace maicord {

// Life cycle of the one request a RestClient or CdnClient carries in its
// request_state. The caller moves empty -> writing -> pending; the worker moves
// pending -> active. Cancelling parks a pending or active request in its cancel
// twin until the worker publishes it; only the worker moves publishing -> ready
// and only the taker ready -> empty.
enum RequestState : LONG {
    request_state_empty = 0,
    request_state_writing = 1,
    request_state_pending = 2,
    request_state_active = 3,
    request_state_ready = 4,
    request_state_cancel_pending = 5,
    request_state_cancel_active = 6,
    request_state_publishing = 7,
};

// Claims the request id for cancellation. A pending request is cancelled when the
// worker activates it; an active one is told through the cancel signal.
template <typename Client>
bool network_client_cancel(Client* client, NetworkSignal* cancel_signal, u32 cancel_value,
                           u64 request_id) noexcept {
    LONG claimed_state = request_state_empty;
    for (;;) {
        const LONG state = read_acquire(&client->request_state);
        if (state != request_state_pending && state != request_state_active) {
            return false;
        }
        if (static_cast<u64>(read_acquire(&client->current_request_id)) != request_id) {
            return false;
        }
        const LONG cancelling =
            state == request_state_pending ? request_state_cancel_pending : request_state_cancel_active;
        if (InterlockedCompareExchange(&client->request_state, cancelling, state) == state) {
            claimed_state = cancelling;
            break;
        }
    }
    if (claimed_state == request_state_cancel_pending) {
        return true;
    }
    const NetworkResult signalled = network_signal_post(&client->worker.loop, cancel_signal, cancel_value);
    if (!signalled.ok()) {
        return InterlockedCompareExchange(&client->request_state, request_state_active,
                                          request_state_cancel_active) != request_state_cancel_active;
    }
    return true;
}

// The worker's first step in completing a request: false when the request is
// no longer active (nothing to publish), otherwise moves it to publishing and
// reports whether it was being cancelled and, if asked, whether the worker had
// already started it.
template <typename Client>
bool network_client_begin_publish(Client* client, bool* cancelled, bool* active_cancel = nullptr) noexcept {
    *cancelled = false;
    if (active_cancel) {
        *active_cancel = false;
    }
    LONG prior =
        InterlockedCompareExchange(&client->request_state, request_state_publishing, request_state_active);
    if (prior != request_state_active) {
        if (prior != request_state_cancel_pending && prior != request_state_cancel_active) {
            return false;
        }
        if (InterlockedCompareExchange(&client->request_state, request_state_publishing, prior) != prior) {
            return false;
        }
        *cancelled = true;
        if (active_cancel) {
            *active_cancel = prior == request_state_cancel_active;
        }
    }
    return true;
}

} // namespace maicord
