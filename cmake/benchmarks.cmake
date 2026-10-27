# Opt-in freestanding benchmarks with shipping code generation (/GL, LTCG).
# Each tools/benchmark_*.ps1 configures the Release cache with
# -DMAICORD_BUILD_BENCHMARKS=ON under the build mutex, builds its one target,
# runs it and removes the outputs again.
option(MAICORD_BUILD_BENCHMARKS "Build opt-in freestanding benchmark executables" OFF)
if(NOT MAICORD_BUILD_BENCHMARKS)
    return()
endif()
get_property(maicord_targets_before DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)

# The *_observer.cpp translation units stay outside LTCG so the measured code
# cannot be folded into the benchmark harness.
foreach(maicord_observer IN ITEMS channel_name_preflight guild_channel_validation guild_role_payload
        text_projection)
    add_library(maicord_${maicord_observer}_observer OBJECT tests/bench/${maicord_observer}_observer.cpp)
    target_link_libraries(maicord_${maicord_observer}_observer PRIVATE maicord_compile_contract)
    set_target_properties(maicord_${maicord_observer}_observer PROPERTIES CXX_EXTENSIONS OFF
        MAICORD_NO_WHOLE_PROGRAM ON)
endforeach()
maicord_program(MaiCordChannelNamePreflightBenchmark ENTRY maicord_channel_name_preflight_benchmark_entry
    SOURCES tests/bench/channel_name_preflight.cpp $<TARGET_OBJECTS:maicord_channel_name_preflight_observer>
    LIBS maicord_core maicord_protocol maicord_model kernel32)
maicord_program(MaiCordGuildChannelValidationBenchmark ENTRY maicord_guild_channel_validation_benchmark_entry
    SOURCES tests/bench/guild_channel_validation.cpp $<TARGET_OBJECTS:maicord_guild_channel_validation_observer>
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_app_core maicord_message_runtime kernel32)
maicord_program(MaiCordGuildRolePayloadBenchmark ENTRY maicord_guild_role_payload_benchmark_entry
    SOURCES tests/bench/guild_role_payload.cpp $<TARGET_OBJECTS:maicord_guild_role_payload_observer>
    LIBS maicord_core maicord_platform maicord_protocol maicord_model maicord_app_core kernel32)
maicord_program(MaiCordTextProjectionBenchmark ENTRY maicord_text_projection_benchmark_entry
    SOURCES tests/bench/text_projection.cpp $<TARGET_OBJECTS:maicord_text_projection_observer>
    LIBS maicord_core maicord_platform maicord_ui maicord_ui_input maicord_model maicord_protocol maicord_app_core
        maicord_text kernel32 user32 crypt32)
maicord_program(MaiCordProcessBenchmark ENTRY maicord_process_benchmark_entry SOURCES benchmarks/process_benchmark.cpp
    LIBS maicord_core kernel32)
maicord_program(MaiCordImportBenchmark ENTRY maicord_import_benchmark_entry SOURCES benchmarks/import_benchmark.cpp
    LIBS kernel32 user32 shell32)
maicord_program(MaiCordGpuTextureStorageBenchmark ENTRY maicord_gpu_texture_storage_entry
    SOURCES benchmarks/gpu_texture_storage.cpp
    LIBS maicord_core kernel32 d3d11 dxgi)
maicord_program(MaiCordWindowFrameBenchmark ENTRY maicord_window_frame_benchmark_entry
    SOURCES benchmarks/window_frame_benchmark.cpp
    LIBS kernel32 user32 dwmapi d3d11 dxgi dcomp)
maicord_program(MaiCordPngCrcBenchmark ENTRY maicord_png_crc_entry SOURCES benchmarks/png_crc_benchmark.cpp
    LIBS maicord_core maicord_media_codec kernel32)
maicord_program(MaiCordResolverPublicationProbe ENTRY maicord_resolver_publication_entry
    SOURCES tests/resolver_publication.cpp
    LIBS maicord_core kernel32 ws2_32
    DEFINES MAICORD_RESOLVER_PUBLICATION_PROBE=1)
foreach(maicord_json_mode IN ITEMS Number Integer Ascii String)
    string(TOLOWER "${maicord_json_mode}" maicord_json_name)
    string(TOUPPER "${maicord_json_mode}" maicord_json_define)
    maicord_program(MaiCordJson${maicord_json_mode}Benchmark ENTRY maicord_json_${maicord_json_name}_entry
        SOURCES tests/json_string_decode.cpp
        LIBS maicord_core kernel32
        DEFINES MAICORD_JSON_${maicord_json_define}_BENCHMARK=1)
endforeach()
maicord_program(MaiCordReadyUsersBenchmark ENTRY maicord_ready_users_entry
    SOURCES tests/gateway_ready_users.cpp src/model/user_profile.cpp
    LIBS maicord_app_core maicord_core maicord_platform maicord_protocol maicord_media_codec maicord_model kernel32
        ${maicord_tls_libs}
    DEFINES MAICORD_READY_USERS_BENCHMARK=1)
