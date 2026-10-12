#include "maicord/ui/scene.hpp"

namespace maicord {
namespace {

constexpr u32 semantic_id(UiSemantic semantic) noexcept {
    return static_cast<u32>(semantic) + 1;
}

Utf8View semantic_name(UiSemantic semantic) noexcept {
    switch (semantic) {
    case UiSemantic::root:
        return utf8_literal("Shell");
    case UiSemantic::guild_rail:
        return utf8_literal("GuildRail");
    case UiSemantic::home_button:
        return utf8_literal("Home");
    case UiSemantic::guild_navigation:
        return utf8_literal("GuildNavigation");
    case UiSemantic::channel_sidebar:
        return utf8_literal("ChannelSidebar");
    case UiSemantic::channel_header:
        return utf8_literal("ChannelHeader");
    case UiSemantic::channel_navigation:
        return utf8_literal("ChannelNavigation");
    case UiSemantic::channel_footer:
        return utf8_literal("ChannelFooter");
    case UiSemantic::content:
        return utf8_literal("Content");
    case UiSemantic::title_bar:
        return utf8_literal("TitleBar");
    case UiSemantic::title_label:
        return utf8_literal("TitleLabel");
    case UiSemantic::member_toggle:
        return utf8_literal("MemberToggle");
    case UiSemantic::pins_toggle:
        return utf8_literal("PinnedMessages");
    case UiSemantic::threads_toggle:
        return utf8_literal("Threads");
    case UiSemantic::thread_header:
        return utf8_literal("ThreadHeader");
    case UiSemantic::thread_status:
        return utf8_literal("ThreadStatus");
    case UiSemantic::thread_primary:
        return utf8_literal("ThreadPrimaryAction");
    case UiSemantic::thread_overflow:
        return utf8_literal("ThreadActions");
    case UiSemantic::thread_feedback:
        return utf8_literal("ThreadFeedback");
    case UiSemantic::thread_cancel:
        return utf8_literal("ThreadCancel");
    case UiSemantic::settings:
        return utf8_literal("UserSettings");
    case UiSemantic::friends_button:
        return utf8_literal("Friends");
    case UiSemantic::message_view:
        return utf8_literal("MessageView");
    case UiSemantic::composer:
        return utf8_literal("Composer");
    case UiSemantic::member_sidebar:
        return utf8_literal("MemberSidebar");
    case UiSemantic::private_create:
        return utf8_literal("CreateDirectMessage");
    case UiSemantic::private_add:
        return utf8_literal("AddPeople");
    case UiSemantic::private_call:
        return utf8_literal("StartCall");
    case UiSemantic::inbox_toggle:
        return utf8_literal("Inbox");
    case UiSemantic::guild_menu:
        return utf8_literal("ServerMenu");
    case UiSemantic::count:
        return Utf8View{};
    }
    return Utf8View{};
}

UiWidgetKind semantic_kind(UiSemantic semantic) noexcept {
    if (semantic == UiSemantic::member_toggle || semantic == UiSemantic::pins_toggle ||
        semantic == UiSemantic::threads_toggle || semantic == UiSemantic::home_button ||
        semantic == UiSemantic::channel_footer || semantic == UiSemantic::settings ||
        semantic == UiSemantic::friends_button || semantic == UiSemantic::thread_primary ||
        semantic == UiSemantic::thread_overflow || semantic == UiSemantic::thread_cancel ||
        semantic == UiSemantic::private_create || semantic == UiSemantic::private_add ||
        semantic == UiSemantic::private_call || semantic == UiSemantic::inbox_toggle ||
        semantic == UiSemantic::guild_menu) {
        return UiWidgetKind::button;
    }
    return semantic == UiSemantic::channel_navigation || semantic == UiSemantic::message_view ||
                   semantic == UiSemantic::member_sidebar
               ? UiWidgetKind::scroll_view
               : UiWidgetKind::container;
}

bool add_node(UiScene* scene, UiSemantic semantic, UiWidgetHandle parent, UiLayoutStyle style) noexcept {
    const u16 flags =
        semantic == UiSemantic::member_toggle || semantic == UiSemantic::pins_toggle ||
                semantic == UiSemantic::threads_toggle || semantic == UiSemantic::home_button ||
                semantic == UiSemantic::channel_footer || semantic == UiSemantic::settings ||
                semantic == UiSemantic::friends_button || semantic == UiSemantic::thread_primary ||
                semantic == UiSemantic::thread_overflow || semantic == UiSemantic::thread_cancel ||
                semantic == UiSemantic::private_create || semantic == UiSemantic::private_add ||
                semantic == UiSemantic::private_call || semantic == UiSemantic::inbox_toggle ||
                semantic == UiSemantic::guild_menu
            ? static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable)
            : ui_widget_enabled;
    // Thread header controls are driven by the thread header owner.
    const u16 admitted_flags =
        semantic >= UiSemantic::thread_header && semantic <= UiSemantic::thread_cancel ? ui_widget_none : flags;
    const u32 id = semantic_id(semantic);
    const UiWidgetResult result = ui_widget_manager_add(
        &scene->widgets, parent,
        UiWidgetDesc{id, semantic_kind(semantic), admitted_flags,
                     UiLayoutNodeSpec{semantic_name(semantic), style, UiSize{}, 0.0f, id}});
    if (!result.ok()) {
        return false;
    }
    scene->semantics[static_cast<u32>(semantic)] = result.handle;
    return true;
}

UiLayoutStyle fixed_width(float width) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fixed(width);
    style.height = ui_layout_fill();
    style.vertical_alignment = UiLayoutAlignment::stretch;
    return style;
}

