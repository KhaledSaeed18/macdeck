// End-to-end tests for agent prompts: the real hook (src/hook.ts) runs as a child process, the way
// Claude Code runs it, and talks to a PromptBroker on a temporary socket; a fake deck records what
// the broker shows and answers like a tap would. Run: node --test test/
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdtempSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { after, before, test } from "node:test";
import { PromptBroker } from "../src/prompts.ts";

const HOOK = new URL("../src/hook.ts", import.meta.url).pathname;
const socket = path.join(mkdtempSync(path.join(os.tmpdir(), "macdeck-test-")), "host.sock");
const broker = new PromptBroker();
let server: ReturnType<PromptBroker["listen"]>;
let shown: string[][] = [];                          // every message "sent to the deck"
const quiet = () => {};

before(async () => { server = broker.listen(quiet, socket); await new Promise((r) => server.once("listening", r)); });
after(() => server.close());

function deck() { shown = []; broker.attach((...f) => shown.push(f.map(String))); }

// Run the hook with an event; resolves with what it printed.
function hook(event: object, onShown?: (id: string) => void): Promise<string> {
  return new Promise((resolve) => {
    const child = spawn(process.execPath, [HOOK], { env: { ...process.env, MACDECK_SOCKET: socket } });
    let out = "";
    child.stdout.on("data", (d) => { out += d; });
    child.on("exit", () => resolve(out.trim()));
    child.stdin.end(JSON.stringify({ hook_event_name: "PermissionRequest", cwd: "/Users/someone/my-app", ...event }));
    if (onShown) waitFor("ASKSHOW").then((m) => onShown(m[1]));
  });
}
async function waitFor(type: string, after = 0): Promise<string[]> {
  for (let i = 0; i < 200; i++) {
    const m = shown.slice(after).find((f) => f[0] === type);
    if (m) return m;
    await new Promise((r) => setTimeout(r, 10));
  }
  throw new Error(`deck never got ${type}`);
}
const decision = (out: string) => JSON.parse(out).hookSpecificOutput.decision;
const card = (id: string) => ({
  ask: shown.filter((f) => f[0] === "ASK" && f[1] === id).at(-1)!,
  text: shown.filter((f) => f[0] === "ASKTEXT" && f[1] === id).map((f) => f[2]),
  options: shown.filter((f) => f[0] === "ASKOPT" && f[1] === id).map((f) => f[3]),
});

const bash = { tool_name: "Bash", tool_input: { command: "npm test", description: "Run the tests" } };
const suggestion = [{ type: "addRules", rules: [{ toolName: "Bash", ruleContent: "npm test:*" }], behavior: "allow", destination: "localSettings" }];

test("command: the card shows it; Allow allows", async () => {
  deck();
  const out = await hook(bash, (id) => {
    const c = card(id);
    assert.deepEqual(c.ask.slice(2, 6), ["permission", "Claude", "my-app", "Run a command"]);
    assert.deepEqual(c.text, ["$ npm test", "Run the tests"]);
    assert.deepEqual(c.options, ["Allow", "Deny"]);           // no suggestions: no "Always"
    broker.answer(id, "0", quiet);
  });
  assert.deepEqual(decision(out), { behavior: "allow" });
  assert.ok(shown.some((f) => f[0] === "ASKDONE"));
});

test("Always remembers Claude's suggested rule", async () => {
  deck();
  const out = await hook({ ...bash, permission_suggestions: suggestion }, (id) => {
    assert.deepEqual(card(id).options, ["Allow", "Always", "Deny"]);
    const always = shown.find((f) => f[0] === "ASKOPT" && f[3] === "Always")!;
    assert.equal(always[4], "allow Bash(npm test:*) in my-app");
    broker.answer(id, "1", quiet);
  });
  assert.deepEqual(decision(out), { behavior: "allow", updatedPermissions: suggestion });
});

test("file edit: Deny denies with a message", async () => {
  deck();
  const ev = { tool_name: "Edit", tool_input: { file_path: "/Users/someone/my-app/src/a.ts", old_string: "a\nb", new_string: "c" } };
  const out = await hook(ev, (id) => {
    assert.deepEqual(card(id).text, ["~/my-app/src/a.ts", "replace 2 line(s) with 1"]);
    broker.answer(id, "1", quiet);
  });
  assert.deepEqual(decision(out), { behavior: "deny", message: "Denied on the MacDeck." });
});

