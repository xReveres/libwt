# Development and contributing

Use a Linux environment. CI uses Node.js 22 and Python 3.12; the native toolchain
requirements are listed in [building.md](building.md). Keep generated artifacts,
local certificates, and Python environments under the ignored `build/` directory.

## Setup

```sh
cmake --preset node
cmake --build --preset node -j4
python3 -m venv build/venv
. build/venv/bin/activate
python -m pip install aioquic==1.3.0 cryptography==46.0.5
python -m pip install -r requirements-format.txt
npm ci
```

Install clang-format 20 separately; the formatting script requires the executable
name `clang-format-20`. Node unit tests use the `openssl` command-line tool.
On systems missing Python's venv support, install the distribution's Python venv
package before creating the environment.

## Test suites

The three CTest entries do not launch a browser or run the Python interoperability
tests:

```sh
ctest --preset default
```

| CTest entry           | Coverage                                                            |
| --------------------- | ------------------------------------------------------------------- |
| `protocol`            | Variable integers, settings, QPACK headers and malformed input      |
| `lifecycle`           | Startup validation, repeated stop, logging, callback exceptions     |
| `publication_failure` | A failed HTTP 200 send must not publish a session or close callback |

Run the integration suite explicitly, with the Python environment activated:

```sh
python3 tests/InteropTest.py
python3 tests/SessionStatisticsTest.py
python3 tests/ProtocolRuntimeTest.py
node --test bindings/node/test.js
python3 tests/InteropTest.py --server 'node examples/node-echo.js'
python3 tests/NodeSessionTest.py
python3 tests/NodeOverflowTest.py
python3 tests/NodeLifecycleOverflowTest.py
```

| Test                           | Coverage                                                                                                                         |
| ------------------------------ | -------------------------------------------------------------------------------------------------------------------------------- |
| `InteropTest.py`               | Fragmented headers, concurrent connections, datagram echo, one-session limit, certificate rotation and rollback, active shutdown |
| `SessionStatisticsTest.py`     | Admission, session isolation, counters, retained handles, callback failures and restart                                          |
| `ProtocolRuntimeTest.py`       | SETTINGS ordering, fragmented close capsules, critical streams and input limits                                                  |
| `bindings/node/test.js`        | Configuration validation, lifecycle, timers, bounded logging and listener exceptions                                             |
| `NodeSessionTest.py`           | Session events, metadata, event ordering, retained handles, reentrancy and listener failures                                     |
| `NodeOverflowTest.py`          | Bounded bridge datagram queue and continued session/close delivery                                                               |
| `NodeLifecycleOverflowTest.py` | Atomic reservation of session and close events under lifecycle pressure                                                          |

Python clients generate temporary certificates and use local UDP endpoints. Most
scripts expect binaries in `build/`. `InteropTest.py --server 'COMMAND'` overrides
the server launch command and appends port, certificate, and key arguments.
`SessionStatisticsTest.py --server /path/to/wt_statistics_server` accepts a server
executable override. Some Node fixtures use fixed ports; run integration tests
sequentially to avoid listener conflicts.

### Optional Chromium test

```sh
python -m pip install playwright
python3 tests/InteropTest.py --chromium /absolute/path/to/chromium
```

The script launches the specified executable through Playwright and checks a
WebTransport datagram echo using a certificate hash. The browser executable must
already be installed. The helper launches Chromium with `--no-sandbox`; use it in
an appropriate test environment. CI does not currently enable this branch, so
passing the ordinary Python suite does not establish browser compatibility.

### CMake consumers and installation

Check installed-package consumption after native changes:

```sh
cmake --install build --prefix "$PWD/build/install"
cmake -S tests/consumer-installed -B build/consumer-installed \
  -DCMAKE_PREFIX_PATH="$PWD/build/install"
cmake --build build/consumer-installed
build/consumer-installed/consumer
```

Check package discovery from a copied prefix:

```sh
cmake -E copy_directory build/install build/relocated
cmake -S tests/consumer-installed -B build/consumer-relocated \
  -DCMAKE_PREFIX_PATH="$PWD/build/relocated"
cmake --build build/consumer-relocated
build/consumer-relocated/consumer
```

The CI copy test leaves the original installation in place. It exercises the copied
package but does not by itself prove complete independence from the original prefix.

Check `add_subdirectory` integration using the installed MsQuic headers and library
(adjust `lib` if GNUInstallDirs selected a different directory):

```sh
cmake -S tests/consumer-subdirectory -B build/consumer-subdirectory \
  -DLIBWT_USE_SYSTEM_MSQUIC=ON \
  -DLIBWT_MSQUIC_INCLUDE_DIR="$PWD/build/install/include" \
  -DLIBWT_MSQUIC_LIBRARY="$PWD/build/install/lib/libmsquic.so"
cmake --build build/consumer-subdirectory -j4
build/consumer-subdirectory/consumer
```

These checks mirror the consumer stages in the [workflow](../.github/workflows/ci.yml).

## Code style

Formatting is defined by `.editorconfig`, `.clang-format`, `.cmake-format.yaml`,
`.prettierrc.json`, and `pyproject.toml`:

- UTF-8, LF line endings, final newlines, and no trailing whitespace.
- C++: clang-format 20, four-space indentation, Allman braces, 100-column limit.
- Python: Ruff, four-space indentation, 100-column limit.
- JavaScript, TypeScript, JSON, HTML, YAML, and documentation: Prettier.
- CMake: cmake-format, four-space indentation, 100-column limit.

The exact Python tool versions are in `requirements-format.txt`; Prettier is pinned
in `package.json` and `package-lock.json`. After the setup above:

```sh
npm run format:check
npm run format
```

The first command checks, and the second rewrites first-party files selected by
`scripts/format.py`. Vendored codec sources are excluded. Run the check before
submitting a pull request.

## Contribution workflow

1. Describe the problem or intended behavior, especially for public API or protocol changes.
2. Make a focused change and update the relevant documentation and TypeScript declarations.
3. Add regression coverage for changed protocol, lifecycle, ownership, or queue behavior.
4. Run formatting and the tests relevant to the change. Run native and Node integration
   suites when changing shared session behavior; run consumer checks for build/package changes.
5. In the pull request, explain the resulting behavior and list the commands run and
   any checks that were not run.

Preserve the distinction between a queued send, transport acknowledgment, and
application delivery. Keep callbacks safe during shutdown, preserve retained-session
behavior, and account for concurrent connections. Document changes to fixed limits
and admission rules. Include upstream provenance when changing vendored sources.

A successful build or CTest run is separate evidence from protocol interoperability,
browser behavior, or performance. Report those checks separately. There are no
dedicated benchmark or sanitizer presets in the current repository.
