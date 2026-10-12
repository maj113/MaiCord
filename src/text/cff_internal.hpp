#pragma once
#include "maicord/text/cff_outline.hpp"

namespace maicord::cff_detail {

struct Dictionary {
    double matrix[6]{.001, 0, 0, .001, 0, 0};
    double default_width = 0, nominal_width = 0;
    u32 charstrings = 0, charset = 0, private_offset = 0, private_size = 0;
    u32 subrs = 0, fd_array = 0, fd_select = 0, random_seed = 0;
    bool has_matrix = false, cid = false;
};

SfntStatus index_open(ConstSlice<u8> data, u32* offset, CffIndex* index) noexcept;
ConstSlice<u8> index_object(const CffIndex& index, u32 object) noexcept;
SfntStatus dictionary(ConstSlice<u8> bytes, Dictionary* result) noexcept;
SfntStatus private_subrs(ConstSlice<u8> table, const Dictionary& dict,
                           CffIndex* subrs, u32* seed, double* default_width, double* nominal_width) noexcept;
SfntStatus select_dict(const CffFont& font, u16 glyph, CffIndex* subrs,
                         double* matrix, u32* seed, double* default_width, double* nominal_width) noexcept;
SfntStatus standard_glyph(const CffFont& font, u32 code, u16* glyph) noexcept;

} // namespace maicord::cff_detail
