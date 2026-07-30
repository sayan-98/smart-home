// ---------------------------------------------------------------------------
//  core.ts - logger, database client, auth helpers and permission checks.
//
//  Kept in one file because these four things are used by nearly every route
//  and splitting them across four modules would add imports without adding
//  clarity.
// ---------------------------------------------------------------------------
import { PrismaClient, Role } from '@prisma/client';
import bcrypt from 'bcryptjs';
import crypto from 'node:crypto';
import type { NextFunction, Request, Response } from 'express';
import jwt from 'jsonwebtoken';
import pino from 'pino';

import { env, isProd } from './env.js';

// --- logging ---------------------------------------------------------------

export const log = pino({
  level: isProd ? 'info' : 'debug',
  transport: isProd ? undefined : { target: 'pino-pretty', options: { colorize: true } },
  redact: {
    // These end up in request bodies and would otherwise be logged verbatim.
    paths: ['req.headers.authorization', '*.password', '*.apiKey', '*.mqttPass', '*.claimCode'],
    censor: '[redacted]',
  },
});

// --- database --------------------------------------------------------------

export const prisma = new PrismaClient({
  log: isProd ? ['warn', 'error'] : ['warn', 'error'],
});

/**
 * Prisma returns BigInt for autoincrement ids, and JSON.stringify throws on
 * BigInt. Patch it once rather than remembering to map every response.
 */
(BigInt.prototype as unknown as { toJSON(): string }).toJSON = function () {
  return this.toString();
};

// --- tokens and secrets ----------------------------------------------------

export interface JwtPayload {
  sub: string;
  email: string;
}

export function signUserToken(userId: string, email: string): string {
  return jwt.sign({ sub: userId, email } satisfies JwtPayload, env.JWT_SECRET, {
    expiresIn: env.JWT_EXPIRES_IN as jwt.SignOptions['expiresIn'],
  });
}

export function hashPassword(plain: string): Promise<string> {
  return bcrypt.hash(plain, 12);
}

export function verifyPassword(plain: string, hash: string): Promise<boolean> {
  return bcrypt.compare(plain, hash);
}

/**
 * Device API keys are long-lived and revocable, NOT JWTs. A JWT expires, and a
 * device that has been offline for a month cannot refresh one - it would lock
 * itself out permanently. Revocation is the property that actually matters, and
 * deleting the hash gives us that.
 */
export function generateDeviceApiKey(): string {
  return crypto.randomBytes(32).toString('hex'); // 64 hex chars
}

export function hashApiKey(key: string): Promise<string> {
  return bcrypt.hash(key, 10);
}

export function verifyApiKey(key: string, hash: string): Promise<boolean> {
  return bcrypt.compare(key, hash);
}

/** Timing-safe compare for anything short and guessable. */
export function safeEqual(a: string, b: string): boolean {
  const ba = Buffer.from(a);
  const bb = Buffer.from(b);
  if (ba.length !== bb.length) return false;
  return crypto.timingSafeEqual(ba, bb);
}

// --- request context -------------------------------------------------------

declare global {
  // eslint-disable-next-line @typescript-eslint/no-namespace
  namespace Express {
    interface Request {
      userId?: string;
      userEmail?: string;
      deviceId?: string;
    }
  }
}

export class HttpError extends Error {
  constructor(
    public status: number,
    message: string,
  ) {
    super(message);
  }
}

export function requireAuth(req: Request, _res: Response, next: NextFunction): void {
  const header = req.headers.authorization;
  if (!header?.startsWith('Bearer ')) {
    return next(new HttpError(401, 'authentication required'));
  }
  try {
    const payload = jwt.verify(header.slice(7), env.JWT_SECRET) as JwtPayload;
    req.userId = payload.sub;
    req.userEmail = payload.email;
    next();
  } catch {
    next(new HttpError(401, 'invalid or expired token'));
  }
}

// --- authorization ---------------------------------------------------------

const ROLE_RANK: Record<Role, number> = {
  GUEST: 0,
  MEMBER: 1,
  ADMIN: 2,
  OWNER: 3,
};

/**
 * The requirements never specified who may control what - no roles, no guest
 * access, no sharing, no revocation. Everything below exists to close that gap.
 * Every device-touching route goes through one of these.
 */
export async function requireHomeRole(
  userId: string,
  homeId: string,
  minimum: Role,
): Promise<Role> {
  const membership = await prisma.membership.findUnique({
    where: { userId_homeId: { userId, homeId } },
  });
  if (!membership) throw new HttpError(403, 'you do not have access to this home');
  if (ROLE_RANK[membership.role] < ROLE_RANK[minimum]) {
    throw new HttpError(403, `this action requires the ${minimum} role`);
  }
  return membership.role;
}

/**
 * Resolves a device the user is actually allowed to touch. Returning 404 rather
 * than 403 for a device in someone else's home avoids confirming it exists.
 */
export async function authorizeDevice(
  userId: string,
  deviceId: string,
  minimum: Role = 'GUEST',
) {
  const device = await prisma.device.findUnique({
    where: { id: deviceId },
    include: { relays: { orderBy: { channel: 'asc' } } },
  });
  if (!device) throw new HttpError(404, 'device not found');
  if (!device.homeId) throw new HttpError(409, 'device is not assigned to a home');
  await requireHomeRole(userId, device.homeId, minimum);
  return device;
}

/** Every home the user can see, with their role in each. */
export async function userHomes(userId: string) {
  const memberships = await prisma.membership.findMany({
    where: { userId },
    include: { home: true },
  });
  return memberships.map((m) => ({ ...m.home, role: m.role }));
}

export async function audit(entry: {
  homeId?: string | null;
  userId?: string | null;
  action: string;
  target?: string;
  detail?: unknown;
  aiPrompt?: string;
  ip?: string;
}): Promise<void> {
  try {
    await prisma.auditLog.create({
      data: {
        homeId: entry.homeId ?? null,
        userId: entry.userId ?? null,
        action: entry.action,
        target: entry.target,
        detail: entry.detail === undefined ? undefined : (entry.detail as object),
        aiPrompt: entry.aiPrompt,
        ip: entry.ip,
      },
    });
  } catch (err) {
    // An audit failure must never take down the action being audited.
    log.warn({ err }, 'audit write failed');
  }
}

// --- express helpers -------------------------------------------------------

/** Wraps an async handler so a rejected promise reaches the error middleware. */
export function handler<T extends (req: Request, res: Response) => Promise<unknown>>(fn: T) {
  return (req: Request, res: Response, next: NextFunction): void => {
    fn(req, res).catch(next);
  };
}
