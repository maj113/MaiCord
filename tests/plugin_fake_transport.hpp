#pragma once
// Scripted HTTP server for the plugin catalog tests: exact host + target
// routes with in-memory bodies, everything else 404. A request naming the
// wrong host for its origin counts as wrong_host.
#include "maicord/app/update_fetch.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::plugin_fake {

struct Route {
    UpdateHost host;
    const char* target;
    u16 status;
    const char* location;
    const u8* body;
    u64 body_bytes;
    u64 retry_after;
    u32 hits;
};
struct Server {
    Route routes[16];
    u32 count, requests, wrong_host;
    const char* origin; // the Pages or provider host this server answers for
};

inline UpdateTransportError get(void* context, const UpdateHttpRequest& request, UpdateHttpResponse* response) noexcept {
    auto* server = static_cast<Server*>(context);
    ++server->requests;
    *response = {};
    const bool closed = request.host == UpdateHost::pages || request.host == UpdateHost::provider;
    const Utf8View expected = closed ? Utf8View{server->origin, c_string_length(server->origin)} : update_host_name(request.host);
    if (!expected.bytes || request.host_name.bytes != expected.bytes || !bytes_equal(request.host_name.data, expected.data, expected.bytes)) {
        ++server->wrong_host;
        response->status = 404;
        return UpdateTransportError::none;
    }
    Route* route = nullptr;
    for (u32 i = 0; i < server->count; ++i) {
        Route& r = server->routes[i];
        if (r.host == request.host && c_string_length(r.target) == request.target.count &&
            bytes_equal(r.target, request.target.data, request.target.count)) route = &r;
    }
    if (!route) { response->status = 404; return UpdateTransportError::none; }
    ++route->hits;
    response->status = route->status;
    const auto field = [&](const char* name, const char* value) noexcept {
        update_http_observe_field(response, {reinterpret_cast<const u8*>(name), c_string_length(name)},
            {reinterpret_cast<const u8*>(value), c_string_length(value)});
    };
    if (route->location) field("Location", route->location);
    if (route->retry_after) {
        char number[24]{};
        number[decimal_write(number, route->retry_after)] = 0;
        field("Retry-After", number);
    }
    if (route->status < 200 || route->status >= 300) return UpdateTransportError::none;
    response->has_content_length = true;
    response->content_length = route->body_bytes;
    if (route->body_bytes > request.maximum_body_bytes) return UpdateTransportError::oversize;
    for (u64 offset = 0; offset < route->body_bytes;) {
        const u32 n = static_cast<u32>(minimum<u64>(route->body_bytes - offset, 16 * 1024));
        if (!request.sink(request.sink_context, {route->body + offset, n})) return UpdateTransportError::sink;
        response->body_bytes += n;
        offset += n;
    }
    return UpdateTransportError::none;
}
inline UpdateTransport transport(Server& server) noexcept { return {&server, get, nullptr}; }
// Routes keep the target pointer: it must outlive the server's use.
inline Route& serve(Server& server, UpdateHost host, const char* target, u16 status, const void* body = nullptr, u64 bytes = 0,
    const char* location = nullptr) noexcept {
    Route& r = server.routes[server.count < 16 ? server.count++ : 15];
    r = {};
    r.host = host; r.target = target; r.status = status; r.location = location;
    r.body = static_cast<const u8*>(body); r.body_bytes = bytes;
    return r;
}
inline void reset(Server& server, const char* origin) noexcept {
    clear_bytes(&server, sizeof(server));
    server.origin = origin;
}

} // namespace maicord::plugin_fake
