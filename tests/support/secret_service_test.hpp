#pragma once
// Secret Service helpers the tests need and the client does not: a status's name for reports, and the removal of the
// items a test stored in a real keyring (the client never deletes its master key). Over the public D-Bus client.
#include "maicord/platform/dbus/connection.hpp"
#include "maicord/platform/dbus/secret_service.hpp"

namespace maicord::dbus {

inline const char* secret_status_text(SecretStatus status) noexcept {
    switch (status) {
    case SecretStatus::ok:
        return "ok";
    case SecretStatus::not_found:
        return "not found";
    case SecretStatus::unavailable:
        return "unavailable";
    case SecretStatus::locked:
        return "locked";
    default:
        return "failed";
    }
}

// Deletes the unlocked items whose attributes include all of `attributes` (SearchItems, then Item.Delete); how many
// went, -1 when the service could not be asked.
inline i32 secret_test_erase(const SecretAttribute* attributes, u32 count) noexcept {
    Connection* c = connection_open_session();
    if (!c)
        return -1;
    Builder body;
    builder_init(&body);
    builder_open_array(&body, "{ss}");
    for (u32 i = 0; i < count; ++i) {
        builder_open_entry(&body);
        builder_string(&body, attributes[i].name);
        builder_string(&body, attributes[i].value);
        builder_close_entry(&body);
    }
    builder_close_array(&body);
    const CallTarget search{"org.freedesktop.secrets", "/org/freedesktop/secrets", "org.freedesktop.Secret.Service",
                            "SearchItems"};
    Message reply;
    const Status status = connection_call_sync(c, search, &body, 4000, &reply);
    builder_release(&body);
    i32 deleted = -1;
    if (status == Status::ok) {
        deleted = 0;
        Reader r;
        message_reader(reply, &r);
        const char* path = nullptr;
        if (reader_enter_array(&r)) {
            while (reader_more(r) && reader_string(&r, &path, nullptr)) {
                const CallTarget remove{"org.freedesktop.secrets", path, "org.freedesktop.Secret.Item", "Delete"};
                Message answer;
                const Status removed = connection_call_sync(c, remove, nullptr, 4000, &answer);
                if (removed == Status::ok || removed == Status::error_reply)
                    message_release(&answer);
                deleted += removed == Status::ok ? 1 : 0;
            }
        }
    }
    if (status == Status::ok || status == Status::error_reply)
        message_release(&reply);
    connection_close(c);
    return deleted;
}

} // namespace maicord::dbus
