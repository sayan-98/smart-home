// ---------------------------------------------------------------------------
//  routes/homes.ts - homes, rooms, sharing and OTA manifests.
// ---------------------------------------------------------------------------
import { Router } from 'express';
import { z } from 'zod';

import {
  HttpError,
  audit,
  handler,
  prisma,
  requireAuth,
  requireHomeRole,
  userHomes,
} from '../core.js';
import { disconnectUser } from '../realtime/socket.js';

export const homeRouter = Router();
homeRouter.use(requireAuth);

homeRouter.get(
  '/',
  handler(async (req, res) => {
    res.json(await userHomes(req.userId!));
  }),
);

homeRouter.post(
  '/',
  handler(async (req, res) => {
    const body = z
      .object({ name: z.string().min(1).max(60), timezone: z.string().max(40).default('IST-5:30') })
      .parse(req.body);

    const home = await prisma.home.create({ data: body });
    await prisma.membership.create({
      data: { userId: req.userId!, homeId: home.id, role: 'OWNER' },
    });
    res.status(201).json({ ...home, role: 'OWNER' });
  }),
);

homeRouter.get(
  '/:id',
  handler(async (req, res) => {
    await requireHomeRole(req.userId!, req.params.id, 'GUEST');
    const home = await prisma.home.findUnique({
      where: { id: req.params.id },
      include: {
        rooms: { orderBy: { order: 'asc' } },
        devices: { include: { relays: { orderBy: { channel: 'asc' } } } },
        scenes: true,
      },
    });
    res.json(home);
  }),
);

// --- rooms -----------------------------------------------------------------

homeRouter.post(
  '/:id/rooms',
  handler(async (req, res) => {
    await requireHomeRole(req.userId!, req.params.id, 'MEMBER');
    const body = z
      .object({
        name: z.string().min(1).max(40),
        icon: z.string().max(20).default('room'),
        order: z.number().int().min(0).default(0),
      })
      .parse(req.body);

    res.status(201).json(
      await prisma.room.create({ data: { ...body, homeId: req.params.id } }),
    );
  }),
);

homeRouter.delete(
  '/:id/rooms/:roomId',
  handler(async (req, res) => {
    await requireHomeRole(req.userId!, req.params.id, 'ADMIN');
    await prisma.room.delete({ where: { id: req.params.roomId } });
    res.status(204).end();
  }),
);

// --- sharing ---------------------------------------------------------------

homeRouter.get(
  '/:id/members',
  handler(async (req, res) => {
    await requireHomeRole(req.userId!, req.params.id, 'MEMBER');
    const members = await prisma.membership.findMany({
      where: { homeId: req.params.id },
      include: { user: { select: { id: true, email: true, name: true } } },
    });
    res.json(members.map((m) => ({ ...m.user, role: m.role, membershipId: m.id })));
  }),
);

homeRouter.post(
  '/:id/members',
  handler(async (req, res) => {
    await requireHomeRole(req.userId!, req.params.id, 'ADMIN');
    const body = z
      .object({
        email: z.string().email(),
        role: z.enum(['ADMIN', 'MEMBER', 'GUEST']).default('GUEST'),
      })
      .parse(req.body);

    const user = await prisma.user.findUnique({ where: { email: body.email.toLowerCase() } });
    if (!user) throw new HttpError(404, 'no account with that email');

    const membership = await prisma.membership.upsert({
      where: { userId_homeId: { userId: user.id, homeId: req.params.id } },
      create: { userId: user.id, homeId: req.params.id, role: body.role },
      update: { role: body.role },
    });

    await audit({
      homeId: req.params.id,
      userId: req.userId,
      action: 'home.share',
      target: user.email,
      detail: { role: body.role },
      ip: req.ip,
    });
    res.status(201).json(membership);
  }),
);

homeRouter.delete(
  '/:id/members/:userId',
  handler(async (req, res) => {
    await requireHomeRole(req.userId!, req.params.id, 'ADMIN');

    const owners = await prisma.membership.count({
      where: { homeId: req.params.id, role: 'OWNER' },
    });
    const target = await prisma.membership.findUnique({
      where: { userId_homeId: { userId: req.params.userId, homeId: req.params.id } },
    });
    if (!target) throw new HttpError(404, 'not a member of this home');
    if (target.role === 'OWNER' && owners <= 1) {
      throw new HttpError(409, 'a home must keep at least one owner');
    }

    await prisma.membership.delete({ where: { id: target.id } });

    // Revocation has to take effect now, not at their next reconnect.
    disconnectUser(req.params.userId);

    await audit({
      homeId: req.params.id,
      userId: req.userId,
      action: 'home.revoke',
      target: req.params.userId,
      ip: req.ip,
    });
    res.status(204).end();
  }),
);
