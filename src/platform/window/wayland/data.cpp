// The clipboard, the primary selection and file drops: wl_data_device and zwp_primary_selection on the seat.
//
// Reading is a pipe: the offer is asked to write one mime type into it (receive) and the window reads it, bounded by a
// time limit, waiting on the pipe as a system waitable. A selection the window itself owns is read from memory (reading
// its own source through the compositor would wait for an answer only this thread can give). Writing offers the text
// formats (and PNG and an HTML <img> for images) from a Payload that lives until the source is cancelled or replaced;
// `send` events write it into the pipe the receiver passes. Dropped file lists become DroppedFiles.
#include "maicord/core/heap.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/media/image/png_encode.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/window/clipboard.hpp"
#include "maicord/platform/window/dropped_files.hpp"
#include "maicord/platform/window/wayland.hpp"
#include "maicord/platform/window/wayland/uri.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "backend.hpp"

namespace maicord::wayland {
namespace {

constexpr long kSysRtSigaction = 13;
constexpr long kSysPipe2 = 293;
constexpr int kSigPipe = 13;
constexpr i16 kPollOut = 0x4;
constexpr u32 kTextReadMs = 300;
constexpr u32 kImageReadMs = 2000;
constexpr u32 kDropReadMs = 1000;
constexpr u32 kTextLimit = 16u << 20;
constexpr u32 kImageLimit = 64u << 20;
constexpr u32 kListLimit = 1u << 20;
constexpr u32 kWriteMs = 3000;
constexpr u32 kDropActionCopy = 1;

struct MimeName {
    const char* name;
    u32 bit;
};

constexpr MimeName kMimes[] = {
    {"text/plain;charset=utf-8", mime_text_utf8}, {"UTF8_STRING", mime_utf8_string},
    {"text/plain", mime_text_plain},              {"image/png", mime_png},
    {"text/html", mime_html},                     {"text/uri-list", mime_uri_list},
    {"x-special/gnome-copied-files", mime_gnome_files},
};

bool same(const char* left, const char* right) noexcept {
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

u32 mime_bit(const char* mime) noexcept {
    for (const MimeName& entry : kMimes)
        if (same(mime, entry.name))
            return entry.bit;
    return 0;
}

const char* mime_name(u32 bit) noexcept {
    for (const MimeName& entry : kMimes)
        if (entry.bit == bit)
            return entry.name;
    return nullptr;
}

// The text format to ask for among an offer's, best first; 0 when it has none.
u32 text_bit_of(u32 mimes) noexcept {
    const u32 preferred[] = {mime_text_utf8, mime_utf8_string, mime_text_plain};
    for (const u32 bit : preferred)
        if (mimes & bit)
            return bit;
    return 0;
}

// An unreaped SIGPIPE would end the process when a receiver closes its pipe early.
void ignore_sigpipe() noexcept {
    struct KernelSigaction {
        void* handler;
        u64 flags;
        void* restorer;
        u64 mask;
    };
    const KernelSigaction action{reinterpret_cast<void*>(1), 0, nullptr, 0};  // SIG_IGN
    syscall4(kSysRtSigaction, kSigPipe, reinterpret_cast<long>(&action), 0, 8);
}

// Pipes ---------------------------------------------------------------------------------------------------------------

bool make_pipe(int fds[2]) noexcept {
    return !syscall_failed(syscall2(kSysPipe2, reinterpret_cast<long>(fds), kOpenCloseOnExec));
}

// Everything the other end writes until it closes, within `timeout_ms`, into a heap block the caller frees.
bool read_pipe(int fd, u32 timeout_ms, u32 limit, u8** out, u32* bytes) noexcept {
    u32 capacity = 4096;
    u8* data = static_cast<u8*>(heap_allocate(capacity));
    if (!data)
        return false;
    u32 used = 0;
    const u64 deadline = clock_monotonic_milliseconds() + timeout_ms;
    const Waitable waitable = waitable_borrow_fd(fd);
    bool done = false;
    while (!done) {
        const u64 now = clock_monotonic_milliseconds();
        if (now >= deadline)
            break;
        const WaitManyResult waited = wait_many(&waitable, 1, static_cast<u32>(deadline - now), 0);
        if (waited.failed || waited.timed_out)
            break;
        if (used == capacity) {
            if (capacity >= limit)
                break;
            u8* grown = static_cast<u8*>(heap_allocate(capacity * 2));
            if (!grown)
                break;
            copy_bytes(grown, data, used);
            heap_free(data);
            data = grown;
            capacity *= 2;
        }
        const long count = sys_read(fd, data + used, capacity - used);
        if (count == 0)
            done = true;
        else if (count > 0)
            used += static_cast<u32>(count);
        else if (count != -kErrorInterrupted && count != -kErrorAgain)
            break;
    }
    waitable_close(waitable);
    if (!done) {
        heap_free(data);
        return false;
    }
    *out = data;
    *bytes = used;
    return true;
}

// Writes the bytes into the receiver's pipe, a chunk when the pipe has room, within the time limit.
void write_pipe(int fd, const u8* data, u32 bytes) noexcept {
    const u64 deadline = clock_monotonic_milliseconds() + kWriteMs;
    u32 written = 0;
    while (written < bytes) {
        const u64 now = clock_monotonic_milliseconds();
        if (now >= deadline)
            return;
        PollFd poll{fd, kPollOut, 0};
        const u64 left = deadline - now;
        const TimeSpec timeout{static_cast<i64>(left / 1000), static_cast<i64>(left % 1000) * 1'000'000};
        if (sys_ppoll(&poll, 1, &timeout) <= 0 || (poll.revents & (kPollError | kPollHangUp | kPollInvalid)))
            return;
        const long count = sys_write(fd, data + written, minimum<u32>(bytes - written, 4096));
        if (count < 0 && count != -kErrorInterrupted && count != -kErrorAgain)
            return;
        if (count > 0)
            written += static_cast<u32>(count);
    }
}

// Offers -------------------------------------------------------------------------------------------------------------

OfferSlot* slot_of(void* offer) noexcept {
    if (!offer)
        return nullptr;
    for (OfferSlot& slot : g.data.offers)
        if (slot.offer == offer)
            return &slot;
    return nullptr;
}

bool slot_in_use(const OfferSlot* slot) noexcept {
    return slot == g.data.selection || slot == g.data.primary_selection || slot == g.data.drag;
}

void destroy_slot(OfferSlot* slot) noexcept {
    if (!slot || !slot->offer)
        return;
    if (slot->primary)
        zwp_primary_selection_offer_v1_destroy(static_cast<zwp_primary_selection_offer_v1*>(slot->offer));
    else
        wl_data_offer_destroy(static_cast<wl_data_offer*>(slot->offer));
    slot->offer = nullptr;
    slot->mimes = 0;
}

// A slot for an offer the compositor has just announced: a free one, else the first not in use.
OfferSlot* new_slot(void* offer, bool primary) noexcept {
    OfferSlot* chosen = nullptr;
    for (OfferSlot& slot : g.data.offers) {
        if (!slot.offer) {
            chosen = &slot;
            break;
        }
        if (!chosen && !slot_in_use(&slot))
            chosen = &slot;
    }
    if (!chosen)
        return nullptr;
    destroy_slot(chosen);
    chosen->offer = offer;
    chosen->mimes = 0;
    chosen->primary = primary;
    return chosen;
}

void on_offer_mime(void* data, wl_data_offer*, const char* mime) noexcept {
    static_cast<OfferSlot*>(data)->mimes |= mime_bit(mime);
}
void on_offer_source_actions(void*, wl_data_offer*, u32) noexcept {}
void on_offer_action(void*, wl_data_offer*, u32) noexcept {}
const wl_data_offer_listener kOfferListener = {on_offer_mime, on_offer_source_actions, on_offer_action};

void on_primary_offer_mime(void* data, zwp_primary_selection_offer_v1*, const char* mime) noexcept {
    static_cast<OfferSlot*>(data)->mimes |= mime_bit(mime);
}
const zwp_primary_selection_offer_v1_listener kPrimaryOfferListener = {on_primary_offer_mime};

// The bytes of one mime type of an offer, in a heap block.
bool read_offer(const OfferSlot* slot, u32 bit, u32 timeout_ms, u32 limit, u8** out, u32* bytes) noexcept {
    const char* mime = mime_name(bit);
    int fds[2];
    if (!slot || !slot->offer || !mime || !make_pipe(fds))
        return false;
    if (slot->primary)
        zwp_primary_selection_offer_v1_receive(static_cast<zwp_primary_selection_offer_v1*>(slot->offer), mime, fds[1]);
    else
        wl_data_offer_receive(static_cast<wl_data_offer*>(slot->offer), mime, fds[1]);
    sys_close(fds[1]);  // libwayland sent its own copy
    g_client.display_flush(g.display);
    const bool ok = read_pipe(fds[0], timeout_ms, limit, out, bytes);
    sys_close(fds[0]);
    return ok;
}

void on_data_offer(void*, wl_data_device*, wl_data_offer* offer) noexcept {
    if (OfferSlot* slot = new_slot(offer, false))
        wl_data_offer_add_listener(offer, &kOfferListener, slot);
    else
        wl_data_offer_destroy(offer);
}

void on_selection(void*, wl_data_device*, wl_data_offer* offer) noexcept {
    DataState& data = g.data;
    OfferSlot* slot = slot_of(offer);
    if (data.selection && data.selection != slot)
        destroy_slot(data.selection);
    data.selection = slot;
    ring_push(make_event(EventType::clipboard_changed, now_ms32()));
}

void on_drag_enter(void*, wl_data_device*, u32 serial, wl_surface*, i32, i32, wl_data_offer* offer) noexcept {
    DataState& data = g.data;
    if (data.drag)
        destroy_slot(data.drag);
    data.drag = slot_of(offer);
    data.drag_serial = serial;
    data.drag_accepts = data.drag && data.drop_target && (data.drag->mimes & mime_uri_list);
    if (!data.drag)
        return;
    wl_data_offer* wl_offer = static_cast<wl_data_offer*>(data.drag->offer);
    wl_data_offer_accept(wl_offer, serial, data.drag_accepts ? "text/uri-list" : nullptr);
    if (data.drag_accepts && g_client.proxy_get_version(reinterpret_cast<Proxy*>(wl_offer)) >= wl_data_offer_set_actions_since)
        wl_data_offer_set_actions(wl_offer, kDropActionCopy, kDropActionCopy);
}

void on_drag_leave(void*, wl_data_device*) noexcept {
    DataState& data = g.data;
    destroy_slot(data.drag);
    data.drag = nullptr;
    data.drag_accepts = false;
}

void on_drag_motion(void*, wl_data_device*, u32, i32, i32) noexcept {}

void on_drop(void*, wl_data_device*) noexcept {
    DataState& data = g.data;
    OfferSlot* slot = data.drag;
    if (!slot || !data.drag_accepts)
        return;
    u8* list = nullptr;
    u32 bytes = 0;
    DroppedFiles* files = nullptr;
    if (read_offer(slot, mime_uri_list, kDropReadMs, kListLimit, &list, &bytes)) {
        files = uri_list_files(list, bytes);
        heap_free(list);
    }
    wl_data_offer* wl_offer = static_cast<wl_data_offer*>(slot->offer);
    if (g_client.proxy_get_version(reinterpret_cast<Proxy*>(wl_offer)) >= wl_data_offer_finish_since)
        wl_data_offer_finish(wl_offer);
    destroy_slot(slot);
    data.drag = nullptr;
    data.drag_accepts = false;
    if (!files)
        return;
    WindowEvent event = make_event(EventType::drop_files, now_ms32());
    event.drop.files = files;
    if (!ring_push(event))
        dropped_files_release(files);
}

const wl_data_device_listener kDeviceListener = {on_data_offer, on_drag_enter, on_drag_leave,
                                                 on_drag_motion, on_drop,       on_selection};

void on_primary_data_offer(void*, zwp_primary_selection_device_v1*, zwp_primary_selection_offer_v1* offer) noexcept {
    if (OfferSlot* slot = new_slot(offer, true))
        zwp_primary_selection_offer_v1_add_listener(offer, &kPrimaryOfferListener, slot);
    else
        zwp_primary_selection_offer_v1_destroy(offer);
}

void on_primary_selection(void*, zwp_primary_selection_device_v1*, zwp_primary_selection_offer_v1* offer) noexcept {
    DataState& data = g.data;
    OfferSlot* slot = slot_of(offer);
    if (data.primary_selection && data.primary_selection != slot)
        destroy_slot(data.primary_selection);
    data.primary_selection = slot;
}

const zwp_primary_selection_device_v1_listener kPrimaryDeviceListener = {on_primary_data_offer, on_primary_selection};

// Selections we own ----------------------------------------------------------------------------------------------------

void free_payload(Payload* payload) noexcept {
    if (!payload)
        return;
    heap_free(payload->text);
    heap_free(payload->png);
    heap_free(payload->html);
    heap_free(payload);
}

u8* copy_block(const u8* data, u32 bytes) noexcept {
    u8* block = static_cast<u8*>(heap_allocate(bytes ? bytes : 1));
    if (block && bytes)
        copy_bytes(block, data, bytes);
    return block;
}

void send_payload(const Payload* payload, const char* mime, int fd) noexcept {
    const u32 bit = mime_bit(mime);
    if (payload && (bit & mime_any_text) && payload->text)
        write_pipe(fd, payload->text, payload->text_bytes);
    else if (payload && bit == mime_png && payload->png)
        write_pipe(fd, payload->png, payload->png_bytes);
    else if (payload && bit == mime_html && payload->html)
        write_pipe(fd, payload->html, payload->html_bytes);
    sys_close(fd);
}

void on_source_target(void*, wl_data_source*, const char*) noexcept {}
void on_source_send(void* data, wl_data_source*, const char* mime, i32 fd) noexcept {
    send_payload(static_cast<const Payload*>(data), mime, fd);
}
void on_source_cancelled(void* data, wl_data_source* source) noexcept {
    DataState& state = g.data;
    if (data != state.payload)
        return;
    wl_data_source_destroy(source);
    state.source = nullptr;
    free_payload(state.payload);
    state.payload = nullptr;
}
void on_source_drop_performed(void*, wl_data_source*) noexcept {}
void on_source_finished(void*, wl_data_source*) noexcept {}
void on_source_action(void*, wl_data_source*, u32) noexcept {}
const wl_data_source_listener kSourceListener = {on_source_target,         on_source_send,     on_source_cancelled,
                                                 on_source_drop_performed, on_source_finished, on_source_action};

void on_primary_source_send(void* data, zwp_primary_selection_source_v1*, const char* mime, i32 fd) noexcept {
    send_payload(static_cast<const Payload*>(data), mime, fd);
}
void on_primary_source_cancelled(void* data, zwp_primary_selection_source_v1* source) noexcept {
    DataState& state = g.data;
    if (data != state.primary_payload)
        return;
    zwp_primary_selection_source_v1_destroy(source);
    state.primary_source = nullptr;
    free_payload(state.primary_payload);
    state.primary_payload = nullptr;
}
const zwp_primary_selection_source_v1_listener kPrimarySourceListener = {on_primary_source_send,
                                                                         on_primary_source_cancelled};

// Takes ownership of `payload` and offers `mimes` as the clipboard.
bool own_clipboard(Payload* payload, const u32* mimes, u32 count) noexcept {
    DataState& data = g.data;
    if (!data.device || !g.data_manager || g.input_serial == 0) {
        free_payload(payload);
        return false;
    }
    if (data.source) {
        wl_data_source_destroy(data.source);
        free_payload(data.payload);
    }
    data.source = wl_data_device_manager_create_data_source(g.data_manager);
    data.payload = payload;
    wl_data_source_add_listener(data.source, &kSourceListener, payload);
    for (u32 i = 0; i < count; ++i)
        wl_data_source_offer(data.source, mime_name(mimes[i]));
    wl_data_device_set_selection(data.device, data.source, g.input_serial);
    g_client.display_flush(g.display);
    return true;
}

Payload* new_payload() noexcept {
    return static_cast<Payload*>(heap_allocate(sizeof(Payload), true));
}

// Reads the clipboard or primary text into `output`.
ClipboardTextResult read_text(const OfferSlot* slot, const Payload* own, const Slice<char>& output) noexcept {
    ClipboardTextResult result{};
    const u8* text = nullptr;
    u32 bytes = 0;
    u8* block = nullptr;
    if (own && own->text) {
        text = own->text;
        bytes = own->text_bytes;
    } else if (slot && text_bit_of(slot->mimes)) {
        if (!read_offer(slot, text_bit_of(slot->mimes), kTextReadMs, kTextLimit, &block, &bytes)) {
            result.error = ClipboardTextError::data_failed;
            return result;
        }
        text = block;
    } else {
        result.error = ClipboardTextError::unavailable;
        return result;
    }
    if (!utf8_validate(Utf8View{reinterpret_cast<const char*>(text), bytes}).valid)
        result.error = ClipboardTextError::invalid_text;
    else if (bytes > output.count)
        result.error = ClipboardTextError::capacity;
    else {
        copy_bytes(output.data, text, bytes);
        result.bytes = bytes;
    }
    heap_free(block);
    return result;
}

} // namespace

void data_attach() noexcept {
    DataState& data = g.data;
    if (!g.seat)
        return;
    ignore_sigpipe();
    if (g.data_manager) {
        data.device = wl_data_device_manager_get_data_device(g.data_manager, g.seat);
        wl_data_device_add_listener(data.device, &kDeviceListener, nullptr);
    }
    if (g.primary_manager) {
        data.primary_device = zwp_primary_selection_device_manager_v1_get_device(g.primary_manager, g.seat);
        zwp_primary_selection_device_v1_add_listener(data.primary_device, &kPrimaryDeviceListener, nullptr);
    }
}

void data_shutdown() noexcept {
    DataState& data = g.data;
    for (OfferSlot& slot : data.offers)
        destroy_slot(&slot);
    if (data.source)
        wl_data_source_destroy(data.source);
    free_payload(data.payload);
    if (data.primary_source)
        zwp_primary_selection_source_v1_destroy(data.primary_source);
    free_payload(data.primary_payload);
    if (data.device) {
        if (g.data_manager && g_client.proxy_get_version(reinterpret_cast<Proxy*>(data.device)) >= wl_data_device_release_since)
            wl_data_device_release(data.device);
        else
            g_client.proxy_destroy(reinterpret_cast<Proxy*>(data.device));
    }
    if (data.primary_device)
        zwp_primary_selection_device_v1_destroy(data.primary_device);
    clear_bytes(&data, static_cast<u32>(sizeof(data)));
}

} // namespace maicord::wayland

namespace maicord {

using namespace wayland;

struct ClipboardCapture {
    ClipboardCaptureKind kind;
    DroppedFiles* files;
    u8* png;
    u32 png_bytes;
};

u32 dropped_files_count(const DroppedFiles* files) noexcept {
    return files ? files->count : 0;
}

u32 dropped_files_path_length(const DroppedFiles* files, u32 index) noexcept {
    return files && index < files->count ? files->length[index] : 0;
}

u32 dropped_files_path(const DroppedFiles* files, u32 index, const Slice<wchar_t>& output) noexcept {
    if (!files || index >= files->count || files->length[index] > output.count)
        return 0;
    copy_bytes(output.data, files->characters + files->start[index], files->length[index] * sizeof(wchar_t));
    return files->length[index];
}

void dropped_files_release(DroppedFiles* files) noexcept {
    if (!files)
        return;
    heap_free(files->characters);
    heap_free(files);
}

void window_set_drop_target(Window window, bool accept_files) noexcept {
    if (window == reinterpret_cast<Window>(&g.window) && g.window_exists)
        g.data.drop_target = accept_files;
}

ClipboardTextResult clipboard_read_utf8(const Slice<char>& output) noexcept {
    return read_text(g.data.selection, g.data.payload, output);
}

bool clipboard_has_text() noexcept {
    const Payload* own = g.data.payload;
    return (own && own->text) || (g.data.selection && text_bit_of(g.data.selection->mimes));
}

bool clipboard_write_utf8(const Utf8View& text) noexcept {
    if (!utf8_validate(text).valid)
        return false;
    Payload* payload = new_payload();
    if (!payload)
        return false;
    payload->text = copy_block(reinterpret_cast<const u8*>(text.data), text.bytes);
    payload->text_bytes = text.bytes;
    if (!payload->text) {
        free_payload(payload);
        return false;
    }
    const u32 mimes[] = {mime_text_utf8, mime_text_plain, mime_utf8_string};
    return own_clipboard(payload, mimes, 3);
}

bool clipboard_write_image(const u8* rgba, u32 width, u32 height, u32 stride, const Utf8View& source_url) noexcept {
    const u64 bound = png_encode_bound(width, height);
    const u64 workspace_bytes = png_encode_workspace_bytes(width, height);
    if (!rgba || !bound || stride < width * 4)
        return false;
    // The encoder takes tightly packed rows.
    const u64 packed_bytes = static_cast<u64>(width) * height * 4;
    u8* packed = static_cast<u8*>(heap_allocate(packed_bytes));
    u8* workspace = static_cast<u8*>(heap_allocate(workspace_bytes + 4));
    u8* png = static_cast<u8*>(heap_allocate(bound));
    Payload* payload = new_payload();
    bool ok = packed && workspace && png && payload;
    if (ok) {
        for (u32 row = 0; row < height; ++row)
            copy_bytes(packed + static_cast<u64>(row) * width * 4, rgba + static_cast<u64>(row) * stride, width * 4);
        u8* aligned = reinterpret_cast<u8*>((reinterpret_cast<usize>(workspace) + 3) & ~usize{3});
        const u32 size = png_encode_rgba8(packed, width, height, Slice<u8>{aligned, static_cast<u32>(workspace_bytes)},
                                          Slice<u8>{png, static_cast<u32>(bound)});
        ok = size != 0;
        if (ok) {
            payload->png = copy_block(png, size);
            payload->png_bytes = size;
            ok = payload->png != nullptr;
        }
    }
    heap_free(packed);
    heap_free(workspace);
    heap_free(png);
    if (!ok) {
        free_payload(payload);
        return false;
    }
    u32 mimes[2] = {mime_png, 0};
    u32 count = 1;
    if (source_url.bytes != 0) {
        // The linked-image HTML other applications paste: <img src="...">, with the quote and ampersand escaped.
        const char head[] = "<img src=\"";
        const char tail[] = "\">";
        u32 capacity = static_cast<u32>(sizeof(head) - 1 + sizeof(tail) - 1) + source_url.bytes * 6;
        u8* html = static_cast<u8*>(heap_allocate(capacity));
        if (html) {
            u32 used = 0;
            for (u32 i = 0; i < sizeof(head) - 1; ++i)
                html[used++] = static_cast<u8>(head[i]);
            for (u32 i = 0; i < source_url.bytes; ++i) {
                const char c = source_url.data[i];
                const char* replacement = c == '"' ? "&quot;" : c == '&' ? "&amp;" : c == '<' ? "&lt;" : nullptr;
                if (replacement) {
                    for (; *replacement; ++replacement)
                        html[used++] = static_cast<u8>(*replacement);
                } else {
                    html[used++] = static_cast<u8>(c);
                }
            }
            for (u32 i = 0; i < sizeof(tail) - 1; ++i)
                html[used++] = static_cast<u8>(tail[i]);
            payload->html = html;
            payload->html_bytes = used;
            mimes[count++] = mime_html;
        }
    }
    return own_clipboard(payload, mimes, count);
}

bool clipboard_has_attachment() noexcept {
    const OfferSlot* slot = g.data.selection;
    return slot && !g.data.payload && (slot->mimes & (mime_uri_list | mime_gnome_files | mime_png)) != 0;
}

ClipboardCapture* clipboard_capture_attachment() noexcept {
    const OfferSlot* slot = g.data.selection;
    if (g.data.payload || !slot)
        return nullptr;  // what the window itself put there is not an attachment
    ClipboardCapture* capture = nullptr;
    u8* data = nullptr;
    u32 bytes = 0;
    const u32 list_bit = (slot->mimes & mime_uri_list) ? mime_uri_list : (slot->mimes & mime_gnome_files);
    if (list_bit && read_offer(slot, list_bit, kDropReadMs, kListLimit, &data, &bytes)) {
        DroppedFiles* files = uri_list_files(data, bytes);
        heap_free(data);
        if (files) {
            capture = static_cast<ClipboardCapture*>(heap_allocate(sizeof(ClipboardCapture), true));
            if (!capture) {
                dropped_files_release(files);
                return nullptr;
            }
            capture->kind = ClipboardCaptureKind::files;
            capture->files = files;
            return capture;
        }
    }
    if ((slot->mimes & mime_png) && read_offer(slot, mime_png, kImageReadMs, kImageLimit, &data, &bytes)) {
        capture = static_cast<ClipboardCapture*>(heap_allocate(sizeof(ClipboardCapture), true));
        if (!capture) {
            heap_free(data);
            return nullptr;
        }
        capture->kind = ClipboardCaptureKind::image_png;
        capture->png = data;
        capture->png_bytes = bytes;
    }
    return capture;
}

void clipboard_capture_release(ClipboardCapture* capture) noexcept {
    if (!capture)
        return;
    dropped_files_release(capture->files);
    heap_free(capture->png);
    heap_free(capture);
}

ClipboardCaptureKind clipboard_capture_kind(const ClipboardCapture* capture) noexcept {
    return capture->kind;
}

const DroppedFiles* clipboard_capture_files(const ClipboardCapture* capture) noexcept {
    return capture->files;
}

ConstSlice<u8> clipboard_capture_png(const ClipboardCapture* capture) noexcept {
    return ConstSlice<u8>{capture->png, capture->png_bytes};
}

ClipboardTextResult primary_selection_read_utf8(const Slice<char>& output) noexcept {
    return read_text(g.data.primary_selection, g.data.primary_payload, output);
}

bool primary_selection_write_utf8(const Utf8View& text) noexcept {
    DataState& data = g.data;
    if (!data.primary_device || !g.primary_manager || g.input_serial == 0 || !utf8_validate(text).valid)
        return false;
    Payload* payload = new_payload();
    if (!payload)
        return false;
    payload->text = copy_block(reinterpret_cast<const u8*>(text.data), text.bytes);
    payload->text_bytes = text.bytes;
    if (!payload->text) {
        free_payload(payload);
        return false;
    }
    if (data.primary_source) {
        zwp_primary_selection_source_v1_destroy(data.primary_source);
        free_payload(data.primary_payload);
    }
    data.primary_source = zwp_primary_selection_device_manager_v1_create_source(g.primary_manager);
    data.primary_payload = payload;
    zwp_primary_selection_source_v1_add_listener(data.primary_source, &kPrimarySourceListener, payload);
    const u32 text_bits[] = {mime_text_utf8, mime_text_plain, mime_utf8_string};
    for (const u32 bit : text_bits)
        zwp_primary_selection_source_v1_offer(data.primary_source, mime_name(bit));
    zwp_primary_selection_device_v1_set_selection(data.primary_device, data.primary_source, g.input_serial);
    g_client.display_flush(g.display);
    return true;
}

bool primary_selection_has_text() noexcept {
    const Payload* own = g.data.primary_payload;
    return (own && own->text) || (g.data.primary_selection && text_bit_of(g.data.primary_selection->mimes));
}

} // namespace maicord
