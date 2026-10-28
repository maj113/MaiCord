#include "maicord/app/channel_overview_controller.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool scope_valid(ChannelOverviewScope scope) noexcept {
    return scope.account_scope && scope.profile_id && scope.self_user_id && scope.guild_id &&
           scope.channel_id && scope.channel_generation;
}
ChannelOverviewPatch patch(const ChannelOverviewSnapshot& value, u8 fields) noexcept {
    return {channel_overview_name_view(value), channel_overview_topic_view(value), value.slowmode_seconds,
            fields, value.nsfw};
}
bool snapshot_valid(const ChannelOverviewSnapshot& value, ChannelOverviewScope scope) noexcept {
    return value.channel_id == scope.channel_id && value.guild_id == scope.guild_id && value.type == 0 &&
           value.name_bytes <= kChannelOverviewNameBytes &&
           value.topic_bytes <= kChannelOverviewTopicBytes && value.known_fields == channel_overview_all &&
           channel_overview_patch_valid(patch(value, channel_overview_all));
}
void apply(ChannelOverviewSnapshot& value, const ChannelOverviewPatch& input) noexcept {
    if (input.fields & channel_overview_name) {
        copy_bytes(value.name, input.name.data, input.name.bytes);
        value.name_bytes = input.name.bytes;
    }
    if (input.fields & channel_overview_topic) {
        copy_bytes(value.topic, input.topic.data, input.topic.bytes);
        value.topic_bytes = input.topic.bytes;
    }
    if (input.fields & channel_overview_slowmode)
        value.slowmode_seconds = input.slowmode_seconds;
    if (input.fields & channel_overview_nsfw)
        value.nsfw = input.nsfw;
    value.known_fields |= input.fields;
}
u8 difference(const ChannelOverviewSnapshot& a, const ChannelOverviewSnapshot& b, u8 fields) noexcept {
    u8 changed = static_cast<u8>((a.known_fields ^ b.known_fields) & fields);
    if ((fields & channel_overview_name) &&
        !utf8_equal(channel_overview_name_view(a), channel_overview_name_view(b)))
        changed |= channel_overview_name;
    if ((fields & channel_overview_topic) &&
        !utf8_equal(channel_overview_topic_view(a), channel_overview_topic_view(b)))
        changed |= channel_overview_topic;
    if ((fields & channel_overview_slowmode) && a.slowmode_seconds != b.slowmode_seconds)
        changed |= channel_overview_slowmode;
    if ((fields & channel_overview_nsfw) && a.nsfw != b.nsfw)
        changed |= channel_overview_nsfw;
    return changed;
}
void revise(ChannelOverviewController& value) noexcept {
    if (value.revision != ~u64{0})
        ++value.revision;
}
void fetch(ChannelOverviewController& value) noexcept {
    value.operation = ChannelOverviewOperation::fetch;
    value.queued = true;
    value.retry_at_ms = 0;
    value.authoritative = false;
    value.phase = value.has_baseline ? ChannelOverviewPhase::refetch : ChannelOverviewPhase::loading;
}
bool editable(const ChannelOverviewController& value) noexcept {
    return value.active && value.available && value.can_manage && value.authoritative &&
           !value.request_id && !value.queued;
}
void install_remote(ChannelOverviewController& value, const ChannelOverviewSnapshot& remote) noexcept {
    const u8 dirty = value.dirty_fields;
    const u8 changes = difference(value.baseline, remote, channel_overview_all);
    apply(value.baseline, patch(remote, channel_overview_all));
    apply(value.draft, patch(remote, static_cast<u8>(channel_overview_all & ~dirty)));
    value.dirty_fields = difference(value.baseline, value.draft, channel_overview_all);
    value.conflict_fields =
        static_cast<u8>((value.conflict_fields | (changes & dirty)) & value.dirty_fields);
    if (value.conflict_fields) {
        value.phase = ChannelOverviewPhase::conflict;
        value.failure = ChannelOverviewFailure::conflict;
    }
}
} // namespace
bool channel_overview_scope_equal(ChannelOverviewScope a, ChannelOverviewScope b) noexcept {
    return a.account_scope == b.account_scope && a.profile_id == b.profile_id &&
           a.self_user_id == b.self_user_id && a.guild_id == b.guild_id && a.channel_id == b.channel_id &&
           a.channel_generation == b.channel_generation;
}
bool channel_overview_controller_open(ChannelOverviewController* value, ChannelOverviewScope scope,
                                      bool available, bool can_manage) noexcept {
    if (!value || value->request_id || !scope_valid(scope) || !can_manage || value->revision == ~u64{0})
        return false;
    const u64 revision = value->revision;
    reset_value(value);
    value->revision = revision;
    value->scope = scope;
    value->active = true;
    value->available = available;
    value->can_manage = can_manage;
    value->baseline.channel_id = value->draft.channel_id = scope.channel_id;
    value->baseline.guild_id = value->draft.guild_id = scope.guild_id;
    fetch(*value);
    revise(*value);
    return true;
}
void channel_overview_controller_cancel(ChannelOverviewController* value) noexcept {
    if (!value)
        return;
    value->active = false;
    value->queued = false;
    value->retry_at_ms = 0;
    value->phase = ChannelOverviewPhase::retired;
    value->failure = ChannelOverviewFailure::cancelled;
    revise(*value);
}
void channel_overview_controller_authority(ChannelOverviewController* value, ChannelOverviewScope scope,
                                           bool available, bool can_manage) noexcept {
    if (!value || !value->active)
        return;
    if (!scope_valid(scope) || !channel_overview_scope_equal(value->scope, scope)) {
        channel_overview_controller_cancel(value);
        return;
    }
    const bool was_manage = value->can_manage;
    value->available = available;
    value->can_manage = can_manage && !value->permission_denied;
    if (!value->can_manage) {
        value->phase = ChannelOverviewPhase::permission_lost;
        value->failure = ChannelOverviewFailure::permissions;
        value->queued = false;
        value->authoritative = false;
    } else if (!was_manage && !value->request_id)
        fetch(*value);
}
void channel_overview_controller_reauthorize(ChannelOverviewController* value, ChannelOverviewScope scope,
                                             bool available, bool can_manage) noexcept {
    if (!value || !value->active || !scope_valid(scope) ||
        !channel_overview_scope_equal(value->scope, scope))
        return;
    value->permission_denied = !can_manage;
    channel_overview_controller_authority(value, scope, available, can_manage);
}
bool channel_overview_controller_edit(ChannelOverviewController* value,
                                      const ChannelOverviewPatch& input) noexcept {
    if (!value || !editable(*value))
        return false;
    auto validated = input;
    if ((input.fields & channel_overview_name) && !input.name.bytes)
        validated.name = utf8_literal("_");
    if (!channel_overview_patch_valid(validated))
        return false;
    ChannelOverviewSnapshot candidate = value->draft;
    apply(candidate, input); // Source may borrow draft itself; commit only after copying.
    // An inline model can already have edited the borrowed draft bytes. Always
    // compare the staged value with baseline, not those already-mutated bytes.
    value->draft = candidate;
    value->dirty_fields = difference(value->baseline, value->draft, channel_overview_all);
    value->conflict_fields &= value->dirty_fields;
    if (!value->conflict_fields) {
        value->phase = ChannelOverviewPhase::ready;
        value->failure = ChannelOverviewFailure::none;
    }
    revise(*value);
    return true;
}
bool channel_overview_controller_save(ChannelOverviewController* value) noexcept {
    if (!value || !editable(*value) || value->phase == ChannelOverviewPhase::conflict ||
        !value->dirty_fields)
        return false;
    if (!channel_overview_patch_valid(patch(value->draft, value->dirty_fields))) {
        value->phase = ChannelOverviewPhase::failed;
        value->failure = ChannelOverviewFailure::input;
        revise(*value);
        return false;
    }
    value->operation = ChannelOverviewOperation::update;
    value->queued = true;
    value->retry_at_ms = 0;
    value->phase = ChannelOverviewPhase::saving;
    value->failure = ChannelOverviewFailure::none;
    value->rate_attempts = 0;
    revise(*value);
    return true;
}
bool channel_overview_controller_reset(ChannelOverviewController* value) noexcept {
    if (!value || !value->active || !value->has_baseline || value->request_id || value->queued)
        return false;
    value->draft = value->baseline;
    value->dirty_fields = value->conflict_fields = 0;
    value->failure = ChannelOverviewFailure::none;
    value->phase = value->can_manage && value->authoritative ? ChannelOverviewPhase::ready
                                                             : ChannelOverviewPhase::permission_lost;
    revise(*value);
    return true;
}
bool channel_overview_controller_reload(ChannelOverviewController* value) noexcept {
    if (!value || !value->active || value->request_id)
        return false;
    fetch(*value);
    value->rate_attempts = 0;
    revise(*value);
    return true;
}
bool channel_overview_controller_rebase(ChannelOverviewController* value) noexcept {
    if (!value || !editable(*value) || value->phase != ChannelOverviewPhase::conflict)
        return false;
    value->phase = ChannelOverviewPhase::ready;
    value->failure = ChannelOverviewFailure::none;
    value->conflict_fields = 0;
    revise(*value);
    return true;
}
ChannelOverviewRequest channel_overview_controller_request(const ChannelOverviewController& value,
                                                           u64 now_ms) noexcept {
    if (!value.active || !value.available || !value.queued || value.request_id ||
        (value.retry_at_ms && value.retry_at_ms > now_ms) ||
        (value.operation == ChannelOverviewOperation::update &&
         (!value.can_manage || !value.authoritative ||
          !channel_overview_patch_valid(patch(value.draft, value.dirty_fields)))))
        return {};
    return {value.scope,
            value.operation == ChannelOverviewOperation::update ? patch(value.draft, value.dirty_fields)
                                                                : ChannelOverviewPatch{},
            value.revision,
            value.remote_revision,
            value.operation,
            true};
}
bool channel_overview_controller_submitted(ChannelOverviewController* value,
                                           const ChannelOverviewRequest& request, u64 request_id) noexcept {
    if (!value || !request.ready || !request_id || value->request_id || !value->active || !value->queued ||
        !value->available || !channel_overview_scope_equal(value->scope, request.scope) ||
        request.revision != value->revision || request.remote_revision != value->remote_revision ||
        request.operation != value->operation)
        return false;
    value->request_id = request_id;
    value->submitted_remote_revision = request.remote_revision;
    value->queued = false;
    value->retry_at_ms = 0;
    value->phase = request.operation == ChannelOverviewOperation::update ? ChannelOverviewPhase::saving
                                                                         : ChannelOverviewPhase::loading;
    return true;
}
bool channel_overview_controller_admission_failed(ChannelOverviewController* value,
                                                  const ChannelOverviewRequest& request,
                                                  ChannelOverviewFailure failure) noexcept {
    if (!value || !request.ready || !value->active || value->request_id || !value->queued ||
        !channel_overview_scope_equal(value->scope, request.scope) || request.revision != value->revision ||
        request.remote_revision != value->remote_revision || request.operation != value->operation)
        return false;
    value->queued = false;
    value->retry_at_ms = 0;
    value->phase = ChannelOverviewPhase::failed;
    value->failure = failure == ChannelOverviewFailure::none ? ChannelOverviewFailure::input : failure;
    revise(*value);
    return true;
}
bool channel_overview_controller_invalidate(ChannelOverviewController* value,
                                            ChannelOverviewScope scope) noexcept {
    if (!value || !value->active || !channel_overview_scope_equal(value->scope, scope))
        return false;
    if (value->remote_revision == ~u64{0}) {
        channel_overview_controller_cancel(value);
        return false;
    }
    ++value->remote_revision;
    value->authoritative = false;
    if (!value->request_id)
        fetch(*value);
    else
        value->phase = ChannelOverviewPhase::refetch;
    revise(*value);
    return true;
}
bool channel_overview_controller_complete(ChannelOverviewController* value, ChannelOverviewScope scope,
                                          u64 request_id, ChannelOverviewCompletion completion,
                                          const ChannelOverviewSnapshot* remote, u64 now_ms) noexcept {
    if (!value || !request_id || value->request_id != request_id ||
        !channel_overview_scope_equal(value->scope, scope))
        return false;
    value->request_id = 0;
    if (!value->active)
        return true;
    const auto operation = value->operation;
    value->queued = false;
    value->retry_at_ms = 0;
    if (completion.kind == ChannelOverviewCompletionKind::permission_lost) {
        value->can_manage = false;
        value->permission_denied = true;
        value->authoritative = false;
        value->phase = ChannelOverviewPhase::permission_lost;
        value->failure = ChannelOverviewFailure::permissions;
    } else if (value->remote_revision != value->submitted_remote_revision) {
        value->failure = ChannelOverviewFailure::conflict;
        fetch(*value);
    } else if (completion.kind == ChannelOverviewCompletionKind::rate_limited) {
        value->failure = ChannelOverviewFailure::rate_limited;
        // Honor an actual future deadline, with bounded attempts and no invented
        // immediate fallback. An implausible deadline requires a manual reload.
        if ((operation == ChannelOverviewOperation::fetch || value->can_manage) &&
            value->rate_attempts < 3 && completion.retry_at_ms > now_ms &&
            completion.retry_at_ms - now_ms <= 86'400'000) {
            ++value->rate_attempts;
            value->queued = true;
            value->retry_at_ms = completion.retry_at_ms;
            value->phase = operation == ChannelOverviewOperation::update ? ChannelOverviewPhase::saving
                                                                         : ChannelOverviewPhase::loading;
        } else
            value->phase = ChannelOverviewPhase::failed;
    } else if (completion.kind != ChannelOverviewCompletionKind::success) {
        value->failure = completion.kind == ChannelOverviewCompletionKind::rejected
                             ? ChannelOverviewFailure::rejected
                         : completion.kind == ChannelOverviewCompletionKind::cancelled
                             ? ChannelOverviewFailure::cancelled
                             : ChannelOverviewFailure::ambiguous;
        if (operation == ChannelOverviewOperation::update &&
            completion.kind != ChannelOverviewCompletionKind::rejected)
            fetch(*value);
        else
            value->phase = ChannelOverviewPhase::failed;
    } else if (!remote || !snapshot_valid(*remote, scope)) {
        value->failure = ChannelOverviewFailure::response;
        if (operation == ChannelOverviewOperation::update)
            fetch(*value);
        else {
            value->authoritative = false;
            value->phase = ChannelOverviewPhase::failed;
        }
    } else {
        value->phase = ChannelOverviewPhase::ready;
        value->failure = ChannelOverviewFailure::none;
        if (!value->has_baseline || operation == ChannelOverviewOperation::update) {
            value->baseline = value->draft = *remote;
            value->dirty_fields = value->conflict_fields = 0;
        } else
            install_remote(*value, *remote);
        value->has_baseline = value->authoritative = true;
        if (!value->can_manage) {
            value->phase = ChannelOverviewPhase::permission_lost;
            value->failure = ChannelOverviewFailure::permissions;
        }
        value->rate_attempts = 0;
    }
    revise(*value);
    return true;
}
u64 channel_overview_controller_next_wake(const ChannelOverviewController& value) noexcept {
    if (!value.active || !value.available || value.request_id || !value.queued ||
        (value.operation == ChannelOverviewOperation::update && !value.can_manage))
        return 0;
    return value.retry_at_ms ? value.retry_at_ms : 1;
}
} // namespace maicord
