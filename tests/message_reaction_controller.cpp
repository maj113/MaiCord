#include "maicord/app/message_reaction_controller.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace reaction_controller_probe {
struct Fixture {
    MessageReactionController controller{};
    MessageReactionIntent storage[kMessageReactionMaximumIntents]{};
    MessageReactionActor actor{1, 2, 3, 4};
    MessageReactionSource source{{7, 1}, 8, 9};
    MessageReactionAuthority initial{4, 4, 0, false, false, true, true};
    u64 request{};
    bool reset(u32 backed = kMessageReactionMaximumIntents) noexcept {
        request = 0;
        return message_reaction_controller_initialize(&controller, {storage, backed}, actor);
    }
    MessageReactionTarget target(u64 emoji = 5, u64 channel = 6) noexcept {
        MessageReactionTarget target{};
        target.channel_id = channel;
        target.message_id = 10;
        target.emoji_id = emoji;
        target.name[0] = 'a';
        target.name_bytes = 1;
        return target;
    }
    MessageReactionOutcome choose(const MessageReactionTarget& target, bool desired,
                                  u64 now_ms = 0) noexcept {
        return message_reaction_controller_queue(&controller, target, source, initial, desired, now_ms);
    }
    MessageReactionTicket send(u64 now_ms = 0) noexcept {
        MessageReactionIntent pending{};
        MessageReactionTicket ticket{};
        if (message_reaction_controller_peek_due(controller, now_ms, &pending))
            message_reaction_controller_admit(&controller, pending, ++request, now_ms, &ticket);
        return ticket;
    }
};
Fixture fixture{};
// Independent projected model: authoritative foreigners are not adjusted by
// local choice; the current user's contribution is exactly zero or one.
struct Model {
    u32 foreign_normal{4}, foreign_burst{};
    bool mine_normal{};
    bool mine_burst{};
    bool matches(const MessageReactionProjection& projected) const noexcept {
        const auto normal = projected.authority.normal + (projected.desired_normal ? 1u : 0u) -
                            (projected.authority.mine_normal ? 1u : 0u);
        const auto burst = projected.authority.burst + (projected.desired_burst ? 1u : 0u) -
                           (projected.authority.mine_burst ? 1u : 0u);
        return normal == foreign_normal + (mine_normal ? 1u : 0u) &&
               burst == foreign_burst + (mine_burst ? 1u : 0u);
    }
};
} // namespace reaction_controller_probe
void test_message_reaction_controller(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace reaction_controller_probe;
    check(fixture.reset(1), "reaction intent: demand-backed initialization accepts one slot");
    auto target = fixture.target();
    auto other_target = fixture.target(11, 12);
    const auto queued = fixture.choose(target, true);
    check(queued.ok() && fixture.controller.count == 1 && queued.projection.desired_normal,
          "reaction intent: copied desired add accepted before model projection");
    target.name[0] = 'z';
    check(fixture.controller.entries[0].target.name[0] == 'a',
          "reaction intent: route name copied from caller storage");
    auto ticket = fixture.send();
    const auto serial = fixture.controller.serial;
    check(fixture.choose(other_target, true).error == MessageReactionError::capacity &&
              fixture.controller.serial == serial && fixture.controller.flight.active,
          "reaction intent: capacity failure preserves submitted owner and existing desired");
    check(message_reaction_controller_grow_storage(&fixture.controller, {fixture.storage, 2}) &&
              fixture.choose(other_target, true).ok() && fixture.controller.count == 2,
          "reaction intent: stable backed prefix can grow while one request owns flight");
    MessageReactionIntent pending{};
    check(!message_reaction_controller_peek_due(fixture.controller, 0, &pending),
          "reaction intent: exactly one HTTP flight owns submission");
    fixture.choose(target, false);
    auto result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                       MessageReactionCompletion::success, 1);
    check(result.kind == MessageReactionOutcomeKind::retained && !result.projection.desired_normal &&
              result.projection.authority.mine_normal,
          "reaction intent: old add success preserves newer remove against confirmed baseline");
    check(fixture.controller.count == 2 &&
              message_reaction_controller_peek_due(fixture.controller, 1, &pending) &&
              pending.target.channel_id == 12,
          "reaction intent: distinct queued channel survives earlier completion");
    auto second = fixture.send(1);
    message_reaction_controller_complete(&fixture.controller, second, MessageReactionCompletion::success,
                                         2);
    auto remove = fixture.send(2);
    result = message_reaction_controller_complete(&fixture.controller, remove,
                                                  MessageReactionCompletion::success, 3);
    check(result.kind == MessageReactionOutcomeKind::settled && !result.projection.authority.mine_normal &&
              fixture.controller.count == 2 && fixture.controller.entries[0].awaiting_echo &&
              fixture.controller.entries[1].awaiting_echo &&
              !message_reaction_controller_peek_due(fixture.controller, 3, &pending),
          "reaction intent: final desired remove settles without rolling back successful distinct target");

    fixture.reset();
    target = fixture.target();
    fixture.choose(target, true);
    ticket = fixture.send();
    fixture.choose(target, false);
    fixture.choose(target, true);
    result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                  MessageReactionCompletion::success, 1);
    check(result.kind == MessageReactionOutcomeKind::settled && fixture.controller.count == 1 &&
              fixture.controller.entries[0].awaiting_echo && result.projection.authority.normal == 5,
          "reaction intent: add remove add coalesces latest desired into one successful add");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    fixture.choose(target, false);
    check(message_reaction_controller_observe(&fixture.controller, target, true, true),
          "reaction intent: validated own echo updates retained baseline");
    MessageReactionProjection projected{};
    message_reaction_controller_project(fixture.controller, target, &projected);
    Model model{};
    check(model.matches(projected) && projected.authority.normal == 5,
          "reaction intent: old own add echo never changes newer displayed remove");
    message_reaction_controller_observe(&fixture.controller, target, true, true);
    message_reaction_controller_project(fixture.controller, target, &projected);
    check(projected.authority.normal == 5,
          "reaction intent: duplicate own echo is idempotent independently of optimistic model");
    result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                  MessageReactionCompletion::success, 1);
    check(result.projection.authority.normal == 5 && result.kind == MessageReactionOutcomeKind::retained,
          "reaction intent: HTTP after matching own echo does not double count");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    message_reaction_controller_observe(&fixture.controller, target, true, false);
    model.foreign_normal = 5;
    model.mine_normal = true;
    result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                  MessageReactionCompletion::success, 1);
    check(model.matches(result.projection) && result.projection.authority.normal == 6,
          "reaction intent: foreign count update survives HTTP own membership confirmation");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    message_reaction_controller_observe(&fixture.controller, target, false, true);
    result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                  MessageReactionCompletion::success, 1);
    check(result.authority_conflict && !result.projection.authority.mine_normal &&
              result.projection.desired_normal,
          "reaction intent: newer own Gateway authority fences stale HTTP membership");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                  MessageReactionCompletion::terminal, 1);
    check(result.kind == MessageReactionOutcomeKind::failed && !result.projection.optimistic &&
              result.projection.authority.normal == 4 && !fixture.controller.count,
          "reaction intent: terminal current failure restores retained authority only");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    fixture.choose(target, false);
    fixture.choose(target, true);
    result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                  MessageReactionCompletion::terminal, 1);
    check(result.kind == MessageReactionOutcomeKind::retained && fixture.controller.count == 1 &&
              result.projection.desired_normal,
          "reaction intent: terminal older serial cannot erase fresh same-target action");

    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    auto stale_ticket = ticket;
    ++stale_ticket.request_id;
    check(message_reaction_controller_complete(&fixture.controller, stale_ticket,
                                               MessageReactionCompletion::success, 1)
                      .kind == MessageReactionOutcomeKind::ignored &&
              fixture.controller.flight.active,
          "reaction intent: foreign response ID cannot release flight ownership");
    message_reaction_controller_cancel(&fixture.controller, target.channel_id, target.message_id, &target);
    fixture.choose(target, true);
    result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                  MessageReactionCompletion::success, 1);
    check(result.kind == MessageReactionOutcomeKind::retired && !result.project &&
              fixture.controller.count == 1 && !fixture.controller.entries[0].authority.mine_normal,
          "reaction intent: administrative barrier prevents old response mutating reused identity");
    ticket = fixture.send(1);
    auto actor = fixture.actor;
    ++actor.account_scope;
    message_reaction_controller_reset(&fixture.controller, actor);
    fixture.choose(target, true, 2);
    check(fixture.controller.flight.active && fixture.controller.flight.retired &&
              !message_reaction_controller_peek_due(fixture.controller, 2, &pending),
          "reaction intent: new actor accepts queued choice but waits for old transport drain");
    result =
        message_reaction_controller_complete(&fixture.controller, ticket, MessageReactionCompletion::retry,
                                             3, 999999, MessageReactionRetryScope::global);
    check(!fixture.controller.flight.active && !fixture.controller.global_cooldown_ms &&
              !fixture.controller.entries[0].authority.mine_normal,
          "reaction intent: retired old actor cannot inject cooldown or rollback into new actor");
    ticket = fixture.send(3);
    stale_ticket = ticket;
    --stale_ticket.generation;
    check(message_reaction_controller_complete(&fixture.controller, stale_ticket,
                                               MessageReactionCompletion::success, 4)
                  .kind == MessageReactionOutcomeKind::ignored,
          "reaction intent: request ID reuse still requires exact generation and captured serial");

    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    message_reaction_controller_complete(&fixture.controller, ticket, MessageReactionCompletion::retry, 1,
                                         50000, MessageReactionRetryScope::channel);
    fixture.choose(other_target, true, 2);
    check(message_reaction_controller_peek_due(fixture.controller, 2, &pending) &&
              pending.target.channel_id == other_target.channel_id,
          "reaction intent: channel retry floor does not starve eligible other channel");
    ticket = fixture.send(2);
    message_reaction_controller_complete(&fixture.controller, ticket, MessageReactionCompletion::success,
                                         3);
    message_reaction_controller_cancel(&fixture.controller, target.channel_id, target.message_id, &target);
    fixture.choose(target, true, 4);
    u64 due{};
    check(message_reaction_controller_next_due(fixture.controller, &due) && due == 50000 &&
              !message_reaction_controller_peek_due(fixture.controller, 49999, &pending),
          "reaction intent: cancel and requeue cannot shorten retained server channel cooldown");
    ticket = fixture.send(50000);
    message_reaction_controller_complete(&fixture.controller, ticket, MessageReactionCompletion::retry,
                                         50001, 100000, MessageReactionRetryScope::global);
    fixture.choose(other_target, true, 50002);
    check(!message_reaction_controller_peek_due(fixture.controller, 99999, &pending),
          "reaction intent: global floor applies to distinct channels");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    message_reaction_controller_complete(&fixture.controller, ticket, MessageReactionCompletion::retry, 1,
                                         1000000, MessageReactionRetryScope::channel);
    check(message_reaction_controller_next_due(fixture.controller, &due) &&
              due == kMessageReactionIntentLifetimeMs,
          "reaction intent: long Retry-After never extends finite intent deadline");
    MessageReactionOutcome expired{};
    check(message_reaction_controller_expire(&fixture.controller, kMessageReactionIntentLifetimeMs,
                                             &expired) &&
              expired.kind == MessageReactionOutcomeKind::expired && !fixture.controller.count,
          "reaction intent: queued deadline expires projected ownership");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    message_reaction_controller_expire(&fixture.controller, kMessageReactionIntentLifetimeMs, &expired);
    check(fixture.controller.flight.active && fixture.controller.flight.retired &&
              !fixture.controller.count,
          "reaction intent: deadline expiry retains borrowed flight until exact response drains");
    result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                  MessageReactionCompletion::success,
                                                  kMessageReactionIntentLifetimeMs + 1);
    check(!fixture.controller.flight.active && !result.project,
          "reaction intent: expired completion cannot resurrect optimistic reaction");
    fixture.reset();
    fixture.choose(target, true);
    for (u32 index = 0; index <= kMessageReactionMaximumRetries; ++index) {
        ticket = fixture.send(index * 10000);
        result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                      MessageReactionCompletion::retry, index * 10000 + 1);
    }
    check(result.kind == MessageReactionOutcomeKind::failed && !fixture.controller.count,
          "reaction intent: repeated transient failure has bounded retry budget");

    fixture.reset();
    auto renamed = target;
    renamed.name[0] = 'q';
    renamed.animated = true;
    check(message_reaction_target_equal(target, renamed),
          "reaction intent: custom emoji rename and animation retain snowflake identity");
    auto nullname = target;
    nullname.name_bytes = 0;
    fixture.choose(target, true);
    check(message_reaction_controller_observe(&fixture.controller, nullname, true, true),
          "reaction intent: custom Gateway null name matches retained ID identity");
    check(fixture.choose(nullname, true).error == MessageReactionError::invalid_input,
          "reaction intent: outbound custom route still needs actual name metadata");
    auto unicode = target;
    unicode.emoji_id = 0;
    unicode.name_bytes = 3;
    copy_bytes(unicode.name, "\xE2\x9D\xA4", 3);
    auto variation = unicode;
    variation.name_bytes = 6;
    copy_bytes(variation.name + 3, "\xEF\xB8\x8F", 3);
    check(
        !message_reaction_target_equal(unicode, variation),
        "reaction intent: Unicode exact bytes preserve variation selector without universal normalization");
    auto invalid = unicode;
    invalid.name_bytes = 1;
    check(fixture.choose(invalid, true).error == MessageReactionError::invalid_input,
          "reaction intent: invalid UTF8 refused before mutation");
    fixture.reset();
    fixture.choose(target, true);
    auto burst = target;
    burst.type = MessageReactionType::burst;
    fixture.choose(burst, true);
    message_reaction_controller_project(fixture.controller, target, &projected);
    model = {};
    model.mine_normal = true;
    model.mine_burst = true;
    check(model.matches(projected),
          "reaction intent: normal and burst desired membership share one authoritative aggregate");
    ticket = fixture.send();
    message_reaction_controller_observe(&fixture.controller, burst, true, false);
    result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                  MessageReactionCompletion::success, 1);
    model.foreign_burst = 1;
    check(model.matches(result.projection),
          "reaction intent: independent reaction types preserve foreign burst counts across normal HTTP");
    auto changed = fixture.source;
    ++changed.revision;
    check(message_reaction_controller_rebind_source(&fixture.controller, burst, changed) &&
              fixture.controller.entries[0].source.revision == changed.revision,
          "reaction intent: validated resident source refresh rebinds surviving aggregate");
    auto full = fixture.initial;
    full.total = 12;
    full.normal = 10;
    full.burst = 2;
    check(message_reaction_controller_snapshot(&fixture.controller, burst, full) &&
              fixture.controller.entries[0].authority.total == 12,
          "reaction intent: authoritative history snapshot replaces independent aggregate");
    fixture.reset();
    fixture.choose(target, true);
    message_reaction_controller_peek_due(fixture.controller, 0, &pending);
    fixture.choose(target, false);
    fixture.choose(target, true);
    check(!message_reaction_controller_admit(&fixture.controller, pending, 1, 0, &ticket),
          "reaction intent: stale peek cannot submit superseded serial");
    fixture.reset();
    for (u32 index = 0; index < kMessageReactionMaximumIntents; ++index)
        fixture.choose(fixture.target(100 + index), true);
    check(fixture.controller.count == kMessageReactionMaximumIntents &&
              fixture.choose(fixture.target(1000), true).error == MessageReactionError::capacity,
          "reaction intent: exactly64 copied target intents bound storage and admission");
    message_reaction_controller_cancel(&fixture.controller, 0, 0);
    check(!fixture.controller.count && fixture.choose(target, true).ok(),
          "reaction intent: cleared capacity reusable without identity ABA");
    const auto before = fixture.controller.serial;
    auto bad_type = target;
    bad_type.type = static_cast<MessageReactionType>(255);
    check(fixture.choose(bad_type, true).error == MessageReactionError::invalid_input &&
              fixture.controller.serial == before,
          "reaction intent: invalid reaction enum refuses admission without disturbing latest choice");
    auto bad_source = fixture.source;
    bad_source.handle.generation = 0;
    check(message_reaction_controller_queue(&fixture.controller, target, bad_source, fixture.initial, false,
                                            0)
                      .error == MessageReactionError::invalid_input &&
              fixture.controller.entries[0].desired_mine,
          "reaction intent: stale source generation cannot erase current desired membership");
    auto bad_authority = fixture.initial;
    bad_authority.mine_normal = true;
    bad_authority.normal = 0;
    check(message_reaction_controller_queue(&fixture.controller, target, fixture.source, bad_authority,
                                            false, 0)
                  .error == MessageReactionError::invalid_input,
          "reaction intent: incoherent authoritative mine/count refused before optimistic projection");
    fixture.controller.serial = ~u64{0};
    check(fixture.choose(target, false).error == MessageReactionError::serial_exhausted &&
              fixture.controller.entries[0].desired_mine,
          "reaction intent: exhausted action serial refuses ABA reuse and preserves existing choice");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    auto refreshed = fixture.source;
    ++refreshed.revision;
    check(message_reaction_controller_rebind_source(&fixture.controller, target, refreshed) &&
              fixture.controller.flight.intent.source.revision == refreshed.revision,
          "reaction intent: approved source refresh updates exact submitted fence as well as queued owner");
    full = fixture.initial;
    full.total = 7;
    full.normal = 7;
    full.mine_normal = false;
    message_reaction_controller_snapshot(&fixture.controller, target, full);
    result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                  MessageReactionCompletion::success, 1);
    check(result.authority_conflict && result.projection.authority.normal == 7 &&
              !result.projection.authority.mine_normal,
          "reaction intent: full authoritative snapshot fences earlier HTTP and preserves new foreign "
          "aggregate");
    fixture.reset(2);
    fixture.choose(target, true);
    fixture.choose(burst, true);
    ticket = fixture.send();
    auto source2 = fixture.source;
    ++source2.revision;
    check(message_reaction_controller_queue(&fixture.controller, target, source2, fixture.initial, true, 1)
                  .ok() &&
              fixture.controller.count == 1 && fixture.controller.flight.retired &&
              fixture.controller.entries[0].source.revision == source2.revision,
          "reaction intent: new source retires all older normal and burst choices without requiring spare "
          "capacity");
    result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                  MessageReactionCompletion::success, 2);
    check(!result.project && !fixture.controller.entries[0].authority.mine_normal,
          "reaction intent: source barrier old response cannot claim fresh source authority");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    message_reaction_controller_cancel(&fixture.controller, target.channel_id, target.message_id);
    message_reaction_controller_complete(&fixture.controller, ticket, MessageReactionCompletion::retry, 1,
                                         90000, MessageReactionRetryScope::channel);
    fixture.choose(target, true, 2);
    check(!message_reaction_controller_peek_due(fixture.controller, 89999, &pending),
          "reaction intent: canceled same-actor request still installs actual server retry floor when it "
          "drains");
    message_reaction_controller_reset(&fixture.controller, fixture.actor);
    fixture.choose(target, true, 3);
    check(
        message_reaction_controller_next_due(fixture.controller, &due) && due == 90000 &&
            !message_reaction_controller_peek_due(fixture.controller, 89999, &pending),
        "reaction intent: same-actor READY reset retains server deadline rather than resetting rate owner");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    check(message_reaction_controller_complete(&fixture.controller, ticket,
                                               static_cast<MessageReactionCompletion>(255), 1)
                      .error == MessageReactionError::invalid_input &&
              fixture.controller.flight.active,
          "reaction intent: invalid completion classification cannot drain captured transport owner");
    result = message_reaction_controller_complete(
        &fixture.controller, ticket, MessageReactionCompletion::success, kMessageReactionIntentLifetimeMs);
    check(result.kind == MessageReactionOutcomeKind::expired && !result.projection.authority.mine_normal &&
              !fixture.controller.count,
          "reaction intent: delayed success after deadline cannot revive expired intent without a prior "
          "tick");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    auto vote = target;
    vote.type = MessageReactionType::vote;
    check(fixture.choose(vote, true).error == MessageReactionError::invalid_input,
          "reaction intent: vote remains outside outbound normal and burst intents");
    const auto mine_revision = fixture.controller.entries[0].mine_revision;
    check(message_reaction_controller_observe(&fixture.controller, vote, true, false),
          "reaction intent: foreign vote Gateway addition retained as residual total");
    message_reaction_controller_observe(&fixture.controller, vote, true, true);
    message_reaction_controller_observe(&fixture.controller, vote, false, true);
    message_reaction_controller_observe(&fixture.controller, vote, false, false);
    check(fixture.controller.entries[0].authority.total == 5 &&
              fixture.controller.entries[0].authority.normal == 4 &&
              !fixture.controller.entries[0].authority.mine_normal &&
              fixture.controller.entries[0].mine_revision == mine_revision,
          "reaction intent: vote count semantics preserve residual and never fabricate own normal "
          "membership");
    result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                  MessageReactionCompletion::success, 1);
    check(result.projection.authority.total == 6 && result.projection.authority.normal == 5 &&
              result.projection.authority.mine_normal && !result.authority_conflict,
          "reaction intent: normal HTTP confirmation preserves foreign residual vote contribution");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    message_reaction_controller_complete(&fixture.controller, ticket, MessageReactionCompletion::success,
                                         1);
    fixture.choose(target, false, 2);
    ticket = fixture.send(2);
    message_reaction_controller_complete(&fixture.controller, ticket, MessageReactionCompletion::success,
                                         3);
    message_reaction_controller_observe(&fixture.controller, target, true, true);
    message_reaction_controller_observe(&fixture.controller, target, true, false);
    message_reaction_controller_project(fixture.controller, target, &projected);
    model = {};
    model.foreign_normal = 5;
    check(fixture.controller.entries[0].awaiting_echo && !projected.authority.mine_normal &&
              model.matches(projected),
          "reaction intent: old add echo after successful remove cannot erase latest confirmed choice or "
          "foreign count");
    message_reaction_controller_observe(&fixture.controller, target, false, true);
    check(!fixture.controller.count,
          "reaction intent: matching final remove echo releases bounded settled own fence");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    message_reaction_controller_observe(&fixture.controller, target, true, true);
    result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                  MessageReactionCompletion::terminal, 1);
    check(result.kind == MessageReactionOutcomeKind::settled && result.projection.authority.mine_normal &&
              !fixture.controller.count,
          "reaction intent: authoritative matching own echo settles despite later transport failure");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    message_reaction_controller_complete(&fixture.controller, ticket, MessageReactionCompletion::success,
                                         1);
    check(message_reaction_controller_next_due(fixture.controller, &due) &&
              due == kMessageReactionIntentLifetimeMs &&
              !message_reaction_controller_peek_due(fixture.controller, 100, &pending),
          "reaction intent: settled fence waits only for finite original expiry without repeating REST");
    check(message_reaction_controller_expire(&fixture.controller, kMessageReactionIntentLifetimeMs,
                                             &expired) &&
              expired.kind == MessageReactionOutcomeKind::settled && !fixture.controller.count,
          "reaction intent: missing final echo releases settled fence silently at bounded expiry");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    message_reaction_controller_complete(&fixture.controller, ticket, MessageReactionCompletion::success,
                                         1);
    check(message_reaction_controller_snapshot(&fixture.controller, target, fixture.initial) &&
              !fixture.controller.count,
          "reaction intent: fresh authoritative snapshot retires settled fence and owns later live state");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    auto confirmed = fixture.initial;
    confirmed.total = confirmed.normal = 5;
    confirmed.mine_normal = true;
    message_reaction_controller_snapshot(&fixture.controller, target, confirmed);
    result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                  MessageReactionCompletion::terminal, 1);
    check(
        result.kind == MessageReactionOutcomeKind::settled && !fixture.controller.count &&
            result.projection.authority.mine_normal,
        "reaction intent: matching full snapshot settles active request despite later terminal transport");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    message_reaction_controller_complete(&fixture.controller, ticket, MessageReactionCompletion::success,
                                         1);
    check(message_reaction_controller_observe(&fixture.controller, target, false, true,
                                              kMessageReactionIntentLifetimeMs) &&
              !fixture.controller.count,
          "reaction intent: expired settled fence releases before contrary live own event");
    fixture.reset();
    fixture.choose(target, true);
    check(
        message_reaction_controller_observe(&fixture.controller, target, false, false,
                                            kMessageReactionIntentLifetimeMs) &&
            fixture.controller.count == 1 && fixture.controller.entries[0].authority.normal == 3,
        "reaction intent: expired pending event updates foreign authority before deferred expiry rollback");
    check(message_reaction_controller_expire(&fixture.controller, kMessageReactionIntentLifetimeMs,
                                             &expired) &&
              expired.projection.authority.normal == 3 && !expired.projection.desired_normal,
          "reaction intent: expired pending rollback retains just-observed foreign count without own "
          "contribution");
    fixture.reset();
    fixture.choose(target, true);
    ticket = fixture.send();
    message_reaction_controller_observe(&fixture.controller, target, true, true);
    message_reaction_controller_observe(&fixture.controller, target, true, false);
    check(message_reaction_controller_expire(&fixture.controller, kMessageReactionIntentLifetimeMs,
                                             &expired) &&
              expired.kind == MessageReactionOutcomeKind::settled &&
              expired.projection.authority.mine_normal && expired.projection.authority.normal == 6 &&
              fixture.controller.flight.active && fixture.controller.flight.retired,
          "reaction intent: validated achieved state at deadline settles while retaining foreign counts "
          "and flight drain");
    result = message_reaction_controller_complete(&fixture.controller, ticket,
                                                  MessageReactionCompletion::terminal,
                                                  kMessageReactionIntentLifetimeMs + 1);
    check(result.kind == MessageReactionOutcomeKind::retired && !result.project &&
              !fixture.controller.flight.active,
          "reaction intent: later terminal HTTP cannot fail or rollback achieved expiry tombstone");
}
} // namespace maicord
