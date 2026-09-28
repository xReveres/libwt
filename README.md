# libwt

A C++17 WebTransport datagram server library for Linux, built on MsQuic, with
optional Node.js bindings and TypeScript declarations.

libwt accepts WebTransport sessions over QUIC/HTTP/3 and exchanges unreliable
datagrams with clients. Applications supply TLS credentials and admission rules,
then handle each accepted session through the C++ callback API or Node.js events.

## Features

- Datagram send and receive with copied outgoing payloads and explicit rejection reasons.
- Request metadata and admission checks before a session is exposed to the application.
- One accepted WebTransport session per QUIC connection.
- TLS certificate rotation for new connections while existing sessions remain open.
- Server and session counters, live transport statistics, and structured error logs.
- Bounded native send queues and bounded Node.js event queues.
- CMake integration through `libwt::wt`, including installation and package discovery.
- Native, Node.js, and browser examples; protocol and interoperability tests.

The implementation is **datagram-only and server-only**. It does not expose
WebTransport streams or provide a general HTTP/3 web server. Linux is the only
platform accepted by the build. The protocol implementation includes draft-02
and draft-07 WebTransport negotiation; see [architecture and limits](docs/architecture.md).

## Quick start

From a checkout, on Linux with a C/C++17 compiler, CMake 3.25+, Make, Git, and Perl:

```sh
cmake --preset default
cmake --build --preset default -j4
ctest --preset default
```

The first configure downloads the pinned MsQuic source and its OpenSSL submodule;
the first build compiles them. Node.js and Python are not required for the native
library and CTest suite. See [build and installation](docs/building.md) for dependency
alternatives, CMake 3.24 commands, installation, and all build options.

To build the optional Node.js addon (Node.js 22 is used by CI; development headers
are required):

```sh
cmake --preset node
cmake --build --preset node -j4
node --test bindings/node/test.js
```

Both presets use `build/`. The Node test command also needs the `openssl` executable.

## Usage

Link C++ applications to `libwt::wt` and include `<libwt/Server.hpp>`. Configure
`on_request` to admit sessions and install a datagram handler in `on_session`:

```cpp
libwt::ServerConfig config;
config.certificate_file = "cert.pem";
config.private_key_file = "key.pem";
config.on_request = [](const libwt::Request &request) {
    return request.path == "/echo";
};
config.on_session = [](const libwt::SessionPtr &session) {
    session->SetDatagramHandler([](libwt::Session &active, const uint8_t *data, size_t size) {
        active.SendDatagram(data, size);
    });
};
```

This is the configuration portion of an echo server. The [C++ API guide](docs/api.md)
provides a complete program, lifecycle rules, and return-value semantics.

The local Node.js binding exposes `Server` and `Session`:

```js
const { Server } = require('./bindings/node');
const server = new Server({
  certificateFile: 'cert.pem',
  privateKeyFile: 'key.pem',
  path: '/echo',
});
server.on('session', (session) => {
  session.on('datagram', (data) => session.send(data));
});
server.start();
process.on('SIGINT', () => server.stop());
process.on('SIGTERM', () => server.stop());
```

See [Node.js API](docs/node.md) for configuration and events, and
[running the examples](docs/building.md#running-the-examples) for local certificates
and the browser client. A successful send means queued, not delivered.

## Documentation

| Guide                                      | Contents                                                |
| ------------------------------------------ | ------------------------------------------------------- |
| [Build and installation](docs/building.md) | Requirements, CMake options, consumers, examples        |
| [Architecture](docs/architecture.md)       | Directory map, components, ownership, protocol limits   |
| [C++ API](docs/api.md)                     | Configuration, sessions, callbacks, statistics          |
| [Node.js API](docs/node.md)                | JavaScript/TypeScript API, queues, environment variable |
| [Development](docs/development.md)         | Test suites, formatting, contribution workflow          |
| [Troubleshooting](docs/troubleshooting.md) | Build, TLS, admission, datagrams, shutdown              |

## Roadmap

These are follow-up areas visible in the current repository, not promised releases:

- Add browser interoperability to CI; the Chromium test currently requires an explicit executable.
- Define a distributable Node.js package and native binary installation layout; the current package is private and loads an addon from the build tree.
- Establish compatibility coverage beyond the pinned MsQuic dependency and current Linux CI environment.

WebTransport streams, multiple sessions on one connection, and other operating
systems would require implementation work and are outside the current API.

## Contributing

Use focused pull requests with a description of the behavior changed and the checks
run. Preserve API lifetime guarantees and add regression coverage for protocol,
lifecycle, or queue changes. See the [contributor guide](docs/development.md).

### Code style

The repository uses clang-format 20, Ruff, Prettier, and cmake-format. Install the
[formatting dependencies](docs/development.md#code-style), then run:

```sh
npm run format:check
```

Vendored dependency attribution and existing license notices are documented in
[third_party/README.md](third_party/README.md).
