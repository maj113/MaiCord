#include "accessibility_internal.hpp"
#include "../../../src/platform/uia/provider.cpp"
#include "../../../src/platform/uia/provider_patterns.cpp"
#include "../../../src/platform/uia/provider_text.cpp"
#include "maicord/ui/core/widget_builder.hpp"
#include "../../support/test_output.hpp"

namespace maicord::accessibility_test {
namespace {

u32 failures;

} // namespace

namespace accessibility_detail {

void check(bool valid, const char* name) noexcept {
    test_check(&failures, valid, name);
}

bool equal(Utf8View text, Utf8View expected) noexcept {
    return utf8_equal(text, expected);
}

} // namespace accessibility_detail

namespace {

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
                                          fixed(100, 16), {100, 16}).handle;
    }
    scene->settings = build.button(scene->root, {40, utf8_literal("UserSettings")}, fixed(100, 20)).handle;
    scene->label = build.text(scene->root, {41, utf8_literal("LoginLabel")}, fixed(100, 16), {100, 16}).handle;
    scene->field = build.text_field(scene->root, {42, utf8_literal("LoginField")}, fixed(200, 24)).handle;
    UiLayoutStyle hidden = fixed(50, 20);
    hidden.flags = ui_layout_node_none;
    scene->hidden = build.button(scene->root, {43, utf8_literal("Hidden")}, hidden).handle;
    scene->footer = build.container(scene->root, {44, utf8_literal("Footer")}, fixed(200, 20)).handle;
    scene->disabled = build.widget(scene->root, {45, utf8_literal("Submit")}, UiWidgetKind::button,
                                   ui_widget_hit_test, fixed(80, 20)).handle;
    return build.ok() && ui_widget_manager_layout_rectangle(&scene->manager, {0, 0, 400, 600}).ok() &&
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

} // namespace

namespace accessibility_detail {

bool build_tree(UiAccessTree* tree, const Scene& scene, bool with_row0) noexcept {
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

} // namespace accessibility_detail

namespace {

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

} // namespace

namespace accessibility_detail {

const UiAccessTree* host_tree(void* context) noexcept {
    auto* host = static_cast<HostContext*>(context);
    host->tree_thread = GetCurrentThreadId();
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

// New IRawElementProviderSimple reference for key (0 = root), as IUnknown*; the caller releases it.
// A null or disconnected host is refused.
IRawElementProviderSimple* element(UiaProviderHost* host, u64 key) noexcept {
    return static_cast<IRawElementProviderSimple*>(element_create(host, key, key == 0));
}

} // namespace accessibility_detail

namespace {

bool name_is(IRawElementProviderSimple* simple, const wchar_t* expected) noexcept {
    VARIANT value{};
    if (!simple || FAILED(simple->GetPropertyValue(UIA_NamePropertyId, &value)) || value.vt != VT_BSTR)
        return false;
    const bool matched = wide_equal(value.bstrVal, expected);
    VariantClear(&value);
    return matched;
}

} // namespace

namespace accessibility_detail {

int int_property(IRawElementProviderSimple* simple, PROPERTYID id) noexcept {
    VARIANT value{};
    if (!simple || FAILED(simple->GetPropertyValue(id, &value)))
        return -1;
    const int result = value.vt == VT_I4 ? value.lVal : value.vt == VT_BOOL ? (value.boolVal ? 1 : 0) : -1;
    VariantClear(&value);
    return result;
}

} // namespace accessibility_detail

namespace {

IRawElementProviderSimple* simple_of(IRawElementProviderFragment* fragment) noexcept {
    IRawElementProviderSimple* simple = nullptr;
    if (fragment)
        fragment->QueryInterface(__uuidof(IRawElementProviderSimple), reinterpret_cast<void**>(&simple));
    return simple;
}

void test_provider(const Scene& scene, UiAccessTree* tree) noexcept {
    check(build_tree(tree, scene), "provider snapshot");
    HostContext context{tree, {}, 0};
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
    check(disabled_invoke && disabled_invoke->Invoke() == static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED) && context.act.calls == calls &&
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
    check(stale && stale->GetPropertyValue(UIA_NamePropertyId, &stale_value) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE),
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
    check(host.elements == 0 && !element(&host, 0), "shutdown disconnects the root");
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
    HostContext context{tree, {}, 0};
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

    HostContext context{tree, {}, 0};
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
    check(build.ok() && ui_widget_manager_layout_rectangle(&scene.manager, {0, 0, 300, 400}).ok(), "group scene layout");
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
    bool name_ok, child_ok, invoke_ok;
    DWORD window_thread;
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
    probe.context = {tree, {}, 0};
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
    check(probe.name_ok, "UIA probe reads the root name through UI Automation");
    check(probe.child_ok, "UIA probe finds a descendant button by name");
    check(probe.invoke_ok && probe.context.act.calls == 1 && probe.context.act.action == UiAccessAction::invoke &&
              probe.context.act.thread == probe.window_thread,
          "UIA probe Invoke reaches the provider on the window thread");
    check(probe.context.tree_thread == probe.window_thread, "snapshot reads stay on the window thread");
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

