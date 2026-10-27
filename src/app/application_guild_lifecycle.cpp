#include "application_guild_lifecycle.hpp"
#include "application_toast.hpp"
#include "application_account_presentation.hpp"
#include "application_attachments.hpp"
#include "application_channel_overview.hpp"
#include "application_composer_text_input.hpp"
#include "application_content_input.hpp"
#include "application_guild_folders.hpp"
#include "application_guild_invite.hpp"
#include "application_guild_settings.hpp"
#include "application_media_picker.hpp"
#include "application_message_history.hpp"
#include "application_message_interaction.hpp"
#include "application_profile.hpp"
#include "application_settings.hpp"
#include "application_settings_layer.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "application_user_settings.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/guild_settings_model.hpp"
#include "maicord/platform/file_picker.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/guild_assets.hpp"
#include "maicord/protocol/guild_folders.hpp"
#include "maicord/protocol/guild_invite.hpp"
#include "maicord/render/layer_presentation.hpp"
#include "maicord/model/bounded_edit.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/ui/guild_context_policy.hpp"
#include "maicord/ui/message_markdown.hpp"
#include "application_account_access.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {
constexpr u64 kPageOwner = 0x4D4149474C494645ull;
constexpr UiPageKey kPage{kPageOwner, 1};
constexpr u32 kControlCapacity = 12;
constexpr u32 kMenuChoiceBase = 2'000;
constexpr u64 kCopiedMs = 2'000;
constexpr u32 kServerNameCodepoints = kGuildCreateNameMaximum;
constexpr u32 kFolderNameCodepoints = 32;
constexpr u32 kNameBytes = kServerNameCodepoints * 4;
static_assert(kFolderNameCodepoints * 4 <= kGuildFolderNameBytes);

enum class Mode : u8 { none, invite, create, folder, privacy };
enum class Op : u8 { none, invite, create, leave, reorder };
enum class Menu : u8 { none, expiry, uses };
enum : u64 {
    control_link = 1,
    control_expiry,
    control_uses,
    control_temporary,
    control_icon,
    control_icon_remove,
    control_name,
    control_join_category,
    control_join,
    control_color,
    // Privacy settings, in the official client's order.
    control_privacy_dms,
    control_privacy_requests,
    control_privacy_activity,
    control_privacy_joining,
};

static constexpr Utf8View kExpiryLabels[kGuildInviteExpiryCount]{
    utf8_literal("30 minutes"), utf8_literal("1 hour"), utf8_literal("6 hours"), utf8_literal("12 hours"),
    utf8_literal("1 day"),      utf8_literal("7 days"), utf8_literal("Never")};
constexpr Utf8View kUsesLabels[kGuildInviteUsesCount]{
    utf8_literal("No limit"), utf8_literal("1 use"),   utf8_literal("5 uses"),  utf8_literal("10 uses"),
    utf8_literal("25 uses"),  utf8_literal("50 uses"), utf8_literal("100 uses")};

// A picked server icon held until creation: the file bytes (prepared again by
// the account module at submit) and what preparation reported.
struct Picked {
    u8* data;
    u32 bytes;
    GuildImageInfo info;
};

struct Owner {
    UiSettingsScene scene;
    SettingsLayer layer;
    UiPageRegistry registry;
    UiPage page;
    UiSettingsPageContent content;
    UiSettingsControl controls[kControlCapacity];
    UiSettingsControlPresentation presentation[kControlCapacity];
    // Server tiles: an avatar-presented row whose entry is
    // application_guild_icon_media(guild) draws the rail's icon over initials.
    UiSettingsAvatarMedia avatar_media[kControlCapacity];
    u32 control_count;
    UiSettingsEditor editor;
    UiSettingsFooter footer;
    MessageComposer model;
    BoundedEdit binding;
    char name[kNameBytes];
    PopupItem popup_items[kGuildInviteExpiryCount];
    Utf8View color_labels[1 + kGuildRoleColorCount];
    u64 palette_revision;
    char title[192];
    Mode mode;
    Menu menu;
    u64 lifetime, account_scope, profile_id;
    u64 guild_id, channel_id, folder_id;
    u16 guild_generation;
    // Invite People.
    u8 expiry, uses;
    bool temporary;
    GuildAdminInvite invite;
    bool invite_valid;
    u8 link_expiry, link_uses;
    bool link_temporary;
    u64 copied_until;
    // Create a server.
    Picked icon;
    NativeFilePicker picker;
    bool picker_consumed;
    u64 created_guild_id, created_at;
    // Folder settings.
    u32 color_index;
    // Privacy settings: the synced settings revision last projected.
    u64 settings_revision;
    // Modal request.
    Op op;
    bool queued, failed;
    u64 request_id, retry_at;
    Utf8View status;
    char banks[2][1024];
    u32 bank_index;
    u32 structurals[8192];
};
Owner* owner;
u64 lifetime_serial;

// Work that outlives the modal: leave, channel moves and a deferred join.
struct Background {
    u64 account_scope;
    u64 leave_guild, leave_request, leave_retry;
    u64 leave_revision;
    char leave_title[192], leave_context[512];
    u16 leave_title_bytes, leave_context_bytes;
    Utf8View leave_status;
    bool leave_queued, leave_owner, leave_dialog, leave_done;
    u64 reorder_guild, reorder_request, reorder_retry;
    GuildAdminPosition positions[kGuildAdminMaximumPositions];
    u32 position_count;
    bool reorder_queued;
    char join_code[kGuildInviteCodeMaximumBytes];
    u8 join_code_bytes;
    bool join_pending, join_dialog;
    u64 join_revision;
    Utf8View join_status;
};
Background g_background{};

bool connected() noexcept {
    return g_app.account.instance && g_app.account.status.gateway_ready;
}
ChatStateStore& state() noexcept {
    return g_app.account_ui->chat_state;
}
bool background_scope() noexcept {
    if (!g_app.account_ui)
        return false;
    if (g_background.account_scope != g_app.plugin_account_scope_serial) {
        reset_value(&g_background);
        g_background.account_scope = g_app.plugin_account_scope_serial;
    }
    return true;
}
bool lane_free() noexcept {
    return g_app.details_request_owner == MessageHistoryRequestOwner::none;
}
void claim_lane(u64 id) noexcept {
    g_app.details_request_owner = MessageHistoryRequestOwner::channel_overview;
    g_app.details_request_id = id;
}
bool scope_current() noexcept {
    if (!owner || !application_account_available() || owner->account_scope != g_app.plugin_account_scope_serial ||
        owner->profile_id != g_app.account.status.profile_id)
        return false;
    if (owner->mode == Mode::invite || owner->mode == Mode::privacy) {
        const auto* guild = chat_state_store_find_guild(state(), owner->guild_id);
        return guild && guild->generation == owner->guild_generation && !guild->unavailable;
    }
    if (owner->mode == Mode::folder) {
        ApplicationGuildFolder folder{};
        return application_guild_folders_folder(owner->folder_id, &folder);
    }
    return true;
}
bool busy() noexcept {
    return owner && (owner->queued || owner->request_id);
}
bool editable() noexcept {
    return owner && owner->scene.active && !busy() && !owner->created_guild_id;
}

using Bank = TextBank;

