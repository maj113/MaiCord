#include "../src/app/account_ui_runtime.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
void test_profile_decode_scratch(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace app_detail;
    auto* ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!ui) { check(false, "profile scratch fixture allocation"); return; }
    UserProfile retained{};
    const auto full = utf8_literal(
        R"({"user":{"id":"42","username":"user","global_name":"Name"},"user_profile":{"bio":"kept bio","banner":"a_banner","theme_colors":[1,2]},"guild_member":{"roles":["10","11"]},"mutual_guilds":[{"id":"7","name":"guild"}]})");
    check(ui->profile_decode.decode_into(full, 42, 7, retained) && retained.role_count == 2 &&
          retained.has_theme && utf8_equal(retained.bio.view(), utf8_literal("kept bio")),
          "profile scratch commits complete decoded profile");
    // Simulate the intervening history/navigation phase reusing the real union.
    for (auto& value : ui->message_history_page_structurals) value = 0xa5a5a5a5u;
    check(retained.user_id == 42 && retained.role_count == 2 && retained.roles[1] == 11 &&
          utf8_equal(retained.banner.view(), utf8_literal("a_banner")),
          "profile fields remain owned after decoder scratch is overwritten");
    check(!ui->profile_decode.decode_into(full, 43, 7, retained) && retained.user_id == 42 &&
          retained.role_count == 2 && utf8_equal(retained.bio.view(), utf8_literal("kept bio")),
          "identity mismatch preserves the displayed profile");
    const auto invalid = utf8_literal(
        R"({"user":{"id":"42","username":"partial"},"user_profile":{"bio":"do not commit","theme_colors":["invalid"]}})");
    check(!ui->profile_decode.decode_into(invalid, 42, 7, retained) &&
          utf8_equal(retained.username.view(), utf8_literal("user")) && retained.role_count == 2 &&
          utf8_equal(retained.bio.view(), utf8_literal("kept bio")),
          "late decode failure cannot expose a partially decoded profile");
    const auto minimal = utf8_literal(R"({"user":{"id":"42","username":"next"}})");
    check(ui->profile_decode.decode_into(minimal, 42, 0, retained) && retained.guild_id == 0 &&
          retained.role_count == 0 && retained.bio.bytes == 0 && !retained.has_theme &&
          utf8_equal(retained.display_name.view(), utf8_literal("next")),
          "scratch reuse clears fields absent from the next valid profile");
    VirtualFree(ui, 0, MEM_RELEASE);
}
}
