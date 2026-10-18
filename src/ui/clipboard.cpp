#include "maicord/ui/clipboard.hpp"
#include "maicord/platform/clipboard_image.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord {
namespace {

inline constexpr u32 kWin32ConversionMaximum = 0x7FFFFFFFu;

bool system_open(void*, HWND owner) noexcept {
    if (!OpenClipboard(owner))
        return false;
    if (EmptyClipboard())
        return true;
    CloseClipboard();
    return false;
}
bool system_publish(void*, UINT format, HGLOBAL data) noexcept {
    return SetClipboardData(format, data) != nullptr;
}
void system_close(void*) noexcept {
    CloseClipboard();
}
ClipboardSink g_sink{};

constexpr u32 kAppendFailed = ~0u;
u32 append(Slice<char> output, u32 used, const char* text, u32 bytes) noexcept {
    if (used == kAppendFailed || bytes > output.count || used > output.count - bytes)
        return kAppendFailed;
    copy_bytes(output.data + used, text, bytes);
    return used + bytes;
}
template <u32 N> u32 append(Slice<char> output, u32 used, const char (&text)[N]) noexcept {
    return append(output, used, text, N - 1);
}
void put_offset(char* at, u32 value) noexcept {
    for (u32 digit = 10; digit-- > 0;) {
        at[digit] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
}

} // namespace

ClipboardSink clipboard_sink_install(ClipboardSink sink) noexcept {
    const ClipboardSink previous = g_sink;
    g_sink = sink.open && sink.publish && sink.close ? sink : ClipboardSink{};
    return previous;
}

bool clipboard_commit(HWND owner, Slice<ClipboardPayload> payloads) noexcept {
    const ClipboardSink sink =
        g_sink.open ? g_sink : ClipboardSink{nullptr, system_open, system_publish, system_close};
    bool ok = owner && payloads.data && payloads.count;
    for (u32 index = 0; ok && index < payloads.count; ++index)
        ok = payloads[index].format && payloads[index].data;
    if (ok && sink.open(sink.context, owner)) {
        for (u32 index = 0; ok && index < payloads.count; ++index) {
            ok = sink.publish(sink.context, payloads[index].format, payloads[index].data);
            if (ok)
                payloads[index].data = nullptr;
        }
        sink.close(sink.context);
    } else {
        ok = false;
    }
    for (u32 index = 0; payloads.data && index < payloads.count; ++index)
        if (payloads[index].data) {
            GlobalFree(payloads[index].data);
            payloads[index].data = nullptr;
        }
    return ok;
}

ClipboardTextResult clipboard_read_utf8(HWND owner, Slice<char> output) noexcept {
    if (!owner || (!output.data && output.count != 0) || output.count > kWin32ConversionMaximum) {
        return ClipboardTextResult{0, ClipboardTextError::invalid_input};
    }
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        return ClipboardTextResult{0, ClipboardTextError::unavailable};
    }
    if (!OpenClipboard(owner)) {
        return ClipboardTextResult{0, ClipboardTextError::open_failed};
    }

    ClipboardTextResult result{0, ClipboardTextError::data_failed};
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    const SIZE_T allocation_bytes = data ? GlobalSize(data) : 0;
    const auto* wide = data ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
    if (wide && allocation_bytes >= sizeof(wchar_t)) {
        const usize available_units = allocation_bytes / sizeof(wchar_t);
        const usize maximum_scan = static_cast<usize>(output.count) + 1;
        const usize scan_units = minimum(available_units, maximum_scan);
        usize units = 0;
        while (units < scan_units && wide[units] != 0) {
            ++units;
        }
        if (units == scan_units) {
            result.error = scan_units < available_units ? ClipboardTextError::capacity
                                                        : ClipboardTextError::invalid_text;
        } else if (units > kWin32ConversionMaximum) {
            result.error = ClipboardTextError::capacity;
        } else if (units == 0) {
            result = ClipboardTextResult{0, ClipboardTextError::none};
        } else {
            const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide,
                                                     static_cast<int>(units), nullptr, 0, nullptr, nullptr);
            if (required <= 0) {
                result.error = ClipboardTextError::invalid_text;
            } else if (static_cast<u32>(required) > output.count) {
                result.error = ClipboardTextError::capacity;
            } else {
                const int written =
                    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, static_cast<int>(units),
                                        output.data, static_cast<int>(output.count), nullptr, nullptr);
                result = written == required
                             ? ClipboardTextResult{static_cast<u32>(written), ClipboardTextError::none}
                             : ClipboardTextResult{0, ClipboardTextError::invalid_text};
            }
        }
    }
    if (wide) {
        GlobalUnlock(data);
    }
    CloseClipboard();
    return result;
}

