#include "maicord/app/received_attachment_open.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord { namespace {
bool identity_valid(const ReceivedAttachmentOpenIdentity& id) noexcept {
    return id.account_scope && id.channel_id && id.message_id && id.attachment_id && id.message_revision;
}
bool same(const ReceivedAttachmentOpenIdentity& a, const ReceivedAttachmentOpenIdentity& b) noexcept {
    return a.account_scope == b.account_scope && a.channel_id == b.channel_id &&
           a.message_id == b.message_id && a.attachment_id == b.attachment_id &&
           a.message_revision == b.message_revision && a.source_channel_id == b.source_channel_id;
}
bool url_valid(const ReceivedAttachmentOpenIdentity& id, Utf8View url) noexcept {
    // Match the native URL opener's bounded UTF-16 conversion and accept only
    // a CDN HTTPS attachment path, never a file or executable path.
    if (!identity_valid(id) || !url.data || !url.bytes || url.bytes > 2047)
        return false;
    u8 target[kCdnMaximumTargetBytes]{}, prefix[80]{};
    const auto parsed = parse_signed_attachment_url(url, id.attachment_id, {target, sizeof(target)});
    if (!parsed.ok())
        return false;
    ProtocolWriter writer{{prefix, sizeof(prefix)}, 0};
    const bool prefix_written = parsed.ephemeral
        ? protocol_writer_append_literal(&writer, "/ephemeral-attachments/")
        : protocol_writer_append_literal(&writer, "/attachments/");
    return prefix_written &&
           protocol_writer_append_u64(&writer, id.source_channel_id ? id.source_channel_id : id.channel_id) &&
           protocol_writer_append_literal(&writer, "/") &&
           protocol_writer_append_u64(&writer, id.attachment_id) && protocol_writer_append_literal(&writer, "/") &&
           parsed.target_bytes > writer.used && bytes_equal(target, prefix, writer.used);
}
ReceivedAttachmentOpenResult resolve(ReceivedAttachmentOpen& intent, Utf8View source,
                                     MediaUrlResolver* resolver, u64 now_ms, u64 now_seconds,
                                     Utf8View* output) noexcept {
    const auto url = media_url_resolve(resolver, source, now_ms, now_seconds, intent.force_refresh);
    if (url.bytes) {
        const bool valid = url_valid(intent.identity, url);
        reset_value(&intent);
        if (!valid)
            return ReceivedAttachmentOpenResult::invalid;
        *output = url;
        return ReceivedAttachmentOpenResult::ready;
    }
    const auto state = media_url_resolution_state(*resolver, source);
    if (state == MediaUrlState::queued || state == MediaUrlState::pending) {
        intent.pending = true;
        return ReceivedAttachmentOpenResult::pending;
    }
    reset_value(&intent);
    return ReceivedAttachmentOpenResult::failed;
}
} // namespace
ReceivedAttachmentOpenResult received_attachment_open_begin(
    ReceivedAttachmentOpen* intent, const ReceivedAttachmentOpenIdentity& id, Utf8View source,
    MediaUrlResolver* resolver, u64 now_ms, u64 now_seconds, Utf8View* output, bool force_refresh) noexcept {
    if (output) reset_value(output);
    if (!intent) return ReceivedAttachmentOpenResult::invalid;
    reset_value(intent);
    if (!output || !resolver || !url_valid(id, source)) return ReceivedAttachmentOpenResult::invalid;
    intent->identity = id;
    intent->source_fingerprint = hash64_bytes(kHash64Seed, source.data, source.bytes);
    intent->force_refresh = force_refresh;
    // A new click may retry its failed result. Frame resume
    // never rearms failures, and other sources retain their attempts/deadlines.
    for (auto& entry : resolver->entries) {
        if ((entry.state == MediaUrlState::failed || (force_refresh && entry.state == MediaUrlState::ready)) &&
            entry.original_bytes == source.bytes &&
            bytes_equal(entry.original, source.data, source.bytes)) {
            entry.state = MediaUrlState::queued;
            entry.attempts = 0;
            entry.retry_at_ms = 0;
            entry.refreshed_bytes = 0;
            entry.last_used_ms = now_ms;
            break;
        }
    }
    return resolve(*intent, source, resolver, now_ms, now_seconds, output);
}
ReceivedAttachmentOpenResult received_attachment_open_resume(
    ReceivedAttachmentOpen* intent, const ReceivedAttachmentOpenIdentity& id, Utf8View source,
    MediaUrlResolver* resolver, u64 now_ms, u64 now_seconds, Utf8View* output) noexcept {
    if (output) reset_value(output);
    if (!intent || !output || !resolver) return ReceivedAttachmentOpenResult::invalid;
    if (!intent->pending) return ReceivedAttachmentOpenResult::none;
    if (!same(intent->identity, id) || !url_valid(id, source) ||
        intent->source_fingerprint != hash64_bytes(kHash64Seed, source.data, source.bytes)) {
        reset_value(intent);
        return ReceivedAttachmentOpenResult::stale;
    }
    if (intent->force_refresh && media_url_resolution_state(*resolver, source) == MediaUrlState::empty) {
        reset_value(intent);
        return ReceivedAttachmentOpenResult::failed;
    }
    return resolve(*intent, source, resolver, now_ms, now_seconds, output);
}
} // namespace maicord
