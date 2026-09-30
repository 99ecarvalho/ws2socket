#!/usr/bin/env python3
# Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
# SPDX-License-Identifier: LGPL-3.0-or-later
"""End-to-end tests for ws2socket.

Each test starts the real ws2socket binary against a local TCP service and
talks to it with a real WebSocket client.

Usage:
    python3 tests/test_integration.py [path/to/ws2socket] [-k PATTERN]

Requirements: Python 3.8+, the "websockets" package, and the "openssl"
command-line tool (for the TLS tests).
"""

import asyncio
import os
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import urllib.request

try:
    import websockets
    import websockets.exceptions
except ImportError:
    sys.exit("error: the 'websockets' package is required "
             "(pip install websockets, or apt install python3-websockets)")

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BINARY = os.path.join(HERE, "..", "build", "ws2socket")


# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class TcpServer:
    """Threaded TCP server; `handler(conn)` runs once per connection."""

    def __init__(self, handler):
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        self.handler = handler
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self._run, args=(conn,), daemon=True).start()

    def _run(self, conn):
        with conn:
            try:
                self.handler(conn)
            except OSError:
                pass

    def close(self):
        self.sock.close()


def echo_handler(conn):
    while True:
        data = conn.recv(65536)
        if not data:
            return
        conn.sendall(data)


class Proxy:
    """Runs ws2socket as a subprocess for the duration of a `with` block."""

    def __init__(self, binary, args, port=None):
        self.port = port or free_port()
        self.args = [binary, "--listen", f"127.0.0.1:{self.port}"] + args
        self.log = tempfile.TemporaryFile()

    def __enter__(self):
        self.proc = subprocess.Popen(self.args, stdout=self.log, stderr=self.log)
        deadline = time.time() + 5
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise AssertionError(f"ws2socket exited early:\n{self.output()}")
            try:
                socket.create_connection(("127.0.0.1", self.port), 0.2).close()
                return self
            except OSError:
                time.sleep(0.05)
        raise AssertionError("ws2socket did not start listening")

    def __exit__(self, *exc):
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def output(self):
        self.log.seek(0)
        return self.log.read().decode(errors="replace")

    @property
    def url(self):
        return f"ws://127.0.0.1:{self.port}/websockify"


# websockets >= 14 raises InvalidStatus; older versions raise InvalidStatusCode
REJECTED = tuple(getattr(websockets.exceptions, name)
                 for name in ("InvalidStatus", "InvalidStatusCode")
                 if hasattr(websockets.exceptions, name))


def rejected_status(exc):
    response = getattr(exc, "response", None)
    return response.status_code if response is not None else exc.status_code


def run(coro):
    return asyncio.run(asyncio.wait_for(coro, 20))


async def echo_roundtrip(url, payload=b"hello", **kwargs):
    async with websockets.connect(url, **kwargs) as ws:
        await ws.send(payload)
        received = b""
        while len(received) < len(payload):
            received += await ws.recv()
        return received


