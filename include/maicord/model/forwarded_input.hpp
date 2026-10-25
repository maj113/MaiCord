#pragma once
#include "maicord/model/message_composer.hpp"

namespace maicord {

// Control characters, C1 controls and line separators never enter a single-line field.
constexpr bool text_input_single_line_forbidden(u32 unit) noexcept {
    return unit < 32 || (unit >= 127 && unit <= 159) || unit == 0x2028 || unit == 0x2029;
}

// Text input operations for a MessageComposer a host binds as its edit field. Every call
// forwards to the composer's own operations once Ready (the host's conditions on the
// binding) and the composer accept the target. Replace first rejects any UTF-16 unit for
// which Forbidden is true. CarryIdentity forwards the target's identity (the composer's
// document identity) instead of the composer's current one.
template <bool (*Ready)(const TextInputTarget&) noexcept, bool (*Forbidden)(u32) noexcept,
          bool CarryIdentity = false>
struct ForwardedInput {
    static TextInputTarget forward(const TextInputTarget& target) noexcept {
        auto result = message_composer_input_target(static_cast<MessageComposer*>(target.context));
        if constexpr (CarryIdentity)
            result.identity = target.identity;
        return result;
    }
    // Ends the native composition group and drops pending input.
    static void abandon(const TextInputTarget& target) noexcept {
        text_input_end_group(forward(target));
        text_input_cancel(forward(target));
    }

    static constexpr TextInputOperations operations{
        [](const TextInputTarget& t) noexcept { return Ready(t) && text_input_ready(forward(t)); },
        [](const TextInputTarget& t) noexcept { return text_input_revision(forward(t)); },
        [](const TextInputTarget& t, u32* n) noexcept { return text_input_length(forward(t), n); },
        [](const TextInputTarget& t, TextInputSelection* s) noexcept {
            return text_input_selection(forward(t), s);
        },
        [](const TextInputTarget& t, TextInputSelection s) noexcept { return text_input_select(forward(t), s); },
        [](const TextInputTarget& t, u32 a, u32 b, Slice<u16> out, u32* n, u32* next) noexcept {
            return text_input_copy(forward(t), a, b, out, n, next);
        },
        [](const TextInputTarget& t, u32 a, u32 b, ConstSlice<u16> in, TextInputChange* change) noexcept {
            if (!change || (!in.data && in.count))
                return TextInputResult::invalid_input;
            for (u32 i = 0; i < in.count; ++i)
                if (Forbidden(in[i]))
                    return TextInputResult::invalid_input;
            return text_input_replace(forward(t), a, b, in, change);
        },
        [](const TextInputTarget& t) noexcept { text_input_cancel(forward(t)); },
        [](const TextInputTarget& t) noexcept {
            auto* model = static_cast<MessageComposer*>(t.context);
            return (model->history || message_composer_enable_history(model, 2048)) &&
                   text_input_begin_group(forward(t));
        },
        [](const TextInputTarget& t) noexcept { text_input_end_group(forward(t)); },
    };
};

} // namespace maicord
