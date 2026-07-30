// ---------------------------------------------------------------------------
//  routes/ota.ts - the update manifest devices poll, plus release management.
//
//  The manifest endpoint is the ONE route a device calls without a user token,
//  so it is deliberately narrow: it reads query parameters, checks a device
//  exists, and returns a version and a URL. It mutates nothing.
//
//  Staged rollout is not a nicety. Pushing a bad build to an entire fleet at
//  once is a self-inflicted outage with no remote fix - every device would need
//  physically reflashing. `rolloutPercent` bounds the blast radius, and the
//  bucket is derived from the device uuid so a given device stays in or out of
//  a wave consistently instead of flapping between polls.
// ---------------------------------------------------------------------------
import crypto from 'node:crypto';
import { Router } from 'express';
import { z } from 'zod';

import { HttpError, handler, log, prisma, requireAuth } from '../core.js';

export const otaRouter = Router();

/** Stable 0-99 bucket for a device, so rollout membership does not flap. */
function rolloutBucket(uuid: string): number {
  const digest = crypto.createHash('sha256').update(uuid).digest();
  return digest.readUInt16BE(0) % 100;
}

function isNewer(remote: string, local: string): boolean {
  const r = remote.split('.').map(Number);
  const l = local.split('.').map(Number);
  for (let i = 0; i < 3; i++) {
    if ((r[i] ?? 0) !== (l[i] ?? 0)) return (r[i] ?? 0) > (l[i] ?? 0);
  }
  return false;
}

/**
 * GET /api/ota/manifest?uuid=...&hw=...&fw=...
 * Called by the firmware. Returns 204 when there is nothing to install.
 */
otaRouter.get(
  '/manifest',
  handler(async (req, res) => {
    const query = z
      .object({
        uuid: z.string().min(4).max(64),
        hw: z.string().max(40).default('devkitv1-8ch'),
        fw: z.string().max(20).default('0.0.0'),
      })
      .parse(req.query);

    const device = await prisma.device.findUnique({ where: { uuid: query.uuid } });
    if (!device) return res.status(204).end();

    const release = await prisma.otaRelease.findFirst({
      where: { hardware: query.hw, rolloutPercent: { gt: 0 } },
      orderBy: { createdAt: 'desc' },
    });
    if (!release) return res.status(204).end();

    if (!isNewer(release.version, query.fw)) return res.status(204).end();

    if (rolloutBucket(query.uuid) >= release.rolloutPercent) {
      log.debug(
        { uuid: query.uuid, percent: release.rolloutPercent },
        'device not in this rollout wave yet',
      );
      return res.status(204).end();
    }

    log.info(
      { uuid: query.uuid, from: query.fw, to: release.version },
      'offering firmware update',
    );

    // sha256 is verified by the firmware while streaming, before any boot flag
    // is touched. A truncated download never becomes bootable.
    res.json({
      version: release.version,
      url: release.url,
      sha256: release.sha256,
      mandatory: release.mandatory,
      notes: release.notes,
    });
  }),
);

// --- release management (authenticated) ------------------------------------

otaRouter.get(
  '/releases',
  requireAuth,
  handler(async (_req, res) => {
    res.json(await prisma.otaRelease.findMany({ orderBy: { createdAt: 'desc' } }));
  }),
);

otaRouter.post(
  '/releases',
  requireAuth,
  handler(async (req, res) => {
    const body = z
      .object({
        version: z.string().regex(/^\d+\.\d+\.\d+$/, 'version must be N.N.N'),
        hardware: z.string().max(40).default('devkitv1-8ch'),
        url: z.string().url(),
        sha256: z.string().regex(/^[0-9a-f]{64}$/, 'sha256 must be 64 hex characters'),
        notes: z.string().max(2000).optional(),
        // Defaults to 0: a new release reaches nobody until you deliberately
        // widen it. Opting in beats opting out when the mistake is unfixable.
        rolloutPercent: z.number().int().min(0).max(100).default(0),
        mandatory: z.boolean().default(false),
      })
      .parse(req.body);

    const release = await prisma.otaRelease.upsert({
      where: { version_hardware: { version: body.version, hardware: body.hardware } },
      create: body,
      update: body,
    });
    res.status(201).json(release);
  }),
);

otaRouter.patch(
  '/releases/:id',
  requireAuth,
  handler(async (req, res) => {
    const body = z
      .object({
        rolloutPercent: z.number().int().min(0).max(100).optional(),
        mandatory: z.boolean().optional(),
      })
      .parse(req.body);

    const release = await prisma.otaRelease.update({ where: { id: req.params.id }, data: body });

    if (body.rolloutPercent === 0) {
      // The kill switch: stops further devices picking it up. Devices already
      // running it are unaffected - they roll back on their own only if the
      // image never proved it could get online.
      log.warn({ version: release.version }, 'rollout halted');
    }
    res.json(release);
  }),
);

otaRouter.get(
  '/rollout/:id',
  requireAuth,
  handler(async (req, res) => {
    const release = await prisma.otaRelease.findUnique({ where: { id: req.params.id } });
    if (!release) throw new HttpError(404, 'release not found');

    const devices = await prisma.device.findMany({
      where: { hardwareRevision: release.hardware },
      select: { uuid: true, name: true, firmwareVersion: true, online: true },
    });

    res.json({
      release: release.version,
      rolloutPercent: release.rolloutPercent,
      total: devices.length,
      updated: devices.filter((d) => d.firmwareVersion === release.version).length,
      inWave: devices.filter((d) => rolloutBucket(d.uuid) < release.rolloutPercent).length,
      devices,
    });
  }),
);