def raw_handshake(port, path="/websockify", extra=""):
    """Open a WebSocket with a hand-written handshake; return (sock, response)."""
    sock = socket.create_connection(("127.0.0.1", port), 5)
    sock.sendall((f"GET {path} HTTP/1.1\r\nHost: localhost\r\n"
                  "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                  f"Sec-WebSocket-Version: 13\r\n{extra}\r\n").encode())
    response = b""
    while b"\r\n\r\n" not in response:
        chunk = sock.recv(4096)
        if not chunk:
            break
        response += chunk
    return sock, response.decode(errors="replace")


def masked_frame(payload, opcode=0x2, fin=True):
    mask = os.urandom(4)
    header = bytes([(0x80 if fin else 0) | opcode])
    n = len(payload)
    if n < 126:
        header += bytes([0x80 | n])
    elif n < 65536:
        header += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        header += bytes([0x80 | 127]) + struct.pack(">Q", n)
    body = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return header + mask + body


def read_frame(sock):
    def exact(n):
        buf = b""
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("closed")
            buf += chunk
        return buf
    b0, b1 = exact(2)
    n = b1 & 0x7F
    if n == 126:
        n = struct.unpack(">H", exact(2))[0]
    elif n == 127:
        n = struct.unpack(">Q", exact(8))[0]
    return b0 & 0x0F, exact(n)


# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

def test_basic_echo(binary):
    echo = TcpServer(echo_handler)
    with Proxy(binary, ["--target", f"127.0.0.1:{echo.port}"]) as p:
        assert run(echo_roundtrip(p.url)) == b"hello"


def test_log_level_labels(binary):
    echo = TcpServer(echo_handler)
    with Proxy(binary, ["--target", f"127.0.0.1:{echo.port}", "--verbose"]) as p:
        run(echo_roundtrip(p.url))
        time.sleep(0.2)
    out = p.output()
    assert "UNKNOWN" not in out, out
    assert "] INFO: " in out and "] DEBUG: " in out, out


def test_log_level_filtering(binary):
    echo = TcpServer(echo_handler)
    with tempfile.NamedTemporaryFile("w", suffix=".conf") as conf:
        conf.write(f"[proxy]\ntarget = 127.0.0.1:{echo.port}\n"
                   "[logging]\nlevel = warning\n")
        conf.flush()
        with Proxy(binary, ["--config", conf.name]) as p:
            run(echo_roundtrip(p.url))
            time.sleep(0.2)
    out = p.output()
    assert "INFO" not in out and "DEBUG" not in out, out


def test_client_without_compression(binary):
    echo = TcpServer(echo_handler)
    with Proxy(binary, ["--target", f"127.0.0.1:{echo.port}"]) as p:
        assert run(echo_roundtrip(p.url, compression=None)) == b"hello"
        _, response = raw_handshake(p.port)
        assert "101" in response.split("\r\n")[0], response
        assert "Sec-WebSocket-Extensions" not in response, response


def test_compression_negotiated_when_offered(binary):
    echo = TcpServer(echo_handler)
    with Proxy(binary, ["--target", f"127.0.0.1:{echo.port}"]) as p:
        _, response = raw_handshake(
            p.port, extra="Sec-WebSocket-Extensions: permessage-deflate\r\n")
        assert "permessage-deflate" in response, response
        payload = b"compress me " * 500
        assert run(echo_roundtrip(p.url, payload)) == payload


def test_ping_keeps_session_open(binary):
    echo = TcpServer(echo_handler)

    async def scenario(url):
        async with websockets.connect(url) as ws:
            pong = await ws.ping(b"are-you-there")
            await asyncio.wait_for(pong, 5)
            await ws.send(b"after-ping")
            return await ws.recv()

    with Proxy(binary, ["--target", f"127.0.0.1:{echo.port}"]) as p:
        assert run(scenario(p.url)) == b"after-ping"


def test_fragmented_message(binary):
    echo = TcpServer(echo_handler)
    for compression in ("deflate", None):
        with Proxy(binary, ["--target", f"127.0.0.1:{echo.port}"]) as p:
            async def scenario():
                async with websockets.connect(p.url, compression=compression) as ws:
                    await ws.send([b"part1-", b"part2-", b"part3"])
                    got = b""
                    while len(got) < 17:
                        got += await ws.recv()
                    return got
            assert run(scenario()) == b"part1-part2-part3", compression


def test_large_burst_from_target(binary):
    size = 1024 * 1024
    blob = os.urandom(size)

    def burst(conn):
        conn.sendall(blob)
        conn.recv(1)

    target = TcpServer(burst)

    async def scenario(url):
        async with websockets.connect(url, max_size=None) as ws:
            got = b""
            while len(got) < size:
                got += await ws.recv()
            return got

    with Proxy(binary, ["--target", f"127.0.0.1:{target.port}"]) as p:
        assert run(scenario(p.url)) == blob


def test_large_message_from_client(binary):
    echo = TcpServer(echo_handler)
    payload = os.urandom(900 * 1024)  # below the 1 MiB default buffer_size
    with Proxy(binary, ["--target", f"127.0.0.1:{echo.port}"]) as p:
        for compression in (None, "deflate"):
            assert run(echo_roundtrip(p.url, payload, compression=compression,
                                      max_size=None)) == payload, compression


def test_buffer_size_limits_messages(binary):
    echo = TcpServer(echo_handler)
    with tempfile.NamedTemporaryFile("w", suffix=".conf") as conf:
        conf.write(f"[proxy]\ntarget = 127.0.0.1:{echo.port}\nbuffer_size = 1000\n")
        conf.flush()
        with Proxy(binary, ["--config", conf.name]) as p:
            assert run(echo_roundtrip(p.url, b"a" * 900)) == b"a" * 900

            async def too_big():
                async with websockets.connect(p.url) as ws:
                    await ws.send(os.urandom(2000))
                    try:
                        await ws.recv()
                    except websockets.ConnectionClosed as e:
                        return e.rcvd.code if e.rcvd else None
            assert run(too_big()) == 1009


def test_oversized_decompressed_message_is_rejected(binary):
    """A message that inflates beyond buffer_size must not be truncated."""
    echo = TcpServer(echo_handler)
    payload = b"\0" * (4 * 1024 * 1024)  # tiny compressed, 4 MiB inflated

    async def scenario(url):
        async with websockets.connect(url, max_size=None) as ws:
            await ws.send(payload)
            got = b""
            try:
                while len(got) < len(payload):
                    got += await ws.recv()
            except websockets.ConnectionClosed as e:
                return got, e.rcvd.code if e.rcvd else None
            return got, None

    with Proxy(binary, ["--target", f"127.0.0.1:{echo.port}"]) as p:
        got, code = run(scenario(p.url))
        assert got == b"", f"received {len(got)} truncated bytes"
        assert code == 1009, code


def test_split_frame_header(binary):
    """Frame headers that arrive one byte at a time must be reassembled."""
    echo = TcpServer(echo_handler)
    with Proxy(binary, ["--target", f"127.0.0.1:{echo.port}"]) as p:
        sock, response = raw_handshake(p.port)
        assert "101" in response
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for byte in masked_frame(b"x" * 300):
            sock.send(bytes([byte]))
            time.sleep(0.001)
        opcode, data = read_frame(sock)
        assert (opcode, data) == (0x2, b"x" * 300)
        sock.close()


def test_cli_overrides_config_file(binary):
    echo = TcpServer(echo_handler)
    with tempfile.NamedTemporaryFile("w", suffix=".conf") as conf:
        conf.write("[proxy]\ntarget = 127.0.0.1:1\n"
                   "[logging]\nconsole = false\n")
        conf.flush()
        with Proxy(binary, ["--config", conf.name,
                            "--target", f"127.0.0.1:{echo.port}"]) as p:
            assert run(echo_roundtrip(p.url)) == b"hello"
            time.sleep(0.2)
        assert p.output().strip() == "", "console = false was ignored"


def test_invalid_arguments_are_rejected(binary):
    for args in (["--target", "localhost"],
                 ["--target", "host:99999"],
                 ["--listen", "0.0.0.0:abc", "--target", "127.0.0.1:1"],
                 ["--port", "0", "--target", "127.0.0.1:1"]):
        result = subprocess.run([binary] + args, capture_output=True, timeout=5)
        assert result.returncode != 0, f"{args} was accepted"


def test_static_files_and_426(binary):
    echo = TcpServer(echo_handler)
    with tempfile.TemporaryDirectory() as root:
        with open(os.path.join(root, "index.html"), "w") as f:
            f.write("<h1>ok</h1>")
        with Proxy(binary, ["--target", f"127.0.0.1:{echo.port}",
                            "--web-root", root]) as p:
            body = urllib.request.urlopen(
                f"http://127.0.0.1:{p.port}/", timeout=5).read()
            assert body == b"<h1>ok</h1>"
    with Proxy(binary, ["--target", f"127.0.0.1:{echo.port}"]) as p:
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{p.port}/", timeout=5)
            raise AssertionError("expected 426")
        except urllib.error.HTTPError as e:
            assert e.code == 426


def make_cert(directory):
    cert = os.path.join(directory, "cert.pem")
    key = os.path.join(directory, "key.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", key, "-out", cert, "-days", "1",
                    "-subj", "/CN=localhost"],
                   check=True, capture_output=True)
    return cert, key


def test_tls(binary):
    if not shutil.which("openssl"):
        raise SkipTest("openssl command not found")
    echo = TcpServer(echo_handler)
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    with tempfile.TemporaryDirectory() as tmp:
        cert, key = make_cert(tmp)
        with open(os.path.join(tmp, "index.html"), "w") as f:
            f.write("secure")
        with Proxy(binary, ["--target", f"127.0.0.1:{echo.port}",
                            "--cert", cert, "--key", key,
                            "--web-root", tmp]) as p:
            url = f"wss://127.0.0.1:{p.port}/websockify"
            assert run(echo_roundtrip(url, ssl=ctx)) == b"hello"
            payload = os.urandom(300000)
            assert run(echo_roundtrip(url, payload, ssl=ctx,
                                      max_size=None)) == payload
            body = urllib.request.urlopen(
                f"https://127.0.0.1:{p.port}/index.html",
                context=ctx, timeout=5).read()
            assert body == b"secure"


def test_tls_handshake_does_not_block_listener(binary):
    if not shutil.which("openssl"):
        raise SkipTest("openssl command not found")
    echo = TcpServer(echo_handler)
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    with tempfile.TemporaryDirectory() as tmp:
        cert, key = make_cert(tmp)
        with Proxy(binary, ["--target", f"127.0.0.1:{echo.port}",
                            "--cert", cert, "--key", key]) as p:
            idle = socket.create_connection(("127.0.0.1", p.port))  # never speaks TLS
            try:
                url = f"wss://127.0.0.1:{p.port}/"
                assert run(echo_roundtrip(url, ssl=ctx)) == b"hello"
            finally:
                idle.close()


def test_tls_sends_certificate_chain(binary):
    """A client that trusts only the root CA must accept the server."""
    if not shutil.which("openssl"):
        raise SkipTest("openssl command not found")
    echo = TcpServer(echo_handler)
    with tempfile.TemporaryDirectory() as tmp:
        def path(name):
            return os.path.join(tmp, name)

        def openssl(*args):
            subprocess.run(["openssl", *args], check=True, capture_output=True)

        with open(path("ca.ext"), "w") as f:
            f.write("basicConstraints=critical,CA:true\nkeyUsage=keyCertSign\n")
        with open(path("srv.ext"), "w") as f:
            f.write("subjectAltName=DNS:localhost,IP:127.0.0.1\n")
        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                "-keyout", path("root.key"), "-out", path("root.pem"),
                "-subj", "/CN=Test Root")
        for name, issuer, ext, cn in (("int", "root", "ca.ext", "Test Intermediate"),
                                      ("srv", "int", "srv.ext", "localhost")):
            openssl("req", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", path(f"{name}.key"), "-out", path(f"{name}.csr"),
                    "-subj", f"/CN={cn}")
            openssl("x509", "-req", "-days", "1", "-in", path(f"{name}.csr"),
                    "-CA", path(f"{issuer}.pem"), "-CAkey", path(f"{issuer}.key"),
                    "-CAcreateserial", "-extfile", path(ext),
                    "-out", path(f"{name}.pem"))
        with open(path("fullchain.pem"), "w") as out:
            for name in ("srv", "int"):
                with open(path(f"{name}.pem")) as f:
                    out.write(f.read())

        ctx = ssl.create_default_context(cafile=path("root.pem"))
        with Proxy(binary, ["--target", f"127.0.0.1:{echo.port}",
                            "--cert", path("fullchain.pem"),
                            "--key", path("srv.key")]) as p:
            url = f"wss://localhost:{p.port}/websockify"
            assert run(echo_roundtrip(url, ssl=ctx)) == b"hello"


