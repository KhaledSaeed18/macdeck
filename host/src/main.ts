// MacDeck host: connects to the deck over `adb forward`, defines its buttons, streams
// metrics, clock and agent sessions once a second, and runs the allowlisted action for
// each PRESS.
// Protocol: ../../protocol/PROTOCOL.md.   Run: node src/main.ts
import { execFile } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import net from "node:net";
import os from "node:os";
import { promisify } from "node:util";
import { loadButtons, MAX_BUTTONS, runAction, type ButtonDef } from "./actions.ts";
import { AgentWatcher, ago, ascii, listAgents, type Agent } from "./agents.ts";
import { focusAgent } from "./focus.ts";
import { readMac, setMac } from "./mac.ts";
import { collect } from "./metrics.ts";

const run = promisify(execFile);
const LOCAL_PORT = Number(process.env.MACDECK_PORT ?? 18765);
const DECK_PORT = 8765;
const MAX_AGENTS = 6;   // agent cards on the deck's Agents page
const agentId = (key: string) => createHash("sha1").update(key).digest("hex").slice(0, 8);
const SERIAL = process.env.MACDECK_SERIAL ?? "";
const ADB = process.env.ADB ?? [
  `${process.env.ANDROID_HOME ?? `${os.homedir()}/Library/Android/sdk`}/platform-tools/adb`,
  "/opt/homebrew/bin/adb", "/usr/local/bin/adb",
].find((p) => existsSync(p)) ?? "adb";

const buttons: ButtonDef[] = loadButtons(import.meta.url);

// Tablet settings (brightness, dim/off timeouts, CPU mode). MacDeck OS has no writable
// storage that survives a reboot, so the host keeps them and re-sends them on connect.
const STATE = new URL("../state.json", import.meta.url);
const settings: Record<string, string> = existsSync(STATE) ? JSON.parse(readFileSync(STATE, "utf8")) : {};
const saveSettings = () => writeFileSync(STATE, JSON.stringify(settings, null, 2) + "\n");
const clean = (s: string) => s.replace(/[|\r\n]/g, " ");
const log = (...a: unknown[]) => console.log(new Date().toISOString().slice(11, 19), ...a);

async function adb(...args: string[]) {
  return run(ADB, SERIAL ? ["-s", SERIAL, ...args] : args, { timeout: 10_000 });
}

