#include "maicord/ui/login_scene.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/widget_builder.hpp"

namespace maicord {
namespace {

constexpr u32 login_element_id(LoginElement element) noexcept {
    return static_cast<u32>(element) + 1;
}

bool focusable(LoginControl control) noexcept {
    return control == LoginControl::none || control == LoginControl::login ||
           control == LoginControl::password || control == LoginControl::submit ||
           control == LoginControl::forgot_password || control == LoginControl::back ||
           control == LoginControl::qr || control == LoginControl::mfa_totp ||
           control == LoginControl::mfa_sms || control == LoginControl::mfa_backup ||
           control == LoginControl::mfa_webauthn || control == LoginControl::phone_code ||
           control == LoginControl::phone_resend || control == LoginControl::browser_sign_in;
}

u8 capability_for_control(LoginControl control) noexcept {
    switch (control) {
    case LoginControl::mfa_totp:
        return login_mfa_capability_totp;
    case LoginControl::mfa_sms:
        return login_mfa_capability_sms;
    case LoginControl::mfa_backup:
        return login_mfa_capability_backup;
    case LoginControl::mfa_webauthn:
        return login_mfa_capability_webauthn;
    default:
        return login_mfa_capability_none;
    }
}

LoginElement element_for_control(LoginControl control) noexcept {
    switch (control) {
    case LoginControl::login:
        return LoginElement::login_field;
    case LoginControl::password:
    case LoginControl::phone_code:
        return LoginElement::password_field;
    case LoginControl::submit:
        return LoginElement::submit;
    case LoginControl::forgot_password:
    case LoginControl::back:
        return LoginElement::forgot_password;
    case LoginControl::qr:
        return LoginElement::qr;
    case LoginControl::browser_sign_in:
        return LoginElement::browser_sign_in;
    case LoginControl::mfa_totp:
        return LoginElement::mfa_totp;
    case LoginControl::mfa_sms:
    case LoginControl::phone_resend:
        return LoginElement::mfa_sms;
    case LoginControl::mfa_backup:
        return LoginElement::mfa_backup;
    case LoginControl::mfa_webauthn:
        return LoginElement::mfa_webauthn;
    case LoginControl::none:
        return LoginElement::count;
    }
    return LoginElement::count;
}

LoginControl control_for_element(LoginElement element) noexcept {
    switch (element) {
    case LoginElement::login_field:
        return LoginControl::login;
    case LoginElement::password_field:
        return LoginControl::password;
    case LoginElement::submit:
        return LoginControl::submit;
    case LoginElement::forgot_password:
        return LoginControl::forgot_password;
    case LoginElement::qr:
        return LoginControl::qr;
    case LoginElement::browser_sign_in:
        return LoginControl::browser_sign_in;
    case LoginElement::mfa_totp:
        return LoginControl::mfa_totp;
    case LoginElement::mfa_sms:
        return LoginControl::mfa_sms;
    case LoginElement::mfa_backup:
        return LoginControl::mfa_backup;
    case LoginElement::mfa_webauthn:
        return LoginControl::mfa_webauthn;
    default:
        return LoginControl::none;
    }
}

// Forgot/Back share one widget, as do the password/phone-code and
// SMS/resend pairs: the control a widget means in the current mode.
LoginControl semantic_control(const LoginScene& scene, LoginControl control) noexcept {
    if (control == LoginControl::forgot_password &&
        (scene.back_only || scene.phone_mode || scene.recovery_mode || scene.reset_mode))
        return LoginControl::back;
    if (scene.phone_mode && control == LoginControl::password)
        return LoginControl::phone_code;
    if (scene.phone_mode && control == LoginControl::mfa_sms)
        return LoginControl::phone_resend;
    return control;
}

Utf8View element_name(LoginElement element) noexcept {
    switch (element) {
    case LoginElement::root:
        return utf8_literal("Login");
    case LoginElement::card:
        return utf8_literal("Card");
    case LoginElement::content_row:
        return utf8_literal("ContentRow");
    case LoginElement::form:
        return utf8_literal("Form");
    case LoginElement::heading:
        return utf8_literal("Heading");
    case LoginElement::subtitle:
        return utf8_literal("Subtitle");
    case LoginElement::login_label:
        return utf8_literal("LoginLabel");
    case LoginElement::login_field:
        return utf8_literal("LoginField");
    case LoginElement::password_label:
        return utf8_literal("PasswordLabel");
    case LoginElement::password_field:
        return utf8_literal("PasswordField");
    case LoginElement::auxiliary:
        return utf8_literal("Auxiliary");
    case LoginElement::forgot_password:
        return utf8_literal("ForgotPassword");
    case LoginElement::mfa_totp:
        return utf8_literal("MfaTotp");
    case LoginElement::mfa_sms:
        return utf8_literal("MfaSms");
    case LoginElement::mfa_backup:
        return utf8_literal("MfaBackup");
    case LoginElement::mfa_webauthn:
        return utf8_literal("MfaWebAuthn");
    case LoginElement::submit:
        return utf8_literal("Submit");
    case LoginElement::status:
        return utf8_literal("Status");
    case LoginElement::status_message:
        return utf8_literal("StatusMessage");
    case LoginElement::status_account:
        return utf8_literal("StatusAccount");
    case LoginElement::qr_column:
        return utf8_literal("QrColumn");
    case LoginElement::qr:
        return utf8_literal("QrCode");
    case LoginElement::qr_title:
        return utf8_literal("QrTitle");
    case LoginElement::qr_subtitle:
        return utf8_literal("QrSubtitle");
    case LoginElement::browser_sign_in:
        return utf8_literal("BrowserSignIn");
    case LoginElement::count:
        return Utf8View{};
    }
    return Utf8View{};
}

// UiWidgetBuilder stops at its first error, so these report nothing: a failed add leaves
// its element handle invalid and build_login_widgets checks widgets.ok() once.
void add_widget(LoginScene* scene, UiWidgetBuilder& widgets, LoginElement element, UiWidgetHandle parent,
                UiWidgetKind kind, u16 widget_flags, const UiLayoutStyle& style, UiSize intrinsic = UiSize{},
                float baseline = 0.0f) noexcept {
    const UiWidgetResult result = widgets.widget(parent, {login_element_id(element), element_name(element)}, kind,
                                                 widget_flags, style, intrinsic, baseline);
    if (result.ok())
        scene->elements[static_cast<u32>(element)] = result.handle;
}

UiLayoutStyle text_style(float height, float margin_top, UiLayoutAlignment alignment) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = alignment == UiLayoutAlignment::stretch ? ui_layout_fill() : ui_layout_content();
    style.height = ui_layout_fixed(height);
    style.margin.top = margin_top;
    style.horizontal_alignment = alignment;
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_text_content);
    return style;
}

