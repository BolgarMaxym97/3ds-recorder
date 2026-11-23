// GET /v1/support - Help's "Support the project" links (app/source/support.c), read from the
// repo's .github/FUNDING.yml so a changed link needs neither an app release nor a deploy.
// The console cannot reach GitHub over TLS; this Worker can. One "<label> <url>" a line;
// an empty body when FUNDING.yml has none (the app then shows no topic).
import type { Fetcher } from "./update";

export const FUNDING = "https://raw.githubusercontent.com/BolgarMaxym97/3ds-recorder/main/.github/FUNDING.yml";
const MAX_LINKS = 3;
const MAX_URL = 80;

const PLATFORMS: Record<string, [string, string]> = {
  ko_fi: ["Ko-fi", "https://ko-fi.com/"],
  buy_me_a_coffee: ["Buy Me a Coffee", "https://buymeacoffee.com/"],
  patreon: ["Patreon", "https://patreon.com/"],
  liberapay: ["Liberapay", "https://liberapay.com/"],
  github: ["GitHub Sponsors", "https://github.com/sponsors/"],
};

const HOSTS: Record<string, string> = { "send.monobank.ua": "Monobank" };

function values(raw: string): string[] {
  const v = raw.replace(/\s+#.*$/, "").trim();
  if (!v || v.startsWith("#")) return [];
  const list = v.startsWith("[") && v.endsWith("]") ? v.slice(1, -1).split(",") : [v];
  return list.map((s) => s.trim().replace(/^["']|["']$/g, "")).filter(Boolean);
}

function goodUrl(url: string): URL | null {
  if (url.length > MAX_URL || !/^https:\/\/[\x21-\x7e]+$/.test(url)) return null;
  try {
    return new URL(url);
  } catch {
    return null;
  }
}

// The links in FUNDING.yml, in its order: platform names and `custom` URLs.
export function fundingLinks(yml: string): { label: string; url: string }[] {
  const links: { label: string; url: string }[] = [];
  for (const line of yml.split(/\r?\n/)) {
    const m = /^([a-z_]+):(.*)$/.exec(line);
    if (!m) continue;
    const [, key, raw] = m;
    for (const v of values(raw)) {
      let label: string, url: string;
      if (key === "custom") {
        const u = goodUrl(v);
        if (!u) continue;
        label = HOSTS[u.hostname] ?? u.hostname.replace(/^www\./, "");
        url = v;
      } else if (PLATFORMS[key] && /^[\w.-]+$/.test(v)) {
        [label, url] = [PLATFORMS[key][0], PLATFORMS[key][1] + v];
        if (!goodUrl(url)) continue;
      } else continue;
      if (links.length < MAX_LINKS) links.push({ label, url });
      if (key !== "custom") break; // a platform: its first name only
    }
  }
  return links;
}

export async function supportResponse(fetcher: Fetcher, source = FUNDING): Promise<Response> {
  let res: Response;
  try {
    res = await fetcher(source, { cf: { cacheTtl: 3600, cacheEverything: true } } as RequestInit);
  } catch {
    return new Response(null, { status: 502 });
  }
  if (res.status !== 200) {
    await res.body?.cancel();
    return new Response(null, { status: 502 });
  }
  const body = fundingLinks(await res.text()).map((l) => `${l.label} ${l.url}\n`).join("");
  return new Response(body, { headers: { "Content-Type": "text/plain; charset=utf-8", "Cache-Control": "no-store" } });
}
