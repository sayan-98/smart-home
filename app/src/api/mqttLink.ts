// ---------------------------------------------------------------------------
//  mqttLink.ts - control from anywhere, over a broker.
//
//  Why a broker instead of exposing the device or running a server:
//
//    ESP32  --outbound TLS-->  broker  <--outbound WSS--  app
//
//  Both ends dial OUT. Nothing needs a public IP, a port forward, or a static
//  address, so it works behind carrier-grade NAT - which is what you are on if
//  your internet is a phone hotspot or most Indian broadband.
//
//  Two properties of the firmware make this much simpler than it looks:
//
//  1. State topics are RETAINED. The moment this connects, the broker replays
//     the current state of every channel of every device. No polling, no cold
//     start, no "loading" spinner - the UI is correct within a second of
//     opening the app in the office.
//
//  2. Devices are discovered from those same retained messages. There is no
//     device registry to configure; subscribing to the wildcard IS the
//     registry.
//
//  Deliberately no HTTP server anywhere in this path, so there is nothing to
//  keep warm and nothing to wake up.
// ---------------------------------------------------------------------------
import mqtt, { type MqttClient } from 'mqtt';

export interface BrokerConfig {
  /** Host only, e.g. "abc123.s1.eu.hivemq.cloud" */
  host: string;
  /** WebSocket-over-TLS port. HiveMQ Cloud uses 8884. */
  port: number;
  username: string;
  password: string;
  /** Path for the WebSocket listener. HiveMQ uses /mqtt. */
  path: string;
}

export interface RemoteChannel {
  channel: number;
  name: string;
  icon: string;
  state: boolean;
  source: string;
  rev: number;
  enabled: boolean;
  group: number;
}

export interface RemoteDevice {
  uuid: string;
  homeId: string;
  online: boolean;
  firmware?: string;
  /** Current LAN address, learned from the heartbeat. */
  ip?: string;
  rssi?: number;
  brownout?: boolean;
  channels: Map<number, RemoteChannel>;
}

export interface MqttLinkEvents {
  onStatus: (connected: boolean, detail?: string) => void;
  onDevices: (devices: Map<string, RemoteDevice>) => void;
}

const TOPIC_ROOT = 'smarthome';

export class MqttLink {
  private client: MqttClient | null = null;
  private devices = new Map<string, RemoteDevice>();
  private config: BrokerConfig | null = null;

  constructor(private events: MqttLinkEvents) {}

  get connected(): boolean {
    return Boolean(this.client?.connected);
  }

  connect(config: BrokerConfig): void {
    this.disconnect();
    this.config = config;

    const url = `wss://${config.host}:${config.port}${config.path}`;

    this.client = mqtt.connect(url, {
      username: config.username,
      password: config.password,
      // A stable-but-unique id: two phones must not evict each other, and a
      // reconnect should not leave a ghost session behind.
      clientId: `sh-app-${Math.random().toString(16).slice(2, 10)}`,
      clean: true,
      reconnectPeriod: 4000,
      connectTimeout: 12000,
      keepalive: 45,
      protocolVersion: 5,
    });

    this.client.on('connect', () => {
      // Retained state arrives immediately after these subscriptions, which is
      // what makes the app correct on open rather than after a poll.
      this.client?.subscribe(`${TOPIC_ROOT}/+/+/relay/+/state`, { qos: 1 });
      this.client?.subscribe(`${TOPIC_ROOT}/+/+/status`, { qos: 1 });
      // The heartbeat carries the device's current LAN address. Without it you
      // would have to hunt for the IP after every DHCP change just to open the
      // device's own web page - the app itself never needs it.
      this.client?.subscribe(`${TOPIC_ROOT}/+/+/diag`, { qos: 0 });
      this.events.onStatus(true);
    });

    this.client.on('reconnect', () => this.events.onStatus(false, 'reconnecting'));
    this.client.on('offline', () => this.events.onStatus(false, 'offline'));
    this.client.on('close', () => this.events.onStatus(false));

    this.client.on('error', (err) => {
      // Bad credentials are the common case and worth naming, because the
      // generic message is useless.
      const message = /not authorized|bad user|unauthorized/i.test(err.message)
        ? 'The broker rejected those credentials'
        : err.message;
      this.events.onStatus(false, message);
    });

    this.client.on('message', (topic, payload) => {
      try {
        this.handle(topic, payload.toString('utf8'));
      } catch {
        /* one malformed retained message must not break the link */
      }
    });
  }

