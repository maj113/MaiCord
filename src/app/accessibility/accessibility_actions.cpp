#include "accessibility_internal.hpp"
#include "maicord/core/virtual_memory.hpp"
#if defined(_WIN32)
#include "maicord/core/win32_kernel.hpp"
#endif
#include "../guild/community.hpp"
#include "../shell/content_point.hpp"
#include "../shell/caption.hpp"
#include "../account/profile_editor.hpp"
#include "../chat/report.hpp"
#include "accessibility.hpp"
#if defined(_WIN32)
#include "maicord/core/win32_user.hpp"
#else
// The wheel delta of one notch, as the Win32 headers name it.
#define WHEEL_DELTA 120
#endif
#include "../account/application_account_logout.hpp"
#include "../account/account_presentation.hpp"
#include "../guild/channel_overview.hpp"
#include "../chat/composer/composer.hpp"
#include "../chat/composer/input.hpp"
#include "../shell/content_input.hpp"
#include "../account/custom_status.hpp"
#include "../plugins/settings_dialog.hpp"
#include "../guild/settings/settings.hpp"
#include "../guild/lifecycle.hpp"
#include "../account/login/input.hpp"
#include "../account/login/ui.hpp"
#include "../chat/polls/create.hpp"
#include "../settings/layer.hpp"
#include "maicord/app/account/login/continuations.hpp"
#include "maicord/core/log.hpp"

