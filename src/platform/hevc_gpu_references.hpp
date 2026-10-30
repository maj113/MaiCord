#pragma once
#include "maicord/media/hevc_references.hpp"

namespace maicord {
struct HevcGpuReferences {
    HevcReference pictures[15]{};
    u8 before[8]{}, following[8]{}, long_term[8]{};
    u8 count{}, before_count{}, following_count{}, long_count{};
};
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// DXVA sees the DPB after current RPS marking, excluding the destination picture.
// Current reference lists still name old DPB positions and must be remapped.
inline bool hevc_gpu_reference_map(const HevcReferenceState& previous, const HevcReferencePlan& plan,
                                   u8 surface, HevcGpuReferences* output) noexcept {
    if (!output || previous.count > 15 || plan.after.count > 15 || surface >= 32 || plan.before_count > 8 ||
        plan.following_count > 8 || plan.long_count > 8)
        return false;
    for (u32 i = 0; i < previous.count; ++i) {
        const auto& ref = previous.pictures[i];
        if (ref.surface >= 32)
            return false;
        for (u32 j = 0; j < i; ++j)
            if (ref.surface == previous.pictures[j].surface || ref.poc == previous.pictures[j].poc)
                return false;
    }
    for (u32 i = 0; i < plan.after.count; ++i) {
        const auto& ref = plan.after.pictures[i];
        if (ref.surface >= 32)
            return false;
        for (u32 j = 0; j < i; ++j)
            if (ref.surface == plan.after.pictures[j].surface || ref.poc == plan.after.pictures[j].poc)
                return false;
        if (ref.surface == surface || ref.poc == plan.poc) {
            if (ref.surface != surface || ref.poc != plan.poc || ref.long_term)
                return false;
            continue;
        }
        bool found{};
        for (u32 j = 0; j < previous.count; ++j)
            found |= ref.poc == previous.pictures[j].poc && ref.surface == previous.pictures[j].surface;
        if (!found)
            return false;
    }
    HevcGpuReferences mapped{};
    u8 indices[15]{};
    for (auto& index : indices)
        index = 255;
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
                indices[i] = mapped.count;
                mapped.pictures[mapped.count++] = ref;
            }
        }
    u32 used{};
    for (u32 group = 0; group < 3; ++group) {
        const u8* source = group == 0 ? plan.before : group == 1 ? plan.following : plan.long_term;
        const u32 count = group == 0   ? plan.before_count
                          : group == 1 ? plan.following_count
                                       : plan.long_count;
        u8* destination = group == 0 ? mapped.before : group == 1 ? mapped.following : mapped.long_term;
        for (u32 i = 0; i < count; ++i) {
            if (source[i] >= previous.count || indices[source[i]] == 255 || (used & (1u << source[i])))
                return false;
            used |= 1u << source[i];
            const u8 index = indices[source[i]];
            const auto& ref = mapped.pictures[index];
            if (group == 2 ? !ref.long_term
                           : ref.long_term || (group == 0 ? ref.poc >= plan.poc : ref.poc <= plan.poc))
                return false;
            destination[i] = index;
        }
    }
    mapped.before_count = plan.before_count;
    mapped.following_count = plan.following_count;
    mapped.long_count = plan.long_count;
    *output = mapped;
    return true;
}
#pragma clang attribute pop
} // namespace maicord
