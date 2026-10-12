#include "maicord/ui/search_filter_context.hpp"

namespace maicord {
void test_search_filter_context(void (*check)(bool, const char*) noexcept) noexcept {
    const auto empty = ui_search_filter_context(utf8_literal("from:"), 5, 5);
    check(empty.kind == UiSearchFilterKind::from && empty.token_begin == 0 && empty.token_end == 5 &&
              empty.value_begin == 5 && empty.value_end == 5 && !empty.has_value &&
              empty.quote_complete && empty.same_kind_occurrences == 1 && empty.other_value_count == 0,
          "search_filter_context_empty_value_is_active_after_colon");
    const auto middle = ui_search_filter_context(utf8_literal("hi from:alice tail"), 10, 10);
    check(middle.kind == UiSearchFilterKind::from && middle.token_begin == 3 && middle.token_end == 13 &&
              middle.value_begin == 8 && middle.value_end == 13 &&
              utf8_equal(middle.value(), utf8_literal("alice")) &&
              utf8_equal(middle.prefix(), utf8_literal("al")),
          "search_filter_context_mid_value_prefix_retains_entire_replacement_and_suffix");
    const auto selected = ui_search_filter_context(utf8_literal("in:general rest"), 5, 9);
    const auto reversed = ui_search_filter_context(utf8_literal("in:general rest"), 9, 5);
    check(selected.kind == UiSearchFilterKind::in && reversed.kind == UiSearchFilterKind::in &&
              utf8_equal(selected.prefix(), utf8_literal("genera")) &&
              utf8_equal(reversed.prefix(), utf8_literal("ge")) && selected.value_end == 10,
          "search_filter_context_forward_and_reverse_selection_stay_in_one_value");
    check(ui_search_filter_context(utf8_literal("from:alice in:general"), 8, 17).kind == UiSearchFilterKind::none &&
              ui_search_filter_context(utf8_literal("from:alice"), 4, 7).kind == UiSearchFilterKind::none,
          "search_filter_context_cross_token_or_colon_selection_is_suppressed");
    check(ui_search_filter_context(utf8_literal("from:alice tail"), 10, 10).kind == UiSearchFilterKind::from &&
              ui_search_filter_context(utf8_literal("from:alice tail"), 11, 11).kind == UiSearchFilterKind::none &&
              ui_search_filter_context(utf8_literal("from:alice"), 0, 0).kind == UiSearchFilterKind::none,
          "search_filter_context_end_boundary_is_active_but_separator_and_key_are_not");
    constexpr auto quoted_text = utf8_literal("in:\"build room\" tail");
    const auto quoted = ui_search_filter_context(quoted_text, 10, 10);
    check(quoted.quoted && quoted.quote_complete && quoted.value_begin == 3 && quoted.value_end == 15 &&
              utf8_equal(quoted.value(), utf8_literal("build room")) &&
              utf8_equal(quoted.prefix(), utf8_literal("build ")),
          "search_filter_context_quoted_spaces_decode_without_replacing_neighbors");
    constexpr auto unfinished_text = utf8_literal("mentions:\"Alice Smith");
    const auto unfinished = ui_search_filter_context(unfinished_text, unfinished_text.bytes, unfinished_text.bytes);
    check(unfinished.kind == UiSearchFilterKind::mentions && unfinished.quoted && !unfinished.quote_complete &&
              utf8_equal(unfinished.value(), utf8_literal("Alice Smith")) && unfinished.prefix_bytes == 11,
          "search_filter_context_unfinished_quote_is_tolerated");
    const auto opener = ui_search_filter_context(utf8_literal("from:\""), 6, 6);
    check(opener.kind == UiSearchFilterKind::from && !opener.quote_complete && !opener.has_value &&
              opener.other_value_count == 0 && opener.prefix_bytes == 0,
          "search_filter_context_open_quote_without_text_does_not_consume_identity_slot");
    constexpr auto escaped_text = utf8_literal("from:\"A\\\"B\\\\C\"");
    const auto escaped = ui_search_filter_context(escaped_text, escaped_text.bytes, escaped_text.bytes);
    const auto escape_half = ui_search_filter_context(escaped_text, 8, 8);
    check(escaped.quote_complete && utf8_equal(escaped.value(), utf8_literal("A\"B\\C")) &&
              utf8_equal(escape_half.prefix(), utf8_literal("A")),
          "search_filter_context_escape_pairs_decode_and_do_not_read_past_caret");
    constexpr auto slash_value = utf8_literal("in:\"folder\\\\\"");
    const auto slash = ui_search_filter_context(utf8_literal("in:\"folder\\\\\" tail"),
                                                 slash_value.bytes, slash_value.bytes);
    check(slash.quote_complete && slash.value_end == slash_value.bytes &&
              utf8_equal(slash.value(), utf8_literal("folder\\")),
          "search_filter_context_even_backslashes_allow_closing_quote_and_preserve_tail");
    constexpr auto unknown_escape_text = utf8_literal("in:\"A\\q\"");
    const auto unknown_escape = ui_search_filter_context(unknown_escape_text, 8, 8);
    check(unknown_escape.quote_complete && utf8_equal(unknown_escape.value(), utf8_literal("A\\q")),
          "search_filter_context_unrecognized_escape_is_literal_as_in_query_parser");
    const auto malformed_quote = ui_search_filter_context(utf8_literal("from:\"A\"tail"), 8, 8);
    check(malformed_quote.kind == UiSearchFilterKind::none && malformed_quote.value_bytes == 0 &&
              malformed_quote.token_end == 0,
          "search_filter_context_invalid_quoted_suffix_returns_atomic_none");
    constexpr auto unicode_text = utf8_literal("from:\xc5\xbd\xc3\xa9 tail");
    const auto unicode = ui_search_filter_context(unicode_text, 7, 7);
    check(unicode.kind == UiSearchFilterKind::from && unicode.value_end == 9 &&
              utf8_equal(unicode.prefix(), utf8_literal("\xc5\xbd")) && unicode.value_bytes == 4,
          "search_filter_context_unicode_prefix_uses_utf8_byte_offsets");
    check(ui_search_filter_context(unicode_text, 6, 7).kind == UiSearchFilterKind::none &&
              ui_search_filter_context(unicode_text, 7, 8).kind == UiSearchFilterKind::none,
          "search_filter_context_split_unicode_anchor_or_caret_is_atomic_none");
    const char malformed[]{'f','r','o','m',':',static_cast<char>(0xc0),static_cast<char>(0xaf)};
    check(ui_search_filter_context({malformed, sizeof(malformed)}, 5, 5).kind == UiSearchFilterKind::none &&
              ui_search_filter_context({nullptr, 1}, 0, 0).kind == UiSearchFilterKind::none &&
              ui_search_filter_context(utf8_literal("from:x"), 7, 7).kind == UiSearchFilterKind::none,
          "search_filter_context_bad_utf8_null_storage_and_out_of_range_are_rejected");
    check(ui_search_filter_context(utf8_literal("from:x\t"), 5, 5).kind == UiSearchFilterKind::none &&
              ui_search_filter_context(utf8_literal("from:x\x7f"), 5, 5).kind == UiSearchFilterKind::none &&
              ui_search_filter_context(utf8_literal("from:x\xe2\x80\xa8"), 5, 5).kind == UiSearchFilterKind::none,
          "search_filter_context_controls_anywhere_prevent_partial_publication");
    constexpr auto counts_text = utf8_literal("from:1 from:2 in:9 from:3 from:4 from:");
    const auto counts = ui_search_filter_context(counts_text, counts_text.bytes, counts_text.bytes);
    check(counts.kind == UiSearchFilterKind::from && counts.same_kind_occurrences == 5 &&
              counts.other_value_count == 4 && !counts.has_value,
          "search_filter_context_fifth_empty_identity_exposes_four_other_populated_values");
    constexpr auto replacing_text = utf8_literal("from:1 from:2 from:3 from:4");
    const auto replacing = ui_search_filter_context(replacing_text, replacing_text.bytes, replacing_text.bytes);
    check(replacing.same_kind_occurrences == 4 && replacing.other_value_count == 3 && replacing.has_value,
          "search_filter_context_replacing_existing_fourth_value_does_not_add_an_identity_slot");
    const auto empty_counts = ui_search_filter_context(utf8_literal("from:\"\" from: from:\""), 20, 20);
    check(empty_counts.kind == UiSearchFilterKind::from && empty_counts.same_kind_occurrences == 3 &&
              empty_counts.other_value_count == 0,
          "search_filter_context_empty_and_unfinished_quotes_are_not_populated_occurrences");
    constexpr auto literal = utf8_literal("\"from:alice\" on:2026-10-05 before:x after:x future:y");
    check(ui_search_filter_context(literal, 8, 8).kind == UiSearchFilterKind::none &&
              ui_search_filter_context(literal, 20, 20).kind == UiSearchFilterKind::none &&
              ui_search_filter_context(literal, literal.bytes, literal.bytes).kind == UiSearchFilterKind::none,
          "search_filter_context_quoted_free_text_dates_and_unknown_keys_offer_no_values");
    char maximum[kUiSearchFilterQueryBytes + 1];
    copy_bytes(maximum, "from:", 5);
    for (u32 i = 5; i < sizeof(maximum); ++i) maximum[i] = 'a';
    const auto bounded = ui_search_filter_context({maximum, kUiSearchFilterQueryBytes}, 256, 256);
    check(bounded.kind == UiSearchFilterKind::from && bounded.value_bytes == 251 && bounded.prefix_bytes == 251 &&
              ui_search_filter_context({maximum, kUiSearchFilterQueryBytes + 1}, 256, 256).kind == UiSearchFilterKind::none,
          "search_filter_context_exact_query_capacity_is_supported_and_overflow_is_atomic_none");
    const auto copied = unicode;
    check(copied.value().data != unicode.value().data && utf8_equal(copied.value(), unicode.value()),
          "search_filter_context_copy_owns_decoded_bytes_without_self_pointer_aba");
    const auto has = ui_search_filter_fixed_choices(UiSearchFilterKind::has);
    const auto pinned = ui_search_filter_fixed_choices(UiSearchFilterKind::pinned);
    bool unique = has.count == 9;
    for (u32 i = 0; i < has.count; ++i)
        for (u32 j = 0; j < i; ++j)
            unique = unique && !utf8_equal(has[i].value, has[j].value);
    check(unique && utf8_equal(has[8].label, utf8_literal("Forward")) &&
              utf8_equal(has[8].value, utf8_literal("snapshot")) && has[8].semantic == 8,
          "search_filter_fixed_has_choices_are_unique_and_forward_uses_canonical_value");
    check(pinned.count == 2 && utf8_equal(pinned[0].value, utf8_literal("true")) &&
              utf8_equal(pinned[1].value, utf8_literal("false")) &&
              ui_search_filter_fixed_choices(UiSearchFilterKind::from).count == 0,
          "search_filter_fixed_pinned_choices_are_bounded_and_user_choices_are_not_fabricated");
    constexpr auto keys = utf8_literal("authorType:b fileType:p LinkFrom:you filename:\"Q");
    check(
        ui_search_filter_context(keys, 12, 12).kind == UiSearchFilterKind::author_type &&
            ui_search_filter_context(keys, 23, 23).kind == UiSearchFilterKind::file_type &&
            ui_search_filter_context(keys, 36, 36).kind == UiSearchFilterKind::link_from &&
            ui_search_filter_context(keys, keys.bytes, keys.bytes).kind == UiSearchFilterKind::file_name &&
            utf8_equal(ui_search_filter_context(keys, keys.bytes, keys.bytes).prefix(), utf8_literal("Q")),
        "search_filter_context_recognizes_camel_case_keys_without_case");
    const auto authors = ui_search_filter_fixed_choices(UiSearchFilterKind::author_type);
    const auto types = ui_search_filter_fixed_choices(UiSearchFilterKind::file_type);
    bool distinct = types.count == kUiSearchFilterChoiceMaximum;
    for (u32 i = 0; i < types.count; ++i)
        for (u32 j = 0; j < i; ++j)
            distinct = distinct && types[i].id != types[j].id;
    check(authors.count == 3 && utf8_equal(authors[2].value, utf8_literal("webhook")) && distinct &&
              ui_search_filter_fixed_choices(UiSearchFilterKind::link_from).count == 0,
          "search_filter_fixed_author_type_and_file_type_choices_are_bounded_and_distinct");
}
} // namespace maicord
