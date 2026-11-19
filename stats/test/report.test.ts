import { SELF } from "cloudflare:test";
import { env } from "cloudflare:workers";
import { beforeEach, describe, expect, it } from "vitest";
import { expire } from "../src/index";
import { extract } from "../src/report";

const BASE = "https://stats.example";

const recLog = [
  "[2026-11-14 12:04:36.365] recorder started (build Nov 14 2026 14:23:52, New 3DS, Luma 13.5.0, kernel 2.58-0)",
  "[2026-11-14 12:05:01.002] program started: pid 41 title 00040000001B5100 (CPU 804 MHz, L2 on)",
  "[2026-11-14 12:27:34.090] recording started (file 1024 MB prepared, fps 30, both, audio on)",
  "[2026-11-14 12:30:11.659] recording saved: 20261114_122733.r3d, 157 s, 409777 KB, frames 4620 + 4624, dropped 3, throttled 0, reason: user",
  "[2026-11-14 12:30:11.779] video drops: ring full 0, DMA error 0, DMA start failed 0",
  "[2026-11-14 12:33:56.275] recording saved: 20261114_123031.r3d, 204 s, 494764 KB, frames 6014 + 6014, dropped 2, throttled 0, reason: user",
  "[2026-11-14 12:40:00.000] program started: pid 42 title 00040000001B5100 (CPU 804 MHz, L2 on)",
  "[2026-11-14 12:41:00.000] program started: pid 43 title 0004000000175E00 (CPU 804 MHz, L2 on)",
  "[2026-11-14 12:50:00.000] system notification 203: POWER held",
  "[2026-11-14 12:50:00.100] system notification 100: the recorder must end",
  "",
].join("\n");

// What app/source/stats_core.c sends on a New 3DS XL.
const report = {
  v: 1,
  id: "K7QM29XD",
  app: "1.5.0",
  ch: "",
  build: "cia",
  model: "New 3DS XL",
  region: "EUR",
  system: "11.17.0-50E",
  luma: "13.5.0",
  kernel: "2.58-0",
  config: "fps=30\nscreens=both\nstats=on\n",
  app_log: "[2026-11-14 12:00:00.000] === app start: app 1.5.0 (cia) | New 3DS XL (model 4)\n",
  rec_log: recLog,
};

// Each request from its own address, so the rate limiter (10 a minute per address) stays out of the
// way; the limiter's own test sends from one.
let ipNumber = 0;
const nextIp = () => `198.51.100.${++ipNumber % 250}`;

function post(path: string, body: BodyInit, headers: Record<string, string> = {}) {
  return SELF.fetch(BASE + path, { method: "POST", headers: { "cf-connecting-ip": nextIp(), ...headers }, body });
}

function postReport(body: unknown) {
  return post("/v1/report", typeof body === "string" ? body : JSON.stringify(body), { "Content-Type": "application/json" });
}

function dumpBytes(size = 64): Uint8Array {
  const bytes = new Uint8Array(size);
  bytes.set([0xde, 0xc0, 0xad, 0xde, 0xfe, 0xca, 0xad, 0xde]);
  return bytes;
}

beforeEach(async () => {
  await env.DB.exec("DELETE FROM reports; DELETE FROM dumps;");
});

