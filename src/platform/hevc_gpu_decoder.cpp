#include "maicord/platform/hevc_gpu_decoder.hpp"
#include "maicord/platform/mp4_decoder.hpp"
#include "maicord/media/hevc_references.hpp"
#include "maicord/media/hevc_mp4_demux.hpp"
#include "maicord/media/h264_output_queue.hpp"
#include "maicord/media/video_display.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/win32_process.hpp"
#include "hevc_gpu_geometry.hpp"
#include "hevc_gpu_references.hpp"
#include "gpu_decoder_fence.hpp"
#include <d3d11_4.h>
#include <initguid.h>
#include <dxva.h>

namespace maicord {
namespace {
#if defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
enum ProbePhase : u32 {
    probe_guard,
    probe_begin,
    probe_get_picture,
    probe_release_picture,
    probe_get_matrix,
    probe_release_matrix,
    probe_get_bitstream,
    probe_release_bitstream,
    probe_get_slices,
    probe_release_slices,
    probe_submit,
    probe_end,
    probe_phase_count
};
struct ProbeTiming {
    u64 total_ms{}, max_ms{};
    u32 calls{}, first_slow_sample_plus_one{};
};
#endif
struct State {
    HevcConfiguration config;
    HevcPicture picture;
    Mp4Demuxer demux;
    HevcReferenceState references;
    H264OutputQueue output;
    ID3D11VideoDevice* device;
    ID3D11VideoContext* context;
    ID3D11Multithread* guard;
    ID3D11VideoDecoder* decoder;
    ID3D11Texture2D* texture;
    ID3D11VideoDecoderOutputView* views[32];
    GpuDecoderFence wait;
    GpuVideoFrame frame;
    VideoDisplayExtent display;
    u32 feedback;
    u8 surface_count;
    bool full_stream;
#if defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
    ProbeTiming probe_timing[probe_phase_count];
    u32 probe_busy_errors{}, probe_driver_errors{};
    u32 probe_bitstream_capacity{}, probe_slice_capacity{}, probe_mapping_reuse{};
    const void* probe_bitstream_mapping{};
#endif
};
static_assert(sizeof(State) <= 16384);
#if defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
struct ProbeClock {
    State& state;
    u32 phase;
    u64 start{GetTickCount64()};
    ~ProbeClock() noexcept {
        auto& timing = state.probe_timing[phase];
        const u64 elapsed = GetTickCount64() - start;
        ++timing.calls;
        timing.total_ms += elapsed;
        timing.max_ms = maximum(timing.max_ms, elapsed);
        if (elapsed > 50 && !timing.first_slow_sample_plus_one)
            timing.first_slow_sample_plus_one = state.demux.sample_index + 1;
    }
};
u32 probe_buffer_phase(D3D11_VIDEO_DECODER_BUFFER_TYPE type, bool release) noexcept {
    const u32 get = type == D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS            ? probe_get_picture
                    : type == D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX ? probe_get_matrix
                    : type == D3D11_VIDEO_DECODER_BUFFER_BITSTREAM                   ? probe_get_bitstream
                                                                                     : probe_get_slices;
    return get + static_cast<u32>(release);
}
void probe_metric(const char* key, u64 value) noexcept {
    char output[128]{};
    u32 count{};
    while (*key && count < 90)
        output[count++] = *key++;
    output[count++] = '=';
    char digits[20]{};
    u32 length{};
    do {
        digits[length++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value && length < 20);
    while (length)
        output[count++] = digits[--length];
    output[count++] = '\n';
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, count, &written, nullptr);
}
void probe_phase_metric(const char* phase, const char* suffix, u64 value) noexcept {
    char key[96]{};
    u32 count{};
    const char* parts[]{"hevc_", phase, suffix};
    for (const char* part : parts)
        while (*part && count < sizeof(key) - 1)
            key[count++] = *part++;
    probe_metric(key, value);
}
void probe_sample_metric(u32 sample, const char* field, u64 value) noexcept {
    char key[96]{};
    u32 count{};
    const char* prefix = "hevc_sample";
    while (*prefix)
        key[count++] = *prefix++;
    key[count++] = static_cast<char>('0' + sample / 10);
    key[count++] = static_cast<char>('0' + sample % 10);
    key[count++] = '_';
    while (*field && count < sizeof(key) - 1)
        key[count++] = *field++;
    probe_metric(key, value);
}
void probe_sample_index_metric(u32 sample, const char* collection, u32 index, const char* field,
                               u64 value) noexcept {
    char key[64]{};
    u32 count{};
    while (*collection && count < 30)
        key[count++] = *collection++;
    if (index >= 10)
        key[count++] = static_cast<char>('0' + index / 10);
    key[count++] = static_cast<char>('0' + index % 10);
    key[count++] = '_';
    while (*field && count < sizeof(key) - 1)
        key[count++] = *field++;
    probe_sample_metric(sample, key, value);
}
#define MAICORD_HEVC_PROBE_TIME(owner, phase, operation)                                                   \
    [&]() noexcept {                                                                                       \
        ProbeClock clock{owner, phase};                                                                    \
        return (operation);                                                                                \
    }()
#else
#define MAICORD_HEVC_PROBE_TIME(owner, phase, operation) (operation)
#endif
H264Error mapped(HevcError error) noexcept {
    return static_cast<H264Error>(static_cast<u8>(error));
}
HRESULT upload(State& s, D3D11_VIDEO_DECODER_BUFFER_TYPE type, const void* data, u32 bytes,
               D3D11_VIDEO_DECODER_BUFFER_DESC* out) noexcept {
    UINT capacity{};
    void* destination{};
    HRESULT result =
        MAICORD_HEVC_PROBE_TIME(s, probe_buffer_phase(type, false),
                                s.context->GetDecoderBuffer(s.decoder, type, &capacity, &destination));
    if (FAILED(result))
        return result;
#if defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
    if (type == D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL) {
        s.probe_slice_capacity = capacity;
        if (destination && destination == s.probe_bitstream_mapping)
            ++s.probe_mapping_reuse;
    }
#endif
    const bool fits = destination && bytes <= capacity;
    if (fits)
        copy_bytes(destination, data, bytes);
    result = MAICORD_HEVC_PROBE_TIME(s, probe_buffer_phase(type, true),
                                     s.context->ReleaseDecoderBuffer(s.decoder, type));
    if (!fits)
        return E_OUTOFMEMORY;
    *out = {};
    out->BufferType = type;
    out->DataSize = bytes;
    return result;
}
HRESULT submit(State& s, const Mp4AccessUnit& unit, const HevcPicture& picture,
               const HevcReferencePlan& plan, u8 surface) noexcept {
    const auto& a = s.config.sps;
    const auto& b = s.config.pps;
    HevcGpuReferences references{};
    if (!hevc_gpu_reference_map(s.references, plan, surface, &references))
        return E_INVALIDARG;
    DXVA_PicParams_HEVC p{};
    p.PicWidthInMinCbsY = static_cast<USHORT>(a.coded_width >> (a.log2_min_cb_minus3 + 3));
    p.PicHeightInMinCbsY = static_cast<USHORT>(a.coded_height >> (a.log2_min_cb_minus3 + 3));
    p.chroma_format_idc = 1;
    p.log2_max_pic_order_cnt_lsb_minus4 = static_cast<USHORT>(a.poc_bits - 4);
    // Optional accelerator hint stays conservative; the software output queue
    // still enforces the exact SPS reorder and buffering limits.
    p.NoPicReorderingFlag = 0;
    p.CurrPic.Index7Bits = surface;
    p.sps_max_dec_pic_buffering_minus1 = static_cast<UCHAR>(a.max_buffering - 1);
    p.log2_min_luma_coding_block_size_minus3 = a.log2_min_cb_minus3;
    p.log2_diff_max_min_luma_coding_block_size = a.log2_diff_cb;
    p.log2_min_transform_block_size_minus2 = a.log2_min_tb_minus2;
    p.log2_diff_max_min_transform_block_size = a.log2_diff_tb;
    p.max_transform_hierarchy_depth_inter = a.transform_depth_inter;
    p.max_transform_hierarchy_depth_intra = a.transform_depth_intra;
    p.num_short_term_ref_pic_sets = a.short_term_count;
    p.num_long_term_ref_pics_sps = a.long_term_count;
    p.num_ref_idx_l0_default_active_minus1 = static_cast<UCHAR>(b.references_l0 - 1);
    p.num_ref_idx_l1_default_active_minus1 = static_cast<UCHAR>(b.references_l1 - 1);
    p.init_qp_minus26 = b.initial_qp_minus26;
    p.ucNumDeltaPocsOfRefRpsIdx = picture.reference_rps_delta_count;
    p.wNumBitsForShortTermRPSInSlice = picture.short_term_bits;
    p.scaling_list_enabled_flag = a.scaling_enabled;
    p.amp_enabled_flag = a.amp;
    p.sample_adaptive_offset_enabled_flag = a.sao;
    p.pcm_enabled_flag = a.pcm;
    p.pcm_sample_bit_depth_luma_minus1 = a.pcm_luma_depth_minus1;
    p.pcm_sample_bit_depth_chroma_minus1 = a.pcm_chroma_depth_minus1;
    p.log2_min_pcm_luma_coding_block_size_minus3 = a.log2_min_pcm_minus3;
    p.log2_diff_max_min_pcm_luma_coding_block_size = a.log2_diff_pcm;
    p.pcm_loop_filter_disabled_flag = a.pcm_loop_filter_disabled;
    p.long_term_ref_pics_present_flag = a.long_term_present;
    p.sps_temporal_mvp_enabled_flag = a.temporal_mvp;
    p.strong_intra_smoothing_enabled_flag = a.strong_intra_smoothing;
    p.dependent_slice_segments_enabled_flag = b.dependent_slices;
    p.output_flag_present_flag = b.output_flag_present;
    p.num_extra_slice_header_bits = b.extra_slice_bits;
    p.sign_data_hiding_enabled_flag = b.sign_data_hiding;
    p.cabac_init_present_flag = b.cabac_init;
    p.constrained_intra_pred_flag = b.constrained_intra;
    p.transform_skip_enabled_flag = b.transform_skip;
    p.cu_qp_delta_enabled_flag = b.cu_qp_delta;
    p.pps_slice_chroma_qp_offsets_present_flag = b.slice_chroma_offsets;
    p.weighted_pred_flag = b.weighted_pred;
    p.weighted_bipred_flag = b.weighted_bipred;
    p.transquant_bypass_enabled_flag = b.transquant_bypass;
    p.tiles_enabled_flag = b.tiles;
    p.entropy_coding_sync_enabled_flag = b.entropy_sync;
    p.uniform_spacing_flag = b.uniform_spacing;
    p.loop_filter_across_tiles_enabled_flag = b.loop_filter_tiles;
    p.pps_loop_filter_across_slices_enabled_flag = b.loop_filter_slices;
    p.deblocking_filter_override_enabled_flag = b.deblocking_override;
    p.pps_deblocking_filter_disabled_flag = b.deblocking_disabled;
    p.lists_modification_present_flag = b.lists_modification;
    p.slice_segment_header_extension_present_flag = b.slice_header_extension;
    p.IrapPicFlag = picture.irap;
    p.IdrPicFlag = picture.idr;
    p.IntraPicFlag = picture.slice_type == 2;
    p.pps_cb_qp_offset = b.cb_qp_offset;
    p.pps_cr_qp_offset = b.cr_qp_offset;
    p.num_tile_columns_minus1 = b.tile_columns_minus1;
    p.num_tile_rows_minus1 = b.tile_rows_minus1;
    copy_bytes(p.column_width_minus1, b.column_width_minus1, sizeof(p.column_width_minus1));
    copy_bytes(p.row_height_minus1, b.row_height_minus1, sizeof(p.row_height_minus1));
    p.diff_cu_qp_delta_depth = b.diff_cu_qp_depth;
    p.pps_beta_offset_div2 = b.beta_offset_div2;
    p.pps_tc_offset_div2 = b.tc_offset_div2;
    p.log2_parallel_merge_level_minus2 = b.log2_parallel_merge_minus2;
    p.CurrPicOrderCntVal = plan.poc;
    p.StatusReportFeedbackNumber = s.feedback == ~u32{0} ? 1 : s.feedback + 1;
    for (auto& ref : p.RefPicList)
        ref.bPicEntry = 0xff;
    for (auto& ref : p.RefPicSetStCurrBefore)
        ref = 0xff;
    for (auto& ref : p.RefPicSetStCurrAfter)
        ref = 0xff;
    for (auto& ref : p.RefPicSetLtCurr)
        ref = 0xff;
    for (u32 i = 0; i < references.count; ++i) {
        const auto& ref = references.pictures[i];
        p.RefPicList[i].Index7Bits = ref.surface;
        p.RefPicList[i].AssociatedFlag = ref.long_term;
        p.PicOrderCntValList[i] = ref.poc;
    }
    copy_bytes(p.RefPicSetStCurrBefore, references.before, references.before_count);
    copy_bytes(p.RefPicSetStCurrAfter, references.following, references.following_count);
    copy_bytes(p.RefPicSetLtCurr, references.long_term, references.long_count);
#if defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
    if (s.demux.sample_index >= 50 && s.demux.sample_index <= 53) {
        const u32 sample = s.demux.sample_index;
        probe_sample_metric(sample, "nal_type", picture.nal_type);
        probe_sample_metric(sample, "poc_lsb", picture.poc_lsb);
        probe_sample_metric(sample, "plan_poc_signed_bits", static_cast<u64>(static_cast<i64>(plan.poc)));
        probe_sample_metric(sample, "reference", picture.reference);
        probe_sample_metric(sample, "surface", surface);
        probe_sample_metric(sample, "no_reordering_hint", p.NoPicReorderingFlag);
        probe_sample_metric(sample, "rps_negative", picture.short_term.negative);
        probe_sample_metric(sample, "rps_positive", picture.short_term.positive);
        probe_sample_metric(sample, "rps_used_mask", picture.short_term.used);
        const u32 short_count = picture.short_term.negative + picture.short_term.positive;
        for (u32 i = 0; i < short_count; ++i)
            probe_sample_index_metric(sample, "rps_delta", i, "signed_bits",
                                      static_cast<u64>(static_cast<i64>(picture.short_term.delta[i])));
        probe_sample_metric(sample, "old_reference_count", s.references.count);
        for (u32 i = 0; i < s.references.count; ++i) {
            const auto& ref = s.references.pictures[i];
            probe_sample_index_metric(sample, "old_ref", i, "poc_signed_bits",
                                      static_cast<u64>(static_cast<i64>(ref.poc)));
            probe_sample_index_metric(sample, "old_ref", i, "surface", ref.surface);
            probe_sample_index_metric(sample, "old_ref", i, "long_term", ref.long_term);
            probe_sample_index_metric(sample, "dxva_ref", i, "entry", p.RefPicList[i].bPicEntry);
        }
        probe_sample_metric(sample, "retained_reference_count", references.count);
        for (u32 i = 0; i < references.count; ++i) {
            const auto& ref = references.pictures[i];
            probe_sample_index_metric(sample, "retained_ref", i, "poc_signed_bits",
                                      static_cast<u64>(static_cast<i64>(ref.poc)));
            probe_sample_index_metric(sample, "retained_ref", i, "surface", ref.surface);
            probe_sample_index_metric(sample, "retained_ref", i, "long_term", ref.long_term);
        }
        probe_sample_metric(sample, "after_reference_count", plan.after.count);
        for (u32 i = 0; i < plan.after.count; ++i) {
            const auto& ref = plan.after.pictures[i];
            probe_sample_index_metric(sample, "after_ref", i, "poc_signed_bits",
                                      static_cast<u64>(static_cast<i64>(ref.poc)));
            probe_sample_index_metric(sample, "after_ref", i, "surface", ref.surface);
            probe_sample_index_metric(sample, "after_ref", i, "long_term", ref.long_term);
        }
        probe_sample_metric(sample, "before_count", plan.before_count);
        probe_sample_metric(sample, "following_count", plan.following_count);
        probe_sample_metric(sample, "long_count", plan.long_count);
        for (u32 i = 0; i < plan.before_count; ++i)
            probe_sample_index_metric(sample, "before", i, "dxva_index", p.RefPicSetStCurrBefore[i]);
        for (u32 i = 0; i < plan.following_count; ++i)
            probe_sample_index_metric(sample, "following", i, "dxva_index", p.RefPicSetStCurrAfter[i]);
        for (u32 i = 0; i < plan.long_count; ++i)
            probe_sample_index_metric(sample, "long", i, "dxva_index", p.RefPicSetLtCurr[i]);
    }
#endif
    DXVA_Qmatrix_HEVC q{};
    if (a.scaling_enabled) {
        const auto& scaling = b.scaling;
        for (u32 i = 0; i < 6; ++i) {
            hevc_gpu_scaling_diagonal<4>(scaling.list4x4[i], q.ucScalingLists0[i]);
            hevc_gpu_scaling_diagonal<8>(scaling.list8x8[i], q.ucScalingLists1[i]);
            hevc_gpu_scaling_diagonal<8>(scaling.list16x16[i], q.ucScalingLists2[i]);
        }
        for (u32 i = 0; i < 2; ++i)
            hevc_gpu_scaling_diagonal<8>(scaling.list32x32[i], q.ucScalingLists3[i]);
        copy_bytes(q.ucScalingListDCCoefSizeID2, scaling.dc16, sizeof(q.ucScalingListDCCoefSizeID2));
        copy_bytes(q.ucScalingListDCCoefSizeID3, scaling.dc32, sizeof(q.ucScalingListDCCoefSizeID3));
    }
    D3D11_VIDEO_DECODER_BUFFER_DESC buffers[4]{};
    HRESULT result = upload(s, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, &p, sizeof(p), &buffers[0]);
    u32 buffer_count = 1;
    // DXVA HEVC forbids sending a matrix when scaling lists are disabled.
    if (SUCCEEDED(result) && a.scaling_enabled) {
        result =
            upload(s, D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, &q, sizeof(q), &buffers[1]);
        ++buffer_count;
    }
    if (FAILED(result))
        return result;
    UINT bit_capacity{};
    void* bit_data{};
    DXVA_Slice_HEVC_Short slices[kHevcMaximumSlices]{};
    result =
        MAICORD_HEVC_PROBE_TIME(s, probe_get_bitstream,
                                s.context->GetDecoderBuffer(s.decoder, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM,
                                                            &bit_capacity, &bit_data));
    if (FAILED(result))
        return result;
#if defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
    s.probe_bitstream_capacity = bit_capacity;
    s.probe_bitstream_mapping = bit_data;
#endif
    const u64 aligned = hevc_gpu_bitstream_extent(picture.bitstream_bytes);
    const u64 slice_bytes = u64{picture.slice_count} * sizeof(DXVA_Slice_HEVC_Short);
    if (!bit_data || aligned > bit_capacity || !picture.slice_count ||
        picture.slice_count > kHevcMaximumSlices)
        result = E_OUTOFMEMORY;
    if (SUCCEEDED(result)) {
        auto* bytes = static_cast<u8*>(bit_data);
        u32 offset{};
        for (u32 i = 0; i < picture.slice_count; ++i) {
            const auto& span = picture.slices[i];
            if (span.offset > unit.bytes.count || span.bytes > unit.bytes.count - span.offset ||
                offset > picture.bitstream_bytes || picture.bitstream_bytes - offset < 3 ||
                span.bytes > picture.bitstream_bytes - offset - 3) {
                result = E_INVALIDARG;
                break;
            }
            slices[i] = {offset, span.bytes + 3, 0};
            bytes[offset++] = 0;
            bytes[offset++] = 0;
            bytes[offset++] = 1;
            copy_bytes(bytes + offset, unit.bytes.data + span.offset, span.bytes);
            offset += span.bytes;
        }
        if (SUCCEEDED(result) && offset != picture.bitstream_bytes)
            result = E_INVALIDARG;
        if (SUCCEEDED(result)) {
            const u32 padding = static_cast<u32>(aligned) - offset;
            clear_bytes(bytes + offset, padding);
            slices[picture.slice_count - 1].SliceBytesInBuffer += padding;
            buffers[buffer_count].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
            buffers[buffer_count++].DataSize = static_cast<UINT>(aligned);
        }
    }
    // Release the compressed buffer before mapping the separate slice buffer.
    // Drivers may reuse upload memory between decoder buffer types.
    const HRESULT released = MAICORD_HEVC_PROBE_TIME(
        s, probe_release_bitstream,
        s.context->ReleaseDecoderBuffer(s.decoder, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM));
    if (SUCCEEDED(result))
        result = released;
    if (SUCCEEDED(result)) {
        result = upload(s, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, slices, static_cast<u32>(slice_bytes),
                        &buffers[buffer_count]);
        ++buffer_count;
    }
    if (SUCCEEDED(result))
        result = MAICORD_HEVC_PROBE_TIME(s, probe_submit,
                                         s.context->SubmitDecoderBuffers(s.decoder, buffer_count, buffers));
    return result;
}
bool reset(H264GpuDecoder& handle) noexcept {
    auto& s = *static_cast<State*>(handle.state);
    auto cursor = s.demux;
    mp4_demux_reset(&cursor);
    if (!cursor.ready)
        return false;
    s.demux = cursor;
    s.references = {};
    s.output = {};
    handle.submitted = false;
    return true;
}
Mp4DecodedFrame output_frame(State& s) noexcept {
    H264Output picture{};
    Mp4DecodedFrame frame{};
    if (!h264_output_pop(&s.output, &picture)) {
        frame.error = Mp4DecodeError::sample;
        return frame;
    }
    s.frame.subresource = picture.surface;
    frame.gpu = &s.frame;
    frame.width = s.frame.width;
    frame.height = s.frame.height;
    frame.display_width = s.display.width;
    frame.display_height = s.display.height;
    frame.duration_us = frame.source_duration_us = picture.duration_us;
    frame.presentation_time_us = picture.presentation_time_us;
    return frame;
}
} // namespace

bool hevc_gpu_decoder_initialize(H264GpuDecoder* handle, ConstSlice<u8> encoded, ID3D11Device* device,
                                 ID3D11DeviceContext* context, bool full_stream,
                                 Mp4SourceAvailability availability) noexcept {
    if (!handle || !device || !context || !encoded.data || !encoded.count)
        return false;
    hevc_gpu_decoder_release(handle);
    // A progressive source is borrowed in place; its owner outlives release.
    const bool borrowed = availability.available != nullptr;
    if (encoded.count > (borrowed ? kMediaMaximumProgressiveVideoBytes : kMp4DemuxMaximumEncodedBytes)) {
        handle->status = E_INVALIDARG;
        handle->demux_error = Mp4DemuxError::unsupported;
        handle->parse_error = H264Error::unsupported;
        return false;
    }
    constexpr u32 offset = (sizeof(State) + 15u) & ~15u;
    auto* s = static_cast<State*>(VirtualAlloc(nullptr, u64{offset} + (borrowed ? 0 : encoded.count),
                                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!s) {
        handle->status = E_OUTOFMEMORY;
        return false;
    }
    handle->state = s;
    s->full_stream = full_stream;
    s->output.leased = 255;
    const u8* owned = encoded.data;
    if (!borrowed) {
        auto* copy = reinterpret_cast<u8*>(s) + offset;
        copy_bytes(copy, encoded.data, encoded.count);
        owned = copy;
    }
    handle->demux_error = hevc_demux_initialize(&s->demux, {owned, encoded.count}, availability);
    HRESULT result = handle->demux_error == Mp4DemuxError::none ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(result)) {
        auto cursor = s->demux;
        const auto unit = mp4_demux_next_ready(&cursor);
        u8 pps{};
        auto error = unit.ok() ? hevc_picture_parameter_id(unit.bytes, s->demux.nal_length_bytes, &pps)
                               : HevcError::malformed;
        if (error == HevcError::none)
            error = hevc_select_parameters(hevc_demux_hvcc(s->demux), pps, &s->config);
        if (error == HevcError::none)
            error = hevc_parse_picture(unit.bytes, s->config, &s->picture);
        if (error == HevcError::none && !s->picture.irap)
            error = HevcError::unsupported;
        handle->parse_error = mapped(error);
        if (error != HevcError::none)
            result = E_INVALIDARG;
    }
    const auto& a = s->config.sps;
    const auto surface_extent = hevc_gpu_surface_extent(a.coded_width, a.coded_height);
    if (SUCCEEDED(result) &&
        (a.profile != 1 || !surface_extent.width || !a.max_buffering || a.max_buffering > 15 ||
         a.max_reorder > 15 || a.log2_min_cb_minus3 > 3 || !a.width || !a.height ||
         a.crop_x > a.coded_width || a.width > a.coded_width - a.crop_x || a.crop_y > a.coded_height ||
         a.height > a.coded_height - a.crop_y ||
         (a.matrix != 1 && a.matrix != 2 && a.matrix != 5 && a.matrix != 6))) {
        handle->parse_error = H264Error::unsupported;
        result = E_INVALIDARG;
    }
    s->display = video_display_rotated(video_display_extent(a.width, a.height, a.sar_width, a.sar_height),
                                       s->demux.rotation);
    if (SUCCEEDED(result) && !s->display.width) {
        handle->parse_error = H264Error::unsupported;
        result = E_INVALIDARG;
    }
    if (SUCCEEDED(result))
        result = device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&s->device));
    if (SUCCEEDED(result))
        result =
            context->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(&s->context));
    if (SUCCEEDED(result))
        result = context->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&s->guard));
    D3D11_VIDEO_DECODER_DESC desc{D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, a.coded_width, a.coded_height,
                                  DXGI_FORMAT_NV12};
    D3D11_VIDEO_DECODER_CONFIG selected{};
    if (SUCCEEDED(result)) {
        BOOL supported{};
        result = s->device->CheckVideoDecoderFormat(&desc.Guid, DXGI_FORMAT_NV12, &supported);
        if (SUCCEEDED(result) && !supported)
            result = DXGI_ERROR_UNSUPPORTED;
    }
    if (SUCCEEDED(result)) {
        UINT count{};
        result = s->device->GetVideoDecoderConfigCount(&desc, &count);
        bool found{};
        for (UINT i = 0; SUCCEEDED(result) && i < count; ++i) {
            D3D11_VIDEO_DECODER_CONFIG candidate{};
            result = s->device->GetVideoDecoderConfig(&desc, i, &candidate);
            if (SUCCEEDED(result) && candidate.ConfigBitstreamRaw == 1 &&
                bytes_equal(&candidate.guidConfigBitstreamEncryption, &DXVA_NoEncrypt, sizeof(GUID))) {
                selected = candidate;
                found = true;
                break;
            }
        }
        if (SUCCEEDED(result) && !found)
            result = DXGI_ERROR_UNSUPPORTED;
    }
    if (SUCCEEDED(result))
        result = s->device->CreateVideoDecoder(&desc, &selected, &s->decoder);
    if (SUCCEEDED(result)) {
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = surface_extent.width;
        texture.Height = surface_extent.height;
        texture.ArraySize =
            maximum<u32>(selected.ConfigMinRenderTargetBuffCount, maximum<u32>(a.max_buffering, 1) + 2);
        texture.MipLevels = 1;
        texture.Format = DXGI_FORMAT_NV12;
        texture.SampleDesc.Count = 1;
        texture.BindFlags = D3D11_BIND_DECODER;
        if (texture.ArraySize > 32)
            result = DXGI_ERROR_UNSUPPORTED;
        else
            result = device->CreateTexture2D(&texture, nullptr, &s->texture);
        s->surface_count = static_cast<u8>(texture.ArraySize);
    }
    if (SUCCEEDED(result)) {
        D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC view{};
        view.DecodeProfile = desc.Guid;
        view.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
        const u32 count = full_stream ? s->surface_count : 1;
        for (u32 i = 0; i < count && SUCCEEDED(result); ++i) {
            view.Texture2D.ArraySlice = i;
            result = s->device->CreateVideoDecoderOutputView(s->texture, &view, &s->views[i]);
        }
    }
    handle->status = result;
    if (FAILED(result)) {
        const auto parse = handle->parse_error;
        const auto demux = handle->demux_error;
        hevc_gpu_decoder_release(handle);
        handle->status = result;
        handle->parse_error = parse;
        handle->demux_error = demux;
        return false;
    }
    s->frame = {s->texture, 0, a.crop_x, a.crop_y, a.width, a.height, a.matrix == 1, a.full_range,
                s->demux.rotation, {}};
    return true;
}

