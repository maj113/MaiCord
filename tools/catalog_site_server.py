#!/usr/bin/env python3
"""Serves a built plugin catalog site over HTTPS, for tools/test_catalog_wayland.sh.

    tools/catalog_site_server.py --site DIR --cert CERT.pem --key KEY.pem [--port 443] [--prefix /MaiCord-plugins]
                                 [--log FILE]

DIR is what MaiCord-plugins' scripts/build_catalog.py, build_universal.py and publish_index.py wrote (index-4.json,
index-3.json, details/, previews/, files/). The client reads the catalog at https://<owner>.github.io/<repo>/, so the
test points that name at 127.0.0.1 (a private /etc/hosts) and trusts CERT through SSL_CERT_FILE. Standard library only;
prints "ready" once it listens. Each request is logged as "<status> <path>" to --log.
"""
import argparse
import functools
import http.server
import os
import ssl
import sys
import threading


class Handler(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    prefix = "/"
    log_path = None
    lock = threading.Lock()

    def translate_path(self, path):
        path = path.split("?", 1)[0].split("#", 1)[0]
        if not path.startswith(self.prefix + "/"):
            return os.path.join(self.directory, "\0missing")
        return super().translate_path(path[len(self.prefix):])

    def log_message(self, format, *arguments):
        if self.log_path:
            with self.lock, open(self.log_path, "a") as handle:
                handle.write(f"{arguments[1] if len(arguments) > 1 else '-'} {self.path}\n")

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--site", required=True)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--port", type=int, default=443)
    parser.add_argument("--prefix", default="/MaiCord-plugins")
    parser.add_argument("--log")
    options = parser.parse_args()
    Handler.prefix = options.prefix.rstrip("/")
    Handler.log_path = options.log
    handler = functools.partial(Handler, directory=os.path.abspath(options.site))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", options.port), handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(options.cert, options.key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    print("ready", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
