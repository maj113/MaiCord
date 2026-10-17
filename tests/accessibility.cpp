#include "maicord/core/utf.hpp"
#include "maicord/platform/uia_provider.hpp"
#include "maicord/ui/accessibility.hpp"
#include "maicord/ui/accessibility_scenes.hpp"
#include "maicord/ui/call_scene.hpp"
#include "maicord/ui/forum_post_scene.hpp"
#include "maicord/ui/image_viewer.hpp"
#include "maicord/ui/inbox_scene.hpp"
#include "maicord/ui/media_picker.hpp"
#include "maicord/ui/pinned_messages_scene.hpp"
#include "maicord/ui/quick_switcher_scene.hpp"
#include "maicord/ui/search_scene.hpp"
#include "maicord/ui/text_form.hpp"
#include "maicord/ui/thread_browser_scene.hpp"
#include "maicord/ui/widget_builder.hpp"
#include "test_output.hpp"

#include <ole2.h>
#include <UIAutomation.h>

namespace maicord::accessibility_test {
namespace {

u32 failures;

void check(bool valid, const char* name) noexcept {
    if (valid)
        return;
    ++failures;
    test_write_text("FAIL: ");
    test_write_line(name);
}

bool equal(Utf8View text, Utf8View expected) noexcept {
    return utf8_equal(text, expected);
}

bool wide_equal(const wchar_t* text, const wchar_t* expected) noexcept {
    if (!text || !expected)
        return text == expected;
    u32 index = 0;
    for (; text[index] && expected[index]; ++index)
        if (text[index] != expected[index])
            return false;
    return text[index] == expected[index];
}

constexpr u8 kSurface = 3;

// ---- synthetic scene -----------------------------------------------------

struct Scene {
    UiWidget widgets[32];
    UiLayoutNode layout[32];
    UiWidgetManager manager;
    UiWidgetHandle root, list, rows[3], labels[3], icon, settings, label, field, hidden, footer, disabled;
};

UiLayoutStyle fixed(float width, float height, UiLayoutFlow flow = UiLayoutFlow::overlay) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fixed(width);
    style.height = ui_layout_fixed(height);
    style.flow = flow;
    return style;
}

bool build_scene(Scene* scene) noexcept {
    if (!ui_widget_manager_initialize(&scene->manager, {scene->widgets, 32}, {scene->layout, 32}))
        return false;
    UiWidgetBuilder build(scene->manager);
    scene->root = build.container(ui_widget_invalid_handle(), {1, utf8_literal("Root")},
                                  fixed(400, 600, UiLayoutFlow::vertical)).handle;
    scene->list = build.widget(scene->root, {2, utf8_literal("ChannelNavigation")}, UiWidgetKind::scroll_view,
                               ui_widget_none, fixed(200, 50, UiLayoutFlow::vertical)).handle;
    for (u32 index = 0; index < 3; ++index) {
        scene->rows[index] =
            build.button(scene->list, {10 + index, utf8_literal("ChannelRow")}, fixed(200, 30, UiLayoutFlow::horizontal))
                .handle;
        if (index == 0)
            scene->icon = build.widget(scene->rows[0], {20, utf8_literal("ChannelIcon")}, UiWidgetKind::image,
                                       ui_widget_none, fixed(16, 16)).handle;
        scene->labels[index] = build.text(scene->rows[index], {30 + index, utf8_literal("ChannelLabel")},
                                          fixed(100, 16), {100, 16}, 12).handle;
    }
    scene->settings = build.button(scene->root, {40, utf8_literal("UserSettings")}, fixed(100, 20)).handle;
    scene->label = build.text(scene->root, {41, utf8_literal("LoginLabel")}, fixed(100, 16), {100, 16}, 12).handle;
    scene->field = build.text_field(scene->root, {42, utf8_literal("LoginField")}, fixed(200, 24)).handle;
    UiLayoutStyle hidden = fixed(50, 20);
    hidden.flags = ui_layout_node_none;
    scene->hidden = build.button(scene->root, {43, utf8_literal("Hidden")}, hidden).handle;
    scene->footer = build.container(scene->root, {44, utf8_literal("Footer")}, fixed(200, 20)).handle;
    scene->disabled = build.widget(scene->root, {45, utf8_literal("Submit")}, UiWidgetKind::button,
                                   ui_widget_hit_test, fixed(80, 20)).handle;
    return build.ok() && ui_widget_manager_layout(&scene->manager, {400, 600}).ok() &&
           ui_widget_manager_focus(&scene->manager, scene->field);
}

bool handle_equal(UiWidgetHandle a, UiWidgetHandle b) noexcept {
    return a.slot == b.slot && a.generation == b.generation;
}

void describe(void* context, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&, const UiLayoutNode&,
              UiAccessWidgetInfo* info) noexcept {
    const auto& scene = *static_cast<const Scene*>(context);
    if (handle_equal(handle, scene.list)) {
        info->role = UiAccessRole::list;
        info->name = utf8_literal("Channels");
        info->actions = ui_access_action_scroll;
        info->scroll = {25.0f, 50.0f};
        return;
    }
    for (u32 index = 0; index < 3; ++index)
        if (handle_equal(handle, scene.rows[index])) {
            info->role = UiAccessRole::list_item;
            info->key = 100 + index;
            info->item = 100 + index;
            info->state |= ui_access_selectable | (index == 1 ? ui_access_selected : 0);
            info->actions = ui_access_action_select | ui_access_action_invoke;
            return;
        }
    if (handle_equal(handle, scene.field))
        info->value = utf8_literal("rob");
}

bool build_tree(UiAccessTree* tree, const Scene& scene, bool with_row0 = true) noexcept {
    if (!ui_access_tree_reset(tree, utf8_literal("Synthetic"), {0, 0, 400, 600}))
        return false;
    const UiAccessScope scope{describe, const_cast<Scene*>(&scene), kSurface, true};
    if (ui_access_add_widgets(tree, 0, scene.manager, scope) == kUiAccessNone)
        return false;
    const char* labels[]{"general", "random", "off-screen"};
    for (u32 index = with_row0 ? 0 : 1; index < 3; ++index) {
        u32 bytes = 0;
        while (labels[index][bytes])
            ++bytes;
        ui_access_attribute_text(tree, {&scene.manager, scene.labels[index]}, {labels[index], bytes}, {});
    }
    ui_access_attribute_text(tree, {&scene.manager, scene.label}, utf8_literal("Email"), {});
    const UiLayoutNode* footer = ui_widget_manager_layout_node(scene.manager, scene.footer);
    ui_access_attribute_text(tree, {&scene.manager, scene.footer}, utf8_literal("Online \xEF\xBF\xBC  now"),
                             footer ? footer->rectangle : UiRect{});
    // Host-built nodes beside the widget projection.
    UiAccessNodeDesc toggle{};
    toggle.key = ui_access_key(kSurface, 500);
    toggle.rectangle = {300, 0, 340, 20};
    toggle.role = UiAccessRole::toggle_button;
    toggle.name = utf8_literal("Member list");
    toggle.state = static_cast<u16>(ui_access_checked | ui_access_toggleable);
    toggle.actions = ui_access_action_toggle;
    UiAccessNodeDesc slider{};
    slider.key = ui_access_key(kSurface, 501);
    slider.rectangle = {300, 30, 380, 50};
    slider.role = UiAccessRole::slider;
    slider.name = utf8_literal("Volume");
    slider.range = {2, 0, 4, 1};
    slider.actions = ui_access_action_range;
    UiAccessNodeDesc folder{};
    folder.key = ui_access_key(kSurface, 502);
    folder.rectangle = {300, 60, 380, 80};
    folder.role = UiAccessRole::list_item;
    folder.name = utf8_literal("Server folder");
    folder.state = ui_access_collapsed;
    folder.actions = ui_access_action_expand;
    return ui_access_add(tree, 0, toggle) != kUiAccessNone && ui_access_add(tree, 0, slider) != kUiAccessNone &&
           ui_access_add(tree, 0, folder) != kUiAccessNone && ui_access_tree_finish(tree);
}

UiAccessNode g_nodes[256];
char g_text[16384];
u32 g_slots[512];
u32 g_keys[512];

u32 find_widget(const UiAccessTree& tree, u64 identity) noexcept {
    return ui_access_find(tree, ui_access_key(kSurface, identity));
}

u64 widget_identity(u32 id) noexcept {
    return static_cast<u64>(id) | (1ull << 63);
}

void test_tree(const Scene& scene, UiAccessTree* tree) noexcept {
    check(build_tree(tree, scene), "synthetic scene projects and finishes");
    const u32 list = find_widget(*tree, widget_identity(2));
    const u32 row0 = find_widget(*tree, 100), row1 = find_widget(*tree, 101), row2 = find_widget(*tree, 102);
    const u32 settings = find_widget(*tree, widget_identity(40));
    const u32 label = find_widget(*tree, widget_identity(41));
    const u32 field = find_widget(*tree, widget_identity(42));
    const u32 disabled = find_widget(*tree, widget_identity(45));
    check(list != kUiAccessNone && tree->nodes[list].role == UiAccessRole::list &&
              equal(ui_access_name(*tree, list), utf8_literal("Channels")),
          "describer refines a scroll view into a named list");
    check(row0 != kUiAccessNone && row1 != kUiAccessNone && row2 != kUiAccessNone &&
              tree->nodes[row0].parent == list && tree->nodes[list].first_child == row0 &&
              tree->nodes[row0].next_sibling == row1 && tree->nodes[row1].next_sibling == row2 &&
              tree->nodes[row2].previous_sibling == row1 && tree->nodes[list].child_count == 3,
          "list items follow widget order with parent and sibling links");
    check(row0 != kUiAccessNone && equal(ui_access_name(*tree, row0), utf8_literal("general")) &&
              equal(ui_access_name(*tree, row1), utf8_literal("random")) &&
              tree->nodes[row0].first_child == kUiAccessNone,
          "list items are named by their drawn text; icon and label children are presentational");
    check(row1 != kUiAccessNone && (tree->nodes[row1].state & ui_access_selected) &&
              !(tree->nodes[row0].state & ui_access_selected) && tree->nodes[row0].item == 100,
          "selection state and routing identity survive projection");
    check(row2 != kUiAccessNone && (tree->nodes[row2].state & ui_access_offscreen) &&
              !(tree->nodes[row0].state & ui_access_offscreen) && tree->nodes[row1].visible.bottom == 50.0f &&
              tree->nodes[row1].rectangle.bottom == 60.0f,
          "clipping marks hidden rows offscreen and clips partial rows");
    check(row0 != kUiAccessNone && tree->nodes[row0].rectangle.left == 0.0f &&
              tree->nodes[row0].rectangle.right == 200.0f && tree->nodes[row0].rectangle.bottom == 30.0f,
          "bounds come from layout");
    check(settings != kUiAccessNone && tree->nodes[settings].role == UiAccessRole::button &&
              equal(ui_access_name(*tree, settings), utf8_literal("User settings")) &&
              (tree->nodes[settings].actions & ui_access_action_invoke) &&
              (tree->nodes[settings].state & ui_access_focusable),
          "an undescribed button is accessible by default with a humanized name");
    check(field != kUiAccessNone && tree->nodes[field].role == UiAccessRole::edit &&
              equal(ui_access_name(*tree, field), utf8_literal("Email")) &&
              equal(ui_access_value(*tree, field), utf8_literal("rob")) && tree->focus == field &&
              (tree->nodes[field].state & ui_access_focused),
          "an edit takes its preceding label as name, keeps its value and the manager focus");
    check(label != kUiAccessNone && tree->nodes[label].role == UiAccessRole::text,
          "labels stay readable static text");
    check(disabled != kUiAccessNone && (tree->nodes[disabled].state & ui_access_disabled) &&
              equal(ui_access_name(*tree, disabled), utf8_literal("Submit")),
          "a button without input enablement is disabled");
    check(find_widget(*tree, widget_identity(43)) == kUiAccessNone, "hidden widgets are not projected");
    check(find_widget(*tree, widget_identity(44)) == kUiAccessNone && find_widget(*tree, widget_identity(1)) == kUiAccessNone,
          "anonymous containers flatten into their parent");
    u32 online = kUiAccessNone;
    for (u32 child = tree->nodes[0].first_child; child != kUiAccessNone; child = tree->nodes[child].next_sibling)
        if (equal(ui_access_name(*tree, child), utf8_literal("Online now")))
            online = child;
    check(online != kUiAccessNone && tree->nodes[online].role == UiAccessRole::text &&
              tree->nodes[online].next_sibling == disabled,
          "container text becomes normalized static text in reading order");
    check(equal(ui_access_automation_id(*tree, row0), utf8_literal("ChannelRow")), "automation id is the debug name");
    check(ui_access_hit_test(*tree, 100, 15) == row0 && ui_access_hit_test(*tree, 100, 45) == row1 &&
              ui_access_hit_test(*tree, 390, 590) == 0,
          "hit testing finds the deepest visible node");
    for (u32 index = 1; index < tree->count; ++index)
        if (tree->nodes[index].parent >= index) {
            check(false, "parents precede children after compaction");
            break;
        }
    Scene again{};
    const u64 row0_key = row0 != kUiAccessNone ? tree->nodes[row0].key : 0;
    check(build_scene(&again) && build_tree(tree, again) && ui_access_find(*tree, row0_key) != kUiAccessNone,
          "keys are stable across rebuilds of the same scene");
    char humanized[64];
    const u32 a = ui_access_humanize(utf8_literal("CreateDirectMessage"), {humanized, 64});
    check(equal({humanized, a}, utf8_literal("Create direct message")), "humanize camel case");
    const u32 b = ui_access_humanize(utf8_literal("GIFs"), {humanized, 64});
    check(equal({humanized, b}, utf8_literal("GIFs")), "humanize keeps plural acronyms");
    const u32 c = ui_access_humanize(utf8_literal("HTTPServer_Status"), {humanized, 64});
    check(equal({humanized, c}, utf8_literal("HTTP server status")), "humanize splits acronym runs");
}

// ---- provider (direct COM calls through the SDK's C++ interfaces) ---------

struct ActRecord {
    u64 key;
    UiAccessAction action;
    double number;
    char text[32];
    u32 text_bytes;
    u32 calls;
    DWORD thread;
};

struct HostContext {
    UiAccessTree* tree;
    ActRecord act;
    DWORD tree_thread;
    u32 tree_calls;
};

const UiAccessTree* host_tree(void* context) noexcept {
    auto* host = static_cast<HostContext*>(context);
    host->tree_thread = GetCurrentThreadId();
    ++host->tree_calls;
    return host->tree;
}

bool host_act(void* context, u64 key, UiAccessAction action, double number, Utf8View text) noexcept {
    auto& record = static_cast<HostContext*>(context)->act;
    record.key = key;
    record.action = action;
    record.number = number;
    record.text_bytes = minimum<u32>(text.bytes, 32);
    copy_bytes(record.text, text.data, record.text_bytes);
    record.thread = GetCurrentThreadId();
    ++record.calls;
    return true;
}

float host_scale(void*) noexcept {
    return 1.0f;
}

IRawElementProviderSimple* element(UiaProviderHost* host, u64 key) noexcept {
    return static_cast<IRawElementProviderSimple*>(uia_host_element(host, key));
}

template <typename T> T* pattern(IRawElementProviderSimple* simple, PATTERNID id) noexcept {
    IUnknown* unknown = nullptr;
    if (!simple || FAILED(simple->GetPatternProvider(id, &unknown)) || !unknown)
        return nullptr;
    T* result = nullptr;
    unknown->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&result));
    unknown->Release();
    return result;
}