maicord_program(MaiCordGatewayPipelineBenchmark ENTRY maicord_gateway_pipeline_benchmark_entry
    SOURCES benchmarks/gateway_pipeline_benchmark.cpp src/model/user_profile.cpp
    LIBS maicord_app_core maicord_core maicord_platform maicord_protocol maicord_media_codec maicord_model kernel32
        shell32 ${maicord_tls_libs}
    LINK_OPTIONS "/MAP:${CMAKE_CURRENT_BINARY_DIR}/MaiCordGatewayPipelineBenchmark.map")
maicord_program(MaiCordRestWorkerStorageBenchmark ENTRY maicord_rest_worker_storage_entry
    SOURCES benchmarks/rest_worker_storage.cpp
    LIBS maicord_core maicord_protocol maicord_platform maicord_media_codec maicord_app_core kernel32
        ${maicord_tls_libs})
maicord_program(MaiCordRestStorageBenchmark ENTRY maicord_rest_storage_entry SOURCES tests/rest_response_storage.cpp
    LIBS maicord_core maicord_protocol maicord_platform maicord_media_codec maicord_app_core kernel32
        ${maicord_tls_libs}
    DEFINES MAICORD_REST_STORAGE_BENCHMARK=1)
maicord_program(MaiCordArenaBenchmark ENTRY maicord_arena_benchmark_entry SOURCES benchmarks/arena_strategies.cpp
    LIBS maicord_core kernel32)
maicord_program(MaiCordHttpCompressionBenchmark ENTRY maicord_http_compression_entry
    SOURCES benchmarks/http_compression.cpp
    LIBS maicord_core maicord_media_codec maicord_protocol kernel32)
maicord_program(MaiCordTransportBenchmark ENTRY maicord_transport_benchmark_entry
    SOURCES benchmarks/transport_benchmark.cpp
    LIBS maicord_core maicord_protocol maicord_platform maicord_app_core kernel32 ${maicord_tls_libs})
maicord_program(MaiCordGcm64Stages ENTRY maicord_gcm64_stages_entry SOURCES benchmarks/gcm64_stages.cpp
    LIBS maicord_core maicord_platform kernel32 bcrypt crypt32)
maicord_program(MaiCordGcm64Benchmark ENTRY maicord_gcm64_benchmark_entry SOURCES benchmarks/gcm64_benchmark.cpp
    LIBS maicord_core maicord_platform kernel32 bcrypt crypt32)
maicord_program(MaiCordVoiceCryptoBenchmark ENTRY maicord_voice_crypto_benchmark_entry
    SOURCES benchmarks/voice_crypto_benchmark.cpp
    LIBS maicord_core maicord_platform kernel32 bcrypt crypt32)
maicord_program(MaiCordAttachmentUploadBenchmark ENTRY maicord_attachment_upload_benchmark_entry
    SOURCES benchmarks/attachment_upload.cpp benchmarks/local_attachment.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_message_runtime kernel32 ws2_32)
maicord_program(MaiCordRuntimeBenchmark ENTRY maicord_runtime_benchmark_entry SOURCES benchmarks/runtime_benchmark.cpp
    LIBS kernel32)
maicord_program(MaiCordCoreBenchmark ENTRY maicord_core_benchmark_entry
    SOURCES benchmarks/core_benchmark.cpp src/core/log.cpp src/app/message_draft_persistence.cpp
        src/app/message_draft_session.cpp src/app/file_persistence.cpp
    LIBS maicord_core maicord_protocol maicord_model kernel32)
maicord_program(MaiCordTextBenchmark ENTRY maicord_text_benchmark_entry
    SOURCES benchmarks/text_benchmark.cpp src/render/message_hit_test.cpp
    LIBS maicord_core maicord_text kernel32)
maicord_program(MaiCordMarkdownBenchmark ENTRY maicord_markdown_benchmark_entry
    SOURCES benchmarks/markdown_benchmark.cpp
    LIBS maicord_core maicord_ui kernel32)
maicord_program(MaiCordGlyphBenchmark ENTRY maicord_glyph_benchmark_entry
    SOURCES benchmarks/glyph_benchmark.cpp src/render/native_glyph_outlines.cpp
    LIBS maicord_core maicord_text maicord_render_core maicord_atlas_gpu maicord_paint_program kernel32 d3d11)
maicord_program(MaiCordMediaBenchmark ENTRY maicord_media_benchmark_entry
    SOURCES benchmarks/media_benchmark.cpp tests/media_fixture.cpp
    LIBS maicord_core maicord_protocol maicord_render_core maicord_media_core maicord_media_codec maicord_cdn_media
        maicord_atlas_gpu kernel32 d3d11)
# tools/benchmark_opus.ps1: Opus encode/decode CPU per frame and bit-exactness digests.
maicord_program(MaiCordOpusBenchmark ENTRY maicord_opus_benchmark_entry SOURCES benchmarks/opus_benchmark.cpp
    LIBS maicord_core maicord_media_codec maicord_voice_encoder kernel32
    INCLUDES tests)
