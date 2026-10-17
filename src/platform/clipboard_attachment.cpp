#include "maicord/platform/clipboard_attachment.hpp"
#include "maicord/platform/clipboard_image.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include <shlobj.h>
#include <shellapi.h>
namespace maicord {
#if defined(MAICORD_CLIPBOARD_UI_FIXTURE)
IDataObject* clipboard_attachment_ui_fixture_provider() noexcept;
#endif
namespace {
bool ole_initialized;
struct Content {
    STGMEDIUM medium;
    CO_MTA_USAGE_COOKIE apartment;
    PTP_WORK cleanup;
    volatile LONG references;
    u64 bytes;
    const u8* memory;
    u8* owned; // converted image bytes (VirtualAlloc); no medium
    // Upload, preview and duration readers share one stream; Seek+Read is
    // not atomic, so stream-backed reads are serialized (zeroed = SRWLOCK_INIT).
    SRWLOCK stream_lock;
};
void CALLBACK cleanup(PTP_CALLBACK_INSTANCE, void* opaque, PTP_WORK work) noexcept {
    auto* source = static_cast<Content*>(opaque);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (source->owned) VirtualFree(source->owned, 0, MEM_RELEASE);
    else if (source->memory) GlobalUnlock(source->medium.hGlobal);
    if (source->medium.tymed) ReleaseStgMedium(&source->medium);
    if (SUCCEEDED(initialized)) CoUninitialize();
    CoDecrementMTAUsage(source->apartment);
    CloseThreadpoolWork(work);
    VirtualFree(source, 0, MEM_RELEASE);
}
void retain(void* opaque) noexcept { InterlockedIncrement(&static_cast<Content*>(opaque)->references); }
void release(void* opaque) noexcept {
    auto* source = static_cast<Content*>(opaque);
    if (InterlockedDecrement(&source->references) == 0) SubmitThreadpoolWork(source->cleanup);
}
ConstSlice<u8> read(void* opaque, u64 offset, Slice<u8> scratch) noexcept {
    auto& source = *static_cast<Content*>(opaque);
    if (offset >= source.bytes || !scratch.count) return {};
    if (!scratch.data) {
        // Only converted PNG storage belongs to this source. A locked provider
        // HGLOBAL has a stable address, but its bytes can still change.
        if (!source.owned || scratch.count > source.bytes - offset) return {};
        return {source.owned + offset, scratch.count};
    }
    const u32 count = static_cast<u32>(minimum<u64>(source.bytes - offset, scratch.count));
    if (source.memory) return {source.memory + offset, count};
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    ULONG copied = 0;
    AcquireSRWLockExclusive(&source.stream_lock);
    const bool read = SUCCEEDED(source.medium.pstm->Seek(position, STREAM_SEEK_SET, nullptr)) &&
                      SUCCEEDED(source.medium.pstm->Read(scratch.data, count, &copied)) && copied == count;
    ReleaseSRWLockExclusive(&source.stream_lock);
    if (!read) return {};
    return {scratch.data, copied};
}
const UploadSourceOps operations{retain, release, read};
bool get(IDataObject* object, CLIPFORMAT format, LONG index, DWORD types, STGMEDIUM* medium) noexcept {
    FORMATETC request{format, nullptr, DVASPECT_CONTENT, index, types};
    if (FAILED(object->GetData(&request, medium))) return false;
    if ((medium->tymed == TYMED_HGLOBAL && (types & TYMED_HGLOBAL) && medium->hGlobal) ||
        (medium->tymed == TYMED_ISTREAM && (types & TYMED_ISTREAM) && medium->pstm)) return true;
    ReleaseStgMedium(medium); *medium = {}; return false;
}
bool available(IDataObject* object, CLIPFORMAT format, DWORD types = TYMED_HGLOBAL) noexcept {
    constexpr DWORD kinds[]{TYMED_HGLOBAL, TYMED_ISTREAM};
    for (const DWORD type : kinds) {
        FORMATETC request{format, nullptr, DVASPECT_CONTENT, -1, type};
        if ((types & type) && object->QueryGetData(&request) == S_OK) return true;
    }
    return false;
}
// Some providers (WinRT DataPackage behind the OLE clipboard) answer
// QueryGetData only for their exact medium; the enumeration is authoritative.
bool enumerated(IDataObject* object, CLIPFORMAT format) noexcept {
    IEnumFORMATETC* formats = nullptr;
    if (FAILED(object->EnumFormatEtc(DATADIR_GET, &formats)) || !formats) return false;
    FORMATETC entry{};
    bool found = false;
    for (u32 guard = 0; !found && guard < 256 && formats->Next(1, &entry, nullptr) == S_OK; ++guard) {
        found = entry.cfFormat == format && (entry.tymed & (TYMED_HGLOBAL | TYMED_ISTREAM));
        if (entry.ptd) CoTaskMemFree(entry.ptd);
    }
    formats->Release();
    return found;
}
u32 length(const wchar_t* text, u32 maximum) noexcept { u32 n = 0; while (n < maximum && text[n]) ++n; return n; }
bool valid_drop(HGLOBAL block) noexcept {
    const SIZE_T bytes = GlobalSize(block);
    const auto* header = static_cast<const DROPFILES*>(GlobalLock(block));
    bool valid = false;
    if (header && bytes >= sizeof(DROPFILES) && header->pFiles >= sizeof(DROPFILES) && header->pFiles < bytes) {
        const auto* names = reinterpret_cast<const u8*>(header) + header->pFiles;
        const SIZE_T remaining = bytes - header->pFiles;
        if (header->fWide && !(header->pFiles & 1)) {
            const auto* wide = reinterpret_cast<const wchar_t*>(names);
            for (SIZE_T i = 1; i < remaining / sizeof(wchar_t); ++i) if (!wide[i - 1] && !wide[i]) { valid = true; break; }
        } else if (!header->fWide) {
            for (SIZE_T i = 1; i < remaining; ++i) if (!names[i - 1] && !names[i]) { valid = true; break; }
        }
    }
    if (header) GlobalUnlock(block);
    return valid;
}
Content* content_create(DWORD* error) noexcept {
    *error = ERROR_NOT_ENOUGH_MEMORY;
    auto* content = static_cast<Content*>(VirtualAlloc(nullptr, sizeof(Content), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!content) return nullptr;
    content->cleanup = CreateThreadpoolWork(cleanup, content, nullptr);
    if (!content->cleanup || FAILED(CoIncrementMTAUsage(&content->apartment))) {
        if (content->cleanup) CloseThreadpoolWork(content->cleanup);
        VirtualFree(content, 0, MEM_RELEASE); return nullptr;
    }
    content->references = 1;
    return content;
}
UploadSource adopt_png(ClipboardPng* png, DWORD* error) noexcept {
    auto* content = content_create(error);
    if (!content) { clipboard_png_release(png); return {}; }
    content->owned = png->data; content->memory = png->data; content->bytes = png->bytes; *png = {};
    return {content, &operations, content->bytes};
}
UploadSource adopt(STGMEDIUM* medium, u64 expected, u64 limit, DWORD* error) noexcept {
    auto* content = content_create(error);
    if (!content) return {};
    content->medium = *medium; *medium = {};
    *error = ERROR_INVALID_DATA;
    u64 bytes = 0;
    if (content->medium.tymed == TYMED_HGLOBAL) {
        bytes = GlobalSize(content->medium.hGlobal);
        if (expected && expected <= bytes) bytes = expected;
        else if (expected) bytes = 0;
        content->memory = static_cast<const u8*>(GlobalLock(content->medium.hGlobal));
        if (!content->memory) bytes = 0;
    } else if (content->medium.tymed == TYMED_ISTREAM) {
        STATSTG stat{};
        bytes = expected;
        if (SUCCEEDED(content->medium.pstm->Stat(&stat, STATFLAG_NONAME))) bytes = stat.cbSize.QuadPart;
        if (expected && bytes != expected) bytes = 0;
    }
    if (!bytes || bytes > limit) {
        if (bytes) *error = ERROR_FILE_TOO_LARGE;
        release(content); return {};
    }
    content->bytes = bytes;
    return {content, &operations, content->bytes};
}
DWORD visit_files(IDataObject* object, u32 maximum, ClipboardAttachmentVisitor visitor, void* context, u32* count) noexcept {
    STGMEDIUM metadata{};
    if (!get(object, CF_HDROP, -1, TYMED_HGLOBAL, &metadata)) return ERROR_INVALID_DATA;
    if (!valid_drop(metadata.hGlobal)) { ReleaseStgMedium(&metadata); return ERROR_INVALID_DATA; }
    const u32 files = DragQueryFileW(static_cast<HDROP>(metadata.hGlobal), 0xffffffffu, nullptr, 0);
    DWORD error = !files ? ERROR_INVALID_DATA : files > maximum ? ERROR_TOO_MANY_NAMES : ERROR_SUCCESS;
    auto* path = error ? nullptr : static_cast<wchar_t*>(VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!error && !path) error = ERROR_NOT_ENOUGH_MEMORY;
    for (u32 i = 0; !error && i < files; ++i) {
        const u32 units = DragQueryFileW(static_cast<HDROP>(metadata.hGlobal), i, path, 32768);
        if (!units || units >= 32768 || !visitor(context, {path, units}, {}, {})) error = ERROR_INVALID_DATA;
        else ++*count;
    }
    if (path) VirtualFree(path, 0, MEM_RELEASE);
    ReleaseStgMedium(&metadata); return error;
}
DWORD visit_virtual_files(IDataObject* object, CLIPFORMAT descriptors, u32 maximum, u64 limit,
    ClipboardAttachmentVisitor visitor, void* context, u32* count) noexcept {
    const auto contents = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
    STGMEDIUM metadata{};
    if (!get(object, descriptors, -1, TYMED_HGLOBAL, &metadata)) return ERROR_INVALID_DATA;
    const SIZE_T bytes = GlobalSize(metadata.hGlobal);
    const auto* group = static_cast<const FILEGROUPDESCRIPTORW*>(GlobalLock(metadata.hGlobal));
    DWORD error = !group || bytes < sizeof(UINT) || !group->cItems ? ERROR_INVALID_DATA :
        group->cItems > maximum ? ERROR_TOO_MANY_NAMES :
        sizeof(UINT) + static_cast<u64>(group->cItems) * sizeof(FILEDESCRIPTORW) > bytes ? ERROR_INVALID_DATA : ERROR_SUCCESS;
    for (u32 i = 0; !error && i < group->cItems; ++i) {
        const auto& file = group->fgd[i];
        const u32 units = length(file.cFileName, MAX_PATH);
        const u64 size = (file.dwFlags & FD_FILESIZE) ? (static_cast<u64>(file.nFileSizeHigh) << 32) | file.nFileSizeLow : 0;
        if (!units || units == MAX_PATH || ((file.dwFlags & FD_ATTRIBUTES) && (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))) { error = ERROR_INVALID_DATA; break; }
        if (size > limit) { error = ERROR_FILE_TOO_LARGE; break; }
        STGMEDIUM medium{};
        if (!get(object, contents, static_cast<LONG>(i), TYMED_HGLOBAL | TYMED_ISTREAM, &medium)) { error = ERROR_INVALID_DATA; break; }
        DWORD source_error = 0;
        auto source = adopt(&medium, size, limit, &source_error);
        if (medium.tymed) ReleaseStgMedium(&medium);
        if (!source.context) { error = source_error; break; }
        if (!visitor(context, {}, {file.cFileName, units}, source)) { upload_source_release(source); error = ERROR_INVALID_DATA; }
        else ++*count;
    }
    if (group) GlobalUnlock(metadata.hGlobal);
    ReleaseStgMedium(&metadata); return error;
}
// Largest payload read from an image stream: a maximal DIB, or a PNG one byte
// past the allowance (so oversize reports as too large, not truncated).
u64 image_read_limit(ClipboardPasteKind kind, u64 limit) noexcept {
    return kind == ClipboardPasteKind::png ? minimum<u64>(limit + 1, 0xfffff000u)
                                           : 256 + 1024 + kClipboardPasteMaximumPixels * 4;
}
DWORD convert_global(HGLOBAL block, ClipboardPasteKind kind, u64 limit, ClipboardPng* png) noexcept {
    const SIZE_T bytes = GlobalSize(block);
    const auto* data = static_cast<const u8*>(GlobalLock(block));
    if (!data) return ERROR_INVALID_DATA;
    const DWORD error = bytes > 0xffffffffull ? ERROR_INVALID_DATA :
        clipboard_paste_png(kind, {data, static_cast<u32>(bytes)}, limit, png);
    GlobalUnlock(block);
    return error;
}
DWORD convert_stream(IStream* stream, ClipboardPasteKind kind, u64 limit, ClipboardPng* png) noexcept {
    const u64 cap = image_read_limit(kind, limit);
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    STATSTG stat{};
    u64 capacity = SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart ? stat.cbSize.QuadPart : 1u << 20;
    if (capacity > cap) capacity = cap;
    u8* buffer = static_cast<u8*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(capacity), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    u64 used = 0;
    DWORD error = buffer ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY;
    while (!error) {
        if (used == capacity) {
            if (capacity >= cap) break; // larger than any acceptable payload; convert what fits
            const u64 grown = minimum(cap, capacity * 2);
            auto* next = static_cast<u8*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(grown), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!next) { error = ERROR_NOT_ENOUGH_MEMORY; break; }
            copy_bytes(next, buffer, static_cast<u32>(used));
            VirtualFree(buffer, 0, MEM_RELEASE); buffer = next; capacity = grown;
        }
        ULONG read = 0;
        const HRESULT result = stream->Read(buffer + used, static_cast<ULONG>(minimum<u64>(capacity - used, 1u << 24)), &read);
        if (FAILED(result)) { error = ERROR_INVALID_DATA; break; }
        used += read;
        if (!read || result == S_FALSE) break;
    }
    if (!error) {
        if (!used || used > 0xffffffffull) error = ERROR_INVALID_DATA;
        else if (!png || !limit) error = ERROR_INVALID_PARAMETER;
        else if (kind != ClipboardPasteKind::png)
            error = clipboard_paste_png(kind, {buffer, static_cast<u32>(used)}, limit, png);
        else {
            *png = {};
            const u64 bytes = clipboard_image_png_length({buffer, static_cast<u32>(used)});
            if (!bytes) error = ERROR_INVALID_DATA;
            else if (bytes > limit) error = ERROR_FILE_TOO_LARGE;
            else {
                SYSTEM_INFO system{}; GetSystemInfo(&system);
                const u64 page = system.dwPageSize;
                const u64 keep = (bytes + page - 1) / page * page;
                const u64 committed = (capacity + page - 1) / page * page;
                // The stream buffer already owns the PNG. Drop unused tail
                // pages before transfer so retained commitment matches a copy.
                if (keep < committed && !VirtualFree(buffer + keep, committed - keep, MEM_DECOMMIT)) {
                    // A failed trim uses the original copy and cleanup path.
                    error = clipboard_paste_png(kind, {buffer, static_cast<u32>(used)}, limit, png);
                } else {
                    *png = {buffer, bytes}; buffer = nullptr;
                }
            }
        }
    }
    if (buffer) VirtualFree(buffer, 0, MEM_RELEASE);
    return error;
}
DWORD image_from_object(IDataObject* object, CLIPFORMAT format, ClipboardPasteKind kind, u64 limit, ClipboardPng* png) noexcept {
    STGMEDIUM medium{};
    if (!get(object, format, -1, TYMED_HGLOBAL | TYMED_ISTREAM, &medium) && !get(object, format, -1, TYMED_HGLOBAL, &medium) &&
        !get(object, format, -1, TYMED_ISTREAM, &medium))
        return ERROR_INVALID_DATA;
    const DWORD error = medium.tymed == TYMED_HGLOBAL ? convert_global(medium.hGlobal, kind, limit, png)
                                                      : convert_stream(medium.pstm, kind, limit, png);
    ReleaseStgMedium(&medium);
    return error;
}
// The Win32 clipboard renders delayed OLE formats itself (WM_RENDERFORMAT) and
// synthesizes CF_DIBV5/CF_DIB; it is read only while unchanged since capture.
DWORD image_from_win32(CLIPFORMAT format, ClipboardPasteKind kind, u64 limit, DWORD sequence, ClipboardPng* png) noexcept {
    if (!sequence || GetClipboardSequenceNumber() != sequence || !IsClipboardFormatAvailable(format)) return ERROR_INVALID_DATA;
    bool opened = false;
    for (u32 attempt = 0; attempt < 20 && !(opened = OpenClipboard(nullptr) != FALSE); ++attempt) Sleep(5);
    if (!opened) return ERROR_INVALID_DATA;
    DWORD error = ERROR_INVALID_DATA;
    if (GetClipboardSequenceNumber() == sequence)
        if (HANDLE data = GetClipboardData(format)) error = convert_global(static_cast<HGLOBAL>(data), kind, limit, png);
    CloseClipboard();
    return error;
}
DWORD visit_object(IDataObject* object, DWORD sequence, u32 maximum, u64 limit, ClipboardAttachmentVisitor visitor,
    void* context, u32* count) noexcept {
    const auto png_format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"PNG"));
    const auto descriptors = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
    const bool live = sequence && GetClipboardSequenceNumber() == sequence;
    const auto image = [&](CLIPFORMAT format) noexcept {
        return format && (available(object, format, TYMED_HGLOBAL | TYMED_ISTREAM) || enumerated(object, format) ||
                          (live && IsClipboardFormatAvailable(format)));
    };
    ClipboardPasteFormats formats{};
    formats.png = image(png_format);
    formats.dibv5 = image(CF_DIBV5);
    formats.dib = image(CF_DIB);
    formats.files = available(object, CF_HDROP);
    formats.virtual_files = descriptors && available(object, descriptors);
    ClipboardPasteKind plan[kClipboardPastePlanCapacity]{};
    const u32 steps = clipboard_paste_plan(formats, plan);
    DWORD failure = ERROR_NOT_SUPPORTED;
    bool image_too_large = false;
    for (u32 step = 0; step < steps; ++step) {
        const auto kind = plan[step];
        DWORD error = ERROR_SUCCESS;
        if (kind == ClipboardPasteKind::files) error = visit_files(object, maximum, visitor, context, count);
        else if (kind == ClipboardPasteKind::virtual_files)
            error = visit_virtual_files(object, descriptors, maximum, limit, visitor, context, count);
        else {
            // A PNG already over the allowance will not shrink as a re-encoded DIB.
            if (image_too_large) continue;
            const CLIPFORMAT format = kind == ClipboardPasteKind::png ? png_format : kind == ClipboardPasteKind::dibv5 ? CLIPFORMAT{CF_DIBV5} : CLIPFORMAT{CF_DIB};
            ClipboardPng png{};
            error = image_from_object(object, format, kind, limit, &png);
            if (error == ERROR_INVALID_DATA) error = image_from_win32(format, kind, limit, sequence, &png);
            if (!error) {
                auto source = adopt_png(&png, &error);
                if (source.context) {
                    constexpr wchar_t name[] = L"image.png";
                    if (!visitor(context, {}, {name, 9}, source)) { upload_source_release(source); return ERROR_INVALID_DATA; }
                    *count = 1; return ERROR_SUCCESS;
                }
            }
            image_too_large = error == ERROR_FILE_TOO_LARGE;
        }
        if (!error) return ERROR_SUCCESS;
        // Visited entries belong to the caller now; only an untouched attempt falls back.
        if (*count) return error;
        if (failure == ERROR_NOT_SUPPORTED || error == ERROR_FILE_TOO_LARGE || error == ERROR_TOO_MANY_NAMES) failure = error;
    }
    return failure;
}
} // namespace
bool clipboard_attachment_available() noexcept {
#if defined(MAICORD_CLIPBOARD_UI_FIXTURE)
    if (clipboard_attachment_ui_fixture_provider())
        return true;
#endif
    const UINT descriptors = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW),
               png = RegisterClipboardFormatW(L"PNG");
    return IsClipboardFormatAvailable(CF_HDROP) ||
           (descriptors && IsClipboardFormatAvailable(descriptors)) ||
           (png && IsClipboardFormatAvailable(png)) || IsClipboardFormatAvailable(CF_DIBV5) ||
           IsClipboardFormatAvailable(CF_DIB);
}
bool clipboard_attachment_capture(HWND, ClipboardAttachmentData* output) noexcept {
    if (!output || output->object)
        return false;
    if (!ole_initialized) {
        if (FAILED(OleInitialize(nullptr)))
            return false;
        ole_initialized = true;
    }
    IDataObject* object = nullptr;
#if defined(MAICORD_CLIPBOARD_UI_FIXTURE)
    object = clipboard_attachment_ui_fixture_provider();
    if (object)
        object->AddRef();
#endif
    if (!object) {
        if (FAILED(OleGetClipboard(&object)))
            return false;
        output->sequence = GetClipboardSequenceNumber();
    }
    const HRESULT result =
        CoMarshalInterThreadInterfaceInStream(__uuidof(IDataObject), object, &output->object);
    object->Release();
    return SUCCEEDED(result);
}
void clipboard_attachment_release(ClipboardAttachmentData* input) noexcept {
    if (input && input->object) {
        LARGE_INTEGER zero{};
        input->object->Seek(zero, STREAM_SEEK_SET, nullptr);
        CoReleaseMarshalData(input->object); input->object->Release(); *input = {};
    }
}
void clipboard_attachment_shutdown() noexcept { if (ole_initialized) { OleUninitialize(); ole_initialized = false; } }
DWORD clipboard_attachment_visit(ClipboardAttachmentData* input, u32 maximum, u64 limit,
    ClipboardAttachmentVisitor visitor, void* context, u32* count) noexcept {
    if (!input || !input->object || !maximum || !limit || !visitor || !count) return ERROR_INVALID_PARAMETER;
    IDataObject* object = nullptr; *count = 0;
    const DWORD sequence = input->sequence;
    const HRESULT result = CoGetInterfaceAndReleaseStream(input->object, __uuidof(IDataObject), reinterpret_cast<void**>(&object));
    *input = {};
    if (FAILED(result)) return ERROR_INVALID_DATA;
    const DWORD error = visit_object(object, sequence, maximum, limit, visitor, context, count);
    object->Release(); return error;
}
}
