#include "maicord/app/plugins/settings_page.hpp"
#include "maicord/app/plugins/text.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/color_hex.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/text_bank.hpp"

namespace maicord {
namespace {

// Windows virtual-key codes the keybind text names.
struct KeyName {
    u32 vk;
    Utf8View name;
};
constexpr KeyName kKeyNames[]{
    {0x20, utf8_literal("Space")},   {0x0D, utf8_literal("Enter")},  {0x09, utf8_literal("Tab")},
    {0x1B, utf8_literal("Escape")},  {0x08, utf8_literal("Backspace")}, {0x2E, utf8_literal("Delete")},
    {0x2D, utf8_literal("Insert")},  {0x24, utf8_literal("Home")},   {0x23, utf8_literal("End")},
    {0x21, utf8_literal("PageUp")},  {0x22, utf8_literal("PageDown")}, {0x26, utf8_literal("Up")},
    {0x28, utf8_literal("Down")},    {0x25, utf8_literal("Left")},   {0x27, utf8_literal("Right")},
};
constexpr u32 kFirstFunctionKey = 0x70, kFunctionKeys = 24;

const mc_setting_def* settings_of(const PluginBinding& binding) noexcept {
    return binding.image.descriptor->settings;
}
const u8* block_of(const PluginBinding& binding) noexcept {
    return static_cast<const u8*>(binding.ctx.settings);
}

bool visible(const PluginBinding& binding, const mc_setting_def& setting) noexcept {
    if (setting.flags & MC_SET_HIDDEN) return false;
    if (!setting.visible_if) return true;
    const mc_setting_def& gate = settings_of(binding)[setting.visible_if - 1];
    const u8* at = block_of(binding) + gate.offset;
    if (gate.type == MC_SET_TOGGLE) return *at != 0;
    return byte_load_le32(at) == setting.visible_value;
}

// ---- Text of values -----------------------------------------------------------------

u32 write_key(u32 key, Slice<char> out) noexcept {
    TextWriter w{out, 0, true};
    if (key & MC_KEY_CTRL) w.add("Ctrl+", 5);
    if (key & MC_KEY_SHIFT) w.add("Shift+", 6);
    if (key & MC_KEY_ALT) w.add("Alt+", 4);
    const u32 vk = key & 0xFF;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        const char c = static_cast<char>(vk);
        w.add(&c, 1);
    } else if (vk >= kFirstFunctionKey && vk < kFirstFunctionKey + kFunctionKeys) {
        w.add("F", 1);
        w.number(vk - kFirstFunctionKey + 1);
    } else {
        for (const KeyName& entry : kKeyNames)
            if (entry.vk == vk) w.add(entry.name);
    }
    return w.ok ? w.used : 0;
}

bool parse_key(Utf8View text, u32* key) noexcept {
    u32 value = 0, vk = 0;
    for (u32 first = 0; first <= text.bytes;) {
        u32 end = first;
        while (end < text.bytes && text.data[end] != '+') ++end;
        const Utf8View token{text.data + first, end - first};
        const bool last = end == text.bytes;
        if (!token.bytes || vk) return false;
        if (!last && ascii_equal_ignore_case(token, utf8_literal("ctrl"))) value |= MC_KEY_CTRL;
        else if (!last && ascii_equal_ignore_case(token, utf8_literal("shift"))) value |= MC_KEY_SHIFT;
        else if (!last && ascii_equal_ignore_case(token, utf8_literal("alt"))) value |= MC_KEY_ALT;
        else if (!last) return false;
        else if (token.bytes == 1 && ascii_is_alnum(token.data[0])) vk = static_cast<u32>(ascii_lower(token.data[0]) >= 'a' ? ascii_lower(token.data[0]) - 32 : token.data[0]);
        else if ((token.data[0] == 'F' || token.data[0] == 'f') && token.bytes > 1) {
            u64 number = 0;
            if (!decimal_parse({token.data + 1, token.bytes - 1}, &number) || !number || number > kFunctionKeys) return false;
            vk = kFirstFunctionKey + static_cast<u32>(number) - 1;
        } else {
            for (const KeyName& entry : kKeyNames)
                if (ascii_equal_ignore_case(token, entry.name)) vk = entry.vk;
            if (!vk) return false;
        }
        first = end + 1;
    }
    *key = value | vk;
    return vk != 0;
}

PluginSettingsOutcome from(mc_status status) noexcept {
    return status == MC_OK ? PluginSettingsOutcome::changed : PluginSettingsOutcome::refused;
}

void add_row(PluginSettingsPage* page, u64 id, const UiSettingsControl& control, bool danger) noexcept {
    page->controls[page->count] = control;
    page->controls[page->count].id = id;
    page->presentation[page->count] = {danger ? UiSettingsVariant::critical : UiSettingsVariant::standard, 0,
                                       kUiSettingsNoDefault};
    page->ids[page->count++] = id;
}

} // namespace

