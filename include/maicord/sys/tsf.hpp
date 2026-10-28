#pragma once

// Text Services Framework, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/ole.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <msctf.h>
#include <textstor.h>
#include <inputscope.h>
#endif

// <textstor.h>
#define TS_E_INVALIDPOS MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0200)
#define TS_E_NOLOCK MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0201)
#define TS_E_NOOBJECT MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0202)
#define TS_E_NOSELECTION MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0205)
#define TS_E_NOLAYOUT MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0206)
#define TS_E_INVALIDPOINT MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0207)
#define TS_E_SYNCHRONOUS MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0208)
#define TS_S_ASYNC MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_ITF, 0x0300)
#define TS_AS_TEXT_CHANGE ( 0x1 )
#define TS_AS_SEL_CHANGE ( 0x2 )
#define TS_AS_LAYOUT_CHANGE ( 0x4 )
#define TS_AS_ATTR_CHANGE ( 0x8 )
#define TS_AS_STATUS_CHANGE ( 0x10 )
#define TS_AS_ALL_SINKS ( ( ( ( ( TS_AS_TEXT_CHANGE | TS_AS_SEL_CHANGE ) | TS_AS_LAYOUT_CHANGE ) | TS_AS_ATTR_CHANGE ) | TS_AS_STATUS_CHANGE ) )
#define TS_LF_SYNC ( 0x1 )
#define TS_LF_READ ( 0x2 )
#define TS_LF_READWRITE ( 0x6 )
#define TS_SS_TRANSITORY ( 0x4 )
#define TS_SS_NOHIDDENTEXT ( 0x8 )
#define TS_IAS_NOQUERY ( 0x1 )
#define TS_IAS_QUERYONLY ( 0x2 )
#define TS_DEFAULT_SELECTION ( ( ULONG )-1 )
#define GXFPF_ROUND_NEAREST ( 0x1 )
#define GXFPF_NEAREST ( 0x2 )

// <msctf.h>
#define TF_CLIENTID_NULL ((TfClientId)0)
#define TF_INVALID_EDIT_COOKIE ( 0 )
#define TF_POPF_ALL ( 0x1 )
#define TF_ES_SYNC ( 0x1 )
#define TF_ES_READWRITE ( 0x6 )
#define TF_INVALID_COOKIE ( 0xffffffff )

typedef __declspec(uuid("1faf509e-44c1-458e-950a-38a96705a62b")) DWORD TsViewCookie;
typedef __declspec(uuid("7213778c-7bb0-4270-b050-6189ee594e97")) DWORD TfEditCookie;
typedef __declspec(uuid("88a9c478-f3ec-4763-8345-cd9250443f8d")) DWORD TfGuidAtom;
typedef __declspec(uuid("de403c21-89fd-4f85-8b87-64584d063fbc")) DWORD TfClientId;

MAICORD_SYS_TYPES_BEGIN
struct IEnumTfRanges;
struct ITextStoreACP;
struct ITextStoreACPSink;
struct ITfCategoryMgr;
struct ITfCompositionView;
struct ITfContext;
struct ITfContextOwnerCompositionSink;
struct ITfContextView;
struct ITfDisplayAttributeInfo;
struct ITfDisplayAttributeMgr;
struct ITfDocumentMgr;
struct ITfEditRecord;
struct ITfEditSession;
struct ITfInputScope;
struct ITfKeystrokeMgr;
struct ITfProperty;
struct ITfRange;
struct ITfRangeACP;
struct ITfReadOnlyProperty;
struct ITfSource;
struct ITfTextEditSink;
struct ITfThreadMgr;

