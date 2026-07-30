// ---------------------------------------------------------------------------
//  store.ts - app state, connection management and the outbound command queue.
//
//  TWO MODES, and the distinction matters:
//
//    DIRECT - the app talks straight to the ESP32's own REST + WebSocket
//             server. No account, no backend, no broker, no internet. This is
//             the right mode for one board on your own Wi-Fi, and it keeps
//             working when everything else is down.
//
//    CLOUD  - REST + Socket.IO against the backend. Needed only for control
//             from outside the house, multiple devices, or the AI features.
//
//  Two behaviours below earn their keep in both modes:
//
//  1. OPTIMISTIC UI WITH RECONCILIATION. Tapping a tile flips it immediately,
//     because a switch that waits for a round trip feels broken. The real state
//     then arrives over the socket and overwrites the guess. If the command
//     failed, the tile flips back - visibly, so you know it did not happen
//     rather than being quietly lied to.
//
//  2. A COMMAND QUEUE. Commands issued while disconnected are replayed on
//     reconnect, newest-per-channel only. Replaying three stale toggles for one
//     socket would be worse than dropping two of them.
// ---------------------------------------------------------------------------
import { Network } from '@capacitor/network';
import { io, type Socket } from 'socket.io-client';

import { MqttLink, type BrokerConfig, type RemoteDevice } from './mqttLink.js';
import {
  ApiError,
  cloud,
  discoverDirect,
  getApiBase,
  getBroker,
  getDirectBase,
  getToken,
  lan,
  loadEndpoints,
  probeLan,
  rememberEndpoint,
  setBroker,
  setDirectBase,
  type Mode,
  type StoredBroker,
} from './transport.js';

export interface Relay {
  id: string;
  channel: number;
  name: string;
  icon: string;
  state: boolean;
  source: string;
  rev: number;
  enabled: boolean;
  groupId: number;
  changedAt: string;
}

export interface Device {
  id: string;
  uuid: string;
  name: string;
  online: boolean;
  firmwareVersion: string;
  rssi: number | null;
  lastBrownout: boolean;
  lastIp: string | null;
  relays: Relay[];
  room?: { id: string; name: string } | null;
}

export interface Home {
  id: string;
  name: string;
  role: string;
}

type Listener = () => void;

interface QueuedCommand {
  deviceId: string;
  channel: number | 'all';
  action: 'on' | 'off' | 'toggle';
  at: number;
}

/** Shape of one channel in the device's own /api/state response. */
interface LocalChannel {
  channel: number;
  name: string;
  icon: string;
  state: boolean;
  source: string;
  rev: number;
  enabled: boolean;
  group: number;
  changedAt: number;
}

class Store {
  mode: Mode = 'offline';
  /** Set when running without a backend. */
  direct = false;
  directBase: string | null = null;

  lanBase: string | null = null;
  lanUuid: string | null = null;

  homes: Home[] = [];
  activeHomeId: string | null = null;
  devices: Device[] = [];

  aiEnabled = false;
  error: string | null = null;

  /** Set when controlling through a broker from outside the house. */
  remote = false;
  remoteDetail: string | null = null;

  private socket: Socket | null = null;
  private ws: WebSocket | null = null;
  private link: MqttLink | null = null;
  /** uuid -> homeId, needed to address command topics. */
  private homeOf = new Map<string, string>();
  private listeners = new Set<Listener>();
  private queue: QueuedCommand[] = [];
  private timer: ReturnType<typeof setInterval> | null = null;
  private disposed = false;

  subscribe(fn: Listener): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  private emit(): void {
    for (const fn of this.listeners) fn();
  }

  private setError(message: string | null): void {
    this.error = message;
    this.emit();
  }

  clearError(): void {
    this.setError(null);
  }

  // --- lifecycle -----------------------------------------------------------

