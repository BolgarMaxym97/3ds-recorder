import { SELF } from "cloudflare:test";
import { describe, expect, it } from "vitest";
import { fileResponse, manifestResponse, manifestVersion, RELEASES, type Fetcher } from "../src/update";

const MANIFEST = "version=1.7.0\ncia=8352704 " + "a".repeat(64) + "\nnotes.en=One|Two\n";
const SIG = "0f".repeat(64);

function fake(status: number, body = "data", headers: Record<string, string> = {}) {
  const calls: { url: string; init: RequestInit }[] = [];
  const fetcher: Fetcher = async (url, init) => {
    calls.push({ url, init });
    return new Response(body, { status, headers });
  };
  return { calls, fetcher };
}

describe("manifest", () => {
  it("reads the version line", () => {
    expect(manifestVersion(MANIFEST)).toBe("1.7.0");
    expect(manifestVersion("")).toBe("");
    expect(manifestVersion("version=1.7\n")).toBe("");
  });

  it("serves the manifest with its signature", async () => {
    const r = manifestResponse(MANIFEST, SIG + "\n");
    expect(r.status).toBe(200);
    expect(r.headers.get("X-Sig")).toBe(SIG);
    expect(await r.text()).toBe(MANIFEST);
  });

  it("is 404 without a manifest or a signature", () => {
    expect(manifestResponse("", "").status).toBe(404);
    expect(manifestResponse(MANIFEST, "").status).toBe(404);
    expect(manifestResponse(MANIFEST, "zz").status).toBe(404);
  });

  it("the deployed Worker answers /v1/update (404 until a release is signed)", async () => {
    const r = await SELF.fetch("https://stats.example/v1/update");
    expect([200, 404]).toContain(r.status);
    await r.body?.cancel();
    expect((await SELF.fetch("https://stats.example/v1/update", { method: "POST" })).status).toBe(405);
  });
});

describe("file", () => {
  it("passes the release file through", async () => {
    const { calls, fetcher } = fake(200, "cia bytes", { "Content-Length": "9", "Accept-Ranges": "bytes" });
    const r = await fileResponse("/v1/update/1.7.0/cia", new Request("https://x/"), MANIFEST, fetcher);
    expect(r!.status).toBe(200);
    expect(await r!.text()).toBe("cia bytes");
    expect(r!.headers.get("Accept-Ranges")).toBe("bytes");
    expect(calls[0].url).toBe(`${RELEASES}/v1.7.0/3ds-recorder.cia`);
  });

  it("passes Range on and a 206 back", async () => {
    const { calls, fetcher } = fake(206, "tail", { "Content-Range": "bytes 100-103/104" });
    const req = new Request("https://x/", { headers: { Range: "bytes=100-" } });
    const r = await fileResponse("/v1/update/1.7.0/3dsx", req, MANIFEST, fetcher);
    expect(r!.status).toBe(206);
    expect(r!.headers.get("Content-Range")).toBe("bytes 100-103/104");
    expect(new Headers(calls[0].init.headers).get("Range")).toBe("bytes=100-");
    expect(calls[0].url).toBe(`${RELEASES}/v1.7.0/3ds-recorder.3dsx`);
  });

  it("only the manifest's version: not an open proxy", async () => {
    const { calls, fetcher } = fake(200);
    expect((await fileResponse("/v1/update/1.6.0/cia", new Request("https://x/"), MANIFEST, fetcher))!.status).toBe(404);
    expect((await fileResponse("/v1/update/1.7.0/cia", new Request("https://x/"), "", fetcher))!.status).toBe(404);
    expect(await fileResponse("/v1/update/1.7.0/zip", new Request("https://x/"), MANIFEST, fetcher)).toBeNull();
    expect(await fileResponse("/v1/update/../x/cia", new Request("https://x/"), MANIFEST, fetcher)).toBeNull();
    expect(calls.length).toBe(0);
  });

  it("an upstream error is a 502", async () => {
    const { fetcher } = fake(404);
    expect((await fileResponse("/v1/update/1.7.0/cia", new Request("https://x/"), MANIFEST, fetcher))!.status).toBe(502);
  });

  it("unknown update paths are 404 on the Worker", async () => {
    const r = await SELF.fetch("https://stats.example/v1/update/1.0.0/exe");
    expect(r.status).toBe(404);
  });
});
