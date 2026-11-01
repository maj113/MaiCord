#include "settings_dialog.hpp"
#include "../settings/layer.hpp"
#include "../settings/modal.hpp"
#include "../shell/state.hpp"
#include "maicord/app/plugins/settings_page.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/chat/bounded_edit.hpp"
#include "maicord/ui/shell/popup_scene.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {

constexpr u64 kPageOwner = 0x4D43504C55475354ull; // "MCPLUGST"
constexpr UiPageKey kPage{kPageOwner, 1};
constexpr u32 kChoiceBase = 1000;
// Room for the text of a value as typed: a number, a colour or a key is short; a list is long.
constexpr u32 kShortBytes = 96, kUserListBytes = MC_SETTING_ITEM_MAX * 21;
constexpr u32 kListBytes = MC_SETTING_LIST_BYTES_MAX + MC_SETTING_ITEM_MAX;

struct Form : SettingsDialog {
    PluginSettingsPage plugin_page;
    UiSettingsFooter footer;
    UiSettingsEditor editors[MC_SETTING_MAX];
    MessageComposer models[MC_SETTING_MAX];
    BoundedEdit bindings[MC_SETTING_MAX];
    PopupItem popup_items[MC_SETTING_ITEM_MAX];
    u32 offsets[MC_SETTING_MAX], capacities[MC_SETTING_MAX];
    u8* storage;
    PluginRuntime* runtime;
    const PluginBinding* binding_address;
    u32 generation;
    u64 lifetime, account_scope;
    u32 choice_setting;
    Utf8View status;
    bool failed;
};
Form* form;
u64 lifetime_serial;

PluginBinding* binding() noexcept {
    return const_cast<PluginBinding*>(form->binding_address);
}
const mc_setting_def& setting_of(u32 index) noexcept {
    return binding()->image.descriptor->settings[index];
}
// Settings the user types: everything but switches, choices, slider-steps and headers.
bool typed(const mc_setting_def& setting) noexcept {
    return setting.type != MC_SET_HEADER && setting.type != MC_SET_TOGGLE && setting.type != MC_SET_CHOICE;
}
u32 capacity_for(const mc_setting_def& setting) noexcept {
    switch (setting.type) {
    case MC_SET_TEXT:
    case MC_SET_SECRET: return (setting.max_bytes ? setting.max_bytes : MC_SETTING_TEXT_MAX) + 4;
    case MC_SET_TEXT_LIST: return kListBytes;
    case MC_SET_USER_LIST: return kUserListBytes;
    default: return kShortBytes;
    }
}
bool secret(u32 index) noexcept {
    return setting_of(index).type == MC_SET_SECRET;
}
bool secret_set(u32 index) noexcept {
    mc_str text;
    copy_bytes(&text, static_cast<const u8*>(binding()->ctx.settings) + setting_of(index).offset, sizeof(text));
    return text.bytes != 0;
}
u32 index_of(u64 control) noexcept {
    return control >= 1 && control <= MC_SETTING_MAX ? static_cast<u32>(control - 1) : ~0u;
}
bool plugin_current() noexcept {
    const PluginRuntime* runtime = form->runtime;
    for (u32 i = 0; i < runtime->binding_count; ++i)
        if (runtime->bindings[i] == form->binding_address) return runtime->bindings[i]->generation == form->generation;
    return false;
}

bool binding_ready(void* model, u64 identity) noexcept {
    MessageComposer* focused = nullptr;
    return form && form->lifetime == identity && form->scene.active &&
           settings_layer_editor(&form->layer, nullptr, &focused, nullptr) && focused == model;
}
// Puts what the plugin holds for setting into its editor (a secret's editor starts empty).
void load_editor(u32 index) noexcept {
    auto& f = *form;
    const mc_setting_def& setting = setting_of(index);
    char* text = reinterpret_cast<char*>(f.storage + f.offsets[index]);
    const u32 bytes = secret(index) ? 0 : plugin_setting_format(*binding(), index, {text, f.capacities[index]});
    message_composer_refresh_borrowed(&f.models[index], bytes);
    message_composer_set_selection(&f.models[index], bytes, bytes);
    bounded_edit_bind(&f.bindings[index], &f.models[index], f.capacities[index],
                      setting.type == MC_SET_TEXT_LIST || setting.type == MC_SET_USER_LIST ||
                          (setting.type == MC_SET_TEXT && (setting.flags & MC_SET_MULTILINE)),
                      f.lifetime, binding_ready, &f.models[index]);
}

const UiSettingsEditor* editor_view(void*, u32 index, bool, bool masked) noexcept {
    auto& f = *form;
    const mc_setting_def& setting = setting_of(index);
    const Utf8View hint{setting.hint.data, setting.hint.bytes};
    f.editors[index] = settings_layer_editor_view(
        &f.layer, index + 1, f.models[index],
        masked && secret_set(index) ? utf8_literal("Saved. Type to replace it.") : hint, true);
    return &f.editors[index];
}

