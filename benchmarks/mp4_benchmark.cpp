#include "maicord/core/time.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/media/mp4_demux.hpp"
#include "mp4_source_reader_reference.hpp"
#include "mp4_mft_decoder.hpp"
#include "mp4_gpu_benchmark.hpp"

#include <Windows.h>
#include <psapi.h>

namespace maicord {
namespace {

inline constexpr u32 kMaximumTimedFrames = 8'192;
u64 g_mf_frame_us[kMaximumTimedFrames]{};
u64 g_direct_frame_us[kMaximumTimedFrames]{};

struct LessU64 {
    bool operator()(u64 left, u64 right) const noexcept {
        return left < right;
    }
};

struct FileBytes {
    u8* data;
    u32 count;
};

u32 text_length(const char* text) noexcept {
    u32 count = 0;
    while (text[count]) {
        ++count;
    }
    return count;
}

void write_text(const char* text) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, text_length(text),
              &written, nullptr);
}

void write_u64(u64 value) noexcept {
    char reverse[24]{};
    u32 count = 0;
    do {
        reverse[count++] = static_cast<char>('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    char output[24]{};
    for (u32 index = 0; index < count; ++index) {
        output[index] = reverse[count - index - 1u];
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, count, &written,
              nullptr);
}

void write_metric(const char* name, u64 value) noexcept {
    write_text(name);
    write_text("=");
    write_u64(value);
    write_text("\r\n");
}

FileBytes read_input(const wchar_t* variable) noexcept {
    wchar_t path[32'768]{};
    const DWORD path_bytes = GetEnvironmentVariableW(
        variable, path,
        static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    if (path_bytes == 0 || path_bytes >= sizeof(path) / sizeof(path[0])) {
        return {};
    }
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ |
                                                  FILE_SHARE_WRITE |
                                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || length.QuadPart <= 0 ||
        length.QuadPart > ~0u) {
        CloseHandle(file);
        return {};
    }
    const u32 bytes = static_cast<u32>(length.QuadPart);
    auto* allocation = static_cast<u8*>(VirtualAlloc(
        nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    DWORD read = 0;
    const bool ok = allocation && ReadFile(file, allocation, bytes, &read,
                                            nullptr) && read == bytes;
    CloseHandle(file);
    if (!ok) {
        if (allocation) {
            VirtualFree(allocation, 0, MEM_RELEASE);
        }
        return {};
    }
    return FileBytes{allocation, bytes};
}

u64 process_private_bytes() noexcept {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(
                                  &counters),
                              sizeof(counters))) {
        return 0;
    }
    return counters.PrivateUsage;
}

u64 process_cycles() noexcept {
    ULONG64 cycles = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &cycles);
    return cycles;
}

bool validate_length_prefixed_sample(ConstSlice<u8> bytes,
                                     u8 length_bytes) noexcept {
    u32 cursor = 0;
    u32 units = 0;
    while (cursor < bytes.count) {
        if (length_bytes == 0 || bytes.count - cursor < length_bytes) {
            return false;
        }
        u32 length = 0;
        for (u32 index = 0; index < length_bytes; ++index) {
            length = (length << 8u) | bytes.data[cursor + index];
        }
        cursor += length_bytes;
        if (length == 0 || length > bytes.count - cursor) {
            return false;
        }
        cursor += length;
        ++units;
    }
    return units != 0;
}

int run() noexcept {
    const Clock clock = clock_initialize();
    const FileBytes input = read_input(L"MAICORD_MP4_BENCHMARK_INPUT");
    if (!input.data) {
        write_text("mp4_benchmark_input=missing\r\n");
        return 2;
    }
    const ConstSlice<u8> encoded{input.data, input.count};
    const u64 process_memory_baseline = process_private_bytes();
    Mp4Demuxer demuxer{};
    const i64 demux_init_begin = clock_now();
    const Mp4DemuxError initialized = mp4_demux_initialize(&demuxer, encoded);
    const u64 demux_init_us =
        clock_microseconds(clock, demux_init_begin, clock_now());
    if (initialized != Mp4DemuxError::none) {
        write_metric("demux_error", static_cast<u64>(initialized));
        VirtualFree(input.data, 0, MEM_RELEASE);
        return 3;
    }
    u64 sample_bytes = 0;
    u64 duration_us = 0;
    u32 sample_count = 0;
    bool valid_samples = true;
    const u64 demux_cycles_begin = process_cycles();
    const i64 demux_scan_begin = clock_now();
    for (;;) {
        const Mp4AccessUnit sample = mp4_demux_next(&demuxer);
        if (sample.error == Mp4DemuxError::end_of_stream) {
            break;
        }
        if (!sample.ok() ||
            !validate_length_prefixed_sample(sample.bytes,
                                             demuxer.nal_length_bytes)) {
            valid_samples = false;
            break;
        }
        sample_bytes += sample.bytes.count;
        duration_us += sample.duration_us;
        ++sample_count;
    }
    const u64 demux_scan_us =
        clock_microseconds(clock, demux_scan_begin, clock_now());
    const u64 demux_cycles = process_cycles() - demux_cycles_begin;
    write_metric("input_bytes", input.count);
    write_metric("demux_width", demuxer.width);
    write_metric("demux_height", demuxer.height);
    write_metric("demux_samples", sample_count);
    write_metric("demux_sample_bytes", sample_bytes);
    write_metric("demux_duration_us", duration_us);
    write_metric("demux_init_us", demux_init_us);
    write_metric("demux_scan_us", demux_scan_us);
    write_metric("demux_process_cycles", demux_cycles);
    write_metric("demux_heap_growth_bytes",
                 process_private_bytes() - process_memory_baseline);
    write_metric("demux_correct", valid_samples &&
                                      sample_count == demuxer.sample_count
                                  ? 1u
                                  : 0u);
    if (!valid_samples || sample_count != demuxer.sample_count) {
        VirtualFree(input.data, 0, MEM_RELEASE);
        return 4;
    }

    // Hot-cache/raw parser throughput.  Keep this bounded to roughly 100 ms
    // so the benchmark remains useful in CI and never turns into a soak test.
    wchar_t skip_repeat_value[2]{};
    const bool skip_repeat = GetEnvironmentVariableW(
                                 L"MAICORD_MP4_SKIP_DEMUX_REPEAT",
                                 skip_repeat_value, 2) == 1 &&
                             skip_repeat_value[0] == L'1';
    u64 repeated_units = 0;
    u64 repeated_bytes = 0;
    u64 repeated_checksum = 0;
    u32 repeated_iterations = 0;
    const i64 repeated_begin = clock_now();
    if (!skip_repeat) {
        for (; repeated_iterations < 262'144u; ++repeated_iterations) {
            mp4_demux_reset(&demuxer);
            for (;;) {
                const Mp4AccessUnit sample = mp4_demux_next(&demuxer);
                if (sample.error == Mp4DemuxError::end_of_stream) {
                    break;
                }
                if (!sample.ok()) {
                    valid_samples = false;
                    break;
                }
                repeated_bytes += sample.bytes.count;
                repeated_checksum = repeated_checksum * 131u +
                                    sample.bytes.data[0] + sample.bytes.count;
                ++repeated_units;
            }
            if (!valid_samples ||
                clock_microseconds(clock, repeated_begin, clock_now()) >=
                    100'000u) {
                ++repeated_iterations;
                break;
            }
        }
    }
    const u64 repeated_us = maximum<u64>(
        1u, clock_microseconds(clock, repeated_begin, clock_now()));
    write_metric("demux_repeated_iterations", repeated_iterations);
    write_metric("demux_repeated_us", repeated_us);
    write_metric("demux_access_units_per_second",
                 skip_repeat ? 0u
                             : repeated_units * 1'000'000u / repeated_us);
    write_metric("demux_referenced_bytes_per_second",
                 skip_repeat ? 0u
                             : repeated_bytes * 1'000'000u / repeated_us);
    write_metric("demux_repeated_checksum", repeated_checksum);
    if (!valid_samples) {
        VirtualFree(input.data, 0, MEM_RELEASE);
        return 4;
    }

    if (!mp4_reference_thread_start()) {
        write_text("mf_runtime=failed\r\n");
        VirtualFree(input.data, 0, MEM_RELEASE);
        return 5;
    }
    wchar_t cold_direct_value[2]{};
    wchar_t gpu_value[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_MP4_GPU", gpu_value, 2) == 1 && gpu_value[0] == L'1') {
        const int result = mp4_gpu_benchmark(encoded, write_metric);
        mp4_reference_thread_stop();
        VirtualFree(input.data, 0, MEM_RELEASE);
        return result;
    }
    const bool cold_direct = GetEnvironmentVariableW(
                                 L"MAICORD_MP4_DIRECT_COLD",
                                 cold_direct_value, 2) == 1 &&
                             cold_direct_value[0] == L'1';
    if (cold_direct) {
        const u64 memory_baseline = process_private_bytes();
        const u64 cycles_begin = process_cycles();
        const i64 init_begin = clock_now();
        Mp4DirectDecoder direct{};
        const bool ready = mp4_direct_decoder_initialize(&direct, encoded);
        const u64 init_us = clock_microseconds(clock, init_begin, clock_now());
        const i64 decode_begin = clock_now();
        u64 first_frame_us = 0;
        u64 checksum = 0;
        u32 frames = 0;
        if (ready) {
            for (u32 index = 0; index < sample_count + 16u; ++index) {
                const Mp4DecodedFrame frame =
                    mp4_direct_decoder_read(&direct, false);
                if (!frame.ok()) {
                    break;
                }
                if (frames == 0) {
                    first_frame_us = clock_microseconds(
                        clock, decode_begin, clock_now());
                }
                const u32 stride = maximum(1u, frame.rgba8.count / 257u);
                for (u32 byte = 0; byte < frame.rgba8.count;
                     byte += stride) {
                    checksum = checksum * 131u + frame.rgba8.data[byte];
                }
                ++frames;
            }
        }
        write_metric("cold_direct_ready", ready ? 1u : 0u);
        write_metric("cold_direct_init_us", init_us);
        write_metric("cold_direct_first_frame_us", first_frame_us);
        write_metric("cold_direct_startup_to_first_frame_us",
                     ready && first_frame_us != 0
                         ? init_us + first_frame_us
                         : 0u);
        write_metric("cold_direct_decode_us", clock_microseconds(
            clock, decode_begin, clock_now()));
        write_metric("cold_direct_frames", frames);
        write_metric("cold_direct_process_cycles",
                     process_cycles() - cycles_begin);
        write_metric("cold_direct_heap_growth_bytes",
                     process_private_bytes() - memory_baseline);
        write_metric("cold_direct_checksum", checksum);
        write_metric("cold_direct_d3d_decode_calls", 0u);
        mp4_direct_decoder_release(&direct);
        mp4_reference_thread_stop();
        VirtualFree(input.data, 0, MEM_RELEASE);
        return ready && frames == sample_count && checksum != 0 ? 0 : 6;
    }
    const u64 mf_memory_baseline = process_private_bytes();
    const u64 mf_cycles_begin = process_cycles();
    const i64 mf_init_begin = clock_now();
    Mp4ReferenceDecoder decoder{};
    const bool decoder_ready = mp4_reference_initialize_source_reader(
        &decoder, encoded, static_cast<u16>(demuxer.width),
        static_cast<u16>(demuxer.height));
    const u64 mf_init_us = clock_microseconds(clock, mf_init_begin, clock_now());
    u64 mf_first_frame_us = 0;
    i64 mf_decode_begin = 0;
    u64 checksum = 0;
    u32 decoded_frames = 0;
    if (decoder_ready) {
        mf_decode_begin = clock_now();
        for (u32 index = 0; index < sample_count + 16u; ++index) {
            const i64 frame_begin = clock_now();
            const Mp4DecodedFrame frame = mp4_reference_read(&decoder, false);
            const u64 frame_us = clock_microseconds(
                clock, frame_begin, clock_now());
            if (!frame.ok()) {
                break;
            }
            if (decoded_frames < kMaximumTimedFrames) {
                g_mf_frame_us[decoded_frames] = frame_us;
            }
            if (decoded_frames == 0) {
                mf_first_frame_us = clock_microseconds(
                    clock, mf_decode_begin, clock_now());
            }
            const u32 stride = maximum(1u, frame.rgba8.count / 257u);
            for (u32 byte = 0; byte < frame.rgba8.count; byte += stride) {
                checksum = checksum * 131u + frame.rgba8.data[byte];
            }
            ++decoded_frames;
        }
    }
    const u64 mf_decode_us =
        mf_decode_begin == 0
            ? 0
            : clock_microseconds(clock, mf_decode_begin, clock_now());
    const u64 mf_cycles = process_cycles() - mf_cycles_begin;
    const u64 mf_heap_growth = process_private_bytes() - mf_memory_baseline;
    write_metric("mf_source_reader_ready", decoder_ready ? 1u : 0u);
    write_metric("mf_source_reader_init_us", mf_init_us);
    write_metric("mf_source_reader_first_frame_us", mf_first_frame_us);
    write_metric("mf_source_reader_startup_to_first_frame_us",
                 decoder_ready && mf_first_frame_us != 0
                     ? mf_init_us + mf_first_frame_us
                     : 0u);
    write_metric("mf_source_reader_decode_us", mf_decode_us);
    write_metric("mf_source_reader_frames", decoded_frames);
    write_metric("mf_source_reader_process_cycles", mf_cycles);
    write_metric("mf_source_reader_heap_growth_bytes", mf_heap_growth);
    write_metric("mf_source_reader_checksum", checksum);
    const u32 mf_timed_frames = minimum(decoded_frames,
                                        kMaximumTimedFrames);
    heap_sort(g_mf_frame_us, mf_timed_frames, LessU64{});
    write_metric("mf_source_reader_frame_p50_us",
                 mf_timed_frames == 0
                     ? 0
                     : g_mf_frame_us[(mf_timed_frames - 1u) * 50u / 100u]);
    write_metric("mf_source_reader_frame_p95_us",
                 mf_timed_frames == 0
                     ? 0
                     : g_mf_frame_us[(mf_timed_frames - 1u) * 95u / 100u]);
    write_metric("mf_source_reader_frame_p99_us",
                 mf_timed_frames == 0
                     ? 0
                     : g_mf_frame_us[(mf_timed_frames - 1u) * 99u / 100u]);
    write_metric("mf_source_reader_frame_max_us",
                 mf_timed_frames == 0 ? 0
                                      : g_mf_frame_us[mf_timed_frames - 1u]);
    mp4_reference_release(&decoder);

    const u64 direct_memory_baseline = process_private_bytes();
    const u64 direct_cycles_begin = process_cycles();
    const i64 direct_init_begin = clock_now();
    Mp4DirectDecoder direct{};
    const bool direct_ready = mp4_direct_decoder_initialize(&direct, encoded);
    const u64 direct_init_us =
        clock_microseconds(clock, direct_init_begin, clock_now());
    u64 direct_first_frame_us = 0;
    u64 direct_checksum = 0;
    u32 direct_frames = 0;
    const i64 direct_decode_begin = clock_now();
    if (direct_ready) {
        for (u32 index = 0; index < sample_count + 16u; ++index) {
            const i64 frame_begin = clock_now();
            const Mp4DecodedFrame frame =
                mp4_direct_decoder_read(&direct, false);
            const u64 frame_us = clock_microseconds(
                clock, frame_begin, clock_now());
            if (!frame.ok()) {
                break;
            }
            if (direct_frames < kMaximumTimedFrames) {
                g_direct_frame_us[direct_frames] = frame_us;
            }
            if (direct_frames == 0) {
                direct_first_frame_us = clock_microseconds(
                    clock, direct_decode_begin, clock_now());
            }
            const u32 stride = maximum(1u, frame.rgba8.count / 257u);
            for (u32 byte = 0; byte < frame.rgba8.count; byte += stride) {
                direct_checksum = direct_checksum * 131u +
                                  frame.rgba8.data[byte];
            }
            ++direct_frames;
        }
    }
    const u64 direct_decode_us = clock_microseconds(
        clock, direct_decode_begin, clock_now());
    const u64 direct_cycles = process_cycles() - direct_cycles_begin;
    const u64 direct_heap_growth =
        process_private_bytes() - direct_memory_baseline;
    write_metric("direct_ready", direct_ready ? 1u : 0u);
    write_metric("direct_init_us", direct_init_us);
    write_metric("direct_first_frame_us", direct_first_frame_us);
    write_metric("direct_startup_to_first_frame_us",
                 direct_ready && direct_first_frame_us != 0
                     ? direct_init_us + direct_first_frame_us
                     : 0u);
    write_metric("direct_decode_us", direct_decode_us);
    write_metric("direct_frames", direct_frames);
    write_metric("direct_process_cycles", direct_cycles);
    write_metric("direct_heap_growth_bytes", direct_heap_growth);
    write_metric("direct_checksum", direct_checksum);
    write_metric("direct_d3d_decode_calls", 0u);
    const u32 direct_timed_frames = minimum(direct_frames,
                                            kMaximumTimedFrames);
    heap_sort(g_direct_frame_us, direct_timed_frames, LessU64{});
    write_metric("direct_frame_p50_us",
                 direct_timed_frames == 0
                     ? 0
                     : g_direct_frame_us[(direct_timed_frames - 1u) * 50u /
                                         100u]);
    write_metric("direct_frame_p95_us",
                 direct_timed_frames == 0
                     ? 0
                     : g_direct_frame_us[(direct_timed_frames - 1u) * 95u /
                                         100u]);
    write_metric("direct_frame_p99_us",
                 direct_timed_frames == 0
                     ? 0
                     : g_direct_frame_us[(direct_timed_frames - 1u) * 99u /
                                         100u]);
    write_metric("direct_frame_max_us",
                 direct_timed_frames == 0
                     ? 0
                     : g_direct_frame_us[direct_timed_frames - 1u]);
    write_metric("direct_last_status", static_cast<u32>(
                                           mp4_direct_decoder_last_status(
                                               &direct)));
    write_metric("direct_color_matrix",
                 mp4_direct_decoder_color_matrix(&direct));
    write_metric("direct_nominal_range",
                 mp4_direct_decoder_nominal_range(&direct));
    const Mp4DirectOutputLayout direct_layout =
        mp4_direct_decoder_output_layout(&direct);
    write_metric("direct_storage_width", direct_layout.storage_width);
    write_metric("direct_storage_height", direct_layout.storage_height);
    write_metric("direct_storage_pitch", direct_layout.pitch);
    write_metric("direct_aperture_width", direct_layout.width);
    write_metric("direct_aperture_height", direct_layout.height);
    mp4_direct_decoder_release(&direct);

    Mp4ReferenceDecoder reference{};
    Mp4DirectDecoder candidate{};
    const bool comparison_reference_ready =
        mp4_reference_initialize_source_reader(&reference, encoded,
            static_cast<u16>(demuxer.width), static_cast<u16>(demuxer.height));
    const bool comparison_candidate_ready =
        mp4_direct_decoder_initialize(&candidate, encoded);
    const bool comparison_ready = comparison_reference_ready &&
                                  comparison_candidate_ready;
    u64 absolute_error = 0;
    u64 squared_error = 0;
    u64 compared_channels = 0;
    u32 maximum_error = 0;
    u32 maximum_error_frame = 0;
    u32 maximum_error_pixel = 0;
    u32 maximum_error_channel = 0;
    u32 maximum_error_expected = 0;
    u32 maximum_error_actual = 0;
    u64 errors_over_two = 0;
    u64 errors_over_eight = 0;
    u64 errors_over_sixteen = 0;
    u32 compared_frames = 0;
    bool alpha_exact = true;
    bool shape_exact = comparison_ready;
    Mp4DecodeError comparison_reference_error = Mp4DecodeError::none;
    Mp4DecodeError comparison_candidate_error = Mp4DecodeError::none;
    if (comparison_ready) {
        for (u32 index = 0; index < sample_count + 16u; ++index) {
            const Mp4DecodedFrame expected =
                mp4_reference_read(&reference, false);
            const Mp4DecodedFrame actual =
                mp4_direct_decoder_read(&candidate, false);
            if (!expected.ok() || !actual.ok()) {
                comparison_reference_error = expected.error;
                comparison_candidate_error = actual.error;
                shape_exact = expected.ok() == actual.ok();
                break;
            }
            if (expected.width != actual.width ||
                expected.height != actual.height ||
                expected.rgba8.count != actual.rgba8.count) {
                write_metric("comparison_expected_width", expected.width);
                write_metric("comparison_expected_height", expected.height);
                write_metric("comparison_actual_width", actual.width);
                write_metric("comparison_actual_height", actual.height);
                shape_exact = false;
                break;
            }
            for (u32 pixel = 0;
                 pixel + 3u < expected.rgba8.count; pixel += 4u) {
                for (u32 channel = 0; channel < 3u; ++channel) {
                    const i32 delta =
                        static_cast<i32>(expected.rgba8.data[pixel + channel]) -
                        actual.rgba8.data[pixel + channel];
                    const u32 magnitude =
                        static_cast<u32>(delta < 0 ? -delta : delta);
                    if (magnitude > maximum_error) {
                        maximum_error = magnitude;
                        maximum_error_frame = compared_frames;
                        maximum_error_pixel = pixel / 4u;
                        maximum_error_channel = channel;
                        maximum_error_expected =
                            expected.rgba8.data[pixel + channel];
                        maximum_error_actual =
                            actual.rgba8.data[pixel + channel];
                    }
                    errors_over_two += magnitude > 2u ? 1u : 0u;
                    errors_over_eight += magnitude > 8u ? 1u : 0u;
                    errors_over_sixteen += magnitude > 16u ? 1u : 0u;
                    absolute_error += magnitude;
                    squared_error += static_cast<u64>(magnitude) * magnitude;
                    ++compared_channels;
                }
                alpha_exact = alpha_exact &&
                              expected.rgba8.data[pixel + 3u] ==
                                  actual.rgba8.data[pixel + 3u];
            }
            ++compared_frames;
        }
    }
    write_metric("correctness_frames", compared_frames);
    write_metric("correctness_reference_ready",
                 comparison_reference_ready ? 1u : 0u);
    write_metric("correctness_candidate_ready",
                 comparison_candidate_ready ? 1u : 0u);
    write_metric("correctness_reference_error",
                 static_cast<u32>(comparison_reference_error));
    write_metric("correctness_candidate_error",
                 static_cast<u32>(comparison_candidate_error));
    write_metric("correctness_candidate_status", static_cast<u32>(
        mp4_direct_decoder_last_status(&candidate)));
    write_metric("correctness_shape_exact", shape_exact ? 1u : 0u);
    write_metric("correctness_alpha_exact", alpha_exact ? 1u : 0u);
    write_metric("correctness_maximum_channel_error", maximum_error);
    write_metric("correctness_maximum_error_frame", maximum_error_frame);
    write_metric("correctness_maximum_error_pixel", maximum_error_pixel);
    write_metric("correctness_maximum_error_channel", maximum_error_channel);
    write_metric("correctness_maximum_error_expected", maximum_error_expected);
    write_metric("correctness_maximum_error_actual", maximum_error_actual);
    write_metric("correctness_errors_over_two", errors_over_two);
    write_metric("correctness_errors_over_eight", errors_over_eight);
    write_metric("correctness_errors_over_sixteen", errors_over_sixteen);
    write_metric("correctness_mean_absolute_error_x1000",
                 compared_channels == 0
                     ? ~0ull
                     : absolute_error * 1'000u / compared_channels);
    write_metric("correctness_mse_x1000",
                 compared_channels == 0
                     ? ~0ull
                     : squared_error * 1'000u / compared_channels);
    mp4_direct_decoder_release(&candidate);
    mp4_reference_release(&reference);

    const bool structure_complete = comparison_ready && shape_exact &&
        alpha_exact && compared_frames == sample_count &&
        comparison_reference_error == Mp4DecodeError::end_of_stream &&
        comparison_candidate_error == Mp4DecodeError::end_of_stream;
    write_metric("decode_structure_complete", structure_complete ? 1u : 0u);

    // Compare native 4:2:0 planes before RGB conversion. This separates H.264
    // reconstruction/crop/order bugs from the reference scaler's chroma filter.
    // A second loop validates drain/flush/restart against the same reference.
    const FileBytes native_reference = read_input(L"MAICORD_MP4_REFERENCE_I420");
    Mp4DirectDecoder native_candidate{};
    u32 native_frames = 0;
    u64 native_mismatches = 0;
    u32 native_maximum_error = 0;
    bool native_complete = false;
    if (native_reference.data &&
        mp4_direct_decoder_initialize(&native_candidate, encoded)) {
        const u64 frame_bytes = static_cast<u64>(native_candidate.width) *
            native_candidate.height +
            static_cast<u64>((native_candidate.width + 1u) / 2u) *
            ((native_candidate.height + 1u) / 2u) * 2u;
        auto* scratch = static_cast<u8*>(VirtualAlloc(
            nullptr, static_cast<SIZE_T>(frame_bytes), MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE));
        if (scratch && frame_bytes * sample_count == native_reference.count) {
            for (u32 index = 0; index < sample_count * 2u; ++index) {
                const Mp4DecodedFrame frame =
                    mp4_direct_decoder_read(&native_candidate, true);
                if (!frame.ok() || !mp4_direct_decoder_copy_i420(
                        &native_candidate, Slice<u8>{scratch, static_cast<u32>(frame_bytes)})) {
                    break;
                }
                const u8* expected = native_reference.data +
                    static_cast<u64>(index % sample_count) * frame_bytes;
                for (u32 byte = 0; byte < frame_bytes; ++byte) {
                    const i32 delta = static_cast<i32>(expected[byte]) - scratch[byte];
                    const u32 magnitude = static_cast<u32>(delta < 0 ? -delta : delta);
                    native_mismatches += magnitude != 0 ? 1u : 0u;
                    native_maximum_error = maximum(native_maximum_error, magnitude);
                }
                ++native_frames;
            }
            native_complete = native_frames == sample_count * 2u &&
                mp4_direct_decoder_read(&native_candidate, false).error ==
                    Mp4DecodeError::end_of_stream;
        }
        if (scratch) {
            VirtualFree(scratch, 0, MEM_RELEASE);
        }
    }
    write_metric("native_reference_present", native_reference.data ? 1u : 0u);
    write_metric("native_reference_frames", native_frames);
    write_metric("native_reference_complete", native_complete ? 1u : 0u);
    write_metric("native_reference_mismatched_bytes", native_mismatches);
    write_metric("native_reference_maximum_error", native_maximum_error);
    const bool native_passed = !native_reference.data ||
        (native_complete && native_mismatches == 0);
    mp4_direct_decoder_release(&native_candidate);
    if (native_reference.data) {
        VirtualFree(native_reference.data, 0, MEM_RELEASE);
    }

    const FileBytes ffmpeg_reference =
        read_input(L"MAICORD_MP4_REFERENCE_RGBA");
    u64 ffmpeg_absolute_error = 0;
    u64 ffmpeg_squared_error = 0;
    u64 ffmpeg_channels = 0;
    u32 ffmpeg_maximum_error = 0;
    u32 ffmpeg_frames = 0;
    bool ffmpeg_alpha_exact = true;
    Mp4DirectDecoder ffmpeg_candidate{};
    if (ffmpeg_reference.data &&
        mp4_direct_decoder_initialize(&ffmpeg_candidate, encoded)) {
        const u64 frame_bytes = static_cast<u64>(ffmpeg_candidate.width) *
                                ffmpeg_candidate.height * 4u;
        const u32 available_frames = frame_bytes == 0
                                         ? 0
                                         : static_cast<u32>(
                                               ffmpeg_reference.count /
                                               frame_bytes);
        for (u32 frame_index = 0; frame_index < available_frames;
             ++frame_index) {
            const Mp4DecodedFrame actual =
                mp4_direct_decoder_read(&ffmpeg_candidate, false);
            if (!actual.ok() || actual.rgba8.count != frame_bytes) {
                break;
            }
            const u8* expected = ffmpeg_reference.data +
                                 static_cast<u64>(frame_index) * frame_bytes;
            for (u32 pixel = 0; pixel + 3u < actual.rgba8.count;
                 pixel += 4u) {
                for (u32 channel = 0; channel < 3u; ++channel) {
                    const i32 delta = static_cast<i32>(expected[pixel + channel]) -
                                      actual.rgba8.data[pixel + channel];
                    const u32 magnitude =
                        static_cast<u32>(delta < 0 ? -delta : delta);
                    ffmpeg_maximum_error = maximum(ffmpeg_maximum_error,
                                                    magnitude);
                    ffmpeg_absolute_error += magnitude;
                    ffmpeg_squared_error +=
                        static_cast<u64>(magnitude) * magnitude;
                    ++ffmpeg_channels;
                }
                ffmpeg_alpha_exact = ffmpeg_alpha_exact &&
                    expected[pixel + 3u] == actual.rgba8.data[pixel + 3u];
            }
            ++ffmpeg_frames;
        }
    }
    write_metric("ffmpeg_reference_present",
                 ffmpeg_reference.data ? 1u : 0u);
    write_metric("ffmpeg_correctness_frames", ffmpeg_frames);
    write_metric("ffmpeg_correctness_alpha_exact",
                 ffmpeg_alpha_exact ? 1u : 0u);
    write_metric("ffmpeg_correctness_maximum_channel_error",
                 ffmpeg_maximum_error);
    write_metric("ffmpeg_correctness_mean_absolute_error_x1000",
                 ffmpeg_channels == 0
                     ? 0
                     : ffmpeg_absolute_error * 1'000u / ffmpeg_channels);
    write_metric("ffmpeg_correctness_mse_x1000",
                 ffmpeg_channels == 0
                     ? 0
                     : ffmpeg_squared_error * 1'000u / ffmpeg_channels);
    mp4_direct_decoder_release(&ffmpeg_candidate);
    if (ffmpeg_reference.data) {
        VirtualFree(ffmpeg_reference.data, 0, MEM_RELEASE);
    }
    mp4_reference_thread_stop();
    VirtualFree(input.data, 0, MEM_RELEASE);
    return decoder_ready && decoded_frames == sample_count && checksum != 0 &&
                   direct_ready && direct_frames == sample_count && direct_checksum != 0 &&
                   structure_complete && native_passed
               ? 0
               : 6;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_mp4_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