bool name_is(IRawElementProviderSimple* simple, const wchar_t* expected) noexcept {
    VARIANT value{};
    if (!simple || FAILED(simple->GetPropertyValue(UIA_NamePropertyId, &value)) || value.vt != VT_BSTR)
        return false;
    const bool matched = wide_equal(value.bstrVal, expected);
    VariantClear(&value);
    return matched;
}

int int_property(IRawElementProviderSimple* simple, PROPERTYID id) noexcept {
    VARIANT value{};
    if (!simple || FAILED(simple->GetPropertyValue(id, &value)))
        return -1;
    const int result = value.vt == VT_I4 ? value.lVal : value.vt == VT_BOOL ? (value.boolVal ? 1 : 0) : -1;
    VariantClear(&value);
    return result;
}

IRawElementProviderSimple* simple_of(IRawElementProviderFragment* fragment) noexcept {
    IRawElementProviderSimple* simple = nullptr;
    if (fragment)
        fragment->QueryInterface(__uuidof(IRawElementProviderSimple), reinterpret_cast<void**>(&simple));
    return simple;
}

void test_provider(const Scene& scene, UiAccessTree* tree) noexcept {
    check(build_tree(tree, scene), "provider snapshot");
    HostContext context{tree, {}, 0, 0};
    UiaProviderHost host{};
    check(uia_host_initialize(&host, nullptr, {&context, host_tree, host_act, host_scale}) && !host.runtime &&
              !uia_host_listening(host),
          "provider host starts without loading UI Automation");
    check(uia_host_load(&host) && host.runtime, "provider loads UIAutomationCore and OleAut32 on demand");

    IRawElementProviderSimple* root = element(&host, 0);
    IRawElementProviderFragmentRoot* fragment_root = nullptr;
    check(root && SUCCEEDED(root->QueryInterface(__uuidof(IRawElementProviderFragmentRoot),
                                                 reinterpret_cast<void**>(&fragment_root))) &&
              fragment_root,
          "root exposes IRawElementProviderFragmentRoot");
    ProviderOptions options{};
    check(root && SUCCEEDED(root->get_ProviderOptions(&options)) && options == ProviderOptions_ServerSideProvider,
          "server-side provider without COM threading");
    check(name_is(root, L"Synthetic"), "root name");

    IRawElementProviderFragment* focus = nullptr;
    check(fragment_root && SUCCEEDED(fragment_root->GetFocus(&focus)) && focus, "root reports the focused element");
    IRawElementProviderSimple* focused = simple_of(focus);
    check(name_is(focused, L"Email") && int_property(focused, UIA_ControlTypePropertyId) == UIA_EditControlTypeId &&
              int_property(focused, UIA_HasKeyboardFocusPropertyId) == 1,
          "focused edit has name, control type and keyboard focus");
    IValueProvider* value = pattern<IValueProvider>(focused, UIA_ValuePatternId);
    BSTR text = nullptr;
    check(value && SUCCEEDED(value->get_Value(&text)) && wide_equal(text, L"rob"), "value pattern reads the edit");
    SysFreeString(text);
    check(value && SUCCEEDED(value->SetValue(L"hello")) && context.act.action == UiAccessAction::set_value &&
              equal({context.act.text, context.act.text_bytes}, utf8_literal("hello")),
          "value pattern forwards SetValue as UTF-8");
    if (value)
        value->Release();

    IRawElementProviderFragment* root_fragment = nullptr;
    IRawElementProviderFragment* first = nullptr;
    root->QueryInterface(__uuidof(IRawElementProviderFragment), reinterpret_cast<void**>(&root_fragment));
    check(root_fragment && SUCCEEDED(root_fragment->Navigate(NavigateDirection_FirstChild, &first)) && first,
          "root navigates to its first child");
    IRawElementProviderSimple* list = simple_of(first);
    check(name_is(list, L"Channels") && int_property(list, UIA_ControlTypePropertyId) == UIA_ListControlTypeId,
          "first child is the channel list");
    IRawElementProviderFragment* row0 = nullptr;
    IRawElementProviderFragment* row1 = nullptr;
    IRawElementProviderFragment* back = nullptr;
    check(first && SUCCEEDED(first->Navigate(NavigateDirection_FirstChild, &row0)) && row0 &&
              SUCCEEDED(row0->Navigate(NavigateDirection_NextSibling, &row1)) && row1 &&
              SUCCEEDED(row1->Navigate(NavigateDirection_Parent, &back)) && back,
          "fragment navigation walks children, siblings and parents");
    IRawElementProviderSimple* row0_simple = simple_of(row0);
    IRawElementProviderSimple* row1_simple = simple_of(row1);
    IRawElementProviderSimple* back_simple = simple_of(back);
    check(name_is(row0_simple, L"general") && name_is(row1_simple, L"random") && name_is(back_simple, L"Channels") &&
              int_property(row0_simple, UIA_ControlTypePropertyId) == UIA_ListItemControlTypeId,
          "list items expose their names");
    UiaRect bounds{};
    check(row0 && SUCCEEDED(row0->get_BoundingRectangle(&bounds)) && bounds.width == 200.0 && bounds.height == 30.0,
          "bounding rectangle scales layout bounds");
    SAFEARRAY* runtime_id = nullptr;
    LONG upper = -1;
    int first_part = 0;
    LONG zero = 0;
    check(row0 && SUCCEEDED(row0->GetRuntimeId(&runtime_id)) && runtime_id &&
              SUCCEEDED(SafeArrayGetUBound(runtime_id, 1, &upper)) && upper == 2 &&
              SUCCEEDED(SafeArrayGetElement(runtime_id, &zero, &first_part)) && first_part == UiaAppendRuntimeId,
          "runtime ids append to the host window id");
    if (runtime_id)
        SafeArrayDestroy(runtime_id);

    ISelectionItemProvider* item0 = pattern<ISelectionItemProvider>(row0_simple, UIA_SelectionItemPatternId);
    ISelectionItemProvider* item1 = pattern<ISelectionItemProvider>(row1_simple, UIA_SelectionItemPatternId);
    BOOL selected0 = TRUE, selected1 = FALSE;
    check(item0 && item1 && SUCCEEDED(item0->get_IsSelected(&selected0)) && !selected0 &&
              SUCCEEDED(item1->get_IsSelected(&selected1)) && selected1,
          "selection items report selection");
    check(item0 && SUCCEEDED(item0->Select()) && context.act.action == UiAccessAction::select &&
              context.act.key == tree->nodes[find_widget(*tree, 100)].key,
          "Select routes to the item key");
    ISelectionProvider* selection = pattern<ISelectionProvider>(list, UIA_SelectionPatternId);
    SAFEARRAY* selected = nullptr;
    check(selection && SUCCEEDED(selection->GetSelection(&selected)) && selected &&
              SUCCEEDED(SafeArrayGetUBound(selected, 1, &upper)) && upper == 0,
          "list selection returns the selected item");
    if (selected)
        SafeArrayDestroy(selected);
    IScrollProvider* scroll = pattern<IScrollProvider>(list, UIA_ScrollPatternId);
    double percent = 0;
    BOOL scrollable = FALSE;
    check(scroll && SUCCEEDED(scroll->get_VerticalScrollPercent(&percent)) && percent == 25.0 &&
              SUCCEEDED(scroll->get_VerticallyScrollable(&scrollable)) && scrollable &&
              SUCCEEDED(scroll->Scroll(ScrollAmount_NoAmount, ScrollAmount_SmallIncrement)) &&
              context.act.action == UiAccessAction::scroll &&
              static_cast<UiAccessScroll>(static_cast<u32>(context.act.number)) == UiAccessScroll::small_increment,
          "scroll pattern reports position and forwards scrolling");

    IRawElementProviderSimple* settings = element(&host, ui_access_key(kSurface, widget_identity(40)));
    IInvokeProvider* invoke = pattern<IInvokeProvider>(settings, UIA_InvokePatternId);
    check(name_is(settings, L"User settings") && invoke && SUCCEEDED(invoke->Invoke()) &&
              context.act.action == UiAccessAction::invoke &&
              context.act.key == ui_access_key(kSurface, widget_identity(40)),
          "Invoke routes to the widget action");
    check(!pattern<IToggleProvider>(settings, UIA_TogglePatternId), "buttons do not claim the toggle pattern");
    IRawElementProviderSimple* disabled = element(&host, ui_access_key(kSurface, widget_identity(45)));
    IInvokeProvider* disabled_invoke = pattern<IInvokeProvider>(disabled, UIA_InvokePatternId);
    const u32 calls = context.act.calls;
    check(disabled_invoke && disabled_invoke->Invoke() == UIA_E_ELEMENTNOTENABLED && context.act.calls == calls &&
              int_property(disabled, UIA_IsEnabledPropertyId) == 0,
          "disabled elements refuse Invoke");

    IRawElementProviderSimple* toggle_element = element(&host, ui_access_key(kSurface, 500));
    IToggleProvider* toggle = pattern<IToggleProvider>(toggle_element, UIA_TogglePatternId);
    ToggleState state = ToggleState_Off;
    check(toggle && SUCCEEDED(toggle->get_ToggleState(&state)) && state == ToggleState_On &&
              SUCCEEDED(toggle->Toggle()) && context.act.action == UiAccessAction::toggle,
          "toggle pattern reports and forwards the switch state");
    IRawElementProviderSimple* slider_element = element(&host, ui_access_key(kSurface, 501));
    IRangeValueProvider* range = pattern<IRangeValueProvider>(slider_element, UIA_RangeValuePatternId);
    double current = 0, maximum_value = 0;
    check(range && SUCCEEDED(range->get_Value(&current)) && current == 2.0 &&
              SUCCEEDED(range->get_Maximum(&maximum_value)) && maximum_value == 4.0 &&
              SUCCEEDED(range->SetValue(3.0)) && context.act.action == UiAccessAction::set_range &&
              context.act.number == 3.0 && range->SetValue(9.0) == E_INVALIDARG,
          "range value pattern reads, sets and validates");
    IRawElementProviderSimple* folder_element = element(&host, ui_access_key(kSurface, 502));
    IExpandCollapseProvider* expand = pattern<IExpandCollapseProvider>(folder_element, UIA_ExpandCollapsePatternId);
    ExpandCollapseState expand_state = ExpandCollapseState_Expanded;
    check(expand && SUCCEEDED(expand->get_ExpandCollapseState(&expand_state)) &&
              expand_state == ExpandCollapseState_Collapsed && SUCCEEDED(expand->Expand()) &&
              context.act.action == UiAccessAction::expand,
          "expand/collapse pattern reports and forwards");

    IRawElementProviderFragment* hit = nullptr;
    IRawElementProviderSimple* hit_simple = nullptr;
    check(fragment_root && SUCCEEDED(fragment_root->ElementProviderFromPoint(100, 15, &hit)) && hit &&
              name_is(hit_simple = simple_of(hit), L"general"),
          "element from point uses the snapshot hit test");

    const u64 removed_key = ui_access_key(kSurface, 999);
    IRawElementProviderSimple* stale = element(&host, removed_key);
    VARIANT stale_value{};
    check(stale && stale->GetPropertyValue(UIA_NamePropertyId, &stale_value) == UIA_E_ELEMENTNOTAVAILABLE,
          "elements for missing keys report ElementNotAvailable");

    IUnknown* release[]{root, fragment_root, focus, focused, root_fragment, first, list, row0, row1, back,
                        row0_simple, row1_simple, back_simple, item0, item1, selection, scroll, settings, invoke,
                        disabled, disabled_invoke, toggle_element, toggle, slider_element, range, folder_element,
                        expand, hit, hit_simple, stale};
    for (auto* unknown : release)
        if (unknown)
            unknown->Release();
    check(host.elements == 1, "every element is released except the cached root");
    uia_host_shutdown(&host);
    check(host.elements == 0 && !uia_host_element(&host, 0), "shutdown disconnects the root");
}

