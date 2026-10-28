// Plugin SDK host, ABI 5: lifecycle, settings pages and keybinds, commands,
// pre-send hooks, storage, timers and interface lookup. Menus, slots and
// decorators live in plugin_sdk_surfaces.cpp; read, events, actions and
// effects in plugin_sdk_data.cpp.
#include "plugin_sdk_internal.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/color_hex.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/hash.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace plugin_sdk {

NativePluginSdkBinding* g_live[kPluginSdkBindings];

// ---- Basics -------------------------------------------------------------------

NativePluginSdkBinding* live(const void* candidate) noexcept {
    if (!candidate) return nullptr;
    for (auto* binding : g_live)
        if (binding == candidate && binding->magic == kBindingMagic) return binding;
    return nullptr;
}
NativePluginSdkBinding* active(void* candidate, u64 generation, bool registration) noexcept {
    auto* binding = live(candidate);
    if (!binding || !generation || binding->generation != generation) return nullptr;
    if (registration) return binding->phase == Phase::starting ? binding : nullptr;
    return binding->phase == Phase::starting || binding->phase == Phase::enabled ? binding : nullptr;
}
bool callable(const NativePluginSdkBinding& binding, const void* handler) noexcept {
    return handler && binding.phase == Phase::enabled &&
           !plugin_call_ledger_disable_requested(binding.runtime->ledger, binding.owner_id);
}
bool usable(const NativePluginSdkBinding* binding, const NativePluginSdkRuntime& runtime) noexcept {
    return binding && binding->runtime == &runtime && binding->phase == Phase::enabled &&
           !plugin_call_ledger_disable_requested(runtime.ledger, binding->owner_id);
}
NativePluginSdkBinding* binding_for(const NativePluginSdkRuntime& runtime, u64 owner) noexcept {
    for (auto* binding : g_live)
        if (binding && binding->runtime == &runtime && binding->phase == Phase::enabled && binding->owner_id == owner)
            return binding;
    return nullptr;
}
u64 now_ms(const NativePluginSdkRuntime& runtime) noexcept {
    return runtime.clock_ms ? runtime.clock_ms() : GetTickCount64();
}
bool text_ok(mc_str text, u32 maximum, bool required, bool lines) noexcept {
    if (text.bytes > maximum || (required && !text.bytes) || (text.bytes && !text.data)) return false;
    if (!text.bytes) return true;
    const auto checked = utf8_validate(view(text));
    if (!checked.valid) return false;
    for (u32 i = 0; i < text.bytes; ++i) {
        const u8 c = static_cast<u8>(text.data[i]);
        if (c < 32 && !(lines && c == '\n')) return false;
    }
    return true;
}
u32 copy_text(char* output, u32 capacity, Utf8View text) noexcept {
    const u32 bytes = minimum(capacity, text.bytes);
    copy_bytes(output, text.data, bytes);
    return bytes;
}
bool read_prefix_bytes(const void* source, void* output, u32 output_bytes, u32 minimum_bytes, u32 stride) noexcept {
    clear_bytes(output, output_bytes);
    if (!source) return false;
    u32 bytes{};
    copy_bytes(&bytes, source, sizeof(bytes));
    bytes = minimum(minimum(bytes, stride), output_bytes);
    if (bytes < minimum_bytes) return false;
    copy_bytes(output, source, bytes);
    copy_bytes(output, &bytes, sizeof(bytes));
    return true;
}
mc_status write_prefix_bytes(void* output, const void* value, u32 value_bytes) noexcept {
    if (!output) return MC_E_INVALID;
    u32 bytes{};
    copy_bytes(&bytes, output, sizeof(bytes));
    if (bytes < 8) return MC_E_INVALID;
    bytes = minimum(bytes, value_bytes);
    copy_bytes(output, value, bytes);
    copy_bytes(output, &bytes, sizeof(bytes));
    return MC_OK;
}
mc_status status_of(NativePluginResult result) noexcept {
    switch (result.error) {
    case NativePluginError::none: return MC_OK;
    case NativePluginError::capability_denied: return MC_E_DENIED;
    case NativePluginError::budget: return MC_E_BUDGET;
    case NativePluginError::capacity: return MC_E_CAPACITY;
    case NativePluginError::not_found: return MC_E_NOT_FOUND;
    case NativePluginError::inactive:
    case NativePluginError::stale: return MC_E_INACTIVE;
    case NativePluginError::busy: return MC_E_BUSY;
    case NativePluginError::unsupported: return MC_E_UNSUPPORTED;
    default: return MC_E_INVALID;
    }
}
bool url_ok(Utf8View url) noexcept {
    constexpr Utf8View scheme = utf8_literal("https://");
    if (url.bytes <= scheme.bytes || url.bytes > kPluginSdkUrlBytes ||
        !ascii_equal_ignore_case({url.data, scheme.bytes}, scheme)) return false;
    for (u32 i = 0; i < url.bytes; ++i) {
        const u8 c = static_cast<u8>(url.data[i]);
        if (c <= 32 || c == 127) return false;
    }
    return utf8_validate(url).valid;
}
bool read_node(const mc_slot_node* nodes, u32 stride, u32 index, const NodeLimits& limits, mc_slot_node* node) noexcept {
    const auto* element = reinterpret_cast<const mc_slot_node*>(reinterpret_cast<const u8*>(nodes) +
                                                                static_cast<usize>(index) * stride);
    if (!read_prefix(element, node, offsetof(mc_slot_node, tooltip), stride)) return false;
    if (node->kind > MC_NODE_INPUT || !(limits.allowed_kinds & (1u << node->kind))) return false;
    const bool needs_text = node->kind == MC_NODE_LABEL || node->kind == MC_NODE_BADGE || node->kind == MC_NODE_LINK ||
                            node->kind == MC_NODE_INPUT;
    const u32 text_limit = node->kind == MC_NODE_BADGE || node->kind == MC_NODE_BUTTON ? minimum(limits.text, kSlotTextBytes)
                                                                                      : limits.text;
    if (!text_ok(node->text, text_limit, needs_text) || !text_ok(node->tooltip, kSlotTooltipBytes, false) ||
        !text_ok(node->icon, 64, node->kind == MC_NODE_ICON) || !text_ok(node->value, limits.value, false))
        return false;
    if (node->kind == MC_NODE_BUTTON && (!node->action_id || (!node->text.bytes && !node->icon.bytes))) return false;
    if (node->kind == MC_NODE_LINK && !url_ok(view(node->value))) return false;
    if (node->kind != MC_NODE_LINK && node->kind != MC_NODE_INPUT) reset_value(&node->value);
    if (node->kind != MC_NODE_BUTTON) node->action_id = 0;
    if (node->tone > MC_TONE_CRITICAL) node->tone = MC_TONE_NEUTRAL;
    return true;
}
} // namespace plugin_sdk

namespace {
using namespace plugin_sdk;

bool command_name_ok(mc_str name) noexcept {
    if (!name.bytes || name.bytes > kCommandNameBytes || !name.data) return false;
    for (u32 i = 0; i < name.bytes; ++i) {
        const char c = name.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}
Page* find_page(NativePluginSdkBinding& binding, u64 page_id) noexcept {
    for (auto& page : binding.pages)
        if (page.registered && page.page_id == page_id) return &page;
    return nullptr;
}
const Page* find_page(const NativePluginSdkBinding& binding, u64 page_id) noexcept {
    return find_page(const_cast<NativePluginSdkBinding&>(binding), page_id);
}
u32 find_control(const Page& page, u64 control_id) noexcept {
    for (u32 i = 0; i < page.controls.count; ++i)
        if (page.bank[i].id == control_id) return i;
    return ~0u;
}

// ---- Keybinds ---------------------------------------------------------------------

bool chord_equal(NativePluginSdkChord a, NativePluginSdkChord b) noexcept {
    return a.key == b.key && a.modifiers == b.modifiers;
}
void keybinds_clear_page(NativePluginSdkBinding& binding, u64 page_id) noexcept {
    for (auto& keybind : binding.keybinds)
        if (keybind.page_id == page_id) reset_value(&keybind);
}
// Resolves one keybind control; returns the conflict label (empty when usable).
Utf8View keybind_resolve(const NativePluginSdkBinding& binding, u64 page_id, u64 control_id, NativePluginSdkChord chord,
                         char (&buffer)[kConflictBytes]) noexcept {
    const auto& runtime = *binding.runtime;
    for (const auto* other : g_live) {
        if (!other || other->runtime != &runtime || (other->phase != Phase::enabled && other->phase != Phase::starting))
            continue;
        for (const auto& keybind : other->keybinds) {
            if (!keybind.active || !chord_equal(keybind.chord, chord)) continue;
            if (other == &binding && keybind.page_id == page_id && keybind.control_id == control_id) continue;
            if (other == &binding) return utf8_literal("another shortcut of this plugin");
            const u32 bytes = copy_text(buffer, kConflictBytes, utf8_literal("another plugin"));
            return {buffer, bytes};
        }
    }
    Utf8View conflict{};
    if (runtime.keybind_check && !runtime.keybind_check(runtime.keybind_context, chord, &conflict)) {
        const u32 bytes = utf8_truncated_length(conflict, kConflictBytes);
        copy_bytes(buffer, conflict.data, bytes);
        return bytes ? Utf8View{buffer, bytes} : utf8_literal("a MaiCord shortcut");
    }
    if (!(chord.modifiers & 3u) && !(chord.key >= 0x70 && chord.key <= 0x87))
        return utf8_literal("a shortcut that needs Ctrl or Alt");
    return {};
}
bool keybind_add(NativePluginSdkBinding& binding, u64 page_id, u64 control_id, NativePluginSdkChord chord, bool usable_now,
                 Utf8View label) noexcept {
    for (auto& keybind : binding.keybinds)
        if (!keybind.page_id) {
            keybind.page_id = page_id;
            keybind.control_id = control_id;
            keybind.chord = chord;
            keybind.active = usable_now;
            keybind.label_bytes = copy_text(keybind.label, kLabelBytes, label);
            ++binding.runtime->keybind_revision;
            return true;
        }
    return false;
}

// ---- mc.ui.pages@1 ----------------------------------------------------------------

mc_status MC_CALL pages_register(void* handle, uint64_t generation, uint64_t page_id, mc_str label) {
    auto* binding = active(handle, generation, true);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_SETTINGS)) return MC_E_DENIED;
    if (!page_id || !text_ok(label, kLabelBytes - 1, true)) return MC_E_INVALID;
    if (find_page(*binding, page_id)) return MC_E_INVALID;
    Page* page = nullptr;
    for (auto& candidate : binding->pages)
        if (!candidate.registered) { page = &candidate; break; }
    if (!page) return MC_E_BUDGET;
    clear_bytes(page, sizeof(*page));
    page->page_id = page_id;
    page->label_bytes = copy_text(page->label, kLabelBytes, view(label));
    page->title = {page->label, page->label_bytes};
    page->controls = {page->bank, 0};
    page->revision = 1;
    page->format = kNativePluginSettingsFormatHost;
    const auto& services = binding->services;
    const auto registered = services.register_page(services.binding, services.generation, page_id,
                                                   {page->label, page->label_bytes}, page);
    if (!registered.ok()) {
        clear_bytes(page, sizeof(*page));
        return status_of(registered);
    }
    page->registered = true;
    return MC_OK;
}
mc_status MC_CALL pages_unregister(void* handle, uint64_t generation, uint64_t page_id) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_SETTINGS)) return MC_E_DENIED;
    Page* page = find_page(*binding, page_id);
    if (!page) return MC_E_NOT_FOUND;
    const auto& services = binding->services;
    const auto removed = services.unregister_page(services.binding, services.generation, page_id);
    if (!removed.ok()) return status_of(removed);
    keybinds_clear_page(*binding, page_id);
    ++binding->runtime->keybind_revision;
    clear_bytes(page, sizeof(*page));
    return MC_OK;
}
constexpr mc_pages_v1 kPages{sizeof(mc_pages_v1), 1, pages_register, pages_unregister};

