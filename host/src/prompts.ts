// Agent prompts on the deck. The Claude Code hook (hook.ts) connects to a local Unix socket and
// sends the PermissionRequest event; the broker shows it on the deck as a card (ASK messages),
// waits for the tap (ANSWER), and replies with Claude's decision. Replying "{}" means "no
// decision": Claude then asks on the Mac as usual. Three kinds of card:
//
//   permission  any tool call (command, file edit, web fetch, MCP tool...): Allow / Always / Deny
//   question    AskUserQuestion: each question in turn, its options as tiles (single or multi)
//   plan        ExitPlanMode: Auto-accept / Approve / Keep planning
//
// Every card also has "On Mac" on the deck. A card disappears when Claude stops waiting (the hook
// is cancelled, e.g. because the question was answered on the Mac) and is re-sent if the deck
// reconnects while Claude is still waiting.
import { chmodSync, existsSync, mkdirSync, unlinkSync } from "node:fs";
import net from "node:net";
import path from "node:path";
import { ascii } from "./agents.ts";
import { SOCKET } from "./socket.ts";

type Send = (...fields: (string | number)[]) => void;
type Json = Record<string, any>;
type Decision = Json | null;                         // null: no decision, Claude asks as usual

interface Option { icon: string; label: string; detail: string; style: string; decide?: () => Decision }
interface Step { title: string; text: string[]; options: Option[]; multi: boolean; question?: string }
interface Prompt {
  id: string; agent: string; where: string; kind: "permission" | "question" | "plan";
  input: Json;                                       // the tool's input, echoed back where needed
  steps: Step[]; step: number;
  answers: Record<string, string>;                   // AskUserQuestion: question text -> label(s)
  finish: (d: Decision) => void;
}

const WAIT_MS = 9.5 * 60_000;                        // the hook's own timeout is 600 s
const home = (p: string) => p.replace(/^\/Users\/[^/]+/, "~");
const short = (s: unknown, n: number) => ascii(String(s ?? ""), n);
const output = (decision: Json) => ({ hookSpecificOutput: { hookEventName: "PermissionRequest", decision } });

// ---------------------------------------------------------------- what the agent wants, in words
function describeTool(name: string, input: Json): { title: string; text: string[] } {
  const file = home(String(input.file_path ?? input.notebook_path ?? input.path ?? ""));
  const lines = (s: unknown) => String(s ?? "").split("\n").length;
  switch (name) {
    case "Bash": return { title: "Run a command", text: [`$ ${input.command ?? ""}`, String(input.description ?? "")] };
    case "Edit": return { title: "Edit a file", text: [file, `replace ${lines(input.old_string)} line(s) with ${lines(input.new_string)}${input.replace_all ? ", every match" : ""}`] };
    case "MultiEdit": return { title: "Edit a file", text: [file, `${(input.edits ?? []).length} changes`] };
    case "Write": return { title: "Write a file", text: [file, `${lines(input.content)} lines`] };
    case "NotebookEdit": return { title: "Edit a notebook", text: [file, String(input.edit_mode ?? "edit") + " a cell"] };
    case "Read": return { title: "Read a file", text: [file] };
    case "Glob": case "Grep": return { title: "Search files", text: [String(input.pattern ?? ""), home(String(input.path ?? ""))] };
    case "WebFetch": return { title: "Fetch a web page", text: [String(input.url ?? ""), String(input.prompt ?? "")] };
    case "WebSearch": return { title: "Search the web", text: [String(input.query ?? "")] };
  }
  const mcp = /^mcp__(.+?)__(.+)$/.exec(name);
  const json = JSON.stringify(input);
  if (mcp) return { title: `Use ${mcp[2]} (${mcp[1]})`, text: [json] };
  return { title: `Use ${name}`, text: [json === "{}" ? "" : json] };
}

// "Always": say what will be remembered, from Claude's own permission suggestions
function describeSuggestions(suggestions: Json[], where: string): string {
  const scope = (d: string) => d === "session" ? "for this session" : d === "userSettings" ? "everywhere" : `in ${where}`;
  const parts = suggestions.map((s) => {
    if (s.type === "addRules") return `allow ${(s.rules ?? []).map((r: Json) => r.ruleContent ? `${r.toolName}(${r.ruleContent})` : r.toolName).join(", ")} ${scope(s.destination)}`;
    if (s.type === "addDirectories") return `allow access to ${(s.directories ?? []).map(home).join(", ")}`;
    if (s.type === "setMode") return `switch to ${s.mode} mode`;
    return "";
  }).filter(Boolean);
  return parts.join("; ") || "don't ask again for this";
}

