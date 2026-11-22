#pragma once
#include "maicord/media/video/hevc_references.hpp"

namespace maicord {
struct HevcGpuReferences {
    HevcReference pictures[15]{};
    u8 before[8]{}, following[8]{}, long_term[8]{};
    u8 count{}, before_count{}, following_count{}, long_count{};
};
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// DXVA sees the DPB after current RPS marking, excluding the destination picture.
// Current reference lists still name old DPB positions and must be remapped.
// plan is hevc_reference_plan's for previous: previous holds distinct surfaces and POCs, after holds the current
// picture on surface and pictures of previous, and each list entry names a picture of previous that after kept, once,
// in its list's order. False when a kept picture has the current POC (a malformed stream).
inline bool hevc_gpu_reference_map(const HevcReferenceState& previous, const HevcReferencePlan& plan,
                                   u8 surface, HevcGpuReferences* output) noexcept {
    HevcGpuReferences mapped{};
    u8 indices[15];
    for (auto& index : mapped.before)
        index = 255;
    for (auto& index : mapped.following)
        index = 255;
    for (auto& index : mapped.long_term)
        index = 255;
    for (u32 i = 0; i < previous.count; ++i)
        for (u32 j = 0; j < plan.after.count; ++j) {
            const auto& ref = plan.after.pictures[j];
            if (ref.surface != surface && ref.poc == previous.pictures[i].poc &&
                ref.surface == previous.pictures[i].surface) {
                if (ref.poc == plan.poc)
                    return false;
                indices[i] = mapped.count;
                mapped.pictures[mapped.count++] = ref;
            }
        }
    for (u32 i = 0; i < plan.before_count; ++i)
        mapped.before[i] = indices[plan.before[i]];
    for (u32 i = 0; i < plan.following_count; ++i)
        mapped.following[i] = indices[plan.following[i]];
    for (u32 i = 0; i < plan.long_count; ++i)
        mapped.long_term[i] = indices[plan.long_term[i]];
    mapped.before_count = plan.before_count;
    mapped.following_count = plan.following_count;
    mapped.long_count = plan.long_count;
    *output = mapped;
    return true;
}
#pragma clang attribute pop
} // namespace maicord