bool binding_ready(void* model, u64 identity) noexcept {
    MessageComposer* focused = nullptr;
    return owner && owner->lifetime == identity && editable() &&
           settings_layer_editor(&owner->layer, nullptr, &focused, nullptr, nullptr) && focused == model;
}
void load_editor(Utf8View text, u32 limit) noexcept {
    application_composer_text_input_suspend();
    const u32 bytes = minimum<u32>(text.bytes, kNameBytes);
    copy_bytes(owner->name, text.data, bytes);
    message_composer_refresh_borrowed(&owner->model, bytes);
    message_composer_set_selection(&owner->model, bytes, bytes);
    bounded_edit_bind(&owner->binding, &owner->model, limit, false, owner->lifetime, binding_ready, &owner->model);
}
Utf8View editor_text() noexcept {
    return message_composer_text(owner->model);
}

Utf8View invite_link(Bank& bank) noexcept {
    const u32 start = bank.used;
    bank.add("https://discord.gg/");
    bank.add({owner->invite.code, owner->invite.code_bytes});
    return bank.since(start);
}
bool options_changed() noexcept {
    return !owner->invite_valid || owner->expiry != owner->link_expiry || owner->uses != owner->link_uses ||
           owner->temporary != owner->link_temporary;
}
Utf8View expiry_sentence(Bank& bank) noexcept {
    const u32 start = bank.used;
    const u32 seconds = kGuildInviteExpirySeconds[owner->link_expiry];
    if (!seconds)
        bank.add("Your invite link never expires.");
    else {
        bank.add("Your invite link expires in ");
        bank.add(kExpiryLabels[owner->link_expiry]);
        bank.add(".");
    }
    const u32 uses = kGuildInviteUses[owner->link_uses];
    if (uses == 1)
        bank.add(" It works once.");
    else if (uses) {
        bank.add(" It works ");
        bank.number(uses);
        bank.add(" times.");
    }
    return bank.since(start);
}
void queue(Op op) noexcept {
    owner->op = op;
    owner->queued = true;
    owner->failed = false;
    reset_value(&owner->status);
    owner->retry_at = 0;
    pump_message_history();
}

void release_icon() noexcept {
    if (owner->icon.data) {
        secure_clear_bytes(owner->icon.data, owner->icon.bytes);
        VirtualFree(owner->icon.data, 0, MEM_RELEASE);
    }
    reset_value(&owner->icon);
}
Utf8View image_error(GuildImageError error) noexcept {
    switch (error) {
    case GuildImageError::format:
    case GuildImageError::kind: return utf8_literal("Choose a PNG, GIF, JPEG or WebP image.");
    case GuildImageError::too_large: return utf8_literal("This image is over 8 MB. Choose a smaller file.");
    case GuildImageError::decode: return utf8_literal("This image couldn't be read.");
    default: return utf8_literal("This image couldn't be prepared.");
    }
}
bool read_icon(ConstSlice<wchar_t> path, Utf8View* error) noexcept {
    u8* data = nullptr;
    u32 total = 0;
    if (!application_image_file_read(path, &data, &total, error))
        return false;
    // The module prepares (crops/resizes) the image the same way at submit.
    Picked picked{data, total, {}};
    GuildAssetRequest probe{};
    probe.operation = GuildAssetOperation::image_probe;
    probe.profile = GuildImageProfile::icon;
    probe.image = {data, total};
    probe.info = &picked.info;
    const auto probed = g_app.account.api.guild_assets && g_app.account.instance
                            ? g_app.account.api.guild_assets(g_app.account.instance, &probe, 0)
                            : RestClientSubmitResult{RestClientSubmitError::invalid_input};
    if (!probed.ok() || picked.info.error != GuildImageError::none) {
        *error = image_error(picked.info.error);
        secure_clear_bytes(data, total);
        VirtualFree(data, 0, MEM_RELEASE);
        return false;
    }
    release_icon();
    owner->icon = picked;
    return true;
}
void poll_picker() noexcept {
    if (!owner || !owner->picker.runtime)
        return;
    const auto status = native_file_picker_status(&owner->picker);
    if (status.phase != NativeFilePickerPhase::complete)
        return;
    if (!owner->picker_consumed) {
        owner->picker_consumed = true;
        Utf8View error{};
        if (status.ok() && status.count == 1 && owner->scene.active && owner->mode == Mode::create) {
            if (read_icon(native_file_picker_path(&owner->picker, 0), &error)) {
                owner->failed = false;
                reset_value(&owner->status);
            } else {
                owner->failed = true;
                owner->status = error;
            }
        } else if (!status.ok() && status.error != NativeFilePickerError::cancelled && owner->scene.active) {
            owner->failed = true;
            owner->status = utf8_literal("Couldn't open that file.");
        }
    }
    native_file_picker_release(&owner->picker, 0);
}
void default_server_name(Bank& bank) noexcept {
    const auto& ui = *g_app.account_ui;
    const auto* self = user_store_find(ui.user_store, ui.chat_state.self_user_id);
    const Utf8View name = self ? user_store_name(ui.user_store, *self) : Utf8View{};
    // The official client suggests "<name>'s server" within the 100 code point limit.
    u32 bytes = 0, codepoints = 0;
    while (bytes < name.bytes && codepoints < kServerNameCodepoints - 9) {
        const auto scalar = utf8_decode_one(name.data + bytes, name.bytes - bytes);
        if (!scalar.valid)
            break;
        bytes += scalar.width;
        ++codepoints;
    }
    if (bytes) {
        bank.add({name.data, bytes});
        bank.add("\xE2\x80\x99s server");
    } else
        bank.add("My server");
}

