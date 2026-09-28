# Node.js API

Build with `LIBWT_BUILD_NODE=ON` or the `node` preset before requiring
`bindings/node`. The binding is a local CommonJS package named `libwt-local`, marked
`private: true`; there is no npm publication or prebuilt binary installation flow.
TypeScript declarations are in [`index.d.ts`](../bindings/node/index.d.ts).

## Example

Save this in the repository root and run it with Node.js after generating the
[example certificates](building.md#running-the-examples):

```js
'use strict';
const { Server } = require('./bindings/node');

const server = new Server({
  bindAddress: '127.0.0.1',
  port: 4433,
  certificateFile: 'build/certs/cert.pem',
  privateKeyFile: 'build/certs/key.pem',
  path: '/echo',
});
server.on('log', (record) => console.error(record.level, record.message));
server.on('session', (session) => {
  console.log('Accepted', session.id, session.request.path);
  session.on('datagram', (data) => {
    const result = session.send(data);
    if (result !== 'accepted') console.error('Echo send rejected:', result);
  });
  session.on('close', () => console.log('Closed', session.id));
});
server.start();
process.on('SIGINT', () => server.stop());
process.on('SIGTERM', () => server.stop());
```

## Configuration

`new Server(config)` requires a configuration object. `start()` requires certificate
and key paths even though the TypeScript properties are optional for construction.

| Property                            | Default       | Meaning                                                                   |
| ----------------------------------- | ------------- | ------------------------------------------------------------------------- |
| `bindAddress`                       | `'127.0.0.1'` | Numeric bind address                                                      |
| `port`                              | `4433`        | UDP port, integer from 0 to 65535                                         |
| `certificateFile`, `privateKeyFile` | Empty strings | PEM credentials                                                           |
| `idleTimeoutMs`                     | `30000`       | Connection idle timeout                                                   |
| `handshakeTimeoutMs`                | `5000`        | Handshake idle timeout                                                    |
| `maxConnections`                    | `1024`        | Integer from 1 to 65536                                                   |
| `maxPendingDatagrams`               | `1024`        | Global native-to-JavaScript datagram queue limit, integer from 1 to 65536 |
| `path`                              | `'/echo'`     | Exact request path match                                                  |
| `origin`                            | Empty string  | Exact Origin match; empty disables this filter                            |
| `authority`                         | Empty string  | Exact authority match; empty disables this filter                         |

Numeric options must be finite unsigned 32-bit integers, with the additional
bounds above. Explicit `undefined` values are not treated as omitted options.
Admission is synchronous in native code using these filters; there is no JavaScript
`on_request` callback. Filters do not support patterns or URL normalization.

### Environment variable

`LIBWT_NODE_ADDON` overrides the native addon module path. Use an absolute path:

```sh
LIBWT_NODE_ADDON="$PWD/build/custom/bindings/node/wt_node.node" \
  node examples/node-echo.js 4433 build/certs/cert.pem build/certs/key.pem
```

Without it, the loader uses `build/bindings/node/wt_node.node` relative to the
repository layout. This variable changes addon discovery, not shared-library
resolution. No other libwt runtime environment variables are read by the binding.

## Server

`Server` extends `EventEmitter`.

| Method                               | Behavior                                                                                                                 |
| ------------------------------------ | ------------------------------------------------------------------------------------------------------------------------ |
| `start(): void`                      | Starts the native server and a 5 ms polling timer. Repeated calls while running are harmless. Throws on startup failure. |
| `stop(): void`                       | Clears the timer, waits for native shutdown, then polls remaining events. May be called repeatedly.                      |
| `poll(): void`                       | Drains pending events synchronously. Also useful after failed startup, when there is no timer.                           |
| `reloadCertificate(cert, key): void` | Transactional rotation; throws on failure. Existing sessions remain open.                                                |
| `statistics(): ServerStatistics`     | Native totals plus `bridge_dropped_datagrams` and `bridge_dropped_logs`.                                                 |

| Event       | Argument                         | When emitted                                                  |
| ----------- | -------------------------------- | ------------------------------------------------------------- |
| `'session'` | `Session`                        | An accepted session is published                              |
| `'close'`   | The same `Session`               | That session ends, after its own close event                  |
| `'log'`     | `{ level, message, sessionId? }` | Native warning or error; `sessionId` is a bigint when present |

The server's `'close'` event is a **session close notification**, not a separate
server-stopped event. Datagram events belong to sessions. Internal `handle` and
`timer` properties are implementation details; application code should use the methods above.

## Session

Instances are created by libwt; `new Session()` is unsupported. `Session` extends
`EventEmitter` and has these properties and methods:

| Member                                       | Meaning                                                                                               |
| -------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| `id: bigint`                                 | Read-only identity within the server                                                                  |
| `request: Request`                           | Read-only frozen metadata: `authority`, `path`, `origin`, `peerAddress` strings and `peerPort` number |
| `closed: boolean`                            | Native close state; can become true before the queued close event is delivered                        |
| `send(data: Buffer): DatagramResult`         | Copies and queues a payload; accepts a Node `Buffer`                                                  |
| `close(): void`                              | Initiates connection shutdown; harmless after close                                                   |
| `statistics(): SessionStatistics`            | Native counters plus this session's `bridge_dropped_datagrams`                                        |
| `transportStatistics(): TransportStatistics` | Live sample with an `available` flag                                                                  |

`send()` returns `'accepted'`, `'size'`, `'queue'`, `'state'`, or `'error'`, matching
the [C++ send outcomes](api.md#session-operations). Invalid arguments throw.
`'accepted'` does not confirm delivery. `maxPendingDatagrams` controls incoming
bridge events, not this outgoing queue.

Session events are `'datagram'` with a copied `Buffer`, and `'close'` with no arguments.
Retained sessions keep their metadata and counters after shutdown. Sends after
close normally return `'state'`; transport samples report `available: false`.

All statistics integer fields, including transport fields, are JavaScript `bigint`.
Only `available` is boolean. See [statistics semantics](api.md#statistics) for units
and counter meanings. For JSON logging, convert bigints explicitly:

```js
const json = JSON.stringify(server.statistics(), (_, value) =>
  typeof value === 'bigint' ? value.toString() : value,
);
console.log(json);
```

## Event delivery and queue limits

Native callbacks copy events into a mutex-protected queue. JavaScript polls that
queue every 5 ms after successful startup; listeners run on the JavaScript thread.
The timer keeps the process alive until `stop()` is called. Event-loop blocking
delays delivery and can exhaust bridge capacity.

- Queued datagrams across all sessions are bounded by `maxPendingDatagrams`.
  Overflow drops new incoming bridge events and increments server and session
  `bridge_dropped_datagrams`. Native receive totals still count those datagrams.
- Logs have a separate limit of 256 queued records; overflow increments
  `bridge_dropped_logs`.
- Lifecycle capacity is `2 * maxConnections + 64`. Each published session reserves
  room for both creation and close events. If reservation fails, the newly accepted
  native session is closed without being exposed to JavaScript.
- These limits apply to the native bridge. `poll()` moves a batch into JavaScript,
  where unprocessed events can remain pending if a listener throws.

Listeners should handle their own errors. An exception propagates from `poll()`;
when thrown during automatic timer dispatch it can become an uncaught exception.
The current event is consumed, while later events remain queued for a subsequent
poll. Reentrant polling is deferred until the active dispatch completes.

Unlike native transport callbacks, JavaScript listeners may call `stop()`: they
run after events have crossed the bridge. `stop()` can synchronously invoke
remaining listeners through its final poll.
