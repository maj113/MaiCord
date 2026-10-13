#include "application_tooltip.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_settings.hpp"
#include "application_friends.hpp"
#include "application_thread_browser.hpp"
#include "application_video.hpp"
#include "application_guild_folders.hpp"
#include "application_voice.hpp"
#include "application_plugins.hpp"
#include "application_reaction_tooltip.hpp"
#include "maicord/ui/plugin_slots.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/tooltip.hpp"
#include "maicord/ui/call_scene.hpp"
#include "maicord/ui/theme_editor.hpp"
#include "maicord/ui/pinned_messages_scene.hpp"

#include <Windows.h>

namespace maicord::app_detail {
struct ApplicationTooltip {
    UiTooltipScene scene;
    UiTooltipKey dismissed;
    UiTooltipKey failed;
    UiRect failed_anchor;
    UiPoint point;
    UiWidgetManager widgets;
    UiWidget root;
    UiLayoutNode root_layout;
    UiRect viewport;
    u32 published_revision;
    bool published_visible;
    bool pointer_valid, keyboard, pressed;
    bool viewport_failed;
};
static_assert(sizeof(ApplicationTooltip) <= 4096);
namespace {
bool content_available() noexcept {
    return g_app.renderer_ready && g_app.account_ui && !g_app.login.visible &&
           !(g_app.renderer.forum_form && g_app.renderer.forum_form->scene && g_app.renderer.forum_form->scene->active) &&
           (!application_settings_active() || renderer_theme_editor_visible(g_app.renderer)) &&
           !g_app.text_form && !g_app.popup_scene.active && !g_app.reaction_viewer.active &&
           !g_app.image_viewer.scene.active &&
           !(g_app.renderer.profile_scene && g_app.renderer.profile_scene->active) &&
           !application_friends_requests_active() && !IsIconic(g_app.window);
}
bool widget_target(const UiWidgetManager& manager, UiWidgetHandle handle, u64 scope, u64 identity,
                   u64 revision, u32 action, Utf8View label, UiTooltipTarget* target) noexcept {
    if (!target || !label.bytes || !ui_widget_manager_input_enabled(manager, handle))
        return false;
    const auto* node = ui_widget_manager_layout_node(manager, handle);
    if (!node)
        return false;
    const auto anchor = ui_rect_intersection(node->rectangle, node->clip_rectangle);
    if (!ui_rect_has_area(anchor))
        return false;
    *target = {{{&manager, handle}, scope, identity, revision, action}, anchor, label};
    return true;
}
bool shell_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto& manager = g_app.shell_scene.widgets;
    UiRect voice_anchor{};
    u32 voice_identity = 0;
    if (const Utf8View voice = keyboard ? Utf8View{}
                                        : application_voice_tooltip(point.x, point.y, &voice_anchor, &voice_identity);
        voice.bytes) {
        const auto footer = ui_widget_manager_find(manager, static_cast<u32>(UiSemantic::channel_footer) + 1);
        *target = {{{&manager, footer}, scope, 0x5600u + voice_identity, voice_identity, 0x5600u + voice_identity},
                   voice_anchor,
                   voice};
        return true;
    }
    const auto handle = keyboard ? manager.focused : ui_widget_manager_hit_test(manager, point.x, point.y);
    const auto* widget = ui_widget_manager_resolve(manager, handle);
    if (!widget)
        return false;
    Utf8View label{};
    if (widget->id == static_cast<u32>(UiSemantic::settings) + 1)
        label = utf8_literal("User settings");
    else if (widget->id == static_cast<u32>(UiSemantic::member_toggle) + 1)
        label = g_app.preferences.member_pane_visible ? utf8_literal("Hide member list")
                                                      : utf8_literal("Show member list");
    else if (widget->id == static_cast<u32>(UiSemantic::pins_toggle) + 1)
        label = utf8_literal("Pinned messages");
    else if (widget->id == static_cast<u32>(UiSemantic::inbox_toggle) + 1)
        label = utf8_literal("Inbox");
    else if (widget->id == static_cast<u32>(UiSemantic::threads_toggle) + 1)
        label = utf8_literal("Browse threads");
    else if (widget->id == static_cast<u32>(UiSemantic::friends_button) + 1)
        label = utf8_literal("Friends");
    else if (widget->id == static_cast<u32>(UiSemantic::private_create) + 1)
        label = utf8_literal("Create DM");
    else if (widget->id == static_cast<u32>(UiSemantic::private_add) + 1)
        label = utf8_literal("Add friends to DM");
    else if (widget->id == static_cast<u32>(UiSemantic::private_call) + 1)
        label = utf8_literal("Start voice call");
    else if (widget->id == static_cast<u32>(UiSemantic::home_button) + 1)
        label = utf8_literal("Direct messages");
    else if (widget->id == static_cast<u32>(UiSemantic::channel_footer) + 1) {
        // The widget manager skips disabled children. A disabled Settings gear
        // must not inherit its profile-footer parent's hint at the same point.
        const auto settings = ui_widget_manager_find(manager, static_cast<u32>(UiSemantic::settings) + 1);
        const auto* settings_node = ui_widget_manager_layout_node(manager, settings);
        if (!keyboard && settings_node && settings_node->visible &&
            ui_rect_contains(ui_rect_intersection(settings_node->rectangle, settings_node->clip_rectangle),
                             point.x, point.y))
            return false;
        label = utf8_literal("Your account");
    }
    return widget_target(manager, handle, scope, widget->id, g_app.preferences.member_pane_visible,
                         widget->id, label, target);
}
// Call view controls; the scene owns its labels and state (mute, fullscreen).
bool call_target(UiPoint point, u64 scope, UiTooltipTarget* target) noexcept {
    const auto* scene = g_app.renderer.call_scene;
    UiRect anchor{};
    u32 identity = 0;
    const Utf8View label = scene ? ui_call_scene_tooltip(*scene, point.x, point.y, &anchor, &identity) : Utf8View{};
    if (!label.bytes || !ui_rect_has_area(anchor))
        return false;
    *target = {{scene->owner, scope, 0xca00u + identity, identity, 0xca00u + identity}, anchor, label};
    return true;
}
bool friends_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto* scene = g_app.renderer.friends_scene;
    if (!scene || !application_friends_active())
        return false;
    UiWidgetHandle widget{};
    UiFriendsTarget source{};
    Utf8View label{};
    return ui_friends_scene_icon_action(*scene, keyboard, point.x, point.y, &widget, &source, &label) &&
           widget_target(scene->widgets, widget, scope, source.user_id, scene->revision,
                         0xe00u + static_cast<u32>(source.action), label, target);
}
bool guild_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto& scene = g_app.guild_rail_scene;
    if (!scene.active)
        return false;
    const auto handle = keyboard ? scene.widgets.focused : ui_widget_manager_hit_test(scene.widgets, point.x, point.y);
    const auto* row = guild_rail_scene_resolve(scene, handle);
    if (!row && scene.add_button.generation && handle.slot == scene.add_button.slot &&
        handle.generation == scene.add_button.generation) {
        if (!widget_target(scene.widgets, handle, scope, 0, 1, 0xd01u, utf8_literal("Add a server"), target))
            return false;
        target->placement = UiTooltipPlacement::right;
        return true;
    }
    char name[kUiTooltipMaximumTextBytes];
    const Utf8View label = row ? application_guild_rail_label(row->view, {name, sizeof(name)}) : Utf8View{};
    if (!row || !widget_target(scene.widgets, handle, scope, row->view.id, row->view.revision, 0xd00u, label, target))
        return false;
    target->placement = UiTooltipPlacement::right;
    return true;
}
bool composer_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    if (application_friends_active() || application_thread_browser_read_blocked() || !g_app.message_composer_scene.active)
        return false;
    const auto& scene = g_app.message_composer_scene;
    const auto control = keyboard ? message_composer_scene_focused_control(scene)
                                  : message_composer_scene_hit_test(scene, point.x, point.y);
    Utf8View label{};
    UiWidgetHandle handle{};
    const UiComposerVoiceMode voice = message_composer_scene_voice_mode(scene);
    const bool voice_bar = ui_composer_voice_bar_visible(voice);
    switch (control) {
    case MessageComposerControl::add_attachment:
        label = voice_bar ? utf8_literal("Delete voice message") : utf8_literal("Upload or create");
        handle = scene.add_attachment;
        break;
    case MessageComposerControl::gif:
        label = utf8_literal("GIFs");
        handle = scene.gif;
        break;
    case MessageComposerControl::sticker:
        label = utf8_literal("Stickers");
        handle = scene.sticker;
        break;
    case MessageComposerControl::emoji:
        label = !voice_bar                                ? utf8_literal("Emoji")
                : voice == UiComposerVoiceMode::recording ? utf8_literal("Stop recording")
                : scene.voice && scene.voice->playing     ? utf8_literal("Pause voice message")
                                                          : utf8_literal("Play voice message");
        handle = scene.emoji;
        break;
    case MessageComposerControl::send:
        label = g_app.message_edit.editor_active          ? utf8_literal("Save edit")
                : voice == UiComposerVoiceMode::available ? utf8_literal("Record Voice Message")
                : voice_bar                               ? utf8_literal("Send Voice Message")
                                                          : utf8_literal("Send message");
        handle = scene.send;
        break;
    case MessageComposerControl::cancel_reply:
        label = g_app.message_edit.editor_active ? utf8_literal("Cancel edit") : utf8_literal("Cancel reply");
        handle = scene.reply_cancel;
        break;
    case MessageComposerControl::reply_mention:
        label = scene.mention_reply_author ? utf8_literal("Click to disable pinging the original author.")
                                          : utf8_literal("Click to enable pinging the original author.");
        handle = scene.reply_mention;
        break;
    case MessageComposerControl::attachments: {
        // Upload tile action bar: the official client's per-button labels.
        if (keyboard)
            return false;
        const auto hit = message_composer_scene_attachment_hit(scene, point.x, point.y);
        const auto tiles = message_composer_scene_attachment_tiles(scene);
        if (hit.index >= tiles.count || hit.part == UiComposerAttachmentPart::tile ||
            hit.part == UiComposerAttachmentPart::none)
            return false;
        const auto& tile = tiles[hit.index];
        label = hit.part == UiComposerAttachmentPart::retry     ? utf8_literal("Retry upload")
                : hit.part == UiComposerAttachmentPart::spoiler ? (tile.spoiler ? utf8_literal("Remove spoiler")
                                                                                : utf8_literal("Spoiler attachment"))
                : hit.part == UiComposerAttachmentPart::edit    ? utf8_literal("Modify attachment")
                                                                : utf8_literal("Remove attachment");
        const auto view = message_composer_scene_attachment_view(scene, false);
        const auto geometry = ui_composer_attachment_tile_geometry(view.strip, tile, hit.index, view.scroll);
        UiRect anchor{};
        for (u32 i = 0; i < geometry.button_count; ++i)
            if (geometry.parts[i] == hit.part)
                anchor = ui_rect_intersection(geometry.buttons[i], view.strip);
        if (!ui_rect_has_area(anchor) || !ui_widget_manager_input_enabled(scene.widgets, scene.attachments))
            return false;
        *target = {{{&scene.widgets, scene.attachments}, scope, tile.id, tile.spoiler ? 1u : 0u,
                    0x300u + static_cast<u32>(hit.part)},
                   anchor,
                   label};
        return true;
    }
    case MessageComposerControl::plugin_slot: {
        // Plugin tooltip text stays borrowed until the next slot revision.
        const u32 index = keyboard ? message_composer_scene_focused_plugin_slot(scene)
                                   : message_composer_scene_plugin_slot(scene, point.x, point.y);
        UiPluginSlotNode nodes[kMessageComposerPluginSlots]{};
        if (index >= ui_plugin_slot_toolbar(kUiPluginSlotComposerToolbar, {nodes, kMessageComposerPluginSlots}))
            return false;
        return widget_target(scene.widgets, scene.plugin_slots[index], scope, g_app.account_ui->active_channel_id,
                             application_plugins_slot_revision(), 0x180u + index,
                             nodes[index].tooltip.bytes ? nodes[index].tooltip : nodes[index].text, target);
    }
    default:
        return false;
    }
    return widget_target(scene.widgets, handle, scope, g_app.account_ui->active_channel_id,
                         scene.reply_message_id, 0x100u + static_cast<u32>(control), label, target);
}
bool message_target(UiPoint point, u64 scope, UiTooltipTarget* target) noexcept {
    const auto& bar = g_app.message_action_bar;
    if (application_friends_active() || application_thread_browser_read_blocked() || !bar.active || bar.owner.manager != &g_app.message_scene.widgets)
        return false;
    const auto hit = message_action_bar_hit_test(bar, point.x, point.y);
    Utf8View label{};
    switch (hit.action) {
    case MessageActionBarAction::reply: label = utf8_literal("Reply"); break;
    case MessageActionBarAction::forward: label = utf8_literal("Forward"); break;
    case MessageActionBarAction::add_reaction: label = utf8_literal("Add reaction"); break;
    case MessageActionBarAction::more: label = utf8_literal("More"); break;
    case MessageActionBarAction::edit: label = utf8_literal("Edit"); break;
    case MessageActionBarAction::copy_link: label = utf8_literal("Copy message link"); break;
    case MessageActionBarAction::mark_unread: label = utf8_literal("Mark unread"); break;
    case MessageActionBarAction::pin: label = utf8_literal("Pin message"); break;
    case MessageActionBarAction::unpin: label = utf8_literal("Unpin message"); break;
    case MessageActionBarAction::delete_message: label = utf8_literal("Delete message"); break;
    case MessageActionBarAction::copy_id: label = utf8_literal("Copy message ID"); break;
    case MessageActionBarAction::retry: label = utf8_literal("Retry send"); break;
    case MessageActionBarAction::plugin_0:
    case MessageActionBarAction::plugin_1: label = application_plugins_hover_label(hit.catalog_index); break;
    default: break;
    }
    if (!label.bytes)
        return false;
    const auto found = message_store_find(g_app.message_view.store, bar.message_id);
    const auto* message =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    if (!message || message->channel_id != g_app.account_ui->active_channel_id ||
        !ui_widget_manager_resolve(g_app.message_scene.widgets, bar.owner.handle))
        return false;
    const auto anchor = ui_rect_intersection(hit.rectangle, bar.clip);
    if (!ui_rect_has_area(anchor))
        return false;
    *target = {{bar.owner, scope, message->id, message->revision, 0x200u + static_cast<u32>(hit.action)},
               anchor,
               label};
    return true;
}
bool search_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto* scene = g_app.renderer.search_scene;
    if (!scene || !scene->active)
        return false;
    const auto& manager = scene->widgets;
    const auto handle = keyboard ? manager.focused : ui_widget_manager_hit_test(manager, point.x, point.y);
    if (!handle.generation || handle.slot >= 48)
        return false;
    const auto source = scene->targets[handle.slot];
    Utf8View label{};
    switch (source.action) {
    case UiSearchAction::edit:
        label = utf8_literal("Search messages");
        break;
    case UiSearchAction::submit:
        label = utf8_literal("Search");
        break;
    case UiSearchAction::close:
        label = utf8_literal("Close search");
        break;
    case UiSearchAction::filters:
        label = utf8_literal("Search filters");
        break;
    case UiSearchAction::previous:
        label = utf8_literal("Previous results");
        break;
    case UiSearchAction::next:
        label = utf8_literal("Next results");
        break;
    case UiSearchAction::newest:
        label = utf8_literal("Newest first");
        break;
    case UiSearchAction::oldest:
        label = utf8_literal("Oldest first");
        break;
    case UiSearchAction::relevance:
        label = utf8_literal("Most relevant");
        break;
    case UiSearchAction::result:
        label = utf8_literal("Jump to message");
        break;
    case UiSearchAction::scope:
        label = utf8_literal("Search the whole server");
        break;
    default:
        return false;
    }
    return widget_target(manager, handle, scope,
                         source.message_id ? source.message_id : static_cast<u64>(source.action),
                         scene->source_revision, 0x400u + static_cast<u32>(source.action), label, target);
}
bool pinned_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto* scene = g_app.renderer.pinned_messages_scene;
    if (!scene || !ui_pinned_messages_scene_current(*scene)) return false;
    const auto handle = keyboard ? scene->widgets.focused
                                : ui_widget_manager_hit_test(scene->widgets, point.x, point.y);
    const auto source = ui_pinned_messages_scene_target(*scene, handle);
    Utf8View label{};
    switch (source.action) {
    case UiPinnedMessagesAction::close: label = utf8_literal("Close pins"); break;
    case UiPinnedMessagesAction::refresh: label = utf8_literal("Refresh pins"); break;
    case UiPinnedMessagesAction::jump: label = utf8_literal("Jump to message"); break;
    case UiPinnedMessagesAction::unpin: label = utf8_literal("Unpin message"); break;
    case UiPinnedMessagesAction::previous: label = utf8_literal("Newer pins"); break;
    case UiPinnedMessagesAction::next: label = utf8_literal("Older pins"); break;
    case UiPinnedMessagesAction::retry: label = utf8_literal("Retry loading pins"); break;
    default: return false;
    }
    return widget_target(scene->widgets, handle, scope,
                         source.message_id ? source.message_id : source.channel_id,
                         scene->source_revision, 0x900u + static_cast<u32>(source.action), label, target);
}
bool browser_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto* scene = application_thread_browser_scene();
    if (!scene || !ui_thread_browser_scene_current(*scene)) return false;
    const auto handle = keyboard ? scene->widgets.focused
                                : ui_widget_manager_hit_test(scene->widgets, point.x, point.y);
    const auto source = ui_thread_browser_scene_target(*scene, handle);
    if (!ui_thread_browser_scene_target_current(*scene, source)) return false;
    Utf8View label{};
    switch (source.action) {
    case UiThreadBrowserAction::close: label = utf8_literal("Close thread browser"); break;
    case UiThreadBrowserAction::active: label = utf8_literal("Active threads"); break;
    case UiThreadBrowserAction::archived: label = utf8_literal("Archived threads"); break;
    case UiThreadBrowserAction::previous: label = utf8_literal("Previous threads"); break;
    case UiThreadBrowserAction::next: label = utf8_literal("Next threads"); break;
    case UiThreadBrowserAction::retry: label = utf8_literal("Retry loading threads"); break;
    case UiThreadBrowserAction::open_thread:
        label = scene->forum ? utf8_literal("Open post") : utf8_literal("Open thread");
        break;
    case UiThreadBrowserAction::forum_sort: label = utf8_literal("Sort by"); break;
    case UiThreadBrowserAction::forum_list: label = utf8_literal("List"); break;
    case UiThreadBrowserAction::forum_gallery: label = utf8_literal("Gallery"); break;
    case UiThreadBrowserAction::forum_tag: label = utf8_literal("Filter by tag"); break;
    default: return false;
    }
    return widget_target(scene->widgets, handle, scope,
                         source.thread_id ? source.thread_id : static_cast<u64>(source.action),
                         source.source_publication, 0xb00u + static_cast<u32>(source.action), label, target);
}
bool gallery_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto* scene = g_app.renderer.media_picker_scene;
    if (!scene || !ui_media_picker_scene_current(*scene))
        return false;
    const auto handle =
        keyboard ? scene->widgets.focused : ui_widget_manager_hit_test(scene->widgets, point.x, point.y);
    if (!handle.generation || handle.slot >= kUiMediaPickerWidgetCapacity)
        return false;
    const auto source = scene->projection->targets[handle.slot];
    Utf8View label{};
    auto action = source.action;
    UiRect favorite_anchor{};
    switch (source.action) {
    case UiMediaPickerAction::edit:
        label = scene->content->placeholder;
        break;
    case UiMediaPickerAction::back:
        label = utf8_literal("Back to GIF categories");
        break;
    case UiMediaPickerAction::tab:
        label = source.tab == UiMediaPickerKind::gif       ? utf8_literal("GIFs")
                : source.tab == UiMediaPickerKind::sticker ? utf8_literal("Stickers")
                                                           : utf8_literal("Emoji");
        break;
    case UiMediaPickerAction::item:
    case UiMediaPickerAction::category:
        for (u32 i = 0; i < scene->cell_count; ++i) {
            const auto& cell = scene->projection->cells[i];
            if (cell.widget.slot == handle.slot) {
                if (!keyboard && ui_media_picker_scene_favorite(*scene, i, &favorite_anchor) &&
                    ui_rect_contains(favorite_anchor, point.x, point.y)) {
                    action = UiMediaPickerAction::favorite;
                    label = cell.item.favorite_pending ? utf8_literal("Updating favorite")
                            : cell.item.favorite_failed
                                ? utf8_literal("Couldn't update favorite. Try again (F)")
                            : cell.item.favorite ? utf8_literal("Remove from favorites (F)")
                                                 : utf8_literal("Add to favorites (F)");
                } else
                    label = cell.item.detail.bytes ? cell.item.detail : cell.item.label;
                break;
            }
        }
        break;
    case UiMediaPickerAction::section:
    case UiMediaPickerAction::section_jump:
        if (source.section_index < scene->source->sections.count)
            label = scene->source->sections[source.section_index].label;
        break;
    default:
        return false;
    }
    if (!label.bytes ||
        !widget_target(scene->widgets, handle, scope, source.source_index + 1, scene->source_revision,
                       0x600u + static_cast<u32>(action), label, target))
        return false;
    if (action == UiMediaPickerAction::favorite)
        target->anchor = favorite_anchor;
    return true;
}
bool theme_editor_target(UiPoint point, bool keyboard, u64 scope, UiTooltipTarget* target) noexcept {
    const auto* scene = g_app.renderer.theme_editor_scene;
    if (!scene || !renderer_theme_editor_visible(g_app.renderer))
        return false;
    const auto handle =
        keyboard ? scene->widgets.focused : ui_widget_manager_hit_test(scene->widgets, point.x, point.y);
    for (u32 i = 0; i < kUiThemeEditorElements; ++i) {
        if (handle.slot != scene->elements[i].slot || handle.generation != scene->elements[i].generation)
            continue;
        const auto element = static_cast<UiThemeEditorElement>(i);
        Utf8View label{};
        switch (element) {
        case UiThemeEditorElement::close:
            label = utf8_literal("Back to settings");
            break;
        case UiThemeEditorElement::dark:
            label = utf8_literal("Dark appearance");
            break;
        case UiThemeEditorElement::light:
            label = utf8_literal("Light appearance");
            break;
        case UiThemeEditorElement::add:
            label = utf8_literal("Add a gradient color");
            break;
        case UiThemeEditorElement::remove:
            label = utf8_literal("Remove selected color");
            break;
        case UiThemeEditorElement::hex:
            label = utf8_literal("Edit color value");
            break;
        case UiThemeEditorElement::surprise:
            label = utf8_literal("Create a coordinated palette");
            break;
        case UiThemeEditorElement::reset:
            label = utf8_literal("Reset your gradient");
            break;
        default:
            return false;
        }
        return widget_target(scene->widgets, handle, scope, g_app.theme_editor_lifetime_serial,
                             scene->generation, 0x700u + i, label, target);
    }
    return false;
}
// Plugin badges on channel/DM rows and server icons, account panel buttons.
bool plugin_surface_target(UiPoint point, u64 scope, UiTooltipTarget* target) noexcept {
    ApplicationPluginSurfaceHit hit{};
    if (!application_plugins_surface_hit(point.x, point.y, &hit) || !hit.label.bytes || !hit.manager ||
        !ui_rect_has_area(hit.anchor))
        return false;
    const u64 identity = hit.target.channel_id ? hit.target.channel_id : hit.target.guild_id;
    *target = {{{hit.manager, hit.owner}, scope, identity, application_plugins_slot_revision(),
                0x5700u + hit.target.surface * 4 + hit.index},
               hit.anchor,
               hit.label};
    if (hit.target.surface == kUiPluginSlotGuildRail)
        target->placement = UiTooltipPlacement::right;
    return true;
}
bool resolve_target(UiPoint point, bool keyboard, UiTooltipTarget* target) noexcept {
    // Check current host ownership before touching any old scene descriptor.
    if (!content_available() || GetCapture() == g_app.window)
        return false;
    const u64 scope = g_app.plugin_account_scope_serial ? g_app.plugin_account_scope_serial
                                                        : g_app.account.status.profile_id;
    if (!scope)
        return false;
    if (renderer_theme_editor_visible(g_app.renderer))
        return (!keyboard || g_app.keyboard_focus_visible) &&
               theme_editor_target(point, keyboard, scope, target);
    if (keyboard) {
        if (!g_app.keyboard_focus_visible)
            return false;
        if (application_friends_active())
            return friends_target(point, true, scope, target);
        if (g_app.renderer.media_picker_scene &&
            g_app.context_focus == &g_app.renderer.media_picker_scene->widgets)
            return gallery_target(point, true, scope, target);
        if (const auto* browser = application_thread_browser_scene();
            browser && g_app.context_focus == &browser->widgets)
            return browser_target(point, true, scope, target);
        if (g_app.renderer.search_scene && g_app.context_focus == &g_app.renderer.search_scene->widgets)
            return search_target(point, true, scope, target);
        if (g_app.renderer.pinned_messages_scene &&
            g_app.context_focus == &g_app.renderer.pinned_messages_scene->widgets)
            return pinned_target(point, true, scope, target);
        if (!application_thread_browser_read_blocked() &&
            application_video_tooltip_target(point, true, scope, target))
            return true;
        if (g_app.context_focus == &g_app.shell_scene.widgets)
            return shell_target(point, true, scope, target);
        if (g_app.context_focus == &g_app.guild_rail_scene.widgets)
            return guild_target(point, true, scope, target);
        if (g_app.context_focus == &g_app.message_composer_scene.widgets)
            return composer_target(point, true, scope, target);
        return false;
    }
    if (g_app.renderer.media_picker_scene &&
        ui_rect_contains(g_app.renderer.media_picker_scene->panel, point.x, point.y))
        return gallery_target(point, false, scope, target);
    if (const auto* call = g_app.renderer.call_scene; call && ui_call_scene_contains(*call, point.x, point.y))
        return call_target(point, scope, target);
    if (friends_target(point, false, scope, target))
        return true;
    if (const auto* browser = application_thread_browser_scene();
        browser && ui_thread_browser_scene_contains(*browser, point.x, point.y))
        return browser_target(point, false, scope, target);
    if (g_app.renderer.search_scene &&
        ui_search_scene_contains(*g_app.renderer.search_scene, point.x, point.y))
        return search_target(point, false, scope, target);
    if (g_app.renderer.pinned_messages_scene &&
        ui_pinned_messages_scene_contains(*g_app.renderer.pinned_messages_scene, point.x, point.y))
        return pinned_target(point, false, scope, target);
    if (!application_thread_browser_read_blocked() &&
        (application_video_tooltip_target(point, false, scope, target) ||
         message_target(point, scope, target) ||
         (!application_friends_active() && application_reaction_tooltip_target(point, scope, target))))
        return true;
    return plugin_surface_target(point, scope, target) || composer_target(point, false, scope, target) ||
           guild_target(point, false, scope, target) || shell_target(point, false, scope, target);
}
ApplicationTooltip* ensure() noexcept {
    if (g_app.tooltip)
        return g_app.tooltip;
    auto* owner = static_cast<ApplicationTooltip*>(
        VirtualAlloc(nullptr, sizeof(ApplicationTooltip), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!owner)
        return nullptr;
    if (!ui_widget_manager_initialize(&owner->widgets, {&owner->root, 1}, {&owner->root_layout, 1})) {
        VirtualFree(owner, 0, MEM_RELEASE);
        return nullptr;
    }
    auto style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.flags |= ui_layout_node_clip_children;
    const auto root = ui_widget_manager_add(
        &owner->widgets, ui_widget_invalid_handle(),
        {1, UiWidgetKind::container, ui_widget_none, {utf8_literal("Tooltip overlay"), style, {}, 0, 1}});
    if (!root.ok() || !ui_tooltip_bind_text_owner(&owner->scene, {&owner->widgets, root.handle})) {
        VirtualFree(owner, 0, MEM_RELEASE);
        return nullptr;
    }
    g_app.tooltip = owner;
    g_app.renderer.tooltip_scene = &owner->scene;
    return owner;
}
bool refresh(ApplicationTooltip& owner, u64 now_ms) noexcept {
    const bool old_visible = owner.published_visible;
    const u32 old_revision = owner.published_revision;
    const auto viewport = application_viewport_dips();
    if (owner.viewport_failed && ui_rect_equal(viewport, owner.viewport))
        return true;
    if (!ui_rect_equal(viewport, owner.viewport)) {
        const bool observed = ui_rect_has_area(owner.viewport);
        // A resize invalidates the old pointer/focus observation and its delay.
        // A fresh native input event may offer the newly laid-out source.
        if (observed) {
            ui_tooltip_clear(&owner.scene);
            owner.pointer_valid = owner.keyboard = false;
            owner.dismissed = owner.failed = {};
        }
        if (!ui_widget_manager_layout_rectangle(&owner.widgets, viewport).ok()) {
            ui_tooltip_clear(&owner.scene);
            owner.viewport = viewport;
            owner.viewport_failed = true;
            owner.pointer_valid = owner.keyboard = false;
            owner.published_visible = false;
            owner.published_revision = owner.scene.revision;
            g_app.renderer.retained_presentation.valid = false;
            application_request_frame();
            return false;
        }
        owner.viewport = viewport;
        owner.viewport_failed = false;
    }
    UiTooltipTarget target{};
    const bool resolved = !owner.pressed && (owner.keyboard || owner.pointer_valid) &&
                          resolve_target(owner.point, owner.keyboard, &target);
    // Reaction pills lead with their emoji; their names wrap beside it.
    const bool reaction = resolved && (media_asset_key_valid(target.emoji.media) || target.emoji.glyph.bytes);
    if (!reaction)
        application_reaction_tooltip_leave();
    if (!resolved || ui_tooltip_key_equal(owner.dismissed, target.key) ||
        (ui_tooltip_key_equal(owner.failed, target.key) &&
         ui_rect_equal(owner.failed_anchor, target.anchor))) {
        ui_tooltip_clear(&owner.scene);
    } else {
        owner.dismissed = {};
        owner.failed = {};
        UiTooltipStyle style{};
        if (reaction) {
            style.maximum_width = 260;
            style.maximum_height = 120;
        }
        ui_tooltip_offer(&owner.scene, target, viewport,
                         owner.keyboard ? UiTooltipTrigger::keyboard : UiTooltipTrigger::pointer, now_ms, style);
        const bool due = owner.scene.pending && now_ms >= owner.scene.show_at_ms;
        ui_tooltip_tick(&owner.scene, now_ms,
                        g_app.account_ui ? &g_app.account_ui->text_measure_cache : nullptr);
        if (due && !owner.scene.pending && !owner.scene.visible)
            owner.dismissed = target.key;
    }
    if (old_visible != owner.scene.visible || old_revision != owner.scene.revision) {
        g_app.renderer.retained_presentation.valid = false;
        if (!renderer_rebuild_content_text(&g_app.renderer, application_theme())) {
            // Failed text publication must not remain a dirty visible revision
            // that requests and retries another frame forever. Suppress the exact
            // failed source until its identity/geometry changes or explicit clear.
            owner.failed = target.key;
            owner.failed_anchor = target.anchor;
            ui_tooltip_clear(&owner.scene);
            owner.published_visible = false;
            owner.published_revision = owner.scene.revision;
            application_request_frame();
            return false;
        }
        application_request_frame();
    }
    owner.published_visible = owner.scene.visible;
    owner.published_revision = owner.scene.revision;
    application_reaction_tooltip_pump();
    return true;
}
} // namespace

