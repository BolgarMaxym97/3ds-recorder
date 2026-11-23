// The in-app update's manifest (src/update.ts, app/source/update_core.c), signed on the
// maintainer's Mac. The Worker only serves what this writes; it never holds the key.
//
//   node scripts/update-manifest.mjs keygen [--force]
//       makes the Ed25519 key pair: the private key to ~/.config/3ds-recorder/update-key.json (0600;
//       keep a copy in a password manager), the public key printed as C for app/source/update_key.h.
//       Once: a new key needs an app release before manifests signed with it are taken.
//   node scripts/update-manifest.mjs sign <version> <3ds-recorder.cia> <3ds-recorder.3dsx> <notes dir>
//       [--key <file>] [--out <dir>]
//       checks the notes (tools/i18n.py notes: <dir>/<lang>.txt, one point a line), then writes
//       <out>/latest.txt and <out>/latest.sig.txt (default out: update/). Then deploy the Worker.
//
// The manifest:
//   version=1.7.0
//   cia=<bytes> <sha256 hex>
//   3dsx=<bytes> <sha256 hex>
//   notes.<lang>=<line>|<line>...
import { createHash, createPrivateKey, createPublicKey, generateKeyPairSync, sign, verify } from "node:crypto";
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import { homedir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, "../..");
const KEY = join(homedir(), ".config/3ds-recorder/update-key.json");

function option(name, fallback) {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : fallback;
}

function die(message) {
  console.error(message);
  process.exit(1);
}

function publicHex(privateJwk) {
  return Buffer.from(createPublicKey(createPrivateKey({ key: privateJwk, format: "jwk" })).export({ format: "jwk" }).x, "base64url").toString("hex");
}

function cArray(hex) {
  const bytes = hex.match(/../g).map((b) => `0x${b}`);
  const rows = [];
  for (let i = 0; i < bytes.length; i += 8) rows.push("    " + bytes.slice(i, i + 8).join(", ") + ",");
  return rows.join("\n");
}

function keygen() {
  const file = option("--key", KEY);
  if (existsSync(file) && !process.argv.includes("--force")) die(`${file} already exists (--force makes a new one).`);
  mkdirSync(dirname(file), { recursive: true });
  const { privateKey } = generateKeyPairSync("ed25519");
  const jwk = privateKey.export({ format: "jwk" });
  writeFileSync(file, JSON.stringify(jwk), { mode: 0o600 });
  console.log(`Private key: ${file} (keep a copy in a password manager; never commit it).`);
  console.log("Public key for app/source/update_key.h:\n" + cArray(publicHex(jwk)));
}

function fileLine(name, path) {
  const data = readFileSync(path);
  return `${name}=${data.length} ${createHash("sha256").update(data).digest("hex")}`;
}

function signRelease() {
  const [version, cia, x3dsx, notesDir] = process.argv.slice(3);
  if (!/^\d+\.\d+\.\d+$/.test(version ?? "") || !cia || !x3dsx || !notesDir)
    die("usage: sign <version> <3ds-recorder.cia> <3ds-recorder.3dsx> <notes dir> [--key <file>] [--out <dir>]");
  try {
    execFileSync("python3", [join(REPO, "tools/i18n.py"), "notes", notesDir], { stdio: "inherit" });
  } catch {
    die("The notes have problems (above): nothing signed.");
  }
  const lines = [`version=${version}`, fileLine("cia", cia), fileLine("3dsx", x3dsx)];
  for (const f of readdirSync(notesDir).filter((f) => f.endsWith(".txt")).sort()) {
    const points = readFileSync(join(notesDir, f), "utf8").split("\n").map((l) => l.trim()).filter(Boolean);
    lines.push(`notes.${f.slice(0, -4)}=${points.join("|")}`);
  }
  const manifest = lines.join("\n") + "\n";

  const keyFile = option("--key", KEY);
  if (!existsSync(keyFile)) die(`No key at ${keyFile}: run keygen first.`);
  const jwk = JSON.parse(readFileSync(keyFile, "utf8"));
  const privateKey = createPrivateKey({ key: jwk, format: "jwk" });
  const sig = sign(null, Buffer.from(manifest), privateKey);
  if (!verify(null, Buffer.from(manifest), createPublicKey(privateKey), sig)) die("The signature does not check.");

  const out = resolve(option("--out", join(HERE, "../update")));
  mkdirSync(out, { recursive: true });
  writeFileSync(join(out, "latest.txt"), manifest);
  writeFileSync(join(out, "latest.sig.txt"), sig.toString("hex") + "\n");
  console.log(manifest + `Signed with the key ${publicHex(jwk).slice(0, 16)}… → ${out}/latest.txt, latest.sig.txt`);
}

const command = process.argv[2];
if (command === "keygen") keygen();
else if (command === "sign") signRelease();
else die("usage: keygen [--force] | sign <version> <cia> <3dsx> <notes dir> [--key <file>] [--out <dir>]");
