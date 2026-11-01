#include "consent_dialog.hpp"
#include "../chat/composer/text_form.hpp"
#include "maicord/app/plugins/consent.hpp"
#include "maicord/app/update/fetch.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::app_detail {
namespace {

// One consent dialog at a time; the form's callbacks find their request here.
constexpr u64 kScope = 0x4D43504C55474E43ull; // "MCPLUGNC"
struct Pending {
    bool active;
    u64 serial;
    PluginApproval approval;
    PluginConsentAccepted accepted;
    void* context;
    char title[MC_NAME_MAX + 16];
    char text[kPluginConsentTextBytes];
};
Pending g_pending;
u64 g_serial;

bool valid(TextFormOwnerKey key) noexcept {
    return g_pending.active && key.scope == kScope && key.item == g_pending.serial;
}
bool accept(TextFormOwnerKey key, Utf8View) noexcept {
    if (!valid(key)) return false;
    g_pending.active = false;
    g_pending.accepted(g_pending.context, g_pending.approval);
    return true;
}
void closed(TextFormOwnerKey key) noexcept {
    if (valid(key)) g_pending.active = false;
}

} // namespace

bool application_plugin_consent_open(const PluginConsentRequest& request, PluginConsentAccepted accepted,
                                     void* context) noexcept {
    if (!accepted || g_pending.active) return false;
    const u32 bytes = plugin_consent_text(request, {g_pending.text, sizeof(g_pending.text)});
    if (!bytes) return false;
    UpdateTextWriter title{{g_pending.title, sizeof(g_pending.title)}, 0, true};
    title.add(utf8_literal("Enable "));
    title.add(request.name);
    title.add(utf8_literal("?"));
    if (!title.ok) return false;
    // Allowing the plugin allows everything it listed, hosts included.
    g_pending.approval = {request.permissions, request.hosts.count >= 32 ? ~0u : (1u << request.hosts.count) - 1};
    g_pending.accepted = accepted;
    g_pending.context = context;
    g_pending.serial = ++g_serial;
    g_pending.active = true;
    ApplicationTextFormRequest form{};
    form.title = {g_pending.title, title.used};
    form.context = {g_pending.text, bytes};
    form.accept = utf8_literal("Allow and enable");
    form.owner = {kScope, 0, g_pending.serial};
    form.valid = valid;
    form.save = accept;
    form.closed = closed;
    form.confirmation_only = true;
    form.danger = (request.permissions & kPluginHighRiskPermissions) != 0;
    if (application_text_form_open(form)) return true;
    g_pending.active = false;
    return false;
}

} // namespace maicord::app_detail