void push(const UiSettingsControl& control,
          UiSettingsControlPresentation presentation = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault}) noexcept {
    if (owner->control_count >= kControlCapacity)
        return;
    owner->presentation[owner->control_count] = presentation;
    reset_value(&owner->avatar_media[owner->control_count]);
    owner->controls[owner->control_count++] = control;
}
bool resolve_color(void*, u64 id, u32 choice, BackgroundTheme* theme) noexcept {
    if (!owner || !theme || id != control_color || choice > kGuildRoleColorCount)
        return false;
    const u32 color = choice ? kGuildRoleColors[choice - 1] : kGuildFolderDefaultColor;
    reset_value(theme);
    theme->colors[0] = color;
    theme->color_count = 1;
    return true;
}
Utf8View footer_status(Bank& bank) noexcept {
    if (owner->failed && owner->status.bytes)
        return owner->status;
    if (!connected())
        return utf8_literal("Available when MaiCord is connected.");
    switch (owner->mode) {
    case Mode::invite:
        if (owner->request_id || owner->queued)
            return utf8_literal("Generating an invite link\xE2\x80\xA6");
        return options_changed() ? utf8_literal("Generate a new link to use these settings.") : expiry_sentence(bank);
    case Mode::create:
        if (owner->created_guild_id)
            return utf8_literal("Server created. Opening it\xE2\x80\xA6");
        if (owner->request_id || owner->queued)
            return utf8_literal("Creating your server\xE2\x80\xA6");
        return utf8_literal("By creating a server, you agree to Discord\xE2\x80\x99s Community Guidelines.");
    case Mode::folder:
        return owner->status.bytes ? owner->status : utf8_literal("Only you can see folders.");
    default:
        return {};
    }
}
void project_invite(Bank& bank) noexcept {
    auto& p = *owner;
    const bool edit = editable();
    const Utf8View link = p.invite_valid ? invite_link(bank) : utf8_literal("Generating a link\xE2\x80\xA6");
    const bool copied = p.copied_until && clock_monotonic_milliseconds() < p.copied_until;
    push({control_link, link, p.invite_valid ? expiry_sentence(bank) : Utf8View{}, UiSettingsControlKind::action,
          p.invite_valid, false, {}, 0, copied ? utf8_literal("Copied") : utf8_literal("Copy"), nullptr},
         {copied ? UiSettingsVariant::positive : UiSettingsVariant::primary, 0, kUiSettingsNoDefault});
    push({control_expiry, utf8_literal("Expire after"), {}, UiSettingsControlKind::choice, edit, false,
          {kExpiryLabels, kGuildInviteExpiryCount}, p.expiry, {}, nullptr});
    push({control_uses, utf8_literal("Max number of uses"), {}, UiSettingsControlKind::choice, edit, false,
          {kUsesLabels, kGuildInviteUsesCount}, p.uses, {}, nullptr});
    push({control_temporary, utf8_literal("Grant temporary membership"),
          utf8_literal("Temporary members leave when they go offline unless they get a role."),
          UiSettingsControlKind::toggle, edit, p.temporary, {}, 0, {}, nullptr});
    p.footer = {footer_status(bank), utf8_literal("Generate a new link"), utf8_literal("Close"), true,
                connected() && !busy() && options_changed(), busy(), p.failed};
    const auto* channel = chat_state_store_find_channel(state(), p.channel_id);
    const auto* guild = chat_state_store_find_guild(state(), p.guild_id);
    const u32 start = bank.used;
    if (channel && guild) {
        bank.add("Share this link to invite people to ");
        bank.add(chat_state_store_guild_name(state(), *guild));
        bank.add(". They join in #");
        bank.add(chat_state_store_channel_name(state(), *channel));
        bank.add(".");
    }
    p.content.description = bank.since(start);
}
void project_create(Bank& bank) noexcept {
    auto& p = *owner;
    const bool edit = editable();
    const u32 start = bank.used;
    if (p.icon.data) {
        bank.number(p.icon.info.width);
        bank.add("\xC3\x97");
        bank.number(p.icon.info.height);
        bank.add(p.icon.info.resized ? utf8_literal(" image, resized for upload.") : utf8_literal(" image."));
    }
    const Utf8View icon_text = p.icon.data ? bank.since(start)
                                           : utf8_literal("Optional. PNG, GIF, JPEG or WebP; square works best.");
    push({control_icon, utf8_literal("Server icon"), icon_text, UiSettingsControlKind::action,
          edit && connected() && !p.picker.runtime, false, {}, 0,
          p.icon.data ? utf8_literal("Change") : utf8_literal("Upload"), nullptr});
    if (p.icon.data)
        push({control_icon_remove, utf8_literal("Remove icon"), {}, UiSettingsControlKind::action, edit, false, {}, 0,
              utf8_literal("Remove"), nullptr},
             {UiSettingsVariant::standard, ui_settings_presentation_compact, kUiSettingsNoDefault});
    p.editor = settings_layer_editor_view(&p.layer, control_name, p.model, utf8_literal("Server name"), edit);
    push({control_name, utf8_literal("Server name"), {}, UiSettingsControlKind::edit, edit, false, {}, 0, {},
          &p.editor});
    push({control_join_category, utf8_literal("Have an invite already?"), {}, UiSettingsControlKind::category, true,
          false, {}, 0, {}, nullptr});
    push({control_join, utf8_literal("Join a server"), utf8_literal("Enter an invite link to join a server you were invited to."),
          UiSettingsControlKind::action, edit, false, {}, 0, utf8_literal("Join a server"), nullptr});
    const auto name = editor_text();
    const auto valid = utf8_validate(name);
    bool visible = false;
    for (u32 i = 0; i < name.bytes; ++i)
        visible = visible || name.data[i] != ' ';
    const bool ready = valid.valid && valid.codepoints >= kGuildCreateNameMinimum && visible;
    p.footer = {footer_status(bank), utf8_literal("Create"), utf8_literal("Cancel"), true,
                ready && connected() && !busy() && !p.created_guild_id && g_app.account.api.guild_assets,
                busy() || p.created_guild_id, p.failed};
    p.content.description =
        utf8_literal("Give your new server a personality with a name and an icon. You can always change it later.");
}
void project_folder(Bank& bank) noexcept {
    auto& p = *owner;
    ApplicationGuildFolder folder{};
    const bool known = application_guild_folders_folder(p.folder_id, &folder);
    p.editor = settings_layer_editor_view(&p.layer, control_name, p.model,
                                          known ? bank.add({folder.placeholder, folder.placeholder_bytes})
                                                : utf8_literal("Folder name"),
                                          known);
    push({control_name, utf8_literal("Folder name"), {}, UiSettingsControlKind::edit, known, false, {}, 0, {},
          &p.editor});
    p.color_labels[0] = utf8_literal("Default");
    for (u32 i = 0; i < kGuildRoleColorCount; ++i)
        p.color_labels[i + 1] = utf8_literal("Colour");
    push({control_color, utf8_literal("Folder colour"), {}, UiSettingsControlKind::swatches, known, false,
          {p.color_labels, kGuildRoleColorCount + 1}, p.color_index, {}, nullptr});
    p.footer = {footer_status(bank), utf8_literal("Done"), utf8_literal("Cancel"), true, known && connected(), false,
                p.failed};
    reset_value(&p.content.description);
}
// Privacy settings: the official client's per-server modal. Each switch edits one
// privacy.*_guild_ids list of the synced settings at once (batched into the
// settings-proto PATCH); USER_SETTINGS_PROTO_UPDATE repaints it live.
GuildPrivacyList privacy_list(u64 control) noexcept {
    return control == control_privacy_dms        ? GuildPrivacyList::direct_messages
           : control == control_privacy_requests ? GuildPrivacyList::message_requests
           : control == control_privacy_activity ? GuildPrivacyList::activity_status
                                                 : GuildPrivacyList::activity_joining;
}
bool privacy_allowed(u64 control) noexcept {
    return !application_user_settings_guild_restricted(privacy_list(control), owner->guild_id);
}
void project_privacy(Bank& bank) noexcept {
    auto& p = *owner;
    p.settings_revision = application_user_settings_revision();
    const bool ready = application_user_settings_ready() && connected();
    const bool dms = privacy_allowed(control_privacy_dms);
    push({control_privacy_dms, utf8_literal("Direct messages"),
          utf8_literal("Allow direct messages from other members in this server."), UiSettingsControlKind::toggle,
          ready, dms, {}, 0, {}, nullptr});
    push({control_privacy_requests, utf8_literal("Message requests"),
          utf8_literal("If direct messages are enabled, filter messages from server members you may not know."),
          UiSettingsControlKind::toggle, ready && dms, dms && privacy_allowed(control_privacy_requests), {}, 0, {},
          nullptr});
    push({control_privacy_activity, utf8_literal("Activity status"),
          utf8_literal("Share your activity status in this server."), UiSettingsControlKind::toggle, ready,
          privacy_allowed(control_privacy_activity), {}, 0, {}, nullptr});
    push({control_privacy_joining, utf8_literal("Activity joining"),
          utf8_literal("Allow users to join your activity in this server."), UiSettingsControlKind::toggle, ready,
          privacy_allowed(control_privacy_joining), {}, 0, {}, nullptr});
    const Utf8View failure = application_user_settings_failure();
    const Utf8View status = p.failed && p.status.bytes ? p.status
                            : failure.bytes            ? failure
                            : !connected()             ? utf8_literal("Available when MaiCord is connected.")
                            : !ready                   ? utf8_literal("Loading your Discord settings\xE2\x80\xA6")
                                                       : Utf8View{};
    p.footer = {status, utf8_literal("Done"), utf8_literal("Global settings"), true, true, false,
                p.failed || failure.bytes != 0};
    const auto* guild = chat_state_store_find_guild(state(), p.guild_id);
    p.content.description = guild ? bank.add(chat_state_store_guild_name(state(), *guild)) : Utf8View{};
}
bool project() noexcept {
    if (!owner || !owner->scene.active)
        return true;
    auto& p = *owner;
    p.bank_index ^= 1;
    Bank bank{p.banks[p.bank_index], sizeof(p.banks[0]), 0};
    p.control_count = 0;
    if (p.mode == Mode::invite)
        project_invite(bank);
    else if (p.mode == Mode::create)
        project_create(bank);
    else if (p.mode == Mode::privacy)
        project_privacy(bank);
    else
        project_folder(bank);
    p.content.controls = {p.controls, p.control_count};
    p.content.presentation = p.presentation;
    p.content.avatar_media = p.avatar_media;
    p.content.footer = &p.footer;
    ++p.content.revision;
    ui_settings_scene_set_focus_visible(&p.scene, g_app.keyboard_focus_visible);
    return ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips());
}
bool repaint() noexcept {
    if (!project())
        return false;
    application_request_frame();
    return !g_app.renderer_ready || renderer_rebuild_content_text(&g_app.renderer, application_theme());
}

