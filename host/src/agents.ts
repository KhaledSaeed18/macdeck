// AI coding-agent sessions running on this Mac, read passively from the files the agents
// already write. Nothing is installed into the agents' own configuration.
//
//   Claude Code  ~/.claude/sessions/<pid>.json   one file per running session, with a live
//                status: busy | shell | waiting | idle (internal format, may change)
//   Codex        ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl   session log; a turn is
//                running between its task_started and task_complete events
//   opencode     ~/.local/share/opencode/opencode.db (SQLite, opened read-only): a session
//                is working until its newest session_message row is an "idle" one
//   Copilot CLI  ~/.copilot/session-state/<id>/events.jsonl: a turn runs from user.message /
//                assistant.turn_start until assistant.turn_end (or session.shutdown)
//   Qwen Code    ~/.qwen/projects/<dir>/chats/<id>.jsonl: the model's turn continues while its
//                last message calls a tool; a final text-only reply ends it
//
// One rule for every agent: a session is listed while it is working or waiting for the user,
// and for IDLE_SHOWN_MS after it went idle; then it is removed (listAgents).
import { closeSync, existsSync, openSync, readdirSync, readFileSync, readSync, statSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { DatabaseSync, type StatementSync } from "node:sqlite";

export type AgentState = "working" | "waiting" | "idle";
export interface Agent {
  key: string;            // stable per session
  agent: string;          // "Claude", "Codex", ...
  state: AgentState;
  title: string;          // session name, else the project folder
  where: string;          // project folder name
  since: number;          // ms epoch when the current state began
  detail?: string;        // what it is waiting for, if known
}

const HOME = os.homedir();
const CLAUDE_SESSIONS = path.join(HOME, ".claude", "sessions");
const CODEX_HOME = path.join(HOME, ".codex");
const OPENCODE_DB = path.join(HOME, ".local", "share", "opencode", "opencode.db");
const COPILOT_SESSIONS = path.join(HOME, ".copilot", "session-state");
const QWEN_PROJECTS = path.join(HOME, ".qwen", "projects");
const IDLE_SHOWN_MS = 30 * 60_000;  // keep an idle session on the deck this long, then drop it
const STALE_MS = 15 * 60_000;    // a "working" session with no activity this long is assumed dead

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
function readHead(file: string, bytes: number): string {
  const fd = openSync(file, "r");
  try {
    const buf = Buffer.alloc(bytes), n = readSync(fd, buf, 0, buf.length, 0);
    return buf.toString("utf8", 0, n);
  } finally { closeSync(fd); }
}

// A Codex thread's meta line, and its first real prompt (skipping the injected <...> context
// blocks): the title for threads without a name, such as `codex exec` runs.
interface CodexMeta { id: string; cwd: string; prompt?: string }
function codexMeta(file: string): CodexMeta | undefined {
  const lines = readHead(file, 512 * 1024).split("\n");
  let meta: CodexMeta;
  try { const p = JSON.parse(lines[0]).payload; meta = { id: String(p.id), cwd: String(p.cwd ?? "") }; } catch { return undefined; }
  for (const l of lines) {
    if (!l.includes('"role":"user"')) continue;
    try {
      const text = JSON.parse(l).payload?.content?.find((c: { text?: string }) => c.text && !c.text.startsWith("<"))?.text;
      if (text) { meta.prompt = text.replace(/\s+/g, " ").trim(); break; }
    } catch { /* cut-off last line */ }
  }
  return meta;
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

const metaCache = new Map<string, CodexMeta>();
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
      if (now - mtime > Math.max(IDLE_SHOWN_MS, STALE_MS)) continue;
      let meta = metaCache.get(file);
      if (!meta?.prompt) {                    // re-read until the prompt has been written
        meta = codexMeta(file);
        if (!meta) continue;
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
      const running = last?.type === "task_started" && now - mtime < STALE_MS;
      if (!running && now - mtime > IDLE_SHOWN_MS) continue;
      const where = folder(meta.cwd);
      out.push({ key: `codex:${meta.id}`, agent: "Codex", state: running ? "working" : "idle",
        title: codexTitle(meta.id) || meta.prompt || where || "Codex", where, since: last?.at ?? mtime });
    }
  }
  return out;
}

// ---- opencode (top-level sessions only; subagent sessions have a parent_id)
let opencodeQuery: StatementSync | undefined;
function opencodeSessions(): Agent[] {
  if (!existsSync(OPENCODE_DB)) return [];
  opencodeQuery ??= new DatabaseSync(OPENCODE_DB, { readOnly: true }).prepare(`
    SELECT s.id, s.title, s.directory,
      (SELECT type FROM session_message m WHERE m.session_id = s.id ORDER BY seq DESC LIMIT 1) AS last,
      (SELECT max(time_updated) FROM session_message m WHERE m.session_id = s.id) AS active,
      (SELECT max(time_created) FROM session_message m WHERE m.session_id = s.id AND type = 'user') AS asked
    FROM session_v2 s
    WHERE s.parent_id IS NULL AND s.time_archived IS NULL AND s.time_updated > ?`);
  const now = Date.now(), out: Agent[] = [];
  for (const r of opencodeQuery.all(now - IDLE_SHOWN_MS) as Record<string, string | number | null>[]) {
    const active = Number(r.active ?? 0), working = r.last !== null && r.last !== "idle" && now - active < STALE_MS;
    if (!working && now - active > IDLE_SHOWN_MS) continue;
    const where = folder(r.directory);
    out.push({ key: `opencode:${r.id}`, agent: "opencode", state: working ? "working" : "idle",
      title: String(r.title || where || "opencode"), where, since: working ? Number(r.asked ?? active) : active });
  }
  return out;
}