u32 plugin_setting_format(const PluginBinding& binding, u32 index, Slice<char> output) noexcept {
    const mc_setting_def& setting = settings_of(binding)[index];
    const u8* at = block_of(binding) + setting.offset;
    TextWriter w{output, 0, true};
    switch (setting.type) {
    case MC_SET_INT:
    case MC_SET_SLIDER: {
        char digits[12];
        w.add(digits, decimal_write_signed(digits, static_cast<i32>(byte_load_le32(at))));
        break;
    }
    case MC_SET_TEXT:
    case MC_SET_SECRET: {
        mc_str text;
        copy_bytes(&text, at, sizeof(text));
        w.add(plugin_view(text));
        break;
    }
    case MC_SET_TEXT_LIST: {
        mc_list list;
        copy_bytes(&list, at, sizeof(list));
        for (u32 i = 0; i < list.count; ++i) {
            if (i) w.add("\n", 1);
            w.add(plugin_view(list.items[i]));
        }
        break;
    }
    case MC_SET_COLOUR: {
        const u32 colour = byte_load_le32(at);
        char text[7];
        text[0] = '#';
        for (u32 i = 0; i < 6; ++i) text[1 + i] = ascii_hex_digit_upper(colour >> (20 - 4 * i));
        w.add(text, 7);
        break;
    }
    case MC_SET_KEYBIND: return byte_load_le32(at) ? write_key(byte_load_le32(at), output) : 0;
    case MC_SET_CHANNEL: {
        const u64 id = byte_load_le64(at);
        if (id) w.number(id);
        break;
    }
    case MC_SET_USER_LIST: {
        mc_ids ids;
        copy_bytes(&ids, at, sizeof(ids));
        for (u32 i = 0; i < ids.count; ++i) {
            if (i) w.add("\n", 1);
            w.number(ids.items[i]);
        }
        break;
    }
    default: break;
    }
    return w.ok ? w.used : 0;
}

static bool plugin_setting_parse(const mc_setting_def& setting, Utf8View text, Slice<u8> bytes, u32* used) noexcept {
    *used = 0;
    const Utf8View trimmed = ascii_trim(text);
    switch (setting.type) {
    case MC_SET_TEXT:
    case MC_SET_SECRET:
    case MC_SET_TEXT_LIST:
        if (text.bytes > bytes.count) return false;
        copy_bytes(bytes.data, text.data, text.bytes);
        *used = text.bytes;
        return true;
    case MC_SET_INT:
    case MC_SET_SLIDER: {
        const bool negative = trimmed.bytes && trimmed.data[0] == '-';
        u64 magnitude = 0;
        if (!decimal_parse({trimmed.data + negative, trimmed.bytes - negative}, &magnitude) ||
            magnitude > (negative ? 0x80000000ull : 0x7FFFFFFFull))
            return false;
        byte_store_le32(bytes.data, negative ? static_cast<u32>(0u - static_cast<u32>(magnitude)) : static_cast<u32>(magnitude));
        *used = 4;
        return true;
    }
    case MC_SET_COLOUR: {
        u32 rgb = 0;
        if (!color_hex_parse_rgb24(trimmed, &rgb)) return false;
        byte_store_le32(bytes.data, rgb);
        *used = 4;
        return true;
    }
    case MC_SET_KEYBIND: {
        u32 key = 0;
        if (trimmed.bytes && !parse_key(trimmed, &key)) return false;
        byte_store_le32(bytes.data, key);
        *used = 4;
        return true;
    }
    case MC_SET_CHANNEL: {
        u64 id = 0;
        if (trimmed.bytes && !decimal_parse(trimmed, &id)) return false;
        byte_store_le64(bytes.data, id);
        *used = 8;
        return true;
    }
    case MC_SET_USER_LIST: {
        // Ids separated by anything that is not a digit.
        for (u32 at = 0; at < text.bytes;) {
            while (at < text.bytes && !ascii_is_digit(text.data[at])) {
                if (!ascii_is_space(text.data[at]) && text.data[at] != ',') return false;
                ++at;
            }
            u32 end = at;
            while (end < text.bytes && ascii_is_digit(text.data[end])) ++end;
            if (end == at) break;
            u64 id = 0;
            if (!decimal_parse({text.data + at, end - at}, &id) || *used + 8 > bytes.count) return false;
            byte_store_le64(bytes.data + *used, id);
            *used += 8;
            at = end;
        }
        return true;
    }
    default: return false;
    }
}

