#pragma once

// UI Automation, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/ole.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <ole2.h>
#include <UIAutomation.h>
#endif

// <UIAutomationCoreApi.h>
#define UIA_E_ELEMENTNOTENABLED 0x80040200
#define UIA_E_ELEMENTNOTAVAILABLE 0x80040201
#define UiaAppendRuntimeId 3

typedef int PROPERTYID;
typedef int PATTERNID;
typedef int TEXTATTRIBUTEID;
typedef int CONTROLTYPEID;
typedef void* UIA_HWND;

MAICORD_SYS_TYPES_BEGIN
struct IExpandCollapseProvider;
struct IGridItemProvider;
struct IGridProvider;
struct IInvokeProvider;
struct IRangeValueProvider;
struct IRawElementProviderFragment;
struct IRawElementProviderFragmentRoot;
struct IRawElementProviderSimple;
struct IScrollItemProvider;
struct IScrollProvider;
struct ISelectionItemProvider;
struct ISelectionProvider;
struct ITextProvider;
struct ITextRangeProvider;
struct IToggleProvider;
struct IUIAutomation;
struct IUIAutomationCondition;
struct IUIAutomationElement;
struct IUIAutomationInvokePattern;
struct IValueProvider;
struct UiaRect;

// <UIAutomationCore.h>
enum NavigateDirection {
    NavigateDirection_Parent = 0,
    NavigateDirection_NextSibling = 1,
    NavigateDirection_PreviousSibling = 2,
    NavigateDirection_FirstChild = 3,
    NavigateDirection_LastChild = 4
};
enum ProviderOptions {
    ProviderOptions_ClientSideProvider = 0x1,
    ProviderOptions_ServerSideProvider = 0x2,
    ProviderOptions_NonClientAreaProvider = 0x4,
    ProviderOptions_OverrideProvider = 0x8,
    ProviderOptions_ProviderOwnsSetFocus = 0x10,
    ProviderOptions_UseComThreading = 0x20,
    ProviderOptions_RefuseNonClientSupport = 0x40,
    ProviderOptions_HasNativeIAccessible = 0x80,
    ProviderOptions_UseClientCoordinates = 0x100
};
enum ExpandCollapseState {
    ExpandCollapseState_Collapsed = 0,
    ExpandCollapseState_Expanded = 1,
    ExpandCollapseState_PartiallyExpanded = 2,
    ExpandCollapseState_LeafNode = 3
};
enum ScrollAmount {
    ScrollAmount_LargeDecrement = 0,
    ScrollAmount_SmallDecrement = 1,
    ScrollAmount_NoAmount = 2,
    ScrollAmount_LargeIncrement = 3,
    ScrollAmount_SmallIncrement = 4
};
enum ToggleState {
    ToggleState_Off = 0,
    ToggleState_On = 1,
    ToggleState_Indeterminate = 2
};
enum TextUnit {
    TextUnit_Character = 0,
    TextUnit_Format = 1,
    TextUnit_Word = 2,
    TextUnit_Line = 3,
    TextUnit_Paragraph = 4,
    TextUnit_Page = 5,
    TextUnit_Document = 6
};
enum TextPatternRangeEndpoint {
    TextPatternRangeEndpoint_Start = 0,
    TextPatternRangeEndpoint_End = 1
};
struct UiaRect {
    double left;
    double top;
    double width;
    double height;
};
struct __declspec(uuid("d6dd68d1-86fd-4332-8666-9abedea2d24c")) __declspec(novtable) IRawElementProviderSimple : IUnknown {
    virtual HRESULT __stdcall get_ProviderOptions(enum ProviderOptions* pRetVal) = 0;
    virtual HRESULT __stdcall GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) = 0;
    virtual HRESULT __stdcall GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) = 0;
};
struct __declspec(uuid("620ce2a5-ab8f-40a9-86cb-de3c75599b58")) __declspec(novtable) IRawElementProviderFragmentRoot : IUnknown {
    virtual HRESULT __stdcall ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** pRetVal) = 0;
    virtual HRESULT __stdcall GetFocus(IRawElementProviderFragment** pRetVal) = 0;
};
struct __declspec(uuid("f7063da8-8359-439c-9297-bbc5299a7d87")) __declspec(novtable) IRawElementProviderFragment : IUnknown {
    virtual HRESULT __stdcall Navigate(enum NavigateDirection direction, IRawElementProviderFragment** pRetVal) = 0;
    virtual HRESULT __stdcall GetRuntimeId(SAFEARRAY** pRetVal) = 0;
    virtual HRESULT __stdcall get_BoundingRectangle(struct UiaRect* pRetVal) = 0;
    MAICORD_COM_UNUSED(GetEmbeddedFragmentRoots);
    virtual HRESULT __stdcall SetFocus() = 0;
};
struct __declspec(uuid("d847d3a5-cab0-4a98-8c32-ecb45c59ad24")) __declspec(novtable) IExpandCollapseProvider : IUnknown {
    virtual HRESULT __stdcall Expand() = 0;
    virtual HRESULT __stdcall Collapse() = 0;
    virtual HRESULT __stdcall get_ExpandCollapseState(enum ExpandCollapseState* pRetVal) = 0;
};
struct __declspec(uuid("b17d6187-0907-464b-a168-0ef17a1572b1")) __declspec(novtable) IGridProvider : IUnknown {
    virtual HRESULT __stdcall GetItem(int row, int column, IRawElementProviderSimple** pRetVal) = 0;
    virtual HRESULT __stdcall get_RowCount(int* pRetVal) = 0;
    virtual HRESULT __stdcall get_ColumnCount(int* pRetVal) = 0;
};
struct __declspec(uuid("d02541f1-fb81-4d64-ae32-f520f8a6dbd1")) __declspec(novtable) IGridItemProvider : IUnknown {
    MAICORD_COM_UNUSED(get_Row);
    virtual HRESULT __stdcall get_Column(int* pRetVal) = 0;
    MAICORD_COM_UNUSED(get_RowSpan);
    MAICORD_COM_UNUSED(get_ColumnSpan);
    virtual HRESULT __stdcall get_ContainingGrid(IRawElementProviderSimple** pRetVal) = 0;
};
struct __declspec(uuid("54fcb24b-e18e-47a2-b4d3-eccbe77599a2")) __declspec(novtable) IInvokeProvider : IUnknown {
    virtual HRESULT __stdcall Invoke() = 0;
};
struct __declspec(uuid("36dc7aef-33e6-4691-afe1-2be7274b3d33")) __declspec(novtable) IRangeValueProvider : IUnknown {
    virtual HRESULT __stdcall SetValue(double val) = 0;
    virtual HRESULT __stdcall get_Value(double* pRetVal) = 0;
    MAICORD_COM_UNUSED(get_IsReadOnly);
    virtual HRESULT __stdcall get_Maximum(double* pRetVal) = 0;
};
struct __declspec(uuid("2360c714-4bf1-4b26-ba65-9b21316127eb")) __declspec(novtable) IScrollItemProvider : IUnknown {
    virtual HRESULT __stdcall ScrollIntoView() = 0;
};
struct __declspec(uuid("fb8b03af-3bdf-48d4-bd36-1a65793be168")) __declspec(novtable) ISelectionProvider : IUnknown {
    virtual HRESULT __stdcall GetSelection(SAFEARRAY** pRetVal) = 0;
};
struct __declspec(uuid("b38b8077-1fc3-42a5-8cae-d40c2215055a")) __declspec(novtable) IScrollProvider : IUnknown {
    virtual HRESULT __stdcall Scroll(enum ScrollAmount horizontalAmount, enum ScrollAmount verticalAmount) = 0;
    MAICORD_COM_UNUSED(SetScrollPercent);
    MAICORD_COM_UNUSED(get_HorizontalScrollPercent);
    virtual HRESULT __stdcall get_VerticalScrollPercent(double* pRetVal) = 0;
    MAICORD_COM_UNUSED(get_HorizontalViewSize);
    MAICORD_COM_UNUSED(get_VerticalViewSize);
    MAICORD_COM_UNUSED(get_HorizontallyScrollable);
    virtual HRESULT __stdcall get_VerticallyScrollable(BOOL* pRetVal) = 0;
};
struct __declspec(uuid("2acad808-b2d4-452d-a407-91ff1ad167b2")) __declspec(novtable) ISelectionItemProvider : IUnknown {
    virtual HRESULT __stdcall Select() = 0;
    MAICORD_COM_UNUSED(AddToSelection);
    MAICORD_COM_UNUSED(RemoveFromSelection);
    virtual HRESULT __stdcall get_IsSelected(BOOL* pRetVal) = 0;
    virtual HRESULT __stdcall get_SelectionContainer(IRawElementProviderSimple** pRetVal) = 0;
};
struct __declspec(uuid("56d00bd0-c4f4-433c-a836-1a52a57e0892")) __declspec(novtable) IToggleProvider : IUnknown {
    virtual HRESULT __stdcall Toggle() = 0;
    virtual HRESULT __stdcall get_ToggleState(enum ToggleState* pRetVal) = 0;
};
struct __declspec(uuid("c7935180-6fb3-4201-b174-7df73adbf64a")) __declspec(novtable) IValueProvider : IUnknown {
    virtual HRESULT __stdcall SetValue(LPCWSTR val) = 0;
    virtual HRESULT __stdcall get_Value(BSTR* pRetVal) = 0;
};
struct __declspec(uuid("5347ad7b-c355-46f8-aff5-909033582f63")) __declspec(novtable) ITextRangeProvider : IUnknown {
    virtual HRESULT __stdcall Clone(ITextRangeProvider** pRetVal) = 0;
    virtual HRESULT __stdcall Compare(ITextRangeProvider* range, BOOL* pRetVal) = 0;
    virtual HRESULT __stdcall CompareEndpoints(enum TextPatternRangeEndpoint endpoint, ITextRangeProvider* targetRange,
        enum TextPatternRangeEndpoint targetEndpoint, int* pRetVal) = 0;
    virtual HRESULT __stdcall ExpandToEnclosingUnit(enum TextUnit unit) = 0;
    MAICORD_COM_UNUSED(FindAttribute);
    virtual HRESULT __stdcall FindText(BSTR text, BOOL backward, BOOL ignoreCase, ITextRangeProvider** pRetVal) = 0;
    virtual HRESULT __stdcall GetAttributeValue(TEXTATTRIBUTEID attributeId, VARIANT* pRetVal) = 0;
    virtual HRESULT __stdcall GetBoundingRectangles(SAFEARRAY** pRetVal) = 0;
    virtual HRESULT __stdcall GetEnclosingElement(IRawElementProviderSimple** pRetVal) = 0;
    virtual HRESULT __stdcall GetText(int maxLength, BSTR* pRetVal) = 0;
    virtual HRESULT __stdcall Move(enum TextUnit unit, int count, int* pRetVal) = 0;
    virtual HRESULT __stdcall MoveEndpointByUnit(enum TextPatternRangeEndpoint endpoint, enum TextUnit unit, int count,
        int* pRetVal) = 0;
    virtual HRESULT __stdcall MoveEndpointByRange(enum TextPatternRangeEndpoint endpoint,
        ITextRangeProvider* targetRange, enum TextPatternRangeEndpoint targetEndpoint) = 0;
    virtual HRESULT __stdcall Select() = 0;
    MAICORD_COM_UNUSED(AddToSelection);
    MAICORD_COM_UNUSED(RemoveFromSelection);
    virtual HRESULT __stdcall ScrollIntoView(BOOL alignToTop) = 0;
};
struct __declspec(uuid("3589c92c-63f3-4367-99bb-ada653b77cf2")) __declspec(novtable) ITextProvider : IUnknown {
    virtual HRESULT __stdcall GetSelection(SAFEARRAY** pRetVal) = 0;
    MAICORD_COM_UNUSED(GetVisibleRanges);
    MAICORD_COM_UNUSED(RangeFromChild);
    MAICORD_COM_UNUSED(RangeFromPoint);
    virtual HRESULT __stdcall get_DocumentRange(ITextRangeProvider** pRetVal) = 0;
};
// <UIAutomationClient.h>
enum TreeScope {
    TreeScope_None = 0,
    TreeScope_Element = 0x1,
    TreeScope_Children = 0x2,
    TreeScope_Descendants = 0x4,
    TreeScope_Parent = 0x8,
    TreeScope_Ancestors = 0x10,
    TreeScope_Subtree = ((TreeScope_Element | TreeScope_Children) | TreeScope_Descendants)
};
const long UIA_InvokePatternId = 10000;
const long UIA_SelectionPatternId = 10001;
const long UIA_ValuePatternId = 10002;
const long UIA_RangeValuePatternId = 10003;
const long UIA_ScrollPatternId = 10004;
const long UIA_ExpandCollapsePatternId = 10005;
const long UIA_GridPatternId = 10006;
const long UIA_GridItemPatternId = 10007;
const long UIA_SelectionItemPatternId = 10010;
const long UIA_TextPatternId = 10014;
const long UIA_TogglePatternId = 10015;
const long UIA_ScrollItemPatternId = 10017;
const long UIA_ControlTypePropertyId = 30003;
const long UIA_NamePropertyId = 30005;
const long UIA_HasKeyboardFocusPropertyId = 30008;
const long UIA_IsEnabledPropertyId = 30010;
const long UIA_IsPasswordPropertyId = 30019;
const long UIA_PositionInSetPropertyId = 30152;
const long UIA_SizeOfSetPropertyId = 30153;
const long UIA_FontWeightAttributeId = 40007;
const long UIA_IsReadOnlyAttributeId = 40015;
const long UIA_ButtonControlTypeId = 50000;
const long UIA_EditControlTypeId = 50004;
const long UIA_ListItemControlTypeId = 50007;
const long UIA_ListControlTypeId = 50008;
const long UIA_RadioButtonControlTypeId = 50013;
const long UIA_GroupControlTypeId = 50026;
struct __declspec(uuid("d22108aa-8ac5-49a5-837b-37bbb3d7591e")) __declspec(novtable) IUIAutomationElement : IUnknown {
    virtual HRESULT __stdcall SetFocus() = 0;
    virtual HRESULT __stdcall GetRuntimeId(SAFEARRAY** runtimeId) = 0;
    virtual HRESULT __stdcall FindFirst(enum TreeScope scope, IUIAutomationCondition* condition,
        IUIAutomationElement** found) = 0;
    MAICORD_COM_UNUSED(FindAll);
    MAICORD_COM_UNUSED(FindFirstBuildCache);
    MAICORD_COM_UNUSED(FindAllBuildCache);
    MAICORD_COM_UNUSED(BuildUpdatedCache);
    MAICORD_COM_UNUSED(GetCurrentPropertyValue);
    MAICORD_COM_UNUSED(GetCurrentPropertyValueEx);
    MAICORD_COM_UNUSED(GetCachedPropertyValue);
    MAICORD_COM_UNUSED(GetCachedPropertyValueEx);
    virtual HRESULT __stdcall GetCurrentPatternAs(PATTERNID patternId, const IID& riid, void** patternObject) = 0;
    MAICORD_COM_UNUSED(GetCachedPatternAs);
    MAICORD_COM_UNUSED(GetCurrentPattern);
    MAICORD_COM_UNUSED(GetCachedPattern);
    MAICORD_COM_UNUSED(GetCachedParent);
    MAICORD_COM_UNUSED(GetCachedChildren);
    MAICORD_COM_UNUSED(get_CurrentProcessId);
    virtual HRESULT __stdcall get_CurrentControlType(CONTROLTYPEID* retVal) = 0;
    MAICORD_COM_UNUSED(get_CurrentLocalizedControlType);
    virtual HRESULT __stdcall get_CurrentName(BSTR* retVal) = 0;
};
struct __declspec(uuid("352ffba8-0973-437c-a61f-f64cafd81df9")) __declspec(novtable) IUIAutomationCondition : public IUnknown {
};
struct __declspec(uuid("fb377fbe-8ea6-46d5-9c73-6499642d3059")) __declspec(novtable) IUIAutomationInvokePattern : IUnknown {
    virtual HRESULT __stdcall Invoke() = 0;
};
struct __declspec(uuid("30cbe57d-d9d0-452a-ab13-7ac5ac4825ee")) __declspec(novtable) IUIAutomation : IUnknown {
    MAICORD_COM_UNUSED(CompareElements);
    MAICORD_COM_UNUSED(CompareRuntimeIds);
    MAICORD_COM_UNUSED(GetRootElement);
    virtual HRESULT __stdcall ElementFromHandle(UIA_HWND hwnd, IUIAutomationElement** element) = 0;
    MAICORD_COM_UNUSED(ElementFromPoint);
    MAICORD_COM_UNUSED(GetFocusedElement);
    MAICORD_COM_UNUSED(GetRootElementBuildCache);
    MAICORD_COM_UNUSED(ElementFromHandleBuildCache);
    MAICORD_COM_UNUSED(ElementFromPointBuildCache);
    MAICORD_COM_UNUSED(GetFocusedElementBuildCache);
    MAICORD_COM_UNUSED(CreateTreeWalker);
    MAICORD_COM_UNUSED(get_ControlViewWalker);
    MAICORD_COM_UNUSED(get_ContentViewWalker);
    MAICORD_COM_UNUSED(get_RawViewWalker);
    MAICORD_COM_UNUSED(get_RawViewCondition);
    MAICORD_COM_UNUSED(get_ControlViewCondition);
    MAICORD_COM_UNUSED(get_ContentViewCondition);
    MAICORD_COM_UNUSED(CreateCacheRequest);
    MAICORD_COM_UNUSED(CreateTrueCondition);
    MAICORD_COM_UNUSED(CreateFalseCondition);
    virtual HRESULT __stdcall CreatePropertyCondition(PROPERTYID propertyId, VARIANT value,
        IUIAutomationCondition** newCondition) = 0;
};
class __declspec(uuid("ff48dba4-60ef-4201-aa87-54103eef594e")) CUIAutomation;
MAICORD_SYS_TYPES_END
