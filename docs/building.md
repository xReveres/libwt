# Build and installation

Run commands from the repository root unless stated otherwise.

## Requirements

| Component                                       | Requirement                                                          |
| ----------------------------------------------- | -------------------------------------------------------------------- |
| Platform                                        | Linux; other platforms are rejected by CMake                         |
| Native toolchain                                | C and C++ compiler with C++17 support, CMake 3.24+                   |
| Presets                                         | CMake 3.25+ because `CMakePresets.json` uses schema version 6        |
| Default build                                   | Make, Git, network access for FetchContent, Perl for bundled OpenSSL |
| Node addon                                      | Node.js with Node-API 8 support and `node_api.h`; CI uses Node.js 22 |
| Python interoperability tests                   | Python 3.12 as used in CI, `aioquic==1.3.0`, `cryptography==46.0.5`  |
| Node unit tests / manual certificate generation | `openssl` executable                                                 |
| Optional browser test                           | Python Playwright package and a Chromium executable                  |

The native library and CTest executables do not require npm or Python packages.
The default dependency build uses MsQuic commit
`a01333cf7c2659cce0ff03ef3f21e1ff15bb5b83` (v2.6.1) and its OpenSSL submodule.

## Build from source

```sh
cmake --preset default
cmake --build --preset default -j4
ctest --preset default
```

The default preset uses Unix Makefiles, Debug mode, and `build/`. Tests and examples
are enabled when libwt is the top-level project. To use CMake 3.24 or a separate
Release build directory, use ordinary CMake commands:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j4
ctest --test-dir build/release --output-on-failure
```

Most Python tests and the default Node loader expect artifacts in `build/`; use
the default layout for the full contributor test suite.

### Node.js addon

```sh
cmake --preset node
cmake --build --preset node -j4
node --test bindings/node/test.js
```

This produces `build/bindings/node/wt_node.node`. The `default` and `node` presets
share the same directory, so reconfiguration toggles the Node target in place.
For a custom build directory, set `LIBWT_NODE_ADDON` to the absolute addon path
before loading `bindings/node`.

## CMake configuration

| Variable                   | Default                            | Meaning                                                              |
| -------------------------- | ---------------------------------- | -------------------------------------------------------------------- |
| `LIBWT_BUILD_TESTS`        | Top-level: `ON`; subproject: `OFF` | Native test executables and CTest registration                       |
| `LIBWT_BUILD_EXAMPLES`     | Top-level: `ON`; subproject: `OFF` | Native echo executable; also built when tests are enabled            |
| `LIBWT_BUILD_NODE`         | `OFF`                              | Build the Node-API addon                                             |
| `LIBWT_USE_SYSTEM_MSQUIC`  | `OFF`                              | Find installed MsQuic headers and shared library                     |
| `LIBWT_MSQUIC_SOURCE_DIR`  | Empty                              | Use an existing MsQuic checkout, including OpenSSL                   |
| `LIBWT_MSQUIC_INCLUDE_DIR` | Discovered in system mode          | Directory containing `msquic.h`                                      |
| `LIBWT_MSQUIC_LIBRARY`     | Discovered in system mode          | Path to the MsQuic shared library                                    |
| `LIBWT_NODE_EXECUTABLE`    | Discovered `node`                  | Node executable used to locate headers                               |
| `LIBWT_NODE_INCLUDE_DIR`   | Discovered                         | Directory containing `node_api.h`                                    |
| `CMAKE_BUILD_TYPE`         | `Debug` in presets                 | Build configuration for single-configuration generators              |
| `CMAKE_INSTALL_PREFIX`     | CMake default                      | Installation prefix; can also be set with `cmake --install --prefix` |

An existing CMake target named `msquic` takes precedence over dependency discovery.
Otherwise system mode takes precedence over a source directory, which takes precedence
over FetchContent. The bundled path defaults to `QUIC_TLS_LIB=openssl`,
`QUIC_BUILD_SHARED=ON`, and disables MsQuic tools, tests, benchmarks, and logging.
Those are dependency cache defaults; changing them can alter the build and install behavior.

### Existing MsQuic checkout

```sh
cmake -S . -B build -DLIBWT_MSQUIC_SOURCE_DIR=/absolute/path/to/msquic
```

Use the pinned revision for reproducibility and initialize its `submodules/openssl`
submodule. The directory option does not download or initialize missing submodules.

### Installed MsQuic

```sh
cmake -S . -B build/system \
  -DLIBWT_USE_SYSTEM_MSQUIC=ON \
  -DLIBWT_MSQUIC_INCLUDE_DIR=/absolute/path/to/msquic/include \
  -DLIBWT_MSQUIC_LIBRARY=/absolute/path/to/libmsquic.so