// ---------------------------------------------------------------- building a prompt
function build(agent: string, ev: Json, finish: (d: Decision) => void, id: string): Prompt {
  const where = path.basename(String(ev.cwd ?? "")) || agent;
  const name = String(ev.tool_name ?? ""), input: Json = ev.tool_input ?? {};
  const allow = (extra: Json = {}): Decision => output({ behavior: "allow", ...extra });
  const base = { id, agent, where, input, step: 0, answers: {}, finish };

  if (name === "AskUserQuestion" && Array.isArray(input.questions) && input.questions.length) {
    const steps: Step[] = input.questions.map((q: Json) => ({
      title: q.question, text: [], multi: !!q.multiSelect, question: q.question,
      options: (q.options ?? []).slice(0, 6).map((o: Json) => ({ icon: "-", label: o.label, detail: o.description ?? "", style: "-" })),
    }));
    return { ...base, kind: "question", steps };
  }

  if (name === "ExitPlanMode") {
    const md = String(input.plan ?? "").split("\n").map((l) => l.trim()).filter(Boolean);
    const heading = (md.find((l) => l.startsWith("#")) ?? md[0] ?? "").replace(/^#+\s*/, "");
    const points = md.filter((l) => l !== md[0] && /^(#{2,}|[-*]|\d+[.)])\s/.test(l)).slice(0, 5).map((l) => l.replace(/^#+\s*/, ""));
    const mode = (m: string) => [{ type: "setMode", mode: m, destination: "session" }];
    return {
      ...base, kind: "plan", steps: [{
        title: "Approve the plan?", text: [heading, ...points], multi: false, options: [
          { icon: "auto", label: "Auto-accept", detail: "approve, and accept file edits without asking", style: "accent",
            decide: () => allow({ updatedInput: input, updatedPermissions: mode("acceptEdits") }) },
          { icon: "check", label: "Approve", detail: "approve, and ask before each edit", style: "blue",
            decide: () => allow({ updatedInput: input, updatedPermissions: mode("default") }) },
          { icon: "back", label: "Keep planning", detail: "", style: "-",
            decide: () => output({ behavior: "deny", message: "The user wants to keep planning. Refine the plan and present it again." }) },
        ],
      }],
    };
  }

  const { title, text } = describeTool(name, input);
  const suggestions: Json[] = Array.isArray(ev.permission_suggestions) ? ev.permission_suggestions : [];
  const options: Option[] = [{ icon: "check", label: "Allow", detail: "", style: "accent", decide: () => allow() }];
  if (suggestions.length)
    options.push({ icon: "always", label: "Always", detail: describeSuggestions(suggestions, where), style: "blue",
      decide: () => allow({ updatedPermissions: suggestions }) });
  options.push({ icon: "cross", label: "Deny", detail: "", style: "bad",
    decide: () => output({ behavior: "deny", message: "Denied on the MacDeck." }) });
  return { ...base, kind: "permission", steps: [{ title, text: text.filter(Boolean), options, multi: false }] };
}

// ---------------------------------------------------------------- the broker
export class PromptBroker {
  private send?: Send;
  private prompts = new Map<string, Prompt>();
  private seq = 0;

  listen(log: (...a: unknown[]) => void, socket = SOCKET): net.Server {
    mkdirSync(path.dirname(socket), { recursive: true, mode: 0o700 });
    if (existsSync(socket)) unlinkSync(socket);        // left over from a previous run
    const server = net.createServer((sock) => this.accept(sock, log));
    server.listen(socket, () => { chmodSync(socket, 0o600); log(`agent prompts: listening on ${home(socket)}`); });
    server.on("error", (e) => log("prompt socket error:", e.message));
    return server;
  }

  attach(send: Send) { this.send = send; for (const p of this.prompts.values()) this.show(p); }
  detach() { this.send = undefined; }               // prompts stay open; re-sent on reconnect

  answer(id: string, value: string, log: (...a: unknown[]) => void) {
    const p = this.prompts.get(id);
    if (!p) return;
    if (value === "mac") { log(`prompt ${id}: answer on the Mac`); p.finish(null); return; }
    const step = p.steps[p.step];
    const picked = value.split(",").map(Number).filter((i) => Number.isInteger(i) && step.options[i]);
    if (!picked.length) return;
    if (p.kind === "question") {
      p.answers[step.question!] = picked.map((i) => step.options[i].label).join(", ");
      log(`prompt ${id}: "${step.question}" -> ${p.answers[step.question!]}`);
      if (++p.step < p.steps.length) { this.show(p); return; }
      // all answered: AskUserQuestion gets its answers through updatedInput
      p.finish(output({ behavior: "allow", updatedInput: { ...p.input, answers: p.answers } }));
      return;
    }
    const opt = step.options[picked[0]];
    log(`prompt ${id}: ${opt.label}`);
    p.finish(opt.decide?.() ?? null);
  }

  private accept(sock: net.Socket, log: (...a: unknown[]) => void) {
    let buf = "", started = false;
    sock.setEncoding("utf8");
    sock.on("data", (d) => {
      buf += d;
      const nl = buf.indexOf("\n");
      if (started || nl < 0) return;
      started = true;
      let msg: Json;
      try { msg = JSON.parse(buf.slice(0, nl)); } catch { sock.end("{}\n"); return; }
      if (!this.send) { sock.end("{}\n"); return; }  // no deck: Claude asks on the Mac
      const id = `p${++this.seq}`;
      let done = false;
      const finish = (d: Decision) => {
        if (done) return;
        done = true; clearTimeout(timer);
        this.prompts.delete(id);
        this.send?.("ASKDONE", id);
        if (!sock.destroyed) sock.end((d ? JSON.stringify(d) : "{}") + "\n");
      };
      const p = build(String(msg.agent ?? "Claude"), msg.event ?? {}, finish, id);
      const timer = setTimeout(() => { log(`prompt ${id}: no answer in time`); finish(null); }, WAIT_MS);
      sock.on("close", () => { if (!done) { log(`prompt ${id}: Claude stopped waiting`); finish(null); } });
      this.prompts.set(id, p);
      log(`prompt ${id}: ${p.kind} from ${p.agent} in ${p.where}: ${p.steps[0].title}`);
      this.show(p);
    });
    sock.on("error", () => {});
  }

  private show(p: Prompt) {
    const send = this.send;
    if (!send) return;
    const s = p.steps[p.step];
    send("ASK", p.id, p.kind, p.agent, short(p.where, 38), short(s.title, 190), p.step + 1, p.steps.length, s.multi ? 1 : 0);
    for (const t of s.text.slice(0, 6)) send("ASKTEXT", p.id, short(t, 250));
    for (const o of s.options) send("ASKOPT", p.id, o.icon, short(o.label, 38), short(o.detail, 150), o.style);
    send("ASKSHOW", p.id);
  }
}
