"""Local QUIC/HTTP3 interoperability, rotation and Chromium datagram smoke tests.

Test-only dependencies: aioquic, cryptography, playwright. No public CA/network.
"""

import argparse
import asyncio
import hashlib
import ipaddress
import os
from pathlib import Path
import signal
import shlex
import socket
import ssl
import subprocess
import tempfile
from datetime import datetime, timedelta, timezone

from aioquic.asyncio import connect, QuicConnectionProtocol
from aioquic.h3.connection import H3Connection
from aioquic.h3.events import HeadersReceived, DatagramReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import ConnectionTerminated, StreamReset
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID


def certificate(directory):
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    now = datetime.now(timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - timedelta(minutes=1))
        .not_valid_after(now + timedelta(days=7))
        .add_extension(
            x509.SubjectAlternativeName(
                [x509.DNSName("localhost"), x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]
            ),
            critical=False,
        )
        .sign(key, hashes.SHA256())
    )
    (directory / "key.pem").write_bytes(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
    )
    (directory / "cert.pem").write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    return cert


class Client(QuicConnectionProtocol):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.http = H3Connection(self._quic, enable_webtransport=True)
        self.events = asyncio.Queue()

    def quic_event_received(self, event):
        if isinstance(event, (ConnectionTerminated, StreamReset)):
            self.events.put_nowait(event)
        for http_event in self.http.handle_event(event):
            if isinstance(http_event, (HeadersReceived, DatagramReceived)):
                self.events.put_nowait(http_event)

    async def open_session(
        self,
        path=b"/echo",
        fragmented=False,
        origin=b"https://localhost",
        authority=b"localhost",
    ):
        stream = self._quic.get_next_available_stream_id()
        headers = [
            (b":method", b"CONNECT"),
            (b":scheme", b"https"),
            (b":authority", authority),
            (b":path", path),
            (b":protocol", b"webtransport"),
        ]
        if origin is not None:
            headers.append((b"origin", origin))
        if fragmented:
            captured = []
            original = self._quic.send_stream_data
            self._quic.send_stream_data = lambda stream_id, data, end_stream=False: captured.append(
                (stream_id, data, end_stream)
            )
            try:
                self.http.send_headers(stream, headers)
            finally:
                self._quic.send_stream_data = original
            for stream_id, data, end_stream in captured:
                for i in range(len(data)):
                    original(stream_id, data[i : i + 1], end_stream and i == len(data) - 1)
                    self.transmit()
                    await asyncio.sleep(0.002)
        else:
            self.http.send_headers(stream, headers)
        self.transmit()
        return stream, await asyncio.wait_for(self.events.get(), 5)

    async def echo(self, stream, payload):
        self.http.send_datagram(stream, payload)
        self.transmit()
        event = await asyncio.wait_for(self.events.get(), 5)
        assert isinstance(event, DatagramReceived) and event.data == payload, event


async def wait_log(path, token, process):
    for _ in range(250):
        if token in path.read_text():
            return
        if process.poll() is not None:
            raise AssertionError(path.read_text())
        await asyncio.sleep(0.02)
    raise AssertionError(f"Timeout waiting for {token}: {path.read_text()}")