void add_text(LoginScene* scene, UiWidgetBuilder& widgets, LoginElement element, UiWidgetHandle parent, float width,
              float height, float margin_top, UiLayoutAlignment alignment) noexcept {
    const UiWidgetResult result =
        widgets.text(parent, {login_element_id(element), element_name(element)},
                     text_style(height, margin_top, alignment), {width, height}, height * 0.8f);
    if (result.ok())
        scene->elements[static_cast<u32>(element)] = result.handle;
}

void add_control(LoginScene* scene, UiWidgetBuilder& widgets, LoginElement element, UiWidgetHandle parent,
                 UiWidgetKind kind, const UiLayoutStyle& style, bool keyboard_focusable = true) noexcept {
    const UiWidgetKey key{login_element_id(element), element_name(element)};
    if (kind != UiWidgetKind::button && kind != UiWidgetKind::text_field) {
        add_widget(scene, widgets, element, parent, kind,
                   static_cast<u16>(ui_widget_enabled | ui_widget_hit_test |
                                    (keyboard_focusable ? ui_widget_focusable : 0)),
                   style);
        return;
    }
    const UiWidgetResult result = kind == UiWidgetKind::button ? widgets.button(parent, key, style, keyboard_focusable)
                                                               : widgets.text_field(parent, key, style, keyboard_focusable);
    if (result.ok())
        scene->elements[static_cast<u32>(element)] = result.handle;
}

void add_auxiliary_link(LoginScene* scene, UiWidgetBuilder& widgets, UiWidgetHandle parent,
                        float margin_top) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fixed(144.0f);
    style.height = ui_layout_fixed(22.0f);
    style.margin.top = margin_top;
    style.vertical_alignment = UiLayoutAlignment::center;
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_text_content);
    add_control(scene, widgets, LoginElement::forgot_password, parent, UiWidgetKind::button, style);
    UiLayoutNode* node = ui_widget_manager_layout_node(
        &scene->widgets, scene->elements[static_cast<u32>(LoginElement::forgot_password)]);
    if (node) {
        node->spec.intrinsic_size = UiSize{144.0f, 22.0f};
        node->spec.intrinsic_baseline = 17.0f;
    }
}

