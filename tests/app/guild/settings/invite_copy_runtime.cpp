// Server Settings > Invites rows: Copy Link for everyone, Revoke for managers.
// Copy Link writes the full invite URL to the captured clipboard (clipboard_system_* below, never the user's
// clipboard) and confirms it with a toast. Synthetic invite codes only.
#include "../../../../src/app/guild/settings/invites.cpp"
#include "maicord/platform/window/win32.hpp"
#include "maicord/platform/window/win32_clipboard.hpp"
#include "../../../support/test_output.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::invite_copy_test {
using namespace app_detail;
using namespace app_detail::server_settings;
u32 failures;
void check(bool value, const char* label) noexcept {
    test_check(&failures, value, label);
}
UiSettingsControl rows[16];
u32 row_count, revoke_forms;
bool can_edit = true, request_busy;
ChatStateStore chat{};
struct Captured {
    u32 opens, publishes;
    UINT format;
    char text[256];
    bool refuse;
} clip{};
bool newest_toast(UiToastKind kind, Utf8View text) noexcept {
    const auto& t = g_app.toasts;
    return t.count && t.entries[t.count - 1].kind == kind &&
           utf8_equal({t.entries[t.count - 1].text, t.entries[t.count - 1].bytes}, text);
}
SettingsLayerActivation press(u32 row, u32 button) noexcept {
    SettingsLayerActivation a{};
    a.value.target = {UiSettingsTargetKind::control, {}, kRowBase + row};
    a.value.kind = UiSettingsControlKind::buttons;
    a.value.selected_choice = button;
    a.pointer = a.button = true;
    return a;
}
void project() noexcept {
    row_count = 0;
    static char text[4096];
    Bank bank{text, sizeof(text), 0};
    kInvitesPage.project(bank);
}
const UiSettingsControl* invite_row(u32 index) noexcept {
    for (u32 i = 0; i < row_count; ++i)
        if (rows[i].id == kRowBase + index)
            return &rows[i];
    return nullptr;
}

