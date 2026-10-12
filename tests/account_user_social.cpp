#include "maicord/app/account_user_social.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "test_output.hpp"

// Capture-the-submission: notes, report menus and submissions, and message
// flag edits reach the transport with the official client's target, method,
// bucket and body. Report bodies are built from a captured-shape menu.
namespace maicord {
namespace user_social_account_test {
u32 failures;
struct Capture {
    RestRouteKey route;
    u64 id;
    u32 submissions, cancellations, target_bytes, body_bytes;
    u8 target[kRestClientMaximumTargetBytes], body[kUserSocialMaximumBodyBytes];
    bool json;
    RestClientSubmitError result;
} capture;
u32 structurals[16384];

void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
bool same(const u8* data, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(data), bytes}, expected);
}
bool submitted(const UserSocialRequest& r, RestMethod method, RestMajorParameter major, u64 major_key,
               Utf8View target, Utf8View body) noexcept {
    RestClient* rest = reinterpret_cast<RestClient*>(&capture);
    const u32 before = capture.submissions;
    if (!account_user_social_submit(rest, &r, 41).ok() || capture.submissions != before + 1)
        return false;
    return capture.id == 41 && capture.route.method == method && capture.route.major == major &&
           capture.route.major_key == major_key && (capture.route.route_id >> 16) == (kUserSocialRouteBase >> 16) &&
           capture.json == (body.bytes != 0) && same(capture.target, capture.target_bytes, target) &&
           same(capture.body, capture.body_bytes, body);
}

void test_notes() noexcept {
    using M = RestMethod;
    using P = RestMajorParameter;
    UserSocialRequest get{};
    get.operation = UserSocialOperation::note_get;
    get.user_id = 80351110224678912ull;
    check(submitted(get, M::get, P::none, 0, utf8_literal("/api/v9/users/@me/notes/80351110224678912"), {}),
          "user social: a note is read from the user's notes route");
    UserSocialRequest put = get;
    put.operation = UserSocialOperation::note_put;
    put.text = utf8_literal("Met at \"Rust\" meetup\nlikes tea");
    check(submitted(put, M::put, P::none, 0, utf8_literal("/api/v9/users/@me/notes/80351110224678912"),
                    utf8_literal(R"({"note":"Met at \"Rust\" meetup\nlikes tea"})")),
          "user social: saving a note PUTs the escaped note");
    put.text = {};
    check(submitted(put, M::put, P::none, 0, utf8_literal("/api/v9/users/@me/notes/80351110224678912"),
                    utf8_literal(R"({"note":""})")),
          "user social: an empty note clears it");
    char long_note[1100];
    for (auto& c : long_note)
        c = 'n';
    put.text = {long_note, 257};
    RestClient* rest = reinterpret_cast<RestClient*>(&capture);
    const u32 before = capture.submissions;
    check(account_user_social_submit(rest, &put, 1).error == RestClientSubmitError::invalid_input &&
              capture.submissions == before,
          "user social: a 257-code-point note never submits");
    put.text = {long_note, 256};
    check(user_social_request_valid(put), "user social: a 256-code-point note is accepted");

    UserNoteRecord note{};
    check(user_note_decode(utf8_literal(R"({"user_id":"1","note_user_id":"42","note":"hi é"})"),
                           {structurals, 16384}, false, &note) &&
              note.user_id == 42 && utf8_equal({note.text, note.bytes}, utf8_literal("hi \xC3\xA9")),
          "user social: the note response names its user and decodes escapes");
    check(user_note_decode(utf8_literal(R"({"id":"42","note":null})"), {structurals, 16384}, true, &note) &&
              note.user_id == 42 && note.bytes == 0,
          "user social: USER_NOTE_UPDATE with a null note clears it");
    check(!user_note_decode(utf8_literal(R"({"id":"42","note":"x"})"), {structurals, 16384}, false, &note),
          "user social: a REST response without note_user_id is rejected");
}

void test_message_flags() noexcept {
    UserSocialRequest flags{};
    flags.operation = UserSocialOperation::message_flags;
    flags.channel_id = 20;
    flags.message_id = 30;
    flags.flags = 4 | 32;
    check(submitted(flags, RestMethod::patch, RestMajorParameter::channel, 20,
                    utf8_literal("/api/v9/channels/20/messages/30"), utf8_literal(R"({"flags":36})")),
          "user social: Remove embeds PATCHes the complete flags with SUPPRESS_EMBEDS set");
}

