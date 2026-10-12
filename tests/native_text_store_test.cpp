// Native text store. No argument runs the suite; --oracle-data keeps the detailed CSV
// oracle that tools/verify_native_tsf.py consumes (manual).
#include "tsf_composition_fixture.hpp"
#include "maicord/ui/secure_text_store.hpp"
#include "maicord/ui/text.hpp"
#include "maicord/render/text_edit_layout.hpp"

namespace maicord {
namespace {
char output_buffer[16384];
u32 output_count = 0;
bool output_ok = true;
bool detailed_output = false;
struct QueryRecord {
    u32 values[9]{};
};
struct FailureContext {
    u32 sample = 0, fixture = 0, test_case = 0, phase = 0, width = 0, scale_percent = 0, operation = 0;
    QueryRecord recent[16]{};
    u32 recorded = 0;
} failure_context;
void flush() noexcept {
    DWORD n = 0;
    if (output_count &&
        (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output_buffer, output_count, &n, nullptr) ||
         n != output_count))
        output_ok = false;
    output_count = 0;
}
void print(const char* text) noexcept {
    while (*text) {
        if (output_count == sizeof(output_buffer))
            flush();
        output_buffer[output_count++] = *text++;
    }
}
void number(u32 value) noexcept {
    char text[11];
    u32 n = 10;
    text[n] = 0;
    do {
        text[--n] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    print(text + n);
}
void print_record(const QueryRecord& record) noexcept {
    for (u32 i = 0; i < 9; ++i) {
        if (i)
            print(",");
        number(record.values[i]);
    }
    print("\n");
}
bool fail(u32 line, const char* expression) noexcept {
    print("FAIL ");
    number(line);
    print(" ");
    print(expression);
    print("\n");
    print("CONTEXT sample=");
    number(failure_context.sample);
    print(" fixture=");
    number(failure_context.fixture);
    print(" case=");
    number(failure_context.test_case);
    print(" phase=");
    number(failure_context.phase);
    print(" width=");
    number(failure_context.width);
    print(" scale_percent=");
    number(failure_context.scale_percent);
    print(" operation=");
    number(failure_context.operation);
    print("\n");
    const u32 count = minimum(failure_context.recorded, 16u);
    print("RECENT queries=");
    number(count);
    print(" of ");
    number(failure_context.recorded);
    print("\n");
    for (u32 i = failure_context.recorded - count; i < failure_context.recorded; ++i)
        print_record(failure_context.recent[i % 16]);
    return false;
}
#define CHECK(x)                                                                                           \
    do {                                                                                                   \
        if (!(x))                                                                                          \
            return fail(__LINE__, #x);                                                                     \
    } while (false)
bool configure_output() noexcept {
    const wchar_t* argument = GetCommandLineW();
    bool quoted = false;
    while (*argument) {
        if (*argument == L'"')
            quoted = !quoted;
        else if (!quoted && (*argument == L' ' || *argument == L'\t'))
            break;
        ++argument;
    }
    while (*argument == L' ' || *argument == L'\t')
        ++argument;
    if (!*argument)
        return true;
    // --oracle-data retains the detailed CSV oracle consumed by verify_native_tsf.py.
    const bool option_quoted = *argument == L'"';
    if (option_quoted)
        ++argument;
    const wchar_t* option = L"--oracle-data";
    while (*option && *argument == *option) {
        ++argument;
        ++option;
    }
    if (*option || (option_quoted && *argument != L'"'))
        return fail(__LINE__, "expected only --oracle-data");
    if (option_quoted)
        ++argument;
    while (*argument == L' ' || *argument == L'\t')
        ++argument;
    if (*argument)
        return fail(__LINE__, "expected only --oracle-data");
    detailed_output = true;
    return true;
}
struct Fixture {
    TextSystem system{};
    SecureEdit edit{};
    SecureTextStore store{};
    TextEditLayoutOwner layout{};
    TextEditMetric selections[48]{};
    float width = 0, scale = 1;
    u32 changes = 0;
    bool valid = true;
    ~Fixture() noexcept {
        secure_text_store_release(&store);
        text_edit_layout_release(&layout);
        secure_edit_release(&edit);
        text_system_release(&system);
    }
    Utf8View text() const noexcept {
        const auto bytes = secure_edit_view(edit);
        return {reinterpret_cast<const char*>(bytes.data), bytes.count};
    }
    bool refresh() noexcept {
        SecureEditUtf16Selection selected{};
        CHECK(secure_edit_utf16_selection(edit, &selected) == TextInputResult::unchanged);
        const auto value = text();
        TextEditLayoutInput input{value,
                                    text_edit_signature(value),
                                    selected.anchor,
                                    selected.caret,
                                    12.5f,
                                    24.25f,
                                    width,
                                    120,
                                    1000000,
                                    text_edit_layout_wrap,
                                    {}};
        CHECK(text_edit_layout_measure(&layout, input, system));
        CHECK((layout.geometry || !value.bytes) && !GetModuleHandleW(L"dwrite.dll") && !GetModuleHandleW(L"dwritecore.dll"));
        return true;
    }
    static void changed(void* opaque) noexcept {
        auto& f = *static_cast<Fixture*>(opaque);
        ++f.changes;
        f.valid = f.refresh() && f.valid;
    }
    static bool bounds(void* opaque, SecureTextStoreGeometry* out) noexcept {
        const auto& f = *static_cast<Fixture*>(opaque);
        *out = {{100 + static_cast<LONG>(12.5f * f.scale), 200 + static_cast<LONG>(24.25f * f.scale),
                 100 + static_cast<LONG>((12.5f + f.width) * f.scale),
                 200 + static_cast<LONG>(144.25f * f.scale)},
                {0, 0, 2000, 2000}};
        return true;
    }
    static bool point(void* opaque, POINT p, bool round, u32* position, bool* inside) noexcept {
        auto& f = *static_cast<Fixture*>(opaque);
        return text_edit_layout_point_geometry(f.layout, (p.x - 100) / f.scale, (p.y - 200) / f.scale,
                                                 round, position, inside);
    }
    static bool range(void* opaque, u32 first, u32 last, RECT* out, bool* clipped) noexcept {
        auto& f = *static_cast<Fixture*>(opaque);
        TextEditMetric metrics[48]{};
        u32 count = 0;
        if (first == last) {
            if (!text_edit_layout_position_geometry(f.layout, first, metrics))
                return false;
            metrics[0].width = 1;
            count = 1;
        } else if (!text_edit_layout_range_geometry(f.layout, first, last, {metrics, 48}, &count) ||
                   !count)
            return false;
        float left = metrics[0].left, top = metrics[0].top, right = left + metrics[0].width,
              bottom = top + metrics[0].height;
        for (u32 i = 1; i < count; ++i) {
            left = minimum(left, metrics[i].left);
            top = minimum(top, metrics[i].top);
            right = maximum(right, metrics[i].left + metrics[i].width);
            bottom = maximum(bottom, metrics[i].top + metrics[i].height);
        }
        *out = {100 + static_cast<LONG>(left * f.scale), 200 + static_cast<LONG>(top * f.scale),
                100 + static_cast<LONG>(right * f.scale), 200 + static_cast<LONG>(bottom * f.scale)};
        SecureTextStoreGeometry view{};
        bounds(opaque, &view);
        *clipped = out->left < view.text_screen.left || out->top < view.text_screen.top ||
                   out->right > view.text_screen.right || out->bottom > view.text_screen.bottom;
        return true;
    }
};
struct Sink final : ITextStoreACPSink {
    ULONG references = 1;
    Fixture* fixture = nullptr;
    ITextStoreACP* store = nullptr;
    ITfContextOwnerCompositionSink* owner = nullptr;
    tsf_test::Composition composition;
    u32 operation = 0, records = 0, expected_records = 0, extent_records = 0, point_records = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output)
            return E_POINTER;
        *output = nullptr;
        if (!bytes_equal(&id, &__uuidof(IUnknown), sizeof(GUID)) &&
            !bytes_equal(&id, &__uuidof(ITextStoreACPSink), sizeof(GUID)))
            return E_NOINTERFACE;
        *output = static_cast<ITextStoreACPSink*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return --references;
    }
    void record(u32 type, u32 a, u32 b, HRESULT result, const RECT& rectangle = {},
                u32 extra = 0) noexcept {
        const QueryRecord value{{type, a, b, static_cast<u32>(result), static_cast<u32>(rectangle.left),
                                 static_cast<u32>(rectangle.top), static_cast<u32>(rectangle.right),
                                 static_cast<u32>(rectangle.bottom), extra}};
        failure_context.recent[failure_context.recorded % 16] = value;
        ++failure_context.recorded;
        if (detailed_output)
            print_record(value);
        ++records;
        if (type == 0)
            ++extent_records;
        else
            ++point_records;
    }
    HRESULT STDMETHODCALLTYPE OnLockGranted(DWORD) override {
        failure_context.operation = operation;
        LONG units = 0;
        if (store->GetEndACP(&units) != S_OK)
            return E_FAIL;
        if (operation == 1) {
            TS_SELECTION_ACP selection{units, units, {TS_AE_END, FALSE}};
            TS_TEXTCHANGE change{};
            if (store->SetSelection(1, &selection) != S_OK ||
                store->InsertTextAtSelection(TS_IAS_NOQUERY, L"k", 1, nullptr, nullptr, &change) != S_OK)
                return E_FAIL;
            composition.range.start = units;
            composition.range.length = 1;
            BOOL accepted = FALSE;
            if (owner->OnStartComposition(&composition, &accepted) != S_OK || !accepted)
                return E_FAIL;
            return S_OK;
        }
        if (operation == 2) {
            constexpr wchar_t replacement[] = L"か👩🏽‍💻";
            constexpr LONG count = sizeof(replacement) / sizeof(wchar_t) - 1;
            TS_TEXTCHANGE change{};
            if (store->SetText(0, composition.range.start, composition.range.start + 1, replacement, count,
                               &change) != S_OK)
                return E_FAIL;
            composition.range.length = count;
            if (owner->OnUpdateComposition(&composition, &composition.range) != S_OK)
                return E_FAIL;
            return owner->OnEndComposition(&composition);
        }
        expected_records += static_cast<u32>((units + 1) * 3);
        for (LONG first = 0; first <= units; ++first) {
            const LONG ends[]{first, minimum(first + 1, units), units};
            for (LONG end : ends) {
                RECT rectangle{};
                BOOL clipped = FALSE;
                const auto result = store->GetTextExt(1, first, end, &rectangle, &clipped);
                record(0, first, end, result, rectangle, clipped);
            }
        }
        SecureTextStoreGeometry view{};
        Fixture::bounds(fixture, &view);
        const LONG columns = (view.text_screen.right - view.text_screen.left + 6) / 7 + 1;
        const LONG rows = (view.text_screen.bottom - view.text_screen.top + 6) / 11 + 1;
        expected_records += static_cast<u32>(columns * rows * 4);
        const DWORD point_flags[]{0, GXFPF_NEAREST, GXFPF_ROUND_NEAREST,
                                  GXFPF_NEAREST | GXFPF_ROUND_NEAREST};
        for (LONG y = view.text_screen.top - 3; y <= view.text_screen.bottom + 3; y += 11)
            for (LONG x = view.text_screen.left - 3; x <= view.text_screen.right + 3; x += 7)
                for (DWORD flags : point_flags) {
                    POINT point{x, y};
                    LONG position = -1;
                    const auto result = store->GetACPFromPoint(1, &point, flags, &position);
                    record(1, x, y, result, {static_cast<LONG>(flags), position, 0, 0});
                }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTextChange(DWORD, const TS_TEXTCHANGE*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange() override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnLayoutChange(TsLayoutCode, TsViewCookie) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStatusChange(DWORD) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnAttrsChange(LONG, LONG, ULONG, const TS_ATTRID*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStartEditTransaction() override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnEndEditTransaction() override {
        return S_OK;
    }
};
bool run() noexcept {
    constexpr Utf8View samples[]{utf8_literal("AV ffi café\r\nTo"),
                                 utf8_literal("日本語 カタカナ 한국어"),
                                 utf8_literal("אבג العربية 123"),
                                 utf8_literal("नमस्ते ภาษาไทย"),
                                 utf8_literal("தமிழ் বাংলা"),
                                 utf8_literal("😀 👩🏽‍💻 ❤️ 🇸🇮"),
                                 utf8_literal("𓀀 ᚠᚢᚦ 𝄞"),
                                 utf8_literal("a\tbc\n\nxyz"),
                                 utf8_literal(""),
                                 utf8_literal("𓀀\u200d𓃀 𓐯"),
                                 utf8_literal("ᚠ\u0308 ᚢ\u0301"),
                                 utf8_literal("אבג (ᚠᚢᚦ) 123")};
    const float widths[]{40, 200}, scales[]{1, 1.5f};
    u32 cases = 0, total = 0, fixtures = 0, phases = 0, rejected = 0;
    for (u32 sample_index = 0; sample_index < sizeof(samples) / sizeof(samples[0]); ++sample_index)
        for (float width : widths)
            for (float scale : scales) {
                const auto sample = samples[sample_index];
                failure_context = {};
                failure_context.sample = sample_index;
                failure_context.fixture = fixtures++;
                failure_context.test_case = cases;
                failure_context.width = static_cast<u32>(width);
                failure_context.scale_percent = static_cast<u32>(scale * 100);
                Fixture f;
                f.width = width;
                f.scale = scale;
                f.layout.selections = f.selections;
                f.layout.selection_capacity = 48;
                CHECK(text_system_initialize(&f.system, L"Segoe UI", 16, L"en-US").ok());
                CHECK(secure_edit_initialize(&f.edit, 4096, false, {0, true}) &&
                      secure_edit_enable_history(&f.edit, 8192));
                const auto assigned = secure_edit_assign(&f.edit, sample);
                if (assigned == TextInputResult::invalid_input) {
                    CHECK(utf8_equal(sample, utf8_literal("a\tbc\n\nxyz")));
                    ++rejected;
                    if (detailed_output)
                        print("REJECT tab\n");
                    continue;
                }
                CHECK(assigned <= TextInputResult::changed && f.refresh());
                CHECK(secure_text_store_initialize(&f.store, {reinterpret_cast<HWND>(1), Fixture::bounds,
                                                              Fixture::changed, &f, Fixture::range,
                                                              Fixture::point}) &&
                      secure_text_store_bind(&f.store, &f.edit, SecureTextInputScope::text));
                Sink sink;
                sink.fixture = &f;
                sink.store = static_cast<ITextStoreACP*>(secure_text_store_test_interface(&f.store));
                CHECK(sink.store->QueryInterface(__uuidof(ITfContextOwnerCompositionSink),
                                                 reinterpret_cast<void**>(&sink.owner)) == S_OK);
                CHECK(sink.store->AdviseSink(__uuidof(ITextStoreACPSink), &sink, TS_AS_ALL_SINKS) == S_OK);
                for (u32 phase = 0; phase < 5; ++phase) {
                    failure_context.phase = phase;
                    failure_context.operation = 0;
                    failure_context.recorded = 0;
                    if (phase == 1 || phase == 2) {
                        sink.operation = phase;
                        HRESULT session = E_FAIL;
                        CHECK(sink.store->RequestLock(TS_LF_SYNC | TS_LF_READWRITE, &session) == S_OK &&
                              session == S_OK && f.valid);
                        SecureTextComposition current{};
                        CHECK(secure_text_store_composition(f.store, &current) &&
                              current.active == (phase == 1));
                    }
                    if (phase == 3 || phase == 4) {
                        CHECK((phase == 3 ? secure_edit_undo(&f.edit) : secure_edit_redo(&f.edit)) ==
                              TextInputResult::changed);
                        secure_text_store_synchronize(&f.store);
                        CHECK(f.refresh());
                    }
                    if (phase == 3)
                        CHECK(utf8_equal(f.text(), sample));
                    if (detailed_output) {
                        print("CASE ");
                        number(cases);
                        print(" ");
                        number(phase);
                        print("\n");
                    }
                    sink.operation = 0;
                    HRESULT session = E_FAIL;
                    CHECK(sink.store->RequestLock(TS_LF_SYNC | TS_LF_READ, &session) == S_OK &&
                          session == S_OK);
                    CHECK(sink.records == sink.expected_records);
                    ++phases;
                }
                CHECK(f.changes == 2 && sink.records && sink.extent_records && sink.point_records);
                total += sink.records;
                ++cases;
                CHECK(sink.store->UnadviseSink(&sink) == S_OK);
                sink.owner->Release();
                CHECK(sink.references == 1);
            }
    CHECK(fixtures == 48 && cases == 44 && phases == 220 && rejected == 4 && total > 0);
    print("PASS ");
    number(cases);
    print(" ");
    number(total);
    print("\n");
    return true;
}
} // namespace
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_native_text_store_entry() noexcept {
    const bool valid = maicord::configure_output() && maicord::run();
    maicord::flush();
    ExitProcess(valid && maicord::output_ok ? 0 : 1);
}