  private device(homeId: string, uuid: string): RemoteDevice {
    let d = this.devices.get(uuid);
    if (!d) {
      d = { uuid, homeId, online: false, channels: new Map() };
      this.devices.set(uuid, d);
    }
    d.homeId = homeId;
    return d;
  }

  /** smarthome/<homeId>/<uuid>/relay/<ch>/state  |  .../status */
  private handle(topic: string, raw: string): void {
    const parts = topic.split('/');
    if (parts.length < 4 || parts[0] !== TOPIC_ROOT) return;

    const homeId = parts[1];
    const uuid = parts[2];
    if (!raw.trim()) return;

    const body = JSON.parse(raw) as Record<string, unknown>;

    if (parts[3] === 'status') {
      const d = this.device(homeId, uuid);
      d.online = Boolean(body.online);
      if (typeof body.fw === 'string') d.firmware = body.fw;
      this.events.onDevices(this.devices);
      return;
    }

    if (parts[3] === 'diag') {
      const d = this.device(homeId, uuid);
      if (typeof body.ip === 'string' && body.ip) d.ip = body.ip;
      if (typeof body.rssi === 'number') d.rssi = body.rssi;
      if (typeof body.brownout === 'boolean') d.brownout = body.brownout;
      // A heartbeat is proof of life even if the retained status is stale.
      d.online = true;
      this.events.onDevices(this.devices);
      return;
    }

    if (parts[3] === 'relay' && parts[5] === 'state') {
      const channel = Number(parts[4]);
      if (!Number.isFinite(channel)) return;

      const d = this.device(homeId, uuid);
      const existing = d.channels.get(channel);
      const rev = Number(body.rev ?? 0);

      // Retained messages and live updates can interleave on reconnect. The
      // device's monotonic revision decides, not arrival order.
      if (existing && rev !== 0 && rev < existing.rev) return;

      d.channels.set(channel, {
        channel,
        name: String(body.name ?? `Socket ${channel + 1}`),
        icon: String(body.icon ?? 'socket'),
        state: Boolean(body.state),
        source: String(body.source ?? 'unknown'),
        rev,
        enabled: body.enabled !== false,
        group: Number(body.group ?? 0),
      });
      this.events.onDevices(this.devices);
    }
  }

  /**
   * Sends a command. `rev` makes the write conflict-safe: the device applies it
   * only if it is newer than what the channel already has, so a command sent
   * over a slow mobile link cannot undo a wall-switch press that happened while
   * it was in flight.
   */
  publishCommand(
    homeId: string,
    uuid: string,
    channel: number | 'all',
    action: 'on' | 'off' | 'toggle',
    rev?: number,
  ): boolean {
    if (!this.client?.connected) return false;

    const topic = `${TOPIC_ROOT}/${homeId}/${uuid}/relay/${channel}/set`;
    const payload: Record<string, unknown> = { action };
    if (rev !== undefined) payload.rev = rev;

    this.client.publish(topic, JSON.stringify(payload), { qos: 1 });
    return true;
  }

  /** Asks a device to republish everything. Rarely needed - state is retained. */
  requestSnapshot(homeId: string, uuid: string): void {
    this.client?.publish(
      `${TOPIC_ROOT}/${homeId}/${uuid}/cmd`,
      JSON.stringify({ cmd: 'snapshot' }),
      { qos: 1 },
    );
  }

  disconnect(): void {
    if (!this.client) return;
    this.client.removeAllListeners();
    this.client.end(true);
    this.client = null;
    this.devices.clear();
  }

  get brokerHost(): string | null {
    return this.config?.host ?? null;
  }
}
