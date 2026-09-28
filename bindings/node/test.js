'use strict';
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { test } = require('node:test');
const { Server, Session } = require('./index');

test('configuration and lifecycle', () => {
  assert.throws(() => new Server({ port: 65536 }), /port exceeds 65535/);
  assert.throws(() => new Server({ maxConnections: 0 }), /maxConnections/);
  assert.throws(() => new Server({ maxPendingDatagrams: 0 }), /maxPendingDatagrams/);
  const server = new Server({});
  const logs = [];
  server.on('log', (record) => logs.push(record));
  assert.equal(server.statistics().connections_accepted, 0n);
  assert.equal(server.statistics().bridge_dropped_datagrams, 0n);
  assert.equal(server.statistics().bridge_dropped_logs, 0n);
  assert.throws(() => server.start(), /Certificate, private key/);
  server.poll(); // Failed start did not install a timer.
  assert.deepEqual(
    logs.map(({ level }) => level),
    ['error'],
  );
  assert.match(logs[0].message, /Certificate, private key/);
  assert.equal(logs[0].sessionId, undefined);
  assert.throws(() => server.reloadCertificate('missing', 'missing'), /not running/);
  server.poll();
  assert.equal(logs.length, 2);
  assert.equal(server.statistics().certificate_reload_errors, 1n);
  assert.throws(() => new Session(), /created by libwt/);
  assert.equal(server.send, undefined);
  assert.equal(server.close, undefined);
  assert.equal(server.sessionStatistics, undefined);
  assert.equal(server.transportStatistics, undefined);
  server.stop();
  server.stop();
});

test('logs have a separate bounded queue after failed startup', () => {
  const server = new Server({ maxConnections: 1 });
  let delivered = 0;
  server.on('log', () => ++delivered);
  assert.throws(() => server.start(), /Certificate, private key/);
  for (let index = 0; index < 300; ++index)
    assert.throws(() => server.reloadCertificate('missing', 'missing'), /not running/);
  assert.equal(server.statistics().bridge_dropped_logs, 45n);
  server.poll();
  assert.equal(delivered, 256);
  assert.equal(server.statistics().bridge_dropped_logs, 45n);
  assert.throws(() => server.reloadCertificate('missing', 'missing'), /not running/);
  server.poll();
  assert.equal(delivered, 257);
  server.stop();
});

test('a throwing log listener leaves later records for poll', () => {
  const server = new Server({});
  const delivered = [];
  server.on('log', (record) => {
    delivered.push(record.message);
    if (delivered.length === 1) throw new Error('listener test');
  });
  assert.throws(() => server.start(), /Certificate, private key/);
  assert.throws(() => server.reloadCertificate('missing', 'missing'), /not running/);
  assert.throws(() => server.poll(), /listener test/);
  assert.equal(delivered.length, 1);
  server.poll();
  assert.equal(delivered.length, 2);
  server.stop();
});

test('numeric configuration validates boundaries and reads each getter once', () => {
  for (const name of [
    'port',
    'idleTimeoutMs',
    'handshakeTimeoutMs',
    'maxConnections',
    'maxPendingDatagrams',
  ]) {
    for (const value of [NaN, Infinity, -Infinity, -1, 1.5, 4294967296]) {
      assert.throws(() => new Server({ [name]: value }), name);
    }
  }
  for (const [name, values] of Object.entries({
    port: [0, 65535],
    idleTimeoutMs: [0, 4294967295],
    handshakeTimeoutMs: [0, 4294967295],
    maxConnections: [1, 65536],
    maxPendingDatagrams: [1, 65536],
  })) {
    for (const value of values) new Server({ [name]: value }).stop();
  }
  const values = {
    bindAddress: '127.0.0.1',
    port: 0,
    certificateFile: '',
    privateKeyFile: '',
    idleTimeoutMs: 0,
    handshakeTimeoutMs: 0,
    maxConnections: 1,
    maxPendingDatagrams: 1,
    path: '/echo',
    origin: '',
    authority: '',
  };
  const config = {};
  const reads = new Map();
  for (const [name, value] of Object.entries(values)) {
    Object.defineProperty(config, name, {
      get() {
        reads.set(name, (reads.get(name) || 0) + 1);
        return value;
      },
    });
  }
  new Server(config).stop();
  for (const name of Object.keys(values)) assert.equal(reads.get(name), 1, name);
  assert.throws(() => new Server({ maxConnections: 65537 }), /maxConnections/);
  assert.throws(() => new Server({ maxPendingDatagrams: 65537 }), /maxPendingDatagrams/);
});

function withCertificate(run) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'libwt-node-'));
  try {
    const cert = path.join(directory, 'cert.pem');
    const key = path.join(directory, 'key.pem');
    execFileSync(
      'openssl',
      [
        'req',
        '-x509',
        '-newkey',
        'ec',
        '-pkeyopt',
        'ec_paramgen_curve:P-256',
        '-nodes',
        '-subj',
        '/CN=localhost',
        '-days',
        '1',
        '-keyout',
        key,
        '-out',
        cert,
      ],
      { stdio: 'ignore' },
    );
    run({ bindAddress: '127.0.0.1', certificateFile: cert, privateKeyFile: key });
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
}

test('timer ownership and failed startup', () => {
  const set = global.setInterval;
  const clear = global.clearInterval;
  const active = new Set();
  let next = 0;
  global.setInterval = () => {
    const timer = ++next;
    active.add(timer);
    return timer;
  };
  global.clearInterval = (timer) => {
    assert.ok(active.delete(timer));
  };
  try {
    const failed = new Server({});
    assert.throws(() => failed.start(), /Certificate, private key/);
    assert.equal(active.size, 0);
    failed.stop();
    withCertificate((config) => {
      const server = new Server(config);
      server.start();
      server.start();
      assert.equal(active.size, 1);
      server.poll();
      server.stop();
      server.stop();
      assert.equal(active.size, 0);
      server.start();
      assert.equal(active.size, 1);
      server.stop();
      assert.equal(active.size, 0);
    });
  } finally {
    global.setInterval = set;
    global.clearInterval = clear;
  }
});

test('native start, repeated start, stop, and restart with real timer', () => {
  withCertificate((config) => {
    const server = new Server(config);
    server.start();
    server.start();
    server.stop();
    server.stop();
    server.start();
    server.stop();
  });
});
