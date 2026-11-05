#include "va_file.hpp"

#include "../hevc_gpu_references.hpp"
// Per-picture control flow, no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord::vaapi {

bool submit_h264_sample(Session* session, const H264Sps& sps, const H264Pps& pps, const H264Picture& picture,
                        ConstSlice<u8> sample, u8 length_bytes, const H264ReferenceState& references,
                        const H264PictureOrder& order, u8 surface) noexcept {
    if (!picture_begin(session, surface))
        return false;
    PictureParameterH264 parameters;
    h264_picture_parameters(sps, pps, picture.first, references, order, surface, session->surface, &parameters);
    IqMatrixH264 matrix;
    h264_iq_matrix(pps, &matrix);
    bool ok = picture_buffer(session, kPictureParameterBuffer, &parameters, sizeof(parameters)) &&
              picture_buffer(session, kIqMatrixBuffer, &matrix, sizeof(matrix));
    H264NalCursor cursor{sample, 0, length_bytes};
    u32 slices = 0;
    while (ok) {
        const H264Nal nal = h264_next_nal(&cursor);
        if (nal.error == H264Error::end)
            break;
        if (nal.error != H264Error::none) {
            ok = false;
            break;
        }
        if (nal.type != 1 && nal.type != 5)
            continue;
        H264SliceDetail detail;
        SliceParameterH264 slice;
        ok = slices++ < picture.slice_count &&
             h264_parse_slice_detail(nal.bytes, sps, pps, &detail) == H264Error::none &&
             h264_slice_parameters(sps, pps, picture.first, detail, references, order, session->surface, nal.bytes.count,
                                   &slice) &&
             picture_slice(session, &slice, sizeof(slice), nal.bytes.data, nal.bytes.count);
    }
    return picture_end(session) && ok && slices == picture.slice_count;
}

bool submit_hevc_sample(Session* session, const HevcConfiguration& configuration, const HevcPicture& picture,
                        ConstSlice<u8> sample, const HevcReferenceState& references, const HevcReferencePlan& plan,
                        u8 surface) noexcept {
    HevcGpuReferences mapped{};
    if (!hevc_gpu_reference_map(references, plan, surface, &mapped) || !picture.slice_count ||
        !picture_begin(session, surface))
        return false;
    PictureParameterHevc parameters;
    hevc_picture_parameters(configuration.sps, configuration.pps, picture, picture.slice_type == 2, plan, mapped, surface,
                            session->surface, &parameters);
    bool ok = picture_buffer(session, kPictureParameterBuffer, &parameters, sizeof(parameters));
    if (ok && configuration.sps.scaling_enabled) {
        IqMatrixHevc matrix;
        hevc_iq_matrix(configuration.pps, &matrix);
        ok = picture_buffer(session, kIqMatrixBuffer, &matrix, sizeof(matrix));
    }
    HevcSliceDetail previous{}, detail{};
    for (u32 i = 0; ok && i < picture.slice_count; ++i) {
        const auto& span = picture.slices[i];
        if (span.offset > sample.count || span.bytes > sample.count - span.offset || span.bytes < 3) {
            ok = false;
            break;
        }
        const ConstSlice<u8> nal{sample.data + span.offset, span.bytes};
        HevcNal header{nal, HevcError::none, static_cast<u8>((nal[0] >> 1) & 63u), static_cast<u8>((nal[1] & 7u) - 1u)};
        SliceParameterHevc slice;
        ok = hevc_parse_slice_detail(header, configuration, i ? &previous : nullptr, &detail) == HevcError::none;
        if (!ok)
            break;
        hevc_slice_parameters(detail, mapped, i + 1 == picture.slice_count, nal.count, &slice);
        ok = picture_slice(session, &slice, sizeof(slice), nal.data, nal.count);
        previous = detail;
    }
    return picture_end(session) && ok;
}

} // namespace maicord::vaapi
#pragma clang attribute pop
