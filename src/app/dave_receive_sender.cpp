#include "maicord/core/win32_process.hpp"
#include "maicord/app/dave_receive_sender.hpp"
#include "maicord/app/mls_exporter.hpp"
#include "maicord/core/byte_range.hpp"
#include "byte_views.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
struct DaveReceiveNode {
    DaveReceiveNode* older;
    u64 revision, expiry_ms;
    DaveReceiveEpoch epoch;
};
namespace {
constexpr u64 never = ~u64{0};
u64 deadline(u64 now, u64 duration) noexcept {
    return duration > never - now ? never : now + duration;
}
DaveSenderError validate(const DaveReceiveSender& sender, DaveReceiveIdentity identity, u64 now) noexcept {
    if (!sender.initialized)
        return DaveSenderError::not_initialized;
    if (identity.call_generation != sender.identity.call_generation ||
        identity.user_id != sender.identity.user_id)
        return DaveSenderError::identity_mismatch;
    return now < sender.last_now_ms ? DaveSenderError::invalid_time : DaveSenderError::none;
}
bool touches(const DaveReceiveSender& sender, ConstSlice<u8> span) noexcept {
    if (byte_ranges_overlap(span, object_bytes(sender)))
        return true;
    for (const auto* node = sender.newest; node; node = node->older)
        if (byte_ranges_overlap(span, object_bytes(*node)) || dave_receive_epoch_aliases(node->epoch, span))
            return true;
    return false;
}
bool external(const DaveReceiveSender& sender, ConstSlice<u8> span) noexcept {
    return byte_range_valid(span) && !touches(sender, span);
}
void destroy(DaveReceiveNode* node) noexcept {
    dave_receive_epoch_release(&node->epoch);
    secure_clear_bytes(node, sizeof(*node));
    VirtualFree(node, 0, MEM_RELEASE);
}
void collect(DaveReceiveSender& sender, u64 now) noexcept {
    sender.last_now_ms = now;
    if (now <= sender.next_expiry_ms)
        return;
    sender.next_expiry_ms = never;
    auto** link = &sender.newest;
    while (*link) {
        auto* node = *link;
        if (now > node->expiry_ms) {
            *link = node->older;
            destroy(node);
            --sender.count;
        } else {
            sender.next_expiry_ms = minimum(sender.next_expiry_ms, node->expiry_ms);
            link = &node->older;
        }
    }
}
void refresh(DaveReceiveSender& sender, u64 expires) noexcept {
    // Authorized transitions renew grace for live previous epochs. This is
    // distinct from the non-extending old-generation deadlines within an epoch.
    for (auto* node = sender.newest; node; node = node->older)
        node->expiry_ms = expires;
    sender.next_expiry_ms = sender.newest ? expires : never;
}
DaveSenderOpenResult fail(DaveSenderError error) noexcept {
    DaveSenderOpenResult result{};
    result.error = error;
    return result;
}
} // namespace
DaveSenderError dave_receive_sender_initialize(DaveReceiveSender* sender, DaveReceiveIdentity identity,
                                               u64 now) noexcept {
    if (!sender || sender->initialized || sender->newest || !identity.call_generation || !identity.user_id)
        return DaveSenderError::invalid_input;
    sender->identity = identity;
    sender->last_now_ms = now;
    sender->next_expiry_ms = never;
    sender->initialized = true;
    return DaveSenderError::none;
}
void dave_receive_sender_release(DaveReceiveSender* sender) noexcept {
    if (!sender)
        return;
    while (sender->newest) {
        auto* node = sender->newest;
        sender->newest = node->older;
        destroy(node);
    }
    reset_value(sender);
}
DaveSenderTransitionResult dave_receive_sender_transition(DaveReceiveSender& sender,
                                                          DaveReceiveIdentity identity, u64 revision,
                                                          ConstSlice<u8> seed, u64 now, u64 grace,
                                                          Gcm64Backend backend) noexcept {
    const auto valid = validate(sender, identity, now);
    if (valid != DaveSenderError::none)
        return {{}, 0, valid};
    if (!revision || revision <= sender.latest_revision)
        return {{}, 0, DaveSenderError::stale_transition};
    if (seed.count != 16 || !external(sender, seed))
        return {{}, 0, DaveSenderError::invalid_input};
    auto* node = static_cast<DaveReceiveNode*>(
        VirtualAlloc(nullptr, sizeof(DaveReceiveNode), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!node)
        return {{}, thread_last_error(), DaveSenderError::storage};
    const auto initialized = dave_receive_epoch_initialize(&node->epoch, seed, now, backend);
    if (!initialized.ok()) {
        destroy(node);
        return {initialized, 0, DaveSenderError::epoch_initialize};
    }
    collect(sender, now);
    refresh(sender, deadline(now, grace));
    node->older = sender.newest;
    node->revision = revision;
    node->expiry_ms = never;
    sender.newest = node;
    ++sender.count;
    sender.latest_revision = revision;
    return {};
}
DaveSenderTransitionResult dave_receive_sender_transition_exporter(
    DaveReceiveSender& sender, DaveReceiveIdentity identity, u64 revision, KdfSha256Workspace& workspace,
    ConstSlice<u8> exporter_secret, u64 now, u64 grace, Gcm64Backend backend) noexcept {
    const auto valid = validate(sender, identity, now);
    if (valid != DaveSenderError::none)
        return {{}, 0, valid};
    if (!revision || revision <= sender.latest_revision)
        return {{}, 0, DaveSenderError::stale_transition};
    if (!external(sender, exporter_secret) || !external(sender, object_bytes(workspace)) ||
        !external(sender, {workspace.storage.data, workspace.storage.capacity}))
        return {{}, 0, DaveSenderError::invalid_input};
    u8 seed[16];
    const auto exported = dave_export_media_seed(workspace, exporter_secret, identity.user_id, {seed, 16});
    auto result = exported.ok() ? dave_receive_sender_transition(sender, identity, revision, {seed, 16},
                                                                 now, grace, backend)
                                : DaveSenderTransitionResult{exported, 0, DaveSenderError::epoch_export};
    secure_clear_bytes(seed, sizeof(seed));
    return result;
}
DaveSenderError dave_receive_sender_retire(DaveReceiveSender& sender, DaveReceiveIdentity identity,
                                           u64 revision, u64 now, u64 grace) noexcept {
    const auto valid = validate(sender, identity, now);
    if (valid != DaveSenderError::none)
        return valid;
    if (!revision || revision <= sender.latest_revision)
        return DaveSenderError::stale_transition;
    collect(sender, now);
    refresh(sender, deadline(now, grace));
    sender.latest_revision = revision;
    return DaveSenderError::none;
}
DaveSenderError dave_receive_sender_collect(DaveReceiveSender& sender, DaveReceiveIdentity identity,
                                            u64 now) noexcept {
    const auto valid = validate(sender, identity, now);
    if (valid == DaveSenderError::none)
        collect(sender, now);
    return valid;
}
DaveSenderOpenResult dave_receive_sender_open(DaveReceiveSender& sender, DaveReceiveIdentity identity,
                                              KdfSha256Workspace& workspace, ConstSlice<u8> frame, u64 now,
                                              Slice<u8> scratch, Slice<u8> output) noexcept {
    const auto valid = validate(sender, identity, now);
    if (valid != DaveSenderError::none)
        return fail(valid);
    if (!workspace.storage.data || !workspace.storage.locked)
        return fail(DaveSenderError::not_initialized);
    DaveClearRange ranges[kDaveMaximumClearRanges];
    DaveFrameView view{};
    const auto parsed = dave_frame_parse(frame, {ranges, kDaveMaximumClearRanges}, &view);
    if (!parsed.ok()) {
        auto result = fail(DaveSenderError::invalid_frame);
        result.parse_error = parsed.error;
        return result;
    }
    if (output.count < view.body_bytes)
        return fail(DaveSenderError::output_capacity);
    const u32 needed = view.clear_bytes || frame.data == output.data ? view.body_bytes : 0;
    if (scratch.count < needed)
        return fail(DaveSenderError::scratch_capacity);
    // The bytes this call writes (output and scratch prefixes) must stay clear of
    // everything owned and of each other; the frame is only read. A frame that
    // sat in owned memory could only fail authentication, never corrupt state.
    const ConstSlice<u8> written{output.data, view.body_bytes}, temporary{scratch.data, needed};
    if (touches(sender, written) || aliases_workspace(workspace, written) ||
        (byte_ranges_overlap(written, frame) && frame.data != output.data) ||
        (needed && (touches(sender, temporary) || aliases_workspace(workspace, temporary) ||
                    byte_ranges_overlap(temporary, frame) || byte_ranges_overlap(temporary, written))))
        return fail(DaveSenderError::invalid_input);
    collect(sender, now);
    auto result = fail(sender.newest ? DaveSenderError::rejected : DaveSenderError::no_epoch);
    for (auto* node = sender.newest; node; node = node->older) {
        ++result.epochs_tried;
        result.epoch =
            dave_receive_epoch_open(node->epoch, workspace, view, now, scratch.data, output.data);
        result.crypto_attempts += dave_epoch_attempted(result.epoch) ? 1u : 0u;
        if (result.epoch == DaveEpochError::none) {
            result.revision = node->revision;
            result.bytes = view.body_bytes;
            result.error = DaveSenderError::none;
            return result;
        }
    }
    return result;
}
} // namespace maicord
