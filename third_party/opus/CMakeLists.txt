# maiopus: fixed-point libopus 1.6.1 fork; provenance in upstream-manifest.json.
# A complete decoder plus a 48 kHz, 20 ms VBR voice encoder, freestanding
# (no CRT, no heap). The parent project may set:
#   MAIOPUS_DECODER_TARGET / MAIOPUS_ENCODER_TARGET  target names
#   MAIOPUS_LINK_CONTRACT  an INTERFACE target linked privately (link options)
#   MAIOPUS_LTCG           compile for whole-program optimization with
#                          MAIOPUS_LTCG_OPTION (default -flto)
#   MAIOPUS_SSE4_1         use the x86 SSE4.1 kernels (compile-time selection,
#                          no runtime CPU dispatch)
#   MAIOPUS_ARCH           extra code generation option, e.g. /arch:AVX
if(NOT DEFINED MAIOPUS_DECODER_TARGET)
    set(MAIOPUS_DECODER_TARGET maiopus_decoder)
endif()
if(NOT DEFINED MAIOPUS_ENCODER_TARGET)
    set(MAIOPUS_ENCODER_TARGET maiopus_encoder)
endif()

set(maiopus_definitions
    OPUS_BUILD FIXED_POINT DISABLE_FLOAT_API USE_ALLOCA CUSTOM_SUPPORT "PACKAGE_VERSION=\"1.6.1\"")
# Upstream C keeps its own warning level. No CRT libraries, security cookie,
# heap or float math path.
set(maiopus_options /W3 /Zl /GS- /Gy /Gw /GF /Oi /utf-8 /Brepro)
if(MAIOPUS_LTCG)
    # A literal option: test builds strip it with list(REMOVE_ITEM).
    if(NOT DEFINED MAIOPUS_LTCG_OPTION)
        set(MAIOPUS_LTCG_OPTION -flto)
    endif()
    list(APPEND maiopus_options ${MAIOPUS_LTCG_OPTION})
endif()
if(MAIOPUS_ARCH)
    list(APPEND maiopus_options ${MAIOPUS_ARCH})
endif()
set(maiopus_decoder_simd)
set(maiopus_encoder_simd)
if(MAIOPUS_SSE4_1)
    list(APPEND maiopus_definitions
        OPUS_X86_MAY_HAVE_SSE2 OPUS_X86_PRESUME_SSE2 OPUS_X86_MAY_HAVE_SSE4_1 OPUS_X86_PRESUME_SSE4_1)
    set(maiopus_decoder_simd celt/x86/celt_lpc_sse4_1.c celt/x86/pitch_sse4_1.c)
    set(maiopus_encoder_simd silk/x86/NSQ_sse4_1.c silk/x86/NSQ_del_dec_sse4_1.c silk/x86/VAD_sse4_1.c
        silk/x86/VQ_WMat_EC_sse4_1.c silk/fixed/x86/burg_modified_FIX_sse4_1.c silk/fixed/x86/vector_ops_FIX_sse4_1.c)
endif()

add_library(${MAIOPUS_DECODER_TARGET} STATIC
    celt/bands.c
    celt/celt.c
    celt/celt_decoder.c
    celt/celt_lpc.c
    celt/cwrs.c
    celt/entcode.c
    celt/entdec.c
    celt/entenc.c
    celt/kiss_fft.c
    celt/laplace.c
    celt/mathops.c
    celt/mdct.c
    celt/modes.c
    celt/pitch.c
    celt/quant_bands.c
    celt/rate.c
    celt/vq.c
    silk/CNG.c
    silk/LPC_analysis_filter.c
    silk/LPC_fit.c
    silk/LPC_inv_pred_gain.c
    silk/NLSF2A.c
    silk/NLSF_decode.c
    silk/NLSF_stabilize.c
    silk/NLSF_unpack.c
    silk/PLC.c
    silk/bwexpander.c
    silk/bwexpander_32.c
    silk/code_signs.c
    silk/dec_API.c
    silk/decode_core.c
    silk/decode_frame.c
    silk/decode_indices.c
    silk/decode_parameters.c
    silk/decode_pitch.c
    silk/decode_pulses.c
    silk/decoder_set_fs.c
    silk/gain_quant.c
    silk/init_decoder.c
    silk/inner_prod_aligned.c
    silk/lin2log.c
    silk/log2lin.c
    silk/pitch_est_tables.c
    silk/resampler.c
    silk/resampler_down2.c
    silk/resampler_down2_3.c
    silk/resampler_private_AR2.c
    silk/resampler_private_IIR_FIR.c
    silk/resampler_private_down_FIR.c
    silk/resampler_private_up2_HQ.c
    silk/resampler_rom.c
    silk/shell_coder.c
    silk/sort.c
    silk/stereo_MS_to_LR.c
    silk/stereo_decode_pred.c
    silk/sum_sqr_shift.c
    silk/table_LSF_cos.c
    silk/tables_LTP.c
    silk/tables_NLSF_CB_NB_MB.c
    silk/tables_NLSF_CB_WB.c
    silk/tables_gain.c
    silk/tables_other.c
    silk/tables_pitch_lag.c
    silk/tables_pulses_per_block.c
    src/opus.c
    src/opus_decoder.c
    ${maiopus_decoder_simd})
