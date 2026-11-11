#include "internal.hpp"
#include "maicord/os/text.hpp"
#include "../../media/file_over_limit.hpp"
#include "../../account/application_account_module.hpp"
#include "../../account/account_presentation.hpp"
#include "../../chat/composer/attachments.hpp"
#include "../../chat/composer/text_input.hpp"
#include "maicord/ui/guild/context_policy.hpp"
#include "maicord/model/guild/channel_settings_model.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/os/file.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/model/guild/community_settings.hpp"
#include "maicord/app/account/account_profile_edit.hpp"
#include "../../account/account_access.hpp"
#include "maicord/core/test_hook.hpp"

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
    apps,
    moderation,
    // Standalone entries after a divider and no heading (Enable Community,
    // Server Template), each its own group.
    community,
    templates,
};
// A page's label is its line of kPageLabels.
struct PageDefinition {
    Page page;
    PageGroup group;
    UiIcon icon;
    u32 section;
    const PageOps* ops;
};
constexpr PageDefinition kPages[]{
    {Page::profile, PageGroup::none, UiIcon::guild, guild_settings_profile, &kProfilePage},
    {Page::server_tag, PageGroup::none, UiIcon::tag, guild_settings_server_tag, &kServerTagPage},
    {Page::engagement, PageGroup::none, UiIcon::activity, guild_settings_engagement, &kEngagementPage},
    {Page::boost_perks, PageGroup::none, UiIcon::boost, guild_settings_boost_perks, &kBoostPage},
    {Page::emoji, PageGroup::expression, UiIcon::emoji, guild_settings_expressions, &kExpressionPage},
    {Page::stickers, PageGroup::expression, UiIcon::sticker, guild_settings_expressions, &kExpressionPage},
    {Page::soundboard, PageGroup::expression, UiIcon::audio_lines, guild_settings_expressions, &kExpressionPage},
    {Page::members, PageGroup::people, UiIcon::members, guild_settings_members, &kMembersPage},
    {Page::roles, PageGroup::people, UiIcon::shield, guild_settings_roles, &kRolesPage},
    {Page::invites, PageGroup::people, UiIcon::user_plus, guild_settings_invites, &kInvitesPage},
    {Page::access, PageGroup::people, UiIcon::lock, guild_settings_invites, &kAccessPage},
    {Page::integrations, PageGroup::apps, UiIcon::webhook, guild_settings_integrations, &kIntegrationsPage},
    {Page::safety, PageGroup::moderation, UiIcon::shield_check, guild_settings_safety, &kSafetyPage},
    {Page::audit_log, PageGroup::moderation, UiIcon::audit_log, guild_settings_audit_log, &kAuditLogPage},
    {Page::bans, PageGroup::moderation, UiIcon::gavel, guild_settings_bans, &kBansPage},
    {Page::automod, PageGroup::moderation, UiIcon::shield_alert, guild_settings_automod, &kAutoModPage},
    {Page::community, PageGroup::community, UiIcon::compass, guild_settings_invites, &kCommunityPage},
    {Page::guild_template, PageGroup::templates, UiIcon::file_text, guild_settings_invites, &kTemplatePage},
};
static_assert(sizeof(kPages) / sizeof(kPages[0]) == kPageCount);
// In kPages order. Enable Community's label follows the COMMUNITY feature (community_page_label).
constexpr char kPageLabels[] = "Server Profile\0Server Tag\0Engagement\0Boost Perks\0Emoji\0Stickers\0Soundboard\0"
                               "Members\0Roles\0Invites\0Access\0Integrations\0Safety Setup\0Audit Log\0Bans\0AutoMod\0"
                               "Enable Community\0Server Template\0";
static_assert(name_list_count(kPageLabels) == kPageCount);
constexpr bool pages_in_order() noexcept {
    for (u32 i = 0; i < kPageCount; ++i)
        if (static_cast<u32>(kPages[i].page) != i + 1)
            return false;
    return true;
}
static_assert(pages_in_order());
// The headings of the groups from expression to moderation; the others have none.
constexpr char kGroupHeaders[] = "EXPRESSION\0PEOPLE\0APPS\0MODERATION\0";