describe("POST /v1/report", () => {
  it("stores the report with the columns read from recorder.log", async () => {
    const response = await postReport(report);

    expect(response.status).toBe(204);
    const row = await env.DB.prepare("SELECT * FROM reports").first();
    expect(row).toMatchObject({
      install_id: "K7QM29XD",
      app_version: "1.5.0",
      channel: "",
      build: "cia",
      model: "New 3DS XL",
      region: "EUR",
      system_version: "11.17.0-50E",
      luma: "13.5.0",
      kernel: "2.58-0",
      config: report.config,
      titles: "00040000001B5100,0004000000175E00",
      starts: 1,
      recordings: 2,
      dropped: 5,
      hangs: 1,
      app_log: report.app_log,
      rec_log: recLog,
    });
    expect(row?.received_at).toMatch(/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/);
  });

  it("accepts a report with only the required fields", async () => {
    expect((await postReport({ v: 1, id: "K7QM29XD", app: "1.5.0" })).status).toBe(204);
    expect(await env.DB.prepare("SELECT titles, starts FROM reports").first()).toEqual({ titles: null, starts: 0 });
  });

  it.each([
    ["not json", "{", "bad json"],
    ["an array", [], "not an object"],
    ["another version", { ...report, v: 2 }, "unknown version"],
    ["an id with I", { ...report, id: "K7QM29XI" }, "bad id"],
    ["a short id", { ...report, id: "K7QM29X" }, "bad id"],
    ["no app", { ...report, app: undefined }, "bad app"],
    ["a number as model", { ...report, model: 4 }, "bad model"],
    ["a too long recorder log", { ...report, rec_log: "x".repeat(48 * 1024 + 1) }, "bad rec_log"],
  ])("rejects %s", async (_name, body, error) => {
    const response = await postReport(body);

    expect(response.status).toBe(400);
    expect(await response.text()).toBe(error);
    expect(await env.DB.prepare("SELECT COUNT(*) AS n FROM reports").first("n")).toBe(0);
  });

  it("refuses a body over 96 KB", async () => {
    const response = await postReport({ ...report, rec_log: "x".repeat(97 * 1024) });

    expect(response.status).toBe(413);
  });
});

describe("a repeated report", () => {
  it("replaces the row it repeats when the app sends from the same marks again", async () => {
    await postReport({ ...report, from: "1a2b3c4d:64:100/5e6f7a8b:64:2000" });
    await postReport({ ...report, from: "1a2b3c4d:64:100/5e6f7a8b:64:2000", app_log: "longer\n" });
    await postReport({ ...report, from: "1a2b3c4d:64:900/5e6f7a8b:64:2000" });
    await postReport({ ...report, id: "K7QM29XE", from: "1a2b3c4d:64:100/5e6f7a8b:64:2000" });
    await postReport(report); // no marks (an older app): never merged
    await postReport(report);

    const { results } = await env.DB.prepare("SELECT install_id, sent_from, app_log FROM reports ORDER BY id").all();
    expect(results).toEqual([
      { install_id: "K7QM29XD", sent_from: "1a2b3c4d:64:100/5e6f7a8b:64:2000", app_log: "longer\n" },
      { install_id: "K7QM29XD", sent_from: "1a2b3c4d:64:900/5e6f7a8b:64:2000", app_log: report.app_log },
      { install_id: "K7QM29XE", sent_from: "1a2b3c4d:64:100/5e6f7a8b:64:2000", app_log: report.app_log },
      { install_id: "K7QM29XD", sent_from: null, app_log: report.app_log },
      { install_id: "K7QM29XD", sent_from: null, app_log: report.app_log },
    ]);
  });
});