// Shape of GET /reporting/menu/message: nodes keyed by ID, child options as
// [label, target] tuples and checkbox data as [value, label, description].
constexpr char kMenu[] = R"({"name":"message","variant":"6","version":"1.0","language":"en",
"postback_url":"/reporting/message","root_node_id":1,"success_node_id":100,"fail_node_id":101,
"nodes":{
 "1":{"id":1,"key":"start","header":"What's going on?","subheader":"Pick the closest match.","info":null,
      "button":null,"elements":[{"name":"message_preview","type":"message_preview","data":null,"should_submit_data":false}],
      "report_type":null,"children":[["Spam",2],["Something else",3]],"is_multi_select_required":false,"is_auto_submit":false},
 "2":{"id":2,"key":"spam","header":"What kind of spam?","subheader":null,"info":"Select all that apply.",
      "button":{"type":"submit","target":100},
      "elements":[{"name":"spam_kind","type":"checkbox","should_submit_data":true,
                   "data":[["ads","Advertising","Promotions or links"],["scam","Scam \"giveaway\"",null],["bots","Bot messages"]]},
                  {"name":"details","type":"free_text","should_submit_data":true,"data":null}],
      "children":[],"is_multi_select_required":true,"is_auto_submit":false},
 "3":{"id":3,"key":"other","header":"Tell us more","button":{"type":"next","target":4},"elements":[{"name":"note","type":"text","data":{"header":"Heads up","body":"Discord reviews every report."}}],"children":[]},
 "4":{"id":4,"key":"other_submit","header":"Submit","button":{"type":"submit"},"elements":[],"children":[],"is_auto_submit":true},
 "100":{"id":100,"key":"success","header":"Thanks for the report","button":{"type":"done"},"elements":[{"name":"success","type":"success","data":null}],"children":[]},
 "101":{"id":101,"key":"fail","header":"Something went wrong","button":{"type":"cancel"},"elements":[],"children":[]}
}})";

void test_report_menu() noexcept {
    using M = RestMethod;
    using P = RestMajorParameter;
    UserSocialRequest menu_request{};
    menu_request.operation = UserSocialOperation::report_menu;
    menu_request.report = ReportMenuName::message;
    check(submitted(menu_request, M::get, P::none, 0, utf8_literal("/api/v9/reporting/menu/message"), {}),
          "user social: the message report menu is fetched by name");
    menu_request.report = ReportMenuName::user;
    check(submitted(menu_request, M::get, P::none, 0, utf8_literal("/api/v9/reporting/menu/user"), {}),
          "user social: the user report menu is fetched by name");

    ReportMenu menu{};
    check(report_menu_decode({kMenu, sizeof(kMenu) - 1}, {structurals, 16384}, &menu) && menu.node_count == 6 &&
              menu.root_node_id == 1 && menu.success_node_id == 100 && menu.fail_node_id == 101 &&
              utf8_equal(menu.version_json, utf8_literal("\"1.0\"")) &&
              utf8_equal(menu.variant_json, utf8_literal("\"6\"")),
          "user social: the report menu decodes its nodes, root and terminal nodes");
    const auto* root = report_menu_node(menu, 1);
    const auto* spam = report_menu_node(menu, 2);
    const auto* other = report_menu_node(menu, 3);
    const auto* final_node = report_menu_node(menu, 4);
    check(root && root->child_count == 2 && utf8_equal(menu.children[root->child_first].label, utf8_literal("Spam")) &&
              menu.children[root->child_first + 1].target == 3 && root->button == ReportButtonKind::none &&
              root->element_count == 1 && menu.elements[root->element_first].kind == ReportElementKind::message_preview,
          "user social: child options keep their labels and targets");
    check(spam && spam->button == ReportButtonKind::submit && spam->button_target == 100 &&
              spam->multi_select_required && spam->element_count == 2 &&
              menu.elements[spam->element_first].kind == ReportElementKind::checkbox &&
              menu.elements[spam->element_first].option_count == 3 &&
              utf8_equal(menu.options[menu.elements[spam->element_first].option_first + 1].label,
                         utf8_literal("Scam \"giveaway\"")) &&
              utf8_equal(menu.options[menu.elements[spam->element_first].option_first].description,
                         utf8_literal("Promotions or links")) &&
              menu.elements[spam->element_first + 1].kind == ReportElementKind::free_text &&
              utf8_equal(spam->info, utf8_literal("Select all that apply.")),
          "user social: checkbox groups decode values, labels and descriptions");
    check(other && other->button == ReportButtonKind::next && other->button_target == 4 &&
              utf8_equal(menu.elements[other->element_first].header, utf8_literal("Heads up")) && final_node &&
              final_node->auto_submit,
          "user social: text blocks, next buttons and auto-submit nodes decode");

    const u32 path[]{1, 2};
    const ReportAnswer answers[]{{spam->element_first, 0b101, {}},
                                 {spam->element_first + 1, 0, utf8_literal("They post \"free nitro\" links")}};
    ReportSubmission submission{ReportMenuName::message, 20, 30, 0, 0, {path, 2}, {answers, 2}};
    u8 body[kUserSocialMaximumBodyBytes];
    const auto written = report_write_submit(menu, submission, {body, sizeof(body)});
    const auto expected = utf8_literal(
        R"({"version":"1.0","variant":"6","language":"en","breadcrumbs":[1,2],"elements":{"spam_kind":["ads","bots"],"details":"They post \"free nitro\" links"},"channel_id":"20","message_id":"30","name":"message"})");
    check(written.ok() && same(body, written.bytes, expected),
          "user social: a message report lists the path, selected values and the message");
    UserSocialRequest submit{};
    submit.operation = UserSocialOperation::report_submit;
    submit.report = ReportMenuName::message;
    submit.text = {reinterpret_cast<const char*>(body), written.bytes};
    check(submitted(submit, M::post, P::none, 0, utf8_literal("/api/v9/reporting/message"), expected),
          "user social: the report POSTs the built body to the menu's postback");

    const u32 user_path[]{1, 3, 4};
    ReportSubmission user{ReportMenuName::user, 0, 0, 77, 10, {user_path, 3}, {}};
    const auto user_written = report_write_submit(menu, user, {body, sizeof(body)});
    check(user_written.ok() &&
              same(body, user_written.bytes,
                   utf8_literal(R"({"version":"1.0","variant":"6","language":"en","breadcrumbs":[1,3,4],"elements":{},"guild_id":"10","user_id":"77","name":"user"})")),
          "user social: a user report names the contextual server before the user");
    const u32 bad_path[]{1, 9};
    ReportSubmission bad{ReportMenuName::message, 20, 30, 0, 0, {bad_path, 2}, {}};
    ReportSubmission no_message{ReportMenuName::message, 20, 0, 0, 0, {path, 2}, {}};
    check(!report_write_submit(menu, bad, {body, sizeof(body)}).ok() &&
              !report_write_submit(menu, no_message, {body, sizeof(body)}).ok(),
          "user social: unknown nodes or a missing message never build a report");
    report_menu_release(&menu);
    check(!report_menu_decode(utf8_literal(R"({"nodes":{}})"), {structurals, 16384}, &menu),
          "user social: a menu without a root node is rejected");
}

