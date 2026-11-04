#include "internal.hpp"
#include "../../media/file_over_limit.hpp"
#include "../../account/application_account_module.hpp"
#include "../../account/account_presentation.hpp"
#include "../../chat/composer/attachments.hpp"
#include "../../chat/composer/text_input.hpp"
#include "maicord/ui/guild/context_policy.hpp"
#include "maicord/model/guild/channel_settings_model.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/app/account/account_profile_edit.hpp"
#include "../../account/account_access.hpp"

namespace maicord::app_detail::server_settings {

Settings* owner;
u64 next_form;
u64 next_form_item;

namespace {

u64 lifetime_serial;
constexpr u64 kTwoFactorRequired = 60'003;
constexpr u64 kMaximumEmoji = 30'008;
constexpr u64 kMaximumStickers = 30'039;

enum class PageGroup : u8 {
    none,
    expression,
    people,
    moderation,
};
struct PageDefinition {
    Page page;
    PageGroup group;
    UiIcon icon;
    u32 section;
    Utf8View label;
    const PageOps* ops;
};
constexpr PageDefinition kPages[]{
    {Page::profile, PageGroup::none, UiIcon::guild, guild_settings_profile, utf8_literal("Server Profile"),
     &kProfilePage},
    {Page::engagement, PageGroup::none, UiIcon::activity, guild_settings_engagement, utf8_literal("Engagement"),
     &kEngagementPage},
    {Page::boost_perks, PageGroup::none, UiIcon::boost, guild_settings_boost_perks, utf8_literal("Boost Perks"),
     &kBoostPage},
    {Page::emoji, PageGroup::expression, UiIcon::emoji, guild_settings_expressions, utf8_literal("Emoji"),
     &kExpressionPage},
    {Page::stickers, PageGroup::expression, UiIcon::sticker, guild_settings_expressions, utf8_literal("Stickers"),
     &kExpressionPage},
    {Page::soundboard, PageGroup::expression, UiIcon::audio_lines, guild_settings_expressions,
     utf8_literal("Soundboard"), &kExpressionPage},
    {Page::members, PageGroup::people, UiIcon::members, guild_settings_members, utf8_literal("Members"),
     &kMembersPage},
    {Page::roles, PageGroup::people, UiIcon::shield, guild_settings_roles, utf8_literal("Roles"), &kRolesPage},
    {Page::invites, PageGroup::people, UiIcon::user_plus, guild_settings_invites, utf8_literal("Invites"),
     &kInvitesPage},
    {Page::safety, PageGroup::moderation, UiIcon::shield_check, guild_settings_safety, utf8_literal("Safety Setup"),
     &kSafetyPage},
    {Page::audit_log, PageGroup::moderation, UiIcon::audit_log, guild_settings_audit_log, utf8_literal("Audit Log"),
     &kAuditLogPage},
    {Page::bans, PageGroup::moderation, UiIcon::gavel, guild_settings_bans, utf8_literal("Bans"), &kBansPage},
};
static_assert(sizeof(kPages) / sizeof(kPages[0]) == kPageCount);
constexpr bool pages_in_order() noexcept {
    for (u32 i = 0; i < kPageCount; ++i)
        if (static_cast<u32>(kPages[i].page) != i + 1)
            return false;
    return true;
}
static_assert(pages_in_order());
constexpr Utf8View kGroupHeaders[]{{}, utf8_literal("EXPRESSION"), utf8_literal("PEOPLE"), utf8_literal("MODERATION")};

const PageDefinition& page_definition(Page page) noexcept {
    const u32 index = static_cast<u32>(page);
    return kPages[index && index <= kPageCount ? index - 1 : 0];
}
const PageOps& ops() noexcept {
    return *page_definition(owner->page).ops;
}

} // namespace

const ChatStateStore& state() noexcept {
    return g_app.account_ui->chat_state;
}
const GuildDetailStore& details() noexcept {
    return g_app.account_ui->guild_details;
}
const GuildDetailRecord* guild_details() noexcept {
    return guild_detail_store_find(details(), owner->guild_id);
}
u8 premium_tier() noexcept {
    const auto* record = guild_details();
    return record ? record->premium_tier : 0;
}
u64 guild_features() noexcept {
    const auto* record = guild_details();
    return record ? record->features : 0;
}
bool scope_current() noexcept {
    if (!owner || !application_account_available())
        return false;
    const auto* guild = chat_state_store_find_guild(state(), owner->guild_id);
    return guild && !guild->unavailable && guild->generation == owner->guild_generation &&
           owner->account_scope == g_app.plugin_account_scope_serial &&
           owner->profile_id == g_app.account.status.profile_id && owner->self_user_id == state().self_user_id;
}
bool connected() noexcept {
    return g_app.account.instance && g_app.account.status.running && g_app.account.status.gateway_ready &&
           g_app.account.api.guild_admin;
}
bool busy() noexcept {
    return owner->request.queued || owner->request_id;
}

namespace guild_settings_detail {

bool read_op(Op op) noexcept {
    return op == Op::role_counts || op == Op::ban_list || op == Op::invite_list || op == Op::audit_page ||
           op == Op::sticker_list || op == Op::widget_get || op == Op::profile_get || op == Op::vanity_get ||
           op == Op::members_search || op == Op::ban_search;
}

} // namespace guild_settings_detail

bool mutation_pending() noexcept {
    return busy() && !read_op(owner->request.op);
}
bool page_allowed(Page page) noexcept {
    return page != Page::none && page < Page::count && (owner->access.sections & page_definition(page).section);
}
bool can_manage_guild() noexcept {
    return guild_settings_can_manage_guild(owner->access);
}

namespace {

Page first_page() noexcept {
    for (const auto& definition : kPages)
        if (page_allowed(definition.page))
            return definition.page;
    return Page::none;
}
UiSettingsPageContent& page_content(Page page) noexcept {
    return owner->content[static_cast<u32>(page) - 1];
}
bool page_dirty() noexcept {
    return owner->page != Page::none && ops().dirty && ops().dirty();
}

} // namespace

const RoleRecord* role(u64 id) noexcept {
    return id ? chat_state_store_find_role(state(), owner->guild_id, id) : nullptr;
}
bool pending(const Picked& picked) noexcept {
    return picked.set || picked.clear;
}
void release_picked(Picked* picked) noexcept {
    if (picked->data) {
        secure_clear_bytes(picked->data, picked->capacity);
        pages_release(picked->data);
    }
    reset_value(picked);
}

namespace {

// The icon, banner and splash picks and their data URIs.
void release_images() noexcept {
    auto& p = *owner;
    release_picked(&p.icon);
    release_picked(&p.banner);
    release_picked(&p.splash);
    release_uri(&p.icon_uri, &p.icon_uri_bytes);
    release_uri(&p.banner_uri, &p.banner_uri_bytes);
    release_uri(&p.splash_uri, &p.splash_uri_bytes);
}

} // namespace

bool editable() noexcept {
    return owner && owner->scene.active && scope_current() && !mutation_pending() && !owner->layer.choice &&
           !g_app.text_form;
}

namespace {

struct EditorStorage {
    char* text;
    u32 capacity, codepoints;
    bool multiline;
};
EditorStorage editor_storage(Editor index) noexcept {
    auto& p = *owner;
    switch (index) {
    case editor_name: return {p.name_text, kGuildAdminNameBytes, kGuildAdminNameCodepoints, false};
    case editor_hex: return {p.hex_text, 16, 7, false};
    case editor_description:
        return {p.description_text, kGuildAdminProfileDescriptionBytes, kGuildAdminProfileDescriptionCodepoints, true};
    case editor_vanity: return {p.vanity_text, kGuildAdminVanityCodeBytes, kGuildAdminVanityCodeCharacters, false};
    default: return {p.search_text, kSearchBytes, 100, false};
    }
}

} // namespace

void set_editor(Editor index, const Utf8View& text) noexcept {
    application_composer_text_input_suspend();
    const auto storage = editor_storage(index);
    const u32 bytes = minimum(text.bytes, storage.capacity);
    if (text.data != storage.text)
        copy_bytes(storage.text, text.data, bytes);
    Editors::load(index, bytes, storage.codepoints, storage.multiline);
}

namespace {

Editor editor_of(u64 control) noexcept {
    return control == control_name          ? editor_name
           : control == control_hex         ? editor_hex
           : control == control_search      ? editor_search
           : control == control_description ? editor_description
           : control == control_vanity      ? editor_vanity
                                            : editor_count;
}

} // namespace

Utf8View editor_text(Editor index) noexcept {
    return message_composer_text(owner->models[index]);
}

namespace {

// Reserves the next control slot with its presentation, or ~0u when the page is full.
u32 claim_control(UiSettingsControlPresentation presentation) noexcept {
    if (owner->control_count >= kControlCapacity)
        return ~0u;
    owner->presentation[owner->control_count] = presentation;
    reset_value(&owner->avatar_media[owner->control_count]);
    reset_value(&owner->list_items[owner->control_count]);
    return owner->control_count++;
}

} // namespace

void popup_row(u32 index, const Utf8View& label, u32 action, u32 flags) noexcept {
    auto& item = owner->popup_items[index];
    reset_value(&item);
    item.label = label;
    item.action = kMenuChoiceBase + action;
    item.flags = static_cast<u16>(popup_item_enabled | flags);
}
u32 push_row(u64 id, const Utf8View& label, const Utf8View& description, UiSettingsControlKind kind, bool enabled,
             bool checked, const Utf8View& action, UiSettingsControlPresentation presentation) noexcept {
    const u32 index = claim_control(presentation);
    if (index == ~0u)
        return index;
    auto& control = owner->controls[index];
    control.id = id;
    control.label = label;
    control.description = description;
    control.kind = kind;
    control.enabled = enabled;
    control.checked = checked;
    control.choices = {};
    control.selected_choice = 0;
    control.action_label = action;
    control.editor = nullptr;
    return index;
}
u32 push_choices(u64 id, const Utf8View& label, const Utf8View& description, UiSettingsControlKind kind, bool enabled,
                 const ConstSlice<Utf8View>& choices, u32 selected, const Utf8View& action,
                 UiSettingsControlPresentation presentation) noexcept {
    const u32 index = push_row(id, label, description, kind, enabled, false, action, presentation);
    if (index != ~0u) {
        owner->controls[index].choices = choices;
        owner->controls[index].selected_choice = selected;
    }
    return index;
}
u32 push_editor(u64 id, const Utf8View& label, const Utf8View& description, UiSettingsControlKind kind, bool enabled,
                Editor index, const Utf8View& placeholder, const Utf8View& action) noexcept {
    const UiSettingsEditor* view = Editors::view(index, id, placeholder, enabled);
    const u32 row = push_row(id, label, description, kind, enabled, false, action);
    if (row != ~0u)
        owner->controls[row].editor = view;
    return row;
}
u32 push_category(u64 id, const Utf8View& label, const Utf8View& description) noexcept {
    return push_row(id, label, description, UiSettingsControlKind::category, true);
}
u32 push_notice(const Utf8View& description, const Utf8View& label) noexcept {
    return push_row(control_notice, label, description, UiSettingsControlKind::notice, true);
}
u32 push_error(const Utf8View& label, const Utf8View& description, const Utf8View& action) noexcept {
    return push_row(control_notice, label, description, UiSettingsControlKind::notice, true, false, action,
                    {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
}
u32 push_loading(const Utf8View& description, u64 id) noexcept {
    return push_row(id, {}, description, UiSettingsControlKind::notice, true, true);
}
u32 push_count_heading(Bank& bank, const Utf8View& title, u32 count) noexcept {
    const u32 start = bank.used;
    bank.add(title);
    bank.number(count);
    return push_category(control_list_category, bank.since(start));
}
Utf8View channel_label(Bank& bank, u64 channel_id, const Utf8View& none) noexcept {
    const auto* channel = channel_id ? chat_state_store_find_channel(state(), channel_id) : nullptr;
    if (!channel)
        return none;
    const u32 start = bank.used;
    bank.add(channel->type == 2 ? utf8_literal("") : utf8_literal("#"));
    bank.add(chat_state_store_channel_name(state(), *channel));
    return bank.since(start);
}

// "512×512 PNG · 1.0 MB", plus the source size when it was resized.
void image_summary(Bank& bank, const GuildImageInfo& info) noexcept {
    static constexpr Utf8View kinds[]{{}, utf8_literal(" PNG"), utf8_literal(" APNG"), utf8_literal(" GIF"),
                               utf8_literal(" JPEG"), utf8_literal(" WebP"), utf8_literal(" Lottie")};
    if (info.width) {
        bank.number(info.width);
        bank.add("×");
        bank.number(info.height);
    }
    bank.add(kinds[static_cast<u32>(info.kind) < 7 ? static_cast<u32>(info.kind) : 0]);
    if (info.animated)
        bank.add(" (animated)");
    bank.add(" · ");
    if (info.bytes >= 1024 * 1024) {
        bank.number(info.bytes / (1024 * 1024));
        bank.add(".");
        bank.number(info.bytes % (1024 * 1024) * 10 / (1024 * 1024));
        bank.add(" MB");
    } else {
        bank.number((info.bytes + 1023) / 1024);
        bank.add(" KB");
    }
    if (info.resized) {
        bank.add(", resized from ");
        bank.number(info.source_width);
        bank.add("×");
        bank.number(info.source_height);
    }
}
// Description of an image row: the pending change, or the current state.
Utf8View picked_text(Bank& bank, const Picked& picked, const Utf8View& current) noexcept {
    if (picked.clear)
        return utf8_literal("Removed when you save.");
    if (!picked.set)
        return current;
    const u32 start = bank.used;
    bank.add("New: ");
    image_summary(bank, picked.info);
    bank.add(". Save to apply.");
    return bank.since(start);
}

void relative_time(Bank& bank, u64 then_ms, u64 now_ms, bool future) noexcept {
    const u64 delta = future ? (then_ms > now_ms ? then_ms - now_ms : 0) : (now_ms > then_ms ? now_ms - then_ms : 0);
    const u64 minutes = delta / 60'000, hours = minutes / 60, days = hours / 24;
    if (!future && minutes < 1) {
        bank.add("just now");
        return;
    }
    if (future)
        bank.add("in ");
    const u64 value = days ? days : hours ? hours : minutes ? minutes : 1;
    bank.number(value);
    bank.add(days    ? (value == 1 ? utf8_literal(" day") : utf8_literal(" days"))
             : hours ? (value == 1 ? utf8_literal(" hour") : utf8_literal(" hours"))
                     : (value == 1 ? utf8_literal(" minute") : utf8_literal(" minutes")));
    if (!future)
        bank.add(" ago");
}
Utf8View user_name(const GuildAdminUser& user) noexcept {
    return user.display_bytes ? Utf8View{user.display, user.display_bytes}
                              : Utf8View{user.username, user.username_bytes};
}
Utf8View user_label(u64 id) noexcept {
    for (u32 i = 0; i < owner->audit_user_count; ++i)
        if (owner->audit_users[i].id == id)
            return user_name(owner->audit_users[i]);
    const auto& users = g_app.account_ui->user_store;
    if (const auto* user = user_store_find(users, id)) {
        const auto name = user_store_name(users, *user);
        if (name.bytes)
            return name;
    }
    return {};
}

namespace {

Utf8View footer_status() noexcept {
    if (owner->request_id && mutation_pending())
        return utf8_literal("Saving changes…");
    if (owner->failed && owner->status.bytes)
        return owner->status;
    if (ops().invalid) {
        const auto invalid = ops().invalid();
        if (invalid.bytes)
            return invalid;
    }
    return utf8_literal("Careful — you have unsaved changes!");
}
bool can_save() noexcept {
    return editable() && connected() && ops().can_save && ops().can_save();
}

} // namespace
namespace guild_settings_detail {

// The next request of a multi-part save.
Op next_save_op() noexcept {
    return ops().next_save ? ops().next_save() : Op::none;
}

} // namespace guild_settings_detail
namespace {

bool resolve_color(void*, u64 id, u32 choice, BackgroundTheme* theme) noexcept {
    if (!owner || !theme)
        return false;
    auto& p = *owner;
    u32 rgba = 0;
    if (id >= kRowBase && id < kRowBase + kRowCapacity) {
        const u32 row = static_cast<u32>(id - kRowBase);
        rgba = choice == kUiSettingsAccentChoice ? p.row_accent[row] : choice < 4 ? p.chip_colors[row][choice] : 0;
    } else if (p.page == Page::roles && p.editing_role && id == control_header && choice == kUiSettingsAccentChoice)
        rgba = guild_role_color_rgba(p.role_draft.color);
    else if (p.page == Page::roles && p.editing_role && id == control_color && choice <= kGuildRoleColorCount)
        rgba = guild_role_color_rgba(choice ? kGuildRoleColors[choice - 1] : 0);
    else if (p.page == Page::profile && id == control_banner_colors && choice <= kGuildProfileBannerColorCount) {
        // The first swatch stands for the icon's own colour.
        u32 color = 0;
        if (choice)
            color_hex_parse_rgb24(kGuildProfileBannerColors[choice - 1], &color);
        rgba = guild_role_color_rgba(color);
    }
    if (!rgba)
        return false;
    reset_value(theme);
    theme->colors[0] = rgba >> 8;
    theme->color_count = 1;
    return true;
}
bool project_page() noexcept {
    if (!owner || !owner->scene.active || owner->page == Page::none)
        return true;
    auto& p = *owner;
    p.bank_index ^= 1;
    Bank bank{p.banks[p.bank_index], kBankBytes, 0};
    p.control_count = 0;
    ops().project(bank);
    const bool dirty = page_dirty(), saving = p.request_id && mutation_pending();
    p.footer = {footer_status(), utf8_literal("Save changes"), utf8_literal("Reset"), dirty || saving, can_save(), saving,
                p.failed};
    for (u32 i = 1; i <= kPageCount; ++i) {
        auto& content = page_content(static_cast<Page>(i));
        const bool selected = static_cast<Page>(i) == p.page;
        content.controls = selected ? ConstSlice<UiSettingsControl>{p.controls, p.control_count}
                                    : ConstSlice<UiSettingsControl>{};
        content.presentation = selected ? p.presentation : nullptr;
        content.avatar_media = selected ? p.avatar_media : nullptr;
        content.list_items = selected ? p.list_items : nullptr;
        content.footer = selected ? &p.footer : nullptr;
        if (selected) {
            if (p.page == Page::roles && p.editing_role) {
                const u32 start = bank.used;
                bank.add("Edit role — ");
                bank.add({p.role_draft.name, p.role_draft.name_bytes});
                content.title = bank.since(start);
            } else
                content.title = page_definition(p.page).label;
            content.description = p.page == Page::roles && !p.editing_role
                                      ? utf8_literal("Use roles to group your server members and assign permissions.")
                                      : Utf8View{};
            ++content.revision;
        }
    }
    ui_settings_scene_set_focus_visible(&p.scene, g_app.keyboard_focus_visible);
    ui_settings_scene_bind_palette(&p.scene, {owner, resolve_color, ++p.palette_revision});
    return ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips());
}

void build_navigation() noexcept {
    auto& p = *owner;
    const auto* guild = chat_state_store_find_guild(state(), p.guild_id);
    static char header[kGuildAdminNameBytes];
    u32 header_bytes = 0;
    if (guild) {
        const auto name = chat_state_store_guild_name(state(), *guild);
        header_bytes = minimum(name.bytes, kGuildAdminNameBytes);
        copy_bytes(header, name.data, header_bytes);
        // Sidebar headings are uppercase eyebrows.
        for (u32 i = 0; i < header_bytes; ++i)
            if (header[i] >= 'a' && header[i] <= 'z')
                header[i] = static_cast<char>(header[i] - 32);
    }
    p.nav_count = 0;
    const auto add = [&](UiSettingsNavEntry entry) noexcept { p.nav[p.nav_count++] = entry; };
    add({UiSettingsNavKind::header, false, UiIcon::count,
         header_bytes ? Utf8View{header, header_bytes} : utf8_literal("SERVER SETTINGS"), {}, 0});
    PageGroup group = PageGroup::none;
    for (const auto& definition : kPages) {
        if (!page_allowed(definition.page))
            continue;
        if (definition.group != group) {
            group = definition.group;
            if (group != PageGroup::none)
                add({UiSettingsNavKind::header, false, UiIcon::count, kGroupHeaders[static_cast<u32>(group)], {},
                     0});
        }
        add({UiSettingsNavKind::page, false, definition.icon, definition.label, page_key(definition.page), 0});
    }
    if (guild_settings_can_delete(p.access)) {
        add({UiSettingsNavKind::separator, false, UiIcon::count, {}, {}, 0});
        add({UiSettingsNavKind::action, true, UiIcon::trash, utf8_literal("Delete Server"), {}, kDeleteServerAction});
    }
    ui_settings_scene_bind_navigation(&p.scene, {p.nav, p.nav_count}, ++p.nav_revision);
}

} // namespace

bool repaint() noexcept {
    return settings_modal_present(project_page());
}

namespace {

void clear_status() noexcept {
    owner->failed = false;
    reset_value(&owner->status);
}
// The picked file's name as UTF-8, for the expression name forms.
void set_upload_filename(ConstSlice<wchar_t> filename) noexcept {
    auto& p = *owner;
    const int units = WideCharToMultiByte(CP_UTF8, 0, filename.data, static_cast<int>(filename.count),
                                          p.upload_filename, sizeof(p.upload_filename), nullptr, nullptr);
    p.upload_filename_bytes = units > 0 ? static_cast<u32>(units) : 0;
}

} // namespace

void queue(Op op, u64 target) noexcept {
    auto& r = owner->request;
    reset_value(&r);
    r.op = op;
    r.target = target;
    r.queued = true;
    owner->retry_at = 0;
    clear_status();
    application_request_frame();
}
bool request_read(bool& requested, bool& failed, Op op) noexcept {
    if (requested)
        return false;
    requested = true;
    failed = false;
    queue(op);
    return true;
}
void retry_read(bool& requested, bool& failed, Op op) noexcept {
    if (!failed || busy() || !connected())
        return;
    requested = false;
    request_read(requested, failed, op);
}

namespace guild_settings_detail {

Utf8View failure_text(const RestClientResponse& response) noexcept {
    if (response.error != RestClientError::none)
        return utf8_literal("Couldn't reach Discord. Try again.");
    switch (response.status_code) {
    case 403: return utf8_literal("You don't have permission to do this.");
    case 404: return utf8_literal("This no longer exists.");
    case 429: return utf8_literal("Too many changes at once. Wait a moment and try again.");
    case 400: return utf8_literal("Discord rejected this change.");
    default: return utf8_literal("Something went wrong. Try again.");
    }
}
// Service error codes worth their own wording; otherwise the status text.
Utf8View failure_text(const RestClientResponse& response, const Utf8View& body) noexcept {
    if (response.error == RestClientError::none && response.status_code >= 400 && body.bytes) {
        const u64 code = guild_asset_error_code(body, {owner->scratch, kScratchUnits});
        if (code == kTwoFactorRequired)
            return utf8_literal("Discord needs your two-factor code for this. Use Discord in your browser.");
        if (code == kMaximumEmoji)
            return utf8_literal("This server has no free emoji slots.");
        if (code == kMaximumStickers)
            return utf8_literal("This server has no free sticker slots.");
    }
    return failure_text(response);
}
void ensure_page_data() noexcept {
    if (owner->page == Page::soundboard && connected())
        application_soundboard_request_guild(owner->guild_id);
    if (busy() || !connected() || !ops().ensure_data)
        return;
    ops().ensure_data();
}

ChannelCreate creation{};

} // namespace guild_settings_detail
namespace {

u64 creation_serial{};

bool creation_current(TextFormOwnerKey key) noexcept {
    return creation.serial && key.scope == creation.serial && key.collection == creation.guild_id &&
           g_app.account_ui && !g_app.login.visible && creation.account_scope == g_app.plugin_account_scope_serial &&
           guild_context_can_create_channel(g_app.account_ui->chat_state, creation.parent_id ? 0 : creation.guild_id,
                                            creation.parent_id);
}
bool creation_save(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!creation_current(key) || creation.queued || creation.request_id)
        return false;
    if (!channel_settings_name_valid(text) || text.bytes > kGuildAdminNameBytes) {
        creation.status = creation.type == 4 ? utf8_literal("Category names need 1–100 characters on one line.")
                                             : utf8_literal("Channel names need 1–100 characters on one line.");
        ++creation.revision;
        return false;
    }
    copy_bytes(creation.name, text.data, text.bytes);
    creation.name_bytes = static_cast<u16>(text.bytes);
    creation.queued = true;
    creation.retry_at = 0;
    reset_value(&creation.status);
    ++creation.revision;
    application_request_frame();
    return false;
}
ApplicationTextFormProgress creation_progress(TextFormOwnerKey key) noexcept {
    ApplicationTextFormProgress view{};
    view.revision = creation.revision;
    if (!creation_current(key) || creation.done) {
        view.complete = true;
        return view;
    }
    if (creation.queued || creation.request_id) {
        view.status = utf8_literal("Creating…");
        view.editable = view.can_accept = view.can_cancel = view.dismissible = false;
        return view;
    }
    view.status = creation.status;
    return view;
}
void creation_closed(TextFormOwnerKey key) noexcept {
    if (key.scope == creation.serial && !creation.request_id)
        reset_value(&creation);
}

} // namespace

bool guard_dirty() noexcept {
    if (!page_dirty())
        return false;
    settings_layer_shake(&owner->layer);
    return true;
}
TextFormOwnerKey form_key(u64 kind, u64 item) noexcept {
    return {owner ? owner->lifetime : 0, kind, item};
}
bool form_valid(TextFormOwnerKey key) noexcept {
    return owner && owner->scene.active && key.scope == owner->lifetime && scope_current();
}
bool form_save(TextFormOwnerKey key, Utf8View) noexcept {
    if (!form_valid(key) || busy())
        return false;
    application_text_form_close(false, false);
    if (key.collection == form_role_delete) {
        const auto* r = role(key.item);
        if (r && guild_settings_role_deletable(owner->access, *r))
            queue(Op::role_delete, key.item);
    } else if (key.collection == form_unban) {
        if (guild_settings_can_unban(owner->access))
            queue(Op::ban_remove, key.item);
    } else if (key.collection == form_invite_revoke) {
        if (page_allowed(Page::invites) && key.item < owner->invite_count)
            queue(Op::invite_delete, key.item);
    }
    repaint();
    return true;
}

namespace {

GuildImageProfile pick_profile(Pick pick) noexcept {
    switch (pick) {
    case Pick::banner: return GuildImageProfile::banner;
    case Pick::role_icon: return GuildImageProfile::role_icon;
    case Pick::emoji: return GuildImageProfile::emoji;
    case Pick::sticker: return GuildImageProfile::sticker;
    default: return GuildImageProfile::count;
    }
}

} // namespace

void show_failure(Utf8View text) noexcept {
    owner->failed = true;
    owner->status = text;
    repaint();
}
void start_pick(Pick pick) noexcept {
    auto& p = *owner;
    if (p.picker.runtime || !connected() || !editable())
        return;
    static constexpr Utf8View titles[]{{},
                                utf8_literal("Choose a server icon"),
                                utf8_literal("Choose a server banner"),
                                utf8_literal("Choose a role icon"),
                                utf8_literal("Choose an emoji image"),
                                utf8_literal("Choose a sticker file"),
                                utf8_literal("Choose a sound file"),
                                utf8_literal("Choose an invite background")};
    p.pick = pick;
    p.pick_role = pick == Pick::role_icon ? p.editing_role : 0;
    p.picker_consumed = false;
    if (native_file_picker_start(&p.picker, g_app.window, 1, titles[static_cast<u32>(pick)]) !=
        NativeFilePickerError::none) {
        p.pick = Pick::none;
        show_failure(utf8_literal("Couldn't open the file picker."));
    }
}

namespace {

bool read_picked_file(ConstSlice<wchar_t> path, Picked* out, Utf8View* error) noexcept {
    u8* data = nullptr;
    u32 bytes = 0;
    if (!application_image_file_read(path, &data, &bytes, error, {owner->size_error, sizeof(owner->size_error)}))
        return false;
    release_picked(out);
    out->data = data;
    out->bytes = out->capacity = bytes;
    return true;
}
// Validates through the account module; the same preparation runs at submit.
bool probe_picked(GuildImageProfile profile, Picked* picked, ConstSlice<wchar_t> path, Utf8View* error) noexcept {
    if (application_guild_image_probe(profile, {picked->data, picked->bytes}, &picked->info))
        return true;
    *error = application_guild_image_error_text(
        profile, picked->info.error == GuildImageError::none ? GuildImageError::input : picked->info.error, path,
        picked->bytes, {owner->size_error, sizeof(owner->size_error)});
    return false;
}
// Server icon and invite background: the official chooser's checks (10 MB,
// PNG, JPEG, WebP or GIF), probed by the account module's cropper.
bool probe_profile_image(Picked* picked, ConstSlice<wchar_t> path, ProfileImageInfo* info, Utf8View* error) noexcept {
    ProfileImageRequest request{};
    request.source = {picked->data, picked->bytes};
    request.info = info;
    reset_value(info);
    if (picked->bytes > kProfileImageSourceMaximumBytes) {
        *error = application_file_over_limit_text(path, picked->bytes, kProfileImageSourceMaximumBytes,
                                                  {owner->size_error, sizeof(owner->size_error)});
        return false;
    }
    if (g_app.account.api.profile_image && g_app.account.instance &&
        g_app.account.api.profile_image(g_app.account.instance, &request) && info->error == ProfileImageError::none)
        return true;
    *error = info->error == ProfileImageError::format ? utf8_literal("Choose a PNG, JPEG, WebP or GIF image.")
                                                      : utf8_literal("This image couldn't be read.");
    return false;
}
void accept_pick(Pick pick, ConstSlice<wchar_t> path, ConstSlice<wchar_t> filename) noexcept {
    auto& p = *owner;
    Picked* slot = pick == Pick::icon       ? &p.icon
                   : pick == Pick::banner   ? &p.banner
                   : pick == Pick::splash   ? &p.splash
                   : pick == Pick::role_icon ? &p.role_icon
                                             : &p.upload;
    if (pick == Pick::role_icon && p.pick_role != p.editing_role)
        return;
    Picked picked{};
    ProfileImageInfo info{};
    Utf8View error{};
    bool ok = read_picked_file(path, &picked, &error);
    if (ok && (pick == Pick::icon || pick == Pick::splash))
        ok = probe_profile_image(&picked, path, &info, &error);
    else if (ok && pick == Pick::sound) {
        // MP3/Ogg checked by the account module's demuxers (size, length).
        SoundboardFileInfo sound{};
        ok = application_soundboard_probe({picked.data, picked.bytes}, &sound, &error);
        if (!ok && sound.error == SoundboardFileError::too_large)
            error = application_file_over_limit_text(path, picked.bytes, kSoundboardMaximumUploadBytes,
                                                     {p.size_error, sizeof(p.size_error)});
    } else if (ok) {
        ok = probe_picked(pick_profile(pick), &picked, path, &error);
        // A GIF banner needs ANIMATED_BANNER; the official chooser takes 10 MB.
        if (ok && pick == Pick::banner && picked.info.source_kind == GuildImageKind::gif &&
            !(guild_features() & guild_feature_animated_banner)) {
            error = utf8_literal("Animated server banners need boost level 3.");
            ok = false;
        } else if (ok && pick == Pick::banner && picked.bytes > kProfileImageSourceMaximumBytes) {
            error = application_file_over_limit_text(path, picked.bytes, kProfileImageSourceMaximumBytes,
                                                     {p.size_error, sizeof(p.size_error)});
            ok = false;
        }
    }
    if (!ok) {
        release_picked(&picked);
        show_failure(error);
        return;
    }
    release_picked(slot);
    *slot = picked;
    slot->set = true;
    switch (pick) {
    case Pick::icon:
        p.icon_info = info;
        release_uri(&p.icon_uri, &p.icon_uri_bytes);
        p.profile_draft.icon = GuildProfileImageEdit::replace;
        break;
    case Pick::splash:
        slot->info.source_kind = info.kind;
        slot->info.kind = info.kind;
        slot->info.width = info.width;
        slot->info.height = info.height;
        slot->info.bytes = slot->bytes;
        release_uri(&p.splash_uri, &p.splash_uri_bytes);
        p.boost_draft.splash = GuildProfileImageEdit::replace;
        break;
    case Pick::banner:
        release_uri(&p.banner_uri, &p.banner_uri_bytes);
        p.boost_draft.banner = GuildProfileImageEdit::replace;
        break;
    case Pick::sound:
    case Pick::emoji:
    case Pick::sticker:
        set_upload_filename(filename);
        next_form = pick == Pick::emoji ? form_emoji_create : pick == Pick::sticker ? form_sticker_name : form_sound_name;
        next_form_item = 0;
        break;
    default: break;
    }
    clear_status();
    repaint();
}

} // namespace
namespace guild_settings_detail {

void poll_picker() noexcept {
    auto& p = *owner;
    if (!p.picker.runtime)
        return;
    const auto status = native_file_picker_status(&p.picker);
    if (status.phase != NativeFilePickerPhase::complete)
        return;
    if (!p.picker_consumed) {
        p.picker_consumed = true;
        const Pick pick = p.pick;
        p.pick = Pick::none;
        if (status.ok() && status.count == 1 && pick != Pick::none && p.scene.active && scope_current())
            accept_pick(pick, native_file_picker_path(&p.picker, 0), native_file_picker_filename(&p.picker, 0));
        else if (!status.ok() && status.error != NativeFilePickerError::cancelled && p.scene.active)
            show_failure(utf8_literal("Couldn't open that file."));
    }
    native_file_picker_release(&p.picker, 0);
}

} // namespace guild_settings_detail

void open_channel_menu(Menu menu, u32 types, u64 current, const Utf8View& none, PermissionSet required,
                       const UiRect& anchor) noexcept {
    auto& p = *owner;
    Bank bank{p.popup_text, sizeof(p.popup_text), 0};
    u32 count = 0;
    popup_row(count, none, 0, !current ? popup_item_checked : 0);
    p.menu_channel[count++] = 0;
    const auto& s = state();
    const u32 first = count;
    const u32 slots = chat_state_store_channel_slot_bound(s);
    for (u32 i = 0; i < slots && count < kPopupCapacity; ++i) {
        const auto& channel = s.channels[i];
        if (!channel.occupied || channel.guild_id != p.guild_id || channel.type >= 32 ||
            !(types & (1u << channel.type)))
            continue;
        // Channels the account cannot reach stay out unless already chosen.
        if (channel.id != current && channel.permissions_resolved &&
            (channel.effective_permissions & required) != required)
            continue;
        const u32 start = bank.used;
        if (channel.type != 2 && channel.type != 13)
            bank.add("#");
        bank.add(chat_state_store_channel_name(s, channel));
        popup_row(count, bank.since(start), 0, channel.id == current ? popup_item_checked : 0);
        p.menu_channel[count++] = channel.id;
    }
    // The invite picker lists text-like channels before voice; the others
    // follow the channel list.
    const auto order =
        menu == Menu::widget_channel ? GuildChannelPickerOrder::invite : GuildChannelPickerOrder::channel_list;
    using Row = Settings::PopupRow;
    auto& rows = p.popup_rows;
    for (u32 i = first; i < count; ++i)
        rows[i - first] = {p.popup_items[i], chat_state_store_find_channel(s, p.menu_channel[i])};
    heap_sort_shared(rows, count - first, [&s, order](const Row& a, const Row& b) noexcept {
        return a.channel && b.channel && guild_settings_channel_before(s, order, *a.channel, *b.channel);
    });
    for (u32 i = first; i < count; ++i) {
        p.popup_items[i] = rows[i - first].item;
        p.popup_items[i].action = kMenuChoiceBase + i;
        p.popup_items[i].flags = static_cast<u16>((p.popup_items[i].flags & ~popup_item_separator_before) |
                                                  (i == first ? popup_item_separator_before : 0));
        p.menu_channel[i] = rows[i - first].channel ? rows[i - first].channel->id : 0;
    }
    p.menu = menu;
    settings_layer_open_choice(&p.layer, p.guild_id, {p.popup_items, count}, anchor, 280);
}

namespace {

void activate(SettingsLayer*, const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    const auto target = activation.value.target;
    switch (target.kind) {
    case UiSettingsTargetKind::close:
        request_close();
        return;
    case UiSettingsTargetKind::page: {
        const auto page = static_cast<Page>(target.page.page_id);
        if (page == p.page || !page_allowed(page) || guard_dirty())
            return;
        select_page(page);
        return;
    }
    case UiSettingsTargetKind::save: {
        if (!can_save())
            return;
        const Op op = next_save_op();
        if (op == Op::none)
            return;
        queue(op, op == Op::role_icon ? p.editing_role : 0);
        application_composer_text_input_suspend();
        repaint();
        return;
    }
    case UiSettingsTargetKind::reset:
        if (mutation_pending())
            return;
        clear_status();
        if (ops().load)
            ops().load();
        repaint();
        return;
    case UiSettingsTargetKind::action:
        if (target.control_id == kDeleteServerAction && guild_settings_can_delete(p.access) && editable() &&
            !busy() && !guard_dirty())
            open_expression_form(form_delete_server, 0);
        return;
    case UiSettingsTargetKind::control:
        break;
    default:
        return;
    }
    const u64 id = target.control_id;
    // A field's own button (the role search's Create Role) acts; the field focuses.
    if (editor_of(id) != editor_count && !(activation.pointer && activation.button)) {
        settings_layer_focus(&p.layer, target);
        return;
    }
    if (id == control_notice) {
        if (ops().notice)
            ops().notice();
        repaint();
        return;
    }
    if (editable() && ops().activate)
        ops().activate(activation);
}

void choose(SettingsLayer*, u32 action) noexcept {
    auto& p = *owner;
    const Menu menu = p.menu;
    p.menu = Menu::none;
    if (!action || !editable() || !ops().choose)
        return;
    ops().choose(menu, action);
}

bool edited(SettingsLayer*, u64 control) noexcept {
    auto& p = *owner;
    const Editor editor = editor_of(control);
    const auto text = editor_text(editor);
    if (editor == editor_description && p.page == Page::profile) {
        auto& d = p.profile_draft.profile;
        d.description_bytes = static_cast<u16>(minimum(text.bytes, kGuildAdminProfileDescriptionBytes));
        copy_bytes(d.description, text.data, d.description_bytes);
    } else if (editor == editor_vanity && p.page == Page::boost_perks) {
        // Spaces become dashes as the official field types them.
        char code[kGuildAdminVanityCodeBytes];
        const u32 bytes = guild_vanity_code_input(text, {code, sizeof(code)});
        if (bytes != text.bytes || !bytes_equal(code, text.data, bytes))
            set_editor(editor_vanity, {code, bytes});
        p.vanity_error = false;
    } else if (editor == editor_name) {
        if (p.page == Page::profile) {
            auto& d = p.profile_draft.profile;
            d.name_bytes = static_cast<u16>(minimum(text.bytes, kGuildAdminNameBytes));
            copy_bytes(d.name, text.data, d.name_bytes);
            d.name_known = true;
        } else if (p.page == Page::roles && p.editing_role) {
            p.role_draft.name_bytes = static_cast<u16>(minimum(text.bytes, kGuildAdminNameBytes));
            copy_bytes(p.role_draft.name, text.data, p.role_draft.name_bytes);
        }
    } else if (editor == editor_hex && p.page == Page::roles && p.editing_role) {
        u32 color = 0;
        p.hex_invalid = text.bytes && !color_hex_parse_rgb24(text, &color);
        if (!text.bytes)
            p.role_draft.color = 0;
        else if (!p.hex_invalid)
            p.role_draft.color = color;
    } else if (editor == editor_search) {
        p.scene.content_offset = 0;
    }
    return true;
}
SettingsLayerEditor editor_hook(SettingsLayer*, u64 control) noexcept {
    const Editor editor = editor_of(control);
    return editor == editor_count
               ? SettingsLayerEditor{}
               : SettingsLayerEditor{&owner->models[editor], &owner->bindings[editor], editor == editor_description};
}
void context_hook(SettingsLayer*, const SettingsLayerActivation& activation) noexcept {
    if (editable() && ops().context)
        ops().context(activation);
}
void editor_enter_hook(SettingsLayer*, u64 control) noexcept {
    if (editable() && control == control_search)
        bans_editor_enter();
}
constexpr SettingsLayerHooks kHooks{editor_hook, edited, activate, choose,
                                    [](SettingsLayer*) noexcept { request_close(); },
                                    [](SettingsLayer*) noexcept { return repaint(); },
                                    [](SettingsLayer*, u64 dragged, u64 target, bool after) noexcept {
                                        roles_drop(dragged, target, after);
                                    },
                                    [](SettingsLayer*) noexcept { return editable(); },
                                    context_hook, editor_enter_hook};

// Store reconciliation: rebaseline clean drafts, flag dirty ones.
bool reconcile() noexcept {
    return ops().reconcile && ops().reconcile(clock_monotonic_milliseconds());
}
u64 signature() noexcept {
    const auto& s = state();
    u64 h = s.guild_mutations;
    h = h * 1'000'003 + s.permission_mutations;
    h = h * 1'000'003 + s.channel_mutations;
    if (owner->page == Page::members) {
        h = h * 1'000'003 + g_app.account_ui->guild_member_store.revision;
        h = h * 1'000'003 + g_app.account_ui->user_store.revision;
    }
    if (owner->page == Page::emoji)
        h = h * 1'000'003 + s.emoji_mutations;
    h = h * 1'000'003 + details().revision;
    h = h * 1'000'003 + owner->bans.revision;
    h = h * 1'000'003 + g_app.keyboard_focus_visible;
    return h;
}

} // namespace

void select_page(Page page) noexcept {
    auto& p = *owner;
    settings_layer_close_choice(&p.layer);
    p.page = page;
    p.editing_role = 0;
    p.profile_requested = false;
    clear_status();
    release_images();
    release_picked(&p.role_icon);
    set_editor(editor_search, {});
    if (ops().load)
        ops().load();
    ui_page_registry_select(&p.registry, page_key(page));
    project_page();
    ui_settings_scene_focus(&p.scene, {UiSettingsTargetKind::page, page_key(page), 0});
    ensure_page_data();
    repaint();
}

void request_close() noexcept {
    if (!owner)
        return;
    settings_layer_close_choice(&owner->layer);
    if (guard_dirty()) {
        repaint();
        return;
    }
    application_guild_settings_close();
}

} // namespace maicord::app_detail::server_settings
namespace maicord::app_detail {

bool application_image_file_read(const ConstSlice<wchar_t>& path, u8** data, u32* bytes, Utf8View* error,
                                 Slice<char> error_storage) noexcept {
    *error = utf8_literal("Couldn't read that file.");
    if (!path.data || !path.count || path.count >= kNativeFilePickerMaximumPathUnits)
        return false;
    auto* name = static_cast<wchar_t*>(
        commit_pages((path.count + 1) * sizeof(wchar_t)));
    if (!name)
        return false;
    copy_bytes(name, path.data, path.count * sizeof(wchar_t));
    name[path.count] = 0;
    const HANDLE file = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    pages_release(name);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    Win32LargeInteger size{};
    bool ok = GetFileSizeEx(file, size.native()) && size.QuadPart > 0;
    if (ok && size.QuadPart > kGuildImageSourceMaximumBytes) {
        *error = application_file_over_limit_text(path, static_cast<u64>(size.QuadPart), kGuildImageSourceMaximumBytes,
                                                  error_storage);
        ok = false;
    }
    u8* contents = ok ? static_cast<u8*>(commit_pages(static_cast<SIZE_T>(size.QuadPart)))
                      : nullptr;
    u32 total = 0;
    while (contents && total < size.QuadPart) {
        DWORD read = 0;
        if (!ReadFile(file, contents + total, static_cast<DWORD>(size.QuadPart - total), &read, nullptr) || !read)
            break;
        total += read;
    }
    CloseHandle(file);
    if (!contents || total != size.QuadPart) {
        if (contents)
            pages_release(contents);
        return false;
    }
    *data = contents;
    *bytes = total;
    return true;
}

bool application_guild_image_probe(GuildImageProfile profile, ConstSlice<u8> image, GuildImageInfo* info,
                                   Slice<u8> data_uri) noexcept {
    GuildAssetRequest request{};
    request.operation = GuildAssetOperation::image_probe;
    request.profile = profile;
    request.image = image;
    request.info = info;
    request.data_uri = data_uri;
    reset_value(info);
    return g_app.account.api.guild_assets && g_app.account.instance &&
           g_app.account.api.guild_assets(g_app.account.instance, &request, 0).ok() &&
           info->error == GuildImageError::none;
}

Utf8View application_guild_image_error_text(GuildImageProfile profile, GuildImageError error, ConstSlice<wchar_t> path,
                                            u64 bytes, Slice<char> storage) noexcept {
    const bool sticker = profile == GuildImageProfile::sticker;
    switch (error) {
    case GuildImageError::format:
        return sticker ? utf8_literal("Choose a PNG, APNG, GIF or Lottie JSON file.")
                       : utf8_literal("Choose a PNG, GIF, JPEG or WebP image.");
    case GuildImageError::kind:
        return sticker ? utf8_literal("Stickers must be PNG, APNG, GIF or Lottie JSON.")
               : profile == GuildImageProfile::role_icon
                   ? utf8_literal("Role icons must be a still PNG, JPEG or WebP image.")
                   : utf8_literal("This image type isn't supported here.");
    case GuildImageError::dimensions:
        return sticker ? utf8_literal("Stickers must be 320×320 pixels.")
                       : utf8_literal("Role icons need at least 64×64 pixels.");
    case GuildImageError::too_large: {
        const u64 limit = guild_image_limit(profile);
        // The file's own size, when it is the one over the limit (a resized PNG that still overflows keeps the old text).
        if (bytes > limit)
            return application_file_over_limit_text(path, bytes, limit, storage);
        return profile == GuildImageProfile::emoji || profile == GuildImageProfile::role_icon
                   ? utf8_literal("This image is over 256 KB. Choose a smaller file or a PNG to resize.")
               : sticker ? utf8_literal("Stickers must be 512 KB or smaller.")
                         : utf8_literal("This image is over 8 MB. Choose a smaller file.");
    }
    case GuildImageError::decode: return utf8_literal("This image couldn't be read.");
    default: return utf8_literal("This image couldn't be prepared.");
    }
}

bool application_guild_settings_available(u64 guild_id) noexcept {
    if (!g_app.account_ui)
        return false;
    return guild_settings_access(g_app.account_ui->chat_state, guild_id).sections != 0;
}

bool application_guild_settings_active() noexcept {
    return owner && owner->scene.active;
}

u32 application_guild_settings_page_items(u64 guild_id, u32 action_base, Slice<PopupItem> output) noexcept {
    if (!g_app.account_ui || !action_base || !output.data)
        return 0;
    const u32 sections = guild_settings_access(g_app.account_ui->chat_state, guild_id).sections;
    u32 used = 0;
    for (const auto& definition : kPages)
        if ((sections & definition.section) && used < output.count) {
            reset_value(&output[used]);
            output[used].label = definition.label;
            output[used].action = action_base + static_cast<u32>(definition.page);
            output[used].flags = popup_item_enabled;
            ++used;
        }
    return used;
}

bool application_guild_settings_open(u64 guild_id, u8 requested_page) noexcept {
    if (!application_account_available() || !g_app.renderer_ready || g_app.text_form || application_guild_settings_active() ||
        (owner && owner->request_id) || !application_guild_settings_available(guild_id))
        return false;
    settings_modal_yield();
    if (!owner) {
        owner = settings_modal_allocate<Settings>(kHooks);
        if (!owner)
            return false;
        message_composer_initialize(&owner->models[editor_name], {owner->name_text, kGuildAdminNameBytes});
        message_composer_initialize(&owner->models[editor_hex], {owner->hex_text, 16});
        message_composer_initialize(&owner->models[editor_search], {owner->search_text, kSearchBytes});
        message_composer_initialize(&owner->models[editor_description],
                                    {owner->description_text, kGuildAdminProfileDescriptionBytes});
        message_composer_initialize(&owner->models[editor_vanity], {owner->vanity_text, kGuildAdminVanityCodeBytes});
        guild_ban_store_bind(&owner->bans, {owner->ban_records, kBanCapacity});
    }
    auto& p = *owner;
    const auto* guild = chat_state_store_find_guild(state(), guild_id);
    p.lifetime = ++lifetime_serial;
    p.guild_id = guild_id;
    p.guild_generation = guild->generation;
    p.account_scope = g_app.plugin_account_scope_serial;
    p.profile_id = g_app.account.status.profile_id;
    p.self_user_id = state().self_user_id;
    p.access = guild_settings_access(state(), guild_id);
    p.counts = 0;
    p.counts_requested = p.bans_requested = p.bans_failed = false;
    p.invite_count = p.audit_count = p.audit_user_count = 0;
    p.invites_requested = p.invites_loaded = p.audit_requested = p.audit_complete = false;
    p.invites_failed = p.audit_failed = p.stickers_failed = false;
    p.audit_filter_user = 0;
    p.audit_filter_action = 0;
    p.audit_filter_action_set = false;
    p.editing_role = p.open_after_create = 0;
    reset_value(&p.request);
    clear_status();
    p.menu = Menu::none;
    p.engagement_base = p.engagement_draft = {};
    p.engagement_conflict = p.engagement_echo = p.engagement_guild_sent = false;
    p.widget_requested = p.widget_loaded = p.widget_failed = p.profile_requested = false;
    p.profile_loaded = false;
    p.vanity_requested = p.vanity_loaded = p.vanity_failed = p.vanity_error = false;
    p.vanity_base_bytes = 0;
    p.vanity_uses = 0;
    p.ban_search_count = p.ban_search_bytes = 0;
    reset_value(&p.bulk_notice);
    p.selected_count = 0;
    release_images();
    release_picked(&p.role_icon);
    release_picked(&p.upload);
    p.pick = Pick::none;
    p.sticker_count = 0;
    p.stickers_requested = p.stickers_loaded = false;
    p.safety_echo = false;
    reset_value(&p.form_status);
    next_form = next_form_item = 0;
    guild_ban_store_reset(&p.bans, guild_id);
    settings_layer_reset(&p.layer);
    if (!ui_page_registry_initialize(&p.registry, {p.pages, kPageCount}))
        return false;
    for (u32 i = 0; i < kPageCount; ++i) {
        const auto& definition = kPages[i];
        p.content[i] = {definition.label, {}, {}, 1, kUiSettingsPageFormat, &kUiDefaultSettingsStyle, nullptr};
        if (!ui_page_registry_register(&p.registry, {page_key(definition.page), definition.label, &p.content[i]}).ok())
            return false;
    }
    p.page = requested_page && requested_page <= kPageCount && page_allowed(static_cast<Page>(requested_page))
                 ? static_cast<Page>(requested_page)
                 : first_page();
    if (p.page == Page::none)
        return false;
    settings_modal_take_input();
    ui_settings_scene_set_presentation(&p.scene, UiSettingsPresentation::layer);
    ui_page_registry_select(&p.registry, page_key(p.page));
    set_editor(editor_search, {});
    if (ops().load)
        ops().load();
    // The first update activates the scene; navigation binds before it.
    build_navigation();
    p.scene.focus = {UiSettingsTargetKind::page, page_key(p.page), 0};
    {
        // project() requires an active scene; seed the selected page first.
        auto& content = page_content(p.page);
        reset_value(&content.controls);
        if (!ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips())) {
            application_guild_settings_close(false);
            return false;
        }
    }
    p.signature = signature();
    if (!project_page()) {
        application_guild_settings_close(false);
        return false;
    }
    settings_modal_bind(p);
    application_sync_attachment_drop_target();
    synchronize_member_subscription(clock_monotonic_milliseconds());
    ensure_page_data();
    if (repaint())
        return true;
    application_guild_settings_close(false);
    return false;
}

