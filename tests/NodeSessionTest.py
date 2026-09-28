"""Exercise delayed Node Session event delivery and reentrant listeners."""

import asyncio
from pathlib import Path
import signal
import socket
import ssl
import subprocess
import tempfile

from aioquic.asyncio import connect
from aioquic.h3.events import HeadersReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import ConnectionTerminated

from InteropTest import Client, certificate, wait_log


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
                    "node",
                    "--expose-gc",
                    "tests/node-session-server.js",
                    str(port),
                    str(directory / "cert.pem"),
                    str(directory / "key.pem"),
                ],
                cwd=root,
                stdout=output,
                stderr=output,
            )
            try:
                await wait_log(log, "READY", process)
                async with connect(
                    "127.0.0.1", port, configuration=config, create_protocol=Client
                ) as client:
                    _, rejected = await client.open_session(b"/wrong")
                    assert isinstance(rejected, HeadersReceived)
                    assert (b":status", b"403") in rejected.headers
                    _, rejected = await client.open_session(origin=b"https://wrong.test")
                    assert isinstance(rejected, HeadersReceived)
                    assert (b":status", b"403") in rejected.headers
                    _, rejected = await client.open_session(authority=b"wrong.test")
                    assert isinstance(rejected, HeadersReceived)
                    assert (b":status", b"403") in rejected.headers
                    stream, response = await client.open_session()
                    assert isinstance(response, HeadersReceived)
                    assert (b":status", b"200") in response.headers
                    for payload in (b"one", b"two"):
                        client.http.send_datagram(stream, payload)
                        client.transmit()
                    await asyncio.sleep(0.2)
                    client._quic.send_stream_data(client.http._local_control_stream_id, b"\x04\x00")
                    client.transmit()
                    event = await asyncio.wait_for(client.events.get(), 5)
                    assert isinstance(event, ConnectionTerminated), event
                await asyncio.sleep(0.1)
                process.send_signal(signal.SIGUSR1)
                assert process.wait(timeout=10) == 0, log.read_text()
                assert "PASS delayed polling" in log.read_text(), log.read_text()
                print("PASS Node Session events, retained handle, listener reentrancy")
            except BaseException:
                print(log.read_text())
                raise
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)


if __name__ == "__main__":
    asyncio.run(run())
