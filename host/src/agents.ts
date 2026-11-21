// AI coding-agent sessions running on this Mac, read passively from the files the agents
// already write. Nothing is installed into the agents' own configuration.
//
//   Claude Code  ~/.claude/sessions/<pid>.json   one file per running session, with a live
//                status: busy | shell | waiting | idle (internal format, may change)
//   Codex        ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl   session log; a turn is
//                running between its task_started and task_complete events
import { closeSync, existsSync, openSync, readdirSync, readFileSync, readSync, statSync } from "node:fs";
import os from "node:os";
import path from "node:path";

export type AgentState = "working" | "waiting" | "idle";
export interface Agent {
  key: string;            // stable per session
  agent: string;          // "Claude", "Codex"
  state: AgentState;
  title: string;          // session name, else the project folder
  where: string;          // project folder name
  since: number;          // ms epoch when the current state began
  detail?: string;        // what it is waiting for, if known
}

const HOME = os.homedir();
const CLAUDE_SESSIONS = path.join(HOME, ".claude", "sessions");
const CODEX_HOME = path.join(HOME, ".codex");
const CODEX_IDLE_SHOWN_MS = 30 * 60_000;   // keep an idle Codex thread on the deck this long
const CODEX_STALE_MS = 15 * 60_000;        // a "running" turn with no log activity this long is assumed dead

const alive = (pid: number) => {
  try { process.kill(pid, 0); return true; } catch (e) { return (e as NodeJS.ErrnoException).code === "EPERM"; }
};
const folder = (cwd: unknown) => (typeof cwd === "string" && cwd ? path.basename(cwd) : "");

function claudeSessions(): Agent[] {
  if (!existsSync(CLAUDE_SESSIONS)) return [];
  const out: Agent[] = [];
  for (const f of readdirSync(CLAUDE_SESSIONS)) {
    if (!/^\d+\.json$/.test(f)) continue;     // never touch the *.key files next to them
    let s: Record<string, unknown>;
    try { s = JSON.parse(readFileSync(path.join(CLAUDE_SESSIONS, f), "utf8")); } catch { continue; }
    const pid = Number(s.pid);
    if (!pid || !alive(pid) || s.spare || String(s.kind ?? "").startsWith("daemon")) continue;
    const st = String(s.status ?? "idle");
    const state: AgentState = st === "waiting" ? "waiting" : st === "busy" || st === "shell" ? "working" : "idle";
    const where = folder(s.cwd);
    out.push({
      key: `claude:${s.sessionId ?? pid}`, agent: "Claude", state,
      title: typeof s.name === "string" && s.name ? s.name : where || "Claude Code",
      where, since: Number(s.statusUpdatedAt ?? s.updatedAt ?? s.startedAt ?? Date.now()),
      detail: typeof s.waitingFor === "string" ? s.waitingFor : undefined,
    });
  }
  return out;
}

// ---- Codex
function readTail(file: string, bytes: number): string {
  const fd = openSync(file, "r");
  try {
    const size = statSync(file).size, start = Math.max(0, size - bytes), buf = Buffer.alloc(size - start);
    readSync(fd, buf, 0, buf.length, start);
    return buf.toString("utf8");
  } finally { closeSync(fd); }
}
function firstLine(file: string): string {
  const fd = openSync(file, "r");
  try {
    const buf = Buffer.alloc(64 * 1024), n = readSync(fd, buf, 0, buf.length, 0), s = buf.toString("utf8", 0, n);
    return s.slice(0, s.indexOf("\n") >= 0 ? s.indexOf("\n") : n);
  } finally { closeSync(fd); }
}

let titles = new Map<string, string>(), titlesMtime = 0;
function codexTitle(id: string): string | undefined {
  const index = path.join(CODEX_HOME, "session_index.jsonl");
  try {
    const m = statSync(index).mtimeMs;
    if (m !== titlesMtime) {
      titles = new Map();
      for (const l of readFileSync(index, "utf8").split("\n")) {
        try { const o = JSON.parse(l); if (o.id && o.thread_name) titles.set(o.id, o.thread_name); } catch { /* partial line */ }
      }
      titlesMtime = m;
    }
  } catch { /* no index */ }
  return titles.get(id);
}