// ---- mc.settings@3 ----------------------------------------------------------------

bool read_setting(const mc_settings_page& page, u32 index, mc_setting* output) noexcept {
    const auto* element = reinterpret_cast<const mc_setting*>(reinterpret_cast<const u8*>(page.controls) +
                                                              static_cast<usize>(index) * page.control_stride);
    return read_prefix(element, output, offsetof(mc_setting, label), page.control_stride);
}
struct Demand {
    u32 text, views;
};
bool add(Demand& demand, u32 text, u32 views = 0) noexcept {
    demand.text += text;
    demand.views += views;
    return demand.text <= kPageTextBytes && demand.views <= kPageViews;
}
bool slider_steps(const mc_setting& control, u32* steps) noexcept {
    if (control.step <= 0 || control.minimum >= control.maximum) return false;
    const i64 span = static_cast<i64>(control.maximum) - control.minimum;
    if (span % control.step) return false;
    const i64 count = span / control.step + 1;
    if (count < 2 || count > kPluginSdkSliderSteps) return false;
    *steps = static_cast<u32>(count);
    return true;
}
u32 slider_selected(const mc_setting& control, u32 steps) noexcept {
    if (control.value <= control.minimum) return 0;
    const i64 index = (static_cast<i64>(control.value) - control.minimum + control.step / 2) / control.step;
    return index >= steps ? steps - 1 : static_cast<u32>(index);
}
// Byte limit of an edited value: text honours max_bytes, keybinds and colours are short.
u32 edit_limit(const mc_setting& control) noexcept {
    return control.kind == MC_SETTING_TEXT ? (control.max_bytes ? minimum(control.max_bytes, kTextMaximum) : 256) : 63;
}
constexpr Utf8View kEditLabel = utf8_literal("Edit");
constexpr Utf8View kNotSet = utf8_literal("Not set");
constexpr Utf8View kSeparator = utf8_literal(" \xC2\xB7 ");
constexpr Utf8View kConflictPrefix = utf8_literal("Conflicts with ");

// Validates the control and accounts its arena demand without writing.
mc_status validate_setting(const mc_setting& control, Demand& demand) noexcept {
    if (!control.id || !text_ok(control.label, kLabelBytes, true) ||
        !text_ok(control.description, kDescriptionBytes, false) || !text_ok(control.action_label, kActionBytes, false))
        return MC_E_INVALID;
    if (!add(demand, control.label.bytes + control.description.bytes + control.action_label.bytes))
        return MC_E_BUDGET;
    switch (control.kind) {
    case MC_SETTING_TOGGLE:
    case MC_SETTING_HEADER:
    case MC_SETTING_NOTICE: return MC_OK;
    case MC_SETTING_ACTION: return control.action_label.bytes ? MC_OK : MC_E_INVALID;
    case MC_SETTING_CHOICE: {
        if (!control.choice_count || control.choice_count > kPluginSdkChoices || !control.choices ||
            control.selected >= control.choice_count) return MC_E_INVALID;
        for (u32 i = 0; i < control.choice_count; ++i) {
            mc_str choice{};
            copy_bytes(&choice, control.choices + i, sizeof(choice));
            if (!text_ok(choice, kChoiceBytes, true)) return MC_E_INVALID;
            if (!add(demand, choice.bytes)) return MC_E_BUDGET;
        }
        return add(demand, 0, control.choice_count) ? MC_OK : MC_E_BUDGET;
    }
    case MC_SETTING_TEXT:
    case MC_SETTING_KEYBIND: {
        if (!text_ok(control.text, edit_limit(control), false)) return MC_E_INVALID;
        u32 conflict = 0;
        if (control.kind == MC_SETTING_KEYBIND && control.text.bytes) {
            char normal[64];
            u32 bytes{};
            if (!native_plugin_sdk_keybind_normalize(view(control.text), normal, &bytes)) return MC_E_INVALID;
            conflict = kSeparator.bytes + kConflictPrefix.bytes + kConflictBytes;
        }
        return add(demand, 2 * maximum(control.text.bytes, kNotSet.bytes) + kSeparator.bytes + kEditLabel.bytes + conflict)
                   ? MC_OK : MC_E_BUDGET;
    }
    case MC_SETTING_COLOUR:
        if (control.colour > 0xFFFFFFu) return MC_E_INVALID;
        return add(demand, 14 + kSeparator.bytes + kEditLabel.bytes) ? MC_OK : MC_E_BUDGET;
    case MC_SETTING_SLIDER: {
        u32 steps{};
        if (!slider_steps(control, &steps)) return MC_E_INVALID;
        return add(demand, 24, steps) ? MC_OK : MC_E_BUDGET;
    }
    default: return MC_E_UNSUPPORTED;
    }
}
struct Writer {
    Page& page;
    Utf8View text(Utf8View value) noexcept {
        char* at = page.text + page.text_used;
        copy_bytes(at, value.data, value.bytes);
        page.text_used += value.bytes;
        return {at, value.bytes};
    }
    Utf8View join(Utf8View first, Utf8View second) noexcept {
        if (!first.bytes) return text(second);
        char* at = page.text + page.text_used;
        text(first);
        text(kSeparator);
        text(second);
        return {at, first.bytes + kSeparator.bytes + second.bytes};
    }
    Utf8View* views(u32 count) noexcept {
        Utf8View* at = page.views + page.views_used;
        for (u32 i = 0; i < count; ++i) reset_value(&at[i]);
        page.views_used += count;
        return at;
    }
};
void write_setting(NativePluginSdkBinding& binding, Writer& writer, const mc_setting& control, u32 index) noexcept {
    Page& page = writer.page;
    auto& out = page.bank[index];
    auto& extra = page.extra[index];
    reset_value(&out);
    reset_value(&extra);
    extra.sdk_kind = control.kind;
    out.id = control.id;
    out.label = writer.text(view(control.label));
    out.enabled = control.enabled != 0;
    const Utf8View description = view(control.description);
    switch (control.kind) {
    case MC_SETTING_TOGGLE:
        out.kind = NativePluginSettingsKind::toggle;
        out.checked = control.checked != 0;
        out.description = writer.text(description);
        break;
    case MC_SETTING_CHOICE: {
        out.kind = NativePluginSettingsKind::choice;
        out.description = writer.text(description);
        Utf8View* choices = writer.views(control.choice_count);
        for (u32 i = 0; i < control.choice_count; ++i) {
            mc_str choice{};
            copy_bytes(&choice, control.choices + i, sizeof(choice));
            choices[i] = writer.text(view(choice));
        }
        out.choices = {choices, control.choice_count};
        out.selected_choice = control.selected;
        break;
    }
    case MC_SETTING_ACTION:
        out.kind = NativePluginSettingsKind::action;
        out.description = writer.text(description);
        out.action_label = writer.text(view(control.action_label));
        break;
    case MC_SETTING_HEADER:
        out.kind = NativePluginSettingsKind::category;
        out.description = writer.text(description);
        break;
    case MC_SETTING_NOTICE:
        out.kind = NativePluginSettingsKind::notice;
        out.description = writer.text(description);
        out.action_label = writer.text(view(control.action_label));
        break;
    case MC_SETTING_TEXT:
    case MC_SETTING_KEYBIND:
    case MC_SETTING_COLOUR: {
        out.kind = NativePluginSettingsKind::action;
        out.action_label = control.action_label.bytes ? writer.text(view(control.action_label)) : kEditLabel;
        Utf8View value = view(control.text);
        char normal[64];
        char colour[7];
        Utf8View conflict{};
        char conflict_buffer[kConflictBytes];
        if (control.kind == MC_SETTING_KEYBIND && value.bytes) {
            u32 bytes{};
            native_plugin_sdk_keybind_normalize(value, normal, &bytes);
            value = {normal, bytes};
            NativePluginSdkChord chord{};
            if (permitted(binding, MC_PERM_KEYBINDS)) {
                if (!native_plugin_sdk_keybind_chord(value, &chord)) conflict = utf8_literal("Win shortcuts (not supported)");
                else conflict = keybind_resolve(binding, page.page_id, control.id, chord, conflict_buffer);
                if (control.enabled && !keybind_add(binding, page.page_id, control.id, chord, !conflict.bytes, view(control.label)))
                    conflict = utf8_literal("too many shortcuts");
            }
        } else if (control.kind == MC_SETTING_COLOUR) {
            value = {colour, color_hex_format(control.colour << 8, false, {colour, sizeof(colour)})};
        }
        extra.value = writer.text(value);
        extra.max_bytes = edit_limit(control);
        out.description = writer.join(description, extra.value.bytes ? extra.value : kNotSet);
        if (conflict.bytes) {
            writer.text(kSeparator);
            writer.text(kConflictPrefix);
            writer.text({conflict.data, utf8_truncated_length(conflict, kConflictBytes)});
            out.description.bytes = static_cast<u32>(page.text + page.text_used - out.description.data);
        }
        break;
    }
    case MC_SETTING_SLIDER: {
        u32 steps{};
        slider_steps(control, &steps);
        const u32 selected = slider_selected(control, steps);
        out.kind = NativePluginSettingsKind::slider;
        out.description = writer.text(description);
        Utf8View* choices = writer.views(steps);
        char digits[24];
        const i64 value = static_cast<i64>(control.minimum) + static_cast<i64>(selected) * control.step;
        const u32 bytes = decimal_write_signed(digits, value);
        choices[selected] = writer.text({digits, bytes});
        out.choices = {choices, steps};
        out.selected_choice = selected;
        extra.minimum = control.minimum;
        extra.step = control.step;
        break;
    }
    }
}

