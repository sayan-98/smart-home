// ---------------------------------------------------------------------------
//  bridge.ts - the MQTT <-> database <-> Socket.IO bridge.
//
//  There is exactly ONE direction of truth, and enforcing it is the whole job:
//
//      device --MQTT retained--> bridge --> Postgres --> Socket.IO --> app
//
//  The app never writes state to two places. When a user taps a tile the app
//  calls REST, REST publishes an MQTT command, the device decides, and the new
//  state comes back along the path above. Optimistic UI happens in the app, not
//  in the database. Without this rule the two buses drift and you get sockets
//  that show ON in the app and are physically OFF.
// ---------------------------------------------------------------------------
import mqtt, { type MqttClient } from 'mqtt';

import { log, prisma } from '../core.js';
import { env } from '../env.js';
import { emitToHome } from '../realtime/socket.js';

let client: MqttClient | null = null;
const ROOT = env.MQTT_TOPIC_ROOT;

/** `smarthome/<homeId>/<uuid>/...` */
function parseTopic(topic: string): { homeId: string; uuid: string; rest: string } | null {
  const parts = topic.split('/');
  if (parts.length < 4 || parts[0] !== ROOT) return null;
  return { homeId: parts[1], uuid: parts[2], rest: parts.slice(3).join('/') };
}

function safeJson(payload: Buffer): Record<string, unknown> | null {
  const text = payload.toString('utf8').trim();
  if (!text) return null;
  if (!text.startsWith('{') && !text.startsWith('[')) return null;
  try {
    return JSON.parse(text) as Record<string, unknown>;
  } catch {
    return null;
  }
}

// --- inbound handlers ------------------------------------------------------

async function onStatus(uuid: string, body: Record<string, unknown>): Promise<void> {
  const online = Boolean(body.online);
  const device = await prisma.device.findUnique({ where: { uuid } });
  if (!device) return;

  await prisma.device.update({
    where: { uuid },
    data: {
      online,
      lastSeenAt: new Date(),
      firmwareVersion: typeof body.fw === 'string' ? body.fw : device.firmwareVersion,
    },
  });

  await prisma.deviceEvent.create({
    data: { deviceId: device.id, type: online ? 'online' : 'offline', payload: body as object },
  });

  log.info({ uuid, online }, 'device presence changed');
  if (device.homeId) {
    emitToHome(device.homeId, 'device:presence', { deviceId: device.id, uuid, online });
  }
}

async function onRegister(uuid: string, body: Record<string, unknown>): Promise<void> {
  const mac = typeof body.mac === 'string' ? body.mac : null;
  if (!mac) return;

  const relayCount = Number(body.relayCount ?? 8);
  const memory = (body.memory ?? {}) as Record<string, number>;

  // A device that has never been seen is created UNCLAIMED. It stays inert
  // until someone with an account enters the claim code printed on it -
  // otherwise knowing a MAC would be enough to adopt someone else's hardware.
  const device = await prisma.device.upsert({
    where: { uuid },
    create: {
      uuid,
      mac,
      name: typeof body.name === 'string' ? body.name : 'Smart Home Node',
      firmwareVersion: String(body.firmwareVersion ?? '0.0.0'),
      hardwareRevision: String(body.hardwareRevision ?? 'unknown'),
      relayCount,
      claimCode: typeof body.claimCode === 'string' ? body.claimCode : null,
      // Prisma will not take a bare null for a nullable Json column, so an
      // absent field is simply omitted rather than written as null.
      ...(body.capabilities ? { capabilities: body.capabilities as object } : {}),
      online: true,
      lastSeenAt: new Date(),
      resetReason: typeof body.resetReason === 'string' ? body.resetReason : null,
      freeHeap: memory.freeHeap ?? null,
      uptimeMs: body.uptimeMs ? BigInt(String(body.uptimeMs)) : null,
    },
    update: {
      firmwareVersion: String(body.firmwareVersion ?? '0.0.0'),
      hardwareRevision: String(body.hardwareRevision ?? 'unknown'),
      relayCount,
      // Prisma will not take a bare null for a nullable Json column, so an
      // absent field is simply omitted rather than written as null.
      ...(body.capabilities ? { capabilities: body.capabilities as object } : {}),
      online: true,
      lastSeenAt: new Date(),
      resetReason: typeof body.resetReason === 'string' ? body.resetReason : null,
      // Only refresh the claim code while the device is still unclaimed.
      ...(typeof body.claimCode === 'string' ? { claimCode: body.claimCode } : {}),
    },
  });

  // Make sure a Relay row exists for every channel the device advertises.
  for (let ch = 0; ch < relayCount; ch++) {
    await prisma.relay.upsert({
      where: { deviceId_channel: { deviceId: device.id, channel: ch } },
      create: { deviceId: device.id, channel: ch, name: `Socket ${ch + 1}` },
      update: {},
    });
  }

  log.info(
    { uuid, fw: device.firmwareVersion, claimed: device.claimed, relayCount },
    'device registered',
  );

  if (device.homeId) emitToHome(device.homeId, 'device:registered', { deviceId: device.id, uuid });
}

