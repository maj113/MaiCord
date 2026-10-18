#include "../src/app/application_activity.cpp"
#include "maicord/core/win32_types.hpp"

// Response tests never enumerate processes or access the saved games file.
namespace maicord {
ConstSlice<DetectableGame> game_detection_builtin() noexcept {
    return {};
}
u32 game_detection_display_name(Utf8View, Slice<char>) noexcept {
    return 0;
}
GameDetectionMatch game_detection_match(const RegisteredGames&, ConstSlice<DetectableGame>,
                                        ConstSlice<Utf8View>) noexcept {
    return {};
}
u32 game_detection_snapshot(Slice<char>, Slice<Utf8View>, bool) noexcept {
    return 0;
}
bool registered_games_add(RegisteredGames*, Utf8View, Utf8View) noexcept {
    return false;
}
bool registered_games_remove(RegisteredGames*, u32) noexcept {
    return false;
}
bool registered_games_load(RegisteredGames*) noexcept {
    return false;
}
bool registered_games_save(const RegisteredGames&) noexcept {
    return false;
}
} // namespace maicord

namespace maicord::app_detail {
namespace {
u32 retry_completions;
bool WINAPI complete_retry(void*, u64, u64, u64, const RestClientResponse*, Utf8View) noexcept {
    ++retry_completions;
    return true;
}
} // namespace

void test_activity_retry(void (*check)(bool, const char*) noexcept) noexcept {
    const auto old_instance = g_app.account.instance;
    const auto old_complete = g_app.account.api.self_status_complete;
    g_app.account.instance = &retry_completions;
    g_app.account.api.self_status_complete = complete_retry;
    retry_completions = 0;
    for (u32 kind = 0; kind < 5; ++kind) {
        g = {};
        g.request_id = 71;
        g.queued = true;
        g.attempts = static_cast<u8>(kind == 4 ? 3 : 1);
        RestClientResponse response{};
        response.request_id = 71;
        response.status_code = 429;
        response.rate_limited = kind != 1;
        response.retry_at_ms = kind == 2 ? 0 : 1234567;
        response.rejection.captcha = kind == 0;
        check(application_activity_sharing_response(response, {}), "activity exact completion consumed");
        if (kind == 3)
            check(g.queued && !g.failed && !g.request_id && g.retry_at == 1234567 && g.attempts == 1,
                  "activity genuine 429 retains exact deadline and attempt count");
        else
            check(!g.queued && g.failed && !g.request_id && !g.retry_at,
                  "activity challenge, unknown cooldown or exhausted retry is terminal");
    }
    check(retry_completions == 5, "activity completes each owned receipt once");
    g = {};
    g_app.account.instance = old_instance;
    g_app.account.api.self_status_complete = old_complete;
}
} // namespace maicord::app_detail
