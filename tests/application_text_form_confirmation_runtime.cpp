#include "application_state.hpp"
#include "application_text_form_state.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_composer_input.hpp"
#include "application_composer_text_input.hpp"
#include "application_message_interaction.hpp"
#include "application_scrolling.hpp"
#include "application_shell_input.hpp"
#include "maicord/ui/clipboard.hpp"
#include "test_output.hpp"

namespace maicord::confirmation_runtime_test {
using namespace app_detail;
struct Observations {
    u32 failures, editor_calls, store_calls, measurements, builds, saves, render_copies;
    u32 login_prepares, login_releases, login_refreshes;
    bool owner_valid, save_ok, fail_build, ordinary_projection;
    void* last_form;
} seen{};
UiTextMeasureRecord measure_records[32]{};
ApplicationTextFormProgress async_progress{};
u32 closed_owners{};
void check(bool passed, const char* label) noexcept;
bool valid(TextFormOwnerKey key) noexcept;
ApplicationTextFormProgress progress(TextFormOwnerKey key) noexcept {
    check(valid(key), "async form: progress projection uses exact current semantic owner");
    return async_progress;
}
void closed(TextFormOwnerKey key) noexcept {
    ++closed_owners;
    check(key.scope == 7 && key.collection == 2 && key.item == 90 && !g_app.text_form &&
              !g_app.renderer.text_form,
          "async form: closed callback receives exact owner after renderer and native form detach");
}
bool measure_label(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* output) noexcept {
    const auto valid_text = utf8_validate(request.text);
    if (!output || !valid_text.valid || request.constraint.width <= 0)
        return false;
    const float natural = static_cast<float>(valid_text.codepoints) * 8;
    const u32 columns = maximum(1u, static_cast<u32>(request.constraint.width / 8));
    const u32 lines = request.no_wrap ? 1u : maximum(1u, (valid_text.codepoints + columns - 1) / columns);
    *output = {{minimum(natural, request.constraint.width), static_cast<float>(lines) * 20}, 16, lines};
    return true;
}
void check(bool valid, const char* label) noexcept {
    test_check(&seen.failures, valid, label);
}
bool valid(TextFormOwnerKey key) noexcept {
    return seen.owner_valid && key.scope == 7 && key.collection == 2 && key.item == 90;
}
bool save(TextFormOwnerKey key, Utf8View text) noexcept {
    ++seen.saves;
    check(valid(key) && !text.data && !text.bytes,
          "confirmation: Accept calls current semantic owner with empty payload");
    return seen.save_ok;
}
bool freed(void* pointer) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    return pointer && VirtualQuery(pointer, &info, sizeof(info)) == sizeof(info) && info.State == MEM_FREE;
}
void report_number(const char* label, u64 value) noexcept {
    u32 bytes = 0;
    while (label[bytes])
        ++bytes;
    char digits[20];
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    for (u32 i = 0; i < count / 2; ++i) {
        const char saved = digits[i];
        digits[i] = digits[count - i - 1];
        digits[count - i - 1] = saved;
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), label, bytes, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), digits, count, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
}
void footprint(const ApplicationTextForm& form) noexcept {
    MEMORY_BASIC_INFORMATION owner{}, scene{};
    const bool queried =
        VirtualQuery(&form, &owner, sizeof(owner)) == sizeof(owner) &&
        VirtualQuery(form.scene.storage.arena.base, &scene, sizeof(scene)) == sizeof(scene);
    check(queried && owner.State == MEM_COMMIT && owner.AllocationBase == &form &&
              owner.RegionSize >= sizeof(form) && scene.State == MEM_COMMIT &&
              scene.AllocationBase == form.scene.storage.arena.base &&
              scene.RegionSize >= form.scene.storage.arena.committed,
          "confirmation footprint: actual owner/scene committed regions contain their retained objects");
    report_number("Confirmation sizeof ApplicationTextForm: ", sizeof(form));
    report_number("Confirmation sizeof TextFormScene: ", sizeof(form.scene));
    report_number("Confirmation sizeof TextFormSurface: ", sizeof(form.surface));
    report_number("Confirmation sizeof SecureEdit: ", sizeof(form.edit));
    report_number("Confirmation sizeof SecureTextStore: ", sizeof(form.store));
    report_number("Confirmation sizeof SecureTextComposition: ", sizeof(form.composition));
    report_number("Confirmation inline display bytes: ", 0);
    report_number("Confirmation inline labels capacity bytes: ", sizeof(form.labels));
    report_number("Confirmation copied label bytes: ",
                  form.request.title.bytes + form.request.context.bytes + form.request.label.bytes +
                      form.request.accept.bytes);
    report_number("Confirmation scene arena committed bytes: ", form.scene.storage.arena.committed);
    report_number("Confirmation scene arena used bytes: ", form.scene.storage.arena.offset);
    if (queried) {
        report_number("Confirmation owner committed region bytes: ", owner.RegionSize);
        report_number("Confirmation scene committed region bytes: ", scene.RegionSize);
    }
}
ApplicationTextFormRequest request() noexcept {
    return {utf8_literal("Remove friend?"),
            utf8_literal("This removes your connection to this person."),
            utf8_literal("Unused editor label"),
            utf8_literal("Remove friend"),
            {nullptr, 1},
            {7, 2, 90},
            valid,
            save,
            1,
            true};
}
void tail(u32 key) noexcept {
    application_text_form_input(UiInputKind::key_up, key, false);
}
void ordinary_projection() noexcept {
    seen.fail_build = false;
    check(application_text_form_open(request()),
          "ordinary projection: owned confirmation fixture opens before model boundary probe");
    auto* form = g_app.text_form;
    if (!form)
        return;
    // Exercise the actual ordinary project/scene/surface with owned model bytes;
    // allocation/history/TSF opening remains covered by the normal GPU fixture.
    u8 model[] = {'A', 0xf0, 0x9f, 0x98, 0x80};
    form->request.confirmation_only = false;
    form->edit.storage = {model, sizeof(model), sizeof(model), true};
    form->edit.caret = 2;
    seen.ordinary_projection = true;
    const auto refresh_projection = []() noexcept {
        g_app.keyboard_focus_visible = !g_app.text_form->scene.content.focus_visible;
        return application_text_form_synchronize();
    };
    check(refresh_projection() && form->scene.content.text.data == reinterpret_cast<const char*>(model) &&
              form->scene.content.text.bytes == sizeof(model) && form->scene.content.anchor_utf16 == 0 &&
              form->scene.content.caret_utf16 == 3 && !seen.render_copies,
          "ordinary projection: actual controller borrows exact UTF-8 model and validates supplementary "
          "UTF-16 selection without display copying");
    model[0] = 'B';
    ++form->edit.revision;
    check(refresh_projection() && form->scene.content.text.data == reinterpret_cast<const char*>(model) &&
              form->scene.content.text.data[0] == 'B' && !seen.render_copies,
          "ordinary projection: next model revision retains borrowed owner and reprojects current bytes");
    const u32 builds = seen.builds, calls = seen.editor_calls;
    form->edit.password = true;
    check(!refresh_projection() && seen.builds == builds && seen.editor_calls == calls,
          "ordinary projection: password is rejected before obtaining plaintext view or publishing "
          "renderer work");
    form->edit.password = false;
    form->edit.storage.capacity = kApplicationTextFormBytes + 1;
    check(!refresh_projection() && seen.builds == builds && seen.editor_calls == calls,
          "ordinary projection: oversized model capacity is rejected before borrowing");
    form->edit.storage.capacity = sizeof(model) - 1;
    check(!refresh_projection() && seen.builds == builds && seen.editor_calls == calls,
          "ordinary projection: byte count beyond capacity is rejected before borrowing");
    form->edit.storage.capacity = sizeof(model);
    form->edit.storage.data = nullptr;
    check(!refresh_projection() && seen.builds == builds,
          "ordinary projection: nonempty null view cannot enter presentation");
    form->edit.storage.data = model;
    form->edit.caret = 99;
    check(!refresh_projection() && seen.builds == builds,
          "ordinary projection: invalid UTF-16 selection prevents renderer publication");
    form->edit.storage.bytes = 0;
    form->edit.caret = 0;
    check(refresh_projection() && form->scene.content.text.data == reinterpret_cast<const char*>(model) &&
              !form->scene.content.text.bytes && !seen.render_copies,
          "ordinary projection: valid empty model borrows zero bytes without an extra buffer");
    form->request.confirmation_only = true;
    form->edit = {};
    application_text_form_close(false, false);
    seen.ordinary_projection = false;
}
void locked_progress_lifecycle() noexcept {
    seen.save_ok = false;
    const u32 initial_editor_calls = seen.editor_calls, initial_store_calls = seen.store_calls;
    const u32 initial_saves = seen.saves, initial_closed = closed_owners;
    async_progress = {
        utf8_literal("Confirm action"), utf8_literal("Continue"), 20, false, false, false, true, true};
    auto asynchronous = request();
    asynchronous.progress = progress;
    asynchronous.closed = closed;
    check(application_text_form_open(asynchronous) && g_app.text_form,
          "locked async form: default cancellable confirmation opens before commitment");
    auto* form = g_app.text_form;
    if (!form)
        return;
    const auto* cancel = text_form_node(form->scene, TextFormElement::cancel);
    const LPARAM cancel_point =
        cancel ? MAKELPARAM(static_cast<short>((cancel->rectangle.left + cancel->rectangle.right) * .5f),
                            static_cast<short>((cancel->rectangle.top + cancel->rectangle.bottom) * .5f))
               : 0;
    LRESULT result = 0;
    check(cancel &&
              application_text_form_window_message(WM_LBUTTONDOWN, MK_LBUTTON, cancel_point, &result) &&
              form->pointer_button.generation &&
              application_text_form_input(UiInputKind::key_down, VK_SPACE, false) &&
              form->keyboard.pressed.manager == &form->scene.widgets,
          "locked async form: Cancel owns real pointer and Space press before progress changes");
    async_progress.editable = async_progress.can_accept = async_progress.can_cancel =
        async_progress.dismissible = false;
    check(application_text_form_synchronize() && g_app.text_form == form && form->scene.active &&
              !form->can_cancel && !form->dismissible && !form->scene.content.can_cancel &&
              text_form_focused(form->scene) == TextFormElement::count &&
              !form->pointer_button.generation && !form->scene.widgets.captured.generation &&
              !form->keyboard.pressed.manager && !form->dragging && !application_text_form_next_wake(),
          "locked async form: committed progress retires Cancel press capture focus and caret without "
          "closing");
    async_progress.dismissible = true;
    check(application_text_form_synchronize() && form->dismissible && !form->can_cancel,
          "locked async form: dismissal flag changes synchronize even when host revision stays unchanged");
    async_progress.dismissible = false;
    check(application_text_form_synchronize() && !form->dismissible,
          "locked async form: nondismissible flag restores without a synthetic revision change");
    check(application_text_form_window_message(WM_LBUTTONUP, 0, cancel_point, &result) &&
              application_text_form_input(UiInputKind::key_up, VK_SPACE, false) &&
              g_app.text_form == form && !text_form_focus(&form->scene, TextFormElement::cancel) &&
              seen.saves == initial_saves,
          "locked async form: disabled Cancel click and old Space release cannot dismiss or accept");
    check(application_text_form_input(UiInputKind::key_down, VK_ESCAPE, false) && g_app.text_form == form &&
              application_text_form_input(UiInputKind::key_up, VK_ESCAPE, false) &&
              application_text_form_window_message(WM_LBUTTONDOWN, 0, MAKELPARAM(1, 1), &result) &&
              application_text_form_window_message(WM_LBUTTONUP, 0, MAKELPARAM(1, 1), &result) &&
              g_app.text_form == form && closed_owners == initial_closed && seen.saves == initial_saves,
          "locked async form: Escape and outside down/up remain modal and consume dismissal without "
          "click-through");
    check(
        application_text_form_window_message(WM_KILLFOCUS, 0, 0, &result) &&
            application_text_form_window_message(WM_CANCELMODE, 0, 0, &result) && g_app.text_form == form &&
            !form->keyboard.pressed.manager && !form->pointer_button.generation &&
            !form->scene.widgets.captured.generation && !form->dragging && closed_owners == initial_closed,
        "locked async form: blur and cancel mode clear owned transients while preserving committed owner");
    const u32 locked_builds = seen.builds, locked_scene_revision = form->scene.revision;
    const u32 locked_layout_revision = form->scene.widgets.layout.revision;
    const auto scene_storage = form->scene.storage.arena.base;
    bool retained = true;
    for (u32 frame = 0; frame < 64; ++frame)
        retained &= application_text_form_synchronize() && g_app.text_form == form &&
                    seen.builds == locked_builds && form->scene.revision == locked_scene_revision &&
                    form->scene.widgets.layout.revision == locked_layout_revision &&
                    form->scene.storage.arena.base == scene_storage;
    check(retained && seen.saves == initial_saves,
          "locked async form: 64 pending updates retain owner geometry and text without new callbacks or "
          "publication");
    async_progress.status = utf8_literal("Could not finish. Retry to continue.");
    async_progress.accept = utf8_literal("Retry");
    async_progress.can_accept = true;
    ++async_progress.revision;
    check(application_text_form_synchronize() && g_app.text_form == form &&
              text_form_focused(form->scene) == TextFormElement::accept && !form->can_cancel &&
              !form->dismissible && application_text_form_input(UiInputKind::key_down, VK_SPACE, false),
          "locked async form: retry failure enables only Accept and preserves nondismissible ownership");
    async_progress.can_accept = false;
    ++async_progress.revision;
    check(application_text_form_synchronize() && !form->keyboard.pressed.manager,
          "locked async form: progress disables an armed Retry button before release");
    async_progress.can_accept = true;
    ++async_progress.revision;
    check(application_text_form_synchronize() &&
              application_text_form_input(UiInputKind::key_up, VK_SPACE, false) &&
              application_text_form_input(UiInputKind::key_down, VK_RETURN, true) &&
              seen.saves == initial_saves && g_app.text_form == form,
          "locked async form: old Space release and held repeated Return do not activate newly reenabled "
          "Retry");
    tail(VK_RETURN);
    check(application_text_form_input(UiInputKind::key_down, VK_RETURN, false) && g_app.text_form == form &&
              seen.saves == initial_saves + 1 &&
              application_text_form_input(UiInputKind::key_down, VK_RETURN, true) &&
              seen.saves == initial_saves + 1,
          "locked async form: fresh Retry activates once and held Return cannot duplicate pending work");
    tail(VK_RETURN);
    check(application_text_form_input(UiInputKind::key_down, VK_RETURN, false) &&
              seen.saves == initial_saves + 2 && g_app.text_form == form,
          "locked async form: deliberate Retry after a fresh key-up can activate again");
    tail(VK_RETURN);
    seen.owner_valid = false;
    check(application_text_form_synchronize() && !g_app.text_form && closed_owners == initial_closed + 1,
          "locked async form: stale callback validity retires nondismissible owner unconditionally");
    seen.owner_valid = true;
    check(application_text_form_open(asynchronous) && g_app.text_form,
          "locked async form: explicit cancellation fixture opens with nondismissible progress");
    application_text_form_cancel_owner(valid);
    check(!g_app.text_form && closed_owners == initial_closed + 2,
          "locked async form: explicit owner cancellation bypasses user dismissal flags");
    check(application_text_form_open(asynchronous), "locked async form: programmatic close fixture opens");
    application_text_form_close(false, false);
    check(!g_app.text_form && closed_owners == initial_closed + 3,
          "locked async form: programmatic close always retires exact committed owner");
    check(application_text_form_open(asynchronous), "locked async form: completion fixture opens");
    async_progress.complete = true;
    ++async_progress.revision;
    check(application_text_form_synchronize() && !g_app.text_form && closed_owners == initial_closed + 4 &&
              seen.editor_calls == initial_editor_calls && seen.store_calls == initial_store_calls,
          "locked async form: completion retires owner and entire confirmation lifecycle performs zero "
          "editor or TSF work");
}
void startup_recovery_lifecycle() noexcept {
    auto recovery = request();
    recovery.login_owner = true;
    recovery.progress = progress;
    recovery.closed = closed;
    async_progress = {utf8_literal("Could not finish removing local account data."),
                      utf8_literal("Retry"),
                      40,
                      false,
                      true,
                      false,
                      false,
                      false};
    const u32 editor_calls = seen.editor_calls, store_calls = seen.store_calls;
    const u32 saves = seen.saves, closures = closed_owners;
    const u32 prepares = seen.login_prepares, releases = seen.login_releases,
              login_refreshes = seen.login_refreshes;
    check(!application_text_form_open(recovery),
          "startup recovery: login-owned confirmation cannot enter ordinary account context");
    auto* account_ui = g_app.account_ui;
    g_app.login.visible = true;
    check(!application_text_form_open(recovery),
          "startup recovery: admission rejects a loaded account behind login");
    g_app.account_ui = nullptr;
    auto ordinary = recovery;
    ordinary.confirmation_only = false;
    check(!application_text_form_open(ordinary) && seen.editor_calls == editor_calls,
          "startup recovery: login admission cannot create an ordinary editor or credential form");
    ordinary = recovery;
    ordinary.login_owner = false;
    check(!application_text_form_open(ordinary),
          "startup recovery: unmarked forms remain excluded from login without account models");
    check(application_text_form_open(recovery) && g_app.text_form && !g_app.account_ui &&
              g_app.text_form->request.login_owner && !g_app.text_form->edit.storage.data &&
              !g_app.text_form->store.implementation && !g_app.text_form->editable &&
              !g_app.text_form->can_cancel && !g_app.text_form->dismissible &&
              text_form_focused(g_app.text_form->scene) == TextFormElement::accept &&
              seen.login_prepares == prepares + 1 && seen.login_refreshes == login_refreshes + 1,
          "startup recovery: exact login owner opens visible Retry confirmation without account editor or "
          "native store");
    auto* form = g_app.text_form;
    if (form) {
        const auto* measure = form->measure;
        MEMORY_BASIC_INFORMATION cache_region{};
        check(measure && measure->backend == renderer_text_measure_backend &&
                  measure->backend_context == &g_app.renderer &&
                  measure->storage.count == kTextFormElements * kUiTextMeasureCacheWays &&
                  VirtualQuery(measure, &cache_region, sizeof(cache_region)) == sizeof(cache_region) &&
                  cache_region.AllocationBase == form && form->scene.measure == measure,
              "startup recovery: bounded renderer font cache shares modal allocation and borrows no "
              "account storage");
        LRESULT result = 0;
        check(application_text_form_input(UiInputKind::key_down, VK_ESCAPE, false) &&
                  application_text_form_input(UiInputKind::key_up, VK_ESCAPE, false) &&
                  application_text_form_window_message(WM_LBUTTONDOWN, 0, MAKELPARAM(1, 1), &result) &&
                  application_text_form_window_message(WM_LBUTTONUP, 0, MAKELPARAM(1, 1), &result) &&
                  application_text_form_window_message(WM_KILLFOCUS, 0, 0, &result) &&
                  g_app.text_form == form && !g_app.account_ui && closed_owners == closures,
              "startup recovery: Escape outside click and blur preserve the nondismissible login owner");
        const u32 builds = seen.builds, measurements = measure->backend_calls;
        bool stable = true;
        for (u32 frame = 0; frame < 64; ++frame)
            stable &= application_text_form_synchronize() && g_app.text_form == form && !g_app.account_ui &&
                      seen.builds == builds && measure->backend_calls == measurements;
        check(stable, "startup recovery: 64 unchanged frames reuse bounded font cache without account "
                      "allocation or publication");
        const u32 font_generation = measure->generation, font_calls = measure->backend_calls;
        const auto font_focus = text_form_focused(form->scene);
        const u16 font_token = form->scene.measure_revision;
        const u32 builds_before_font = seen.builds;
        application_text_form_theme_changed();
        check(!form->scene.layout_signature && measure->generation != font_generation &&
                  form->scene.measure_revision == font_token && seen.builds == builds_before_font &&
                  seen.editor_calls == editor_calls && seen.store_calls == store_calls &&
                  text_form_focused(form->scene) == font_focus,
              "startup recovery: committed font change invalidates owned cache even when the compact "
              "font token repeats, without eager rendering or editor/store/focus reset");
        check(application_text_form_synchronize() && form->scene.layout_signature &&
                  form->scene.measure_revision == font_token && measure->backend_calls > font_calls &&
                  text_form_focused(form->scene) == font_focus && !g_app.account_ui,
              "startup recovery: dirty same-token projection measures fresh native labels without "
              "allocating account storage or resetting focus");
        const u32 second_generation = measure->generation, second_calls = measure->backend_calls;
        application_text_form_theme_changed();
        check(measure->generation != second_generation && application_text_form_synchronize() &&
                  measure->backend_calls > second_calls && form->scene.measure_revision == font_token,
              "startup recovery: every actual font change invalidates private measurements, not only "
              "the first observation of a compact token");
        seen.save_ok = false;
        check(application_text_form_input(UiInputKind::key_down, VK_RETURN, false) &&
                  application_text_form_input(UiInputKind::key_down, VK_RETURN, true) &&
                  seen.saves == saves + 1 && g_app.text_form == form && !g_app.account_ui,
              "startup recovery: deliberate Retry invokes exact owner once while repeated Return remains "
              "blocked");
        tail(VK_RETURN);
        seen.owner_valid = false;
        check(application_text_form_synchronize() && !g_app.text_form && closed_owners == closures + 1 &&
                  !g_app.account_ui && !g_app.renderer.text_form,
              "startup recovery: stale login owner retires confirmation and renderer without entering auth "
              "admission");
        seen.owner_valid = true;
    }
    application_text_form_close(false, false);
    check(seen.login_releases == releases + 1,
          "startup recovery: owned login rendering storage releases after form renderer detaches");
    seen.fail_build = true;
    check(!application_text_form_open(recovery) && !g_app.text_form && !g_app.renderer.text_form &&
              !g_app.account_ui && seen.login_prepares == prepares + 2 &&
              seen.login_releases == releases + 2,
          "startup recovery: renderer publication failure rolls back modal and login text owner without "
          "account allocation");
    seen.fail_build = false;
    g_app.login.visible = false;
    g_app.account_ui = account_ui;
    check(seen.editor_calls == editor_calls && seen.store_calls == store_calls,
          "startup recovery: complete login confirmation lifecycle performs zero editor or TSF work");
}
void run() noexcept {
    struct Cleanup {
        ~Cleanup() noexcept {
            application_text_form_close(false, false);
            if (g_app.account_ui)
                VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
            g_app.account_ui = nullptr;
            g_app.context_focus = nullptr;
        }
    } cleanup;
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.account_ui != nullptr,
          "confirmation: owned zeroed account UI fixture allocates without account or window");
    if (!g_app.account_ui)
        return;
    check(ui_text_measure_cache_initialize(&g_app.account_ui->text_measure_cache, {measure_records, 32},
                                           measure_label, nullptr),
          "confirmation: bounded native label measurement cache initializes independently of editor "
          "measurement");
    g_app.renderer_ready = true;
    g_app.renderer.width = 1000;
    g_app.renderer.height = 700;
    g_app.renderer.dpi = 96;
    g_app.renderer.text_measure_revision = 37;
    g_app.window = nullptr;
    g_app.keyboard_focus_visible = true;
    seen.owner_valid = true;
    seen.save_ok = true;
    auto normal = request();
    normal.confirmation_only = false;
    normal.initial = {};
    check(!application_text_form_open(normal) && seen.editor_calls == 2 && !seen.store_calls &&
              !g_app.text_form,
          "confirmation: normal-form negative control reaches instrumented editor initialization and "
          "releases its failed owner");
    seen.editor_calls = 0;
    check(application_text_form_open(request()) && g_app.text_form &&
              text_form_focused(g_app.text_form->scene) == TextFormElement::cancel &&
              !text_form_node(g_app.text_form->scene, TextFormElement::edit) &&
              !text_form_node(g_app.text_form->scene, TextFormElement::label) &&
              !g_app.text_form->edit.storage.data && !g_app.text_form->edit.history &&
              !g_app.text_form->store.implementation && !application_text_form_next_wake() &&
              !seen.editor_calls && !seen.store_calls && !seen.measurements,
          "confirmation: real controller/scene/surface opens Cancel-focused without editor, history, TSF, "
          "measurement or caret wake");
    if (!g_app.text_form)
        return;
    footprint(*g_app.text_form);
    const auto arena = g_app.text_form->scene.storage.arena.base;
    const u32 revision = g_app.text_form->scene.revision, builds = seen.builds;
    MSG key{};
    key.hwnd = nullptr;
    key.message = WM_KEYDOWN;
    key.wParam = 'X';
    check(!application_text_form_filter_message(key) &&
              application_text_form_input(UiInputKind::character, 'x', false) &&
              application_text_form_synchronize() && g_app.text_form->scene.storage.arena.base == arena &&
              g_app.text_form->scene.revision == revision && seen.builds == builds,
          "confirmation: text/filter input and idle synchronization retain scene without touching edit or "
          "native store");
    application_text_form_tick(~u64{0});
    check(!application_text_form_next_wake() && !seen.editor_calls && !seen.store_calls &&
              !seen.measurements,
          "confirmation: unbounded tick has no editor blink or native text work");
    void* owner = g_app.text_form;
    check(application_text_form_input(UiInputKind::key_down, VK_RETURN, false) && !g_app.text_form &&
              !seen.saves && freed(owner),
          "confirmation: default Enter cancels safely and releases both retained scene and modal owner");
    tail(VK_RETURN);
    seen.save_ok = false;
    check(application_text_form_open(request()) &&
              application_text_form_input(UiInputKind::key_down, VK_TAB, false) && g_app.text_form &&
              text_form_focused(g_app.text_form->scene) == TextFormElement::accept &&
              application_text_form_input(UiInputKind::key_down, VK_RETURN, false) && g_app.text_form &&
              seen.saves == 1 && g_app.text_form->status.bytes &&
              application_text_form_input(UiInputKind::key_down, VK_RETURN, true) && seen.saves == 1,
          "confirmation: Tab reaches Accept, failed semantic action retains error and repeated Enter "
          "cannot duplicate submission");
    tail(VK_RETURN);
    seen.save_ok = true;
    check(application_text_form_input(UiInputKind::key_down, VK_RETURN, false) && !g_app.text_form &&
              seen.saves == 2,
          "confirmation: deliberate Retry accepts empty semantic callback and closes without editor "
          "resources");
    tail(VK_RETURN);
    check(application_text_form_open(request()), "confirmation: pointer-button fixture reopens");
    if (g_app.text_form) {
        const auto* node = text_form_node(g_app.text_form->scene, TextFormElement::accept);
        LRESULT result = 0;
        const LPARAM point =
            node ? MAKELPARAM(static_cast<short>((node->rectangle.left + node->rectangle.right) * .5f),
                              static_cast<short>((node->rectangle.top + node->rectangle.bottom) * .5f))
                 : 0;
        check(node && application_text_form_window_message(WM_LBUTTONDOWN, MK_LBUTTON, point, &result) &&
                  application_text_form_window_message(WM_LBUTTONUP, 0, point, &result) &&
                  !g_app.text_form && seen.saves == 3,
              "confirmation: owned retained Accept button activates through actual mouse down/up "
              "controller path");
    }
    check(application_text_form_open(request()), "confirmation: owner-invalidation fixture reopens");
    seen.owner_valid = false;
    check(application_text_form_synchronize() && !g_app.text_form && seen.saves == 3,
          "confirmation: stale semantic owner dismisses without invoking acceptance");
    seen.owner_valid = true;
    seen.fail_build = true;
    check(!application_text_form_open(request()) && !g_app.text_form && freed(seen.last_form) &&
              !g_app.renderer.text_form,
          "confirmation: renderer publication failure rolls back owner and renderer binding");
    check(!seen.editor_calls && !seen.store_calls && !seen.measurements,
          "confirmation: complete lifecycle performs zero editor/history/TSF calls and zero editor "
          "measurements");
    ordinary_projection();
    seen.fail_build = false;
    seen.save_ok = false;
    async_progress = {utf8_literal("Creating thread…"), utf8_literal("Create"), 1, true, true, false};
    auto asynchronous = request();
    asynchronous.progress = progress;
    asynchronous.closed = closed;
    const u32 saves_before = seen.saves;
    check(
        application_text_form_open(asynchronous) &&
            application_text_form_input(UiInputKind::key_down, VK_TAB, false) &&
            application_text_form_input(UiInputKind::key_down, VK_RETURN, false) && g_app.text_form &&
            seen.saves == saves_before + 1 && utf8_equal(g_app.text_form->status, async_progress.status),
        "async form: queued save retains exact owner and explicit host status without generic retry text");
    tail(VK_RETURN);
    async_progress.can_accept = false;
    ++async_progress.revision;
    check(application_text_form_synchronize() && g_app.text_form &&
              text_form_focused(g_app.text_form->scene) == TextFormElement::cancel &&
              !text_form_focus(&g_app.text_form->scene, TextFormElement::accept),
          "async form: pending progress disables repeated acceptance and preserves Cancel");
    const u32 stable_builds = seen.builds, stable_saves = seen.saves;
    bool stable = true;
    for (u32 frame = 0; frame < 64; ++frame)
        stable = application_text_form_synchronize() && stable;
    check(stable && seen.builds == stable_builds && seen.saves == stable_saves && !closed_owners,
          "async form: 64 unchanged progress frames rebuild no text and issue no duplicate save");
    async_progress.status = utf8_literal("Check whether the thread was created before retrying.");
    async_progress.accept = utf8_literal("Refresh");
    async_progress.can_accept = true;
    ++async_progress.revision;
    check(application_text_form_synchronize() && g_app.text_form &&
              utf8_equal(g_app.text_form->scene.content.accept, utf8_literal("Refresh")),
          "async form: uncertain outcome projects explicit reconciliation instead of blind Retry");
    async_progress.complete = true;
    ++async_progress.revision;
    check(application_text_form_synchronize() && !g_app.text_form && closed_owners == 1,
          "async form: completed projection releases form and closes exact owner once");
    locked_progress_lifecycle();
    startup_recovery_lifecycle();
}
} // namespace maicord::confirmation_runtime_test

