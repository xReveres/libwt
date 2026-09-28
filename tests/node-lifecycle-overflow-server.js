'use strict';
const { Server } = require('../bindings/node');

const [port, certificateFile, privateKeyFile] = process.argv.slice(2);
const server = new Server({
  port: Number(port),
  certificateFile,
  privateKeyFile,
  path: '/echo',
  maxConnections: 4,
});
let sessions = 0;
let closes = 0;
let logs = 0;
server.on('log', () => ++logs);
server.on('session', (session) => {
  ++sessions;
  session.on('close', () => ++closes);
});
for (let index = 0; index < 300; ++index) {
  try {
    server.reloadCertificate('missing', 'missing');
  } catch {
    // The server has not started; these records fill only the log queue.
  }
}
server.start();
clearInterval(server.timer);
server.timer = null;
const keepAlive = setInterval(() => {}, 1000);
console.log('READY');
process.on('SIGTERM', () => {
  clearInterval(keepAlive);
  server.stop();
  const stats = server.statistics();
  console.log(
    `SESSIONS ${sessions} CLOSED ${closes} TRANSPORT ${stats.sessions_accepted}/${stats.sessions_closed} LOGS ${logs} DROPPED ${stats.bridge_dropped_logs}`,
  );
  process.exit(
    sessions === 36 &&
      closes === 36 &&
      stats.sessions_accepted === 37n &&
      stats.sessions_closed === 37n &&
      logs === 256 &&
      stats.bridge_dropped_logs === 44n
      ? 0
      : 4,
  );
});