const metaCache = new Map<string, { id: string; cwd: string }>();
function codexSessions(): Agent[] {
  const out: Agent[] = [], now = Date.now();
  for (const back of [0, 1]) {                // today's and yesterday's folders
    const d = new Date(now - back * 86_400_000);
    const dir = path.join(CODEX_HOME, "sessions", String(d.getFullYear()),
      String(d.getMonth() + 1).padStart(2, "0"), String(d.getDate()).padStart(2, "0"));
    if (!existsSync(dir)) continue;
    for (const f of readdirSync(dir)) {
      if (!f.startsWith("rollout-") || !f.endsWith(".jsonl")) continue;
      const file = path.join(dir, f);
      let mtime: number;
      try { mtime = statSync(file).mtimeMs; } catch { continue; }
      if (now - mtime > Math.max(CODEX_IDLE_SHOWN_MS, CODEX_STALE_MS)) continue;
      let meta = metaCache.get(file);
      if (!meta) {
        try { const p = JSON.parse(firstLine(file)).payload; meta = { id: String(p.id), cwd: String(p.cwd ?? "") }; } catch { continue; }
        metaCache.set(file, meta);
      }
      let last: { type: string; at: number } | undefined;
      for (const l of readTail(file, 256 * 1024).split("\n").reverse()) {
        if (!l.includes('"task_started"') && !l.includes('"task_complete"')) continue;
        try {
          const o = JSON.parse(l), t = o.payload?.type;
          if (t === "task_started" || t === "task_complete") { last = { type: t, at: Date.parse(o.timestamp) || mtime }; break; }
        } catch { /* first, partial line of the tail */ }
      }
      const running = last?.type === "task_started" && now - mtime < CODEX_STALE_MS;
      if (!running && now - mtime > CODEX_IDLE_SHOWN_MS) continue;
      const where = folder(meta.cwd);
      out.push({ key: `codex:${meta.id}`, agent: "Codex", state: running ? "working" : "idle",
        title: codexTitle(meta.id) || where || "Codex", where, since: last?.at ?? mtime });
    }
  }
  return out;
}

const RANK: Record<AgentState, number> = { waiting: 0, working: 1, idle: 2 };

export function listAgents(): Agent[] {
  const all = [...claudeSessions(), ...codexSessions()];
  return all.sort((a, b) => RANK[a.state] - RANK[b.state] || b.since - a.since);
}

// "45s", "12m", "3h", "2d" since a timestamp
export function ago(ms: number, now = Date.now()): string {
  const s = Math.max(0, Math.round((now - ms) / 1000));
  return s < 60 ? `${s}s` : s < 3600 ? `${Math.floor(s / 60)}m` : s < 86400 ? `${Math.floor(s / 3600)}h` : `${Math.floor(s / 86400)}d`;
}

// The deck's font is ASCII only: fold accents, replace the rest, and shorten to fit.
export function ascii(s: string, max: number): string {
  const t = s.normalize("NFKD").replace(/[̀-ͯ]/g, "").replace(/[‘’]/g, "'")
    .replace(/[“”]/g, '"').replace(/[–—]/g, "-").replace(/[^\x20-\x7e]/g, "?").trim();
  return t.length > max ? t.slice(0, max - 3).trimEnd() + "..." : t;
}

// Notices worth interrupting for: a session started waiting for the user, or finished a turn.
// A new state must be seen on two polls in a row, so brief flickers don't produce notices.
export class AgentWatcher {
  private seen = new Map<string, { state: AgentState; pending?: AgentState }>();
  private primed = false;

  update(agents: Agent[]): { agent: Agent; event: "waiting" | "finished" }[] {
    const events: { agent: Agent; event: "waiting" | "finished" }[] = [], keys = new Set<string>();
    for (const a of agents) {
      keys.add(a.key);
      const prev = this.seen.get(a.key);
      if (!prev) { this.seen.set(a.key, { state: a.state }); continue; }
      if (a.state === prev.state) { prev.pending = undefined; continue; }
      if (prev.pending !== a.state) { prev.pending = a.state; continue; }
      if (this.primed && a.state === "waiting") events.push({ agent: a, event: "waiting" });
      if (this.primed && a.state === "idle" && prev.state === "working") events.push({ agent: a, event: "finished" });
      this.seen.set(a.key, { state: a.state });
    }
    for (const k of this.seen.keys()) if (!keys.has(k)) this.seen.delete(k);
    this.primed = true;
    return events;
  }
}