bool project() noexcept {
    if (!application_plugin_settings_active()) return true;
    auto& f = *form;
    plugin_settings_project(*binding(), {nullptr, editor_view}, &f.plugin_page);
    auto& page = f.plugin_page;
    for (u32 i = 0; i < page.count; ++i) {
        const u32 index = index_of(page.ids[i]);
        if (index != ~0u && page.controls[i].kind == UiSettingsControlKind::edit && secret(index) && secret_set(index))
            page.controls[i].action_label = utf8_literal("Clear");
    }
    f.footer = {f.status.bytes ? f.status : utf8_literal("Typed values apply when you save."),
                utf8_literal("Save"),
                {},
                true,
                true,
                false,
                f.failed};
    f.content.title = page.title;
    f.content.description = page.content.description;
    f.content.controls = page.content.controls;
    f.content.presentation = page.content.presentation;
    f.content.footer = &f.footer;
    ++f.content.revision;
    return settings_modal_update(f);
}
bool repaint() noexcept {
    return settings_modal_present(project());
}

// Commits every typed row that changed; secrets only when something was typed.
void save() noexcept {
    auto& f = *form;
    bool refused = false;
    for (u32 i = 0; i < f.plugin_page.count; ++i) {
        const u32 index = index_of(f.plugin_page.ids[i]);
        if (index == ~0u || !typed(setting_of(index)) || f.plugin_page.controls[i].kind == UiSettingsControlKind::slider) continue;
        const Utf8View text = message_composer_text(f.models[index]);
        char current[kListBytes];
        const u32 held = secret(index) ? 0 : plugin_setting_format(*binding(), index, {current, sizeof(current)});
        if (secret(index) ? !text.bytes : (text.bytes == held && bytes_equal(text.data, current, held))) continue;
        const PluginSettingsOutcome outcome = plugin_settings_commit(binding(), &f.plugin_page, f.plugin_page.ids[i], text);
        refused = refused || outcome == PluginSettingsOutcome::refused;
        if (outcome == PluginSettingsOutcome::changed) load_editor(index);
    }
    f.failed = refused;
    f.status = refused ? utf8_literal("Some values weren't accepted.") : utf8_literal("Saved.");
    repaint();
}

void activate(SettingsLayer*, const SettingsLayerActivation& activation) noexcept {
    if (!form) return;
    auto& f = *form;
    const auto target = activation.value.target;
    switch (target.kind) {
    case UiSettingsTargetKind::close: application_plugin_settings_close(); return;
    case UiSettingsTargetKind::save: save(); return;
    case UiSettingsTargetKind::control: break;
    default: return;
    }
    if (!plugin_current()) return;
    const u32 index = index_of(target.control_id);
    const auto kind = activation.value.kind;
    if (kind == UiSettingsControlKind::edit || kind == UiSettingsControlKind::multiline) {
        if (activation.button && index != ~0u && secret(index)) {
            plugin_binding_change_setting(binding(), index, nullptr, 0);
            f.status = utf8_literal("Cleared.");
            f.failed = false;
            repaint();
        } else {
            settings_layer_focus(&f.layer, target);
        }
        return;
    }
    if (kind == UiSettingsControlKind::choice && index != ~0u) {
        const mc_setting_def& setting = setting_of(index);
        const u32 count = setting.item_count < MC_SETTING_ITEM_MAX ? setting.item_count : MC_SETTING_ITEM_MAX;
        u32 current = 0;
        copy_bytes(&current, static_cast<const u8*>(binding()->ctx.settings) + setting.offset, 4);
        for (u32 i = 0; i < count; ++i) {
            reset_value(&f.popup_items[i]);
            f.popup_items[i].label = {setting.items[i].data, setting.items[i].bytes};
            f.popup_items[i].action = kChoiceBase + i;
            f.popup_items[i].flags = static_cast<u16>(popup_item_enabled | (i == current ? popup_item_checked : 0));
        }
        f.choice_setting = index;
        settings_layer_open_choice(&f.layer, kPageOwner, {f.popup_items, count}, activation.anchor, 240);
        return;
    }
    if (plugin_settings_activate(binding(), &f.plugin_page, activation.value) != PluginSettingsOutcome::none) repaint();
}
void choose(SettingsLayer*, u32 action) noexcept {
    if (!form || action < kChoiceBase || !plugin_current()) return;
    UiSettingsActivation picked{};
    picked.target = {UiSettingsTargetKind::control, kPage, form->choice_setting + 1};
    picked.kind = UiSettingsControlKind::choice;
    picked.selected_choice = action - kChoiceBase;
    plugin_settings_activate(binding(), &form->plugin_page, picked);
    repaint();
}
bool edited(SettingsLayer*, u64 control) noexcept {
    const u32 index = form ? index_of(control) : ~0u;
    if (index == ~0u || !typed(setting_of(index))) return false;
    form->failed = false;
    form->status = {};
    return true;
}
bool editor_hook(SettingsLayer*, u64 control, MessageComposer** model, BoundedEdit** edit_binding,
                 bool* multiline) noexcept {
    const u32 index = form ? index_of(control) : ~0u;
    if (index == ~0u || !typed(setting_of(index))) return false;
    *model = &form->models[index];
    *edit_binding = &form->bindings[index];
    const mc_setting_def& setting = setting_of(index);
    *multiline = setting.type == MC_SET_TEXT_LIST || setting.type == MC_SET_USER_LIST ||
                 (setting.type == MC_SET_TEXT && (setting.flags & MC_SET_MULTILINE));
    return true;
}
constexpr SettingsLayerHooks kHooks{editor_hook,
                                    edited,
                                    activate,
                                    choose,
                                    settings_hook_close<application_plugin_settings_close>,
                                    settings_hook<repaint>,
                                    nullptr,
                                    settings_hook<application_plugin_settings_active>};

