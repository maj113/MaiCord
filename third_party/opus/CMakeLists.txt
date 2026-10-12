# Opus 1.6.1 decoder and voice encoder subsets; upstream bytes bound by upstream-manifest.json.
add_library(maicord_opus_decoder STATIC
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
    src/extensions.c
    src/opus.c
    src/opus_decoder.c
    maicord_support.c)
target_include_directories(maicord_opus_decoder PUBLIC include PRIVATE . celt silk src)
target_compile_features(maicord_opus_decoder PRIVATE c_std_17)
target_compile_definitions(maicord_opus_decoder PRIVATE
    OPUS_BUILD FIXED_POINT DISABLE_FLOAT_API USE_ALLOCA CUSTOM_SUPPORT
    ENABLE_HARDENING OVERRIDE_celt_fatal "PACKAGE_VERSION=\"1.6.1\"")
# Upstream C is isolated from the application's stricter C++ warnings policy.
# No compiler-inserted CRT libraries, security cookie, heap, or float math path.
target_compile_options(maicord_opus_decoder PRIVATE
    /W3 /Zl /GS- /Gy /Gw /GF /Oi /utf-8 /O2 /Ob1 /GL /Brepro)
target_link_libraries(maicord_opus_decoder PRIVATE maicord_link_contract)
set_target_properties(maicord_opus_decoder PROPERTIES
    C_EXTENSIONS OFF MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Encoder-only units for the account DLL's voice sender. Shared CELT/SILK
# objects come from the decoder subset; src/opus_encoder.c is compiled through
# maicord_encoder_profile.c (single 20 ms VBR frames, no repacketizer).
add_library(maicord_opus_encoder STATIC
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
    maicord_encoder_profile.c)
target_include_directories(maicord_opus_encoder PUBLIC include PRIVATE . celt silk silk/fixed src)
target_compile_features(maicord_opus_encoder PRIVATE c_std_17)
target_compile_definitions(maicord_opus_encoder PRIVATE
    OPUS_BUILD FIXED_POINT DISABLE_FLOAT_API USE_ALLOCA CUSTOM_SUPPORT
    ENABLE_HARDENING OVERRIDE_celt_fatal "PACKAGE_VERSION=\"1.6.1\"")
# Measured: /O1 removes about a fifth of the encoder code for under 1% SILK
# encode time; the CELT analysis unit stays /O2, where /O1 cost about 6%.
target_compile_options(maicord_opus_encoder PRIVATE
    /W3 /Zl /GS- /Gy /Gw /GF /Oi /utf-8 /O1 /GL /Brepro)
set_source_files_properties(celt/celt_encoder.c PROPERTIES COMPILE_OPTIONS "/O2;/Ob1")
target_link_libraries(maicord_opus_encoder PUBLIC maicord_opus_decoder PRIVATE maicord_link_contract)
set_target_properties(maicord_opus_encoder PROPERTIES
    C_EXTENSIONS OFF MSVC_RUNTIME_LIBRARY "MultiThreaded")
