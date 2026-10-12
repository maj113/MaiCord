#pragma once

#include "maicord/ui/widget_manager.hpp"

namespace maicord {

struct UiWidgetKey {
    u32 id;
    // Debug name only, not the rendered label. Borrowed bytes must
    // outlive the retained node, until its manager tree is reset or released.
    Utf8View name;
};

// A short-lived, allocation-free construction view over a retained owner.
// It stops at the first error. The scene remains responsible for aborting or
// resetting an incomplete tree; successful handles keep the owner's lifetime.
// Construct/use on the owner UI thread. The manager object must remain alive
// and initialized throughout builder use; the captured generation rejects a
// reset, but cannot make a dangling or released/reinitialized owner safe.
// Do not retain this construction view across owner teardown or callbacks.
class UiWidgetBuilder {
  public:
    explicit UiWidgetBuilder(UiWidgetManager& manager) noexcept;

    UiWidgetResult widget(UiWidgetHandle parent, UiWidgetKey key, UiWidgetKind kind, u16 flags,
                          const UiLayoutStyle& style, UiSize intrinsic = {}, float baseline = 0) noexcept;
    UiWidgetResult container(UiWidgetHandle parent, UiWidgetKey key, const UiLayoutStyle& style) noexcept;
    // Enables retained text measurement through the manager's callback/key.
    // Without a callback, the supplied intrinsic extent/baseline is used.
    UiWidgetResult text(UiWidgetHandle parent, UiWidgetKey key, const UiLayoutStyle& style,
                        UiSize intrinsic = {}, float baseline = 0) noexcept;
    UiWidgetResult button(UiWidgetHandle parent, UiWidgetKey key, const UiLayoutStyle& style,
                          bool keyboard_focusable = true) noexcept;
    UiWidgetResult text_field(UiWidgetHandle parent, UiWidgetKey key, const UiLayoutStyle& style,
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