  /** Returns true when a session (direct or cloud) is active. */
  async init(): Promise<boolean> {
    this.disposed = false;

    // A broker session is tried first: it is the only mode that works from
    // outside the house, and it costs nothing to fall back from.
    const broker = await getBroker();
    if (broker) {
      this.startRemote(broker);
      return true;
    }

    const directBase = await getDirectBase();
    if (directBase) {
      const ok = await this.startDirect(directBase);
      if (ok) return true;
      // Saved address no longer answers - fall through so the user can
      // re-discover or sign in, rather than sitting on a dead screen.
      this.setError('The saved device did not answer. Is it powered on?');
    }

    const token = await getToken();
    if (!token) {
      this.mode = 'offline';
      this.emit();
      return false;
    }

    await this.refreshHomes();
    await this.connectSocket();

    this.timer = setInterval(() => void this.probeLanPath(), 30_000);
    void Network.addListener('networkStatusChange', () => void this.probeLanPath());
    await this.probeLanPath();
    return this.homes.length > 0;
  }

  dispose(): void {
    this.disposed = true;
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
    this.socket?.disconnect();
    this.socket = null;
    this.ws?.close();
    this.ws = null;
    this.link?.disconnect();
    this.link = null;
  }

  // --- remote mode (broker) ------------------------------------------------

  /**
   * Connects through a broker. This is the only mode that reaches the house
   * from outside it: both the device and this app dial OUT to the broker, so
   * no port forwarding, static IP or public endpoint is involved.
   */
  async connectRemote(broker: StoredBroker): Promise<boolean> {
    await setBroker(broker);
    this.startRemote(broker);

    // Wait briefly for a verdict so the UI can report a bad password instead of
    // silently sitting on a login screen.
    for (let i = 0; i < 30; i++) {
      await new Promise((r) => setTimeout(r, 400));
      if (this.link?.connected) return true;
      if (this.remoteDetail) return false;
    }
    return this.link?.connected ?? false;
  }

  private startRemote(broker: BrokerConfig): void {
    this.remote = true;
    this.direct = false;
    this.remoteDetail = null;

    this.link?.disconnect();
    this.link = new MqttLink({
      onStatus: (connected, detail) => {
        this.mode = connected ? 'remote' : 'offline';
        this.remoteDetail = connected ? null : (detail ?? null);
        if (connected) {
          this.setError(null);
          void this.flushQueue();
        } else if (detail && detail !== 'reconnecting') {
          this.setError(detail);
        }
        this.emit();
      },
      onDevices: (devices) => this.applyRemoteDevices(devices),
    });

    this.link.connect(broker);
  }

  /** Rebuilds the device list from what the broker replayed. */
  private applyRemoteDevices(devices: Map<string, RemoteDevice>): void {
    this.devices = [...devices.values()].map((d) => {
      this.homeOf.set(d.uuid, d.homeId);
      return {
        id: d.uuid,
        uuid: d.uuid,
        name: d.uuid,
        online: d.online,
        firmwareVersion: d.firmware ?? '',
        rssi: null,
        lastBrownout: false,
        lastIp: null,
        relays: [...d.channels.values()]
          .sort((a, b) => a.channel - b.channel)
          .map((c) => ({
            id: `${d.uuid}:${c.channel}`,
            channel: c.channel,
            name: c.name,
            icon: c.icon,
            state: c.state,
            source: c.source,
            rev: c.rev,
            enabled: c.enabled,
            groupId: c.group,
            changedAt: '',
          })),
      };
    });

    this.homes = [{ id: 'remote', name: 'My Home', role: 'OWNER' }];
    this.activeHomeId = 'remote';
    this.emit();
  }

  async forgetRemote(): Promise<void> {
    await setBroker(null);
    this.link?.disconnect();
    this.link = null;
    this.remote = false;
    this.devices = [];
    this.homes = [];
    this.mode = 'offline';
    this.emit();
  }

  // --- direct mode ---------------------------------------------------------

  /** Probes an address and, if a device answers, switches into direct mode. */
  async connectDirect(hint?: string): Promise<boolean> {
    const found = await discoverDirect(hint);
    if (!found) {
      this.setError(
        'No device answered. Check that this tablet is on the same Wi-Fi, and try the IP address shown on the device page.',
      );
      return false;
    }
    await setDirectBase(found.base);
    return this.startDirect(found.base);
  }

  private async startDirect(base: string): Promise<boolean> {
    try {
      const info = await lan<{
        uuid: string;
        name: string;
        firmware: string;
        rssi: number;
        ip: string;
      }>(base, '/api/info');

      this.direct = true;
      this.directBase = base;
      this.homes = [{ id: 'local', name: info.name || 'Smart Home', role: 'OWNER' }];
      this.activeHomeId = 'local';
      this.aiEnabled = false; // AI lives in the backend, by design

      await this.refreshDirectState(info);
      this.openDirectSocket(base);
      this.mode = 'lan';
      this.emit();
      return true;
    } catch {
      this.direct = false;
      this.directBase = null;
      return false;
    }
  }