UiLayoutStyle fixed_height(float height) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fixed(height);
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    return style;
}

bool shell_flag(const UiShellLayoutMetrics& metrics, UiShellLayoutFlags flag) noexcept {
    return (metrics.flags & static_cast<u16>(flag)) != 0;
}

bool shell_metrics_valid(const UiShellLayoutMetrics& metrics) noexcept {
    constexpr u16 known_flags =
        static_cast<u16>(ui_shell_layout_navigation | ui_shell_layout_members | ui_shell_layout_composer |
                         ui_shell_layout_pins | ui_shell_layout_threads | ui_shell_layout_private);
    const bool navigation = shell_flag(metrics, ui_shell_layout_navigation);
    const bool members = shell_flag(metrics, ui_shell_layout_members);
    const bool composer = shell_flag(metrics, ui_shell_layout_composer);
    return metrics.revision != 0 && (metrics.flags & ~known_flags) == 0 &&
           ui_extent_valid(metrics.guild_rail_width) && metrics.guild_rail_width > 0.0f &&
           ui_extent_valid(metrics.navigation_width) && (!navigation || metrics.navigation_width > 0.0f) &&
           ui_extent_valid(metrics.member_sidebar_width) &&
           (!members || metrics.member_sidebar_width > 0.0f) &&
           ui_extent_valid(metrics.navigation_min_viewport_width) &&
           ui_extent_valid(metrics.member_min_viewport_width) &&
           ui_extent_valid(metrics.navigation_gutter_before) &&
           ui_extent_valid(metrics.content_gutter_before) && ui_extent_valid(metrics.title_bar_height) &&
           metrics.title_bar_height > 0.0f && ui_extent_valid(metrics.navigation_header_height) &&
           (!navigation || metrics.navigation_header_height > 0.0f) &&
           ui_extent_valid(metrics.navigation_footer_height) &&
           (!navigation || metrics.navigation_footer_height > 0.0f) &&
           ui_extent_valid(metrics.member_sidebar_top) && ui_extent_valid(metrics.message_bottom_margin) &&
           ui_extent_valid(metrics.composer_height) && (!composer || metrics.composer_height > 0.0f) &&
           ui_insets_valid(metrics.composer_margin) && ui_insets_valid(metrics.navigation_header_padding) &&
           ui_insets_valid(metrics.title_bar_padding) && ui_extent_valid(metrics.navigation_footer_reserve);
}

float shell_content_left(const UiShellLayoutMetrics& metrics, bool navigation) noexcept {
    return metrics.guild_rail_width + metrics.content_gutter_before +
           (navigation ? metrics.navigation_width + metrics.navigation_gutter_before : 0.f);
}

