#include "ui_internal.hpp"
#include "maicord/app/plugins/text.hpp"
#include "maicord/core/utf.hpp"

// What a plugin shows the user (a banner, a modal) and the navigation actions its menus and buttons use
// (open a link, copy text, open a channel or profile). Requests are validated and copied here; the
// application shows them (PluginUiHost) and reports the answer back.
namespace maicord {
using namespace plugin_ui_detail;
namespace {

constexpr u32 kModalTitleBytes = 64, kModalBodyBytes = 512, kModalButtonBytes = 32, kModalLabelBytes = 64;
constexpr u32 kModalInitialBytes = 2000, kModalToggleBytes = 96, kModalItemBytes = 64;
constexpr u32 kModalMaximumChars = 2000;

struct Modal {
    bool active;
    u64 tag;
    PluginModalRequest request;
    char title[kModalTitleBytes], body[kModalBodyBytes], accept[kModalButtonBytes], cancel[kModalButtonBytes];
    char input_label[kModalLabelBytes], input_initial[kModalInitialBytes], input_hint[kModalLabelBytes];
    char toggle_label[kModalToggleBytes], select_label[kModalLabelBytes];
    char items[kPluginModalSelectItems][kModalItemBytes];
};
Modal g_modal;
u32 g_banner_serial;

// Short human text: UTF-8 without control characters, minimum..maximum bytes.
bool text_ok(const mc_str& text, u32 minimum, u32 maximum) noexcept {
    return text.bytes >= minimum && text.bytes <= maximum && plugin_text_clean(text.data, text.bytes);
}

// Multi-line text: like text_ok, with line breaks and tabs allowed.
bool body_ok(const mc_str& text, u32 maximum) noexcept {
    return text.bytes <= maximum && plugin_text_clean(text.data, text.bytes, kPluginTextBody);
}

Utf8View keep(char* storage, const mc_str& text) noexcept {
    copy_bytes(storage, text.data, text.bytes);
    return {storage, text.bytes};
}

mc_status status_of(PluginUiStatus status) noexcept {
    switch (status) {
    case PluginUiStatus::ok: return MC_OK;
    case PluginUiStatus::busy: return MC_E_BUSY;
    case PluginUiStatus::not_found: return MC_E_NOT_FOUND;
    default: return MC_E_DENIED;
    }
}

bool gesture(const PluginBinding& binding) noexcept {
    return (binding.ctx.flags & MC_CTX_GESTURE) != 0;
}

Slot* slot_of(const PluginBinding& binding) noexcept {
    return find_slot(binding.owner_id);
}

bool http_url(mc_str url) noexcept {
    if (!url.bytes || url.bytes > MC_HTTP_URL_MAX || !url.data || !utf8_validate(plugin_view(url)).valid) return false;
    for (u32 i = 0; i < url.bytes; ++i)
        if (static_cast<u8>(url.data[i]) <= 0x20 || static_cast<u8>(url.data[i]) == 0x7f) return false;
    const auto prefix = [&](const char* scheme) noexcept {
        const u32 length = c_string_length(scheme);
        if (url.bytes <= length) return false;
        for (u32 i = 0; i < length; ++i) {
            const char c = url.data[i] >= 'A' && url.data[i] <= 'Z' ? static_cast<char>(url.data[i] + 32) : url.data[i];
            if (c != scheme[i]) return false;
        }
        return true;
    };
    return prefix("https://") || prefix("http://");
}

} // namespace

// ---- Banners -----------------------------------------------------------------------------

namespace plugin_ui_detail {

mc_status banner_set(PluginBinding& binding, const mc_banner& banner) noexcept {
    if ((banner.tone != MC_TONE_NEUTRAL && banner.tone != MC_TONE_WARNING) || !text_ok(banner.title, 1, kBannerTitleBytes) ||
        !text_ok(banner.text, 0, kBannerTextBytes) || !text_ok(banner.action, 0, kBannerActionBytes))
        return MC_E_INVALID;
    Slot* slot = slot_of(binding);
    if (!slot) return MC_E_INACTIVE;
    Banner& shown = slot->banner;
    shown.present = true;
    shown.warning = banner.tone == MC_TONE_WARNING;
    shown.tag = banner.tag;
    shown.title_bytes = banner.title.bytes;
    shown.text_bytes = banner.text.bytes;
    shown.action_bytes = banner.action.bytes;
    copy_bytes(shown.title, banner.title.data, banner.title.bytes);
    copy_bytes(shown.text, banner.text.data, banner.text.bytes);
    copy_bytes(shown.action, banner.action.data, banner.action.bytes);
    if (++g_banner_serial == 0) ++g_banner_serial;
    shown.serial = g_banner_serial;
    if (g_host && g_host->banner_changed) g_host->banner_changed();
    return MC_OK;
}

mc_status banner_clear(PluginBinding& binding, u64 tag) noexcept {
    Slot* slot = slot_of(binding);
    if (!slot) return MC_E_INACTIVE;
    if (!slot->banner.present || slot->banner.tag != tag) return MC_E_NOT_FOUND;
    slot->banner = {};
    if (g_host && g_host->banner_changed) g_host->banner_changed();
    return MC_OK;
}

// ---- Modals ------------------------------------------------------------------------------

mc_status modal_open(PluginBinding& binding, const mc_modal& modal, u64 tag) noexcept {
    if (!gesture(binding)) return MC_E_GESTURE;
    const u32 known = MC_MODAL_DANGER | MC_MODAL_ACCEPT_FOCUSED | MC_MODAL_MULTILINE | MC_MODAL_READ_ONLY | MC_MODAL_INFO;
    const bool input = modal.has_input == 1, select = modal.select_label.bytes != 0;
    const bool read_only = (modal.flags & MC_MODAL_READ_ONLY) != 0, info = (modal.flags & MC_MODAL_INFO) != 0;
    if ((modal.flags & ~known) || modal.has_input > 1 || modal.toggle_initial > 1 ||
        !text_ok(modal.title, 1, kModalTitleBytes) || !body_ok(modal.body, kModalBodyBytes) ||
        !text_ok(modal.accept, 0, kModalButtonBytes) || !text_ok(modal.cancel, 0, kModalButtonBytes) ||
        !text_ok(modal.input_label, 0, kModalLabelBytes) || !text_ok(modal.input_hint, 0, kModalLabelBytes) ||
        !body_ok(modal.input_initial, kModalInitialBytes) || modal.input_max_chars > kModalMaximumChars ||
        !text_ok(modal.toggle_label, 0, kModalToggleBytes) || !text_ok(modal.select_label, 0, kModalLabelBytes))
        return MC_E_INVALID;
    // A confirmation may have a toggle, a form a dropdown; the other combinations are not offered.
    if ((input && modal.toggle_label.bytes) || (!input && (select || modal.input_label.bytes || modal.input_initial.bytes ||
                                                           modal.input_hint.bytes || modal.input_max_chars)) ||
        (modal.flags & MC_MODAL_MULTILINE && !input))
        return MC_E_INVALID;
    // Read-only text is a form's text shown, not edited; an information dialog has its accept button only and
    // nothing to answer.
    if ((read_only && (!input || !modal.input_initial.bytes || modal.input_hint.bytes || modal.input_max_chars || select)) ||
        (info && (modal.cancel.bytes || modal.toggle_label.bytes || (modal.flags & MC_MODAL_DANGER) || (input && !read_only))))
        return MC_E_INVALID;
    if (select) {
        if (!modal.select_items || !modal.select_count || modal.select_count > MC_MODAL_SELECT_MAX ||
            modal.select_initial >= modal.select_count)
            return MC_E_INVALID;
        for (u32 i = 0; i < modal.select_count; ++i)
            if (!text_ok(modal.select_items[i], 1, kModalItemBytes)) return MC_E_INVALID;
    } else if (modal.select_count || modal.select_initial) {
        return MC_E_INVALID;
    }
    if (!g_host || !g_host->open_modal) return MC_E_UNSUPPORTED;
    if (g_modal.active) return MC_E_BUSY;

    Modal& m = g_modal;
    m = {};
    PluginModalRequest& request = m.request;
    request.kind = read_only ? PluginModalKind::text
                   : input   ? (select ? PluginModalKind::input_select : PluginModalKind::input)
                             : (modal.toggle_label.bytes ? PluginModalKind::toggle : PluginModalKind::confirm);
    request.danger = (modal.flags & MC_MODAL_DANGER) != 0;
    request.multiline = (modal.flags & MC_MODAL_MULTILINE) != 0;
    request.single = info;
    request.owner_id = binding.owner_id;
    request.title = keep(m.title, modal.title);
    request.context = keep(m.body, modal.body);
    request.accept = keep(m.accept, modal.accept);
    request.cancel = keep(m.cancel, modal.cancel);
    request.input_label = keep(m.input_label, modal.input_label);
    request.input_initial = keep(m.input_initial, modal.input_initial);
    request.input_hint = keep(m.input_hint, modal.input_hint);
    request.input_max_chars = modal.input_max_chars;
    request.toggle_label = keep(m.toggle_label, modal.toggle_label);
    request.toggle_initial = modal.toggle_initial != 0;
    request.select_label = keep(m.select_label, modal.select_label);
    request.select_count = select ? modal.select_count : 0;
    request.select_initial = modal.select_initial;
    for (u32 i = 0; i < request.select_count; ++i) request.select_items[i] = keep(m.items[i], modal.select_items[i]);
    m.tag = tag;
    m.active = true;
    if (!g_host->open_modal(request)) {
        m = {};
        return MC_E_BUSY;
    }
    return MC_OK;
}

// ---- Actions -----------------------------------------------------------------------------

mc_status open_url(PluginBinding& binding, mc_str url) noexcept {
    if (!http_url(url)) return MC_E_INVALID;
    if (!gesture(binding)) return MC_E_GESTURE;
    return g_host && g_host->open_url ? status_of(g_host->open_url(plugin_view(url))) : MC_E_UNSUPPORTED;
}

mc_status copy_text(PluginBinding& binding, mc_str text) noexcept {
    if (!(binding.permissions & MC_PERM_CLIPBOARD)) return MC_E_DENIED;
    if (!text.bytes || text.bytes > MC_ACTION_TEXT_MAX || !text.data || !utf8_validate(plugin_view(text)).valid) return MC_E_INVALID;
    if (!gesture(binding)) return MC_E_GESTURE;
    return g_host && g_host->copy_text ? status_of(g_host->copy_text(plugin_view(text))) : MC_E_UNSUPPORTED;
}

mc_status open_target(PluginBinding& binding, u32 what, u64 id, u64 sub) noexcept {
    if ((what != MC_OPEN_CHANNEL && what != MC_OPEN_PROFILE) || !id) return MC_E_INVALID;
    if (!gesture(binding)) return MC_E_GESTURE;
    if (what == MC_OPEN_CHANNEL)
        return g_host && g_host->open_channel ? status_of(g_host->open_channel(id)) : MC_E_UNSUPPORTED;
    return g_host && g_host->open_profile ? status_of(g_host->open_profile(id, sub)) : MC_E_UNSUPPORTED;
}

void feedback_remove(Slot* slot) noexcept {
    if (slot->banner.present) {
        slot->banner = {};
        if (g_host && g_host->banner_changed) g_host->banner_changed();
    }
    if (g_modal.active && g_modal.request.owner_id == slot->binding->owner_id) {
        g_modal = {};
        if (g_host && g_host->close_modal) g_host->close_modal();
    }
}

} // namespace plugin_ui_detail

bool plugin_banner_current(PluginBannerView* output) noexcept {
    for (u32 i = 0; i < g_slot_count; ++i) {
        const Slot& slot = g_slots[i];
        if (!slot.banner.present || !callable(*slot.binding)) continue;
        const Banner& banner = slot.banner;
        *output = {slot.binding->owner_id, banner.tag, banner.serial, banner.warning,
                   {banner.title, banner.title_bytes}, {banner.text, banner.text_bytes},
                   {banner.action, banner.action_bytes}};
        return true;
    }
    return false;
}

bool plugin_banner_resolve(bool action) noexcept {
    PluginBannerView shown{};
    if (!plugin_banner_current(&shown)) return false;
    Slot* slot = find_slot(shown.owner_id);
    slot->banner = {};
    PluginCompletion completion{};
    completion.kind = PluginCompletionKind::banner;
    completion.tag = shown.tag;
    completion.gesture = true;
    completion.which = action ? 1 : 2;
    plugin_dispatch_completion(shown.owner_id, completion);
    return true;
}

const PluginModalRequest* plugin_modal_current() noexcept {
    return g_modal.active ? &g_modal.request : nullptr;
}

bool plugin_modal_resolve(bool accepted, Utf8View input, bool toggle, u32 select) noexcept {
    if (!g_modal.active) return false;
    const u64 owner = g_modal.request.owner_id;
    const u64 tag = g_modal.tag;
    const bool has_input = g_modal.request.kind == PluginModalKind::input || g_modal.request.kind == PluginModalKind::input_select;
    const bool has_toggle = g_modal.request.kind == PluginModalKind::toggle;
    const bool has_select = g_modal.request.kind == PluginModalKind::input_select;
    const u32 items = g_modal.request.select_count;
    g_modal = {};
    PluginCompletion completion{};
    completion.kind = PluginCompletionKind::modal;
    completion.tag = tag;
    completion.gesture = accepted;
    completion.accepted = accepted ? 1 : 0;
    completion.toggle = accepted && has_toggle && toggle ? 1 : 0;
    completion.select_index = accepted && has_select && select < items ? select : 0;
    completion.text = accepted && has_input ? input : Utf8View{};
    plugin_dispatch_completion(owner, completion);
    return true;
}

} // namespace maicord
