#include "tsf_composition_fixture.hpp"
#include "maicord/ui/secure_text_store.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/sys/tsf.hpp"

namespace maicord {
namespace {
bool range_geometry(void*, u32 first, u32 last, RECT* output, bool* clipped) noexcept {
    *output = {static_cast<LONG>(first + 120), 240, static_cast<LONG>(last + 120), 260};
    *clipped = false;
    return true;
}
using tsf_test::Composition;
struct WriteSink final : ITextStoreACPSink {
    ULONG references = 1;
    ITextStoreACP* store;
    ITfContextOwnerCompositionSink* owner;
    Composition* composition;
    u32 phase = 0;
    HRESULT (*point_check)(ITextStoreACP*) noexcept = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!bytes_equal(&id, &__uuidof(IUnknown), sizeof(GUID)) &&
            !bytes_equal(&id, &__uuidof(ITextStoreACPSink), sizeof(GUID))) return E_NOINTERFACE;
        *output = static_cast<ITextStoreACPSink*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE OnLockGranted(DWORD) override {
        if (point_check) return point_check(store);
        RECT rectangle{};
        BOOL clipped = TRUE;
        if (store->GetTextExt(1, 0, 1, &rectangle, &clipped) != S_OK || clipped ||
            rectangle.left != 120 || rectangle.right != 121 || rectangle.bottom != 260) return E_FAIL;
        TS_TEXTCHANGE change{};
        if (phase++ == 0) {
            const WCHAR first[] = {L'k'};
            const auto inserted = store->InsertTextAtSelection(TS_IAS_NOQUERY, first, 1, nullptr, nullptr, &change);
            if (FAILED(inserted)) return inserted;
            composition->range.start = 1; composition->range.length = 1;
            BOOL accepted = FALSE;
            owner->OnStartComposition(composition, &accepted);
            return accepted ? S_OK : E_FAIL;
        }
        const WCHAR final[] = {0x304B};
        const auto replaced = store->SetText(0, 1, 2, final, 1, &change);
        if (FAILED(replaced)) return replaced;
        return owner->OnEndComposition(composition);
    }
    HRESULT STDMETHODCALLTYPE OnTextChange(DWORD, const TS_TEXTCHANGE*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnLayoutChange(TsLayoutCode, TsViewCookie) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStatusChange(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnAttrsChange(LONG, LONG, ULONG, const TS_ATTRID*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStartEditTransaction() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnEndEditTransaction() override { return S_OK; }
};
bool point_bounds(void*, SecureTextStoreGeometry* out) noexcept {
    *out = {{100, 200, 160, 220}, {80, 180, 180, 240}}; return true;
}
bool point_hit(void*, POINT point, bool round, u32* position, bool* inside) noexcept {
    if (point.y == 999) return false;
    *inside = point.x >= 100 && point.x < 120 && point.y >= 200 && point.y < 220;
    *position = (round || !*inside) && point.x >= 110 ? 2 : 0;
    if (point.y == 998) *position = 100;
    return true;
}
HRESULT point_queries(ITextStoreACP* acp) noexcept {
    struct Case {POINT point; DWORD flags; HRESULT status; LONG position;};
    const Case cases[]{
        {{115,210},0,S_OK,0},{{115,210},GXFPF_NEAREST,S_OK,0},
        {{115,210},GXFPF_ROUND_NEAREST,S_OK,2},{{105,210},GXFPF_ROUND_NEAREST,S_OK,0},
        {{115,210},GXFPF_NEAREST|GXFPF_ROUND_NEAREST,S_OK,2},
        {{150,210},0,TS_E_INVALIDPOINT,0},{{150,210},GXFPF_ROUND_NEAREST,TS_E_INVALIDPOINT,0},
        {{150,210},GXFPF_NEAREST,S_OK,2},{{180,210},0,TS_E_INVALIDPOINT,0},
        {{180,210},GXFPF_NEAREST,S_OK,2},{{90,210},GXFPF_NEAREST,S_OK,0},
        {{115,999},GXFPF_NEAREST,TS_E_NOLAYOUT,0},{{115,998},GXFPF_NEAREST,E_FAIL,0}};
    for (const auto& test : cases) {
        LONG position = -7;
        if (acp->GetACPFromPoint(1,&test.point,test.flags,&position) != test.status ||
            position != (test.status == S_OK ? test.position : -7)) return E_FAIL;
    }
    POINT point{115,210}; LONG position = -7;
    return acp->GetACPFromPoint(2,&point,0,&position) == E_INVALIDARG &&
        acp->GetACPFromPoint(1,nullptr,0,&position) == E_INVALIDARG &&
        acp->GetACPFromPoint(1,&point,0,nullptr) == E_INVALIDARG ? S_OK : E_FAIL;
}
void test_point_queries(void (*check)(bool,const char*) noexcept) noexcept {
    SecureEdit edit{}; SecureTextStore store{};
    const bool ready = secure_edit_initialize(&edit,32,false) &&
        secure_text_store_initialize(&store,{reinterpret_cast<HWND>(1),point_bounds,nullptr,nullptr,nullptr,point_hit}) &&
        secure_text_store_bind(&store,&edit,SecureTextInputScope::text);
    check(ready,"TSF point fixture");
    if (ready) {
        secure_edit_assign(&edit,utf8_literal("\xf0\x9f\x98\x80" "ab"));
        auto* acp=static_cast<ITextStoreACP*>(secure_text_store_test_interface(&store));
        WriteSink sink{}; sink.store=acp; sink.point_check=point_queries;
        POINT point{115,210}; LONG position=-7; HRESULT session=E_FAIL;
        check(acp->GetACPFromPoint(1,&point,0,&position)==TS_E_NOLOCK && position==-7,"TSF point requires read lock");
        check(acp->AdviseSink(__uuidof(ITextStoreACPSink),&sink,TS_AS_ALL_SINKS)==S_OK &&
            acp->RequestLock(TS_LF_SYNC|TS_LF_READ,&session)==S_OK && session==S_OK,
            "TSF point maps containing and nearest UTF16 positions, rejects outside and stale layout");
        SecureEditUtf16Selection selection{};
        check(secure_edit_utf16_selection(edit,&selection)==SecureEditResult::unchanged && selection.caret==4,
            "TSF point queries preserve selection and do not return current caret");
        acp->UnadviseSink(&sink);
    }
    secure_text_store_release(&store);secure_edit_release(&edit);
}
}
void test_text_composition_history(void (*check)(bool, const char*) noexcept) noexcept {
    test_point_queries(check);
    SecureEdit edit{}, second{};
    SecureTextStore store{};
    const bool initialized = secure_edit_initialize(&edit, 32, false) && secure_edit_enable_history(&edit, 64) &&
        secure_edit_initialize(&second, 32, false) && secure_edit_enable_history(&second, 64) &&
        secure_text_store_initialize(&store, {reinterpret_cast<HWND>(1), nullptr, nullptr, nullptr, range_geometry}) &&
        secure_text_store_bind(&store, &edit, SecureTextInputScope::text);
    check(initialized, "composition history fixture initializes");
    auto* acp = static_cast<ITextStoreACP*>(secure_text_store_test_interface(&store));
    ITfContextOwnerCompositionSink* sink = nullptr;
    if (initialized) acp->QueryInterface(__uuidof(ITfContextOwnerCompositionSink), reinterpret_cast<void**>(&sink));
    check(sink != nullptr, "composition history exposes real adapter sink");
    if (sink) {
        Composition composition{};
        BOOL accepted = FALSE;
        secure_edit_assign(&edit, utf8_literal("a"));
        composition.range.length = 1;
        sink->OnStartComposition(&composition, &accepted);
        check(accepted != FALSE, "composition start begins history group");
        const u16 first[] = {'k'}, final[] = {0x304B};
        SecureEditUtf16Change change{};
        secure_edit_replace_utf16(&edit, 0, 1, {first, 1}, &change);
        sink->OnUpdateComposition(&composition, nullptr);
        secure_edit_replace_utf16(&edit, 0, 1, {final, 1}, &change);
        check(!secure_edit_can_undo(edit), "composition intermediate edits are not replayable");
        sink->OnEndComposition(&composition);
        check(secure_edit_history_costs(edit).undo_steps == 1 && secure_edit_undo(&edit) == SecureEditResult::changed &&
            edit.storage.bytes == 1 && edit.storage.data[0] == 'a' && secure_edit_redo(&edit) == SecureEditResult::changed &&
            edit.storage.bytes == 3 && edit.storage.data[0] == 0xE3, "composition end commits one native undo unit");
        sink->OnStartComposition(&composition, &accepted);
        secure_edit_replace_utf16(&edit, 0, 1, {first, 1}, &change);
        check(secure_text_store_bind(&store, &second, SecureTextInputScope::text) && secure_edit_can_undo(edit) &&
            secure_edit_undo(&edit) == SecureEditResult::changed && edit.storage.bytes == 3 && !secure_edit_can_undo(second),
            "composition rebind finishes old history without contaminating new field");
        sink->OnStartComposition(&composition, &accepted);
        check(!accepted, "composition invalid range rejects start");
        composition.range.length = 0;
        sink->OnStartComposition(&composition, &accepted);
        secure_edit_insert_scalar(&second, 'Q');
        secure_text_store_unbind(&store);
        check(accepted && secure_edit_undo(&second) == SecureEditResult::changed && second.storage.bytes == 0,
            "composition unbind closes active undo unit");
        secure_text_store_bind(&store, &edit, SecureTextInputScope::text);
        secure_edit_assign(&edit, utf8_literal("a"));
        WriteSink writer{}; writer.store = acp; writer.owner = sink; writer.composition = &composition;
        HRESULT session = E_FAIL;
        check(acp->AdviseSink(__uuidof(ITextStoreACPSink), &writer, TS_AS_ALL_SINKS) == S_OK &&
            acp->RequestLock(TS_LF_SYNC | TS_LF_READWRITE, &session) == S_OK && session == S_OK &&
            !secure_edit_can_undo(edit) && edit.storage.bytes == 2,
            "composition initial ACP insertion before start remains in open group");
        check(acp->RequestLock(TS_LF_SYNC | TS_LF_READWRITE, &session) == S_OK && session == S_OK &&
            secure_edit_history_costs(edit).undo_steps == 1 && secure_edit_undo(&edit) == SecureEditResult::changed &&
            edit.storage.bytes == 1 && edit.storage.data[0] == 'a' && secure_edit_redo(&edit) == SecureEditResult::changed &&
            edit.storage.bytes == 4, "composition spans write locks and undoes the initial insertion too");
        check(acp->UnadviseSink(&writer) == S_OK && writer.references == 1, "composition native write sink released");
        check(composition.references == 1 && composition.range.references == 1, "composition fixture references balance");
        MessageComposer composer{};
        char text[64]{};
        check(message_composer_initialize(&composer, {text, sizeof(text)}), "composer native fixture initializes");
        message_composer_reset(&composer, 9, false);
        message_composer_insert_utf8(&composer, utf8_literal("a"));
        check(secure_text_store_bind_target(&store, message_composer_input_target(&composer), SecureTextInputScope::text),
            "native adapter binds the actual composer model");
        writer.phase = 0;
        check(acp->AdviseSink(__uuidof(ITextStoreACPSink), &writer, TS_AS_ALL_SINKS) == S_OK &&
            acp->RequestLock(TS_LF_SYNC | TS_LF_READWRITE, &session) == S_OK && session == S_OK &&
            composer.bytes == 2 && message_composer_undo(&composer) == MessageComposerResult::unchanged,
            "composer native precomposition insertion stays grouped");
        check(acp->RequestLock(TS_LF_SYNC | TS_LF_READWRITE, &session) == S_OK && session == S_OK &&
            composer.bytes == 4 && message_composer_undo(&composer) == MessageComposerResult::changed &&
            composer.bytes == 1 && text[0] == 'a' && message_composer_redo(&composer) == MessageComposerResult::changed &&
            composer.bytes == 4, "composer native conversion commits one reversible IME edit");
        acp->UnadviseSink(&writer);
        secure_text_store_unbind(&store);
        message_composer_release(&composer);
        sink->Release();
    }
    secure_text_store_release(&store);
    secure_edit_release(&edit); secure_edit_release(&second);
}
}
