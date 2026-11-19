// The report the app sends - the one place its shape is written down on this side.
// app/source/stats_core.c builds it; keep the two in step.

export const MAX_REPORT = 96 * 1024;
export const MAX_DUMP = 48 * 1024;

// Crockford base32: no I, L, O, U.
export const INSTALL_ID = /^[0-9A-HJKMNP-TV-Z]{8}$/;
export const DUMP_NAME = /^crash_dump_\d{8}\.dmp$/;
// Luma's crash dump header starts with these two words (little-endian 0xDEADC0DE, 0xDEADCAFE).
const DUMP_MAGIC = [0xde, 0xc0, 0xad, 0xde, 0xfe, 0xca, 0xad, 0xde];

export interface Report {
  install_id: string;
  app_version: string;
  channel: string | null;
  build: string | null;
  model: string | null;
  region: string | null;
  system_version: string | null;
  luma: string | null;
  kernel: string | null;
  config: string | null;
  app_log: string | null;
  rec_log: string | null;
  sent_from: string | null;
  lang: string | null;
  sys_lang: string | null;
}

export interface Extracted {
  titles: string | null;
  starts: number;
  recordings: number;
  dropped: number;
  hangs: number;
}

type Result = { ok: true; report: Report } | { ok: false; error: string };

// Optional text: absent or null -> null; anything else must be a string within `max`.
function text(body: Record<string, unknown>, key: string, max: number): string | null | undefined {
  const value = body[key];
  if (value === undefined || value === null) return null;
  if (typeof value !== "string" || value.length > max) return undefined;
  return value;
}

export function parseReport(raw: unknown): Result {
  if (typeof raw !== "object" || raw === null || Array.isArray(raw)) return { ok: false, error: "not an object" };
  const body = raw as Record<string, unknown>;

  if (body.v !== 1) return { ok: false, error: "unknown version" };
  if (typeof body.id !== "string" || !INSTALL_ID.test(body.id)) return { ok: false, error: "bad id" };
  const app = text(body, "app", 16);
  if (!app) return { ok: false, error: "bad app" };

  const fields = {
    channel: text(body, "ch", 16),
    build: text(body, "build", 8),
    model: text(body, "model", 32),
    region: text(body, "region", 8),
    system_version: text(body, "system", 32),
    luma: text(body, "luma", 16),
    kernel: text(body, "kernel", 16),
    config: text(body, "config", 4096),
    app_log: text(body, "app_log", 32 * 1024),
    rec_log: text(body, "rec_log", 48 * 1024),
    sent_from: text(body, "from", 64),
    lang: text(body, "lang", 16),
    sys_lang: text(body, "sys_lang", 8),
  };
  for (const [key, value] of Object.entries(fields)) {
    if (value === undefined) return { ok: false, error: `bad ${key}` };
  }

  // Before 1.5.0 the app sent no "lang": its config.ini has it (no lang= line: English).
  if (fields.lang === null && typeof fields.config === "string") fields.lang = configLang(fields.config);
  if (fields.sys_lang === "") fields.sys_lang = null;
  return { ok: true, report: { install_id: body.id, app_version: app, ...fields } as Report };
}

// The app's language from its config.ini ("lang=uk"), "en" when the line is not there.
export function configLang(config: string): string {
  const match = /^lang=([A-Za-z-]{2,16})\s*$/m.exec(config);
  return match ? match[1] : "en";
}

// Short columns read from recorder.log, so the statistics outlive the 180-day log text.
export function extract(recLog: string | null): Extracted {
  const log = recLog ?? "";
  const titles = new Set<string>();
  for (const match of log.matchAll(/program started: pid \d+ title ([0-9A-F]{16})/g)) titles.add(match[1]);
  let recordings = 0;
  let dropped = 0;
  for (const match of log.matchAll(/recording saved: [^\n]*?, dropped (\d+)/g)) {
    recordings++;
    dropped += Number(match[1]);
  }
  return {
    titles: titles.size ? [...titles].join(",") : null,
    starts: count(log, /recorder started \(/g),
    recordings,
    dropped,
    hangs: count(log, /system notification 203/g),
  };
}

function count(log: string, re: RegExp): number {
  return [...log.matchAll(re)].length;
}

export function isDump(bytes: Uint8Array): boolean {
  return bytes.length >= DUMP_MAGIC.length && DUMP_MAGIC.every((b, i) => bytes[i] === b);
}