u8 shell_variant(const UiShellLayoutMetrics& metrics, float width) noexcept {
    const float content_left = shell_content_left(metrics, shell_flag(metrics, ui_shell_layout_navigation));
    // Keep the requested pane width and preference. A narrow transition can
    // arrive without a native resize (for example, restoring the member pane).
    const bool members =
        shell_flag(metrics, ui_shell_layout_members) &&
        width - content_left - metrics.member_sidebar_width >= kUiShellMinimumMessageWidthDips;
    const float header_width = maximum(0.f, width - content_left - metrics.title_bar_padding.left -
                                                metrics.title_bar_padding.right);
    const float existing_controls = (metrics.member_sidebar_width > 0.f ? 52.f : 8.f) +
                                    (shell_flag(metrics, ui_shell_layout_pins) ? 44.f : 0.f);
    const bool threads =
        shell_flag(metrics, ui_shell_layout_threads) && header_width >= existing_controls + 44.f;
    // The inbox joins only while a readable label (72) and, in conversation
    // views, the search field (120) still fit; Friends has no search.
    const float placed = existing_controls + (threads ? 44.f : 0.f) +
                         (shell_flag(metrics, ui_shell_layout_private) ? 88.f : 0.f);
    const float search = shell_flag(metrics, ui_shell_layout_composer) || metrics.member_sidebar_width > 0.f ? 120.f : 0.f;
    const bool inbox = header_width >= placed + 40.f + 72.f + search;
    return static_cast<u8>((shell_flag(metrics, ui_shell_layout_navigation) ? 1u : 0u) |
                           (members ? 2u : 0u) | (shell_flag(metrics, ui_shell_layout_composer) ? 4u : 0u) |
                           (shell_flag(metrics, ui_shell_layout_pins) ? 8u : 0u) | (threads ? 16u : 0u) |
                           (shell_flag(metrics, ui_shell_layout_private) ? 32u : 0u) | (inbox ? 64u : 0u));
}

bool project_shell(UiScene* scene) noexcept {
    scene->count = scene->widgets.count;
    scene->generation = scene->widgets.generation;
    for (u32 index = 0; index < scene->count; ++index) {
        const UiWidget& widget = scene->storage.widgets[index];
        const UiLayoutNode* layout = ui_layout_engine_resolve(scene->widgets.layout, widget.layout);
        if (!layout || widget.id == 0 || widget.id > static_cast<u32>(UiSemantic::count)) {
            return false;
        }
        UiNode& node = scene->nodes[index];
        node = UiNode{};
        node.rectangle = layout->rectangle;
        node.widget = UiWidgetHandle{static_cast<u16>(index), widget.generation};
        node.semantic = static_cast<UiSemantic>(widget.id - 1);
        node.generation = widget.generation;
        node.visible = layout->visible;
    }
    return true;
}

float header_search_width(float content_width, float controls_width) noexcept {
    // Reserve a readable channel label before growing the trailing search.
    // The compact field stays at least 120 DIP while space permits all controls.
    const float available = maximum(0.f, content_width - controls_width);
    return minimum(available, minimum(280.f, maximum(120.f, available - 100.f)));
}

void update_header_controls(UiScene* scene, const UiShellLayoutMetrics& metrics, float width,
                            u8 variant) noexcept {
    const float left = shell_content_left(metrics, (variant & 1u) != 0);
    const auto label = scene->semantics[static_cast<u32>(UiSemantic::title_label)];
    const auto toggle = scene->semantics[static_cast<u32>(UiSemantic::member_toggle)];
    const auto pins = scene->semantics[static_cast<u32>(UiSemantic::pins_toggle)];
    const auto threads = scene->semantics[static_cast<u32>(UiSemantic::threads_toggle)];
    const auto add = scene->semantics[static_cast<u32>(UiSemantic::private_add)];
    const auto call = scene->semantics[static_cast<u32>(UiSemantic::private_call)];
    const float existing_width = (toggle.generation ? 52.f : 8.f) + (pins.generation ? 44.f : 0.f) +
                                 (threads.generation ? 44.f : 0.f);
    const float controls_width = existing_width + (add.generation ? 44.f : 0.f) + (call.generation ? 44.f : 0.f);
    // The inbox sits after the search field at the trailing edge; every margin
    // measured from the field's left edge includes it.
    const float trailing = scene->semantics[static_cast<u32>(UiSemantic::inbox_toggle)].generation ? 40.f : 0.f;
    const float search_width =
        header_search_width(
            maximum(0.f, width - left - metrics.title_bar_padding.left - metrics.title_bar_padding.right),
            controls_width + trailing) +
        trailing;
    if (auto* node =
            ui_layout_engine_resolve(&scene->widgets.layout, scene->widgets.storage[label.slot].layout)) {
        // Reserve search plus the original icon button and its eight-DIP gaps.
        node->spec.style.margin.right = search_width + controls_width;
    }
    if (toggle.generation)
        if (auto* node = ui_layout_engine_resolve(&scene->widgets.layout,
                                                  scene->widgets.storage[toggle.slot].layout))
            node->spec.style.margin.right = search_width + 8.f;
    if (pins.generation)
        if (auto* node =
                ui_layout_engine_resolve(&scene->widgets.layout, scene->widgets.storage[pins.slot].layout))
            node->spec.style.margin.right = search_width + (toggle.generation ? 52.f : 8.f);
    if (threads.generation)
        if (auto* node = ui_layout_engine_resolve(&scene->widgets.layout,
                                                  scene->widgets.storage[threads.slot].layout))
            node->spec.style.margin.right =
                search_width + (toggle.generation ? 52.f : 8.f) + (pins.generation ? 44.f : 0.f);
    if (add.generation)
        if (auto* node =
                ui_layout_engine_resolve(&scene->widgets.layout, scene->widgets.storage[add.slot].layout))
            node->spec.style.margin.right = search_width + existing_width;
    if (call.generation)
        if (auto* node =
                ui_layout_engine_resolve(&scene->widgets.layout, scene->widgets.storage[call.slot].layout))
            node->spec.style.margin.right = search_width + existing_width + (add.generation ? 44.f : 0.f);
}