void open_menu(Menu menu, UiRect anchor) noexcept {
    auto& p = *owner;
    const bool expiry = menu == Menu::expiry;
    const u32 count = expiry ? kGuildInviteExpiryCount : kGuildInviteUsesCount;
    const u32 selected = expiry ? p.expiry : p.uses;
    for (u32 i = 0; i < count; ++i) {
        reset_value(&p.popup_items[i]);
        p.popup_items[i].label = expiry ? kExpiryLabels[i] : kUsesLabels[i];
        p.popup_items[i].action = kMenuChoiceBase + i;
        p.popup_items[i].flags = static_cast<u16>(popup_item_enabled | (i == selected ? popup_item_checked : 0));
    }
    p.menu = menu;
    settings_layer_open_choice(&p.layer, p.guild_id, {p.popup_items, count}, anchor, 200);
}
void copy_link() noexcept {
    auto& p = *owner;
    if (!p.invite_valid)
        return;
    char text[64];
    Bank bank{text, sizeof(text), 0};
    const auto link = invite_link(bank);
    if (application_copy_text(link, kToastInviteCopied, kToastLinkCopyFailed))
        p.copied_until = deadline_after(clock_monotonic_milliseconds(), kCopiedMs);
}
void start_pick() noexcept {
    auto& p = *owner;
    if (p.picker.runtime || !connected() || !editable())
        return;
    p.picker_consumed = false;
    if (native_file_picker_start(&p.picker, g_app.window, 1, utf8_literal("Choose a server icon")) !=
        NativeFilePickerError::none) {
        p.failed = true;
        p.status = utf8_literal("Couldn't open the file picker.");
    }
}
void save_folder() noexcept {
    auto& p = *owner;
    const u32 color = p.color_index && p.color_index <= kGuildRoleColorCount ? kGuildRoleColors[p.color_index - 1]
                                                                            : kGuildFolderDefaultColor;
    const auto result = application_guild_folders_edit(p.folder_id, editor_text(), color, p.color_index != 0);
    if (result == GuildFolderEditError::none || result == GuildFolderEditError::unchanged) {
        application_guild_lifecycle_close();
        return;
    }
    p.failed = true;
    p.status = utf8_literal("This folder couldn't be saved.");
}
void activate(SettingsLayer*, const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    const auto target = activation.value.target;
    switch (target.kind) {
    case UiSettingsTargetKind::reset:
        // Privacy: the secondary button opens the global DM settings, as
        // the official client's modal links to them.
        if (p.mode == Mode::privacy) {
            application_guild_lifecycle_close();
            application_settings_open_key(utf8_literal("content.dms"));
            return;
        }
        application_guild_lifecycle_close();
        return;
    case UiSettingsTargetKind::close:
        application_guild_lifecycle_close();
        return;
    case UiSettingsTargetKind::save:
        if (p.mode == Mode::invite && options_changed() && !busy())
            queue(Op::invite);
        else if (p.mode == Mode::create && p.footer.can_save)
            queue(Op::create);
        else if (p.mode == Mode::folder)
            save_folder();
        if (owner)
            repaint();
        return;
    case UiSettingsTargetKind::control:
        break;
    default:
        return;
    }
    const u64 id = target.control_id;
    if (id == control_name) {
        settings_layer_focus(&p.layer, target);
        return;
    }
    if (id == control_link) {
        copy_link();
    } else if (!editable()) {
        return;
    } else if (id == control_expiry) {
        open_menu(Menu::expiry, activation.anchor);
        return;
    } else if (id == control_uses) {
        open_menu(Menu::uses, activation.anchor);
        return;
    } else if (id == control_temporary) {
        p.temporary = activation.value.checked;
        ui_settings_scene_animate_toggle(&p.scene, target, activation.value.checked, clock_monotonic_milliseconds());
    } else if (id == control_icon) {
        start_pick();
    } else if (id == control_icon_remove) {
        release_icon();
    } else if (id == control_join) {
        application_guild_lifecycle_close();
        application_guild_lifecycle_open_join();
        return;
    } else if (id == control_color) {
        p.color_index = activation.value.selected_choice <= kGuildRoleColorCount ? activation.value.selected_choice : 0;
    } else if (p.mode == Mode::privacy && id >= control_privacy_dms && id <= control_privacy_joining) {
        // A switch on allows the server: its ID leaves the restricted list.
        const bool allowed = activation.value.checked;
        if (!application_user_settings_set_guild_restricted(privacy_list(id), p.guild_id, !allowed)) {
            p.failed = true;
            p.status = utf8_literal("This setting couldn\xE2\x80\x99t be changed right now.");
            repaint();
            return;
        }
        ui_settings_scene_animate_toggle(&p.scene, target, allowed, clock_monotonic_milliseconds());
    } else {
        return;
    }
    p.failed = false;
    repaint();
}
void choose(SettingsLayer*, u32 action) noexcept {
    auto& p = *owner;
    const Menu menu = p.menu;
    p.menu = Menu::none;
    if (!editable() || action < kMenuChoiceBase)
        return;
    const u32 index = action - kMenuChoiceBase;
    if (menu == Menu::expiry && index < kGuildInviteExpiryCount)
        p.expiry = static_cast<u8>(index);
    else if (menu == Menu::uses && index < kGuildInviteUsesCount)
        p.uses = static_cast<u8>(index);
    else
        return;
    repaint();
}
bool edited(SettingsLayer*, u64 control) noexcept {
    if (control != control_name)
        return false;
    owner->failed = false;
    return true;
}
bool editor_hook(SettingsLayer*, u64 control, MessageComposer** model, BoundedEdit** binding,
                 bool* multiline) noexcept {
    if (control != control_name || owner->mode == Mode::invite)
        return false;
    *model = &owner->model;
    *binding = &owner->binding;
    *multiline = false;
    return true;
}
constexpr SettingsLayerHooks kHooks{editor_hook, edited, activate, choose,
                                    [](SettingsLayer*) noexcept { application_guild_lifecycle_close(); },
                                    [](SettingsLayer*) noexcept { return repaint(); }, nullptr,
                                    [](SettingsLayer*) noexcept { return editable(); }};