u8 login_layout_variant(bool full_viewport, bool qr_visible, float width, float height,
                        const LoginLayoutMetrics& metrics) noexcept {
    const bool intermediate_width = !full_viewport && width < metrics.intermediate_max_width;
    const bool short_viewport = height < metrics.short_viewport_height;
    return static_cast<u8>((full_viewport ? 1u : 0u) | (qr_visible ? 2u : 0u) |
                           (intermediate_width ? 4u : 0u) | (short_viewport ? 8u : 0u));
}

constexpr bool login_metrics_valid(const LoginLayoutMetrics& metrics) noexcept {
    return metrics.revision != 0 && ui_extent_valid(metrics.full_viewport_max_width) &&
           ui_extent_valid(metrics.qr_min_width) && ui_extent_valid(metrics.intermediate_max_width) &&
           ui_extent_valid(metrics.short_viewport_height) && ui_extent_valid(metrics.wide_card_width) &&
           ui_extent_valid(metrics.compact_card_width) && ui_extent_valid(metrics.card_height) &&
           ui_insets_valid(metrics.outer_margin) && ui_insets_valid(metrics.card_padding) &&
           ui_insets_valid(metrics.full_viewport_padding) && ui_extent_valid(metrics.qr_gap) &&
           ui_extent_valid(metrics.qr_column_width) && ui_extent_valid(metrics.qr_extent) &&
           ui_extent_valid(metrics.input_height) && ui_extent_valid(metrics.submit_height) &&
           metrics.full_viewport_max_width > 0.0f &&
           metrics.qr_min_width > metrics.full_viewport_max_width &&
           metrics.intermediate_max_width > metrics.full_viewport_max_width &&
           metrics.wide_card_width > 0.0f && metrics.compact_card_width > 0.0f &&
           metrics.card_height > 0.0f && metrics.input_height > 0.0f && metrics.submit_height > 0.0f;
}
// The only runtime callers pass the default metrics (tests pass valid copies).
static_assert(login_metrics_valid(kDefaultLoginLayoutMetrics));