bool build_shell(UiScene* scene, UiShellView view, const UiShellLayoutMetrics& metrics, u8 variant,
                 float width, float height) noexcept {
    if (!scene->widgets.storage.data) {
        if (!scene->storage.widgets.data || !scene->storage.layout_nodes.data || !scene->nodes ||
            !ui_widget_manager_initialize(&scene->widgets, scene->storage.widgets,
                                          scene->storage.layout_nodes)) {
            return false;
        }
    } else {
        ui_widget_manager_reset(&scene->widgets);
    }
    for (u32 index = 0; index < static_cast<u32>(UiSemantic::count); ++index) {
        scene->semantics[index] = ui_widget_invalid_handle();
    }

    UiLayoutStyle root_style = ui_layout_style();
    root_style.flow = UiLayoutFlow::overlay;
    root_style.width = ui_layout_fill();
    root_style.height = ui_layout_fill();
    root_style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children);
    if (!add_node(scene, UiSemantic::root, ui_widget_invalid_handle(), root_style)) {
        return false;
    }
    const UiWidgetHandle root = scene->semantics[static_cast<u32>(UiSemantic::root)];
    UiLayoutStyle rail_style = fixed_width(metrics.guild_rail_width);
    rail_style.flow = UiLayoutFlow::vertical;
    if (!add_node(scene, UiSemantic::guild_rail, root, rail_style)) {
        return false;
    }
    const UiWidgetHandle rail = scene->semantics[static_cast<u32>(UiSemantic::guild_rail)];
    UiLayoutStyle home_style = fixed_height(metrics.title_bar_height);
    home_style.margin = UiInsets{8.0f, 8.0f, 8.0f, 0.0f};
    home_style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children |
                                        ui_layout_node_text_content);
    UiLayoutStyle guild_navigation_style = ui_layout_style();
    guild_navigation_style.width = ui_layout_fill();
    guild_navigation_style.height = ui_layout_fill();
    guild_navigation_style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children);
    if (!add_node(scene, UiSemantic::home_button, rail, home_style) ||
        !add_node(scene, UiSemantic::guild_navigation, rail, guild_navigation_style))
        return false;

    UiWidgetHandle channels = ui_widget_invalid_handle();
    if ((variant & 1u) != 0) {
        UiLayoutStyle channels_style = fixed_width(metrics.navigation_width);
        channels_style.flow = UiLayoutFlow::vertical;
        channels_style.margin.left = metrics.guild_rail_width + metrics.navigation_gutter_before;
        if (!add_node(scene, UiSemantic::channel_sidebar, root, channels_style)) {
            return false;
        }
        channels = scene->semantics[static_cast<u32>(UiSemantic::channel_sidebar)];
    }

    UiLayoutStyle content_style = ui_layout_style();
    content_style.flow = UiLayoutFlow::vertical;
    content_style.width = ui_layout_fill();
    content_style.height = ui_layout_fill();
    const float content_left = shell_content_left(metrics, (variant & 1u) != 0);
    content_style.margin.left = content_left;
    content_style.margin.right = (variant & 2u) ? metrics.member_sidebar_width : 0.f;
    content_style.horizontal_alignment = UiLayoutAlignment::stretch;
    content_style.vertical_alignment = UiLayoutAlignment::stretch;
    if (!add_node(scene, UiSemantic::content, root, content_style)) {
        return false;
    }
    const UiWidgetHandle content = scene->semantics[static_cast<u32>(UiSemantic::content)];

    if ((variant & 2u) != 0) {
        UiLayoutStyle members_style = fixed_width(metrics.member_sidebar_width);
        members_style.horizontal_alignment = UiLayoutAlignment::end;
        members_style.margin.top = metrics.member_sidebar_top;
        members_style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children |
                                               ui_layout_node_scroll_vertical);
        if (!add_node(scene, UiSemantic::member_sidebar, root, members_style)) {
            return false;
        }
    }

    if (channels.generation != 0) {
        UiLayoutStyle header_style = fixed_height(metrics.navigation_header_height);
        header_style.padding = metrics.navigation_header_padding;
        const bool guild_header = view == UiShellView::guild_channel;
        if (guild_header) {
            // The official header pads 8 DIP around a 32-DIP dropdown whose own
            // 8-DIP inline padding keeps the name at the usual 16-DIP inset.
            const float vertical = maximum(0.f, (metrics.navigation_header_height - 32.f) * .5f);
            header_style.padding = {8.f, vertical, 8.f, vertical};
        }
        if (!add_node(scene, UiSemantic::channel_header, channels, header_style)) {
            return false;
        }
        if (guild_header) {
            UiLayoutStyle menu_style = fixed_height(32.f);
            menu_style.vertical_alignment = UiLayoutAlignment::center;
            menu_style.padding = {8.f, 0, 8.f, 0};
            if (!add_node(scene, UiSemantic::guild_menu,
                          scene->semantics[static_cast<u32>(UiSemantic::channel_header)], menu_style))
                return false;
        }
        if (view == UiShellView::friends || view == UiShellView::private_channels) {
            auto friends_style = fixed_height(40.f);
            friends_style.margin = {8.f, 4.f, 8.f, 4.f};
            friends_style.padding = {16.f, 0, 16.f, 0};
            if (!add_node(scene, UiSemantic::friends_button, channels, friends_style))
                return false;
            UiLayoutStyle create_style = fixed_width(24.f);
            create_style.height = ui_layout_fixed(24.f);
            create_style.horizontal_alignment = UiLayoutAlignment::end;
            create_style.vertical_alignment = UiLayoutAlignment::center;
            create_style.flags |= ui_layout_node_allow_overlap;
            if (!add_node(scene, UiSemantic::private_create,
                          scene->semantics[static_cast<u32>(UiSemantic::channel_header)], create_style))
                return false;
        }
        UiLayoutStyle navigation_style = ui_layout_style();
        navigation_style.width = ui_layout_fill();
        navigation_style.height = ui_layout_fill();
        navigation_style.horizontal_alignment = UiLayoutAlignment::stretch;
        navigation_style.vertical_alignment = UiLayoutAlignment::stretch;
        navigation_style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children |
                                                  ui_layout_node_scroll_vertical);
        UiLayoutStyle footer_style = fixed_height(metrics.navigation_footer_height);
        footer_style.flow = UiLayoutFlow::overlay;
        footer_style.margin = {kUiFloatingSurfaceInset, kUiFloatingSurfaceInset + metrics.navigation_footer_reserve,
                               kUiFloatingSurfaceInset,
                               kUiFloatingSurfaceInset};
        if (!add_node(scene, UiSemantic::channel_navigation, channels, navigation_style) ||
            !add_node(scene, UiSemantic::channel_footer, channels, footer_style)) {
            return false;
        }
        UiLayoutStyle settings_style = fixed_width(32.f);
        settings_style.height = ui_layout_fixed(32.f);
        settings_style.horizontal_alignment = UiLayoutAlignment::end;
        settings_style.vertical_alignment = UiLayoutAlignment::center;
        settings_style.margin.right = 4.f;
        settings_style.flags |= ui_layout_node_allow_overlap;
        if (!add_node(scene, UiSemantic::settings,
                      scene->semantics[static_cast<u32>(UiSemantic::channel_footer)], settings_style))
            return false;
    }

    UiLayoutStyle title_style = fixed_height(metrics.title_bar_height);
    title_style.flow = UiLayoutFlow::overlay;
    title_style.margin.left = content_left;
    title_style.padding = metrics.title_bar_padding;
    if (!add_node(scene, UiSemantic::title_bar, root, title_style)) {
        return false;
    }
    const UiWidgetHandle title = scene->semantics[static_cast<u32>(UiSemantic::title_bar)];
    UiLayoutStyle title_label_style = ui_layout_style();
    title_label_style.width = ui_layout_fill();
    title_label_style.height = ui_layout_fill();
    title_label_style.horizontal_alignment = UiLayoutAlignment::stretch;
    title_label_style.vertical_alignment = UiLayoutAlignment::stretch;
    title_label_style.flags =
        static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children |
                         ui_layout_node_text_content | ui_layout_node_allow_zero_size);
    if (!add_node(scene, UiSemantic::title_label, title, title_label_style)) {
        return false;
    }
    // The member-list toggle is part of the guild-channel title bar even
    // while the list itself is hidden. Friends views have no member width.
    if (metrics.member_sidebar_width > 0.0f) {
        UiLayoutStyle toggle_style = fixed_width(36.0f);
        toggle_style.height = ui_layout_fixed(32.0f);
        toggle_style.vertical_alignment = UiLayoutAlignment::center;
        toggle_style.horizontal_alignment = UiLayoutAlignment::end;
        toggle_style.padding = UiInsets{8.0f, 0.0f, 8.0f, 0.0f};
        toggle_style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children |
                                              ui_layout_node_text_content);
        if (!add_node(scene, UiSemantic::member_toggle, title, toggle_style)) {
            return false;
        }
    }
    if (view != UiShellView::friends && shell_flag(metrics, ui_shell_layout_pins)) {
        UiLayoutStyle pins_style = fixed_width(36.f);
        pins_style.height = ui_layout_fixed(32.f);
        pins_style.vertical_alignment = UiLayoutAlignment::center;
        pins_style.horizontal_alignment = UiLayoutAlignment::end;
        pins_style.padding = {8.f, 0, 8.f, 0};
        pins_style.flags |= ui_layout_node_clip_children;
        if (!add_node(scene, UiSemantic::pins_toggle, title, pins_style))
            return false;
    }
    if (view != UiShellView::friends && (variant & 16u)) {
        UiLayoutStyle threads_style = fixed_width(36.f);
        threads_style.height = ui_layout_fixed(32.f);
        threads_style.vertical_alignment = UiLayoutAlignment::center;
        threads_style.horizontal_alignment = UiLayoutAlignment::end;
        threads_style.padding = {8.f, 0, 8.f, 0};
        threads_style.flags |= ui_layout_node_clip_children;
        if (!add_node(scene, UiSemantic::threads_toggle, title, threads_style))
            return false;
    }
    if (view == UiShellView::private_channels && (variant & 32u)) {
        const UiSemantic buttons[]{UiSemantic::private_add, UiSemantic::private_call};
        for (const auto semantic : buttons) {
            UiLayoutStyle style = fixed_width(36.f);
            style.height = ui_layout_fixed(32.f);
            style.vertical_alignment = UiLayoutAlignment::center;
            style.horizontal_alignment = UiLayoutAlignment::end;
            style.padding = {8.f, 0, 8.f, 0};
            style.flags |= ui_layout_node_clip_children;
            if (!add_node(scene, semantic, title, style))
                return false;
        }
    }
    if (variant & 64u) {
        UiLayoutStyle inbox_style = fixed_width(36.f);
        inbox_style.height = ui_layout_fixed(32.f);
        inbox_style.vertical_alignment = UiLayoutAlignment::center;
        inbox_style.horizontal_alignment = UiLayoutAlignment::end;
        inbox_style.padding = {8.f, 0, 8.f, 0};
        inbox_style.flags |= ui_layout_node_clip_children;
        if (!add_node(scene, UiSemantic::inbox_toggle, title, inbox_style))
            return false;
    }
    UiLayoutStyle message_style = ui_layout_style();
    message_style.width = ui_layout_fill();
    message_style.height = ui_layout_fill();
    message_style.margin.bottom = metrics.message_bottom_margin;
    message_style.margin.top = metrics.title_bar_height;
    message_style.horizontal_alignment = UiLayoutAlignment::stretch;
    message_style.vertical_alignment = UiLayoutAlignment::stretch;
    message_style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children |
                                           ui_layout_node_scroll_vertical);
    if (!add_node(scene, UiSemantic::message_view, content, message_style)) {
        return false;
    }
    if ((variant & 4u) != 0) {
        UiLayoutStyle composer_style = fixed_height(metrics.composer_height);
        composer_style.margin = metrics.composer_margin;
        if (!add_node(scene, UiSemantic::composer, content, composer_style)) {
            return false;
        }
    }

    // The optional component borrows these six shell slots. They stay hidden
    // and zero-sized until a current thread projection is published after
    // layout; ordinary channels reserve no strip height or input targets.
    UiLayoutStyle thread_style = ui_layout_style();
    thread_style.width = ui_layout_fixed(0);
    thread_style.height = ui_layout_fixed(0);
    thread_style.flags = ui_layout_node_allow_zero_size;
    if (!add_node(scene, UiSemantic::thread_header, content, thread_style))
        return false;
    const auto thread_root = scene->semantics[static_cast<u32>(UiSemantic::thread_header)];
    constexpr UiSemantic thread_children[]{UiSemantic::thread_status, UiSemantic::thread_primary,
                                           UiSemantic::thread_overflow, UiSemantic::thread_feedback,
                                           UiSemantic::thread_cancel};
    for (auto semantic : thread_children)
        if (!add_node(scene, semantic, thread_root, thread_style))
            return false;

    update_header_controls(scene, metrics, width, variant);
    if (!ui_widget_manager_layout(&scene->widgets, UiSize{width, height}).ok()) {
        return false;
    }
    ++scene->layout_passes;
    if (!project_shell(scene)) {
        return false;
    }
    ++scene->layout_rebuilds;
    return true;
}

} // namespace

