#pragma once

#include "maicord/app/gateway_client.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/heap_sort.hpp"

namespace maicord {

// Both callers first establish that group summaries are fixed-size records at
// the packed body's tail. Share the bounded uniqueness check across JSON
// decode and typed-event preflight without retaining all group IDs.
__declspec(noinline) inline bool gateway_member_list_summary_ids_unique(
    ConstSlice<u8> payload, u32 groups) noexcept {
    if (!groups) return true;
    const u32 stride = sizeof(GatewayClientMemberListMutation);
    if (!payload.data || groups > 2048 || groups > payload.count / stride) return false;
    const u32 begin = payload.count - groups * stride;
    u64 keys[256];
    for (u32 first = 0; first < groups; first += 256) {
        const u32 count = minimum(256u, groups - first);
        for (u32 i = 0; i < count; ++i)
            keys[i] = byte_load_le64(payload.data + begin + (first + i) * stride);
        heap_sort(keys, count, [](u64 left, u64 right) noexcept { return left < right; });
        for (u32 i = 1; i < count; ++i) if (keys[i - 1] == keys[i]) return false;
        for (u32 prior = 0; prior < first; ++prior) {
            const u64 id = byte_load_le64(payload.data + begin + prior * stride);
            u32 low = 0, high = count;
            while (low < high) {
                const u32 middle = low + (high - low) / 2;
                if (keys[middle] < id) low = middle + 1;
                else high = middle;
            }
            if (low < count && keys[low] == id) return false;
        }
    }
    return true;
}

} // namespace maicord
