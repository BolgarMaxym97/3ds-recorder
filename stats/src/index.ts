import { DUMP_NAME, extract, INSTALL_ID, isDump, MAX_DUMP, MAX_REPORT, parseReport } from "./report";

// POST /v1/report - new log lines from one app launch (JSON); 204
// POST /v1/dump   - one Luma crash dump (raw bytes, X-Id and X-Name headers); 204
// GET  /v1/health - "ok", for checking a console can reach the Worker at all
// The app only looks at the status: 2xx means "sent, do not send it again".
export default {
  async fetch(request, env): Promise<Response> {
    const { pathname } = new URL(request.url);

    if (pathname === "/v1/health" && request.method === "GET") return new Response("ok");
    if (pathname !== "/v1/report" && pathname !== "/v1/dump") return new Response(null, { status: 404 });
    if (request.method !== "POST") return new Response(null, { status: 405, headers: { Allow: "POST" } });

    const limit = pathname === "/v1/report" ? MAX_REPORT : MAX_DUMP;
    if (Number(request.headers.get("Content-Length") ?? 0) > limit) return new Response(null, { status: 413 });
    const body = await readLimited(request, limit);
    if (!body) return new Response(null, { status: 413 });

    return pathname === "/v1/report" ? report(env.DB, body) : dump(env.DB, request.headers, body);
  },

  async scheduled(_controller, env): Promise<void> {
    await expire(env.DB);
  },
} satisfies ExportedHandler<Env>;

// The body up to `limit` bytes, null past it: a chunked body has no Content-Length to check first.
async function readLimited(request: Request, limit: number): Promise<Uint8Array | null> {
  if (!request.body) return new Uint8Array(0);
  const chunks: Uint8Array[] = [];
  let size = 0;
  const reader = request.body.getReader();
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    size += value.length;
    if (size > limit) {
      await reader.cancel();
      return null;
    }
    chunks.push(value);
  }
  const body = new Uint8Array(size);
  let at = 0;
  for (const chunk of chunks) {
    body.set(chunk, at);
    at += chunk.length;
  }
  return body;
}

async function report(db: D1Database, body: Uint8Array): Promise<Response> {
  let raw: unknown;
  try {
    raw = JSON.parse(new TextDecoder().decode(body));
  } catch {
    return new Response("bad json", { status: 400 });
  }
  const parsed = parseReport(raw);
  if (!parsed.ok) return new Response(parsed.error, { status: 400 });

  const row = { ...parsed.report, ...extract(parsed.report.rec_log) };
  const columns = Object.keys(row) as (keyof typeof row)[];
  // The same marks again = the app did not get the last answer: the newer text replaces that row.
  const update = columns.map((column) => `${column} = excluded.${column}`).join(", ");
  await db
    .prepare(
      `INSERT INTO reports (${columns.join(", ")}) VALUES (${columns.map(() => "?").join(", ")})
       ON CONFLICT (install_id, sent_from) DO UPDATE SET ${update},
         received_at = strftime('%Y-%m-%dT%H:%M:%SZ', 'now')`,
    )
    .bind(...columns.map((column) => row[column]))
    .run();
  return new Response(null, { status: 204 });
}

async function dump(db: D1Database, headers: Headers, body: Uint8Array): Promise<Response> {
  const id = headers.get("X-Id") ?? "";
  const name = headers.get("X-Name") ?? "";
  if (!INSTALL_ID.test(id)) return new Response("bad id", { status: 400 });
  if (!DUMP_NAME.test(name)) return new Response("bad name", { status: 400 });
  if (!isDump(body)) return new Response("not a dump", { status: 400 });

  await db
    .prepare("INSERT OR IGNORE INTO dumps (install_id, name, size, data) VALUES (?, ?, ?, ?)")
    .bind(id, name, body.length, body)
    .run();
  return new Response(null, { status: 204 });
}

// Log text and dumps go after 180 days; the short columns stay for the statistics.
export async function expire(db: D1Database): Promise<void> {
  const cutoff = "strftime('%Y-%m-%dT%H:%M:%SZ', 'now', '-180 days')";
  await db.batch([
    db.prepare(
      `UPDATE reports SET app_log = NULL, rec_log = NULL
       WHERE received_at < ${cutoff} AND (app_log IS NOT NULL OR rec_log IS NOT NULL)`,
    ),
    db.prepare(`DELETE FROM dumps WHERE received_at < ${cutoff}`),
  ]);
}