bool build_login_widgets(LoginScene* scene, float width, float height,
                         const LoginLayoutMetrics& metrics) noexcept {
    if (!scene->widgets.storage.data) {
        if ((!scene->storage.arena.base &&
             !ui_fixed_widget_storage_initialize(&scene->storage, kMaximumLoginWidgets)) ||
            !ui_widget_manager_initialize(&scene->widgets, scene->storage.widgets,
                                          scene->storage.layout_nodes)) {
            return false;
        }
    } else {
        ui_widget_manager_reset(&scene->widgets);
    }
    for (u32 index = 0; index < static_cast<u32>(LoginElement::count); ++index) {
        scene->elements[index] = ui_widget_invalid_handle();
    }

    UiWidgetBuilder widgets(scene->widgets);

    UiLayoutStyle root_style = ui_layout_style();
    root_style.width = ui_layout_fill();
    root_style.height = ui_layout_fill();
    root_style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children);
    add_widget(scene, widgets, LoginElement::root, ui_widget_invalid_handle(), UiWidgetKind::container,
               ui_widget_enabled, root_style);
    const UiWidgetHandle root = scene->elements[static_cast<u32>(LoginElement::root)];

    const bool short_viewport = height < metrics.short_viewport_height;
    const bool intermediate_width = !scene->full_viewport && width < metrics.intermediate_max_width;
    UiLayoutStyle card_style = ui_layout_style();
    card_style.flow = UiLayoutFlow::horizontal;
    card_style.width =
        scene->full_viewport || intermediate_width
            ? ui_layout_fill()
            : ui_layout_fixed(scene->qr_visible ? metrics.wide_card_width : metrics.compact_card_width);
    card_style.height =
        scene->full_viewport || short_viewport ? ui_layout_fill() : ui_layout_fixed(metrics.card_height);
    card_style.horizontal_alignment = UiLayoutAlignment::center;
    card_style.vertical_alignment = UiLayoutAlignment::center;
    card_style.margin = scene->full_viewport ? UiInsets{} : metrics.outer_margin;
    card_style.padding = scene->full_viewport ? metrics.full_viewport_padding : metrics.card_padding;
    card_style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children);
    add_widget(scene, widgets, LoginElement::card, root, UiWidgetKind::container, ui_widget_enabled, card_style);
    const UiWidgetHandle card = scene->elements[static_cast<u32>(LoginElement::card)];

    UiLayoutStyle row_style = ui_layout_style();
    row_style.flow = UiLayoutFlow::horizontal;
    row_style.width = ui_layout_fill();
    row_style.height = ui_layout_fill();
    row_style.gap = scene->qr_visible ? metrics.qr_gap : 0.0f;
    row_style.horizontal_alignment = UiLayoutAlignment::stretch;
    row_style.vertical_alignment = UiLayoutAlignment::stretch;
    add_widget(scene, widgets, LoginElement::content_row, card, UiWidgetKind::container, ui_widget_enabled,
               row_style);
    const UiWidgetHandle row = scene->elements[static_cast<u32>(LoginElement::content_row)];

    UiLayoutStyle form_style = ui_layout_style();
    form_style.flow = UiLayoutFlow::vertical;
    form_style.width = ui_layout_fill();
    form_style.height = ui_layout_fill();
    form_style.horizontal_alignment = UiLayoutAlignment::stretch;
    form_style.vertical_alignment = UiLayoutAlignment::stretch;
    add_widget(scene, widgets, LoginElement::form, row, UiWidgetKind::container, ui_widget_enabled, form_style);
    const UiWidgetHandle form = scene->elements[static_cast<u32>(LoginElement::form)];
    // Recovery copy uses the form width instead of the shorter sign-in copy's
    // intrinsic boxes. Reserve two subtitle lines before laying out Back/fields.
    const bool alternate = scene->phone_mode || scene->recovery_mode || scene->reset_mode || scene->browser_mode;
    const auto recovery_alignment = alternate ? UiLayoutAlignment::stretch : UiLayoutAlignment::center;
    add_text(scene, widgets, LoginElement::heading, form, 200.0f, 30.0f, 0.0f, recovery_alignment);
    add_text(scene, widgets, LoginElement::subtitle, form, 300.0f, alternate ? 44.0f : 20.0f, 4.0f,
             recovery_alignment);
    if (!scene->phone_mode && !scene->reset_mode && !scene->browser_mode && !(scene->recovery_mode && scene->back_only))
        add_text(scene, widgets, LoginElement::login_label, form, 180.0f, 16.0f, 13.0f, UiLayoutAlignment::start);

    UiLayoutStyle field_style = ui_layout_style();
    field_style.width = ui_layout_fill();
    field_style.height = ui_layout_fixed(metrics.input_height);
    field_style.margin.top = 9.0f;
    field_style.padding = scene->phone_mode ? UiInsets{} : UiInsets{12.0f, 8.0f, 12.0f, 8.0f};
    field_style.horizontal_alignment = UiLayoutAlignment::stretch;
    // The password field reserves room for its show/hide toggle.
    UiLayoutStyle secret_style = field_style;
    if (!scene->phone_mode) secret_style.padding.right = 40.0f;
    if (!scene->phone_mode && !scene->reset_mode && !scene->browser_mode && !(scene->recovery_mode && scene->back_only))
        add_control(scene, widgets, LoginElement::login_field, form, UiWidgetKind::text_field, field_style,
                    scene->mfa_capabilities == 0);
    if (scene->phone_mode || !scene->credential_fields_hidden) {
        add_text(scene, widgets, LoginElement::password_label, form, 180.0f, 16.0f, 15.0f, UiLayoutAlignment::start);
        add_control(scene, widgets, LoginElement::password_field, form, UiWidgetKind::text_field, secret_style);
    }

    UiLayoutStyle auxiliary_style = ui_layout_style();
    auxiliary_style.flow = UiLayoutFlow::horizontal;
    auxiliary_style.width = ui_layout_fill();
    auxiliary_style.height = ui_layout_fixed(24.0f);
    auxiliary_style.margin.top = 4.0f;
    auxiliary_style.gap = 4.0f;
    auxiliary_style.horizontal_alignment = UiLayoutAlignment::stretch;
    add_widget(scene, widgets, LoginElement::auxiliary, form, UiWidgetKind::container, ui_widget_enabled,
               auxiliary_style);
    const UiWidgetHandle auxiliary = scene->elements[static_cast<u32>(LoginElement::auxiliary)];
    if (scene->mfa_capabilities == 0)
        add_auxiliary_link(scene, widgets, auxiliary, 0.0f);
    if (scene->browser_available) {
        auto browser_style = text_style(22.0f, 0.0f, UiLayoutAlignment::end);
        browser_style.width = ui_layout_fill();
        add_control(scene, widgets, LoginElement::browser_sign_in, auxiliary, UiWidgetKind::button, browser_style);
    }
    if (scene->phone_mode) {
        UiLayoutStyle resend_style = ui_layout_style();
        resend_style.width = ui_layout_fixed(128.0f);
        resend_style.height = ui_layout_fill();
        resend_style.horizontal_alignment = UiLayoutAlignment::end;
        add_control(scene, widgets, LoginElement::mfa_sms, auxiliary, UiWidgetKind::button, resend_style);
    }
    if (scene->mfa_capabilities != 0) {
        for (u32 index = 0; index < kLoginMfaPresentationMethodCount; ++index) {
            const LoginMfaPresentationMethod& method = kLoginMfaPresentationMethods[index];
            if ((scene->mfa_capabilities & method.capability) == 0) {
                continue;
            }
            UiLayoutStyle method_style = ui_layout_style();
            method_style.width = ui_layout_fill();
            method_style.height = ui_layout_fill();
            method_style.horizontal_alignment = UiLayoutAlignment::stretch;
            method_style.vertical_alignment = UiLayoutAlignment::stretch;
            add_control(scene, widgets, method.element, auxiliary, UiWidgetKind::button, method_style);
        }
    }

    // Allocate the separate reset Back row after its visual method row. Native
    // focus traversal follows widget order, so Tab and Shift+Tab agree with it.
    if (scene->reset_mode && scene->mfa_capabilities)
        add_auxiliary_link(scene, widgets, form, 4.0f);

    UiLayoutStyle submit_style = ui_layout_style();
    submit_style.width = ui_layout_fill();
    submit_style.height = ui_layout_fixed(metrics.submit_height);
    submit_style.margin.top = 17.0f;
    submit_style.horizontal_alignment = UiLayoutAlignment::stretch;
    add_control(scene, widgets, LoginElement::submit, form, UiWidgetKind::button, submit_style);
    UiLayoutStyle status_style = ui_layout_style();
    status_style.flow = UiLayoutFlow::vertical;
    status_style.width = ui_layout_fill();
    status_style.height = ui_layout_fill();
    status_style.margin.top = 8.0f;
    status_style.horizontal_alignment = UiLayoutAlignment::stretch;
    status_style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_allow_zero_size);
    add_widget(scene, widgets, LoginElement::status, form, UiWidgetKind::container, ui_widget_none, status_style,
               UiSize{}, 0.0f);
    const UiWidgetHandle status = scene->elements[static_cast<u32>(LoginElement::status)];
    add_text(scene, widgets, LoginElement::status_message, status, 0.0f,
             scene->reset_mode && scene->mfa_capabilities ? 48.0f : alternate ? 64.0f : 18.0f, 0.0f,
             UiLayoutAlignment::stretch);
    add_text(scene, widgets, LoginElement::status_account, status, 0.0f, 18.0f, 0.0f, UiLayoutAlignment::stretch);

    if (scene->qr_visible) {
        UiLayoutStyle qr_column_style = ui_layout_style();
        qr_column_style.flow = UiLayoutFlow::vertical;
        qr_column_style.width = ui_layout_fixed(metrics.qr_column_width);
        qr_column_style.height = ui_layout_fill();
        qr_column_style.horizontal_alignment = UiLayoutAlignment::stretch;
        qr_column_style.vertical_alignment = UiLayoutAlignment::stretch;
        add_widget(scene, widgets, LoginElement::qr_column, row, UiWidgetKind::container, ui_widget_enabled,
                   qr_column_style);
        const UiWidgetHandle qr_column = scene->elements[static_cast<u32>(LoginElement::qr_column)];
        UiLayoutStyle qr_style = ui_layout_style();
        qr_style.width = ui_layout_fixed(metrics.qr_extent);
        qr_style.height = ui_layout_fixed(metrics.qr_extent);
        qr_style.margin.top = 44.0f;
        qr_style.horizontal_alignment = UiLayoutAlignment::center;
        add_control(scene, widgets, LoginElement::qr, qr_column, UiWidgetKind::image, qr_style);
        add_text(scene, widgets, LoginElement::qr_title, qr_column, 216.0f, 20.0f, 14.0f, UiLayoutAlignment::center);
        add_text(scene, widgets, LoginElement::qr_subtitle, qr_column, 240.0f, 40.0f, 6.0f,
                 UiLayoutAlignment::center);
    }
    return widgets.ok() && ui_widget_manager_layout_rectangle(&scene->widgets, {0, 0, width, height}).ok();
}

