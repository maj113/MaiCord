#include "maicord/ui/secure_text_store.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/tsf.hpp"

namespace maicord {
namespace {
constexpr GUID attribute_property{0x34b45670,0x7526,0x11d2,{0xa1,0x47,0x00,0x10,0x5a,0x27,0x99,0xb5}};
constexpr GUID synthetic_attribute{0x0cf91d11,0x4247,0x46cf,{0x92,0x29,0x18,0xf9,0xb8,0x6a,0x72,0x17}};
bool identity(REFIID a, REFIID b) noexcept { return bytes_equal(&a,&b,sizeof(GUID)); }
struct View final : ITfCompositionView {
    ULONG references = 1;
    ITfRange* range = nullptr;
    SecureTextStore* unbind_during_read = nullptr;
    SecureEdit* replacement_during_read = nullptr;
    bool release_during_read = false;
    bool replacement_accepted = false;
    SecureTextStore* release_on_release = nullptr;
    bool released_owner = false;
    u32 range_reads = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!identity(id,__uuidof(IUnknown)) && !identity(id,__uuidof(ITfCompositionView))) return E_NOINTERFACE;
        *output = static_cast<ITfCompositionView*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references;
        auto* store = release_on_release;
        release_on_release = nullptr;
        if (store) { released_owner = true; secure_text_store_release(store); }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetOwnerClsid(CLSID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRange(ITfRange** output) override {
        if (!output) return E_POINTER;
        ++range_reads;
        auto* unbind = unbind_during_read;
        unbind_during_read = nullptr;
        if (unbind) {
            if (release_during_read) secure_text_store_release(unbind);
            else if (replacement_during_read) replacement_accepted = secure_text_store_bind(
                unbind,replacement_during_read,SecureTextInputScope::digits);
            else secure_text_store_unbind(unbind);
        }
        *output = range;
        if (!range) return E_FAIL;
        range->AddRef(); return S_OK;
    }
};
struct Session final : ITfEditSession {
    ULONG references = 1;
    ITfContext* context = nullptr;
    ITfContextOwnerCompositionSink* owner = nullptr;
    View view;
    TfGuidAtom attribute = 0;
    u32 operation = 0;
    bool accepted = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!identity(id,__uuidof(IUnknown)) && !identity(id,__uuidof(ITfEditSession))) return E_NOINTERFACE;
        *output = static_cast<ITfEditSession*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
        if (operation == 4) return owner->OnEndComposition(&view);
        if (!view.range) {
            HRESULT result = context->GetStart(cookie, &view.range);
            if (FAILED(result) || !view.range) return E_FAIL;
            ITfRangeACP* acp = nullptr;
            result = view.range->QueryInterface(__uuidof(ITfRangeACP), reinterpret_cast<void**>(&acp));
            if (SUCCEEDED(result) && acp) result = acp->SetExtent(0,4);
            com_release(acp);
            if (FAILED(result)) return result;
        }
        ITfProperty* property = nullptr;
        // TSF need not notify for a write session with no modifications. A
        // supported attribute set/clear transaction forces a genuine callback
        // while leaving its final composition projection exactly identical.
        HRESULT result = context->GetProperty(attribute_property,&property);
        if (FAILED(result) || !property) return E_FAIL;
        if (operation == 1) {
            VARIANT value{}; value.vt = VT_I4; value.lVal = static_cast<LONG>(attribute);
            result = property->SetValue(cookie,view.range,&value);
            if (SUCCEEDED(result)) result = property->Clear(cookie,view.range);
            property->Release(); return result;
        }
        if (operation == 0 || operation == 2) {
            VARIANT value{}; value.vt = VT_I4; value.lVal = static_cast<LONG>(attribute);
            result = property->SetValue(cookie,view.range,&value);
            if (SUCCEEDED(result) && operation == 0) {
                BOOL allow = FALSE;
                result = owner->OnStartComposition(&view,&allow);
                accepted = allow != FALSE;
                // Change attributes after the extent notification, without an
                // OnUpdateComposition call or a text/selection mutation.
                if (SUCCEEDED(result) && accepted) result = property->Clear(cookie,view.range);
                else result = E_FAIL;
            }
        } else result = property->Clear(cookie,view.range);
        property->Release(); return result;
    }
};
bool bounds(void*,SecureTextStoreGeometry* value) noexcept { *value={{0,0,200,40},{0,0,200,40}}; return true; }
struct Notifications { u32 count = 0; SecureTextStore* release = nullptr; bool released = false; };
void changed(void* opaque) noexcept {
    auto& notification = *static_cast<Notifications*>(opaque);
    ++notification.count;
    if (notification.release) {
        auto* store = notification.release;
        notification.release = nullptr;
        notification.released = true;
        secure_text_store_release(store);
    }
}
struct AsyncSink final : ITextStoreACPSink {
    ULONG references = 1;
    SecureTextStore* store = nullptr;
    SecureEdit* replacement = nullptr;
    u32 grants = 0;
    bool queued = false, rebound = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!identity(id,__uuidof(IUnknown)) && !identity(id,__uuidof(ITextStoreACPSink))) return E_NOINTERFACE;
        *output = static_cast<ITextStoreACPSink*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE OnLockGranted(DWORD) override {
        if (++grants != 1) return E_FAIL;
        auto* acp = static_cast<ITextStoreACP*>(secure_text_store_test_interface(store));
        HRESULT session = E_FAIL;
        queued = acp->RequestLock(TS_LF_READWRITE,&session) == S_OK && session == TS_S_ASYNC;
        rebound = secure_text_store_bind(store,replacement,SecureTextInputScope::digits);
        return queued && rebound ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE OnTextChange(DWORD,const TS_TEXTCHANGE*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnLayoutChange(TsLayoutCode,TsViewCookie) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStatusChange(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnAttrsChange(LONG,LONG,ULONG,const TS_ATTRID*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStartEditTransaction() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnEndEditTransaction() override { return S_OK; }
};
void test_deferred_async_lock(void (*check)(bool,const char*) noexcept) noexcept {
    SecureTextStore store{}; SecureEdit edit{}, next{}; AsyncSink sink;
    const bool ready = secure_edit_initialize(&edit,32,false) && secure_edit_initialize(&next,32,false) &&
        secure_text_store_initialize(&store,{reinterpret_cast<HWND>(1)}) &&
        secure_text_store_bind(&store,&edit,SecureTextInputScope::text);
    check(ready,"TSF deferred async-lock fixture initialized");
    if (ready) {
        sink.store = &store; sink.replacement = &next;
        auto* acp = static_cast<ITextStoreACP*>(secure_text_store_test_interface(&store));
        HRESULT session = E_FAIL;
        check(acp->AdviseSink(__uuidof(ITextStoreACPSink),&sink,TS_AS_ALL_SINKS) == S_OK &&
            acp->RequestLock(TS_LF_SYNC|TS_LF_READ,&session) == S_OK && session == S_OK &&
            sink.queued && sink.rebound && sink.grants == 1,
            "TSF deferred rebind cancels queued old-context write grant before it touches replacement history");
        secure_edit_release(&edit);
        check(secure_text_store_process_deferred() && sink.references == 1,
            "TSF deferred async-lock cleanup drains and releases old sink exactly once");
    }
    secure_text_store_release(&store); secure_edit_release(&next); secure_edit_release(&edit);
}
}

void test_tsf_end_edit(void (*check)(bool,const char*) noexcept) noexcept {
    test_deferred_async_lock(check);
    using Initialize = HRESULT(WINAPI*)(void*,DWORD);
    using Uninitialize = void(WINAPI*)();
    using GetManager = HRESULT(WINAPI*)(ITfThreadMgr**);
    using CreateCategory = HRESULT(WINAPI*)(ITfCategoryMgr**);
    HMODULE ole = LoadLibraryExW(L"ole32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto initialize = ole ? reinterpret_cast<Initialize>(GetProcAddress(ole,"CoInitializeEx")) : nullptr;
    const auto uninitialize = ole ? reinterpret_cast<Uninitialize>(GetProcAddress(ole,"CoUninitialize")) : nullptr;
    const HRESULT initialized = initialize && uninitialize ? initialize(nullptr,COINIT_APARTMENTTHREADED) : E_FAIL;
    check(SUCCEEDED(initialized),"TSF end-edit owned COM apartment initialized");
    HWND window = CreateWindowExW(0,L"STATIC",L"MaiCord TSF fixture",WS_POPUP,0,0,200,40,nullptr,nullptr,nullptr,nullptr);
    SecureEdit edit{}, replacement{}; SecureTextStore store{}; Notifications notifications;
    u32& changes = notifications.count;
    const bool ready = SUCCEEDED(initialized) && window && secure_edit_initialize(&edit,32,false) &&
        secure_edit_initialize(&replacement,32,false) && secure_edit_assign(&edit,utf8_literal("abcd")) ==
        SecureEditResult::changed && secure_text_store_initialize(&store,{window,bounds,changed,&notifications}) &&
        secure_text_store_bind(&store,&edit,SecureTextInputScope::text) && secure_text_store_attach(&store);
    check(ready,"TSF end-edit genuine hidden-window context attached");
    ITfThreadMgr* manager = nullptr; ITfDocumentMgr* document = nullptr; ITfContext* context = nullptr;
    ITfCategoryMgr* category = nullptr; ITfContextOwnerCompositionSink* owner = nullptr; ITfTextEditSink* end_edit = nullptr;
    Session session;
    if (ready) {
        HMODULE tsf = GetModuleHandleW(L"msctf.dll");
        const auto get_manager = reinterpret_cast<GetManager>(GetProcAddress(tsf,"TF_GetThreadMgr"));
        const auto create_category = reinterpret_cast<CreateCategory>(GetProcAddress(tsf,"TF_CreateCategoryMgr"));
        bool found = get_manager && create_category && SUCCEEDED(get_manager(&manager)) && manager;
        // Obtain and immediately restore only this fixture window's association.
        if (found) {
            found = SUCCEEDED(manager->AssociateFocus(window,nullptr,&document)) && document;
            ITfDocumentMgr* removed = nullptr;
            if (document) found = SUCCEEDED(manager->AssociateFocus(window,document,&removed)) && found;
            com_release(removed);
        }
        found = found && SUCCEEDED(document->GetTop(&context)) && context &&
            SUCCEEDED(create_category(&category)) && category &&
            SUCCEEDED(category->RegisterGUID(synthetic_attribute,&session.attribute));
        auto* acp = static_cast<ITextStoreACP*>(secure_text_store_test_interface(&store));
        found = found && acp->QueryInterface(__uuidof(ITfContextOwnerCompositionSink),reinterpret_cast<void**>(&owner)) == S_OK &&
            acp->QueryInterface(__uuidof(ITfTextEditSink),reinterpret_cast<void**>(&end_edit)) == S_OK;
        check(found,"TSF end-edit genuine context and sink interfaces available");
        if (found) {
            TfClientId client = TF_CLIENTID_NULL;
            const HRESULT activated = manager->Activate(&client);
            check(SUCCEEDED(activated),"TSF end-edit fixture edit client activated");
            if (SUCCEEDED(activated)) {
                session.context = context; session.owner = owner;
                const u32 text_revision = edit.revision;
                u32 first_revision = 0;
                u32 first_notifications = 0;
                for (u32 operation = 0; operation < 4; ++operation) {
                    const auto prior_reads = session.view.range_reads;
                    session.operation = operation; HRESULT result = E_FAIL;
                    check(context->RequestEditSession(client,&session,TF_ES_SYNC|TF_ES_READWRITE,&result) == S_OK && result == S_OK,
                        "TSF end-edit real read-write session completes");
                    SecureTextComposition projection{};
                    check(secure_text_store_composition(store,&projection) && projection.active && projection.start == 0 && projection.end == 4,
                        "TSF end-edit keeps the unchanged composition extent");
                    const bool complete = operation != 2;
                    check(projection.complete == complete && projection.span_count == 1 && edit.revision == text_revision,
                        "TSF end-edit refreshes property-only completion without mutating model text");
                    if (operation == 0) {
                        first_revision = projection.revision;
                        first_notifications = changes;
                        check(changes > 0 && changes <= 2,"TSF composition-start and end-edit refresh notifications coalesce under one lock");
                    } else check(changes == first_notifications + (operation > 1 ? operation - 1 : 0),
                        "TSF end-edit property changes notify once and identical sessions do not notify");
                    if (operation == 1) check(projection.revision == first_revision,"TSF end-edit identical properties preserve revision");
                    if (operation == 1) check(session.view.range_reads > prior_reads,
                        "TSF identical composition properties were refreshed through a genuine end-edit callback");
                }
                check(session.accepted && session.view.references == 2,"TSF active composition view retained once");
                const auto prior_notifications = changes;
                session.operation = 2;
                session.view.unbind_during_read = &store;
                HRESULT reentered = E_FAIL;
                check(context->RequestEditSession(client,&session,TF_ES_SYNC|TF_ES_READWRITE,&reentered) == S_OK && reentered == S_OK,
                    "TSF end-edit real GetRange callback can reenter unbind");
                SecureTextComposition ended{};
                check(secure_text_store_composition(store,&ended) && !ended.active,
                    "TSF reentrant unbind cannot publish stale spans");
                check(changes == prior_notifications,"TSF reentrant unbind cannot publish stale notifications");
                check(!session.view.unbind_during_read && session.view.references == 1,
                    "TSF genuine end-edit callback consumes unbind hook and releases captured view");
                check(secure_text_store_process_deferred(),"TSF deferred unbind drains outside native edit-session traversal");
                check(secure_edit_assign(&replacement,utf8_literal("wxyz")) == SecureEditResult::changed,
                    "TSF replacement target populated");
                check(secure_text_store_bind(&store,&replacement,SecureTextInputScope::digits) && session.view.references == 1,
                    "TSF context replacement unadvises and releases current composition view");
                const auto prior = changes;
                check(end_edit->OnEndEdit(context,0,nullptr) == S_OK && changes == prior,
                    "TSF stale context callback cannot republish an ended composition");
                for (u32 action = 0; action < 2; ++action) {
                    com_release(session.view.range);
                    com_release(context);
                    check(document->GetTop(&context) == S_OK && context,"TSF replacement context is installed");
                    session.context = context;
                    session.operation = 0;
                    HRESULT start = E_FAIL;
                    check(context->RequestEditSession(client,&session,TF_ES_SYNC|TF_ES_READWRITE,&start) == S_OK && start == S_OK,
                        "TSF replacement composition starts through genuine native session");
                    session.view.replacement_during_read = action == 0 ? &edit : nullptr;
                    session.view.release_during_read = action == 1;
                    session.view.unbind_during_read = &store;
                    const auto before = changes;
                    session.operation = 2;
                    HRESULT finish = E_FAIL;
                    check(context->RequestEditSession(client,&session,TF_ES_SYNC|TF_ES_READWRITE,&finish) == S_OK && finish == S_OK,
                        "TSF native end-edit callback can logically rebind or release its public owner");
                    check(!session.view.unbind_during_read && session.view.references == 1 && changes == before,
                        "TSF queued replacement/release cannot publish old composition spans or notifications");
                    if (action == 0) {
                        check(session.view.replacement_accepted,"TSF reentrant replacement target is accepted immediately");
                        secure_edit_release(&replacement);
                    } else {
                        check(!store.implementation,"TSF public owner clears immediately during native callback");
                        secure_edit_release(&edit);
                    }
                    check(secure_text_store_process_deferred(),"TSF deferred replacement/release drains after native call returned");
                }
                manager->Deactivate();
                for (u32 finish_kind = 0; finish_kind < 2; ++finish_kind) {
                // Separate owned documents isolate the production callback
                // path: config.changed is emitted by RequestLock after its
                // model lock ends, while native OnLockGranted is still active.
                com_release(session.view.range); com_release(context); com_release(document);
                com_release(end_edit); com_release(owner);
                const bool notification_ready = secure_edit_initialize(&edit,32,false) &&
                    secure_edit_assign(&edit,utf8_literal("abcd")) == SecureEditResult::changed &&
                    secure_text_store_initialize(&store,{window,bounds,changed,&notifications}) &&
                    secure_text_store_bind(&store,&edit,SecureTextInputScope::text) && secure_text_store_attach(&store);
                check(notification_ready,"TSF RequestLock notification-release fixture attached");
                if (notification_ready) {
                    bool associated = SUCCEEDED(manager->AssociateFocus(window,nullptr,&document)) && document;
                    ITfDocumentMgr* removed = nullptr;
                    if (document) associated = SUCCEEDED(manager->AssociateFocus(window,document,&removed)) && associated;
                    com_release(removed);
                    auto* active_acp = static_cast<ITextStoreACP*>(secure_text_store_test_interface(&store));
                    associated = associated && document->GetTop(&context) == S_OK && context &&
                        active_acp->QueryInterface(__uuidof(ITfContextOwnerCompositionSink),reinterpret_cast<void**>(&owner)) == S_OK;
                    check(associated,"TSF notification-release genuine document retrieved");
                    if (associated) {
                        const bool extra = SUCCEEDED(manager->Activate(&client));
                        check(extra,"TSF notification-release edit client activated");
                        if (extra) {
                            session.context = context; session.owner = owner;
                            session.view.release_during_read = false;
                            session.view.replacement_during_read = nullptr;
                            session.operation = 0;
                            HRESULT start = E_FAIL;
                            check(context->RequestEditSession(client,&session,TF_ES_SYNC|TF_ES_READWRITE,&start) == S_OK && start == S_OK,
                                "TSF notification-release composition starts");
                            const auto before = changes;
                            notifications.released = false;
                            session.view.released_owner = false;
                            if (finish_kind == 0) notifications.release = &store;
                            else session.view.release_on_release = &store;
                            session.operation = finish_kind == 0 ? 2 : 4;
                            HRESULT finish = E_FAIL;
                            check(context->RequestEditSession(client,&session,TF_ES_SYNC|TF_ES_READWRITE,&finish) == S_OK && finish == S_OK,
                                "TSF RequestLock post-lock application notification can release public owner");
                            check(!store.implementation && session.view.references == 1 &&
                                (finish_kind == 0 ? notifications.released && changes == before + 1 :
                                 session.view.released_owner && changes == before),
                                "TSF notification/view-release clears logical owner without extra stale notifications");
                            secure_edit_release(&edit);
                            check(secure_text_store_process_deferred(),"TSF notification release drains after RequestEditSession returns");
                            manager->Deactivate();
                        }
                    }
                }
                }
            }
        }
    }
    com_release(session.view.range); com_release(end_edit); com_release(owner);
    com_release(category); com_release(context); com_release(document); com_release(manager);
    secure_text_store_release(&store); secure_text_store_release(&store);
    check(session.references == 1 && session.view.references == 1,"TSF fixture references balance after teardown");
    secure_edit_release(&replacement); secure_edit_release(&edit);
    if (window) DestroyWindow(window);
    if (SUCCEEDED(initialized)) uninitialize();
    if (ole) FreeLibrary(ole);
}
} // namespace maicord