test("MCP tool and web fetch read well", async () => {
  deck();
  await hook({ tool_name: "mcp__github__create_issue", tool_input: { title: "Bug" } }, (id) => {
    assert.equal(card(id).ask[5], "Use create_issue (github)");
    broker.answer(id, "0", quiet);
  });
  deck();
  await hook({ tool_name: "WebFetch", tool_input: { url: "https://example.com", prompt: "Summarise" } }, (id) => {
    assert.equal(card(id).ask[5], "Fetch a web page");
    assert.deepEqual(card(id).text, ["https://example.com", "Summarise"]);
    broker.answer(id, "0", quiet);
  });
});

test("On Mac: no decision, so Claude asks on the Mac", async () => {
  deck();
  const out = await hook(bash, (id) => broker.answer(id, "mac", quiet));
  assert.equal(out, "");
});

test("questions: one at a time, single then multi choice, answers in updatedInput", async () => {
  deck();
  const questions = [
    { question: "Which database?", header: "DB", multiSelect: false,
      options: [{ label: "Postgres", description: "Relational" }, { label: "SQLite", description: "A file" }] },
    { question: "Which checks?", header: "Checks", multiSelect: true,
      options: [{ label: "Lint", description: "" }, { label: "Types", description: "" }, { label: "Tests", description: "" }] },
  ];
  const out = await hook({ tool_name: "AskUserQuestion", tool_input: { questions } }, async (id) => {
    let c = card(id);
    assert.deepEqual(c.ask.slice(2, 9), ["question", "Claude", "my-app", "Which database?", "1", "2", "0"]);
    assert.deepEqual(c.options, ["Postgres", "SQLite"]);
    const before = shown.length;
    broker.answer(id, "1", quiet);                       // SQLite
    await waitFor("ASKSHOW", before);
    c = card(id);
    assert.deepEqual(c.ask.slice(5, 9), ["Which checks?", "2", "2", "1"]);
    broker.answer(id, "0,2", quiet);                     // Lint + Tests
  });
  const d = decision(out);
  assert.equal(d.behavior, "allow");
  assert.deepEqual(d.updatedInput.questions, questions);
  assert.deepEqual(d.updatedInput.answers, { "Which database?": "SQLite", "Which checks?": "Lint, Tests" });
});

test("plan: Approve, Auto-accept and Keep planning", async () => {
  const plan = "# Add a settings page\n\nSome intro.\n\n## Steps\n1. Add the route\n2. Store preferences\n- Update docs";
  const ev = { tool_name: "ExitPlanMode", tool_input: { plan } };
  deck();
  let out = await hook(ev, (id) => {
    const c = card(id);
    assert.deepEqual(c.ask.slice(2, 6), ["plan", "Claude", "my-app", "Approve the plan?"]);
    assert.deepEqual(c.text, ["Add a settings page", "Steps", "1. Add the route", "2. Store preferences", "- Update docs"]);
    assert.deepEqual(c.options, ["Auto-accept", "Approve", "Keep planning"]);
    broker.answer(id, "1", quiet);
  });
  assert.deepEqual(decision(out), { behavior: "allow", updatedInput: { plan },
    updatedPermissions: [{ type: "setMode", mode: "default", destination: "session" }] });
  deck();
  out = await hook(ev, (id) => broker.answer(id, "0", quiet));
  assert.deepEqual(decision(out).updatedPermissions, [{ type: "setMode", mode: "acceptEdits", destination: "session" }]);
  deck();
  out = await hook(ev, (id) => broker.answer(id, "2", quiet));
  assert.equal(decision(out).behavior, "deny");
});

test("no deck connected: the hook returns at once with no decision", async () => {
  broker.detach();
  const t = Date.now();
  assert.equal(await hook(bash), "");
  assert.ok(Date.now() - t < 2000);
});

test("Claude stops waiting (hook cancelled): the card is removed", async () => {
  deck();
  const child = spawn(process.execPath, [HOOK], { env: { ...process.env, MACDECK_SOCKET: socket } });
  child.stdin.end(JSON.stringify(bash));
  const [, id] = await waitFor("ASKSHOW");
  child.kill();
  const done = await waitFor("ASKDONE");
  assert.equal(done[1], id);
});

test("the deck reconnects while Claude waits: the card is shown again", async () => {
  deck();
  const pending = hook(bash);
  const [, id] = await waitFor("ASKSHOW");
  broker.detach();
  deck();                                                // a new deck session
  await waitFor("ASKSHOW");
  assert.equal(card(id).ask[5], "Run a command");
  broker.answer(id, "0", quiet);
  assert.deepEqual(decision(await pending), { behavior: "allow" });
});
