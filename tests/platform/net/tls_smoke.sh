#!/bin/sh
# The TLS client over the system OpenSSL against local `openssl s_server` processes (loopback only, no network).
#
#   tls_smoke.sh <tls_client test binary> <work directory>
#
# Makes a throwaway EC certificate for localhost and 127.0.0.1 and starts four servers on ports the kernel picks (TLS
# 1.3 and TLS 1.2, each as a status page and as a file server), then runs the cases of tests/platform/net/
# tls_client_linux.cpp against them with that certificate as the only trust (SSL_CERT_FILE, the override the client
# honours): sessions by name and by address, a download of 100,000 bytes over many records, a host name the
# certificate does not name, a certificate nobody trusts, and a server that only speaks TLS 1.1. Exits 77 (skipped)
# when there is no `openssl` command. Prints the failing case and the server logs, and exits 1, on any failure.
set -u

binary=$1
work=$2

if ! command -v openssl >/dev/null 2>&1; then
    echo "SKIP: no openssl command"
    exit 77
fi

rm -rf "$work"
mkdir -p "$work"
cd "$work" || exit 1

pids=""
failed=0

cleanup() {
    for pid in $pids; do
        kill "$pid" 2>/dev/null
    done
    for pid in $pids; do
        wait "$pid" 2>/dev/null
    done
    if [ "$failed" != 0 ]; then
        for log in server-*.log; do
            [ -f "$log" ] && { echo "--- $log"; cat "$log"; }
        done
    fi
    cd / && rm -rf "$work"
}
trap cleanup EXIT INT TERM

certificate() {
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout "$1.key" -out "$1.pem" -days 2 \
        -subj /CN=localhost -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1
}

certificate server || { echo "FAIL: cannot make the server certificate"; failed=1; exit 1; }
certificate other || { echo "FAIL: cannot make the second certificate"; failed=1; exit 1; }
yes 0123456789abcdef | head -c 100000 > payload

# start_server <name> <s_server option>...: sets port_<name> to the port it listens on.
start_server() {
    name=$1
    shift
    openssl s_server -accept 127.0.0.1:0 -cert server.pem -key server.key -alpn http/1.1 -ign_eof "$@" \
        </dev/null >"server-$name.log" 2>&1 &
    pids="$pids $!"
    tries=0
    while ! grep -q '^ACCEPT' "server-$name.log" 2>/dev/null; do
        tries=$((tries + 1))
        if [ "$tries" -gt 100 ]; then
            echo "FAIL: s_server $name did not start"
            failed=1
            return 1
        fi
        sleep 0.05
    done
    port=$(sed -n 's/^ACCEPT .*:\([0-9][0-9]*\)$/\1/p' "server-$name.log" | head -n 1)
    eval "port_$name=$port"
}

start_server www13 -www || exit 1
start_server www12 -www -tls1_2 || exit 1
start_server file13 -WWW || exit 1
start_server file12 -WWW -tls1_2 || exit 1
# A TLS 1.1 server (security level 0 enables it); some builds cannot offer it, and then the case is skipped.
legacy=0
if start_server legacy -www -tls1_1 -cipher 'ALL:@SECLEVEL=0'; then
    legacy=1
else
    failed=0
fi

export SSL_CERT_FILE="$work/server.pem"

# run <description> <arguments>...
run() {
    description=$1
    shift
    if ! "$binary" "$@"; then
        echo "FAIL: $description"
        failed=1
    fi
}

# run_untrusted <description> <arguments>...: the same with a trust that does not hold the server's certificate.
run_untrusted() {
    description=$1
    shift
    if ! env SSL_CERT_FILE="$work/other.pem" "$binary" "$@"; then
        echo "FAIL: $description"
        failed=1
    fi
}

