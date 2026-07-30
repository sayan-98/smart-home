// ---------------------------------------------------------------------------
//  routes/devices.ts - claiming, control, configuration, schedules, rules.
//
//  Every mutating route resolves the device through authorizeDevice(), which
//  checks that the caller is a member of the home the device belongs to and
//  has a sufficient role. There is no path here that touches a relay without
//  going through it.
// ---------------------------------------------------------------------------
import { Router } from 'express';
import { z } from 'zod';

import {
  HttpError,
  audit,
  authorizeDevice,
  generateDeviceApiKey,
  handler,
  hashApiKey,
  prisma,
  requireAuth,
  requireHomeRole,
  safeEqual,
} from '../core.js';
import {
  publishConfig,
  publishDeviceCommand,
  publishRelayCommand,
} from '../mqtt/bridge.js';

export const deviceRouter = Router();
deviceRouter.use(requireAuth);

// --- listing ---------------------------------------------------------------

deviceRouter.get(
  '/',
  handler(async (req, res) => {
    const homeId = z.string().min(1).parse(req.query.homeId);
    await requireHomeRole(req.userId!, homeId, 'GUEST');

    const devices = await prisma.device.findMany({
      where: { homeId },
      include: { relays: { orderBy: { channel: 'asc' } }, room: true },
      orderBy: { createdAt: 'asc' },
    });
    res.json(devices);
  }),
);

deviceRouter.get(
  '/:id',
  handler(async (req, res) => {
    res.json(await authorizeDevice(req.userId!, req.params.id));
  }),
);

// --- claiming --------------------------------------------------------------

/**
 * Adoption. The device self-registers over MQTT but stays inert until this
 * runs: without the claim code printed on the enclosure, knowing a MAC would be
 * enough to take over someone else's hardware.
 *
 * The API key is returned exactly once, here. Only its hash is stored.
 */
deviceRouter.post(
  '/claim',
  handler(async (req, res) => {
    const body = z
      .object({
        uuid: z.string().min(4).max(64),
        claimCode: z.string().length(8),
        homeId: z.string().min(1),
        roomId: z.string().optional(),
        name: z.string().max(60).optional(),
      })
      .parse(req.body);

    await requireHomeRole(req.userId!, body.homeId, 'ADMIN');

    const device = await prisma.device.findUnique({ where: { uuid: body.uuid } });
    if (!device) throw new HttpError(404, 'device not found - power it on and wait for it to register');

    if (device.claimed && device.homeId !== body.homeId) {
      throw new HttpError(409, 'this device is already claimed by another home');
    }
    if (!device.claimCode || !safeEqual(body.claimCode.toUpperCase(), device.claimCode)) {
      // A deliberate delay: the claim code is short enough to be worth guessing.
      await new Promise((r) => setTimeout(r, 700));
      throw new HttpError(403, 'wrong claim code');
    }

    const apiKey = generateDeviceApiKey();

    const updated = await prisma.device.update({
      where: { uuid: body.uuid },
      data: {
        claimed: true,
        claimedAt: new Date(),
        homeId: body.homeId,
        roomId: body.roomId,
        name: body.name ?? device.name,
        apiKeyHash: await hashApiKey(apiKey),
        // Per-device broker credentials, so its ACL can be scoped to its own
        // subtree. A shared credential would let one leaked device control
        // every home.
        mqttUsername: `dev_${device.uuid}`,
        claimCode: null,
      },
      include: { relays: { orderBy: { channel: 'asc' } } },
    });

    await audit({
      homeId: body.homeId,
      userId: req.userId,
      action: 'device.claim',
      target: device.uuid,
      ip: req.ip,
    });

    res.json({
      device: updated,
      // Shown once. The app forwards it to the device over the LAN
      // (POST /api/claim) and then stores it for direct local control.
      apiKey,
      mqttUsername: updated.mqttUsername,
    });
  }),
);

