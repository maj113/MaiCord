// Shared input fixtures for the isolated application runtime tests. Include
// inside the test namespace after its check(), modifiers() and the
// scoped_shortcuts, scoped_shortcut_allowed, settings, theme_editor and
// underlying_input observers.
void key(u32 code, bool control = false) noexcept {
    modifiers(control);
    route_application_input(UiInputKind::key_down, code);
    route_application_input(UiInputKind::key_up, code);
    modifiers();
}
// The production router is real. This bounded observer replaces only scoped
// read admission; no transport or read-state controller is created here.
void scoped_shortcut_routing() noexcept {
    scoped_shortcuts = 0;
    scoped_shortcut_allowed = true;
    settings = theme_editor = false;
    underlying_input = 0;
    modifiers(false, false, true);
    check(route_application_input(UiInputKind::key_down, VK_ESCAPE) && scoped_shortcuts == 1 &&
              !underlying_input,
          "Scoped input: fresh Shift Escape admits once before ordinary content keys");
    settings = true;
    check(route_application_input(UiInputKind::key_down, VK_ESCAPE, true) &&
              route_application_input(UiInputKind::character, VK_ESCAPE) &&
              route_application_input(UiInputKind::key_up, VK_ESCAPE) && scoped_shortcuts == 1 &&
              !underlying_input,
          "Scoped input: accepted key tail survives foreground replacement without a second admission");
    check(route_application_input(UiInputKind::key_down, VK_ESCAPE) && scoped_shortcuts == 1,
          "Scoped input: settings retains fresh Shift Escape before scope admission");
    settings = false;
    theme_editor = true;
    check(route_application_input(UiInputKind::key_down, VK_ESCAPE) && scoped_shortcuts == 1,
          "Scoped input: theme editor retains Shift Escape");
    theme_editor = false;
    TextFormScene modal_scene{};
    TextFormSurface modal_surface{};
    modal_scene.active = true;
    modal_surface.scene = &modal_scene;
    const auto previous_form = g_app.renderer.text_form;
    g_app.renderer.text_form = &modal_surface;
    check(route_application_input(UiInputKind::key_down, VK_ESCAPE) && scoped_shortcuts == 1,
          "Scoped input: generic form layer retains Shift Escape before content admission");
    g_app.renderer.text_form = previous_form;
    g_app.message_edit.editor_active = true;
    route_application_input(UiInputKind::key_down, VK_ESCAPE);
    check(scoped_shortcuts == 1, "Scoped input: active message editor keeps Escape semantics");
    g_app.message_edit.editor_active = false;
    const auto reply = g_app.account_ui->message_composer_controller.reply_message_id;
    g_app.account_ui->message_composer_controller.reply_message_id = 777;
    route_application_input(UiInputKind::key_down, VK_ESCAPE);
    check(scoped_shortcuts == 1, "Scoped input: active reply keeps Escape semantics");
    g_app.account_ui->message_composer_controller.reply_message_id = reply;
    modifiers();
    route_application_input(UiInputKind::key_down, VK_ESCAPE);
    modifiers(true, false, true);
    route_application_input(UiInputKind::key_down, VK_ESCAPE);
    modifiers(false, true, true);
    route_application_input(UiInputKind::key_down, VK_ESCAPE);
    modifiers(false, false, true);
    route_application_input(UiInputKind::key_down, VK_ESCAPE, true);
    check(scoped_shortcuts == 1, "Scoped input: unmodified, Ctrl, Alt and repeated Escape do not admit");
    scoped_shortcut_allowed = false;
    route_application_input(UiInputKind::key_down, VK_ESCAPE);
    check(scoped_shortcuts == 2, "Scoped input: refused scope remains a failed admission boundary");
    scoped_shortcut_allowed = true;
    underlying_input = 0;
    check(route_application_input(UiInputKind::key_down, VK_ESCAPE) && scoped_shortcuts == 3 &&
              route_application_input(UiInputKind::key_down, VK_ESCAPE, true) &&
              route_application_input(UiInputKind::key_up, VK_ESCAPE) && scoped_shortcuts == 3 &&
              !underlying_input,
          "Scoped input: explicit new gesture after refusal admits once and consumes held repeat/release");
    modifiers();
}
