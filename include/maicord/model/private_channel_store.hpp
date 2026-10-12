#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/core/arena.hpp"

namespace maicord {

inline constexpr u32 kPrivateRecipientMaximum = 256;
struct PrivateNicknameRecord {
    u64 channel_id;
    u64 user_id;
    char name[128];
    u8 bytes;
};
struct PrivateNicknameInput { u64 user_id; Utf8View name; };
// Message request state of a DM (is_message_request / is_spam).
enum PrivateChannelRequestFlag : u8 {
    private_request_pending = 1u << 0,
    private_request_spam = 1u << 1,
};
// DM safety warning types (channel safety_warnings[].type).
enum class PrivateSafetyWarning : u8 {
    none,
    stranger_danger = 1,
    inappropriate_conversation_tier_1 = 2,
    inappropriate_conversation_tier_2 = 3,
    likely_account_takeover = 4,
};
struct PrivateChannelRecord {
    u64 id;
    u64 owner_id;
    u64 sort_sequence;
    // The first undismissed safety warning; zero id when there is none.
    u64 safety_warning_id;
    u32 recipient_offset;
    u32 revision;
    u16 recipient_count;
    u8 icon_bytes;
    char icon[34];
    u8 request_flags;
    PrivateSafetyWarning safety_warning;
};
struct PrivateChannelStore {
    u64 sort_sequence;
    Slice<PrivateChannelRecord> records;
    Slice<u64> recipients;
    u32 count;
    u32 recipient_count;
    u32 revision;
    Arena record_backing;
    Arena recipient_backing;
    Slice<PrivateNicknameRecord> nicknames;
    u32 nickname_count;
    Arena nickname_backing;
};
struct PrivateChannelInput {
    u64 id;
    u64 owner_id;
    ConstSlice<u64> recipients;
    Utf8View icon;
    bool recipients_known;
    bool owner_known;
    bool icon_known;
    // Message request and safety fields replace the record's when known.
    bool request_known;
    bool safety_known;
    u8 request_flags;
    PrivateSafetyWarning safety_warning;
    u64 safety_warning_id;
};

bool private_channel_store_initialize(PrivateChannelStore* store,
    Slice<PrivateChannelRecord> records, Slice<u64> recipients,
    Slice<PrivateNicknameRecord> nicknames = {}) noexcept;
bool private_channel_store_initialize_owned(PrivateChannelStore* store, u32 records, u32 recipients) noexcept;
void private_channel_store_release(PrivateChannelStore* store) noexcept;
void private_channel_store_clear(PrivateChannelStore* store) noexcept;
bool private_channel_store_touch_sort(PrivateChannelStore* store, u64 id) noexcept;
constexpr u64 private_channel_recency(u64 channel_id, u64 last_message_id) noexcept {
    return (last_message_id ? last_message_id : channel_id) >> 22;
}
const PrivateChannelRecord* private_channel_store_find(const PrivateChannelStore& store, u64 id) noexcept;
bool private_channel_store_can_replace(const PrivateChannelStore& store, const PrivateChannelInput& input) noexcept;
bool private_channel_store_prepare(PrivateChannelStore* store, const PrivateChannelInput& input) noexcept;
bool private_channel_store_replace(PrivateChannelStore* store, const PrivateChannelInput& input) noexcept;
bool private_channel_store_remove(PrivateChannelStore* store, u64 id) noexcept;
ConstSlice<u64> private_channel_store_recipients(const PrivateChannelStore& store,
    const PrivateChannelRecord& record) noexcept;
// Optimistic local updates after an accepted request or a dismissed warning.
bool private_channel_store_set_request(PrivateChannelStore* store, u64 id, u8 request_flags) noexcept;
bool private_channel_store_clear_safety_warning(PrivateChannelStore* store, u64 id, u64 warning_id) noexcept;
// Pending message requests; spam requests are counted separately.
u32 private_channel_store_request_count(const PrivateChannelStore& store, bool spam) noexcept;
constexpr bool private_channel_is_request(const PrivateChannelRecord& record) noexcept {
    return (record.request_flags & (private_request_pending | private_request_spam)) != 0;
}
Utf8View private_channel_store_nickname(const PrivateChannelStore& store, u64 channel_id, u64 user_id) noexcept;
bool private_channel_store_nicknames(const PrivateChannelStore& store, u64 channel_id,
    Slice<PrivateNicknameInput> output, u32* count) noexcept;
bool private_channel_store_prepare_nicknames(PrivateChannelStore* store, u64 channel_id,
    ConstSlice<PrivateNicknameInput> names) noexcept;
bool private_channel_store_replace_nicknames(PrivateChannelStore* store, u64 channel_id,
    ConstSlice<PrivateNicknameInput> names) noexcept;

} // namespace maicord
