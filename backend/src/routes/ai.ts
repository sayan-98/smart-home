// ---------------------------------------------------------------------------
//  routes/ai.ts - the Groq-backed endpoints.
//
//  The shape of every route here is the same: ask the model for a PROPOSAL,
//  validate it against the real database and the caller's real role, then
//  either execute or ask for confirmation. The model is never the thing that
//  decides.
//
//  All of it degrades gracefully. With no GROQ_API_KEY, or Groq down, or the
//  free-tier quota exhausted, these endpoints return a clear message and the
//  app falls back to manual control. On-device schedules and automations are
//  entirely unaffected - they never touched this path.
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
} from '../core.js';
import { aiEnabled, env } from '../env.js';
import {
  GroqError,
  draftSchedule,
  explainDiagnostics,
  interpretCommand,
  summariseUsage,
} from '../ai/groq.js';
import {
  buildInventory,
  needsConfirmation,
  rateLimit,
  validateActions,
  type ValidatedAction,
} from '../ai/guard.js';
import { publishRelayCommand } from '../mqtt/bridge.js';

export const aiRouter = Router();
aiRouter.use(requireAuth);

function assertEnabled(): void {
  if (!aiEnabled) {
    throw new HttpError(503, 'AI features are not configured on this server');
  }
}

function assertQuota(userId: string): void {
  if (!rateLimit(userId, env.AI_RATE_LIMIT_PER_MINUTE)) {
    throw new HttpError(429, 'too many AI requests - try again in a minute');
  }
}

async function execute(actions: ValidatedAction[]): Promise<number> {
  let sent = 0;
  for (const item of actions) {
    const { action } = item;
    if (action.type === 'relay') {
      const relay = await prisma.relay.findUnique({
        where: { deviceId_channel: { deviceId: item.deviceId, channel: action.channel } },
      });
      if (
        publishRelayCommand(item.homeId, item.deviceUuid, action.channel, action.action, {
          rev: (relay?.rev ?? 0) + 1,
        })
      ) {
        sent++;
      }
    } else if (action.type === 'timer') {
      if (
        publishRelayCommand(item.homeId, item.deviceUuid, action.channel, 'on', {
          seconds: action.seconds,
        })
      ) {
        sent++;
      }
    } else if (action.type === 'allOff') {
      if (publishRelayCommand(item.homeId, item.deviceUuid, 'all', 'off')) sent++;
    }
  }
  return sent;
}

/**
 * POST /api/ai/command
 *   { homeId, text, confirm?: boolean }
 *
 * "turn off everything in the bedroom", "switch on the geyser for 20 minutes".
 */
aiRouter.post(
  '/command',
  handler(async (req, res) => {
    assertEnabled();
    assertQuota(req.userId!);

    const body = z
      .object({
        homeId: z.string().min(1),
        text: z.string().min(1).max(500),
        confirm: z.boolean().default(false),
      })
      .parse(req.body);

    const role = await requireHomeRole(req.userId!, body.homeId, 'GUEST');
    const inventory = await buildInventory(body.homeId);

    if (inventory.channels.length === 0) {
      throw new HttpError(409, 'this home has no claimed devices yet');
    }

    let intent;
    try {
      intent = await interpretCommand(body.text, inventory);
    } catch (err) {
      if (err instanceof GroqError) {
        // Never a hard failure for the user - the manual controls still work.
        throw new HttpError(503, 'the AI service is unavailable right now');
      }
      throw err;
    }

    if (!intent || !intent.understood || intent.actions.length === 0) {
      return res.json({
        understood: false,
        reply: intent?.reply ?? "I couldn't work out what to change. Try naming the socket.",
        actions: [],
        executed: 0,
      });
    }

    // Nothing the model returned is trusted until it survives this.
    const guard = await validateActions(body.homeId, role, intent.actions);

    if (guard.allowed.length === 0) {
      return res.json({
        understood: false,
        reply: "I couldn't match that to anything you can control here.",
        rejected: guard.rejected.map((r) => r.reason),
        actions: [],
        executed: 0,
      });
    }

    const confirmation = needsConfirmation(guard, intent.confidence);
    if (confirmation.required && !body.confirm) {
      return res.json({
        understood: true,
        needsConfirmation: true,
        reason: confirmation.reason,
        reply: intent.reply,
        actions: guard.allowed.map((a) => a.describe),
        executed: 0,
      });
    }

    const executed = await execute(guard.allowed);

    await audit({
      homeId: body.homeId,
      userId: req.userId,
      action: 'ai.command',
      detail: {
        actions: guard.allowed.map((a) => a.describe),
        rejected: guard.rejected.length,
        confidence: intent.confidence,
      },
      // The originating sentence is recorded, so an odd action can always be
      // traced back to what was actually said.
      aiPrompt: body.text,
      ip: req.ip,
    });

    res.json({
      understood: true,
      reply: intent.reply,
      actions: guard.allowed.map((a) => a.describe),
      rejected: guard.rejected.map((r) => r.reason),
      executed,
    });
  }),
);

