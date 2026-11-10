#pragma once

#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/platform/window/wayland/abi.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "wayland_protocols.hpp"

// A second client of the compositor that plays the other applications of a desktop (Linux, tests only): a clipboard
// manager (wlr-data-control: it sees the clipboard and sets it) and an input method
// (input-method-v2: it is told when text input is enabled, and sends preedit, commits and deletions). Its connection is
// served by a thread of its own, because the window under test answers it only when the test pumps the window, and
// the window waits for it while it reads a selection the helper owns. The thread only dispatches events; the test thread
// only sends requests. The fields the two share are volatile and read after a wait for them.

namespace maicord::inject {

using namespace wayland;

inline constexpr u32 kMaxOffered = 4;
inline constexpr u32 kMimeBits = 8;

struct Offered {
    const char* mime;
    const u8* data;
    u32 bytes;
};

struct HelperOffer {
    zwlr_data_control_offer_v1* offer;
    volatile u32 mimes;  // bit i: the offer lists kMimeNames[i]
};

inline constexpr const char* kMimeNames[kMimeBits] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING",
                                                      "image/png", "text/html", "text/uri-list",
                                                      "x-special/gnome-copied-files", "text/x-unused"};

struct Helper {
    wl_display* display;
    wl_registry* registry;
    wl_seat* seat;
    zwlr_data_control_manager_v1* data_manager;
    zwlr_data_control_device_v1* data_device;
    zwp_input_method_manager_v2* ime_manager;
    zwp_input_method_v2* ime;
    Thread* thread;
    volatile u32 stop;

    // Clipboard: the offers the compositor announced, the current selections and the sources the helper owns.
    HelperOffer offers[6];
    HelperOffer* selection;
    volatile u32 selection_events;  // data-control selection events seen
    zwlr_data_control_source_v1* source;
    Offered offered[kMaxOffered];
    u32 offered_count;

    // Input method.
    volatile u32 ime_active;
    volatile u32 ime_done_count;
    volatile u32 ime_deactivations;
    char ime_surrounding[4100];
    volatile u32 ime_surrounding_bytes;
    volatile u32 ime_cursor, ime_anchor;
    volatile u32 ime_hint, ime_purpose;
};

