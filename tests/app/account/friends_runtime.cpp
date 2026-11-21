#include "account/user_actions.hpp"
#include "../../../src/app/account/friends.cpp"
#include "shell/state.hpp"
#include "../../support/account_module_fake.hpp"
#include "account/friends.hpp"
#include "chat/application_private_conversation.hpp"
#include "account/profile.hpp"
#include "chat/messages/interaction.hpp"
#include "chat/composer/text_form.hpp"
#include "account/friends_search.hpp"
#include "account/friends_storage.hpp"
#include "plugins/plugins.hpp"
#include "maicord/app/account/account_relationships.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/platform/window/clipboard.hpp"
#include "../../support/application_action_captcha_stubs.hpp"
#include "../../support/test_output.hpp"

#include "maicord/platform/window/key.hpp"
#include "../../support/window_fixture.hpp"
#include "../../support/released_memory.hpp"

namespace maicord::friends_runtime_test {
using namespace app_detail;
// Commit and admission metrics of the controller's relationship banks, read from its private state.
struct FriendsStorageBankMetrics {
    usize reserved_bytes, committed_bytes;
    u32 admitted_count, commit_count;
    const void* address;
};
struct ApplicationFriendsStorageMetrics {
    usize controller_bytes;
    FriendsStorageBankMetrics active, candidate, indices, requests_indices;
};
FriendsStorageBankMetrics bank_metrics(const StableStorage& bank) noexcept {
    return {bank.arena.reserved, bank.arena.committed, bank.capacity, bank.arena.commit_count, bank.arena.base};
}
ApplicationFriendsStorageMetrics application_friends_storage_metrics() noexcept {
    const auto* friends = g_app.friends;
    if (!friends) return {sizeof(ApplicationFriends), {}, {}, {}, {}};
    return {sizeof(ApplicationFriends), bank_metrics(friends->storage.active), bank_metrics(friends->storage.candidate),
        bank_metrics(friends->storage.indices), bank_metrics(friends->requests_indices)};
}
struct AccountFixture { RestClient rest; } account{};
UiWidget popup_widgets[32]{};UiLayoutNode popup_nodes[32]{};u8 modal_token{};
struct Observations {
    u32 failures, submissions, builds, form_opens, input_layouts, input_syncs;
    u64 request_id, user_id, profile_user, dm_user, call_channel, request_channel;
    RelationshipOperation operation;
    RestMethod method;
    RestClientSubmitError admission;
    bool settings, fail_build, requests_mode;
    char body[256]; u32 body_bytes;
    char target[128];u32 target_bytes;
    char headers[128];u32 header_bytes; // the request's extra header lines (X-Context-Properties)
    u32 response_capacity; // of the last large-response submission
    char clipboard[kFriendsSearchBytes];u32 clipboard_bytes;
    ApplicationTextFormRequest form;
} seen{};
void check(bool valid, const char* label) noexcept {
    test_check(&seen.failures, valid, label);
}
// The X-Context-Properties line of {"location":"Friends"}.
constexpr auto kFriendsContext = utf8_literal("X-Context-Properties: eyJsb2NhdGlvbiI6IkZyaWVuZHMifQ==\r\n");
bool context_is(Utf8View expected) noexcept {
    return utf8_equal({seen.headers, seen.header_bytes}, expected);
}
enum SyntheticModifiers : u8 { synthetic_control = 1, synthetic_shift = 2, synthetic_alt = 4 };
bool synthetic_input(UiInputKind kind, u32 code, bool repeated, u8 modifiers = 0) noexcept {
    // The fixture supplies a key message directly. Its modifiers must belong to that message: the headless window's
    // key table holds exactly those, and nothing else, for the call.
    const bool control = (modifiers & synthetic_control) != 0;
    const bool shift = (modifiers & synthetic_shift) != 0;
    const bool alt = (modifiers & synthetic_alt) != 0;
    null_window_release_keys();
    if (control) window_test::hold(key::ctrl);
    if (shift) window_test::hold(key::shift);
    if (alt) window_test::hold(key::alt);
    const bool applied = window_key_down(key::ctrl) == control && window_key_down(key::shift) == shift &&
        window_key_down(key::alt) == alt;
    check(applied, "friends fixture: synthetic message owns exact modifiers");
    const bool handled = applied && application_friends_input(kind, code, repeated);
    null_window_release_keys();
    return handled;
}
UiFriendsScene* main_scene() noexcept { return const_cast<UiFriendsScene*>(g_app.renderer.friends_scene); }
UiFriendsScene* scene() noexcept { return const_cast<UiFriendsScene*>(application_friends_requests_active()?
    g_app.renderer.friends_requests_scene:g_app.renderer.friends_scene); }
bool publish(void*, const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    application_friends_gateway(event, payload); return true;
}
bool snapshot(Utf8View json, u64 sequence, Utf8View presences = {}) noexcept {
    u32 array_scratch[4096]{}, object_scratch[1024]{};
    return gateway_client_publish_relationship_snapshot({json, 0, JsonValueKind::array},
        presences.bytes ? JsonValue{presences, 0, JsonValueKind::object} : JsonValue{},
        {array_scratch, 4096}, {object_scratch, 1024}, sequence, 1000, publish, nullptr).ok();
}
// One relationship dispatch through the real Gateway route.
bool relationship_event(Utf8View name, GatewayClientDispatchKind kind, Utf8View json, u64 sequence,
                        bool synchronize) noexcept {
    u32 scratch[1024]{};char storage[512]{};
    GatewayEnvelope envelope{};
    envelope.event_name = {name, 0, false};
    envelope.data = {json, 0, JsonValueKind::object};
    envelope.has_event_name = true;envelope.has_sequence = true;envelope.sequence = sequence;
    GatewayClientEvent event{};ConstSlice<u8> payload{};
    const auto routed = gateway_client_route_dispatch(envelope, {scratch, 1024}, 64, {storage, 512}, {}, 4, &event, &payload);
    if (!routed.ok() || event.dispatch_kind != kind) return false;
    application_friends_gateway(event, payload);
    return !synchronize || application_friends_synchronize();
}
bool remove_relationship(Utf8View json, u64 sequence, bool synchronize = true) noexcept {
    return relationship_event(utf8_literal("RELATIONSHIP_REMOVE"), GatewayClientDispatchKind::relationship_remove, json,
                              sequence, synchronize);
}
bool add_relationship(Utf8View json, u64 sequence) noexcept {
    return relationship_event(utf8_literal("RELATIONSHIP_ADD"), GatewayClientDispatchKind::relationship_add, json, sequence,
                              true);
}
bool action(UiFriendsAction operation, u64 user = 0, UiFriendsTab tab = UiFriendsTab::all,UiFriendsSectionKind section=UiFriendsSectionKind::incoming) noexcept {
    auto* current = scene();
    // Tab leaves the fields (Add Friend's username, then its note).
    for(u32 field=0;field<3&&application_friends_search_focused();++field){
        synthetic_input(UiInputKind::key_down,key::tab,false);
        synthetic_input(UiInputKind::key_up,key::tab,false);
    }
    if(operation==UiFriendsAction::profile||operation==UiFriendsAction::remove||operation==UiFriendsAction::block||
        operation==UiFriendsAction::ignore||operation==UiFriendsAction::unignore||operation==UiFriendsAction::nickname||
        operation==UiFriendsAction::voice_call){
        if(!current||!action(UiFriendsAction::more,user,tab)||!g_app.popup_scene.active||g_app.popup_scene.owner_id!=user)return false;
        return application_friends_popup_action(0xF100u+static_cast<u32>(operation));
    }
    if (!current || !ui_friends_scene_focus(current, {operation, user, tab,section})) return false;
    g_app.context_focus = &current->widgets;
    const bool down = synthetic_input(UiInputKind::key_down, key::enter, false);
    const bool up = synthetic_input(UiInputKind::key_up, key::enter, false);
    return down && up;
}
bool save_form(Utf8View text={}) noexcept {
    const auto form=seen.form;
    if(!form.valid||!form.save||!form.valid(form.owner)||!form.save(form.owner,text))return false;
    g_app.text_form=nullptr;return true;
}
void cancel_form() noexcept {g_app.text_form=nullptr;}
bool replace_query(Utf8View query) noexcept {
    const auto target=application_friends_search_target();u32 length=0,units=0,written=0,next=0;u16 input[128]{};TextInputChange change{};
    if(!text_input_ready(target)||text_input_length(target,&length)==TextInputResult::invalid_input||
        !utf8_utf16_position_for_byte_offset(query,query.bytes,&units)||
        !utf8_copy_utf16_range(query,0,units,{input,128},&written,&next)||next!=units)return false;
    const auto result=text_input_replace(target,0,length,{input,written},&change);
    return (result==TextInputResult::changed||result==TextInputResult::unchanged)&&application_friends_search_changed(result==TextInputResult::changed);
}
bool tab(UiFriendsTab value) noexcept { return action(UiFriendsAction::select_tab, 0, value); }
bool row(u64 id, UiFriendsRow* output) noexcept {
    const auto* current = scene();
    if (!current || !current->source || !current->source->find || !current->source->resolve) return false;
    u32 index = 0;
    return current->source->find(current->source->context, id, &index) &&
        current->source->resolve(current->source->context, index, output);
}
bool response(u64 id, u16 status, Utf8View body = {}, u64 retry_at = 0, ApiRejection rejection = {}) noexcept {
    RestClientResponse completed{};
    completed.request_id = id;completed.status_code = status;completed.retry_at_ms = retry_at;
    completed.body_bytes = body.bytes;completed.rejection = rejection;
    return application_friends_response(completed, body);
}
bool recovery_error(u64 sequence) noexcept {
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;event.sequence = sequence;
    event.dispatch_kind = GatewayClientDispatchKind::relationship_snapshot_begin;
    application_friends_gateway(event, {});
    event.dispatch_kind = GatewayClientDispatchKind::relationship_snapshot_complete;
    event.state.record_count = 1; // Declares a missing item: keep the old active model.
    application_friends_gateway(event, {});
    return application_friends_synchronize() && scene() && scene()->phase == UiFriendsPhase::error;
}
RestClientSubmitResult WINAPI submit(void* instance, RelationshipOperation operation,
    u64 user, Utf8View username, u64 id, RelationshipLocation location) noexcept {
    if (instance != &account) return {RestClientSubmitError::invalid_input};
    seen.operation = operation;seen.user_id = user;
    return account_relationship_submit(&account.rest, operation, user, username, id, location);
}
void context_menu_coordinates() noexcept {
    // Context menus arrive at content pixels; the sidebar's point is not Friends'.
    check(application_friends_window_event(window_test::context_menu(600, 150)) && !application_friends_window_event(window_test::context_menu(100, 150)),
        "friends: context menu is swallowed in Friends content and leaves private sidebar available");
    g_app.context_focus = &scene()->widgets;
    check(application_friends_window_event(window_test::context_menu_key()),
        "friends: keyboard context menu is swallowed when Friends owns logical focus");
    g_app.context_focus = &g_app.shell_scene.widgets;
    check(!application_friends_window_event(window_test::context_menu_key()),
        "friends: keyboard context menu preserves shell/private-sidebar routing");
    g_app.context_focus = &scene()->widgets;
    g_app.window = nullptr;
}
bool only_row(u64 id) noexcept {
    UiFriendsRow result{};return scene()&&scene()->source_count==1&&row(id,&result);
}
void search_runtime_fixture() noexcept {
    constexpr auto users=utf8_literal(R"([{"id":"101","type":1,"nickname":"Separate Nick","user":{"id":"101","username":"needle.user","global_name":"Display One"}},{"id":"102","type":1,"nickname":"Unique nickname","user":{"id":"102","username":"other.name","global_name":"Display Needle"}},{"id":"103","type":3,"user":{"id":"103","username":"pending.name","global_name":"Pending Person"}},{"id":"104","type":2,"user":{"id":"104","username":"block.account","global_name":"Blocked Person"}},{"id":"105","type":1,"nickname":"Different nickname","user":{"id":"105","username":"unicode.user","global_name":"\u017diga Native"}}])");
    check(snapshot(users,100,utf8_literal(R"({"friends":[{"user_id":"101","status":"online"},{"user_id":"102","status":"offline"},{"user_id":"105","status":"idle"}]})"))&&
        application_friends_synchronize()&&tab(UiFriendsTab::all)&&scene()->source_count==3,
        "friends search: named authoritative fixture preserves distinct username, nickname and global name fields");
    auto* all=application_friends_search_model();
    check(all&&all->capacity==kFriendsSearchBytes&&!all->owns_storage&&!all->owned_storage.base&&!all->history&&!all->bytes&&
        !text_input_ready(application_friends_search_target()),
        "friends search: inactive 128-byte document borrows its controller bank and allocates no editor or history");
    if(!all)return;
    const auto notifications=seen.input_layouts, synchronizations=seen.input_syncs;
    check(action(UiFriendsAction::search)&&application_friends_search_focused()&&text_input_ready(application_friends_search_target())&&
        all->history&&message_composer_history_costs(*all).payload_bytes==2048&&!all->owned_storage.base&&
        seen.input_layouts>notifications&&seen.input_syncs>synchronizations,
        "friends search: semantic focus lazily creates bounded history and synchronizes native text geometry");
    check(replace_query(utf8_literal("NEEDLE.USER"))&&only_row(101)&&scene()->content->total_count==3,
        "friends search: username matching ignores case and retains pre-filter tab count");
    check(replace_query(utf8_literal("unique NICK"))&&only_row(102),
        "friends search: relationship nickname matches independently of username and global name");
    check(replace_query(utf8_literal("display needle"))&&only_row(102),
        "friends search: global name remains searchable when a different nickname is displayed");
    constexpr char unicode_query[]={static_cast<char>(0xC5),static_cast<char>(0xBE),'I','G','A'};
    check(replace_query({unicode_query,sizeof(unicode_query)})&&only_row(105),
        "friends search: UTF-16 target replacement performs Unicode case-insensitive name filtering");
    check(replace_query(utf8_literal("Ziga"))&&!scene()->source_count,
        "friends search: matching preserves accents");
    check(replace_query(utf8_literal(" needle"))&&only_row(102)&&replace_query(utf8_literal(" needle.user"))&&!scene()->source_count,
        "friends search: leading whitespace matches literal name substring and is never trimmed from username query");
    check(replace_query(utf8_literal("needle"))&&scene()->source_count==2,"friends search: broad query returns matching fields once per user");
    const auto key_owner=g_app.context_focus;
    check(synthetic_input(UiInputKind::key_down,key::space,false)&&
        synthetic_input(UiInputKind::character,' ',false)&&
        utf8_equal(message_composer_text(*all),utf8_literal("needle "))&&application_friends_search_focused(),
        "friends search: Space key preserves edit ownership and its character inserts literal whitespace");
    check(synthetic_input(UiInputKind::key_down,key::up,false)&&synthetic_input(UiInputKind::key_down,key::down,false)&&
        synthetic_input(UiInputKind::key_down,key::page_up,false)&&synthetic_input(UiInputKind::key_down,key::page_down,false)&&
        application_friends_search_focused()&&g_app.context_focus==key_owner&&replace_query(utf8_literal("needle")),
        "friends search: list navigation keys leave focused single-line edit and document unchanged");
    const auto target=application_friends_search_target();
    const auto source_revision=scene()->source->revision, count=scene()->source_count, text_revision=all->text_revision;
    const auto widgets_count=scene()->widgets.storage.count;const auto arena=scene()->storage.arena;
    const auto layout_revision=scene()->widgets.layout.revision;
    check(text_input_select(target,{0,2})==TextInputResult::changed&&application_friends_search_changed(false)&&
        scene()->source->revision==source_revision&&scene()->source_count==count&&scene()->content->total_count==3&&
        all->text_revision==text_revision&&scene()->widgets.storage.count==widgets_count&&
        scene()->widgets.layout.revision==layout_revision&&
        scene()->storage.arena.offset==arena.offset&&scene()->storage.arena.commit_count==arena.commit_count,
        "friends search: selection-only edits retain filtered source, layout and exact storage");
    auto stale=target;++stale.identity;u32 length=0;TextInputChange change{};
    check(!text_input_ready(stale)&&text_input_length(stale,&length)==TextInputResult::invalid_input,
        "friends search: stale document identity rejects readiness and delegated operations");
    u16 overflow[129]{};for(auto& unit:overflow)unit='x';
    const u16 newline[]={'x','\n'}, zero[]={'x',0}, surrogate[]={0xD800}, separator[]={0x2028}, paragraph[]={0x2029};
    check(text_input_replace(target,0,6,{overflow,129},&change)==TextInputResult::capacity&&
        text_input_replace(target,0,6,{newline,2},&change)==TextInputResult::invalid_input&&
        text_input_replace(target,0,6,{zero,2},&change)==TextInputResult::invalid_input&&
        text_input_replace(target,0,6,{surrogate,1},&change)==TextInputResult::invalid_utf16&&
        text_input_replace(target,0,6,{separator,1},&change)==TextInputResult::invalid_input&&
        text_input_replace(target,0,6,{paragraph,1},&change)==TextInputResult::invalid_input&&
        text_input_select(target,{999,999})==TextInputResult::invalid_utf16&&
        text_input_replace(target,999,999,{},&change)==TextInputResult::invalid_utf16&&
        utf8_equal(message_composer_text(*all),utf8_literal("needle"))&&all->text_revision==text_revision,
        "friends search: overflow, multiline, Unicode separators, NUL, malformed surrogate and stale ACP ranges fail atomically");
    const u16 supplementary[]={0xD83D,0xDE00,'x'};u16 copied[3]{};u32 copied_units=0,next_unit=0;
    check(text_input_replace(target,0,6,{supplementary,3},&change)==TextInputResult::changed&&application_friends_search_changed(true)&&
        text_input_length(target,&length)==TextInputResult::unchanged&&length==3&&all->bytes==5&&
        text_input_select(target,{2,3})==TextInputResult::changed&&all->anchor==4&&all->caret==5&&
        text_input_select(target,{1,1})==TextInputResult::invalid_utf16&&
        text_input_copy(target,0,3,{copied,3},&copied_units,&next_unit)==TextInputResult::unchanged&&copied_units==3&&next_unit==3&&
        copied[0]==supplementary[0]&&copied[1]==supplementary[1]&&copied[2]=='x'&&replace_query(utf8_literal("needle")),
        "friends search: ACP uses UTF-16 units while borrowed selection stores UTF-8 offsets and rejects split surrogates");
    const u16 x[]={'x'},y[]={'y'};
    check(text_input_begin_group(target)&&text_input_replace(target,6,6,{x,1},&change)==TextInputResult::changed&&
        text_input_replace(target,7,7,{y,1},&change)==TextInputResult::changed&&application_friends_search_changed(true),
        "friends search: native history transaction admits two bounded edits");
    seen.settings=true;application_friends_search_cancel_native_group(stale);seen.settings=false;
    check(!text_input_begin_group(target),"friends search: stale identity cannot close current native history transaction");
    seen.settings=true;application_friends_search_cancel_native_group(target);seen.settings=false;
    check(message_composer_undo(all)==MessageComposerResult::changed&&application_friends_search_changed(true)&&
        utf8_equal(message_composer_text(*all),utf8_literal("needle")),
        "friends search: identity-valid native cleanup closes history while settings blocks readiness and one Undo restores both edits");
    seen.settings=true;
    check(!text_input_ready(target)&&!synthetic_input(UiInputKind::character,'x',false),
        "friends search: opaque settings excludes the underlying edit target");seen.settings=false;
    g_app.text_form=reinterpret_cast<ApplicationTextForm*>(&modal_token);
    check(!text_input_ready(target)&&text_input_replace(target,0,0,{},&change)==TextInputResult::invalid_input,
        "friends search: native modal blocks old input target without modifying the query");cancel_form();
    check(tab(UiFriendsTab::online)&&!text_input_ready(target),"friends search: old All target is rejected after switching documents");
    auto* online=application_friends_search_model();
    check(online&&online!=all&&!online->history&&!online->owns_storage&&!online->owned_storage.base&&!online->bytes&&
        action(UiFriendsAction::search,0,UiFriendsTab::online)&&replace_query(utf8_literal("needle.user"))&&only_row(101),
        "friends search: Online has an independent lazy borrowed query document");
    check(tab(UiFriendsTab::pending)&&action(UiFriendsAction::search,0,UiFriendsTab::pending)&&
        replace_query(utf8_literal("pending"))&&only_row(103)&&
        tab(UiFriendsTab::all)&&utf8_equal(message_composer_text(*application_friends_search_model()),utf8_literal("needle"))&&
        scene()->source_count==2&&tab(UiFriendsTab::online)&&utf8_equal(message_composer_text(*application_friends_search_model()),utf8_literal("needle.user"))&&
        tab(UiFriendsTab::add_friend)&&application_friends_search_model()&&application_friends_search_model()!=all&&
        application_friends_search_focused()&&text_input_ready(application_friends_search_target())&&
        !application_friends_search_model()->bytes,
        "friends search: three list tabs retain separate queries and Add Friend focuses its own username document");
    check(tab(UiFriendsTab::all)&&action(UiFriendsAction::search),"friends search: returns to retained All query");
    const auto visible_target=application_friends_search_target();
    check(action(UiFriendsAction::more,101)&&g_app.popup_scene.active&&g_app.popup_owner_kind==PopupOwnerKind::friends&&
        !text_input_ready(visible_target),"friends search: real More popup owns routing and excludes search input");dismiss_popup();
    check(action(UiFriendsAction::search),"friends search: explicit focus restores target after popup dismissal");
    const Window window = window_test::open();
    check(window!=nullptr,"friends search: owns a headless mouse-capture fixture");
    const auto retained=application_friends_search_target();void* history=all->history;
    if (window) {
        g_app.window = window;
        const auto pointer_tab = [&](UiFriendsTab value, bool second_press) noexcept {
            const auto* current = scene();
            if (!current)
                return false;
            for (u32 slot = 0; slot < current->widgets.count; ++slot) {
                const auto* layout = ui_widget_manager_layout_node(
                    current->widgets, {static_cast<u16>(slot), current->widgets.generation});
                if (!layout)
                    continue;
                const float x = (layout->rectangle.left + layout->rectangle.right) * .5f;
                const float y = (layout->rectangle.top + layout->rectangle.bottom) * .5f;
                const auto hit = ui_friends_scene_hit_test(*current, x, y);
                if (hit.action != UiFriendsAction::select_tab || hit.tab != value)
                    continue;
                const float px = static_cast<float>(static_cast<i16>(x)), py = static_cast<float>(static_cast<i16>(y));
                return application_friends_window_event(second_press ? window_test::pointer_double(px, py) : window_test::pointer_down(px, py)) &&
                       window_pointer_captured(g_app.window) &&
                       application_friends_window_event(window_test::pointer_up(px, py, PointerButton::left)) && !window_pointer_captured(g_app.window);
            }
            return false;
        };
        check(
            pointer_tab(UiFriendsTab::online, false) && scene()->tab == UiFriendsTab::online &&
                pointer_tab(UiFriendsTab::all, true) && scene()->tab == UiFriendsTab::all,
            "friends host: native rapid tab presses activate both releases without swallowing second down");
        g_app.window = window;
        const auto field = ui_friends_scene_search_geometry(*scene()).field;
        const float location_x = static_cast<float>(static_cast<i16>((field.left + field.right) * .5f)),
                    location_y = static_cast<float>(static_cast<i16>((field.top + field.bottom) * .5f));
        check(application_friends_window_event(window_test::pointer_down(location_x, location_y))&&window_pointer_captured(g_app.window)&&
            application_friends_window_event(window_test::pointer_move(0, 0, window_test::button_bit(PointerButton::left))),
            "friends search: captured selection drag continues outside center viewport");
        application_friends_hide();
        check(!window_pointer_captured(g_app.window)&&!text_input_ready(retained)&&
            !application_friends_window_event(window_test::pointer_move(0, 0, window_test::button_bit(PointerButton::left))),
            "friends search: hide releases owned mouse capture and invalidates drag and input target");
        g_app.window=nullptr;window_test::close();
    } else
        application_friends_hide();
    check(application_friends_show()&&all==application_friends_search_model()&&all->history==history&&
        utf8_equal(message_composer_text(*all),utf8_literal("needle"))&&action(UiFriendsAction::search)&&
        action(UiFriendsAction::clear_search)&&!all->bytes&&scene()->source_count==3,
        "friends search: hide retains bounded document/history and semantic Clear restores all rows");
    const auto costs=message_composer_history_costs(*all);
    check(costs.payload_bytes==2048&&costs.used_bytes<=costs.payload_bytes&&costs.group_bytes<=kFriendsSearchBytes,
        "friends search: history payload stays bounded independently of borrowed text storage");
    constexpr UiFriendsTab query_tabs[]={UiFriendsTab::online,UiFriendsTab::pending};
    for(const auto value:query_tabs){
        check(tab(value)&&action(UiFriendsAction::search,0,value)&&replace_query({}),"friends search: fixture clears retained tab query");
    }
    check(tab(UiFriendsTab::all),"friends search: leaves All query clear for subsequent mutation fixture");
}
void confirmation_runtime_fixture() noexcept {
    constexpr auto strangers=utf8_literal(R"([{"id":"61","type":3,"is_stranger_request":true,"user":{"id":"61","username":"stranger"}},{"id":"80","type":3,"user":{"id":"80","username":"unknown"}},{"id":"70","type":1,"user":{"id":"70","username":"friend"}}])");
    check(snapshot(strangers,200)&&application_friends_synchronize()&&tab(UiFriendsTab::pending),"friends confirmation: publishes stranger and ordinary incoming requests");
    auto submitted=seen.submissions,opened=seen.form_opens;
    check(action(UiFriendsAction::accept,61,UiFriendsTab::pending)&&seen.form_opens==opened+1&&seen.form.confirmation_only&&
        seen.form.valid(seen.form.owner)&&seen.submissions==submitted&&!application_friends_pending(4000),
        "friends confirmation: known stranger requires native approval before any PUT admission");
    const auto approved=seen.form;
    check(save_form()&&approved.valid(approved.owner)&&!approved.save(approved.owner,{})&&application_friends_submit(201,4000)&&
        seen.operation==RelationshipOperation::accept_stranger&&seen.user_id==61&&
        utf8_equal({seen.body,seen.body_bytes},utf8_literal("{\"confirm_stranger_request\":true}"))&&response(201,204),
        "friends confirmation: one explicit approval queues exact stranger consent body and rejects duplicate Save while busy");
    submitted=seen.submissions;opened=seen.form_opens;
    check(action(UiFriendsAction::accept,80,UiFriendsTab::pending)&&application_friends_submit(202,4100)&&
        seen.operation==RelationshipOperation::accept&&utf8_equal({seen.body,seen.body_bytes},utf8_literal("{}"))&&
        response(202,400,{},0,{80013,false,false})&&seen.form_opens==opened+1&&seen.form.confirmation_only&&
        seen.submissions==submitted+1&&!application_friends_pending(100000)&&!application_friends_next_wake(),
        "friends confirmation: server 80013 opens approval without silent consent or automatic PUT retry");
    check(save_form()&&application_friends_submit(203,4200)&&seen.operation==RelationshipOperation::accept_stranger&&
        utf8_equal({seen.body,seen.body_bytes},utf8_literal("{\"confirm_stranger_request\":true}"))&&response(203,204)&&tab(UiFriendsTab::all),
        "friends confirmation: server-requested approval sends consent only after native Save");
    submitted=seen.submissions;opened=seen.form_opens;
    check(action(UiFriendsAction::more,70)&&g_app.popup_owner_kind==PopupOwnerKind::friends&&g_app.popup_scene.owner_id==70&&
        g_app.popup_scene.item_count==6&&g_app.popup_scene.items[1].action==0xF100u+static_cast<u32>(UiFriendsAction::voice_call)&&
        g_app.popup_scene.items[2].action==0xF100u+static_cast<u32>(UiFriendsAction::nickname)&&
        utf8_equal(g_app.popup_scene.items[2].label,utf8_literal("Add Friend Nickname"))&&
        g_app.popup_scene.items[3].action==0xF100u+static_cast<u32>(UiFriendsAction::remove)&&
        g_app.popup_scene.items[5].action==0xF100u+static_cast<u32>(UiFriendsAction::ignore)&&
        (g_app.popup_scene.items[3].flags&popup_item_danger)&&seen.submissions==submitted&&seen.form_opens==opened,
        "friends confirmation: production More popup owns identity and exposes call, nickname and destructive actions without mutating");
    check(remove_relationship(utf8_literal(R"({"id":"70"})"),201,false)&&
        application_friends_popup_action(0xF100u+static_cast<u32>(UiFriendsAction::remove))&&!g_app.popup_scene.active&&
        g_app.popup_owner_kind==PopupOwnerKind::none&&seen.form_opens==opened&&seen.submissions==submitted&&application_friends_synchronize(),
        "friends confirmation: stale popup row is dismissed before confirmation or mutation");
    check(snapshot(utf8_literal(R"([{"id":"70","type":1,"user":{"id":"70","username":"friend"}}])"),202)&&
        application_friends_synchronize()&&action(UiFriendsAction::remove,70),"friends confirmation: Remove opens first owner-validated modal");
    const auto old=seen.form;cancel_form();
    check(action(UiFriendsAction::block,70)&&!old.valid(old.owner)&&!old.save(old.owner,{}),
        "friends confirmation: new modal serial rejects an older approval for the same user");
    const auto block=seen.form;const auto profile=g_app.account.status.profile_id;++g_app.account.status.profile_id;
    check(!block.valid(block.owner)&&!block.save(block.owner,{}),"friends confirmation: account switch rejects retained approval key");
    g_app.account.status.profile_id=profile;
    check(snapshot(utf8_literal(R"([{"id":"70","type":2}])"),203)&&application_friends_synchronize()&&
        !block.valid(block.owner)&&!block.save(block.owner,{}),"friends confirmation: changed relationship type rejects retained approval");cancel_form();
    check(snapshot(utf8_literal(R"([{"id":"90","type":3,"is_stranger_request":true,"user":{"id":"90","username":"removed"}}])"),204)&&
        application_friends_synchronize()&&tab(UiFriendsTab::pending)&&action(UiFriendsAction::accept,90,UiFriendsTab::pending),
        "friends confirmation: stale-ID fixture opens stranger confirmation");
    const auto removed=seen.form;
    check(remove_relationship(utf8_literal(R"({"id":"90"})"),205)&&!removed.valid(removed.owner)&&!removed.save(removed.owner,{})&&
        !application_friends_pending(100000),"friends confirmation: live removal rejects approval before request queuing");cancel_form();
    check(snapshot(utf8_literal(R"([{"id":"91","type":3,"user":{"id":"91","username":"late"}}])"),206)&&
        application_friends_synchronize()&&action(UiFriendsAction::accept,91,UiFriendsTab::pending)&&application_friends_submit(204,4300)&&
        remove_relationship(utf8_literal(R"({"id":"91"})"),207),"friends confirmation: ordinary PUT can outlive authoritative removal");
    opened=seen.form_opens;submitted=seen.submissions;
    check(response(204,400,{},0,{80013,false,false})&&seen.form_opens==opened&&seen.submissions==submitted&&
        !application_friends_pending(100000)&&scene()->phase==UiFriendsPhase::error&&action(UiFriendsAction::retry,0,UiFriendsTab::pending)&&
        application_friends_submit(205,4400)&&seen.operation==RelationshipOperation::list&&!seen.user_id&&!seen.body_bytes&&
        response(205,200,utf8_literal("[]"))&&tab(UiFriendsTab::all),
        "friends confirmation: late 80013 for removed request offers GET recovery and never confirms or resubmits stale PUT");
    check(snapshot(utf8_literal(R"([{"id":"92","type":3,"user":{"id":"92","username":"withdrawn"}}])"),208)&&
        application_friends_synchronize()&&tab(UiFriendsTab::pending)&&action(UiFriendsAction::accept,92,UiFriendsTab::pending)&&
        application_friends_submit(206,4500)&&response(206,400,{},0,{80013,false,false})&&seen.form.confirmation_only,
        "friends confirmation: server consent prompt may precede a later live withdrawal");
    const auto withdrawn=seen.form;
    check(remove_relationship(utf8_literal(R"({"id":"92"})"),209)&&!withdrawn.valid(withdrawn.owner)&&!withdrawn.save(withdrawn.owner,{}),
        "friends confirmation: withdrawal invalidates an already-open server consent prompt");cancel_form();
    opened=seen.form_opens;submitted=seen.submissions;
    check(action(UiFriendsAction::retry,0,UiFriendsTab::pending)&&seen.form_opens==opened&&
        application_friends_submit(207,4600)&&seen.operation==RelationshipOperation::list&&!seen.user_id&&!seen.body_bytes&&
        seen.submissions==submitted+1&&response(207,200,utf8_literal("[]"))&&tab(UiFriendsTab::all),
        "friends confirmation: Retry after withdrawal refreshes the list instead of reopening a stale prompt");
}
void pending_sections_runtime_fixture() noexcept {
    constexpr auto requests=utf8_literal(R"([{"id":"501","type":3,"is_stranger_request":false,"user":{"id":"501","username":"needle.one"}},{"id":"502","type":3,"is_stranger_request":false,"user":{"id":"502","username":"needle.two"}},{"id":"503","type":3,"is_stranger_request":false,"user":{"id":"503","username":"needle.three"}},{"id":"504","type":3,"is_stranger_request":false,"user":{"id":"504","username":"needle.four"}},{"id":"505","type":3,"is_stranger_request":false,"user":{"id":"505","username":"needle.five"}},{"id":"506","type":3,"is_stranger_request":false,"user":{"id":"506","username":"different"}},{"id":"601","type":4,"user":{"id":"601","username":"sent"}},{"id":"701","type":3,"user_ignored":true,"is_stranger_request":false,"user":{"id":"701","username":"ignored"}},{"id":"702","type":3,"is_spam_request":true,"is_stranger_request":false,"user":{"id":"702","username":"spam"}},{"id":"703","type":3,"user_ignored":true,"is_spam_request":true,"is_stranger_request":false,"user":{"id":"703","username":"overlap"}}])");
    check(snapshot(requests,300)&&application_friends_synchronize()&&tab(UiFriendsTab::pending)&&scene()->source_count==7&&
        scene()->sections&&scene()->sections->count==2&&scene()->sections->sections[0].kind==UiFriendsSectionKind::incoming&&
        scene()->sections->sections[0].count==6&&scene()->sections->sections[1].kind==UiFriendsSectionKind::outgoing&&
        scene()->sections->sections[1].first==6&&scene()->sections->sections[1].count==1&&scene()->sections->hidden_count==3,
        "friends Pending: native Received and Sent ranges exclude ignored/spam and deduplicate hidden overlapping flags");
    if(!scene()||!scene()->sections)return;
    check(action(UiFriendsAction::toggle_section,0,UiFriendsTab::pending)&&scene()->source_count==7&&
        scene()->sections->sections[0].collapsed&&ui_friends_scene_visible_count(*scene())==1,
        "friends Pending: collapsing Received retains host source and displays only expanded Sent row");
    UiFriendsVisibleRow visible{};const auto last=ui_friends_scene_visible_count(*scene());
    check(last&&ui_friends_scene_visible_row(*scene(),last-1,&visible)&&visible.row.user_id==601&&ui_rect_width(visible.avatar)==32,
        "friends Pending: retained last media row remains outgoing after incoming section collapse");
    check(action(UiFriendsAction::toggle_section,0,UiFriendsTab::pending)&&!scene()->sections->sections[0].collapsed&&
        action(UiFriendsAction::search,0,UiFriendsTab::pending)&&replace_query(utf8_literal("needle"))&&scene()->source_count==5&&
        scene()->sections->sections[0].count==5&&scene()->sections->sections[1].count==0&&scene()->content->total_count==7,
        "friends Pending: search publishes filtered section counts while retaining unfiltered ordinary total");
    const auto retained_search=application_friends_search_target();auto* background=main_scene();
    const auto background_count=background?background->source_count:0;const auto submissions=seen.submissions;
    const auto before_modal_bank=application_friends_storage_metrics().requests_indices;
    check(!before_modal_bank.address&&!before_modal_bank.reserved_bytes&&!before_modal_bank.committed_bytes&&!before_modal_bank.admitted_count&&!before_modal_bank.commit_count,
        "friends storage: optional hidden index bank has no reservation or committed pages before first modal");
    check(action(UiFriendsAction::toggle_hidden,0,UiFriendsTab::pending)&&application_friends_requests_active()&&scene()!=background&&
        main_scene()==background&&scene()->sections&&scene()->sections->hidden_view&&scene()->source_count==3&&
        !scene()->content&&!text_input_ready(retained_search)&&seen.submissions==submissions,
        "friends Pending: Other requests opens a separate unfiltered modal and blocks retained search without REST submission");
    if(application_friends_requests_active()&&scene()&&scene()->source){
        const auto modal_bank=application_friends_storage_metrics().requests_indices;SYSTEM_INFO modal_system{};GetSystemInfo(&modal_system);
        check(modal_bank.address&&modal_bank.reserved_bytes&&modal_bank.committed_bytes==modal_system.dwPageSize&&modal_bank.admitted_count==3&&modal_bank.commit_count==1,
            "friends storage: first three-row Other requests modal commits exactly one optional index page");
        const auto* modal_source=scene()->source;const auto modal_revision=scene()->revision,modal_builds=seen.builds;
        bool idle=true;for(u32 i=0;i<8;++i)idle=application_friends_synchronize()&&idle;
        const auto modal_retained=application_friends_storage_metrics().requests_indices;
        check(idle&&scene()->source==modal_source&&scene()->revision==modal_revision&&seen.builds==modal_builds&&
            modal_bank.reserved_bytes==modal_retained.reserved_bytes&&modal_bank.committed_bytes==modal_retained.committed_bytes&&
            modal_bank.commit_count==modal_retained.commit_count,
            "friends storage: 8 idle modal synchronizations retain optional bank commitment and admitted high-water prefix");
        UiFriendsRow first{},second{},third{};
        check(scene()->source->resolve(scene()->source->context,0,&first)&&scene()->source->resolve(scene()->source->context,1,&second)&&
            scene()->source->resolve(scene()->source->context,2,&third)&&first.ignored&&second.ignored&&third.spam&&!third.ignored&&
            first.user_id!=second.user_id&&first.user_id!=third.user_id&&second.user_id!=third.user_id,
            "friends Pending: Other requests source orders ignored first and represents spam/ignored overlap once");
        check(application_friends_window_event(window_test::pointer_down(100, 300))&&
            application_friends_window_event(window_test::pointer_up(100, 300, PointerButton::left))&&application_friends_requests_active()&&
            main_scene()==background&&background->source_count==background_count,
            "friends Pending: modal consumes private-sidebar pointer input and preserves background projection");
        check(action(UiFriendsAction::toggle_hidden,0,UiFriendsTab::pending)&&!application_friends_requests_active()&&
            scene()==background&&scene()->source_count==5&&utf8_equal(message_composer_text(*application_friends_search_model()),utf8_literal("needle")),
            "friends Pending: Close restores the same filtered background document and retained main scene");
    }
    const auto before_clear_forms=seen.form_opens,before_clear_submissions=seen.submissions;
    check(action(UiFriendsAction::clear_incoming,0,UiFriendsTab::pending)&&seen.form_opens==before_clear_forms+1&&
        seen.form.confirmation_only&&seen.form.valid(seen.form.owner)&&!application_friends_pending(5000)&&seen.submissions==before_clear_submissions,
        "friends Pending: filtered five-row incoming Clear opens native confirmation before bulk DELETE admission");
    check(utf8_equal(seen.form.context,utf8_literal("Clear all 9 friend requests?")),
        "friends Pending: bulk consent names all nine incoming requests including hidden and unmatched rows despite five visible matches");
    const auto obsolete_clear=seen.form;
    check(remove_relationship(utf8_literal(R"({"id":"601"})"),301)&&!obsolete_clear.valid(obsolete_clear.owner)&&
        !obsolete_clear.save(obsolete_clear.owner,{})&&!application_friends_pending(5000),
        "friends Pending: membership epoch change invalidates a retained bulk approval even when incoming matches remain");cancel_form();
    check(snapshot(requests,302)&&application_friends_synchronize()&&scene()->source_count==5&&
        action(UiFriendsAction::clear_incoming,0,UiFriendsTab::pending)&&save_form()&&application_friends_submit(320,5000)&&
        seen.operation==RelationshipOperation::clear_incoming&&seen.method==RestMethod::delete_method&&!seen.user_id&&!seen.body_bytes&&
        utf8_equal({seen.target,seen.target_bytes},utf8_literal("/api/v9/users/@me/relationships?relationship_type=3")),
        "friends Pending: approved Clear submits the unfiltered incoming route rather than five filtered user requests");
    check(response(320,204)&&application_friends_pending(5001)&&main_scene()->source_count==5&&application_friends_submit(321,5001)&&
        seen.operation==RelationshipOperation::list&&response(321,200,requests)&&main_scene()->source_count==5,
        "friends Pending: bulk acknowledgment preserves current rows until authoritative GET and obeys the returned membership");
    const auto hidden_forms=seen.form_opens;
    check(action(UiFriendsAction::toggle_hidden,0,UiFriendsTab::pending)&&application_friends_requests_active()&&
        action(UiFriendsAction::clear_hidden,0,UiFriendsTab::pending)&&!application_friends_requests_active()&&
        seen.form_opens==hidden_forms&&main_scene()->source_count==5&&application_friends_submit(322,5100)&&
        seen.operation==RelationshipOperation::clear_hidden&&seen.method==RestMethod::delete_method&&!seen.user_id&&
        utf8_equal({seen.body,seen.body_bytes},utf8_literal("{\"filters\":[1,2]}")),
        "friends Pending: Other requests Clear all closes immediately and submits filters without a confirmation or speculative deletion");
    check(response(322,204)&&application_friends_pending(5101)&&main_scene()->source_count==5&&application_friends_submit(323,5101)&&
        seen.operation==RelationshipOperation::list&&response(323,200,utf8_literal(R"([{"id":"601","type":4}])"))&&
        main_scene()->source_count==0&&main_scene()->sections->hidden_count==0,
        "friends Pending: hidden bulk completion reconciles authoritative GET without assuming filter OR semantics");
    check(action(UiFriendsAction::search,0,UiFriendsTab::pending)&&replace_query({})&&scene()->source_count==1&&tab(UiFriendsTab::all),
        "friends Pending: fixture clears retained query after authoritative bulk reconciliation");
}
void ignore_and_decline_runtime_fixture() noexcept {
    constexpr auto users=utf8_literal(R"([{"id":"801","type":1,"user":{"id":"801","username":"kept.friend"}},{"id":"811","type":3,"is_stranger_request":false,"user":{"id":"811","username":"known.safe"}},{"id":"815","type":3,"is_stranger_request":false,"user":{"id":"815","username":"declined"}},{"id":"814","type":4,"user":{"id":"814","username":"cancelled"}}])");
    check(snapshot(users,310)&&application_friends_synchronize()&&tab(UiFriendsTab::all),"friends Ignore: publishes friend and explicitly safe pending rows");
    auto submissions=seen.submissions,forms=seen.form_opens;
    check(action(UiFriendsAction::ignore,801)&&seen.form_opens==forms+1&&seen.form.confirmation_only&&
        seen.form.valid(seen.form.owner)&&seen.submissions==submissions&&!application_friends_pending(5200)&&
        save_form()&&application_friends_submit(330,5200)&&seen.operation==RelationshipOperation::ignore&&
        seen.method==RestMethod::put&&!seen.body_bytes&&utf8_equal({seen.target,seen.target_bytes},utf8_literal("/api/v9/users/@me/relationships/801/ignore"))&&response(330,204),
        "friends Ignore: More Ignore requires native consent and submits bodyless PUT ignore rather than relationship DELETE");
    UiFriendsRow value{};
    check(row(801,&value)&&value.kind==UiFriendsKind::friend_user&&value.ignored&&scene()->source_count==1,
        "friends Ignore: successful Ignore preserves friendship and updates its authoritative ignored flag");
    forms=seen.form_opens;
    check(action(UiFriendsAction::unignore,801)&&seen.form_opens==forms&&application_friends_submit(331,5300)&&
        seen.operation==RelationshipOperation::unignore&&seen.method==RestMethod::delete_method&&!seen.body_bytes&&
        utf8_equal({seen.target,seen.target_bytes},utf8_literal("/api/v9/users/@me/relationships/801/ignore"))&&response(331,204)&&
        row(801,&value)&&value.kind==UiFriendsKind::friend_user&&!value.ignored,
        "friends Ignore: Unignore submits DELETE ignore and preserves the friend without redundant confirmation");
    forms=seen.form_opens;
    check(tab(UiFriendsTab::pending)&&scene()->source_count==3&&!scene()->sections->hidden_count&&
        action(UiFriendsAction::decline,815,UiFriendsTab::pending)&&seen.form_opens==forms&&application_friends_submit(332,5400)&&
        seen.operation==RelationshipOperation::remove&&seen.method==RestMethod::delete_method&&!seen.body_bytes&&
        utf8_equal({seen.target,seen.target_bytes},utf8_literal("/api/v9/users/@me/relationships/815"))&&response(332,204)&&!row(815,&value),
        "friends Pending: incoming X declines through bodyless relationship DELETE and does not set ignored");
    check(action(UiFriendsAction::cancel,814,UiFriendsTab::pending)&&application_friends_submit(333,5500)&&
        seen.operation==RelationshipOperation::remove&&seen.method==RestMethod::delete_method&&!seen.body_bytes&&response(333,204)&&!row(814,&value),
        "friends Pending: outgoing Cancel remains a separate semantic action on relationship DELETE");
    forms=seen.form_opens;
    check(action(UiFriendsAction::accept,811,UiFriendsTab::pending)&&seen.form_opens==forms&&application_friends_submit(334,5600)&&
        seen.operation==RelationshipOperation::accept_stranger&&utf8_equal({seen.body,seen.body_bytes},utf8_literal("{\"confirm_stranger_request\":true}"))&&response(334,204)&&
        tab(UiFriendsTab::all)&&row(811,&value)&&value.kind==UiFriendsKind::friend_user,
        "friends confirmation: explicit false stranger classification submits confirmed-safe body directly while unknown requests retain plain first PUT");
}
void stale_classification_runtime_fixture() noexcept {
    constexpr auto classified=utf8_literal(R"([{"id":"901","type":3,"is_stranger_request":true,"user":{"id":"901","username":"same.request"}}])");
    constexpr auto safe=utf8_literal(R"([{"id":"901","type":3,"is_stranger_request":false,"user":{"id":"901","username":"same.request"}}])");
    const auto submissions=seen.submissions;
    check(snapshot(classified,400)&&application_friends_synchronize()&&tab(UiFriendsTab::pending)&&
        action(UiFriendsAction::accept,901,UiFriendsTab::pending)&&seen.form.confirmation_only&&seen.form.valid(seen.form.owner),
        "friends classification: known unfamiliar incoming request opens current classification-bound native consent");
    const auto classified_form=seen.form;
    check(snapshot(safe,401)&&application_friends_synchronize()&&!classified_form.valid(classified_form.owner)&&
        !classified_form.save(classified_form.owner,{})&&!application_friends_pending(5700)&&seen.submissions==submissions,
        "friends classification: stranger classification change alone invalidates consent for the same type ID and relationship flags");cancel_form();
    check(action(UiFriendsAction::accept,901,UiFriendsTab::pending)&&application_friends_pending(5700)&&
        snapshot(classified,402)&&application_friends_synchronize()&&!application_friends_submit(340,5700)&&
        seen.submissions==submissions&&!application_friends_pending(100000)&&scene()->phase==UiFriendsPhase::error,
        "friends classification: queued known-safe acceptance cannot submit consent after same request becomes unfamiliar");
    check(action(UiFriendsAction::retry,0,UiFriendsTab::pending)&&application_friends_submit(341,5701)&&
        seen.operation==RelationshipOperation::list&&!seen.user_id&&!seen.body_bytes&&response(341,200,utf8_literal("[]"))&&tab(UiFriendsTab::all),
        "friends classification: stale queued consent recovers through authoritative GET instead of repeating a PUT");
}
void stale_bulk_retry_runtime_fixture() noexcept {
    constexpr auto incoming=utf8_literal(R"([{"id":"951","type":3},{"id":"952","type":3},{"id":"953","type":3},{"id":"954","type":3},{"id":"955","type":3},{"id":"956","type":3}])");
    check(snapshot(incoming,410)&&application_friends_synchronize()&&tab(UiFriendsTab::pending)&&
        action(UiFriendsAction::clear_incoming,0,UiFriendsTab::pending)&&save_form()&&application_friends_submit(350,5800)&&
        seen.operation==RelationshipOperation::clear_incoming&&response(350,500),
        "friends bulk retry: admitted incoming clear exposes explicit retry after server failure");
    check(remove_relationship(utf8_literal(R"({"id":"956"})"),411)&&action(UiFriendsAction::retry,0,UiFriendsTab::pending)&&
        application_friends_submit(351,5801)&&seen.operation==RelationshipOperation::list&&!seen.user_id&&!seen.body_bytes&&
        response(351,200,incoming),
        "friends bulk retry: explicit retry after membership change fetches GET rather than clearing a newly changed incoming scope");
    check(action(UiFriendsAction::clear_incoming,0,UiFriendsTab::pending)&&save_form()&&application_friends_submit(352,5900)&&
        response(352,429,{},6000)&&!application_friends_pending(5999)&&remove_relationship(utf8_literal(R"({"id":"956"})"),412),
        "friends bulk retry: genuine 429 preserves original bulk intent before a later live membership change");
    const auto submissions=seen.submissions;
    check(application_friends_pending(6000)&&!application_friends_submit(353,6000)&&seen.submissions==submissions&&
        scene()->phase==UiFriendsPhase::error&&action(UiFriendsAction::retry,0,UiFriendsTab::pending)&&
        application_friends_submit(354,6001)&&seen.operation==RelationshipOperation::list&&!seen.user_id&&!seen.body_bytes&&
        response(354,200,utf8_literal("[]"))&&tab(UiFriendsTab::all),
        "friends bulk retry: stale rate-limited bulk intent submits no repeated DELETE and explicit recovery uses GET");
}
void nickname_and_call_runtime_fixture() noexcept {
    check(snapshot(utf8_literal(R"([{"id":"120","type":1,"user":{"id":"120","username":"nick.friend","global_name":"Nick Friend"}}])"),900)&&
        application_friends_synchronize()&&tab(UiFriendsTab::all),"friends nickname: fixture publishes one friend");
    const auto forms=seen.form_opens, submissions=seen.submissions;
    check(action(UiFriendsAction::nickname,120)&&seen.form_opens==forms+1&&!seen.form.confirmation_only&&
        utf8_equal(seen.form.title,utf8_literal("Add Friend Nickname"))&&!seen.form.initial.bytes&&
        seen.form.maximum_codepoints==32&&seen.form.valid(seen.form.owner)&&seen.submissions==submissions,
        "friends nickname: More opens the nickname editor without a request");
    check(save_form(utf8_literal("  Bestie  "))&&application_friends_submit(900,9000)&&
        seen.operation==RelationshipOperation::nickname&&seen.method==RestMethod::patch&&
        utf8_equal({seen.target,seen.target_bytes},utf8_literal("/api/v9/users/@me/relationships/120"))&&
        utf8_equal({seen.body,seen.body_bytes},utf8_literal("{\"nickname\":\"Bestie\"}"))&&response(900,204),
        "friends nickname: Save PATCHes the trimmed nickname");
    UiFriendsRow updated{};
    check(row(120,&updated)&&utf8_equal(updated.display_name,utf8_literal("Bestie")),
        "friends nickname: the friend row shows the saved nickname at once");
    check(action(UiFriendsAction::nickname,120)&&utf8_equal(seen.form.title,utf8_literal("Change Friend Nickname"))&&
        utf8_equal(seen.form.initial,utf8_literal("Bestie"))&&save_form({})&&application_friends_submit(901,9100)&&
        utf8_equal({seen.body,seen.body_bytes},utf8_literal("{\"nickname\":null}"))&&response(901,204)&&
        row(120,&updated)&&utf8_equal(updated.display_name,utf8_literal("Nick Friend")),
        "friends nickname: an empty nickname clears it back to the friend's name");
    const auto stale=seen.form;
    check(action(UiFriendsAction::nickname,120)&&remove_relationship(utf8_literal(R"({"id":"120"})"),901,false)&&
        !seen.form.valid(seen.form.owner)&&!stale.valid(stale.owner),
        "friends nickname: removing the friend invalidates an open nickname editor");
    cancel_form();
    check(snapshot(utf8_literal(R"([{"id":"121","type":1,"user":{"id":"121","username":"caller"}}])"),902)&&
        application_friends_synchronize()&&tab(UiFriendsTab::all),"friends call: fixture publishes one friend");
    seen.call_channel=0;
    check(action(UiFriendsAction::voice_call,121)&&seen.dm_user==121&&!application_friends_active()&&
        application_friends_synchronize()&&!seen.call_channel&&application_friends_show(),
        "friends call: Start Voice Call opens the DM and never rings a conversation that is not theirs");
}
void bounded_performance_fixture() noexcept {
    constexpr u32 records = 4096;
    constexpr u64 first_id = 10000, sequence = 1000;
    check(tab(UiFriendsTab::all), "friends benchmark: selects All before bounded snapshot fixture");
    const u32 user_revision = g_app.account_ui->user_store.revision;
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;event.sequence = sequence;event.has_sequence = true;
    event.dispatch_kind = GatewayClientDispatchKind::relationship_snapshot_begin;
    application_friends_gateway(event, {});
    event.dispatch_kind = GatewayClientDispatchKind::relationship_snapshot_item;
    event.state.record_count = 1;event.dispatch_payload_bytes = sizeof(RelationshipMutation);
    RelationshipMutation item{};
    item.record.type = RelationshipType::friend_;item.record.presence = RelationshipPresence::unknown;
    for (u32 i = records; i; --i) {
        item.record.user_id = first_id + i;
        application_friends_gateway(event, {reinterpret_cast<const u8*>(&item), sizeof(item)});
    }
    event.dispatch_kind = GatewayClientDispatchKind::relationship_snapshot_complete;
    event.state.record_count = records;event.dispatch_payload_bytes = 0;
    application_friends_gateway(event, {});
    const bool projected = application_friends_synchronize();
    check(projected && scene() && scene()->phase == UiFriendsPhase::ready && scene()->tab == UiFriendsTab::all &&
        scene()->source_count == records && scene()->source && scene()->source->count == records &&
        g_app.account_ui->user_store.revision == user_revision,
        "friends benchmark: 4096 descending pointer-free items commit complete All projection without user-bank hydration");
    if (!scene() || !scene()->source || scene()->source->count != records) return;
    bool sorted = true;
    for (u32 i = 0; i < records; ++i) {
        UiFriendsRow projected_row{};
        sorted = sorted && scene()->source->resolve(scene()->source->context, i, &projected_row) &&
            projected_row.user_id == first_id + i + 1 && projected_row.kind == UiFriendsKind::friend_user;
    }
    check(sorted, "friends benchmark: equal unnamed rows retain deterministic ascending identity order across full source");
    const auto arena = scene()->storage.arena;
    const auto* source = scene()->source;
    const u32 revision = scene()->revision, source_revision = source->revision, builds = seen.builds;
    const u32 widget_count = scene()->widgets.storage.count;
    const auto storage=application_friends_storage_metrics();
    check(storage.controller_bytes<128*1024&&storage.active.admitted_count>=records&&!storage.candidate.admitted_count&&
        storage.indices.admitted_count>=records&&storage.active.committed_bytes<=storage.active.reserved_bytes&&
        !storage.candidate.committed_bytes&&storage.candidate.reserved_bytes&&storage.indices.committed_bytes<=storage.indices.reserved_bytes,
        "friends benchmark: the store keeps the snapshot and the candidate bank returns its pages after the snapshot");
    bool retained = true;
    for (u32 i = 0; i < 8; ++i) retained = application_friends_synchronize() && retained;
    const auto retained_storage=application_friends_storage_metrics();
    const auto same_bank=[](const FriendsStorageBankMetrics& a,const FriendsStorageBankMetrics& b) noexcept {
        return a.reserved_bytes==b.reserved_bytes&&a.committed_bytes==b.committed_bytes&&a.commit_count==b.commit_count;};
    check(same_bank(storage.active,retained_storage.active)&&
        same_bank(storage.candidate,retained_storage.candidate)&&same_bank(storage.indices,retained_storage.indices),
        "friends benchmark: 8 unchanged synchronizations retain all three bank admission and commit high-water metrics");
    check(retained && scene()->revision == revision && seen.builds == builds &&
        scene()->source == source && source->revision == source_revision && scene()->source_count == records &&
        scene()->widgets.storage.count == widget_count &&
        scene()->storage.arena.committed == arena.committed && scene()->storage.arena.offset == arena.offset &&
        scene()->storage.arena.commit_count == arena.commit_count,
        "friends benchmark: 8 unchanged synchronizations retain revisions, text publication and exact committed storage");
}
// A relationships reply as GET /users/@me/relationships sends it: `friends` friends with decorated users (decoration,
// server tag, nameplate), then `blocked` blocked users with plain ones. IDs count up from `first`.
struct ListWriter {
    char* data;
    u32 used;
    void put(const char* text) noexcept { while (*text) data[used++] = *text++; }
    void id(u64 value) noexcept {
        char digits[20];u32 count = 0;
        do { digits[count++] = static_cast<char>('0' + value % 10);value /= 10; } while (value);
        while (count) data[used++] = digits[--count];
    }
};
u32 relationships_reply(char* output, u32 friends, u32 blocked, u64 first) noexcept {
    ListWriter w{output, 0};
    w.put("[");
    for (u32 i = 0; i < friends + blocked; ++i) {
        const u64 user = first + i;
        const bool decorated = i < friends;
        if (i) w.put(",");
        w.put("{\"id\":\"");w.id(user);w.put(decorated ? "\",\"type\":1" : "\",\"type\":2");
        w.put(",\"nickname\":null,\"user_ignored\":false,\"user\":{\"id\":\"");w.id(user);
        w.put("\",\"username\":\"user");w.id(user);w.put("\",\"global_name\":\"Person ");w.id(user);
        w.put("\",\"avatar\":\"14733482e560d9267c0a414b21b2fb8d\",\"discriminator\":\"0\",\"public_flags\":64,");
        if (decorated)
            w.put("\"avatar_decoration_data\":{\"asset\":\"a_d3f2a1c4b5e6f7a8b9c0d1e2f3a4b5c6\",\"sku_id\":\"1144058522808614923\","
                  "\"expires_at\":null},\"primary_guild\":{\"identity_guild_id\":\"1234567890123456789\","
                  "\"identity_enabled\":true,\"tag\":\"TAG\",\"badge\":\"0123456789abcdef0123456789abcdef\"},"
                  "\"collectibles\":{\"nameplate\":{\"sku_id\":\"1349849614353829990\",\"asset\":"
                  "\"nameplates/nameplates/twilight/\",\"label\":\"COLLECTIBLES_NAMEPLATES_TWILIGHT_A11Y\","
                  "\"palette\":\"cobalt\",\"expires_at\":null}},");
        else
            w.put("\"avatar_decoration_data\":null,\"primary_guild\":null,\"collectibles\":null,");
        w.put("\"display_name_styles\":null,\"clan\":null},\"is_spam_request\":false,"
              "\"since\":\"2023-02-10T01:58:05.348000+00:00\",\"stranger_request\":false}");
    }
    w.put("]");
    return w.used;
}
// The centre of the control that the scene's hit test names `target`, in whole pixels as a pointer reports it.
bool control_point(const UiFriendsTarget& target, float* x, float* y) noexcept {
    const auto* current = scene();
    if (!current) return false;
    for (u32 slot = 0; slot < current->widgets.count; ++slot) {
        const auto* layout = ui_widget_manager_layout_node(current->widgets, {static_cast<u16>(slot), current->widgets.generation});
        if (!layout) continue;
        const float cx = static_cast<float>(static_cast<i16>((layout->rectangle.left + layout->rectangle.right) * .5f));
        const float cy = static_cast<float>(static_cast<i16>((layout->rectangle.top + layout->rectangle.bottom) * .5f));
        if (ui_friends_target_equal(ui_friends_scene_hit_test(*current, cx, cy), target)) { *x = cx; *y = cy; return true; }
    }
    return false;
}
// Issue #18: a mouse click on a Pending row's Accept did nothing when Gateway traffic rebuilt the list between the press
// and the release. The click sends the PUT, and the RELATIONSHIP_ADD that follows makes the row a friend.
void pointer_accept_runtime_fixture() noexcept {
    const Window window = window_test::open();
    check(window != nullptr, "friends pointer Accept: owns a headless pointer-capture fixture");
    if (!window) return;
    g_app.window = window;
    constexpr auto list = utf8_literal(R"([{"id":"1001","type":3,"user":{"id":"1001","username":"pointer.request"}},{"id":"1002","type":1,"user":{"id":"1002","username":"pointer.friend"}},{"id":"1003","type":3,"user":{"id":"1003","username":"pointer.next"}},{"id":"1004","type":3,"user":{"id":"1004","username":"pointer.other"}}])");
    float x = 0, y = 0;
    check(snapshot(list, 600, utf8_literal(R"({"friends":[{"user_id":"1002","status":"online"}]})")) &&
              application_friends_synchronize() && tab(UiFriendsTab::pending) &&
              control_point({UiFriendsAction::accept, 1001, UiFriendsTab::pending}, &x, &y),
          "friends pointer Accept: the Pending row shows its Accept button");
    const u32 submissions = seen.submissions, rows = scene()->source_revision;
    check(application_friends_window_event(window_test::pointer_down(x, y)) && window_pointer_captured(g_app.window),
          "friends pointer Accept: the press captures the pointer");
    // A friend's presence and a message author's name arrive while the button is held, and the next frame rebuilds the list.
    GatewayClientEvent presence{};
    presence.kind = GatewayClientEventKind::dispatch;presence.dispatch_kind = GatewayClientDispatchKind::presence_update;
    presence.presence.user_id = 1002;presence.presence.status = GatewayClientMemberListStatus::idle;
    application_friends_gateway(presence, {});
    check(user_store_upsert(&g_app.account_ui->user_store, 7777, utf8_literal("Message Author")).ok() &&
              application_friends_synchronize() && scene()->source_revision != rows,
          "friends pointer Accept: Gateway traffic rebuilds the list between the press and the release");
    check(application_friends_window_event(window_test::pointer_up(x, y, PointerButton::left)) &&
              !window_pointer_captured(g_app.window) && application_friends_pending(7000),
          "friends pointer Accept: the release over the same Accept button queues the request");
    check(application_friends_submit(710, 7000) && seen.submissions == submissions + 1 &&
              seen.operation == RelationshipOperation::accept && seen.user_id == 1001 && seen.method == RestMethod::put &&
              utf8_equal({seen.target, seen.target_bytes}, utf8_literal("/api/v9/users/@me/relationships/1001")) &&
              utf8_equal({seen.body, seen.body_bytes}, utf8_literal("{}")) && context_is(kFriendsContext),
          "friends pointer Accept: the request is PUT /users/@me/relationships/1001 with the body {} from Friends");
    // Discord sends RELATIONSHIP_ADD before the 204 reply.
    UiFriendsRow value{};
    check(add_relationship(utf8_literal(R"({"id":"1001","type":1,"nickname":null,"user":{"id":"1001","username":"pointer.request"},"should_notify":false})"), 602) &&
              response(710, 204) && !application_friends_pending(100000) && scene()->phase == UiFriendsPhase::ready &&
              !scene()->status.bytes && !row(1001, &value) && scene()->source_count == 2,
          "friends pointer Accept: RELATIONSHIP_ADD and the 204 reply take the request out of Pending without an error");
    check(tab(UiFriendsTab::all) && row(1001, &value) && value.kind == UiFriendsKind::friend_user && !value.busy &&
              tab(UiFriendsTab::pending),
          "friends pointer Accept: the accepted user is a friend in All");
    // The guard that stays: a release over another user's button does not act.
    check(control_point({UiFriendsAction::accept, 1003, UiFriendsTab::pending}, &x, &y) &&
              application_friends_window_event(window_test::pointer_down(x, y)) &&
              remove_relationship(utf8_literal(R"({"id":"1003"})"), 603) &&
              ui_friends_target_equal(ui_friends_scene_hit_test(*scene(), x, y), {UiFriendsAction::accept, 1004, UiFriendsTab::pending}) &&
              application_friends_window_event(window_test::pointer_up(x, y, PointerButton::left)) &&
              !application_friends_pending(100000) && seen.submissions == submissions + 1,
          "friends pointer Accept: a release over the next user's Accept, moved under the pointer, sends nothing");
    check(tab(UiFriendsTab::all), "friends pointer Accept: the fixture returns to All");
    g_app.window = nullptr;window_test::close();
}
// The first row button that the list's bottom edge cuts or hides (its rectangle and the list's clip).
bool button_past_edge(UiRect* button, UiRect* clip) noexcept {
    const auto* current = scene();
    for (u32 slot = 0; current && slot < current->widgets.count; ++slot) {
        const auto* layout = ui_widget_manager_layout_node(current->widgets, {static_cast<u16>(slot), current->widgets.generation});
        if (!layout || layout->rectangle.bottom <= layout->clip_rectangle.bottom ||
            layout->rectangle.right - layout->rectangle.left > 64) continue;
        *button = layout->rectangle;*clip = layout->clip_rectangle;
        return true;
    }
    return false;
}
// An Accept button that the list's bottom edge cuts: its user and a point on its visible part.
bool cut_accept(u64* user, float* x, float* y) noexcept {
    const auto* current = scene();
    for (u32 slot = 0; current && slot < current->widgets.count; ++slot) {
        const auto* layout = ui_widget_manager_layout_node(current->widgets, {static_cast<u16>(slot), current->widgets.generation});
        if (!layout || layout->rectangle.top >= layout->clip_rectangle.bottom ||
            layout->rectangle.bottom <= layout->clip_rectangle.bottom) continue;
        const float px = static_cast<float>(static_cast<i16>((layout->rectangle.left + layout->rectangle.right) * .5f));
        const float py = static_cast<float>(static_cast<i16>((layout->rectangle.top + layout->clip_rectangle.bottom) * .5f));
        const auto hit = ui_friends_scene_hit_test(*current, px, py);
        if (hit.action != UiFriendsAction::accept) continue;
        *user = hit.user_id;*x = px;*y = py;
        return true;
    }
    return false;
}
// A press on the Accept of a row the list's edge cuts does not scroll the row before the release, also when
// Gateway traffic rebuilds the list while the button is held. The release sends the request.
void pointer_cut_row_runtime_fixture() noexcept {
    const Window window = window_test::open();
    check(window != nullptr, "friends cut row: owns a headless pointer-capture fixture");
    if (!window) return;
    g_app.window = window;
    constexpr auto list = utf8_literal(R"([{"id":"1250","type":1,"user":{"id":"1250","username":"cut.friend"}},{"id":"1201","type":3,"user":{"id":"1201","username":"cut.01"}},{"id":"1202","type":3,"user":{"id":"1202","username":"cut.02"}},{"id":"1203","type":3,"user":{"id":"1203","username":"cut.03"}},{"id":"1204","type":3,"user":{"id":"1204","username":"cut.04"}},{"id":"1205","type":3,"user":{"id":"1205","username":"cut.05"}},{"id":"1206","type":3,"user":{"id":"1206","username":"cut.06"}},{"id":"1207","type":3,"user":{"id":"1207","username":"cut.07"}},{"id":"1208","type":3,"user":{"id":"1208","username":"cut.08"}},{"id":"1209","type":3,"user":{"id":"1209","username":"cut.09"}},{"id":"1210","type":3,"user":{"id":"1210","username":"cut.10"}},{"id":"1211","type":3,"user":{"id":"1211","username":"cut.11"}},{"id":"1212","type":3,"user":{"id":"1212","username":"cut.12"}},{"id":"1213","type":3,"user":{"id":"1213","username":"cut.13"}},{"id":"1214","type":3,"user":{"id":"1214","username":"cut.14"}},{"id":"1215","type":3,"user":{"id":"1215","username":"cut.15"}},{"id":"1216","type":3,"user":{"id":"1216","username":"cut.16"}}])");
    u64 user = 0;UiRect button{}, clip{};float x = 0, y = 0;
    check(snapshot(list, 800, utf8_literal(R"({"friends":[{"user_id":"1250","status":"online"}]})")) &&
              application_friends_synchronize() && tab(UiFriendsTab::pending) && button_past_edge(&button, &clip),
          "friends cut row: the Pending list runs past its bottom edge");
    // Scroll so that the edge leaves only the top 12 dips of the first row past it in view. Scrolling that row into
    // view on the press would move its Accept button out from under the pointer.
    check(ui_friends_scene_scroll(scene(), button.top + 12 - clip.bottom) &&
              application_friends_synchronize() && cut_accept(&user, &x, &y),
          "friends cut row: the list's bottom edge cuts an Accept button");
    const float scroll = scene()->scroll_dips;
    const u32 submissions = seen.submissions, rows = scene()->source_revision;
    check(ui_friends_target_equal(ui_friends_scene_hit_test(*scene(), x, y), {UiFriendsAction::accept, user, UiFriendsTab::pending}) &&
              application_friends_window_event(window_test::pointer_down(x, y)) && scene()->scroll_dips == scroll,
          "friends cut row: the press focuses the cut row's Accept and leaves the list where it is");
    GatewayClientEvent presence{};
    presence.kind = GatewayClientEventKind::dispatch;presence.dispatch_kind = GatewayClientDispatchKind::presence_update;
    presence.presence.user_id = 1250;presence.presence.status = GatewayClientMemberListStatus::idle;
    application_friends_gateway(presence, {});
    check(application_friends_synchronize() && scene()->source_revision != rows && scene()->scroll_dips == scroll,
          "friends cut row: a rebuild while the button is held keeps the pressed row in place");
    check(application_friends_window_event(window_test::pointer_up(x, y, PointerButton::left)) &&
              application_friends_pending(9000) && application_friends_submit(730, 9000) &&
              seen.submissions == submissions + 1 && seen.operation == RelationshipOperation::accept && seen.user_id == user &&
              context_is(kFriendsContext) && response(730, 204),
          "friends cut row: the release sends the Accept of the pressed row");
    check(tab(UiFriendsTab::all), "friends cut row: the fixture returns to All");
    g_app.window = nullptr;window_test::close();
}
// The official client sends X-Context-Properties {"location":"Friends"} when a Friends row accepts, declines,
// cancels or removes, and {"location":"Spam requests modal"} from the spam requests dialog. Other requests carry
// no location.
void context_location_runtime_fixture() noexcept {
    constexpr auto list = utf8_literal(R"([{"id":"1101","type":1,"user":{"id":"1101","username":"context.friend"}},{"id":"1102","type":3,"is_stranger_request":false,"user":{"id":"1102","username":"context.declined"}},{"id":"1103","type":4,"user":{"id":"1103","username":"context.sent"}},{"id":"1104","type":3,"user_ignored":true,"user":{"id":"1104","username":"context.hidden"}},{"id":"1105","type":3,"user":{"id":"1105","username":"context.profile"}},{"id":"1106","type":3,"is_stranger_request":true,"user":{"id":"1106","username":"context.stranger"}}])");
    check(snapshot(list, 700) && application_friends_synchronize() && tab(UiFriendsTab::all),
          "friends context: publishes a friend, requests both ways, a hidden request and a stranger");
    check(action(UiFriendsAction::remove, 1101) && save_form() && application_friends_submit(720, 8000) &&
              seen.operation == RelationshipOperation::remove && seen.method == RestMethod::delete_method &&
              context_is(kFriendsContext) && response(720, 204),
          "friends context: Remove Friend from the row's More menu names Friends");
    check(tab(UiFriendsTab::pending) && action(UiFriendsAction::decline, 1102, UiFriendsTab::pending) &&
              application_friends_submit(721, 8100) && seen.operation == RelationshipOperation::remove &&
              context_is(kFriendsContext) && response(721, 204),
          "friends context: the incoming row's X names Friends");
    check(action(UiFriendsAction::cancel, 1103, UiFriendsTab::pending) && application_friends_submit(722, 8200) &&
              seen.operation == RelationshipOperation::remove && context_is(kFriendsContext) && response(722, 204),
          "friends context: the outgoing row's Cancel names Friends");
    check(action(UiFriendsAction::accept, 1106, UiFriendsTab::pending) && save_form() &&
              application_friends_submit(723, 8300) && seen.operation == RelationshipOperation::accept_stranger &&
              context_is(kFriendsContext) && response(723, 204),
          "friends context: an Accept confirmed for a stranger names Friends");
    check(action(UiFriendsAction::toggle_hidden, 0, UiFriendsTab::pending) && application_friends_requests_active() &&
              action(UiFriendsAction::accept, 1104, UiFriendsTab::pending) && application_friends_submit(724, 8400) &&
              seen.operation == RelationshipOperation::accept &&
              context_is(utf8_literal("X-Context-Properties: eyJsb2NhdGlvbiI6IlNwYW0gcmVxdWVzdHMgbW9kYWwifQ==\r\n")) &&
              response(724, 204),
          "friends context: Accept in the hidden requests dialog names the spam requests dialog");
    if (application_friends_requests_active()) application_friends_close_requests();
    check(application_friends_synchronize() && application_friends_user_action(1105, ApplicationFriendsUserAction::accept) &&
              application_friends_submit(725, 8500) && seen.operation == RelationshipOperation::accept &&
              context_is({}) && response(725, 204),
          "friends context: Accept from a profile carries no location");
    check(tab(UiFriendsTab::all) && action(UiFriendsAction::block, 1105) && save_form() &&
              application_friends_submit(726, 8600) && seen.operation == RelationshipOperation::block && context_is({}) &&
              response(726, 204),
          "friends context: Block carries no location, as the official Friends row offers no Block");
}
// The list at the realistic maximum (1,000 friends and 5,000 blocked users, about 3 MB) loads through the REST path.
// A reply over the decoder's 4 MiB cap fails and keeps the list it had.
void full_list_fixture() noexcept {
    constexpr u32 friends = 1000, blocked = 5000, capacity = 5u << 20;
    char* reply = static_cast<char*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(reply != nullptr, "friends full list: fixture reply allocates");
    if (!reply) return;
    const u32 bytes = relationships_reply(reply, friends, blocked, 500000);
    check(bytes > 2u << 20 && bytes < kRelationshipsMaximumBytes,
          "friends full list: the realistic maximum is above the old 256 KiB and 2 MiB caps and within 4 MiB");
    check(tab(UiFriendsTab::all) && queue(RelationshipOperation::list) && application_friends_submit(601, 9000) &&
              seen.operation == RelationshipOperation::list && seen.method == RestMethod::get &&
              seen.response_capacity == kRelationshipsMaximumBytes,
          "friends full list: the GET asks for a large response of the decoder's cap");
    const bool loaded = response(601, 200, {reply, bytes}) && application_friends_synchronize();
    check(loaded && g_app.friends->store.count == friends + blocked && g_app.friends->ready && scene() &&
              scene()->source_count == friends,
          "friends full list: 1,000 decorated friends and 5,000 blocked users load into the list");
    const u32 revision = g_app.friends->store.revision;
    const u32 over = relationships_reply(reply, 0, 10000, 900000);
    check(over > kRelationshipsMaximumBytes && queue(RelationshipOperation::list) &&
              application_friends_submit(602, 9100) && response(602, 200, {reply, over}) &&
              g_app.friends->store.count == friends + blocked && g_app.friends->store.revision == revision &&
              g_app.friends->request.phase == FriendsRequestPhase::failed,
          "friends full list: a reply over the cap fails and keeps the old list");
    RestClientResponse failed{};
    failed.request_id = 603;failed.error = RestClientError::response;
    check(queue(RelationshipOperation::list) && application_friends_submit(603, 9200) &&
              application_friends_response(failed, {}) && g_app.friends->store.count == friends + blocked &&
              g_app.friends->store.revision == revision,
          "friends full list: a reply the REST client refuses (over its cap) keeps the old list");
    VirtualFree(reply, 0, MEM_RELEASE);
}
struct Cleanup {
    ~Cleanup() noexcept {
        application_friends_release();popup_scene_release(&g_app.popup_scene);ui_scene_release(&g_app.shell_scene);
        if (g_app.account_ui) VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
        g_app.account_ui = nullptr;g_app.account = {};g_app.context_focus = nullptr;
    }
};
void run() noexcept {
    Cleanup cleanup{};
    g_app.account_ui = static_cast<AccountUiRuntime*>(VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.account_ui != nullptr, "friends: owned zeroed account UI fixture allocates without credentials");
    if (!g_app.account_ui) return;
    auto& ui = *g_app.account_ui;
    check(chat_state_store_initialize(&ui.chat_state, {ui.guild_record_storage, 16}, {ui.guild_order_storage, 16},
            {ui.channel_record_storage, 64}, {ui.channel_order_storage, 64}, {ui.chat_state_name_storage, 4096}) &&
        user_store_initialize(&ui.user_store, {ui.user_record_storage, 64}, {ui.user_order_storage, 64},
            {ui.user_name_storage, 8192}) && ui_scene_initialize_bootstrap(&g_app.shell_scene)&&
        popup_scene_initialize_owned(&g_app.popup_scene,8,{popup_widgets,32},{popup_nodes,32}),
        "friends: actual state/user stores and retained shell initialize with bounded fixture backing");
    g_app.renderer_ready = true;g_app.renderer.width = 1280;g_app.renderer.height = 700;g_app.renderer.dpi = 96;
    g_app.window = nullptr;g_app.keyboard_focus_visible = true;g_app.preferences = client_preferences_defaults();
    g_app.account.instance = &account;g_app.account.status.profile_id = 4;g_account_fake.relationships = submit;
    check(snapshot(utf8_literal("[]"), 1) && application_friends_show() && scene() &&
        scene()->phase == UiFriendsPhase::ready && !scene()->source_count && !application_friends_pending(1000) && !seen.submissions,
        "friends: authoritative empty READY shows ready empty scene and never queues implicit REST GET");
    if (!scene()) return;
    const auto empty_storage=application_friends_storage_metrics();
    check(empty_storage.controller_bytes<128*1024&&empty_storage.active.reserved_bytes&&empty_storage.candidate.reserved_bytes&&
        empty_storage.indices.reserved_bytes&&!empty_storage.active.committed_bytes&&!empty_storage.candidate.committed_bytes&&
        !empty_storage.indices.committed_bytes&&!empty_storage.active.admitted_count&&!empty_storage.candidate.admitted_count&&
        !empty_storage.indices.admitted_count,
        "friends storage: authoritative empty model reserves three banks without touching array pages");
    const u32 revision = scene()->revision, builds = seen.builds;
    check(application_friends_synchronize() && scene()->revision == revision && seen.builds == builds,
        "friends: idle synchronization retains scene revision and text publication");
    context_menu_coordinates();
    seen.settings = true;
    check(!synthetic_input(UiInputKind::character, 'x', false) &&
        !application_friends_window_event(window_test::context_menu(600, 150)) &&
        application_friends_synchronize() && seen.builds == builds && !application_friends_show(),
        "friends: opaque settings barrier suppresses Friends input and hidden text rebuild");
    seen.settings = false;
    const auto widgets = memory_mark(scene()->storage.arena.base);
    application_friends_hide();
    check(!application_friends_active() && !scene() && memory_released(widgets) && !application_friends_pending(1000),
        "friends: hide unbinds and frees retained widgets while preserving authoritative empty state");
    seen.fail_build = true;
    check(!application_friends_show() && !application_friends_active() && !scene(),
        "friends: failed renderer publication rolls back visible scene");
    seen.fail_build = false;
    constexpr auto initial = utf8_literal(R"([{"id":"10","type":1,"user":{"id":"10","username":"alice","global_name":"Alice"}},{"id":"20","type":3,"user":{"id":"20","username":"bob","global_name":"Bob"}}])");
    check(snapshot(initial, 2, utf8_literal(R"({"friends":[{"user_id":"10","status":"online"}]})")) && application_friends_show() &&
        scene() && scene()->source_count == 1 && !application_friends_pending(1000),
        "friends: real typed Gateway publisher hydrates named authoritative rows and initial presence without GET");
    if (!scene()) return;
    const auto small_storage=application_friends_storage_metrics();SYSTEM_INFO storage_system{};GetSystemInfo(&storage_system);
    check(small_storage.active.committed_bytes<=storage_system.dwPageSize&&!small_storage.candidate.committed_bytes&&
        small_storage.indices.committed_bytes<=storage_system.dwPageSize&&small_storage.active.admitted_count>=2&&
        !small_storage.candidate.admitted_count&&small_storage.indices.admitted_count>=1,
        "friends storage: two relationships commit at most one native page per bank and none stay in the candidate bank");
    const u32 modifier_submissions = seen.submissions, modifier_forms = seen.form_opens;
    const auto modifier_tab = scene()->tab;
    check(ui_friends_scene_focus(scene(), {UiFriendsAction::select_tab, 0, UiFriendsTab::pending}) &&
        !synthetic_input(UiInputKind::key_down, key::enter, false, synthetic_control) &&
        !synthetic_input(UiInputKind::key_up, key::enter, false, synthetic_control) &&
        scene()->tab == modifier_tab && seen.submissions == modifier_submissions && seen.form_opens == modifier_forms,
        "friends fixture: explicit Ctrl rejects retained tab activation before mutation without weakening production modifiers");
    check(tab(UiFriendsTab::pending) && scene()->source_count == 1 &&
        action(UiFriendsAction::accept, 20, UiFriendsTab::pending) && application_friends_pending(1000),
        "friends: real pending-row Enter queues accept without optimistically changing relationship");
    UiFriendsRow before{};
    check(row(20, &before) && before.kind == UiFriendsKind::incoming && before.busy,
        "friends: queued mutation keeps authoritative incoming row and reports pending feedback");
    seen.admission = RestClientSubmitError::busy;
    check(!application_friends_submit(1, 1000) && !application_friends_pending(1099) &&
        application_friends_pending(1100) && application_friends_next_wake() == 1100 &&
        g_app.message_history_request_owner == MessageHistoryRequestOwner::none && !g_app.primary_request_id,
        "friends: false busy admission keeps fixed queued mutation and waits 100ms without claiming primary lane");
    seen.admission = RestClientSubmitError::none;
    check(application_friends_submit(2, 1100) && seen.operation == RelationshipOperation::accept &&
        seen.user_id == 20 && seen.request_id == 2 && !application_friends_pending(1200),
        "friends: accepted mutation submits actual bounded account helper once and waits for matching completion");
    check(response(999, 204) && row(20, &before) && before.kind == UiFriendsKind::incoming && before.busy,
        "friends: stale response ID cannot complete current mutation");
    check(response(2, 429, {}, 1500) && !application_friends_pending(1499) && application_friends_pending(1500) &&
        application_friends_submit(3, 1500) && response(3, 204) && tab(UiFriendsTab::all) &&
        row(20, &before) && before.kind == UiFriendsKind::friend_user && !before.busy,
        "friends: 429 uses server retry deadline and matching successful accept becomes authoritative friend");
    const auto before_block_forms=seen.form_opens;const auto before_block_submissions=seen.submissions;
    check(action(UiFriendsAction::block, 20)&&seen.form_opens==before_block_forms+1&&seen.form.confirmation_only&&
        seen.form.valid&&seen.form.valid(seen.form.owner)&&!application_friends_pending(1600)&&seen.submissions==before_block_submissions&&
        save_form()&&application_friends_submit(4,1600),
        "friends: More Block opens native confirmation and only explicit approval submits");
    const auto blocked = [](u64 id) noexcept {
        const auto* store = application_friends_relationships();
        const auto* record = store ? relationship_store_find(*store, id) : nullptr;
        return record && record->type == RelationshipType::blocked;
    };
    check(snapshot(utf8_literal(R"([{"id":"10","type":1},{"id":"20","type":2}])"), 3) &&
        application_friends_synchronize() && response(4, 204) && tab(UiFriendsTab::all) && blocked(20) &&
        !row(20, &before) && scene()->source_count == 1,
        "friends: newer Gateway membership epoch survives late successful mutation completion; blocked users list no tab");
    check(application_friends_user_action(20, ApplicationFriendsUserAction::unblock) && application_friends_submit(5, 1700) &&
        response(5, 429, {}, 1800, {0, false, true}) &&
        !application_friends_pending(100000) && !application_friends_next_wake() && scene()->phase == UiFriendsPhase::error,
        "friends: CAPTCHA rejection becomes explicit error without automatic rate-limit retry");
    check(action(UiFriendsAction::retry, 0, UiFriendsTab::all) && application_friends_submit(6, 1800) &&
        response(6, 429, {}, 1900, {40002, true, false}) && !application_friends_pending(100000) &&
        !application_friends_next_wake(),
        "friends: account verification code alone also suppresses automatic 429 retry");
    check(action(UiFriendsAction::retry, 0, UiFriendsTab::all) && application_friends_submit(60, 1900) &&
        response(60, 204) && !blocked(20) && scene()->source_count == 1,
        "friends: explicit Retry resubmits preserved operation and successful removal updates authoritative rows");
    check(action(UiFriendsAction::profile, 10) && seen.profile_user == 10 &&
        action(UiFriendsAction::open_dm, 10) && seen.dm_user == 10 && !application_friends_active() && !scene() &&
        application_friends_show() && !application_friends_pending(2000),
        "friends: profile and DM semantic actions route correct identity, hide chat surface and reopen without GET");
    UiFriendsTarget send_checked{};
    const UiFriendsTarget send{UiFriendsAction::add_friend, 0, UiFriendsTab::add_friend};
    const auto before_add_forms=seen.form_opens;
    check(tab(UiFriendsTab::add_friend) && application_friends_search_focused() && application_friends_search_model() &&
        scene()->content->search.active && !ui_friends_scene_activation(*scene(), send, &send_checked) &&
        seen.form_opens == before_add_forms,
        "friends: Add Friend opens on its own username field; Send waits for a name and no dialog opens");
    const auto add_submissions = seen.submissions;
    check(replace_query(utf8_literal("bad name!")) && synthetic_input(UiInputKind::key_down, key::enter, false) &&
        !application_friends_pending(2000) && seen.submissions == add_submissions &&
        utf8_equal(scene()->status, utf8_literal("Hm, didn't work. Double check that the username is correct.")) &&
        scene()->content->tone == UiFriendsTone::danger && scene()->phase == UiFriendsPhase::ready,
        "friends: Enter on a malformed username reports under the field without a request");
    auto* add_field = application_friends_search_model();
    check(add_field && synthetic_input(UiInputKind::key_down, 'A', false, synthetic_control) && add_field->anchor == 0 &&
        add_field->caret == add_field->bytes && add_field->bytes == 9 &&
        synthetic_input(UiInputKind::key_down, key::backspace, false) && !add_field->bytes,
        "friends: Ctrl+A selects the whole Add Friend field and Backspace then clears it");
    check(replace_query(utf8_literal("friend.name")) && !scene()->status.bytes &&
        scene()->content->tone == UiFriendsTone::neutral && action(UiFriendsAction::add_friend, 0, UiFriendsTab::add_friend),
        "friends: typing clears the result and Send Friend Request queues the field's username");
    check(application_friends_submit(7, 2000) && seen.operation == RelationshipOperation::request &&
        utf8_equal({seen.body, seen.body_bytes}, utf8_literal("{\"username\":\"friend.name\",\"discriminator\":null}")) && response(7, 400) &&
        !application_friends_pending(100000) && scene()->phase == UiFriendsPhase::ready &&
        utf8_equal(scene()->status, utf8_literal("Hm, didn't work. Double check that the username is correct.")) &&
        scene()->content->tone == UiFriendsTone::danger &&
        utf8_equal(message_composer_text(*application_friends_search_model()), utf8_literal("friend.name")),
        "friends: a rejected request reports under the field, keeps the name and never invents an outgoing row");
    check(action(UiFriendsAction::add_friend, 0, UiFriendsTab::add_friend) && application_friends_submit(8, 2100) &&
        response(8, 400, {}, 0, {80000, false, false}) &&
        utf8_equal(scene()->status, utf8_literal("friend.name is not accepting friend requests. They\xE2\x80\x99ll have to add you to become friends.")),
        "friends: a closed account names the user it could not reach");
    check(action(UiFriendsAction::add_friend, 0, UiFriendsTab::add_friend) && application_friends_submit(80, 2150) &&
        response(80, 400, {}, 0, {80007, false, false}) &&
        utf8_equal(scene()->status, utf8_literal("You're already friends with that user!")),
        "friends: an existing friendship is reported as Discord words it");
    check(action(UiFriendsAction::add_friend, 0, UiFriendsTab::add_friend) && application_friends_submit(81, 2200) &&
        utf8_equal({seen.body, seen.body_bytes}, utf8_literal("{\"username\":\"friend.name\",\"discriminator\":null}")) && response(81, 204) &&
        utf8_equal(scene()->status, utf8_literal("Success! Your friend request to friend.name was sent.")) &&
        scene()->content->tone == UiFriendsTone::positive && !application_friends_search_model()->bytes,
        "friends: a sent request reports success and empties the field for the next name");
    check(tab(UiFriendsTab::all) && !scene()->status.bytes && scene()->content->tone == UiFriendsTone::neutral,
        "friends: leaving Add Friend drops its finished result");
    check(tab(UiFriendsTab::all) && recovery_error(4) && scene()->source_count == 1 &&
        action(UiFriendsAction::retry) && application_friends_submit(9, 2200) && seen.operation == RelationshipOperation::list,
        "friends: incomplete Gateway snapshot preserves current model and explicit recovery Retry queues GET");
    check(snapshot(utf8_literal(R"([{"id":"30","type":1,"user":{"id":"30","username":"carol"}}])"), 5) &&
        response(9, 200, utf8_literal(R"([{"id":"10","type":1}])")) && scene()->source_count == 1 && row(30, &before),
        "friends: stale GET snapshot cannot overwrite newer completed Gateway membership transaction");
    check(recovery_error(6) && action(UiFriendsAction::retry) && application_friends_submit(10, 2300) &&
        response(10, 200, utf8_literal("[{\"id\":")) && scene()->phase == UiFriendsPhase::error && scene()->source_count == 1,
        "friends: malformed bounded GET response preserves existing authoritative records and exposes Retry");
    check(action(UiFriendsAction::retry) && application_friends_submit(11, 2400),
        "friends: failed GET can be retried explicitly");
    GatewayClientEvent presence{};
    presence.kind = GatewayClientEventKind::dispatch;presence.dispatch_kind = GatewayClientDispatchKind::presence_update;
    presence.presence.user_id = 30;presence.presence.status = GatewayClientMemberListStatus::idle;
    application_friends_gateway(presence, {});
    check(response(11, 200, utf8_literal(R"([{"id":"30","type":1,"nickname":"REST confirmed"}])")) &&
        row(30, &before) && utf8_equal(before.display_name, utf8_literal("REST confirmed")) && before.presence == UiPresence::idle,
        "friends: presence churn does not invalidate GET membership epoch and REST preserves newer presence");
    check(recovery_error(7) && action(UiFriendsAction::retry) && application_friends_submit(12, 2500),
        "friends: recovery GET starts for oversized-response fixture");
    RestClientResponse oversized{};
    oversized.request_id = 12;oversized.error = RestClientError::response;
    oversized.body_bytes = kAccountHistoryResponseBytes + 1;
    check(application_friends_response(oversized, {}) && !application_friends_pending(100000) &&
        scene()->source_count == 1 && scene()->phase == UiFriendsPhase::error,
        "friends: oversize transport failure keeps model without hidden retry");
    check(snapshot(utf8_literal(R"([{"id":"40","type":3,"user":{"id":"40","username":"dana"}}])"), 8) &&
        application_friends_synchronize() && tab(UiFriendsTab::pending) &&
        action(UiFriendsAction::accept, 40, UiFriendsTab::pending) && application_friends_submit(13, 2600) &&
        response(13, 429, {}, 2700),
        "friends: incoming Accept receives genuine 429 before authoritative live removal");
    check(remove_relationship(utf8_literal(R"({"id":"40"})"), 9) && !row(40, &before),
        "friends: actual typed RELATIONSHIP_REMOVE removes rate-limited incoming row before retry deadline");
    const u32 stale_submissions = seen.submissions;
    check(!application_friends_pending(2699) && application_friends_pending(2700) &&
        !application_friends_submit(14, 2700) && seen.submissions == stale_submissions &&
        !application_friends_pending(100000) && !application_friends_next_wake() && scene()->phase == UiFriendsPhase::error,
        "friends: stale Accept deadline becomes explicit error without resubmitting PUT after Gateway removal");
    const bool copied_snapshot = snapshot(utf8_literal(R"([{"id":"50","type":1,"user":{"id":"50","username":"elliot"}}])"), 10);
    const bool copied_sync = copied_snapshot && application_friends_synchronize();
    const bool copied_tab = copied_sync && tab(UiFriendsTab::add_friend);
    const bool copied_typed = copied_tab && replace_query(utf8_literal("pending.friend"));
    const bool copied_action = copied_typed && action(UiFriendsAction::add_friend, 0, UiFriendsTab::add_friend);
    const bool copied_submit = copied_action && application_friends_submit(15, 2800);
    const bool copied_operation = copied_submit && seen.operation == RelationshipOperation::request;
    check(copied_operation,
        "friends: copied username mutation is accepted before incomplete live-state marker");
    GatewayClientEvent incomplete{};
    incomplete.kind = GatewayClientEventKind::dispatch;
    incomplete.dispatch_kind = GatewayClientDispatchKind::relationship_state_incomplete;
    application_friends_gateway(incomplete, {});
    check(response(15, 204) && scene()->phase == UiFriendsPhase::error &&
        !application_friends_pending(100000) && !application_friends_next_wake() &&
        action(UiFriendsAction::retry, 0, UiFriendsTab::add_friend) &&
        application_friends_submit(16, 2900) && seen.operation == RelationshipOperation::list &&
        !seen.user_id && !seen.body_bytes,
        "friends: successful in-flight mutation preserves incomplete-state Retry and recovery GET clears old POST scope");
    check(response(16, 200, utf8_literal(R"([{"id":"50","type":1}])")) &&
        scene()->phase == UiFriendsPhase::ready && !application_friends_pending(100000) &&
        tab(UiFriendsTab::all) && scene()->source_count == 1,
        "friends: complete recovery GET clears persistent incomplete marker and restores ready rows");
    UiFriendsTarget accepted{};
    const UiFriendsTarget legal_old_row{UiFriendsAction::profile, 50, UiFriendsTab::all};
    check(ui_friends_scene_activation(*scene(), legal_old_row, &accepted) &&
        remove_relationship(utf8_literal(R"({"id":"50"})"), 11, false) &&
        !ui_friends_scene_activation(*scene(), legal_old_row, &accepted),
        "friends: authoritative live mutation invalidates a previously legal retained row before repaint");
    check(application_friends_synchronize() && !scene()->source_count,
        "friends: repaint rebinds fresh empty projection after live row invalidation");
    search_runtime_fixture();confirmation_runtime_fixture();pending_sections_runtime_fixture();ignore_and_decline_runtime_fixture();
    stale_classification_runtime_fixture();stale_bulk_retry_runtime_fixture();nickname_and_call_runtime_fixture();
    bounded_performance_fixture();
    pointer_accept_runtime_fixture();
    pointer_cut_row_runtime_fixture();
    context_location_runtime_fixture();
    full_list_fixture();
    const auto owner = memory_mark(g_app.friends);
    const auto requests_bank = memory_mark(application_friends_storage_metrics().requests_indices.address);
    application_friends_release();
    check(!g_app.friends && !scene() && !application_friends_pending(100000) && memory_released(owner) &&
        seen.form.valid && !seen.form.valid(seen.form.owner),
        "friends: final release unbinds renderer, invalidates retained form owner and frees controller VM");
    check(memory_released(requests_bank),
        "friends storage: final controller release frees the optional hidden index bank");
}
} // namespace maicord::friends_runtime_test

