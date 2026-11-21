# MacDeck

Turn an old Android tablet into a dedicated touchscreen deck for a Mac: tool buttons, live system metrics, and (later) AI coding-agent status. The Mac does all the real work; the tablet is only a fast, always-on display and touch surface.

Instead of running an app on top of Android, MacDeck replaces Android's entire userspace with **MacDeck OS**: the tablet's own stock Linux kernel plus a tiny ramdisk that starts a single deck program. No Android framework, no Java, no launcher. It boots straight into the deck.

> **Status: early work in progress.** MacDeck OS v0 boots, draws and handles touch on the target tablet. The Mac connection is the next step.

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
                      ├─ adbd      (development access)
                      └─ deck0     (our program: draws to /dev/graphics/fb0, reads /dev/input/event0)
```

v0 measurements: the kernel reaches `init` at ~4.5 s, `deck0` is running at ~5.1 s, only 4 userspace processes run, and ~834 MB of RAM is free (stock Android idled at ~55 MB free).

**No Samsung or third-party binaries are stored in this repository.** `scripts/build-os-image.py` extracts the kernel and the few stock ramdisk files it reuses from a dump of *your own* device's boot partition at build time.

## Repository layout

| Path | What |
|---|---|
| `os/deck0/` | The deck program (C, statically linked, no dependencies beyond libc) |
| `os/ramdisk/` | `init.rc` and `default.prop` for MacDeck OS |
| `scripts/build-os-image.py` | Builds the boot image: stock kernel + our ramdisk, stock load addresses |
| `scripts/backup-partitions.sh` | Dumps every eMMC partition over ADB from TWRP, in verified, resumable chunks |
| `spikes/` | Small experiments (e.g. `fbtouch`, the first framebuffer + touch test) |
| `Makefile` | Builds everything |

## Build

Requirements: Android NDK (r28 tested; found via `ANDROID_NDK_HOME` or `~/Library/Android/sdk/ndk/*`) and Python 3.

```bash
make                     # build/deck0
MACDECK_STOCK_BOOT=/path/to/KERNEL.img make image   # build/macdeck-os-v0.img
```

`KERNEL.img` is a raw dump of the tablet's `KERNEL` partition. `scripts/backup-partitions.sh` produces one while the tablet is booted into TWRP.

## Flashing (at your own risk)

You need [Heimdall](https://git.sr.ht/~grimler/Heimdall) (the maintained fork builds natively on Apple Silicon) and, for the backup, TWRP for `lt02`.

1. **Back up first.** Flash TWRP to `RECOVERY`, boot it, and run `scripts/backup-partitions.sh <out-dir>`.
2. Download mode: Power + Home + Volume Down, then Volume Up.
3. `heimdall flash --RECOVERY build/macdeck-os-v0.img --no-reboot`
4. Boot it: Power + Home + Volume Down until the screen goes black, then immediately Power + Home + Volume Up.

Hard-won notes:
- Samsung's stock `init` **reboots into recovery** if it can't load `/sepolicy`, or if a `critical` service keeps crashing. With MacDeck OS in `RECOVERY` that means a boot loop, so the stock policy files are kept and no service is marked critical.
- The bootloader ignores the image's own kernel cmdline, so SELinux stays enforcing. `deck0` runs in the `init_shell` domain, which is permissive in the 4.4 policy.
- Samsung's `adbd` stays `uid=shell` even with `ro.secure=0`.
- **A normal power-on boots stock Android, which rewrites `RECOVERY` with stock recovery** (`install-recovery.sh`) and erases MacDeck OS.
- A weak USB cable or a bus-powered hub can make the USB link drop when VBUS sags (`mv-udc: vbus is 0`). The backup script survives this with small verified chunks.

The bootloader partitions are never written, so download mode always remains available as a way back.

## Roadmap

- [x] Safety net: Heimdall on Apple Silicon, TWRP, full verified partition backup
- [x] MacDeck OS v0: boots to our own deck program, drawing + touch, no Android
- [ ] Mac link: live metrics on the deck, buttons trigger Mac actions
- [ ] Text rendering and a real deck layout
- [ ] Root development channel, SELinux handling, move MacDeck OS to the main boot partition
- [ ] AI-agent status (Claude Code, Codex, …)
- [ ] Wi-Fi transport (tablet on a wall charger)

## License

MIT. See [LICENSE](LICENSE). Samsung, TWRP and Heimdall are separate projects under their own licenses; none of their files are included here.
