# MacDeck

Turn an old Android tablet into a dedicated touchscreen deck for a Mac: tool buttons, live system metrics, and (later) AI coding-agent status. The Mac does all the real work; the tablet is only a fast, always-on display and touch surface.

Instead of running an app on top of Android, MacDeck replaces Android's entire userspace with **MacDeck OS**: the tablet's own stock Linux kernel plus a tiny ramdisk that starts a single deck program. No Android framework, no Java, no launcher. It boots straight into the deck.

> **Status: early work in progress.** MacDeck OS v2.1 is the tablet's main system. Power-on, or plugging in a powered-off tablet, boots straight into the deck. It has two swipeable pages: a **Deck** page with live Mac metrics and buttons that run actions on the Mac over USB, and a **Tablet** page with battery, power and system info and the tablet's power controls.

## Target hardware

| | |
|---|---|
| Tablet | Samsung Galaxy Tab 3 7.0 Wi-Fi, **SM-T210** (`lt02wifi`), stock Android 4.4.2 |
| SoC | Marvell PXA986, 2× Cortex-A9, ~830 MiB RAM |
| Display / touch | 1024×600 framebuffer (32 bpp), `sec_touchscreen` multitouch |
| Host | Apple Silicon Mac (macOS) |

Other devices would need their own kernel, partition layout and quirks. The approach generalises, but the image builder is SM-T210-specific.

## How it works

```
 Mac  ── USB ──  SM-T210
                 ├─ Samsung stock kernel 3.4 (unchanged, from the device's own boot partition)
                 └─ MacDeck ramdisk
                      ├─ init      (Samsung's stock Android init, used only as a tiny process supervisor)
                      ├─ ueventd   (creates /dev nodes)
                      ├─ adbd      (development access + the USB link)
                      └─ deck      (our program: draws to /dev/graphics/fb0, reads /dev/input/event0,
                                    listens on 127.0.0.1:8765 for the Mac)

 Mac: host/ (Node + TypeScript, no dependencies)
      adb forward tcp:18765 → tablet:8765, then speaks protocol/PROTOCOL.md:
      sends clock + 4 metric tiles + 6 button definitions, runs allowlisted actions on PRESS
```

Measured: the kernel reaches `init` at ~4.5 s and the deck is running at ~5.1 s, only 4 userspace processes run, and ~834 MB of RAM is free (stock Android idled at ~55 MB free).

**No Samsung or third-party binaries are stored in this repository.** `scripts/build-os-image.py` extracts the kernel and the few stock ramdisk files it reuses from a dump of *your own* device's boot partition at build time.

## Power management

Measured on the SM-T210 over a 250 mA USB port, the backlight dominates power use:

| Backlight | Battery current |
|---|---|
| 255 | −426 mA |
| 160 | −190 mA |
| 80 (default) | −98 mA |
| 30 | −56 mA |
| off | **+78 mA (charging)** |

The deck therefore:
- defaults to brightness 80
- dims after 1 min idle and turns the backlight off after 5 min, or 2 min after the Mac disconnects
- stops rendering while off
- treats the first touch as wake-only; the power key toggles the screen
- samples sysfs every 2 s, or every 30 s while off
- sleeps in poll() for up to 1 s when idle

Without Android, the CPU governor was stuck on `userspace` at 1205 MHz. The deck switches it to `interactive`, or to `conservative` capped at 624 MHz in *saver* mode. All of these are adjustable on the Tablet page. The Mac host stores the settings (the tablet has no persistent storage) and re-sends them on connect.

## Repository layout

| Path | What |
|---|---|
| `os/deck/` | The deck program (C, statically linked, libc only), the Spleen font header and a Mac-side preview renderer |
| `os/ramdisk/` | `init.rc` and `default.prop` for MacDeck OS |
| `host/` | Mac host: metrics, button layout, actions (`config.example.json`; your own `config.json` is git-ignored) |
| `protocol/PROTOCOL.md` | The Mac ↔ deck message format (v1) |
| `third_party/spleen/` | Spleen font license (the font is compiled into the deck) |
| `scripts/build-os-image.py` | Builds the boot image: stock kernel + our ramdisk, stock load addresses |
| `scripts/backup-partitions.sh` | Dumps every eMMC partition over ADB from TWRP, in verified, resumable chunks |
| `scripts/bdf2c.py` | Converts BDF bitmap fonts to a C header |
| `spikes/` | Small experiments (e.g. `fbtouch`, the first framebuffer + touch test) |
| `Makefile` | Builds everything |

## Build

Requirements: Android NDK (r28 tested; found via `ANDROID_NDK_HOME` or `~/Library/Android/sdk/ndk/*`) and Python 3.

