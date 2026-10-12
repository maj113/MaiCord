#pragma once
#include "maicord/render/d3d11_renderer.hpp"
namespace maicord {
// The profile editor page (Profiles settings with its preview column), the
// image cropper and the decoration picker drawn by the real renderer from
// synthetic data; frames go through `capture` (MAICORD_UI_PROBE_CAPTURE_DIR).
// No application owner, transport or account is involved.
bool profile_editor_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache* measure, const ChatStateStore* state,
                             const UserStore* users, bool (*capture)(const wchar_t*) noexcept) noexcept;
} // namespace maicord