def test_token_routing(binary):
    echo_a = TcpServer(lambda c: (c.sendall(b"A"), echo_handler(c)))
    echo_b = TcpServer(lambda c: (c.sendall(b"B"), echo_handler(c)))

    async def first_byte(url):
        async with websockets.connect(url) as ws:
            return await ws.recv()

    with tempfile.NamedTemporaryFile("w", suffix=".tokens") as tokens:
        tokens.write("# comment\n"
                     f"alpha: 127.0.0.1:{echo_a.port}\n"
                     f"beta: 127.0.0.1:{echo_b.port}\n")
        tokens.flush()
        with Proxy(binary, ["--token-file", tokens.name]) as p:
            base = f"ws://127.0.0.1:{p.port}/websockify"
            assert run(first_byte(base + "?token=alpha")) == b"A"
            assert run(first_byte(base + "?token=beta")) == b"B"
            for bad in ("?token=nope", "", "?mytoken=alpha"):
                try:
                    run(first_byte(base + bad))
                    raise AssertionError(f"{bad!r} was accepted")
                except REJECTED as e:
                    assert rejected_status(e) == 403, bad


def test_max_connections(binary):
    echo = TcpServer(echo_handler)
    with tempfile.NamedTemporaryFile("w", suffix=".conf") as conf:
        conf.write(f"[server]\nmax_connections = 2\n"
                   f"[proxy]\ntarget = 127.0.0.1:{echo.port}\n")
        conf.flush()
        with Proxy(binary, ["--config", conf.name]) as p:
            async def scenario():
                a = await websockets.connect(p.url)
                b = await websockets.connect(p.url)
                try:
                    await websockets.connect(p.url)
                    raise AssertionError("third connection was accepted")
                except REJECTED as e:
                    assert rejected_status(e) == 503
                await a.close()
                await asyncio.sleep(0.5)
                c = await websockets.connect(p.url)
                await c.send(b"again")
                assert await c.recv() == b"again"
                await b.close()
                await c.close()
            run(scenario())