function session(sock: net.Socket): Promise<void> {
  return new Promise((resolve) => {
    let buf = "", lastRx = Date.now(), seq = 0, greeted = false, screenOn = true;
    const send = (...fields: (string | number)[]) => sock.write(fields.map((f) => clean(String(f))).join("|") + "\n");
    const watcher = new AgentWatcher();
    let shownAgents = new Map<string, Agent>();  // id -> session, for FOCUS taps

    // The Mac page: state every 2 s while the screen is on, and right after each change.
    let macBusy = false, macAgain = false, macAt = 0;
    const macTick = async (force = false): Promise<void> => {
      if (macBusy) { macAgain ||= force; return; }  // a change arrived mid-read: read once more after
      if (!force && Date.now() - macAt < 2000) return;
      macBusy = true;
      try {
        const m = await readMac();
        const b = m.battery, onoff = (v: boolean) => (v ? "on" : "off");
        send("MAC", "battery", b.pct, b.charging ? 1 : 0, b.source, b.line, b.detail);
        send("MAC", "sound", m.sound.volume, m.sound.muted ? 1 : 0);
        send("MAC", "wifi", onoff(m.wifi.on), m.wifi.detail);
        send("MAC", "airdrop", m.airdrop);
        send("MAC", "dark", onoff(m.dark));
        macAt = Date.now();
      } catch (e) { log("mac state error:", (e as Error).message); }
      finally { macBusy = false; }
      if (macAgain) { macAgain = false; return macTick(true); }
    };

    // Agent sessions: notices (and a screen wake) even while the screen is off; the cards
    // themselves only while it is on.
    const agentTick = () => {
      let agents;
      try { agents = listAgents(); } catch (e) { log("agents error:", (e as Error).message); return; }
      for (const { agent: a, event } of watcher.update(agents)) {
        const what = event === "waiting" ? "needs you" : "finished";
        log(`agent ${a.agent} ${what}: ${a.where}`);
        send("WAKE");
        send("STATUS", ascii(`${a.agent} ${what}: ${a.title}`, 60), 15);
      }
      if (!screenOn) return;
      const count = (st: string) => agents.filter((a) => a.state === st).length;
      send("AGENTS", agents.length, count("waiting"), count("working"), count("idle"));
      // With more sessions than cards, the last card becomes "+N more" on the deck.
      const cards = agents.length > MAX_AGENTS ? MAX_AGENTS - 1 : MAX_AGENTS;
      shownAgents = new Map();
      for (let s = 0; s < MAX_AGENTS; s++) {
        const a = s < cards ? agents[s] : undefined;
        if (!a) { send("AGENT", s, ""); continue; }
        const id = agentId(a.key);
        shownAgents.set(id, a);
        send("AGENT", s, a.agent, a.state, ascii(a.title, 28), ascii(a.where, 28), ago(a.since), ascii(a.detail ?? "", 28), id);
      }
    };

    const tick = async () => {
      if (!greeted) return;
      if (Date.now() - lastRx > 5000) { log("no reply from deck for 5 s, reconnecting"); sock.destroy(); return; }
      agentTick();
      if (!screenOn) { send("PING", ++seq); return; }  // screen off: heartbeat (and agent notices) only
      void macTick();
      const d = new Date();
      send("CLOCK", d.toTimeString().slice(0, 5), d.toLocaleDateString("en-GB", { weekday: "short", day: "numeric", month: "short" }).replace(",", ""));
      try {
        const { tiles, samples } = await collect();
        tiles.forEach((t, i) => send("TILE", i, t.label, t.value, t.detail, t.level));
        for (const [key, value] of Object.entries(samples)) send("SAMPLE", key, value);
      } catch (e) { log("metrics error:", (e as Error).message); }
      send("PING", ++seq);
    };
    const timer = setInterval(tick, 1000);

    sock.on("data", (chunk) => {
      lastRx = Date.now();
      buf += chunk.toString("utf8");
      let i;
      while ((i = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, i).trim();
        buf = buf.slice(i + 1);
        const [type, ...f] = line.split("|");
        if (type === "HELLO") {
          greeted = true;
          log(`deck says hello: ${f.join(" ")}`);
          for (let s = 0; s < MAX_BUTTONS; s++) {
            const b = buttons[s];
            send("BUTTON", s, b?.id ?? "", b?.label ?? "", b?.color ?? "000000", b?.home ? "home" : "");
          }
          for (const [k, v] of Object.entries(settings)) send("SETTING", k, v);
          send("STATUS", `Connected to ${os.hostname().replace(/\.local$/, "")}`);
          void tick();
        } else if (type === "SETTING" && f.length >= 2) {
          settings[f[0]] = f[1];
          saveSettings();
          log(`tablet setting ${f[0]}=${f[1]} saved`);
        } else if (type === "SCREEN") {
          const on = f[0] !== "off";
          if (on !== screenOn) log(`tablet screen ${f[0]}`);
          screenOn = on;
          if (on) void tick();
        } else if (type === "MACSET" && f.length >= 2) {
          log(`mac ${f[0]}=${f[1]}`);
          setMac(f[0], f[1]).then(
            (msg) => { if (msg) send("STATUS", msg); },
            (e) => { send("STATUS", ascii(`Mac ${f[0]} failed: ${(e as Error).message}`, 60), 8); log(`mac ${f[0]} failed:`, (e as Error).message); },
          ).finally(() => void macTick(true));
        } else if (type === "FOCUS") {
          const a = shownAgents.get(f[0]);
          if (!a) { send("STATUS", "That session is gone", 8); continue; }
          focusAgent(a).then(
            (msg) => { send("STATUS", ascii(`${msg}: ${a.title}`, 60)); log(`focus ${a.agent} ${a.where}: ${msg}`); },
            (e) => { send("STATUS", ascii(`Can't show ${a.agent}: ${(e as Error).message}`, 60), 8); log(`focus ${a.agent} failed:`, (e as Error).message); },
          );
        } else if (type === "PRESS") {
          const b = buttons.find((x) => x.id === f[0]);
          if (!b) { send("STATUS", `Unknown button ${f[0]}`, 8); continue; }
          log(`press: ${b.id}`);
          runAction(b).then(
            (msg) => { send("STATUS", msg); log(msg); },
            (e) => { send("STATUS", `${b.label} failed`, 8); log(`${b.id} failed:`, (e as Error).message); },
          );
        }
      }
    });
    sock.on("close", () => { clearInterval(timer); resolve(); });
    sock.on("error", (e) => log("socket error:", e.message));
  });
}

async function main() {
  log(`MacDeck host, adb=${ADB}${SERIAL ? `, serial=${SERIAL}` : ""}`);
  for (;;) {
    try {
      await adb("forward", `tcp:${LOCAL_PORT}`, `tcp:${DECK_PORT}`);
      const sock = net.connect(LOCAL_PORT, "127.0.0.1");
      sock.setNoDelay(true);
      await new Promise<void>((ok, fail) => { sock.once("connect", ok); sock.once("error", fail); });
      log("connected through adb forward");
      await session(sock);
      log("disconnected");
    } catch (e) {
      log("waiting for deck:", (e as Error).message.split("\n")[0]);
    }
    await new Promise((r) => setTimeout(r, 2000));
  }
}

process.on("SIGINT", async () => {
  await adb("forward", "--remove", `tcp:${LOCAL_PORT}`).catch(() => {});
  process.exit(0);
});

void main();
