#include "maicord/protocol/application_commands.hpp"
#include "maicord/model/message_composer.hpp"
namespace maicord {
void test_command_argument_editing(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr auto catalog = utf8_literal(
        R"({"applications":[{"id":"10","name":"Search"}],"application_commands":[{"id":"100","application_id":"10","version":"1","name":"find","type":1,"options":[{"name":"scope","type":3},{"name":"query","type":3,"autocomplete":true},{"name":"count","type":4,"min_value":1,"max_value":10},{"name":"caf\u00e9","type":3},{"name":"ratio","type":10},{"name":"title","type":3,"required":true,"min_length":2,"max_length":4},{"name":"size","type":4,"choices":[{"name":"Two","value":2}]},{"name":"flag","type":5},{"name":"who","type":6}]}]})");
    u32 scratch[2048];
    CommandIndex index{};
    check(command_index_decode(catalog, {scratch, 2048}, &index), "argument editing command schema");
    if (index.command_count != 1)
        return;
    const auto& command = index.commands[0];
    // Composer keys: a known option name not used yet, after a
    // blank; its value runs to the blank before the next key.
    constexpr auto input = utf8_literal("scope:all query:hello scope:inside count:2");
    CommandArgumentSpan spans[25];
    u32 count = command_argument_spans(index, command, input, {spans, 25});
    check(count == 3 && utf8_equal(spans[1].name, utf8_literal("query")) &&
              utf8_equal(spans[1].value, utf8_literal("hello scope:inside")) && spans[1].key_first == 10 &&
              spans[1].value_first == 16 && spans[1].value_last == 34 && spans[0].value_last == 9 &&
              spans[2].value_first == 41 && spans[2].value_last == input.bytes,
          "an option name already used is value text, and values run to the blank before the next key");
    constexpr auto link = utf8_literal("scope:see https://example.com/a:b");
    check(command_argument_spans(index, command, link, {spans, 25}) == 1 &&
              utf8_equal(spans[0].value, utf8_literal("see https://example.com/a:b")),
          "words ending in ':' that name no option stay in the value");
    u8 bytes[2048];
    ProtocolWriter writer{{bytes, sizeof(bytes)}, 0};
    Utf8View error{};
    check(command_arguments(index, command, input, &writer, &error, utf8_literal("query"),
                            utf8_literal("hello scope:inside")) &&
              utf8_equal({reinterpret_cast<const char*>(bytes), writer.used},
                         utf8_literal(R"([{"name":"scope","type":3,"value":"all"},{"name":"query","type":3,"value":"hello scope:inside","focused":true},{"name":"count","type":4,"value":2}])")),
          "autocomplete retains completed arguments before and after the focus");
    char text[256];
    MessageComposer composer{};
    message_composer_initialize(&composer, {text, sizeof(text)});
    message_composer_insert_utf8(&composer, input);
    count = command_argument_spans(index, command, input, {spans, 25});
    check(count == 3 &&
              message_composer_replace_range(&composer, spans[1].value_first, spans[1].value_last,
                                             utf8_literal("chosen")) == MessageComposerResult::changed &&
              utf8_equal(message_composer_text(composer), utf8_literal("scope:all query:chosen count:2")),
          "accepting a middle suggestion preserves the later arguments exactly");
    message_composer_release(&composer);
    constexpr auto empty_middle = utf8_literal("query: count:2");
    check(command_argument_spans(index, command, empty_middle, {spans, 25}) == 2 && !spans[0].value.bytes &&
              spans[0].value_first == 6 && spans[0].value_last == 6,
          "an empty option before another has an insertion point");
    writer.used = 0;
    check(command_arguments(index, command, empty_middle, &writer, &error, utf8_literal("query"), {}) &&
              utf8_equal({reinterpret_cast<const char*>(bytes), writer.used},
                         utf8_literal(R"([{"name":"query","type":3,"value":"","focused":true},{"name":"count","type":4,"value":2}])")),
          "an empty focused option does not consume the following option");
    constexpr auto unicode = utf8_literal("caf\xc3\xa9:cr\xc3\xa8me query:ok title:abc");
    writer.used = 0;
    check(command_argument_spans(index, command, unicode, {spans, 25}) == 3 &&
              utf8_equal(spans[0].name, utf8_literal("caf\xc3\xa9")) &&
              command_arguments(index, command, unicode, &writer, &error),
          "escaped non-ASCII option names match typed UTF-8 and serialize");
    // The checks and their messages.
    command_argument_spans(index, command, utf8_literal("count:x ratio:1 title:a size:Three flag:yes who:bob"),
                           {spans, 25});
    char message[160];
    const auto check_span = [&](u32 n, bool allow_empty) noexcept {
        return command_option_check(index, spans[n].option, true, spans[n].value, allow_empty,
                                    {message, sizeof(message)});
    };
    check(utf8_equal(check_span(0, false), utf8_literal("Input a valid integer.")) && !check_span(1, false).bytes &&
              utf8_equal(check_span(2, false), utf8_literal("Enter between 2 and 4 characters.")) &&
              utf8_equal(check_span(3, false), utf8_literal("Not a valid choice.")) &&
              utf8_equal(check_span(4, false), utf8_literal("Pick either true or false.")) &&
              utf8_equal(check_span(5, false), utf8_literal("Not a valid user.")),
          "option checks give the client's per-type messages");
    command_argument_spans(index, command, utf8_literal("count:11 title: who:<@!42>"), {spans, 25});
    check(utf8_equal(check_span(0, false), utf8_literal("Enter a number between 1 and 10.")) &&
              utf8_equal(check_span(1, false), utf8_literal("This option is required. Specify a value.")) &&
              !check_span(1, true).bytes && !check_span(2, false).bytes &&
              utf8_equal(command_option_check(index, spans[1].option, false, {}, true, {message, sizeof(message)}),
                         utf8_literal("This option is required. Specify a value.")),
          "ranges, empty and absent required options and mentions are checked like the client");
    writer.used = 0;
    check(!command_arguments(index, command, utf8_literal("count:4"), &writer, &error) &&
              utf8_equal(error, utf8_literal("This option is required. Specify a value.")),
          "a missing required option stops the send with the client's message");
    writer.used = 0;
    check(command_arguments(index, command, utf8_literal("count:007 ratio:+.5 title:ab size:Two flag:TRUE who:<@!42>"),
                            &writer, &error) &&
              utf8_equal({reinterpret_cast<const char*>(bytes), writer.used},
                         utf8_literal(R"([{"name":"count","type":4,"value":7},{"name":"ratio","type":10,"value":0.5},{"name":"title","type":3,"value":"ab"},{"name":"size","type":4,"value":2},{"name":"flag","type":5,"value":true},{"name":"who","type":6,"value":"42"}])")),
          "typed numbers serialize as JSON numbers, choices as their values and mentions as ids");
    // Number normalization: en-US group commas go,
    // 0x/0o/0b integers parse, and Infinity is a number outside every range.
    writer.used = 0;
    check(command_arguments(index, command, utf8_literal("count:0x0A ratio:1,000.50 title:ab"), &writer, &error) &&
              utf8_equal({reinterpret_cast<const char*>(bytes), writer.used},
                         utf8_literal(R"([{"name":"count","type":4,"value":10},{"name":"ratio","type":10,"value":1000.5},{"name":"title","type":3,"value":"ab"}])")),
          "hexadecimal integers and grouped numbers serialize as their values");
    command_argument_spans(index, command, utf8_literal("ratio:Infinity count:0b101"), {spans, 25});
    check(check_span(0, false).bytes && !check_span(1, false).bytes,
          "Infinity is refused by the number range while binary integers pass");
    // A chip value ends at its end marker (U+2063, never sent): text after it
    // is outside the chip, and a name typed inside the value stays text.
    constexpr auto ended = utf8_literal("query:a title:b\xE2\x81\xA3 tail count:3\xE2\x81\xA3");
    count = command_argument_spans(index, command, ended, {spans, 25});
    check(count == 2 && utf8_equal(spans[0].value, utf8_literal("a title:b")) && spans[0].value_last == 15 &&
              spans[0].end_last == 18 && utf8_equal(spans[1].name, utf8_literal("count")) &&
              utf8_equal(spans[1].value, utf8_literal("3")) && spans[1].end_last == ended.bytes,
          "a terminated value keeps typed option names and the text after its end is outside every chip");
    writer.used = 0;
    check(!command_arguments(index, command, ended, &writer, &error) &&
              utf8_equal(error, utf8_literal("This option is required. Specify a value.")),
          "an option name inside another value is not that option");
    const CommandAcceptedChoice accepted[]{{utf8_literal("query"), utf8_literal("Best match"),
                                            utf8_literal("best-id"), JsonValueKind::string}};
    writer.used = 0;
    check(command_arguments(index, command, utf8_literal("query:Best match title:ab"), &writer, &error, {}, {}, {},
                            nullptr, {accepted, 1}) &&
              utf8_equal({reinterpret_cast<const char*>(bytes), writer.used},
                         utf8_literal(R"([{"name":"query","type":3,"value":"best-id"},{"name":"title","type":3,"value":"ab"}])")),
          "a taken autocomplete choice shows its name and sends its value");
    check(command_choice_score(utf8_literal("Two"), utf8_literal("two")) == 10 &&
              command_choice_score(utf8_literal("Twenty"), utf8_literal("tw")) == 7 &&
              command_choice_score(utf8_literal("Network"), utf8_literal("two")) == 5 &&
              command_choice_score(utf8_literal("Big red"), utf8_literal("red big")) == 3 &&
              command_choice_score(utf8_literal("Tomorrow"), utf8_literal("tmw")) == 1 &&
              !command_choice_score(utf8_literal("Tomorrow"), utf8_literal("x")),
          "choices rank like the client's queryChoice");
    command_index_release(&index);
}
} // namespace maicord
