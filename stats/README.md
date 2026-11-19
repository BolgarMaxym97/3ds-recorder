# Statistics and logs

A Cloudflare Worker with a D1 database that collects what the 3DS Recorder app's logs gained since
the last launch, so we can see how the recorder behaves on consoles and in games we cannot test
(the New 3DS above all). The app sends only when the user left Settings → «Share Statistics» on
and the console is on Wi-Fi, and it never shows the result (`app/source/stats.h`).

- `POST /v1/report` — JSON, at most 96 KB: console facts, `config.ini`, the new lines of `app.log`
  (≤ 16 KB) and `recorder.log` (≤ 32 KB). `204` stored, `400` malformed, `413` too big.
- `POST /v1/dump` — one Luma ARM11 crash dump as raw bytes, at most 48 KB, with `X-Id` (install
  ID) and `X-Name` (`crash_dump_NNNNNNNN.dmp`). `204` / `400` / `413`.
- `GET /v1/health` — `ok`.
- Both POSTs: at most 10 a minute from one client address (Workers Rate Limiting binding
  `LIMITER`), else `429`. A launch sends 1-2; the counters are per Cloudflare location and
  approximate, so a burst gets through a little past 10 before the 429s start.
- Cron `0 3 * * *` — clears `app_log` / `rec_log` and deletes dumps older than 180 days; the short
  columns stay.

The app moves its "sent up to here" marks (`/3ds/recorder/stats.ini`) on a 2xx, so nothing is
sent twice and nothing is lost when a send fails; on a 4xx other than 429 (and a report too big to
build) it moves them too, so lines the Worker will never take are not sent at every launch. What a report holds is defined in
`src/report.ts`; `app/source/stats_core.c` + `stats.c` build it. The Worker also reads a few
columns out of `rec_log` (`titles`, `starts`, `recordings`, `dropped`, `hangs`) so the statistics
outlive the log text.

No IP, no country: the Worker never stores them (the client address is only the rate limiter's key), Workers Logs is off in `wrangler.jsonc`, and the
app blanks LAN addresses in the logs before sending. The install ID is random, made on the console,
and shown in Settings next to the switch (`XXXX-XXXX`; look it up without the dash).

The app talks plain HTTP: the console's TLS does not get through to workers.dev (3ds-ua, 2026-10-23;
again 2026-11-19: workers.dev has an ECDSA certificate and refuses the RSA / CBC suites the 3DS offers).

## Develop

```bash
npm ci
npm test                 # Vitest inside the Workers runtime, local D1
npm run typecheck
npm run dev              # local Worker on :8787 (apply migrations --local once first)
```

## Deploy

```bash
npx wrangler d1 migrations apply 3ds-recorder-stats --remote
npx wrangler deploy      # https://3ds-recorder-stats.bolgar-maxym.workers.dev
curl http://3ds-recorder-stats.bolgar-maxym.workers.dev/v1/health   # ok
```

The address is built into the app (`STATS_URL` in `app/source/stats.h`); changing it takes a release.

## Queries

`npx wrangler d1 execute 3ds-recorder-stats --remote --command "<SQL>"` (add `--json` for whole log
texts), or the D1 console in the Cloudflare dashboard.

Consoles by model and app version:

```sql
SELECT model, app_version, COUNT(DISTINCT install_id) AS consoles, COUNT(*) AS reports
FROM reports GROUP BY model, app_version ORDER BY consoles DESC;
```

New consoles per day (first report of each ID):

```sql
SELECT substr(first, 1, 10) AS day, COUNT(*) AS consoles
FROM (SELECT MIN(received_at) AS first FROM reports GROUP BY install_id)
GROUP BY day ORDER BY day DESC;
```

Games played on New 3DS (title IDs; look them up on 3dsdb / hshop):

```sql
WITH RECURSIVE split(tid, rest) AS (
  SELECT '', titles || ',' FROM reports WHERE titles IS NOT NULL AND model LIKE 'New%'
  UNION ALL
  SELECT substr(rest, 1, instr(rest, ',') - 1), substr(rest, instr(rest, ',') + 1) FROM split WHERE rest <> ''
)
SELECT tid, COUNT(*) AS reports FROM split WHERE tid <> '' GROUP BY tid ORDER BY reports DESC;
```

Reports with dropped frames or a forced power-off (POWER held, usually after a hang):

```sql
SELECT id, received_at, install_id, model, app_version, titles, recordings, dropped, hangs
FROM reports WHERE dropped > 0 OR hangs > 0 ORDER BY received_at DESC LIMIT 50;
```

Everything from one console (the ID from its Settings screen, without the dash):

```sql
SELECT id, received_at, app_version, model, system_version, luma, titles, recordings, dropped, hangs
FROM reports WHERE install_id = 'K7QM29XD' ORDER BY received_at;
SELECT rec_log FROM reports WHERE id = 123;     -- one report's recorder.log lines
```

Settings in use (fps, replay, autostart …):

```sql
SELECT config, COUNT(DISTINCT install_id) AS consoles FROM reports GROUP BY config ORDER BY consoles DESC;
```

Languages: the app's (Settings → Language) against the console's own (which translation to make next):

```sql
SELECT lang, sys_lang, COUNT(DISTINCT install_id) AS consoles FROM reports
WHERE id IN (SELECT MAX(id) FROM reports GROUP BY install_id) GROUP BY lang, sys_lang ORDER BY consoles DESC;
```

Crash dumps; save one to a file to read it with Luma's `luma3ds_exception_dump_parser.py`:

```sql
SELECT d.id, d.received_at, d.install_id, d.name, d.size, r.model, r.app_version
FROM dumps d LEFT JOIN reports r ON r.id = (SELECT MAX(id) FROM reports WHERE install_id = d.install_id)
ORDER BY d.received_at DESC;
```

```bash
npx wrangler d1 execute 3ds-recorder-stats --remote --json --command "SELECT hex(data) AS h FROM dumps WHERE id = 1" \
  | python3 -c 'import json,sys; sys.stdout.buffer.write(bytes.fromhex(json.load(sys.stdin)[0]["results"][0]["h"]))' > dump.dmp
```

Delete one console's records on request:

```sql
DELETE FROM reports WHERE install_id = 'K7QM29XD';
DELETE FROM dumps WHERE install_id = 'K7QM29XD';
```
