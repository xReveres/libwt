'use strict';
const assert = require('node:assert/strict');
const { Server, Session } = require('../bindings/node');

const [port, certificateFile, privateKeyFile] = process.argv.slice(2);
const server = new Server({
  port: Number(port),
  certificateFile,
  privateKeyFile,
  origin: 'https://localhost',
  authority: 'localhost',
});
const seen = [];
const logs = [];
let retained;
let datagrams = 0;
let serverDatagrams = 0;
let serverCloses = 0;
server.on('datagram', () => ++serverDatagrams);
server.on('log', (record) => logs.push(record));
server.on('close', (session) => {
  assert.equal(session, retained);
  seen.push('server-close');
  ++serverCloses;
});
server.on('session', (session) => {
  assert(session instanceof Session);
  assert.equal(typeof session.id, 'bigint');
  assert.throws(() => (session.id = 9n), TypeError);
  assert.equal(session.request.path, '/echo');
  assert.equal(session.request.authority, 'localhost');
  assert.equal(session.request.origin, 'https://localhost');
  assert.equal(typeof session.request.peerPort, 'number');
  assert.throws(() => (session.request.path = '/changed'), TypeError);
  assert.throws(() => (session.request = {}), TypeError);
  assert.equal(session, retained || session);
  retained = session;
  seen.push('session');
  server.poll(); // Reentrant poll must leave the current batch in order.
  session.on('datagram', (data) => {
    assert.equal(session, retained);
    seen.push(data.toString());
    if (++datagrams === 1) throw new Error('expected listener failure');
  });
  session.on('close', () => {
    seen.push('close');
    server.stop(); // Reentrant stop must preserve the remaining batch.
    throw new Error('expected close listener failure');
  });
});
server.start();
clearInterval(server.timer);
server.timer = null;
const keepAlive = setInterval(() => {}, 1000);
console.log('READY');
process.on('SIGUSR1', async () => {
  try {
    assert.throws(() => server.poll(), /expected listener failure/);
    assert.throws(() => server.poll(), /expected close listener failure/);
    server.poll();
    assert.deepEqual(seen, ['session', 'one', 'two', 'close', 'server-close']);
    assert.equal(datagrams, 2);
    assert.equal(serverDatagrams, 0);
    assert.equal(serverCloses, 1);
    assert(logs.some((record) => record.level === 'error' && record.sessionId === retained.id));
    assert.equal(retained.closed, true);
    assert.equal(retained.send(Buffer.from('after-close')), 'state');
    assert.throws(() => retained.send('invalid'), /Buffer/);
    retained.close();
    retained.close();
    assert.equal(retained.transportStatistics().available, false);
    const stats = retained.statistics();
    assert.equal(stats.datagrams_received, 2n);
    assert.equal(stats.bytes_received, 6n);
    assert.equal(stats.send_rejected_state, 1n);
    assert.equal(stats.bridge_dropped_datagrams, 0n);
    const weak = new WeakRef(retained);
    retained = null;
    for (let attempt = 0; attempt < 100 && weak.deref(); ++attempt) {
      await new Promise((resolve) => setImmediate(resolve));
      global.gc();
    }
    assert.equal(weak.deref(), undefined);
    clearInterval(keepAlive);
    console.log('PASS delayed polling, event order, retention, listener failure, reentrancy');
    process.exit(0);
  } catch (error) {
    console.error(error);
    process.exit(4);
  }
});
