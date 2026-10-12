#pragma once

#include "maicord/ui/login_scene.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/qr_code.hpp"
#include "maicord/ui/text_composition.hpp"

namespace maicord {

struct LoginCompositionGeometry {
    UiRect rectangle;
    TextCompositionSpan span;
};

struct LoginPresentationInput {
    ConstSlice<UiRect> edit_selections;
    ConstSlice<LoginCompositionGeometry> edit_composition;
    UiRect edit_caret;
    const QrCode* qr_code;
    bool edit_active;
    bool caret_visible;
};

bool login_presentation_build(const LoginScene& scene,
                              const LoginPresentationInput& input,
                              const Theme& theme, float viewport_width,
                              float viewport_height, u32 dpi,
                              PaintStream* stream) noexcept;

} // namespace maicord
