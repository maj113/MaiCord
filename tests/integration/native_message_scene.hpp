#pragma once
#include "maicord/ui/chat/messages/text_coordinator.hpp"

namespace maicord {
// A four-row message scene and the storage of its text presentation: the native
// projection the isolated message probes draw over a timeline their adapter owns.
inline constexpr u32 kNativeMessageRows = 4;
struct NativeMessageScene {
    MessageScene scene{};
    MessageTextPresentation text{};
    UiWidget widgets[kNativeMessageRows * kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode nodes[kNativeMessageRows * kMessageSceneWidgetsPerRow + 1]{};
    MessageTextCommand commands[128]{};
    char bytes[8192]{};
    TextStyleSpan styles[128]{};
    MessageMarkdownSemantic semantics[64]{};
    MessageTextMeasurement measurements[8]{};
    MessageTextSpoilerReveal reveals[4]{};
    u32 markdown_scratch[8192]{};
    ~NativeMessageScene() noexcept { message_scene_release(&scene); }
    bool initialize(MessageTimeline* timeline) noexcept {
        return message_scene_initialize_owned(&scene, timeline, kNativeMessageRows, 1,
            {widgets, kNativeMessageRows * kMessageSceneWidgetsPerRow + 1},
            {nodes, kNativeMessageRows * kMessageSceneWidgetsPerRow + 1}) &&
            message_text_presentation_initialize(&text, {commands, 128}, {bytes, sizeof(bytes)},
                {styles, 128}, {semantics, 64}, {measurements, 8}, {reveals, 4}, {markdown_scratch, 8192});
    }
};
} // namespace maicord