// ---- event decisions -----------------------------------------------------

struct EventShape {
    bool checked;
    u32 selected;
    u32 children;
    u32 focus;
    const char* live;
    u32 live_bytes;
    float range;
};

bool event_tree(UiAccessTree* tree, const EventShape& shape) noexcept {
    if (!ui_access_tree_reset(tree, utf8_literal("Events"), {0, 0, 100, 100}))
        return false;
    UiAccessNodeDesc list{};
    list.key = 1;
    list.rectangle = {0, 0, 50, 100};
    list.role = UiAccessRole::list;
    list.name = utf8_literal("List");
    const u32 list_node = ui_access_add(tree, 0, list);
    for (u32 index = 0; index < shape.children; ++index) {
        UiAccessNodeDesc item{};
        item.key = 10 + index;
        item.rectangle = {0, 10.0f * index, 50, 10.0f * index + 10};
        item.role = UiAccessRole::list_item;
        item.name = utf8_literal("Item");
        item.state = static_cast<u16>(ui_access_selectable | (index == shape.selected ? ui_access_selected : 0) |
                                      (index == shape.focus ? ui_access_focused : 0));
        item.actions = ui_access_action_select;
        ui_access_add(tree, list_node, item);
    }
    UiAccessNodeDesc toggle{};
    toggle.key = 2;
    toggle.rectangle = {60, 0, 100, 10};
    toggle.role = UiAccessRole::check_box;
    toggle.name = utf8_literal("Mute");
    toggle.state = static_cast<u16>(shape.checked ? ui_access_checked : 0);
    toggle.actions = ui_access_action_toggle;
    ui_access_add(tree, 0, toggle);
    UiAccessNodeDesc status{};
    status.key = 3;
    status.rectangle = {60, 20, 100, 30};
    status.role = UiAccessRole::status;
    status.name = {shape.live, shape.live_bytes};
    status.state = ui_access_live;
    ui_access_add(tree, 0, status);
    UiAccessNodeDesc slider{};
    slider.key = 4;
    slider.rectangle = {60, 40, 100, 50};
    slider.role = UiAccessRole::slider;
    slider.name = utf8_literal("Volume");
    slider.range = {shape.range, 0, 10, 1};
    slider.actions = ui_access_action_range;
    ui_access_add(tree, 0, slider);
    return ui_access_tree_finish(tree);
}

bool has_property(const UiAccessEventPlan& plan, const UiAccessTree& tree, u64 key, UiAccessProperty property) noexcept {
    for (u32 index = 0; index < plan.property_count; ++index)
        if (plan.properties[index].property == property && tree.nodes[plan.properties[index].node].key == key)
            return true;
    return false;
}

void test_events(UiAccessTree* tree) noexcept {
    static UiAccessObservation current[64], previous[64];
    UiAccessEventState state{};
    check(ui_access_event_state_initialize(&state, {current, 64}, {previous, 64}), "event state initializes");
    UiAccessEventPlan plan{};
    EventShape shape{false, 0, 3, 0, "Connecting", 10, 5};
    check(event_tree(tree, shape) && ui_access_plan_events(&state, *tree, &plan) && plan.focus_changed &&
              !plan.property_count && !plan.structure_count && !plan.structure_root,
          "the first snapshot only announces focus");
    check(event_tree(tree, shape) && ui_access_plan_events(&state, *tree, &plan) && !plan.focus_changed &&
              !plan.property_count && !plan.structure_count,
          "an unchanged snapshot raises nothing");
    shape.checked = true;
    shape.selected = 1;
    shape.range = 7;
    check(event_tree(tree, shape) && ui_access_plan_events(&state, *tree, &plan) &&
              has_property(plan, *tree, 2, UiAccessProperty::toggle) &&
              has_property(plan, *tree, 10, UiAccessProperty::selected) &&
              has_property(plan, *tree, 11, UiAccessProperty::selected) &&
              has_property(plan, *tree, 4, UiAccessProperty::range) && plan.properties[0].old_range >= 0,
          "toggle, selection and range changes raise property events");
    shape.children = 4;
    shape.focus = 3;
    check(event_tree(tree, shape) && ui_access_plan_events(&state, *tree, &plan) && plan.structure_count == 1 &&
              tree->nodes[plan.structures[0]].key == 1 && plan.focus_changed &&
              tree->nodes[plan.focus_node].key == 13,
          "a changed list raises a structure change and focus follows the new item");
    shape.live = "Connected";
    shape.live_bytes = 9;
    check(event_tree(tree, shape) && ui_access_plan_events(&state, *tree, &plan) &&
              has_property(plan, *tree, 3, UiAccessProperty::name) && !plan.focus_changed,
          "live status text changes are announced");

    UiAccessMessageCursor cursor{};
    check(!ui_access_message_should_announce(&cursor, 5, 100, false), "the first observed message only primes");
    check(!ui_access_message_should_announce(&cursor, 5, 100, false), "no new message, no announcement");
    check(ui_access_message_should_announce(&cursor, 5, 101, false), "a new message in the same channel announces");
    check(!ui_access_message_should_announce(&cursor, 5, 102, true), "own messages are not announced");
    check(!ui_access_message_should_announce(&cursor, 6, 500, false), "switching channels primes again");
    check(!ui_access_message_should_announce(&cursor, 6, 400, false), "older history never announces");
    check(ui_access_message_should_announce(&cursor, 6, 501, false), "the next new message after a switch announces");
}

// ---- overlay projections -------------------------------------------------

UiAccessNode g_overlay_nodes[2048];
char g_overlay_text[128 * 1024];
u32 g_overlay_slots[8192];
u32 g_overlay_keys[4096];
constexpr u8 kOverlaySurface = 20;

bool starts_with(Utf8View text, Utf8View prefix) noexcept {
    return text.bytes >= prefix.bytes && bytes_equal(text.data, prefix.data, prefix.bytes);
}

bool descends(const UiAccessTree& tree, u32 node, u32 ancestor) noexcept {
    for (u32 current = node; current != kUiAccessNone && current < tree.count; current = tree.nodes[current].parent)
        if (current == ancestor)
            return true;
    return false;
}

// First node (in reading order) with role under ancestor whose name starts with prefix.
u32 find_node(const UiAccessTree& tree, u32 ancestor, UiAccessRole role, Utf8View prefix = {}) noexcept {
    for (u32 index = 1; index < tree.count; ++index)
        if (tree.nodes[index].role == role && descends(tree, index, ancestor) &&
            starts_with(ui_access_name(tree, index), prefix))
            return index;
    return kUiAccessNone;
}

u32 count_role(const UiAccessTree& tree, u32 ancestor, UiAccessRole role) noexcept {
    u32 count = 0;
    for (u32 index = 1; index < tree.count; ++index)
        count += tree.nodes[index].role == role && descends(tree, index, ancestor) ? 1u : 0u;
    return count;
}

// Projects one overlay and its drawn text the way the application does.
template <typename Scene, typename Text>
u32 project_overlay(UiAccessTree* tree, const Scene& scene,
                    u32 (*add)(UiAccessTree*, u32, const Scene&, u8) noexcept, Text text) noexcept {
    if (!ui_access_tree_reset(tree, utf8_literal("Overlay"), {0, 0, 1600, 1000}))
        return kUiAccessNone;
    const u32 node = add(tree, 0, scene, kOverlaySurface);
    if (node == kUiAccessNone)
        return kUiAccessNone;
    static UiTextCommand commands[512];
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    if (ui_text_command_stream_initialize(&stream, {commands, 512}) && text(scene, &output))
        for (u32 index = 0; index < stream.count; ++index)
            ui_access_attribute_text(tree, commands[index].owner, commands[index].text, commands[index].rectangle);
    return ui_access_tree_finish(tree) ? (tree->count > 1 ? tree->nodes[0].first_child : kUiAccessNone)
                                       : kUiAccessNone;
}

struct QuickFixture {
    u32 calls;
};
bool quick_row(void*, u32 index, UiQuickSwitcherRow* row) noexcept {
    *row = {1000 + index, 71, 100 + index, 0, UiQuickSwitcherKind::text_channel, utf8_literal("release-planning"),
            utf8_literal("Native Lab"), utf8_literal("Channel"), {}, true};
    return true;
}
bool always_current(void*) noexcept {
    return true;
}