const UiBootstrapMetrics kDefaultUiBootstrapMetrics{
    16.0f, 12.0f, 40.0f, 58.0f, 8.0f, 34.0f, 34.0f, 68.0f, 40.0f, 72.0f, 46.0f, 32.0f,
};

bool ui_scene_initialize_bootstrap(UiScene* scene) noexcept {
    if (!scene || scene->storage.arena.base) {
        return false;
    }
    *scene = UiScene{};
    if (!ui_fixed_widget_storage_initialize(&scene->storage, kMaximumUiNodes,
                                            sizeof(UiNode) * kMaximumUiNodes, alignof(UiNode))) {
        return false;
    }
    scene->nodes = ui_fixed_widget_storage_sidecar<UiNode>(scene->storage);
    if (!scene->nodes || !ui_widget_manager_initialize(&scene->widgets, scene->storage.widgets,
                                                       scene->storage.layout_nodes)) {
        ui_scene_release(scene);
        return false;
    }
    return true;
}

void ui_scene_release(UiScene* scene) noexcept {
    if (!scene) {
        return;
    }
    ui_fixed_widget_storage_release(&scene->storage);
    *scene = UiScene{};
}

bool ui_scene_layout(UiScene* scene, float width, float height) noexcept {
    return ui_scene_layout_with_metrics(scene, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics,
                                        width, height);
}

