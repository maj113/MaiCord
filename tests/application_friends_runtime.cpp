#include "application_user_actions.hpp"
#include "application_state.hpp"
#include "application_friends.hpp"
#include "application_private_conversation.hpp"
#include "application_profile.hpp"
#include "application_message_interaction.hpp"
#include "application_text_form.hpp"
#include "application_friends_search.hpp"
#include "application_friends_storage.hpp"
#include "application_plugins.hpp"
#include "maicord/app/account_relationships.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/clipboard.hpp"
#include "application_action_captcha_stubs.hpp"
#include "test_output.hpp"

#include "maicord/core/win32_user.hpp"

namespace maicord::friends_runtime_test {
using namespace app_detail;
struct AccountFixture { RestClient rest; } account{};
UiWidget popup_widgets[32]{};UiLayoutNode popup_nodes[32]{};u8 modal_token{};
struct Observations {
    u32 failures, submissions, builds, form_opens, input_layouts, input_syncs;
    u64 request_id, user_id, profile_user, dm_user;
    RelationshipOperation operation;
    RestMethod method;
    RestClientSubmitError admission;
    bool settings, fail_build;
    char body[256]; u32 body_bytes;
    char target[128];u32 target_bytes;
    char clipboard[kFriendsSearchBytes];u32 clipboard_bytes;
    ApplicationTextFormRequest form;
} seen{};
void check(bool valid, const char* label) noexcept {
    test_check(&seen.failures, valid, label);
}
enum SyntheticModifiers : u8 { synthetic_control = 1, synthetic_shift = 2, synthetic_alt = 4 };
bool synthetic_input(UiInputKind kind, u32 code, bool repeated, u8 modifiers = 0) noexcept {
    // The fixture supplies a key message directly, without a Windows input
    // message. Its modifiers must belong to that message rather than whatever
    // the interactive desktop happened to publish to this thread's key table.
    BYTE original[256]{}, keys[256]{};
    const bool saved = GetKeyboardState(original) != FALSE;
    check(saved, "friends fixture: thread keyboard state is saved before synthetic input");
    if (!saved) return false;
    const bool control = (modifiers & synthetic_control) != 0;
    const bool shift = (modifiers & synthetic_shift) != 0;
    const bool alt = (modifiers & synthetic_alt) != 0;
    if (control) keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
    if (shift) keys[VK_SHIFT] = keys[VK_LSHIFT] = 0x80;
    if (alt) keys[VK_MENU] = keys[VK_LMENU] = 0x80;
    const bool applied = SetKeyboardState(keys) != FALSE &&
        ((GetKeyState(VK_CONTROL) & 0x8000) != 0) == control &&
        ((GetKeyState(VK_SHIFT) & 0x8000) != 0) == shift &&
        ((GetKeyState(VK_MENU) & 0x8000) != 0) == alt;
    check(applied, "friends fixture: synthetic message owns exact thread-local modifiers");
    const bool handled = applied && application_friends_input(kind, code, repeated);
    BYTE restored_keys[256]{};
    bool restored = SetKeyboardState(original) != FALSE && GetKeyboardState(restored_keys) != FALSE;
    for (u32 i = 0; restored && i < sizeof(original); ++i)
        restored = restored_keys[i] == original[i];
    check(restored, "friends fixture: original thread keyboard state is restored after synthetic input");
    return handled && restored;
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
bool remove_relationship(Utf8View json, u64 sequence, bool synchronize = true) noexcept {
    u32 scratch[1024]{};char storage[512]{};
    GatewayEnvelope envelope{};
    envelope.event_name = {utf8_literal("RELATIONSHIP_REMOVE"), 0, false};
    envelope.data = {json, 0, JsonValueKind::object};
    envelope.has_event_name = true;envelope.has_sequence = true;envelope.sequence = sequence;
    GatewayClientEvent event{};ConstSlice<u8> payload{};
    const auto routed = gateway_client_route_dispatch(envelope, {scratch, 1024}, 64, {storage, 512}, {}, 4, &event, &payload);
    if (!routed.ok() || event.dispatch_kind != GatewayClientDispatchKind::relationship_remove) return false;
    application_friends_gateway(event, payload);
    return !synchronize || application_friends_synchronize();
}
bool action(UiFriendsAction operation, u64 user = 0, UiFriendsTab tab = UiFriendsTab::all,UiFriendsSectionKind section=UiFriendsSectionKind::incoming) noexcept {
    auto* current = scene();
    if(application_friends_search_focused()){
        synthetic_input(UiInputKind::key_down,VK_TAB,false);
        synthetic_input(UiInputKind::key_up,VK_TAB,false);
    }
    if(operation==UiFriendsAction::profile||operation==UiFriendsAction::remove||operation==UiFriendsAction::block||
        operation==UiFriendsAction::ignore||operation==UiFriendsAction::unignore){
        if(!current||!action(UiFriendsAction::more,user,tab)||!g_app.popup_scene.active||g_app.popup_scene.owner_id!=user)return false;
        return application_friends_popup_action(0xF100u+static_cast<u32>(operation));
    }
    if (!current || !ui_friends_scene_focus(current, {operation, user, tab,section})) return false;
    g_app.context_focus = &current->widgets;
    const bool down = synthetic_input(UiInputKind::key_down, VK_RETURN, false);
    const bool up = synthetic_input(UiInputKind::key_up, VK_RETURN, false);
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
    u64 user, Utf8View username, u64 id) noexcept {
    if (instance != &account) return {RestClientSubmitError::invalid_input};
    seen.operation = operation;seen.user_id = user;
    return account_relationship_submit(&account.rest, operation, user, username, id);
}
bool freed(void* pointer) noexcept {
    MEMORY_BASIC_INFORMATION information{};
    return pointer && VirtualQuery(pointer, &information, sizeof(information)) == sizeof(information) && information.State == MEM_FREE;
}
void context_menu_coordinates() noexcept {
    // Owned, never shown: native screen/client conversion without desktop input.
    HWND window = CreateWindowExW(0, L"STATIC", L"MaiCord Friends context fixture", WS_POPUP,
        400, 200, 1280, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "friends: owned hidden window provides context-menu coordinate fixture");
    if (!window) return;
    g_app.window = window;
    POINT content{600, 150}, sidebar{100, 150};
    const bool converted = ClientToScreen(window, &content) && ClientToScreen(window, &sidebar);
    check(converted && application_friends_window_message(WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window),
        MAKELPARAM(content.x, content.y)) && !application_friends_window_message(WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window),
        MAKELPARAM(sidebar.x, sidebar.y)),
        "friends: screen-coordinate context menu is swallowed in Friends content and leaves private sidebar available");
    g_app.context_focus = &scene()->widgets;
    check(application_friends_window_message(WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window), -1),
        "friends: keyboard context menu is swallowed when Friends owns logical focus");
    g_app.context_focus = &g_app.shell_scene.widgets;
    check(!application_friends_window_message(WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window), -1),
        "friends: keyboard context menu preserves shell/private-sidebar routing");
    g_app.context_focus = &scene()->widgets;
    g_app.window = nullptr;
    DestroyWindow(window);
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
    check(synthetic_input(UiInputKind::key_down,VK_SPACE,false)&&
        synthetic_input(UiInputKind::character,' ',false)&&
        utf8_equal(message_composer_text(*all),utf8_literal("needle "))&&application_friends_search_focused(),
        "friends search: Space key preserves edit ownership and its character inserts literal whitespace");
    check(synthetic_input(UiInputKind::key_down,VK_UP,false)&&synthetic_input(UiInputKind::key_down,VK_DOWN,false)&&
        synthetic_input(UiInputKind::key_down,VK_PRIOR,false)&&synthetic_input(UiInputKind::key_down,VK_NEXT,false)&&
        application_friends_search_focused()&&g_app.context_focus==key_owner&&replace_query(utf8_literal("needle")),
        "friends search: list navigation keys leave focused single-line edit and document unchanged");
    const auto target=application_friends_search_target();
    const auto source_revision=scene()->source->revision, count=scene()->source_count, text_revision=all->text_revision;
    const auto widgets=scene()->widgets.storage;const auto arena=scene()->storage.arena;
    const auto layout_revision=scene()->widgets.layout.revision;
    check(text_input_select(target,{0,2})==TextInputResult::changed&&application_friends_search_changed(false)&&
        scene()->source->revision==source_revision&&scene()->source_count==count&&scene()->content->total_count==3&&
        all->text_revision==text_revision&&scene()->widgets.storage.data==widgets.data&&scene()->widgets.storage.count==widgets.count&&
        scene()->widgets.layout.revision==layout_revision&&scene()->storage.arena.base==arena.base&&
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
        replace_query(utf8_literal("pending"))&&only_row(103)&&tab(UiFriendsTab::blocked)&&
        action(UiFriendsAction::search,0,UiFriendsTab::blocked)&&replace_query(utf8_literal("block.account"))&&only_row(104)&&
        tab(UiFriendsTab::all)&&utf8_equal(message_composer_text(*application_friends_search_model()),utf8_literal("needle"))&&
        scene()->source_count==2&&tab(UiFriendsTab::online)&&utf8_equal(message_composer_text(*application_friends_search_model()),utf8_literal("needle.user"))&&
        tab(UiFriendsTab::add_friend)&&!application_friends_search_model()&&!text_input_ready(application_friends_search_target()),
        "friends search: four tabs retain separate queries and Add friend exposes no search document");
    check(tab(UiFriendsTab::all)&&action(UiFriendsAction::search),"friends search: returns to retained All query");
    const auto visible_target=application_friends_search_target();
    check(action(UiFriendsAction::more,101)&&g_app.popup_scene.active&&g_app.popup_owner_kind==PopupOwnerKind::friends&&
        !text_input_ready(visible_target),"friends search: real More popup owns routing and excludes search input");dismiss_popup();
    check(action(UiFriendsAction::search),"friends search: explicit focus restores target after popup dismissal");
    HWND window=CreateWindowExW(0,L"STATIC",L"MaiCord Friends search capture fixture",WS_POPUP,400,200,1280,700,
        nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    check(window!=nullptr,"friends search: owns an invisible native mouse-capture fixture");
    const auto retained=application_friends_search_target();void* history=all->history;
    if (window) {
        g_app.window = window;
        const auto pointer_tab = [&](UiFriendsTab value, UINT press) noexcept {
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
                const auto point = MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
                return application_friends_window_message(press, MK_LBUTTON, point) &&
                       GetCapture() == window &&
                       application_friends_window_message(WM_LBUTTONUP, 0, point) && GetCapture() != window;
            }
            return false;
        };
        check(
            pointer_tab(UiFriendsTab::online, WM_LBUTTONDOWN) && scene()->tab == UiFriendsTab::online &&
                pointer_tab(UiFriendsTab::all, WM_LBUTTONDBLCLK) && scene()->tab == UiFriendsTab::all,
            "friends host: native rapid tab presses activate both releases without swallowing second down");
        g_app.window = window;
        const auto field = ui_friends_scene_search_geometry(*scene()).field;
        const auto location = MAKELPARAM(static_cast<short>((field.left + field.right) * .5f),
                                         static_cast<short>((field.top + field.bottom) * .5f));
        check(application_friends_window_message(WM_LBUTTONDOWN,MK_LBUTTON,location)&&GetCapture()==window&&
            application_friends_window_message(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(0,0)),
            "friends search: captured selection drag continues outside center viewport");
        application_friends_hide();
        check(GetCapture()!=window&&!text_input_ready(retained)&&
            !application_friends_window_message(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(0,0)),
            "friends search: hide releases owned mouse capture and invalidates drag and input target");
        g_app.window=nullptr;DestroyWindow(window);
    } else
        application_friends_hide();
    check(application_friends_show()&&all==application_friends_search_model()&&all->history==history&&
        utf8_equal(message_composer_text(*all),utf8_literal("needle"))&&action(UiFriendsAction::search)&&
        action(UiFriendsAction::clear_search)&&!all->bytes&&scene()->source_count==3,
        "friends search: hide retains bounded document/history and semantic Clear restores all rows");
    const auto costs=message_composer_history_costs(*all);
    check(costs.payload_bytes==2048&&costs.used_bytes<=costs.payload_bytes&&costs.group_bytes<=kFriendsSearchBytes,
        "friends search: history payload stays bounded independently of borrowed text storage");
    constexpr UiFriendsTab query_tabs[]={UiFriendsTab::online,UiFriendsTab::pending,UiFriendsTab::blocked};
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
        g_app.popup_scene.item_count==4&&g_app.popup_scene.items[1].action==0xF100u+static_cast<u32>(UiFriendsAction::remove)&&
        g_app.popup_scene.items[3].action==0xF100u+static_cast<u32>(UiFriendsAction::ignore)&&
        (g_app.popup_scene.items[1].flags&popup_item_danger)&&seen.submissions==submitted&&seen.form_opens==opened,
        "friends confirmation: production More popup owns identity and exposes destructive actions without mutating");
    check(remove_relationship(utf8_literal(R"({"id":"70"})"),201,false)&&
        application_friends_popup_action(0xF100u+static_cast<u32>(UiFriendsAction::remove))&&!g_app.popup_scene.active&&
        g_app.popup_owner_kind==PopupOwnerKind::none&&seen.form_opens==opened&&seen.submissions==submitted&&application_friends_synchronize(),
        "friends confirmation: stale popup row is dismissed before confirmation or mutation");
    check(snapshot(utf8_literal(R"([{"id":"70","type":1,"user":{"id":"70","username":"friend"}}])"),202)&&
        application_friends_synchronize()&&action(UiFriendsAction::remove,70),"friends confirmation: Remove opens first owner-validated modal");
    const auto old=seen.form;cancel_form();
    check(action(UiFriendsAction::block,70)&&!old.valid(old.owner)&&!old.save(old.owner,{})&&
        seen.form.owner.collection!=old.owner.collection,
        "friends confirmation: new modal serial rejects an older approval for the same user");
    const auto block=seen.form;const auto profile=g_app.account.status.profile_id;++g_app.account.status.profile_id;
    check(!block.valid(block.owner)&&!block.save(block.owner,{}),"friends confirmation: account switch rejects retained approval key");
    g_app.account.status.profile_id=profile;
    check(snapshot(utf8_literal(R"([{"id":"70","type":2}])"),203)&&application_friends_synchronize()&&
        !block.valid(block.owner)&&!block.save(block.owner,{}),"friends confirmation: changed relationship type rejects retained approval");cancel_form();
    check(tab(UiFriendsTab::blocked)&&action(UiFriendsAction::profile,70,UiFriendsTab::blocked),
        "friends confirmation: blocked More retains legal Profile without inline secondary widget");
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
            modal_bank.address==modal_retained.address&&modal_bank.reserved_bytes==modal_retained.reserved_bytes&&modal_bank.committed_bytes==modal_retained.committed_bytes&&
            modal_bank.admitted_count==modal_retained.admitted_count&&modal_bank.commit_count==modal_retained.commit_count,
            "friends storage: 8 idle modal synchronizations retain optional bank commitment and admitted high-water prefix");
        UiFriendsRow first{},second{},third{};
        check(scene()->source->resolve(scene()->source->context,0,&first)&&scene()->source->resolve(scene()->source->context,1,&second)&&
            scene()->source->resolve(scene()->source->context,2,&third)&&first.ignored&&second.ignored&&third.spam&&!third.ignored&&
            first.user_id!=second.user_id&&first.user_id!=third.user_id&&second.user_id!=third.user_id,
            "friends Pending: Other requests source orders ignored first and represents spam/ignored overlap once");
        check(application_friends_window_message(WM_LBUTTONDOWN,0,MAKELPARAM(100,300))&&
            application_friends_window_message(WM_LBUTTONUP,0,MAKELPARAM(100,300))&&application_friends_requests_active()&&
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
        utf8_equal({seen.target,seen.target_bytes},utf8_literal("/api/v10/users/@me/relationships?relationship_type=3")),
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
        seen.method==RestMethod::put&&!seen.body_bytes&&utf8_equal({seen.target,seen.target_bytes},utf8_literal("/api/v10/users/@me/relationships/801/ignore"))&&response(330,204),
        "friends Ignore: More Ignore requires native consent and submits bodyless PUT ignore rather than relationship DELETE");
    UiFriendsRow value{};
    check(row(801,&value)&&value.kind==UiFriendsKind::friend_user&&value.ignored&&scene()->source_count==1,
        "friends Ignore: successful Ignore preserves friendship and updates its authoritative ignored flag");
    forms=seen.form_opens;
    check(action(UiFriendsAction::unignore,801)&&seen.form_opens==forms&&application_friends_submit(331,5300)&&
        seen.operation==RelationshipOperation::unignore&&seen.method==RestMethod::delete_method&&!seen.body_bytes&&
        utf8_equal({seen.target,seen.target_bytes},utf8_literal("/api/v10/users/@me/relationships/801/ignore"))&&response(331,204)&&
        row(801,&value)&&value.kind==UiFriendsKind::friend_user&&!value.ignored,
        "friends Ignore: Unignore submits DELETE ignore and preserves the friend without redundant confirmation");
    forms=seen.form_opens;
    check(tab(UiFriendsTab::pending)&&scene()->source_count==3&&!scene()->sections->hidden_count&&
        action(UiFriendsAction::decline,815,UiFriendsTab::pending)&&seen.form_opens==forms&&application_friends_submit(332,5400)&&
        seen.operation==RelationshipOperation::remove&&seen.method==RestMethod::delete_method&&!seen.body_bytes&&
        utf8_equal({seen.target,seen.target_bytes},utf8_literal("/api/v10/users/@me/relationships/815"))&&response(332,204)&&!row(815,&value),
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
    void* controller = g_app.friends;
    const auto arena = scene()->storage.arena;
    const auto widgets = scene()->widgets.storage;
    const auto* source = scene()->source;
    const u32 revision = scene()->revision, source_revision = source->revision, builds = seen.builds;
    MEMORY_BASIC_INFORMATION controller_vm{}, scene_vm{};
    const auto storage=application_friends_storage_metrics();
    check(storage.controller_bytes<128*1024&&storage.active.admitted_count>=records&&storage.candidate.admitted_count>=records&&
        storage.indices.admitted_count>=records&&storage.active.committed_bytes<=storage.active.reserved_bytes&&
        storage.candidate.committed_bytes<=storage.candidate.reserved_bytes&&storage.indices.committed_bytes<=storage.indices.reserved_bytes,
        "friends benchmark: small controller owns three sparsely committed banks with admitted snapshot prefixes");
    const bool queried = VirtualQuery(controller, &controller_vm, sizeof(controller_vm)) == sizeof(controller_vm) &&
        VirtualQuery(arena.base, &scene_vm, sizeof(scene_vm)) == sizeof(scene_vm);
    check(queried && controller_vm.State == MEM_COMMIT && scene_vm.State == MEM_COMMIT &&
        controller_vm.AllocationBase == controller && scene_vm.AllocationBase == arena.base,
        "friends benchmark: actual controller and retained scene allocations have committed VM regions");
    bool retained = true;
    for (u32 i = 0; i < 8; ++i) retained = application_friends_synchronize() && retained;
    const auto retained_storage=application_friends_storage_metrics();
    const auto same_bank=[](const FriendsStorageBankMetrics& a,const FriendsStorageBankMetrics& b) noexcept {
        return a.address==b.address&&a.reserved_bytes==b.reserved_bytes&&a.committed_bytes==b.committed_bytes&&a.admitted_count==b.admitted_count&&a.commit_count==b.commit_count;};
    check(storage.controller_bytes==retained_storage.controller_bytes&&same_bank(storage.active,retained_storage.active)&&
        same_bank(storage.candidate,retained_storage.candidate)&&same_bank(storage.indices,retained_storage.indices),
        "friends benchmark: 8 unchanged synchronizations retain all three bank admission and commit high-water metrics");
    MEMORY_BASIC_INFORMATION controller_after{}, scene_after{};
    retained = retained && VirtualQuery(controller, &controller_after, sizeof(controller_after)) == sizeof(controller_after) &&
        VirtualQuery(arena.base, &scene_after, sizeof(scene_after)) == sizeof(scene_after);
    check(retained && g_app.friends == controller && scene()->revision == revision && seen.builds == builds &&
        scene()->source == source && source->revision == source_revision && scene()->source_count == records &&
        scene()->widgets.storage.data == widgets.data && scene()->widgets.storage.count == widgets.count &&
        scene()->storage.arena.base == arena.base &&
        scene()->storage.arena.committed == arena.committed && scene()->storage.arena.offset == arena.offset &&
        scene()->storage.arena.commit_count == arena.commit_count &&
        controller_after.State == MEM_COMMIT && controller_after.AllocationBase == controller_vm.AllocationBase &&
        controller_after.BaseAddress == controller_vm.BaseAddress && controller_after.RegionSize == controller_vm.RegionSize &&
        scene_after.State == MEM_COMMIT && scene_after.AllocationBase == scene_vm.AllocationBase &&
        scene_after.BaseAddress == scene_vm.BaseAddress && scene_after.RegionSize == scene_vm.RegionSize,
        "friends benchmark: 8 unchanged synchronizations retain revisions, text publication and exact committed storage");
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
    g_app.account.instance = &account;g_app.account.status.profile_id = 4;g_app.account.api.relationships = submit;
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
    void* widgets = scene()->storage.arena.base;
    const u32 revision = scene()->revision, builds = seen.builds;
    check(application_friends_synchronize() && scene()->revision == revision && seen.builds == builds,
        "friends: idle synchronization retains scene revision and text publication");
    context_menu_coordinates();
    seen.settings = true;
    check(!synthetic_input(UiInputKind::character, 'x', false) &&
        !application_friends_window_message(WM_CONTEXTMENU, 0, MAKELPARAM(600, 150)) &&
        application_friends_synchronize() && seen.builds == builds && !application_friends_show(),
        "friends: opaque settings barrier suppresses Friends input and hidden text rebuild");
    seen.settings = false;
    application_friends_hide();
    check(!application_friends_active() && !scene() && freed(widgets) && !application_friends_pending(1000),
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
    check(small_storage.active.committed_bytes<=storage_system.dwPageSize&&small_storage.candidate.committed_bytes<=storage_system.dwPageSize&&
        small_storage.indices.committed_bytes<=storage_system.dwPageSize&&small_storage.active.admitted_count>=2&&
        small_storage.candidate.admitted_count>=2&&small_storage.indices.admitted_count>=1,
        "friends storage: two relationships commit at most one native page per primary bank");
    const u32 modifier_submissions = seen.submissions, modifier_forms = seen.form_opens;
    const auto modifier_tab = scene()->tab;
    check(ui_friends_scene_focus(scene(), {UiFriendsAction::select_tab, 0, UiFriendsTab::pending}) &&
        !synthetic_input(UiInputKind::key_down, VK_RETURN, false, synthetic_control) &&
        !synthetic_input(UiInputKind::key_up, VK_RETURN, false, synthetic_control) &&
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
    check(snapshot(utf8_literal(R"([{"id":"10","type":1},{"id":"20","type":2}])"), 3) &&
        application_friends_synchronize() && response(4, 204) && tab(UiFriendsTab::blocked) &&
        row(20, &before) && before.kind == UiFriendsKind::blocked,
        "friends: newer Gateway membership epoch survives late successful mutation completion");
    check(action(UiFriendsAction::unblock, 20, UiFriendsTab::blocked) && application_friends_submit(5, 1700) &&
        response(5, 429, {}, 1800, {0, false, true}) &&
        !application_friends_pending(100000) && !application_friends_next_wake() && scene()->phase == UiFriendsPhase::error,
        "friends: CAPTCHA rejection becomes explicit error without automatic rate-limit retry");
    check(action(UiFriendsAction::retry, 0, UiFriendsTab::blocked) && application_friends_submit(6, 1800) &&
        response(6, 429, {}, 1900, {40002, true, false}) && !application_friends_pending(100000) &&
        !application_friends_next_wake(),
        "friends: account verification code alone also suppresses automatic 429 retry");
    check(action(UiFriendsAction::retry, 0, UiFriendsTab::blocked) && application_friends_submit(60, 1900) &&
        response(60, 204) && tab(UiFriendsTab::all) && scene()->source_count == 1,
        "friends: explicit Retry resubmits preserved operation and successful removal updates authoritative rows");
    check(action(UiFriendsAction::profile, 10) && seen.profile_user == 10 &&
        action(UiFriendsAction::open_dm, 10) && seen.dm_user == 10 && !application_friends_active() && !scene() &&
        application_friends_show() && !application_friends_pending(2000),
        "friends: profile and DM semantic actions route correct identity, hide chat surface and reopen without GET");
    const auto before_add_forms=seen.form_opens;
    check(tab(UiFriendsTab::add_friend) && action(UiFriendsAction::add_friend, 0, UiFriendsTab::add_friend) && seen.form_opens == before_add_forms+1 &&
        seen.form.valid && seen.form.save && seen.form.valid(seen.form.owner),
        "friends: Add friend opens bounded owner-validated text form seam");
    char username[] = "friend.name";
    check(save_form({username, sizeof(username) - 1}),
        "friends: validated text form queues copied username");
    for (u32 i = 0; i < sizeof(username) - 1; ++i) username[i] = 'x';
    check(application_friends_submit(7, 2000) && seen.operation == RelationshipOperation::request &&
        utf8_equal({seen.body, seen.body_bytes}, utf8_literal("{\"username\":\"friend.name\",\"discriminator\":null}")) && response(7, 400) &&
        !application_friends_pending(100000), "friends: username is copied through controller/helper and rejection never invents outgoing row");
    check(action(UiFriendsAction::retry, 0, UiFriendsTab::add_friend) && application_friends_submit(8, 2100) &&
        utf8_equal({seen.body, seen.body_bytes}, utf8_literal("{\"username\":\"friend.name\",\"discriminator\":null}")) && response(8, 204),
        "friends: explicit username Retry preserves payload when request and retry storage alias");
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
    const bool copied_action = copied_tab && action(UiFriendsAction::add_friend, 0, UiFriendsTab::add_friend);
    const bool copied_save = copied_action && save_form(utf8_literal("pending.friend"));
    const bool copied_submit = copied_save && application_friends_submit(15, 2800);
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
    stale_classification_runtime_fixture();stale_bulk_retry_runtime_fixture();
    bounded_performance_fixture();
    void* owner = g_app.friends;
    const auto* requests_bank_address=application_friends_storage_metrics().requests_indices.address;
    application_friends_release();
    check(!g_app.friends && !scene() && !application_friends_pending(100000) && freed(owner) &&
        seen.form.valid && !seen.form.valid(seen.form.owner),
        "friends: final release unbinds renderer, invalidates retained form owner and frees controller VM");
    const auto released_requests=application_friends_storage_metrics().requests_indices;
    check(!released_requests.address&&!released_requests.reserved_bytes&&!released_requests.committed_bytes&&!released_requests.admitted_count&&!released_requests.commit_count&&
        freed(const_cast<void*>(requests_bank_address)),
        "friends storage: final controller release clears all optional hidden index bank metrics");
}
} // namespace maicord::friends_runtime_test

namespace maicord {
bool text_edit_layout_hit_test(const TextEditLayoutOwner&,float,float,u32*) noexcept {return false;}
ClipboardTextResult clipboard_read_utf8(HWND,Slice<char> output) noexcept {
    const auto& state=friends_runtime_test::seen;
    if(!output.data&&output.count)return {0,ClipboardTextError::invalid_input};
    if(state.clipboard_bytes>output.count)return {0,ClipboardTextError::capacity};
    for(u32 i=0;i<state.clipboard_bytes;++i)output[i]=state.clipboard[i];
    return {state.clipboard_bytes,ClipboardTextError::none};
}
bool clipboard_write_utf8(HWND,Utf8View text) noexcept {
    auto& state=friends_runtime_test::seen;
    if(text.bytes>sizeof(state.clipboard)||!utf8_validate(text).valid)return false;
    for(u32 i=0;i<text.bytes;++i)state.clipboard[i]=text.data[i];state.clipboard_bytes=text.bytes;return true;
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
bool application_settings_active() noexcept { return friends_runtime_test::seen.settings; }
bool synchronize_navigation_view() noexcept { return true; }
bool synchronize_selected_channel() noexcept { return true; }
bool synchronize_shell_layout() noexcept {
    return ui_scene_layout_with_metrics(&g_app.shell_scene, application_friends_active() ? UiShellView::friends : UiShellView::private_channels,
        application_friends_active() ? kFriendsShellLayoutMetrics : kGuildChannelShellLayoutMetrics, 1280, 700);
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
bool application_text_form_open(const ApplicationTextFormRequest& request) noexcept {
    ++friends_runtime_test::seen.form_opens;friends_runtime_test::seen.form = request;
    g_app.text_form=reinterpret_cast<ApplicationTextForm*>(&friends_runtime_test::modal_token);return true;
}
void application_text_form_cancel_owner(TextFormOwnerValid validator) noexcept {
    if(friends_runtime_test::seen.form.valid==validator)g_app.text_form=nullptr;
}
float client_dip_x(LPARAM value) noexcept { return static_cast<short>(LOWORD(value)); }
float client_dip_y(LPARAM value) noexcept { return static_cast<short>(HIWORD(value)); }
} // namespace app_detail
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_friends_runtime_entry() noexcept {
    maicord::friends_runtime_test::run();
    maicord::test_exit(maicord::friends_runtime_test::seen.failures, "Friends controller runtime");
}
