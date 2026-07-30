// ---------------------------------------------------------------------------
//  guard.ts - the layer that makes AI control safe to expose.
//
//  The model proposes; this decides. Every action is re-checked against the
//  real database and the user's real role before anything reaches a relay. If
//  the model hallucinates a uuid, or a prompt injection through a room name
//  produces an "turn everything on" action, it dies here rather than switching
//  a real appliance.
//
//  This is deliberately paranoid about things the model "shouldn't" get wrong.
//  Cheap to check; expensive to be wrong about, because the other end is mains
//  wiring in someone's house.
// ---------------------------------------------------------------------------
import type { Role } from '@prisma/client';

import { prisma } from '../core.js';
import type { IntentAction, Inventory } from './groq.js';

export interface ValidatedAction {
  action: IntentAction;
  deviceId: string;
  deviceUuid: string;
  homeId: string;
  /** Human-readable, for the confirmation the user sees. */
  describe: string;
}

export interface GuardResult {
  allowed: ValidatedAction[];
  rejected: { action: IntentAction; reason: string }[];
}

/**
 * Builds the inventory the model is allowed to see. Only channels in homes the
 * user is a member of, and only fields needed for matching - no api keys, no
 * ip addresses, no ids beyond what an action needs to reference.
 */
export async function buildInventory(homeId: string): Promise<Inventory> {
  const home = await prisma.home.findUnique({
    where: { id: homeId },
    include: {
      devices: {
        where: { claimed: true },
        include: { relays: { orderBy: { channel: 'asc' } }, room: true },
      },
      scenes: { select: { id: true, name: true } },
    },
  });
  if (!home) throw new Error('home not found');

  const channels = home.devices.flatMap((device) =>
    device.relays
      .filter((r) => r.enabled)
      .map((r) => ({
        deviceUuid: device.uuid,
        channel: r.channel,
        name: r.name,
        room: device.room?.name ?? null,
        state: r.state,
      })),
  );

  return {
    timezone: home.timezone,
    nowLocal: new Date().toISOString(),
    channels,
    scenes: home.scenes,
  };
}

/**
 * Re-validates proposed actions against the database. Nothing from the model is
 * trusted: not the uuid, not the channel number, not the fact that the channel
 * exists at all.
 */
export async function validateActions(
  homeId: string,
  role: Role,
  actions: IntentAction[],
): Promise<GuardResult> {
  const allowed: ValidatedAction[] = [];
  const rejected: { action: IntentAction; reason: string }[] = [];

  // GUEST may control existing devices but must not be able to arm timers that
  // outlive their session, and must never trigger a whole-device shutdown.
  const guestBlocked = new Set(['timer', 'allOff']);

  const devices = await prisma.device.findMany({
    where: { homeId, claimed: true },
    include: { relays: true },
  });
  const byUuid = new Map(devices.map((d) => [d.uuid, d]));

  const scenes = await prisma.scene.findMany({ where: { homeId }, select: { id: true } });
  const sceneIds = new Set(scenes.map((s) => s.id));

  for (const action of actions) {
    if (role === 'GUEST' && guestBlocked.has(action.type)) {
      rejected.push({ action, reason: 'guests cannot perform this action' });
      continue;
    }

    if (action.type === 'scene') {
      if (!sceneIds.has(action.sceneId)) {
        rejected.push({ action, reason: 'no such scene in this home' });
        continue;
      }
      allowed.push({
        action,
        deviceId: '',
        deviceUuid: '',
        homeId,
        describe: 'run a scene',
      });
      continue;
    }

    const device = byUuid.get(action.deviceUuid);
    if (!device) {
      // The most common hallucination, and the one that matters most: a uuid
      // belonging to a device in someone else's home.
      rejected.push({ action, reason: 'unknown device, or not in this home' });
      continue;
    }

    if (action.type === 'allOff') {
      allowed.push({
        action,
        deviceId: device.id,
        deviceUuid: device.uuid,
        homeId,
        describe: `turn off everything on ${device.name}`,
      });
      continue;
    }

    const relay = device.relays.find((r) => r.channel === action.channel);
    if (!relay) {
      rejected.push({ action, reason: `channel ${action.channel} does not exist on this device` });
      continue;
    }
    if (!relay.enabled) {
      rejected.push({ action, reason: `${relay.name} is disabled` });
      continue;
    }

    const describe =
      action.type === 'timer'
        ? `turn on ${relay.name} for ${Math.round(action.seconds / 60)} min`
        : `turn ${action.action} ${relay.name}`;

    allowed.push({ action, deviceId: device.id, deviceUuid: device.uuid, homeId, describe });
  }

  return { allowed, rejected };
}

/**
 * Actions that deserve an explicit "are you sure" rather than silent execution.
 *
 * The failure this prevents is specific: a misheard or mis-parsed request
 * switching a lot of mains appliances at once, at 3 a.m., because the model was
 * 60% sure. Confirming costs one tap; being wrong costs trust.
 */
export function needsConfirmation(
  result: GuardResult,
  confidence: 'high' | 'medium' | 'low',
): { required: boolean; reason?: string } {
  if (confidence === 'low') {
    return { required: true, reason: 'the request was ambiguous' };
  }
  if (result.allowed.some((a) => a.action.type === 'allOff')) {
    return { required: true, reason: 'this turns off every socket on a device' };
  }
  if (result.allowed.length > 4) {
    return { required: true, reason: `this changes ${result.allowed.length} sockets at once` };
  }
  if (confidence === 'medium' && result.allowed.some((a) => a.action.type !== 'relay')) {
    return { required: true, reason: 'the request was only partly clear' };
  }
  return { required: false };
}

/** Per-user, per-minute limiter. Free-tier quotas are per-minute and per-day. */
const buckets = new Map<string, { count: number; resetAt: number }>();

export function rateLimit(userId: string, perMinute: number): boolean {
  const now = Date.now();
  const bucket = buckets.get(userId);

  if (!bucket || now > bucket.resetAt) {
    buckets.set(userId, { count: 1, resetAt: now + 60_000 });
    return true;
  }
  if (bucket.count >= perMinute) return false;
  bucket.count++;
  return true;
}

/** Keeps the map from growing without bound on a long-lived process. */
export function pruneRateLimits(): void {
  const now = Date.now();
  for (const [key, bucket] of buckets) {
    if (now > bucket.resetAt) buckets.delete(key);
  }
}
