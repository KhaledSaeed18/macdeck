// Button definitions and the allowlisted actions they run. The deck only ever sends a
// button id; the command it maps to lives here on the Mac, never on the tablet.
import { execFile } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { promisify } from "node:util";

const run = promisify(execFile);

export type Action =
  | { type: "open-app"; app: string }
  | { type: "command"; argv: string[] };

export interface ButtonDef { id: string; label: string; color: string; action: Action }

export function loadButtons(dir: string): ButtonDef[] {
  const local = new URL("../config.json", dir), example = new URL("../config.example.json", dir);
  const file = existsSync(local) ? local : example;
  const cfg = JSON.parse(readFileSync(file, "utf8")) as { buttons: ButtonDef[] };
  if (!Array.isArray(cfg.buttons)) throw new Error(`${file.pathname}: "buttons" must be an array`);
  for (const b of cfg.buttons) {
    if (!/^[a-z0-9_-]{1,31}$/.test(b.id)) throw new Error(`bad button id: ${JSON.stringify(b.id)}`);
    if (!/^[0-9a-f]{6}$/i.test(b.color)) throw new Error(`bad color for ${b.id}`);
  }
  console.log(`buttons from ${file.pathname.split("/").slice(-2).join("/")}: ${cfg.buttons.map((b) => b.id).join(", ")}`);
  return cfg.buttons.slice(0, 6);
}

export async function runAction(b: ButtonDef): Promise<string> {
  const a = b.action;
  if (a.type === "open-app") {
    await run("/usr/bin/open", ["-a", a.app], { timeout: 10_000 });
    return `Opened ${a.app}`;
  }
  if (a.type === "command") {
    const [cmd, ...args] = a.argv;
    await run(cmd, args, { timeout: 30_000 });
    return `${b.label}: done`;
  }
  throw new Error(`unknown action type for ${b.id}`);
}
