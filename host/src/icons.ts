// Button icons: 16x16 pixel art in host/icons/<name>.txt, one row per line ('#' = ink,
// '.' = empty). The deck draws them in its own bitmap style; they are sent as 64 hex digits
// (16 rows x 16 bits, the most significant bit is the leftmost pixel), so new icons never
// need a reflash.
import { existsSync, readFileSync } from "node:fs";

const SIZE = 16;
const cache = new Map<string, string>();

export function iconHex(name: string): string {
  const cached = cache.get(name);
  if (cached) return cached;
  if (!/^[a-z0-9-]{1,32}$/.test(name)) throw new Error(`bad icon name ${JSON.stringify(name)}`);
  const file = new URL(`../icons/${name}.txt`, import.meta.url);
  if (!existsSync(file)) throw new Error(`no icon file icons/${name}.txt`);
  const rows = readFileSync(file, "utf8").split("\n").map((r) => r.trim()).filter(Boolean);
  if (rows.length !== SIZE || rows.some((r) => r.length !== SIZE || /[^#.]/.test(r)))
    throw new Error(`icons/${name}.txt must be ${SIZE} lines of ${SIZE} '#' or '.'`);
  const hex = rows.map((r) => parseInt(r.replace(/#/g, "1").replace(/\./g, "0"), 2).toString(16).padStart(4, "0")).join("");
  cache.set(name, hex);
  return hex;
}