namespace helper_detail {

inline Helper* g_target;

inline bool same(const char* left, const char* right) noexcept {
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

inline void on_global(void*, wl_registry* registry, u32 name, const char* interface, u32 version) noexcept {
    Helper& helper = *g_target;
    if (same(interface, "wl_seat") && !helper.seat)
        helper.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    else if (same(interface, "zwlr_data_control_manager_v1") && !helper.data_manager)
        helper.data_manager = static_cast<zwlr_data_control_manager_v1*>(wl_registry_bind(
            registry, name, &zwlr_data_control_manager_v1_interface, version < 2 ? version : 2));
    else if (same(interface, "zwp_input_method_manager_v2") && !helper.ime_manager)
        helper.ime_manager = static_cast<zwp_input_method_manager_v2*>(
            wl_registry_bind(registry, name, &zwp_input_method_manager_v2_interface, 1));
}
inline void on_global_remove(void*, wl_registry*, u32) noexcept {}
inline const wl_registry_listener kRegistryListener = {on_global, on_global_remove};

inline void on_offer_mime(void* data, zwlr_data_control_offer_v1*, const char* mime) noexcept {
    HelperOffer* offer = static_cast<HelperOffer*>(data);
    for (u32 i = 0; i < kMimeBits; ++i)
        if (same(mime, kMimeNames[i]))
            offer->mimes = offer->mimes | (1u << i);
}
inline const zwlr_data_control_offer_v1_listener kOfferListener = {on_offer_mime};

inline void on_data_offer(void*, zwlr_data_control_device_v1*, zwlr_data_control_offer_v1* offer) noexcept {
    Helper& helper = *g_target;
    for (HelperOffer& slot : helper.offers) {
        if (!slot.offer) {
            slot.offer = offer;
            slot.mimes = 0;
            zwlr_data_control_offer_v1_add_listener(offer, &kOfferListener, &slot);
            return;
        }
    }
    zwlr_data_control_offer_v1_destroy(offer);
}

inline HelperOffer* offer_slot(Helper& helper, zwlr_data_control_offer_v1* offer) noexcept {
    for (HelperOffer& slot : helper.offers)
        if (offer && slot.offer == offer)
            return &slot;
    return nullptr;
}

inline void release_unused(Helper& helper) noexcept {
    for (HelperOffer& slot : helper.offers) {
        if (slot.offer && &slot != helper.selection) {
            zwlr_data_control_offer_v1_destroy(slot.offer);
            slot.offer = nullptr;
            slot.mimes = 0;
        }
    }
}

inline void on_selection(void*, zwlr_data_control_device_v1*, zwlr_data_control_offer_v1* offer) noexcept {
    Helper& helper = *g_target;
    helper.selection = offer_slot(helper, offer);
    helper.selection_events = helper.selection_events + 1;
    release_unused(helper);
}

// The primary selection is not used: its offers go.
inline void on_primary_selection(void*, zwlr_data_control_device_v1*, zwlr_data_control_offer_v1*) noexcept {
    release_unused(*g_target);
}

inline void on_finished(void*, zwlr_data_control_device_v1*) noexcept {}

inline const zwlr_data_control_device_v1_listener kDeviceListener = {on_data_offer, on_selection, on_finished,
                                                                     on_primary_selection};

// Writes a source's bytes into the pipe a reader passed.
inline void write_all(int fd, const u8* data, u32 bytes) noexcept {
    u32 written = 0;
    const u64 deadline = clock_monotonic_milliseconds() + 3000;
    while (written < bytes && clock_monotonic_milliseconds() < deadline) {
        PollFd poll{fd, 0x4, 0};
        const TimeSpec timeout{0, 50'000'000};
        if (sys_ppoll(&poll, 1, &timeout) <= 0)
            continue;
        const long count = sys_write(fd, data + written, bytes - written < 4096 ? bytes - written : 4096);
        if (count > 0)
            written += static_cast<u32>(count);
        else if (count != -kErrorAgain && count != -kErrorInterrupted)
            break;
    }
}

inline void on_send(void*, zwlr_data_control_source_v1*, const char* mime, i32 fd) noexcept {
    const Helper& helper = *g_target;
    for (u32 i = 0; i < helper.offered_count; ++i)
        if (same(helper.offered[i].mime, mime))
            write_all(fd, helper.offered[i].data, helper.offered[i].bytes);
    sys_close(fd);
}
inline void on_cancelled(void*, zwlr_data_control_source_v1*) noexcept {}
inline const zwlr_data_control_source_v1_listener kSourceListener = {on_send, on_cancelled};

inline void on_ime_activate(void*, zwp_input_method_v2*) noexcept {
    g_target->ime_active = 1;
}
inline void on_ime_deactivate(void*, zwp_input_method_v2*) noexcept {
    g_target->ime_active = 0;
    g_target->ime_deactivations = g_target->ime_deactivations + 1;
}
inline void on_ime_surrounding(void*, zwp_input_method_v2*, const char* text, u32 cursor, u32 anchor) noexcept {
    Helper& helper = *g_target;
    u32 length = 0;
    while (text[length] && length < sizeof(helper.ime_surrounding) - 1) {
        helper.ime_surrounding[length] = text[length];
        ++length;
    }
    helper.ime_surrounding[length] = 0;
    helper.ime_surrounding_bytes = length;
    helper.ime_cursor = cursor;
    helper.ime_anchor = anchor;
}
inline void on_ime_cause(void*, zwp_input_method_v2*, u32) noexcept {}
inline void on_ime_content_type(void*, zwp_input_method_v2*, u32 hint, u32 purpose) noexcept {
    g_target->ime_hint = hint;
    g_target->ime_purpose = purpose;
}
inline void on_ime_done(void*, zwp_input_method_v2*) noexcept {
    g_target->ime_done_count = g_target->ime_done_count + 1;
}
inline void on_ime_unavailable(void*, zwp_input_method_v2*) noexcept {}
inline const zwp_input_method_v2_listener kImeListener = {on_ime_activate, on_ime_deactivate, on_ime_surrounding,
                                                          on_ime_cause,    on_ime_content_type, on_ime_done,
                                                          on_ime_unavailable};

// The service thread: dispatches the helper's events until stopped.
inline u32 serve(void* argument) noexcept {
    Helper& helper = *static_cast<Helper*>(argument);
    while (!helper.stop) {
        while (g_client.display_prepare_read(helper.display) != 0)
            g_client.display_dispatch_pending(helper.display);
        g_client.display_flush(helper.display);
        PollFd poll{g_client.display_get_fd(helper.display), kPollIn, 0};
        const TimeSpec timeout{0, 20'000'000};
        if (sys_ppoll(&poll, 1, &timeout) > 0 && (poll.revents & kPollIn))
            g_client.display_read_events(helper.display);
        else
            g_client.display_cancel_read(helper.display);
        g_client.display_dispatch_pending(helper.display);
    }
    return 0;
}

} // namespace helper_detail

// Connects and starts serving. What a compositor lacks is simply absent: check data_manager and ime_manager.
inline bool helper_open(Helper* helper) noexcept {
    clear_bytes(helper, static_cast<u32>(sizeof(*helper)));
    if (!client_load())
        return false;
    helper->display = g_client.display_connect(nullptr);
    if (!helper->display)
        return false;
    helper_detail::g_target = helper;
    helper->registry = wl_display_get_registry(helper->display);
    wl_registry_add_listener(helper->registry, &helper_detail::kRegistryListener, nullptr);
    g_client.display_roundtrip(helper->display);
    if (!helper->seat)
        return false;
    if (helper->data_manager) {
        helper->data_device = zwlr_data_control_manager_v1_get_data_device(helper->data_manager, helper->seat);
        zwlr_data_control_device_v1_add_listener(helper->data_device, &helper_detail::kDeviceListener, nullptr);
    }
    if (helper->ime_manager) {
        helper->ime = zwp_input_method_manager_v2_get_input_method(helper->ime_manager, helper->seat);
        zwp_input_method_v2_add_listener(helper->ime, &helper_detail::kImeListener, nullptr);
    }
    g_client.display_roundtrip(helper->display);  // the current selection, before the thread takes the queue
    helper->thread = thread_start(helper_detail::serve, helper, 256 * 1024, nullptr);
    return helper->thread != nullptr;
}

inline void helper_close(Helper* helper) noexcept {
    if (helper->thread) {
        helper->stop = 1;
        thread_wait(helper->thread, 2000);
        thread_close(helper->thread);
    }
    if (helper->display) {
        g_client.display_flush(helper->display);
        g_client.display_disconnect(helper->display);
    }
    clear_bytes(helper, static_cast<u32>(sizeof(*helper)));
}

// Waits for a flag the service thread sets: `done()` true within the time, polling.
template <typename Done> bool helper_wait(Done done, u32 timeout_ms) noexcept {
    const u64 start = clock_monotonic_milliseconds();
    while (!done()) {
        if (clock_monotonic_milliseconds() - start >= timeout_ms)
            return false;
        thread_sleep(5);
    }
    return true;
}

// The helper owns the clipboard with these formats until it sets another.
inline void helper_set_selection(Helper* helper, const Offered* offered, u32 count) noexcept {
    if (helper->source)
        zwlr_data_control_source_v1_destroy(helper->source);
    for (u32 i = 0; i < count && i < kMaxOffered; ++i)
        helper->offered[i] = offered[i];
    helper->offered_count = count < kMaxOffered ? count : kMaxOffered;
    helper->source = zwlr_data_control_manager_v1_create_data_source(helper->data_manager);
    zwlr_data_control_source_v1_add_listener(helper->source, &helper_detail::kSourceListener, nullptr);
    for (u32 i = 0; i < helper->offered_count; ++i)
        zwlr_data_control_source_v1_offer(helper->source, helper->offered[i].mime);
    zwlr_data_control_device_v1_set_selection(helper->data_device, helper->source);
    g_client.display_flush(helper->display);
}

// Asks the owner of the selection for one format; the read end of the pipe it will write to, or -1. The owner is
// the window under test: the caller pumps it until the pipe has data.
inline int helper_receive(Helper* helper, const char* mime) noexcept {
    HelperOffer* offer = helper->selection;
    int fds[2];
    if (!offer || !offer->offer || syscall_failed(syscall2(293, reinterpret_cast<long>(fds), kOpenCloseOnExec)))
        return -1;
    zwlr_data_control_offer_v1_receive(offer->offer, mime, fds[1]);
    g_client.display_flush(helper->display);
    sys_close(fds[1]);
    return fds[0];
}

// The input method answers the text input: all of these are committed together.
inline void helper_ime_preedit(Helper* helper, const char* text, i32 begin, i32 end) noexcept {
    zwp_input_method_v2_set_preedit_string(helper->ime, text, begin, end);
}
inline void helper_ime_commit_string(Helper* helper, const char* text) noexcept {
    zwp_input_method_v2_commit_string(helper->ime, text);
}
inline void helper_ime_delete(Helper* helper, u32 before, u32 after) noexcept {
    zwp_input_method_v2_delete_surrounding_text(helper->ime, before, after);
}
inline void helper_ime_commit(Helper* helper) noexcept {
    zwp_input_method_v2_commit(helper->ime, helper->ime_done_count);
    g_client.display_flush(helper->display);
}

} // namespace maicord::inject