bool ensure_owner() noexcept {
    if (owner)
        return true;
    owner = static_cast<Owner*>(VirtualAlloc(nullptr, sizeof(Owner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!owner)
        return false;
    owner->layer.scene = &owner->scene;
    owner->layer.hooks = &kHooks;
    message_composer_initialize(&owner->model, {owner->name, kNameBytes});
    return true;
}
bool can_open() noexcept {
    return application_account_available() && g_app.renderer_ready && !g_app.text_form && !application_channel_overview_active() &&
           ensure_owner() && !busy();
}
// Callers check can_open() and fill their mode's fields first.
bool open_modal(Mode mode, Utf8View title, u64 focus) noexcept {
    application_settings_close(false);
    application_media_picker_close();
    application_profile_close();
    auto& p = *owner;
    const u32 title_bytes = minimum<u32>(title.bytes, sizeof(p.title));
    if (title.data != p.title)
        copy_bytes(p.title, title.data, title_bytes);
    // A previous modal's create request may still be outstanding; its
    // response is matched by ID and dropped.
    p.lifetime = ++lifetime_serial;
    p.mode = mode;
    p.menu = Menu::none;
    p.account_scope = g_app.plugin_account_scope_serial;
    p.profile_id = g_app.account.status.profile_id;
    p.queued = p.failed = false;
    p.op = Op::none;
    reset_value(&p.status);
    p.copied_until = 0;
    settings_layer_reset(&p.layer);
    if (!ui_page_registry_initialize(&p.registry, {&p.page, 1}))
        return false;
    p.content = {{p.title, title_bytes}, {}, {}, 1, kUiSettingsPageFormat, &kUiDefaultSettingsStyle, nullptr};
    if (!ui_page_registry_register(&p.registry, {kPage, p.content.title, &p.content}).ok())
        return false;
    application_composer_text_input_suspend();
    dismiss_popup();
    ui_scroll_host_cancel(&g_app.scroll_host);
    g_app.message_composer.focused = false;
    ui_settings_scene_set_presentation(&p.scene, UiSettingsPresentation::dialog);
    ui_settings_scene_bind_palette(&p.scene, {owner, resolve_color, ++p.palette_revision});
    p.scene.focus = {UiSettingsTargetKind::control, kPage, focus};
    if (!ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips()) || !project()) {
        application_guild_lifecycle_close(false);
        return false;
    }
    renderer_bind_settings_scene(&g_app.renderer, &p.scene);
    settings_layer_begin_entrance(&p.layer, clock_monotonic_milliseconds(), g_app.preferences.reduced_motion);
    g_app.context_focus = &p.scene.widgets;
    if (focus)
        settings_layer_focus(&p.layer, {UiSettingsTargetKind::control, kPage, focus});
    application_sync_attachment_drop_target();
    if (repaint())
        return true;
    application_guild_lifecycle_close(false);
    return false;
}

bool leave_valid(TextFormOwnerKey key) noexcept {
    return background_scope() && key.scope == g_background.account_scope && g_background.leave_dialog &&
           key.item == g_background.leave_guild;
}
bool leave_save(TextFormOwnerKey key, Utf8View) noexcept {
    if (!leave_valid(key))
        return true;
    auto& b = g_background;
    // Owners only read the explanation.
    if (b.leave_owner || b.leave_done)
        return true;
    if (b.leave_queued || b.leave_request)
        return false;
    b.leave_queued = true;
    b.leave_status = utf8_literal("Leaving\xE2\x80\xA6");
    ++b.leave_revision;
    pump_message_history();
    return false;
}
ApplicationTextFormProgress leave_progress(TextFormOwnerKey) noexcept {
    auto& b = g_background;
    ApplicationTextFormProgress progress{};
    progress.status = b.leave_status;
    progress.revision = b.leave_revision;
    progress.editable = false;
    progress.accept = b.leave_owner ? utf8_literal("Got it") : utf8_literal("Leave server");
    progress.can_accept = b.leave_owner || (!b.leave_queued && !b.leave_request);
    progress.can_cancel = !b.leave_owner;
    progress.complete = b.leave_done || !chat_state_store_find_guild(state(), b.leave_guild);
    return progress;
}
void leave_closed(TextFormOwnerKey) noexcept {
    g_background.leave_dialog = false;
    // An unsent leave is dropped with its dialog; one in flight finishes.
    g_background.leave_queued = false;
}

bool join_valid(TextFormOwnerKey key) noexcept {
    return background_scope() && key.scope == g_background.account_scope && g_background.join_dialog;
}
// An invite link (with or without its scheme) or a bare invite code.
bool parse_invite(Utf8View text, char* code, u8* bytes) noexcept {
    while (text.bytes && (text.data[0] == ' ' || text.data[0] == '\t')) {
        ++text.data;
        --text.bytes;
    }
    while (text.bytes && (text.data[text.bytes - 1] == ' ' || text.data[text.bytes - 1] == '\t'))
        --text.bytes;
    if (guild_invite_code_valid(text.data, text.bytes) && text.bytes <= kGuildInviteCodeMaximumBytes) {
        copy_bytes(code, text.data, text.bytes);
        *bytes = static_cast<u8>(text.bytes);
        return true;
    }
    char url[300];
    u32 used = 0;
    bool scheme = false;
    for (u32 i = 0; i + 2 < text.bytes && !scheme; ++i)
        scheme = text.data[i] == ':' && text.data[i + 1] == '/' && text.data[i + 2] == '/';
    if (!scheme) {
        copy_bytes(url, "https://", 8);
        used = 8;
    }
    if (text.bytes > sizeof(url) - used)
        return false;
    copy_bytes(url + used, text.data, text.bytes);
    used += text.bytes;
    InviteLink link{};
    if (!message_markdown_invite_link({url, used}, &link) || invite_link_event(link) ||
        link.code_bytes > kGuildInviteCodeMaximumBytes)
        return false;
    copy_bytes(code, link.code, link.code_bytes);
    *bytes = link.code_bytes;
    return true;
}
bool join_save(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!join_valid(key))
        return true;
    auto& b = g_background;
    if (!parse_invite(text, b.join_code, &b.join_code_bytes)) {
        b.join_status = utf8_literal("Enter a valid invite link or code, like https://discord.gg/abc123.");
        ++b.join_revision;
        return false;
    }
    b.join_pending = true;
    return true;
}
ApplicationTextFormProgress join_progress(TextFormOwnerKey) noexcept {
    ApplicationTextFormProgress progress{};
    progress.status = g_background.join_status;
    progress.revision = g_background.join_revision;
    progress.accept = utf8_literal("Join server");
    return progress;
}
void join_closed(TextFormOwnerKey) noexcept {
    g_background.join_dialog = false;
}