cmake --build build/system -j4
```

Omit the explicit paths if CMake can find the installation. Compatibility with an
arbitrary system version is not enforced by version discovery; matching headers
and library are the application's responsibility. System MsQuic is not bundled
into libwt's install tree and must remain available to the runtime loader.

## Install and consume from CMake

```sh
cmake --install build --prefix "$PWD/build/install"
cmake -S tests/consumer-installed -B build/consumer-installed \
  -DCMAKE_PREFIX_PATH="$PWD/build/install"
cmake --build build/consumer-installed
build/consumer-installed/consumer
```

Installation exports the shared library, public headers, CMake package files,
the project license (normally `share/libwt/LICENSE`), and third-party attribution.
When libwt builds MsQuic itself, its shared library and
notices are also installed. `wt` uses an `$ORIGIN` install RPATH to locate adjacent
libraries. GNUInstallDirs determines the library directory, commonly `lib` or `lib64`.
The Node addon and examples do not have libwt install rules.

In an application with its own `main.cpp`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(my_server LANGUAGES CXX)
find_package(libwt CONFIG REQUIRED)
add_executable(my_server main.cpp)
target_link_libraries(my_server PRIVATE libwt::wt)
```

Alternatively, place a checkout at `external/libwt` and use:

```cmake
cmake_minimum_required(VERSION 3.24)
project(my_server LANGUAGES C CXX)
add_subdirectory(external/libwt)
add_executable(my_server main.cpp)
target_link_libraries(my_server PRIVATE libwt::wt)
```

The exported target supplies the include path and C++17 requirement. The installed
package version check accepts requests within the same minor version.

## Running the examples

Generate a short-lived local EC certificate with the `openssl` executable:

```sh
mkdir -p build/certs
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
  -subj /CN=localhost -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' \
  -days 7 -keyout build/certs/key.pem -out build/certs/cert.pem
```

Start either server (both bind to `127.0.0.1`, accept `/echo`, and print `READY`):

```sh
./build/wt_echo 4433 build/certs/cert.pem build/certs/key.pem
```

```sh
node examples/node-echo.js 4433 build/certs/cert.pem build/certs/key.pem
```

SIGINT/SIGTERM stop the examples. SIGHUP reloads the certificate and key from their
original paths. The examples are also test fixtures: **exit code 4 can be expected
after a manual run** because shutdown checks require both accepted and rejected
requests and other test traffic. Use the [minimal C++ program](api.md#minimal-echo-server)
or [Node example](node.md#example) as a starting point for an application.

To use `examples/browser-datagram.html`, obtain the certificate fingerprint:

```sh
openssl x509 -in build/certs/cert.pem -noout -fingerprint -sha256
python3 -m http.server 8000 --bind 127.0.0.1 --directory examples
```

Open `http://127.0.0.1:8000/browser-datagram.html` in a browser that exposes
WebTransport and `serverCertificateHashes`. Paste the fingerprint's hex value
(colons are accepted, the `sha256 Fingerprint=` label is not), set the server port,
and click **Connect and echo**. The client always connects to `127.0.0.1` at `/echo`.
The HTTP file server serves the page; libwt serves the separate QUIC endpoint.
Browser compatibility must be checked with the actual browser version being used.