# tools/opus_conformance.ps1: the official Opus test vectors through the shipping decoder.
maicord_program(MaiCordOpusConformance ENTRY maicord_opus_conformance_entry SOURCES benchmarks/opus_conformance.cpp
    LIBS maicord_core maicord_media_codec maicord_opus_decoder kernel32 bcrypt
    INCLUDES tests third_party/opus/include)
# tools/benchmark_media_decode.ps1: CPU image/audio decode over a generated corpus.
maicord_program(MaiCordMediaDecodeBenchmark ENTRY maicord_media_decode_benchmark_entry
    SOURCES benchmarks/media_decode_benchmark.cpp
    LIBS maicord_core maicord_media_codec kernel32)
maicord_program(MaiCordUiBenchmark ENTRY maicord_ui_benchmark_entry
    SOURCES benchmarks/text_form.cpp benchmarks/text_entry.cpp benchmarks/widget_scroll.cpp benchmarks/input_router.cpp
        benchmarks/navigation_shortcuts.cpp benchmarks/private_channels.cpp benchmarks/private_order.cpp
        benchmarks/ui_benchmark.cpp src/render/content_text_retention.cpp src/render/text_edit_layout.cpp
        src/render/text_form_surface.cpp
    LIBS maicord_core maicord_platform maicord_ui maicord_ui_input maicord_model maicord_protocol maicord_app_core
        maicord_message_runtime maicord_message_media maicord_media_core maicord_render_core maicord_text kernel32
        user32 crypt32)
maicord_program(MaiCordModelBenchmark ENTRY maicord_model_benchmark_entry SOURCES benchmarks/model_benchmark.cpp
    LIBS maicord_core maicord_model kernel32)
maicord_program(MaiCordMessageEventBenchmark ENTRY maicord_message_event_benchmark_entry
    SOURCES benchmarks/message_event_benchmark.cpp
    LIBS maicord_core maicord_protocol kernel32)
maicord_program(MaiCordMailboxBenchmark ENTRY maicord_mailbox_benchmark_entry SOURCES benchmarks/mailbox_benchmark.cpp
    LIBS maicord_core kernel32)
maicord_program(MaiCordTimelineBenchmark ENTRY maicord_timeline_benchmark_entry
    SOURCES benchmarks/timeline_benchmark.cpp
    LIBS maicord_core maicord_ui maicord_model maicord_protocol maicord_app_core maicord_message_runtime kernel32)
maicord_program(MaiCordEventPumpBenchmark ENTRY maicord_event_pump_benchmark_entry
    SOURCES benchmarks/event_pump_benchmark.cpp
    LIBS maicord_core maicord_ui maicord_model maicord_protocol maicord_app_core maicord_message_runtime kernel32)
maicord_program(MaiCordRetentionBenchmark ENTRY maicord_retention_benchmark_entry
    SOURCES benchmarks/retention_benchmark.cpp
    LIBS maicord_core maicord_ui maicord_model maicord_protocol maicord_app_core maicord_message_runtime kernel32)
maicord_program(MaiCordHistoryBenchmark ENTRY maicord_history_benchmark_entry SOURCES benchmarks/history_benchmark.cpp
    LIBS maicord_core maicord_ui maicord_model maicord_protocol maicord_app_core maicord_message_runtime kernel32)
maicord_program(MaiCordChannelSwitchBenchmark ENTRY maicord_channel_switch_benchmark_entry
    SOURCES benchmarks/channel_switch_benchmark.cpp
    LIBS maicord_core maicord_ui maicord_model maicord_protocol maicord_app_core maicord_message_runtime kernel32)
maicord_program(MaiCordRestBenchmark ENTRY maicord_rest_benchmark_entry SOURCES benchmarks/rest_benchmark.cpp
    LIBS maicord_core maicord_protocol kernel32)
maicord_program(MaiCordAccountBenchmark ENTRY maicord_account_benchmark_entry
    SOURCES src/model/user_profile.cpp benchmarks/account_benchmark.cpp
    LIBS maicord_account_host maicord_account_paths maicord_core maicord_platform maicord_protocol maicord_app_core
        maicord_model kernel32 ${maicord_tls_libs})
# Fresh-process glyph atlas and repeated-render observations
# (tools/measure_glyph_cache.py --side native|reference).
maicord_program(MaiCordGlyphCacheProbe WIN32 ENTRY maicord_glyph_cache_probe_entry
    SOURCES config/maicord.manifest tests/integration/glyph_cache_probe.cpp src/core/log.cpp
    LIBS maicord_core maicord_text maicord_ui maicord_ui_input maicord_model maicord_message_runtime maicord_render_core
        maicord_media_codec maicord_atlas_gpu maicord_renderer kernel32 user32 d3d11 dxgi dcomp
    PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
maicord_copy_fonts(MaiCordGlyphCacheProbe)

# Opt-in measurement tools; several compare with Media Foundation and FFmpeg
# references, so they compile against the SDK (MAICORD_SDK_INCLUDES).
get_property(maicord_targets_after DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
list(REMOVE_ITEM maicord_targets_after ${maicord_targets_before})
set_property(TARGET ${maicord_targets_after} PROPERTY MAICORD_SDK_INCLUDES ON)
