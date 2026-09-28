"""Verify bounded Node datagram bridge drops excess traffic without losing lifecycle events."""

import asyncio
import socket
import ssl
import subprocess
import tempfile
from pathlib import Path

from aioquic.asyncio import connect
from aioquic.h3.events import HeadersReceived
from aioquic.quic.configuration import QuicConfiguration
from InteropTest import Client, certificate, wait_log


async def run():
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(dir=root / "build") as temp:
        directory = Path(temp)
        certificate(directory)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        log = directory / "server.log"
        with log.open("w") as output:
            process = subprocess.Popen(
                [
                    "node",
                    "tests/node-overflow-server.js",
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
                config = QuicConfiguration(
                    is_client=True, alpn_protocols=["h3"], max_datagram_frame_size=65536
                )
                config.verify_mode = ssl.CERT_NONE
                async with connect(
                    "127.0.0.1", port, configuration=config, create_protocol=Client
                ) as client:
                    stream, response = await client.open_session()
                    assert (
                        isinstance(response, HeadersReceived)
                        and (b":status", b"200") in response.headers
                    )
                    for number in range(300):
                        client.http.send_datagram(stream, str(number).encode())
                        client.transmit()
                    await asyncio.sleep(0.2)
                    async with connect(
                        "127.0.0.1", port, configuration=config, create_protocol=Client
                    ) as second:
                        other_stream, response = await second.open_session()
                        assert isinstance(response, HeadersReceived)
                        assert (b":status", b"200") in response.headers
                        second.http.send_datagram(other_stream, b"second")
                        second.transmit()
                        await asyncio.sleep(0.2)
                process.terminate()
                process.wait(timeout=5)
                assert process.returncode == 0 and "SESSIONS 2 CLOSED 2" in log.read_text(), (
                    log.read_text()
                )
                print("PASS Node bridge overflow with bounded queue")
            except BaseException:
                print(log.read_text())
                raise
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)


if __name__ == "__main__":
    asyncio.run(run())
