#include "../../support/test_main_shared.hpp"

#include "maicord/core/hash.hpp"
#include "maicord/media/image/apng.hpp"
#include "maicord/media/image/jpeg.hpp"
#include "maicord/media/image/dominant_color.hpp"
#include "../../support/media_fixture.hpp"
#include "../../fixtures/jpeg_reference_fixture.hpp"
#include "../../fixtures/jpeg_progressive_fixture.hpp"
#include "../../fixtures/jpeg_cmyk_fixture.hpp"
#include "../../fixtures/webp_fixture.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/core/byte_order.hpp"

namespace maicord {
u8 g_png_encoded[17'000];
u8 g_png_zlib[17'000];
alignas(u32) u8 g_png_scanlines[17'000];
Vp8lWorkspace g_vp8l_workspace{};
LossyWebpWorkspace g_lossy_webp_workspace{};
JpegWorkspace g_jpeg_workspace{};

namespace {

Vp8ModeWorkspace g_vp8_mode_workspace{};
Vp8CoefficientWorkspace g_vp8_coefficient_workspace{};
alignas(u32) u8 g_webp_rgba[16 * 16 * 4];

} // namespace

u32 test_find_png_chunk(ConstSlice<u8> encoded, u32 type, u32 occurrence) noexcept {
    u32 cursor = 8;
    while (cursor <= encoded.count && encoded.count - cursor >= 12) {
        const u32 length = byte_load_be32(encoded.data + cursor);
        if (length > encoded.count - cursor - 12) {
            return 0xFFFFFFFFu;
        }
        if (byte_load_be32(encoded.data + cursor + 4) == type) {
            if (occurrence == 0) {
                return cursor;
            }
            --occurrence;
        }
        cursor += length + 12;
    }
    return 0xFFFFFFFFu;
}

bool test_rewrite_png_chunk_crc(Slice<u8> encoded, u32 chunk_offset) noexcept {
    if (!encoded.data || chunk_offset > encoded.count || encoded.count - chunk_offset < 12) {
        return false;
    }
    const u32 length = byte_load_be32(encoded.data + chunk_offset);
    if (length > encoded.count - chunk_offset - 12) {
        return false;
    }
    png_fixture_write_be32(encoded.data + chunk_offset + 8 + length,
                           png_fixture_crc32(ConstSlice<u8>{encoded.data + chunk_offset + 4, length + 4}));
    return true;
}

void test_zlib_deflate() noexcept {
    constexpr u8 stored[] = {
        0x78, 0x01, 0x01, 0x14, 0x00, 0xEB, 0xFF, 0x73, 0x74, 0x6F, 0x72, 0x65, 0x64, 0x20, 0x62, 0x6C,
        0x6F, 0x63, 0x6B, 0x20, 0x70, 0x61, 0x79, 0x6C, 0x6F, 0x61, 0x64, 0x52, 0x3E, 0x07, 0xC7,
    };
    u8 output[1'300]{};
    DeflateWorkspace workspace{};
    const DeflateInputSpan stored_span{stored, sizeof(stored)};

    u8 corrupt[sizeof(stored) + 1]{};
    for (u32 index = 0; index < sizeof(stored); ++index) {
        corrupt[index] = stored[index];
    }
    corrupt[sizeof(stored) - 1] ^= 1;
    const DeflateInputSpan corrupt_span{corrupt, sizeof(stored)};
    expect(zlib_inflate(ConstSlice<DeflateInputSpan>{&corrupt_span, 1}, Slice<u8>{output, sizeof(output)},
                        20, &workspace)
                   .error == ZlibInflateError::checksum,
           "zlib_adler_reject");
    corrupt[sizeof(stored) - 1] ^= 1;
    corrupt[sizeof(stored)] = 0;
    const DeflateInputSpan trailing_span{corrupt, sizeof(corrupt)};
    const DeflateInputSpan truncated_span{stored, sizeof(stored) - 1};
    constexpr u8 dictionary_header[] = {0x78, 0x20};
    const DeflateInputSpan dictionary_span{dictionary_header, sizeof(dictionary_header)};
    constexpr u8 reserved_block[] = {0x78, 0x01, 0x07, 0, 0, 0, 1};
    const DeflateInputSpan reserved_span{reserved_block, sizeof(reserved_block)};
    expect(zlib_inflate(ConstSlice<DeflateInputSpan>{&trailing_span, 1}, Slice<u8>{output, sizeof(output)},
                        20, &workspace)
                       .error == ZlibInflateError::trailing_data &&
               zlib_inflate(ConstSlice<DeflateInputSpan>{&truncated_span, 1},
                            Slice<u8>{output, sizeof(output)}, 20, &workspace)
                       .error == ZlibInflateError::truncated &&
               zlib_inflate(ConstSlice<DeflateInputSpan>{&dictionary_span, 1},
                            Slice<u8>{output, sizeof(output)}, 0, &workspace)
                       .error == ZlibInflateError::unsupported_dictionary &&
               zlib_inflate(ConstSlice<DeflateInputSpan>{&reserved_span, 1},
                            Slice<u8>{output, sizeof(output)}, 0, &workspace)
                       .error == ZlibInflateError::invalid_block &&
               zlib_inflate(ConstSlice<DeflateInputSpan>{&stored_span, 1}, Slice<u8>{output, 19}, 20,
                            &workspace)
                       .error == ZlibInflateError::output_capacity &&
               zlib_inflate(ConstSlice<DeflateInputSpan>{&stored_span, 1},
                            Slice<u8>{output, sizeof(output)}, 19, &workspace)
                       .error == ZlibInflateError::output_size,
           "zlib_structural_bounds_and_errors");
}

void test_jpeg_baseline() noexcept {
    u8 encoded[256]{};
    JpegFixtureResult fixture{};
    expect(build_baseline_jpeg_fixture(Slice<u8>{encoded, sizeof(encoded)}, false, &fixture),
           "jpeg_baseline_fixture_build");
    JpegImage image{};
    const JpegDecodeResult decoded =
        jpeg_decode_coefficients(ConstSlice<u8>{encoded, fixture.encoded_bytes}, &image, &g_jpeg_workspace);
    expect(decoded.ok() && decoded.parse.segments == 5 &&
               decoded.decoded_blocks == 3 && image.width == 8 &&
               image.height == 8 && image.component_count == 3 && image.coefficient_blocks == 3 &&
               image.has_jfif && image.color_transform == JpegColorTransform::ycbcr &&
               image.orientation == JpegOrientation::top_left && image.tables->icc &&
               image.tables->icc->profile.disposition == JpegIccDisposition::none &&
               jpeg_coefficient_block(g_jpeg_workspace.coefficients, 3, 0).values[0] == 0 &&
               jpeg_coefficient_block(g_jpeg_workspace.coefficients, 3, 1).values[0] == -256 &&
               jpeg_coefficient_block(g_jpeg_workspace.coefficients, 3, 2).values[0] == 256,
           "jpeg_baseline_parse_and_entropy_exact");

    u8 oriented_encoded[sizeof(kJpeg420Reference) + 36]{};
    for (u8 orientation = 1; orientation <= 8; ++orientation) {
        u32 oriented_bytes = 0;
        JpegImage oriented_image{};
        const bool built = build_jpeg_exif_orientation_fixture(
            ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)},
            Slice<u8>{oriented_encoded, sizeof(oriented_encoded)}, orientation, (orientation & 1u) == 0,
            &oriented_bytes);
        const JpegDecodeResult oriented =
            built ? jpeg_decode_coefficients(ConstSlice<u8>{oriented_encoded, oriented_bytes},
                                             &oriented_image, &g_jpeg_workspace)
                  : JpegDecodeResult{};
        expect(built && oriented_bytes == sizeof(kJpeg420Reference) + 36 && oriented.ok() &&
                   oriented_image.orientation == static_cast<JpegOrientation>(orientation) &&
                   jpeg_display_width(oriented_image) == 16 && jpeg_display_height(oriented_image) == 16,
               "jpeg_exif_orientation_all_values_and_endianness");
    }

    u8 rectangular_jpeg[256]{};
    u8 rectangular_oriented[320]{};
    JpegFixtureResult rectangular_fixture{};
    u32 rectangular_oriented_bytes = 0;
    JpegImage rectangular_image{};
    expect(build_baseline_jpeg_fixture(Slice<u8>{rectangular_jpeg, sizeof(rectangular_jpeg)}, true,
                                       &rectangular_fixture) &&
               build_jpeg_exif_orientation_fixture(
                   ConstSlice<u8>{rectangular_jpeg, rectangular_fixture.encoded_bytes},
                   Slice<u8>{rectangular_oriented, sizeof(rectangular_oriented)},
                   static_cast<u8>(JpegOrientation::right_top), false, &rectangular_oriented_bytes) &&
               jpeg_decode_coefficients(ConstSlice<u8>{rectangular_oriented, rectangular_oriented_bytes},
                                        &rectangular_image, &g_jpeg_workspace)
                   .ok() &&
               rectangular_image.width == 16 && rectangular_image.height == 8 &&
               jpeg_display_width(rectangular_image) == 8 && jpeg_display_height(rectangular_image) == 16,
           "jpeg_exif_orientation_swaps_display_dimensions");

    // A malformed or reserved EXIF orientation is ignored, not an error.
    const auto orientation_ignored = [&](auto&& damage, const char* name) noexcept {
        u32 bytes = 0;
        JpegImage damaged{};
        const bool built = build_jpeg_exif_orientation_fixture(
            ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)},
            Slice<u8>{oriented_encoded, sizeof(oriented_encoded)}, static_cast<u8>(JpegOrientation::right_top), false,
            &bytes);
        damage();
        expect(built &&
                   jpeg_decode_coefficients(ConstSlice<u8>{oriented_encoded, bytes}, &damaged, &g_jpeg_workspace).ok() &&
                   damaged.orientation == JpegOrientation::top_left,
               name);
    };
    orientation_ignored(
        [&] {
            for (u32 index = 16; index < 20; ++index)
                oriented_encoded[index] = 0xFF;
        },
        "jpeg_malformed_exif_offset_ignored_safely");
    orientation_ignored([&] { oriented_encoded[30] = 9; }, "jpeg_reserved_exif_orientation_ignored");

    u8 icc_encoded[sizeof(kJpeg420Reference) + 1'024]{};
    u8 icc_lut_encoded[sizeof(kJpegCmykAdobeReference) + 4'096]{};
    JpegIccFixtureResult icc_fixture{};
    JpegImage icc_image{};
    const Slice<u8> icc_buffer{icc_encoded, sizeof(icc_encoded)};
    const Slice<u8> icc_lut_buffer{icc_lut_encoded, sizeof(icc_lut_encoded)};
    const auto matrix_fixture = [&](JpegIccFixtureCurve curve, bool version_four, bool reverse_chunks) noexcept {
        return build_jpeg_icc_matrix_trc_fixture(ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)},
                                                 icc_buffer, curve, version_four, reverse_chunks, &icc_fixture);
    };
    const auto lut_fixture = [&](JpegIccLutFixtureSpace space, JpegIccLutFixturePrecision precision,
                                 bool reverse_chunks) noexcept {
        const bool cmyk = space == JpegIccLutFixtureSpace::cmyk;
        return build_jpeg_icc_lut_fixture(
            cmyk ? ConstSlice<u8>{kJpegCmykAdobeReference, sizeof(kJpegCmykAdobeReference)}
                 : ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)},
            icc_lut_buffer, space, precision, reverse_chunks, &icc_fixture);
    };
    // The fixture just built, decoded from the buffer holding it.
    const auto decode = [&](const u8* bytes) noexcept {
        return jpeg_decode_coefficients(ConstSlice<u8>{bytes, icc_fixture.encoded_bytes}, &icc_image,
                                        &g_jpeg_workspace);
    };
    const auto profile = [&]() noexcept {
        return icc_image.tables && icc_image.tables->icc ? &icc_image.tables->icc->profile : nullptr;
    };
    // The profile's disposition; none when the decode failed.
    const auto disposition = [&](const u8* bytes) noexcept {
        return decode(bytes).ok() && profile() ? profile()->disposition : JpegIccDisposition::none;
    };
    const auto patch = [&](Slice<u8> bytes, u32 profile_offset, u8 value) noexcept {
        return jpeg_icc_fixture_write_profile_byte(bytes, icc_fixture, profile_offset, value);
    };
    expect(matrix_fixture(JpegIccFixtureCurve::parametric_srgb, true, true),
           "jpeg_icc_v4_reverse_chunk_fixture_build");
    const JpegDecodeResult icc_decoded = decode(icc_encoded);
    const JpegIccProfile* icc_profile = profile();
    expect(icc_decoded.ok() && icc_profile && icc_profile->disposition == JpegIccDisposition::applied &&
               icc_profile->major_version == 4 && icc_profile->chunk_count == 2 &&
               icc_profile->profile_bytes == 384 && icc_profile->entry_count == 0 &&
               icc_profile->transform_kind == JpegIccTransformKind::matrix_trc &&
               icc_profile->pcs == JpegIccPcs::xyz &&
               icc_profile->data_color_space == JpegIccDataColorSpace::rgb &&
               icc_profile->curves[0].kind == JpegIccCurveKind::parametric &&
               icc_profile->curves[1].kind == JpegIccCurveKind::parametric &&
               icc_profile->curves[2].kind == JpegIccCurveKind::parametric &&
               icc_profile->to_linear_srgb[0] > -0.001f && icc_profile->to_linear_srgb[0] < 0.001f &&
               icc_profile->to_linear_srgb[2] > 0.999f && icc_profile->to_linear_srgb[2] < 1.001f &&
               icc_profile->to_linear_srgb[4] > 0.999f && icc_profile->to_linear_srgb[4] < 1.001f &&
               icc_profile->to_linear_srgb[6] > 0.999f && icc_profile->to_linear_srgb[6] < 1.001f,
           "jpeg_icc_v4_parametric_scatter_matrix_exact");

    for (u8 function = 0; function <= 4; ++function) {
        bool built = matrix_fixture(JpegIccFixtureCurve::parametric_srgb, true, false);
        constexpr u32 function_offsets[3] = {273, 313, 353};
        for (u32 channel = 0; channel < 3 && built; ++channel) {
            built = patch(icc_buffer, function_offsets[channel], function);
        }
        expect(built && disposition(icc_encoded) == JpegIccDisposition::applied &&
                   profile()->curves[0].kind == JpegIccCurveKind::parametric,
               "jpeg_icc_parametric_function_zero_through_four");
    }

    expect(matrix_fixture(JpegIccFixtureCurve::sampled_srgb, false, false), "jpeg_icc_v2_table_fixture_build");
    const JpegDecodeResult icc_table_decoded = decode(icc_encoded);
    icc_profile = profile();
    expect(icc_table_decoded.ok() && icc_profile &&
               icc_profile->disposition == JpegIccDisposition::applied && icc_profile->major_version == 2 &&
               icc_profile->profile_bytes == 696 && icc_profile->entry_count == 195 &&
               icc_profile->curves[0].kind == JpegIccCurveKind::table &&
               icc_profile->curves[0].table_offset == 0 && icc_profile->curves[0].table_count == 65 &&
               icc_profile->curves[1].table_offset == 65 && icc_profile->curves[2].table_offset == 130 &&
               icc_profile->entries[0] == 0 && icc_profile->entries[64] == 0xFFFFu &&
               icc_profile->entries[194] == 0xFFFFu,
           "jpeg_icc_v2_curve_table_retained_bounded");

    expect(matrix_fixture(JpegIccFixtureCurve::sampled_srgb, false, false) && patch(icc_buffer, 276, 0xFF) &&
               patch(icc_buffer, 277, 0xFF) &&
               disposition(icc_encoded) == JpegIccDisposition::malformed_ignored,
           "jpeg_icc_descending_curve_rejected");

    constexpr u32 curve_count_low_offsets[3] = {275, 419, 563};
    bool identity_curve_fixture = matrix_fixture(JpegIccFixtureCurve::sampled_srgb, false, false);
    for (u32 channel = 0; channel < 3; ++channel) {
        identity_curve_fixture = identity_curve_fixture && patch(icc_buffer, curve_count_low_offsets[channel], 0);
    }
    expect(identity_curve_fixture && disposition(icc_encoded) == JpegIccDisposition::applied &&
               profile()->curves[0].kind == JpegIccCurveKind::identity,
           "jpeg_icc_zero_count_curve_is_identity");

    constexpr u32 gamma_high_offsets[3] = {276, 420, 564};
    bool gamma_curve_fixture = matrix_fixture(JpegIccFixtureCurve::sampled_srgb, false, false);
    for (u32 channel = 0; channel < 3; ++channel) {
        gamma_curve_fixture = gamma_curve_fixture && patch(icc_buffer, curve_count_low_offsets[channel], 1) &&
                              patch(icc_buffer, gamma_high_offsets[channel], 1);
    }
    expect(gamma_curve_fixture && disposition(icc_encoded) == JpegIccDisposition::applied &&
               profile()->curves[0].kind == JpegIccCurveKind::parametric && profile()->curves[0].g == 1.0f,
           "jpeg_icc_single_count_curve_is_u8fixed8_gamma");

    expect(matrix_fixture(JpegIccFixtureCurve::parametric_srgb, true, false), "jpeg_icc_incomplete_fixture_build");
    icc_encoded[icc_fixture.chunk_data_offsets[0] - 1u] = 3;
    icc_encoded[icc_fixture.chunk_data_offsets[1] - 1u] = 3;
    expect(disposition(icc_encoded) == JpegIccDisposition::incomplete_ignored, "jpeg_icc_incomplete_chunks_ignored");

    expect(matrix_fixture(JpegIccFixtureCurve::parametric_srgb, true, true), "jpeg_icc_duplicate_fixture_build");
    icc_encoded[icc_fixture.chunk_data_offsets[1] - 2u] = 1;
    expect(disposition(icc_encoded) == JpegIccDisposition::malformed_ignored, "jpeg_icc_duplicate_sequence_ignored");

    expect(matrix_fixture(JpegIccFixtureCurve::parametric_srgb, true, false) && patch(icc_buffer, 3, 0) &&
               disposition(icc_encoded) == JpegIccDisposition::malformed_ignored,
           "jpeg_icc_bad_declared_size_ignored");

    expect(matrix_fixture(JpegIccFixtureCurve::parametric_srgb, true, false) && patch(icc_buffer, 20, 'L') &&
               patch(icc_buffer, 21, 'a') && patch(icc_buffer, 22, 'b') &&
               disposition(icc_encoded) == JpegIccDisposition::unsupported_ignored,
           "jpeg_icc_unsupported_pcs_ignored");

    expect(lut_fixture(JpegIccLutFixtureSpace::rgb, JpegIccLutFixturePrecision::mft2, true),
           "jpeg_icc_mft2_rgb_reverse_chunk_fixture_build");
    const JpegDecodeResult icc_mft2_rgb = decode(icc_lut_encoded);
    icc_profile = profile();
    expect(icc_mft2_rgb.ok() && icc_profile && icc_profile->disposition == JpegIccDisposition::applied &&
               icc_profile->transform_kind == JpegIccTransformKind::lut &&
               icc_profile->pcs == JpegIccPcs::lab &&
               icc_profile->data_color_space == JpegIccDataColorSpace::rgb &&
               icc_profile->profile_bytes == 268 && icc_profile->entry_count == 36 &&
               icc_profile->lut.input_channels == 3 && icc_profile->lut.output_channels == 3 &&
               icc_profile->lut.precision_bytes == 2 && icc_profile->lut.grid_points[0] == 2 &&
               icc_profile->lut.grid_points[1] == 2 && icc_profile->lut.grid_points[2] == 2 &&
               icc_profile->lut.input_curves[0].table_offset == 0 &&
               icc_profile->lut.input_curves[2].table_offset == 4 && icc_profile->lut.clut_offset == 6 &&
               icc_profile->lut.clut_entry_count == 24 &&
               icc_profile->lut.output_curves[0].table_offset == 30 &&
               icc_profile->lut.output_curves[2].table_offset == 34,
           "jpeg_icc_mft2_rgb_scatter_retained_bounded");

    expect(lut_fixture(JpegIccLutFixtureSpace::rgb, JpegIccLutFixturePrecision::mft1, false),
           "jpeg_icc_mft1_rgb_fixture_build");
    const JpegDecodeResult icc_mft1_rgb = decode(icc_lut_encoded);
    icc_profile = profile();
    expect(icc_mft1_rgb.ok() && icc_profile && icc_profile->disposition == JpegIccDisposition::applied &&
               icc_profile->major_version == 2 && icc_profile->profile_bytes == 1'752 &&
               icc_profile->entry_count == 1'560 && icc_profile->lut.precision_bytes == 1 &&
               icc_profile->lut.input_curves[0].table_count == 256 && icc_profile->lut.clut_offset == 768 &&
               icc_profile->lut.clut_entry_count == 24 &&
               icc_profile->lut.output_curves[0].table_offset == 792,
           "jpeg_icc_mft1_fixed_tables_retained_bounded");

    expect(lut_fixture(JpegIccLutFixtureSpace::cmyk, JpegIccLutFixturePrecision::mft2, true),
           "jpeg_icc_mft2_cmyk_fixture_build");
    const JpegDecodeResult icc_mft2_cmyk = decode(icc_lut_encoded);
    icc_profile = profile();
    expect(icc_mft2_cmyk.ok() && icc_profile && icc_profile->disposition == JpegIccDisposition::applied &&
               icc_profile->data_color_space == JpegIccDataColorSpace::cmyk &&
               icc_image.color_transform == JpegColorTransform::cmyk && icc_profile->profile_bytes == 320 &&
               icc_profile->entry_count == 62 && icc_profile->lut.input_channels == 4 &&
               icc_profile->lut.grid_points[3] == 2 && icc_profile->lut.clut_offset == 8 &&
               icc_profile->lut.clut_entry_count == 48 &&
               icc_profile->lut.output_curves[0].table_offset == 56,
           "jpeg_icc_mft2_inverse_cmyk_retained_bounded");

    expect(lut_fixture(JpegIccLutFixtureSpace::rgb, JpegIccLutFixturePrecision::mft2, false) &&
               patch(icc_lut_buffer, 154, 17) &&
               disposition(icc_lut_encoded) == JpegIccDisposition::unsupported_ignored,
           "jpeg_icc_lut_grid_over_cap_rejected");

    expect(lut_fixture(JpegIccLutFixtureSpace::rgb, JpegIccLutFixturePrecision::mft2, false) &&
               patch(icc_lut_buffer, 157, 0) &&
               disposition(icc_lut_encoded) == JpegIccDisposition::malformed_ignored,
           "jpeg_icc_lut_nonidentity_matrix_rejected");

    expect(lut_fixture(JpegIccLutFixtureSpace::rgb, JpegIccLutFixturePrecision::mft1, false) &&
               patch(icc_lut_buffer, 20, 'X') && patch(icc_lut_buffer, 21, 'Y') &&
               patch(icc_lut_buffer, 22, 'Z') && patch(icc_lut_buffer, 23, ' ') &&
               disposition(icc_lut_encoded) == JpegIccDisposition::unsupported_ignored,
           "jpeg_icc_mft1_xyz_rejected_without_ambiguous_scaling");

    u8 gray_encoded[192]{};
    JpegFixtureResult gray_fixture{};
    const auto gray_icc_fixture = [&](bool pcs_lab, bool reverse_chunks) noexcept {
        return build_jpeg_icc_gray_trc_fixture(ConstSlice<u8>{gray_encoded, gray_fixture.encoded_bytes},
                                               icc_lut_buffer, pcs_lab, reverse_chunks, &icc_fixture);
    };
    expect(build_grayscale_jpeg_fixture(Slice<u8>{gray_encoded, sizeof(gray_encoded)}, &gray_fixture) &&
               gray_icc_fixture(true, true),
           "jpeg_icc_gray_lab_trc_fixture_build");
    const JpegDecodeResult icc_gray_lab = decode(icc_lut_encoded);
    icc_profile = profile();
    expect(icc_gray_lab.ok() && icc_profile && icc_image.component_count == 1 &&
               icc_image.color_transform == JpegColorTransform::grayscale &&
               icc_profile->disposition == JpegIccDisposition::applied &&
               icc_profile->transform_kind == JpegIccTransformKind::gray_trc &&
               icc_profile->pcs == JpegIccPcs::lab &&
               icc_profile->data_color_space == JpegIccDataColorSpace::gray &&
               icc_profile->profile_bytes == 160 && icc_profile->entry_count == 0 &&
               icc_profile->curves[0].kind == JpegIccCurveKind::parametric &&
               icc_profile->curves[0].g == 1.0f,
           "jpeg_icc_gray_lab_trc_scatter_applied");

    expect(gray_icc_fixture(false, false) && disposition(icc_lut_encoded) == JpegIccDisposition::applied &&
               profile()->transform_kind == JpegIccTransformKind::gray_trc && profile()->pcs == JpegIccPcs::xyz,
           "jpeg_icc_gray_xyz_trc_applied");

    JpegFixtureResult restarted_fixture{};
    expect(build_baseline_jpeg_fixture(Slice<u8>{encoded, sizeof(encoded)}, true, &restarted_fixture),
           "jpeg_restart_fixture_build");
    const JpegDecodeResult restarted = jpeg_decode_coefficients(
        ConstSlice<u8>{encoded, restarted_fixture.encoded_bytes}, &image, &g_jpeg_workspace);
    expect(restarted.ok() && restarted.parse.restart_markers == 1 &&
               restarted.decoded_blocks == 6 && image.restart_interval == 1 && image.width == 16 &&
               jpeg_coefficient_block(g_jpeg_workspace.coefficients, 6, 1).values[0] == 0 &&
               jpeg_coefficient_block(g_jpeg_workspace.coefficients, 6, 3).values[0] == -256 &&
               jpeg_coefficient_block(g_jpeg_workspace.coefficients, 6, 5).values[0] == 256,
           "jpeg_restart_predictor_and_sequence");

    const u32 restarted_bytes = restarted_fixture.encoded_bytes;
    expect(jpeg_parse(ConstSlice<u8>{encoded, restarted_bytes + 1}, &image, &g_jpeg_workspace).error ==
               JpegError::trailing_data,
           "jpeg_trailing_data_reject");

    expect(build_baseline_jpeg_fixture(Slice<u8>{encoded, sizeof(encoded)}, false, &fixture),
           "jpeg_malformed_fixture_rebuild");
    const JpegParseResult parsed =
        jpeg_parse(ConstSlice<u8>{encoded, fixture.encoded_bytes}, &image, &g_jpeg_workspace);
    expect(parsed.ok(), "jpeg_malformed_fixture_parse");
    encoded[image.entropy_offset + image.entropy_bytes - 1] ^= 1;
    const JpegDecodeResult bad_padding = jpeg_decode_parsed_coefficients(
        ConstSlice<u8>{encoded, fixture.encoded_bytes}, image, &g_jpeg_workspace.coefficients);
    expect(bad_padding.error == JpegError::entropy, "jpeg_entropy_padding_reject");

    // Whether any block of the last decode, of `blocks` blocks, has an AC coefficient.
    const auto has_ac_coefficient = [](u32 blocks) noexcept {
        for (u32 block = 0; block < blocks; ++block) {
            for (u32 coefficient = 1; coefficient < 64; ++coefficient) {
                if (jpeg_coefficient_block(g_jpeg_workspace.coefficients, blocks, block).values[coefficient] != 0) {
                    return true;
                }
            }
        }
        return false;
    };
    JpegImage progressive_image{};
    const ConstSlice<u8> progressive_bytes{reinterpret_cast<const u8*>(kJpegProgressive420Reference),
                                           kJpegProgressive420ReferenceBytes};
    const JpegDecodeResult progressive =
        jpeg_decode_coefficients(progressive_bytes, &progressive_image, &g_jpeg_workspace);
    expect(progressive.ok() && progressive_image.progressive && progressive.parse.scans == 10 &&
               progressive_image.scan_count == 10 && progressive.decoded_scans == 10 &&
               progressive.decoded_blocks == 32 &&
               progressive_image.width == 16 &&
               progressive_image.height == 16 && progressive_image.coefficient_blocks == 6 &&
               has_ac_coefficient(progressive_image.coefficient_blocks),
           "jpeg_progressive_spectral_and_refinement_decode");

    JpegImage progressive_edge_image{};
    const JpegDecodeResult progressive_edge = jpeg_decode_coefficients(
        ConstSlice<u8>{reinterpret_cast<const u8*>(kJpegProgressive420EdgeReference),
                       kJpegProgressive420EdgeReferenceBytes},
        &progressive_edge_image, &g_jpeg_workspace);
    expect(progressive_edge.ok() && progressive_edge_image.progressive &&
               progressive_edge_image.width == 17 && progressive_edge_image.height == 9 &&
               progressive_edge_image.coefficient_blocks == 12 &&
               progressive_edge_image.components[0].block_columns == 4 &&
               progressive_edge.decoded_scans == 10 && progressive_edge.decoded_blocks == 56,
           "jpeg_progressive_noninterleaved_edge_traversal");

    JpegImage progressive_restart_image{};
    const JpegDecodeResult progressive_restart = jpeg_decode_coefficients(
        ConstSlice<u8>{reinterpret_cast<const u8*>(kJpegProgressive420RestartReference),
                       kJpegProgressive420RestartReferenceBytes},
        &progressive_restart_image, &g_jpeg_workspace);
    expect(progressive_restart.ok() && progressive_restart_image.progressive &&
               progressive_restart_image.width == 32 && progressive_restart_image.height == 16 &&
               progressive_restart_image.restart_interval == 1 &&
               progressive_restart_image.restart_markers == 34 &&
               progressive_restart.parse.restart_markers == 34 && progressive_restart.decoded_scans == 10 &&
               progressive_restart.decoded_blocks == 64,
           "jpeg_progressive_restart_predictor_eob_and_sequence");

    u8 malformed_progressive[kJpegProgressive420ReferenceBytes]{};
    const auto restore_progressive = [&]() noexcept {
        for (u32 index = 0; index < sizeof(malformed_progressive); ++index) {
            malformed_progressive[index] = static_cast<u8>(kJpegProgressive420Reference[index]);
        }
    };
    const auto parse_progressive = [&]() noexcept {
        return jpeg_parse(ConstSlice<u8>{malformed_progressive, sizeof(malformed_progressive)}, &image,
                          &g_jpeg_workspace)
            .error;
    };
    restore_progressive();
    bool changed_scan = false;
    for (u32 index = 0; index + 12 < sizeof(malformed_progressive); ++index) {
        if (malformed_progressive[index] == 0xFF && malformed_progressive[index + 1] == 0xDA) {
            const u32 component_count = malformed_progressive[index + 4];
            malformed_progressive[index + 5 + component_count * 2] = 1;
            changed_scan = true;
            break;
        }
    }
    expect(changed_scan && parse_progressive() == JpegError::scan,
           "jpeg_progressive_interleaved_ac_reject");

    restore_progressive();
    bool changed_refinement = false;
    for (u32 index = 0; index + 12 < sizeof(malformed_progressive); ++index) {
        if (malformed_progressive[index] != 0xFF || malformed_progressive[index + 1] != 0xDA) {
            continue;
        }
        const u32 component_count = malformed_progressive[index + 4];
        const u32 approximation_offset = index + 7 + component_count * 2;
        const u8 successive_high = static_cast<u8>(malformed_progressive[approximation_offset] >> 4);
        if (successive_high != 0) {
            malformed_progressive[approximation_offset] =
                static_cast<u8>((successive_high << 4) | successive_high);
            changed_refinement = true;
            break;
        }
    }
    expect(changed_refinement && parse_progressive() == JpegError::scan,
           "jpeg_progressive_successive_refinement_order_reject");

    restore_progressive();
    bool changed_progressive_padding = false;
    for (u32 index = sizeof(malformed_progressive) - 2; index != 0; --index) {
        if (malformed_progressive[index] == 0xFF && malformed_progressive[index + 1] == 0xD9 && index > 0) {
            malformed_progressive[index - 1] ^= 1;
            changed_progressive_padding = true;
            break;
        }
    }
    expect(
        changed_progressive_padding &&
            jpeg_decode_coefficients(ConstSlice<u8>{malformed_progressive, sizeof(malformed_progressive)},
                                     &image, &g_jpeg_workspace)
                    .error == JpegError::entropy,
        "jpeg_progressive_entropy_padding_reject");

    expect(build_baseline_jpeg_fixture(Slice<u8>{encoded, sizeof(encoded)}, false, &fixture),
           "jpeg_huffman_fixture_rebuild");
    bool changed_huffman = false;
    for (u32 index = 0; index + 6 < fixture.encoded_bytes; ++index) {
        if (encoded[index] == 0xFF && encoded[index + 1] == 0xC4) {
            encoded[index + 5] = 2;
            encoded[index + 6] = 0;
            changed_huffman = true;
            break;
        }
    }
    expect(
        changed_huffman &&
            jpeg_parse(ConstSlice<u8>{encoded, fixture.encoded_bytes}, &image, &g_jpeg_workspace).error ==
                JpegError::huffman,
        "jpeg_all_ones_huffman_code_reject");

    expect(build_baseline_jpeg_fixture(Slice<u8>{encoded, sizeof(encoded)}, false, &fixture),
           "jpeg_symbol_fixture_rebuild");
    changed_huffman = false;
    for (u32 index = 0; index + 21 < fixture.encoded_bytes; ++index) {
        if (encoded[index] == 0xFF && encoded[index + 1] == 0xC4) {
            encoded[index + 21] = 12;
            changed_huffman = true;
            break;
        }
    }
    expect(
        changed_huffman &&
            jpeg_parse(ConstSlice<u8>{encoded, fixture.encoded_bytes}, &image, &g_jpeg_workspace).error ==
                JpegError::huffman,
        "jpeg_invalid_dc_symbol_reject");

    JpegImage reference_image{};
    const JpegDecodeResult reference = jpeg_decode_coefficients(
        ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)}, &reference_image, &g_jpeg_workspace);
    expect(reference.ok() && reference_image.width == 16 && reference_image.height == 16 &&
               reference_image.component_count == 3 && reference_image.maximum_horizontal_sampling == 2 &&
               reference_image.maximum_vertical_sampling == 2 &&
               reference_image.components[0].horizontal_sampling == 2 &&
               reference_image.components[0].vertical_sampling == 2 &&
               reference_image.components[1].horizontal_sampling == 1 &&
               reference_image.components[1].vertical_sampling == 1 &&
               reference_image.coefficient_blocks == 6 && reference.decoded_blocks == 6 &&
               has_ac_coefficient(reference_image.coefficient_blocks),
           "jpeg_real_420_custom_huffman_ac_coefficients");

    JpegImage cmyk_image{};
    const JpegDecodeResult cmyk =
        jpeg_decode_coefficients(ConstSlice<u8>{kJpegCmykAdobeReference, sizeof(kJpegCmykAdobeReference)},
                                 &cmyk_image, &g_jpeg_workspace);
    expect(cmyk.ok() && cmyk.parse.scans == 1 && cmyk.decoded_scans == 1 && !cmyk_image.progressive &&
               cmyk_image.width == 16 && cmyk_image.height == 16 && cmyk_image.component_count == 4 &&
               cmyk_image.coefficient_blocks == 16 && cmyk_image.has_adobe && !cmyk_image.has_jfif &&
               cmyk_image.color_transform == JpegColorTransform::cmyk &&
               has_ac_coefficient(cmyk_image.coefficient_blocks),
           "jpeg_adobe_cmyk_baseline_decode");

    u8 cmyk_without_adobe[sizeof(kJpegCmykAdobeReference) - 16]{};
    cmyk_without_adobe[0] = kJpegCmykAdobeReference[0];
    cmyk_without_adobe[1] = kJpegCmykAdobeReference[1];
    for (u32 index = 2; index < sizeof(cmyk_without_adobe); ++index) {
        cmyk_without_adobe[index] = kJpegCmykAdobeReference[index + 16];
    }
    JpegImage cmyk_without_adobe_image{};
    const JpegDecodeResult cmyk_without_adobe_decoded =
        jpeg_decode_coefficients(ConstSlice<u8>{cmyk_without_adobe, sizeof(cmyk_without_adobe)},
                                 &cmyk_without_adobe_image, &g_jpeg_workspace);
    expect(cmyk_without_adobe_decoded.ok() && !cmyk_without_adobe_image.has_adobe &&
               cmyk_without_adobe_image.component_count == 4 &&
               cmyk_without_adobe_image.color_transform == JpegColorTransform::cmyk,
           "jpeg_four_component_without_app14_chromium_compatibility");

    JpegImage ycck_image{};
    const JpegDecodeResult ycck = jpeg_decode_coefficients(
        ConstSlice<u8>{kJpegYcckProgressiveReference, sizeof(kJpegYcckProgressiveReference)}, &ycck_image,
        &g_jpeg_workspace);
    expect(ycck.ok() && ycck.parse.scans > 1 && ycck.decoded_scans == ycck.parse.scans &&
               ycck_image.progressive && ycck_image.width == 16 && ycck_image.height == 16 &&
               ycck_image.component_count == 4 && ycck_image.coefficient_blocks == 10 &&
               ycck_image.maximum_horizontal_sampling == 2 && ycck_image.maximum_vertical_sampling == 2 &&
               ycck_image.components[3].horizontal_sampling == 2 &&
               ycck_image.components[3].vertical_sampling == 2 && ycck_image.has_adobe &&
               ycck_image.color_transform == JpegColorTransform::ycck &&
               has_ac_coefficient(ycck_image.coefficient_blocks),
           "jpeg_adobe_ycck_progressive_decode");

    u8 invalid_cmyk_transform[sizeof(kJpegCmykAdobeReference)]{};
    copy_bytes(invalid_cmyk_transform, kJpegCmykAdobeReference, sizeof(invalid_cmyk_transform));
    invalid_cmyk_transform[17] = 1;
    expect(jpeg_parse(ConstSlice<u8>{invalid_cmyk_transform, sizeof(invalid_cmyk_transform)}, &image,
                      &g_jpeg_workspace)
                   .error == JpegError::unsupported_color,
           "jpeg_four_component_adobe_transform_one_reject");
}