void application_guild_settings_close(bool restore_focus) noexcept {
    if (!owner || !owner->scene.active)
        return;
    settings_modal_suspend(*owner);
    application_text_form_cancel_owner(form_valid);
    // A queued request is dropped; one in flight finishes and is discarded.
    owner->request.queued = false;
    if (owner->picker.runtime && !owner->picker_consumed) {
        native_file_picker_cancel(&owner->picker);
        owner->pick = Pick::none;
    }
    release_picked(&owner->role_icon);
    // Images and data URIs of a save in flight stay until its reply.
    if (!owner->request_id) {
        release_images();
        release_picked(&owner->upload);
    }
    next_form = 0;
    settings_modal_unbind(*owner);
    owner->page = Page::none;
    owner->editing_role = 0;
    settings_modal_restore_focus(restore_focus && application_account_available());
    synchronize_member_subscription(clock_monotonic_milliseconds());
    settings_modal_repaint_shell();
}

void application_guild_settings_release() noexcept {
    application_guild_settings_close(false);
    if (!owner)
        return;
    // A null request cancels this DETAILS request; its receipt is still drained.
    application_cancel_request(&AccountModuleApi::guild_admin, owner->request_id);
    members_release();
    if (owner->picker.runtime) {
        native_file_picker_cancel(&owner->picker);
        native_file_picker_release(&owner->picker, 5'000);
    }
    release_picked(&owner->upload);
    release_images();
    for (auto& model : owner->models)
        message_composer_release(&model);
    settings_modal_free(owner);
}

bool application_guild_settings_synchronize() noexcept {
    if (!owner || !owner->scene.active)
        return true;
    auto& p = *owner;
    if (!scope_current()) {
        application_text_form_cancel_owner(form_valid);
        application_guild_settings_close(false);
        return true;
    }
    const auto access = guild_settings_access(state(), p.guild_id);
    bool changed = false;
    if (access.sections != p.access.sections || access.top_role_id != p.access.top_role_id ||
        access.permissions != p.access.permissions) {
        p.access = access;
        if (!access.sections) {
            application_guild_settings_close(false);
            return true;
        }
        build_navigation();
        if (!page_allowed(p.page)) {
            select_page(first_page());
            return true;
        }
        changed = true;
    }
    if (settings_layer_choice_lapsed(p.layer)) {
        p.menu = Menu::none;
        changed = true;
    }
    changed = reconcile() || changed;
    const u64 sig = signature();
    if (sig != p.signature) {
        p.signature = sig;
        changed = true;
    }
    if (changed)
        return repaint();
    return settings_modal_refresh(p);
}

SettingsLayer* application_guild_settings_settings_layer() noexcept {
    return owner ? &owner->layer : nullptr;
}

u64 application_guild_settings_next_wake() noexcept {
    if (g_app.details_request_owner != MessageHistoryRequestOwner::none)
        return 0;
    if (creation.queued && !creation.request_id)
        return creation.retry_at ? creation.retry_at : 1;
    if (owner && application_guild_settings_active() && owner->search_due_at &&
        (!owner->request.queued || owner->search_due_at < owner->retry_at))
        return owner->search_due_at;
    if (!owner || !owner->request.queued)
        return 0;
    return owner->retry_at ? owner->retry_at : 1;
}

bool application_guild_settings_create_channel(u64 guild_id, u64 category_id, u8 type) noexcept {
    if (!application_account_available() || g_app.text_form || creation.queued || creation.request_id ||
        (type != 0 && type != 2 && type != 4) || (type == 4 && category_id))
        return false;
    const auto& s = state();
    const auto* category = category_id ? chat_state_store_find_channel(s, category_id) : nullptr;
    if (category_id && !category)
        return false;
    const u64 guild = category ? category->guild_id : guild_id;
    if (!guild_context_can_create_channel(s, category ? 0 : guild, category_id))
        return false;
    reset_value(&creation);
    creation.serial = ++creation_serial;
    creation.guild_id = guild;
    creation.parent_id = category_id;
    creation.type = type;
    creation.account_scope = g_app.plugin_account_scope_serial;
    static char context[kGuildAdminNameBytes + 16];
    Bank bank{context, sizeof(context), 0};
    if (category) {
        bank.add("In ");
        bank.add(chat_state_store_channel_name(s, *category));
    } else if (type != 4)
        bank.add("Outside any category.");
    ApplicationTextFormRequest request{};
    request.title = type == 4   ? utf8_literal("Create category")
                    : type == 2 ? utf8_literal("Create voice channel")
                                : utf8_literal("Create text channel");
    request.context = bank.since(0);
    request.label = type == 4 ? utf8_literal("Category name") : utf8_literal("Channel name");
    request.accept = type == 4 ? utf8_literal("Create category") : utf8_literal("Create channel");
    request.owner = {creation.serial, guild, category_id};
    request.valid = creation_current;
    request.save = creation_save;
    request.maximum_codepoints = kGuildAdminNameCodepoints;
    request.maximum_bytes = kGuildAdminNameBytes;
    request.single_line = true;
    request.progress = creation_progress;
    request.closed = creation_closed;
    if (application_text_form_open(request))
        return true;
    reset_value(&creation);
    return false;
}

} // namespace maicord::app_detail