namespace maicord::app_detail {
namespace {

// Storage layout: observations (current, previous) and the key index are
// committed whole; nodes, slots and text follow, reserved at full capacity.
constexpr usize kObservationBytes = sizeof(UiAccessObservation) * kObservationCapacity * 2;
constexpr usize kKeyBytes = sizeof(u32) * kKeyIndexCapacity;
constexpr usize kNodeOffset = kObservationBytes + kKeyBytes;
constexpr usize kSlotOffset = kNodeOffset + sizeof(UiAccessNode) * kNodeCapacity;
constexpr usize kTextOffset = kSlotOffset + sizeof(u32) * kSlotCapacity;
constexpr usize kStorageBytes = kTextOffset + kTextCapacity;

bool commit_range(u8* base, usize offset, usize bytes) noexcept {
    if (!bytes || pages_commit_at(base + offset, bytes))
        return true;
    log_write(LogLevel::warning, "accessibility", "storage_commit_failed");
    return false;
}

// Grows the regions the last snapshot exhausted; false when nothing can grow.
bool grow_exhausted() noexcept {
    auto& tree = g_access.tree;
    auto* base = static_cast<u8*>(g_access.storage);
    const auto grow = [&](u32 region, u32& count, u32 capacity, usize offset, usize unit) noexcept {
        if (!(tree.exhausted & region) || count >= capacity)
            return false;
        const u32 next = minimum(count * 2, capacity);
        if (!commit_range(base, offset, unit * next))
            return false;
        count = next;
        return true;
    };
    const bool nodes = grow(ui_access_exhausted_nodes, tree.nodes.count, kNodeCapacity, kNodeOffset,
                            sizeof(UiAccessNode));
    const bool slots = grow(ui_access_exhausted_slots, tree.slot_map.count, kSlotCapacity, kSlotOffset,
                            sizeof(u32));
    const bool text = grow(ui_access_exhausted_text, tree.text.count, kTextCapacity, kTextOffset, 1);
    return nodes || slots || text;
}

void build_tree() noexcept {
    g_access.dirty = false;
    // A snapshot that ran out of committed storage is rebuilt after growing,
    // so the published tree matches one built at full capacity.
    do
        build_snapshot();
    while (g_access.tree.exhausted && grow_exhausted());
}

} // namespace
namespace accessibility_detail {

bool ensure_storage() noexcept {
    if (g_access.storage)
        return true;
    auto* base = static_cast<u8*>(pages_reserve(kStorageBytes));
    if (!base)
        return false;
    auto* observations = reinterpret_cast<UiAccessObservation*>(base);
    auto* keys = reinterpret_cast<u32*>(base + kObservationBytes);
    auto* nodes = reinterpret_cast<UiAccessNode*>(base + kNodeOffset);
    auto* slots = reinterpret_cast<u32*>(base + kSlotOffset);
    auto* text = reinterpret_cast<char*>(base + kTextOffset);
    if (!commit_range(base, 0, kNodeOffset) ||
        !commit_range(base, kNodeOffset, sizeof(UiAccessNode) * kInitialNodeCapacity) ||
        !commit_range(base, kSlotOffset, sizeof(u32) * kInitialSlotCapacity) ||
        !commit_range(base, kTextOffset, kInitialTextCapacity) ||
        !ui_access_tree_initialize(&g_access.tree, {nodes, kInitialNodeCapacity}, {text, kInitialTextCapacity},
                                   {slots, kInitialSlotCapacity}, {keys, kKeyIndexCapacity}) ||
        !ui_access_event_state_initialize(&g_access.events, {observations, kObservationCapacity},
                                          {observations + kObservationCapacity, kObservationCapacity})) {
        pages_release(base);
        return false;
    }
    g_access.storage = base;
    g_access.dirty = true;
    return true;
}

const UiAccessTree* access_tree(void*) noexcept {
    if (!ensure_storage())
        return nullptr;
    if (g_access.dirty || !g_access.tree.finished)
        build_tree();
    return g_access.tree.finished ? &g_access.tree : nullptr;
}

} // namespace accessibility_detail
namespace {

// Value.SetValue and text selection reach only editors with an app edit path.
bool editable_by_automation(const UiAccessNode& node, UiAccessAction action) noexcept {
    if (node.role != UiAccessRole::edit && node.role != UiAccessRole::combo_box)
        return false;
    switch (node.surface) {
    case surface_composer:
        return true;
    case surface_settings:
        return node.item != 0; // inline page editors (not the settings search)
    case surface_login:
        return action == UiAccessAction::set_value;
    default:
        return false;
    }
}

} // namespace
namespace accessibility_detail {

bool access_act(void*, u64 key, UiAccessAction action, double number, Utf8View text) noexcept {
    if (g_access.shut_down || !g_app.window)
        return false;
    if (action == UiAccessAction::set_value || action == UiAccessAction::select_text) {
        const u32 index = g_access.tree.finished ? ui_access_find(g_access.tree, key) : kUiAccessNone;
        if (index == kUiAccessNone || !editable_by_automation(g_access.tree.nodes[index], action))
            return false;
    }
    if (action == UiAccessAction::set_value) {
        const u32 bytes = minimum<u32>(text.bytes, static_cast<u32>(sizeof(g_access.pending_text)));
        copy_bytes(g_access.pending_text, text.data, bytes);
        g_access.pending_text_bytes = bytes;
    }
    if (g_access.pending_count >= kPendingCapacity)
        return false;
    g_access.pending[g_access.pending_count++] = {key, number, action};
    if (!g_access.posted)
        g_access.posted = window_post({window_surface_native(g_app.window), wake_accessibility}, 0, 0);
    return true;
}

} // namespace accessibility_detail
namespace {

// Activates through the same pointer route a mouse click takes.
// Injected pointer events carry client coordinates: content rows start below the caption.
bool click_at(float x, float y) noexcept {
    if (!g_app.window)
        return false;
    const float scale = access_scale(nullptr);
    const float caption = static_cast<float>(g_app.renderer.caption_height);
    const float client_x = static_cast<float>(static_cast<int>(x * scale));
    const float client_y = static_cast<float>(static_cast<int>(y * scale)) + caption;
    WindowEvent move{};
    move.type = EventType::pointer_move;
    move.pointer.x = client_x;
    move.pointer.y = client_y;
    WindowEvent press = move;
    press.type = EventType::pointer_down;
    press.button = static_cast<u8>(PointerButton::left);
    press.pointer.buttons = static_cast<u8>(1u << static_cast<u8>(PointerButton::left));
    WindowEvent release = press;
    release.type = EventType::pointer_up;
    release.pointer.buttons = 0;
    window_inject(g_app.window, move, nullptr);
    window_inject(g_app.window, press, nullptr);
    window_inject(g_app.window, release, nullptr);
    // The pointer where it really is again, so hover follows it after the activation moved things.
    float cursor_x = 0, cursor_y = 0;
    const WindowMetrics metrics = window_metrics(g_app.window);
    if (window_pointer_position(g_app.window, &cursor_x, &cursor_y) && cursor_x >= 0 && cursor_y >= caption &&
        cursor_x < static_cast<float>(metrics.width_px) && cursor_y < static_cast<float>(metrics.height_px)) {
        WindowEvent back{};
        back.type = EventType::pointer_move;
        back.pointer.x = static_cast<float>(static_cast<int>(cursor_x));
        back.pointer.y = static_cast<float>(static_cast<int>(cursor_y));
        window_inject(g_app.window, back, nullptr);
    }
    application_request_frame();
    return true;
}

bool click_node(const UiAccessNode& node) noexcept {
    if ((node.state & ui_access_offscreen) || !ui_rect_has_area(node.visible))
        return false;
    const UiPoint point = center(node.visible);
    return click_at(point.x, point.y);
}

template <typename T> T* mutable_scene(const T* scene) noexcept {
    return const_cast<T*>(scene); // renderer bindings borrow scenes owned mutably by their controllers
}

// Scroll offset that brings [top, top + height) inside a viewport of extent.
float reveal_offset(float scroll, float top, float height, float extent) noexcept {
    if (top < scroll)
        return top;
    if (top + height > scroll + extent)
        return top + height - extent;
    return scroll;
}

// ScrollItem.ScrollIntoView: each list scrolls through its own reveal routine.
bool reveal_item(const UiAccessNode& node) noexcept {
    const u64 item = node.item;
    if (!item)
        return false;
    auto& renderer = g_app.renderer;
    const auto* view = g_app.account_ui ? &g_app.account_ui->navigation_view : nullptr;
    switch (node.surface) {
    case surface_guilds:
        return view && guild_rail_scene_reveal_guild(&g_app.guild_rail_scene, *view, item);
    case surface_channels:
        return view && shell_navigation_scene_reveal_channel(&g_app.shell_navigation_scene, *view, item);
    case surface_messages:
        return message_scene_reveal_message(&g_app.message_scene, item);
    case surface_members:
        return member_list_scene_reveal_user(&g_app.member_list_scene, item);
    case surface_settings: {
        auto* scene = mutable_scene(renderer.settings_scene);
        return scene && ui_settings_scene_reveal(scene, {UiSettingsTargetKind::control, scene->selected_page, item});
    }
    case surface_quick: {
        auto* scene = mutable_scene(renderer.quick_switcher_scene);
        if (!scene || item - 1 >= scene->accepted_count)
            return false;
        const float top = static_cast<float>(item - 1) * kUiQuickSwitcherRowHeight;
        return ui_quick_switcher_scene_scroll(
            scene, reveal_offset(scene->scroll_dips, top, kUiQuickSwitcherRowHeight, ui_rect_height(scene->body)));
    }
    case surface_search: {
        auto* scene = mutable_scene(renderer.search_scene);
        const u64 index = item - 1;
        if (!scene || scene->layout != UiSearchLayout::list || !scene->source || index >= scene->source->count ||
            index + 1 >= sizeof(scene->row_offsets) / sizeof(scene->row_offsets[0]))
            return false;
        const float top = scene->row_offsets[index], bottom = scene->row_offsets[index + 1];
        return ui_search_scene_scroll(
            scene, reveal_offset(scene->scroll_dips, top, bottom - top, ui_rect_height(scene->body)));
    }
    case surface_pins: {
        auto* scene = mutable_scene(renderer.pinned_messages_scene);
        return scene && scene->content &&
               ui_pinned_messages_scene_focus(
                   scene, {UiPinnedMessagesAction::jump, scene->content->channel_id, item});
    }
    case surface_threads: {
        auto* scene = renderer.thread_browser_scene;
        return scene && ui_thread_browser_scene_focus(
                            scene, {UiThreadBrowserAction::open_thread, item, scene->source_publication, {}});
    }
    case surface_inbox: {
        auto* scene = mutable_scene(renderer.inbox_scene);
        if (!scene)
            return false;
        for (u32 slot = 0; slot < scene->count && slot < kUiInboxVisibleRows; ++slot)
            if (scene->first + slot + 1 == item && scene->rows[slot].generation &&
                scene->rows[slot].slot < kUiInboxWidgetCapacity)
                return ui_inbox_scene_focus(scene, scene->targets[scene->rows[slot].slot]);
        return false;
    }
    case surface_picker: {
        auto* scene = mutable_scene(renderer.media_picker_scene);
        return scene && ui_media_picker_scene_select(scene, static_cast<u32>(item - 1));
    }
    default:
        return false;
    }
}

bool account_surface(u8 surface) noexcept {
    return surface == surface_guilds || surface == surface_channels || surface == surface_messages ||
           surface == surface_members;
}

// Scrolls the node's item into view and republishes the frame.
bool reveal_node(const UiAccessNode& node) noexcept {
    if (!reveal_item(node))
        return false;
    if (account_surface(node.surface) && !refresh_account_frame(true))
        log_write(LogLevel::warning, "accessibility", "reveal_refresh_failed");
    application_request_frame();
    return true;
}

// Reveals the node, republishes the frame and returns the node at its new place.
bool reveal_and_resolve(UiAccessNode* node) noexcept {
    if (!reveal_node(*node))
        return false;
    g_access.dirty = true;
    const UiAccessTree* tree = access_tree(nullptr);
    const u32 index = tree ? ui_access_find(*tree, node->key) : kUiAccessNone;
    if (index == kUiAccessNone)
        return false;
    *node = tree->nodes[index];
    return true;
}

SettingsLayer* active_settings_layer() noexcept {
    SettingsLayer* const layers[]{application_channel_overview_settings_layer(),
                                  application_custom_status_settings_layer(),
                                  application_plugin_settings_settings_layer(),
                                  application_guild_settings_settings_layer(),
                                  application_guild_lifecycle_settings_layer(),
                                  application_poll_create_settings_layer(),
                                  application_guild_community_settings_layer(),
                                  application_profile_editor_settings_layer(),
                                  application_report_settings_layer()};
    for (auto* layer : layers)
        if (layer && layer->scene && layer->scene == g_app.renderer.settings_scene && layer->scene->active)
            return layer;
    return nullptr;
}

// Focuses the editor through the pointer path, then replaces its text.
bool set_settings_text(const UiAccessNode& node, Utf8View text) noexcept {
    SettingsLayer* layer = active_settings_layer();
    if (!layer)
        return false;
    u64 control = 0;
    MessageComposer* model = nullptr;
    BoundedEdit* binding = nullptr;
    if (!settings_layer_editor(layer, &control, &model, &binding) || control != node.item) {
        click_node(node);
        if (!settings_layer_editor(layer, &control, &model, &binding) || control != node.item)
            return false;
    }
    return settings_layer_replace_text(layer, text);
}

bool select_settings_text(const UiAccessNode& node, u32 anchor, u32 caret) noexcept {
    SettingsLayer* layer = active_settings_layer();
    u64 control = 0;
    MessageComposer* model = nullptr;
    BoundedEdit* binding = nullptr;
    if (!layer || !settings_layer_editor(layer, &control, &model, &binding) || control != node.item)
        return false;
    message_composer_set_selection(model, anchor, caret);
    return settings_layer_edit_changed(layer, false);
}

// Login fields: the field is focused through the pointer path first; the
// identifier keeps the paste path's rules (no links, recovery validation).
bool set_login_text(const UiAccessNode& node, Utf8View text) noexcept {
    auto& login = g_app.login;
    if (login_phone_mode(login.status))
        return false;
    click_node(node);
    SecureEdit* edit = application_active_login_edit();
    if (!edit)
        return false;
    const bool identifier = edit == &login.login;
    if (identifier && (text.bytes > kUserAuthMaximumLoginBytes || login_verification_link_candidate(text) ||
                       login_reset_link_candidate(text) ||
                       (login_recovery_mode(login.status) && text.bytes && !user_auth_recovery_identifier_valid(text))))
        return false;
    SecureEditResult result = SecureEditResult::unchanged;
    if (text.bytes) {
        secure_edit_select_all(edit);
        result = secure_edit_insert_utf8(edit, text);
    } else {
        result = secure_edit_clear(edit);
    }
    if (identifier) {
        if (result == SecureEditResult::changed)
            edit->password = false;
        application_login_prepare_identifier();
    } else if (edit == &login.mfa && result == SecureEditResult::changed &&
               login.status == RendererLoginStatus::mfa_input_invalid) {
        login.status = RendererLoginStatus::mfa_required;
    }
    return result != SecureEditResult::changed || application_refresh_login_view();
}

bool select_composer_text(u32 anchor, u32 caret) noexcept {
    if (!g_app.account_ui || !g_app.message_composer_scene.active)
        return false;
    return composer_apply_result(message_composer_set_selection(&g_app.message_composer, anchor, caret), false);
}

// Overlay rows that select without activating.
bool select_overlay_item(const UiAccessNode& node) noexcept {
    switch (node.surface) {
    case surface_quick: {
        auto* scene = mutable_scene(g_app.renderer.quick_switcher_scene);
        const bool selected = scene && node.item && ui_quick_switcher_scene_select(scene, static_cast<u32>(node.item - 1));
        application_request_frame();
        return selected;
    }
    case surface_picker:
    case surface_pins:
    case surface_threads:
    case surface_inbox:
        if (node.role != UiAccessRole::list_item)
            return false;
        reveal_item(node);
        application_request_frame();
        return true;
    default:
        return false;
    }
}

bool wheel_node(const UiAccessNode& node, UiAccessScroll amount) noexcept {
    if (!ui_rect_has_area(node.visible) || amount == UiAccessScroll::none)
        return false;
    const bool up = amount == UiAccessScroll::small_decrement || amount == UiAccessScroll::large_decrement;
    const bool large = amount == UiAccessScroll::large_decrement || amount == UiAccessScroll::large_increment;
    const int delta = (large ? 5 : 1) * (up ? WHEEL_DELTA : -WHEEL_DELTA);
    const UiPoint point = center(node.visible);
    const float scale = access_scale(nullptr);
    WindowEvent wheel{};
    wheel.type = EventType::wheel;
    wheel.wheel.x = static_cast<float>(static_cast<LONG>(point.x * scale));
    wheel.wheel.y = static_cast<float>(static_cast<LONG>(point.y * scale) + static_cast<LONG>(g_app.renderer.caption_height));
    wheel.wheel.dy120 = delta;
    window_inject(g_app.window, wheel, nullptr);
    application_request_frame();
    return true;
}

void focus_owner(UiWidgetManager* owner) noexcept {
    ui_button_keyboard_cancel(&g_app.button_keyboard);
    if (owner != &g_app.message_composer_scene.widgets && g_app.message_composer.focused)
        release_message_composer_focus();
    g_app.context_focus = owner;
    g_app.keyboard_focus_visible = true;
    window_focus(g_app.window);
    if (!refresh_account_frame(true))
        log_write(LogLevel::warning, "accessibility", "focus_refresh_failed");
}

bool focus_item(u8 surface, u64 item) noexcept {
    if (!g_app.account_ui || !item)
        return false;
    const auto& view = g_app.account_ui->navigation_view;
    switch (surface) {
    case surface_guilds:
        if (!guild_rail_scene_reveal_guild(&g_app.guild_rail_scene, view, item))
            return false;
        focus_owner(&g_app.guild_rail_scene.widgets);
        return true;
    case surface_channels:
        if (!shell_navigation_scene_reveal_channel(&g_app.shell_navigation_scene, view, item))
            return false;
        focus_owner(&g_app.shell_navigation_scene.widgets);
        return true;
    case surface_messages:
        if (!message_scene_reveal_message(&g_app.message_scene, item) &&
            !message_scene_focus_message(&g_app.message_scene, item))
            return false;
        focus_owner(&g_app.message_scene.widgets);
        return true;
    case surface_members:
        if (!member_list_scene_reveal_user(&g_app.member_list_scene, item))
            return false;
        focus_owner(&g_app.member_list_scene.widgets);
        return true;
    default:
        return false;
    }
}

bool set_composer_text(Utf8View text) noexcept {
    if (!g_app.account_ui || !g_app.message_composer_scene.active)
        return false;
    const Utf8View current = message_composer_text(g_app.message_composer);
    return composer_apply_result(message_composer_replace_range(&g_app.message_composer, 0, current.bytes, text), true);
}

bool set_settings_slider(const UiAccessNode& node, double value) noexcept {
    const auto* scene = g_app.renderer.settings_scene;
    if (!scene || !ui_rect_has_area(node.visible) || node.range.maximum <= node.range.minimum)
        return false;
    const UiPoint point = center(node.visible);
    const UiSettingsTarget target = ui_settings_scene_hit_test(*scene, point.x, point.y);
    UiSettingsSliderGeometry geometry{};
    if (!ui_settings_scene_slider_geometry(*scene, target, &geometry) || !ui_rect_has_area(geometry.track))
        return false;
    const float fraction = static_cast<float>((value - node.range.minimum) / (node.range.maximum - node.range.minimum));
    const float x = geometry.track.left + fraction * ui_rect_width(geometry.track);
    return click_at(minimum(geometry.track.right - 0.5f, maximum(geometry.track.left + 0.5f, x)),
                    (geometry.track.top + geometry.track.bottom) * 0.5f);
}

} // namespace
namespace accessibility_detail {

void run_action(const PendingAction& pending) noexcept {
    const UiAccessTree* tree = access_tree(nullptr);
    if (!tree)
        return;
    const u32 index = ui_access_find(*tree, pending.key);
    if (index == kUiAccessNone)
        return;
    UiAccessNode node = tree->nodes[index];
    const bool locked = application_account_logout_blocked() || application_account_logout_host_owned();
    const bool row = node.role == UiAccessRole::list_item;
    const bool edit = node.role == UiAccessRole::edit || node.role == UiAccessRole::combo_box;
    const Utf8View text{g_access.pending_text, g_access.pending_text_bytes};
    switch (pending.action) {
    case UiAccessAction::focus:
        if (!locked && row && focus_item(node.surface, node.item))
            return;
        if (node.surface == surface_composer && node.role == UiAccessRole::edit && !locked) {
            if (focus_message_composer_control(MessageComposerControl::edit))
                focus_owner(&g_app.message_composer_scene.widgets);
            return;
        }
        if (!locked && row && select_overlay_item(node))
            return;
        if (edit) {
            if (ui_access_needs_reveal(node) && !reveal_and_resolve(&node))
                return;
            click_node(node);
        }
        return;
    case UiAccessAction::select:
    case UiAccessAction::invoke:
    case UiAccessAction::toggle:
        if (node.surface == surface_caption) {
            application_caption_command(static_cast<UiCaptionPart>(node.item));
            return;
        }
        if (!locked && row && (node.surface == surface_guilds || node.surface == surface_channels) &&
            pending.action != UiAccessAction::toggle && !(node.state & (ui_access_expanded | ui_access_collapsed))) {
            if (focus_item(node.surface, node.item) && !activate_focused_navigation())
                log_write(LogLevel::warning, "accessibility", "navigation_activation_failed");
            return;
        }
        if (!locked && row && node.surface == surface_messages) {
            focus_item(node.surface, node.item);
            return;
        }
        if (!locked && pending.action == UiAccessAction::select && select_overlay_item(node))
            return;
        // Offscreen (or mostly clipped) items scroll into view before the click.
        if (ui_access_needs_reveal(node) &&
            (!(node.actions & ui_access_action_scroll_item) || !reveal_and_resolve(&node)))
            return;
        click_node(node);
        return;
    case UiAccessAction::expand:
    case UiAccessAction::collapse: {
        if (node.surface == surface_quick)
            return; // the switcher's list is open for as long as the switcher
        const bool expanded = (node.state & ui_access_expanded) != 0;
        if (expanded != (pending.action == UiAccessAction::expand))
            click_node(node);
        return;
    }
    case UiAccessAction::scroll:
        wheel_node(node, static_cast<UiAccessScroll>(static_cast<u32>(pending.number)));
        return;
    case UiAccessAction::scroll_into_view:
        reveal_node(node);
        return;
    case UiAccessAction::set_value:
        if (locked)
            return;
        if (node.surface == surface_composer && node.role == UiAccessRole::edit)
            set_composer_text(text);
        else if (node.surface == surface_settings)
            set_settings_text(node, text);
        else if (node.surface == surface_login)
            set_login_text(node, text);
        return;
    case UiAccessAction::select_text: {
        u32 anchor = 0, caret = 0;
        ui_access_text_selection_unpack(pending.number, &anchor, &caret);
        if (locked)
            return;
        if (node.surface == surface_composer)
            select_composer_text(anchor, caret);
        else if (node.surface == surface_settings)
            select_settings_text(node, anchor, caret);
        return;
    }
    case UiAccessAction::set_range:
        if (node.surface == surface_settings)
            set_settings_slider(node, pending.number);
        return;
    default:
        return;
    }
}

} // namespace accessibility_detail
} // namespace maicord::app_detail