void test_webp_lossless() noexcept {
    struct Fixture {
        ConstSlice<u8> encoded;
        u32 width;
        u32 height;
        u64 rgba_hash;
    };
    const Fixture fixtures[] = {
        {ConstSlice<u8>{kWebpOne, sizeof(kWebpOne)}, 1, 1, 0xD38B1EFD51EFB61Dull},
        {ConstSlice<u8>{kWebpPalette, sizeof(kWebpPalette)}, 8, 8, 0x71795AF1A23AF695ull},
        {ConstSlice<u8>{kWebpGradient, sizeof(kWebpGradient)}, 16, 16, 0x0C28A89B429BABA5ull},
    };
    u8* workspace_bytes = reinterpret_cast<u8*>(&g_vp8l_workspace);
    for (u32 index = 0; index < sizeof(g_vp8l_workspace); ++index) {
        workspace_bytes[index] = 0xA5;
    }
    bool decoded_all = true;
    for (u32 index = 0; index < static_cast<u32>(sizeof(fixtures) / sizeof(fixtures[0])); ++index) {
        WebpImage image{};
        const WebpDecodeResult decoded =
            webp_lossless_decode_rgba8(fixtures[index].encoded, &image, &g_vp8l_workspace,
                                       Slice<u8>{g_webp_rgba, sizeof(g_webp_rgba)});
        decoded_all = decoded_all && decoded.ok() && decoded.parse.chunks == 1 &&
                      image.codec == WebpCodec::vp8l_lossless && image.width == fixtures[index].width &&
                      image.height == fixtures[index].height &&
                      decoded.rgba_bytes == image.width * image.height * 4 &&
                      hash64_bytes(kHash64Seed, g_webp_rgba, decoded.rgba_bytes) == fixtures[index].rgba_hash;
    }
    expect(decoded_all, "webp_vp8l_deterministic_fixture_decode");

    WebpImage image{};
    u8 malformed[sizeof(kWebpOne)]{};
    copy_bytes(malformed, kWebpOne, sizeof(malformed));
    malformed[4] ^= 1;
    expect(webp_parse(ConstSlice<u8>{malformed, sizeof(malformed)}, &image).error == WebpError::riff,
           "webp_exact_riff_size_reject");
    malformed[4] ^= 1;

    malformed[21] = 0xFF;
    malformed[22] = static_cast<u8>(malformed[22] | 0x3F);
    expect(webp_parse(ConstSlice<u8>{malformed, sizeof(malformed)}, &image).error == WebpError::dimensions,
           "webp_dimension_cap_reject");

    constexpr u8 animated[] = {
        'R', 'I', 'F', 'F', 22, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8',
        'X', 10,  0,   0,   0,  2, 0, 0, 0,   0,   0,   0,   0,   0,   0,
    };
    const WebpParseResult lossy_parse =
        webp_parse(ConstSlice<u8>{kWebpLossy4x4, sizeof(kWebpLossy4x4)}, &image);
    expect(lossy_parse.ok() && lossy_parse.chunks == 1 && image.codec == WebpCodec::vp8_lossy &&
               image.width == 4 && image.height == 4 && image.bitstream.data == kWebpLossy4x4 + 20 &&
               image.bitstream.count == sizeof(kWebpLossy4x4) - 20 && !image.alpha_bitstream.data &&
               image.alpha_bitstream.count == 0 && !image.has_alpha &&
               webp_lossless_decode_rgba8(ConstSlice<u8>{kWebpLossy4x4, sizeof(kWebpLossy4x4)}, &image,
                                          &g_vp8l_workspace, Slice<u8>{g_webp_rgba, sizeof(g_webp_rgba)})
                       .error == WebpError::unsupported_lossy &&
               webp_parse(ConstSlice<u8>{animated, sizeof(animated)}, &image).error ==
                   WebpError::unsupported_animation,
           "webp_lossy_container_admission_and_animation_rejection");

    expect(webp_lossless_decode_rgba8(ConstSlice<u8>{kWebpOne, sizeof(kWebpOne)}, &image, &g_vp8l_workspace,
                                      Slice<u8>{g_webp_rgba, 3})
                       .error == WebpError::output_capacity &&
               webp_parse(ConstSlice<u8>{kWebpOne, sizeof(kWebpOne) - 1}, &image).error == WebpError::riff,
           "webp_output_and_exact_document_bounds");
}

