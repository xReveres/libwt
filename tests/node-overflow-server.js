'use strict';
const { Server } = require('../bindings/node');
const [port, certificateFile, privateKeyFile] = process.argv.slice(2);
const server = new Server({
  port: Number(port),
  certificateFile,
  privateKeyFile,
  path: '/echo',
  maxPendingDatagrams: 1,
});
let sessions = 0;
let closed = 0;
const retained = [];
server.on('session', (session) => {
  ++sessions;
  retained.push(session);
  session.on('close', () => {
    ++closed;
  });
});
server.start();
clearInterval(server.timer);
server.timer = null;
const keepAlive = setInterval(() => {}, 1000);
console.log('READY');
process.on('SIGTERM', () => {
  clearInterval(keepAlive);
  server.stop();
  const dropped = server.statistics().bridge_dropped_datagrams;
  const sessionDropped = retained.map((session) => session.statistics().bridge_dropped_datagrams);
  const sum = sessionDropped.reduce((count, value) => count + value, 0n);
  console.log(
    `DROPPED ${dropped} SESSION_DROPPED ${sessionDropped} SESSIONS ${sessions} CLOSED ${closed}`,
  );
  process.exit(
    dropped > 0n && sum === dropped && sessionDropped[0] > 0n && sessions === 2 && closed === 2
      ? 0
      : 4,
  );
});