describe("POST /v1/dump", () => {
  const headers = { "X-Id": "K7QM29XD", "X-Name": "crash_dump_00000012.dmp", "Content-Type": "application/octet-stream" };

  it("stores a Luma dump as it came", async () => {
    const bytes = dumpBytes(1492);

    expect((await post("/v1/dump", bytes, headers)).status).toBe(204);
    const row = await env.DB.prepare("SELECT install_id, name, size, data FROM dumps").first<{ data: ArrayBuffer }>();
    expect(row).toMatchObject({ install_id: "K7QM29XD", name: "crash_dump_00000012.dmp", size: 1492 });
    expect(new Uint8Array(row!.data)).toEqual(bytes);
  });

  it("keeps a dump sent twice once", async () => {
    await post("/v1/dump", dumpBytes(), headers);
    expect((await post("/v1/dump", dumpBytes(), headers)).status).toBe(204);
    expect(await env.DB.prepare("SELECT COUNT(*) AS n FROM dumps").first("n")).toBe(1);
  });

  it("refuses a chunked body over the limit", async () => {
    const big = dumpBytes(48 * 1024 + 1);
    const stream = new ReadableStream({
      start(controller) {
        controller.enqueue(big.subarray(0, 30000));
        controller.enqueue(big.subarray(30000));
        controller.close();
      },
    });
    const response = await SELF.fetch(BASE + "/v1/dump", {
      method: "POST",
      headers: { "cf-connecting-ip": nextIp(), ...headers },
      body: stream,
      duplex: "half",
    } as RequestInit);
    expect(response.status).toBe(413);
  });

  it.each([
    ["a bad id", { ...headers, "X-Id": "nope" }, dumpBytes(), 400],
    ["a bad name", { ...headers, "X-Name": "../x.dmp" }, dumpBytes(), 400],
    ["no Luma magic", headers, new Uint8Array(64), 400],
    ["a dump over 48 KB", headers, dumpBytes(48 * 1024 + 1), 413],
  ])("refuses %s", async (_name, h, body, status) => {
    expect((await post("/v1/dump", body, h)).status).toBe(status);
    expect(await env.DB.prepare("SELECT COUNT(*) AS n FROM dumps").first("n")).toBe(0);
  });
});

describe("routing", () => {
  it("answers health", async () => {
    const response = await SELF.fetch(BASE + "/v1/health");
    expect(await response.text()).toBe("ok");
  });

  it("has 404 and 405", async () => {
    expect((await SELF.fetch(BASE + "/v1/other")).status).toBe(404);
    expect((await SELF.fetch(BASE + "/v1/report")).status).toBe(405);
  });
});

describe("expire", () => {
  it("drops log text and dumps older than 180 days and keeps the statistics", async () => {
    await postReport(report);
    await postReport(report);
    await post("/v1/dump", dumpBytes(), { "X-Id": "K7QM29XD", "X-Name": "crash_dump_00000001.dmp" });
    await post("/v1/dump", dumpBytes(), { "X-Id": "K7QM29XD", "X-Name": "crash_dump_00000002.dmp" });
    await env.DB.exec("UPDATE reports SET received_at = '2020-01-01T00:00:00Z' WHERE id = (SELECT MIN(id) FROM reports)");
    await env.DB.exec("UPDATE dumps SET received_at = '2020-01-01T00:00:00Z' WHERE name = 'crash_dump_00000001.dmp'");

    await expire(env.DB);

    const { results } = await env.DB.prepare("SELECT app_log IS NULL AS gone, rec_log IS NULL AS rgone, recordings, config FROM reports ORDER BY id").all();
    expect(results).toEqual([
      { gone: 1, rgone: 1, recordings: 2, config: report.config },
      { gone: 0, rgone: 0, recordings: 2, config: report.config },
    ]);
    const { results: dumps } = await env.DB.prepare("SELECT name FROM dumps").all();
    expect(dumps).toEqual([{ name: "crash_dump_00000002.dmp" }]);
  });
});

describe("rate limit", () => {
  it("answers 429 past 10 POSTs a minute from one address", async () => {
    const statuses: number[] = [];
    for (let i = 0; i < 12; i++) {
      const response = await SELF.fetch(BASE + "/v1/report", {
        method: "POST",
        headers: { "cf-connecting-ip": "203.0.113.7" },
        body: "{}",
      });
      statuses.push(response.status);
    }
    expect(statuses.slice(0, 10).every((s) => s !== 429)).toBe(true);
    expect(statuses.slice(10)).toEqual([429, 429]);
  });
});

describe("extract", () => {
  it("does not count drop counters of other lines as recordings", () => {
    expect(extract("video drops: ring full 0, dropped 9\n")).toMatchObject({ recordings: 0, dropped: 0 });
  });

  it("handles no log", () => {
    expect(extract(null)).toEqual({ titles: null, starts: 0, recordings: 0, dropped: 0, hangs: 0 });
  });
});