void test_webp_alpha() noexcept {
    constexpr u32 width = 4;
    constexpr u32 height = 3;
    constexpr u8 expected[] = {
        0, 17, 255, 3, 4, 200, 10, 80, 255, 1, 128, 64,
    };
    u8 payload[1 + width * height]{};
    u8 output[16]{};
    bool filters_exact = true;
    for (u32 filter = 0; filter < 4; ++filter) {
        payload[0] = static_cast<u8>(filter << 2);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                const u32 index = y * width + x;
                u8 predictor = 0;
                if (filter != 0) {
                    if (x == 0) {
                        predictor = y == 0 ? 0 : expected[index - width];
                    } else if (y == 0) {
                        predictor = expected[index - 1];
                    } else if (filter == 1) {
                        predictor = expected[index - 1];
                    } else if (filter == 2) {
                        predictor = expected[index - width];
                    } else {
                        const i32 gradient = static_cast<i32>(expected[index - 1]) +
                                             expected[index - width] - expected[index - width - 1];
                        predictor = gradient < 0 ? 0 : gradient > 255 ? 255 : static_cast<u8>(gradient);
                    }
                }
                payload[index + 1] = static_cast<u8>(expected[index] - predictor);
            }
        }
        const WebpAlphaDecodeResult decoded =
            webp_alpha_decode(ConstSlice<u8>{payload, sizeof(payload)}, width, height, nullptr, Slice<u8>{},
                              Slice<u8>{output, sizeof(output)});
        filters_exact =
            filters_exact && decoded.ok() && decoded.alpha_bytes == width * height &&
            decoded.compression == WebpAlphaCompression::raw &&
            static_cast<u32>(decoded.filter) == filter &&
            bytes_equal(ConstSlice<u8>{output, width * height}, ConstSlice<u8>{expected, sizeof(expected)});
    }
    expect(filters_exact, "webp_alpha_all_raw_filters_exact");

    payload[0] = static_cast<u8>(payload[0] | 0x10u);
    const WebpAlphaDecodeResult preprocessed =
        webp_alpha_decode(ConstSlice<u8>{payload, sizeof(payload)}, width, height, nullptr, Slice<u8>{},
                          Slice<u8>{output, sizeof(output)});
    expect(
        preprocessed.ok() && preprocessed.preprocessing == WebpAlphaPreprocessing::level_reduction &&
            bytes_equal(ConstSlice<u8>{output, width * height}, ConstSlice<u8>{expected, sizeof(expected)}),
        "webp_alpha_level_reduction_is_informative");

    u8 oversized[sizeof(payload) + 1]{};
    for (u32 index = 0; index < sizeof(payload); ++index) {
        oversized[index] = payload[index];
    }
    payload[0] = 0x40;
    constexpr u32 full = sizeof(output);
    const auto alpha_error = [&](ConstSlice<u8> data, u32 alpha_width, u32 alpha_height, u32 capacity) noexcept {
        return webp_alpha_decode(data, alpha_width, alpha_height, nullptr, Slice<u8>{}, Slice<u8>{output, capacity})
            .error;
    };
    const bool bounds_exact =
        alpha_error({payload, sizeof(payload)}, width, height, full) == WebpAlphaError::reserved_bits &&
        alpha_error({reinterpret_cast<const u8*>("\x20\0"), 2}, 1, 1, full) ==
            WebpAlphaError::unsupported_preprocessing &&
        alpha_error({reinterpret_cast<const u8*>("\x02\0"), 2}, 1, 1, full) ==
            WebpAlphaError::unsupported_compression &&
        alpha_error({oversized, sizeof(oversized)}, width, height, full) == WebpAlphaError::data_size &&
        alpha_error({oversized, sizeof(payload) - 1}, width, height, full) == WebpAlphaError::data_size &&
        alpha_error({oversized, sizeof(payload)}, width, height, 11) == WebpAlphaError::output_capacity &&
        alpha_error({oversized, sizeof(payload)}, 0, height, full) == WebpAlphaError::dimensions;
    expect(bounds_exact, "webp_alpha_header_size_and_capacity_bounds");

    WebpImage source_image{};
    alignas(u32) u8 reference_rgba[16 * 16 * 4]{};
    u8 expected_green[16 * 16]{};
    const WebpDecodeResult reference =
        webp_lossless_decode_rgba8(ConstSlice<u8>{kWebpGradient, sizeof(kWebpGradient)}, &source_image,
                                   &g_vp8l_workspace, Slice<u8>{reference_rgba, sizeof(reference_rgba)});
    u8 compressed[sizeof(kWebpGradient)]{};
    compressed[0] = 1;
    const u32 compressed_bytes = source_image.bitstream.count + 1;
    for (u32 index = 0; index < source_image.bitstream.count; ++index) {
        compressed[index + 1] = source_image.bitstream[index];
    }
    for (u32 index = 0; index < 16 * 16; ++index) {
        expected_green[index] = reference_rgba[index * 4 + 1];
    }
    alignas(u32) u8 rgba_scratch[16 * 16 * 4]{};
    u8 alpha[16 * 16]{};
    const WebpAlphaDecodeResult compressed_result =
        webp_alpha_decode(ConstSlice<u8>{compressed, compressed_bytes}, 16, 16, &g_vp8l_workspace,
                          Slice<u8>{rgba_scratch, sizeof(rgba_scratch)}, Slice<u8>{alpha, sizeof(alpha)});
    bool prefixes_rejected = reference.ok() && compressed_result.ok() && compressed_result.lossless.ok() &&
                             compressed_result.alpha_bytes == 16 * 16 &&
                             bytes_equal(ConstSlice<u8>{alpha, sizeof(alpha)},
                                         ConstSlice<u8>{expected_green, sizeof(expected_green)});
    for (u32 bytes = 0; bytes < compressed_bytes && prefixes_rejected; ++bytes) {
        prefixes_rejected = !webp_alpha_decode(ConstSlice<u8>{compressed, bytes}, 16, 16, &g_vp8l_workspace,
                                               Slice<u8>{rgba_scratch, sizeof(rgba_scratch)},
                                               Slice<u8>{alpha, sizeof(alpha)})
                                 .ok();
    }
    expect(prefixes_rejected, "webp_alpha_lossless_green_and_prefix_rejection");
    expect(webp_alpha_decode(ConstSlice<u8>{compressed, compressed_bytes}, 16, 16, &g_vp8l_workspace,
                             Slice<u8>{rgba_scratch, sizeof(rgba_scratch) - 1},
                             Slice<u8>{alpha, sizeof(alpha)})
                   .error == WebpAlphaError::scratch_capacity,
           "webp_alpha_lossless_scratch_bound");
}

