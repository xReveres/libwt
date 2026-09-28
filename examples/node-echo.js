'use strict';
const { Server } = require('../bindings/node');
if (process.argv.length !== 5) {
  console.error('Usage: node node-echo.js PORT CERT KEY');
  process.exit(2);
}
const [port, certificateFile, privateKeyFile] = process.argv.slice(2);
const server = new Server({ port: Number(port), certificateFile, privateKeyFile, path: '/echo' });
server.on('session', (session) => {
  const sample = session.transportStatistics();
  console.log(`SAMPLE available=${sample.available} session=${session.id}`);
  session.on('datagram', (data) => session.send(data));
  session.on('close', () => console.log('CLOSED'));
});
try {
  server.start();
} catch (error) {
  console.error(error);
  process.exit(1);
}
process.on('SIGHUP', () => {
  try {
    server.reloadCertificate(certificateFile, privateKeyFile);
    console.log('RELOADED');
  } catch (error) {
    console.log(`RELOAD_FAILED: ${error.message}`);
  }
});
const stop = () => {
  server.stop();
  const stats = server.statistics();
  console.log(
    `STATS received=${stats.datagrams_received} sent=${stats.datagrams_sent} active=${stats.active_sessions}`,
  );
  process.exit(
    stats.datagrams_received > 0n &&
      stats.datagrams_sent > 0n &&
      stats.sessions_rejected > 0n &&
      stats.active_sessions === 0n &&
      stats.sessions_accepted === stats.sessions_closed &&
      stats.connections_accepted === stats.connections_closed
      ? 0
      : 4,
  );
};
process.on('SIGTERM', stop);
process.on('SIGINT', stop);
console.log('READY');
