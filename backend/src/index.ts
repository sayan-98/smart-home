// ---------------------------------------------------------------------------
//  index.ts - server entry point.
// ---------------------------------------------------------------------------
import { createServer } from 'node:http';
import cors from 'cors';
import express, { type NextFunction, type Request, type Response } from 'express';
import rateLimit from 'express-rate-limit';
import helmet from 'helmet';
import { pinoHttp } from 'pino-http';
import { ZodError } from 'zod';

import { pruneRateLimits } from './ai/guard.js';
import { HttpError, log, prisma } from './core.js';
import { aiEnabled, corsOrigins, env, isProd } from './env.js';
import { mqttConnected, startMqttBridge, stopMqttBridge } from './mqtt/bridge.js';
import { connectedClients, initSocket } from './realtime/socket.js';
import { aiRouter } from './routes/ai.js';
import { authRouter } from './routes/auth.js';
import { deviceRouter } from './routes/devices.js';
import { homeRouter } from './routes/homes.js';
import { otaRouter } from './routes/ota.js';

const app = express();

app.set('trust proxy', 1); // Render terminates TLS in front of us
app.use(helmet({ crossOriginResourcePolicy: false }));
app.use(cors({ origin: corsOrigins, credentials: true }));
app.use(express.json({ limit: '256kb' }));
app.use(
  pinoHttp({
    logger: log,
    // The keep-warm cron hits /health every 10 minutes; logging it is noise.
    autoLogging: { ignore: (req: { url?: string }) => req.url === '/health' },
  }),
);

app.use(
  rateLimit({
    windowMs: 60_000,
    limit: 300,
    standardHeaders: true,
    legacyHeaders: false,
    // The keep-warm ping must not eat the budget.
    skip: (req) => req.path === '/health',
  }),
);

// ---------------------------------------------------------------------------
//  /health doubles as the keep-warm target.
//
//  Render's free tier sleeps after 15 minutes idle and takes 30-60 s to wake.
//  Point a free cron (cron-job.org) at this every 10 minutes. Realtime does not
//  depend on it - MQTT lives on HiveMQ Cloud, which is always on - but a warm
//  service makes the app feel immediate and keeps the Alexa cloud skill inside
//  its 8-second timeout later.
// ---------------------------------------------------------------------------
app.get('/health', (_req, res) => {
  res.json({
    ok: true,
    uptimeSec: Math.round(process.uptime()),
    mqtt: mqttConnected(),
    sockets: connectedClients(),
    ai: aiEnabled,
  });
});

app.use('/api/auth', authRouter);
app.use('/api/homes', homeRouter);
app.use('/api/devices', deviceRouter);
app.use('/api/ai', aiRouter);
app.use('/api/ota', otaRouter);

app.use((_req, res) => res.status(404).json({ error: 'not found' }));

// --- error handling --------------------------------------------------------

app.use((err: unknown, _req: Request, res: Response, _next: NextFunction) => {
  if (err instanceof ZodError) {
    const detail = err.issues.map((i) => `${i.path.join('.') || 'body'}: ${i.message}`).join('; ');
    return res.status(400).json({ error: detail });
  }
  if (err instanceof HttpError) {
    return res.status(err.status).json({ error: err.message });
  }

  log.error({ err }, 'unhandled error');
  // Never leak internals to a client; the detail is in the log.
  res.status(500).json({ error: isProd ? 'internal server error' : String(err) });
});

// --- startup ---------------------------------------------------------------

const server = createServer(app);
initSocket(server);
startMqttBridge();

/**
 * Telemetry retention. A 30 s heartbeat is roughly a million rows per device
 * per year and the Supabase free tier is 500 MB total, so this is what keeps
 * the project inside the budget it was designed for.
 */
async function pruneEvents(): Promise<void> {
  const cutoff = new Date(Date.now() - env.EVENT_RETENTION_DAYS * 86_400_000);
  try {
    const { count } = await prisma.deviceEvent.deleteMany({ where: { at: { lt: cutoff } } });
    if (count > 0) log.info({ count, cutoff }, 'pruned old device events');
  } catch (err) {
    log.error({ err }, 'event retention job failed');
  }
  pruneRateLimits();
}

setInterval(() => void pruneEvents(), 6 * 3600 * 1000).unref();
setTimeout(() => void pruneEvents(), 30_000).unref();

server.listen(env.PORT, () => {
  log.info(
    { port: env.PORT, env: env.NODE_ENV, ai: aiEnabled },
    'Smart Home OS backend listening',
  );
  if (!aiEnabled) {
    log.warn('GROQ_API_KEY is not set - AI endpoints will report themselves as unavailable');
  }
});

// --- shutdown --------------------------------------------------------------

async function shutdown(signal: string): Promise<void> {
  log.info({ signal }, 'shutting down');
  server.close();
  await stopMqttBridge();
  await prisma.$disconnect();
  process.exit(0);
}

process.on('SIGTERM', () => void shutdown('SIGTERM'));
process.on('SIGINT', () => void shutdown('SIGINT'));