void test_vp8_keyframe_header() noexcept {
    constexpr u32 payload_offset = 20;
    constexpr u32 payload_bytes = sizeof(kWebpLossy4x4) - payload_offset;
    const u8* payload = kWebpLossy4x4 + payload_offset;
    Vp8Keyframe frame{};
    const Vp8ParseResult parsed = vp8_parse_keyframe(ConstSlice<u8>{payload, payload_bytes}, &frame);
    expect(parsed.ok() && frame.payload_bytes == payload_bytes && frame.first_partition_bytes == 15 &&
               frame.first_partition.data == payload + 10 && frame.first_partition.count == 15 &&
               frame.remaining_data.data == payload + 25 &&
               frame.remaining_data.count == payload_bytes - 25 && frame.width == 4 && frame.height == 4 &&
               frame.version == 0 && frame.horizontal_scale == 0 && frame.vertical_scale == 0,
           "vp8_keyframe_header_and_partition_bounds");

    bool prefixes_rejected = true;
    for (u32 bytes = 0; bytes < 25 && prefixes_rejected; ++bytes) {
        prefixes_rejected = !vp8_parse_keyframe(ConstSlice<u8>{payload, bytes}, &frame).ok();
    }
    expect(prefixes_rejected, "vp8_keyframe_all_required_header_prefixes_reject");

    u8 malformed[payload_bytes]{};
    auto restore = [&]() noexcept {
        for (u32 index = 0; index < payload_bytes; ++index) {
            malformed[index] = payload[index];
        }
    };
    restore();
    malformed[0] |= 1u;
    const bool interframe_rejected =
        vp8_parse_keyframe(ConstSlice<u8>{malformed, sizeof(malformed)}, &frame).error ==
        Vp8Error::interframe;
    restore();
    malformed[0] = static_cast<u8>((malformed[0] & ~0x0Eu) | (4u << 1));
    const bool version_rejected =
        vp8_parse_keyframe(ConstSlice<u8>{malformed, sizeof(malformed)}, &frame).error ==
        Vp8Error::unsupported_version;
    restore();
    malformed[0] &= static_cast<u8>(~(1u << 4));
    const bool invisible_rejected =
        vp8_parse_keyframe(ConstSlice<u8>{malformed, sizeof(malformed)}, &frame).error ==
        Vp8Error::invisible_frame;
    restore();
    malformed[3] ^= 1u;
    const bool signature_rejected =
        vp8_parse_keyframe(ConstSlice<u8>{malformed, sizeof(malformed)}, &frame).error ==
        Vp8Error::signature;
    restore();
    malformed[6] = 0;
    malformed[7] = 0;
    const bool zero_size_rejected =
        vp8_parse_keyframe(ConstSlice<u8>{malformed, sizeof(malformed)}, &frame).error ==
        Vp8Error::dimensions;
    restore();
    malformed[6] = 1;
    malformed[7] = 2;
    const bool cap_rejected =
        vp8_parse_keyframe(ConstSlice<u8>{malformed, sizeof(malformed)}, &frame).error ==
        Vp8Error::dimensions;
    restore();
    malformed[1] = 0xFF;
    malformed[2] = 0xFF;
    const bool partition_rejected =
        vp8_parse_keyframe(ConstSlice<u8>{malformed, sizeof(malformed)}, &frame).error ==
        Vp8Error::first_partition;
    expect(interframe_rejected && version_rejected && invisible_rejected && signature_rejected &&
               zero_size_rejected && cap_rejected && partition_rejected &&
               vp8_parse_keyframe(ConstSlice<u8>{payload, kWebpEncodedMaximumBytes + 1}, &frame).error ==
                   Vp8Error::encoded_limit &&
               vp8_parse_keyframe(ConstSlice<u8>{payload, payload_bytes}, nullptr).error ==
                   Vp8Error::invalid_input,
           "vp8_keyframe_type_version_signature_and_bounds");
}

void test_vp8_bool_reader() noexcept {
    constexpr u8 encoded[] = {
        0x00, 0xA9, 0x40, 0xEF, 0xCD, 0xAB, 0x22, 0xCB, 0x22, 0x5D, 0x17, 0x9A, 0x6D,
        0x9C, 0x6F, 0x4E, 0x0B, 0xB0, 0x8A, 0x3C, 0xA6, 0x1C, 0xF8, 0xCE, 0x5D, 0x2F,
        0x9A, 0xBD, 0xBD, 0xA0, 0xD1, 0xFF, 0x3A, 0xF2, 0x6E, 0xAB, 0xD5, 0x38, 0xB3,
        0xCA, 0x71, 0x6E, 0x50, 0x14, 0x3B, 0x9B, 0xE5, 0xF7, 0x78, 0x00,
    };
    Vp8BoolReader reader{};
    bool exact = vp8_bool_reader_initialize(&reader, ConstSlice<u8>{encoded, sizeof(encoded)});
    for (u32 index = 0; index < 256 && exact; ++index) {
        const u8 probability = static_cast<u8>((index * 73u) % 255u + 1u);
        const u32 expected = (index * 29u + (index >> 2)) & 1u;
        exact = vp8_bool_read(&reader, probability) == expected && reader.ok();
    }
    const u32 literal = vp8_bool_read_literal(&reader, 16);
    exact = exact && reader.ok() && literal == 0xBEEFu && reader.byte_offset == sizeof(encoded) &&
            reader.bit_count < 8 && reader.range >= 128 && reader.range <= 255;
    expect(exact, "vp8_bool_probability_and_literal_vector");

    Vp8BoolReader truncated{};
    bool truncation_exact =
        vp8_bool_reader_initialize(&truncated, ConstSlice<u8>{encoded, sizeof(encoded) - 1});
    for (u32 index = 0; index < 256 && truncation_exact; ++index) {
        const u8 probability = static_cast<u8>((index * 73u) % 255u + 1u);
        truncation_exact =
            vp8_bool_read(&truncated, probability) == ((index * 29u + (index >> 2)) & 1u) && truncated.ok();
    }
    if (truncation_exact) {
        vp8_bool_read_literal(&truncated, 16);
        truncation_exact = truncated.error == Vp8BoolError::truncated;
    }
    expect(truncation_exact, "vp8_bool_exact_partition_truncation");

    Vp8BoolReader invalid{};
    const bool one_byte_rejected = !vp8_bool_reader_initialize(&invalid, ConstSlice<u8>{encoded, 1}) &&
                                   invalid.error == Vp8BoolError::truncated;
    constexpr u8 probability_endpoints[] = {0, 0, 0};
    vp8_bool_reader_initialize(&invalid,
                               ConstSlice<u8>{probability_endpoints, sizeof(probability_endpoints)});
    const bool probability_endpoints_valid = vp8_bool_read(&invalid, 0) == 0 && invalid.ok() &&
                                             vp8_bool_read(&invalid, 255) == 0 && invalid.ok();
    vp8_bool_reader_initialize(&invalid, ConstSlice<u8>{encoded, sizeof(encoded)});
    vp8_bool_read_literal(&invalid, 33);
    expect(one_byte_rejected && probability_endpoints_valid &&
               invalid.error == Vp8BoolError::literal_width &&
               !vp8_bool_reader_initialize(nullptr, ConstSlice<u8>{encoded, sizeof(encoded)}),
           "vp8_bool_initialization_probability_and_width_bounds");

    struct ReferenceReader {
        const u8* bytes;
        u32 count;
        u32 offset;
        u64 value;
        u32 range;
        i32 bits;
        bool eof;
    };
    const auto reference_load = [](ReferenceReader* reference) noexcept {
        if (reference->offset < reference->count) {
            reference->bits += 8;
            reference->value = static_cast<u64>(reference->bytes[reference->offset++]) |
                               (reference->value << 8);
        } else if (!reference->eof) {
            reference->value <<= 8;
            reference->bits += 8;
            reference->eof = true;
        }
    };
    const auto reference_read = [&reference_load](ReferenceReader* reference,
                                                   u8 probability) noexcept -> u32 {
        u32 range = reference->range;
        if (reference->bits < 0) {
            reference_load(reference);
        }
        const u32 split = (range * probability) >> 8;
        const u32 current = static_cast<u32>(reference->value >> reference->bits);
        const u32 bit = current > split ? 1u : 0u;
        if (bit != 0) {
            range -= split;
            reference->value -= static_cast<u64>(split + 1u) << reference->bits;
        } else {
            range = split + 1u;
        }
        u32 highest_bit = 0;
        for (u32 scan = range; scan > 1; scan >>= 1) {
            ++highest_bit;
        }
        const u32 shift = 7u ^ highest_bit;
        reference->range = (range << shift) - 1u;
        reference->bits -= static_cast<i32>(shift);
        return bit;
    };
    u8 long_encoded[4096]{};
    for (u32 index = 0; index < sizeof(long_encoded); ++index) {
        long_encoded[index] = static_cast<u8>((index * 197u + (index >> 2u) * 29u + 17u) & 255u);
    }
    ReferenceReader reference{long_encoded, sizeof(long_encoded), 0, 0, 254, -8, false};
    reference_load(&reference);
    Vp8BoolReader long_reader{};
    bool long_exact = vp8_bool_reader_initialize(
        &long_reader, ConstSlice<u8>{long_encoded, sizeof(long_encoded)});
    for (u32 index = 0; index < 10'000u && long_exact; ++index) {
        const u8 probability = static_cast<u8>((index * 149u + (index >> 3u) * 31u) % 255u + 1u);
        long_exact = reference_read(&reference, probability) ==
                         vp8_bool_read(&long_reader, probability) &&
                     long_reader.ok();
    }
    expect(long_exact, "vp8_bool_long_stream_matches_reference_decoder");
}

