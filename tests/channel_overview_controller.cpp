#include "maicord/app/channel_overview_controller.hpp"
namespace maicord {
namespace {
ChannelOverviewSnapshot initial() noexcept {
    ChannelOverviewSnapshot value{};
    value.channel_id = 20;
    value.guild_id = 10;
    value.known_fields = channel_overview_all;
    value.name_bytes = 7;
    copy_bytes(value.name, "general", 7);
    value.topic_bytes = 3;
    copy_bytes(value.topic, "old", 3);
    return value;
}
bool submit(ChannelOverviewController& value, u64 id, u64 now = 1) noexcept {
    const auto request = channel_overview_controller_request(value, now);
    return request.ready && channel_overview_controller_submitted(&value, request, id);
}
} // namespace
void test_channel_overview_controller(void (*check)(bool, const char*) noexcept) noexcept {
    const ChannelOverviewScope scope{1, 2, 3, 10, 20, 1};
    ChannelOverviewController value{};
    const auto remote = initial();
    check(!channel_overview_controller_next_wake(value) &&
              !channel_overview_controller_open(&value, scope, true, false),
          "channel overview controller: dormant owner does no work and cannot open without permission "
          "authority");
    check(channel_overview_controller_open(&value, scope, true, true) && submit(value, 100) &&
              !channel_overview_controller_edit(&value, {utf8_literal("x"), {}, 0, 1, false}) &&
              !channel_overview_controller_complete(&value, scope, 999, {}, &remote, 1) &&
              value.request_id == 100,
          "channel overview controller: loading locks edits and unrelated receipts cannot consume its "
          "exact ticket");
    check(channel_overview_controller_complete(&value, scope, 100, {}, &remote, 1) && value.authoritative &&
              value.phase == ChannelOverviewPhase::ready && !channel_overview_controller_next_wake(value),
          "channel overview controller: authoritative GET publishes owned baseline and retires idle wake");
    check(
        channel_overview_controller_edit(&value, {{}, {}, 0, 1, false}) && value.draft.name_bytes == 0 &&
            !channel_overview_controller_save(&value) && value.failure == ChannelOverviewFailure::input,
        "channel overview controller: empty inline name is a valid transient draft but cannot reach PATCH");
    copy_bytes(value.draft.name, "renamed", 7);
    check(channel_overview_controller_edit(&value, {{value.draft.name, 7}, {}, 0, 1, false}) &&
              value.dirty_fields == 1 &&
              utf8_equal(channel_overview_name_view(value.baseline), utf8_literal("general")) &&
              channel_overview_controller_save(&value),
          "channel overview controller: borrowed inline buffer edits compare against baseline without a "
          "third string bank");
    const auto prepared = channel_overview_controller_request(value, 1);
    check(prepared.ready && prepared.patch.fields == 1 && prepared.patch.name.data == value.draft.name &&
              !channel_overview_controller_edit(&value, {utf8_literal("later"), {}, 0, 1, false}) &&
              !channel_overview_controller_reset(&value) && submit(value, 101),
          "channel overview controller: queued and submitted Save lock a dirty-only borrowed request until "
          "exact consumption");
    auto accepted = remote;
    accepted.name_bytes = 7;
    copy_bytes(accepted.name, "renamed", 7);
    check(channel_overview_controller_complete(&value, scope, 101, {}, &accepted, 2) &&
              !value.dirty_fields &&
              utf8_equal(channel_overview_name_view(value.baseline), utf8_literal("renamed")),
          "channel overview controller: successful normalized server response commits baseline and draft "
          "together");
    check(channel_overview_controller_edit(&value, {{}, utf8_literal("mine"), 0, 2, false}) &&
              channel_overview_controller_save(&value) && submit(value, 102),
          "channel overview controller: conflict fixture submits one topic edit");
    auto newer = accepted;
    newer.topic_bytes = 6;
    copy_bytes(newer.topic, "remote", 6);
    check(channel_overview_controller_invalidate(&value, scope) &&
              channel_overview_controller_complete(&value, scope, 102, {}, &accepted, 3) &&
              value.phase == ChannelOverviewPhase::refetch &&
              value.operation == ChannelOverviewOperation::fetch &&
              utf8_equal(channel_overview_topic_view(value.draft), utf8_literal("mine")) && submit(value, 103),
          "channel overview controller: newer Gateway metadata rejects an older REST baseline and "
          "refetches without losing draft");
    check(channel_overview_controller_complete(&value, scope, 103, {}, &newer, 4) &&
              value.phase == ChannelOverviewPhase::conflict && !channel_overview_controller_save(&value) &&
              channel_overview_controller_rebase(&value) && channel_overview_controller_save(&value) &&
              submit(value, 104),
          "channel overview controller: conflict requires explicit reviewed rebase before another Save");
    check(channel_overview_controller_complete(&value, scope, 104,
                                               {ChannelOverviewCompletionKind::ambiguous}, nullptr, 5) &&
              value.phase == ChannelOverviewPhase::refetch &&
              value.operation == ChannelOverviewOperation::fetch &&
              !channel_overview_controller_save(&value) && submit(value, 105),
          "channel overview controller: ambiguous PATCH never blindly replays and requires authoritative "
          "fetch first");
    check(channel_overview_controller_complete(&value, scope, 105, {}, &newer, 6) &&
              value.dirty_fields == 2 && value.phase == ChannelOverviewPhase::ready &&
              !channel_overview_controller_next_wake(value) && channel_overview_controller_save(&value) &&
              submit(value, 106),
          "channel overview controller: unchanged refresh preserves reviewable draft but does not "
          "automatically resubmit it");
    check(
        channel_overview_controller_complete(
            &value, scope, 106, {ChannelOverviewCompletionKind::permission_lost}, nullptr, 7) &&
            value.phase == ChannelOverviewPhase::permission_lost && value.dirty_fields == 2 &&
            !value.can_manage,
        "channel overview controller: known permission rejection retains draft and disables further edits");
    channel_overview_controller_authority(&value, scope, true, true);
    check(!value.can_manage &&
              !channel_overview_controller_edit(&value, {{}, utf8_literal("other"), 0, 2, false}),
          "channel overview controller: unchanged cached permission flags cannot clear a server403 latch");
    channel_overview_controller_reauthorize(&value, scope, true, true);
    check(value.phase == ChannelOverviewPhase::refetch && submit(value, 107) &&
              channel_overview_controller_complete(&value, scope, 107, {}, &newer, 8) &&
              channel_overview_controller_save(&value) && submit(value, 108),
          "channel overview controller: fresh permission proof triggers refetch before explicit retry "
          "admission");
    check(channel_overview_controller_complete(
              &value, scope, 108, {ChannelOverviewCompletionKind::rate_limited, 1000}, nullptr, 9) &&
              channel_overview_controller_next_wake(value) == 1000 &&
              !channel_overview_controller_request(value, 999).ready && submit(value, 109, 1000),
          "channel overview controller: known429 honors an absolute future deadline without an immediate "
          "polling retry");
    check(
        channel_overview_controller_complete(
            &value, scope, 109, {ChannelOverviewCompletionKind::rate_limited, 1000}, nullptr, 1000) &&
            value.phase == ChannelOverviewPhase::failed && !channel_overview_controller_next_wake(value),
        "channel overview controller: missing or overdue rate deadline fails without a zero-timeout loop");
    check(channel_overview_controller_reload(&value) && submit(value, 110),
          "channel overview controller: cancelled receipt fixture has an exact fetch ticket");
    auto successor = scope;
    successor.account_scope = 2;
    channel_overview_controller_authority(&value, successor, true, true);
    check(value.phase == ChannelOverviewPhase::retired && value.request_id == 110 &&
              !channel_overview_controller_open(&value, successor, true, true) &&
              !channel_overview_controller_complete(&value, successor, 110, {}, &remote, 10) &&
              channel_overview_controller_complete(
                  &value, scope, 110, {ChannelOverviewCompletionKind::cancelled}, nullptr, 10) &&
              !value.request_id && !channel_overview_controller_next_wake(value),
          "channel overview controller: scope replacement preserves exact old-ticket drainage and forbids "
          "reuse until consumed");
    check(channel_overview_controller_open(&value, successor, true, true) && submit(value, 111) &&
              channel_overview_controller_complete(&value, successor, 111, {}, &remote, 11) &&
              channel_overview_controller_edit(&value, {{}, {}, 5, 4, false}) &&
              channel_overview_controller_reset(&value) && !value.dirty_fields &&
              value.draft.slowmode_seconds == 0,
          "channel overview controller: new scope starts from fresh authority and explicit Reset discards "
          "staged changes");
    channel_overview_controller_authority(&value, successor, false, true);
    check(channel_overview_controller_reload(&value) &&
              !channel_overview_controller_request(value, 12).ready &&
              !channel_overview_controller_next_wake(value),
          "channel overview controller: same-account disconnect pauses admission and scheduling without "
          "losing owner state");
    channel_overview_controller_cancel(&value);
    ChannelOverviewController receipt{};
    check(channel_overview_controller_open(&receipt, scope, true, true) && submit(receipt, 200) &&
              channel_overview_controller_complete(&receipt, scope, 200, {}, &remote, 1) &&
              channel_overview_controller_edit(&receipt, {{}, {}, 5, 4, false}) &&
              channel_overview_controller_save(&receipt),
          "channel overview controller: admission/invalidation fixture stages a real dirty slowmode edit");
    const auto refused = channel_overview_controller_request(receipt, 1);
    check(
        channel_overview_controller_admission_failed(&receipt, refused, ChannelOverviewFailure::rejected) &&
            !receipt.queued && receipt.dirty_fields == 4 &&
            !channel_overview_controller_next_wake(receipt) && channel_overview_controller_save(&receipt) &&
            submit(receipt, 201),
        "channel overview controller: synchronous refusal retires admission without losing draft or "
        "polling forever");
    check(channel_overview_controller_invalidate(&receipt, scope) && !receipt.authoritative &&
              channel_overview_controller_complete(
                  &receipt, scope, 201, {ChannelOverviewCompletionKind::rate_limited, 1000}, nullptr, 2) &&
              receipt.operation == ChannelOverviewOperation::fetch && submit(receipt, 202) &&
              channel_overview_controller_complete(&receipt, scope, 202, {}, &remote, 3) &&
              receipt.dirty_fields == 4 && !receipt.queued,
          "channel overview controller: binary Gateway invalidation fences even a429 response and "
          "refreshes before another explicit PATCH");
    channel_overview_controller_cancel(&receipt);
    ChannelOverviewController reopened{};
    check(channel_overview_controller_open(&reopened, scope, true, true),
          "channel overview controller: same-scope reopening fixture has an initial queued request");
    const auto before_close = channel_overview_controller_request(reopened, 12);
    channel_overview_controller_cancel(&reopened);
    check(channel_overview_controller_open(&reopened, scope, true, true) &&
              !channel_overview_controller_submitted(&reopened, before_close, 300) && submit(reopened, 302),
          "channel overview controller: same-scope reopening advances admission revision and rejects "
          "captured requests from a retired lifetime");
    channel_overview_controller_cancel(&reopened);
}
} // namespace maicord