void discard_login_layout(LoginScene* scene) noexcept {
    ui_widget_manager_reset(&scene->widgets);
    for (auto& element : scene->elements) {
        element = ui_widget_invalid_handle();
    }
    scene->layout_width = 0;
    scene->layout_height = 0;
    scene->layout_metrics_revision = 0;
}

bool control_input_enabled(const LoginScene& scene, LoginControl control) noexcept {
    if (scene.restoring_session)
        return false;
    if (scene.auth_retry_resume && control != LoginControl::back)
        return control == LoginControl::submit && !scene.submit_disabled;
    if (control == LoginControl::browser_sign_in) return scene.browser_available;
    if (control == LoginControl::submit && scene.captcha_action) return !scene.submit_disabled;
    if (scene.back_only)
        return control == LoginControl::back;
    if (scene.phone_mode)
        return control == LoginControl::back ||
               (!scene.action_disabled &&
                (control == LoginControl::phone_code ||
                 (control == LoginControl::submit && !scene.submit_disabled) ||
                 (control == LoginControl::phone_resend && !scene.phone_resend_disabled)));
    if (scene.recovery_mode)
        return control == LoginControl::back ||
               (!scene.action_disabled && (control == LoginControl::login ||
                                           (control == LoginControl::submit && !scene.submit_disabled)));
    if (scene.reset_mode) {
        if (control == LoginControl::back)
            return true;
        if (control == LoginControl::login || control == LoginControl::qr ||
            control == LoginControl::forgot_password)
            return false;
    }
    if (control == LoginControl::forgot_password)
        return !scene.recovery_disabled && !scene.action_disabled && !scene.mfa_capabilities;
    if (control == LoginControl::back)
        return false;
    const bool credential = control == LoginControl::login || control == LoginControl::password ||
                            control == LoginControl::submit;
    return !(scene.action_disabled && credential) &&
           !(scene.submit_disabled && control == LoginControl::submit);
}

