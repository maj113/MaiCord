#include "maicord/model/message_poll_store.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr u32 kFullPoll = gateway_message_poll_field_question | gateway_message_poll_field_answers |
                          gateway_message_poll_field_expiry | gateway_message_poll_field_multiselect |
                          gateway_message_poll_field_layout;
GatewayMessagePoll full_poll(Utf8View question, ConstSlice<GatewayMessagePollAnswer> answers) noexcept {
    GatewayMessagePoll poll{};
    poll.question = question;
    poll.answers = answers;
    poll.fields = kFullPoll;
    poll.expiry_ms = 1000000;
    poll.layout_type = 1;
    poll.present = true;
    return poll;
}
struct ReleasePoll {
    MessagePollStore& store;
    ~ReleasePoll() noexcept {
        message_poll_store_release(&store);
    }
};
bool same_text(Utf8View a, Utf8View b) noexcept {
    return a.bytes == b.bytes && bytes_equal(a.data, b.data, a.bytes);
}
} // namespace
void test_message_poll_store(void (*check)(bool, const char*) noexcept) noexcept {
    MessagePollStore store{};
    ReleasePoll release{store};
    check(message_poll_store_initialize_owned(&store), "poll model: configure dormant bounded owner");
    auto costs = message_poll_store_metrics(store);
    check(!store.owned && !store.records && !costs.reserved_bytes && !costs.committed_bytes &&
              !costs.commit_count,
          "poll model: an account with no received poll reserves and commits no bank");
    GatewayMessagePoll results{};
    results.present = results.results_known = true;
    results.fields = gateway_message_poll_field_results;
    check(message_poll_store_apply(&store, 10, 20, results).error == MessagePollStoreError::incomplete &&
              !store.owned,
          "poll model: results-only unknown message cannot allocate an incomplete ghost record");

    char question[]{'C', 'h', 'o', 'o', 's', 'e', ' ', 'c', 'o', 'l', 'o', 'r'};
    GatewayMessagePollAnswer answers[]{{1, utf8_literal("Red"), 0, {}, false},
                                       {5, utf8_literal("Blue"), 777, utf8_literal("blue"), true}};
    GatewayMessagePollAnswerCount counts[]{{1, 5, false}, {5, 2, true}};
    auto input = full_poll({question, sizeof(question)}, {answers, 2});
    input.fields |= gateway_message_poll_field_results;
    input.results_known = true;
    input.answer_counts = {counts, 2};
    auto applied = message_poll_store_apply(&store, 10, 20, input, 10);
    check(applied.ok() && applied.changed && applied.record && applied.record->answer_count == 2,
          "poll model: real typed question/answers/results publish one received poll");
    if (!applied.ok() || !applied.record)
        return;
    question[0] = '!';
    const auto* poll = message_poll_store_find(store, 10);
    check(poll &&
              same_text(message_poll_store_string(store, poll->question), utf8_literal("Choose color")) &&
              poll->answers[1].emoji_id == 777 && poll->answers[1].emoji_animated &&
              poll->answers[0].count_known && poll->answers[0].count == 5 && poll->answers[1].me_known &&
              poll->answers[1].me_voted,
          "poll model: no decode-scratch borrowing and independent typed emoji/count/own state");
    costs = message_poll_store_metrics(store);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    check(costs.committed_bytes == 3 * static_cast<usize>(system.dwPageSize) && costs.commit_count == 3,
          "poll model: first small poll commits only owner, record and active text pages");
    const auto stable_costs = costs;
    const u32 stable_revision = store.revision, stable_content = store.content_revision,
              stable_text = store.text_used;
    question[0] = 'C';
    auto metadata = input;
    metadata.fields &= ~gateway_message_poll_field_results;
    metadata.answer_counts = {};
    metadata.results_known = false;
    bool retained = true;
    for (u32 frame = 0; frame < 64; ++frame) {
        const auto update = message_poll_store_apply(&store, 10, 20, metadata);
        const auto* current = message_poll_store_find(store, 10);
        retained &= update.ok() && !update.changed && current && current->results_known &&
                    current->answers[0].count_known && current->answers[0].count == 5 &&
                    current->answers[1].me_known && current->answers[1].me_voted;
    }
    costs = message_poll_store_metrics(store);
    check(retained && store.revision == stable_revision && store.content_revision == stable_content &&
              store.text_used == stable_text && costs.committed_bytes == stable_costs.committed_bytes &&
              costs.commit_count == stable_costs.commit_count && !store.compactions,
          "poll model: 64 identical metadata publications retain counts, text, versions and physical "
          "commit counters");
    const char malformed[]{static_cast<char>(0xc0)};
    auto invalid_question = metadata;
    invalid_question.fields = gateway_message_poll_field_question;
    invalid_question.question = {malformed, 1};
    GatewayMessagePollAnswer duplicate_answers[]{answers[0], answers[0]};
    auto duplicate_inventory = metadata;
    duplicate_inventory.fields = gateway_message_poll_field_answers;
    duplicate_inventory.answers = {duplicate_answers, 2};
    GatewayMessagePollAnswerCount foreign_count[]{{999, 2, false}};
    auto foreign_results = results;
    foreign_results.answer_counts = {foreign_count, 1};
    check(message_poll_store_apply(&store, 10, 20, invalid_question).error ==
                  MessagePollStoreError::invalid_utf8 &&
              message_poll_store_apply(&store, 10, 20, duplicate_inventory).error ==
                  MessagePollStoreError::invalid_input &&
              message_poll_store_apply(&store, 10, 20, foreign_results).error ==
                  MessagePollStoreError::invalid_input &&
              store.revision == stable_revision && store.text_used == stable_text &&
              poll->answers[0].count == 5,
          "poll model: malformed UTF8, duplicate answer IDs and foreign result IDs reject without "
          "corrupting prior publication");

    auto vote = [&](u64 user, u32 option, bool add, u64 serial) noexcept {
        return message_poll_store_vote(&store, {20, 10, user, 0, option, add, false}, 7, serial);
    };
    const u32 content_before_votes = poll->content_revision;
    check(vote(7, 5, true, 11).ok() && poll->answers[1].count == 2,
          "poll model: snapshot-known current user's repeated add is a semantic duplicate");
    check(vote(50, 1, true, 12).changed && poll->answers[0].count == 6,
          "poll model: observed other voter increments the independently known aggregate");
    const u32 voted_revision = poll->result_revision;
    check(vote(50, 1, true, 13).ok() && !vote(50, 1, true, 14).changed && poll->answers[0].count == 6 &&
              poll->result_revision == voted_revision,
          "poll model: repeated same-state events with newer serials do not drift known counts");
    check(vote(50, 1, false, 15).changed && poll->answers[0].count == 5 && !vote(50, 1, false, 16).changed,
          "poll model: observed remove converges once and repeated remove is retained");
    check(
        vote(50, 1, true, 16).error == MessagePollStoreError::stale &&
            vote(50, 1, true, 12).error == MessagePollStoreError::stale && poll->answers[0].count == 5 &&
            poll->content_revision == content_before_votes && store.content_revision == stable_content,
        "poll model: exact-serial duplicates/out-of-order events cannot mutate counts or relayout content");
    auto ticket = message_poll_store_result_ticket(store, 10, 16);
    check(vote(99, 1, true, 17).changed, "poll model: newer live vote arrives during HTTP results request");
    const auto stale_results = message_poll_store_apply(&store, 10, 20, results, 0, &ticket);
    check(stale_results.ok() && stale_results.results_ignored && !stale_results.changed &&
              poll->answers[0].count == 6,
          "poll model: late empty HTTP results cannot erase newer Gateway aggregate/own observations");
    const GatewayMessagePoll removed{};
    check(message_poll_store_apply(&store, 10, 20, removed, 0, &ticket).error ==
                  MessagePollStoreError::stale &&
              message_poll_store_find(store, 10) && poll->answers[0].count == 6,
          "poll model: stale HTTP explicit-null cannot remove a poll with newer Gateway votes");
    const auto old_content_ticket = ticket;
    auto changed_question = metadata;
    changed_question.fields = gateway_message_poll_field_question;
    changed_question.question = utf8_literal("Updated color question");
    check(message_poll_store_apply(&store, 10, 20, changed_question, 18).changed,
          "poll model: authoritative content gets a distinct internal content version");
    const u32 fresh_content = poll->content_revision;
    check(message_poll_store_apply(&store, 10, 20, input, 0, &ticket).error ==
                  MessagePollStoreError::stale &&
              poll->content_revision == fresh_content &&
              same_text(message_poll_store_string(store, poll->question), changed_question.question),
          "poll model: stale HTTP content ticket rejects the whole old question/answers snapshot");
    ticket = message_poll_store_result_ticket(store, 10, 18);
    check(message_poll_store_apply(&store, 10, 20, results, 0, &ticket).changed && poll->results_known &&
              poll->answers[0].count_known && !poll->answers[0].count && poll->answers[1].me_known &&
              !poll->answers[1].me_voted && poll->content_revision == fresh_content,
          "poll model: current authoritative empty counts mean known zero and do not relayout the poll");
    results.is_finalized = true;
    check(message_poll_store_apply(&store, 10, 20, results, 19).changed && poll->is_finalized &&
              !vote(7, 1, true, 20).changed && !poll->answers[0].me_voted && !poll->answers[0].count,
          "poll model: finalized poll blocks subsequent live vote mutation");
    results.is_finalized = false;
    ticket = message_poll_store_result_ticket(store, 10, 20);
    check(message_poll_store_apply(&store, 10, 20, results, 0, &ticket).error ==
                  MessagePollStoreError::stale &&
              poll->is_finalized,
          "poll model: even a current snapshot cannot reopen finalized results");
    results.results_known = false;
    check(message_poll_store_apply(&store, 10, 20, results).error == MessagePollStoreError::invalid_input,
          "poll model: nonnullable results object cannot counterfeit an explicit unknown replacement");

    auto unknown = full_poll({}, {answers, 2});
    unknown.layout_type = 2;
    applied = message_poll_store_apply(&store, 11, 20, unknown);
    poll = message_poll_store_find(store, 11);
    check(applied.ok() && poll && poll->layout_type == 2 && !poll->results_known &&
              !poll->answers[0].count_known && !poll->answers[0].me_known &&
              !poll->answers[1].count_known && !poll->answers[1].me_known && !poll->is_finalized,
          "poll model: first HTTP/history poll without results has unknown counts and own votes");
    if (!poll)
        return;
    check(message_poll_store_apply(&store, 11, 20, unknown, 21).ok() &&
              !poll->results_known && !poll->answers[0].count_known && !poll->answers[0].me_known &&
              message_poll_store_apply(&store, 11, 21, unknown, 22).error == MessagePollStoreError::stale &&
              poll->channel_id == 20 && poll->last_gateway_serial == 21,
          "poll model: Gateway metadata cannot infer empty results or cross the existing channel scope");
    const auto unknown_ticket = message_poll_store_result_ticket(store, 11, 21);
    check(message_poll_store_vote(&store, {20, 11, 7, 0, 1, true, false}, 7, 22).changed &&
              !poll->answers[0].count_known && poll->answers[0].me_known && poll->answers[0].me_voted &&
              !poll->answers[1].me_known && !poll->results_known,
          "poll model: an own vote event establishes its own mark without inventing aggregate counts");
    const u32 own_revision = poll->result_revision;
    check(!message_poll_store_vote(&store, {20, 11, 7, 0, 1, true, false}, 7, 23).changed &&
              poll->result_revision == own_revision && !poll->answers[0].count_known,
          "poll model: duplicate own event preserves unknown aggregate state");
    results.results_known = true;
    const auto unknown_late_results = message_poll_store_apply(&store, 11, 20, results, 0, &unknown_ticket);
    check(unknown_late_results.ok() && unknown_late_results.results_ignored &&
              !poll->results_known && !poll->answers[0].count_known && poll->answers[0].me_voted &&
              !poll->answers[1].me_known,
          "poll model: older HTTP results cannot replace newer partial Gateway own authority");
    const auto unknown_current_ticket = message_poll_store_result_ticket(store, 11, 23);
    check(message_poll_store_apply(&store, 11, 20, results, 0, &unknown_current_ticket).changed &&
              poll->results_known && poll->answers[0].count_known && !poll->answers[0].count &&
              poll->answers[0].me_known && !poll->answers[0].me_voted && poll->answers[1].me_known &&
              message_poll_store_own_vote(&store, 11, 20, 7, 1u).changed &&
              poll->answers[0].count == 1 && poll->answers[0].me_voted,
          "poll model: current explicit empty results establish zero before an accepted own vote");
    // The accepted PUT answers/@me: answer 5 (index 1) only.
    check(message_poll_store_own_vote(&store, 11, 20, 7, 2u).changed && poll->answers[0].count == 0 &&
              !poll->answers[0].me_voted && poll->answers[1].count == 1 && poll->answers[1].me_voted,
          "poll model: an accepted own vote moves the own mark and both counts by one");
    check(!message_poll_store_own_vote(&store, 11, 20, 7, 2u).changed &&
              !message_poll_store_vote(&store, {20, 11, 7, 0, 5, true, false}, 7, 24).changed &&
              !message_poll_store_vote(&store, {20, 11, 7, 0, 1, false, false}, 7, 25).changed &&
              poll->answers[0].count == 0 && poll->answers[1].count == 1,
          "poll model: the gateway echo of an accepted own vote never counts twice");
    check(message_poll_store_vote(&store, {20, 11, 9, 0, 5, true, false}, 7, 26).changed &&
              poll->answers[1].count == 2 && message_poll_store_own_vote(&store, 11, 20, 7, 0).changed &&
              poll->answers[1].count == 1 && !poll->answers[1].me_voted &&
              message_poll_store_own_vote(&store, 11, 20, 7, 4u).error == MessagePollStoreError::invalid_input &&
              message_poll_store_own_vote(&store, 11, 21, 7, 1u).error == MessagePollStoreError::invalid_input,
          "poll model: removing the own vote keeps others' votes; foreign answers or channels are rejected");
    const auto old_generation = poll->content_revision;
    const auto deleted_ticket = message_poll_store_result_ticket(store, 11, 23);
    check(message_poll_store_delete(&store, 11) && !message_poll_store_find(store, 11) &&
              message_poll_store_apply(&store, 11, 20, unknown, 24).ok() &&
              message_poll_store_find(store, 11)->content_revision != old_generation &&
              message_poll_store_apply(&store, 11, 20, unknown, 0, &deleted_ticket).error ==
                  MessagePollStoreError::stale,
          "poll model: delete/recreate with identical IDs cannot accept an old HTTP content ticket");
    poll = message_poll_store_find(store, 11);
    check(poll && poll->last_gateway_serial == 24 && !poll->results_known &&
              !poll->answers[0].count_known && !poll->answers[0].me_known &&
              !poll->answers[1].count_known && !poll->answers[1].me_known,
          "poll model: first Gateway publication without results also leaves every answer unknown");

    const auto before_page = message_poll_store_metrics(store);
    auto* const before_owner = store.owned;
    auto* const before_text = store.text;
    const u32 before_page_count = store.count, before_page_revision = store.revision;
    GatewayMessagePollAnswer invalid_answers[]{{0, utf8_literal("invalid"), 0, {}, false}};
    auto invalid_poll = full_poll(utf8_literal("bad"), {invalid_answers, 1});
    MessagePollPageInput rejected_page[]{{12, 20, &metadata, 0, nullptr},
                                         {13, 20, &invalid_poll, 0, nullptr}};
    const auto rejected_page_result = message_poll_store_apply_page(&store, {rejected_page, 2});
    check(rejected_page_result.error == MessagePollStoreError::invalid_input &&
              !rejected_page_result.changed && store.owned == before_owner && store.text == before_text &&
              store.count == before_page_count && store.revision == before_page_revision &&
              !message_poll_store_find(store, 12) &&
              message_poll_store_metrics(store).commit_count == before_page.commit_count,
          "poll model: later invalid page candidate rolls back earlier valid poll and every live "
          "bank/version");
    const auto* retained_poll = message_poll_store_find(store, 10);
    const u32 retained_content = retained_poll->content_revision,
              retained_results = retained_poll->result_revision;
    MessagePollPageInput accepted_page[]{{10, 20, &input, 0, &old_content_ticket},
                                         {12, 20, &metadata, 0, nullptr}};
    const auto accepted_page_result = message_poll_store_apply_page(&store, {accepted_page, 2});
    check(accepted_page_result.ok() && accepted_page_result.changed && accepted_page_result.applied == 1 &&
              accepted_page_result.ignored == 1 && store.count == before_page_count + 1 &&
              store.owned != before_owner && store.text != before_text &&
              store.revision != before_page_revision && message_poll_store_find(store, 12) &&
              message_poll_store_find(store, 10)->content_revision == retained_content &&
              message_poll_store_find(store, 10)->result_revision == retained_results &&
              message_poll_store_find(store, 10)->is_finalized,
          "poll model: accepted page skips stale resident ticket, publishes unrelated new poll atomically "
          "and preserves resident versions");
    const auto stable_page = message_poll_store_metrics(store);
    auto* const accepted_owner = store.owned;
    const MessagePollPageInput stale_page[]{accepted_page[0]};
    const auto ignored_page_result = message_poll_store_apply_page(&store, {stale_page, 1});
    check(ignored_page_result.ok() && !ignored_page_result.changed && ignored_page_result.ignored == 1 &&
              store.owned == accepted_owner &&
              message_poll_store_metrics(store).commit_count == stable_page.commit_count &&
              message_poll_store_apply_page(&store, {}).ok(),
          "poll model: all-stale and ordinary pages allocate no transaction bank or scene publication");
    auto resident_ticket = message_poll_store_result_ticket(store, 12, 1000);
    auto result_page_poll = results;
    result_page_poll.results_known = true;
    MessagePollPageInput result_page[]{{12, 20, &result_page_poll, 0, &resident_ticket}};
    const auto content_binding = store.content_revision;
    const auto structural_content = message_poll_store_find(store, 12)->content_revision;
    check(message_poll_store_apply_page(&store, {result_page, 1}).changed &&
              store.content_revision != content_binding &&
              message_poll_store_find(store, 12)->content_revision == structural_content &&
              message_poll_store_find(store, 12)->results_known,
          "poll model: result-only pointer-changing publication forces global borrowed-string rebind "
          "without changing action content identity");

    MessagePollStore bounded{};
    ReleasePoll bounded_release{bounded};
    check(message_poll_store_initialize_owned(&bounded, 4, 64),
          "poll model: configure small real text-bank fixture");
    char short_question[30], longer_question[48];
    for (auto& byte : short_question)
        byte = 'a';
    for (auto& byte : longer_question)
        byte = 'b';
    GatewayMessagePollAnswer compact_answers[]{{1, utf8_literal("A"), 0, {}, false},
                                               {2, utf8_literal("B"), 0, {}, false}};
    auto compact = full_poll({short_question, sizeof(short_question)}, {compact_answers, 2});
    check(message_poll_store_apply(&bounded, 1, 2, compact).ok() &&
              message_poll_store_apply(&bounded, 3, 2, compact).ok() && bounded.text_used == 64,
          "poll model: actual 64-byte text arena holds two copied poll bodies");
    const auto before_failure = message_poll_store_metrics(bounded);
    auto replace = compact;
    replace.fields = gateway_message_poll_field_question;
    replace.question = {longer_question, sizeof(longer_question)};
    check(message_poll_store_apply(&bounded, 1, 2, replace).error == MessagePollStoreError::capacity &&
              bounded.text_used == 64 &&
              same_text(message_poll_store_string(bounded, message_poll_store_find(bounded, 3)->question),
                        compact.question) &&
              message_poll_store_metrics(bounded).commit_count == before_failure.commit_count,
          "poll model: full text preflight rejects atomically without copying or committing another bank");
    replace.question = utf8_literal("short");
    applied = message_poll_store_apply(&bounded, 1, 2, replace);
    poll = message_poll_store_find(bounded, 1);
    check(applied.ok() && applied.changed && bounded.compactions == 1 && !bounded.text_wasted && poll &&
              same_text(message_poll_store_string(bounded, poll->question), replace.question) &&
              same_text(message_poll_store_string(bounded, poll->answers[0].text), utf8_literal("A")) &&
              same_text(message_poll_store_string(bounded, message_poll_store_find(bounded, 3)->question),
                        compact.question),
          "poll model: real bank compaction preserves retained answer aliases and other poll text");

    MessagePollStore observations{};
    ReleasePoll observations_release{observations};
    check(message_poll_store_initialize_owned(&observations, 1, 128),
          "poll model: configure shared bounded voter observations");
    auto observed_poll = full_poll(utf8_literal("One"), {answers, 2});
    observed_poll.fields |= gateway_message_poll_field_results;
    observed_poll.results_known = true;
    check(message_poll_store_apply(&observations, 30, 20, observed_poll, 1).ok(),
          "poll model: baseline has known-zero answers");
    const auto observation_costs = message_poll_store_metrics(observations);
    check(message_poll_store_vote(&observations, {20, 30, 7, 0, 5, true, false}, 7, 2).changed,
          "poll model: own membership occupies first shared observation");
    bool streamed = true;
    for (u32 i = 0; i < 128; ++i)
        streamed &=
            message_poll_store_vote(&observations, {20, 30, 100 + i, 0, 1, true, false}, 7, 3 + i).ok();
    poll = message_poll_store_find(observations, 30);
    check(streamed && poll && poll->answers[0].count_known && poll->answers[0].count == 128 &&
              !poll->answers[1].count_known && poll->answers[1].me_known && poll->answers[1].me_voted &&
              !poll->results_known &&
              message_poll_store_metrics(observations).committed_bytes == observation_costs.committed_bytes,
          "poll model: 129 memberships evict only the affected answer aggregate, preserve known own vote "
          "and allocate nothing");
    check(message_poll_store_vote(&observations, {20, 30, 900, 0, 1, true, false}, 7, 131).changed &&
              !poll->answers[0].count_known && poll->answers[0].count == 128 && poll->answers[1].me_voted,
          "poll model: a second exact eviction downgrades that aggregate instead of inventing a count");
    GatewayMessagePollAnswerCount refreshed[]{{1, 129, false}, {5, 1, true}};
    auto refresh = observed_poll;
    refresh.fields = gateway_message_poll_field_results;
    refresh.answer_counts = {refreshed, 2};
    check(message_poll_store_apply(&observations, 30, 20, refresh, 132).changed && poll->results_known &&
              poll->answers[0].count_known && poll->answers[0].count == 129 &&
              poll->answers[1].count_known && poll->answers[1].me_voted,
          "poll model: authoritative refresh restores both aggregates after bounded observation pressure");
    const auto full_costs = message_poll_store_metrics(observations);
    check(message_poll_store_apply(&observations, 31, 20, observed_poll).error ==
                  MessagePollStoreError::capacity &&
              observations.count == 1 &&
              message_poll_store_metrics(observations).commit_count == full_costs.commit_count,
          "poll model: record-capacity failure preserves the live poll and committed budget");
}
} // namespace maicord
