#!/usr/bin/python3
"""python-net-test: checks Python's networking on Vexa's Linux subsystem: an
HTTP server and client over loopback, asyncio, multiprocessing pipes and
selectors. With a URL argument, it also downloads that URL. Prints
"python-net-test: passed" at the end, or what failed."""
import asyncio
import hashlib
import http.server
import multiprocessing
import selectors
import socket
import sys
import threading
import urllib.request

failures = []


def check(name, condition):
    if not condition:
        failures.append(name)
        print("python-net-test: FAILED:", name, flush=True)
    else:
        print("python-net-test:", name, "ok", flush=True)


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = ("you asked for " + self.path).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def child(connection):
    connection.send({"from": "child", "numbers": list(range(5))})
    connection.close()


async def echo(reader, writer):
    data = await reader.read(100)
    writer.write(data.upper())
    await writer.drain()
    writer.close()


async def asyncio_round_trip():
    server = await asyncio.start_server(echo, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    writer.write(b"hello asyncio")
    await writer.drain()
    answer = await reader.read(100)
    writer.close()
    server.close()
    await server.wait_closed()
    return answer


def main():
    # An HTTP server in a thread, and urllib asking it something.
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:%d/page" % server.server_address[1]
    with urllib.request.urlopen(url, timeout=10) as response:
        check("http over loopback", response.read() == b"you asked for /page")
    server.shutdown()

    # asyncio (its event loop wakes itself up through a socket pair).
    check("asyncio streams", asyncio.run(asyncio_round_trip()) == b"HELLO ASYNCIO")

    # multiprocessing pipes are socket pairs.
    parent, other = multiprocessing.Pipe()
    process = multiprocessing.Process(target=child, args=(other,))
    process.start()
    message = parent.recv()
    process.join()
    check("multiprocessing pipe", message == {"from": "child", "numbers": [0, 1, 2, 3, 4]})

    # selectors (epoll) on a UDP socket.
    a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    b.bind(("127.0.0.1", 0))
    selector = selectors.DefaultSelector()
    selector.register(b, selectors.EVENT_READ)
    a.sendto(b"ping", b.getsockname())
    events = selector.select(timeout=5)
    check("selectors", len(events) == 1 and b.recvfrom(16)[0] == b"ping")
    selector.close()
    a.close()
    b.close()

    # A download from outside, if we were given a URL (and its SHA-1).
    if len(sys.argv) > 1:
        with urllib.request.urlopen(sys.argv[1], timeout=30) as response:
            data = response.read()
        digest = hashlib.sha1(data).hexdigest()
        print("python-net-test: downloaded %d bytes, sha1 %s" % (len(data), digest), flush=True)
        check("download", len(sys.argv) < 3 or digest == sys.argv[2])

    if failures:
        print("python-net-test: failed:", ", ".join(failures))
        return 1
    print("python-net-test: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
