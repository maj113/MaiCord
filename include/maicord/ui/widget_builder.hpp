#pragma once

#include "maicord/ui/widget_manager.hpp"

namespace maicord {

struct UiWidgetKey {
    u32 id;
    // Debug name only, not the rendered label. Borrowed bytes must
    // outlive the retained node, until its manager tree is reset or released.
    Utf8View name;
};

// A short-lived, allocation-free construction view over a retained owner; it stops
// at the first error. The scene aborts or resets an incomplete tree; successful
// handles keep the owner's lifetime. UI thread only. The manager must stay alive
// and initialized while the builder is used: the captured generation rejects a
// reset but cannot make a dangling or reinitialized owner safe. Do not keep the
// view across owner teardown or callbacks.
class UiWidgetBuilder {
  public:
    explicit UiWidgetBuilder(UiWidgetManager& manager) noexcept;

    UiWidgetResult widget(UiWidgetHandle parent, const UiWidgetKey& key, UiWidgetKind kind, u16 flags,
                          const UiLayoutStyle& style, UiSize intrinsic = {}, float baseline = 0) noexcept;
    UiWidgetResult container(UiWidgetHandle parent, const UiWidgetKey& key,
                             const UiLayoutStyle& style) noexcept;
    // Enables retained text measurement through the manager's callback/key.
    // Without a callback, the supplied intrinsic extent/baseline is used.
    UiWidgetResult text(UiWidgetHandle parent, const UiWidgetKey& key, const UiLayoutStyle& style,
                        UiSize intrinsic = {}, float baseline = 0) noexcept;
    UiWidgetResult button(UiWidgetHandle parent, const UiWidgetKey& key, const UiLayoutStyle& style,
                          bool keyboard_focusable = true) noexcept;
    UiWidgetResult text_field(UiWidgetHandle parent, const UiWidgetKey& key, const UiLayoutStyle& style,
                              bool keyboard_focusable = true) noexcept;

    bool ok() const noexcept {
        return error_ == UiWidgetError::none;
    }
    UiWidgetError error() const noexcept {
        return error_;
    }

  private:
    UiWidgetManager* manager_;
    u16 generation_;
    UiWidgetError error_;
};

static_assert(sizeof(UiWidgetBuilder) == 16);

} // namespace maicord