target_include_directories(${MAIOPUS_DECODER_TARGET} PUBLIC include PRIVATE . celt silk src)
target_compile_features(${MAIOPUS_DECODER_TARGET} PRIVATE c_std_17)
target_compile_definitions(${MAIOPUS_DECODER_TARGET} PRIVATE ${maiopus_definitions})
target_compile_options(${MAIOPUS_DECODER_TARGET} PRIVATE ${maiopus_options} /O2 /Ob1)
set_target_properties(${MAIOPUS_DECODER_TARGET} PROPERTIES
    C_EXTENSIONS OFF MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Encoder-only units. Shared CELT/SILK objects come from the decoder library.
add_library(${MAIOPUS_ENCODER_TARGET} STATIC
    celt/celt_encoder.c
    silk/A2NLSF.c
    silk/HP_variable_cutoff.c
    silk/LP_variable_cutoff.c
    silk/NLSF_VQ.c
    silk/NLSF_VQ_weights_laroia.c
    silk/NLSF_del_dec_quant.c
    silk/NLSF_encode.c
    silk/NSQ.c
    silk/NSQ_del_dec.c
    silk/VAD.c
    silk/VQ_WMat_EC.c
    silk/ana_filt_bank_1.c
    silk/biquad_alt.c
    silk/check_control_input.c
    silk/control_SNR.c
    silk/control_audio_bandwidth.c
    silk/control_codec.c
    silk/enc_API.c
    silk/encode_indices.c
    silk/encode_pulses.c
    silk/init_encoder.c
    silk/interpolate.c
    silk/process_NLSFs.c
    silk/quant_LTP_gains.c
    silk/sigm_Q15.c
    silk/stereo_LR_to_MS.c
    silk/stereo_encode_pred.c
    silk/stereo_find_predictor.c
    silk/stereo_quant_pred.c
    silk/fixed/LTP_analysis_filter_FIX.c
    silk/fixed/LTP_scale_ctrl_FIX.c
    silk/fixed/apply_sine_window_FIX.c
    silk/fixed/autocorr_FIX.c
    silk/fixed/burg_modified_FIX.c
    silk/fixed/corrMatrix_FIX.c
    silk/fixed/encode_frame_FIX.c
    silk/fixed/find_LPC_FIX.c
    silk/fixed/find_LTP_FIX.c
    silk/fixed/find_pitch_lags_FIX.c
    silk/fixed/find_pred_coefs_FIX.c
    silk/fixed/k2a_FIX.c
    silk/fixed/k2a_Q16_FIX.c
    silk/fixed/noise_shape_analysis_FIX.c
    silk/fixed/pitch_analysis_core_FIX.c
    silk/fixed/process_gains_FIX.c
    silk/fixed/residual_energy_FIX.c
    silk/fixed/schur64_FIX.c
    silk/fixed/schur_FIX.c
    silk/fixed/vector_ops_FIX.c
    silk/fixed/warped_autocorrelation_FIX.c
    src/opus_encoder.c
    ${maiopus_encoder_simd})
target_include_directories(${MAIOPUS_ENCODER_TARGET} PUBLIC include PRIVATE . celt silk silk/fixed src)
target_compile_features(${MAIOPUS_ENCODER_TARGET} PRIVATE c_std_17)
target_compile_definitions(${MAIOPUS_ENCODER_TARGET} PRIVATE ${maiopus_definitions})
# Measured: /O1 removes about a fifth of the encoder code for under 1% SILK
# encode time; the CELT analysis unit stays /O2, where /O1 cost about 6%.
target_compile_options(${MAIOPUS_ENCODER_TARGET} PRIVATE ${maiopus_options})
get_target_property(maiopus_encoder_sources ${MAIOPUS_ENCODER_TARGET} SOURCES)
list(REMOVE_ITEM maiopus_encoder_sources celt/celt_encoder.c)
set_source_files_properties(${maiopus_encoder_sources} PROPERTIES COMPILE_OPTIONS /O1)
set_source_files_properties(celt/celt_encoder.c PROPERTIES COMPILE_OPTIONS "/O2;/Ob1")
target_link_libraries(${MAIOPUS_ENCODER_TARGET} PUBLIC ${MAIOPUS_DECODER_TARGET})
set_target_properties(${MAIOPUS_ENCODER_TARGET} PROPERTIES
    C_EXTENSIONS OFF MSVC_RUNTIME_LIBRARY "MultiThreaded")

if(MAIOPUS_LINK_CONTRACT)
    target_link_libraries(${MAIOPUS_DECODER_TARGET} PRIVATE ${MAIOPUS_LINK_CONTRACT})
    target_link_libraries(${MAIOPUS_ENCODER_TARGET} PRIVATE ${MAIOPUS_LINK_CONTRACT})
endif()