bool ui_scene_layout_with_metrics(UiScene* scene, UiShellView view, const UiShellLayoutMetrics& metrics,
                                  float width, float height) noexcept {
    if (!scene || !ui_extent_valid(width) || !ui_extent_valid(height) || width <= 0.0f || height <= 0.0f ||
        view >= UiShellView::count || !shell_metrics_valid(metrics)) {
        if (scene) {
            ++scene->rejected;
        }
        return false;
    }
    const u8 variant = shell_variant(metrics, width);
    if (scene->count != 0 && scene->width == width && scene->height == height && scene->view == view &&
        scene->layout_metrics_revision == metrics.revision && scene->layout_variant == variant) {
        return true;
    }
    const bool same_structure = scene->count != 0 && scene->layout_variant == variant &&
                                scene->view == view && scene->layout_metrics_revision == metrics.revision;
    if (same_structure) {
        update_header_controls(scene, metrics, width, variant);
        if (!ui_widget_manager_layout(&scene->widgets, UiSize{width, height}).ok()) {
            ++scene->rejected;
            return false;
        }
        ++scene->layout_passes;
        if (!project_shell(scene)) {
            ++scene->rejected;
            return false;
        }
    } else if (!build_shell(scene, view, metrics, variant, width, height)) {
        ++scene->rejected;
        return false;
    }
    scene->width = width;
    scene->height = height;
    scene->layout_variant = variant;
    scene->layout_metrics_revision = metrics.revision;
    scene->view = view;
    return true;
}

