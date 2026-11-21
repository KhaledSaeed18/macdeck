// The deck's Mac page: battery, sound, Wi-Fi, AirDrop, dark mode and lock.
// Everything uses tools that ship with macOS. The deck only sends a key and a value
// (MACSET|key|value); each key maps to one fixed command here, run without a shell.
import { execFile } from "node:child_process";
import { existsSync } from "node:fs";
import { promisify } from "node:util";

const run = promisify(execFile);
const sh = async (cmd: string, args: string[]) => (await run(cmd, args, { timeout: 8000 })).stdout;
const osa = (...lines: string[]) => sh("/usr/bin/osascript", lines.flatMap((l) => ["-e", l]));

export interface MacState {
  battery: { pct: number; charging: boolean; source: string; line: string; detail: string };
  sound: { volume: number; muted: boolean };
  wifi: { on: boolean; detail: string };
  airdrop: "off" | "contacts" | "everyone";
  dark: boolean;
}

// ---------------------------------------------------------------- battery (pmset, ioreg)
const int = (xml: string, key: string, from = 0) => {
  const m = new RegExp(`<key>${key}</key>\\s*<integer>(-?\\d+)</integer>`).exec(xml.slice(from));
  return m ? Number(m[1]) : undefined;
};

async function battery(): Promise<MacState["battery"]> {
  const [batt, xml] = await Promise.all([sh("/usr/bin/pmset", ["-g", "batt"]), sh("/usr/sbin/ioreg", ["-rn", "AppleSmartBattery", "-a"])]);
  // e.g. "Now drawing from 'AC Power'\n -InternalBattery-0 (id=1)\t87%; charging; 0:42 remaining present: true"
  const pct = Number(/(\d+)%/.exec(batt)?.[1] ?? -1);
  const status = /\d+%; ([^;]+);/.exec(batt)?.[1] ?? "";
  const time = /(\d+:\d\d) remaining/.exec(batt)?.[1];
  const onPower = batt.includes("'AC Power'");
  const charging = status === "charging" || status === "finishing charge";
  const line = charging ? (time && time !== "0:00" ? `Charging, ${time} to full` : "Charging")
    : status === "charged" ? "Fully charged"
    : onPower ? "On power, not charging"   // e.g. optimized charging holding at 80 %
    : time ? `${time} left` : "On battery";

  const design = int(xml, "DesignCapacity"), nominal = int(xml, "NominalChargeCapacity"), cycles = int(xml, "CycleCount");
  const adapter = xml.indexOf("<key>AdapterDetails</key>");
  const adapterW = onPower && adapter >= 0 ? int(xml, "Watts", adapter) : undefined;
  let amps = int(xml, "InstantAmperage") ?? 0;
  if (amps > 2 ** 31) amps -= 2 ** 64;                 // reported as an unsigned 64-bit value
  const drawW = Math.abs(amps * (int(xml, "Voltage") ?? 0)) / 1e6;
  const parts = [
    design && nominal ? `Health ${Math.round((100 * nominal) / design)}%` : "",
    cycles !== undefined ? `${cycles} cycles` : "",
    adapterW ? `${adapterW}W adapter` : !onPower && drawW >= 0.5 ? `${drawW.toFixed(1)}W` : "",
  ];
  return { pct, charging, source: onPower ? "Power adapter" : "Battery", line, detail: parts.filter(Boolean).join("  ") };
}

// ---------------------------------------------------------------- sound (AppleScript)
async function sound(): Promise<MacState["sound"]> {
  const out = await osa("get volume settings");  // "output volume:50, input volume:36, alert volume:100, output muted:false"
  return { volume: Number(/output volume:(\d+)/.exec(out)?.[1] ?? 0), muted: /output muted:true/.test(out) };
}

// ---------------------------------------------------------------- Wi-Fi (networksetup, ipconfig)
let wifiPort: string | undefined;
async function wifiDevice(): Promise<string> {
  wifiPort ??= /Hardware Port: Wi-Fi\nDevice: (\S+)/.exec(await sh("/usr/sbin/networksetup", ["-listallhardwareports"]))?.[1] ?? "en0";
  return wifiPort;
}
async function wifi(): Promise<MacState["wifi"]> {
  const dev = await wifiDevice();
  const on = /: On/.test(await sh("/usr/sbin/networksetup", ["-getairportpower", dev]));
  if (!on) return { on, detail: "Off" };
  // The network name needs Location permission on recent macOS, so show the address instead.
  const ip = (await sh("/usr/sbin/ipconfig", ["getifaddr", dev]).catch(() => "")).trim();
  return { on, detail: ip ? `Connected, ${ip}` : "Not connected" };
}

// ---------------------------------------------------------------- AirDrop (sharingd preference)
const AIRDROP = { off: "Off", contacts: "Contacts Only", everyone: "Everyone" } as const;
async function airdrop(): Promise<MacState["airdrop"]> {
  const v = (await sh("/usr/bin/defaults", ["read", "com.apple.sharingd", "DiscoverableMode"]).catch(() => "Contacts Only")).trim();
  return v === "Off" ? "off" : v === "Everyone" ? "everyone" : "contacts";
}

// ---------------------------------------------------------------- dark mode
async function dark(): Promise<boolean> {
  return (await sh("/usr/bin/defaults", ["read", "-g", "AppleInterfaceStyle"]).catch(() => "")).trim() === "Dark";
}

// ---------------------------------------------------------------- lock
// macOS's own "lock now" (login.framework), the same as Control-Command-Q.
async function lock() {
  if (!existsSync("/usr/bin/python3")) throw new Error("needs /usr/bin/python3");
  await sh("/usr/bin/python3", ["-c",
    "import ctypes; ctypes.CDLL('/System/Library/PrivateFrameworks/login.framework/login').SACLockScreenImmediate()"]);
}

// ---------------------------------------------------------------- public
export async function readMac(): Promise<MacState> {
  const [b, s, w, a, d] = await Promise.all([battery(), sound(), wifi(), airdrop(), dark()]);
  return { battery: b, sound: s, wifi: w, airdrop: a, dark: d };
}

// Apply one control from the deck. Returns a short message for the deck's toast, if any.
export async function setMac(key: string, value: string): Promise<string | undefined> {
  const on = value === "on" || value === "1";
  switch (key) {
    case "volume": {
      const v = Number(value);
      if (!Number.isInteger(v) || v < 0 || v > 100) throw new Error(`bad volume ${value}`);
      await osa(`set volume output volume ${v}`, "set volume output muted false");
      return;
    }
    case "mute": await osa(`set volume output muted ${on}`); return;
    case "wifi": await sh("/usr/sbin/networksetup", ["-setairportpower", await wifiDevice(), on ? "on" : "off"]); return;
    case "airdrop": {
      const mode = AIRDROP[value as keyof typeof AIRDROP];
      if (!mode) throw new Error(`bad AirDrop mode ${value}`);
      await sh("/usr/bin/defaults", ["write", "com.apple.sharingd", "DiscoverableMode", "-string", mode]);
      await sh("/usr/bin/killall", ["sharingd"]).catch(() => {});  // it restarts and reads the new mode
      return;
    }
    case "dark":
      await osa(`tell application "System Events" to tell appearance preferences to set dark mode to ${on}`);
      return;
    case "lock": await lock(); return "Mac locked";
  }
  throw new Error(`unknown Mac control ${key}`);
}