  private async refreshDirectState(info?: {
    uuid: string;
    name: string;
    firmware: string;
    rssi: number;
    ip: string;
  }): Promise<void> {
    if (!this.directBase) return;

    const meta =
      info ??
      (await lan<{ uuid: string; name: string; firmware: string; rssi: number; ip: string }>(
        this.directBase,
        '/api/info',
      ));

    const snapshot = await lan<{ channels: LocalChannel[] }>(this.directBase, '/api/state');

    this.devices = [
      {
        id: meta.uuid,
        uuid: meta.uuid,
        name: meta.name || 'Smart Home Node',
        online: true,
        firmwareVersion: meta.firmware,
        rssi: meta.rssi ?? null,
        lastBrownout: false,
        lastIp: meta.ip ?? null,
        relays: snapshot.channels.map((c) => ({
          id: `${meta.uuid}:${c.channel}`,
          channel: c.channel,
          name: c.name,
          icon: c.icon,
          state: c.state,
          source: c.source,
          rev: c.rev,
          enabled: c.enabled,
          groupId: c.group,
          changedAt: String(c.changedAt),
        })),
      },
    ];
    this.emit();
  }

  /**
   * The device's own WebSocket. This is what makes a wall-switch press or an
   * Alexa command appear on the tablet instantly, with no cloud involved.
   */
  private openDirectSocket(base: string): void {
    this.ws?.close();
    const url = base.replace(/^http/, 'ws') + '/ws';

    try {
      const ws = new WebSocket(url);
      this.ws = ws;

      ws.onopen = () => {
        this.mode = 'lan';
        void this.flushQueue();
        this.emit();
      };

      ws.onmessage = (evt) => {
        try {
          const msg = JSON.parse(String(evt.data)) as {
            type: string;
            channel?: number;
            state?: boolean;
            source?: string;
            rev?: number;
            data?: { channels: LocalChannel[] };
          };

          if (msg.type === 'relay' && msg.channel !== undefined) {
            this.applyRelayState(
              this.devices[0]?.id ?? '',
              msg.channel,
              Boolean(msg.state),
              msg.source ?? 'unknown',
              msg.rev ?? 0,
            );
          } else if (msg.type === 'state' && msg.data) {
            const device = this.devices[0];
            if (device) {
              for (const c of msg.data.channels) {
                this.applyRelayState(device.id, c.channel, c.state, c.source, c.rev);
              }
            }
          }
        } catch {
          /* ignore malformed frames */
        }
      };

      ws.onclose = () => {
        if (this.disposed || !this.direct) return;
        this.mode = 'offline';
        this.emit();
        // The device reboots, Wi-Fi drops, the tablet sleeps. Just keep trying.
        setTimeout(() => {
          if (!this.disposed && this.directBase) this.openDirectSocket(this.directBase);
        }, 3000);
      };

      ws.onerror = () => ws.close();
    } catch {
      setTimeout(() => {
        if (!this.disposed && this.directBase) this.openDirectSocket(this.directBase);
      }, 3000);
    }
  }

  async forgetDirect(): Promise<void> {
    await setDirectBase(null);
    this.direct = false;
    this.directBase = null;
    this.devices = [];
    this.homes = [];
    this.ws?.close();
    this.ws = null;
    this.mode = 'offline';
    this.emit();
  }

  // --- cloud mode ----------------------------------------------------------

  private async connectSocket(): Promise<void> {
    const base = await getApiBase();
    const token = await getToken();
    if (!base || !token) return;

    this.socket?.disconnect();
    this.socket = io(base, {
      auth: { token },
      transports: ['websocket', 'polling'],
      reconnectionDelay: 1000,
      reconnectionDelayMax: 10_000,
    });

    this.socket.on('connect', () => {
      if (this.mode !== 'lan') this.mode = 'cloud';
      void this.flushQueue();
      this.emit();
    });

    this.socket.on('disconnect', () => {
      if (this.mode !== 'lan') this.mode = 'offline';
      this.emit();
    });

    this.socket.on(
      'relay:changed',
      (evt: { deviceId: string; channel: number; state: boolean; source: string; rev: number }) => {
        this.applyRelayState(evt.deviceId, evt.channel, evt.state, evt.source, evt.rev);
      },
    );

    this.socket.on('device:presence', (evt: { deviceId: string; online: boolean }) => {
      const device = this.devices.find((d) => d.id === evt.deviceId);
      if (device) {
        device.online = evt.online;
        this.emit();
      }
    });
  }

