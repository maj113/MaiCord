#include "maicord/ui/search_query.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool search_user(void*, Utf8View text, u64* id) noexcept {
    if (text.bytes == 5 && bytes_equal(text.data, "alice", 5)) {
        *id = 81;
        return true;
    }
    constexpr auto escaped_name = utf8_literal("Alice \"Builder\"");
    if (text.bytes == escaped_name.bytes && bytes_equal(text.data, escaped_name.data, text.bytes)) {
        *id = 83;
        return true;
    }
    return false;
}
bool search_channel(void*, Utf8View text, u64* id) noexcept {
    if (text.bytes == 10 && bytes_equal(text.data, "build room", 10)) {
        *id = 91;
        return true;
    }
    constexpr auto escaped_channel = utf8_literal("folder\\");
    if (text.bytes == escaped_channel.bytes && bytes_equal(text.data, escaped_channel.data, text.bytes)) {
        *id = 92;
        return true;
    }
    return false;
}
bool search_date(void*, Utf8View, u64* first, u64* last) noexcept {
    *first = 100000000;
    *last = *first + 23 * 3600000ull;
    return true;
}
struct SearchDateBounds { u64 first, last; };
bool search_date_bounds(void* context, Utf8View, u64* first, u64* last) noexcept {
    const auto& bounds = *static_cast<const SearchDateBounds*>(context);
    *first = bounds.first;
    *last = bounds.last;
    return true;
}
} // namespace
void test_search_query(void (*check)(bool, const char*) noexcept) noexcept {
    const UiSearchQueryContext context{nullptr, search_user, search_channel, search_date};
    const auto q = ui_search_query_parse(
        utf8_literal(
            "release \"two words\" from:alice in:\"build room\" mentions:<@!82> has:image pinned:true"),
        MessageSearchScope::guild, 71, MessageSearchSort::newest, &context);
    check(q.ok() && q.query.author_count == 1 && q.query.author_ids[0] == 81 &&
              q.query.channel_ids[0] == 91 && q.query.mention_ids[0] == 82 &&
              q.query.has == search_has_image &&
              q.query.pinned == MessageSearchPinned::pinned && q.query.content_bytes == 19 &&
              bytes_equal(q.query.content, "release \"two words\"", 19),
          "search_query_resolves_loaded_names_and_native_mentions_preserves_quoted_free_text");
    const auto repeated =
        ui_search_query_parse(utf8_literal("from:81 from:<@81> has:link has:poll"),
                              MessageSearchScope::channel, 71, MessageSearchSort::oldest);
    check(repeated.ok() && repeated.query.author_count == 1 &&
              repeated.query.has == (search_has_link | search_has_poll) &&
              repeated.query.sort == MessageSearchSort::oldest,
          "search_query_deduplicates_identity_filters_and_has_values");
    const auto forwarded = ui_search_query_parse(utf8_literal("has:forward has:snapshot"),
        MessageSearchScope::guild, 71, MessageSearchSort::newest);
    u8 forward_target[kMessageSearchMaximumTargetBytes]{};
    const auto forward_written = write_message_search_target(forwarded.query,
        {forward_target, sizeof(forward_target)});
    u32 snapshot_parameters = 0;
    constexpr auto snapshot_parameter = utf8_literal("&has=snapshot");
    for (u32 at = 0; at + snapshot_parameter.bytes <= forward_written.bytes; ++at)
        if (bytes_equal(forward_target + at, snapshot_parameter.data, snapshot_parameter.bytes))
            ++snapshot_parameters;
    check(forwarded.ok() && forwarded.query.has == search_has_snapshot &&
        forward_written.ok() && snapshot_parameters == 1,
        "search_query_forward_label_alias_deduplicates_to_existing_snapshot_wire_filter");
    check(!ui_search_query_parse(utf8_literal("from:ambiguous"), MessageSearchScope::guild, 71,
                                 MessageSearchSort::newest, &context)
               .ok(),
          "search_query_unavailable_or_ambiguous_name_has_no_display_name_authority");
    check(!ui_search_query_parse(utf8_literal("from:1 from:2 from:3 from:4 from:5"),
                                 MessageSearchScope::guild, 71, MessageSearchSort::newest)
               .ok(),
          "search_query_caps_identity_filters");
    const auto literal =
        ui_search_query_parse(utf8_literal("https://example.invalid \"from:alice\" future:filter"),
                              MessageSearchScope::guild, 71, MessageSearchSort::newest);
    check(literal.ok() && literal.query.content_bytes == 50 && !literal.query.author_count,
          "search_query_unknown_keys_and_quoted_filter_like_words_stay_free_text");
    const auto day = ui_search_query_parse(utf8_literal("on:2026-03-29"), MessageSearchScope::guild,
                                           71, MessageSearchSort::newest, &context);
    const auto after =
        ui_search_query_parse(utf8_literal("after:2026-03-29"), MessageSearchScope::guild, 71,
                              MessageSearchSort::newest, &context);
    const auto before =
        ui_search_query_parse(utf8_literal("before:2026-03-29"), MessageSearchScope::guild, 71,
                              MessageSearchSort::newest, &context);
    check(day.ok() && day.query.min_id == (100000000ull << 22) &&
              day.query.max_id == ((100000000ull + 23 * 3600000ull) << 22) &&
              after.query.min_id == day.query.max_id && before.query.max_id == day.query.min_id,
          "search_query_local_calendar_injected_23_hour_day_boundaries_match_public_client_start_next_day");
    check(!ui_search_query_parse(utf8_literal("on:2025-02-29"), MessageSearchScope::guild, 71,
                                 MessageSearchSort::newest)
                  .ok() &&
              ui_search_query_parse(utf8_literal("on:2024-02-29"), MessageSearchScope::guild, 71,
                                    MessageSearchSort::newest)
                  .ok(),
          "search_query_validates_leap_calendar_dates");
    char full[kMessageSearchContentBytes + 1];
    for (auto& c : full)
        c = 'a';
    const auto capacity =
        ui_search_query_parse({full, kMessageSearchContentBytes}, MessageSearchScope::guild,
                              71, MessageSearchSort::newest);
    check(capacity.ok() && capacity.query.content_bytes == kMessageSearchContentBytes &&
              !ui_search_query_parse({full, sizeof(full)}, MessageSearchScope::guild, 71,
                                     MessageSearchSort::newest)
                   .ok(),
          "search_query_256_utf8_byte_capacity_admission");

    const auto quoted_ids = ui_search_query_parse(
        utf8_literal(R"(from:"81" mentions:"<@!82>" in:"<#91>" has:"image" pinned:"false")"),
        MessageSearchScope::guild, 71, MessageSearchSort::newest);
    check(quoted_ids.ok() && quoted_ids.query.author_count == 1 && quoted_ids.query.author_ids[0] == 81 &&
        quoted_ids.query.mention_count == 1 && quoted_ids.query.mention_ids[0] == 82 &&
        quoted_ids.query.channel_count == 1 && quoted_ids.query.channel_ids[0] == 91 &&
        quoted_ids.query.has == search_has_image &&
        quoted_ids.query.pinned == MessageSearchPinned::unpinned,
        "search_query_quoted_filter_values_resolve_without_loaded_identity_names");
    const auto escaped_identity = ui_search_query_parse(
        utf8_literal(R"(from:"Alice \"Builder\"" has:image)"), MessageSearchScope::guild,
        71, MessageSearchSort::newest, &context);
    check(escaped_identity.ok() && escaped_identity.query.author_ids[0] == 83 &&
        escaped_identity.query.has == search_has_image,
        "search_query_unescapes_quoted_identity_name_before_resolving");
    const auto terminal_slashes = ui_search_query_parse(utf8_literal(R"(in:"folder\\" has:image)"),
        MessageSearchScope::guild, 71, MessageSearchSort::newest, &context);
    check(terminal_slashes.ok() && terminal_slashes.query.channel_ids[0] == 92 &&
        terminal_slashes.query.has == search_has_image &&
        ui_search_query_parse(utf8_literal(R"(in:"folder\")"), MessageSearchScope::guild,
            71, MessageSearchSort::newest, &context).error == UiSearchQueryError::invalid_text &&
        ui_search_query_parse(utf8_literal(R"(in:"folder\\\")"), MessageSearchScope::guild,
            71, MessageSearchSort::newest, &context).error == UiSearchQueryError::invalid_text,
        "search_query_terminal_backslash_pairs_decode_and_odd_trailing_runs_reject");

    // Two backslashes escape each other, so the following quote closes the
    // free-text phrase and has:image must remain an independent filter.
    constexpr auto slash_phrase = utf8_literal(R"("folder\\")");
    const auto even_slashes = ui_search_query_parse(utf8_literal(R"("folder\\" has:image)"),
        MessageSearchScope::guild, 71, MessageSearchSort::newest);
    check(even_slashes.ok() && even_slashes.query.has == search_has_image &&
        even_slashes.query.content_bytes == slash_phrase.bytes &&
        bytes_equal(even_slashes.query.content, slash_phrase.data, slash_phrase.bytes),
        "search_query_even_backslash_quote_closes_phrase_and_retains_following_filter");
    constexpr auto odd_phrase = utf8_literal(R"("say \"hello\"")");
    const auto odd_slashes = ui_search_query_parse(utf8_literal(R"("say \"hello\"" from:81)"),
        MessageSearchScope::guild, 71, MessageSearchSort::newest);
    check(odd_slashes.ok() && odd_slashes.query.author_ids[0] == 81 &&
        odd_slashes.query.content_bytes == odd_phrase.bytes &&
        bytes_equal(odd_slashes.query.content, odd_phrase.data, odd_phrase.bytes),
        "search_query_odd_backslash_preserves_literal_quotes_inside_phrase");
    const auto unterminated = ui_search_query_parse(utf8_literal("release from:\"alice has:image"),
        MessageSearchScope::guild, 71, MessageSearchSort::newest, &context);
    check(unterminated.error == UiSearchQueryError::invalid_text && unterminated.error_offset == 13 &&
        !ui_search_query_parse(utf8_literal(R"(from:"a""b")"), MessageSearchScope::guild,
            71, MessageSearchSort::newest, &context).ok() &&
        !ui_search_query_parse(utf8_literal(R"(from:"")"), MessageSearchScope::guild,
            71, MessageSearchSort::newest, &context).ok(),
        "search_query_reports_unclosed_quote_and_rejects_malformed_or_empty_identity_values");
    check(!ui_search_query_parse(utf8_literal("from:<#81>"), MessageSearchScope::guild,
              71, MessageSearchSort::newest).ok() &&
        !ui_search_query_parse(utf8_literal("in:<@91>"), MessageSearchScope::guild,
              71, MessageSearchSort::newest).ok() &&
        !ui_search_query_parse(utf8_literal("mentions:<@&82>"), MessageSearchScope::guild,
              71, MessageSearchSort::newest).ok(),
        "search_query_native_mentions_keep_user_channel_and_role_types_distinct");
    check(ui_search_query_parse(utf8_literal(R"(on:"2024-02-29")"), MessageSearchScope::guild,
        71, MessageSearchSort::newest).ok(), "search_query_quoted_calendar_filter_resolves");

    SearchDateBounds bounds{100, 100};
    const UiSearchQueryContext bounds_context{&bounds, nullptr, nullptr, search_date_bounds};
    auto bounded_date = [&]() noexcept {
        return ui_search_query_parse(utf8_literal("on:2026-10-04"), MessageSearchScope::guild,
            71, MessageSearchSort::newest, &bounds_context);
    };
    check(bounded_date().error == UiSearchQueryError::invalid_date,
        "search_query_zero_length_local_calendar_interval_is_rejected");
    bounds = {101, 100};
    check(bounded_date().error == UiSearchQueryError::invalid_date,
        "search_query_reverse_local_calendar_interval_is_rejected");
    bounds = {~u64{0} >> 22, (~u64{0} >> 22) + 1};
    check(bounded_date().error == UiSearchQueryError::invalid_date &&
        ui_search_query_parse(utf8_literal("after:2026-10-04"), MessageSearchScope::guild,
            71, MessageSearchSort::newest, &bounds_context).error == UiSearchQueryError::invalid_date,
        "search_query_snowflake_boundary_overflow_is_rejected_before_shifting");
    bounds = {0, 86400000};
    const auto epoch_day = bounded_date();
    check(epoch_day.ok() && epoch_day.query.max_id == (86400000ull << 22) &&
        ui_search_query_parse(utf8_literal("before:2015-01-01"), MessageSearchScope::guild,
            71, MessageSearchSort::newest).error == UiSearchQueryError::invalid_date,
        "search_query_epoch_day_is_bounded_and_before_epoch_cannot_become_unfiltered_search");

    const auto files = ui_search_query_parse(
        utf8_literal(
            R"(report linkFrom:YouTube.com fileType:.PDF filename:"Q3 plan" authorType:bot authortype:-webhook has:-embed)"),
        MessageSearchScope::guild, 71, MessageSearchSort::newest);
    const auto& f = files.query;
    check(files.ok() && f.hostname_bytes == 11 && bytes_equal(f.link_hostname, "youtube.com", 11) &&
              f.extension_bytes == 3 && bytes_equal(f.attachment_extension, "pdf", 3) &&
              f.filename_bytes == 7 && bytes_equal(f.attachment_filename, "Q3 plan", 7) &&
              f.author_types == search_author_bot &&
              f.author_types_excluded == search_author_webhook &&
              f.has_excluded == search_has_embed && !f.has && f.content_bytes == 6 &&
              message_search_query_valid(f),
          "search_query_parses_link_file_author_type_and_excluded_has_operators");
    check(!ui_search_query_parse(utf8_literal("linkFrom:a/b"), MessageSearchScope::guild, 71,
                                 MessageSearchSort::newest)
                  .ok() &&
              !ui_search_query_parse(utf8_literal("fileType:p.f"), MessageSearchScope::guild, 71,
                                     MessageSearchSort::newest)
                   .ok() &&
              !ui_search_query_parse(utf8_literal("authorType:robot"), MessageSearchScope::guild, 71,
                                     MessageSearchSort::newest)
                   .ok() &&
              !ui_search_query_parse(utf8_literal("linkFrom:a.com linkFrom:b.com"),
                                     MessageSearchScope::guild, 71, MessageSearchSort::newest)
                   .ok(),
          "search_query_rejects_invalid_hostnames_extensions_author_types_and_repeated_terms");
    check(!ui_search_query_parse(utf8_literal("has:link has:-link"), MessageSearchScope::guild, 71,
                                 MessageSearchSort::newest)
                  .ok() &&
              !ui_search_query_parse(utf8_literal("authorType:-bot authorType:bot"),
                                     MessageSearchScope::guild, 71, MessageSearchSort::newest)
                   .ok(),
          "search_query_rejects_values_both_included_and_excluded");
    const auto during =
        ui_search_query_parse(utf8_literal("during:2026-03-29"), MessageSearchScope::guild, 71,
                              MessageSearchSort::newest, &context);
    check(during.ok() && during.query.min_id == day.query.min_id && during.query.max_id == day.query.max_id,
          "search_query_during_is_the_same_calendar_day_as_on");
}
} // namespace maicord