namespace maicord {
bool text_edit_layout_hit_test(const TextEditLayoutOwner&,float,float,u32*) noexcept {return false;}
ClipboardTextResult clipboard_read_utf8(const Slice<char>& output) noexcept {
    const auto& state=friends_runtime_test::seen;
    if(!output.data&&output.count)return {0,ClipboardTextError::invalid_input};
    if(state.clipboard_bytes>output.count)return {0,ClipboardTextError::capacity};
    for(u32 i=0;i<state.clipboard_bytes;++i)output[i]=state.clipboard[i];
    return {state.clipboard_bytes,ClipboardTextError::none};
}
bool clipboard_write_utf8(const Utf8View& text) noexcept {
    auto& state=friends_runtime_test::seen;
    if(text.bytes>sizeof(state.clipboard)||!utf8_validate(text).valid)return false;
    for(u32 i=0;i<text.bytes;++i)state.clipboard[i]=text.data[i];state.clipboard_bytes=text.bytes;return true;
}
RestClientSubmitResult rest_client_submit_large_response(RestClient* client, const RestClientRequest& request,
                                                         u32 response_capacity) noexcept {
    friends_runtime_test::seen.response_capacity = response_capacity;
    return rest_client_submit(client, request);
}
RestClientSubmitResult rest_client_submit(RestClient*, const RestClientRequest& request) noexcept {
    auto& capture = friends_runtime_test::seen;
    ++capture.submissions;capture.request_id = request.request_id;
    capture.method=request.route.method;
    if(request.target.count>sizeof(capture.target))return {RestClientSubmitError::invalid_input};
    capture.target_bytes=request.target.count;
    for(u32 i=0;i<request.target.count;++i)capture.target[i]=static_cast<char>(request.target[i]);
    if (request.body.count > sizeof(capture.body)) return {RestClientSubmitError::body_capacity};
    capture.body_bytes = request.body.count;
    for (u32 i = 0; i < request.body.count; ++i) capture.body[i] = static_cast<char>(request.body[i]);
    if (request.extra_headers.count > sizeof(capture.headers)) return {RestClientSubmitError::invalid_input};
    capture.header_bytes = request.extra_headers.count;
    for (u32 i = 0; i < request.extra_headers.count; ++i) capture.headers[i] = static_cast<char>(request.extra_headers[i]);
    return {capture.admission};
}
void renderer_bind_friends_scene(D3D11Renderer* renderer, const UiFriendsScene* value) noexcept {
    if (renderer->friends_scene == value) return;
    renderer->friends_scene = value;renderer->interaction_text_friends_revision = ~(value ? value->revision : 0u);
}
void renderer_bind_friends_requests_scene(D3D11Renderer* renderer, const UiFriendsScene* value) noexcept {
    if(renderer->friends_requests_scene==value)return;
    renderer->friends_requests_scene=value;renderer->interaction_text_friends_requests_revision=~(value?value->revision:0u);
}
bool renderer_rebuild_content_text(D3D11Renderer* renderer, const Theme&) noexcept {
    auto& state = friends_runtime_test::seen;++state.builds;
    if (state.fail_build) return false;
    renderer->interaction_text_friends_revision = renderer->friends_scene ? renderer->friends_scene->revision : 0;
    renderer->interaction_text_friends_requests_revision=renderer->friends_requests_scene?renderer->friends_requests_scene->revision:0;
    return true;
}
namespace app_detail {
u32 application_plugins_menu_items(ApplicationPluginMenuSurface, u64, Slice<PopupItem>, u64,
                                   const ApplicationPluginMenuExtra*) noexcept { return 0; }
bool application_plugins_menu_bind() noexcept { return true; }
void application_plugins_menu_dismissed() noexcept {}
void application_plugins_own_removal(PluginOwnRemoval, u64) noexcept {}
bool application_settings_active() noexcept { return friends_runtime_test::seen.settings; }
bool synchronize_navigation_view() noexcept { return true; }
bool synchronize_selected_channel() noexcept { return true; }
bool synchronize_shell_layout() noexcept {
    return ui_scene_layout_with_metrics(&g_app.shell_scene, application_friends_active() ? UiShellView::friends : UiShellView::private_channels,
        application_friends_active() ? ui_friends_shell_layout_metrics(kUiThemeMetricsDefault)
                                     : ui_guild_channel_shell_layout_metrics(kUiThemeMetricsDefault),
        1280, 700);
}
bool refresh_account_text(bool) noexcept { return true; }
void application_composer_text_input_suspend() noexcept {}
bool application_composer_text_input_synchronize() noexcept {++friends_runtime_test::seen.input_syncs;return true;}
void application_composer_text_input_notify_layout() noexcept {++friends_runtime_test::seen.input_layouts;}
bool dismiss_popup() noexcept {popup_scene_close(&g_app.popup_scene);g_app.popup_owner_kind=PopupOwnerKind::none;return true;}
void set_popup_owner(PopupOwnerKind owner) noexcept {g_app.popup_owner_kind=owner;}
UiRect application_viewport_dips() noexcept {return {0,0,1280,700};}
bool application_profile_close() noexcept { return true; }
void application_image_viewer_close() noexcept {}
void application_tabs_note_navigation() noexcept {}
bool application_profile_open(u64 user, const UiRect&, ProfileAnchor) noexcept {
    friends_runtime_test::seen.profile_user = user;return true;
}
bool application_user_menu_open(u64, UiRect, u64, UserMenuSource, ConstSlice<PopupItem>,
                                const UiPopupFeedback*) noexcept {
    return true;
}
bool queue_private_conversation(PrivateConversationOperation operation, u64 user) noexcept {
    if (operation != PrivateConversationOperation::open) return false;
    friends_runtime_test::seen.dm_user = user;return true;
}
bool application_private_channel_call(u64 channel) noexcept {
    friends_runtime_test::seen.call_channel = channel;return true;
}
bool application_private_channel_request_busy(u64) noexcept { return false; }
bool application_private_channel_accept_request(u64 channel) noexcept {
    friends_runtime_test::seen.request_channel = channel;return true;
}
bool application_private_channel_ignore_request(u64 channel) noexcept {
    friends_runtime_test::seen.request_channel = ~channel;return true;
}
void application_private_channel_set_requests_mode(bool requests) noexcept {
    friends_runtime_test::seen.requests_mode = requests;
}
bool application_motion_hover_tick(UiMotionHover* hover, u64 id, u64 now, const UiMotionStyle& style) noexcept {
    return ui_motion_hover_tick(hover, id, now, style, true);
}
bool application_text_form_open(const ApplicationTextFormRequest& request) noexcept {
    ++friends_runtime_test::seen.form_opens;friends_runtime_test::seen.form = request;
    g_app.text_form=reinterpret_cast<ApplicationTextForm*>(&friends_runtime_test::modal_token);return true;
}
void application_text_form_cancel_owner(TextFormOwnerValid validator) noexcept {
    if(friends_runtime_test::seen.form.valid==validator)g_app.text_form=nullptr;
}
} // namespace app_detail
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_friends_runtime_entry() noexcept {
    maicord::friends_runtime_test::run();
    maicord::test_exit(maicord::friends_runtime_test::seen.failures, "Friends controller runtime");
}
