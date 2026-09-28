"""Exercise HTTP/3 stream and capsule boundaries against a real QUIC server."""

import asyncio
from pathlib import Path
import signal
import socket
import ssl
import subprocess
import tempfile

from aioquic.asyncio import QuicConnectionProtocol, connect
from aioquic.h3.connection import H3Connection
from aioquic.h3.events import DataReceived, DatagramReceived, HeadersReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import ConnectionTerminated

from InteropTest import Client, certificate, wait_log


class ProtocolClient(Client):
    def quic_event_received(self, event):
        if isinstance(event, ConnectionTerminated):
            self.events.put_nowait(event)
        for http_event in self.http.handle_event(event):
            if isinstance(http_event, (HeadersReceived, DatagramReceived)) or (
                isinstance(http_event, DataReceived) and http_event.stream_ended
            ):
                self.events.put_nowait(http_event)


class DelayedSettingsClient(ProtocolClient):
    def __init__(self, *args, **kwargs):
        QuicConnectionProtocol.__init__(self, *args, **kwargs)
        self.events = asyncio.Queue()
        self.pending_settings = []
        send = self._quic.send_stream_data
        self._quic.send_stream_data = lambda *args: self.pending_settings.append(args)
        try:
            self.http = H3Connection(self._quic, enable_webtransport=True)
        finally:
            self._quic.send_stream_data = send

    def release_settings(self):
        for args in self.pending_settings:
            self._quic.send_stream_data(*args)
        self.pending_settings.clear()
        self.transmit()


async def error_case(port, config, send, expected):
    async with connect(
        "127.0.0.1", port, configuration=config, create_protocol=ProtocolClient
    ) as client:
        await send(client)
        event = await asyncio.wait_for(client.events.get(), 5)
        assert isinstance(event, ConnectionTerminated) and event.error_code == expected, event


async def run():
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(dir=root / "build") as temp:
        directory = Path(temp)
        certificate(directory)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        config = QuicConfiguration(
            is_client=True, alpn_protocols=["h3"], max_datagram_frame_size=65536
        )
        config.verify_mode = ssl.CERT_NONE
        log = directory / "server.log"
        with log.open("w") as output:
            process = subprocess.Popen(
                [
                    str(root / "build/wt_echo"),
                    str(port),
                    str(directory / "cert.pem"),
                    str(directory / "key.pem"),
                ],
                stdout=output,
                stderr=output,
            )
            try:
                await wait_log(log, "READY", process)
                async with connect(
                    "127.0.0.1", port, configuration=config, create_protocol=DelayedSettingsClient
                ) as client:
                    stream = client._quic.get_next_available_stream_id()
                    client.http.send_headers(
                        stream,
                        [
                            (b":method", b"CONNECT"),
                            (b":scheme", b"https"),
                            (b":authority", b"localhost"),
                            (b":path", b"/echo"),
                            (b":protocol", b"webtransport"),
                            (b"origin", b"https://localhost"),
                        ],
                    )
                    client.transmit()
                    await asyncio.sleep(0.1)
                    assert client.events.empty(), "CONNECT completed before SETTINGS"
                    client.release_settings()
                    response = await asyncio.wait_for(client.events.get(), 5)
                    assert isinstance(response, HeadersReceived), response
                    assert (b":status", b"200") in response.headers
                print("PASS CONNECT waits for SETTINGS")

                async with connect(
                    "127.0.0.1", port, configuration=config, create_protocol=ProtocolClient
                ) as client:
                    stream, response = await client.open_session(fragmented=True)
                    assert (b":status", b"200") in response.headers
                    await client.echo(stream, b"capsule-before-close")
                    capsule = b"\x80\x00\x28\x43\x04\x00\x00\x00\x00"
                    for byte in capsule:
                        client.http.send_data(stream, bytes([byte]), end_stream=False)
                        client.transmit()
                    event = await asyncio.wait_for(client.events.get(), 5)
                    assert isinstance(event, DataReceived) and event.stream_ended, event
                print("PASS fragmented CLOSE_WEBTRANSPORT_SESSION capsule")

                async def duplicate_settings(client):
                    client._quic.send_stream_data(client.http._local_control_stream_id, b"\x04\x00")
                    client.transmit()

                await error_case(port, config, duplicate_settings, 0x105)
                print("PASS duplicate SETTINGS closes connection")

                async def close_control(client):
                    client._quic.send_stream_data(
                        client.http._local_control_stream_id, b"", end_stream=True
                    )
                    client.transmit()

                await error_case(port, config, close_control, 0x104)
                print("PASS critical control-stream closure")

                async def oversized_headers(client):
                    stream = client._quic.get_next_available_stream_id()
                    client._quic.send_stream_data(stream, b"\x01\x80\x00\x40\x01")
                    client.transmit()

                await error_case(port, config, oversized_headers, 0x107)
                print("PASS HEADERS frame length limit")

                async def oversized_capsule(client):
                    stream, response = await client.open_session()
                    assert (b":status", b"200") in response.headers
                    client.http.send_data(stream, b"\x00\x80\x00\x80\x00", end_stream=False)
                    for _ in range(33):
                        client.http.send_data(stream, b"x" * 1024, end_stream=False)
                    client.transmit()

                await error_case(port, config, oversized_capsule, 0x10E)
                print("PASS capsule receive-buffer limit")
                async with connect(
                    "127.0.0.1", port, configuration=config, create_protocol=ProtocolClient
                ) as client:
                    _, response = await client.open_session(b"/denied")
                    assert isinstance(response, HeadersReceived), response
                    assert (b":status", b"403") in response.headers
            except BaseException:
                print(log.read_text())
                raise
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    process.wait(timeout=10)
            assert process.returncode == 0, log.read_text()


if __name__ == "__main__":
    asyncio.run(run())