void test_quick_switcher(UiAccessTree* tree) noexcept {
    static UiQuickSwitcherScene scene{};
    QuickFixture fixture{};
    UiQuickSwitcherSource source{&fixture, 100, 1, quick_row, always_current};
    UiQuickSwitcherContent content{};
    content.phase = UiQuickSwitcherPhase::ready;
    content.revision = 1;
    content.edit = {utf8_literal("release"), 3, 7, true, true, true};
    check(ui_quick_switcher_scene_update(&scene, &source, &content, {0, 0, 1280, 800}),
          "quick switcher scene builds");
    const u32 dialog = project_overlay(tree, scene, ui_access_add_quick_switcher,
                                       [](const UiQuickSwitcherScene& s, UiTextOutput* out) noexcept {
                                           return ui_quick_switcher_scene_text_output(s, kDefaultTheme, out);
                                       });
    check(dialog != kUiAccessNone && tree->nodes[dialog].role == UiAccessRole::dialog &&
              (tree->nodes[dialog].state & ui_access_modal) &&
              equal(ui_access_name(*tree, dialog), utf8_literal("Quick switcher")),
          "quick switcher: modal dialog");
    const u32 combo = find_node(*tree, dialog, UiAccessRole::combo_box);
    check(combo != kUiAccessNone && equal(ui_access_name(*tree, combo), utf8_literal("Search destinations")) &&
              equal(ui_access_value(*tree, combo), utf8_literal("release")) &&
              (tree->nodes[combo].actions & ui_access_action_text) && (tree->nodes[combo].state & ui_access_expanded) &&
              (tree->nodes[combo].state & ui_access_focused) && tree->nodes[combo].placement.selection_anchor == 3 &&
              tree->nodes[combo].placement.selection_caret == 7 && tree->focus == combo,
          "quick switcher: query is a focused combo box with value, text selection and an open list");
    const u32 list = find_node(*tree, dialog, UiAccessRole::list, utf8_literal("Results"));
    const u32 first = find_node(*tree, list, UiAccessRole::list_item);
    check(list != kUiAccessNone && first != kUiAccessNone &&
              equal(ui_access_name(*tree, first), utf8_literal("release-planning, text channel, Native Lab")) &&
              (tree->nodes[first].state & ui_access_selected) && tree->nodes[first].placement.position == 1 &&
              tree->nodes[first].placement.set_size == 100 && tree->nodes[first].item == 1 &&
              (tree->nodes[first].actions & ui_access_action_scroll_item) && tree->nodes[first].first_child == kUiAccessNone,
          "quick switcher: result rows are list items with kind, selection and position of the whole set");
    check(count_role(*tree, list, UiAccessRole::list_item) == scene.count && tree->nodes[list].scroll.vertical_percent == 0,
          "quick switcher: every materialized row is listed with the list scroll position");
    UiAccessNameBuilder summary{};
    check(ui_access_quick_switcher_summary(scene, &summary) && equal(summary.view(), utf8_literal("100 results")),
          "quick switcher: result count summary");
    ui_quick_switcher_scene_release(&scene);
}

bool search_row(void*, u32 index, UiSearchRow* row) noexcept {
    *row = {71, index + 101, utf8_literal("Ana"), utf8_literal("# general"), utf8_literal("Build is green"),
            utf8_literal("Today at 12:30"), false, index % 3 == 0};
    return true;
}

void test_search(UiAccessTree* tree) noexcept {
    static UiSearchScene scene{};
    UiSearchSource results{nullptr, 6, 1, search_row};
    UiSearchContent content{};
    content.edit = {utf8_literal("green"), 5, 5, true, true, true};
    content.panel = content.tabs = true;
    content.revision = 1;
    content.total = 6;
    content.phase = UiSearchPhase::ready;
    content.tab_known = 1;
    content.tab_totals[0] = 6;
    check(ui_search_scene_update(&scene, &results, &content, {0, 0, 1280, 900}, {0, 0, 1280, 48}),
          "search scene builds");
    const u32 pane = project_overlay(tree, scene, ui_access_add_search,
                                     [](const UiSearchScene& s, UiTextOutput* out) noexcept {
                                         return ui_search_scene_text_output(s, kDefaultTheme, out, nullptr);
                                     });
    check(pane != kUiAccessNone && tree->nodes[pane].role == UiAccessRole::pane, "search: pane");
    const u32 field = find_node(*tree, pane, UiAccessRole::edit, utf8_literal("Search messages"));
    check(field != kUiAccessNone && equal(ui_access_value(*tree, field), utf8_literal("green")) &&
              (tree->nodes[field].actions & ui_access_action_text),
          "search: query field with value and text pattern");
    const u32 tabs = find_node(*tree, pane, UiAccessRole::tab_list);
    const u32 messages = find_node(*tree, tabs, UiAccessRole::tab, utf8_literal("Messages"));
    check(tabs != kUiAccessNone && count_role(*tree, tabs, UiAccessRole::tab) == 5 && messages != kUiAccessNone &&
              equal(ui_access_name(*tree, messages), utf8_literal("Messages, 6 results")) &&
              (tree->nodes[messages].state & ui_access_selected),
          "search: result type tabs form a tab list with counts and selection");
    const u32 list = find_node(*tree, pane, UiAccessRole::list, utf8_literal("Search results"));
    const u32 row = find_node(*tree, list, UiAccessRole::list_item);
    check(list != kUiAccessNone && row != kUiAccessNone &&
              equal(ui_access_name(*tree, row), utf8_literal("Ana, Today at 12:30: Build is green, in # general")) &&
              tree->nodes[row].placement.position == 1 && tree->nodes[row].placement.set_size == 6 &&
              count_role(*tree, list, UiAccessRole::list_item) >= 2,
          "search: results list items carry author, time, text, channel and set position");
    const u32 next = find_node(*tree, pane, UiAccessRole::button, utf8_literal("Next page"));
    check(next != kUiAccessNone && find_node(*tree, pane, UiAccessRole::button, utf8_literal("Page 1, current page")) !=
                                       kUiAccessNone,
          "search: pagination names pages and marks the current one");
    // Over the thread browser the header field is scoped to its threads.
    UiSearchContent scoped{};
    scoped.edit = {{}, 0, 0, true, true, true};
    scoped.revision = 1;
    scoped.heading = scoped.placeholder = utf8_literal("Search threads in product");
    scoped.scope_chip = utf8_literal("Threads");
    check(ui_search_scene_update(&scene, nullptr, &scoped, {0, 0, 1280, 900}, {0, 0, 1280, 48}) &&
              scene.scope.generation,
          "search scene builds with a thread scope chip");
    const u32 scoped_pane = project_overlay(tree, scene, ui_access_add_search,
                                            [](const UiSearchScene& s, UiTextOutput* out) noexcept {
                                                return ui_search_scene_text_output(s, kDefaultTheme, out, nullptr);
                                            });
    const u32 chip = find_node(*tree, scoped_pane, UiAccessRole::button, utf8_literal("Remove search scope: Threads"));
    check(find_node(*tree, scoped_pane, UiAccessRole::edit, utf8_literal("Search threads in product")) != kUiAccessNone &&
              chip != kUiAccessNone && equal(ui_access_help(*tree, chip), utf8_literal("Search the whole server instead")),
          "search: a scoped field is named by its scope and the chip says how to widen it");
    ui_search_scene_release(&scene);
}

bool pin_row(void*, u32 index, UiPinnedMessagesRow* row) noexcept {
    *row = {71, 100 + index, 200 + index, utf8_literal("Ana"), utf8_literal("Oct 5"), utf8_literal("Ship it"),
            {}, {}, 24.f, true, false};
    return true;
}

void test_pins(UiAccessTree* tree) noexcept {
    static UiPinnedMessagesScene scene{};
    UiPinnedMessagesSource source{nullptr, 3, 1, pin_row, always_current};
    UiPinnedMessagesContent content{};
    content.channel = utf8_literal("# notes");
    content.channel_id = 71;
    content.revision = 1;
    content.phase = UiPinnedMessagesPhase::ready;
    check(ui_pinned_messages_scene_update(&scene, &source, &content, {100, 48, 1300, 780}), "pins scene builds");
    const u32 pane = project_overlay(tree, scene, ui_access_add_pinned_messages,
                                     [](const UiPinnedMessagesScene& s, UiTextOutput* out) noexcept {
                                         return ui_pinned_messages_scene_text_output(s, kDefaultTheme, out);
                                     });
    check(pane != kUiAccessNone && equal(ui_access_name(*tree, pane), utf8_literal("Pinned messages in # notes")),
          "pins: named pane");
    const u32 list = find_node(*tree, pane, UiAccessRole::list, utf8_literal("Pinned messages"));
    const u32 row = find_node(*tree, list, UiAccessRole::list_item);
    check(row != kUiAccessNone && equal(ui_access_name(*tree, row), utf8_literal("Ana, Oct 5: Ship it")) &&
              tree->nodes[row].item == 100 && tree->nodes[row].placement.set_size == 3 &&
              (tree->nodes[row].actions & (ui_access_action_invoke | ui_access_action_scroll_item)) ==
                  (ui_access_action_invoke | ui_access_action_scroll_item),
          "pins: pinned messages are list items routed by message id");
    check(find_node(*tree, list, UiAccessRole::button, utf8_literal("Unpin message from Ana")) != kUiAccessNone &&
              find_node(*tree, pane, UiAccessRole::button, utf8_literal("Close pinned messages")) != kUiAccessNone,
          "pins: unpin and close buttons are named");
    ui_pinned_messages_scene_release(&scene);
}

void test_threads(UiAccessTree* tree) noexcept {
    static UiThreadBrowserScene scene{};
    UiThreadBrowserRow rows[3]{{100, utf8_literal("Planning"), false, true, true, 11},
                               {101, utf8_literal("Secrets"), true, false, true, 12},
                               {102, {}, false, false, false, 10}};
    UiThreadBrowserView view{};
    view.rows = {rows, 3};
    view.title = utf8_literal("# product");
    view.source_publication = 1;
    view.revision = 1;
    view.total = 3;
    view.inventory_synced = true;
    check(ui_thread_browser_scene_update(&scene, &view, {320, 48, 1280, 784}), "thread browser scene builds");
    const u32 pane = project_overlay(tree, scene, ui_access_add_thread_browser,
                                     [](const UiThreadBrowserScene& s, UiTextOutput* out) noexcept {
                                         return ui_thread_browser_scene_text_output(s, kDefaultTheme, out);
                                     });
    check(pane != kUiAccessNone && equal(ui_access_name(*tree, pane), utf8_literal("Threads in # product")) &&
              equal(ui_access_help(*tree, pane), utf8_literal("Search thread names with the header search bar")),
          "threads: named pane points to the header search bar");
    const u32 tabs = find_node(*tree, pane, UiAccessRole::tab_list);
    const u32 active = find_node(*tree, tabs, UiAccessRole::tab, utf8_literal("Active threads"));
    check(tabs != kUiAccessNone && active != kUiAccessNone && (tree->nodes[active].state & ui_access_selected) &&
              count_role(*tree, tabs, UiAccessRole::tab) == 2,
          "threads: active/archived tabs form a tab list");
    const u32 list = find_node(*tree, pane, UiAccessRole::list, utf8_literal("Active threads"));
    check(list != kUiAccessNone &&
              find_node(*tree, list, UiAccessRole::list_item, utf8_literal("Planning, public thread, active, joined")) !=
                  kUiAccessNone &&
              find_node(*tree, list, UiAccessRole::list_item, utf8_literal("Secrets, private thread, archived")) !=
                  kUiAccessNone &&
              find_node(*tree, list, UiAccessRole::list_item, utf8_literal("Unnamed thread, announcement thread")) !=
                  kUiAccessNone,
          "threads: rows describe kind, archive and membership state");
    check(count_role(*tree, pane, UiAccessRole::edit) == 0 &&
              find_node(*tree, pane, UiAccessRole::button, utf8_literal("Search")) == kUiAccessNone,
          "threads: the panel has no search field or Search button of its own");
    ui_thread_browser_scene_release(&scene);
}

bool inbox_row(void*, u32 index, UiInboxRow* row) noexcept {
    *row = {UiInboxRowKind::mention, 500 + index, 70, 9, utf8_literal("Ana"), utf8_literal("#general"),
            utf8_literal("@you ship it"), utf8_literal("5 minutes ago")};
    return true;
}

