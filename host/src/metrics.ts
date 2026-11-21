// macOS system metrics for the deck tiles. Uses only built-in tools (vm_stat, netstat, statfs).
import { execFile } from "node:child_process";
import { statfs } from "node:fs/promises";
import os from "node:os";
import { promisify } from "node:util";

const run = promisify(execFile);

export interface Tile { label: string; value: string; detail: string; level: number }

let lastCpu = cpuTimes();
let lastNet: { rx: number; tx: number; at: number } | null = null;

function cpuTimes() {
  let idle = 0, total = 0;
  for (const c of os.cpus()) {
    const t = c.times;
    idle += t.idle;
    total += t.user + t.nice + t.sys + t.irq + t.idle;
  }
  return { idle, total };
}

export function size(bytes: number, unit = ""): string {
  const k = 1024, units = ["B", "K", "M", "G", "T"];
  let i = 0, v = bytes;
  while (v >= 1000 && i < units.length - 1) { v /= k; i++; }
  const s = v >= 100 || i === 0 ? v.toFixed(0) : v.toFixed(1);
  return `${s}${units[i]}${unit}`;
}

function cpu(): Tile {
  const now = cpuTimes();
  const dIdle = now.idle - lastCpu.idle, dTotal = now.total - lastCpu.total;
  lastCpu = now;
  const pct = dTotal > 0 ? Math.round(100 * (1 - dIdle / dTotal)) : 0;
  return { label: "CPU", value: `${pct}%`, detail: `${os.cpus().length} cores ld ${os.loadavg()[0].toFixed(1)}`, level: pct };
}

// "Memory used" close to Activity Monitor: active + wired + compressed pages.
async function memory(): Promise<Tile> {
  const { stdout } = await run("/usr/bin/vm_stat");
  const page = Number(/page size of (\d+)/.exec(stdout)?.[1] ?? 16384);
  const pages = (name: string) => Number(new RegExp(`${name}:\\s+(\\d+)`).exec(stdout)?.[1] ?? 0);
  const used = (pages("Pages active") + pages("Pages wired down") + pages("Pages occupied by compressor")) * page;
  const total = os.totalmem();
  const pct = Math.round((100 * used) / total);
  return { label: "MEMORY", value: size(used), detail: `of ${size(total)} used`, level: pct };
}

async function disk(): Promise<Tile> {
  const s = await statfs("/System/Volumes/Data");
  const total = s.blocks * s.bsize, free = s.bavail * s.bsize;
  const usedPct = Math.round(100 * (1 - free / total));
  return { label: "DISK FREE", value: size(free), detail: `of ${size(total)}`, level: usedPct };
}

// Sum link-level byte counters of physical/VPN interfaces (lo0 excluded).
async function network(): Promise<Tile> {
  const { stdout } = await run("/usr/sbin/netstat", ["-ibn"]);
  let rx = 0, tx = 0;
  const seen = new Set<string>();
  for (const line of stdout.split("\n").slice(1)) {
    const f = line.trim().split(/\s+/);
    if (f.length < 10 || !f[2]?.startsWith("<Link#") || f[0] === "lo0" || seen.has(f[0])) continue;
    seen.add(f[0]);
    // with an empty Address column there are 10 fields, otherwise 11
    const off = f.length >= 11 ? 1 : 0;
    rx += Number(f[5 + off]) || 0;
    tx += Number(f[8 + off]) || 0;
  }
  const at = Date.now() / 1000;
  let down = 0, up = 0;
  if (lastNet && at > lastNet.at) {
    down = Math.max(0, (rx - lastNet.rx) / (at - lastNet.at));
    up = Math.max(0, (tx - lastNet.tx) / (at - lastNet.at));
  }
  lastNet = { rx, tx, at };
  return { label: "NETWORK", value: size(down, "/s"), detail: `up ${size(up, "/s")}`, level: -1 };
}

export async function collect(): Promise<Tile[]> {
  const c = cpu();
  const [m, d, n] = await Promise.all([memory(), disk(), network()]);
  return [c, m, d, n];
}