void sync_login_input(LoginScene* scene) noexcept {
    static constexpr LoginControl controls[]{
        LoginControl::login,           LoginControl::password,   LoginControl::submit,
        LoginControl::forgot_password, LoginControl::qr,         LoginControl::mfa_totp,
        LoginControl::mfa_sms,         LoginControl::mfa_backup, LoginControl::mfa_webauthn,
        LoginControl::browser_sign_in};
    for (const auto control : controls) {
        const auto handle = scene->elements[static_cast<u32>(element_for_control(control))];
        if (ui_widget_manager_resolve(scene->widgets, handle)) {
            // Forgot and Back share one widget; apply its current meaning once.
            const auto semantic = semantic_control(*scene, control);
            ui_widget_manager_block_input(&scene->widgets, handle,
                                          !control_input_enabled(*scene, semantic));
        }
    }
    if (!control_input_enabled(*scene, scene->focus))
        scene->focus = LoginControl::none;
}

} // namespace

UiRect login_phone_cell_rectangle(UiRect bounds, u32 cell) noexcept {
    if (!ui_rect_valid(bounds) || cell >= kLoginPhoneCodeDigits)
        return {};
    const float gap = minimum(8.0f, ui_rect_width(bounds) / 36.0f);
    const float width = minimum(44.0f, (ui_rect_width(bounds) - gap * 6.0f) / 6.0f);
    const float height = minimum(44.0f, ui_rect_height(bounds));
    const float inset = (ui_rect_width(bounds) - width * 6.0f - gap * 6.0f) * 0.5f;
    const float left = bounds.left + inset + cell * (width + gap) + (cell >= 3 ? gap : 0.0f);
    const float top = bounds.top + (ui_rect_height(bounds) - height) * 0.5f;
    return {left, top, left + width, top + height};
}
UiRect login_phone_caret_rectangle(UiRect bounds, u32 position) noexcept {
    if (position > kLoginPhoneCodeDigits)
        return {};
    const auto cell = login_phone_cell_rectangle(bounds, minimum(position, 5u));
    if (!ui_rect_has_area(cell))
        return {};
    const float glyph_half = minimum(4.5f, ui_rect_width(cell) * 0.25f);
    const float x = (cell.left + cell.right) * 0.5f + (position == 6 ? glyph_half : -glyph_half);
    return {x, cell.top + ui_rect_height(cell) * 0.2f, x + 1.0f, cell.bottom - ui_rect_height(cell) * 0.2f};
}
UiRect login_phone_range_rectangle(UiRect bounds, u32 first, u32 last) noexcept {
    if (first > last || last > kLoginPhoneCodeDigits)
        return {};
    if (first == last)
        return login_phone_caret_rectangle(bounds, first);
    const auto begin = login_phone_cell_rectangle(bounds, first);
    const auto end = login_phone_cell_rectangle(bounds, last - 1);
    return {begin.left, begin.top, end.right, end.bottom};
}
bool login_phone_point_position(UiRect bounds, u32 digits, float x, float y, bool round_nearest,
                                u32* position, bool* inside) noexcept {
    if (!position || !inside || digits > kLoginPhoneCodeDigits || !ui_rect_valid(bounds) ||
        !ui_coordinate_valid(x) || !ui_coordinate_valid(y))
        return false;
    *inside = x >= bounds.left && x <= bounds.right && y >= bounds.top && y <= bounds.bottom;
    // Every box remains addressable even when earlier boxes are empty. Points
    // inside a box select that slot; nearest rounding in gaps selects a box.
    for (u32 i = 0; i < kLoginPhoneCodeDigits; ++i) {
        const auto cell = login_phone_cell_rectangle(bounds, i);
        const auto next = login_phone_cell_rectangle(bounds, minimum(i + 1, 5u));
        const float edge = round_nearest && i < 5 ? (cell.right + next.left) * 0.5f : cell.right;
        if (x <= edge) {
            *position = i;
            return true;
        }
    }
    *position = kLoginPhoneCodeDigits;
    return true;
}