const PageDefinition& page_definition(Page page) noexcept {
    const u32 index = static_cast<u32>(page);
    return kPages[index && index <= kPageCount ? index - 1 : 0];
}
Utf8View fixed_label(const PageDefinition& definition) noexcept {
    return name_list_at(kPageLabels, static_cast<u32>(definition.page) - 1);
}
// The sidebar and submenu label of a page in one guild.
Utf8View page_label(const PageDefinition& definition, u64 guild_id) noexcept {
    return definition.page == Page::community ? community_page_label(guild_id) : fixed_label(definition);
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
    return g_app.account.instance && g_app.account.status.running && g_app.account.status.gateway_ready;
}
bool busy() noexcept {
    return owner->request.queued || owner->request_id;
}

namespace guild_settings_detail {

bool read_op(Op op) noexcept {
    return op == Op::role_counts || op == Op::ban_list || op == Op::invite_list || op == Op::audit_page ||
           op == Op::sticker_list || op == Op::widget_get || op == Op::profile_get || op == Op::vanity_get ||
           op == Op::members_search || op == Op::ban_search || (op == Op::apps && apps_reading()) ||
           (op == Op::automod && automod_reading()) || op == Op::top_games ||
           (op == Op::server_access && server_access_reading());
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
    release_picked(&p.custom_banner);
    release_uri(&p.icon_uri, &p.icon_uri_bytes);
    release_uri(&p.banner_uri, &p.banner_uri_bytes);
    release_uri(&p.splash_uri, &p.splash_uri_bytes);
    release_uri(&p.custom_banner_uri, &p.custom_banner_uri_bytes);
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
    case editor_description:
        return {p.description_text, kGuildAdminProfileDescriptionBytes, kGuildAdminProfileDescriptionCodepoints, true};
    case editor_vanity: return {p.vanity_text, kGuildAdminVanityCodeBytes, kGuildAdminVanityCodeCharacters, false};
    case editor_words: return {p.words_text, kWordsBytes, kWordsCodepoints, true};
    case editor_patterns: return {p.patterns_text, kPatternsBytes, kPatternsCodepoints, true};
    case editor_allowed: return {p.allowed_text, kWordsBytes, kWordsCodepoints, true};
    case editor_tag: return {p.tag_text, kGuildAdminTagBytes, kGuildAdminTagCharacters, false};
    case editor_game_search:
        return {p.game_search_text, sizeof(p.game_search_text), kGuildAdminGameQueryCodepoints, false};
    default:
        if (index >= editor_trait && index <= editor_trait_last)
            return {p.trait_text[index - editor_trait], kGuildAdminProfileTraitBytes, kGuildAdminProfileTraitCodepoints,
                    false};
        return {p.search_text, kSearchBytes, 100, false};
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
           : control == control_search      ? editor_search
           : control == control_description ? editor_description
           : control == control_vanity      ? editor_vanity
           : control == control_words       ? editor_words
           : control == control_patterns    ? editor_patterns
           : control == control_allowed     ? editor_allowed
           : control == control_games_search ? editor_game_search
           : control == control_tag_text      ? editor_tag
           : control >= control_trait && control < control_trait + kGuildAdminProfileTraits
               ? static_cast<Editor>(editor_trait + (control - control_trait))
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
// The next `count` views of this projection's choice pool, or null when it is full.
Utf8View* claim_choices(u32 count) noexcept {
    auto& p = *owner;
    if (count > kChoicePool - p.choice_used)
        return nullptr;
    Utf8View* views = p.choice_pool[p.bank_index] + p.choice_used;
    p.choice_used += count;
    return views;
}

} // namespace

// One cold function for every text a row takes.
__declspec(noinline) void store_text(Utf8View* output, Text text) noexcept {
    if (text.bits & Text::kViewBit) {
        *output = *reinterpret_cast<const Utf8View*>(text.bits & ~Text::kViewBit);
        return;
    }
    const char* chars = reinterpret_cast<const char*>(text.bits);
    *output = {chars, c_string_length(chars)};
}
Utf8View text_view(Text text) noexcept {
    Utf8View view;
    store_text(&view, text);
    return view;
}

void popup_row(u32 index, Text label, u32 action, u32 flags) noexcept {
    popup_item_set(&owner->popup_items[index], text_view(label), kMenuChoiceBase + action,
                   static_cast<u16>(popup_item_enabled | flags));
}
u32 push_row(u64 id, Text label, Text description, UiSettingsControlKind kind, bool enabled, bool checked, Text action,
             UiSettingsControlPresentation presentation) noexcept {
    const u32 index = claim_control(presentation);
    if (index == ~0u)
        return index;
    auto& control = owner->controls[index];
    control.id = id;
    store_text(&control.label, label);
    store_text(&control.description, description);
    control.kind = kind;
    control.enabled = enabled;
    control.checked = checked;
    control.choices = {};
    control.selected_choice = 0;
    store_text(&control.action_label, action);
    control.editor = nullptr;
    return index;
}
u32 push_toggle(u64 id, Text label, Text description, bool enabled, bool checked) noexcept {
    return push_row(id, label, description, UiSettingsControlKind::toggle, enabled, checked);
}
u32 push_action(u64 id, Text label, Text description, Text action, bool enabled,
                UiSettingsControlPresentation presentation) noexcept {
    return push_row(id, label, description, UiSettingsControlKind::action, enabled, false, action, presentation);
}
u32 push_choices(u64 id, Text label, Text description, UiSettingsControlKind kind, bool enabled,
                 const ConstSlice<Utf8View>& choices, u32 selected, Text action,
                 UiSettingsControlPresentation presentation) noexcept {
    const u32 index = push_row(id, label, description, kind, enabled, false, action, presentation);
    if (index != ~0u) {
        owner->controls[index].choices = choices;
        owner->controls[index].selected_choice = selected;
    }
    return index;
}
u32 push_names(u64 id, Text label, Text description, UiSettingsControlKind kind, bool enabled, const char* names,
               u32 shown, u32 selected, UiSettingsControlPresentation presentation) noexcept {
    const u32 row = push_row(id, label, description, kind, enabled, false, {}, presentation);
    Utf8View* views = row != ~0u ? claim_choices(shown) : nullptr;
    if (views) {
        NameList list{names};
        for (u32 i = 0; i < shown; ++i) {
            if (i)
                list.next();
            views[i] = list.name();
        }
        const u32 count = shown;
        owner->controls[row].choices = {views, count};
        owner->controls[row].selected_choice = selected;
    }
    return row;
}
u32 push_buttons(u64 id, Text label, Text description, bool enabled, const char* names, u32 shown,
                 UiSettingsControlPresentation presentation) noexcept {
    return push_names(id, label, description, UiSettingsControlKind::buttons, enabled, names, shown, 0, presentation);
}
u32 push_choice(u64 id, Text label, Text description, bool enabled, Text current) noexcept {
    const u32 row = push_row(id, label, description, UiSettingsControlKind::choice, enabled);
    Utf8View* view = row != ~0u ? claim_choices(1) : nullptr;
    if (view) {
        *view = text_view(current);
        owner->controls[row].choices = {view, 1};
    }
    return row;
}
u32 push_editor(u64 id, Text label, Text description, UiSettingsControlKind kind, bool enabled, Editor index,
                Text placeholder, Text action) noexcept {
    const UiSettingsEditor* view = Editors::view(index, id, text_view(placeholder), enabled);
    const u32 row = push_row(id, label, description, kind, enabled, false, action);
    if (row != ~0u)
        owner->controls[row].editor = view;
    return row;
}
u32 push_category(u64 id, Text label, Text description) noexcept {
    return push_row(id, label, description, UiSettingsControlKind::category, true);
}
u32 push_notice(Text description, Text label) noexcept {
    return push_row(control_notice, label, description, UiSettingsControlKind::notice, true);
}
u32 push_error(Text label, Text description, Text action) noexcept {
    return push_row(control_notice, label, description, UiSettingsControlKind::notice, true, false, action,
                    {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
}
u32 push_loading(Text description, u64 id) noexcept {
    return push_row(id, {}, description, UiSettingsControlKind::notice, true, true);
}
u32 push_count_heading(Bank& bank, Text title, u32 count) noexcept {
    const u32 start = bank.used;
    bank.add(text_view(title));
    bank.number(count);
    return push_category(control_list_category, bank.since(start));
}
Utf8View channel_label(Bank& bank, u64 channel_id, Text none) noexcept {
    const auto* channel = channel_id ? chat_state_store_find_channel(state(), channel_id) : nullptr;
    if (!channel)
        return text_view(none);
    const u32 start = bank.used;
    bank.add(channel->type == 2 ? utf8_literal("") : utf8_literal("#"));
    bank.add(chat_state_store_channel_name(state(), *channel));
    return bank.since(start);
}

// "512×512 PNG · 1.0 MB", plus the source size when it was resized.
void image_summary(Bank& bank, const GuildImageInfo& info) noexcept {
    if (info.width) {
        bank.number(info.width);
        bank.add("×");
        bank.number(info.height);
    }
    // The kinds after none, in GuildImageKind order.
    bank.add(name_list_at(" PNG\0 APNG\0 GIF\0 JPEG\0 WebP\0 Lottie\0", static_cast<u32>(info.kind) - 1));
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
Utf8View picked_text(Bank& bank, const Picked& picked, Text current) noexcept {
    if (picked.clear)
        return utf8_literal("Removed when you save.");
    if (!picked.set)
        return text_view(current);
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
        if (const char* invalid = ops().invalid())
            return text_view(invalid);
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
        rgba = choice == kUiSettingsAccentChoice ? p.row_accent[row]
               : choice < 4 && row < kChipRows   ? p.chip_colors[row][choice]
                                                 : 0;
    } else if (p.page == Page::roles && p.editing_role && id == control_header && choice == kUiSettingsAccentChoice)
        rgba = guild_role_color_rgba(p.role_draft.color);
    else if (p.page == Page::roles && p.editing_role && id == control_color && choice <= kGuildRoleColorCount)
        rgba = guild_role_color_rgba(choice ? kGuildRoleColors[choice - 1] : 0);
    else if (p.page == Page::roles && p.editing_role && id == control_hex && choice == 0)
        rgba = guild_role_color_rgba(p.role_draft.color);
    else if (p.page == Page::profile && id == control_banner_colors && choice <= kGuildProfileBannerColorCount) {
        // The first tile stands for the icon's own colour.
        u32 color = 0;
        if (choice)
            color_hex_parse_rgb24({kGuildProfileBannerColors[choice - 1], kGuildProfileBannerColorBytes}, &color);
        else
            color = profile_icon_color();
        rgba = guild_role_color_rgba(color);
    } else if (p.page == Page::server_tag) {
        u32 color = 0;
        if (tag_color(id, choice, &color))
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
    p.control_count = p.choice_used = 0;
    p.glyph_tile_count = 0;
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
        content.glyph_tiles = selected ? p.glyph_tiles : nullptr;
        content.glyph_tile_count = selected ? p.glyph_tile_count : 0;
        content.footer = selected ? &p.footer : nullptr;
        if (selected) {
            if (p.page == Page::roles && p.editing_role) {
                const u32 start = bank.used;
                bank.add("Edit role — ");
                bank.add({p.role_draft.name, p.role_draft.name_bytes});
                content.title = bank.since(start);
            } else
                content.title = page_label(page_definition(p.page), p.guild_id);
            content.description = p.page == Page::roles && !p.editing_role
                                      ? utf8_literal("Use roles to group your server members and assign permissions.")
                                      : Utf8View{};
            // Server Profile lays out as sections beside its live preview.
            if (p.page == Page::profile)
                profile_page_content(&content);
            ++content.revision;
        }
    }
    ui_settings_scene_set_focus_visible(&p.scene, g_app.keyboard_focus_visible);
    ui_settings_scene_bind_palette(&p.scene, {owner, resolve_color, ++p.palette_revision});
    if (!ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips()))
        return false;
    // The preview card sits where the scene placed the aside; a new card height lays the page out again.
    if (profile_place_preview()) {
        ++page_content(p.page).revision;
        if (!ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips()))
            return false;
        profile_place_preview();
    }
    return true;
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
    const auto add = [&](UiSettingsNavEntry entry) noexcept {
        if (p.nav_count < sizeof(p.nav) / sizeof(p.nav[0]))
            p.nav[p.nav_count++] = entry;
    };
    add({UiSettingsNavKind::header, false, UiIcon::count,
         header_bytes ? Utf8View{header, header_bytes} : utf8_literal("SERVER SETTINGS"), {}, 0});
    // Every section after the first starts with a divider; the headed ones
    // follow it with their heading.
    PageGroup group = PageGroup::none;
    const auto enter = [&](PageGroup next) noexcept {
        group = next;
        add({UiSettingsNavKind::separator, false, UiIcon::count, {}, {}, 0});
        if (next >= PageGroup::expression && next <= PageGroup::moderation)
            add({UiSettingsNavKind::header, false, UiIcon::count,
                 name_list_at(kGroupHeaders, static_cast<u32>(next) - static_cast<u32>(PageGroup::expression)), {}, 0});
    };
    // App Directory is a link (no page) that shows with the settings, under
    // Integrations when that shows.
    bool app_directory = false;
    const auto add_app_directory = [&]() noexcept {
        app_directory = true;
        if (group != PageGroup::apps)
            enter(PageGroup::apps);
        add({UiSettingsNavKind::action, false, UiIcon::external_link, utf8_literal("App Directory"), {},
             kAppDirectoryAction});
    };
    // Community servers reach AutoMod from Safety Setup, which stays selected while it shows (the official sidebar).
    const bool community = (guild_features() & guild_feature_community) != 0 && page_allowed(Page::safety);
    for (const auto& definition : kPages) {
        if (!page_allowed(definition.page) || (community && definition.page == Page::automod))
            continue;
        if (!app_directory && definition.group > PageGroup::apps)
            add_app_directory();
        if (definition.group != group)
            enter(definition.group);
        UiSettingsNavEntry entry{UiSettingsNavKind::page, false, definition.icon, page_label(definition, p.guild_id),
                                 page_key(definition.page), 0};
        entry.badge = server_template_nav_badge(definition.page);
        if (community && definition.page == Page::safety)
            entry.subpage = page_key(Page::automod);
        add(entry);
    }
    if (!app_directory)
        add_app_directory();
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
    const int units = utf16_to_utf8(filename.data, static_cast<int>(filename.count), p.upload_filename, sizeof(p.upload_filename), false);
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
    if (busy() || !connected())
        return;
    if (ops().ensure_data && ops().ensure_data())
        return;
    // Nothing for the page to read: the template's sync state, for the sidebar's dot.
    server_template_ensure_status();
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
    // Webhook avatars are prepared like a server icon.
    case Pick::webhook_avatar: return GuildImageProfile::icon;
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
    // In Pick order after none.
    static constexpr char kTitles[] = "Choose a server icon\0Choose a server banner\0Choose a role icon\0"
                                      "Choose an emoji image\0Choose a sticker file\0Choose a sound file\0"
                                      "Choose an invite background\0Choose a webhook avatar\0"
                                      "Choose a Discovery banner\0Choose a server banner\0";
    p.pick = pick;
    p.pick_role = pick == Pick::role_icon ? p.editing_role : 0;
    p.picker_consumed = false;
    if (native_file_picker_start(&p.picker, g_app.window, 1, name_list_at(kTitles, static_cast<u32>(pick) - 1)) !=
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
    if (g_app.account.instance && account_module_profile_image(g_app.account.instance, &request) &&
        info->error == ProfileImageError::none)
        return true;
    *error = info->error == ProfileImageError::format ? utf8_literal("Choose a PNG, JPEG, WebP or GIF image.")
                                                      : utf8_literal("This image couldn't be read.");
    return false;
}
void accept_pick(Pick pick, ConstSlice<wchar_t> path, ConstSlice<wchar_t> filename) noexcept {
    auto& p = *owner;
    Picked* slot = pick == Pick::icon       ? &p.icon
                   : pick == Pick::banner   ? &p.banner
                   : pick == Pick::splash || pick == Pick::discovery_splash ? &p.splash
                   : pick == Pick::custom_banner ? &p.custom_banner
                   : pick == Pick::role_icon ? &p.role_icon
                                             : &p.upload;
    if (pick == Pick::role_icon && p.pick_role != p.editing_role)
        return;
    Picked picked{};
    ProfileImageInfo info{};
    Utf8View error{};
    bool ok = read_picked_file(path, &picked, &error);
    if (ok && (pick == Pick::icon || pick == Pick::splash || pick == Pick::discovery_splash ||
               pick == Pick::custom_banner))
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
    case Pick::discovery_splash:
    case Pick::custom_banner:
        slot->info.source_kind = info.kind;
        slot->info.kind = info.kind;
        slot->info.width = info.width;
        slot->info.height = info.height;
        slot->info.bytes = slot->bytes;
        if (pick == Pick::custom_banner) {
            release_uri(&p.custom_banner_uri, &p.custom_banner_uri_bytes);
            guild_server_profile_set_custom_banner(&p.profile_draft);
            break;
        }
        release_uri(&p.splash_uri, &p.splash_uri_bytes);
        if (pick == Pick::splash)
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

void open_popup(Menu menu, u64 subject, u32 count, const UiRect& anchor, u32 width) noexcept {
    auto& p = *owner;
    p.menu = menu;
    settings_layer_open_choice(&p.layer, subject, {p.popup_items, count}, anchor, static_cast<float>(width));
}

void open_channel_menu(Menu menu, u32 types, u64 current, Text none, PermissionSet required, const UiRect& anchor,
                       bool create_row) noexcept {
    auto& p = *owner;
    Bank bank{p.popup_text, sizeof(p.popup_text), 0};
    u32 count = 0;
    if (none.bits) {
        popup_row(count, none, count, !current ? popup_item_checked : 0);
        p.menu_channel[count++] = 0;
    }
    if (create_row) {
        popup_row(count, "Create one for me", count, current == kGuildCommunityNewChannel ? popup_item_checked : 0);
        p.menu_channel[count++] = kGuildCommunityNewChannel;
    }
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
                                                  (first && i == first ? popup_item_separator_before : 0));
        p.menu_channel[i] = rows[i - first].channel ? rows[i - first].channel->id : 0;
    }
    open_popup(menu, p.guild_id, count, anchor, 280);
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
        if (target.control_id == kAppDirectoryAction)
            open_app_directory();
        else if (target.control_id == kDeleteServerAction && guild_settings_can_delete(p.access) && editable() &&
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
    } else if (editor == editor_search) {
        p.scene.content_offset = 0;
    } else if (p.page == Page::profile && (editor == editor_game_search ||
                                           (editor >= editor_trait && editor <= editor_trait_last))) {
        profile_edited(editor, text);
    } else if (p.page == Page::server_tag && editor == editor_tag) {
        tag_edited(text);
    }
    return true;
}
SettingsLayerEditor editor_hook(SettingsLayer*, u64 control) noexcept {
    const Editor editor = editor_of(control);
    return editor == editor_count
               ? SettingsLayerEditor{}
               : SettingsLayerEditor{&owner->models[editor], &owner->bindings[editor], editor_storage(editor).multiline};
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
    const HANDLE file = file_open(name, {FileAccess::read, FileCreate::existing, kFileShareRead, kFileSequential});
    pages_release(name);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    u64 size = 0;
    bool ok = file_size(file, &size) && size > 0;
    if (ok && size > kGuildImageSourceMaximumBytes) {
        *error = application_file_over_limit_text(path, size, kGuildImageSourceMaximumBytes,
                                                  error_storage);
        ok = false;
    }
    u8* contents = ok ? static_cast<u8*>(commit_pages(static_cast<SIZE_T>(size)))
                      : nullptr;
    u32 total = 0;
    while (contents && total < size) {
        u32 read = 0;
        if (!file_read(file, contents + total, static_cast<u32>(size - total), &read) || !read)
            break;
        total += read;
    }
    file_close(file);
    if (!contents || total != size) {
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
    return g_app.account.instance && account_module_guild_assets(g_app.account.instance, &request, 0).ok() &&
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
        if ((sections & definition.section) && used < output.count)
            popup_item_set(&output[used++], page_label(definition, guild_id),
                           action_base + static_cast<u32>(definition.page),
                           popup_item_enabled);
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
        for (u32 i = 0; i < editor_count; ++i) {
            const auto storage = editor_storage(static_cast<Editor>(i));
            message_composer_initialize(&owner->models[i], {storage.text, storage.capacity});
        }
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
    p.top_games_requested = p.top_games_failed = p.has_icon_color = false;
    p.top_game_count = 0;
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
    u32 i = 0;
    for (NameList label{kPageLabels}; label; label.next(), ++i) {
        p.content[i] = {label.name(), {}, {}, 1, kUiSettingsPageFormat, &kUiDefaultSettingsStyle, nullptr};
        if (!ui_page_registry_register(&p.registry, {page_key(kPages[i].page), label.name(), &p.content[i]}).ok())
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

// Fake-server UI probes (tools/test_server_settings_wayland.sh): MAICORD_TEST_GUILD_SETTINGS names a page by its
// sidebar label, and Server Settings opens there once, on the first server, as soon as the account is ready.
void application_guild_settings_test_open() noexcept {
#if defined(MAICORD_TEST_HOOKS) && !defined(_WIN32)
    static bool opened;
    const char* label = test_hook("MAICORD_TEST_GUILD_SETTINGS");
    if (opened || !label || !g_app.account_ui || !connected() || !state().guild_count)
        return;
    for (const auto& definition : kPages)
        if (utf8_equal(fixed_label(definition), {label, c_string_length(label)}))
            opened = application_guild_settings_open(state().guilds[0].id, static_cast<u8>(definition.page));
#endif
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
    profile_close_preview();
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
    application_cancel_request(account_module_guild_admin, owner->request_id);
    members_release();
    apps_release();
    automod_release();
    server_access_release();
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
        // The Community entry's label follows the guild's features.
        for (u32 i = 0; i < p.nav_count; ++i)
            if (p.nav[i].kind == UiSettingsNavKind::page && ui_page_key_equal(p.nav[i].page, page_key(Page::community)) &&
                p.nav[i].label.data != community_page_label(p.guild_id).data) {
                build_navigation();
                break;
            }
        changed = true;
    }
    // The Server Template entry's dot follows the template's sync state.
    for (u32 i = 0; i < p.nav_count; ++i)
        if (p.nav[i].kind == UiSettingsNavKind::page &&
            ui_page_key_equal(p.nav[i].page, page_key(Page::guild_template)) &&
            p.nav[i].badge != server_template_nav_badge(Page::guild_template)) {
            build_navigation();
            changed = true;
            break;
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
