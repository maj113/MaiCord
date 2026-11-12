#pragma once
#include "maicord/core/base.hpp"

namespace maicord::app_detail {
// Publication IDs are process-wide, monotonic and never reused. Layout/source
// revision changes do not retire this immutable accepted content identity.
struct ApplicationSearchSourceTicket {
    u64 message_id;
    u32 publication_revision, index;
};
static_assert(sizeof(ApplicationSearchSourceTicket) == 16);
// What a ticket resolves to (search.hpp).
struct ApplicationSearchSourceView;
} // namespace maicord::app_detail