/**
 * POST /api/ai/schedule
 *   { deviceId, text, apply?: boolean }
 *
 * Authoring, not runtime. The resulting rules are pushed to the device and run
 * there - offline, forever, with Groq never consulted again.
 */
aiRouter.post(
  '/schedule',
  handler(async (req, res) => {
    assertEnabled();
    assertQuota(req.userId!);

    const body = z
      .object({
        homeId: z.string().min(1),
        text: z.string().min(1).max(500),
      })
      .parse(req.body);

    await requireHomeRole(req.userId!, body.homeId, 'MEMBER');
    const inventory = await buildInventory(body.homeId);

    let draft;
    try {
      draft = await draftSchedule(body.text, inventory);
    } catch {
      throw new HttpError(503, 'the AI service is unavailable right now');
    }

    if (!draft || !draft.understood || draft.schedules.length === 0) {
      return res.json({
        understood: false,
        reply: draft?.reply ?? "I couldn't turn that into a schedule.",
        schedules: [],
      });
    }

    // Same rule as commands: verify every uuid/channel against reality before
    // showing the user something they might tap "save" on.
    const known = new Set(inventory.channels.map((c) => `${c.deviceUuid}:${c.channel}`));
    const valid = draft.schedules.filter((s) =>
      s.channels.every((ch) => known.has(`${s.deviceUuid}:${ch}`)),
    );

    // Returned as a DRAFT for review - never written straight to the device.
    // These rules will run unattended on mains sockets.
    res.json({
      understood: valid.length > 0,
      reply: draft.reply,
      schedules: valid,
      discarded: draft.schedules.length - valid.length,
    });
  }),
);

/** POST /api/ai/diagnose { deviceId } - plain-English device health. */
aiRouter.post(
  '/diagnose',
  handler(async (req, res) => {
    assertEnabled();
    assertQuota(req.userId!);

    const { deviceId } = z.object({ deviceId: z.string().min(1) }).parse(req.body);

    const device = await prisma.device.findUnique({ where: { id: deviceId } });
    if (!device?.homeId) throw new HttpError(404, 'device not found');
    await requireHomeRole(req.userId!, device.homeId, 'GUEST');

    const recent = await prisma.deviceEvent.findMany({
      where: { deviceId, type: 'heartbeat' },
      orderBy: { at: 'desc' },
      take: 5,
    });

    const summary = await explainDiagnostics({
      firmware: device.firmwareVersion,
      online: device.online,
      rssi: device.rssi,
      freeHeap: device.freeHeap,
      resetReason: device.resetReason,
      brownout: device.lastBrownout,
      uptimeMs: device.uptimeMs?.toString(),
      recentHeartbeats: recent.map((e) => e.payload),
    });

    res.json({
      summary: summary ?? 'No summary available right now.',
      // The raw numbers are always returned, so the page is useful even when
      // the AI call failed.
      raw: {
        online: device.online,
        rssi: device.rssi,
        freeHeap: device.freeHeap,
        resetReason: device.resetReason,
        brownout: device.lastBrownout,
      },
    });
  }),
);

/** POST /api/ai/insights { homeId } - weekly usage summary. */
aiRouter.post(
  '/insights',
  handler(async (req, res) => {
    assertEnabled();
    assertQuota(req.userId!);

    const { homeId } = z.object({ homeId: z.string().min(1) }).parse(req.body);
    await requireHomeRole(req.userId!, homeId, 'GUEST');

    const since = new Date(Date.now() - 7 * 24 * 3600 * 1000);
    const devices = await prisma.device.findMany({
      where: { homeId },
      include: { relays: true },
    });

    const stats = await Promise.all(
      devices.map(async (d) => ({
        device: d.name,
        online: d.online,
        resetReason: d.resetReason,
        brownout: d.lastBrownout,
        switches: await prisma.deviceEvent.count({
          where: { deviceId: d.id, type: 'relay', at: { gte: since } },
        }),
        channels: d.relays.map((r) => ({ name: r.name, on: r.state })),
      })),
    );

    const summary = await summariseUsage({ periodDays: 7, devices: stats });
    res.json({ summary: summary ?? 'No summary available right now.', stats });
  }),
);

/** Lets the app grey out AI features instead of showing errors. */
aiRouter.get('/status', (_req, res) => {
  res.json({ enabled: aiEnabled, model: aiEnabled ? env.GROQ_MODEL : null });
});