void test_vp8_keyframe_control() noexcept {
    constexpr u32 payload_offset = 20;
    constexpr u32 payload_bytes = sizeof(kWebpLossy4x4) - payload_offset;
    const u8* payload = kWebpLossy4x4 + payload_offset;
    Vp8Keyframe frame{};
    const Vp8ParseResult envelope = vp8_parse_keyframe(ConstSlice<u8>{payload, payload_bytes}, &frame);
    Vp8ControlHeader header{};
    Vp8ProbabilityWorkspace probabilities{};
    const Vp8ControlParseResult parsed = vp8_parse_keyframe_control(frame, &header, &probabilities);
    const bool exact =
        envelope.ok() && parsed.ok() && header.color_space == 0 && header.clamping_type == 0 &&
        !header.segmentation.enabled && header.segmentation.absolute_delta == 1 &&
        header.segmentation.tree_probability[0] == 255 && header.segmentation.tree_probability[1] == 255 &&
        header.segmentation.tree_probability[2] == 255 && !header.loop_filter.simple &&
        header.loop_filter.level == 5 && header.loop_filter.sharpness == 0 &&
        !header.loop_filter.use_delta && header.token_partition_count == 1 &&
        header.token_partitions[0].bytes.data == payload + 25 &&
        header.token_partitions[0].bytes.count == payload_bytes - 25 &&
        header.quantization.base_quantizer == 19 && header.quantization.y1_dc_delta == 0 &&
        header.quantization.y2_dc_delta == 0 && header.quantization.y2_ac_delta == 0 &&
        header.quantization.uv_dc_delta == -2 && header.quantization.uv_ac_delta == 0 &&
        !header.refresh_entropy_probabilities && header.coefficient_probability_updates == 2 &&
        probabilities.coefficient[0] == 128 && probabilities.coefficient[33] == 253 &&
        probabilities.coefficient[528] == 0 && probabilities.coefficient[651] == 0 &&
        !header.use_skip_probability && header.macroblock_reader.ok() &&
        header.macroblock_reader.byte_offset == 11 && header.macroblock_reader.bit_count == 1 &&
        header.macroblock_reader.range == 246;
    expect(exact, "vp8_keyframe_complete_control_header");

    Vp8ModeWorkspace mode_workspace{};
    const Vp8CoefficientParseResult coefficients = vp8_decode_keyframe_macroblocks(
        frame, header, probabilities, &mode_workspace, &g_vp8_coefficient_workspace);
    constexpr Vp8SubblockMode expected_fixture_modes[16] = {
        Vp8SubblockMode::dc,          Vp8SubblockMode::true_motion, Vp8SubblockMode::true_motion,
        Vp8SubblockMode::true_motion, Vp8SubblockMode::true_motion, Vp8SubblockMode::dc,
        Vp8SubblockMode::dc,          Vp8SubblockMode::dc,          Vp8SubblockMode::true_motion,
        Vp8SubblockMode::dc,          Vp8SubblockMode::dc,          Vp8SubblockMode::dc,
        Vp8SubblockMode::true_motion, Vp8SubblockMode::dc,          Vp8SubblockMode::dc,
        Vp8SubblockMode::dc,
    };
    bool fixture_modes_exact = coefficients.ok() && mode_workspace.columns == 1 && mode_workspace.rows == 1 &&
                               mode_workspace.macroblock_count == 1 &&
                               mode_workspace.macroblocks[0].luma == Vp8IntraMode::subblocks &&
                               mode_workspace.macroblocks[0].chroma == Vp8IntraMode::vertical &&
                               mode_workspace.macroblocks[0].segment_id == 0 &&
                               mode_workspace.macroblocks[0].skip_coefficients == 0;
    for (u32 index = 0; index < 16 && fixture_modes_exact; ++index) {
        fixture_modes_exact =
            mode_workspace.macroblocks[0].luma_subblock[index] == expected_fixture_modes[index];
    }
    expect(fixture_modes_exact, "vp8_keyframe_macroblock_modes_and_context");

    u32 nonzero_coefficients = 0;
    for (u32 block = 0; block < 24; ++block) {
        for (u32 coefficient = 0; coefficient < 16; ++coefficient) {
            nonzero_coefficients +=
                g_vp8_coefficient_workspace.macroblocks[0].values[block][coefficient] != 0;
        }
    }
    const u64 coefficient_hash =
        hash64_bytes(kHash64Seed, g_vp8_coefficient_workspace.macroblocks[0].values,
                     sizeof(g_vp8_coefficient_workspace.macroblocks[0].values));
    expect(coefficients.ok() && g_vp8_coefficient_workspace.columns == 1 &&
               g_vp8_coefficient_workspace.rows == 1 && g_vp8_coefficient_workspace.macroblock_count == 1 &&
               g_vp8_coefficient_workspace.macroblocks[0].has_coefficients == 1 &&
               nonzero_coefficients == 37 && coefficient_hash == 0x58843F07BFBE9282ull &&
               g_vp8_coefficient_workspace.macroblocks[0].values[0][0] == -40 &&
               g_vp8_coefficient_workspace.macroblocks[0].values[0][1] == -276 &&
               g_vp8_coefficient_workspace.macroblocks[0].values[16][0] == -76 &&
               g_vp8_coefficient_workspace.macroblocks[0].values[20][1] == -115,
           "vp8_coefficients_exact_tokens_contexts_and_dequantization");

    bool token_prefixes_rejected = coefficients.ok();
    for (u32 bytes = 0; bytes < 45 && token_prefixes_rejected; ++bytes) {
        Vp8ControlHeader truncated_header = header;
        truncated_header.token_partitions[0].bytes.count = bytes;
        token_prefixes_rejected =
            !vp8_decode_keyframe_macroblocks(frame, truncated_header, probabilities, &mode_workspace,
                                             &g_vp8_coefficient_workspace)
                 .ok();
    }
    expect(token_prefixes_rejected, "vp8_coefficients_all_required_token_prefixes_reject");

    // The fixture's first partition is 15 bytes; its last mode decision only renormalizes into the 14th, and
    // libwebp decodes it from 13 (the lookahead may run past the end; a read that needs a window bit there may
    // not). Partition-end trimming of this kind is what some CDN encoders emit.
    bool mode_prefixes_rejected = envelope.ok();
    for (u32 bytes = 0; bytes < 13 && mode_prefixes_rejected; ++bytes) {
        Vp8Keyframe truncated = frame;
        truncated.first_partition.count = bytes;
        Vp8ControlHeader truncated_header{};
        const Vp8ControlParseResult truncated_control =
            vp8_parse_keyframe_control(truncated, &truncated_header, &probabilities);
        mode_prefixes_rejected =
            !truncated_control.ok() ||
            !vp8_decode_keyframe_macroblocks(truncated, truncated_header, probabilities, &mode_workspace,
                                             &g_vp8_coefficient_workspace)
                 .ok();
    }
    expect(mode_prefixes_rejected, "vp8_modes_all_required_partition_prefixes_reject");

    Vp8Keyframe trimmed = frame;
    trimmed.first_partition.count = 13;
    Vp8ControlHeader trimmed_header{};
    Vp8ModeWorkspace trimmed_modes{};
    const bool trimmed_control = vp8_parse_keyframe_control(trimmed, &trimmed_header, &probabilities).ok();
    const bool trimmed_decoded = trimmed_control && vp8_decode_keyframe_macroblocks(trimmed, trimmed_header,
                                                                                    probabilities, &trimmed_modes,
                                                                                    &g_vp8_coefficient_workspace)
                                                        .ok();
    bool trimmed_exact = trimmed_decoded && trimmed_modes.macroblock_count == 1 &&
                         trimmed_modes.macroblocks[0].luma == Vp8IntraMode::subblocks &&
                         trimmed_modes.macroblocks[0].chroma == Vp8IntraMode::vertical &&
                         hash64_bytes(kHash64Seed, g_vp8_coefficient_workspace.macroblocks[0].values,
                                      sizeof(g_vp8_coefficient_workspace.macroblocks[0].values)) ==
                             0x58843F07BFBE9282ull;
    for (u32 index = 0; index < 16 && trimmed_exact; ++index)
        trimmed_exact = trimmed_modes.macroblocks[0].luma_subblock[index] == expected_fixture_modes[index];
    expect(trimmed_exact, "vp8_partition_ending_inside_last_lookahead_decodes");

    // The header's last decision renormalizes into byte 11; 10 bytes hold every bit it reads.
    bool required_prefixes_rejected = envelope.ok();
    for (u32 bytes = 0; bytes < 10 && required_prefixes_rejected; ++bytes) {
        Vp8Keyframe truncated = frame;
        truncated.first_partition.count = bytes;
        required_prefixes_rejected = !vp8_parse_keyframe_control(truncated, &header, &probabilities).ok();
    }
    Vp8Keyframe header_only = frame;
    header_only.first_partition.count = 10;
    expect(required_prefixes_rejected && vp8_parse_keyframe_control(header_only, &header, &probabilities).ok(),
           "vp8_control_all_required_partition_prefixes_reject");

    Vp8Keyframe missing_tokens = frame;
    missing_tokens.remaining_data.count = 1;
    const bool token_bound = vp8_parse_keyframe_control(missing_tokens, &header, &probabilities).error ==
                             Vp8ControlError::token_partition_bounds;
    expect(token_bound &&
               vp8_parse_keyframe_control(frame, nullptr, &probabilities).error ==
                   Vp8ControlError::invalid_input &&
               vp8_parse_keyframe_control(frame, &header, nullptr).error == Vp8ControlError::invalid_input,
           "vp8_control_workspace_and_token_bounds");

    // Authored with the RFC 6386 boolean encoder. This exercises every
    // optional keyframe-control branch plus a four-partition size table.
    constexpr u8 branch_partition[] = {
        0xF9, 0x20, 0xE5, 0x60, 0xE1, 0xDF, 0x1B, 0x10, 0x66, 0xB9, 0x9F, 0x85, 0x66, 0x8E, 0x28, 0xF7,
        0x63, 0xA7, 0x45, 0x7B, 0xDE, 0xFF, 0x00, 0x9D, 0xA6, 0xC0, 0xCB, 0x7B, 0x00, 0x00, 0x48, 0x0F,
        0xFC, 0x1D, 0x7E, 0xB5, 0x66, 0x97, 0x4A, 0x64, 0x63, 0x4C, 0x47, 0xAD, 0x94, 0x00,
    };
    constexpr u8 branch_tokens[] = {
        9, 0, 0, 9, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    Vp8Keyframe branches{};
    branches.first_partition = ConstSlice<u8>{branch_partition, sizeof(branch_partition)};
    branches.remaining_data = ConstSlice<u8>{branch_tokens, sizeof(branch_tokens)};
    branches.width = 32;
    branches.height = 32;
    const Vp8ControlParseResult branch_result =
        vp8_parse_keyframe_control(branches, &header, &probabilities);
    const bool branch_exact =
        branch_result.ok() && header.color_space == 1 && header.clamping_type == 1 &&
        header.segmentation.enabled && header.segmentation.update_map && header.segmentation.update_data &&
        !header.segmentation.absolute_delta && header.segmentation.quantizer[0] == 5 &&
        header.segmentation.quantizer[1] == -7 && header.segmentation.quantizer[2] == 0 &&
        header.segmentation.quantizer[3] == -127 && header.segmentation.filter_strength[0] == 3 &&
        header.segmentation.filter_strength[1] == -4 && header.segmentation.filter_strength[2] == 0 &&
        header.segmentation.filter_strength[3] == 63 && header.segmentation.tree_probability[0] == 0 &&
        header.segmentation.tree_probability[1] == 255 && header.segmentation.tree_probability[2] == 177 &&
        header.loop_filter.simple && header.loop_filter.level == 37 && header.loop_filter.sharpness == 5 &&
        header.loop_filter.use_delta && header.loop_filter.update_delta &&
        header.loop_filter.reference_delta[0] == 2 && header.loop_filter.reference_delta[1] == -17 &&
        header.loop_filter.reference_delta[2] == 0 && header.loop_filter.reference_delta[3] == -63 &&
        header.loop_filter.mode_delta[0] == 0 && header.loop_filter.mode_delta[1] == 6 &&
        header.loop_filter.mode_delta[2] == -7 && header.loop_filter.mode_delta[3] == 0 &&
        header.token_partition_count == 4 && header.token_partitions[0].bytes.data == branch_tokens + 9 &&
        header.token_partitions[0].bytes.count == 9 &&
        header.token_partitions[1].bytes.data == branch_tokens + 18 &&
        header.token_partitions[1].bytes.count == 9 &&
        header.token_partitions[2].bytes.data == branch_tokens + 27 &&
        header.token_partitions[2].bytes.count == 2 &&
        header.token_partitions[3].bytes.data == branch_tokens + 29 &&
        header.token_partitions[3].bytes.count == 2 && header.quantization.base_quantizer == 100 &&
        header.quantization.y1_dc_delta == 3 && header.quantization.y2_dc_delta == -4 &&
        header.quantization.y2_ac_delta == 0 && header.quantization.uv_dc_delta == 0 &&
        header.quantization.uv_ac_delta == -15 && header.refresh_entropy_probabilities &&
        header.coefficient_probability_updates == 3 && probabilities.coefficient[0] == 0 &&
        probabilities.coefficient[33] == 17 && probabilities.coefficient[528] == 255 &&
        header.use_skip_probability && header.skip_probability == 0 && header.macroblock_reader.ok();
    expect(branch_exact, "vp8_control_all_optional_branches_and_four_partitions");

    const Vp8CoefficientParseResult branch_coefficients = vp8_decode_keyframe_macroblocks(
        branches, header, probabilities, &mode_workspace, &g_vp8_coefficient_workspace);
    constexpr Vp8SubblockMode expected_branch_modes[16] = {
        Vp8SubblockMode::dc,
        Vp8SubblockMode::true_motion,
        Vp8SubblockMode::vertical_edge,
        Vp8SubblockMode::horizontal_edge,
        Vp8SubblockMode::right_down,
        Vp8SubblockMode::vertical_right,
        Vp8SubblockMode::left_down,
        Vp8SubblockMode::vertical_left,
        Vp8SubblockMode::horizontal_down,
        Vp8SubblockMode::horizontal_up,
        Vp8SubblockMode::dc,
        Vp8SubblockMode::vertical_edge,
        Vp8SubblockMode::right_down,
        Vp8SubblockMode::left_down,
        Vp8SubblockMode::horizontal_down,
        Vp8SubblockMode::horizontal_up,
    };
    bool branch_modes_exact =
        branch_coefficients.ok() && mode_workspace.columns == 2 && mode_workspace.rows == 2 &&
        mode_workspace.macroblock_count == 4 && mode_workspace.macroblocks[0].segment_id == 0 &&
        !mode_workspace.macroblocks[0].skip_coefficients &&
        mode_workspace.macroblocks[0].luma == Vp8IntraMode::dc &&
        mode_workspace.macroblocks[0].chroma == Vp8IntraMode::dc &&
        mode_workspace.macroblocks[1].segment_id == 1 && mode_workspace.macroblocks[1].skip_coefficients &&
        mode_workspace.macroblocks[1].luma == Vp8IntraMode::horizontal &&
        mode_workspace.macroblocks[1].chroma == Vp8IntraMode::vertical &&
        mode_workspace.macroblocks[2].segment_id == 2 && !mode_workspace.macroblocks[2].skip_coefficients &&
        mode_workspace.macroblocks[2].luma == Vp8IntraMode::true_motion &&
        mode_workspace.macroblocks[2].chroma == Vp8IntraMode::horizontal &&
        mode_workspace.macroblocks[3].segment_id == 3 && mode_workspace.macroblocks[3].skip_coefficients &&
        mode_workspace.macroblocks[3].luma == Vp8IntraMode::subblocks &&
        mode_workspace.macroblocks[3].chroma == Vp8IntraMode::true_motion;
    for (u32 index = 0; index < 16 && branch_modes_exact; ++index) {
        branch_modes_exact =
            mode_workspace.macroblocks[3].luma_subblock[index] == expected_branch_modes[index];
    }
    expect(branch_modes_exact, "vp8_modes_all_trees_segments_skips_and_neighbor_contexts");

    bool branch_coefficients_zero = branch_coefficients.ok() && g_vp8_coefficient_workspace.columns == 2 &&
                                    g_vp8_coefficient_workspace.rows == 2 &&
                                    g_vp8_coefficient_workspace.macroblock_count == 4;
    for (u32 macroblock = 0; macroblock < 4 && branch_coefficients_zero; ++macroblock) {
        branch_coefficients_zero = !g_vp8_coefficient_workspace.macroblocks[macroblock].has_coefficients;
        for (u32 block = 0; block < 24 && branch_coefficients_zero; ++block) {
            for (u32 coefficient = 0; coefficient < 16 && branch_coefficients_zero; ++coefficient) {
                branch_coefficients_zero =
                    g_vp8_coefficient_workspace.macroblocks[macroblock].values[block][coefficient] == 0;
            }
        }
    }
    expect(branch_coefficients_zero, "vp8_coefficients_four_partitions_skip_and_zero_contexts");

    u8 malformed_tokens[sizeof(branch_tokens)]{};
    for (u32 index = 0; index < sizeof(branch_tokens); ++index) {
        malformed_tokens[index] = branch_tokens[index];
    }
    malformed_tokens[0] = 1;
    branches.remaining_data = ConstSlice<u8>{malformed_tokens, sizeof(malformed_tokens)};
    const bool short_explicit_partition =
        vp8_parse_keyframe_control(branches, &header, &probabilities).error ==
        Vp8ControlError::token_partition_bounds;
    malformed_tokens[0] = 0xFF;
    malformed_tokens[1] = 0xFF;
    malformed_tokens[2] = 0xFF;
    const bool overrun_partition = vp8_parse_keyframe_control(branches, &header, &probabilities).error ==
                                   Vp8ControlError::token_partition_bounds;
    branches.remaining_data = ConstSlice<u8>{branch_tokens, 8};
    expect(short_explicit_partition && overrun_partition &&
               vp8_parse_keyframe_control(branches, &header, &probabilities).error ==
                   Vp8ControlError::token_partition_table,
           "vp8_control_partition_table_exact_bounds");
}

void test_lossy_webp_keyframe_decode() noexcept {
    WebpImage image{};
    Vp8Keyframe frame{};
    Vp8ControlHeader header{};
    Vp8ProbabilityWorkspace probabilities{};
    const LossyWebpDecodeResult opaque = lossy_webp_decode_keyframe(
        ConstSlice<u8>{kWebpLossy4x4, sizeof(kWebpLossy4x4)}, &image, &frame, &header, &probabilities,
        &g_vp8_mode_workspace, &g_vp8_coefficient_workspace, nullptr, {}, {});
    expect(opaque.ok() && opaque.parse.chunks == 1 && opaque.alpha_bytes == 0 &&
               image.codec == WebpCodec::vp8_lossy && !image.has_alpha && frame.width == 4 &&
               frame.height == 4 && g_vp8_mode_workspace.macroblock_count == 1 &&
               g_vp8_coefficient_workspace.macroblock_count == 1,
           "lossy_webp_complete_opaque_cpu_decode");

    constexpr u32 encoded_bytes = 136;
    u8 alpha_webp[encoded_bytes]{};
    auto write_u32 = [](u8* output, u32 value) noexcept {
        output[0] = static_cast<u8>(value);
        output[1] = static_cast<u8>(value >> 8);
        output[2] = static_cast<u8>(value >> 16);
        output[3] = static_cast<u8>(value >> 24);
    };
    write_u32(alpha_webp, 0x46464952u);
    write_u32(alpha_webp + 4, encoded_bytes - 8);
    write_u32(alpha_webp + 8, 0x50424557u);
    write_u32(alpha_webp + 12, 0x58385056u);
    write_u32(alpha_webp + 16, 10);
    alpha_webp[20] = 0x10;
    alpha_webp[24] = 3;
    alpha_webp[27] = 3;
    write_u32(alpha_webp + 30, 0x48504C41u);
    write_u32(alpha_webp + 34, 17);
    alpha_webp[38] = 0;
    for (u32 index = 0; index < 16; ++index) {
        alpha_webp[39 + index] = static_cast<u8>(index * 13u + 7u);
    }
    write_u32(alpha_webp + 56, 0x20385056u);
    write_u32(alpha_webp + 60, sizeof(kWebpLossy4x4) - 20);
    for (u32 index = 20; index < sizeof(kWebpLossy4x4); ++index) {
        alpha_webp[64 + index - 20] = kWebpLossy4x4[index];
    }
    alignas(u32) u8 alpha_scratch[16 * 4]{};
    u8 alpha[16]{};
    const LossyWebpDecodeResult decoded_alpha = lossy_webp_decode_keyframe(
        ConstSlice<u8>{alpha_webp, sizeof(alpha_webp)}, &image, &frame, &header, &probabilities,
        &g_vp8_mode_workspace, &g_vp8_coefficient_workspace, &g_vp8l_workspace,
        Slice<u8>{alpha_scratch, sizeof(alpha_scratch)}, Slice<u8>{alpha, sizeof(alpha)});
    bool alpha_exact = decoded_alpha.ok() && decoded_alpha.parse.chunks == 3 &&
                       decoded_alpha.alpha_bytes == 16 && image.has_alpha &&
                       image.alpha_bitstream.data == alpha_webp + 38 && image.alpha_bitstream.count == 17;
    for (u32 index = 0; index < 16 && alpha_exact; ++index) {
        alpha_exact = alpha[index] == static_cast<u8>(index * 13u + 7u);
    }
    expect(alpha_exact, "lossy_webp_external_raw_alpha_decode");

    alpha_webp[20] = 0;
    expect(!webp_parse(ConstSlice<u8>{alpha_webp, sizeof(alpha_webp)}, &image).ok(),
           "lossy_webp_alpha_feature_correlation_reject");
    expect(lossy_webp_decode_keyframe(ConstSlice<u8>{kWebpOne, sizeof(kWebpOne)}, &image, &frame, &header,
                                      &probabilities, &g_vp8_mode_workspace, &g_vp8_coefficient_workspace,
                                      nullptr, {}, {})
                   .error == LossyWebpError::unsupported_codec,
           "lossy_webp_lossless_codec_reject");
}

void test_animated_webp_lossless() noexcept {
    u8 encoded[512]{};
    AnimatedWebpFixtureResult fixture{};
    expect(build_lossless_animated_webp_fixture(Slice<u8>{encoded, sizeof(encoded)}, &fixture),
           "animated_webp_fixture_build");
    if (fixture.encoded_bytes == 0) {
        return;
    }

    AnimatedWebpWorkspace animation_workspace{};
    AnimatedWebpImage animation{};
    const AnimatedWebpParseResult parsed = animated_webp_parse(
        ConstSlice<u8>{encoded, fixture.encoded_bytes}, &animation, &animation_workspace);
    expect(parsed.ok() && parsed.chunks == 7 && parsed.frame_chunks == 4 && animation.width == 4 &&
               animation.height == 4 && animation.frame_count == 4 && animation.play_count == 2 &&
               animation.background_rgba == 0x80302010u && animation.has_alpha &&
               !animation.has_color_profile && animation.frames == animation_workspace.frames &&
               animation.frames[0].blend == AnimatedWebpBlend::source &&
               animation.frames[1].blend == AnimatedWebpBlend::over &&
               animation.frames[1].dispose == AnimatedWebpDispose::background &&
               animation.frames[3].x_offset == 2 && animation.frames[3].y_offset == 2,
           "animated_webp_exact_container_and_controls");

    const u64 durations[4] = {50'000, 10'000, 10'000, 20'000};
    bool frames_exact = true;
    for (u32 frame = 0; frame < 4; ++frame) {
        const AnimatedWebpDecodeResult decoded = animated_webp_decode_frame_rgba8(
            ConstSlice<u8>{encoded, fixture.encoded_bytes}, frame, &animation, &animation_workspace,
            &g_vp8l_workspace, Slice<u8>{g_webp_rgba, sizeof(g_webp_rgba)});
        frames_exact = frames_exact && decoded.ok() && decoded.duration_us == durations[frame] &&
                       decoded.frame_decode.rgba_bytes ==
                           static_cast<u32>(decoded.frame.width) * decoded.frame.height * 4u;
        const u32 expected = animated_webp_fixture_source_rgba(frame);
        const u32 pixel_count = static_cast<u32>(decoded.frame.width) * decoded.frame.height;
        for (u32 pixel = 0; pixel < pixel_count; ++pixel) {
            const u32 actual = static_cast<u32>(g_webp_rgba[pixel * 4]) |
                               static_cast<u32>(g_webp_rgba[pixel * 4 + 1]) << 8 |
                               static_cast<u32>(g_webp_rgba[pixel * 4 + 2]) << 16 |
                               static_cast<u32>(g_webp_rgba[pixel * 4 + 3]) << 24;
            frames_exact = frames_exact && actual == expected;
        }
    }
    expect(frames_exact, "animated_webp_all_lossless_frames_exact");

    // An animation can exceed the still-frame budget while every frame remains small.
    // A valid trailing unknown RIFF chunk exercises whole-container byte boundaries.
    auto* large = static_cast<u8*>(pages_allocate(kAnimatedWebpEncodedMaximumBytes));
    expect(large != nullptr, "animated_webp_large_container_allocate");
    if (large) {
        copy_bytes(large, encoded, fixture.encoded_bytes);
        auto store32 = [](u8* out, u32 value) noexcept {
            for (u32 byte = 0; byte < 4; ++byte)
                out[byte] = static_cast<u8>(value >> (byte * 8));
        };
        copy_bytes(large + fixture.encoded_bytes, "JUNK", 4);
        const u32 sizes[]{kWebpEncodedMaximumBytes + 4096u, kAnimatedWebpEncodedMaximumBytes};
        for (const u32 bytes : sizes) {
            store32(large + 4, bytes - 8);
            store32(large + fixture.encoded_bytes + 4, bytes - fixture.encoded_bytes - 8);
            const auto decoded = animated_webp_decode_frame_rgba8(
                {large, bytes}, 3, &animation, &animation_workspace, &g_vp8l_workspace,
                {g_webp_rgba, sizeof(g_webp_rgba)});
            expect(decoded.ok() && animation.frame_count == 4 && decoded.frame_index == 3 &&
                       decoded.duration_us == durations[3],
                   "animated_webp_large_container_last_frame_decodes_with_bounded_workspace");
        }
        expect(animated_webp_parse({large, kAnimatedWebpEncodedMaximumBytes + 1}, &animation,
                                   &animation_workspace).error == AnimatedWebpError::encoded_limit,
               "animated_webp_over_container_budget_rejected_before_read");
        pages_release(large);
    }

    bool prefixes_rejected = true;
    for (u32 bytes = 0; bytes < fixture.encoded_bytes; ++bytes) {
        prefixes_rejected =
            prefixes_rejected &&
            !animated_webp_parse(ConstSlice<u8>{encoded, bytes}, &animation, &animation_workspace).ok();
    }
    expect(prefixes_rejected, "animated_webp_every_proper_prefix_reject");

    u8 corrupt[512]{};
    auto restore = [&]() noexcept { copy_bytes(corrupt, encoded, fixture.encoded_bytes); };
    restore();
    corrupt[20] |= 0x80;
    expect(animated_webp_parse(ConstSlice<u8>{corrupt, fixture.encoded_bytes}, &animation,
                               &animation_workspace)
                   .error == AnimatedWebpError::unsupported_feature,
           "animated_webp_reserved_feature_reject");
    restore();
    corrupt[100] = 2;
    expect(animated_webp_parse(ConstSlice<u8>{corrupt, fixture.encoded_bytes}, &animation,
                               &animation_workspace)
                   .error == AnimatedWebpError::rectangle,
           "animated_webp_frame_rectangle_reject");
    restore();
    corrupt[68] = 'V';
    corrupt[69] = 'P';
    corrupt[70] = '8';
    corrupt[71] = ' ';
    expect(animated_webp_parse(ConstSlice<u8>{corrupt, fixture.encoded_bytes}, &animation,
                               &animation_workspace)
                   .error == AnimatedWebpError::chunk,
           "animated_webp_malformed_lossy_frame_reject");
    restore();
    corrupt[20] &= static_cast<u8>(~0x10u);
    expect(animated_webp_parse(ConstSlice<u8>{corrupt, fixture.encoded_bytes}, &animation,
                               &animation_workspace)
                   .error == AnimatedWebpError::order,
           "animated_webp_alpha_feature_mismatch_reject");
    restore();
    for (u32 index = 225; index < 236; ++index) {
        corrupt[index] = 0xFF;
    }
    const AnimatedWebpDecodeResult corrupt_late = animated_webp_decode_frame_rgba8(
        ConstSlice<u8>{corrupt, fixture.encoded_bytes}, 3, &animation, &animation_workspace,
        &g_vp8l_workspace, Slice<u8>{g_webp_rgba, sizeof(g_webp_rgba)});
    expect(!corrupt_late.ok() && corrupt_late.error == AnimatedWebpError::frame_decode,
           "animated_webp_late_frame_bitstream_reject");
}

void test_animated_webp_mixed_codecs() noexcept {
    u8 encoded[512]{};
    AnimatedWebpFixtureResult fixture{};
    expect(build_mixed_animated_webp_fixture(Slice<u8>{encoded, sizeof(encoded)}, &fixture),
           "animated_webp_mixed_fixture_build");
    if (fixture.encoded_bytes == 0) {
        return;
    }

    AnimatedWebpWorkspace animation_workspace{};
    AnimatedWebpImage animation{};
    const AnimatedWebpParseResult parsed = animated_webp_parse(
        ConstSlice<u8>{encoded, fixture.encoded_bytes}, &animation, &animation_workspace);
    expect(parsed.ok() && parsed.chunks == 5 && parsed.frame_chunks == 4 && animation.frame_count == 3 &&
               animation.play_count == 1 && animation.width == 4 && animation.height == 4 &&
               animation.has_alpha && animation.frames[0].codec == AnimatedWebpCodec::vp8l_lossless &&
               animation.frames[1].codec == AnimatedWebpCodec::vp8_lossy &&
               animation.frames[1].dispose == AnimatedWebpDispose::background &&
               !animation.frames[1].has_alpha &&
               animation.frames[2].codec == AnimatedWebpCodec::vp8_lossy && animation.frames[2].has_alpha,
           "animated_webp_mixed_exact_container");

    bool decoded_exact = parsed.ok();
    for (u32 index = 0; index < 3 && decoded_exact; ++index) {
        Vp8Keyframe vp8_frame{};
        Vp8ControlHeader vp8_header{};
        const AnimatedWebpDecodeResult decoded = animated_webp_decode_frame(
            ConstSlice<u8>{encoded, fixture.encoded_bytes}, index, &animation, &animation_workspace,
            &g_vp8l_workspace, &g_lossy_webp_workspace, &vp8_frame, &vp8_header,
            Slice<u8>{g_webp_rgba, sizeof(g_webp_rgba)}, Slice<u8>{g_webp_rgba, 64},
            Slice<u8>{g_webp_rgba + 64, sizeof(g_webp_rgba) - 64});
        decoded_exact = decoded.ok() && decoded.duration_us == (30u + index * 10u) * 1'000u;
        if (index == 0) {
            decoded_exact = decoded_exact && decoded.frame_decode.rgba_bytes == 64;
            for (u32 pixel = 0; pixel < 16; ++pixel) {
                const u32 actual = static_cast<u32>(g_webp_rgba[pixel * 4]) |
                                   static_cast<u32>(g_webp_rgba[pixel * 4 + 1]) << 8 |
                                   static_cast<u32>(g_webp_rgba[pixel * 4 + 2]) << 16 |
                                   static_cast<u32>(g_webp_rgba[pixel * 4 + 3]) << 24;
                decoded_exact = decoded_exact && actual == 0xFF0000FFu;
            }
        } else {
            decoded_exact = decoded_exact && decoded.lossy_decode.ok() && vp8_frame.width == 4 &&
                            vp8_frame.height == 4 && vp8_header.loop_filter.level == 5 &&
                            g_lossy_webp_workspace.modes.macroblock_count == 1 &&
                            g_lossy_webp_workspace.coefficients.macroblock_count == 1 &&
                            decoded.lossy_decode.alpha_bytes == (index == 2 ? 16u : 0u);
            if (index == 2) {
                for (u32 pixel = 0; pixel < 16; ++pixel) {
                    decoded_exact = decoded_exact && g_webp_rgba[64 + pixel] == 128;
                }
            }
        }
    }
    expect(decoded_exact, "animated_webp_mixed_all_frames_decode");

    const AnimatedWebpFrame alpha_frame = animation.frames[2];
    const u32 second_child = alpha_frame.payload_offset + 26;
    encoded[second_child] = 'A';
    encoded[second_child + 1] = 'L';
    encoded[second_child + 2] = 'P';
    encoded[second_child + 3] = 'H';
    expect(animated_webp_parse(ConstSlice<u8>{encoded, fixture.encoded_bytes}, &animation,
                               &animation_workspace)
                   .error == AnimatedWebpError::duplicate_chunk,
           "animated_webp_mixed_duplicate_bitstream_reject");
}

void test_animated_webp_opaque_alpha_hint() noexcept {
    u8 encoded[256]{};
    AnimatedWebpFixtureResult fixture{};
    expect(build_opaque_lossy_animated_webp_fixture({encoded, sizeof(encoded)}, &fixture),
           "animated_webp_opaque_alpha_hint_fixture_build");
    if (fixture.encoded_bytes == 0) {
        return;
    }
    AnimatedWebpWorkspace workspace{};
    AnimatedWebpImage animation{};
    const ConstSlice<u8> bytes{encoded, fixture.encoded_bytes};
    const AnimatedWebpParseResult parsed = animated_webp_parse(bytes, &animation, &workspace);
    expect(parsed.ok() && animation.has_alpha && animation.frame_count == 2 &&
               animation.play_count == 2 && animation.width == 4 && animation.height == 4 &&
               !animation.frames[0].has_alpha && !animation.frames[1].has_alpha &&
               animation.frames[0].blend == AnimatedWebpBlend::source &&
               animation.frames[1].dispose == AnimatedWebpDispose::background,
           "animated_webp_redundant_alpha_hint_preserves_actual_frame_metadata");

    bool all_frames_decoded = parsed.ok();
    for (u32 index = 0; index < 2 && all_frames_decoded; ++index) {
        Vp8Keyframe frame{};
        Vp8ControlHeader header{};
        // Opaque frames must require neither VP8L nor alpha/output scratch,
        // even when the container advertises the redundant alpha feature.
        const AnimatedWebpDecodeResult decoded = animated_webp_decode_parsed_frame(
            bytes, animation, index, nullptr, &g_lossy_webp_workspace, &frame, &header, {}, {}, {});
        all_frames_decoded = decoded.ok() && decoded.lossy_decode.ok() &&
                             decoded.lossy_decode.alpha_bytes == 0 &&
                             !decoded.frame_image.has_alpha &&
                             frame.width == 4 && frame.height == 4 &&
                             g_lossy_webp_workspace.modes.macroblock_count == 1 &&
                             g_lossy_webp_workspace.coefficients.macroblock_count == 1 &&
                             decoded.duration_us == (20u + index * 20u) * 1'000u;
    }
    expect(all_frames_decoded, "animated_webp_redundant_alpha_hint_all_opaque_frames_decode");

    encoded[20] &= static_cast<u8>(~0x10u);
    expect(animated_webp_parse(bytes, &animation, &workspace).ok() && !animation.has_alpha &&
               animation.frame_count == 2,
           "animated_webp_opaque_animation_also_accepts_exact_alpha_hint");
    encoded[20] |= 0x10u;
    bool prefixes_rejected = true;
    for (u32 length = 0; length < fixture.encoded_bytes; ++length) {
        prefixes_rejected = prefixes_rejected &&
                            !animated_webp_parse({encoded, length}, &animation, &workspace).ok();
    }
    expect(prefixes_rejected, "animated_webp_redundant_alpha_hint_keeps_exact_container_bounds");
}

void test_apng_decode() noexcept {
    ApngFixtureResult fixture{};
    expect(build_rgba8_apng_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                    Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &fixture),
           "apng_fixture_build");
    if (fixture.encoded_bytes == 0) {
        return;
    }

    ApngWorkspace apng_workspace{};
    ApngImage animation{};
    const ApngParseResult parsed =
        apng_parse(ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes}, &animation, &apng_workspace);
    expect(parsed.ok() && parsed.chunks == 13 && parsed.sequence_chunks == 8 &&
               animation.frame_count == 4 && animation.play_count == 2 && animation.default_in_animation &&
               animation.default_span_count == 2 && animation.frames[0].span_count == 2 &&
               animation.frames[1].span_count == 2 &&
               animation.frames[1].dispose == ApngDispose::previous &&
               animation.frames[1].blend == ApngBlend::over &&
               animation.frames[2].dispose == ApngDispose::background && animation.frames[3].width == 1 &&
               animation.frames[3].x_offset == 3,
           "apng_exact_chunk_sequence_and_controls");

    const u32 widths[4] = {4, 2, 2, 1};
    const u32 heights[4] = {4, 2, 2, 1};
    const u64 durations[4] = {50'000, 10'000, 10'000, 16'667};
    bool all_frames_match = parsed.ok();
    DeflateWorkspace deflate_workspace{};
    for (u32 frame_index = 0; frame_index < 4 && all_frames_match; ++frame_index) {
        PngImage frame_image{};
        const ApngDecodeResult decoded = apng_decode_parsed_frame(
            ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes}, animation, frame_index,
            &deflate_workspace, &frame_image, Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)});
        all_frames_match = decoded.ok() && decoded.duration_us == durations[frame_index] &&
                           frame_image.width == widths[frame_index] &&
                           frame_image.height == heights[frame_index] && frame_image.color_type == 6 &&
                           frame_image.bit_depth == 8;
        for (u32 y = 0; y < frame_image.height && all_frames_match; ++y) {
            const u8* row = g_png_scanlines + y * frame_image.row_stride;
            all_frames_match = row[0] == 0;
            for (u32 x = 0; x < frame_image.width && all_frames_match; ++x) {
                const u8* pixel = row + 1 + x * 4;
                const u32 actual = static_cast<u32>(pixel[0]) | static_cast<u32>(pixel[1]) << 8 |
                                   static_cast<u32>(pixel[2]) << 16 | static_cast<u32>(pixel[3]) << 24;
                all_frames_match = actual == apng_fixture_source_rgba(frame_index, x, y);
            }
        }
    }
    expect(all_frames_match, "apng_all_frames_independent_zlib_and_exact_rgba");

    PngImage default_image{};
    const PngDecodeResult default_decoded = apng_decode_parsed_default(
        ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes}, animation, &deflate_workspace, &default_image,
        Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)});
    expect(default_decoded.ok() && default_image.width == 4 && default_image.height == 4 &&
               default_image.idat_span_count == 2,
           "apng_default_png_stream_validated");

    u8 malformed[2'048]{};
    expect(fixture.encoded_bytes <= sizeof(malformed), "apng_fixture_mutation_capacity");
    if (fixture.encoded_bytes > sizeof(malformed)) {
        return;
    }
    copy_bytes(malformed, g_png_encoded, fixture.encoded_bytes);
    const u32 actl = test_find_png_chunk(ConstSlice<u8>{malformed, fixture.encoded_bytes}, 0x6163544Cu, 0);
    const u32 first_fctl =
        test_find_png_chunk(ConstSlice<u8>{malformed, fixture.encoded_bytes}, 0x6663544Cu, 0);
    const u32 second_fctl =
        test_find_png_chunk(ConstSlice<u8>{malformed, fixture.encoded_bytes}, 0x6663544Cu, 1);
    expect(actl != 0xFFFFFFFFu && first_fctl != 0xFFFFFFFFu && second_fctl != 0xFFFFFFFFu,
           "apng_fixture_chunk_offsets");

    png_fixture_write_be32(malformed + second_fctl + 8, 99);
    test_rewrite_png_chunk_crc(Slice<u8>{malformed, fixture.encoded_bytes}, second_fctl);
    expect(
        apng_parse(ConstSlice<u8>{malformed, fixture.encoded_bytes}, &animation, &apng_workspace).error ==
            ApngError::frame_sequence,
        "apng_sequence_gap_reject");

    copy_bytes(malformed, g_png_encoded, fixture.encoded_bytes);
    png_fixture_write_be32(malformed + second_fctl + 12, 999);
    test_rewrite_png_chunk_crc(Slice<u8>{malformed, fixture.encoded_bytes}, second_fctl);
    expect(
        apng_parse(ConstSlice<u8>{malformed, fixture.encoded_bytes}, &animation, &apng_workspace).error ==
            ApngError::frame_control,
        "apng_frame_rectangle_outside_canvas_reject");

    copy_bytes(malformed, g_png_encoded, fixture.encoded_bytes);
    png_fixture_write_be32(malformed + actl + 8, kApngFrameCapacity + 1);
    test_rewrite_png_chunk_crc(Slice<u8>{malformed, fixture.encoded_bytes}, actl);
    expect(
        apng_parse(ConstSlice<u8>{malformed, fixture.encoded_bytes}, &animation, &apng_workspace).error ==
            ApngError::frame_limit,
        "apng_declared_frame_capacity_reject");

    copy_bytes(malformed, g_png_encoded, fixture.encoded_bytes);
    malformed[first_fctl + 8 + 24] = 2;
    test_rewrite_png_chunk_crc(Slice<u8>{malformed, fixture.encoded_bytes}, first_fctl);
    const ApngParseResult normalized =
        apng_parse(ConstSlice<u8>{malformed, fixture.encoded_bytes}, &animation, &apng_workspace);
    expect(normalized.ok() && animation.frames[0].dispose == ApngDispose::background,
           "apng_first_previous_normalizes_to_background");

    copy_bytes(malformed, g_png_encoded, fixture.encoded_bytes);
    malformed[first_fctl + 10] ^= 1;
    expect(
        apng_parse(ConstSlice<u8>{malformed, fixture.encoded_bytes}, &animation, &apng_workspace).error ==
            ApngError::chunk_crc,
        "apng_chunk_crc_reject");

    bool all_prefixes_rejected = true;
    for (u32 prefix = 0; prefix < fixture.encoded_bytes; ++prefix) {
        all_prefixes_rejected =
            all_prefixes_rejected &&
            !apng_parse(ConstSlice<u8>{g_png_encoded, prefix}, &animation, &apng_workspace).ok();
    }
    expect(all_prefixes_rejected, "apng_all_proper_prefixes_reject");

    ApngFixtureResult excluded_fixture{};
    expect(build_rgba8_apng_default_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                            Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &excluded_fixture),
           "apng_excluded_default_fixture_build");
    const ApngParseResult excluded_parsed = apng_parse(
        ConstSlice<u8>{g_png_encoded, excluded_fixture.encoded_bytes}, &animation, &apng_workspace);
    PngImage excluded_default_image{};
    const PngDecodeResult excluded_default = apng_decode_parsed_default(
        ConstSlice<u8>{g_png_encoded, excluded_fixture.encoded_bytes}, animation, &deflate_workspace,
        &excluded_default_image, Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)});
    const u8* default_first_pixel = g_png_scanlines + 1;
    const u32 default_first_rgba =
        static_cast<u32>(default_first_pixel[0]) | static_cast<u32>(default_first_pixel[1]) << 8 |
        static_cast<u32>(default_first_pixel[2]) << 16 | static_cast<u32>(default_first_pixel[3]) << 24;
    PngImage excluded_frame_image{};
    const ApngDecodeResult excluded_frame = apng_decode_parsed_frame(
        ConstSlice<u8>{g_png_encoded, excluded_fixture.encoded_bytes}, animation, 0, &deflate_workspace,
        &excluded_frame_image, Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)});
    const u8* animation_pixel = g_png_scanlines + 1;
    const u32 animation_rgba =
        static_cast<u32>(animation_pixel[0]) | static_cast<u32>(animation_pixel[1]) << 8 |
        static_cast<u32>(animation_pixel[2]) << 16 | static_cast<u32>(animation_pixel[3]) << 24;
    expect(excluded_parsed.ok() && !animation.default_in_animation && animation.default_span_count == 1 &&
               animation.frame_count == 1 && animation.frames[0].x_offset == 3 &&
               animation.frames[0].y_offset == 3 && excluded_default.ok() &&
               excluded_default_image.width == 4 && excluded_default_image.height == 4 &&
               default_first_rgba == apng_fixture_source_rgba(0, 0, 0) && excluded_frame.ok() &&
               excluded_frame_image.width == 1 && excluded_frame_image.height == 1 &&
               animation_rgba == apng_fixture_source_rgba(3, 0, 0),
           "apng_excluded_default_and_frame_streams_decode");
}

