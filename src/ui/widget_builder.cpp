#include "maicord/ui/widget_builder.hpp"

namespace maicord {
namespace {

u16 control_flags(bool keyboard_focusable) noexcept {
    return static_cast<u16>(ui_widget_enabled | ui_widget_hit_test |
                            (keyboard_focusable ? ui_widget_focusable : 0));
}

} // namespace

UiWidgetBuilder::UiWidgetBuilder(UiWidgetManager& manager) noexcept
    : manager_(&manager), generation_(manager.generation),
      error_(manager.generation ? UiWidgetError::none : UiWidgetError::invalid_argument) {
}

UiWidgetResult UiWidgetBuilder::widget(UiWidgetHandle parent, const UiWidgetKey& key, UiWidgetKind kind,
                                       u16 flags, const UiLayoutStyle& style, UiSize intrinsic,
                                       float baseline) noexcept {
    if (ok() && manager_->generation != generation_) {
        error_ = UiWidgetError::invalid_parent;
    }
    if (!ok()) {
        return {error_, ui_widget_invalid_handle()};
    }
    const UiWidgetDesc description{key.id, kind, flags, {key.name, style, intrinsic, baseline, key.id}};
    const UiWidgetResult result = ui_widget_manager_add(manager_, parent, description);
    if (!result.ok()) {
        error_ = result.error;
    }
    return result;
}

UiWidgetResult UiWidgetBuilder::container(UiWidgetHandle parent, const UiWidgetKey& key,
                                          const UiLayoutStyle& style) noexcept {
    return widget(parent, key, UiWidgetKind::container, ui_widget_enabled, style);
}

UiWidgetResult UiWidgetBuilder::text(UiWidgetHandle parent, const UiWidgetKey& key,
                                     const UiLayoutStyle& style, UiSize intrinsic,
                                     float baseline) noexcept {
    UiLayoutStyle measured_style = style;
    measured_style.flags |= ui_layout_node_text_content;
    return widget(parent, key, UiWidgetKind::text, ui_widget_none, measured_style, intrinsic, baseline);
}

UiWidgetResult UiWidgetBuilder::button(UiWidgetHandle parent, const UiWidgetKey& key,
                                       const UiLayoutStyle& style, bool keyboard_focusable) noexcept {
    return widget(parent, key, UiWidgetKind::button, control_flags(keyboard_focusable), style);
}

UiWidgetResult UiWidgetBuilder::text_field(UiWidgetHandle parent, const UiWidgetKey& key,
                                           const UiLayoutStyle& style, bool keyboard_focusable) noexcept {
    return widget(parent, key, UiWidgetKind::text_field, control_flags(keyboard_focusable), style);
}

} // namespace maicord