mc_status MC_CALL settings_set_page(void* handle, uint64_t generation, uint64_t page_id, const mc_settings_page* source) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_SETTINGS)) return MC_E_DENIED;
    Page* page = find_page(*binding, page_id);
    if (!page) return MC_E_NOT_FOUND;
    mc_settings_page header{};
    if (!read_prefix(source, &header, offsetof(mc_settings_page, reserved))) return MC_E_INVALID;
    if (header.control_count > kPluginSdkControls) return MC_E_BUDGET;
    if (header.control_count &&
        (!header.controls || header.control_stride < offsetof(mc_setting, label))) return MC_E_INVALID;
    if (!text_ok(header.title, kLabelBytes - 1, false) || !text_ok(header.description, kDescriptionBytes, false))
        return MC_E_INVALID;
    // Validate everything before the first write so a refused page keeps the old one.
    Demand demand{header.title.bytes + header.description.bytes, 0};
    if (!add(demand, 0)) return MC_E_BUDGET;
    for (u32 i = 0; i < header.control_count; ++i) {
        mc_setting control{};
        if (!read_setting(header, i, &control)) return MC_E_INVALID;
        const mc_status checked = validate_setting(control, demand);
        if (checked != MC_OK) return checked;
        for (u32 j = 0; j < i; ++j) {
            mc_setting previous{};
            read_setting(header, j, &previous);
            if (previous.id == control.id) return MC_E_INVALID;
        }
    }
    keybinds_clear_page(*binding, page_id);
    page->text_used = 0;
    page->views_used = 0;
    Writer writer{*page};
    page->title = header.title.bytes ? writer.text(view(header.title)) : Utf8View{page->label, page->label_bytes};
    page->description = writer.text(view(header.description));
    for (u32 i = 0; i < header.control_count; ++i) {
        mc_setting control{};
        read_setting(header, i, &control);
        write_setting(*binding, writer, control, i);
    }
    page->controls = {page->bank, header.control_count};
    if (page->revision != ~u32{0}) ++page->revision;
    ++binding->runtime->keybind_revision;
    return MC_OK;
}
mc_status MC_CALL settings_set_handler(void* handle, uint64_t generation, mc_setting_fn handler) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_SETTINGS)) return MC_E_DENIED;
    binding->setting_handler = handler;
    return MC_OK;
}
mc_status MC_CALL settings_set_keybind_handler(void* handle, uint64_t generation, mc_keybind_fn handler) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted_all(*binding, MC_PERM_SETTINGS | MC_PERM_KEYBINDS)) return MC_E_DENIED;
    binding->keybind_handler = handler;
    ++binding->runtime->keybind_revision;
    return MC_OK;
}
constexpr mc_settings_v3 kSettings{sizeof(mc_settings_v3), 3, settings_set_page, settings_set_handler,
                                   settings_set_keybind_handler};

// ---- mc.msg.hooks@2 ---------------------------------------------------------------

mc_status MC_CALL hooks_set_pre_send(void* handle, uint64_t generation, mc_pre_send_fn handler) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_MESSAGE_SEND)) return MC_E_DENIED;
    binding->pre_send = handler;
    return MC_OK;
}
constexpr mc_msg_hooks_v2 kHooks{sizeof(mc_msg_hooks_v2), 2, hooks_set_pre_send};

// ---- mc.cmd@1 -----------------------------------------------------------------------

mc_status MC_CALL cmd_register(void* handle, uint64_t generation, const mc_command* source) {
    auto* binding = active(handle, generation, true);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_COMMANDS)) return MC_E_DENIED;
    mc_command command{};
    if (!read_prefix(source, &command, offsetof(mc_command, options))) return MC_E_INVALID;
    if (!command.command_id || !command_name_ok(command.name) ||
        !text_ok(command.description, kCommandDescriptionBytes, false) ||
        command.option_count > kPluginSdkCommandOptions ||
        (command.option_count && (!command.options || command.option_stride < offsetof(mc_command_option, description))))
        return MC_E_INVALID;
    for (u32 i = 0; i < binding->command_count; ++i) {
        const auto& existing = binding->commands[i];
        if (existing.id == command.command_id ||
            utf8_equal({existing.name, existing.name_bytes}, view(command.name))) return MC_E_INVALID;
    }
    if (binding->command_count == kPluginSdkCommands) return MC_E_BUDGET;
    Command candidate{};
    for (u32 i = 0; i < command.option_count; ++i) {
        mc_command_option option{};
        const auto* element = reinterpret_cast<const mc_command_option*>(
            reinterpret_cast<const u8*>(command.options) + static_cast<usize>(i) * command.option_stride);
        if (!read_prefix(element, &option, offsetof(mc_command_option, description), command.option_stride) ||
            option.type > MC_OPTION_BOOLEAN || !command_name_ok(option.name) ||
            !text_ok(option.description, kCommandDescriptionBytes, false)) return MC_E_INVALID;
        auto& out = candidate.options[i];
        out.type = option.type;
        out.required = option.required != 0;
        out.name_bytes = copy_text(out.name, kCommandNameBytes, view(option.name));
        out.description_bytes = copy_text(out.description, kCommandDescriptionBytes, view(option.description));
    }
    candidate.id = command.command_id;
    candidate.option_count = command.option_count;
    candidate.name_bytes = copy_text(candidate.name, kCommandNameBytes, view(command.name));
    candidate.description_bytes = copy_text(candidate.description, kCommandDescriptionBytes, view(command.description));
    copy_value(&binding->commands[binding->command_count++], candidate);
    return MC_OK;
}
mc_status MC_CALL cmd_unregister(void* handle, uint64_t generation, uint64_t command_id) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    for (u32 i = 0; i < binding->command_count; ++i)
        if (binding->commands[i].id == command_id) {
            for (u32 j = i + 1; j < binding->command_count; ++j) binding->commands[j - 1] = binding->commands[j];
            reset_value(&binding->commands[--binding->command_count]);
            return MC_OK;
        }
    return MC_E_NOT_FOUND;
}
mc_status MC_CALL cmd_set_handler(void* handle, uint64_t generation, mc_command_fn handler) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_COMMANDS)) return MC_E_DENIED;
    binding->command_handler = handler;
    return MC_OK;
}
constexpr mc_cmd_v1 kCommands{sizeof(mc_cmd_v1), 1, cmd_register, cmd_unregister, cmd_set_handler};

// ---- mc.store@1 ---------------------------------------------------------------------

static constexpr u8 kStoreMagic[8]{'M', 'C', 'S', 'T', 'O', 'R', 'E', '1'};
constexpr u32 kStoreHeaderBytes = 24;
constexpr u32 kStoreFileBytes = kStoreHeaderBytes + kPluginSdkStoreKeys * 3 + kPluginSdkStoreBytes;