// <textstor.h>
struct TS_STATUS {
    DWORD dwDynamicFlags;
    DWORD dwStaticFlags;
};
struct TS_TEXTCHANGE {
    LONG acpStart;
    LONG acpOldEnd;
    LONG acpNewEnd;
};
enum __MIDL___MIDL_itf_textstor_0000_0000_0001 {
    TS_AE_START = 1,
    TS_AE_END = 2
};
typedef __MIDL___MIDL_itf_textstor_0000_0000_0001 TsActiveSelEnd;
struct TS_SELECTIONSTYLE {
    TsActiveSelEnd ase;
    BOOL fInterimChar;
};
struct TS_SELECTION_ACP {
    LONG acpStart;
    LONG acpEnd;
    TS_SELECTIONSTYLE style;
};
typedef __declspec(uuid("ef3457d9-8446-49a7-a9e6-b50d9d5f3fd9")) GUID TS_ATTRID;
struct TS_ATTRVAL {
    TS_ATTRID idAttr;
    DWORD dwOverlapId;
    VARIANT varValue;
};
enum __MIDL___MIDL_itf_textstor_0000_0000_0002 {
    TS_LC_CHANGE = 1,
};
typedef __MIDL___MIDL_itf_textstor_0000_0000_0002 TsLayoutCode;
enum __MIDL___MIDL_itf_textstor_0000_0000_0003 {
    TS_RT_PLAIN = 0,
};
typedef __MIDL___MIDL_itf_textstor_0000_0000_0003 TsRunType;
struct TS_RUNINFO {
    ULONG uCount;
    TsRunType type;
};
struct __declspec(uuid("28888fe3-c2a0-483a-a3ea-8cb1ce51ff3d")) __declspec(novtable) ITextStoreACP : IUnknown {
    virtual HRESULT __stdcall AdviseSink(const IID& riid, IUnknown* punk, DWORD dwMask) = 0;
    virtual HRESULT __stdcall UnadviseSink(IUnknown* punk) = 0;
    virtual HRESULT __stdcall RequestLock(DWORD dwLockFlags, HRESULT* phrSession) = 0;
    virtual HRESULT __stdcall GetStatus(TS_STATUS* pdcs) = 0;
    virtual HRESULT __stdcall QueryInsert(LONG acpTestStart, LONG acpTestEnd, ULONG cch, LONG* pacpResultStart,
        LONG* pacpResultEnd) = 0;
    virtual HRESULT __stdcall GetSelection(ULONG ulIndex, ULONG ulCount, TS_SELECTION_ACP* pSelection,
        ULONG* pcFetched) = 0;
    virtual HRESULT __stdcall SetSelection(ULONG ulCount, const TS_SELECTION_ACP* pSelection) = 0;
    virtual HRESULT __stdcall GetText(LONG acpStart, LONG acpEnd, WCHAR* pchPlain, ULONG cchPlainReq,
        ULONG* pcchPlainRet, TS_RUNINFO* prgRunInfo, ULONG cRunInfoReq, ULONG* pcRunInfoRet, LONG* pacpNext) = 0;
    virtual HRESULT __stdcall SetText(DWORD dwFlags, LONG acpStart, LONG acpEnd, const WCHAR* pchText, ULONG cch,
        TS_TEXTCHANGE* pChange) = 0;
    virtual HRESULT __stdcall GetFormattedText(LONG acpStart, LONG acpEnd, IDataObject** ppDataObject) = 0;
    virtual HRESULT __stdcall GetEmbedded(LONG acpPos, const GUID& rguidService, const IID& riid, IUnknown** ppunk) = 0;
    virtual HRESULT __stdcall QueryInsertEmbedded(const GUID* pguidService, const FORMATETC* pFormatEtc,
        BOOL* pfInsertable) = 0;
    virtual HRESULT __stdcall InsertEmbedded(DWORD dwFlags, LONG acpStart, LONG acpEnd, IDataObject* pDataObject,
        TS_TEXTCHANGE* pChange) = 0;
    virtual HRESULT __stdcall InsertTextAtSelection(DWORD dwFlags, const WCHAR* pchText, ULONG cch, LONG* pacpStart,
        LONG* pacpEnd, TS_TEXTCHANGE* pChange) = 0;
    virtual HRESULT __stdcall InsertEmbeddedAtSelection(DWORD dwFlags, IDataObject* pDataObject, LONG* pacpStart,
        LONG* pacpEnd, TS_TEXTCHANGE* pChange) = 0;
    virtual HRESULT __stdcall RequestSupportedAttrs(DWORD dwFlags, ULONG cFilterAttrs,
        const TS_ATTRID* paFilterAttrs) = 0;
    virtual HRESULT __stdcall RequestAttrsAtPosition(LONG acpPos, ULONG cFilterAttrs, const TS_ATTRID* paFilterAttrs,
        DWORD dwFlags) = 0;
    virtual HRESULT __stdcall RequestAttrsTransitioningAtPosition(LONG acpPos, ULONG cFilterAttrs,
        const TS_ATTRID* paFilterAttrs, DWORD dwFlags) = 0;
    virtual HRESULT __stdcall FindNextAttrTransition(LONG acpStart, LONG acpHalt, ULONG cFilterAttrs,
        const TS_ATTRID* paFilterAttrs, DWORD dwFlags, LONG* pacpNext, BOOL* pfFound, LONG* plFoundOffset) = 0;
    virtual HRESULT __stdcall RetrieveRequestedAttrs(ULONG ulCount, TS_ATTRVAL* paAttrVals, ULONG* pcFetched) = 0;
    virtual HRESULT __stdcall GetEndACP(LONG* pacp) = 0;
    virtual HRESULT __stdcall GetActiveView(TsViewCookie* pvcView) = 0;
    virtual HRESULT __stdcall GetACPFromPoint(TsViewCookie vcView, const POINT* ptScreen, DWORD dwFlags,
        LONG* pacp) = 0;
    virtual HRESULT __stdcall GetTextExt(TsViewCookie vcView, LONG acpStart, LONG acpEnd, RECT* prc,
        BOOL* pfClipped) = 0;
    virtual HRESULT __stdcall GetScreenExt(TsViewCookie vcView, RECT* prc) = 0;
    virtual HRESULT __stdcall GetWnd(TsViewCookie vcView, HWND* phwnd) = 0;
};
struct __declspec(uuid("22d44c94-a419-4542-a272-ae26093ececf")) __declspec(novtable) ITextStoreACPSink : IUnknown {
    virtual HRESULT __stdcall OnTextChange(DWORD dwFlags, const TS_TEXTCHANGE* pChange) = 0;
    virtual HRESULT __stdcall OnSelectionChange() = 0;
    virtual HRESULT __stdcall OnLayoutChange(TsLayoutCode lcode, TsViewCookie vcView) = 0;
    virtual HRESULT __stdcall OnStatusChange(DWORD dwFlags) = 0;
    virtual HRESULT __stdcall OnAttrsChange(LONG acpStart, LONG acpEnd, ULONG cAttrs, const TS_ATTRID* paAttrs) = 0;
    virtual HRESULT __stdcall OnLockGranted(DWORD dwLockFlags) = 0;
    virtual HRESULT __stdcall OnStartEditTransaction() = 0;
    virtual HRESULT __stdcall OnEndEditTransaction() = 0;
};
// <msctf.h>
enum __MIDL___MIDL_itf_msctf_0000_0000_0001 {
};
typedef __MIDL___MIDL_itf_msctf_0000_0000_0001 TfAnchor;
struct __declspec(uuid("aa80e801-2021-11d2-93e0-0060b067b86e")) __declspec(novtable) ITfThreadMgr : IUnknown {
    virtual HRESULT __stdcall Activate(TfClientId* ptid) = 0;
    virtual HRESULT __stdcall Deactivate() = 0;
    virtual HRESULT __stdcall CreateDocumentMgr(ITfDocumentMgr** ppdim) = 0;
    MAICORD_COM_UNUSED(EnumDocumentMgrs);
    virtual HRESULT __stdcall GetFocus(ITfDocumentMgr** ppdimFocus) = 0;
    virtual HRESULT __stdcall SetFocus(ITfDocumentMgr* pdimFocus) = 0;
    virtual HRESULT __stdcall AssociateFocus(HWND hwnd, ITfDocumentMgr* pdimNew, ITfDocumentMgr** ppdimPrev) = 0;
};
struct __declspec(uuid("aa80e7f4-2021-11d2-93e0-0060b067b86e")) __declspec(novtable) ITfDocumentMgr : IUnknown {
    virtual HRESULT __stdcall CreateContext(TfClientId tidOwner, DWORD dwFlags, IUnknown* punk, ITfContext** ppic,
        TfEditCookie* pecTextStore) = 0;
    virtual HRESULT __stdcall Push(ITfContext* pic) = 0;
    virtual HRESULT __stdcall Pop(DWORD dwFlags) = 0;
    virtual HRESULT __stdcall GetTop(ITfContext** ppic) = 0;
};
struct __declspec(uuid("D7540241-F9A1-4364-BEFC-DBCD2C4395B7")) __declspec(novtable) ITfCompositionView : IUnknown {
    virtual HRESULT __stdcall GetOwnerClsid(CLSID* pclsid) = 0;
    virtual HRESULT __stdcall GetRange(ITfRange** ppRange) = 0;
};
struct __declspec(uuid("5F20AA40-B57A-4F34-96AB-3576F377CC79")) __declspec(novtable) ITfContextOwnerCompositionSink : IUnknown {
    virtual HRESULT __stdcall OnStartComposition(ITfCompositionView* pComposition, BOOL* pfOk) = 0;
    virtual HRESULT __stdcall OnUpdateComposition(ITfCompositionView* pComposition, ITfRange* pRangeNew) = 0;
    virtual HRESULT __stdcall OnEndComposition(ITfCompositionView* pComposition) = 0;
};
struct __declspec(uuid("2433bf8e-0f9b-435c-ba2c-180611978c30")) __declspec(novtable) ITfContextView : IUnknown {
    MAICORD_COM_UNUSED(GetRangeFromPoint);
    virtual HRESULT __stdcall GetTextExt(TfEditCookie ec, ITfRange* pRange, RECT* prc, BOOL* pfClipped) = 0;
    virtual HRESULT __stdcall GetScreenExt(RECT* prc) = 0;
    virtual HRESULT __stdcall GetWnd(HWND* phwnd) = 0;
};
enum __MIDL_ITfContext_0001 {
};
typedef __MIDL_ITfContext_0001 TfActiveSelEnd;
struct TF_SELECTIONSTYLE {
    TfActiveSelEnd ase;
    BOOL fInterimChar;
};
struct TF_SELECTION {
    ITfRange* range;
    TF_SELECTIONSTYLE style;
};
typedef __declspec(uuid("bc7d979a-846a-444d-afef-0a9bfa82b961")) TS_STATUS TF_STATUS;
struct __declspec(uuid("aa80e7fd-2021-11d2-93e0-0060b067b86e")) __declspec(novtable) ITfContext : IUnknown {
    virtual HRESULT __stdcall RequestEditSession(TfClientId tid, ITfEditSession* pes, DWORD dwFlags,
        HRESULT* phrSession) = 0;
    MAICORD_COM_UNUSED(InWriteSession);
    virtual HRESULT __stdcall GetSelection(TfEditCookie ec, ULONG ulIndex, ULONG ulCount, TF_SELECTION* pSelection,
        ULONG* pcFetched) = 0;
    virtual HRESULT __stdcall SetSelection(TfEditCookie ec, ULONG ulCount, const TF_SELECTION* pSelection) = 0;
    virtual HRESULT __stdcall GetStart(TfEditCookie ec, ITfRange** ppStart) = 0;
    MAICORD_COM_UNUSED(GetEnd);
    virtual HRESULT __stdcall GetActiveView(ITfContextView** ppView) = 0;
    MAICORD_COM_UNUSED(EnumViews);
    virtual HRESULT __stdcall GetStatus(TF_STATUS* pdcs) = 0;
    virtual HRESULT __stdcall GetProperty(const GUID& guidProp, ITfProperty** ppProp) = 0;
};
struct __declspec(uuid("17d49a3d-f8b8-4b2f-b254-52319dd64c53")) __declspec(novtable) ITfReadOnlyProperty : IUnknown {
    MAICORD_COM_UNUSED(GetType);
    virtual HRESULT __stdcall EnumRanges(TfEditCookie ec, IEnumTfRanges** ppEnum, ITfRange* pTargetRange) = 0;
    virtual HRESULT __stdcall GetValue(TfEditCookie ec, ITfRange* pRange, VARIANT* pvarValue) = 0;
    virtual HRESULT __stdcall GetContext(ITfContext** ppContext) = 0;
};
struct __declspec(uuid("42d4d099-7c1a-4a89-b836-6c6f22160df0")) __declspec(novtable) ITfEditRecord : IUnknown {
};
struct __declspec(uuid("8127d409-ccd3-4683-967a-b43d5b482bf7")) __declspec(novtable) ITfTextEditSink : IUnknown {
    virtual HRESULT __stdcall OnEndEdit(ITfContext* pic, TfEditCookie ecReadOnly, ITfEditRecord* pEditRecord) = 0;
};
struct __declspec(uuid("aa80e803-2021-11d2-93e0-0060b067b86e")) __declspec(novtable) ITfEditSession : IUnknown {
    virtual HRESULT __stdcall DoEditSession(TfEditCookie ec) = 0;
};
enum __MIDL_ITfRange_0001 {
};
typedef __MIDL_ITfRange_0001 TfGravity;
enum __MIDL_ITfRange_0002 {
};
typedef __MIDL_ITfRange_0002 TfShiftDir;
struct TF_HALTCOND {
    ITfRange* pHaltRange;
    TfAnchor aHaltPos;
    DWORD dwFlags;
};
struct __declspec(uuid("aa80e7ff-2021-11d2-93e0-0060b067b86e")) __declspec(novtable) ITfRange : IUnknown {
    virtual HRESULT __stdcall GetText(TfEditCookie ec, DWORD dwFlags, WCHAR* pchText, ULONG cchMax, ULONG* pcch) = 0;
    virtual HRESULT __stdcall SetText(TfEditCookie ec, DWORD dwFlags, const WCHAR* pchText, LONG cch) = 0;
    virtual HRESULT __stdcall GetFormattedText(TfEditCookie ec, IDataObject** ppDataObject) = 0;
    virtual HRESULT __stdcall GetEmbedded(TfEditCookie ec, const GUID& rguidService, const IID& riid,
        IUnknown** ppunk) = 0;
    virtual HRESULT __stdcall InsertEmbedded(TfEditCookie ec, DWORD dwFlags, IDataObject* pDataObject) = 0;
    virtual HRESULT __stdcall ShiftStart(TfEditCookie ec, LONG cchReq, LONG* pcch, const TF_HALTCOND* pHalt) = 0;
    virtual HRESULT __stdcall ShiftEnd(TfEditCookie ec, LONG cchReq, LONG* pcch, const TF_HALTCOND* pHalt) = 0;
    virtual HRESULT __stdcall ShiftStartToRange(TfEditCookie ec, ITfRange* pRange, TfAnchor aPos) = 0;
    virtual HRESULT __stdcall ShiftEndToRange(TfEditCookie ec, ITfRange* pRange, TfAnchor aPos) = 0;
    virtual HRESULT __stdcall ShiftStartRegion(TfEditCookie ec, TfShiftDir dir, BOOL* pfNoRegion) = 0;
    virtual HRESULT __stdcall ShiftEndRegion(TfEditCookie ec, TfShiftDir dir, BOOL* pfNoRegion) = 0;
    virtual HRESULT __stdcall IsEmpty(TfEditCookie ec, BOOL* pfEmpty) = 0;
    virtual HRESULT __stdcall Collapse(TfEditCookie ec, TfAnchor aPos) = 0;
    virtual HRESULT __stdcall IsEqualStart(TfEditCookie ec, ITfRange* pWith, TfAnchor aPos, BOOL* pfEqual) = 0;
    virtual HRESULT __stdcall IsEqualEnd(TfEditCookie ec, ITfRange* pWith, TfAnchor aPos, BOOL* pfEqual) = 0;
    virtual HRESULT __stdcall CompareStart(TfEditCookie ec, ITfRange* pWith, TfAnchor aPos, LONG* plResult) = 0;
    virtual HRESULT __stdcall CompareEnd(TfEditCookie ec, ITfRange* pWith, TfAnchor aPos, LONG* plResult) = 0;
    virtual HRESULT __stdcall AdjustForInsert(TfEditCookie ec, ULONG cchInsert, BOOL* pfInsertOk) = 0;
    virtual HRESULT __stdcall GetGravity(TfGravity* pgStart, TfGravity* pgEnd) = 0;
    virtual HRESULT __stdcall SetGravity(TfEditCookie ec, TfGravity gStart, TfGravity gEnd) = 0;
    virtual HRESULT __stdcall Clone(ITfRange** ppClone) = 0;
    virtual HRESULT __stdcall GetContext(ITfContext** ppContext) = 0;
};
struct __declspec(uuid("057a6296-029b-4154-b79a-0d461d4ea94c")) __declspec(novtable) ITfRangeACP : ITfRange {
    virtual HRESULT __stdcall GetExtent(LONG* pacpAnchor, LONG* pcch) = 0;
    virtual HRESULT __stdcall SetExtent(LONG acpAnchor, LONG cch) = 0;
};
struct __declspec(uuid("f99d3f40-8e32-11d2-bf46-00105a2799b5")) __declspec(novtable) IEnumTfRanges : IUnknown {
    virtual HRESULT __stdcall Clone(IEnumTfRanges** ppEnum) = 0;
    virtual HRESULT __stdcall Next(ULONG ulCount, ITfRange** ppRange, ULONG* pcFetched) = 0;
    virtual HRESULT __stdcall Reset() = 0;
};
struct __declspec(uuid("e2449660-9542-11d2-bf46-00105a2799b5")) __declspec(novtable) ITfProperty : ITfReadOnlyProperty {
    MAICORD_COM_UNUSED(FindRange);
    MAICORD_COM_UNUSED(SetValueStore);
    virtual HRESULT __stdcall SetValue(TfEditCookie ec, ITfRange* pRange, const VARIANT* pvarValue) = 0;
    virtual HRESULT __stdcall Clear(TfEditCookie ec, ITfRange* pRange) = 0;
};
struct __declspec(uuid("aa80e7f0-2021-11d2-93e0-0060b067b86e")) __declspec(novtable) ITfKeystrokeMgr : IUnknown {
    MAICORD_COM_UNUSED(AdviseKeyEventSink);
    MAICORD_COM_UNUSED(UnadviseKeyEventSink);
    MAICORD_COM_UNUSED(GetForeground);
    virtual HRESULT __stdcall TestKeyDown(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) = 0;
    virtual HRESULT __stdcall TestKeyUp(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) = 0;
    virtual HRESULT __stdcall KeyDown(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) = 0;
    virtual HRESULT __stdcall KeyUp(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) = 0;
};
enum __MIDL___MIDL_itf_msctf_0000_0070_0001 {
    TF_LS_SOLID = 1,
    TF_LS_DOT = 2,
    TF_LS_DASH = 3,
    TF_LS_SQUIGGLE = 4
};
typedef __MIDL___MIDL_itf_msctf_0000_0070_0001 TF_DA_LINESTYLE;
enum __MIDL___MIDL_itf_msctf_0000_0070_0002 {
    TF_CT_SYSCOLOR = 1,
    TF_CT_COLORREF = 2
};
typedef __MIDL___MIDL_itf_msctf_0000_0070_0002 TF_DA_COLORTYPE;
struct TF_DA_COLOR {
    TF_DA_COLORTYPE type;
    union {
        int nIndex;
        COLORREF cr;
    };
};
enum __MIDL___MIDL_itf_msctf_0000_0070_0004 {
    TF_ATTR_INPUT = 0,
    TF_ATTR_TARGET_CONVERTED = 1,
    TF_ATTR_CONVERTED = 2,
    TF_ATTR_TARGET_NOTCONVERTED = 3,
    TF_ATTR_INPUT_ERROR = 4,
    TF_ATTR_FIXEDCONVERTED = 5,
    TF_ATTR_OTHER = - 1
};
typedef __MIDL___MIDL_itf_msctf_0000_0070_0004 TF_DA_ATTR_INFO;
struct TF_DISPLAYATTRIBUTE {
    TF_DA_COLOR crText;
    TF_DA_COLOR crBk;
    TF_DA_LINESTYLE lsStyle;
    BOOL fBoldLine;
    TF_DA_COLOR crLine;
    TF_DA_ATTR_INFO bAttr;
};
struct __declspec(uuid("70528852-2f26-4aea-8c96-215150578932")) __declspec(novtable) ITfDisplayAttributeInfo : IUnknown {
    virtual HRESULT __stdcall GetGUID(GUID* pguid) = 0;
    MAICORD_COM_UNUSED(GetDescription);
    virtual HRESULT __stdcall GetAttributeInfo(TF_DISPLAYATTRIBUTE* pda) = 0;
    MAICORD_COM_UNUSED(SetAttributeInfo);
    virtual HRESULT __stdcall Reset() = 0;
};
struct __declspec(uuid("8ded7393-5db1-475c-9e71-a39111b0ff67")) __declspec(novtable) ITfDisplayAttributeMgr : IUnknown {
    MAICORD_COM_UNUSED(OnUpdateInfo);
    MAICORD_COM_UNUSED(EnumDisplayAttributeInfo);
    virtual HRESULT __stdcall GetDisplayAttributeInfo(const GUID& guid, ITfDisplayAttributeInfo** ppInfo,
        CLSID* pclsidOwner) = 0;
};
struct __declspec(uuid("c3acefb5-f69d-4905-938f-fcadcf4be830")) __declspec(novtable) ITfCategoryMgr : IUnknown {
    MAICORD_COM_UNUSED(RegisterCategory);
    MAICORD_COM_UNUSED(UnregisterCategory);
    MAICORD_COM_UNUSED(EnumCategoriesInItem);
    MAICORD_COM_UNUSED(EnumItemsInCategory);
    MAICORD_COM_UNUSED(FindClosestCategory);
    MAICORD_COM_UNUSED(RegisterGUIDDescription);
    MAICORD_COM_UNUSED(UnregisterGUIDDescription);
    MAICORD_COM_UNUSED(GetGUIDDescription);
    MAICORD_COM_UNUSED(RegisterGUIDDWORD);
    MAICORD_COM_UNUSED(UnregisterGUIDDWORD);
    MAICORD_COM_UNUSED(GetGUIDDWORD);
    virtual HRESULT __stdcall RegisterGUID(const GUID& rguid, TfGuidAtom* pguidatom) = 0;
    virtual HRESULT __stdcall GetGUID(TfGuidAtom guidatom, GUID* pguid) = 0;
};
struct __declspec(uuid("4ea48a35-60ae-446f-8fd6-e6a8d82459f7")) __declspec(novtable) ITfSource : IUnknown {
    virtual HRESULT __stdcall AdviseSink(const IID& riid, IUnknown* punk, DWORD* pdwCookie) = 0;
    virtual HRESULT __stdcall UnadviseSink(DWORD dwCookie) = 0;
};
// <inputscope.h>
enum __MIDL___MIDL_itf_inputscope_0000_0000_0001 {
    IS_DEFAULT = 0,
    IS_LOGINNAME = 6,
    IS_DIGITS = 28,
    IS_PASSWORD = 31,
    IS_PHRASELIST = - 1,
    IS_REGULAREXPRESSION = - 2,
    IS_SRGS = - 3,
    IS_XML = - 4,
    IS_ENUMSTRING = - 5
};
typedef __MIDL___MIDL_itf_inputscope_0000_0000_0001 InputScope;
struct __declspec(uuid("fde1eaee-6924-4cdf-91e7-da38cff5559d")) __declspec(novtable) ITfInputScope : IUnknown {
    virtual HRESULT __stdcall GetInputScopes(InputScope** pprgInputScopes, UINT* pcCount) = 0;
    virtual HRESULT __stdcall GetPhrase(BSTR** ppbstrPhrases, UINT* pcCount) = 0;
    virtual HRESULT __stdcall GetRegularExpression(BSTR* pbstrRegExp) = 0;
    virtual HRESULT __stdcall GetSRGS(BSTR* pbstrSRGS) = 0;
    virtual HRESULT __stdcall GetXML(BSTR* pbstrXML) = 0;
};
MAICORD_SYS_TYPES_END