namespace maicord {
bool renderer_prepare_login_form_text(D3D11Renderer*) noexcept {
    ++confirmation_runtime_test::seen.login_prepares;
    return true;
}
void renderer_release_login_form_text(D3D11Renderer* renderer) noexcept {
    ++confirmation_runtime_test::seen.login_releases;
    confirmation_runtime_test::check(
        renderer && !renderer->text_form,
        "startup recovery: login render resources release after form detaches");
}
bool renderer_text_measure_backend(void*, const UiTextMeasureRequest& request,
                                   UiTextMeasureValue* output) noexcept {
    return confirmation_runtime_test::measure_label(nullptr, request, output);
}
// Every editor/native-store boundary is instrumented and fails if entered.
// A normal-form negative control proves these counters observe allocation work.
bool secure_edit_initialize(SecureEdit*, u32, bool, SecureEditPolicy) noexcept {
    ++confirmation_runtime_test::seen.editor_calls;
    return false;
}
void secure_edit_release(SecureEdit*) noexcept {
    ++confirmation_runtime_test::seen.editor_calls;
}
bool secure_edit_enable_history(SecureEdit*, u32) noexcept {
    ++confirmation_runtime_test::seen.editor_calls;
    return false;
}
void secure_edit_break_history_run(SecureEdit*) noexcept {
    ++confirmation_runtime_test::seen.editor_calls;
}
void secure_edit_cancel_composition(SecureEdit*) noexcept {
    ++confirmation_runtime_test::seen.editor_calls;
}
ConstSlice<u8> secure_edit_view(const SecureEdit& edit) noexcept {
    ++confirmation_runtime_test::seen.editor_calls;
    return confirmation_runtime_test::seen.ordinary_projection
               ? ConstSlice<u8>{edit.storage.data, edit.storage.bytes}
               : ConstSlice<u8>{};
}
SecureEditResult secure_edit_render_text(const SecureEdit&, Slice<char>, u32*) noexcept {
    ++confirmation_runtime_test::seen.editor_calls;
    ++confirmation_runtime_test::seen.render_copies;
    return SecureEditResult::invalid_input;
}
SecureEditResult secure_edit_utf16_selection(const SecureEdit& edit,
                                             SecureEditUtf16Selection* output) noexcept {
    ++confirmation_runtime_test::seen.editor_calls;
    if (!confirmation_runtime_test::seen.ordinary_projection || !output || !edit.storage.data ||
        !edit.storage.locked || !edit.storage.capacity)
        return SecureEditResult::invalid_input;
    const Utf8View text{reinterpret_cast<const char*>(edit.storage.data), edit.storage.bytes};
    return utf8_utf16_position_for_codepoint(text, edit.anchor, &output->anchor) &&
                   utf8_utf16_position_for_codepoint(text, edit.caret, &output->caret)
               ? SecureEditResult::unchanged
               : SecureEditResult::invalid_utf8;
}
#define EDIT_RESULT(name, args)                                                                            \
    SecureEditResult name args noexcept {                                                                  \
        ++confirmation_runtime_test::seen.editor_calls;                                                    \
        return SecureEditResult::invalid_input;                                                            \
    }
EDIT_RESULT(secure_edit_assign, (SecureEdit*, Utf8View))
EDIT_RESULT(secure_edit_set_utf16_selection, (SecureEdit*, SecureEditUtf16Selection))
EDIT_RESULT(secure_edit_insert_utf16, (SecureEdit*, u16, u64))
EDIT_RESULT(secure_edit_insert_utf8, (SecureEdit*, Utf8View))
EDIT_RESULT(secure_edit_backspace, (SecureEdit*, u64))
EDIT_RESULT(secure_edit_delete_forward, (SecureEdit*, u64))
EDIT_RESULT(secure_edit_move, (SecureEdit*, SecureEditMove, bool))
EDIT_RESULT(secure_edit_select_all, (SecureEdit*))
EDIT_RESULT(secure_edit_select_word, (SecureEdit*, u32))
EDIT_RESULT(secure_edit_select_line, (SecureEdit*, u32))
EDIT_RESULT(secure_edit_undo, (SecureEdit*))
EDIT_RESULT(secure_edit_redo, (SecureEdit*))
#undef EDIT_RESULT
bool secure_text_store_initialize(SecureTextStore*, SecureTextStoreConfig) noexcept {
    ++confirmation_runtime_test::seen.store_calls;
    return false;
}
bool secure_text_store_bind(SecureTextStore*, SecureEdit*, SecureTextInputScope) noexcept {
    ++confirmation_runtime_test::seen.store_calls;
    return false;
}
bool secure_text_store_attach(SecureTextStore*) noexcept {
    ++confirmation_runtime_test::seen.store_calls;
    return false;
}
bool secure_text_store_composition(const SecureTextStore&, SecureTextComposition*) noexcept {
    ++confirmation_runtime_test::seen.store_calls;
    return false;
}
bool secure_text_store_filter_message(SecureTextStore*, const MSG&) noexcept {
    ++confirmation_runtime_test::seen.store_calls;
    return false;
}
void secure_text_store_release(SecureTextStore*) noexcept {
    ++confirmation_runtime_test::seen.store_calls;
}
void secure_text_store_unbind(SecureTextStore*) noexcept {
    ++confirmation_runtime_test::seen.store_calls;
}
void secure_text_store_set_window_focus(SecureTextStore*, bool) noexcept {
    ++confirmation_runtime_test::seen.store_calls;
}
void secure_text_store_synchronize(SecureTextStore*) noexcept {
    ++confirmation_runtime_test::seen.store_calls;
}
void secure_text_store_notify_layout(SecureTextStore*) noexcept {
    ++confirmation_runtime_test::seen.store_calls;
}
u64 text_edit_signature(Utf8View) noexcept {
    return 0;
}
bool text_edit_layout_measure(TextEditLayoutOwner* owner, const TextEditLayoutInput& input,
                                const TextSystem&) noexcept {
    ++confirmation_runtime_test::seen.measurements;
    if (!confirmation_runtime_test::seen.ordinary_projection)
        return false;
    owner->origin_x = input.origin_x;
    owner->origin_y = input.viewport_top;
    owner->width = input.width;
    owner->height = input.height;
    owner->layout_height = input.height;
    owner->active = true;
    return true;
}
bool text_edit_layout_hit_test(const TextEditLayoutOwner&, float, float, u32*) noexcept {
    ++confirmation_runtime_test::seen.measurements;
    return false;
}
bool text_edit_layout_point_geometry(const TextEditLayoutOwner&, float, float, bool, u32*,
                                       bool*) noexcept {
    ++confirmation_runtime_test::seen.measurements;
    return false;
}
bool text_edit_layout_move_vertical(const TextEditLayoutOwner&, bool, u32*) noexcept {
    ++confirmation_runtime_test::seen.measurements;
    return false;
}
bool text_edit_layout_range_geometry(const TextEditLayoutOwner&, u32, u32,
                                       Slice<TextEditMetric>, u32*) noexcept {
    ++confirmation_runtime_test::seen.measurements;
    return false;
}
bool text_edit_layout_scroll(TextEditLayoutOwner*, float) noexcept {
    ++confirmation_runtime_test::seen.measurements;
    return false;
}
void text_edit_layout_invalidate(TextEditLayoutOwner* owner) noexcept {
    owner->active = false;
}
void text_edit_layout_release(TextEditLayoutOwner* owner) noexcept {
    *owner = {};
}
ClipboardTextResult clipboard_read_utf8(HWND, Slice<char>) noexcept {
    ++confirmation_runtime_test::seen.editor_calls;
    return {0, ClipboardTextError::unavailable};
}
bool clipboard_write_utf8(HWND, Utf8View) noexcept {
    ++confirmation_runtime_test::seen.editor_calls;
    return false;
}
void renderer_bind_text_form(D3D11Renderer* renderer, TextFormSurface* surface) noexcept {
    renderer->text_form = surface;
    if (surface)
        confirmation_runtime_test::seen.last_form = app_detail::g_app.text_form;
}
bool renderer_rebuild_content_text(D3D11Renderer* renderer, const Theme&) noexcept {
    ++confirmation_runtime_test::seen.builds;
    return !confirmation_runtime_test::seen.fail_build && renderer->text_form &&
           text_form_surface_update(renderer->text_form, TextSystem{});
}
namespace app_detail {
bool application_refresh_login_view() noexcept {
    ++confirmation_runtime_test::seen.login_refreshes;
    confirmation_runtime_test::check(
        g_app.login.visible && !g_app.account_ui && g_app.text_form && g_app.text_form->request.login_owner,
        "startup recovery: login display refresh projects only the admitted login-owned confirmation");
    return true;
}
UiRect application_viewport_dips() noexcept {
    return {0, 0, static_cast<float>(g_app.renderer.width), static_cast<float>(g_app.renderer.height)};
}
bool refresh_account_text(bool) noexcept {
    return true;
}
void application_composer_text_input_suspend() noexcept {
}
void end_message_text_selection() noexcept {
}
void end_message_composer_pointer_selection() noexcept {
}
void end_scrollbar_drag() noexcept {
}
void end_pane_resize() noexcept {
}
bool stop_local_typing(u64) noexcept {
    return true;
}
void reset_message_composer_caret_timing(bool) noexcept {
}
void set_popup_owner(PopupOwnerKind owner) noexcept {
    g_app.popup_owner_kind = owner;
}
} // namespace app_detail
} // namespace maicord
namespace maicord::link_trust_runtime_test {
void run(void (*check)(bool, const char*) noexcept, UiTextMeasureBackend measure) noexcept;
}
extern "C" __declspec(noreturn) void WINAPI maicord_text_form_confirmation_entry() noexcept {
    maicord::confirmation_runtime_test::run();
    maicord::link_trust_runtime_test::run(maicord::confirmation_runtime_test::check,
                                          maicord::confirmation_runtime_test::measure_label);
    const bool failed = maicord::confirmation_runtime_test::seen.failures != 0;
    constexpr char pass[] = "PASS confirmation runtime\n", fail[] = "FAIL confirmation runtime\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}

namespace maicord::app_detail {
// The post parent is absent in this generic confirmation fixture.
bool application_forum_form_active() noexcept {
    return false;
}
bool application_forum_post_current(const ApplicationForumPostTicket&) noexcept {
    return false;
}
bool application_forum_form_suspend_child(const ApplicationForumPostTicket&) noexcept {
    return false;
}
void application_forum_form_resume_child(const ApplicationForumPostTicket&) noexcept {
}
} // namespace maicord::app_detail