bool store_path(const NativePluginSdkBinding& binding, wchar_t (&path)[600], bool create) noexcept {
    const auto& runtime = *binding.runtime;
    const u32 base = runtime.data_directory_characters;
    if (!base || base >= 512) return false;
    copy_bytes(path, runtime.data_directory, base * 2);
    path[base] = 0;
    if (create) CreateDirectoryW(path, nullptr);
    u32 at = base;
    path[at++] = L'\\';
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    for (u32 i = 0; i < 16; ++i) path[at++] = digits[(binding.owner_id >> (60 - 4 * i)) & 15];
    path[at] = 0;
    if (create) CreateDirectoryW(path, nullptr);
    static constexpr wchar_t leaf[] = L"\\store.bin";
    for (u32 i = 0; i < sizeof(leaf) / sizeof(leaf[0]); ++i) path[at + i] = leaf[i];
    return true;
}
bool key_ok(mc_str key) noexcept {
    if (!key.bytes || key.bytes > kStoreKeyBytes || !key.data) return false;
    for (u32 i = 0; i < key.bytes; ++i)
        if (key.data[i] < 0x20 || key.data[i] > 0x7e) return false;
    return true;
}
bool store_decode(Store& store, const u8* file, u32 bytes) noexcept {
    if (bytes < kStoreHeaderBytes || !bytes_equal(file, kStoreMagic, 8)) return false;
    const u32 count = byte_load_le32(file + 8), payload = byte_load_le32(file + 12);
    if (count > kPluginSdkStoreKeys || payload != bytes - kStoreHeaderBytes ||
        byte_load_le64(file + 16) != hash64_bytes(kHash64Seed, file + kStoreHeaderBytes, payload)) return false;
    Store candidate{};
    u32 at = kStoreHeaderBytes;
    for (u32 i = 0; i < count; ++i) {
        if (bytes - at < 3) return false;
        const u32 key = file[at], value = static_cast<u32>(file[at + 1]) | static_cast<u32>(file[at + 2]) << 8;
        at += 3;
        if (!key || key > kStoreKeyBytes || value > kPluginSdkStoreValueBytes || bytes - at < key + value ||
            candidate.used + key + value > kPluginSdkStoreBytes ||
            !key_ok({reinterpret_cast<const char*>(file + at), key})) return false;
        candidate.entries[i] = {static_cast<u16>(candidate.used), static_cast<u16>(value), static_cast<u8>(key)};
        copy_bytes(candidate.data + candidate.used, file + at, key + value);
        candidate.used += key + value;
        at += key + value;
    }
    if (at != bytes) return false;
    candidate.count = count;
    candidate.loaded = true;
    store = candidate;
    return true;
}
void store_load(NativePluginSdkBinding& binding) noexcept {
    auto& store = binding.store;
    if (store.loaded) return;
    reset_value(&store);
    store.loaded = true;
    wchar_t path[600];
    if (!store_path(binding, path, false)) return;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    u8 buffer[kStoreFileBytes];
    DWORD read = 0;
    Win32LargeInteger size{};
    const bool ok = GetFileSizeEx(file, size.native()) && size.QuadPart <= kStoreFileBytes &&
                    ReadFile(file, buffer, static_cast<DWORD>(size.QuadPart), &read, nullptr) &&
                    read == static_cast<DWORD>(size.QuadPart);
    CloseHandle(file);
    // A damaged store starts empty; the next write replaces it.
    if (ok) store_decode(store, buffer, read);
}
bool store_save(NativePluginSdkBinding& binding) noexcept {
    auto& store = binding.store;
    if (!store.loaded || !store.dirty) return true;
    wchar_t path[600], temporary[608];
    if (!store_path(binding, path, true)) {
        store.dirty = false; // memory-only runtime
        return true;
    }
    u8 buffer[kStoreFileBytes];
    copy_bytes(buffer, kStoreMagic, 8);
    u32 at = kStoreHeaderBytes;
    for (u32 i = 0; i < store.count; ++i) {
        const auto& entry = store.entries[i];
        buffer[at] = entry.key_bytes;
        buffer[at + 1] = static_cast<u8>(entry.value_bytes);
        buffer[at + 2] = static_cast<u8>(entry.value_bytes >> 8);
        at += 3;
        copy_bytes(buffer + at, store.data + entry.offset, static_cast<u32>(entry.key_bytes) + entry.value_bytes);
        at += static_cast<u32>(entry.key_bytes) + entry.value_bytes;
    }
    byte_store_le32(buffer + 8, store.count);
    byte_store_le32(buffer + 12, at - kStoreHeaderBytes);
    byte_store_le64(buffer + 16, hash64_bytes(kHash64Seed, buffer + kStoreHeaderBytes, at - kStoreHeaderBytes));
    u32 length = 0;
    while (path[length]) ++length;
    copy_bytes(temporary, path, length * 2);
    static constexpr wchar_t suffix[] = L".new";
    for (u32 i = 0; i < 5; ++i) temporary[length + i] = suffix[i];
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool wrote = WriteFile(file, buffer, at, &written, nullptr) && written == at;
    CloseHandle(file);
    if (!wrote || !MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary);
        return false;
    }
    store.dirty = false;
    return true;
}
u32 store_find(const Store& store, mc_str key) noexcept {
    for (u32 i = 0; i < store.count; ++i) {
        const auto& entry = store.entries[i];
        if (entry.key_bytes == key.bytes && bytes_equal(store.data + entry.offset, key.data, key.bytes)) return i;
    }
    return ~0u;
}
void store_erase(Store& store, u32 index) noexcept {
    const auto removed = store.entries[index];
    const u32 bytes = static_cast<u32>(removed.key_bytes) + removed.value_bytes;
    copy_bytes(store.data + removed.offset, store.data + removed.offset + bytes, store.used - removed.offset - bytes);
    store.used -= bytes;
    for (u32 i = index + 1; i < store.count; ++i) store.entries[i - 1] = store.entries[i];
    reset_value(&store.entries[--store.count]);
    for (u32 i = 0; i < store.count; ++i)
        if (store.entries[i].offset > removed.offset)
            store.entries[i].offset = static_cast<u16>(store.entries[i].offset - bytes);
}
NativePluginSdkBinding* store_binding(void* handle, uint64_t generation, mc_status* status) noexcept {
    auto* binding = active(handle, generation);
    *status = !binding ? MC_E_INACTIVE : !permitted(*binding, MC_PERM_STORE) ? MC_E_DENIED : MC_OK;
    if (*status != MC_OK) return nullptr;
    store_load(*binding);
    return binding;
}
mc_status MC_CALL store_get(void* handle, uint64_t generation, mc_str key, void* buffer, uint32_t capacity,
                            uint32_t* bytes) {
    mc_status status{};
    auto* binding = store_binding(handle, generation, &status);
    if (!binding) return status;
    if (!key_ok(key) || !bytes) return MC_E_INVALID;
    *bytes = 0;
    const auto& store = binding->store;
    const u32 index = store_find(store, key);
    if (index == ~0u) return MC_E_NOT_FOUND;
    const auto& entry = store.entries[index];
    *bytes = entry.value_bytes;
    if (capacity < entry.value_bytes) return MC_E_TRUNCATED;
    if (entry.value_bytes && !buffer) return MC_E_INVALID;
    copy_bytes(buffer, store.data + entry.offset + entry.key_bytes, entry.value_bytes);
    return MC_OK;
}
mc_status MC_CALL store_set(void* handle, uint64_t generation, mc_str key, const void* value, uint32_t bytes) {
    mc_status status{};
    auto* binding = store_binding(handle, generation, &status);
    if (!binding) return status;
    if (!key_ok(key) || (bytes && !value)) return MC_E_INVALID;
    if (bytes > kPluginSdkStoreValueBytes) return MC_E_CAPACITY;
    auto& store = binding->store;
    const u32 index = store_find(store, key);
    const u32 previous = index == ~0u ? 0 : static_cast<u32>(store.entries[index].key_bytes) + store.entries[index].value_bytes;
    if (store.used - previous + key.bytes + bytes > kPluginSdkStoreBytes ||
        (index == ~0u && store.count == kPluginSdkStoreKeys)) return MC_E_BUDGET;
    u8 copy[kPluginSdkStoreValueBytes];
    copy_bytes(copy, value, bytes);
    if (index != ~0u) store_erase(store, index);
    store.entries[store.count++] = {static_cast<u16>(store.used), static_cast<u16>(bytes), static_cast<u8>(key.bytes)};
    copy_bytes(store.data + store.used, key.data, key.bytes);
    copy_bytes(store.data + store.used + key.bytes, copy, bytes);
    store.used += key.bytes + bytes;
    store.dirty = true;
    return MC_OK;
}
mc_status MC_CALL store_remove(void* handle, uint64_t generation, mc_str key) {
    mc_status status{};
    auto* binding = store_binding(handle, generation, &status);
    if (!binding) return status;
    if (!key_ok(key)) return MC_E_INVALID;
    const u32 index = store_find(binding->store, key);
    if (index == ~0u) return MC_E_NOT_FOUND;
    store_erase(binding->store, index);
    binding->store.dirty = true;
    return MC_OK;
}
mc_status MC_CALL store_usage(void* handle, uint64_t generation, uint32_t* used, uint32_t* limit) {
    mc_status status{};
    auto* binding = store_binding(handle, generation, &status);
    if (!binding) return status;
    if (used) *used = binding->store.used;
    if (limit) *limit = kPluginSdkStoreBytes;
    return MC_OK;
}
constexpr mc_store_v1 kStore{sizeof(mc_store_v1), 1, store_get, store_set, store_remove, store_usage};

