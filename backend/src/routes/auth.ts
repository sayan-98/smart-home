// ---------------------------------------------------------------------------
//  routes/auth.ts - registration, login, and the current user.
// ---------------------------------------------------------------------------
import { Router } from 'express';
import rateLimit from 'express-rate-limit';
import { z } from 'zod';

import {
  HttpError,
  audit,
  handler,
  hashPassword,
  prisma,
  requireAuth,
  signUserToken,
  userHomes,
  verifyPassword,
} from '../core.js';

export const authRouter = Router();

// Brute-force protection belongs here, on the server, not on the device -
// an ESP32 rate-limiting itself does nothing about someone flooding the API.
const loginLimiter = rateLimit({
  windowMs: 15 * 60 * 1000,
  limit: 20,
  standardHeaders: true,
  legacyHeaders: false,
  message: { error: 'too many attempts, try again later' },
});

const credentials = z.object({
  email: z.string().email().max(200),
  password: z.string().min(8, 'password must be at least 8 characters').max(200),
  name: z.string().max(80).optional(),
});

authRouter.post(
  '/register',
  loginLimiter,
  handler(async (req, res) => {
    const body = credentials.parse(req.body);
    const email = body.email.toLowerCase();

    const existing = await prisma.user.findUnique({ where: { email } });
    if (existing) throw new HttpError(409, 'an account with that email already exists');

    const user = await prisma.user.create({
      data: { email, passwordHash: await hashPassword(body.password), name: body.name },
    });

    // A first home, so the app has somewhere to put the first device.
    const home = await prisma.home.create({ data: { name: 'My Home' } });
    await prisma.membership.create({
      data: { userId: user.id, homeId: home.id, role: 'OWNER' },
    });

    await audit({ homeId: home.id, userId: user.id, action: 'user.register', ip: req.ip });

    res.status(201).json({
      token: signUserToken(user.id, user.email),
      user: { id: user.id, email: user.email, name: user.name },
      homes: [{ ...home, role: 'OWNER' }],
    });
  }),
);

authRouter.post(
  '/login',
  loginLimiter,
  handler(async (req, res) => {
    const body = credentials.omit({ name: true }).parse(req.body);
    const user = await prisma.user.findUnique({ where: { email: body.email.toLowerCase() } });

    // Same message and roughly the same work either way, so the response does
    // not reveal whether the account exists.
    const ok = user && (await verifyPassword(body.password, user.passwordHash));
    if (!user || !ok) throw new HttpError(401, 'invalid email or password');

    res.json({
      token: signUserToken(user.id, user.email),
      user: { id: user.id, email: user.email, name: user.name },
      homes: await userHomes(user.id),
    });
  }),
);

authRouter.get(
  '/me',
  requireAuth,
  handler(async (req, res) => {
    const user = await prisma.user.findUnique({ where: { id: req.userId! } });
    if (!user) throw new HttpError(404, 'user not found');
    res.json({
      user: { id: user.id, email: user.email, name: user.name },
      homes: await userHomes(user.id),
    });
  }),
);
