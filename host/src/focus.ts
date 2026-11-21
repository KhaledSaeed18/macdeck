// Bring the app that shows an agent session to the front: walk up from the session's process
// to the outermost macOS app bundle (a terminal such as Ghostty, or a desktop app such as
// Claude) and open it. Sessions without a known process are matched by working folder.
import { execFile } from "node:child_process";
import path from "node:path";
import { promisify } from "node:util";
import type { Agent } from "./agents.ts";

const run = promisify(execFile);
// An installed app's executable: the top-level bundle in /Applications or ~/Applications
// (not a helper bundle nested inside it, nor one under Application Support).
const APP = /^((?:\/Users\/[^/]+)?\/Applications\/[^/]+\.app)\/Contents\//;

interface Proc { ppid: number; args: string }
async function processes(): Promise<Map<number, Proc>> {
  const { stdout } = await run("/bin/ps", ["-axww", "-o", "pid=,ppid=,args="], { maxBuffer: 16 << 20 });
  const map = new Map<number, Proc>();
  for (const l of stdout.split("\n")) {
    const m = /^\s*(\d+)\s+(\d+)\s+(.*)$/.exec(l);
    if (m) map.set(Number(m[1]), { ppid: Number(m[2]), args: m[3] });
  }
  return map;
}

// The outermost app bundle among a process and its ancestors (launchd's direct child).
function owningApp(pid: number, procs: Map<number, Proc>): string | undefined {
  let app: string | undefined;
  for (let p = pid, hops = 0; p > 1 && hops < 64; hops++) {
    const pr = procs.get(p);
    if (!pr) break;
    const m = APP.exec(pr.args);
    if (m) app = m[1];  // keep climbing: the outermost app wins
    p = pr.ppid;
  }
  return app;
}

// Processes matching `proc` whose current directory is `cwd` (lsof -d cwd).
async function pidsInFolder(procs: Map<number, Proc>, proc: RegExp, cwd: string): Promise<number[]> {
  const candidates = [...procs].filter(([, p]) => proc.test(p.args)).map(([pid]) => pid);
  if (!candidates.length) return [];
  try {
    const { stdout } = await run("/usr/sbin/lsof", ["-a", "-d", "cwd", "-Fpn", "-p", candidates.join(",")]);
    const out: number[] = [];
    let pid = 0;
    for (const l of stdout.split("\n")) {
      if (l[0] === "p") pid = Number(l.slice(1));
      else if (l[0] === "n" && path.resolve(l.slice(1)) === path.resolve(cwd)) out.push(pid);
    }
    return out.sort((a, b) => b - a);  // newest first
  } catch { return []; }
}

// The app to open for a session: an app path or name, or an Error saying why there is none.
export async function resolveApp(a: Agent): Promise<string> {
  const f = a.focus, procs = await processes();
  const pids = f.pid ? [f.pid] : f.proc && f.cwd ? await pidsInFolder(procs, f.proc, f.cwd) : [];
  for (const pid of pids) {
    const app = owningApp(pid, procs);
    if (app) return app;
  }
  if (f.app) return f.app;
  throw new Error(pids.length ? "its terminal is not an app (tmux or ssh?)" : "no running process found");
}

export async function focusAgent(a: Agent): Promise<string> {
  const app = await resolveApp(a);
  await run("/usr/bin/open", ["-a", app], { timeout: 10_000 });
  return `Opened ${path.basename(app, ".app")}`;
}
