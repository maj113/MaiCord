#pragma once
#include "maicord/media/hevc.hpp"
namespace maicord {
struct HevcReference {
    i32 poc{};
    u8 surface{};
    bool long_term{};
};
struct HevcReferenceState {
    HevcReference pictures[15]{};
    i32 previous_msb{};
    u16 previous_lsb{};
    u8 count{};
    bool ready{};
};
struct HevcReferencePlan {
    HevcReferenceState after{};
    i32 poc{};
    u8 before[8]{}, following[8]{}, long_term[8]{};
    u8 before_count{}, following_count{}, long_count{};
};
HevcError hevc_reference_plan(const HevcReferenceState&, const HevcSps&, const HevcPicture&, u8 surface,
                              HevcReferencePlan*) noexcept;
u32 hevc_reference_surfaces(const HevcReferenceState&) noexcept;
} // namespace maicord
