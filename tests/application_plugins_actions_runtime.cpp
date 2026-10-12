#include "application_plugins_actions.hpp"

namespace maicord::app_detail {
// mc.actions open_modal: the node list maps onto the shared text form.
void test_application_plugins_actions_runtime(void (*check)(bool, const char*) noexcept) noexcept {
    NativePluginSdkModal modal{};
    modal.owner_id = 7;
    modal.modal_id = 9;
    modal.title = utf8_literal("Rename");
    modal.input_max_bytes = 40;
    modal.nodes[0] = {MC_NODE_LABEL, MC_TONE_NEUTRAL, utf8_literal("Pick a new name."), {}};
    modal.nodes[1] = {MC_NODE_SEPARATOR, MC_TONE_NEUTRAL, {}, {}};
    modal.nodes[2] = {MC_NODE_LINK, MC_TONE_ACCENT, utf8_literal("Rules"), utf8_literal("https://example.com/r")};
    modal.nodes[3] = {MC_NODE_INPUT, MC_TONE_NEUTRAL, utf8_literal("Name"), utf8_literal("old")};
    modal.nodes[4] = {MC_NODE_LINK, MC_TONE_NEUTRAL, {}, utf8_literal("https://example.com")};
    modal.node_count = 5;
    char body[256];
    auto form = application_plugins_modal_form(modal, {body, sizeof(body)});
    check(utf8_equal(form.title, utf8_literal("Rename")) &&
              utf8_equal(form.context, utf8_literal("Pick a new name.\n\nRules (https://example.com/r)\nhttps://example.com")) &&
              !form.confirmation_only && form.single_line && form.maximum_bytes == 40 &&
              utf8_equal(form.label, utf8_literal("Name")) && utf8_equal(form.initial, utf8_literal("old")) &&
              utf8_equal(form.accept, utf8_literal("Save")) && !form.danger,
          "plugin modal: labels, separators and links form the body; the input is a single-line field");
    modal.nodes[3] = {MC_NODE_BUTTON, MC_TONE_CRITICAL, utf8_literal("Delete"), {}};
    modal.node_count = 4;
    modal.input_max_bytes = 0;
    form = application_plugins_modal_form(modal, {body, 12});
    check(form.confirmation_only && form.danger && utf8_equal(form.accept, utf8_literal("Delete")) &&
              utf8_equal(form.context, utf8_literal("Pick a new n")) && !form.label.bytes,
          "plugin modal: without an input it is a confirmation; a critical button is dangerous; the body is cut");
    modal.nodes[0] = {MC_NODE_LABEL, MC_TONE_NEUTRAL, utf8_literal("\xC3\xA9\xC3\xA9"), {}};
    modal.nodes[3] = {MC_NODE_INPUT, MC_TONE_NEUTRAL, {}, {}};
    form = application_plugins_modal_form(modal, {body, 3});
    check(utf8_equal(form.context, utf8_literal("\xC3\xA9\n")) && !form.confirmation_only && form.maximum_bytes == 256 &&
              utf8_equal(form.accept, utf8_literal("Save")),
          "plugin modal: the body is cut on a UTF-8 boundary; an unset input limit is 256 bytes");
}
} // namespace maicord::app_detail
