#pragma once
#include "maicord/core/memory.hpp"
#include "maicord/sys/tsf.hpp"
namespace maicord::tsf_test {
// The adapter only queries ACP extent for this unattached fixture. Every other
// range operation rejects use, so unexpected dependencies cannot pass silently.
struct Range final : ITfRangeACP {
    ULONG references = 1;
    LONG start = 0, length = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!bytes_equal(&id, &__uuidof(IUnknown), sizeof(GUID)) &&
            !bytes_equal(&id, &__uuidof(ITfRange), sizeof(GUID)) &&
            !bytes_equal(&id, &__uuidof(ITfRangeACP), sizeof(GUID))) return E_NOINTERFACE;
        *output = static_cast<ITfRangeACP*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE GetExtent(LONG* anchor, LONG* count) override { *anchor = start; *count = length; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetExtent(LONG anchor, LONG count) override { start = anchor; length = count; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetText(TfEditCookie, DWORD, WCHAR*, ULONG, ULONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetText(TfEditCookie, DWORD, const WCHAR*, LONG) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFormattedText(TfEditCookie, IDataObject**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetEmbedded(TfEditCookie, REFGUID, REFIID, IUnknown**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE InsertEmbedded(TfEditCookie, DWORD, IDataObject*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftStart(TfEditCookie, LONG, LONG*, const TF_HALTCOND*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftEnd(TfEditCookie, LONG, LONG*, const TF_HALTCOND*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftStartToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftEndToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftStartRegion(TfEditCookie, TfShiftDir, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftEndRegion(TfEditCookie, TfShiftDir, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsEmpty(TfEditCookie, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Collapse(TfEditCookie, TfAnchor) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsEqualStart(TfEditCookie, ITfRange*, TfAnchor, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsEqualEnd(TfEditCookie, ITfRange*, TfAnchor, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CompareStart(TfEditCookie, ITfRange*, TfAnchor, LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CompareEnd(TfEditCookie, ITfRange*, TfAnchor, LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AdjustForInsert(TfEditCookie, ULONG, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetGravity(TfGravity*, TfGravity*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetGravity(TfEditCookie, TfGravity, TfGravity) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Clone(ITfRange**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetContext(ITfContext**) override { return E_NOTIMPL; }
};
struct Composition final : ITfCompositionView {
    ULONG references = 1;
    Range range;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!bytes_equal(&id, &__uuidof(IUnknown), sizeof(GUID)) &&
            !bytes_equal(&id, &__uuidof(ITfCompositionView), sizeof(GUID))) return E_NOINTERFACE;
        *output = static_cast<ITfCompositionView*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE GetOwnerClsid(CLSID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRange(ITfRange** output) override { *output = &range; range.AddRef(); return S_OK; }
};
}
