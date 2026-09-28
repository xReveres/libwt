"""Exercise isolated C++ session counters with real aioquic connections."""

import asyncio
import argparse
from pathlib import Path
import signal
import socket
import ssl
import subprocess
import tempfile

from aioquic.asyncio import connect
from aioquic.h3.events import DatagramReceived, HeadersReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import ConnectionTerminated

from InteropTest import Client, certificate, wait_log


async def run(server_path=None):
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
                    str(server_path or root / "build/wt_statistics_server"),
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
                    "127.0.0.1", port, configuration=config, create_protocol=Client
                ) as first:
                    _, response = await first.open_session(b"/reject")
                    assert isinstance(response, HeadersReceived)
                    assert (b":status", b"403") in response.headers
                    _, response = await first.open_session(b"/throw-request")
                    assert isinstance(response, HeadersReceived)
                    assert (b":status", b"403") in response.headers
                    first_stream, response = await first.open_session(b"/first")
                    assert isinstance(response, HeadersReceived)
                    assert (b":status", b"200") in response.headers
                    async with connect(
                        "127.0.0.1", port, configuration=config, create_protocol=Client
                    ) as second:
                        second_stream, response = await second.open_session(b"/second")
                        assert isinstance(response, HeadersReceived)
                        assert (b":status", b"200") in response.headers
                        await asyncio.gather(
                            first.echo(first_stream, b"alpha"),
                            second.echo(second_stream, b"bravo"),
                        )
                        first.http.send_datagram(first_stream, b"burst")
                        first.transmit()
                        second.http.send_datagram(second_stream, b"throw")
                        second.transmit()
                        await asyncio.wait_for(first.events.get(), 5)
                        event = await asyncio.wait_for(second.events.get(), 5)
                        assert isinstance(event, ConnectionTerminated), event
                async with connect(
                    "127.0.0.1", port, configuration=config, create_protocol=Client
                ) as admission:
                    _, event = await admission.open_session(b"/close-true")
                    assert isinstance(event, (HeadersReceived, ConnectionTerminated)), event
                async with connect(
                    "127.0.0.1", port, configuration=config, create_protocol=Client
                ) as admission:
                    _, event = await admission.open_session(b"/close-false")
                    assert isinstance(event, HeadersReceived), event
                    assert (b":status", b"403") in event.headers
                async with connect(
                    "127.0.0.1", port, configuration=config, create_protocol=Client
                ) as throwing:
                    _, event = await throwing.open_session(b"/throw-session")
                    if isinstance(event, HeadersReceived):
                        assert (b":status", b"200") in event.headers
                        event = await asyncio.wait_for(throwing.events.get(), 5)
                    assert isinstance(event, ConnectionTerminated), event
                async with connect(
                    "127.0.0.1", port, configuration=config, create_protocol=Client
                ) as no_handler:
                    stream, event = await no_handler.open_session(b"/none")
                    assert isinstance(event, HeadersReceived), event
                    no_handler.http.send_datagram(stream, b"silent")
                    no_handler.transmit()
                    try:
                        await asyncio.wait_for(no_handler.events.get(), 0.3)
                        raise AssertionError("session without handler delivered a datagram")
                    except TimeoutError:
                        pass
                async with connect(
                    "127.0.0.1", port, configuration=config, create_protocol=Client
                ) as replace:
                    stream, event = await replace.open_session(b"/replace")
                    if isinstance(event, DatagramReceived):
                        assert event.data == b"ready", event
                        event = await asyncio.wait_for(replace.events.get(), 5)
                    else:
                        ready = await asyncio.wait_for(replace.events.get(), 5)
                        assert isinstance(ready, DatagramReceived) and ready.data == b"ready", ready
                    assert isinstance(event, HeadersReceived), event
                    assert (b":status", b"200") in event.headers
                    await replace.echo(stream, b"hello")
                async with connect(
                    "127.0.0.1", port, configuration=config, create_protocol=Client
                ) as malformed:
                    malformed._quic.send_stream_data(
                        malformed.http._local_control_stream_id, b"\x04\x00"
                    )
                    malformed.transmit()
                    event = await asyncio.wait_for(malformed.events.get(), 5)
                    assert isinstance(event, ConnectionTerminated) and event.error_code == 0x105, (
                        event
                    )
                process.send_signal(signal.SIGTERM)
                assert process.wait(timeout=10) == 0, log.read_text()
                assert "PASS session statistics" in log.read_text(), log.read_text()
                missing_log = directory / "missing-request.log"
                with missing_log.open("w") as missing_output:
                    missing = subprocess.Popen(
                        [
                            str(server_path or root / "build/wt_statistics_server"),
                            str(port),
                            str(directory / "cert.pem"),
                            str(directory / "key.pem"),
                            "no-request",
                        ],
                        stdout=missing_output,
                        stderr=missing_output,
                    )
                    try:
                        await wait_log(missing_log, "READY", missing)
                        async with connect(
                            "127.0.0.1", port, configuration=config, create_protocol=Client
                        ) as client:
                            _, response = await client.open_session(b"/first")
                            assert isinstance(response, HeadersReceived), response
                            assert (b":status", b"403") in response.headers
                        missing.send_signal(signal.SIGTERM)
                        assert missing.wait(timeout=10) == 0, missing_log.read_text()
                    finally:
                        if missing.poll() is None:
                            missing.terminate()
                            missing.wait(timeout=5)
                print("PASS session API, isolation, send outcomes, restart")
            except BaseException:
                print(log.read_text())
                raise
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server")
    args = parser.parse_args()
    asyncio.run(run(args.server))