float ui_scene_composer_width(const UiShellLayoutMetrics& metrics, float width) noexcept {
    if (!ui_extent_valid(width) || width <= 0.f || !shell_metrics_valid(metrics) ||
        !shell_flag(metrics, ui_shell_layout_composer))
        return 0.f;
    const u8 variant = shell_variant(metrics, width);
    return maximum(0.f, width - shell_content_left(metrics, (variant & 1u) != 0) -
                            ((variant & 2u) ? metrics.member_sidebar_width : 0.f) -
                            metrics.composer_margin.left - metrics.composer_margin.right);
}

UiNodeHandle ui_scene_find(const UiScene& scene, UiSemantic semantic) noexcept {
    const u32 semantic_index = static_cast<u32>(semantic);
    if (semantic_index >= static_cast<u32>(UiSemantic::count)) {
        return UiNodeHandle{kInvalidUiWidgetSlot, 0};
    }
    const UiWidgetHandle widget = scene.semantics[semantic_index];
    return UiNodeHandle{widget.slot, widget.generation};
}

const UiNode* ui_scene_resolve(const UiScene& scene, UiNodeHandle handle) noexcept {
    if (handle.generation == 0 || handle.slot >= scene.count) {
        return nullptr;
    }
    const UiNode& node = scene.nodes[handle.slot];
    return node.visible && node.generation == handle.generation ? &node : nullptr;
}

