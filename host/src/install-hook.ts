// Add (or with --remove, take out) the MacDeck PermissionRequest hook in ~/.claude/settings.json,
// so Claude Code's permission prompts, questions and plan approvals also appear on the deck.
// A backup of the settings file is written first; running it twice changes nothing.
//   node src/install-hook.ts [--remove] [--settings <file>]
import { copyFileSync, existsSync, readFileSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";

const args = process.argv.slice(2);
const remove = args.includes("--remove");
const file = args.includes("--settings") ? args[args.indexOf("--settings") + 1] : path.join(os.homedir(), ".claude", "settings.json");
const hook = new URL("./hook.ts", import.meta.url).pathname;
const command = `"${process.execPath}" "${hook}"`;
const ours = (h: { command?: string }) => typeof h.command === "string" && h.command.includes(hook);

type Entry = { matcher?: string; hooks: { type: string; command?: string; timeout?: number }[] };
const settings = existsSync(file) ? JSON.parse(readFileSync(file, "utf8")) : {};
settings.hooks ??= {};
const list: Entry[] = settings.hooks.PermissionRequest ?? [];
const installed = list.some((e) => e.hooks?.some(ours));

if (remove ? !installed : installed) {
  console.log(remove ? "The MacDeck hook is not installed; nothing to do." : "The MacDeck hook is already installed; nothing to do.");
  process.exit(0);
}
if (existsSync(file)) {
  const backup = `${file}.macdeck-${new Date().toISOString().replace(/[:.]/g, "-")}.bak`;
  copyFileSync(file, backup);
  console.log(`backup: ${backup}`);
}
if (remove) {
  settings.hooks.PermissionRequest = list.map((e) => ({ ...e, hooks: e.hooks.filter((h) => !ours(h)) })).filter((e) => e.hooks.length);
  if (!settings.hooks.PermissionRequest.length) delete settings.hooks.PermissionRequest;
} else {
  // 600 s: Claude's default; the host gives up a little earlier and lets Claude ask as usual
  settings.hooks.PermissionRequest = [...list, { matcher: "*", hooks: [{ type: "command", command, timeout: 600 }] }];
}
writeFileSync(file, JSON.stringify(settings, null, 2) + "\n");
console.log(`${remove ? "removed the MacDeck hook from" : "added the MacDeck hook to"} ${file}`);
if (!remove) console.log(`  PermissionRequest (all tools) -> ${command}`);
