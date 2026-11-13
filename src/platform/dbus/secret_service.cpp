#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/platform/dbus/connection.hpp"
#include "maicord/platform/dbus/secret_service.hpp"

namespace maicord::dbus {
namespace {

constexpr const char* kService = "org.freedesktop.secrets";
constexpr const char* kServicePath = "/org/freedesktop/secrets";
constexpr const char* kServiceInterface = "org.freedesktop.Secret.Service";
constexpr const char* kCollectionInterface = "org.freedesktop.Secret.Collection";
constexpr const char* kItemInterface = "org.freedesktop.Secret.Item";
constexpr const char* kSessionInterface = "org.freedesktop.Secret.Session";
constexpr const char* kDefaultAlias = "/org/freedesktop/secrets/aliases/default";
constexpr u32 kCallTimeoutMs = 4000;
constexpr u32 kPathCapacity = 192;
constexpr u32 kMaxItems = 8;

struct Client {
    Connection* connection;
    char session[kPathCapacity];
};

SecretStatus classify(const Message& reply) noexcept {
    const char* name = reply.error_name ? reply.error_name : "";
    if (c_string_equal(name, "org.freedesktop.Secret.Error.IsLocked"))
        return SecretStatus::locked;
    if (c_string_equal(name, "org.freedesktop.DBus.Error.ServiceUnknown") || c_string_equal(name, "org.freedesktop.DBus.Error.NameHasNoOwner") ||
        c_string_equal(name, "org.freedesktop.DBus.Error.NoReply") || c_string_equal(name, "org.freedesktop.DBus.Error.Disconnected") ||
        c_string_equal(name, "org.freedesktop.DBus.Error.AccessDenied") || c_string_equal(name, "org.freedesktop.DBus.Error.Spawn.ServiceNotFound"))
        return SecretStatus::unavailable;
    log_write(LogLevel::warning, "secret_service", name);
    return SecretStatus::failed;
}

// A call whose reply is the method return: false with *status set otherwise. On true the caller releases `reply`.
bool call(Client* client, const CallTarget& target, const Builder* body, Message* reply, SecretStatus* status) noexcept {
    const Status result = connection_call_sync(client->connection, target, body, kCallTimeoutMs, reply);
    if (result == Status::ok)
        return true;
    if (result == Status::error_reply) {
        *status = classify(*reply);
        message_release(reply);
    } else {
        *status = result == Status::failed ? SecretStatus::failed : SecretStatus::unavailable;
    }
    return false;
}

bool copy_path(const char* path, u32 length, char* out) noexcept {
    if (length >= kPathCapacity)
        return false;
    copy_bytes(out, path, length + 1);
    return true;
}

bool open_client(Client* client, SecretStatus* status) noexcept {
    client->connection = connection_open_session();
    if (!client->connection) {
        *status = SecretStatus::unavailable;
        return false;
    }
    Builder body;
    builder_init(&body);
    builder_string(&body, "plain");
    builder_open_variant(&body, "s");
    builder_string(&body, "");
    builder_close_variant(&body);
    Message reply;
    const CallTarget target{kService, kServicePath, kServiceInterface, "OpenSession"};
    const bool ok = call(client, target, &body, &reply, status);
    builder_release(&body);
    if (!ok) {
        connection_close(client->connection);
        client->connection = nullptr;
        return false;
    }
    // (v output, o session)
    Reader r;
    message_reader(reply, &r);
    const char* path = nullptr;
    u32 length = 0;
    const bool parsed = reader_skip(&r) && reader_string(&r, &path, &length) && copy_path(path, length, client->session);
    message_release(&reply);
    if (!parsed) {
        *status = SecretStatus::failed;
        connection_close(client->connection);
        client->connection = nullptr;
        return false;
    }
    return true;
}

void close_client(Client* client) noexcept {
    if (!client->connection)
        return;
    const CallTarget target{kService, client->session, kSessionInterface, "Close"};
    connection_call(client->connection, target, nullptr, 0, nullptr, nullptr);
    connection_close(client->connection);
    client->connection = nullptr;
}

void append_attributes(Builder* body, const SecretAttribute* attributes, u32 count) noexcept {
    builder_open_array(body, "{ss}");
    for (u32 i = 0; i < count; ++i) {
        builder_open_entry(body);
        builder_string(body, attributes[i].name);
        builder_string(body, attributes[i].value);
        builder_close_entry(body);
    }
    builder_close_array(body);
}

// An `ao` into `paths`: the first kMaxItems are kept, the rest read and dropped.
bool read_paths(Reader* r, char (*paths)[kPathCapacity], u32* count) noexcept {
    bool parsed = reader_enter_array(r);
    while (parsed && reader_more(*r)) {
        const char* path = nullptr;
        u32 length = 0;
        parsed = reader_string(r, &path, &length);
        if (parsed && *count < kMaxItems)
            parsed = copy_path(path, length, paths[(*count)++]);
    }
    return parsed && reader_leave(r);
}

struct Found {
    char unlocked[kMaxItems][kPathCapacity];
    char locked[kMaxItems][kPathCapacity];
    u32 unlocked_count;
    u32 locked_count;
};

// Unlock(ao objects) -> (ao unlocked, o prompt): a keyring that needs no password unlocks without asking anyone. True
// when everything asked for is unlocked and no prompt is pending (a prompt would need the user, and is never run).
// The unlocked paths replace `unlocked`'s contents when it is given.
bool unlock_paths(Client* client, const char (*paths)[kPathCapacity], u32 count, char (*unlocked)[kPathCapacity],
                  u32* unlocked_count) noexcept {
    Builder body;
    builder_init(&body);
    builder_open_array(&body, "o");
    for (u32 i = 0; i < count; ++i)
        builder_object_path(&body, paths[i]);
    builder_close_array(&body);
    Message reply;
    SecretStatus ignored = SecretStatus::failed;
    const CallTarget target{kService, kServicePath, kServiceInterface, "Unlock"};
    const bool ok = call(client, target, &body, &reply, &ignored);
    builder_release(&body);
    if (!ok)
        return false;
    Reader r;
    message_reader(reply, &r);
    u32 done = 0;
    bool parsed = reader_enter_array(&r);
    while (parsed && reader_more(r)) {
        const char* path = nullptr;
        u32 length = 0;
        parsed = reader_string(&r, &path, &length);
        if (parsed && unlocked && done < kMaxItems)
            parsed = copy_path(path, length, unlocked[done]);
        ++done;
    }
    const char* prompt = nullptr;
    u32 prompt_length = 0;
    parsed = parsed && reader_leave(&r) && reader_string(&r, &prompt, &prompt_length);
    const bool no_prompt = parsed && prompt_length == 1 && prompt[0] == '/';
    message_release(&reply);
    if (unlocked_count)
        *unlocked_count = done < kMaxItems ? done : kMaxItems;
    return no_prompt && done == count;
}

// SearchItems: (ao unlocked, ao locked). Items found locked are unlocked here when that needs no prompt.
SecretStatus search(Client* client, const SecretAttribute* attributes, u32 count, Found* found) noexcept {
    *found = Found{};
    Builder body;
    builder_init(&body);
    append_attributes(&body, attributes, count);
    Message reply;
    SecretStatus status = SecretStatus::failed;
    const CallTarget target{kService, kServicePath, kServiceInterface, "SearchItems"};
    const bool ok = call(client, target, &body, &reply, &status);
    builder_release(&body);
    if (!ok)
        return status;
    Reader r;
    message_reader(reply, &r);
    const bool parsed = read_paths(&r, found->unlocked, &found->unlocked_count) &&
                        read_paths(&r, found->locked, &found->locked_count);
    message_release(&reply);
    if (parsed && found->unlocked_count == 0 && found->locked_count != 0 &&
        unlock_paths(client, found->locked, found->locked_count, found->unlocked, &found->unlocked_count))
        found->locked_count = 0;
    return parsed ? SecretStatus::ok : SecretStatus::failed;
}

} // namespace

const char* secret_status_name(SecretStatus status) noexcept {
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

SecretStatus secret_lookup(const SecretAttribute* attributes, u32 count, u8* secret, u32 capacity, u32* length) noexcept {
    *length = 0;
    Client client{};
    SecretStatus status = SecretStatus::failed;
    if (!open_client(&client, &status))
        return status;
    Found found;
    status = search(&client, attributes, count, &found);
    if (status == SecretStatus::ok) {
        if (found.unlocked_count == 0) {
            status = found.locked_count ? SecretStatus::locked : SecretStatus::not_found;
        } else {
            Builder body;
            builder_init(&body);
            builder_object_path(&body, client.session);
            Message reply;
            const CallTarget target{kService, found.unlocked[0], kItemInterface, "GetSecret"};
            if (call(&client, target, &body, &reply, &status)) {
                // (o session, ay parameters, ay value, s content_type)
                Reader r;
                message_reader(reply, &r);
                const char* text = nullptr;
                const u8* parameters = nullptr;
                const u8* value = nullptr;
                u32 parameter_bytes = 0;
                u32 value_bytes = 0;
                const bool parsed = reader_enter_struct(&r) && reader_string(&r, &text, nullptr) &&
                                    reader_byte_array(&r, &parameters, &parameter_bytes) &&
                                    reader_byte_array(&r, &value, &value_bytes);
                if (!parsed) {
                    status = SecretStatus::failed;
                } else if (value_bytes > capacity) {
                    *length = value_bytes;
                    status = SecretStatus::failed;
                } else {
                    copy_bytes(secret, value, value_bytes);
                    *length = value_bytes;
                    status = SecretStatus::ok;
                }
                message_release(&reply);
            }
            builder_release(&body);
        }
    }
    close_client(&client);
    return status;
}

SecretStatus secret_store(const char* label, const SecretAttribute* attributes, u32 count, const u8* secret,
                          u32 length) noexcept {
    Client client{};
    SecretStatus status = SecretStatus::failed;
    if (!open_client(&client, &status))
        return status;

    char collection[kPathCapacity];
    {
        Builder body;
        builder_init(&body);
        builder_string(&body, "default");
        Message reply;
        const CallTarget target{kService, kServicePath, kServiceInterface, "ReadAlias"};
        copy_bytes(collection, kDefaultAlias, c_string_length(kDefaultAlias) + 1);
        if (call(&client, target, &body, &reply, &status)) {
            Reader r;
            message_reader(reply, &r);
            const char* path = nullptr;
            u32 path_length = 0;
            if (reader_string(&r, &path, &path_length) && !(path_length == 1 && path[0] == '/'))
                copy_path(path, path_length, collection);
            message_release(&reply);
        } else if (status == SecretStatus::unavailable) {
            builder_release(&body);
            close_client(&client);
            return status;
        }
        builder_release(&body);
    }

    Builder body;
    builder_init(&body);
    builder_open_array(&body, "{sv}");
    builder_dict_string(&body, "org.freedesktop.Secret.Item.Label", label);
    builder_open_dict_entry(&body, "org.freedesktop.Secret.Item.Attributes", "a{ss}");
    append_attributes(&body, attributes, count);
    builder_close_dict_entry(&body);
    builder_close_array(&body);
    builder_open_struct(&body, "(oayays)");
    builder_object_path(&body, client.session);
    builder_byte_array(&body, nullptr, 0);
    builder_byte_array(&body, secret, length);
    builder_string(&body, "application/octet-stream");
    builder_close_struct(&body);
    builder_bool(&body, true);  // replace an item with the same attributes
    Message reply;
    const CallTarget target{kService, collection, kCollectionInterface, "CreateItem"};
    // The collection may be locked: (o item, o prompt) with a prompt path and no item (the mock, KWallet), or the error
    // org.freedesktop.Secret.Error.IsLocked (GNOME Keyring). One that needs no password unlocks without a prompt, and then
    // the call is made again.
    for (u32 attempt = 0; attempt != 2; ++attempt) {
        if (call(&client, target, &body, &reply, &status)) {
            Reader r;
            message_reader(reply, &r);
            const char* item = nullptr;
            const char* prompt = nullptr;
            u32 item_length = 0;
            u32 prompt_length = 0;
            const bool parsed = reader_string(&r, &item, &item_length) && reader_string(&r, &prompt, &prompt_length);
            const bool no_item = parsed && item_length == 1 && item[0] == '/';
            status = !parsed ? SecretStatus::failed : no_item ? SecretStatus::locked : SecretStatus::ok;
            message_release(&reply);
        }
        if (status != SecretStatus::locked || attempt == 1)
            break;
        if (!unlock_paths(&client, &collection, 1, nullptr, nullptr))
            break;
    }
    builder_release(&body);
    close_client(&client);
    return status;
}

SecretStatus secret_erase(const SecretAttribute* attributes, u32 count) noexcept {
    Client client{};
    SecretStatus status = SecretStatus::failed;
    if (!open_client(&client, &status))
        return status;
    Found found;
    status = search(&client, attributes, count, &found);
    if (status == SecretStatus::ok) {
        if (found.unlocked_count == 0) {
            status = found.locked_count ? SecretStatus::locked : SecretStatus::not_found;
        } else {
            for (u32 i = 0; i < found.unlocked_count && status == SecretStatus::ok; ++i) {
                Message reply;
                const CallTarget target{kService, found.unlocked[i], kItemInterface, "Delete"};
                if (call(&client, target, nullptr, &reply, &status))
                    message_release(&reply);
            }
        }
    }
    close_client(&client);
    return status;
}

} // namespace maicord::dbus
