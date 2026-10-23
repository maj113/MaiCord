#include "maicord/ui/geometry.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"
namespace maicord {
namespace {
inline UiRect reference_overlap(const UiRect& a, const UiRect& b) noexcept {
    return {maximum(a.left, b.left), maximum(a.top, b.top),
            minimum(a.right, b.right), minimum(a.bottom, b.bottom)};
}
UiRect reference_intersection(const UiRect& a, const UiRect& b) noexcept {
    auto result = reference_overlap(a, b);
    if (result.right < result.left) result.right = result.left;
    if (result.bottom < result.top) result.bottom = result.top;
    return result;
}
u32 random_state = 123;
u32 next() noexcept {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
float coordinate() noexcept {
    return static_cast<float>(static_cast<i32>(next() % 2097153) - 1048576) * .25f;
}
u32 failures;
void compare(const UiRect& a, const UiRect& b) noexcept {
    const auto expected_overlap = reference_overlap(a, b);
    const auto expected_intersection = reference_intersection(a, b);
    const auto overlap = ui_rect_overlap(a, b);
    const auto intersection = ui_rect_intersection(a, b);
    test_check(&failures, bytes_equal(&overlap, &expected_overlap, sizeof(UiRect)), "rectangle overlap mismatch");
    test_check(&failures, bytes_equal(&intersection, &expected_intersection, sizeof(UiRect)), "rectangle intersection mismatch");
}

}
}
extern "C" __declspec(noreturn) void WINAPI rectangle_geometry_entry() noexcept {
    using namespace maicord;
    const float edges[] = {-kMaximumUiCoordinate, -1.f, -0.f, 0.f, 1.f, kMaximumUiCoordinate};
    for (float a : edges) for (float b : edges) for (float c : edges) for (float d : edges) {
        compare({a, b, c, d}, {d, c, b, a});
        compare({a, b, c, d}, {a, b, c, d});
    }
    for (u32 i = 0; i < 16384; ++i) {
        const UiRect a{coordinate(), coordinate(), coordinate(), coordinate()};
        const UiRect b{coordinate(), coordinate(), coordinate(), coordinate()};
        compare(a, b);
    }
    ExitProcess(failures ? 1 : 0);
}
