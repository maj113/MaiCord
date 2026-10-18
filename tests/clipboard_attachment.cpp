#include "file_drop_fixture.hpp"
#include "maicord/app/message_attachment_drafts.hpp"
#include "maicord/model/upload_allowance.hpp"
#include "maicord/protocol/upload_allowance.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/platform/clipboard_image.hpp"
#include "maicord/media/png.hpp"

#include <Windows.h>
namespace maicord {
namespace {
// A provider-owned virtual file. Returning its original allocation lets the test
// distinguish retaining the medium from copying the attachment into app memory.
struct Provider final : IDataObject {
    volatile LONG refs = 1;
    // Signaled whenever only the test's own reference remains.
    const HANDLE unreferenced = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HGLOBAL metadata = nullptr, payload = nullptr;
    CLIPFORMAT descriptor = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
    CLIPFORMAT contents = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
    u32 content_reads = 0;
    IStream* stream = nullptr;
    // Raw formats in offer order; stream entries answer only TYMED_ISTREAM,
    // like a WinRT DataPackage seen through the OLE clipboard.
    struct Raw { CLIPFORMAT format; HGLOBAL block; bool stream; };
    Raw raws[4]{};
    u32 raw_count = 0;
    const Raw* raw(CLIPFORMAT format) const noexcept {
        for (u32 i = 0; i < raw_count; ++i) if (raws[i].format == format) return &raws[i];
        return nullptr;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        *out = nullptr; if (!bytes_equal(&id, &__uuidof(IUnknown), sizeof(GUID)) &&
            !bytes_equal(&id, &__uuidof(IDataObject), sizeof(GUID))) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG left = InterlockedDecrement(&refs);
        if (left == 1) SetEvent(unreferenced);
        return left;
    }
    void await_unreferenced() noexcept {
        while (InterlockedCompareExchange(&refs, 1, 1) != 1) WaitForSingleObject(unreferenced, INFINITE);
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* request, STGMEDIUM* out) override {
        if (QueryGetData(request) != S_OK) return DV_E_FORMATETC;
        if (request->cfFormat == contents) ++content_reads;
        *out = {};
        if (const auto* entry = raw_count ? raw(request->cfFormat) : nullptr) {
            if (entry->stream) {
                out->tymed = TYMED_ISTREAM;
                return CreateStreamOnHGlobal(entry->block, FALSE, &out->pstm);
            }
            // HDROP is copied: the shell owns and frees what it reads.
            out->tymed = TYMED_HGLOBAL; out->hGlobal = entry->block;
            out->pUnkForRelease = this; AddRef(); return S_OK;
        }
        out->tymed = TYMED_HGLOBAL;
        out->hGlobal = request->cfFormat == descriptor ? metadata : payload;
        // ReleaseStgMedium releases IStream even with pUnkForRelease set.
        if (stream && request->cfFormat == contents) { out->tymed = TYMED_ISTREAM; out->pstm = stream; stream->AddRef(); }
        out->pUnkForRelease = this; AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override {
        if (raw_count) {
            const auto* entry = raw(f->cfFormat);
            return entry && (f->tymed & (entry->stream ? TYMED_ISTREAM : TYMED_HGLOBAL)) ? S_OK : DV_E_FORMATETC;
        }
        return (f->tymed & TYMED_HGLOBAL) && (f->cfFormat == descriptor || (f->cfFormat == contents && f->lindex == 0)) ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};
using Check = void (*)(bool, const char*) noexcept;
void store32(u8* p, u32 v) noexcept { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); p[2] = static_cast<u8>(v >> 16); p[3] = static_cast<u8>(v >> 24); }
// Writes a DIB header (and masks or palette after it); returns the pixel offset.
u32 dib_header(u8* out, u32 header, i32 width, i32 height, u16 bits, u32 compression, const u32* in_header_masks,
               const u32* masks_after, u32 masks_after_count, const u32* palette, u32 used) noexcept {
    clear_bytes(out, header);
    store32(out, header); store32(out + 4, static_cast<u32>(width)); store32(out + 8, static_cast<u32>(height));
    out[12] = 1; out[14] = static_cast<u8>(bits); store32(out + 16, compression); store32(out + 32, used);
    for (u32 i = 0; in_header_masks && i < 4 && 40 + i * 4 + 4 <= header; ++i) store32(out + 40 + i * 4, in_header_masks[i]);
    u32 offset = header;
    for (u32 i = 0; i < masks_after_count; ++i, offset += 4) store32(out + offset, masks_after[i]);
    for (u32 i = 0; i < used; ++i, offset += 4) store32(out + offset, palette[i]);
    return offset;
}
struct Decoded { PngImage image; u8* scan; bool ok; };
Decoded decode(const ClipboardPng& png, u8* scan, u32 capacity) noexcept {
    Decoded result{{}, scan, false};
    DeflateWorkspace workspace{};
    result.ok = png.data && png.bytes <= 0xffffffffu &&
                png_decode_scanlines({png.data, static_cast<u32>(png.bytes)}, &result.image, &workspace, {scan, capacity}).ok();
    return result;
}
bool pixel_is(const Decoded& d, u32 x, u32 y, u8 r, u8 g, u8 b, u8 a) noexcept {
    const u8* p = d.scan + u64{y} * d.image.row_stride + 1 + u64{x} * d.image.channels;
    return p[0] == r && p[1] == g && p[2] == b && (d.image.channels == 3 ? a == 255 : p[3] == a);
}
HGLOBAL global_copy(const void* data, u32 bytes) noexcept {
    const HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (void* target = block ? GlobalLock(block) : nullptr) { copy_bytes(target, data, bytes); GlobalUnlock(block); }
    return block;
}
// Runs one paste through the real worker and returns the picked attachment.
bool paste(Provider& provider, NativeFilePicker* picker) noexcept {
    ClipboardAttachmentData input{};
    return SUCCEEDED(CoMarshalInterThreadInterfaceInStream(__uuidof(IDataObject), &provider, &input.object)) &&
           native_file_picker_take_clipboard(picker, &input, 10, 1u << 20) == NativeFilePickerError::none &&
           file_drop_fixture::await(picker) && native_file_picker_status(picker).ok();
}
void settle(Provider& provider, NativeFilePicker* picker) noexcept {
    file_drop_fixture::reap(picker);
    provider.await_unreferenced();
}
void test_clipboard_images(Check check, Provider& provider) noexcept {
    ClipboardPasteKind plan[kClipboardPastePlanCapacity]{};
    check(clipboard_paste_plan({true, false, true, true, false}, plan) == 3 && plan[0] == ClipboardPasteKind::png &&
              plan[1] == ClipboardPasteKind::dib && plan[2] == ClipboardPasteKind::files &&
              clipboard_paste_plan({false, false, false, true, true}, plan) == 2 && plan[0] == ClipboardPasteKind::files &&
              plan[1] == ClipboardPasteKind::virtual_files && clipboard_paste_plan({}, plan) == 0,
          "paste prefers image data over file lists and keeps files as a fallback");
    const u32 scan_capacity = 1u << 20;
    auto* scan = static_cast<u8*>(VirtualAlloc(nullptr, scan_capacity + (1u << 20), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!scan) { check(false, "clipboard image test allocation"); return; }
    u8* work = scan + scan_capacity; // DIB/PNG fixtures
    ClipboardPng png{};
    // Stored PNG with GlobalAlloc-style padding: uploaded unchanged, padding trimmed.
    u8 rgba[4 * 4 * 4];
    for (u32 i = 0; i < sizeof(rgba); ++i) rgba[i] = static_cast<u8>(i * 7);
    const u64 png_bytes = clipboard_image_encode_png(rgba, 4, 4, 16, {work, 4096});
    check(png_bytes && clipboard_image_png_length({work, static_cast<u32>(png_bytes) + 64}) == png_bytes &&
              !clipboard_image_png_length({work, static_cast<u32>(png_bytes) - 1}) &&
              clipboard_paste_png(ClipboardPasteKind::png, {work, static_cast<u32>(png_bytes) + 64}, 1u << 20, &png) == ERROR_SUCCESS &&
              png.bytes == png_bytes && bytes_equal(png.data, work, static_cast<u32>(png_bytes)),
          "registered PNG bytes paste unchanged without allocator padding");
    clipboard_png_release(&png);
    check(clipboard_paste_png(ClipboardPasteKind::png, {work, static_cast<u32>(png_bytes)}, png_bytes - 1, &png) == ERROR_FILE_TOO_LARGE && !png.data,
          "oversized PNG reports the upload allowance");
    // Snipping Tool's CF_DIB: BITMAPINFOHEADER, BI_BITFIELDS, masks after the header, bottom-up, unused alpha.
    const u32 rgb_masks[3]{0x00ff0000, 0x0000ff00, 0x000000ff};
    u32 offset = dib_header(work, 40, 5, 3, 32, BI_BITFIELDS, nullptr, rgb_masks, 3, nullptr, 0);
    for (u32 row = 0; row < 3; ++row)
        for (u32 x = 0; x < 5; ++x) {
            const u32 y = 2 - row;
            u8* p = work + offset + (row * 5 + x) * 4;
            p[0] = static_cast<u8>(x * 40); p[1] = static_cast<u8>(y * 80); p[2] = 200; p[3] = 0;
        }
    const u32 snip_dib_bytes = offset + 5 * 3 * 4;
    auto decoded = clipboard_paste_png(ClipboardPasteKind::dib, {work, snip_dib_bytes}, 1u << 20, &png) == ERROR_SUCCESS
                       ? decode(png, scan, scan_capacity) : Decoded{};
    check(decoded.ok && decoded.image.width == 5 && decoded.image.height == 3 && decoded.image.color_type == 2 &&
              pixel_is(decoded, 0, 0, 200, 0, 0, 255) && pixel_is(decoded, 4, 2, 200, 160, 160, 255) && pixel_is(decoded, 2, 1, 200, 80, 80, 255),
          "BI_BITFIELDS DIB with trailing masks becomes an opaque PNG in row order");
    clipboard_png_release(&png);
    // BITMAPV5HEADER, top-down, real alpha.
    const u32 v5_masks[4]{0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000};
    offset = dib_header(work, 124, 4, -2, 32, BI_BITFIELDS, v5_masks, nullptr, 0, nullptr, 0);
    for (u32 i = 0; i < 8; ++i) { u8* p = work + offset + i * 4; p[0] = 10; p[1] = 20; p[2] = 30; p[3] = static_cast<u8>(i * 30); }
    decoded = clipboard_paste_png(ClipboardPasteKind::dibv5, {work, offset + 32}, 1u << 20, &png) == ERROR_SUCCESS
                  ? decode(png, scan, scan_capacity) : Decoded{};
    check(decoded.ok && decoded.image.color_type == 6 && pixel_is(decoded, 0, 0, 30, 20, 10, 0) && pixel_is(decoded, 3, 1, 30, 20, 10, 210),
          "top-down DIBV5 keeps straight alpha");
    clipboard_png_release(&png);
    for (u32 i = 0; i < 8; ++i) work[offset + i * 4 + 3] = 0;
    decoded = clipboard_paste_png(ClipboardPasteKind::dibv5, {work, offset + 32}, 1u << 20, &png) == ERROR_SUCCESS
                  ? decode(png, scan, scan_capacity) : Decoded{};
    check(decoded.ok && decoded.image.color_type == 2 && pixel_is(decoded, 1, 1, 30, 20, 10, 255),
          "uniformly zero alpha is treated as an unused byte, not transparency");
    clipboard_png_release(&png);
    // Windows synthesizes CF_DIBV5 from CF_DIB with the masks repeated after the V5 header.
    const u32 v5_rgb_masks[4]{0x00ff0000, 0x0000ff00, 0x000000ff, 0};
    offset = dib_header(work, 124, 2, 1, 32, BI_BITFIELDS, v5_rgb_masks, rgb_masks, 3, nullptr, 0);
    store32(work + offset, 0x00112233); store32(work + offset + 4, 0x00445566);
    decoded = clipboard_paste_png(ClipboardPasteKind::dibv5, {work, offset + 8}, 1u << 20, &png) == ERROR_SUCCESS
                  ? decode(png, scan, scan_capacity) : Decoded{};
    check(decoded.ok && decoded.image.color_type == 2 && pixel_is(decoded, 0, 0, 0x11, 0x22, 0x33, 255) &&
              pixel_is(decoded, 1, 0, 0x44, 0x55, 0x66, 255),
          "synthesized DIBV5 skips masks repeated after its header");
    clipboard_png_release(&png);
    // 24-bit with row padding, 8-bit palette, and 16-bit 565 bitfields.
    offset = dib_header(work, 40, 3, 2, 24, BI_RGB, nullptr, nullptr, 0, nullptr, 0);
    clear_bytes(work + offset, 24);
    work[offset + 0] = 1; work[offset + 1] = 2; work[offset + 2] = 3;        // bottom row, x=0
    work[offset + 12 + 6] = 7; work[offset + 12 + 7] = 8; work[offset + 12 + 8] = 9; // top row, x=2
    decoded = clipboard_paste_png(ClipboardPasteKind::dib, {work, offset + 24}, 1u << 20, &png) == ERROR_SUCCESS
                  ? decode(png, scan, scan_capacity) : Decoded{};
    check(decoded.ok && pixel_is(decoded, 0, 1, 3, 2, 1, 255) && pixel_is(decoded, 2, 0, 9, 8, 7, 255),
          "24-bit DIB honours four-byte row padding");
    clipboard_png_release(&png);
    const u32 palette[3]{0x00ff0000, 0x0000ff00, 0x000000ff};
    offset = dib_header(work, 40, 5, 1, 8, BI_RGB, nullptr, nullptr, 0, palette, 3);
    const u8 indices[8]{0, 1, 2, 1, 0, 0, 0, 0};
    copy_bytes(work + offset, indices, 8);
    decoded = clipboard_paste_png(ClipboardPasteKind::dib, {work, offset + 8}, 1u << 20, &png) == ERROR_SUCCESS
                  ? decode(png, scan, scan_capacity) : Decoded{};
    check(decoded.ok && pixel_is(decoded, 0, 0, 255, 0, 0, 255) && pixel_is(decoded, 2, 0, 0, 0, 255, 255) && pixel_is(decoded, 3, 0, 0, 255, 0, 255),
          "palette DIB expands through its colour table");
    clipboard_png_release(&png);
    const u32 masks565[3]{0xf800, 0x07e0, 0x001f};
    offset = dib_header(work, 40, 2, 1, 16, BI_BITFIELDS, nullptr, masks565, 3, nullptr, 0);
    work[offset] = 0x00; work[offset + 1] = 0xf8; work[offset + 2] = 0xe0; work[offset + 3] = 0x07;
    decoded = clipboard_paste_png(ClipboardPasteKind::dib, {work, offset + 4}, 1u << 20, &png) == ERROR_SUCCESS
                  ? decode(png, scan, scan_capacity) : Decoded{};
    check(decoded.ok && pixel_is(decoded, 0, 0, 255, 0, 0, 255) && pixel_is(decoded, 1, 0, 0, 255, 0, 255),
          "16-bit 565 bitfields scale to eight bits");
    clipboard_png_release(&png);
    check(clipboard_paste_png(ClipboardPasteKind::dib, {work, offset + 3}, 1u << 20, &png) == ERROR_INVALID_DATA &&
              (work[14] = 2, clipboard_paste_png(ClipboardPasteKind::dib, {work, offset + 4}, 1u << 20, &png) == ERROR_INVALID_DATA),
          "truncated or unsupported DIBs are rejected");
    // A screenshot-like image compresses and round-trips; noise falls back safely. 160x140 keeps the
    // filtered stream (140 * 481 bytes) above one 65535-byte stored block, so the fallback still splits.
    constexpr u32 kWide = 160, kTall = 140;
    offset = dib_header(work, 40, kWide, kTall, 32, BI_RGB, nullptr, nullptr, 0, nullptr, 0);
    u32 seed = 1;
    for (u32 pass = 0; pass < 2; ++pass) {
        for (u32 i = 0; i < kWide * kTall; ++i) {
            u8* p = work + offset + i * 4;
            const u32 x = i % kWide, y = i / kWide;
            seed = seed * 1103515245u + 12345u;
            const u8 v = pass ? static_cast<u8>(seed >> 16) : static_cast<u8>(x < kWide / 2 ? 40 : x + y);
            p[0] = v; p[1] = static_cast<u8>(pass ? seed >> 8 : y); p[2] = static_cast<u8>(pass ? seed >> 24 : 90); p[3] = 0;
        }
        decoded = clipboard_paste_png(ClipboardPasteKind::dib, {work, offset + kWide * kTall * 4}, 1u << 20, &png) == ERROR_SUCCESS
                      ? decode(png, scan, scan_capacity) : Decoded{};
        bool same = decoded.ok && decoded.image.width == kWide && decoded.image.height == kTall;
        for (u32 i = 0; same && i < kWide * kTall; ++i) {
            const u8* p = work + offset + i * 4;
            same = pixel_is(decoded, i % kWide, kTall - 1 - i / kWide, p[2], p[1], p[0], 255);
        }
        check(same && (pass || png.bytes < kWide * kTall * 3 / 8),
              pass ? "incompressible DIB still produces an exact PNG" : "DIB conversion deflates and round-trips exactly");
        clipboard_png_release(&png);
    }
    check(clipboard_paste_png(ClipboardPasteKind::dib, {work, offset + kWide * kTall * 4}, 4096, &png) == ERROR_FILE_TOO_LARGE && !png.data,
          "converted image over the allowance reports too large");

    // End to end through OLE and the picker worker.
    u8* scratch = work + 512 * 1024;
    const u32 padded_png = static_cast<u32>(png_bytes) + 64;
    clipboard_image_encode_png(rgba, 4, 4, 16, {scratch, 4096});
    clear_bytes(scratch + png_bytes, 64);
    offset = dib_header(work, 40, 5, 3, 32, BI_BITFIELDS, nullptr, rgb_masks, 3, nullptr, 0);
    for (u32 i = 0; i < 15; ++i) { u8* p = work + offset + i * 4; p[0] = 1; p[1] = 2; p[2] = 3; p[3] = 0; }
    wchar_t path[MAX_PATH]{}; u32 path_units = 0; FileSnapshot snapshot{};
    const bool seeded = file_drop_fixture::seed(path, &path_units, &snapshot);
    const ConstSlice<wchar_t> paths[]{{path, path_units}};
    const HGLOBAL drop = seeded ? reinterpret_cast<HGLOBAL>(file_drop_fixture::make({paths, 1})) : nullptr;
    const CLIPFORMAT png_format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"PNG"));
    const HGLOBAL good_png = global_copy(scratch, padded_png), dib = global_copy(work, snip_dib_bytes);
    scratch[20] ^= 0xff; scratch[0] = 0; // not a PNG any more
    const HGLOBAL bad_png = global_copy(scratch, padded_png);
    NativeFilePicker picker{};
    provider.raws[0] = {png_format, good_png, true}; provider.raws[1] = {CF_DIB, dib, false};
    provider.raws[2] = {CF_HDROP, drop, false}; provider.raw_count = 3;
    auto source = UploadSource{};
    bool ok = paste(provider, &picker);
    auto filename = native_file_picker_filename(&picker, 0);
    source = native_file_picker_source(&picker, 0);
    auto bytes = source.ops ? source.ops->read(source.context, 0, {scratch + 8192, 8192}) : ConstSlice<u8>{};
    clipboard_image_encode_png(rgba, 4, 4, 16, {scratch, 4096});
    check(ok && filename.count == 9 && bytes_equal(filename.data, L"image.png", 18) && !native_file_picker_path(&picker, 0).count &&
              bytes.count == png_bytes && bytes_equal(bytes.data, scratch, static_cast<u32>(png_bytes)),
          "stream-only PNG (Snipping Tool) pastes as image.png ahead of its bitmap and file formats");
    const bool owned_source = upload_source_valid(source);
    if (owned_source) upload_source_retain(source);
    const auto immutable = owned_source
        ? source.ops->read(source.context, 0, {nullptr, static_cast<u32>(source.bytes)}) : ConstSlice<u8>{};
    const auto immutable_part = owned_source ? source.ops->read(source.context, 2, {nullptr, 3}) : ConstSlice<u8>{};
    check(owned_source && immutable.data && immutable.data == bytes.data && immutable.count == png_bytes &&
              immutable_part.data == immutable.data + 2 && immutable_part.count == 3 &&
              !source.ops->read(source.context, 0, {nullptr, 0}).data &&
              !source.ops->read(source.context, source.bytes, {nullptr, 1}).data &&
              !source.ops->read(source.context, 1, {nullptr, static_cast<u32>(source.bytes)}).data &&
              !source.ops->read(source.context, ~u64{0}, {nullptr, 1}).data,
          "owned clipboard PNG supports exact immutable ranges and rejects empty or out-of-range requests");
    settle(provider, &picker);
    const auto later = owned_source ? source.ops->read(source.context, 2, {scratch + 8192, 3}) : ConstSlice<u8>{};
    check(owned_source && later.count == 3 && bytes_equal(later.data, scratch + 2, 3) &&
              immutable.count == png_bytes && bytes_equal(immutable.data, scratch, static_cast<u32>(png_bytes)),
          "retained immutable PNG view survives picker release and later ordinary reads");
    if (owned_source) upload_source_release(source);
    provider.raws[0].block = bad_png; provider.raws[0].stream = false;
    ok = paste(provider, &picker);
    source = native_file_picker_source(&picker, 0);
    bytes = source.ops ? source.ops->read(source.context, 0, {scratch + 8192, 8192}) : ConstSlice<u8>{};
    png = {const_cast<u8*>(bytes.data), bytes.count};
    decoded = ok && source.bytes == bytes.count ? decode(png, scan, scan_capacity) : Decoded{};
    check(decoded.ok && decoded.image.width == 5 && pixel_is(decoded, 4, 2, 3, 2, 1, 255),
          "unreadable PNG falls back to the DIB, converted to PNG");
    const auto converted_view = upload_source_valid(source)
        ? source.ops->read(source.context, 0, {nullptr, static_cast<u32>(source.bytes)}) : ConstSlice<u8>{};
    check(converted_view.data == bytes.data && converted_view.count == source.bytes && converted_view.count != 0,
          "converted DIB owns immutable PNG bytes too");
    png = {};
    settle(provider, &picker);
    provider.raws[1] = provider.raws[2]; provider.raw_count = 2;
    ok = paste(provider, &picker);
    const auto picked = native_file_picker_path(&picker, 0);
    check(seeded && ok && native_file_picker_status(&picker).count == 1 && picked.count == path_units &&
              bytes_equal(picked.data, path, path_units * 2),
          "with no readable image the clipboard file list is used");
    settle(provider, &picker);
    provider.raw_count = 0;
    GlobalFree(good_png); GlobalFree(bad_png); GlobalFree(dib);
    if (drop) GlobalFree(drop);
    if (seeded) DeleteFileW(path);
    VirtualFree(scan, 0, MEM_RELEASE);
}
}
void test_clipboard_attachment(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace file_drop_fixture;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) { check(false, "clipboard test MTA initialization"); return; }
    Provider provider;
    provider.metadata = GlobalAlloc(GHND, sizeof(FILEGROUPDESCRIPTORW));
    provider.payload = GlobalAlloc(GHND, 8);
    auto* metadata = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(provider.metadata));
    auto* payload = static_cast<u8*>(GlobalLock(provider.payload));
    if (!metadata || !payload) { check(false, "virtual clipboard fixture allocation"); CoUninitialize(); return; }
    metadata->cItems = 1; metadata->fgd[0].dwFlags = FD_FILESIZE; metadata->fgd[0].nFileSizeLow = 8;
    constexpr wchar_t name[] = L"report-\u017e.pdf";
    copy_bytes(metadata->fgd[0].cFileName, name, sizeof(name));
    copy_bytes(payload, "pdfbytes", 8);
    ClipboardAttachmentData input{};
    NativeFilePicker picker{};
    check(SUCCEEDED(CoMarshalInterThreadInterfaceInStream(__uuidof(IDataObject), &provider, &input.object)) &&
        native_file_picker_take_clipboard(&picker, &input, 10, 1024) == NativeFilePickerError::none && await(&picker) &&
        native_file_picker_status(&picker).ok(), "named arbitrary clipboard file prepares without disk staging");
    const auto filename = native_file_picker_filename(&picker, 0);
    auto source = native_file_picker_source(&picker, 0);
    u8 scratch[8]{};
    const auto bytes = source.ops ? source.ops->read(source.context, 0, {scratch, 8}) : ConstSlice<u8>{};
    check(filename.count == sizeof(name) / sizeof(wchar_t) - 1 &&
        bytes_equal(filename.data, name, filename.count * sizeof(wchar_t)) && !native_file_picker_path(&picker, 0).count,
        "virtual clipboard file preserves Unicode filename and has no staging path");
    check(bytes.data == payload && bytes.count == 8 && bytes_equal(bytes.data, "pdfbytes", 8),
        "clipboard upload reads the provider allocation directly without a payload copy");
    const auto global_view = source.ops ? source.ops->read(source.context, 0, {nullptr, 8}) : ConstSlice<u8>{};
    check(source.ops && !global_view.data && !global_view.count && bytes_equal(payload, "pdfbytes", 8),
        "provider-owned HGLOBAL refuses immutable borrow without changing ordinary bytes");
    MessageAttachmentDraftStore drafts{};
    MessageAttachmentFile file{{}, {}, filename, source};
    native_file_picker_snapshot(&picker, 0, &file.snapshot);
    u64 id = 0;
    check(message_attachment_drafts_initialize(&drafts) &&
        message_attachment_drafts_add(&drafts, 123, {&file, 1}, {&id, 1}) == MessageAttachmentDraftError::none,
        "virtual clipboard source enters ordinary attachment draft");
    reap(&picker);
    const auto* draft = message_attachment_drafts_get(&drafts, id);
    const auto retained = draft && draft->content.ops ? draft->content.ops->read(draft->content.context, 2, {scratch, 3}) : ConstSlice<u8>{};
    check(retained.data == payload + 2 && retained.count == 3, "draft retains original bytes after preparation worker closes");
    message_attachment_drafts_release(&drafts);
    provider.await_unreferenced();
    provider.content_reads = 0;
    check(SUCCEEDED(CoMarshalInterThreadInterfaceInStream(__uuidof(IDataObject), &provider, &input.object)) &&
        native_file_picker_take_clipboard(&picker, &input, 10, 7) == NativeFilePickerError::none && await(&picker) &&
        native_file_picker_status(&picker).native_error == ERROR_FILE_TOO_LARGE && provider.content_reads == 0,
        "account allowance rejects oversized named file before requesting its contents");
    reap(&picker); clipboard_attachment_release(&input);
    provider.await_unreferenced();

    ULONG written = 0;
    check(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &provider.stream)) &&
        SUCCEEDED(provider.stream->Write("pdfbytes", 8, &written)) && written == 8 &&
        SUCCEEDED(CoMarshalInterThreadInterfaceInStream(__uuidof(IDataObject), &provider, &input.object)) &&
        native_file_picker_take_clipboard(&picker, &input, 10, 1024) == NativeFilePickerError::none && await(&picker) &&
        native_file_picker_status(&picker).ok(), "clipboard accepts indexed IStream file contents without a temporary file");
    source = native_file_picker_source(&picker, 0); upload_source_retain(source); reap(&picker);
    LARGE_INTEGER query_position{}; query_position.QuadPart = 2;
    const bool positioned = SUCCEEDED(provider.stream->Seek(query_position, STREAM_SEEK_SET, nullptr));
    const auto stream_view = source.ops ? source.ops->read(source.context, 0, {nullptr, 8}) : ConstSlice<u8>{};
    query_position.QuadPart = 0; ULARGE_INTEGER retained_position{};
    check(positioned && source.ops && !stream_view.data && !stream_view.count &&
              SUCCEEDED(provider.stream->Seek(query_position, STREAM_SEEK_CUR, &retained_position)) &&
              retained_position.QuadPart == 2,
        "clipboard IStream refuses immutable borrow without a seek or read");
    const auto streamed = source.ops ? source.ops->read(source.context, 3, {scratch, 5}) : ConstSlice<u8>{};
    check(streamed.data == scratch && streamed.count == 5 && bytes_equal(streamed.data, "bytes", 5),
        "retained clipboard stream seeks directly into the existing upload scratch buffer");
    upload_source_release(source);
    provider.await_unreferenced();
    if (provider.stream) { provider.stream->Release(); provider.stream = nullptr; }

    GlobalUnlock(provider.payload); GlobalFree(provider.payload); provider.payload = nullptr;
    test_clipboard_images(check, provider);
    GlobalUnlock(provider.metadata); GlobalFree(provider.metadata);
    CoUninitialize(); CloseHandle(provider.unreferenced);

    UploadPolicy policy{};
    check(upload_allowance_bytes(0, 0, policy) == 10u * kUploadMiB &&
        upload_allowance_bytes(1, 0, policy) == 50u * kUploadMiB &&
        upload_allowance_bytes(3, 0, policy) == 50u * kUploadMiB &&
        upload_allowance_bytes(2, 0, policy) == 500u * kUploadMiB &&
        upload_allowance_bytes(0, 4, policy) == 250u * kUploadMiB,
        "account and guild attachment allowances");
    constexpr auto apex = utf8_literal("{\"assignments\":{\"1\":{\"123\":{\"assignments\":[[3942863072,3,0],[587404649,1,0],[981354709,1,0],[3383307794,1,0]]}}}}");
    u32 structural[512]{};
    JsonValue value{}; value.kind = JsonValueKind::object; value.source = apex;
    check(upload_policy_decode(value, 123, {structural, 512}, &policy) && policy.floor_bytes == 20u * kUploadMiB &&
        upload_allowance_bytes(2, 4, policy) == 1024u * kUploadMiB && upload_allowance_bytes(0, 4, policy) == 20u * kUploadMiB,
        "Apex assignments apply base rollout precedence, Nitro rollout, and guild holdout");
    constexpr auto state = utf8_literal("{\"apex_experiments\":{\"assignments\":{\"1\":{\"456\":{\"assignments\":[]}}}}}");
    value.source = state; bool changed = true;
    check(upload_policy_state(value, 123, {structural, 512}, &policy, &changed) && !changed && policy.nitro_bytes == 1024u * kUploadMiB,
        "another user's state update does not clear the account allowance");
    constexpr auto eligibility = utf8_literal("{\"assignments\":{\"1\":{\"123\":{\"assignments\":[[981354709,1,8],[3942863072,2,null]]}}}}");
    value.source = eligibility;
    check(upload_policy_decode(value, 123, {structural, 512}, &policy) && !policy.nitro_bytes && policy.floor_bytes == 20u * kUploadMiB,
        "eligibility-only experiments do not grant a larger upload allowance");
    GatewayEnvelope envelope{}; envelope.opcode = 0; envelope.has_event_name = true;
    envelope.event_name = {utf8_literal("USER_UPDATE"), false};
    envelope.data.kind = JsonValueKind::object; envelope.data.source = utf8_literal("{\"id\":\"123\",\"premium_type\":2}");
    GatewayClientEvent event{}; ConstSlice<u8> event_payload{}; char payload_storage[1024]{};
    check(gateway_client_route_dispatch(envelope, {structural, 512}, 64, {payload_storage, 1024}, {}, 123, &event, &event_payload).ok() &&
        event.dispatch_kind == GatewayClientDispatchKind::user_snapshot && event.state.record_count == 1,
        "gateway subscription changes reach the account user snapshot path");
    GatewayClientUserMutation user{}; Utf8View display{}, avatar{}; u32 cursor = 0;
    check(gateway_client_user_record(event_payload, &cursor, &user, &display, &avatar) && user.premium_type == 2 && (user.flags & 4),
        "user snapshot preserves the premium tier separately from identity data");
    envelope.event_name = {utf8_literal("STATE_UPDATE"), false};
    envelope.data.source = utf8_literal("{\"apex_experiments\":{\"assignments\":{\"1\":{\"123\":{\"assignments\":[[587404649,1,0]]}}}}}");
    check(gateway_client_route_dispatch(envelope, {structural, 512}, 64, {payload_storage, 1024}, {}, 123, &event, &event_payload).ok() &&
        event.state.upload_policy_known && event.state.upload_policy.floor_bytes == 20u * kUploadMiB && event.state.record_count == 0,
        "gateway state updates carry changed upload experiment assignments");
    envelope.event_name = {utf8_literal("GUILD_UPDATE"), false};
    envelope.data.source = utf8_literal("{\"id\":\"456\",\"name\":\"guild\",\"owner_id\":\"123\",\"roles\":[],\"features\":[\"MAX_FILE_SIZE_50_MB\",\"MAX_FILE_SIZE_250_MB\"]}");
    check(gateway_client_route_dispatch(envelope, {structural, 512}, 64, {payload_storage, 1024}, {}, 123, &event, &event_payload).ok() &&
        event.state.upload_features_known && event.state.upload_features == 5,
        "gateway guild updates retain advertised per-file allowances");
}
}