// ---- JSONL session logs (Copilot, Qwen): recent files, newest records first
function recentLogs(files: string[]): { file: string; mtime: number }[] {
  const now = Date.now(), out: { file: string; mtime: number }[] = [];
  for (const file of files) {
    try { const mtime = statSync(file).mtimeMs; if (now - mtime <= Math.max(IDLE_SHOWN_MS, STALE_MS)) out.push({ file, mtime }); } catch { /* gone */ }
  }
  return out;
}
function* newestRecords(file: string): Generator<Record<string, any>> {
  for (const l of readTail(file, 256 * 1024).split("\n").reverse()) {
    if (!l.trim()) continue;
    try { yield JSON.parse(l); } catch { /* first, partial line of the tail */ }
  }
}
function* oldestRecords(file: string): Generator<Record<string, any>> {
  for (const l of readHead(file, 256 * 1024).split("\n")) {
    try { yield JSON.parse(l); } catch { /* cut-off last line */ }
  }
}
const listDir = (dir: string) => (existsSync(dir) ? readdirSync(dir) : []);
const promptCache = new Map<string, string>();
function firstPrompt(file: string, pick: (r: Record<string, any>) => unknown): string | undefined {
  if (promptCache.has(file)) return promptCache.get(file);
  for (const r of oldestRecords(file)) {
    const t = pick(r);
    const p = typeof t === "string" ? t.replace(/\s+/g, " ").trim() : "";
    if (p.length >= 3) { promptCache.set(file, p); return p; }  // skip "y", "ok": not a useful title
  }
  return undefined;
}

function copilotSessions(): Agent[] {
  const files = listDir(COPILOT_SESSIONS).map((d) => path.join(COPILOT_SESSIONS, d, "events.jsonl"));
  const out: Agent[] = [], now = Date.now();
  for (const { file, mtime } of recentLogs(files)) {
    let last: Record<string, any> | undefined;
    for (const r of newestRecords(file)) {
      if (["user.message", "assistant.turn_start", "assistant.turn_end", "session.shutdown"].includes(r.type)) { last = r; break; }
    }
    if (!last) continue;
    const working = (last.type === "user.message" || last.type === "assistant.turn_start") && now - mtime < STALE_MS;
    let cwd = "";
    try { cwd = /^cwd: (.*)$/m.exec(readFileSync(path.join(path.dirname(file), "workspace.yaml"), "utf8"))?.[1] ?? ""; } catch { /* none */ }
    const where = folder(cwd);
    out.push({ key: `copilot:${path.basename(path.dirname(file))}`, agent: "Copilot", state: working ? "working" : "idle",
      title: firstPrompt(file, (r) => r.type === "user.message" && r.data?.content) || where || "Copilot",
      where, since: Date.parse(last.timestamp) || mtime });
  }
  return out;
}

function qwenSessions(): Agent[] {
  const files = listDir(QWEN_PROJECTS).flatMap((p) =>
    listDir(path.join(QWEN_PROJECTS, p, "chats")).filter((f) => f.endsWith(".jsonl")).map((f) => path.join(QWEN_PROJECTS, p, "chats", f)));
  const out: Agent[] = [], now = Date.now();
  for (const { file, mtime } of recentLogs(files)) {
    let last: Record<string, any> | undefined;
    for (const r of newestRecords(file)) if (r.type !== "system") { last = r; break; }  // skip telemetry
    if (!last) continue;
    const callsTool = (last.message?.parts ?? []).some((p: Record<string, unknown>) => p.functionCall);
    const working = (last.type !== "assistant" || callsTool) && now - mtime < STALE_MS;
    const where = folder(last.cwd);
    out.push({ key: `qwen:${last.sessionId ?? file}`, agent: "Qwen", state: working ? "working" : "idle",
      title: firstPrompt(file, (r) => r.type === "user" && r.message?.parts?.find((p: { text?: string }) => p.text)?.text) || where || "Qwen",
      where, since: Date.parse(last.timestamp) || mtime });
  }
  return out;
}

const RANK: Record<AgentState, number> = { waiting: 0, working: 1, idle: 2 };

// One unreadable source (a format change after an agent update) must not hide the others.
function safe(name: string, read: () => Agent[]): Agent[] {
  try { return read(); } catch (e) { console.warn(`agents: ${name}: ${(e as Error).message}`); return []; }
}

export function listAgents(): Agent[] {
  const now = Date.now(), all = [...safe("claude", claudeSessions), ...safe("codex", codexSessions), ...safe("opencode", opencodeSessions),
    ...safe("copilot", copilotSessions), ...safe("qwen", qwenSessions)];
  return all.filter((a) => a.state !== "idle" || now - a.since <= IDLE_SHOWN_MS).sort((a, b) => RANK[a.state] - RANK[b.state] || b.since - a.since);
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
