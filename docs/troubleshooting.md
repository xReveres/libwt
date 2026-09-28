# Troubleshooting

## Configure and build

**CMake rejects the presets.** The project minimum is CMake 3.24, but preset schema
version 6 needs CMake 3.25+. Upgrade CMake or use the explicit `cmake -S . -B build`
commands in [building.md](building.md#build-from-source).

**Configure spends a long time fetching dependencies or fails during Git operations.**
The default path downloads MsQuic and OpenSSL. Check network access and Git errors.
For an existing checkout use `LIBWT_MSQUIC_SOURCE_DIR`; for an installed dependency
use `LIBWT_USE_SYSTEM_MSQUIC=ON`. A source checkout must include its OpenSSL submodule.

**Bundled OpenSSL configuration fails.** Confirm that the C/C++ toolchain, Make,
and Perl are available. Read the dependency error above CMake's final summary.
The bundled path builds OpenSSL from source; installing the OpenSSL CLI alone is
not a replacement for that source build.

**`Node-API headers not found`.** Install Node.js development headers or set
`LIBWT_NODE_INCLUDE_DIR` to the directory containing `node_api.h`. CMake searches
near the Node executable and in `/usr/include/node` and `/usr/local/include/node`.

**Node cannot find `wt_node.node`.** Build with the `node` preset. If using a different
build directory, set `LIBWT_NODE_ADDON` to the absolute addon path. `npm ci` installs
formatting dependencies; it does not build the native addon.

**The runtime loader cannot find `libwt.so` or `libmsquic.so`.** Keep the installed
library layout intact and ensure a system MsQuic installation is available to the
loader when using system mode. Inspect dependencies with `ldd` on the executable
or addon. CMake's package discovery path (`CMAKE_PREFIX_PATH`) and Node's addon path
do not configure the operating system's shared-library search path.

## Startup and TLS

**`Certificate, private key and a positive connection limit are required`.** Provide
both credential paths and a nonzero native connection limit. Creating a server
object does not validate that it can start.

**`Invalid bind address`.** Use a numeric address such as `127.0.0.1`, `0.0.0.0`,
or `::1`. The default listener binds only to loopback.

**`MsQuic TLS configuration failed: ...`.** Check that the PEM files exist, are
readable, and contain a matching certificate and private key. The numeric suffix
is the status returned by MsQuic. Register the log callback/event and retain the
startup error message when diagnosing failures.

**`MsQuic listener failed: ...`.** Check for another process using the same UDP
address/port, an unavailable local address, or insufficient privileges for the port.
Remember that QUIC traffic uses UDP.

**Certificate reload fails.** Rotation requires a running server and valid new
credentials. A failed reload preserves the previous configuration. A successful
reload affects new handshakes; it does not replace credentials on live sessions.

## Admission and client compatibility

**CONNECT returns 403.** Native applications must set `on_request` and return true
for allowed requests. The Node binding compares path, origin, and authority exactly.
Its default path is `/echo`; empty origin and authority options disable those filters.

**CONNECT returns 400 or the connection closes during protocol negotiation.** The
peer must negotiate HTTP datagrams and a supported WebTransport setting. Inspect
the protocol log and verify that the client matches the implementation's
[protocol scope](architecture.md#protocol-scope-and-limits). Malformed or unsupported
headers can close the connection instead of producing an HTTP response.

**A second session is reset.** Only one accepted session is supported per QUIC
connection, even if that session has already ended. Use a new connection.

**The browser example cannot connect.** Start the echo server first, use the correct
port, and paste the current certificate's SHA-256 fingerprint without the output
label. Regenerating the certificate changes its hash. Use the local page-serving
instructions and check that the browser exposes WebTransport and certificate-hash
support. The ordinary aioquic test does not test the browser API.

## Datagrams and callbacks

**Send returns `Size` / `'size'`.** The payload plus its HTTP/3 identifier must fit
the negotiated datagram limit. The 65,535-byte internal ceiling is not the usable
network payload size. Reduce the payload; there is no built-in fragmentation API.

**Send returns `Queue` / `'queue'`.** Native outstanding sends have a fixed limit
of 64 per connection, shared with control sends. Apply application pacing or retry
later if appropriate. Do not spin in a callback waiting for capacity.

**Send returns `State` / `'state'`.** The session may be closing or closed, or
datagram sending may be unavailable. Use session close notifications to stop
producing data. A previously accepted send still has no delivery guarantee.

**Node receives fewer events than native receive counters suggest.** Check
`bridge_dropped_datagrams`, event-loop blocking, and `maxPendingDatagrams`.
Native counters count valid incoming datagrams before bridge delivery. Logs have
their own bounded queue and `bridge_dropped_logs` counter.

**Node throws while serializing statistics.** Counters and transport integer fields
are `bigint`. Supply a JSON replacer that converts them to strings; see the
[Node API](node.md#session).

**The Node process does not exit.** A started server owns a referenced interval.
Call `stop()` during application shutdown. Listener exceptions propagate through
`poll()` and can also disrupt application cleanup.

**Native shutdown hangs.** Never call `Stop()` or destroy the server from a native
callback. Avoid waiting in callbacks for an application thread that is itself
waiting for `Stop()`. Keep captured state alive until shutdown completes.

## Tests and tooling

**An echo example exits with code 4 after working.** Both example servers contain
integration-test assertions, including a requirement that at least one request
was rejected. A manual echo alone does not satisfy them. Use the standalone usage
snippets for ordinary application startup and shutdown.

**Python reports missing `aioquic` or `cryptography`.** Activate the test environment
and install the pinned dependencies in [development.md](development.md#setup).
The tests are invoked separately from CTest.

**Creating a virtual environment fails with missing `ensurepip`.** Install the
distribution's Python venv package. On systems where that is unavailable, an
isolated target directory can also be used:

```sh
python3 -m pip install --target build/python aioquic==1.3.0 cryptography==46.0.5
PYTHONPATH="$PWD/build/python" python3 tests/InteropTest.py
```

**A formatter is missing.** Install `clang-format-20`, run `npm ci`, and install
`requirements-format.txt` in the active Python environment. The check invokes all
four formatting tools, even for a documentation-focused change.