async def chromium_test(port, cert, executable):
    from playwright.async_api import async_playwright
    from http.server import BaseHTTPRequestHandler, HTTPServer
    import threading

    class Page(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(b"<!doctype html><title>libwt local test</title>")

        def log_message(self, *args):
            pass

    origin = HTTPServer(("127.0.0.1", 0), Page)
    worker = threading.Thread(target=origin.serve_forever, daemon=True)
    worker.start()
    try:
        async with async_playwright() as playwright:
            browser = await playwright.chromium.launch(
                executable_path=executable, args=["--no-sandbox"]
            )
            try:
                page = await browser.new_page()
                await page.goto(f"http://127.0.0.1:{origin.server_port}")
                result = await page.evaluate(
                    """async ({port, hash}) => {
                    const transport = new WebTransport(`https://127.0.0.1:${port}/echo`, {
                        serverCertificateHashes: [{algorithm: 'sha-256', value: new Uint8Array(hash)}]
                    });
                    transport.closed.catch(() => {});
                    await Promise.race([transport.ready, new Promise((_, reject) => setTimeout(() => reject(new Error('ready timeout')), 8000))]);
                    const writer = transport.datagrams.writable.getWriter();
                    const reader = transport.datagrams.readable.getReader();
                    await writer.write(new TextEncoder().encode('chromium-libwt'));
                    const {value} = await Promise.race([reader.read(), new Promise((_, reject) => setTimeout(() => reject(new Error('datagram timeout')), 5000))]);
                    writer.releaseLock(); reader.releaseLock(); transport.close();
                    return new TextDecoder().decode(value);
                }""",
                    {"port": port, "hash": list(cert.fingerprint(hashes.SHA256()))},
                )
                assert result == "chromium-libwt", result
                print("PASS Chromium WebTransport handshake and datagram echo")
            finally:
                await browser.close()
    finally:
        origin.shutdown()
        origin.server_close()
        worker.join()


async def run(args):
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(dir=root / "build") as temp:
        directory = Path(temp)
        first = certificate(directory)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        log = directory / "server.log"
        with log.open("w") as output:
            process = subprocess.Popen(
                (shlex.split(args.server) if args.server else [str(root / "build/wt_echo")])
                + [str(port), str(directory / "cert.pem"), str(directory / "key.pem")],
                stdout=output,
                stderr=output,
            )
            try:
                await wait_log(log, "READY", process)
                config = QuicConfiguration(
                    is_client=True, alpn_protocols=["h3"], max_datagram_frame_size=65536
                )
                config.verify_mode = (
                    ssl.CERT_NONE
                )  # Test certificates only; never production configuration.
                async with connect(
                    "127.0.0.1", port, configuration=config, create_protocol=Client
                ) as fragmented:
                    stream, response = await fragmented.open_session(fragmented=True)
                    assert (
                        isinstance(response, HeadersReceived)
                        and (b":status", b"200") in response.headers
                    ), response
                    await fragmented.echo(stream, b"fragmented-headers")
                    # A second SETTINGS on the critical stream is a connection error.
                    fragmented._quic.send_stream_data(
                        fragmented.http._local_control_stream_id, b"\x04\x00"
                    )
                    fragmented.transmit()
                    event = await asyncio.wait_for(fragmented.events.get(), 5)
                    assert isinstance(event, ConnectionTerminated) and event.error_code == 0x105, (
                        event
                    )
                print("PASS byte-fragmented HEADERS and duplicate SETTINGS rejection")

                async def parallel_client(index):
                    async with connect(
                        "127.0.0.1", port, configuration=config, create_protocol=Client
                    ) as client:
                        stream, response = await client.open_session()
                        assert (
                            isinstance(response, HeadersReceived)
                            and (b":status", b"200") in response.headers
                        ), response
                        for packet in range(10):
                            await client.echo(stream, f"client-{index}-packet-{packet}".encode())

                await asyncio.gather(*(parallel_client(index) for index in range(8)))
                print("PASS 8 simultaneous connections / 80 isolated datagrams")
                async with connect(
                    "127.0.0.1", port, configuration=config, create_protocol=Client
                ) as client:
                    stream, response = await client.open_session()
                    assert (
                        isinstance(response, HeadersReceived)
                        and (b":status", b"200") in response.headers
                    ), response
                    await client.echo(stream, b"first")
                    _, rejected = await client.open_session()
                    assert isinstance(rejected, StreamReset) and rejected.error_code == 0x10B, (
                        rejected
                    )
                    await client.echo(stream, b"after-second-session-rejected")
                    print("PASS HTTP/3 CONNECT, echo, one-session limit")
                    second = certificate(directory)
                    process.send_signal(signal.SIGHUP)
                    await wait_log(log, "RELOADED", process)
                    await client.echo(stream, b"alive-after-reload")
                    async with connect(
                        "127.0.0.1", port, configuration=config, create_protocol=Client
                    ) as fresh:
                        assert (
                            fresh._quic.tls._peer_certificate.serial_number == second.serial_number
                        )
                        new_stream, response = await fresh.open_session()
                        assert (b":status", b"200") in response.headers
                        await fresh.echo(new_stream, b"new-certificate")
                    print(
                        "PASS certificate rotation: old session alive, new handshake uses new certificate"
                    )
                    good_key = (directory / "key.pem").read_bytes()
                    (directory / "key.pem").write_text("invalid key")
                    process.send_signal(signal.SIGHUP)
                    await wait_log(log, "RELOAD_FAILED", process)
                    await client.echo(stream, b"alive-after-failed-reload")
                    async with connect(
                        "127.0.0.1", port, configuration=config, create_protocol=Client
                    ) as fresh:
                        assert (
                            fresh._quic.tls._peer_certificate.serial_number == second.serial_number
                        )
                        _, response = await fresh.open_session(b"/denied")
                        assert (
                            isinstance(response, HeadersReceived)
                            and (b":status", b"403") in response.headers
                        ), response
                    (directory / "key.pem").write_bytes(good_key)
                    print("PASS failed reload rollback and request rejection")
                if args.chromium:
                    await chromium_test(port, second, args.chromium)
                async with connect(
                    "127.0.0.1", port, configuration=config, create_protocol=Client
                ) as active:
                    stream, response = await active.open_session()
                    assert (
                        isinstance(response, HeadersReceived)
                        and (b":status", b"200") in response.headers
                    ), response
                    await active.echo(stream, b"before-server-stop")
                    process.terminate()
                    event = await asyncio.wait_for(active.events.get(), 5)
                    assert isinstance(event, ConnectionTerminated), event
                print("PASS Stop with an active session")
            except BaseException:
                print(log.read_text())
                raise
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    raise AssertionError("Server Stop did not complete")
            assert process.returncode == 0, (process.returncode, log.read_text())
            if not args.server or Path(shlex.split(args.server)[0]).name != "node":
                output = log.read_text()
                assert "SAMPLE available=1 received=1 oversize=1" in output, output
                assert (
                    "STATS received=" in output
                    and "active_sessions=0 active_connections=0" in output
                ), output
            else:
                output = log.read_text()
                assert "SAMPLE available=true session=1" in output, output
                assert "STATS received=" in output and " active=0" in output, output
            print("PASS server shutdown")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--chromium")
    parser.add_argument("--server")
    asyncio.run(run(parser.parse_args()))
