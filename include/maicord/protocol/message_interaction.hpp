#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {

// Interaction type values a user can send from a message.
enum class MessageInteractionType : u8 {
    message_component = 3,
    modal_submit = 5,
};

inline constexpr u32 kMessageInteractionDataMaximumBytes = 12 * 1024;
inline constexpr u32 kMessageInteractionMaximumValues = 25;
inline constexpr u32 kMessageInteractionCustomIdMaximumBytes = 400;

// Copied by value across the account module boundary; data is the complete
// JSON "data" object (built by the writers below).
struct MessageInteractionRequest {
    u64 application_id;
    u64 guild_id; // 0 in private channels
    u64 channel_id;
    u64 message_id; // component interactions only
    u64 nonce;
    u32 message_flags;
    u32 data_bytes;
    MessageInteractionType type;
    u8 reserved[7];
    char data[kMessageInteractionDataMaximumBytes];
};

bool message_interaction_request_valid(const MessageInteractionRequest& request) noexcept;
// POST /interactions body. Component interactions carry the message identity:
// {"type":3,"nonce":"..","guild_id":"..","channel_id":"..","message_flags":0,
//  "message_id":"..","application_id":"..","session_id":"..","data":{..}}.
// Modal submits omit the message fields.
bool message_interaction_write_body(const MessageInteractionRequest& request, Utf8View session_id,
                                    ProtocolWriter* output) noexcept;
// {"component_type":2,"custom_id":".."}; selects add "type" and "values".
bool message_interaction_write_component_data(u16 component_type, Utf8View custom_id,
                                              ConstSlice<Utf8View> values, ProtocolWriter* output) noexcept;

// Gateway INTERACTION_CREATE / INTERACTION_SUCCESS / INTERACTION_FAILURE.
struct MessageInteractionEvent {
    u64 id;
    u64 nonce;
    u32 reason_code; // failures only
};
bool message_interaction_event_decode(Utf8View json, Slice<u32> structurals, MessageInteractionEvent* output) noexcept;

// INTERACTION_MODAL_CREATE: a form of short/paragraph text inputs and text
// paragraphs. Strings borrow the caller's pool.
inline constexpr u32 kMessageModalMaximumFields = 5;
inline constexpr u32 kMessageModalMaximumParagraphs = 4;
inline constexpr u32 kMessageModalPoolBytes = 8 * 1024;
inline constexpr u32 kMessageModalValueMaximumBytes = 4000 * 4;

enum class MessageModalInputStyle : u8 { short_text = 1, paragraph = 2 };

struct MessageModalField {
    Utf8View custom_id;
    Utf8View label;
    Utf8View description;
    Utf8View placeholder;
    Utf8View value; // prefilled value
    u32 id;
    u16 min_length;
    u16 max_length;
    MessageModalInputStyle style;
    bool required;
    bool labelled; // wrapped in a Label (type 18) rather than an action row
};

struct MessageModal {
    u64 interaction_id;
    u64 application_id;
    u64 channel_id;
    u64 nonce;
    Utf8View custom_id;
    Utf8View title;
    Utf8View application_name;
    MessageModalField fields[kMessageModalMaximumFields];
    Utf8View paragraphs[kMessageModalMaximumParagraphs]; // text displays, shown above the fields
    u32 field_count;
    u32 paragraph_count;
    bool unsupported; // contains inputs other than text (selects, uploads)
};

bool message_modal_decode(Utf8View json, Slice<u32> structurals, Slice<char> pool, MessageModal* output) noexcept;
// Length/required validation of one value; returns false with a short reason.
bool message_modal_value_valid(const MessageModalField& field, Utf8View value, Utf8View* reason) noexcept;
// {"id":"..","custom_id":"..","components":[{"type":1,"components":[{"type":4,
// "custom_id":"..","value":".."}]}]} with Label wrappers kept as
// {"type":18,"component":{..}}.
bool message_modal_write_submit_data(const MessageModal& modal, ConstSlice<Utf8View> values,
                                     ProtocolWriter* output) noexcept;

} // namespace maicord
