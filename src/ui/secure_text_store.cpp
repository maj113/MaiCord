#include "maicord/ui/secure_text_store.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/sys/gdi.hpp"
#include "maicord/sys/tsf.hpp"
#include "maicord/sys/crt.hpp"
#include <stddef.h>

namespace maicord {
static_assert(sizeof(Win32Rect) == sizeof(RECT) && alignof(Win32Rect) == alignof(RECT) &&
              offsetof(Win32Rect, top) == offsetof(RECT, top) &&
              offsetof(Win32Rect, right) == offsetof(RECT, right) &&
              offsetof(Win32Rect, bottom) == offsetof(RECT, bottom));
namespace {

inline constexpr TsViewCookie kSecureTextView = 1;

using TfCreateThreadMgrFunction = HRESULT(WINAPI*)(ITfThreadMgr** manager);
using TfCreateCategoryMgrFunction = HRESULT(WINAPI*)(ITfCategoryMgr** manager);
using TfCreateDisplayAttributeMgrFunction = HRESULT(WINAPI*)(ITfDisplayAttributeMgr** manager);

inline constexpr GUID kDisplayAttributeProperty{0x34b45670,
                                                0x7526,
                                                0x11d2,
                                                {0xa1, 0x47, 0x00, 0x10, 0x5a, 0x27, 0x99, 0xb5}};

bool guid_equal(REFGUID left, REFGUID right) noexcept {
    if (left.Data1 != right.Data1 || left.Data2 != right.Data2 || left.Data3 != right.Data3) {
        return false;
    }
    for (u32 index = 0; index < 8; ++index) {
        if (left.Data4[index] != right.Data4[index]) {
            return false;
        }
    }
    return true;
}

HRESULT edit_result_hresult(TextInputResult result) noexcept {
    switch (result) {
    case TextInputResult::unchanged:
    case TextInputResult::changed:
        return S_OK;
    case TextInputResult::capacity:
        return E_FAIL;
    case TextInputResult::invalid_utf16:
        return TS_E_INVALIDPOS;
    default:
        return E_INVALIDARG;
    }
}

InputScope native_scope(SecureTextInputScope scope) noexcept {
    switch (scope) {
    case SecureTextInputScope::login_name:
        return IS_LOGINNAME;
    case SecureTextInputScope::digits:
        return IS_DIGITS;
    default:
        return IS_DEFAULT;
    }
}

bool point_in_rectangle(const POINT& point, const RECT& rectangle) noexcept {
    return point.x >= rectangle.left && point.x < rectangle.right && point.y >= rectangle.top &&
           point.y < rectangle.bottom;
}

TextCompositionLineStyle composition_line_style(TF_DA_LINESTYLE style) noexcept {
    switch (style) {
    case TF_LS_SOLID:
        return TextCompositionLineStyle::solid;
    case TF_LS_DOT:
        return TextCompositionLineStyle::dot;
    case TF_LS_DASH:
        return TextCompositionLineStyle::dash;
    case TF_LS_SQUIGGLE:
        return TextCompositionLineStyle::squiggle;
    default:
        return TextCompositionLineStyle::none;
    }
}

TextCompositionAttribute composition_attribute(TF_DA_ATTR_INFO attribute) noexcept {
    switch (attribute) {
    case TF_ATTR_INPUT:
        return TextCompositionAttribute::input;
    case TF_ATTR_TARGET_CONVERTED:
        return TextCompositionAttribute::target_converted;
    case TF_ATTR_CONVERTED:
        return TextCompositionAttribute::converted;
    case TF_ATTR_TARGET_NOTCONVERTED:
        return TextCompositionAttribute::target_not_converted;
    case TF_ATTR_INPUT_ERROR:
        return TextCompositionAttribute::input_error;
    case TF_ATTR_FIXEDCONVERTED:
        return TextCompositionAttribute::fixed_converted;
    default:
        return TextCompositionAttribute::other;
    }
}

u32 composition_rgb(const TF_DA_COLOR& color, bool* present) noexcept {
    if (!present) {
        return 0;
    }
    COLORREF native = 0;
    if (color.type == TF_CT_COLORREF) {
        native = color.cr;
    } else if (color.type == TF_CT_SYSCOLOR && color.nIndex >= 0) {
        native = GetSysColor(color.nIndex);
    } else {
        *present = false;
        return 0;
    }
    *present = true;
    return (static_cast<u32>(GetRValue(native)) << 16) |
           (static_cast<u32>(GetGValue(native)) << 8) | static_cast<u32>(GetBValue(native));
}

TextCompositionSpan default_composition_span(u32 start, u32 end) noexcept {
    return TextCompositionSpan{start,
                               end,
                               0,
                               0,
                               0,
                               TextCompositionLineStyle::solid,
                               TextCompositionAttribute::input,
                               text_composition_color_none,
                               false};
}

class SecureTextStoreImplementation;
SecureTextStoreImplementation* deferred_head = nullptr;
inline constexpr u8 deferred_reset = 1;
inline constexpr u8 deferred_detach = 2;
inline constexpr u8 deferred_attach = 4;

class SecureTextStoreImplementation final : public ITextStoreACP,
                                            public ITfInputScope,
                                            public ITfContextOwnerCompositionSink,
                                            public ITfTextEditSink {
  public:
    LONG references = 1;
    SecureTextStoreConfig config{};
    TextInputTarget edit{};
    SecureTextInputScope scope = SecureTextInputScope::text;
    IUnknown* sink_identity = nullptr;
    ITextStoreACPSink* sink = nullptr;
    DWORD sink_mask = 0;
    DWORD lock_flags = 0;
    DWORD pending_lock_flags = 0;
    bool pending_layout = false;
    bool application_change_pending = false;
    bool manager_activated = false;
    bool context_pushed = false;
    bool focus_associated = false;
    bool detach_after_key = false;
    bool history_group_owned = false;
    u32 key_depth = 0;
    u32 observed_revision = 0;
    u32 observed_length = 0;
    TextInputSelection observed_selection{};
    SecureTextComposition composition{};
    ITfCompositionView* composition_view = nullptr;
    DWORD text_edit_sink_cookie = TF_INVALID_COOKIE;
    SecureTextStoreImplementation* deferred_next = nullptr;
    u32 callback_depth = 0;
    u8 deferred_operations = 0;
    bool deferred_queued = false;
    bool cleanup_active = false;
    HMODULE msctf_module = nullptr;
    ITfThreadMgr* thread_manager = nullptr;
    ITfKeystrokeMgr* keystroke_manager = nullptr;
    ITfDocumentMgr* document_manager = nullptr;
    ITfDocumentMgr* previous_document_manager = nullptr;
    ITfContext* context = nullptr;
    ITfCategoryMgr* category_manager = nullptr;
    ITfDisplayAttributeMgr* display_attribute_manager = nullptr;
    TfClientId client_id = TF_CLIENTID_NULL;
    TfEditCookie edit_cookie = TF_INVALID_EDIT_COOKIE;

    class CallbackScope final {
        SecureTextStoreImplementation* owner;
        bool previous_cleanup;
      public:
        explicit CallbackScope(SecureTextStoreImplementation* value, bool cleanup = false) noexcept
            : owner(value), previous_cleanup(value->cleanup_active) {
            owner->AddRef();
            ++owner->callback_depth;
            if (cleanup) owner->cleanup_active = true;
        }
        ~CallbackScope() noexcept {
            owner->cleanup_active = previous_cleanup;
            --owner->callback_depth;
            owner->Release();
        }
    };

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** output) override {
        if (!output) {
            return E_POINTER;
        }
        *output = nullptr;
        if (guid_equal(riid, __uuidof(IUnknown)) || guid_equal(riid, __uuidof(ITextStoreACP))) {
            *output = static_cast<ITextStoreACP*>(this);
        } else if (guid_equal(riid, __uuidof(ITfInputScope))) {
            *output = static_cast<ITfInputScope*>(this);
        } else if (guid_equal(riid, __uuidof(ITfContextOwnerCompositionSink))) {
            *output = static_cast<ITfContextOwnerCompositionSink*>(this);
        } else if (guid_equal(riid, __uuidof(ITfTextEditSink))) {
            *output = static_cast<ITfTextEditSink*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = InterlockedDecrement(&references);
        if (remaining == 0) {
            this->~SecureTextStoreImplementation();
            VirtualFree(this, 0, MEM_RELEASE);
        }
        return static_cast<ULONG>(remaining);
    }

    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID riid, IUnknown* unknown, DWORD mask) override {
        if (!unknown) {
            return E_INVALIDARG;
        }
        IUnknown* identity = nullptr;
        HRESULT result = unknown->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&identity));
        if (FAILED(result)) {
            return result;
        }
        if (identity == sink_identity) {
            sink_mask = mask & TS_AS_ALL_SINKS;
            identity->Release();
            return S_OK;
        }
        if (sink_identity) {
            identity->Release();
            return CONNECT_E_ADVISELIMIT;
        }
        if (!guid_equal(riid, __uuidof(ITextStoreACPSink))) {
            identity->Release();
            return E_INVALIDARG;
        }
        ITextStoreACPSink* advised = nullptr;
        result = unknown->QueryInterface(__uuidof(ITextStoreACPSink), reinterpret_cast<void**>(&advised));
        if (FAILED(result)) {
            identity->Release();
            return result;
        }
        sink_identity = identity;
        sink = advised;
        sink_mask = mask & TS_AS_ALL_SINKS;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UnadviseSink(IUnknown* unknown) override {
        if (!unknown) {
            return E_INVALIDARG;
        }
        IUnknown* identity = nullptr;
        const HRESULT result =
            unknown->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&identity));
        if (FAILED(result)) {
            return result;
        }
        const bool matches = identity == sink_identity;
        identity->Release();
        if (!matches) {
            return CONNECT_E_NOCONNECTION;
        }
        clear_sink();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RequestLock(DWORD requested, HRESULT* session) override {
        CallbackScope callback(this);
        if (!session) {
            return E_INVALIDARG;
        }
        *session = E_FAIL;
        const DWORD type = requested & TS_LF_READWRITE;
        if (!sink || deferred_operations || cleanup_active || (context && text_edit_sink_cookie == TF_INVALID_COOKIE) || (type != TS_LF_READ && type != TS_LF_READWRITE) ||
            (requested & ~(TS_LF_SYNC | TS_LF_READWRITE)) != 0) {
            return E_INVALIDARG;
        }
        if (lock_flags != 0) {
            if ((requested & TS_LF_SYNC) != 0) {
                *session = TS_E_SYNCHRONOUS;
                return S_OK;
            }
            if (lock_flags == TS_LF_READ && type == TS_LF_READWRITE && pending_lock_flags == 0) {
                pending_lock_flags = TS_LF_READWRITE;
                *session = TS_S_ASYNC;
                return S_OK;
            }
            return E_FAIL;
        }

        grant_lock(requested);
        *session = last_session_result;
        while (pending_lock_flags != 0) {
            if (deferred_operations || cleanup_active) {
                pending_lock_flags = 0;
                break;
            }
            const DWORD pending = pending_lock_flags;
            pending_lock_flags = 0;
            grant_lock(pending);
        }
        flush_layout();
        if (application_change_pending) {
            application_change_pending = false;
            if (config.changed) {
                config.changed(config.context);
            }
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetStatus(TS_STATUS* status) override {
        if (!status) {
            return E_INVALIDARG;
        }
        status->dwDynamicFlags = 0;
        status->dwStaticFlags = TS_SS_TRANSITORY;
        if (!edit || text_input_ready(edit)) {
            status->dwStaticFlags |= TS_SS_NOHIDDENTEXT;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInsert(LONG start, LONG end, ULONG, LONG* result_start,
                                          LONG* result_end) override {
        if (!result_start || !result_end || start < 0 || end < start) {
            return E_INVALIDARG;
        }
        u32 length = 0;
        if (!document_length(&length) || static_cast<u32>(end) > length) {
            return E_INVALIDARG;
        }
        *result_start = start;
        *result_end = end;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSelection(ULONG index, ULONG count, TS_SELECTION_ACP* selection,
                                           ULONG* fetched) override {
        if (!selection || !fetched || count == 0) {
            return E_INVALIDARG;
        }
        *fetched = 0;
        if (!has_read_lock()) {
            return TS_E_NOLOCK;
        }
        if (index == TS_DEFAULT_SELECTION) {
            index = 0;
        }
        if (index != 0 || !edit) {
            return index == 0 ? TS_E_NOSELECTION : E_INVALIDARG;
        }
        TextInputSelection value{};
        if (text_input_selection(edit, &value) != TextInputResult::unchanged) {
            return E_FAIL;
        }
        selection[0].acpStart = static_cast<LONG>(minimum(value.anchor, value.caret));
        selection[0].acpEnd = static_cast<LONG>(maximum(value.anchor, value.caret));
        selection[0].style.ase =
            value.anchor != value.caret && value.caret < value.anchor ? TS_AE_START : TS_AE_END;
        selection[0].style.fInterimChar = FALSE;
        *fetched = 1;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSelection(ULONG count, const TS_SELECTION_ACP* selection) override {
        if (!selection || count != 1) {
            return E_INVALIDARG;
        }
        if (!has_write_lock()) {
            return TS_E_NOLOCK;
        }
        if (!edit || selection[0].acpStart < 0 || selection[0].acpEnd < selection[0].acpStart) {
            return TS_E_INVALIDPOS;
        }
        const u32 start = static_cast<u32>(selection[0].acpStart);
        const u32 end = static_cast<u32>(selection[0].acpEnd);
        const TextInputSelection value = selection[0].style.ase == TS_AE_START
                                                   ? TextInputSelection{end, start}
                                                   : TextInputSelection{start, end};
        const TextInputResult result = text_input_select(edit, value);
        if (result == TextInputResult::changed || result == TextInputResult::unchanged) {
            observe();
        }
        return edit_result_hresult(result);
    }

    HRESULT STDMETHODCALLTYPE GetText(LONG start, LONG end, WCHAR* plain, ULONG plain_capacity,
                                      ULONG* plain_count, TS_RUNINFO* runs, ULONG run_capacity,
                                      ULONG* run_count, LONG* next) override {
        if (!plain_count || !run_count || !next || (!plain && plain_capacity != 0) ||
            (!runs && run_capacity != 0) || start < 0) {
            return E_INVALIDARG;
        }
        *plain_count = 0;
        *run_count = 0;
        *next = start;
        if (!has_read_lock()) {
            return TS_E_NOLOCK;
        }
        u32 length = 0;
        if (!document_length(&length) || static_cast<u32>(start) > length) {
            return TS_E_INVALIDPOS;
        }
        const u32 bounded_end = end == -1 ? length : static_cast<u32>(end);
        if (end < -1 || bounded_end < static_cast<u32>(start) || bounded_end > length) {
            return TS_E_INVALIDPOS;
        }
        const u32 requested = bounded_end - static_cast<u32>(start);
        u32 returned = requested;
        if (plain_capacity != 0) {
            u32 copied = 0;
            u32 copied_next = static_cast<u32>(start);
            const TextInputResult result = text_input_copy(
                edit, static_cast<u32>(start), bounded_end,
                Slice<u16>{reinterpret_cast<u16*>(plain), plain_capacity}, &copied, &copied_next);
            if (result != TextInputResult::unchanged) {
                return edit_result_hresult(result);
            }
            *plain_count = copied;
            returned = copied_next - static_cast<u32>(start);
        } else if (plain) {
            return E_INVALIDARG;
        }
        if (run_capacity != 0 && returned != 0) {
            runs[0].uCount = returned;
            runs[0].type = TS_RT_PLAIN;
            *run_count = 1;
        }
        *next = start + static_cast<LONG>((plain_capacity != 0 || run_capacity != 0) ? returned : 0);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetText(DWORD, LONG start, LONG end, const WCHAR* text, ULONG count,
                                      TS_TEXTCHANGE* change) override {
        if ((!text && count != 0) || !change || start < 0 || end < start) {
            return E_INVALIDARG;
        }
        if (!has_write_lock()) {
            return TS_E_NOLOCK;
        }
        return replace(static_cast<u32>(start), static_cast<u32>(end), text, count, change);
    }

    HRESULT STDMETHODCALLTYPE GetFormattedText(LONG, LONG, IDataObject** output) override {
        if (output) {
            *output = nullptr;
        }
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetEmbedded(LONG, REFGUID, REFIID, IUnknown** output) override {
        if (!output) {
            return E_INVALIDARG;
        }
        *output = nullptr;
        return TS_E_NOOBJECT;
    }

    HRESULT STDMETHODCALLTYPE QueryInsertEmbedded(const GUID*, const FORMATETC*,
                                                  BOOL* insertable) override {
        if (!insertable) {
            return E_INVALIDARG;
        }
        *insertable = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE InsertEmbedded(DWORD, LONG, LONG, IDataObject*, TS_TEXTCHANGE*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE InsertTextAtSelection(DWORD flags, const WCHAR* text, ULONG count,
                                                    LONG* start, LONG* end,
                                                    TS_TEXTCHANGE* change) override {
        if ((!text && count != 0) || (flags & ~(TS_IAS_NOQUERY | TS_IAS_QUERYONLY)) != 0 ||
            ((flags & TS_IAS_NOQUERY) != 0 && (flags & TS_IAS_QUERYONLY) != 0)) {
            return E_INVALIDARG;
        }
        if (!has_write_lock()) {
            return TS_E_NOLOCK;
        }
        if (!edit) {
            return E_FAIL;
        }
        TextInputSelection selection{};
        if (text_input_selection(edit, &selection) != TextInputResult::unchanged) {
            return E_FAIL;
        }
        const u32 begin = minimum(selection.anchor, selection.caret);
        const u32 finish = maximum(selection.anchor, selection.caret);
        if (count > ~0u - begin) {
            return E_INVALIDARG;
        }
        const u32 inserted_end = begin + count;
        if ((flags & TS_IAS_QUERYONLY) != 0) {
            if (!start || !end) {
                return E_INVALIDARG;
            }
            *start = static_cast<LONG>(begin);
            *end = static_cast<LONG>(inserted_end);
            return S_OK;
        }
        if (!change) {
            return E_INVALIDARG;
        }
        if ((flags & TS_IAS_NOQUERY) == 0 && (!start || !end)) {
            return E_INVALIDARG;
        }
        const HRESULT result = replace(begin, finish, text, count, change);
        if (SUCCEEDED(result) && (flags & TS_IAS_NOQUERY) == 0) {
            *start = static_cast<LONG>(begin);
            *end = static_cast<LONG>(inserted_end);
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(DWORD, IDataObject*, LONG*, LONG*,
                                                        TS_TEXTCHANGE*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE RequestSupportedAttrs(DWORD, ULONG, const TS_ATTRID*) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RequestAttrsAtPosition(LONG position, ULONG, const TS_ATTRID*,
                                                     DWORD) override {
        return position_valid(position) ? S_OK : TS_E_INVALIDPOS;
    }

    HRESULT STDMETHODCALLTYPE RequestAttrsTransitioningAtPosition(LONG position, ULONG, const TS_ATTRID*,
                                                                  DWORD) override {
        return position_valid(position) ? S_OK : TS_E_INVALIDPOS;
    }

    HRESULT STDMETHODCALLTYPE FindNextAttrTransition(LONG start, LONG halt, ULONG, const TS_ATTRID*, DWORD,
                                                     LONG* next, BOOL* found, LONG* found_offset) override {
        if (!next || !found || !found_offset || !position_valid(start) || !position_valid(halt)) {
            return E_INVALIDARG;
        }
        *next = halt;
        *found = FALSE;
        *found_offset = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RetrieveRequestedAttrs(ULONG, TS_ATTRVAL*, ULONG* fetched) override {
        if (!fetched) {
            return E_INVALIDARG;
        }
        *fetched = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetEndACP(LONG* end) override {
        if (!end) {
            return E_INVALIDARG;
        }
        if (!has_read_lock()) {
            return TS_E_NOLOCK;
        }
        u32 length = 0;
        if (!document_length(&length)) {
            return E_FAIL;
        }
        *end = static_cast<LONG>(length);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetActiveView(TsViewCookie* view) override {
        if (!view) {
            return E_INVALIDARG;
        }
        *view = kSecureTextView;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetACPFromPoint(TsViewCookie view, const POINT* point, DWORD flags,
                                              LONG* position) override {
        if (view != kSecureTextView || !point || !position) {
            return E_INVALIDARG;
        }
        if (!has_read_lock()) {
            return TS_E_NOLOCK;
        }
        SecureTextStoreGeometry geometry{};
        if (!get_geometry(&geometry)) {
            return TS_E_NOLAYOUT;
        }
        if (!point_in_rectangle(*point, *geometry.text_screen.native()) && (flags & GXFPF_NEAREST) == 0) {
            return TS_E_INVALIDPOINT;
        }
        const bool visible = point_in_rectangle(*point, *geometry.text_screen.native());
        u32 hit = 0;
        bool inside = false;
        if (!config.point || !config.point(config.context, *point,
                !visible || (flags & GXFPF_ROUND_NEAREST) != 0, &hit, &inside))
            return TS_E_NOLAYOUT;
        if ((!inside || !visible) && (flags & GXFPF_NEAREST) == 0)
            return TS_E_INVALIDPOINT;
        if (hit > 0x7fffffffu || !position_valid(static_cast<LONG>(hit)))
            return E_FAIL;
        *position = static_cast<LONG>(hit);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTextExt(TsViewCookie view, LONG start, LONG end, RECT* rectangle,
                                         BOOL* clipped) override {
        if (view != kSecureTextView || !rectangle || !clipped || start < 0 || end < start) {
            return E_INVALIDARG;
        }
        if (!has_read_lock()) {
            return TS_E_NOLOCK;
        }
        if (!position_valid(start) || !position_valid(end)) {
            return TS_E_INVALIDPOS;
        }
        if (config.range_geometry) {
            bool range_clipped = false;
            if (!config.range_geometry(config.context, static_cast<u32>(start), static_cast<u32>(end),
                rectangle, &range_clipped)) return TS_E_NOLAYOUT;
            *clipped = range_clipped ? TRUE : FALSE;
            return S_OK;
        }
        SecureTextStoreGeometry geometry{};
        if (!get_geometry(&geometry)) {
            return TS_E_NOLAYOUT;
        }
        *rectangle = *geometry.text_screen.native();
        *clipped = rectangle->left < geometry.window_screen.left ||
                           rectangle->top < geometry.window_screen.top ||
                           rectangle->right > geometry.window_screen.right ||
                           rectangle->bottom > geometry.window_screen.bottom
                       ? TRUE
                       : FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetScreenExt(TsViewCookie view, RECT* rectangle) override {
        if (view != kSecureTextView || !rectangle) {
            return E_INVALIDARG;
        }
        SecureTextStoreGeometry geometry{};
        if (!get_geometry(&geometry)) {
            return TS_E_NOLAYOUT;
        }
        *rectangle = *geometry.window_screen.native();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetWnd(TsViewCookie view, HWND* window) override {
        if (view != kSecureTextView || !window) {
            return E_INVALIDARG;
        }
        *window = config.window;
        return config.window ? S_OK : E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE GetInputScopes(InputScope** scopes, UINT* count) override {
        if (!scopes || !count) {
            return E_INVALIDARG;
        }
        *scopes = nullptr;
        *count = 0;
        auto* values = static_cast<InputScope*>(CoTaskMemAlloc(sizeof(InputScope)));
        if (!values) {
            return E_OUTOFMEMORY;
        }
        values[0] = native_scope(scope);
        *scopes = values;
        *count = 1;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPhrase(BSTR** phrases, UINT* count) override {
        if (!phrases || !count) {
            return E_INVALIDARG;
        }
        *phrases = nullptr;
        *count = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetRegularExpression(BSTR* expression) override {
        if (!expression) {
            return E_INVALIDARG;
        }
        *expression = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSRGS(BSTR* srgs) override {
        if (!srgs) {
            return E_INVALIDARG;
        }
        *srgs = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetXML(BSTR* xml) override {
        if (!xml) {
            return E_INVALIDARG;
        }
        *xml = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnStartComposition(ITfCompositionView* view, BOOL* accepted) override {
        CallbackScope callback(this);
        if (!accepted) {
            return E_INVALIDARG;
        }
        *accepted = FALSE;
        if (!view || !edit || composition.active || composition_view || deferred_operations || cleanup_active ||
            (context && text_edit_sink_cookie == TF_INVALID_COOKIE)) {
            return S_OK;
        }
        ITfRange* range = nullptr;
        const HRESULT result = view->GetRange(&range);
        if (FAILED(result) || !range) {
            com_release(range);
            return S_OK;
        }
        const bool starts_group = !history_group_owned;
        if (starts_group && !text_input_begin_group(edit)) { range->Release(); return S_OK; }
        history_group_owned = true;
        view->AddRef();
        composition_view = view;
        bool changed = false;
        const bool updated = update_composition(range, edit_cookie, &changed);
        range->Release();
        if (updated && composition_view == view && composition.active) {
            *accepted = TRUE;
            if (changed) notify_composition_change();
        } else if (composition_view == view) clear_composition();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnUpdateComposition(ITfCompositionView* view,
                                                  ITfRange* new_range) override {
        CallbackScope callback(this);
        if (!composition.active || !edit || deferred_operations || cleanup_active ||
            (context && text_edit_sink_cookie == TF_INVALID_COOKIE) || (!view && !new_range) ||
            (view && view != composition_view)) {
            return S_OK;
        }
        ITfRange* owned_range = nullptr;
        ITfRange* range = new_range;
        if (!range && SUCCEEDED(view->GetRange(&owned_range))) {
            range = owned_range;
        }
        bool changed = false;
        const bool updated = range && update_composition(range, edit_cookie, &changed);
        const u32 published_revision = composition.revision;
        com_release(owned_range);
        if (updated && changed && composition.active && composition.revision == published_revision &&
            (!view || composition_view == view)) {
            notify_composition_change();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnEndComposition(ITfCompositionView* view) override {
        CallbackScope callback(this);
        if (!cleanup_active && !deferred_operations && composition.active && view == composition_view) {
            const auto target = edit;
            const u32 ended_revision = composition.revision == ~0u ? 1 : composition.revision + 1;
            clear_composition();
            if (!cleanup_active && !deferred_operations && edit == target &&
                !composition.active && composition.revision == ended_revision) notify_composition_change();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnEndEdit(ITfContext* edited_context, TfEditCookie read_cookie,
                                         ITfEditRecord*) override {
        // Unadvise/Pop can invoke callbacks while the old context still exists.
        if (!context || edited_context != context || text_edit_sink_cookie == TF_INVALID_COOKIE ||
            deferred_operations || cleanup_active || !edit || !composition.active || !composition_view) return S_OK;
        CallbackScope callback(this);
        auto* read_context = context;
        auto* read_view = composition_view;
        const auto target = edit;
        const DWORD advised_cookie = text_edit_sink_cookie;
        const u32 revision = composition.revision;
        read_context->AddRef();
        read_view->AddRef();
        ITfRange* range = nullptr;
        SecureTextComposition next{};
        const bool collected = SUCCEEDED(read_view->GetRange(&range)) && range &&
            build_composition(range, read_context, target, read_cookie, &next);
        // Every external COM read may reenter field teardown/rebinding. Publish
        // only into the exact still-active document/composition we captured.
        if (collected && context == read_context && composition_view == read_view && edit == target &&
            !deferred_operations && text_edit_sink_cookie == advised_cookie && composition.revision == revision &&
            composition.active && !same_composition(next)) {
            next.revision = revision == ~0u ? 1 : revision + 1;
            copy_value(&composition, next);
            notify_composition_change();
        }
        com_release(range);
        read_view->Release();
        read_context->Release();
        return S_OK;
    }

    bool unsafe_teardown() const noexcept { return callback_depth || lock_flags || key_depth; }

    void queue_deferred(u8 operations) noexcept {
        deferred_operations |= operations;
        if (operations & deferred_detach) deferred_operations &= static_cast<u8>(~deferred_attach);
        if (operations & deferred_attach) deferred_operations &= static_cast<u8>(~deferred_detach);
        if (deferred_queued) return;
        AddRef();
        deferred_queued = true;
        deferred_next = deferred_head;
        deferred_head = this;
    }

    bool process_deferred() noexcept {
        CallbackScope cleanup(this, true);
        const u8 operations = deferred_operations;
        // Preserve final attachment intent across cleanup callbacks. A new
        // intrusive queue entry distinguishes a reentrant request from this
        // batch's in-flight intent; it will be handled at the next boundary.
        deferred_operations = operations & (deferred_detach | deferred_attach);
        const auto finish = [this](bool result) noexcept {
            if (!deferred_queued) deferred_operations = 0;
            return result;
        };
        if (operations & deferred_detach) return finish(detach(true));
        if (!destroy_context(true)) { queue_deferred(operations); return finish(false); }
        if (deferred_queued || !edit) return finish(true);
        if (operations & deferred_attach) {
            if (!thread_manager) return finish(attach(true));
            return finish(create_context());
        }
        return finish(!thread_manager || create_context());
    }

    bool attach(bool draining = false) noexcept {
        if (!edit || !config.window || !IsWindow(config.window)) return false;
        if (!draining && (unsafe_teardown() || deferred_queued)) { queue_deferred(deferred_reset | deferred_attach); return true; }
        if (thread_manager) {
            return true;
        }
        if (!edit || !config.window || !IsWindow(config.window)) {
            return false;
        }
        CallbackScope cleanup(this, true);
        msctf_module = LoadLibraryExW(L"msctf.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!msctf_module) {
            detach();
            return false;
        }
        const auto create_thread_manager =
            reinterpret_cast<TfCreateThreadMgrFunction>(GetProcAddress(msctf_module, "TF_CreateThreadMgr"));
        if (!create_thread_manager || FAILED(create_thread_manager(&thread_manager)) || !thread_manager ||
            FAILED(thread_manager->Activate(&client_id))) {
            detach();
            return false;
        }
        manager_activated = true;
        if (FAILED(thread_manager->QueryInterface(__uuidof(ITfKeystrokeMgr), reinterpret_cast<void**>(&keystroke_manager)))) {
            detach(); return false;
        }
        if (FAILED(thread_manager->CreateDocumentMgr(&document_manager)) || !document_manager ||
            !create_context()) {
            detach();
            return false;
        }
        ITfDocumentMgr* previous = nullptr;
        if (FAILED(thread_manager->AssociateFocus(config.window, document_manager, &previous))) {
            com_release(previous);
            detach();
            return false;
        }
        previous_document_manager = previous;
        focus_associated = true;
        if (GetFocus() == config.window && FAILED(thread_manager->SetFocus(document_manager))) {
            detach();
            return false;
        }
        return true;
    }

    bool detach(bool draining = false) noexcept {
        if (!draining && unsafe_teardown()) {
            clear_composition();
            queue_deferred(deferred_reset | deferred_detach);
            return false;
        }
        CallbackScope cleanup(this, true);
        if (!destroy_context(true)) { queue_deferred(deferred_reset | deferred_detach); return false; }
        if (deferred_queued) return true;
        if (focus_associated && thread_manager && config.window) {
            ITfDocumentMgr* replaced = nullptr;
            if (SUCCEEDED(
                    thread_manager->AssociateFocus(config.window, previous_document_manager, &replaced))) {
                com_release(replaced);
            }
        }
        focus_associated = false;
        com_release(previous_document_manager);
        com_release(document_manager);
        com_release(display_attribute_manager);
        com_release(category_manager);
        com_release(keystroke_manager);
        if (manager_activated && thread_manager) {
            thread_manager->Deactivate();
        }
        manager_activated = false;
        client_id = TF_CLIENTID_NULL;
        com_release(thread_manager);
        clear_sink();
        if (msctf_module) {
            FreeLibrary(msctf_module);
            msctf_module = nullptr;
        }
        return true;
    }

    bool bind(TextInputTarget next_edit, SecureTextInputScope next_scope) noexcept {
        if (!text_input_ready(next_edit)) {
            return false;
        }
        if (edit == next_edit && scope == next_scope) {
            synchronize();
            return true;
        }
        const bool deferred = unsafe_teardown() || deferred_queued;
        if (deferred) queue_deferred(deferred_reset);
        if (edit) {
            text_input_cancel(edit);
        }
        clear_composition();
        edit = next_edit;
        scope = next_scope;
        application_change_pending = false;
        observe();
        if (deferred) return true;
        if (thread_manager && !rebuild_context()) {
            return false;
        }
        return true;
    }

    void unbind() noexcept {
        if (!edit) {
            return;
        }
        const bool deferred = unsafe_teardown() || deferred_queued;
        if (deferred) queue_deferred(deferred_reset);
        if (edit) {
            text_input_cancel(edit);
        }
        clear_composition();
        reset_value(&edit);
        application_change_pending = false;
        observe();
        if (!deferred && thread_manager) destroy_context();
    }

    void synchronize() noexcept {
        if (!edit || deferred_operations) {
            return;
        }
        u32 length = 0;
        TextInputSelection selection{};
        if (text_input_length(edit, &length) != TextInputResult::unchanged ||
            text_input_selection(edit, &selection) != TextInputResult::unchanged) {
            return;
        }
        const bool text_changed = text_input_revision(edit) != observed_revision || length != observed_length;
        const bool selection_changed =
            selection.anchor != observed_selection.anchor || selection.caret != observed_selection.caret;
        if (text_changed) {
            const TS_TEXTCHANGE change{0, static_cast<LONG>(observed_length), static_cast<LONG>(length)};
            notify_text_change(change);
        }
        if (selection_changed) {
            notify_selection_change();
        }
        observed_revision = text_input_revision(edit);
        observed_length = length;
        observed_selection = selection;
    }

    void notify_layout() noexcept {
        if (lock_flags != 0) {
            pending_layout = true;
            return;
        }
        if (sink && (sink_mask & TS_AS_LAYOUT_CHANGE) != 0) {
            sink->OnLayoutChange(TS_LC_CHANGE, kSecureTextView);
        }
    }

    bool copy_composition(SecureTextComposition* output) const noexcept {
        if (!output) {
            return false;
        }
        copy_value(output, composition);
        return true;
    }

  private:
    HRESULT last_session_result = E_FAIL;

    void clear_composition() noexcept {
        auto* previous_view = composition_view;
        composition_view = nullptr;
        const bool ends_group = history_group_owned;
        history_group_owned = false;
        const u32 revision = composition.revision == ~0u ? 1 : composition.revision + 1;
        reset_value<SecureTextComposition>(&composition);
        composition.revision = revision;
        if (ends_group) text_input_end_group(edit);
        // Publish the cleared owner before the external Release can reenter.
        if (previous_view) previous_view->Release();
    }

    bool range_extent(ITfRange* range, u32* start, u32* end, const TextInputTarget& target) const noexcept {
        if (!range || !start || !end) {
            return false;
        }
        ITfRangeACP* acp = nullptr;
        LONG anchor = 0;
        LONG length = 0;
        const HRESULT queried =
            range->QueryInterface(__uuidof(ITfRangeACP), reinterpret_cast<void**>(&acp));
        const HRESULT extent = SUCCEEDED(queried) && acp ? acp->GetExtent(&anchor, &length) : E_FAIL;
        com_release(acp);
        u32 document_end = 0;
        if (FAILED(extent) || anchor < 0 || length < 0 ||
            edit != target || text_input_length(target, &document_end) != TextInputResult::unchanged ||
            static_cast<u32>(anchor) > document_end ||
            static_cast<u32>(length) > document_end - static_cast<u32>(anchor)) {
            return false;
        }
        *start = static_cast<u32>(anchor);
        *end = static_cast<u32>(anchor) + static_cast<u32>(length);
        return true;
    }

    bool ensure_display_attribute_managers() noexcept {
        if (category_manager && display_attribute_manager) {
            return true;
        }
        if (!msctf_module) {
            return false;
        }
        const auto create_category = reinterpret_cast<TfCreateCategoryMgrFunction>(
            GetProcAddress(msctf_module, "TF_CreateCategoryMgr"));
        const auto create_display = reinterpret_cast<TfCreateDisplayAttributeMgrFunction>(
            GetProcAddress(msctf_module, "TF_CreateDisplayAttributeMgr"));
        if (!create_category || !create_display ||
            (!category_manager &&
             (FAILED(create_category(&category_manager)) || !category_manager)) ||
            (!display_attribute_manager &&
             (FAILED(create_display(&display_attribute_manager)) || !display_attribute_manager))) {
            com_release(display_attribute_manager);
            com_release(category_manager);
            return false;
        }
        return true;
    }

    bool apply_display_attribute(ITfReadOnlyProperty* property, ITfRange* range,
                                 TextCompositionSpan* span, TfEditCookie read_cookie,
                                 ITfContext* read_context, const TextInputTarget& target) noexcept {
        if (!property || !range || !span) {
            return false;
        }
        VARIANT value{};
        if (property->GetValue(read_cookie, range, &value) != S_OK || value.vt != VT_I4) {
            return value.vt == VT_EMPTY;
        }
        if (context != read_context || edit != target || deferred_operations || text_edit_sink_cookie == TF_INVALID_COOKIE ||
            !ensure_display_attribute_managers()) return false;
        auto* category = category_manager;
        auto* display = display_attribute_manager;
        category->AddRef();
        display->AddRef();
        GUID identifier{};
        ITfDisplayAttributeInfo* information = nullptr;
        TF_DISPLAYATTRIBUTE native{};
        const HRESULT resolved = category->GetGUID(static_cast<TfGuidAtom>(value.lVal), &identifier);
        const HRESULT fetched = SUCCEEDED(resolved)
                                    ? display->GetDisplayAttributeInfo(identifier, &information, nullptr)
                                    : E_FAIL;
        const HRESULT described =
            SUCCEEDED(fetched) && information ? information->GetAttributeInfo(&native) : E_FAIL;
        com_release(information);
        display->Release();
        category->Release();
        if (FAILED(described)) {
            return false;
        }
        span->line_style = composition_line_style(native.lsStyle);
        span->attribute = composition_attribute(native.bAttr);
        span->bold_line = native.fBoldLine != FALSE;
        bool present = false;
        span->text_rgb = composition_rgb(native.crText, &present);
        if (present) {
            span->color_flags |= text_composition_color_text;
        }
        span->background_rgb = composition_rgb(native.crBk, &present);
        if (present) {
            span->color_flags |= text_composition_color_background;
        }
        span->line_rgb = composition_rgb(native.crLine, &present);
        if (present) {
            span->color_flags |= text_composition_color_line;
        }
        return true;
    }

    bool collect_display_spans(ITfRange* composition_range,
                               SecureTextComposition* next, TfEditCookie read_cookie,
                               ITfContext* read_context, const TextInputTarget& target) noexcept {
        if (!read_context || !composition_range || !next) {
            return false;
        }
        ITfProperty* property = nullptr;
        IEnumTfRanges* ranges = nullptr;
        if (FAILED(read_context->GetProperty(kDisplayAttributeProperty, &property)) || !property ||
            FAILED(property->EnumRanges(read_cookie, &ranges, composition_range)) || !ranges) {
            com_release(ranges);
            com_release(property);
            return false;
        }
        bool complete = true;
        for (;;) {
            ITfRange* range = nullptr;
            ULONG fetched = 0;
            const HRESULT result = ranges->Next(1, &range, &fetched);
            if (result != S_OK || fetched != 1 || !range) {
                com_release(range);
                if (result != S_FALSE) {
                    complete = false;
                }
                break;
            }
            u32 start = 0;
            u32 end = 0;
            if (range_extent(range, &start, &end, target) && end > start) {
                start = maximum(start, next->start);
                end = minimum(end, next->end);
                if (end > start) {
                    if (next->span_count == kTextCompositionSpanCapacity) {
                        complete = false;
                    } else {
                        TextCompositionSpan span = default_composition_span(start, end);
                        if (!apply_display_attribute(property, range, &span, read_cookie, read_context, target)) {
                            complete = false;
                        }
                        next->spans[next->span_count++] = span;
                    }
                }
            } else {
                complete = false;
            }
            range->Release();
        }
        ranges->Release();
        property->Release();
        return complete;
    }

    bool same_composition(const SecureTextComposition& next) const noexcept {
        if (composition.active != next.active || composition.complete != next.complete ||
            composition.start != next.start || composition.end != next.end ||
            composition.span_count != next.span_count) return false;
        for (u32 index = 0; index < next.span_count; ++index) {
            const auto& a = composition.spans[index];
            const auto& b = next.spans[index];
            if (a.start != b.start || a.end != b.end || a.text_rgb != b.text_rgb ||
                a.background_rgb != b.background_rgb || a.line_rgb != b.line_rgb ||
                a.line_style != b.line_style || a.attribute != b.attribute ||
                a.color_flags != b.color_flags || a.bold_line != b.bold_line) return false;
        }
        return true;
    }

    bool build_composition(ITfRange* range, ITfContext* read_context, const TextInputTarget& target,
                           TfEditCookie read_cookie, SecureTextComposition* next) noexcept {
        u32 start = 0;
        u32 end = 0;
        if (!range_extent(range, &start, &end, target)) return false;
        reset_value(next);
        next->start = start;
        next->end = end;
        next->active = true;
        next->complete = collect_display_spans(range, next, read_cookie, read_context, target);
        if (next->span_count == 0 && end > start) {
            next->spans[next->span_count++] = default_composition_span(start, end);
        }
        return true;
    }

    bool update_composition(ITfRange* range, TfEditCookie read_cookie, bool* changed) noexcept {
        *changed = false;
        const auto target = edit;
        const u32 revision = composition.revision;
        auto* read_context = context;
        if (read_context) read_context->AddRef();
        SecureTextComposition next{};
        const bool collected = build_composition(range, read_context, target, read_cookie, &next);
        const bool current = context == read_context && edit == target && !deferred_operations && composition.revision == revision;
        if (collected && current && !same_composition(next)) {
            next.revision = revision == ~0u ? 1 : revision + 1;
            copy_value(&composition, next);
            *changed = true;
        }
        com_release(read_context);
        return collected && current;
    }

    void notify_composition_change() noexcept {
        if (lock_flags != 0) {
            application_change_pending = true;
        } else if (config.changed) {
            config.changed(config.context);
        }
    }

    bool has_read_lock() const noexcept {
        return !deferred_operations && !cleanup_active && (lock_flags & TS_LF_READ) != 0;
    }

    bool has_write_lock() const noexcept {
        return !deferred_operations && !cleanup_active && (lock_flags & TS_LF_READWRITE) == TS_LF_READWRITE;
    }

    void grant_lock(DWORD requested) noexcept {
        // Text services may insert provisional text before OnStartComposition.
        // Snapshot before granting write access; keep it across locks while the
        // composition remains active. Ordinary write transactions end here.
        if ((requested & TS_LF_READWRITE) == TS_LF_READWRITE && edit && !history_group_owned) {
            if (!text_input_begin_group(edit)) { last_session_result = E_OUTOFMEMORY; return; }
            history_group_owned = true;
        }
        ITextStoreACPSink* current = sink;
        current->AddRef();
        lock_flags = requested & TS_LF_READWRITE;
        last_session_result = current->OnLockGranted(requested);
        lock_flags = 0;
        if (history_group_owned && !composition.active) {
            history_group_owned = false;
            text_input_end_group(edit);
        }
        current->Release();
    }

    bool document_length(u32* length) const noexcept {
        return edit && text_input_length(edit, length) == TextInputResult::unchanged;
    }

    bool position_valid(LONG position) const noexcept {
        if (position < 0) {
            return false;
        }
        u32 length = 0;
        return document_length(&length) && static_cast<u32>(position) <= length;
    }

    HRESULT replace(u32 start, u32 end, const WCHAR* text, ULONG count,
                    TS_TEXTCHANGE* text_change) noexcept {
        if (!edit) {
            return E_FAIL;
        }
        TextInputChange change{};
        const TextInputResult result = text_input_replace(
            edit, start, end, ConstSlice<u16>{reinterpret_cast<const u16*>(text), static_cast<u32>(count)},
            &change);
        if (result != TextInputResult::changed && result != TextInputResult::unchanged) {
            return edit_result_hresult(result);
        }
        *text_change = TS_TEXTCHANGE{static_cast<LONG>(change.start), static_cast<LONG>(change.old_end),
                                     static_cast<LONG>(change.new_end)};
        observe();
        if (result == TextInputResult::changed) {
            pending_layout = true;
            application_change_pending = true;
        }
        return S_OK;
    }

    void observe() noexcept {
        if (!edit) {
            observed_revision = 0;
            observed_length = 0;
            reset_value<TextInputSelection>(&observed_selection);
            return;
        }
        observed_revision = text_input_revision(edit);
        if (text_input_length(edit, &observed_length) != TextInputResult::unchanged) {
            observed_length = 0;
        }
        if (text_input_selection(edit, &observed_selection) != TextInputResult::unchanged) {
            reset_value<TextInputSelection>(&observed_selection);
        }
    }

    void notify_text_change(const TS_TEXTCHANGE& change) noexcept {
        if (lock_flags != 0) {
            return;
        }
        if (sink && (sink_mask & TS_AS_TEXT_CHANGE) != 0) {
            sink->OnTextChange(0, &change);
        }
        notify_layout();
    }

    void notify_selection_change() noexcept {
        if (lock_flags == 0 && sink && (sink_mask & TS_AS_SEL_CHANGE) != 0) {
            sink->OnSelectionChange();
        }
    }

    void flush_layout() noexcept {
        if (!pending_layout) {
            return;
        }
        pending_layout = false;
        notify_layout();
    }

    bool get_geometry(SecureTextStoreGeometry* geometry) const noexcept {
        return geometry && config.geometry && config.geometry(config.context, geometry);
    }

    void clear_sink() noexcept {
        com_release(sink);
        com_release(sink_identity);
        sink_mask = 0;
        lock_flags = 0;
        pending_lock_flags = 0;
    }

    bool create_context() noexcept {
        if (!document_manager || context || client_id == TF_CLIENTID_NULL) {
            return false;
        }
        if (FAILED(document_manager->CreateContext(client_id, 0, static_cast<ITextStoreACP*>(this),
                                                   &context, &edit_cookie)) ||
            !context) {
            com_release(context);
            return false;
        }
        ITfSource* source = nullptr;
        const HRESULT queried = context->QueryInterface(__uuidof(ITfSource), reinterpret_cast<void**>(&source));
        const HRESULT advised = SUCCEEDED(queried) && source ? source->AdviseSink(
            __uuidof(ITfTextEditSink), static_cast<ITfTextEditSink*>(this), &text_edit_sink_cookie) : E_NOINTERFACE;
        com_release(source);
        if (FAILED(advised) || text_edit_sink_cookie == TF_INVALID_COOKIE) {
            destroy_context();
            return false;
        }
        if (FAILED(document_manager->Push(context))) {
            destroy_context();
            edit_cookie = TF_INVALID_EDIT_COOKIE;
            return false;
        }
        context_pushed = true;
        return true;
    }

    bool destroy_context(bool draining = false) noexcept {
        if (!draining && unsafe_teardown()) { queue_deferred(deferred_reset); return false; }
        CallbackScope cleanup(this, true);
        const DWORD cookie = text_edit_sink_cookie;
        text_edit_sink_cookie = TF_INVALID_COOKIE;
        if (context && cookie != TF_INVALID_COOKIE) {
            ITfSource* source = nullptr;
            const HRESULT queried = context->QueryInterface(__uuidof(ITfSource), reinterpret_cast<void**>(&source));
            const HRESULT removed = SUCCEEDED(queried) && source ? source->UnadviseSink(cookie) : E_NOINTERFACE;
            com_release(source);
            if (FAILED(removed) && removed != CONNECT_E_NOCONNECTION) {
                text_edit_sink_cookie = cookie;
                queue_deferred(deferred_reset);
                return false;
            }
        }
        if (context_pushed && document_manager && FAILED(document_manager->Pop(TF_POPF_ALL))) {
            queue_deferred(deferred_reset);
            return false;
        }
        context_pushed = false;
        clear_composition();
        com_release(context);
        edit_cookie = TF_INVALID_EDIT_COOKIE;
        clear_sink();
        return true;
    }

    bool rebuild_context() noexcept {
        CallbackScope cleanup(this, true);
        if (!destroy_context(true)) return false;
        return deferred_operations || create_context();
    }
};

// Four COM interface pointers plus the composition-view/sink cookie; the
// implementation must stay within one committed page.
static_assert(sizeof(SecureTextStoreImplementation) == 1464);

SecureTextStoreImplementation* implementation(SecureTextStore* store) noexcept {
    return store ? static_cast<SecureTextStoreImplementation*>(store->implementation) : nullptr;
}

const SecureTextStoreImplementation* implementation(const SecureTextStore& store) noexcept {
    return static_cast<const SecureTextStoreImplementation*>(store.implementation);
}

} // namespace

bool secure_text_store_process_deferred() noexcept {
    // Detach one finite batch. Reentrant requests and cleanup retries remain
    // linked for a later top-level dispatch boundary, never an in-callback loop.
    auto* pending = deferred_head;
    deferred_head = nullptr;
    bool success = true;
    while (pending) {
        auto* current = pending;
        pending = current->deferred_next;
        if (current->unsafe_teardown()) {
            current->deferred_next = deferred_head;
            deferred_head = current;
            continue;
        }
        current->deferred_queued = false;
        current->deferred_next = nullptr;
        success = current->process_deferred() && success;
        current->Release();
    }
    return success;
}

bool secure_text_store_initialize(SecureTextStore* store, SecureTextStoreConfig config) noexcept {
    if (!store || store->implementation || !config.window) {
        return false;
    }
    void* memory = VirtualAlloc(nullptr, sizeof(SecureTextStoreImplementation),
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory) {
        return false;
    }
    auto* value = new (memory) SecureTextStoreImplementation{};
    value->config = config;
    store->implementation = value;
    return true;
}

void secure_text_store_release(SecureTextStore* store) noexcept {
    auto* value = implementation(store);
    if (!value) {
        return;
    }
    store->implementation = nullptr;
    value->unbind();
    if (value->key_depth) value->detach_after_key = true;
    else value->detach();
    value->Release();
}

bool secure_text_store_attach(SecureTextStore* store) noexcept {
    auto* value = implementation(store);
    return value && value->attach();
}

void secure_text_store_detach(SecureTextStore* store) noexcept {
    auto* value = implementation(store);
    if (value) {
        if (value->key_depth) value->detach_after_key = true;
        else value->detach();
    }
}

bool secure_text_store_attached(const SecureTextStore& store) noexcept {
    const auto* value = implementation(store);
    return value && value->thread_manager;
}

bool secure_text_store_bind(SecureTextStore* store, SecureEdit* edit, SecureTextInputScope scope) noexcept {
    auto* value = implementation(store);
    return value && value->bind(secure_edit_input_target(edit), scope);
}
bool secure_text_store_bind_target(SecureTextStore* store, TextInputTarget target, SecureTextInputScope scope) noexcept {
    auto* value = implementation(store);
    return value && value->bind(target, scope);
}

void secure_text_store_unbind(SecureTextStore* store) noexcept {
    auto* value = implementation(store);
    if (value) {
        value->unbind();
    }
}

void secure_text_store_set_window_focus(SecureTextStore* store, bool focused) noexcept {
    auto* value = implementation(store);
    if (value && value->thread_manager) {
        value->thread_manager->SetFocus(focused ? value->document_manager : nullptr);
    }
}

void secure_text_store_synchronize(SecureTextStore* store) noexcept {
    auto* value = implementation(store);
    if (value) {
        value->synchronize();
    }
}

void secure_text_store_notify_layout(SecureTextStore* store) noexcept {
    auto* value = implementation(store);
    if (value) {
        value->notify_layout();
    }
}

bool secure_text_store_composition(const SecureTextStore& store,
                                   SecureTextComposition* composition) noexcept {
    const auto* value = implementation(store);
    return value && value->copy_composition(composition);
}

bool secure_text_store_filter_message(SecureTextStore* store, const MSG& message) noexcept {
    if (message.message != WM_KEYDOWN && message.message != WM_KEYUP) return false;
    auto* value = implementation(store);
    if (!value || !value->edit || !value->keystroke_manager || message.hwnd != value->config.window ||
        GetFocus() != value->config.window) return false;
    // Native text services may call back into the host. Keep this object and
    // its loaded TSF module alive if that callback closes or rebinds the field.
    value->AddRef();
    ++value->key_depth;
    const auto edit = value->edit;
    BOOL eaten = FALSE;
    const bool down = message.message == WM_KEYDOWN;
    const HRESULT tested = down ? value->keystroke_manager->TestKeyDown(message.wParam, message.lParam, &eaten) :
        value->keystroke_manager->TestKeyUp(message.wParam, message.lParam, &eaten);
    bool consumed = value->detach_after_key || value->edit != edit || GetFocus() != value->config.window;
    if (!consumed && tested == S_OK && eaten) {
        eaten = FALSE;
        const HRESULT dispatched = down ? value->keystroke_manager->KeyDown(message.wParam, message.lParam, &eaten) :
            value->keystroke_manager->KeyUp(message.wParam, message.lParam, &eaten);
        consumed = (dispatched == S_OK && eaten) || value->detach_after_key || value->edit != edit;
    }
    if (--value->key_depth == 0 && value->detach_after_key) {
        value->detach_after_key = false;
        value->detach();
    }
    value->Release();
    return consumed;
}

void* secure_text_store_test_interface(SecureTextStore* store) noexcept {
    auto* value = implementation(store);
    return value ? static_cast<ITextStoreACP*>(value) : nullptr;
}

} // namespace maicord