deviceRouter.post(
  '/:id/unclaim',
  handler(async (req, res) => {
    const device = await authorizeDevice(req.userId!, req.params.id, 'ADMIN');

    // Decommissioning matters: a device that keeps its credentials after being
    // sold or returned is a live account in someone else's house.
    await prisma.device.update({
      where: { id: device.id },
      data: { claimed: false, apiKeyHash: null, mqttUsername: null, homeId: null, roomId: null },
    });
    publishDeviceCommand(device.homeId, device.uuid, 'factory-reset');

    await audit({
      homeId: device.homeId,
      userId: req.userId,
      action: 'device.unclaim',
      target: device.uuid,
      ip: req.ip,
    });
    res.json({ ok: true });
  }),
);

// --- control ---------------------------------------------------------------

const relayCommand = z.object({
  action: z.enum(['on', 'off', 'toggle']),
  seconds: z.number().int().min(1).max(86400).optional(),
});

deviceRouter.post(
  '/:id/relay/:channel',
  handler(async (req, res) => {
    const device = await authorizeDevice(req.userId!, req.params.id, 'GUEST');
    const channel = z.coerce.number().int().min(0).max(31).parse(req.params.channel);
    const body = relayCommand.parse(req.body);

    const relay = device.relays.find((r) => r.channel === channel);
    if (!relay) throw new HttpError(404, 'no such channel');
    if (!relay.enabled) throw new HttpError(409, `${relay.name} is disabled`);

    // Send the revision we believe is current. The device applies the command
    // only if it is newer, so a slow round trip cannot undo a wall-switch press
    // that happened in the meantime.
    const sent = publishRelayCommand(device.homeId, device.uuid, channel, body.action, {
      rev: relay.rev + 1,
      seconds: body.seconds,
    });
    if (!sent) throw new HttpError(503, 'the message broker is unreachable');

    await audit({
      homeId: device.homeId,
      userId: req.userId,
      action: 'relay.command',
      target: `${device.uuid}:${channel}`,
      detail: body,
      ip: req.ip,
    });

    // Deliberately NOT writing state here. The device decides, and the new
    // state arrives back through MQTT -> bridge -> Socket.IO. Writing it here
    // too is how the app and the hardware end up disagreeing.
    res.status(202).json({ accepted: true, channel, action: body.action });
  }),
);

deviceRouter.post(
  '/:id/relay/all',
  handler(async (req, res) => {
    const device = await authorizeDevice(req.userId!, req.params.id, 'MEMBER');
    const body = relayCommand.parse(req.body);
    if (!publishRelayCommand(device.homeId, device.uuid, 'all', body.action)) {
      throw new HttpError(503, 'the message broker is unreachable');
    }
    res.status(202).json({ accepted: true, action: body.action });
  }),
);

// --- naming and configuration ---------------------------------------------

deviceRouter.patch(
  '/:id/relay/:channel',
  handler(async (req, res) => {
    const device = await authorizeDevice(req.userId!, req.params.id, 'MEMBER');
    const channel = z.coerce.number().int().min(0).max(31).parse(req.params.channel);
    const body = z
      .object({
        name: z.string().min(1).max(23).optional(),
        icon: z.string().max(15).optional(),
        roomId: z.string().nullable().optional(),
        groupId: z.number().int().min(0).max(255).optional(),
        enabled: z.boolean().optional(),
        // Deliberately NOT stored here. Restore behaviour has to work when the
        // device boots with no network at all, so the device owns it and this
        // only forwards it.
        restore: z.enum(['off', 'on', 'last']).optional(),
      })
      .parse(req.body);

    const { restore, ...persisted } = body;

    const relay = await prisma.relay.update({
      where: { deviceId_channel: { deviceId: device.id, channel } },
      data: persisted,
    });

    // Mirror to the device so the name is right in the Alexa/Hue listing and on
    // the device's own web page, not just in the app.
    if (Object.keys(body).length > 0) {
      publishConfig(device.homeId, device.uuid, {
        channels: [
          {
            index: channel,
            ...(body.name ? { name: body.name } : {}),
            ...(body.icon ? { icon: body.icon } : {}),
            ...(body.enabled !== undefined ? { enabled: body.enabled } : {}),
            ...(body.groupId !== undefined ? { group: body.groupId } : {}),
            ...(restore ? { restore } : {}),
          },
        ],
      });
    }

    res.json(relay);
  }),
);

