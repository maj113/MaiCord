#include "maicord/media/hevc_references.hpp"
namespace maicord {
u32 hevc_reference_surfaces(const HevcReferenceState& s) noexcept {
    u32 mask{};
    for (u32 i = 0; i < s.count && i < 15; ++i)
        if (s.pictures[i].surface < 32)
            mask |= 1u << s.pictures[i].surface;
    return mask;
}
HevcError hevc_reference_plan(const HevcReferenceState& s, const HevcSps& a, const HevcPicture& p,
                              u8 surface, HevcReferencePlan* output) noexcept {
    if (!output || s.count > 15 || surface >= 32 || a.poc_bits < 4 || a.poc_bits > 16 || !a.max_buffering ||
        a.max_buffering > 15 || p.short_term.negative + p.short_term.positive > 15 ||
        p.long_term_count > 15)
        return HevcError::invalid_input;
    if (!s.ready && !p.irap)
        return HevcError::unsupported;
    for (u32 i = 0; i < s.count; ++i) {
        if (s.pictures[i].surface >= 32 || (!p.idr && s.pictures[i].surface == surface))
            return HevcError::invalid_input;
        for (u32 j = 0; j < i; ++j)
            if (s.pictures[i].surface == s.pictures[j].surface || s.pictures[i].poc == s.pictures[j].poc)
                return HevcError::invalid_input;
    }
    const i64 modulus = i64{1} << a.poc_bits;
    if (p.poc_lsb >= modulus)
        return HevcError::malformed;
    const bool reset = p.idr || (p.nal_type >= 16 && p.nal_type <= 18);
    i64 msb = reset || !s.ready ? 0 : s.previous_msb;
    if (!reset && s.ready) {
        if (p.poc_lsb < s.previous_lsb && s.previous_lsb - p.poc_lsb >= modulus / 2)
            msb += modulus;
        else if (p.poc_lsb > s.previous_lsb && p.poc_lsb - s.previous_lsb > modulus / 2)
            msb -= modulus;
    }
    const i64 poc = p.idr ? 0 : msb + p.poc_lsb;
    if (poc < (-2147483647ll - 1) || poc > 2147483647ll || msb < (-2147483647ll - 1) || msb > 2147483647ll)
        return HevcError::malformed;
    HevcReferencePlan plan{};
    plan.poc = static_cast<i32>(poc);
    plan.after = s;
    plan.after.count = 0;
    u32 retained{};
    const auto admit = [&](i64 wanted, bool lsb_only, bool used, bool long_term, u32 group) noexcept {
        i32 found = -1;
        for (u32 i = 0; i < s.count; ++i) {
            const i64 current = s.pictures[i].poc;
            const bool matches =
                lsb_only ? ((current % modulus + modulus) % modulus == wanted) : current == wanted;
            if (matches) {
                if (found >= 0)
                    return false;
                found = static_cast<i32>(i);
            }
        }
        if (found < 0)
            return !used; // Unused unavailable preroll may be omitted.
        const u32 index = static_cast<u32>(found);
        if (retained & (1u << index))
            return false;
        if (plan.after.count >= 15)
            return false;
        retained |= 1u << index;
        auto entry = s.pictures[index];
        entry.long_term = long_term;
        plan.after.pictures[plan.after.count++] = entry;
        if (used) {
            u8* count = group == 0   ? &plan.before_count
                        : group == 1 ? &plan.following_count
                                     : &plan.long_count;
            u8* array = group == 0 ? plan.before : group == 1 ? plan.following : plan.long_term;
            if (*count >= 8)
                return false;
            array[(*count)++] = static_cast<u8>(index);
        }
        return true;
    };
    if (!reset) {
        const u32 short_count = p.short_term.negative + p.short_term.positive;
        for (u32 i = 0; i < short_count; ++i) {
            const i32 delta = p.short_term.delta[i];
            if ((i < p.short_term.negative && delta >= 0) || (i >= p.short_term.negative && delta <= 0) ||
                !admit(poc + delta, false, (p.short_term.used & (1u << i)) != 0, false,
                       i < p.short_term.negative ? 0 : 1))
                return HevcError::malformed;
        }
        for (u32 i = 0; i < p.long_term_count; ++i) {
            const auto& r = p.long_term[i];
            if (r.poc_lsb >= modulus)
                return HevcError::malformed;
            const i64 wanted = r.delta_msb_present
                                   ? poc - i64{r.delta_msb_cycles} * modulus - p.poc_lsb + r.poc_lsb
                                   : r.poc_lsb;
            if (!admit(wanted, !r.delta_msb_present, r.used, true, 2))
                return HevcError::malformed;
        }
    }
    if (p.reference) {
        if (plan.after.count >= a.max_buffering)
            return HevcError::unsupported;
        plan.after.pictures[plan.after.count++] = {plan.poc, surface, false};
    }
    // HEVC prevTid0Pic excludes RADL/RASL and sub-layer non-reference pictures.
    if (p.temporal_id == 0 && p.reference && (p.nal_type < 6 || p.nal_type > 9)) {
        plan.after.previous_lsb = p.idr ? 0 : p.poc_lsb;
        plan.after.previous_msb = static_cast<i32>(msb);
    }
    plan.after.ready = true;
    *output = plan;
    return HevcError::none;
}
} // namespace maicord