void test_inbox(UiAccessTree* tree) noexcept {
    static UiInboxScene scene{};
    UiInboxSource source{nullptr, 4, 1, inbox_row, always_current};
    UiInboxContent content{};
    content.revision = 1;
    content.phase = UiInboxPhase::ready;
    content.this_server_available = true;
    check(ui_inbox_scene_update(&scene, &source, &content, {240, 48, 1280, 800}), "inbox scene builds");
    const u32 pane = project_overlay(tree, scene, ui_access_add_inbox,
                                     [](const UiInboxScene& s, UiTextOutput* out) noexcept {
                                         return ui_inbox_scene_text_output(s, kDefaultTheme, out);
                                     });
    const u32 tabs = find_node(*tree, pane, UiAccessRole::tab_list);
    const u32 mentions = find_node(*tree, tabs, UiAccessRole::tab, utf8_literal("Mentions"));
    check(pane != kUiAccessNone && equal(ui_access_name(*tree, pane), utf8_literal("Inbox")) && mentions != kUiAccessNone &&
              (tree->nodes[mentions].state & ui_access_selected) &&
              find_node(*tree, tabs, UiAccessRole::tab, utf8_literal("Unreads")) != kUiAccessNone,
          "inbox: mentions and unreads tabs");
    const u32 everyone = find_node(*tree, pane, UiAccessRole::check_box, utf8_literal("Include @everyone"));
    const u32 server = find_node(*tree, pane, UiAccessRole::check_box, utf8_literal("Only this server"));
    check(everyone != kUiAccessNone && (tree->nodes[everyone].state & ui_access_checked) && server != kUiAccessNone &&
              !(tree->nodes[server].state & ui_access_checked) &&
              (tree->nodes[server].actions & ui_access_action_toggle),
          "inbox: mention filters are check boxes with their state");
    const u32 list = find_node(*tree, pane, UiAccessRole::list, utf8_literal("Recent mentions"));
    const u32 row = find_node(*tree, list, UiAccessRole::list_item);
    check(row != kUiAccessNone &&
              equal(ui_access_name(*tree, row), utf8_literal("Ana, 5 minutes ago: @you ship it, in #general")) &&
              tree->nodes[row].placement.position == 1 && tree->nodes[row].placement.set_size == 4,
          "inbox: mentions list items carry author, time, text and channel");
    ui_inbox_scene_release(&scene);
}

void test_call(UiAccessTree* tree) noexcept {
    static UiCallScene scene{};
    UiCallParticipant people[3]{};
    people[0].user_id = 1;
    people[0].name = utf8_literal("Ana");
    people[0].self = true;
    people[1].user_id = 2;
    people[1].name = utf8_literal("Bo");
    people[1].speaking = people[1].muted = true;
    people[2].user_id = 3;
    people[2].name = utf8_literal("Cy");
    people[2].deafened = true;
    UiCallContent content{};
    content.title = utf8_literal("General");
    content.participants = {people, 3};
    content.connected = true;
    check(ui_call_scene_update(&scene, content, {300, 0, 1500, 900}, 1000, true), "call scene builds");
    const u32 pane = project_overlay(tree, scene, ui_access_add_call,
                                     [](const UiCallScene& s, UiTextOutput* out) noexcept {
                                         return ui_call_scene_text_output(s, kDefaultTheme, out);
                                     });
    check(pane != kUiAccessNone && equal(ui_access_name(*tree, pane), utf8_literal("Call: General")) &&
              count_role(*tree, pane, UiAccessRole::text) == 0,
          "call: named pane without duplicated drawn text");
    const u32 list = find_node(*tree, pane, UiAccessRole::list, utf8_literal("Participants, 3"));
    check(list != kUiAccessNone && count_role(*tree, list, UiAccessRole::list_item) == 3 &&
              find_node(*tree, list, UiAccessRole::list_item, utf8_literal("Ana, you")) != kUiAccessNone &&
              find_node(*tree, list, UiAccessRole::list_item, utf8_literal("Bo, speaking, muted")) != kUiAccessNone &&
              find_node(*tree, list, UiAccessRole::list_item, utf8_literal("Cy, deafened")) != kUiAccessNone,
          "call: participants carry speaking, mute and deafen state");
    const u32 bar = find_node(*tree, pane, UiAccessRole::group, utf8_literal("Call controls"));
    const u32 mute = find_node(*tree, bar, UiAccessRole::toggle_button, utf8_literal("Mute"));
    const u32 camera = find_node(*tree, bar, UiAccessRole::button, utf8_literal("Camera"));
    check(bar != kUiAccessNone && mute != kUiAccessNone && (tree->nodes[mute].actions & ui_access_action_toggle) &&
              !(tree->nodes[mute].state & ui_access_checked) && camera != kUiAccessNone &&
              (tree->nodes[camera].state & ui_access_disabled) &&
              find_node(*tree, bar, UiAccessRole::button, utf8_literal("Disconnect")) != kUiAccessNone,
          "call: control bar exposes toggles, disabled controls and disconnect");
    ui_call_scene_release(&scene);
}

bool measure_text(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    const auto validation = utf8_validate(request.text);
    if (!validation.valid || request.constraint.width <= 0)
        return false;
    const float natural = static_cast<float>(validation.codepoints) * 8;
    const u32 columns = maximum(1u, static_cast<u32>(request.constraint.width / 8));
    const u32 lines = request.no_wrap ? 1u : maximum(1u, (validation.codepoints + columns - 1) / columns);
    *value = {{request.no_wrap ? natural : minimum(natural, request.constraint.width), static_cast<float>(lines) * 20},
              16,
              lines};
    return true;
}

void test_forms(UiAccessTree* tree) noexcept {
    static UiTextMeasureRecord records[64]{};
    static UiTextMeasureCache cache{};
    check(ui_text_measure_cache_initialize(&cache, {records, 64}, measure_text, nullptr), "form measure cache");
    static TextFormScene form{};
    TextFormContent content{utf8_literal("Description"), utf8_literal("image.png"),
                            utf8_literal("Describe this attachment"), utf8_literal("Too long"),
                            utf8_literal("Cancel"), utf8_literal("Save"), utf8_literal("A\r\n\xF0\x9F\x98\x80"),
                            0, 5, true, true};
    check(text_form_update(&form, content, {100, 50, 900, 750}, &cache), "text form builds");
    const u32 dialog = project_overlay(tree, form, ui_access_add_text_form,
                                       [](const TextFormScene&, UiTextOutput*) noexcept { return true; });
    const u32 edit = find_node(*tree, dialog, UiAccessRole::edit);
    check(dialog != kUiAccessNone && tree->nodes[dialog].role == UiAccessRole::dialog &&
              equal(ui_access_name(*tree, dialog), utf8_literal("Description")) &&
              equal(ui_access_help(*tree, dialog), utf8_literal("image.png")) && edit != kUiAccessNone &&
              equal(ui_access_name(*tree, edit), utf8_literal("Describe this attachment")) &&
              tree->nodes[edit].placement.selection_caret == 7 && (tree->nodes[edit].state & ui_access_focused) &&
              (tree->nodes[edit].actions & ui_access_action_text),
          "text form: dialog with labelled field, UTF-16 caret mapped to bytes and focus");
    const u32 status = find_node(*tree, dialog, UiAccessRole::status);
    check(status != kUiAccessNone && (tree->nodes[status].state & ui_access_live) &&
              find_node(*tree, dialog, UiAccessRole::button, utf8_literal("Save")) != kUiAccessNone &&
              find_node(*tree, dialog, UiAccessRole::button, utf8_literal("Cancel")) != kUiAccessNone,
          "text form: error text is a live status; buttons are named");
    text_form_release(&form);

    static ForumPostScene forum{};
    ForumPostTagView tags[]{{1, utf8_literal("Questions"), false, true}, {2, utf8_literal("Review"), true, true}};
    ForumPostContent post{};
    post.heading = utf8_literal("Create a forum post");
    post.title_label = utf8_literal("Title");
    post.body_label = utf8_literal("Message");
    post.tags_hint = utf8_literal("Choose tags");
    post.cancel = utf8_literal("Cancel");
    post.accept = utf8_literal("Create post");
    post.fields[0] = {utf8_literal("Release"), 0, 7, {}, true};
    post.fields[1] = {utf8_literal("Line\nNext"), 0, 0, {}, false};
    post.tags = {tags, 2};
    post.editable = post.can_accept = true;
    check(forum_post_update(&forum, post, {0, 0, 640, 600}, &cache), "forum post builds");
    const u32 panel = project_overlay(tree, forum, ui_access_add_forum_post,
                                      [](const ForumPostScene&, UiTextOutput*) noexcept { return true; });
    const u32 title = find_node(*tree, panel, UiAccessRole::edit, utf8_literal("Title"));
    const u32 body = find_node(*tree, panel, UiAccessRole::edit, utf8_literal("Message"));
    const u32 group = find_node(*tree, panel, UiAccessRole::group, utf8_literal("Choose tags"));
    const u32 review = find_node(*tree, group, UiAccessRole::check_box, utf8_literal("Review"));
    check(panel != kUiAccessNone && tree->nodes[panel].role == UiAccessRole::dialog &&
              equal(ui_access_name(*tree, panel), utf8_literal("Create a forum post")) && title != kUiAccessNone &&
              equal(ui_access_value(*tree, title), utf8_literal("Release")) &&
              tree->nodes[title].placement.selection_caret == 7 && body != kUiAccessNone &&
              (tree->nodes[body].state & ui_access_multiline) && review != kUiAccessNone &&
              (tree->nodes[review].state & ui_access_checked) && tree->nodes[review].placement.set_size == 2,
          "forum: dialog with title/body fields and checkable tags in a group");
    forum_post_release(&forum);
}

void test_viewer(UiAccessTree* tree) noexcept {
    static UiImageViewer viewer{};
    check(ui_image_viewer_open(&viewer, {0, 0, 1200, 800}, {1600, 900}), "image viewer opens");
    const u32 dialog = project_overlay(tree, viewer, ui_access_add_image_viewer,
                                       [](const UiImageViewer& v, UiTextOutput* out) noexcept {
                                           return ui_image_viewer_text_output(v, kDefaultTheme, out);
                                       });
    const u32 image = find_node(*tree, dialog, UiAccessRole::image, utf8_literal("Image"));
    check(dialog != kUiAccessNone && equal(ui_access_name(*tree, dialog), utf8_literal("Image viewer")) &&
              (tree->nodes[dialog].state & ui_access_modal) && image != kUiAccessNone &&
              starts_with(ui_access_help(*tree, image), utf8_literal("Zoom ")),
          "media viewer: modal dialog around the image with its zoom");
    check(find_node(*tree, dialog, UiAccessRole::button, utf8_literal("Close image")) != kUiAccessNone &&
              find_node(*tree, dialog, UiAccessRole::button, utf8_literal("Zoom in")) != kUiAccessNone &&
              find_node(*tree, dialog, UiAccessRole::button, utf8_literal("Open original")) == kUiAccessNone,
          "media viewer: visible actions only");
}

bool picker_item(void*, u32 index, UiMediaPickerItem* item) noexcept {
    *item = {index + 1, index + 1001, utf8_literal("Party parrot"), {}, 0, 320, 180, UiMediaPickerItemKind::gif, true};
    return true;
}