bool login_scene_layout(LoginScene* scene, float width, float height) noexcept {
    return login_scene_layout_with_metrics(scene, width, height, kDefaultLoginLayoutMetrics);
}

void login_scene_release(LoginScene* scene) noexcept {
    if (!scene) {
        return;
    }
    ui_fixed_widget_storage_release(&scene->storage);
    reset_value<LoginScene>(scene);
}

bool login_scene_layout_with_metrics(LoginScene* scene, float width, float height,
                                     const LoginLayoutMetrics& metrics) noexcept {
    if (!scene || !ui_extent_valid(width) || !ui_extent_valid(height) || width <= 0.0f || height <= 0.0f) {
        return false;
    }
    const LoginControl focus = scene->focus;
    scene->focus = focusable(focus) ? focus : LoginControl::none;
    // Retained MFA metadata cannot replace the only escape control when the
    // independent QR flow reports a challenge.
    if (scene->back_only) {
        scene->mfa_capabilities = 0;
        scene->credential_fields_hidden = scene->phone_mode || scene->recovery_mode || scene->reset_mode || scene->browser_mode;
    }
    scene->mfa_capabilities &= login_mfa_capability_all;
    const u8 selected = scene->selected_mfa_capability & scene->mfa_capabilities;
    scene->selected_mfa_capability = selected != 0 && (selected & static_cast<u8>(selected - 1)) == 0
                                         ? selected
                                         : login_mfa_capability_none;
    const bool full_viewport = width <= metrics.full_viewport_max_width;
    if (scene->phone_mode)
        scene->mfa_capabilities = scene->selected_mfa_capability = 0;
    const bool qr_visible = !scene->phone_mode && !scene->recovery_mode && !scene->reset_mode && !scene->browser_mode &&
                            !full_viewport && width >= metrics.qr_min_width;
    const u8 variant = login_layout_variant(full_viewport, qr_visible, width, height, metrics);
    const bool same_structure =
        scene->widgets.count != 0 && scene->layout_mfa_capabilities == scene->mfa_capabilities &&
        scene->layout_credential_fields_hidden == scene->credential_fields_hidden &&
        scene->layout_recovery_mode == scene->recovery_mode &&
        scene->layout_reset_mode == scene->reset_mode && scene->layout_phone_mode == scene->phone_mode &&
        scene->layout_browser_mode == scene->browser_mode && scene->layout_browser_available == scene->browser_available &&
        scene->layout_recovery_result == (scene->recovery_mode && scene->back_only) &&
        scene->layout_variant == variant && scene->layout_metrics_revision == metrics.revision;
    const bool reusable = same_structure && scene->layout_width == width && scene->layout_height == height;
    scene->full_viewport = full_viewport;
    scene->qr_visible = qr_visible;
    if (reusable) {
        sync_login_input(scene);
        if (scene->focus == LoginControl::none) {
            return ui_widget_manager_focus(&scene->widgets, ui_widget_invalid_handle());
        }
        return login_scene_focus(scene, scene->focus);
    }
    if (same_structure) {
        if (!ui_widget_manager_layout_rectangle(&scene->widgets, {0, 0, width, height}).ok()) {
            discard_login_layout(scene);
            return false;
        }
    } else {
        if (!build_login_widgets(scene, width, height, metrics)) {
            discard_login_layout(scene);
            return false;
        }
        ++scene->layout_rebuilds;
    }
    ++scene->layout_passes;
    scene->layout_width = width;
    scene->layout_height = height;
    scene->layout_mfa_capabilities = scene->mfa_capabilities;
    scene->layout_credential_fields_hidden = scene->credential_fields_hidden;
    scene->layout_recovery_mode = scene->recovery_mode;
    scene->layout_reset_mode = scene->reset_mode;
    scene->layout_phone_mode = scene->phone_mode;
    scene->layout_browser_mode = scene->browser_mode;
    scene->layout_browser_available = scene->browser_available;
    scene->layout_recovery_result = scene->recovery_mode && scene->back_only;
    scene->layout_variant = variant;
    scene->layout_metrics_revision = metrics.revision;
    sync_login_input(scene);
    if (scene->focus != LoginControl::none && !login_scene_focus(scene, scene->focus)) {
        scene->focus = LoginControl::none;
    }
    return true;
}