bool clipboard_write_utf8(HWND owner, Utf8View text) noexcept {
    if (!owner || (!text.data && text.bytes != 0) || text.bytes > kWin32ConversionMaximum) {
        return false;
    }
    const int units = text.bytes == 0 ? 0
                                      : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data,
                                                            static_cast<int>(text.bytes), nullptr, 0);
    if (units < 0 || (text.bytes != 0 && units == 0)) {
        return false;
    }
    const SIZE_T allocation_bytes = (static_cast<SIZE_T>(units) + 1u) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, allocation_bytes);
    if (!memory) {
        return false;
    }
    auto* wide = static_cast<wchar_t*>(GlobalLock(memory));
    if (!wide) {
        GlobalFree(memory);
        return false;
    }
    const int written = text.bytes == 0 ? 0
                                        : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data,
                                                              static_cast<int>(text.bytes), wide, units);
    if (written != units) {
        GlobalUnlock(memory);
        GlobalFree(memory);
        return false;
    }
    wide[units] = 0;
    GlobalUnlock(memory);
    ClipboardPayload payload{CF_UNICODETEXT, memory};
    return clipboard_commit(owner, {&payload, 1});
}

u32 clipboard_html_image_fragment(Utf8View url, Slice<char> output) noexcept {
    if (!url.data || !url.bytes || url.bytes > kClipboardHtmlMaximumUrlBytes || !output.data ||
        !utf8_validate(url).valid)
        return 0;
    // CF_HTML: a header of fixed-width byte offsets, then the document.
    static constexpr char header[] = "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\n"
                                     "StartFragment:0000000000\r\nEndFragment:0000000000\r\n";
    static_assert(sizeof(header) - 1 == 105);
    u32 used = append(output, 0, header);
    const u32 start_html = used;
    used = append(output, used, "<html><body>\r\n<!--StartFragment-->");
    const u32 start_fragment = used;
    used = append(output, used, "<img src=\"");
    for (u32 index = 0; index < url.bytes && used != kAppendFailed; ++index) {
        const char c = url.data[index];
        if (static_cast<u8>(c) < 0x20)
            return 0;
        used = c == '&'   ? append(output, used, "&amp;")
               : c == '"' ? append(output, used, "&quot;")
               : c == '<' ? append(output, used, "&lt;")
               : c == '>' ? append(output, used, "&gt;")
                          : append(output, used, &c, 1);
    }
    used = append(output, used, "\">");
    const u32 end_fragment = used;
    used = append(output, used, "<!--EndFragment-->\r\n</body></html>");
    if (used == kAppendFailed)
        return 0;
    put_offset(output.data + 23, start_html);
    put_offset(output.data + 43, used);
    put_offset(output.data + 69, start_fragment);
    put_offset(output.data + 93, end_fragment);
    return used;
}

bool clipboard_write_image(HWND owner, const u8* rgba, u32 width, u32 height, u32 stride,
                           Utf8View source_url) noexcept {
    const u64 dib_bytes = clipboard_image_dib_bytes(width, height);
    const u64 png_bytes = clipboard_image_png_bytes(width, height);
    const UINT png_format = RegisterClipboardFormatW(L"PNG");
    const UINT html_format = source_url.bytes ? RegisterClipboardFormatW(L"HTML Format") : 0;
    if (!owner || !rgba || !dib_bytes || !png_bytes || stride < width * 4 || png_bytes > 0xffffffffull ||
        !png_format || (source_url.bytes && !html_format))
        return false;
    ClipboardPayload payloads[3]{{CF_DIBV5, GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(dib_bytes))},
                                 {png_format, GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(png_bytes))},
                                 {}};
    u32 count = 2;
    bool ok = payloads[0].data && payloads[1].data;
    if (ok) {
        auto* bytes = static_cast<u8*>(GlobalLock(payloads[0].data));
        ok = bytes && clipboard_image_build_dib(rgba, width, height, stride,
                                                {bytes, static_cast<u32>(dib_bytes)}) == dib_bytes;
        if (bytes)
            GlobalUnlock(payloads[0].data);
        auto* encoded = ok ? static_cast<u8*>(GlobalLock(payloads[1].data)) : nullptr;
        ok = ok && encoded &&
             clipboard_image_encode_png(rgba, width, height, stride,
                                        {encoded, static_cast<u32>(png_bytes)}) == png_bytes;
        if (encoded)
            GlobalUnlock(payloads[1].data);
    }
    if (ok && source_url.bytes) {
        // The image already costs width * height * 8 bytes; this bound is small.
        HGLOBAL html = GlobalAlloc(GMEM_MOVEABLE, kClipboardHtmlImageCapacity + 1);
        auto* text = html ? static_cast<char*>(GlobalLock(html)) : nullptr;
        const u32 bytes =
            text ? clipboard_html_image_fragment(source_url, {text, kClipboardHtmlImageCapacity}) : 0;
        if (text) {
            text[bytes] = 0; // readers stop at EndHTML; the terminator is customary
            GlobalUnlock(html);
        }
        // Publish exactly the payload, not the worst-case escaping bound.
        if (HGLOBAL exact = bytes ? GlobalReAlloc(html, bytes + 1, GMEM_MOVEABLE) : nullptr)
            html = exact;
        payloads[2] = {html_format, html};
        count = 3;
        ok = bytes != 0;
    }
    if (!ok) {
        for (auto& payload : payloads)
            if (payload.data)
                GlobalFree(payload.data);
        return false;
    }
    return clipboard_commit(owner, {payloads, count});
}

} // namespace maicord
