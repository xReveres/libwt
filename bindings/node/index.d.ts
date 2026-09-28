import { EventEmitter } from 'node:events';

export type DatagramResult = 'accepted' | 'size' | 'queue' | 'state' | 'error';
export interface Request {
  readonly authority: string;
  readonly path: string;
  readonly origin: string;
  readonly peerAddress: string;
  readonly peerPort: number;
}
export interface LogRecord {
  level: 'warning' | 'error';
  message: string;
  sessionId?: bigint;
}
export interface Config {
  bindAddress?: string;
  port?: number;
  certificateFile?: string;
  privateKeyFile?: string;
  idleTimeoutMs?: number;
  handshakeTimeoutMs?: number;
  maxConnections?: number;
  maxPendingDatagrams?: number;
  path?: string;
  origin?: string;
  authority?: string;
}
export interface ServerStatistics {
  connections_accepted: bigint;
  connections_rejected: bigint;
  connections_connected: bigint;
  connections_closed: bigint;
  active_connections: bigint;
  sessions_accepted: bigint;
  sessions_rejected: bigint;
  sessions_closed: bigint;
  active_sessions: bigint;
  datagrams_received: bigint;
  bytes_received: bigint;
  datagrams_ignored: bigint;
  datagrams_sent: bigint;
  bytes_sent: bigint;
  send_rejected_size: bigint;
  send_rejected_queue: bigint;
  send_rejected_state: bigint;
  send_errors: bigint;
  datagrams_acknowledged: bigint;
  datagrams_lost: bigint;
  datagrams_canceled: bigint;
  callback_errors: bigint;
  protocol_errors: bigint;
  certificate_reloads: bigint;
  certificate_reload_errors: bigint;
  bridge_dropped_datagrams: bigint;
  bridge_dropped_logs: bigint;
}
export interface SessionStatistics {
  datagrams_received: bigint;
  bytes_received: bigint;
  datagrams_sent: bigint;
  bytes_sent: bigint;
  send_rejected_size: bigint;
  send_rejected_queue: bigint;
  send_rejected_state: bigint;
  send_errors: bigint;
  datagrams_acknowledged: bigint;
  datagrams_lost: bigint;
  datagrams_canceled: bigint;
  callback_errors: bigint;
  bridge_dropped_datagrams: bigint;
}
export type Statistics = ServerStatistics;
export interface TransportStatistics {
  available: boolean;
  rtt_us: bigint;
  min_rtt_us: bigint;
  max_rtt_us: bigint;
  congestion_window_bytes: bigint;
  path_mtu: bigint;
  packets_sent: bigint;
  packets_received: bigint;
  suspected_lost_packets: bigint;
  spurious_lost_packets: bigint;
  udp_bytes_sent: bigint;
  udp_bytes_received: bigint;
  receive_dropped_packets: bigint;
  receive_decryption_failures: bigint;
}
export class Session extends EventEmitter {
  private constructor();
  readonly id: bigint;
  readonly request: Request;
  readonly closed: boolean;
  send(data: Buffer): DatagramResult;
  close(): void;
  statistics(): SessionStatistics;
  transportStatistics(): TransportStatistics;
  on(event: 'datagram', listener: (data: Buffer) => void): this;
  on(event: 'close', listener: () => void): this;
}
export class Server extends EventEmitter {
  constructor(config: Config);
  start(): void;
  poll(): void;
  stop(): void;
  reloadCertificate(cert: string, key: string): void;
  statistics(): ServerStatistics;
  on(event: 'session', listener: (session: Session) => void): this;
  on(event: 'close', listener: (session: Session) => void): this;
  on(event: 'log', listener: (record: LogRecord) => void): this;
}
