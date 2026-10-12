#include "maicord/media/gpu_video_frame.hpp"

namespace maicord {
void test_video_frame_bounds(void (*check)(bool, const char*) noexcept) noexcept {
    GpuVideoFrame frame{};
    frame.width = frame.height = 2048;
    check(gpu_video_frame_geometry_valid(frame, 2080, 2080), "video_frame_legacy_coded_budget");
    frame.width = 2560;
    frame.height = 1350;
    check(gpu_video_frame_geometry_valid(frame, 2560, 1360), "video_frame_wide_recording_crop");
    check(gpu_video_frame_geometry_valid(frame, 2560, 1408), "video_frame_wide_driver_padded_surface");
    frame.crop_y = 10;
    check(gpu_video_frame_geometry_valid(frame, 2560, 1360), "video_frame_wide_exact_crop_edge");
    frame.crop_y = 12;
    check(!gpu_video_frame_geometry_valid(frame, 2560, 1360), "video_frame_crop_outside_source");
    frame.crop_y = 1;
    check(!gpu_video_frame_geometry_valid(frame, 2560, 1360), "video_frame_odd_chroma_crop");
    frame.crop_y = 0;
    frame.width = 4096;
    frame.height = 1024;
    check(gpu_video_frame_geometry_valid(frame, 4096, 1024), "video_frame_wide_same_visible_budget");
    frame.width = 3840;
    frame.height = 2160;
    check(gpu_video_frame_geometry_valid(frame, 3840, 2176), "video_frame_uhd_stream");
    frame.width = 4096;
    check(gpu_video_frame_geometry_valid(frame, 4096, 2176), "video_frame_dci_4k_stream");
    frame.height = 2305;
    check(!gpu_video_frame_geometry_valid(frame, 4096, 2320), "video_frame_visible_budget_preserved");
    frame.width = 2048;
    frame.height = 2048;
    check(!gpu_video_frame_geometry_valid(frame, 4096, 2400), "video_frame_coded_budget_preserved");
    frame.width = frame.height = 4096;
    check(!gpu_video_frame_geometry_valid(frame, 4096, 4096), "video_frame_square_4096_rejected");
    frame.width = 2048;
    frame.height = 2048;
    frame.crop_x = ~u32{0};
    check(!gpu_video_frame_geometry_valid(frame, 2080, 2080), "video_frame_crop_overflow_rejected");
    frame = {};
    check(!gpu_video_frame_geometry_valid(frame, 2560, 1352), "video_frame_zero_extent_rejected");
    // A portrait camera recording: landscape-coded 1920x1080 turned a quarter.
    frame.width = 1920;
    frame.height = 1080;
    const u32 step = gpu_video_frame_reduction(frame.width, frame.height, kGpuVideoPosterExtent);
    check(gpu_video_frame_output_width(frame, 1) == 1920 && gpu_video_frame_output_height(frame, step) == 216,
          "video_frame_upright_output_extent");
    for (u8 rotation = 0; rotation < 4; ++rotation) {
        frame.rotation = rotation;
        const bool turned = (rotation & 1) != 0;
        check(gpu_video_frame_transposed(frame) == turned &&
                  gpu_video_frame_output_width(frame, 1) == (turned ? 1080u : 1920u) &&
                  gpu_video_frame_output_height(frame, 1) == (turned ? 1920u : 1080u) &&
                  gpu_video_frame_output_width(frame, step) == (turned ? 216u : 384u) &&
                  gpu_video_frame_geometry_valid(frame, 1920, 1088),
              "video_frame_rotated_output_extent");
    }
}
} // namespace maicord