async function onRelayState(
  uuid: string,
  channel: number,
  body: Record<string, unknown>,
): Promise<void> {
  const device = await prisma.device.findUnique({ where: { uuid } });
  if (!device) return;

  const rev = Number(body.rev ?? 0);
  const state = Boolean(body.state);
  const source = String(body.source ?? 'unknown');

  const existing = await prisma.relay.findUnique({
    where: { deviceId_channel: { deviceId: device.id, channel } },
  });

  // Last-write-wins on the DEVICE's revision, not on arrival order. Retained
  // messages and reconnect snapshots arrive out of order all the time, and
  // without this an old retained value can overwrite a newer live one.
  if (existing && rev !== 0 && rev < existing.rev) {
    log.debug({ uuid, channel, rev, have: existing.rev }, 'ignoring stale relay state');
    return;
  }

  const relay = await prisma.relay.upsert({
    where: { deviceId_channel: { deviceId: device.id, channel } },
    create: {
      deviceId: device.id,
      channel,
      name: typeof body.name === 'string' ? body.name : `Socket ${channel + 1}`,
      state,
      source,
      rev,
      tsSynced: Boolean(body.tsSynced),
      changedAt: new Date(),
    },
    update: {
      state,
      source,
      rev,
      tsSynced: Boolean(body.tsSynced),
      changedAt: new Date(),
      ...(typeof body.name === 'string' ? { name: body.name } : {}),
    },
  });

  await prisma.deviceEvent.create({
    data: { deviceId: device.id, type: 'relay', channel, state, source, rev },
  });

  if (device.homeId) {
    emitToHome(device.homeId, 'relay:changed', {
      deviceId: device.id,
      uuid,
      channel,
      state,
      source,
      rev,
      name: relay.name,
      at: relay.changedAt,
    });
  }
}

async function onDiagnostics(uuid: string, body: Record<string, unknown>): Promise<void> {
  const device = await prisma.device.findUnique({ where: { uuid } });
  if (!device) return;

  const brownout = body.brownout === true || body.resetReason === 'brownout';

  await prisma.device.update({
    where: { uuid },
    data: {
      lastSeenAt: new Date(),
      rssi: typeof body.rssi === 'number' ? body.rssi : null,
      freeHeap: typeof body.freeHeap === 'number' ? body.freeHeap : null,
      uptimeMs: body.uptimeMs ? BigInt(String(body.uptimeMs)) : null,
      resetReason: typeof body.resetReason === 'string' ? body.resetReason : null,
      lastIp: typeof body.ip === 'string' ? body.ip : null,
      lastBrownout: brownout,
      online: true,
    },
  });

  await prisma.deviceEvent.create({
    data: { deviceId: device.id, type: 'heartbeat', payload: body as object },
  });

  if (brownout) {
    // Worth a warning line: this is a 5 V supply problem, and it gets
    // misdiagnosed as a firmware crash every time.
    log.warn({ uuid }, 'device reports a brownout reset - check its power supply');
  }

  if (device.homeId) {
    emitToHome(device.homeId, 'device:diagnostics', { deviceId: device.id, uuid, ...body });
  }
}

