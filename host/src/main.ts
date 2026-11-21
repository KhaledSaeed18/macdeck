// MacDeck host: connects to the deck over `adb forward`, defines its buttons, streams
// metrics + clock once a second, and runs the allowlisted action for each PRESS.
// Protocol: ../../protocol/PROTOCOL.md.   Run: node src/main.ts
import { execFile } from "node:child_process";
import { existsSync } from "node:fs";
import net from "node:net";
import os from "node:os";
import { promisify } from "node:util";
import { loadButtons, runAction, type ButtonDef } from "./actions.ts";
import { collect } from "./metrics.ts";

const run = promisify(execFile);
const LOCAL_PORT = Number(process.env.MACDECK_PORT ?? 18765);
const DECK_PORT = 8765;
const SERIAL = process.env.MACDECK_SERIAL ?? "";
const ADB = process.env.ADB ?? [
  `${process.env.ANDROID_HOME ?? `${os.homedir()}/Library/Android/sdk`}/platform-tools/adb`,
  "/opt/homebrew/bin/adb", "/usr/local/bin/adb",
].find((p) => existsSync(p)) ?? "adb";

const buttons: ButtonDef[] = loadButtons(import.meta.url);
const clean = (s: string) => s.replace(/[|\r\n]/g, " ");
const log = (...a: unknown[]) => console.log(new Date().toISOString().slice(11, 19), ...a);

async function adb(...args: string[]) {
  return run(ADB, SERIAL ? ["-s", SERIAL, ...args] : args, { timeout: 10_000 });
}

function session(sock: net.Socket): Promise<void> {
  return new Promise((resolve) => {
    let buf = "", lastRx = Date.now(), seq = 0, greeted = false;
    const send = (...fields: (string | number)[]) => sock.write(fields.map((f) => clean(String(f))).join("|") + "\n");

    const tick = async () => {
      if (!greeted) return;
      if (Date.now() - lastRx > 5000) { log("no reply from deck for 5 s, reconnecting"); sock.destroy(); return; }
      const d = new Date();
      send("CLOCK", d.toTimeString().slice(0, 5), d.toLocaleDateString("en-GB", { weekday: "short", day: "numeric", month: "short" }).replace(",", ""));
      try {
        (await collect()).forEach((t, i) => send("TILE", i, t.label, t.value, t.detail, t.level));
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
          for (let s = 0; s < 6; s++) {
            const b = buttons[s];
            send("BUTTON", s, b?.id ?? "", b?.label ?? "", b?.color ?? "000000");
          }
          send("STATUS", `Connected to ${os.hostname().replace(/\.local$/, "")}`);
          void tick();
        } else if (type === "PRESS") {
          const b = buttons.find((x) => x.id === f[0]);
          if (!b) { send("STATUS", `Unknown button ${f[0]}`); continue; }
          log(`press: ${b.id}`);
          runAction(b).then(
            (msg) => { send("STATUS", msg); log(msg); },
            (e) => { send("STATUS", `${b.label} failed`); log(`${b.id} failed:`, (e as Error).message); },
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
