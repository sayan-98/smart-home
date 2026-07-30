// ---------------------------------------------------------------------------
//  transport.ts - the dual-path connection.
//
//  The app talks to the hardware two ways and picks automatically:
//
//    LAN   -> straight to the ESP32's own REST + WebSocket server.
//             Works with the internet down, and with the backend asleep on a
//             free tier. Lower latency, because nothing leaves the house.
//
//    CLOUD -> Render REST + Socket.IO.
//             For when you are not at home.
//
//  LAN is preferred whenever it is reachable, and the UI says which one is in
//  use so a slow response is never a mystery.
//
//  Probing is cheap and bounded: a 1.2 s timeout against the device's /api/info
//  endpoint. Android WebView cannot resolve mDNS .local names reliably, so a
//  device's last-known LAN IP is remembered and tried first.
// ---------------------------------------------------------------------------
import { Preferences } from '@capacitor/preferences';

export type Mode = 'lan' | 'remote' | 'cloud' | 'offline';

export interface DeviceEndpoint {
  uuid: string;
  /** Last IP the device was reachable at, e.g. "192.168.1.42". */
  lanIp?: string;
  /** Device API key, handed out once at claim time. */
  apiKey?: string;
}

const LAN_TIMEOUT_MS = 1200;
const KEY_ENDPOINTS = 'sh.endpoints';
const KEY_TOKEN = 'sh.token';
const KEY_API_BASE = 'sh.apiBase';
const KEY_DIRECT = 'sh.directBase';

// --- direct mode -----------------------------------------------------------
//
// One board on your own Wi-Fi does not need a cloud account. In direct mode the
// app skips sign-in entirely and talks to the ESP32's own REST + WebSocket
// server. No backend, no broker, no internet - and it keeps working when all
// three are down. The cloud path stays available for control from outside.

export async function getDirectBase(): Promise<string | null> {
  return (await Preferences.get({ key: KEY_DIRECT })).value;
}

export async function setDirectBase(base: string | null): Promise<void> {
  if (base) await Preferences.set({ key: KEY_DIRECT, value: base.replace(/\/+$/, '') });
  else await Preferences.remove({ key: KEY_DIRECT });
}

// --- broker (control from anywhere) ---------------------------------------

const KEY_BROKER = 'sh.broker';

export interface StoredBroker {
  host: string;
  port: number;
  username: string;
  password: string;
  path: string;
}

export async function getBroker(): Promise<StoredBroker | null> {
  const raw = (await Preferences.get({ key: KEY_BROKER })).value;
  if (!raw) return null;
  try {
    return JSON.parse(raw) as StoredBroker;
  } catch {
    return null;
  }
}

export async function setBroker(broker: StoredBroker | null): Promise<void> {
  if (broker) await Preferences.set({ key: KEY_BROKER, value: JSON.stringify(broker) });
  else await Preferences.remove({ key: KEY_BROKER });
}

// --- remembered sign-in details -------------------------------------------
//
// Broker hostnames are a 32-character hex blob, and digging one out of a
// dashboard every time you reinstall is a reason to stop using the thing. So
// whatever last worked is kept, and offered back with one tap.
//
// It lives in Capacitor Preferences, which is app-private storage on Android:
// other apps cannot read it, but anyone holding an unlocked tablet can open
// this app anyway. That is the same trade every phone password manager makes,
// and it is the right one for a household device. Nothing is ever sent
// anywhere, and nothing is ever committed to the repository.

const KEY_SAVED = 'sh.savedCreds';

export interface SavedCreds {
  directHint?: string;
  broker?: StoredBroker;
  account?: { base: string; email: string; password: string };
}

export async function getSavedCreds(): Promise<SavedCreds> {
  const raw = (await Preferences.get({ key: KEY_SAVED })).value;
  if (!raw) return {};
  try {
    return JSON.parse(raw) as SavedCreds;
  } catch {
    return {};
  }
}

/** Merges into whatever is already remembered. */
export async function rememberCreds(patch: SavedCreds): Promise<void> {
  const current = await getSavedCreds();
  const merged = { ...current, ...patch };
  await Preferences.set({ key: KEY_SAVED, value: JSON.stringify(merged) });
}

export async function forgetCreds(): Promise<void> {
  await Preferences.remove({ key: KEY_SAVED });
}

export interface DeviceInfoResponse {
  uuid: string;
  name: string;
  firmware: string;
  relayCount: number;
  hostname: string;
  wifiConnected: boolean;
  apActive: boolean;
  ssid: string;
  rssi: number;
  ip: string;
  claimed: boolean;
  claimCode: string;
  timeSynced: boolean;
  time: string;
}

/**
 * Finds a device on this network. Tries an explicit host first, then the mDNS
 * name, then the SoftAP address a freshly-flashed board sits on.
 *
 * mDNS often fails inside an Android WebView even when the device is right
 * there, which is why the raw IP is always offered as an alternative.
 */
