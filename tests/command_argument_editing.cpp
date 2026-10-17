#include "maicord/protocol/application_commands.hpp"
#include "maicord/model/message_composer.hpp"
namespace maicord {
void test_command_argument_editing(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr auto catalog = utf8_literal(
        R"({"applications":[{"id":"10","name":"Search"}],"application_commands":[{"id":"100","application_id":"10","version":"1","name":"find","type":1,"options":[{"name":"scope","type":3},{"name":"query","type":3,"autocomplete":true},{"name":"count","type":4},{"name":"caf\u00e9","type":3}]}]})");
    u32 scratch[2048];
    CommandIndex index{};
    check(command_index_decode(catalog, {scratch, 2048}, &index), "argument editing command schema");
    if (index.command_count != 1)
        return;
    const auto& command = index.commands[0];
    constexpr auto input = utf8_literal(R"(scope:all query:"hello scope:inside" count:2)");
    CommandArgumentFocus focus{};
    check(command_argument_focus(index, command, input, 21, &focus) &&
              utf8_equal(focus.name, utf8_literal("query")) &&
              utf8_equal(focus.value, utf8_literal(R"("hello scope:inside")")) && focus.value_begin == 16 &&
              focus.value_end == 36 && focus.quoted && focus.closed,
          "caret selects a middle argument without treating quoted option-like text as a new argument");
    char query_storage[256];
    Utf8View query{};
    check(command_argument_query(focus, {query_storage, 256}, &query) &&
              utf8_equal(query, utf8_literal("hello scope:inside")),
          "quoted autocomplete value is decoded without quotes");
    u8 bytes[2048];
    ProtocolWriter writer{{bytes, sizeof(bytes)}, 0};
    Utf8View error{};
    check(
        command_arguments(index, command, input, &writer, &error, focus.name, query) &&
            utf8_equal(
                {reinterpret_cast<const char*>(bytes), writer.used},
                utf8_literal(
                    R"([{"name":"scope","type":3,"value":"all"},{"name":"query","type":3,"value":"hello scope:inside","focused":true},{"name":"count","type":4,"value":2}])")),
        "autocomplete retains completed arguments before and after the focus");
    char text[256];
    MessageComposer composer{};
    message_composer_initialize(&composer, {text, sizeof(text)});
    message_composer_insert_utf8(&composer, input);
    check(
        message_composer_replace_range(&composer, focus.value_begin, focus.value_end,
                                       utf8_literal("\"chosen\"")) == MessageComposerResult::changed &&
            utf8_equal(message_composer_text(composer), utf8_literal("scope:all query:\"chosen\" count:2")),
        "accepting a middle suggestion preserves the later arguments exactly");
    check(command_argument_find(index, command, message_composer_text(composer), utf8_literal("query"),
                                &focus) &&
              message_composer_set_selection(&composer, focus.value_begin, focus.value_end) ==
                  MessageComposerResult::changed &&
              composer.bytes == 32 && composer.anchor == 16 && composer.caret == 24,
          "clicking an existing option can select its value without inserting a duplicate");
    message_composer_release(&composer);
    check(!command_argument_focus(index, command, input, 12, &focus) &&
              !command_argument_focus(index, command, input, 10, &focus) &&
              command_argument_focus(index, command, input, input.bytes, &focus) &&
              utf8_equal(focus.name, utf8_literal("count")),
          "option names and between-option padding do not select a value");
    constexpr auto partial = utf8_literal("query:\"hi ");
    check(command_argument_focus(index, command, partial, partial.bytes, &focus) && !focus.closed &&
              command_argument_query(focus, {query_storage, 256}, &query) &&
              utf8_equal(query, utf8_literal("hi ")),
          "unterminated quoted input remains editable and retains its trailing space");
    writer.used = 0;
    check(command_arguments(index, command, partial, &writer, &error, utf8_literal("query"), query),
          "partial focused quotes can request suggestions");
    constexpr auto empty_middle = utf8_literal("query: count:2");
    check(command_argument_find(index, command, empty_middle, utf8_literal("query"), &focus) &&
              !focus.value.bytes && focus.value_begin == 6 && focus.value_end == 6,
          "empty argument preceding another named option has an insertion point");
    writer.used = 0;
    check(
        command_arguments(index, command, empty_middle, &writer, &error, utf8_literal("query"), {}) &&
            utf8_equal(
                {reinterpret_cast<const char*>(bytes), writer.used},
                utf8_literal(
                    R"([{"name":"query","type":3,"value":"","focused":true},{"name":"count","type":4,"value":2}])")),
        "empty focused option does not consume the following option");
    constexpr auto unicode = utf8_literal("caf\xc3\xa9:cr\xc3\xa8me query:ok");
    check(command_argument_focus(index, command, unicode, 10, &focus) &&
              utf8_equal(focus.name, utf8_literal("caf\xc3\xa9")) &&
              !command_argument_focus(index, command, unicode, 9, &focus),
          "escaped Unicode schema names match typed UTF-8 and reject caret offsets inside a scalar");
    writer.used = 0;
    check(command_arguments(index, command, unicode, &writer, &error),
          "escaped non-ASCII option names also serialize correctly");
    command_index_release(&index);
}
} // namespace maicord