run "TLS 1.3 session by name" --connect "$port_www13" localhost TLSv1.3
run "TLS 1.3 session by address" --connect "$port_www13" 127.0.0.1 TLSv1.3
run "TLS 1.2 session by name" --connect "$port_www12" localhost TLSv1.2
run "TLS 1.3 download" --download "$port_file13" localhost 100000
run "TLS 1.2 download" --download "$port_file12" localhost 100000
# X509_V_ERR_HOSTNAME_MISMATCH (62) as 0x02000000 | 62.
run "wrong host name" --reject "$port_www13" wrong.example 0200003e
run "wrong host name over TLS 1.2" --reject "$port_www12" wrong.example 0200003e
# X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT (18): the server's certificate is self-signed and this trust does not hold it.
run_untrusted "untrusted certificate" --reject "$port_www13" localhost 02000012
if [ "$legacy" = 1 ]; then
    # Anything below TLS 1.2 is refused: the handshake fails in the library (an OpenSSL error code, bit 31 set).
    run "TLS 1.1 server" --reject-class "$port_legacy" localhost 80000000
else
    echo "NOTE: this openssl cannot serve TLS 1.1; the minimum-version case is skipped"
fi

# wait_for_line <log> <line>: until the server has printed the line (what the client sent), or a few seconds pass.
wait_for_line() {
    tries=0
    while ! grep -qx "$2" "$1" 2>/dev/null; do
        tries=$((tries + 1))
        if [ "$tries" -gt 100 ]; then
            return 1
        fi
        sleep 0.05
    done
}

# start_driven <name> <s_server option>...: like start_server, but the server takes its commands from the FIFO
# commands-<name>, held open for writing on file descriptor 3 until the caller closes it. s_server reads its input with
# one read per command, so the caller pauses between commands.
start_driven() {
    name=$1
    shift
    mkfifo "commands-$name"
    exec 3<>"commands-$name"
    openssl s_server -accept 127.0.0.1:0 -cert server.pem -key server.key -alpn http/1.1 "$@" \
        <&3 >"server-$name.log" 2>&1 &
    pids="$pids $!"
    tries=0
    while ! grep -q '^ACCEPT' "server-$name.log" 2>/dev/null; do
        tries=$((tries + 1))
        if [ "$tries" -gt 100 ]; then
            echo "FAIL: s_server $name did not start"
            failed=1
            return 1
        fi
        sleep 0.05
    done
    port=$(sed -n 's/^ACCEPT .*:\([0-9][0-9]*\)$/\1/p' "server-$name.log" | head -n 1)
    eval "port_$name=$port"
}

# A key update the server asks for in mid-session (TLS 1.3): "K" sends one and requests the client's, "Q" ends the
# session. The client sends A, receives the update and "hello", sends B (the first record after the update, with the
# client's own update in front of it) and expects the server to close.
if start_driven rekey -msg; then
    "$binary" --rekey "$port_rekey" localhost &
    client=$!
    if wait_for_line server-rekey.log A; then
        printf 'K\n' >&3
        sleep 0.3
        printf 'hello\n' >&3
        wait_for_line server-rekey.log B || { echo "FAIL: key update, the server never read the second line"; failed=1; }
    else
        echo "FAIL: key update, the server never read the first line"
        failed=1
    fi
    printf 'Q\n' >&3
    wait "$client" || { echo "FAIL: key update session"; failed=1; }
    # The server sent an update that asked for the client's and received the client's answer.
    grep -q '^>>> TLS 1.3, Handshake \[length 0005\], KeyUpdate' server-rekey.log &&
        grep -q '^<<< TLS 1.3, Handshake \[length 0005\], KeyUpdate' server-rekey.log ||
        { echo "FAIL: key update, the server log shows no update in both directions"; failed=1; }
fi
exec 3>&-

# A renegotiation the server asks for (TLS 1.2): "R" makes it send a HelloRequest, which the client refuses with a
# warning alert that OpenSSL writes while decrypting; the client checks that the alert waits to go out first.
if start_driven control -tls1_2; then
    "$binary" --control "$port_control" localhost &
    client=$!
    if wait_for_line server-control.log A; then
        printf 'R\n' >&3
    else
        echo "FAIL: renegotiation, the server never read the first line"
        failed=1
    fi
    wait "$client" || { echo "FAIL: renegotiation session"; failed=1; }
fi
exec 3>&-

[ "$failed" = 0 ] && echo "PASS maicord_tls_smoke"
exit "$failed"