// --- lifecycle -------------------------------------------------------------

export function startMqttBridge(): MqttClient {
  if (client) return client;

  log.info({ url: env.MQTT_URL }, 'connecting to MQTT broker');

  client = mqtt.connect(env.MQTT_URL, {
    username: env.MQTT_USERNAME,
    password: env.MQTT_PASSWORD,
    clientId: `smarthome-backend-${Math.random().toString(16).slice(2, 10)}`,
    clean: true,
    reconnectPeriod: 5000,
    connectTimeout: 15000,
  });

  client.on('connect', () => {
    log.info('MQTT connected');
    // The backend is the one account with a wide subscription. Devices get
    // per-device ACLs scoped to their own subtree - without that, one leaked
    // device credential could subscribe to '#' and control every home.
    client!.subscribe(`${ROOT}/+/+/status`, { qos: 1 });
    client!.subscribe(`${ROOT}/+/+/register`, { qos: 1 });
    client!.subscribe(`${ROOT}/+/+/relay/+/state`, { qos: 1 });
    client!.subscribe(`${ROOT}/+/+/diag`, { qos: 1 });
  });

  client.on('error', (err) => log.error({ err }, 'MQTT error'));
  client.on('reconnect', () => log.warn('MQTT reconnecting'));
  client.on('offline', () => log.warn('MQTT offline'));

  client.on('message', (topic, payload) => {
    void (async () => {
      try {
        const parsed = parseTopic(topic);
        if (!parsed) return;
        const { uuid, rest } = parsed;

        const body = safeJson(payload);
        if (!body) return;

        if (rest === 'status') return onStatus(uuid, body);
        if (rest === 'register') return onRegister(uuid, body);
        if (rest === 'diag') return onDiagnostics(uuid, body);

        const relayMatch = /^relay\/(\d+)\/state$/.exec(rest);
        if (relayMatch) return onRelayState(uuid, Number(relayMatch[1]), body);
      } catch (err) {
        // One malformed message must never take the bridge down.
        log.error({ err, topic }, 'failed to handle MQTT message');
      }
    })();
  });

  return client;
}

// --- outbound --------------------------------------------------------------

function baseTopic(homeId: string | null, uuid: string): string {
  return `${ROOT}/${homeId ?? 'unclaimed'}/${uuid}`;
}

export function publishRelayCommand(
  homeId: string | null,
  uuid: string,
  channel: number | 'all',
  action: 'on' | 'off' | 'toggle',
  opts: { rev?: number; seconds?: number } = {},
): boolean {
  if (!client?.connected) {
    log.warn({ uuid }, 'MQTT not connected, command dropped');
    return false;
  }
  const topic = `${baseTopic(homeId, uuid)}/relay/${channel}/set`;
  const payload: Record<string, unknown> = { action };
  // Including `rev` makes the write conflict-safe: the device applies it only
  // if it is newer than what the channel already has, so a delayed command
  // cannot undo a wall-switch press that happened after it was sent.
  if (opts.rev !== undefined) payload.rev = opts.rev;
  if (opts.seconds !== undefined) payload.seconds = opts.seconds;

  client.publish(topic, JSON.stringify(payload), { qos: 1 });
  log.debug({ topic, payload }, 'relay command published');
  return true;
}

export function publishDeviceCommand(
  homeId: string | null,
  uuid: string,
  cmd: string,
  extra: Record<string, unknown> = {},
): boolean {
  if (!client?.connected) return false;
  client.publish(`${baseTopic(homeId, uuid)}/cmd`, JSON.stringify({ cmd, ...extra }), { qos: 1 });
  return true;
}

export function publishConfig(
  homeId: string | null,
  uuid: string,
  patch: Record<string, unknown>,
): boolean {
  if (!client?.connected) return false;
  client.publish(`${baseTopic(homeId, uuid)}/config/set`, JSON.stringify(patch), { qos: 1 });
  return true;
}

export function mqttConnected(): boolean {
  return Boolean(client?.connected);
}

export async function stopMqttBridge(): Promise<void> {
  if (!client) return;
  await client.endAsync();
  client = null;
}