// ---- mc.timer@1 -------------------------------------------------------------------------

mc_status MC_CALL timer_set_handler(void* handle, uint64_t generation, mc_timer_fn handler) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_TIMER)) return MC_E_DENIED;
    binding->timer_handler = handler;
    return MC_OK;
}
mc_status MC_CALL timer_start(void* handle, uint64_t generation, uint64_t id, uint32_t interval, uint32_t repeat) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_TIMER)) return MC_E_DENIED;
    if (!id || interval < kPluginSdkTimerMinimumMs) return MC_E_INVALID;
    Timer* timer = nullptr;
    for (auto& candidate : binding->timers)
        if (candidate.active && candidate.id == id) timer = &candidate;
    if (!timer)
        for (auto& candidate : binding->timers)
            if (!candidate.active) { timer = &candidate; break; }
    if (!timer) return MC_E_BUDGET;
    *timer = {id, now_ms(*binding->runtime) + interval, interval, repeat != 0, true};
    return MC_OK;
}
mc_status MC_CALL timer_cancel(void* handle, uint64_t generation, uint64_t id) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    for (auto& timer : binding->timers)
        if (timer.active && timer.id == id) {
            reset_value(&timer);
            return MC_OK;
        }
    return MC_E_NOT_FOUND;
}
constexpr mc_timer_v1 kTimers{sizeof(mc_timer_v1), 1, timer_set_handler, timer_start, timer_cancel};

// ---- Interface lookup ----------------------------------------------------------------

constexpr u64 kReadPermissions = MC_PERM_READ_CONTEXT | MC_PERM_READ_GUILDS | MC_PERM_READ_MEMBERS |
                                 MC_PERM_READ_PRESENCE | MC_PERM_READ_VOICE | MC_PERM_READ_MESSAGES;
constexpr u64 kActionPermissions = MC_PERM_ACT_MESSAGES | MC_PERM_ACT_PRESENCE | MC_PERM_NAVIGATE |
                                   MC_PERM_CLIPBOARD | MC_PERM_OPEN_LINKS | MC_PERM_NOTIFY;
struct InterfaceEntry {
    Utf8View name;
    u32 version;
    u64 permissions; // any of them grants the table
    const void* table;
};
const InterfaceEntry kInterfaces[]{
    {utf8_literal("mc.ui.pages"), 1, MC_PERM_SETTINGS, &kPages},
    {utf8_literal("mc.settings"), 3, MC_PERM_SETTINGS, &kSettings},
    {utf8_literal("mc.ui.menus"), 2, MC_PERM_MENUS, &kMenus},
    {utf8_literal("mc.msg.hooks"), 2, MC_PERM_MESSAGE_SEND, &kHooks},
    {utf8_literal("mc.cmd"), 1, MC_PERM_COMMANDS, &kCommands},
    {utf8_literal("mc.ui.slots"), 2, MC_PERM_UI_SLOTS, &kSlots},
    {utf8_literal("mc.store"), 1, MC_PERM_STORE, &kStore},
    {utf8_literal("mc.read"), 2, kReadPermissions, &kRead},
    {utf8_literal("mc.events"), 1, MC_PERM_READ_GUILDS | MC_PERM_READ_MEMBERS | MC_PERM_READ_PRESENCE |
                                       MC_PERM_READ_VOICE | MC_PERM_READ_MESSAGES, &kEvents},
    {utf8_literal("mc.msg.render"), 1, MC_PERM_MESSAGE_RENDER, &kRender},
    {utf8_literal("mc.actions"), 1, kActionPermissions, &kActions},
    {utf8_literal("mc.timer"), 1, MC_PERM_TIMER, &kTimers},
    {utf8_literal("mc.fx"), 1, MC_PERM_EFFECTS, &kFx},
};
mc_status MC_CALL get_interface(void* handle, uint64_t generation, const char* name, uint32_t version,
                                const void** table) {
    if (table) *table = nullptr;
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!name || !table) return MC_E_INVALID;
    u32 bytes = 0;
    while (bytes <= 32 && name[bytes]) ++bytes;
    if (bytes > 32) return MC_E_INVALID;
    for (const auto& entry : kInterfaces) {
        if (!utf8_equal(entry.name, {name, bytes})) continue;
        if (version != entry.version) return MC_E_UNSUPPORTED;
        if (!permitted(*binding, entry.permissions)) return MC_E_DENIED;
        *table = entry.table;
        return MC_OK;
    }
    return MC_E_UNSUPPORTED;
}

// ---- Adapter trampolines (the host slot's entry points) ----------------------

NativePluginSdkBinding* from_state(void* state) noexcept {
    if (!state) return nullptr;
    StateHeader header{};
    copy_bytes(&header, state, sizeof(header));
    auto* binding = header.magic == kHeaderMagic ? live(header.binding) : nullptr;
    return binding && binding->plugin_state == static_cast<u8*>(state) + kPluginSdkStateHeaderBytes ? binding : nullptr;
}
void reset_registrations(NativePluginSdkBinding& binding) noexcept {
    surfaces_reset(binding);
    data_reset(binding);
    bool keybinds = false;
    for (const auto& keybind : binding.keybinds) keybinds = keybinds || keybind.page_id;
    if (keybinds) ++binding.runtime->keybind_revision;
    binding.setting_handler = nullptr;
    binding.keybind_handler = nullptr;
    binding.pre_send = nullptr;
    binding.command_handler = nullptr;
    binding.timer_handler = nullptr;
    clear_bytes(binding.pages, sizeof(binding.pages));
    clear_bytes(binding.keybinds, sizeof(binding.keybinds));
    clear_bytes(binding.commands, sizeof(binding.commands));
    clear_bytes(binding.timers, sizeof(binding.timers));
    clear_bytes(binding.buckets, sizeof(binding.buckets));
    binding.command_count = 0;
}
bool WINAPI adapter_start(NativePluginServices services, void* state, u32 state_bytes) noexcept {
    const auto* slot = static_cast<const NativePluginSlot*>(services.binding);
    auto* runtime = slot && slot->host ? slot->host->sdk : nullptr;
    auto* binding = runtime ? live(runtime->pending) : nullptr;
    if (!binding || binding->phase != Phase::queried || !state ||
        state_bytes < kPluginSdkStateHeaderBytes + binding->plugin_state_bytes) return false;
    runtime->pending = nullptr;
    const StateHeader header{kHeaderMagic, binding};
    copy_bytes(state, &header, sizeof(header));
    binding->services = services;
    binding->generation = services.generation;
    binding->plugin_state = static_cast<u8*>(state) + kPluginSdkStateHeaderBytes;
    binding->phase = Phase::starting;
    binding->start_attempted = true;
    // Events queued before this plugin started are not its history.
    binding->event_next = runtime->event_head;
    const mc_host host{sizeof(mc_host), kPluginSdkHostAbi, binding, binding->generation, get_interface};
    const mc_status status = binding->definition.start(&host, binding->plugin_state, binding->plugin_state_bytes);
    if (status != MC_OK || binding->phase != Phase::starting) return false;
    binding->phase = Phase::enabled;
    return true;
}
void WINAPI adapter_stop(void* state, u32) noexcept {
    auto* binding = from_state(state);
    if (!binding) return;
    binding->phase = Phase::stopping;
    if (binding->start_attempted && binding->definition.stop)
        binding->definition.stop(binding->plugin_state, binding->plugin_state_bytes);
    store_save(*binding);
    reset_value(&binding->store);
    reset_registrations(*binding);
    binding->start_attempted = false;
    binding->plugin_state = nullptr;
    binding->generation = 0;
    reset_value(&binding->services);
    binding->phase = Phase::empty;
    clear_bytes(state, sizeof(StateHeader));
}
NativePluginActivationResult WINAPI adapter_settings(void* state, u32, NativePluginSettingsActivation activation) noexcept {
    auto* binding = from_state(state);
    if (!binding || binding->phase != Phase::enabled || !binding->setting_handler) return {false, false};
    Page* page = find_page(*binding, activation.target.page.page_id);
    const u32 index = page ? find_control(*page, activation.target.control_id) : ~0u;
    if (index == ~0u) return {false, false};
    const auto& extra = page->extra[index];
    mc_setting_value value{};
    value.size = sizeof(value);
    value.kind = extra.sdk_kind;
    value.page_id = page->page_id;
    value.control_id = activation.target.control_id;
    switch (extra.sdk_kind) {
    case MC_SETTING_TOGGLE: value.checked = activation.checked ? 1u : 0u; break;
    case MC_SETTING_CHOICE: value.selected = activation.selected_choice; break;
    case MC_SETTING_SLIDER:
        value.selected = activation.selected_choice;
        value.value = static_cast<i32>(extra.minimum + static_cast<i64>(activation.selected_choice) * extra.step);
        break;
    case MC_SETTING_ACTION:
    case MC_SETTING_NOTICE: break;
    default: return {false, false}; // edited through native_plugin_sdk_settings_submit
    }
    const u32 revision = page->revision;
    Gesture gesture(binding->runtime);
    const mc_status status = binding->setting_handler(binding->plugin_state, binding->plugin_state_bytes, &value);
    return {status == MC_OK, page->revision != revision};
}

// ---- Command parsing ------------------------------------------------------------------