Mp4DecodedFrame hevc_gpu_decoder_read(H264GpuDecoder* handle, bool loop, u32 budget) noexcept {
    Mp4DecodedFrame frame{};
    frame.error = Mp4DecodeError::invalid_input;
    if (!handle || !handle->state)
        return frame;
    auto& s = *static_cast<State*>(handle->state);
    if (budget && s.output.leased != 255)
        return frame;
    if (!s.full_stream && handle->submitted) {
        frame.error = Mp4DecodeError::end_of_stream;
        return frame;
    }
    s.output.leased = 255;
    u32 submissions{};
    bool restarted{};
    for (;;) {
        if (s.output.count &&
            (!s.full_stream || h264_output_ready(s.output, hevc_reference_surfaces(s.references),
                                                 s.config.sps.max_reorder, s.config.sps.max_buffering))) {
            handle->submitted = true;
            return output_frame(s);
        }
        auto next = s.demux;
        const auto unit = mp4_demux_next(&next);
        if (unit.error == Mp4DemuxError::end_of_stream) {
            if (s.output.count) {
                handle->submitted = true;
                return output_frame(s);
            }
            if (loop && !restarted && s.full_stream && reset(*handle)) {
                restarted = true;
                continue;
            }
            frame.error = Mp4DecodeError::end_of_stream;
            return frame;
        }
        if (unit.ok() && !mp4_demux_available(s.demux, unit.bytes)) {
            frame.error = Mp4DecodeError::data_pending;
            return frame;
        }
        if (budget && submissions >= budget) {
            frame.error = Mp4DecodeError::needs_service;
            return frame;
        }
        HevcPicture picture{};
        const auto error =
            unit.ok() ? hevc_parse_picture(unit.bytes, s.config, &picture) : HevcError::malformed;
        handle->parse_error = mapped(error);
        if (error != HevcError::none) {
            handle->status = E_INVALIDARG;
            frame.error = Mp4DecodeError::stream;
            return frame;
        }
        if (picture.irap && s.output.count) {
            if (picture.no_output_prior)
                s.output = {};
            else {
                handle->submitted = true;
                return output_frame(s);
            }
        }
        const u8 surface =
            h264_free_surface(s.output, picture.idr ? 0 : hevc_reference_surfaces(s.references),
                              s.full_stream ? s.surface_count : 1);
        HevcReferencePlan plan{};
        handle->parse_error =
            mapped(hevc_reference_plan(s.references, s.config.sps, picture, surface, &plan));
        if (handle->parse_error != H264Error::none) {
            handle->status = E_INVALIDARG;
            frame.error = Mp4DecodeError::stream;
            return frame;
        }
        MAICORD_HEVC_PROBE_TIME(s, probe_guard, s.guard->Enter());
        HRESULT result = MAICORD_HEVC_PROBE_TIME(
            s, probe_begin, s.context->DecoderBeginFrame(s.decoder, s.views[surface], 0, nullptr));
        if (SUCCEEDED(result)) {
            result = submit(s, unit, picture, plan, surface);
            const HRESULT end =
                MAICORD_HEVC_PROBE_TIME(s, probe_end, s.context->DecoderEndFrame(s.decoder));
            if (SUCCEEDED(result))
                result = end;
        }
        s.guard->Leave();
        handle->status = result;
#if defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
        if (FAILED(result)) {
            ++s.probe_driver_errors;
            if (h264_gpu_decoder_busy(*handle))
                ++s.probe_busy_errors;
        }
#endif
        if (FAILED(result)) {
            frame.error = Mp4DecodeError::sample;
            return frame;
        }
        s.picture = picture;
        s.demux = next;
        s.references = plan.after;
        if (++s.feedback == 0)
            ++s.feedback;
        ++submissions;
        if (picture.output && !h264_output_push(&s.output, {unit.duration_us, plan.poc, surface,
                                                            unit.presentation_time_us_signed})) {
            handle->status = E_UNEXPECTED;
            frame.error = Mp4DecodeError::sample;
            return frame;
        }
    }
}

