#!/bin/sh
# Save the tablet's screen as a PNG: scripts/deck-screenshot.sh out.png
# The SM-T210 framebuffer is 1024x600, 32 bpp BGRA. Samsung's 4.4 adbd has no exec-out,
# so the frame is copied to /data/local/tmp and pulled.
set -eu
ADB=${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}
out=${1:?usage: deck-screenshot.sh out.png}
tmp=$(mktemp -d)
$ADB shell dd if=/dev/graphics/fb0 of=/data/local/tmp/fb.raw bs=4096 count=600 >/dev/null 2>&1
$ADB pull /data/local/tmp/fb.raw "$tmp/fb.raw" >/dev/null 2>&1
python3 - "$tmp/fb.raw" "$tmp/fb.ppm" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
o = bytearray(len(d) // 4 * 3)
o[0::3], o[1::3], o[2::3] = d[2::4], d[1::4], d[0::4]
open(sys.argv[2], "wb").write(b"P6\n1024 600\n255\n" + o)
PY
sips -s format png "$tmp/fb.ppm" --out "$out" >/dev/null
rm -rf "$tmp"
echo "$out"
