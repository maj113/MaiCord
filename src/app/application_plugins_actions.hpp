#pragma once
#include "maicord/app/plugin_sdk.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord::app_detail {
// The shared text form's content for a plugin modal: the title; labels and
// links joined by newlines as the body (links show their address since the
// form can't open them; a separator leaves a blank line); the INPUT's label,
// initial text and limit as a single-line field; the BUTTON's text as the
// accept label (critical tone: dangerous). Without an INPUT it is a
// confirmation. The body is written to body (cut on a UTF-8 boundary).
struct ApplicationPluginModalForm {
    Utf8View title, context, label, accept, initial;
    u32 maximum_bytes;
    bool confirmation_only, danger, single_line;
};
inline ApplicationPluginModalForm application_plugins_modal_form(const NativePluginSdkModal& modal,
                                                                 Slice<char> body) noexcept {
    ApplicationPluginModalForm form{};
    form.title = modal.title;
    form.confirmation_only = true;
    form.accept = utf8_literal("OK");
    const NativePluginSdkModalNode* input = nullptr;
    bool labelled = false;
    const u32 count = modal.node_count < kPluginSdkModalNodes ? modal.node_count : kPluginSdkModalNodes;
    for (u32 i = 0; i < count; ++i) {
        const auto& node = modal.nodes[i];
        if (node.kind == MC_NODE_INPUT) {
            input = &node;
        } else if (node.kind == MC_NODE_BUTTON) {
            labelled = node.text.bytes != 0;
            if (labelled) form.accept = node.text;
            form.danger = node.tone == MC_TONE_CRITICAL;
        }
    }
    if (input) {
        form.confirmation_only = false;
        form.label = input->text;
        form.initial = input->value;
        const u32 limit = modal.input_max_bytes ? modal.input_max_bytes : 256u;
        form.maximum_bytes = limit < kPluginSdkMessageBytes ? limit : kPluginSdkMessageBytes;
        form.single_line = true;
        if (!labelled) form.accept = utf8_literal("Save");
    }
    u32 at = 0;
    const auto append = [&](Utf8View text) noexcept {
        at += utf8_copy_truncated(body.data + at, body.count - at, text);
    };
    bool first = true;
    for (u32 i = 0; i < count && body.data; ++i) {
        const auto& node = modal.nodes[i];
        if (node.kind != MC_NODE_LABEL && node.kind != MC_NODE_LINK && node.kind != MC_NODE_SEPARATOR) continue;
        if (!first) append(utf8_literal("\n"));
        first = false;
        if (node.kind == MC_NODE_LABEL) {
            append(node.text);
        } else if (node.kind == MC_NODE_LINK && node.text.bytes) {
            append(node.text);
            append(utf8_literal(" ("));
            append(node.value);
            append(utf8_literal(")"));
        } else if (node.kind == MC_NODE_LINK) {
            append(node.value);
        }
    }
    form.context = {body.data, at};
    return form;
}
} // namespace maicord::app_detail
