// ---------------------------------------------------------------------------
//  socket.ts - Socket.IO fan-out.
//
//  Read-only by design. Clients receive state; they never push it. Commands go
//  through REST, which publishes MQTT, which the device decides on. That
//  asymmetry is what stops the two buses from drifting apart - see
//  mqtt/bridge.ts for the full rule.
//
//  Clients join a room per home, so a user only ever receives events for homes
//  they are actually a member of.
// ---------------------------------------------------------------------------
import type { Server as HttpServer } from 'node:http';
import jwt from 'jsonwebtoken';
import { Server, type Socket } from 'socket.io';

import { log, prisma, type JwtPayload } from '../core.js';
import { corsOrigins, env } from '../env.js';

let io: Server | null = null;

interface SocketData {
  userId: string;
  homeIds: string[];
}

export function initSocket(server: HttpServer): Server {
  io = new Server(server, {
    cors: { origin: corsOrigins, credentials: true },
    // The Capacitor app may be on a phone network; polling fallback matters.
    transports: ['websocket', 'polling'],
    pingInterval: 25000,
    pingTimeout: 20000,
  });

  io.use(async (socket: Socket, next) => {
    try {
      const token =
        (socket.handshake.auth?.token as string | undefined) ??
        socket.handshake.headers.authorization?.replace('Bearer ', '');
      if (!token) return next(new Error('authentication required'));

      const payload = jwt.verify(token, env.JWT_SECRET) as JwtPayload;

      // Resolve membership at connect time. A user removed from a home keeps
      // receiving events until they reconnect, which is why revocation also
      // disconnects them explicitly (see disconnectUser below).
      const memberships = await prisma.membership.findMany({
        where: { userId: payload.sub },
        select: { homeId: true },
      });

      (socket.data as SocketData) = {
        userId: payload.sub,
        homeIds: memberships.map((m) => m.homeId),
      };
      next();
    } catch {
      next(new Error('invalid token'));
    }
  });

  io.on('connection', (socket: Socket) => {
    const data = socket.data as SocketData;
    for (const homeId of data.homeIds) socket.join(`home:${homeId}`);
    socket.join(`user:${data.userId}`);

    log.debug({ userId: data.userId, homes: data.homeIds.length }, 'socket connected');

    socket.emit('ready', { homeIds: data.homeIds });

    socket.on('disconnect', (reason) => {
      log.debug({ userId: data.userId, reason }, 'socket disconnected');
    });
  });

  log.info('Socket.IO ready');
  return io;
}

export function emitToHome(homeId: string, event: string, payload: unknown): void {
  io?.to(`home:${homeId}`).emit(event, payload);
}

export function emitToUser(userId: string, event: string, payload: unknown): void {
  io?.to(`user:${userId}`).emit(event, payload);
}

/** Called when a membership is revoked, so access ends immediately. */
export function disconnectUser(userId: string): void {
  io?.in(`user:${userId}`).disconnectSockets(true);
}

export function connectedClients(): number {
  return io?.engine.clientsCount ?? 0;
}