deviceRouter.patch(
  '/:id',
  handler(async (req, res) => {
    const device = await authorizeDevice(req.userId!, req.params.id, 'MEMBER');
    const body = z
      .object({
        name: z.string().min(1).max(60).optional(),
        roomId: z.string().nullable().optional(),
      })
      .parse(req.body);

    const updated = await prisma.device.update({ where: { id: device.id }, data: body });
    if (body.name) publishConfig(device.homeId, device.uuid, { device: { name: body.name } });
    res.json(updated);
  }),
);

// --- schedules and automations --------------------------------------------

const scheduleInput = z.object({
  name: z.string().min(1).max(40),
  enabled: z.boolean().default(true),
  channels: z.array(z.number().int().min(0).max(31)).min(1),
  action: z.enum(['on', 'off', 'toggle']),
  minute: z.number().int().min(0).max(1439),
  days: z.number().int().min(1).max(127).default(127),
  catchUp: z.boolean().default(false),
});

deviceRouter.get(
  '/:id/schedules',
  handler(async (req, res) => {
    const device = await authorizeDevice(req.userId!, req.params.id);
    res.json(await prisma.schedule.findMany({ where: { deviceId: device.id } }));
  }),
);

/**
 * Replaces the whole set, then pushes it to the device. The device is what
 * actually runs them - that is why a schedule keeps firing when this backend is
 * asleep on a free tier and the internet is out.
 */
deviceRouter.put(
  '/:id/schedules',
  handler(async (req, res) => {
    const device = await authorizeDevice(req.userId!, req.params.id, 'MEMBER');
    const items = z.array(scheduleInput).max(24).parse(req.body);

    await prisma.$transaction([
      prisma.schedule.deleteMany({ where: { deviceId: device.id } }),
      prisma.schedule.createMany({
        data: items.map((s) => ({ ...s, deviceId: device.id })),
      }),
    ]);

    const stored = await prisma.schedule.findMany({ where: { deviceId: device.id } });
    publishConfig(device.homeId, device.uuid, {
      schedules: stored.map((s) => ({
        id: s.id.slice(0, 19),
        enabled: s.enabled,
        channels: s.channels,
        action: s.action,
        minute: s.minute,
        days: s.days,
        catchUp: s.catchUp,
      })),
    });

    await audit({
      homeId: device.homeId,
      userId: req.userId,
      action: 'schedules.replace',
      target: device.uuid,
      detail: { count: stored.length },
      ip: req.ip,
    });
    res.json(stored);
  }),
);

deviceRouter.get(
  '/:id/automations',
  handler(async (req, res) => {
    const device = await authorizeDevice(req.userId!, req.params.id);
    res.json(await prisma.automation.findMany({ where: { deviceId: device.id } }));
  }),
);

// --- maintenance -----------------------------------------------------------

deviceRouter.post(
  '/:id/command',
  handler(async (req, res) => {
    const device = await authorizeDevice(req.userId!, req.params.id, 'ADMIN');
    const { cmd } = z
      .object({ cmd: z.enum(['snapshot', 'diag', 'identify', 'reboot', 'ota']) })
      .parse(req.body);

    if (!publishDeviceCommand(device.homeId, device.uuid, cmd)) {
      throw new HttpError(503, 'the message broker is unreachable');
    }
    await audit({
      homeId: device.homeId,
      userId: req.userId,
      action: `device.${cmd}`,
      target: device.uuid,
      ip: req.ip,
    });
    res.status(202).json({ accepted: true, cmd });
  }),
);

deviceRouter.get(
  '/:id/events',
  handler(async (req, res) => {
    const device = await authorizeDevice(req.userId!, req.params.id);
    const limit = z.coerce.number().int().min(1).max(200).default(50).parse(req.query.limit ?? 50);
    res.json(
      await prisma.deviceEvent.findMany({
        where: { deviceId: device.id },
        orderBy: { at: 'desc' },
        take: limit,
      }),
    );
  }),
);