void test_gif_decode() noexcept {
    GifFixtureResult fixture{};
    expect(build_indexed_gif_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                     Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &fixture),
           "gif_fixture_build");
    if (fixture.encoded_bytes == 0) {
        return;
    }

    GifWorkspace workspace{};
    GifImage image{};
    const GifParseResult parsed =
        gif_parse(ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes}, &image, &workspace);
    expect(parsed.ok() && image.canvas_width == 4 && image.canvas_height == 4 && image.frame_count == 4 &&
               image.play_count == 3 && image.has_global_palette && image.global_palette_count == 4 &&
               image.background_index == 0 && image.background_rgba == gif_fixture_palette_rgba(0, 0) &&
               image.frames[0].dispose == GifDispose::none &&
               image.frames[1].dispose == GifDispose::previous &&
               (image.frames[1].flags & gif_frame_local_palette) != 0 &&
               (image.frames[1].flags & gif_frame_transparent) != 0 &&
               image.frames[2].dispose == GifDispose::background &&
               (image.frames[2].flags & gif_frame_interlaced) != 0 && image.frames[3].x_offset == 3 &&
               parsed.extensions == 6,
           "gif_exact_blocks_controls_palette_and_loop_normalization");
    const u32 first_data_offset = image.frames[0].data_offset;
    const u32 second_descriptor = image.frames[1].data_offset - 23;

    const u32 widths[4] = {4, 2, 2, 1};
    const u32 heights[4] = {4, 2, 4, 1};
    const u64 durations[4] = {50'000, 100'000, 100'000, 20'000};
    bool all_frames_exact = parsed.ok();
    for (u32 frame = 0; frame < 4 && all_frames_exact; ++frame) {
        const GifDecodeResult decoded =
            gif_decode_parsed_frame(ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes}, image, frame,
                                    &workspace, Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)});
        all_frames_exact = decoded.ok() && decoded.frame.width == widths[frame] &&
                           decoded.frame.height == heights[frame] &&
                           decoded.duration_us == durations[frame] && decoded.palette.count == 4 &&
                           decoded.palette.background_rgba == gif_fixture_palette_rgba(0, 0);
        if (frame == 1) {
            all_frames_exact = all_frames_exact && decoded.palette.has_transparency &&
                               decoded.palette.transparent_index == 0 &&
                               (decoded.palette.rgba[0] >> 24) == 0;
        }
        for (u32 y = 0; y < heights[frame] && all_frames_exact; ++y) {
            for (u32 x = 0; x < widths[frame] && all_frames_exact; ++x) {
                const u8 actual = g_png_scanlines[y * widths[frame] + x];
                all_frames_exact = actual == gif_fixture_source_index(frame, x, y);
            }
        }
        for (u32 palette_index = 0; palette_index < 4 && all_frames_exact; ++palette_index) {
            u32 expected = gif_fixture_palette_rgba(frame, palette_index);
            if (frame == 1 && palette_index == 0) {
                expected &= 0x00FFFFFFu;
            }
            all_frames_exact = decoded.palette.rgba[palette_index] == expected;
        }
    }
    expect(all_frames_exact, "gif_all_frames_exact_indices_local_palette_and_interlace");
    const GifFrame saved_frame = workspace.frames[0];
    workspace.frames[0].width = workspace.frames[0].height = 0xffff;
    const auto oversized = gif_decode_parsed_frame({g_png_encoded, fixture.encoded_bytes}, image, 0,
                                                  &workspace, {g_png_scanlines, 1});
    expect(oversized.error == GifError::output_capacity && oversized.pixels == 0xfffe0001u,
           "gif_16_bit_dimensions_multiply_without_signed_overflow");
    workspace.frames[0] = saved_frame;

    u8 malformed[1'024]{};
    expect(fixture.encoded_bytes <= sizeof(malformed), "gif_fixture_mutation_capacity");
    if (fixture.encoded_bytes > sizeof(malformed)) {
        return;
    }
    copy_bytes(malformed, g_png_encoded, fixture.encoded_bytes);
    malformed[0] ^= 1;
    expect(gif_parse(ConstSlice<u8>{malformed, fixture.encoded_bytes}, &image, &workspace).error ==
               GifError::signature,
           "gif_signature_reject");

    // Encoders (Pillow among them) write a transparent index past the frame's palette when no colour is
    // transparent; the GIF plays, as it does in browsers, with the frame opaque.
    copy_bytes(malformed, g_png_encoded, fixture.encoded_bytes);
    u32 control = ~0u;
    for (u32 at = 0; at + 8 <= fixture.encoded_bytes && control == ~0u; ++at)
        if (malformed[at] == 0x21 && malformed[at + 1] == 0xF9 && malformed[at + 2] == 4 && (malformed[at + 3] & 1))
            control = at;
    bool tolerated = control != ~0u;
    if (tolerated) {
        malformed[control + 6] = 4; // the transparent frame's palette holds four colours
        GifImage lenient{};
        const GifParseResult reparsed = gif_parse(ConstSlice<u8>{malformed, fixture.encoded_bytes}, &lenient, &workspace);
        const GifDecodeResult frame = gif_decode_parsed_frame(ConstSlice<u8>{malformed, fixture.encoded_bytes}, lenient, 1,
                                                              &workspace, Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)});
        tolerated = reparsed.ok() && lenient.frame_count == 4 && (lenient.frames[1].flags & gif_frame_transparent) == 0 &&
                    frame.ok() && !frame.palette.has_transparency;
        for (u32 y = 0; y < 2 && tolerated; ++y)
            for (u32 x = 0; x < 2 && tolerated; ++x)
                tolerated = g_png_scanlines[y * 2 + x] == gif_fixture_source_index(1, x, y);
    }
    expect(tolerated, "gif_transparent_index_past_palette_plays_opaque");
    gif_parse(ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes}, &image, &workspace);

    copy_bytes(malformed, g_png_encoded, fixture.encoded_bytes);
    const u32 first_descriptor_packed = first_data_offset - 2;
    malformed[first_descriptor_packed] |= 0x08;
    expect(gif_parse(ConstSlice<u8>{malformed, fixture.encoded_bytes}, &image, &workspace).error ==
               GifError::reserved_bits,
           "gif_image_reserved_bits_reject");

    copy_bytes(malformed, g_png_encoded, fixture.encoded_bytes);
    const u32 first_descriptor = first_descriptor_packed - 9;
    malformed[first_descriptor + 5] = 99;
    malformed[first_descriptor + 6] = 0;
    expect(gif_parse(ConstSlice<u8>{malformed, fixture.encoded_bytes}, &image, &workspace).error ==
               GifError::frame_rectangle,
           "gif_frame_rectangle_reject");

    copy_bytes(malformed, g_png_encoded, fixture.encoded_bytes);
    byte_store_le16(malformed + 6, 1024);byte_store_le16(malformed + 8, 1024);
    const u16 rectangle[]{257, 513, 511, 255};
    for(u32 field=0;field<4;++field)byte_store_le16(malformed+first_descriptor+1+field*2,rectangle[field]);
    expect(gif_parse({malformed, fixture.encoded_bytes}, &image, &workspace).ok() &&
               image.frames[0].x_offset == 257 && image.frames[0].y_offset == 513 &&
               image.frames[0].width == 511 && image.frames[0].height == 255,
           "gif_rectangle_preserves_each_16_bit_descriptor_field");

    copy_bytes(malformed, g_png_encoded, fixture.encoded_bytes);
    const u32 second_control_packed = second_descriptor - 8 + 3;
    malformed[second_control_packed] = static_cast<u8>((malformed[second_control_packed] & ~0x1Cu) | 0x10u);
    expect(gif_parse(ConstSlice<u8>{malformed, fixture.encoded_bytes}, &image, &workspace).ok() &&
               image.frames[1].dispose == GifDispose::previous,
           "gif_legacy_disposal_four_normalizes_to_previous");

    bool all_prefixes_rejected = true;
    for (u32 prefix = 0; prefix < fixture.encoded_bytes; ++prefix) {
        all_prefixes_rejected = all_prefixes_rejected &&
                                !gif_parse(ConstSlice<u8>{g_png_encoded, prefix}, &image, &workspace).ok();
    }
    expect(all_prefixes_rejected, "gif_all_proper_prefixes_reject");

    copy_bytes(malformed, g_png_encoded, fixture.encoded_bytes);
    malformed[25 + 16] = 0;
    malformed[25 + 17] = 0;
    expect(gif_parse(ConstSlice<u8>{malformed, fixture.encoded_bytes}, &image, &workspace).ok() &&
               image.play_count == 0,
           "gif_netscape_zero_is_infinite");
    malformed[25 + 16] = 1;
    expect(gif_parse(ConstSlice<u8>{malformed, fixture.encoded_bytes}, &image, &workspace).ok() &&
               image.play_count == 2,
           "gif_netscape_positive_is_extra_repetitions");

    for (u32 index = 25; index + 19 < fixture.encoded_bytes; ++index) {
        malformed[index] = g_png_encoded[index + 19];
    }
    const u32 without_loop_bytes = fixture.encoded_bytes - 19;
    expect(gif_parse(ConstSlice<u8>{malformed, without_loop_bytes}, &image, &workspace).ok() &&
               image.play_count == 1,
           "gif_absent_loop_extension_is_one_total_play");

    const GifParseResult restored =
        gif_parse(ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes}, &image, &workspace);
    copy_bytes(malformed, g_png_encoded, fixture.encoded_bytes);
    malformed[first_data_offset + 1] = static_cast<u8>((malformed[first_data_offset + 1] & 0xF8u) | 1u);
    GifImage malformed_image{};
    const GifParseResult malformed_parsed =
        gif_parse(ConstSlice<u8>{malformed, fixture.encoded_bytes}, &malformed_image, &workspace);
    expect(restored.ok() && malformed_parsed.ok() &&
               gif_decode_parsed_frame(ConstSlice<u8>{malformed, fixture.encoded_bytes}, malformed_image, 0,
                                       &workspace, Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)})
                       .error == GifError::lzw_missing_clear,
           "gif_lzw_initial_clear_required");
    expect(gif_decode_parsed_frame(ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes}, image, 0,
                                   &workspace, Slice<u8>{g_png_scanlines, 15})
                   .error == GifError::output_capacity,
           "gif_index_output_capacity_reject");

    GifFixtureResult deferred{};
    expect(build_deferred_clear_gif_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                            Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &deferred),
           "gif_deferred_clear_fixture_build");
    const GifParseResult deferred_parsed =
        gif_parse(ConstSlice<u8>{g_png_encoded, deferred.encoded_bytes}, &image, &workspace);
    const GifDecodeResult deferred_decoded =
        gif_decode_parsed_frame(ConstSlice<u8>{g_png_encoded, deferred.encoded_bytes}, image, 0, &workspace,
                                Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)});
    bool deferred_exact = deferred_parsed.ok() && deferred_decoded.ok() && image.play_count == 1 &&
                          deferred_decoded.pixels == 8'192;
    for (u32 index = 0; index < 8'192 && deferred_exact; ++index) {
        deferred_exact = g_png_scanlines[index] == (index & 1u);
    }
    expect(deferred_exact, "gif_lzw_full_dictionary_deferred_clear_supported");

    // Each row carries its base-four index across the pixel columns.
    // This distinguishes row order across all four interlace passes.
    const u16 interlace_widths[] = {1, 3, 17};
    const u16 interlace_heights[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65};
    bool interlace_rows_exact = true;
    for (u16 width : interlace_widths) {
        for (u16 height : interlace_heights) {
            const u8 header[] = {
                'G', 'I', 'F', '8', '7', 'a', static_cast<u8>(width), 0,
                static_cast<u8>(height), 0, 0x81, 0, 0,
                0, 0, 0, 85, 85, 85, 170, 170, 170, 255, 255, 255,
                0x2C, 0, 0, 0, 0, static_cast<u8>(width), 0,
                static_cast<u8>(height), 0, 0x40, 2,
            };
            u32 compressed = 0, bits = 0, bit_count = 0;
            for (u32 stream_row = 0; stream_row < height; ++stream_row) {
                for (u32 x = 0; x < width; ++x) {
                    const u32 value = (stream_row >> ((x % 4) * 2)) & 3u;
                    bits |= (4u | (value << 3)) << bit_count;
                    bit_count += 6;
                    while (bit_count >= 8) {
                        g_png_zlib[compressed++] = static_cast<u8>(bits);
                        bits >>= 8;
                        bit_count -= 8;
                    }
                }
            }
            bits |= 5u << bit_count;
            bit_count += 3;
            while (bit_count != 0) {
                g_png_zlib[compressed++] = static_cast<u8>(bits);
                bits >>= 8;
                bit_count = bit_count > 8 ? bit_count - 8 : 0;
            }
            copy_bytes(g_png_encoded, header, sizeof(header));
            u32 cursor = sizeof(header), offset = 0;
            while (offset < compressed) {
                const u32 count = minimum(255u, compressed - offset);
                g_png_encoded[cursor++] = static_cast<u8>(count);
                copy_bytes(g_png_encoded + cursor, g_png_zlib + offset, count);
                cursor += count;
                offset += count;
            }
            g_png_encoded[cursor++] = 0;
            g_png_encoded[cursor++] = 0x3B;
            const u32 pixels = static_cast<u32>(width) * height;
            g_png_scanlines[pixels] = 0xCC;
            const auto decoded = gif_decode_frame({g_png_encoded, cursor}, 0, &image, &workspace,
                                                   {g_png_scanlines, pixels});
            interlace_rows_exact = interlace_rows_exact && decoded.ok() &&
                                   g_png_scanlines[pixels] == 0xCC;
            constexpr u32 starts[] = {0, 4, 2, 1}, steps[] = {8, 8, 4, 2};
            u32 stream_row = 0;
            for (u32 pass = 0; pass < 4; ++pass) {
                for (u32 y = starts[pass]; y < height; y += steps[pass], ++stream_row) {
                    for (u32 x = 0; x < width; ++x) {
                        interlace_rows_exact = interlace_rows_exact &&
                            g_png_scanlines[y * width + x] == ((stream_row >> ((x % 4) * 2)) & 3u);
                    }
                }
            }
        }
    }
    expect(interlace_rows_exact, "gif_interlace_unique_row_order_small_and_odd_heights");
}