Utf8View command_name(Utf8View text, Utf8View* rest) noexcept {
    if (rest) reset_value(rest);
    if (!text.bytes || text.data[0] != '/') return {};
    u32 end = 1;
    while (end < text.bytes && !ascii_is_space(text.data[end])) ++end;
    u32 at = end;
    while (at < text.bytes && ascii_is_space(text.data[at])) ++at;
    if (rest) *rest = {text.data + at, text.bytes - at};
    return {text.data + 1, end - 1};
}
struct CommandMatch {
    NativePluginSdkBinding* binding;
    const Command* command;
};
CommandMatch match_command(const NativePluginSdkRuntime& runtime, Utf8View name) noexcept {
    if (!name.bytes) return {};
    for (auto* binding : g_live) {
        if (!usable(binding, runtime)) continue;
        for (u32 i = 0; i < binding->command_count; ++i)
            if (utf8_equal({binding->commands[i].name, binding->commands[i].name_bytes}, name))
                return {binding, &binding->commands[i]};
    }
    return {};
}
bool parse_integer(Utf8View text, i64* value) noexcept {
    u32 at = 0;
    bool negative = false;
    if (text.bytes && (text.data[0] == '-' || text.data[0] == '+')) {
        negative = text.data[0] == '-';
        at = 1;
    }
    if (at == text.bytes) return false;
    u64 magnitude = 0;
    for (; at < text.bytes; ++at) {
        const char c = text.data[at];
        if (c < '0' || c > '9') return false;
        const u64 digit = static_cast<u64>(c - '0');
        if (magnitude > (0x8000'0000'0000'0000ull - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    if (!negative && magnitude > 0x7FFF'FFFF'FFFF'FFFFull) return false;
    *value = negative ? static_cast<i64>(0ull - magnitude) : static_cast<i64>(magnitude);
    return true;
}
bool parse_boolean(Utf8View text, bool* value) noexcept {
    static constexpr Utf8View yes[]{utf8_literal("true"), utf8_literal("yes"), utf8_literal("on"), utf8_literal("1")};
    static constexpr Utf8View no[]{utf8_literal("false"), utf8_literal("no"), utf8_literal("off"), utf8_literal("0")};
    for (auto word : yes)
        if (ascii_equal_ignore_case(text, word)) { *value = true; return true; }
    for (auto word : no)
        if (ascii_equal_ignore_case(text, word)) { *value = false; return true; }
    return false;
}
// Notices are composed into the second output buffer.
NativePluginSdkComposeResult notice(NativePluginSdkRuntime* runtime, u64 owner, Utf8View first, Utf8View second = {},
                                   Utf8View third = {}) noexcept {
    char* out = runtime->output[1];
    u32 at = 0;
    const Utf8View parts[]{first, second, third};
    for (auto part : parts) {
        const u32 bytes = utf8_truncated_length(part, kPluginSdkOutputBytes - at);
        copy_bytes(out + at, part.data, bytes);
        at += bytes;
    }
    return {NativePluginSdkComposeAction::notice, {out, at}, owner};
}
bool output_ok(const mc_text_output& output, u32 maximum_action) noexcept {
    return output.action <= maximum_action && output.bytes <= kPluginSdkOutputBytes;
}
// The key names of a keybind's normal form with their virtual keys.
struct NamedKey {
    Utf8View name;
    u8 code;
};
constexpr NamedKey kNamedKeys[]{
    {utf8_literal("Space"), 0x20}, {utf8_literal("Enter"), 0x0D}, {utf8_literal("Tab"), 0x09},
    {utf8_literal("Escape"), 0x1B}, {utf8_literal("Backspace"), 0x08}, {utf8_literal("Delete"), 0x2E},
    {utf8_literal("Insert"), 0x2D}, {utf8_literal("Home"), 0x24}, {utf8_literal("End"), 0x23},
    {utf8_literal("PageUp"), 0x21}, {utf8_literal("PageDown"), 0x22}, {utf8_literal("Up"), 0x26},
    {utf8_literal("Down"), 0x28}, {utf8_literal("Left"), 0x25}, {utf8_literal("Right"), 0x27}};

} // namespace

// ---- Public API ---------------------------------------------------------------------------

NativePluginSdkBinding* native_plugin_sdk_create(NativePluginSdkRuntime* runtime, u64 approved) noexcept {
    if (!runtime) return nullptr;
    u32 index = kPluginSdkBindings;
    for (u32 i = 0; i < kPluginSdkBindings; ++i)
        if (!g_live[i]) { index = i; break; }
    if (index == kPluginSdkBindings) return nullptr;
    auto* binding = static_cast<NativePluginSdkBinding*>(
        VirtualAlloc(nullptr, sizeof(NativePluginSdkBinding), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!binding) return nullptr;
    binding->magic = kBindingMagic;
    binding->runtime = runtime;
    binding->approved = approved & MC_PERM_ALL;
    g_live[index] = binding;
    return binding;
}

bool native_plugin_sdk_destroy(NativePluginSdkBinding* binding) noexcept {
    if (!binding) return true;
    if (!live(binding) || binding->phase == Phase::starting || binding->phase == Phase::enabled ||
        binding->phase == Phase::stopping) return false;
    for (auto*& slot : g_live)
        if (slot == binding) slot = nullptr;
    // The ledger tracks a bounded set of owners; a released plugin leaves it.
    if (binding->owner_id) plugin_call_ledger_forget(binding->runtime->ledger, binding->owner_id);
    if (binding->runtime->pending == binding) binding->runtime->pending = nullptr;
    binding->magic = 0;
    return VirtualFree(binding, 0, MEM_RELEASE) != FALSE;
}

NativePluginSdkQueryError native_plugin_sdk_query(NativePluginSdkBinding* binding, mc_query_fn query, u64 expected_owner,
                                                NativePluginDefinition* adapter) noexcept {
    if (adapter) reset_value(adapter);
    if (!live(binding) || !query || !adapter || binding->phase != Phase::empty) return NativePluginSdkQueryError::invalid;
    const mc_host_info info{sizeof(mc_host_info), kPluginSdkHostAbi};
    mc_plugin_definition definition{};
    definition.size = sizeof(definition);
    definition.abi_version = MC_ABI_VERSION;
    mc_status status = MC_E_INVALID;
    {
        PluginCallLedger* ledger = binding->runtime->ledger;
        const auto ticket = plugin_call_begin(ledger, expected_owner, PluginCallKind::query);
        status = query(&info, &definition);
        plugin_call_end(ledger, ticket);
    }
    if (status != MC_OK) return NativePluginSdkQueryError::query_failed;
    // Fields past the size the plugin reports are treated as zero.
    const u32 written = definition.size;
    if (written < offsetof(mc_plugin_definition, start)) return NativePluginSdkQueryError::invalid;
    if (written < sizeof(definition))
        clear_bytes(reinterpret_cast<u8*>(&definition) + written, static_cast<u32>(sizeof(definition)) - written);
    definition.size = sizeof(definition);
    if (definition.abi_version != MC_ABI_VERSION || definition.min_host_abi > kPluginSdkHostAbi)
        return NativePluginSdkQueryError::abi;
    if (!definition.owner_id) return NativePluginSdkQueryError::invalid;
    if (expected_owner && definition.owner_id != expected_owner) return NativePluginSdkQueryError::owner;
    if (definition.permissions & ~binding->approved) return NativePluginSdkQueryError::permissions;
    if (definition.state_bytes > kPluginSdkStateBytes) return NativePluginSdkQueryError::budget;
    if (!definition.state_alignment) definition.state_alignment = 16;
    if ((definition.state_alignment & (definition.state_alignment - 1)) || definition.state_alignment > 64 ||
        !definition.start || !definition.stop) return NativePluginSdkQueryError::invalid;
    binding->definition = definition;
    binding->owner_id = definition.owner_id;
    binding->plugin_state_bytes = definition.state_bytes;
    binding->phase = Phase::queried;
    // Menus, slots and everything else are SDK-owned; the slot host keeps pages.
    const u32 capabilities = (definition.permissions & MC_PERM_SETTINGS) ? native_plugin_capability_pages : 0u;
    adapter->owner_id = definition.owner_id;
    adapter->required_capabilities = capabilities;
    adapter->page_limit = capabilities ? kPluginSdkPages : 0;
    adapter->state_bytes = kPluginSdkStateHeaderBytes + (definition.state_bytes + 63u) / 64u * 64u;
    adapter->state_alignment = 64;
    adapter->start = adapter_start;
    adapter->stop = adapter_stop;
    adapter->activate_settings = adapter_settings;
    return NativePluginSdkQueryError::none;
}

NativePluginResult native_plugin_sdk_enable(NativePluginSdkBinding* binding, NativePluginHost* host,
                                           const NativePluginDefinition& adapter, Slice<u8> state) noexcept {
    if (!live(binding) || !host || host->sdk != binding->runtime || binding->phase != Phase::queried ||
        !native_plugin_sdk_adapter(adapter) || adapter.owner_id != binding->owner_id)
        return {NativePluginError::invalid_argument};
    plugin_call_ledger_forget(binding->runtime->ledger, binding->owner_id);
    binding->runtime->pending = binding;
    const auto result = native_plugin_host_enable(host, adapter, state);
    binding->runtime->pending = nullptr;
    return result;
}

bool native_plugin_sdk_adapter(const NativePluginDefinition& definition) noexcept {
    return definition.start == adapter_start && definition.stop == adapter_stop;
}
u64 native_plugin_sdk_permissions(const NativePluginSdkBinding* binding) noexcept {
    return live(binding) ? binding->definition.permissions : 0;
}
const NativePluginSdkBinding* native_plugin_sdk_find(const NativePluginSdkRuntime& runtime, u64 owner) noexcept {
    return binding_for(runtime, owner);
}

bool native_plugin_sdk_settings_edit(const NativePluginSdkRuntime& runtime, UiPageKey key, u64 control_id,
                                    NativePluginSdkEdit* edit) noexcept {
    if (edit) reset_value(edit);
    const auto* binding = binding_for(runtime, key.owner_id);
    const Page* page = binding ? find_page(*binding, key.page_id) : nullptr;
    const u32 index = page ? find_control(*page, control_id) : ~0u;
    if (!edit || index == ~0u || !page->bank[index].enabled) return false;
    const auto& extra = page->extra[index];
    if (extra.sdk_kind != MC_SETTING_TEXT && extra.sdk_kind != MC_SETTING_COLOUR && extra.sdk_kind != MC_SETTING_KEYBIND)
        return false;
    *edit = {extra.sdk_kind, extra.sdk_kind == MC_SETTING_COLOUR ? 7u : extra.max_bytes, page->bank[index].label,
             extra.value};
    return true;
}

NativePluginError native_plugin_sdk_settings_submit(NativePluginSdkRuntime* runtime, UiPageKey key, u64 control_id,
                                                   Utf8View text) noexcept {
    if (!runtime) return NativePluginError::invalid_argument;
    NativePluginSdkEdit edit{};
    if (!native_plugin_sdk_settings_edit(*runtime, key, control_id, &edit)) return NativePluginError::stale;
    auto* binding = binding_for(*runtime, key.owner_id);
    if (!callable(*binding, reinterpret_cast<const void*>(binding->setting_handler))) return NativePluginError::inactive;
    mc_setting_value value{};
    value.size = sizeof(value);
    value.kind = edit.kind;
    value.page_id = key.page_id;
    value.control_id = control_id;
    if (edit.kind == MC_SETTING_COLOUR) {
        if (!native_plugin_sdk_colour_parse(text, &value.colour)) return NativePluginError::invalid_argument;
    } else if (edit.kind == MC_SETTING_KEYBIND) {
        char normal[64];
        u32 bytes = 0;
        if (text.bytes && !native_plugin_sdk_keybind_normalize(text, normal, &bytes))
            return NativePluginError::invalid_argument;
        copy_bytes(runtime->input, normal, bytes);
        value.text = {runtime->input, bytes};
    } else {
        if (!text_ok({text.data, text.bytes}, edit.max_bytes, false)) return NativePluginError::invalid_argument;
        copy_bytes(runtime->input, text.data, text.bytes);
        value.text = {runtime->input, text.bytes};
    }
    mc_status status{};
    {
        Call call(*binding, PluginCallKind::settings);
        Gesture gesture(runtime);
        status = binding->setting_handler(binding->plugin_state, binding->plugin_state_bytes, &value);
    }
    return status == MC_OK ? NativePluginError::none : NativePluginError::activation_failed;
}

bool native_plugin_sdk_keybind_normalize(Utf8View text, char (&output)[64], u32* bytes) noexcept {
    if (!bytes || !text.bytes || text.bytes > 63) return false;
    static constexpr Utf8View modifiers[]{utf8_literal("Ctrl"), utf8_literal("Alt"), utf8_literal("Shift"), utf8_literal("Win")};
    static constexpr Utf8View aliases[]{utf8_literal("control"), utf8_literal("option"), utf8_literal("shift"), utf8_literal("meta")};
    u32 mask = 0;
    char key[12];
    u32 key_bytes = 0;
    u32 start = 0;
    for (u32 at = 0; at <= text.bytes; ++at) {
        if (at < text.bytes && text.data[at] != '+') continue;
        u32 first = start, last = at;
        while (first < last && text.data[first] == ' ') ++first;
        while (last > first && text.data[last - 1] == ' ') --last;
        const Utf8View part{text.data + first, last - first};
        start = at + 1;
        if (!part.bytes) return false;
        bool modifier = false;
        for (u32 m = 0; m < 4; ++m)
            if (ascii_equal_ignore_case(part, modifiers[m]) || ascii_equal_ignore_case(part, aliases[m])) {
                if (mask & (1u << m)) return false;
                mask |= 1u << m;
                modifier = true;
            }
        if (modifier) continue;
        if (key_bytes) return false;
        const char c = part.data[0];
        if (part.bytes == 1 && ascii_is_alnum(c)) {
            key[0] = c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
            key_bytes = 1;
        } else if ((c == 'f' || c == 'F') && part.bytes >= 2 && part.bytes <= 3 && part.data[1] >= '0' &&
                   part.data[1] <= '9') {
            i64 number = 0;
            if (!parse_integer({part.data + 1, part.bytes - 1}, &number) || number < 1 || number > 24) return false;
            key[0] = 'F';
            key_bytes = 1;
            if (number >= 10) key[key_bytes++] = static_cast<char>('0' + number / 10);
            key[key_bytes++] = static_cast<char>('0' + number % 10);
        } else {
            for (const auto& named : kNamedKeys)
                if (ascii_equal_ignore_case(part, named.name)) {
                    copy_bytes(key, named.name.data, named.name.bytes);
                    key_bytes = named.name.bytes;
                    break;
                }
            if (!key_bytes && ascii_equal_ignore_case(part, utf8_literal("esc"))) {
                copy_bytes(key, "Escape", 6);
                key_bytes = 6;
            }
            if (!key_bytes) return false;
        }
    }
    if (!key_bytes) return false;
    u32 at = 0;
    for (u32 m = 0; m < 4; ++m)
        if (mask & (1u << m)) {
            copy_bytes(output + at, modifiers[m].data, modifiers[m].bytes);
            at += modifiers[m].bytes;
            output[at++] = '+';
        }
    copy_bytes(output + at, key, key_bytes);
    *bytes = at + key_bytes;
    return true;
}

bool native_plugin_sdk_keybind_chord(Utf8View normal, NativePluginSdkChord* chord) noexcept {
    if (!chord) return false;
    reset_value(chord);
    char text[64];
    u32 bytes = 0;
    if (!native_plugin_sdk_keybind_normalize(normal, text, &bytes)) return false;
    u32 at = 0;
    u8 modifiers = 0;
    const auto take = [&](Utf8View word, u8 bit) noexcept {
        if (bytes - at > word.bytes && bytes_equal(text + at, word.data, word.bytes) && text[at + word.bytes] == '+') {
            modifiers |= bit;
            at += word.bytes + 1;
        }
    };
    take(utf8_literal("Ctrl"), 1);
    take(utf8_literal("Alt"), 2);
    take(utf8_literal("Shift"), 4);
    if (bytes - at > 4 && bytes_equal(text + at, "Win+", 4)) return false;
    const Utf8View key{text + at, bytes - at};
    u8 code = 0;
    if (key.bytes == 1) code = static_cast<u8>(key.data[0]); // 'A'-'Z' and '0'-'9' are their virtual keys
    else if (key.data[0] == 'F' && key.bytes <= 3 && key.data[1] >= '0' && key.data[1] <= '9') {
        const u32 number = key.bytes == 2 ? static_cast<u32>(key.data[1] - '0')
                                          : static_cast<u32>(key.data[1] - '0') * 10 + static_cast<u32>(key.data[2] - '0');
        code = static_cast<u8>(0x70 + number - 1);
    } else {
        for (const auto& named : kNamedKeys)
            if (utf8_equal(named.name, key)) code = named.code;
    }
    if (!code) return false;
    *chord = {code, modifiers};
    return true;
}

bool native_plugin_sdk_colour_parse(Utf8View text, u32* colour) noexcept {
    u32 rgba = 0;
    if (!colour || !color_hex_parse(text, false, &rgba)) return false;
    *colour = rgba >> 8;
    return true;
}

bool native_plugin_sdk_keybind_press(NativePluginSdkRuntime* runtime, NativePluginSdkChord chord, bool text_focus) noexcept {
    if (!runtime || !chord.key) return false;
    if (text_focus && !(chord.modifiers & 3u)) return false;
    for (auto* binding : g_live) {
        if (!usable(binding, *runtime) || !permitted(*binding, MC_PERM_KEYBINDS) ||
            !callable(*binding, reinterpret_cast<const void*>(binding->keybind_handler))) continue;
        for (const auto& keybind : binding->keybinds) {
            if (!keybind.page_id || !keybind.active || !chord_equal(keybind.chord, chord)) continue;
            const u64 page = keybind.page_id, control = keybind.control_id;
            Call call(*binding, PluginCallKind::keybind);
            Gesture gesture(runtime);
            binding->keybind_handler(binding->plugin_state, binding->plugin_state_bytes, page, control);
            return true;
        }
    }
    return false;
}

u32 native_plugin_sdk_keybinds(const NativePluginSdkRuntime& runtime, Slice<NativePluginSdkKeybindView> output) noexcept {
    u32 count = 0;
    for (const auto* binding : g_live) {
        if (!usable(binding, runtime) || !permitted(*binding, MC_PERM_KEYBINDS) || !binding->keybind_handler) continue;
        for (const auto& keybind : binding->keybinds)
            if (keybind.page_id && keybind.active && count < output.count)
                output[count++] = {binding->owner_id, keybind.page_id, keybind.control_id, keybind.chord,
                                   {keybind.label, keybind.label_bytes}};
    }
    return count;
}

void native_plugin_sdk_keybinds_recheck(NativePluginSdkRuntime* runtime) noexcept {
    if (!runtime) return;
    for (auto* binding : g_live) {
        if (!usable(binding, *runtime)) continue;
        for (auto& keybind : binding->keybinds) {
            if (!keybind.page_id) continue;
            char buffer[kConflictBytes];
            keybind.active = keybind_resolve(*binding, keybind.page_id, keybind.control_id, keybind.chord, buffer).bytes == 0;
        }
    }
    ++runtime->keybind_revision;
}

u32 native_plugin_sdk_commands(const NativePluginSdkRuntime& runtime, Utf8View prefix,
                              Slice<NativePluginSdkCommandView> output) noexcept {
    u32 count = 0;
    for (auto* binding : g_live) {
        if (!usable(binding, runtime)) continue;
        for (u32 i = 0; i < binding->command_count && count < output.count; ++i) {
            const auto& command = binding->commands[i];
            if (prefix.bytes > command.name_bytes || !bytes_equal(command.name, prefix.data, prefix.bytes)) continue;
            // An earlier plugin's command wins a name clash; skip shadowed ones.
            bool shadowed = false;
            for (u32 j = 0; j < count; ++j)
                shadowed = shadowed || utf8_equal(output[j].name, {command.name, command.name_bytes});
            if (shadowed) continue;
            output[count++] = {binding->owner_id, command.id, {command.name, command.name_bytes},
                               {command.description, command.description_bytes}};
        }
    }
    return count;
}

bool native_plugin_sdk_command_matches(const NativePluginSdkRuntime& runtime, Utf8View text) noexcept {
    return match_command(runtime, command_name(text, nullptr)).command != nullptr;
}

NativePluginSdkComposeResult native_plugin_sdk_run_command(NativePluginSdkRuntime* runtime,
                                                         const NativePluginSdkDraft& draft) noexcept {
    if (!runtime) return {};
    Utf8View rest{};
    const Utf8View name = command_name(draft.text, &rest);
    const auto match = match_command(*runtime, name);
    if (!match.command) return {};
    auto& binding = *match.binding;
    const Command& command = *match.command;
    if (!callable(binding, reinterpret_cast<const void*>(binding.command_handler)))
        return notice(runtime, binding.owner_id, utf8_literal("This command isn't available right now."));
    if (rest.bytes > kPluginSdkInputBytes)
        return notice(runtime, binding.owner_id, utf8_literal("The command text is too long."));
    while (rest.bytes && ascii_is_space(rest.data[rest.bytes - 1])) --rest.bytes;
    // Copy-only: arguments point into the runtime's input copy, not the composer.
    copy_bytes(runtime->input, rest.data, rest.bytes);
    const Utf8View raw{runtime->input, rest.bytes};
    mc_command_argument arguments[kPluginSdkCommandOptions]{};
    u32 at = 0;
    for (u32 i = 0; i < command.option_count; ++i) {
        const auto& option = command.options[i];
        auto& argument = arguments[i];
        argument.size = sizeof(argument);
        argument.type = option.type;
        argument.name = {option.name, option.name_bytes};
        while (at < raw.bytes && ascii_is_space(raw.data[at])) ++at;
        u32 end = at;
        if (i + 1 == command.option_count && option.type == MC_OPTION_STRING) end = raw.bytes;
        else while (end < raw.bytes && !ascii_is_space(raw.data[end])) ++end;
        const Utf8View token{raw.data + at, end - at};
        at = end;
        const Utf8View option_name{option.name, option.name_bytes};
        if (!token.bytes) {
            if (option.required)
                return notice(runtime, binding.owner_id, utf8_literal("Missing "), option_name, utf8_literal("."));
            continue;
        }
        argument.present = 1;
        argument.text = {token.data, token.bytes};
        if (option.type == MC_OPTION_INTEGER && !parse_integer(token, &argument.integer))
            return notice(runtime, binding.owner_id, option_name, utf8_literal(" must be a whole number."));
        if (option.type == MC_OPTION_BOOLEAN) {
            bool flag = false;
            if (!parse_boolean(token, &flag))
                return notice(runtime, binding.owner_id, option_name, utf8_literal(" must be true or false."));
            argument.boolean = flag ? 1u : 0u;
        }
    }
    mc_command_invocation invocation{};
    invocation.size = sizeof(invocation);
    invocation.argument_count = command.option_count;
    invocation.command_id = command.id;
    invocation.channel_id = draft.channel_id;
    invocation.guild_id = draft.guild_id;
    invocation.arguments = arguments;
    invocation.argument_stride = sizeof(mc_command_argument);
    invocation.raw = {raw.data, raw.bytes};
    mc_text_output output{sizeof(mc_text_output), MC_CMD_DONE, runtime->output[0], kPluginSdkOutputBytes, 0};
    mc_status status{};
    {
        Call call(binding, PluginCallKind::command);
        Gesture gesture(runtime);
        status = binding.command_handler(binding.plugin_state, binding.plugin_state_bytes, &invocation, &output);
    }
    if (status != MC_OK) return notice(runtime, binding.owner_id, utf8_literal("The command didn't complete."));
    // Only action and length are trusted from the struct; text is read from our buffer.
    const Utf8View text{runtime->output[0], minimum(output.bytes, kPluginSdkOutputBytes)};
    if (!output_ok(output, MC_CMD_NOTICE) || !utf8_validate(text).valid)
        return notice(runtime, binding.owner_id, utf8_literal("The command returned text MaiCord can't use."));
    switch (output.action) {
    case MC_CMD_SEND:
        return text.bytes ? NativePluginSdkComposeResult{NativePluginSdkComposeAction::send, text, binding.owner_id}
                          : NativePluginSdkComposeResult{NativePluginSdkComposeAction::done, {}, binding.owner_id};
    case MC_CMD_REPLACE: return {NativePluginSdkComposeAction::replace, text, binding.owner_id};
    case MC_CMD_NOTICE: return {NativePluginSdkComposeAction::notice, text, binding.owner_id};
    default: return {NativePluginSdkComposeAction::done, {}, binding.owner_id};
    }
}

NativePluginSdkComposeResult native_plugin_sdk_pre_send(NativePluginSdkRuntime* runtime,
                                                      const NativePluginSdkDraft& draft) noexcept {
    if (!runtime || draft.text.bytes > kPluginSdkInputBytes) return {};
    copy_bytes(runtime->input, draft.text.data, draft.text.bytes);
    Utf8View current{runtime->input, draft.text.bytes};
    u32 next = 0;
    u64 owner = 0;
    for (auto* binding : g_live) {
        if (!binding || binding->runtime != runtime || !permitted(*binding, MC_PERM_MESSAGE_SEND) ||
            !callable(*binding, reinterpret_cast<const void*>(binding->pre_send))) continue;
        mc_message_view message{};
        message.size = sizeof(message);
        message.flags = MC_MESSAGE_OWN;
        message.channel_id = draft.channel_id;
        message.author_id = draft.author_id;
        message.guild_id = draft.guild_id;
        message.content = {current.data, current.bytes};
        mc_text_output output{sizeof(mc_text_output), MC_SEND_UNCHANGED, runtime->output[next], kPluginSdkOutputBytes, 0};
        mc_status status{};
        {
            Call call(*binding, PluginCallKind::pre_send);
            status = binding->pre_send(binding->plugin_state, binding->plugin_state_bytes, &message, &output);
        }
        if (status != MC_OK || !output_ok(output, MC_SEND_CANCEL)) continue;
        if (output.action == MC_SEND_CANCEL) return {NativePluginSdkComposeAction::cancel, {}, binding->owner_id};
        const Utf8View replaced{runtime->output[next], output.bytes};
        if (output.action != MC_SEND_REPLACE || !utf8_validate(replaced).valid) continue;
        current = replaced;
        owner = binding->owner_id;
        next ^= 1;
    }
    return owner ? NativePluginSdkComposeResult{NativePluginSdkComposeAction::replace, current, owner}
                 : NativePluginSdkComposeResult{};
}

NativePluginSdkPoll native_plugin_sdk_poll(NativePluginSdkRuntime* runtime) noexcept {
    NativePluginSdkPoll result{};
    if (!runtime) return result;
    for (auto* binding : g_live) {
        if (!binding || binding->runtime != runtime || binding->phase != Phase::enabled) continue;
        if (binding->store.dirty) store_save(*binding);
        for (auto& timer : binding->timers) {
            if (!timer.active || !callable(*binding, reinterpret_cast<const void*>(binding->timer_handler))) continue;
            const u64 now = now_ms(*runtime);
            if (timer.due <= now) {
                const u64 id = timer.id;
                // Repeats restart from now so a stalled loop never fires a burst.
                if (timer.repeat) timer.due = now + timer.interval;
                else reset_value(&timer);
                Call call(*binding, PluginCallKind::timer);
                binding->timer_handler(binding->plugin_state, binding->plugin_state_bytes, id);
            }
            if (timer.active && binding->phase == Phase::enabled && (!result.next_timer_ms || timer.due < result.next_timer_ms))
                result.next_timer_ms = timer.due;
        }
    }
    result.events_pending = native_plugin_sdk_event_deliver(runtime);
    return result;
}

void native_plugin_sdk_flush(NativePluginSdkRuntime* runtime) noexcept {
    for (auto* binding : plugin_sdk::g_live)
        if (binding && binding->runtime == runtime && binding->store.dirty) store_save(*binding);
}

} // namespace maicord
