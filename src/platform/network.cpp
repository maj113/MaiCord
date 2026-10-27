#include "maicord/core/win32_process.hpp"
#include "maicord/core/memory.hpp"
#include "network_native.hpp"

#include "maicord/sys/winsock.hpp"

namespace maicord {
namespace {

volatile LONG g_winsock_pinned = 0;

NetworkResult success() noexcept {
    return NetworkResult{0, NetworkError::none};
}

NetworkResult failure(NetworkError error, i32 native_error) noexcept {
    return NetworkResult{native_error, error};
}

void initialize_operation(NetworkOperation* operation, NetworkConnection* owner, NetworkOperationKind kind,
                          u32 generation) noexcept {
    reset_value<NetworkOperation>(operation);
    operation->owner = owner;
    operation->generation = generation;
    operation->kind = kind;
}

bool post_resolve_complete(NetworkResolver* resolver, DWORD native_error) noexcept {
    resolver->native_error = static_cast<i32>(native_error);
    // Publication transfers ownership to the consumer, which may immediately
    // release the resolver. Do not access it after posting the packet.
    return PostQueuedCompletionStatus(resolver->completion_port, 0,
                                      reinterpret_cast<ULONG_PTR>(resolver),
                                      native(&resolver->operation.overlapped)) != FALSE;
}

void CALLBACK resolve_complete(DWORD native_error, DWORD, OVERLAPPED* overlapped) noexcept {
    auto* operation = reinterpret_cast<NetworkOperation*>(overlapped);
    post_resolve_complete(static_cast<NetworkResolver*>(operation->owner), native_error);
}

bool bind_wildcard(SOCKET socket_value, i32 family) noexcept {
    if (family == AF_INET) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        return bind(socket_value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    }
    if (family == AF_INET6) {
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        reset_value<IN6_ADDR>(&address.sin6_addr);
        return bind(socket_value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    }
    WSASetLastError(WSAEAFNOSUPPORT);
    return false;
}

} // namespace

NetworkResult network_loop_initialize(NetworkLoop* loop) noexcept {
    if (!loop) {
        return failure(NetworkError::invalid_argument, WSAEINVAL);
    }
    reset_value<NetworkLoop>(loop);

    WSADATA data{};
    const i32 startup_error = WSAStartup(MAKEWORD(2, 2), &data);
    if (startup_error != 0) {
        return failure(NetworkError::winsock_startup, startup_error);
    }
    loop->winsock_started = true;
    // A cancelled GetAddrInfoExW lookup keeps running on a Winsock pool thread
    // after its WSA_E_CANCELLED completion, and the last WSACleanup frees the
    // namespace catalog under it. Pin one reference for the process lifetime.
    if (InterlockedCompareExchange(&g_winsock_pinned, 1, 0) == 0 && WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        InterlockedExchange(&g_winsock_pinned, 0);
    }

    loop->completion_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (!loop->completion_port) {
        const i32 native_error = static_cast<i32>(thread_last_error());
        network_loop_release(loop);
        return failure(NetworkError::completion_port_create, native_error);
    }
    return success();
}

void network_loop_release(NetworkLoop* loop) noexcept {
    if (!loop) {
        return;
    }
    if (loop->completion_port) {
        CloseHandle(loop->completion_port);
    }
    if (loop->winsock_started) {
        WSACleanup();
    }
    reset_value<NetworkLoop>(loop);
}

NetworkResult network_connection_connect(NetworkLoop* loop, NetworkConnection* connection,
                                         const sockaddr* address, i32 address_bytes) noexcept {
    if (!loop || !loop->completion_port || !connection || !address || address_bytes <= 0 ||
        (address->sa_family != AF_INET && address->sa_family != AF_INET6)) {
        return failure(NetworkError::invalid_argument, WSAEINVAL);
    }

    reset_value<NetworkConnection>(connection);
    connection->socket = INVALID_SOCKET;
    connection->generation = 1;
    const SOCKET socket_value =
        WSASocketW(address->sa_family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (socket_value == INVALID_SOCKET) {
        return failure(NetworkError::socket_create, WSAGetLastError());
    }
    connection->socket = socket_value;
    const BOOL no_delay=TRUE;
    if(setsockopt(socket_value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&no_delay),sizeof(no_delay))==SOCKET_ERROR) {
        const i32 native_error=WSAGetLastError();network_connection_close(connection);
        return failure(NetworkError::socket_create,native_error);
    }

    const HANDLE associated =
        CreateIoCompletionPort(reinterpret_cast<HANDLE>(socket_value), loop->completion_port,
                               reinterpret_cast<ULONG_PTR>(connection), 0);
    if (!associated) {
        const i32 native_error = static_cast<i32>(thread_last_error());
        network_connection_close(connection);
        return failure(NetworkError::completion_port_associate, native_error);
    }

    GUID identifier = WSAID_CONNECTEX;
    LPFN_CONNECTEX connect_ex = nullptr;
    DWORD returned = 0;
    if (WSAIoctl(socket_value, SIO_GET_EXTENSION_FUNCTION_POINTER, &identifier, sizeof(identifier),
                 &connect_ex, sizeof(connect_ex), &returned, nullptr, nullptr) == SOCKET_ERROR ||
        !connect_ex) {
        const i32 native_error = WSAGetLastError();
        network_connection_close(connection);
        return failure(NetworkError::connect_extension, native_error);
    }
    if (!bind_wildcard(socket_value, address->sa_family)) {
        const i32 native_error = WSAGetLastError();
        network_connection_close(connection);
        return failure(NetworkError::bind_local, native_error);
    }

    initialize_operation(&connection->connect_operation, connection, NetworkOperationKind::connect,
                         connection->generation);
    connection->state = NetworkConnectionState::connecting;
    connection->connect_pending = true;
    const BOOL started = connect_ex(socket_value, address, address_bytes, nullptr, 0, nullptr,
                                    native(&connection->connect_operation.overlapped));
    if (!started) {
        const i32 native_error = WSAGetLastError();
        if (native_error != WSA_IO_PENDING) {
            connection->connect_pending = false;
            connection->state = NetworkConnectionState::failed;
            network_connection_close(connection);
            return failure(NetworkError::connect_start, native_error);
        }
    }
    return success();
}

NetworkResult network_connection_receive(NetworkConnection* connection, Slice<u8> buffer) noexcept {
    if (!connection || connection->state != NetworkConnectionState::connected ||
        connection->receive_pending || !buffer.data || buffer.count == 0) {
        return failure(NetworkError::invalid_argument, WSAEINVAL);
    }
    initialize_operation(&connection->receive_operation, connection, NetworkOperationKind::receive,
                         connection->generation);
    connection->receive_operation.buffer.buf = reinterpret_cast<char*>(buffer.data);
    connection->receive_operation.buffer.len = buffer.count;
    connection->receive_pending = true;
    const i32 started = WSARecv(connection->socket, native(&connection->receive_operation.buffer), 1, nullptr,
                                &connection->receive_operation.receive_flags,
                                native(&connection->receive_operation.overlapped), nullptr);
    if (started == SOCKET_ERROR) {
        const i32 native_error = WSAGetLastError();
        if (native_error != WSA_IO_PENDING) {
            connection->receive_pending = false;
            return failure(NetworkError::receive_start, native_error);
        }
    }
    return success();
}

NetworkResult network_connection_send(NetworkConnection* connection, ConstSlice<u8> buffer) noexcept {
    if (!connection || connection->state != NetworkConnectionState::connected || connection->send_pending ||
        !buffer.data || buffer.count == 0) {
        return failure(NetworkError::invalid_argument, WSAEINVAL);
    }
    initialize_operation(&connection->send_operation, connection, NetworkOperationKind::send,
                         connection->generation);
    connection->send_operation.buffer.buf = reinterpret_cast<char*>(const_cast<u8*>(buffer.data));
    connection->send_operation.buffer.len = buffer.count;
    connection->send_pending = true;
    const i32 started = WSASend(connection->socket, native(&connection->send_operation.buffer), 1, nullptr, 0,
                                native(&connection->send_operation.overlapped), nullptr);
    if (started == SOCKET_ERROR) {
        const i32 native_error = WSAGetLastError();
        if (native_error != WSA_IO_PENDING) {
            connection->send_pending = false;
            return failure(NetworkError::send_start, native_error);
        }
    }
    return success();
}

void network_connection_close(NetworkConnection* connection) noexcept {
    if (!connection) {
        return;
    }
    if (connection->socket != INVALID_SOCKET && connection->socket != 0) {
        closesocket(connection->socket);
    }
    connection->socket = INVALID_SOCKET;
    ++connection->generation;
    connection->state = NetworkConnectionState::closed;
}

NetworkResult network_resolver_start(NetworkLoop* loop, NetworkResolver* resolver,
                                     const wchar_t* hostname, const wchar_t* service) noexcept {
    if (!loop || !loop->completion_port || !resolver || resolver->pending || resolver->results ||
        !hostname || hostname[0] == 0) {
        return failure(NetworkError::invalid_argument, WSAEINVAL);
    }

    reset_value<NetworkResolver>(resolver);
    resolver->generation = 1;
    resolver->operation.owner = resolver;
    resolver->operation.generation = resolver->generation;
    resolver->operation.kind = NetworkOperationKind::resolve;
    resolver->hints.ai_family = AF_UNSPEC;
    resolver->hints.ai_socktype = SOCK_STREAM;
    resolver->hints.ai_protocol = IPPROTO_TCP;
    resolver->completion_port = loop->completion_port;
    resolver->state = NetworkResolverState::resolving;
    resolver->pending = true;

    const i32 started = GetAddrInfoExW(hostname, service, NS_DNS, nullptr, native(&resolver->hints),
                                      &resolver->results, nullptr, native(&resolver->operation.overlapped),
                                      resolve_complete, &resolver->cancel_handle);
    if (started == WSA_IO_PENDING) {
        return success();
    }

    // Winsock only invokes the callback for the pending path. Normalize an
    // immediate success or failure into the same NetworkLoop completion stream.
    if (!post_resolve_complete(resolver, static_cast<DWORD>(started))) {
        resolver->pending = false;
        resolver->state = NetworkResolverState::failed;
        return failure(NetworkError::resolve_start, static_cast<i32>(thread_last_error()));
    }
    return success();
}

NetworkResult network_resolver_cancel(NetworkResolver* resolver) noexcept {
    if (!resolver || !resolver->pending || resolver->state != NetworkResolverState::resolving ||
        !resolver->cancel_handle) {
        return failure(NetworkError::invalid_argument, WSAEINVAL);
    }
    const i32 cancelled = GetAddrInfoExCancel(&resolver->cancel_handle);
    if (cancelled != 0) {
        return failure(NetworkError::resolve_cancel, cancelled);
    }
    return success();
}

bool network_resolver_release(NetworkResolver* resolver) noexcept {
    if (!resolver || resolver->pending) {
        return false;
    }
    if (resolver->results) {
        FreeAddrInfoExW(resolver->results);
    }
    reset_value<NetworkResolver>(resolver);
    return true;
}

NetworkResult network_signal_post(NetworkLoop* loop, NetworkSignal* signal, u32 value) noexcept {
    if (!loop || !loop->completion_port || !signal ||
        InterlockedCompareExchange(&signal->pending, 1, 0) != 0) {
        return failure(NetworkError::invalid_argument, ERROR_INVALID_PARAMETER);
    }
    reset_value<NetworkOperation>(&signal->operation);
    signal->operation.owner = signal;
    signal->operation.kind = NetworkOperationKind::signal;
    signal->value = value;
    if (!PostQueuedCompletionStatus(loop->completion_port, 0, reinterpret_cast<ULONG_PTR>(signal),
                                    native(&signal->operation.overlapped))) {
        const i32 native_error = static_cast<i32>(thread_last_error());
        InterlockedExchange(&signal->pending, 0);
        return failure(NetworkError::signal_post, native_error);
    }
    return success();
}

NetworkPollResult network_loop_poll(NetworkLoop* loop, u32 timeout_milliseconds) noexcept {
    NetworkPollResult result{};
    result.state = NetworkPollState::error;
    if (!loop || !loop->completion_port) {
        result.completion.native_error = ERROR_INVALID_PARAMETER;
        return result;
    }

    DWORD transferred = 0;
    ULONG_PTR completion_key = 0;
    OVERLAPPED* overlapped = nullptr;
    const BOOL succeeded = GetQueuedCompletionStatus(loop->completion_port, &transferred, &completion_key,
                                                     &overlapped, timeout_milliseconds);
    if (!overlapped) {
        const i32 native_error = static_cast<i32>(thread_last_error());
        if (!succeeded && native_error == WAIT_TIMEOUT) {
            result.state = NetworkPollState::timeout;
            return result;
        }
        result.completion.native_error = succeeded ? 0 : native_error;
        return result;
    }

    auto* operation = reinterpret_cast<NetworkOperation*>(overlapped);
    if (operation->kind == NetworkOperationKind::signal) {
        auto* signal = static_cast<NetworkSignal*>(operation->owner);
        result.state = NetworkPollState::completion;
        result.completion.signal = signal;
        result.completion.signal_value = signal ? signal->value : 0;
        result.completion.kind = NetworkOperationKind::signal;
        result.completion.succeeded = signal != nullptr;
        result.completion.stale = !signal || completion_key != reinterpret_cast<ULONG_PTR>(signal);
        if (signal) {
            InterlockedExchange(&signal->pending, 0);
        }
        return result;
    }
    if (operation->kind == NetworkOperationKind::resolve) {
        auto* resolver = static_cast<NetworkResolver*>(operation->owner);
        result.state = NetworkPollState::completion;
        result.completion.resolver = resolver;
        result.completion.native_error = resolver ? resolver->native_error : WSAEINVAL;
        result.completion.kind = NetworkOperationKind::resolve;
        result.completion.succeeded = resolver && resolver->native_error == 0;
        result.completion.stale = !resolver || operation->generation != resolver->generation ||
                                  completion_key != reinterpret_cast<ULONG_PTR>(resolver);
        if (result.completion.stale) {
            return result;
        }
        resolver->pending = false;
        resolver->cancel_handle = nullptr;
        if (resolver->native_error == 0) {
            resolver->state = NetworkResolverState::resolved;
        } else if (resolver->native_error == WSA_E_CANCELLED) {
            resolver->state = NetworkResolverState::cancelled;
        } else {
            resolver->state = NetworkResolverState::failed;
        }
        return result;
    }

    if (operation->kind == NetworkOperationKind::file_read || operation->kind == NetworkOperationKind::file_write) {
        auto* file = static_cast<NetworkFile*>(operation->owner);
        result.state = NetworkPollState::completion;
        result.completion.file = file;
        result.completion.transferred = transferred;
        result.completion.native_error = succeeded ? 0 : static_cast<i32>(thread_last_error());
        result.completion.kind = operation->kind;
        result.completion.succeeded = succeeded != FALSE;
        result.completion.stale = !file || operation->generation != file->generation ||
            completion_key != reinterpret_cast<ULONG_PTR>(file);
        if (!result.completion.stale) file->pending = false;
        return result;
    }

    if (operation->kind == NetworkOperationKind::datagram_receive || operation->kind == NetworkOperationKind::datagram_send) {
        auto* datagram = static_cast<NetworkDatagram*>(operation->owner);
        result.state = NetworkPollState::completion;
        result.completion.datagram = datagram;
        result.completion.transferred = transferred;
        result.completion.native_error = succeeded ? 0 : static_cast<i32>(thread_last_error());
        result.completion.kind = operation->kind;
        result.completion.succeeded = succeeded != FALSE;
        const bool owned = datagram && operation->generation == datagram->generation &&
            completion_key == reinterpret_cast<ULONG_PTR>(datagram);
        result.completion.stale = !owned || !datagram->open;
        // Even a cancelled/closed owner's packet retires the corresponding I/O.
        // A zero-length receive is a valid datagram, never a peer-close signal.
        if (owned) {
            if (operation->kind == NetworkOperationKind::datagram_send) {
                datagram->send_pending = false;
            } else if (operation == &datagram->receive_operation) {
                datagram->receive_pending = false;
            } else {
                const auto slot = static_cast<u32>(operation - datagram->extra_receive_operations);
                result.completion.datagram_slot = static_cast<u8>(slot + 1);
                datagram->extra_receive_pending = static_cast<u8>(datagram->extra_receive_pending & ~(1u << slot));
            }
        }
        return result;
    }
    auto* connection = static_cast<NetworkConnection*>(operation->owner);
    result.state = NetworkPollState::completion;
    result.completion.connection = connection;
    result.completion.transferred = transferred;
    result.completion.native_error = succeeded ? 0 : static_cast<i32>(thread_last_error());
    result.completion.kind = operation->kind;
    result.completion.succeeded = succeeded != FALSE;
    result.completion.stale = !connection || operation->generation != connection->generation ||
                              completion_key != reinterpret_cast<ULONG_PTR>(connection);
    if (result.completion.stale) {
        return result;
    }

    switch (operation->kind) {
    case NetworkOperationKind::connect:
        connection->connect_pending = false;
        if (succeeded &&
            setsockopt(connection->socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) == 0) {
            connection->state = NetworkConnectionState::connected;
        } else {
            if (succeeded) {
                result.completion.succeeded = false;
                result.completion.native_error = WSAGetLastError();
            }
            connection->state = NetworkConnectionState::failed;
        }
        break;
    case NetworkOperationKind::receive:
        connection->receive_pending = false;
        if (!succeeded) {
            connection->state = NetworkConnectionState::failed;
        } else if (transferred == 0) {
            connection->state = NetworkConnectionState::peer_closed;
        }
        break;
    case NetworkOperationKind::send:
        connection->send_pending = false;
        if (!succeeded) {
            connection->state = NetworkConnectionState::failed;
        }
        break;
    case NetworkOperationKind::resolve:
    case NetworkOperationKind::signal:
    case NetworkOperationKind::file_read:
    case NetworkOperationKind::file_write:
    case NetworkOperationKind::datagram_receive:
    case NetworkOperationKind::datagram_send:
        break;
    }
    return result;
}

} // namespace maicord