void test_picker(UiAccessTree* tree) noexcept {
    static UiMediaPickerScene scene{};
    UiMediaPickerSource source{nullptr, 3, 7, 40, 1, {}, picker_item};
    UiMediaPickerContent content{};
    content.account_scope = 3;
    content.query_generation = 7;
    content.revision = 1;
    content.kind = UiMediaPickerKind::gif;
    content.phase = UiMediaPickerPhase::ready;
    content.available_tabs = 7;
    content.edit.active = true;
    check(ui_media_picker_scene_update(&scene, &source, &content, {0, 0, 1000, 800}, {560, 700, 940, 740}),
          "media picker builds");
    check(ui_media_picker_scene_select(&scene, 1), "media picker selects");
    const u32 dialog = project_overlay(tree, scene, ui_access_add_media_picker,
                                       [](const UiMediaPickerScene&, UiTextOutput*) noexcept { return true; });
    const u32 tabs = find_node(*tree, dialog, UiAccessRole::tab_list);
    const u32 gifs = find_node(*tree, tabs, UiAccessRole::tab, utf8_literal("GIFs"));
    check(dialog != kUiAccessNone && equal(ui_access_name(*tree, dialog), utf8_literal("GIF picker")) &&
              count_role(*tree, tabs, UiAccessRole::tab) == 3 && gifs != kUiAccessNone &&
              (tree->nodes[gifs].state & ui_access_selected),
          "picker: emoji/GIF/sticker tabs");
    const u32 grid = find_node(*tree, dialog, UiAccessRole::list, utf8_literal("GIFs"));
    const u32 first = find_node(*tree, grid, UiAccessRole::list_item);
    const u32 second = first != kUiAccessNone ? tree->nodes[first].next_sibling : kUiAccessNone;
    check(grid != kUiAccessNone && (tree->nodes[grid].actions & ui_access_action_grid) &&
              tree->nodes[grid].placement.grid_row >= 1 && tree->nodes[grid].placement.grid_column >= 2 &&
              first != kUiAccessNone && second != kUiAccessNone &&
              (tree->nodes[first].actions & ui_access_action_grid_item) && tree->nodes[first].placement.grid_row == 0 &&
              tree->nodes[first].placement.grid_column == 0 && tree->nodes[second].placement.grid_column == 1 &&
              (tree->nodes[second].state & ui_access_selected) &&
              equal(ui_access_name(*tree, first), utf8_literal("Party parrot")),
          "picker: cells form a grid with row/column positions and selection");
    check(find_node(*tree, dialog, UiAccessRole::edit, utf8_literal("Search GIFs")) != kUiAccessNone,
          "picker: search field");

    if (grid == kUiAccessNone || second == kUiAccessNone) {
        ui_media_picker_scene_release(&scene);
        return;
    }
    // Grid and GridItem patterns through the provider.
    HostContext context{tree, {}, 0, 0};
    UiaProviderHost host{};
    check(uia_host_initialize(&host, nullptr, {&context, host_tree, host_act, host_scale}) && uia_host_load(&host),
          "picker provider host");
    IRawElementProviderSimple* grid_element = element(&host, tree->nodes[grid].key);
    IGridProvider* grid_pattern = pattern<IGridProvider>(grid_element, UIA_GridPatternId);
    IRawElementProviderSimple* cell = nullptr;
    int rows = 0, columns = 0;
    check(grid_pattern && SUCCEEDED(grid_pattern->get_RowCount(&rows)) &&
              SUCCEEDED(grid_pattern->get_ColumnCount(&columns)) && rows == tree->nodes[grid].placement.grid_row &&
              columns == tree->nodes[grid].placement.grid_column && SUCCEEDED(grid_pattern->GetItem(0, 1, &cell)) &&
              cell && grid_pattern->GetItem(rows, 0, &cell) == E_INVALIDARG,
          "Grid pattern reports dimensions and finds cells");
    IRawElementProviderSimple* second_element = element(&host, tree->nodes[second].key);
    IGridItemProvider* item = pattern<IGridItemProvider>(second_element, UIA_GridItemPatternId);
    IRawElementProviderSimple* container = nullptr;
    int column = -1;
    check(item && SUCCEEDED(item->get_Column(&column)) && column == 1 && SUCCEEDED(item->get_ContainingGrid(&container)) &&
              container && int_property(second_element, UIA_PositionInSetPropertyId) == -1,
          "GridItem pattern reports its cell and containing grid");
    IUnknown* release[]{grid_element, grid_pattern, cell, second_element, item, container};
    for (auto* unknown : release)
        if (unknown)
            unknown->Release();
    uia_host_shutdown(&host);
    ui_media_picker_scene_release(&scene);
}

void test_overlays() noexcept {
    static UiAccessTree tree{};
    check(ui_access_tree_initialize(&tree, {g_overlay_nodes, 2048}, {g_overlay_text, sizeof(g_overlay_text)},
                                    {g_overlay_slots, 8192}, {g_overlay_keys, 4096}),
          "overlay tree initializes");
    test_quick_switcher(&tree);
    test_search(&tree);
    test_pins(&tree);
    test_threads(&tree);
    test_inbox(&tree);
    test_call(&tree);
    test_forms(&tree);
    test_viewer(&tree);
    test_picker(&tree);
}

// ---- demand-committed capacity ----------------------------------------------

// Hosts commit snapshot storage on demand and rebuild when a region runs out:
// the snapshot must say which region was too small, and only on a real
// capacity shortfall (field limits and successful builds flag nothing).
void test_exhaustion(const Scene& scene) noexcept {
    static UiAccessNode nodes[256];
    static char text[16384];
    static u32 slots[512], keys[512];
    UiAccessTree tree{};
    check(ui_access_tree_initialize(&tree, {nodes, 256}, {text, sizeof(text)}, {slots, 512}, {keys, 512}) &&
              build_tree(&tree, scene) && tree.exhausted == 0,
          "full-capacity snapshot exhausts nothing");
    const u32 needed_nodes = tree.count, needed_text = tree.text_used;

    check(ui_access_tree_initialize(&tree, {nodes, 3}, {text, sizeof(text)}, {slots, 512}, {keys, 512}),
          "small node tree initializes");
    build_tree(&tree, scene);
    check(tree.exhausted == ui_access_exhausted_nodes, "node shortfall flags nodes only");
    check(ui_access_tree_initialize(&tree, {nodes, 256}, {text, sizeof(text)}, {slots, 2}, {keys, 512}),
          "small slot tree initializes");
    build_tree(&tree, scene);
    check((tree.exhausted & ui_access_exhausted_slots) && !(tree.exhausted & ui_access_exhausted_nodes),
          "slot shortfall flags slots");
    const u32 short_text = needed_text > 8 ? needed_text - 8 : 1;
    check(ui_access_tree_initialize(&tree, {nodes, 256}, {text, short_text}, {slots, 512}, {keys, 512}),
          "small text tree initializes");
    build_tree(&tree, scene);
    check((tree.exhausted & ui_access_exhausted_text) && !(tree.exhausted & ui_access_exhausted_nodes),
          "text shortfall flags text");
    check(ui_access_tree_reset(&tree, utf8_literal("Synthetic"), {0, 0, 400, 600}) && tree.exhausted == 0,
          "reset clears exhaustion");

    // Doubling until nothing is flagged reproduces the full-capacity snapshot.
    u32 node_count = 4, text_count = 64, slot_count = 4;
    for (u32 attempt = 0; attempt < 16; ++attempt) {
        check(ui_access_tree_initialize(&tree, {nodes, node_count}, {text, text_count}, {slots, slot_count},
                                        {keys, 512}),
              "growing tree initializes");
        const bool built = build_tree(&tree, scene) && tree.exhausted == 0;
        if (!tree.exhausted) {
            check(built && tree.count == needed_nodes && tree.text_used == needed_text,
                  "grown snapshot matches the full-capacity snapshot");
            break;
        }
        if (tree.exhausted & ui_access_exhausted_nodes) node_count = minimum(node_count * 2, 256u);
        if (tree.exhausted & ui_access_exhausted_slots) slot_count = minimum(slot_count * 2, 512u);
        if (tree.exhausted & ui_access_exhausted_text) text_count = minimum<u32>(text_count * 2, sizeof(text));
        check(attempt != 15, "demand growth converges");
    }
}

// ---- text pattern ----------------------------------------------------------

void test_text_units() noexcept {
    const Utf8View text = utf8_literal("one two, three\nfour \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD!");
    u32 start = 4, end = 4;
    ui_access_text_expand(text, UiAccessTextUnit::word, &start, &end);
    check(start == 4 && end == 7, "word unit: a word with its trailing space");
    start = 5;
    end = 5;
    ui_access_text_expand(text, UiAccessTextUnit::line, &start, &end);
    check(start == 0 && end == 15, "line unit ends after the line feed");
    start = 21;
    end = 21;
    ui_access_text_expand(text, UiAccessTextUnit::character, &start, &end);
    check(start == 20 && end == 28, "character unit keeps an emoji with its skin tone");
    check(ui_access_text_align(text, 22) == 20, "offsets align to cluster starts");
    start = 0;
    end = 3;
    check(ui_access_text_move(text, UiAccessTextUnit::word, 2, &start, &end) == 2 && start == 7 && end == 9,
          "Move by two words lands on the punctuation word");
    start = end = text.bytes;
    check(ui_access_text_move(text, UiAccessTextUnit::line, -1, &start, &end) == -1 && start == 15 && end == 15,
          "degenerate Move back one line from the end");
    u32 offset = 0;
    check(ui_access_text_move_endpoint(text, UiAccessTextUnit::document, 5, &offset) == 1 && offset == text.bytes &&
              ui_access_text_move_endpoint(text, UiAccessTextUnit::character, -100, &offset) < 0 && offset == 0,
          "endpoints move by document and clamp at the start");
    start = 0;
    end = 4;
    check(ui_access_text_move(text, UiAccessTextUnit::line, 5, &start, &end) == 1 && start == 15 && end == text.bytes,
          "Move never starts a unit at the text end");
    start = end = 0;
    ui_access_text_expand({}, UiAccessTextUnit::word, &start, &end);
    check(start == 0 && end == 0, "empty text expands to an empty range");
    u32 anchor = 0, caret = 0;
    ui_access_text_selection_unpack(ui_access_text_selection_pack(12, 7000), &anchor, &caret);
    check(anchor == 12 && caret == 7000, "selection packs into an action number");
}