struct IdParse {
    u64 id;
    bool seen;
};
bool visit_id(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<IdParse*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("id")))
        return true;
    p.seen = !p.seen && json_value_to_snowflake(value, &p.id);
    return p.seen;
}
u64 created_id(Utf8View body) noexcept {
    const auto scanned = json_scan_structurals_avx1(body, {owner->structurals, 8192});
    if (!scanned.ok())
        return 0;
    IdParse p{};
    return json_visit_root_object(body, {owner->structurals, scanned.count}, 32, visit_id, &p).ok() && p.seen ? p.id
                                                                                                            : 0;
}
Utf8View failure(const RestClientResponse& response) noexcept {
    if (!response.transport_ok())
        return utf8_literal("Couldn't reach Discord. Try again.");
    if (response.status_code == 429)
        return utf8_literal("Slow down. Try again in a moment.");
    if (response.status_code == 403)
        return utf8_literal("You don't have permission to do this.");
    return {};
}
} // namespace

bool application_guild_lifecycle_open_invite(u64 guild_id, u64 channel_id) noexcept {
    if (!application_account_available() || !g_app.account.api.guild_admin)
        return false;
    const auto& s = state();
    const auto* guild = chat_state_store_find_guild(s, guild_id);
    const u64 selected = channel_id                                                     ? channel_id
                         : !s.home_selected && s.selected_guild_id == guild_id ? s.selected_channel_id
                                                                                : 0;
    const u64 channel = guild_context_invite_channel(s, guild_id, selected);
    if (!guild || !channel || !can_open())
        return false;
    Bank bank{owner->title, sizeof(owner->title), 0};
    bank.add("Invite friends to ");
    bank.add(chat_state_store_guild_name(s, *guild));
    owner->guild_id = guild_id;
    owner->guild_generation = guild->generation;
    owner->channel_id = channel;
    owner->expiry = owner->link_expiry = kGuildInviteDefaultExpiry;
    owner->uses = owner->link_uses = 0;
    owner->temporary = owner->link_temporary = false;
    owner->invite_valid = false;
    reset_value(&owner->invite);
    if (!open_modal(Mode::invite, {owner->title, bank.used}, control_link))
        return false;
    // The official client makes the default link at once (7 days, no limit).
    queue(Op::invite);
    return repaint();
}

bool application_guild_lifecycle_open_create() noexcept {
    if (!g_app.account.api.guild_assets || !can_open())
        return false;
    release_icon();
    owner->created_guild_id = 0;
    if (!open_modal(Mode::create, utf8_literal("Create a server"), control_name))
        return false;
    char name[kNameBytes];
    Bank bank{name, sizeof(name), 0};
    default_server_name(bank);
    load_editor(bank.since(0), kServerNameCodepoints);
    return repaint();
}

bool application_guild_lifecycle_open_folder(u64 folder_id) noexcept {
    ApplicationGuildFolder folder{};
    if (!folder_id || !application_guild_folders_folder(folder_id, &folder) || !can_open())
        return false;
    owner->folder_id = folder_id;
    owner->color_index = 0;
    for (u32 i = 0; folder.has_color && i < kGuildRoleColorCount; ++i)
        if (kGuildRoleColors[i] == folder.color)
            owner->color_index = i + 1;
    if (!open_modal(Mode::folder, utf8_literal("Folder settings"), control_name))
        return false;
    // A longer name set elsewhere stays editable; new text keeps the limit.
    const u32 existing = utf8_validate({folder.name, folder.name_bytes}).codepoints;
    load_editor({folder.name, folder.name_bytes}, maximum(kFolderNameCodepoints, existing));
    return repaint();
}

bool application_guild_lifecycle_open_privacy(u64 guild_id) noexcept {
    const auto* guild = application_account_available() ? chat_state_store_find_guild(state(), guild_id) : nullptr;
    if (!guild || guild->unavailable || !can_open())
        return false;
    owner->guild_id = guild_id;
    owner->guild_generation = guild->generation;
    owner->settings_revision = 0;
    if (!open_modal(Mode::privacy, utf8_literal("Privacy settings"), control_privacy_dms))
        return false;
    return repaint();
}

bool application_guild_lifecycle_open_join() noexcept {
    if (!application_account_available() || !background_scope() || g_app.text_form || application_channel_overview_active())
        return false;
    auto& b = g_background;
    reset_value(&b.join_status);
    ++b.join_revision;
    ApplicationTextFormRequest request{utf8_literal("Join a server"),
                                       utf8_literal("Enter an invite below to join an existing server."),
                                       utf8_literal("Invite link"),
                                       utf8_literal("Join server"),
                                       {},
                                       {b.account_scope, 0x4A4F494E, 1},
                                       join_valid,
                                       join_save,
                                       200,
                                       false};
    request.single_line = true;
    request.progress = join_progress;
    request.closed = join_closed;
    b.join_dialog = true;
    if (!application_text_form_open(request)) {
        b.join_dialog = false;
        return false;
    }
    return true;
}

bool application_guild_lifecycle_leave(u64 guild_id) noexcept {
    if (!application_account_available() || !background_scope() || g_app.text_form || !g_app.account.api.guild_admin)
        return false;
    const auto& s = state();
    const auto* guild = chat_state_store_find_guild(s, guild_id);
    if (!guild || guild->unavailable)
        return false;
    auto& b = g_background;
    if (b.leave_request)
        return false;
    b.leave_guild = guild_id;
    b.leave_owner = guild_context_owner(s, guild_id);
    b.leave_done = b.leave_queued = false;
    reset_value(&b.leave_status);
    ++b.leave_revision;
    const Utf8View name = chat_state_store_guild_name(s, *guild);
    Bank title{b.leave_title, sizeof(b.leave_title), 0};
    Bank context{b.leave_context, sizeof(b.leave_context), 0};
    if (b.leave_owner) {
        title.add("You own this server");
        context.add("Transfer ownership of ");
        context.add(name);
        context.add(" to someone else in Server settings, or delete the server, before you leave.");
    } else {
        title.add("Leave \xE2\x80\x98");
        title.add(name);
        title.add("\xE2\x80\x99");
        context.add("Are you sure you want to leave ");
        context.add(name);
        context.add("? You won\xE2\x80\x99t be able to rejoin unless you are re-invited.");
    }
    b.leave_title_bytes = static_cast<u16>(title.used);
    b.leave_context_bytes = static_cast<u16>(context.used);
    ApplicationTextFormRequest request{{b.leave_title, b.leave_title_bytes},
                                       {b.leave_context, b.leave_context_bytes},
                                       {},
                                       b.leave_owner ? utf8_literal("Got it") : utf8_literal("Leave server"),
                                       {},
                                       {b.account_scope, 0x4C45415645, guild_id},
                                       leave_valid,
                                       leave_save,
                                       0,
                                       true};
    request.progress = leave_progress;
    request.closed = leave_closed;
    request.danger = !b.leave_owner;
    b.leave_dialog = true;
    if (!application_text_form_open(request)) {
        b.leave_dialog = false;
        return false;
    }
    return true;
}