def test_version_and_help(binary):
    out = subprocess.run([binary, "--version"], capture_output=True, text=True)
    assert out.returncode == 0 and "Copyright" in out.stdout
    out = subprocess.run([binary, "--help"], capture_output=True, text=True)
    assert out.returncode == 0 and "--token-file" in out.stdout


# -----------------------------------------------------------------------------
# Runner
# -----------------------------------------------------------------------------

class SkipTest(Exception):
    pass


def main():
    args = sys.argv[1:]
    pattern = None
    if "-k" in args:
        i = args.index("-k")
        pattern = args[i + 1]
        del args[i:i + 2]
    binary = os.path.abspath(args[0] if args else DEFAULT_BINARY)
    if not os.access(binary, os.X_OK):
        sys.exit(f"error: ws2socket binary not found at {binary}")

    tests = [(name, fn) for name, fn in globals().items()
             if name.startswith("test_") and callable(fn)
             and (pattern is None or pattern in name)]
    failed = skipped = 0
    for name, fn in tests:
        try:
            fn(binary)
            print(f"PASS  {name}")
        except SkipTest as e:
            skipped += 1
            print(f"SKIP  {name}: {e}")
        except Exception:
            failed += 1
            print(f"FAIL  {name}")
            traceback.print_exc()
    print(f"\n{len(tests) - failed - skipped} passed, {failed} failed, "
          f"{skipped} skipped")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