void test_png_decode() noexcept {
    PngFixtureResult fixture{};
    expect(build_rgba8_png_fixture(64, 64, Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                   Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &fixture),
           "png_fixture_build");

    PngImage image{};
    DeflateWorkspace workspace{};
    const PngDecodeResult decoded =
        png_decode_scanlines(ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes}, &image, &workspace,
                             Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)});
    expect(decoded.ok() && decoded.parse.chunks == 4 && image.width == 64 && image.height == 64 &&
               image.color_type == 6 && image.bit_depth == 8 && image.channels == 4 &&
               image.bytes_per_pixel == 4 && image.row_bytes == 256 && image.row_stride == 257 &&
               image.inflated_bytes == fixture.filtered_bytes &&
               image.compressed_bytes == fixture.zlib_bytes && image.idat_span_count == 2 &&
               decoded.inflate.output_bytes == fixture.filtered_bytes &&
               decoded.reconstructed_bytes == fixture.filtered_bytes,
           "png_rgba8_segmented_idat_decode");

    bool pixels_match = decoded.ok();
    for (u32 y = 0; y < image.height && pixels_match; ++y) {
        const u8* row = g_png_scanlines + y * image.row_stride;
        pixels_match = row[0] == 0;
        for (u32 x = 0; x < image.width && pixels_match; ++x) {
            const u32 expected = png_fixture_rgba(x, y, image.width, image.height);
            const u8* pixel = row + 1 + x * 4;
            const u32 actual = static_cast<u32>(pixel[0]) | static_cast<u32>(pixel[1]) << 8 |
                               static_cast<u32>(pixel[2]) << 16 | static_cast<u32>(pixel[3]) << 24;
            pixels_match = actual == expected;
        }
    }
    expect(pixels_match, "png_all_five_filters_reconstruct_pixels");

    g_png_encoded[0] ^= 1;
    expect(png_parse(ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes}, &image).error ==
               PngError::signature,
           "png_signature_reject");
    g_png_encoded[0] ^= 1;

    g_png_encoded[16] ^= 1;
    expect(png_parse(ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes}, &image).error ==
               PngError::chunk_crc,
           "png_chunk_crc_reject");
    g_png_encoded[16] ^= 1;

    constexpr u8 apng_type[4] = {'a', 'c', 'T', 'L'};
    for (u32 index = 0; index < 4; ++index) {
        g_png_encoded[37 + index] = apng_type[index];
    }
    png_fixture_write_be32(g_png_encoded + 52, png_fixture_crc32(ConstSlice<u8>{g_png_encoded + 37, 15}));
    expect(png_parse(ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes}, &image).error ==
               PngError::unsupported_animation,
           "png_apng_explicit_reject");

    constexpr u8 critical_type[4] = {'A', 'B', 'C', 'D'};
    for (u32 index = 0; index < 4; ++index) {
        g_png_encoded[37 + index] = critical_type[index];
    }
    png_fixture_write_be32(g_png_encoded + 52, png_fixture_crc32(ConstSlice<u8>{g_png_encoded + 37, 15}));
    expect(png_parse(ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes}, &image).error ==
               PngError::unknown_critical_chunk,
           "png_unknown_critical_chunk_reject");
    expect(build_rgba8_png_fixture(64, 64, Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                   Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &fixture),
           "png_fixture_restore_after_chunk_types");

    const u32 second_idat_length = fixture.zlib_bytes - 11;
    const u32 second_idat_data = 64;
    const u32 second_idat_crc = second_idat_data + second_idat_length;
    g_png_encoded[second_idat_data + second_idat_length - 1] ^= 1;
    png_fixture_write_be32(g_png_encoded + second_idat_crc,
                           png_fixture_crc32(ConstSlice<u8>{g_png_encoded + 60, second_idat_length + 4}));
    expect(png_decode_scanlines(ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes}, &image, &workspace,
                                Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)})
                   .error == PngError::inflate,
           "png_zlib_checksum_reject_after_chunk_crc");
    expect(build_rgba8_png_fixture(64, 64, Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                   Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &fixture),
           "png_fixture_restore_after_checksum");

    expect(png_decode_scanlines(ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes}, &image, &workspace,
                                Slice<u8>{g_png_scanlines, fixture.filtered_bytes - 1})
                   .error == PngError::inflated_limit,
           "png_scanline_capacity_reject");

    PngFixtureResult malformed{};
    expect(build_png_format_fixture(6, 8, 9, 3, Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                    Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &malformed),
           "png_malformed_fixture_build");
    const u32 idat_data = 41;
    const u32 filtered_data = idat_data + 7;
    const u32 idat_crc = idat_data + malformed.zlib_bytes;
    g_png_encoded[filtered_data] = 5;
    png_fixture_write_be32(
        g_png_encoded + idat_data + malformed.zlib_bytes - 4,
        png_fixture_adler32(ConstSlice<u8>{g_png_encoded + filtered_data, malformed.filtered_bytes}));
    png_fixture_write_be32(g_png_encoded + idat_crc,
                           png_fixture_crc32(ConstSlice<u8>{g_png_encoded + 37, malformed.zlib_bytes + 4}));
    expect(png_decode_scanlines(ConstSlice<u8>{g_png_encoded, malformed.encoded_bytes}, &image, &workspace,
                                Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)})
                   .error == PngError::filter,
           "png_unknown_row_filter_reject");

    expect(build_png_format_fixture(6, 8, 9, 3, Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                    Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &malformed),
           "png_reserved_chunk_fixture_restore");
    g_png_encoded[39] = 'a';
    png_fixture_write_be32(g_png_encoded + idat_crc,
                           png_fixture_crc32(ConstSlice<u8>{g_png_encoded + 37, malformed.zlib_bytes + 4}));
    expect(png_parse(ConstSlice<u8>{g_png_encoded, malformed.encoded_bytes}, &image).error ==
               PngError::chunk_type,
           "png_reserved_chunk_type_bit_reject");

    expect(build_png_format_fixture(3, 8, 9, 3, Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                    Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &malformed),
           "png_palette_index_fixture_build");
    constexpr u32 indexed_idat_data = 133;
    constexpr u32 indexed_filtered_data = indexed_idat_data + 7;
    g_png_encoded[indexed_filtered_data + 1] = 17;
    png_fixture_write_be32(g_png_encoded + indexed_idat_data + malformed.zlib_bytes - 4,
                           png_fixture_adler32(ConstSlice<u8>{g_png_encoded + indexed_filtered_data,
                                                              malformed.filtered_bytes}));
    png_fixture_write_be32(
        g_png_encoded + indexed_idat_data + malformed.zlib_bytes,
        png_fixture_crc32(ConstSlice<u8>{g_png_encoded + indexed_idat_data - 4, malformed.zlib_bytes + 4}));
    expect(png_decode_scanlines(ConstSlice<u8>{g_png_encoded, malformed.encoded_bytes}, &image, &workspace,
                                Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)})
                   .error == PngError::palette_index,
           "png_out_of_range_palette_index_reject");
}