bool application_guild_lifecycle_reorder_channels(u64 guild_id, ConstSlice<GuildAdminPosition> positions) noexcept {
    if (!application_account_available() || !background_scope() || !guild_id || !positions.data || !positions.count ||
        positions.count > kGuildAdminMaximumPositions || !g_app.account.api.guild_admin)
        return false;
    auto& b = g_background;
    if (b.reorder_queued || b.reorder_request)
        return false;
    copy_bytes(b.positions, positions.data, positions.count * static_cast<u32>(sizeof(GuildAdminPosition)));
    b.position_count = positions.count;
    b.reorder_guild = guild_id;
    b.reorder_queued = true;
    b.reorder_retry = 0;
    pump_message_history();
    return true;
}

void application_guild_lifecycle_frame() noexcept {
    if (!g_app.account_ui)
        return;
    if (background_scope()) {
        auto& b = g_background;
        if (b.join_pending && !g_app.text_form && !application_channel_overview_active()) {
            b.join_pending = false;
            application_guild_invite_open({b.join_code, b.join_code_bytes});
        }
    }
    if (owner && owner->created_guild_id) {
        const u64 id = owner->created_guild_id;
        if (chat_state_store_find_guild(state(), id)) {
            owner->created_guild_id = 0;
            application_guild_lifecycle_close();
            select_content_guild(id);
        } else if (clock_monotonic_milliseconds() - owner->created_at > 15'000) {
            // The server exists; GUILD_CREATE is late. Close and let it appear.
            owner->created_guild_id = 0;
            application_guild_lifecycle_close();
        }
    }
}

bool application_guild_lifecycle_active() noexcept {
    return owner && owner->scene.active;
}

void application_guild_lifecycle_close(bool restore_focus) noexcept {
    if (!owner || !owner->scene.active)
        return;
    application_composer_text_input_suspend();
    settings_layer_close_choice(&owner->layer);
    settings_layer_cancel_pointer(&owner->layer);
    if (owner->picker.runtime) {
        native_file_picker_cancel(&owner->picker);
        native_file_picker_release(&owner->picker, 0);
    }
    // A queued request is dropped; one in flight finishes and is discarded.
    owner->queued = false;
    owner->menu = Menu::none;
    renderer_bind_settings_scene(&g_app.renderer, nullptr);
    owner->layer.entrance_started = 0;
    renderer_set_settings_modal_progress(&g_app.renderer, 1);
    ui_settings_scene_close(&owner->scene);
    if (restore_focus && application_account_available())
        g_app.context_focus = &g_app.shell_scene.widgets;
    application_sync_attachment_drop_target();
    if (g_app.renderer_ready)
        refresh_account_text(true);
    application_request_frame();
}

void application_guild_lifecycle_release() noexcept {
    application_guild_lifecycle_close(false);
    auto& b = g_background;
    if (g_app.account.instance && g_app.account.api.guild_admin) {
        if (b.leave_request)
            g_app.account.api.guild_admin(g_app.account.instance, nullptr, b.leave_request);
        if (b.reorder_request)
            g_app.account.api.guild_admin(g_app.account.instance, nullptr, b.reorder_request);
    }
    reset_value(&g_background);
    if (!owner)
        return;
    if (owner->request_id && g_app.account.instance) {
        if (owner->op == Op::create && g_app.account.api.guild_assets)
            g_app.account.api.guild_assets(g_app.account.instance, nullptr, owner->request_id);
        else if (g_app.account.api.guild_admin)
            g_app.account.api.guild_admin(g_app.account.instance, nullptr, owner->request_id);
    }
    if (owner->picker.runtime) {
        native_file_picker_cancel(&owner->picker);
        native_file_picker_release(&owner->picker, 5'000);
    }
    release_icon();
    ui_settings_scene_release(&owner->scene);
    message_composer_release(&owner->model);
    secure_clear_bytes(owner, sizeof(*owner));
    VirtualFree(owner, 0, MEM_RELEASE);
    owner = nullptr;
}

bool application_guild_lifecycle_synchronize() noexcept {
    if (!owner || !owner->scene.active)
        return true;
    auto& p = *owner;
    if (!scope_current()) {
        application_guild_lifecycle_close(false);
        return true;
    }
    bool changed = false;
    if (p.picker.runtime) {
        poll_picker();
        changed = !p.picker.runtime;
    }
    if (p.copied_until && clock_monotonic_milliseconds() >= p.copied_until) {
        p.copied_until = 0;
        changed = true;
    }
    if (p.layer.choice && !settings_layer_choice_current(p.layer)) {
        p.layer.choice = false;
        p.menu = Menu::none;
        changed = true;
    }
    // Privacy follows the synced settings: optimistic edits, PATCH results
    // and USER_SETTINGS_PROTO_UPDATE from other sessions.
    if (p.mode == Mode::privacy && p.settings_revision != application_user_settings_revision())
        changed = true;
    if (changed)
        return repaint();
    if (!ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips()))
        return false;
    return g_app.renderer.interaction_text_settings_revision == p.scene.revision ||
           renderer_rebuild_content_text(&g_app.renderer, application_theme());
}

bool application_guild_lifecycle_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    return owner && owner->scene.active && settings_layer_input(&owner->layer, kind, key, repeated);
}

bool application_guild_lifecycle_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    return application_guild_lifecycle_active() && settings_layer_window_message(&owner->layer, message, wparam, lparam);
}

u64 application_guild_lifecycle_next_wake() noexcept {
    u64 wake = 0;
    const auto earliest = [&](u64 at) noexcept {
        const u64 due = at ? at : 1;
        wake = wake ? minimum(wake, due) : due;
    };
    if (owner && owner->queued && !owner->request_id)
        earliest(owner->retry_at);
    if (g_background.leave_queued && !g_background.leave_request)
        earliest(g_background.leave_retry);
    if (g_background.reorder_queued && !g_background.reorder_request)
        earliest(g_background.reorder_retry);
    return wake;
}

// Sends a background admin request. A busy submit leaves it queued to retry shortly; any other
// outcome dequeues it, and a sent one takes the details lane.
static RestClientSubmitResult send_background(const GuildAdminRequest& request, u64 now, bool& queued, u64& retry,
                                              u64& request_id) noexcept {
    const u64 id = application_next_request_id();
    const auto submitted = g_app.account.api.guild_admin(g_app.account.instance, &request, id);
    if (submitted.error == RestClientSubmitError::busy) {
        retry = deadline_after(now, 50);
        return submitted;
    }
    queued = false;
    if (submitted.ok()) {
        request_id = id;
        claim_lane(id);
    }
    return submitted;
}

