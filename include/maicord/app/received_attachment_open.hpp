#pragma once
#include "maicord/app/media_url_resolver.hpp"

namespace maicord {
struct ReceivedAttachmentOpenIdentity {
    u64 account_scope{}, channel_id{}, message_id{}, attachment_id{};
    u32 message_revision{};
    // Forwarded CDN owner; zero keeps the enclosing channel's ordinary path.
    u64 source_channel_id{};
};
struct ReceivedAttachmentOpen {
    ReceivedAttachmentOpenIdentity identity{};
    u64 source_fingerprint{};
    bool pending{};
    bool force_refresh{};
};
static_assert(sizeof(ReceivedAttachmentOpen) <= 64);
enum class ReceivedAttachmentOpenResult : u8 { none, pending, ready, invalid, stale, failed };
// Stores one scoped user intent, never a URL or resource. Ready output borrows
// the current message or resolver arena. Only ready permits HTTPS activation.
// force_refresh is for a new user retry: it re-arms only its own ready/failed
// request once. Frame resume observes that request and cannot reset its
// attempts/deadline or requeue a terminal failure.
ReceivedAttachmentOpenResult received_attachment_open_begin(
    ReceivedAttachmentOpen*, const ReceivedAttachmentOpenIdentity&, Utf8View source,
    MediaUrlResolver*, u64 now_ms, u64 now_seconds, Utf8View* ready_url,
    bool force_refresh = false) noexcept;
ReceivedAttachmentOpenResult received_attachment_open_resume(
    ReceivedAttachmentOpen*, const ReceivedAttachmentOpenIdentity&, Utf8View source,
    MediaUrlResolver*, u64 now_ms, u64 now_seconds, Utf8View* ready_url) noexcept;
} // namespace maicord