// The first colour of image_palette gives what the official client's median cut gives for the same pixels (values taken
// from its quantizer): a gradient; one colour (its 5-bit cell's centre); skipped translucent and near-white pixels
// (black when nothing is left); a picture with alpha; the inverted boxes of one and two sampled pixels; and 250 grey,
// which is not near-white and outweighs a larger green area by population times volume.
void test_image_dominant_color() noexcept {
    static u8 rgba[64 * 64 * 4];
    static u32 histogram[kImageDominantColorHistogramEntries];
    constexpr u32 entries = kImageDominantColorHistogramEntries;
    // `pixel` gives 0xRRGGBBAA.
    const auto fill = [](u32 width, u32 height, auto&& pixel) noexcept {
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width; ++x) {
                const u32 value = pixel(x, y);
                u8* p = rgba + (y * width + x) * 4;
                p[0] = static_cast<u8>(value >> 24);
                p[1] = static_cast<u8>(value >> 16);
                p[2] = static_cast<u8>(value >> 8);
                p[3] = static_cast<u8>(value);
            }
    };
    const auto dominant = [&](u32 width, u32 height, auto&& pixel) noexcept {
        fill(width, height, pixel);
        u32 colors[kImagePaletteColors];
        return image_palette({rgba, width * height * 4}, width, height, {histogram, entries}, colors) ? colors[0] : 0;
    };
    constexpr u32 green = 0x088959FFu;
    expect(dominant(64, 64, [](u32 x, u32 y) { return (x * 4) << 24 | (y * 4) << 16 | (x + y) * 2 << 8 | 255u; }) ==
               0xB8A0AA,
           "dominant_color_gradient");
    expect(dominant(16, 16, [](u32, u32) { return green; }) == 0x0C8C5C, "dominant_color_one_colour_cell_centre");
    expect(dominant(16, 16, [](u32 x, u32) { return x < 8 ? 0xFFFFFFFFu : 0x0A141E64u; }) == 0,
           "dominant_color_white_and_translucent_skipped");
    expect(dominant(40, 40,
                    [](u32 x, u32 y) { return x < 30 ? green : y < 20 ? 0xFFFFFFFFu : 0x141418FFu; }) == 0x0C8C5C,
           "dominant_color_largest_area");
    expect(dominant(50, 30,
                    [](u32 x, u32 y) {
                        return (x * 37 % 256) << 24 | (y * 53 % 256) << 16 | (x * y * 11 % 256) << 8 |
                               ((x + y) % 7 ? 255u : 100u);
                    }) == 0x41422F,
           "dominant_color_mixed_alpha");
    expect(dominant(1, 1, [](u32, u32) { return green; }) == 0x084830, "dominant_color_one_pixel");
    expect(dominant(11, 1, [](u32 x, u32) { return x == 0 ? green : x == 10 ? 0xC8281EFFu : 0u; }) == 0x6C1810,
           "dominant_color_two_pixels");
    expect(dominant(48, 48,
                    [](u32 x, u32 y) {
                        return x + y < 40 ? green : x * y % 5 == 0 ? 0xFAFAFAFFu : 0x111214FFu;
                    }) == 0xFCFCFC,
           "dominant_color_population_times_volume");
    // The same 16x16 pixels (the last picture's first rows hold green) with a short histogram or pixel buffer.
    u32 colors[kImagePaletteColors];
    expect(image_palette({rgba, 1024}, 16, 16, {histogram, entries}, colors) != 0 &&
               image_palette({rgba, 1024}, 16, 16, {histogram, entries - 1}, colors) == 0 &&
               image_palette({rgba, 1023}, 16, 16, {histogram, entries}, colors) == 0,
           "dominant_color_rejects_short_buffers");
    // The whole palette (Discord's quantizer gives five colours, repeats included; the two-pixel picture one) and
    // the companion a single-colour palette's theme takes as its second colour (the client's first suggestion).
    const auto palette = [&](u32 width, u32 height, auto&& pixel, u32 count, const u32* want) noexcept {
        fill(width, height, pixel);
        u32 got[kImagePaletteColors]{};
        if (image_palette({rgba, width * height * 4}, width, height, {histogram, entries}, got) != count)
            return false;
        for (u32 i = 0; i < count; ++i)
            if (got[i] != want[i])
                return false;
        return true;
    };
    constexpr u32 gradient[]{0xB8A0AA, 0x1F7F4D, 0xA01F5D, 0x58A07A, 0x444444};
    constexpr u32 area[]{0x0C8C5C, 0x14141C, 0x10903C, 0x10903C, 0x10903C};
    constexpr u32 mixed[]{0x41422F, 0x84DE58, 0x9B5FD4, 0xD95B4E, 0x3BA93B};
    constexpr u32 two[]{0x6C1810};
    expect(palette(64, 64, [](u32 x, u32 y) { return (x * 4) << 24 | (y * 4) << 16 | (x + y) * 2 << 8 | 255u; }, 5,
                   gradient) &&
               palette(40, 40, [](u32 x, u32 y) { return x < 30 ? green : y < 20 ? 0xFFFFFFFFu : 0x141418FFu; }, 5,
                       area) &&
               palette(50, 30,
                       [](u32 x, u32 y) {
                           return (x * 37 % 256) << 24 | (y * 53 % 256) << 16 | (x * y * 11 % 256) << 8 |
                                  ((x + y) % 7 ? 255u : 100u);
                       },
                       5, mixed) &&
               palette(11, 1, [](u32 x, u32) { return x == 0 ? green : x == 10 ? 0xC8281EFFu : 0u; }, 1, two),
           "dominant_color_palette_matches_the_client");
    expect(image_palette_companion(0x6C1810) == 0x46106C && image_palette_companion(0x0C8C5C) == 0x7B8C0C &&
               image_palette_companion(0) == 0x261B32 && image_palette_companion(0xFFFFFF) == 0x997AB8 &&
               image_palette_companion(0x808080) == 0x805AA6 && image_palette_companion(0xFF0000) == 0x8000FF &&
               image_palette_companion(0x123456) == 0x125612 && image_palette_companion(0x101010) == 0x362647,
           "dominant_color_single_colour_companion_matches_the_client");
}

} // namespace maicord