bool application_guild_lifecycle_submit(u64 now) noexcept {
    if (!g_app.account_ui || !lane_free() || !g_app.account.instance)
        return false;
    auto& b = g_background;
    // Leaving first: it is the most explicit and the shortest.
    if (b.leave_queued && !b.leave_request && (!b.leave_retry || b.leave_retry <= now) && background_scope()) {
        GuildAdminRequest request{};
        request.operation = GuildAdminOperation::guild_leave;
        request.guild_id = b.leave_guild;
        const auto sent = send_background(request, now, b.leave_queued, b.leave_retry, b.leave_request);
        if (!sent.ok() && sent.error != RestClientSubmitError::busy) {
            b.leave_status = utf8_literal("Couldn't leave this server. Try again.");
            ++b.leave_revision;
        }
        return sent.ok();
    }
    if (b.reorder_queued && !b.reorder_request && (!b.reorder_retry || b.reorder_retry <= now)) {
        GuildAdminRequest request{};
        request.operation = GuildAdminOperation::channel_reorder;
        request.guild_id = b.reorder_guild;
        request.positions = {b.positions, b.position_count};
        const auto sent = send_background(request, now, b.reorder_queued, b.reorder_retry, b.reorder_request);
        if (!sent.ok() && sent.error != RestClientSubmitError::busy)
            log_u64(LogLevel::warning, "guild_lifecycle", "reorder_submit_error", static_cast<u64>(sent.error));
        return sent.ok();
    }
    if (!owner || !owner->queued || owner->request_id || (owner->retry_at && owner->retry_at > now))
        return false;
    auto& p = *owner;
    if (!scope_current() || !connected()) {
        p.queued = false;
        p.failed = true;
        p.status = utf8_literal("Couldn't reach Discord. Try again.");
        repaint();
        return false;
    }
    const u64 id = application_next_request_id();
    RestClientSubmitResult submitted{RestClientSubmitError::invalid_input};
    if (p.op == Op::invite) {
        InviteCreate options{kGuildInviteExpirySeconds[p.expiry], kGuildInviteUses[p.uses], p.temporary, false};
        GuildAdminRequest request{};
        request.operation = GuildAdminOperation::invite_create;
        request.channel_id = p.channel_id;
        request.invite = &options;
        submitted = g_app.account.api.guild_admin(g_app.account.instance, &request, id);
    } else if (p.op == Op::create && g_app.account.api.guild_assets) {
        GuildAssetRequest request{};
        request.operation = GuildAssetOperation::guild_create;
        request.name = editor_text();
        request.image = {p.icon.data, p.icon.bytes};
        submitted = g_app.account.api.guild_assets(g_app.account.instance, &request, id);
    }
    if (submitted.error == RestClientSubmitError::busy) {
        p.retry_at = deadline_after(now, 50);
        return true;
    }
    p.queued = false;
    if (!submitted.ok()) {
        p.failed = true;
        p.status = p.op == Op::create ? utf8_literal("Your server couldn't be created.")
                                      : utf8_literal("An invite link couldn't be created.");
        repaint();
        return false;
    }
    p.request_id = id;
    claim_lane(id);
    repaint();
    return true;
}

bool application_guild_lifecycle_owns_response(u64 request_id) noexcept {
    return request_id && ((owner && owner->request_id == request_id) || g_background.leave_request == request_id ||
                          g_background.reorder_request == request_id);
}

bool application_guild_lifecycle_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!application_guild_lifecycle_owns_response(response.request_id))
        return false;
    const bool ok = response.transport_ok() && response.status_code >= 200 && response.status_code < 300;
    auto& b = g_background;
    if (response.request_id == b.leave_request) {
        b.leave_request = 0;
        if (ok) {
            // GUILD_DELETE removes the server; the dialog closes on success.
            b.leave_done = true;
            reset_value(&b.leave_status);
        } else {
            const auto text = failure(response);
            b.leave_status = text.bytes ? text : utf8_literal("Couldn't leave this server. Try again.");
        }
        ++b.leave_revision;
        application_request_frame();
        return true;
    }
    if (response.request_id == b.reorder_request) {
        b.reorder_request = 0;
        // CHANNEL_UPDATE events move the rows; a refusal leaves them in place.
        if (!ok)
            log_u64(LogLevel::warning, "guild_lifecycle", "reorder_status", response.status_code);
        return true;
    }
    auto& p = *owner;
    p.request_id = 0;
    if (!p.scene.active)
        return true;
    if (p.op == Op::invite) {
        GuildAdminInvite invite{};
        if (ok && guild_admin_decode_invite(body, {p.structurals, 8192}, &invite).ok()) {
            p.invite = invite;
            p.invite_valid = true;
            p.link_expiry = p.expiry;
            p.link_uses = p.uses;
            p.link_temporary = p.temporary;
            p.failed = false;
            reset_value(&p.status);
        } else {
            const auto text = failure(response);
            p.failed = true;
            p.status = text.bytes ? text : utf8_literal("An invite link couldn't be created.");
        }
        return repaint();
    }
    if (p.op == Op::create) {
        const u64 id = ok ? created_id(body) : 0;
        if (id) {
            p.created_guild_id = id;
            p.created_at = clock_monotonic_milliseconds();
            return repaint();
        }
        const auto text = failure(response);
        p.failed = true;
        p.status = text.bytes ? text
                   : response.status_code == 400
                       ? utf8_literal("Discord refused this server. You may be in too many servers.")
                       : utf8_literal("Your server couldn't be created.");
        return repaint();
    }
    return true;
}

u64 application_guild_lifecycle_caret_wake() noexcept {
    if (!application_guild_lifecycle_active())
        return 0;
    u64 wake = settings_layer_next_wake(owner->layer);
    if (owner->copied_until && (!wake || owner->copied_until < wake))
        wake = owner->copied_until;
    if (owner->picker.runtime) {
        const u64 poll = deadline_after(clock_monotonic_milliseconds(), 50);
        wake = wake ? minimum(wake, poll) : poll;
    }
    return wake;
}

void application_guild_lifecycle_tick(u64 now) noexcept {
    if (application_guild_lifecycle_active())
        settings_layer_tick(&owner->layer, now, g_app.preferences.reduced_motion);
}

bool application_guild_lifecycle_edit_focused() noexcept {
    return application_guild_lifecycle_active() &&
           settings_layer_editor(&owner->layer, nullptr, nullptr, nullptr, nullptr);
}

MessageComposer* application_guild_lifecycle_edit_model() noexcept {
    MessageComposer* model = nullptr;
    return application_guild_lifecycle_active() &&
                   settings_layer_editor(&owner->layer, nullptr, &model, nullptr, nullptr)
               ? model
               : nullptr;
}

TextInputTarget application_guild_lifecycle_edit_target() noexcept {
    BoundedEdit* binding = nullptr;
    return application_guild_lifecycle_active() &&
                   settings_layer_editor(&owner->layer, nullptr, nullptr, &binding, nullptr)
               ? bounded_edit_input_target(binding)
               : TextInputTarget{};
}

void application_guild_lifecycle_edit_cancel_native_group(TextInputTarget target) noexcept {
    if (owner)
        settings_layer_cancel_native_group(&owner->layer, target);
}

UiRect application_guild_lifecycle_edit_bounds() noexcept {
    return application_guild_lifecycle_active() ? settings_layer_edit_bounds(&owner->layer) : UiRect{};
}

bool application_guild_lifecycle_edit_changed(bool content) noexcept {
    return application_guild_lifecycle_active() && settings_layer_edit_changed(&owner->layer, content);
}

u32 application_guild_lifecycle_owner_bytes() noexcept {
    return owner ? sizeof(*owner) : 0;
}
} // namespace maicord::app_detail