u32 application_tooltip_owner_bytes() noexcept {
    return sizeof(ApplicationTooltip);
}
void application_tooltip_clear() noexcept {
    if (!g_app.tooltip)
        return;
    auto& owner = *g_app.tooltip;
    const bool visible = owner.scene.visible;
    ui_tooltip_clear(&owner.scene);
    owner.pointer_valid = owner.keyboard = owner.pressed = false;
    owner.dismissed = {};
    owner.failed = {};
    if (visible) {
        g_app.renderer.retained_presentation.valid = false;
        application_request_frame();
    }
}
void application_tooltip_release() noexcept {
    application_tooltip_clear();
    auto* owner = g_app.tooltip;
    g_app.tooltip = nullptr;
    g_app.renderer.tooltip_scene = nullptr;
    if (owner)
        VirtualFree(owner, 0, MEM_RELEASE);
}
u64 application_tooltip_next_wake() noexcept {
    return g_app.tooltip ? ui_tooltip_next_wake(g_app.tooltip->scene) : 0;
}
bool application_tooltip_synchronize(u64 now_ms) noexcept {
    return !g_app.tooltip || refresh(*g_app.tooltip, now_ms);
}
void application_tooltip_input(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_MOUSELEAVE || message == WM_KILLFOCUS || message == WM_CANCELMODE ||
        message == WM_CAPTURECHANGED || message == WM_SIZE || message == WM_DPICHANGED ||
        message == WM_ENTERSIZEMOVE || message == WM_EXITSIZEMOVE) {
        application_tooltip_clear();
        return;
    }
    const bool pointer = message == WM_MOUSEMOVE;
    const bool keyboard = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool press = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                       message == WM_MBUTTONDOWN || message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL;
    if (!pointer && !keyboard && !press)
        return;
    if (press) {
        if (g_app.tooltip) {
            g_app.tooltip->pressed = true;
            ui_tooltip_clear(&g_app.tooltip->scene);
            application_request_frame();
        }
        return;
    }
    const float scale = 96.f / maximum(g_app.renderer.dpi, 1u);
    const UiPoint point{static_cast<float>(static_cast<short>(LOWORD(lparam))) * scale,
                        static_cast<float>(static_cast<short>(HIWORD(lparam))) * scale};
    UiTooltipTarget candidate{};
    const bool navigation =
        keyboard && (wparam == VK_TAB || wparam == VK_UP || wparam == VK_DOWN || wparam == VK_LEFT ||
                     wparam == VK_RIGHT || wparam == VK_HOME || wparam == VK_END);
    // Retain the first eligible pointer observation even when dispatch has not
    // published its action bar/video control yet. Synchronize resolves the new
    // typed source after dispatch without requiring a second mouse movement.
    const bool first_pointer = pointer && content_available();
    if (!g_app.tooltip && !first_pointer && !resolve_target(point, keyboard, &candidate) &&
        !(navigation && content_available()))
        return;
    auto* owner = ensure();
    if (!owner)
        return;
    owner->keyboard = keyboard;
    owner->pressed = false;
    if (pointer) {
        owner->point = point;
        owner->pointer_valid = true;
        owner->dismissed = {};
    }
    if (keyboard && (wparam == VK_RETURN || wparam == VK_SPACE || wparam == VK_ESCAPE)) {
        owner->dismissed = owner->scene.key;
        ui_tooltip_clear(&owner->scene);
        application_request_frame();
    }
    refresh(*owner, clock_monotonic_milliseconds());
    if (pointer && owner->scene.pending) {
        TRACKMOUSEEVENT tracking{static_cast<DWORD>(sizeof(TRACKMOUSEEVENT)), TME_LEAVE, g_app.window, 0};
        TrackMouseEvent(&tracking);
    }
}
} // namespace maicord::app_detail
