// Button definitions and the allowlisted actions they run. The deck only ever sends a
// button id; the command it maps to lives here on the Mac, never on the tablet.
import { execFile } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { promisify } from "node:util";
import { iconHex } from "./icons.ts";

const run = promisify(execFile);

export type Action =
  | { type: "open-app"; app: string }
  | { type: "command"; argv: string[] };

// Up to 12 buttons: all appear on the Shortcuts page; those with `home: true` (or, if none
// are flagged, the first four) are also pinned to the Home page.
export const MAX_BUTTONS = 12;
// `icon` names a 16x16 pixel-art file in host/icons/ (shown above the label on the deck).
export interface ButtonDef { id: string; label: string; color: string; home?: boolean; icon?: string; action: Action }

export function loadButtons(dir: string): ButtonDef[] {
  const local = new URL("../config.json", dir), example = new URL("../config.example.json", dir);
  const file = existsSync(local) ? local : example;
  const cfg = JSON.parse(readFileSync(file, "utf8")) as { buttons: ButtonDef[] };
  if (!Array.isArray(cfg.buttons)) throw new Error(`${file.pathname}: "buttons" must be an array`);
  for (const b of cfg.buttons) {
    if (!/^[a-z0-9_-]{1,31}$/.test(b.id)) throw new Error(`bad button id: ${JSON.stringify(b.id)}`);
    if (!/^[0-9a-f]{6}$/i.test(b.color)) throw new Error(`bad color for ${b.id}`);
    if (b.icon) {
      try { iconHex(b.icon); } catch (e) { console.warn(`${b.id}: ${(e as Error).message}; shown without an icon`); delete b.icon; }
    }
  }
  console.log(`buttons from ${file.pathname.split("/").slice(-2).join("/")}: ${cfg.buttons.map((b) => b.id).join(", ")}`);
  if (cfg.buttons.length > MAX_BUTTONS) console.warn(`only the first ${MAX_BUTTONS} buttons are used`);
  return cfg.buttons.slice(0, MAX_BUTTONS);
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
