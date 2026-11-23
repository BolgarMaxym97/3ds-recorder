// In-app update (app/source/update.c).
//
// GET /v1/update                          - the signed manifest: text, the signature in X-Sig; 404 when
//                                           there is none yet
// GET /v1/update/<version>/<cia|3dsx>     - that release's file from GitHub, passed through (Range
//                                           too); 404 for a version the manifest does not name
//
// The manifest is signed on the maintainer's Mac (scripts/update-manifest.mjs), never here: the
// Worker holds no key, so a Worker that served something else could not make the app take it. The
// app checks the signature and then each file's sha256 from the manifest. The files go through the
// Worker because the console's TLS does not get through to GitHub (nor to workers.dev: plain HTTP).

export const RELEASES = "https://github.com/BolgarMaxym97/3ds-recorder/releases/download";
const FILE_PATH = /^\/v1\/update\/(\d+\.\d+\.\d+)\/(cia|3dsx)$/;

export type Fetcher = (url: string, init: RequestInit) => Promise<Response>;

/** The version line of a manifest, "" when there is none. */
export function manifestVersion(manifest: string): string {
  return /^version=(\d+\.\d+\.\d+)\s*$/m.exec(manifest)?.[1] ?? "";
}

export function manifestResponse(manifest: string, sig: string): Response {
  if (!manifestVersion(manifest) || !/^[0-9a-f]{128}$/.test(sig.trim())) return new Response(null, { status: 404 });
  return new Response(manifest, {
    headers: { "Content-Type": "text/plain; charset=utf-8", "X-Sig": sig.trim(), "Cache-Control": "no-store" },
  });
}

/** null: not an update file path. */
export async function fileResponse(
  pathname: string,
  request: Request,
  manifest: string,
  fetcher: Fetcher,
  releases = RELEASES,
): Promise<Response | null> {
  const m = FILE_PATH.exec(pathname);
  if (!m) return null;
  const [, version, ext] = m;
  if (version !== manifestVersion(manifest)) return new Response(null, { status: 404 });

  const range = request.headers.get("Range");
  const upstream = await fetcher(`${releases}/v${version}/3ds-recorder.${ext}`, {
    headers: range ? { Range: range } : {},
    redirect: "follow",
  });
  if (upstream.status === 416) return new Response(null, { status: 416 });
  if (upstream.status !== 200 && upstream.status !== 206) {
    await upstream.body?.cancel();
    return new Response(null, { status: 502 });
  }
  const headers = new Headers({ "Content-Type": "application/octet-stream" });
  for (const name of ["Content-Length", "Content-Range", "Accept-Ranges"]) {
    const value = upstream.headers.get(name);
    if (value) headers.set(name, value);
  }
  return new Response(upstream.body, { status: upstream.status, headers });
}