void plugin_settings_project(const PluginBinding& binding, const PluginSettingsHost& host, PluginSettingsPage* page) noexcept {
    const mc_descriptor& descriptor = *binding.image.descriptor;
    const u32 revision = page->content.revision + 1;
    page->count = page->choice_used = page->slider_used = 0;
    page->title = {binding.image.info.name, binding.image.info.name_bytes};
    for (u32 i = 0; i < descriptor.setting_count; ++i) {
        const mc_setting_def& setting = descriptor.settings[i];
        if (!visible(binding, setting)) continue;
        UiSettingsControl control{};
        control.label = plugin_view(setting.label);
        control.description = plugin_view(setting.description);
        control.enabled = true;
        const u8* at = block_of(binding) + setting.offset;
        const bool danger = setting.flags & MC_SET_DANGER;
        switch (setting.type) {
        case MC_SET_HEADER: control.kind = UiSettingsControlKind::category; break;
        case MC_SET_TOGGLE:
            control.kind = UiSettingsControlKind::toggle;
            control.checked = *at != 0;
            break;
        case MC_SET_CHOICE: {
            if (setting.item_count > kPluginSettingsChoiceViews - page->choice_used) continue;
            Utf8View* views = page->choice_views + page->choice_used;
            for (u32 item = 0; item < setting.item_count; ++item) views[item] = plugin_view(setting.items[item]);
            page->choice_used += setting.item_count;
            control.kind = UiSettingsControlKind::choice;
            control.choices = {views, setting.item_count};
            control.selected_choice = byte_load_le32(at);
            break;
        }
        default: {
            const u64 steps = setting.type == MC_SET_SLIDER ? (static_cast<i64>(setting.maximum) - setting.minimum) / setting.step + 1 : 0;
            if (setting.type == MC_SET_SLIDER && steps <= kPluginSettingsSliderSteps &&
                page->slider_used + steps <= kPluginSettingsSliderSliders * kPluginSettingsSliderSteps) {
                // A slider with a label per step; wider ranges are typed.
                const i32 value = static_cast<i32>(byte_load_le32(at));
                Utf8View* views = page->slider_views + page->slider_used;
                for (u32 step = 0; step < steps; ++step) {
                    char* text = page->slider_text[page->slider_used + step];
                    views[step] = {text, decimal_write_signed(text, static_cast<i64>(setting.minimum) + static_cast<i64>(step) * setting.step)};
                }
                page->slider_used += static_cast<u32>(steps);
                control.kind = UiSettingsControlKind::slider;
                control.choices = {views, static_cast<u32>(steps)};
                control.selected_choice = value > setting.minimum ? static_cast<u32>((static_cast<i64>(value) - setting.minimum) / setting.step) : 0;
                break;
            }
            const bool multiline = setting.type == MC_SET_TEXT_LIST || setting.type == MC_SET_USER_LIST || (setting.type == MC_SET_TEXT && (setting.flags & MC_SET_MULTILINE));
            control.editor = host.editor(host.context, i, multiline, setting.type == MC_SET_SECRET);
            if (!control.editor) continue;
            control.kind = multiline ? UiSettingsControlKind::multiline : UiSettingsControlKind::edit;
            break;
        }
        }
        add_row(page, i + 1, control, danger);
        if (page->rejected == i + 1u) {
            UiSettingsControl error{};
            error.label = utf8_literal("That value isn't accepted.");
            error.kind = UiSettingsControlKind::error;
            error.enabled = true;
            add_row(page, i + 1, error, false);
        }
    }
    if (descriptor.network_count) {
        UiSettingsControl header{};
        header.label = utf8_literal("Network access");
        header.description = utf8_literal("The servers this plugin may reach, over HTTPS.");
        header.kind = UiSettingsControlKind::category;
        header.enabled = true;
        add_row(page, kPluginSettingsNetworkHeader, header, false);
        for (u32 i = 0; i < descriptor.network_count; ++i) {
            UiSettingsControl toggle{};
            toggle.label = plugin_view(descriptor.network[i]);
            toggle.kind = UiSettingsControlKind::toggle;
            toggle.enabled = true;
            toggle.checked = (binding.approved_hosts >> i) & 1;
            add_row(page, kPluginSettingsHostBase + i, toggle, false);
        }
    }
    page->content.title = page->title;
    page->content.description = {binding.image.info.summary, binding.image.info.summary_bytes};
    page->content.controls = {page->controls, page->count};
    page->content.presentation = page->presentation;
    page->content.revision = revision;
}