void test_text_pattern(UiAccessTree* tree) noexcept {
    ui_access_tree_reset(tree, utf8_literal("Text"), {0, 0, 400, 300});
    UiAccessNodeDesc edit{};
    edit.key = 700;
    edit.rectangle = {10, 10, 300, 60};
    edit.role = UiAccessRole::edit;
    edit.name = utf8_literal("Message");
    edit.value = utf8_literal("one two\nthree");
    edit.state = ui_access_focusable | ui_access_focused | ui_access_multiline;
    edit.actions = ui_access_action_value | ui_access_action_text | ui_access_action_focus;
    edit.placement.selection_anchor = 7;
    edit.placement.selection_caret = 4;
    UiAccessNodeDesc password{};
    password.key = 701;
    password.rectangle = {10, 70, 300, 90};
    password.role = UiAccessRole::edit;
    password.name = utf8_literal("Password");
    password.value = utf8_literal("secret");
    password.state = ui_access_password;
    password.actions = ui_access_action_value | ui_access_action_text;
    check(ui_access_add(tree, 0, edit) != kUiAccessNone && ui_access_add(tree, 0, password) != kUiAccessNone &&
              ui_access_tree_finish(tree),
          "text snapshot");
    HostContext context{tree, {}, 0, 0};
    UiaProviderHost host{};
    check(uia_host_initialize(&host, nullptr, {&context, host_tree, host_act, host_scale}) && uia_host_load(&host),
          "text provider host");
    IRawElementProviderSimple* field = element(&host, 700);
    ITextProvider* text = pattern<ITextProvider>(field, UIA_TextPatternId);
    IValueProvider* value = pattern<IValueProvider>(field, UIA_ValuePatternId);
    check(text && value, "edits keep the Value pattern and add the Text pattern");
    SAFEARRAY* selection = nullptr;
    ITextRangeProvider* selected = nullptr;
    LONG zero = 0;
    BSTR string = nullptr;
    check(text && SUCCEEDED(text->GetSelection(&selection)) && selection &&
              SUCCEEDED(SafeArrayGetElement(selection, &zero, &selected)) && selected &&
              SUCCEEDED(selected->GetText(-1, &string)) && wide_equal(string, L"two"),
          "GetSelection returns the ordered caret selection");
    SysFreeString(string);
    string = nullptr;
    if (selection)
        SafeArrayDestroy(selection);
    ITextRangeProvider* document = nullptr;
    check(text && SUCCEEDED(text->get_DocumentRange(&document)) && document &&
              SUCCEEDED(document->GetText(-1, &string)) && wide_equal(string, L"one two\nthree"),
          "DocumentRange reads the whole draft");
    SysFreeString(string);
    string = nullptr;
    check(document && SUCCEEDED(document->GetText(3, &string)) && wide_equal(string, L"one"),
          "GetText honours maxLength");
    SysFreeString(string);
    string = nullptr;
    ITextRangeProvider* walker = nullptr;
    int moved = 0;
    check(document && SUCCEEDED(document->Clone(&walker)) && walker &&
              SUCCEEDED(walker->ExpandToEnclosingUnit(TextUnit_Line)) && SUCCEEDED(walker->GetText(-1, &string)) &&
              wide_equal(string, L"one two\n"),
          "ExpandToEnclosingUnit by line");
    SysFreeString(string);
    string = nullptr;
    check(walker && SUCCEEDED(walker->Move(TextUnit_Line, 1, &moved)) && moved == 1 &&
              SUCCEEDED(walker->GetText(-1, &string)) && wide_equal(string, L"three"),
          "Move to the next line");
    SysFreeString(string);
    string = nullptr;
    check(walker && SUCCEEDED(walker->ExpandToEnclosingUnit(TextUnit_Character)) &&
              SUCCEEDED(walker->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Word, 1, &moved)) &&
              moved == 1 && SUCCEEDED(walker->GetText(-1, &string)) && wide_equal(string, L"three"),
          "MoveEndpointByUnit extends by word");
    SysFreeString(string);
    string = nullptr;
    int order = 0;
    BOOL same = TRUE;
    check(walker && selected && SUCCEEDED(walker->CompareEndpoints(TextPatternRangeEndpoint_Start, selected,
                                                                   TextPatternRangeEndpoint_End, &order)) &&
              order > 0 && SUCCEEDED(walker->Compare(selected, &same)) && !same,
          "ranges compare by endpoints");
    check(walker && selected &&
              SUCCEEDED(walker->MoveEndpointByRange(TextPatternRangeEndpoint_Start, selected,
                                                    TextPatternRangeEndpoint_Start)) &&
              SUCCEEDED(walker->GetText(-1, &string)) && wide_equal(string, L"two\nthree"),
          "MoveEndpointByRange");
    SysFreeString(string);
    string = nullptr;
    ITextRangeProvider* found = nullptr;
    BSTR needle = SysAllocString(L"THREE");
    check(document && SUCCEEDED(document->FindText(needle, FALSE, TRUE, &found)) && found &&
              SUCCEEDED(found->Select()) && context.act.action == UiAccessAction::select_text,
          "FindText (ignoring case) and Select request a text selection");
    SysFreeString(needle);
    u32 anchor = 0, caret = 0;
    ui_access_text_selection_unpack(context.act.number, &anchor, &caret);
    check(anchor == 8 && caret == 13 && context.act.key == 700, "Select packs the range's byte offsets");
    VARIANT attribute{};
    check(document && SUCCEEDED(document->GetAttributeValue(UIA_IsReadOnlyAttributeId, &attribute)) &&
              attribute.vt == VT_BOOL && attribute.boolVal == VARIANT_FALSE,
          "IsReadOnly text attribute");
    VariantClear(&attribute);
    check(document && SUCCEEDED(document->GetAttributeValue(UIA_FontWeightAttributeId, &attribute)) &&
              attribute.vt == VT_UNKNOWN,
          "unsupported attributes report the reserved not-supported value");
    VariantClear(&attribute);
    SAFEARRAY* rectangles = nullptr;
    LONG upper = 0;
    check(document && SUCCEEDED(document->GetBoundingRectangles(&rectangles)) && rectangles &&
              SUCCEEDED(SafeArrayGetUBound(rectangles, 1, &upper)) && upper == -1,
          "ranges without host geometry have no rectangles");
    if (rectangles)
        SafeArrayDestroy(rectangles);
    IRawElementProviderSimple* enclosing = nullptr;
    check(document && SUCCEEDED(document->GetEnclosingElement(&enclosing)) && name_is(enclosing, L"Message"),
          "GetEnclosingElement is the edit");
    IRawElementProviderSimple* secret = element(&host, 701);
    IValueProvider* secret_value = pattern<IValueProvider>(secret, UIA_ValuePatternId);
    BSTR hidden = nullptr;
    check(!pattern<ITextProvider>(secret, UIA_TextPatternId) && secret_value &&
              FAILED(secret_value->get_Value(&hidden)) && !hidden &&
              int_property(secret, UIA_IsPasswordPropertyId) == 1,
          "password fields are protected: IsPassword, no value, no text pattern");
    IUnknown* release[]{field, text, value, selected, document, walker, found, enclosing, secret, secret_value};
    for (auto* unknown : release)
        if (unknown)
            unknown->Release();
    check(host.ranges == 0 && host.elements == 0, "text ranges and elements are released");
    uia_host_shutdown(&host);
}

// ---- scroll into view, radio groups, synthetic groups ----------------------

void test_scroll_and_radio(UiAccessTree* tree) noexcept {
    ui_access_tree_reset(tree, utf8_literal("Lists"), {0, 0, 400, 300});
    UiAccessNodeDesc list{};
    list.key = 800;
    list.rectangle = {0, 0, 200, 100};
    list.role = UiAccessRole::list;
    list.name = utf8_literal("Members");
    const u32 list_node = ui_access_add(tree, 0, list);
    UiAccessNodeDesc row{};
    row.key = 801;
    row.item = 42;
    row.rectangle = {0, 120, 200, 150};
    row.role = UiAccessRole::list_item;
    row.name = utf8_literal("Offscreen member");
    row.state = ui_access_offscreen | ui_access_disabled;
    row.actions = ui_access_action_invoke | ui_access_action_scroll_item;
    ui_access_add(tree, list_node, row);
    UiAccessNodeDesc group{};
    group.key = 810;
    group.rectangle = {220, 0, 400, 100};
    group.role = UiAccessRole::group;
    group.name = utf8_literal("Theme");
    group.actions = ui_access_action_select;
    const u32 group_node = ui_access_add(tree, 0, group);
    const char* labels[]{"Dark", "Light", "System"};
    for (u32 index = 0; index < 3; ++index) {
        UiAccessNodeDesc radio{};
        radio.key = 811 + index;
        radio.rectangle = {220, 30.0f * index, 400, 30.0f * index + 30};
        radio.role = UiAccessRole::radio_button;
        radio.name = {labels[index], index == 2 ? 6u : index == 1 ? 5u : 4u};
        radio.state = static_cast<u16>(ui_access_selectable | (index == 1 ? ui_access_selected | ui_access_checked : 0));
        radio.actions = ui_access_action_select;
        radio.placement = {index + 1, 3, 0, 0, 0, 0};
        ui_access_add(tree, group_node, radio);
    }
    check(ui_access_tree_finish(tree), "scroll/radio snapshot");
    const u32 offscreen = ui_access_find(*tree, 801);
    check(offscreen != kUiAccessNone && ui_access_needs_reveal(tree->nodes[offscreen]), "offscreen items need a reveal");
    UiAccessNode partial{};
    partial.rectangle = {0, 0, 100, 40};
    partial.visible = {0, 30, 100, 40};
    UiAccessNode mostly{};
    mostly.rectangle = {0, 0, 100, 40};
    mostly.visible = {0, 0, 100, 30};
    check(ui_access_needs_reveal(partial) && !ui_access_needs_reveal(mostly),
          "items clipped below half their height need a reveal");

    HostContext context{tree, {}, 0, 0};
    UiaProviderHost host{};
    check(uia_host_initialize(&host, nullptr, {&context, host_tree, host_act, host_scale}) && uia_host_load(&host),
          "scroll provider host");
    IRawElementProviderSimple* member = element(&host, 801);
    IScrollItemProvider* scroll_item = pattern<IScrollItemProvider>(member, UIA_ScrollItemPatternId);
    check(scroll_item && SUCCEEDED(scroll_item->ScrollIntoView()) &&
              context.act.action == UiAccessAction::scroll_into_view && context.act.key == 801,
          "ScrollItem routes ScrollIntoView to the item even while it is offscreen and disabled");
    IRawElementProviderSimple* theme = element(&host, 810);
    ISelectionProvider* selection = pattern<ISelectionProvider>(theme, UIA_SelectionPatternId);
    SAFEARRAY* selected = nullptr;
    LONG upper = -1;
    check(selection && SUCCEEDED(selection->GetSelection(&selected)) && selected &&
              SUCCEEDED(SafeArrayGetUBound(selected, 1, &upper)) && upper == 0 &&
              !pattern<ISelectionItemProvider>(theme, UIA_SelectionItemPatternId) &&
              int_property(theme, UIA_ControlTypePropertyId) == UIA_GroupControlTypeId,
          "radio group: a group with Selection over its options");
    if (selected)
        SafeArrayDestroy(selected);
    IRawElementProviderSimple* light = element(&host, 812);
    ISelectionItemProvider* option = pattern<ISelectionItemProvider>(light, UIA_SelectionItemPatternId);
    IRawElementProviderSimple* container = nullptr;
    BOOL is_selected = FALSE;
    check(option && SUCCEEDED(option->get_IsSelected(&is_selected)) && is_selected &&
              SUCCEEDED(option->get_SelectionContainer(&container)) && name_is(container, L"Theme") &&
              int_property(light, UIA_ControlTypePropertyId) == UIA_RadioButtonControlTypeId &&
              int_property(light, UIA_PositionInSetPropertyId) == 2 && int_property(light, UIA_SizeOfSetPropertyId) == 3,
          "radio options: RadioButton with SelectionItem, container and set position");
    check(option && SUCCEEDED(option->Select()) && context.act.action == UiAccessAction::select &&
              context.act.key == 812,
          "selecting a radio option routes to that option");
    IUnknown* release[]{member, scroll_item, theme, selection, light, option, container};
    for (auto* unknown : release)
        if (unknown)
            unknown->Release();
    uia_host_shutdown(&host);
}

struct GroupScene {
    UiWidget widgets[16];
    UiLayoutNode layout[16];
    UiWidgetManager manager;
    UiWidgetHandle root, field, rows[3], footer;
};

void describe_grouped(void* context, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&,
                      const UiLayoutNode&, UiAccessWidgetInfo* info) noexcept {
    const auto& scene = *static_cast<const GroupScene*>(context);
    for (u32 index = 0; index < 3; ++index)
        if (handle_equal(handle, scene.rows[index])) {
            info->role = UiAccessRole::list_item;
            info->group = 1;
            info->key = 900 + index;
        }
}

void describe_group_node(void*, u8 group, UiAccessNodeDesc* description) noexcept {
    description->name = group == 1 ? utf8_literal("Rows") : Utf8View{};
}

