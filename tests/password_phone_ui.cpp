#include "maicord/ui/login_text_presentation.hpp"
#include "maicord/ui/login_presentation.hpp"
#include "maicord/ui/secure_text_store.hpp"
#include "../src/app/application_login_geometry.hpp"
#include "maicord/sys/tsf.hpp"

namespace maicord {
namespace {
void synchronize(LoginScene& scene, LoginView& view) noexcept {
    scene.phone_mode = login_phone_mode(view.status); scene.reset_mode = view.reset_mode;
    scene.recovery_mode = login_recovery_mode(view.status);
    scene.back_only = login_requires_back(view.status, view.remote_status);
    scene.phone_resend_disabled = scene.phone_mode && !login_phone_editable(view.status);
    scene.credential_fields_hidden = login_credential_fields_hidden(view.status, view.remote_status, view.reset_mode);
    scene.action_disabled = login_view_action_disabled(view); scene.submit_disabled = login_view_submit_disabled(view);
    scene.recovery_disabled = login_view_recovery_disabled(view); scene.focus = view.focus;
}
struct PhoneTsf {
    SecureEdit* edit;
    const UiRect* clip = nullptr;
    static bool bounds(void*, SecureTextStoreGeometry* output) noexcept {
        *output = {{130, 230, 730, 296}, {0, 0, 2000, 2000}}; return true;
    }
    static bool range(void* context, u32 first, u32 last, RECT* rectangle, bool* clipped) noexcept {
        const auto& fixture = *static_cast<PhoneTsf*>(context);
        return app_detail::application_phone_range_geometry(*fixture.edit,
            {20, 20, 420, 64}, 144, {100, 200}, first, last, rectangle, clipped, fixture.clip);
    }
    static bool point(void* context, POINT p, bool round, u32* position, bool* inside) noexcept {
        return login_phone_point_position({20, 20, 420, 64}, static_cast<PhoneTsf*>(context)->edit->storage.bytes,
            (p.x - 100) / 1.5f, (p.y - 200) / 1.5f, round, position, inside);
    }
};
struct PhoneSink final : ITextStoreACPSink {
    ULONG references = 1; ITextStoreACP* store = nullptr; const WCHAR* insertion = nullptr; ULONG units = 0;
    HRESULT insertion_result = E_FAIL, range_result = E_FAIL, point_result = E_FAIL; LONG position = -1; RECT rectangle{};
    BOOL range_clipped = FALSE; bool write = true;
    LONG insertion_start = 0, insertion_end = -1;
    TS_TEXTCHANGE text_change{};
    WCHAR copied[6]{}; ULONG copied_units = 0; LONG copied_next = 0; HRESULT copy_result = E_FAIL;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER; *output = nullptr;
        if (!bytes_equal(&id, &__uuidof(IUnknown), sizeof(GUID)) && !bytes_equal(&id, &__uuidof(ITextStoreACPSink), sizeof(GUID))) return E_NOINTERFACE;
        *output = static_cast<ITextStoreACPSink*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE OnLockGranted(DWORD) override {
        LONG end = 0; if (store->GetEndACP(&end) != S_OK) return E_FAIL;
        TS_TEXTCHANGE change{};
        if (write) insertion_result = store->SetText(0, insertion_start, insertion_end < 0 ? end : insertion_end, insertion, units, &change);
        text_change = change;
        if (store->GetEndACP(&end) != S_OK) return E_FAIL;
        ULONG runs = 0;
        copy_result = store->GetText(0, end, copied, 6, &copied_units, nullptr, 0, &runs, &copied_next);
        range_result = store->GetTextExt(1, 0, end, &rectangle, &range_clipped);
        const auto cell = login_phone_cell_rectangle({20, 20, 420, 64}, 2);
        POINT p{100 + static_cast<LONG>((cell.left + 2) * 1.5f), 200 + static_cast<LONG>((cell.top + 20) * 1.5f)};
        point_result = store->GetACPFromPoint(1, &p, GXFPF_NEAREST | GXFPF_ROUND_NEAREST, &position);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTextChange(DWORD, const TS_TEXTCHANGE*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnLayoutChange(TsLayoutCode, TsViewCookie) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStatusChange(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnAttrsChange(LONG, LONG, ULONG, const TS_ATTRID*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStartEditTransaction() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnEndEditTransaction() override { return S_OK; }
};
} // namespace
void test_password_phone_ui(void (*check)(bool, const char*) noexcept) noexcept {
    LoginScene scene{}; PaintStream paint{};
    check(paint_stream_initialize(&paint), "phone native paint owner initializes");
    UiTextCommand commands[kMaximumLoginTextCommands]{}; UiTextCommandStream text{};
    check(ui_text_command_stream_initialize(&text, {commands, kMaximumLoginTextCommands}), "phone text commands reuse fixed capacity");
    const LoginStatus statuses[]{LoginStatus::recovery_phone_required, LoginStatus::recovery_phone_verify_pending,
        LoginStatus::recovery_phone_resend_pending, LoginStatus::recovery_phone_input_invalid,
        LoginStatus::recovery_phone_code_rejected, LoginStatus::recovery_phone_resent, LoginStatus::recovery_phone_resend_failed,
        LoginStatus::recovery_phone_verify_rate_limited, LoginStatus::recovery_phone_resend_rate_limited,
        LoginStatus::recovery_phone_captcha_required, LoginStatus::recovery_phone_failed, LoginStatus::recovery_phone_token_ready};
    const float widths[]{320, 400, 560, 1000};
    for (float width : widths) for (auto status : statuses) {
        LoginView view{}; view.status = status; view.visible = true; view.password_mask = utf8_literal("123456");
        view.phone_code_ready = login_phone_editable(status); view.focus = login_phone_editable(status) ? LoginControl::phone_code : LoginControl::back;
        view.edit_active = login_phone_editable(status); view.edit_anchor = 2; view.edit_caret = 4;
        synchronize(scene, view); scene.focus_visible = true;
        check(login_scene_layout(&scene, width, 600), "phone all responsive native states lay out"); view.focus = scene.focus;
        check(scene.phone_mode && !scene.qr_visible && scene.widgets.count <= kMaximumLoginWidgets &&
            !login_scene_element(scene, LoginElement::login_field) && !login_scene_element(scene, LoginElement::login_label) &&
            scene.mfa_capabilities == 0 && login_view_matches_scene(view, &scene) &&
            login_text_presentation_build(scene, view, kDefaultTheme, &text),
            "phone hides original identifier password QR and MFA choices within existing widget bank");
        const auto bounds = login_scene_content_rectangle(scene, LoginElement::password_field);
        u32 digit_commands = 0; bool safe = true;
        for (u32 i = 0; i < text.count; ++i) {
            safe &= ui_rect_has_area(ui_rect_intersection(commands[i].rectangle, commands[i].clip));
            if (commands[i].owner.handle.slot == scene.elements[static_cast<u32>(LoginElement::password_field)].slot) {
                safe &= commands[i].text.bytes == 1 && commands[i].text.data[0] == static_cast<char>('1' + digit_commands) &&
                    ui_rect_equal(commands[i].rectangle, login_phone_cell_rectangle(bounds, digit_commands));
                ++digit_commands;
            }
        }
        check(safe && digit_commands == 6, "six code characters occupy exact shared cell rectangles with clipped native copy");
        const auto a = login_phone_cell_rectangle(bounds, 1), b = login_phone_cell_rectangle(bounds, 2), c = login_phone_cell_rectangle(bounds, 3);
        check(c.left - b.right > b.left - a.right, "phone code has a larger midpoint spacer after digit three");
        const auto back = login_scene_rectangle(scene, LoginElement::forgot_password);
        const auto resend = login_scene_rectangle(scene, LoginElement::mfa_sms);
        const auto verify = login_scene_rectangle(scene, LoginElement::submit);
        check(login_scene_hit_test(scene, back.left + 1, back.top + 1) == LoginControl::back &&
            login_control_action(LoginControl::back, status, {}) == LoginAction::back &&
            login_control_action(LoginControl::password, status, {}) == LoginAction::none &&
            login_control_action(LoginControl::login, status, {}) == LoginAction::none &&
            login_control_action(LoginControl::qr, status, {}) == LoginAction::none &&
            login_control_action(LoginControl::mfa_sms, status, {}) == LoginAction::none,
            "shared phone mouse Enter and Space semantics cannot fall through to credential or MFA submission");
        if (login_phone_editable(status)) {
            check(login_scene_hit_test(scene, resend.left + 1, resend.top + 1) == LoginControl::phone_resend &&
                login_scene_hit_test(scene, verify.left + 1, verify.top + 1) == LoginControl::submit &&
                login_control_action(LoginControl::phone_code, status, {}) == LoginAction::phone_verify &&
                login_control_action(LoginControl::phone_resend, status, {}) == LoginAction::phone_resend &&
                login_scene_focus(&scene, LoginControl::phone_code), "phone editable actions have separate Verify and Resend dispatch");
            const auto next = login_scene_move_focus(&scene, false);
            check(next == LoginControl::back && login_scene_move_focus(&scene, true) == LoginControl::phone_code,
                "native Tab and Shift-Tab traverse only visible phone controls");
        } else check(!login_scene_focus(&scene, LoginControl::phone_code) && !login_scene_focus(&scene, LoginControl::phone_resend) &&
            !login_scene_focus(&scene, LoginControl::submit) && login_scene_focus(&scene, LoginControl::back) &&
            login_scene_move_focus(&scene, false) == LoginControl::back,
            "pending CAPTCHA authoritative cooldown and indeterminate states retain Back as the only enabled control");
        UiRect selections[2]{login_phone_cell_rectangle(bounds, 2), login_phone_cell_rectangle(bounds, 3)};
        const LoginPresentationInput input{{selections, 2}, {}, login_phone_caret_rectangle(bounds, 4), nullptr, view.edit_active, true};
        check(login_presentation_build(scene, input, kDefaultTheme, width, 600, 144, &paint), "native phone cell selection and caret paint uses fixed geometry");
        const auto layouts = scene.layout_rebuilds; const auto storage = scene.storage.arena.base;
        check(login_scene_layout(&scene, width, 600) && scene.layout_rebuilds == layouts && scene.storage.arena.base == storage,
            "unchanged phone state retains native widgets without per-frame allocation or layout rebuild");
    }
    LoginView unsafe{}; unsafe.status = LoginStatus::recovery_phone_required; unsafe.login = utf8_literal("original-phone");
    check(!login_view_valid(unsafe), "phone projection rejects original phone identifier exposure");
    unsafe.login = {}; unsafe.password_mask = utf8_literal("12345a"); check(!login_view_valid(unsafe), "phone projection rejects nondigit code text");
    SecureEdit edit{};
    check(secure_edit_initialize(&edit, 64, false), "phone reuses an ordinary locked MFA edit bank");
    app_detail::application_phone_tsf_mutation_stamp() = 0;
    edit.revision = ~u32{0};
    const auto boundary_target = app_detail::application_phone_input_target(&edit);
    check(boundary_target.operations == app_detail::application_phone_input_target(&edit).operations &&
        text_input_ready(boundary_target) && app_detail::application_phone_insert_utf8(&edit, utf8_literal("1")) == SecureEditResult::changed &&
        !edit.revision && app_detail::application_phone_tsf_mutation_stamp() == 1,
        "constant phone target survives revision wrap to zero and encodes a live mutation without the absent sentinel");
    app_detail::application_phone_select_all(&edit); edit.revision = ~u32{0} - 1;
    check(app_detail::application_phone_insert_utf8(&edit, utf8_literal("2")) == SecureEditResult::changed &&
        edit.revision == ~u32{0} && app_detail::application_phone_tsf_mutation_stamp() == 0x100000000ull &&
        app_detail::application_phone_insert_utf8(&edit, utf8_literal("x")) == SecureEditResult::invalid_input &&
        edit.revision == ~u32{0} && app_detail::application_phone_tsf_mutation_stamp() == 0x100000000ull,
        "phone mutation stamp represents UINT_MAX and invalid edits cannot replace its admitted revision");
    app_detail::application_phone_tsf_mutation_stamp() = 0;
    check(app_detail::application_phone_move(&edit, SecureEditMove::home, false) == SecureEditResult::changed && edit.revision == ~u32{0} &&
        app_detail::application_phone_tsf_mutation_stamp() != static_cast<u64>(edit.revision) + 1,
        "selection-only changes cannot restore a consumed UINT_MAX phone mutation stamp");
    secure_edit_clear(&edit);
    check(!edit.revision && app_detail::application_phone_tsf_mutation_stamp() != static_cast<u64>(edit.revision) + 1,
        "consumed zero phone stamp never aliases an actual revision-zero mutation");
    check(app_detail::application_phone_insert_utf8(&edit, utf8_literal("123")) == SecureEditResult::changed &&
        app_detail::application_phone_insert_utf8(&edit, utf8_literal("4567")) == SecureEditResult::invalid_input && edit.storage.bytes == 6 &&
        bytes_equal(edit.storage.data, "123   ", 6) &&
        app_detail::application_phone_insert_utf8(&edit, utf8_literal("4a6")) == SecureEditResult::invalid_input && edit.storage.bytes == 6 &&
        app_detail::application_phone_insert_utf8(&edit, utf8_literal("456")) == SecureEditResult::changed && edit.storage.bytes == 6,
        "typed and pasted digit preflight rejects overlong or invalid entire insertion without truncating or modifying prior text");
    check(app_detail::application_phone_move(&edit, SecureEditMove::previous, false) == SecureEditResult::changed &&
        app_detail::application_phone_backspace(&edit) == SecureEditResult::changed && bytes_equal(edit.storage.data, "12345 ", 6) &&
        app_detail::application_phone_insert_utf8(&edit, utf8_literal("9")) == SecureEditResult::changed && bytes_equal(edit.storage.data, "123459", 6),
        "phone Backspace clears the occupied current cell without shifting later digits");
    check(app_detail::application_phone_select(&edit, {2, 2}) == SecureEditResult::changed &&
        app_detail::application_phone_delete_forward(&edit) == SecureEditResult::changed && bytes_equal(edit.storage.data, "12 459", 6) &&
        !login_phone_code_valid({reinterpret_cast<const char*>(edit.storage.data), 6}) &&
        app_detail::application_phone_select(&edit, {4, 4}) == SecureEditResult::changed &&
        app_detail::application_phone_insert_utf8(&edit, utf8_literal("7")) == SecureEditResult::changed &&
        bytes_equal(edit.storage.data, "12 479", 6) && edit.caret == 2,
        "sparse earlier hole survives positional overwrite and typing focuses the first empty cell");
    check(app_detail::application_phone_backspace(&edit) == SecureEditResult::changed &&
        bytes_equal(edit.storage.data, "1  479", 6) && edit.caret == 1 &&
        app_detail::application_phone_select(&edit, {1, 3}) == SecureEditResult::changed &&
        app_detail::application_phone_insert_utf8(&edit, utf8_literal("23")) == SecureEditResult::changed &&
        bytes_equal(edit.storage.data, "123479", 6) && login_phone_code_valid({reinterpret_cast<const char*>(edit.storage.data), 6}),
        "empty-cell Backspace clears left and selected paste fills its original cell positions");
    check(app_detail::application_phone_select(&edit, {1, 4}) == SecureEditResult::changed &&
        app_detail::application_phone_insert_utf8(&edit, utf8_literal("8")) == SecureEditResult::changed &&
        bytes_equal(edit.storage.data, "18  79", 6) && edit.caret == 2,
        "short selection replacement clears leftover selected cells rather than compacting the suffix");
    const auto sparse_revision = edit.revision;
    app_detail::application_phone_tsf_mutation_stamp() = 0;
    check(app_detail::application_phone_move(&edit, SecureEditMove::next, true) == SecureEditResult::changed &&
        edit.anchor == 2 && edit.caret == 3 && edit.revision == sparse_revision &&
        app_detail::application_phone_tsf_mutation_stamp() == 0,
        "shift navigation across empty cells preserves the consumed content mutation stamp");
    LoginView sparse{}; sparse.status = LoginStatus::recovery_phone_required; sparse.visible = true;
    sparse.focus = LoginControl::phone_code; sparse.edit_active = true; sparse.edit_anchor = 2; sparse.edit_caret = 5;
    sparse.password_mask = {reinterpret_cast<const char*>(edit.storage.data), 6};
    synchronize(scene, sparse);
    check(login_view_valid(sparse) && login_view_submit_disabled(sparse) && login_scene_layout(&scene, 400, 600) &&
        login_text_presentation_build(scene, sparse, kDefaultTheme, &text), "sparse projection admits holes while disabling Verify");
    u32 occupied_commands = 0;
    const auto sparse_bounds = login_scene_content_rectangle(scene, LoginElement::password_field);
    for (u32 i = 0; i < text.count; ++i) if (commands[i].owner.handle.slot == scene.elements[static_cast<u32>(LoginElement::password_field)].slot) {
        const u32 positions[]{0, 1, 4, 5};
        check(occupied_commands < 4 && ui_rect_equal(commands[i].rectangle, login_phone_cell_rectangle(sparse_bounds, positions[minimum(occupied_commands, 3u)])),
            "sparse glyphs retain their own cell rectangles and empty cells emit no glyph");
        ++occupied_commands;
    }
    check(occupied_commands == 4, "sparse code draws only four occupied cells");
    sparse.phone_code_ready = true;
    check(!login_view_valid(sparse), "projection cannot claim a sparse six-byte code is complete");
    const auto last_cell = login_phone_cell_rectangle(sparse_bounds, 5);
    u32 clicked = 0; bool inside = false;
    check(login_phone_point_position(sparse_bounds, 0, last_cell.right - 1, last_cell.top + 1, true, &clicked, &inside) &&
        clicked == 5 && inside, "the right side of an empty sixth cell selects its independent slot");
    secure_edit_clear(&edit);
    SecureTextStore store{}; PhoneTsf geometry{&edit}; PhoneSink sink;
    const bool bound = secure_text_store_initialize(&store, {reinterpret_cast<HWND>(1), PhoneTsf::bounds, nullptr, &geometry, PhoneTsf::range, PhoneTsf::point}) &&
        secure_text_store_bind_target(&store, app_detail::application_phone_input_target(&edit), SecureTextInputScope::digits);
    check(bound, "phone TSF binds the digit-validating target with no new native window");
    if (bound) {
        sink.store = static_cast<ITextStoreACP*>(secure_text_store_test_interface(&store));
        const bool advised = sink.store && sink.store->AdviseSink(__uuidof(ITextStoreACPSink), &sink, TS_AS_ALL_SINKS) == S_OK;
        check(advised, "phone TSF fixture advises native ACP sink");
        if (advised) {
            const WCHAR* cases[]{L"1234567", L"12a456", (L"\xff11" L"23456"), L"123456"};
            const u32 counts[]{7, 6, 6, 6};
            for (u32 i = 0; i < 4; ++i) {
                sink.insertion = cases[i]; sink.units = counts[i]; HRESULT session = E_FAIL;
                const auto revision = edit.revision;
                check(sink.store->RequestLock(TS_LF_SYNC | TS_LF_READWRITE, &session) == S_OK && session == S_OK &&
                    (i == 3 ? sink.insertion_result == S_OK && edit.storage.bytes == 6 : FAILED(sink.insertion_result) && !edit.storage.bytes && edit.revision == revision),
                    "actual TSF SetText rejects overlong nonASCII nondigit inserts atomically and accepts exactly six digits");
                if (i == 3) check(sink.range_result == S_OK && sink.point_result == S_OK && sink.position == 2 &&
                    sink.rectangle.left > 130 && sink.rectangle.right < 730,
                    "TSF range and point geometry share six-cell mapping including DPI and screen origin");
            }
            sink.insertion_start = 2; sink.insertion_end = 3; sink.insertion = nullptr; sink.units = 0;
            HRESULT sparse_session = E_FAIL;
            check(sink.store->RequestLock(TS_LF_SYNC | TS_LF_READWRITE, &sparse_session) == S_OK && sparse_session == S_OK &&
                sink.insertion_result == S_OK && sink.text_change.acpStart == 2 && sink.text_change.acpOldEnd == 3 &&
                sink.text_change.acpNewEnd == 3 && edit.storage.bytes == 6 && bytes_equal(edit.storage.data, "12 456", 6) &&
                sink.copy_result == S_OK && sink.copied_units == 6 && sink.copied_next == 6 && sink.copied[2] == L' ' &&
                sink.copied[3] == L'4', "actual TSF deletion preserves six ACP positions and exposes the hole without shifting subsequent digits");
            sink.insertion_start = 4; sink.insertion_end = 4; sink.insertion = L"98"; sink.units = 2; sparse_session = E_FAIL;
            check(sink.store->RequestLock(TS_LF_SYNC | TS_LF_READWRITE, &sparse_session) == S_OK && sparse_session == S_OK &&
                sink.insertion_result == S_OK && bytes_equal(edit.storage.data, "12 498", 6) &&
                sink.text_change.acpOldEnd == 6 && sink.text_change.acpNewEnd == 6 && !login_phone_code_valid({reinterpret_cast<const char*>(edit.storage.data), 6}),
                "actual TSF positional paste overwrites only addressed slots and cannot complete an earlier hole");
            sink.write = false;
            const UiRect wholly_clipped{};
            geometry.clip = &wholly_clipped;
            HRESULT session = E_FAIL;
            check(sink.store->RequestLock(TS_LF_SYNC | TS_LF_READ, &session) == S_OK && session == S_OK &&
                sink.range_result == S_OK && sink.range_clipped && !sink.rectangle.left && !sink.rectangle.top &&
                !sink.rectangle.right && !sink.rectangle.bottom,
                "actual TSF GetTextExt returns zero geometry and clipped=true for a wholly clipped phone widget");
            const UiRect partially_clipped{180, 30, 230, 54};
            geometry.clip = &partially_clipped; session = E_FAIL;
            check(sink.store->RequestLock(TS_LF_SYNC | TS_LF_READ, &session) == S_OK && session == S_OK &&
                sink.range_result == S_OK && sink.range_clipped && sink.rectangle.left == 370 && sink.rectangle.top == 245 &&
                sink.rectangle.right == 445 && sink.rectangle.bottom == 281,
                "actual TSF GetTextExt intersects partial phone clip before DPI and screen-origin projection");
            geometry.clip = nullptr;
            sink.store->UnadviseSink(&sink);
        }
    }
    secure_text_store_release(&store); secure_edit_release(&edit);
    app_detail::application_phone_tsf_mutation_stamp() = 0;
    scene.phone_mode = false; scene.back_only = false; scene.credential_fields_hidden = false; scene.phone_resend_disabled = false;
    scene.action_disabled = scene.submit_disabled = scene.recovery_disabled = false; scene.focus = LoginControl::login;
    check(login_scene_layout(&scene, 1000, 700) && scene.qr_visible && login_scene_focus(&scene, LoginControl::login),
        "phone teardown restores original credential and QR layout within retained scene owner");
    login_scene_release(&scene); paint_stream_release(&paint);
}
} // namespace maicord