PluginSettingsOutcome plugin_settings_activate(PluginBinding* binding, PluginSettingsPage* page,
                                               const UiSettingsActivation& activation) noexcept {
    const u64 id = activation.target.control_id;
    bool shown = false;
    for (u32 i = 0; i < page->count; ++i) shown = shown || (page->ids[i] == id && page->controls[i].kind == activation.kind);
    if (!shown) return PluginSettingsOutcome::none;
    if (id >= kPluginSettingsHostBase) {
        const u32 host = static_cast<u32>(id - kPluginSettingsHostBase);
        return plugin_binding_set_host(binding, host, activation.checked) ? PluginSettingsOutcome::changed : PluginSettingsOutcome::refused;
    }
    const u32 index = static_cast<u32>(id - 1);
    const mc_setting_def& setting = settings_of(*binding)[index];
    u8 value[4];
    u32 bytes = 4;
    switch (setting.type) {
    case MC_SET_TOGGLE: value[0] = activation.checked ? 1 : 0; bytes = 1; break;
    case MC_SET_CHOICE: byte_store_le32(value, activation.selected_choice); break;
    case MC_SET_SLIDER:
        byte_store_le32(value, static_cast<u32>(static_cast<i64>(setting.minimum) + static_cast<i64>(activation.selected_choice) * setting.step));
        break;
    default: return PluginSettingsOutcome::none;
    }
    return from(plugin_binding_change_setting(binding, index, value, bytes));
}

PluginSettingsOutcome plugin_settings_commit(PluginBinding* binding, PluginSettingsPage* page, u64 control_id,
                                             Utf8View text) noexcept {
    bool shown = false;
    for (u32 i = 0; i < page->count; ++i) shown = shown || page->ids[i] == control_id;
    if (!shown || !control_id || control_id >= kPluginSettingsHostBase) return PluginSettingsOutcome::none;
    const u32 index = static_cast<u32>(control_id - 1);
    const mc_setting_def& setting = settings_of(*binding)[index];
    if (setting.type == MC_SET_HEADER || setting.type == MC_SET_TOGGLE || setting.type == MC_SET_CHOICE)
        return PluginSettingsOutcome::none;
    u8 bytes[MC_SETTING_LIST_BYTES_MAX + MC_SETTING_ITEM_MAX + 8];
    u32 used = 0;
    const bool parsed = plugin_setting_parse(setting, text, {bytes, sizeof(bytes)}, &used);
    const PluginSettingsOutcome outcome = parsed ? from(plugin_binding_change_setting(binding, index, bytes, used))
                                                 : PluginSettingsOutcome::refused;
    page->rejected = outcome == PluginSettingsOutcome::refused ? control_id : 0;
    return outcome;
}

} // namespace maicord