```bash
make                     # build/deck (tablet) + build/deck-preview (Mac)
make preview             # render the deck UI with sample data → build/preview.png
MACDECK_STOCK_BOOT=/path/to/KERNEL.img MACDECK_VERSION=1.4 make image   # build/macdeck-os-v1.4.img
```

`KERNEL.img` is a raw dump of the tablet's `KERNEL` partition. `scripts/backup-partitions.sh` produces one while the tablet is booted into TWRP.

## Run the Mac host

Requirements: Node 22.18+ (runs the TypeScript directly) and `adb`.

```bash
cd host
cp config.example.json config.json   # optional: your own buttons (open-app / command actions)
node src/main.ts                     # MACDECK_SERIAL=<serial> if several devices are attached
```

The host reconnects automatically. The deck greys out when the Mac goes away.

## Flashing (at your own risk)

You need [Heimdall](https://git.sr.ht/~grimler/Heimdall) (the maintained fork builds natively on Apple Silicon) and, for the backup, TWRP for `lt02`.

1. **Back up first.** Flash TWRP to `RECOVERY`, boot it, and run `scripts/backup-partitions.sh <out-dir>`. Keep `KERNEL.img`: it is both the build input and your way back to stock Android.
2. Download mode: Power + Home + Volume Down, then Volume Up. Once MacDeck OS is running: `adb shell setprop debug.macdeck.reboot download`.
3. Test from the recovery slot first: `heimdall flash --RECOVERY build/macdeck-os-v1.4.img --no-reboot`, then boot it with Power + Home + Volume Down until the screen goes black, then immediately Power + Home + Volume Up.
4. Make it the main system: `heimdall flash --KERNEL build/macdeck-os-v1.4.img --RECOVERY twrp.img`. Stock Android will no longer boot; `heimdall flash --KERNEL KERNEL.img` restores it.

Hard-won notes:
- Samsung's stock `init` **reboots into recovery** if it can't load `/sepolicy`, or if a `critical` service keeps crashing. With MacDeck OS in `RECOVERY` that means a boot loop, so the stock policy files are kept and no service is marked critical.
- The bootloader ignores the image's own kernel cmdline, so `androidboot.selinux=permissive` does nothing. `init.rc` runs `setenforce 0` instead.
- Samsung's `adbd` stays `uid=shell` even with `ro.secure=0`, so `adb reboot download` fails. MacDeck OS exposes dev switches the shell user may set:
  - `setprop debug.macdeck.reboot download|recovery|normal|off`
  - `setprop debug.macdeck.deck stop|start`, then test builds from `/data/local/tmp` without reflashing
- Loopback is down unless something runs `ifup lo`. Without it, every adb-forwarded connection closes immediately.
- The stock kernel reports touch in portrait axes (x 0–598, y 0–1022) while the framebuffer is landscape. The deck detects this and rotates.
- While MacDeck OS lives only in `RECOVERY`, a normal power-on boots stock Android, which rewrites `RECOVERY` with stock recovery (`install-recovery.sh`). Moving MacDeck OS to `KERNEL` ends this.
- Powered off and then plugged in, the bootloader starts `KERNEL` in low-power charging mode (`androidboot.mode=charger`). Samsung's init then runs only `on charger`, and the kernel **does not register the touchscreen** (event0 becomes the power key). MacDeck OS reboots out of charger mode into a normal boot, and the deck finds the touchscreen by name.
- A weak USB cable or a bus-powered hub can make the USB link drop when VBUS sags (`mv-udc: vbus is 0`). The backup script survives this with small verified chunks.

The bootloader partitions are never written, so download mode always remains available as a way back.

## Roadmap

- [x] Safety net: Heimdall on Apple Silicon, TWRP, full verified partition backup
- [x] MacDeck OS v0: boots to our own deck program, drawing + touch, no Android
- [x] Mac link: live metrics on the deck, buttons trigger Mac actions (protocol v1)
- [x] Text rendering (Spleen bitmap font) and a first deck layout
- [x] MacDeck OS as the main system (`KERNEL`), TWRP kept in `RECOVERY`; plugging in a powered-off tablet starts the deck
- [x] Swipeable pages, tablet battery/power page, brightness/dim/off/CPU controls, idle screen-off
- [ ] Custom layouts and more pages (agent status, notifications)
- [ ] AI-agent status (Claude Code, Codex, …)
- [ ] Wi-Fi transport (tablet on a wall charger)

## License

MIT. See [LICENSE](LICENSE). The Spleen font compiled into the deck is BSD-2-Clause ([third_party/spleen/LICENSE](third_party/spleen/LICENSE)). Samsung, TWRP and Heimdall are separate projects under their own licenses; none of their files are included here.