bool hevc_gpu_decoder_acknowledge_frame(H264GpuDecoder* handle) noexcept {
    if (!handle || !handle->state)
        return false;
    static_cast<State*>(handle->state)->output.leased = 255;
    return true;
}
ConstSlice<u8> hevc_gpu_decoder_source(const H264GpuDecoder& handle) noexcept {
    const auto* s = static_cast<const State*>(handle.state);
    return s ? s->demux.encoded : ConstSlice<u8>{};
}
H264GpuMovie hevc_gpu_decoder_movie(const H264GpuDecoder& handle) noexcept {
    const auto* s = static_cast<const State*>(handle.state);
    if (!s || !s->demux.ready)
        return {};
    auto cursor = s->demux;
    mp4_demux_reset(&cursor);
    const auto first = mp4_demux_next(&cursor);
    return {s->demux.duration_us, s->demux.sample_count, s->demux.has_edit_list,
            first.presentation_time_us_signed, first.ok()};
}
H264GpuDiagnostic hevc_gpu_decoder_diagnostic(const H264GpuDecoder& handle) noexcept {
    const auto* s = static_cast<const State*>(handle.state);
    if (!s)
        return {};
    auto cursor = s->demux;
    const auto next = mp4_demux_next(&cursor);
    return {s->demux.sample_index, next.presentation_time_us_signed, next.decode_time_us, s->output.count,
            next.ok()};
}
Mp4DemuxError hevc_gpu_decoder_seek(H264GpuDecoder* handle, i64 target, Mp4SeekPoint* point) noexcept {
    if (!handle || !handle->state || !point)
        return Mp4DemuxError::invalid_input;
    auto& s = *static_cast<State*>(handle->state);
    const ConstSlice<u8> out{reinterpret_cast<const u8*>(point), sizeof(*point)};
    if (!s.full_stream || s.output.leased != 255 || !byte_range_valid(out) ||
        byte_ranges_overlap(out, {reinterpret_cast<const u8*>(handle), sizeof(*handle)}) ||
        byte_ranges_overlap(out, {reinterpret_cast<const u8*>(&s), sizeof(s)}) ||
        byte_ranges_overlap(out, s.demux.encoded))
        return Mp4DemuxError::invalid_input;
    auto next = s.demux;
    Mp4SeekPoint selected{};
    const auto result = hevc_demux_seek_sync(&next, target, &selected);
    if (result != Mp4DemuxError::none)
        return result;
    auto probe = next;
    const auto unit = mp4_demux_next_ready(&probe);
    if (unit.error == Mp4DemuxError::data_pending)
        return unit.error;
    HevcPicture picture{};
    if (!unit.ok() || hevc_parse_picture(unit.bytes, s.config, &picture) != HevcError::none ||
        !picture.irap)
        return Mp4DemuxError::unsupported;
    s.demux = next;
    s.references = {};
    s.output = {};
    handle->submitted = false;
    handle->status = S_OK;
    handle->parse_error = H264Error::none;
    *point = selected;
    return result;
}
bool hevc_gpu_decoder_seek_start(H264GpuDecoder* handle) noexcept {
    if (!handle || !handle->state || static_cast<State*>(handle->state)->output.leased != 255)
        return false;
    if (!reset(*handle))
        return false;
    handle->status = S_OK;
    handle->parse_error = H264Error::none;
    return true;
}
void hevc_gpu_decoder_release(H264GpuDecoder* handle) noexcept {
    if (!handle)
        return;
    if (auto* s = static_cast<State*>(handle->state)) {
        for (auto*& view : s->views)
            com_release(view);
        com_release(s->texture);
        com_release(s->decoder);
        gpu_decoder_fence_release(&s->wait);
        com_release(s->guard);
        com_release(s->context);
        com_release(s->device);
        VirtualFree(s, 0, MEM_RELEASE);
    }
    *handle = {};
}
bool hevc_gpu_decoder_busy(const H264GpuDecoder& handle) noexcept {
    return h264_gpu_decoder_busy(handle);
}
bool hevc_gpu_decoder_wait(H264GpuDecoder* handle, u32 timeout) noexcept {
    if (!handle || !handle->state || !timeout || timeout > 16)
        return false;
    auto& s = *static_cast<State*>(handle->state);
    const HRESULT result = gpu_decoder_fence_wait(&s.wait, s.context, s.guard, timeout);
    if (FAILED(result))
        handle->status = result;
    return SUCCEEDED(result);
}
#if defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
void hevc_gpu_decoder_probe_report(const H264GpuDecoder& handle) noexcept {
    const auto* state = static_cast<const State*>(handle.state);
    if (!state)
        return;
    constexpr const char* phases[]{"guard",      "begin",          "get_picture",   "release_picture",
                                   "get_matrix", "release_matrix", "get_bitstream", "release_bitstream",
                                   "get_slices", "release_slices", "submit",        "end"};
    static_assert(sizeof(phases) / sizeof(phases[0]) == probe_phase_count);
    for (u32 i = 0; i < probe_phase_count; ++i) {
        const auto& timing = state->probe_timing[i];
        probe_phase_metric(phases[i], "_calls", timing.calls);
        probe_phase_metric(phases[i], "_total_ms", timing.total_ms);
        probe_phase_metric(phases[i], "_max_ms", timing.max_ms);
        probe_phase_metric(phases[i], "_first_slow_sample_plus_one", timing.first_slow_sample_plus_one);
    }
    const auto& sps = state->config.sps;
    const auto surface = hevc_gpu_surface_extent(sps.coded_width, sps.coded_height);
    probe_metric("hevc_sps_coded_width", sps.coded_width);
    probe_metric("hevc_sps_coded_height", sps.coded_height);
    probe_metric("hevc_surface_width", surface.width);
    probe_metric("hevc_surface_height", surface.height);
    probe_metric("hevc_min_cb_size", u64{1} << (sps.log2_min_cb_minus3 + 3));
    probe_metric("hevc_scaling_enabled", sps.scaling_enabled);
    probe_metric("hevc_surface_count", state->surface_count);
    probe_metric("hevc_max_reorder", sps.max_reorder);
    probe_metric("hevc_max_buffering", sps.max_buffering);
    probe_metric("hevc_profile", sps.profile);
    probe_metric("hevc_profile_guid_data1", D3D11_DECODER_PROFILE_HEVC_VLD_MAIN.Data1);
    probe_metric("hevc_busy_errors", state->probe_busy_errors);
    probe_metric("hevc_driver_errors", state->probe_driver_errors);
    probe_metric("hevc_bitstream_last_capacity", state->probe_bitstream_capacity);
    probe_metric("hevc_slices_last_capacity", state->probe_slice_capacity);
    probe_metric("hevc_mapping_address_reuse", state->probe_mapping_reuse);
}
#endif
#undef MAICORD_HEVC_PROBE_TIME
} // namespace maicord