void free_storage() noexcept {
    if (!form || !form->storage) return;
    for (u32 i = 0; i < MC_SETTING_MAX; ++i)
        if (form->models[i].text) message_composer_release(&form->models[i]);
    VirtualFree(form->storage, 0, MEM_RELEASE);
    form->storage = nullptr;
}

} // namespace

bool application_plugin_settings_open(PluginBinding* target) noexcept {
    if (!target || !target->image.descriptor || !g_app.account_ui || g_app.login.visible || !g_app.renderer_ready ||
        g_app.text_form || application_plugin_settings_active())
        return false;
    settings_modal_yield();
    if (!form) {
        form = settings_modal_allocate<Form>(kHooks);
        if (!form) return false;
    }
    auto& f = *form;
    free_storage();
    const mc_descriptor& descriptor = *target->image.descriptor;
    u32 total = 0;
    for (u32 i = 0; i < descriptor.setting_count; ++i) {
        f.offsets[i] = total;
        f.capacities[i] = typed(descriptor.settings[i]) ? capacity_for(descriptor.settings[i]) : 0;
        total += (f.capacities[i] + 15) & ~15u;
    }
    f.storage = total ? static_cast<u8*>(VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) : nullptr;
    if (total && !f.storage) return false;
    f.binding_address = target;
    f.runtime = target->runtime;
    f.generation = target->generation;
    f.lifetime = ++lifetime_serial;
    f.account_scope = g_app.plugin_account_scope_serial;
    f.status = {};
    f.failed = false;
    f.plugin_page.rejected = 0;
    for (u32 i = 0; i < descriptor.setting_count; ++i) {
        if (!f.capacities[i]) continue;
        message_composer_initialize(&f.models[i], {reinterpret_cast<char*>(f.storage + f.offsets[i]), f.capacities[i]});
        load_editor(i);
    }
    if (!settings_dialog_begin(f, kPage, {target->image.info.name, target->image.info.name_bytes})) return false;
    if (settings_dialog_show(f, kPage, 0, false, project) && repaint()) return true;
    application_plugin_settings_close(false);
    return false;
}

bool application_plugin_settings_active() noexcept {
    return form && form->scene.active;
}

void application_plugin_settings_close(bool restore_focus) noexcept {
    if (!application_plugin_settings_active()) return;
    settings_modal_suspend(*form);
    settings_modal_leave(*form, restore_focus && g_app.account_ui && !g_app.login.visible);
}

void application_plugin_settings_release() noexcept {
    application_plugin_settings_close(false);
    if (!form) return;
    free_storage();
    settings_modal_free(form);
}

bool application_plugin_settings_synchronize() noexcept {
    if (!application_plugin_settings_active()) return true;
    auto& f = *form;
    if (!g_app.account_ui || g_app.login.visible || f.account_scope != g_app.plugin_account_scope_serial || !plugin_current()) {
        application_plugin_settings_close(false);
        return true;
    }
    if (f.layer.choice && !settings_layer_choice_current(f.layer)) {
        f.layer.choice = false;
        return repaint();
    }
    return settings_modal_refresh(f);
}

SettingsLayer* application_plugin_settings_settings_layer() noexcept {
    return form ? &form->layer : nullptr;
}
u64 application_plugin_settings_caret_wake() noexcept {
    return application_plugin_settings_active() ? settings_layer_next_wake(form->layer) : 0;
}

} // namespace maicord::app_detail
