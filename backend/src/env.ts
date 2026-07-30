// ---------------------------------------------------------------------------
//  env.ts - configuration, validated at startup.
//
//  Failing loudly here beats a NaN port or an undefined secret surfacing as a
//  mystery 500 three hours into a deploy.
// ---------------------------------------------------------------------------
import 'dotenv/config';
import { z } from 'zod';

const schema = z.object({
  NODE_ENV: z.enum(['development', 'production', 'test']).default('development'),
  PORT: z.coerce.number().int().positive().default(3000),

  // Supabase gives two URLs: a pooled one for runtime, a direct one for
  // migrations. Using the pooled URL for migrations fails in confusing ways.
  DATABASE_URL: z.string().min(1, 'DATABASE_URL is required (Supabase pooled connection)'),
  DIRECT_URL: z.string().optional(),

  JWT_SECRET: z.string().min(32, 'JWT_SECRET must be at least 32 characters'),
  JWT_EXPIRES_IN: z.string().default('30d'),

  // MQTT. Render's free tier cannot host a broker - no raw TCP, and it sleeps -
  // so this points at HiveMQ Cloud Serverless (free, always on, TLS-only).
  MQTT_URL: z.string().default('mqtts://localhost:8883'),
  MQTT_USERNAME: z.string().optional(),
  MQTT_PASSWORD: z.string().optional(),
  MQTT_TOPIC_ROOT: z.string().default('smarthome'),

  // Groq. Backend only - never shipped to the app or the firmware, because an
  // APK is trivially unpacked and an ESP32 cannot keep a secret.
  GROQ_API_KEY: z.string().optional(),
  GROQ_MODEL: z.string().default('llama-3.3-70b-versatile'),
  GROQ_FAST_MODEL: z.string().default('llama-3.1-8b-instant'),
  AI_RATE_LIMIT_PER_MINUTE: z.coerce.number().int().positive().default(10),

  // Telemetry retention. Supabase free is 500 MB and a 30 s heartbeat is about
  // a million rows per device per year, so this is not optional.
  EVENT_RETENTION_DAYS: z.coerce.number().int().positive().default(30),

  CORS_ORIGINS: z.string().default('*'),
  PUBLIC_BASE_URL: z.string().optional(),
});

const parsed = schema.safeParse(process.env);

if (!parsed.success) {
  const issues = parsed.error.issues
    .map((i) => `  - ${i.path.join('.') || '(root)'}: ${i.message}`)
    .join('\n');
  console.error(`\nInvalid environment configuration:\n${issues}\n`);
  console.error('Copy .env.example to .env and fill it in.\n');
  process.exit(1);
}

export const env = parsed.data;

export const isProd = env.NODE_ENV === 'production';

/** AI features degrade gracefully rather than erroring when no key is set. */
export const aiEnabled = Boolean(env.GROQ_API_KEY);

export const corsOrigins =
  env.CORS_ORIGINS === '*' ? true : env.CORS_ORIGINS.split(',').map((s) => s.trim());