export async function discoverDirect(
  hint?: string,
): Promise<{ base: string; info: DeviceInfoResponse } | null> {
  const candidates: string[] = [];
  if (hint?.trim()) {
    const h = hint.trim().replace(/^https?:\/\//, '').replace(/\/+$/, '');
    candidates.push(`http://${h}`);
  }
  candidates.push('http://smarthome.local', 'http://192.168.4.1');

  for (const base of candidates) {
    try {
      const res = await timedFetch(`${base}/api/info`, { method: 'GET' }, LAN_TIMEOUT_MS);
      if (!res.ok) continue;
      const info = (await res.json()) as DeviceInfoResponse;
      if (info.uuid) return { base, info };
    } catch {
      // Not at this address; try the next.
    }
  }
  return null;
}

// --- persisted settings ----------------------------------------------------

export async function getToken(): Promise<string | null> {
  return (await Preferences.get({ key: KEY_TOKEN })).value;
}

export async function setToken(token: string | null): Promise<void> {
  if (token) await Preferences.set({ key: KEY_TOKEN, value: token });
  else await Preferences.remove({ key: KEY_TOKEN });
}

export async function getApiBase(): Promise<string> {
  const stored = (await Preferences.get({ key: KEY_API_BASE })).value;
  return stored ?? import.meta.env.VITE_API_BASE ?? '';
}

export async function setApiBase(url: string): Promise<void> {
  await Preferences.set({ key: KEY_API_BASE, value: url.replace(/\/+$/, '') });
}

export async function loadEndpoints(): Promise<Record<string, DeviceEndpoint>> {
  const raw = (await Preferences.get({ key: KEY_ENDPOINTS })).value;
  if (!raw) return {};
  try {
    return JSON.parse(raw) as Record<string, DeviceEndpoint>;
  } catch {
    return {};
  }
}

export async function rememberEndpoint(ep: DeviceEndpoint): Promise<void> {
  const all = await loadEndpoints();
  all[ep.uuid] = { ...all[ep.uuid], ...ep };
  await Preferences.set({ key: KEY_ENDPOINTS, value: JSON.stringify(all) });
}

// --- fetch with a timeout --------------------------------------------------

async function timedFetch(url: string, init: RequestInit, ms: number): Promise<Response> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), ms);
  try {
    return await fetch(url, { ...init, signal: controller.signal });
  } finally {
    clearTimeout(timer);
  }
}

// --- LAN probing -----------------------------------------------------------

/**
 * Is this device reachable directly right now?
 *
 * Tries the remembered IP first, then the mDNS name. The mDNS attempt often
 * fails inside an Android WebView even when the device is right there, which
 * is exactly why the IP is cached from the last successful cloud sync.
 */
export async function probeLan(ep: DeviceEndpoint): Promise<string | null> {
  const candidates = [
    ep.lanIp ? `http://${ep.lanIp}` : null,
    `http://smarthome-${ep.uuid.slice(-4)}.local`,
  ].filter(Boolean) as string[];

  for (const base of candidates) {
    try {
      const res = await timedFetch(`${base}/api/info`, { method: 'GET' }, LAN_TIMEOUT_MS);
      if (!res.ok) continue;
      const info = (await res.json()) as { uuid?: string };
      // Guard against another device answering on a recycled DHCP lease.
      if (info.uuid === ep.uuid) return base;
    } catch {
      // Unreachable on this candidate; try the next.
    }
  }
  return null;
}

// --- unified request -------------------------------------------------------

export class ApiError extends Error {
  constructor(
    public status: number,
    message: string,
  ) {
    super(message);
  }
}

async function parseError(res: Response): Promise<never> {
  let message = `HTTP ${res.status}`;
  try {
    const body = (await res.json()) as { error?: string };
    if (body.error) message = body.error;
  } catch {
    /* keep the status-code message */
  }
  throw new ApiError(res.status, message);
}

/** Cloud call, against the Render backend. */
export async function cloud<T>(path: string, init: RequestInit = {}): Promise<T> {
  const base = await getApiBase();
  if (!base) throw new ApiError(0, 'No server configured');

  const token = await getToken();
  const res = await timedFetch(
    `${base}${path}`,
    {
      ...init,
      headers: {
        'Content-Type': 'application/json',
        ...(token ? { Authorization: `Bearer ${token}` } : {}),
        ...init.headers,
      },
    },
    // Render's free tier cold-starts in 30-60 s. A short timeout here would
    // show a spurious failure on the first request after it sleeps.
    20000,
  );

  if (!res.ok) await parseError(res);
  return res.status === 204 ? (null as T) : ((await res.json()) as T);
}

/** Direct LAN call, against the ESP32 itself. */
export async function lan<T>(
  base: string,
  path: string,
  init: RequestInit = {},
  apiKey?: string,
): Promise<T> {
  const res = await timedFetch(
    `${base}${path}`,
    {
      ...init,
      headers: {
        'Content-Type': 'application/json',
        ...(apiKey ? { 'X-API-Key': apiKey } : {}),
        ...init.headers,
      },
    },
    5000,
  );
  if (!res.ok) await parseError(res);
  return res.status === 204 ? (null as T) : ((await res.json()) as T);
}