UiRect ui_scene_content_rectangle(const UiScene& scene, UiSemantic semantic) noexcept {
    const UiNode* node = ui_scene_resolve(scene, ui_scene_find(scene, semantic));
    const UiLayoutNode* layout =
        node ? ui_widget_manager_layout_node(scene.widgets, node->widget) : nullptr;
    return layout ? layout->content_rectangle : UiRect{};
}

UiShellHeaderGeometry ui_scene_header_geometry(const UiScene& scene) noexcept {
    UiShellHeaderGeometry geometry{};
    const auto* title = ui_scene_resolve(scene, ui_scene_find(scene, UiSemantic::title_bar));
    if (!title)
        return geometry;
    geometry.title = title->rectangle;
    if (const auto* label = ui_scene_resolve(scene, ui_scene_find(scene, UiSemantic::title_label)))
        geometry.label = label->rectangle;
    if (const auto* toggle = ui_scene_resolve(scene, ui_scene_find(scene, UiSemantic::member_toggle)))
        geometry.member_toggle = toggle->rectangle;
    if (const auto* pins = ui_scene_resolve(scene, ui_scene_find(scene, UiSemantic::pins_toggle)))
        geometry.pins = pins->rectangle;
    if (const auto* threads = ui_scene_resolve(scene, ui_scene_find(scene, UiSemantic::threads_toggle)))
        geometry.threads = threads->rectangle;
    if (const auto* inbox = ui_scene_resolve(scene, ui_scene_find(scene, UiSemantic::inbox_toggle)))
        geometry.inbox = inbox->rectangle;
    const auto content = ui_scene_content_rectangle(scene, UiSemantic::title_bar);
    const float controls_width = (ui_rect_has_area(geometry.member_toggle) ? 52.f : 8.f) +
                                 (ui_rect_has_area(geometry.pins) ? 44.f : 0.f) +
                                 (ui_rect_has_area(geometry.threads) ? 44.f : 0.f);
    const float trailing = ui_rect_has_area(geometry.inbox) ? 40.f : 0.f;
    const float field_width = header_search_width(ui_rect_width(content), controls_width + trailing);
    geometry.search_field = {maximum(content.left, content.right - trailing - field_width), geometry.title.top + 8.f,
                             content.right - trailing, geometry.title.top + 40.f};
    return geometry;
}

UiLayoutValidationResult ui_scene_validate(const UiScene& scene,
                                           Slice<UiLayoutDiagnostic> output) noexcept {
    return ui_layout_engine_validate(scene.widgets.layout, output);
}

UiVirtualRange ui_virtual_range(u32 item_count, float item_extent, float scroll_offset,
                                float viewport_extent, u32 overscan) noexcept {
    if (item_count == 0 || !ui_extent_valid(item_extent) || item_extent < 1.0f / 1024.0f ||
        !ui_extent_valid(scroll_offset) || !ui_extent_valid(viewport_extent) || viewport_extent <= 0.0f) {
        return UiVirtualRange{};
    }
    const u32 first_visible = static_cast<u32>(scroll_offset / item_extent);
    const u32 visible_end = static_cast<u32>((scroll_offset + viewport_extent) / item_extent) + 1;
    const u32 first = first_visible > overscan ? first_visible - overscan : 0;
    const u32 end = minimum(item_count, visible_end > item_count - minimum(item_count, overscan)
                                            ? item_count
                                            : visible_end + overscan);
    return UiVirtualRange{minimum(first, item_count), end > first ? end - first : 0,
                          static_cast<float>(first) * item_extent - scroll_offset};
}

} // namespace maicord