LoginControl login_scene_hit_test(const LoginScene& scene, float x, float y) noexcept {
    const UiWidgetHandle hit = ui_widget_manager_hit_test(scene.widgets, x, y);
    const UiWidget* widget = ui_widget_manager_resolve(scene.widgets, hit);
    if (!widget || widget->id == 0 || widget->id > static_cast<u32>(LoginElement::count)) {
        return LoginControl::none;
    }
    const LoginControl control =
        semantic_control(scene, control_for_element(static_cast<LoginElement>(widget->id - 1)));
    if (!control_input_enabled(scene, control)) {
        return LoginControl::none;
    }
    return control;
}

UiRect login_password_reveal_rectangle(const LoginScene& scene) noexcept {
    if (!scene.password_reveal_available || scene.phone_mode || scene.credential_fields_hidden)
        return {};
    const UiRect field = login_scene_rectangle(scene, LoginElement::password_field);
    if (!ui_rect_has_area(field) || ui_rect_width(field) < 64.0f)
        return {};
    const float extent = minimum(28.0f, ui_rect_height(field) - 8.0f);
    const float top = field.top + (ui_rect_height(field) - extent) * 0.5f;
    return {field.right - 8.0f - extent, top, field.right - 8.0f, top + extent};
}

bool login_password_reveal_hit_test(const LoginScene& scene, float x, float y) noexcept {
    const UiRect box = login_password_reveal_rectangle(scene);
    return ui_rect_has_area(box) && !scene.restoring_session && !scene.action_disabled && x >= box.left &&
           x <= box.right && y >= box.top && y <= box.bottom;
}

bool login_scene_focus(LoginScene* scene, LoginControl control) noexcept {
    if (!scene || !focusable(control) || !control_input_enabled(*scene, control) ||
        (control == LoginControl::qr && !scene->qr_visible) ||
        (control == LoginControl::password && scene->credential_fields_hidden) ||
        (capability_for_control(control) != login_mfa_capability_none &&
         (scene->mfa_capabilities & capability_for_control(control)) == 0)) {
        return false;
    }
    if (control == LoginControl::none) {
        scene->focus = control;
        return ui_widget_manager_focus(&scene->widgets, ui_widget_invalid_handle());
    }
    const LoginElement element = element_for_control(control);
    if (element == LoginElement::count) {
        return false;
    }
    const UiWidgetHandle widget = scene->elements[static_cast<u32>(element)];
    if (!ui_widget_manager_focus(&scene->widgets, widget)) {
        return false;
    }
    scene->focus = control;
    return true;
}

LoginControl login_scene_move_focus(LoginScene* scene, bool reverse) noexcept {
    if (!scene) {
        return LoginControl::none;
    }
    sync_login_input(scene);
    const UiWidgetHandle focused = ui_widget_manager_move_focus(&scene->widgets, reverse);
    const UiWidget* widget = ui_widget_manager_resolve(scene->widgets, focused);
    if (!widget || widget->id == 0 || widget->id > static_cast<u32>(LoginElement::count)) {
        scene->focus = LoginControl::none;
        return LoginControl::none;
    }
    const LoginControl control = semantic_control(
        *scene, control_for_element(static_cast<LoginElement>(widget->id - 1)));
    scene->focus = control;
    return control;
}

const UiLayoutNode* login_scene_element(const LoginScene& scene, LoginElement element) noexcept {
    const u32 index = static_cast<u32>(element);
    if (index >= static_cast<u32>(LoginElement::count)) {
        return nullptr;
    }
    return ui_widget_manager_layout_node(scene.widgets, scene.elements[index]);
}

UiRect login_scene_rectangle(const LoginScene& scene, LoginElement element) noexcept {
    const UiLayoutNode* node = login_scene_element(scene, element);
    return node ? node->rectangle : UiRect{};
}

UiRect login_scene_content_rectangle(const LoginScene& scene, LoginElement element) noexcept {
    const UiLayoutNode* node = login_scene_element(scene, element);
    return node ? node->content_rectangle : UiRect{};
}

} // namespace maicord
