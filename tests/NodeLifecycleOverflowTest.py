"""Verify that an exhausted Node lifecycle queue never exposes a partial session."""

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
                    "tests/node-lifecycle-overflow-server.js",
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
                for index in range(37):
                    async with connect(
                        "127.0.0.1", port, configuration=config, create_protocol=Client
                    ) as client:
                        _, response = await client.open_session()
                        if index < 36:
                            assert isinstance(response, HeadersReceived), response
                            assert (b":status", b"200") in response.headers
                        else:
                            assert isinstance(response, (HeadersReceived, ConnectionTerminated))
                    await asyncio.sleep(0.02)
                process.send_signal(signal.SIGTERM)
                assert process.wait(timeout=10) == 0, log.read_text()
                assert "SESSIONS 36 CLOSED 36 TRANSPORT 37/37" in log.read_text()
                print("PASS atomic Node lifecycle reservation")
            except BaseException:
                print(log.read_text())
                raise
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)


if __name__ == "__main__":
    asyncio.run(run())
