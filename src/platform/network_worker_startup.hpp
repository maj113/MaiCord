#pragma once
#include "maicord/platform/network_worker.hpp"
namespace maicord::network_worker_detail {
// Internal bootstrap-failure publisher, also used by the deterministic fault
// fixture. Invoke only on the worker thread, with no initialized loop/work.
void publish_startup_failure(NetworkWorker* worker, const NetworkResult& error) noexcept;
}