void run() noexcept {
    owner = static_cast<Settings*>(
        VirtualAlloc(nullptr, sizeof(Settings), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(owner != nullptr, "invite copy: settings fixture");
    if (!owner)
        return;
    g_app.window = win32_window(reinterpret_cast<HWND>(usize{0x10})); // never opened: the sink stands in
    owner->invites_loaded = true;
    owner->invite_count = 2;
    constexpr char codes[2][8]{"abc123", "Zx9"};
    for (u32 i = 0; i < 2; ++i) {
        owner->invites[i].code_bytes = static_cast<u8>(lstrlenA(codes[i]));
        copy_bytes(owner->invites[i].code, codes[i], owner->invites[i].code_bytes);
    }
    project();
    const auto* row = invite_row(0);
    check(row && row->kind == UiSettingsControlKind::buttons && row->enabled && row->choices.count == 2 &&
              utf8_equal(row->choices[0], utf8_literal("Copy Link")) &&
              utf8_equal(row->choices[1], utf8_literal("Revoke")) &&
              utf8_equal(row->label, utf8_literal("abc123")),
          "invite copy: a manager's row offers Copy Link before Revoke");
    can_edit = false;
    project();
    row = invite_row(1);
    check(row && row->enabled && row->choices.count == 1 &&
              utf8_equal(row->choices[0], utf8_literal("Copy Link")),
          "invite copy: without Manage Server a row still offers Copy Link");

    kInvitesPage.activate(press(1, 0));
    check(clip.opens == 1 && clip.publishes == 1 && clip.format == CF_UNICODETEXT &&
              utf8_equal({clip.text, static_cast<u32>(lstrlenA(clip.text))},
                         utf8_literal("https://discord.gg/Zx9")),
          "invite copy: Copy Link writes the invite URL to the clipboard");
    check(newest_toast(UiToastKind::success, utf8_literal("Invite link copied")),
          "invite copy: Copy Link confirms with an Invite link copied toast");

    kInvitesPage.activate(press(1, 1));
    check(!revoke_forms, "invite copy: Revoke needs Manage Server");
    can_edit = true;
    request_busy = true;
    kInvitesPage.activate(press(0, 0));
    check(clip.opens == 2 && utf8_equal({clip.text, static_cast<u32>(lstrlenA(clip.text))},
                                        utf8_literal("https://discord.gg/abc123")),
          "invite copy: copying works while a request is in flight");
    kInvitesPage.activate(press(0, 1));
    check(!revoke_forms, "invite copy: Revoke waits for the request in flight");
    request_busy = false;
    kInvitesPage.activate(press(0, 1));
    check(revoke_forms == 1 && clip.opens == 2,
          "invite copy: Revoke opens its confirmation without copying");

    ui_toast_clear(&g_app.toasts);
    clip.refuse = true;
    kInvitesPage.activate(press(0, 0));
    check(newest_toast(UiToastKind::failure, utf8_literal("Unable to copy link")),
          "invite copy: a refused clipboard shows the failure toast");
    clip.refuse = false;
    kInvitesPage.activate(press(7, 0));
    check(clip.opens == 3, "invite copy: rows past the list do nothing");

    g_app.window = nullptr;
    VirtualFree(owner, 0, MEM_RELEASE);
    owner = nullptr;
}
} // namespace maicord::invite_copy_test

namespace maicord {
bool clipboard_system_open() noexcept {
    ++invite_copy_test::clip.opens;
    invite_copy_test::clip.publishes = 0;
    return !invite_copy_test::clip.refuse;
}
bool clipboard_system_publish(UINT format, HGLOBAL data) noexcept {
    auto& clip = invite_copy_test::clip;
    ++clip.publishes;
    clip.format = format;
    const auto* wide = static_cast<const wchar_t*>(GlobalLock(data));
    const int bytes =
        wide ? WideCharToMultiByte(CP_UTF8, 0, wide, -1, clip.text, sizeof(clip.text), nullptr, nullptr)
             : 0;
    if (wide)
        GlobalUnlock(data);
    if (bytes <= 0)
        clip.text[0] = 0;
    GlobalFree(data);
    return true;
}
void clipboard_system_close() noexcept {
}
} // namespace maicord

namespace maicord::app_detail {
bool application_text_form_open(const ApplicationTextFormRequest& request) noexcept {
    invite_copy_test::revoke_forms += request.confirmation_only && request.danger;
    return true;
}
namespace server_settings {
Settings* owner;
u32 push(const UiSettingsControl& control, UiSettingsControlPresentation) noexcept {
    if (invite_copy_test::row_count < 16)
        invite_copy_test::rows[invite_copy_test::row_count++] = control;
    return invite_copy_test::row_count;
}
Utf8View text_view(Text text) noexcept {
    if (text.bits & Text::kViewBit)
        return *reinterpret_cast<const Utf8View*>(text.bits & ~Text::kViewBit);
    const char* chars = reinterpret_cast<const char*>(text.bits);
    return {chars, c_string_length(chars)};
}
u32 push_category(u64 id, Text label, Text) noexcept {
    return push({id, text_view(label), {}, UiSettingsControlKind::category, true, false, {}, 0, {}, nullptr}, {});
}
u32 push_notice(Text description, Text label) noexcept {
    return push({1, text_view(label), text_view(description), UiSettingsControlKind::notice, true, false, {}, 0, {},
                 nullptr},
                {});
}
u32 push_error(Text label, Text description, Text action) noexcept {
    return push({1, text_view(label), text_view(description), UiSettingsControlKind::notice, true, false, {}, 0,
                 text_view(action), nullptr},
                {});
}
u32 push_buttons(u64 id, Text label, Text description, bool enabled, const char* names, u32 shown,
                 UiSettingsControlPresentation presentation) noexcept {
    static Utf8View choices[16][4];
    Utf8View* row = choices[invite_copy_test::row_count < 16 ? invite_copy_test::row_count : 15];
    NameList list{names};
    for (u32 i = 0; i < shown; ++i) {
        if (i)
            list.next();
        row[i] = list.name();
    }
    UiSettingsControl control{id, text_view(label), text_view(description), UiSettingsControlKind::buttons, enabled,
                              false, {row, shown}, 0, {}, nullptr};
    return push(control, presentation);
}
u32 push_loading(Text description, u64) noexcept {
    return push_notice(description, {});
}
u32 push_count_heading(Bank& bank, Text title, u32 count) noexcept {
    const u32 start = bank.used;
    bank.add(text_view(title));
    bank.number(count);
    return push_category(control_list_category, bank.since(start), {});
}
void open_confirmation(u64 kind, u64 item, Text, Text, Text, bool (*)(TextFormOwnerKey, Utf8View) noexcept,
                       Text) noexcept {
    ApplicationTextFormRequest request{};
    request.danger = request.confirmation_only = true;
    request.owner = form_key(kind, item);
    application_text_form_open(request);
}
const ChatStateStore& state() noexcept {
    return invite_copy_test::chat;
}
Utf8View user_label(u64) noexcept {
    return {};
}
void relative_time(Bank& bank, u64, u64, bool) noexcept {
    bank.add(utf8_literal("soon"));
}
bool editable() noexcept {
    return invite_copy_test::can_edit;
}
bool busy() noexcept {
    return invite_copy_test::request_busy;
}
bool connected() noexcept {
    return true;
}
void queue(Op, u64) noexcept {
}
bool request_read(bool&, bool&, Op) noexcept {
    return false;
}
void retry_read(bool&, bool&, Op) noexcept {
}
TextFormOwnerKey form_key(u64, u64) noexcept {
    return {};
}
bool form_valid(TextFormOwnerKey) noexcept {
    return true;
}
bool form_save(TextFormOwnerKey, Utf8View) noexcept {
    return true;
}
} // namespace server_settings
} // namespace maicord::app_detail

extern "C" __declspec(noreturn) void WINAPI maicord_invite_copy_runtime_entry() noexcept {
    using namespace maicord::invite_copy_test;
    run();
    maicord::test_exit(failures, "invite copy");
}