void test_rejections() noexcept {
    RestClient* rest = reinterpret_cast<RestClient*>(&capture);
    const u32 before = capture.submissions;
    UserSocialRequest none{};
    none.operation = UserSocialOperation::note_get;
    UserSocialRequest flags{};
    flags.operation = UserSocialOperation::message_flags;
    flags.channel_id = 1;
    UserSocialRequest submit{};
    submit.operation = UserSocialOperation::report_submit;
    submit.text = utf8_literal("not json");
    UserSocialRequest bad{};
    bad.operation = UserSocialOperation::count;
    check(account_user_social_submit(rest, &none, 1).error == RestClientSubmitError::invalid_input &&
              account_user_social_submit(rest, &flags, 1).error == RestClientSubmitError::invalid_input &&
              account_user_social_submit(rest, &submit, 1).error == RestClientSubmitError::invalid_input &&
              account_user_social_submit(rest, &bad, 1).error == RestClientSubmitError::invalid_input &&
              account_user_social_submit(rest, nullptr, 1).error == RestClientSubmitError::invalid_input &&
              account_user_social_submit(nullptr, &flags, 1).error == RestClientSubmitError::invalid_input &&
              capture.submissions == before,
          "user social: missing IDs, non-JSON reports and unknown operations never submit");
    capture.result = RestClientSubmitError::busy;
    UserSocialRequest get{};
    get.operation = UserSocialOperation::note_get;
    get.user_id = 5;
    check(account_user_social_submit(rest, &get, 5).error == RestClientSubmitError::busy,
          "user social: transport backpressure propagates");
    capture.result = RestClientSubmitError::none;
    check(account_user_social_cancel(rest, 5).ok() && capture.cancellations == 1 &&
              account_user_social_cancel(rest, 0).error == RestClientSubmitError::invalid_input,
          "user social: cancel names exactly one request");
}
} // namespace user_social_account_test

RestClientSubmitResult rest_client_submit(RestClient*,
                                          const RestClientRequest& request) noexcept {
    auto& c = user_social_account_test::capture;
    ++c.submissions;
    c.route = request.route;
    c.id = request.request_id;
    c.json = request.json_body;
    if (request.target.count > sizeof(c.target) || request.body.count > sizeof(c.body))
        return {RestClientSubmitError::invalid_input};
    c.target_bytes = request.target.count;
    c.body_bytes = request.body.count;
    copy_bytes(c.target, request.target.data, request.target.count);
    if (request.body.count)
        copy_bytes(c.body, request.body.data, request.body.count);
    return {c.result};
}

bool rest_client_cancel(RestClient*, u64 request_id) noexcept {
    ++user_social_account_test::capture.cancellations;
    return request_id == 5;
}
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_account_user_social_entry() noexcept {
    using namespace maicord::user_social_account_test;
    test_notes();
    test_message_flags();
    test_report_menu();
    test_rejections();
    constexpr char pass[] = "PASS user social requests\n";
    constexpr char fail[] = "FAIL user social requests\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failures ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failures ? 1 : 0);
}