  /** In cloud mode, detects whether a device is also reachable directly. */
  private async probeLanPath(): Promise<void> {
    if (this.direct) return;
    const first = this.devices[0];
    if (!first) return;

    const endpoints = await loadEndpoints();
    const ep = endpoints[first.uuid] ?? { uuid: first.uuid, lanIp: first.lastIp ?? undefined };

    const base = await probeLan(ep);
    const wasLan = this.mode === 'lan';

    if (base) {
      this.lanBase = base;
      this.lanUuid = first.uuid;
      this.mode = 'lan';
      if (!wasLan) void this.refreshLanState();
    } else {
      this.lanBase = null;
      this.lanUuid = null;
      this.mode = this.socket?.connected ? 'cloud' : 'offline';
    }
    this.emit();
  }

  private async refreshLanState(): Promise<void> {
    if (!this.lanBase || !this.lanUuid) return;
    try {
      const snapshot = await lan<{ channels: LocalChannel[] }>(this.lanBase, '/api/state');
      const device = this.devices.find((d) => d.uuid === this.lanUuid);
      if (!device) return;
      for (const c of snapshot.channels) {
        this.applyRelayState(device.id, c.channel, c.state, c.source, c.rev);
      }
    } catch {
      /* fall back to whatever the cloud last reported */
    }
  }

  // --- shared state --------------------------------------------------------

  private applyRelayState(
    deviceId: string,
    channel: number,
    state: boolean,
    source: string,
    rev: number,
  ): void {
    const device = this.devices.find((d) => d.id === deviceId) ?? this.devices[0];
    const relay = device?.relays.find((r) => r.channel === channel);
    if (!relay) return;

    // Revisions are monotonic and assigned by the device. Out-of-order messages
    // - which retained MQTT and reconnect snapshots produce routinely - must
    // not overwrite something newer.
    if (rev !== 0 && rev < relay.rev) return;

    relay.state = state;
    relay.source = source;
    relay.rev = rev;
    this.emit();
  }

  async refreshHomes(): Promise<void> {
    // Remote state is pushed by the broker's retained messages; there is
    // nothing to pull.
    if (this.remote) return;
    if (this.direct) return this.refreshDirectState();
    try {
      const me = await cloud<{ homes: Home[] }>('/api/auth/me');
      this.homes = me.homes;
      this.activeHomeId ??= me.homes[0]?.id ?? null;

      const status = await cloud<{ enabled: boolean }>('/api/ai/status').catch(() => ({
        enabled: false,
      }));
      this.aiEnabled = status.enabled;

      if (this.activeHomeId) await this.refreshDevices();
      this.setError(null);
    } catch (err) {
      this.setError(err instanceof ApiError ? err.message : 'Could not reach the server');
    }
  }

  async refreshDevices(): Promise<void> {
    if (this.remote) {
      // Nudge every known device to republish, for the rare case where a
      // retained message was missed.
      for (const [uuid, homeId] of this.homeOf) this.link?.requestSnapshot(homeId, uuid);
      return;
    }
    if (this.direct) return this.refreshDirectState();
    if (!this.activeHomeId) return;
    try {
      this.devices = await cloud<Device[]>(`/api/devices?homeId=${this.activeHomeId}`);
      // Cache each device's LAN address so direct control works next time even
      // when mDNS is unavailable, which it usually is inside a WebView.
      for (const d of this.devices) {
        if (d.lastIp) await rememberEndpoint({ uuid: d.uuid, lanIp: d.lastIp });
      }
      this.emit();
    } catch (err) {
      this.setError(err instanceof ApiError ? err.message : 'Could not load devices');
    }
  }

  // --- commands ------------------------------------------------------------