void test_synthetic_groups(UiAccessTree* tree) noexcept {
    static GroupScene scene{};
    check(ui_widget_manager_initialize(&scene.manager, {scene.widgets, 16}, {scene.layout, 16}), "group scene");
    UiWidgetBuilder build(scene.manager);
    scene.root = build.container(ui_widget_invalid_handle(), {1, utf8_literal("Root")},
                                 fixed(300, 400, UiLayoutFlow::vertical)).handle;
    scene.field = build.button(scene.root, {2, utf8_literal("Search")}, fixed(300, 30)).handle;
    for (u32 index = 0; index < 3; ++index)
        scene.rows[index] = build.button(scene.root, {10 + index, utf8_literal("Row")}, fixed(300, 40)).handle;
    scene.footer = build.button(scene.root, {20, utf8_literal("Next page")}, fixed(300, 30)).handle;
    check(build.ok() && ui_widget_manager_layout(&scene.manager, {300, 400}).ok(), "group scene layout");
    ui_access_tree_reset(tree, utf8_literal("Groups"), {0, 0, 300, 400});
    const UiAccessScope scope{describe_grouped, &scene, kSurface, false, describe_group_node};
    check(ui_access_add_widgets(tree, 0, scene.manager, scope) != kUiAccessNone && ui_access_tree_finish(tree),
          "grouped projection");
    const u32 search = find_widget(*tree, widget_identity(2));
    const u32 next = find_widget(*tree, widget_identity(20));
    const u32 first = find_widget(*tree, 900), last = find_widget(*tree, 902);
    const u32 list = first != kUiAccessNone ? tree->nodes[first].parent : kUiAccessNone;
    check(list != kUiAccessNone && tree->nodes[list].role == UiAccessRole::list &&
              equal(ui_access_name(*tree, list), utf8_literal("Rows")) && tree->nodes[last].parent == list &&
              tree->nodes[list].child_count == 3 && tree->nodes[search].next_sibling == list &&
              tree->nodes[list].next_sibling == next,
          "flat widgets join a synthetic list at their first member's place");
    check(list != kUiAccessNone && tree->nodes[list].rectangle.top == 30.0f &&
              tree->nodes[list].rectangle.bottom == 150.0f && !(tree->nodes[list].state & ui_access_offscreen),
          "synthetic list bounds unite its members");
}

// ---- announcements and throttling ------------------------------------------

void test_announcer() noexcept {
    UiAccessAnnouncer announcer{};
    check(ui_access_announce_admit(&announcer, utf8_literal("Reconnecting"), UiAccessPoliteness::polite, 0),
          "first announcement is admitted");
    check(!ui_access_announce_admit(&announcer, utf8_literal("Reconnecting"), UiAccessPoliteness::polite, 1000),
          "identical text inside the repeat window is dropped");
    check(ui_access_announce_admit(&announcer, utf8_literal("Reconnecting"), UiAccessPoliteness::polite,
                                   kUiAccessAnnounceRepeatMs + 10),
          "identical text after the repeat window is admitted");
    UiAccessAnnouncer burst{};
    check(ui_access_announce_admit(&burst, utf8_literal("1 result"), UiAccessPoliteness::polite, 100) &&
              ui_access_announce_admit(&burst, utf8_literal("2 results"), UiAccessPoliteness::polite, 200) &&
              ui_access_announce_admit(&burst, utf8_literal("3 results"), UiAccessPoliteness::polite, 300) &&
              !ui_access_announce_admit(&burst, utf8_literal("4 results"), UiAccessPoliteness::polite, 400),
          "polite announcements are limited per window");
    check(ui_access_announce_admit(&burst, utf8_literal("Message failed to send"), UiAccessPoliteness::assertive, 450),
          "assertive announcements bypass the polite budget");
    check(!ui_access_announce_admit(&burst, utf8_literal("Message failed to send"), UiAccessPoliteness::assertive,
                                    900),
          "assertive announcements still drop repeats");
    check(ui_access_announce_admit(&burst, utf8_literal("5 results"), UiAccessPoliteness::polite,
                                   100 + kUiAccessAnnounceWindowMs + 1),
          "the polite budget refills");
    check(!ui_access_announce_admit(&burst, {}, UiAccessPoliteness::polite, 5000) && burst.dropped == 2,
          "empty text is ignored and drops are counted");
}

void test_throttle() noexcept {
    UiAccessThrottle throttle{};
    check(ui_access_throttle_frame(&throttle, 1000, false) == UiAccessThrottleDecision::idle && !throttle.builds,
          "no listener: frames cost nothing");
    check(ui_access_throttle_frame(&throttle, 1000, true) == UiAccessThrottleDecision::build,
          "the first listened frame builds");
    u32 builds = 0;
    for (u64 now = 1016; now < 1500; now += 16)
        builds += ui_access_throttle_frame(&throttle, now, true) == UiAccessThrottleDecision::build ? 1u : 0u;
    check(builds >= 4 && builds <= 5 && throttle.coalesced > 20 && throttle.deferred,
          "animation frames coalesce to about ten snapshots a second");
    check(ui_access_throttle_delay(throttle, throttle.last_build_ms + 30) == kUiAccessRebuildIntervalMs - 30,
          "a deferred rebuild is due at the end of the interval");
    ui_access_throttle_mark_urgent(&throttle);
    check(ui_access_throttle_frame(&throttle, throttle.last_build_ms + 1, true) == UiAccessThrottleDecision::build &&
              !throttle.deferred,
          "focus and input changes rebuild immediately");
    check(ui_access_throttle_frame(&throttle, throttle.last_build_ms + 5, true) == UiAccessThrottleDecision::defer &&
              ui_access_throttle_frame(&throttle, throttle.last_build_ms + kUiAccessRebuildIntervalMs, true) ==
                  UiAccessThrottleDecision::build,
          "the trailing frame of an animation is published");
}

void test_text_events(UiAccessTree* tree) noexcept {
    static UiAccessObservation current[32], previous[32];
    UiAccessEventState state{};
    ui_access_event_state_initialize(&state, {current, 32}, {previous, 32});
    auto build = [&](u32 caret) noexcept {
        ui_access_tree_reset(tree, utf8_literal("Events"), {0, 0, 100, 100});
        UiAccessNodeDesc edit{};
        edit.key = 5;
        edit.rectangle = {0, 0, 100, 20};
        edit.role = UiAccessRole::edit;
        edit.name = utf8_literal("Message");
        edit.value = utf8_literal("hello");
        edit.state = ui_access_focused;
        edit.actions = ui_access_action_value | ui_access_action_text;
        edit.placement.selection_anchor = edit.placement.selection_caret = caret;
        ui_access_add(tree, 0, edit);
        return ui_access_tree_finish(tree);
    };
    UiAccessEventPlan plan{};
    check(build(5) && ui_access_plan_events(&state, *tree, &plan) && build(5) &&
              ui_access_plan_events(&state, *tree, &plan) && !plan.property_count,
          "an unchanged caret raises nothing");
    check(build(2) && ui_access_plan_events(&state, *tree, &plan) &&
              has_property(plan, *tree, 5, UiAccessProperty::text_selection),
          "a moved caret raises TextSelectionChanged");
}

// ---- system UI Automation probe ------------------------------------------

struct Probe {
    HWND window;
    UiaProviderHost host;
    HostContext context;
    bool name_ok, child_ok, invoke_ok, created;
    DWORD window_thread;
    HRESULT failure;
};

Probe* g_probe;

LRESULT CALLBACK probe_window(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    LRESULT result = 0;
    if (g_probe && uia_host_window_message(&g_probe->host, message, wparam, lparam, &result))
        return result;
    return DefWindowProcW(window, message, wparam, lparam);
}

DWORD WINAPI probe_client(void* parameter) noexcept {
    auto& probe = *static_cast<Probe*>(parameter);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* automation = nullptr;
    IUIAutomationElement* window = nullptr;
    IUIAutomationCondition* condition = nullptr;
    IUIAutomationElement* button = nullptr;
    IUIAutomationInvokePattern* invoke = nullptr;
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                              reinterpret_cast<void**>(&automation));
    if (SUCCEEDED(hr))
        hr = automation->ElementFromHandle(probe.window, &window);
    BSTR name = nullptr;
    if (SUCCEEDED(hr) && SUCCEEDED(window->get_CurrentName(&name)))
        probe.name_ok = wide_equal(name, L"Synthetic");
    SysFreeString(name);
    if (SUCCEEDED(hr)) {
        VARIANT target{};
        target.vt = VT_BSTR;
        target.bstrVal = SysAllocString(L"User settings");
        hr = automation->CreatePropertyCondition(UIA_NamePropertyId, target, &condition);
        VariantClear(&target);
    }
    if (SUCCEEDED(hr))
        hr = window->FindFirst(TreeScope_Descendants, condition, &button);
    CONTROLTYPEID type = 0;
    if (SUCCEEDED(hr) && button && SUCCEEDED(button->get_CurrentControlType(&type)))
        probe.child_ok = type == UIA_ButtonControlTypeId;
    if (SUCCEEDED(hr) && button)
        hr = button->GetCurrentPatternAs(UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern),
                                         reinterpret_cast<void**>(&invoke));
    if (SUCCEEDED(hr) && invoke)
        probe.invoke_ok = SUCCEEDED(invoke->Invoke());
    probe.failure = hr;
    if (invoke)
        invoke->Release();
    if (button)
        button->Release();
    if (condition)
        condition->Release();
    if (window)
        window->Release();
    if (automation)
        automation->Release();
    CoUninitialize();
    return 0;
}

int run_probe(UiAccessTree* tree) noexcept {
    static Probe probe{};
    probe.context = {tree, {}, 0, 0};
    g_probe = &probe;
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.lpfnWndProc = probe_window;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"MaiCordAccessibilityProbe";
    RegisterClassExW(&type);
    probe.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, type.lpszClassName, L"MaiCord probe",
                                   WS_POPUP, -32000, -32000, 400, 600, nullptr, nullptr, type.hInstance, nullptr);
    probe.window_thread = GetCurrentThreadId();
    if (!probe.window || !uia_host_initialize(&probe.host, probe.window,
                                              {&probe.context, host_tree, host_act, host_scale})) {
        check(false, "probe window and host");
        return 1;
    }
    HANDLE thread = CreateThread(nullptr, 0, probe_client, &probe, 0, nullptr);
    const u64 deadline = GetTickCount64() + 8000;
    while (thread && GetTickCount64() < deadline) {
        const DWORD wait = MsgWaitForMultipleObjects(1, &thread, FALSE, 50, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0)
            break;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    const bool finished = thread && WaitForSingleObject(thread, 0) == WAIT_OBJECT_0;
    check(finished, "UIA probe client finished in time");
    check(probe.host.requests != 0, "UIA probe reached WM_GETOBJECT");
    check(probe.name_ok, "UIA probe reads the root name through UI Automation");
    check(probe.child_ok, "UIA probe finds a descendant button by name");
    check(probe.invoke_ok && probe.context.act.calls == 1 && probe.context.act.action == UiAccessAction::invoke &&
              probe.context.act.thread == probe.window_thread,
          "UIA probe Invoke reaches the provider on the window thread");
    check(probe.context.tree_thread == probe.window_thread, "snapshot reads stay on the window thread");
    check(probe.host.marshaled != 0, "calls from UI Automation threads were marshaled to the window thread");
    uia_host_shutdown(&probe.host);
    DestroyWindow(probe.window);
    UnregisterClassW(type.lpszClassName, type.hInstance);
    if (thread)
        CloseHandle(thread);
    g_probe = nullptr;
    return 0;
}

bool probe_requested() noexcept {
    const wchar_t* line = GetCommandLineW();
    const wchar_t* flag = L"--uia-probe";
    for (; *line; ++line) {
        u32 index = 0;
        while (flag[index] && line[index] == flag[index])
            ++index;
        if (!flag[index])
            return true;
    }
    return false;
}

} // namespace

void run() noexcept {
    static Scene scene{};
    static UiAccessTree tree{};
    check(build_scene(&scene), "synthetic widget scene builds");
    check(ui_access_tree_initialize(&tree, {g_nodes, 256}, {g_text, sizeof(g_text)}, {g_slots, 512}, {g_keys, 512}),
          "accessibility tree initializes");
    if (probe_requested()) {
        check(build_tree(&tree, scene), "probe snapshot");
        run_probe(&tree);
        return;
    }
    test_tree(scene, &tree);
    test_provider(scene, &tree);
    test_events(&tree);
    test_text_units();
    test_text_pattern(&tree);
    test_text_events(&tree);
    test_scroll_and_radio(&tree);
    test_synthetic_groups(&tree);
    test_announcer();
    test_throttle();
    test_overlays();
    test_exhaustion(scene);
}


} // namespace maicord::accessibility_test

extern "C" __declspec(noreturn) void WINAPI maicord_accessibility_entry() noexcept {
    maicord::accessibility_test::run();
    ExitProcess(maicord::accessibility_test::failures ? 1 : 0);
}
