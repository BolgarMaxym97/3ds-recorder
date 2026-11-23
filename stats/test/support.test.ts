import { SELF } from "cloudflare:test";
import repoFunding from "../../.github/FUNDING.yml?raw";
import { describe, expect, it } from "vitest";
import { FUNDING, fundingLinks, supportResponse } from "../src/support";
import type { Fetcher } from "../src/update";

const TEMPLATE = `# These are supported funding model platforms

github: # Replace with up to 4 GitHub Sponsors-enabled usernames e.g., [user1, user2]
patreon: # Replace with a single Patreon username
ko_fi: # Replace with a single Ko-fi username
custom: # Replace with up to 4 custom sponsorship URLs e.g., ['link1', 'link2']
`;

const OURS = `github: # Replace with up to 4 GitHub Sponsors-enabled usernames e.g., [user1, user2]
ko_fi: maksymbolhar
custom: ["https://send.monobank.ua/jar/9jJDGho7H8"]
`;

function fake(status: number, body = "") {
  const calls: { url: string; init: RequestInit }[] = [];
  const fetcher: Fetcher = async (url, init) => {
    calls.push({ url, init });
    return new Response(body, { status });
  };
  return { calls, fetcher };
}

describe("fundingLinks", () => {
  it("reads ko_fi and the Monobank jar", () => {
    expect(fundingLinks(OURS)).toEqual([
      { label: "Ko-fi", url: "https://ko-fi.com/maksymbolhar" },
      { label: "Monobank", url: "https://send.monobank.ua/jar/9jJDGho7H8" },
    ]);
  });

  it("nothing from GitHub's empty template", () => {
    expect(fundingLinks(TEMPLATE)).toEqual([]);
    expect(fundingLinks("")).toEqual([]);
  });

  it("custom: one URL, a list, a label from the host; bad URLs skipped", () => {
    expect(fundingLinks("custom: https://www.example.com/give")).toEqual([{ label: "example.com", url: "https://www.example.com/give" }]);
    expect(fundingLinks(`custom: ['http://a.com', "https://b.com/x y", 'https://c.com/${"z".repeat(80)}', https://d.com]`)).toEqual([
      { label: "d.com", url: "https://d.com" },
    ]);
  });

  it("platforms: the first name; a bad name skipped", () => {
    expect(fundingLinks("github: [one, two]\npatreon: a/b")).toEqual([{ label: "GitHub Sponsors", url: "https://github.com/sponsors/one" }]);
  });

  it("at most 3 links", () => {
    expect(fundingLinks("custom: [https://a.com, https://b.com, https://c.com, https://d.com]").length).toBe(3);
  });

  it("the repo's FUNDING.yml gives links", () => {
    expect(fundingLinks(repoFunding).length).toBeGreaterThan(0);
  });
});

describe("supportResponse", () => {
  it("one line a link, cached FUNDING.yml from GitHub", async () => {
    const { calls, fetcher } = fake(200, OURS);
    const r = await supportResponse(fetcher);
    expect(r.status).toBe(200);
    expect(await r.text()).toBe("Ko-fi https://ko-fi.com/maksymbolhar\nMonobank https://send.monobank.ua/jar/9jJDGho7H8\n");
    expect(calls[0].url).toBe(FUNDING);
    expect((calls[0].init as { cf?: { cacheTtl?: number } }).cf?.cacheTtl).toBe(3600);
  });

  it("an empty body without links", async () => {
    const r = await supportResponse(fake(200, TEMPLATE).fetcher);
    expect(r.status).toBe(200);
    expect(await r.text()).toBe("");
  });

  it("GitHub failing is a 502", async () => {
    expect((await supportResponse(fake(404).fetcher)).status).toBe(502);
    expect((await supportResponse(async () => Promise.reject(new Error("down")))).status).toBe(502);
  });

  it("the Worker refuses other methods", async () => {
    expect((await SELF.fetch("https://stats.example/v1/support", { method: "POST" })).status).toBe(405);
  });
});
