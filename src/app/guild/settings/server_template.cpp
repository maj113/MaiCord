#include "server_access.hpp"
#include "../../shell/toast.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"

namespace maicord::app_detail::server_settings {
namespace {

constexpr Utf8View kGenerate[]{utf8_literal("Generate Template")};
// Sync shows only while the server has unsynced changes.
constexpr Utf8View kActions[]{utf8_literal("Sync Template"), utf8_literal("Preview Template"),
                              utf8_literal("Delete Template")};
constexpr u32 kSync = 0, kPreview = 1;

Utf8View title() noexcept {
    return editor_text(editor_name);
}
Utf8View description() noexcept {
    return editor_text(editor_description);
}

// The official page refuses to show templates to anyone who cannot see every channel.
bool channels_visible() noexcept {
    const auto& s = state();
    const u32 slots = chat_state_store_channel_slot_bound(s);
    for (u32 i = 0; i < slots; ++i) {
        const auto& c = s.channels[i];
        if (c.occupied && c.guild_id == owner->guild_id && c.permissions_resolved &&
            !(c.effective_permissions & kPermissionViewChannel))
            return false;
    }
    return true;
}

void load() noexcept {
    auto* s = server_access_state();
    const bool exists = s && s->template_exists;
    const auto& t = s ? s->guild_template : GuildTemplate{};
    set_editor(editor_name, exists ? Utf8View{t.name, t.name_bytes} : Utf8View{});
    set_editor(editor_description, exists ? Utf8View{t.description, t.description_bytes} : Utf8View{});
}

bool dirty() noexcept {
    const auto* s = owner->server_access;
    if (!s || !s->template_exists)
        return false;
    const auto& t = s->guild_template;
    return !utf8_equal(ascii_trim(title()), {t.name, t.name_bytes}) ||
           !utf8_equal(ascii_trim(description()), {t.description, t.description_bytes});
}

bool texts_valid() noexcept {
    return guild_template_title_valid(title()) && guild_template_description_valid(description());
}

void project(Bank& bank) noexcept {
    auto* s = server_access_state();
    push_category(control_list_category, utf8_literal("Server Template"),
                  utf8_literal("A server template is an easy way to share your server setup and help anyone create "
                               "a server instantly."));
    push_notices("When someone uses your server template link, they create a new server pre-filled with the same "
                 "channels, roles, permissions and settings as yours.\0\0"
                 "Channels and channel topics · Roles and permissions · Default server settings\0"
                 "Templates will copy:\0"
                 "Messages or any content · Members or bots · Your server icon, Boosts or other perks\0"
                 "Templates will not copy:");
    if (!s) {
        push_no_memory();
        return;
    }
    if (!channels_visible()) {
        push_notice(utf8_literal("You must be able to see all channels in this server to view or create server "
                                 "templates."));
        return;
    }
    if (s->templates_failed) {
        push_load_error(utf8_literal("Couldn't load the server template"));
        return;
    }
    if (!s->templates_loaded) {
        push_loading(utf8_literal("Loading the server template…"));
        return;
    }
    push_status_error();
    const bool edit = editable() && can_manage_guild();
    push_editor(control_name, utf8_literal("Template Title"), {}, UiSettingsControlKind::edit, edit, editor_name,
                utf8_literal("Who is this server for? E.g. School Club, Artists' Community"));
    if (title().bytes && !guild_template_title_valid(title()))
        push_row(template_title_error, utf8_literal("Name must be at least 2 characters long"), {},
                 UiSettingsControlKind::error, true);
    push_editor(control_description, utf8_literal("Template Description"), {}, UiSettingsControlKind::multiline, edit,
                editor_description, utf8_literal("What can people do in this server?"));
    if (!guild_template_description_valid(description()))
        push_row(template_description_error, utf8_literal("Descriptions can be up to 120 characters."), {},
                 UiSettingsControlKind::error, true);
    if (!s->template_exists) {
        push_choices(template_create_button, utf8_literal("Create a template link"),
                     utf8_literal("Anyone with the link can create a new server from this one's setup."),
                     UiSettingsControlKind::buttons, edit && !busy() && texts_valid(), {kGenerate, 1});
        return;
    }
    const auto& t = s->guild_template;
    const u32 start = bank.used;
    bank.add("https://discord.new/");
    bank.add({t.code, t.code_bytes});
    push_row(template_link, utf8_literal("Template Link"), bank.since(start), UiSettingsControlKind::action, true,
             false, utf8_literal("Copy"));
    if (t.dirty) {
        push_notice(utf8_literal("* You have unsynced changes. Update the channels, roles and permissions in your "
                                 "template by syncing."));
        if (t.updated_at_ms) {
            const u32 since = bank.used;
            bank.add("Last sync: ");
            relative_time(bank, t.updated_at_ms, clock_unix_milliseconds(), false);
            push_notice(bank.since(since));
        }
    }
    push_choices(template_actions, utf8_literal("Manage template"), {}, UiSettingsControlKind::buttons,
                 edit && !busy(), t.dirty ? ConstSlice<Utf8View>{kActions, 3} : ConstSlice<Utf8View>{kActions + 1, 2});
}

bool delete_confirmed(TextFormOwnerKey key, Utf8View) noexcept {
    if (!form_valid(key) || busy())
        return false;
    application_text_form_close(false, false);
    server_access_queue(AccessStep::template_delete);
    repaint();
    return true;
}

void confirm_delete() noexcept {
    open_confirmation(form_template_delete, 0, utf8_literal("Delete Template"),
                      utf8_literal("Are you sure you want to delete your template? This will prevent anyone with the "
                                   "link from using it."),
                      utf8_literal("Delete Template"), delete_confirmed);
}

// The snapshot the link creates: its channels and roles.
void preview() noexcept {
    const auto& t = owner->server_access->guild_template;
    open_preview(form_template_delete, 1, utf8_literal("Preview Template"), {t.name, t.name_bytes},
                 t.description_bytes ? Utf8View{t.description, t.description_bytes} : Utf8View{},
                 t.preview_bytes ? Utf8View{t.preview, t.preview_bytes}
                                 : utf8_literal("This template has no channels or roles."));
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto* s = owner->server_access;
    const u64 id = activation.value.target.control_id;
    if (!s)
        return;
    if (id == template_link && s->template_exists) {
        char link[64];
        const u32 bytes = guild_template_write_link({s->guild_template.code, s->guild_template.code_bytes},
                                                    {link, sizeof(link)});
        if (bytes)
            application_copy_text({link, bytes}, kToastLinkCopied, kToastLinkCopyFailed);
        return;
    }
    if (!can_manage_guild() || busy())
        return;
    if (id == template_create_button && !s->template_exists && texts_valid())
        server_access_queue(AccessStep::template_create);
    else if (id == template_actions && s->template_exists) {
        const u32 choice = activation.value.selected_choice + (s->guild_template.dirty ? 0 : 1);
        if (choice == kSync)
            server_access_queue(AccessStep::template_sync);
        else if (choice == kPreview) {
            preview();
            return;
        } else
            confirm_delete();
    }
    repaint();
}

bool can_save() noexcept {
    return dirty() && texts_valid() && can_manage_guild();
}

Op next_save() noexcept {
    auto* s = owner->server_access;
    if (!s || !can_save())
        return Op::none;
    s->step = AccessStep::template_update;
    return Op::server_access;
}

bool ensure_data() noexcept {
    auto* s = server_access_state();
    if (!s || s->templates_requested || !channels_visible())
        return false;
    s->templates_requested = true;
    s->templates_failed = false;
    server_access_queue(AccessStep::templates_get);
    return true;
}

void notice() noexcept {
    auto* s = owner->server_access;
    if (!s || !s->templates_failed || busy() || !connected())
        return;
    s->templates_requested = false;
    ensure_data();
}

Utf8View invalid() noexcept {
    if (!guild_template_title_valid(title()))
        return utf8_literal("Name must be at least 2 characters long");
    if (!guild_template_description_valid(description()))
        return utf8_literal("Descriptions can be up to 120 characters.");
    return {};
}

// A template reply carries the whole server: its index takes a bank sized to it.
bool decode(const Utf8View& body, bool list, GuildTemplate* output, u32* count) noexcept {
    const SIZE_T units = SIZE_T{body.bytes} + 1;
    auto* scratch = static_cast<u32*>(commit_pages(units * sizeof(u32)));
    if (!scratch)
        return false;
    bool ok = false;
    if (list) {
        *count = guild_template_decode_list(body, {scratch, static_cast<u32>(units)}, output);
        ok = *count != ~0u;
    } else {
        ok = guild_template_decode(body, {scratch, static_cast<u32>(units)}, output);
        *count = 1;
    }
    pages_release(scratch);
    return ok;
}

} // namespace

bool template_build(AccessStep step, GuildCommunityRequest* community) noexcept {
    auto* s = owner->server_access;
    const Utf8View code{s->guild_template.code, s->guild_template.code_bytes};
    switch (step) {
    case AccessStep::templates_get: community->operation = GuildCommunityOperation::templates_list; return true;
    case AccessStep::template_create:
    case AccessStep::template_update: {
        // The texts as typed, as the official form sends them.
        const u32 bytes = guild_template_write(title(), description(), {s->body, sizeof(s->body)});
        community->operation = step == AccessStep::template_create ? GuildCommunityOperation::template_create
                                                                   : GuildCommunityOperation::template_update;
        community->body = s->body;
        community->body_bytes = bytes;
        community->code = step == AccessStep::template_update ? code : Utf8View{};
        return bytes && can_manage_guild() && (step == AccessStep::template_create) != s->template_exists;
    }
    case AccessStep::template_sync:
    case AccessStep::template_delete:
        community->operation = step == AccessStep::template_sync ? GuildCommunityOperation::template_sync
                                                                 : GuildCommunityOperation::template_delete;
        community->code = code;
        return s->template_exists && can_manage_guild();
    default: return false;
    }
}

void template_response(AccessStep step, const RestClientResponse& response, const Utf8View& body) noexcept {
    auto& p = *owner;
    auto* s = owner->server_access;
    const bool ok = response.succeeded();
    // The sidebar's read may answer while another page shows: it touches neither that page's editors nor its status.
    const bool shown = p.page == Page::guild_template;
    if (step == AccessStep::templates_get) {
        u32 count = 0;
        GuildTemplate first{};
        if (!ok || !decode(body, true, &first, &count)) {
            s->templates_failed = true;
            p.failed = shown;
            p.status = !shown ? Utf8View{} : ok ? utf8_literal("The server template couldn't be read.") : p.status;
            return;
        }
        s->templates_loaded = true;
        s->template_exists = count != 0;
        if (s->template_exists)
            copy_value(&s->guild_template, first);
        if (shown)
            load();
        return;
    }
    // A 2xx whose reply outgrew the ordinary response bound still changed the template: carry on below, where an
    // unreadable reply reads the template again.
    if (!ok && (response.status_code < 200 || response.status_code >= 300))
        return;
    if (step == AccessStep::template_delete) {
        s->template_exists = false;
        reset_value(&s->guild_template);
        load();
        return;
    }
    u32 count = 0;
    GuildTemplate updated{};
    if (!ok || !decode(body, false, &updated, &count)) {
        // Saved, but the reply is unreadable: read the template again.
        s->templates_requested = false;
        s->templates_loaded = false;
        server_access_queue(AccessStep::templates_get);
        s->templates_requested = true;
        return;
    }
    copy_value(&s->guild_template, updated);
    s->template_exists = true;
    load();
}

const PageOps kTemplatePage{load,      project, activate,    nullptr, dirty,   can_save,
                            next_save, nullptr, ensure_data, notice,  invalid, nullptr};

u16 server_template_nav_badge(Page page) noexcept {
    const auto* s = owner ? owner->server_access : nullptr;
    return page == Page::guild_template && s && s->template_exists && s->guild_template.dirty ? kUiSettingsNavDot : 0;
}

bool server_template_ensure_status() noexcept {
    // Not over a page's error: queueing clears it.
    if (!owner || owner->failed || !can_manage_guild() || !channels_visible())
        return false;
    return ensure_data();
}

} // namespace maicord::app_detail::server_settings
