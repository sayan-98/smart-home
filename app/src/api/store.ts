// ---------------------------------------------------------------------------
//  store.ts - app state, connection management and the outbound command queue.
//
//  Two behaviours here earn their keep:
//
//  1. OPTIMISTIC UI WITH RECONCILIATION. Tapping a tile flips it immediately,
//     because a switch that waits 300 ms for a round trip feels broken. The
//     real state then arrives over the socket and overwrites the guess. If the
//     command failed, the tile flips back - visibly, so you know it did not
//     happen rather than being quietly lied to.
//
//  2. A COMMAND QUEUE. Commands issued while disconnected are held and replayed
//     on reconnect, newest-per-channel only. Replaying three stale toggles for
//     one socket would be worse than dropping two of them.
// ---------------------------------------------------------------------------
import { Network } from '@capacitor/network';
import { io, type Socket } from 'socket.io-client';

import {
  ApiError,
  cloud,
  getApiBase,
  getToken,
  lan,
  loadEndpoints,
  probeLan,
  rememberEndpoint,
  type Mode,
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

class Store {
  mode: Mode = 'offline';
  lanBase: string | null = null;
  lanUuid: string | null = null;

  homes: Home[] = [];
  activeHomeId: string | null = null;
  devices: Device[] = [];

  aiEnabled = false;
  error: string | null = null;
  busy = false;

  private socket: Socket | null = null;
  private listeners = new Set<Listener>();
  private queue: QueuedCommand[] = [];
  private probeTimer: ReturnType<typeof setInterval> | null = null;

  subscribe(fn: Listener): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  private emit(): void {
    for (const fn of this.listeners) fn();
  }

  // --- lifecycle -----------------------------------------------------------

  async init(): Promise<void> {
    const token = await getToken();
    if (!token) {
      this.mode = 'offline';
      this.emit();
      return;
    }

    await this.refreshHomes();
    await this.connectSocket();

    // Re-probe the LAN periodically and on any network change: walking in the
    // front door should switch the app to direct control without a restart.
    this.probeTimer = setInterval(() => void this.probeLanPath(), 30_000);
    void Network.addListener('networkStatusChange', () => void this.probeLanPath());
    await this.probeLanPath();
  }

  dispose(): void {
    if (this.probeTimer) clearInterval(this.probeTimer);
    this.socket?.disconnect();
    this.socket = null;
  }

  private setError(message: string | null): void {
    this.error = message;
    this.emit();
  }

  clearError(): void {
    this.setError(null);
  }

  // --- connection ----------------------------------------------------------

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

    // The authoritative update. Whatever the UI guessed, this is the truth.
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

  /** Detects whether we can reach a device directly on this network. */
  private async probeLanPath(): Promise<void> {
    const endpoints = await loadEndpoints();
    const first = this.devices[0];
    if (!first) return;

    const ep = endpoints[first.uuid] ?? {
      uuid: first.uuid,
      lanIp: first.lastIp ?? undefined,
    };

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

  // --- state ---------------------------------------------------------------

  private applyRelayState(
    deviceId: string,
    channel: number,
    state: boolean,
    source: string,
    rev: number,
  ): void {
    const device = this.devices.find((d) => d.id === deviceId);
    const relay = device?.relays.find((r) => r.channel === channel);
    if (!relay) return;

    // Revisions are monotonic and assigned by the device. An out-of-order
    // message - which retained MQTT and reconnect snapshots produce routinely -
    // must not overwrite something newer.
    if (rev !== 0 && rev < relay.rev) return;

    relay.state = state;
    relay.source = source;
    relay.rev = rev;
    this.emit();
  }

  async refreshHomes(): Promise<void> {
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
    if (!this.activeHomeId) return;
    try {
      this.devices = await cloud<Device[]>(`/api/devices?homeId=${this.activeHomeId}`);

      // Cache each device's LAN address so direct control works next time even
      // if mDNS is unavailable, which it usually is inside a WebView.
      for (const d of this.devices) {
        if (d.lastIp) await rememberEndpoint({ uuid: d.uuid, lanIp: d.lastIp });
      }
      this.emit();
    } catch (err) {
      this.setError(err instanceof ApiError ? err.message : 'Could not load devices');
    }
  }

  /** In LAN mode the device itself is the source of truth. */
  private async refreshLanState(): Promise<void> {
    if (!this.lanBase || !this.lanUuid) return;
    try {
      const snapshot = await lan<{ channels: { channel: number; state: boolean; source: string; rev: number }[] }>(
        this.lanBase,
        '/api/state',
      );
      const device = this.devices.find((d) => d.uuid === this.lanUuid);
      if (!device) return;
      for (const ch of snapshot.channels) {
        this.applyRelayState(device.id, ch.channel, ch.state, ch.source, ch.rev);
      }
    } catch {
      // Fall back to whatever the cloud last told us.
    }
  }

  // --- commands ------------------------------------------------------------

  async toggle(deviceId: string, channel: number): Promise<void> {
    const device = this.devices.find((d) => d.id === deviceId);
    const relay = device?.relays.find((r) => r.channel === channel);
    if (!device || !relay) return;

    const previous = relay.state;
    const action = previous ? 'off' : 'on';

    // Optimistic: flip now, reconcile when the truth arrives.
    relay.state = !previous;
    relay.source = 'app';
    this.emit();

    try {
      await this.send(device, channel, action);
    } catch (err) {
      // Visibly revert. Silently leaving the wrong state on screen is worse
      // than showing that the command failed.
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
    // Prefer the direct path. It is faster, and it is the only one that works
    // when the internet is down.
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

  /**
   * Replays queued commands, newest-per-channel only. Replaying three stale
   * toggles for one socket would be worse than dropping two of them.
   */
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

  get activeDevices(): Device[] {
    return this.devices;
  }
}

export const store = new Store();
