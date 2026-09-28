'use strict';
const { EventEmitter } = require('node:events');
const path = require('node:path');
const native = require(
  process.env.LIBWT_NODE_ADDON || path.join(__dirname, '../../build/bindings/node/wt_node.node'),
);

const sessionKey = Symbol('libwt session');

class Session extends EventEmitter {
  #handle;
  constructor(key, id, handle, request) {
    super();
    if (key !== sessionKey) throw new TypeError('Session instances are created by libwt');
    Object.defineProperty(this, 'id', { value: id, enumerable: true });
    Object.defineProperty(this, 'request', { value: Object.freeze(request), enumerable: true });
    this.#handle = handle;
  }
  get closed() {
    return native.sessionClosed(this.#handle);
  }
  send(data) {
    return native.send(this.#handle, data);
  }
  close() {
    native.close(this.#handle);
  }
  statistics() {
    return native.sessionStatistics(this.#handle);
  }
  transportStatistics() {
    return native.transportStatistics(this.#handle);
  }
}

class Server extends EventEmitter {
  #sessions = new Map();
  #events = [];
  #eventIndex = 0;
  #dispatching = false;
  #pollAgain = false;
  constructor(config) {
    super();
    this.handle = native.create(config);
    this.timer = null;
  }
  start() {
    if (this.timer !== null) return;
    native.start(this.handle);
    this.timer = setInterval(() => this.poll(), 5);
  }
  poll() {
    if (this.#dispatching) {
      this.#pollAgain = true;
      return;
    }
    this.#dispatching = true;
    try {
      do {
        this.#pollAgain = false;
        for (const event of native.poll(this.handle)) this.#events.push(event);
        while (this.#eventIndex < this.#events.length) {
          const event = this.#events[this.#eventIndex++];
          if (event.type === 'session') {
            const session = new Session(sessionKey, event.id, event.handle, event.request);
            this.#sessions.set(event.id, session);
            this.emit('session', session);
          } else if (event.type === 'log') {
            const { level, message, sessionId } = event;
            this.emit(
              'log',
              sessionId === undefined ? { level, message } : { level, message, sessionId },
            );
          } else if (event.type === 'server-close') {
            this.emit('close', event.session);
          } else {
            const session = this.#sessions.get(event.id);
            if (!session) continue;
            if (event.type === 'datagram') session.emit('datagram', event.data);
            else if (event.type === 'close') {
              this.#sessions.delete(event.id);
              this.#events.splice(this.#eventIndex, 0, { type: 'server-close', session });
              session.emit('close');
            }
          }
        }
        this.#events = [];
        this.#eventIndex = 0;
      } while (this.#pollAgain);
    } finally {
      if (this.#eventIndex) {
        this.#events = this.#events.slice(this.#eventIndex);
        this.#eventIndex = 0;
      }
      this.#dispatching = false;
    }
  }
  stop() {
    if (this.timer !== null) clearInterval(this.timer);
    this.timer = null;
    native.stop(this.handle);
    this.poll();
  }
  reloadCertificate(cert, key) {
    native.reload(this.handle, cert, key);
  }
  statistics() {
    return native.statistics(this.handle);
  }
}
module.exports = { Server, Session };