  async toggle(deviceId: string, channel: number): Promise<void> {
    const device = this.devices.find((d) => d.id === deviceId);
    const relay = device?.relays.find((r) => r.channel === channel);
    if (!device || !relay) return;

    const previous = relay.state;
    const action = previous ? 'off' : 'on';

    relay.state = !previous;
    relay.source = 'app';
    this.emit();

    try {
      await this.send(device, channel, action);
    } catch (err) {
      // Visibly revert. Leaving the wrong state on screen is worse than showing
      // that the command failed.
      relay.state = previous;
      this.emit();
      this.setError(err instanceof ApiError ? err.message : 'Command failed');
      this.queue.push({ deviceId, channel, action, at: Date.now() });
    }
  }

  async allOff(deviceId: string): Promise<void> {
    const device = this.devices.find((d) => d.id === deviceId);
    if (!device) return;
    try {
      await this.send(device, 'all', 'off');
    } catch (err) {
      this.setError(err instanceof ApiError ? err.message : 'Command failed');
    }
  }

  private async send(
    device: Device,
    channel: number | 'all',
    action: 'on' | 'off' | 'toggle',
  ): Promise<void> {
    if (this.remote && this.link) {
      const homeId = this.homeOf.get(device.uuid) ?? 'unclaimed';
      const relay = device.relays.find((r) => r.channel === channel);
      // Carry the revision so a command sent over a slow mobile link cannot
      // undo a wall-switch press that happened while it was in flight.
      const ok = this.link.publishCommand(
        homeId,
        device.uuid,
        channel,
        action,
        relay ? relay.rev + 1 : undefined,
      );
      if (!ok) throw new ApiError(0, 'Not connected to the broker');
      return;
    }

    if (this.direct && this.directBase) {
      await lan(this.directBase, `/api/relay/${channel}`, {
        method: 'POST',
        body: JSON.stringify({ action }),
      });
      return;
    }

    // Prefer the direct path even in cloud mode: it is faster, and it is the
    // only one that works when the internet is down.
    if (this.mode === 'lan' && this.lanBase && this.lanUuid === device.uuid) {
      const endpoints = await loadEndpoints();
      await lan(
        this.lanBase,
        `/api/relay/${channel}`,
        { method: 'POST', body: JSON.stringify({ action }) },
        endpoints[device.uuid]?.apiKey,
      );
      return;
    }

    await cloud(`/api/devices/${device.id}/relay/${channel}`, {
      method: 'POST',
      body: JSON.stringify({ action }),
    });
  }

  async rename(deviceId: string, channel: number, name: string): Promise<void> {
    if (this.direct && this.directBase) {
      await lan(this.directBase, '/api/config', {
        method: 'POST',
        body: JSON.stringify({ channels: [{ index: channel, name }] }),
      });
      await this.refreshDirectState();
      return;
    }
    await cloud(`/api/devices/${deviceId}/relay/${channel}`, {
      method: 'PATCH',
      body: JSON.stringify({ name }),
    });
    await this.refreshDevices();
  }

  async setRestore(deviceId: string, channel: number, restore: 'off' | 'on' | 'last'): Promise<void> {
    if (this.direct && this.directBase) {
      await lan(this.directBase, '/api/config', {
        method: 'POST',
        body: JSON.stringify({ channels: [{ index: channel, restore }] }),
      });
      return;
    }
    await cloud(`/api/devices/${deviceId}/relay/${channel}`, {
      method: 'PATCH',
      body: JSON.stringify({ restore }),
    });
  }

  /** Replays queued commands, newest-per-channel only. */
  private async flushQueue(): Promise<void> {
    if (this.queue.length === 0) return;

    const newest = new Map<string, QueuedCommand>();
    for (const cmd of this.queue) newest.set(`${cmd.deviceId}:${cmd.channel}`, cmd);
    this.queue = [];

    for (const cmd of newest.values()) {
      // Anything older than five minutes is stale intent, not a pending action.
      if (Date.now() - cmd.at > 300_000) continue;
      const device = this.devices.find((d) => d.id === cmd.deviceId);
      if (!device) continue;
      try {
        await this.send(device, cmd.channel, cmd.action);
      } catch {
        /* dropped - the next refresh shows the real state */
      }
    }
  }

  get queuedCount(): number {
    return this.queue.length;
  }
}

export const store = new Store();
